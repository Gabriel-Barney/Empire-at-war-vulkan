"""Refuses generated or oversized files that a landing would add to the repository.

Gabriel, 2026-11-07: about 35,000 lines of generated files (probe payloads, a model index) were tracked and had to be
cleaned up later (roadmap groups cu17 / cu18). Every change reaches `dev` through tools\\land_on_dev.cmd, which runs
this check before it pushes; build_gate.cmd runs it for the dev -> main release.

Only files ADDED between --base and --head are checked (a renamed file counts as added); files already tracked are
never blocked. A new file fails when:
  size    its blob is over 512 KiB, or over 128 KiB for a data file (.json, .csv, .xml, .txt, ...) outside the
          test-data folders: a big data file is almost always a tool's dump;
  output  a folder in its path is a build / tool output folder (out, work, Mods, bin, obj, logs, build-*, OPTIONAL_*,
          ...), except under a test-data folder (tests/, */testdata/), where such names are fixture content;
  type    its extension is a build product (.obj, .pdb, .exe, .dll, .pyc, .log, .spv, ...);
  binary  git sees it as binary and it is outside the folders meant for binary test data and references
          (tests/, */testdata/, */tests/golden/, docs/images/, docs/formats/examples/, docs/archive/,
          tools/probe_mod/*/refs/).
A file that should be tracked anyway goes in tools/lint/tracked_allow.txt (one glob per line, ** allowed), with a
comment saying why. Anything a tool writes belongs in an ignored output folder instead (docs/BUILD.md).

Usage: check_tracked.py [--base REF] [--head REF]     (defaults: origin/dev, HEAD)
       check_tracked.py --selftest
Exit: 0 clean, 1 refused files, 2 git or usage error.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

MAX_BYTES = 512 * 1024
DATA_MAX_BYTES = 128 * 1024
DATA_EXTS = {'.json', '.csv', '.tsv', '.xml', '.txt', '.crc', '.yaml', '.yml', '.html', '.ini'}
OUTPUT_DIRS = {'out', 'work', 'mods', 'bin', 'obj', 'logs', 'dist', 'backups', '__pycache__', '.vs', 'x64',
               'debug', 'release', 'cmakefiles'}
OUTPUT_PREFIXES = ('build-', 'optional_')
BUILD_EXTS = {'.obj', '.o', '.pdb', '.ilk', '.exe', '.dll', '.lib', '.exp', '.idb', '.ipch', '.pch', '.pyc', '.pyo',
              '.log', '.tmp', '.actual', '.spv', '.dxil', '.cso', '.fxo', '.trace', '.pkl', '.dat'}
DATA_ROOTS = ('tests/**', '**/testdata/**')
BINARY_ROOTS = DATA_ROOTS + ('**/tests/golden/**', 'docs/images/**', 'docs/formats/examples/**', 'docs/archive/**',
                             'tools/probe_mod/*/refs/**')
HERE = os.path.dirname(os.path.abspath(__file__))
ALLOW_FILE = os.path.join(HERE, 'tracked_allow.txt')


def glob_re(pattern):
    """'**' matches any number of folders (also none), '*' and '?' stay inside one folder."""
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith('**/', i):
            out.append('(?:.*/)?')
            i += 3
        elif pattern.startswith('**', i):
            out.append('.*')
            i += 2
        elif pattern[i] == '*':
            out.append('[^/]*')
            i += 1
        elif pattern[i] == '?':
            out.append('[^/]')
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile(''.join(out) + r'\Z', re.IGNORECASE)


def matches(path, patterns):
    return any(glob_re(p).match(path) for p in patterns)


def read_allow(path):
    if not os.path.exists(path):
        return []
    out = []
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            out.append(line.replace('\\', '/'))
    return out


def reasons_for(path, size, is_binary, allow):
    """The rules a new file breaks ([] when it may be tracked)."""
    if matches(path, allow):
        return []
    why = []
    ext = os.path.splitext(path)[1].lower()
    limit = DATA_MAX_BYTES if ext in DATA_EXTS and not matches(path, DATA_ROOTS) else MAX_BYTES
    if size is not None and size > limit:
        why.append('size: %d KiB is over the %d KiB limit' % ((size + 1023) // 1024, limit // 1024))
    folders = path.split('/')[:-1]
    bad = [f for f in folders if f.lower() in OUTPUT_DIRS or f.lower().startswith(OUTPUT_PREFIXES)]
    if bad and not matches(path, DATA_ROOTS):
        why.append('output: inside the output folder "%s/"' % bad[0])
    if ext in BUILD_EXTS:
        why.append('type: %s files are build or run products' % ext)
    if is_binary and not matches(path, BINARY_ROOTS):
        why.append('binary: binary file outside the test-data and reference folders')
    return why


def git(args, cwd=None):
    r = subprocess.run(['git'] + args, cwd=cwd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('git %s: %s' % (' '.join(args), r.stderr.decode('utf-8', 'replace').strip()))
    return r.stdout


def added_files(base, head, cwd=None):
    """[(path, size, is_binary)] for files in head that base does not have (renames count as added)."""
    raw = git(['diff', '--no-renames', '--diff-filter=A', '--numstat', '-z', base, head], cwd)
    files = []
    for rec in raw.split(b'\0'):
        if not rec:
            continue
        added, _deleted, path = rec.decode('utf-8', 'replace').split('\t', 2)
        files.append((path, added == '-'))
    out = []
    if files:
        spec = ''.join('%s:%s\n' % (head, p) for p, _ in files).encode('utf-8')
        r = subprocess.run(['git', 'cat-file', '--batch-check=%(objectsize)'], input=spec, cwd=cwd,
                           capture_output=True)
        sizes = r.stdout.decode().split()
        for (p, b), s in zip(files, sizes):
            out.append((p, int(s) if s.isdigit() else None, b))
    return out


def check(base, head, allow_file=ALLOW_FILE, cwd=None):
    allow = read_allow(allow_file)
    files = added_files(base, head, cwd)
    bad = [(p, reasons_for(p, s, b, allow)) for p, s, b in files]
    return len(files), [(p, r) for p, r in bad if r]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base', default='origin/dev')
    ap.add_argument('--head', default='HEAD')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    try:
        n, bad = check(a.base, a.head)
    except RuntimeError as e:
        print('check_tracked: %s' % e)
        return 2
    if not bad:
        print('check_tracked: %d new file(s) since %s, OK' % (n, a.base))
        return 0
    print('check_tracked: %d of %d new file(s) since %s should not be tracked:' % (len(bad), n, a.base))
    for p, why in bad:
        print('  %s' % p)
        for w in why:
            print('      %s' % w)
    print('Fix: if a tool wrote the file, remove it from the commit ("git rm --cached <file>"), add its output folder to')
    print('.gitignore and commit. If it must be tracked, add a glob for it to tools/lint/tracked_allow.txt with a')
    print('comment saying why, commit, and rerun.')
    return 1


def selftest():
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)
            print('FAIL', what)

    rx = glob_re('tools/probe_mod/*/refs/**')
    expect(rx.match('tools/probe_mod/g1/refs/a/b.png'), 'glob * and **')
    expect(not rx.match('tools/probe_mod/g1/x/refs/b.png'), 'glob * stays in one folder')
    expect(glob_re('**/testdata/**').match('engine/apps/game/testdata/m/Mods/x.lua'), 'glob leading **')
    expect(glob_re('**/testdata/**').match('testdata/x.bin'), 'glob ** matches no folder')

    allow = ['docs/shaders/*.zip']
    r = lambda p, s=10, b=False: reasons_for(p, s, b, allow)
    expect(r('engine/core/src/x.cpp') == [], 'plain source passes')
    expect(any(w.startswith('size') for w in r('docs/big.json', MAX_BYTES + 1)), 'size limit')
    expect(r('docs/ok.json', DATA_MAX_BYTES) == [], 'data size at the limit passes')
    expect(any(w.startswith('size') for w in r('tools/meg/eaw_models_index.json', 372201)), 'data dump over 128 KiB')
    expect(r('engine/apps/sim/testdata/x_golden.crc', 300 * 1024) == [], 'test data keeps the 512 KiB limit')
    expect(r('engine/apps/sim/src/big.cpp', 468 * 1024) == [], 'source keeps the 512 KiB limit')
    expect(any(w.startswith('output') for w in r('tools/probe_mod/v2/Mods/P/Data/x.xml')), 'Mods output folder')
    expect(any(w.startswith('output') for w in r('tools/probe_mod/v2/OPTIONAL_X/a.txt')), 'OPTIONAL_ output folder')
    expect(any(w.startswith('output') for w in r('build-wt-msvc/CMakeCache.txt')), 'build- output folder')
    expect(r('engine/apps/game/testdata/mod_fixture/corruption/Mods/F/Data/XML/a.xml') == [], 'Mods in testdata')
    expect(any(w.startswith('type') for w in r('tools/x/run.log')), 'log file type')
    expect(any(w.startswith('type') for w in r('engine/render/shaders/a.spv', 10, True)), 'spv type')
    expect(any(w.startswith('binary') for w in r('tools/probe_mod/x/shot.png', 10, True)), 'binary outside data')
    expect(r('tests/visual_baselines/smoke/a.png', 10, True) == [], 'visual baseline binary')
    expect(r('tools/probe_mod/g1_capture/refs/a.png', 10, True) == [], 'probe refs binary')
    expect(r('docs/shaders/shaders_decompiled.zip', MAX_BYTES * 4, True) == [], 'allow file wins')

    tmp = tempfile.mkdtemp(prefix='check_tracked_')
    try:
        def put(rel, data):
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(data)

        env_args = ['-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false', '-c', 'core.autocrlf=false']
        git(['init', '-q', '-b', 'main'], tmp)
        put('src/a.cpp', b'int a;\n')
        put('big_old.json', b'x' * (MAX_BYTES + 10))
        git(['add', '-A'], tmp)
        git(env_args + ['commit', '-q', '-m', 'base'], tmp)
        base = git(['rev-parse', 'HEAD'], tmp).decode().strip()
        put('src/b.cpp', b'int b;\n')
        put('big_old.json', b'y' * (MAX_BYTES + 20))
        put('tools/k/out/r.txt', b'r\n')
        put('docs/huge.csv', b'z' * (MAX_BYTES + 1))
        put('tools/k/shot.png', b'\x89PNG\r\n\x1a\n\0\0\0')
        put('tests/golden/g.bin', b'\0\1\2')
        put('allowed/thing.exe', b'MZ\0\0')
        git(['add', '-A'], tmp)
        git(env_args + ['commit', '-q', '-m', 'head'], tmp)
        allow = os.path.join(tmp, 'allow.txt')
        open(allow, 'w').write('# test\nallowed/**\n')
        n, bad = check(base, 'HEAD', allow, tmp)
        got = sorted(p for p, _ in bad)
        expect(n == 6, 'six added files seen (got %d)' % n)
        expect(got == ['docs/huge.csv', 'tools/k/out/r.txt', 'tools/k/shot.png'], 'refused set: %s' % got)
        n2, bad2 = check('HEAD', 'HEAD', allow, tmp)
        expect(n2 == 0 and not bad2, 'nothing added, nothing refused')
    finally:
        subprocess.run(['cmd', '/c', 'rmdir', '/s', '/q', tmp] if os.name == 'nt' else ['rm', '-rf', tmp],
                       capture_output=True)
    print('CHECK_TRACKED SELFTEST %s (%d failed)' % ('OK' if not fails else 'FAILED', len(fails)))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
