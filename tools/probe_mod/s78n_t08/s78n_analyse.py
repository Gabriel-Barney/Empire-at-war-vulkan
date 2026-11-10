#!/usr/bin/env python3
"""s78n_analyse.py - turns the s78n t08 probe records into a verdict per case, with the numbers.

  s78n_analyse.py RECORDS.log [--config config.json] [--out report.txt] [--json report.json]

(A) tractor beam (docs/re/TACTICAL_COMBAT.md sec. 5.4.2): the port lets the beam start when the XY centre distance
    d <= R + E, E = the target's X half-extent when its facing is along the line of sight (|f.u| > 0.7071, the
    "toward" trials), else its Y half-extent ("side" trials). Per trial the report gives the centre distance at the
    order (d0), how the beam started (IN_PLACE: within 15 frames of the order, the user still; CLOSED: after the user
    walked or the target was stepped closer, with the interval [d at the first poll with the beam, d one poll
    earlier] that holds the retail threshold T; NONE: never, T < the smallest d seen) and whether that agrees with the
    port's T = R + E (tolerance 1.0). Then the threshold intervals per facing from all trials together.
      A0 setup     both controls (d = R - 50) started in place;
      A1 footprint the beam started at a centre distance beyond R (so a target term E > 0 is added) and never beyond
                   R + max(E_x, E_y) + 1;
      A2 per trial each trial agrees with the port's R + E for its facing;
      A3 facing    the fitted toward / side thresholds hold R + E_x / R + E_y (not swapped).
    Verdict (A): PASS when A0-A2 pass (A3 PASS or INCONCLUSIVE), FAIL when A1 or A2 fails, INCONCLUSIVE when A0 fails.
(B) Combat_Bonus aura on an allied player (sec. 5.4.3 item 4): Health_Bonus b of Emperor_Land_Combat_Bonus. After a
    Lua Take_Damage(D) on a fresh unit of health H the hull falls by D / H without the bonus and by D / (H (1 + b))
    with it (0.30 / 0.24 for an AT-ST, D = 0.3 H, b = 0.25); a unit damaged before the Emperor arrives goes from
    1 - D/H (0.70) to (H - D + b H) / (H (1 + b)) (0.76) when the aura reaches it then (port: current += new max - old),
    or to (H - D) / (H (1 + b)) (0.56) if only the maximum rose. Tolerance 0.015.
      B0 setup     Empire and Underworld allied (menu, or Make_Ally by the probe: noted), the Emperor spawned, the own
                   near unit has the bonus and the enemy near unit has not;
      B1 ally      the allied player's near unit has the bonus (the question);
      B2 radius    the far units (1200+ from the Emperor): own / ally with the bonus = no radius (the port has none);
      B3 arrival   units damaged before the Emperor spawned: own / ally get the bonus at his arrival, with the current
                   health raised by the same amount (the port's aura_on + aura_give).
    Verdict (B): PASS when B0 and B1 pass and B2 / B3 agree with the port; FAIL when B1 fails (allies not reached) or
    B2 / B3 differ; INCONCLUSIVE when B0 fails.
"""
import argparse
import json
import os
import sys

TOL_D = 1.0
TOL_H = 0.015


def parse_line(line):
    line = line.strip()
    if not line.startswith('S8N|') or not line.endswith('|END'):
        return None
    parts = line[4:-4].split('|')
    rec = {'kind': parts[0]}
    for p in parts[1:]:
        if '=' in p:
            k, v = p.split('=', 1)
            rec[k] = v
    try:
        rec['n'] = int(rec.get('n', '0'))
    except ValueError:
        rec['n'] = 0
    try:
        rec['f'] = int(rec.get('f', ''))
    except ValueError:
        rec['f'] = None
    return rec


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load(path):
    recs = []
    seen = set()
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            r = parse_line(line)
            if r and (r.get('sid'), r['n']) not in seen:
                seen.add((r.get('sid'), r['n']))
                recs.append(r)
    by_sid = {}
    for r in recs:
        by_sid.setdefault(r.get('sid', '?'), []).append(r)
    for s in by_sid.values():
        s.sort(key=lambda r: r['n'])
    return recs, by_sid


def fmt(v, p=2):
    return ('%.' + str(p) + 'f') % v if isinstance(v, (int, float)) else '?'


# ---- (A) ------------------------------------------------------------------------------------------------------------

def analyse_tb(rs, cfg):
    out = []
    res = {'trials': [], 'verdicts': {}}
    tcfg = next((r for r in rs if r['kind'] == 'TCFG'), {})
    tbc = (cfg or {}).get('tb', {})
    R = fnum(tcfg.get('R')) or tbc.get('R')
    ex = fnum(tcfg.get('ex')) or tbc.get('ex')
    ey = fnum(tcfg.get('ey')) or tbc.get('ey')
    inplace = tbc.get('inplace', 15)
    if R is None or ex is None or ey is None:
        out.append('  no TCFG record and no config: R / E unknown')
        return out, res
    out.append('  R %.2f, E_x %.2f, E_y %.2f -> port threshold toward R + E_x = %.2f, side R + E_y = %.2f' % (
        R, ex, ey, R + ex, R + ey))
    trials = {}
    for r in rs:
        k = r.get('k')
        if k is None:
            continue
        t = trials.setdefault(k, {'k': int(k), 'polls': []})
        kind = r['kind']
        if kind == 'TTRIAL':
            t.update(name=r.get('name'), face=r.get('face'), d_plan=fnum(r.get('d')), ttrial=r)
        elif kind == 'PRE':
            t['d0'] = fnum(r.get('d'))
            t['pre'] = r
        elif kind == 'ACT' and r.get('why') == 'order':
            t['ret'] = r.get('ret')
        elif kind == 'P':
            t['polls'].append(r)
        elif kind == 'ENG':
            t['eng'] = r
        elif kind == 'NOENG':
            t['noeng'] = r
        elif kind == 'STEP':
            t['step'] = r
        elif kind == 'TURN':
            t['turn'] = r.get('call')
        elif kind == 'TELE':
            t['tele'] = r.get('call')
        elif kind == 'TEND':
            t['tend'] = r
    bounds = {'toward': [None, None], 'side': [None, None]}   # [lo, hi]: lo <= T < hi
    all_ok = True
    any_trial = False
    ctrl = {}
    beyond_R = []
    beyond_max = []
    for k in sorted(trials, key=lambda x: int(x)):
        t = trials[k]
        if t.get('name') is None:
            continue
        any_trial = True
        face = t.get('face')
        Tp = R + (ex if face == 'toward' else ey)
        d0 = t.get('d0')
        eng = t.get('eng')
        polls = t['polls']
        dmin = min((fnum(p.get('d')) for p in polls if fnum(p.get('d')) is not None), default=None)
        info = {'k': t['k'], 'name': t['name'], 'face': face, 'd_plan': t.get('d_plan'), 'd0': d0, 'T_port': Tp,
                'ret': t.get('ret'), 'turn': t.get('turn'), 'tele': t.get('tele')}
        if eng is not None:
            de = fnum(eng.get('d'))
            dp = fnum(eng.get('prev_d'))
            r_eng = fnum(eng.get('r'))
            um = fnum(eng.get('um'))
            stepping = eng.get('stepping') == 'true'
            still = um is not None and um <= 3.0
            if r_eng is not None and r_eng <= inplace and still and not stepping and (
                    d0 is None or de is None or de >= d0 - 3.0):
                how = 'IN_PLACE'
                lo, hi = de, None
                ok = de is not None and Tp >= de - TOL_D
            else:
                how = 'CLOSED'
                # one poll of latency allowed: the beam may start one service after the crossing, so the upper
                # bound is d two polls before the first poll with the beam (d one poll before when there is none)
                i_eng = next((i for i, p in enumerate(polls) if p.get('act') == '1' or p.get('eff') == '1'), None)
                if i_eng is not None and i_eng >= 2 and fnum(polls[i_eng - 2].get('d')) is not None:
                    dp = max(dp if dp is not None else 0.0, fnum(polls[i_eng - 2].get('d')))
                lo, hi = de, dp
                ok = de is not None and dp is not None and de - TOL_D <= Tp <= dp + TOL_D
            info.update(how=how, d_eng=de, d_prev=dp, r_eng=r_eng, user_moved=um, stepping=stepping)
            if de is not None and de > R + TOL_D:
                beyond_R.append('%s d %.2f' % (t['name'], de))
            if de is not None and de > R + max(ex, ey) + TOL_D:
                beyond_max.append('%s d %.2f' % (t['name'], de))
        else:
            how = 'NONE'
            lo, hi = None, dmin
            ok = dmin is None or Tp < dmin + TOL_D
            info.update(how=how, d_min=dmin, stepped=t.get('step') is not None)
        b = bounds.get(face)
        if b is not None:
            if lo is not None:
                b[0] = lo if b[0] is None else max(b[0], lo)
            if hi is not None:
                b[1] = hi if b[1] is None else min(b[1], hi)
        info['port_agrees'] = ok
        all_ok = all_ok and ok
        if t['name'].startswith('ctrl'):
            ctrl[t['name']] = how
        res['trials'].append(info)
        if how == 'IN_PLACE':
            what = 'started in place at d %s (r %s frames)' % (fmt(info['d_eng']), fmt(info['r_eng'], 0))
        elif how == 'CLOSED':
            what = 'started after closing in: T in [%s, %s] (r %s, user moved %s, %s)' % (
                fmt(info['d_eng']), fmt(info['d_prev']), fmt(info['r_eng'], 0), fmt(info['user_moved']),
                'target stepped' if info['stepping'] else 'user walked')
        else:
            what = 'never started; smallest d %s (%s)' % (fmt(dmin), 'target stepped' if info['stepped'] else 'no step')
        out.append('  trial %d %-13s %-6s d0 %8s (plan %8s) order ret %-5s: %s -> port R+E %.2f: %s' % (
            t['k'], t['name'], face, fmt(d0), fmt(t.get('d_plan')), t.get('ret'), what, Tp,
            'PASS' if ok else 'FAIL'))
        if face == 'side':
            out.append('      side setup: Turn_To_Face %s, Teleport %s' % (t.get('turn'), t.get('tele')))
    if not any_trial:
        out.append('  no trial records')
        return out, res
    v = res['verdicts']
    a0 = ctrl.get('ctrl_toward') == 'IN_PLACE' and ctrl.get('ctrl_side') == 'IN_PLACE'
    v['A0'] = 'PASS' if a0 else 'INCONCLUSIVE'
    out.append('  A0 setup (controls at R - 50 start in place): %s (ctrl_toward %s, ctrl_side %s)' % (
        v['A0'], ctrl.get('ctrl_toward'), ctrl.get('ctrl_side')))
    lo_t, hi_t = bounds['toward']
    lo_s, hi_s = bounds['side']
    hi_all = max([x for x in (lo_t, lo_s) if x is not None], default=None)
    if beyond_max:
        v['A1'] = 'FAIL'
        a1why = 'started beyond R + max E: ' + ', '.join(beyond_max)
    elif beyond_R:
        v['A1'] = 'PASS'
        a1why = 'started beyond R: ' + ', '.join(beyond_R)
    elif (hi_t is not None and hi_t < R + ex - TOL_D) or (hi_s is not None and hi_s < R + ey - TOL_D):
        v['A1'] = 'FAIL'
        a1why = 'never started beyond R + 1; threshold under R + E (centre distance only?)'
    else:
        v['A1'] = 'INCONCLUSIVE'
        a1why = 'no start beyond R and no threshold at or under R'
    out.append('  A1 footprint (beam may start at R < d <= R + E): %s (%s; largest start distance %s)' % (
        v['A1'], a1why, fmt(hi_all)))
    v['A2'] = 'PASS' if all_ok else 'FAIL'
    out.append('  A2 every trial agrees with the port\'s R + E: %s' % v['A2'])

    def iv(lo, hi):
        return '[%s, %s)' % (fmt(lo) if lo is not None else '-inf', fmt(hi) if hi is not None else '+inf')

    in_t = (lo_t is None or lo_t <= R + ex + TOL_D) and (hi_t is None or R + ex < hi_t + TOL_D)
    in_s = (lo_s is None or lo_s <= R + ey + TOL_D) and (hi_s is None or R + ey < hi_s + TOL_D)
    sw_t = (lo_t is None or lo_t <= R + ey + TOL_D) and (hi_t is None or R + ey < hi_t + TOL_D)
    sw_s = (lo_s is None or lo_s <= R + ex + TOL_D) and (hi_s is None or R + ex < hi_s + TOL_D)
    if abs(ex - ey) < 2 * TOL_D:
        v['A3'] = 'INCONCLUSIVE'
        a3why = 'E_x and E_y too close to tell'
    elif in_t and in_s and not (sw_t and sw_s):
        v['A3'] = 'PASS'
        a3why = 'toward fits R + E_x, side fits R + E_y'
    elif sw_t and sw_s and not (in_t and in_s):
        v['A3'] = 'FAIL'
        a3why = 'swapped: toward fits R + E_y, side fits R + E_x'
    elif in_t and in_s:
        v['A3'] = 'INCONCLUSIVE'
        a3why = 'intervals too wide to tell the facing rule'
    else:
        v['A3'] = 'FAIL'
        a3why = 'neither assignment fits'
    out.append('  A3 facing: T toward in %s (port %.2f), T side in %s (port %.2f): %s (%s)' % (
        iv(lo_t, hi_t), R + ex, iv(lo_s, hi_s), R + ey, v['A3'], a3why))
    res['bounds'] = {'toward': [lo_t, hi_t], 'side': [lo_s, hi_s]}
    if v['A0'] != 'PASS':
        overall = 'INCONCLUSIVE'
    elif v['A1'] == 'FAIL' or v['A2'] == 'FAIL':
        overall = 'FAIL'
    elif v['A1'] == 'PASS' and v['A2'] == 'PASS' and v['A3'] in ('PASS', 'INCONCLUSIVE'):
        overall = 'PASS'
    else:
        overall = 'FAIL' if v['A3'] == 'FAIL' else 'INCONCLUSIVE'
    v['A'] = overall
    return out, res


# ---- (B) ------------------------------------------------------------------------------------------------------------

def classify_drop(dv, nobonus, bonus):
    if dv is None:
        return '?'
    if abs(dv - bonus) <= TOL_H:
        return 'BONUS'
    if abs(dv - nobonus) <= TOL_H:
        return 'NONE'
    return 'OTHER'


def analyse_cb(rs, cfg):
    out = []
    res = {'units': {}, 'verdicts': {}}
    cc = next((r for r in rs if r['kind'] == 'CCFG'), {})
    cbc = (cfg or {}).get('cb', {})
    H = fnum(cc.get('health')) or cbc.get('health')
    D = fnum(cc.get('dmg')) or cbc.get('dmg')
    b = fnum(cc.get('bonus')) if cc.get('bonus') is not None else cbc.get('bonus')
    if H is None or D is None or b is None:
        out.append('  no CCFG record and no config: health / damage / bonus unknown')
        return out, res
    drop_no = D / H
    drop_b = D / (H * (1 + b))
    pre_no = 1 - D / H
    pre_add = (H - D + b * H) / (H * (1 + b))
    pre_max = (H - D) / (H * (1 + b))
    out.append('  unit health %.0f, Take_Damage %.1f, Health_Bonus %.3f: drop %.4f without / %.4f with the bonus; '
               'pre-damaged %.4f -> %.4f (port: current raised) or %.4f (max only)' % (H, D, b, drop_no, drop_b, pre_no,
                                                                                     pre_add, pre_max))
    ally = next((r for r in rs if r['kind'] == 'ALLY'), {})
    ally2 = next((r for r in rs if r['kind'] == 'ALLY2'), None)
    fix = next((r for r in rs if r['kind'] == 'ALLYFIX'), None)
    hero = next((r for r in rs if r['kind'] == 'HERO'), {})
    inv = next((r for r in rs if r['kind'] == 'INVULN_BLOCKS'), None)
    out.append('  relations at the start: Empire->Underworld %s, Underworld->Empire %s, Empire->Rebel %s, '
               'Underworld->Rebel %s (1 = allied)%s' % (
                   ally.get('own_ally'), ally.get('ally_own'), ally.get('own_enemy'), ally.get('ally_enemy'),
                   '' if not fix else '; Make_Ally called (%s / %s), then Empire->Underworld %s' % (
                       fix.get('own_make'), fix.get('ally_make'), (ally2 or {}).get('own_ally'))))
    out.append('  hero: %s %s (owner %s, at %s)%s' % (hero.get('type'), hero.get('how'), hero.get('owner'),
                                                     hero.get('xy'), '' if not inv else
                                                     '; invulnerability blocked the Lua damage: units made vulnerable'))
    units = {}
    hero_n = hero.get('n', 10 ** 9)
    for r in rs:
        key = r.get('key')
        if not key:
            continue
        u = units.setdefault(key, {'key': key})
        kind = r['kind']
        h = fnum(r.get('hull'))
        if kind == 'UNIT':
            u.update(owner=r.get('owner'), type=r.get('type'), xy=r.get('xy'), how=r.get('how'))
        elif kind == 'H' and key.startswith('pre_'):
            if r['n'] < hero_n:
                u['pre_before'] = h
            elif 'pre_after' not in u:
                u['pre_after'] = h
                u['pre_after_dhero'] = fnum(r.get('dhero'))
        elif kind == 'H0':
            u['h0'] = h
            u['dhero'] = fnum(r.get('dhero'))
        elif kind == 'H1':
            u['h1'] = h
        elif kind == 'HEND':
            u['hend'] = h
        elif kind == 'HULL':
            u.setdefault('edges', []).append((r.get('f'), r.get('from'), r.get('to')))
    off_f = next((r['f'] for r in rs if r['kind'] == 'HEROOFF'), None)
    for key in sorted(units):
        u = units[key]
        if key.startswith('pre_'):
            pb, pa = u.get('pre_before'), u.get('pre_after')
            if pa is None:
                cls = '?'
            elif abs(pa - pre_add) <= TOL_H:
                cls = 'BONUS'
            elif abs(pa - pre_max) <= TOL_H:
                cls = 'BONUS_MAX_ONLY'
            elif abs(pa - pre_no) <= TOL_H:
                cls = 'NONE'
            else:
                cls = 'OTHER'
            u['cls'] = cls
            out.append('  %-10s owner %-10s hull before the Emperor %s, after %s (%s from him) -> %s' % (
                key, u.get('owner'), fmt(pb, 4), fmt(pa, 4), fmt(u.get('pre_after_dhero'), 0), cls))
        else:
            h0, h1 = u.get('h0'), u.get('h1')
            dv = (h0 - h1) if h0 is not None and h1 is not None else None
            u['drop'] = dv
            u['cls'] = classify_drop(dv, drop_no, drop_b)
            out.append('  %-10s owner %-10s %s from the Emperor: hull %s -> %s, drop %s -> %s' % (
                key, u.get('owner'), fmt(u.get('dhero'), 0), fmt(h0, 4), fmt(h1, 4), fmt(dv, 4), u['cls']))
        if off_f is not None:
            late = [e for e in u.get('edges', []) if e[0] is not None and e[0] >= off_f]
            if late:
                u['after_hero_off'] = late
                out.append('      after the Emperor left: ' + ', '.join('f %s %s -> %s' % e for e in late[:4]))
    res['units'] = units
    v = res['verdicts']

    def c(key):
        return units.get(key, {}).get('cls', '?')

    allied = ally.get('own_ally') == '1' or (ally2 or {}).get('own_ally') == '1'
    b0 = allied and hero.get('type', '?') != '?' and c('near_own') == 'BONUS' and c('near_enemy') == 'NONE'
    v['B0'] = 'PASS' if b0 else 'INCONCLUSIVE'
    out.append('  B0 setup: allied %s%s, hero %s, own near %s, enemy near %s: %s' % (
        allied, ' (by Make_Ally, not the menu)' if allied and ally.get('own_ally') != '1' else '', hero.get('type'),
        c('near_own'), c('near_enemy'), v['B0']))
    if c('near_ally') == 'BONUS':
        v['B1'] = 'PASS'
    elif c('near_ally') == 'NONE':
        v['B1'] = 'FAIL'
    else:
        v['B1'] = 'INCONCLUSIVE'
    out.append('  B1 the allied player\'s near unit gets the aura (port: yes): %s (%s)' % (v['B1'], c('near_ally')))
    far = (c('far_own'), c('far_ally'), c('far_enemy'))
    if far[0] == 'BONUS' and far[1] == 'BONUS' and far[2] == 'NONE':
        v['B2'] = 'PASS'
        b2why = 'no radius, as the port'
    elif far[0] == 'NONE' and far[1] == 'NONE':
        v['B2'] = 'FAIL'
        b2why = 'the far units get nothing: retail has a radius, the port has none'
    else:
        v['B2'] = 'INCONCLUSIVE'
        b2why = 'mixed'
    out.append('  B2 radius: far own %s, far ally %s, far enemy %s: %s (%s)' % (far + (v['B2'], b2why)))
    pre = (c('pre_own'), c('pre_ally'), c('pre_enemy'))
    if pre[0] == 'BONUS' and pre[1] == 'BONUS' and pre[2] == 'NONE':
        v['B3'] = 'PASS'
        b3why = 'applied at the source\'s arrival with the current health raised, as the port'
    elif 'BONUS_MAX_ONLY' in pre[:2]:
        v['B3'] = 'FAIL'
        b3why = 'applied at arrival but only the maximum rose (port raises the current too)'
    elif pre[0] == 'NONE' and pre[1] == 'NONE':
        v['B3'] = 'FAIL'
        b3why = 'not applied to units already there (port applies at the source\'s arrival)'
    else:
        v['B3'] = 'INCONCLUSIVE'
        b3why = 'mixed'
    out.append('  B3 arrival: pre own %s, pre ally %s, pre enemy %s: %s (%s)' % (pre + (v['B3'], b3why)))
    if v['B0'] != 'PASS':
        overall = 'INCONCLUSIVE'
    elif v['B1'] == 'FAIL' or v['B2'] == 'FAIL' or v['B3'] == 'FAIL':
        overall = 'FAIL'
    elif v['B1'] == 'PASS' and v['B2'] in ('PASS', 'INCONCLUSIVE') and v['B3'] in ('PASS', 'INCONCLUSIVE'):
        overall = 'PASS'
    else:
        overall = 'INCONCLUSIVE'
    v['B'] = overall
    return out, res


# ---- report ---------------------------------------------------------------------------------------------------------

def report(records_log, out_txt=None, out_json=None, cfg_path=None):
    cfg = None
    if cfg_path and os.path.exists(cfg_path):
        with open(cfg_path) as f:
            cfg = json.load(f)
    recs, by_sid = load(records_log)
    lines = ['s78n t08 probe report: %s (%d records, %d scripts)' % (records_log, len(recs), len(by_sid))]
    res = {'records': len(recs), 'scripts': [], 'tb': None, 'cb': None, 'verdicts': []}
    final = {}
    for sid, rs in sorted(by_sid.items(), key=lambda kv: kv[1][0]['n'] if kv[1] else 0):
        arm = next((r for r in rs if r['kind'] == 'ARM'), None)
        skip = next((r for r in rs if r['kind'] == 'SKIP'), None)
        done = next((r for r in rs if r['kind'] == 'DONE'), None)
        hb = [r for r in rs if r['kind'] == 'HB']
        errs = [r for r in rs if r['kind'] in ('ERR', 'APIERR')]
        probe = arm.get('p') if arm else None
        res['scripts'].append({'sid': sid, 'armed': arm is not None, 'skip': skip is not None, 'probe': probe,
                               'done': done is not None})
        head = 'script %s: %s' % (sid, ('ARMED %s (%s, mode %s)' % (probe, arm.get('fac'), arm.get('mode'))) if arm
                                  else ('skipped (%s, mode %s, human side %s)' % (
                                      skip.get('fac'), skip.get('mode'), skip.get('human_side')) if skip else
                                        'no ARM / SKIP record'))
        lines.append('')
        lines.append(head)
        if not arm:
            continue
        lines.append('  %s; last heartbeat f %s; %d ERR / APIERR' % (
            'DONE ' + done.get('apierr', '') if done else 'NO DONE (a retail call may have stopped the script)',
            hb[-1]['f'] if hb else '?', len(errs)))
        for e in errs[:6]:
            lines.append('    %s %s' % (e['kind'], '|'.join('%s=%s' % (k, e[k]) for k in ('what', 'where', 'api', 'err', 'k')
                                                          if k in e)))
        if probe == 'tb':
            o, r = analyse_tb(rs, cfg)
            res['tb'] = r
            final['A'] = (r['verdicts'].get('A', 'INCONCLUSIVE'), sid)
        else:
            o, r = analyse_cb(rs, cfg)
            res['cb'] = r
            final['B'] = (r['verdicts'].get('B', 'INCONCLUSIVE'), sid)
        lines.extend(o)
    lines.append('')
    va = final.get('A', ('NO DATA', '-'))
    vb = final.get('B', ('NO DATA', '-'))
    res['verdicts'] = ['(A) tractor beam range includes the target footprint: %s' % va[0],
                       '(B) Combat_Bonus aura reaches an allied player\'s units: %s' % vb[0]]
    lines.append('VERDICT ' + res['verdicts'][0] + ' (script %s)' % va[1])
    lines.append('VERDICT ' + res['verdicts'][1] + ' (script %s)' % vb[1])
    text = '\n'.join(lines)
    if out_txt:
        with open(out_txt, 'w') as f:
            f.write(text + '\n')
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(res, f, indent=1, default=str)
    return text, res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('records')
    ap.add_argument('--config')
    ap.add_argument('--out')
    ap.add_argument('--json')
    a = ap.parse_args()
    cfg = a.config
    if cfg is None:
        c = os.path.join(os.path.dirname(os.path.abspath(a.records)), 'config.json')
        cfg = c if os.path.exists(c) else None
    text, res = report(a.records, a.out, a.json, cfg)
    print(text)
    return 0 if res['records'] else 10


if __name__ == '__main__':
    sys.exit(main())
