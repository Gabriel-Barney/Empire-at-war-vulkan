#!/usr/bin/env python3
"""cu8h_trace.py - one cu8h_dbg.py trace: start the debugger against the game, arm it, write the reports.

  cu8h_trace.py [--label NAME] [--n-env 4] [--stop-mode-frame 120] [--max-hits 1500000] [--wait-process-sec 900]
                [--arm-now | --arm-file PATH [--stop-file PATH]] [--non-interactive] [--sites]
                [--windows 0,1,2,...] [--table A-B ...] [--exe <game>\\corruption\\StarWarsG.exe]

1. Creates tools\\probe_mod\\cu8h_probes\\out\\<yyyymmdd-hhmmss>-trace-<label>\\ and starts cu8h_dbg.py there (it waits up
   to --wait-process-sec for StarWarsG.exe; start the game before or after this script). out\\latest_trace.txt names
   the run folder and the arm / stop files.
2. Arming, one of:
   - default: press Enter right before you click Begin (skirmish: Start Game; galactic: Begin on the battle bar). The
     main-menu background battle draws on the same stream all the time, so the watchpoint stays off until then;
   - --arm-file PATH: no prompt (for a session driven by computer use). Any old PATH / --stop-file is deleted first;
     create PATH (e.g. `New-Item -ItemType File PATH` in PowerShell) right before Begin / the order; removing it
     disarms. --stop-file PATH: create it to stop the trace early (the debugger clears its watchpoints and detaches);
   - --arm-now (or --non-interactive without --arm-file): arms at once.
3. The tracer stops by itself at mode frame --stop-mode-frame of a battle that was not current when it armed
   (0 = never), at --max-hits, when the stop file appears, or when the game exits; the game keeps running. Then
   cu8h_dbg_parse.py writes report.txt (runs, phase totals, checks A-D, VERDICT line) and, with --sites,
   cu8h_sites.py writes sites.txt (load split, per-window rates, call-site counts, formation-ctor draws).
Exit code: the tracer's (0 ok), or 20 when the report could not be written.
"""
import argparse
import datetime
import os
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption\StarWarsG.exe'


def remove(path):
    if path and os.path.exists(path):
        os.remove(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', default='load')
    ap.add_argument('--n-env', type=int, default=4)
    ap.add_argument('--stop-mode-frame', type=int, default=120)
    ap.add_argument('--max-hits', type=int, default=1500000)
    ap.add_argument('--wait-process-sec', type=int, default=900)
    ap.add_argument('--arm-now', action='store_true')
    ap.add_argument('--arm-file')
    ap.add_argument('--stop-file')
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--sites', action='store_true')
    ap.add_argument('--windows')
    ap.add_argument('--table', action='append')
    ap.add_argument('--exe', default=DEFAULT_EXE)
    a = ap.parse_args()
    run = os.path.join(HERE, 'out', datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-trace-' + a.label)
    os.makedirs(run)
    log = os.path.join(run, 'trace.log')
    arm = os.path.abspath(a.arm_file) if a.arm_file else os.path.join(run, 'arm')
    stop = os.path.abspath(a.stop_file) if a.stop_file else os.path.join(run, 'stop')
    if a.arm_file:
        remove(arm)
        remove(stop)
    with open(os.path.join(HERE, 'out', 'latest_trace.txt'), 'w') as f:
        f.write('run=%s\narm=%s\nstop=%s\n' % (run, arm, stop))
    cmd = [sys.executable, '-I', os.path.join(HERE, 'cu8h_dbg.py'), '--out', log, '--wait-process-sec',
           str(a.wait_process_sec), '--stop-mode-frame', str(a.stop_mode_frame), '--max-hits', str(a.max_hits),
           '--stop-file', stop]
    file_armed = bool(a.arm_file)
    cmd += ['--arm-file', arm] if file_armed or not (a.arm_now or a.non_interactive) else ['--arm-now']
    print('cu8h trace: run folder', run, flush=True)
    if file_armed:
        print('arm file : %s  (create it right before Begin / the order; delete it to disarm)' % arm)
        print('stop file: %s  (create it to stop early)' % stop)
        print('stops by itself at mode frame %s of the new battle (0 = never), or when the game exits' %
              a.stop_mode_frame, flush=True)
    tracer = subprocess.Popen(cmd)
    try:
        if not file_armed and not (a.arm_now or a.non_interactive):
            input('Start the game and the battle setup. Press Enter right before you click Begin / Start Game ... ')
            open(arm, 'w').close()
            print('armed; waiting for the tracer to stop (mode frame %d of the new battle)' % a.stop_mode_frame)
        rc = tracer.wait()
    except KeyboardInterrupt:
        open(stop, 'w').close()  # the tracer clears the watchpoints and detaches
        rc = tracer.wait()
    if not os.path.exists(log):
        print('no trace written')
        return rc or 20
    report = os.path.join(run, 'report.txt')
    prc = subprocess.call([sys.executable, '-I', os.path.join(HERE, 'cu8h_dbg_parse.py'), log, '--exe', a.exe,
                           '--n-env', str(a.n_env), '--out', report], stdout=subprocess.DEVNULL)
    if prc != 0 or not os.path.exists(report):
        return 20
    for line in open(report):
        if line.startswith(('hits:', 'seed writes', 'A:', 'B:', 'C:', 'D:', 'VERDICT')):
            print(line.rstrip())
    print('report:', report)
    if a.sites:
        cmd = [sys.executable, '-I', os.path.join(HERE, 'cu8h_sites.py'), log, '--exe', a.exe, '--quiet',
               '--out', os.path.join(run, 'sites.txt')]
        if a.windows:
            cmd += ['--windows', a.windows]
        for t in a.table or []:
            cmd += ['--table', t]
        if subprocess.call(cmd) != 0:
            return 20
    if file_armed:
        remove(arm)
        remove(stop)
    print('CU8H_TRACE_DONE exit=%d run=%s' % (rc, run), flush=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
