#!/usr/bin/env python3
"""a2f_run.py - one a2f t07 budget probe session: back up the player's profile, build + install Mods\\A2fBudgetProbe, launch
FoC with the mod, scan the game's memory for the probe Lua's records and watch the AI budget block while it runs, write
the report when the game exits, then clean up and restore the profile (always, also after an error or Ctrl+C).

  a2f_run.py [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--no-mod-equations] [--no-percept] [--no-profile]
             [--wait-start-sec N] [--scan-sec S] [--report-only RUN_DIR]

  --non-interactive   never waits for a key; progress lines; ends with A2F_RUN_DONE exit=<n>
  --no-launch         install and scan only; start the game yourself (StarWarsG.exe MODPATH=Mods\\A2fBudgetProbe in corruption)
  --windowed          launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (computer-use driven run)
  --no-mod-equations  build without the A2FB_* equation file and calls (fallback if the game ignores the mod's equations)
  --no-percept        build without channel D (the D4 EvaluatePerception scripts): a t04 budget run they cannot perturb
  --no-profile        skip the profile snapshot / restore (not recommended)
  --report-only DIR   rebuild report.txt from DIR\\records.txt + budget_watch.jsonl (no game, no install)
  --port-trace ERR    with --report-only: the per-cycle P / I table against a2j_port.py's port.err (a2j t04)

Output: tools\\probe_mod\\a2f_budget\\out\\<yyyymmdd-hhmmss>\\ with records.txt (every probe string found), budget_watch.jsonl
(the memory watcher), report.txt / report.json / samples.tsv, run.json, run_status.txt, cleanup.log, profile_backup\\
and profile_restore.json; out\\latest.txt names the newest run folder.
Exit codes: 0 records found and the report written; 10 no records; 20 preflight (game missing / already running);
30 interrupted; 31 error.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
from probe_run import say, running, set_state, scan_once  # noqa: E402
sys.path.insert(0, os.path.join(PROBE_MOD, 'a1_tactical'))
sys.path.insert(0, os.path.join(PROBE_MOD, 'q2_traces'))
import a2f_cleanup  # noqa: E402
import a2f_report  # noqa: E402
import a2f_watch  # noqa: E402
import build_a2f_mod  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_a2f_mod.MOD_NAME
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
NEEDLE = b'A2FB' + b'1|'
RECORD_RE = re.compile(rb'A2FB1\|[SFPDQ]\|[^|]*\|\d+\|[A-Z0-9]+\|[ -~]{0,330}?\|END')

STEPS = r'''
 A2F T07 BUDGET PROBE. Nothing is clicked for you; follow these steps and then leave the game alone.
 1) Main menu > Single Player > Galactic Conquest > campaign "Heart of the Maw" (the Corruption sandbox GC).
    Faction REBEL (so the Empire is an AI player), difficulty Medium, Start (accept the defaults). Do NOT pick Empire:
    then the Empire has no AI budget to look at.
 2) The campaign opens with the "Active Story Objectives" box: close it. The galaxy then runs. Click nothing else, give
    no orders, do not pause; keep the game window in front (FoC pauses when it loses focus). "A2FB PROBE: story script
    running" shows at the start.
 3) After 5 minutes of game time "A2FB PROBE: 5-minute dense phase done" shows: keep the game running. If the AI attacks
    and a battle prompt appears, choose auto-resolve (the prompt pauses galactic time). At 25 minutes of game time (normal
    speed) "A2FB PROBE: 25 minutes recorded" shows: keep going. At 30 minutes "A2FB PROBE: 30 minutes recorded" shows
    (a2j t04: the 30-minute run). Then Esc > Quit to the main menu > Quit. Do not save the campaign. The run finishes by
    itself (A2F_RUN_DONE exit=<n>).
'''


def canary_status(mod, percept=True):
    """{'Story': 'deleted' | 'present' | 'no mod folder'} - _CustomScriptMessage deleted the file when the script ran."""
    res = {}
    for ch, fn in build_a2f_mod.CANARIES.items():
        if not percept and ch.startswith('Percept'):
            res[ch] = 'not built (--no-percept)'
        elif not os.path.isdir(mod):
            res[ch] = 'no mod folder'
        else:
            res[ch] = 'present (script did not run)' if os.path.exists(os.path.join(mod, fn)) else 'deleted (script ran)'
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_a2f_mod.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--no-mod-equations', action='store_true')
    ap.add_argument('--no-percept', action='store_true')
    ap.add_argument('--no-profile', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--report-only')
    ap.add_argument('--port-trace', help='with --report-only: a2j_port.py port.err for the per-cycle P / I table (a2j t04)')
    a = ap.parse_args()
    if a.report_only:
        d = a.report_only
        res, text = a2f_report.run(os.path.join(d, 'records.txt'), os.path.join(d, 'budget_watch.jsonl'),
                                   os.path.join(d, 'A2FB_TYPES.json'), d, port_trace=a.port_trace)
        say(text)
        return 0 if res['records'] else 10
    import a1mem  # noqa: E402 - Windows-only ctypes
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('A2F_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('A2F_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_a2f_mod.MOD_NAME)
    records_path = os.path.join(run_dir, 'records.txt')
    watch_path = os.path.join(run_dir, 'budget_watch.jsonl')
    info = {'game': a.game, 'mod': mod, 'started': time.time(), 'scans': 0, 'mod_equations': not a.no_mod_equations,
            'percept': not a.no_percept}
    code = 0
    proc = None
    watcher_thread = None
    stop = threading.Event()
    profile = None
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        if not a.no_profile:
            import q2_profile
            profile = q2_profile
            profile.snapshot(run_dir)
        a2f_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        types = build_a2f_mod.build(a.game, mod, use_mod_eq=not a.no_mod_equations, percept=not a.no_percept)
        with open(os.path.join(run_dir, 'A2FB_TYPES.json'), 'w') as f:
            json.dump(types, f, indent=1)
        qt = os.path.join(mod, 'A2FB_QUEUE_TYPES.json')     # a2j: ground / space queue per type (report D3)
        if os.path.exists(qt):
            with open(qt, 'rb') as src, open(os.path.join(run_dir, 'A2FB_QUEUE_TYPES.json'), 'wb') as dst:
                dst.write(src.read())
        say('\n' + '=' * 110 + '\n Mod installed (Mods\\%s). Full notes: tools\\probe_mod\\a2f_budget\\README.md'
            % build_a2f_mod.MOD_NAME + STEPS + '=' * 110)
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + (WINDOWED if a.windowed else []) + [MODPATH], cwd=corr)
            say('Game launched: StarWarsG.exe %s%s' % ('WINDOWED ... ' if a.windowed else '', MODPATH))
        else:
            say('Start the game yourself: StarWarsG.exe %s (in %s)' % (MODPATH, corr))
        set_state(run_dir, state='waiting_for_game_start', run_dir=run_dir)
        pids = []
        while not pids:
            if time.time() - t0 > a.wait_start_sec:
                raise RuntimeError('StarWarsG.exe did not start within %d s' % a.wait_start_sec)
            time.sleep(2)
            pids = a1mem.find_pids()
        proc = a1mem.Proc(pids[0])
        info['identity'] = 'ok' if proc.pe_identity() else 'unknown (not FoC 1.121?)'
        set_state(run_dir, state='game_running', run_dir=run_dir)
        say('Game running (pid %d, exe %s). Scanning for records every %.0f s and watching the budget block until it '
            'exits ...' % (pids[0], info['identity'], a.scan_sec))
        game = a1mem.Game(proc)
        wf = open(watch_path, 'w')
        watcher = a2f_watch.Watcher(proc, game, wf)
        watcher_thread = threading.Thread(target=watcher.run, args=(lambda: not stop.is_set() and proc.alive(),),
                                          daemon=True)
        watcher_thread.start()
        seen = set()
        with open(records_path, 'w') as log:
            while proc.alive():
                new, st = scan_once(proc, NEEDLE, RECORD_RE, seen, log)
                info['scans'] += 1
                if new:
                    say('  +%d records (%d total, %.0f MB scanned); watcher lines %d' % (
                        new, len(seen), st['bytes'] / 1e6, watcher.lines))
                time.sleep(a.scan_sec)
        stop.set()
        watcher_thread.join(5)
        wf.close()
        info['records'] = len(seen)
        info['watch_lines'] = watcher.lines
        info['canary'] = canary_status(mod, not a.no_percept)
        say('canary:', info['canary'])
        set_state(run_dir, state='collecting', run_dir=run_dir)
        res, text = a2f_report.run(records_path, watch_path, os.path.join(run_dir, 'A2FB_TYPES.json'), run_dir)
        if not res['records']:
            code = 10
            say('NO RECORDS (%d scans): did the story plot load? see canary above and README "If nothing came back"'
                % info['scans'])
        say(text)
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except Exception as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        stop.set()
        if watcher_thread is not None:
            watcher_thread.join(5)
        if proc is not None:
            try:
                proc.close()
            except Exception:  # noqa: BLE001
                pass
        rep = a2f_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        prof_ok = None
        if profile is not None:
            try:
                ok_p, msg = a2f_cleanup.restore_profile(run_dir)
                prof_ok = ('verified: ' if ok_p else 'NOT DONE: ') + msg
                say('profile:', msg)
                if not ok_p:
                    say('Close the game if it is running, then run cleanup_a2f.cmd (it restores the profile saved in '
                        '%s).' % run_dir)
            except Exception as e:  # noqa: BLE001
                prof_ok = 'ERROR %s' % e
        info['profile_restore'] = prof_ok
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE',
                  profile=prof_ok, run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| profile:', prof_ok,
            '| run folder:', run_dir)
        say('A2F_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
