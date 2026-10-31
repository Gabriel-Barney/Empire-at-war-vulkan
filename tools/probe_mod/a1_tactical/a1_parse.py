#!/usr/bin/env python3
"""a1_parse.py - turns an a1_reader.py log into the two A1 probe reports.

  a1_parse.py parse <reader.log> [--out report.txt] [--json report.json]
  a1_parse.py model [--frames 2000]          print the predicted free-store schedules (no log needed)

Free-store timing (A1 t02, AI_TACTICAL §5.3 / §12 item 10). The free store is serviced every 3rd frame (phase
unknown: 0, 1 or 2 mod 3). FreeStoreService runs when now - LastService > 20, On_Unit_Service passes when
now - LastUnitService > 2, both stamps start at 0. Three readings of `now`:
  float  now = (float)F * (1/30 as float), the subtraction in float (the A1 reading: F = 600, 1200, 1800 ...)
  exact  now = F / 30 in real arithmetic: 603, 1206, 1809 (phase 0), the 'about every 20.1 s' of AI_TACTICAL §5.3
Predicted with phase 0 (a1_parse.py model): float 600, 1200, 1803, 2403 (at 1800 the float subtraction is exactly
20.0, not > 20); exact 603, 1206, 1809. Unit passes: float 63, 123, 186, 246; exact 63, 126, 189, 252.
For every script instance (sid) the report lists the frames at which the engine called FreeStoreService and started
On_Unit_Service passes, and which (reading, phase) reproduce them.

Landing points (A1 t04, PHASE4 A1 "Land reinforcements", AI_GALACTIC_3 §3.3). Per land battle: the players (id,
faction, human, population capacity +0x404 at the first frame and every change), every REINFORCEMENT_POINT* with
its owner at first sight and every owner change, and per player the capacity computed from what it holds; then the
answer lines: which points the attacker (campaign: the non-human side with units; skirmish: both) owns at the first
frame, its +0x404, the effective land cap by §3.3, and Base_Population_Capacity = +0x404 - held capacity.
"""
import argparse
import json
import re
import struct
import sys
from fractions import Fraction

sys.dont_write_bytecode = True

LINE_RE = re.compile(r'(\w+)=(\S*)')


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


INV30 = f32(1.0 / 30.0)


def now_of(model, F):
    if model == 'float':
        return f32(f32(float(F)) * INV30)
    return Fraction(F, 30)


def fires(model, phase, rate, frames=4000):
    """Frames at which a check `now - last > rate` passes (last starts at 0, set to now when it passes)."""
    out = []
    last = 0.0 if model == 'float' else Fraction(0)
    for F in range(phase, frames, 3):
        now = now_of(model, F)
        d = f32(now - last) if model == 'float' else now - last
        if d > rate:
            out.append(F)
            last = now
    return out


MODELS = [(m, p) for m in ('float', 'exact') for p in (0, 1, 2)]


def match_models(observed, rate, subset=False):
    """(model, phase) whose predicted list reproduces the observed frames (prefix equality, or subset)."""
    res = []
    if not observed:
        return res
    for m, p in MODELS:
        pred = fires(m, p, rate, max(observed) + 10)
        if subset:
            if set(observed) <= set(pred):
                res.append('%s/p%d' % (m, p))
        elif pred[:len(observed)] == observed:
            res.append('%s/p%d' % (m, p))
    return res


def parse_lines(path):
    rows = []
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line:
                continue
            tag = line.split(' ', 1)[0]
            kv = dict(LINE_RE.findall(line))
            if tag == 'LUA':
                kv['rec'] = line.split(' rec=', 1)[1] if ' rec=' in line else ''
            rows.append((tag, kv, line))
    return rows


def rec_fields(text):
    parts = text.split('|')
    d = {'kind': parts[1]}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    return d


def frame_of(r):
    try:
        return int(round(float(r['t']) * 30))
    except (KeyError, ValueError):
        return None


def freestore_report(rows, out):
    recs = [rec_fields(kv['rec']) for tag, kv, _ in rows if tag == 'LUA' and kv.get('rec', '').startswith('A1P1|')]
    recs = [r for r in recs if r.get('sid', '?') != '?' and 't' in r]  # drop text that only looks like a record
    srcs = [kv for tag, kv, _ in rows if tag == 'SRC']
    scans = [kv for tag, kv, _ in rows if tag == 'SCAN']
    out.append('== Free-store timing probe (A1 t02) ==')
    out.append('heap scans: %d (%s MB per scan); probe source text seen in memory: %s; records found: %d' % (
        len(scans), scans[-1]['mb'] if scans else '-', 'yes (mode %s)' % ','.join(sorted({s['n'] for s in srcs}))
        if srcs else 'no', len(recs)))
    result = {'instances': []}
    if not recs:
        out.append('NO PROBE RECORDS: either the hooked BusyTacticalFreeStore.lua was not loaded from the mod (no SRC line '
                   'either: the mod file was never read), or no AI free store ran (no AI player in a tactical battle), '
                   'or the scan missed them (see SCAN lines).')
        return result
    by_sid = {}
    for r in recs:
        by_sid.setdefault(r.get('sid', '?'), []).append(r)
    for sid, rs in sorted(by_sid.items(), key=lambda kv_: min(int(float(r.get('gf', 0)) if r.get('gf', '?') != '?'
                                                                   else 0) for r in kv_[1])):
        head = next((r for r in rs if r.get('pid', '?') != '?'), rs[0])
        out.append('')
        out.append('-- instance sid=%s player id=%s faction=%s mode=%s' % (sid, head.get('pid'), head.get('fac'),
                                                                         head.get('mode')))
        inst = {'sid': sid, 'pid': head.get('pid'), 'fac': head.get('fac'), 'mode': head.get('mode')}
        gf_off = sorted({int(float(r['gf'])) - frame_of(r) for r in rs if r.get('gf', '?') != '?'
                         and frame_of(r) is not None})
        for kind in ('LOAD', 'BASEDEF', 'MAIN', 'FAC'):
            for r in rs:
                if r['kind'] == kind:
                    extra = ''
                    if kind == 'FAC':
                        extra = ' ' + ' '.join('%s=%s' % (k, v) for k, v in r.items() if k[:1].isupper())
                    out.append('   %-8s F=%s  t=%s%s' % (kind, frame_of(r), r.get('t'), extra))
                    inst[kind.lower()] = frame_of(r)
        fss = sorted((int(r['n']), frame_of(r), r.get('t')) for r in rs if r['kind'] == 'FSS')
        ous = sorted((int(r['batch']), frame_of(r), r.get('units_prev'), r.get('unit')) for r in rs if r['kind'] == 'OUS')
        add = sorted((int(r['n']), frame_of(r), r.get('unit')) for r in rs if r['kind'] == 'ADD')
        out.append('   FreeStoreService calls: ' + (', '.join('#%d F=%s (t=%s)' % x for x in fss) or 'none'))
        out.append('   On_Unit_Service passes: ' + (', '.join('#%d F=%s' % x[:2] for x in ous) or 'none'))
        if ous:
            out.append('      units per pass: ' + ', '.join(str(x[2]) for x in ous[1:]) + ' (pass 1 unit: %s)' % ous[0][3])
        out.append('   On_Unit_Added: ' + (', '.join('#%d F=%s %s' % x for x in add) or 'none'))
        out.append('   GetCurrentTime.Frame() - F: %s' % (gf_off or '?'))
        fss_frames = [x[1] for x in fss if x[1] is not None]
        early = [F for F in fss_frames if F < 300]
        late = [F for F in fss_frames if F >= 300]
        inst.update(fss=fss_frames, ous=[x[1] for x in ous], add=[x[1] for x in add])
        if early:
            out.append('   NOTE: FreeStoreService ran at F=%s, before the first 20 s period: main() runs the service once '
                       'when the script starts, the timer only gates the later resumes' % early)
        m_fss = match_models(late, 20.0)
        m_ous = match_models([x[1] for x in ous if x[1] is not None], 2.0, subset=True)
        out.append('   readings that reproduce the FreeStoreService frames %s: %s' % (late, ', '.join(m_fss) or 'NONE'))
        out.append('   readings whose unit-pass frames contain every observed pass: %s' % (', '.join(m_ous) or 'NONE'))
        inst.update(fss_models=m_fss, ous_models=m_ous)
        verdict = []
        if late:
            if 'float/p0' in m_fss:
                verdict.append('A1 reading CONFIRMED: scheduled FreeStoreService at F=%s (float clock, phase 0)' % late)
            elif 'exact/p0' in m_fss:
                verdict.append('A1 reading REFUTED: scheduled FreeStoreService at F=%s (exact clock: the ~20.1 s of '
                               '§5.3)' % late)
            elif late[0] == 600:
                verdict.append('A1 reading PARTLY confirmed: first scheduled FreeStoreService at F=600, but the later '
                               'calls %s fit no reading (see the readings line)' % late[1:])
            else:
                verdict.append('scheduled FreeStoreService at F=%s: neither the float nor the exact clock with phase '
                               '0 (readings: %s)' % (late, ', '.join(m_fss) or 'none'))
        if ous and ous[0][1] is not None:
            verdict.append('first On_Unit_Service pass at F=%d (%s the A1 reading F=63)' % (
                ous[0][1], 'matches' if ous[0][1] == 63 else 'differs from'))
        for v in verdict:
            out.append('   => ' + v)
        inst['verdict'] = verdict
        result['instances'].append(inst)
    return result


def landing_report(rows, out):
    out.append('')
    out.append('== Landing-point ownership probe (A1 t04) ==')
    modes = {}
    for tag, kv, _ in rows:
        n = kv.get('n')
        if tag == 'MODE':
            modes[n] = {'mode': kv, 'players': {}, 'points': {}, 'pcap': [], 'calc': {}, 'calc_hist': [], 'owners': {},
                        'pown': []}
        if n not in modes:
            continue
        m = modes[n]
        if tag == 'P':
            m['players'][kv['id']] = dict(kv, popcap0=kv.get('popcap'))
        elif tag == 'PCAP':
            m['pcap'].append(kv)
        elif tag == 'POINT':
            m['points'][kv['obj']] = dict(kv, owner0=kv['owner'], frame0=kv['frame'], changes=[])
        elif tag == 'POWN':
            m['pown'].append(kv)
            if kv['obj'] in m['points']:
                m['points'][kv['obj']]['changes'].append((kv['frame'], kv['owner']))
        elif tag == 'CALC':
            m['calc_hist'].append(kv)
            m['calc'].setdefault(kv['id'], []).append((kv['frame'], kv['calc']))
        elif tag == 'OWNERS':
            m['owners'].setdefault(kv['id'], []).append(kv)
    result = {'battles': []}
    land = {n: m for n, m in modes.items() if m['mode'].get('type') == '1'}
    if not land:
        out.append('no land battle in this log')
        return result
    for n, m in sorted(land.items(), key=lambda kv_: int(kv_[0])):
        md = m['mode']
        campaign = int(md.get('stack', '1') or 1) >= 2
        out.append('')
        out.append('-- land battle %s: %s (game_type=%s, stack=%s), first seen at mode frame %s' % (
            n, 'campaign battle' if campaign else 'skirmish', md.get('game_type'), md.get('stack'), md.get('frame')))
        out.append('   player  faction      human neutral  +0x404 first -> changes           held cap first   '
                   'objects (top types)')
        info = []
        for pid, p in sorted(m['players'].items(), key=lambda kv_: int(kv_[0])):
            ch = ['F%s:%s' % (c['frame'], c['popcap']) for c in m['pcap'] if c['id'] == pid]
            calc = m['calc'].get(pid, [])
            own = m['owners'].get(pid, [])
            out.append('   %-6s  %-12s %-5s %-7s  %-6s %-24s %-15s %s' % (
                pid, p.get('faction'), p.get('human'), p.get('neutral'), p.get('popcap0'), ' '.join(ch) or '-',
                ' '.join('%s@F%s' % (c[1], c[0]) for c in calc[:4]) or '-',
                ('%s (%s)' % (own[0]['count'], own[0]['top'])) if own else '-'))
            info.append((pid, p, calc, own))
        out.append('   reinforcement points (first sight owner @ frame -> changes):')
        for obj, pt in sorted(m['points'].items(), key=lambda kv_: int(kv_[0])):
            out.append('     obj %-6s %-40s cap %-4s at (%s, %s): owner %s @F%s%s' % (
                obj, pt['type'], pt.get('addcap'), pt.get('x'), pt.get('y'), pt['owner0'], pt['frame0'],
                ''.join('  -> %s @F%s' % (o, f) for f, o in pt['changes'])))
        # answers
        human = [pid for pid, p, _, _ in info if p.get('human') == '1']
        active = [pid for pid, p, calc, own in info if own and p.get('neutral') != '1'
                  and p.get('faction') not in ('NEUTRAL', 'HOSTILE')]
        # Every playing side: in the fallback run (human = Underworld) the human is the attacker, so the side that
        # holds a landing point at first sight is the attacker, not "the non-human side".
        attackers = active
        out.append('   answers (%s):' % ('campaign: every playing side; the attacker is the one holding its landing point '
                                         'at first sight' if campaign else 'skirmish: every playing side'))
        bat = {'n': n, 'campaign': campaign, 'first_frame': md.get('frame'), 'players': {}}
        f_first = int(md.get('frame') or 0)
        f_last = max([f_first] + [int(kv['frame']) for kv in m['pcap'] + m['calc_hist'] + m['pown']
                                  if kv.get('frame', 'None') != 'None'])
        checkpoints = sorted({f_first, 30, 300, f_last})

        def at(history, F, start):
            v = start
            for f, val in history:
                if int(f) <= F:
                    v = val
            return v

        for pid in attackers:
            p = m['players'][pid]
            pts0 = [pt for pt in m['points'].values() if pt['owner0'] == pid]
            first_own = min([int(pt['frame0']) for pt in pts0] +
                            [int(f) for pt in m['points'].values() for f, o in pt['changes']
                             if o.split('->')[-1] == pid], default=None)
            pc_hist = [(c['frame'], c['popcap'].split('->')[-1]) for c in m['pcap'] if c['id'] == pid]
            calc_hist = m['calc'].get(pid, [])
            rows_ = []
            for F in checkpoints:
                pc = int(at(pc_hist, F, p.get('popcap0') or 0))
                held = int(at(calc_hist, F, 0))
                rows_.append({'frame': F, 'popcap': pc, 'held': held, 'base': pc - held,
                              'cap': min(10, pc) if campaign else pc})
            out.append('     player %s (%s%s): %d reinforcement point(s) owned at first sight%s; first frame owning one: %s' % (
                pid, p.get('faction'), ', human' if pid in human else '', len(pts0),
                ' (' + ', '.join('%s @F%s' % (pt['type'], pt['frame0']) for pt in pts0) + ')' if pts0 else '',
                first_own))
            for r in rows_:
                out.append('       F=%-6s +0x404=%-4s held capacity=%-4s -> Base_Population_Capacity = +0x404 - held = %-4s '
                           '%s' % (r['frame'], r['popcap'], r['held'], r['base'],
                                   'effective land cap min(10, +0x404) = %d' % r['cap'] if campaign else
                                   '(skirmish cap = min(+0x404, unit-cap curve 10))'))
            bat['players'][pid] = {'faction': p.get('faction'), 'points_at_first_sight': len(pts0),
                                   'first_frame_owning': first_own, 'checkpoints': rows_}
        result['battles'].append(bat)
    return result


def cmd_parse(a):
    rows = parse_lines(a.log)
    out = ['A1 tactical probes: %s' % a.log]
    for tag, kv, line in rows:
        if tag in ('GAME', 'EXIT'):
            out.append('   ' + line)
    modes = [kv for tag, kv, _ in rows if tag == 'MODE']
    out.append('   tactical modes: ' + (', '.join('#%s %s stack=%s' % (m['n'], {'1': 'land', '2': 'space'}.get(
        m.get('type'), m.get('type')), m.get('stack')) for m in modes) or 'none'))
    out.append('')
    res = {'freestore': freestore_report(rows, out), 'landing': landing_report(rows, out)}
    text = '\n'.join(out) + '\n'
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(text)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(res, f, indent=1)
    sys.stdout.write(text)
    return res


def cmd_model(a):
    for rate, what in ((20.0, 'FreeStoreService'), (2.0, 'On_Unit_Service')):
        print('%s (rate %g):' % (what, rate))
        for m, p in MODELS:
            print('  %-6s phase %d: %s' % (m, p, fires(m, p, rate, a.frames)[:8]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('parse')
    p.add_argument('log')
    p.add_argument('--out')
    p.add_argument('--json')
    p.set_defaults(fn=cmd_parse)
    p = sub.add_parser('model')
    p.add_argument('--frames', type=int, default=2000)
    p.set_defaults(fn=cmd_model)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
