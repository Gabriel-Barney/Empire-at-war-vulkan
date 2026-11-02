#!/usr/bin/env python3
"""s7s8_parse.py - turns an s7s8_reader.py log into the S7/S8 probe report.

  s7s8_parse.py parse <reader.log> [--out report.txt] [--json report.json]
  s7s8_parse.py --selftest [--keep DIR]     synthetic reader log (port-model numbers) -> parse -> check the verdicts

Inputs per battle (reader MODE n; type 1 land, 2 space): the probe's Lua records (LUA lines, "S78P1|KIND|...|END";
f = mode frame) and the memory trace of new objects (OBJ / ST / GONE: owner, flags, health, position, animation slot
type:mode:position:speed, locomotor state, current target, targeting state, container, team target). SPAWN records
carry the object id (Get_Object_ID), which is matched to the memory object id first; type + frame + position second.

Answers (item numbers of the S7/S8 probe request; README.md has the expected values and the docs/re sections):
  1  SPMAT: grenade arc at ~275 (angle, flight frames, apex, speed) against the lob model (v 30, G 1.6364,
     Can_Lob_Slower: v *= 0.8 while solvable) from the measured positions; range edge: target at ~547 (scanned = the
     SPMAT's current target is that tank; fired = a grenade launched) and the control at ~530.
  2  HUMAN: Remote_Bomb ground markers (DUMMY_BARRAGE_TARGET), the Saboteur's BOMBTOSS (anim 47) clips, bombs.
  3  SPMAT / AT_AT / MDU: deploy windows from locomotor states (0x16 stopped, 0x18 deploying, 0x19 deployed, 0x1a
     undeploying) after each DEPLOY order, and the reversal pair against the mirror model.
  4  MUZZLE_LAND / MUZZLE_SPACE: shots per type (projectiles first seen next to the unit).
  5  MDU / STICKY (and the SPMAT DEPLOY calls): what Activate_Ability returned and when the block finished, against
     the locomotor state change, the target's death and the next move order.
  6  SQUAD: per-member targets, the shared team target, and how the squadron retargets after its target goes.
  7  ESCORT: escort fighters' distance from the carrier's death point, response to an intruder, return.
  8  REINF: spawn -> first shot of four MZ8 tanks (random first-shot delay or not); DT3: lasers / missiles per phase.
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

DYING = 0x40
BOMBTOSS = 47
LOCO = {0x16: 'stopped', 0x18: 'deploying', 0x19: 'deployed', 0x1a: 'undeploying'}
LOB_V0, LOB_G, Z_ADJUST = 30.0, 1.636363636, 6.0
GRENADE = 'PROJ_GROUND_SPMAT_GRENADE'
ATAT_LASER = 'PROJ_GROUND_LASER_CANNON_RED_ATAT'
DT3_LASER, DT3_MISSILE = 'PROJ_GROUND_LASER_CANNON_RED_DT3', 'PROJ_DARK_TROOPER_MISSILE'
MZ8_BLAST = 'PROJ_MOBILE_PULSE_CANNON_BLAST'
REMOTE_BOMB = 'PROJ_REMOTE_BOMB'
MARKER = 'DUMMY_BARRAGE_TARGET'
ESCORT_SQUADS = ('TIE_INTERCEPTOR_SQUADRON', 'TIE_BOMBER_SQUADRON', 'TIE_FIGHTER_SQUADRON')
ESCORT_FIGHTERS = ('TIE_INTERCEPTOR', 'TIE_BOMBER', 'TIE_FIGHTER')
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
    """S78P1|KIND|r=..|f=..|k=v|...|END -> (kind, fields); a P record keeps its tag list in fields['tags']."""
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
    d['F'] = _int(d.get('f'))
    return kind, d


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
    records = {}
    order = []
    meta = {'modeloff': [], 'teamdata': [], 'scans': 0, 'src': False, 'exit': None, 'game': None}

    def battle(n):
        return battles.setdefault(n, {'n': n, 'type': None, 'objs': {}, 'players': {}, 'last': None})

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
                if text.startswith('S78P1|') and text not in records:
                    records[text] = _int(d.get('n'), 0)
                    order.append(text)
                continue
            d = kvs(rest)
            n = _int(d.get('n'))
            if kind == 'MODE':
                b = battle(n)
                b['type'] = _int(d.get('type'))
            elif kind == 'PL':
                battle(n)['players'][_int(d.get('id'))] = d.get('faction')
            elif kind == 'OBJ':
                b = battle(n)
                oid = _int(d.get('obj'))
                b['objs'][oid] = {'id': oid, 'type': d.get('type'), 'role': d.get('role'), 'frame': _int(d.get('frame')),
                                  'x': _float(d.get('x')), 'y': _float(d.get('y')), 'z': _float(d.get('z')),
                                  'hp0': _float(d.get('hp')), 'owner0': _int(d.get('owner')), 'st': [], 'gone': None}
            elif kind == 'ST':
                b = battle(n)
                o = b['objs'].get(_int(d.get('obj')))
                F = _int(d.get('frame'))
                if o is None or F is None:
                    continue
                an = d.get('anim', '-')
                a = None
                if an not in ('-', 'none'):
                    p = an.split(':')
                    if len(p) == 4:
                        a = (int(p[0]), int(p[1]), float(p[2]), float(p[3]))
                lo = d.get('loco', '-')
                o['st'].append({'F': F, 'owner': _int(d.get('owner')), 'flags': _int(d.get('flags'), 0),
                                'hp': _float(d.get('hp')), 'x': _float(d.get('x')), 'y': _float(d.get('y')),
                                'z': _float(d.get('z')), 'anim': a, 'loco': _int(lo) if lo != '-' else None,
                                'tgt': d.get('tgt', '-'), 'ts': _int(d.get('ts')), 'cont': d.get('cont', '-'),
                                'tt': d.get('tt', '-')})
                b['last'] = max(b['last'] or 0, F)
            elif kind == 'GONE':
                b = battle(n)
                o = b['objs'].get(_int(d.get('obj')))
                if o is not None and o['gone'] is None:
                    o['gone'] = _int(d.get('frame'))
            elif kind == 'MODE_END':
                b = battle(n)
                lf = _int(d.get('last_frame'))
                if lf is not None:
                    b['last'] = max(b['last'] or 0, lf)
            elif kind in ('MODELOFF', 'MODELOFF_FAIL'):
                meta['modeloff'].append(dict(d, kind=kind))
            elif kind == 'TEAMDATA':
                meta['teamdata'].append(d)
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


# ---- object helpers ------------------------------------------------------------------------------------------------

def samples(o):
    """[(F, x, y, z)], one per frame (the last one seen in that frame)."""
    pts = {}
    if o.get('frame') is not None and o.get('x') is not None:
        pts[o['frame']] = (o['x'], o['y'], o['z'] or 0.0)
    for s in o['st']:
        if s['x'] is not None:
            pts[s['F']] = (s['x'], s['y'], s['z'] or 0.0)
    return [(F,) + pts[F] for F in sorted(pts)]


def pos_at(o, F):
    best = None
    for s in samples(o):
        if best is None or s[0] <= F:
            best = s
        if s[0] > F:
            break
    return best


def dist2(a, b):
    return math.hypot(a[1] - b[1], a[2] - b[2])


def runs(o, key, F0=None, F1=None):
    """[[value, first frame, next change frame or None]] of one ST field, frames F0..F1."""
    out = []
    for s in o['st']:
        if (F0 is not None and s['F'] < F0) or (F1 is not None and s['F'] > F1):
            continue
        v = s[key]
        if not out or out[-1][0] != v:
            if out:
                out[-1][2] = s['F']
            out.append([v, s['F'], None])
    return out


def value_at(o, key, F):
    v = None
    for s in o['st']:
        if s['F'] > F:
            break
        v = s[key]
    return v


def first_where(o, key, pred, F0, F1=None):
    for s in o['st']:
        if s['F'] >= F0 and (F1 is None or s['F'] <= F1) and pred(s[key]):
            return s['F']
    return None


def death_frame(o):
    for s in o['st']:
        if (s['flags'] & DYING) or (s['hp'] is not None and s['hp'] <= 0.0):
            return s['F']
    return o['gone']


def readable(o, key):
    return o is not None and any(s[key] not in (None, '-') for s in o['st'])


def projectiles(battle, F0, F1, types=None):
    out = []
    for o in battle['objs'].values():
        if o['role'] != 'projectile' or o['frame'] is None or not (F0 <= o['frame'] <= F1):
            continue
        if types is None or o['type'] in types:
            out.append(o)
    return sorted(out, key=lambda o: (o['frame'], o['id']))


def nearest(proj, shooters, radius):
    """Tag of the shooter nearest to the projectile's first position (within radius), else None."""
    p = samples(proj)
    if not p:
        return None
    best = None
    for tag, so in shooters.items():
        if so is None or (so['frame'] is not None and so['frame'] > p[0][0]) or \
                (so['gone'] is not None and so['gone'] < p[0][0]):
            continue
        sp = pos_at(so, p[0][0])
        if sp is None:
            continue
        d = dist2(p[0], sp)
        if d <= radius and (best is None or d < best[0]):
            best = (d, tag)
    return best[1] if best else None


def fmt(v, nd=1):
    if v is None:
        return '?'
    if isinstance(v, float):
        return ('%.' + str(nd) + 'f') % v
    return str(v)


def loco_name(v):
    return LOCO.get(v, hex(v)) if v is not None else '?'


# ---- maths ---------------------------------------------------------------------------------------------------------

def lstsq(rows, ys):
    """Least squares for a few unknowns (normal equations, Gaussian elimination)."""
    n = len(rows[0])
    a = [[sum(r[i] * r[j] for r in rows) for j in range(n)] + [sum(r[i] * y for r, y in zip(rows, ys))]
         for i in range(n)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-12:
            return None
        a[c], a[p] = a[p], a[c]
        for r in range(n):
            if r != c:
                k = a[r][c] / a[c][c]
                a[r] = [x - k * y for x, y in zip(a[r], a[c])]
    return [a[i][n] / a[i][i] for i in range(n)]


def lob_solve(d, h, v):
    disc = v ** 4 - 2.0 * LOB_G * h * v * v - LOB_G * LOB_G * d * d
    if disc < 0 or d <= 0:
        return None
    th = math.atan((v * v + math.sqrt(disc)) / (LOB_G * d))
    return {'v': v, 'theta': math.degrees(th), 'T': d / (v * math.cos(th)),
            'apex': (v * math.sin(th)) ** 2 / (2.0 * LOB_G)}


def lob(d, h, v0=LOB_V0, slower=True):
    """The port model of slot 44's solver: high arc at v0; Can_Lob_Slower: v *= 0.8 while still solvable."""
    best = lob_solve(d, h, v0)
    if best is None or not slower:
        return best
    v = v0
    while True:
        s = lob_solve(d, h, v * 0.8)
        if s is None:
            return best
        v, best = v * 0.8, s


def arc_fit(o):
    """Measured flight of one grenade: horizontal speed, initial vertical speed, gravity, angle, frames, apex."""
    p = samples(o)
    if len(p) < 4:
        return None
    F0, x0, y0, z0 = p[0]
    ts = [s[0] - F0 for s in p]
    hs = [math.hypot(s[1] - x0, s[2] - y0) for s in p]
    zs = [s[3] - z0 for s in p]
    lin = lstsq([[1.0, t] for t in ts], hs)
    quad = lstsq([[1.0, t, t * t] for t in ts], zs)
    if lin is None or quad is None:
        return None
    vh, vz, g = lin[1], quad[1], -2.0 * quad[2]
    end = o['gone'] if o['gone'] is not None else p[-1][0]
    return {'F0': F0, 'vh': vh, 'vz': vz, 'g': g, 'theta': math.degrees(math.atan2(vz, vh)),
            'v': math.hypot(vh, vz), 'T': p[-1][0] - F0, 'gone': end, 'apex': max(zs), 'n': len(p),
            'land': p[-1]}


# ---- scenario binding ----------------------------------------------------------------------------------------------

def scenarios_of(recs):
    out = []
    cur = None
    for _n, kind, d, _t in recs:
        if kind == 'SCN' and d.get('ev') == 'start':
            cur = {'name': d.get('scn'), 'start': d['F'], 'end': None, 'recs': []}
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


def frame_offset(battle, spawns):
    """Memory frame - Lua frame from SPAWN ids that match memory objects; applied only when >= 3 (clock mismatch)."""
    offs = sorted(battle['objs'][_int(sp.get('id'))]['frame'] - sp['F'] for sp in spawns
                  if sp.get('ok') == '1' and _int(sp.get('id')) in battle['objs'] and sp['F'] is not None and
                  battle['objs'][_int(sp.get('id'))]['frame'] is not None)
    if not offs:
        return 0, 0
    med = offs[len(offs) // 2]
    return (med if abs(med) >= 3 else 0), len(offs)


def bind_spawns(battle, spawns):
    out = {}
    used = set()
    for sp in spawns:
        if sp.get('ok') != '1':
            continue
        o = battle['objs'].get(_int(sp.get('id')))
        if o is not None and o['type'] == sp.get('type', '').upper() and o['id'] not in used:
            used.add(o['id'])
            out[sp['tag']] = o
            continue
        t = sp.get('type', '').upper()
        x, y = _float(sp.get('x')), _float(sp.get('y'))
        best = None
        for o in battle['objs'].values():
            if o['type'] != t or o['id'] in used or o['frame'] is None or sp['F'] is None:
                continue
            if abs(o['frame'] - sp['F']) > 15:
                continue
            d = math.hypot(o['x'] - x, o['y'] - y) if x is not None and o['x'] is not None else 0.0
            if best is None or d < best[0]:
                best = (d, o)
        if best is not None:
            used.add(best[1]['id'])
            out[sp['tag']] = best[1]
    return out


class Scn:
    """One scenario's records and bound objects."""

    def __init__(self, sc, battle):
        self.sc, self.b = sc, battle
        self.name = sc['name']
        self.start = sc['start']
        self.end = sc['end'] if sc['end'] is not None else (battle.get('last') or sc['start'])
        self.spawns = [dict(d) for k, d in sc['recs'] if k == 'SPAWN']
        self.objs = bind_spawns(battle, self.spawns)
        self.acts = {d.get('blk'): d for k, d in sc['recs'] if k == 'ACT'}
        self.blks = {}
        for k, d in sc['recs']:
            if k == 'BLK':
                self.blks.setdefault(d.get('blk'), d)
        self.marks = {}
        for k, d in sc['recs']:
            if k == 'MARK':
                self.marks.setdefault(d.get('mark'), d)
        self.orders = [d for k, d in sc['recs'] if k == 'ORDER']
        self.L = []

    def order(self, tag, what):
        return next((d for d in self.orders if d.get('tag') == tag and d.get('order') == what), None)

    def obj(self, tag):
        return self.objs.get(tag)

    def spawn_lines(self):
        for sp in self.spawns:
            o = self.objs.get(sp['tag'])
            self.L.append('spawn %-9s %-40s ok=%s %s at (%s, %s, %s) -> memory obj %s' % (
                sp['tag'], sp.get('type'), sp.get('ok'), sp.get('how', sp.get('why', '')), sp.get('x', '?'),
                sp.get('y', '?'), sp.get('z', '?'), o['id'] if o else '-'))

    def block_line(self, blk, extra=''):
        a = self.acts.get(blk)
        b = self.blks.get(blk)
        if a is None:
            self.L.append('%-14s no ACT record' % blk)
            return None
        fin = b['F'] if b else None
        self.L.append('%-14s F=%-6s %s(%s) call=%s ret=%s ready %s->%s active %s; block finished %s%s%s' % (
            blk, a['F'], a.get('ab'), a.get('arg'), a.get('call'), a.get('ret', a.get('call')), a.get('ready_before'),
            a.get('ready_after'), a.get('active_after'),
            ('F=%s (+%s) ev=%s res=%s' % (fin, fin - a['F'], b.get('ev'), b.get('res'))) if b and fin is not None and
            a['F'] is not None else 'never' if a.get('ret') == 'block' else '-', ' ' if extra else '', extra))
        return {'F': a['F'], 'ret': a.get('ret'), 'finish': fin, 'res': b.get('res') if b else None}


# ---- deploy windows (item 3) ---------------------------------------------------------------------------------------

def deploy_sequence(s, tag, blk_names):
    """Locomotor state runs after each DEPLOY order of one unit: order -> first state change -> next state."""
    o = s.obj(tag)
    res = {'readable': readable(o, 'loco'), 'orders': {}}
    if not res['readable']:
        s.L.append('locomotor state of %s: UNREADABLE (no loco samples; see TEAMDATA / ST lines)' % tag)
        return res
    rr = runs(o, 'loco', s.start, s.end)
    s.L.append('loco runs of %s: %s' % (tag, ', '.join('%s@%s' % (loco_name(v), F) for v, F, _e in rr)))
    order_frames = sorted(a['F'] for b, a in s.acts.items() if a.get('tag') == tag and a['F'] is not None)
    for blk in blk_names:
        a = s.acts.get(blk)
        if a is None or a['F'] is None:
            continue
        F = a['F']
        later = [x for x in order_frames if x > F]
        F1 = later[0] if later else None   # a state change after the unit's next order belongs to that order
        before = value_at(o, 'loco', F)
        chg = first_where(o, 'loco', lambda v, b=before: v is not None and v != b, F, F1)
        new = value_at(o, 'loco', chg) if chg is not None else None
        nxt = first_where(o, 'loco', lambda v, n=new: v is not None and v != n, chg) if chg is not None else None
        fin = value_at(o, 'loco', nxt) if nxt is not None else None
        r = {'order': F, 'before': before, 'change': chg, 'state': new, 'next': nxt, 'final': fin}
        res['orders'][blk] = r
        s.L.append('  %-12s order F=%s in %s -> %s at +%s -> %s at +%s (state lasted %s)' % (
            blk, F, loco_name(before), loco_name(new), fmt(None if chg is None else chg - F), loco_name(fin),
            fmt(None if nxt is None else nxt - F), fmt(None if nxt is None or chg is None else nxt - chg)))
    return res


def reversal_check(s, seq, d2, u2, d3, full_undeploy):
    """Mirror model (Deploy(false) from 0x18 restarts the undeploy clip at len - cur - 1 and the reverse): undeploy2
    would last e + 1 (e = frames spent deploying); deploy3 lasts L_undeploy - e + e2 (e2 = frames spent undeploying)."""
    o = seq['orders']
    a, b, c = o.get(d2), o.get(u2), o.get(d3)
    if not (a and b and c) or None in (a['change'], b['change'], c['change']):
        s.L.append('reversal: not measurable (missing state changes)')
        return {'verdict': 'NOT_MEASURED'}
    e = b['change'] - a['change']
    e2 = c['change'] - b['change']
    u_done = b['state'] == 0x1a and b['final'] == 0x16 and b['next'] is not None and b['next'] <= c['order']
    d3_len = (c['next'] - c['change']) if c['next'] is not None and c['final'] == 0x19 else None
    res = {'e': e, 'e2': e2, 'deploy3': d3_len, 'L_undeploy': full_undeploy}
    if u_done:
        u_len = b['next'] - b['change']
        res['undeploy2'] = u_len
        ok = abs(u_len - (e + 1)) <= 3
        s.L.append('reversal: deploying %d frames, then undeploy took %d (mirror model %d) -> %s' % (
            e, u_len, e + 1, 'MIRRORS' if ok else 'DIFFERS'))
        res['verdict'] = 'MIRRORS' if ok else 'DIFFERS'
        return res
    if full_undeploy is None or d3_len is None:
        s.L.append('reversal: deploying %d, undeploying %d, deploy3 %s frames; no full undeploy to compare' % (
            e, e2, fmt(d3_len)))
        res['verdict'] = 'NOT_MEASURED'
        return res
    pred = full_undeploy - e + e2
    ok = abs(d3_len - pred) <= 3
    s.L.append('reversal: deploying %d, undeploying %d, then deploy took %d (mirror model L_u - e + e2 = %d - %d + %d '
               '= %d) -> %s' % (e, e2, d3_len, full_undeploy, e, e2, pred, 'MIRRORS' if ok else 'DIFFERS'))
    res['verdict'] = 'MIRRORS' if ok else 'DIFFERS'
    res['pred'] = pred
    return res


# ---- scenario analysers --------------------------------------------------------------------------------------------

def an_spmat(s):
    r = {'name': s.name}
    sp, v275, v547, v530 = s.obj('spmat'), s.obj('v275'), s.obj('v547'), s.obj('v530')
    edge = s.marks.get('edge', {}).get('F')
    ctrl = s.marks.get('control', {}).get('F')
    und = s.acts.get('undeploy1', {}).get('F') or s.end
    nades = projectiles(s.b, s.start, s.end, (GRENADE,))
    r['grenades'] = len(nades)
    victims = {k: v for k, v in (('v275', v275), ('v547', v547), ('v530', v530)) if v is not None}
    for o in nades:
        f = arc_fit(o)
        o['fit'] = f
        land = f['land'] if f else (samples(o) or [None])[-1]
        o['aim'] = None
        if land is not None:
            best = min(((dist2(land, pos_at(v, land[0])), k) for k, v in victims.items() if pos_at(v, land[0])),
                       default=None)
            o['aim'] = best[1] if best and best[0] < 120 else None
    blasts = projectiles(s.b, s.start, s.end, ('PROJ_BLASTER_CANNON_GREEN',))
    s.L.append('grenades %d (at v275 %d, v547 %d, v530 %d, unattributed %d); blaster shots %d' % (
        len(nades), sum(o['aim'] == 'v275' for o in nades), sum(o['aim'] == 'v547' for o in nades),
        sum(o['aim'] == 'v530' for o in nades), sum(o['aim'] is None for o in nades), len(blasts)))
    # arc
    arcs = [o for o in nades if o['aim'] == 'v275' and o['fit']]
    if sp is None or v275 is None or not arcs:
        r['arc'] = 'NOT_MEASURED'
        s.L.append('ARC: NOT_MEASURED (%s)' % ('no grenade at v275' if sp and v275 else 'units not in memory'))
    else:
        f = arcs[0]['fit']
        a = pos_at(sp, f['F0'])
        t = pos_at(v275, f['F0'])
        d, h = dist2(a, t), (t[3] + Z_ADJUST) - a[3]
        pred, fast = lob(d, h), lob(d, h, slower=False)
        r.update({'arc_meas': f, 'arc_pred': pred, 'arc_d': d})
        ok = pred is not None and abs(f['theta'] - pred['theta']) <= 4.0 and abs(f['T'] - pred['T']) <= 3.0
        nf = fast is not None and abs(f['theta'] - fast['theta']) <= 4.0 and abs(f['T'] - fast['T']) <= 3.0
        r['arc'] = 'MATCHES_LOB_SLOWER' if ok else ('MATCHES_V30_NO_SLOWER' if nf else 'DIFFERS')
        s.L.append('ARC (first of %d grenades at v275): d=%.1f h=%.1f; measured angle %.1f deg, %d frames (%.2f s), '
                   'apex %.0f, speed %.1f, g %.3f; model (Can_Lob_Slower) angle %s, %s frames, apex %s, v %s; v30 '
                   'only: %s deg / %s frames -> %s' % (
                       len(arcs), d, h, f['theta'], f['T'], f['T'] / 30.0, f['apex'], f['v'], f['g'],
                       fmt(pred and pred['theta']), fmt(pred and pred['T']), fmt(pred and pred['apex'], 0),
                       fmt(pred and pred['v']), fmt(fast and fast['theta']), fmt(fast and fast['T']), r['arc']))
    # range edge
    if edge is None or v547 is None or sp is None:
        r['edge'] = 'NOT_MEASURED'
        s.L.append('EDGE: NOT_MEASURED (no edge mark / units)')
    else:
        w1 = ctrl if ctrl is not None else und
        a, t = pos_at(sp, edge + 30), pos_at(v547, edge + 30)
        d, h = dist2(a, t), (t[3] + Z_ADJUST) - a[3]
        acc = lob_solve(d, h, LOB_V0) is not None
        fired = [o for o in nades if edge <= o['frame'] < w1]
        scanned = None
        if readable(sp, 'tgt'):
            scanned = first_where(sp, 'tgt', lambda v: v == str(v547['id']), edge, w1) is not None
        tss = sorted({x for x in (s2['ts'] for s2 in sp['st'] if edge <= s2['F'] < w1) if x is not None})
        r.update({'edge_d': d, 'edge_h': h, 'edge_accept_model': acc, 'edge_fired': len(fired), 'edge_scanned': scanned,
                  'edge_ts': tss})
        if fired:
            r['edge'] = 'FIRED'
        elif scanned:
            r['edge'] = 'SCANNED_NOT_FIRED'
        elif scanned is False:
            r['edge'] = 'NOT_SCANNED'
        else:
            r['edge'] = 'NOT_FIRED_SCAN_UNKNOWN'
        ctrl_n = len([o for o in nades if ctrl is not None and ctrl <= o['frame'] < und])
        r['control_fired'] = ctrl_n
        s.L.append('EDGE: v547 at d=%.1f h=%.1f (model: %s at v 30); grenades %d in F=%s..%s; SPMAT target = v547: %s; '
                   'targeting states %s -> %s; control v530: %d grenades%s' % (
                       d, h, 'reachable' if acc else 'beyond reach', len(fired), edge, w1,
                       {True: 'yes', False: 'no', None: 'unreadable'}[scanned], tss or '-', r['edge'], ctrl_n,
                       '' if ctrl_n else ' (control did not fire: the edge result says little)'))
    # deploy windows and blocks
    seq = deploy_sequence(s, 'spmat', ('deploy1', 'undeploy1', 'deploy2', 'undeploy2', 'deploy3'))
    r['deploy'] = seq
    if seq['readable']:
        d1 = seq['orders'].get('deploy1')
        if d1 and d1['change'] is not None and d1['next'] is not None:
            first = next((o['frame'] for o in nades if o['frame'] >= d1['order']), None)
            r['window'] = d1['next'] - d1['order']
            s.L.append('DEPLOY WINDOW SPMAT: order -> deploying +%d, deploying lasted %d, order -> deployed +%d; first '
                       'grenade +%s' % (d1['change'] - d1['order'], d1['next'] - d1['change'], r['window'],
                                        fmt(None if first is None else first - d1['order'])))
        u1 = seq['orders'].get('undeploy1')
        full_u = (u1['next'] - u1['change']) if u1 and u1['next'] and u1['change'] and u1['state'] == 0x1a else None
        r['reversal'] = reversal_check(s, seq, 'deploy2', 'undeploy2', 'deploy3', full_u)
    r['blocks'] = {b: s.block_line(b) for b in ('deploy1', 'undeploy1', 'deploy2', 'undeploy2', 'deploy3')}
    return r


def shots_between(nades, cuts):
    """Counts of projectiles between consecutive (name, frame) cuts."""
    out = []
    for i, (name, F) in enumerate(cuts):
        F1 = cuts[i + 1][1] if i + 1 < len(cuts) else 10 ** 9
        out.append((name, F, sum(1 for o in nades if F is not None and F <= o['frame'] < F1)))
    return out


def an_atat(s):
    r = {'name': s.name}
    at = s.obj('atat')
    shots = projectiles(s.b, s.start, s.end, (ATAT_LASER,))
    cuts = [('attack', (s.order('atat', 'attack_victim') or {}).get('F', s.start))]
    for b in ('deploy1', 'undeploy1', 'deploy2', 'undeploy2', 'deploy3'):
        a = s.acts.get(b) or s.acts.get(b + '_troopers')
        if a:
            cuts.append((b, a['F']))
    r['phases'] = shots_between(shots, cuts)
    s.L.append('lasers %d; per phase: %s' % (len(shots), ', '.join('%s@%s:%d' % p for p in r['phases'])))
    for b in ('deploy1', 'undeploy1', 'deploy2', 'undeploy2', 'deploy3'):
        if b in s.acts:
            s.block_line(b)
        if b + '_troopers' in s.acts:
            s.block_line(b + '_troopers')
    seq = deploy_sequence(s, 'atat', ('deploy1', 'undeploy1', 'deploy2', 'undeploy2', 'deploy3'))
    r['deploy'] = seq
    others = [o for o in s.b['objs'].values() if o['role'] == 'other' and o['frame'] is not None and
              s.start <= o['frame'] <= s.end and at is not None and pos_at(at, o['frame']) and
              dist2(samples(o)[0], pos_at(at, o['frame'])) < 300]
    r['troopers'] = len(others)
    deploy_ret = (s.acts.get('deploy1') or {}).get('ret')
    went = seq['readable'] and any(o.get('state') == 0x18 for o in seq['orders'].values())
    r['verdict'] = ('DEPLOYS' if went else 'NO_DEPLOY_STATE' if seq['readable'] else 'LOCO_UNREADABLE') + \
        ' (DEPLOY ret=%s)' % deploy_ret
    s.L.append('new objects next to the AT-AT (troopers?): %d -> %s' % (len(others), r['verdict']))
    return r


def an_mdu(s):
    r = {'name': s.name}
    seq = deploy_sequence(s, 'mdu', ('deploy', 'deploy_again', 'undeploy', 'deploy_moving'))
    r['deploy'] = seq
    mb = (s.order('mdu', 'move_back') or {}).get('F')
    mf = (s.order('mdu', 'move_far') or {}).get('F')
    for b in ('deploy', 'deploy_again', 'undeploy', 'deploy_moving'):
        info = s.block_line(b) or {}
        o = seq['orders'].get(b, {}) if seq['readable'] else {}
        rel = '-'
        fin = info.get('finish')
        if fin is not None and o.get('next') is not None:
            rel = 'block finish - state change = %+d' % (fin - o['next'])
        elif fin is not None and o.get('change') is not None:
            rel = 'block finish - first change = %+d' % (fin - o['change'])
        if b == 'deploy_moving' and fin is not None and mb is not None:
            rel += '; block finish - move_back order = %+d' % (fin - mb)
        info['rel'] = rel
        r[b] = info
        s.L.append('  %s: %s' % (b, rel))
    dm = r.get('deploy_moving', {})
    d0 = r.get('deploy', {})
    o = seq['orders'].get('deploy', {}) if seq['readable'] else {}
    r['deploy_verdict'] = ('NO_BLOCK' if d0.get('ret') != 'block' else
                           'FINISHES_AT_DEPLOYED' if d0.get('finish') is not None and o.get('next') is not None and
                           abs(d0['finish'] - o['next']) <= 4 else
                           'FINISHED' if d0.get('finish') is not None else 'NEVER_FINISHED')
    r['again_verdict'] = 'NIL' if r.get('deploy_again', {}).get('ret') == 'nil' else 'RETURNED_%s' % (
        r.get('deploy_again', {}).get('ret'))
    mo = seq['orders'].get('deploy_moving', {}) if seq['readable'] else {}
    r['moving_verdict'] = ('NO_BLOCK' if dm.get('ret') != 'block' else
                           'FINISHED_BY_MOVE_ORDER' if dm.get('finish') is not None and mb is not None and
                           0 <= dm['finish'] - mb <= 4 else
                           'FINISHED_AT_DEPLOYED' if dm.get('finish') is not None and mo.get('state') == 0x18 else
                           'FINISHED' if dm.get('finish') is not None else 'NEVER_FINISHED')
    s.L.append('DEPLOY block: %s; DEPLOY again while deployed: %s; DEPLOY while moving (move F=%s): loco %s -> %s; '
               '%s' % (r['deploy_verdict'], r['again_verdict'], mf, loco_name(mo.get('before')),
                       loco_name(mo.get('state')), r['moving_verdict']))
    return r


def an_sticky(s):
    r = {'name': s.name}
    live, doomed = s.obj('live'), s.obj('doomed')
    kill = s.marks.get('target_killed', {}).get('F')
    if doomed is not None and death_frame(doomed) is not None:
        kill = death_frame(doomed)
    mv = (s.order('inf2', 'move_away') or {}).get('F')
    bl = s.block_line('sb_live') or {}
    bd = s.block_line('sb_dead') or {}
    bombs = projectiles(s.b, s.start, s.end)
    s.L.append('projectiles: %s; live target hit frames %s; doomed dies F=%s; move_away F=%s; before_move mark: %s' % (
        ', '.join('%s@%s' % (o['type'], o['frame']) for o in bombs[:6]) or '-',
        [x['F'] for x in (live or {'st': []})['st'] if x['hp'] is not None and live['hp0'] is not None and
         x['hp'] < live['hp0']][:3], kill, mv, s.marks.get('before_move', {}).get('sb_dead')))
    if bd.get('ret') != 'block':
        v = 'NO_BLOCK (ret=%s)' % bd.get('ret')
    elif bd.get('finish') is None:
        v = 'NEVER_FINISHED (pending at scenario end)'
    elif kill is not None and bd['finish'] < kill:
        v = 'FINISHED_BEFORE_KILL'
    elif kill is not None and bd['finish'] - kill <= 6:
        v = 'FINISHES_ON_TARGET_DEATH'
    elif mv is not None and 0 <= bd['finish'] - mv <= 6:
        v = 'FINISHES_ON_NEW_ORDER'
    else:
        v = 'FINISHED_LATER'
    r['live'] = bl
    r['dead'] = bd
    r['kill'] = kill
    r['move'] = mv
    r['verdict'] = v
    s.L.append('BLOCK WITH A TARGET THAT DIES: %s (finish F=%s, kill F=%s, move F=%s)' % (v, bd.get('finish'), kill,
                                                                                            mv))
    return r


def an_dt3(s):
    r = {'name': s.name}
    lasers = projectiles(s.b, s.start, s.end, (DT3_LASER,))
    miss = projectiles(s.b, s.start, s.end, (DT3_MISSILE,))
    cuts = [('attack', (s.order('dt3', 'attack_victim') or {}).get('F', s.start)),
            ('deploy1', (s.acts.get('deploy1') or {}).get('F')), ('jet', (s.acts.get('jet') or {}).get('F')),
            ('attack2', (s.order('dt3', 'attack_victim2') or {}).get('F')),
            ('deploy2', (s.acts.get('deploy2') or {}).get('F'))]
    cuts = [c for c in cuts if c[1] is not None]
    pl, pm = shots_between(lasers, cuts), shots_between(miss, cuts)
    r['phases'] = [(a[0], a[1], a[2], b[2]) for a, b in zip(pl, pm)]
    s.L.append('per phase (name@F: lasers/missiles): %s' % ', '.join('%s@%s:%d/%d' % p for p in r['phases']))
    for b in ('deploy1', 'jet', 'deploy2'):
        s.block_line(b)
    r['deploy'] = deploy_sequence(s, 'dt3', ('deploy1', 'jet', 'deploy2'))
    ph = {p[0]: p for p in r['phases']}
    if 'attack2' in ph and 'deploy2' in ph:
        silent = ph['attack2'][3] == 0
        again = ph['deploy2'][3] > 0
        r['verdict'] = ('MISSILES_SILENT_UNTIL_REDEPLOY' if silent and again else
                        'MISSILES_FIRE_AFTER_JET' if not silent else 'MISSILES_SILENT_EVEN_AFTER_REDEPLOY')
    else:
        r['verdict'] = 'NOT_MEASURED'
    s.L.append('DT3 -> %s' % r['verdict'])
    return r


def an_reinf(s):
    r = {'name': s.name, 'units': []}
    shooters = {sp['tag']: s.obj(sp['tag']) for sp in s.spawns if sp['tag'].startswith('mz8_')}
    shots = projectiles(s.b, s.start, s.end, (MZ8_BLAST,))
    by = {}
    for o in shots:
        t = nearest(o, shooters, 120.0)
        if t:
            by.setdefault(t, []).append(o['frame'])
    for sp in s.spawns:
        if not sp['tag'].startswith('mz8_'):
            continue
        o = shooters.get(sp['tag'])
        F0 = o['frame'] if o and o['frame'] is not None else sp['F']
        first = min(by.get(sp['tag'], []), default=None)
        att = first_where(o, 'ts', lambda v: v == 4, F0) if o else None
        u = {'tag': sp['tag'], 'spawn': F0, 'first': first, 'delay': None if first is None else first - F0,
             'attacking': att, 'shots': len(by.get(sp['tag'], []))}
        r['units'].append(u)
        s.L.append('%s spawn F=%s, targeting state 4 at +%s, first shot +%s (%d shots)' % (
            sp['tag'], F0, fmt(None if att is None else att - F0), fmt(u['delay']), u['shots']))
    ds = [u['delay'] for u in r['units'] if u['delay'] is not None]
    if len(ds) < 2:
        r['verdict'] = 'NOT_MEASURED'
    elif max(ds) - min(ds) <= 30 and max(ds) <= 90:
        r['verdict'] = 'NO_RANDOM_DELAY'
    elif max(ds) - min(ds) > 45:
        r['verdict'] = 'RANDOM_DELAY_LIKELY'
    else:
        r['verdict'] = 'UNCLEAR'
    s.L.append('first-shot delays %s (a surviving random delay would spread up to 159 frames) -> %s' % (ds, r['verdict']))
    return r


def an_muzzle(s):
    r = {'name': s.name, 'types': []}
    space = s.name.endswith('SPACE')
    shooters = {sp['tag']: s.obj(sp['tag']) for sp in s.spawns if re.match(r'mz\d+$', sp['tag'])}
    shots = projectiles(s.b, s.start, s.end)
    by = {}
    for o in shots:
        t = nearest(o, shooters, 450.0 if space else 120.0)
        if t:
            by.setdefault(t, []).append(o)
    for sp in s.spawns:
        if not re.match(r'mz\d+$', sp['tag']):
            continue
        o = shooters.get(sp['tag'])
        ps = by.get(sp['tag'], [])
        tss = sorted({x['ts'] for x in o['st'] if x['ts'] is not None}) if o else []
        v = 'NOT_SPAWNED' if sp.get('ok') != '1' else ('FIRES' if ps else 'SILENT')
        r['types'].append({'type': sp.get('type'), 'verdict': v, 'shots': len(ps)})
        s.L.append('%-42s %-11s shots %3d %s; targeting states %s' % (
            sp.get('type'), v, len(ps), ','.join(sorted({p['type'] for p in ps})) or '', tss or '-'))
    return r


def an_squad(s):
    r = {'name': s.name}
    sq = s.obj('squad')
    tgts = {t: s.obj(t) for t in ('t1', 't2', 't3')}
    tid = {str(o['id']): t for t, o in tgts.items() if o}
    members = []
    if sq is not None:
        members = [o for o in s.b['objs'].values() if any(x['cont'] == str(sq['id']) for x in o['st'])]
    if not members:
        members = [o for o in s.b['objs'].values() if o['type'] == 'X-WING' and o['frame'] is not None and
                   s.start <= o['frame'] <= s.start + 30]
    r['members'] = [o['id'] for o in members]
    if not members or not any(readable(o, 'tgt') for o in members):
        r['verdict'] = 'TARGET_UNREADABLE'
        s.L.append('members %s; member targets unreadable -> %s' % (r['members'], r['verdict']))
        return r
    t0 = min((o['frame'] for o in tgts.values() if o), default=s.start)
    frames = sorted({x['F'] for o in members for x in o['st'] if t0 <= x['F'] <= s.end})
    same = tot = 0
    for F in frames:
        vals = [value_at(o, 'tgt', F) for o in members if death_frame(o) is None or death_frame(o) > F]
        vals = [v for v in vals if v not in (None, '-', 'none', 'x')]
        if len(vals) >= 2:
            tot += 1
            same += len(set(vals)) == 1
    r['shared'] = (same / tot) if tot else None
    s.L.append('members %s; frames with every member on one target: %s of %d' % (r['members'], same, tot))
    tts = sorted({x['tt'] for o in members for x in o['st'] if x['tt'] not in ('-',)})
    s.L.append('team target values seen (container +0x110 -> +0x30): %s' % (tts or 'unreadable'))
    drops = []
    for n in (1, 2):
        m = s.marks.get('drop%d' % n)
        if not m:
            continue
        F = m['F']
        tag = m.get('target')
        dobj = tgts.get(tag)
        did = str(dobj['id']) if dobj else None
        rows = []
        for o in members:
            clr = first_where(o, 'tgt', lambda v: v != did, F)
            new = first_where(o, 'tgt', lambda v: v not in (None, '-', 'none', 'x', did), F)
            rows.append((o['id'], clr, new, value_at(o, 'tgt', new) if new is not None else None))
        news = [x[2] for x in rows if x[2] is not None]
        nt = {x[3] for x in rows if x[3] is not None}
        tt_chg = None
        for o in members:
            c = first_where(o, 'tt', lambda v: v not in ('-', did), F)
            if c is not None:
                tt_chg = c if tt_chg is None else min(tt_chg, c)
        if len(news) == len(rows) and news and max(news) - min(news) <= 1 and len(nt) == 1:
            v = 'ALL_AT_ONCE'
        elif news and len(nt) == 1:
            v = 'SAME_TARGET_STAGGERED'
        elif news:
            v = 'INDIVIDUAL'
        else:
            v = 'NO_RETARGET'
        first = min(rows, key=lambda x: x[2] if x[2] is not None else 10 ** 9)
        drops.append({'n': n, 'F': F, 'dropped': tag, 'rows': rows, 'verdict': v, 'tt_change': tt_chg,
                      'first_member': first[0]})
        s.L.append('drop%d F=%s (%s): %s; team target changed at +%s -> %s (first member to switch: %s)' % (
            n, F, tag, '; '.join('m%s clear +%s new +%s=%s' % (i, fmt(None if c is None else c - F),
                                                               fmt(None if w is None else w - F),
                                                               tid.get(t, t)) for i, c, w, t in rows),
            fmt(None if tt_chg is None else tt_chg - F), v, first[0]))
    r['drops'] = drops
    vs = {d['verdict'] for d in drops}
    r['verdict'] = vs.pop() if len(vs) == 1 else ('MIXED %s' % sorted(vs) if vs else 'NOT_MEASURED')
    return r


def centroid(objs, F):
    ps = [pos_at(o, F) for o in objs]
    ps = [p for p in ps if p is not None]
    if not ps:
        return None
    return (F, sum(p[1] for p in ps) / len(ps), sum(p[2] for p in ps) / len(ps), 0.0)


def an_escort(s):
    r = {'name': s.name}
    car = s.obj('carrier')
    owner = car['owner0'] if car else None
    esc = [o for o in s.b['objs'].values() if o['type'] in ESCORT_FIGHTERS + ESCORT_SQUADS and o['frame'] is not None
           and s.start <= o['frame'] <= s.end and (owner is None or o['owner0'] == owner)]
    fighters = [o for o in esc if o['type'] in ESCORT_FIGHTERS] or esc
    dead = s.marks.get('carrier_dead')
    intr = s.obj('intruder')
    Fi = s.marks.get('intruder', {}).get('F')
    Fo = s.marks.get('intruder_gone', {}).get('F')
    r['escorts'] = len(esc)
    if not fighters or dead is None:
        r['verdict'] = 'NOT_MEASURED'
        s.L.append('escorts %d, carrier_dead mark %s -> NOT_MEASURED' % (len(fighters), 'yes' if dead else 'no'))
        return r
    Fd = dead['F']
    if car is not None and death_frame(car) is not None:
        Fd = death_frame(car)
    dp = (Fd, _float(dead.get('x')), _float(dead.get('y')), 0.0)
    if dp[1] is None and car is not None:
        dp = pos_at(car, Fd)
    start = pos_at(car, s.start) if car else None
    pts = {}
    for k in (0, 300, 600, 1199):
        c = centroid(fighters, Fd + k)
        pts[k] = None if c is None else dist2(c, dp)
    s.L.append('escorts %d (%d fighters); carrier dies F=%s at (%.0f, %.0f); escort centroid distance from there: '
               '%s' % (len(esc), len(fighters), Fd, dp[1], dp[2], ', '.join('+%d: %s' % (k, fmt(v, 0))
                                                                             for k, v in pts.items())))
    eng = None
    near = None
    back = None
    if intr is not None and Fi is not None:
        Fo2 = Fo if Fo is not None else s.end
        iid = str(intr['id'])
        eng = any(first_where(o, 'tgt', lambda v: v == iid, Fi, Fo2) is not None for o in fighters) \
            if any(readable(o, 'tgt') for o in fighters) else None
        shots = [p for p in projectiles(s.b, Fi, Fo2) if dist2(samples(p)[-1], pos_at(intr, p['frame'])) < 300]
        near = min((dist2(pos_at(o, F), pos_at(intr, F)) for o in fighters for F in range(Fi, Fo2, 30)
                    if pos_at(o, F) and pos_at(intr, F)), default=None)
        c = centroid(fighters, s.end)
        back = None if c is None else dist2(c, dp)
        r.update({'engaged_target': eng, 'shots_at_intruder': len(shots), 'closest_to_intruder': near,
                  'back_dist': back})
        s.L.append('intruder F=%s..%s: targeted by an escort: %s; shots landing near it %d; closest escort %s; '
                   'centroid distance from the death point at scenario end %s' % (
                       Fi, Fo2, {True: 'yes', False: 'no', None: 'unreadable'}[eng], len(shots), fmt(near, 0),
                       fmt(back, 0)))
    held = pts.get(1199) is not None and pts[1199] <= 600
    went_start = start is not None and pts.get(1199) is not None and \
        dist2(centroid(fighters, Fd + 1199), start) < 400 < pts[1199]
    if held:
        v = 'GUARDS_LAST_POSITION'
        if eng is False and not r.get('shots_at_intruder'):
            v += '_IGNORES_INTRUDER'
        elif back is not None and back > 800:
            v += '_BUT_DID_NOT_RETURN'
    elif went_start:
        v = 'RETURNED_TO_SPAWN_POINT'
    else:
        v = 'LEFT_THE_POSITION'
    r['verdict'] = v
    r['dist'] = pts
    s.L.append('ESCORT -> %s' % v)
    return r


ANALYSERS = {'SPMAT': an_spmat, 'ATAT': an_atat, 'MDU': an_mdu, 'STICKY': an_sticky, 'DT3': an_dt3,
             'REINF': an_reinf, 'MUZZLE_LAND': an_muzzle, 'MUZZLE_SPACE': an_muzzle, 'SQUAD': an_squad,
             'ESCORT': an_escort}


# ---- item 2: the human's Remote_Bomb ground order ------------------------------------------------------------------

def toss_clips(o):
    """BOMBTOSS clips of one object: [{'start', 'end', 'max', 'done'}]."""
    out = []
    cur = None
    for x in o['st']:
        a = x['anim']
        if a is not None and a[0] == BOMBTOSS:
            if cur is None or a[2] + 0.5 < cur['max_last']:
                cur = {'start': x['F'], 'end': None, 'max': a[2], 'max_last': a[2], 'done': a[1] == 2}
                out.append(cur)
            cur['max'] = max(cur['max'], a[2])
            cur['max_last'] = a[2]
            cur['done'] = cur['done'] or a[1] == 2
        elif cur is not None and a is not None:
            cur['end'] = x['F']
            cur = None
    return out


def an_human(battle, rr):
    L = []
    res = {'name': 'HUMAN'}
    hum = next((d for _n, k, d, _t in rr if k == 'HUMAN'), None)
    counts = [(d['F'], d.get('markers'), d.get('bombs')) for _n, k, d, _t in rr if k == 'COUNT' and
              d.get('scn') == 'HUMAN']
    sabs = [o for o in battle['objs'].values() if o['role'] == 'human']
    markers = sorted([o for o in battle['objs'].values() if o['type'] == MARKER], key=lambda o: o['frame'] or 0)
    bombs = sorted([o for o in battle['objs'].values() if o['type'] == REMOTE_BOMB], key=lambda o: o['frame'] or 0)
    clips = [c for o in sabs for c in toss_clips(o)]
    end = battle.get('last')
    L.append('Saboteur: %s; memory: saboteurs %s, BOMBTOSS clips %s, bombs at F=%s; Lua marker/bomb counts %s' % (
        ('spawned id=%s at (%s, %s)' % (hum.get('id'), hum.get('x'), hum.get('y'))) if hum and hum.get('saboteur') ==
        '1' else 'NOT SPAWNED (%s)' % (hum and ' '.join('%s=%s' % kv for kv in hum.items() if kv[0] != 'F')),
        [o['id'] for o in sabs], ['F=%s..%s max %.1f%s' % (c['start'], fmt(c['end']), c['max'],
                                                          ' finished' if c['done'] else '') for c in clips],
        [o['frame'] for o in bombs], counts[:12]))
    res['markers'] = []
    for m in markers:
        gone = m['gone']
        after = [c for c in clips if c['start'] >= m['frame'] - 3]
        bomb = next((b['frame'] for b in bombs if b['frame'] >= m['frame']), None)
        interrupted = [c for c in after if c['max'] < 12.0 and not c['done'] and (gone is None or c['start'] <= gone)]
        if gone is not None and bomb is not None and abs(gone - bomb) <= 6:
            v = 'REMOVED_AT_THE_THROW'
        elif gone is not None and any(c['max'] >= 12.0 and abs(gone - c['start']) <= 60 for c in after):
            v = 'REMOVED_AT_THE_THROW'
        elif gone is None and interrupted:
            v = 'PERSISTS_AFTER_INTERRUPTED_TOSS'
        elif gone is None and not after:
            v = 'PERSISTS_NO_TOSS'
        elif gone is None:
            v = 'PERSISTS'
        else:
            v = 'REMOVED_OTHER'
        p = samples(m)[0] if samples(m) else (None, None, None, None)
        res['markers'].append({'F': m['frame'], 'gone': gone, 'verdict': v, 'interrupted': len(interrupted)})
        L.append('marker obj %s F=%s at (%s, %s) gone F=%s (battle end F=%s); tosses after it %d (interrupted before '
                 'frame 12: %d); bomb F=%s -> %s' % (m['id'], m['frame'], fmt(p[1], 0), fmt(p[2], 0), fmt(gone), end,
                                                     len(after), len(interrupted), fmt(bomb), v))
    vs = [m['verdict'] for m in res['markers']]
    if not markers:
        lua_m = max((_int(c[1], 0) for c in counts), default=0)
        res['verdict'] = ('NO_MARKER (ground target not offered, or H1-H3 not done)' if not lua_m else
                          'MARKER_IN_LUA_ONLY (memory missed it)')
    elif 'PERSISTS_AFTER_INTERRUPTED_TOSS' in vs:
        res['verdict'] = 'MARKER_USED_AND_PERSISTS_AFTER_INTERRUPT'
    elif 'REMOVED_AT_THE_THROW' in vs and not any(v.startswith('PERSISTS') for v in vs):
        res['verdict'] = 'MARKER_USED_REMOVED_AT_THROW_ONLY'
    else:
        res['verdict'] = 'MARKER_USED: ' + ','.join(vs)
    L.append('REMOTE_BOMB GROUND MARKER -> %s' % res['verdict'])
    res['lines'] = L
    return res


# ---- report --------------------------------------------------------------------------------------------------------

def pick_battle(battles, rr, want_type):
    arm_n = next((n for n, k, _d, _t in rr if k == 'ARM'), None)
    cands = [b for b in battles if battles[b]['type'] == want_type and (arm_n is None or b <= max(arm_n, 1))]
    if arm_n in battles and battles[arm_n]['type'] == want_type:
        return arm_n
    return max(cands, default=None)


def parse(path):
    battles, recs, meta = read_log(path)
    runs_ = {}
    for n, kind, d, text in recs:
        runs_.setdefault(d.get('r', '?'), []).append((n, kind, d, text))
    out = {'runs': [], 'meta': meta, 'lines': [], 'answers': {}}
    L = out['lines']
    L.append('S7/S8 probe report (%s)' % os.path.basename(path))
    g = meta['game'] or {}
    L.append('game: pid=%s identity=%s; heap scans %d; Lua records %d; probe source in memory: %s; reader exit: %s' % (
        g.get('pid', '?'), g.get('identity', '?'), meta['scans'], len(recs), 'yes' if meta['src'] else 'no',
        ' '.join('%s=%s' % kv for kv in (meta['exit'] or {}).items()) or '-'))
    for m in meta['modeloff']:
        L.append('model pointer search: %s' % ' '.join('%s=%s' % kv for kv in m.items()))
    L.append('team data layout: %s' % (', '.join('%s (mode %s)' % (d.get('how'), d.get('n')) for d in meta['teamdata'])
                                       or 'never resolved'))
    skips = [d for _n, k, d, _t in recs if k == 'SKIP']
    L.append('SKIP (other AI players / modes): %s' % (', '.join('%s/%s' % (d.get('fac'), d.get('mode'))
                                                                for d in skips[:8]) or '-'))
    ans = out['answers']
    for rid, rr in runs_.items():
        arm = next((d for _n, k, d, _t in rr if k == 'ARM'), None)
        if arm is None:
            continue
        mode = arm.get('mode')
        bn = pick_battle(battles, rr, 1 if mode == 'Land' else 2)
        battle = battles.get(bn, {'objs': {}, 'players': {}, 'last': None})
        spawns = [d for _n, k, d, _t in rr if k == 'SPAWN']
        off, nm = frame_offset(battle, spawns)
        if off:
            for _n, _k, d, _t in rr:
                if d['F'] is not None:
                    d['F'] += off
        run = {'rid': rid, 'mode': mode, 'battle': bn, 'scenarios': [], 'frame_offset': off}
        out['runs'].append(run)
        L.append('')
        L.append('== run %s: %s battle %s, AI %s (player %s), map %s, points_ok=%s, %s ==' % (
            rid, mode, bn, arm.get('fac'), arm.get('pid'), arm.get('map'), arm.get('points_ok'),
            arm.get('service_rate')))
        ticks = [d['F'] for _n, k, d, _t in rr if k == 'TICK' and d['F'] is not None]
        L.append('script resumes: %s (gaps %s; 3 = every free-store service); Lua->memory frame offset %+d (from %d '
                 'spawn ids)' % (ticks[:8], sorted({ticks[i + 1] - ticks[i] for i in range(len(ticks) - 1)}), off, nm))
        vic = next((d for _n, k, d, _t in rr if k == 'VICTIMS'), {})
        L.append('victims owned by %s (player %s); memory objects in battle: %d' % (
            vic.get('faction', '?'), vic.get('id', '?'), len(battle['objs'])))
        for _n, k, d, _t in rr:
            if k in ('APIERR', 'ERR'):
                L.append('%s F=%s scn=%s %s' % (k, d['F'], d.get('scn'), ' '.join(
                    '%s=%s' % (a, b) for a, b in d.items() if a in ('api', 'err', 'msg', 'step'))))
        done = next((d for _n, k, d, _t in rr if k == 'DONE'), None)
        L.append('DONE: %s' % ('F=%s scenarios=%s' % (done['F'], done.get('scenarios')) if done else
                               'not reached (game quit early?)'))
        for sc in scenarios_of(rr):
            s = Scn(sc, battle)
            L.append('')
            L.append('-- %s: F=%s..%s (%s)' % (sc['name'], sc['start'], fmt(sc['end']),
                                              (sc.get('end_rec') or {}).get('why', 'no end record')))
            s.spawn_lines()
            fn = ANALYSERS.get(s.name)
            r = fn(s) if fn else {'name': s.name}
            r['start'], r['end'] = sc['start'], sc['end']
            run['scenarios'].append(r)
            for line in s.L:
                L.append('   ' + line)
        if mode == 'Land':
            h = an_human(battle, rr)
            run['scenarios'].append(h)
            L.append('')
            L.append('-- HUMAN (Remote_Bomb steps H1-H3)')
            for line in h['lines']:
                L.append('   ' + line)
        by = {r['name']: r for r in run['scenarios']}
        if 'SPMAT' in by:
            ans['1_arc'] = by['SPMAT'].get('arc')
            ans['1_edge'] = by['SPMAT'].get('edge')
            ans['3_spmat_window'] = by['SPMAT'].get('window')
            ans['3_spmat_reversal'] = (by['SPMAT'].get('reversal') or {}).get('verdict')
        if 'ATAT' in by:
            ans['3_atat'] = by['ATAT'].get('verdict')
        if 'HUMAN' in by:
            ans['2_marker'] = by['HUMAN'].get('verdict')
        if 'MDU' in by:
            ans['5_deploy_block'] = by['MDU'].get('deploy_verdict')
            ans['5_deploy_again'] = by['MDU'].get('again_verdict')
            ans['5_deploy_moving'] = by['MDU'].get('moving_verdict')
        if 'STICKY' in by:
            ans['5_target_dies'] = by['STICKY'].get('verdict')
        for k in ('MUZZLE_LAND', 'MUZZLE_SPACE'):
            if k in by:
                ans['4_' + k.lower()] = {t['type']: t['verdict'] for t in by[k].get('types', [])}
        if 'SQUAD' in by:
            ans['6_squad'] = by['SQUAD'].get('verdict')
        if 'ESCORT' in by:
            ans['7_escort'] = by['ESCORT'].get('verdict')
        if 'REINF' in by:
            ans['8_first_shot'] = by['REINF'].get('verdict')
        if 'DT3' in by:
            ans['8_dt3'] = by['DT3'].get('verdict')
    L.append('')
    L.append('== ANSWERS (README.md lists the expected values) ==')
    for k in sorted(ans):
        L.append('   %-18s %s' % (k, ans[k]))
    if not out['runs']:
        L.append('')
        L.append('NO ARMED RUN: no ARM record (the AI player was not the configured faction in a land or space battle, '
                 'the hook did not load, or the records were not found: see SCAN / LUA lines)')
    return out


def cmd_parse(a):
    res = parse(a.log)
    text = '\n'.join(res['lines']) + '\n'
    if getattr(a, 'out', None):
        with open(a.out, 'w') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if getattr(a, 'json', None):
        with open(a.json, 'w') as f:
            json.dump({'runs': res['runs'], 'answers': res['answers']}, f, indent=1, default=str)
    return res


# ---- selftest ------------------------------------------------------------------------------------------------------

class Synth:
    """Synthetic reader log that follows the port model with known numbers."""

    def __init__(self):
        self.lines = ['S78READER 1 scan_sec=5', 'GAME pid=1 base=0x0 identity=test']
        self.recs = []
        self.next_id = 9000
        self.n = 0
        self.rid = None

    def mode(self, typ, run):
        self.n += 1
        self.rid = 'abc123.%d' % run
        self.lines.append('MODE n=%d ptr=0x%x type=%d game_type=3 stack=1 frame=0 gframe=0' % (self.n, self.n, typ))

    def rec(self, kind, F, body):
        self.recs.append((self.n, 'S78P1|%s|r=%s|f=%d|%s|END' % (kind, self.rid, F, body)))

    def obj(self, F, typ, role, x, y, z=0.0, owner=1, hp=1.0):
        self.next_id += 1
        self.lines.append('OBJ n=%d frame=%d obj=%d addr=0x1 store=primary type=%s owner=%d x=%.1f y=%.1f z=%.1f '
                          'hp=%.2f role=%s' % (self.n, F, self.next_id, typ, owner, x, y, z, hp, role))
        return self.next_id

    def st(self, F, oid, x, y, z=0.0, hp=1.0, owner=1, flags=0, anim='-', loco='-', tgt='-', ts='-', cont='-',
           tt='-'):
        self.lines.append('ST n=%d frame=%d obj=%d owner=%d flags=%s hp=%.2f x=%.1f y=%.1f z=%.1f anim=%s loco=%s '
                          'tgt=%s ts=%s cont=%s tt=%s' % (self.n, F, oid, owner, hex(flags), hp, x, y, z, anim, loco,
                                                          tgt, ts, cont, tt))

    def gone(self, F, oid):
        self.lines.append('GONE n=%d frame=%d obj=%d type=X' % (self.n, F, oid))

    def spawn(self, F, scn, tag, typ, role, x, y, z=0.0, owner=1, ok=True):
        if not ok:
            self.rec('SPAWN', F, 'scn=%s|tag=%s|type=%s|ok=0|why=spawn_failed' % (scn, tag, typ))
            return None
        oid = self.obj(F, typ.upper(), role, x, y, z, owner)
        self.rec('SPAWN', F, 'scn=%s|tag=%s|type=%s|ok=1|how=cfg|id=%d|n=1|owner=%d|x=%.1f|y=%.1f|z=%.1f' % (
            scn, tag, typ, oid, owner, x, y, z))
        return oid

    def act(self, F, scn, tag, blk, ab, arg, ret, fin=None):
        self.rec('ACT', F, 'scn=%s|tag=%s|blk=%s|ab=%s|arg=%s|call=ok|ret=%s|ready_before=1|ready_after=1|'
                           'active_after=0' % (scn, tag, blk, ab, arg, ret))
        if fin is not None:
            self.rec('BLK', fin, 'scn=%s|blk=%s|ev=finished|res=true|after=%d' % (scn, blk, fin - F))

    def proj(self, F, typ, x, y, z=0.0, life=3):
        oid = self.obj(F, typ, 'projectile', x, y, z)
        self.gone(F + life, oid)
        return oid

    def grenade(self, F, x0, y0, z0, ux, d, v, theta):
        th = math.radians(theta)
        vh, vz = v * math.cos(th), v * math.sin(th)
        T = int(d / vh)
        oid = self.obj(F, GRENADE, 'projectile', x0, y0, z0)
        for t in range(1, T + 1):
            self.st(F + t, oid, x0 + ux * vh * t, y0, z0 + vz * t - 0.5 * LOB_G * t * t)
        self.gone(F + T + 1, oid)
        return T

    def scn(self, F, name, i, dur):
        self.rec('SCN', F, 'scn=%s|ev=start|i=%d|dur=%d|mode=x|points_ok=true' % (name, i, dur))

    def scn_end(self, F, name):
        self.rec('SCN', F, 'scn=%s|ev=end|why=time|despawned=0|pending_blocks=' % name)

    def text(self):
        out = list(self.lines)
        out += ['LUA n=%d frame=99999 addr=0x10 rec=%s' % (n, r) for n, r in self.recs]
        out += ['MODE_END n=%d last_frame=20000' % self.n, 'EXIT reason=game_exited modes=%d' % self.n]
        return '\n'.join(out) + '\n'


def synth_land(s, truth):
    s.mode(1, 1)
    s.rec('ARM', 0, 'pid=2|fac=REBEL|mode=Land|map=_MP_LAND_GEONOSIS|points_ok=true|service_rate=20->0.05')
    for i in range(12):
        s.rec('TICK', 3 + 3 * i, 'n=%d|src=fss' % (i + 1))
    s.rec('VICTIMS', 450, 'faction=Hostile|id=4')
    X, Y, Z = 1000.0, 2000.0, 10.0
    # --- SPMAT: deploy 10 frames after the order, deployed 40 later; arc at 275; nothing at 547; fire at 530
    F = 450
    s.scn(F, 'SPMAT', 1, 2400)
    sp = s.spawn(F, 'SPMAT', 'spmat', 'SPMAT_Walker', 'attacker', X, Y, Z)
    v275 = s.spawn(F + 30, 'SPMAT', 'v275', 'T4B_Tank', 'victim', X + 275, Y, Z, owner=4)
    loco = [(F, 0x16), (F + 55, 0x18), (F + 95, 0x19), (F + 1637, 0x1a), (F + 1677, 0x16), (F + 1876, 0x18),
            (F + 1888, 0x1a), (F + 1900, 0x18), (F + 1940, 0x19)]
    tgt = {}
    s.act(F + 45, 'SPMAT', 'spmat', 'deploy1', 'DEPLOY', 'bool', 'block', F + 96)
    pred = lob(275.0, Z + Z_ADJUST - Z)
    for k in range(4):
        s.grenade(F + 120 + 150 * k, X, Y, Z, 1.0, 275.0, pred['v'], pred['theta'])
    truth['arc'] = 'MATCHES_LOB_SLOWER'
    s.rec('ORDER', F + 585, 'scn=SPMAT|tag=v275|order=despawn|ok=1')
    s.gone(F + 586, v275)
    v547 = s.spawn(F + 585, 'SPMAT', 'v547', 'T4B_Tank', 'victim', X + 547, Y, Z, owner=4)
    s.rec('MARK', F + 585, 'scn=SPMAT|mark=edge|')
    tgt[F + 600] = (str(v547), 3)
    v530 = s.spawn(F + 1185, 'SPMAT', 'v530', 'T4B_Tank', 'victim', X + 530, Y, Z, owner=4)
    s.rec('MARK', F + 1185, 'scn=SPMAT|mark=control|')
    tgt[F + 1200] = (str(v530), 4)
    p530 = lob(530.0, Z_ADJUST)
    for k in range(2):
        s.grenade(F + 1220 + 150 * k, X, Y, Z, 1.0, 530.0, p530['v'], p530['theta'])
    truth['edge'] = 'SCANNED_NOT_FIRED'
    s.act(F + 1635, 'SPMAT', 'spmat', 'undeploy1', 'DEPLOY', 'bool', 'block', F + 1678)
    s.act(F + 1875, 'SPMAT', 'spmat', 'deploy2', 'DEPLOY', 'bool', 'block', F + 1887)
    s.act(F + 1887, 'SPMAT', 'spmat', 'undeploy2', 'DEPLOY', 'bool', 'block', F + 1899)
    s.act(F + 1899, 'SPMAT', 'spmat', 'deploy3', 'DEPLOY', 'bool', 'block', F + 1941)
    truth['window'] = 50
    truth['reversal'] = 'MIRRORS'
    keys = sorted(set([f for f, _v in loco] + list(tgt)))
    cur_l, cur_t = 0x16, ('none', 2)
    for f in keys:
        cur_l = dict(loco).get(f, cur_l)
        cur_t = tgt.get(f, cur_t)
        s.st(f, sp, X, Y, Z, loco=hex(cur_l), tgt=cur_t[0], ts=cur_t[1])
    s.scn_end(F + 2400, 'SPMAT')
    # --- ATAT: DEPLOY returns nil, DEPLOY_TROOPERS nil, lasers every 45 frames, never 0x18
    F = 2853
    s.scn(F, 'ATAT', 2, 1500)
    at = s.spawn(F, 'ATAT', 'atat', 'AT_AT_Walker', 'attacker', X, Y, Z)
    s.spawn(F, 'ATAT', 'victim', 'T4B_Tank', 'victim', X + 250, Y, Z, owner=4)
    s.rec('ORDER', F + 30, 'scn=ATAT|tag=atat|order=attack_victim|ok=1')
    for blk, at_f in (('deploy1', 330), ('undeploy1', 780), ('deploy2', 1080), ('undeploy2', 1092), ('deploy3', 1104)):
        s.act(F + at_f, 'ATAT', 'atat', blk, 'DEPLOY', 'bool', 'nil')
        s.act(F + at_f, 'ATAT', 'atat', blk + '_troopers', 'DEPLOY_TROOPERS', 'none', 'nil')
    s.st(F + 1, at, X, Y, Z, loco='0x16')
    for k in range(30):
        s.proj(F + 60 + 45 * k, ATAT_LASER, X + 10, Y, Z + 20)
    truth['atat'] = 'NO_DEPLOY_STATE (DEPLOY ret=nil)'
    s.scn_end(F + 1500, 'ATAT')
    # --- MDU: block finishes at deployed / at stopped; DEPLOY again -> nil; DEPLOY while moving -> ends at move order
    F = 4356
    s.scn(F, 'MDU', 3, 1200)
    md = s.spawn(F, 'MDU', 'mdu', 'Rebel_Mobile_Defense_Unit', 'attacker', X, Y, Z)
    s.act(F + 30, 'MDU', 'mdu', 'deploy', 'DEPLOY', 'bool', 'block', F + 91)
    s.act(F + 300, 'MDU', 'mdu', 'deploy_again', 'DEPLOY', 'bool', 'nil')
    s.act(F + 330, 'MDU', 'mdu', 'undeploy', 'DEPLOY', 'bool', 'block', F + 392)
    s.rec('ORDER', F + 600, 'scn=MDU|tag=mdu|order=move_far|ok=1')
    s.act(F + 630, 'MDU', 'mdu', 'deploy_moving', 'DEPLOY', 'bool', 'block', F + 930)
    s.rec('ORDER', F + 930, 'scn=MDU|tag=mdu|order=move_back|ok=1')
    for f, v in ((F + 1, 0x16), (F + 31, 0x18), (F + 91, 0x19), (F + 331, 0x1a), (F + 391, 0x16), (F + 601, 0x17)):
        s.st(f, md, X + (5 if f > F + 600 else 0), Y, Z, loco=hex(v))
    truth['mdu'] = ('FINISHES_AT_DEPLOYED', 'NIL', 'FINISHED_BY_MOVE_ORDER')
    s.scn_end(F + 1200, 'MDU')
    # --- STICKY: the block of the order at a target that dies stays pending until the next move order
    F = 5559
    s.scn(F, 'STICKY', 4, 1500)
    s.spawn(F, 'STICKY', 'inf1', 'Rebel_Infiltrator', 'attacker', X, Y - 60, Z)
    s.spawn(F, 'STICKY', 'inf2', 'Rebel_Infiltrator', 'attacker', X, Y + 60, Z)
    s.spawn(F, 'STICKY', 'live', 'T4B_Tank', 'victim', X + 120, Y - 60, Z, owner=4)
    s.act(F + 30, 'STICKY', 'inf1', 'sb_live', 'STICKY_BOMB', 'obj', 'block', F + 70)
    s.proj(F + 62, 'PROJ_STICKY_BOMB', X + 20, Y - 60, Z, life=200)
    dm = s.spawn(F + 450, 'STICKY', 'doomed', 'T4B_Tank', 'victim', X + 450, Y + 60, Z, owner=4)
    s.act(F + 480, 'STICKY', 'inf2', 'sb_dead', 'STICKY_BOMB', 'obj', 'block', F + 1200)
    s.rec('ORDER', F + 540, 'scn=STICKY|tag=doomed|order=kill|ok=1|calls=1|valid_after=0')
    s.rec('MARK', F + 540, 'scn=STICKY|mark=target_killed|')
    s.st(F + 541, dm, X + 450, Y + 60, Z, hp=0.0, owner=4, flags=0x40)
    s.rec('MARK', F + 1200, 'scn=STICKY|mark=before_move|sb_dead=pending')
    s.rec('ORDER', F + 1200, 'scn=STICKY|tag=inf2|order=move_away|ok=1')
    truth['sticky'] = 'FINISHES_ON_NEW_ORDER'
    s.scn_end(F + 1500, 'STICKY')
    # --- DT3: lasers undeployed, missiles deployed, nothing after the jet until redeploy
    F = 7062
    s.scn(F, 'DT3', 5, 2100)
    s.spawn(F, 'DT3', 'dt3', 'Dark_Trooper_PhaseIII', 'attacker', X, Y, Z)
    s.spawn(F, 'DT3', 'victim', 'T4B_Tank', 'victim', X + 200, Y, Z, owner=4)
    s.rec('ORDER', F + 30, 'scn=DT3|tag=dt3|order=attack_victim|ok=1')
    s.act(F + 300, 'DT3', 'dt3', 'deploy1', 'DEPLOY', 'bool', 'block', F + 340)
    s.act(F + 750, 'DT3', 'dt3', 'jet', 'JET_PACK', 'pos', 'block', F + 800)
    s.rec('ORDER', F + 1200, 'scn=DT3|tag=dt3|order=attack_victim2|ok=1')
    s.act(F + 1650, 'DT3', 'dt3', 'deploy2', 'DEPLOY', 'bool', 'block', F + 1690)
    for a, b, typ in ((60, 290, DT3_LASER), (360, 740, DT3_MISSILE), (1230, 1640, DT3_LASER),
                      (1700, 2090, DT3_MISSILE)):
        for f in range(F + a, F + b, 40):
            s.proj(f, typ, X + 5, Y, Z + 10)
    truth['dt3'] = 'MISSILES_SILENT_UNTIL_REDEPLOY'
    s.scn_end(F + 2100, 'DT3')
    # --- REINF: four MZ8s fire 21 frames after their spawn
    F = 9165
    s.scn(F, 'REINF', 6, 780)
    s.spawn(F, 'REINF', 'victim', 'T4B_Tank', 'victim', X + 200, Y, Z, owner=4)
    for k in range(1, 5):
        f = F + 30 + (k - 1) * 150
        side = (k - 2.5) * 70
        oid = s.spawn(f, 'REINF', 'mz8_%d' % k, 'MZ8_Pulse_Cannon_Tank', 'attacker', X, Y + side, Z)
        s.st(f + 12, oid, X, Y + side, Z, ts=4)
        for j in range(2):
            s.proj(f + 21 + 159 * j, MZ8_BLAST, X + 15, Y + side, Z + 8)
    truth['reinf'] = 'NO_RANDOM_DELAY'
    s.scn_end(F + 780, 'REINF')
    # --- MUZZLE_LAND: every land-capable type spawns and stays silent; the space-only ones fail to spawn
    F = 9948
    s.scn(F, 'MUZZLE_LAND', 7, 660)
    import build_s7s8_mod as B
    for i, t in enumerate(B.MUZZLE_TYPES):
        f = F + (330 if i >= 4 else 0)
        oid = s.spawn(f, 'MUZZLE_LAND', 'mz%d' % (i + 1), t, 'attacker', X + 220 * (i % 4), Y, Z,
                      ok=('Landing' in t or 'UM07' in t))
        if oid and i < 4:
            s.gone(F + 331, oid)
    truth['muzzle_land'] = {t: ('SILENT' if ('Landing' in t or 'UM07' in t) else 'NOT_SPAWNED')
                            for t in B.MUZZLE_TYPES}
    s.scn_end(F + 660, 'MUZZLE_LAND')
    s.rec('DONE', F + 663, 'mode=Land|scenarios=8|apierr=')
    # --- HUMAN: marker 1 with an interrupted toss (persists), marker 2 removed by the throw at clip frame 12
    sab = s.obj(500, 'UNDERWORLD_SABOTEUR', 'human', 300.0, 300.0)
    s.rec('HUMAN', 455, 'player=1|units=12|saboteur=1|id=%d|x=300.0|y=300.0' % sab)
    m1 = s.obj(2000, MARKER, 'marker', 500.0, 300.0)
    for n in range(0, 8):
        s.st(2100 + n, sab, 480.0, 300.0, anim='%d:0:%.3f:1.000' % (BOMBTOSS, float(n)))
    s.st(2108, sab, 480.0, 300.0, anim='2:1:0.000:1.000')
    m2 = s.obj(3000, MARKER, 'marker', 520.0, 360.0)
    for n in range(0, 20):
        s.st(3050 + n, sab, 500.0, 350.0, anim='%d:%d:%.3f:1.000' % (BOMBTOSS, 2 if n == 19 else 0, float(n)))
    s.obj(3062, REMOTE_BOMB, 'projectile', 505.0, 352.0)
    s.gone(3063, m2)
    s.st(3071, sab, 500.0, 350.0, anim='0:1:0.000:1.000')
    s.rec('COUNT', 2001, 'scn=HUMAN|what=markers_bombs|markers=1|bombs=0')
    _ = m1
    truth['marker'] = 'MARKER_USED_AND_PERSISTS_AFTER_INTERRUPT'


def synth_space(s, truth):
    s.mode(2, 2)
    s.rec('ARM', 0, 'pid=2|fac=REBEL|mode=Space|map=_MP_SPACE_KUAT|points_ok=true|service_rate=20->0.05')
    s.rec('VICTIMS', 450, 'faction=Hostile|id=4')
    import build_s7s8_mod as B
    F = 450
    s.scn(F, 'MUZZLE_SPACE', 1, 660)
    for i, t in enumerate(B.MUZZLE_TYPES):
        f = F + (330 if i >= 4 else 0)
        ok = 'Landing' not in t
        x = 0.0 + 600 * (i % 4)
        oid = s.spawn(f, 'MUZZLE_SPACE', 'mz%d' % (i + 1), t, 'attacker', x, 0.0, ok=ok)
        if oid and i < 4:
            s.gone(F + 331, oid)
        if ok:
            for j in range(3):
                s.proj(f + 40 + 30 * j, 'PROJ_SHIP_SMALL_LASER_CANNON_GREEN', x + 30, 20.0)
    truth['muzzle_space'] = {t: ('FIRES' if 'Landing' not in t else 'NOT_SPAWNED') for t in B.MUZZLE_TYPES}
    s.scn_end(F + 660, 'MUZZLE_SPACE')
    # --- SQUAD: three members share the target; each drop retargets all of them in one frame
    F = 1113
    s.scn(F, 'SQUAD', 2, 2100)
    sq = s.spawn(F, 'SQUAD', 'squad', 'Rebel_X-Wing_Squadron', 'attacker', -1000.0, 1000.0)
    mem = [s.obj(F + 1, 'X-WING', 'attacker', -1000.0 + 20 * k, 1000.0) for k in range(3)]
    ts_ = [s.spawn(F + 300, 'SQUAD', 't%d' % (k + 1), 'Tartan_Patrol_Cruiser', 'victim', -350.0, 750.0 + 250 * k,
                   owner=4) for k in range(3)]
    timeline = [(F + 320, ts_[1]), (F + 1201, 'none'), (F + 1207, ts_[0]), (F + 1801, 'none'), (F + 1810, ts_[2])]
    for f, t in timeline:
        for k, m in enumerate(mem):
            s.st(f, m, -900.0 + 20 * k, 1000.0, tgt=str(t), ts=4, cont=str(sq), tt=str(t))
    s.rec('MARK', F + 1200, 'scn=SQUAD|mark=drop1|target=t2|lua_target=%d' % ts_[1])
    s.gone(F + 1201, ts_[1])
    s.rec('MARK', F + 1800, 'scn=SQUAD|mark=drop2|target=t1|lua_target=%d' % ts_[0])
    s.gone(F + 1801, ts_[0])
    truth['squad'] = 'ALL_AT_ONCE'
    s.scn_end(F + 2100, 'SQUAD')
    # --- ESCORT: the fighters stay around the death point, attack the intruder, come back
    F = 3216
    s.scn(F, 'ESCORT', 3, 4500)
    ex, ey = 1400.0, -1200.0
    car = s.spawn(F, 'ESCORT', 'carrier', 'Star_Destroyer', 'attacker', ex, ey, owner=1)
    fs = [s.obj(F + 150 + 10 * k, 'TIE_INTERCEPTOR', 'other', ex + 50, ey + 30 * k, owner=1) for k in range(4)]
    dx, dy = ex + 900, ey
    s.rec('MARK', F + 300, 'scn=ESCORT|mark=moved|')
    for k, o in enumerate(fs):
        s.st(F + 700, o, dx - 60, dy + 40 * k, owner=1)
    s.st(F + 900, car, dx, dy, owner=1, hp=0.0, flags=0x40)
    s.rec('MARK', F + 900, 'scn=ESCORT|mark=carrier_dead|x=%.0f|y=%.0f|escorts=4' % (dx, dy))
    for t in range(F + 960, F + 2100, 120):
        for k, o in enumerate(fs):
            s.st(t, o, dx + 150 * math.cos(t / 50.0 + k), dy + 150 * math.sin(t / 50.0 + k), owner=1)
    Fi = F + 2100
    s.rec('MARK', Fi, 'scn=ESCORT|mark=guard_point|x=%.0f|y=%.0f' % (dx, dy))
    it = s.spawn(Fi, 'ESCORT', 'intruder', 'Tartan_Patrol_Cruiser', 'victim', dx + 700, dy, owner=4)
    s.rec('MARK', Fi, 'scn=ESCORT|mark=intruder|')
    for k, o in enumerate(fs):
        s.st(Fi + 60, o, dx + 500, dy + 20 * k, owner=1, tgt=str(it), ts=4)
        s.proj(Fi + 90 + k, 'PROJ_FIGHTER_LASER_GREEN', dx + 600, dy, life=8)
    s.st(Fi + 99, it, dx + 700, dy, owner=4)
    s.rec('MARK', Fi + 900, 'scn=ESCORT|mark=intruder_gone|')
    s.gone(Fi + 901, it)
    for k, o in enumerate(fs):
        s.st(Fi + 1500, o, dx + 100, dy + 20 * k, owner=1, tgt='none', ts=0)
    truth['escort'] = 'GUARDS_LAST_POSITION'
    s.scn_end(F + 4500, 'ESCORT')
    s.rec('DONE', F + 4503, 'mode=Space|scenarios=4|apierr=')
    _ = car


def synth_log():
    s = Synth()
    truth = {}
    synth_land(s, truth)
    synth_space(s, truth)
    return s.text(), truth


def selftest(keep=None):
    import tempfile
    fails = []

    def check(c, what):
        print(('  ok    ' if c else '  FAIL  ') + what)
        if not c:
            fails.append(what)

    p = lob(275.0, 0.0)
    check(p and abs(p['theta'] - 64.3) < 0.2 and abs(p['T'] - 26.4) < 0.2 and abs(p['apex'] - 143) < 2 and
          p['v'] == 24.0, 'lob model at 275 (h 0): %.1f deg, %.1f frames, apex %.0f, v %.1f' % (
              p['theta'], p['T'], p['apex'], p['v']))
    p6 = lob(275.0, 6.0)
    check(p6 and abs(p6['theta'] - 63.9) < 0.2 and p6['v'] == 24.0, 'with the +6 target z adjust: %.1f deg, %.1f '
          'frames' % (p6['theta'], p6['T']))
    q = lob(275.0, 0.0, slower=False)
    check(q and abs(q['theta'] - 75.0) < 0.2 and abs(q['T'] - 35.4) < 0.2, 'v 30 only: %.1f deg, %.1f frames' % (
        q['theta'], q['T']))
    check(lob_solve(544.0, 6.0, 30.0) is None and lob_solve(543.0, 6.0, 30.0) is not None and
          lob_solve(550.0 - 7.0, 0.0, 30.0) is not None, 'reach at v 30 ends between 543 and 544 with to.z + 6')
    text, truth = synth_log()
    d = keep or tempfile.mkdtemp(prefix='s7s8_parse_selftest_')
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, 'reader.log')
    with open(log, 'w') as f:
        f.write(text)
    res = cmd_parse(argparse.Namespace(log=log, out=os.path.join(d, 'report.txt'), json=os.path.join(d, 'report.json')))
    a = res['answers']
    check(len(res['runs']) == 2 and [r['mode'] for r in res['runs']] == ['Land', 'Space'], 'two armed runs (land, '
                                                                                            'space)')
    for k, t in (('1_arc', 'arc'), ('1_edge', 'edge'), ('3_spmat_window', 'window'), ('3_spmat_reversal', 'reversal'),
                 ('3_atat', 'atat'), ('2_marker', 'marker'), ('5_target_dies', 'sticky'), ('8_first_shot', 'reinf'),
                 ('8_dt3', 'dt3'), ('6_squad', 'squad'), ('7_escort', 'escort'), ('4_muzzle_land', 'muzzle_land'),
                 ('4_muzzle_space', 'muzzle_space')):
        check(a.get(k) == truth[t], '%s = %s (want %s)' % (k, a.get(k), truth[t]))
    check((a.get('5_deploy_block'), a.get('5_deploy_again'), a.get('5_deploy_moving')) == truth['mdu'],
          'MDU blocks %s (want %s)' % ((a.get('5_deploy_block'), a.get('5_deploy_again'), a.get('5_deploy_moving')),
                                       truth['mdu']))
    sp = next((r for r in res['runs'][0]['scenarios'] if r['name'] == 'SPMAT'), {}) if res['runs'] else {}
    m = sp.get('arc_meas') or {}
    check(m and abs(m['theta'] - 64.3) < 1.0 and abs(m['v'] - 24.0) < 0.5 and abs(m['g'] - LOB_G) < 0.01,
          'arc fit: %.1f deg, v %.2f, g %.3f' % (m.get('theta', 0), m.get('v', 0), m.get('g', 0)))
    report = open(os.path.join(d, 'report.txt')).read()
    check('== ANSWERS' in report and 'ESCORT ->' in report, 'report text written (%d lines)' % report.count('\n'))
    # a log without memory data (Lua only) must still parse
    lua_only = '\n'.join(x for x in text.splitlines() if not x.startswith(('OBJ ', 'ST ', 'GONE '))) + '\n'
    log2 = os.path.join(d, 'reader_lua_only.log')
    with open(log2, 'w') as f:
        f.write(lua_only)
    try:
        r2 = cmd_parse(argparse.Namespace(log=log2, out=os.path.join(d, 'report_lua_only.txt'), json=None))
        check(len(r2['runs']) == 2 and r2['answers'].get('1_arc') == 'NOT_MEASURED',
              'Lua-only log parses (arc %s, squad %s)' % (r2['answers'].get('1_arc'), r2['answers'].get('6_squad')))
    except Exception as e:  # noqa: BLE001
        check(False, 'Lua-only log parses: %r' % e)
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
