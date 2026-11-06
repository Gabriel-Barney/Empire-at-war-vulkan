#!/usr/bin/env python3
"""BM3: self-test of compare_results.py on synthetic result files (no game data, no build).

    python tools/benchmark/test_compare_results.py [WORKDIR]

Covers: a clean headless and a clean game comparison (verdicts, exit 0), the refusals (different scenario / version / digest / kind /
not-comparable / fingerprint mismatch / different settings, exit 2) and their --allow-* overrides, noisy runs, --fail-slower (exit 4,
not for a noisy run) and unreadable input.
"""
import contextlib
import io
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare_results as cr  # noqa: E402

FAILS = 0


def check(cond, what):
    global FAILS
    if not cond:
        FAILS += 1
        print("FAIL:", what)


def run(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = cr.main(list(argv))
    return rc, out.getvalue(), err.getvalue()


def scen(name="space_battle", version=2, save="aa", fp="bb"):
    return {"name": name, "version": version, "save_digest": save, "fingerprint_digest": fp}


def headless(median=1.0, noisy=False, **over):
    d = {
        "format": "alamo-bench-result", "format_version": 1, "tool": "alamo_headless", "mode": "sim-only",
        "scenario": scen(),
        "fingerprint": {"ok": True, "message": "every run matched the fingerprint"}, "comparable": True,
        "settings": {"ticks": 1800, "workers": 15, "timed_runs": 3},
        "machine": {"cpu": "CPU A", "gpu": "GPU A", "gpu_driver": "1", "logical_cores": 16, "memory_mb": 32000, "os": "Windows"},
        "build": {"compiler": "MSVC", "config": "Release", "asserts": False},
        "summary": {"ticks": 5400, "median_ms": median, "p99_ms": median * 2.5, "worst_ms": 40.0, "mean_ms": median * 1.1,
                    "ticks_per_second": 1000.0 / median, "max_other_cpu_pct": 60.0 if noisy else 1.0, "noisy": noisy},
        "zones": [{"name": "T5.battles", "share": 0.3, "mean_ms": median * 0.3}, {"name": "tac.ai", "share": 0.2, "mean_ms": median * 0.2}],
    }
    d.update(over)
    return d


def stats(m):
    return {"count": 100, "mean": m, "median": m, "p99": m * 1.5, "worst": m * 3, "total": m * 100}


def game(frame=5.0, **over):
    d = {
        "alamo_game_benchmark": 1, "result": "ok", "mode": "fixed", "preset": "bm-high", "device": "GPU A", "width": 1920, "height": 1080,
        "present_mode": "immediate", "frame_cap": 0, "taa": False, "frames_per_tick": 2, "target_ticks": 1800, "start_tick": 1000,
        "ticks": 1800, "unsettled_frames": 0, "frame_ms": stats(frame), "render_ms": stats(frame * 0.8), "sim_ms": stats(frame * 0.4),
        "final_crc": "00ff", "scenario": "tests/benchmark/nebula_battle.abench", "scenario_version": 1, "fingerprint": "match",
        "gpu_ms": {"frame": stats(frame * 0.2), "scene": stats(frame * 0.1)}, "load_ms": 900.0, "early_ms": stats(frame),
        "hitches": {"threshold_ms": 25.0, "count": 2, "worst": [[0, 40.0]]},
    }
    d.update(over)
    return d


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="bench_compare_")
    os.makedirs(work, exist_ok=True)

    def w(name, obj):
        p = os.path.join(work, name)
        with open(p, "w", encoding="utf-8") as f:
            f.write(obj if isinstance(obj, str) else json.dumps(obj))
        return p

    # --- headless ---
    a, same, slow, fast = w("h_a.json", headless(1.0)), w("h_same.json", headless(1.01)), w("h_slow.json", headless(1.2)), w("h_fast.json", headless(0.8))
    rc, out, err = run(a, same)
    check(rc == 0 and "same" in out and "SLOWER" not in out, "headless: 1% apart reads same")
    rc, out, err = run(a, slow)
    check(rc == 0 and "SLOWER" in out and "+20.0%" in out and "ticks per second" in out, "headless: 20% slower")
    check("zone T5.battles mean" in out, "headless: zones listed")
    rc, out, err = run(a, fast)
    check(rc == 0 and "faster" in out and "SLOWER" not in out, "headless: faster, and ticks/s up reads faster")
    rc, out, err = run(a, slow, "--fail-slower", "10")
    check(rc == 4 and "SLOWER than 10%" in out, "headless: --fail-slower trips at 20%")
    rc, out, err = run(a, slow, "--fail-slower", "25")
    check(rc == 0, "headless: --fail-slower 25 does not trip at 20%")
    rc, out, err = run(a, fast, "--fail-slower", "1")
    check(rc == 0, "headless: faster never trips --fail-slower")
    noisy = w("h_noisy.json", headless(1.2, noisy=True))
    rc, out, err = run(a, noisy, "--fail-slower", "5")
    check(rc == 0 and "was noisy" in out and "(noisy)" in out, "headless: a noisy run is flagged and never fails the gate")

    # refusals
    for tag, mod in (("name", {"scenario": scen(name="land_battle")}),
                     ("version", {"scenario": scen(version=3)}),
                     ("save digest", {"scenario": scen(save="zz")}),
                     ("fingerprint digest", {"scenario": scen(fp="zz")}),
                     ("not comparable", {"comparable": False, "fingerprint": {"ok": False, "message": "run 1 differs at tick 5"}})):
        p = w("h_bad_" + tag.replace(" ", "_") + ".json", headless(1.0, **mod))
        rc, out, err = run(a, p)
        check(rc == 2 and "refused" in err and out == "", tag + ": headless refused")
        rc, out, err = run(a, p, "--allow-different-scenario")
        check(rc == 0 and "OVERRIDDEN --allow-different-scenario" in out, tag + ": headless overridden")
    other_ticks = w("h_ticks.json", headless(1.0, settings={"ticks": 900, "workers": 15, "timed_runs": 3}))
    rc, out, err = run(a, other_ticks)
    check(rc == 2 and "ticks" in err and "--allow-different-settings" in err, "headless: different tick count refused")
    rc, out, err = run(a, other_ticks, "--allow-different-settings")
    check(rc == 0, "headless: different tick count overridden")
    other_pc = w("h_pc.json", headless(1.0, machine={"cpu": "CPU B", "gpu": "GPU A", "gpu_driver": "1", "logical_cores": 8, "memory_mb": 32000, "os": "Windows"}))
    rc, out, err = run(a, other_pc)
    check(rc == 0 and "note: cpu: CPU A -> CPU B" in out, "headless: a different CPU is a note, not a refusal")
    rc, out, err = run(a, other_pc, "--allow-different-machine")
    check(rc == 0 and "cpu:" not in out, "headless: --allow-different-machine silences the note")

    # --- game ---
    g1, g2, g3 = w("g_a.json", game(5.0)), w("g_b.json", game(5.0)), w("g_slow.json", game(6.5))
    rc, out, err = run(g1, g2)
    check(rc == 0 and "nebula_battle" in out and "GPU frame median" in out and "load time" in out and "hitches" in out, "game: clean comparison")
    rc, out, err = run(g1, g3)
    check(rc == 0 and "SLOWER" in out and "frame median" in out, "game: 30% slower")
    rc, out, err = run(g1, g3, "--fail-slower", "10")
    check(rc == 4, "game: --fail-slower")
    rc, out, err = run(g1, w("g_crc.json", game(5.0, final_crc="0001")))
    check(rc == 0 and "final CRC differs" in out, "game: a different final CRC is a note")
    for tag, mod in (("scenario", {"scenario": "tests/benchmark/water_battle.abench"}), ("version", {"scenario_version": 2}),
                     ("fingerprint", {"fingerprint": "the run started at tick 5, the scenario at 6"}), ("result", {"result": "failed", "error": "boom"}),
                     ("start tick", {"start_tick": 5})):
        p = w("g_bad_" + tag.replace(" ", "_") + ".json", game(5.0, **mod))
        rc, out, err = run(g1, p)
        check(rc == 2 and "refused" in err, tag + ": game refused")
        rc, out, err = run(g1, p, "--allow-different-scenario")
        check(rc == 0, tag + ": game overridden")
    for tag, mod in (("mode", {"mode": "realtime"}), ("preset", {"preset": "bm-low"}), ("size", {"width": 1280}), ("frames per tick", {"frames_per_tick": 1}),
                     ("ticks", {"target_ticks": 300})):
        p = w("g_set_" + tag.replace(" ", "_") + ".json", game(5.0, **mod))
        rc, out, err = run(g1, p)
        check(rc == 2 and "--allow-different-settings" in err, tag + ": game settings refused")
        rc, out, err = run(g1, p, "--allow-different-settings")
        check(rc == 0, tag + ": game settings overridden")

    # --- kinds and bad input ---
    rc, out, err = run(a, g1)
    check(rc == 2 and "headless result" in err and "game one" in err, "a headless and a game result do not compare")
    rc, out, err = run(a, w("empty.json", ""))
    check(rc == 2 and "cannot read" in err, "unreadable JSON refused")
    rc, out, err = run(a, w("list.json", "[1]"))
    check(rc == 2 and "not a benchmark result" in err, "a JSON list refused")
    rc, out, err = run(a, w("other.json", {"hello": 1}))
    check(rc == 2 and "not a benchmark result" in err, "an unrelated object refused")
    rc, out, err = run(a, os.path.join(work, "missing.json"))
    check(rc == 2, "a missing file refused")

    if FAILS:
        print("test_compare_results: %d FAILED" % FAILS)
        return 1
    print("test_compare_results: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
