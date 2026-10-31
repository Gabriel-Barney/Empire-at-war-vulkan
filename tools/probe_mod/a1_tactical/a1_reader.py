#!/usr/bin/env python3
"""a1_reader.py - read-only recorder for the A1 tactical-AI probes (FoC Steam x64 1.121). Both probes share it.

  a1_reader.py --out LOG [--status JSON] [--caps CAPS.json] [--wait-process-sec 900] [--timeout-sec 14400]
               [--scan-sec 5] [--first-scan-frame 150] [--allow-unknown-exe] [--pid N --test-base 0xADDR]

It waits for StarWarsG.exe (FoC 1.121: PE timestamp / image size checked; any other exe is reported and ignored),
then for every tactical mode (land or space) it records:
  * landing-point probe: every REINFORCEMENT_POINT* object (first sight with owner and position, then every owner
    change with the mode frame current when the poll saw it; polls run every --poll-sec, several per frame), every
    player's land population capacity (player +0x404) at the first frame and at every change, and per player the
    capacity computed from the objects it holds (sum of the types' Additional_Population_Capacity from --caps, alive
    objects only) whenever that sum changes (full store pass every 0.5 s and right after any owner / +0x404 change);
  * free-store probe: the probe Lua's records ("A1P1|...|END" strings kept alive by the hooked
    BusyTacticalFreeStore.lua), found by scanning the game's private heap every --scan-sec seconds (in a second
    thread, so the frame-by-frame polling goes on) and once more when the mode ends.
It never writes the game's memory. It ends when the game exits (or --timeout-sec).

Log lines (one event per line, key=value):
  A1READER 1 ...                         start; GAME pid= base= timestamp= size_of_image= identity=ok|unknown
  MODE n= ptr= type=1|2 game_type= stack= frame= gframe= t=        a tactical mode became current
  PLAYERS n= frame= count= vec=<idx:id,...>   then  P n= frame= slot= id= index= human= faction= neutral= popcap=
  PCAP n= frame= id= popcap=<old>-><new>
  POINT n= frame= obj= type= owner= x= y= addcap=      first sight of a reinforcement point
  POWN n= frame= obj= type= owner=<old>-><new>
  CALC n= frame= id= calc=<sum> objects=<alive count>  (on change)
  OWNERS n= frame= id= count= top=<TYPE:k,...>          at the first traversal with objects and at frame >= 300
  SCAN n= frame= regions= mb= sec= records=<new>        LUA n= frame= addr= rec=<A1P1|...|END>   SRC n= frame= marker=
  MODE_END n= last_frame=       EXIT reason= modes= scans= records=
"""
import argparse
import json
import os
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a1mem  # noqa: E402

SRC_MARKERS = (b'A1PROBE_SRC_MARK_FREESTORE_V1',)
REC_NEEDLE = b'A1P1|'


class Log:
    def __init__(self, path):
        self.f = open(path, 'a', encoding='ascii', errors='replace')
        self.lock = threading.Lock()
        self.t0 = time.time()

    def __call__(self, line):
        with self.lock:
            self.f.write(line + '\n')
            self.f.flush()


def kv(**k):
    return ' '.join('%s=%s' % (a, b) for a, b in k.items())


class Recorder:
    def __init__(self, a, proc, log):
        self.a, self.p, self.log = a, proc, log
        self.g = a1mem.Game(proc)
        self.caps = {}
        if a.caps and os.path.exists(a.caps):
            self.caps = {k.upper(): v for k, v in json.load(open(a.caps)).items()}
        self.n = 0
        self.mode_ptr = None
        self.mode_type = None
        self.frame = None
        self.points = {}      # obj addr -> [obj id, type, owner]
        self.popcap = {}      # player id -> popcap
        self.calc = {}        # player id -> calc
        self.owners_logged = set()
        self.last_full = 0.0
        self.records = set()
        self.src_seen = set()
        self.scans = 0
        self.scan_thread = None
        self.next_scan = 0.0
        self.stats = {'modes': 0, 'scans': 0, 'records': 0, 'points': 0, 'owner_changes': 0, 'popcap_changes': 0}

    # ---- heap scan (thread) -----------------------------------------------------------------------------------
    def _scan(self, n, frame, why):
        t = time.time()
        try:
            hits, st = self.p.scan([REC_NEEDLE] + list(SRC_MARKERS))
        except OSError as e:
            self.log('SCAN_ERROR ' + kv(n=n, frame=frame, error=str(e).replace(' ', '_')))
            return
        new = 0
        for addr, text in a1mem.lua_records(hits[REC_NEEDLE]):
            if text in self.records:
                continue
            self.records.add(text)
            new += 1
            self.log('LUA ' + kv(n=n, frame=frame, addr=hex(addr), rec=text))
        for m in SRC_MARKERS:
            if hits[m] and (n, m) not in self.src_seen:
                self.src_seen.add((n, m))
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

    # ---- per-mode polling ---------------------------------------------------------------------------------------
    def new_mode(self, m):
        self.n += 1
        self.stats['modes'] = self.n
        self.mode_ptr, self.mode_type, self.frame = m['ptr'], m['type'], m['frame']
        self.points, self.popcap, self.calc, self.owners_logged = {}, {}, {}, set()
        self.last_full = 0.0
        self.next_scan = 0.0
        self.log('MODE ' + kv(n=self.n, ptr=hex(m['ptr']), type=m['type'], game_type=m['game_type'], stack=m['stack'],
                              frame=m['frame'], gframe=m['gframe'], t='%.1f' % (time.time() - self.log.t0)))
        self.poll_players(first=True)

    def end_mode(self):
        self.log('MODE_END ' + kv(n=self.n, last_frame=self.frame))
        self.start_scan('mode_end', wait=True)
        self.mode_ptr = None

    def poll_players(self, first=False):
        pl = self.g.players()
        if not pl:
            return
        if first:
            self.log('PLAYERS ' + kv(n=self.n, frame=self.frame, count=pl['count'],
                                     vec=','.join('%d:%d' % v for v in pl['vec']) or '-'))
            for p in pl['by_id']:
                self.log('P ' + kv(n=self.n, frame=self.frame, slot=p['slot'], id=p['id'], index=p['index'],
                                   human=p['human'], faction=p['faction'] or '?', neutral=p['neutral'],
                                   popcap=p['popcap']))
                self.popcap[p['id']] = p['popcap']
            return
        for p in pl['by_id']:
            old = self.popcap.get(p['id'])
            if old != p['popcap']:
                self.popcap[p['id']] = p['popcap']
                self.stats['popcap_changes'] += 1
                self.log('PCAP ' + kv(n=self.n, frame=self.frame, id=p['id'], popcap='%s->%s' % (old, p['popcap'])))

    def poll_points(self):
        for addr, rec in self.points.items():
            b = self.p.read(addr + a1mem.OBJ_OWNER, 4)
            if not b:
                continue
            owner = int.from_bytes(b, 'little', signed=True)
            if owner != rec[2]:
                self.stats['owner_changes'] += 1
                self.log('POWN ' + kv(n=self.n, frame=self.frame, obj=rec[0], type=rec[1],
                                      owner='%d->%d' % (rec[2], owner)))
                rec[2] = owner

    def full_pass(self):
        objs = self.g.objects(self.mode_ptr)
        if objs is None:
            return
        calc, count, types = {}, {}, {}
        for oid, owner, tname, x, y, _z, flags, addr in objs:
            alive = (flags & 0x42) == 0
            if 'REINFORCEMENT_POINT' in tname and addr not in self.points:
                self.points[addr] = [oid, tname, owner]
                self.stats['points'] += 1
                self.log('POINT ' + kv(n=self.n, frame=self.frame, obj=oid, type=tname, owner=owner, x='%.0f' % x,
                                       y='%.0f' % y, addcap=self.caps.get(tname, '?'), alive=int(alive)))
            if not alive:
                continue
            calc[owner] = calc.get(owner, 0) + int(self.caps.get(tname, 0) or 0)
            count[owner] = count.get(owner, 0) + 1
            types.setdefault(owner, {})[tname] = types.get(owner, {}).get(tname, 0) + 1
        for pid in sorted(set(calc) | set(self.calc)):
            if calc.get(pid, 0) != self.calc.get(pid):
                self.calc[pid] = calc.get(pid, 0)
                self.log('CALC ' + kv(n=self.n, frame=self.frame, id=pid, calc=calc.get(pid, 0),
                                      objects=count.get(pid, 0)))
        tag = 'f300' if (self.frame or 0) >= 300 else 'first'
        if objs and tag not in self.owners_logged:
            self.owners_logged.add(tag)
            for pid in sorted(count):
                top = sorted(types[pid].items(), key=lambda kv_: (-kv_[1], kv_[0]))[:5]
                self.log('OWNERS ' + kv(n=self.n, frame=self.frame, id=pid, count=count[pid],
                                        top=','.join('%s:%d' % t for t in top)))

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
        changes = self.stats['owner_changes'] + self.stats['popcap_changes']
        self.poll_points()  # every poll, not only on a new frame: a change is logged with the frame it was seen in
        self.poll_players()
        now = time.time()
        if now - self.last_full >= 0.5 or changes != self.stats['owner_changes'] + self.stats['popcap_changes']:
            self.last_full = now
            self.full_pass()
        if (self.frame or 0) >= self.a.first_scan_frame and now >= self.next_scan:
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--status')
    ap.add_argument('--caps')
    ap.add_argument('--wait-process-sec', type=float, default=900)
    ap.add_argument('--timeout-sec', type=float, default=14400)
    ap.add_argument('--scan-sec', type=float, default=5.0)
    ap.add_argument('--first-scan-frame', type=int, default=150)
    ap.add_argument('--poll-sec', type=float, default=0.004)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--test-base', help='hex base of the fake game image (selftest)')
    a = ap.parse_args()
    log = Log(a.out)
    log('A1READER 1 ' + kv(scan_sec=a.scan_sec, first_scan_frame=a.first_scan_frame, caps=len(json.load(
        open(a.caps))) if a.caps and os.path.exists(a.caps) else 0))
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
                st = dict(r.stats, state='running', mode=r.n, frame=r.frame, reads=p.reads, failed_reads=p.failed,
                          time=time.strftime('%H:%M:%S'))
                with open(a.status, 'w') as f:
                    json.dump(st, f)
            time.sleep(a.poll_sec)
    except KeyboardInterrupt:
        reason = 'interrupted'
    if r.mode_ptr is not None:
        r.log('MODE_END ' + kv(n=r.n, last_frame=r.frame, why=reason))
    if r.scan_thread is not None:
        r.scan_thread.join(timeout=30)
    log('EXIT ' + kv(reason=reason, modes=r.n, scans=r.scans, records=len(r.records), points=r.stats['points'],
                     failed_reads=p.failed))
    if a.status:
        with open(a.status, 'w') as f:
            json.dump(dict(r.stats, state='done', reason=reason, reads=p.reads, failed_reads=p.failed), f)
    p.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
