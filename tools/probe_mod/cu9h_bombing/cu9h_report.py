#!/usr/bin/env python3
"""cu9h_report.py - report of one cu9h bombing-run probe run (roadmap group cu9h t02; docs/re/HUD_GAPS.md sec. 9.9).

  cu9h_report.py RUN_DIR [--exe <game>\\corruption\\StarWarsG.exe] [--out RUN_DIR\\report.txt]

Inputs in RUN_DIR (written by cu9h_run.py, cu9h_dbg.py and cu9h_mark.py):
  reader.log  "<time> S <state>" on every change of the bombing-run state (cu9h_mem.KEYS; the ground point, cursor and
              frames do not count as a change) and "<time> T <state>" every 0.1 s while targeting is on
  dbg.log     the watchpoint hits (cu9h_dbg.py): writes of the ground point x / z, the targeting flag, the blob pointer
  marks.log   "<time> MARK label=<label> <state>": the driver's marks (cu9h_mark.py), one per screenshot / click
Sections Q1..Q4 answer the four items of HUD_GAPS sec. 9.9; Q1 and the colour of Q2 also need the driver's screenshots
(the report says which marks to compare). Function starts of the writers come from the exe's .pdata
(../cu8h_probes/cu8h_dbg_parse.Funcs; without the exe the raw RVAs are printed).
"""
import argparse
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'cu8h_probes'))
import cu9h_mem as M  # noqa: E402

DEFAULT_EXE = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption\StarWarsG.exe'
KNOWN = {0x2ee40: 'world_mouse_move(0x2ee40)', 0x3035c0: 'hud_reset(0x3035c0)', 0x30c3b0: 'hud_frame(0x30c3b0)',
         0x312570: 'land_hud_update(0x312570)', 0x2e7950: 'targeting_toggle(0x2e7950)',
         0x346a80: 'b_special_weapon_click(0x346a80)', 0x2f6c30: 'hud_mouse(0x2f6c30)', 0x63f00: 'hotkey(0x63f00)',
         0x306290: 'hud_mode_switch(0x306290)', 0x346de0: 'hud_drag_start(0x346de0)', 0x60c60: 'input_dispatch(0x60c60)'}


def load_state_lines(path, kinds=('S', 'T', 'MARK')):
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path, encoding='ascii', errors='replace'):
        parts = line.rstrip('\n').split(' ', 2)
        if len(parts) < 3 or parts[1] not in kinds:
            continue
        try:
            t = float(parts[0])
        except ValueError:
            continue
        d = M.parse_line(parts[2])
        d['_t'], d['_kind'] = t, parts[1]
        out.append(d)
    return out


def load_dbg(path):
    hits, other = [], []
    if not os.path.exists(path):
        return hits, other
    for line in open(path, encoding='ascii', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('HIT '):
            head, _, walk = line.partition(' walk=')
            d = M.parse_line(head[4:])
            d['walk'] = [w for w in walk.split(',') if w]
            hits.append(d)
        elif line:
            other.append(line)
    return hits, other


class Names:
    def __init__(self, exe):
        self.f = None
        try:
            import cu8h_dbg_parse
            f = cu8h_dbg_parse.Funcs(exe)
            self.f = f if f.starts else None
        except Exception:  # noqa: BLE001
            self.f = None

    def fn(self, tok, ret):
        if tok.startswith('m:'):
            return tok
        r = int(tok, 16)
        s = self.f.of(r - 1 if ret else r) if self.f else r
        return KNOWN.get(s, '%x' % s)

    def chain(self, walk, n=5):
        return [self.fn(w, i > 0) for i, w in enumerate(walk[:n])]


def num(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def report(run_dir, exe=DEFAULT_EXE, out=None):
    L = []
    names = Names(exe)
    rd = load_state_lines(os.path.join(run_dir, 'reader.log'), ('S', 'T'))
    marks = load_state_lines(os.path.join(run_dir, 'marks.log'), ('MARK',))
    hits, dbg_other = load_dbg(os.path.join(run_dir, 'dbg.log'))
    L.append('cu9h t02 bombing-run probe report: %s' % run_dir)
    L.append('reader samples %d, marks %d, debugger hits %d, function names %s' % (
        len(rd), len(marks), len(hits), 'from the exe' if names.f else 'NOT available (raw RVAs)'))
    battle = [d for d in rd if d.get('phase') not in (None, '-', 'NO_GAME', 'GALAXY', 'MENU_OR_SKIRMISH', 'SPACE', 'OTHER')]
    rec = [d for d in battle if d.get('rec_en') == '1']
    L.append('')
    L.append('== battle / record ==')
    if not battle:
        L.append('  no campaign land battle seen (phase never left the galaxy / menu)')
    else:
        L.append('  first battle sample: %s' % ' '.join('%s=%s' % (k, battle[0].get(k)) for k in (
            'phase', 'local_id', 'recs_enabled', 'mgr_en', 'mgr_interval', 'rec_int', 'rec_ready', 'rec_last')))
    if not rec:
        L.append('  NO RECORD for the local player: no bombers counted in orbit, or the record went to another player '
                 '(recs_enabled above) - HUD_GAPS sec. 9.2 "no record" case; Q1..Q3 cannot be answered from this run')
    starts = sorted({num(d.get('rec_last')) for d in rec if num(d.get('rec_last')) is not None})
    L.append('  record last-start frames seen: %s; intervals: %s' % (starts, sorted({d.get('rec_int') for d in rec})))
    prev = None
    trans = []
    for d in rd:
        k = (d.get('phase'), d.get('comp_disabled'))
        if k != prev:
            trans.append(d)
            prev = k
    L.append('  phase / button transitions:')
    for d in trans:
        L.append('    t=%.2f mf=%s phase=%s button_disabled=%s state=%s dial=%s expected=%s rec_av=%s inp=%s ready=%s' % (
            d['_t'], d.get('mf'), d.get('phase'), d.get('comp_disabled'), d.get('comp_state'), d.get('comp_dial'),
            d.get('dial_expected'), d.get('rec_av'), d.get('rec_inp'), d.get('rec_ready')))

    # ---- Q1
    L.append('')
    L.append('== Q1: the b_special_weapon dial at 1.0 (HUD_GAPS sec. 9.9 item 1) ==')
    ready = [d for d in rec if d.get('ready') == '1']
    cool = [d for d in rec if d.get('phase') == 'COOLDOWN']
    rdials = sorted({d.get('comp_dial') for d in ready})
    L.append('  READY samples %d: button disabled values %s, dial values %s (expected 0 / 100)' % (
        len(ready), sorted({d.get('comp_disabled') for d in ready}), rdials))
    cd = [num(d.get('comp_dial')) for d in cool if num(d.get('comp_dial')) is not None]
    L.append('  COOLDOWN samples %d: button disabled values %s, dial %s..%s' % (
        len(cool), sorted({d.get('comp_disabled') for d in cool}), min(cd) if cd else '-', max(cd) if cd else '-'))
    mism = [d for d in rec if num(d.get('dial_expected')) is not None and num(d.get('comp_dial')) is not None
            and abs(num(d['dial_expected']) - num(d['comp_dial'])) > 1]
    both = [d for d in rec if num(d.get('dial_expected')) is not None and num(d.get('comp_dial')) is not None]
    L.append('  dial vs the sec. 9.3 formula: %d samples compared, %d differ by more than 1%s' % (
        len(both), len(mism), '' if not mism else ' (first: ' + '; '.join(
            'mf=%s dial=%s expected=%s rec_av=%s ready=%s last=%s' % (d.get('mf'), d.get('comp_dial'),
                                                                       d.get('dial_expected'), d.get('rec_av'),
                                                                       d.get('rec_ready'), d.get('rec_last'))
            for d in mism[:3]) + ')'))
    q1 = [m for m in marks if m.get('label', '').lower().startswith('q1')]
    L.append('  q1 marks (pair each with its screenshot):')
    for m in q1 or marks[:0]:
        L.append('    %s: phase=%s button=%s dial=%s' % (m.get('label'), m.get('phase'),
                                                          'DISABLED' if m.get('comp_disabled') == '1' else 'ENABLED',
                                                          m.get('comp_dial')))
    has_ready = any(m.get('phase') == 'READY' for m in q1)
    has_cool = any(m.get('phase') == 'COOLDOWN' for m in q1)
    L.append('  VERDICT Q1 (memory): %s. The answer itself is visual: a READY zoom with the full i_button_build '
             'overlay = "full", a READY zoom that looks like a plain enabled button = "nothing".' % (
                 'READY and COOLDOWN marks present' if has_ready and has_cool else
                 'MISSING %s marks' % ' and '.join(x for x, ok in (('READY', has_ready), ('COOLDOWN', has_cool)) if not ok)))

    # ---- Q2
    L.append('')
    L.append('== Q2: blob colours while targeting (item 2) ==')
    tgt = [d for d in rd if d.get('flag') == '1']
    cols = {}
    for d in tgt:
        cols.setdefault(d.get('blob_col'), []).append(d)
    for c, ds in sorted(cols.items(), key=lambda x: str(x[0])):
        L.append('  %s: %d samples, colour values %s' % (c, len(ds), sorted({d.get('colour') for d in ds})[:4]))
    q2 = [m for m in marks if m.get('label', '').lower().startswith('q2')]
    L.append('  q2 marks:')
    for m in q2:
        L.append('    %s: targeting=%s blob=%s colour=%s ground=%s cursor=%s' % (
            m.get('label'), m.get('flag'), m.get('blob_col'), m.get('colour'), m.get('ground'), m.get('cursor')))
    g, r = 'GREEN' in cols, 'RED' in cols
    L.append('  VERDICT Q2 (memory): %s. Check the screenshots of the q2 marks: a RED mark (1,0,0,1) must look red and '
             'a GREEN one (0,1,0,1) green for the RGBA order.' % (
                 'both GREEN (0,1,0,1) and RED (1,0,0,1) written' if g and r else
                 'only %s seen' % ('GREEN' if g else 'RED' if r else 'no colour')))

    # ---- Q3
    L.append('')
    L.append('== Q3: does a click on the DISABLED button run its callback (item 3) ==')
    fl = [h for h in hits if h.get('what') == 'flag']
    armed = any(ln.startswith('ARM mask=') and 'mask=none' not in ln for ln in dbg_other)
    L.append('  debugger: %s; %d writes of the targeting flag (+0x20b2)' % (
        'armed in a battle' if armed else 'NEVER ARMED (no battle seen, or dbg.log missing)', len(fl)))
    bad, good = [], []
    for h in fl:
        ch = names.chain(h['walk'])
        L.append('    t=%s mf=%s flag:=%s button_disabled=%s ready=%s rec_av=%s inp=%s writer=%s' % (
            h.get('t'), h.get('mf'), h.get('val'), h.get('comp_disabled'), h.get('ready'), h.get('rec_av'),
            h.get('rec_inp'), ' <- '.join(ch)))
        if h.get('val') == '1':
            (bad if h.get('comp_disabled') == '1' or h.get('ready') == '0' else good).append(h)
    rflag = [d for d in rd if d.get('flag') == '1' and d.get('comp_disabled') == '1']
    q3 = [m for m in marks if m.get('label', '').lower().startswith('q3')]
    L.append('  q3 marks: %s' % ', '.join('%s (button %s, targeting %s)' % (
        m.get('label'), 'DISABLED' if m.get('comp_disabled') == '1' else 'ENABLED', m.get('flag')) for m in q3))
    L.append('  reader samples with targeting on while the button is disabled: %d' % len(rflag))
    if bad:
        v = 'CALLBACK_RAN: the flag was set while the button was disabled (%d times; writer %s)' % (
            len(bad), ' <- '.join(names.chain(bad[0]['walk'])))
    elif q3 and armed:
        v = 'CALLBACK_NOT_RUN: %d disabled-button click(s) marked and no flag:=1 write while disabled' % len(q3)
    else:
        v = 'INCONCLUSIVE: %s' % ('no q3 mark (click the button during the cooldown, then mark q3)' if not q3 else
                                  'the debugger was not armed')
    L.append('  VERDICT Q3: %s; control (flag:=1 by an enabled click): %s' % (
        v, '%d, writer %s' % (len(good), ' <- '.join(names.chain(good[0]['walk']))) if good else 'NONE SEEN'))

    # ---- Q4
    L.append('')
    L.append('== Q4: the writer of the HUD ground point +0x26dc (item 4) ==')
    gr = [h for h in hits if h.get('what') in ('ground_x', 'ground_z')]
    groups = {}
    for h in gr:
        key = (h['what'], tuple(names.chain(h['walk'], 4)))
        e = groups.setdefault(key, {'n': 0, 'tgt': 0, 'vals': set()})
        e['n'] += 1
        e['tgt'] += h.get('flag') == '1'
        if len(e['vals']) < 4:
            e['vals'].add(h.get('val'))
    L.append('  %d ground-point writes caught (the watchpoint has a budget per targeting session)' % len(gr))
    for (what, ch), e in sorted(groups.items(), key=lambda x: -x[1]['n']):
        L.append('    %-8s n=%-5d targeting_on=%-5d %s   values e.g. %s' % (what, e['n'], e['tgt'], ' <- '.join(ch),
                                                                         sorted(e['vals'])))
    writers = sorted({ch[0] for (what, ch) in groups if what == 'ground_x' and ch})
    L.append('  VERDICT Q4: writers of +0x26dc: %s' % (', '.join(writers) if writers else 'NONE CAUGHT'))
    L.append('')
    L.append('== debugger log (non-hit lines) ==')
    L.extend('  ' + ln for ln in dbg_other[:60])
    text = '\n'.join(L) + '\n'
    with open(out or os.path.join(run_dir, 'report.txt'), 'w') as f:
        f.write(text)
    return text, len(battle)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run_dir')
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--out')
    a = ap.parse_args()
    text, n = report(a.run_dir, a.exe, a.out)
    print(text)
    return 0 if n else 10


if __name__ == '__main__':
    sys.exit(main())
