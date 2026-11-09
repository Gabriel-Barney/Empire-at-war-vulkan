#!/usr/bin/env python3
"""ccp_parse.py - turns a ccp_reader.py log into the ccpgu t02 report.

  ccp_parse.py parse <reader.log> [--config ccp_config.json] [--out report.txt] [--json report.json]
  ccp_parse.py --selftest        synthetic logs from the port model (and two deviations) through the parser

Per land battle with a human player (the main-menu background battle is listed and skipped):

Part 1, timing (frames = the mode frame, +0x10, read by the memory reader; SKIRMISH_SETUP 4.2):
  C  capture frame: the last owner change of a relevant point (Reinforcement_Point*) before the domination timer arms
  A  arm frame: mode +0x2b0 0 -> 1 (expected A = C), with the timer value seen at A
  T  test frame: +0x2b0 1 -> 0 (the 0x15 test ran)
  D  declaration: victory monitor +0x20 0 -> 1, with the countdown, type (0x15 = 21) and player at D
  E  countdown zero: first frame >= D with monitor +0x24 <= 0 (the end condition of BATTLE_TRANSITIONS 3.1a)
  plus the visible end: the first STALL after D (the frame stopped: end screen) and MODE_END.
Port (engine/apps/sim/src/tactical_battle_victory.cpp): the capture service (s4_tick, tactical_battle.cpp:4494, via
tactical_battle_garrison.cpp:276) arms the timer at 900 (:113-116) before check_victory (:4513) decrements it in the same
tick (:331), so the capture frame shows 899, the test runs at C + 899 and declares at once (:224-227, countdown 210 =
kRecordCountdown :16); the countdown runs from the next tick (:328) and the battle ends at C + 1109 (:358).
Nominal "900 + 210" = 1110; the port's [M] is +-1 for the order of the capture service and the mode tick.

Part 2, start owners: every REINFORCEMENT_POINT* object at the first walk with its owner player and faction (reader
POINT lines), the Lua PT records, and the map file's placements (ccp_config.json 'expected', from ccp_maps.py).
Port: tactical_battle_bt.cpp:696 bt_map_owner (raw 3 = Neutral faction -> the Neutral player), :781.
"""
import argparse
import json
import os
import sys

sys.dont_write_bytecode = True

PORT = {'timer_at_capture': 899, 'arm_minus_capture': 0, 'capture_to_test': 899, 'test_to_decl': 0,
        'decl_countdown': 210, 'decl_to_zero': 210, 'capture_to_zero': 1109}
PORT_REF = {
    'timer_at_capture': 'tactical_battle_victory.cpp:115 (arm 900) + :331 (same-tick decrement)',
    'arm_minus_capture': 'tactical_battle_garrison.cpp:276 (arm before the owner write, same tick)',
    'capture_to_test': 'tactical_battle_victory.cpp:331-335',
    'test_to_decl': 'tactical_battle_victory.cpp:224-227',
    'decl_countdown': 'tactical_battle_victory.cpp:16 kRecordCountdown, :226',
    'decl_to_zero': 'tactical_battle_victory.cpp:328',
    'capture_to_zero': 'tactical_battle_victory.cpp:358 (finish)',
}
RELEVANT = ('REINFORCEMENT_POINT', 'REINFORCEMENT_POINT_PLUS1_CAP', 'REINFORCEMENT_POINT_PLUS2_CAP',
            'REINFORCEMENT_POINT_PLUS3_CAP', 'REINFORCEMENT_POINT_PLUS4_CAP', 'REINFORCEMENT_POINT_PLUS5_CAP',
            'REINFORCEMENT_POINT_PLUS10_CAP')
VICTORY_0x15 = 0x15


def tok(line):
    parts = line.rstrip('\n').split(' ')
    kind = parts[0]
    d = {}
    for p in parts[1:]:
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    return kind, d


def num(v):
    if v is None or v in ('None', '?', ''):
        return None
    try:
        return int(v, 0) if isinstance(v, str) else int(v)
    except ValueError:
        return None


def lua_fields(rec):
    parts = rec.split('|')
    d = {'kind': parts[1] if len(parts) > 1 else '?'}
    for p in parts[2:-1]:
        if '=' in p:
            k, v = p.split('=', 1)
            if k != 'kind':          # the record kind is never overwritten by a field
                d[k] = v
    return d


def owners_of(s):
    out = {}
    for item in (s or '').split(','):
        if ':' in item:
            k, v = item.split(':', 1)
            out[k] = v
    return out


def load(path):
    modes = {}
    order = []
    cur = None
    lua = []
    meta = {}
    for line in open(path, encoding='ascii', errors='replace'):
        kind, d = tok(line)
        if kind in ('CCPREADER', 'GAME', 'EXIT'):
            meta[kind] = d
            continue
        if kind == 'LUA':
            lua.append((num(d.get('n')), lua_fields(d.get('rec', ''))))
            continue
        n = num(d.get('n'))
        if n is None:
            continue
        if n not in modes:
            modes[n] = {'n': n, 'players': {}, 'points': [], 'st': [], 'stalls': [], 'resumes': [], 'gaps': 0,
                        'mode_end': None, 'mon': None, 'cpv': None, 'mode': None}
            order.append(n)
        m = modes[n]
        if kind == 'MODE':
            m['mode'] = d
        elif kind == 'PL':
            m['players'][num(d['id'])] = {'faction': d.get('faction', '?'), 'human': d.get('human') == '1'}
        elif kind == 'POINT':
            m['points'].append(d)
        elif kind == 'ST':
            row = {k: (d[k] if k == 'owners' else num(d.get(k))) for k in
                   ('frame', 'armed', 'timer', 'cpp', 'decl', 'cd', 'dtype', 'dplayer', 'dcd', 'hold', 'owners', 'b1')}
            if row['armed'] is not None and row['b1'] is None:
                row['b1'] = (row['armed'] >> 8) & 0xFF   # logs before 2026-11-08 read +0x2b0 as an i32 (257 / 256)
                row['armed'] &= 0xFF
            m['st'].append(row)
        elif kind == 'STALL':
            m['stalls'].append(num(d.get('frame')))
        elif kind == 'RESUME':
            m['resumes'].append(num(d.get('frame')))
        elif kind == 'GAP':
            m['gaps'] += 1
        elif kind == 'MODE_END':
            m['mode_end'] = d
        elif kind == 'MON':
            m['mon'] = d
        elif kind == 'CPV':
            m['cpv'] = d
        cur = n
    del cur
    for m in modes.values():
        tick_correct(m['st'])
    return meta, [modes[n] for n in order], lua


def tick_correct(st):
    """Retail writes the mode frame counter (+0x10) after the tick's sim state, so a poll inside tick k+1 can see that
    tick's changes still stamped k. Every ST line whose stamp equals the previous line's stamp therefore belongs to the
    next tick (run 20261108-214851: stamps 645, 645, 647 while the timer counts down one per tick). 'stamp' keeps the
    logged value, 'frame' becomes the tick. Two such reads can chain (stamps 1044, 1044, 1045 for ticks 1044, 1045,
    1046): a line stamped with the previous line's tick that is a pure one-step decrement of the running timer or
    countdown is also the next tick. Check: timer + tick (countdown + tick) stays constant while it runs."""
    prev = None
    for row in st:
        stamp = row['frame']
        row['stamp'] = stamp
        if prev is not None and stamp is not None:
            step = ((row['armed'] == 1 and prev['armed'] == 1 and None not in (row['timer'], prev['timer']) and
                     row['timer'] == prev['timer'] - 1) or
                    (row['decl'] == 1 and prev['decl'] == 1 and None not in (row['cd'], prev['cd']) and
                     row['cd'] == prev['cd'] - 1 and row['armed'] == prev['armed']))
            if stamp == prev['stamp'] or (stamp == prev['frame'] and step):
                row['frame'] = prev['frame'] + 1 if stamp == prev['frame'] else stamp + 1
        prev = row


def lua_players(m, lua):
    """(human ids, {id: faction}) from this mode's Lua ARM (human_id) and PL records."""
    human, fac = [], {}
    for n, r in lua:
        if n != m['n']:
            continue
        if r['kind'] == 'ARM' and num(r.get('human_id')) is not None:
            human.append(num(r['human_id']))
        elif r['kind'] == 'PL' and num(r.get('id')) is not None:
            fac[num(r['id'])] = r.get('faction', '?')
            if r.get('human') == 'true':
                human.append(num(r['id']))
    return sorted(set(human)), fac


def analyse_mode(m, lua=()):
    res = {'n': m['n'], 'human': [i for i, p in m['players'].items() if p['human']], 'notes': [], 'human_src': 'reader'}
    if not res['human']:
        # reader logs before 2026-11-08 read the player list only at mode frame 0, where retail still holds the
        # pre-remap list (faction order, no human): take the human and the factions from the Lua records
        hum, fac = lua_players(m, lua)
        if hum:
            res['human'], res['human_src'] = hum, 'lua'
            m['players'] = {i: {'faction': f, 'human': i in hum} for i, f in fac.items()}
            res['notes'].append('human player from the Lua ARM / PL records (the reader player list had none); the '
                                'factions of the reader POINT lines were relabelled from Lua')
    rel_ids = {p['obj'] for p in m['points'] if p.get('relevant') == '1'}
    relabel = res['human_src'] == 'lua'
    res['points'] = [{'obj': p['obj'], 'type': p['type'], 'relevant': p.get('relevant') == '1',
                      'start_owner': num(p.get('owner')),
                      'start_faction': (m['players'].get(num(p.get('owner')), {}).get('faction', '?') if relabel else
                                        p.get('owner_faction', '?')),
                      'x': p.get('x'), 'y': p.get('y')} for p in m['points']]
    res['stamp_repeats'] = sum(1 for s in m['st'] if s.get('stamp') is not None and s['frame'] != s['stamp'])
    st = m['st']
    captures, arms, disarms = [], [], []
    decl = None
    prev = None
    for s in st:
        if prev is not None:
            po, no = owners_of(prev['owners']), owners_of(s['owners'])
            for k, v in no.items():
                if k in rel_ids and po.get(k) is not None and po.get(k) != v:
                    captures.append({'frame': s['frame'], 'stamp': s.get('stamp'), 'obj': k, 'from': num(po[k]),
                                     'to': num(v)})
            if s['armed'] == 1 and prev['armed'] != 1:
                arms.append(s)
            if prev['armed'] == 1 and s['armed'] != 1:
                disarms.append(s)
        elif s['armed'] == 1:
            arms.append(s)
        if decl is None and s['decl'] == 1:
            decl = s
        prev = s
    res['captures'] = captures
    res['arms'] = [(a['frame'], a['timer'], a['cpp']) for a in arms]
    res['disarms'] = [d['frame'] for d in disarms]
    res['mon_ok'] = (m['mon'] or {}).get('ok') == '1'
    res['cpv'] = (m['cpv'] or {}).get('raw')
    res['gaps'] = m['gaps']
    res['mode_end'] = m['mode_end']
    if decl is None:
        res['notes'].append('no declaration seen (battle not won while recorded, or the monitor read failed: mon_ok=%s)'
                            % res['mon_ok'])
    A = None
    for a in arms:
        if decl is None or a['frame'] <= decl['frame']:
            A = a
    if A is None:
        res['notes'].append('the domination timer never armed (+0x2b0 stayed 0: no capture completed, or the offset is '
                            'wrong; CPV raw=%s)' % res['cpv'])
        return res
    C = None
    for c in captures:
        if c['frame'] <= A['frame']:
            C = c
    T = next((d for d in disarms if d['frame'] > A['frame']), None)
    E = None
    if decl is not None:
        E = next((s for s in st if s['frame'] >= decl['frame'] and s['cd'] is not None and s['cd'] <= 0), None)
    t = {'C': C['frame'] if C else None, 'A': A['frame'], 'timer_at_A': A['timer'], 'player_at_A': A['cpp'],
         'T': T['frame'] if T else None, 'D': decl['frame'] if decl else None,
         'cd_at_D': decl['cd'] if decl else None, 'dtype': decl['dtype'] if decl else None,
         'dplayer': decl['dplayer'] if decl else None, 'E': E['frame'] if E else None,
         'stall_after_D': next((f for f in m['stalls'] if decl and f is not None and f >= decl['frame']), None),
         'mode_end_frame': num((m['mode_end'] or {}).get('last_frame')),
         'stamps': {'C': C.get('stamp') if C else None, 'A': A.get('stamp'), 'T': T.get('stamp') if T else None,
                    'D': decl.get('stamp') if decl else None, 'E': E.get('stamp') if E else None}}
    res['frames'] = t
    if C is None:
        res['notes'].append('no relevant point changed owner before the arm frame (captured before the first walk?)')
    # per-frame consistency of the two counters
    bad = 0
    for s in st:
        if s['armed'] == 1 and A['frame'] <= s['frame'] and (T is None or s['frame'] < T['frame']):
            if s['timer'] is not None and s['timer'] + s['frame'] != A['timer'] + A['frame']:
                bad += 1
    res['timer_steps_off'] = bad
    bad = 0
    if decl is not None:
        for s in st:
            if s['frame'] > decl['frame'] and (E is None or s['frame'] <= E['frame']) and s['cd'] is not None:
                if s['cd'] + s['frame'] != decl['cd'] + decl['frame']:
                    bad += 1
    res['countdown_steps_off'] = bad
    mv = {}
    if C is not None:
        mv['timer_at_capture'] = A['timer'] if A['frame'] == C['frame'] else None
        mv['arm_minus_capture'] = A['frame'] - C['frame']
        if T is not None:
            mv['capture_to_test'] = T['frame'] - C['frame']
        if E is not None:
            mv['capture_to_zero'] = E['frame'] - C['frame']
    if T is not None and decl is not None:
        mv['test_to_decl'] = decl['frame'] - T['frame']
    if decl is not None:
        mv['decl_countdown'] = decl['cd']
        if E is not None:
            mv['decl_to_zero'] = E['frame'] - decl['frame']
    res['measured'] = mv
    res['compare'] = {k: {'retail': mv.get(k), 'port': PORT[k],
                          'verdict': 'n/a' if mv.get(k) is None else ('MATCH' if mv[k] == PORT[k] else
                                                                       'DIFF %+d' % (mv[k] - PORT[k]))}
                      for k in PORT}
    if decl is not None:
        if decl['dtype'] != VICTORY_0x15:
            res['notes'].append('declared type %s, not 0x15 (21)' % decl['dtype'])
        if res['human'] and decl['dplayer'] not in res['human']:
            res['notes'].append('declared player %s is not the human %s' % (decl['dplayer'], res['human']))
    return res


def report(meta, modes, lua, cfg):
    L = ['ccpgu t02 Capture Control Points probe report', '=' * 46, '']
    g = meta.get('GAME', {})
    L.append('game: pid=%s identity=%s; reader exit: %s' % (g.get('pid'), g.get('identity'),
                                                          ' '.join('%s=%s' % kv for kv in meta.get('EXIT', {}).items())))
    if cfg:
        exp = cfg.get('expected') or {}
        L.append('config: map=%s human=%s ai=%s squads=%s; map file: %s relevant point(s) %s' % (
            cfg.get('map'), cfg.get('human'), cfg.get('ai'), ','.join('%s x%d' % (s['type'], s['n']) for s in cfg.get('squads', [])),
            len(exp.get('relevant', [])) if exp else '?',
            ', '.join('#%s %s raw %s (%s) at (%s, %s)' % (r['map_id'], r['type'], r['raw_owner'], r['raw_faction'], r['x'],
                                                          r['y']) for r in exp.get('relevant', []))))
    out = {'battles': [], 'skipped': []}
    for m in modes:
        res = analyse_mode(m, lua)
        if not res['human']:
            out['skipped'].append(m['n'])
            L += ['', 'mode %d: no human player (main-menu background battle): skipped' % m['n']]
            continue
        out['battles'].append(res)
        L += ['', 'battle (mode %d), human player id(s) %s (from %s), players %s' % (
            m['n'], res['human'], res['human_src'], ', '.join('%s=%s%s' % (i, p['faction'], '*' if p['human'] else '')
                                            for i, p in sorted(m['players'].items(), key=lambda x: (x[0] is None, x[0]))))]
        L.append('  reader: monitor ok=%s, CPV raw=%s, frame gaps=%d' % (res['mon_ok'], res['cpv'], res['gaps']))
        L.append('  PART 2 - point objects at the first walk (start owner):')
        for p in res['points']:
            L.append('    obj %-7s %-34s relevant=%d start owner %s (%s) at (%s, %s)' % (
                p['obj'], p['type'], p['relevant'], p['start_owner'], p['start_faction'], p['x'], p['y']))
        if not res['points']:
            L.append('    none found (walk failed or the map has none)')
        rel = [p for p in res['points'] if p['relevant']]
        facs = sorted({p['start_faction'] for p in rel})
        L.append('    -> relevant points: %d, start owner faction(s): %s (port: Neutral, tactical_battle_bt.cpp:696)%s' % (
            len(rel), ', '.join(facs) or '-', '' if facs == ['NEUTRAL'] else '  <-- CHECK'))
        if cfg and cfg.get('expected'):
            n_exp = len(cfg['expected'].get('relevant', []))
            L.append('    -> map file places %d relevant point(s): %s' % (n_exp, 'same count' if n_exp == len(rel) else
                                                                          'COUNT DIFFERS (another map was played?)'))
        L.append('  PART 1 - capture -> battle end:')
        for c in res['captures']:
            L.append('    capture frame %s: obj %s owner %s -> %s' % (c['frame'], c['obj'], c['from'], c['to']))
        L.append('    arms (frame, timer, player): %s; disarms: %s' % (res['arms'], res['disarms']))
        fr = res.get('frames')
        if fr:
            L.append('    C=%(C)s A=%(A)s timer@A=%(timer_at_A)s player@A=%(player_at_A)s T=%(T)s D=%(D)s cd@D=%(cd_at_D)s '
                     'type=%(dtype)s player=%(dplayer)s E(countdown<=0)=%(E)s stall after D=%(stall_after_D)s '
                     'mode end=%(mode_end_frame)s' % fr)
            L.append('    counter steps off the 1-per-frame line: timer %d, countdown %d' % (
                res['timer_steps_off'], res['countdown_steps_off']))
            L.append('    frames are ticks: %d ST line(s) repeated the previous stamp and were moved to the next tick'
                     ' (the frame counter is written after the sim state); logged stamps C=%s A=%s T=%s D=%s E=%s' % (
                         res['stamp_repeats'], *(fr['stamps'][k] for k in 'CATDE')))
            L.append('    %-18s %8s %6s  %-8s %s' % ('value', 'retail', 'port', 'verdict', 'port source'))
            for k, v in res['compare'].items():
                L.append('    %-18s %8s %6s  %-8s %s' % (k, v['retail'], v['port'], v['verdict'], PORT_REF[k]))
            ctz = res['compare']['capture_to_zero']
            L.append('    RESULT capture -> end = %s frames (port %d = nominal 900 + 210 - 1; [M] +-1): %s' % (
                ctz['retail'], PORT['capture_to_zero'], ctz['verdict']))
            if fr.get('stall_after_D') is not None and fr.get('E') is not None:
                L.append('    visible end: the frame counter stopped at %s (%+d vs the countdown-zero tick %s; -1 = the battle'
                         ' ended inside that tick, before its counter write)' % (
                             fr['stall_after_D'], fr['stall_after_D'] - fr['E'], fr['E']))
        for nte in res['notes']:
            L.append('    NOTE: ' + nte)
    # Lua
    L += ['', 'Lua records (%d):' % len(lua)]
    by_kind = {}
    for _n, r in lua:
        by_kind.setdefault(r['kind'], []).append(r)
    for k in ('LOAD', 'SKIP', 'ARM', 'PL', 'ENUM', 'PTS', 'PT', 'SPAWN', 'SPAWNED', 'NOSPAWN', 'OWN', 'ALL', 'LOST', 'APIERR',
              'ERR', 'DONE'):
        for r in sorted(by_kind.get(k, []), key=lambda r: (r.get('sid', ''), num(r.get('n')) or 0)):
            L.append('  ' + k + ' ' + ' '.join('%s=%s' % (a, b) for a, b in r.items() if a != 'kind'))
    hb = sorted(by_kind.get('HB', []), key=lambda r: num(r.get('n')) or 0)
    if hb:
        L.append('  HB x%d, last: %s' % (len(hb), ' '.join('%s=%s' % (a, b) for a, b in hb[-1].items() if a != 'kind')))
    if not by_kind.get('ARM'):
        L.append('  NOTE: no ARM record: the hook did not run in the battle (wrong AI faction, no human Empire, or the'
                 ' records were not found)')
    out['lua'] = {k: len(v) for k, v in by_kind.items()}
    return '\n'.join(L) + '\n', out


def cmd_parse(a):
    cfg = None
    if getattr(a, 'config', None) and os.path.exists(a.config):
        cfg = json.load(open(a.config))
    meta, modes, lua = load(a.log)
    text, out = report(meta, modes, lua, cfg)
    if a.out:
        with open(a.out, 'w') as f:
            f.write(text)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(out, f, indent=1, default=str)
    print(text)
    return out


# ---- selftest ----------------------------------------------------------------------------------------------------

def synth(path, C=600, timer_at_c=899, test_after=899, cd_at_d=210, zero_after=210, background=True, lua=True):
    """A reader log in the port model (defaults) or shifted by the arguments."""
    L = ['CCPREADER 1 scan_sec=5 poll_sec=0.002 map=_MP_LAND_KAMINO human=Empire',
         'GAME pid=1 base=0x140000000 timestamp=0x67002393 size_of_image=0xcc0000 identity=ok']
    n = 0
    if background:
        n += 1
        L += ['MODE n=%d ptr=0x1 type=1 game_type=1 stack=1 frame=0 gframe=0 t=1.0' % n,
              'PL n=%d id=0 index=0 faction=REBEL human=0 neutral=0' % n,
              'PL n=%d id=1 index=1 faction=EMPIRE human=0 neutral=0' % n,
              'ST n=%d frame=0 armed=0 timer=-1 cpp=-1 decl=0 cd=0 dtype=0 dplayer=0 dcd=0 hold=0 owners=5:3' % n,
              'MODE_END n=%d last_frame=900 why=mode_changed' % n]
    n += 1
    L += ['MODE n=%d ptr=0x2 type=1 game_type=4 stack=1 frame=0 gframe=0 t=9.0' % n,
          'PL n=%d id=0 index=0 faction=REBEL human=0 neutral=0' % n,
          'PL n=%d id=1 index=1 faction=EMPIRE human=1 neutral=0' % n,
          'PL n=%d id=3 index=3 faction=NEUTRAL human=0 neutral=1' % n,
          'POINT n=%d frame=0 obj=82036 addr=0x10 type=REINFORCEMENT_POINT relevant=1 owner=3 owner_faction=NEUTRAL '
          'x=3453.5 y=2791.4' % n,
          'CPV n=%d frame=0 raw=00000000ffffffffffffffff00000000' % n,
          'MON n=%d frame=0 ptr=0x100 ok=1 back=0x2' % n]

    def st(f, armed, timer, cpp, decl, cd, dtype, dplayer, owner):
        return ('ST n=%d frame=%d armed=%d timer=%d cpp=%d decl=%d cd=%d dtype=%d dplayer=%d dcd=%d hold=0 owners=82036:%d'
                % (n, f, armed, timer, cpp, decl, cd, dtype, dplayer, cd if decl else 0, owner))
    L.append(st(0, 0, -1, -1, 0, 0, 0, 0, 3))
    for f in range(C, C + test_after):
        L.append(st(f, 1, timer_at_c - (f - C), 1, 0, 0, 0, 0, 1))
    D = C + test_after
    for k in range(0, zero_after + 1):
        L.append(st(D + k, 0, -1, -1, 1, max(0, cd_at_d - k * cd_at_d // zero_after) if zero_after else 0, 0x15, 1, 1))
    L.append('STALL n=%d frame=%d sec=1.5' % (n, D + zero_after + 2))
    L.append('MODE_END n=%d last_frame=%d why=game_exited' % (n, D + zero_after + 2))
    if lua:
        L += ['LUA n=%d frame=40 addr=0x1 rec=CCP1|ARM|sid=abc|n=2|f=3|fac=REBEL|mode=Land|human=Empire|END' % n,
              'LUA n=%d frame=40 addr=0x2 rec=CCP1|PT|sid=abc|n=4|f=30|cls=relevant|type=Reinforcement_Point|obj=82036|'
              'owner=NEUTRAL:3|xy=3454,2791|END' % n]
    L.append('EXIT reason=game_exited modes=%d scans=3 records=2' % n)
    with open(path, 'w') as f:
        f.write('\n'.join(L) + '\n')


def synth_retail(path):
    n = 1
    L = ['CCPREADER 1 scan_sec=5 poll_sec=0.002 map=_MP_LAND_KAMINO human=Empire',
         'MODE n=1 ptr=0x2 type=1 game_type=3 stack=1 frame=0 gframe=0 t=55.4',
         'PL n=1 id=0 index=0 faction=REBEL human=0 neutral=0',
         'PL n=1 id=1 index=1 faction=EMPIRE human=0 neutral=0',
         'PL n=1 id=3 index=3 faction=NEUTRAL human=0 neutral=1',
         'MON n=1 frame=0 ptr=0x100 ok=1 back=0x2',
         'POINT n=1 frame=0 obj=82036 addr=0x10 type=REINFORCEMENT_POINT relevant=1 owner=3 owner_faction=NEUTRAL x=1 y=2',
         'POINT n=1 frame=0 obj=82113 addr=0x20 type=REINFORCEMENT_POINT_REBEL relevant=0 owner=1 owner_faction=EMPIRE '
         'x=2919.4 y=2023.9']

    def st(f, armed, timer, cpp, decl, cd, dtype=0, dplayer=0, owner=0):
        return ('ST n=%d frame=%d armed=%d timer=%d cpp=%d decl=%s cd=%s dtype=%s dplayer=%s dcd=%s hold=0 '
                'owners=82036:%d,82113:1' % (n, f, armed, timer, cpp, decl, cd, dtype, dplayer, cd, owner))
    L.append(st(0, 256, -1, -1, 0, 0, owner=3))
    for f in range(454, 1353):
        L.append(st(f, 257, 1353 - f, 0, 0, 0))
        if f == 645:
            L.append(st(645, 257, 1353 - 646, 0, 0, 0))  # tick 646 seen before the counter write
        if f == 1044:                                    # chained: ticks 1045 and 1046 both seen early
            L += [st(1044, 257, 1353 - 1045, 0, 0, 0), st(1045, 257, 1353 - 1046, 0, 0, 0)]
    L = [ln for ln in L if ' frame=646 ' not in ln and not ln.startswith('ST n=1 frame=1045 armed=257 timer=308 ')
         and not ln.startswith('ST n=1 frame=1046 ')]
    L.append(st(1352, 257, 0, 0, 1, 210, 21, 0))     # tick 1353: test + declaration, seen with stamp 1352
    for f in range(1353, 1563):
        L.append(st(f, 256, -1, -1, 1, 1563 - f, 21, 0))  # 1353: disarmed, countdown 210; 1562: countdown 1
    L.append(st(1562, 256, -1, -1, 1, 0, 21, 0))     # tick 1563: countdown 0, same stamp
    L.append('ST n=1 frame=1562 armed=256 timer=-1 cpp=-1 decl=None cd=None dtype=None dplayer=None dcd=None hold=None '
             'owners=82036:0,82113:1')
    L += ['STALL n=1 frame=1562 sec=1.5', 'MODE_END n=1 last_frame=1562 why=mode_changed',
          'LUA n=1 frame=90 addr=0x1 rec=CCP1|ARM|sid=0A72E0|n=2|f=63|fac=REBEL|mode=Land|human=Empire|human_id=0|END',
          'LUA n=1 frame=90 addr=0x2 rec=CCP1|PL|sid=0A72E0|n=3|f=63|name=Empire|id=0|faction=EMPIRE|human=true|END',
          'LUA n=1 frame=90 addr=0x3 rec=CCP1|PL|sid=0A72E0|n=4|f=63|name=Rebel|id=1|faction=REBEL|human=false|END',
          'LUA n=1 frame=90 addr=0x4 rec=CCP1|PL|sid=0A72E0|n=5|f=63|name=Neutral|id=3|faction=NEUTRAL|human=false|END',
          'EXIT reason=game_exited modes=1 scans=3 records=4']
    with open(path, 'w') as f:
        f.write('\n'.join(L) + '\n')


def selftest():
    fails = []

    def check(c, what):
        print(('  ok    ' if c else '  FAIL  ') + what)
        if not c:
            fails.append(what)

    tmp = os.path.join(os.environ.get('TEMP', '.'), 'ccp_parse_selftest_%d.log' % os.getpid())
    try:
        synth(tmp)
        meta, modes, lua = load(tmp)
        text, out = report(meta, modes, lua, None)
        b = out['battles'][0] if out['battles'] else None
        check(out['skipped'] == [1] and b is not None, 'background battle skipped, one battle analysed')
        check(b and all(v['verdict'] == 'MATCH' for v in b['compare'].values()),
              'port model -> every value MATCH (%s)' % (b and {k: v['verdict'] for k, v in b['compare'].items()}))
        check(b and b['frames']['C'] == 600 and b['frames']['T'] == 1499 and b['frames']['E'] == 1709,
              'frames C / T / E (%s)' % (b and b['frames']))
        check(b and b['timer_steps_off'] == 0 and b['countdown_steps_off'] == 0, 'counters step once per frame')
        check('RESULT capture -> end = 1109 frames' in text and 'start owner faction(s): NEUTRAL' in text,
              'report lines')
        check('ARM sid=abc' in text and 'PT sid=abc' in text, 'Lua records listed')
        # retail arms after the mode tick (timer 900 at the capture frame) and the end comes one frame later
        synth(tmp, timer_at_c=900, test_after=900, lua=False)
        meta, modes, lua = load(tmp)
        text, out = report(meta, modes, lua, None)
        b = out['battles'][0]
        check(b['compare']['timer_at_capture']['verdict'] == 'DIFF +1' and
              b['compare']['capture_to_zero']['verdict'] == 'DIFF +1', 'one-frame-later model -> DIFF +1')
        check('no ARM record' in text, 'missing Lua noted')
        # the retail run 20261108-214851 shape: pre-remap PL without a human, armed read as an i32 (257 / 256),
        # declaration and countdown zero logged as a second line with the previous stamp, monitor gone at the end
        synth_retail(tmp)
        meta, modes, lua = load(tmp)
        text, out = report(meta, modes, lua, None)
        b = out['battles'][0] if out['battles'] else None
        check(b is not None and b['human'] == [0] and b['human_src'] == 'lua', 'retail shape: human 0 from Lua')
        check(b and b['frames']['C'] == 454 and b['frames']['T'] == 1353 and b['frames']['D'] == 1353 and
              b['frames']['E'] == 1563 and b['frames']['stamps']['E'] == 1562,
              'retail shape: ticks C 454 / T 1353 / D 1353 / E 1563 (%s)' % (b and b.get('frames')))
        check(b and all(v['verdict'] == 'MATCH' for v in b['compare'].values()) and b['timer_steps_off'] == 0 and
              b['countdown_steps_off'] == 0, 'retail shape: every value MATCH after the tick correction (%s)' % (
                  b and {k: v['verdict'] for k, v in b['compare'].items()}))
        check(b and any(p['type'] == 'REINFORCEMENT_POINT_REBEL' and p['start_faction'] == 'REBEL' for p in b['points']),
              'retail shape: point factions relabelled from Lua')
        # no capture at all
        synth(tmp, C=10 ** 6, test_after=0, zero_after=0, lua=False)
        meta, modes, lua = load(tmp)
        _text, out = report(meta, modes, lua, None)
        b = out['battles'][0]
        check('frames' not in b or b['frames']['C'] is not None, 'degenerate log does not crash')
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    print('PARSE SELFTEST %s' % ('OK' if not fails else 'FAILED'))
    return 0 if not fails else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('parse')
    s.add_argument('log')
    s.add_argument('--config')
    s.add_argument('--out')
    s.add_argument('--json')
    a = ap.parse_args()
    cmd_parse(a)
    return 0


if __name__ == '__main__':
    sys.exit(main())
