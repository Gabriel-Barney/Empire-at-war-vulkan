"""Summarise the logs of build_msvc / build_clang / build_asan for build_gate.cmd.

Prints one line per build and exits 1 when any build did not finish, any step exited non-zero, or a warning was logged.
The step-exit, test-total and skips patterns come from build_summary.py. The warning rule here is deliberately narrower
than build_summary's (compiler / linker "warning:" lines only): build_summary also counts error-like lines anywhere in the
log, including the output of passing tests, which would fail the gate on test output.
Then runs the source lints that need no build (modc t04: check_mod_names.py and its self-test, docs/status/modc-audit.md) and
prints one line for them; a lint failure fails the gate too. (The same lints also run as ctest entries in every build.)
Usage: gate_summary.py <logs dir>
"""
import re
import subprocess
import sys
from pathlib import Path

from build_summary import SKIPS, STEP_EXIT, TESTS_TOTAL

LINTS = [('mod names', ['check_mod_names.py']), ('mod names selftest', ['check_mod_names.py', '--selftest'])]


def run_lints():
    here = Path(__file__).resolve().parent
    bad = False
    parts = []
    for name, args in LINTS:
        r = subprocess.run([sys.executable, str(here / args[0])] + args[1:], capture_output=True, text=True)
        lines = (r.stdout or '').strip().splitlines()
        last = lines[-1] if lines else (r.stderr or '').strip()
        parts.append(f'{name}: {"ok" if r.returncode == 0 else "FAIL"}')
        if r.returncode != 0:
            bad = True
            for line in lines[:20]:
                print(f'       {line}')
        elif name == 'mod names':
            parts[-1] = f'{name}: {last}'
    print(f"lint   {'FAIL' if bad else 'PASS'}  " + '; '.join(parts))
    return bad

BUILDS = [('msvc', 'msvc_configure.txt', 'msvc_build.txt'),
          ('clang', 'clang_configure.txt', 'clang_build.txt'),
          ('asan', 'asan_configure.txt', 'asan_build.txt')]


def read(p):
    try:
        return p.read_text(encoding='utf8', errors='replace')
    except OSError:
        return None


def skipped_builds(logs):
    """Builds gate_scope.py (build_gate.cmd, bo1 t05) decided the change does not need: {name: reason}."""
    out = {}
    text = read(logs / 'gate_scope.txt') or ''
    for line in text.splitlines():
        m = re.match(r'skip\s+(\w+): (.*)', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def main(logs):
    bad = False
    skipped = skipped_builds(logs)
    for name, cfg, bld in BUILDS:
        if name in skipped:
            print(f'{name:6} SKIP  not needed for this change ({skipped[name]})')
            continue
        c, b = read(logs / cfg), read(logs / bld)
        if c is None or b is None or 'BUILD_DONE' not in c:
            print(f'{name:6} FAIL  did not finish (missing log or no BUILD_DONE)')
            bad = True
            continue
        text = c + '\n' + b
        exits = dict(STEP_EXIT.findall(text))
        steps_ok = all(exits.get(s) == '0' for s in ('configure', 'build', 'ctest'))
        warn = len({l.strip() for l in text.splitlines() if re.search(r'\bwarning( [A-Z]+\d+)?:', l)})
        total = TESTS_TOTAL.search(text)
        skips = SKIPS.search(text)
        ok = steps_ok and warn == 0
        bad |= not ok
        print(f"{name:6} {'PASS' if ok else 'FAIL'}  exits={exits} warnings={warn} "
              f"tests={total.group(2) if total else '?'}  {skips.group(0) if skips else 'skips: not reported'}")
    bad |= run_lints()
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(Path(sys.argv[1])))
