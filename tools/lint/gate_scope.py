"""Which of the gate's optional builds (clang, ASan) a change needs (roadmap bo1 t05).

MSVC always runs. Decided from the files changed against the merge base with origin/dev (committed, uncommitted and
untracked). ANY DOUBT RUNS EVERYTHING: a missing ref, a git error, an empty change set, or a build / toolchain / CMake / script
file in the change set.
  clang runs when sim code changed: anything under engine/ except render, audio, video, ui (the CLAUDE.md sim definition),
        or a build-affecting file.
  asan  runs when engine/jobs or other threading code changed: engine/jobs, engine/core, or a changed C++ file that mentions
        std::thread / mutex / atomic / condition_variable / jthread / semaphore / latch / barrier / future / async,
        or a build-affecting file.
Docs, roadmap, .claude, launcher and similar changes need neither. Writes <logs>/gate_scope.txt (one "run|skip <build>: why"
line per build; build_gate.cmd and gate_summary.py read it) and prints the same lines.
ALAMO_GATE_ALL=1 in the environment forces everything.
Usage: gate_scope.py <logs dir> [--base origin/dev] | --selftest
"""
import os
import re
import subprocess
import sys
from pathlib import Path

NON_SIM = ('engine/render/', 'engine/audio/', 'engine/video/', 'engine/ui/')
THREAD_DIRS = ('engine/jobs/', 'engine/core/')
THREAD_RE = re.compile(r'std::(thread|jthread|mutex|shared_mutex|recursive_mutex|atomic|condition_variable|counting_semaphore|'
                       r'binary_semaphore|latch|barrier|future|async|promise|packaged_task)|<(thread|mutex|atomic|latch|'
                       r'barrier|semaphore|future|condition_variable|stop_token)>')
CODE_EXT = ('.cpp', '.hpp', '.h', '.c', '.cc', '.inl')
DOCS_ONLY = ('docs/', 'roadmap-store/', '.claude/', 'launcher/', 'extracted/', 're_reference/')


def build_affecting(p):
    name = p.rsplit('/', 1)[-1]
    return (name == 'CMakeLists.txt' or name.endswith(('.cmake', '.cmd', '.bat', '.ps1', '.gitattributes', '.in'))
            or p.startswith(('toolchain/', 'tools/lint/', 'tools/testbuild/')) or name in ('CMakePresets.json',))


def decide(files, mentions_threads):
    """files: changed repo paths; mentions_threads(path) -> bool. Returns {'clang': (run, why), 'asan': (run, why)}."""
    if not files:
        return {'clang': (True, 'no changed files found (doubt)'), 'asan': (True, 'no changed files found (doubt)')}
    clang = asan = None
    for f in files:
        if build_affecting(f):
            return {'clang': (True, f'build-affecting file {f}'), 'asan': (True, f'build-affecting file {f}')}
        if f.startswith('engine/') and not f.startswith(NON_SIM) and clang is None:
            clang = f
        if asan is None and (f.startswith(THREAD_DIRS) or (f.endswith(CODE_EXT) and mentions_threads(f))):
            asan = f
        if (not f.startswith('engine/') and not f.startswith(DOCS_ONLY) and not f.startswith('tools/')
                and f.endswith(CODE_EXT)):
            return {'clang': (True, f'unclassified code file {f}'), 'asan': (True, f'unclassified code file {f}')}
    return {'clang': (clang is not None, f'sim code changed: {clang}' if clang else 'no sim code changed'),
            'asan': (asan is not None, f'threading code changed: {asan}' if asan else 'no threading code changed')}


def git(*args):
    r = subprocess.run(['git', *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return r.stdout


def changed_files(base):
    mb = git('merge-base', base, 'HEAD').strip()
    files = set(git('diff', '--name-only', mb).splitlines())
    files |= set(git('ls-files', '-o', '--exclude-standard').splitlines())
    return sorted(f for f in files if f)


def selftest():
    no = lambda f: False
    d = decide(['docs/BUILD.md', 'roadmap-store/x.json'], no)
    assert not d['clang'][0] and not d['asan'][0], d
    d = decide(['engine/render/src/a.cpp'], no)
    assert not d['clang'][0] and not d['asan'][0], d
    d = decide(['engine/tactical/src/a.cpp'], no)
    assert d['clang'][0] and not d['asan'][0], d
    d = decide(['engine/jobs/src/a.cpp'], no)
    assert d['clang'][0] and d['asan'][0], d
    d = decide(['engine/render/src/a.cpp'], lambda f: True)
    assert not d['clang'][0] and d['asan'][0], d
    for doubt in ([], ['engine/tactical/CMakeLists.txt'], ['build_msvc.cmd'], ['toolchain/x.txt'], ['src/odd.cpp']):
        d = decide(doubt, no)
        assert d['clang'][0] and d['asan'][0], (doubt, d)
    print('gate_scope selftest ok')


def main(argv):
    if '--selftest' in argv:
        return selftest()
    logs = Path(argv[0])
    base = argv[argv.index('--base') + 1] if '--base' in argv else 'origin/dev'
    try:
        if os.environ.get('ALAMO_GATE_ALL') == '1':
            raise RuntimeError('ALAMO_GATE_ALL=1')
        files = changed_files(base)

        def mentions(f):
            try:
                return bool(THREAD_RE.search(Path(f).read_text(encoding='utf8', errors='replace')))
            except OSError:
                return True
        d = decide(files, mentions)
    except Exception as e:  # any doubt runs all
        d = {'clang': (True, f'scope check failed: {e}'), 'asan': (True, f'scope check failed: {e}')}
    lines = [f"{'run ' if run else 'skip'} {name}: {why}" for name, (run, why) in d.items()]
    logs.mkdir(exist_ok=True)
    (logs / 'gate_scope.txt').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
