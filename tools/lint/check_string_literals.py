"""Find C / C++ string and character literals that do not end on their line (2026-11-12).

The most common compile error in the build records (MSVC C2001 "newline in string literal", 105 failed builds between
2026-10-30 and 2026-11-12) came from edits that turned a "\\n" escape into a real line break. This finds them in about
a second, before a build starts. Raw strings, comments, digit separators (1'000) and #error / #warning text are
understood; a backslash-newline inside a literal (legal, but not used here) counts as a continuation.

Usage:
  check_string_literals.py <file> ...       check these files
  check_string_literals.py --changed [--base origin/dev]
                                            C++ files changed against the merge base with --base, committed or not,
                                            plus untracked ones (build_agent.cmd / build_module.cmd run this first)
  check_string_literals.py --all            every tracked C++ file
  check_string_literals.py --hook           Claude Code PostToolUse hook: reads the hook JSON on stdin, checks the
                                            edited file; exit 2 with the finding on stderr, so the agent sees it
  check_string_literals.py --selftest
Exit 1 when a literal is unterminated (2 in --hook mode), else 0.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

EXTS = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ipp'}

TOKEN = re.compile(r'''
   (?P<pp>^[ \t]*\#[ \t]*(?:error|warning)\b[^\n]*)
 | (?P<raw>(?:u8|[uUL])?R"(?P<d>[^()\\\s"]{0,16})\([\s\S]*?\)(?P=d)")
 | (?P<bc>/\*[\s\S]*?\*/)
 | (?P<lc>//[^\n]*)
 | (?P<str>(?:u8|[uUL])?"(?:[^"\\\n]|\\[\s\S])*")
 | (?P<chr>(?:u8|[uUL])?'(?:[^'\\\n]|\\[\s\S])+')
 | (?P<num>\.?\d(?:[eEpP][+-]|['\w.])*)
 | (?P<id>[A-Za-z_]\w*)
 | (?P<bad>["'])
''', re.X | re.M)


def find_bad(text: str):
    """[(line, column, kind)] for every quote that does not start a complete literal."""
    out = []
    for m in TOKEN.finditer(text):
        if m.lastgroup == 'bad':
            pos = m.start()
            line = text.count('\n', 0, pos) + 1
            col = pos - (text.rfind('\n', 0, pos) + 1) + 1
            out.append((line, col, 'string' if m.group() == '"' else 'character'))
    return out


def check_file(path: Path, out=print) -> int:
    try:
        text = path.read_text(encoding='utf8', errors='replace')
    except OSError:
        return 0
    text = text.replace('\r\n', '\n')
    bad = find_bad(text)
    lines = text.split('\n')
    for line, col, kind in bad[:5]:
        src = lines[line - 1].strip() if line - 1 < len(lines) else ''
        out(f'{path}:{line}:{col}: unterminated {kind} literal: {src[:160]}')
    if bad:
        out(f'  (an edit may have turned a "\\n" escape into a real line break; {len(bad)} in this file)')
    return 1 if bad else 0


def git_lines(*args):
    p = subprocess.run(['git', *args], capture_output=True, text=True)
    return p.stdout.splitlines() if p.returncode == 0 else []


def changed_files(base: str):
    mb = git_lines('merge-base', base, 'HEAD')
    ref = mb[0] if mb else 'HEAD'
    top = git_lines('rev-parse', '--show-toplevel')
    root = Path(top[0]) if top else Path.cwd()
    names = set(git_lines('diff', '--name-only', '--diff-filter=ACMR', ref))
    names |= set(git_lines('ls-files', '--others', '--exclude-standard'))
    return [root / n for n in sorted(names) if Path(n).suffix.lower() in EXTS]


def all_files():
    top = git_lines('rev-parse', '--show-toplevel')
    root = Path(top[0]) if top else Path.cwd()
    return [root / n for n in git_lines('ls-files') if Path(n).suffix.lower() in EXTS]


def hook() -> int:
    try:
        data = json.loads(sys.stdin.read() or '{}')
        fp = (data.get('tool_input') or {}).get('file_path') or ''
    except (ValueError, AttributeError):
        return 0  # never get in the way when the hook input is not what we expect
    if not fp or Path(fp).suffix.lower() not in EXTS:
        return 0
    msgs = []
    if check_file(Path(fp), out=msgs.append):
        print('\n'.join(msgs), file=sys.stderr)
        return 2
    return 0


def selftest() -> int:
    good = r'''
#include <cstdio>
#error don't build this on old compilers
// it's a comment with "quotes
/* and a block's comment
   "spanning lines */
int a = 1'000'000; auto b = 0x1'FF; double c = 1.5e+3'0;
const char* s = "a \"quoted\" \\ string\n";
const char* r = R"x(raw "string"
spanning ' lines)x";
auto w = L"wide"; auto u = u8"utf8"; char q = '\''; char e = '"'; wchar_t z = L'x';
auto ud = "abc"_sv; int t = sizeof('a');
'''
    bad1 = 'const char* s = "first line\nsecond line";\n'
    bad2 = "char c = 'x;\n"
    fails = []
    if find_bad(good):
        fails.append(f'false positive: {find_bad(good)}')
    if [k for _, _, k in find_bad(bad1)] != ['string', 'string']:
        fails.append(f'missed split string: {find_bad(bad1)}')
    if [k for _, _, k in find_bad(bad2)] != ['character']:
        fails.append(f'missed char: {find_bad(bad2)}')
    if find_bad(bad1)[0][:2] != (1, 17):
        fails.append(f'wrong position: {find_bad(bad1)[0]}')
    for f in fails:
        print('FAIL: ' + f)
    print('check_string_literals selftest: ' + ('FAILED' if fails else 'ok'))
    return 1 if fails else 0


def main(argv) -> int:
    if '--selftest' in argv:
        return selftest()
    if '--hook' in argv:
        return hook()
    if '--changed' in argv:
        base = argv[argv.index('--base') + 1] if '--base' in argv else 'origin/dev'
        files = changed_files(base)
    elif '--all' in argv:
        files = all_files()
    else:
        files = [Path(a) for a in argv if not a.startswith('--')]
    rc = 0
    for f in files:
        rc |= check_file(f)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
