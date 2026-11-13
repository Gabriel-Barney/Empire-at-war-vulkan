"""What a build tree last built, so build records say which code they tested and an identical failing rerun is caught
before it costs a build (fewer failed builds, 2026-11-13).

The build scripts call
  begin <tree> --modules <list|all> --tests <""|regex|all|changed|none> [--refuse-repeat]
      before configuring. Records the source state in <tree>/build_state.json: commit (HEAD), worktree (folder name),
      branch, dirty (tracked edits), source (a hash of HEAD + uncommitted tracked edits + untracked files), modules, tests.
      With --refuse-repeat (build_agent.cmd, build_module.cmd) it first reads the previous build_state.json of the tree:
      when that run FAILED on exactly the same source, modules and tests, it prints that run's summary again and exits 3
      without building. Set ALAMO_RERUN=1 to build anyway (a flaky test, a changed environment).
tools/lint/build_summary.py --state <tree>/build_state.json then adds the outcome (rc, failure_kind, golden_drift,
real_failures, summary) and roadmap_store.py record-build copies the fields into the build record when the script sets
ALAMO_BUILD_EXTRA to that file.
Selftest: build_state.py --selftest
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

STATE = 'build_state.json'
BIG = 4 << 20  # untracked files above this are hashed by size and mtime only
# Keys that define "the same build"; a failure with all of them equal is a repeat.
SAME = ('commit', 'source', 'modules', 'tests')


def git(cwd, *args, binary=False):
    p = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=not binary)
    return p.returncode, p.stdout if binary else p.stdout.strip()


def source_state(cwd) -> dict:
    """commit, worktree, branch, dirty and a hash of everything a build would read from the checkout."""
    rc, sha = git(cwd, 'rev-parse', 'HEAD')
    sha = sha if rc == 0 else None
    rc, top = git(cwd, 'rev-parse', '--show-toplevel')
    top = top if rc == 0 and top else str(cwd)
    rc, br = git(cwd, 'rev-parse', '--abbrev-ref', 'HEAD')
    h = hashlib.sha256((sha or 'no-head').encode())
    rc, diff = git(cwd, 'diff', 'HEAD', '--binary', binary=True)
    dirty = bool(diff)
    h.update(diff or b'')
    rc, others = git(cwd, 'ls-files', '--others', '--exclude-standard', '-z')
    for name in sorted(n for n in others.split('\0') if n):
        p = Path(top) / name
        h.update(b'\0' + name.encode())
        try:
            st = p.stat()
            if st.st_size > BIG:
                h.update(f'{st.st_size}:{st.st_mtime_ns}'.encode())
            else:
                h.update(p.read_bytes())
        except OSError:
            h.update(b'?')
    return {'commit': sha, 'worktree': Path(top).name, 'branch': br if br and br != 'HEAD' else None,
            'dirty': dirty or bool(others.strip('\0')), 'source': h.hexdigest()[:16]}


def read_state(tree: Path):
    try:
        return json.loads((tree / STATE).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return None


def is_repeat(old, new) -> bool:
    return bool(old) and old.get('rc') not in (None, 0) and all(old.get(k) == new.get(k) for k in SAME)


def cmd_begin(tree: Path, modules: str, tests: str, refuse_repeat: bool, cwd) -> int:
    new = {**source_state(cwd), 'modules': modules.strip(), 'tests': tests.strip(),
           'started': time.strftime('%Y-%m-%dT%H:%M:%S')}
    old = read_state(tree)
    if refuse_repeat and is_repeat(old, new):
        if os.environ.get('ALAMO_RERUN') == '1':
            print('build_state: same source as the last FAILED run; building again because ALAMO_RERUN=1')
        else:
            print(f'build_state: nothing changed since the last run in {tree.name} ({old.get("started")}), and it FAILED:')
            print(f'  commit {str(new["commit"])[:10]}, source {new["source"]}, modules "{new["modules"]}", tests "{new["tests"]}"')
            print('--- summary of that run')
            print((old.get('summary') or '(no summary saved)').rstrip())
            print('---')
            print('Not built again: change the code (or the module list / test filter) first. A flaky test or a changed')
            print('environment (game data, toolchain): set ALAMO_RERUN=1 and run again.')
            return 3
    tree.mkdir(parents=True, exist_ok=True)
    (tree / STATE).write_text(json.dumps(new, indent=1) + '\n', encoding='utf8')
    return 0


def selftest() -> int:
    bad = []

    def check(c, msg):
        if not c:
            bad.append(msg)

    with tempfile.TemporaryDirectory() as td:
        repo = Path(td) / 'wt-x'
        repo.mkdir()

        def g(*a):
            subprocess.run(['git', *a], cwd=repo, check=True, capture_output=True)

        g('init', '-q')
        g('config', 'user.email', 'selftest@example.invalid')
        g('config', 'user.name', 'selftest')
        (repo / '.gitignore').write_text('/build-*/\n')
        (repo / 'a.cpp').write_text('int a;\n')
        g('add', '.')
        g('commit', '-q', '-m', 'one')
        tree = repo / 'build-wt-msvc'
        s0 = source_state(repo)
        check(s0['worktree'] == 'wt-x' and not s0['dirty'] and s0['commit'], f'clean state {s0}')
        (repo / 'a.cpp').write_text('int b;\n')
        s1 = source_state(repo)
        check(s1['dirty'] and s1['source'] != s0['source'], 'a tracked edit changes the source hash')
        (repo / 'new.cpp').write_text('int n;\n')
        s2 = source_state(repo)
        check(s2['source'] != s1['source'], 'an untracked file changes the source hash')
        (repo / 'new.cpp').write_text('int m;\n')
        check(source_state(repo)['source'] != s2['source'], 'an untracked file edit changes the source hash')
        # the build tree itself is ignored, so writing the state does not change the hash
        check(cmd_begin(tree, 'all', '', True, repo) == 0, 'first begin builds')
        st = read_state(tree)
        check(source_state(repo)['source'] == st['source'], 'the state file must not change the source hash')
        # a pass is no reason to refuse
        st['rc'] = 0
        (tree / STATE).write_text(json.dumps(st))
        check(cmd_begin(tree, 'all', '', True, repo) == 0, 'a rerun after a pass builds')
        # a failure on the same source refuses, unless ALAMO_RERUN=1 or something differs
        st = read_state(tree)
        st['rc'], st['summary'] = 1, 'SUMMARY FAIL'
        (tree / STATE).write_text(json.dumps(st))
        check(cmd_begin(tree, 'all', '', True, repo) == 3, 'an identical failing rerun is refused')
        check(cmd_begin(tree, 'all', '', False, repo) == 0, 'without --refuse-repeat it builds')
        (tree / STATE).write_text(json.dumps(st))
        check(cmd_begin(tree, 'all', 'lua', True, repo) == 0, 'another test filter builds')
        (tree / STATE).write_text(json.dumps(st))
        os.environ['ALAMO_RERUN'] = '1'
        try:
            check(cmd_begin(tree, 'all', '', True, repo) == 0, 'ALAMO_RERUN=1 builds')
        finally:
            del os.environ['ALAMO_RERUN']
        (tree / STATE).write_text(json.dumps(st))
        (repo / 'a.cpp').write_text('int c;\n')
        check(cmd_begin(tree, 'all', '', True, repo) == 0, 'an edit builds')
        # an interrupted run (no rc) is not a failure
        check(cmd_begin(tree, 'all', '', True, repo) == 0, 'a run without an outcome is not a repeat')
    for b in bad:
        print('FAIL: ' + b)
    print('build_state selftest: ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--selftest', action='store_true')
    sub = ap.add_subparsers(dest='cmd')
    b = sub.add_parser('begin')
    b.add_argument('tree')
    b.add_argument('--modules', default='all')
    b.add_argument('--tests', default='')
    b.add_argument('--refuse-repeat', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if a.cmd == 'begin':
        try:
            return cmd_begin(Path(a.tree), a.modules, a.tests, a.refuse_repeat, os.getcwd())
        except OSError as e:  # a record is never worth failing the build over
            print(f'build_state: {e!r}')
            return 0
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
