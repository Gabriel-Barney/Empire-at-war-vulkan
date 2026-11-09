#!/usr/bin/env python3
r"""ccp_run.py - one ccpgu t02 Capture Control Points probe session: snapshot the player's profile, preset the skirmish
game type / map, build + install Mods\CcpProbe, start the memory reader, launch FoC windowed, wait for the game to exit,
write the report, then remove the mod and restore the profile (always, also after an error or Ctrl+C).

  ccp_run.py [--game DIR] [--map _MP_LAND_KAMINO] [--human EMPIRE] [--non-interactive] [--no-launch]
             [--no-preset] [--no-profile] [--no-spawn] [--no-invulnerable] [--wait-start-sec N] [--scan-sec S]

  --non-interactive  never waits for a key; prints progress, ends with CCP_RUN_DONE exit=<n>
  --no-launch        install + start the reader only; start the game yourself, from the corruption folder:
                     StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\CcpProbe
                     (with --non-interactive the run waits up to --wait-start-sec for the game to start, then for it
                     to exit, and cleans up by itself)
  --no-preset        do not preset the profile (game type Capture Control Points + the map); pick them in the menu
  --no-profile       no snapshot / restore and no preset (not recommended)

Preset (default): before the game starts, HKCU\Software\Petroglyph\StarWars FOC\Profiles\Profile0\Player gets
LastInternetGameType = 3 (the skirmish setup opens on game type 4, Capture Control Points; UI.md 9.7: type = stored + 1)
and LastInternetMapLand = .\Data\Art\Maps\<map>.TED (the remembered land map, shared by types 2 and 4). The snapshot is
taken first, so the restore at the end puts the old values back.

Output: tools\probe_mod\ccp_probe\out\<yyyymmdd-hhmmss>\ with reader.log (+ reader_status.json: 'phase' and 'hint' for
the driver, reader_stdout.txt), ccp_config.json, report.txt / report.json (ccp_parse.py), run.json, run_status.txt,
cleanup.log, profile_backup\ / profile_restore.json. out\latest.txt names the newest run folder.
Exit codes: 0 report with a declared victory; 10 no battle data / no declaration; 20 preflight (game missing or already
running); 30 interrupted; 31 error.
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
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'q2_traces'))
from probe_run import say, running, set_state  # noqa: E402
import build_ccp_mod  # noqa: E402
import ccp_cleanup  # noqa: E402
import ccp_parse  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_ccp_mod.MOD_NAME
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
PROFILE_KEY = r'Software\Petroglyph\StarWars FOC\Profiles\Profile0\Player'
GAME_TYPE_CCP_STORED = 3       # LastInternetGameType = type - 1; type 4 = Capture Control Points (UI.md 9.7)


def preset_values(map_name):
    return {'LastInternetGameType': ('dword', GAME_TYPE_CCP_STORED),
            'LastInternetMapLand': ('sz', '.\\Data\\Art\\Maps\\%s.TED' % map_name.upper())}


def preset_profile(map_name, writer=None):
    """Writes the preset values; writer(name, kind, value) for tests. Returns the values written."""
    vals = preset_values(map_name)
    if writer is None:
        import winreg

        def writer(name, kind, value):
            with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, PROFILE_KEY, 0, winreg.KEY_SET_VALUE) as k:
                winreg.SetValueEx(k, name, 0, winreg.REG_DWORD if kind == 'dword' else winreg.REG_SZ, value)
    for name, (kind, value) in vals.items():
        writer(name, kind, value)
    return vals


def map_label(map_name):
    return map_name.upper().replace('_MP_LAND_', '').capitalize()


def steps_text(map_name, human, preset, ai):
    bar = '=' * 110
    pick = ('already preset: the setup screen should open on CAPTURE CONTROL POINTS with %s selected; check both'
            % map_label(map_name)) if preset else (
        'in the game-type list (the box with LAND / SPACE / CAPTURE CONTROL POINTS rows, top left of the setup screen) '
        'click CAPTURE CONTROL POINTS, then pick %s in the map list' % map_label(map_name))
    return '''
%s
 ccpgu t02 Capture Control Points probe installed (Mods\\%s), memory reader running.
 Details: tools\\probe_mod\\ccp_probe\\README.md. Progress: out\\<run>\\reader_status.json ("phase", "hint").
 1) Main menu > Single Player > Skirmish. Game type and map: %s.
    Slot 1 (you): %s, team 1. Slot 2: AI (any difficulty) %s, team 2. Every other slot CLOSED. Defaults otherwise.
    Start the battle.
 2) Give NO orders, click nothing on the map. 5 s in, the probe spawns your troopers and a walker on the
    control point; they capture it (10 s), the 30 s domination timer runs, the victory text shows, ~7 s later the
    battle ends. About 60 s of battle in all (phase "ended" in reader_status.json).
 3) On the end screen: continue / quit to the main menu, then quit the game. The run ends and cleans up by itself.
%s
''' % (bar, build_ccp_mod.MOD_NAME, pick, human.upper(), ai, bar)


def restore_stale(out_root):
    """A previous run whose profile snapshot was never restored (crash): restore it before taking a new one."""
    latest = os.path.join(out_root, 'latest.txt')
    if not os.path.exists(latest):
        return None
    prev = open(latest).read().strip()
    if not prev or not os.path.isdir(prev):
        return None
    if os.path.exists(os.path.join(prev, 'profile_backup', 'profile.json')) and \
            not os.path.exists(os.path.join(prev, ccp_cleanup.DONE)):
        return ccp_cleanup.restore_profile(prev)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_ccp_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--map', default=build_ccp_mod.DEFAULT_MAP)
    ap.add_argument('--human', default=build_ccp_mod.DEFAULT_HUMAN, choices=sorted(build_ccp_mod.SIDES))
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--no-preset', action='store_true')
    ap.add_argument('--no-profile', action='store_true')
    ap.add_argument('--no-spawn', action='store_true')
    ap.add_argument('--no-invulnerable', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--source-game', help=argparse.SUPPRESS)  # offline test: read shipped files here, install in --game
    a = ap.parse_args()
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('CCP_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('CCP_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    os.makedirs(out_root, exist_ok=True)
    stale = restore_stale(out_root)
    if stale is not None:
        say('previous run profile:', stale[1])
        if not stale[0]:
            say('CCP_RUN_DONE exit=20')
            return 20
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_ccp_mod.MOD_NAME)
    info = {'game': a.game, 'mod': mod, 'map': a.map, 'human': a.human, 'started': time.time(),
            'modpath': MODPATH, 'launch': [exe] + WINDOWED + [MODPATH]}
    reader_log = os.path.join(run_dir, 'reader.log')
    cfg_path = os.path.join(run_dir, 'ccp_config.json')
    status_path = os.path.join(run_dir, 'reader_status.json')
    code = 0
    reader = None
    profile = None
    preset = False
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        ccp_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        if not a.no_profile:
            import q2_profile
            profile = q2_profile
            profile.snapshot(run_dir)
            if not a.no_preset:
                info['preset'] = preset_profile(a.map)
                preset = True
                say('profile preset: %s' % info['preset'])
        src = a.source_game or a.game
        _p, cfg = build_ccp_mod.build(src, mod, None, a.map, a.human, cfg_path, not a.no_spawn, not a.no_invulnerable,
                                      os.path.basename(run_dir))
        exp = cfg.get('expected') or {}
        info['expected_points'] = len(exp.get('relevant', [])) if exp else None
        if exp and not exp.get('control'):
            say('WARNING: %s has no CONTROL tag: it is not listed under Capture Control Points.' % a.map)
        reader = subprocess.Popen(
            [sys.executable, '-I', os.path.join(HERE, 'ccp_reader.py'), '--out', reader_log, '--config', cfg_path,
             '--status', status_path, '--wait-process-sec', str(int(a.wait_start_sec)), '--scan-sec', str(a.scan_sec)],
            stdout=open(os.path.join(run_dir, 'reader_stdout.txt'), 'w'), stderr=subprocess.STDOUT)
        say(steps_text(a.map, a.human, preset, cfg['ai']))
        say('run folder:', run_dir)
        t0 = time.time()
        if a.no_launch and not a.non_interactive:
            set_state(run_dir, state='waiting_for_manual_game', run_dir=run_dir)
            input('Start the game (cd corruption; StarWarsG.exe %s %s), play, quit it, then press Enter here... '
                  % (' '.join(WINDOWED), MODPATH))
            while running('StarWarsG.exe'):
                time.sleep(3)
        else:
            if not a.no_launch:
                # Windowed: an exclusive-fullscreen game minimises on the first focus change.
                subprocess.Popen([exe] + WINDOWED + [MODPATH], cwd=corr)
                say('Game launched: StarWarsG.exe %s %s' % (' '.join(WINDOWED), MODPATH))
            else:
                say('Waiting for the game: cd "%s" and run StarWarsG.exe %s %s' % (corr, ' '.join(WINDOWED), MODPATH))
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
            res = ccp_parse.cmd_parse(argparse.Namespace(log=reader_log, config=cfg_path,
                                                         out=os.path.join(run_dir, 'report.txt'),
                                                         json=os.path.join(run_dir, 'report.json')))
            battles = res['battles']
            info['battles'] = len(battles)
            info['results'] = [{'measured': b.get('measured'), 'frames': b.get('frames'),
                                'start_owners': sorted({p['start_faction'] for p in b['points'] if p['relevant']})}
                               for b in battles]
            if not any((b.get('frames') or {}).get('D') is not None for b in battles):
                code = 10
                say('NO DECLARATION RECORDED (see report.txt / reader.log).')
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
        rep = ccp_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        prof_ok = 'skipped'
        if profile is not None:
            try:
                ok_p, msg = ccp_cleanup.restore_profile(run_dir)
                prof_ok = 'OK' if ok_p else 'NOT RESTORED'
                say('profile:', msg)
                if not ok_p:
                    say('Close the game if it is running, then run cleanup_ccp.cmd (it restores the profile saved in '
                        '%s).' % run_dir)
            except Exception as e:  # noqa: BLE001
                prof_ok = 'ERROR %s' % e
                say('profile restore error:', e)
        info['profile_restore'] = prof_ok
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1, default=str)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE',
                  profile=prof_ok, run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| profile:', prof_ok,
            '| run folder:', run_dir)
        say('CCP_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
