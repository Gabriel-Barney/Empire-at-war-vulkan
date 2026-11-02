#!/usr/bin/env python3
"""s7s8_reader.py - read-only recorder for the S7/S8 probe (FoC Steam x64 1.121).

  s7s8_reader.py --out LOG --config s7s8_config.json [--status JSON] [--wait-process-sec 900] [--timeout-sec 14400]
                 [--scan-sec 5] [--poll-sec 0.004] [--allow-unknown-exe] [--pid N --test-base 0xADDR]
  s7s8_reader.py --selftest          offline checks (record regex, model-pointer search, extra-field decoding)

Process access, exe check and heap scan come from tools/probe_mod/a1_tactical/a1mem.py (imported, not copied):
OpenProcess(VM_READ | QUERY_INFORMATION | SYNCHRONIZE) + ReadProcessMemory only; the game is never written.

For every land and space mode it records, polling every --poll-sec (several polls per 30 Hz frame):
  * new objects (absent at the mode's first walk) of the probe's types (config watch lists) and every new PROJ_*
    object, within 1300 (land) / 3800 (space) of a test site; the ground marker DUMMY_BARRAGE_TARGET, PROJ_REMOTE_BOMB
    and the Saboteur anywhere; any other new object near a site as role 'other' (launched fighters, deployed troopers).
    Both object stores of the mode are walked (mode +0x18 primary, +0x20 secondary when it looks like a store), so a
    projectile is seen whichever store holds it. Per object, on change: owner (+0x58), flags (+0x3a0: 0x40 dying,
    0x02 removing), health (+0x5c), position (+0x78), the primary animation slot of its model (searched model
    pointer, as the OI probe), and (not for projectiles):
      loco = Walk/locomotor state: obj +0xa8 -> data pack +0x48 (MOVEMENT sec. 3.2; 0x16 stopped, 0x18 deploying,
             0x19 deployed, 0x1a undeploying);
      tgt  = current target: obj +0xf0 -> targeting pack +0x08 (object id, 'none' or 'x' for an unknown pointer);
      ts   = targeting state: targeting pack +0x18 (TACTICAL_COMBAT sec. 3.1.1: 4 attacking, 3 not aimed, 2 out of range);
      cont = container: obj +0x2b0 (object id of the squadron / team container);
      tt   = team target of that container: container +0x110 -> team data +0x30 (FIGHTER_FLYING sec. 4.10 B; a
             pointer is tried first, then the same offset inline; TEAMDATA line says which);
  * the probe's Lua records ("S78P1|...|END" strings) by a heap scan every --scan-sec (second thread) and at mode end.

Log lines (key=value): S78READER 1, GAME, MODE n= ptr= type= frame= gframe=, PL, STORE n= which= ptr=,
  OBJ n= frame= obj= addr= store= type= owner= x= y= z= hp= role=,
  ST n= frame= obj= owner= flags= hp= x= y= z= anim=T:M:P:S loco= tgt= ts= cont= tt=,
  GONE n= frame= obj= type=, MODELOFF / MODELOFF_FAIL, TEAMDATA n= how=, WALK, SCAN, LUA n= frame= addr= rec=, SRC,
  MODE_END, EXIT.
"""
import argparse
import json
import math
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

REC_NEEDLE = b'S78P1|'
SRC_MARKERS = (b'S78PROBE_SRC_MARK_V1',)
RECORD_RE = re.compile(rb'S78P1\|([A-Z]+)\|([ -~]{0,385}?)\|END')

OBJ_HP = 0x5C
OBJ_LOCO = 0xA8
OBJ_TD = 0xF0
OBJ_CONT = 0x2B0
LOCO_STATE = 0x48
TD_TARGET = 0x08
TD_STATE = 0x18
TEAM_DATA = 0x110
TEAM_TARGET = 0x30
MODE_SECONDARY = 0x20
SLOT = 0xB8
MODEL_TIME = 0x148
ANIM_TYPES = 119
RADIUS = {1: 1300.0, 2: 3800.0}
MAX_WATCH = 700


def kv(**k):
    return ' '.join('%s=%s' % (a, b) for a, b in k.items())


def lua_records(hits):
    out, seen = [], set()
    for addr, tail in hits:
        m = RECORD_RE.match(tail)
        if not m:
            continue
        text = m.group(0).decode('ascii')
        if text not in seen:
            seen.add(text)
            out.append((addr, text))
    return out


def plausible_ptr(q):
    return q is not None and 0x10000 <= q < 0x7FFF00000000 and q % 8 == 0


# ---- model pointer search (as oi_reader.py) ------------------------------------------------------------------------

def slot_ok(b, off=SLOT):
    if len(b) < off + 0x20:
        return None
    clip, crc, typ, mode, var, fr, sp = struct.unpack_from('<QIiIiff', b, off)
    if not plausible_ptr(clip) or crc == 0:
        return None
    if not (0 <= typ < ANIM_TYPES) or mode not in (0, 1, 2) or not (0 <= var < 16):
        return None
    if not (math.isfinite(fr) and 0.0 <= fr < 5000.0 and math.isfinite(sp) and 0.0 < sp <= 20.0):
        return None
    return typ, mode, fr, sp


def time_ok(b, sim_t):
    if sim_t is None or len(b) < MODEL_TIME + 8:
        return False
    f32 = struct.unpack_from('<f', b, MODEL_TIME)[0]
    f64 = struct.unpack_from('<d', b, MODEL_TIME)[0]
    return (math.isfinite(f32) and abs(f32 - sim_t) < 2.0) or (math.isfinite(f64) and abs(f64 - sim_t) < 2.0)


def model_candidates(read, obj, sim_t, span=0x400, span2=0x200):
    out = {}
    ob = read(obj, span)
    if not ob:
        return out
    firsts = []
    for off in range(0, span, 8):
        q = struct.unpack_from('<Q', ob, off)[0]
        if not plausible_ptr(q):
            continue
        firsts.append((off, q))
        b = read(q, MODEL_TIME + 8)
        if b and slot_ok(b):
            out[(off, None)] = 2 + (2 if time_ok(b, sim_t) else 0)
    if any(v >= 4 for v in out.values()):
        return out
    for off, q in firsts:
        qb = read(q, span2)
        if not qb:
            continue
        for off2 in range(0, span2, 8):
            r = struct.unpack_from('<Q', qb, off2)[0]
            if not plausible_ptr(r):
                continue
            b = read(r, MODEL_TIME + 8)
            if b and slot_ok(b):
                out[(off, off2)] = 1 + (2 if time_ok(b, sim_t) else 0)
    return out


def choose_model_offset(per_object):
    tally = {}
    for cands in per_object:
        for k, s in cands.items():
            t = tally.setdefault(k, [0, 0])
            t[0] += 1
            t[1] += s
    if not tally:
        return None
    k, (n, s) = max(tally.items(), key=lambda it: (it[1][0], it[1][1], -(it[0][0]), -(it[0][1] or 0)))
    need = 2 if len(per_object) >= 2 else 1
    if n < need or (n == 1 and s < 4):
        return None
    return k, n, s


# ---- extra fields --------------------------------------------------------------------------------------------------

def u64(read, addr):
    b = read(addr, 8)
    return struct.unpack('<Q', b)[0] if b else None


def i32(read, addr):
    b = read(addr, 4)
    return struct.unpack('<i', b)[0] if b else None


def obj_ref(q, known):
    """Object pointer -> 'none' (null), its id (known object) or 'x' (unknown pointer)."""
    if q is None:
        return '-'
    if q == 0:
        return 'none'
    k = known.get(q)
    return str(k[0]) if k else 'x'


def team_target(read, cont, known, how):
    """(text, how) for the team target of container cont; how = 'ptr' / 'inline' once one layout has worked."""
    if how in (None, 'ptr'):
        p = u64(read, cont + TEAM_DATA)
        if plausible_ptr(p):
            q = u64(read, p + TEAM_TARGET)
            if q is not None and (q == 0 or q in known):
                return obj_ref(q, known), 'ptr'
    if how in (None, 'inline'):
        q = u64(read, cont + TEAM_DATA + TEAM_TARGET)
        if q is not None and (q == 0 or q in known):
            return obj_ref(q, known), 'inline'
    return '-', how


def extras(read, ob, known, team_how):
    """Decoded extra fields of one object from its block ob (bytes from obj +0x50): dict + team layout used."""
    def at(off):
        return struct.unpack_from('<Q', ob, off - a1mem.OBJ_BASE)[0]

    out = {'loco': '-', 'tgt': '-', 'ts': '-', 'cont': '-', 'tt': '-'}
    lp = at(OBJ_LOCO)
    if plausible_ptr(lp):
        v = i32(read, lp + LOCO_STATE)
        if v is not None and 0 <= v <= 0x2A:
            out['loco'] = hex(v)
    tp = at(OBJ_TD)
    if plausible_ptr(tp):
        b = read(tp, 0x20)
        if b:
            q = struct.unpack_from('<Q', b, TD_TARGET)[0]
            out['tgt'] = obj_ref(q, known) if (q == 0 or plausible_ptr(q)) else '-'
            s = struct.unpack_from('<i', b, TD_STATE)[0]
            if 0 <= s <= 16:
                out['ts'] = str(s)
    cp = at(OBJ_CONT)
    if cp:
        out['cont'] = obj_ref(cp, known) if plausible_ptr(cp) else '-'
        if cp in known:
            out['tt'], team_how = team_target(read, cp, known, team_how)
    return out, team_how


# ---- recorder ------------------------------------------------------------------------------------------------------

class Log:
    def __init__(self, path):
        self.f = open(path, 'a', encoding='ascii', errors='replace')
        self.lock = threading.Lock()
        self.t0 = time.time()

    def __call__(self, line):
        with self.lock:
            self.f.write(line + '\n')
            self.f.flush()


class Recorder:
    def __init__(self, a, proc, log, cfg):
        self.a, self.p, self.log, self.cfg = a, proc, log, cfg
        self.g = a1mem.Game(proc)
        w = cfg.get('watch', {})
        self.roles = {}
        for role in ('attackers', 'victims', 'human'):
            for t in w.get(role, []):
                self.roles[t.upper()] = role.rstrip('s')
        self.always = set(t.upper() for t in w.get('always', []))
        self.points = {1: [], 2: []}
        for mode, key in ((1, 'land'), (2, 'space')):
            m = cfg.get(key) or {}
            if m.get('points_ok'):
                self.points[mode] = [(p['x'], p['y']) for p in m.get('points', {}).values()]
        self.n = 0
        self.mode_ptr = None
        self.mode_type = None
        self.frame = None
        self.gframe = None
        self.records = set()
        self.scans = 0
        self.scan_thread = None
        self.next_scan = 0.0
        self.stats = {'modes': 0, 'scans': 0, 'records': 0, 'objects': 0, 'states': 0, 'model_off': None}
        self.reset_mode()

    def reset_mode(self):
        self.initial = None
        self.known = {}
        self.watched = {}
        self.sizes = {}
        self.stores = []
        self.last_walk = 0.0
        self.model_key = None
        self.model_tries = 0
        self.next_model_try = 0.0
        self.team_how = None
        self.team_logged = False

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
        self.mode_ptr, self.mode_type, self.frame, self.gframe = m['ptr'], m['type'], m['frame'], m['gframe']
        self.reset_mode()
        self.log('MODE ' + kv(n=self.n, ptr=hex(m['ptr']), type=m['type'], game_type=m['game_type'], stack=m['stack'],
                              frame=m['frame'], gframe=m['gframe'], t='%.1f' % (time.time() - self.log.t0)))
        pl = self.g.players()
        for p in (pl or {}).get('by_id', []):
            self.log('PL ' + kv(n=self.n, id=p['id'], faction=p['faction'] or '?', human=p['human']))
        for which, off in (('primary', a1mem.MODE_PRIMARY), ('secondary', MODE_SECONDARY)):
            gom = self.p.u64(self.mode_ptr + off)
            if not plausible_ptr(gom):
                continue
            head = self.p.u64(gom + a1mem.GOM_ID_HEAD)
            size = self.p.u64(gom + a1mem.GOM_ID_SIZE)
            if plausible_ptr(head) and size is not None and size <= 200000:
                self.stores.append((which, gom))
                self.log('STORE ' + kv(n=self.n, which=which, ptr=hex(gom), size=size))

    def end_mode(self, why='mode_changed'):
        self.log('MODE_END ' + kv(n=self.n, last_frame=self.frame, why=why))
        self.start_scan('mode_end', wait=True)
        self.mode_ptr = None

    def near_point(self, x, y):
        pts = self.points.get(self.mode_type) or []
        if not pts:
            return True
        r = RADIUS.get(self.mode_type, 2000.0)
        return any(math.hypot(x - px, y - py) <= r for px, py in pts)

    def role_of(self, tname, x, y):
        if tname in self.always:
            return self.roles.get(tname, 'marker' if 'DUMMY' in tname else ('projectile' if tname.startswith('PROJ_')
                                                                              else 'human'))
        if not self.near_point(x, y):
            return None
        if tname in self.roles:
            return self.roles[tname]
        if tname.startswith('PROJ_'):
            return 'projectile'
        return 'other'

    def walk(self):
        t = time.time()
        seen = set()
        first = self.initial is None
        for which, gom in self.stores:
            head = self.p.u64(gom + a1mem.GOM_ID_HEAD)
            size = self.p.u64(gom + a1mem.GOM_ID_SIZE)
            if not head or size is None or size > 200000:
                continue
            self.sizes[which] = size
            node = self.p.u64(head)
            k = 0
            while node and node != head and k <= size + 16:
                nb = self.p.read(node, 0x20)
                if not nb:
                    break
                nxt, = struct.unpack_from('<Q', nb, 0)
                val, = struct.unpack_from('<Q', nb, 0x18)
                k += 1
                node = nxt
                if not val:
                    continue
                seen.add(val)
                if val in self.known:
                    continue
                ob = self.p.read(val + a1mem.OBJ_BASE, a1mem.OBJ_END - a1mem.OBJ_BASE)
                if not ob:
                    continue
                oid, = struct.unpack_from('<i', ob, a1mem.OBJ_ID - a1mem.OBJ_BASE)
                tptr, = struct.unpack_from('<Q', ob, a1mem.OBJ_TYPE - a1mem.OBJ_BASE)
                tname = self.g.type_name(tptr)
                self.known[val] = (oid, tname)
                if first:
                    continue
                x, y, z = struct.unpack_from('<fff', ob, a1mem.OBJ_POS - a1mem.OBJ_BASE)
                role = self.role_of(tname, x, y)
                if role is None or len(self.watched) >= MAX_WATCH:
                    continue
                owner, = struct.unpack_from('<i', ob, a1mem.OBJ_OWNER - a1mem.OBJ_BASE)
                hp, = struct.unpack_from('<f', ob, OBJ_HP - a1mem.OBJ_BASE)
                self.watched[val] = {'id': oid, 'type': tname, 'role': role, 'last': None, 'lastF': None}
                self.stats['objects'] += 1
                self.log('OBJ ' + kv(n=self.n, frame=self.frame, obj=oid, addr=hex(val), store=which, type=tname,
                                     owner=owner, x='%.1f' % x, y='%.1f' % y, z='%.1f' % z, hp='%.2f' % hp, role=role))
        if first:
            self.initial = set(seen)
            self.log('WALK ' + kv(n=self.n, frame=self.frame, initial=len(seen), stores=len(self.stores),
                                  sec='%.3f' % (time.time() - t)))
        for addr in [a for a in self.watched if a not in seen]:
            w = self.watched.pop(addr)
            self.log('GONE ' + kv(n=self.n, frame=self.frame, obj=w['id'], type=w['type']))
        for addr in [a for a in self.known if a not in seen]:
            del self.known[addr]
        self.last_walk = time.time()

    def find_model(self):
        objs = [a for a, w in self.watched.items() if w['role'] not in ('projectile', 'marker')]
        if not objs:
            return
        self.model_tries += 1
        sim_t = (self.gframe / 30.0) if self.gframe is not None else None
        per = [model_candidates(self.p.read, a, sim_t) for a in objs[:6]]
        best = choose_model_offset(per)
        if best is None:
            self.log('MODELOFF_FAIL ' + kv(n=self.n, frame=self.frame, try_=self.model_tries, objs=len(per),
                                           cands=sum(len(c) for c in per)))
            return
        (off, off2), cnt, score = best
        self.model_key = (off, off2)
        self.stats['model_off'] = '%s/%s' % (hex(off), hex(off2) if off2 is not None else '-')
        self.log('MODELOFF ' + kv(n=self.n, frame=self.frame, how='d1' if off2 is None else 'd2', off=hex(off),
                                  off2=hex(off2) if off2 is not None else '-', objs=cnt, score=score))

    def anim(self, addr):
        if self.model_key is None:
            return '-'
        off, off2 = self.model_key
        m = self.p.u64(addr + off)
        if m and off2 is not None:
            m = self.p.u64(m + off2)
        if not m:
            return '-'
        b = self.p.read(m + SLOT, 0x20)
        if not b:
            return '-'
        clip, crc, typ, mode, var, fr, sp = struct.unpack_from('<QIiIiff', b, 0)
        if not clip:
            return 'none'
        return '%d:%d:%.3f:%.3f' % (typ, mode, fr, sp)

    def poll_watched(self):
        for addr, w in list(self.watched.items()):
            ob = self.p.read(addr + a1mem.OBJ_BASE, a1mem.OBJ_END - a1mem.OBJ_BASE)
            if not ob:
                continue
            owner, = struct.unpack_from('<i', ob, a1mem.OBJ_OWNER - a1mem.OBJ_BASE)
            x, y, z = struct.unpack_from('<fff', ob, a1mem.OBJ_POS - a1mem.OBJ_BASE)
            hp, = struct.unpack_from('<f', ob, OBJ_HP - a1mem.OBJ_BASE)
            flags = ob[a1mem.OBJ_FLAGS - a1mem.OBJ_BASE]
            if w['role'] in ('projectile', 'marker'):
                ex = {'loco': '-', 'tgt': '-', 'ts': '-', 'cont': '-', 'tt': '-'}
                an = '-'
            else:
                ex, how = extras(self.p.read, ob, self.known, self.team_how)
                if how is not None and how != self.team_how:
                    self.team_how = how
                    self.log('TEAMDATA ' + kv(n=self.n, frame=self.frame, how=how))
                an = self.anim(addr)
            key = (owner, flags, '%.2f' % hp, an, ex['loco'], ex['tgt'], ex['ts'], ex['cont'], ex['tt'])
            pos = ('%.1f' % x, '%.1f' % y, '%.1f' % z)
            last = w['last']
            if last is not None and last[0] == key and (last[1] == pos or w['lastF'] == self.frame):
                continue
            w['last'] = (key, pos)
            w['lastF'] = self.frame
            self.stats['states'] += 1
            self.log('ST ' + kv(n=self.n, frame=self.frame, obj=w['id'], owner=owner, flags=hex(flags), hp=key[2],
                                x=pos[0], y=pos[1], z=pos[2], anim=an, loco=ex['loco'], tgt=ex['tgt'], ts=ex['ts'],
                                cont=ex['cont'], tt=ex['tt']))

    def step(self):
        m = self.g.mode()
        if m is None:
            return
        tactical = m['type'] in (1, 2) and m['ptr']
        if self.mode_ptr is not None and m['ptr'] != self.mode_ptr:
            self.end_mode()
        if not tactical:
            return
        if self.mode_ptr is None:
            self.new_mode(m)
        if m['frame'] is not None:
            self.frame = m['frame']
        self.gframe = m['gframe']
        now = time.time()
        changed = False
        for which, gom in self.stores:
            size = self.p.u64(gom + a1mem.GOM_ID_SIZE)
            if size != self.sizes.get(which):
                changed = True
        if self.initial is None or (changed and now - self.last_walk >= 0.01) or now - self.last_walk >= 1.0:
            self.walk()
        if self.model_key is None and self.watched and now >= self.next_model_try and self.model_tries < 20:
            self.next_model_try = now + 1.0
            self.find_model()
        self.poll_watched()
        if (self.frame or 0) >= 150 and now >= self.next_scan:
            self.next_scan = now + self.a.scan_sec
            self.start_scan('periodic')


def wait_for_game(a, log):
    t0 = time.time()
    ignored = set()
    while time.time() - t0 < a.wait_process_sec:
        for pid in a1mem.find_pids():
            if pid in ignored:
                continue
            try:
                p = a1mem.Proc(pid)
            except OSError as e:
                log('OPEN_FAILED ' + kv(pid=pid, error=str(e).replace(' ', '_')))
                ignored.add(pid)
                continue
            if p.pe_identity() or a.allow_unknown_exe:
                return p
            log('IGNORED ' + kv(pid=pid, timestamp=hex(p.timestamp or 0), size_of_image=hex(p.size_of_image or 0)))
            ignored.add(pid)
            p.close()
        time.sleep(1.0)
    return None


# ---- selftest ------------------------------------------------------------------------------------------------------

class FakeMem:
    def __init__(self):
        self.mem = {}

    def put(self, addr, data):
        self.mem[addr] = bytearray(data)

    def read(self, addr, n):
        for base, buf in self.mem.items():
            if base <= addr and addr + n <= base + len(buf):
                return bytes(buf[addr - base:addr - base + n])
        return None


def selftest():
    fails = []

    def check(c, what):
        print(('  ok    ' if c else '  FAIL  ') + what)
        if not c:
            fails.append(what)

    rec = b'xx S78P1|ACT|r=abc123.1|f=612|scn=SPMAT|tag=spmat|blk=deploy1|ab=DEPLOY|ret=block|END yy'
    i = rec.find(REC_NEEDLE)
    got = lua_records([(0x1000 + i, rec[i:i + 400])])
    check(len(got) == 1 and got[0][1].endswith('|END') and 'blk=deploy1' in got[0][1], 'record regex')
    check(lua_records([(0, b'S78P1|P|f=1|no end here')]) == [], 'unterminated record ignored')

    fm = FakeMem()

    def model(t_sim, typ, fr):
        b = bytearray(0x200)
        struct.pack_into('<QIiIiff', b, SLOT, 0x300000, 0xABCD, typ, 1, 0, fr, 1.0)
        struct.pack_into('<f', b, MODEL_TIME, t_sim)
        return b

    for base, mdl in ((0x100000, 0x200000), (0x110000, 0x210000)):
        o = bytearray(0x400)
        struct.pack_into('<Q', o, 0x120, mdl)
        fm.put(base, o)
        fm.put(mdl, model(100.0, 25, 3.0))
    best = choose_model_offset([model_candidates(fm.read, 0x100000, 100.0),
                                model_candidates(fm.read, 0x110000, 100.0)])
    check(best is not None and best[0] == (0x120, None), 'model pointer found at +0x120 (%s)' % (best,))

    # extras: object 0x500000 (id 7) with a locomotor pack in state 0x19, a targeting pack aimed at object
    # 0x600000 (id 9) in state 4, inside container 0x700000 (id 11) whose team data (pointer) targets id 9
    fm2 = FakeMem()
    obj = bytearray(0x400)
    struct.pack_into('<Q', obj, OBJ_LOCO, 0x800000)
    struct.pack_into('<Q', obj, OBJ_TD, 0x810000)
    struct.pack_into('<Q', obj, OBJ_CONT, 0x700000)
    loco = bytearray(0x100)
    struct.pack_into('<i', loco, LOCO_STATE, 0x19)
    fm2.put(0x800000, loco)
    td = bytearray(0x40)
    struct.pack_into('<Q', td, TD_TARGET, 0x600000)
    struct.pack_into('<i', td, TD_STATE, 4)
    fm2.put(0x810000, td)
    cont = bytearray(0x200)
    struct.pack_into('<Q', cont, TEAM_DATA, 0x820000)
    fm2.put(0x700000, cont)
    team = bytearray(0x80)
    struct.pack_into('<Q', team, TEAM_TARGET, 0x600000)
    fm2.put(0x820000, team)
    known = {0x500000: (7, 'X-WING'), 0x600000: (9, 'TARTAN_PATROL_CRUISER'), 0x700000: (11, 'REBEL_X-WING_SQUADRON')}
    ob = bytes(obj[a1mem.OBJ_BASE:a1mem.OBJ_END])
    ex, how = extras(fm2.read, ob, known, None)
    check(ex == {'loco': '0x19', 'tgt': '9', 'ts': '4', 'cont': '11', 'tt': '9'} and how == 'ptr',
          'extras decoded through pointers: %s (%s)' % (ex, how))
    # inline team data, null target, unknown locomotor value
    struct.pack_into('<Q', cont, TEAM_DATA, 0)
    struct.pack_into('<Q', cont, TEAM_DATA + TEAM_TARGET, 0x500000)
    fm2.put(0x700000, cont)
    struct.pack_into('<Q', td, TD_TARGET, 0)
    fm2.put(0x810000, td)
    struct.pack_into('<i', loco, LOCO_STATE, 0x99)
    fm2.put(0x800000, loco)
    ex, how = extras(fm2.read, ob, known, None)
    check(ex == {'loco': '-', 'tgt': 'none', 'ts': '4', 'cont': '11', 'tt': '7'} and how == 'inline',
          'inline team data, null target, out-of-range state: %s (%s)' % (ex, how))
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
    ap.add_argument('--poll-sec', type=float, default=0.004)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--test-base', help='hex base of a fake game image (tests)')
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
    log('S78READER 1 ' + kv(scan_sec=a.scan_sec, poll_sec=a.poll_sec,
                            land=(cfg.get('land') or {}).get('map'), space=(cfg.get('space') or {}).get('map')))
    if a.pid:
        p = a1mem.Proc(a.pid, int(a.test_base, 16) if a.test_base else None)
        identity = 'test' if a.test_base else ('ok' if p.pe_identity() else 'unknown')
    else:
        p = wait_for_game(a, log)
        if p is None:
            log('EXIT ' + kv(reason='no_game', modes=0, scans=0, records=0))
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
            if a.status and now - last_status > 2.0:
                last_status = now
                with open(a.status, 'w') as f:
                    json.dump(dict(r.stats, state='running', mode=r.n, frame=r.frame, reads=p.reads,
                                   failed_reads=p.failed, time=time.strftime('%H:%M:%S')), f)
            time.sleep(a.poll_sec)
    except KeyboardInterrupt:
        reason = 'interrupted'
    if r.mode_ptr is not None:
        log('MODE_END ' + kv(n=r.n, last_frame=r.frame, why=reason))
    if r.scan_thread is not None:
        r.scan_thread.join(timeout=30)
    log('EXIT ' + kv(reason=reason, modes=r.n, scans=r.scans, records=len(r.records), objects=r.stats['objects'],
                     states=r.stats['states'], model_off=r.stats['model_off'], failed_reads=p.failed))
    if a.status:
        with open(a.status, 'w') as f:
            json.dump(dict(r.stats, state='done', reason=reason, reads=p.reads, failed_reads=p.failed), f)
    p.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
