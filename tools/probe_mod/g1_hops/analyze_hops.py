"""Galactic fleet hops in a Q2 reference trace (tools/probe_mod/q2_traces, "Q2TRACE 1" format).

Standard library only. Run with the project venv:
    toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\g1_hops\\analyze_hops.py <trace.q2t> [--from A] [--to B] [--csv]

For every GALACTIC_FLEET object it finds runs of constant per-frame steps (a hyperspace hop), names the origin and
destination planet (the planet centre the run starts at and the planet centre the run points at), and prints:

    fleet  owner  from -> to  distance  step/frame  frames  seconds@30Hz

`frames` counts mode frames from the sample at the origin planet centre (the departure sample, first position of the
hop) to the sample at the arrival orbit slot (the snap, GALACTIC_ECONOMY.md 3.2), i.e. the number of ticks the hop takes
and what the port's `frames_for_hop()` / `GalacticSim::update()` count. The trace carries no wall clock; seconds are
frames / 30 (the default single-player tick rate, MAIN_LOOP.md 1.2).

step/frame = slowest Hyperspace_Speed x faction Hyperspace_Speed_Factor (0.07) x M x R (GALACTIC_ECONOMY.md 3.1), so
step / 0.07 = s * M * R: with s = 1.5 (almost every ship), M = 5 (Object_Max_Speed_Multiplier_Galactic, FoC) and R the
route's HS_Speed_Factor (5, 6, 7, 10) or 1 (no route).
"""
import collections
import math
import struct
import sys


def f32(h):
    return struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]


def load(path):
    cur = -1
    obj = {}
    hist = collections.defaultdict(list)  # id -> [(frame, x, y, z)]
    tname = {}
    planets = set()
    for line in open(path):
        p = line.split()
        if not p:
            continue
        c = p[0]
        if c == 'F':
            cur = int(p[1])
        elif c == 'O':
            i = int(p[1])
            o = obj.setdefault(i, {})
            o['owner'] = int(p[2])
            o['type'] = int(p[3])
            o['pos'] = (f32(p[4]), f32(p[5]), f32(p[6]))
            hist[i].append((cur,) + o['pos'])
        elif c == 'G':
            planets.add(int(p[1]))
        elif c == 'T':
            tname[int(p[1])] = p[4]
    return obj, hist, tname, planets


def hops(path):
    obj, hist, tname, planet_ids = load(path)
    pl = {tname.get(obj[i]['type'], str(i)): obj[i]['pos'] for i in planet_ids if i in obj}
    rows = []
    for i, h in sorted(hist.items()):
        if tname.get(obj[i]['type']) != 'GALACTIC_FLEET':
            continue
        steps = []
        for a, b in zip(h, h[1:]):
            d = math.hypot(b[1] - a[1], b[2] - a[2])
            if b[0] - a[0] == 1 and 0.05 < d < 10:
                steps.append((a, b, d))
        runs, run = [], []
        for s in steps:
            if run and s[0][0] == run[-1][1][0] and abs(s[2] - run[-1][2]) < 1e-3:
                run.append(s)
            else:
                if run:
                    runs.append(run)
                run = [s]
        if run:
            runs.append(run)
        for r in runs:
            if len(r) < 5:
                continue
            a0, b1 = r[0][0], r[-1][1]
            v = r[0][2]
            L = math.hypot(b1[1] - a0[1], b1[2] - a0[2])
            ux, uy = (b1[1] - a0[1]) / L, (b1[2] - a0[2]) / L
            frm = [n for n, p in pl.items() if math.hypot(p[0] - a0[1], p[1] - a0[2]) < v + 1e-3]
            to = sorted(((rx * ux + ry * uy), n) for n, p in pl.items()
                        for rx, ry in [(p[0] - a0[1], p[1] - a0[2])]
                        if abs(rx * uy - ry * ux) < 0.5 and rx * ux + ry * uy > L - 1)
            if not frm or not to:
                continue
            frm_n, to_n = frm[0], to[0][1]
            # The departure sample sits on the origin planet centre; the arrival snap is one frame after the last step.
            frames = b1[0] + 1 - a0[0]
            dist = math.dist(pl[frm_n][:2], pl[to_n][:2])
            rows.append((i, obj[i]['owner'], frm_n, to_n, dist, v, frames))
    return rows


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    want_from = argv[argv.index('--from') + 1].upper() if '--from' in argv else None
    want_to = argv[argv.index('--to') + 1].upper() if '--to' in argv else None
    csv = '--csv' in argv
    seen = set()
    print('fleet,owner,from,to,distance,step_per_frame,frames,seconds_30hz,step_over_0.07' if csv else
          'fleet owner from -> to  distance  step/frame  frames  s@30Hz  step/0.07')
    for i, owner, a, b, dist, v, frames in hops(argv[1]):
        if want_from and a != want_from or want_to and b != want_to:
            continue
        key = (a, b, round(v, 3), frames)
        tag = '' if key not in seen else '  (same as an earlier hop)'
        seen.add(key)
        if csv:
            print('%d,%d,%s,%s,%.3f,%.5f,%d,%.2f,%.3f' % (i, owner, a, b, dist, v, frames, frames / 30.0, v / 0.07))
        else:
            print('%5d %3d %-12s -> %-12s %8.3f %9.4f %6d %7.2f %8.3f%s' % (i, owner, a, b, dist, v, frames, frames / 30.0, v / 0.07, tag))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
