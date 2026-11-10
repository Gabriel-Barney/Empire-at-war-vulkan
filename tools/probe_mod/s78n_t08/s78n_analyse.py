#!/usr/bin/env python3
"""s78n_analyse.py - turns the s78n t08 probe records into a verdict per case, with the numbers.

  s78n_analyse.py RECORDS.log [--config config.json] [--out report.txt] [--json report.json]

(A) second version (probe "tv", 2026-11-10; TACTICAL_COMBAT.md sec. 5.4.5): the beam may start when the XY centre
    distance d <= R + E, E = the target's collision-bound model Y half x Scale_Factor when its facing is along the
    line of sight (|f.u| > 0.7071), else the X half (Nebulon-B: 109.95 / 23.48, thresholds 1109.95 / 1023.48).
    Per trial: d0 and |f.u| at the order (f from the two facing bones, else the commanded facing), the predicted
    E and IN / OUT, the observed IN (beam with nothing moved) / OUT, and when X was stepped closer (or the user
    walked) the interval [d at the beam, d up to two polls earlier] that holds T, checked against R + E at |f.u|
    of the beam start. MATCH when both agree; the old port rule (along |max.x|, across |max.y|: 1023.48 /
    1111.86) is printed beside it for reference.
      A0 setup     both controls (950) start in place;
      A1 facing    with bone reads: along trials |f.u| > 0.98, across < 0.17, no turn > 1 deg from the settled
                   facing (the settle read that agreed) to the order and to the beam start, the right band at the
                   beam start (ASSUMED without bone reads);
      A2 per trial every trial MATCHes sec. 5.4.5;
      A3 threshold the intervals from all trials per facing hold 1109.95 (along) and 1023.48 (across).
    Verdict (A): INCONCLUSIVE when A0 fails; FAIL when A2 fails; PASS when A3 passes and A1 is PASS / ASSUMED; else
    INCONCLUSIVE.
(A) first version (probe "tb", run 20261109-200249; kept for --report-only of that run): the port let the beam start
    when the XY centre distance
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
import math
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


# ---- (A), second version (probe "tv") -------------------------------------------------------------------------------

def _xy(v):
    try:
        a, b = v.split(',')
        return float(a), float(b)
    except (AttributeError, ValueError):
        return None


def _face(rec):
    if rec is None:
        return None
    fx, fy = fnum(rec.get('fx')), fnum(rec.get('fy'))
    return (fx, fy) if fx is not None and fy is not None else None


def _fu(f, u, x):
    """|f . unit(u - x)|, or None."""
    if f is None or u is None or x is None:
        return None
    dx, dy = u[0] - x[0], u[1] - x[1]
    ln = math.hypot(dx, dy)
    return abs(f[0] * dx + f[1] * dy) / ln if ln > 0 else None


def _turn(a, b):
    """angle in degrees between two facings (0..180), or None."""
    if a is None or b is None:
        return None
    return abs(math.degrees(math.atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])))


def analyse_tv(rs, cfg):
    out = []
    res = {'trials': [], 'verdicts': {}}
    vc = next((r for r in rs if r['kind'] == 'VCFG'), {})
    tvc = (cfg or {}).get('tv', {})

    def val(key, ckey):
        v = fnum(vc.get(key))
        return v if v is not None else tvc.get(ckey)

    R, ea, ec = val('R', 'R'), val('ea', 'e_along'), val('ec', 'e_across')
    oa, oc = val('oa', 'old_along'), val('oc', 'old_across')
    inplace = tvc.get('inplace', 30)
    if None in (R, ea, ec, oa, oc):
        out.append('  no VCFG record and no config: R / E unknown')
        return out, res
    bones = vc.get('bf') or tvc.get('bone_front') or ''
    res.update(R=R, e_along=ea, e_across=ec, T_along=R + ea, T_across=R + ec, old_T_along=R + oa,
               old_T_across=R + oc)
    out.append('  R %.2f; sec. 5.4.5: E along the facing %.2f, across %.2f -> thresholds %.2f / %.2f (|f.u| > 0.7071 '
               '= along); old port %.2f / %.2f; facing read: %s' % (
                   R, ea, ec, R + ea, R + ec, R + oa, R + oc,
                   'bones %s / %s' % (bones, vc.get('br') or tvc.get('bone_rear')) if bones and bones != '_' else
                   'none (the commanded facing is assumed)'))
    trials = {}
    ulock = []
    for r in rs:
        k = r.get('k')
        if k is None:
            continue
        t = trials.setdefault(k, {'k': int(k), 'polls': [], 'faces': []})
        kind = r['kind']
        if kind == 'VTRIAL':
            t.update(name=r.get('name'), face=r.get('face'), d_plan=fnum(r.get('d')), vtrial=r)
        elif kind in ('PLACE', 'SETTLE', 'PRE', 'ENG', 'NOENG', 'STEP', 'TEND'):
            t[kind.lower()] = r
        elif kind == 'ACT' and r.get('why') == 'order':
            t['ret'] = r.get('ret')
        elif kind == 'P':
            t['polls'].append(r)
        elif kind == 'FACE':
            t['faces'].append(r)
        elif kind == 'ULOCK':
            t['ulock'] = r
            ulock.append(r)
    if not any(t.get('name') for t in trials.values()):
        out.append('  no trial records')
        return out, res
    v = res['verdicts']
    ctrl = {}
    face_bad = []
    any_bone = False
    n_new = n_old = n_all = 0
    iv = {'along': [None, None], 'across': [None, None]}   # lo <= T < hi from all trials of that facing

    def narrow(face, lo, hi):
        b = iv[face]
        if lo is not None:
            b[0] = lo if b[0] is None else max(b[0], lo)
        if hi is not None:
            b[1] = hi if b[1] is None else min(b[1], hi)

    for k in sorted(trials, key=lambda x: int(x)):
        t = trials[k]
        if t.get('name') is None:
            continue
        face = t.get('face')
        pre = t.get('pre')
        if pre is None:
            out.append('  trial %d %s: no order (PRE missing; %s)' % (t['k'], t['name'], (t.get('tend') or {}).get(
                'aborted', 'not finished')))
            res['trials'].append({'k': t['k'], 'name': t['name'], 'face': face, 'match': False, 'obs': None})
            n_all += 1
            continue
        d0 = fnum(pre.get('d'))
        u0, x0 = _xy(pre.get('u')), _xy(pre.get('x'))
        f_cmd = None
        if u0 and x0:
            ln = math.hypot(u0[0] - x0[0], u0[1] - x0[1]) or 1.0
            f_cmd = ((u0[0] - x0[0]) / ln, (u0[1] - x0[1]) / ln) if face == 'along' else (0.0, 1.0)
        f_pre = _face(pre)
        src = 'bones' if f_pre else 'commanded'
        any_bone = any_bone or f_pre is not None
        f_use = f_pre or f_cmd
        fu = _fu(f_use, u0, x0)
        # the facing at the start of the measurement: the last settle read (the one that agreed with the read before)
        settled = [_face(r) for r in t['faces'] if r.get('w') == 'settle' and _face(r)]
        f_set = settled[-1] if settled else None
        turned = _turn(f_set, f_pre)
        f_first = settled[0] if settled else None
        first_off = None
        if f_first is not None and f_cmd is not None:
            first_off = _turn(f_first, f_cmd)
            if face == 'along':
                first_off = min(first_off, 180.0 - first_off)
        off = None
        if fu is not None:
            off = math.degrees(math.acos(min(1.0, fu))) if face == 'along' else math.degrees(math.asin(min(1.0, fu)))
        e_new = ea if fu is not None and fu > 0.7071 else ec
        e_old = oa if fu is not None and fu > 0.7071 else oc
        T_new, T_old = R + e_new, R + e_old
        pred = 'IN' if d0 is not None and d0 <= T_new else 'OUT'
        pred_old = 'IN' if d0 is not None and d0 <= T_old else 'OUT'
        eng = t.get('eng')
        um = fnum((eng or {}).get('um'))
        stepping = (eng or {}).get('stepping') == 'true'
        info = {'k': t['k'], 'name': t['name'], 'face': face, 'd_plan': t.get('d_plan'), 'd0': d0, 'fu': fu,
                'facing_src': src, 'off_deg': off, 'turned_deg': turned, 'E': e_new, 'T': T_new, 'pred': pred,
                'E_old': e_old, 'T_old': T_old, 'pred_old': pred_old, 'ret': t.get('ret'),
                'settle': (t.get('settle') or {}).get('why'), 'ulock_pre': (pre or {}).get('ulock')}
        lo = hi = None
        fit = fit_old = True
        r_eng = fnum((eng or {}).get('r'))
        if eng is not None and not stepping and (um is None or um <= 1.0):
            obs = 'IN'
            lo = d0
            note = 'beam r %s%s' % (fmt(r_eng, 0), '' if r_eng is not None and r_eng <= inplace else ' (late)')
        else:
            obs = 'OUT'
            hi = d0
            if eng is not None:
                de = fnum(eng.get('d'))
                dp = max([x for x in (fnum(eng.get('prev_d')), fnum(eng.get('prev2_d'))) if x is not None],
                         default=None)
                fu_e = fnum(eng.get('fu'))     # from the bones, with the positions at the beam start
                if fu_e is None:
                    p_eng = next((p for p in t['polls'] if p.get('r') == eng.get('r')), {})
                    fu_e = _fu(f_cmd, _xy(p_eng.get('u')), _xy(p_eng.get('x'))) or fu
                elif (face == 'along') != (fu_e > 0.7071):
                    face_bad.append('%s at the beam (|f.u| %s: wrong band)' % (t['name'], fmt(fu_e, 4)))
                Te = R + (ea if fu_e is not None and fu_e > 0.7071 else ec)
                To = R + (oa if fu_e is not None and fu_e > 0.7071 else oc)
                lo, hi = de, dp
                fit = de is not None and dp is not None and de - TOL_D <= Te <= dp + TOL_D
                fit_old = de is not None and dp is not None and de - TOL_D <= To <= dp + TOL_D
                info.update(d_eng=de, d_prev=dp, fu_eng=fu_e, T_eng=Te, r_eng=r_eng, user_moved=um,
                            how='target stepped' if stepping else 'user walked')
                note = 'then the beam at d %s after %s: T in [%s, %s] at |f.u| %s -> pred %.2f %s (old %.2f %s)' % (
                    fmt(de), info['how'], fmt(de), fmt(dp), fmt(fu_e, 4), Te, 'fits' if fit else 'DOES NOT FIT', To,
                    'fits' if fit_old else 'no')
            else:
                dmin = min((fnum(p.get('d')) for p in t['polls'] if fnum(p.get('d')) is not None), default=None)
                hi = dmin if dmin is not None else d0
                fit = dmin is None or T_new < dmin + TOL_D
                fit_old = dmin is None or T_old < dmin + TOL_D
                info.update(d_min=dmin)
                note = 'never; smallest d %s (%s)' % (fmt(dmin), 'stepped' if t.get('step') else 'not stepped')
        narrow(face, lo, hi)
        match = obs == pred and fit
        match_old = obs == pred_old and fit_old
        info.update(obs=obs, match=match, match_old=match_old)
        n_all += 1
        n_new += 1 if match else 0
        n_old += 1 if match_old else 0
        if t['name'].startswith('ctrl'):
            ctrl[t['name']] = obs
        turned_eng = _turn(f_set, _face(eng))
        info.update(first_off_deg=first_off, turned_eng_deg=turned_eng)
        if f_pre is not None and (off is None or off > 10.0 or (turned is not None and turned > 1.0) or
                                  (turned_eng is not None and turned_eng > 1.0)):
            face_bad.append('%s (off %s deg, turned %s deg by the order, %s deg by the beam)' % (
                t['name'], fmt(off, 1), fmt(turned, 2), fmt(turned_eng, 2)))
        if face == 'along' and fu is not None and fu <= 0.7071 or face == 'across' and fu is not None and fu > 0.7071:
            face_bad.append('%s (|f.u| %s: wrong band)' % (t['name'], fmt(fu, 4)))
        res['trials'].append(info)
        out.append('  trial %2d %-12s %-6s d0 %8s |f.u| %s (%s; %s deg off the plan; first read %s deg off; turned '
                   '%s deg from the settled facing by the order, %s by the beam; settle %s) E %.2f -> pred %s; '
                   'observed %s, %s: %s  [old port E %.2f -> %s: %s]' % (
                       t['k'], t['name'], face, fmt(d0), fmt(fu, 4), src, fmt(off, 1), fmt(first_off, 1),
                       fmt(turned, 2), fmt(turned_eng, 2), info['settle'], e_new, pred, obs, note,
                       'MATCH' if match else 'MISMATCH', e_old, pred_old, 'agrees' if match_old else 'differs'))
    for u in ulock:
        out.append('  ULOCK: the locked user did not start the beam on %s (k %s, r %s): Suspend_Locomotor(false), '
                   'later trials let the user walk (bracket from its approach)' % (
                       trials.get(u.get('k'), {}).get('name'), u.get('k'), u.get('r')))
    a0 = ctrl.get('ctrl_along') == 'IN' and ctrl.get('ctrl_across') == 'IN'
    v['A0'] = 'PASS' if a0 else 'INCONCLUSIVE'
    out.append('  A0 setup (controls at %s start in place): %s (ctrl_along %s, ctrl_across %s)' % (
        fmt(tvc.get('ctrl_d', R - 50), 0), v['A0'], ctrl.get('ctrl_along'), ctrl.get('ctrl_across')))
    if not any_bone:
        v['A1'] = 'ASSUMED'
        a1why = 'no facing read; Face_Immediate assumed to have set the planned facing'
    elif face_bad:
        v['A1'] = 'FAIL'
        a1why = 'facing not as planned / not settled: ' + ', '.join(face_bad)
    else:
        v['A1'] = 'PASS'
        a1why = ('along |f.u| > 0.98, across < 0.17 (within 10 deg); no turn > 1 deg from the settled facing to the '
                 'order and to the beam start')
    out.append('  A1 target facing: %s (%s)' % (v['A1'], a1why))
    v['A2'] = 'PASS' if n_new == n_all else 'FAIL'
    out.append('  A2 every trial in / out as sec. 5.4.5 predicts: %s (%d / %d trials)' % (v['A2'], n_new, n_all))

    def ivs(b):
        return '[%s, %s)' % (fmt(b[0]) if b[0] is not None else '-inf', fmt(b[1]) if b[1] is not None else '+inf')

    def holds(b, T):
        return (b[0] is None or b[0] <= T + TOL_D) and (b[1] is None or T < b[1] + TOL_D)

    ok_a, ok_c = holds(iv['along'], R + ea), holds(iv['across'], R + ec)
    v['A3'] = 'PASS' if ok_a and ok_c else 'FAIL'
    res['bounds'] = iv
    out.append('  A3 thresholds: along T in %s (pred %.2f %s; old port %.2f %s), across T in %s (pred %.2f %s; old '
               'port %.2f %s): %s' % (
                   ivs(iv['along']), R + ea, 'in' if ok_a else 'OUT', R + oa, 'in' if holds(iv['along'], R + oa)
                   else 'out', ivs(iv['across']), R + ec, 'in' if ok_c else 'OUT', R + oc,
                   'in' if holds(iv['across'], R + oc) else 'out', v['A3']))
    out.append('  old port rule (along %.2f / across %.2f, for reference): %d / %d trials agree' % (
        R + oa, R + oc, n_old, n_all))
    if v['A0'] != 'PASS':
        overall = 'INCONCLUSIVE'
    elif v['A2'] == 'FAIL':
        overall = 'FAIL'
    elif v['A3'] == 'PASS' and v['A1'] in ('PASS', 'ASSUMED'):
        overall = 'PASS'
    else:
        overall = 'INCONCLUSIVE'
    v['A'] = overall
    res['old_agree'] = (n_old, n_all)
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
    res = {'records': len(recs), 'scripts': [], 'tb': None, 'tv': None, 'cb': None, 'verdicts': []}
    final = {}
    space_only = bool((cfg or {}).get('space_only'))
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
        elif probe == 'tv':
            o, r = analyse_tv(rs, cfg)
            res['tv'] = r
            final['A'] = (r['verdicts'].get('A', 'INCONCLUSIVE'), sid)
            space_only = space_only or next((x.get('space_only') for x in rs if x['kind'] == 'VCFG'), '') == 'true'
        else:
            o, r = analyse_cb(rs, cfg)
            res['cb'] = r
            final['B'] = (r['verdicts'].get('B', 'INCONCLUSIVE'), sid)
        lines.extend(o)
    lines.append('')
    va = final.get('A', ('NO DATA', '-'))
    vb = final.get('B', ('NO DATA (space-only run)' if space_only else 'NO DATA', '-'))
    tv = res.get('tv')
    if tv and 'T_along' in tv:
        n_old, n_all = tv.get('old_agree', (0, 0))
        res['verdicts'] = ['(A) tractor beam range, TACTICAL_COMBAT 5.4.5 (R + E: along %.2f / across %.2f): %s '
                           '(old port %.2f / %.2f: %d / %d trials agree)' % (
                               tv['T_along'], tv['T_across'], va[0], tv['old_T_along'], tv['old_T_across'], n_old,
                               n_all)]
    else:
        res['verdicts'] = ['(A) tractor beam range includes the target footprint: %s' % va[0]]
    res['verdicts'].append('(B) Combat_Bonus aura reaches an allied player\'s units: %s' % vb[0])
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
