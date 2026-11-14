#!/usr/bin/env python3
"""a2f_report.py - decodes the a2f t07 budget probe's records and watcher log into report.txt / report.json.

  a2f_report.py <run dir>              reads <run dir>\\records.txt, budget_watch.jsonl, A2FB_TYPES.json
  a2f_report.py --records F [--watch F] [--types F] [--out DIR]

Inputs
  records.txt        lines A2FB1|<channel S story, F free store, P plan>|<sid>|<seq>|<KIND>|<text>|END (the Lua strings
                     a2f_run.py found in the game's memory); texts are space-separated key=value pairs
  budget_watch.jsonl the read-only memory watcher (a2f_watch.py): found / state / flag / chain events
  A2FB_TYPES.json    cost and build time of the probe structure types (written by build_a2f_mod.py)

Outputs: report.txt / report.json, samples.tsv (pools), and since a2h t02 major.tsv (MJ: the MajorItem flush / reclaim
triggers and inputs per Empire sample) and plans.tsv (one row per plan start with its PRET / PEXIT / PFAIL end records);
since a2k t05 saboteur.tsv (section 9: the Underworld saboteur timeline from channels U and F, one row per record).

How the pools are decoded (docs/re/AI_GALACTIC_TOKENS.md BudgetFractionToBuild, AI_GALACTIC_2.md section 3)
  value v = R / D with R = cost of the probe type, D = build time b x category income rate + category available
  credits (all categories: budget +0x68 income per second and the player's credits).  Two probe types with different
  build times give D1 = R1 / v1 and D2 = R2 / v2, so   rate = (D1 - D2) / (b1 - b2),   available = D1 - b1 x rate.
  v >= 1e17 means D <= 0: no pool and no rate: every category before the budget is initialised, a zero-weight
  category after it. The ALL row always has the player's credits in D (AI_GALACTIC_TOKENS).
  The "ALL" row is the check of this reading: its available credits must equal the player's credits.
"""
import argparse
import json
import os
import re
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a2fb_spec  # noqa: E402

RECORD_RE = re.compile(r'^A2FB1\|([SFPDQU])\|([^|]*)\|(\d+)\|([A-Z0-9]+)\|(.*)\|END$')
BIG = 1e17
CAT_KEYS = [c for c, _ in a2fb_spec.CATS]
POOL_CATS = ['OFF', 'DEF', 'INFO', 'INFRA', 'MAJOR']


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def parse_records(path):
    """[dict(ch, sid, seq, kind, f: {k: v})] sorted by (channel, sid, seq)."""
    out = []
    seen = set()
    for line in open(path, encoding='ascii', errors='replace'):
        line = line.strip()
        m = RECORD_RE.match(line)
        if not m or line in seen:
            continue
        seen.add(line)
        kv = {}
        for tok in m.group(5).split(' '):
            if '=' in tok:
                k, v = tok.split('=', 1)
                kv[k] = v
        out.append({'ch': m.group(1), 'sid': m.group(2), 'seq': int(m.group(3)), 'kind': m.group(4), 'f': kv,
                    'text': m.group(5)})
    out.sort(key=lambda r: (r['ch'], r['sid'], r['seq']))
    return out


def build_samples(recs):
    """Merge PL / SM / BF / SH records that share (channel, sid, tag, t, fac) into one sample dict."""
    samples = {}
    order = []
    for r in recs:
        if r['kind'] not in ('PL', 'SM', 'BF', 'SH', 'MJ'):
            continue
        f = r['f']
        key = (r['ch'], r['sid'], f.get('tag'), f.get('t'), f.get('fac'))
        s = samples.get(key)
        if s is None:
            s = {'ch': r['ch'], 'sid': r['sid'], 'tag': f.get('tag'), 't': num(f.get('t')), 'f': num(f.get('f')),
                 'fac': f.get('fac'), 'seq': r['seq'], 'v': {}}
            samples[key] = s
            order.append(key)
        for k, v in f.items():
            if k in ('tag', 't', 'f', 'fac'):
                continue
            s['v'][k] = v
    return [samples[k] for k in order]


def decode_pools(v, fkey, types):
    """{cat: {'state', 'rate', 'avail'}} from the B<k><CAT> fields of one sample."""
    fac = [x for x in a2fb_spec.FACTIONS if x[0] == fkey]
    if not fac:
        return {}
    _, _, _, t1, t2 = fac[0]
    if t1 not in types or t2 not in types:
        return {}
    r1, b1 = types[t1]['cost'], types[t1]['time']
    r2, b2 = types[t2]['cost'], types[t2]['time']
    out = {}
    for c in CAT_KEYS:
        x1, x2 = num(v.get('B1' + c)), num(v.get('B2' + c))
        if x1 is None and x2 is None:
            continue
        if x1 is None or x2 is None:
            out[c] = {'state': 'partial'}
        elif x1 >= BIG and x2 >= BIG:
            # D <= 0: the category has no pool and no rate (a zero-weight category after Init, or any category before it)
            out[c] = {'state': 'empty', 'avail': 0.0, 'rate': 0.0}
        elif x1 >= BIG or x2 >= BIG:
            out[c] = {'state': 'mixed'}
        elif x1 <= 0 or x2 <= 0:
            out[c] = {'state': 'zero', 'note': 'R < 0.0001, or an item of that type is in progress'}
        else:
            d1, d2 = r1 / x1, r2 / x2
            rate = (d1 - d2) / (b1 - b2)
            out[c] = {'state': 'ok', 'rate': rate, 'avail': d1 - b1 * rate, 'D1': d1, 'D2': d2}
    return out


def norm(vec):
    vals = {k: (v if v and v > 0 else 0.0) for k, v in vec.items()}
    s = sum(vals.values())
    if s <= 0:
        return None
    return {k: v / s for k, v in vals.items()}


def shipped_vals(v, fkey):
    """{short key: value} for the SH fields of a sample (the Lua names the fields by the short key of a2fb_spec.SHIPPED)."""
    return {key: num(v[key]) for _n, key in a2fb_spec.SHIPPED.get(fkey, []) if key in v}


def classify(fkey, svals, pools):
    """({template: L1 distance of its normalised weights to the measured pool split}, measured weights) or None."""
    avail = {c: pools[c]['avail'] for c in POOL_CATS if c in pools and pools[c].get('state') in ('ok', 'empty')}
    meas = norm(avail)
    if meas is None:
        return None
    res = {}
    for tname, m in a2fb_spec.TEMPLATE_CANDIDATES.get(fkey, {}).items():
        raw = {}
        for cat in POOL_CATS:
            k = m.get(cat)
            raw[cat] = 0.0 if k is None else svals.get(k)
        if any(x is None for x in raw.values()):
            continue
        n = norm(raw)
        if n is not None:
            res[tname] = sum(abs(meas.get(c, 0.0) - n.get(c, 0.0)) for c in POOL_CATS)
    return res, meas


def fmt(x, w=10):
    if x is None:
        return '-'.rjust(w)
    if abs(x) >= 1e6 or (0 < abs(x) < 1e-3):
        return ('%.3g' % x).rjust(w)
    return ('%.2f' % x).rjust(w)


def sample_line(s, pools):
    v = s['v']
    parts = ['f=%s' % (int(s['f']) if s['f'] is not None else '?'), 'cr=%s' % v.get('cr', '?'),
             'CU=%s' % v.get('CU', '?')]
    for c in CAT_KEYS:
        p = pools.get(c)
        if p is None:
            continue
        if p['state'] == 'ok':
            parts.append('%s[a=%s r=%s]' % (c, fmt(p['avail'], 1), fmt(p['rate'], 1)))
        else:
            parts.append('%s[%s]' % (c, p['state']))
    return ' '.join(parts)


def analyse(recs, types, events):
    samples = build_samples(recs)
    res = {'records': len(recs), 'samples': len(samples), 'channels': {}, 'factions': {}, 'plan_starts': [],
           'canary': [], 'errors': [], 'modeq': None, 'watch': {}}
    for r in recs:
        res['channels'][r['ch']] = res['channels'].get(r['ch'], 0) + 1
        if r['kind'] == 'CANARY':
            res['canary'].append('%s: %s' % (r['ch'], r['text']))
        elif r['kind'] == 'ERR':
            res['errors'].append('%s: %s' % (r['ch'], r['text']))
        elif r['kind'] == 'PSTART':
            res['plan_starts'].append({'ch': r['ch'], **r['f']})
        elif r['kind'] == 'LOAD' and 'modeq' in r['f']:
            res['modeq'] = r['f']['modeq']
    by_fac = {}
    for s in samples:
        if s['fac'] in ('EMPIRE', 'REBEL', 'UNDERWORLD') and ('CU' in s['v'] or 'B1ALL' in s['v']):
            by_fac.setdefault(s['fac'], []).append(s)
    for fkey, lst in by_fac.items():
        lst.sort(key=lambda s: (s['t'] if s['t'] is not None else -1, s['ch'], s['seq']))
        rows = []
        for s in lst:
            pools = decode_pools(s['v'], fkey, types)
            row = {'ch': s['ch'], 'tag': s['tag'], 't': s['t'], 'f': s['f'], 'cr': num(s['v'].get('cr')),
                   'CU': num(s['v'].get('CU')), 'NET': num(s['v'].get('NET')), 'pools': pools}
            rows.append(row)
        res['factions'][fkey] = {'samples': rows}
    # shipped / trigger values over time (SH records, per faction)
    sh = {}
    for s in samples:
        if s['fac'] in a2fb_spec.SHIPPED and any(key in s['v'] for _n, key in a2fb_spec.SHIPPED[s['fac']]):
            sh.setdefault(s['fac'], []).append(s)
    for fkey, lst in sh.items():
        lst.sort(key=lambda s: (s['t'] if s['t'] is not None else -1, s['ch'], s['seq']))
        res['factions'].setdefault(fkey, {'samples': []})['shipped'] = [
            {'ch': s['ch'], 't': s['t'], 'f': s['f'], 'vals': shipped_vals(s['v'], fkey)} for s in lst]
    # first pre-init / post-init per faction
    for fkey, d in res['factions'].items():
        rows = d['samples']
        # before Init every category has no pool and no rate (D <= 0, 1e18); the ALL row still has the credits
        pre = [r for r in rows if r['pools'].get('OFF', {}).get('state') == 'empty' and
               r['pools'].get('INFRA', {}).get('state') == 'empty']
        post = [r for r in rows if r['pools'].get('OFF', {}).get('state') == 'ok' or
                r['pools'].get('INFRA', {}).get('state') == 'ok']
        d['first_preinit_f'] = pre[0]['f'] if pre else None
        d['last_preinit_f'] = pre[-1]['f'] if pre else None
        d['first_postinit_f'] = post[0]['f'] if post else None
        d['modeq_working'] = bool(rows) and any(r['pools'] for r in rows)
        # template classification at the first sample that has every pool category (a full sample)
        d['template_at_first_post'] = None
        full = [r for r in rows if r['pools'].get('ALL', {}).get('state') == 'ok' and
                all(r['pools'].get(c, {}).get('state') in ('ok', 'empty') for c in POOL_CATS) and
                any(r['pools'].get(c, {}).get('state') == 'ok' for c in POOL_CATS)]
        p0 = full[0] if full else (post[0] if post else None)
        if p0 is not None and d.get('shipped'):
            near = min(d['shipped'], key=lambda x: abs((x['f'] or 0) - (p0['f'] or 0)))
            c = classify(fkey, near['vals'], p0['pools'])
            if c:
                dist, meas = c
                d['template_at_first_post'] = {'f_pools': p0['f'], 'f_shipped': near['f'], 'l1': dist,
                                               'measured_weights': meas, 'credits': p0['cr'],
                                               'complete': bool(full)}
    # watcher summary
    fl = {}
    for e in events:
        fac = e.get('fac')
        if fac is None:
            continue
        w = fl.setdefault(fac, {'found': None, 'states': 0, 'flags': [], 'init_changes': [], 'chain': [],
                                'first_state': None, 'last_state': None, 'income_changes': []})
        if e['ev'] == 'found':
            w['found'] = e
        elif e['ev'] == 'chain':
            w['chain'].append(e.get('why'))
        elif e['ev'] == 'flag':
            w['flags'].append({'mf': e.get('mf'), 'gf': e.get('gf'), 'from': e.get('from'), 'to': e.get('to'),
                               'wall': e.get('wall')})
        elif e['ev'] == 'state':
            w['states'] += 1
            if w['first_state'] is None:
                w['first_state'] = e
            else:
                p = w['last_state']
                if p.get('init') != e.get('init'):
                    w['init_changes'].append({'mf': e.get('mf'), 'from': p.get('init'), 'to': e.get('init')})
                if (p.get('income_per_s'), p.get('T')) != (e.get('income_per_s'), e.get('T')):
                    w['income_changes'].append({'mf': e.get('mf'), 'income_per_s': e.get('income_per_s'),
                                                'T': e.get('T'), 'refund': e.get('refund')})
            w['last_state'] = e
    res['watch'] = fl
    res['plans'] = plan_runs(recs)
    res['major'] = major_series(samples, types)
    res['longdone'] = [r['f'].get('f') for r in recs if r['kind'] == 'LONGDONE']
    res['min30'] = [r['f'].get('f') for r in recs if r['kind'] == 'MIN30']
    res['perception'] = perception_series(recs)
    res['production'] = production_events(recs)
    res['saboteur'] = saboteur_data(recs)
    res['last_frame'] = max((num(r['f'].get('f')) or 0 for r in recs), default=None)
    return res


# ---- a2j t02: D4 perception (channel D) and D3 production events (channel Q) --------------------------------------------

def perception_series(recs):
    """{'FAC>planet': [{'f', 'own', 'v': {key: str}, 'sid'}]} from the DT / DS records (merged per sample frame)."""
    out = {}
    idx = {}
    for r in recs:
        if r['ch'] != 'D' or r['kind'] not in ('DT', 'DS'):
            continue
        f = r['f']
        key = '%s>%s' % (f.get('fac'), f.get('tgt'))
        k2 = (key, r['sid'], f.get('f'))
        row = idx.get(k2)
        if row is None:
            row = {'f': num(f.get('f')), 'own': f.get('own'), 'sid': r['sid'], 'v': {}}
            idx[k2] = row
            out.setdefault(key, []).append(row)
        for k, v in f.items():
            if k in a2fb_spec.PERC_KEYS:
                row['v'][k] = v
    for lst in out.values():
        lst.sort(key=lambda x: x['f'] if x['f'] is not None else -1)
    return out


def read_port_perception(path):
    """The port reference table (a2j_port.py table): {'FAC>planet': {frame: {'own', 'v'}}}, plus its comment lines."""
    out, notes = {}, []
    if not path or not os.path.exists(path):
        return out, notes
    head = None
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('#'):
            notes.append(line[1:].strip())
            continue
        cells = line.split('\t')
        if head is None:
            head = cells
            continue
        d = dict(zip(head, cells))
        key = '%s>%s' % (d.get('fac'), d.get('tgt'))
        out.setdefault(key, {})[int(d['frame'])] = {'own': d.get('own'), 'v': {k: d.get(k, '') for k in a2fb_spec.PERC_KEYS}}
    return out, notes


def production_events(recs):
    """[{'kind': QB / QC / QE, 'f', 'fac', 'pl', 'type', 'cost'}] in record order (one GameScoring instance)."""
    out = []
    for r in recs:
        if r['ch'] == 'Q' and r['kind'] in ('QB', 'QC', 'QE'):
            f = r['f']
            out.append({'kind': r['kind'], 'f': num(f.get('f')), 'fac': f.get('fac'), 'pl': f.get('pl'),
                        'type': f.get('type'), 'cost': f.get('cost'), 'cr': f.get('cr', '-'), 'seq': r['seq'],
                        'sid': r['sid']})
    out.sort(key=lambda e: (e['f'] if e['f'] is not None else -1, e['sid'], e['seq']))
    return out


def queue_heads(events, qtypes, fac, frames):
    """Rebuild each (planet, queue) of faction fac from the events: QB appends, QE removes the first entry of that type,
    QC the last one. Returns ({(planet, queue): {frame: head text}}, {(planet, queue): max depth}). The head is the first
    entry; 'n' more entries are queued behind it."""
    queues, depth, heads = {}, {}, {}
    ev = [e for e in events if e['fac'] == fac]
    i = 0
    for fr in frames:
        while i < len(ev) and ev[i]['f'] is not None and ev[i]['f'] <= fr:
            e = ev[i]
            i += 1
            q = (e['pl'], qtypes.get((e['type'] or '').upper(), '?'))
            lst = queues.setdefault(q, [])
            if e['kind'] == 'QB':
                lst.append((e['type'], e['f']))
                depth[q] = max(depth.get(q, 0), len(lst))
            else:
                hit = [j for j, x in enumerate(lst) if x[0] == e['type']]
                if hit:
                    lst.pop(hit[0] if e['kind'] == 'QE' else hit[-1])
        for q, lst in queues.items():
            heads.setdefault(q, {})[fr] = ('%s@%d%s' % (lst[0][0], int(lst[0][1]), (' +%d' % (len(lst) - 1)) if len(lst) > 1 else '')
                                           if lst else '-')
    return heads, depth


PERC_SHOW = ['SCP', 'IGI', 'MK', 'CIP', 'ONF', 'ACT', 'TR', 'GPV', 'IGN']


def render_perception(res, port_ref, port_notes):
    L = ['', '== 6. D4 perception (a2j t02, channel D; A2I_T03_TRAJECTORY.md section 5, A2J_T02_PROBE_PLAN.md) ==']
    L.append('   keys: SCP Should_Conquer_Pirate_Planet, IGI Is_Good_Invasion_Target, MK Variable_Target.Markup,')
    L.append('         CIP ConnectsIsolatedPlanetsByForce, ONF Opens_New_Front, ACT AnyCurrentThreats, TR TradeRoutes,')
    L.append('         GPV GenericPlanetValue, IGN Is_AI_And_Should_Be_Ignored (all keys in perception.tsv).')
    L.append('   nil = no value (before the AI Init: a warning; later: a failed evaluation, which stops that pair\'s')
    L.append('   script: its samples end there), skip = no human player.')
    if port_notes:
        L.append('   port reference: ' + ' / '.join(port_notes))
    if not res['perception']:
        L.append('   no D records (old kit, or the AI plot scripts did not run: see the PerceptU / PerceptE canaries)')
    last = res.get('last_frame')
    for key, rows in res['perception'].items():
        L.append('-- %s: %d samples, frames %s .. %s%s' % (key, len(rows), fi(rows[0]['f']), fi(rows[-1]['f']), (
            '; the channel STOPPED %d frames before the end of the run (a failed evaluation?)' % (last - rows[-1]['f']))
            if last is not None and rows[-1]['f'] is not None and last - rows[-1]['f'] > 400 else ''))
        nil_keys = sorted({k for r in rows for k, v in r['v'].items() if v in ('nil', 'ERR') and (r['f'] or 0) > 30})
        if nil_keys:
            L.append('   keys with nil / ERR after frame 30: %s' % ', '.join(nil_keys))
        pr = port_ref.get(key, {})
        L.append('   %6s %-11s %s' % ('frame', 'owner', ' '.join(
            ('%-17s' % (k + ' r|p' if pr else k)) for k in PERC_SHOW)))
        for r in rows:
            if r['f'] is None:
                continue
            p = pr.get(int(r['f']))
            cells = []
            for k in PERC_SHOW:
                rv = r['v'].get(k, '')
                cells.append(('%-17s' % ('%s|%s' % (rv, p['v'].get(k, '') if p else '-'))) if pr else '%-17s' % rv)
            L.append('   %6d %-11s %s%s' % (int(r['f']), r['own'], ' '.join(cells),
                                           ('  port own=%s' % p['own']) if p and p['own'] != r['own'] else ''))
    return L


def render_production(res, qtypes):
    L = ['', '== 7. D3 production (a2j t02, channel Q: GameScoring Galactic_Production_Begin / Canceled / End events) ==']
    ev = res['production']
    if not ev:
        L.append('   no Q records (old kit, or GameScoring did not load: see the Scoring canary)')
        return L
    facs = sorted({e['fac'] for e in ev if e['fac']})
    L.append('   events: %d (%s)' % (len(ev), ', '.join('%s %d' % (f, sum(1 for e in ev if e['fac'] == f)) for f in facs)))
    qof = lambda e: (qtypes.get((e['type'] or '').upper(), '?') + ' ')[0]
    early = [e for e in ev if e['f'] is not None and e['f'] <= 1500 and e['fac'] not in ('REBEL', 'NEUTRAL', 'nil')]
    L.append('   AI players\' events in frames 0..1500 (QB begin, QC cancel, QE end; queue g / s from A2FB_QUEUE_TYPES.json;')
    L.append('   cr = the owner\'s credits when the event ran):')
    for e in early[:240]:
        L.append('     f=%5d %-10s %s %-12s %s %-40s cost=%-6s cr=%s' % (int(e['f']), e['fac'], e['kind'], e['pl'], qof(e),
                                                                   e['type'], e['cost'], e['cr']))
    if len(early) > 240:
        L.append('     ... %d more in production.tsv' % (len(early) - 240))
    first = {}
    for e in early:
        if e['kind'] == 'QB':
            first.setdefault((e['fac'], e['pl'], qof(e)), e)
    if first:
        L.append('   first Begin per (player, planet, queue) in frames 0..1500 (lane A: per-queue start frames):')
        for k in sorted(first, key=lambda k: (first[k]['f'], k)):
            e = first[k]
            L.append('     f=%5d %-10s %-12s %s %-40s cr=%s' % (int(e['f']), k[0], k[1], k[2], e['type'], e['cr']))
    frames = list(range(0, 1401, 30))
    heads, depth = queue_heads(ev, qtypes, 'EMPIRE', frames)
    if heads:
        cols = sorted(heads)
        L.append('   Empire queue heads every 30 frames (item@begin frame, +n = queued behind; max depth: %s)' % ', '.join(
            '%s/%s %d' % (q[0], q[1], depth.get(q, 0)) for q in cols))
        L.append('     %5s %s' % ('frame', ' '.join('%-30s' % ('%s/%s' % q)[:30] for q in cols)))
        last = None
        for fr in frames:
            row = tuple(heads[q].get(fr, '-') for q in cols)
            if row != last:
                L.append('     %5d %s' % (fr, ' '.join('%-30s' % c[:30] for c in row)))
            last = row
        L.append('     (rows only where a head changed; a QB while the queue is busy (+n) shows that Begin is the enqueue,')
        L.append('      not the build start: then an item starts when the one before it ends)')
    return L


# ---- a2j t04: per-fiscal-cycle P / I against a port run (a2j_port.py; the a2g_trace_patch A2G BUD rows) --------------

PORT_PLAYER = {'EMPIRE': 1, 'UNDERWORLD': 5}
PI_CATS = [('OFF', 'Offensive'), ('INFRA', 'Infrastructure'), ('MAJOR', 'MajorItem')]
PAY0, PERIOD = 1351, 1350


def read_port_bud(path, player):
    """{'tick': [rows], 'frame': [rows]} of A2G BUD lines; row = {'f', 'cr', 'gain', 'cat': {key: (A, R, P, w, I)}}."""
    bud = re.compile(r'^A2G BUD (\w+) f=(-?\d+) p=(\d+) cr=(\S+) gain=(\S+) inc68=(\S+) T=(\S+) sumP=(\S+) \|(.*)$')
    cat = re.compile(r'(\w+) A=(\S+) R=(\S+) P=(\S+) w=(\S+) n=(\d+)(?: I=([^;\s]+))?;')
    names = dict((n, k) for k, n in a2fb_spec.CATS if n)
    out = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = bud.match(line.strip())
        if not m or int(m.group(3)) != player:
            continue
        r = {'f': int(m.group(2)), 'cr': float(m.group(4)), 'gain': float(m.group(5)), 'cat': {}}
        for c in cat.finditer(m.group(9)):
            r['cat'][names.get(c.group(1), c.group(1))] = (float(c.group(2)), float(c.group(3)), float(c.group(4)),
                                                           float(c.group(5)), num(c.group(7)))
        out.setdefault(m.group(1), []).append(r)
    return out


def render_cycles(res, port_trace):
    L = ['', '== 8. Per fiscal cycle P / I, retail | port (a2j t04; P = category pool, I = income rate per second) ==']
    L.append('   port: %s (a2j_port.py run; A2G BUD rows of tools/analysis/a2g_trace_patch.py, ALAMO_A2G_TRACE=3)' % port_trace)
    L.append('   retail P / I = the decoded BudgetFractionToBuild pool / rate (available / rate in section 1). Rows: per category')
    L.append('   the first retail sample after each payment (pay = 1351 + 1350 k) whose decoded P differs from the last one before')
    L.append('   the payment (the leaf caches P up to 5 s, so earlier samples still hold the pre-payment P; A2L_T01_RE_ANSWERS')
    L.append('   Q9.2 / Q9.3), and the last one before the next payment; the port row is the last one at or before that frame.')
    for fac, d in res['factions'].items():
        if fac not in PORT_PLAYER:
            continue
        port = read_port_bud(port_trace, PORT_PLAYER[fac])
        prow = sorted(port.get('frame', []), key=lambda r: r['f'])
        if not prow:
            L.append('-- %s: no port rows for player %d' % (fac, PORT_PLAYER[fac]))
            continue
        rows = sorted([r for r in d['samples'] if r['f'] is not None], key=lambda r: r['f'])
        ticks = port.get('tick', [])
        L.append('-- %s (port player %d): port payments %s' % (fac, PORT_PLAYER[fac], ', '.join(
            'f%d +%.1f' % (t['f'], t['gain']) for t in ticks[:24]) or 'none'))
        L.append('   %2s %5s %4s %6s  %s  %s' % ('k', 'pay', 'at', 'frame', '  '.join(
            '%-27s' % ('%s P r|p  I r|p' % k) for k, _n in PI_CATS), 'credits r|p'))
        last_f = min(rows[-1]['f'] if rows else 0, prow[-1]['f'])
        k = 0
        while PAY0 + k * PERIOD <= last_f:
            pay, nxt = PAY0 + k * PERIOD, PAY0 + (k + 1) * PERIOD
            def has(r, key):
                return r['pools'].get(key, {}).get('state') in ('ok', 'empty')

            def port_at(fr):
                p = None
                for x in prow:
                    if x['f'] <= fr:
                        p = x
                    else:
                        break
                return p

            cyc = [r for r in rows if pay <= r['f'] < nxt]
            for which in ('first', 'last'):
                # per category the first refreshed (last) sample of the cycle that has it (SM samples carry OFF / INFRA
                # only). Refreshed = decoded P differs from the last sample before the payment (Q9.3); none differs -> none.
                picks = {}
                for key, _n in PI_CATS:
                    lst = [r for r in cyc if has(r, key)]
                    if which == 'first':
                        before = [r for r in rows if r['f'] < pay and has(r, key)]
                        old_p = before[-1]['pools'][key].get('avail') if before else None
                        lst = [r for r in lst if old_p is None or r['pools'][key].get('avail') != old_p]
                    picks[key] = (lst[0] if which == 'first' else lst[-1]) if lst else None
                ref = picks['INFRA'] or picks['OFF']
                if ref is None:
                    continue
                cells = []
                for key, _n in PI_CATS:
                    r = picks[key]
                    rp = r['pools'][key] if r else {}
                    pc = None
                    if r:
                        p = port_at(r['f'])
                        pc = p['cat'].get(key) if p else None
                    cells.append('%s|%s %s|%s' % (fmt(rp.get('avail'), 6), fmt(pc[2] if pc else None, 6),
                                                  fmt(rp.get('rate'), 5), fmt(pc[4] if pc else None, 5)))
                p = port_at(ref['f'])
                L.append('   %2d %5d %4s %6d  %s  %s|%s' % (k + 1, pay, which[0], int(ref['f']), '  '.join(
                    '%-27s' % c for c in cells), fmt(ref['cr'], 7), fmt(p['cr'] if p else None, 7)))
            k += 1
    return L


# ---- a2h t02: plan runs (PSTART + PRET / PEXIT / PFAIL) and the MajorItem flush / reclaim series (MJ) ----------------

END_KINDS = ('PRET', 'PEXIT', 'PFAIL')
MAJOR_FLAGS = ['NMS', 'NTF', 'CRM', 'HES', 'AGM', 'HDS']


def plan_runs(recs):
    """{'runs': [run], 'summary': {plan|fac: counts}}. A run is one PSTART with the end records of the same script
    instance (channel, sid) and start number n. Runs without n (a2f / a2g records) are listed without ends."""
    runs = []
    by_key = {}
    for r in recs:
        if r['ch'] != 'P' or r['kind'] != 'PSTART':
            continue
        f = r['f']
        run = {'plan': f.get('plan'), 'fac': f.get('fac'), 'f': num(f.get('f')), 't': num(f.get('t')),
               'sid': r['sid'], 'n': f.get('n'), 'prev': f.get('prev'), 'spend': f.get('spend'),
               'unalloc': f.get('unalloc'), 'cr': f.get('cr'), 'ends': []}
        runs.append(run)
        if run['n'] is not None:
            by_key[(r['sid'], run['n'])] = run
    orphans = []
    for r in recs:
        if r['ch'] != 'P' or r['kind'] not in END_KINDS:
            continue
        f = r['f']
        e = {'kind': r['kind'], 'f': num(f.get('f')), 'age': num(f.get('age')), 'spend': f.get('spend'),
             'unalloc': f.get('unalloc'), 'by': f.get('by'), 'line': f.get('line'), 'args': f.get('args'),
             'text': r['text']}
        run = by_key.get((r['sid'], f.get('n')))
        if run is None:
            orphans.append(dict(e, plan=f.get('plan'), fac=f.get('fac')))
        else:
            run['ends'].append(e)
    # a run whose end record was lost from the scan: the next run of the same instance names its end in prev=
    by_sid = {}
    for run in runs:
        by_sid.setdefault(run['sid'], []).append(run)
    for lst in by_sid.values():
        lst.sort(key=lambda x: int(x['n']) if (x['n'] or '').isdigit() else 0)
        for a, b in zip(lst, lst[1:]):
            a['next_prev'] = b['prev']
    summary = {}
    for run in runs:
        key = '%s|%s' % (run['plan'], run['fac'])
        s = summary.setdefault(key, {'starts': 0, 'PRET': 0, 'PEXIT': 0, 'PFAIL': 0, 'no_end': 0, 'no_end_confirmed': 0,
                                     'with_n': 0, 'first_f': None, 'last_f': None})
        s['starts'] += 1
        if s['first_f'] is None or (run['f'] is not None and run['f'] < s['first_f']):
            s['first_f'] = run['f']
        if s['last_f'] is None or (run['f'] is not None and run['f'] > s['last_f']):
            s['last_f'] = run['f']
        if run['n'] is None:
            continue
        s['with_n'] += 1
        kinds = [e['kind'] for e in run['ends']]
        for k in END_KINDS:
            if k in kinds:
                s[k] += 1
        if not kinds:
            s['no_end'] += 1
            if run.get('next_prev') == 'none':
                s['no_end_confirmed'] += 1     # released by the goal system (thread killed), not a lost record
    return {'runs': runs, 'summary': summary, 'orphan_ends': orphans}


def major_series(samples, types):
    """{faction: [{'ch', 'tag', 't', 'f', 'vals': {key: str}, 'major': decoded MajorItem pool or None}]} from the MJ
    fields of the merged samples (sorted by time)."""
    out = {}
    for s in samples:
        if s['fac'] not in a2fb_spec.MAJOR_FACTIONS:
            continue
        vals = {k: s['v'][k] for k in a2fb_spec.MAJOR_KEYS if k in s['v']}
        if not vals:
            continue
        pools = decode_pools(s['v'], s['fac'], types)
        out.setdefault(s['fac'], []).append({'ch': s['ch'], 'tag': s['tag'], 't': s['t'], 'f': s['f'], 'vals': vals,
                                             'major': pools.get('MAJOR')})
    for lst in out.values():
        lst.sort(key=lambda x: (x['t'] if x['t'] is not None else -1, x['ch']))
    return out


def fi(x):
    """A frame number for printing (int, or '-')."""
    return '-' if x is None else int(x)


def flag_on(v):
    x = num(v)
    return x is not None and x > 0


def render_major(res):
    L = ['', '== 5. MajorItem flush / reclaim (a2h t02; MJ record, A2G_T02_T04_PORT_VS_RETAIL.md section 4) ==']
    L.append('   keys: NMS Needs_More_Space_Force_Globally, NTF Need_To_Flush_MajorItem_Budget (goal Flush_MajorItem_Budget),')
    L.append('         CRM Can_Reclaim_Excess_MajorItem_Budget (goal Reclaim_Excess_...), HES Has_Enough_Space_Force_Globally,')
    L.append('         FRT / ERT friendly / enemy space raw totals, HST / SST Variable_Human / Variable_Self.SpaceTotalUnnormalized,')
    L.append('         AGM ActiveGoals with Parameter_Category MajorItem (AG, without one, is in a2h runs only: it fails in')
    L.append('         retail and stopped the script that evaluated it, a2i t02), HDS HasUnit(Death_Star),')
    L.append('         MNT / MNTU Maintenance, PLC / PLCU PlanetsControlled (skip = no human player, nil = no answer)')
    if res.get('longdone'):
        L.append('   25-minute mark (LONGDONE) reached at frame %s' % res['longdone'][0])
    else:
        L.append('   25-minute mark (LONGDONE) NOT reached: the run was shorter than 1500 s of galactic time')
    if not res['major']:
        L.append('   no MJ records (old kit, or the Empire is human / not sampled)')
    for fac, rows in res['major'].items():
        L.append('-- %s: %d samples with MJ, frames %s .. %s' % (fac, len(rows), fi(rows[0]['f']), fi(rows[-1]['f'])))
        for k in MAJOR_FLAGS:
            on = [r for r in rows if flag_on(r['vals'].get(k))]
            skip = sum(1 for r in rows if r['vals'].get(k) in ('skip', 'nil', 'ERR'))
            L.append('   %-4s > 0 in %d of %d samples%s%s' % (
                k, len(on), len(rows), (' (first at frame %s, last %s)' % (fi(on[0]['f']), fi(on[-1]['f']))) if on else '',
                (', %d skip / nil / ERR' % skip) if skip else ''))
        L.append('   changes of %s (frame: values):' % '/'.join(MAJOR_FLAGS))
        last = None
        shown = 0
        for r in rows:
            sig = tuple(flag_on(r['vals'].get(k)) for k in MAJOR_FLAGS)
            if sig != last or r is rows[0] or r is rows[-1]:
                if shown < 80:
                    v = r['vals']
                    L.append('     f=%s [%s %s] %s' % (
                        int(r['f']) if r['f'] is not None else '?', r['ch'], r['tag'],
                        ' '.join('%s=%s' % (k, v[k]) for k in a2fb_spec.MAJOR_KEYS if k in v)))
                shown += 1
            last = sig
        if shown > 80:
            L.append('     ... %d more changes in major.tsv' % (shown - 80))
    fl = [r for r in res['plans']['runs'] if r['plan'] == 'FlushMajorItemBudget']
    L.append('   FlushMajorItemBudget plan runs: %d' % len(fl))
    for r in fl[:20]:
        L.append('     f=%s fac=%s spend=%s unalloc=%s ends=%s' % (
            fi(r['f']), r['fac'], r['spend'], r['unalloc'],
            ','.join('%s@%s' % (e['kind'], fi(e['f'])) for e in r['ends']) or '-'))
    # MajorItem pool around each Flush run (PS = before, PE = after; full samples of that plan)
    for fac, rows in res['major'].items():
        pe = [r for r in rows if r['tag'] in ('PS', 'PE') and r['major']]
        for r in pe[:20]:
            m = r['major']
            L.append('     %s f=%s MajorItem pool %s' % (r['tag'], fi(r['f']), (
                'a=%.1f r=%.3f' % (m['avail'], m['rate'])) if m.get('state') == 'ok' else m.get('state')))
    return L


def render_plans(res):
    L = ['   per plan (a2h t02: end records; no_end = no PRET / PEXIT / PFAIL for that start: released by the goal system '
         'or the record was lost; confirmed = the next start of the same script says prev=none):']
    summ = res['plans']['summary']
    if not summ:
        return L + ['     none']
    L.append('     %-42s %-11s %6s %6s %6s %6s %7s %9s %8s %8s' % ('plan', 'faction', 'starts', 'PEXIT', 'PFAIL', 'PRET',
                                                               'no_end', 'confirmed', 'first_f', 'last_f'))
    for key in sorted(summ):
        plan, fac = key.split('|', 1)
        s = summ[key]
        L.append('     %-42s %-11s %6d %6d %6d %6d %7d %9d %8s %8s' % (
            plan, fac, s['starts'], s['PEXIT'], s['PFAIL'], s['PRET'], s['no_end'], s['no_end_confirmed'],
            int(s['first_f']) if s['first_f'] is not None else '-', int(s['last_f']) if s['last_f'] is not None else '-'))
    if res['plans']['orphan_ends']:
        L.append('     end records whose PSTART was not found (lost from the scan): %d' % len(res['plans']['orphan_ends']))
    cp = [r for r in res['plans']['runs'] if r['plan'] in ('ConquerPiratePlan', 'LiftBlockadePlan')]
    for r in cp[:30]:
        L.append('     %s f=%s fac=%s ends=%s' % (r['plan'], fi(r['f']), r['fac'], ', '.join(
            '%s@%s age=%s%s' % (e['kind'], fi(e['f']), e['age'], (' line=%s' % e['line']) if e.get('line') else '')
            for e in r['ends']) or '-'))
    return L


def render(res, extra=None):
    L = []
    L.append('a2f t07 budget probe: report')
    L.append('records: %d (channels %s), samples: %d' % (res['records'], res['channels'], res['samples']))
    L.append('')
    L.append('Scripts that ran (canary = _CustomScriptMessage deleted the kit\'s file; the run status says which files '
             'are gone): ' + ('; '.join(res['canary']) if res['canary'] else 'no CANARY record found'))
    if res['errors']:
        L.append('Lua errors: ' + ' | '.join(res['errors'][:8]))
    L.append('')
    L.append('== 1. Pools by category (decoded from BudgetFractionToBuild; a = available credits, r = rate) ==')
    for fkey, d in res['factions'].items():
        rows = d['samples']
        if not rows:
            continue
        L.append('')
        L.append('-- %s: %d samples; mod equations %s' % (fkey, len(rows),
                 'answer' if d['modeq_working'] else 'DID NOT ANSWER (nil / ERR everywhere)'))
        L.append('   first / last sample frame with OFF and INFRA empty (budget not initialised): %s, last: %s, first with real pools: %s' % (
            d['first_preinit_f'], d['last_preinit_f'], d['first_postinit_f']))
        shown = 0
        last_state = None
        for r in rows:
            sig = tuple(sorted((c, p['state']) for c, p in r['pools'].items()))
            if shown < 14 or sig != last_state:
                if shown < 60:
                    L.append('   [%s %s] %s' % (r['ch'], r['tag'], sample_line({'f': r['f'], 'v': {
                        'cr': r['cr'], 'CU': r['CU']}}, r['pools'])))
                    shown += 1
            last_state = sig
        if len(rows) > shown:
            L.append('   ... %d more samples in report.json / samples.tsv' % (len(rows) - shown))
        t = d.get('template_at_first_post')
        if t:
            L.append('   first real pools (f=%s, credits %s): measured weights %s' % (
                t['f_pools'], t['credits'], {k: round(v, 4) for k, v in t['measured_weights'].items()}))
            L.append('   distance (L1) to the template candidates (smaller = closer; 0 = that template\'s weights): %s'
                     % {k: round(v, 4) for k, v in t['l1'].items()})
        sh = d.get('shipped')
        if sh:
            L.append('   shipped equations (first 3 / last):')
            for x in (sh[:3] + sh[-1:]):
                L.append('     f=%s t=%s %s' % (x['f'], x['t'], x['vals']))
    L.append('')
    L.append('== 2. Plan hook (Budget.Get_Spendable_Resources / Get_Unallocated_Resources at plan start) ==')
    if res['plan_starts']:
        for p in res['plan_starts'][:40]:
            L.append('   %s f=%s fac=%s budget=%s spendable=%s unallocated=%s cr=%s' % (
                p.get('plan'), p.get('f'), p.get('fac'), p.get('budget'), p.get('spend'), p.get('unalloc'),
                p.get('cr')))
    else:
        L.append('   none recorded (no hooked plan started, or the plan scripts did not load)')
    L += render_plans(res)
    L.append('')
    L.append('== 3. Budget block watcher (memory, read only): reset flag +0x74 ==')
    if not res['watch']:
        L.append('   no watcher data (budget_watch.jsonl empty): the player -> AI -> context -> budget chain was not '
                 'found; see the chain lines in budget_watch.jsonl')
    for fac, w in res['watch'].items():
        fnd = w['found']
        L.append('-- %s: %s' % (fac, 'found at ' + str(fnd.get('budget')) if fnd else 'budget NOT found'))
        for c in w['chain'][:4]:
            L.append('   chain: ' + str(c))
        L.append('   states logged: %d; init byte changes: %s' % (w['states'], w['init_changes'] or 'none'))
        L.append('   reset flag transitions (mode frame): %s' % (
            ', '.join('%s: %s -> %s' % (x['mf'], x['from'], x['to']) for x in w['flags']) or
            'NONE observed (flag stayed %s)' % (w['last_state'] or {}).get('flag')))
        fs = w['first_state']
        if fs:
            L.append('   first state: mf=%s init=%s income/s=%s T=%s flag=%s refund=%s' % (
                fs.get('mf'), fs.get('init'), fs.get('income_per_s'), fs.get('T'), fs.get('flag'), fs.get('refund')))
        L.append('   income / T / refund changes: %s' % (
            '; '.join('mf %s: inc=%s T=%s refund=%s' % (x['mf'], x['income_per_s'], x['T'], x['refund'])
                      for x in w['income_changes'][:8]) or 'none'))
    L.append('')
    L.append('== 4. Answers ==')
    for fkey, d in res['factions'].items():
        if not d['samples']:
            continue
        L.append('%s: pre-Init (Lua saw 1e18 in every category) up to frame %s; first real pools at frame %s.' % (
            fkey, d['last_preinit_f'], d['first_postinit_f']))
        for fac, w in res['watch'].items():
            if fac == fkey and w['init_changes']:
                L.append('   watcher: context init byte %s' % ', '.join(
                    'mf %s %s -> %s' % (x['mf'], x['from'], x['to']) for x in w['init_changes']))
    L.append('Reset flag: see section 3 (its setter is the transition 0 -> nonzero; compare its frame with the Lua '
             'samples around it: a template switch shows as a change of the SH trigger values, the first service as '
             'the init byte change).')
    L += render_major(res)
    L.append('   30-minute mark (MIN30, a2j t04): %s' % (
        ('reached at frame %s' % res['min30'][0]) if res.get('min30') else 'NOT reached (run shorter than 1800 s)'))
    L += render_perception(res, *(extra or {}).get('port_perception', ({}, [])))
    L += render_production(res, (extra or {}).get('qtypes', {}))
    if (extra or {}).get('port_trace'):
        L += render_cycles(res, extra['port_trace'])
    L += render_saboteur(res)
    return '\n'.join(L) + '\n'


# ---- a2k t05: the Underworld saboteur (A2K_T08_RE_ANSWERS.md section 5, Q5; H1 / H2 of Q4.2) ------------------------

SAB_PLAN = 'AI_Plan_Underworld_DeploySaboteur'
SAB_PROGRESS = ('PSTAGE', 'PPROD', 'PMOVS', 'PMOVE', 'PBLK')
SAB_TIMELINE = {('U', 'US'), ('F', 'FSAB'), ('F', 'FSVC'), ('F', 'FADD')}
SAB_COLS = ['id', 'src', 'f', 'type', 'pl', 'par', 'parid', 'cnt', 'parpl', 'gp', 'gpid', 'gpcnt', 'gppl', 'st', 'tr',
            'tp', 'safe', 'ok']


def up(v):
    return (v or '').upper()


def saboteur_data(recs):
    """{'runs': [DeploySaboteur runs with goal / target / stage / results / ends], 'timeline': {id: [rows]},
    'admissions': [FADD rows], 'desire': {frame: {target: row}}, 'counts': [UN rows], 'marks': {...}}."""
    starts = {}
    runs = []
    for r in recs:
        if r['ch'] == 'P' and r['kind'] == 'PSTART' and r['f'].get('plan') == SAB_PLAN:
            f = r['f']
            run = {'sid': r['sid'], 'n': f.get('n'), 'f': num(f.get('f')), 'prev': f.get('prev'), 'goal': f.get('goal'),
                   'tgt': up(f.get('tgt')), 'tgtc0': f.get('tgtc'), 'cr': f.get('cr'), 'spend': f.get('spend'),
                   'progress': [], 'ends': []}
            runs.append(run)
            starts[(r['sid'], f.get('n'))] = run
    orphans = 0
    for r in recs:
        if r['ch'] != 'P' or r['f'].get('plan') != SAB_PLAN or r['kind'] not in SAB_PROGRESS + END_KINDS:
            continue
        run = starts.get((r['sid'], r['f'].get('n')))
        if run is None:
            orphans += 1
            continue
        f = r['f']
        row = {'kind': r['kind'], 'f': num(f.get('f'))}
        for k in ('stage', 'stagek', 'stages', 'stage2', 'stage2k', 'blk', 'res', 'dt', 'units', 'tgtc', 'by', 'line', 'age',
                  'k'):
            if k in f:
                row[k] = f[k]
        (run['ends'] if r['kind'] in END_KINDS else run['progress']).append(row)
    for run in runs:
        for x in run['progress']:
            if x['kind'] == 'PSTAGE':
                run['stage'] = up(x.get('stage'))
                run['stagek'] = x.get('stagek')          # a2m t12: obj / type / ERR (absent before a2m: stage=ERR)
                run['f_stage'] = x['f']
            elif x['kind'] == 'PPROD':
                run['produce'] = x.get('res')
                run['f_produce'] = x['f']
            elif x['kind'] == 'PMOVS':
                run['stage2'] = up(x.get('stage2'))
                run['units'] = x.get('units')
                run['f_move0'] = x['f']
            elif x['kind'] == 'PMOVE':
                run['move'] = x.get('res')
                run['f_move'] = x['f']
        ends = sorted(run['ends'], key=lambda e: e['f'] if e['f'] is not None else -1)
        run['end'] = '+'.join(e['kind'] for e in ends) or 'none'
        run['f_end'] = ends[-1]['f'] if ends else None
        run['tgtc_end'] = ends[-1].get('tgtc') if ends else None
    timeline = {}
    admissions = []
    desire = {}
    counts = []
    rows_all = []
    for r in recs:
        key = (r['ch'], r['kind'])
        f = r['f']
        if key in SAB_TIMELINE:
            row = {'src': '%s/%s' % key, 'f': num(f.get('f')), 'id': f.get('id', '?')}
            for k in SAB_COLS[3:]:
                if k in f:
                    row[k] = f[k]
            if r['kind'] == 'FADD':
                admissions.append(row)
            rows_all.append(row)
        elif key == ('U', 'UD'):
            fr = num(f.get('f'))
            desire.setdefault(fr, {})[up(f.get('tgt'))] = {'own': f.get('own'), 'cor': f.get('cor'), 'CP': f.get('CP'),
                                                          'SP': f.get('SP')}
        elif key == ('U', 'UN'):
            counts.append({'f': num(f.get('f')), 'n': f.get('n'), 'other': f.get('other')})
    # the saboteur's objects: what channel U / FSAB found by type (company, and a transport the filter matches), the
    # parents U saw that are not fleets (a transport), and store entries of the saboteur type; FSVC / FADD rows of
    # other store units go to the count only
    ids = {x['id'] for x in rows_all if x['src'] in ('U/US', 'F/FSAB')}
    ids |= {x['parid'] for x in rows_all if x['src'] == 'U/US' and x.get('parid') and up(x.get('par')) != 'GALACTIC_FLEET'}
    other_svc = 0
    for x in rows_all:
        if x['id'] in ids or up(x.get('type')) == 'UNDERWORLD_SABOTEUR_TEAM':
            timeline.setdefault(x['id'], []).append(x)
        elif x['src'] == 'F/FSVC':
            other_svc += 1
    for rows in timeline.values():
        rows.sort(key=lambda x: (x['f'] if x['f'] is not None else -1, x['src']))
    marks = {k: [r['f'].get('f') for r in recs if r['kind'] == k] for k in ('UEND', 'SABDONE', 'SABLOAD')}
    return {'runs': runs, 'orphans': orphans, 'timeline': timeline, 'admissions': admissions, 'desire': desire,
            'counts': counts, 'marks': marks, 'other_svc': other_svc}


def desire_before(sab, tgt, frame):
    """The UD row of target tgt at the latest sample frame <= frame (frame, row) or (None, None)."""
    best = None
    for fr in sorted(k for k in sab['desire'] if k is not None):
        if frame is not None and fr > frame:
            break
        if tgt in sab['desire'][fr]:
            best = fr
    return (best, sab['desire'][best][tgt]) if best is not None else (None, None)


def render_saboteur(res):
    sab = res.get('saboteur') or {}
    if not (sab.get('runs') or sab.get('timeline') or sab.get('desire') or sab.get('counts')):
        return []
    L = ['', '== 9. Underworld saboteur (a2k t05; A2K_T08_RE_ANSWERS.md section 5 = Q5, H1 / H2 of Q4.2) ==']
    m = sab['marks']
    L.append('   channel U end (UEND) at frame %s; story SABDONE at %s; extensions loaded (SABLOAD): %d' % (
        ', '.join(m['UEND']) or '-', ', '.join(m['SABDONE']) or '-', len(m['SABLOAD'])))
    L.append('   DeploySaboteur runs (P channel; stage = AssembleForce\'s MainForce.Get_Stage(); produce / move = '
             'BlockOnCommand results of Produce_Force(stage) / Move_To(Target): true / false / nil; desire = the UD '
             'sample at or before the start, CP for Corrupt_Planet, SP for Sabotage_Planet):')
    if not sab['runs']:
        L.append('     none (no AI_Plan_Underworld_DeploySaboteur PSTART)')
    for r in sab['runs']:
        dfr, d = desire_before(sab, r['tgt'], r['f'])
        des = '-'
        if d is not None:
            des = '%s@%s' % (d.get('SP') if r['goal'] == 'Sabotage_Planet' else d.get('CP'), fi(dfr))
        L.append('     f=%5s n=%s %-15s tgt=%-12s desire=%-12s stage=%-12s@%-5s produce=%-5s@%-5s move=%-5s@%-5s '
                 'stage2=%s units=%s end=%s@%s tgtc %s -> %s prev=%s' % (
                     fi(r['f']), r['n'], r['goal'], r['tgt'], des, r.get('stage', '-'), fi(r.get('f_stage')),
                     r.get('produce', '-'), fi(r.get('f_produce')), r.get('move', '-'), fi(r.get('f_move')),
                     r.get('stage2', '-'), r.get('units', '-'), r['end'], fi(r['f_end']), r['tgtc0'], r['tgtc_end'],
                     r['prev']))
    if sab['orphans']:
        L.append('     progress / end records whose PSTART was not found (lost from the scan): %d' % sab['orphans'])
    L.append('   saboteur timeline per object id (U/US every 30 frames, every 300 after 1400 in a --sab-long run: pl, parent fleet type / id / member count / '
             'planet (parpl=nil: in transit); F/FSAB every free-store service (8 s): st = Is_Object_On_Free_Store, '
             'tr = Is_Unit_In_Transit; F/FSVC = On_Unit_Service (unreserved store entry); F/FADD = admission):')
    if not sab['timeline']:
        L.append('     none (no US / FSAB / FSVC / FADD record for a saboteur)')
    for oid in sorted(sab['timeline'], key=lambda k: (len(k), k)):
        rows = sab['timeline'][oid]
        L.append('   -- id %s: %d rows, frames %s .. %s' % (oid, len(rows), fi(rows[0]['f']), fi(rows[-1]['f'])))
        last_u = None
        for x in rows:
            if x['src'] == 'U/US':
                sig = tuple(x.get(k) for k in ('type', 'pl', 'par', 'parid', 'cnt', 'parpl', 'gp', 'gpid', 'gpcnt', 'gppl'))
                if sig == last_u:
                    continue                               # unchanged 30-frame sample: shown once
                last_u = sig
                text = '%s pl=%s par=%s parid=%s cnt=%s parpl=%s' % (x.get('type'), x.get('pl'), x.get('par'),
                                                                     x.get('parid'), x.get('cnt'), x.get('parpl'))
                if 'gp' in x:
                    text += ' gp=%s gpid=%s gpcnt=%s gppl=%s' % (x.get('gp'), x.get('gpid'), x.get('gpcnt'), x.get('gppl'))
            elif x['src'] == 'F/FADD':
                text = 'pl=%s admitted to the free store (On_Unit_Added) %s' % (x.get('pl'), x.get('type'))
            else:
                text = 'pl=%s st=%s tr=%s%s%s' % (x.get('pl'), x.get('st', '-'), x.get('tr', '-'),
                                                 (' safe=%s' % x['safe']) if 'safe' in x else '',
                                                 (' tp=%s %s' % (x.get('tp'), x.get('type'))) if 'tp' in x else '')
            L.append('     f=%5s %-6s %s' % (fi(x['f']), x['src'], text))
    if sab.get('other_svc'):
        L.append('   On_Unit_Service calls for other Underworld store units (F/FSVC, not listed): %d' % sab['other_svc'])
    if sab['counts']:
        ch = []
        prev = None
        for c in sab['counts']:
            if (c['n'], c['other']) != prev:
                ch.append('f%s: %s (other %s)' % (fi(c['f']), c['n'], c['other']))
                prev = (c['n'], c['other'])
        L.append('   saboteurs the Underworld owns (U/UN, changes only): ' + ', '.join(ch))
    if sab['admissions']:
        L.append('   free-store admissions (F/FADD, Underworld, every type): ' + ', '.join(
            '%s@%s %s' % (x.get('type'), fi(x['f']), x.get('pl')) for x in sab['admissions'][:40]))
    if sab['desire']:
        frames = sorted(k for k in sab['desire'] if k is not None)
        tg = sorted({t for fr in frames for t in sab['desire'][fr]})
        shown = [t for t in tg if any(sab['desire'][fr].get(t, {}).get('CP') not in (None, 'own') for fr in frames)]
        L.append('   desire equations per sample frame (U/UD; CP = Should_Corrupt_Planet, SP = Should_Sabotage_Planet, raw '
                 'values: no learning adjust; "own" = Underworld planet, not a target). Accepted = a DeploySaboteur '
                 'PSTART above (the goal system\'s proposals are not visible from Lua):')
        L.append('     %5s ' % 'frame' + ' '.join('%-17s' % t[:17] for t in shown))
        for fr in frames:
            cells = []
            for t in shown:
                d = sab['desire'][fr].get(t)
                cells.append('%-17s' % ('-' if d is None else ('%s/%s%s' % (d.get('CP'), d.get('SP'),
                                                                           ' c' if d.get('cor') == 'true' else ''))))
            L.append('     %5s ' % fi(fr) + ' '.join(cells))
        # a2m t12: the planet owners and corruption at the UD samples (the long run's "who holds Kashyyyk" question)
        own = []
        last = {}
        for fr in frames:
            for t in tg:
                d = sab['desire'][fr].get(t) or {}
                sig = (d.get('own'), d.get('cor'))
                if t not in last:
                    last[t] = sig
                elif sig != last[t]:
                    own.append('f%s %s %s%s -> %s%s' % (fi(fr), t, last[t][0], ' c' if last[t][1] == 'true' else '',
                                                        sig[0], ' c' if sig[1] == 'true' else ''))
                    last[t] = sig
        L.append('   planet owner / corruption changes at the UD samples (frames %s .. %s; c = corrupted): %s' % (
            fi(frames[0]) if frames else '-', fi(frames[-1]) if frames else '-', ', '.join(own) or 'none'))
    return L


def write_saboteur_tsv(res, path):
    sab = res.get('saboteur') or {}
    cols = SAB_COLS
    with open(path, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        rows = [x for lst in (sab.get('timeline') or {}).values() for x in lst]
        rows.sort(key=lambda x: (x['f'] if x['f'] is not None else -1, x['id'], x['src']))
        for x in rows:
            f.write('\t'.join('' if x.get(c) is None else str(x.get(c)) for c in cols) + '\n')


def write_tsv(res, path):
    with open(path, 'w') as f:
        f.write('faction\tchannel\ttag\tt\tframe\tcredits\tCU\t' + '\t'.join(
            '%s_avail\t%s_rate\t%s_state' % (c, c, c) for c in CAT_KEYS) + '\n')
        for fkey, d in res['factions'].items():
            for r in d['samples']:
                cells = []
                for c in CAT_KEYS:
                    p = r['pools'].get(c, {})
                    cells += [str(p.get('avail', '')), str(p.get('rate', '')), p.get('state', '')]
                f.write('\t'.join([fkey, r['ch'], str(r['tag']), str(r['t']), str(r['f']), str(r['cr']),
                                   str(r['CU'])] + cells) + '\n')


def write_major_tsv(res, path):
    """a2h t02: one row per sample with MJ fields (the MajorItem flush / reclaim triggers and inputs)."""
    with open(path, 'w') as f:
        f.write('faction\tchannel\ttag\tt\tframe\t' + '\t'.join(a2fb_spec.MAJOR_KEYS) +
                '\tMAJOR_avail\tMAJOR_rate\tMAJOR_state\n')
        for fac, rows in res['major'].items():
            for r in rows:
                m = r['major'] or {}
                f.write('\t'.join([fac, r['ch'], str(r['tag']), str(r['t']), str(r['f'])] +
                                  [r['vals'].get(k, '') for k in a2fb_spec.MAJOR_KEYS] +
                                  [str(m.get('avail', '')), str(m.get('rate', '')), m.get('state', '')]) + '\n')


def write_plans_tsv(res, path):
    """a2h t02: one row per plan run (PSTART) with its end records."""
    with open(path, 'w') as f:
        f.write('plan\tfaction\tframe\tsid\tn\tprev\tspend\tunalloc\tcredits\tends\n')
        for r in res['plans']['runs']:
            ends = ';'.join('%s@%s age=%s%s' % (e['kind'], e['f'], e['age'],
                                                (' by=%s line=%s' % (e['by'], e['line'])) if e.get('by') else '')
                            for e in r['ends'])
            f.write('\t'.join(str(x) for x in (r['plan'], r['fac'], r['f'], r['sid'], r['n'], r['prev'], r['spend'],
                                                r['unalloc'], r['cr'], ends)) + '\n')


def write_perception_tsv(res, path, port_ref):
    """a2j t02: one row per D4 sample (retail keys), with the port reference value next to each when it has that frame."""
    with open(path, 'w') as f:
        f.write('pair\tframe\towner\t' + '\t'.join(a2fb_spec.PERC_KEYS) + '\tport_owner\t' +
                '\t'.join('port_' + k for k in a2fb_spec.PERC_KEYS) + '\n')
        for key, rows in res['perception'].items():
            pr = port_ref.get(key, {})
            for r in rows:
                p = pr.get(int(r['f'])) if r['f'] is not None else None
                f.write('\t'.join([key, str(fi(r['f'])), str(r['own'])] + [r['v'].get(k, '') for k in a2fb_spec.PERC_KEYS] +
                                  [p['own'] if p else ''] + [p['v'].get(k, '') if p else '' for k in a2fb_spec.PERC_KEYS])
                        + '\n')


def write_production_tsv(res, path, qtypes):
    with open(path, 'w') as f:
        f.write('frame\tevent\tfaction\tplanet\tqueue\ttype\tcost\tcredits\n')
        for e in res['production']:
            f.write('\t'.join(str(x) for x in (fi(e['f']), e['kind'], e['fac'], e['pl'],
                                                qtypes.get((e['type'] or '').upper(), '?'), e['type'], e['cost'],
                                                e['cr'])) + '\n')


DEFAULT_PORT_PERCEPTION = os.path.join(HERE, 'port_reference_a2j.tsv')


def run(records, watch, types_path, out_dir, port_trace=None, port_perception=DEFAULT_PORT_PERCEPTION, qtypes_path=None):
    recs = parse_records(records) if records and os.path.exists(records) else []
    types = json.load(open(types_path)) if types_path and os.path.exists(types_path) else {}
    if qtypes_path is None and types_path:
        qtypes_path = os.path.join(os.path.dirname(os.path.abspath(types_path)), 'A2FB_QUEUE_TYPES.json')
    qtypes = json.load(open(qtypes_path)) if qtypes_path and os.path.exists(qtypes_path) else {}
    events = []
    if watch and os.path.exists(watch):
        import a2f_watch
        events = a2f_watch.load_events(watch)
    res = analyse(recs, types, events)
    os.makedirs(out_dir, exist_ok=True)
    port_ref, port_notes = read_port_perception(port_perception)
    extra = {'port_perception': (port_ref, port_notes), 'qtypes': qtypes,
             'port_trace': port_trace if port_trace and os.path.exists(port_trace) else None}
    text = render(res, extra)
    write_perception_tsv(res, os.path.join(out_dir, 'perception.tsv'), port_ref)
    write_production_tsv(res, os.path.join(out_dir, 'production.tsv'), qtypes)
    with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
        f.write(text)
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(res, f, indent=1, default=str)
    write_tsv(res, os.path.join(out_dir, 'samples.tsv'))
    write_major_tsv(res, os.path.join(out_dir, 'major.tsv'))
    write_plans_tsv(res, os.path.join(out_dir, 'plans.tsv'))
    write_saboteur_tsv(res, os.path.join(out_dir, 'saboteur.tsv'))
    return res, text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run_dir', nargs='?')
    ap.add_argument('--records')
    ap.add_argument('--watch')
    ap.add_argument('--types')
    ap.add_argument('--out')
    ap.add_argument('--port-trace', help='a2j t04: stderr of an a2j_port.py run (A2G BUD rows): section 8, per-cycle P / I')
    ap.add_argument('--port-perception', default=DEFAULT_PORT_PERCEPTION,
                    help='a2j t02: port D4 table (a2j_port.py table; default the kit\'s port_reference_a2j.tsv)')
    a = ap.parse_args()
    if a.run_dir:
        rd = a.run_dir
        rec = a.records or os.path.join(rd, 'records.txt')
        wat = a.watch or os.path.join(rd, 'budget_watch.jsonl')
        typ = a.types or os.path.join(rd, 'A2FB_TYPES.json')
        out = a.out or rd
    else:
        if not a.records:
            ap.error('give a run dir or --records')
        rec, wat, typ = a.records, a.watch, a.types
        out = a.out or os.path.dirname(os.path.abspath(rec))
    res, text = run(rec, wat, typ, out, a.port_trace, a.port_perception)
    sys.stdout.write(text)
    return 0 if res['records'] else 10


if __name__ == '__main__':
    sys.exit(main())
