"""Stop hung test or tool processes started from this checkout's own build trees (2026-11-12).

Usage: python -I tools/worktree/stop_tree_procs.py [--dry-run] [build tree ...]
       python -I tools/worktree/stop_tree_procs.py --selftest

Stops every process whose executable lies under a build-* folder of the checkout this is run from
(or under the named build trees, which must be build-* folders of it). Processes of other worktrees,
the main checkout, the toolchain and everything else on the machine are never touched, so this cannot
stop another session's tests or builds.

This is the narrow form of "taskkill /F /IM alamo_*.exe" that .claude/settings.json allows without a
prompt: the plain command also stops other sessions' test runs.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys


def norm(p: str) -> str:
    return os.path.normcase(os.path.normpath(p))


def toplevel() -> str:
    return os.path.normpath(subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True,
                                           text=True, check=True).stdout.strip())


def tree_roots(top: str, names: list[str]) -> list[str] | None:
    if not names:
        return [os.path.join(top, d) for d in sorted(os.listdir(top))
                if d.lower().startswith('build-') and os.path.isdir(os.path.join(top, d))]
    roots = []
    for n in names:
        p = os.path.normpath(n if os.path.isabs(n) else os.path.join(top, n))
        if norm(os.path.dirname(p)) != norm(top) or not os.path.basename(p).lower().startswith('build-'):
            print(f'refused {n}: not a build-* folder of {top}')
            return None
        roots.append(p)
    return roots


def matches(procs: list[dict], roots: list[str], own_pid: int) -> list[dict]:
    rs = [norm(r) + os.sep for r in roots]
    out = []
    for p in procs:
        exe = p.get('ExecutablePath')
        if not exe or p.get('ProcessId') == own_pid:
            continue
        e = norm(exe)
        if any(e.startswith(r) for r in rs):
            out.append(p)
    return out


def list_procs() -> list[dict]:
    ps = ('Get-CimInstance Win32_Process | Select-Object ProcessId,Name,ExecutablePath | '
          'ConvertTo-Json -Compress')
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps],
                       capture_output=True, text=True, check=True)
    data = json.loads(r.stdout or '[]')
    return data if isinstance(data, list) else [data]


def main(argv: list[str]) -> int:
    if argv[:1] == ['--selftest']:
        return selftest()
    dry = '--dry-run' in argv
    top = toplevel()
    roots = tree_roots(top, [a for a in argv if a != '--dry-run'])
    if roots is None:
        return 2
    if not roots:
        print(f'no build-* folders in {top}')
        return 0
    found = matches(list_procs(), roots, os.getpid())
    if not found:
        print('no processes running from ' + ', '.join(os.path.basename(r) for r in roots))
        return 0
    bad = 0
    for p in found:
        pid, exe = p['ProcessId'], p['ExecutablePath']
        if dry:
            print(f'would stop {pid} {exe}')
            continue
        r = subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True, text=True)
        print(f'{"stopped" if r.returncode == 0 else "could not stop"} {pid} {exe}')
        bad += r.returncode != 0
    return 1 if bad else 0


def selftest() -> int:
    top = os.path.join('E:' + os.sep, 'repo', '.claude', 'worktrees', 'a')
    other = os.path.join('E:' + os.sep, 'repo', '.claude', 'worktrees', 'ab')
    procs = [
        {'ProcessId': 1, 'ExecutablePath': os.path.join(top, 'build-wt-msvc', 'engine', 'core', 'alamo_core_tests.exe')},
        {'ProcessId': 2, 'ExecutablePath': os.path.join(other, 'build-wt-msvc', 'alamo_core_tests.exe')},
        {'ProcessId': 3, 'ExecutablePath': os.path.join(top, 'toolchain', 'venv', 'Scripts', 'ctest.exe')},
        {'ProcessId': 4, 'ExecutablePath': os.path.join(top, 'BUILD-CLANG', 'x.exe')},
        {'ProcessId': 5, 'ExecutablePath': None},
        {'ProcessId': 6, 'ExecutablePath': os.path.join(top, 'build-wt-msvcX', 'y.exe')},
    ]
    roots = [os.path.join(top, 'build-wt-msvc'), os.path.join(top, 'build-clang')]
    got = sorted(p['ProcessId'] for p in matches(procs, roots, own_pid=99))
    assert got == [1, 4], got
    assert matches(procs, roots, own_pid=1)[0]['ProcessId'] == 4
    real = toplevel()
    assert tree_roots(real, ['..']) is None
    assert tree_roots(real, [os.path.join(real, 'engine')]) is None
    assert tree_roots(real, ['build-x']) == [os.path.join(real, 'build-x')]
    print('stop_tree_procs selftest: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
