#!/usr/bin/env python3
"""Regenerate the golden-backed CRC goldens as checkpoint files (ALAMO_CRC 2) and print what changed.

Run it through tools\\regen_goldens.cmd (sets the toolchain PATH and the game-data environment). What it does:

1. Refuses to run when tracked files have uncommitted edits (exit 3) unless --force: the golden diff should come from
   the committed code, so the PR can state the intended behaviour change (docs/status/golden-checkpoints.md).
2. Headless goldens (engine/apps/headless/testdata/*.crc): finds every ctest entry of the MSVC tree that passes
   `--golden <checked-in file>` (ctest --show-only=json-v1, so the commands are never copied by hand), runs the same
   command through the program's own writer - without --golden / --check-workers, with
   `--crc-out <tmp> --crc-checkpoint N` - and stores the result over the golden. N and the keep ticks are those of the
   existing golden when it is already a checkpoint file, else --checkpoint (default 64).
3. Game smoke goldens (engine/apps/game/testdata/game_smoke_*.crc / .rec): runs the smoke tests
   alamo_game_smoke_{skirmish,campaign,land} of the tree (they need a GPU and the game data; --only headless skips them),
   converts the dumped full CRC log to a checkpoint file and copies the .rec.
4. Benchmark scenarios (tests/benchmark/*.abench, BM1; part of --only headless / all, skipped by --no-bench, alone with --only bench):
   a scenario carries an expected per-tick CRC fingerprint, so an intended sim behaviour change makes it stale. Each scenario is
   checked with `alamo_headless --bench X --runs 1 --warmup 0 --quiet` (exit 3 = "scenario changed, not comparable" = stale);
   a stale one is remade by tools/benchmark/regen_scenario.cmd (version bump + new .crc / .alsv / .acmd, summarised as
   "version 1 -> 2"). With --dry-run (or --bench-check) nothing is remade: it only reports which scenarios are stale.
5. Prints a diff summary per file: unchanged / changed, record counts, the first changed checkpoint (tick, columns that
   differ, the window of ticks it lies in), how many checkpoints changed. State the intended behaviour change from that in
   the PR. The exact tick: run the old and the new build with --crc-out full.crc and `alamo_headless --compare`.

Options: --bench-check (only report stale scenarios, exit 4 when one is stale; implies --only bench), --no-bench, --build-dir DIR (default build-msvc; the fast MSVC tree, Release, already built), --only headless|game|all
(default headless), --match REGEX (only headless goldens whose file name matches), --checkpoint N, --force, --dry-run (run
everything, change nothing), --list (what would run).
Exit: 0 ok, 1 a run failed, 3 refused (dirty tree), 2 usage / missing tree, 4 --bench-check found a stale scenario.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import convert_goldens as cg  # noqa: E402

HEADLESS_DIR = "engine/apps/headless/testdata"
GAME_DIR = "engine/apps/game/testdata"
SMOKES = ("skirmish", "campaign", "land")


def norm(p: str) -> str:
    return os.path.normcase(os.path.abspath(p)).replace("\\", "/")


def tracked_changes() -> list[str]:
    r = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print("git status failed: " + r.stderr.strip(), file=sys.stderr)
        sys.exit(2)
    return [line for line in r.stdout.splitlines() if line.strip()]


def ctest_json(build_dir: str) -> list[dict]:
    r = subprocess.run(["ctest", "--test-dir", build_dir, "--show-only=json-v1"], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"ctest --show-only failed in {build_dir}: {r.stderr.strip() or r.stdout.strip()}", file=sys.stderr)
        sys.exit(2)
    return json.loads(r.stdout).get("tests", [])


def prop(test: dict, name: str):
    for p in test.get("properties", []):
        if p.get("name") == name:
            return p.get("value")
    return None


def strip_options(cmd: list[str]) -> list[str]:
    """The test command without its comparison options: --golden F, --check-workers, --crc-out F, --profile-out F."""
    out, i = [], 0
    while i < len(cmd):
        a = cmd[i]
        if a in ("--golden", "--crc-out", "--profile-out", "--crc-checkpoint", "--crc-keep"):
            i += 2
            continue
        if a in ("--check-workers", "--profile"):
            i += 1
            continue
        out.append(a)
        i += 1
    return out


def headless_jobs(build_dir: str, checkpoint: int) -> list[dict]:
    """One job per checked-in headless golden: the first ctest entry (by name) that passes --golden <that file>."""
    want = {norm(os.path.join(ROOT, p)): p for p in sorted(
        f"{HEADLESS_DIR}/{n}" for n in os.listdir(os.path.join(ROOT, HEADLESS_DIR)) if n.endswith(".crc"))}
    jobs: dict[str, dict] = {}
    for t in sorted(ctest_json(build_dir), key=lambda t: t["name"]):
        cmd = t.get("command") or []
        if "--golden" not in cmd:
            continue
        g = norm(cmd[cmd.index("--golden") + 1])
        if g not in want or g in jobs:
            continue
        rel = want[g]
        every, keep = checkpoint, []
        try:
            old = cg.parse(open(os.path.join(ROOT, rel), encoding="utf-8", newline="").read())
            if old.checkpoint:
                every, keep = old.checkpoint, old.keep
        except (OSError, cg.CrcError):
            pass
        jobs[g] = {"test": t["name"], "golden": rel, "cmd": strip_options(cmd), "cwd": prop(t, "WORKING_DIRECTORY") or build_dir,
                   "every": every, "keep": keep}
    missing = [rel for g, rel in want.items() if g not in jobs]
    return [jobs[g] for g in sorted(jobs, key=lambda g: want[g])], missing


def run(cmd, cwd, env, log):
    print("  $ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    log.append((cmd, r.stdout + r.stderr))
    return r


def summarize(rel: str, old_text: str | None, new_text: str) -> tuple[str, str]:
    """(status, one or more lines) for the diff summary."""
    if old_text is None:
        return "new", f"{rel}: new file"
    if old_text == new_text:
        return "unchanged", f"{rel}: unchanged"
    try:
        old, new = cg.parse(old_text), cg.parse(new_text)
    except cg.CrcError as e:
        return "changed", f"{rel}: changed (old file unreadable: {e})"
    head = f"{rel}: CHANGED, {len(old.records)} -> {len(new.records)} records, ticks {old.records[-1].tick if old.records else '-'} -> {new.records[-1].tick if new.records else '-'}"
    if old.systems != new.systems or old.parity_parts != new.parity_parts:
        return "changed", head + "\n    columns changed: " + " ".join(old.systems) + "  ->  " + " ".join(new.systems)
    # Compare on the ticks both files store; the first checkpoint that differs and how many do.
    new_by = {r.tick: r for r in new.records}
    both = [(r, new_by[r.tick]) for r in old.records if r.tick in new_by]
    diffs = [(a, b) for a, b in both if a.key() != b.key()]
    if (not diffs and old.records and new.records and old.records[-1].tick == new.records[-1].tick and old.comments == new.comments
            and set(new_by) <= {r.tick for r in old.records}):
        return "format", (f"{rel}: values unchanged on all {len(both)} stored checkpoints "
                          f"(format {'v2' if old.checkpoint else 'v1'} {len(old.records)} records -> v2 {len(new.records)} records)")
    lines = [head]
    if diffs:
        a, b = diffs[0]
        idx = [r.tick for r, _ in both].index(a.tick)
        prev = both[idx - 1][0].tick if idx else None
        cols = [new.systems[i] for i in range(len(a.systems)) if a.systems[i] != b.systems[i]]
        if a.parity != b.parity:
            cols.append("parity")
        if a.combined != b.combined:
            cols.append("combined")
        window = f"{prev + 1}..{a.tick}" if prev is not None else str(a.tick)
        lines.append(f"    first changed checkpoint: tick {a.tick} (the first differing tick is in {window}); columns: {', '.join(cols)}")
        lines.append(f"    {len(diffs)} of {len(both)} shared checkpoints differ")
    else:
        lines.append("    every shared checkpoint is equal (only the run length / stored ticks / comments changed)")
    olds, news = {r.tick for r in old.records}, set(new_by)
    if olds != news and old.checkpoint:  # (an old full log always has more ticks than the checkpoint log)
        lines.append(f"    stored ticks: {len(olds - news)} only in the old file, {len(news - olds)} only in the new one")
    if old.comments != new.comments:
        lines.append("    comments: " + " | ".join(old.comments) + "  ->  " + " | ".join(new.comments))
    return "changed", "\n".join(lines)


BENCH_DIR = "tests/benchmark"


def bench_version(path: str) -> str:
    try:
        for line in open(path, encoding="utf-8").read().splitlines():
            if line.startswith("version "):
                return line.split()[1]
    except OSError:
        pass
    return "?"


def bench_host(path: str) -> str:
    """The manifest's `host` line: "headless" (alamo_headless --bench) or "game" (alamo_game, rendered; BM2/BM3)."""
    try:
        for line in open(path, encoding="utf-8").read().splitlines():
            if line.startswith("host "):
                return line.split()[1]
    except OSError:
        pass
    return "headless"


def bench_scenarios() -> list[str]:
    d = os.path.join(ROOT, BENCH_DIR)
    return sorted(f"{BENCH_DIR}/{n}" for n in os.listdir(d) if n.endswith(".abench")) if os.path.isdir(d) else []


def bench_step(build_dir: str, build_arg: str, check_only: bool, listing: bool, env, logs, summaries) -> int:
    """Check every benchmark scenario; remake the stale ones (unless check_only). Returns the number of failed runs; stale
    scenarios found by a check-only pass are counted in the second result via summaries status 'stale'."""
    scenarios = bench_scenarios()
    exe = os.path.join(build_dir, "bin", "alamo_headless.exe")
    game_exe = os.path.join(build_dir, "bin", "alamo_game.exe")
    print(f"benchmark scenarios ({len(scenarios)}):")
    for rel in scenarios:
        print(f"  {rel}")
    if listing or not scenarios:
        return 0
    if not os.path.isfile(exe):
        print(f"  FAILED: {exe} not found (build first)")
        return 1
    failed = 0
    for rel in scenarios:
        print(f"{rel}:")
        path = os.path.join(ROOT, rel)
        v0 = bench_version(path)
        is_game = bench_host(path) == "game"
        if is_game and not check_only and not os.path.isfile(game_exe):  # the remake of a stale game scene needs alamo_game
            failed += 1
            print(f"  FAILED: {game_exe} not found (build first)")
            continue
        # A game scene is checked like any other: sim-only and full length by alamo_headless --bench (bm2c t02; no GPU, no window),
        # remade by the rendered alamo_game --hidden --bench-regen. (alamo_game --bench-scenario also answers a stale scene with 3.)
        r = run([exe, "--bench", path, "--runs", "1", "--warmup", "0", "--quiet"], ROOT, env, logs)
        if r.returncode == 0:
            summaries.append(("unchanged", f"{rel}: scenario fingerprint unchanged (version {v0})"))
            continue
        if r.returncode != 3:
            failed += 1
            print(f"  FAILED: exit {r.returncode}\n" + "\n".join("    " + l for l in (r.stdout + r.stderr).strip().splitlines()[-8:]))
            continue
        why = next((l.split("scenario changed, not comparable:", 1)[1].strip() for l in r.stdout.splitlines()
                    if "scenario changed, not comparable:" in l), "fingerprint differs")
        if check_only:
            summaries.append(("stale", f"{rel}: STALE (version {v0}): {why}"))
            continue
        scen = os.path.splitext(os.path.basename(rel))[0]
        if is_game:
            rr = run([game_exe, "--hidden", "--bench-regen", path], ROOT, env, logs)
        else:
            rr = run([os.path.join(ROOT, "tools", "benchmark", "regen_scenario.cmd"), scen, build_arg], ROOT, env, logs)
        if rr.returncode != 0:
            failed += 1
            print(f"  FAILED: regen_scenario exit {rr.returncode}\n" + "\n".join("    " + l for l in (rr.stdout + rr.stderr).strip().splitlines()[-8:]))
            continue
        v1 = bench_version(path)
        if v0 == v1:  # the remake produced the same files (a damaged or hand-edited sidecar was restored)
            summaries.append(("unchanged", f"{rel}: was stale ({why}); remade, files and version {v0} unchanged"))
        else:
            summaries.append(("changed", f"{rel}: CHANGED, regenerated, version {v0} -> {v1} ({why}); commit tests/benchmark/{scen}.*"))
    return failed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default="build-msvc")
    ap.add_argument("--only", choices=("headless", "game", "all", "bench"), default="headless")
    ap.add_argument("--bench-check", action="store_true", help="only report stale benchmark scenarios (implies --only bench; exit 4 when stale)")
    ap.add_argument("--no-bench", action="store_true", help="leave the benchmark scenarios (tests/benchmark) alone")
    ap.add_argument("--checkpoint", type=int, default=cg.DEFAULT_CHECKPOINT)
    ap.add_argument("--force", action="store_true", help="run although tracked files have uncommitted edits")
    ap.add_argument("--dry-run", action="store_true", help="run, print the summary, write no golden")
    ap.add_argument("--list", action="store_true", help="print what would run and stop")
    ap.add_argument("--match", default=None, help="regex: only the headless goldens whose file name matches (e.g. 'replay|synthetic')")
    args = ap.parse_args()
    if args.bench_check:
        args.only = "bench"

    dirty = tracked_changes()
    if dirty and not args.force and not args.list:
        print("regen_goldens: refusing to run, tracked files have uncommitted edits (commit first so the golden diff comes from the "
              "committed code, or pass --force):", file=sys.stderr)
        for line in dirty[:20]:
            print("  " + line, file=sys.stderr)
        if len(dirty) > 20:
            print(f"  ... and {len(dirty) - 20} more", file=sys.stderr)
        return 3

    build_dir = args.build_dir if os.path.isabs(args.build_dir) else os.path.join(ROOT, args.build_dir)
    if not os.path.isfile(os.path.join(build_dir, "CTestTestfile.cmake")):
        print(f"regen_goldens: {build_dir} is not a configured build tree (run build_msvc.cmd first)", file=sys.stderr)
        return 2
    tmp = os.path.join(build_dir, "tmp", "regen_goldens")
    os.makedirs(tmp, exist_ok=True)
    env = dict(os.environ, TMP=tmp, TEMP=tmp)
    logs: list = []
    summaries: list[tuple[str, str]] = []
    failed = 0

    if args.only in ("headless", "all"):
        jobs, missing = headless_jobs(build_dir, args.checkpoint)
        if args.match:
            jobs = [j for j in jobs if re.search(args.match, os.path.basename(j["golden"]))]
            missing = []
        for rel in missing:
            print(f"regen_goldens: no ctest entry with --golden {rel}: left alone", file=sys.stderr)
        print(f"headless goldens ({len(jobs)}):")
        for j in jobs:
            print(f"  {j['golden']}  <- ctest {j['test']}, checkpoint {j['every']}" + (f", keep {j['keep']}" if j["keep"] else ""))
        if args.list:
            pass
        else:
            for j in jobs:
                out = os.path.join(tmp, os.path.basename(j["golden"]))
                if os.path.exists(out):
                    os.remove(out)
                cmd = list(j["cmd"])
                if "--workers" not in cmd:
                    cmd += ["--workers", "0"]
                cmd += ["--crc-out", out, "--crc-checkpoint", str(j["every"])]
                if j["keep"]:
                    cmd += ["--crc-keep", ",".join(map(str, j["keep"]))]
                print(f"{j['golden']}:")
                r = run(cmd, j["cwd"], env, logs)
                if r.returncode != 0 or not os.path.isfile(out):
                    failed += 1
                    why = f"exit {r.returncode}" if r.returncode != 0 else "no output written (game data missing? ALAMO_FOC_DATA)"
                    print(f"  FAILED: {why}\n" + "\n".join("    " + l for l in (r.stdout + r.stderr).strip().splitlines()[-8:]))
                    continue
                new_text = open(out, encoding="utf-8", newline="").read()
                path = os.path.join(ROOT, j["golden"])
                old_text = open(path, encoding="utf-8", newline="").read() if os.path.isfile(path) else None
                status, text = summarize(j["golden"], old_text, new_text)
                summaries.append((status, text))
                if status != "unchanged" and not args.dry_run:
                    with open(path, "w", encoding="utf-8", newline="") as f:
                        f.write(new_text)

    if args.only in ("game", "all"):
        have_game = any(t["name"] == "alamo_game_smoke_skirmish" for t in ctest_json(build_dir))
        print("game smoke goldens: " + ("alamo_game_smoke_{skirmish,campaign,land}" if have_game else "no alamo_game smoke tests in this tree"))
        if not have_game:
            if args.only == "game":
                failed += 1
        elif not args.list:
            smoke_out = os.path.join(build_dir, "smoke_out")
            for k in SMOKES:
                for ext in (".crc", ".rec"):
                    p = os.path.join(smoke_out, f"game_smoke_{k}{ext}")
                    if os.path.exists(p):
                        os.remove(p)
            r = run(["ctest", "--test-dir", build_dir, "-R", "^alamo_game_smoke_(skirmish|campaign|land)$", "--output-on-failure"], build_dir, env, logs)
            print("\n".join("    " + l for l in r.stdout.strip().splitlines()[-12:]))
            for k in SMOKES:
                base = os.path.join(smoke_out, f"game_smoke_{k}")
                if not (os.path.isfile(base + ".crc") and os.path.isfile(base + ".rec")):
                    failed += 1
                    print(f"  FAILED: game_smoke_{k}: the smoke run wrote no .crc / .rec (no GPU or game data, or the run failed)")
                    continue
                gpath = os.path.join(ROOT, GAME_DIR, f"game_smoke_{k}")
                try:
                    old_crc = open(gpath + ".crc", encoding="utf-8", newline="").read()
                    old = cg.parse(old_crc)
                except (OSError, cg.CrcError):
                    old_crc, old = None, None
                every = old.checkpoint if old and old.checkpoint else args.checkpoint
                keep = old.keep if old and old.checkpoint else []
                full = cg.parse(open(base + ".crc", encoding="utf-8", newline="").read())
                new_text = cg.to_text(cg.convert(full, every, keep))
                status, text = summarize(f"{GAME_DIR}/game_smoke_{k}.crc", old_crc, new_text)
                summaries.append((status, text))
                new_rec = open(base + ".rec", "rb").read()
                old_rec = open(gpath + ".rec", "rb").read() if os.path.isfile(gpath + ".rec") else None
                summaries.append(("unchanged" if new_rec == old_rec else "changed",
                                  f"{GAME_DIR}/game_smoke_{k}.rec: " + ("unchanged" if new_rec == old_rec else f"CHANGED ({len(old_rec or b'')} -> {len(new_rec)} bytes)")))
                if not args.dry_run:
                    if status != "unchanged":
                        with open(gpath + ".crc", "w", encoding="utf-8", newline="") as f:
                            f.write(new_text)
                    if new_rec != old_rec:
                        with open(gpath + ".rec", "wb") as f:
                            f.write(new_rec)

    if args.only in ("headless", "all", "bench") and not (args.no_bench and args.only != "bench"):
        build_arg = os.path.relpath(build_dir, ROOT) if not os.path.isabs(args.build_dir) else build_dir
        failed += bench_step(build_dir, build_arg, args.dry_run or args.bench_check, args.list, env, logs, summaries)

    print()
    print("=== diff summary" + (" (dry run: nothing written)" if args.dry_run else "") + " ===")
    for _, text in summaries:
        print(text)
    stale = [s for s in summaries if s[0] == "stale"]
    if stale:
        print(f"{len(stale)} benchmark scenario(s) stale: run tools/regen_goldens.cmd (or tools/benchmark/regen_scenario.cmd) to remake them")
    changed = [s for s in summaries if s[0] in ("changed", "new")]
    fmt = [s for s in summaries if s[0] == "format"]
    print(f"{len(changed)} of {len(summaries)} files changed in value" + (f", {len(fmt)} only in format (full log -> checkpoint log)" if fmt else "")
          + (f", {failed} run(s) FAILED" if failed else ""))
    if changed:
        print("State the intended behaviour change in the PR from the first changed checkpoint of each file; for the exact tick run the old "
              "and the new build with --crc-out full.crc and `alamo_headless --compare old.crc new.crc`.")
    if failed:
        return 1
    return 4 if stale and args.bench_check else 0


if __name__ == "__main__":
    sys.exit(main())
