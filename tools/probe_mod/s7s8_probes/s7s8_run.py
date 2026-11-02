#!/usr/bin/env python3
"""s7s8_run.py - one S7/S8 probe session: build + install Mods\\S7S8Probe, start the memory reader, launch FoC
(windowed) with the mod, wait for the game to exit, write the report, clean up (always, also after an error or Ctrl+C).

  s7s8_run.py [--game DIR] [--land-map _MP_LAND_GEONOSIS] [--space-map _MP_SPACE_KUAT] [--faction REBEL]
              [--non-interactive] [--no-launch] [--wait-start-sec N] [--scan-sec S] [--no-text]

  --non-interactive  never waits for a key; prints progress, ends with S7S8_RUN_DONE exit=<n>
  --no-launch        install + start the reader only; start the game yourself
                     (StarWarsG.exe WINDOWED MODPATH=Mods\\S7S8Probe in the corruption folder)

One game session, two skirmish battles (README.md, "Run plan"): LAND on Geonosis (about 6.5 minutes, plus the
Saboteur steps H1-H3 done by hand), then SPACE on Kuat (about 4.5 minutes, no orders).
Output: tools\\probe_mod\\s7s8_probes\\out\\<yyyymmdd-hhmmss>\\ with reader.log (+ reader_status.json,
reader_stdout.txt), s7s8_config.json (test sites), report.txt / report.json (s7s8_parse.py), notes.txt (your H1-H3
notes, interactive runs), run.json, run_status.txt, cleanup.log. out\\latest.txt holds the newest run folder.
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
import build_s7s8_mod  # noqa: E402
import s7s8_cleanup  # noqa: E402
import s7s8_parse  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_s7s8_mod.MOD_NAME
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
MENU_NAME = {'_MP_LAND_GEONOSIS': 'GEONOSIS', '_MP_SPACE_KUAT': 'KUAT'}


def say(*a):
    print(*a, flush=True)


def running(name):
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq %s' % name, '/NH'], capture_output=True, text=True)
    return name.lower() in r.stdout.lower()


def set_state(where, **kv):
    with open(os.path.join(where, 'run_status.txt'), 'w') as f:
        for k, v in kv.items():
            f.write('%s=%s\n' % (k, v))
        f.write('time=%s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))


def steps_text(land, space, faction, text):
    bar = '=' * 112
    human = 'EMPIRE' if faction.upper() != 'EMPIRE' else 'REBEL'
    msg = ('on-screen probe messages ("S7S8 PROBE: ...") mark each phase' if text else
           'no on-screen probe messages this time (text file not built): use the clock')
    return '''
%s
 S7/S8 probe installed (Mods\\%s), memory reader running. Full steps: tools\\probe_mod\\s7s8_probes\\README.md
 (%s). Two battles in ONE game session; keep the game windowed, never alt-tab into exclusive fullscreen.

 BATTLE 1 - LAND (about 6.5 min)
  1) Main menu > Single Player > Skirmish > LAND. Map: %s. Slot 1 (you): %s, team 1. Slot 2: AI %s (any
     difficulty), team 2. Every other slot CLOSED. Default credits / tech / settings. Start.
  2) Give your own army NO orders. After about 15 s the camera jumps to an Underworld SABOTEUR the probe gave you
     (next to your units). The probe's units appear in another part of the map on their own; leave them alone.
  3) With only the Saboteur selected (README "H1-H3"):
     H1  Remote Bomb button, click BARE GROUND about one screen-third away; at once right-click the ground on the
         other side to walk him away. Look at the spot for 15 s: does a small bomb marker stay? Note it.
     H2  Remote Bomb button, click bare ground just in front of him (2-3 body lengths) and right-click away AT ONCE
         (the throw releases about 0.4 s after it starts). Look for the marker for 15 s. Note it.
     H3  Remote Bomb button, bare ground just in front of him, let him throw. Wait 20 s.
     If the button never accepts a ground click (only units), note "ground not offered" and stop the H steps.
  4) Wait until about 6 min after the battle started ("land scenarios done"). Esc > Quit to the MAIN MENU
     (do not close the game).

 BATTLE 2 - SPACE (about 4.5 min)
  5) Skirmish > SPACE. Map: %s. Same slots (you %s team 1, AI %s team 2, others closed). Start.
  6) Give NO orders at all; keep your fleet where it starts. Wait about 4.5 min ("space scenarios done").
  7) Esc > Quit, then quit the game. The run ends by itself, writes the report and cleans up.
%s
''' % (bar, build_s7s8_mod.MOD_NAME, msg, MENU_NAME.get(land.upper(), land), human, faction.upper(),
       MENU_NAME.get(space.upper(), space), human, faction.upper(), bar)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_s7s8_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--land-map', default=build_s7s8_mod.DEFAULT_LAND_MAP)
    ap.add_argument('--space-map', default=build_s7s8_mod.DEFAULT_SPACE_MAP)
    ap.add_argument('--faction', default=build_s7s8_mod.DEFAULT_FACTION)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--no-text', action='store_true', help='do not install the Game_Message text file')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--source-game', help=argparse.SUPPRESS)  # offline test: read shipped files here, install in --game
    a = ap.parse_args()
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('S7S8_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('S7S8_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_s7s8_mod.MOD_NAME)
    info = {'game': a.game, 'mod': mod, 'land_map': a.land_map, 'space_map': a.space_map, 'faction': a.faction,
            'started': time.time()}
    reader_log = os.path.join(run_dir, 'reader.log')
    cfg_path = os.path.join(run_dir, 's7s8_config.json')
    code = 0
    reader = None
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        s7s8_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        src = a.source_game or a.game
        _p, cfg = build_s7s8_mod.build(src, mod, None, a.land_map, a.space_map, a.faction, cfg_path, not a.no_text)
        info['land_points_ok'] = cfg['land']['points_ok']
        info['space_points_ok'] = cfg['space']['points_ok']
        info['text'] = cfg['text']
        if not cfg['land']['points_ok']:
            say('WARNING: the land map could not be read; scenarios fall back to the AI unit and the SPMAT range-edge '
                'test (item 1) cannot answer (see s7s8_config.json).')
        reader = subprocess.Popen(
            [sys.executable, '-I', os.path.join(HERE, 's7s8_reader.py'), '--out', reader_log, '--config', cfg_path,
             '--status', os.path.join(run_dir, 'reader_status.json'), '--wait-process-sec',
             str(int(a.wait_start_sec)), '--scan-sec', str(a.scan_sec)],
            stdout=open(os.path.join(run_dir, 'reader_stdout.txt'), 'w'), stderr=subprocess.STDOUT)
        say(steps_text(a.land_map, a.space_map, a.faction, cfg['text']))
        t0 = time.time()
        if a.no_launch and not a.non_interactive:
            set_state(run_dir, state='waiting_for_manual_game', run_dir=run_dir)
            input('Start the game with WINDOWED %s, play both battles, quit it, then press Enter here... ' % MODPATH)
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
        if not a.non_interactive:
            try:
                notes = input('\nYour notes on H1-H3 (marker seen after H1? after H2? ground accepted?), Enter = none: ')
            except EOFError:
                notes = ''
            if notes.strip():
                with open(os.path.join(run_dir, 'notes.txt'), 'w') as f:
                    f.write(notes.strip() + '\n')
                info['notes'] = notes.strip()
        if os.path.exists(reader_log):
            res = s7s8_parse.cmd_parse(argparse.Namespace(log=reader_log, out=os.path.join(run_dir, 'report.txt'),
                                                          json=os.path.join(run_dir, 'report.json')))
            info['armed_runs'] = [r['mode'] for r in res['runs']]
            info['answers'] = res['answers']
            if not res['runs']:
                code = 10
                say('NO PROBE DATA: no ARM record (see report.txt / reader.log).')
            else:
                for k in sorted(res['answers']):
                    say('  %-18s %s' % (k, res['answers'][k]))
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
        rep = s7s8_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1, default=str)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('S7S8_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
