#!/usr/bin/env python3
"""fow_reader.py - read-only recorder for the FOW regrow probe (FoC Steam x64 1.121).

  fow_reader.py --out LOG [--status JSON] [--wait-process-sec 900] [--timeout-sec 14400] [--scan-sec 5]
                [--poll-sec 0.004] [--allow-unknown-exe]

Waits for StarWarsG.exe (FoC 1.121 PE identity, a1mem.py), then in every LAND mode:
  * finds the FOW grids (FOG_OF_WAR.md §1.1 / §1.3: mode +0x198 = FOWGrid* array by player id, count at +0x190;
    falls back to a search of the mode object for a pointer array whose entries validate as grids) and follows the
    human player's grid (player +0x62);
  * once per mode frame (polls every --poll-sec, several per 33 ms frame) reads the grid header, the state bytes and
    the u16 reveal counts and logs what changed: decays (+0x44), cells whose count went 0 -> >0 (CUP) or >0 -> 0
    (CDN), cells whose state went >0 -> 0 (ZERO, with the +0x44 decay frame that did it), and for every CDN group
    the state min / max / zero count at each change (TRK) until the group is all 0;
  * every 2 s logs the probe's spawned objects (POBJ: type, owner, position) from the object store;
  * every --scan-sec scans the game's private heap for the probe Lua's records ("FWR1|...|END", fow_hook.lua).
It never writes the game's memory. Ends when the game exits.

Log lines (key=value; cells = row:c0-c1 spans joined by ','):
  FWREADER 1 / GAME / MODE n= ptr= type= frame= / P n= id= human= faction= ... (a1 format)
  GRIDS n= frame= via= ids=   GRID n= id= addr= W= H= N= cw= ch= x0= y0= x1= y1= last= step= revall=   TARGET n= id=
  BASE n= f= counted= nonzero=          first full read of the target grid
  DEC n= f= last= step=                 the grid's last-decay frame (+0x44) changed
  CUP / CDN n= f= last= k= cells=       ZERO n= f= last= k= cells=
  GRP n= g= f= k= smin= smax= cells=    a CDN group starts being tracked
  TRK n= g= f= last= smin= smax= zero=<z>/<k> counted=<c>
  SKIP n= f= prev=                      frames not seen between two processed frames
  POBJ n= f= obj= type= owner= x= y=    SCAN / LUA n= frame= rec=FWR1|...|END / SRC   MODE_END   EXIT
"""
import argparse
import json
import os
import re
import struct
import sys
import threading
import time
from array import array

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
import a1mem  # noqa: E402  (read-only process access + documented layout, validated by the A1 run 2026-10-31)
from a1mem import Log, wait_for_game  # noqa: E402

REC_NEEDLE = b'FWR1|'
SRC_MARKERS = (b'FWRPROBE_SRC_MARK_V1',)
RECORD_RE = re.compile(rb'FWR1\|([A-Z]+)\|([ -~]{0,360}?)\|END')
PROBE_TYPES = ('MPTL', 'T4B_TANK', 'T2B_TANK', 'SPMAT_WALKER', 'AT_AA_WALKER', 'AT_ST_WALKER',
               'CANDEROUS_ASSAULT_TANK', 'MZ8_PULSE_CANNON_TANK')

# ---- FOWGrid (FOG_OF_WAR.md §1.1) -----------------------------------------------------------------------------------
MODE_GRID_COUNT = 0x190
MODE_GRID_ARRAY = 0x198
G_SIZE = 0x58


def grid_header(b):
    st, cn, fd, dense = struct.unpack_from('<QQQQ', b, 0)
    N, W, H = struct.unpack_from('<iii', b, 0x20)
    cw, ch, y1, x0, y0, x1 = struct.unpack_from('<6f', b, 0x2C)
    last, owner, step = struct.unpack_from('<iii', b, 0x44)
    return {'state': st, 'count': cn, 'fade': fd, 'dense': dense, 'N': N, 'W': W, 'H': H, 'cw': cw, 'ch': ch,
            'x0': x0, 'y0': y0, 'x1': x1, 'y1': y1, 'last': last, 'owner': owner, 'step': step, 'revall': b[0x50]}


def grid_valid(h):
    if not (1 <= h['W'] <= 512 and 1 <= h['H'] <= 512 and h['N'] == h['W'] * h['H']):
        return False
    if not (h['state'] and h['count'] and h['fade']) or not (0 <= h['owner'] < 64):
        return False
    if not (h['cw'] > 0 and h['ch'] > 0 and h['x1'] > h['x0'] and h['y1'] > h['y0']):
        return False
    return abs(h['cw'] * h['W'] - (h['x1'] - h['x0'])) < 1.0 and abs(h['ch'] * h['H'] - (h['y1'] - h['y0'])) < 1.0


def read_grid(p, addr):
    b = p.read(addr, G_SIZE) if addr else None
    if not b:
        return None
    h = grid_header(b)
    return h if grid_valid(h) else None


def grids_from_array(p, arr, count):
    out = {}
    if not arr or not (0 < count <= 64):
        return out
    b = p.read(arr, 8 * count)
    if not b:
        return out
    for i in range(count):
        g, = struct.unpack_from('<Q', b, 8 * i)
        h = read_grid(p, g)
        if h and h['owner'] == i:
            out[i] = g
    return out


def find_grids(p, mode_ptr):
    """({player id: grid addr}, how found)."""
    arr = p.u64(mode_ptr + MODE_GRID_ARRAY)
    cnt = p.i32(mode_ptr + MODE_GRID_COUNT)
    got = grids_from_array(p, arr, cnt or 0)
    if got:
        return got, 'mode+0x198[id]'
    for size in (0x800, 0x400):
        blk = p.read(mode_ptr, size)
        if blk:
            break
    else:
        return {}, 'none'
    for off in range(0, len(blk), 8):
        q, = struct.unpack_from('<Q', blk, off)
        if q < 0x10000 or q > 0x7FFFFFFFFFFF:
            continue
        for cnt in (16, 8, 3, 2):   # a short allocation may end before 16 slots
            got = grids_from_array(p, q, cnt)
            if got:
                return got, 'mode+0x%x[id]' % off
        h = read_grid(p, q)
        if h:
            return {h['owner']: q}, 'mode+0x%x direct' % off
    return {}, 'none'


# ---- change detection (no process access: the selftest drives it with a reference grid) -----------------------------

def diff_positions(a, b, chunk=4096):
    out = []
    for off in range(0, len(a), chunk):
        sa, sb = a[off:off + chunk], b[off:off + chunk]
        if sa != sb:
            out.extend(off + i for i in range(len(sa)) if sa[i] != sb[i])
    return out


def spans_text(idxs, W, limit=6000):
    if len(idxs) > limit:
        return 'TOO_MANY'
    rows = {}
    for i in sorted(idxs):
        rows.setdefault(i // W, []).append(i % W)
    parts = []
    for r in sorted(rows):
        cs = rows[r]
        start = prev = cs[0]
        for c in cs[1:] + [None]:
            if c is not None and c == prev + 1:
                prev = c
                continue
            parts.append('%d:%d-%d' % (r, start, prev))
            if c is not None:
                start = prev = c
    return ','.join(parts)


def components(idxs, W):
    """8-connected components of a cell index list (one tracked group per released area)."""
    left = set(idxs)
    out = []
    while left:
        stack = [left.pop()]
        comp = []
        while stack:
            i = stack.pop()
            comp.append(i)
            r, c = divmod(i, W)
            for dr in (-1, 0, 1):
                for dc in (-1, 0, 1):
                    j = (r + dr) * W + (c + dc)
                    if (dr or dc) and 0 <= c + dc < W and j in left:
                        left.remove(j)
                        stack.append(j)
        out.append(sorted(comp))
    return sorted(out)


class FrameProcessor:
    """Turns successive full reads of one grid into CUP / CDN / ZERO / DEC / GRP / TRK log lines."""

    def __init__(self, log, n, W, H, track_max=5000, track_frames=900):
        self.log, self.n, self.W, self.H, self.N = log, n, W, H, W * H
        self.state = self.count_b = None
        self.last = self.f = None
        self.groups = []
        self.gnext = 1
        self.track_max, self.track_frames = track_max, track_frames
        self.frames = 0

    def process(self, f, hdr, state, count_b):
        """state: N bytes; count_b: 2N bytes (little-endian u16)."""
        self.frames += 1
        cnt = array('H')
        cnt.frombytes(count_b)
        if self.state is None:
            self.log('BASE n=%d f=%d last=%d counted=%d nonzero=%d' % (
                self.n, f, hdr['last'], sum(1 for v in cnt if v), sum(1 for v in state if v)))
            self.state, self.count_b, self.cnt, self.last, self.f = state, count_b, cnt, hdr['last'], f
            return
        if f > self.f + 1:
            self.log('SKIP n=%d f=%d prev=%d' % (self.n, f, self.f))
        last = hdr['last']
        if last != self.last:
            self.log('DEC n=%d f=%d last=%d step=%d' % (self.n, f, last, hdr['step']))
        ups, dns = [], []
        if count_b != self.count_b:
            for i in sorted(set(j >> 1 for j in diff_positions(self.count_b, count_b))):
                o, v = self.cnt[i], cnt[i]
                if o == 0 and v:
                    ups.append(i)
                elif o and v == 0:
                    dns.append(i)
        zeros = []
        if state != self.state:
            zeros = [i for i in diff_positions(self.state, state) if state[i] == 0 and self.state[i] != 0]
        for kind, idxs in (('CUP', ups), ('CDN', dns), ('ZERO', zeros)):
            if idxs:
                self.log('%s n=%d f=%d last=%d k=%d cells=%s' % (kind, self.n, f, last, len(idxs),
                                                                 spans_text(idxs, self.W)))
        if dns and len(dns) <= self.track_max:
            for comp in components(dns, self.W):
                ss = [state[i] for i in comp]
                g = {'g': self.gnext, 'f0': f, 'cells': comp, 'prev': None}
                self.gnext += 1
                self.groups.append(g)
                self.log('GRP n=%d g=%d f=%d k=%d smin=%d smax=%d cells=%s' % (
                    self.n, g['g'], f, len(comp), min(ss), max(ss), spans_text(comp, self.W)))
        keep = []
        for g in self.groups:
            ss = [state[i] for i in g['cells']]
            z = sum(1 for s in ss if s == 0)
            c = sum(1 for i in g['cells'] if cnt[i])
            cur = (min(ss), max(ss), z, c)
            if cur != g['prev']:
                g['prev'] = cur
                self.log('TRK n=%d g=%d f=%d last=%d smin=%d smax=%d zero=%d/%d counted=%d' % (
                    self.n, g['g'], f, last, cur[0], cur[1], z, len(ss), c))
            if z < len(ss) and f - g['f0'] <= self.track_frames:
                keep.append(g)
        self.groups = keep
        self.state, self.count_b, self.cnt, self.last, self.f = state, count_b, cnt, last, f


# ---- recorder --------------------------------------------------------------------------------------------------------

class Recorder:
    def __init__(self, a, proc, log):
        self.a, self.p, self.log = a, proc, log
        self.g = a1mem.Game(proc)
        self.n = 0
        self.mode_ptr = None
        self.frame = None
        self.grids = {}
        self.target = None
        self.fp = None
        self.next_find = 0.0
        self.next_obj = 0.0
        self.objs = {}
        self.records = set()
        self.src_seen = set()
        self.scan_thread = None
        self.next_scan = 0.0
        self.stats = {'modes': 0, 'scans': 0, 'records': 0, 'frames': 0, 'torn': 0}

    def _scan(self, n, frame, why):
        t = time.time()
        try:
            hits, st = self.p.scan([REC_NEEDLE] + list(SRC_MARKERS))
        except OSError as e:
            self.log('SCAN_ERROR n=%d frame=%s error=%s' % (n, frame, str(e).replace(' ', '_')))
            return
        new = 0
        for addr, text in a1mem.lua_records(hits[REC_NEEDLE], RECORD_RE):
            if text in self.records:
                continue
            self.records.add(text)
            new += 1
            self.log('LUA n=%d frame=%s addr=%s rec=%s' % (n, frame, hex(addr), text))
        for mk in SRC_MARKERS:
            if hits[mk] and (n, mk) not in self.src_seen:
                self.src_seen.add((n, mk))
                self.log('SRC n=%d frame=%s marker=%s copies=%d' % (n, frame, mk.decode(), len(hits[mk])))
        self.stats['scans'] += 1
        self.stats['records'] = len(self.records)
        self.log('SCAN n=%d frame=%s why=%s regions=%d mb=%.0f sec=%.2f records=%d' % (
            n, frame, why, st['regions'], st['bytes'] / 1e6, time.time() - t, new))

    def start_scan(self, why, wait=False):
        if self.scan_thread is not None and self.scan_thread.is_alive():
            if not wait:
                return
            self.scan_thread.join()
        self.scan_thread = threading.Thread(target=self._scan, args=(self.n, self.frame, why), daemon=True)
        self.scan_thread.start()
        if wait:
            self.scan_thread.join()

    def new_mode(self, m):
        self.n += 1
        self.stats['modes'] = self.n
        self.mode_ptr, self.frame = m['ptr'], m['frame']
        self.grids, self.target, self.fp, self.objs = {}, None, None, {}
        self.next_find = self.next_obj = self.next_scan = 0.0
        self.log('MODE n=%d ptr=%s type=%s game_type=%s stack=%s frame=%s gframe=%s' % (
            self.n, hex(m['ptr']), m['type'], m['game_type'], m['stack'], m['frame'], m['gframe']))
        pl = self.g.players()
        for pp in (pl or {}).get('by_id', []):
            self.log('P n=%d slot=%d id=%d index=%d human=%d faction=%s neutral=%s' % (
                self.n, pp['slot'], pp['id'], pp['index'], pp['human'], pp['faction'] or '?', pp['neutral']))
            if pp['human'] and self.target is None:
                self.target = pp['id']
        self.log('TARGET n=%d id=%s' % (self.n, self.target))

    def end_mode(self, why='mode_changed'):
        self.log('MODE_END n=%d last_frame=%s why=%s frames=%d' % (self.n, self.frame, why,
                                                                   self.fp.frames if self.fp else 0))
        self.start_scan('mode_end', wait=True)
        self.mode_ptr = None

    def poll_grid(self):
        now = time.time()
        if not self.grids:
            if now < self.next_find:
                return
            self.next_find = now + 0.25
            self.grids, how = find_grids(self.p, self.mode_ptr)
            if not self.grids:
                return
            self.log('GRIDS n=%d frame=%s via=%s ids=%s' % (self.n, self.frame, how,
                                                              ','.join(str(k) for k in sorted(self.grids))))
            for pid in sorted(self.grids):
                h = read_grid(self.p, self.grids[pid])
                self.log('GRID n=%d id=%d addr=%s W=%d H=%d N=%d cw=%r ch=%r x0=%r y0=%r x1=%r y1=%r last=%d '
                         'step=%d revall=%d' % (self.n, pid, hex(self.grids[pid]), h['W'], h['H'], h['N'], h['cw'],
                                                h['ch'], h['x0'], h['y0'], h['x1'], h['y1'], h['last'], h['step'],
                                                h['revall']))
            if self.target not in self.grids:
                self.log('NO_TARGET_GRID n=%d target=%s' % (self.n, self.target))
        if self.target not in self.grids:
            return
        f0 = self.frame
        if self.fp is not None and f0 == self.fp.f:
            return
        h = read_grid(self.p, self.grids[self.target])
        if not h:
            return
        cnt = self.p.read(h['count'], 2 * h['N'])
        st = self.p.read(h['state'], h['N'])
        f1 = self.p.u32(self.mode_ptr + a1mem.MODE_FRAME)
        if cnt is None or st is None or f1 != f0:
            self.stats['torn'] += 1
            return
        if self.fp is None:
            self.fp = FrameProcessor(self.log, self.n, h['W'], h['H'])
        self.fp.process(f0, h, st, cnt)
        self.stats['frames'] = self.fp.frames

    def poll_objects(self):
        now = time.time()
        if now < self.next_obj:
            return
        self.next_obj = now + 2.0
        objs = self.g.objects(self.mode_ptr)
        for oid, owner, tname, x, y, _z, flags, addr in objs or []:
            if tname not in PROBE_TYPES:
                continue
            old = self.objs.get(addr)
            if old is None or abs(old[0] - x) + abs(old[1] - y) > 5:
                self.objs[addr] = (x, y)
                self.log('POBJ n=%d f=%s obj=%d type=%s owner=%d x=%.2f y=%.2f flags=%d' % (
                    self.n, self.frame, oid, tname, owner, x, y, flags))

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
        self.poll_grid()
        self.poll_objects()
        if (self.frame or 0) >= 90 and time.time() >= self.next_scan:
            self.next_scan = time.time() + self.a.scan_sec
            self.start_scan('periodic')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--status')
    ap.add_argument('--wait-process-sec', type=float, default=900)
    ap.add_argument('--timeout-sec', type=float, default=14400)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--poll-sec', type=float, default=0.004)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    a = ap.parse_args()
    log = Log(a.out)
    log('FWREADER 1 scan_sec=%s poll_sec=%s' % (a.scan_sec, a.poll_sec))
    p = wait_for_game(a, log)
    if p is None:
        log('EXIT reason=no_game modes=0')
        return 2
    log('GAME pid=%d base=%s timestamp=%s size_of_image=%s identity=%s' % (
        p.pid, hex(p.base), hex(p.timestamp or 0), hex(p.size_of_image or 0),
        'ok' if p.timestamp == a1mem.FOC_TIMESTAMP else 'unknown'))
    r = Recorder(a, p, log)
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
        r.log('MODE_END n=%d last_frame=%s why=%s frames=%d' % (r.n, r.frame, reason, r.fp.frames if r.fp else 0))
    if r.scan_thread is not None:
        r.scan_thread.join(timeout=30)
    log('EXIT reason=%s modes=%d scans=%d records=%d frames=%d torn=%d failed_reads=%d' % (
        reason, r.n, r.stats['scans'], len(r.records), r.stats['frames'], r.stats['torn'], p.failed))
    if a.status:
        with open(a.status, 'w') as f:
            json.dump(dict(r.stats, state='done', reason=reason, reads=p.reads, failed_reads=p.failed), f)
    p.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
