#!/usr/bin/env python3
r"""PR check without hosted CI: build this checkout in its own tree, run the tests and (optionally) comment the result on the PR.

Usage (from the project / worktree root):
  tools\pr_check.cmd [PR] [--compiler msvc|clang|both] [--modules "core;..."] [--tests fast|all|<ctest regex>] [--post] [--tree NAME]

  PR          PR number; default: the open PR of the current branch (`gh pr view`).
  --compiler  default msvc; `both` also runs clang (use it for sim code: CLAUDE.md "Platform and compilers").
  --modules   default: every module (the ALAMO_ENGINE_MODULES default).
  --tests     default fast (ctest -LE slow); `all` runs the slow tier too (what the pre-merge gate does).
  --post      post the summary as a PR comment with `gh pr comment`. Without it the summary is only printed and written to
              logs\pr_check_<PR>.md (posting is visible to others, so it is opt-in).
  --tree      build tree name, default `wt` (build-wt-<compiler>, one tree per worktree, see tools\worktree).

The comment records the commit, whether the tree was dirty, the compiler(s), the test tier, warnings, test counts, skips and the
first failures, taken from tools\lint\build_summary.py. Exit code 0 only when every build passed with no warnings.
Selftest: python tools/pr_check/pr_check.py --selftest
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DEFAULT_MODULES = ("core;vfs;data;jobs;galactic;objects;anim;commands;tactical;lua;ai;assets;rhi;shaderxlat;"
                   "materials;terrain;render;ui;audio;video;apps")


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)


def git(*a):
    return run(['git', *a]).stdout.strip()


def find_pr(arg):
    if arg:
        return arg
    r = run(['gh', 'pr', 'view', '--json', 'number', '-q', '.number'])
    if r.returncode or not r.stdout.strip():
        sys.exit('pr_check: no PR given and the current branch has none (gh pr view failed)')
    return r.stdout.strip()


def build_one(compiler, tree, modules, tests):
    """Run build_agent.cmd for one compiler; returns (exit_code, summary_text, seconds)."""
    log = os.path.join('logs', f'agent_{tree}_{compiler}.txt')
    args = ['cmd', '/c', 'build_agent.cmd', compiler, tree, modules]
    if tests != 'fast':
        args.append(tests)
    t0 = time.time()
    r = run(args)
    secs = time.time() - t0
    s = run([sys.executable, os.path.join('tools', 'lint', 'build_summary.py'), log])
    return s.returncode, (s.stdout + s.stderr).strip(), secs


def format_comment(pr, sha, dirty, tests, results):
    ok = all(code == 0 for _, code, _, _ in results)
    lines = [f"**PR check {'PASS' if ok else 'FAIL'}** for `{sha[:10]}`" + (' (working tree had uncommitted edits)' if dirty else ''),
             f"tier: `{tests}` (fast = tests without the `slow` label; the pre-merge gate runs everything)", '']
    for compiler, code, text, secs in results:
        lines += [f"<details><summary>{compiler}: {'pass' if code == 0 else 'FAIL'} in {secs:.0f}s</summary>", '', '```', text[-3500:], '```', '', '</details>']
    lines += ['', 'Run locally with `tools\\pr_check.cmd` (no hosted CI).']
    return '\n'.join(lines)


def selftest():
    c = format_comment('1', 'abcdef0123456789', True, 'fast', [('msvc', 0, 'ok', 12.0), ('clang', 1, 'boom', 9.0)])
    assert 'FAIL' in c.splitlines()[0] and 'uncommitted' in c and 'clang: FAIL' in c and 'msvc: pass' in c
    c = format_comment('1', 'abcdef0123456789', False, 'all', [('msvc', 0, 'ok', 1.0)])
    assert 'PASS' in c.splitlines()[0] and 'uncommitted' not in c
    assert re.search(r'abcdef0123', c)
    print('pr_check selftest ok')
    return 0


def main():
    if '--selftest' in sys.argv:
        return selftest()
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument('pr', nargs='?')
    p.add_argument('--compiler', default='msvc', choices=['msvc', 'clang', 'both'])
    p.add_argument('--modules', default=DEFAULT_MODULES)
    p.add_argument('--tests', default='fast')
    p.add_argument('--post', action='store_true')
    p.add_argument('--tree', default='wt')
    a = p.parse_args()
    pr = find_pr(a.pr)
    sha = git('rev-parse', 'HEAD')
    dirty = bool(git('status', '--porcelain', '--untracked-files=no'))
    head = run(['gh', 'pr', 'view', pr, '--json', 'headRefOid', '-q', '.headRefOid']).stdout.strip()
    if head and head != sha:
        print(f'pr_check: warning: checkout {sha[:10]} is not the PR head {head[:10]}; the comment says which commit was built')
    results = []
    for compiler in (['msvc', 'clang'] if a.compiler == 'both' else [a.compiler]):
        print(f'pr_check: building {compiler} (tree build-{a.tree}-{compiler}) ...', flush=True)
        code, text, secs = build_one(compiler, a.tree, a.modules, a.tests)
        print(text)
        results.append((compiler, code, text, secs))
    comment = format_comment(pr, sha, dirty, a.tests, results)
    os.makedirs(os.path.join(ROOT, 'logs'), exist_ok=True)
    out = os.path.join(ROOT, 'logs', f'pr_check_{pr}.md')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(comment + '\n')
    print(f'pr_check: summary written to {out}')
    if a.post:
        r = run(['gh', 'pr', 'comment', pr, '--body-file', out])
        print('pr_check: comment posted' if r.returncode == 0 else f'pr_check: comment failed: {r.stderr}')
    return 0 if all(code == 0 for _, code, _, _ in results) else 1


if __name__ == '__main__':
    sys.exit(main())
