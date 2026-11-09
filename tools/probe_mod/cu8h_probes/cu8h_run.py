#!/usr/bin/env python3
"""cu8h_run.py - one cu8h probe session: build + install Mods\\CU8HProbe, (launch FoC with it,) watch the game while it
runs, write the report when it exits, clean up (always, also after an error or Ctrl+C).

  cu8h_run.py --session galendor|hum|luke|kashyyyk|jetpack [--game DIR] [--non-interactive] [--no-launch]
              [--windowed] [--dry-run] [--wait-start-sec N] [--scan-sec S] [--poll-sec 0.002] [--report-only RUN_DIR]

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
                      hum / luke (cu8k t05, CU8F sec. 2.7 item 6): when SAP +0x2e8 names a thrown saber, the watcher
                      finds that object and logs per change (every frame in flight): its position (+0x78), flags
                      (+0x3a0), projectile pack (obj +0xe8) target +0x08 / travelled +0x5c / damage +0x64 / max travel
                      +0x6c, the thrower's position and the 3D distance; also SAP +0x2e4 (target id) and +0x2d8 (aim
                      point) on the hero lines, and the target object's position. The report derives the target radius
                      R = max travel - Projectile_Max_Flight_Distance (250) - |aim z - spawn z| (CU8F sec. 2.7 step 3).
  --session kashyyyk  cu8k t05 (MOVEMENT sec. 4.7.6 items 1 and 3): campaign "CU8K PROBE: KASHYYYK SPACE". Runs two
                      cu8h_dbg.py traces one after the other, armed by hand with out\\arm (stop: out\\stop): 1 the
                      Kashyyyk space battle load (stops at mode frame 240), 2 one group move of the fleet.
  --session jetpack   cu8k t05 (MOVEMENT sec. 4.7.6 item 4): land skirmish Dagobah, three Dark Troopers jetpack; the
                      Lua records C8G|DBGARM| / C8G|DBGSTOP| arm and stop the cu8h_dbg.py trace (out\\arm, out\\stop).
  Traces: out\\<run>\\trace<k>-<label>\\ (trace.log, report.txt, sites.txt); out\\trace_status.txt says which trace
  waits for what.
Output: tools\\probe_mod\\cu8h_probes\\out\\<yyyymmdd-hhmmss>-<session>\\ (watch.log, records.log, report.txt, config.json,
run.json, cleanup.log); out\\latest.txt names the newest. Exit codes: 0 ok; 10 nothing recorded; 20 preflight; 30
interrupted; 31 error. The game's memory is only read.
"""
import argparse
import collections
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

PY = sys.executable
ARM_FILE = os.path.join(HERE, 'out', 'arm')
STOP_FILE = os.path.join(HERE, 'out', 'stop')
STATUS_FILE = os.path.join(HERE, 'out', 'trace_status.txt')
DEFAULT_EXE = os.path.join(build_cu8h_mod.DEFAULT_GAME, 'corruption', 'StarWarsG.exe')

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
SAP_TARGET_ID = 0x2E4          # the throw target's id (CU8F sec. 2.7 step 1)
SAP_AIM = 0x2D8                # the target point at activation (x, y, z floats)
SABER_NONE = 0x3FFFFF
OBJ_ID = 0x50
OBJ_POS = 0x78
OBJ_FLAGS = 0x3A0              # & 0x42: dead / removing
OBJ_PROJ_PACK = 0xE8           # projectile data pack pointer (TACTICAL_COMBAT step 11)
PACK_TARGET, PACK_TRAVELLED, PACK_DAMAGE, PACK_MAX_TRAVEL = 0x08, 0x5C, 0x64, 0x6C
SABER_MFD = 250.0              # Lukes_Light_Saber Projectile_Max_Flight_Distance
CATCH_DIST = 15.0              # Luke's Saber_Throw Catch_Dist

# cu8h_dbg.py traces per session: label, stop at mode frame N of a battle that was not current when armed (0 = never),
# stop_after_frames = stop N mode frames after arming in the mode that was current then (0 = never),
# arm = 'file' (out\arm by hand) or 'lua' (the probe's DBGARM / DBGSTOP records), cu8h_sites.py tables.
TRACES = {
    'kashyyyk': [
        {'label': 'kashyyyk-load', 'arm': 'file', 'stop_mode_frame': 240, 'tables': ['0-240'],
         'wait': 'create out\\arm right before you click Begin on the battle bar (stops by itself at mode frame 240)'},
        {'label': 'kashyyyk-move', 'arm': 'file', 'stop_mode_frame': 0, 'stop_after_frames': 600, 'tables': ['0-100000'],
         'wait': 'select the fleet (Calamari_Cruiser, Nebulon_B_Frigate, 2 X-wing squadrons), create out\\arm, '
                 'right-click ONE move order (stops by itself 600 mode frames = 20 s of game time later)'},
    ],
    'jetpack': [
        {'label': 'jetpack', 'arm': 'lua', 'stop_mode_frame': 0, 'stop_after_frames': 1500, 'tables': ['0-100000'],
         'wait': 'nothing to do: the probe Lua arms (DBGARM) and stops (DBGSTOP) the trace'},
    ],
}

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
    'kashyyyk': r'''
 cu8k t05 KASHYYYK session (MOVEMENT 4.7.6 items 1 and 3; two debugger traces, armed by hand).
 1) Single Player > Galactic Conquest > "CU8K PROBE: KASHYYYK SPACE" > REBEL > Start.
 2) Drag the fleet at Corellia to KASHYYYK. When the battle bar (Begin / Auto-Resolve) appears: create out\arm,
    then click Begin, then Begin on the load screen. Trace 1 stops by itself at mode frame 240 (8 s of battle).
 3) Wait until out\trace_status.txt says trace 2 waits (a few seconds after trace 1). When the fleet has arrived
    and fights: box-select the Calamari Cruiser, the Nebulon-B and both X-wing squadrons, create out\arm, right-click
    ONE move order into empty space. Trace 2 stops by itself 600 mode frames (20 s game time) later; wait until
    out\trace_status.txt says all_traces_done (parsing takes a minute).
 4) Esc > Quit Game > Yes, quit the game. Never save.
''',
    'jetpack': r'''
 cu8k t05 JETPACK session (MOVEMENT 4.7.6 item 4; about 35 s in battle, the debugger is armed by the probe).
 1) Single Player > Skirmish > LAND, map DAGOBAH. Slot 1 (you): EMPIRE, team 1. Slot 2: AI REBEL, team 2. Others
    closed. Start, Begin. Give NO orders.
 2) Three Dark Troopers appear at 10 s; "debugger arming" at 11 s; the trials run from 19 s to 26 s; wait for
    "CU8K PROBE: jetpack probe done" (about 35 s; the game runs slower while the debugger is armed).
 3) Esc > Quit Game > Yes, quit the game.
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
        self.sabers = {}      # saber id -> {'addr', 'hero', 'last'}
        self.done_sabers = set()
        self.find_tries = {}

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

    def find(self, m, oid):
        """Address and type of object `oid` in the mode's store (walk, stops at the match), or (None, None)."""
        import a1mem
        p = self.p
        gom = p.u64(m['ptr'] + a1mem.MODE_PRIMARY)
        head = p.u64(gom + a1mem.GOM_ID_HEAD) if gom else None
        size = p.u64(gom + a1mem.GOM_ID_SIZE) if gom else None
        if not head or size is None or size > 200000:
            return None, None
        node, n = p.u64(head), 0
        while node and node != head and n <= size + 16:
            nb = p.read(node, 0x20)
            if not nb:
                break
            nxt, = struct.unpack_from('<Q', nb, 0)
            val, = struct.unpack_from('<Q', nb, 0x18)
            if val and p.i32(val + OBJ_ID) == oid:
                return val, self.g.type_name(p.u64(val + a1mem.OBJ_TYPE))
            node = nxt
            n += 1
        return None, None

    def pos(self, addr):
        b = self.p.read(addr + OBJ_POS, 12)
        return struct.unpack('<fff', b) if b else None

    def saber_step(self, m, sid, d):
        """One poll of a thrown saber; logs on change; returns False once it is gone."""
        p, addr = self.p, d['addr']
        why = None
        oid = p.i32(addr + OBJ_ID)
        if oid != sid:
            why = 'id_changed'
        b = p.read(addr + OBJ_POS, 12) if not why else None
        fl = p.read(addr + OBJ_FLAGS, 1) if not why else None
        if not why and (b is None or fl is None):
            why = 'unreadable'
        if why:
            self.log('SBGONE id=%d mf=%s gf=%s why=%s' % (sid, m['frame'], m['gframe'], why))
            return False
        x, y, z = struct.unpack('<fff', b)
        pack = p.u64(addr + OBJ_PROJ_PACK)
        pk = p.read(pack, 0x70) if pack else None
        if pk:
            tgt, = struct.unpack_from('<I', pk, PACK_TARGET)
            trav, dmg, maxt = (struct.unpack_from('<f', pk, o)[0] for o in (PACK_TRAVELLED, PACK_DAMAGE, PACK_MAX_TRAVEL))
        else:
            tgt, trav, dmg, maxt = None, None, None, None
        hp = self.pos(d['hero'])
        dist = ((x - hp[0]) ** 2 + (y - hp[1]) ** 2 + (z - hp[2]) ** 2) ** 0.5 if hp else None
        key = (round(x, 3), round(y, 3), round(z, 3), fl[0], None if trav is None else round(trav, 3),
               None if maxt is None else round(maxt, 3))
        if key != d.get('last'):
            d['last'] = key
            self.log('SB id=%d mf=%s gf=%s x=%.3f y=%.3f z=%.3f flags=%x trav=%s maxt=%s dmg=%s ptgt=%s hx=%s hy=%s hz=%s '
                     'd=%s' % (sid, m['frame'], m['gframe'], x, y, z, fl[0],
                               'na' if trav is None else '%.3f' % trav, 'na' if maxt is None else '%.3f' % maxt,
                               'na' if dmg is None else '%.3f' % dmg, 'na' if tgt is None else '%x' % tgt,
                               *(('%.3f' % v) for v in (hp or (float('nan'),) * 3)),
                               'na' if dist is None else '%.3f' % dist))
        return True

    def saber_seen(self, m, hero_addr, s):
        """A new thrown-saber id on a hero: find the saber and the throw target, log them."""
        try:
            sid = int(s.get('saber', ''), 16)
        except ValueError:
            return
        if sid == SABER_NONE or sid in self.sabers or sid in self.done_sabers:
            return
        addr, tname = self.find(m, sid)
        if not addr:
            k = self.find_tries[sid] = self.find_tries.get(sid, 0) + 1
            if k >= 50:
                self.done_sabers.add(sid)
                self.log('SABER id=%d mf=%s gf=%s addr=none tries=%d' % (sid, m['frame'], m['gframe'], k))
            return
        self.sabers[sid] = {'addr': addr, 'hero': hero_addr}
        self.log('SABER id=%d type=%s addr=%x mf=%s gf=%s sap_tgt=%s aim=%s' % (sid, tname, addr, m['frame'], m['gframe'],
                                                                             s.get('sap_tgt'), s.get('aim')))
        tid = num(s.get('sap_tgt'))
        if tid is not None and tid >= 0:
            taddr, ttype = self.find(m, tid)
            tp = self.pos(taddr) if taddr else None
            self.log('TARGET id=%d type=%s mf=%s gf=%s pos=%s' % (tid, ttype, m['frame'], m['gframe'],
                                                                 ','.join('%.3f' % v for v in tp) if tp else 'none'))

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
        ab = p.read(sap + SAP_AIM, 0x10) if sap else None
        if ab:
            ax, ay, az, tid = struct.unpack('<fffi', ab)
            s['aim'] = '%.3f,%.3f,%.3f' % (ax, ay, az)
            s['sap_tgt'] = tid
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
                if s is not None and s.get('saber') not in (None, 'nosap', '?', '%x' % SABER_NONE):
                    self.saber_seen(m, addr, s)
            for sid, d in list(self.sabers.items()):
                if not self.saber_step(m, sid, d):
                    del self.sabers[sid]
                    self.done_sabers.add(sid)
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
                                                    'APIERR', 'DONE', 'SAY', 'JSPAWN', 'JORDER', 'JPHASE', 'JEND',
                                                    'DBGARM', 'DBGSTOP')]
    L.append('probe records: %d (HP / JST polls not listed)' % len(recs))
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
    fields = ('b3b9', 'drawn', 'start', 'end', 'eng', 'bone', 'b3b4', 'b3b5', 'b3b6', 'saber', 'beh_index', 'holster',
              'sap_tgt', 'aim')
    last = {}
    per = {}
    for e in ev:
        if e['kind'] in ('HERO', 'MODE', 'GONE', 'SABER', 'TARGET', 'SBGONE'):
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
    L.extend(saber_report(ev, per))
    L.extend(trace_summaries(run_dir))
    text = '\n'.join(L) + '\n'
    with open(os.path.join(run_dir, 'report.txt'), 'w') as f:
        f.write(text)
    return text, len(ev) + len(recs) + len(trace_dirs(run_dir))


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def saber_report(ev, per):
    """CU8F sec. 2.7 item 6: per thrown saber, its flight frame by frame, the death / end frames and the target radius."""
    L = ['', '== thrown sabers (CU8F_T01_T02_T07_RE sec. 2.7 item 6; frames = mode frames; reads are not frame-locked, '
             'a row can mix two frames) ==']
    sabers = {}
    last_sid = None
    for e in ev:
        if e['kind'] == 'SABER' and e.get('addr') != 'none':
            sabers.setdefault(e['id'], {'saber': e, 'rows': [], 'target': None, 'gone': None})
            last_sid = e['id']
        elif e['kind'] == 'TARGET' and last_sid in sabers and sabers[last_sid]['target'] is None:
            sabers[last_sid]['target'] = e
        elif e['kind'] == 'SB' and e['id'] in sabers:
            sabers[e['id']]['rows'].append(e)
        elif e['kind'] == 'SBGONE' and e['id'] in sabers:
            sabers[e['id']]['gone'] = e
    if not sabers:
        L.append('  none seen')
        return L
    ends = {}     # saber id (hex, as on the ST lines) -> (set mf, cleared mf)
    for who, es in per.items():
        for a, b in zip(es, es[1:]):
            if a.get('saber') != b.get('saber'):
                if b.get('saber') not in ('3fffff', 'nosap', '?'):
                    ends.setdefault(b.get('saber'), [b.get('mf'), None])
                if a.get('saber') not in ('3fffff', 'nosap', '?') and a.get('saber') in ends:
                    ends[a.get('saber')][1] = b.get('mf')
    for sid, d in sorted(sabers.items(), key=lambda kv: int(kv[0])):
        sv = d['saber']
        rows = d['rows']
        L.append('saber id=%s (%x) type=%s first seen mf=%s; SAP target id=%s, aim point (SAP +0x2d8) %s' % (
            sid, int(sid), sv.get('type'), sv.get('mf'), sv.get('sap_tgt'), sv.get('aim')))
        if d['target']:
            t = d['target']
            L.append('  target id=%s type=%s at mf=%s pos=%s' % (t.get('id'), t.get('type'), t.get('mf'), t.get('pos')))
        if not rows:
            L.append('  no SB rows (saber object not read)')
            continue
        first = rows[0]
        x0, y0, z0 = (fnum(first.get(k)) for k in ('x', 'y', 'z'))
        maxt = next((fnum(r.get('maxt')) for r in rows if fnum(r.get('maxt')) is not None), None)
        aim = [fnum(v) for v in (sv.get('aim') or '').split(',')] if sv.get('aim') else []
        L.append('  spawn (first row) mf=%s pos=%.3f,%.3f,%.3f thrower %s,%s,%s d=%s; max travel +0x6c=%s; damage=%s' % (
            first.get('mf'), x0, y0, z0, first.get('hx'), first.get('hy'), first.get('hz'), first.get('d'),
            maxt, first.get('dmg')))
        if maxt is not None and len(aim) == 3 and aim[2] is not None and z0 is not None:
            dz = abs(aim[2] - z0)
            L.append('  target radius R = max travel - %.0f - |aim z - spawn z| = %.3f - %.0f - %.3f = %.3f (includes the '
                     'container radius when the target is a team member)' % (SABER_MFD, maxt, SABER_MFD, dz,
                                                                           maxt - SABER_MFD - dz))
        L.append('  per frame (last row of each mode frame): mf travelled d(thrower) pos flags')
        byf = collections.OrderedDict()
        for r in rows:
            byf[r.get('mf')] = r
        dmin, catch_mf, alive_last, dead_mf, far = None, None, None, None, False
        for mf, r in byf.items():
            dd = fnum(r.get('d'))
            fl = int(r.get('flags', '0'), 16)
            if fl & 0x42:
                dead_mf = dead_mf or mf
            else:
                alive_last = mf
                if dd is not None and dd >= CATCH_DIST:
                    far = True
                elif dd is not None and far and catch_mf is None:
                    catch_mf = mf      # back within Catch_Dist after it had left (it spawns at the thrower's hand)
                if dd is not None and far and (dmin is None or dd < dmin[0]):
                    dmin = (dd, mf)
            L.append('    mf=%s trav=%s d=%s pos=%s,%s,%s flags=%s' % (mf, r.get('trav'), r.get('d'), r.get('x'),
                                                                   r.get('y'), r.get('z'), r.get('flags')))
        gone = d['gone']
        gone_mf = gone.get('mf') if gone else None
        set_mf, end_mf = ends.get('%x' % int(sid), [None, None])
        L.append('  SAP +0x2e8 set mf=%s, cleared (ability end) mf=%s; saber last alive row mf=%s, flagged dead mf=%s, '
                 'object gone mf=%s (%s)' % (set_mf, end_mf, alive_last, dead_mf, gone_mf,
                                            gone.get('why') if gone else '-'))

        def diff(x, y):
            return '?' if num(x) is None or num(y) is None else str(num(x) - num(y))
        if catch_mf:
            verdict = 'CATCH: end - catch row = %s frames (11 expected)' % diff(end_mf, catch_mf)
        else:
            verdict = ('no catch row, the saber died on its way: end - flagged dead = %s, end - gone = %s (the epsilon '
                       'of sec. 2.7 step 4)' % (diff(end_mf, dead_mf), diff(end_mf, gone_mf)))
        L.append('  closest to the thrower after leaving Catch_Dist %.0f: %s; first row back within it: mf=%s -> %s' % (
            CATCH_DIST, ('%.3f at mf=%s' % dmin) if dmin else '-', catch_mf, verdict))
    return L


def trace_dirs(run_dir):
    if not os.path.isdir(run_dir):
        return []
    return sorted(os.path.join(run_dir, d) for d in os.listdir(run_dir)
                  if d.startswith('trace') and os.path.isdir(os.path.join(run_dir, d)))


def trace_summaries(run_dir):
    """The cu8h_dbg.py traces of a kashyyyk / jetpack session: windows and formation-ctor totals from sites.txt."""
    L = []
    for d in trace_dirs(run_dir):
        L += ['', '== trace %s ==' % os.path.basename(d)]
        log = os.path.join(d, 'trace.log')
        if os.path.exists(log):
            for line in open(log):
                if line.startswith(('ATTACH', 'ARM', 'DISARM', 'STOP', 'DETACH', 'EXIT', 'NO_GAME')):
                    L.append('  ' + line.rstrip())
        sp = os.path.join(d, 'sites.txt')
        if not os.path.exists(sp):
            L.append('  no sites.txt')
            continue
        keep = False
        for line in open(sp):
            line = line.rstrip()
            if line.startswith('== 1.') or line.startswith('== 3.') or line.startswith('== 5.'):
                keep = True
            elif line.startswith('== '):
                keep = False
            if keep and line and not line.startswith('  n'):
                L.append(line)
        L.append('  (full: %s, %s)' % (sp, os.path.join(d, 'report.txt')))
    return L


class TraceRunner(threading.Thread):
    """Runs the session's cu8h_dbg.py traces one after the other against the game (pid), then their reports."""

    def __init__(self, pid, run_dir, segments, exe):
        super().__init__(daemon=True)
        self.pid, self.run_dir, self.segments, self.exe = pid, run_dir, segments, exe
        self.proc = None
        self.current = None
        self.finished = False

    def status(self, **kv):
        with open(STATUS_FILE, 'w') as f:
            for k, v in kv.items():
                f.write('%s=%s\n' % (k, v))
            f.write('time=%s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))

    def run(self):
        for i, seg in enumerate(self.segments):
            for f in (ARM_FILE, STOP_FILE):
                if os.path.exists(f):
                    os.remove(f)
            d = os.path.join(self.run_dir, 'trace%d-%s' % (i + 1, seg['label']))
            os.makedirs(d, exist_ok=True)
            log = os.path.join(d, 'trace.log')
            self.current = seg
            self.status(trace='%d/%d' % (i + 1, len(self.segments)), label=seg['label'], state='waiting_for_arm',
                        arm=ARM_FILE, stop=STOP_FILE, todo=seg['wait'], run_dir=d)
            say('TRACE %d/%d %s: %s' % (i + 1, len(self.segments), seg['label'], seg['wait']))
            cmd = [PY, '-I', os.path.join(HERE, 'cu8h_dbg.py'), '--out', log, '--pid', str(self.pid), '--arm-file',
                   ARM_FILE, '--stop-file', STOP_FILE, '--stop-mode-frame', str(seg['stop_mode_frame']),
                   '--stop-after-frames', str(seg.get('stop_after_frames', 0)),
                   '--max-hits', str(seg.get('max_hits', 1500000))]
            with open(os.path.join(d, 'dbg_console.log'), 'w') as con:
                self.proc = subprocess.Popen(cmd, stdout=con, stderr=subprocess.STDOUT)
                state = 'waiting_for_arm'
                while self.proc.poll() is None:
                    # trace_status.txt follows the trace: armed once the debugger logged ARM, with the hit count
                    if os.path.exists(STOP_FILE):
                        new = 'stopping'
                    elif os.path.exists(ARM_FILE):
                        new = 'armed'
                    else:
                        new = state
                    size = os.path.getsize(log) if os.path.exists(log) else 0
                    if new != state or new == 'armed':
                        state = new
                        self.status(trace='%d/%d' % (i + 1, len(self.segments)), label=seg['label'], state=state,
                                    trace_bytes=size, arm=ARM_FILE, stop=STOP_FILE, todo=seg['wait'], run_dir=d)
                    time.sleep(1.0)
                rc = self.proc.returncode
            self.status(trace='%d/%d' % (i + 1, len(self.segments)), label=seg['label'], state='parsing', rc=rc,
                        run_dir=d)
            subprocess.call([PY, '-I', os.path.join(HERE, 'cu8h_dbg_parse.py'), log, '--exe', self.exe, '--out',
                             os.path.join(d, 'report.txt')], stdout=subprocess.DEVNULL)
            cmd = [PY, '-I', os.path.join(HERE, 'cu8h_sites.py'), log, '--exe', self.exe, '--quiet', '--out',
                   os.path.join(d, 'sites.txt')]
            for t in seg.get('tables', []):
                cmd += ['--table', t]
            subprocess.call(cmd, stdout=subprocess.DEVNULL)
            ended = any(line.startswith(('EXIT', 'NO_GAME')) for line in open(log)) if os.path.exists(log) else True
            say('TRACE %d/%d %s done (exit %s): %s' % (i + 1, len(self.segments), seg['label'], rc, d))
            if ended:
                break
        for f in (ARM_FILE, STOP_FILE):
            if os.path.exists(f):
                os.remove(f)
        self.status(state='all_traces_done', run_dir=self.run_dir)
        self.finished = True

    def touch(self, path):
        if self.current is not None and not self.finished and not os.path.exists(path):
            open(path, 'w').close()


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
    tracer = None
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
        if a.session in TRACES:
            tracer = TraceRunner(pids[0], run_dir, TRACES[a.session], os.path.join(corr, 'StarWarsG.exe'))
            tracer.start()
        lua_armed = lua_stopped = False
        scan = a.scan_sec if a.session != 'jetpack' else min(a.scan_sec, 1.0)
        with open(os.path.join(run_dir, 'records.log'), 'w') as log:
            while proc.alive():
                if a.session in ('hum', 'luke', 'jetpack'):
                    new, st = scan_once(proc, NEEDLE, RECORD_RE, seen, log)
                    info['scans'] += 1
                    if new:
                        say('  +%d records (%d total)%s' % (new, len(seen), '; watch lines %d' % watcher.lines
                                                            if watcher else ''))
                    if tracer and not lua_armed and any('|DBGARM|' in r for r in seen):
                        lua_armed = True
                        tracer.touch(ARM_FILE)
                        say('  DBGARM record: arm file created (the debugger arms within a second)')
                    if tracer and not lua_stopped and any('|DBGSTOP|' in r for r in seen):
                        lua_stopped = True
                        tracer.touch(STOP_FILE)
                        say('  DBGSTOP record: debugger stopping')
                time.sleep(scan)
        if watcher:
            watcher.stop = True
            watcher.join(5)
        if tracer:
            tracer.join(600)
        proc.close()
        set_state(run_dir, state='collecting', run_dir=run_dir)
        if a.session in ('hum', 'luke', 'jetpack', 'kashyyyk'):
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
