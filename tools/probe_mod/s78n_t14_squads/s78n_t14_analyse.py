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
    5 + r, and the sec. 7.2 Halton fit in float32 (seed = (int32) container id x NOW, NOW in [g, g + 10] of the
    spawn or an order; the retail slot order; baseline = the 5 % quantile of 200 far NOW values); the re-lay rule
    (8.11 deg) from the measured headings; between legs the change of the slot set and of each member's slot;
  * FIGHTER_FLYING 7.7: the container's start / braking ramps against member OverrideDeceleration x 1.56, and the
    straggler cap 0x5ab640 solved for the layout box HX at each steady poll against the predicted HX.
f = GetCurrentTime() x 30 (mode frame), g = GetCurrentTime.Frame() x 30 (NOW, records from s78n t17 on), read at
the free-store poll (every 3 frames), so a start or stop is known to +-3 frames without the fit. Records without g
analyse as before; the NOW checks then print n/a.
"""
import argparse
import json
import math
import re
import struct
import sys

sys.dont_write_bytecode = True
REC_RE = re.compile(r'S8Q\|([A-Z0-9_]+)\|(.*)\|END')
SETTLE_WIN = 60      # frames before the next order whose polls give a member's settled slot
MOVE_EPS = 0.3       # a member / the container has started when it is this far from its position at the order
RECOVER_D = 4.0      # sec. 7.3 item 6: recovery threshold
CROSS_D = 1.5        # Q2: a member's start is compared through the frame it is this far from its position at the order
NOW_WIN = 10         # Dartboard fit: NOW searched in [event NOW, event NOW + NOW_WIN] (the container's next service)
NOW_BASE_OFF = 5000  # Dartboard fit: the baseline NOW values start this far after the leg's own order ...
NOW_BASE_N = 200     # ... and are this many (every 7th frame)
BASE_Q = 0.05        # the baseline quantile compared with: Halton scatters are quasi-periodic in the index, so the
                     # very best of many far seeds can repeat a pattern; a low quantile is stable
HALTON_MATCH = 0.5   # a leg's fit is a match when its score is below this x the baseline quantile ...
ABS_MATCH = 0.75     # ... and below this (score units: side error, along error x ALONG_W)
SHIFT_MAX = 2.0      # Dartboard fit: a common along shift of the settled slots allowed (the +-1 along dead band)
ALONG_W = 0.15       # Dartboard fit: weight of the along error in the score; a member settles anywhere in the +-1
                     # along dead band (and a Rebel member keeps a lag), while the side error converges on the march
RELAY_DEG = 8.11     # FIGHTER_FLYING 7.2: re-lay when the heading turns by more than acos(0.99)
RELAY_BORDER = 1.5   # headings this close to RELAY_DEG accept either outcome
HX_TOL = 1.0         # straggler cap: measured HX within this of the prediction is a match
ACC_TOL = 0.1        # container acceleration: within this fraction of the prediction is a match
ACC_MIN_POLLS = 4    # a ramp fitted through fewer polls (3: an exact parabola, no check) is shown but not judged


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
    try:
        d['g'] = int(d['g'])  # NOW (GetCurrentTime.Frame() x 30); absent in records before s78n t17
    except (KeyError, ValueError):
        d['g'] = None
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


def heading_deg(u):
    return None if u is None else math.degrees(math.atan2(u[1], u[0])) % 360.0


def angle_deg(a, b):
    c = max(-1.0, min(1.0, a[0] * b[0] + a[1] * b[1]))
    s = a[0] * b[1] - a[1] * b[0]
    return math.degrees(math.atan2(s, c))


def assign(a, b, wa=1.0):
    """Min-cost (sum of squared distances, the first coordinate weighted by wa) matching of point lists a and b (same
    length <= 12, bitmask DP). Returns (rms, pairs [(i, j)])."""
    n = len(a)
    if n == 0 or n != len(b):
        return None, []
    cost = [[(wa * (a[i][0] - b[j][0])) ** 2 + (a[i][1] - b[j][1]) ** 2 for j in range(n)] for i in range(n)]
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


def f32(x):
    """x rounded to an IEEE float32 (one float operation computed in double and rounded is the float result: double
    rounding is innocuous for + - * / sqrt from float operands)."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def radical_inverse(i, b):
    """0x241820 / 0x241830 (FIGHTER_FLYING 7.2 step 3): the base-b radical inverse of the uint32 index i, accumulated
    in float: f = 0, b_k = (float)b; while i != 0 { f += (float)(i % b) / b_k; i /= b; b_k *= (float)b }."""
    i &= 0xFFFFFFFF
    fb = f32(float(b))
    f, bk = 0.0, fb
    while i:
        f = f32(f + f32(float(i % b) / bk))
        i //= b
        bk = f32(bk * fb)
    return f


def dart_dims(n, hx, hy, spacing, sp=1.0):
    """FIGHTER_FLYING 7.2 Dartboard steps 1-2 in float: k = ceil(sqrt n), m = ceil(n / k), L = (k - 1) g 20 + k hx,
    Wd = (m - 1) g 20 + m hy (half-extents), r = sqrtf(hx^2 + hy^2); SPREAD_OUT (sp = 1.8): L, Wd and r x sp."""
    k = ceil_sqrt(n)
    m = n // k + (1 if n % k else 0)
    hx, hy, g = f32(hx), f32(hy), f32(spacing)
    L = f32(f32(f32(float(k - 1) * g) * 20.0) + f32(float(k) * hx))
    Wd = f32(f32(f32(float(m - 1) * g) * 20.0) + f32(float(m) * hy))
    r = f32(math.sqrt(f32(f32(hx * hx) + f32(hy * hy))))
    if sp != 1.0:
        L, Wd, r = f32(L * sp), f32(Wd * sp), f32(r * sp)
    return {'k': k, 'm': m, 'L': L, 'Wd': Wd, 'r': r}


def dartboard_points(n, hx, hy, spacing, seed, sp=1.0):
    """FIGHTER_FLYING 7.2 Dartboard 0x5ac2a0 steps 3-5: both generators start at the uint32 index seed ((int32)
    container id x NOW, wrapping); up to 1000 tries, both draws every try; x = (h2 - 0.5) L, y = (h3 - 0.5) Wd;
    rejected when sqrtf(dy^2 + dx^2) < 5.0 + r to an accepted point. The accepted points [(x along, y side)] in
    acceptance order, or None (fewer than n: retail lays Rows instead)."""
    if n <= 0:
        return None
    d = dart_dims(n, hx, hy, spacing, sp)
    L, Wd = d['L'], d['Wd']
    lim = f32(5.0 + d['r'])
    i2 = i3 = seed & 0xFFFFFFFF
    pts = []
    for _ in range(1000):
        if len(pts) >= n:
            break
        h2 = radical_inverse(i2, 2)
        i2 = (i2 + 1) & 0xFFFFFFFF
        h3 = radical_inverse(i3, 3)
        i3 = (i3 + 1) & 0xFFFFFFFF
        x = f32(f32(h2 - 0.5) * L)
        y = f32(f32(h3 - 0.5) * Wd)
        ok = True
        for px, py in pts:
            dy, dx = f32(y - py), f32(x - px)
            if f32(math.sqrt(f32(f32(dy * dy) + f32(dx * dx)))) < lim:
                ok = False
                break
        if ok:
            pts.append((x, y))
    return pts if len(pts) >= n else None


def retail_assign(pts, members, kpos, u, Wd, m):
    """FIGHTER_FLYING 7.2 Dartboard step 6: which member gets which point. pts in acceptance order; members = their
    positions at layout time (index order); kpos = the container's; u = the layout direction. Points sorted by x
    descending, members by projection onto u descending (stable: ties keep their order, as MSVC's insertion sort for
    <= 32 elements); groups i = 0; repeat { j = i; while j < n and x[i] - x[j] <= Wd / m: j += 1; sort members [i, j)
    by projection onto (-u.y, u.x) descending and points [i, j) by y descending; i = j + 1 } while i < n (index j
    skipped: a retail quirk). Returns [point per member] or None."""
    n = len(pts)
    if n == 0 or len(members) != n or any(p is None for p in members) or kpos is None or u is None:
        return None

    def proj(q, v):
        return (members[q][0] - kpos[0]) * v[0] + (members[q][1] - kpos[1]) * v[1]

    P = sorted(range(n), key=lambda q: -pts[q][0])
    M = sorted(range(n), key=lambda q: -proj(q, u))
    lim = f32(Wd / float(m))
    side = (-u[1], u[0])
    i = 0
    while True:
        j = i
        while j < n and f32(pts[P[i]][0] - pts[P[j]][0]) <= lim:
            j += 1
        M[i:j] = sorted(M[i:j], key=lambda q: -proj(q, side))
        P[i:j] = sorted(P[i:j], key=lambda q: -pts[q][1])
        i = j + 1
        if not i < n:
            break
    out = [None] * n
    for q in range(n):
        out[M[q]] = pts[P[q]]
    return out


def joint_hx(alongs, hx, c_jiggle=0.0):
    """FIGHTER_FLYING 7.7 item 2: HX = (max - min) / 2 of the members' slot x +- their half-extent hx (box seeded
    with 0) + the container type's MaxJiggleDistance."""
    if not alongs:
        return None
    hi = max(0.0, max(a + hx for a in alongs))
    lo = min(0.0, min(a - hx for a in alongs))
    return (hi - lo) / 2.0 + c_jiggle


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


def fit_rms(slots, pts):
    """Min-cost match of the settled slots to the points, allowing one common along shift (clamped to +-SHIFT_MAX: the
    +-1 along dead band of FIGHTER_FLYING 7 step 4). The along error counts ALONG_W (a member settles anywhere in the
    dead band; the side error converges during the march). Returns (score, shift, pairs [(slot i, point j)])."""
    rms, pairs = assign(slots, pts, ALONG_W)
    shift = mean([slots[i][0] - pts[j][0] for i, j in pairs])
    shift = max(-SHIFT_MAX, min(SHIFT_MAX, shift))
    rms2, pairs2 = assign([(a - shift, s) for a, s in slots], pts, ALONG_W)
    if rms2 <= rms:
        return rms2, shift, pairs2
    return rms, 0.0, pairs


def plain_rms(slots, refs, shift=0.0):
    """Unweighted RMS distance of slots[j] (minus the along shift) to refs[j]."""
    return math.sqrt(mean([(s[0] - shift - r[0]) ** 2 + (s[1] - r[1]) ** 2 for s, r in zip(slots, refs)]))


def dart_fit_now(legs, cfg):
    """The Dartboard layouts, seeded the retail way (FIGHTER_FLYING 7.2, S78N_T14_RUN.md "rerun"): seed = (int32)
    container id x NOW, NOW in [event NOW, event NOW + NOW_WIN] for each event that may have laid the leg's slots (the
    spawn: the container's first service; each order up to this leg: the next state-0 service after the path), Halton
    in float32. legs: [{'key': (trial, leg), 'slots': settled slots (container frame at the stop), 'cid', 'n',
    'events': [(label, NOW or None, positions at the event or None, layout direction or None)]}].
    Per leg: the best score (fit_rms) over the candidates; the baseline is the BASE_Q quantile of NOW_BASE_N NOW
    values from NOW_BASE_OFF after the leg's own order; a match when best <= min(ABS_MATCH, HALTON_MATCH x baseline)
    (S78N_T14_RUN.md: with the shipped jiggle no seed can stand out). For the match, the retail slot order
    (retail_assign) from the members' positions at that event is checked against the settled slots too.
    Returns {'legs': {key: result}, 'n_fit', 'n_match', 'n_nonow'}."""
    xv = (cfg or {}).get('xml', {}).get('dart', {})
    hx, hy = xv.get('hx') or 3.0, xv.get('hy') or 3.0
    g = xv.get('m_spacing') or 0.5
    cache = {}
    out = {'legs': {}, 'n_fit': 0, 'n_match': 0, 'n_nonow': 0}

    def points(cid, n, now):
        key = (cid, n, now)
        if key not in cache:
            cache[key] = dartboard_points(n, hx, hy, g, (cid * now) & 0xFFFFFFFF)
        return cache[key]

    for L in legs:
        n = L['n']
        slots = L['slots']
        res = {'status': None}
        out['legs'][L['key']] = res
        if L['cid'] is None or n < 2 or slots is None or len(clean(slots)) != n:
            res['status'] = 'no settled slots'
            continue
        evs = [e for e in L['events'] if e[1] is not None]
        if not evs:
            res['status'] = 'n/a (records without NOW)'
            out['n_nonow'] += 1
            continue
        dims = dart_dims(n, hx, hy, g)
        best, cands = None, 0
        for label, g0, pos, u in evs:
            for d in range(0, NOW_WIN + 1):
                now = g0 + d
                pts = points(L['cid'], n, now)
                if not pts:
                    continue
                cands += 1
                rms, shift, pairs = fit_rms(slots, pts)
                if best is None or rms < best['rms']:
                    best = {'rms': rms, 'shift': shift, 'event': label, 'dnow': d, 'now': now, 'pts': pts,
                            'pairs': pairs, 'pos': pos, 'u': u}
        g_ref = evs[-1][1]
        base = []
        for i in range(NOW_BASE_N):
            pts = points(L['cid'], n, g_ref + NOW_BASE_OFF + 7 * i)
            if pts:
                base.append(fit_rms(slots, pts)[0])
        if best is None or not base:
            res['status'] = 'no scatter'
            continue
        out['n_fit'] += 1
        base.sort()
        bmin = base[int(BASE_Q * (len(base) - 1))]
        match = best['rms'] <= min(ABS_MATCH, HALTON_MATCH * bmin)
        res.update({k: v for k, v in best.items() if k not in ('pos', 'u')})
        mp = [None] * n
        for i, j in best['pairs']:
            mp[i] = best['pts'][j]
        res.update({'status': 'fit', 'cands': cands, 'base_min': bmin, 'base_med': median(base), 'match': match,
                    'Wd': dims['Wd'], 'L': dims['L'], 'rms_plain': plain_rms(slots, mp, best['shift'])})
        if match:
            out['n_match'] += 1
        # the retail slot order from the members' positions at that event (orders only: the spawn layout runs on the
        # drop positions, which no poll sees): the same score with the retail member <-> point pairs
        pos, u = best['pos'], best['u']
        res['retail'] = None
        if pos is not None and u is not None:
            ass = retail_assign(best['pts'], pos['m'], pos['c'], u, dims['Wd'], dims['m'])
            if ass is not None:
                sh = best['shift']
                res['retail'] = math.sqrt(mean([(ALONG_W * (slots[j][0] - sh - ass[j][0])) ** 2 +
                                                (slots[j][1] - ass[j][1]) ** 2 for j in range(n)]))
                res['retail_plain'] = plain_rms(slots, ass, sh)
                res['retail_pts'] = ass
    return out


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
            info['halton'] = {k: v for k, v in h.items() if k not in ('pts', 'pairs', 'retail_pts')}
            if h.get('match'):
                # the min-cost member <-> point pairs (independent of the retail slot order, which is checked apart)
                info['halton_pts'] = h['pts']
                ref = [None] * n
                for i, j in h['pairs']:
                    ref[i] = h['pts'][j]
    return ref, info


def speed_expect(cfg, key):
    """(team speed, container acceleration) per frame that FIGHTER_FLYING 7.7 / MOVEMENT 3.4 predict from the config
    (older configs: computed from Max_Speed, OverrideDeceleration or else OverrideAcceleration, x 1.56)."""
    xa = (cfg or {}).get('xml', {})
    xv = xa.get(key, {})
    mult = xa.get('speed_mult') or 1.56
    vt = xv.get('team_speed')
    if vt is None and xv.get('m_speed'):
        vt = min(xv.get('c_speed') or xv['m_speed'], xv['m_speed']) * mult
    a = xv.get('c_accel')
    if a is None and (xv.get('m_dec') or xv.get('m_acc')):
        a = (xv.get('m_dec') or xv.get('m_acc')) * mult
    return vt, a


def container_speed(win, centre, cs, f_stop, c_end, cruise, vteam, a_exp, n):
    """FIGHTER_FLYING 7.7 checks of one leg from the container's track (polls every 3 frames, plus the odd
    On_Unit_Service poll in between).
    accel / brake: the start ramp and the braking ramp, each a parabola fitted to the distance walked at the polls
    strictly inside the ramp (quad_rate). hx_samples: the straggler cap 0x5ab640 solved for HX at
    each poll pair where the container runs steadily (speed change below half the expected acceleration), below
    0.95 x the team speed, not braking for the stop, with its rearmost member p behind it along its facing (the pair's
    direction of travel): speed v = (1 - t) vteam, t = ((-HX - 2) - p) / 10, so HX = -p - 2 - 10 (1 - v / vteam)."""
    out = {'accel': None, 'brake': None, 'accel_n': 0, 'brake_n': 0, 'hx_samples': [], 'team_speed': vteam,
           'a_exp': a_exp}
    tr = [(p['f'], centre(p), p) for p in win]
    tr = [x for x in tr if x[1] is not None]
    seg = [(fa, fb, norm(sub(cb, ca)) / (fb - fa), pa, pb, ca, cb)
           for (fa, ca, pa), (fb, cb, pb) in zip(tr, tr[1:]) if fb > fa]
    if len(seg) < 3 or not cruise:
        return out
    v = [s[2] for s in seg]
    fm = [0.5 * (s[0] + s[1]) for s in seg]
    a_ref = a_exp or 0.05
    # the distance walked at each poll (seg k runs from poll k to poll k + 1)
    fp = [seg[0][0]] + [s[1] for s in seg]
    dist = [0.0]
    for s in seg:
        dist.append(dist[-1] + s[2] * (s[1] - s[0]))

    def step(k, sign):  # window k rises (sign 1) / falls (-1) from window k - 1 by more than 0.3 a per frame
        return sign * (v[k] - v[k - 1]) > 0.3 * a_ref * (fm[k] - fm[k - 1])

    # start ramp: the rising run of windows from the first moving one; braking: the falling run after the last window
    # at >= 0.9 x cruise. Their first and last windows hold the start / the cruise / the stop / the plateau at v = a
    # (the braking target, 7.7 item 3), so only the polls between them are fitted
    k0 = next((k for k, x in enumerate(v) if x > 0.02), None)
    if k0 is not None:
        k1 = k0
        while k1 + 1 < len(v) and step(k1 + 1, 1):
            k1 += 1
        out['accel'], out['accel_n'] = quad_rate(fp, dist, k0 + 1, k1, 1)
    kc = max((k for k, x in enumerate(v) if x >= 0.9 * cruise), default=None)
    if kc is not None:
        kb = kc
        while kb + 1 < len(v) and step(kb + 1, -1):
            kb += 1
        out['brake'], out['brake_n'] = quad_rate(fp, dist, kc + 2, kb, -1)
    if not vteam:
        return out
    for i in range(1, len(seg) - 1):
        fa, fb, vv, pa, pb, ca, cb = seg[i]
        if (cs is not None and fa < cs) or (f_stop is not None and fb > f_stop):
            continue
        if not 0.05 * vteam < vv < 0.95 * vteam:
            continue
        if abs(vv - v[i - 1]) / (fm[i] - fm[i - 1]) > 0.5 * a_ref or abs(v[i + 1] - vv) / (fm[i + 1] - fm[i]) > 0.5 * a_ref:
            continue
        if c_end is not None and norm(sub(c_end, cb)) <= vv * vv / (2.0 * a_ref) + 10.0:
            continue
        fdir = unit(sub(cb, ca))
        if fdir is None:
            continue
        rears = []
        for p, c in ((pa, ca), (pb, cb)):
            ps = [fdir[0] * (m[0] - c[0]) + fdir[1] * (m[1] - c[1]) for m in p['m'] if m is not None]
            if len(ps) == n:
                rears.append(min(ps))
        if len(rears) != 2:
            continue
        rear = 0.5 * (rears[0] + rears[1])
        hx = -rear - 2.0 - 10.0 * (1.0 - vv / vteam)
        if rear < -2.0 and hx > 0.0:
            out['hx_samples'].append(hx)
    return out


def quad_rate(fp, dist, i0, i1, sign):
    """The constant acceleration of the distance walked at polls i0..i1 (inclusive): a least-squares parabola
    dist = c0 + c1 t + c2 t^2, a = 2 c2 x sign. While the fit is poor (residual RMS > 0.02, e.g. the straggler cap
    bending the ramp) the poll with the larger residual of the two ends is dropped. Returns (a or None, polls used)."""
    pts = [(fp[i], dist[i]) for i in range(max(0, i0), min(i1, len(fp) - 1) + 1)]
    while len(pts) >= 3:
        tm = mean([f for f, _ in pts])
        rows = [(1.0, f - tm, (f - tm) ** 2) for f, _ in pts]
        A = [[sum(r[i] * r[j] for r in rows) for j in range(3)] for i in range(3)]
        b = [sum(r[i] * d for r, (_, d) in zip(rows, pts)) for i in range(3)]
        c = solve3(A, b)
        if c is None:
            return None, len(pts)
        res = [d - (c[0] + c[1] * r[1] + c[2] * r[2]) for r, (_, d) in zip(rows, pts)]
        rms = math.sqrt(mean([x * x for x in res]))
        if rms <= 0.02 or len(pts) == 3:
            return 2.0 * c[2] * sign, len(pts)
        if abs(res[0]) > abs(res[-1]):
            pts = pts[1:]
        else:
            pts = pts[:-1]
    return None, len(pts)


def solve3(A, b):
    """Gaussian elimination with partial pivoting for a 3 x 3 system; None when singular."""
    m = [list(A[i]) + [b[i]] for i in range(3)]
    for c in range(3):
        p = max(range(c, 3), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            return None
        m[c], m[p] = m[p], m[c]
        for r in range(3):
            if r != c:
                k = m[r][c] / m[c][c]
                m[r] = [x - k * y for x, y in zip(m[r], m[c])]
    return [m[i][3] / m[i][i] for i in range(3)]


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
    order_pos, u_starts = {}, {}
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
        # the path heading at the start (the first 30 units: the layout direction of a re-lay at the order, 7.2
        # caller 1) and at the stop (the last 30 units: the container's facing, the frame of its settled slots)
        u_start = u_end = None
        for f, c in ctrack:
            if c is not None and c0 is not None and norm(sub(c, c0)) >= 30.0:
                u_start = unit(sub(c, c0))
                break
        if c_end is not None:
            for f, c in reversed(ctrack):
                if c is not None and (f_stop is None or f <= f_stop) and norm(sub(c_end, c)) >= 30.0:
                    u_end = unit(sub(c_end, c))
                    break
        u_start, u_end = u_start or u, u_end or u
        fslots = []
        for j in range(n):
            offs = []
            for p in swin:
                c = centre(p)
                if c is not None and j < len(p['m']) and p['m'][j] is not None:
                    offs.append(frame_of(u_end, sub(p['m'][j], c)))
            fslots.append((mean([o[0] for o in offs]), mean([o[1] for o in offs])) if offs else None)
        order_pos[lg['leg']] = {'c': c0, 'm': list(p0['m'])}
        u_starts[lg['leg']] = u_start
        events = [('spawn', t['spawn_f'])] if t.get('spawn_f') is not None else []
        events += [('leg %d order' % x['leg'], x['f']) for x in legs[:li + 1]]
        events_now = [('spawn', t.get('spawn_g'), None, None)] if t.get('spawn_f') is not None else []
        events_now += [('leg %d order' % x['leg'], x.get('g'), order_pos.get(x['leg']), u_starts.get(x['leg']))
                       for x in legs[:li + 1]]
        vteam, a_exp = speed_expect(cfg, t['key'])
        speed = container_speed(win, centre, cs, f_stop, c_end, cruise, vteam, a_exp, n)
        speed['accel_est'], speed['brake_est'] = speed['accel'], speed['brake']
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
        # FIGHTER_FLYING 7.7 item 2: the HX the straggler cap should use for this leg's layout
        hxm, cj = xv.get('hx') or 3.0, xv.get('c_jiggle') or 0.0
        if info['arr'] == 'ROWS' and xv.get('m_spacing') is not None:
            speed['hx_pred'] = joint_hx([a for a, _ in rows_slots(n, hxm, xv.get('hy') or 3.0, xv['m_spacing'])],
                                        hxm, cj)
            speed['hx_src'] = 'Rows grid'
        elif info.get('halton_pts'):
            speed['hx_pred'] = joint_hx([a for a, _ in info['halton_pts']], hxm, cj)
            speed['hx_src'] = 'fitted Halton layout'
        elif len(clean(fslots)) == n:
            speed['hx_pred'] = joint_hx([a for a, _ in clean(fslots)], hxm, cj)
            speed['hx_src'] = 'settled slots'
        else:
            speed['hx_pred'], speed['hx_src'] = None, '-'
        hs = speed['hx_samples']
        speed['hx_med'] = median(hs)
        speed['hx_match'] = None if not hs or speed['hx_pred'] is None else \
            abs(speed['hx_med'] - speed['hx_pred']) <= HX_TOL
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
            'layout': info, 'events': events, 'mslots': mslots, 'u_start': u_start, 'u_end': u_end,
            'bend': angle_deg(u_start, u_end) if u_start and u_end else None, 'fslots': fslots,
            'events_now': events_now, 'g_order': lg.get('g'), 'speed': speed})
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
            t['spawn_g'] = r['g']
        elif kind == 'TEAM':
            t['cid'] = r.get('cid')
            t['ids'] = [x for x in r.get('ids', '').split(';') if x]
            t['team'] = r
        elif kind == 'P' and r['f'] is not None:
            ms = [xy(x) for x in r.get('m', '').split(';')]
            if ms and ms[-1] is None and r.get('m', '').endswith(';'):
                ms = ms[:-1]
            t['polls'].append({'f': r['f'], 'g': r['g'], 'n': r['n'], 'leg': int(r.get('leg', '0')), 'ph': r.get('ph'),
                               'c': xy(r.get('c')), 'm': ms})
        elif kind == 'LEG' and r.get('via') in (None, 'nil'):
            i = int(r['leg'])
            lc = legs_cfg[i - 1] if 0 < i <= len(legs_cfg) else {}
            t['legs'].append({'leg': i, 'f': r['f'], 'g': r['g'], 'name': r.get('name'),
                              'turn': float(r.get('turn', '0') or 0),
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


def relay_rule(rs):
    """FIGHTER_FLYING 7.2 re-lay rule through the fitted layout events of the Dartboard legs: leg 1 is laid at the
    spawn or at its order (the spawn facing is not recorded); a later leg is laid at its own order when its path heading
    at the start turns more than RELAY_DEG from the last laid direction, else it keeps the previous leg's layout
    (within RELAY_BORDER of RELAY_DEG either is accepted). A bend later in a leg (7.2 caller 3) is not modelled: see
    the leg's bend angle. Returns (verdict, [(k, leg, angle, expected, got)])."""
    detail, ok = [], []
    for r in rs:
        laid, prev = None, None
        for L in r['legs']:
            if L.get('missing'):
                break
            h = (L.get('layout') or {}).get('halton') or {}
            if h.get('status') != 'fit' or not h.get('match'):
                break
            leg, got, us = L['leg'], h['event'], L.get('u_start')
            own = 'leg %d order' % leg
            if leg == 1 or laid is None or us is None:
                ang, exp = None, {'spawn', own} if leg == 1 else {own}
            else:
                ang = abs(angle_deg(laid, us))
                if ang > RELAY_DEG + RELAY_BORDER:
                    exp = {own}
                elif ang < RELAY_DEG - RELAY_BORDER:
                    exp = {prev}
                else:
                    exp = {prev, own}
            if got == own or laid is None:
                laid = us
            prev = got
            ok.append(got in exp)
            detail.append((r['k'], leg, ang, ' or '.join(sorted(e for e in exp if e)), got))
    if not ok:
        return 'NO DATA (no matched Halton leg)', detail
    return ('MATCH' if all(ok) else 'DIFFERS') + ' (%d / %d legs)' % (sum(ok), len(ok)), detail


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
        # after the stop: only legs that have the reference slots (a leg without settled slots is skipped, not counted
        # as never closing; S78N_T14_RUN.md reading 8)
        def has_ref(L):
            s = L['slots'] if ref == 'settled' else L.get('pred_slots')
            return s is not None and len(clean(s)) == len(s) and len(s) > 0
        cl = [L['close_f'].get(ref) for L in legs if L.get('f_stop') is not None and has_ref(L)]
        v['close_after_stop'] = cl
        v['close_skipped'] = sum(1 for L in legs if L.get('f_stop') is not None and not has_ref(L))
        v['Q1_close'] = 'NO DATA' if not cl else ('CLOSES' if all(c is not None for c in cl) else 'STAYS OPEN')
        # FIGHTER_FLYING 7.7 item 1: container acceleration = braking; item 2: the straggler cap's HX
        vteam, a_exp = speed_expect(cfg, key)
        acc = [(L['speed']['accel_est'], L['speed']['accel_n']) for L in legs
               if L['speed'].get('accel_est') is not None]
        brk = [(L['speed']['brake_est'], L['speed']['brake_n']) for L in legs
               if L['speed'].get('brake_est') is not None]
        v['accel'] = {'start': [x for x, _ in acc], 'brake': [x for x, _ in brk], 'expected': a_exp,
                      'team_speed': vteam, 'start_n': [n for _, n in acc], 'brake_n': [n for _, n in brk]}
        both = [x for x, n in acc + brk if n >= ACC_MIN_POLLS]
        n_thin = sum(1 for _, n in acc + brk if n < ACC_MIN_POLLS)
        if not both:
            v['Q77_accel'] = 'NO DATA' + (' (only ramps fitted through < %d polls)' % ACC_MIN_POLLS if n_thin else '')
        elif a_exp is None:
            v['Q77_accel'] = 'n/a (no expected value in the config)'
        else:
            ok = [abs(x - a_exp) <= ACC_TOL * a_exp for x in both]
            v['Q77_accel'] = ('MATCH' if all(ok) else 'DIFFERS') + ' (%d / %d ramps within %d %% of %.4f%s)' % (
                sum(ok), len(ok), int(ACC_TOL * 100), a_exp,
                '; %d ramp(s) fitted through < %d polls not judged' % (n_thin, ACC_MIN_POLLS) if n_thin else '')
        hxl = [(r['k'], L['leg'], L['speed']) for r in rs for L in r['legs'] if not L.get('missing')
               and L['speed'].get('hx_samples')]
        v['hx'] = [{'k': k, 'leg': leg, 'n': len(s['hx_samples']), 'med': s['hx_med'], 'lo': min(s['hx_samples']),
                    'hi': max(s['hx_samples']), 'pred': s.get('hx_pred'), 'src': s.get('hx_src'),
                    'match': s.get('hx_match')} for k, leg, s in hxl]
        hm = [h['match'] for h in v['hx'] if h['match'] is not None]
        v['Q77_straggler'] = 'NO DATA (no poll with the cap binding)' if not hm else \
            ('MATCH' if all(hm) else 'DIFFERS') + ' (%d / %d legs within %.1f of the predicted HX)' % (
                sum(hm), len(hm), HX_TOL)
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
                    # slots lie inside +-L/2 x +-Wd/2 by construction; the settled positions add the jiggle and the
                    # +-1 along dead band
                    inside.append(q['max_along'] <= d['L'] / 2 + q['jiggle'] + 1.5 and
                                  q['max_side'] <= d['Wd'] / 2 + q['jiggle'] + 1.5)
            v['dart_inside'] = inside
            v['halton'] = hal
            fits = [h for h in hal if h.get('status') == 'fit']
            nonow = [h for h in hal if str(h.get('status', '')).startswith('n/a')]
            nm = sum(1 for h in fits if h.get('match'))
            bounds = 'inside L/2 x Wd/2' if inside and all(inside) else 'outside L/2 x Wd/2'
            if not fits:
                v['Q3'] = ('n/a: records without NOW (before s78n t17); %s' % bounds if nonow else
                           'NO DATA (%s)' % bounds)
            elif nm == len(fits):
                v['Q3'] = 'MATCH (Halton seeded container id x NOW, %d / %d legs)' % (nm, len(fits))
            elif nm:
                v['Q3'] = 'PARTIAL (Halton seeded container id x NOW, %d / %d legs)' % (nm, len(fits))
            else:
                v['Q3'] = 'NO MATCH (0 / %d legs; %s)' % (len(fits), bounds)
            # the retail slot order (7.2 step 6) on the matched legs laid at an order
            ro = [h for h in fits if h.get('match') and h.get('retail') is not None]
            v['retail_order'] = [(h['k'], h['leg'], h['retail'], h['rms']) for h in ro]
            rok = [h['retail'] <= h['rms'] + 0.5 for h in ro]
            v['Q3_order'] = 'NO DATA (no matched leg laid at an order)' if not ro else \
                ('MATCH' if all(rok) else 'DIFFERS') + ' (%d / %d legs: retail order score within 0.5 of the best ' \
                'match)' % (sum(rok), len(rok))
            v['relay_rule'], v['relay_detail'] = relay_rule(rs)
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
    # the clocks: g = NOW (GetCurrentTime.Frame() x 30, s78n t17), f = the mode frame
    gf = [r['g'] - r['f'] for r in armed['recs'] if r.get('g') is not None and r.get('f') is not None]
    res['clock'] = {'records_with_g': len(gf), 'g_minus_f_min': min(gf) if gf else None,
                    'g_minus_f_max': max(gf) if gf else None}
    if gf:
        lines.append('clock: %d records carry NOW (g = GetCurrentTime.Frame() x 30); g - f (mode frame) from %+d to %+d'
                     % (len(gf), min(gf), max(gf)))
    else:
        lines.append('clock: no record carries NOW (g): records from before s78n t17, or GetCurrentTime.Frame failed '
                     '(see APIERR); the Dartboard NOW fit prints n/a')
    # pass 1: the settled slots; the Dartboard layout fit (seed container id x NOW); pass 2 with it
    dlegs = []
    for t in trials:
        if (t.get('arr') or '').upper() != 'DARTBOARD':
            continue
        for L in analyse_trial(t, cfg)['legs']:
            if not L.get('missing'):
                fs = L['fslots'] if len(clean(L['fslots'])) == len(t['ids']) else L['slots']
                dlegs.append({'key': (t['k'], L['leg']), 'slots': fs, 'cid': trial_cid(t), 'n': len(t['ids']),
                              'events': L['events_now']})
    dfit = dart_fit_now(dlegs, cfg)
    res['dart_fit'] = {k: v for k, v in dfit.items() if k != 'legs'}
    if dlegs:
        lines.append('Dartboard layout fit (FIGHTER_FLYING 7.2: Halton in float32, seed container id x NOW, NOW in '
                     '[event, event + %d]) over %d legs: %d fitted, %d match, %d without NOW. Score = RMS of (side '
                     'error, along error x %.2f) after one common along shift (<= %.0f); a match when the score is <= '
                     '%.2f and <= %.1f x the %d %% quantile of %d NOW values from %d frames after the order' % (
                         NOW_WIN, len(dlegs), dfit['n_fit'], dfit['n_match'], dfit['n_nonow'], ALONG_W, SHIFT_MAX,
                         ABS_MATCH, HALTON_MATCH, int(BASE_Q * 100), NOW_BASE_N, NOW_BASE_OFF))
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
                h = q.get('halton') or {}
                if h.get('status') == 'fit':
                    lines.append('        Halton fit (seed container id %s x NOW): score %.2f (plain RMS %.2f, along shift '
                                 '%+.2f) laid at %s +%d (NOW %d) of %d candidates; baseline q5 %.2f, median %.2f -> %s;'
                                 ' retail slot order score %s (plain %s)' % (
                                     r.get('cid'), h['rms'], h['rms_plain'], h['shift'], h['event'], h['dnow'],
                                     h['now'], h['cands'], h['base_min'], h['base_med'],
                                     'MATCH' if h['match'] else 'no match',
                                     fmt(h.get('retail'), '%.2f') if h['match'] else '-',
                                     fmt(h.get('retail_plain'), '%.2f') if h['match'] else '-'))
                    lines.append('        settled slots in the stop facing: ' + slot_str(L['fslots']))
                    if h['match']:
                        lines.append('        laid points: ' + slot_str(q.get('halton_pts') or []))
                else:
                    lines.append('        Halton fit: %s' % (h.get('status') or 'n/a'))
            if q.get('set_change') is not None:
                lines.append('        change from the previous settled slots: set RMS %.2f, per-member RMS %.2f' % (
                    q['set_change'], q['member_change']))
            s = L['speed']
            lines.append('     path heading: at the start %s deg, at the stop %s deg (bend %s deg)' % (
                fmt(heading_deg(L['u_start']), '%.1f'), fmt(heading_deg(L['u_end']), '%.1f'), fmt(L['bend'], '%.1f')))
            lines.append('     7.7 container speed step: start ramp %s / frame (parabola through %d polls), braking %s / '
                         'frame (%d polls); expected %s' % (fmt(s['accel_est'], '%.4f'), s['accel_n'],
                                                            fmt(s['brake_est'], '%.4f'), s['brake_n'],
                                                            fmt(s['a_exp'], '%.4f')))
            hs = s['hx_samples']
            lines.append('     7.7 straggler cap: HX solved at %d polls: %s; predicted %s (%s, team speed %s)%s' % (
                len(hs), 'median %.2f (%.2f..%.2f)' % (s['hx_med'], min(hs), max(hs)) if hs else 'n/a',
                fmt(s.get('hx_pred'), '%.2f'), s.get('hx_src'), fmt(s['team_speed'], '%.3f'),
                '' if s.get('hx_match') is None else ' -> MATCH' if s['hx_match'] else ' -> DIFFERS'))
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
        lines.append('  Q1 after the stop: frames to all within the recovery distance: %s -> %s%s' % (
            ' '.join(fmt(c, '%.0f') for c in x['close_after_stop']), x['Q1_close'],
            ' (%d leg(s) without settled slots skipped)' % x['close_skipped'] if x.get('close_skipped') else ''))
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
                if h.get('status') == 'fit':
                    lines.append('     trial %d leg %d: Halton score %.2f (baseline q5 %.2f; plain RMS %.2f) laid at %s +%d '
                                 '(NOW %d)%s%s' % (
                                     h['k'], h['leg'], h['rms'], h['base_min'], h['rms_plain'], h['event'], h['dnow'],
                                     h['now'], '' if h['match'] else ' (no match)',
                                     '; retail slot order score %.2f' % h['retail']
                                     if h.get('retail') is not None and h['match'] else ''))
                else:
                    lines.append('     trial %d leg %d: %s' % (h['k'], h['leg'], h.get('status')))
            lines.append('  Q3 Dartboard slot order (7.2 step 6: x / projection sorts, groups with the i = j + 1 skip): '
                         '%s' % x['Q3_order'])
            lines.append('  Q3 Dartboard re-lay rule (7.2: a new layout when the path heading turns > %.2f deg from the '
                         'last laid one): %s' % (RELAY_DEG, x['relay_rule']))
            for k, leg, ang, exp, got in x.get('relay_detail', []):
                lines.append('     trial %d leg %d: turn from the last layout %s deg, expected %s, fitted %s' % (
                    k, leg, fmt(ang, '%.1f'), exp, got))
        a = x['accel']
        lines.append('  7.7 container acceleration per frame: start ramps %s; braking %s; expected %s (member '
                     'OverrideDeceleration x %s) -> %s' % (
                         ' '.join(('%.4f' if n >= ACC_MIN_POLLS else '(%.4f)') % y
                                  for y, n in zip(a['start'], a.get('start_n') or [ACC_MIN_POLLS] * len(a['start']))) or '-',
                         ' '.join(('%.4f' if n >= ACC_MIN_POLLS else '(%.4f)') % y
                                  for y, n in zip(a['brake'], a.get('brake_n') or [ACC_MIN_POLLS] * len(a['brake']))) or '-',
                         fmt(a['expected'], '%.4f'), fmt(xvall.get('speed_mult') or 1.56, '%.2f'), x['Q77_accel']))
        lines.append('  7.7 straggler cap (HX solved per poll; Rows 3 x 3 grid predicts 15.0): %s -> %s' % (
            '; '.join('trial %d leg %d: %.2f (%.2f..%.2f, %d polls) vs %s (%s)' % (
                h['k'], h['leg'], h['med'], h['lo'], h['hi'], h['n'], fmt(h['pred'], '%.2f'), h['src'])
                for h in x['hx']) or 'no samples', x['Q77_straggler']))
    lines.append('expected (port, FIGHTER_FLYING 7.2): leg 2 (0 deg) and leg 4 (5 deg) keep the slots (small change, '
                 'jiggle only), leg 3 (30 deg) re-lays (Dartboard: a new scatter, set change well above the jiggle; '
                 'Rows: the same grid, members may swap); a leg whose path runs > 8.11 deg from the last layout re-lays')
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
