#!/usr/bin/env python3
"""run_tswp.py - one TSW planet-destroy probe session: build + install the mod, launch FoC with it, wait for the game to
exit, clean up (always, also after an error or Ctrl+C).

  run_tswp.py [--game DIR] [--no-launch] [--non-interactive] [--rate SECONDS]

  --no-launch        install only; start the game yourself (StarWarsG.exe MODPATH=Mods\\TswPlanetProbe), quit it, then
                     press Enter here (with --non-interactive the run waits for StarWarsG.exe to start and exit)
  --rate             GameScoring ServiceRate of the hook (default 0 = every service; see README)
The results are the game's message boxes ("TSWP ..."): screenshot each one before clicking OK (README "Click path").
out\\<yyyymmdd-hhmmss>\\run.txt records what was installed and when the game ran.
"""
import argparse
import datetime
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_mod  # noqa: E402
import cleanup_tswp  # noqa: E402

STEPS = r'''
==========================================================================================================
 TSW planet-destroy probe installed (Mods\TswPlanetProbe). Click path: README.md. In short:
 Single Player > Galactic Conquest > "TSWP PROBE: ALDERAAN" > Empire > Start.
 Wait for "TSWP PROBE: galactic plot loaded". Select the fleet at Byss (Death Star + 3 Star Destroyers), move it to
 Alderaan, fight the space battle (no auto-resolve). The hook fires the Death Star by itself.
 Run W (won): give no orders until the battle ends (attack the station with the Star Destroyers if it drags on).
 Run L (lost): right after the "TSWP ... fired" box, press Retreat.
 Screenshot EVERY "TSWP ..." message box before clicking OK, and the galaxy map after each battle.
==========================================================================================================
'''


def running(name='StarWarsG.exe'):
    try:
        out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + name, '/NH'], capture_output=True, text=True).stdout
    except OSError:
        return False
    return name.lower() in out.lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=cleanup_tswp.DEFAULT_GAME)
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--rate', type=float, default=0.0)
    ap.add_argument('--wait-start-sec', type=int, default=900)
    a = ap.parse_args()
    run_dir = os.path.join(HERE, 'out', datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, 'run.txt'), 'w')

    def note(s):
        print(s)
        log.write(time.strftime('%H:%M:%S ') + s + '\n')
        log.flush()

    rc = 0
    try:
        corruption = os.path.join(a.game, 'corruption')
        exe = os.path.join(corruption, 'StarWarsG.exe')
        if not os.path.exists(exe):
            note('no %s: is the game at %s?' % (exe, a.game))
            return 20
        if running():
            note('StarWarsG.exe is already running: quit it first')
            return 21
        build_mod.build(a.game, os.path.join(corruption, 'Mods', build_mod.MOD_NAME), a.rate)
        note('installed Mods\\%s (ServiceRate %s)' % (build_mod.MOD_NAME, a.rate))
        print(STEPS)
        if a.no_launch:
            if a.non_interactive:
                t0 = time.time()
                while not running() and time.time() - t0 < a.wait_start_sec:
                    time.sleep(2)
                while running():
                    time.sleep(2)
            else:
                input('Start the game yourself, play, quit it, then press Enter here... ')
        else:
            note('launching StarWarsG.exe MODPATH=Mods\\%s' % build_mod.MOD_NAME)
            subprocess.run([exe, 'MODPATH=Mods\\' + build_mod.MOD_NAME], cwd=corruption)
            time.sleep(2)
            while running():
                time.sleep(2)
        note('game exited')
    except KeyboardInterrupt:
        note('interrupted')
        rc = 30
    except BaseException as e:  # noqa: B902 - cleanup must run
        note('error: %r' % (e,))
        rc = 31
    finally:
        ok, msg = cleanup_tswp.cleanup(a.game)
        note('cleanup: ' + msg)
        print('TSWP_RUN_DONE exit=%d cleanup=%s' % (rc, 'ok' if ok else 'INCOMPLETE'))
        log.close()
    return rc


if __name__ == '__main__':
    sys.exit(main())
