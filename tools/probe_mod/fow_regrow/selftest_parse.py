#!/usr/bin/env python3
"""selftest_parse.py - offline end-to-end check of the reader's change detection and the parser (no game needed).

  python -I selftest_parse.py          ends with SELFTEST_PARSE OK / FAILED

A reference grid (fow_model.GridSim, the human player's grid, Polus geometry) runs frame by frame: decay at common
tick step 4 when id == F mod 16, then every 3rd frame a Python mirror of fow_hook.lua's schedule (same constants and
record format: spawn at the AI anchor, 4 regrow trials aiming at decay phases 16 / 1 / 8 / 12, then 10 top-edge
reveals) and the human's own units (a static base reveal plus a patrol that moves, once in the same frame as an
undo), then fow_reader.FrameProcessor reads the grid (with some frames skipped, as a busy poller would) and writes
the log; fow_parse.py must give:
  spec        the FOG_OF_WAR.md rules              -> Q1 CONFIRMED (4 trials, 4 phases), Q2 CONFIRMED
  step17      regrow step 17 instead of 21         -> Q1 REFUTED
  hide16      game logic hides at state < 16       -> Q1 REFUTED (hidden before the cells reach 0)
  plainedge   reveals clipped without the shift    -> Q2 REFUTED
  nolua       no Lua records (hook not loaded)     -> INCONCLUSIVE, no crash
Output: out\\selftest\\<variant>.log / .txt.
"""
import argparse
import os
import struct
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fow_model as fm  # noqa: E402
import fow_parse  # noqa: E402
import fow_reader  # noqa: E402

OUT = os.path.join(HERE, 'out', 'selftest')
FAILS = []

# fow_hook.lua constants (keep in sync)
R = 90
START_F = 180
TRIAL_D1 = [16, 1, 8, 12]
EDGE_D = [-300, 7.5, 22.5, 37.5, 52.5, 67.5, 82.5, 97.5, 112.5, 127.5]


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        FAILS.append(what)


class HookSim:
    """Python mirror of fow_hook.lua's state machine (records in the same format)."""

    def __init__(self, sim, geo, hid, anchor, tgt, ex, quirk=True, hide_below=0):
        self.sim, self.geo, self.hid = sim, geo, hid
        self.anchor, self.tgt_cells, self.ex = anchor, [geo.cell(*tgt)], ex
        self.quirk, self.hide_below = quirk, hide_below
        self.recs = []
        self.phase, self.ti, self.ei = 'wait', 0, 0
        self.tr = self.e = None
        self.cad = []
        self.steps = 0
        self.just_undid = None
        self.x1, self.y1 = geo.x1, geo.y1

    def rec(self, kind, F, *kv):
        self.recs.append((F, 'FWR1|%s|sid=SIM|f=%d%s|END' % (kind, F, ''.join('|%s=%s' % p for p in kv))))

    def d1(self, F):
        return fm.first_decay_after(F, self.hid)

    def vis(self):
        return 0 if self.sim.fogged(self.tgt_cells, self.hide_below) else 1

    def reveal(self, x, y, F):
        cells, _ = fm.reveal_cells(self.geo, x, y, R, quirk=self.quirk)
        self.sim.reveal(cells, F)
        return cells

    def sample(self, F, ph):
        tr = self.tr
        v = self.vis()
        tr['n'] += 1
        if v != tr.get('v') or ph != tr.get('vph'):
            self.rec('VIS', F, ('trial', self.ti), ('ph', ph), ('vis', v), ('why', 'type_tgt' if v else 'type_nil'),
                     ('prev', tr.get('vf', -1)), ('prevvis', 'nil' if tr.get('v') is None else tr['v']),
                     ('n', tr['n']))
        tr['v'], tr['vph'], tr['vf'] = v, ph, F

    def step(self, F):
        self.just_undid = None
        self.steps += 1
        if len(self.cad) < 40:
            self.cad.append(F)
            if len(self.cad) == 40:
                self.rec('CAD', F, ('frames', ','.join(map(str, self.cad))))
        if self.phase == 'wait':
            if F >= START_F:
                self.rec('START', F, ('map', '_MP_LAND_POLUS'), ('hid', self.hid), ('aid', 1), ('afac', 'REBEL'),
                         ('R', R), ('x1', '%.2f' % self.x1), ('y1', '%.2f' % self.y1), ('hobjs', 12), ('aobjs', 9),
                         ('anchor_hdist', '2900.00'), ('d1now', self.d1(F)))
                self.rec('SPAWN', F, ('ok', 1), ('type', 'MPTL'), ('d_src_tgt', '8.00'), ('d_anchor', '0.00'),
                         ('hdist', '2890.00'))
                self.rec('EDGEX', F, ('x', '%.2f' % self.ex), ('hdist', '2500.00'))
                self.phase, self.ti, self.tr = 'trial', 1, {'ph': 'pre', 't0': F, 'n': 0}
        elif self.phase == 'trial':
            tr = self.tr
            if tr['ph'] == 'pre':
                self.sample(F, 'pre')
                if F >= tr['t0'] + 15:
                    tr['cells'] = self.reveal(self.anchor[0], self.anchor[1], F)
                    tr['fr'], tr['ph'] = F, 'hold'
                    self.rec('REV', F, ('kind', 'regrow'), ('trial', self.ti), ('R', R), ('on', 'src'), ('ok', 1))
            elif tr['ph'] == 'hold':
                self.sample(F, 'hold')
                want = TRIAL_D1[self.ti - 1]
                if F >= tr['fr'] + 45 and (self.d1(F) == want or F >= tr['fr'] + 205):
                    self.sim.undo(tr['cells'])
                    tr['fu'], tr['ph'] = F, 'post'
                    self.just_undid = self.ti
                    self.rec('UNDO', F, ('kind', 'regrow'), ('trial', self.ti), ('d1', self.d1(F)), ('want', want))
            elif tr['ph'] == 'post':
                self.sample(F, 'post')
                if F >= tr['fu'] + 270:
                    self.rec('TEND', F, ('trial', self.ti), ('samples', tr['n']), ('lastvis', tr['v']))
                    self.ti += 1
                    if self.ti > len(TRIAL_D1):
                        self.phase, self.ei, self.e = 'edge', 1, {'ph': 'rev'}
                    else:
                        self.tr = {'ph': 'pre', 't0': F, 'n': 0}
        elif self.phase == 'edge':
            e = self.e
            if e['ph'] == 'rev':
                d = EDGE_D[self.ei - 1]
                y = self.y1 - d
                e['cells'] = self.reveal(self.ex, y, F)
                e['fr'], e['ph'] = F, 'hold'
                self.rec('REV', F, ('kind', 'edge'), ('i', self.ei), ('d', '%.2f' % d), ('x', '%.2f' % self.ex),
                         ('y', '%.2f' % y), ('R', R), ('ok', 1))
            elif e['ph'] == 'hold' and F >= e['fr'] + 9:
                self.sim.undo(e['cells'])
                e['fu'], e['ph'] = F, 'gap'
                self.rec('UNDO', F, ('kind', 'edge'), ('i', self.ei))
            elif e['ph'] == 'gap' and F >= e['fu'] + 9:
                self.ei += 1
                if self.ei > len(EDGE_D):
                    self.phase = 'done'
                    self.rec('DONE', F, ('steps', self.steps))
                else:
                    e['ph'] = 'rev'


def run_variant(name, step=None, quirk=True, hide_below=0, lua=True):
    geo = fm.Geometry.from_terrain(187, 187)
    hid = 0
    sim = fm.GridSim(geo, hid, step=step)
    lines = []
    log = lines.append
    hook = HookSim(sim, geo, hid, anchor=(3100.0, 650.0), tgt=(3108.0, 650.0), ex=geo.x1 * 0.7, quirk=quirk,
                   hide_below=hide_below)
    base, _ = fm.reveal_cells(geo, 700.0, 3000.0, 300.0)
    patrol = {'pos': (900.0, 2600.0), 'cells': None}

    def move_patrol(F):
        if patrol['cells'] is not None:
            sim.undo(patrol['cells'])
        x, y = patrol['pos']
        patrol['pos'] = (1000.0 if x < 950 else 900.0, y)
        patrol['cells'], _ = fm.reveal_cells(geo, patrol['pos'][0], y, 200.0)
        sim.reveal(patrol['cells'], F)

    log('FWREADER 1 scan_sec=5 poll_sec=0.004 (selftest %s)' % name)
    log('GAME pid=0 base=0x0 timestamp=0x67002393 size_of_image=0xcc0000 identity=test')
    log('MODE n=1 ptr=0x0 type=1 game_type=3 stack=1 frame=0 gframe=0')
    log('P n=1 slot=0 id=0 index=0 human=1 faction=EMPIRE neutral=0')
    log('P n=1 slot=1 id=1 index=1 human=0 faction=REBEL neutral=0')
    log('TARGET n=1 id=0')
    log('GRIDS n=1 frame=0 via=selftest ids=0,1')
    h = sim.header()
    log('GRID n=1 id=0 addr=0x0 W=%d H=%d N=%d cw=%r ch=%r x0=%r y0=%r x1=%r y1=%r last=0 step=-1 revall=0' % (
        h['W'], h['H'], h['N'], h['cw'], h['ch'], h['x0'], h['y0'], h['x1'], h['y1']))
    fp = fow_reader.FrameProcessor(log, 1, geo.W, geo.H)
    F = 0
    while hook.phase != 'done' and F < 9000:
        sim.tick(F)                          # common tick step 4
        if F == 1:
            sim.reveal(base, F)              # the human base (REVEAL service, step 8)
        if F % 150 == 7:
            move_patrol(F)
        if F % 3 == 0:                       # AI free store (T7), every 3rd frame
            hook.step(F)
            if hook.just_undid == 2:
                move_patrol(F)               # a human unit moves in the same frame as an undo
        if F % 37 != 5:                      # the reader misses some frames
            hdr = sim.header()
            fp.process(F, hdr, bytes(sim.state), sim.count.tobytes())
        F += 1
    if lua:
        for f, text in hook.recs:
            log('LUA n=1 frame=%d addr=0x0 rec=%s' % (f + 150, text))
    log('MODE_END n=1 last_frame=%d why=selftest frames=%d' % (F, fp.frames))
    log('EXIT reason=selftest modes=1 scans=1 records=%d frames=%d torn=0 failed_reads=0' % (len(hook.recs), fp.frames))
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + '.log')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    modes, misc = fow_parse.read_log(path)
    text, results = fow_parse.report(modes, misc)
    with open(os.path.join(OUT, name + '.txt'), 'w', encoding='utf-8') as f:
        f.write(text)
    return results[0] if results else None, text


class FakeProc:
    """Sparse fake process memory for fow_reader.find_grids (documented layout, FOG_OF_WAR.md §1.1)."""

    def __init__(self):
        self.mem = {}

    def put(self, addr, data):
        self.mem[addr] = bytes(data)

    def read(self, addr, n):
        for base, data in self.mem.items():
            if base <= addr and addr + n <= base + len(data):
                return data[addr - base:addr - base + n]
        return None

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack('<i', b)[0] if b else None


def fake_grid(owner, geo):
    b = bytearray(0x58)
    struct.pack_into('<QQQQ', b, 0, 0x5000000 + owner * 0x100000, 0x6000000 + owner * 0x100000, 0x7000000, 0)
    struct.pack_into('<iii', b, 0x20, geo.N, geo.W, geo.H)
    struct.pack_into('<6f', b, 0x2C, geo.cw, geo.ch, geo.y1, geo.x0, geo.y0, geo.x1)
    struct.pack_into('<iii', b, 0x44, 32, owner, 21)
    return b


def check_grid_discovery():
    print('reader grid discovery')
    geo = fm.Geometry.from_terrain(187, 187)
    for where in (0x198, 0x2a8):
        p = FakeProc()
        mode = 0x1000000
        block = bytearray(0x800)
        arr = 0x2000000
        if where == 0x198:
            struct.pack_into('<i', block, 0x190, 3)
        struct.pack_into('<Q', block, where, arr)
        p.put(mode, block)
        p.put(arr, struct.pack('<QQQ', 0x3000000, 0x3100000, 0))   # player 2 (neutral) has no grid
        p.put(0x3000000, fake_grid(0, geo))
        p.put(0x3100000, fake_grid(1, geo))
        grids, how = fow_reader.find_grids(p, mode)
        check(grids == {0: 0x3000000, 1: 0x3100000}, 'array at mode+0x%x found via %s' % (where, how))
        h = fow_reader.read_grid(p, grids.get(0))
        check(h is not None and (h['W'], h['H'], h['last'], h['step']) == (250, 250, 32, 21), 'header decoded')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args()
    check_grid_discovery()

    print('variant spec (FOG_OF_WAR.md rules)')
    a, text = run_variant('spec')
    check(a is not None and a['q1'] == 'CONFIRMED', 'Q1 CONFIRMED')
    trials = a['trials'] if a else []
    check(len(trials) == 4 and all(t['verdict'] == 'CONFIRMED' for t in trials), 'all 4 trials CONFIRMED')
    check(all(t.get('phase_exact') for t in trials), 'zero frame == undo + 192 + first-decay phase, every trial')
    deltas = sorted(t.get('delta') for t in trials)
    check(deltas == [193, 200, 204, 208], 'windows cover the phases: %s == [193, 200, 204, 208]' % deltas)
    check(all(t.get('circle') and t['circle'][2] == 6 and t['circle'][3] for t in trials),
          'interior reveal = full midpoint circle r 6')
    check(all(t.get('lua_window') and t['lua_window'][0] < t['delta'] <= t['lua_window'][1] < t['delta'] + 3
              for t in trials), 'Lua samples: visible before the zeroing decay, hidden at the first step from it on')
    check(a is not None and a['q2'] == 'CONFIRMED', 'Q2 CONFIRMED')
    edges = a['edges'] if a else []
    check(len(edges) == 10 and sum(1 for e in edges if e['shift'] > 0) >= 6, '10 edge reveals, >= 6 with a shift')
    check(a is not None and a.get('cadence') == [3], 'Lua cadence 3 frames')

    print('variant step17 (regrow step 17)')
    a, _ = run_variant('step17', step=17)
    check(a is not None and a['q1'] == 'REFUTED', 'Q1 REFUTED (%s)' % (a and [t.get('delta') for t in a['trials']]))
    check(a is not None and a['q2'] == 'CONFIRMED', 'Q2 still CONFIRMED')

    print('variant hide16 (game logic hides below state 16)')
    a, _ = run_variant('hide16', hide_below=15)
    check(a is not None and a['q1'] == 'REFUTED', 'Q1 REFUTED')
    check(a is not None and any('before the cells reached 0' in n for t in a['trials'] for n in t['notes']),
          'note: hidden before the cells reached 0')

    print('variant plainedge (no top-edge shift)')
    a, _ = run_variant('plainedge', quirk=False)
    check(a is not None and a['q2'] == 'REFUTED', 'Q2 REFUTED')
    check(a is not None and any(e['label'] == 'plain' for e in a['edges']), 'edge reveals labelled plain')

    print('variant nolua (hook not loaded)')
    a, text = run_variant('nolua', lua=False)
    check(a is not None and a['q1'] == 'INCONCLUSIVE' and a['q2'] == 'INCONCLUSIVE', 'INCONCLUSIVE, report written')

    print('SELFTEST_PARSE %s' % ('OK' if not FAILS else 'FAILED (%d)' % len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == '__main__':
    sys.exit(main())
