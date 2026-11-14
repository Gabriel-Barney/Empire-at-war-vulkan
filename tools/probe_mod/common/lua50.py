"""lua50.py - the stock Lua 5.0.2 interpreter the probe-kit selftests run their hooked scripts under (cu18 t05,
2026-11-07: one build script and one lookup instead of five copies and three output folders).

LUA50 = tools\\probe_mod\\out\\tools\\lua50.exe, built by common\\build_lua50.cmd (project-local MSVC at /Od) on first use.
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), 'out', 'tools')
LUA50 = os.path.join(TOOLS, 'lua50.exe')
BUILD_LOG = os.path.join(TOOLS, 'lua50_build.txt')
BUILD_CMD = os.path.join(HERE, 'build_lua50.cmd')


def find_lua(build=True):
    """Path of lua50.exe, building it first when missing and `build` is set; None when it is not there (no toolchain,
    or the build failed: see BUILD_LOG)."""
    if not os.path.isfile(LUA50) and build:
        subprocess.run(['cmd', '/c', BUILD_CMD], capture_output=True)
    return LUA50 if os.path.isfile(LUA50) else None
