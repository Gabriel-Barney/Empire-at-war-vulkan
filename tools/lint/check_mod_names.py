"""modc t04 lint: no mod or faction name literals in engine/ sim code (docs/MODS.md §1 rule 1, docs/status/modc-audit.md).

    python tools/lint/check_mod_names.py [--engine DIR] [--names FILE] [--quiet]
    python tools/lint/check_mod_names.py --selftest

Scans the sim code of engine/ plus the game's presentation host and UI (SCOPE below: every module except render, audio
and video; engine/apps/game/app and engine/ui since modc t20; tests, testdata and tools folders are never scanned) for string literals that contain a name of
tools/lint/mod_names.txt as a whole token, or that are a Steam workshop item id (9+ digits). Comments are not code and
are never flagged. Exit 1 on any violation or malformed marker; 0 otherwise.

Allowed uses carry a marker in a comment (the reason is required; it cites the docs/re spec and address for a retail
name, or the roadmap task that replaces a kept port rule):
    [name-ok: reason]          on the line of the literal, or in the comment lines directly above the statement
                               (applies to the next line that has code)
    [name-ok-begin: reason]    ... [name-ok-end]   every literal between the two lines
"""
import re
import sys
from pathlib import Path

SCOPE = ['core', 'jobs', 'data', 'galactic', 'objects', 'anim', 'commands', 'tactical', 'ai', 'lua', 'vfs',
         'apps/sim', 'apps/headless', 'apps/game/src', 'apps/game/rules', 'apps/game/include',
         'apps/game/app', 'ui']  # modc t20: the presentation host and the UI
SKIP_DIRS = {'tests', 'testdata', 'tools', 'bench', 'benches'}
EXTS = {'.cpp', '.hpp', '.h', '.inl', '.cc'}
WORKSHOP_ID = re.compile(r'^[1-9]\d{8,10}$')
MARK = re.compile(r'\[name-ok(-begin)?:\s*([^\]]*)\]')
MARK_END = re.compile(r'\[name-ok-end\]')


def load_names(path):
    out = []
    for line in Path(path).read_text(encoding='utf8').splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            out.append(line.upper())
    return out


def tokenize(text):
    """Literals as (line, content) and comments as (line, text, starts_line) where starts_line = no code before it on
    that line. Also the set of lines that hold code (anything outside comments and whitespace)."""
    literals, comments, code_lines = [], [], set()
    i, n, line = 0, len(text), 1
    line_has_code = False
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
            line_has_code = False
            i += 1
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            comments.append((line, text[i:j], not line_has_code))
            i = j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            body = text[i:j]
            comments.append((line, body, not line_has_code))
            line += body.count('\n')
            i = j
            continue
        if c.isspace():
            i += 1
            continue
        code_lines.add(line)
        line_has_code = True
        # Raw string: R"delim( ... )delim" (with an optional u8 / u / U / L prefix before R).
        m = re.match(r'(?:u8|u|U|L)?R"([^()\\ \t\n]{0,16})\(', text[i:i + 24])
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            close = ')' + m.group(1) + '"'
            start = i + m.end()
            j = text.find(close, start)
            j = n if j < 0 else j
            literals.append((line, text[start:j]))
            line += text[i:j].count('\n')
            i = j + len(close)
            continue
        if c == '"' or c == "'":
            j = i + 1
            buf = []
            while j < n and text[j] != c and text[j] != '\n':
                if text[j] == '\\' and j + 1 < n:
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            if c == '"':
                literals.append((line, ''.join(buf)))
            i = j + 1
            continue
        # Identifiers and numbers in one step (a digit separator ' must not open a char literal: 1'000).
        if c.isalnum() or c == '_':
            j = i
            while j < n and (text[j].isalnum() or text[j] == '_' or (text[j] == "'" and j > i and text[j - 1].isdigit())):
                j += 1
            i = j
            continue
        i += 1
    return literals, comments, code_lines


def find_names(content, names):
    up = content.upper()
    hits = []
    for name in names:
        if name.startswith('='):  # whole literal only
            if up.strip() == name[1:]:
                hits.append(name[1:])
            continue
        start = 0
        while True:
            k = up.find(name, start)
            if k < 0:
                break
            before = up[k - 1] if k > 0 else ''
            after = up[k + len(name)] if k + len(name) < len(up) else ''
            if not before.isalnum() and not after.isalnum():
                hits.append(name)
                break
            start = k + 1
    if WORKSHOP_ID.match(content.strip()):
        hits.append('workshop id')
    return hits


def comment_blocks(comments):
    """Consecutive own-line comments form one block (a marker may wrap over several // lines); a comment after code
    is a block of its own. Yields (first line, last line, joined text, starts_line)."""
    blocks = []
    for line, body, starts_line in comments:
        text = re.sub(r'^\s*(//|/\*)', '', body)
        text = re.sub(r'\*/\s*$', '', text)
        last = line + body.count('\n')
        if blocks and starts_line and blocks[-1][3] and line == blocks[-1][1] + 1:
            first, _, joined, _ = blocks[-1]
            blocks[-1] = (first, last, joined + ' ' + text.strip(), True)
        else:
            blocks.append((line, last, text.strip(), starts_line))
    return blocks


def statement_lines(start, code_lines, src_lines):
    """The code lines of the statement that starts on code line `start`: up to the first line whose code ends with
    ';', '{' or '}' (so a marker above `if (x)` + `for (... "Name" ...) {` covers both lines)."""
    out = []
    for ln in sorted(l for l in code_lines if l >= start):
        out.append(ln)
        code = src_lines[ln - 1].split('//')[0].rstrip() if ln - 1 < len(src_lines) else ''
        if code.endswith((';', '{', '}')) or len(out) >= 8:
            break
    return out


def allowed_lines(comments, code_lines, last_line, src_lines=()):
    """Lines whose literals a marker allows, and marker errors."""
    allowed, errors = set(), []
    region = None  # start line of an open region
    for line, end_line, body, starts_line in comment_blocks(comments):
        for m in MARK.finditer(body):
            if not m.group(2).strip():
                errors.append((line, 'marker without a reason'))
            if m.group(1):  # begin
                if region is not None:
                    errors.append((line, f'name-ok-begin inside the region opened on line {region}'))
                region = line
            else:
                allowed.update(range(line, end_line + 1))
                if starts_line:  # a comment block of its own: the next statement
                    nxt = next((ln for ln in sorted(code_lines) if ln > end_line), None)
                    if nxt is not None:
                        allowed.update(statement_lines(nxt, code_lines, src_lines))
        line = end_line
        if MARK_END.search(body):
            if region is None:
                errors.append((line, 'name-ok-end without name-ok-begin'))
            else:
                allowed.update(range(region, line + 1))
                region = None
    if region is not None:
        errors.append((region, 'name-ok-begin without name-ok-end'))
        allowed.update(range(region, last_line + 1))
    return allowed, errors


def check_file(path, rel, names, out):
    text = path.read_text(encoding='utf8', errors='replace')
    literals, comments, code_lines = tokenize(text)
    allowed, errors = allowed_lines(comments, code_lines, text.count('\n') + 1, text.split('\n'))
    bad = 0
    ok = 0
    for line, msg in errors:
        out.append(f'MARKER {rel}:{line}: {msg}')
        bad += 1
    for line, content in literals:
        hits = find_names(content, names)
        if not hits:
            continue
        if line in allowed:
            ok += 1
            continue
        shown = content if len(content) <= 60 else content[:57] + '...'
        out.append(f'VIOLATION {rel}:{line}: "{shown}" ({", ".join(hits)}) -> data-driven, or a [name-ok: reason] marker')
        bad += 1
    return bad, ok


def scan(engine, names, scope=SCOPE):
    files = []
    for mod in scope:
        root = engine / mod
        if not root.is_dir():
            continue
        for p in sorted(root.rglob('*')):
            if p.suffix.lower() not in EXTS or not p.is_file():
                continue
            if SKIP_DIRS.intersection(part.lower() for part in p.relative_to(engine).parts[:-1]):
                continue
            files.append(p)
    files = sorted(set(files))
    out, bad, ok = [], 0, 0
    for p in files:
        b, o = check_file(p, p.relative_to(engine).as_posix(), names, out)
        bad += b
        ok += o
    return files, out, bad, ok


def run(engine, names_file, quiet=False):
    names = load_names(names_file)
    files, out, bad, ok = scan(Path(engine), names)
    for line in out:
        print(line)
    if bad:
        print(f'check_mod_names: {bad} violation(s) in {len(files)} files (docs/status/modc-audit.md)')
        return 1
    if not quiet:
        print(f'check_mod_names: OK ({len(files)} files, {ok} allowed by marker, {len(names)} names)')
    return 0


def selftest(here):
    """The fixture engine/ under tools/lint/fixture_mod_names must give exactly the expected findings."""
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    names = load_names(here / 'mod_names.txt')
    # Unit checks of the tokenizer and the token rule.
    lits, comments, _ = tokenize('a = "x\\"Empire"; // "Rebel"\n/* "Hutts" */ b = R"d(Neutral\n)d"; c = 1\'000; d = \'"\';')
    expect([c for _, c in lits] == ['x\\"Empire', 'Neutral\n'], f'tokenizer literals {lits}')
    expect(len(comments) == 2, f'tokenizer comments {comments}')
    expect(find_names('Rebel_Infantry_Squad', names) == ['REBEL'], 'token at an underscore')
    expect(find_names('BasicRebel', names) == [] and find_names('TEXT_FACTION_REBELLION', names) == [], 'no partial tokens')
    expect(find_names('Is_Neutral', names) == [] and find_names(' neutral ', names) == ['NEUTRAL'], 'whole-literal names')
    expect(find_names('0123456789', names) == [], 'digits that are no workshop id')
    expect(find_names('1125571106', names) == ['workshop id'], 'workshop id')
    expect(find_names('Data\\XML\\FactionFiles.xml', names) == [], 'plain literal')
    # The fixture tree.
    files, out, bad, ok = scan(here / 'fixture_mod_names', names)
    got = sorted(o.split(':')[0] + ':' + o.split(':')[1] for o in out)
    want = sorted(['VIOLATION galactic/src/bad.cpp:4', 'VIOLATION galactic/src/bad.cpp:5', 'VIOLATION galactic/src/bad.cpp:7',
                   'VIOLATION galactic/src/bad.cpp:9', 'VIOLATION galactic/src/bad.cpp:12', 'MARKER galactic/src/bad.cpp:14',
                   'MARKER galactic/src/bad.cpp:16', 'VIOLATION galactic/src/bad.cpp:17'])
    expect(got == want, 'fixture findings:\n  got  ' + '\n       '.join(got) + '\n  want ' + '\n       '.join(want))
    expect(ok == 8, f'fixture: {ok} literals allowed by marker, want 8')
    expect(len(files) == 2, f'fixture: {len(files)} files scanned, want 2 (tests/ is skipped)')
    for f in fails:
        print('SELFTEST FAIL', f)
    if fails:
        return 1
    print(f'check_mod_names selftest: OK ({len(out)} expected findings, {ok} allowed)')
    return 0


def main(argv):
    here = Path(__file__).resolve().parent
    engine = here.parent.parent / 'engine'
    names = here / 'mod_names.txt'
    quiet = False
    args = list(argv)
    if '--selftest' in args:
        return selftest(here)
    while args:
        a = args.pop(0)
        if a == '--engine' and args:
            engine = Path(args.pop(0))
        elif a == '--names' and args:
            names = Path(args.pop(0))
        elif a == '--quiet':
            quiet = True
        else:
            print(__doc__)
            return 2
    return run(engine, names, quiet)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
