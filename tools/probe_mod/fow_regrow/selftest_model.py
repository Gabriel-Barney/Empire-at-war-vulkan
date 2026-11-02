#!/usr/bin/env python3
"""selftest_model.py - offline checks of the prediction model (fow_model.py) against FOG_OF_WAR.md. No game needed.

  python -I selftest_model.py          ends with SELFTEST_MODEL OK / FAILED
"""
import os
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fow_model as fm  # noqa: E402

FAILS = []


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        FAILS.append(what)


def widths(spans):
    return [spans[r][1] - spans[r][0] + 1 for r in sorted(spans)]


def main():
    print('decay (§4.2)')
    check(fm.decay_step() == 21, 'step = (int)(238 / (30 * 6 / 16)) = 21')
    states = [s for _, s in fm.regrow_schedule(0, 0)]
    check(states == [238, 217, 196, 175, 154, 133, 112, 91, 70, 49, 28, 7, 0], '239+ -> 238 -> ... -> 7 -> 0 (13 decays)')
    for start in (239, 240, 247, 255):
        check([s for _, s in fm.regrow_schedule(5, 0, start_state=start)] == states, 'start state %d regrows alike' % start)
    wins = sorted(fm.regrow_window(F, 0) for F in range(16))
    check(wins == list(range(193, 209)), 'regrow window over the 16 undo phases = 193..208 frames, each once')
    check(abs(193 / 30.0 - 6.433) < 0.001 and abs(208 / 30.0 - 6.933) < 0.001, '= 6.43 .. 6.93 s')
    for pid in (0, 1, 6, 15):
        for F in (100, 101, 115, 116):
            sch = fm.regrow_schedule(F, pid)
            check(all(D % 16 == pid % 16 for D, _ in sch) and sch[0][0] - F in range(1, 17)
                  and all(b[0] - a[0] == 16 for a, b in zip(sch, sch[1:])),
                  'pid %d undo F=%d: decays at id mod 16, first 1..16 frames later, then every 16' % (pid, F))
    check(fm.first_decay_after(16, 0) == 16 and fm.first_decay_after(15, 0) == 1,
          'undo in a decay frame waits a full period (decay runs before Lua, §4.4)')

    print('reference grid (§2.3, §4.2)')
    geo = fm.Geometry.from_terrain(187, 187)
    sim = fm.GridSim(geo, pid=0)
    cells, (row, col, r) = fm.reveal_cells(geo, 1500.0, 1500.0, 90.0)
    sim.reveal(cells, 3)
    i0 = sim.idx([next(iter(cells))])[0]
    check(sim.state[i0] == 0xEF and sim.count[i0] == 1, 'reveal of a fogged cell: state 0xEF, count 1')
    for F in range(4, 100):
        sim.tick(F)
    check(sim.state[i0] == 255, 'counted cell after decays: 255 - fade (fade 0)')
    sim.undo(cells)
    Fu = 99
    zero_at = None
    for F in range(100, 400):
        sim.tick(F)
        if zero_at is None and sim.fogged(cells):
            zero_at = F
    check(zero_at == Fu + fm.regrow_window(Fu, 0), 'GridSim zeroes the cells at undo + regrow_window (%s)' % zero_at)

    print('geometry (§1.2)')
    check((geo.W, geo.H) == (250, 250) and abs(geo.cw - 14.96) < 1e-5, 'Polus 187 vertices, extent 20 W: 250 x 250 cells of 14.96')
    g2 = fm.Geometry.from_terrain(187, 187, 'W-1')
    check((g2.W, abs(g2.cw - 15.0) < 1e-6) == (248, True), 'extent 20 (W - 1): 248 cells of 15.0')
    check(geo.radius_cells(90) == 6 and g2.radius_cells(90) == 6, 'R 90 -> r 6 cells on both')
    check(geo.cell(1870, geo.y1 + 1000)[0] == 0 and geo.cell(1870, geo.y1 - 7.5)[0] == 0
          and geo.cell(1870, geo.y1 - 22.5)[0] == 1 and geo.cell(1870, geo.y0 - 50)[0] == geo.H - 1,
          'row 0 is the max-y edge; points outside clamp into the box')
    check(geo.cell(-10, 100)[1] == 0 and geo.cell(1e6, 100)[1] == geo.W - 1, 'columns clamp')

    print('reveal shape (§2.2)')
    for rr in range(0, 13):
        sp = fm.reveal_spans(500, 500, 250, 250, rr)
        w = widths(sp)
        check(len(sp) == 2 * rr + 1 and w == w[::-1] and max(w) == 2 * rr + 1
              and all(sp[k][0] + sp[k][1] == 500 for k in sp), 'interior r=%d: %d rows, symmetric, centre width %d' % (
                  rr, 2 * rr + 1, 2 * rr + 1))
        check(sp == fm.reveal_spans(500, 500, 250, 250, rr, quirk=False), 'interior r=%d: quirk == plain' % rr)
    check(widths(fm.reveal_spans(500, 500, 250, 250, 6)) == [5, 7, 9, 11, 13, 13, 13, 13, 13, 11, 9, 7, 5],
          'r=6 row widths 5 7 9 11 13 13 13 13 13 11 9 7 5')

    print('top-edge quirk (§2.2)')
    W, H, rr, c = 250, 250, 6, 125
    full = fm.reveal_spans(W, H, c, rr, rr)        # circle centred on row r: fully inside
    for k in range(0, 2 * rr + 2):
        q = fm.reveal_spans(W, H, c, k, rr)
        p = fm.reveal_spans(W, H, c, k, rr, quirk=False)
        if k < rr:
            want = {j: full[j] for j in full if j <= k + rr}
            check(q == want and q != p, 'centre row %d: drawn as the circle centred on row %d (shift %d), rows 0..%d '
                  '(far part cut), differs from the plain clip' % (k, rr, rr - k, k + rr))
            check(widths(q)[0] == widths(full)[0], '   centre row %d: row 0 holds the circle top (width %d)' % (
                k, widths(full)[0]))
        else:
            check(q == p, 'centre row %d >= r: no shift (control)' % k)
    for k in range(H - rr - 1, H):
        check(fm.reveal_spans(W, H, c, k, rr) == fm.reveal_spans(W, H, c, k, rr, quirk=False),
              'bottom edge row %d: rows simply cut (quirk == plain)' % k)
    q0 = fm.reveal_spans(W, H, 0, 0, rr)
    check(min(lo for lo, _ in q0.values()) == 0, 'top-left corner: columns clamp at 0')

    print('SELFTEST_MODEL %s' % ('OK' if not FAILS else 'FAILED (%d)' % len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == '__main__':
    sys.exit(main())
