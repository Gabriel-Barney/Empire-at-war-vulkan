#!/usr/bin/env python3
"""r1_run.py - one R1 hardpoint probe session: build + install Mods\\R1HardpointProbe, launch FoC with it (unless
--no-launch), scan the game's memory for the probe records while it runs, write the report when the game exits, clean
up (always, also after an error or Ctrl+C).

  r1_run.py [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--dry-run] [--wait-start-sec N]
            [--scan-sec S] [--report-only RECORDS.log]

  --no-launch        install and scan only; start the game yourself with the printed command line
  --windowed         launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (computer-use driven run)
  --non-interactive  never waits for a key; prints progress, ends with R1_RUN_DONE exit=<n>
  --dry-run          offline check: build the mod into out\\<run>\\dryrun_mod (nothing installed, no game), print the
                     steps and the command line, exit 0
  --report-only      rebuild report.txt from an earlier run's records.log

Records: the probe Lua keeps strings "R1P|<KIND>|...|END" alive in a global table; this script finds them with a
read-only scan of the game's committed private memory every --scan-sec seconds (../a1_tactical/a1mem.py). It never
writes the game's memory.
Output: tools\\probe_mod\\r1_hardpoint\\out\\<yyyymmdd-hhmmss>\\ with records.log, report.txt, report.json,
config.json, run.json, run_status.txt (state=installing / waiting_for_game_start / game_running / collecting / done),
cleanup.log; out\\latest.txt names the newest run folder.
Exit codes: 0 report written with records; 10 no records; 20 preflight (game missing, game already running);
30 interrupted; 31 error.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
from probe_run import say, running, set_state, scan_once  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
import build_r1_mod  # noqa: E402
import r1_cleanup  # noqa: E402
import r1_parse  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
MODPATH = ['MODPATH=Mods\\' + build_r1_mod.MOD_NAME]
NEEDLE = b'R1P|'
RECORD_RE = re.compile(rb'R1P\|[A-Z0-9_]+\|[ -~]{0,800}?\|END')

STEPS = r'''
 R1 HARDPOINT probe (about 10 minutes in the battle). Full notes: tools\probe_mod\r1_hardpoint\README.md
 1) Single Player > Skirmish > SPACE, map KUAT. Slot 1 (you): EMPIRE, team 1. Slot 2: AI (Easy) REBEL, team 2.
    Every other slot CLOSED. Default credits and settings. Start.
 2) Give NO orders (moving the camera is fine; never pause, never change the speed, never save).
    0:10 two Star Destroyers (yours) appear in the empty corner: {minimap}
    Frame ship A (zoom in until it fills about 2/3 of the screen) and take the BEFORE shots before 2:00.
 3) One kill per minute, each announced 10 s ahead by an "R1 PROBE:" message: 2:00 A front-left turret, 3:00 A engines,
    4:00 A shield generator, 5:00 A tractor beam, 6:00 ship A destroyed, 7:00 B rear-right turret,
    8:00 B front-right turret. After each kill: screenshot + zooms of that spot (README "Driver steps").
 4) "R1 PROBE: all done" at about 9:05. Esc > Quit (main menu), then quit the game. The run ends by itself and
    cleans up.
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_r1_mod.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=4.0)
    ap.add_argument('--report-only')
    ap.add_argument('--source-zip', help=argparse.SUPPRESS)  # offline dry run without an install
    a = ap.parse_args()
    if a.report_only:
        d = os.path.dirname(os.path.abspath(a.report_only))
        text, recs = r1_parse.report(a.report_only, os.path.join(d, 'report.txt'), os.path.join(d, 'report.json'))
        say(text)
        say('R1_RUN_DONE exit=%d' % (0 if recs else 10))
        return 0 if recs else 10
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    cmdline = ' '.join(['StarWarsG.exe'] + (WINDOWED if a.windowed else []) + MODPATH)
    if a.dry_run:
        os.makedirs(run_dir, exist_ok=True)
        mod = os.path.join(run_dir, 'dryrun_mod', build_r1_mod.MOD_NAME)
        cfg = build_r1_mod.build(a.game, mod, a.source_zip, no_text=bool(a.source_zip))
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        say('DRY RUN: built %s (%d files); site: %s' % (mod, len(cfg['files']), cfg['site_note']))
        say('\n' + '=' * 110 + STEPS.format(minimap=cfg['minimap']) + '=' * 110)
        say('Command line (in %s): %s' % (corr, cmdline))
        say('R1_RUN_DONE exit=0')
        return 0
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('R1_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('R1_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    import a1mem  # noqa: E402 - Windows-only ctypes; not needed for --report-only / --dry-run
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    records_log = os.path.join(run_dir, 'records.log')
    info = {'game': a.game, 'modpath': MODPATH, 'started': time.time(), 'scans': 0}
    code = 0
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        r1_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # stale folder from a crashed run
        cfg = build_r1_mod.build(a.game, os.path.join(corr, 'Mods', build_r1_mod.MOD_NAME))
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        info['site'] = cfg.get('site_note')
        say('\n' + '=' * 110 + STEPS.format(minimap=cfg['minimap']) + '=' * 110)
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + (WINDOWED if a.windowed else []) + MODPATH, cwd=corr)
            say('Game launched: ' + cmdline)
        else:
            say('Start the game yourself, in %s:\n  %s' % (corr, cmdline))
        set_state(run_dir, state='waiting_for_game_start', run_dir=run_dir, cmdline=cmdline)
        pids = []
        while not pids:
            if time.time() - t0 > a.wait_start_sec:
                raise RuntimeError('StarWarsG.exe did not start within %d s' % a.wait_start_sec)
            time.sleep(2)
            pids = a1mem.find_pids()
        proc = a1mem.Proc(pids[0])
        info['identity'] = 'ok' if proc.pe_identity() else 'unknown (not FoC 1.121?)'
        set_state(run_dir, state='game_running', run_dir=run_dir)
        say('Game running (pid %d, exe %s). Scanning for records every %.0f s until it exits ...' % (
            pids[0], info['identity'], a.scan_sec))
        seen = set()
        with open(records_log, 'w') as log:
            while proc.alive():
                new, st = scan_once(proc, NEEDLE, RECORD_RE, seen, log)
                info['scans'] += 1
                if new:
                    say('  +%d records (%d total, %.0f MB scanned)' % (new, len(seen), st['bytes'] / 1e6))
                time.sleep(a.scan_sec)
        proc.close()
        info['exit_seen'] = time.time()
        set_state(run_dir, state='collecting', run_dir=run_dir)
        info['records'] = len(seen)
        text, recs = r1_parse.report(records_log, os.path.join(run_dir, 'report.txt'),
                                     os.path.join(run_dir, 'report.json'))
        if not recs:
            code = 10
            say('NO RECORDS: did the space skirmish run with you EMPIRE and the AI REBEL?')
        say(text)
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except (Exception, SystemExit) as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        rep = r1_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('R1_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
