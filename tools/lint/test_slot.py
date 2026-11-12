"""Machine-wide limit on concurrent test runs (2026-11-12).

Usage: python -I tools/lint/test_slot.py -- <command> [args...]
       python -I tools/lint/test_slot.py --status
       python -I tools/lint/test_slot.py --selftest

Takes one of N slots shared by every checkout and worktree on this machine, runs the command, and
passes its exit code through. While all slots are taken it waits and says so. Slots are byte locks
on files in %LOCALAPPDATA%\\AlamoTestSlots; Windows drops a lock when its process ends, so a killed
or crashed run never leaves a slot taken.

Why: about 25 sessions saw tests time out when several full ctest runs (each -j 4 or -j 8) shared
the 16 cores, and passed when run alone. Two full runs at a time keep the machine busy without
pushing timing-sensitive tests over their timeouts.

Environment:
  ALAMO_TEST_SLOTS     number of slots (default 2); 0 turns the limit off
  ALAMO_TEST_SLOT_WAIT longest wait in seconds before running anyway (default 5400)
  ALAMO_TEST_SLOT_DIR  slot folder (default %LOCALAPPDATA%\\AlamoTestSlots; the selftest sets it)
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time

if os.name == 'nt':
    import msvcrt
else:  # pragma: no cover - the project is Windows only; keep the module importable elsewhere
    msvcrt = None

POLL_S = 5.0
REPORT_EVERY_S = 300.0


def slot_dir() -> str:
    d = os.environ.get('ALAMO_TEST_SLOT_DIR')
    if not d:
        base = os.environ.get('LOCALAPPDATA') or tempfile.gettempdir()
        d = os.path.join(base, 'AlamoTestSlots')
    os.makedirs(d, exist_ok=True)
    return d


def slot_count() -> int:
    try:
        return max(0, int(os.environ.get('ALAMO_TEST_SLOTS', '2')))
    except ValueError:
        return 2


def max_wait() -> float:
    try:
        return float(os.environ.get('ALAMO_TEST_SLOT_WAIT', '5400'))
    except ValueError:
        return 5400.0


class Slot:
    def __init__(self, d: str, index: int, fh):
        self.dir, self.index, self.fh = d, index, fh

    def info_path(self) -> str:
        return os.path.join(self.dir, f'slot{self.index}.json')

    def write_info(self, cmd: list[str]) -> None:
        try:
            with open(self.info_path(), 'w', encoding='utf-8') as f:
                json.dump({'pid': os.getpid(), 'cwd': os.getcwd(), 'cmd': ' '.join(cmd)[:300],
                           'since': time.strftime('%Y-%m-%d %H:%M:%S')}, f)
        except OSError:
            pass

    def release(self) -> None:
        try:
            os.remove(self.info_path())
        except OSError:
            pass
        try:
            self.fh.seek(0)
            msvcrt.locking(self.fh.fileno(), msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
        self.fh.close()


def try_take(d: str, n: int) -> Slot | None:
    for i in range(n):
        fh = open(os.path.join(d, f'slot{i}.lock'), 'a+b')
        try:
            fh.seek(0)
            msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            fh.close()
            continue
        return Slot(d, i, fh)
    return None


def holders(d: str, n: int) -> list[str]:
    out = []
    for i in range(n):
        try:
            with open(os.path.join(d, f'slot{i}.json'), encoding='utf-8') as f:
                h = json.load(f)
            out.append(f"slot {i}: pid {h.get('pid')} since {h.get('since')} in {h.get('cwd')}")
        except (OSError, ValueError):
            out.append(f'slot {i}: taken (no info)')
    return out


def acquire(cmd: list[str]) -> Slot | None:
    n = slot_count()
    if n == 0 or msvcrt is None:
        return None
    d = slot_dir()
    start = time.monotonic()
    last_report = None
    while True:
        s = try_take(d, n)
        if s is not None:
            if last_report is not None:
                print(f'test_slot: got slot {s.index} after {time.monotonic() - start:.0f} s', flush=True)
            s.write_info(cmd)
            return s
        waited = time.monotonic() - start
        if waited >= max_wait():
            print(f'test_slot: waited {waited:.0f} s, running without a slot', flush=True)
            return None
        if last_report is None or time.monotonic() - last_report >= REPORT_EVERY_S:
            print(f'test_slot: all {n} test slots are taken; waiting (ALAMO_TEST_SLOTS=0 skips this):', flush=True)
            for line in holders(d, n):
                print('  ' + line, flush=True)
            last_report = time.monotonic()
        time.sleep(POLL_S)


def run(cmd: list[str]) -> int:
    slot = acquire(cmd)
    try:
        return subprocess.call(cmd)
    finally:
        if slot is not None:
            slot.release()


def status() -> int:
    n, d = slot_count(), slot_dir()
    if n == 0:
        print('test_slot: limit off (ALAMO_TEST_SLOTS=0)')
        return 0
    free = []
    for i in range(n):
        fh = open(os.path.join(d, f'slot{i}.lock'), 'a+b')
        try:
            fh.seek(0)
            msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
            msvcrt.locking(fh.fileno(), msvcrt.LK_UNLCK, 1)
            free.append(i)
        except OSError:
            pass
        finally:
            fh.close()
    print(f'test_slot: {n} slots in {d}; free: {free or "none"}')
    for i, line in enumerate(holders(d, n)):
        if i not in free:
            print('  ' + line)
    return 0


def selftest() -> int:
    with tempfile.TemporaryDirectory() as d:
        os.environ['ALAMO_TEST_SLOT_DIR'] = d
        os.environ['ALAMO_TEST_SLOTS'] = '2'
        a = try_take(d, 2)
        b = try_take(d, 2)
        assert a is not None and b is not None and a.index != b.index, 'two slots expected'
        a.write_info(['x'])
        # A third run in another process must find both slots taken.
        code = ('import sys; sys.path.insert(0, sys.argv[1]); import test_slot as t; '
                'sys.exit(0 if t.try_take(sys.argv[2], 2) is None else 1)')
        here = os.path.dirname(os.path.abspath(__file__))
        r = subprocess.call([sys.executable, '-I', '-c', code, here, d])
        assert r == 0, 'a third process took a slot while both were held'
        assert 'pid' in holders(d, 2)[a.index], 'holder info missing'
        a.release()
        r = subprocess.call([sys.executable, '-I', '-c', code.replace('is None else 1', 'is not None else 1'), here, d])
        assert r == 0, 'a released slot was not free for another process'
        b.release()
        # Exit codes pass through, and a run with the limit off takes no slot.
        assert run([sys.executable, '-c', 'import sys; sys.exit(3)']) == 3, 'exit code not passed through'
        os.environ['ALAMO_TEST_SLOTS'] = '0'
        assert acquire(['x']) is None
        # A lock held by a process that is killed is released by Windows.
        p = subprocess.Popen([sys.executable, '-I', '-c',
                              'import sys, time; sys.path.insert(0, sys.argv[1]); import test_slot as t; '
                              's = t.try_take(sys.argv[2], 1); print("held", flush=True); time.sleep(60)', here, d],
                             stdout=subprocess.PIPE, text=True)
        assert p.stdout.readline().strip() == 'held'
        assert try_take(d, 1) is None, 'slot held by a live process was free'
        p.kill()
        p.wait()
        p.stdout.close()
        s = try_take(d, 1)
        assert s is not None, 'slot of a killed process stayed taken'
        s.release()
    print('test_slot selftest: ok')
    return 0


def main(argv: list[str]) -> int:
    if argv[:1] == ['--selftest']:
        return selftest()
    if argv[:1] == ['--status']:
        return status()
    if argv[:1] == ['--']:
        argv = argv[1:]
    if not argv:
        print(__doc__)
        return 2
    return run(argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
