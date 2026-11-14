#!/usr/bin/env python3
"""s10_parse.py - turns the S10 probe log (_S10MapOwners.txt) into a raw-map-owner -> in-game-owner table.

  s10_parse.py parse <log> [--out table.txt] [--tag t0] [--no-popups]
                    <log> = the hook log (_S10MapOwners.txt) or an s10reader log (first line S10READER)
  s10_parse.py fake <out.log> [--rule faction|slot]   fake log from expected_objects.json (parser self-test)
  s10_parse.py selftest                                fake logs for both rules -> parse -> check the verdicts

Inputs next to this script: maps.json (scenarios, the port's player tables) and expected_objects.json (the map
placements with their raw owner id, written by build_mod.py / `ted_owners.py expected --json`).

s10reader logs (reader/s10reader.cpp: memory dumps "O id= owner= type= pos=" of the land mode's object store, plus the
text of every message box): the owner is the player id. Map per battle = the hook popup's map name, else the scenario
map whose placements match best. Faction per player id = the hook popup "S10 t0 <map> FACTION=id:count ..." (from
Find_Player), else a vote over the logged objects whose type has a single-faction <Affiliation>.

Join: every map placement (type + raw owner + x, y) is matched to the nearest unused logged object of the same type
within 25 units (x, y only; z follows the terrain). For each raw owner id the table gives how many placements were
found, which in-game owner (faction / player id) they had, and three checks:
  id   - in-game owner Get_ID() == raw owner id
  fac  - in-game owner faction == faction_order[raw] (Factions.xml then Expansion_Factions.xml order)
  port - the port's guess: port_players[raw] (the port's battle player table), 'drop' when raw >= its size
         (TacticalBattle::bt_owner_valid destroys the placement).
"""
import argparse
import json
import math
import os
import re
import sys

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
TOL = 25.0
POINT_KINDS = ('reinforcement_point', 'capture_point')

KV = re.compile(r'(\w+)=(\S*)')


def norm_map(m):
    m = m.replace('/', '\\').split('\\')[-1].upper()
    return m[:-4] if m.endswith('.TED') else m


def kv(s):
    return dict(KV.findall(s))


def parse_log(path):
    """[battle] with battle = {n, mode, map, players_at_start, dumps: [{tag, local, players, objs}]}"""
    battles, cur, dump = [], None, None
    with open(path, encoding='latin-1') as f:
        for raw in f:
            line = raw.strip()
            if line.startswith('MODE_START'):
                d = kv(line)
                if d.get('mode', '').upper() == 'LAND':
                    cur = {'n': int(d.get('battle', '0') or 0), 'mode': d.get('mode'), 'map': norm_map(d.get('map', '?')),
                           'start_players': {}, 'dumps': []}
                    battles.append(cur)
                else:
                    cur = None
                dump = None
                continue
            if cur is None:
                continue
            if line.startswith('PLAYER ') and dump is None:
                d = kv(line)
                cur['start_players'][d.get('query')] = d
            elif line.startswith('DUMP'):
                d = kv(line)
                dump = {'tag': d.get('tag'), 'time': d.get('time'), 'local': None, 'players': {}, 'objs': [],
                        'complete': False}
                cur['dumps'].append(dump)
            elif dump is not None and line.startswith('LOCAL'):
                dump['local'] = kv(line)
            elif dump is not None and line.startswith('PLAYER '):
                d = kv(line)
                dump['players'][d.get('query')] = d
            elif dump is not None and line.startswith('OBJ'):
                head, _, own = line.partition(' owner:')
                d = kv(head)
                o = kv(own)
                pos = None
                if 'pos' in d:
                    try:
                        pos = tuple(float(v) for v in d['pos'].split(','))
                    except ValueError:
                        pos = None
                elif 'dist' in d:
                    try:
                        d0, d1, d2 = (float(v) for v in d['dist'].split(','))
                        pos = ((d0 * d0 - d1 * d1 + 1.0e6) / 2000.0, (d0 * d0 - d2 * d2 + 1.0e6) / 2000.0, 0.0)
                    except ValueError:
                        pos = None
                dump['objs'].append({'src': d.get('src'), 'type': d.get('type', '?'), 'pos': pos,
                                     'owner_id': o.get('id'), 'owner_faction': o.get('faction'),
                                     'owner_human': o.get('human')})
            elif dump is not None and line.startswith('END'):
                dump['complete'] = True
    return battles


def tkey(t):
    """The hook writes type names with whitespace replaced by '_' (one token per field)."""
    return re.sub(r'\s+', '_', t.strip()).upper()


def dedup(objs):
    seen, out = set(), []
    for o in objs:
        key = (tkey(o['type']), None if o['pos'] is None else (round(o['pos'][0], 1), round(o['pos'][1], 1)))
        if key in seen:
            continue
        seen.add(key)
        out.append(o)
    return out


def join(placements, objs):
    """[(placement, obj or None, dist)] in placement order; nearest unused same-type object within TOL."""
    by_type = {}
    for i, o in enumerate(objs):
        by_type.setdefault(tkey(o['type']), []).append(i)
    used, out = set(), []
    for p in placements:
        best, bd = None, TOL
        for i in by_type.get(tkey(p['type']), []):
            o = objs[i]
            if i in used or o['pos'] is None:
                continue
            dd = math.hypot(o['pos'][0] - p['pos'][0], o['pos'][1] - p['pos'][1])
            if dd <= bd:
                best, bd = i, dd
        if best is not None:
            used.add(best)
            out.append((p, objs[best], bd))
        else:
            out.append((p, None, None))
    return out, used


def fac_eq(a, b):
    return (a or '').upper() == (b or '').upper()


def report_battle(b, scen, exp, order, tag_pref, w):
    dumps = [d for d in b['dumps'] if d['objs']]
    if not dumps:
        w('== battle %d map %s: no dump with objects (dumps: %s)' % (
            b['n'], b['map'], ', '.join('%s/%d' % (d['tag'], len(d['objs'])) for d in b['dumps']) or 'none'))
        return None
    dump = next((d for d in dumps if d['tag'] == tag_pref), dumps[0])
    sid = scen['id'] if scen else 'UNKNOWN'
    port = scen.get('port_players', []) if scen else []
    w('')
    w('== %s  battle %d  map %s  dump %s (game time %s, %d objects logged)' % (
        sid, b['n'], b['map'], dump['tag'], dump['time'], len(dump['objs'])))
    if scen:
        if scen['mode'] == 'campaign':
            w('   campaign: attacker %s, defender %s' % (scen['attacker'], scen['defender']))
        else:
            w('   skirmish slots: %s' % ', '.join('%d=%s/%s' % (i + 1, f, c) for i, (f, c) in enumerate(scen['slots'])))
    loc = dump['local'] or {}
    w('   local player: %s id=%s' % (loc.get('faction'), loc.get('id')))
    w('   players (Find_Player): ' + ' | '.join(
        '%s id=%s%s' % (q, p.get('id'), ' HUMAN' if p.get('human') == 'true' else '')
        for q, p in dump['players'].items() if p.get('id') not in (None, 'nil')))
    if exp is None:
        w('   (map not in expected_objects.json: no join)')
        return None
    objs = dedup(dump['objs'])
    pl = [r for r in exp['objects']]
    joined, used = join(pl, objs)
    rows = {}
    for p, o, dd in joined:
        r = rows.setdefault(p['owner'], {'n': 0, 'found': 0, 'owners': {}, 'kinds': {}, 'id': 0, 'fac': 0, 'port': 0,
                                         'npoints': 0})
        r['n'] += 1
        r['kinds'][p['kind']] = r['kinds'].get(p['kind'], 0) + 1
        if o is None:
            continue
        r['found'] += 1
        key = '%s id=%s' % (o['owner_faction'], o['owner_id'])
        r['owners'][key] = r['owners'].get(key, 0) + 1
        if str(o['owner_id']) == str(p['owner']):
            r['id'] += 1
        if p['owner'] < len(order) and fac_eq(o['owner_faction'], order[p['owner']]):
            r['fac'] += 1
        if p['owner'] < len(port) and fac_eq(o['owner_faction'], port[p['owner']]):
            r['port'] += 1
    w('   %-4s %-6s %-6s %-34s %-9s %-9s %-9s %s' % ('raw', 'placed', 'found', 'in-game owner (count)', 'id==raw',
                                                   'fac==raw', 'port ok', 'port guess'))
    result = {}
    for raw in sorted(rows):
        r = rows[raw]
        guess = port[raw] if raw < len(port) else 'drop'
        owners = ', '.join('%s:%d' % kv_ for kv_ in sorted(r['owners'].items(), key=lambda x: -x[1])) or '-'
        f = r['found'] or 1
        w('   %-4s %-6d %-6d %-34s %-9s %-9s %-9s %s' % (
            raw, r['n'], r['found'], owners, '%d/%d' % (r['id'], r['found']), '%d/%d' % (r['fac'], r['found']),
            '%d/%d' % (r['port'], r['found']) if guess != 'drop' else ('0/%d' % r['found']), guess))
        result[raw] = {'placed': r['n'], 'found': r['found'], 'owners': r['owners'], 'id_eq': r['id'] / f if r['found'] else None,
                       'fac_eq': r['fac'] / f if r['found'] else None, 'port_ok': (r['port'] / f if guess != 'drop' else 0.0) if r['found'] else None,
                       'port_guess': guess}
    w('   points (reinforcement / capture; raw-3 = Neutral ones summarised):')
    neutral = {}
    for p, o, dd in joined:
        if p['kind'] not in POINT_KINDS:
            continue
        if p['owner'] == 3:
            k = 'NOT FOUND' if o is None else '%s id=%s' % (o['owner_faction'], o['owner_id'])
            neutral[k] = neutral.get(k, 0) + 1
            continue
        w('     raw %-2s %-36s (%8.1f %8.1f) -> %s' % (
            p['owner'], p['type'], p['pos'][0], p['pos'][1],
            'NOT FOUND' if o is None else '%s id=%s%s' % (o['owner_faction'], o['owner_id'],
                                                         '' if dd < 1.0 else ' (moved %.1f)' % dd)))
    if neutral:
        w('     raw 3  %d points -> %s' % (sum(neutral.values()),
                                          ', '.join('%s: %d' % t for t in sorted(neutral.items()))))
    extra, xy = {}, {}
    for i, o in enumerate(objs):
        if i in used:
            continue
        k = '%s id=%s' % (o['owner_faction'], o['owner_id'])
        extra.setdefault(k, {})
        extra[k][o['type']] = extra[k].get(o['type'], 0) + 1
        if o['pos'] is not None:
            xy.setdefault(k, []).append(o['pos'])
    points = [p for p in pl if p['kind'] in POINT_KINDS and 'REINFORCEMENT' in p['type'].upper()]
    w('   logged objects with no map placement (spawned at battle start: forces, base layouts, squad members),')
    w('   with their centroid and the nearest Reinforcement_Point* placement (where the side landed / started):')
    for k in sorted(extra):
        items = sorted(extra[k].items(), key=lambda x: -x[1])
        near = ''
        if xy.get(k):
            cx = sum(p[0] for p in xy[k]) / len(xy[k])
            cy = sum(p[1] for p in xy[k]) / len(xy[k])
            near = ' | centroid (%.0f %.0f)' % (cx, cy)
            if points:
                q = min(points, key=lambda p: math.hypot(p['pos'][0] - cx, p['pos'][1] - cy))
                near += ' nearest %s raw %d at %.0f' % (q['type'], q['owner'], math.hypot(q['pos'][0] - cx,
                                                                                          q['pos'][1] - cy))
        w('     %-24s %4d  %s%s%s' % (k, sum(extra[k].values()), ', '.join('%s x%d' % t for t in items[:8]),
                                      ' ...' if len(items) > 8 else '', near))
    later = [d for d in dumps if d is not dump]
    for d in later:
        lj, _ = join(pl, dedup(d['objs']))
        changes = []
        for (p, o0, _), (_, o1, _) in zip(joined, lj):
            if p['kind'] in POINT_KINDS and o0 is not None and o1 is not None and o0['owner_id'] != o1['owner_id']:
                changes.append('%s raw %d: %s -> %s' % (p['type'], p['owner'], o0['owner_faction'], o1['owner_faction']))
        w('   dump %s (time %s): %d point owner changes since %s%s' % (
            d['tag'], d['time'], len(changes), dump['tag'], (': ' + '; '.join(changes[:10])) if changes else ''))
    return result


# ---- s10reader logs (memory dumps + popup text) --------------------------------------------------------------------

OLINE = re.compile(r'^O id=(-?\d+) owner=(-?\d+) type=(.*?) pos=(\S+) health=(\S+) flags=(\d+)')
POPUP_SUMMARY = re.compile(r'S10 (t\d+|end) (\S+) ((?:[A-Za-z_]+=-?\d+:\d+\s*)+)')
POPUP_ALIVE = re.compile(r'S10 hook alive battle=\d+ mode=(\S+) map=(\S+)')


def is_reader_log(path):
    with open(path, encoding='latin-1') as f:
        return f.readline().startswith('S10READER')


def parse_reader_log(path):
    """(battles, popups). battle = {n, mode, map: None, start_players, dumps, popups, hook_map, hook_ids}; the dumps'
    objects have owner_id (the raw player id from memory) and owner_faction None (filled by assign_factions)."""
    battles, popups, cur, dump = [], [], None, None
    by_n = {}
    with open(path, encoding='latin-1') as f:
        for raw in f:
            line = raw.rstrip('\r\n')
            if line.startswith('MODE battle='):
                d = kv(line)
                cur = {'n': int(d['battle']), 'mode': {'1': 'Land', '2': 'Space'}.get(d.get('type'), d.get('type')),
                       'map': None, 'start_players': {}, 'dumps': [], 'popups': [], 'hook_map': None, 'hook_ids': {},
                       'stack': d.get('stack')}
                battles.append(cur)
                by_n[cur['n']] = cur
                dump = None
            elif line.startswith('MODE_END'):
                dump = None
            elif line.startswith('DUMP ') and cur is not None:
                d = kv(line)
                dump = {'tag': d.get('tag'), 'time': d.get('t'), 'local': None, 'players': {}, 'objs': [],
                        'complete': False, 'mode_frame': d.get('mode_frame')}
                cur['dumps'].append(dump)
            elif line.startswith('O ') and dump is not None:
                m = OLINE.match(line)
                if not m:
                    continue
                try:
                    pos = tuple(float(v) for v in m.group(4).split(','))
                except ValueError:
                    pos = None
                dump['objs'].append({'src': 'MEM', 'id': int(m.group(1)), 'type': m.group(3), 'pos': pos,
                                     'owner_id': m.group(2), 'owner_faction': None, 'owner_human': None,
                                     'health': m.group(5), 'dead': bool(int(m.group(6)) & 2)})
            elif line.startswith('END ') and dump is not None:
                dump['complete'] = True
                dump = None
            elif line.startswith('POPUP '):
                d = kv(line.split(' title=')[0])
                text = line.partition(' title=')[2]
                p = {'battle': int(d.get('battle', '0') or 0), 't': d.get('t'), 'text': text}
                popups.append(p)
                b = by_n.get(p['battle'])
                m = POPUP_ALIVE.search(text)
                if m and b is not None:
                    b['hook_map'] = norm_map(m.group(2))
                m = POPUP_SUMMARY.search(text)
                if m and b is not None:
                    b['hook_map'] = b['hook_map'] or norm_map(m.group(2))
                    for fac, pid, _ in re.findall(r'([A-Za-z_]+)=(-?\d+):(\d+)', m.group(3)):
                        b['hook_ids'].setdefault(fac.upper(), pid)
                if b is not None:
                    b['popups'].append(p)
    return battles, popups


def identify_map(objs, exp):
    """Best scenario map for a dump: the share of the map's non-marker placements matched (same type within TOL)."""
    best, best_score = None, -1.0
    objs = dedup(objs)
    for name, d in exp.items():
        pl = [r for r in d['objects'] if r['kind'] != 'marker' and not r['type'].startswith('?')]
        if not pl:
            continue
        joined, _ = join(pl, objs)
        score = sum(1 for _, o, _ in joined if o is not None) / len(pl)
        if score > best_score:
            best, best_score = name, score
    return best, best_score


def affiliations():
    """{TYPE NAME: faction} for the types whose XML Affiliation names exactly one faction."""
    import ted_owners
    out = {}
    for name, _, _, aff in ted_owners.load_types().values():
        facs = [x.strip() for x in aff.replace('\n', ',').split(',') if x.strip()]
        if len(facs) == 1:
            out[tkey(name)] = facs[0].upper()
    return out


def assign_factions(b, aff, use_popups=True):
    """Player id -> faction for a reader battle. First choice: the GameScoring hook's popup ("S10 t0 <map>
    FACTION=id:count ...", Find_Player per faction). Otherwise a vote over every logged object whose type has a
    single-faction Affiliation (independent of the map's raw owner field, so not circular). Sets owner_faction on
    every object ('?<id>' when nothing decides) and returns {id: (faction, evidence)}."""
    ids = {}
    if use_popups and b['hook_ids']:
        for fac, pid in b['hook_ids'].items():
            ids.setdefault(pid, (fac, 'hook popup (Find_Player)'))
    votes = {}
    for d in b['dumps']:
        for o in d['objs']:
            f = aff.get(tkey(o['type']))
            if f:
                votes.setdefault(o['owner_id'], {}).setdefault(f, 0)
                votes[o['owner_id']][f] += 1
    for pid, v in votes.items():
        if pid in ids:
            continue
        fac, n = max(v.items(), key=lambda x: x[1])
        total = sum(v.values())
        if n >= 3 and n >= 0.6 * total:
            ids[pid] = (fac, 'affiliation vote %d/%d' % (n, total))
    for d in b['dumps']:
        for o in d['objs']:
            o['owner_faction'] = ids.get(o['owner_id'], ('?' + str(o['owner_id']), ''))[0]
        d['players'] = {'%s#%s' % (f, pid): {'id': pid, 'evidence': ev} for pid, (f, ev) in ids.items()}
    b['ids'] = ids
    b['votes'] = votes
    return ids


def reader_battles(path, exp, use_popups=True, aff=None):
    battles, popups = parse_reader_log(path)
    aff = affiliations() if aff is None else aff
    for b in battles:
        first = next((d for d in b['dumps'] if d['objs']), None)
        if b['hook_map'] and b['hook_map'] in exp and use_popups:
            b['map'], b['map_score'], b['map_src'] = b['hook_map'], None, 'hook popup'
        elif first is not None:
            b['map'], b['map_score'] = identify_map(first['objs'], exp)
            b['map_src'] = 'placement match %.0f%%' % (100 * b['map_score'])
        else:
            b['map'], b['map_src'] = '?', 'no dump'
        assign_factions(b, aff, use_popups)
    return battles, popups


def reader_header(b, w):
    w('')
    w('-- reader battle %d (%s, stack %s): map %s (%s); dumps %s' % (
        b['n'], b['mode'], b.get('stack'), b['map'], b.get('map_src'),
        ', '.join('%s/%d' % (d['tag'], len(d['objs'])) for d in b['dumps']) or 'none'))
    for pid in sorted(b.get('ids', {}), key=lambda x: int(x)):
        f, ev = b['ids'][pid]
        w('   player id %s = %s (%s)' % (pid, f, ev))
    for pid in sorted(b.get('votes', {}), key=lambda x: int(x)):
        w('   affiliation votes id %s: %s' % (pid, ', '.join('%s %d' % t for t in sorted(b['votes'][pid].items(),
                                                                                         key=lambda x: -x[1]))))
    for p in b['popups']:
        w('   popup t=%s: %s' % (p['t'], p['text']))


def cmd_parse(a):
    cfg = json.load(open(os.path.join(HERE, 'maps.json')))
    exp_path = os.path.join(HERE, 'expected_objects.json')
    exp = json.load(open(exp_path)) if os.path.exists(exp_path) else {}
    order = cfg['faction_order']
    out = []
    w = out.append
    reader = is_reader_log(a.log)
    if reader:
        battles, popups = reader_battles(a.log, exp, use_popups=not getattr(a, 'no_popups', False),
                                         aff=getattr(a, 'aff', None))
        battles = [b for b in battles if b['mode'] == 'Land']
    else:
        battles, popups = parse_log(a.log), []
    w('S10 map-owner probe: %s (%s), %d land battle(s)' % (a.log, 's10reader memory log' if reader else 'hook log',
                                                         len(battles)))
    shown = {b['n'] for b in battles}
    for p in popups:
        if p['battle'] not in shown:
            w('popup (battle %d, t=%s): %s' % (p['battle'], p['t'], p['text']))
    w('Rules: id==raw -> the map owner field is the player id; fac==raw -> player id = faction index in %s' % order)
    results = {}
    for b in battles:
        if reader:
            reader_header(b, w)
        scen = next((s for s in cfg['scenarios'] if s['map'] == b['map']), None)
        r = report_battle(b, scen, exp.get(b['map']), order, a.tag, w)
        if r is not None:
            results['%s#%d' % (scen['id'] if scen else b['map'], b['n'])] = r
    text = '\n'.join(out) + '\n'
    sys.stdout.write(text)
    if a.out:
        with open(a.out, 'w') as f:
            f.write(text)
        with open(os.path.splitext(a.out)[0] + '.json', 'w') as f:
            json.dump(results, f, indent=1)
    return results


# ---- fake logs (self-test) -----------------------------------------------------------------------------------------

def fake_log(path, rule):
    """rule 'faction': raw owner r -> the player of faction_order[r] (id r);
       rule 'slot':    skirmish raw 0/1 -> slot 1/2 player, campaign raw 0 -> attacker, 1 -> defender; others Neutral."""
    cfg = json.load(open(os.path.join(HERE, 'maps.json')))
    exp = json.load(open(os.path.join(HERE, 'expected_objects.json')))
    order = cfg['faction_order']
    lines = ['S10HOOK 1 loaded', 'MODE_START battle=0 mode=Galactic map=? time=0.000 frame=0']
    n = 0
    for s in cfg['scenarios']:
        n += 1
        if s['mode'] == 'campaign':
            human = s['attacker']
            sides = [s['attacker'], s['defender']]
        else:
            human = s['slots'][0][0]
            sides = [s['slots'][0][0], s['slots'][1][0]]
        ids = {f: i for i, f in enumerate(order)}
        lines.append('MODE_START battle=%d mode=Land map=DATA\\ART\\MAPS\\%s.TED time=1.000 frame=30' % (n, s['map']))
        for tag in ('t0', 't10'):
            lines.append('DUMP battle=%d tag=%s mode=Land map=%s time=1.0 frame=30 services=1' % (n, tag, s['map']))
            lines.append('LOCAL id=%d faction=%s name=x human=true neutral=false' % (ids[human], human.upper()))
            for f in order:
                lines.append('PLAYER query=%s id=%d faction=%s name=x human=%s neutral=%s' % (
                    f, ids[f], f.upper(), 'true' if f == human else 'false', 'true' if f == 'Neutral' else 'false'))
            for i, r in enumerate(exp[s['map']]['objects']):
                if r['kind'] == 'marker' or (i % 17) == 5:
                    continue  # markers are not objects; drop a few as "destroyed"
                raw = r['owner']
                if rule == 'faction':
                    f = order[raw] if raw < len(order) else 'Neutral'
                else:
                    f = sides[raw] if raw < 2 else 'Neutral'
                x, y, z = r['pos']
                if i % 2:
                    lines.append('OBJ src=P:%s type=%s pos=%.2f,%.2f,%.2f hull=1 owner:id=%d faction=%s name=x human=%s '
                                 'neutral=false' % (f, r['type'], x + 0.5, y - 0.5, z, ids[f], f.upper(),
                                                    'true' if f == human else 'false'))
                else:  # the Get_Distance fallback
                    d = [math.sqrt(x * x + y * y + z * z), math.sqrt((x - 1000) ** 2 + y * y + z * z),
                         math.sqrt(x * x + (y - 1000) ** 2 + z * z)]
                    lines.append('OBJ src=T type=%s dist=%.2f,%.2f,%.2f hull=1 owner:id=%d faction=%s name=x human=false '
                                 'neutral=false' % (r['type'], d[0], d[1], d[2], ids[f], f.upper()))
            lines.append('OBJ src=P:%s type=Spawned_Unit pos=1.00,2.00,3.00 hull=1 owner:id=%d faction=%s name=x '
                         'human=true neutral=false' % (human, ids[human], human.upper()))
            lines.append('END battle=%d tag=%s lines=0' % (n, tag))
        lines.append('MODE_END mode=Land time=50.000 services=50')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def cmd_fake(a):
    fake_log(a.out, a.rule)
    print('wrote', a.out)


def cmd_selftest(a):
    tmp = os.path.join(HERE, 'out', 'selftest')
    os.makedirs(tmp, exist_ok=True)
    ok = True
    for rule in ('faction', 'slot'):
        log = os.path.join(tmp, 'fake_%s.txt' % rule)
        fake_log(log, rule)
        res = cmd_parse(argparse.Namespace(log=log, out=os.path.join(tmp, 'table_%s.txt' % rule), tag='t0'))
        for key, rows in res.items():
            for raw, r in rows.items():
                if not r['found']:
                    continue
                if rule == 'faction' and raw < 8 and r['fac_eq'] != 1.0:
                    print('SELFTEST FAIL %s %s raw %s fac_eq %s' % (rule, key, raw, r['fac_eq']))
                    ok = False
                if rule == 'faction' and raw < 8 and r['id_eq'] != 1.0:
                    print('SELFTEST FAIL %s %s raw %s id_eq %s' % (rule, key, raw, r['id_eq']))
                    ok = False
        if len(res) != len(json.load(open(os.path.join(HERE, 'maps.json')))['scenarios']):
            print('SELFTEST FAIL %s: %d battles parsed' % (rule, len(res)))
            ok = False
    print('SELFTEST OK' if ok else 'SELFTEST FAILED')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('parse')
    s.add_argument('log')
    s.add_argument('--out')
    s.add_argument('--tag', default='t0', help='primary dump (hook logs; reader logs use their first dump)')
    s.add_argument('--no-popups', action='store_true', help='reader logs: ignore the hook popups (map and player ids '
                                                             'from placements and affiliation votes only)')
    s = sub.add_parser('fake')
    s.add_argument('out')
    s.add_argument('--rule', default='faction', choices=('faction', 'slot'))
    sub.add_parser('selftest')
    a = ap.parse_args()
    if a.cmd == 'parse':
        cmd_parse(a)
    elif a.cmd == 'fake':
        cmd_fake(a)
    else:
        sys.exit(cmd_selftest(a))


if __name__ == '__main__':
    main()
