"""Repeatable build-time baseline (roadmap bo1 t01).

Measures, in a throw-away MSVC build tree with the compiler cache OFF (so the numbers are real compile time):
  clean     configure + build of every target (no tests)
  cpp       one-line edit of one .cpp (engine/vfs/src/vfs.cpp unless --cpp), rebuild
  header    one-line edit of one core header (engine/core/include/alamo/core/hash.hpp unless --header), rebuild
Then prints the slowest files from .ninja_log. The edits are appended comments and are reverted afterwards.

  python tools/benchmark/build_times.py [--tree NAME] [--steps clean,cpp,header] [--cache on] [--json out.json]

The tree is build-bench-<NAME>; it is deleted before the clean step. Takes as long as the builds do (minutes).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TC = ROOT / "toolchain"


def run_env(cache_on):
    """Environment after setup_x64.bat (cl on PATH), ninja on PATH."""
    out = subprocess.run(
        f'cmd /c ""{TC / "msvc" / "setup_x64.bat"}" >nul && set"',
        capture_output=True, text=True, shell=True, check=True).stdout
    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    env["PATH"] = str(TC / "venv" / "Scripts") + os.pathsep + env.get("PATH", "")
    if not cache_on:
        env["ALAMO_COMPILER_CACHE"] = "OFF"
    else:
        env.setdefault("SCCACHE_DIR", str(TC / "sccache" / "cache"))
    return env


def timed(cmd, env):
    cmd = [shutil.which(cmd[0], path=env["PATH"]) or cmd[0]] + cmd[1:]
    t0 = time.perf_counter()
    r = subprocess.run(cmd, env=env, cwd=ROOT, capture_output=True, text=True)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-3000:] + r.stderr[-2000:])
        raise SystemExit(f"command failed: {cmd}")
    return dt, r.stdout


def ninja_stats(tree):
    """Slowest compile/link steps of the last build in the tree's .ninja_log (start, end in ms)."""
    rows = []
    log = tree / ".ninja_log"
    for line in log.read_text().splitlines():
        if line.startswith("#"):
            continue
        p = line.split("\t")
        if len(p) >= 4:
            rows.append((int(p[1]) - int(p[0]), p[3]))
    rows.sort(reverse=True)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="base")
    ap.add_argument("--steps", default="clean,cpp,header")
    ap.add_argument("--cache", default="off", choices=["on", "off"])
    ap.add_argument("--cpp", default="engine/vfs/src/vfs.cpp")
    ap.add_argument("--header", default="engine/core/include/alamo/core/hash.hpp")
    ap.add_argument("--configure-extra", default="", help="extra cmake -D arguments, space separated")
    ap.add_argument("--json", default="")
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args()

    env = run_env(a.cache == "on")
    tree = ROOT / f"build-bench-{a.tree}"
    steps = a.steps.split(",")
    res = {"tree": a.tree, "cache": a.cache, "steps": {}}
    cfg = ["cmake", "-S", ".", "-B", str(tree), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           f"-DCMAKE_MAKE_PROGRAM={TC / 'venv' / 'Scripts' / 'ninja.exe'}", "-DCMAKE_CXX_COMPILER=cl"]
    cfg += a.configure_extra.split()

    if "clean" in steps:
        shutil.rmtree(tree, ignore_errors=True)
        dt1, _ = timed(cfg, env)
        dt2, _ = timed(["cmake", "--build", str(tree)], env)
        res["steps"]["clean"] = {"configure_s": round(dt1, 1), "build_s": round(dt2, 1)}
        top = ninja_stats(tree)
        res["top_files_ms"] = top[: a.top]
        print(f"clean: configure {dt1:.1f} s, build {dt2:.1f} s")
        print("slowest steps (ms):")
        for ms, name in top[: a.top]:
            print(f"  {ms:7d}  {name}")

    for step, path in (("cpp", a.cpp), ("header", a.header)):
        if step not in steps:
            continue
        f = ROOT / path
        orig = f.read_bytes()
        try:
            f.write_bytes(orig + b"\n// bench edit\n")
            dt, out = timed(["cmake", "--build", str(tree)], env)
            n = len(re.findall(r"^\[\d+/\d+\]", out, re.M))
            res["steps"][step] = {"build_s": round(dt, 1), "ninja_steps": n}
            print(f"{step} edit ({path}): build {dt:.1f} s, {n} ninja steps")
        finally:
            f.write_bytes(orig)
        timed(["cmake", "--build", str(tree)], env)  # back to the unedited state

    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
