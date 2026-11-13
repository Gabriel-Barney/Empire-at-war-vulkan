"""Golden-only merge conflicts in tools\\land_on_dev.cmd (2026-11-13).

When `git merge origin/dev` stops on conflicts and every conflicted file is a golden that tools\\regen_goldens.cmd can
write, the documented manual fix is "take dev's, then regenerate". land_on_dev.cmd does it with this script:
  resolve   after the failed merge: takes origin/dev's version of each conflicted golden, commits the merge and saves the
            list in the git dir (alamo_golden_merge.txt). Exit 1, merge left as it is, when any conflicted file is not
            such a golden (or the checkout fails): resolve those by hand as before.
  regen     after land_on_dev built the merged HEAD (build_agent.cmd msvc wt all none): runs regen_goldens for the listed
            goldens only, puts back any other golden it rewrote (the land check's fast tier then shows that drift as a
            failure, for a person to look at) and commits the listed ones. Exit 1 when regen_goldens failed.
Goldens it handles: engine/apps/headless/testdata/*.crc, engine/apps/game/testdata/game_smoke_*.crc / .rec and
tests/benchmark/* (scenario files). Goldens a test exe owns (--regen / --write-golden) stay manual.
Selftest: golden_merge.py --selftest
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIST = 'alamo_golden_merge.txt'
HEADLESS = re.compile(r'^engine/apps/headless/testdata/[^/]+\.crc$')
SMOKE = re.compile(r'^engine/apps/game/testdata/game_smoke_[a-z]+\.(?:crc|rec)$')
BENCH = re.compile(r'^tests/benchmark/[^/]+\.(?:abench|alsv|acmd|crc)$')


def kind(path):
    p = path.replace('\\', '/')
    return 'headless' if HEADLESS.match(p) else 'game' if SMOKE.match(p) else 'bench' if BENCH.match(p) else None


def git(cwd, *args):
    p = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout.strip(), p.stderr.strip()


def list_path(cwd) -> Path:
    _, p, _ = git(cwd, 'rev-parse', '--git-path', LIST)
    return Path(cwd) / p


def cmd_resolve(cwd) -> int:
    _, out, _ = git(cwd, 'diff', '--name-only', '--diff-filter=U')
    files = [f for f in out.splitlines() if f]
    if not files:
        print('golden_merge: no conflicted files')
        return 1
    other = [f for f in files if not kind(f)]
    if other:
        print('golden_merge: conflicts outside the goldens regen_goldens writes, resolve by hand:')
        for f in other:
            print('  ' + f)
        return 1
    for f in files:
        rc, _, err = git(cwd, 'checkout', '--theirs', '--', f)
        if rc:
            print(f'golden_merge: cannot take origin/dev\'s {f} ({err}); resolve by hand')
            return 1
        git(cwd, 'add', '--', f)
    rc, _, err = git(cwd, 'commit', '--no-edit')
    if rc:
        print(f'golden_merge: merge commit failed: {err}')
        return 1
    list_path(cwd).write_text(''.join(f + '\n' for f in files), encoding='utf8')
    print(f'golden_merge: took origin/dev\'s version of {len(files)} conflicted golden(s) and committed the merge:')
    for f in files:
        print('  ' + f)
    return 0


def regen_runs(files):
    """regen_goldens.py argument lists that cover the listed goldens."""
    kinds = {kind(f) for f in files}
    runs = []
    if 'headless' in kinds or 'bench' in kinds:
        args = ['--only', 'headless' if 'headless' in kinds else 'bench']
        if 'headless' in kinds:
            names = sorted(Path(f).name for f in files if kind(f) == 'headless')
            args += ['--match', '^(' + '|'.join(re.escape(n) for n in names) + ')$']
            if 'bench' not in kinds:
                args.append('--no-bench')
        runs.append(args)
    if 'game' in kinds:
        runs.append(['--only', 'game'])
    return runs


def cmd_regen(cwd, tree) -> int:
    lp = list_path(cwd)
    if not lp.is_file():
        print('golden_merge: no golden merge pending')
        return 0
    files = [f for f in lp.read_text(encoding='utf8').splitlines() if f]
    _, dirty, _ = git(cwd, 'status', '--porcelain', '-uno')
    if dirty:  # everything regen_goldens changes is put back or committed below, so start from a clean tree
        print('golden_merge: tracked files have uncommitted edits; commit them first:\n' + dirty)
        return 1
    script = ROOT / 'tools' / 'regen_goldens.cmd'
    failed = False
    for args in regen_runs(files):
        # --force: the second run sees the first run's goldens as uncommitted edits
        full = ['--build-dir', tree, '--force', *args]
        line = ' '.join(f'"{a}"' for a in full)
        print(f'golden_merge: regen_goldens {line}', flush=True)
        r = subprocess.run(f'cmd.exe /d /s /c ""{script}" {line}"', cwd=cwd)
        failed |= r.returncode != 0
    _, out, _ = git(cwd, 'diff', '--name-only')
    changed = [f for f in out.splitlines() if f]
    extra = [f for f in changed if f not in files]
    if extra:
        git(cwd, 'checkout', '--', *extra)
        print('golden_merge: put back goldens outside the conflict that regen_goldens also changed (the merged code changes '
              'them: the land check will fail on them; look before regenerating):')
        for f in extra:
            print('  ' + f)
    mine = [f for f in changed if f in files]
    if failed:
        print('golden_merge: regen_goldens failed (above); nothing committed. Fix it, then regenerate and commit by hand.')
        git(cwd, 'checkout', '--', *mine) if mine else None
        return 1
    if mine:
        git(cwd, 'add', '--', *mine)
        msg = ('Regenerate goldens after merging origin/dev (land_on_dev: golden-only merge conflict)\n\n'
               + ''.join(f'- {f}\n' for f in mine))
        rc, _, err = git(cwd, 'commit', '-m', msg)
        if rc:
            print(f'golden_merge: commit failed: {err}')
            return 1
        print(f'golden_merge: committed {len(mine)} regenerated golden(s)')
    else:
        print('golden_merge: origin/dev\'s goldens already match the merged code; nothing to commit')
    lp.unlink()
    return 0


def selftest() -> int:
    bad = []

    def check(c, msg):
        if not c:
            bad.append(msg)

    check(kind('engine/apps/headless/testdata/skirmish_ai_foc_land_golden.crc') == 'headless', 'headless kind')
    check(kind('engine/apps/headless/testdata/galactic_ai_hotm_golden.txt') is None, 'AI .txt golden is manual')
    check(kind('engine/apps/game/testdata/game_smoke_land.rec') == 'game', 'smoke kind')
    check(kind('tests/benchmark/land_battle.alsv') == 'bench', 'bench kind')
    check(kind('engine/ai/tests/golden/ai_golden.txt') is None, 'test-owned golden is manual')
    runs = regen_runs(['engine/apps/headless/testdata/a.b.crc', 'engine/apps/game/testdata/game_smoke_land.crc'])
    check(runs == [['--only', 'headless', '--match', r'^(a\.b\.crc)$', '--no-bench'], ['--only', 'game']], f'runs {runs}')
    check(regen_runs(['tests/benchmark/x.crc']) == [['--only', 'bench']], 'bench-only run')
    with tempfile.TemporaryDirectory() as td:
        repo = Path(td)

        def g(*a):
            subprocess.run(['git', *a], cwd=repo, check=True, capture_output=True)

        g('init', '-q', '-b', 'dev')
        g('config', 'user.email', 'selftest@example.invalid')
        g('config', 'user.name', 'selftest')
        gold = repo / 'engine/apps/headless/testdata'
        gold.mkdir(parents=True)
        (gold / 'g.crc').write_text('base\n')
        (repo / 'a.cpp').write_text('int a;\n')
        g('add', '.')
        g('commit', '-q', '-m', 'base')
        g('checkout', '-q', '-b', 'work')
        (gold / 'g.crc').write_text('mine\n')
        g('commit', '-q', '-am', 'mine')
        g('checkout', '-q', 'dev')
        (gold / 'g.crc').write_text('dev\n')
        g('commit', '-q', '-am', 'dev')
        g('checkout', '-q', 'work')
        check(subprocess.run(['git', 'merge', '--no-edit', 'dev'], cwd=repo, capture_output=True).returncode != 0, 'conflict')
        check(cmd_resolve(repo) == 0, 'golden-only conflict resolves')
        check((gold / 'g.crc').read_text() == 'dev\n', 'took dev\'s golden')
        check(git(repo, 'status', '--porcelain', '-uno')[1] == '', 'merge committed')
        check(list_path(repo).read_text() == 'engine/apps/headless/testdata/g.crc\n', 'list saved')
        # a code conflict is left for a person
        (repo / 'a.cpp').write_text('int w;\n')
        g('commit', '-q', '-am', 'w')
        g('checkout', '-q', 'dev')
        (repo / 'a.cpp').write_text('int d;\n')
        g('commit', '-q', '-am', 'd')
        g('checkout', '-q', 'work')
        subprocess.run(['git', 'merge', '--no-edit', 'dev'], cwd=repo, capture_output=True)
        check(cmd_resolve(repo) == 1, 'a code conflict is not resolved')
        check('a.cpp' in git(repo, 'diff', '--name-only', '--diff-filter=U')[1], 'merge left in progress')
    for b in bad:
        print('FAIL: ' + b)
    print('golden_merge selftest: ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


def main(argv) -> int:
    cwd = os.getcwd()
    if argv == ['--selftest']:
        return selftest()
    if argv[:1] == ['resolve']:
        return cmd_resolve(cwd)
    if argv[:1] == ['regen']:
        return cmd_regen(cwd, argv[1] if len(argv) > 1 else 'build-wt-msvc')
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
