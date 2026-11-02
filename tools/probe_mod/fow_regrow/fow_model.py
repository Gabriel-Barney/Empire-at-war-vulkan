#!/usr/bin/env python3
"""fow_model.py - the predictions of docs/re/FOG_OF_WAR.md that the fow_regrow probe tests (no game needed).

  fow_model.py predict [--terrain 187] [--extent W|W-1] [--R 90] [--pid 0]

Spec rules used (FOG_OF_WAR.md):
  §1.2  W = ceil(extent / 15), cell = extent / W (f32), cell of a point after a 0.001 inset clamp,
        col = (int)((x - x0) / cw), row = (int)((y1 - y) / ch): row 0 is the max-y edge.
  §2.2  r = (int)(R / cw + 0.5); filled midpoint circle; the span array is indexed by the UNCLAMPED row offset, so a
        circle whose centre row is < r is drawn shifted toward +row (-y) by (r - row) rows, far part cut
        ("top-edge quirk"). quirk=False gives the plain clipped circle (what the quirk is tested against).
  §2.3  counting: count 0 -> state 0 becomes 0xEF, 1..15 -> 255 - s, 16..238 -> 255; Undo = count - 1.
  §4.2  decay every 16 frames for player id == F (mod 16): counted -> 255 - fade, else T[s] = max(0, s - 21),
        T[239..255] = 238. So after the last count drop: 238 (first decay, 1..16 frames later), 217, ... 7, 0 =
        13 decays, hidden 193..208 frames after the undo.
GridSim is a small reference grid (one player) for the offline selftests.
"""
import argparse
import struct
import sys
from array import array

sys.dont_write_bytecode = True

DESIRED_LAND = 15.0
FPS = 30.0
REGROW_T = 6.0
PERIOD = 16


def f32(v):
    return struct.unpack('<f', struct.pack('<f', float(v)))[0]


def ceil_cells(v):
    """0x22f9f0: (int)v + 1 when v > (float)(int)v."""
    i = int(v)
    return i + 1 if v > float(i) else i


class Geometry:
    """One FOW grid's geometry (all grids of a mode share it)."""

    def __init__(self, x0, y0, x1, y1, W=None, H=None, cell=DESIRED_LAND):
        self.x0, self.y0, self.x1, self.y1 = f32(x0), f32(y0), f32(x1), f32(y1)
        if W is None:
            W = min(512, max(0, ceil_cells(f32(f32(self.x1 - self.x0) / f32(cell)))))
        if H is None:
            H = min(512, max(0, ceil_cells(f32(f32(self.y1 - self.y0) / f32(cell)))))
        self.W, self.H, self.N = int(W), int(H), int(W) * int(H)
        self.cw = f32(f32(self.x1 - self.x0) / self.W)
        self.ch = f32(f32(self.y1 - self.y0) / self.H)

    @classmethod
    def from_terrain(cls, verts_w, verts_h, extent='W'):
        """Land box from the .ted vertex grid (spacing 20). extent 'W' = [0, 20 W] (RE 2026-11-01, the probe brief),
        'W-1' = [0, 20 (W - 1)] (TERRAIN_WATER_POST §1.4 vertex span). The reader logs the real box."""
        nw, nh = (verts_w, verts_h) if extent == 'W' else (verts_w - 1, verts_h - 1)
        return cls(0.0, 0.0, 20.0 * nw, 20.0 * nh)

    def cell(self, x, y):
        """(row, col) of a world point (§1.2)."""
        x = min(max(f32(x), f32(self.x0 + f32(0.001))), f32(self.x1 - f32(0.001)))
        y = min(max(f32(y), f32(self.y0 + f32(0.001))), f32(self.y1 - f32(0.001)))
        col = int(f32(f32(x - self.x0) / self.cw))
        row = int(f32(f32(self.y1 - y) / self.ch))
        idx = min(max(row * self.W + col, 0), self.N - 1)
        return idx // self.W, idx % self.W

    def radius_cells(self, R):
        return int(f32(f32(f32(R) / self.cw) + f32(0.5)))

    def row_y(self, row):
        """World y range (top, bottom) of a grid row."""
        return self.y1 - row * self.ch, self.y1 - (row + 1) * self.ch

    def col_x(self, col):
        return self.x0 + col * self.cw, self.x0 + (col + 1) * self.cw

    def describe(self):
        return 'W=%d H=%d cw=%.6f ch=%.6f box x %.3f..%.3f y %.3f..%.3f' % (
            self.W, self.H, self.cw, self.ch, self.x0, self.x1, self.y0, self.y1)


def circle_points(r):
    """(row offset k, column offset) of every plot8 point of the §2.2 midpoint circle (k = 0..2r)."""
    pts = []

    def plot8(x, y):
        pts.extend([(r + y, -x), (r + y, x), (r + x, -y), (r + x, y), (r - x, -y), (r - x, y), (r - y, -x),
                    (r - y, x)])

    if r <= 0:
        plot8(0, 0)
        return pts
    x, y, d, dE, dSE = 0, r, 1 - r, 3, 5 - 2 * r
    plot8(0, r)
    while x < y:
        old = d
        if old >= 0:
            y -= 1
        x += 1
        plot8(x, y)
        d += dSE if old >= 0 else dE
        dSE += 2 if old < 0 else 4
        dE += 2
    return pts


def reveal_spans(W, H, col, row, r, quirk=True):
    """{grid row: (lo, hi)} drawn by one reveal. quirk=True is §2.2 (span index = unclamped offset);
    quirk=False is the plain clipped circle."""
    rowA, rowB = row - r, row + r
    cA, cB = min(max(rowA, 0), H - 1), min(max(rowB, 0), H - 1)
    pts = circle_points(r)
    out = {}
    if quirk:
        n = cB - cA + 1
        span = [[W - 1, 0] for _ in range(n)]
        for k, dc in pts:
            if 0 <= k < n:
                span[k][0] = min(span[k][0], col + dc)
                span[k][1] = max(span[k][1], col + dc)
        for i in range(n):
            lo, hi = max(0, span[i][0]), min(W - 1, span[i][1])
            if lo <= hi:
                out[cA + i] = (lo, hi)
        return out
    span = {}
    for k, dc in pts:
        R_ = rowA + k
        if cA <= R_ <= cB:
            s = span.setdefault(R_, [W - 1, 0])
            s[0], s[1] = min(s[0], col + dc), max(s[1], col + dc)
    for R_, (lo, hi) in span.items():
        lo, hi = max(0, lo), min(W - 1, hi)
        if lo <= hi:
            out[R_] = (lo, hi)
    return out


def spans_to_cells(spans):
    return frozenset((r, c) for r, (lo, hi) in spans.items() for c in range(lo, hi + 1))


def reveal_cells(geo, x, y, R, quirk=True, r=None):
    row, col = geo.cell(x, y)
    if r is None:
        r = geo.radius_cells(R)
    return spans_to_cells(reveal_spans(geo.W, geo.H, col, row, r, quirk)), (row, col, r)


def cells_to_spans(cells):
    rows = {}
    for r, c in cells:
        rows.setdefault(r, []).append(c)
    out = {}
    for r, cs in rows.items():
        cs.sort()
        out[r] = (cs[0], cs[-1]) if cs[-1] - cs[0] + 1 == len(cs) else tuple(cs)
    return out


# ---- decay ---------------------------------------------------------------------------------------------------------

def decay_step(fps=FPS, t=REGROW_T):
    return int(f32(238.0 / f32(f32(fps * t) / 16.0)))


def decay_table(step):
    return [0] + [max(0, i - step) for i in range(1, 239)] + [238] * 17


def first_decay_after(F, pid, period=PERIOD):
    """Frames from a Lua undo in frame F (after that frame's decay, §4.4 order) to the player's next decay."""
    v = (pid - F - 1) % period
    return v + 1


def regrow_schedule(undo_frame, pid, start_state=255, step=None, period=PERIOD):
    """[(decay frame, state after it)] from the first decay after the undo until the state is 0."""
    step = decay_step() if step is None else step
    T = decay_table(step)
    D = undo_frame + first_decay_after(undo_frame, pid, period)
    s, out = start_state, []
    for _ in range(64):
        s = T[s]
        out.append((D, s))
        if s == 0:
            break
        D += period
    return out


def regrow_window(undo_frame, pid, step=None):
    """Frames from the undo to the decay that writes state 0 (the object is hidden from that frame on)."""
    return regrow_schedule(undo_frame, pid, step=step)[-1][0] - undo_frame


# ---- reference grid (selftests) ------------------------------------------------------------------------------------

class GridSim:
    """One player's FOW grid per §2.3 / §4.2. hidden_below: a cell counts as fogged when state <= it (spec: 0)."""

    def __init__(self, geo, pid, step=None, period=PERIOD):
        self.geo, self.pid, self.period = geo, pid, period
        self.step = decay_step() if step is None else step
        self.T = decay_table(self.step)
        self.state = bytearray(geo.N)
        self.count = array('H', [0]) * geo.N
        self.fade = bytearray(geo.N)
        self.last = 0

    def idx(self, cells):
        return [r * self.geo.W + c for r, c in cells]

    def reveal(self, cells, F):
        for i in self.idx(cells):
            if self.count[i] == 0:
                s = self.state[i]
                if s < 16:
                    self.state[i] = 0xEF if s == 0 else 255 - s
                    self.fade[i] = min(max(F - self.last - 1, 0), 15) if self.last else 0
                elif s < 239:
                    self.state[i] = 0xFF
                    self.fade[i] = 0
            if self.count[i] != 0xFFFF:
                self.count[i] += 1

    def undo(self, cells):
        for i in self.idx(cells):
            if self.count[i] > 0:
                self.count[i] -= 1

    def tick(self, F):
        """Common tick step 4: decay when id == F (mod 16)."""
        if F % self.period != self.pid % self.period:
            return
        counted = [i for i, c in enumerate(self.count) if c]
        self.state = bytearray(bytes(self.state).translate(bytes(self.T)))   # uncounted cells: T[s]
        for i in counted:                                                      # counted cells: 255 - fade
            self.state[i] = 255 - self.fade[i]
            self.fade[i] = 0
        self.last = F

    def fogged(self, cells, hidden_below=0):
        return all(self.state[i] <= hidden_below for i in self.idx(cells))

    def header(self):
        g = self.geo
        return {'W': g.W, 'H': g.H, 'N': g.N, 'cw': g.cw, 'ch': g.ch, 'x0': g.x0, 'y0': g.y0, 'x1': g.x1,
                'y1': g.y1, 'last': self.last, 'owner': self.pid, 'step': self.step}


# ---- printed predictions -------------------------------------------------------------------------------------------

def edge_table(geo, R, x, depths):
    """Rows drawn for reveals centred at y = y1 - d (top-edge test), quirk vs plain."""
    lines = []
    for d in depths:
        cells, (row, col, r) = reveal_cells(geo, x, geo.y1 - d, R)
        plain, _ = reveal_cells(geo, x, geo.y1 - d, R, quirk=False)
        qs, ps = reveal_spans(geo.W, geo.H, col, row, r), reveal_spans(geo.W, geo.H, col, row, r, False)
        tag = 'shifted by %d rows' % (r - row) if row < r else 'no shift (control)'
        lines.append('d=%7.1f  centre row %d col %d r=%d: %s, %d cells (plain clip %d)' % (
            d, row, col, r, tag, len(cells), len(plain)))
        for j in sorted(set(qs) | set(ps)):
            q, p = qs.get(j), ps.get(j)
            top, bot = geo.row_y(j)
            fmt = (lambda s: 'cols %3d..%3d (%2d)' % (s[0], s[1], s[1] - s[0] + 1) if s else '-' * 17)
            lines.append('      row %3d  y %8.1f..%8.1f   quirk %s   plain %s' % (j, bot, top, fmt(q), fmt(p)))
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    p = sub.add_parser('predict')
    p.add_argument('--terrain', type=int, default=187, help='.ted vertex grid size (Polus 187)')
    p.add_argument('--extent', choices=('W', 'W-1'), default='W')
    p.add_argument('--R', type=float, default=90.0)
    p.add_argument('--pid', type=int, default=0)
    a = ap.parse_args()
    if a.cmd != 'predict':
        ap.print_help()
        return 1
    geo = Geometry.from_terrain(a.terrain, a.terrain, a.extent)
    print('geometry:', geo.describe(), '| R=%g -> r=%d cells' % (a.R, geo.radius_cells(a.R)))
    print('decay step %d; regrow states after the last count drop: %s' % (
        decay_step(), ', '.join(str(s) for _, s in regrow_schedule(0, 0))))
    print('\nRegrow window (player id %d decays at F == %d mod 16), undo in frame F:' % (a.pid, a.pid % 16))
    print('  F mod 16 | first decay after | hidden after (frames) | seconds')
    for m in range(16):
        w = regrow_window(m, a.pid)
        print('  %8d | %17d | %21d | %.3f' % (m, first_decay_after(m, a.pid), w, w / FPS))
    print('\nTop-edge reveals at x = centre, y = y1 - d:')
    depths = [-300.0] + [7.5 + 15.0 * k for k in range(9)]
    for line in edge_table(geo, a.R, geo.x1 / 2, depths):
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
