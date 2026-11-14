#!/usr/bin/env python3
"""Worker-count sweep (SIMP sp0 / sp1): every bench scene sim-only in alamo_headless at several worker counts, interleaved rounds.

    python tools/benchmark/worker_sweep.py [--workers 0,1,3,7,15] [--rounds 2] [--scenes a,b] [--build-dir build-wt-msvc]
                                           [--out logs/worker_sweep/<stamp>] [--zones tac.ai.perception,tac.perc.*]

For each round, for each scene, for each worker count: one process `alamo_headless --bench SCENE --runs 1 --warmup 0 --workers W
--quiet --bench-json OUT/<scene>_w<W>_r<round>.json` (rendered scenes run sim-only too, like PERFORMANCE.md "Game scenes timed
sim-only"). Interleaving keeps machine-load drift out of the worker-count comparison. Before each run the whole-machine CPU use is
sampled (typeperf) and stored next to the result: record it with every number (several lanes may be building).

Prints, per scene: median tick and p99 per worker count and round, the busy CPU before each run, and the ratio of every count to W0
(the sp1 exit: no scene slower than W0 by more than 3 %). `--zones` adds a zone table (p50 / mean ms and share, W0 and the largest
count) for zone names or `prefix*` patterns. Writes OUT/sweep.json with every number. Exit 0, 1 when a run failed or a fingerprint
did not match, 2 usage. Game data: ALAMO_FOC_DATA (tools\\test_env.cmd sets it).
"""
from __future__ import annotations

import argparse
import datetime
import fnmatch
import json
import os
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_suite import ROOT, cpu_busy_pct, scenes  # noqa: E402


def zone_rows(results: dict, scene: str, counts: list[int], patterns: list[str]) -> list[list[str]]:
    """Mean over rounds of each matching zone's p50 / mean ms and share, at the smallest and largest worker count."""
    lo, hi = counts[0], counts[-1]
    names: list[str] = []
    for w in (lo, hi):
        for d in results[scene].get(w, []):
            for z in d.get("zones", []):
                n = z.get("name", "")
                if any(fnmatch.fnmatchcase(n, p) for p in patterns) and n not in names:
                    names.append(n)

    def avg(w: int, name: str, key: str) -> float | None:
        vals = [z[key] for d in results[scene].get(w, []) for z in d.get("zones", []) if z.get("name") == name and key in z]
        return statistics.fmean(vals) if vals else None

    rows = [["zone", f"W{lo} p50", f"W{lo} mean", f"W{lo} share", f"W{hi} p50", f"W{hi} mean", f"W{hi} share"]]
    keyed = sorted(names, key=lambda n: -(avg(lo, n, "mean_ms") or 0.0))
    for n in keyed:
        r = [n]
        for w in (lo, hi):
            p50, mean, share = avg(w, n, "p50_ms"), avg(w, n, "mean_ms"), avg(w, n, "share")
            r += [f"{p50:.3f}" if p50 is not None else "-", f"{mean:.3f}" if mean is not None else "-",
                  f"{share * 100:.1f}%" if share is not None else "-"]
        rows.append(r)
    return rows


def table(rows: list[list[str]]) -> str:
    w = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    return "\n".join("  " + "  ".join(c.ljust(w[i]) if i == 0 else c.rjust(w[i]) for i, c in enumerate(r)) for r in rows)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--workers", default="0,1,3,7,15")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--scenes", default="", help="comma list of scene names (default: all in tests/benchmark)")
    ap.add_argument("--build-dir", default="build-wt-msvc")
    ap.add_argument("--out", default="")
    ap.add_argument("--zones", default="", help="comma list of zone names or prefix* patterns for the zone table")
    a = ap.parse_args(argv)
    try:
        counts = sorted({int(x) for x in a.workers.split(",") if x.strip()})
    except ValueError:
        print("worker_sweep: --workers wants a comma list of integers", file=sys.stderr)
        return 2
    exe = os.path.join(ROOT, a.build_dir, "bin", "alamo_headless.exe")
    if not os.path.isfile(exe):
        print(f"worker_sweep: {exe} not found (build first)", file=sys.stderr)
        return 2
    want = [s for s in a.scenes.split(",") if s]
    todo = [(n, p) for n, p, _ in scenes() if not want or n in want]
    if want and len(todo) != len(want):
        print(f"worker_sweep: unknown scene in {want}", file=sys.stderr)
        return 2
    out = a.out or os.path.join(ROOT, "logs", "worker_sweep", datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    results: dict = {n: {} for n, _ in todo}
    busy: dict = {n: {} for n, _ in todo}
    failed = False
    for rnd in range(1, a.rounds + 1):
        for name, path in todo:
            for w in counts:
                res = os.path.join(out, f"{name}_w{w}_r{rnd}.json")
                b = cpu_busy_pct()
                cmd = [exe, "--bench", path, "--runs", "1", "--warmup", "0", "--workers", str(w), "--quiet", "--bench-json", res]
                r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
                with open(res[:-5] + ".log", "w", encoding="utf-8") as f:
                    f.write(r.stdout + r.stderr)
                d = {}
                if os.path.isfile(res):
                    try:
                        d = json.load(open(res, encoding="utf-8"))
                    except (OSError, ValueError):
                        d = {}
                ok = r.returncode == 0 and d.get("comparable", False)
                failed = failed or not ok
                d["busy_before_pct"] = b
                results[name].setdefault(w, []).append(d)
                busy[name].setdefault(w, []).append(b)
                med = d.get("summary", {}).get("median_ms")
                print(f"  round {rnd} {name} W{w}: median {med if med is not None else '?'} ms, busy before "
                      f"{b if b is not None else '?'} %{'' if ok else '  FAILED (exit ' + str(r.returncode) + ')'}", flush=True)

    summary: dict = {"workers": counts, "rounds": a.rounds, "scenes": {}}
    for name, _ in todo:
        rows = [["metric"] + [f"W{w}" for w in counts]]
        sc: dict = {}
        for key, label in (("median_ms", "median ms"), ("p99_ms", "p99 ms")):
            rows.append([label] + [" / ".join(f"{d.get('summary', {}).get(key, float('nan')):.2f}" for d in results[name][w]) for w in counts])
        rows.append(["busy %"] + [" / ".join(f"{b:.0f}" if b is not None else "?" for b in busy[name][w]) for w in counts])
        base = [d.get("summary", {}).get("median_ms") for d in results[name][counts[0]]]
        ratios = []
        for w in counts:
            vals = [d.get("summary", {}).get("median_ms") for d in results[name][w]]
            mean = statistics.fmean([v for v in vals if v is not None]) if any(v is not None for v in vals) else None
            bmean = statistics.fmean([v for v in base if v is not None]) if any(v is not None for v in base) else None
            ratios.append(mean / bmean if mean and bmean else None)
            sc[f"W{w}"] = {"median_ms": vals, "p99_ms": [d.get("summary", {}).get("p99_ms") for d in results[name][w]],
                           "busy_before_pct": busy[name][w], "ratio_to_first": ratios[-1]}
        rows.append([f"/ W{counts[0]}"] + [f"{r:.3f}" if r else "?" for r in ratios])
        summary["scenes"][name] = sc
        print(f"\n{name}:\n{table(rows)}")
        if a.zones:
            pats = [p for p in a.zones.split(",") if p]
            zr = zone_rows(results, name, counts, pats)
            if len(zr) > 1:
                print(table(zr))
    with open(os.path.join(out, "sweep.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    print(f"\nworker_sweep: results in {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
