#!/usr/bin/env python3
"""cu8h_trace.py - one cu8h t01 trace: start cu8h_dbg.py against the game, arm it when you say so, write the report.

  cu8h_trace.py [--label NAME] [--n-env 4] [--stop-mode-frame 120] [--max-hits 1500000] [--wait-process-sec 900]
                [--arm-now] [--non-interactive] [--exe <game>\\corruption\\StarWarsG.exe]

1. Creates tools\\probe_mod\\cu8h_probes\\out\\<yyyymmdd-hhmmss>-trace-<label>\\ and starts cu8h_dbg.py there (it waits up
   to --wait-process-sec for StarWarsG.exe; start the game before or after this script).
2. Arming: press Enter right before you click Begin (skirmish: Start Game; galactic: Begin on the battle bar). The
   main-menu background battle draws on the same stream all the time, so the watchpoint stays off until then.
   --arm-now (or --non-interactive) arms at once instead.
3. The tracer stops by itself at mode frame --stop-mode-frame of the new battle (120 = 4 s) and detaches; the game
   keeps running. Then cu8h_dbg_parse.py writes report.txt (runs, phase totals, checks A-D, VERDICT line).
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', default='load')
    ap.add_argument('--n-env', type=int, default=4)
    ap.add_argument('--stop-mode-frame', type=int, default=120)
    ap.add_argument('--max-hits', type=int, default=1500000)
    ap.add_argument('--wait-process-sec', type=int, default=900)
    ap.add_argument('--arm-now', action='store_true')
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--exe', default=DEFAULT_EXE)
    a = ap.parse_args()
    run = os.path.join(HERE, 'out', datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-trace-' + a.label)
    os.makedirs(run)
    log, arm, stop = (os.path.join(run, n) for n in ('trace.log', 'arm', 'stop'))
    cmd = [sys.executable, '-I', os.path.join(HERE, 'cu8h_dbg.py'), '--out', log, '--wait-process-sec',
           str(a.wait_process_sec), '--stop-mode-frame', str(a.stop_mode_frame), '--max-hits', str(a.max_hits),
           '--stop-file', stop]
    cmd += ['--arm-now'] if (a.arm_now or a.non_interactive) else ['--arm-file', arm]
    print('cu8h trace: run folder', run)
    tracer = subprocess.Popen(cmd)
    try:
        if not (a.arm_now or a.non_interactive):
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
                           '--n-env', str(a.n_env), '--out', report])
    if prc != 0 or not os.path.exists(report):
        return 20
    for line in open(report):
        if line.startswith(('seed writes', 'A:', 'B:', 'C:', 'D:', 'VERDICT')):
            print(line.rstrip())
    print('report:', report)
    return rc


if __name__ == '__main__':
    sys.exit(main())
