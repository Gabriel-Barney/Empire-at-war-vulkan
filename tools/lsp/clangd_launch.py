"""clangd_launch.py - start the project-local clangd for Claude Code's code intelligence (LSP) plugin (2026-11-13).

The plugin (.claude/marketplace/plugins/eaw-clangd) runs `python -I tools/lsp/clangd_launch.py` instead of clangd itself:
- clangd lives in toolchain\\llvm\\bin (rule 5: nothing on PATH), and the project path has spaces, which the plugin's
  `command` field does not allow;
- the MSVC headers and Windows SDK are project-local too, so clangd needs the INCLUDE / LIB environment that
  toolchain\\msvc\\setup_x64.bat sets (the same call build_clang.cmd makes);
- the compile database is in a build tree, not the source root: this picks the newest compile_commands.json of this
  checkout's trees, preferring the clang trees (clang++ GNU driver, the same front end clangd uses).

stdin / stdout are passed straight through to clangd (the LSP stream); diagnostics of this script go to stderr.
`--print` shows what it would run and exits. `--check FILE` runs `clangd --check=FILE` (a one-shot diagnostics run).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TC = os.path.join(ROOT, 'toolchain')
CLANGD = os.path.join(TC, 'llvm', 'bin', 'clangd.exe')
SETUP = os.path.join(TC, 'msvc', 'setup_x64.bat')
# Preference order when two trees have the same age: clang trees first.
TREES = ['build-wt-clang', 'build-clang', 'build-wt-msvc', 'build-msvc']
ENV_KEYS = ('INCLUDE', 'LIB', 'LIBPATH', 'PATH', 'VCToolsInstallDir', 'WindowsSdkDir', 'WindowsSDKVersion',
            'UniversalCRTSdkDir', 'UCRTVersion', 'VCINSTALLDIR')


def log(msg):
    sys.stderr.write('clangd_launch: %s\n' % msg)
    sys.stderr.flush()


def pick_compile_db():
    """Directory of the newest compile_commands.json among this checkout's build trees, or None."""
    best = None
    for rank, name in enumerate(TREES):
        db = os.path.join(ROOT, name, 'compile_commands.json')
        if os.path.isfile(db):
            key = (os.path.getmtime(db), -rank)
            if best is None or key > best[0]:
                best = (key, os.path.dirname(db))
    return best[1] if best else None


def msvc_env():
    """Environment after toolchain\\msvc\\setup_x64.bat, or the current one when the script is missing or fails."""
    env = dict(os.environ)
    if not os.path.isfile(SETUP):
        log('no %s; MSVC headers may not resolve' % SETUP)
        return env
    try:
        out = subprocess.run('call "%s" >nul && set' % SETUP, shell=True, capture_output=True, text=True,
                             timeout=60, stdin=subprocess.DEVNULL).stdout
    except (OSError, subprocess.SubprocessError) as e:
        log('setup_x64.bat failed: %s' % e)
        return env
    for line in out.splitlines():
        k, sep, v = line.partition('=')
        if sep and k in ENV_KEYS:
            env[k] = v
    return env


def build_command(extra):
    check = any(a.startswith('--check') for a in extra)
    cmd = [CLANGD, '--header-insertion=never']
    # LSP mode: index in the background, log errors only. --check: no index, the default (info) log is its report.
    if not check:
        cmd += ['--background-index', '--log=error']
    db = pick_compile_db()
    if db:
        cmd.append('--compile-commands-dir=%s' % db)
    else:
        log('no compile_commands.json in %s yet: build once (build_agent.cmd) for full diagnostics' % ', '.join(TREES))
    return cmd + extra


def main(argv):
    if not os.path.isfile(CLANGD):
        log('clangd missing: %s (toolchain\\5_portability_compilers.cmd installs LLVM)' % CLANGD)
        return 1
    extra = []
    if argv[:1] == ['--check'] and len(argv) > 1:
        extra = ['--check=%s' % os.path.abspath(argv[1])]
    elif argv[:1] == ['--print']:
        print(' '.join('"%s"' % a if ' ' in a else a for a in build_command([])))
        return 0
    else:
        extra = argv
    # Inherited stdin / stdout: clangd speaks LSP straight to Claude Code.
    return subprocess.call(build_command(extra), env=msvc_env(), cwd=ROOT)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
