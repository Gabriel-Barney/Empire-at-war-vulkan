#!/usr/bin/env python3
"""cu9h_run.py - the cu9h bombing-run probe (roadmap group cu9h t02; docs/re/HUD_GAPS.md sec. 9.9): build + install
Mods\\CU9HProbe, (launch FoC with it,) watch the game while it runs, write the report when it exits, clean up (always,
also after an error or Ctrl+C).

  cu9h_run.py [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--dry-run] [--target tatooine|jabiim]
              [--interval-sec 60] [--no-debugger] [--ground-budget 240] [--wait-start-sec 900] [--poll-sec 0.005]
              [--report-only RUN_DIR]

While the game runs:
- a read-only reader (a1mem) polls every --poll-sec the bombing-run state (cu9h_mem.snapshot): the targeting flag,
  the blob and its colour, the ground point, the cursor, the local player's record, the button's disabled bit and
  dial. reader.log gets one line per change and 10 lines a second while targeting; live.txt is rewritten 4 times a
  second with a one-line summary for the driver (phase, button, dial, ready_in, targeting, blob colour);
- once the galaxy (or a campaign land battle) is up, the watchpoint debugger cu9h_dbg.py is attached (dbg.log). It
  arms only in a campaign land battle. It is stopped with a stop file, never killed.
The driver marks each screenshot / click with mark_cu9h.cmd <label> (marks.log).
Output: tools\\probe_mod\\cu9h_bombing\\out\\<yyyymmdd-hhmmss>\\ (reader.log, live.txt, dbg.log, marks.log, report.txt,
config.json, run.json, run_status.txt, cleanup.log, launch_game.cmd); out\\latest.txt names the newest.
Exit codes: 0 ok (a battle was seen); 10 no battle seen; 20 preflight (game missing or already running); 30
interrupted; 31 error.
"""
import argparse
import datetime
import json
import os
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
from probe_run import say, running, set_state  # noqa: E402
import build_cu9h_mod  # noqa: E402
import cu9h_cleanup  # noqa: E402
import cu9h_mem as M  # noqa: E402
import cu9h_report  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
MODPATH = ['MODPATH=Mods\\' + build_cu9h_mod.MOD_NAME]
CHANGE_KEYS = ('phase', 'mt', 'stack', 'mode', 'hud_mode', 'flag', 'blob', 'blob_col', 'local_id', 'mgr_en',
               'rec_count', 'recs_enabled', 'rec_en', 'rec_av', 'rec_inp', 'rec_ready', 'rec_int', 'rec_last',
               'rec_bombers', 'locked', 'ready', 'comp_disabled', 'comp_state', 'comp_dial')

STEPS = r'''
 cu9h t02 BOMBING-RUN probe (full steps: tools\probe_mod\cu9h_bombing\README.md). Never save, never auto-resolve.
 1) Main menu > Single Player > Galactic Conquest > "CU9H PROBE: BOMBING RUN" > REBEL > Start. Click nothing on the
    galaxy: after 3 s the story flies your fleet (2 Y-Wing squadrons + ground units) to %(planet)s and invades.
 2) Begin on the battle bar (if one appears), Begin on the load screen. The bombing-run button (top button of the
    column between the minimap and the command bar) is disabled with a sweeping dial for %(sec)d s, then ready.
 3) Mark every screenshot / click: tools\probe_mod\cu9h_bombing\mark_cu9h.cmd <label> (q1_..., q2_..., q3_...).
    Q1 zoom the button during the cooldown and when ready; Q3 click it during the cooldown; Q2 click it when ready
    and hover a fogged and an open point; then fire a run on an open point and zoom the button again.
 4) Esc > Quit Game > Yes, Exit, quit the game. The run writes out\<run>\report.txt and cleans up.
'''


class Reader(threading.Thread):
    def __init__(self, proc, run_dir, poll):
        super().__init__(daemon=True)
        self.p, self.poll = proc, poll
        self.f = open(os.path.join(run_dir, 'reader.log'), 'w', buffering=1)
        self.live = os.path.join(run_dir, 'live.txt')
        self.stop = False
        self.last_key = None
        self.last_t = 0.0
        self.last_live = 0.0
        self.samples = 0
        self.latest = None
        self.max_phase = 'NO_GAME'

    def run(self):
        while not self.stop and self.p.alive():
            try:
                s = M.snapshot(self.p.read, self.p.base)
            except Exception as e:  # noqa: BLE001 - a torn read must not end the reader
                self.f.write('%.4f ERR %s\n' % (time.time(), str(e).replace('\n', ' ')))
                time.sleep(0.05)
                continue
            self.latest = s
            now = time.time()
            key = tuple(s.get(k) for k in CHANGE_KEYS)
            if key != self.last_key:
                self.f.write('%.4f S %s\n' % (now, M.line(s)))
                self.last_key = key
                self.samples += 1
            elif s.get('flag') == 1 and now - self.last_t >= 0.1:
                self.f.write('%.4f T %s\n' % (now, M.line(s)))
                self.last_t = now
                self.samples += 1
            if s['phase'] in ('COOLDOWN', 'READY', 'TARGETING', 'RUN_IN_PROGRESS', 'LOCKED', 'BATTLE_NO_RECORD'):
                self.max_phase = 'BATTLE'
            if now - self.last_live >= 0.25:
                self.last_live = now
                try:
                    with open(self.live + '.tmp', 'w') as lf:
                        lf.write('%s %s\n%s\n' % (time.strftime('%H:%M:%S'), M.summary(s), M.line(s)))
                    os.replace(self.live + '.tmp', self.live)
                except OSError:
                    pass
            time.sleep(self.poll)
        self.f.close()


def launcher_cmd(run_dir, corr):
    path = os.path.join(run_dir, 'launch_game.cmd')
    with open(path, 'w', newline='\r\n') as f:
        f.write('@echo off\nrem cu9h probe: start FoC windowed with the probe mod (Start-Process explorer.exe <this file>)\n')
        f.write('cd /d "%s"\nstart "" StarWarsG.exe %s\n' % (corr, ' '.join(WINDOWED + MODPATH)))
    return path


def start_debugger(pid, run_dir, budget):
    stop = os.path.join(run_dir, 'dbg.stop')
    cmd = [sys.executable, '-I', os.path.join(HERE, 'cu9h_dbg.py'), '--pid', str(pid), '--out',
           os.path.join(run_dir, 'dbg.log'), '--stop-file', stop, '--ground-budget', str(budget)]
    out = open(os.path.join(run_dir, 'dbg_console.txt'), 'w')
    return subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT), stop


def stop_debugger(dbg, stop):
    if dbg is None or dbg.poll() is not None:
        return
    try:
        open(stop, 'w').write('stop\n')
    except OSError:
        pass
    try:
        dbg.wait(20)
    except subprocess.TimeoutExpired:
        say('WARNING: the debugger did not stop within 20 s; it is left running (never kill it while attached)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=build_cu9h_mod.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true', help='launch with WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--target', choices=sorted(build_cu9h_mod.TARGETS), default='tatooine')
    ap.add_argument('--interval-sec', type=int, default=60)
    ap.add_argument('--no-debugger', action='store_true')
    ap.add_argument('--ground-budget', type=int, default=240)
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--poll-sec', type=float, default=0.005)
    ap.add_argument('--report-only')
    a = ap.parse_args()
    if a.report_only:
        text, n = cu9h_report.report(a.report_only)
        say(text)
        return 0 if n else 10
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    cmdline = ' '.join(['StarWarsG.exe'] + (WINDOWED if a.windowed else []) + MODPATH)
    steps = STEPS % {'planet': build_cu9h_mod.TARGETS[a.target]['planet'], 'sec': a.interval_sec or 120}
    if a.dry_run:
        os.makedirs(run_dir, exist_ok=True)
        mod = os.path.join(run_dir, 'dryrun_mod', build_cu9h_mod.MOD_NAME)
        cfg = build_cu9h_mod.build(a.game, mod, a.target, a.interval_sec)
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        say('DRY RUN: built %s (%d files); map %s' % (mod, len(cfg['files']), cfg['map_check']))
        say('\n' + '=' * 110 + steps + '=' * 110)
        say('Command line (in %s): %s' % (corr, cmdline))
        say('CU9H_RUN_DONE exit=0')
        return 0
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('CU9H_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('CU9H_RUN_DONE exit=20')
        return 20
    if not running('steam.exe'):
        say('WARNING: Steam does not seem to be running; start it first if the game refuses to launch.')
    import a1mem  # noqa: E402 - Windows-only
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    info = {'game': a.game, 'target': a.target, 'interval_sec': a.interval_sec, 'modpath': MODPATH,
            'started': time.time()}
    code = 0
    set_state(run_dir, state='installing', run_dir=run_dir)
    reader = dbg = None
    stop = None
    try:
        cu9h_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        cfg = build_cu9h_mod.build(a.game, os.path.join(corr, 'Mods', build_cu9h_mod.MOD_NAME), a.target,
                                   a.interval_sec)
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        launch = launcher_cmd(run_dir, corr)
        say('\n' + '=' * 110 + steps + '=' * 110)
        if not a.non_interactive and not a.no_launch:
            input('Press Enter to launch the game ...')
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + (WINDOWED if a.windowed else []) + MODPATH, cwd=corr)
            say('Game launched: ' + cmdline)
        else:
            say('Start the game yourself, in %s:\n  %s\n  (or: Start-Process explorer.exe "%s")' % (corr, cmdline, launch))
        set_state(run_dir, state='waiting_for_game_start', run_dir=run_dir, cmdline=cmdline, launcher=launch)
        pids = []
        while not pids:
            if time.time() - t0 > a.wait_start_sec:
                raise RuntimeError('StarWarsG.exe did not start within %d s' % a.wait_start_sec)
            time.sleep(2)
            pids = a1mem.find_pids()
        time.sleep(3)
        proc = a1mem.Proc(pids[0])
        info['identity'] = 'ok' if proc.pe_identity() else 'unknown (not FoC 1.121?)'
        info['pid'] = pids[0]
        say('Game running (pid %d, exe %s). Live state: %s' % (pids[0], info['identity'], os.path.join(run_dir, 'live.txt')))
        reader = Reader(proc, run_dir, a.poll_sec)
        reader.start()
        set_state(run_dir, state='game_running', run_dir=run_dir, debugger='waiting_for_galaxy')
        last_phase = None
        while proc.alive():
            s = reader.latest
            ph = s['phase'] if s else None
            if dbg is None and not a.no_debugger and s and (s['mt'] == 0 or (s['mt'] == 1 and (s['stack'] or 0) >= 2)):
                dbg, stop = start_debugger(pids[0], run_dir, a.ground_budget)
                info['debugger_started'] = time.time()
                say('Debugger attached (galaxy or battle up): %s' % os.path.join(run_dir, 'dbg.log'))
            if ph != last_phase:
                say('  phase %s  %s' % (ph, M.summary(s) if s else ''))
                last_phase = ph
                set_state(run_dir, state='game_running', run_dir=run_dir, phase=ph,
                          debugger='off' if a.no_debugger else ('attached' if dbg else 'waiting_for_galaxy'))
            time.sleep(0.5)
        reader.stop = True
        reader.join(5)
        proc.close()
        stop_debugger(dbg, stop)
        set_state(run_dir, state='collecting', run_dir=run_dir)
        text, n = cu9h_report.report(run_dir)
        say(text)
        if not n:
            code = 10
            say('NO BATTLE SEEN: did the invasion start? (README "If something goes wrong")')
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except (Exception, SystemExit) as e:  # noqa: BLE001
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        if reader is not None:
            reader.stop = True
        stop_debugger(dbg, stop)
        rep = cu9h_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('CU9H_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
