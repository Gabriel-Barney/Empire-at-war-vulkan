"""The fast-tier tests of the engine modules this branch changed, for the inner loop (build_agent.cmd ... changed).

  changed_tests.py <build tree> <out file> [--base origin/dev]

Changed files: everything that differs from the merge base with --base (commits, uncommitted edits, untracked files).
A file under engine/<module>/ selects that module; a test belongs to the module whose engine/<module>/ CMake file
registered it (ctest --show-only=json-v1 backtrace, so tests made by helper functions in engine/CMakeLists.txt count for
the module that called the helper). Tests labelled "slow" are left out, as in the fast tier.
Falls back to the whole fast tier (and says so) when a change reaches every module (engine/CMakeLists.txt,
engine/test_support/, the top-level CMakeLists.txt, cmake/) or selects no test.
Writes one test name per line to <out file> (ctest --tests-from-file). Exit 0; 2 on usage or a tree ctest cannot read.
The changed modules' tests do not cover the modules that depend on them (apps goldens, for example): land_on_dev.cmd
still runs the whole fast tier on the merged HEAD.
Selftest: changed_tests.py --selftest
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EVERYTHING = ('engine/CMakeLists.txt', 'engine/test_support/', 'CMakeLists.txt', 'cmake/')


def git_lines(*args):
    p = subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True)
    return [l for l in p.stdout.splitlines() if l.strip()] if p.returncode == 0 else []


def changed_files(base):
    mb = git_lines('merge-base', base, 'HEAD')
    names = set(git_lines('diff', '--name-only', mb[0] if mb else 'HEAD'))
    names |= set(git_lines('ls-files', '--others', '--exclude-standard'))
    return sorted(n.replace('\\', '/') for n in names)


def modules_of(files, module_dirs):
    """(set of modules, True when a file reaches every module)."""
    mods, everything = set(), False
    for f in files:
        if f in EVERYTHING or any(f.startswith(p) for p in EVERYTHING if p.endswith('/')):
            everything = True
            continue
        parts = f.split('/')
        if len(parts) > 2 and parts[0] == 'engine' and parts[1] in module_dirs:
            mods.add(parts[1])
    return mods, everything


def test_module(test, graph, root_posix):
    """The engine module whose CMake file registered the test (innermost frame under engine/<module>/), or None."""
    files, nodes = graph.get('files', []), graph.get('nodes', [])
    n = test.get('backtrace')
    while n is not None and 0 <= n < len(nodes):
        f = files[nodes[n]['file']].replace('\\', '/')
        rel = f[len(root_posix) + 1:] if f.lower().startswith(root_posix.lower() + '/') else f
        parts = rel.split('/')
        if len(parts) > 2 and parts[0] == 'engine' and parts[1] != 'test_support':
            return parts[1]
        n = nodes[n].get('parent')
    return None


def is_slow(test):
    for p in test.get('properties', []):
        if p.get('name') == 'LABELS' and 'slow' in (p.get('value') or []):
            return True
    return False


def select(doc, mods, everything, root_posix):
    tests = doc.get('tests', [])
    graph = doc.get('backtraceGraph', {})
    fast = [t for t in tests if not is_slow(t)]
    if not everything:
        picked = [t['name'] for t in fast if test_module(t, graph, root_posix) in mods]
        if picked:
            return picked, False
    return [t['name'] for t in fast], True


def main(argv):
    if argv == ['--selftest']:
        return selftest()
    if len(argv) < 2:
        print(__doc__)
        return 2
    tree, out = Path(argv[0]), Path(argv[1])
    base = argv[argv.index('--base') + 1] if '--base' in argv else 'origin/dev'
    p = subprocess.run(['ctest', '--test-dir', str(tree), '--show-only=json-v1'], capture_output=True, text=True)
    if p.returncode != 0:
        print(f'changed_tests: ctest cannot list {tree}: {p.stderr.strip()[:300]}')
        return 2
    doc = json.loads(p.stdout)
    module_dirs = {d.name for d in (ROOT / 'engine').iterdir() if d.is_dir()}
    files = changed_files(base)
    mods, everything = modules_of(files, module_dirs)
    names, fell_back = select(doc, mods, everything, ROOT.as_posix())
    out.write_text(''.join(n + '\n' for n in names), encoding='utf8')
    if fell_back:
        why = 'a change reaches every module' if everything else (
            f'no test in the changed modules ({", ".join(sorted(mods))})' if mods else 'no engine module changed')
        print(f'changed_tests: {why}: the whole fast tier, {len(names)} tests')
    else:
        print(f'changed_tests: modules {", ".join(sorted(mods))} changed against {base}: {len(names)} fast-tier tests '
              '(their dependents are not run; land_on_dev.cmd runs the whole fast tier)')
    return 0


def selftest():
    bad = []
    root = 'E:/r'
    graph = {'files': ['E:/r/engine/CMakeLists.txt', 'E:/r/engine/tactical/CMakeLists.txt', 'E:/r/engine/apps/CMakeLists.txt',
                       'E:/r/engine/tactical/movement/CMakeLists.txt'],
             'nodes': [{'file': 1}, {'file': 0, 'parent': 2}, {'file': 2}, {'file': 3, 'parent': 0}]}
    doc = {'backtraceGraph': graph, 'tests': [
        {'name': 'tac', 'backtrace': 0},
        {'name': 'apps_w4', 'backtrace': 1},  # made by a helper in engine/CMakeLists.txt, called from apps
        {'name': 'mov', 'backtrace': 3},
        {'name': 'tac_slow', 'backtrace': 0, 'properties': [{'name': 'LABELS', 'value': ['slow']}]}]}
    dirs = {'tactical', 'apps', 'core', 'test_support'}
    mods, every = modules_of(['engine/tactical/movement/a.cpp', 'docs/x.md'], dirs)
    if mods != {'tactical'} or every:
        bad.append(f'modules_of {mods} {every}')
    names, fb = select(doc, mods, every, root)
    if names != ['tac', 'mov'] or fb:
        bad.append(f'tactical {names} {fb}')
    names, fb = select(doc, {'apps'}, False, root)
    if names != ['apps_w4'] or fb:
        bad.append(f'helper-made test {names}')
    mods, every = modules_of(['engine/test_support/include/x.hpp'], dirs)
    names, fb = select(doc, mods, every, root)
    if not every or not fb or names != ['tac', 'apps_w4', 'mov']:
        bad.append(f'everything {names} {fb}')
    names, fb = select(doc, {'core'}, False, root)
    if not fb:
        bad.append('a module without tests falls back to the fast tier')
    for b in bad:
        print('FAIL: ' + b)
    print('changed_tests selftest: ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
