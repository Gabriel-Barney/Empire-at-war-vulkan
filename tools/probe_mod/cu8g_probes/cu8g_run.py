#!/usr/bin/env python3
"""cu8g_run.py - one cu8g probe session: build + install Mods\\CU8GProbe, launch FoC with it, scan the game's memory
for the probe records while it runs, write the report when the game exits, clean up (always, also after an error or
Ctrl+C).

  cu8g_run.py --session stick|abil|maps-miss|maps-hit [--map-loose] [--game DIR] [--non-interactive] [--no-launch]
              [--windowed] [--dry-run] [--wait-start-sec N] [--scan-sec S] [--report-only RECORDS.log]

  --session stick      t07 stickiness: SPACE skirmish Kuat, you REBEL, AI EMPIRE (about 5 min)
  --session abil       t09 (optional): LAND skirmish Dagobah, then SPACE skirmish Kuat, you EMPIRE, AI REBEL (12 min)
  --session maps-miss  t08: campaign "CU8G PROBE: MISSING MAPS" without a matching map (fallback expected)
  --session maps-hit   t08: the same with the faction-patched Alderaan copy in the mod (--map-loose: in
                       corruption\\Data\\Art\\Maps instead)
  --non-interactive    never waits for a key; prints progress, ends with CU8G_RUN_DONE exit=<n>
  --no-launch          install and scan only; start the game yourself with the printed command line
  --windowed           launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (computer-use driven run)
  --dry-run            offline check: build the mod into out\\<run>\\dryrun_mod (nothing is installed, no game),
                       print the steps and the command line, exit 0
  --report-only        rebuild report.txt from an earlier run's records.log (no game, no install)

Records: the probe Lua keeps strings "C8G|<KIND>|...|END" alive in global tables; this script finds them with a
read-only scan of the game's committed private memory every --scan-sec seconds (../a1_tactical/a1mem.py). It never
writes the game's memory.
Output: tools\\probe_mod\\cu8g_probes\\out\\<yyyymmdd-hhmmss>-<session>\\ with records.log, report.txt, config.json,
run.json, run_status.txt (state=installing / waiting_for_game_start / game_running / collecting / done),
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
import build_cu8g_mod  # noqa: E402
import cu8g_cleanup  # noqa: E402
import cu8g_parse  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
MODPATH = ['MODPATH=Mods\\' + build_cu8g_mod.MOD_NAME]
NEEDLE = b'C8G|'
RECORD_RE = re.compile(rb'C8G\|[A-Z0-9_]+\|[ -~]{0,480}?\|END')

STEPS = {
    'stick': r'''
 cu8g t07 STICKINESS session (about 5 minutes). Full notes: tools\probe_mod\cu8g_probes\README.md
 1) Single Player > Skirmish > SPACE, map KUAT. Slot 1 (you): REBEL, team 1. Slot 2: AI (any difficulty) EMPIRE,
    team 2. Every other slot CLOSED. Default credits and settings. Start.
 2) Give NO orders and click nothing on the map (moving the camera is fine; never pause, never change the speed).
    "CU8G PROBE: stickiness trials running" appears at about 10 s. The probe spawns frigates, a corvette and a TIE
    bomber far from both bases, six times. Wait for "CU8G PROBE: stickiness trials done" (about 3 to 4 minutes).
 3) Esc > Quit (main menu), then quit the game. The run ends by itself and cleans up.
''',
    'abil': r'''
 cu8g t09 ABILITY session (optional, about 12 minutes). Full notes: tools\probe_mod\cu8g_probes\README.md
 A) Skirmish > LAND, map DAGOBAH. Slot 1 (you): EMPIRE, team 1. Slot 2: AI REBEL, team 2. Other slots CLOSED.
    Start. Give NO orders. The probe spawns a Canderous tank, a T2-B and three AT-STs in an open spot. Wait for
    "CU8G PROBE: lure trials done" (about 2 minutes), then Esc > Quit to the main menu.
 B) Skirmish > SPACE, map KUAT, same slots. Start, no orders. The probe spawns the Accuser, a corvette and Home One.
    Wait for "CU8G PROBE: cancel trials done" (about 3 minutes). Quit the battle, quit the game.
''',
    'maps': r'''
 cu8g t08 MISSING-MAPS session ({session}, about 5 to 10 minutes). Full notes: tools\probe_mod\cu8g_probes\README.md
 Never save. Never auto-resolve: always choose the button that starts the battle.
 1) Single Player > Galactic Conquest > "CU8G PROBE: MISSING MAPS" > REBEL > Start.
 2) LAND: select the ground units on Abregado Rae (2 infantry squads, 1 light tank brigade) and send them to JABIIM
    (Empire, no fleet there). When the battle dialog comes up choose to fight. Write down what happens: which map
    loads (screenshot after it loads), or an error, a return to the galaxy, an auto-resolve, a crash.
    If a battle loads: give no orders for 30 s, take a screenshot, then Esc > Quit to the main menu (this ends the
    probe campaign; do not save).
 3) {space_step}
 4) Quit the game. If the game crashed, the run still ends by itself; start the same session again only for the part
    that did not run yet.
''',
}
SPACE_STEP = {
    'maps-miss': 'SPACE: start the same campaign again (Galactic Conquest > "CU8G PROBE: MISSING MAPS" > REBEL), send\n'
                 '    the fleet at Abregado Rae (2 Nebulon-B, 1 Corellian corvette) to THYFERRA (one Empire Tartan\n'
                 '    cruiser). Choose to fight; note what happens as in 2). If a battle loads: 30 s, screenshot, quit.',
    'maps-hit': 'No space step in maps-hit (the patched map is a land map).',
}


def steps(session):
    if session in STEPS:
        return STEPS[session]
    return STEPS['maps'].format(session=session, space_step=SPACE_STEP[session])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--session', choices=build_cu8g_mod.SESSIONS, required=True)
    ap.add_argument('--map-loose', action='store_true')
    ap.add_argument('--game', default=build_cu8g_mod.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=4.0)
    ap.add_argument('--report-only')
    ap.add_argument('--source-game', help=argparse.SUPPRESS)  # offline test: read shipped files here, install in --game
    a = ap.parse_args()
    if a.report_only:
        out = os.path.join(os.path.dirname(os.path.abspath(a.report_only)), 'report.txt')
        text, recs = cu8g_parse.report(a.report_only, out)
        say(text)
        return 0 if recs else 10
    if a.map_loose and a.session != 'maps-hit':
        say('--map-loose only applies to --session maps-hit')
        return 20
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    src_game = a.source_game or a.game
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + a.session +
                           ('-loose' if a.map_loose else ''))
    cmdline = ' '.join(['StarWarsG.exe'] + (WINDOWED if a.windowed else []) + MODPATH)
    if a.dry_run:
        os.makedirs(run_dir, exist_ok=True)
        mod = os.path.join(run_dir, 'dryrun_mod', build_cu8g_mod.MOD_NAME)
        cfg = build_cu8g_mod.build(a.session, src_game, mod, map_loose=False)
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        say('DRY RUN: built %s (%d files)%s' % (mod, len(cfg['files']), '; --map-loose would put %s in %s' % (
            build_cu8g_mod.HIT_MAP, build_cu8g_mod.loose_map_paths(a.game)[0]) if a.map_loose else ''))
        if 'site_note' in cfg:
            say('site: ' + cfg['site_note'])
        say('\n' + '=' * 110 + steps(a.session) + '=' * 110)
        say('Command line (in %s): %s' % (corr, cmdline))
        say('CU8G_RUN_DONE exit=0')
        return 0
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('CU8G_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('CU8G_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    import a1mem  # noqa: E402 - Windows-only ctypes; not needed for --report-only / --dry-run
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    records_log = os.path.join(run_dir, 'records.log')
    info = {'game': a.game, 'session': a.session, 'map_loose': a.map_loose, 'modpath': MODPATH,
            'started': time.time(), 'scans': 0}
    code = 0
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        cu8g_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # stale files from a crashed run
        cfg = build_cu8g_mod.build(a.session, src_game, os.path.join(corr, 'Mods', build_cu8g_mod.MOD_NAME),
                                   map_loose=a.map_loose, install_game=a.game)
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        info['site'] = cfg.get('site_note')
        say('\n' + '=' * 110 + steps(a.session) + '=' * 110)
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
        text, recs = cu8g_parse.report(records_log, os.path.join(run_dir, 'report.txt'))
        if not recs:
            code = 10
            say('NO RECORDS: did the probe battle / campaign run? (README "If something is missing")')
        say(text)
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except (Exception, SystemExit) as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        rep = cu8g_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('CU8G_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
