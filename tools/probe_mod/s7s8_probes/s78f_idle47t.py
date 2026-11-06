"""s78f_idle47t.py - t11 toggle analysis for the IDLE47T scenario of the S7/S8 probe (2026-11-05).

  s78f_idle47t.py <reader.log>

IDLE47T: six Pod_Walkers (EV_PODWALKER has no ATTACKIDLE clip) facing away from held targets; every 300 frames
"on<c>" (Attack_Target: targeting state 3 / 4) and 150 frames later "off<c>" (Stop()). The synced RNG draws per frame
carry a large background (the battle around), so the test is the step at each edge: the median of single-frame draw
counts in a window after the edge minus the one before it. If slot 47 redraws Get_Float(0.2, 0.75) on every service
of a walker in state 3 / 4, "on" edges step up and "off" edges step down by about six per targeting service frame.
Also prints the walkers' targeting state and clip around each edge.
"""
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # run with python -I
import s78f_extra_parse as P  # noqa: E402


def main():
    modes = P.load(sys.argv[1])
    for n in sorted(modes, key=int):
        m = modes[n]
        recs = [P.parse_rec(r) for r in m['lua']]
        marks = sorted(((d['mark'], d['F']) for d in recs if d['kind'] == 'MARK' and d.get('scn') == 'IDLE47T'
                        and d['F'] is not None), key=lambda x: x[1])
        if not marks:
            continue
        ids = sorted({d['id'] for d in recs if d['kind'] == 'SPAWN' and d.get('scn') == 'IDLE47T' and
                      d.get('tag', '').startswith('i') and not d.get('tag', '').startswith('iv') and d.get('id') in m['st']})
        per, unknown = P.draws_per_frame(m['rng'])
        fr = sorted(per)
        single = {f: per[f] for i, f in enumerate(fr) if i and fr[i - 1] == f - 1}
        print('== mode %s: walkers %s, gaps %d' % (n, ids, unknown))

        def med(a, b):
            v = [single[f] for f in range(a, b) if f in single]
            return (statistics.median(v), len(v)) if v else (None, 0)

        def states(f):
            out = []
            for i in ids:
                best = None
                for d in m['st'].get(i, []):
                    if int(d['frame']) <= f:
                        best = d
                    else:
                        break
                out.append('%s/%s' % (best.get('ts'), best.get('anim', '-').split(':')[0]) if best else '-')
            return ' '.join(out)

        ups, downs = [], []
        for name, f in marks:
            b, nb = med(f - 140, f - 10)
            a, na = med(f + 20, f + 140)
            if b is None or a is None:
                continue
            step = a - b
            (ups if name.startswith('on') else downs).append(step)
            print('  %-6s F=%5d  before %6.1f (%3d)  after %6.1f (%3d)  step %+6.1f   ts/clip at +60: %s' % (
                name, f, b, nb, a, na, step, states(f + 60)))
        if ups and downs:
            print('  on edges:  median step %+.1f  (%s)' % (statistics.median(ups), ' '.join('%+.0f' % x for x in ups)))
            print('  off edges: median step %+.1f  (%s)' % (statistics.median(downs), ' '.join('%+.0f' % x for x in downs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
