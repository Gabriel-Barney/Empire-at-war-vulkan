#!/usr/bin/env python3
"""ccp_reader.py - read-only recorder for the ccpgu t02 Capture Control Points probe (FoC Steam x64 1.121).

  ccp_reader.py --out LOG [--config ccp_config.json] [--status JSON] [--wait-process-sec 900] [--timeout-sec 14400]
                [--scan-sec 5] [--poll-sec 0.002] [--allow-unknown-exe]
  ccp_reader.py --selftest            offline checks against a fake memory image

Process access, exe check, object walk and heap scan come from tools/probe_mod/a1_tactical/a1mem.py (imported, not
copied): OpenProcess(VM_READ | QUERY_INFORMATION | SYNCHRONIZE) + ReadProcessMemory only; the game is never written.

For every land mode (GMM current mode, a1mem layout) it records:
  * the players (id, faction, human; re-read every 30 frames and logged again when they change, because at mode
    frame 0 the list is still the pre-remap one without a human: run 20261108-214851; the Lua ARM record's human_id
    is the fallback) and every REINFORCEMENT_POINT* object of the mode's store with its owner at the
    first walk (POINT lines: part 2 of the question, the start owner);
  * on every poll (several per 30 Hz frame), read between two reads of the mode frame (+0x10; a poll that sees the
    frame change is repeated): the owner (+0x58) of each point, the mode's control-point domination state
    (SKIRMISH_SETUP 4.2: +0x2b0 armed, +0x2b4 timer frames, +0x2b8 player) and the victory monitor (mode +0x28,
    BATTLE_TRANSITIONS 3.1a: +0x00 mode back-pointer = validity check, +0x20 declared, +0x24 countdown, +0x28 declared
    record type / +0x2c player / +0x30 countdown, +0x5c hold). One ST line per change, so while the timer or the
    countdown runs there is one line per frame;
  * GAP (the frame advanced by more than 1 between two polls), STALL / RESUME (no frame advance for 1.5 s: the battle
    end screen or a pause), MODE_END;
  * the probe's Lua records ("CCP1|...|END" strings) by a heap scan every --scan-sec (thread) and at mode end.
The status JSON (--status) carries a 'phase' and a 'hint' for the person or agent driving the game.

Log lines (key=value): CCPREADER 1, GAME, MODE n= ptr= type= frame=, PL n= frame= why= id= index= faction= human=,
  HUMAN n= frame= id= faction= src=, WALK n= frame=
  nodes= points= sec=, POINT n= frame= obj= addr= type= relevant= owner= owner_faction= x= y=, CPV n= frame= raw=,
  MON n= frame= ptr= ok= back=, ST n= frame= armed= timer= cpp= decl= cd= dtype= dplayer= dcd= hold= owners= b1=
  (armed = the byte at +0x2b0, b1 = +0x2b1; the mode frame counter is written after the sim state in a tick, so a
  second change logged with the same frame belongs to the next tick: ccp_parse.py corrects it),
  GAP n= frame= prev=, STALL n= frame= sec=, RESUME n= frame=, SCAN ..., LUA n= frame= addr= rec=, SRC n= frame=,
  MODE_END n= last_frame= why=, EXIT.
"""
import argparse
import json
import os
import re
import struct
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
A1 = os.path.join(os.path.dirname(HERE), 'a1_tactical')
sys.path.insert(0, HERE)
sys.path.insert(0, A1)
import a1mem  # noqa: E402
from a1mem import Log, kv, wait_for_game  # noqa: E402

REC_NEEDLE = b'CCP1|'
SRC_MARKERS = (b'CCPPROBE_SRC_MARK_V1',)
RECORD_RE = re.compile(rb'CCP1\|([A-Z]+)\|([ -~]{0,385}?)\|END')

MODE_MONITOR = 0x28      # VictoryMonitorClass* (BATTLE_TRANSITIONS 3.1a)
MODE_CP = 0x2B0          # +0x2b0 armed (u8), +0x2b4 timer, +0x2b8 player (SKIRMISH_SETUP 4.2); +0x2b1 is another
#                          byte (1 in the retail run 20261108-214851: an i32 read gave 257 / 256)
PL_REREAD_FRAMES = 30    # re-read the player list every 30 mode frames: at mode frame 0 retail still holds the
#                          pre-remap list (faction order, no human flag; run 20261108-214851)
ARM_HUMAN_RE = re.compile(r'\|ARM\|.*\|human_id=(-?\d+)\|')
MODE_READ = 0x2C0
MON_BACK = 0x00
MON_DECL = 0x20
MON_CD = 0x24
MON_REC = 0x28           # declared record copy: +0 type, +4 player, +8 countdown
MON_HOLD = 0x5C
MON_READ = 0x60
POINT_PREFIX = 'REINFORCEMENT_POINT'
RELEVANT = ('REINFORCEMENT_POINT', 'REINFORCEMENT_POINT_PLUS1_CAP', 'REINFORCEMENT_POINT_PLUS2_CAP',
            'REINFORCEMENT_POINT_PLUS3_CAP', 'REINFORCEMENT_POINT_PLUS4_CAP', 'REINFORCEMENT_POINT_PLUS5_CAP',
            'REINFORCEMENT_POINT_PLUS10_CAP')
STALL_SEC = 1.5


def lua_records(hits):
    return a1mem.lua_records(hits, RECORD_RE)


def plausible(q):
    return q is not None and 0x10000 <= q < 0x7FFF00000000 and q % 8 == 0


class Recorder:
    def __init__(self, a, proc, log, cfg, game=None):
        self.a, self.p, self.log, self.cfg = a, proc, log, cfg
        self.g = game or a1mem.Game(proc)
        self.n = 0
        self.mode_ptr = None
        self.frame = None
        self.records = set()
        self.scans = 0
        self.scan_thread = None
        self.next_scan = 0.0
        self.stats = {'modes': 0, 'scans': 0, 'records': 0, 'points': 0, 'states': 0, 'gaps': 0, 'torn': 0,
                      'phase': 'waiting_game', 'hint': 'start the game', 'arm_frame': None, 'decl_frame': None,
                      'end_frame': None, 'human_id': None, 'mon_ok': None}
        self.reset_mode()

    def reset_mode(self):
        self.points = {}          # addr -> {'id', 'type', 'relevant'}
        self.walked = False
        self.last_walk = 0.0
        self.walk_tries = 0
        self.last_state = None
        self.last_frame = None
        self.last_advance = time.time()
        self.stalled = False
        self.mon_checked = False
        self.mon_ok = False
        self.cpv_logged = False
        self.players = {}
        self.human_id = None
        self.pl_sig = None
        self.pl_frame = None
        self.lua_human = None
        self.declared = False
        self.armed_seen = False
        self.ended = False

    # ---- heap scan (thread) ----
    def _scan(self, n, frame, why):
        t = time.time()
        try:
            hits, st = self.p.scan([REC_NEEDLE] + list(SRC_MARKERS))
        except OSError as e:
            self.log('SCAN_ERROR ' + kv(n=n, frame=frame, error=str(e).replace(' ', '_')))
            return
        new = 0
        for addr, text in lua_records(hits[REC_NEEDLE]):
            if text in self.records:
                continue
            self.records.add(text)
            new += 1
            self.log('LUA ' + kv(n=n, frame=frame, addr=hex(addr), rec=text))
            m = ARM_HUMAN_RE.search(text)
            if m and n == self.n:
                self.lua_human = int(m.group(1))  # fallback when the player list shows no human (used in step)
        for m in SRC_MARKERS:
            if hits[m]:
                self.log('SRC ' + kv(n=n, frame=frame, marker=m.decode(), copies=len(hits[m])))
        self.scans += 1
        self.stats['scans'] = self.scans
        self.stats['records'] = len(self.records)
        self.log('SCAN ' + kv(n=n, frame=frame, why=why, regions=st['regions'], mb='%.0f' % (st['bytes'] / 1e6),
                              unreadable=st['unreadable'], sec='%.2f' % (time.time() - t), records=new))

    def start_scan(self, why, wait=False):
        if self.scan_thread is not None and self.scan_thread.is_alive():
            if not wait:
                return
            self.scan_thread.join()
        self.scan_thread = threading.Thread(target=self._scan, args=(self.n, self.frame, why), daemon=True)
        self.scan_thread.start()
        if wait:
            self.scan_thread.join()

    # ---- modes ----
    def new_mode(self, m):
        self.n += 1
        self.stats['modes'] = self.n
        self.mode_ptr, self.frame = m['ptr'], m['frame']
        self.reset_mode()
        self.log('MODE ' + kv(n=self.n, ptr=hex(m['ptr']), type=m['type'], game_type=m['game_type'], stack=m['stack'],
                              frame=m['frame'], gframe=m['gframe'], t='%.1f' % (time.time() - self.log.t0)))
        self.read_players(m['frame'], 'mode_start')
        if self.human_id is None:
            self.set_phase('menu_battle', 'no human player yet (background battle, or the list is not remapped yet)')

    def read_players(self, frame, why):
        """Read the player list; log PL lines when it changed (id / faction / human). The list read at mode frame 0 can
        be the pre-remap one (no human), so step() repeats this every PL_REREAD_FRAMES frames."""
        self.pl_frame = frame
        pl = self.g.players()
        rows = [(p['id'], p['index'], p['faction'] or '?', p['human'], p['neutral']) for p in (pl or {}).get('by_id', [])]
        sig = tuple(rows)
        if not rows or sig == self.pl_sig:
            return
        first = self.pl_sig is None
        self.pl_sig = sig
        self.players = {pid: fac for pid, _i, fac, _h, _n in rows}
        human = [pid for pid, _i, _f, h, _n in rows if h]
        for pid, idx, fac, h, neu in rows:
            self.log('PL ' + kv(n=self.n, frame=frame, why=why if first else 'changed', id=pid, index=idx, faction=fac,
                                human=h, neutral=neu))
        if human:
            self.set_human(human[0], 'players', frame)

    def set_human(self, pid, src, frame):
        if self.human_id == pid:
            return
        self.human_id = pid
        self.stats['human_id'] = pid
        self.log('HUMAN ' + kv(n=self.n, frame=frame, id=pid, faction=self.players.get(pid, '?'), src=src))
        if self.stats['phase'] in ('menu_battle', 'waiting_game', 'menu'):
            self.set_phase('battle', 'battle loaded; give no orders, wait')
        if self.last_state is not None:
            self.update_phase(frame, self.last_state)

    def end_mode(self, why='mode_changed'):
        self.log('MODE_END ' + kv(n=self.n, last_frame=self.frame, why=why))
        if self.declared and not self.ended:
            self.mark_end('mode_end')
        self.start_scan('mode_end', wait=True)
        self.mode_ptr = None

    def set_phase(self, phase, hint):
        self.stats['phase'] = phase
        self.stats['hint'] = hint

    def owner_faction(self, owner):
        return self.players.get(owner, '?')

    def walk(self):
        t = time.time()
        objs = self.g.objects(self.mode_ptr)
        self.walk_tries += 1
        self.last_walk = time.time()
        if objs is None:
            return
        new = 0
        for oid, owner, tname, x, y, _z, _flags, addr in objs:
            if not tname.startswith(POINT_PREFIX) or addr in self.points:
                continue
            rel = tname in RELEVANT
            self.points[addr] = {'id': oid, 'type': tname, 'relevant': rel}
            new += 1
            self.log('POINT ' + kv(n=self.n, frame=self.frame, obj=oid, addr=hex(addr), type=tname, relevant=int(rel),
                                   owner=owner, owner_faction=self.owner_faction(owner), x='%.1f' % x, y='%.1f' % y))
        self.stats['points'] = len(self.points)
        if new or not self.walked:
            self.log('WALK ' + kv(n=self.n, frame=self.frame, nodes=len(objs), points=len(self.points), new=new,
                                  sec='%.3f' % (time.time() - t)))
        self.walked = True

    def check_monitor(self, mon, frame):
        back = self.p.u64(mon + MON_BACK) if plausible(mon) else None
        self.mon_ok = plausible(mon) and back == self.mode_ptr
        self.mon_checked = True
        self.stats['mon_ok'] = self.mon_ok
        self.log('MON ' + kv(n=self.n, frame=frame, ptr=hex(mon or 0), ok=int(self.mon_ok), back=hex(back or 0)))

    def read_state(self):
        """(frame, state tuple) read between two reads of the mode frame, or None."""
        for _ in range(4):
            mb = self.p.read(self.mode_ptr, MODE_READ)
            if not mb:
                return None
            f0, = struct.unpack_from('<I', mb, a1mem.MODE_FRAME)
            mon, = struct.unpack_from('<Q', mb, MODE_MONITOR)
            armed, armed_b1 = mb[MODE_CP], mb[MODE_CP + 1]
            timer, cpp = struct.unpack_from('<ii', mb, MODE_CP + 4)
            if not self.cpv_logged:
                self.cpv_logged = True
                self.log('CPV ' + kv(n=self.n, frame=f0, raw=mb[MODE_CP:MODE_CP + 0x10].hex()))
            if not self.mon_checked:
                self.check_monitor(mon, f0)
            decl = cd = dtype = dplayer = dcd = hold = None
            if self.mon_ok:
                b = self.p.read(mon, MON_READ)
                if b:
                    decl = b[MON_DECL]
                    cd, = struct.unpack_from('<i', b, MON_CD)
                    dtype, dplayer, dcd = struct.unpack_from('<iii', b, MON_REC)
                    hold, = struct.unpack_from('<i', b, MON_HOLD)
            owners = []
            for addr, pt in self.points.items():
                o = self.p.i32(addr + a1mem.OBJ_OWNER)
                owners.append('%s:%s' % (pt['id'], 'x' if o is None else o))
            f1 = self.p.u32(self.mode_ptr + a1mem.MODE_FRAME)
            if f1 == f0:
                return f0, (armed, timer, cpp, decl, cd, dtype, dplayer, dcd, hold, ','.join(owners), armed_b1)
            self.stats['torn'] += 1
        return None

    def mark_end(self, why):
        self.ended = True
        self.stats['end_frame'] = self.frame
        self.set_phase('ended', 'battle over (%s): Esc > Quit to the main menu, then quit the game' % why)

    def poll(self):
        r = self.read_state()
        if r is None:
            return
        frame, st = r
        now = time.time()
        if self.last_frame is not None and frame != self.last_frame:
            if frame > self.last_frame + 1:
                self.stats['gaps'] += 1
                self.log('GAP ' + kv(n=self.n, frame=frame, prev=self.last_frame))
            if self.stalled:
                self.stalled = False
                self.log('RESUME ' + kv(n=self.n, frame=frame))
            self.last_advance = now
        elif self.last_frame is not None and not self.stalled and now - self.last_advance >= STALL_SEC:
            self.stalled = True
            self.log('STALL ' + kv(n=self.n, frame=frame, sec='%.1f' % (now - self.last_advance)))
            if self.declared and not self.ended:
                self.mark_end('stall')
        self.last_frame = frame
        self.frame = frame
        if st != self.last_state:
            self.last_state = st
            self.stats['states'] += 1
            armed, timer, cpp, decl, cd, dtype, dplayer, dcd, hold, owners, b1 = st
            self.log('ST ' + kv(n=self.n, frame=frame, armed=armed, timer=timer, cpp=cpp, decl=decl, cd=cd, dtype=dtype,
                                dplayer=dplayer, dcd=dcd, hold=hold, owners=owners or '-', b1=b1))
            if self.human_id is not None:
                self.update_phase(frame, st)

    def update_phase(self, frame, st):
        armed, timer, cpp, decl, cd, _dt, _dp, _dcd, _h, owners, _b1 = st
        if decl and not self.declared:
            self.declared = True
            self.stats['decl_frame'] = frame
            self.set_phase('declared', 'victory declared; wait for the end screen (~7 s)')
        if self.declared and not self.ended and cd is not None and cd <= 0:
            self.mark_end('countdown')
        if self.declared:
            return
        if armed == 1 and not self.armed_seen:
            self.armed_seen = True
            self.stats['arm_frame'] = frame
            self.set_phase('armed', 'domination timer running (30 s); give no orders')
        elif armed == 0 and self.armed_seen:
            self.armed_seen = False
            self.set_phase('disarmed', 'domination timer stopped without a declaration; keep waiting (the probe units '
                                       'retake the point)')
        elif not self.armed_seen:
            hum = [p for p in owners.split(',') if p] if owners else []
            rel = [p for p in hum if self.relevant_id(p.split(':')[0])]
            mine = [p for p in rel if p.split(':')[1] == str(self.human_id)]
            if rel and len(mine) == len(rel):
                self.set_phase('captured', 'all points are yours; timer should run')
            elif rel:
                self.set_phase('capturing', '%d of %d points yours; wait' % (len(mine), len(rel)))

    def relevant_id(self, oid):
        return any(str(p['id']) == oid and p['relevant'] for p in self.points.values())

    def step(self):
        m = self.g.mode()
        if m is None:
            return
        land = m['type'] == 1 and m['ptr']
        if self.mode_ptr is not None and m['ptr'] != self.mode_ptr:
            self.end_mode()
        if not land:
            if self.stats['phase'] not in ('ended',):
                self.set_phase('menu', 'no land battle: skirmish setup, see the steps')
            return
        if self.mode_ptr is None:
            self.new_mode(m)
        now = time.time()
        if not self.points and now - self.last_walk >= (0.5 if self.walk_tries < 60 else 5.0):
            self.walk()
        self.poll()
        f = self.frame or 0
        if self.pl_frame is None or f >= self.pl_frame + PL_REREAD_FRAMES or f < self.pl_frame:
            self.read_players(f, 'reread')
        if self.human_id is None and self.lua_human is not None:
            self.set_human(self.lua_human, 'lua_arm', f)
        if (self.frame or 0) >= 60 and now >= self.next_scan:
            self.next_scan = now + self.a.scan_sec
            self.start_scan('periodic')


# ---- selftest: a fake game image ---------------------------------------------------------------------------------

class FakeProc:
    """Memory dict for the selftest; implements the a1mem.Proc read API."""

    def __init__(self):
        self.mem = {}
        self.base = 0x140000000
        self.reads = self.failed = 0
        self.pid = 0
        self.timestamp = a1mem.FOC_TIMESTAMP
        self.size_of_image = a1mem.FOC_SIZE_OF_IMAGE

    def put(self, addr, data):
        self.mem[addr] = bytearray(data)

    def read(self, addr, n):
        self.reads += 1
        for b, buf in self.mem.items():
            if b <= addr and addr + n <= b + len(buf):
                return bytes(buf[addr - b:addr - b + n])
        self.failed += 1
        return None

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<I', b)[0] if b else None

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<i', b)[0] if b else None

    def scan(self, needles):
        hits = {n: [] for n in needles}
        for b, buf in self.mem.items():
            for nd in needles:
                i = bytes(buf).find(nd)
                while i != -1:
                    hits[nd].append((b + i, bytes(buf[i:i + 400])))
                    i = bytes(buf).find(nd, i + 1)
        return hits, {'regions': len(self.mem), 'bytes': 0, 'unreadable': 0}

    def alive(self):
        return True


class FakeGame:
    def __init__(self, proc, mode_ptr, points, players):
        self.p, self.mode_ptr, self.pts, self.pl = proc, mode_ptr, points, players

    def mode(self):
        return {'ptr': self.mode_ptr, 'type': 1, 'game_type': 4, 'stack': 1,
                'frame': self.p.u32(self.mode_ptr + a1mem.MODE_FRAME), 'gframe': 0}

    def players(self):
        return {'by_id': [{'id': i, 'index': i, 'faction': f, 'human': h, 'neutral': int(f == 'NEUTRAL')}
                          for i, f, h in self.pl], 'count': len(self.pl), 'vec': []}

    def objects(self, mode_ptr):
        return [(oid, self.p.i32(addr + a1mem.OBJ_OWNER), t, 1.0, 2.0, 0.0, 0, addr) for oid, t, addr in self.pts]


def selftest():
    fails = []

    def check(c, what):
        print(('  ok    ' if c else '  FAIL  ') + what)
        if not c:
            fails.append(what)

    rec = b'xx CCP1|OWN|sid=abc123|n=7|f=612|pt=82036|from=NEUTRAL:3|to=EMPIRE:1|END yy'
    i = rec.find(REC_NEEDLE)
    got = lua_records([(0x1000 + i, rec[i:i + 400])])
    check(len(got) == 1 and got[0][1].endswith('|END') and 'pt=82036' in got[0][1], 'record regex')
    check(lua_records([(0, b'CCP1|P|f=1|no end here')]) == [], 'unterminated record ignored')

    # fake image: mode at 0x10000000, monitor at 0x10100000, one relevant point and one fixed point
    p = FakeProc()
    mode, mon, pt1, pt2 = 0x10000000, 0x10100000, 0x10200000, 0x10300000
    mb = bytearray(MODE_READ)
    struct.pack_into('<Q', mb, MODE_MONITOR, mon)
    struct.pack_into('<iii', mb, MODE_CP, 0x100, -1, -1)  # +0x2b1 = 1 as in retail
    p.put(mode, mb)
    monb = bytearray(MON_READ)
    struct.pack_into('<Q', monb, MON_BACK, mode)
    p.put(mon, monb)
    for a, owner in ((pt1, 3), (pt2, 3)):
        ob = bytearray(0x400)
        struct.pack_into('<i', ob, a1mem.OBJ_OWNER, owner)
        p.put(a, ob)
    p.put(0x20000000, b'....CCP1|ARM|sid=x|n=2|f=3|fac=REBEL|human_id=1|END....')
    # retail: at mode frame 0 the list is the pre-remap one (faction order, no human); remapped a few frames later
    pre = [(0, 'REBEL', 0), (1, 'EMPIRE', 0), (3, 'NEUTRAL', 0)]
    post = [(0, 'REBEL', 0), (1, 'EMPIRE', 1), (3, 'NEUTRAL', 0)]
    game = FakeGame(p, mode, [(82036, 'REINFORCEMENT_POINT', pt1), (99, 'REINFORCEMENT_POINT_REBEL', pt2)], pre)
    out = os.path.join(os.environ.get('TEMP', '.'), 'ccp_reader_selftest_%d.log' % os.getpid())
    if os.path.exists(out):
        os.remove(out)
    log = Log(out)
    a = argparse.Namespace(scan_sec=1000.0)
    r = Recorder(a, p, log, {}, game)

    def set_frame(f):
        struct.pack_into('<I', p.mem[mode], a1mem.MODE_FRAME, f)

    def set_cp(armed, timer, player):
        struct.pack_into('<iii', p.mem[mode], MODE_CP, armed | 0x100, timer, player)

    def set_mon(decl, cd, dtype=0, dplayer=0):
        p.mem[mon][MON_DECL] = decl
        struct.pack_into('<i', p.mem[mon], MON_CD, cd)
        struct.pack_into('<ii', p.mem[mon], MON_REC, dtype, dplayer)

    # capture at frame 400 (timer 899 seen at the capture frame), test + declaration at 1299, countdown 0 at 1509
    for f in range(0, 1520):
        set_frame(f)
        if f == 5:
            game.pl = post
        if f == 400:
            struct.pack_into('<i', p.mem[pt1], a1mem.OBJ_OWNER, 1)
            set_cp(1, 899, 1)
        elif 400 < f < 1299:
            set_cp(1, 1299 - f, 1)
        elif f == 1299:
            set_cp(0, -1, -1)
            set_mon(1, 210, 0x15, 1)
        elif f > 1299:
            set_mon(1, max(0, 210 - (f - 1299)), 0x15, 1)
        r.step()
        r.step()  # two polls per frame: the second sees no change
    if r.scan_thread is not None:
        r.scan_thread.join()
    r.end_mode('selftest')
    log.f.close()
    text = open(out).read()
    os.remove(out)
    check('POINT n=1' in text and 'type=REINFORCEMENT_POINT relevant=1 owner=3 owner_faction=NEUTRAL' in text,
          'points listed with start owner and faction')
    check('MON n=1 frame=0 ptr=0x10100000 ok=1' in text, 'monitor validated by its back-pointer')
    st = [ln for ln in text.splitlines() if ln.startswith('ST ')]
    check(len(st) == 1 + 1 + 898 + 1 + 210, 'one ST line per change (%d)' % len(st))
    check(any('frame=400 armed=1 timer=899 cpp=1' in ln and '82036:1' in ln for ln in st), 'capture frame state')
    check(any('frame=1299 armed=0' in ln and 'decl=1 cd=210 dtype=21 dplayer=1' in ln for ln in st), 'declaration')
    check(r.stats['phase'] == 'ended' and r.stats['end_frame'] == 1509, 'phase ended at countdown 0 (%s, %s)' % (
        r.stats['phase'], r.stats['end_frame']))
    check(r.stats['arm_frame'] == 400 and r.stats['decl_frame'] == 1299, 'arm / declaration frames in the status')
    check(r.human_id == 1 and 'HUMAN n=1 frame=30 id=1 faction=EMPIRE src=players' in text,
          'human found by the player re-read after the remap (%s)' % r.human_id)
    check(text.count('PL n=1 ') == 6 and 'why=changed' in text, 'player list logged at mode start and after the change')
    check(all(' armed=1 ' in ln or ' armed=0 ' in ln for ln in st) and 'b1=1' in st[0], 'armed read as the +0x2b0 byte')
    # Lua fallback: the player list never shows a human, the ARM record's human_id does
    game.pl = pre
    r3 = Recorder(a, p, Log(out), {}, game)
    set_frame(0)
    set_cp(0, -1, -1)
    set_mon(0, 0)
    r3.step()
    r3._scan(r3.n, 0, 'selftest')
    r3.step()
    r3.log.f.close()
    t3 = open(out).read()
    os.remove(out)
    check(r3.human_id == 1 and 'src=lua_arm' in t3 and r3.stats['phase'] == 'captured', 'human from the Lua ARM record')
    check('LUA n=1' in text and 'CCP1|ARM|' in text, 'Lua record found by the heap scan')
    # a torn read: the frame changes between the two frame reads -> the poll is repeated
    calls = {'n': 0}
    orig = p.u32

    def flaky_u32(addr):
        calls['n'] += 1
        v = orig(addr)
        return v + 1 if calls['n'] == 1 else v
    p.u32 = flaky_u32
    r2 = Recorder(a, p, Log(out), {}, game)
    r2.mode_ptr = mode
    res = r2.read_state()
    r2.log.f.close()
    os.remove(out)
    check(res is not None and r2.stats['torn'] == 1, 'torn poll repeated')
    print('READER SELFTEST %s' % ('OK' if not fails else 'FAILED'))
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out')
    ap.add_argument('--config')
    ap.add_argument('--status')
    ap.add_argument('--wait-process-sec', type=float, default=900)
    ap.add_argument('--timeout-sec', type=float, default=14400)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--poll-sec', type=float, default=0.002)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.out:
        ap.error('--out is required')
    cfg = {}
    if a.config and os.path.exists(a.config):
        with open(a.config) as f:
            cfg = json.load(f)
    log = Log(a.out)
    log('CCPREADER 1 ' + kv(scan_sec=a.scan_sec, poll_sec=a.poll_sec, map=cfg.get('map'), human=cfg.get('human')))

    def status(**extra):
        if a.status:
            with open(a.status + '.tmp', 'w') as f:
                json.dump(extra, f)
            os.replace(a.status + '.tmp', a.status)

    status(state='waiting_game', phase='waiting_game', hint='start the game')
    p = wait_for_game(a, log)
    if p is None:
        log('EXIT ' + kv(reason='no_game', modes=0, scans=0, records=0))
        status(state='done', reason='no_game', phase='no_game', hint='the game did not start')
        return 2
    identity = 'ok' if p.timestamp == a1mem.FOC_TIMESTAMP else 'unknown'
    log('GAME ' + kv(pid=p.pid, base=hex(p.base), timestamp=hex(p.timestamp or 0),
                     size_of_image=hex(p.size_of_image or 0), identity=identity))
    r = Recorder(a, p, log, cfg)
    t0 = time.time()
    last_status = 0.0
    reason = 'game_exited'
    try:
        while p.alive():
            if time.time() - t0 > a.timeout_sec:
                reason = 'timeout'
                break
            r.step()
            now = time.time()
            if now - last_status > 1.0:
                last_status = now
                status(**dict(r.stats, state='running', mode=r.n, frame=r.frame, reads=p.reads, failed_reads=p.failed,
                              time=time.strftime('%H:%M:%S')))
            time.sleep(a.poll_sec)
    except KeyboardInterrupt:
        reason = 'interrupted'
    if r.mode_ptr is not None:
        log('MODE_END ' + kv(n=r.n, last_frame=r.frame, why=reason))
    if r.scan_thread is not None:
        r.scan_thread.join(timeout=30)
    log('EXIT ' + kv(reason=reason, modes=r.n, scans=r.scans, records=len(r.records), states=r.stats['states'],
                     gaps=r.stats['gaps'], torn=r.stats['torn'], failed_reads=p.failed))
    status(**dict(r.stats, state='done', reason=reason, reads=p.reads, failed_reads=p.failed))
    p.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
