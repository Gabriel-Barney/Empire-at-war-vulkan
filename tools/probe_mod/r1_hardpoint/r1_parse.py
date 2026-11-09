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
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            for m in REC_RE.finditer(line):
                t = m.group(0)
                if t in seen:
                    continue
                seen.add(t)
                r = parse_line(t)
                if r:
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
    cfg_stages = {s['key']: s for s in B.STAGES}
    for st in stage_recs:
        n = st.get('st')
        key = st.get('key')
        cs = cfg_stages.get(key, {})
        tag = st.get('ship', 'A')
        in_st = [r for r in mine if r.get('st') == n and r['rk'] == 'P']
        before = [r for r in in_st if r.get('why') == 'before']
        kill = [r for r in mine if r['rk'] == 'KILL' and r.get('label') == key]
        if not before or not kill:
            out['stages'].append({'st': n, 'key': key, 'verdict': 'NOT_RUN'})
            lines.append('\nStage %s %s: NOT RUN (no kill record; did the script stop? last frame above)' % (n, key))
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
        lines.append('  %-22s %s' % (label, mapping.get(v, v)))
        return v
    out['answers'] = {
        'lua_type_name': ans('lua_type_name', 'FL', {'HP_KILLED': 'KILLS_THE_HARDPOINT', 'HULL_HIT': 'HITS_THE_HULL'}),
        'lua_lower_case': ans('lua_lower_case', 'LOWER', {'HP_KILLED': 'CASE_INSENSITIVE', 'HULL_HIT': 'NO_MATCH_HULL_HIT'}),
        'lua_collision_mesh': ans('lua_collision_mesh', 'COLL', {'HP_KILLED': 'COLLISION_MESH_MATCHES',
                                                                  'HULL_HIT': 'NO_MATCH_HULL_HIT'}),
        'owner_death': ans('owner_death', 'DEATH', {}),
    }
    for key in ('ENG', 'SHLD', 'TRAC'):
        ans('kill_' + key.lower(), key, {})
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
