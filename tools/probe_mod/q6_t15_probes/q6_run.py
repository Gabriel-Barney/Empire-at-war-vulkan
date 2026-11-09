#!/usr/bin/env python3
"""q6_run.py - one Q6 t15 probe session: build + install the probe mod, launch FoC with it, scan the game's memory for
the probe records while it runs, write the report when the game exits, clean up (always, also after an error or
Ctrl+C).

  q6_run.py [--session vanilla|tr] [--game DIR] [--tr DIR] [--tr-steammod] [--non-interactive] [--no-launch] [--windowed]
            [--wait-start-sec N] [--scan-sec S] [--report-only RECORDS.log]

  --session vanilla  Mods\\Q6T15Probe: galactic campaigns (probes 1, 3, 4) + land and space skirmish (probes 2, 6)
  --session tr       Mods\\Q6T15ProbeTR in front of Thrawn's Revenge: TR Blood Gulch land skirmish (probe 5) and the
                     TR GCMenu campaign by eye (probe 1)
  --non-interactive  never waits for a key; prints progress, ends with Q6_RUN_DONE exit=<n>
  --no-launch        install and scan only; start the game yourself with the printed command line
  --windowed         launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (computer-use driven run)
  --report-only      rebuild report.txt from an earlier run's records.log (no game, no install)

Records: the probe Lua keeps strings "Q6P|<KIND>|p=<probe>|...|END" alive in global tables; this script finds them with
a read-only scan of the game's committed private memory every --scan-sec seconds (../a1_tactical/a1mem.py; retail
_CustomScriptMessage / _MessagePopup show nothing). It never writes the game's memory.
Output: tools\\probe_mod\\q6_t15_probes\\out\\<yyyymmdd-hhmmss>-<session>\\ with records.log, report.txt, run.json,
run_status.txt (state=installing / waiting_for_game_start / game_running / collecting / done), cleanup.log;
out\\latest.txt names the newest run folder.
Exit codes: 0 report written with records; 10 no records; 20 preflight (game / TR missing, game already running);
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
import build_q6_mod  # noqa: E402
import q6_cleanup  # noqa: E402
import q6_parse  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
NEEDLE = b'Q6P|'
RECORD_RE = re.compile(rb'Q6P\|[A-Z0-9_]+\|[ -~]{0,380}?\|END')

STEPS = {
    'vanilla': r'''
 Q6 t15 VANILLA session (probes 1, 2, 3, 4, 6). Full notes: tools\probe_mod\q6_t15_probes\README.md
 Never pause the game and never change the game speed. Esc in a campaign = quit to the main menu, never save.
 A) Single Player > Galactic Conquest > "Q6 PROBE: STORY TIMING" > EMPIRE > Start. Click nothing. Wait for
    "Q6 PROBE: story timing done" (about 2 min 10 s). Esc > Quit to main menu.
 B) Galactic Conquest > "Q6 PROBE: ELIM A (OWN NOTHING)" > EMPIRE > Start. Click nothing for 3 minutes (until "170 s
    passed" or the campaign ends by itself). Note what happened and when (screenshot of any end screen). Quit.
 C) Same for "Q6 PROBE: ELIM B (OWN, THEN LOSE)" (Abregado Rae is yours from 20 s to 50 s) and
    "Q6 PROBE: ELIM C (CONTROL)" (expected to end at once). Optional (q6 t25): "Q6 PROBE: ELIM D (AI GAINS A PLANET)"
    and "Q6 PROBE: ELIM E (FLEET, NO PLANET)", the same way.
 D) Skirmish > LAND, map Geonosis: slot 1 you EMPIRE team 1, slot 2 AI REBEL team 2, all other slots closed. Start.
    No orders. About 5 s in: "selection test set (land) is next to your units". Press Ctrl+A, take a screenshot of the
    whole screen (selection brackets + the selected-units panel). After "tactical part done" (2 min) press Ctrl+A again,
    screenshot, quit the battle.
 E) Skirmish > SPACE, map Kuat, same slots. Same as D: Ctrl+A + screenshot at ~5 s, once more when fighters have
    launched, and after "tactical part done". Quit the battle, quit the game. The run ends by itself.
''',
    'tr': r'''
 Q6 t15 THRAWN'S REVENGE session (probes 5 and 1-TR). Full notes: tools\probe_mod\q6_t15_probes\README.md
 A) Galactic Conquest > the TR GCMenu ("Galactic Conquest Selection Menu", Rebel) > Start. Do NOT pick anything in
    its menus; click nothing for 3 minutes. Note whether the campaign ends (screenshot). Quit (do not save).
 B) Skirmish > LAND, map Blood Gulch: slot 1 you REBEL (New Republic) team 1, slot 2 AI EMPIRE team 2, other slots
    closed, default credits and settings. Start. Give NO orders, ever (your units only defend themselves). Note the
    wall-clock time at the start. Let it run until the battle ends (expected 25 - 40 min; the AI must destroy your
    command centre). Note when your last structure other than the command centre dies, and when the command centre
    dies (screenshot of the end screen with its time). Quit the game. The run ends by itself.
''',
}


def modpath_args(session, corr, tr, steammod=False):
    """Mod arguments. TR: the overlay first (MODPATH / STEAMMOD items are searched in command-line order, earlier wins,
    docs/PHASE2.md "file resolution order"), then TR by a relative MODPATH (default) or by STEAMMOD=1125571106."""
    if session == 'vanilla':
        return ['MODPATH=Mods\\' + build_q6_mod.MOD_NAME]
    if steammod:
        return ['MODPATH=Mods\\' + build_q6_mod.TR_MOD_NAME, 'STEAMMOD=' + os.path.basename(os.path.normpath(tr))]
    try:
        rel = os.path.relpath(tr, corr)
    except ValueError:  # another drive
        rel = tr
    return ['MODPATH=Mods\\' + build_q6_mod.TR_MOD_NAME, 'MODPATH=' + rel]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--session', choices=('vanilla', 'tr'), default='vanilla')
    ap.add_argument('--game', default=build_q6_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--tr', default=build_q6_mod.DEFAULT_TR)
    ap.add_argument('--tr-steammod', action='store_true',
                    help='load TR with STEAMMOD=<workshop id> instead of a relative MODPATH')
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=4.0)
    ap.add_argument('--report-only')
    a = ap.parse_args()
    if a.report_only:
        out = os.path.join(os.path.dirname(os.path.abspath(a.report_only)), 'report.txt')
        text, recs = q6_parse.report(a.report_only, out)
        say(text)
        return 0 if recs else 10
    import a1mem  # noqa: E402 - Windows-only ctypes; not needed for --report-only
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('Q6_RUN_DONE exit=20')
        return 20
    if a.session == 'tr' and not os.path.isdir(os.path.join(a.tr, 'Data')):
        say("Thrawn's Revenge not found at", a.tr, '(subscribe to workshop item 1125571106 or pass --tr DIR)')
        say('Q6_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('Q6_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + a.session)
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    records_log = os.path.join(run_dir, 'records.log')
    mods = modpath_args(a.session, corr, a.tr, a.tr_steammod)
    info = {'game': a.game, 'session': a.session, 'modpath': mods, 'started': time.time(), 'scans': 0}
    code = 0
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        q6_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # stale folders from a crashed run
        if a.session == 'vanilla':
            build_q6_mod.build_vanilla(a.game, os.path.join(corr, 'Mods', build_q6_mod.MOD_NAME))
        else:
            build_q6_mod.build_tr(a.tr, os.path.join(corr, 'Mods', build_q6_mod.TR_MOD_NAME))
        say('\n' + '=' * 110 + STEPS[a.session] + '=' * 110)
        cmdline = ' '.join(['StarWarsG.exe'] + (WINDOWED if a.windowed else []) + mods)
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + (WINDOWED if a.windowed else []) + mods, cwd=corr)
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
        set_state(run_dir, state='collecting', run_dir=run_dir)
        info['records'] = len(seen)
        text, recs = q6_parse.report(records_log, os.path.join(run_dir, 'report.txt'))
        if not recs:
            code = 10
            say('NO RECORDS: did the probe campaigns / battles run? (see README "If something is missing")')
        say(text)
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except Exception as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        rep = q6_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('Q6_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
