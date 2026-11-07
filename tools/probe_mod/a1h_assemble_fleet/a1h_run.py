#!/usr/bin/env python3
"""a1h_run.py - one a1h t09 Assemble_Fleet probe session: build + install Mods\\A1hAssembleProbe, launch FoC with the
mod, scan the game's memory for the story script's records while it runs, write the report when it exits, clean up
(always, also after an error or Ctrl+C).

  a1h_run.py [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--wait-start-sec N] [--scan-sec S]
             [--report-only RECORDS.log]

  --non-interactive  never waits for a key; prints progress, ends with A1H_RUN_DONE exit=<n>
  --no-launch        install and scan only; start the game yourself
                     (StarWarsG.exe MODPATH=Mods\\A1hAssembleProbe in the corruption folder)
  --windowed         launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 (for a computer-use driven run)
  --report-only      rebuild report.txt from an earlier run's records.log (no game, no install)

Records: the story Lua keeps strings "A1HR|<kind>|n=<k>|...|END" alive in its global A1H_REC table; this script finds
them with a read-only scan of the game's committed private memory every --scan-sec seconds (a1_tactical/a1mem.py;
retail _CustomScriptMessage / _MessagePopup show nothing). It never writes the game's memory.
Output: tools\\probe_mod\\a1h_assemble_fleet\\out\\<yyyymmdd-hhmmss>\\ with records.log, report.txt, run.json,
run_status.txt (state=installing / waiting_for_game_start / game_running / collecting / done), cleanup.log;
out\\latest.txt names the newest run folder.
Exit codes: 0 report with a verdict for every case; 10 no records or a case without a verdict; 20 preflight
(game missing / already running); 30 interrupted; 31 error.
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
import a1h_cleanup  # noqa: E402
import build_a1h_mod  # noqa: E402

MODPATH = 'MODPATH=Mods\\' + build_a1h_mod.MOD_NAME
WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
NEEDLE = b'A1HR|'
RECORD_RE = re.compile(rb'A1HR\|([A-Z_]+)\|n=(\d+)\|([ -~]{0,360}?)\|END')

STEPS = r'''
 A1H PROBE (a1h t09: Assemble_Fleet of surface units). Click nothing on the galaxy map; no orders.
 1) Main menu > Single Player > Galactic Conquest. In the campaign list pick "A1H PROBE: ASSEMBLE FLEET",
    faction EMPIRE, Start (accept defaults).
 2) On the galaxy map click nothing. After about 3 s "A1H PROBE: story plot loaded ..." appears; then one line per
    case: "A1H PROBE case C / A / B / D: ..." (about 15 s apart) and "A1H PROBE: done ..." after about a minute.
 3) When "done" shows: take a screenshot of the galaxy map (fleet icons over Tatooine, Abregado Rae and Byss; the
    planets' ground-unit counts). Then Esc > Quit to the main menu > Quit. The run ends by itself.
'''


def parse_record(text):
    """'A1HR|KIND|n=3|k=v|...|END' -> (n, kind, {k: v})."""
    parts = text.split('|')
    kv = {}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            kv[k] = v
    return int(kv.get('n', '0')), parts[1], kv


def report(records_log, out):
    """report.txt from records.log. Returns {case: verdict}."""
    recs = {}
    for line in open(records_log, encoding='ascii', errors='replace'):
        line = line.strip()
        if line.startswith('A1HR|'):
            n, kind, kv = parse_record(line)
            recs[n] = (kind, kv)
    verdicts = {}
    lines = ['a1h t09 probe: Assemble_Fleet of units on a planet surface (port rule: they are launched into the fleet,',
             'GalacticSim::assemble_fleet; a1g t06 [inferred]). Records in story order:', '']
    for n in sorted(recs):
        kind, kv = recs[n]
        if kind == 'VERDICT':
            verdicts[kv.get('case', '?')] = kv.get('verdict', '?')
        lines.append('%3d %-11s %s' % (n, kind, ' '.join('%s=%s' % (k, v) for k, v in kv.items() if k != 'n')))
    lines += ['', 'Verdicts (LAUNCHED = every unit is inside the fleet right after Assemble_Fleet and 10 s later):']
    for c, desc in build_a1h_mod.CASES.items():
        lines.append('  %-50s %s' % (desc, verdicts.get(c, 'NO VERDICT (see the records above)')))
    lines += ['', 'Reading: LAUNCHED in C, A and B confirms the port rule (surface units go up into the new fleet);',
              'NOT_LAUNCHED / NOFLEET means the original does not launch surface units: Assemble_Fleet keeps only units',
              'already in orbit, and the port must change. D shows how a mixed list (ship in orbit + infantry on the',
              'surface) behaves. BEFORE parent = what holds a fresh unit (planet or fleet).']
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return verdicts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_a1h_mod.ted_owners.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--report-only')
    a = ap.parse_args()
    if a.report_only:
        v = report(a.report_only, os.path.join(os.path.dirname(os.path.abspath(a.report_only)), 'report.txt'))
        say(open(os.path.join(os.path.dirname(os.path.abspath(a.report_only)), 'report.txt')).read())
        return 0 if len(v) == len(build_a1h_mod.CASES) else 10
    import a1mem  # noqa: E402 - Windows-only ctypes; not needed for --report-only
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('A1H_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('A1H_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    mod = os.path.join(corr, 'Mods', build_a1h_mod.MOD_NAME)
    records_log = os.path.join(run_dir, 'records.log')
    info = {'game': a.game, 'mod': mod, 'started': time.time(), 'scans': 0}
    code = 0
    set_state(run_dir, state='installing', run_dir=run_dir)
    try:
        a1h_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))  # a stale folder from a crashed run
        build_a1h_mod.build(a.game, mod)
        say('\n' + '=' * 110 + '\n Mod installed (Mods\\%s). Full notes: tools\\probe_mod\\a1h_assemble_fleet\\README.md'
            % build_a1h_mod.MOD_NAME + STEPS + '=' * 110)
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
        verdicts = report(records_log, os.path.join(run_dir, 'report.txt'))
        info['verdicts'] = verdicts
        if len(verdicts) != len(build_a1h_mod.CASES):
            code = 10
            say('NOT EVERY CASE HAS A VERDICT (%d records): see records.log; did "A1H PROBE: done" show?' % len(seen))
        say(open(os.path.join(run_dir, 'report.txt')).read())
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except Exception as e:  # noqa: BLE001 - reported, cleanup still runs
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        rep = a1h_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('A1H_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
