#!/usr/bin/env python3
"""fow_parse.py - turns a fow_reader.py log into the FOW regrow probe report.

  fow_parse.py parse <reader.log> [--out report.txt] [--json report.json]

Per land mode with probe records:
  Q1 regrow window (FOG_OF_WAR.md §4, §5.2), one block per trial: the Lua reveal / undo frames, the cells the human
  grid counted (CUP) and released (CDN), their state at every decay after the undo (TRK: expect 238, 217, ..., 7, 0,
  16 frames apart), the decay frame that zeroed them (ZERO / TRK last=), the predicted frame
  undo + 192 + ((human id - undo - 1) mod 16) + 1, and the Lua visibility samples (Find_Nearest, force 1): the
  object must be visible up to the zeroing decay and hidden from it on.
  Q2 top-edge circle shift (§2.2): for every edge reveal the counted cell set against the quirk prediction and the
  plain clipped circle.
Verdicts: CONFIRMED / REFUTED / INCONCLUSIVE (meanings in README.md).
"""
import argparse
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fow_model as fm  # noqa: E402

EXPECT_STATES = [s for _, s in fm.regrow_schedule(0, 0)]   # 238, 217, ..., 7, 0
WINDOW = (193, 208)


def kv_line(line):
    parts = line.rstrip('\n').split(' ')
    d = {'_kind': parts[0]}
    for p in parts[1:]:
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    return d


def rec_fields(text):
    parts = text.split('|')
    d = {'_kind': parts[1]}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    return d


def cells_of(text):
    out = set()
    if not text or text == 'TOO_MANY':
        return out
    for part in text.split(','):
        r, span = part.split(':')
        a, b = span.split('-')
        out.update((int(r), c) for c in range(int(a), int(b) + 1))
    return out


def num(v, default=None):
    try:
        return int(v)
    except (TypeError, ValueError):
        try:
            return float(v)
        except (TypeError, ValueError):
            return default


class Mode:
    def __init__(self, n):
        self.n = n
        self.grid = {}        # id -> GRID dict
        self.target = None
        self.events = []      # (kind, f, last, cells)
        self.decays = []
        self.groups = {}      # g -> {'f', 'cells', 'trk': [(f, last, smin, smax, zero, k, counted)]}
        self.recs = []
        self.seen = set()
        self.pobj = []
        self.skips = 0
        self.via = None

    def geometry(self):
        g = self.grid.get(self.target)
        if not g:
            return None
        return fm.Geometry(float(g['x0']), float(g['y0']), float(g['x1']), float(g['y1']), int(g['W']), int(g['H']))


def read_log(path):
    modes = {}
    misc = []
    for line in open(path, encoding='ascii', errors='replace'):
        d = kv_line(line)
        k = d['_kind']
        n = num(d.get('n'))
        if k in ('FWREADER', 'GAME', 'EXIT', 'IGNORED'):
            misc.append(line.rstrip())
            continue
        if n is None:
            continue
        m = modes.setdefault(n, Mode(n))
        if k == 'TARGET':
            m.target = num(d.get('id'))
        elif k == 'GRIDS':
            m.via = d.get('via')
        elif k == 'GRID':
            m.grid[num(d['id'])] = d
        elif k in ('CUP', 'CDN', 'ZERO'):
            m.events.append((k, num(d['f']), num(d['last']), cells_of(d.get('cells'))))
        elif k == 'DEC':
            m.decays.append((num(d['f']), num(d['last'])))
        elif k == 'GRP':
            m.groups[num(d['g'])] = {'f': num(d['f']), 'cells': cells_of(d.get('cells')), 'trk': []}
        elif k == 'TRK':
            g = m.groups.get(num(d['g']))
            if g is not None:
                z, kk = d['zero'].split('/')
                g['trk'].append((num(d['f']), num(d['last']), num(d['smin']), num(d['smax']), int(z), int(kk),
                                 num(d.get('counted'), 0)))
        elif k == 'SKIP':
            m.skips += 1
        elif k == 'POBJ':
            m.pobj.append(d)
        elif k == 'LUA':
            text = d.get('rec', '')
            if text not in m.seen:
                m.seen.add(text)
                r = rec_fields(text)
                r['f'] = num(r.get('f'))
                m.recs.append(r)
    return modes, misc


def union_events(m, kind, f0, f1):
    out = set()
    for k, f, _last, cells in m.events:
        if k == kind and f0 <= f <= f1:
            out |= cells
    return out


def circle_fit(cells, geo):
    """(row, col, r, matches the §2.2 interior circle) of a counted cell set."""
    if not cells:
        return None
    rows = [r for r, _ in cells]
    cols = [c for _, c in cells]
    r0, r1, c0, c1 = min(rows), max(rows), min(cols), max(cols)
    if (r1 - r0) % 2 or (c1 - c0) % 2:
        return (r0 + r1) // 2, (c0 + c1) // 2, (r1 - r0) // 2, False
    row, col, r = (r0 + r1) // 2, (c0 + c1) // 2, (r1 - r0) // 2
    want = fm.spans_to_cells(fm.reveal_spans(geo.W, geo.H, col, row, r))
    return row, col, r, set(cells) == set(want)


def analyse_trial(m, geo, hid, rev, undo, vis):
    res = {'trial': rev.get('trial'), 'Fr': rev['f'], 'Fu': undo['f'] if undo else None, 'notes': []}
    if not undo:
        res['verdict'] = 'INCONCLUSIVE'
        res['notes'].append('no UNDO record')
        return res
    Fr, Fu = rev['f'], undo['f']
    cup = union_events(m, 'CUP', Fr - 1, Fr + 3)
    grp = None
    for g in m.groups.values():
        if Fu - 1 <= g['f'] <= Fu + 3 and (grp is None or len(g['cells'] & cup) > len(grp['cells'] & cup)):
            grp = g
    if not cup or grp is None or not (grp['cells'] & cup):
        res['verdict'] = 'INCONCLUSIVE'
        res['notes'].append('no counted cells seen for this reveal / undo (CUP %d cells, group %s)' % (
            len(cup), 'yes' if grp else 'none'))
        return res
    C = grp['cells'] & cup
    fit = circle_fit(C, geo)
    res['cells'] = len(C)
    res['circle'] = fit
    res['Fu_mem'] = grp['f']
    pred_zero = Fu + 192 + fm.first_decay_after(Fu, hid)
    res['pred_zero'] = pred_zero
    res['pred_window'] = pred_zero - Fu
    # decays after the undo, from the group's state summaries
    seq, zero_last, recount = [], None, False
    for f, last, smin, smax, z, k, counted in grp['trk']:
        if counted:
            recount = True
        if smin == smax and (not seq or seq[-1][1] != smin) and f > grp['f'] - 1:
            seq.append((last, smin))
        if z == k and zero_last is None:
            zero_last = last
    states = [s for _, s in seq]
    if states and states[0] >= 239:
        seq, states = seq[1:], states[1:]
    res['states'] = states
    res['decay_frames'] = [d for d, _ in seq]
    res['zero'] = zero_last
    gaps = sorted(set(b - a for a, b in zip(res['decay_frames'], res['decay_frames'][1:])))
    res['decay_gaps'] = gaps
    seq_ok = states == EXPECT_STATES and gaps == [16]
    # Lua visibility
    v = {'pre': [], 'hold': [], 'post': []}
    flip = None
    for r in vis:
        v.setdefault(r.get('ph'), []).append(r.get('vis'))
        if r.get('ph') == 'post' and r.get('vis') == '0' and r.get('prevvis') == '1' and flip is None:
            flip = (num(r.get('prev')), r['f'])
    res['vis'] = {k: ','.join(x) for k, x in v.items() if x}
    res['flip'] = flip
    lua_ok = None
    if flip and zero_last is not None:
        fv, fh = flip
        lua_ok = fv < zero_last + 1 and fh >= zero_last - 1   # +-1: Lua and reader frame counters may differ by one
        res['lua_window'] = (fv - Fu, fh - Fu)
        if fh < zero_last - 1:
            res['notes'].append('hidden for game logic at F=%d, before the cells reached 0 at %d' % (fh, zero_last))
        if fv > zero_last + 1:
            res['notes'].append('still visible at F=%d after the cells reached 0 at %d' % (fv, zero_last))
    elif 'nospawn' in json.dumps(vis) or not vis:
        res['notes'].append('no visibility samples (spawn failed?): grid-only result')
    else:
        res['notes'].append('no visible -> hidden flip in the post-undo samples')
    if 'pre' in res['vis'] and '1' in res['vis']['pre'].split(','):
        res['notes'].append('baseline: object visible before the reveal (another source sees it)')
    if recount:
        res['notes'].append('cells counted again before reaching 0 (another source)')
    if zero_last is None:
        res['verdict'] = 'INCONCLUSIVE'
        res['notes'].append('cells never all reached 0 in the log')
        return res
    delta = zero_last - Fu
    res['delta'] = delta
    res['delta_mem'] = zero_last - grp['f']   # from the frame the reader first saw the count at 0
    win_ok = WINDOW[0] <= delta <= WINDOW[1]
    if not win_ok and grp['f'] - Fu == 1 and WINDOW[0] <= res['delta_mem'] <= WINDOW[1]:
        win_ok = True
        res['notes'].append('Lua frame and reader frame differ by one at the undo (window %d by the reader frame)' %
                            res['delta_mem'])
    res['phase_exact'] = zero_last == pred_zero
    if not res['phase_exact'] and win_ok:
        res['notes'].append('window in range but zero at %d, phase rule predicts %d' % (zero_last, pred_zero))
    if recount:
        res['verdict'] = 'INCONCLUSIVE'
    elif not win_ok and abs(delta - min(max(delta, WINDOW[0]), WINDOW[1])) > 1:
        res['verdict'] = 'REFUTED'
        res['notes'].append('regrow window %d frames, outside %d..%d' % (delta, WINDOW[0], WINDOW[1]))
    elif not seq_ok:
        res['verdict'] = 'REFUTED'
        res['notes'].append('state sequence / decay spacing differs from 238, 217, ..., 0 every 16 frames')
    elif lua_ok is False:
        res['verdict'] = 'REFUTED'
    else:
        res['verdict'] = 'CONFIRMED' if win_ok else 'INCONCLUSIVE'
    return res


def analyse_edge(m, geo, rev, r_obs):
    x, y, R = float(rev['x']), float(rev['y']), float(rev['R'])
    row, col = geo.cell(x, y)
    r = r_obs if r_obs is not None else geo.radius_cells(R)
    Q = fm.spans_to_cells(fm.reveal_spans(geo.W, geo.H, col, row, r))
    P = fm.spans_to_cells(fm.reveal_spans(geo.W, geo.H, col, row, r, quirk=False))
    F = rev['f']
    near = {(a, b) for a, b in union_events(m, 'CUP', F - 1, F + 3)
            if a <= row + r + 2 and abs(b - col) <= r + 2}
    if near == Q and Q != P:
        label = 'quirk'
    elif near == Q:
        label = 'control-ok'
    elif near == P:
        label = 'plain'
    elif not near:
        label = 'none'
    else:
        label = 'other'
    return {'i': rev.get('i'), 'd': rev.get('d'), 'f': F, 'row': row, 'col': col, 'r': r, 'shift': max(0, r - row),
            'label': label, 'observed': fm.cells_to_spans(near), 'quirk': fm.cells_to_spans(Q),
            'plain': fm.cells_to_spans(P), 'n_obs': len(near), 'n_pred': len(Q)}


def analyse_mode(m):
    out = {'n': m.n, 'target': m.target, 'via': m.via, 'skips': m.skips}
    geo = m.geometry()
    recs = sorted(m.recs, key=lambda r: (r['f'] if r['f'] is not None else -1))
    out['records'] = len(recs)
    start = next((r for r in recs if r['_kind'] == 'START'), None)
    out['start'] = start
    out['spawn'] = next((r for r in recs if r['_kind'] == 'SPAWN'), None)
    out['errors'] = [r for r in recs if r['_kind'] == 'ERR']
    cad = next((r for r in recs if r['_kind'] == 'CAD'), None)
    if cad:
        fr = [int(x) for x in cad['frames'].split(',') if x]
        out['cadence'] = sorted(set(b - a for a, b in zip(fr, fr[1:])))
    if geo is None or start is None:
        out['q1'] = out['q2'] = 'INCONCLUSIVE'
        out['why'] = 'no target grid geometry' if geo is None else 'no START record'
        return out
    out['geometry'] = geo.describe()
    hid = num(start.get('hid'))
    if m.target is not None and hid != m.target:
        out.setdefault('warnings', []).append('Lua human id %s != reader target %s' % (hid, m.target))
    out['decay_residues'] = sorted(set(last % 16 for _, last in m.decays if last))
    trials = []
    for rev in recs:
        if rev['_kind'] == 'REV' and rev.get('kind') == 'regrow':
            t = rev.get('trial')
            undo = next((r for r in recs if r['_kind'] == 'UNDO' and r.get('kind') == 'regrow' and
                         r.get('trial') == t), None)
            vis = [r for r in recs if r['_kind'] == 'VIS' and r.get('trial') == t]
            trials.append(analyse_trial(m, geo, hid, rev, undo, vis))
    out['trials'] = trials
    v1 = [t['verdict'] for t in trials]
    out['q1'] = 'REFUTED' if 'REFUTED' in v1 else ('CONFIRMED' if 'CONFIRMED' in v1 else 'INCONCLUSIVE')
    r_obs = next((t['circle'][2] for t in trials if t.get('circle') and t['circle'][3]), None)
    out['r_obs'] = r_obs
    out['r_model'] = geo.radius_cells(float(start.get('R', 90)))
    edges = [analyse_edge(m, geo, r, r_obs) for r in recs if r['_kind'] == 'REV' and r.get('kind') == 'edge']
    out['edges'] = edges
    shifted = [e for e in edges if e['shift'] > 0]
    controls = [e for e in edges if e['shift'] == 0]
    if not shifted:
        out['q2'] = 'INCONCLUSIVE'
    elif any(e['label'] in ('plain', 'other') for e in shifted) or any(e['label'] in ('plain', 'other')
                                                                          for e in controls):
        out['q2'] = 'REFUTED'
    elif all(e['label'] == 'quirk' for e in shifted):
        out['q2'] = 'CONFIRMED'
    else:
        out['q2'] = 'INCONCLUSIVE'
    return out


def fmt_spans(sp):
    return ' '.join('%d:%s' % (r, ('%d-%d' % v) if len(v) == 2 else '/'.join(map(str, v))) for r, v in sorted(sp.items()))


def report(modes, misc):
    L = ['FOW regrow probe (fowf t06, FOG_OF_WAR.md §9 item 8)', '']
    L += ['   ' + x for x in misc]
    results = []
    for n in sorted(modes):
        m = modes[n]
        if not m.recs and not m.events:
            continue
        a = analyse_mode(m)
        results.append(a)
        L.append('')
        L.append('== land mode #%d: target (human) grid id %s, grids found via %s, %d probe records, %d skipped-frame '
                 'gaps ==' % (n, a['target'], a['via'], a['records'], a['skips']))
        if a.get('geometry'):
            L.append('   geometry %s; R 90 -> r %s (model) / %s (observed)' % (a['geometry'], a['r_model'], a['r_obs']))
        if a.get('start'):
            s = a['start']
            L.append('   START f=%s map=%s human id=%s ai id=%s (%s) anchor %s units from the human objects' % (
                s['f'], s.get('map'), s.get('hid'), s.get('aid'), s.get('afac'), s.get('anchor_hdist')))
        if a.get('spawn'):
            L.append('   SPAWN %s' % ' '.join('%s=%s' % (k, v) for k, v in a['spawn'].items() if k not in ('_kind', 'sid')))
        if 'cadence' in a:
            L.append('   Lua step cadence (frames between steps): %s' % a['cadence'])
        if a.get('decay_residues'):
            L.append('   human grid decays at F mod 16 = %s' % a['decay_residues'])
        for e in a.get('errors', []):
            L.append('   ERR %s' % ' '.join('%s=%s' % (k, v) for k, v in e.items() if k not in ('_kind', 'sid')))
        for w in a.get('warnings', []):
            L.append('   WARNING ' + w)
        if a.get('why'):
            L.append('   ' + a['why'])
        L.append('')
        L.append('-- Q1 regrow window: %s' % a['q1'])
        for t in a.get('trials', []):
            L.append('   trial %s: %s  reveal F=%s undo F=%s cells=%s circle=%s' % (
                t['trial'], t['verdict'], t['Fr'], t['Fu'], t.get('cells'), t.get('circle')))
            if 'zero' in t:
                L.append('      decays after undo at %s' % t.get('decay_frames'))
                L.append('      states %s (expected %s)' % (t.get('states'), EXPECT_STATES))
                L.append('      cells reached 0 at F=%s: %s frames = %.2f s after the undo; predicted F=%s (%s frames)' % (
                    t['zero'], t.get('delta'), (t.get('delta') or 0) / 30.0, t.get('pred_zero'), t.get('pred_window')))
            if t.get('vis'):
                L.append('      Lua visibility %s; last visible / first hidden sample after undo: %s' % (
                    t['vis'], t.get('lua_window')))
            for x in t['notes']:
                L.append('      note: ' + x)
        L.append('')
        L.append('-- Q2 top-edge circle shift: %s' % a['q2'])
        for e in a.get('edges', []):
            L.append('   edge %s d=%s: centre row %d col %d r %d shift %d -> %s (%d cells observed, %d predicted)' % (
                e['i'], e['d'], e['row'], e['col'], e['r'], e['shift'], e['label'], e['n_obs'], e['n_pred']))
            if e['label'] not in ('quirk', 'control-ok'):
                L.append('      observed %s' % fmt_spans(e['observed']))
                L.append('      quirk    %s' % fmt_spans(e['quirk']))
                L.append('      plain    %s' % fmt_spans(e['plain']))
    if not results:
        L.append('')
        L.append('NO PROBE DATA: no land mode with grid events or probe records.')
    return '\n'.join(L) + '\n', results


def cmd_parse(a):
    modes, misc = read_log(a.log)
    text, results = report(modes, misc)
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(text)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(results, f, indent=1, default=str)
    sys.stdout.write(text)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    p = sub.add_parser('parse')
    p.add_argument('log')
    p.add_argument('--out')
    p.add_argument('--json')
    a = ap.parse_args()
    if a.cmd != 'parse':
        ap.print_help()
        return 1
    cmd_parse(a)
    return 0


if __name__ == '__main__':
    sys.exit(main())
