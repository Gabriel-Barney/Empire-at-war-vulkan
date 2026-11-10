#!/usr/bin/env python3
"""s78n_run.py - one s78n t08 probe session: back up the player's profile, build + install Mods\\S78NProbe, launch FoC
with it (or wait for you to), scan the game's memory for the probe records while it runs, write the report when the
game exits, then clean up and restore the profile (always, also after an error or Ctrl+C).

  s78n_run.py [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--no-profile] [--dry-run]
              [--wait-start-sec N] [--scan-sec S] [--report-only RECORDS.log]

  --non-interactive   never waits for a key; progress lines; ends with S78N_RUN_DONE exit=<n>
  --no-launch         install and scan only; start the game yourself (README: launch_foc.py from a Claude shell)
  --windowed          launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080
  --no-profile        skip the profile snapshot / restore (not recommended)
  --dry-run           offline check: build the mod into out\\<run>\\dryrun_mod (nothing installed, no game), print the
                      steps and the command line, exit 0
  --report-only       rebuild report.txt from an earlier run's records.log (no game, no install)

Records: the probe Lua keeps strings "S8N|<KIND>|...|END" alive in a global table; this script finds them with a
read-only scan of the game's committed private memory every --scan-sec seconds (../a1_tactical/a1mem.py). It never
writes the game's memory.
Output: tools\\probe_mod\\s78n_t08\\out\\<yyyymmdd-hhmmss>\\ with records.log, report.txt, report.json, config.json,
run.json, run_status.txt (state=installing / waiting_for_game_start / game_running / collecting / done),
cleanup.log, profile_backup\\, profile_restore.json; out\\latest.txt names the newest run folder.
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
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'q2_traces'))
import build_s78n_mod  # noqa: E402
import s78n_cleanup  # noqa: E402
import s78n_analyse  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
MODPATH = ['MODPATH=Mods\\' + build_s78n_mod.MOD_NAME]
NEEDLE = b'S8N|'
RECORD_RE = re.compile(rb'S8N\|[A-Z0-9_]+\|[ -~]{0,520}?\|END')

STEPS = r'''
 s78n t08 session: two skirmish battles in one game session (about 4 + 2 minutes in battle). Notes: README.md
 Rules for both: give NO orders, click nothing on the map (moving the camera is fine), never pause, never change
 the game speed, never save. The probe messages start with "S78N PROBE:".
 A) Single Player > Skirmish > SPACE, map KUAT. Slot 1 (you): EMPIRE, team 1. Slot 2: AI (any difficulty) REBEL,
    team 2. Every other slot CLOSED. Default credits and settings. Start.
    "S78N PROBE: tractor beam trials armed" appears in the first seconds; at 15 s a Star Destroyer (yours) and a
    Nebulon-B frigate (theirs) appear far from both bases, six times in turn. Wait for "S78N PROBE: tractor beam
    trials recorded" (about 4 minutes), then Esc > Quit to the main menu (do not quit the game).
 B) Single Player > Skirmish > LAND, map DAGOBAH (3 players). Slot 1 (you): EMPIRE, team 1. Slot 2: AI UNDERWORLD,
    team 1 (your ally). Slot 3: AI REBEL, team 2. Every other slot CLOSED. Default credits and settings. Start.
    "S78N PROBE: aura trials armed" appears; at 15 s AT-ST walkers of all three players and then the Emperor
    appear far from the bases. Wait for "S78N PROBE: aura trials recorded" (about 2 minutes), Esc > Quit, quit the
    game. The run ends by itself (S78N_RUN_DONE), cleans up and restores the profile.
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_s78n_mod.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--no-profile', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=4.0)
    ap.add_argument('--report-only')
    a = ap.parse_args()
    if a.report_only:
        d = os.path.dirname(os.path.abspath(a.report_only))
        cfgp = os.path.join(d, 'config.json')
        text, res = s78n_analyse.report(a.report_only, os.path.join(d, 'report.txt'), os.path.join(d, 'report.json'),
                                        cfgp if os.path.exists(cfgp) else None)
        say(text)
        return 0 if res['records'] else 10
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    cmdline = ' '.join(['StarWarsG.exe'] + (WINDOWED if a.windowed or a.no_launch else []) + MODPATH)
    if a.dry_run:
        os.makedirs(run_dir, exist_ok=True)
        mod = os.path.join(run_dir, 'dryrun_mod', build_s78n_mod.MOD_NAME)
        cfg = build_s78n_mod.build(a.game, mod)
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        say('DRY RUN: built %s (%d files); sites: %s' % (mod, len(cfg['files']), cfg['site_note']))
        say('tractor: R %.1f, %s E_x %.1f E_y %.1f; trials %s' % (
            cfg['tb']['R'], cfg['tb']['target'], cfg['tb']['ex'], cfg['tb']['ey'],
            ', '.join('%s %.1f' % (t['name'], t['d']) for t in cfg['tb']['trials'])))
        say('\n' + '=' * 110 + STEPS + '=' * 110)
        say('Command line (in %s): %s' % (corr, cmdline))
        say('S78N_RUN_DONE exit=0')
        return 0
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('S78N_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('S78N_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    import a1mem  # noqa: E402 - Windows-only ctypes; not needed for --report-only / --dry-run
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    records_log = os.path.join(run_dir, 'records.log')
    cfg_path = os.path.join(run_dir, 'config.json')
    info = {'game': a.game, 'modpath': MODPATH, 'started': time.time(), 'scans': 0, 'cmdline': cmdline}
    code = 0
    profile = None
    proc = None
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        if not a.no_profile:
            import q2_profile
            profile = q2_profile
            profile.snapshot(run_dir)
        s78n_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        cfg = build_s78n_mod.build(a.game, os.path.join(corr, 'Mods', build_s78n_mod.MOD_NAME))
        with open(cfg_path, 'w') as f:
            json.dump(cfg, f, indent=1)
        info['site'] = cfg.get('site_note')
        say('sites: ' + cfg['site_note'])
        say('tractor: R %.1f, %s E_x %.1f E_y %.1f (%s)' % (cfg['tb']['R'], cfg['tb']['target'], cfg['tb']['ex'],
                                                            cfg['tb']['ey'], cfg['extent']['note']))
        say('\n' + '=' * 110 + STEPS + '=' * 110)
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + (WINDOWED if a.windowed else []) + MODPATH, cwd=corr)
            say('Game launched: ' + cmdline)
        else:
            say('Start the game yourself, in %s:\n  %s' % (corr, cmdline))
            say('From a Claude shell: toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\common\\launch_foc.py '
                + MODPATH[0])
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
        info['exit_seen'] = time.time()
        set_state(run_dir, state='collecting', run_dir=run_dir)
        info['records'] = len(seen)
        text, res = s78n_analyse.report(records_log, os.path.join(run_dir, 'report.txt'),
                                        os.path.join(run_dir, 'report.json'), cfg_path)
        if not res['records']:
            code = 10
            say('NO RECORDS (%d scans): did the battles run with Empire human vs Rebel AI? (README "If something is '
                'missing")' % info['scans'])
        say(text)
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except (Exception, SystemExit) as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        if proc is not None:
            try:
                proc.close()
            except Exception:  # noqa: BLE001
                pass
        rep = s78n_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        prof = None
        if profile is not None:
            try:
                ok_p, msg = s78n_cleanup.restore_profile(run_dir)
                prof = ('verified: ' if ok_p else 'NOT DONE: ') + msg
                say('profile:', msg)
                if not ok_p:
                    say('Close the game if it is running, then run cleanup_s78n.cmd (it restores the profile saved '
                        'in %s).' % run_dir)
            except Exception as e:  # noqa: BLE001
                prof = 'ERROR %s' % e
        info['profile_restore'] = prof
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', profile=prof,
                  run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| profile:', prof,
            '| run folder:', run_dir)
        say('S78N_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
