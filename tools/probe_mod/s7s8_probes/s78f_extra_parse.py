"""s78f_extra_parse.py - report for the s78f t09 / t11 scenarios of the S7/S8 probe (2026-11-05).

  s78f_extra_parse.py <reader.log> [--out report_extra.txt]

Scenarios (s7s8_hook.lua): IDLE47 (t11, slot 47 on a model without an Attack Idle clip), MAGNEPULSE (t09, slew
before the charge-up), BUNKER (t09, garrisoned Merc squad across its grenade reach), GUARD_SHIFT (t09, fighters
guarding a moving frigate + a shift-queued waypoint, ordered by hand).

Synced RNG draws: the reader logs the LCG state (0x140a13e24) on every change ("RNG n= frame= s="). The step count
between two samples is found by stepping s <- s * 0x41C64E6D + 0xBDF (TACTICAL_COMBAT sec. 0.2) until the next value
(capped; a gap above the cap is counted as unknown). Draws are summed per mode frame window.
"""
import argparse
import collections
import math
import re
import sys

MUL, INC, MASK = 0x41C64E6D, 0xBDF, 0xFFFFFFFF
STEP_CAP = 200000

KV = re.compile(r'(\w+)=(\S*)')


def kv(line):
    return dict(KV.findall(line))


def steps_between(a, b):
    s = a
    for n in range(1, STEP_CAP + 1):
        s = (s * MUL + INC) & MASK
        if s == b:
            return n
    return None


def load(path):
    modes = collections.defaultdict(lambda: {'rng': [], 'obj': {}, 'st': collections.defaultdict(list), 'lua': {}})
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('RNG '):
                d = kv(line)
                if d.get('frame') not in (None, 'None'):
                    modes[d['n']]['rng'].append((int(d['frame']), int(d['s'], 16)))
            elif line.startswith('OBJ '):
                d = kv(line)
                modes[d['n']]['obj'][d['obj']] = d
            elif line.startswith('ST '):
                d = kv(line)
                modes[d['n']]['st'][d['obj']].append(d)
            elif line.startswith('LUA '):
                n = kv(line.split(' rec=')[0])['n']
                rec = line.split(' rec=', 1)[1].strip()
                if rec.startswith('S78P1|') and rec.endswith('|END'):
                    modes[n]['lua'][rec] = rec
    return modes


def parse_rec(rec):
    parts = rec.split('|')
    d = {'kind': parts[1]}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    try:
        d['F'] = int(d.get('f', ''))
    except ValueError:
        d['F'] = None
    return d


def draws_per_frame(rng):
    """frame -> draws counted in the samples that arrived on that frame; plus the unknown gaps."""
    per = collections.Counter()
    unknown = 0
    for (f0, s0), (f1, s1) in zip(rng, rng[1:]):
        n = steps_between(s0, s1)
        if n is None:
            unknown += 1
        else:
            per[f1] += n
    return per, unknown


def window(per, a, b):
    if b <= a:
        return None
    tot = sum(v for f, v in per.items() if a <= f < b)
    return tot, b - a, tot / float(b - a)


def frame_hist(st, ids, a, b, key):
    """Frames per value of key(sample) in [a, b), summed over ids. The reader writes an ST line only when something
    changes, so a value holds from its sample until the next one (also from a sample before a). Counting samples
    instead (the s78f version) undercounted long steady states, e.g. state 3 / 4 of held attackers (s78g t03)."""
    h = collections.Counter()
    for i in ids:
        cur, cf = None, None
        for d in st.get(i, []):
            f = int(d['frame'])
            if f >= b:
                break
            if cur is not None and f > a:
                h[key(cur)] += f - max(cf, a)
            cur, cf = d, f
        if cur is not None and b > max(cf, a):
            h[key(cur)] += b - max(cf, a)
    return dict(h)


def ts_hist(st, ids, a, b):
    return frame_hist(st, ids, a, b, lambda d: d.get('ts', '-'))


def anim_hist(st, ids, a, b):
    return frame_hist(st, ids, a, b, lambda d: d.get('anim', '-').split(':')[0])


def report(modes, out):
    w = out.write
    for n in sorted(modes, key=int):
        m = modes[n]
        recs = [parse_rec(r) for r in m['lua']]
        scns = {}
        for d in recs:
            if d['kind'] == 'SCN' and d.get('scn') and d['F'] is not None:
                scns.setdefault(d['scn'], {})[d.get('ev')] = d['F']
        mine = [s for s in ('IDLE47', 'MAGNEPULSE', 'BUNKER', 'GUARD_SHIFT', 'GUARD_MOVE') if s in scns]
        if not mine:
            continue
        per, unknown = draws_per_frame(m['rng'])
        w('== mode %s: RNG samples %d, frames with draws %d, gaps beyond %d steps %d ==\n' % (
            n, len(m['rng']), len(per), STEP_CAP, unknown))
        marks = {}
        for d in recs:
            if d['kind'] == 'MARK' and d['F'] is not None:
                marks[(d.get('scn'), d.get('mark'))] = d
        spawns = [d for d in recs if d['kind'] == 'SPAWN' and d.get('ok') == '1']

        if 'IDLE47' in scns:
            e = scns['IDLE47']
            w('\n-- IDLE47 (t11): F=%s..%s\n' % (e.get('start'), e.get('end')))
            names = ['c0', 'a', 'b', 'c1']
            fr = [marks.get(('IDLE47', k), {}).get('F') for k in names]
            ends = fr[1:] + [e.get('end')]
            for k, a, b in zip(names, fr, ends):
                if a is None or b is None:
                    w('   %-3s not measured\n' % k)
                    continue
                ids = [d['id'] for d in spawns if d.get('scn') == 'IDLE47' and d.get('tag', '').startswith('i')
                       and not d.get('tag', '').startswith('iv') and a <= d['F'] < b]
                r = window(per, a + 90, b)
                w('   %-3s F=%d..%d  draws %s in %s frames = %.2f / frame; attackers %s ts %s clips %s\n' % (
                    k, a + 90, b, r[0], r[1], r[2], len(ids), ts_hist(m['st'], ids, a + 90, b),
                    anim_hist(m['st'], ids, a + 90, b)))

        if 'MAGNEPULSE' in scns:
            e = scns['MAGNEPULSE']
            w('\n-- MAGNEPULSE (t09): F=%s..%s\n' % (e.get('start'), e.get('end')))
            for d in spawns:
                if d.get('scn') == 'MAGNEPULSE':
                    w('   spawn %-4s %s at (%s, %s) id=%s\n' % (d.get('tag'), d.get('type'), d.get('x'), d.get('y'),
                                                            d.get('id')))
            blobs = sorted(int(o['frame']) for o in m['obj'].values() if 'MAGNEPULSE' in o.get('type', '')
                           and o.get('type', '').startswith(('MAGNEPULSE', 'PROJ')))
            w('   blob spawns (memory frames): %s\n' % blobs)
            for d in recs:
                if d['kind'] == 'COUNT' and d.get('scn') == 'MAGNEPULSE':
                    w('   ready=%s at F=%s\n' % (d.get('ready'), d['F']))
            fires = sorted((d for d in recs if d['kind'] == 'FIRE'), key=lambda d: d['F'])
            for d in fires:
                nxt = [b for b in blobs if b >= d['F']]
                dl = (nxt[0] - d['F']) if nxt else None
                w('   FIRE i=%s at %s F=%s ok=%s ret=%s ready=%s -> next blob %s (delay %s)\n' % (
                    d.get('i'), d.get('tag'), d['F'], d.get('ok'), d.get('ret'), d.get('ready'),
                    nxt[0] if nxt else '-', dl))
            w('   expected: delay = slew services + 90; from rest to a target behind up to 45 + 90 = 135; the same\n'
              '   spot again (turret held its aim) 90 (LOB_SOLVER sec. 3.6, sec. 9 probe bullet)\n')

        if 'BUNKER' in scns:
            e = scns['BUNKER']
            w('\n-- BUNKER (t09): F=%s..%s\n' % (e.get('start'), e.get('end')))
            for d in recs:
                if d.get('scn') == 'BUNKER' and (d['kind'] in ('SPAWN', 'ORDER') or
                                                 (d['kind'] == 'COUNT' and d.get('what') == 'in_garrison')):
                    w('   %s %s\n' % (d['kind'], ' '.join('%s=%s' % (k, v) for k, v in sorted(d.items())
                                                       if k not in ('kind', 'r', 'f', 'F', 'scn'))))
            grenades = sorted(int(o['frame']) for o in m['obj'].values() if 'GRENADE' in o.get('type', ''))
            merc_ids = []
            for d in recs:
                if d['kind'] == 'SPAWN' and d.get('tag') == 'mercs':
                    merc_ids = [x for x in d.get('ids', '').split(',') if x]
            near = [o['obj'] for o in m['obj'].values() if o.get('type', '').startswith('UNDERWORLD_MERC')]
            wins = sorted((d for (s, k), d in marks.items() if s == 'BUNKER' and k.startswith('win')),
                          key=lambda d: d['F'])
            ends = [d['F'] for d in wins[1:]] + [e.get('end')]
            for d, b in zip(wins, ends):
                a = d['F']
                if b is None:
                    continue
                r = window(per, a + 30, b)
                g = len([x for x in grenades if a <= x < b])
                w('   d=%-4s F=%d..%d grenades %2d  draws %.2f / frame  merc ts %s\n' % (
                    d.get('d'), a, b, g, r[2] if r else float('nan'), ts_hist(m['st'], near, a + 30, b)))

        for gname in ('GUARD_SHIFT', 'GUARD_MOVE'):
            if gname not in scns:
                continue
            e = scns[gname]
            w('\n-- %s (t09 / t12, by hand): F=%s..%s\n' % (gname, e.get('start'), e.get('end')))
            g = [d for d in spawns if d.get('scn') == gname and d.get('tag') == 'g' and d.get('id') in m['st']]
            gid = g[0]['id'] if g else None
            sq = [d for d in spawns if d.get('scn') == gname and d.get('tag') == 'squad' and any(l and l[0].get('cont') == d.get('id') for l in m['st'].values())]
            sqid = sq[0]['id'] if sq else None
            # only the probe squadron's members (their container is the spawned squadron), not the player's own TIEs
            fighters = [i for i, l in m['st'].items() if sqid and l and l[0].get('cont') == sqid]
            for d in recs:
                if d['kind'] in ('MARK', 'ORDER') and d.get('scn') == gname:
                    w('   %s F=%s %s\n' % (d['kind'], d['F'], d.get('mark') or '%s %s ok=%s' % (
                        d.get('tag'), d.get('order'), d.get('ok'))))
            f0 = e.get('start') or 0
            f1 = e.get('end') or max([int(x['frame']) for l in m['st'].values() for x in l[-1:]] or [f0 + 8400])

            def pos_at(oid, f):
                best = None
                for d in m['st'].get(oid, []):
                    if int(d['frame']) <= f:
                        best = d
                    else:
                        break
                return (float(best['x']), float(best['y'])) if best else None

            prev = None
            for f in range(f0, f1 + 1, 30):
                gp = pos_at(gid, f) if gid else None
                fp = [p for p in (pos_at(o, f) for o in fighters) if p]
                if not fp or not gp:
                    continue
                cx = sum(p[0] for p in fp) / len(fp)
                cy = sum(p[1] for p in fp) / len(fp)
                gs = (math.hypot(gp[0] - prev[0], gp[1] - prev[1]) / 30.0) if prev else 0.0
                prev = gp
                w('   F=%5d  G (%7.0f, %7.0f) speed %5.1f/frame  fighters %d centroid (%7.0f, %7.0f) dist to G %6.0f\n' % (
                    f, gp[0], gp[1], gs, len(fp), cx, cy, math.hypot(cx - gp[0], cy - gp[1])))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--out')
    a = ap.parse_args()
    modes = load(a.log)
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            report(modes, f)
        print('written', a.out)
    else:
        report(modes, sys.stdout)


if __name__ == '__main__':
    main()
