#!/usr/bin/env python3
"""Split one huge single-file test .cpp into <stem>_common.hpp + <stem>.cpp (main) + <stem>_partN.cpp (bo3 t03).

Pure mechanical refactor of tests that are `static` free functions called from main():
  * every top-level `static <ret> name(...) { ... }` function that is not a template and has no `auto`
    return type becomes a movable function: it loses `static`, is declared in the common header and
    defined in one of the part files (parts are filled in file order up to --part-kb);
  * everything else (includes, usings, types, constants, helper variables, `namespace { }` blocks,
    `auto` / template / inline functions) goes to the common header in file order. The anonymous
    namespace of the original becomes a named one (`bo3split`), so types and helpers are shared by
    every part instead of being per-TU copies; functions and variables there become `inline`;
  * main() stays in <stem>.cpp.
Preprocessor conditionals around a movable function are repeated around its declaration/definition.

  python tools/analysis/split_test_cpp.py <file.cpp> [--part-kb 120] [--dry-run]
"""
import argparse, os, re, sys

NS = "bo3split"


def scan(text):
    """Return top-level chunks: list of (start, end) covering text; end exclusive."""
    n = len(text)
    i = 0
    chunks = []
    start = 0
    brace = paren = 0
    code_started = False  # a code token seen since `start`
    line_start = True
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '\n':
            line_start = True
            i += 1
            continue
        if c in ' \t\r':
            i += 1
            continue
        if c == '#' and line_start and brace == 0 and paren == 0:
            if code_started:
                pass  # a directive inside a declaration: leave it
            else:
                j = i
                while True:
                    k = text.find('\n', j)
                    if k < 0:
                        k = n
                        break
                    if text[k - 1] == '\\' or text[k - 1:k + 0] == '\\':
                        j = k + 1
                        continue
                    if text[k - 1] == '\r' and k >= 2 and text[k - 2] == '\\':
                        j = k + 1
                        continue
                    break
                chunks.append((start, min(k + 1, n), 'pp'))
                i = start = min(k + 1, n)
                line_start = True
                continue
        line_start = False
        if c == 'R' and text.startswith('R"', i):
            m = re.match(r'R"([^(\s]*)\(', text[i:i + 40])
            if m:
                end = text.find(')' + m.group(1) + '"', i)
                i = end + len(m.group(1)) + 2
                code_started = True
                continue
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == '\\' else 1
            i += 1
            code_started = True
            continue
        if c == "'":
            # digit separator 1'000 vs char literal
            if i > 0 and (text[i - 1].isalnum()) and i + 1 < n and text[i + 1].isalnum() and not re.match(r"'(\\.|[^'\\])'", text[i:i + 4]):
                i += 1
                continue
            i += 1
            while text[i] != "'":
                i += 2 if text[i] == '\\' else 1
            i += 1
            code_started = True
            continue
        code_started = True
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif c == '{':
            brace += 1
        elif c == '}':
            brace -= 1
            if brace == 0 and paren == 0:
                j = i + 1
                while j < n and text[j] in ' \t':
                    j += 1
                if j < n and text[j] == ';':
                    i = j
                i += 1
                # consume rest of the line (trailing comment)
                k = text.find('\n', i)
                k = n if k < 0 else k + 1
                tail = text[i:k]
                if tail.strip() == '' or tail.lstrip().startswith('//'):
                    i = k
                chunks.append((start, i, 'code'))
                start = i
                code_started = False
                line_start = True
                continue
        elif c == ';' and brace == 0 and paren == 0:
            i += 1
            k = text.find('\n', i)
            k = n if k < 0 else k + 1
            tail = text[i:k]
            if tail.strip() == '' or tail.lstrip().startswith('//'):
                i = k
            chunks.append((start, i, 'code'))
            start = i
            code_started = False
            line_start = True
            continue
        i += 1
    if start < n:
        chunks.append((start, n, 'tail'))
    return chunks


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def head(code):
    """Declaration head: text before the first top-level '{' or ';' (comments removed)."""
    s = strip_comments(code)
    depth = 0
    for k, ch in enumerate(s):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch in '{;' and depth == 0:
            return s[:k].strip()
    return s.strip()


FUNC_HEAD = re.compile(r'^static\s+(?!constexpr|const\s+\w+\s*(?:=|\[))(?P<sig>[\w:<>,\s\*&]+?)\b(?P<name>\w+)\s*\((?P<args>.*)\)\s*(?:const\s*)?(?:noexcept\s*)?$', re.S)


FUNC_HEAD2 = re.compile(r'^(?:static\s+)?(?P<sig>[\w:<>,\s\*&]+?)\b(?P<name>\w+)\s*\((?P<args>.*)\)\s*(?:const\s*)?(?:noexcept\s*)?$', re.S)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--part-kb', type=int, default=120)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--source', action='store_true', help='implementation file (no main): move Class::member definitions to parts')
    a = ap.parse_args()
    path = a.file
    raw = open(path, encoding='utf-8', newline='').read()
    crlf = '\r\n' in raw
    text = raw.replace('\r\n', '\n')
    stem = os.path.splitext(os.path.basename(path))[0]
    d = os.path.dirname(path)
    prefix = suffix = ''
    outer = None
    if a.source:
        mo = re.search(r'^namespace ([\w:]+) \{[ \t]*\n', text, re.M)
        if mo:
            close = text.rstrip().rfind('\n}')
            outer = mo.group(1)
            prefix = text[:mo.start()]
            suffix = text[close + 1:]
            text = text[mo.end():close + 1]
    chunks = scan(text)
    assert ''.join(text[s:e] for s, e, _ in chunks) == text

    cond = []  # stack of directive texts
    items = []  # dict per chunk
    for s, e, kind in chunks:
        t = text[s:e]
        if kind == 'pp':
            ds = strip_comments(t).strip()
            m = re.match(r'#\s*(\w+)', ds)
            dname = m.group(1) if m else ''
            if dname in ('if', 'ifdef', 'ifndef'):
                cond.append(ds)
            elif dname in ('else', 'elif'):
                cond.append(('ELSE', ds)) if False else None
                if cond:
                    cond[-1] = (cond[-1], ds)  # marks a multi-branch conditional
            elif dname == 'endif':
                if cond:
                    cond.pop()
            items.append({'t': t, 'kind': 'pp', 'cond': list(cond)})
            continue
        h = head(t)
        it = {'t': t, 'kind': 'code', 'cond': list(cond), 'h': h}
        items.append(it)

    # find main
    if a.source:
        main_idx = len(items)
    else:
        main_idx = next(i for i, it in enumerate(items) if it['kind'] == 'code' and re.match(r'^int\s+main\s*\(', it['h']))
    movable = []
    member_defs = set()
    if a.source:
        for i, it in enumerate(items):
            if it['kind'] != 'code' or it['cond'] and any(isinstance(c, tuple) for c in it['cond']):
                continue
            h = it['h']
            pre = h.split('=')[0]
            if ('(' in pre and ('::' in pre.split('(')[0] or not re.match(chr(94)+'(static|inline|constexpr|template|extern)'+chr(92)+'b', h)) and '{' in strip_comments(it['t']) and not h.startswith('template')
                    and not re.match(r'^(struct|class|enum|union|namespace)\b', h) and not re.search(r'\boperator\b', pre.split('(')[0])):
                member_defs.add(i)
    for i, it in enumerate(items):
        if a.source:
            break
        if it['kind'] != 'code' or i >= main_idx:
            continue
        h = it['h']
        if h.startswith('template') or not h.startswith('static'):
            continue
        if '{' not in strip_comments(it['t']):
            continue
        m = FUNC_HEAD.match(h)
        if not m or re.search(r'\bauto\b', m.group('sig')) or 'constexpr' in h.split('(')[0] or 'inline' in h.split('(')[0]:
            continue
        if re.search(r'=\s*\{|=\s*\[', h):
            continue
        it['fn'] = m.group('name')
        it['sig'] = m.group('sig').strip()
        it['args'] = m.group('args').strip()
        movable.append(i)
    # conditionals with else-branches around movables: refuse
    for i in movable:
        if any(isinstance(c, tuple) for c in items[i]['cond']):
            it = items[i]
            it.pop('fn')
            movable.remove(i)
    print(f'{stem}: {len(chunks)} chunks, {len(movable)} movable functions, main at chunk {main_idx}')

    def wrap(cs, body):
        if not cs:
            return body
        pre = '\n'.join(cs) + '\n'
        post = '#endif\n' * len(cs)
        if not body.endswith('\n'):
            body += '\n'
        return pre + body + post

    # ---- header
    hdr = []
    decls = []
    parts = [[]]
    psize = 0
    in_ns = False
    mainonly = []
    st = {'psize': 0}

    def add_part(code, cond, ns=False):
        if st['psize'] > a.part_kb * 1024 and parts[-1]:
            parts.append([])
            st['psize'] = 0
        body = wrap(cond, code)
        if ns:
            body = f'namespace {NS} {{\n' + body + f'}}  // namespace {NS}\n'
        parts[-1].append(body)
        st['psize'] += len(code)

    for i, it in enumerate(items):
        if i >= main_idx:
            break
        t = it['t']
        if it['kind'] == 'code' and re.match(r'^void\s*\*?\s*operator\b', it['h']):
            mainonly.append(t)
            continue
        if i in member_defs:
            add_part(t, it['cond'])
            continue
        if (a.source and it['kind'] == 'code' and '::' in it['h'].split('(')[0] and i not in member_defs
                and not re.match(chr(94) + '(namespace|using|struct|class|enum|union|typedef|static_assert|extern|template)' + chr(92) + 'b', it['h'])):
            mainonly.append(t)  # static data member definitions etc.: one TU
            continue
        if i in movable:
            code = t
            # strip `static ` from the first code occurrence
            code = lead_static(code, '')
            code = strip_defaults(code, it['fn'])
            decl_head = it['sig'] + ' ' + it['fn'] + '(' + it['args'] + ');'
            hdr.append(wrap(it['cond'], decl_head + '\n'))
            add_part(code, it['cond'])
            continue
        if it['kind'] == 'pp':
            hdr.append(t)
            continue
        h = it['h']
        if h == 'namespace':
            t = t.replace('namespace {', f'namespace {NS} {{', 1)
            t = re.sub(r'(\n\}\s*//[^\n]*)?\n*$', lambda m: m.group(0), t)
            t = t.rstrip('\n') + f'\nusing namespace {NS};\n'
            # inside the block: functions/vars need inline
            hdr.append(inline_block(t, add_part))
            continue
        hdr.append(inline_top(t, it))
    main_text = ''.join(it['t'] for it in items[main_idx:])
    if a.source:
        hdr_text = ''.join(hdr)
        ns_open = f'namespace {outer} {{\n' if outer else ''
        ns_close = f'}}  // namespace {outer}\n' if outer else ''
        iname = stem + '_internal.hpp'
        outs = {iname: '#pragma once\n// Shared includes, helpers and types of ' + stem + '.cpp (bo3 t03 split); its member definitions are in '
                + stem + '.cpp and ' + stem + '_partN.cpp.\n' + prefix + ns_open + hdr_text.rstrip('\n') + '\n' + ns_close}
        for k, pp in enumerate(parts, 1):
            body = '\n'.join(x.strip('\n') + '\n' for x in pp)
            if k == 1:
                body = ''.join(mainonly) + '\n' + body
            nm = stem + '.cpp' if k == 1 else f'{stem}_part{k}.cpp'
            outs[nm] = f'// {stem}: part {k} (bo3 t03 split; shared code in {iname}).\n#include "{iname}"\n\n' + ns_open + body + ns_close + (suffix if k == 1 and not outer else '')
        for nme, body in outs.items():
            print(f'  {nme}: {len(body)//1024} KB')
        if a.dry_run:
            return
        for nme, body in outs.items():
            if crlf:
                body = body.replace('\n', '\r\n')
            open(os.path.join(d, nme), 'w', encoding='utf-8', newline='').write(body)
        print('parts:', len(parts), ' add to CMake:', ' '.join(f'{stem}_part{k}.cpp' for k in range(2, len(parts) + 1)))
        return
    hdr_text = ''.join(hdr)
    guard_note = f'// Split out of {stem}.cpp (bo3 t03): shared includes, types and helpers; the tests live in {stem}_partN.cpp.\n'
    header = '#pragma once\n' + guard_note + hdr_text.rstrip('\n') + '\n\n// Test functions (defined in the part files)\n' + ''.join(decls)
    common_name = stem + '_common.hpp'
    outs = {common_name: header}
    for k, p in enumerate(parts, 1):
        outs[f'{stem}_part{k}.cpp'] = f'// Part {k} of {stem} (bo3 t03: split by position; see {common_name}).\n#include "{common_name}"\n\n' + '\n'.join(x.strip('\n') + '\n' for x in p)
    outs[stem + '.cpp'] = f'// {stem}: main() (bo3 t03: tests live in {stem}_partN.cpp, shared code in {common_name}).\n#include "{common_name}"\n\n' + ''.join(mainonly) + main_text
    for nme, body in outs.items():
        print(f'  {nme}: {len(body)//1024} KB')
    if a.dry_run:
        return
    for nme, body in outs.items():
        if crlf:
            body = body.replace('\n', '\r\n')
        open(os.path.join(d, nme), 'w', encoding='utf-8', newline='').write(body)
    print('parts:', len(parts), ' add to CMake:', ' '.join(f'{stem}_part{k}.cpp' for k in range(1, len(parts) + 1)))


def lead_static(t, repl):
    """Replace `static ` at the start of the first code line of chunk t (not one inside a body or comment)."""
    lines = t.split(chr(10))
    for k, ln in enumerate(lines):
        st = ln.strip()
        if not st or st.startswith(('//', '/*', '*')):
            continue
        lines[k] = re.sub(r'^(\s*)static\s+', lambda m: m.group(1) + repl, ln, count=1)
        break
    return chr(10).join(lines)


def split_top(args):
    out, depth, cur = [], 0, ''
    for ch in args:
        if ch in '(<[{':
            depth += 1
        elif ch in ')>]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return out


def strip_defaults(code, fn):
    m = re.search(r'\b' + re.escape(fn) + r'\s*\(', code)
    o = m.end()
    depth, k = 1, o
    while depth:
        if code[k] == '(':
            depth += 1
        elif code[k] == ')':
            depth -= 1
        k += 1
    args = code[o:k - 1]
    new = ','.join(re.sub(r'\s*=\s*[^=].*$', '', p, flags=re.S) if re.search(r'[^=!<>]=[^=]', p) else p for p in split_top(args))
    return code[:o] + new + code[k - 1:]


def inline_top(t, it):
    h = it['h']
    if re.match(r'^(struct|class|enum|union|using|typedef|namespace|extern|static_assert)\b', h) or h.startswith('template'):
        return t
    pre = h.split('=')[0]
    if '(' in pre and '{' in strip_comments(t):
        # function we keep in the header: inline
        t2 = lead_static(t, 'inline ')
        if t2 == t and not re.search(r'\b(inline|constexpr)\b', pre.split('(')[0]):
            t2 = add_inline(t)
        return t2
    if '(' in pre:
        return t  # prototype or constructor-style variable: leave
    # variable
    if re.search(r'\bconstexpr\b', pre):
        return lead_static(t, 'inline ') if re.match(r'^static\b', h) else add_inline(t)
    if re.match(r'^(static\s+)?const\b', h) and '*' not in pre.replace('const', ''):
        return lead_static(t, '')
    t2 = lead_static(t, 'inline ')
    return t2 if t2 != t else add_inline(t)


def add_inline(t):
    """Prefix `inline ` to the first code line of chunk t (after leading comments/blank lines)."""
    out, pos = [], 0
    lines = t.split('\n')
    for k, ln in enumerate(lines):
        s = ln.strip()
        if s and not s.startswith('//') and not s.startswith('/*') and not s.startswith('*'):
            lines[k] = re.sub(r'^(\s*)', r'\1inline ', ln, count=1)
            break
    return '\n'.join(lines)


def inline_block(t, add_part):
    # sub-chunk the body of `namespace NS { ... }`
    o = t.find('{')
    c = t.rfind('}')
    body = t[o + 1:c]
    subs = scan(body)
    res = []
    cstack = []
    dirty = 0
    for s, e, kind in subs:
        sub = body[s:e]
        if kind == 'pp':
            ds = strip_comments(sub).strip()
            m = re.match(r'#\s*(\w+)', ds)
            dn = m.group(1) if m else ''
            if dn in ('if', 'ifdef', 'ifndef'):
                cstack.append(ds)
            elif dn in ('else', 'elif'):
                dirty += 1
                cstack.append(ds)  # popped by the matching endif below
            elif dn == 'endif':
                while cstack and cstack[-1].startswith(('#else', '#elif', '# else', '# elif')):
                    cstack.pop()
                    dirty -= 1
                if cstack:
                    cstack.pop()
            res.append(sub)
            continue
        h = head(sub)
        fm = FUNC_HEAD2.match(h)
        if (fm and dirty == 0 and '{' in strip_comments(sub) and '::' not in h.split('(')[0]
                and not h.startswith('template') and not re.search(r'\b(auto|inline|constexpr|operator)\b', h.split('(')[0])
                and not re.search(r'=\s*[\{\[]', h)):
            code = lead_static(sub, '')
            code = strip_defaults(code, fm.group('name'))
            decl = fm.group('sig').strip() + ' ' + fm.group('name') + '(' + fm.group('args').strip() + ');' + chr(10)
            res.append(decl)
            add_part(code, list(cstack), ns=True)
            continue
        res.append(inline_top(sub, {'h': h}))
    return t[:o + 1] + ''.join(res) + t[c:]


main()
