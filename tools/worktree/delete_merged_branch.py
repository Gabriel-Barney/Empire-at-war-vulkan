"""Delete remote claude/* branches that are already merged (2026-11-12).

Usage: python -I tools/worktree/delete_merged_branch.py [--dry-run] <branch> [<branch> ...]
       python -I tools/worktree/delete_merged_branch.py --selftest

Each branch must be named claude/... (an "origin/" prefix is accepted) and its tip on origin must be
contained in origin/dev or origin/main; anything else is refused and left alone. Deleted branches are
logged with their tip SHA to logs/deleted_branches_<date>.txt in the main checkout, so one can be
restored with "git push origin <sha>:refs/heads/<branch>".

This is the narrow form of "git push origin --delete <branch>" that .claude/settings.json allows
without a prompt: the plain command can drop unmerged work, this one cannot.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time

BASES = ('origin/dev', 'origin/main')


def git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(['git', *args], capture_output=True, text=True, check=check)


def normalize(name: str) -> str | None:
    n = name.strip()
    if n.startswith('refs/heads/'):
        n = n[len('refs/heads/'):]
    elif n.startswith('origin/'):
        n = n[len('origin/'):]
    if not n.startswith('claude/') or n == 'claude/' or '..' in n or any(c in n for c in ' ~^:?*[\\'):
        return None
    return n


def log_path() -> str:
    common = git('rev-parse', '--path-format=absolute', '--git-common-dir').stdout.strip()
    main = os.path.dirname(common.rstrip('/\\'))
    d = os.path.join(main, 'logs')
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, time.strftime('deleted_branches_%Y-%m-%d.txt'))


def main(argv: list[str]) -> int:
    if argv[:1] == ['--selftest']:
        return selftest()
    dry = '--dry-run' in argv
    names = [a for a in argv if a != '--dry-run']
    if not names:
        print(__doc__)
        return 2
    git('fetch', '-q', '--prune', 'origin')
    bad = 0
    for raw in names:
        b = normalize(raw)
        if b is None:
            print(f'refused {raw}: only claude/... branches')
            bad += 1
            continue
        tip = git('rev-parse', '--verify', '-q', f'refs/remotes/origin/{b}', check=False).stdout.strip()
        if not tip:
            print(f'skip {b}: not on origin')
            continue
        into = [base for base in BASES
                if git('merge-base', '--is-ancestor', tip, base, check=False).returncode == 0]
        if not into:
            print(f'refused {b}: {tip[:10]} is not merged into {" or ".join(BASES)}')
            bad += 1
            continue
        if dry:
            print(f'would delete {b} ({tip[:10]}, merged into {into[0]})')
            continue
        r = git('push', 'origin', '--delete', b, check=False)
        if r.returncode != 0:
            print(f'failed {b}: {r.stderr.strip()}')
            bad += 1
            continue
        with open(log_path(), 'a', encoding='utf-8') as f:
            f.write(f'{time.strftime("%Y-%m-%d %H:%M:%S")} {b} {tip} merged into {into[0]}\n')
        print(f'deleted {b} ({tip[:10]}, merged into {into[0]})')
    return 1 if bad else 0


def selftest() -> int:
    assert normalize('claude/foo-1a2b') == 'claude/foo-1a2b'
    assert normalize('origin/claude/foo') == 'claude/foo'
    assert normalize('refs/heads/claude/x/y') == 'claude/x/y'
    for n in ('dev', 'main', 'origin/dev', 'feature/x', 'claude/', 'claude/a..b', 'claude/a b', 'claude/*'):
        assert normalize(n) is None, n
    print('delete_merged_branch selftest: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
