#!/usr/bin/env python3
r"""a2j_port.py - the port's side of the a2j probe: a traced headless run of the probe's scenario, for the row-by-row
comparison with a retail run of the a2f_budget kit (D4 perception table, per-fiscal-cycle P / I).

The engine in the repository is never changed: the traces go into an exported copy (git archive) of a commit.

  a2j_port.py prepare --dir D [--commit C]     export C (default HEAD) to D, apply tools/analysis/a2g_trace_patch.py and
                                               port_a2j_patch.py, junction D\toolchain to the project toolchain
  a2j_port.py build   --dir D                  build alamo_headless in D\build (log D\build.txt; project MSVC, no cache)
  a2j_port.py run     --dir D --out O [--ticks N]   the scenario (Sandbox_Heart_Of_The_Maw_Rebel, human Rebel, --story,
                                               seed 1, --workers 0) with ALAMO_A2G_TRACE=3 and the A2J perception trace:
                                               O\port_ai.log (--ai-log), O\port.err (traces), O\port_run.json
  a2j_port.py run ... --control                the same run without the A2J perception trace (the port side of a retail
                                               run built with --no-percept; the trace's evaluations refresh the AI's
                                               leaf cache, so the traced run differs from frame 1460 at 5509c2a)
  a2j_port.py table   --err O\port.err --tsv F write the D4 perception table (one row per pair and sample frame)
  a2j_port.py all     --dir D --out O [--ticks N] [--commit C] [--tsv F]   prepare + build + run + table

Default --ticks 54000 (30 minutes of galactic time). A2J_T02_PROBE_PLAN.md says how the tables are read; a2f_report.py
--port-trace O\port.err prints the per-cycle P / I comparison against a retail run.
"""
import argparse
import datetime
import io
import json
import os
import re
import subprocess
import sys
import tarfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import a2fb_spec  # noqa: E402

EXPORT_PATHS = ['CMakeLists.txt', 'engine', 'tools/lint', 'tools/test_env.cmd', 'tools/cache_env.cmd', 'tools/meg',
                'tools/formats', 'tools/shaders/cpp']
MODULES = 'core;vfs;data;jobs;galactic;objects;anim;commands;tactical;lua;ai;assets;terrain;apps'
SCENARIO = ['--economy', '--galactic-ai', '--story', '--campaign', 'Sandbox_Heart_Of_The_Maw_Rebel', '--human', 'Rebel',
            '--seed', '1', '--workers', '0', '--quiet']
PERC_RE = re.compile(r'^A2J PERC f=(\d+) p=(\d+) fac=(\w+) tgt=(\S+) t=(-?\d+) own=(\S+)(.*)$')


def git(*args):
    return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, check=True).stdout


def prepare(d, commit):
    os.makedirs(d, exist_ok=True)
    sha = git('rev-parse', commit).decode().strip()
    data = git('archive', '--format=tar', sha, *EXPORT_PATHS)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(d)
    py = sys.executable
    subprocess.run([py, '-I', os.path.join(ROOT, 'tools', 'analysis', 'a2g_trace_patch.py'), d], check=True)
    subprocess.run([py, '-I', os.path.join(HERE, 'port_a2j_patch.py'), d], check=True)
    tc = os.path.join(d, 'toolchain')
    if not os.path.exists(tc):
        subprocess.run(['cmd', '/c', 'mklink', '/J', tc, os.path.join(ROOT, 'toolchain')], check=True,
                       capture_output=True)
    with open(os.path.join(d, 'A2J_EXPORT.json'), 'w') as f:
        json.dump({'commit': sha, 'patches': ['tools/analysis/a2g_trace_patch.py', 'port_a2j_patch.py'],
                   'when': datetime.datetime.now().isoformat(timespec='seconds')}, f, indent=1)
    print('exported %s to %s and patched' % (sha[:10], d))
    return sha


def build(d):
    cmd = os.path.join(d, 'a2j_build.cmd')
    with open(cmd, 'w', newline='\r\n') as f:
        f.write('@echo off\nsetlocal\nset "TC=%~dp0toolchain"\ncall "%TC%\\msvc\\setup_x64.bat" >nul\n'
                'set ALAMO_COMPILER_CACHE=OFF\nset "PATH=%TC%\\venv\\Scripts;%PATH%"\n'
                'cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release '
                '"-DCMAKE_MAKE_PROGRAM=%TC%\\venv\\Scripts\\ninja.exe" -DCMAKE_CXX_COMPILER=cl -DALAMO_COMPILER_CACHE=OFF '
                '"-DALAMO_ENGINE_MODULES=' + MODULES + '" > "%~dp0build.txt" 2>&1\n'
                'if errorlevel 1 (echo CONFIGURE_FAILED>> "%~dp0build.txt" & exit /b 1)\n'
                'cmake --build "%~dp0build" --target alamo_headless >> "%~dp0build.txt" 2>&1\n'
                'if errorlevel 1 (echo BUILD_FAILED>> "%~dp0build.txt" & exit /b 1)\n'
                'echo BUILD_OK>> "%~dp0build.txt"\n')
    r = subprocess.run(['cmd', '/c', cmd])
    print('build %s (log %s)' % ('OK' if r.returncode == 0 else 'FAILED', os.path.join(d, 'build.txt')))
    return r.returncode


def eqs_file(path):
    """The D4 token equations (A2FB_D4_*) as name|expression lines for ALAMO_A2H_EXTRA_EQS."""
    with open(path, 'w', newline='\n') as f:
        for n, _k, body in a2fb_spec.PERC_MOD:
            f.write('%s|%s\n' % (n, body))


def run(d, out, ticks, control=False):
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(d, 'build', 'bin', 'alamo_headless.exe')
    if not os.path.isfile(exe):
        raise SystemExit('no %s: run build first' % exe)
    eqf = os.path.join(out, 'a2j_eqs.txt')
    eqs_file(eqf)
    keys = [(k, n) for n, k, _h in a2fb_spec.PERC_SHIPPED] + [(k, n) for n, k, _b in a2fb_spec.PERC_MOD]
    env = {
        'ALAMO_A2G_TRACE': '3',
        'ALAMO_A2H_EXTRA_EQS': eqf,
        'ALAMO_A2J_PERC': ','.join('%s:%s' % (p[1], p[2]) for p in a2fb_spec.PERC_PAIRS),
        'ALAMO_A2J_EQS': ','.join('%s:%s' % kn for kn in keys),
        'ALAMO_A2J_MAXF': str(ticks),
    }
    if control:
        # the same run without the perception trace / extra equations (A2G BUD rows kept). Not identical to the traced
        # run: the trace's evaluations share the AI's per-token leaf cache (EnemyForce totals, 15 s), so they shift when
        # the AI's cached values are refreshed. At 5509c2a the ai-log is the same to frame 1460, then plan order differs
        # (final CRC 5d3a792a7601b351 vs 7cf652411ed4a322 at 54000). The extra equation file alone changes nothing.
        # Use the traced run against a retail run with channel D, the control run against a retail --no-percept run.
        env = {'ALAMO_A2G_TRACE': '3'}
    tmp = os.path.join(out, 'tmp')
    os.makedirs(tmp, exist_ok=True)
    cmd = os.path.join(out, 'a2j_run.cmd')
    log, err = os.path.join(out, 'port_ai.log'), os.path.join(out, 'port.err')
    with open(cmd, 'w', newline='\r\n') as f:
        f.write('@echo off\nsetlocal\ncall "%s"\n' % os.path.join(d, 'tools', 'test_env.cmd'))
        f.write('set "TMP=%s"\nset "TEMP=%s"\n' % (tmp, tmp))
        for k, v in env.items():
            f.write('set "%s=%s"\n' % (k, v))
        f.write('"%s" %s --ticks %d --ai-log "%s" 2> "%s"\nexit /b %%ERRORLEVEL%%\n' % (
            exe, ' '.join(SCENARIO), ticks, log, err))
    r = subprocess.run(['cmd', '/c', cmd], capture_output=True, text=True)
    info = {'exe': exe, 'ticks': ticks, 'args': SCENARIO + ['--ticks', str(ticks)], 'env': env, 'exit': r.returncode,
            'stdout_tail': r.stdout[-2000:]}
    exp = os.path.join(d, 'A2J_EXPORT.json')
    if os.path.exists(exp):
        info['export'] = json.load(open(exp))
    with open(os.path.join(out, 'port_run.json'), 'w') as f:
        json.dump(info, f, indent=1)
    print('port run exit %d: %s, %s' % (r.returncode, log, err))
    return r.returncode


def table(err, tsv, commit=None):
    rows = []
    with open(err, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = PERC_RE.match(line.strip())
            if not m:
                continue
            kv = dict(tok.split('=', 1) for tok in m.group(7).split() if '=' in tok)
            rows.append({'f': int(m.group(1)), 'fac': m.group(3), 'tgt': m.group(4), 'own': m.group(6), 'v': kv})
    with open(tsv, 'w', newline='\n') as f:
        if commit:
            f.write('# port reference, export of commit %s (a2j_port.py; traces: a2g_trace_patch.py + port_a2j_patch.py)\n'
                    % commit)
        f.write('# evaluated right after the AI player\'s own galactic AI service of that frame; FAIL = no value '
                '(retail: nil and the script stops)\n')
        f.write('\t'.join(['fac', 'tgt', 'frame', 'own'] + a2fb_spec.PERC_KEYS) + '\n')
        for r in rows:
            f.write('\t'.join([r['fac'], r['tgt'], str(r['f']), r['own']] +
                              [r['v'].get(k, '') for k in a2fb_spec.PERC_KEYS]) + '\n')
    print('%d perception rows -> %s' % (len(rows), tsv))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['prepare', 'build', 'run', 'table', 'all'])
    ap.add_argument('--dir')
    ap.add_argument('--out')
    ap.add_argument('--commit', default='HEAD')
    ap.add_argument('--ticks', type=int, default=54000)
    ap.add_argument('--err')
    ap.add_argument('--tsv')
    ap.add_argument('--control', action='store_true', help='run: without the A2J trace (the ai-log must not change)')
    a = ap.parse_args()
    if a.cmd in ('prepare', 'all'):
        prepare(a.dir, a.commit)
    if a.cmd in ('build', 'all'):
        if build(a.dir):
            return 1
    if a.cmd in ('run', 'all'):
        if run(a.dir, a.out, a.ticks, a.control):
            return 1
    if a.cmd in ('table', 'all'):
        err = a.err or os.path.join(a.out, 'port.err')
        commit = None
        if a.dir and os.path.exists(os.path.join(a.dir, 'A2J_EXPORT.json')):
            commit = json.load(open(os.path.join(a.dir, 'A2J_EXPORT.json')))['commit']
        table(err, a.tsv or os.path.join(a.out or os.path.dirname(err), 'port_perception.tsv'), commit)
    return 0


if __name__ == '__main__':
    sys.exit(main())
