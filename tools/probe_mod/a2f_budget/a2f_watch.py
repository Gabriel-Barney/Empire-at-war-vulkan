#!/usr/bin/env python3
"""a2f_watch.py - read-only watcher for the AI budget block (the part of the a2f t07 probe Lua cannot see).

The budget-wide fields of AIBudgetClass (docs/re/AI_GALACTIC_2.md section 3) are not reachable from Lua: the reset flag
+0x74 in particular has no query. They are plain fields of a live object, so this watcher reads them from the running
game with ReadProcessMemory (never writes, never injects, never suspends):

  PlayerClass +0x360                 AIPlayerClass* (null for a human; A2GAL_READINGS 1.1, A1_READINGS 38.2)
  AIPlayerClass +0x38 / +0x40        begin / end of the vector of per-mode contexts (AI_GALACTIC 1.1)
  context +0x34 mode (0 galactic), +0x30 opponent index, +0x40 goal system*, +0x69 initialised byte
  goal system +0x190 = the budget (A2GAL_READINGS 1.1: "the budget is context +0x40, +400")
  budget +0x68 income per second f32, +0x70 last tick period T f32, +0x74 reset flag, +0x80 refund accumulator f32
  (AI_GALACTIC_2 section 3, [H])

A poll takes a few small reads (a few milliseconds at most); the loop runs about every 3 ms, far faster than the 30 Hz
logic frame, so a flag that is set for even one frame is seen.  Output lines (JSON, one per line) in budget_watch.jsonl:
  {"ev": "found", ...}   a galactic context with a plausible budget was found for a faction (pointers, init byte)
  {"ev": "state", ...}   first sight, every change of init byte / income / T / flag / refund, and a heartbeat every 5 s
  {"ev": "flag", ...}    the reset flag changed (from, to) - the setter's moment, with the mode frame
  {"ev": "chain", ...}   the chain could not be followed (what failed), once per reason
Every line carries wall (seconds since start), mf (mode frame) and gf (global logic frame).  The raw 0x90 bytes of the
budget block are in "raw" (hex) for offline decoding.  Mode frame values come from a1_tactical/a1mem.py (MAIN_LOOP.md
statics).
"""
import json
import struct
import time

P_AI = 0x360
AI_VEC_BEGIN = 0x38
AI_VEC_END = 0x40
CTX_OPP = 0x30
CTX_MODE = 0x34
CTX_GOAL = 0x40
CTX_INIT = 0x69
GOAL_BUDGET = 0x190
BLOCK = 0x90
B_INCOME = 0x68
B_T = 0x70
B_FLAG = 0x74
B_REFUND = 0x80

USER_LO, USER_HI = 0x10000, 0x7FFFFFFF0000


def plausible(p):
    return USER_LO <= p < USER_HI and p % 8 == 0


class Watcher:
    """proc: needs read(addr, n) -> bytes | None, u64(addr). game: needs players() and mode() (a1mem.Game)."""

    def __init__(self, proc, game, out, clock=time.time):
        self.p = proc
        self.g = game
        self.out = out
        self.clock = clock
        self.t0 = clock()
        self.targets = {}        # faction -> {'budget': addr, 'ctx': addr, ...}
        self.last = {}           # faction -> last state tuple
        self.last_beat = {}      # faction -> wall of the last heartbeat
        self.reasons = set()
        self.flags = {}          # faction -> list of (wall, mf, from, to)
        self.lines = 0

    def emit(self, d):
        d['wall'] = round(self.clock() - self.t0, 3)
        self.out.write(json.dumps(d, sort_keys=True) + '\n')
        self.out.flush()
        self.lines += 1

    def chain_fail(self, fac, why):
        key = (fac, why)
        if key not in self.reasons:
            self.reasons.add(key)
            self.emit({'ev': 'chain', 'fac': fac, 'why': why})

    def frames(self):
        m = self.g.mode() or {}
        return m.get('frame'), m.get('gframe'), m.get('type')

    def resolve(self):
        """Re-walks player -> AI -> galactic context -> budget for every AI player. Returns the targets dict."""
        pls = self.g.players() or []
        if isinstance(pls, dict):             # a1mem.Game.players() returns {'by_id': [...], 'count', 'vec'}
            pls = pls.get('by_id') or []
        found = {}
        for pl in pls:
            fac = pl.get('faction') or ('SLOT%d' % pl.get('slot', -1))
            if pl.get('human'):
                continue
            ai = self.p.u64(pl['addr'] + P_AI)
            if not ai:
                continue
            if not plausible(ai):
                self.chain_fail(fac, 'AI pointer %#x not plausible' % ai)
                continue
            vb = self.p.u64(ai + AI_VEC_BEGIN)
            ve = self.p.u64(ai + AI_VEC_END)
            if not vb or not ve or ve < vb or (ve - vb) % 8 or (ve - vb) > 8 * 8:
                self.chain_fail(fac, 'context vector %#x..%#x not plausible (AI %#x)' % (vb or 0, ve or 0, ai))
                continue
            ctx = None
            for i in range((ve - vb) // 8):
                c = self.p.u64(vb + 8 * i)
                if not c or not plausible(c):
                    continue
                cb = self.p.read(c + CTX_OPP, 0x40)
                if not cb:
                    continue
                mode = struct.unpack_from('<i', cb, CTX_MODE - CTX_OPP)[0]
                if mode == 0:
                    ctx = c
                    break
            if ctx is None:
                continue                      # no galactic context yet (menu, or the galaxy is still loading)
            cb = self.p.read(ctx, 0x70)
            if not cb:
                continue
            goal = struct.unpack_from('<Q', cb, CTX_GOAL)[0]
            init = cb[CTX_INIT]
            if not plausible(goal):
                self.chain_fail(fac, 'goal system pointer %#x not plausible (context %#x)' % (goal, ctx))
                continue
            found[fac] = {'budget': goal + GOAL_BUDGET, 'ctx': ctx, 'goal': goal, 'ai': ai, 'player': pl['addr']}
            if fac not in self.targets or self.targets[fac]['budget'] != found[fac]['budget']:
                mf, gf, _ = self.frames()
                self.emit({'ev': 'found', 'fac': fac, 'mf': mf, 'gf': gf, 'init': init,
                           'player': hex(pl['addr']), 'ai': hex(ai), 'ctx': hex(ctx), 'goal': hex(goal),
                           'budget': hex(goal + GOAL_BUDGET)})
        self.targets = found
        return found

    @staticmethod
    def decode(block):
        inc, = struct.unpack_from('<f', block, B_INCOME)
        t, = struct.unpack_from('<f', block, B_T)
        flag, = struct.unpack_from('<I', block, B_FLAG)
        refund, = struct.unpack_from('<f', block, B_REFUND)
        return inc, t, flag, refund

    def poll(self):
        """One pass over the known targets. Returns the number of lines written."""
        n0 = self.lines
        now = self.clock() - self.t0
        for fac, t in self.targets.items():
            block = self.p.read(t['budget'], BLOCK)
            ib = self.p.read(t['ctx'] + CTX_INIT, 1)
            if not block or not ib:
                self.chain_fail(fac, 'budget block unreadable at %#x' % t['budget'])
                continue
            inc, per, flag, refund = self.decode(block)
            init = ib[0]
            state = (init, inc, per, flag, refund)
            prev = self.last.get(fac)
            beat = now - self.last_beat.get(fac, -1e9) >= 5.0
            if prev == state and not beat:
                continue
            mf, gf, mt = self.frames()
            if prev is not None and prev[3] != flag:
                self.flags.setdefault(fac, []).append((now, mf, prev[3], flag))
                self.emit({'ev': 'flag', 'fac': fac, 'mf': mf, 'gf': gf, 'from': prev[3], 'to': flag})
            self.emit({'ev': 'state', 'fac': fac, 'mf': mf, 'gf': gf, 'mode': mt, 'init': init,
                       'income_per_s': inc, 'T': per, 'flag': flag, 'refund': refund,
                       'changed': prev is None or prev != state, 'raw': block.hex()})
            self.last[fac] = state
            self.last_beat[fac] = now
        return self.lines - n0

    def run(self, alive, sleep=0.003, resolve_every=1.0):
        """Poll until alive() is false."""
        next_resolve = 0.0
        while alive():
            now = self.clock() - self.t0
            if now >= next_resolve:
                try:
                    self.resolve()
                except Exception as e:  # noqa: BLE001 - a failed walk must not end the watcher
                    self.chain_fail('*', 'resolve raised %r' % (e,))
                next_resolve = now + resolve_every
            try:
                self.poll()
            except Exception as e:  # noqa: BLE001
                self.chain_fail('*', 'poll raised %r' % (e,))
            time.sleep(sleep)


def load_events(path):
    """[dict] from a budget_watch.jsonl (bad lines skipped)."""
    out = []
    try:
        for line in open(path, encoding='ascii', errors='replace'):
            line = line.strip()
            if line.startswith('{'):
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
    except OSError:
        pass
    return out
