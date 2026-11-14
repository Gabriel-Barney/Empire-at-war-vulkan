#!/usr/bin/env python3
"""Story-mission battle trace reader (roadmap story t04; docs/re/STORY_T04_KESSEL_RUN.md). Standard library only.

  story_battle.py <run dir | trace.q2t> [--port-trace port.q2t] [--json out.json]

Reads a retail `story` scenario trace (the linked tactical battle of a story campaign, README "Story mission run") and
prints what a hands-off battle does: players, object census by owner at battle frame 1, objects spawned or removed later
(the mission Lua's spawns / despawns), which object types move and when, health changes, and the synced-RNG draws per
battle-frame window. With --port-trace (an `alamo_headless --economy --galactic-ai --story --battles fight --trace-out`
state trace of the same campaign) it adds the port's draws over the same windows, battle frame k = the k-th host tick
after the first one with a tactical mode on top. The port's galactic state trace holds no battle objects, so only the
draws and the battle's start and end compare (STORY_T04_KESSEL_RUN.md §4).
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import q2trace  # noqa: E402  (the kit's parser and LCG step counter)

WINDOWS = [(1, 1), (2, 29), (30, 128), (129, 200), (201, 1800), (1801, 18000)]


def resolve(path):
    return os.path.join(path, "trace.q2t") if os.path.isdir(path) else path


def draws_by_frame(frames, frame_of):
    out, prev = {}, None
    for fr in frames:
        k = frame_of(fr)
        if prev is not None and k is not None:
            n = q2trace.lcg_steps(prev, fr.rng, 2_000_000)
            out[k] = out.get(k, 0) + (n if n is not None else 0)
        prev = fr.rng
    return out


def windows(draws):
    res = []
    for a, b in WINDOWS:
        frames = [f for f in draws if a <= f <= b]
        if frames:
            total = sum(draws[f] for f in frames)
            res.append({"frames": "%d-%d" % (a, b), "samples": len(frames), "draws": total,
                        "per_frame": round(total / len(frames), 2)})
    return res


def retail(path):
    meta, header, frames, types, end = q2trace.parse(path)
    tn = lambda t: types.get(t, (0, 0, "?"))[2]  # noqa: E731
    objs, first, gone, players = {}, {}, [], {}
    moves = collections.defaultdict(lambda: [None, None, 0])
    health = collections.Counter()
    census = None
    for fr in frames:
        for p in fr.lines:
            if p[0] == "O":
                oid = int(p[1])
                first.setdefault(oid, (fr.mframe, int(p[2]), int(p[3])))  # first sample: frame, owner, type
                old = objs.get(oid)
                if old is not None and fr.mframe > 1:
                    t = tn(int(p[3]))
                    if old[3:6] != p[4:7]:
                        m = moves[(t, int(p[2]))]
                        m[0] = fr.mframe if m[0] is None else m[0]
                        m[1] = fr.mframe
                        m[2] += 1
                    if old[9] != p[10]:
                        health[(t, int(p[2]))] += 1
                objs[oid] = p[1:]
            elif p[0] == "X":
                oid = int(p[1])
                o = objs.pop(oid, None)
                if o is not None:
                    gone.append({"frame": fr.mframe, "id": oid, "owner": int(o[1]), "type": tn(int(o[2])), "first": first[oid][0] if oid in first else None})
            elif p[0] == "C":
                players[int(p[1])] = {"human": int(p[4]), "ai": int(p[5])}
        if census is None and fr.mframe >= 1:
            census = {}
            for o in objs.values():
                census.setdefault(int(o[1]), collections.Counter())[tn(int(o[2]))] += 1
    spawned = [{"frame": f[0], "id": oid, "owner": f[1], "type": tn(f[2])}
               for oid, f in sorted(first.items(), key=lambda x: (x[1][0], x[0])) if f[0] > 1]
    draws = draws_by_frame(frames, lambda fr: fr.mframe)
    return {
        "trace": path, "end_reason": end, "start": header.get("start"), "label": meta.get("label"),
        "first_sample": {"global_frame": frames[0].g, "mode_type": frames[0].mtype, "stack": frames[0].stack} if frames else None,
        "last_mode_frame": frames[-1].mframe if frames else None,
        "players": players,
        "census_frame1": {o: {"total": sum(c.values()), "types": c.most_common(12)} for o, c in sorted((census or {}).items())},
        "spawned_after_frame1": spawned, "removed": gone,
        "moves": [{"type": k[0], "owner": k[1], "first": v[0], "last": v[1], "samples": v[2]} for k, v in sorted(moves.items())],
        "health_changes": [{"type": k[0], "owner": k[1], "samples": n} for k, n in health.most_common()],
        "draws_total": sum(draws.values()), "draws_windows": windows(draws),
        "draws_first_frames": [draws.get(f, 0) for f in range(1, 41)],
    }


def port(path):
    _, _, frames, _, end = q2trace.parse(path)
    start = next((fr.g for fr in frames if fr.mtype != 0), None)
    if start is None:
        return {"trace": path, "battle": False}
    before = next((fr for fr in reversed(frames) if fr.g < start), None)
    pre = q2trace.lcg_steps(0, before.rng, 5_000_000) if before else None
    draws = draws_by_frame(frames, lambda fr: fr.g - start + 1 if fr.g >= start else None)
    return {
        "trace": path, "battle": True, "end_reason": end, "battle_host_frame": start,
        "battle_on_top_to": max(fr.g for fr in frames if fr.mtype != 0),
        "stack": next(fr.stack for fr in frames if fr.mtype != 0),
        "draws_seed_to_battle": pre, "draws_total": sum(draws.values()), "draws_windows": windows(draws),
        "draws_first_frames": [draws.get(f, 0) for f in range(1, 41)],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("--port-trace")
    ap.add_argument("--json")
    a = ap.parse_args()
    r = retail(resolve(a.reference))
    out = {"retail": r}
    print("retail %s: end %s, start %s, last mode frame %s" % (r["trace"], r["end_reason"], r["start"], r["last_mode_frame"]))
    print("players (human, ai): %s" % {k: (v["human"], v["ai"]) for k, v in sorted(r["players"].items())})
    for o, c in r["census_frame1"].items():
        print("frame 1 owner %d: %d objects, %s" % (o, c["total"], ", ".join("%s %d" % t for t in c["types"])))
    for s in r["spawned_after_frame1"]:
        print("spawn  frame %5d id %d owner %s %s" % (s["frame"], s["id"], s["owner"], s["type"]))
    for g in r["removed"]:
        print("remove frame %5d id %d owner %d %s (first seen %s)" % (g["frame"], g["id"], g["owner"], g["type"], g["first"]))
    for m in r["moves"]:
        print("moves  %s owner %d: frames %s-%s, %d samples" % (m["type"], m["owner"], m["first"], m["last"], m["samples"]))
    print("health changes: %s" % (r["health_changes"] or "none"))
    print("retail draws: total %d; first frames %s" % (r["draws_total"], r["draws_first_frames"]))
    for w in r["draws_windows"]:
        print("  frames %-11s %6d draws (%.2f / frame)" % (w["frames"], w["draws"], w["per_frame"]))
    if a.port_trace:
        p = port(a.port_trace)
        out["port"] = p
        if not p["battle"]:
            print("port %s: no tactical mode on top" % p["trace"])
        else:
            print("port %s: battle on top from host frame %d to %d (stack %d), %s draws from seed 0 before it, end %s" % (
                p["trace"], p["battle_host_frame"], p["battle_on_top_to"], p["stack"], p["draws_seed_to_battle"], p["end_reason"]))
            print("port draws: total %d; first frames %s" % (p["draws_total"], p["draws_first_frames"]))
            for w in p["draws_windows"]:
                print("  frames %-11s %6d draws (%.2f / frame)" % (w["frames"], w["draws"], w["per_frame"]))
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(out, fh, indent=1, sort_keys=True, default=list)
            fh.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
