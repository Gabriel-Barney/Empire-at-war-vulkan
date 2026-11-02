#!/usr/bin/env python3
"""oi_reader.py - read-only recorder for the OI ability probe (FoC Steam x64 1.121).

  oi_reader.py --out LOG --config oi_config.json [--status JSON] [--wait-process-sec 900] [--timeout-sec 14400]
               [--scan-sec 5] [--poll-sec 0.004] [--allow-unknown-exe] [--pid N --test-base 0xADDR]
  oi_reader.py --selftest            offline checks of the record regex and the model-pointer search

Process access, exe check and heap scan come from tools/probe_mod/a1_tactical/a1mem.py (imported, not copied):
OpenProcess(VM_READ | QUERY_INFORMATION | SYNCHRONIZE) + ReadProcessMemory only; the game is never written.

For every land mode it records, polling every --poll-sec (several polls per 30 Hz frame):
  * the objects the probe spawns: types in the config's watch lists (attackers RANCOR / HAN_SOLO / YODA /
    REBEL_INFILTRATOR, victims MAL_ROCKET_VEHICLE / T4B_TANK / STORMTROOPER, projectiles PROJ_STICKY_BOMB /
    PROJ_SPECIAL_HAN_SOLO_STUN_BLAST) that did not exist at the mode's first pass and (with spawn points) lie within
    900 units of one. Per object, on every change: owner (+0x58), flags (+0x3a0: 0x40 dying, 0x02 removing), health
    (+0x5c, f32; docs/re/L1_S4_REMAINING.md, TERRAIN_WATER_POST §1.6), position (+0x78) and the primary animation
    slot of its model (ANIM_PARTICLES §A.3: ModelClass +0xb8: type id +0x0c, mode +0x10 (0 one-shot, 1 loop,
    2 finished), frame position +0x18 (f32), speed +0x1c). The object -> model pointer offset is not documented: it
    is searched once (pointer fields of the object, one or two levels deep, whose target holds a plausible slot at
    +0xb8 and the sim time global frame / 30 at +0x148) and logged (MODELOFF);
  * the probe's Lua records ("OIP1|...|END" strings) by a heap scan every --scan-sec (second thread) and at mode end.
The store is walked again whenever its size changes, so a new projectile is seen in the poll after it is added.

Log lines (key=value): OIREADER 1, GAME, MODE n= ptr= type= frame= gframe=, PL n= id= faction= human=,
  OBJ n= frame= obj= addr= type= owner= x= y= z= hp= role=, ST n= frame= obj= owner= flags= hp= x= y= z= anim=T:M:P:S
  (anim - when no model), GONE n= frame= obj=, MODELOFF n= frame= how= off= off2= objs= score= | MODELOFF_FAIL,
  WALK n= frame= nodes= sec=, SCAN ..., LUA n= frame= addr= rec=, SRC n= frame= marker=, MODE_END, EXIT.
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

REC_NEEDLE = b'OIP1|'
SRC_MARKERS = (b'OIPROBE_SRC_MARK_V1',)
RECORD_RE = re.compile(rb'OIP1\|([A-Z]+)\|([ -~]{0,385}?)\|END')

OBJ_HP = 0x5C
SLOT = 0xB8
MODEL_TIME = 0x148
ANIM_TYPES = 119
WATCH_RADIUS = 900.0


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


# ---- model pointer search ------------------------------------------------------------------------------------------

def plausible_ptr(q):
    return 0x10000 <= q < 0x7FFF00000000 and q % 8 == 0


def slot_ok(b, off=SLOT):
    """The primary animation slot at b[off:off+0x30] looks valid -> (type, mode, frame, speed) or None."""
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
    """+0x148 holds the sim time of the last advance (global frame / 30), as f32 or f64."""
    if sim_t is None or len(b) < MODEL_TIME + 8:
        return False
    f32 = struct.unpack_from('<f', b, MODEL_TIME)[0]
    f64 = struct.unpack_from('<d', b, MODEL_TIME)[0]
    return (math.isfinite(f32) and abs(f32 - sim_t) < 2.0) or (math.isfinite(f64) and abs(f64 - sim_t) < 2.0)


def model_candidates(read, obj, sim_t, span=0x400, span2=0x200):
    """{(off, off2): score} for one object; off2 = None for a direct pointer. read(addr, n) -> bytes | None."""
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
    """per_object: [ {(off, off2): score} ] -> (key, objects, score) of the best offset, or None."""
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
        for role in ('attackers', 'victims', 'projectiles'):
            for t in w.get(role, []):
                self.roles[t.upper()] = role[:-1]
        self.points = [(p['x'], p['y']) for p in cfg.get('points', {}).values()] if cfg.get('points_ok') else []
        self.n = 0
        self.mode_ptr = None
        self.frame = None
        self.gframe = None
        self.records = set()
        self.scans = 0
        self.scan_thread = None
        self.next_scan = 0.0
        self.stats = {'modes': 0, 'scans': 0, 'records': 0, 'objects': 0, 'states': 0, 'model_off': None}
        self.reset_mode()

    def reset_mode(self):
        self.initial = None      # obj addresses at the mode's first walk
        self.known = {}          # addr -> (id, type name)
        self.watched = {}        # addr -> {'id', 'type', 'role', 'last'}
        self.gom_size = None
        self.last_walk = 0.0
        self.model_key = None
        self.model_tries = 0
        self.next_model_try = 0.0
        self.walks = 0

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
        self.mode_ptr, self.frame, self.gframe = m['ptr'], m['frame'], m['gframe']
        self.reset_mode()
        self.log('MODE ' + kv(n=self.n, ptr=hex(m['ptr']), type=m['type'], game_type=m['game_type'], stack=m['stack'],
                              frame=m['frame'], gframe=m['gframe'], t='%.1f' % (time.time() - self.log.t0)))
        pl = self.g.players()
        for p in (pl or {}).get('by_id', []):
            self.log('PL ' + kv(n=self.n, id=p['id'], faction=p['faction'] or '?', human=p['human']))

    def end_mode(self, why='mode_changed'):
        self.log('MODE_END ' + kv(n=self.n, last_frame=self.frame, why=why))
        self.start_scan('mode_end', wait=True)
        self.mode_ptr = None

    def near_point(self, x, y):
        if not self.points:
            return True
        return any(math.hypot(x - px, y - py) <= WATCH_RADIUS for px, py in self.points)

    def walk(self):
        """Walk the id map; register new watched objects, report watched ones that left the store."""
        t = time.time()
        gom = self.p.u64(self.mode_ptr + a1mem.MODE_PRIMARY)
        if not gom:
            return
        head = self.p.u64(gom + a1mem.GOM_ID_HEAD)
        size = self.p.u64(gom + a1mem.GOM_ID_SIZE)
        if not head or size is None or size > 200000:
            return
        self.gom_size = size
        seen = set()
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
            if self.initial is None:
                continue
            role = self.roles.get(tname)
            if role is None:
                continue
            x, y, z = struct.unpack_from('<fff', ob, a1mem.OBJ_POS - a1mem.OBJ_BASE)
            if not self.near_point(x, y):
                continue
            owner, = struct.unpack_from('<i', ob, a1mem.OBJ_OWNER - a1mem.OBJ_BASE)
            hp, = struct.unpack_from('<f', ob, OBJ_HP - a1mem.OBJ_BASE)
            self.watched[val] = {'id': oid, 'type': tname, 'role': role, 'last': None}
            self.stats['objects'] += 1
            self.log('OBJ ' + kv(n=self.n, frame=self.frame, obj=oid, addr=hex(val), type=tname, owner=owner,
                                 x='%.1f' % x, y='%.1f' % y, z='%.1f' % z, hp='%.2f' % hp, role=role))
        if self.initial is None:
            self.initial = set(seen)
            self.log('WALK ' + kv(n=self.n, frame=self.frame, nodes=k, initial=len(seen),
                                  sec='%.3f' % (time.time() - t)))
        for addr in [a for a in self.watched if a not in seen]:
            w = self.watched.pop(addr)
            self.log('GONE ' + kv(n=self.n, frame=self.frame, obj=w['id'], type=w['type']))
        for addr in [a for a in self.known if a not in seen]:
            del self.known[addr]
        self.walks += 1
        self.last_walk = time.time()

    def find_model(self):
        objs = [a for a, w in self.watched.items() if w['role'] != 'projectile']
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
            st = (owner, flags, '%.2f' % hp, '%.1f' % x, '%.1f' % y, '%.1f' % z,
                  self.anim(addr) if w['role'] != 'projectile' else '-')
            if st != w['last']:
                w['last'] = st
                self.stats['states'] += 1
                self.log('ST ' + kv(n=self.n, frame=self.frame, obj=w['id'], owner=st[0], flags=hex(st[1]), hp=st[2],
                                    x=st[3], y=st[4], z=st[5], anim=st[6]))

    def step(self):
        m = self.g.mode()
        if m is None:
            return
        land = m['type'] == 1 and m['ptr']
        if self.mode_ptr is not None and m['ptr'] != self.mode_ptr:
            self.end_mode()
        if not land:
            return
        if self.mode_ptr is None:
            self.new_mode(m)
        if m['frame'] is not None:
            self.frame = m['frame']
        self.gframe = m['gframe']
        now = time.time()
        gom = self.p.u64(self.mode_ptr + a1mem.MODE_PRIMARY)
        size = self.p.u64(gom + a1mem.GOM_ID_SIZE) if gom else None
        if self.initial is None or (size != self.gom_size and now - self.last_walk >= 0.01) or \
                now - self.last_walk >= 1.0:
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

def selftest():
    fails = []

    def check(c, what):
        print(('  ok    ' if c else '  FAIL  ') + what)
        if not c:
            fails.append(what)

    rec = b'xx OIP1|P|r=abc123.1|f=612|scn=GRENADE|infiltrator=1:1.000:1480:760:0:1;|END yy'
    i = rec.find(REC_NEEDLE)
    got = lua_records([(0x1000 + i, rec[i:i + 400])])
    check(len(got) == 1 and got[0][1].endswith('|END') and 'scn=GRENADE' in got[0][1], 'record regex')
    check(lua_records([(0, b'OIP1|P|f=1|no end here')]) == [], 'unterminated record ignored')

    # fake memory: object at 0x100000 -> +0x120 -> model at 0x200000 (direct), another object via two levels
    mem = {}

    def put(addr, data):
        mem[addr] = bytearray(data)

    def read(addr, n):
        for base, buf in mem.items():
            if base <= addr and addr + n <= base + len(buf):
                return bytes(buf[addr - base:addr - base + n])
        return None

    def model(t_sim, typ, fr):
        b = bytearray(0x200)
        struct.pack_into('<QIiIiff', b, SLOT, 0x300000, 0xABCD, typ, 1, 0, fr, 1.0)
        struct.pack_into('<f', b, MODEL_TIME, t_sim)
        return b

    o1 = bytearray(0x400)
    struct.pack_into('<Q', o1, 0x120, 0x200000)
    struct.pack_into('<Q', o1, 0x40, 0x500000)       # a decoy pointer to junk
    put(0x100000, o1)
    put(0x200000, model(100.0, 63, 12.5))
    put(0x500000, bytes(0x400))
    o2 = bytearray(0x400)
    struct.pack_into('<Q', o2, 0x120, 0x210000)
    put(0x110000, o2)
    put(0x210000, model(100.0, 0, 3.0))
    per = [model_candidates(read, 0x100000, 100.0), model_candidates(read, 0x110000, 100.0)]
    best = choose_model_offset(per)
    check(best is not None and best[0] == (0x120, None), 'direct model pointer found at +0x120 (%s)' % (best,))
    check(slot_ok(bytes(model(1.0, 200, 1.0))) is None, 'slot with type id 200 rejected')
    # two levels: object +0x88 -> holder, holder +0x30 -> model
    o3 = bytearray(0x400)
    struct.pack_into('<Q', o3, 0x88, 0x600000)
    put(0x120000, o3)
    h = bytearray(0x200)
    struct.pack_into('<Q', h, 0x30, 0x220000)
    put(0x600000, h)
    put(0x220000, model(50.0, 104, 7.0))
    o4 = bytearray(0x400)
    struct.pack_into('<Q', o4, 0x88, 0x610000)
    put(0x130000, o4)
    h2 = bytearray(0x200)
    struct.pack_into('<Q', h2, 0x30, 0x230000)
    put(0x610000, h2)
    put(0x230000, model(50.0, 5, 2.0))
    per = [model_candidates(read, 0x120000, 50.0), model_candidates(read, 0x130000, 50.0)]
    best = choose_model_offset(per)
    check(best is not None and best[0] == (0x88, 0x30), 'two-level model pointer found at +0x88 -> +0x30 (%s)' % (best,))
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
    log('OIREADER 1 ' + kv(scan_sec=a.scan_sec, poll_sec=a.poll_sec, points=len(cfg.get('points', {})),
                           points_ok=cfg.get('points_ok'), map=cfg.get('map')))
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
