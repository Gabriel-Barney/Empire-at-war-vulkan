#!/usr/bin/env python3
"""tswp_reader.py - read-only recorder for the TSW planet-destroy probe (FoC Steam x64 1.121).

  tswp_reader.py --out LOG [--wait-process-sec 900] [--scan-sec 2] [--poll-sec 0.002] [--allow-unknown-exe]

Two channels (a1_tactical/a1mem.py: ReadProcessMemory only, never writes, never injects):
  * Lua records. Retail FoC shows no _MessagePopup boxes (2026-10-31 run), so the probe Lua (GameScoring hook and
    galactic story) keeps every event as a Lua string "TSWP1|<KIND>|<seq>|<text>|END"; a second thread scans the game's
    committed private memory for them every --scan-sec seconds.
  * Tactical object store. GameScoring's GameService is only called from its main() loop, which retail never runs
    (2026-10-31: LOAD and BASEDEF records, no service call at ServiceRate 0 or 0.0333), so the hook cannot stamp
    frames. Instead, in every tactical mode after the first galactic mode, the main loop polls the mode frame every
    --poll-sec and on each new frame walks the mode's object store (a1mem.Game.objects) and logs every object that
    appeared (NEW), vanished from the store (GONE) or changed its flags byte (FLAGS; alive = flags & 0x42 == 0), with
    the frame current when the walk saw it (a change made during frame f's update is seen at f or f+1).
It ends when the game exits.

Log lines: TSWPREADER 1 | GAME pid= base= identity= | MODE ptr= type= frame= | REC frame= rec=<TSWP1|...|END> |
           NEW/GONE/FLAGS mode= frame= obj= id= type= owner= [flags=] | STORE mode= frame= count= (first walk) |
           SCAN ... | EXIT reason= scans= records= walks=
"""
import argparse
import os
import re
import sys
import threading
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
import a1mem  # noqa: E402

NEEDLE = b'TSWP1|'
# a1mem.Proc.scan returns 400 bytes per hit: records stay under that
RECORD_RE = re.compile(rb'TSWP1\|([A-Z]+)\|([0-9]+)\|([ -~]{0,385}?)\|END')


def records(hits):
    out = []
    for addr, tail in hits:
        m = RECORD_RE.match(tail)
        if m:
            out.append((addr, m.group(0).decode('ascii')))
    return out


class Log:
    def __init__(self, path):
        self.f = open(path, 'a', encoding='ascii', errors='replace')
        self.lock = threading.Lock()
        self.t0 = time.time()

    def __call__(self, s):
        with self.lock:
            self.f.write('%8.2f %s\n' % (time.time() - self.t0, s))
            self.f.flush()


class Scanner(threading.Thread):
    def __init__(self, p, log, scan_sec, state):
        super().__init__(daemon=True)
        self.p, self.log, self.scan_sec, self.state = p, log, scan_sec, state
        self.seen = set()
        self.scans = 0
        self.stop = False

    def scan_once(self):
        ts = time.time()
        try:
            hits, st = self.p.scan([NEEDLE])
        except OSError as e:
            self.log('SCAN_ERROR %s' % e)
            return
        self.scans += 1
        new = 0
        for addr, text in records(hits[NEEDLE]):
            if text in self.seen:
                continue
            self.seen.add(text)
            new += 1
            self.log('REC frame=%s addr=%s rec=%s' % (self.state.get('frame'), hex(addr), text))
        if self.scans % 30 == 1:
            self.log('SCAN n=%d mb=%.0f sec=%.2f new=%d' % (self.scans, st.get('bytes', 0) / 1e6, time.time() - ts, new))

    def run(self):
        while not self.stop and self.p.alive():
            self.scan_once()
            time.sleep(self.scan_sec)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--wait-process-sec', type=float, default=900)
    ap.add_argument('--scan-sec', type=float, default=2.0)
    ap.add_argument('--poll-sec', type=float, default=0.002)
    ap.add_argument('--allow-unknown-exe', action='store_true')
    ap.add_argument('--attach-in-battle', action='store_true', help='reader restarted during a battle: track it')
    a = ap.parse_args()
    log = Log(a.out)
    log('TSWPREADER 2 scan_sec=%s poll_sec=%s' % (a.scan_sec, a.poll_sec))
    t0 = time.time()
    p = None
    while p is None and time.time() - t0 < a.wait_process_sec:
        for pid in a1mem.find_pids():
            try:
                q = a1mem.Proc(pid)
            except OSError as e:
                log('OPEN_FAILED pid=%d error=%s' % (pid, e))
                continue
            if q.pe_identity() or a.allow_unknown_exe:
                p = q
                break
            q.close()
        if p is None:
            time.sleep(1.0)
    if p is None:
        log('EXIT reason=no_game scans=0 records=0 walks=0')
        return 2
    log('GAME pid=%d base=%s identity=%s' % (p.pid, hex(p.base), 'ok' if p.pe_identity() else 'unknown'))
    g = a1mem.Game(p)
    state = {'frame': None}
    sc = Scanner(p, log, a.scan_sec, state)
    sc.start()
    last_mode = None
    galactic_seen = a.attach_in_battle
    n_mode = 0
    store = {}       # obj addr -> [id, type, owner, flags]
    tsw = {}         # Death Star obj addr -> last logged state key
    last_frame = None
    walks = 0
    reason = 'game_exited'
    try:
        while p.alive():
            m = g.mode()
            if not m:
                time.sleep(a.poll_sec)
                continue
            state['frame'] = m.get('frame')
            key = m.get('ptr')
            if key != last_mode:
                last_mode = key
                n_mode += 1
                store, last_frame, tsw = {}, None, {}
                log('MODE n=%d ptr=%s type=%s game_type=%s frame=%s' % (
                    n_mode, hex(key) if key else None, m.get('type'), m.get('game_type'), m.get('frame')))
                if m.get('type') == 0:
                    galactic_seen = True
            tactical = galactic_seen and m.get('type') in (1, 2) and key
            frame = m.get('frame')
            if tactical and frame is not None and frame != last_frame:
                last_frame = frame
                objs = g.objects(key)
                if objs is not None:
                    walks += 1
                    first = not store
                    now = {}
                    for oid, owner, tname, _x, _y, _z, flags, addr in objs:
                        now[addr] = [oid, tname, owner, flags]
                        old = store.get(addr)
                        if first:
                            continue
                        if old is None:
                            log('NEW mode=%d frame=%d obj=%s id=%d type=%s owner=%d flags=0x%02x' % (
                                n_mode, frame, hex(addr), oid, tname, owner, flags))
                        elif old[3] != flags:
                            log('FLAGS mode=%d frame=%d obj=%s id=%d type=%s owner=%d flags=0x%02x->0x%02x' % (
                                n_mode, frame, hex(addr), oid, tname, owner, old[3], flags))
                    if first:
                        log('STORE mode=%d frame=%d count=%d types=%s' % (n_mode, frame, len(now), ','.join(
                            sorted({v[1] for v in now.values()}))))
                    else:
                        for addr, old in store.items():
                            if addr not in now:
                                log('GONE mode=%d frame=%d obj=%s id=%d type=%s owner=%d' % (
                                    n_mode, frame, hex(addr), old[0], old[1], old[2]))
                    store = now
                    # TSW state (SCRIPT_GAPS sec. 1.2: data pack obj +0x148, state +0x20, 4 = ready; countdown +0x30;
                    # TSW_PLANET_DESTROY sec. 3: 5 on Fire ... 13). +0x148 read as a pointer and, in case the pack is
                    # inline, as the pack itself; both are logged on change so the run shows which one is right.
                    for addr, v in now.items():
                        if 'DEATH_STAR' not in v[1]:
                            continue
                        ptr = p.u64(addr + 0x148)
                        cand = []
                        for base in ((ptr,) if ptr and ptr > 0x10000 else ()) + (addr + 0x148,):
                            s = p.i32(base + 0x20)
                            c = p.i32(base + 0x30)
                            cand.append('%s:%s/%s' % ('ptr' if base == ptr else 'inl', s, c))
                        st = ' '.join(cand)
                        stkey = ' '.join(x.split('/')[0] for x in cand)
                        if tsw.get(addr) != stkey:
                            tsw[addr] = stkey
                            log('TSW mode=%d frame=%d obj=%s type=%s state=%s' % (n_mode, frame, hex(addr), v[1], st))
            time.sleep(a.poll_sec)
    except KeyboardInterrupt:
        reason = 'interrupted'
    sc.stop = True
    sc.join(timeout=30)
    log('EXIT reason=%s scans=%d records=%d walks=%d' % (reason, sc.scans, len(sc.seen), walks))
    return 0


if __name__ == '__main__':
    sys.exit(main())
