#!/usr/bin/env python3
"""build_s78n_t14_mod.py - builds the s78n t14 land-squad formation probe mod (Mods\\S78NT14Probe) from the install.

  build_s78n_t14_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]
                        [--map _MP_LAND_GEONOSIS] [--reps N] [--config-out FILE] [--map-info]

Retail check of the land-squad formation rules the port took from docs/re/FIGHTER_FLYING.md sec. 7 - 7.4 (roadmap
s78n t03 / t09 / t14): one land skirmish, human EMPIRE vs AI REBEL. The Rebel AI's free store spawns, one squad at a
time, a Rows squad (Imperial_Medium_Stormtrooper_Squad: one Stormtrooper_Team of 9 Squad_Stormtrooper) and a
Dartboard squad (Rebel_Mini_Infantry_Squad: one Rebel_Trooper_Team of 5 Squad_Rebel_Trooper), both for the human
player, on a straight, dry, open corridor far from both bases, and walks each container through four legs:
  leg 1  600 straight ahead (from the spawn: the first move),
  leg 2  200 on the same heading (a start with no turn: the start delays show; no re-lay, dot > 0.99),
  leg 3  400 turned 30 deg (a re-lay: > 8.11 deg),
  leg 4  200 turned 5 deg more (no re-lay: < 8.11 deg from the last laid heading),
holding still between legs. Every free-store poll (every 3 frames) the container's and every member's XY position
are recorded. s78n_t14_analyse.py turns them into (Q1) member lag behind the slots while marching and the close-up
after the stop, (Q2) the members' start frames, (Q3) the slot pattern (Rows grid / Dartboard scatter, re-lay or not).

Map: the default is _MP_LAND_GEONOSIS (flat; the corridor search finds a 1400-unit path about 2400 from both
starts). Dagobah (the earlier kits' map) has no corridor this long away from the bases (best: 169 from a start).
The corridor: cells of Clear passability, a dry 140 x 140 square around every point, no water within 150, no map
object within 40, inside the playable box; the start and heading maximise the smallest distance to a start.
Files of the mod (MODPATH=Mods\\S78NT14Probe):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   shipped script + generated SN_CFG + s78n_t14_probe.lua
  Data\\Text\\MasterTextFile_English.dat               shipped texts + the TEXT_S78N_T14_* cue strings
  S78NT14_PROBE_MARKER.txt                            marker: cleanup deletes the folder only when it is there
No XML is changed. Nothing from the game is stored in the repository.
"""
import argparse
import json
import math
import os
import re
import shutil
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
ROOT = os.path.normpath(os.path.join(PROBE_MOD, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
sys.path.insert(0, os.path.join(PROBE_MOD, 'oi_abilities'))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'S78NT14Probe'
MARKER = 'S78NT14_PROBE_MARKER.txt'
DEFAULT_GAME = ted_owners.DEFAULT_GAME
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
GAMECONSTANTS = 'DATA\\XML\\GAMECONSTANTS.XML'
FOC_ZIP = os.path.join(ROOT, 'extracted', 'foc_data_text.zip')
DEFAULT_MAP = '_MP_LAND_GEONOSIS'
# [offline only] the corridor the search found on the install on 2026-11-10 (start, heading, ground z)
FALLBACK_CORRIDOR = {'_MP_LAND_GEONOSIS': {'x': 2000.0, 'y': 800.0, 'z': -34.7, 'theta0': 165.0, 'far': 2374.0}}

# The legs: turn (deg, from the previous leg's heading) and length. Frames are mode frames (30 per second).
LEGS = [
    {'name': 'first_600', 'turn': 0.0, 'len': 600.0},
    {'name': 'straight_200', 'turn': 0.0, 'len': 200.0},
    {'name': 'turn30_400', 'turn': 30.0, 'len': 400.0},
    {'name': 'turn5_200', 'turn': 5.0, 'len': 200.0},
]

SQUADS = {
    'rows': {'company': 'Imperial_Medium_Stormtrooper_Squad', 'alts': ['Imperial_Stormtrooper_Squad'],
             'member': 'Squad_Stormtrooper', 'team': 'Stormtrooper_Team'},
    'dart': {'company': 'Rebel_Mini_Infantry_Squad', 'alts': ['Rebel_Infantry_Squad'],
             'member': 'Squad_Rebel_Trooper', 'team': 'Rebel_Trooper_Team'},
}

CFG = {
    'session': 's78n_t14',
    'ai_faction': 'REBEL',       # the AI whose free store runs the probe
    'human_faction': 'Empire',   # the driver's side; the squads are spawned for this player
    'sq': {
        'start_frame': 450,      # 15 s into the battle
        'ident_wait': 15,        # frames after the spawn before the members are read
        'settle': 180,           # frames after the read before leg 1 (members walk from the drop to their slots)
        'hold': 120,             # frames the container stands still after a leg before the next order
        'still_polls': 3,        # the container counts as stopped after this many polls without moving (> 0.02)
        'moved_min': 2.0,        # the container counts as started when it is this far from its leg start
        'reorder_wait': 90,      # frames without a container start before the order is repeated through a member
        'leg_max': 1200,         # frames after an order before a leg is given up (LEGTO)
        'gap': 30,               # frames between trials
        'invuln': True,          # Make_Invulnerable(true) on the members and the container
        'nofire': False,         # Prevent_All_Fire(true) on the members (off: it may touch more than firing)
        'camera': True,          # Point_Camera_At(position) at each trial and leg start (framing only)
        'owner': 'human',        # 'human' (Empire) or 'ai' (the Rebel AI, then Prevent_AI_Usage too)
    },
}

TEXTS = [
    ('TEXT_S78N_T14_ARMED', 'S78N T14 PROBE: squad formation trials armed. Give no orders, do not pause. About 4 '
                            'minutes.'),
    ('TEXT_S78N_T14_DONE', 'S78N T14 PROBE: squad formation trials recorded. Esc > Quit, then quit the game.'),
]


class Source:
    """Shipped files from the install (GameFiles) or from a text zip (offline check: no .dat, no .ted)."""

    def __init__(self, game, source_zip=None):
        self.zip = zipfile.ZipFile(source_zip) if source_zip else None
        self.names = {n.upper().replace('/', '\\'): n for n in self.zip.namelist()} if self.zip else None
        self.gf = None if self.zip else ted_owners.GameFiles(game)

    def read(self, name):
        if self.zip:
            n = self.names.get(name.upper())
            return (self.zip.read(n), 'zip:' + n) if n else (None, None)
        return self.gf.read(name)

    def need(self, name):
        data, src = self.read(name)
        if data is None:
            raise SystemExit('shipped %s not found' % name)
        return data, src


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n').encode('latin-1')


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def prepare(out):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an s78n t14 probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER), b's78n t14 probe mod (tools/probe_mod/s78n_t14_squads); removed by '
                                     b'cleanup_s78n_t14.cmd\r\n')


# ---- XML values -----------------------------------------------------------------------------------------------------

def _xml_texts(zpath=FOC_ZIP):
    with zipfile.ZipFile(zpath) as z:
        return {n: z.read(n).decode('latin-1') for n in z.namelist() if n.lower().endswith('.xml')}


def _type_body(texts, name):
    for t in texts:
        m = re.search(r'<(\w+)\s+Name="%s"\s*>(.*?)</\1\s*>' % re.escape(name), t, re.S | re.I)
        if m:
            return m.group(2)
    return None


def _tag(texts, name, tag, depth=0):
    """A tag's text on the type or the first Variant_Of_Existing_Type ancestor that has it."""
    body = _type_body(texts, name)
    if body is None or depth > 6:
        return None
    m = re.search(r'<%s>\s*([^<]*?)\s*</%s>' % (tag, tag), body, re.I)
    if m:
        return m.group(1)
    var = re.search(r'<Variant_Of_Existing_Type>\s*([^<\s]+)\s*<', body)
    return _tag(texts, var.group(1), tag, depth + 1) if var else None


def _num(v, default=None):
    if v is None:
        return default
    m = re.match(r'\s*([-+]?[\d.]+)', v)
    return float(m.group(1)) if m else default


def xml_values(zpath=FOC_ZIP):
    """Per squad: the company's member list, the container's arrangement / speed / turn rate / acceleration /
    FormationSpacing, the member's half-extents (Custom_Hard x Scale_Factor), FormationSpacing, speed, acceleration,
    MaxJiggleDistance; GameConstants Max_Move_Frame_Delay and Spread_Out_Spacing_Coefficient."""
    texts = list(_xml_texts(zpath).values())
    out = {}
    for key, sq in SQUADS.items():
        comp = _type_body(texts, sq['company'])
        if comp is None:
            raise SystemExit('company %s not found' % sq['company'])
        units = _tag(texts, sq['company'], 'Company_Units') or ''
        units = [u.strip() for u in units.split(',') if u.strip()]
        team = (_tag(texts, sq['company'], 'Create_Team_Type') or '').strip()
        mem = sq['member']
        hx = _num(_tag(texts, mem, 'Custom_Hard_XExtent'), 0.0)
        hy = _num(_tag(texts, mem, 'Custom_Hard_YExtent'), 0.0)
        sc = _num(_tag(texts, mem, 'Scale_Factor'), 1.0)
        out[key] = {
            'company': sq['company'], 'team': team or sq['team'], 'member': mem, 'n': len(units),
            'units': sorted(set(units)), 'max_squad': _num(_tag(texts, sq['company'], 'Max_Squad_Size')),
            'arrangement': (_tag(texts, team or sq['team'], 'ContainerArrangement') or 'None').strip(),
            'c_speed': _num(_tag(texts, team or sq['team'], 'Max_Speed')),
            'c_turn': _num(_tag(texts, team or sq['team'], 'Max_Rate_Of_Turn')),
            'c_acc': _num(_tag(texts, team or sq['team'], 'Override_Acceleration')),
            'c_spacing': _num(_tag(texts, team or sq['team'], 'FormationSpacing')),
            'm_speed': _num(_tag(texts, mem, 'Max_Speed')),
            'm_turn': _num(_tag(texts, mem, 'Max_Rate_Of_Turn')),
            'm_acc': _num(_tag(texts, mem, 'OverrideAcceleration')),
            'm_spacing': _num(_tag(texts, mem, 'FormationSpacing')),
            'm_jiggle': _num(_tag(texts, mem, 'MaxJiggleDistance'), 0.0),
            'hx': round(hx * sc, 4), 'hy': round(hy * sc, 4), 'hard': [hx, hy], 'scale': sc,
        }
    alltext = ''.join(texts)
    m = re.search(r'<Max_Move_Frame_Delay>\s*([\d.]+)', alltext)
    out['max_move_frame_delay'] = float(m.group(1)) if m else 6.0
    m = re.search(r'<Spread_Out_Spacing_Coefficient>\s*([\d.]+)', alltext)
    out['spread_coef'] = float(m.group(1)) if m else 1.8
    return out


# ---- the corridor ---------------------------------------------------------------------------------------------------

def leg_dirs(theta0):
    """Unit vectors of the legs (cumulative turns from theta0, degrees, counter-clockwise from +x)."""
    out = []
    ang = theta0
    for lg in LEGS:
        ang += lg['turn']
        r = math.radians(ang)
        out.append({'name': lg['name'], 'turn': lg['turn'], 'len': lg['len'], 'deg': round(ang % 360.0, 3),
                    'dx': round(math.cos(r), 6), 'dy': round(math.sin(r), 6)})
    return out


def corridor(game, map_name, clear=40.0, half=3, water_min=150.0, step=4):
    """(site {x, y, z}, theta0, note) of the straight 4-leg path farthest from the starts, or raises."""
    import build_oi_mod as O
    t = O.load_terrain(game, map_name)
    if t is None:
        raise ValueError('map not readable')
    g = O.Grid(t)
    cell = O.CELL
    data, _src = ted_owners.GameFiles(game).read('DATA\\ART\\MAPS\\%s.TED' % map_name.upper())
    pl, _ = ted_owners.parse_placements(data)
    hb = {}
    for _i, _o, _t, _w, (x, y, _z) in pl:
        hb.setdefault((int(x // 100), int(y // 100)), []).append((x, y))

    def near_obj(x, y):
        cx, cy = int(x // 100), int(y // 100)
        for i in (-1, 0, 1):
            for j in (-1, 0, 1):
                for (a, b) in hb.get((cx + i, cy + j), ()):
                    if (a - x) ** 2 + (b - y) ** 2 < clear * clear:
                        return True
        return False

    W, H = g.W, g.H
    ok = [[False] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if not g.in_box(x, y, 200.0) or not g.square_all(g.sat_land, x, y, half):
                continue
            if g.wh is not None and g.dist_water[y][x] * cell < water_min:
                continue
            if clear > 0 and near_obj(x * cell, y * cell):
                continue
            ok[y][x] = True
    cands = sorted(((g.far(x, y), x, y) for y in range(0, H, step) for x in range(0, W, step) if ok[y][x]),
                   reverse=True)
    best = None
    for far0, x, y in cands:
        if best and far0 <= best[0]:
            break
        for k in range(24):
            th = k * 15.0
            px, py = x * cell, y * cell
            good, mind, hs = True, far0, []
            for lg in leg_dirs(th):
                n = int(lg['len'] // 10)
                for i in range(1, n + 1):
                    cx = int(round((px + lg['dx'] * 10 * i) / cell))
                    cy = int(round((py + lg['dy'] * 10 * i) / cell))
                    if not (0 <= cx < W and 0 <= cy < H) or not ok[cy][cx]:
                        good = False
                        break
                    mind = min(mind, g.far(cx, cy))
                    hs.append(t['h'][cy * W + cx])
                if not good:
                    break
                px, py = px + lg['dx'] * lg['len'], py + lg['dy'] * lg['len']
            if good and (best is None or mind > best[0]):
                best = (mind, x, y, th, min(hs), max(hs))
    if best is None:
        raise ValueError('no corridor (clearance %.0f to map objects)' % clear)
    mind, x, y, th, h0, h1 = best
    p = g.point(x, y)
    note = '%s (%s): start (%.0f, %.0f), heading %.0f deg, path %.0f from the nearest start, ground z %.1f..%.1f, ' \
           'map objects >= %.0f away' % (map_name, t['source'], p['x'], p['y'], th, mind, h0, h1, clear)
    return {'x': p['x'], 'y': p['y'], 'z': p['z']}, th, round(mind), note


def site_for(game, map_name, offline):
    if offline:
        fb = FALLBACK_CORRIDOR.get(map_name.upper())
        if fb is None:
            raise SystemExit('no offline corridor for %s' % map_name)
        return {'x': fb['x'], 'y': fb['y'], 'z': fb['z']}, fb['theta0'], fb['far'], 'fallback (offline)'
    last = None
    for clear in (40.0, 25.0, 0.0):
        try:
            return corridor(game, map_name, clear=clear)
        except (OSError, ValueError, KeyError, StopIteration, struct.error, ImportError) as e:
            last = e
    fb = FALLBACK_CORRIDOR.get(map_name.upper())
    if fb is None:
        raise SystemExit('no corridor on %s: %s' % (map_name, last))
    return {'x': fb['x'], 'y': fb['y'], 'z': fb['z']}, fb['theta0'], fb['far'], 'fallback (%s)' % last


# ---- Lua config -----------------------------------------------------------------------------------------------------

def lua_value(v, indent='\t'):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, float)):
        return repr(float(v)) if isinstance(v, float) else str(v)
    if isinstance(v, str):
        return '"%s"' % v.replace('\\', '\\\\').replace('"', '\\"')
    if isinstance(v, list):
        return '{ ' + ', '.join(lua_value(x, indent) for x in v) + ' }'
    if isinstance(v, dict):
        inner = indent + '\t'
        items = []
        for k in sorted(v):
            key = k if re.match(r'^[A-Za-z_]\w*$', k) else '["%s"]' % k
            items.append('%s%s = %s,' % (inner, key, lua_value(v[k], inner)))
        return '{\n' + '\n'.join(items) + '\n' + indent + '}'
    if v is None:
        return 'nil'
    raise TypeError(type(v))


LUA_SKIP = ('files', 'freestore_source', 'site_note', 'map', 'xml')


def lua_config(cfg):
    lua_cfg = {k: v for k, v in cfg.items() if k not in LUA_SKIP}
    return '-- generated by build_s78n_t14_mod.py\nSN_CFG = %s\n' % lua_value(lua_cfg, '')


def est_frames(cfg):
    """Rough battle time of the trials: Max_Speed x 1.57 per frame (s78m: Tyber's Max_Speed 1 walked 1.567 per
    frame), plus the settle / hold / gap times."""
    sq = cfg['sq']
    tot = sq['start_frame']
    for key in sq['order']:
        v = (cfg['xml'][key]['c_speed'] or 1.0) * 1.57
        tot += sq['ident_wait'] + sq['settle'] + sq['gap'] + 30
        for lg in sq['legs']:
            tot += lg['len'] / v + 25 + sq['hold'] + 3 * sq['still_polls']
    return int(tot)


def config(game, offline, map_name=DEFAULT_MAP, reps=2):
    cfg = json.loads(json.dumps(CFG))
    xv = xml_values()
    cfg['xml'] = xv
    sq = cfg['sq']
    site, theta0, far, note = site_for(game, map_name, offline)
    sq['site'] = {'x': float(site['x']), 'y': float(site['y']), 'z': float(site['z'])}
    sq['theta0'] = float(theta0)
    sq['far'] = float(far)
    sq['legs'] = leg_dirs(theta0)
    sq['squads'] = {k: dict(v, arrangement=xv[k]['arrangement']) for k, v in SQUADS.items()}
    sq['order'] = ['rows', 'dart'] * max(1, int(reps))
    sq['map'] = map_name.upper()
    cfg['site_note'] = note
    cfg['map'] = map_name.upper()
    cfg['est_frames'] = est_frames(cfg)
    return cfg


def freestore(src, cfg):
    shipped, fs_src = src.need(FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    probe = open(os.path.join(HERE, 's78n_t14_probe.lua'), encoding='ascii').read()
    return crlf(text + '\n\n' + lua_config(cfg) + '\n' + probe), fs_src


def texts_dat(src, pairs):
    mt, _ = src.need(MASTERTEXT)
    tmp = os.path.join(os.environ.get('TEMP', HERE), 's78n_t14_mt_%d.tmp' % os.getpid())
    with open(tmp, 'wb') as f:
        f.write(mt)
    try:
        keys = {k for k, _ in pairs}
        base = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    finally:
        os.remove(tmp)
    return dat_bytes(base + list(pairs))


def build(game, out, source_zip=None, no_text=False, map_name=DEFAULT_MAP, reps=2):
    """Build Mods\\S78NT14Probe into `out`, reading shipped files from `game` or the zip. Returns the config."""
    src = Source(game, source_zip)
    cfg = config(game, src.zip is not None, map_name, reps)
    prepare(out)
    files = []

    def put(rel, data):
        write(os.path.join(out, rel), data)
        files.append(rel)

    fs, fs_src = freestore(src, cfg)
    put('Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua', fs)
    if not no_text:
        put('Data\\Text\\MasterTextFile_English.dat', texts_dat(src, TEXTS))
    cfg['files'] = files
    cfg['freestore_source'] = fs_src
    return cfg


def describe(cfg):
    sq = cfg['sq']
    out = ['map %s, corridor: %s' % (cfg['map'], cfg['site_note']),
           'trials: %s; legs from the spawn point (%.0f, %.0f):' % (' '.join(sq['order']), sq['site']['x'],
                                                                     sq['site']['y'])]
    for i, lg in enumerate(sq['legs']):
        out.append('  leg %d %-13s turn %4.1f deg -> heading %6.1f deg, %4.0f units' % (
            i + 1, lg['name'], lg['turn'], lg['deg'], lg['len']))
    for key in ('rows', 'dart'):
        x = cfg['xml'][key]
        out.append('  %s: %s -> %s (%s) x %d %s; container Max_Speed %s, turn %s, acc %s, FormationSpacing %s; member '
                   'Max_Speed %s, acc %s, FormationSpacing %s, half-extents %.2f x %.2f, MaxJiggleDistance %s' % (
                       key, x['company'], x['team'], x['arrangement'], x['n'], x['member'], x['c_speed'], x['c_turn'],
                       x['c_acc'], x['c_spacing'], x['m_speed'], x['m_acc'], x['m_spacing'], x['hx'], x['hy'],
                       x['m_jiggle']))
    out.append('  Max_Move_Frame_Delay %s; estimated battle time %.1f minutes' % (
        cfg['xml']['max_move_frame_delay'], cfg['est_frames'] / 1800.0))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--map', default=DEFAULT_MAP)
    ap.add_argument('--reps', type=int, default=2)
    ap.add_argument('--config-out')
    ap.add_argument('--map-info', action='store_true', help='print the corridor and the trial plan, build nothing')
    a = ap.parse_args()
    if a.map_info:
        cfg = config(a.game, False, a.map, a.reps)
        print('\n'.join(describe(cfg)))
        return
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.game, out, a.source_zip, a.no_text, a.map, a.reps)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s: %s' % (out, ', '.join(cfg['files'])))
    print('\n'.join(describe(cfg)))


if __name__ == '__main__':
    main()
