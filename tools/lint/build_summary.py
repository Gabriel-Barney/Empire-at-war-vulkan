"""Short summary of one build script's logs (build_msvc / build_clang / build_asan / build_agent / build_module).

Prints the step exits, the number of unique warnings and errors (first few lines), the ctest result with the names of failed
tests and the tail of their output, and the skips line. Meant for agents: read this instead of the whole log.
Exits 1 when a step failed, a warning or error was logged, a test failed, or the run did not finish.
Failed tests are split (2026-11-13) into golden drift (the test's output shows only stored goldens / CRC logs / benchmark
scenarios that differ: the code changed what a golden records, intended or not) and real failures (a failed CHECK, a
crash, a timeout, anything else), so a behaviour change that only needs regenerated goldens reads differently from a broken
test. The exit code stays 1 for both: dev never takes stale goldens.
Usage: build_summary.py [--state <tree>/build_state.json] <log> [<log> ...]
  --state  adds the outcome (rc, failure_kind, golden_drift, real_failures, summary) to the file tools/lint/build_state.py
           wrote: roadmap_store.py record-build copies it into the build record, and the next run's repeat check reads it.
failure_kind: configure, compile, warnings, test (a real failure), golden (golden drift only), unfinished, other; null on a pass.
Selftest: build_summary.py --selftest
"""
import io
import json
import re
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

MAX_DIAG = 12      # warning / error lines shown
MAX_FAIL_LINES = 25  # output lines shown per failed test
MAX_FAILED = 8     # failed tests with output shown

DONE_MARKERS = ('BUILD_DONE', 'AGENT_DONE', 'MODULE_DONE')
WARN = re.compile(r'\bwarning( [A-Z]+\d+)?:|: warning\b')
ERR = re.compile(r'\berror( [A-Z]+\d+)?:|^FAILED: |fatal error|LNK\d+')
TEST_HEADER = re.compile(r'^\s*\d+/\d+ Test\s+#\d+: (\S+) \.+\s*(.*)$')
# Shared with gate_summary.py: the build scripts' step exit lines, ctest's total line and the skips line.
STEP_EXIT = re.compile(r'^(configure|build|ctest) exit (-?\d+)\s*$', re.M)
# ctest prints "100% tests passed out of N" when all pass, "N% tests passed, F tests failed out of M" otherwise.
TESTS_TOTAL = re.compile(r'\d+% tests passed(?:, (\d+) tests failed)? out of (\d+)')
SKIPS = re.compile(r'^skips: .*$', re.M)
# Golden drift: a stored golden, CRC log or benchmark scenario differs. Matches the messages of apps/headless/crc_out.hpp
# ("golden X: checkpoint tick ..."), ai_run / map_run ("AI golden X: DIFFERS"), the test exes ("FAIL golden X ...",
# "golden X: DIFFERS"), apps/game/compare_smoke.cmake ("CRC log differs from") and the benchmark checks ("scenario changed,
# not comparable", "run N vs golden X").
DRIFT = re.compile(r'\bgolden\b.*(?:DIFFERS|differs|\btick \d+:|DIVERGED|first bad checkpoint)'
                   r'|^FAIL golden |scenario changed, not comparable|CRC log differs from|\bvs golden\b')
# Determinism failures are never drift: runs that differ between runs of one build or by worker count (in-test checks
# print "workers N vs 0: identical" when they agree; the alamo_add_worker_count_check entries compare with <name>_w0.crc).
NOT_DRIFT = re.compile(r'non-deterministic|differs from run \d|workers? \d+ vs \d+: (?!identical)|golden\b.*_w0\.crc', re.I)
WORKER_ENTRY = re.compile(r'^(.+)_w\d+$')
# A failed CHECK of the test harness (engine/test_support): "FAIL <file>:<line>: <expr>"; older exes print "FAILED".
CHECK_FAIL = re.compile(r'^FAIL(?:ED)? .+\.(?:c|cc|cpp|h|hpp|inl):\d+: ')
CRASH = re.compile(r'Exception|Timeout|Not Run|SEGFAULT|Child aborted|SIGSEGV')


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


def test_statuses(lines):
    """name -> the status text of its ctest header line ("***Failed  1.2 sec", "***Exception: ...")."""
    return {m.group(1): m.group(2) for m in (TEST_HEADER.match(l) for l in lines) if m}


def classify(status: str, out_lines) -> str:
    """'golden' when a failed test's output shows only goldens that differ, else 'real'."""
    if CRASH.search(status):
        return 'real'
    if any(NOT_DRIFT.search(l) for l in out_lines):
        return 'real'
    drift = sum(1 for l in out_lines if DRIFT.search(l.strip()))
    if not drift:
        return 'real'
    # A golden comparison may end in one failed CHECK of its own; more failed CHECKs than goldens: something else broke too.
    checks = sum(1 for l in out_lines if CHECK_FAIL.match(l.strip()) and not DRIFT.search(l.strip()))
    return 'golden' if checks <= drift else 'real'


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


def summarize(paths, tree_name='<tree>', with_cache=True):
    """Prints the summary; returns (exit code, outcome fields for build_state.json)."""
    info = {'failure_kind': None, 'golden_drift': [], 'real_failures': []}
    parts = []
    for p in paths:
        try:
            parts.append(Path(p).read_text(encoding='utf8', errors='replace'))
        except OSError:
            print(f'missing log: {p}')
            info['failure_kind'] = 'unfinished'
            return 1, info
    text = '\n'.join(parts)
    lines = text.splitlines()
    bad = False

    done = [m for m in DONE_MARKERS if m in text]
    if not done:
        print('NOT FINISHED (no BUILD_DONE / AGENT_DONE / MODULE_DONE in the logs)')
        bad = True
    exits = STEP_EXIT.findall(text)
    print('exits: ' + (' '.join(f'{k}={v}' for k, v in exits) or 'none found'))
    bad |= any(v != '0' for _, v in exits)
    ex = dict(exits)
    kind = ('unfinished' if not done else 'configure' if ex.get('configure', '0') != '0'
            else 'compile' if ex.get('build', '0') != '0' else None)

    warns = unique(l for l in lines if WARN.search(l) and not ERR.search(l))
    errs = unique(l for l in lines if ERR.search(l))
    print(f'warnings: {len(warns)}  errors: {len(errs)}')
    for l in (errs + warns)[:MAX_DIAG]:
        print('  ' + l[:220])
    if len(errs) + len(warns) > MAX_DIAG:
        print(f'  ... {len(errs) + len(warns) - MAX_DIAG} more in the log')
    bad |= bool(warns or errs)

    m = TESTS_TOTAL.search(text)
    if m:
        nfail = int(m.group(1) or 0)
        print(f'tests: {int(m.group(2)) - nfail}/{m.group(2)} passed' + (f', {nfail} FAILED' if nfail else ''))
        if nfail:
            bad = True
            names = re.findall(r'^\s+\d+ - (\S+) \(([^)]*)\)', text, re.M)
            print('FAILED: ' + ', '.join(f'{n} ({r})' for n, r in names))
            outs = failed_test_output(lines)
            heads = test_statuses(lines)
            kinds = {n: classify(heads.get(n, r), outs.get(n, [])) for n, r in names}
            for n, r in names:  # worker-count entries skipped because their 0-worker setup only drifted follow it
                w = WORKER_ENTRY.match(n)
                if 'Not Run' in heads.get(n, r) + r and w and kinds.get(w.group(1)) == 'golden':
                    kinds[n] = 'golden'
            drift = [n for n in kinds if kinds[n] == 'golden']
            real = [n for n in kinds if kinds[n] != 'golden']
            info['golden_drift'], info['real_failures'] = drift, real
            if real:
                print(f'real failures ({len(real)}): ' + ', '.join(real))
            if drift:
                print(f'golden drift ({len(drift)}): ' + ', '.join(drift))
                print('  their output shows only stored goldens that differ: the code changed what they record. Intended: commit,')
                print(f'  then tools\\regen_goldens.cmd --build-dir {tree_name} --only all (goldens a test exe owns: that exe with')
                print('  --regen), and say so in the commit message. Not intended: it is a regression, fix the code.')
            for n in (real + drift)[:MAX_FAILED]:  # real failures first: they are what needs fixing
                print(f'--- {n}' + (' (golden drift)' if kinds[n] == 'golden' else ''))
                for l in outs.get(n, [])[-MAX_FAIL_LINES:]:
                    print('  ' + l[:200])
            if kind is None:
                kind = 'test' if real else 'golden'
    else:
        print('tests: no ctest result found')
    cache = cache_stats() if with_cache else None
    if cache:
        print(cache)
    skips = SKIPS.search(text)
    if skips:
        print(skips.group(0))
    if kind is None and errs:
        kind = 'compile'
    if kind is None and warns:
        kind = 'warnings'
    if bad and kind is None:
        kind = 'test' if ex.get('ctest', '0') != '0' else 'other'
    info['failure_kind'] = kind if bad else None
    tail = ' (golden drift only: regenerate the goldens if the change was intended)' if bad and kind == 'golden' else ''
    print('SUMMARY ' + ('FAIL' if bad else 'PASS') + tail)
    return (1 if bad else 0), info


def main(argv):
    state = None
    if len(argv) >= 2 and argv[0] == '--state':
        state, argv = Path(argv[1]), argv[2:]
    if not argv:
        print(__doc__)
        return 2
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc, info = summarize(argv, state.parent.name if state is not None else '<tree>')
    out = buf.getvalue()
    sys.stdout.write(out)
    if state is not None:
        try:
            st = json.loads(state.read_text(encoding='utf-8-sig')) if state.is_file() else {}
            st.update(info, rc=rc, summary=out[-6000:])
            state.parent.mkdir(parents=True, exist_ok=True)
            state.write_text(json.dumps(st, indent=1) + '\n', encoding='utf8')
        except (OSError, ValueError) as e:
            print(f'build_summary: could not update {state}: {e!r}')
    return rc


SAMPLE = r'''configure exit 0
build exit 0
      Start  1: alamo_headless_skirmish_ai_foc_land
  1/5 Test  #1: alamo_headless_skirmish_ai_foc_land ...........***Failed    3.10 sec
skirmish: 600 ticks
golden skirmish_ai_foc_land_golden.crc: checkpoint tick 64: system 'ai.goals' expected 1, got 2 (first bad checkpoint, every 64 ticks; columns that differ: ai.goals)
  2/5 Test  #2: alamo_ai_galactic_tests .......................***Failed    9.00 sec
  golden E:/x/engine/ai/tests/golden/ai_galactic_synthetic_crc.txt: DIFFERS
FAILED E:\x y\engine\ai\tests\test_ai_galactic.cpp:1324: same
  3/5 Test  #3: alamo_tactical_movement_tests .................***Failed    1.00 sec
FAIL E:\x y\engine\tactical\movement\tests\test_movement.cpp:88: d < 1.0f
  4/5 Test  #4: alamo_headless_bench_space_battle_w4 ..........***Failed    2.00 sec
bench: run 1 differs from run 0 (non-deterministic): golden checkpoint tick 64 differs
  5/5 Test  #5: alamo_game_smoke_land_golden ..................***Exception: SegFault  0.10 sec
CRC log differs from E:/x/game_smoke_land.crc

20% tests passed, 4 tests failed out of 5

The following tests FAILED:
	  1 - alamo_headless_skirmish_ai_foc_land (Failed)
	  2 - alamo_ai_galactic_tests (Failed)
	  3 - alamo_tactical_movement_tests (Failed)
	  4 - alamo_headless_bench_space_battle_w4 (Failed)
	  5 - alamo_game_smoke_land_golden (SEGFAULT)
ctest exit 8
AGENT_DONE
'''


def selftest() -> int:
    bad = []
    with tempfile.TemporaryDirectory() as td:
        log = Path(td) / 'log.txt'
        log.write_text(SAMPLE, encoding='utf8')
        with redirect_stdout(io.StringIO()):
            rc, info = summarize([str(log)], with_cache=False)
        if rc != 1:
            bad.append(f'rc {rc}')
        if info['golden_drift'] != ['alamo_headless_skirmish_ai_foc_land', 'alamo_ai_galactic_tests']:
            bad.append(f"drift {info['golden_drift']}")
        if info['real_failures'] != ['alamo_tactical_movement_tests', 'alamo_headless_bench_space_battle_w4',
                                     'alamo_game_smoke_land_golden']:
            bad.append(f"real {info['real_failures']}")
        if info['failure_kind'] != 'test':
            bad.append(f"kind {info['failure_kind']}")
        head, rest = SAMPLE.split('  3/5 Test', 1)
        only = head + '\n' + rest[rest.index('20% tests passed'):]
        only = '\n'.join(l for l in only.splitlines() if not re.search(r' [345] - ', l))
        log.write_text(only.replace('4 tests failed out of 5', '2 tests failed out of 2'), encoding='utf8')
        with redirect_stdout(io.StringIO()) as o:
            rc, info = summarize([str(log)], with_cache=False)
        if rc != 1 or info['failure_kind'] != 'golden' or 'golden drift only' not in o.getvalue():
            bad.append(f"golden-only: rc {rc} kind {info['failure_kind']}")
        log.write_text('configure exit 0\nbuild exit 1\nx.cpp(3): error C2001: newline in constant\nAGENT_DONE\n', encoding='utf8')
        with redirect_stdout(io.StringIO()):
            rc, info = summarize([str(log)], with_cache=False)
        if info['failure_kind'] != 'compile':
            bad.append(f"compile kind {info['failure_kind']}")
        log.write_text('configure exit 0\nbuild exit 0\n100% tests passed out of 3\nctest exit 0\nAGENT_DONE\n', encoding='utf8')
        state = Path(td) / 'build-wt-msvc' / 'build_state.json'
        state.parent.mkdir()
        state.write_text('{"commit": "abc"}', encoding='utf8')
        with redirect_stdout(io.StringIO()):
            rc = main(['--state', str(state), str(log)])
        st = json.loads(state.read_text(encoding='utf8'))
        if rc != 0 or st.get('rc') != 0 or st.get('commit') != 'abc' or st.get('failure_kind') is not None:
            bad.append(f'state {st}')
    for b in bad:
        print('FAIL: ' + b)
    print('build_summary selftest: ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
