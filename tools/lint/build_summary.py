"""Short summary of one build script's logs (build_msvc / build_clang / build_asan / build_agent / build_module).

Prints the step exits, the number of unique warnings and errors (first few lines), the ctest result with the names of failed
tests and the tail of their output, and the skips line. Meant for agents: read this instead of the whole log.
Exits 1 when a step failed, a warning or error was logged, a test failed, or the run did not finish.
Usage: build_summary.py <log> [<log> ...]
"""
import json
import re
import subprocess
import sys
from pathlib import Path

MAX_DIAG = 12      # warning / error lines shown
MAX_FAIL_LINES = 25  # output lines shown per failed test
MAX_FAILED = 8     # failed tests with output shown

DONE_MARKERS = ('BUILD_DONE', 'AGENT_DONE', 'MODULE_DONE')
WARN = re.compile(r'\bwarning( [A-Z]+\d+)?:|: warning\b')
ERR = re.compile(r'\berror( [A-Z]+\d+)?:|^FAILED: |fatal error|LNK\d+')
TEST_HEADER = re.compile(r'^\s*\d+/\d+ Test\s+#\d+: (\S+) \.+\s*(.*)$')


def unique(lines):
    seen, out = set(), []
    for l in lines:
        k = l.strip()
        if k and k not in seen:
            seen.add(k)
            out.append(k)
    return out


def failed_test_output(lines):
    """name -> output lines, for tests whose ctest header says Failed / Timeout / Exception / Not Run."""
    result, cur, name = {}, None, None
    for l in lines:
        m = TEST_HEADER.match(l)
        if m:
            name = m.group(1)
            bad = re.search(r'\*\*\*|Failed|Timeout|Exception|Not Run', m.group(2))
            cur = [] if bad else None
            if cur is not None:
                result[name] = cur
            continue
        if re.match(r'^\d+% tests passed', l):
            cur = None
        if cur is not None:
            cur.append(l.rstrip())
    return result


def cache_stats():
    """One line from sccache's server (cumulative since it started), or None when sccache is not installed / not running."""
    exe = Path(__file__).resolve().parents[2] / 'toolchain' / 'sccache' / 'sccache.exe'
    if not exe.exists():
        return None
    try:
        out = subprocess.run([str(exe), '--show-stats', '--stats-format', 'json'], capture_output=True, text=True, timeout=20)
        st = json.loads(out.stdout)['stats']
        hits = sum(st.get('cache_hits', {}).get('counts', {}).values())
        miss = sum(st.get('cache_misses', {}).get('counts', {}).values())
        return f'sccache: {hits} hits, {miss} misses (cumulative since the server started)'
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        return None


def main(paths):
    parts = []
    for p in paths:
        try:
            parts.append(Path(p).read_text(encoding='utf8', errors='replace'))
        except OSError:
            print(f'missing log: {p}')
            return 1
    text = '\n'.join(parts)
    lines = text.splitlines()
    bad = False

    done = [m for m in DONE_MARKERS if m in text]
    if not done:
        print('NOT FINISHED (no BUILD_DONE / AGENT_DONE / MODULE_DONE in the logs)')
        bad = True
    exits = re.findall(r'^(configure|build|ctest) exit (-?\d+)\s*$', text, re.M)
    print('exits: ' + (' '.join(f'{k}={v}' for k, v in exits) or 'none found'))
    bad |= any(v != '0' for _, v in exits)

    warns = unique(l for l in lines if WARN.search(l) and not ERR.search(l))
    errs = unique(l for l in lines if ERR.search(l))
    print(f'warnings: {len(warns)}  errors: {len(errs)}')
    for l in (errs + warns)[:MAX_DIAG]:
        print('  ' + l[:220])
    if len(errs) + len(warns) > MAX_DIAG:
        print(f'  ... {len(errs) + len(warns) - MAX_DIAG} more in the log')
    bad |= bool(warns or errs)
    # The sccache server remembers the compiler path it saw first. Worktrees share one toolchain through a junction, so after
    # the worktree that started the server is deleted every other worktree's compiles fail with this error (seen 2026-11-06).
    if re.search(r'sccache: (error: failed to spawn|encountered fatal error)', text):
        print('HINT: sccache could not start the compiler (a stale server pointing at a deleted worktree?). '
              'Run toolchain\\sccache\\sccache.exe --stop-server and build again.')

    # ctest prints "100% tests passed out of N" when all pass, "N% tests passed, F tests failed out of M" otherwise.
    m = re.search(r'\d+% tests passed(?:, (\d+) tests failed)? out of (\d+)', text)
    if m:
        nfail = int(m.group(1) or 0)
        print(f'tests: {int(m.group(2)) - nfail}/{m.group(2)} passed' + (f', {nfail} FAILED' if nfail else ''))
        if nfail:
            bad = True
            names = re.findall(r'^\s+\d+ - (\S+) \(([^)]*)\)', text, re.M)
            print('FAILED: ' + ', '.join(f'{n} ({r})' for n, r in names))
            for n, out in list(failed_test_output(lines).items())[:MAX_FAILED]:
                print(f'--- {n}')
                for l in out[-MAX_FAIL_LINES:]:
                    print('  ' + l[:200])
    else:
        print('tests: no ctest result found')
    cache = cache_stats()
    if cache:
        print(cache)
    skips = re.search(r'^skips: .*$', text, re.M)
    if skips:
        print(skips.group(0))
    print('SUMMARY ' + ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
