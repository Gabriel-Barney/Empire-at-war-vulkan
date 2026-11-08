#!/usr/bin/env python3
"""cu8g_parse.py - turns the records of a cu8g probe session (records.log, one "C8G|...|END" string per line, as
cu8g_run.py found them in the game's memory) into report.txt.

  cu8g_parse.py <records.log> [--out report.txt]

Sections: script status (LOAD / ARM / SKIP / APIERR / ERR / DONE per script instance), then one per probe found:
stick (t07), lure (t09a), cancel (t09b), maps (t08: galactic timeline + tactical map fingerprints). Each section ends
with a verdict line and what it means for the port (README.md "What each result decides").
"""
import argparse
import collections
import os
import sys

FPS = 30.0
LOW_HEALTH = 0.33   # GameConstants Health_Low_Percent_Threshold (FoC), TACTICAL_COMBAT sec. 2.4 rule 1


def parse_line(text):
    parts = text.strip().split('|')
    if len(parts) < 3 or parts[0] != 'C8G' or parts[-1] != 'END':
        return None
    r = {'kind': parts[1]}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            r.setdefault(k, v)
    r['_f'] = num(r.get('f'))
    r['_n'] = num(r.get('n'))
    return r


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load(path):
    recs = []
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            r = parse_line(line)
            if r:
                recs.append(r)
    # the heap scan finds records in address order: sort each script instance by its record counter
    recs.sort(key=lambda r: (r.get('sid', ''), r['_n'] if r['_n'] is not None else 0))
    return recs


def fmt_f(f):
    return '?' if f is None else '%d (%.1f s)' % (f, f / FPS)


# ---- status -------------------------------------------------------------------------------------------------------

def sec_status(recs):
    out = ['== Script instances ==']
    by = collections.OrderedDict()
    for r in recs:
        by.setdefault(r.get('sid', '?'), []).append(r)
    for sid, rs in by.items():
        kinds = collections.Counter(r['kind'] for r in rs)
        arm = next((r for r in rs if r['kind'] in ('ARM', 'SKIP', 'START')), None)
        desc = ''
        if arm is not None:
            desc = ' '.join('%s=%s' % (k, arm.get(k)) for k in ('p', 'fac', 'mode', 'session') if arm.get(k))
        out.append('sid %s: %d records %s %s' % (sid, len(rs), dict(kinds), desc))
        for r in rs:
            if r['kind'] in ('APIERR', 'ERR'):
                out.append('   %s f=%s %s' % (r['kind'], r.get('f'), ' '.join(
                    '%s=%s' % (k, v) for k, v in r.items() if not k.startswith('_') and k not in (
                        'kind', 'p', 'sid', 'n', 'f'))))
            if r['kind'] == 'DONE':
                out.append('   DONE f=%s %s' % (r.get('f'), r.get('apierr', '')))
        # cu8g t10: where an armed script stopped: its last heartbeat and its last records by frame (ST = step trace)
        if arm is not None and arm['kind'] == 'ARM' and not any(r['kind'] == 'DONE' for r in rs):
            byf = sorted((r for r in rs if r.get('_f') is not None), key=lambda r: (r['_f'], num(r.get('n')) or 0))
            hb = [r for r in byf if r['kind'] == 'HB']
            out.append('   no DONE: last heartbeat f=%s; last records: %s' % (
                hb[-1].get('f') if hb else None,
                ' | '.join('%s f=%s %s' % (r['kind'], r.get('f'), r.get('s', '')) for r in byf[-6:])))
    return out


# ---- t07 stickiness -----------------------------------------------------------------------------------------------

def vfield(v):
    """'1:hull:shield:ttd:rate' -> dict (valid False when '0')."""
    if not v or v[0] != '1':
        return {'valid': False}
    p = v.split(':')
    return {'valid': True, 'hull': num(p[1]) if len(p) > 1 else None, 'shield': num(p[2]) if len(p) > 2 else None,
            'ttd': num(p[3]) if len(p) > 3 else None, 'rate': num(p[4]) if len(p) > 4 else None}


def stick_trials(recs):
    trials = collections.OrderedDict()
    for r in recs:
        if r.get('p') != 'stick' or 'k' not in r:
            continue
        t = trials.setdefault((r.get('sid'), r['k']), {'k': r['k'], 'polls': []})
        kind = r['kind']
        if kind == 'TRIAL':
            t.update(arm=r.get('arm'), value=r.get('value'), set=r.get('set'), f0=r['_f'], F=r.get('F'), V=r.get('V'))
        elif kind == 'ACQ':
            t.update(acq=r.get('after'), facq=r['_f'])
        elif kind == 'NEWCOMER':
            t.update(fn=r['_f'], cond=r.get('cond'), nvhull=num(r.get('vhull')), nvttd=num(r.get('vttd')),
                     ntype=r.get('type'), nhow=r.get('how'), ntgt=r.get('tgt'), dFN=r.get('dFN'), dFV=r.get('dFV'))
        elif kind == 'P':
            t['polls'].append((r['_f'], r.get('tgt', '?'), vfield(r.get('v')), r.get('nc')))
            t.setdefault('arm', r.get('arm'))
        elif kind == 'VDEAD':
            t['fvdead'] = r['_f']
        elif kind == 'TEND':
            t['why'] = r.get('why')
    return list(trials.values())


def stick_verdict(t):
    if t.get('fn') is None:
        return 'INCOMPLETE', 'no newcomer'
    after = [p for p in t['polls'] if p[0] is not None and p[0] >= t['fn']]
    fv = t.get('fvdead')
    sw = next((p for p in after if p[1].startswith('N') and (fv is None or p[0] < fv)), None)
    low = t.get('nvhull') is not None and t['nvhull'] <= LOW_HEALTH
    if sw is not None:
        prev = [p for p in after if p[0] < sw[0] and p[2].get('valid')]
        vh = prev[-1][2] if prev else {}
        return 'SWITCHED', 'to N %d frames after it appeared; V then hull %s ttd %s' % (
            sw[0] - t['fn'], vh.get('hull'), vh.get('ttd'))
    if fv is not None:
        tg = collections.Counter(p[1] for p in after if p[0] < fv)
        if set(tg) <= {'V', 'V~'}:
            return ('KEPT_LOW_HEALTH' if low else 'KEPT'), 'stayed on V until it died %d frames after N' % (fv - t['fn'])
        return 'OTHER', 'targets before V died: %s' % dict(tg)
    return 'OTHER', 'V did not die in the watch window (%s)' % t.get('why')


def sec_stick(recs):
    trials = stick_trials(recs)
    if not trials:
        return []
    out = ['', '== t07 stickiness (CU8F_T04_T06_RE.md sec. 1.5) ==',
           'arm A: no Lua call; B: Set_Targeting_Stickiness_Time_Threshold(-1) (= XML 5 s); C: (1000).',
           'N appears when V time-till-dead <= 4 s with hull >= 0.5 (cond=ttk); a SWITCHED trial means F was not sticky.']
    per_arm = collections.defaultdict(list)
    for t in trials:
        v, why = stick_verdict(t)
        per_arm[t.get('arm')].append(v)
        out.append('trial %s arm %s set=%s: acquire %s, N(%s, %s) at %s cond=%s V hull %s ttd %s dFN %s dFV %s -> %s: %s' % (
            t['k'], t.get('arm'), t.get('set'), t.get('acq'), t.get('ntype'), t.get('nhow'), fmt_f(t.get('fn')),
            t.get('cond'), t.get('nvhull'), t.get('nvttd'), t.get('dFN'), t.get('dFV'), v, why))
        tl = []
        last = None
        for f, tgt, vf, n in t['polls']:
            if tgt != last:
                tl.append('%s@%s' % (tgt, f if f is None else int(f)))
                last = tgt
        out.append('   target timeline: ' + ' '.join(tl))
    summ = {a: collections.Counter(v) for a, v in per_arm.items()}
    out.append('per arm: ' + '; '.join('%s %s' % (a, dict(c)) for a, c in sorted(summ.items(), key=lambda x: str(x[0]))))

    def mostly(arm, v):
        c = summ.get(arm)
        return bool(c) and c[v] * 2 > sum(c.values())
    if mostly('A', 'SWITCHED') and mostly('B', 'KEPT') and not mostly('C', 'SWITCHED'):
        verdict = ('CONFIRMED: spawn default never sticky (0.0); -1 = the XML value. The port rule (cu8f t04a) stands; '
                   'the foc-station cap seeds of cu8g t06 are not a stickiness-default artefact.')
    elif mostly('A', 'KEPT') and mostly('B', 'KEPT'):
        verdict = ('DIFFERS: the spawn default is sticky (the XML 5 s applies from spawn). Revert the spawn 0.0 of '
                   'cu8f t04a (port: initialise the override to -1) and regenerate the goldens; recheck cu8g t06.')
    elif mostly('A', 'SWITCHED') and mostly('B', 'SWITCHED') and mostly('C', 'KEPT'):
        verdict = ('PARTLY: the setter works (C keeps) but -1 did not give a 5 s threshold: check V ttd at N (must '
                   'be <= 5); if it was, a negative value does not mean "use the XML value" (sec. 1.2 step 2).')
    else:
        verdict = 'INCONCLUSIVE: see the trial lines (low health, no acquisition, newcomer never targeted).'
    out.append('VERDICT t07: ' + verdict)
    return out


# ---- t09a lure ----------------------------------------------------------------------------------------------------

def sec_lure(recs):
    trials = collections.OrderedDict()
    for r in recs:
        if r.get('p') != 'lure' or 'k' not in r:
            continue
        t = trials.setdefault((r.get('sid'), r['k']), {'k': r['k'], 'polls': [], 'diverts': [], 'acts': []})
        if r['kind'] == 'LTRIAL':
            t.update(lure=r.get('lure'), pulsing=r.get('is_pulsing'))
        elif r['kind'] == 'LACT':
            t['acts'].append((r['_f'], r.get('arg'), r.get('call'), r.get('active_after')))
        elif r['kind'] == 'DIVERT':
            t['diverts'].append((r['_f'], r.get('why')))
        elif r['kind'] == 'LP':
            t['polls'].append((r['_f'], [x for x in (r.get('vt') or '').split(';') if x], r.get('act')))
    if not trials:
        return []
    out = ['', '== t09a LURE Is_Pulsing (CU8F_T04_T06_RE.md sec. 3.1) ==',
           'After each periodic divert (victims ordered to attack the decoy D) a pulsing lure takes them back to L '
           'within one pulse (15 frames) while the ability is active (Expiration 6 s = 180 frames).']
    results = {}
    for t in trials.values():
        fact = next((a[0] for a in t['acts'] if a[3] == '1'), t['acts'][-1][0] if t['acts'] else None)
        rows, back_n, tot_n = [], 0, 0
        divs = [d for d in t['diverts'] if d[1] == 'periodic']
        for i, (fd, _w) in enumerate(divs):
            end = divs[i + 1][0] if i + 1 < len(divs) else fd + 60
            win = [p for p in t['polls'] if p[0] is not None and fd < p[0] <= end]
            nv = max((len(p[1]) for p in win), default=0)
            back = []
            for v in range(nv):
                f_back = next((p[0] for p in win if len(p[1]) > v and p[1][v].startswith('L')), None)
                back.append(None if f_back is None else int(f_back - fd))
            rel = None if fact is None else int(fd - fact)
            active = any(p[2] == '1' for p in win)
            if rel is not None and rel <= 180:
                back_n += sum(1 for b in back if b is not None)
                tot_n += nv
            rows.append('   divert at %s (+%s after activation, ability active %s): back to L after %s' % (
                int(fd), rel, 'yes' if active else 'no', back))
        res = ('PULSES' if tot_n and back_n * 2 >= tot_n else 'NO_REPEAT' if tot_n else 'NO_DATA')
        results.setdefault(t.get('pulsing'), []).append(res)
        out.append('trial %s %s (Is_Pulsing %s): activation %s -> %s (%d of %d diverted victims re-lured inside 6 s)' % (
            t['k'], t.get('lure'), t.get('pulsing'), t['acts'], res, back_n, tot_n))
        out += rows
    no = results.get('No (mod)', [])
    yes = results.get('Yes (shipped)', [])
    if no and all(r == 'PULSES' for r in no) and yes and all(r == 'PULSES' for r in yes):
        verdict = 'CONFIRMED: Is_Pulsing No still pulses; the port keeps Is_Pulsing parsed and unread.'
    elif no and all(r == 'NO_REPEAT' for r in no) and yes and any(r == 'PULSES' for r in yes):
        verdict = ('DIFFERS: Is_Pulsing No stops the repeat pulses; the port must read Is_Pulsing (one pulse at '
                   'activation when No).')
    else:
        verdict = 'INCONCLUSIVE: control (Yes) did not re-lure, or no activation (see LACT and APIERR).'
    out.append('VERDICT t09a: ' + verdict)
    return out


# ---- t09b cancel ---------------------------------------------------------------------------------------------------

def sec_cancel(recs):
    trials = collections.OrderedDict()
    for r in recs:
        if r.get('p') != 'cancel' or 'k' not in r:
            continue
        t = trials.setdefault((r.get('sid'), r['k']), {'k': r['k'], 'polls': [], 'phases': {}, 'acts': []})
        if r['kind'] == 'CPH':
            t['phases'][r.get('ph')] = r['_f']
        elif r['kind'] == 'CP':
            t['polls'].append((r['_f'], r.get('ph'), num(r.get('d')), r.get('wtb'), r.get('tb'), r.get('h')))
        elif r['kind'] == 'CACT':
            t['acts'].append((r['_f'], r.get('ph'), r.get('call'), r.get('ready_before'), r.get('active_after')))
        elif r['kind'] == 'CRESTORED':
            t['restored'] = (int(num(r.get('after')) or 0), r.get('active'), r.get('wtb'))
        elif r['kind'] == 'CKILL':
            t['kill'] = r.get('res')
        elif r['kind'] == 'CUSER':
            t['user'] = '%s %s %s dTH %s' % (r.get('type'), r.get('id'), r.get('how'), r.get('dTH'))
    if not trials:
        return []
    out = ['', '== t09b Cancel_Special_Ability restore (CU8F_T01_T02_T07_RE.md sec. 1) ==',
           'TRACTOR_BEAM flags tb = ready,active; wtb = victim Is_Under_Effects_Of_Ability; pull = T-W distance '
           'change per second (negative = pulled in).']
    for t in trials.values():
        out.append('trial %s: Home_One %s; kill call %s' % (t['k'], t.get('user'), t.get('kill')))
        for ph in ('base', 'cancel', 'kill', 'restore'):
            ps = [p for p in t['polls'] if p[1] == ph and p[2] is not None and p[0] is not None]
            if len(ps) >= 2 and ps[-1][0] > ps[0][0]:
                pull = (ps[-1][2] - ps[0][2]) / ((ps[-1][0] - ps[0][0]) / FPS)
            else:
                pull = None
            flags = collections.Counter('%s/%s' % (p[4], p[3]) for p in ps)
            acts = [a for a in t['acts'] if a[1] == ph]
            out.append('   %-7s from %s: pull %s /s, tb/wtb %s, activations %s' % (
                ph, fmt_f(t['phases'].get(ph)), 'n/a' if pull is None else '%.1f' % pull, dict(flags),
                [(int(a[0]), a[2], a[3], a[4]) for a in acts][:8]))
        fr = t['phases'].get('restore')
        first = None
        if fr is not None:
            base = next((p[4] for p in t['polls'] if p[1] == 'cancel'), None)
            first = next((p for p in t['polls'] if p[0] is not None and p[0] >= fr and p[4] != base), None)
        out.append('   restore (frame %s = first poll with Home One dead): CRESTORED %s; first tb change after removal: %s' % (
            fmt_f(fr),
            t.get('restored'), None if first is None else '%s at +%d' % (first[4], first[0] - fr)))
    out.append('VERDICT t09b: read by hand: the spec expects the cancel phase to block the activation (tb ready 0 '
               'or call refused, no pull) and the restore at +0..+3 frames (one poll). No block in the cancel '
               'phase = the Combat_Automatic apply did not fire on this target (its trigger is [L]).')
    return out


# ---- t08 maps -------------------------------------------------------------------------------------------------------

def sec_maps(recs):
    gal = [r for r in recs if r.get('p') == 'gal']
    tac = [r for r in recs if r.get('p') == 'maps']
    if not gal and not tac:
        return []
    out = ['', '== t08 missing fallback maps (CU8F_T03_T05_RE.md sec. 1.7) ==']
    sessions = sorted({r.get('session') for r in gal if r.get('session')})
    out.append('galactic sessions: %s' % sessions)
    for r in gal:
        if r['kind'] in ('START', 'CHG', 'DONE'):
            out.append('   %s sid %s t=%s %s' % (r['kind'], r.get('sid'), r.get('t'), ' '.join(
                '%s=%s' % (k, r.get(k)) for k in ('rebel_objs', 'empire_objs', 'Abregado_Rae', 'Jabiim', 'Thyferra')
                if r.get(k) is not None)))
    gaps = []
    hb = [r for r in gal if r['kind'] in ('HB', 'CHG') and num(r.get('t')) is not None]
    for a, b in zip(hb, hb[1:]):
        if a.get('sid') == b.get('sid') and num(b['t']) - num(a['t']) > 6:
            gaps.append('%s..%s' % (a['t'], b['t']))
    out.append('galactic pauses (> 6 s between records: a battle or a dialog): %s' % gaps)
    battles = collections.OrderedDict()
    for r in tac:
        battles.setdefault(r.get('sid'), []).append(r)
    if not battles:
        out.append('NO tactical record: no tactical battle came up in this session (refused, auto-resolved or '
                   'crashed before the first free-store service).')
    for sid, rs in battles.items():
        st = next((r for r in rs if r['kind'] == 'MSTART'), None)
        out.append('tactical battle sid %s: mode %s (AI %s)' % (sid, st and st.get('mode'), st and st.get('fac')))
        for r in rs:
            if r['kind'] == 'MOBJ':
                out.append('   %s %-10s count %s box %s types %s' % (r.get('tag'), r.get('player'), r.get('count'),
                                                                     r.get('box'), (r.get('types') or '')[:200]))
    out.append('VERDICT t08: read with the README table (map size and props of each battle; Alderaan copy = '
               'about 300..3540 x -165..3430 with Alderaan props).')
    return out


def report(path, out=None):
    recs = load(path)
    lines = ['cu8g probe report for %s (%d records)' % (path, len(recs))]
    lines += sec_status(recs)
    for sec in (sec_stick, sec_lure, sec_cancel, sec_maps):
        lines += sec(recs)
    text = '\n'.join(lines) + '\n'
    if out:
        with open(out, 'w') as f:
            f.write(text)
    return text, recs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--out')
    a = ap.parse_args()
    text, recs = report(a.log, a.out or os.path.join(os.path.dirname(os.path.abspath(a.log)), 'report.txt'))
    print(text)
    return 0 if recs else 10


if __name__ == '__main__':
    sys.exit(main())
