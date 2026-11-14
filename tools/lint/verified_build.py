"""Stamp of the last passing build per build tree, so tools\\land_on_dev.cmd can tell whether the commit it is about to
push was built and tested (fewer red dev pushes, 2026-11-12).

A build script calls `begin` before it configures and `pass` after build_summary.py exits 0:
  begin <tree> --modules <list|all> --tests <""|regex|all>   records HEAD, whether tracked files were dirty, the module
                                                            list and the test filter in <tree>/verify_pending.json, and
                                                            drops the old <tree>/verified.json
  pass <tree>                                               turns the pending record into <tree>/verified.json (only
                                                            when the tree was clean at the start)
land_on_dev.cmd then asks:
  land-check --base origin/dev --head HEAD --tree build-wt-msvc --tree build-msvc
    exit 0: HEAD only changes files that do not affect the build (docs, *.md, .claude/), or one of the trees has a
            stamp for exactly HEAD with every module built and the fast tier or all tests run
    exit 1: no such stamp: build and test first
Selftest: verified_build.py --selftest
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

PENDING = 'verify_pending.json'
VERIFIED = 'verified.json'
# Paths that cannot change what the MSVC build or its tests do. Anything else needs a build.
NO_BUILD_PREFIXES = ('docs/', '.claude/', 'launcher/')
NO_BUILD_SUFFIXES = ('.md',)
GOOD_TESTS = ('', 'all')  # fast tier (no filter) or every test; a regex run proves too little


def git(cwd, *args):
    p = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout.strip()


def head_state(cwd):
    rc, sha = git(cwd, 'rev-parse', 'HEAD')
    if rc:
        return None, True
    rc, st = git(cwd, 'status', '--porcelain', '-uno')
    return sha, bool(st) or rc != 0


def source_hash(cwd, rev='HEAD'):
    """Hash of every tracked file of <rev> that can affect the build (bo1 t06): a docs-only / roadmap-only merge changes
    HEAD but not this, so a stamp for the same source hash still counts. None when git fails (then only the sha matches)."""
    rc, out = git(cwd, 'ls-tree', '-r', rev)
    if rc:
        return None
    h = hashlib.sha256()
    for line in out.splitlines():
        meta, _, path = line.partition(chr(9))
        if needs_build([path]):
            h.update(f'{path} {meta.split()[2]}\n'.encode('utf8'))
    return h.hexdigest()


def cmd_begin(tree: Path, modules: str, tests: str, cwd) -> int:
    tree.mkdir(parents=True, exist_ok=True)
    for name in (PENDING, VERIFIED):
        try:
            (tree / name).unlink()
        except FileNotFoundError:
            pass
    sha, dirty = head_state(cwd)
    rec = {'sha': sha, 'dirty': dirty, 'src': source_hash(cwd), 'modules': modules.strip(), 'tests': tests.strip(),
           'started': time.strftime('%Y-%m-%dT%H:%M:%S')}
    (tree / PENDING).write_text(json.dumps(rec, indent=1) + '\n', encoding='utf8')
    return 0


def cmd_pass(tree: Path) -> int:
    pend = tree / PENDING
    if not pend.is_file():
        return 0
    rec = json.loads(pend.read_text(encoding='utf8'))
    pend.unlink()
    if rec.get('sha') and not rec.get('dirty'):
        rec['passed'] = time.strftime('%Y-%m-%dT%H:%M:%S')
        (tree / VERIFIED).write_text(json.dumps(rec, indent=1) + '\n', encoding='utf8')
    return 0


def needs_build(paths) -> bool:
    for p in paths:
        p = p.replace('\\', '/')
        if not (p.startswith(NO_BUILD_PREFIXES) or p.lower().endswith(NO_BUILD_SUFFIXES)):
            return True
    return False


def stamp_ok(tree: Path, sha: str, src=None):
    f = tree / VERIFIED
    if not f.is_file():
        return False, f'{tree.name}: no passing build recorded'
    try:
        rec = json.loads(f.read_text(encoding='utf8'))
    except ValueError:
        return False, f'{tree.name}: unreadable {VERIFIED}'
    if rec.get('sha') != sha and not (src and rec.get('src') == src):
        return False, f'{tree.name}: last pass was on {str(rec.get("sha"))[:10]}, not HEAD {sha[:10]}'
    if rec.get('modules') != 'all':
        return False, f'{tree.name}: last pass built only "{rec.get("modules")}", not all modules'
    if rec.get('tests') not in GOOD_TESTS:
        return False, f'{tree.name}: last pass ran only tests matching "{rec.get("tests")}"'
    return True, f'{tree.name}: passed on {'HEAD ' + sha[:10] if rec.get('sha') == sha else 'the same build sources as HEAD ' + sha[:10] + ' (commit ' + str(rec.get('sha'))[:10] + ')'} at {rec.get("passed")} (tests: {rec.get("tests") or "fast tier"})'


def cmd_land_check(base: str, head: str, trees, cwd) -> int:
    rc, sha = git(cwd, 'rev-parse', head)
    if rc:
        print(f'land-check: cannot resolve {head}')
        return 1
    rc, out = git(cwd, 'diff', '--name-only', base, sha)
    if rc == 0 and not needs_build(out.splitlines()):
        print(f'land-check: {head} changes no build input against {base} (docs / *.md / .claude only); no build needed')
        return 0
    reasons = []
    src = source_hash(cwd, sha)
    for t in trees:
        ok, why = stamp_ok(Path(cwd) / t, sha, src)
        if ok:
            print('land-check: ' + why)
            return 0
        reasons.append(why)
    print('land-check: no passing MSVC build of every module for HEAD ' + sha[:10] + ':')
    for r in reasons:
        print('  ' + r)
    return 1


def selftest() -> int:
    bad = []

    def check(c, msg):
        if not c:
            bad.append(msg)

    with tempfile.TemporaryDirectory() as td:
        repo = Path(td)

        def g(*a):
            subprocess.run(['git', *a], cwd=repo, check=True, capture_output=True)

        g('init', '-q')
        g('config', 'user.email', 'selftest@example.invalid')
        g('config', 'user.name', 'selftest')
        (repo / 'a.cpp').write_text('int a;\n')
        (repo / 'docs').mkdir()
        (repo / 'docs' / 'x.md').write_text('x\n')
        g('add', '.')
        g('commit', '-q', '-m', 'one')
        base = git(repo, 'rev-parse', 'HEAD')[1]
        tree = repo / 'build-wt-msvc'
        # docs-only change: no build needed
        (repo / 'docs' / 'x.md').write_text('y\n')
        g('commit', '-q', '-am', 'docs')
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc'], repo) == 0, 'docs-only change should pass')
        # code change, no stamp: fail
        (repo / 'a.cpp').write_text('int b;\n')
        g('commit', '-q', '-am', 'code')
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc'], repo) == 1, 'code change without a stamp should fail')
        # module subset stamp: fail
        cmd_begin(tree, 'core;lua', '', repo)
        cmd_pass(tree)
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc'], repo) == 1, 'subset stamp should fail')
        # regex stamp: fail
        cmd_begin(tree, 'all', 'lua', repo)
        cmd_pass(tree)
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc'], repo) == 1, 'regex stamp should fail')
        # dirty at begin: no stamp
        (repo / 'a.cpp').write_text('int c;\n')
        cmd_begin(tree, 'all', '', repo)
        cmd_pass(tree)
        check(not (tree / VERIFIED).exists(), 'dirty build must not stamp')
        g('checkout', '-q', '--', 'a.cpp')
        # begin without pass (failed build): no stamp, and it drops an older one
        cmd_begin(tree, 'all', '', repo)
        cmd_pass(tree)
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc'], repo) == 0, 'full fast-tier stamp should pass')
        cmd_begin(tree, 'all', '', repo)
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc'], repo) == 1, 'a started, unfinished build must drop the stamp')
        # all-tests stamp in the second tree counts
        cmd_begin(repo / 'build-msvc', 'all', 'all', repo)
        cmd_pass(repo / 'build-msvc')
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc', 'build-msvc'], repo) == 0, 'build-msvc stamp should pass')
        # a docs-only commit on top of a stamped one keeps the stamp valid (same build-source hash)
        (repo / 'docs' / 'x.md').write_text('z\n')
        g('commit', '-q', '-am', 'docs again')
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc', 'build-msvc'], repo) == 0, 'docs commit after a stamp should keep it')
        # new commit makes the stamp stale
        (repo / 'a.cpp').write_text('int d;\n')
        g('commit', '-q', '-am', 'more')
        check(cmd_land_check(base, 'HEAD', ['build-wt-msvc', 'build-msvc'], repo) == 1, 'stale stamp should fail')
    for b in bad:
        print('FAIL: ' + b)
    print('verified_build selftest: ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--selftest', action='store_true')
    sub = ap.add_subparsers(dest='cmd')
    b = sub.add_parser('begin')
    b.add_argument('tree')
    b.add_argument('--modules', default='all')
    b.add_argument('--tests', default='')
    p = sub.add_parser('pass')
    p.add_argument('tree')
    lc = sub.add_parser('land-check')
    lc.add_argument('--base', default='origin/dev')
    lc.add_argument('--head', default='HEAD')
    lc.add_argument('--tree', action='append', default=[])
    a = ap.parse_args(argv)
    cwd = os.getcwd()
    if a.selftest:
        return selftest()
    if a.cmd == 'begin':
        return cmd_begin(Path(a.tree), a.modules, a.tests, cwd)
    if a.cmd == 'pass':
        return cmd_pass(Path(a.tree))
    if a.cmd == 'land-check':
        rc, top = git(cwd, 'rev-parse', '--show-toplevel')  # trees live at the worktree root, wherever this runs from
        return cmd_land_check(a.base, a.head, a.tree or ['build-wt-msvc', 'build-msvc'], top if rc == 0 and top else cwd)
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
