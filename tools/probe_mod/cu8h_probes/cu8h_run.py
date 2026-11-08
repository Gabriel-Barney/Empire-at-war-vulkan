#!/usr/bin/env python3
"""cu8h_run.py - one cu8h probe session: build + install Mods\\CU8HProbe, (launch FoC with it,) watch the game while it
runs, write the report when it exits, clean up (always, also after an error or Ctrl+C).

  cu8h_run.py --session galendor|hum|luke [--game DIR] [--non-interactive] [--no-launch] [--windowed] [--dry-run]
              [--wait-start-sec N] [--scan-sec S] [--poll-sec 0.002] [--report-only RUN_DIR]

  --session galendor  t01 galactic half: campaign "CU8H PROBE: ENDOR GALACTIC" (Jabiim's land map is _MP_LAND_ENDOR).
                      The synced draws are traced by cu8h_dbg.py (start it yourself, --arm-file); this session only
                      installs the mod, waits for the game to exit and cleans up.
  --session hum       t02: LAND skirmish Dagobah, you EMPIRE, AI REBEL; Vader then Luke are spawned for you. While the
                      game runs, a read-only watcher polls (every --poll-sec) every DARTH_VADER / LUKE_SKYWALKER_JEDI
                      object: bytes +0x3b4..+0x3b9 (+0x3b9 = engine loops suppressed) and its HOLSTER_WEAPON behaviour
                      (type 0x4E: index byte obj +0x332 + 0x4E, array obj +0x278; vftable RVA 0x8b0f78): +0x40 bone,
                      +0x44 transition start, +0x48 transition end, +0x4c last engaged frame, +0x50 drawn. One line per
                      change, with the mode frame and the global frame. The probe Lua records ("C8G|...|END") are
                      found by a heap scan every --scan-sec. Also the thrown-saber id SAP +0x2e8 (SAP = obj +0x100;
                      0x3fffff = no saber in flight; the holster interpolation waits while one is in flight).
  --session luke      as hum without Vader: Luke, a target at about 150 units, an explicit SABER_THROW order 15
                      frames later; 35 s later a second target 120 units away and a SABER_THROW at it while drawn.
Output: tools\\probe_mod\\cu8h_probes\\out\\<yyyymmdd-hhmmss>-<session>\\ (watch.log, records.log, report.txt, config.json,
run.json, cleanup.log); out\\latest.txt names the newest. Exit codes: 0 ok; 10 nothing recorded; 20 preflight; 30
interrupted; 31 error. The game's memory is only read.
"""
import argparse
import datetime
import json
import os
import re
import struct
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
from probe_run import say, running, set_state, scan_once  # noqa: E402
import build_cu8h_mod  # noqa: E402
import cu8h_cleanup  # noqa: E402

WINDOWED = ['WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080']
MODPATH = ['MODPATH=Mods\\' + build_cu8h_mod.MOD_NAME]
NEEDLE = b'C8G|'
RECORD_RE = re.compile(rb'C8G\|[A-Z0-9_]+\|[ -~]{0,480}?\|END')
HEROES = ('DARTH_VADER', 'LUKE_SKYWALKER_JEDI')
OBJ_BEH_ARRAY = 0x278
OBJ_BEH_INDEX = 0x332 + 0x4E
HOLSTER_VFT_RVA = 0x8B0F78
OBJ_SAP = 0x100
SAP_THROWN_SABER = 0x2E8

STEPS = {
    'galendor': r'''
 cu8h t01 GALACTIC session. Start tools\probe_mod\cu8h_probes\cu8h_dbg.py --arm-file <file> first.
 1) Single Player > Galactic Conquest > "CU8H PROBE: ENDOR GALACTIC" > REBEL > Start.
 2) Move the ground units on Abregado Rae to JABIIM. Create the arm file, then Begin. Begin again on the load screen.
 3) After the debugger detached (about 4 s of battle), Esc > Quit, quit the game. Never save.
''',
    'luke': r'''
 cu8h t02 SABER-THROW session (about 1 min 30 s in battle).
 1) Single Player > Skirmish > LAND, map DAGOBAH. Slot 1 (you): EMPIRE, team 1. Slot 2: AI REBEL, team 2. Others
    closed. Start, Begin. Give NO orders.
 2) Luke appears at 10 s with a Rebel trooper, a second one at 50 s. Wait for "CU8H PROBE: lightsaber probe done"
    (about 1 min 30 s).
 3) Esc > Quit, quit the game.
''',
    'hum': r'''
 cu8h t02 LIGHTSABER session (about 3 minutes in battle).
 1) Single Player > Skirmish > LAND, map DAGOBAH. Slot 1 (you): EMPIRE, team 1. Slot 2: AI REBEL, team 2. Others
    closed. Start, Begin. Give NO orders.
 2) Vader appears at 10 s, a Rebel trooper at 30 s (Vader attacks it), it vanishes at 40 s; at about 65 s Luke
    appears with a second trooper. Wait for "CU8H PROBE: lightsaber probe done" (about 2 min 30 s).
 3) Esc > Quit, quit the game.
''',
}


class Watcher(threading.Thread):
    """Polls the hero objects of the current land mode; writes one line per change."""

    def __init__(self, proc, path, poll):
        super().__init__(daemon=True)
        import a1mem
        self.p, self.g, self.poll = proc, a1mem.Game(proc), poll
        self.f = open(path, 'w', buffering=1)
        self.stop = False
        self.heroes = {}      # obj addr -> (id, type)
        self.last = {}
        self.mode_ptr = None
        self.last_walk = 0.0
        self.lines = 0

    def log(self, line):
        self.f.write('%.4f %s\n' % (time.time(), line))
        self.lines += 1

    def walk(self, m):
        objs = self.g.objects(m['ptr']) or []
        for oid, owner, tname, _x, _y, _z, _flags, addr in objs:
            if tname in HEROES and addr not in self.heroes:
                self.heroes[addr] = (oid, tname)
                self.log('HERO id=%d type=%s addr=%x owner=%d mf=%s gf=%s' % (oid, tname, addr, owner, m['frame'],
                                                                              m['gframe']))

    def state(self, addr):
        p = self.p
        b = p.read(addr + 0x3B0, 0x10)
        if not b:
            return None
        ib = p.read(addr + OBJ_BEH_INDEX, 1)
        beh = vft = None
        bs = None
        if ib and ib[0] != 0xFF:
            arr = p.u64(addr + OBJ_BEH_ARRAY)
            beh = p.u64(arr + 8 * ib[0]) if arr else None
            vft = p.u64(beh) if beh else None
            if vft is not None and vft - p.base == HOLSTER_VFT_RVA:
                bs = p.read(beh + 0x40, 0x14)
        idx = ib[0] if ib else None
        s = {'b3b4': b[4], 'b3b5': b[5], 'b3b6': b[6], 'b3b7': b[7], 'b3b8': b[8], 'b3b9': b[9], 'beh_index': idx}
        sap = p.u64(addr + OBJ_SAP)
        fly = p.read(sap + SAP_THROWN_SABER, 4) if sap else None
        s['saber'] = 'nosap' if not sap else ('?' if not fly else '%x' % struct.unpack('<I', fly)[0])
        if bs:
            bone, start, end, eng = struct.unpack_from('<iiii', bs, 0)
            s.update({'bone': bone, 'start': start, 'end': end, 'eng': eng, 'drawn': bs[0x10]})
        else:
            s['holster'] = 'missing' if vft is None else 'vft_%x' % ((vft - p.base) & 0xFFFFFFFF)
        return s

    def run(self):
        while not self.stop and self.p.alive():
            m = self.g.mode()
            if not m or m['type'] != 1 or not m['ptr']:
                time.sleep(0.05)
                continue
            if m['ptr'] != self.mode_ptr:
                self.mode_ptr = m['ptr']
                self.heroes, self.last = {}, {}
                self.log('MODE ptr=%x type=%d mf=%s gf=%s' % (m['ptr'], m['type'], m['frame'], m['gframe']))
            now = time.time()
            if now - self.last_walk > 0.5:
                self.last_walk = now
                self.walk(m)
            for addr, (oid, tname) in list(self.heroes.items()):
                s = self.state(addr)
                key = None if s is None else tuple(sorted(s.items()))
                if key != self.last.get(addr):
                    self.last[addr] = key
                    if s is None:
                        self.log('GONE id=%d type=%s mf=%s gf=%s' % (oid, tname, m['frame'], m['gframe']))
                        del self.heroes[addr]
                    else:
                        self.log('ST id=%d type=%s mf=%s gf=%s %s' % (oid, tname, m['frame'], m['gframe'],
                                                                   ' '.join('%s=%s' % kv for kv in sorted(s.items()))))
            time.sleep(self.poll)
        self.f.close()


# ---- report ------------------------------------------------------------------------------------------------------------

def parse_watch(path):
    ev = []
    for line in open(path):
        parts = line.split()
        if len(parts) < 2:
            continue
        d = {'t': float(parts[0]), 'kind': parts[1]}
        for tok in parts[2:]:
            if '=' in tok:
                k, v = tok.split('=', 1)
                d[k] = v
        ev.append(d)
    return ev


def num(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def report(run_dir):
    L = []
    wpath = os.path.join(run_dir, 'watch.log')
    rpath = os.path.join(run_dir, 'records.log')
    ev = parse_watch(wpath) if os.path.exists(wpath) else []
    L.append('cu8h t02 report: %s' % run_dir)
    L.append('watch lines: %d' % len(ev))
    recs = open(rpath).read().splitlines() if os.path.exists(rpath) else []
    keep = [r for r in recs if r.split('|')[1] in ('ARM', 'SKIP', 'HSPAWN', 'HORDER', 'HPHASE', 'HEND', 'ERR',
                                                    'APIERR', 'DONE', 'SAY')]
    L.append('probe records: %d (HP polls not listed)' % len(recs))
    L.extend('  ' + r for r in sorted(keep, key=lambda r: int(re.search(r'\|n=(\d+)\|', r).group(1))
                                      if re.search(r'\|n=(\d+)\|', r) else 0))
    # attack-target changes from HP records
    hp = []
    for r in recs:
        if r.startswith('C8G|HP|'):
            f = re.search(r'\|f=(\d+)\|', r)
            body = r.split('|')[6] if len(r.split('|')) > 6 else ''
            if f:
                hp.append((int(f.group(1)), body))
    hp.sort()
    L.append('')
    L.append('== hero attack targets (Lua, mode time x 30; changes only) ==')
    prev = None
    for f, body in hp:
        if body != prev:
            L.append('  f=%d %s' % (f, body))
            prev = body
    L.append('')
    L.append('== memory watch (changes; mf = mode frame, gf = global frame) ==')
    fields = ('b3b9', 'drawn', 'start', 'end', 'eng', 'bone', 'b3b4', 'b3b5', 'b3b6', 'saber', 'beh_index', 'holster')
    last = {}
    per = {}
    for e in ev:
        if e['kind'] in ('HERO', 'MODE', 'GONE'):
            L.append('  %s %s' % (e['kind'], ' '.join('%s=%s' % (k, v) for k, v in e.items() if k not in ('t', 'kind'))))
            continue
        if e['kind'] != 'ST':
            continue
        who = e['type'] + '#' + e['id']
        prevs = last.get(who, {})
        diff = [k for k in fields if e.get(k) != prevs.get(k)]
        last[who] = e
        L.append('  %s mf=%s gf=%s %s' % (who, e['mf'], e['gf'], ' '.join('%s=%s' % (k, e.get(k)) for k in diff)))
        per.setdefault(who, []).append(e)
    # checks
    L.append('')
    L.append('== checks (CU8F_T01_T02_T07_RE sec. 2.3 / 2.5) ==')
    for who, es in per.items():
        L.append(who + ':')
        first = es[0]
        L.append('  at first sight: b3b9=%s drawn=%s bone=%s (expected 1 / 0 / >= 0: starts suppressed)' % (
            first.get('b3b9'), first.get('drawn'), first.get('bone')))
        for i in range(1, len(es)):
            a, b = es[i - 1], es[i]
            if a.get('drawn') != b.get('drawn'):
                st, en, eng = num(b.get('start')), num(b.get('end')), num(b.get('eng'))
                mf, gf = num(b.get('mf')), num(b.get('gf'))
                kind = 'DRAW' if b.get('drawn') == '1' else 'HOLSTER'
                L.append('  %s seen mf=%s gf=%s: start=%s end=%s (end-start=%s) start%%15=%s eng=%s%s b3b9=%s' % (
                    kind, mf, gf, st, en, None if st is None or en is None else en - st,
                    None if st is None else st % 15, eng,
                    '' if kind == 'DRAW' or st is None or eng is None else ' start-eng=%d (450 expected: first 15-frame '
                                                                          'boundary at or after eng+450)' % (st - eng),
                    b.get('b3b9')))
            if a.get('saber') != b.get('saber'):
                L.append('  saber %s->%s at mf=%s gf=%s (drawn=%s start=%s end=%s b3b9=%s)' % (
                    a.get('saber'), b.get('saber'), b.get('mf'), b.get('gf'), b.get('drawn'), b.get('start'),
                    b.get('end'), b.get('b3b9')))
            if a.get('b3b9') != b.get('b3b9'):
                L.append('  b3b9 %s->%s at mf=%s gf=%s (drawn=%s start=%s end=%s)' % (
                    a.get('b3b9'), b.get('b3b9'), b.get('mf'), b.get('gf'), b.get('drawn'), b.get('start'), b.get('end')))
    text = '\n'.join(L) + '\n'
    with open(os.path.join(run_dir, 'report.txt'), 'w') as f:
        f.write(text)
    return text, len(ev) + len(recs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--session', choices=build_cu8h_mod.SESSIONS)
    ap.add_argument('--game', default=build_cu8h_mod.DEFAULT_GAME)
    ap.add_argument('--non-interactive', action='store_true')
    ap.add_argument('--no-launch', action='store_true')
    ap.add_argument('--windowed', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--wait-start-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=4.0)
    ap.add_argument('--poll-sec', type=float, default=0.002)
    ap.add_argument('--report-only')
    a = ap.parse_args()
    if a.report_only:
        text, n = report(a.report_only)
        say(text)
        return 0 if n else 10
    if not a.session:
        ap.error('--session is required')
    corr = os.path.join(a.game, 'corruption')
    exe = os.path.join(corr, 'StarWarsG.exe')
    out_root = os.path.join(HERE, 'out')
    run_dir = os.path.join(out_root, datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + a.session)
    cmdline = ' '.join(['StarWarsG.exe'] + (WINDOWED if a.windowed else []) + MODPATH)
    if a.dry_run:
        os.makedirs(run_dir, exist_ok=True)
        mod = os.path.join(run_dir, 'dryrun_mod', build_cu8h_mod.MOD_NAME)
        cfg = build_cu8h_mod.build(a.session, a.game, mod)
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        say('DRY RUN: built %s (%d files)' % (mod, len(cfg['files'])))
        if 'site_note' in cfg:
            say('site: ' + cfg['site_note'])
        say('\n' + '=' * 110 + STEPS[a.session] + '=' * 110)
        say('Command line (in %s): %s' % (corr, cmdline))
        say('CU8H_RUN_DONE exit=0')
        return 0
    if not os.path.isfile(exe):
        say('StarWarsG.exe not found under', corr)
        say('CU8H_RUN_DONE exit=20')
        return 20
    if running('StarWarsG.exe'):
        say('The game is already running; close it first.')
        say('CU8H_RUN_DONE exit=20')
        return 20
    import a1mem  # noqa: E402 - Windows-only
    os.makedirs(run_dir, exist_ok=True)
    with open(os.path.join(out_root, 'latest.txt'), 'w') as f:
        f.write(run_dir + '\n')
    info = {'game': a.game, 'session': a.session, 'modpath': MODPATH, 'started': time.time(), 'scans': 0}
    code = 0
    set_state(run_dir, state='installing', run_dir=run_dir)
    watcher = None
    try:
        cu8h_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        cfg = build_cu8h_mod.build(a.session, a.game, os.path.join(corr, 'Mods', build_cu8h_mod.MOD_NAME))
        with open(os.path.join(run_dir, 'config.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
        info['site'] = cfg.get('site_note')
        say('\n' + '=' * 110 + STEPS[a.session] + '=' * 110)
        t0 = time.time()
        if not a.no_launch:
            subprocess.Popen([exe] + (WINDOWED if a.windowed else []) + MODPATH, cwd=corr)
            say('Game launched: ' + cmdline)
        else:
            say('Start the game yourself, in %s:\n  %s' % (corr, cmdline))
        set_state(run_dir, state='waiting_for_game_start', run_dir=run_dir, cmdline=cmdline)
        pids = []
        while not pids:
            if time.time() - t0 > a.wait_start_sec:
                raise RuntimeError('StarWarsG.exe did not start within %d s' % a.wait_start_sec)
            time.sleep(2)
            pids = a1mem.find_pids()
        time.sleep(3)
        proc = a1mem.Proc(pids[0])
        info['identity'] = 'ok' if proc.pe_identity() else 'unknown (not FoC 1.121?)'
        set_state(run_dir, state='game_running', run_dir=run_dir)
        say('Game running (pid %d, exe %s).' % (pids[0], info['identity']))
        seen = set()
        if a.session in ('hum', 'luke'):
            watcher = Watcher(proc, os.path.join(run_dir, 'watch.log'), a.poll_sec)
            watcher.start()
        with open(os.path.join(run_dir, 'records.log'), 'w') as log:
            while proc.alive():
                if a.session in ('hum', 'luke'):
                    new, st = scan_once(proc, NEEDLE, RECORD_RE, seen, log)
                    info['scans'] += 1
                    if new:
                        say('  +%d records (%d total); watch lines %d' % (new, len(seen), watcher.lines))
                time.sleep(a.scan_sec)
        if watcher:
            watcher.stop = True
            watcher.join(5)
        proc.close()
        set_state(run_dir, state='collecting', run_dir=run_dir)
        if a.session in ('hum', 'luke'):
            text, n = report(run_dir)
            say(text)
            if not n:
                code = 10
    except KeyboardInterrupt:
        say('interrupted')
        code = 30
    except (Exception, SystemExit) as e:  # noqa: BLE001
        say('ERROR:', e)
        info['error'] = str(e)
        code = 31
    finally:
        rep = cu8h_cleanup.cleanup(a.game, os.path.join(run_dir, 'cleanup.log'))
        info['cleanup'] = rep
        info['ended'] = time.time()
        info['exit_code'] = code
        with open(os.path.join(run_dir, 'run.json'), 'w') as f:
            json.dump(info, f, indent=1)
        set_state(run_dir, state='done', exit_code=code, cleanup='OK' if rep['ok'] else 'INCOMPLETE', run_dir=run_dir)
        say('cleanup:', 'OK' if rep['ok'] else 'LEFT SOMETHING, see cleanup.log', '| run folder:', run_dir)
        say('CU8H_RUN_DONE exit=%d' % code)
    return code


if __name__ == '__main__':
    sys.exit(main())
