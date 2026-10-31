#!/usr/bin/env python3
"""a1_run.py - one A1 tactical-AI probe session: build + install Mods\\A1TacticalProbe, start the memory reader,
launch FoC with the mod, wait for the game to exit, write the report, clean up (always, also after an error or Ctrl+C).

  a1_run.py [--probe freestore|landing|both] [--game DIR] [--non-interactive] [--no-launch] [--wait-start-sec N]
            [--scan-sec S]

  --probe            which click path to print and which report to stress (the mod and the reader are the same):
                     freestore = A1 t02 (skirmish land + space), landing = A1 t04 (campaign invasion + skirmish land),
                     both (default) = everything in one game session
  --non-interactive  never waits for a key; prints progress, ends with A1_RUN_DONE exit=<n>
  --no-launch        install + start the reader only; start the game yourself
                     (StarWarsG.exe MODPATH=Mods\\A1TacticalProbe in the corruption folder)

Output: tools\\probe_mod\\a1_tactical\\out\\<yyyymmdd-hhmmss>-<probe>\\ with reader.log (+ reader_status.json),
report.txt / report.json (a1_parse.py), caps.json, run.json, run_status.txt (state=... for a background caller),
cleanup.log. out\\latest.txt holds the newest run folder.
Exit codes: 0 report written with probe data; 10 no tactical battle or no probe data; 20 / 21 preflight;
30 interrupted; 31 error.
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
import a1_cleanup  # noqa: E402
import a1_parse  # noqa: E402
import build_a1_mod  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_a1_mod.MOD_NAME

STEPS = {
    'freestore': r'''
 FREE-STORE TIMING (A1 t02). In every battle: give NO orders and click nothing on the map.
 A) Main menu > Single Player > Skirmish > LAND. Map: UTAPAU. Slot 1 (you): EMPIRE, team 1. Slot 2: AI
    (any difficulty) REBEL, team 2. Other slots closed. Defaults for credits / tech. Start.
    Wait 90 seconds from the moment the battlefield is shown, then Esc > Quit (back to the main menu).
 B) Skirmish > SPACE. Map: KUAT. Same slots (you EMPIRE, AI REBEL). Start, wait 90 s, Esc > Quit.
''',
    'landing': r'''
 LANDING POINTS (A1 t04). In every battle: give NO orders and click nothing on the map.
 C) Main menu > Single Player > Galactic Conquest. In the campaign list pick "S10 PROBE A: TATOOINE", faction
    EMPIRE, Start (accept defaults). On the galaxy map click nothing: after about 3 s a line "S10 PROBE: story plot
    loaded ..." appears and Underworld ground forces fly from Byss to Tatooine and invade.
    When the battle prompt appears choose to FIGHT the battle (not auto-resolve, not retreat).
    The Tatooine land battle loads. Wait 60 s, then Esc > Quit (to the main menu).
    If no battle starts within 60 s of the map appearing, note it, quit to the main menu and go on.
''',
    'landing_skirmish': r'''
 D) Main menu > Single Player > Skirmish > LAND. Map: UTAPAU. Slot 1 (you): EMPIRE, team 1. Slot 2: AI
    (any difficulty) REBEL, team 2. Other slots closed. Start. Wait 60 s, Esc > Quit.
''',
}


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


def steps_text(probe):
    keys = {'freestore': ['freestore'], 'landing': ['landing', 'landing_skirmish'],
            'both': ['freestore', 'landing']}[probe]  # in 'both', skirmish A doubles as the landing skirmish D
    text = ''.join(STEPS[k] for k in keys)
    bar = '=' * 110
    return '\n%s\n A1 probe installed (Mods\\%s), memory reader running. Full click path: tools\\probe_mod\\' \
        'a1_tactical\\README.md\n%s\n Then quit the game (main menu > Quit). The run ends by itself.\n%s\n' % (
            bar, build_a1_mod.MOD_NAME, text, bar)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--probe', choices=('freestore', 'landing', 'both'), default='both')
    ap.add_argument('--game', default=build_a1_mod.ted_owners.DEFAULT_GAME)
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
        say('A1_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('A1_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + a.probe)
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_a1_mod.MOD_NAME)
    info = {'game': a.game, 'mod': mod, 'probe': a.probe, 'started': time.time()}
    reader_log = os.path.join(run_dir, 'reader.log')
    code = 0
    reader = None
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        a1_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        src = a.source_game or a.game
        caps = os.path.join(run_dir, 'caps.json')
        with open(caps, 'w') as f:
            json.dump(build_a1_mod.caps_table(src), f, indent=1, sort_keys=True)
        build_a1_mod.build(src, mod, None, campaign=a.probe != 'freestore')
        reader = subprocess.Popen(
            [sys.executable, '-I', os.path.join(HERE, 'a1_reader.py'), '--out', reader_log, '--status',
             os.path.join(run_dir, 'reader_status.json'), '--caps', caps, '--wait-process-sec',
             str(int(a.wait_start_sec)), '--scan-sec', str(a.scan_sec)],
            stdout=open(os.path.join(run_dir, 'reader_stdout.txt'), 'w'), stderr=subprocess.STDOUT)
        say(steps_text(a.probe))
        t0 = time.time()
        if a.no_launch and not a.non_interactive:
            set_state(run_dir, state='waiting_for_manual_game', run_dir=run_dir)
            input('Start the game with %s, play, quit it, then press Enter here... ' % MODPATH)
        else:
            if not a.no_launch:
                subprocess.Popen([exe, MODPATH], cwd=corr)
                say('Game launched: StarWarsG.exe %s' % MODPATH)
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
            res = a1_parse.cmd_parse(argparse.Namespace(log=reader_log, out=os.path.join(run_dir, 'report.txt'),
                                                        json=os.path.join(run_dir, 'report.json')))
            info['instances'] = len(res['freestore']['instances'])
            info['land_battles'] = len(res['landing']['battles'])
            if not info['instances'] and not info['land_battles']:
                code = 10
                say('NO PROBE DATA: no free-store records and no land battle (see reader.log / reader_status.json).')
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
        rep = a1_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('A1_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
