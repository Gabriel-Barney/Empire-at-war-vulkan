#!/usr/bin/env python3
"""a2f_report.py - decodes the a2f t07 budget probe's records and watcher log into report.txt / report.json.

  a2f_report.py <run dir>              reads <run dir>\\records.txt, budget_watch.jsonl, A2FB_TYPES.json
  a2f_report.py --records F [--watch F] [--types F] [--out DIR]

Inputs
  records.txt        lines A2FB1|<channel S story, F free store, P plan>|<sid>|<seq>|<KIND>|<text>|END (the Lua strings
                     a2f_run.py found in the game's memory); texts are space-separated key=value pairs
  budget_watch.jsonl the read-only memory watcher (a2f_watch.py): found / state / flag / chain events
  A2FB_TYPES.json    cost and build time of the probe structure types (written by build_a2f_mod.py)

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

RECORD_RE = re.compile(r'^A2FB1\|([SFP])\|([^|]*)\|(\d+)\|([A-Z]+)\|(.*)\|END$')
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
        if r['kind'] not in ('PL', 'SM', 'BF', 'SH'):
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
    return res


def render(res):
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
    return '\n'.join(L) + '\n'


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


def run(records, watch, types_path, out_dir):
    recs = parse_records(records) if records and os.path.exists(records) else []
    types = json.load(open(types_path)) if types_path and os.path.exists(types_path) else {}
    events = []
    if watch and os.path.exists(watch):
        import a2f_watch
        events = a2f_watch.load_events(watch)
    res = analyse(recs, types, events)
    os.makedirs(out_dir, exist_ok=True)
    text = render(res)
    with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
        f.write(text)
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(res, f, indent=1, default=str)
    write_tsv(res, os.path.join(out_dir, 'samples.tsv'))
    return res, text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run_dir', nargs='?')
    ap.add_argument('--records')
    ap.add_argument('--watch')
    ap.add_argument('--types')
    ap.add_argument('--out')
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
    res, text = run(rec, wat, typ, out)
    sys.stdout.write(text)
    return 0 if res['records'] else 10


if __name__ == '__main__':
    sys.exit(main())
