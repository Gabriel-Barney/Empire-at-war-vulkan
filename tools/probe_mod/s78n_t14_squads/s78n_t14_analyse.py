#!/usr/bin/env python3
"""s78n_t14_analyse.py - report of an s78n t14 land-squad formation run (records "S8Q|...|END").

  s78n_t14_analyse.py RECORDS.log [--config config.json] [--out report.txt] [--json report.json]

Per trial (one squad: Rows = Stormtrooper_Team, Dartboard = Rebel_Trooper_Team) and per leg (Move_To of the team
container; FIGHTER_FLYING sec. 7 - 7.4):
  * the container: its start (frames after the order), cruise speed, heading vs the planned one, stop frame, miss;
  * Q2 start frames: each member's start, estimated from its first moving polls (sqrt(displacement) linear in t =
    constant acceleration; clamped to the poll interval), minus the container's start; the spread over the members;
  * Q1 lag: member offsets in the container frame (heading u = the container's own displacement over the leg; along
    +u, side +perp(u), as sec. 7 step 3) minus the member's settled slot (mean offset over the last 60 frames before
    the next order); lag = slot along - along (positive = behind). A table at marks after the container start and
    after its stop, and the steady march (container at >= 90 % of its cruise speed, from start + 60 to stop - 30):
    mean / max lag, mean |side error|; each member's peak speed against the container's cruise speed (> 1.05: the
    members walk faster than the team while behind = they can make up a lag); the close-up after the stop;
  * Q3 layout: the settled slots; Rows: RMS against the sec. 7.2 Rows layout with the member's own FormationSpacing
    (the port) and with the container's; Dartboard: the extent against L / 2, Wd / 2, the smallest spacing against
    5 + r, and the best Halton fit (seed = container id x frame, frames around the order; baseline = frames far from
    it); between legs the change of the slot set (re-lay) and of each member's slot.
Frames are mode frames read at the free-store poll (every 3 frames), so a start or stop is known to +-3 frames
without the fit.
"""
import argparse
import json
import math
import re
import sys

sys.dont_write_bytecode = True
REC_RE = re.compile(r'S8Q\|([A-Z0-9_]+)\|(.*)\|END')
SETTLE_WIN = 60      # frames before the next order whose polls give a member's settled slot
MOVE_EPS = 0.3       # a member / the container has started when it is this far from its position at the order
RECOVER_D = 4.0      # sec. 7.3 item 6: recovery threshold
CROSS_D = 1.5        # Q2: a member's start is compared through the frame it is this far from its position at the order
HALTON_WIN = 60      # Dartboard fit: run offsets searched on each side (logic frame - record frame)
HALTON_FINE = 3      # Dartboard fit: per-leg shift around the run offset (when the order is served)
HALTON_MATCH = 0.6   # the fit is a match when its RMS is below this x the best of as many far-away frames


# ---- parsing --------------------------------------------------------------------------------------------------------

def parse_line(line):
    m = REC_RE.search(line)
    if not m:
        return None
    kind, body = m.group(1), m.group(2)
    d = {'kind': kind}
    for part in body.split('|'):
        if '=' in part:
            k, v = part.split('=', 1)
            d[k] = v
    try:
        d['n'] = int(d.get('n', '0'))
    except ValueError:
        d['n'] = 0
    try:
        d['f'] = int(d['f'])
    except (KeyError, ValueError):
        d['f'] = None
    return d


def xy(s):
    if s is None or s in ('?', ''):
        return None
    try:
        a, b = s.split(',')[:2]
        return float(a), float(b)
    except ValueError:
        return None


def load(path):
    recs = []
    seen = set()
    with open(path, errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line in seen:
                continue
            seen.add(line)
            r = parse_line(line)
            if r:
                recs.append(r)
    return recs


# ---- geometry -------------------------------------------------------------------------------------------------------

def sub(a, b):
    return a[0] - b[0], a[1] - b[1]


def norm(v):
    return math.hypot(v[0], v[1])


def frame_of(u, d):
    """(along, side) of d in the frame of the unit heading u (sec. 7 step 3)."""
    return u[0] * d[0] + u[1] * d[1], -u[1] * d[0] + u[0] * d[1]


def unit(v):
    n = norm(v)
    return (v[0] / n, v[1] / n) if n > 1e-9 else None


def angle_deg(a, b):
    c = max(-1.0, min(1.0, a[0] * b[0] + a[1] * b[1]))
    s = a[0] * b[1] - a[1] * b[0]
    return math.degrees(math.atan2(s, c))


def assign(a, b):
    """Min-cost (sum of squared distances) matching of point lists a and b (same length <= 12, bitmask DP).
    Returns (rms, pairs [(i, j)])."""
    n = len(a)
    if n == 0 or n != len(b):
        return None, []
    cost = [[(a[i][0] - b[j][0]) ** 2 + (a[i][1] - b[j][1]) ** 2 for j in range(n)] for i in range(n)]
    INF = float('inf')
    dp = [INF] * (1 << n)
    par = [None] * (1 << n)
    dp[0] = 0.0
    for mask in range(1 << n):
        if dp[mask] == INF:
            continue
        i = bin(mask).count('1')
        if i >= n:
            continue
        for j in range(n):
            if not mask & (1 << j):
                nm = mask | (1 << j)
                v = dp[mask] + cost[i][j]
                if v < dp[nm]:
                    dp[nm] = v
                    par[nm] = (mask, j)
    pairs = []
    mask = (1 << n) - 1
    i = n - 1
    while mask:
        pm, j = par[mask]
        pairs.append((i, j))
        mask = pm
        i -= 1
    return math.sqrt(dp[(1 << n) - 1] / n), sorted(pairs)


def ceil_sqrt(n):
    """0x5abc00's rule: (uint)sqrtf(n), + 1 unless squared = n."""
    s = int(math.sqrt(float(n)))
    return s if s * s == n else s + 1


def rows_slots(n, hx, hy, spacing, f=1.0):
    """sec. 7.2 Rows 0x5ac970 for n identical members (half-extents hx along, hy side): the slot set
    [(along, side)], rows front first, each row from the most negative side."""
    k = ceil_sqrt(n)
    if k == 0:
        return []
    G = spacing * 20.0 * f
    along = 0.0
    HX0 = None
    D = 0.0
    out = []
    left = n
    while left > 0:
        r = min(k, left)
        left -= r
        HX = hx
        W = 2.0 * hy if r == 1 else 2.0 * (G / 2.0 + 2.0 * hy) + (r - 2) * (2.0 * hy + G)
        if HX0 is None:
            HX0 = HX
        else:
            along -= G / 2.0 + HX
        if left == 0:
            D = HX0 - (along - HX)
        side = -W / 2.0
        for i in range(r):
            if r == 1:
                side = 0.0
            elif i == 0:
                side += hy
            else:
                side += G / 2.0 + hy
            out.append((along, side))
            side += G / 2.0 + hy
        along -= G / 2.0 + HX
    return [(a + D / 2.0 - HX0, s) for a, s in out]


def radical_inverse(i, b):
    f, bk = 0.0, float(b)
    while i:
        f += (i % b) / bk
        i //= b
        bk *= b
    return f


def dartboard_points(n, hx, hy, spacing, seed, f=1.0):
    """sec. 7.2 Dartboard 0x5ac2a0: the accepted Halton points [(x along, y side)] (None: fewer than n -> Rows)."""
    k = ceil_sqrt(n)
    if k == 0:
        return None
    m = n // k + (1 if n % k else 0)
    r = math.sqrt(hx * hx + hy * hy)
    L = (k - 1) * spacing * 20.0 + k * hx
    Wd = (m - 1) * spacing * 20.0 + m * hy
    L, Wd, r = L * f, Wd * f, r * f
    i2 = i3 = seed & 0xFFFFFFFF
    pts = []
    for _ in range(1000):
        if len(pts) >= n:
            break
        h2 = radical_inverse(i2, 2)
        i2 = (i2 + 1) & 0xFFFFFFFF
        h3 = radical_inverse(i3, 3)
        i3 = (i3 + 1) & 0xFFFFFFFF
        x = (h2 - 0.5) * L
        y = (h3 - 0.5) * Wd
        if all(math.hypot(y - py, x - px) >= 5.0 + r for px, py in pts):
            pts.append((x, y))
    return pts if len(pts) >= n else None


def dart_dims(n, hx, hy, spacing):
    k = ceil_sqrt(n)
    m = n // k + (1 if n % k else 0)
    return {'k': k, 'm': m, 'L': (k - 1) * spacing * 20.0 + k * hx, 'Wd': (m - 1) * spacing * 20.0 + m * hy,
            'r': math.sqrt(hx * hx + hy * hy)}


def fmt(v, f='%.1f'):
    return '-' if v is None else f % v


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def median(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


# ---- one trial ------------------------------------------------------------------------------------------------------

def start_fit(track, f_order, p_order, eps=MOVE_EPS):
    """Start frame of an object from its (f, pos) track after the order: the first poll farther than eps from its
    position at the order, then a constant-acceleration fit of sqrt(displacement) over the first moving polls
    (displacement <= 8), clamped to [previous poll, first moving poll]. Returns (fit, first moving poll) or (None,
    None)."""
    if p_order is None:
        return None, None
    prev_f = f_order
    first = None
    samples = []
    for f, p in track:
        if f < f_order or p is None:
            continue
        d = norm(sub(p, p_order))
        if first is None:
            if d > eps:
                first = f
                lo = prev_f
            else:
                prev_f = f
                continue
        if d <= 8.0 and len(samples) < 3:
            samples.append((f, math.sqrt(d)))
        else:
            break
    if first is None:
        return None, None
    s = None
    if len(samples) >= 2:
        n = len(samples)
        mx = sum(x for x, _ in samples) / n
        my = sum(y for _, y in samples) / n
        vx = sum((x - mx) ** 2 for x, _ in samples)
        if vx > 0:
            a = sum((x - mx) * (y - my) for x, y in samples) / vx
            if a > 1e-6:
                s = mx - my / a
    if s is None:
        s = first - 1.5
    return max(lo, min(first, s)), first


def cross_time(track, f_order, p_order, level=CROSS_D):
    """Frame at which an object's distance from its position at the order first reaches `level`, linearly
    interpolated between the two polls around it (sub-poll resolution for comparing members that move alike)."""
    if p_order is None:
        return None
    prev = None
    for f, p in track:
        if f < f_order or p is None:
            continue
        d = norm(sub(p, p_order))
        if d >= level:
            if prev is None or d <= prev[1]:
                return float(f)
            return prev[0] + (level - prev[1]) / (d - prev[1]) * (f - prev[0])
        prev = (f, d)
    return None


def dart_fit(legs, cfg):
    """The Dartboard layouts of a whole run (sec. 7.2: Halton bases 2 / 3 seeded with container id x NOW).
    legs: [{'key': (trial, leg), 'slots': settled slots, 'cid', 'n', 'events': [(label, frame)]}] (Dartboard legs only).
    NOW is the logic frame; the records' f is GetCurrentTime() x 30, so one offset o per run is fitted: for each o in
    +-HALTON_WIN, each leg takes its best event e and fine shift d in +-HALTON_FINE (RMS of the min-cost match of its
    settled slots to the scatter of frame e + o + d), and the score is the mean over the legs. The baseline runs the
    same search over as many offsets far away (o + 5000 ...). Returns {o, score, base_min, base_med, match, legs:
    {key: {rms, event, df, frame, pts, pairs}}} or None."""
    xv = (cfg or {}).get('xml', {}).get('dart', {})
    hx, hy = xv.get('hx') or 3.0, xv.get('hy') or 3.0
    g = xv.get('m_spacing') or 0.5
    legs = [L for L in legs if L['cid'] is not None and len(clean(L['slots'])) == L['n'] and L['n'] > 1]
    if not legs:
        return None
    cache = {}

    def leg_best(L, o):
        best = None
        for label, fe in L['events']:
            for d in range(-HALTON_FINE, HALTON_FINE + 1):
                fr = fe + o + d
                key = (L['cid'], L['n'], fr)
                if key not in cache:
                    cache[key] = dartboard_points(L['n'], hx, hy, g, (L['cid'] * fr) & 0xFFFFFFFF)
                pts = cache[key]
                if not pts:
                    continue
                rms, pairs = assign(clean(L['slots']), pts)
                if best is None or rms < best['rms']:
                    best = {'rms': rms, 'event': label, 'df': o + d, 'frame': fr, 'pts': pts, 'pairs': pairs}
        return best

    def score(o):
        bs = [leg_best(L, o) for L in legs]
        if any(x is None for x in bs):
            return None, bs
        return mean([x['rms'] for x in bs]), bs

    step = 2 * HALTON_FINE + 1
    scored = []
    kmax = HALTON_WIN // step
    for o in [i * step for i in range(-kmax, kmax + 1)]:  # centred on 0
        sc, bs = score(o)
        if sc is not None:
            scored.append((sc, o, bs))
    base = []
    for o in [5000 + i * step for i in range(0, 2 * kmax + 1)]:
        sc, _ = score(o)
        if sc is not None:
            base.append(sc)
    if not scored:
        return None
    sc, o, bs = min(scored, key=lambda x: x[0])
    bmin = min(base) if base else None
    return {'o': o, 'score': sc, 'base_min': bmin, 'base_med': median(base),
            'match': bool(bmin and sc <= HALTON_MATCH * bmin),
            'legs': {L['key']: x for L, x in zip(legs, bs)}}


def predict(t, cfg, slots, f_order, dfit=None, leg=None):
    """The layout the RE predicts for this leg's settled slots (sec. 7.2), assigned to the members by the smallest
    squared distance. Returns (reference slots per member or None, info)."""
    xv = (cfg or {}).get('xml', {}).get(t['key'], {})
    arr = (t.get('arr') or xv.get('arrangement') or '?').upper()
    n = len(slots)
    hx, hy = xv.get('hx') or 3.0, xv.get('hy') or 3.0
    jig = xv.get('m_jiggle') or 0.0
    info = {'arr': arr, 'jiggle': jig}
    cs = clean(slots)
    if len(cs) != n or n == 0:
        return None, info
    ref = None
    if arr == 'ROWS':
        for tag, g in (('member', xv.get('m_spacing')), ('container', xv.get('c_spacing'))):
            if g is None:
                continue
            pred = rows_slots(n, hx, hy, g)
            rms, pairs = assign(cs, pred)
            info['rows_' + tag] = {'spacing': g, 'pitch': 2 * hx + g * 20.0, 'rms': rms}
            if tag == 'member':
                ref = [None] * n
                for i, j in pairs:
                    ref[i] = pred[j]
    elif arr == 'DARTBOARD':
        g = xv.get('m_spacing') or 0.5
        dims = dart_dims(n, hx, hy, g)
        info['dims'] = dims
        info['max_along'] = max(abs(a) for a, _ in cs)
        info['max_side'] = max(abs(s) for _, s in cs)
        info['min_gap'] = min(math.hypot(a[0] - b[0], a[1] - b[1]) for i, a in enumerate(cs) for b in cs[i + 1:]) \
            if n > 1 else None
        h = (dfit or {}).get('legs', {}).get((t['k'], leg))
        if h:
            info['halton'] = {k: v for k, v in h.items() if k not in ('pts', 'pairs')}
            info['halton'].update({'match': dfit['match'], 'o': dfit['o'], 'score': dfit['score'],
                                   'base_min': dfit['base_min']})
            if dfit['match']:
                ref = [None] * n
                for i, j in h['pairs']:
                    ref[i] = h['pts'][j]
    return ref, info


def trial_cid(t):
    try:
        return int(float(t.get('cid')))
    except (TypeError, ValueError):
        return None


def analyse_trial(t, cfg, dfit=None):
    """t: {'k', 'key', 'ids', 'polls', 'legs', 'events'}. Returns a result dict."""
    xv = (cfg or {}).get('xml', {}).get(t['key'], {})
    jig = xv.get('m_jiggle') or 0.0
    n = len(t['ids'])
    res = {'k': t['k'], 'key': t['key'], 'n': n, 'cid': t.get('cid'), 'legs': [], 'arr': t.get('arr'),
           'company': t.get('company'), 'notes': []}
    polls = t['polls']
    if not polls or n == 0:
        res['notes'].append('no P records' if not polls else 'no members')
        return res
    # the container's position: readable? equal to a member's position (a wrapper returning a member)?
    nc = sum(1 for p in polls if p['c'] is not None)
    same = 0
    for p in polls:
        if p['c'] is None:
            continue
        for m in p['m']:
            if m is not None and abs(m[0] - p['c'][0]) < 0.011 and abs(m[1] - p['c'][1]) < 0.011:
                same += 1
                break
    res['c_readable'] = nc
    res['c_on_member'] = same
    res['polls'] = len(polls)
    legs = t['legs']
    prev_slots = None
    for li, lg in enumerate(legs):
        f_order = lg['f']
        f_next = legs[li + 1]['f'] if li + 1 < len(legs) else polls[-1]['f'] + 1
        win = [p for p in polls if f_order <= p['f'] < f_next]
        before = [p for p in polls if p['f'] <= f_order]
        if not win or not before:
            res['legs'].append({'leg': lg['leg'], 'name': lg.get('name'), 'missing': True})
            continue
        p0 = before[-1]
        pm1 = before[-2] if len(before) >= 2 else None
        use_c = p0['c'] is not None

        def centre(p):
            if use_c and p['c'] is not None:
                return p['c']
            pts = [m for m in p['m'] if m is not None]
            return (sum(a for a, _ in pts) / len(pts), sum(b for _, b in pts) / len(pts)) if pts else None

        ctrack = [(p['f'], centre(p)) for p in win]
        c0 = centre(p0)
        cs, _cfirst = start_fit(ctrack, f_order, c0)
        # stop: the CSTOP record minus the still polls, else the last poll with a step > 0.02
        last_mv = None
        for (fa, a), (fb, b) in zip(ctrack, ctrack[1:]):
            if a is not None and b is not None and norm(sub(b, a)) > 0.02:
                last_mv = fb
        f_stop = last_mv if last_mv is not None else lg.get('stop_f')
        c_end = None
        for f, c in ctrack:
            if f_stop is not None and f >= f_stop and c is not None:
                c_end = c
                break
        if c_end is None:
            c_end = ctrack[-1][1]
        planned = (lg['dx'], lg['dy']) if lg.get('dx') is not None else unit(sub(lg['to'], lg['from']))
        disp = sub(c_end, c0) if c_end and c0 else None
        u = unit(disp) if disp and norm(disp) >= 20 else planned
        dev = angle_deg(planned, u) if planned and u else None
        sp = []
        for (fa, a), (fb, b) in zip(ctrack, ctrack[1:]):
            if a is not None and b is not None and fb > fa:
                sp.append((fb, norm(sub(b, a)) / (fb - fa)))
        vmax = max((v for _, v in sp), default=0.0)
        cruise = median([v for _, v in sp if v >= 0.8 * vmax]) if vmax > 0 else None
        # settled slots: the last SETTLE_WIN frames of the window (and >= 15 after the stop)
        swin = [p for p in win if p['f'] >= f_next - SETTLE_WIN and (f_stop is None or p['f'] >= f_stop + 15)]
        slots = []
        for j in range(n):
            offs = []
            for p in swin:
                c = centre(p)
                if c is not None and j < len(p['m']) and p['m'][j] is not None:
                    offs.append(frame_of(u, sub(p['m'][j], c)))
            slots.append((mean([o[0] for o in offs]), mean([o[1] for o in offs])) if offs else None)
        events = [('spawn', t['spawn_f'])] if t.get('spawn_f') is not None else []
        events += [('leg %d order' % x['leg'], x['f']) for x in legs[:li + 1]]
        # march slots: the mean offsets over the steady march (no recovery dead band there; members steer to the
        # slot itself). Used for the Dartboard layout fit; falls back to the settled slots.
        mslots = None
        if cs is not None and f_stop is not None and cruise:
            spd0 = dict(sp)
            mwin = [p for p in win if cs + 60 <= p['f'] <= f_stop - 30 and spd0.get(p['f'], 0.0) >= 0.9 * cruise]
            if len(mwin) >= 5:
                mslots = []
                for j in range(n):
                    offs = []
                    for p in mwin:
                        c = centre(p)
                        if c is not None and j < len(p['m']) and p['m'][j] is not None:
                            offs.append(frame_of(u, sub(p['m'][j], c)))
                    mslots.append((mean([o[0] for o in offs]), mean([o[1] for o in offs])) if offs else None)
        pref, info = predict(t, cfg, slots, f_order, dfit, lg['leg'])
        refs = {'settled': slots, 'pred': pref}
        # member starts
        starts = []
        moving_at_order = 0
        for j in range(n):
            mtrack = [(p['f'], p['m'][j] if j < len(p['m']) else None) for p in win]
            m0 = p0['m'][j] if j < len(p0['m']) else None
            if pm1 is not None and m0 is not None and j < len(pm1['m']) and pm1['m'][j] is not None and \
                    norm(sub(m0, pm1['m'][j])) > 0.1:
                moving_at_order += 1
            s, first = start_fit(mtrack, f_order, m0)
            starts.append({'j': j, 'fit': s, 'first': first, 'tD': cross_time(mtrack, f_order, m0),
                           'rel_c': None if s is None or cs is None else s - cs,
                           'rel_o': None if s is None else s - f_order})
        tDs = [x['tD'] for x in starts if x['tD'] is not None]
        for x in starts:
            x['rel'] = None if x['tD'] is None else x['tD'] - min(tDs)
        rels = [x['rel'] for x in starts if x['rel'] is not None]
        c_tD = cross_time(ctrack, f_order, c0)

        def errs_at(p, ref):
            c = centre(p)
            out = []
            if ref is None or c is None:
                return out
            for j in range(n):
                if ref[j] is None or ref[j][0] is None or j >= len(p['m']) or p['m'][j] is None:
                    continue
                a, s_ = frame_of(u, sub(p['m'][j], c))
                ea, es = a - ref[j][0], s_ - ref[j][1]
                out.append({'lag': -ea, 'side': es, 'd': math.hypot(ea, es)})
            return out

        def summ(es):
            if not es:
                return None
            return {'mean_lag': mean([e['lag'] for e in es]), 'max_lag': max(e['lag'] for e in es),
                    'mean_side': mean([abs(e['side']) for e in es]), 'max_d': max(e['d'] for e in es)}

        mark_fs = []
        if cs is not None:
            for dt in (0, 15, 30, 60, 120, 240, 360, 480):
                ft = cs + dt
                if f_stop is not None and ft >= f_stop:
                    break
                mark_fs.append(('start+%d' % dt, min(win, key=lambda q: abs(q['f'] - ft))))
        if f_stop is not None:
            pre = [p for p in win if p['f'] < f_stop]
            if pre:
                mark_fs.append(('pre-stop', pre[-1]))
            for dt in (0, 15, 30, 60, 90):
                ft = f_stop + dt
                if ft >= f_next:
                    break
                mark_fs.append(('stop+%d' % dt, min(win, key=lambda q: abs(q['f'] - ft))))
        marks = [(name, p['f'], summ(errs_at(p, slots)), summ(errs_at(p, pref))) for name, p in mark_fs]
        steady = {}
        if cs is not None and f_stop is not None and cruise:
            spd = dict(sp)
            for key, ref in refs.items():
                es = []
                for p in win:
                    if cs + 60 <= p['f'] <= f_stop - 30 and spd.get(p['f'], 0.0) >= 0.9 * cruise:
                        es.extend(errs_at(p, ref))
                st = summ(es)
                if st:
                    st['p90_d'] = sorted(e['d'] for e in es)[int(0.9 * (len(es) - 1))]
                    st['samples'] = len(es)
                    steady[key] = st
        peaks = []
        if cs is not None and f_stop is not None:
            for j in range(n):
                best = 0.0
                prev = None
                for p in win:
                    if p['f'] < cs or p['f'] > f_stop or j >= len(p['m']) or p['m'][j] is None:
                        prev = None
                        continue
                    if prev is not None and p['f'] > prev[0]:
                        best = max(best, norm(sub(p['m'][j], prev[1])) / (p['f'] - prev[0]))
                    prev = (p['f'], p['m'][j])
                peaks.append(best)
        close, tight = {}, {}
        for key, ref in refs.items():
            close[key] = tight[key] = None
            if ref is None:
                continue
            for p in win:
                es = errs_at(p, ref)
                ok = es and len(es) == n and max(e['d'] for e in es) <= RECOVER_D + jig
                if not ok:
                    continue
                if f_stop is not None and p['f'] >= f_stop and close[key] is None:
                    close[key] = p['f'] - f_stop
                if cs is not None and p['f'] >= cs and (f_stop is None or p['f'] < f_stop) and tight[key] is None:
                    tight[key] = p['f'] - cs
        cslot = clean(slots)
        info['centroid'] = (mean([a for a, _ in cslot]), mean([s for _, s in cslot])) if cslot else None
        if prev_slots is not None and len(clean(prev_slots)) == n and len(cslot) == n:
            info['set_change'] = assign(clean(prev_slots), cslot)[0]
            info['member_change'] = math.sqrt(mean([(a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
                                                    for a, b in zip(prev_slots, slots)]))
        prev_slots = slots
        miss = norm(sub(c_end, lg['to'])) if c_end and lg.get('to') else None
        res['legs'].append({
            'leg': lg['leg'], 'name': lg.get('name'), 'turn': lg.get('turn'), 'f_order': f_order, 'c_start': cs,
            'c_start_rel': None if cs is None else cs - f_order, 'f_stop': f_stop, 'cruise': cruise, 'dev_deg': dev,
            'u': u, 'miss': miss, 'slots': slots, 'pred_slots': pref, 'starts': starts, 'rel_starts': rels,
            'spread': (max(rels) - min(rels)) if rels else None, 'moving_at_order': moving_at_order,
            'lead_c': (min(tDs) - c_tD) if tDs and c_tD is not None else None,
            'marks': marks, 'steady': steady, 'peaks': peaks,
            'peak_ratio_max': (max(peaks) / cruise) if peaks and cruise else None,
            'peak_ratio_med': (median(peaks) / cruise) if peaks and cruise else None,
            'close_f': close, 'tight_f': tight, 'reorder': lg.get('reorder'), 'legto': lg.get('legto'),
            'who': lg.get('who'), 'ret': lg.get('ret'), 'centre': 'container' if use_c else 'centroid',
            'layout': info, 'events': events, 'mslots': mslots})
        if li == 0:
            # the first layout (settle phase before leg 1), in leg 1's frame: the reference for leg 1's change
            f1 = f_order
            sw = [p for p in polls if f1 - SETTLE_WIN <= p['f'] <= f1 and p['c'] is not None]
            s0 = []
            for j in range(n):
                offs = [frame_of(u, sub(p['m'][j], p['c'])) for p in sw if j < len(p['m']) and p['m'][j] is not None]
                s0.append((mean([o[0] for o in offs]), mean([o[1] for o in offs])) if offs else None)
            res['slots0'] = s0
            if len(clean(s0)) == n and len(cslot) == n:
                info['set_change'] = assign(clean(s0), cslot)[0]
                info['member_change'] = math.sqrt(mean([(a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2
                                                        for a, b in zip(s0, slots)]))
    return res


def clean(slots):
    return [s for s in slots if s is not None and s[0] is not None]


# ---- the report -----------------------------------------------------------------------------------------------------

def collect(recs):
    by_sid = {}
    for r in recs:
        by_sid.setdefault(r.get('sid', '?'), []).append(r)
    scripts = []
    for sid, rs in by_sid.items():
        rs.sort(key=lambda r: r['n'])
        s = {'sid': sid, 'armed': any(r['kind'] == 'ARM' for r in rs), 'skip': [r for r in rs if r['kind'] == 'SKIP'],
             'done': any(r['kind'] == 'DONE' for r in rs), 'errs': [r for r in rs if r['kind'] == 'ERR'],
             'apierr': [r for r in rs if r['kind'] == 'APIERR'], 'recs': rs, 'n': len(rs)}
        scripts.append(s)
    return scripts


def trials_of(rs, cfg):
    sq = (cfg or {}).get('sq', {})
    legs_cfg = sq.get('legs', [])
    trials = {}
    for r in rs:
        k = r.get('k')
        if k is None:
            continue
        t = trials.setdefault(k, {'k': int(k), 'key': '?', 'ids': [], 'polls': [], 'legs': [], 'events': []})
        kind = r['kind']
        if kind == 'TRIAL':
            t['key'] = r.get('key', '?')
            t['arr'] = r.get('arr')
            t['company'] = r.get('company')
            t['how'] = r.get('how')
            t['spawn_f'] = r['f']
        elif kind == 'TEAM':
            t['cid'] = r.get('cid')
            t['ids'] = [x for x in r.get('ids', '').split(';') if x]
            t['team'] = r
        elif kind == 'P' and r['f'] is not None:
            ms = [xy(x) for x in r.get('m', '').split(';')]
            if ms and ms[-1] is None and r.get('m', '').endswith(';'):
                ms = ms[:-1]
            t['polls'].append({'f': r['f'], 'n': r['n'], 'leg': int(r.get('leg', '0')), 'ph': r.get('ph'),
                               'c': xy(r.get('c')), 'm': ms})
        elif kind == 'LEG' and r.get('via') in (None, 'nil'):
            i = int(r['leg'])
            lc = legs_cfg[i - 1] if 0 < i <= len(legs_cfg) else {}
            t['legs'].append({'leg': i, 'f': r['f'], 'name': r.get('name'), 'turn': float(r.get('turn', '0') or 0),
                              'from': xy(r.get('from')), 'to': xy(r.get('to')), 'who': r.get('who'),
                              'ret': r.get('ret'), 'dx': lc.get('dx'), 'dy': lc.get('dy')})
        elif kind in ('CSTOP', 'REORDER', 'LEGTO', 'CSTART'):
            t['events'].append(r)
    out = []
    for k in sorted(trials, key=lambda x: int(x)):
        t = trials[k]
        t['polls'].sort(key=lambda p: (p['f'], p['n']))
        ded = []
        for p in t['polls']:
            if ded and ded[-1]['f'] == p['f']:
                continue
            ded.append(p)
        t['polls'] = ded
        t['legs'].sort(key=lambda x: x['f'])
        for e in t['events']:
            for lg in t['legs']:
                if str(lg['leg']) == e.get('leg'):
                    if e['kind'] == 'CSTOP':
                        lg['stop_f'] = e['f']
                    elif e['kind'] == 'REORDER':
                        lg['reorder'] = True
                    elif e['kind'] == 'LEGTO':
                        lg['legto'] = True
        out.append(t)
    return out


def verdicts(results, cfg):
    """Per squad kind: Q1 / Q2 / Q3 summaries."""
    xvall = (cfg or {}).get('xml', {})
    maxd = xvall.get('max_move_frame_delay', 6.0)
    out = {}
    for key in ('rows', 'dart'):
        rs = [r for r in results if r['key'] == key]
        if not rs:
            continue
        legs = [L for r in rs for L in r['legs'] if not L.get('missing')]
        v = {'trials': len(rs)}
        jig = xvall.get(key, {}).get('m_jiggle') or 0.0
        # Q1 against the predicted layout when every leg has one (independent of where the members settle), else
        # against the settled slots (blind to a squad that never closes up: see the settled centroid)
        ref = 'pred' if legs and all(L['steady'].get('pred') for L in legs if L['steady']) and \
            any(L['steady'].get('pred') for L in legs) else 'settled'
        v['q1_ref'] = ref
        for rk in ('settled', 'pred'):
            steady = [L['steady'][rk] for L in legs if L['steady'].get(rk)]
            if not steady:
                continue
            v['steady_' + rk] = {'mean_lag': mean([s['mean_lag'] for s in steady]),
                                 'max_lag': max(s['max_lag'] for s in steady),
                                 'mean_side': mean([s['mean_side'] for s in steady]),
                                 'p90_d': max(s['p90_d'] for s in steady)}
        sv = v.get('steady_' + ref)
        if sv:
            ml = sv['mean_lag']
            v['Q1'] = 'TIGHT' if ml <= 3.0 and sv['max_lag'] <= 8.0 + jig else ('TRAILING' if ml >= 8.0 else 'LOOSE')
        else:
            v['Q1'] = 'NO DATA'
        cents = [L['layout']['centroid'][0] for L in legs if L['layout'].get('centroid')]
        v['settled_centroid_along'] = cents
        pr = [L['peak_ratio_max'] for L in legs if L.get('peak_ratio_max')]
        v['peak_ratio'] = max(pr) if pr else None
        v['catch_up'] = None if not pr else ('YES' if max(pr) > 1.05 else 'NO')
        cl = [L['close_f'].get(ref) for L in legs if L.get('f_stop') is not None]
        v['close_after_stop'] = cl
        v['Q1_close'] = 'NO DATA' if not cl else ('CLOSES' if all(c is not None for c in cl) else 'STAYS OPEN')
        # Q2: the straight continuation (leg 2) shows the delays unmasked by a turn
        straight = [L for L in legs if L['leg'] == 2]
        allrel = [x for L in legs for x in L['rel_starts']]
        srel = [x for L in straight for x in L['rel_starts']]
        v['q2_all'] = allrel
        v['q2_straight'] = srel
        spreads = [L['spread'] for L in straight if L.get('spread') is not None]
        v['q2_spread'] = max(spreads) if spreads else None
        if not spreads:
            v['Q2'] = 'NO DATA'
        elif max(spreads) >= 2.5:
            v['Q2'] = 'STAGGERED' + (' (within 0..%d)' % maxd if max(spreads) <= maxd + 1.5
                                     else ' (wider than 0..%d)' % maxd)
        elif max(spreads) <= 1.0:
            v['Q2'] = 'SIMULTANEOUS'
        else:
            v['Q2'] = 'UNCLEAR (spread %.1f frames)' % max(spreads)
        hist = {}
        for x in srel:
            hist[int(round(x))] = hist.get(int(round(x)), 0) + 1
        v['q2_hist'] = sorted(hist.items())
        # Q3
        lay = [L['layout'] for L in legs if L.get('layout')]
        if key == 'rows':
            rm = [q['rows_member']['rms'] for q in lay if q.get('rows_member') and q['rows_member']['rms'] is not None]
            rc = [q['rows_container']['rms'] for q in lay if q.get('rows_container') and
                  q['rows_container']['rms'] is not None]
            v['rows_rms_member'] = median(rm)
            v['rows_rms_container'] = median(rc)
            if not rm:
                v['Q3'] = 'NO DATA'
            elif median(rm) <= 1.5 + jig and (not rc or median(rm) < median(rc)):
                v['Q3'] = 'MATCH (Rows, the member FormationSpacing: the port)'
            elif rc and median(rc) <= 1.5 + jig:
                v['Q3'] = 'DIFFERS (Rows with the container FormationSpacing)'
            else:
                v['Q3'] = 'DIFFERS (neither Rows prediction)'
        else:
            hal = [dict(q['halton'], leg=L['leg'], k=r['k']) for r in rs for L in r['legs'] if not L.get('missing')
                   for q in [L.get('layout') or {}] if q.get('halton')]
            inside = []
            for q in lay:
                if q.get('dims'):
                    d = q['dims']
                    inside.append(q['max_along'] <= d['L'] / 2 + q['jiggle'] + 0.5 and
                                  q['max_side'] <= d['Wd'] / 2 + q['jiggle'] + 0.5)
            v['dart_inside'] = inside
            v['halton'] = hal
            run_match = bool(hal) and all(h.get('match') for h in hal)
            v['Q3'] = ('MATCH (Halton layouts, run offset %+d)' % hal[0]['o'] if run_match else
                       ('INSIDE BOUNDS ONLY (no Halton match)' if inside and all(inside) else 'DIFFERS'))
            # sec. 7.2 re-lay rule seen through the layout events: leg 2 keeps leg 1's layout, leg 3 is laid out at
            # its own order, leg 4 keeps leg 3's
            ok = []
            if run_match:
                for r in rs:
                    ev = {h['leg']: h['event'] for h in hal if h['k'] == r['k']}
                    if all(i in ev for i in (1, 2, 3, 4)):
                        ok.append(ev[2] == ev[1] and ev[3] == 'leg 3 order' and ev[4] == 'leg 3 order' and
                                  ev[1] in ('spawn', 'leg 1 order'))
            v['relay_rule'] = ('NO DATA (no Halton match)' if not run_match else 'NO DATA' if not ok else
                               'MATCH (%d / %d trials)' % (sum(ok), len(ok)) if all(ok) else
                               'DIFFERS (%d / %d trials as the port)' % (sum(ok), len(ok)))
        # re-lays: leg 2 (0 deg) and leg 4 (5 deg) keep the slots; leg 3 (30 deg) re-lays
        rel = {}
        for L in legs:
            q = L.get('layout') or {}
            if q.get('set_change') is not None:
                rel.setdefault(L['leg'], []).append((q['set_change'], q['member_change']))
        v['relay'] = rel
        out[key] = v
    return out


def report(records_path, out_txt=None, out_json=None, cfg_path=None):
    cfg = None
    if cfg_path:
        with open(cfg_path) as f:
            cfg = json.load(f)
    recs = load(records_path)
    scripts = collect(recs)
    lines = []
    res = {'records': len(recs), 'scripts': [], 'trials': [], 'verdicts': {}}
    lines.append('s78n t14 land-squad formation report: %s (%d records)' % (records_path, len(recs)))
    if cfg:
        sq = cfg['sq']
        lines.append('map %s, site (%.0f, %.0f), legs %s; hold %d, settle %d frames' % (
            sq.get('map', '?'), sq['site']['x'], sq['site']['y'],
            ', '.join('%s %.0f deg %.0f' % (lg['name'], lg['deg'], lg['len']) for lg in sq['legs']), sq['hold'],
            sq['settle']))
    armed = None
    for s in scripts:
        info = {'sid': s['sid'], 'armed': s['armed'], 'skip': bool(s['skip']), 'done': s['done'],
                'errs': len(s['errs']), 'apierr': len(s['apierr']), 'records': s['n']}
        res['scripts'].append(info)
        lines.append('script %s: %s%s%s, %d records, ERR %d, APIERR %d' % (
            s['sid'], 'ARMED' if s['armed'] else 'SKIP' if s['skip'] else 'loaded',
            ', DONE' if s['done'] else '', '' if not s['skip'] else ' (%s)' % s['skip'][0].get('mode', '?'),
            s['n'], len(s['errs']), len(s['apierr'])))
        for e in s['errs'][:5] + s['apierr'][:5]:
            lines.append('  %s %s' % (e['kind'], '|'.join('%s=%s' % (k, v) for k, v in e.items()
                                                          if k not in ('kind', 'sid', 'n'))))
        if s['armed'] and armed is None:
            armed = s
    if armed is None:
        lines.append('no armed script: nothing to analyse (was the battle LAND, Empire human vs Rebel AI?)')
        return finish(lines, res, out_txt, out_json)
    xvall = (cfg or {}).get('xml', {})
    results = []
    trials = trials_of(armed['recs'], cfg)
    # pass 1: the settled slots; the run's Dartboard layout fit; pass 2 with it
    dlegs = []
    for t in trials:
        if (t.get('arr') or '').upper() != 'DARTBOARD':
            continue
        for L in analyse_trial(t, cfg)['legs']:
            if not L.get('missing'):
                dlegs.append({'key': (t['k'], L['leg']), 'slots': L.get('mslots') or L['slots'], 'cid': trial_cid(t), 'n': len(t['ids']),
                              'events': L['events']})
    dfit = dart_fit(dlegs, cfg)
    res['dart_fit'] = None if dfit is None else {k: v for k, v in dfit.items() if k != 'legs'}
    if dfit:
        lines.append('Dartboard layout fit over %d legs: frame offset (logic frame - f) %+d, mean RMS %.2f; the same '
                     'search at far offsets: best %s, median %s -> %s' % (
                         len(dfit['legs']), dfit['o'], dfit['score'], fmt(dfit['base_min'], '%.2f'),
                         fmt(dfit['base_med'], '%.2f'), 'MATCH' if dfit['match'] else 'no match'))
    for t in trials:
        r = analyse_trial(t, cfg, dfit)
        results.append(r)
        xv = xvall.get(t['key'], {})
        lines.append('')
        lines.append('=== trial %d: %s (%s, %s), container %s, %d members, %d polls; container position read in %d '
                     'polls, equal to a member position in %d ===' % (
                         r['k'], r['key'], t.get('arr', '?'), t.get('company', '?'), r.get('cid'), r['n'],
                         r.get('polls', 0), r.get('c_readable', 0), r.get('c_on_member', 0)))
        if r.get('c_on_member') and r.get('c_readable') and r['c_on_member'] > 0.5 * r['c_readable']:
            lines.append('  NOTE: the container position equals a member position in most polls: Get_Position on '
                         'the container may return a member; the lag is then relative to that member')
        if r.get('slots0'):
            lines.append('  settled before leg 1 (frame of leg 1): ' + slot_str(r['slots0']))
        for L in r['legs']:
            if L.get('missing'):
                lines.append('  leg %s: no data' % L['leg'])
                continue
            lines.append('  -- leg %d %s (turn %s deg): ordered f %d through the %s (Move_To %s)%s%s' % (
                L['leg'], L['name'], fmt(L['turn'], '%.0f'), L['f_order'], L['who'], L['ret'],
                ', REORDER through a member' if L.get('reorder') else '', ', LEG TIMEOUT' if L.get('legto') else ''))
            lines.append('     container: start +%s frames, cruise %s / frame, heading %s deg off the plan, stop f %s, '
                         'miss %s; centre = %s; members moving at the order: %d' % (
                             fmt(L['c_start_rel']), fmt(L['cruise'], '%.3f'), fmt(L['dev_deg'], '%.1f'),
                             L['f_stop'], fmt(L['miss']), L['centre'], L['moving_at_order']))
            lines.append('     Q2 member start, frames after the earliest member (%.1f-unit crossing, interpolated): %s; '
                         'spread %s; first moving poll - order: %s; earliest member - container (crossing): %s' % (
                             CROSS_D, ' '.join(fmt(x['rel'], '%.1f') for x in L['starts']), fmt(L['spread']),
                             ' '.join('-' if x['first'] is None else '%d' % (x['first'] - L['f_order'])
                                      for x in L['starts']), fmt(L.get('lead_c'))))
            lines.append('     Q1 lag behind the slot (positive = behind), mean / max over the members, side |err| mean, '
                         'max distance; vs the settled slot | vs the predicted layout:')
            for name, f, sm, pm in L['marks']:
                if sm or pm:
                    lines.append('       %-10s f %5d: %s | %s' % (name, f, mark_str(sm), mark_str(pm)))
            for rk in ('settled', 'pred'):
                st = L['steady'].get(rk)
                if st:
                    lines.append('     steady march vs %-7s (%d samples): lag mean %.1f, max %.1f; side mean %.1f; p90 '
                                 'distance %.1f' % (rk, st['samples'], st['mean_lag'], st['max_lag'], st['mean_side'],
                                                    st['p90_d']))
            lines.append('     member peak speed / container cruise: max %s, median %s; all within %.1f of the slot '
                         '(settled | predicted): while marching from container start +%s | +%s, after the stop +%s | +%s'
                         % (fmt(L['peak_ratio_max'], '%.2f'), fmt(L['peak_ratio_med'], '%.2f'),
                            RECOVER_D + (xv.get('m_jiggle') or 0.0), fmt(L['tight_f'].get('settled'), '%.0f'),
                            fmt(L['tight_f'].get('pred'), '%.0f'), fmt(L['close_f'].get('settled'), '%.0f'),
                            fmt(L['close_f'].get('pred'), '%.0f')))
            q = L.get('layout') or {}
            lines.append('     Q3 settled slots (along, side): ' + slot_str(L['slots']))
            if q.get('rows_member'):
                rm, rc = q['rows_member'], q.get('rows_container')
                lines.append('        Rows (pitch %.0f, member FormationSpacing %.2f) RMS %s; with the container\'s '
                             '%.2f (pitch %.0f) RMS %s' % (rm['pitch'], rm['spacing'], fmt(rm['rms'], '%.2f'),
                                                          rc['spacing'] if rc else 0, rc['pitch'] if rc else 0,
                                                          fmt(rc['rms'] if rc else None, '%.2f')))
            if q.get('dims'):
                d = q['dims']
                lines.append('        Dartboard: |along| <= %.1f (L/2 %.1f), |side| <= %.1f (Wd/2 %.1f), smallest gap '
                             '%s (5 + r = %.1f, jiggle %.1f)' % (q['max_along'], d['L'] / 2, q['max_side'], d['Wd'] / 2,
                                                                 fmt(q['min_gap']), 5 + d['r'], q['jiggle']))
                if q.get('halton'):
                    h = q['halton']
                    lines.append('        Halton fit (seed container id x frame, run offset %+d): RMS %.2f, laid out at %s '
                                 '%+d (frame %d)%s' % (h['o'], h['rms'], h['event'], h['df'], h['frame'],
                                                      '' if h['match'] else ' (the run fit is no match)'))
            if q.get('set_change') is not None:
                lines.append('        change from the previous settled slots: set RMS %.2f, per-member RMS %.2f' % (
                    q['set_change'], q['member_change']))
        res['trials'].append(strip(r))
    v = verdicts(results, cfg)
    res['verdicts'] = v
    lines.append('')
    lines.append('=== summary ===')
    for key, x in v.items():
        xv = xvall.get(key, {})
        lines.append('%s (%s, %d trials):' % (key, xv.get('arrangement', '?'), x['trials']))
        for rk in ('pred', 'settled'):
            sv = x.get('steady_' + rk)
            if sv:
                lines.append('  Q1 steady march vs the %s slots: lag mean %s, max %s, side mean %s, p90 distance %s%s' % (
                    'predicted' if rk == 'pred' else 'settled', fmt(sv['mean_lag']), fmt(sv['max_lag']),
                    fmt(sv['mean_side']), fmt(sv['p90_d']), '  <- used' if rk == x['q1_ref'] else ''))
        lines.append('  Q1 -> %s (port: a member never makes up a start / turn lag; FoC fixture ~30 behind its slot)'
                     % x['Q1'])
        lines.append('  Q1 settled centroid along the heading, relative to the container (Rows predicts 0): %s' % (
            ' '.join(fmt(c) for c in x['settled_centroid_along'])))
        lines.append('  Q1 catch-up: member peak speed / container cruise %s -> members faster than the team while '
                     'behind: %s' % (fmt(x.get('peak_ratio'), '%.2f'), x.get('catch_up') or '-'))
        lines.append('  Q1 after the stop: frames to all within the recovery distance: %s -> %s' % (
            ' '.join(fmt(c, '%.0f') for c in x['close_after_stop']), x['Q1_close']))
        lines.append('  Q2 start offsets on the straight leg 2 (frames after the earliest member): %s; largest spread %s '
                     '(Max_Move_Frame_Delay %s) -> %s' % (' '.join('%.1f' % s for s in x['q2_straight']),
                                                          fmt(x.get('q2_spread')), maxd_s(cfg), x['Q2']))
        lines.append('  Q2 histogram (rounded frames: members): %s' % ' '.join('%d:%d' % kv for kv in x['q2_hist']))
        lines.append('  Q2 all legs: min %s max %s' % (fmt(min(x['q2_all']) if x['q2_all'] else None),
                                                        fmt(max(x['q2_all']) if x['q2_all'] else None)))
        for leg in sorted(x['relay']):
            vals = x['relay'][leg]
            lines.append('  Q3 leg %d slot change (set / per member RMS): %s' % (
                leg, ', '.join('%.2f / %.2f' % a for a in vals)))
        if key == 'rows':
            lines.append('  Q3 Rows RMS: member spacing %s, container spacing %s -> %s' % (
                fmt(x.get('rows_rms_member'), '%.2f'), fmt(x.get('rows_rms_container'), '%.2f'), x['Q3']))
        else:
            lines.append('  Q3 Dartboard: inside L/2 x Wd/2 %s -> %s' % (x.get('dart_inside'), x['Q3']))
            for h in x.get('halton', []):
                lines.append('     trial %d leg %d: Halton RMS %.2f, laid out at %s %+d%s' % (
                    h['k'], h['leg'], h['rms'], h['event'], h['df'], '' if h.get('match') else ' (run fit: no match)'))
            lines.append('  Q3 Dartboard re-lay rule (leg 2 = leg 1 layout, legs 3 and 4 = leg 3 order): %s' % x['relay_rule'])
            lines.append('     (the layout event of each leg: port / RE: leg 1 at the spawn when the first heading is within '
                         '8.11 deg of the spawn facing, else at the order of leg 1; leg 2 = as leg 1; leg 3 at its own order; '
                         'leg 4 = as leg 3)')
    lines.append('expected (port, FIGHTER_FLYING 7.2): leg 2 (0 deg) and leg 4 (5 deg) keep the slots (small change, '
                 'jiggle only), leg 3 (30 deg) re-lays (Dartboard: a new scatter, set change well above the jiggle; '
                 'Rows: the same grid, members may swap)')
    return finish(lines, res, out_txt, out_json)


def slot_str(slots):
    return ' '.join('(%s, %s)' % (fmt(s[0]), fmt(s[1])) if s and s[0] is not None else '(-)' for s in slots)


def maxd_s(cfg):
    return fmt((cfg or {}).get('xml', {}).get('max_move_frame_delay'), '%.0f')


def mark_str(sm):
    if not sm:
        return '%-44s' % '-'
    return 'lag %6.1f / %6.1f  side %5.1f  max %6.1f' % (sm['mean_lag'], sm['max_lag'], sm['mean_side'], sm['max_d'])


def strip(r):
    out = dict(r)
    out['legs'] = [dict(L) for L in r['legs']]
    return out


def finish(lines, res, out_txt, out_json):
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
    text, _ = report(a.records, a.out, a.json, a.config)
    print(text)


if __name__ == '__main__':
    main()
