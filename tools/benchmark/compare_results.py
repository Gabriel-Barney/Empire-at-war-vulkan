#!/usr/bin/env python3
"""BM3: compare two benchmark result files side by side.

    python tools/benchmark/compare_results.py BEFORE.json AFTER.json [--threshold PCT] [--fail-slower PCT]
                                              [--allow-different-scenario] [--allow-different-settings]
                                              [--allow-different-machine] [--markdown]

Reads the two result kinds the benchmark writes and compares like with like only:
  * alamo_headless --bench result   ("format": "alamo-bench-result"; sim-only, per-tick ms, profiler zones)
  * alamo_game --bench-report file  ("alamo_game_benchmark": 1; rendered, per-frame ms, GPU scopes, load and hitches)

A comparison across different scenarios is meaningless, so it is REFUSED (exit 2) unless the two files name the same scenario at
the same version with the same fingerprint (headless: save and fingerprint digests + "comparable"; game: scenario name + version +
fingerprint "match"), and, for game reports, the same mode / preset / size / frames per tick / tick count. Each refusal can be
overridden by the matching --allow-* flag; the report then says in its header what was overridden. A different machine (CPU, GPU,
driver) or build is only a note - comparing two builds is the point; comparing two PCs is the user's call (--allow-different-machine
silences the note).

Rows: lower is better except ticks_per_second. A change smaller than --threshold percent (default 3) reads "same"; a run flagged noisy
(headless: other processes used the CPU) marks its rows "(noisy)" and never counts for --fail-slower.
Exit: 0 compared, 2 refused (different scenario / kind / unreadable), 4 a headline metric is slower than --fail-slower percent.
"""
from __future__ import annotations

import argparse
import json
import sys


class Refused(Exception):
    pass


def load(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        raise Refused(f"{path}: cannot read as JSON: {e}")
    if not isinstance(d, dict):
        raise Refused(f"{path}: not a benchmark result (a JSON object was expected)")
    return d


def kind_of(d: dict, path: str) -> str:
    if d.get("format") == "alamo-bench-result":
        return "headless"
    if "alamo_game_benchmark" in d:
        return "game"
    raise Refused(f"{path}: not a benchmark result (neither \"format\": \"alamo-bench-result\" nor \"alamo_game_benchmark\")")


def get(d: dict, *keys, default=None):
    for k in keys:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return d


def scenario_name(d: dict, kind: str) -> str:
    if kind == "headless":
        return str(get(d, "scenario", "name", default="?"))
    s = str(get(d, "scenario", default="")).replace("\\", "/")
    return s.rsplit("/", 1)[-1].removesuffix(".abench") or "?"


def identity_problems(a: dict, b: dict, kind: str) -> tuple[list[str], list[str]]:
    """(scenario problems, settings problems): each one a reason the two runs measured different things."""
    scen: list[str] = []
    sett: list[str] = []

    def same(label, x, y, into):
        if x != y:
            into.append(f"{label}: {x!r} vs {y!r}")

    same("scenario", scenario_name(a, kind), scenario_name(b, kind), scen)
    if kind == "headless":
        same("scenario version", get(a, "scenario", "version"), get(b, "scenario", "version"), scen)
        same("save digest", get(a, "scenario", "save_digest"), get(b, "scenario", "save_digest"), scen)
        same("fingerprint digest", get(a, "scenario", "fingerprint_digest"), get(b, "scenario", "fingerprint_digest"), scen)
        for tag, d in (("BEFORE", a), ("AFTER", b)):
            if get(d, "comparable") is not True:
                scen.append(f"{tag} is not comparable with its scenario ({get(d, 'fingerprint', 'message', default='fingerprint did not match')})")
        same("ticks", get(a, "settings", "ticks"), get(b, "settings", "ticks"), sett)
        same("mode", get(a, "mode"), get(b, "mode"), sett)
    else:
        same("scenario version", get(a, "scenario_version"), get(b, "scenario_version"), scen)
        for tag, d in (("BEFORE", a), ("AFTER", b)):
            if get(d, "fingerprint") != "match":
                scen.append(f"{tag} fingerprint is {get(d, 'fingerprint', default='missing')!r}, not 'match'")
            if get(d, "result") != "ok":
                scen.append(f"{tag} result is {get(d, 'result')!r} ({get(d, 'error', default='no detail')})")
        same("start tick", get(a, "start_tick"), get(b, "start_tick"), scen)
        for key in ("mode", "preset", "width", "height", "frames_per_tick", "target_ticks", "taa", "frame_cap", "present_mode"):
            same(key, get(a, key), get(b, key), sett)
    return scen, sett


def machine_notes(a: dict, b: dict, kind: str) -> list[str]:
    notes = []

    def diff(label, x, y):
        if x != y:
            notes.append(f"{label}: {x} -> {y}")

    if kind == "headless":
        for k in ("cpu", "logical_cores", "memory_mb", "os", "gpu", "gpu_driver"):
            diff(k, get(a, "machine", k), get(b, "machine", k))
        diff("workers", get(a, "settings", "workers"), get(b, "settings", "workers"))
        for k in ("compiler", "config", "asserts"):
            diff("build " + k, get(a, "build", k), get(b, "build", k))
    else:
        diff("device", get(a, "device"), get(b, "device"))
    return notes


def pct(x: float, y: float) -> float | None:
    return None if not x else (y - x) / x * 100.0


class Row:
    def __init__(self, name: str, x, y, unit: str, higher_better: bool = False, headline: bool = False):
        self.name, self.x, self.y, self.unit, self.hb, self.headline = name, x, y, unit, higher_better, headline

    def change(self) -> float | None:
        if not isinstance(self.x, (int, float)) or not isinstance(self.y, (int, float)):
            return None
        return pct(float(self.x), float(self.y))

    def verdict(self, threshold: float) -> str:
        c = self.change()
        if c is None:
            return ""
        if abs(c) < threshold:
            return "same"
        better = (c > 0) == self.hb
        return ("faster" if better else "SLOWER") if self.unit in ("ms", "/s") else ("better" if better else "WORSE")


def stats_rows(prefix: str, a: dict, b: dict, path: tuple, headline: bool = False) -> list[Row]:
    rows = []
    for k in ("median", "p99", "worst", "mean"):
        x, y = get(a, *path, k), get(b, *path, k)
        if x is not None and y is not None:
            rows.append(Row(f"{prefix} {k}", x, y, "ms", headline=headline and k in ("median", "p99")))
    return rows


def rows_headless(a: dict, b: dict) -> list[Row]:
    rows = []
    for k in ("median_ms", "p99_ms", "worst_ms", "mean_ms"):
        x, y = get(a, "summary", k), get(b, "summary", k)
        if x is not None and y is not None:
            rows.append(Row("tick " + k[:-3], x, y, "ms", headline=k in ("median_ms", "p99_ms")))
    x, y = get(a, "summary", "ticks_per_second"), get(b, "summary", "ticks_per_second")
    if x is not None and y is not None:
        rows.append(Row("ticks per second", x, y, "/s", higher_better=True))
    return rows


def zone_rows(a: dict, b: dict, top: int) -> list[Row]:
    za = {z["name"]: z for z in get(a, "zones", default=[]) if isinstance(z, dict) and "name" in z}
    zb = {z["name"]: z for z in get(b, "zones", default=[]) if isinstance(z, dict) and "name" in z}
    names = sorted(set(za) & set(zb), key=lambda n: -max(za[n].get("share", 0), zb[n].get("share", 0)))[:top]
    return [Row("zone " + n + " mean", za[n].get("mean_ms"), zb[n].get("mean_ms"), "ms") for n in names]


def rows_game(a: dict, b: dict) -> list[Row]:
    rows = stats_rows("frame", a, b, ("frame_ms",), headline=True)
    rows += stats_rows("render (CPU)", a, b, ("render_ms",))
    rows += stats_rows("sim", a, b, ("sim_ms",))
    ga, gb = get(a, "gpu_ms", default={}), get(b, "gpu_ms", default={})
    for scope in ga:
        if scope in gb:
            for k in ("median", "p99"):
                rows.append(Row(f"GPU {scope} {k}", get(ga, scope, k), get(gb, scope, k), "ms"))
    rows += stats_rows("early (first 300 frames)", a, b, ("early_ms",))[:2]
    if get(a, "load_ms") is not None and get(b, "load_ms") is not None:
        rows.append(Row("load time", a["load_ms"], b["load_ms"], "ms"))
    rows.append(Row("hitches", get(a, "hitches", "count"), get(b, "hitches", "count"), "n"))
    return rows


def fmt(v) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.4g}" if abs(v) < 100 else f"{v:.1f}"
    return str(v)


def render(rows: list[Row], threshold: float, noisy: bool) -> list[str]:
    out = []
    w = max([len(r.name) for r in rows] + [6])
    out.append(f"{'metric'.ljust(w)}  {'before':>10}  {'after':>10}  {'change':>8}  verdict")
    for r in rows:
        c = r.change()
        cs = "-" if c is None else f"{c:+.1f}%"
        v = r.verdict(threshold)
        out.append(f"{r.name.ljust(w)}  {fmt(r.x):>10}  {fmt(r.y):>10}  {cs:>8}  {v}{' (noisy)' if noisy and v not in ('', 'same') else ''}")
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--threshold", type=float, default=3.0, help="percent below which a change reads 'same' (default 3)")
    ap.add_argument("--fail-slower", type=float, default=None, metavar="PCT", help="exit 4 when a headline metric is slower by more than PCT percent (noisy runs excluded)")
    ap.add_argument("--allow-different-scenario", action="store_true", help="compare although the scenario / version / fingerprint differ (numbers then mean little)")
    ap.add_argument("--allow-different-settings", action="store_true", help="compare although mode / preset / size / ticks differ")
    ap.add_argument("--allow-different-machine", action="store_true", help="do not note a different CPU / GPU / driver / build")
    ap.add_argument("--zones", type=int, default=8, help="headless: how many profiler zones to list (by share)")
    args = ap.parse_args(argv)

    try:
        a, b = load(args.before), load(args.after)
        ka, kb = kind_of(a, args.before), kind_of(b, args.after)
        if ka != kb:
            raise Refused(f"refused: {args.before} is a {ka} result and {args.after} a {kb} one (sim-only and rendered numbers do not compare)")
        scen, sett = identity_problems(a, b, ka)
        overridden = []
        for problems, allowed, flag in ((scen, args.allow_different_scenario, "--allow-different-scenario"), (sett, args.allow_different_settings, "--allow-different-settings")):
            if problems and not allowed:
                raise Refused("refused: the two results did not measure the same thing:\n  " + "\n  ".join(problems) +
                              f"\n(re-run both on the same scenario version, or pass {flag} to compare anyway)")
            if problems:
                overridden += [f"{flag}: " + p for p in problems]
    except Refused as e:
        print(f"compare_results: {e}", file=sys.stderr)
        return 2

    name = scenario_name(a, ka)
    print(f"compare_results: {name} ({'sim-only' if ka == 'headless' else 'rendered, ' + str(get(a, 'mode'))})")
    print(f"  before: {args.before}\n  after:  {args.after}")
    for o in overridden:
        print(f"  OVERRIDDEN {o}")
    if not args.allow_different_machine:
        for n in machine_notes(a, b, ka):
            print(f"  note: {n}")
    noisy_a = ka == "headless" and bool(get(a, "summary", "noisy"))
    noisy_b = ka == "headless" and bool(get(b, "summary", "noisy"))
    for tag, n, d in (("before", noisy_a, a), ("after", noisy_b, b)):
        if n:
            print(f"  note: {tag} was noisy (other processes used up to {fmt(get(d, 'summary', 'max_other_cpu_pct'))}% of the CPU)")
    if ka == "game":
        for tag, d in (("before", a), ("after", b)):
            if get(d, "unsettled_frames", default=0):
                print(f"  note: {tag} had {get(d, 'unsettled_frames')} unsettled frames")
        if get(a, "final_crc") != get(b, "final_crc"):
            print(f"  note: final CRC differs ({get(a, 'final_crc')} vs {get(b, 'final_crc')}): the sim did not run the same")
    elif get(a, "summary", "ticks") != get(b, "summary", "ticks"):
        print(f"  note: run counts differ ({get(a, 'settings', 'timed_runs')} vs {get(b, 'settings', 'timed_runs')} timed runs)")
    print()
    rows = rows_headless(a, b) if ka == "headless" else rows_game(a, b)
    if ka == "headless":
        rows += zone_rows(a, b, args.zones)
    noisy = noisy_a or noisy_b
    print("\n".join(render(rows, args.threshold, noisy)))

    if args.fail_slower is not None and not noisy:
        bad = [r for r in rows if r.headline and (r.change() or 0.0) * (-1 if r.hb else 1) > args.fail_slower]
        if bad:
            print("\ncompare_results: SLOWER than " + f"{args.fail_slower:g}%: " + ", ".join(f"{r.name} {r.change():+.1f}%" for r in bad))
            return 4
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
