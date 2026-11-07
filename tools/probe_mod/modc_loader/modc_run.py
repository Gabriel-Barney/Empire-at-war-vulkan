#!/usr/bin/env python3
"""modc_run.py - modc t13 / t14 loader probe: builds Mods\\ModcLoaderProbe from your install, launches FoC with
MODPATH=Mods\\ModcLoaderProbe, waits until the game exits, reads which canary files the probe scripts deleted, writes
out\\<run>\\report.txt and result.json, cleans up.

  modc_run.py [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--digit-listed yes|no|unknown]
              [--empty-listed yes|no|unknown]
              [--wait-start-sec S]

  --no-launch       build and wait; you start the game yourself (the command line is printed)
  --windowed        launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (computer-use driven run)
  --digit-listed    answer for "was MODC PROBE 3 in the Galactic Conquest list?" (else asked at the end; with
                    --non-interactive and no answer it is recorded as unknown)
  --empty-listed    the same for "MODC PROBE 4: NO PLANETS"

Steps in the game: README.md. Exit codes: 0 report written, 10 the control script did not run (probe inconclusive),
20 setup problem, 30 interrupted, 31 error.
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
from probe_run import say, running  # noqa: E402
import build_modc_mod  # noqa: E402
import modc_cleanup  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
STEPS = r'''
In the game (English text):
  1. Single Player -> Galactic Conquest. Look at the campaign list: are "MODC PROBE 3: DIGIT ROOT" and
     "MODC PROBE 4: NO PLANETS" there? Take a screenshot of the list. Do NOT start PROBE 3 or 4.
  2. Start "MODC PROBE 1: BOM SCRIPT" as the EMPIRE. Click nothing for 15 seconds. Note the lines on screen
     ("plot 1 loaded" comes from the story XML; "the BOM script RAN" only if the script ran). Quit to the main menu,
     do not save.
  3. Start "MODC PROBE 2: CONTROL" as the EMPIRE. Click nothing for 15 seconds. Note the lines on screen. Quit to the
     main menu, do not save.
  4. Exit the game. This window then writes the report and removes the probe mod.
'''


def canary_states(mod):
    """canary -> 'deleted' | 'present' (deleted = the probe script that owns it ran that far)."""
    return {c: ('present' if os.path.exists(os.path.join(mod, c)) else 'deleted') for c in build_modc_mod.CANARIES}


def verdicts(st, digit_listed, empty_listed='unknown'):
    ran = st['canary_plain_script.txt'] == 'deleted'
    out = {'control_script_ran': ran}
    if not ran:
        out['t13_bom_skipped'] = 'inconclusive (control script did not run)'
        out['t14_digit_root_unit'] = 'inconclusive (control script did not run)'
        out['t14_bom_digit_root_unit'] = 'inconclusive (control script did not run)'
    else:
        out['t13_bom_skipped'] = 'yes' if st['canary_bom_script.txt'] == 'deleted' else 'no'
        plain_unit = st['canary_unit_plain_root.txt'] == 'deleted'
        out['control_unit_found'] = plain_unit
        for key, c in (('t14_digit_root_unit', 'canary_unit_digit_root.txt'),
                       ('t14_bom_digit_root_unit', 'canary_unit_bom_digit_root.txt')):
            if not plain_unit:
                out[key] = 'inconclusive (control unit not found)'
            else:
                out[key] = 'accepted' if st[c] == 'deleted' else 'rejected'
    out['t14_digit_root_campaign_listed'] = digit_listed
    out['t14_campaign_without_planets_listed'] = empty_listed
    return out


def report_text(st, v, info):
    lines = ['modc t13 / t14 loader probe, %s' % datetime.datetime.now().strftime('%Y-%m-%d %H:%M'),
             'command line: %s' % info.get('cmdline', '?'), '', 'canaries (deleted = that check passed in the game):']
    for c, what in build_modc_mod.CANARIES.items():
        lines.append('  %-34s %-8s %s' % (c, st[c], what))
    lines += ['', 'verdicts:']
    for k, x in v.items():
        lines.append('  %-34s %s' % (k, x))
    lines += ['', 'Record the result in docs/re/MODC_LOADER.md (section "Probe") and on the roadmap (modc t13 / t14).']
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=modc_cleanup.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--digit-listed', choices=('yes', 'no', 'unknown'))
    ap.add_argument('--empty-listed', choices=('yes', 'no', 'unknown'))
    ap.add_argument('--wait-start-sec', type=float, default=900)
    a = ap.parse_args()
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('MODC_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('MODC_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_modc_mod.MOD_NAME)
    args = (WINDOWED if a.windowed else []) + ['MODPATH=Mods\\' + build_modc_mod.MOD_NAME]
    info = {'game': a.game, 'cmdline': ' '.join(['StarWarsG.exe'] + args), 'started': time.time()}
    code = 0
    try:
        modc_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # stale folder from a crashed run
        build_modc_mod.build(a.game, mod)
        say('\n' + '=' * 110 + STEPS + '=' * 110)
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + args, cwd=corr)
            say('Game launched: ' + info['cmdline'])
        else:
            say('Start the game yourself, in %s:\n  %s' % (corr, info['cmdline']))
        while not running('StarWarsG.exe'):
            if time.time() - t0 > a.wait_start_sec:
                raise RuntimeError('StarWarsG.exe did not start within %d s' % a.wait_start_sec)
            time.sleep(2)
        say('Game running. Waiting until it exits ...')
        while running('StarWarsG.exe'):
            time.sleep(3)
        st = canary_states(mod)
        listed = {}
        for key, given, entry in (('digit', a.digit_listed, 'MODC PROBE 3: DIGIT ROOT'),
                                  ('empty', a.empty_listed, 'MODC PROBE 4: NO PLANETS')):
            if given is None and not a.non_interactive:
                ans = input('Was "%s" in the Galactic Conquest list? [y/n/?] ' % entry).strip().lower()
                given = {'y': 'yes', 'yes': 'yes', 'n': 'no', 'no': 'no'}.get(ans, 'unknown')
            listed[key] = given or 'unknown'
        v = verdicts(st, listed['digit'], listed['empty'])
        info.update({'canaries': st, 'verdicts': v})
        text = report_text(st, v, info)
        with open(os.path.join(run_dir, 'report.txt'), 'w') as f:
            f.write(text)
        say(text)
        if not v['control_script_ran']:
            code = 10
            say('INCONCLUSIVE: the control script did not run. Was "MODC PROBE 2: CONTROL" started? (README)')
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except Exception as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        rep = modc_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info.update({'cleanup': rep, 'ended': time.time(), 'exit_code': code})
        with open(os.path.join(run_dir, 'result.json'), 'w') as f:
            json.dump(info, f, indent=1)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('MODC_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
