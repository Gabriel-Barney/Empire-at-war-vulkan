#!/usr/bin/env python3
r"""slice_report.py - the a2m t07 report: per Advance_Tech_Rebel plan run, its target planet, and the slice it caused
(sliced type, price, Rebel credits at the slice, frame).

  slice_report.py --run-dir DIR [--trace-dir Q2RUN] [--fac REBEL]

Inputs in DIR: lua_records.txt (q2lua_reader.py: "src sid seq kind text", the P records of the RebelAdvanceTechPlan
hook, make_slice_mod.py) and slice_watch.jsonl (slice_watch.py: credits / tech / unlocked list of the player in memory).
Output: DIR\slice_report.txt (also printed) and DIR\slice_plans.tsv (one row per plan run).

How a slice is recognised: an entry added to the player's unlocked list (+0x1b0) in galactic mode. Its price is the
credit drop the watcher saw at the same frame (the executor charges Add_Credits(-price) just before it appends the
type: REMAINING_GAPS 1.3 step 7.5), "credits at the slice" the value just before that drop. A slice is joined to the plan
run whose landing (PLANDPRE .. PLAND, + 30 frames) contains its frame. A run with no unlock in that window did not
slice: its PLAND count (1 = the droids are still there: nothing affordable / no candidate / not landed; 0 = despawned:
a slice attempt that failed, docs/re/REMAINING_GAPS.md 1.3 step 7.6) and the credits / tech around the landing say why.
"""
import argparse
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slice_watch  # noqa: E402

PLAN = 'Advance_Tech_Rebel'
END_KINDS = ('PDONE', 'PEXIT', 'PFAIL')


def kv(text):
    d = {}
    for tok in text.split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            d.setdefault(k, v)
    return d


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def read_records(path):
    """[(sid, seq, kind, dict)] of the plan's P records, in (sid, seq) order."""
    out = []
    if not os.path.isfile(path):
        return out
    for line in open(path, encoding='ascii', errors='replace'):
        parts = line.rstrip('\n').split(' ', 4)
        if len(parts) < 5 or parts[0] != 'P':
            continue
        d = kv(parts[4])
        if d.get('plan') != PLAN and parts[3] not in ('LOAD', 'ERR'):
            continue
        try:
            seq = int(parts[2])
        except ValueError:
            continue
        out.append((parts[1], seq, parts[3], d, parts[4]))
    out.sort(key=lambda r: (r[0], r[1]))
    return out


def plan_runs(recs):
    runs = []
    cur = {}
    for sid, seq, kind, d, text in recs:
        if kind == 'PSTART':
            r = {'sid': sid, 'n': d.get('n'), 'player': d.get('player'), 'start': num(d.get('f')), 'prev': d.get('prev'),
                 'cr0': d.get('cr'), 'tech0': d.get('tech'), 'target': None, 'owner': None, 'blocks': {}, 'end': None,
                 'end_f': None, 'land_pre': None, 'land': None, 'cands': []}
            runs.append(r)
            cur[sid] = r
            continue
        r = cur.get(sid)
        if r is None or d.get('n') != r['n']:
            continue
        if kind == 'PCAND':
            r['cands'].append((d.get('planet', '?'), d.get('owner', '?'), d.get('slice', '?')))
        elif kind == 'PTARGET':
            r['target'], r['owner'] = d.get('target'), d.get('owner')
        elif kind == 'PLANDPRE':
            r['land_pre'] = d
            r['target'] = r['target'] or d.get('target')
        elif kind in ('PPROD', 'PMOVE', 'PLAND', 'PLANDX'):
            r['blocks'][kind] = d
            if kind == 'PLAND':
                r['land'] = d
        elif kind in END_KINDS and r['end'] is None:
            r['end'], r['end_f'] = kind, num(d.get('f'))
    return runs


def tgt_key(t):
    """'obj:THE_MAW' / 'THE_MAW' / 'The_Maw' -> 'THE_MAW' (the hook prefixes 'obj:' when the name came from the object)."""
    return (t or '').split(':')[-1].strip().upper()


def watch_events(path, fac):
    """(credit changes [(mf, prev, new)], unlocks [{mf, type, cr_before, price, tech}], tech changes [(mf, prev, new)],
    list decode ok?, unlocked-count baselines [(mf, n)]) for the faction, galactic mode only.
    An unlock is an entry added to the unlocked list: by name when the watcher could read the entries, else (older
    watcher, wrong decode) from the count in the raw bytes (slice_watch.decode_raw), named '#<index>'. Its price is the
    credit drop on the same frame minus the per-frame production drain seen on the change before it."""
    ev = slice_watch.load(path)
    crs, unlocks, techs, bases = [], [], [], []
    ok_list = None
    bad = 0
    cur_cr = None
    cur_tech = None
    last_count = None
    pending = []

    def drop_price(i):
        """(price, credits before) of the credit change crs[i] (a drop), drain-corrected."""
        mf, prev, new = crs[i]
        drain = 0.0
        if i > 0:
            pmf, pprev, pnew = crs[i - 1]
            if 0 <= mf - pmf <= 2 and -50.0 < pnew - pprev < 0:
                drain = pprev - pnew
        return round(prev - new - drain, 3), round(prev - drain, 3)

    for e in ev:
        if e.get('fac') != fac:
            continue
        mf = e.get('mf')
        if e.get('ev') == 'player':
            cur_cr = cur_tech = last_count = None
            continue
        if e.get('mt') != 0 or mf is None:
            continue
        if e['ev'] == 'cr':
            if e.get('prev') is not None:
                crs.append((mf, e['prev'], e['cr']))
                if e['cr'] < e['prev']:
                    for u in pending:
                        if mf - u['mf'] <= 2 and u['price'] is None:
                            u['price'], u['cr_before'] = drop_price(len(crs) - 1)
                pending = [u for u in pending if u['price'] is None and mf - u['mf'] <= 2]
            cur_cr = e['cr']
        elif e['ev'] == 'tech':
            if not (0 <= (e.get('tech') or 0) <= 16 and 0 <= (e.get('max') or 0) <= 16):
                continue   # stale player address (old watcher): garbage
            if e.get('prev') is not None and 0 <= e['prev'] <= 16:
                techs.append((mf, e['prev'], e['tech']))
            cur_tech = e['tech']
        elif e['ev'] == 'list' and e.get('which') == 'unlocked':
            layout, _data, n = slice_watch.decode_raw(bytes.fromhex(e.get('raw') or ''))
            if layout is None and not e.get('ok'):
                bad += 1   # stale player address after a mode change (old watcher), or an unknown layout
                continue
            ok_list = True
            added = list(e.get('added') or [])
            if not added and layout is not None:
                if last_count is None:
                    bases.append((mf, n))
                elif n > last_count:
                    added = ['#%d' % (i + 1) for i in range(last_count, n)]
            if layout is not None:
                last_count = n
            for t in added:
                u = {'mf': mf, 'type': t, 'cr_before': cur_cr, 'price': None, 'tech': cur_tech}
                if crs and mf - crs[-1][0] <= 1 and crs[-1][2] < crs[-1][1]:
                    u['price'], u['cr_before'] = drop_price(len(crs) - 1)
                else:
                    pending.append(u)
                unlocks.append(u)
    return crs, unlocks, techs, (ok_list, bad), bases


def trace_planets(trace_dir, watch_path):
    """{PLANET_NAME: (owner faction at the first keyframe, [(mode frame, new owner)])} from trace.q2t, or {}."""
    path = os.path.join(trace_dir or '', 'trace.q2t')
    if not trace_dir or not os.path.isfile(path):
        return {}
    sys.path.insert(0, os.path.dirname(HERE))
    import q2trace  # noqa: E402
    ids = {}
    for e in slice_watch.load(watch_path):
        if e.get('ev') == 'player' and e.get('mt') == 0 and e.get('id') is not None:
            ids[e['id']] = e['fac']
    _meta, _hdr, frames, types, _end = q2trace.parse(path)
    objs, planets, out = {}, set(), {}
    for fr in frames:
        if fr.key:
            objs = {}
        for p in fr.lines:
            if p[0] == 'O':
                objs[int(p[1])] = p[1:]
            elif p[0] == 'X':
                objs.pop(int(p[1]), None)
            elif p[0] == 'G':
                planets.add(int(p[1]))
        for pid in planets:
            o = objs.get(pid)
            if not o:
                continue
            name = (types.get(int(o[2])) or (0, 0, '?'))[2].upper()
            own = ids.get(int(o[1]), o[1])
            if name not in out:
                out[name] = (own, [])
            elif (out[name][1][-1][1] if out[name][1] else out[name][0]) != own:
                out[name][1].append((fr.mframe, own))
    return out


def fmt(v):
    if v is None:
        return '-'
    if isinstance(v, float):
        return '%.2f' % v
    return str(v)


def lua_slice(r):
    """(consumed?, price, tech before, tech after) from the plan records alone. LandUnits() (PGTaskForce) calls
    taskforce.Land_Units() before its BlockOnCommand, so PLANDPRE is written after the landing fired the slicer. Count 1
    at PMOVE and 0 at PLANDPRE = the team despawned at the landing (the slice executed); the price is the credit
    difference (same frame, so no production drain in between)."""
    mv, lp = r['blocks'].get('PMOVE') or {}, r['land_pre'] or {}
    if not mv or not lp:
        return None, None, None, None
    gone = mv.get('count') == '1' and lp.get('count') == '0'
    c0, c1 = num(mv.get('cr')), num(lp.get('cr'))
    price = round(c0 - c1, 3) if (gone and c0 is not None and c1 is not None) else None
    return gone, price, mv.get('tech'), lp.get('tech')


def report(run_dir, trace_dir=None, fac='REBEL'):
    recs = read_records(os.path.join(run_dir, 'lua_records.txt'))
    runs = [r for r in plan_runs(recs) if (r['player'] or '').upper() == fac]
    wpath = os.path.join(run_dir, 'slice_watch.jsonl')
    crs, unlocks, techs, (ok_list, bad_lists), bases = watch_events(wpath, fac)
    owners = trace_planets(trace_dir, wpath)
    L = ['A2M T07 REBEL SLICE REPORT (%s)' % run_dir]
    if trace_dir:
        L.append('q2 trace run: %s (trace.q2t: planet owners%s)' % (trace_dir, '' if owners else ': NOT READ'))
    loads = sum(1 for r in recs if r[2] == 'LOAD')
    errs = [r[4] for r in recs if r[2] == 'ERR']
    L.append('plan hook: %d script instance(s) loaded, %d plan runs (%s), %d probe errors%s' % (
        loads, len(runs), fac, len(errs), (': ' + '; '.join(errs[:4])) if errs else ''))
    L.append('watcher: %d %s credit changes, %d unlocked-list additions (list decode %s), %d tech changes%s' % (
        len(crs), fac, len(unlocks), {None: 'never seen', True: 'ok', False: 'NOT a pointer vector: see raw'}[ok_list] +
        (', %d undecodable reads skipped' % bad_lists if bad_lists else ''), len(techs), ('; unlocked count when the galactic player appeared: %s' %
                     ', '.join('%d (f%d)' % (n, f) for f, n in bases)) if bases else ''))
    L.append('')
    L.append('== 1. Plan runs (frame = mode frame; cr = %s credits from Lua) ==' % fac)
    L.append('  %-4s %6s %-12s %-10s %6s %6s %6s %6s %9s %9s %4s %4s %-6s %6s  %s' % (
        'run', 'start', 'target', 'owner', 'prod', 'move', 'landp', 'land', 'cr@move', 'cr@landp', 'tch', 'cnt',
        'end', 'end_f', 'slice'))
    used = set()
    rows = []
    for i, r in enumerate(runs):
        lp, ld = r['land_pre'] or {}, r['land'] or {}
        mv = r['blocks'].get('PMOVE') or {}
        f0 = num(lp.get('f'))
        f1 = num(ld.get('f'))
        hit = None
        if f0 is not None:
            hi = (f1 if f1 is not None else f0 + 300) + 30
            for j, u in enumerate(unlocks):
                if j not in used and f0 - 1 <= u['mf'] <= hi:
                    hit = u
                    used.add(j)
                    break
        r['slice'] = hit
        gone, lprice, t0, t1 = lua_slice(r)
        r['consumed'] = gone
        key = tgt_key(r['target'])
        own = owners.get(key)
        owner = r['owner'] if r['owner'] not in (None, '?') else (own[0] if own else '?')
        if hit:
            sl = '%s price=%s cr_before=%s f=%d' % (hit['type'], fmt(hit['price']), fmt(hit['cr_before']), hit['mf'])
        elif gone:
            sl = 'team despawned at the landing, no unlock seen'
        else:
            sl = 'none' if f0 is not None else 'no landing'
        if gone:
            sl += ' (lua: price=%s tech %s>%s)' % (fmt(lprice), t0, t1)
        b = r['blocks']
        L.append('  %-4s %6s %-12s %-10s %6s %6s %6s %6s %9s %9s %4s %4s %-6s %6s  %s' % (
            '%d' % (i + 1), fmt(int(r['start']) if r['start'] is not None else None), key[:12] or '-',
            str(owner)[:10], (b.get('PPROD') or {}).get('f', '-'), mv.get('f', '-'),
            lp.get('f', '-'), ld.get('f', '-'), fmt(num(mv.get('cr'))), fmt(num(lp.get('cr'))),
            '%s>%s' % (mv.get('tech', '-'), lp.get('tech', '-')) if lp else '-', lp.get('count', '-'), r['end'] or '-',
            fmt(int(r['end_f'])) if r['end_f'] is not None else '-', sl))
        rows.append([str(i + 1), r['sid'], str(r['n']), fmt(r['start']), r['target'] or '', str(owner),
                     lp.get('f', ''), ld.get('f', ''), lp.get('cr', ''), ld.get('cr', ''), lp.get('tech', ''),
                     ld.get('tech', ''), ld.get('count', ''), r['end'] or '', fmt(r['end_f']),
                     hit['type'] if hit else '', fmt(hit['price']) if hit else '', fmt(hit['cr_before']) if hit else '',
                     str(hit['mf']) if hit else '', mv.get('cr', ''), lp.get('count', ''),
                     '1' if gone else '0', fmt(lprice) if gone else ''])
    L.append('  (prod / move = frames after Produce_Force / Move_To; landp = after Land_Units() was issued: LandUnits()')
    L.append('   calls it before its BlockOnCommand, so the slice has already run there; land = after Land_Units finished;')
    L.append('   tch = tech level at move>landp; cnt = TechForce.Get_Force_Count() at landp (0 = the team despawned: the')
    L.append('   slice executed); owner = planet owner from trace.q2t when the hook could not read it)')
    L.append('')
    L.append('== 2. Every unlocked-list addition of %s in galactic mode (watcher) ==' % fac)
    for j, u in enumerate(unlocks):
        owner = next((i + 1 for i, r in enumerate(runs) if r.get('slice') is u), None)
        L.append('  f=%-6d %-36s price=%-8s cr_before=%-9s tech after=%s  %s' % (
            u['mf'], u['type'], fmt(u['price']), fmt(u['cr_before']), fmt(u['tech']),
            'plan run %d' % owner if owner else '(no plan run landing at that frame: story / other unlock)'))
    if not unlocks:
        L.append('  none')
    if any(u['type'].startswith('#') for u in unlocks):
        L.append('  (#n = entry n of the list, from the raw count: this watcher run could not read the type names; '
                 'price = credit drop minus the per-frame production drain)')
    L.append('  tech changes: %s' % (', '.join('f%d %s>%s' % t for t in techs) or 'none'))
    L.append('')
    L.append('== 3. Per target planet ==')
    per = {}
    for r in runs:
        t = tgt_key(r['target']) or '(no target)'
        e = per.setdefault(t, [0, 0, 0, 0])
        e[0] += 1
        e[1] += 1 if r['land'] else 0
        e[2] += 1 if r.get('slice') else 0
        e[3] += 1 if r.get('consumed') else 0
    for t in sorted(per):
        own = owners.get(t)
        L.append('  %-20s runs %d, landed %d, sliced %d (team despawned %d)%s' % (
            t, per[t][0], per[t][1], per[t][2], per[t][3],
            ('; owner %s%s' % (own[0], ''.join(', f%d -> %s' % c for c in own[1]))) if own else ''))
    maw = per.get('THE_MAW')
    L.append('  The_Maw: %s' % ('no plan run targeted it' if not maw else
                                '%d run(s), %d landed, %d sliced -> %s' % (
                                    maw[0], maw[1], maw[2],
                                    'never slices' if maw[2] == 0 and maw[3] == 0 else 'SLICED')))
    L.append('')
    L.append('== 4. FindTarget candidates (a2m r2, t14.4: PCAND = Is_Good_Slice_Target per planet before the pick) ==')
    seen_runs = [r for r in runs if r['cands']]
    if not seen_runs:
        L.append('  none recorded (a hook built before a2m r2)')
    picks = {}
    for i, r in enumerate(runs):
        if not r['cands']:
            continue
        L.append('  run %-3d target %-12s | %s' % (i + 1, tgt_key(r['target'])[:12] or '-', ' '.join(
            '%s:%s=%s' % (tgt_key(pl), (ow or '?')[:3], sc) for pl, ow, sc in r['cands'])))
        picks[tgt_key(r['target'])] = picks.get(tgt_key(r['target']), 0) + 1
    if seen_runs:
        enemy = {}
        for r in seen_runs:
            for pl, ow, sc in r['cands']:
                v = num(sc)
                if (ow or '').upper() not in (fac.upper(), 'NEUTRAL', 'PIRATES', '?') and v is not None and v > 0:
                    enemy[tgt_key(pl)] = enemy.get(tgt_key(pl), 0) + 1
        L.append('  enemy planets scoring > 0 (picks / runs): %s' % (', '.join(
            '%s %d/%d' % (k, picks.get(k, 0), n) for k, n in sorted(enemy.items())) or 'none'))
        never = sorted(k for k in enemy if picks.get(k, 0) == 0)
        L.append('  scoring > 0 but never picked: %s (each non-best candidate has 0.9/(n-1) per run, '
                 'A2M_T08_T10_T14_RE t14.3)' % (', '.join(never) or 'none'))
    with open(os.path.join(run_dir, 'slice_plans.tsv'), 'w', newline='\n') as f:
        f.write('\t'.join(['run', 'sid', 'n', 'start', 'target', 'owner', 'landpre_f', 'land_f', 'cr_landpre',
                           'cr_land', 'tech_landpre', 'tech_land', 'count_land', 'end', 'end_f', 'sliced_type', 'price',
                           'cr_before_slice', 'slice_f', 'cr_move', 'count_landpre', 'despawned', 'price_lua']) + '\n')
        for row in rows:
            f.write('\t'.join(row) + '\n')
    text = '\n'.join(L) + '\n'
    with open(os.path.join(run_dir, 'slice_report.txt'), 'w', newline='\n') as f:
        f.write(text)
    return {'runs': runs, 'unlocks': unlocks, 'per_target': per}, text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--run-dir', required=True)
    ap.add_argument('--trace-dir')
    ap.add_argument('--fac', default='REBEL')
    a = ap.parse_args()
    _res, text = report(a.run_dir, a.trace_dir, a.fac)
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
