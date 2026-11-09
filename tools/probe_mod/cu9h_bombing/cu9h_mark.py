#!/usr/bin/env python3
"""cu9h_mark.py - the driver's state check and mark for the cu9h bombing-run probe (roadmap group cu9h t02).

  cu9h_mark.py [LABEL] [--run RUN_DIR]

Reads the bombing-run state of the running game once (read-only, cu9h_mem.snapshot) and prints one line: phase
(GALAXY / BATTLE_NO_RECORD / COOLDOWN / READY / TARGETING / RUN_IN_PROGRESS / LOCKED), the button (ENABLED / DISABLED),
the dial (int(fraction x 100), 100 = full), ready_in (seconds of game time to the next ready frame), targeting (the
HUD flag), the blob colour (GREEN / RED), the ground point and the cursor. With a LABEL it also appends the state to
<run>\\marks.log (default run: out\\latest.txt) so the report can pair it with a screenshot. Labels start with the
question: q1_..., q2_..., q3_... (no spaces). Without the game it prints the runner's live.txt instead.
"""
import argparse
import os
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
import cu9h_mem as M  # noqa: E402


def latest_run():
    p = os.path.join(HERE, 'out', 'latest.txt')
    return open(p).read().strip() if os.path.exists(p) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('label', nargs='?')
    ap.add_argument('--run')
    a = ap.parse_args()
    run = a.run or latest_run()
    s = None
    try:
        import a1mem
        pids = a1mem.find_pids()
        if pids:
            p = a1mem.Proc(pids[0])
            try:
                s = M.snapshot(p.read, p.base)
            finally:
                p.close()
    except (OSError, ImportError) as e:
        print('no direct read (%s)' % e)
    t = time.time()
    if s is None:
        live = os.path.join(run, 'live.txt') if run else None
        if live and os.path.exists(live):
            lines = open(live).read().splitlines()
            print('(game not readable; live.txt) ' + (lines[0] if lines else ''))
            body = lines[1] if len(lines) > 1 else ''
        else:
            print('no game and no live.txt')
            return 2
    else:
        print(M.summary(s))
        body = M.line(s)
    if a.label:
        if not run:
            print('no run folder (out\\latest.txt missing): mark not written')
            return 1
        label = a.label.replace(' ', '_')
        with open(os.path.join(run, 'marks.log'), 'a') as f:
            f.write('%.4f MARK label=%s %s\n' % (t, label, body))
        print('marked %s in %s' % (label, os.path.join(run, 'marks.log')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
