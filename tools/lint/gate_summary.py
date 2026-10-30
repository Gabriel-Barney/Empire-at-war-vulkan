"""Summarise the logs of build_msvc / build_clang / build_asan for build_gate.cmd.

Prints one line per build and exits 1 when any build did not finish, any step exited non-zero, or a warning was logged.
Usage: gate_summary.py <logs dir>
"""
import re
import sys
from pathlib import Path

BUILDS = [('msvc', 'msvc_configure.txt', 'msvc_build.txt'),
          ('clang', 'clang_configure.txt', 'clang_build.txt'),
          ('asan', 'asan_configure.txt', 'asan_build.txt')]


def read(p):
    try:
        return p.read_text(encoding='utf8', errors='replace')
    except OSError:
        return None


def main(logs):
    bad = False
    for name, cfg, bld in BUILDS:
        c, b = read(logs / cfg), read(logs / bld)
        if c is None or b is None or 'BUILD_DONE' not in c:
            print(f'{name:6} FAIL  did not finish (missing log or no BUILD_DONE)')
            bad = True
            continue
        text = c + '\n' + b
        exits = dict(re.findall(r'^(configure|build|ctest) exit (-?\d+)\s*$', text, re.M))
        steps_ok = all(exits.get(s) == '0' for s in ('configure', 'build', 'ctest'))
        warn = len({l.strip() for l in text.splitlines() if re.search(r'\bwarning( [A-Z]+\d+)?:', l)})
        total = re.search(r'tests passed(?:, \d+ tests failed)? out of (\d+)', text)
        skips = re.search(r'^skips: .*$', text, re.M)
        ok = steps_ok and warn == 0
        bad |= not ok
        print(f"{name:6} {'PASS' if ok else 'FAIL'}  exits={exits} warnings={warn} "
              f"tests={total.group(1) if total else '?'}  {skips.group(0) if skips else 'skips: not reported'}")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(Path(sys.argv[1])))
