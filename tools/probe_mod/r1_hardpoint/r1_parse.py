#!/usr/bin/env python3
"""r1_parse.py - turns the R1 hardpoint probe records (records.log, "R1P|...|END" lines) into report.txt / report.json.

  r1_parse.py <records.log> [--out report.txt] [--json report.json]
  r1_parse.py --selftest

Per stage: Get_Hull / Get_Shield of the stage's ship before the kill and at the stage end, the breakoff-prop / death-
clone / explosion objects that appeared (sum of the count increases between polls, so a prop that expires does not
hide a new one), and a verdict:
  type-name / lower-case / collision-mesh stages: HP_KILLED (its Death_Breakoff_Prop appeared, or - for a hardpoint
    without one - Get_Hull fell by about its share of the hardpoint health), HULL_HIT (no prop, Get_Hull fell by more:
    the damage went to the hull), SHIP_DIED, NO_EFFECT;
  owner death: ALL_LIVE_BREAK_OFF (one prop of every live hardpoint that has one), PARTIAL, NONE; plus the death clone.
The screenshot part (what the driver saw) is a checklist at the end of the report: the records cannot see visuals.
"""
import argparse
import json
import os
import re
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_r1_mod as B  # noqa: E402

REC_RE = re.compile(r'R1P\|[A-Z0-9_]+\|[ -~]*?\|END')
TOTAL_HP = sum(h for _n, h, _b in B.HARDPOINTS)

# Port prediction per stage (what the port would do / draw), for the report.
PORT = {
    'FL': 'port Lua Take_Damage(n, name) (engine/lua/src/bind_objects.cpp:1794) looks for an attached child object of '
          'that type; battle hardpoints are flat CombatSystem records, so the port does NOTHING here (retail kills it '
          'if vanilla story scripts work)',
    'ENG': 'same Lua path as FL',
    'SHLD': 'same Lua path as FL',
    'TRAC': 'same Lua path as FL',
    'DEATH': 'port CombatSystem kills every live destroyable hardpoint at owner death (combat_system.cpp:3038, '
             'HP_Take_Damage kind 6): one breakoff prop per live hardpoint that has one',
    'LOWER': 'port: no effect (same Lua path); retail lookup case rule unknown',
    'ML': 'same Lua path as FL (no breakoff prop: only Get_Hull can show it)',
    'MR': 'same Lua path as FL (no breakoff prop: only Get_Hull can show it)',
    'BOGUS': 'port: Take_Damage(n, unknown name) is a no-op in battles; retail unknown (hit the hull, nothing, or the '
             'Lua script stops)',
    'COLL': 'port: no effect (same Lua path); retail: does the Lua name also match Collision_Mesh (TACTICAL_COMBAT 2.9 '
            'lookup 0x394d40 is by Collision_Mesh for weapon hits)?',
}

CHECKLIST = r'''
SCREENSHOT CHECKLIST (driver; compare the BEFORE and AFTER zooms of each stage). Port = the B.10 rule as implemented in
engine/render/src/hardpoints.cpp (decal shown, Damage_Particles subtree shown, Engine_Particles subtree hidden,
Model_To_Attach never hidden), data from r1_model_sheet.py (EV_StarDestroyer.ALO):
 1 FL   (front-left turbolaser, bone HP_F-L_Bone): port predicts
        a) scorch decal mesh HP_F-L_Blast appears at the turret            -> seen? yes / no
        b) fire / sparks (2 x p_hp_imperial_damage at HP_F-L_EmitDamage)   -> seen? yes / no
        c) the turret model EV_StarDestroyer_HP00_L-F STAYS on the hull     -> still there? yes / no   <== KEY
           (a loose copy of the same model, Hardpoint_Breakoff_Star_Dest_Weapon_FL, drifts away: seen? yes / no)
        d) explosion Large_Explosion_Space_Empire at the turret             -> seen? yes / no
 2 ENG  (engines, stern; no decal): port predicts
        a) fire / sparks at HP_E_EmitDamage (3 emitters)                    -> seen? yes / no
        b) the engine glow STAYS: its proxy pe_stardestroyerengines hangs under bone engines_big, NOT under
           HP_E_MainEngines (that bone has no children in EV_StarDestroyer.ALO), so the B.10 hide does nothing
                                                                            -> glow still on? yes / no  <== KEY
        c) EV_StarDestroyer_HP00_E (only a hidden collision mesh) - nothing visible changes there
 3 SHLD (shield generator domes on the bridge tower, HP_M-C_Bone): decal HP_M-C_Blast, fire (2 emitters), the dome
        model EV_StarDestroyer_HP00_M-C STAYS (+ a loose copy drifts away), shield bar empty  -> each yes / no
 4 TRAC (tractor beam, HP_Trac_Bone): decal HP_Trac_Blast, fire (1 emitter), nothing attached  -> each yes / no
 5 DEATH (ship A destroyed): the death clone EV_StarDestroyer_D replaces it; loose turret copies of the three live
        turrets with breakoff props (FR, BL, BR) fly off at the death (port: yes)  -> count the loose turrets
        Do the scorch decals of the four killed hardpoints show on the clone? (the clone has its own Blast meshes)
 6 LOWER / 7 COLL (ship B): did B's rear-right (6) / front-right (7) turret explode, scorch and burn?  -> yes / no
 Any change in the hull model or smoke as the hull falls (port: no _ALT meshes in EV_StarDestroyer.ALO, no
 Damaged_Smoke_Asset_Name on Generic_Star_Destroyer, so nothing)                                         -> note
'''


def parse_line(text):
    parts = text.split('|')
    if len(parts) < 3 or parts[0] != 'R1P' or parts[-1] != 'END':
        return None
    r = {'rk': parts[1]}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            r[k] = v
    for k in ('n', 'f'):
        try:
            r[k] = int(r.get(k, ''))
        except ValueError:
            r[k] = None
    return r


def load(path):
    recs = []
    seen = set()
    seen_n = set()
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            for m in REC_RE.finditer(line):
                t = m.group(0)
                if t in seen:
                    continue
                seen.add(t)
                r = parse_line(t)
                if r:
                    key = (r.get('sid'), r['n'])
                    if r['n'] is not None and key in seen_n:
                        continue          # the dup=1 copy of a record already read
                    seen_n.add(key)
                    recs.append(r)
    return recs


def ship(rec, tag):
    """(state, hull, shield) from a 'A=1:h:s:xy' field; state 'live', 'dead', 'none' or '?'."""
    v = rec.get(tag)
    if v is None:
        return '?', None, None
    if v in ('dead', 'none'):
        return v, None, None
    p = v.split(':')

    def num(x):
        try:
            return float(x)
        except ValueError:
            return None
    return 'live', num(p[1]) if len(p) > 1 else None, num(p[2]) if len(p) > 2 else None


SHORT = {k: n for n, k in B.counted()}


def counts(rec):
    """{type name: count} from a 'c=short=count;...' field (the hook writes short keys, build_r1_mod.counted)."""
    out = {}
    for item in (rec.get('c') or '').split(';'):
        if '=' in item:
            k, v = item.split('=', 1)
            try:
                out[SHORT.get(k, k)] = int(v)
            except ValueError:
                pass
    return out


def increases(seq, name):
    tot, prev = 0, None
    for r in seq:
        c = counts(r).get(name)
        if c is None:
            continue
        if prev is not None and c > prev:
            tot += c - prev
        prev = c
    return tot


def fmt(v, f='%.3f'):
    return '?' if v is None else f % v


def analyse(recs):
    """Returns (summary dict, text lines)."""
    arm = [r for r in recs if r['rk'] == 'ARM']
    sid = arm[-1]['sid'] if arm else (recs[0].get('sid') if recs else None)
    mine = sorted([r for r in recs if r.get('sid') == sid], key=lambda r: r['n'] or 0)
    others = sorted({(r.get('sid'), r['rk'], r.get('fac', '?')) for r in recs if r.get('sid') != sid and r['rk'] in ('SKIP', 'ARM')})
    out = {'sid': sid, 'armed': bool(arm), 'stages': [], 'apierr': [], 'errors': [], 'done': None, 'mode': None}
    lines = ['R1 hardpoint probe report', '=' * 26, 'records: %d total, %d in the armed script (sid %s)' % (
        len(recs), len(mine), sid)]
    ns = {r['n'] for r in mine if r['n'] is not None}
    out['missing_n'] = [i for i in range(1, max(ns) + 1) if i not in ns] if ns else []
    if out['missing_n']:
        lines.append('records lost by the heap scan (hook counter n missing between 1 and %d): %s' % (
            max(ns), ','.join(str(i) for i in out['missing_n'][:40])))
    for s, k, fac in others:
        lines.append('  other script %s: %s fac=%s' % (s, k, fac))
    if not arm:
        lines.append('NOT ARMED: no ARM record (wrong factions / mode / no human Empire? see the SKIP lines)')
    for r in mine:
        if r['rk'] == 'APIERR':
            out['apierr'].append('%s: %s' % (r.get('api'), r.get('err')))
        elif r['rk'] == 'ERR':
            out['errors'].append(r.get('err'))
        elif r['rk'] == 'DONE':
            out['done'] = r
        elif r['rk'] == 'MODE':
            out['mode'] = r
        elif r['rk'] == 'SPAWN':
            lines.append('SPAWN ship %s: how=%s valid=%s id=%s xy=%s hull=%s shield=%s (f %s)' % (
                r.get('ship'), r.get('how'), r.get('valid'), r.get('id'), r.get('xy'), r.get('hull'), r.get('shield'), r['f']))
    hbs = [r for r in mine if r['rk'] in ('HB', 'P')]
    if hbs:
        lines.append('last record at frame %s (%s)' % (hbs[-1]['f'], hbs[-1]['rk']))
    stage_recs = [r for r in mine if r['rk'] == 'STAGE']
    killed = set()
    killed_by_ship = {}
    coupling = None
    cfg_stages = B.STAGE_DEFS
    for st in stage_recs:
        n = st.get('st')
        key = st.get('key')
        cs = cfg_stages.get(key, {})
        tag = st.get('ship', 'A')
        in_st = [r for r in mine if r.get('st') == n and r['rk'] == 'P']
        before = [r for r in in_st if r.get('why') == 'before']
        kill = [r for r in mine if r['rk'] == 'KILL' and r.get('label') == key]
        call = [r for r in mine if r['rk'] == 'CALL' and r.get('label') == key]
        if call and not kill:
            # The Take_Damage call was made (CALL record) but never returned to the hook: retail stopped the script.
            out['stages'].append({'st': n, 'key': key, 'kind': 'bogus_name', 'arg': call[-1].get('arg'),
                                  'verdict': 'SCRIPT_STOPPED_AT_CALL', 'f_kill': call[-1]['f']})
            lines.append('\nStage %s %s (ship %s, bogus_name): Take_Damage(%s, "%s") was called at frame %s and did NOT '
                         'return: the Lua script stopped inside the call' % (n, key, tag, call[-1].get('amount'),
                                                                           call[-1].get('arg'), call[-1]['f']))
            lines.append('  VERDICT %s SCRIPT_STOPPED_AT_CALL' % key)
            lines.append('  port: ' + PORT.get(key, ''))
            continue
        later = [r for r in mine if (r['n'] or 0) > (before[-1]['n'] or 0)] if before else []
        if before and not kill and not call and later:
            # The kill call is not in the records (CALL / KILL lost between the poll before and the polls after), yet the
            # script kept writing: it survived the call. Hull / shield effects only show in polls of later frames.
            lp = [r for r in later if r['rk'] == 'P']
            lf = [r for r in lp if r['f'] is not None and r['f'] > (before[-1]['f'] or 0)]
            h0 = ship(before[-1], tag)[1]
            sh0 = ship(before[-1], tag)[2]
            _s, h1, sh1 = ship((lf or lp)[-1], tag) if lp else ('?', None, None)
            out['stages'].append({'st': n, 'key': key, 'kind': cs.get('kind'), 'verdict': 'SCRIPT_STOPPED_AFTER_CALL'
                                  if not lf else 'CALL_NOT_RECORDED_SCRIPT_SURVIVED', 'f_kill': before[-1]['f'],
                                  'records_after': len(later), 'later_frames': bool(lf), 'hull_before': h0, 'hull_after': h1,
                                  'shield_before': sh0, 'shield_after': sh1})
            if lf:
                lines.append('\nStage %s %s: the CALL / KILL records are missing, but %d later records in later frames exist: '
                             'the script SURVIVED the call. Get_Hull %s -> %s, Get_Shield %s -> %s' % (
                                 n, key, len(later), fmt(h0), fmt(h1), fmt(sh0), fmt(sh1)))
            else:
                lines.append('\nStage %s %s: SCRIPT_STOPPED_AFTER_CALL. The kill call RETURNED (record n=%s was written after '
                             'it, same frame %s) and nothing was written in any later frame: retail ended the script at the '
                             'end of that update. The records written around the call (CALL / KILL / SAY) are missing '
                             '(the script state is freed when the script ends). Get_Hull %s -> %s, Get_Shield %s -> %s in '
                             'that frame: no effect can show' % (n, key, max(r['n'] for r in later), before[-1]['f'],
                                                                fmt(h0), fmt(h1), fmt(sh0), fmt(sh1)))
            continue
        if before and not kill and not call:
            # Old records (no CALL record): the stage's 'before' poll is the last thing the script wrote, i.e. the script
            # stopped at the kill call itself (the 2026-11-08 COLL case).
            out['stages'].append({'st': n, 'key': key, 'kind': cs.get('kind'), 'verdict': 'NO_MATCH_SCRIPT_STOPPED',
                                  'f_kill': before[-1]['f']})
            lines.append('\nStage %s %s: the kill call was reached (frame %s) and the script wrote nothing after it '
                         '(NO_MATCH_SCRIPT_STOPPED, inferred: no CALL record in these records)' % (n, key, before[-1]['f']))
            continue
        if not before or not kill:
            # Never ran (no kill call reached: the script stopped earlier, or the battle was left): NOT_RUN, never a
            # "no match" answer.
            last_f = max([r['f'] for r in mine if r['f'] is not None] or [None])
            reason = 'no kill call reached: the last record of the script is at frame %s, the kill was due at frame %s' % (
                last_f, st.get('t_kill'))
            out['stages'].append({'st': n, 'key': key, 'verdict': 'NOT_RUN', 'reason': reason})
            lines.append('\nStage %s %s: NOT RUN (%s)' % (n, key, reason))
            continue
        b, k = before[-1], kill[-1]
        after = [r for r in in_st if r['n'] > k['n'] and r.get('why') != 'pre']
        seq = [b, k] + after
        _s0, h0, sh0 = ship(b, tag)
        end = after[-1] if after else k
        s1, h1, sh1 = ship(end, tag)
        fast = [r for r in after if r.get('why') == 'fast']
        hf = ship(fast[-1], tag)[1] if fast else None
        names = sorted({c for r in seq for c in counts(r)})
        inc = {name: increases(seq, name) for name in names}
        hp = cs.get('hp')
        boff = B.HARDPOINTS[hp][2] if hp is not None else None
        share = B.HARDPOINTS[hp][1] / TOTAL_HP if hp is not None else None
        drop = (h0 - h1) if (h0 is not None and h1 is not None) else None
        res = {'st': n, 'key': key, 'ship': tag, 'kind': cs.get('kind'), 'arg': k.get('arg'), 'amount': k.get('amount'),
               'res': k.get('res'), 'hull_before': h0, 'hull_fast': hf, 'hull_end': h1, 'shield_before': sh0,
               'shield_end': sh1, 'ship_end': s1, 'increases': inc, 'f_kill': k['f']}
        if cs.get('kind') == 'owner_death':
            live = [x for i, x in enumerate(B.HARDPOINTS) if x[2] and i not in killed]
            got = {x[2]: inc.get(x[2], 0) for x in live}
            dead_again = {B.HARDPOINTS[i][2]: inc.get(B.HARDPOINTS[i][2], 0) for i in killed if B.HARDPOINTS[i][2]}
            n_ok = sum(1 for v in got.values() if v >= 1)
            v = 'ALL_LIVE_BREAK_OFF' if live and n_ok == len(live) else ('NONE' if n_ok == 0 else 'PARTIAL')
            if any(dead_again.values()):
                v += '+DEAD_ONES_AGAIN'
            res.update(verdict=v, expected=got, dead_again=dead_again,
                       death_clone=inc.get(B.CFG['death_clone'], 0))
        elif cs.get('kind') == 'bogus_name':
            # A name that exists nowhere. Evidence that the damage still reached the ship: Get_Hull fell, or Get_Shield
            # fell at the first fast poll (Get_Hull = min(hull, hardpoint fraction) hides a hull hit while the hull is
            # above the fraction, so the shield is the second witness).
            hp_before = 1.0 - sum(B.HARDPOINTS[i][1] for i in killed_by_ship.get(tag, ())) / TOTAL_HP
            sh_fast = ship(fast[0], tag)[2] if fast else None
            sh_drop = (sh0 - sh_fast) if (sh0 is not None and sh_fast is not None) else None
            masked = h0 is not None and h0 <= hp_before + 0.005
            if s1 == 'dead':
                v = 'SHIP_DIED'
            elif (drop is not None and drop > 0.02) or (sh_drop is not None and sh_drop > 0.05):
                v = 'NO_MATCH_HIT_SHIP'
            elif masked and (sh0 is None or sh0 <= 0.05):
                v = 'NO_MATCH_UNSEEN_BY_LUA'
            else:
                v = 'NO_MATCH_NO_EFFECT'
            res.update(verdict=v, drop=drop, shield_drop=sh_drop, masked=masked)
        else:
            prop = boff is not None and inc.get(boff, 0) >= 1
            # Get_Hull = min(hull, hardpoint fraction) only with the coupling (type +0xa8); a hardpoint without a breakoff
            # prop is seen only through that, and not while the hull is below the hardpoint fraction (masked).
            hp_before = 1.0 - sum(B.HARDPOINTS[i][1] for i in killed_by_ship.get(tag, ())) / TOTAL_HP
            masked = h0 is not None and h0 < hp_before - 0.02
            in_window = drop is not None and share is not None and 0.5 * share <= drop <= 1.6 * share
            if s1 == 'dead':
                v = 'SHIP_DIED'
            elif prop:
                v = 'HP_KILLED'
            elif boff is None and in_window and coupling is not False:
                v = 'HP_KILLED'
            elif drop is not None and drop > (max(0.02, 1.6 * (share or 0)) if boff is None else 0.02):
                v = 'HULL_HIT'
            elif boff is None and (coupling is False or masked):
                v = 'UNSEEN_BY_LUA'
            else:
                v = 'NO_EFFECT'
            if v == 'HP_KILLED':
                killed_by_ship.setdefault(tag, set()).add(hp)
                if tag == 'A':
                    killed.add(hp)
            if key == 'FL' and prop and drop is not None:
                coupling = in_window   # Get_Hull fell by the hardpoint's share: hull / hardpoint coupling is on
            res.update(verdict=v, breakoff=boff, prop=inc.get(boff, 0) if boff else None, share=share, drop=drop,
                       masked=masked, coupling=coupling)
        out['stages'].append(res)
        lines.append('\nStage %s %s (ship %s, %s): Take_Damage(%s%s) -> %s' % (
            n, key, tag, cs.get('kind'), k.get('amount'), (', "%s"' % k.get('arg')) if k.get('arg') else '', k.get('res')))
        lines.append('  Get_Hull %s -> %s (+90 f) -> %s (stage end); Get_Shield %s -> %s; ship at end: %s' % (
            fmt(h0), fmt(hf), fmt(h1), fmt(sh0), fmt(sh1), s1))
        lines.append('  objects that appeared: ' + (', '.join('%s +%d' % (a, c) for a, c in sorted(inc.items()) if c) or
                                                   'none'))
        if cs.get('kind') == 'owner_death':
            lines.append('  live hardpoints with a breakoff prop at the death: %s' % (
                ', '.join('%s +%d' % (a, c) for a, c in res['expected'].items()) or 'none'))
            lines.append('  already-dead ones (expect +0): %s; death clone +%d' % (
                ', '.join('%s +%d' % (a, c) for a, c in res['dead_again'].items()) or 'none', res['death_clone']))
        elif cs.get('kind') == 'bogus_name':
            lines.append('  expects nothing (the name does not exist); Get_Hull drop %s, Get_Shield drop %s%s' % (
                fmt(res.get('drop')), fmt(res.get('shield_drop')), ' (hull masked by the hardpoint fraction)' if
                res.get('masked') else ''))
        else:
            lines.append('  expects %s (index %s, share %.3f of the hardpoint health, breakoff %s)' % (
                B.HARDPOINTS[hp][0], hp, share, boff or '-'))
        lines.append('  VERDICT %s %s' % (key, res['verdict']))
        lines.append('  port: ' + PORT.get(key, ''))
    if out['mode']:
        m = out['mode']
        lines.append('\nName mode after the FL kill: %s (prop %s, Get_Hull drop %s)' % (m.get('mode'), m.get('prop'), m.get('drop')))
    lines.append('\nANSWERS')
    by = {s['key']: s for s in out['stages']}

    def ans(label, key, mapping):
        s = by.get(key)
        v = s['verdict'] if s else 'NOT_RUN'
        v = mapping.get(v, v)
        lines.append('  %-22s %s' % (label, v))
        return v
    out['answers'] = {
        'lua_type_name': ans('lua_type_name', 'FL', {'HP_KILLED': 'KILLS_THE_HARDPOINT', 'HULL_HIT': 'HITS_THE_HULL'}),
        'lua_lower_case': ans('lua_lower_case', 'LOWER', {'HP_KILLED': 'CASE_INSENSITIVE', 'HULL_HIT': 'NO_MATCH_HULL_HIT'}),
        'lua_collision_mesh': ans('lua_collision_mesh', 'COLL', {'HP_KILLED': 'COLLISION_MESH_MATCHES',
                                                                  'HULL_HIT': 'NO_MATCH_HULL_HIT'}),
        'owner_death': ans('owner_death', 'DEATH', {}),
    }
    out['answers']['lua_bogus_name'] = ans('lua_bogus_name', 'BOGUS', {})
    for key in ('ML', 'MR', 'ENG', 'SHLD', 'TRAC'):
        out['answers']['kill_' + key.lower()] = ans('kill_' + key.lower(), key, {})
    if out['apierr']:
        lines.append('\nAPI errors: ' + '; '.join(out['apierr']))
    if out['errors']:
        lines.append('Lua errors: ' + '; '.join(e or '?' for e in out['errors']))
    lines.append('DONE record: %s' % ('yes (%s)' % out['done'].get('records') if out['done'] else
                                      'NO (the script stopped or the battle was left early)'))
    lines.append(CHECKLIST)
    return out, lines


def report(path, out_txt=None, out_json=None):
    recs = load(path)
    summary, lines = analyse(recs)
    text = '\n'.join(lines)
    if out_txt:
        with open(out_txt, 'w') as f:
            f.write(text + '\n')
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(summary, f, indent=1, default=str)
    return text, recs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('records', nargs='?')
    ap.add_argument('--out')
    ap.add_argument('--json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        import selftest_r1
        return selftest_r1.parse_selftest()
    if not a.records:
        ap.error('records.log needed')
    text, recs = report(a.records, a.out, a.json)
    print(text)
    return 0 if recs else 10


if __name__ == '__main__':
    sys.exit(main())
