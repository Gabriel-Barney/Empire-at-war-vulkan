#!/usr/bin/env python3
"""s78m_parse.py - turns the s78m t06 bribe probe records (records.log) into report.txt / report.json.

  s78m_parse.py RECORDS.log [--out report.txt] [--json report.json]

Records are "S8M|<KIND>|sid=..|n=..|f=<mode frame>|k=v|...|END" lines (s78m_bribe.lua). Frames are mode frames
(30 per second) read at the free-store service, so every frame below is good to one poll (3 frames).

Port values compared (TACTICAL_COMBAT.md sec. 5.4.1; engine file:line in PORT_REF):
  cost      Tactical_Bribe_Cost 25 (Stormtrooper_Team) x Bribe_Modifier_On_Uncorrupted_Planets 1.0 = 25 credits
  team      the container and every live member of the target's team change owner; the other team does not
  timing    payment and every owner change on one frame (no animation wait), at the pending order's next
            targeting service
  recharge  Recharge_Seconds 50 = 1500 frames from the payment
  container a team container as the target is refused (the port accepts combat units only)
  range     Activation_Max_Range 100, XY centre distance from Tyber to the target object
  refused   too few credits: no payment, no owner change, no recharge, the order is used up (no bribe once the
            money is back)
"""
import argparse
import json
import math
import os
import sys

HUMAN = 'UNDERWORLD'
AI = 'EMPIRE'
PORT_COST = 25.0
PORT_RECHARGE = 1500
POLL = 3
RANGE = 100.0          # Tyber_Tactical_Bribe Activation_Max_Range
INCOME_WINDOW = 15     # frames each side of the payment poll used to estimate the income on that poll

PORT_REF = {
    'cost': 'engine/tactical/abilities/src/ability_classes.cpp:288 (bribe_cost), engine/apps/sim/src/battle_catalog.cpp:718-721 '
            '(Tactical_Bribe_Cost, Bribe_Modifier_*), engine/apps/sim/src/tactical_battle_oi_abilities.cpp:204-213 '
            '(pay_credits, battle_planet_corrupted)',
    'accept': 'engine/tactical/abilities/src/ability_classes.cpp:306 (bribe_accepts: combat unit only, container via '
              'x->container)',
    'apply': 'engine/tactical/abilities/src/ability_classes.cpp:644-673 (apply: refused -> BribeRefused, no recharge; '
             'else pay, start_recharge kTacticalBribe, owner change at once)',
    'owner': 'engine/tactical/abilities/src/ability_classes.cpp:1305-1316 + ability_behaviors.cpp:53 (change_owner: '
             'object + container), engine/apps/sim/src/tactical_battle_oi_abilities.cpp:494 + tactical_battle.cpp:4821 '
             '(team_change_owner: every other live member)',
    'recharge': 'engine/tactical/abilities/src/ability_system.cpp:351 (start_recharge, Recharge_Seconds x fps)',
    'range': 'engine/tactical/abilities/src/ability_system.cpp:805 (in_range: XY centre distance <= '
             'Activation_Max_Range), ability_system.cpp:1237 (try_pending at each targeting service)',
}


def parse_line(line):
    line = line.strip()
    if not line.startswith('S8M|') or not line.endswith('|END'):
        return None
    parts = line[4:-4].split('|')
    d = {}
    for p in parts[1:]:
        k, _, v = p.partition('=')
        d[k] = v
    d['kind'] = parts[0]          # last: a field never replaces the record kind
    try:
        d['n'] = int(d.get('n', '0'))
    except ValueError:
        d['n'] = 0
    f = d.get('f', '?')
    d['f'] = int(f) if f.lstrip('-').isdigit() else None
    return d


def load(path):
    recs = []
    with open(path, encoding='ascii', errors='replace') as fh:
        for line in fh:
            r = parse_line(line)
            if r:
                recs.append(r)
    return recs


def xy(s):
    try:
        a, b = s.split(',')
        return float(a), float(b)
    except (ValueError, AttributeError):
        return None


def dist(a, b):
    if a is None or b is None:
        return None
    return math.hypot(a[0] - b[0], a[1] - b[1])


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def fmt(v, f='%.1f'):
    return '?' if v is None else f % v


def team_a_alive(cnt):
    """'A:c=EMPIRE,m=EMPIRE8+;B:...' -> True when team A's container or a member is not dead."""
    if not cnt:
        return None
    for part in cnt.split(';'):
        if part.startswith('A:'):
            return any(not kv.split('=', 1)[-1].startswith('dead') for kv in part[2:].split(','))
    return None


def analyse_trial(k, rs):
    """One trial's numbers from its records (all of one script sid)."""
    by = {}
    for r in rs:
        by.setdefault(r['kind'], []).append(r)
    first = lambda kind: (by.get(kind) or [None])[0]
    t = {'k': k}
    tr = first('TRIAL')
    t['name'] = tr.get('name') if tr else '?'
    t['target_kind'] = tr.get('target') if tr else '?'
    t['squad'] = tr.get('squad') if tr else '?'
    t['spawn_ret'] = tr.get('ret') if tr else '?'
    teams = {}
    for r in by.get('TEAM', []):
        teams[r['role']] = {'cid': r.get('cid'), 'ctype': r.get('ctype'), 'n': int(r.get('n', '0') or 0),
                            'members': [m for m in r.get('members', '').split(';') if m], 'mtype': r.get('mtype')}
    t['teams'] = teams
    pre = first('PRE')
    acts = by.get('ACT', [])
    t['act'] = [{'f': a['f'], 'kind': a.get('ak'), 'ret': a.get('ret'), 'tid': a.get('tid'), 'ttype': a.get('ttype'),
                 'cr': num(a.get('cr')), 'rdy_before': a.get('rdy_before'), 'rdy_after': a.get('rdy_after')} for a in acts]
    f_act = acts[0]['f'] if acts else (pre['f'] if pre else None)
    t['f_act'] = f_act
    t['pre_cr'] = num(pre.get('cr')) if pre else None
    t['pre_ty'] = xy(pre.get('ty')) if pre else None
    t['autofire'] = pre.get('autofire') if pre else None
    money = [{'f': m['f'], 'why': m.get('why'), 'amount': num(m.get('amount')), 'call': m.get('call'),
              'before': num(m.get('before')), 'after': num(m.get('after'))} for m in by.get('MONEY', [])]
    t['money'] = money
    f_restore = next((m['f'] for m in money if m['why'] == 'restore'), None)
    t['f_restore'] = f_restore
    # owner changes
    owns = [{'f': o['f'], 'id': o.get('id'), 'team': o.get('team'), 'kind': o.get('ok'), 'from': o.get('from'),
             'to': o.get('to')} for o in by.get('OWN', [])]
    t['own'] = owns
    conv = [o for o in owns if o['to'] == HUMAN]
    conv_a = [o for o in conv if o['team'] == 'A']
    t['conv_first'] = min((o['f'] for o in conv_a), default=None)
    t['conv_last'] = max((o['f'] for o in conv_a), default=None)
    a = teams.get('A', {'members': [], 'cid': None, 'n': 0})
    conv_ids = {o['id'] for o in conv_a}
    dead_before = {o['id'] for o in owns if o['team'] == 'A' and o['to'] == 'dead' and
                   (t['conv_first'] is None or o['f'] <= t['conv_first'])}
    t['a_members'] = len(a['members'])
    t['a_members_converted'] = len([m for m in a['members'] if m in conv_ids])
    t['a_members_dead_before'] = len([m for m in a['members'] if m in dead_before])
    t['a_container_converted'] = (a.get('cid') in conv_ids) if a.get('cid') and not str(a.get('cid')).startswith('solo') else None
    t['control_converted'] = sorted({o['team'] for o in conv if o['team'] != 'A'})
    t['control_teams'] = sorted(r for r in teams if r != 'A')
    # credits
    crs = [{'f': c['f'], 'from': num(c.get('from')), 'to': num(c.get('to')), 'd': num(c.get('d'))} for c in by.get('CR', [])]
    t['credit_edges'] = crs
    pay = None
    if t['conv_first'] is not None:
        cands = [c for c in crs if c['d'] is not None and c['d'] < 0 and abs(c['f'] - t['conv_first']) <= 2 * POLL]
        if cands:
            pay = min(cands, key=lambda c: abs(c['f'] - t['conv_first']))
    if pay is None and f_act is not None:
        # a payment without an owner change (or before it): the first drop after the order
        cands = [c for c in crs if c['d'] is not None and c['d'] < 0 and c['f'] >= f_act]
        if cands and t['name'] != 'refused_poor':
            pay = cands[0]
    t['pay'] = -pay['d'] if pay else None
    t['pay_f'] = pay['f'] if pay else None
    # income on the payment poll: the CR edge of every other poll within +-INCOME_WINDOW frames (0 when a poll has
    # none); when they all agree, the same income is assumed to have landed on the payment poll too, so
    # price = net drop + income. Disagreeing neighbours: income unknown, price = the net drop.
    t['pay_income'] = None
    t['price'] = t['pay']
    if pay is not None:
        poll_fs = sorted({p['f'] for p in by.get('P', []) if p['f'] is not None} | {c['f'] for c in crs})
        lo = min(c['f'] for c in crs)    # credit edges are recorded from the trial's first poll on
        near = [f for f in poll_fs if f != pay['f'] and abs(f - pay['f']) <= INCOME_WINDOW and f > lo]
        d_at = {c['f']: c['d'] for c in crs if c is not pay}
        incs = [d_at.get(f) or 0.0 for f in near]
        if incs and max(incs) - min(incs) < 0.005:
            t['pay_income'] = incs[0]
            t['price'] = t['pay'] + incs[0]
        t['pay_income_polls'] = len(incs)
    t['act_sync_pay'] = (t['act'][0]['cr'] is not None and t['pre_cr'] is not None and
                         t['act'][0]['cr'] < t['pre_cr'] - 0.001) if t['act'] and t['name'] != 'refused_poor' else None
    t['other_credit_edges'] = [c for c in crs if c is not pay]
    # ready edges
    rdy = [{'f': r['f'], 'from': r.get('from'), 'to': r.get('to')} for r in by.get('RDY', [])]
    t['rdy_edges'] = rdy
    ref = t['pay_f'] if t['pay_f'] is not None else t['conv_first']
    t['rdy_drop_f'] = next((r['f'] for r in rdy if r['from'] == '1' and r['to'] == '0'), None)
    t['ready_again_f'] = next((r['f'] for r in rdy if r['from'] == '0' and r['to'] == '1' and
                               (ref is None or r['f'] >= ref)), None)
    t['recharge'] = (t['ready_again_f'] - ref) if (t['ready_again_f'] is not None and ref is not None) else None
    t['recharge_from_drop'] = (t['ready_again_f'] - t['rdy_drop_f']) if (t['ready_again_f'] is not None and
                                                                          t['rdy_drop_f'] is not None) else None
    # positions around the conversion
    ps = by.get('P', [])
    t['p_count'] = len(ps)
    polls = []
    for p in sorted(ps, key=lambda p: p['f'] if p['f'] is not None else -1):
        ty = xy(p.get('ty'))
        nm2 = num(p.get('nm2'))
        polls.append({'f': p['f'], 'cr': num(p.get('cr')), 'rdy': p.get('rdy'), 'a_alive': team_a_alive(p.get('cnt')),
                      'target': dist(ty, xy(p.get('tgt'))), 'container': dist(ty, xy(p.get('ctr'))),
                      'nearest_member': math.sqrt(nm2) if nm2 is not None else None})
    t['polls'] = polls
    # range: the first poll where each candidate distance is <= RANGE, with the poll before it
    t['range_first'] = {}
    for key in ('target', 'container', 'nearest_member'):
        prev = None
        hit = None
        for p in polls:
            if p['f'] is None or (t['f_act'] is not None and p['f'] < t['f_act']):
                continue
            if p[key] is not None and p[key] <= RANGE:
                hit = {'f': p['f'], 'd': p[key], 'prev_f': prev and prev['f'], 'prev_d': prev and prev[key]}
                break
            prev = p
        t['range_first'][key] = hit
    # affordable window (the refused trial): polls after the order, before the restore, with credits >= the port
    # price, Tyber ready and team A alive. A kept order would bribe in this window.
    fr = f_restore
    win = [p for p in polls if t['f_act'] is not None and p['f'] is not None and p['f'] > t['f_act']
           and (fr is None or p['f'] < fr)
           and p['cr'] is not None and p['cr'] >= PORT_COST and p['rdy'] == '1' and p['a_alive']]
    t['afford'] = {'n': len(win), 'first': win[0]['f'] if win else None, 'last': win[-1]['f'] if win else None,
                   'cr_first': win[0]['cr'] if win else None, 'cr_last': win[-1]['cr'] if win else None,
                   'd_target': win[0]['target'] if win else None}
    t['a_dead_f'] = min((o['f'] for o in owns if o['team'] == 'A' and o['to'] == 'dead' and o['f'] is not None),
                        default=None)
    at = before = None
    if t['conv_first'] is not None:
        before = max((p for p in ps if p['f'] < t['conv_first']), key=lambda p: p['f'], default=None)
        at = next((p for p in ps if p['f'] == t['conv_first']), None)
    for tag, p in (('before', before), ('at', at)):
        if p:
            ty = xy(p.get('ty'))
            t['d_' + tag] = {'f': p['f'], 'target': dist(ty, xy(p.get('tgt'))), 'container': dist(ty, xy(p.get('ctr'))),
                             'nearest_member': math.sqrt(num(p.get('nm2'))) if num(p.get('nm2')) is not None else None}
        else:
            t['d_' + tag] = None
    cv = first('CONV')
    t['ty_at_conv'] = xy(cv.get('tyber')) if cv else None
    t['moved'] = dist(t['pre_ty'], t['ty_at_conv'])
    t['counts_at_conv'] = cv.get('counts') if cv else None
    t['costs'] = [{'type': c.get('type'), 'via': c.get('via'), 'cost': c.get('cost')} for c in by.get('COST', [])]
    te = first('TEND')
    t['end_counts'] = te.get('cnt') if te else None
    t['ended'] = te is not None
    return t


def verdict(ok):
    return 'MATCH' if ok else 'DIFFERS'


def analyse(recs):
    res = {'records': len(recs), 'scripts': [], 'trials': [], 'notes': []}
    sids = []
    for r in recs:
        if r.get('sid') not in sids:
            sids.append(r.get('sid'))
    for sid in sids:
        rs = sorted([r for r in recs if r.get('sid') == sid], key=lambda r: r['n'])
        kinds = {}
        for r in rs:
            kinds[r['kind']] = kinds.get(r['kind'], 0) + 1
        arm = next((r for r in rs if r['kind'] == 'ARM'), None)
        skip = next((r for r in rs if r['kind'] == 'SKIP'), None)
        done = next((r for r in rs if r['kind'] == 'DONE'), None)
        last = rs[-1]
        res['scripts'].append({'sid': sid, 'armed': arm is not None, 'skip': skip and {k: skip.get(k) for k in
                               ('fac', 'mode', 'human', 'human_side')}, 'done': done and done.get('apierr'),
                               'last_kind': last['kind'], 'last_f': last['f'], 'kinds': kinds,
                               'apierr': [r.get('api') + ': ' + r.get('err', '') for r in rs if r['kind'] == 'APIERR'],
                               'err': [r.get('what', r.get('err', '')) for r in rs if r['kind'] == 'ERR'],
                               'steps_tail': [r.get('s') for r in rs if r['kind'] == 'ST'][-5:],
                               'tyber': next(({k: r.get(k) for k in ('id', 'type', 'how', 'owner', 'pos', 'site',
                                                                     'autofire_before', 'autofire_after', 'rdy', 'cr')}
                                              for r in rs if r['kind'] == 'TYBER'), None)})
        if arm is None:
            continue
        ks = []
        for r in rs:
            if r.get('k') and r['kind'] == 'TRIAL' and r['k'] not in ks:
                ks.append(r['k'])
        for k in ks:
            t = analyse_trial(int(k), [r for r in rs if r.get('k') == k])
            t['sid'] = sid
            res['trials'].append(t)
    return res


def lines_trial(t):
    out = []
    out.append('--- trial %d %s (target %s, squad %s) ---' % (t['k'], t['name'], t['target_kind'], t['squad']))
    for role in sorted(t['teams']):
        tm = t['teams'][role]
        out.append('  team %s: container %s (%s), %d x %s' % (role, tm['cid'], tm['ctype'], tm['n'], tm['mtype']))
    for a in t['act']:
        out.append('  order f=%s %s -> %s (%s %s): ret=%s ready %s->%s cr=%s' % (
            a['f'], a['kind'], a['tid'], a['ttype'], '', a['ret'], a['rdy_before'], a['rdy_after'], fmt(a['cr'], '%.2f')))
    for m in t['money']:
        out.append('  Give_Money f=%s %s %s: %s -> %s (%s)' % (m['f'], m['why'], fmt(m['amount'], '%.2f'),
                                                               fmt(m['before'], '%.2f'), fmt(m['after'], '%.2f'), m['call']))
    rel = lambda f: '?' if f is None or t['f_act'] is None else '%+d' % (f - t['f_act'])
    out.append('  owner change of team A: first f=%s (order %s), last f=%s, spread %s frames; members %d/%d converted '
               '(%d died before), container %s; control teams %s converted: %s' % (
                   t['conv_first'], rel(t['conv_first']), t['conv_last'],
                   '?' if t['conv_first'] is None else t['conv_last'] - t['conv_first'], t['a_members_converted'],
                   t['a_members'], t['a_members_dead_before'],
                   {True: 'converted', False: 'NOT converted', None: '?'}[t['a_container_converted']],
                   ','.join(t['control_teams']) or '-', ','.join(t['control_converted']) or 'none'))
    out.append('  payment: %s credits at f=%s (order %s; conversion %s); paid inside the Activate call: %s' % (
        fmt(t['pay'], '%.2f'), t['pay_f'], rel(t['pay_f']),
        '?' if t['pay_f'] is None or t['conv_first'] is None else '%+d' % (t['pay_f'] - t['conv_first']),
        t['act_sync_pay']))
    if t['pay'] is not None:
        out.append('  price: net drop %s + income on that poll %s (from %s neighbouring polls within +-%d frames) = %s' % (
            fmt(t['pay'], '%.2f'), fmt(t['pay_income'], '%+.2f'), t.get('pay_income_polls', 0), INCOME_WINDOW,
            fmt(t['price'], '%.2f')))
    if t['afford']['n'] and t['name'] == 'refused_poor':
        a = t['afford']
        out.append('  affordable window (credits >= %.0f, ready, team A alive, before the restore): f=%s..%s, %d polls, '
                   'credits %s..%s, target %s away; owner change in it: %s' % (
                       PORT_COST, a['first'], a['last'], a['n'], fmt(a['cr_first'], '%.2f'), fmt(a['cr_last'], '%.2f'),
                       fmt(a['d_target']), t['conv_first'] is not None and a['first'] <= t['conv_first'] <= a['last'] + POLL))
    if t['a_dead_f'] is not None and t['conv_first'] is None:
        out.append('  team A died: first death seen f=%s (order %s)' % (t['a_dead_f'], rel(t['a_dead_f'])))
    if t['other_credit_edges']:
        out.append('  other credit changes: ' + ', '.join('f=%s %s' % (c['f'], fmt(c['d'], '%+.2f'))
                                                         for c in t['other_credit_edges'][:12]))
    out.append('  ready: drop f=%s (order %s), ready again f=%s; recharge %s frames from the payment / conversion '
               '(%s from the drop)' % (t['rdy_drop_f'], rel(t['rdy_drop_f']), t['ready_again_f'], t['recharge'],
                                       t['recharge_from_drop']))
    for tag in ('before', 'at'):
        d = t['d_' + tag]
        if d:
            out.append('  Tyber distance %s the conversion (f=%s): to target %s, container %s, nearest member %s' % (
                tag, d['f'], fmt(d['target']), fmt(d['container']), fmt(d['nearest_member'])))
    out.append('  Tyber moved %s from the order to the conversion' % fmt(t['moved']))
    for c in t['costs']:
        out.append('  Get_Bribe_Cost(tyber) %s (%s) = %s' % (c['type'], c['via'], c['cost']))
    out.append('  at the end: %s' % t['end_counts'])
    return out


def verdicts(res):
    out = []
    tr = {t['name']: t for t in res['trials']}
    t1 = tr.get('near_member')
    if t1:
        out.append('Q1 cost: retail price %s (net drop %s, income %s on the payment poll), port %.2f (25 x 1.0) -> %s'
                   '   [%s]' % (
                       fmt(t1['price'], '%.2f'), fmt(t1['pay'], '%.2f'),
                       fmt(t1['pay_income'], '%+.2f') if t1['pay_income'] is not None else 'unknown', PORT_COST,
                       verdict(t1['price'] is not None and abs(t1['price'] - PORT_COST) < 0.01), PORT_REF['cost']))
        t2p = tr.get('far_container')
        if t2p and t2p['price'] is not None:
            out.append('   trial 2 payment: net %s, income %s -> price %s' % (
                fmt(t2p['pay'], '%.2f'), fmt(t2p['pay_income'], '%+.2f'), fmt(t2p['price'], '%.2f')))
        costs = ', '.join('%s=%s' % (c['type'], c['cost']) for c in t1['costs'])
        if costs:
            out.append('   Get_Bribe_Cost(tyber): %s (18.75 would mean the skirmish planet counts as corrupted: '
                       'Bribe_Modifier_On_Corrupted_Planets 0.75)' % costs)
        alive = t1['a_members'] - t1['a_members_dead_before']
        whole = (t1['a_members_converted'] == alive and alive > 0 and t1['a_container_converted'] is not False
                 and not t1['control_converted'])
        out.append('Q2 whole team: %d/%d live members + container %s, control %s -> %s   [%s]' % (
            t1['a_members_converted'], alive, t1['a_container_converted'], t1['control_converted'] or 'untouched',
            verdict(whole), PORT_REF['owner']))
        same = t1['conv_first'] is not None and t1['conv_last'] == t1['conv_first'] and (
            t1['pay_f'] is None or t1['pay_f'] == t1['conv_first'])
        delay = None if t1['conv_first'] is None or t1['f_act'] is None else t1['conv_first'] - t1['f_act']
        out.append('Q3 timing: order -> conversion %s frames, payment on f=%s, owner changes over %s frames; port: '
                   'payment and all owner changes on one frame, no animation wait -> %s%s   [%s]' % (
                       delay, t1['pay_f'], '?' if t1['conv_first'] is None else t1['conv_last'] - t1['conv_first'],
                       verdict(same), '' if delay is None or delay <= 30 else ' (order -> conversion over 1 s: an '
                       'animation or a walk; see the P records)', PORT_REF['apply']))
        rc = t1['recharge']
        out.append('Q4 recharge: %s frames (+-%d), port %d -> %s   [%s]' % (
            rc, 2 * POLL, PORT_RECHARGE, verdict(rc is not None and abs(rc - PORT_RECHARGE) <= 2 * POLL),
            PORT_REF['recharge']))
    t2 = tr.get('far_container')
    if t2:
        first = t2['act'][0] if t2['act'] else None
        fb = next((a for a in t2['act'] if a['kind'] == 'member_fallback'), None)
        cont_ok = first is not None and t2['conv_first'] is not None and (fb is None or t2['conv_first'] < fb['f'])
        out.append('Q5 container as target: Activate ret=%s, %s; port refuses a container -> %s   [%s]' % (
            first and first['ret'], 'converted before any member retry' if cont_ok else
            ('converted only after the member retry' if fb and t2['conv_first'] is not None else 'no conversion'),
            verdict(not cont_ok), PORT_REF['accept']))
        db, da = t2['d_before'], t2['d_at']
        if da or db:
            # Tyber approaches, so the distance at the conversion frame lies in [at the conversion poll, at the poll
            # before). The port converts at the first targeting service with centre distance <= 100, so it needs
            # d_at <= 100.
            d_at_t = da and da['target']
            port_ok = d_at_t is not None and d_at_t <= RANGE
            out.append('Q6 range: Tyber walked %s; centre distance to the target %s at the poll before the conversion '
                       '(f=%s), %s at the conversion poll (f=%s): retail threshold on the centre distance in [%s, %s) '
                       '(= %d + [%s, %s)); port: XY centre distance to the target <= %d -> %s   [%s]' % (
                           fmt(t2['moved']), fmt(db and db['target']), db and db['f'], fmt(d_at_t), da and da['f'],
                           fmt(d_at_t), fmt(db and db['target']), RANGE,
                           fmt(None if d_at_t is None else d_at_t - RANGE),
                           fmt(None if not db or db['target'] is None else db['target'] - RANGE), RANGE,
                           verdict(port_ok), PORT_REF['range']))
            for key in ('target', 'container', 'nearest_member'):
                h = t2['range_first'].get(key)
                if h:
                    out.append('   %s <= %d first at poll f=%s (%s; poll before f=%s: %s), conversion seen f=%s' % (
                        key, RANGE, h['f'], fmt(h['d']), h['prev_f'], fmt(h['prev_d']), t2['conv_first']))
                else:
                    out.append('   %s <= %d: never before the end of the trial polls' % (key, RANGE))
        else:
            out.append('Q6 range: no conversion, no distance')
    t3 = tr.get('refused_poor')
    if t3:
        fr = t3['f_restore']
        poor = next((m for m in t3['money'] if m['why'] == 'poor'), None)
        before = lambda f: f is not None and (fr is None or f < fr)
        conv_poor = before(t3['conv_first'])
        paid_poor = any(c['d'] is not None and c['d'] < 0 and before(c['f']) for c in t3['credit_edges'])
        rdy_poor = before(t3['rdy_drop_f'])
        conv_after = t3['conv_first'] is not None and not conv_poor
        cr_restore = next((m['before'] for m in t3['money'] if m['why'] == 'restore'), None)
        out.append('Q7 refused (credits %s after Give_Money, %s just before the restore): converted while poor %s, '
                   'paid %s, recharge started %s, converted after the money came back %s; port: no, no, no, no -> %s'
                   '   [%s]' % (fmt(poor and poor['after'], '%.2f'), fmt(cr_restore, '%.2f'), conv_poor, paid_poor,
                                rdy_poor, conv_after,
                                verdict(not conv_poor and not paid_poor and not rdy_poor and not conv_after),
                                PORT_REF['apply']))
        if poor and poor['after'] is not None and poor['after'] >= PORT_COST:
            out.append('   NOTE: Give_Money did not bring the credits under 25 (negative amounts refused?): Q7 is void')
        if cr_restore is not None and cr_restore >= PORT_COST and not conv_poor:
            out.append('   NOTE: income lifted the credits to %.2f before the restore without a bribe' % cr_restore)
        a = t3['afford']
        if a['n']:
            out.append('   order used up: %d polls f=%s..%s with credits %s..%s >= %.0f, Tyber ready, team A alive %s '
                       'away, and no bribe -> the refused order is not retried (port: used up) -> %s' % (
                           a['n'], a['first'], a['last'], fmt(a['cr_first'], '%.2f'), fmt(a['cr_last'], '%.2f'),
                           PORT_COST, fmt(a['d_target']), verdict(not conv_poor)))
        else:
            out.append('   order used up: unproven (no poll with credits >= %.0f, Tyber ready and team A alive before '
                       'the restore)' % PORT_COST)
        if t3['a_dead_f'] is not None and (fr is None or t3['a_dead_f'] < fr):
            out.append('   NOTE: team A died at f=%s, before the restore (f=%s): "converted after the money came back" '
                       'only covers the affordable window above' % (t3['a_dead_f'], fr))
    t4 = tr.get('manual')
    if t4:
        if t4['conv_first'] is not None:
            out.append('Q8 by hand: price %s, %d/%d members + container %s, control %s, recharge %s' % (
                fmt(t4['price'], '%.2f'), t4['a_members_converted'], t4['a_members'], t4['a_container_converted'],
                t4['control_converted'] or 'untouched', t4['recharge']))
        else:
            out.append('Q8 by hand: no bribe in the window (skipped or failed)')
    return out


def report(records_path, out_txt=None, out_json=None):
    recs = load(records_path) if os.path.exists(records_path) else []
    res = analyse(recs)
    lines = ['s78m t06 Tactical_Bribe probe report (%d records from %s)' % (len(recs), records_path), '']
    for s in res['scripts']:
        lines.append('script sid %s: %s, last record %s at f=%s, DONE=%s; kinds %s' % (
            s['sid'], 'ARMED' if s['armed'] else ('skipped %s' % s['skip'] if s['skip'] else 'loaded only'),
            s['last_kind'], s['last_f'], 'yes' if s['done'] is not None else 'NO (script stopped or game quit early)',
            ' '.join('%s:%d' % kv for kv in sorted(s['kinds'].items()))))
        if s['tyber']:
            lines.append('  Tyber: %s' % ' '.join('%s=%s' % kv for kv in s['tyber'].items()))
        for x in s['apierr']:
            lines.append('  APIERR ' + x)
        for x in s['err']:
            lines.append('  ERR ' + x)
        if s['armed'] and s['done'] is None and s['steps_tail']:
            lines.append('  last step records: ' + ', '.join(s['steps_tail']))
    lines.append('')
    for t in res['trials']:
        lines += lines_trial(t)
        lines.append('')
    v = verdicts(res)
    res['verdicts'] = v
    lines.append('=== compared with the port (frames +-%d: one free-store poll each side) ===' % POLL)
    lines += v if v else ['no armed trial: nothing to compare (see the script lines above)']
    text = '\n'.join(lines) + '\n'
    if out_txt:
        with open(out_txt, 'w') as f:
            f.write(text)
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(res, f, indent=1, default=str)
    return text, res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('records')
    ap.add_argument('--out')
    ap.add_argument('--json')
    a = ap.parse_args()
    text, res = report(a.records, a.out, a.json)
    print(text)
    return 0 if res['records'] else 10


if __name__ == '__main__':
    sys.exit(main())
