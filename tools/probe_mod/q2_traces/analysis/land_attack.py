"""skai t02: what a retail land skirmish trace (run_skirmish_land_attack.cmd) shows about the AI taking the enemy base.

Standard library only. Reads a Q2TRACE 1 file (or a run folder holding trace.q2t) and prints, every --step ticks:
  - objects per player by type (the AI's army size against the 2-player land unit cap of 9, docs/re/SKIRMISH_SETUP.md);
  - how many AI objects stand within --radius of the human's base (the centroid of the human's objects at the first
    keyframe: the pre-built base and its default forces);
then the first tick an AI object comes that close, and every start object of the human that left the store (tick, type).

    toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\q2_traces\\analysis\\land_attack.py <trace.q2t | run dir>
        [--step 1800] [--radius 1500]

Players: the human is the C line with is_human = 1, the AI the C line with has_ai = 1 (the idle Empire and the Rebel AI
in the probe run).
"""
import argparse
import collections
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import q2trace  # noqa: E402


def f32(h):
    return struct.unpack(">f", bytes.fromhex(h))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--step", type=int, default=1800)
    ap.add_argument("--radius", type=float, default=1500.0)
    a = ap.parse_args()
    path = os.path.join(a.trace, "trace.q2t") if os.path.isdir(a.trace) else a.trace
    meta, _, frames, types, end_reason = q2trace.parse(path)
    name = {k: v[2] for k, v in types.items()}

    objs = {}  # id -> [owner, type, x, y, health]
    human = ai = None
    start = None  # human start objects: id -> type name
    centre = None
    first_near = None
    lost = []
    next_report = 0
    for fr in frames:
        for p in fr.lines:
            if p[0] == "O":
                objs[int(p[1])] = [int(p[2]), int(p[3]), f32(p[4]), f32(p[5]), f32(p[10])]
            elif p[0] == "X":
                oid = int(p[1])
                if start is not None and oid in start:
                    lost.append((fr.mframe, start[oid]))
                objs.pop(oid, None)
            elif p[0] == "C":
                if int(p[4]) == 1:
                    human = int(p[1])
                if int(p[5]) == 1:
                    ai = int(p[1])
        if start is None and fr.key and objs and human is not None:
            start = {i: name.get(o[1], str(o[1])) for i, o in objs.items() if o[0] == human}
            pts = [(objs[i][2], objs[i][3]) for i in start]
            if pts:
                centre = (sum(x for x, _ in pts) / len(pts), sum(y for _, y in pts) / len(pts))
        if centre is None:
            continue
        near = [i for i, o in objs.items() if o[0] == ai and math.hypot(o[2] - centre[0], o[3] - centre[1]) <= a.radius]
        if near and first_near is None:
            first_near = (fr.mframe, sorted(name.get(objs[i][1], "?") for i in near))
        if fr.mframe >= next_report:
            next_report = fr.mframe + a.step
            print("tick %d: %d AI objects within %.0f of the human base" % (fr.mframe, len(near), a.radius))
            for pl in (human, ai):
                c = collections.Counter(name.get(o[1], str(o[1])) for o in objs.values() if o[0] == pl)
                print("  p%s (%s, %d): %s" % (pl, "human" if pl == human else "AI", sum(c.values()),
                                              ", ".join("%sx%d" % kv for kv in sorted(c.items()))))
    print("human p%s, AI p%s, human base centre %s, end: %s" % (human, ai, centre, end_reason))
    print("first AI object near the human base: %s" % (first_near,))
    print("human start objects lost: %d of %d" % (len(lost), len(start or {})))
    for t, ty in lost:
        print("  tick %d %s" % (t, ty))


if __name__ == "__main__":
    main()
