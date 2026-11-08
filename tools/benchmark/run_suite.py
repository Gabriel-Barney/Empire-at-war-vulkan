#!/usr/bin/env python3
"""BM3: run the benchmark scenes (tests/benchmark/*.abench) several times each and report the run-to-run noise on this PC.

    python tools/benchmark/run_suite.py [--repeats 3] [--scenes a,b] [--load] [--build-dir build-msvc] [--out logs/bench_suite]
                                         [--list] [--max-busy 10] [--keep-going]

Default: for every scene, REPEATS separate process runs (a headless scene: alamo_headless --bench, one timed run each; a rendered
scene: alamo_game --hidden --bench-scenario in the scenario's own mode, full length). Each run's result JSON lands in OUT as
<scene>_<n>.json (feed two of them to compare_results.py). The summary is the spread of the headline numbers across the repeats:
min / max / mean, spread = (max - min) / mean and the coefficient of variation, per scene, written to OUT/suite.json and printed.

--load (t04): the rendered scenes in real time (--benchmark realtime --bench-ticks 300), each REPEATS times as a pair: a COLD run
(a fresh empty --cache-dir: pipelines and shaders compile during the run) then a WARM run (the same directory again). The summary
is load_ms, the first-300-frame times and the hitch counts, cold against warm.

A run is marked noisy when the machine was busy: the CPU use sampled just before the run exceeds --max-busy percent (default 10), or
(headless) the result itself says other processes took more than 10%. Noisy numbers are printed but must not become a baseline.
Exit 0 all runs fine, 1 a run failed (a stale scene exits 3 in its tool: remake it, docs/PHASE4.md BM3), 2 usage / missing exe.
Game data: ALAMO_FOC_DATA (set by tools\\test_env.cmd; tools\\benchmark\\run_suite.cmd calls it).
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
BENCH_DIR = os.path.join(ROOT, "tests", "benchmark")


def host_of(path: str) -> str:
    try:
        for line in open(path, encoding="utf-8").read().splitlines():
            if line.startswith("host "):
                return line.split()[1]
    except OSError:
        pass
    return "headless"


def scenes() -> list[tuple[str, str, str]]:
    out = []
    for n in sorted(os.listdir(BENCH_DIR)):
        if n.endswith(".abench"):
            p = os.path.join(BENCH_DIR, n)
            out.append((n[:-7], p, host_of(p)))
    return out


def cpu_busy_pct() -> float | None:
    """The whole machine's CPU use over about 2 s (typeperf; None when unavailable). Called just before a run."""
    try:
        r = subprocess.run(["typeperf", r"\Processor(_Total)\% Processor Time", "-sc", "2"], capture_output=True, text=True, timeout=20)
        vals = []
        for line in r.stdout.splitlines():
            parts = [p.strip().strip('"') for p in line.split('","')]
            if len(parts) == 2:
                try:
                    vals.append(float(parts[1].strip('"')))
                except ValueError:
                    pass
        return round(sum(vals) / len(vals), 1) if vals else None
    except (OSError, subprocess.SubprocessError):
        return None


def stat(values: list[float]) -> dict:
    mean = statistics.fmean(values)
    return {
        "n": len(values), "min": min(values), "max": max(values), "mean": mean,
        "spread_pct": round((max(values) - min(values)) / mean * 100.0, 2) if mean else None,
        "cv_pct": round(statistics.pstdev(values) / mean * 100.0, 2) if mean and len(values) > 1 else 0.0,
    }


def headline(kind: str, d: dict) -> dict:
    if kind == "headless":
        s = d.get("summary", {})
        return {"median_ms": s.get("median_ms"), "p99_ms": s.get("p99_ms"), "ticks_per_second": s.get("ticks_per_second")}
    return {"frame_median_ms": d.get("frame_ms", {}).get("median"), "frame_p99_ms": d.get("frame_ms", {}).get("p99"),
            "sim_median_ms": d.get("sim_ms", {}).get("median"), "gpu_frame_median_ms": d.get("gpu_ms", {}).get("frame", {}).get("median")}


def run_one(cmd: list[str], env, log_path: str) -> tuple[int, float]:
    t0 = time.time()
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(r.stdout + r.stderr)
    return r.returncode, time.time() - t0


def table(rows: list[list[str]]) -> str:
    w = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    return "\n".join("  " + "  ".join(c.ljust(w[i]) if i == 0 else c.rjust(w[i]) for i, c in enumerate(r)) for r in rows)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--scenes", default="", help="comma list of scene names (default: all in tests/benchmark)")
    ap.add_argument("--load", action="store_true", help="map-load scenes: realtime, cold then warm cache, rendered scenes only")
    ap.add_argument("--build-dir", default="build-msvc")
    ap.add_argument("--out", default=os.path.join("logs", "bench_suite"))
    ap.add_argument("--max-busy", type=float, default=10.0, help="CPU percent above which a run counts as noisy (default 10)")
    ap.add_argument("--keep-going", action="store_true", help="carry on after a failed run")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)

    all_scenes = scenes()
    want = {s for s in args.scenes.split(",") if s}
    unknown = want - {n for n, _, _ in all_scenes}
    if unknown:
        print("run_suite: unknown scene(s): " + ", ".join(sorted(unknown)) + "  (have: " + ", ".join(n for n, _, _ in all_scenes) + ")", file=sys.stderr)
        return 2
    chosen = [s for s in all_scenes if not want or s[0] in want]
    if args.load:
        chosen = [s for s in chosen if s[2] == "game"]
    if args.list:
        for n, _, h in chosen:
            print(f"{n}  ({h})")
        return 0
    bd = args.build_dir if os.path.isabs(args.build_dir) else os.path.join(ROOT, args.build_dir)
    headless_exe, game_exe = os.path.join(bd, "bin", "alamo_headless.exe"), os.path.join(bd, "bin", "alamo_game.exe")
    for n, _, h in chosen:
        exe = headless_exe if h == "headless" else game_exe
        if not os.path.isfile(exe):
            print(f"run_suite: {exe} not found (build first)", file=sys.stderr)
            return 2
    if not os.environ.get("ALAMO_FOC_DATA"):
        print("run_suite: ALAMO_FOC_DATA is not set (tools\\benchmark\\run_suite.cmd sets it)", file=sys.stderr)
        return 2
    out = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    summary: dict = {"format": "alamo-bench-suite", "format_version": 1, "date_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                     "mode": "load" if args.load else "scenes", "repeats": args.repeats, "max_busy_pct": args.max_busy, "scenes": []}
    failed = 0
    t_start = time.time()
    for name, path, host in chosen:
        entry: dict = {"name": name, "host": host, "runs": []}
        print(f"{name} ({host}):", flush=True)
        cache_dirs = []
        for i in range(1, args.repeats + 1):
            variants = ["cold", "warm"] if args.load else [""]
            cache = os.path.join(out, f"{name}_cache_{i}")
            if args.load:
                shutil.rmtree(cache, ignore_errors=True)
                cache_dirs.append(cache)
            for v in variants:
                tag = f"{name}_load_{v}_{i}" if args.load else f"{name}_{i}"
                res = os.path.join(out, tag + ".json")
                if os.path.exists(res):
                    os.remove(res)
                if host == "headless":
                    cmd = [headless_exe, "--bench", path, "--runs", "1", "--warmup", "0", "--quiet", "--bench-json", res]
                elif args.load:
                    cmd = [game_exe, "--hidden", "--bench-scenario", path, "--benchmark", "realtime", "--bench-ticks", "300", "--cache-dir", cache, "--bench-report", res]
                else:
                    cmd = [game_exe, "--hidden", "--bench-scenario", path, "--bench-report", res]
                busy = cpu_busy_pct()
                rc, secs = run_one(cmd, env, os.path.join(out, tag + ".log"))
                run: dict = {"file": os.path.basename(res), "rc": rc, "seconds": round(secs, 1), "cpu_busy_before_pct": busy, "variant": v}
                d = None
                if os.path.isfile(res):
                    try:
                        d = json.load(open(res, encoding="utf-8"))
                    except ValueError:
                        d = None
                if rc != 0 or d is None:
                    failed += 1
                    run["failed"] = True
                    print(f"  {tag}: FAILED exit {rc} (log: {tag}.log)", flush=True)
                    entry["runs"].append(run)
                    if not args.keep_going:
                        break
                    continue
                noisy = (busy is not None and busy > args.max_busy) or bool(d.get("summary", {}).get("noisy"))
                run["noisy"] = noisy
                if args.load:
                    run["load_ms"] = d.get("load_ms")
                    run["early_median_ms"] = d.get("early_ms", {}).get("median")
                    run["early_p99_ms"] = d.get("early_ms", {}).get("p99")
                    run["hitches"] = d.get("hitches", {}).get("count")
                    run["worst_ms"] = d.get("frame_ms", {}).get("worst")
                else:
                    run["headline"] = headline("headless" if host == "headless" else "game", d)
                entry["runs"].append(run)
                print(f"  {tag}: {secs:5.1f} s, cpu busy before {busy}%" + ("  NOISY" if noisy else ""), flush=True)
            else:
                continue
            break
        ok = [r for r in entry["runs"] if not r.get("failed")]
        entry["noisy"] = any(r.get("noisy") for r in ok)
        if args.load:
            for v in ("cold", "warm"):
                rs = [r for r in ok if r["variant"] == v]
                if rs:
                    entry[v] = {k: stat([r[k] for r in rs if r.get(k) is not None]) for k in ("load_ms", "early_median_ms", "early_p99_ms", "hitches", "worst_ms")
                                if any(r.get(k) is not None for r in rs)}
        elif ok:
            keys = ok[0]["headline"].keys()
            entry["noise"] = {k: stat([r["headline"][k] for r in ok if r["headline"].get(k) is not None]) for k in keys
                              if any(r["headline"].get(k) is not None for r in ok)}
        summary["scenes"].append(entry)
        if failed and not args.keep_going:
            break
        for c in cache_dirs:
            shutil.rmtree(c, ignore_errors=True)

    summary["wall_seconds"] = round(time.time() - t_start, 1)
    with open(os.path.join(out, "suite.json" if not args.load else "suite_load.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    print()
    if args.load:
        rows = [["scene", "variant", "load ms (min..max)", "early median ms", "early p99 ms", "hitches"]]
        for e in summary["scenes"]:
            for v in ("cold", "warm"):
                s = e.get(v)
                if s:
                    g = lambda k: f"{s[k]['mean']:.1f}" if k in s else "-"
                    rows.append([e["name"] + (" (noisy)" if e["noisy"] else ""), v, f"{s['load_ms']['min']:.0f}..{s['load_ms']['max']:.0f}" if "load_ms" in s else "-",
                                 g("early_median_ms"), g("early_p99_ms"), g("hitches")])
    else:
        rows = [["scene", "runs", "metric", "mean", "spread %", "cv %"]]
        for e in summary["scenes"]:
            for k, s in e.get("noise", {}).items():
                rows.append([e["name"] + (" (noisy)" if e["noisy"] else ""), str(s["n"]), k, f"{s['mean']:.4g}", str(s["spread_pct"]), str(s["cv_pct"])])
    if len(rows) > 1:
        print(table(rows))
    print(f"\nrun_suite: {summary['wall_seconds']} s, {failed} failed run(s); summary: {os.path.relpath(os.path.join(out, 'suite_load.json' if args.load else 'suite.json'), ROOT)}")
    if any(e["noisy"] for e in summary["scenes"]):
        print("run_suite: NOISY runs above (the machine was busy): do not record these as a baseline")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
