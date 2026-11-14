#!/usr/bin/env python3
r"""slice_watch.py - read-only memory watcher for the a2m t07 Rebel slice run: what Lua cannot read.

  slice_watch.py run --run-dir DIR [--wait-process-sec S] [--poll-ms 4]

Waits for StarWarsG.exe (FoC 1.121, PE identity checked), then polls every few milliseconds, for every Rebel / Empire /
Underworld player of the galactic mode (ReadProcessMemory only: never writes, injects or suspends):
  PlayerClass +0x70 f32 credits, +0x84 i32 tech level, +0x88 i32 max tech (GALACTIC_ECONOMY 1.1, A2I_T01 / REMAINING_GAPS
  1.3), +0x1b0 the unlocked-type list, +0x1c8 the locked-type list (REMAINING_GAPS 1.3: the slice appends the sliced type
  to +0x1b0 with 0x27f860). Container: a DynamicVector of GameObjectTypeClass* {T** data, i32 count, i32 capacity}
  (run 20261108-212955: data 0x171d37600b0, count 11 -> 16, capacity 20; REMAINING_GAPS 1.5 "count +0x1b8"); the
  entries are named from the type's std::string at +0xF8 (q2_layout). A std::vector {begin, end, capacity} decode is
  kept as the fallback. The raw 0x18 bytes are logged too (decode_raw() re-decodes them offline: count, no names).
Output DIR\slice_watch.jsonl, one JSON object per line ("wall" seconds since start, "mf" mode frame, "mt" mode type,
"gf" global frame on every line):
  {"ev": "game", ...}                    the process was found
  {"ev": "player", "fac", "addr", ...}   a player was found (or its address changed)
  {"ev": "cr", "fac", "cr", "prev"}      the credits changed
  {"ev": "tech", "fac", "tech", "max", "prev"}
  {"ev": "list", "fac", "which": "unlocked" | "locked", "n", "added": [...], "removed": [...], "raw", "ok"}
  {"ev": "beat", ...}                    every 10 s: mode frame, credits and tech of every watched player
  {"ev": "exit", ...}
DIR\slice_live.json (rewritten every second): the mode frame, the fiscal cycle (mode frame / 1350) and the watched
players' credits / tech / unlocked count, for whoever drives the game.
"""
import argparse
import json
import os
import struct
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'a1_tactical'))

P_CREDITS = 0x70
P_TECH = 0x84
P_MAXTECH = 0x88
LISTS = (('unlocked', 0x1B0), ('locked', 0x1C8))
P_READ = 0x1E0
WATCH = ('REBEL', 'EMPIRE', 'UNDERWORLD')
USER_LO, USER_HI = 0x10000, 0x7FFFFFFF0000
DAY_FRAMES = 1350


def plausible(p):
    return USER_LO <= p < USER_HI and p % 8 == 0


def decode_raw(raw):
    """(layout, data pointer, count) of a list's 0x18 raw bytes, or (None, None, None).
    'dv' = DynamicVector {T** data, i32 count, i32 capacity} (what FoC 1.121 has), 'sv' = std::vector {begin, end, cap}."""
    if len(raw) < 0x18:
        return None, None, None
    d, n, cap = struct.unpack_from('<Qii', raw, 0)
    if n == 0 and cap == 0 and d == 0:
        return 'dv', 0, 0
    if 0 <= n <= cap <= 4096 and plausible(d):
        return 'dv', d, n
    b, e, c = struct.unpack_from('<QQQ', raw, 0)
    if b == 0 and e == 0:
        return 'sv', 0, 0
    if plausible(b) and b <= e <= c and plausible(c) and (e - b) % 8 == 0 and (e - b) // 8 <= 512:
        return 'sv', b, (e - b) // 8
    return None, None, None


class Watcher:
    """proc: read(addr, n), u64(addr); game: players() / mode() / type_name(ptr) (a1mem.Game)."""

    def __init__(self, proc, game, out, live_path=None, clock=time.time):
        self.p = proc
        self.g = game
        self.out = out
        self.live_path = live_path
        self.clock = clock
        self.t0 = clock()
        self.players = {}     # fac -> addr
        self.state = {}       # fac -> {'cr', 'tech', 'max', 'raw': {which: bytes}, 'names': {which: [..]}}
        self.lines = 0
        self.last_beat = -1e9
        self.last_live = -1e9
        self.mf = self.gf = self.mt = None

    def emit(self, d):
        d['wall'] = round(self.clock() - self.t0, 3)
        d['mf'], d['gf'], d['mt'] = self.mf, self.gf, self.mt
        self.out.write(json.dumps(d, sort_keys=True) + '\n')
        self.out.flush()
        self.lines += 1

    def frames(self):
        m = self.g.mode() or {}
        self.mf, self.gf, self.mt = m.get('frame'), m.get('gframe'), m.get('type')

    def resolve(self):
        pls = self.g.players() or {}
        if isinstance(pls, dict):
            pls = pls.get('by_id') or []
        for pl in pls:
            fac = pl.get('faction')
            if fac not in WATCH:
                continue
            if self.players.get(fac) != pl['addr']:
                self.players[fac] = pl['addr']
                self.state.pop(fac, None)
                self.emit({'ev': 'player', 'fac': fac, 'addr': hex(pl['addr']), 'human': pl.get('human'),
                           'id': pl.get('id')})

    def decode_list(self, raw):
        """(ok, [type names]) of a DynamicVector (or std::vector) of type pointers (decode_raw)."""
        layout, b, n = decode_raw(raw)
        if layout is None:
            return False, []
        if n == 0:
            return True, []
        data = self.p.read(b, 8 * n)
        if data is None:
            return False, []
        names = []
        for i in range(n):
            tp, = struct.unpack_from('<Q', data, 8 * i)
            names.append(self.g.type_name(tp) if plausible(tp) else '?%x' % tp)
        return True, names

    def poll(self):
        n0 = self.lines
        self.frames()
        for fac, addr in self.players.items():
            pb = self.p.read(addr, P_READ)
            if not pb:
                continue
            cr, = struct.unpack_from('<f', pb, P_CREDITS)
            tech, mx = struct.unpack_from('<ii', pb, P_TECH)
            if not (0 <= tech <= 16 and 0 <= mx <= 16):
                continue   # stale player address (freed at a mode change, before resolve() sees the new player)
            s = self.state.get(fac)
            if s is None:
                s = self.state[fac] = {'cr': None, 'tech': None, 'max': None, 'raw': {}, 'names': {}}
            if s['cr'] != cr:
                self.emit({'ev': 'cr', 'fac': fac, 'cr': round(cr, 3), 'prev': None if s['cr'] is None else round(s['cr'], 3)})
                s['cr'] = cr
            if s['tech'] != tech or s['max'] != mx:
                self.emit({'ev': 'tech', 'fac': fac, 'tech': tech, 'max': mx, 'prev': s['tech']})
                s['tech'], s['max'] = tech, mx
            for which, off in LISTS:
                raw = pb[off:off + 0x18]
                if s['raw'].get(which) == raw:
                    continue
                ok, names = self.decode_list(raw)
                old = s['names'].get(which, [])
                added = [x for x in names if x not in old]
                removed = [x for x in old if x not in names]
                self.emit({'ev': 'list', 'fac': fac, 'which': which, 'n': len(names), 'ok': ok, 'added': added,
                           'removed': removed, 'raw': raw.hex(), 'all': names if len(names) <= 64 else names[:64]})
                s['raw'][which] = raw
                s['names'][which] = names
        now = self.clock() - self.t0
        if now - self.last_beat >= 10.0:
            self.last_beat = now
            self.emit({'ev': 'beat', 'players': {f: {'cr': None if v['cr'] is None else round(v['cr'], 3),
                                                     'tech': v['tech'], 'unlocked': len(v['names'].get('unlocked', []))}
                                                 for f, v in self.state.items()}})
        if self.live_path and now - self.last_live >= 1.0:
            self.last_live = now
            live = {'mode_frame': self.mf, 'mode_type': self.mt, 'cycle': (self.mf // DAY_FRAMES) if self.mf else None,
                    'wall': round(now, 1), 'time': time.strftime('%H:%M:%S'),
                    'players': {f: {'cr': None if v['cr'] is None else round(v['cr'], 1), 'tech': v['tech'],
                                    'unlocked': len(v['names'].get('unlocked', []))} for f, v in self.state.items()}}
            tmp = self.live_path + '.tmp'
            with open(tmp, 'w') as f:
                json.dump(live, f)
            os.replace(tmp, self.live_path)
        return self.lines - n0

    def run(self, alive, poll_s=0.004, resolve_every=1.0):
        nxt = 0.0
        while alive():
            now = self.clock() - self.t0
            if now >= nxt:
                try:
                    self.resolve()
                except Exception as e:  # noqa: BLE001 - a failed walk must not end the watcher
                    self.emit({'ev': 'error', 'where': 'resolve', 'msg': repr(e)})
                nxt = now + resolve_every
            try:
                self.poll()
            except Exception as e:  # noqa: BLE001
                self.emit({'ev': 'error', 'where': 'poll', 'msg': repr(e)})
                time.sleep(0.5)
            time.sleep(poll_s)


def load(path):
    out = []
    if not os.path.isfile(path):
        return out
    for line in open(path, encoding='ascii', errors='replace'):
        line = line.strip()
        if line.startswith('{'):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def run(a):
    import a1mem  # noqa: E402 - Windows-only ctypes
    os.makedirs(a.run_dir, exist_ok=True)
    out = open(os.path.join(a.run_dir, 'slice_watch.jsonl'), 'a')
    t0 = time.time()
    proc = None
    while proc is None and time.time() - t0 < a.wait_process_sec:
        for pid in a1mem.find_pids():
            try:
                q = a1mem.Proc(pid)
            except OSError:
                continue
            if q.pe_identity():
                proc = q
                break
            q.close()
        if proc is None:
            time.sleep(1.0)
    if proc is None:
        out.write(json.dumps({'ev': 'exit', 'reason': 'no_game'}) + '\n')
        return 2
    w = Watcher(proc, a1mem.Game(proc), out, os.path.join(a.run_dir, 'slice_live.json'))
    w.emit({'ev': 'game', 'pid': proc.pid, 'base': hex(proc.base)})
    try:
        w.run(proc.alive, poll_s=a.poll_ms / 1000.0)
        reason = 'game_exited'
    except KeyboardInterrupt:
        reason = 'interrupted'
    w.emit({'ev': 'exit', 'reason': reason, 'lines': w.lines, 'reads': proc.reads, 'failed': proc.failed})
    proc.close()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    r = sub.add_parser('run')
    r.add_argument('--run-dir', required=True)
    r.add_argument('--wait-process-sec', type=float, default=1800)
    r.add_argument('--poll-ms', type=float, default=4.0)
    a = ap.parse_args()
    if a.cmd == 'run':
        return run(a)
    ap.print_help()
    return 1


if __name__ == '__main__':
    sys.exit(main())
