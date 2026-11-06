#!/usr/bin/env python3
"""a2g t02 / t04: the port's galactic AI budget and plan starts against the retail a2f t07 probe (docs/re/A2G_T02_T04_PORT_VS_RETAIL.md).

Inputs
  retail: tools/probe_mod/a2f_budget/out/<run>/samples.tsv (decoded BudgetFractionToBuild samples) and records.txt (PSTART records)
  port:   alamo_headless --ai-log FILE (plan lines) and the stderr of a build patched by a2g_trace_patch.py (A2G BUD / DROP / ACC
          lines; ALAMO_A2G_TRACE=3 gives one budget row per AI player per frame up to frame 10200)

Commands
  budget  --retail-samples TSV --port-trace ERR [--faction EMPIRE --player 1]
          frame-0 / Init pools, then per fiscal cycle (payment frame 1351 + 1350 k) the Available pool A and rate R per category
          at the retail sample frames: first sample after the payment, last sample before the next one, min over the cycle.
  plans   --retail-records records.txt [more records.txt] --port-log LOG [LOG ...] [--window FRAMES]
          plan starts per plan name in [0, window] (retail PSTART deduplicated by plan + frame over all given runs; the port's
          "plan N NAME for" lines), per galactic minute (30 frames per second), and the port's "ended ... (released)" count.
  drops   --port-trace ERR [--port-log LOG] [--window FRAMES]
          goal-system maintenance drops by goal and reason (fund / desire / invalid / time / notrunning), time drops with
          AllowedTime vs the ETA they failed.
Python 3 standard library only.
"""
import argparse
import collections
import re
import sys

FPS = 30.0
PAY0, PERIOD = 1351, 1350          # first fiscal payment (mode frame) and the cycle, both sides (A2F_T07 1 point 5)
CATS = [('OFF', 'Offensive'), ('INFRA', 'Infrastructure'), ('MAJOR', 'MajorItem'), ('DEF', 'Defensive'), ('INFO', 'Information')]
BUD_RE = re.compile(r'^A2G BUD (\w+) f=(-?\d+) p=(\d+) cr=(\S+) gain=(\S+) inc68=(\S+) T=(\S+) sumP=(\S+) \|(.*)$')
CAT_RE = re.compile(r'(\w+) A=(\S+) R=(\S+) P=(\S+) w=(\S+) n=(\d+);')


def fnum(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def read_retail_samples(path, faction):
    rows = []
    with open(path, encoding='utf-8') as f:
        head = f.readline().rstrip('\n').split('\t')
        for line in f:
            v = dict(zip(head, line.rstrip('\n').split('\t')))
            if v.get('faction') != faction:
                continue
            r = {'f': fnum(v['frame']), 'cr': fnum(v['credits']), 'cat': {}}
            for key, _ in CATS:
                a, rate, st = fnum(v.get(key + '_avail')), fnum(v.get(key + '_rate')), v.get(key + '_state', '')
                if st == 'empty':
                    r['cat'][key] = (0.0, 0.0, 'empty')
                elif a is not None:
                    r['cat'][key] = (a, rate, st)
            rows.append(r)
    rows.sort(key=lambda r: r['f'])
    return rows


def read_port_budget(path, player):
    out = collections.defaultdict(list)  # tag -> rows
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = BUD_RE.match(line.strip())
            if not m or int(m.group(3)) != player:
                continue
            r = {'tag': m.group(1), 'f': int(m.group(2)), 'cr': float(m.group(4)), 'gain': float(m.group(5)),
                 'inc68': float(m.group(6)), 'T': float(m.group(7)), 'sumP': float(m.group(8)), 'cat': {}}
            names = dict((n, k) for k, n in CATS)
            for c in CAT_RE.finditer(m.group(9)):
                key = names.get(c.group(1), c.group(1))
                r['cat'][key] = (float(c.group(2)), float(c.group(3)), float(c.group(4)), float(c.group(5)))
            out[r['tag']].append(r)
    return out


def at_or_before(rows, frame):
    best = None
    for r in rows:
        if r['f'] <= frame:
            best = r
        else:
            break
    return best


def fmt(x, w=8):
    return ('%*.1f' % (w, x)) if x is not None else ' ' * (w - 1) + '-'


def cmd_budget(a):
    ret = read_retail_samples(a.retail_samples, a.faction)
    port = read_port_budget(a.port_trace, a.player)
    frames = port.get('frame', [])
    init = (port.get('init') or [None])[0]
    print('== %s: frame 0 / Init' % a.faction)
    r0 = ret[0]
    print('  retail f=%d credits %.1f   port Init f=%s credits %s' % (r0['f'], r0['cr'], init and init['f'], init and init['cr']))
    for key, name in CATS:
        ra = r0['cat'].get(key)
        pa = init['cat'].get(key) if init else None
        rv = ra[0] if ra else None
        pv = pa[0] if pa else None
        tot_r = r0['cr'] or 1.0
        tot_p = (init['cr'] if init else 0) or 1.0
        d = (pv - rv) if (rv is not None and pv is not None) else None
        print('  %-14s retail A %s (%.4f)   port A %s (%.4f)   diff %s' % (
            name, fmt(rv), (rv or 0) / tot_r, fmt(pv), (pv or 0) / tot_p, fmt(d)))
    ticks = port.get('tick', [])
    print('== fiscal payments (port tick rows)')
    for t in ticks[:12]:
        print('  f=%d gain %.2f -> %.4f/s T=%.0f credits after %.1f' % (t['f'], t['gain'], t['inc68'], t['T'], t['cr']))
    if not frames:
        print('  (no per-frame rows: run the patched build with ALAMO_A2G_TRACE=3)')
        return
    last_port = frames[-1]['f']
    print('== per cycle: Available A (retail | port) at the first retail sample after the payment / the last one before the next')
    print('   k  pay   f_first   OFF r|p            INFRA r|p          MAJOR r|p          f_last   OFF r|p            INFRA r|p'
          '          MAJOR r|p          min INFRA r|p     credits r|p (last)')
    k = 0
    while True:
        pay = PAY0 + k * PERIOD
        nxt = pay + PERIOD
        if pay > last_port:
            break
        cyc = [r for r in ret if pay <= r['f'] < nxt]
        if not cyc:
            k += 1
            continue

        def pick(rr, key):
            v = rr['cat'].get(key)
            return v[0] if v else None

        def first_with(key):
            for r in cyc:
                if pick(r, key) is not None:
                    return r
            return None

        def last_with(key):
            for r in reversed(cyc):
                if pick(r, key) is not None:
                    return r
            return None

        cols = []
        for which in (first_with, last_with):
            fr = which('OFF')
            cols.append(fr['f'] if fr else None)
            for key in ('OFF', 'INFRA', 'MAJOR'):
                r = which(key)
                pr = at_or_before(frames, int(r['f'])) if r else None
                cols.append((pick(r, key) if r else None, pick(pr, key) if pr else None))
        rmin = min((pick(r, 'INFRA') for r in cyc if pick(r, 'INFRA') is not None), default=None)
        pmin = min((pick(r, 'INFRA') for r in frames if pay <= r['f'] < nxt and pick(r, 'INFRA') is not None), default=None)
        lr = cyc[-1]
        lp = at_or_before(frames, int(lr['f']))
        line = '  %2d %5d ' % (k + 1, pay)
        for i in (0, 4):
            line += ' %7s ' % (int(cols[i]) if cols[i] is not None else '-')
            for j in range(1, 4):
                rv, pv = cols[i + j]
                line += ' %s|%s' % (fmt(rv, 7), fmt(pv, 7))
        line += '  %s|%s' % (fmt(rmin, 7), fmt(pmin, 7))
        line += '  %s|%s' % (fmt(lr['cr'], 7), fmt(lp['cr'] if lp else None, 7))
        print(line)
        k += 1
    neg_r = [r for r in ret if (r['cat'].get('INFRA') or (0,))[0] < -0.01]
    neg_p = [r for r in frames if (r['cat'].get('INFRA') or (0,))[0] < -0.01]
    print('== Infrastructure A < -0.01: retail %d samples, port %d frames (min retail %s, min port %s)' % (
        len(neg_r), len(neg_p),
        fmt(min((r['cat']['INFRA'][0] for r in ret if 'INFRA' in r['cat']), default=None)),
        fmt(min((r['cat']['INFRA'][0] for r in frames if 'INFRA' in r['cat']), default=None))))


PSTART_RE = re.compile(r'\|PSTART\|plan=(\w+) t=\S+ f=(\d+) fac=(\w+)')
PLAN_RE = re.compile(r'^(\d+) p(\d+) plan (\d+) (\S+) (for|ended) ?(.*)$')


def norm_plan(name):
    return re.sub(r'[^A-Z]', '', name.upper())


def cmd_plans(a):
    window = a.window
    minutes = window / FPS / 60.0
    retail = {}
    for path in a.retail_records or []:
        with open(path, encoding='latin-1') as f:
            for line in f:
                m = PSTART_RE.search(line)
                if m and int(m.group(2)) <= window:
                    retail[(m.group(1), int(m.group(2)), m.group(3))] = 1
    rcount = collections.Counter((norm_plan(p), fac) for (p, _, fac) in retail)
    print('== retail PSTART in frames 0..%d (%.2f galactic min), deduplicated over %d run(s)' % (window, minutes, len(a.retail_records or [])))
    for (p, fac), n in sorted(rcount.items()):
        frames = sorted(f for (pp, f, ff) in retail if norm_plan(pp) == p and ff == fac)
        print('  %-40s %-10s %3d  (%.2f/min)  frames %s' % (p, fac, n, n / minutes, frames))
    for path in a.port_log:
        starts, rel, ends = collections.Counter(), collections.Counter(), collections.Counter()
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = PLAN_RE.match(line.strip())
                if not m or int(m.group(1)) > window:
                    continue
                key = (m.group(4), 'p' + m.group(2))
                if m.group(5) == 'for':
                    starts[key] += 1
                else:
                    ends[key] += 1
                    if '(released)' in m.group(6):
                        rel[key] += 1
        print('== port %s, frames 0..%d: plan starts / ended (released)' % (path, window))
        for key in sorted(starts, key=lambda k: (-starts[k], k)):
            if a.only and not any(o in key[0] for o in a.only):
                continue
            print('  %-50s %-4s starts %3d (%.2f/min)  released %3d' % (key[0], key[1], starts[key], starts[key] / minutes, rel[key]))


DROP_RE = re.compile(r'^A2G DROP f=(\d+) goal=(\S+) why=(\S+) running=(-?\d+) at=(\S+) eta_el=(\S+) eta=(\S+) nominal=(\S+) created=(\d+)')


def cmd_drops(a):
    c = collections.Counter()
    times = collections.defaultdict(list)
    with open(a.port_trace, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = DROP_RE.match(line.strip())
            if not m or int(m.group(1)) > a.window:
                continue
            c[(m.group(2), m.group(3))] += 1
            if m.group(3) == 'time':
                times[m.group(2)].append((int(m.group(1)), float(m.group(5)), float(m.group(6)), float(m.group(7)), float(m.group(8)),
                                          (int(m.group(1)) - int(m.group(9))) / FPS))
    print('== maintenance drops in frames 0..%d (goal, reason)' % a.window)
    for (g, why), n in sorted(c.items(), key=lambda kv: (-kv[1], kv[0])):
        if why == 'notrunning' and not a.all:
            continue
        print('  %-50s %-8s %4d' % (g, why, n))
    for g, v in sorted(times.items()):
        print('  time drops %s: frame / AllowedTime / ETA(0x525200 + elapsed) / accept ETA / nominal / age s' % g)
        for x in v[:a.limit]:
            print('    f=%6d at=%7.2f eta_el=%7.2f eta=%7.2f nominal=%7.2f age=%6.1f' % x)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('budget')
    b.add_argument('--retail-samples', required=True)
    b.add_argument('--port-trace', required=True)
    b.add_argument('--faction', default='EMPIRE')
    b.add_argument('--player', type=int, default=1)
    p = sub.add_parser('plans')
    p.add_argument('--retail-records', nargs='*')
    p.add_argument('--port-log', nargs='+', required=True)
    p.add_argument('--window', type=int, default=10140)
    p.add_argument('--only', nargs='*')
    d = sub.add_parser('drops')
    d.add_argument('--port-trace', required=True)
    d.add_argument('--window', type=int, default=72000)
    d.add_argument('--all', action='store_true')
    d.add_argument('--limit', type=int, default=12)
    a = ap.parse_args(argv)
    {'budget': cmd_budget, 'plans': cmd_plans, 'drops': cmd_drops}[a.cmd](a)
    return 0


if __name__ == '__main__':
    sys.exit(main())
