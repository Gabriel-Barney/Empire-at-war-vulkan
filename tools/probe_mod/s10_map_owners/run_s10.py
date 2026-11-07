#!/usr/bin/env python3
"""run_s10.py - HISTORICAL (S10 is done, see README.md): one S10 map-owner probe session: build + install the mod, start the memory reader, launch FoC with
the mod, wait for the game to exit, write the tables, clean up (always, also after an error or Ctrl+C).

  run_s10.py [--game DIR] [--non-interactive] [--no-launch] [--no-popup] [--wait-start-sec N]

  --non-interactive  never waits for a key (for a background run): prints progress and ends with S10_RUN_DONE
  --no-launch        install + start the reader only; start the game yourself (StarWarsG.exe MODPATH=Mods\\S10MapOwners).
                     Interactive: press Enter after quitting the game. Non-interactive: the run waits up to
                     --wait-start-sec for StarWarsG.exe to appear and then for it to exit.
  --no-popup         the GameScoring hook shows no message boxes (the memory reader still works)

Output channel (the 2026-10-29 run showed that _CustomScriptMessage writes nothing in retail):
  bin\\s10reader.exe (read-only, ReadProcessMemory) dumps every land battle's object store at mode frames 1, 300 and
  1200 (~0 / 10 / 40 s): id, owner player id, type, position. It also records the text of every message box of the
  game (the hook's "S10 ..." popups) and closes it (OK), so nobody has to read or click them.
Output: out\\<yyyymmdd-hhmmss>\\ with reader.log (+ reader_status.json), table.txt / table.json (from reader.log),
table_hook.txt (only if a hook log appeared), run.json, run_status.txt (state=... for a background caller), cleanup.log.
out\\latest.txt holds the path of the newest run folder.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
import build_mod  # noqa: E402
import cleanup_s10  # noqa: E402
import s10_parse  # noqa: E402
import ted_owners  # noqa: E402

READER = os.path.join(HERE, 'bin', 's10reader.exe')
WARN_POPUP = '_warn_message_popup.txt'  # the retail game's own file; maybe where _MessagePopup text goes

STEPS = r'''
==========================================================================================================
 S10 map-owner probe is installed and the memory reader is running. Exact click path: README.md "Click path".
 In every battle: give NO orders, wait until the battle has run 45 s or more, then Esc > Quit Battle / Retreat.
 Message boxes titled like "S10 ..." are closed by the reader by itself; if one stays open, click OK.

 A) Single Player > Galactic Conquest > "S10 PROBE A: TATOOINE" > faction EMPIRE > Start. Click nothing on the map.
    ~3 s: on-screen text "S10 PROBE: story plot loaded ..."; Underworld ground units fly Byss -> Tatooine and invade.
    Battle prompt: choose to fight (not auto-resolve). Land battle -> wait 45 s -> Esc > Quit to main menu.
    (Fallback if no battle starts within ~60 s: same campaign as UNDERWORLD; when the fleet is over Tatooine and no
    battle starts, use the "Ground forces in orbit" panel > Invade.)
 B) Same with "S10 PROBE B: JABIIM", faction REBEL (Abregado Rae -> Jabiim).
 C) Single Player > Skirmish > Land, map POLUS WINDS. Slot 1 (you): EMPIRE, Team 1. Slot 2: AI REBEL, Team 2.
    Other slots closed. Start -> wait 45 s -> Esc > Quit.
 Then quit the game (main menu > Quit). The run ends by itself.
==========================================================================================================
'''


def running(name):
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq %s' % name, '/NH'], capture_output=True, text=True)
    return name.lower() in r.stdout.lower()


def say(*a):
    print(*a, flush=True)


def set_state(where, **kv):
    with open(os.path.join(where, "run_status.txt"), "w") as f:
        for k, v in kv.items():
            f.write('%s=%s\n' % (k, v))
        f.write('time=%s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))


def ensure_reader():
    if os.path.exists(READER):
        return True
    say('building bin\\s10reader.exe (build_reader.cmd) ...')
    subprocess.run(['cmd', '/c', os.path.join(HERE, 'build_reader.cmd')], capture_output=True)
    return os.path.exists(READER)


def file_state(p):
    try:
        st = os.stat(p)
        return st.st_size, st.st_mtime
    except OSError:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--no-popup', action='store_true')
    ap.add_argument('--popup', action='store_true', help=argparse.SUPPRESS)  # old flag; popups are the default now
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--source-game', help=argparse.SUPPRESS)  # offline test: read shipped files here, install in --game
    a = ap.parse_args()
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    if not ensure_reader():
        say('bin\\s10reader.exe missing and build_reader.cmd failed (logs\\s10reader_build.txt).')
        return 21
    run_dir = os.path.join(HERE, 'out', datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(HERE, 'out', 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', 'S10MapOwners')
    popup = not a.no_popup
    info = {'game': a.game, 'mod': mod, 'popup': popup, 'started': time.time(), 'log_found': None}
    reader_log = os.path.join(run_dir, 'reader.log')
    warn = os.path.join(corr, WARN_POPUP)
    warn0 = file_state(warn)
    code = 0
    reader = None
    set_state(run_dir, state='installing')
    try:
        cleanup_s10.collect_logs(a.game, run_dir, prefix='stale_')  # an old hook log must not count for this run
        build_mod.build(a.source_game or a.game, mod, popup)
        reader = subprocess.Popen([READER, '--out', reader_log, '--status', os.path.join(run_dir, 'reader_status.json'),
                                   '--wait-process-sec', str(int(a.wait_start_sec)), '--timeout-sec', '14400',
                                   '--frames', '1,300,1200', '--dismiss-popups'],
                                  stdout=open(os.path.join(run_dir, 'reader_stdout.txt'), 'w'), stderr=subprocess.STDOUT)
        say(STEPS)
        t0 = time.time()
        info['launch'] = t0
        if a.no_launch and not a.non_interactive:
            set_state(run_dir, state='waiting_for_manual_game', run_dir=run_dir)
            input('Start the game with MODPATH=Mods\\S10MapOwners, play, quit it, then press Enter here... ')
        else:
            if not a.no_launch:
                subprocess.Popen([exe, 'MODPATH=Mods\\S10MapOwners'], cwd=corr)
                say('Game launched.')
            set_state(run_dir, state='waiting_for_game_start', run_dir=run_dir)
            while not running('StarWarsG.exe'):
                if time.time() - t0 > a.wait_start_sec:
                    raise RuntimeError('StarWarsG.exe did not start within %d s' % a.wait_start_sec)
                time.sleep(2)
            set_state(run_dir, state='game_running', run_dir=run_dir)
            say('Game running. Waiting for it to exit ...')
            time.sleep(10)
            while running('StarWarsG.exe'):
                time.sleep(3)
        set_state(run_dir, state='collecting', run_dir=run_dir)
        try:
            info['reader_exit'] = reader.wait(timeout=30)
        except subprocess.TimeoutExpired:
            reader.terminate()
            reader.wait(timeout=10)
            info['reader_exit'] = 'terminated'
        try:
            with open(os.path.join(run_dir, 'reader_stdout.txt')) as f:
                info['reader_stdout'] = f.read().splitlines()[-10:]
            say('reader: ' + ' | '.join(info['reader_stdout']))
        except OSError:
            pass
        w1 = file_state(warn)
        if w1 is not None and w1 != warn0 and w1[0] > 0:
            shutil.copy2(warn, os.path.join(run_dir, WARN_POPUP))  # copy only; the game's file is left as it is
            info['warn_message_popup'] = 'changed (%d bytes), copied' % w1[0]
        logs = cleanup_s10.collect_logs(a.game, run_dir, since=t0 - 5)
        info['log_found'] = logs
        if logs:
            s10_parse.cmd_parse(argparse.Namespace(log=logs[0], out=os.path.join(run_dir, 'table_hook.txt'), tag='t0'))
        if os.path.exists(reader_log):
            res = s10_parse.cmd_parse(argparse.Namespace(log=reader_log, out=os.path.join(run_dir, 'table.txt'),
                                                         tag='t0'))
            info['battles'] = sorted(res)
            say('\ntable written to', os.path.join(run_dir, 'table.txt'))
            if not res:
                code = 10
                say('NO LAND BATTLE DUMP in reader.log (see reader_status.json / reader_stdout.txt).')
        else:
            code = 10
            say('reader.log missing (see reader_stdout.txt).')
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except Exception as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        if reader is not None and reader.poll() is None:
            reader.terminate()
        rep = cleanup_s10.cleanup(a.game, run_dir)
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('S10_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
