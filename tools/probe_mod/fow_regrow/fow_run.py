#!/usr/bin/env python3
"""fow_run.py - one FOW regrow probe session: build + install Mods\\FowRegrowProbe, start the memory reader, launch FoC
with the mod, wait for the game to exit, write the report, clean up (always, also after an error or Ctrl+C).

  fow_run.py [--game DIR] [--map _MP_LAND_POLUS] [--non-interactive] [--no-launch] [--wait-start-sec N]
             [--scan-sec S]

  --non-interactive  never waits for a key; prints progress, ends with FOW_RUN_DONE exit=<n>
  --no-launch        install + start the reader only; start the game yourself
                     (StarWarsG.exe MODPATH=Mods\\FowRegrowProbe in the corruption folder)

Output: tools\\probe_mod\\fow_regrow\\out\\<yyyymmdd-hhmmss>\\ with reader.log (+ reader_status.json), report.txt /
report.json (fow_parse.py), run.json, run_status.txt (state=... for a background caller), cleanup.log.
out\\latest.txt holds the newest run folder.
Exit codes: 0 report written with probe data; 10 no land battle or no probe data; 20 preflight (game missing or
already running); 30 interrupted; 31 error.
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
import build_fow_mod  # noqa: E402
import fow_cleanup  # noqa: E402
import fow_parse  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_fow_mod.MOD_NAME
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
STEPS = r'''
 FOW REGROW PROBE (fowf t06). Give NO orders and click nothing on the map; just wait.
 1) Main menu > Single Player > Skirmish > LAND. Map: (2) POLUS WINDS. Slot 1 (you): EMPIRE, team 1.
    Slot 2: AI (Easy) REBEL, team 2. Other slots closed. Default credits / tech. Start.
 2) Wait 3 minutes from the moment the battlefield is shown (the probe runs by itself from about 6 s in;
    two Rebel MPTL artillery units appear at the Rebel base, that is the probe).
 3) Esc > Quit (back to the main menu), then main menu > Quit. The run ends by itself.
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=fow_cleanup.DEFAULT_GAME)
    ap.add_argument('--map', default=build_fow_mod.DEFAULT_MAP)
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
        say('FOW_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('FOW_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_fow_mod.MOD_NAME)
    info = {'game': a.game, 'mod': mod, 'map': a.map, 'started': time.time()}
    reader_log = os.path.join(run_dir, 'reader.log')
    code = 0
    reader = None
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        fow_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        src = a.source_game or a.game
        minfo = build_fow_mod.map_info(src, a.map)
        info['map_info'] = {k: v for k, v in minfo.items() if k != 'src'}
        with open(os.path.join(run_dir, 'geometry.txt'), 'w') as f:
            f.write('\n'.join(build_fow_mod.describe(a.map, minfo)) + '\n')
        build_fow_mod.build(src, mod, a.map, None, minfo)
        reader = subprocess.Popen(
            [sys.executable, '-I', os.path.join(HERE, 'fow_reader.py'), '--out', reader_log, '--status',
             os.path.join(run_dir, 'reader_status.json'), '--wait-process-sec', str(int(a.wait_start_sec)),
             '--scan-sec', str(a.scan_sec)],
            stdout=open(os.path.join(run_dir, 'reader_stdout.txt'), 'w'), stderr=subprocess.STDOUT)
        bar = '=' * 110
        say('\n%s\n FOW probe installed (Mods\\%s), memory reader running. Full click path: tools\\probe_mod\\'
            'fow_regrow\\README.md\n%s%s\n' % (bar, build_fow_mod.MOD_NAME, STEPS, bar))
        t0 = time.time()
        if a.no_launch and not a.non_interactive:
            set_state(run_dir, state='waiting_for_manual_game', run_dir=run_dir)
            input('Start the game with %s, play, quit it, then press Enter here... ' % MODPATH)
        else:
            if not a.no_launch:
                # Windowed: an exclusive-fullscreen game minimises on the first focus change and cannot be driven.
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
            res = fow_parse.cmd_parse(argparse.Namespace(log=reader_log, out=os.path.join(run_dir, 'report.txt'),
                                                         json=os.path.join(run_dir, 'report.json')))
            info['verdicts'] = [(r['n'], r.get('q1'), r.get('q2')) for r in res]
            if not res or not any(r.get('records') for r in res):
                code = 10
                say('NO PROBE DATA: no land battle with probe records (see reader.log / reader_status.json).')
            say('\nreport written to', os.path.join(run_dir, 'report.txt'))
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
        rep = fow_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1, default=str)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('FOW_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
