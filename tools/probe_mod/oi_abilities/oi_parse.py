#!/usr/bin/env python3
"""oi_parse.py - turns an oi_reader.py log into the OI ability probe report.

  oi_parse.py parse <reader.log> [--out report.txt] [--json report.json] [--config oi_config.json]
  oi_parse.py --selftest [--keep DIR]     synthetic reader log (port-model timings) -> parse -> check the numbers

Inputs per land battle (reader MODE n): the probe's Lua records (LUA lines: ARM, SCN, SPAWN, ACT, FIRED, P, APIERR,
ERR, DONE; frame f = mode frame) and the memory trace of the spawned objects (OBJ / ST / GONE: owner, flags, health
+0x5c, position, primary animation slot type:mode:position:speed). Spawn records are matched to memory objects by type,
frame and position.

Report, per scenario (frames are sim frames; "clip start" is extrapolated from the clip position samples:
start = frame - position / slope, slope = clip frames per sim frame measured on the same clip):
  MAL_LAND / MAL_SHALLOW / MAL_DEEP (§4.12): stun frame (Han's stun-blast projectile, else the Lua FIRED record),
    MAL death frame (flags 0x40 / health <= 0 / left the store) relative to it, first move after the stun (the MAL is
    ordered toward Han 30, 330 and 615 frames after the stun: a stunned unit cannot move), verdict;
  RANCOR_POUND: each POUND_ATTACK (63) clip: start, slope, tank damage frame and the clip position there, clip end;
  RANCOR_EAT: each EAT_ATTACK (64) clip: victim re-owned (attach) and destroyed frames and clip positions;
  GRENADE: BOMBTOSS (47) clip start, PROJ_STICKY_BOMB appearance frame and clip position, clip end, first frame the
    thrower does something else (resume);
  BERSERKER: activation (ACT / FIRED), SPINMOVE (104) start, loop length, every damage pass (health drop of a
    victim) relative to the apply and its clip position / fraction, against the port's model (counter % 5 = 0 and
    20 % < position < 80 % of the clip, ABILITIES_HARDPOINTS §4.13).
"""
import argparse
import json
import math
import os
import re
import sys

sys.dont_write_bytecode = True

DYING = 0x40
REMOVING = 0x02
POUND, EAT, BOMBTOSS, SPIN, ATTACK = 63, 64, 47, 104, 5
ANIM_NAMES = {0: 'Idle', 5: 'Attack', 6: 'Attack Idle', 7: 'Die', 2: 'Move', 47: 'Bombtoss', 63: 'Pound_Attack',
              64: 'Eat_Attack', 65: 'Eaten_Die', 104: 'SpinMove', 66: 'WalkMove'}

LINE_RE = re.compile(r'^([A-Z_]+) (.*)$')


# ---- log reading ---------------------------------------------------------------------------------------------------

def kvs(s):
    out = {}
    for part in s.split(' '):
        if '=' in part:
            k, v = part.split('=', 1)
            out[k] = v
    return out


def rec_parse(text):
    """OIP1|KIND|r=..|f=..|k=v|...|END -> (kind, fields). P records keep their tag list in fields['tags']."""
    parts = text.split('|')
    kind = parts[1]
    d = {}
    for p in parts[2:-1]:
        if kind == 'P' and ';' in p:
            d['tags'] = p
            continue
        if '=' in p:
            k, v = p.split('=', 1)
            d[k] = v
    try:
        d['F'] = int(d.get('f', ''))
    except ValueError:
        d['F'] = None
    return kind, d


def poll_tags(s):
    """'mal=1:0.900:100:200;han=1:1.000:5:6:0:1;proj=0;' -> {tag: {...}}"""
    out = {}
    for item in s.split(';'):
        if '=' not in item:
            continue
        tag, v = item.split('=', 1)
        f = v.split(':')
        if tag == 'proj':
            out['proj'] = f[0]
            continue
        e = {'valid': f[0] == '1'}
        if e['valid'] and len(f) >= 4:
            e['hull'] = _float(f[1])
            e['x'], e['y'] = _float(f[2]), _float(f[3])
            if len(f) >= 6:
                e['active'], e['ready'] = f[4], f[5]
        out[tag] = e
    return out


def _float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _int(v, default=None):
    try:
        return int(v, 0) if isinstance(v, str) else int(v)
    except (TypeError, ValueError):
        return default


def read_log(path):
    battles = {}
    records = {}      # text -> mode n first seen
    order = []
    meta = {'modeloff': [], 'scans': 0, 'src': False, 'exit': None, 'game': None}

    def battle(n):
        return battles.setdefault(n, {'n': n, 'type': None, 'objs': {}, 'players': {}, 'modeloff': None})

    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            m = LINE_RE.match(line.rstrip('\n'))
            if not m:
                continue
            kind, rest = m.group(1), m.group(2)
            if kind == 'LUA':
                i = rest.find('rec=')
                text = rest[i + 4:] if i >= 0 else ''
                d = kvs(rest[:i])
                if text.startswith('OIP1|') and text not in records:
                    records[text] = _int(d.get('n'), 0)
                    order.append(text)
                continue
            d = kvs(rest)
            n = _int(d.get('n'))
            if kind == 'MODE':
                b = battle(n)
                b['type'] = _int(d.get('type'))
                b['first_frame'] = _int(d.get('frame'))
            elif kind == 'PL':
                battle(n)['players'][_int(d.get('id'))] = d.get('faction')
            elif kind == 'OBJ':
                b = battle(n)
                oid = _int(d.get('obj'))
                b['objs'][oid] = {'id': oid, 'type': d.get('type'), 'role': d.get('role'), 'frame': _int(d.get('frame')),
                                  'x': _float(d.get('x')), 'y': _float(d.get('y')), 'hp0': _float(d.get('hp')),
                                  'owner0': _int(d.get('owner')), 'st': [], 'gone': None}
            elif kind == 'ST':
                b = battle(n)
                o = b['objs'].get(_int(d.get('obj')))
                if o is None:
                    continue
                an = d.get('anim', '-')
                a = None
                if an not in ('-', 'none'):
                    p = an.split(':')
                    if len(p) == 4:
                        a = (int(p[0]), int(p[1]), float(p[2]), float(p[3]))
                o['st'].append({'F': _int(d.get('frame')), 'owner': _int(d.get('owner')), 'flags': _int(d.get('flags'), 0),
                                'hp': _float(d.get('hp')), 'x': _float(d.get('x')), 'y': _float(d.get('y')),
                                'z': _float(d.get('z')), 'anim': a})
            elif kind == 'GONE':
                b = battle(n)
                o = b['objs'].get(_int(d.get('obj')))
                if o is not None and o['gone'] is None:
                    o['gone'] = _int(d.get('frame'))
            elif kind == 'MODELOFF':
                battle(n)['modeloff'] = d
                meta['modeloff'].append(d)
            elif kind == 'MODELOFF_FAIL':
                meta['modeloff'].append(dict(d, failed='1'))
            elif kind == 'SCAN':
                meta['scans'] += 1
            elif kind == 'SRC':
                meta['src'] = True
            elif kind == 'EXIT':
                meta['exit'] = d
            elif kind == 'GAME':
                meta['game'] = d
    recs = [(records[t],) + rec_parse(t) + (t,) for t in order]
    return battles, recs, meta


# ---- object analysis -----------------------------------------------------------------------------------------------

def death_frame(o):
    for s in o['st']:
        if (s['flags'] & DYING) or (s['hp'] is not None and s['hp'] <= 0.0):
            return s['F']
    return o['gone']


def hp_drops(o, f0=None, f1=None):
    out = []
    prev = o['hp0']
    for s in o['st']:
        if s['hp'] is None:
            continue
        if prev is not None and s['hp'] < prev - 0.005 and (f0 is None or s['F'] >= f0) and (f1 is None or s['F'] <= f1):
            out.append((s['F'], round(prev - s['hp'], 2), s['hp']))
        prev = s['hp']
    return out


def owner_changes(o):
    out = []
    prev = o['owner0']
    for s in o['st']:
        if s['owner'] != prev:
            out.append((s['F'], prev, s['owner']))
            prev = s['owner']
    return out


def segments(o):
    """Animation clips of an object: [{'type', 'samples': [(F, pos, mode, speed)], 'end': F | None}]. A clip starts
    when the type changes or the position goes back on a one-shot clip; a looping clip keeps one segment and its
    wraps are listed in 'wraps'."""
    segs = []
    cur = None
    for s in o['st']:
        a = s['anim']
        if a is None:
            continue
        typ, mode, pos, sp = a
        new = cur is None or typ != cur['type']
        if not new and mode != 1 and cur['samples'] and pos + 0.5 < cur['samples'][-1][1] and cur['samples'][-1][2] != 1:
            new = True
        if new:
            if cur is not None and cur['end'] is None:
                cur['end'] = s['F']
                cur['end_why'] = 'next clip'
            cur = {'type': typ, 'samples': [], 'end': None, 'end_why': None, 'wraps': []}
            segs.append(cur)
        elif cur['samples'] and pos + 0.5 < cur['samples'][-1][1]:
            cur['wraps'].append((s['F'], cur['samples'][-1][1], pos))
        cur['samples'].append((s['F'], pos, mode, sp))
        if mode == 2 and cur['end'] is None:
            cur['end'] = s['F']
            cur['end_why'] = 'finished'
    for sg in segs:
        sg['slope'], sg['start'] = clip_fit(sg)
    return segs


def clip_fit(sg):
    """(slope in clip frames per sim frame, extrapolated start frame) from the samples before the first wrap /
    finish; one sample per frame (the last one seen in that frame)."""
    pts = {}
    for F, pos, mode, _sp in sg['samples']:
        if mode == 2:
            break
        if pts and pos + 0.5 < pts[max(pts)]:
            break
        pts[F] = pos
    fs = sorted(pts)
    if len(fs) >= 2:
        n = len(fs)
        mf = sum(fs) / n
        mp = sum(pts[f] for f in fs) / n
        den = sum((f - mf) ** 2 for f in fs)
        k = sum((f - mf) * (pts[f] - mp) for f in fs) / den if den else None
    else:
        k = None
    if not fs:
        return None, None
    if k is None or k <= 0:
        return k, fs[0]
    return k, fs[0] - pts[fs[0]] / k


def pos_at(sg, F):
    """Clip position at sim frame F (last sample at or before F)."""
    best = None
    for f, pos, _m, _s in sg['samples']:
        if f <= F:
            best = pos
    return best


def fmt(v, nd=1):
    if v is None:
        return '?'
    if isinstance(v, float):
        return ('%.' + str(nd) + 'f') % v
    return str(v)


# ---- scenario binding ----------------------------------------------------------------------------------------------

def bind_spawns(battle, scn, spawns):
    """{tag: memory object} for one scenario: same type, OBJ frame within 15 frames of the SPAWN record, nearest."""
    out = {}
    used = set()
    for sp in spawns:
        if sp.get('ok') != '1':
            continue
        t = sp.get('type', '').upper()
        F = sp['F']
        x, y = _float(sp.get('x')), _float(sp.get('y'))
        best = None
        for o in battle['objs'].values():
            if o['type'] != t or o['id'] in used or o['frame'] is None or F is None:
                continue
            if abs(o['frame'] - F) > 15:
                continue
            d = math.hypot(o['x'] - x, o['y'] - y) if x is not None and o['x'] is not None else 0.0
            if best is None or d < best[0]:
                best = (d, o)
        if best is not None:
            used.add(best[1]['id'])
            out[sp['tag']] = best[1]
    for tag, o in scn.get('inherit', {}).items():
        out.setdefault(tag, o)
    return out


def scenarios_of(recs):
    """Group the records of one run (r=...) into scenarios, in order."""
    out = []
    cur = None
    for _n, kind, d, _t in recs:
        if kind == 'SCN' and d.get('ev') == 'start':
            cur = {'name': d.get('scn'), 'start': d['F'], 'end': None, 'recs': [], 'point': d.get('point')}
            out.append(cur)
            continue
        if cur is None:
            continue
        if kind == 'SCN' and d.get('ev') == 'end':
            cur['end'] = d['F']
            cur['end_rec'] = d
            cur = None
            continue
        if d.get('scn') == cur['name']:
            cur['recs'].append((kind, d))
    return out


# ---- per-scenario analysis -----------------------------------------------------------------------------------------

def analyse_mal(sc, objs, battle):
    res = {'name': sc['name'], 'lines': []}
    L = res['lines']
    mal, han = objs.get('mal'), objs.get('han')
    acts = [d for k, d in sc['recs'] if k == 'ACT']
    fired = [d for k, d in sc['recs'] if k == 'FIRED']
    water = next((d for k, d in sc['recs'] if k == 'WATER'), {})
    depth = _float(water.get('depth_cfg'))
    res['depth'] = depth
    proj = [o for o in battle['objs'].values() if o['type'] == 'PROJ_SPECIAL_HAN_SOLO_STUN_BLAST' and
            o['frame'] is not None and sc['start'] <= o['frame'] <= (sc['end'] or 10 ** 9)]
    stun = None
    src = None
    if proj:
        stun = min(o['frame'] for o in proj)
        src = 'stun-blast projectile in memory'
    elif fired:
        stun = fired[0]['F']
        src = 'Lua FIRED (3-frame resolution)'
    res['stun'] = stun
    L.append('depth under the water plane at the MAL point (config): %s' % fmt(depth, 2))
    for d in acts:
        L.append('ACT F=%s Activate_Ability(%s, %s): call=%s ready %s -> %s active %s' % (
            d['F'], d.get('ab'), d.get('arg'), d.get('call'), d.get('ready_before'), d.get('ready_after'),
            d.get('active_after')))
    L.append('stun: %s' % (('F=%d (%s)' % (stun, src)) if stun is not None else 'NOT SEEN (no projectile, no FIRED)'))
    lua_dead = None
    for k, d in sc['recs']:
        if k == 'P':
            e = poll_tags(d.get('tags', '')).get('mal')
            if e is not None and not e['valid'] and lua_dead is None:
                lua_dead = d['F']
    dead = death_frame(mal) if mal else None
    if dead is None and lua_dead is not None:
        dead = lua_dead
    res['dead'] = dead
    moved = None
    if mal and stun is not None:
        base = None
        for s in mal['st']:
            if s['F'] is None or s['F'] < stun:
                base = (s['x'], s['y'])
                continue
            if base is None:
                base = (s['x'], s['y'])
            if s['F'] > stun + 30 and base[0] is not None and math.hypot(s['x'] - base[0], s['y'] - base[1]) > 3.0:
                moved = s['F']
                break
    res['moved'] = moved
    if mal is None:
        L.append('MAL not found in memory (OBJ); Lua view only')
    L.append('MAL death: %s; Lua invalid from F=%s; first move after the stun: %s' % (
        ('F=%d (stun %+d)' % (dead, dead - stun)) if dead is not None and stun is not None else fmt(dead),
        fmt(lua_dead), ('F=%d (stun %+d)' % (moved, moved - stun)) if moved and stun is not None else fmt(moved)))
    if mal:
        drops = hp_drops(mal)
        if drops:
            L.append('MAL health drops: ' + ', '.join('F=%d -%s' % (f, a) for f, a, _h in drops[:8]))
    if stun is None:
        v = 'NO_STUN'
    elif dead is not None:
        dd = dead - stun
        v = 'DIES_AT_ONCE' if dd <= 6 else ('DIES_AT_STUN_END' if 585 <= dd <= 620 else 'DIES_AT_%d' % dd)
    else:
        v = 'SURVIVES'
    res['verdict'] = v
    L.append('verdict: %s' % v)
    return res


def analyse_clip_events(sc, objs, attacker_tag, clip_type, events_fn, res):
    a = objs.get(attacker_tag)
    L = res['lines']
    if a is None:
        L.append('%s not found in memory' % attacker_tag)
        return []
    segs = [s for s in segments(a) if s['type'] == clip_type and s['samples'] and
            sc['start'] <= s['samples'][0][0] <= (sc['end'] or 10 ** 9) + 30]
    if not segs:
        types = sorted({s['type'] for s in segments(a)})
        L.append('no %s clip (%d) seen on %s; clip types seen: %s; model offset: %s' % (
            ANIM_NAMES.get(clip_type, '?'), clip_type, attacker_tag, types, 'none' if not a['st'] or
            all(s['anim'] is None for s in a['st']) else 'found'))
    out = []
    for i, sg in enumerate(segs):
        st = sg['start']
        L.append('clip #%d %s: first sample F=%d pos=%.2f, slope %s clip frames / sim frame, start (extrapolated) '
                 'F=%s, end F=%s (%s, %s after start), last pos %.2f' % (
                     i + 1, ANIM_NAMES.get(clip_type), sg['samples'][0][0], sg['samples'][0][1], fmt(sg['slope'], 3),
                     fmt(st, 1), fmt(sg['end']), sg['end_why'] or '-',
                     fmt(None if sg['end'] is None or st is None else sg['end'] - st, 1), sg['samples'][-1][1]))
        out.append((sg, events_fn(sg)))
    return out


def port_first_frame(target, slope):
    """Port rule: the first update whose position >= target -> frames after the clip start."""
    if slope is None or slope <= 0:
        return None
    return int(math.ceil(target / slope - 1e-9))


def analyse_pound(sc, objs, battle):
    res = {'name': sc['name'], 'lines': [], 'clips': []}
    tank = objs.get('tank')

    def ev(sg):
        st = sg['start']
        drops = hp_drops(tank, int(st) if st is not None else None, (int(st) + 150) if st is not None else None) \
            if tank else []
        return drops
    for sg, drops in analyse_clip_events(sc, objs, 'rancor', POUND, ev, res):
        st = sg['start']
        pred = port_first_frame(20.0, sg['slope'])
        for F, amt, hp in drops[:2]:
            res['lines'].append('   tank damage F=%d (-%s, hp %s): %s frames after the clip start, clip pos %s; '
                                'port (pos >= 20 at slope %s): %s frames' % (
                                    F, amt, fmt(hp, 1), fmt(F - st if st is not None else None),
                                    fmt(pos_at(sg, F), 2), fmt(sg['slope'], 3), fmt(pred)))
        res['clips'].append({'start': st, 'slope': sg['slope'], 'damage': [d[0] for d in drops[:1]],
                             'end': sg['end'], 'pred': pred})
    return res


def analyse_eat(sc, objs, battle):
    res = {'name': sc['name'], 'lines': [], 'clips': []}
    troopers = [objs[t] for t in sorted(objs) if t.startswith('trooper')]

    def ev(sg):
        st = sg['start']
        out = []
        for tr in troopers:
            oc = [c for c in owner_changes(tr) if st is None or c[0] >= st - 2]
            dd = death_frame(tr)
            if oc or (dd is not None and st is not None and dd >= st):
                out.append((tr, oc[0][0] if oc else None, dd))
        return out
    for sg, evs in analyse_clip_events(sc, objs, 'rancor', EAT, ev, res):
        st = sg['start']
        for tr, att, dd in evs:
            if st is None:
                continue
            if att is not None and not (st - 2 <= att <= st + 400):
                continue
            res['lines'].append('   victim obj %d: re-owned (attach) F=%s = start %s, clip pos %s (port: pos >= 14 -> '
                                '%s); destroyed F=%s = start %s, clip pos %s (port: pos >= 110 -> %s)' % (
                                    tr['id'], fmt(att), fmt(None if att is None else att - st), fmt(pos_at(sg, att or -1), 2),
                                    fmt(port_first_frame(14.0, sg['slope'])), fmt(dd),
                                    fmt(None if dd is None else dd - st), fmt(pos_at(sg, dd or -1), 2),
                                    fmt(port_first_frame(110.0, sg['slope']))))
            res['clips'].append({'start': st, 'slope': sg['slope'], 'attach': att, 'destroy': dd, 'end': sg['end']})
            break
    return res


def analyse_grenade(sc, objs, battle):
    res = {'name': sc['name'], 'lines': [], 'clips': []}
    L = res['lines']
    for d in [d for k, d in sc['recs'] if k in ('ACT', 'FIRED')]:
        L.append('%s F=%s %s' % ('ACT' if 'arg' in d else 'FIRED', d['F'], ' '.join(
            '%s=%s' % (k, v) for k, v in d.items() if k in ('ab', 'arg', 'call', 'ready_after', 'active', 'ready'))))
    projs = sorted([o for o in battle['objs'].values() if o['type'] == 'PROJ_STICKY_BOMB' and o['frame'] is not None
                    and sc['start'] <= o['frame'] <= (sc['end'] or 10 ** 9)], key=lambda o: o['frame'])
    inf = objs.get('infiltrator')

    def ev(sg):
        return projs
    for sg, pr in analyse_clip_events(sc, objs, 'infiltrator', BOMBTOSS, ev, res):
        st = sg['start']
        p = next((o for o in pr if st is None or o['frame'] >= st - 2), None)
        resume = None
        if inf and sg['end'] is not None:
            for s in inf['st']:
                if s['F'] > sg['end'] and s['anim'] and s['anim'][0] != BOMBTOSS:
                    resume = (s['F'], s['anim'][0])
                    break
        count_est = (sg['samples'][-1][1] + 2.0) if sg['end_why'] == 'finished' else None
        L.append('   grenade projectile appears F=%s = start %s, clip pos %s (port: pos >= 12 -> %s frames)' % (
            fmt(p['frame'] if p else None), fmt(None if p is None or st is None else p['frame'] - st),
            fmt(pos_at(sg, p['frame']) if p else None, 2), fmt(port_first_frame(12.0, sg['slope']))))
        L.append('   toss clip ends F=%s = start %s (clip frame count ~ %s; port: ceil(max(count - 2, 0) / slope) = %s); '
                 'thrower next clip %s at F=%s = start %s' % (
                     fmt(sg['end']), fmt(None if sg['end'] is None or st is None else sg['end'] - st),
                     fmt(count_est, 0), fmt(port_first_frame(count_est - 2.0, sg['slope']) if count_est else None),
                     ANIM_NAMES.get(resume[1], resume[1]) if resume else '?', fmt(resume[0] if resume else None),
                     fmt(None if not resume or st is None else resume[0] - st)))
        res['clips'].append({'start': st, 'slope': sg['slope'], 'proj': p['frame'] if p else None, 'end': sg['end'],
                             'resume': resume[0] if resume else None})
    if not projs:
        L.append('no PROJ_STICKY_BOMB object seen in memory (Lua proj counts: %s)' % ', '.join(sorted({
            poll_tags(d.get('tags', '')).get('proj', '?') for k, d in sc['recs'] if k == 'P'})))
    return res


def berserker_model(n_max, slope, count, period=5):
    """Port model: passes at n (frames since the apply) with n % 5 == 0 (counter = 180 - n) and
    0.2 count < (n * slope) mod (count - 1) < 0.8 count."""
    out = []
    if slope is None or not count or count < 2:
        return out
    for n in range(1, n_max + 1):
        pos = math.fmod(n * slope, count - 1)
        if n % period == 0 and 0.2 * count < pos < 0.8 * count:
            out.append(n)
    return out


def analyse_berserker(sc, objs, battle):
    res = {'name': sc['name'], 'lines': [], 'passes': []}
    L = res['lines']
    acts = [d for k, d in sc['recs'] if k == 'ACT']
    fired = [d for k, d in sc['recs'] if k == 'FIRED']
    for d in acts:
        L.append('ACT F=%s Activate_Ability(BERSERKER, %s): call=%s ready %s -> %s active %s' % (
            d['F'], d.get('arg'), d.get('call'), d.get('ready_before'), d.get('ready_after'), d.get('active_after')))
    fired_f = fired[0]['F'] if fired else None
    L.append('FIRED (Lua, 3-frame resolution): %s' % fmt(fired_f))
    victims = [objs[t] for t in ('tank', 'tank2') if t in objs]
    passes = sorted({F for v in victims for F, _a, _h in hp_drops(v, sc['start'], sc['end'])})
    res['passes'] = passes
    yoda = objs.get('yoda')
    spin = None
    if yoda:
        segs = [s for s in segments(yoda) if s['type'] == SPIN and s['samples'] and
                sc['start'] <= s['samples'][0][0] <= (sc['end'] or 10 ** 9)]
        spin = segs[0] if segs else None
    apply_f = spin['start'] if spin and spin['start'] is not None else fired_f
    res['apply'] = apply_f
    if spin:
        loop = max((w[1] for w in spin['wraps']), default=None)
        # last sample before a wrap is < count - 1 by at most one step (slope): count - 1 ~ last + slope
        count = (int(round(loop + (spin["slope"] or 0.0))) + 1) if loop is not None else None
        L.append('SPINMOVE clip: start (extrapolated) F=%s, slope %s, wraps at F=%s, loop length ~%s -> count ~%s' % (
            fmt(spin['start']), fmt(spin['slope'], 3), [w[0] for w in spin['wraps']][:6], fmt(loop, 2), fmt(count)))
    else:
        count = None
        L.append('no SPINMOVE clip (104) seen on Yoda (model offset missing?): pass frames relative to FIRED only')
    for v in victims:
        dr = hp_drops(v, sc['start'], sc['end'])
        L.append('victim %s obj %d: %d health drops: %s' % (v['type'], v['id'], len(dr), ', '.join(
            'F=%d -%s' % (f, a) for f, a, _h in dr[:24])))
    if apply_f is not None and passes:
        rel = [round(F - apply_f, 1) for F in passes]
        L.append('damage passes, frames after the apply (%s): %s' % ('SPINMOVE start' if spin else 'FIRED', rel))
        gaps = [passes[i + 1] - passes[i] for i in range(len(passes) - 1)]
        L.append('gaps between passes: %s (port: multiples of 5)' % gaps)
        if spin:
            fr = []
            for F in passes:
                p = pos_at(spin, F)
                fr.append('%s(%s)' % (fmt(p, 1), fmt(None if p is None or not count else p / count, 2)))
            L.append('clip position (fraction of the clip) at each pass: %s (port window: 0.20 < f < 0.80)' % ', '.join(fr))
            pred = berserker_model(int(30 * 6), spin['slope'], count)
            L.append('port model passes (n since the apply, counter %% 5 = 0, window 20-80 %%): %s' % pred[:40])
            res['pred'] = pred
            meas = [int(round(F - apply_f)) for F in passes]
            res['match'] = meas == pred[:len(meas)] and len(meas) > 0
            L.append('=> measured %s the port model' % ('MATCHES' if res['match'] else 'DIFFERS FROM'))
    elif not passes:
        L.append('no damage pass seen (no victim health drop)')
    return res


ANALYSERS = {'MAL_LAND': analyse_mal, 'MAL_SHALLOW': analyse_mal, 'MAL_DEEP': analyse_mal, 'RANCOR_POUND': analyse_pound,
             'RANCOR_EAT': analyse_eat, 'GRENADE': analyse_grenade, 'BERSERKER': analyse_berserker}


def water_summary(mals):
    v = {r['name']: r.get('verdict') for r in mals}
    land, sh, dp = v.get('MAL_LAND'), v.get('MAL_SHALLOW'), v.get('MAL_DEEP')
    if not mals:
        return 'no MAL scenario ran'
    if 'NO_STUN' in (land, sh, dp):
        return 'INCOMPLETE: no stun seen in %s (see ACT / APIERR lines); the answer needs a stunned MAL' % ', '.join(
            k for k, x in v.items() if x == 'NO_STUN')
    if land == 'SURVIVES' and sh == 'DIES_AT_ONCE' and dp == 'DIES_AT_ONCE':
        return ('port guess CONFIRMED: a stunned MAL over water dies when the stun starts, at any depth, and survives '
                'on land (ABILITIES_HARDPOINTS §4.12 rule unchanged)')
    if land == 'SURVIVES' and dp == 'DIES_AT_ONCE' and sh == 'SURVIVES':
        return ('ONLY DEEP WATER kills: the port must add a depth test (threshold between the shallow and deep '
                'depths above)')
    if land == 'SURVIVES' and 'DIES_AT_STUN_END' in (sh, dp):
        return 'death at the STUN END over water: the port must defer Die to the stun expiry (and keep the depth rule seen)'
    if land == 'SURVIVES' and sh == 'SURVIVES' and dp == 'SURVIVES':
        return 'NEVER dies: the tag has no visible effect with this stun source; port: drop the rule (or probe another source)'
    if land and land.startswith('DIES'):
        return 'the LAND MAL died too (%s): the stun or something else kills it independent of water; check the log' % land
    return 'mixed result: land %s, shallow %s, deep %s (see the scenario blocks)' % (land, sh, dp)


# ---- report --------------------------------------------------------------------------------------------------------

def parse(path):
    battles, recs, meta = read_log(path)
    runs = {}
    for n, kind, d, text in recs:
        runs.setdefault(d.get('r', '?'), []).append((n, kind, d, text))
    out = {'runs': [], 'meta': meta, 'lines': []}
    L = out['lines']
    L.append('OI ability probe report (%s)' % os.path.basename(path))
    g = meta['game'] or {}
    L.append('game: pid=%s identity=%s; heap scans %d; Lua records %d; probe source text in memory: %s' % (
        g.get('pid', '?'), g.get('identity', '?'), meta['scans'], len(recs), 'yes' if meta['src'] else 'no'))
    for m in meta['modeloff']:
        L.append('model pointer search: %s' % ' '.join('%s=%s' % kv for kv in m.items()))
    loads = [d for _n, k, d, _t in recs if k == 'LOAD']
    skips = [d for _n, k, d, _t in recs if k == 'SKIP']
    L.append('LOAD records: %d; SKIP (other AI players / modes): %s' % (len(loads), ', '.join(
        '%s/%s' % (d.get('fac'), d.get('mode')) for d in skips[:8]) or '-'))
    for rid, rr in runs.items():
        arm = next((d for _n, k, d, _t in rr if k == 'ARM'), None)
        if arm is None:
            continue
        n = rr[0][0]
        battle_n = max((b for b in battles if battles[b]['type'] == 1 and b <= max(x[0] for x in rr)), default=None)
        battle = battles.get(battle_n, {'objs': {}, 'players': {}})
        run = {'rid': rid, 'battle': battle_n, 'scenarios': []}
        out['runs'].append(run)
        L.append('')
        L.append('== run %s (land battle %s): AI %s (player %s), map %s, points_ok=%s, %s ==' % (
            rid, battle_n, arm.get('fac'), arm.get('pid'), arm.get('map'), arm.get('points_ok'), arm.get('service_rate')))
        L.append('APIs: Create_Thread %s, Create_Position %s, Spawn_Unit %s' % (
            arm.get('create_thread'), arm.get('create_position'), arm.get('spawn_unit')))
        ticks = [d['F'] for _n, k, d, _t in rr if k == 'TICK' and d['F'] is not None]
        L.append('script resumes (first %d): F=%s -> gaps %s (3 = every free-store service; 60+ = the ServiceRate '
                 'change did not take and the On_Unit_Service fallback drove the probe)' % (
                     len(ticks), ticks, [ticks[i + 1] - ticks[i] for i in range(len(ticks) - 1)]))
        vic = next((d for _n, k, d, _t in rr if k == 'VICTIMS'), {})
        L.append('victims owned by %s (player %s)' % (vic.get('faction', '?'), vic.get('id', '?')))
        for _n, k, d, _t in rr:
            if k in ('APIERR', 'ERR'):
                L.append('%s F=%s scn=%s %s' % (k, d['F'], d.get('scn'), ' '.join(
                    '%s=%s' % (a, b) for a, b in d.items() if a in ('api', 'err', 'msg', 'step'))))
        done = next((d for _n, k, d, _t in rr if k == 'DONE'), None)
        L.append('DONE: %s' % ('F=%s scenarios=%s' % (done['F'], done.get('scenarios')) if done else
                               'not reached (game quit early?)'))
        keep = {}
        mals = []
        for sc in scenarios_of(rr):
            spawns = [dict(d) for k, d in sc['recs'] if k == 'SPAWN']
            sc['inherit'] = keep
            objs = bind_spawns(battle, sc, spawns)
            if sc['name'] == 'RANCOR_POUND' and 'rancor' in objs:
                keep = {'rancor': objs['rancor']}
            L.append('')
            L.append('-- %s: F=%s..%s (%s)' % (sc['name'], sc['start'], fmt(sc['end']), sc['point']))
            for sp in spawns:
                o = objs.get(sp['tag'])
                L.append('   spawn %-11s %-20s ok=%s %s owner=%s at (%s, %s) -> memory obj %s' % (
                    sp['tag'], sp.get('type'), sp.get('ok'), sp.get('how', sp.get('why', '')), sp.get('owner', '?'),
                    sp.get('x', '?'), sp.get('y', '?'), o['id'] if o else '-'))
            fn = ANALYSERS.get(sc['name'])
            if fn is None:
                continue
            r = fn(sc, objs, battle)
            r['start'], r['end'] = sc['start'], sc['end']
            run['scenarios'].append(r)
            for line in r['lines']:
                L.append('   ' + line)
            if sc['name'].startswith('MAL_'):
                mals.append(r)
        L.append('')
        L.append('== §4.12 Destroy_When_Stunned_Over_Water ==')
        for r in mals:
            L.append('   %-12s depth %-6s stun F=%-6s death %-8s moved %-8s -> %s' % (
                r['name'], fmt(r.get('depth'), 1), fmt(r.get('stun')),
                fmt(None if r.get('dead') is None or r.get('stun') is None else r['dead'] - r['stun']),
                fmt(None if r.get('moved') is None or r.get('stun') is None else r['moved'] - r['stun']), r['verdict']))
        run['water'] = water_summary(mals)
        L.append('=> ' + run['water'])
    if not out['runs']:
        L.append('')
        L.append('NO ARMED RUN: no ARM record (the AI player was not %s in a land battle, the hook did not load, '
                 'or the records were not found: see SCAN / LUA lines)' % 'the configured faction')
    return out


def cmd_parse(a):
    res = parse(a.log)
    text = '\n'.join(res['lines']) + '\n'
    if a.out:
        with open(a.out, 'w') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if getattr(a, 'json', None):
        with open(a.json, 'w') as f:
            json.dump({'runs': res['runs']}, f, indent=1, default=str)
    return res


# ---- selftest ------------------------------------------------------------------------------------------------------

class Synth:
    """A synthetic reader log that follows the port model with known numbers."""

    def __init__(self):
        self.lines = ['OIREADER 1 scan_sec=5 points=9', 'GAME pid=1 base=0x0 identity=test',
                      'MODE n=1 ptr=0x1 type=1 game_type=3 stack=1 frame=0 gframe=100',
                      'PL n=1 id=0 faction=REBEL human=0', 'PL n=1 id=4 faction=HOSTILE human=0',
                      'MODELOFF n=1 frame=460 how=d1 off=0x2c8 off2=- objs=2 score=8']
        self.recs = []
        self.next_id = 9000
        self.objs = {}

    def rec(self, kind, F, body):
        self.recs.append('OIP1|%s|r=abc123.1|f=%d|%s|END' % (kind, F, body))

    def obj(self, F, typ, role, x, y, hp, owner):
        self.next_id += 1
        oid = self.next_id
        self.objs[oid] = {'last': None}
        self.lines.append('OBJ n=1 frame=%d obj=%d addr=0x1 type=%s owner=%d x=%.1f y=%.1f z=0.0 hp=%.2f role=%s' % (
            F, oid, typ, owner, x, y, hp, role))
        return oid

    def st(self, F, oid, owner, hp, x, y, anim='-', flags=0):
        self.lines.append('ST n=1 frame=%d obj=%d owner=%d flags=%s hp=%.2f x=%.1f y=%.1f z=0.0 anim=%s' % (
            F, oid, owner, hex(flags), hp, x, y, anim))

    def gone(self, F, oid):
        self.lines.append('GONE n=1 frame=%d obj=%d type=X' % (F, oid))

    def spawn(self, F, scn, tag, typ, x, y, owner):
        self.rec('SPAWN', F, 'scn=%s|tag=%s|type=%s|ok=1|how=cfg|owner=%d|x=%.1f|y=%.1f|z=0.0' % (
            scn, tag, typ, owner, x, y))

    def text(self):
        out = list(self.lines)
        out += ['LUA n=1 frame=9999 addr=0x10 rec=%s' % r for r in self.recs]
        out += ['SCAN n=1 frame=9999 why=mode_end regions=1 mb=1 unreadable=0 sec=0.1 records=%d' % len(self.recs),
                'MODE_END n=1 last_frame=9999', 'EXIT reason=game_exited modes=1']
        return '\n'.join(out) + '\n'


def synth_log():
    """Expected values -> (log text, truth dict)."""
    s = Synth()
    truth = {}
    s.rec('LOAD', 0, 'map=_MP_LAND_DAGOBAH|faction=REBEL|points_ok=true')
    s.rec('ARM', 0, 'pid=0|fac=REBEL|mode=Land|map=_MP_LAND_DAGOBAH|points_ok=true|service_rate=20->0.05|'
                    'create_thread=function|create_position=function|spawn_unit=function')
    for i in range(12):
        s.rec('TICK', 3 + 3 * i, 'n=%d|src=fss' % (i + 1))
    s.rec('VICTIMS', 450, 'faction=Hostile|id=4')
    F = 450
    # --- three MAL scenarios: land survives (moves at stun + 601), shallow and deep die 1 frame after the stun
    for name, depth, dies in (('MAL_LAND', -15.0, False), ('MAL_SHALLOW', 3.81, True), ('MAL_DEEP', 46.0, True)):
        x, y = 1000.0 + F, 2000.0
        s.rec('SCN', F, 'scn=%s|ev=start|i=1|dur=900|point=p|x=%d|y=%d' % (name, x, y))
        s.spawn(F, name, 'mal', 'MAL_Rocket_Vehicle', x, y, 4)
        mal = s.obj(F, 'MAL_ROCKET_VEHICLE', 'victim', x, y, 150.0, 4)
        s.rec('WATER', F, 'scn=%s|depth_cfg=%.2f|water_height=0.00' % (name, depth))
        s.spawn(F + 3, name, 'han', 'Han_Solo', x + 80, y, 0)
        s.obj(F + 3, 'HAN_SOLO', 'attacker', x + 80, y, 400.0, 0)
        s.rec('ACT', F + 45, 'scn=%s|tag=han|ab=AREA_EFFECT_STUN|arg=position|call=ok|ready_before=1|ready_after=0|'
                             'active_after=0' % name)
        stun = F + 46
        s.obj(stun, 'PROJ_SPECIAL_HAN_SOLO_STUN_BLAST', 'projectile', x + 10, y, 1.0, 0)
        s.rec('FIRED', F + 48, 'scn=%s|tag=han|ab=AREA_EFFECT_STUN|active=0|ready=0' % name)
        s.st(F + 1, mal, 4, 150.0, x, y)
        if dies:
            s.st(stun + 1, mal, 4, 0.0, x, y, flags=0x40)
            s.gone(stun + 40, mal)
            truth[name] = ('DIES_AT_ONCE', 1)
        else:
            s.st(stun + 601, mal, 4, 150.0, x + 5, y)
            truth[name] = ('SURVIVES', 601)
        s.rec('SCN', F + 900, 'scn=%s|ev=end|why=time|despawned=2|kept=' % name)
        F += 903
    # --- Rancor pound: 30 fps clip at speedup 0.75 -> slope 0.75; damage at the first frame with pos >= 20 (n = 27)
    ax, ay = 3800.0, 1080.0
    s.rec('SCN', F, 'scn=RANCOR_POUND|ev=start|i=4|dur=600|point=arena1|x=3800|y=1080')
    s.spawn(F, 'RANCOR_POUND', 'rancor', 'Rancor', ax, ay, 0)
    rancor = s.obj(F, 'RANCOR', 'attacker', ax, ay, 1100.0, 0)
    s.spawn(F, 'RANCOR_POUND', 'tank', 'T4B_Tank', ax + 40, ay, 4)
    tank = s.obj(F, 'T4B_TANK', 'victim', ax + 40, ay, 500.0, 4)
    S = F + 60
    for n in range(0, 60):
        s.st(S + n, rancor, 0, 1100.0, ax, ay, anim='%d:0:%.3f:0.750' % (POUND, min(n * 0.75, 58.0)))
    s.st(S + 59, rancor, 0, 1100.0, ax, ay, anim='%d:2:58.000:0.750' % POUND)
    s.st(S + 27, tank, 4, 100.0, ax + 40, ay)
    truth['pound'] = (S, 27)
    s.rec('SCN', F + 600, 'scn=RANCOR_POUND|ev=end|why=time|despawned=1|kept=rancor,')
    F += 603
    # --- Rancor eat: slope 1.0; attach (re-owned to neutral 3) at n = 14, destroyed at n = 110
    s.rec('SCN', F, 'scn=RANCOR_EAT|ev=start|i=5|dur=900|point=arena1|x=3800|y=1080')
    s.spawn(F, 'RANCOR_EAT', 'trooper1', 'Stormtrooper', ax, ay + 47, 4)
    tr = s.obj(F, 'STORMTROOPER', 'victim', ax, ay + 47, 40.0, 4)
    S = F + 20
    for n in range(0, 125):
        s.st(S + n, rancor, 0, 1100.0, ax, ay, anim='%d:0:%.3f:1.000' % (EAT, min(float(n), 123.0)))
    s.st(S + 14, tr, 3, 40.0, ax + 3, ay + 10)
    s.st(S + 110, tr, 3, 0.0, ax + 3, ay + 10, flags=0x40)
    truth['eat'] = (14, 110)
    s.rec('SCN', F + 900, 'scn=RANCOR_EAT|ev=end|why=time|despawned=1|kept=')
    F += 903
    # --- Grenade: slope 1.0; projectile at n = 12; clip of 30 frames finishes at pos 28 (n = 28); then Attack
    gx, gy = 1480.0, 760.0
    s.rec('SCN', F, 'scn=GRENADE|ev=start|i=6|dur=600|point=arena2|x=1480|y=760')
    s.spawn(F, 'GRENADE', 'infiltrator', 'Rebel_Infiltrator', gx, gy, 0)
    inf = s.obj(F, 'REBEL_INFILTRATOR', 'attacker', gx, gy, 70.0, 0)
    s.spawn(F, 'GRENADE', 'tank', 'T4B_Tank', gx + 55, gy, 4)
    s.obj(F, 'T4B_TANK', 'victim', gx + 55, gy, 500.0, 4)
    s.rec('ACT', F + 30, 'scn=GRENADE|tag=infiltrator|ab=STICKY_BOMB|arg=object|call=ok|ready_before=1|ready_after=1|'
                         'active_after=0')
    S = F + 33
    for n in range(0, 29):
        s.st(S + n, inf, 0, 70.0, gx, gy, anim='%d:%d:%.3f:1.000' % (BOMBTOSS, 2 if n == 28 else 0, float(n)))
    s.obj(S + 12, 'PROJ_STICKY_BOMB', 'projectile', gx + 20, gy, 1.0, 0)
    s.st(S + 31, inf, 0, 70.0, gx, gy, anim='%d:0:0.000:1.000' % ATTACK)
    truth['grenade'] = (12, 28, 31)
    s.rec('SCN', F + 600, 'scn=GRENADE|ev=end|why=time|despawned=2|kept=')
    F += 603
    # --- Berserker: SpinMove loop of 40 frames (wrap at 39), slope 1; passes at n % 5 == 0 and 8 < pos < 32
    bx, by = 3840.0, 1480.0
    s.rec('SCN', F, 'scn=BERSERKER|ev=start|i=7|dur=450|point=arena3|x=3840|y=1480')
    s.spawn(F, 'BERSERKER', 'yoda', 'Yoda', bx, by, 0)
    yoda = s.obj(F, 'YODA', 'attacker', bx, by, 600.0, 0)
    s.spawn(F, 'BERSERKER', 'tank', 'T4B_Tank', bx + 60, by, 4)
    t1 = s.obj(F, 'T4B_TANK', 'victim', bx + 60, by, 500.0, 4)
    s.rec('ACT', F + 30, 'scn=BERSERKER|tag=yoda|ab=BERSERKER|arg=object|call=ok|ready_before=1|ready_after=0|'
                         'active_after=1')
    S = F + 31
    s.rec('FIRED', F + 33, 'scn=BERSERKER|tag=yoda|ab=BERSERKER|active=1|ready=0')
    passes = berserker_model(180, 1.0, 40)
    hp = 500.0
    for n in range(0, 181):
        s.st(S + n, yoda, 0, 600.0, bx + min(n, 55), by, anim='%d:1:%.3f:1.000' % (SPIN, math.fmod(n, 39.0)))
        if n in passes:
            hp -= 35.0
            s.st(S + n, t1, 4, hp, bx + 60, by)
    truth['berserker'] = passes
    s.rec('SCN', F + 450, 'scn=BERSERKER|ev=end|why=time|despawned=3|kept=')
    s.rec('DONE', F + 453, 'scenarios=8|apierr=')
    return s.text(), truth


def selftest(keep=None):
    import tempfile
    fails = []

    def check(c, what):
        print(('  ok    ' if c else '  FAIL  ') + what)
        if not c:
            fails.append(what)

    text, truth = synth_log()
    d = keep or tempfile.mkdtemp(prefix='oi_parse_selftest_')
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, 'reader.log')
    with open(log, 'w') as f:
        f.write(text)
    res = cmd_parse(argparse.Namespace(log=log, out=os.path.join(d, 'report.txt'), json=os.path.join(d, 'report.json')))
    check(len(res['runs']) == 1, 'one armed run')
    run = res['runs'][0] if res['runs'] else {'scenarios': []}
    by = {r['name']: r for r in run['scenarios']}
    for name in ('MAL_LAND', 'MAL_SHALLOW', 'MAL_DEEP'):
        r = by.get(name, {})
        exp, dd = truth[name]
        got = (r.get('dead') - r.get('stun')) if r.get('dead') is not None and r.get('stun') is not None else \
            ((r.get('moved') - r.get('stun')) if r.get('moved') is not None and r.get('stun') is not None else None)
        check(r.get('verdict') == exp and got == dd, '%s verdict %s, delta %s (want %s, %s)' % (
            name, r.get('verdict'), got, exp, dd))
    check('CONFIRMED' in run.get('water', ''), 'water summary: %s' % run.get('water'))
    pc = by.get('RANCOR_POUND', {}).get('clips', [])
    S, n = truth['pound']
    check(bool(pc) and pc[0]['start'] is not None and abs(pc[0]['start'] - S) < 0.01 and pc[0]['damage'] and
          pc[0]['damage'][0] - S == n and pc[0]['pred'] == n, 'pound: start %s, damage at +%s, port %s (want %d, +%d)' % (
              pc[0]['start'] if pc else None, (pc[0]['damage'][0] - pc[0]['start']) if pc and pc[0]['damage'] else None,
              pc[0]['pred'] if pc else None, S, n))
    ec = by.get('RANCOR_EAT', {}).get('clips', [])
    check(bool(ec) and ec[0]['attach'] - ec[0]['start'] == truth['eat'][0] and
          ec[0]['destroy'] - ec[0]['start'] == truth['eat'][1], 'eat: attach / destroy at +%s / +%s (want +%d / +%d)' % (
              (ec[0]['attach'] - ec[0]['start']) if ec else None, (ec[0]['destroy'] - ec[0]['start']) if ec else None,
              truth['eat'][0], truth['eat'][1]))
    gc = by.get('GRENADE', {}).get('clips', [])
    want = truth['grenade']
    check(bool(gc) and (gc[0]['proj'] - gc[0]['start'], gc[0]['end'] - gc[0]['start'], gc[0]['resume'] - gc[0]['start'])
          == want, 'grenade: projectile / clip end / resume at %s (want %s)' % (
              (gc[0]['proj'] - gc[0]['start'], gc[0]['end'] - gc[0]['start'], gc[0]['resume'] - gc[0]['start'])
              if gc else None, want))
    br = by.get('BERSERKER', {})
    meas = [int(round(F - br['apply'])) for F in br.get('passes', [])] if br.get('apply') is not None else []
    check(meas == truth['berserker'] and br.get('match') is True, 'berserker: %d passes %s.. match=%s' % (
        len(meas), meas[:5], br.get('match')))
    report = open(os.path.join(d, 'report.txt')).read()
    check('§4.12' in report and 'BERSERKER' in report, 'report text written (%d lines)' % report.count('\n'))
    print('report: %s' % os.path.join(d, 'report.txt'))
    print('PARSE SELFTEST %s' % ('OK' if not fails else 'FAILED'))
    return 0 if not fails else 1


def main():
    if '--selftest' in sys.argv:
        ap = argparse.ArgumentParser()
        ap.add_argument('--selftest', action='store_true')
        ap.add_argument('--keep')
        a = ap.parse_args()
        return selftest(a.keep)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('parse')
    p.add_argument('log')
    p.add_argument('--out')
    p.add_argument('--json')
    a = ap.parse_args()
    cmd_parse(a)
    return 0


if __name__ == '__main__':
    sys.exit(main())
