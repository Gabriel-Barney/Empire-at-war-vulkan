"""Packs a test build zip for Gabriel: alamo_game + alamo_mod_check (with the DXC, FFmpeg and FSR runtime DLLs) from a
Release build tree, the launchers in
tools/testbuild/launchers (Play.cmd for Forces of Corruption, Play_Mod.cmd for any installed mod), Play.exe (Play.cmd
without a console window, built by build_msvc.cmd),
tools/testbuild/CHANGES.txt and THIRD_PARTY_NOTICES.txt (the license of every third-party component in the zip, read
from toolchain/, plus where to get the exact FFmpeg source and build options the LGPL asks for).

Usage (from the project root; tools/testbuild/package_testbuild.cmd wraps it):
  python -I tools/testbuild/package_testbuild.py [--build-dir build-msvc] [--name NAME] [--smoke] [--allow-dirty]

  --name         zip name without .zip (default alamo-testbuild-<today>, then -<today>b, c, ... if taken)
  --smoke        before zipping, run the Thrawn's Revenge mod check and skirmish smoke through the staged Play_Mod.cmd
                 (needs Forces of Corruption, Thrawn's Revenge and a GPU); a failure stops the packaging
  --allow-dirty  package even when engine/ has uncommitted edits (the zip's BUILD_INFO.txt says so)

Writes dist/<name>.zip (dist/ is git-ignored). CHANGES.txt's @BUILD@ becomes "<branch> <commit>, MSVC <build type>".
Edit tools/testbuild/CHANGES.txt for each build: what changed, what to check, known gaps.
"""
import argparse
import datetime
import os
import pathlib
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = ROOT / "tools" / "testbuild"
BINARIES = ["Play.exe", "alamo_game.exe", "alamo_mod_check.exe", "avcodec-62.dll", "avformat-62.dll", "avutil-60.dll",
            "dxcompiler.dll", "dxil.dll", "amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll",
            "COPYING.LGPLv2.1"]
LAUNCHERS = ["Play.cmd", "Play_Mod.cmd", "find_game.cmd", "launcher.ps1"]
SMOKE_MOD = "1125571106"  # Thrawn's Revenge (Steam workshop), the mod the smoke plays
TOOLCHAIN = ROOT / "toolchain"


def git(*args):
    r = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def build_type(build_dir):
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return ""
    m = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache.read_text(encoding="utf-8", errors="replace"), re.M)
    return m.group(1).strip() if m else ""


def pick_name(dist, wanted):
    if wanted:
        return wanted
    base = "alamo-testbuild-" + datetime.date.today().isoformat()
    for suffix in [""] + [chr(c) for c in range(ord("b"), ord("z") + 1)]:
        if not (dist / (base + suffix + ".zip")).exists():
            return base + suffix
    sys.exit("no free name for " + base)


def crlf(data):
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def one_dir(pattern):
    """The single toolchain folder matching pattern (e.g. lua-*), so a version bump needs no edit here."""
    hits = sorted(p for p in TOOLCHAIN.glob(pattern) if p.is_dir())
    if len(hits) != 1:
        sys.exit("expected one toolchain folder matching %s, found %d" % (pattern, len(hits)))
    return hits[0]


def cmd_value(script, pattern):
    """First group of pattern in a toolchain setup script (the pinned URLs and hashes live there)."""
    m = re.search(pattern, (TOOLCHAIN / script).read_text(encoding="utf-8", errors="replace"), re.M)
    if not m:
        sys.exit("toolchain/%s: no match for %s" % (script, pattern))
    return m.group(1)


def third_party_notices():
    """Text of THIRD_PARTY_NOTICES.txt: each third-party component in the zip with its full license text from
    toolchain\\. The MIT-style licenses want their notice in binary copies; the LGPL wants the FFmpeg source location
    and build options."""
    lua, mini, ffmpeg = one_dir("lua-*"), one_dir("miniaudio-*"), one_dir("ffmpeg-*")
    ff_ver = cmd_value("9_ffmpeg_bink.cmd", r"^set FF_VER=(\S+)")
    ff_url = cmd_value("9_ffmpeg_bink.cmd", r"^set FF_URL=(\S+)").replace("%FF_VER%", ff_ver)
    ff_sha = cmd_value("9_ffmpeg_bink.cmd", r"^set FF_SHA=(\S+)")
    config = (ffmpeg / "BUILD_CONFIG.txt").read_text(encoding="utf-8", errors="replace")
    m = re.search(r'FFMPEG_CONFIGURATION "(.*)"', config)
    if not m:
        sys.exit("%s: no FFMPEG_CONFIGURATION line" % (ffmpeg / "BUILD_CONFIG.txt"))
    ff_options = " ".join(o for o in m.group(1).split() if not o.startswith("--prefix="))  # drop the temp install path
    vma = cmd_value("3_renderer_deps.cmd", r"^set VMA_URL=.*/v([\d.]+)/")
    vkh = cmd_value("3_renderer_deps.cmd", r"^set VKH_URL=.*vulkan-sdk-(\d+(?:\.\d+)*)")
    dxc = cmd_value("3_renderer_deps.cmd", r"^set DXC_URL=.*/download/v([\d.]+)/")
    vk = TOOLCHAIN / "vulkan-headers"
    parts = [  # (name, where it is in the zip, license files)
        ("Lua " + lua.name.split("-", 1)[1], "compiled into alamo_game.exe and alamo_mod_check.exe", [lua / "COPYRIGHT"]),
        ("miniaudio " + mini.name.split("-", 1)[1], "compiled into alamo_game.exe", [mini / "LICENSE"]),
        ("Vulkan Memory Allocator " + vma, "compiled into alamo_game.exe", [TOOLCHAIN / "vma" / "LICENSE.txt"]),
        ("Vulkan-Headers (Vulkan SDK %s)" % vkh, "compiled into the executables",
         [vk / "LICENSE.md", vk / "LICENSES" / "MIT.txt", vk / "LICENSES" / "Apache-2.0.txt"]),
        ("DirectX Shader Compiler " + dxc, "dxcompiler.dll and dxil.dll, unmodified",
         [TOOLCHAIN / "dxc" / n for n in ("LICENSE-LLVM.txt", "LICENSE-MIT.txt", "LICENSE-MS.txt")]),
        ("SMAA (reference implementation)", "shaders compiled into alamo_game.exe",
         [ROOT / "engine" / "render" / "third_party" / "smaa" / "LICENSE.txt"]),
        ("AMD FidelityFX SDK " + one_dir("fidelityfx-*").name.split("-", 1)[1],
         "amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (FSR upscaling), unmodified",
         [one_dir("fidelityfx-*") / "license.md"]),
    ]
    ff_title = "FFmpeg %s: avcodec-62.dll, avformat-62.dll, avutil-60.dll" % ff_ver
    rule = "=" * 78
    out = ["THIRD-PARTY NOTICES", "",
           "The Alamo engine's own code is released into the public domain (the Unlicense). This build also contains",
           "the third-party software below, each under its own license. The game, its assets and its trademarks belong",
           "to their owners and are not part of this build.", ""]
    out += ["%d. %s: %s" % (i, name, where) for i, (name, where, _) in enumerate(parts, 1)]
    out.append("%d. %s" % (len(parts) + 1, ff_title))
    for name, where, files in parts:
        out += ["", rule, "%s: %s" % (name, where), rule]
        for f in files:
            if not f.exists():
                sys.exit("missing license file " + str(f))
            out += ["", "--- %s ---" % f.name, "", f.read_text(encoding="utf-8-sig", errors="replace").rstrip()]
    out += ["", rule, ff_title, rule, "",
            "License: GNU Lesser General Public License, version 2.1 or later. The full text is COPYING.LGPLv2.1 in",
            "this zip. Only FFmpeg's Bink video and audio decoders and its Bink demuxer are built in; no GPL, version 3",
            "or non-free parts are enabled. The FFmpeg source was not modified.",
            "",
            "Corresponding source: the unmodified FFmpeg %s release tarball" % ff_ver,
            "  " + ff_url,
            "  SHA-256 " + ff_sha,
            "built as shared libraries with Microsoft Visual C++ 14.51 (x64) and these configure options:",
            "  " + ff_options,
            "",
            "The engine loads these DLLs at run time and links no FFmpeg code into its own executables. You may",
            "replace them with your own build of FFmpeg %s (same DLL names), for example one built from that source." % ff_ver]
    return "\n".join(out) + "\n"


def run_cmd(script, args=(), env=None):
    """Runs a .cmd with cmd's /s quoting (a quoted script path plus quoted arguments with spaces)."""
    line = " ".join(['"%s"' % script] + ['"%s"' % x if " " in x else x for x in args])
    return subprocess.run('cmd /s /c "%s"' % line, stdin=subprocess.DEVNULL, capture_output=True, text=True, env=env)


def smoke(stage, out_dir):
    """Mod check + TR skirmish smoke through the staged Play_Mod.cmd. Returns an error text or ''."""
    out_dir.mkdir(parents=True, exist_ok=True)
    run_dir = stage / "mod_runs" / ("ws" + SMOKE_MOD)
    env = dict(os.environ, ALAMO_NO_NOTEPAD="1")
    check = run_cmd(stage / "Play_Mod.cmd", ["C" + SMOKE_MOD], env=env)
    report = run_dir / "mod_check.txt"
    print(check.stdout.strip())
    if "Exit code 0." not in check.stdout or not report.exists():
        return "mod check failed: see " + str(report)
    env["ALAMO_ARGS"] = '--smoke skirmish --png-dir "%s" --hidden' % out_dir
    run = run_cmd(stage / "Play_Mod.cmd", [SMOKE_MOD], env=env)
    log = run_dir / "log.txt"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    if run.returncode != 0 or "smoke skirmish: OK" not in text:
        return "TR skirmish smoke failed (exit %d): see %s\n%s" % (run.returncode, log, run.stdout.strip())
    print("TR skirmish smoke: OK (shots in %s)" % out_dir)
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build-msvc")
    ap.add_argument("--name", default="")
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args()

    build_dir = (ROOT / a.build_dir).resolve()
    bin_dir = build_dir / "bin"
    btype = build_type(build_dir)
    if btype != "Release":
        sys.exit("%s is not a Release build tree (CMAKE_BUILD_TYPE=%s); run build_msvc.cmd" % (build_dir, btype or "?"))
    missing = [f for f in BINARIES if not (bin_dir / f).exists()]
    if missing:
        sys.exit("missing in %s: %s" % (bin_dir, ", ".join(missing)))
    dirty = git("status", "--porcelain", "--", "engine")
    if dirty and not a.allow_dirty:
        sys.exit("engine/ has uncommitted edits; commit them (or pass --allow-dirty)\n" + dirty)
    commit, branch = git("rev-parse", "--short", "HEAD"), git("branch", "--show-current") or "detached"
    exe_time = datetime.datetime.fromtimestamp((bin_dir / "alamo_game.exe").stat().st_mtime)

    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    name = pick_name(dist, a.name)
    zip_path = dist / (name + ".zip")
    if zip_path.exists():
        sys.exit(str(zip_path) + " exists; pass another --name")

    stage = build_dir / "testbuild" / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for f in BINARIES:
        shutil.copy2(bin_dir / f, stage / f)
    for f in LAUNCHERS:
        (stage / f).write_bytes(crlf((HERE / "launchers" / f).read_bytes()))
    build = "%s %s%s, MSVC %s" % (branch, commit, " + uncommitted edits" if dirty else "", btype.lower())
    changes = (HERE / "CHANGES.txt").read_text(encoding="utf-8").replace("@BUILD@", build)
    (stage / "CHANGES.txt").write_bytes(crlf(changes.encode("utf-8")))
    info = ["Alamo engine test build " + name, "commit: " + git("rev-parse", "HEAD"), "branch: " + branch,
            "build type: " + btype, "alamo_game.exe built: " + exe_time.strftime("%Y-%m-%d %H:%M"),
            "uncommitted engine edits: " + ("yes" if dirty else "no"),
            "packed: " + datetime.datetime.now().strftime("%Y-%m-%d %H:%M")]
    (stage / "BUILD_INFO.txt").write_bytes(crlf(("\n".join(info) + "\n").encode("utf-8")))
    (stage / "THIRD_PARTY_NOTICES.txt").write_bytes(crlf(third_party_notices().encode("utf-8")))
    files = BINARIES + LAUNCHERS + ["CHANGES.txt", "BUILD_INFO.txt", "THIRD_PARTY_NOTICES.txt"]

    if a.smoke:
        err = smoke(stage, build_dir / "testbuild" / (name + "_smoke"))
        if err:
            sys.exit(err)

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:  # only the packed files: the smoke run's logs, caches and saves stay out
            z.write(stage / f, f)
    print("%s: %d files, %.1f MB (%s)" % (zip_path, len(files), zip_path.stat().st_size / 1e6, build))


if __name__ == "__main__":
    main()
