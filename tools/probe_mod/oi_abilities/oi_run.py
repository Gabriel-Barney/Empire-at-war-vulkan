#!/usr/bin/env python3
"""oi_run.py - one OI ability probe session: build + install Mods\\OIAbilityProbe, start the memory reader, launch FoC
(windowed) with the mod, wait for the game to exit, write the report, clean up (always, also after an error or Ctrl+C).

  oi_run.py [--game DIR] [--map _MP_LAND_DAGOBAH] [--faction REBEL] [--non-interactive] [--no-launch]
            [--wait-start-sec N] [--scan-sec S]

  --non-interactive  never waits for a key; prints progress, ends with OI_RUN_DONE exit=<n>
  --no-launch        install + start the reader only; start the game yourself
                     (StarWarsG.exe WINDOWED MODPATH=Mods\\OIAbilityProbe in the corruption folder)

Output: tools\\probe_mod\\oi_abilities\\out\\<yyyymmdd-hhmmss>\\ with reader.log (+ reader_status.json,
reader_stdout.txt), oi_config.json (spawn points), report.txt / report.json (oi_parse.py), run.json, run_status.txt,
cleanup.log. out\\latest.txt holds the newest run folder.
Exit codes: 0 report written with an armed probe run; 10 no armed run / no data; 20 preflight; 30 interrupted;
31 error.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
from probe_run import say, running, set_state  # noqa: E402
import build_oi_mod  # noqa: E402
import oi_cleanup  # noqa: E402
import oi_parse  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_oi_mod.MOD_NAME
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
SKIRMISH_NAME = {'_MP_LAND_DAGOBAH': 'DAGOBAH'}


def steps_text(map_name, faction):
    bar = '=' * 110
    human = 'EMPIRE' if faction.upper() != 'EMPIRE' else 'REBEL'
    return '''
%s
 OI ability probe installed (Mods\\%s), memory reader running. Details: tools\\probe_mod\\oi_abilities\\README.md
 1) Main menu > Single Player > Skirmish > LAND. Map: %s.
    Slot 1 (you): %s, team 1. Slot 2: AI (any difficulty) %s, team 2. Every other slot CLOSED.
    Default credits / tech. Start.
 2) Give NO orders and click nothing on the map (moving the camera is fine). The probe spawns its own units
    (Han Solo + MAL rocket vehicles on land and in the water, a Rancor, an Infiltrator, Yoda).
    Wait 5 minutes from the moment the battlefield is shown.
 3) Esc > Quit (back to the main menu), then quit the game. The run ends by itself and cleans up.
%s
''' % (bar, build_oi_mod.MOD_NAME, SKIRMISH_NAME.get(map_name.upper(), map_name), human, faction.upper(), bar)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_oi_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--map', default=build_oi_mod.DEFAULT_MAP)
    ap.add_argument('--faction', default=build_oi_mod.DEFAULT_FACTION)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--source-game', help=argparse.SUPPRESS)  # offline test: read shipped files here, install in --game
    a = ap.parse_args()
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('OI_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('OI_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_oi_mod.MOD_NAME)
    info = {'game': a.game, 'mod': mod, 'map': a.map, 'faction': a.faction, 'started': time.time()}
    reader_log = os.path.join(run_dir, 'reader.log')
    cfg_path = os.path.join(run_dir, 'oi_config.json')
    code = 0
    reader = None
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        oi_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        src = a.source_game or a.game
        _p, cfg = build_oi_mod.build(src, mod, None, a.map, a.faction, cfg_path)
        info['points_ok'] = cfg['points_ok']
        if not cfg['points_ok']:
            say('WARNING: the map could not be read; the water scenarios cannot answer 4.12 (see oi_config.json).')
        reader = subprocess.Popen(
            [sys.executable, '-I', os.path.join(HERE, 'oi_reader.py'), '--out', reader_log, '--config', cfg_path,
             '--status', os.path.join(run_dir, 'reader_status.json'), '--wait-process-sec',
             str(int(a.wait_start_sec)), '--scan-sec', str(a.scan_sec)],
            stdout=open(os.path.join(run_dir, 'reader_stdout.txt'), 'w'), stderr=subprocess.STDOUT)
        say(steps_text(a.map, a.faction))
        t0 = time.time()
        if a.no_launch and not a.non_interactive:
            set_state(run_dir, state='waiting_for_manual_game', run_dir=run_dir)
            input('Start the game with WINDOWED %s, play, quit it, then press Enter here... ' % MODPATH)
        else:
            if not a.no_launch:
                # Windowed: an exclusive-fullscreen game minimises on the first focus change.
                subprocess.Popen([exe] + WINDOWED + [MODPATH], cwd=corr)
                say('Game launched: StarWarsG.exe %s %s' % (' '.join(WINDOWED), MODPATH))
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
            info['reader_exit'] = reader.wait(timeout=60)
        except subprocess.TimeoutExpired:
            reader.terminate()
            reader.wait(timeout=10)
            info['reader_exit'] = 'terminated'
        if os.path.exists(reader_log):
            res = oi_parse.cmd_parse(argparse.Namespace(log=reader_log, out=os.path.join(run_dir, 'report.txt'),
                                                        json=os.path.join(run_dir, 'report.json')))
            info['armed_runs'] = len(res['runs'])
            if not res['runs']:
                code = 10
                say('NO PROBE DATA: no ARM record (see report.txt / reader.log).')
            else:
                info['water'] = [r.get('water') for r in res['runs']]
            say('\nreport written to', os.path.join(run_dir, 'report.txt'))
        else:
            code = 10
            say('reader.log missing (see reader_stdout.txt).')
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except (Exception, SystemExit) as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        if reader is not None and reader.poll() is None:
            reader.terminate()
        rep = oi_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('OI_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
