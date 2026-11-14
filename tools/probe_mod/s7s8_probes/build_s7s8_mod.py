#!/usr/bin/env python3
"""build_s7s8_mod.py - builds the S7/S8 probe mod (Mods\\S7S8Probe) from the installed game's own files.

  build_s7s8_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip]
                    [--land-map _MP_LAND_GEONOSIS] [--space-map _MP_SPACE_KUAT] [--faction REBEL]
                    [--config-out s7s8_config.json]
  build_s7s8_mod.py --map-info [--land-map NAME] [--space-map NAME]   prints the chosen points and exits
  build_s7s8_mod.py --rank-land-maps                                  scores every MP land map for the test site

What it writes (nothing from the game is stored in the repository):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   the shipped script, unchanged, + a generated S78_CFG table
                                                       (test sites of both maps) + s7s8_hook.lua appended
  Data\\Text\\MasterTextFile_English.dat                the shipped text + the probe's Game_Message lines (only when
                                                       the install's text file is readable; else no text file)
  S7S8_PROBE_MARKER.txt                                lets cleanup (s7s8_cleanup.py) recognise the folder as ours
and, with --config-out, the same sites as JSON for s7s8_reader.py / s7s8_parse.py.

Sites come from the map files (.ted, read through tools/probe_mod/common/ted_owners.py):
  land  one origin O and direction u (a grid axis) such that the strip O + [0, 580] u, |side| <= 60 is Clear, dry and
        flat (every vertex within 4 units of O's height: the SPMAT range-edge test needs the target at the shooter's
        height) and the wider rectangle O + [-220, 700] u, |side| <= 200 is Clear and dry; at least 900 from every
        start position and 300 inside the playable box. Every land scenario runs there, one after the other.
  space the midpoint of the two start positions moved 1800 to each side of the start line ('sq', 'esc') and the
        midpoint itself ('muz'); u = the start line direction. Clamped 600 inside the playable box.
Without a readable map the config says points_ok = false for that mode and the hook falls back to positions next to
the AI's first unit (the land SPMAT edge test then cannot answer item 1).
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
import ted_owners  # noqa: E402

MOD_NAME = 'S7S8Probe'
MARKER = 'S7S8_PROBE_MARKER.txt'
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
HOOK = os.path.join(HERE, 's7s8_hook.lua')
DEFAULT_LAND_MAP = '_MP_LAND_GEONOSIS'
DEFAULT_SPACE_MAP = '_MP_SPACE_KUAT'
DEFAULT_FACTION = 'REBEL'
HUMAN_FACTION = 'Empire'     # Find_Player name of the human's faction
START_FRAME = 450          # scenarios start 15 s into each battle

CLEAR, WATER = 0x1, 0x20
CELL = 20.0
HEIGHT_SCALE = 0.048828125

# Unit types of the scenarios and the projectiles their weapons fire (vanilla FoC XML, 2026-11-02).
PROJ_OF = {
    'SPMAT_WALKER': ['PROJ_GROUND_SPMAT_GRENADE', 'PROJ_BLASTER_CANNON_GREEN'],
    'AT_AT_WALKER': ['PROJ_GROUND_LASER_CANNON_RED_ATAT'],
    'DARK_TROOPER_PHASEIII': ['PROJ_GROUND_LASER_CANNON_RED_DT3', 'PROJ_DARK_TROOPER_MISSILE'],
    'MZ8_PULSE_CANNON_TANK': ['PROJ_MOBILE_PULSE_CANNON_BLAST'],
    'REBEL_INFILTRATOR': ['PROJ_STICKY_BOMB'],
    'UNDERWORLD_SABOTEUR': ['PROJ_REMOTE_BOMB'],
}
# Muzzle-bone types of roadmap S8 t12 (land models without MUZZLEA_00; WEAPON only in SpaceBehavior).
MUZZLE_TYPES = ['Moldy_Crow', 'Moldy_Crow_Landing', 'Luke_Jedi_Xwing_Landing', 'Underworld_Infantry_Droid_Shuttle',
                'Underworld_Infantry_Droid_Shuttle_Landing', 'Tyber_Zann_Boarding_Shuttle_Prologue',
                'UM07_Cloud_City_Flyover', 'UM07_CloudCity_Refinery']
ATTACKER_TYPES = ['SPMAT_WALKER', 'AT_AT_WALKER', 'DARK_TROOPER_PHASEIII', 'MZ8_PULSE_CANNON_TANK',
                  'REBEL_MOBILE_DEFENSE_UNIT', 'REBEL_INFILTRATOR', 'REBEL_X-WING_SQUADRON', 'X-WING',
                  'STAR_DESTROYER', 'GROUND_MAGNEPULSE_CANNON', 'SKIRMISH_GROUND_MAGNEPULSE_CANNON_E',
                  'GARRISON_BUNKER_REBEL', 'UNDERWORLD_MERC', 'UNDERWORLD_MERC_TEAM', 'POD_WALKER',
                  'TIE_FIGHTER_SQUADRON', 'TIE_FIGHTER', 'DARK_TROOPER_PHASEII', 'BROADSIDE_CLASS_CRUISER'] + \
    [t.upper() for t in MUZZLE_TYPES]
# Scenario order (s78f 2026-11-05: ATAT last; the first run's land script stopped right after it started). Override
# with ALAMO_S78_LAND / ALAMO_S78_SPACE (comma-separated scenario names, s7s8_hook.lua S78_SCN_BY_NAME).
LAND_LIST = ['SPMAT', 'IDLE47', 'MAGNEPULSE', 'MDU', 'DT3', 'REINF', 'MUZZLE_LAND', 'BUNKER', 'STICKY', 'ATAT']
SPACE_LIST = ['MUZZLE_SPACE', 'SQUAD', 'ESCORT', 'GUARD_SHIFT']
VICTIM_TYPES = ['T4B_TANK', 'TARTAN_PATROL_CRUISER']
HUMAN_TYPES = ['UNDERWORLD_SABOTEUR', 'UNDERWORLD_SABOTEUR_TEAM']
# Watched wherever they appear (no distance filter): the ground marker of a Remote_Bomb ground order and the bomb.
ALWAYS_TYPES = ['DUMMY_BARRAGE_TARGET', 'PROJ_REMOTE_BOMB'] + HUMAN_TYPES

# Game_Message lines (TEXT_S78_*); shown only when the text file could be built.
TEXTS = [
    ('TEXT_S78_ARMED_LAND', 'S7S8 PROBE: land scenarios armed. Give no orders except to the Saboteur.'),
    ('TEXT_S78_ARMED_SPACE', 'S7S8 PROBE: space scenarios armed. Give no orders.'),
    ('TEXT_S78_HUMAN_GO', 'S7S8 PROBE: your Saboteur is here (camera). Do steps H1, H2, H3 of the README now.'),
    ('TEXT_S78_LAND_DONE', 'S7S8 PROBE: land scenarios done. Finish H1-H3 if needed, then Esc > Quit.'),
    ('TEXT_S78_SPACE_DONE', 'S7S8 PROBE: space scenarios done. Esc > Quit.'),
    # s78g t02 (GUARD_HAND, space, orders by hand)
    ('TEXT_S78_GUARD_FIND', 'S7S8 PROBE: click the CENTRE of the minimap. Your probe TIE squadron and a Broadside '
                            'are there. Select ONLY the TIE squadron.'),
    ('TEXT_S78_GUARD_GO', 'S7S8 PROBE: GUARD NOW. With the TIE squadron selected, right-click the moving Broadside.'),
    ('TEXT_S78_SHIFT_GO', 'S7S8 PROBE: SHIFT WAYPOINT NOW. Keep the squadron selected; hold Shift and right-click '
                          'empty space about a third of the screen away from the Broadside.'),
    ('TEXT_S78_GUARD_GO2', 'S7S8 PROBE: GUARD AGAIN. Select the TIE squadron, right-click the Broadside.'),
    ('TEXT_S78_SHIFT_GO2', 'S7S8 PROBE: SHIFT WAYPOINT AGAIN. Shift + right-click empty space away from the '
                           'Broadside.'),
    ('TEXT_S78_G_STOP', 'S7S8 PROBE: the Broadside stops now. Give no more orders.'),
]


# ---- shipped files -------------------------------------------------------------------------------------------------

read_shipped = ted_owners.read_shipped


def ted_bytes(game, map_name):
    n = map_name.upper()
    if not n.endswith('.TED'):
        n += '.TED'
    if '\\' not in n:
        n = 'DATA\\ART\\MAPS\\' + n
    if not os.path.isdir(game):
        return None, None, n
    data, src = ted_owners.GameFiles(game).read(n)
    return data, src, n


def ted_header(data):
    """(top-level chunks, start positions [(x, y)], world chunk payload or None, playable box or None)."""
    top = ted_owners.chunks(data)
    starts = []
    for cid, kids, p in top:
        if cid == 3 and not kids and len(p) >= 4:
            c = struct.unpack_from('<I', p, 0)[0]
            starts = [struct.unpack_from('<2f', p, 4 + 8 * i) for i in range(c) if 4 + 8 * i + 8 <= len(p)]
    world = next((p for cid, kids, p in top if cid == 1 and kids), None)
    box = None
    if world is not None:
        for cid, kids, p in ted_owners.chunks(world):
            if cid == 0x103 and not kids:
                m = ted_owners.minis(p)
                if 5 in m and 6 in m and len(m[5]) >= 12 and len(m[6]) >= 12:
                    box = (struct.unpack('<3f', m[5][:12]), struct.unpack('<3f', m[6][:12]))
    return top, starts, world, box


# ---- land site -----------------------------------------------------------------------------------------------------

def load_land(game, map_name):
    """{W, H, h (row-major, world z), pas (column-major x*H+y), wh, wet, starts, box, name, source} or None."""
    data, src, n = ted_bytes(game, map_name)
    if data is None:
        return None
    _top, starts, world, box = ted_header(data)
    t = {'name': n, 'source': src, 'starts': starts, 'box': box, 'wh': None, 'wtype': 0}
    verts = pas = None
    for cid, kids, p in ted_owners.chunks(world):
        if cid == 0x101:
            for c2, _k2, p2 in ted_owners.chunks(p):
                if c2 == 0:
                    m = ted_owners.minis(p2)
                    t['W'], t['H'] = ted_owners.u32(m[0]), ted_owners.u32(m[1])
                    if 0x15 in m:
                        t['wh'] = struct.unpack('<f', m[0x15][:4])[0]
                    t['wtype'] = ted_owners.u32(m.get(0x16)) or 0
                elif c2 == 5:
                    verts = p2
        elif cid == 0x10A:
            pas = p
    if 'W' not in t:
        return None
    W, H = t['W'], t['H']
    t['h'] = [struct.unpack_from('<h', verts, 4 * i)[0] * HEIGHT_SCALE if verts and 4 * i + 2 <= len(verts) else 0.0
              for i in range(W * H)]
    t['pas'] = [struct.unpack_from('<I', pas, 4 * i)[0] for i in range(W * H)] if pas else [CLEAR] * (W * H)
    t['wet'] = t['wtype'] == 1 and t['wh'] is not None
    return t


DIRS = ((1, 0), (0, 1), (-1, 0), (0, -1))


def ok_sat(t):
    """Summed-area table of Clear-and-dry vertices ([y][x], one extra row / column)."""
    W, H = t['W'], t['H']
    s = [[0] * (W + 1) for _ in range(H + 1)]
    for y in range(H):
        row = 0
        for x in range(W):
            ok = t['pas'][x * H + y] == CLEAR and not (t['wet'] and t['h'][y * W + x] <= t['wh'] + 4.0)
            row += 1 if ok else 0
            s[y + 1][x + 1] = s[y][x + 1] + row
    return s


def rect_ok(t, sat, x, y, d, a0, a1, s0, s1):
    """Every vertex of x + [a0, a1] u + [s0, s1] v is Clear and dry (u = d, v = u rotated +90 degrees)."""
    ux, uy = d
    vx, vy = -uy, ux
    xs = [x + a * ux + s * vx for a in (a0, a1) for s in (s0, s1)]
    ys = [y + a * uy + s * vy for a in (a0, a1) for s in (s0, s1)]
    X0, X1, Y0, Y1 = min(xs), max(xs), min(ys), max(ys)
    if X0 < 0 or Y0 < 0 or X1 >= t['W'] or Y1 >= t['H']:
        return False
    tot = sat[Y1 + 1][X1 + 1] - sat[Y0][X1 + 1] - sat[Y1 + 1][X0] + sat[Y0][X0]
    return tot == (X1 - X0 + 1) * (Y1 - Y0 + 1)


def land_site_score(t, sat, x, y, d):
    """None when (x, y, d) is not a valid site, else (min distance to a start, flatness range of the strip)."""
    W = t['W']
    ux, uy = d
    vx, vy = -uy, ux
    wx, wy = x * CELL, y * CELL
    far = min((math.hypot(wx - sx, wy - sy) for sx, sy in t['starts']), default=5000.0)
    if far < 900:
        return None
    box = t['box']
    if box:
        for a in (-11, 35):
            for s in (-10, 10):
                px, py = (x + a * ux + s * vx) * CELL, (y + a * uy + s * vy) * CELL
                if not (box[0][0] + 300 <= px <= box[1][0] - 300 and box[0][1] + 300 <= py <= box[1][1] - 300):
                    return None
    # strict strip 0..580 along, |side| <= 60 and wide rectangle -220..700 along, |side| <= 200: Clear and dry
    if not rect_ok(t, sat, x, y, d, 0, 29, -3, 3) or not rect_ok(t, sat, x, y, d, -11, 35, -10, 10):
        return None
    # the strict strip is flat: every vertex within 4 units of the origin's height
    h0 = t['h'][y * W + x]
    lo = hi = h0
    for a in range(0, 30):
        for s in range(-3, 4):
            h = t['h'][(y + a * uy + s * vy) * W + (x + a * ux + s * vx)]
            if abs(h - h0) > 4.0:
                return None
            lo, hi = min(lo, h), max(hi, h)
    return far, hi - lo


def pick_land_site(t):
    best = None
    sat = ok_sat(t)
    for y in range(0, t['H'], 2):
        for x in range(0, t['W'], 2):
            for d in DIRS:
                sc = land_site_score(t, sat, x, y, d)
                if sc is None:
                    continue
                key = (round(sc[1], 1), -sc[0])
                if best is None or key < best[0]:
                    best = (key, x, y, d, sc)
    if best is None:
        raise ValueError('no flat Clear site (580 x 120 strip in a 920 x 400 clear rectangle) on %s' % t['name'])
    _k, x, y, d, sc = best
    h = t['h'][y * t['W'] + x]
    return {'x': round(x * CELL, 1), 'y': round(y * CELL, 1), 'z': round(h, 2), 'ux': float(d[0]), 'uy': float(d[1]),
            'cell': [x, y], 'start_dist': round(sc[0], 1), 'flat_range': round(sc[1], 2)}


def land_config(game, map_name):
    cfg = {'map': map_name.upper(), 'points_ok': False, 'points': {}, 'starts': [], 'map_source': None, 'note': ''}
    try:
        t = load_land(game, map_name)
    except (OSError, ValueError, KeyError, StopIteration, struct.error) as e:
        cfg['note'] = 'map read failed: %s' % e
        return cfg
    if t is None:
        cfg['note'] = 'map %s not readable from %s (offline build?)' % (map_name, game)
        return cfg
    cfg['map_source'] = '%s (%s)' % (t['name'], t['source'])
    cfg['starts'] = [[round(a, 1), round(b, 1)] for a, b in t['starts']]
    try:
        cfg['points'] = {'site': pick_land_site(t)}
        cfg['points_ok'] = True
    except ValueError as e:
        cfg['note'] = str(e)
    return cfg


def rank_land_maps(game):
    gf = ted_owners.GameFiles(game)
    out = []
    for n in gf.map_names():
        short = n.split('\\')[-1][:-4]
        if not short.startswith('_MP_LAND_'):
            continue
        c = land_config(game, short)
        s = c['points'].get('site')
        out.append((short, s['flat_range'] if s else None, s['start_dist'] if s else None, c['note']))
    return out


# ---- space sites ---------------------------------------------------------------------------------------------------

def space_config(game, map_name):
    cfg = {'map': map_name.upper(), 'points_ok': False, 'points': {}, 'starts': [], 'map_source': None, 'note': ''}
    try:
        data, src, n = ted_bytes(game, map_name)
        if data is None:
            cfg['note'] = 'map %s not readable from %s (offline build?)' % (map_name, game)
            return cfg
        _top, starts, _world, box = ted_header(data)
    except (OSError, ValueError, KeyError, StopIteration, struct.error) as e:
        cfg['note'] = 'map read failed: %s' % e
        return cfg
    cfg['map_source'] = '%s (%s)' % (n, src)
    cfg['starts'] = [[round(a, 1), round(b, 1)] for a, b in starts]
    if len(starts) < 2:
        cfg['note'] = 'fewer than two start positions'
        return cfg
    (ax, ay), (bx, by) = starts[0], starts[1]
    cx, cy = (ax + bx) / 2.0, (ay + by) / 2.0
    L = math.hypot(bx - ax, by - ay) or 1.0
    wx, wy = (bx - ax) / L, (by - ay) / L
    vx, vy = -wy, wx

    def clamp(x, y):
        if box:
            x = min(max(x, box[0][0] + 600), box[1][0] - 600)
            y = min(max(y, box[0][1] + 600), box[1][1] - 600)
        return round(x, 1), round(y, 1)

    pts = {}
    for name, off in (('sq', 1800.0), ('esc', -1800.0), ('muz', 0.0)):
        x, y = clamp(cx + off * vx, cy + off * vy)
        pts[name] = {'x': x, 'y': y, 'z': 0.0, 'ux': round(wx, 4), 'uy': round(wy, 4)}
    cfg['points'] = pts
    cfg['points_ok'] = True
    cfg['box'] = [list(box[0]), list(box[1])] if box else None
    return cfg


# ---- config + Lua generation ---------------------------------------------------------------------------------------

def make_config(game, land_map, space_map, faction):
    return {'faction': faction.upper(), 'human_faction': HUMAN_FACTION, 'start_frame': START_FRAME,
            'land': land_config(game, land_map), 'space': space_config(game, space_map),
            'watch': {'attackers': ATTACKER_TYPES, 'victims': VICTIM_TYPES, 'human': HUMAN_TYPES,
                      'always': ALWAYS_TYPES},
            'proj_of': PROJ_OF, 'muzzle_types': MUZZLE_TYPES}


def lua_num(v):
    return 'nil' if v is None else repr(float(v))


def lua_points(mcfg):
    lines = []
    for k in sorted(mcfg['points']):
        p = mcfg['points'][k]
        lines.append('\t\t\t%s = { x = %s, y = %s, z = %s, ux = %s, uy = %s },' % (
            k, lua_num(p['x']), lua_num(p['y']), lua_num(p.get('z', 0.0)), lua_num(p.get('ux', 1.0)),
            lua_num(p.get('uy', 0.0))))
    return lines


def lua_config(cfg):
    lines = ['-- generated by build_s7s8_mod.py: test sites of the S7/S8 probe',
             'S78_CFG = {',
             '\tfaction = "%s",' % cfg['faction'],
             '\thuman_faction = "%s",' % cfg['human_faction'],
             '\tstart_frame = %d,' % cfg['start_frame'],
             '\ttext = %s,' % ('true' if cfg.get('text') else 'false'),
             # s78h IDLE47Q: also despawn the human's mobile units (all but the Saboteur); ALAMO_S78_QUIET_HUMAN=0 = off
             '\tquiet_human = %s,' % ('false' if os.environ.get('ALAMO_S78_QUIET_HUMAN', '1') == '0' else 'true')]
    for key, env, dflt in (('land_list', 'ALAMO_S78_LAND', LAND_LIST), ('space_list', 'ALAMO_S78_SPACE', SPACE_LIST)):
        names = [n.strip().upper() for n in os.environ.get(env, '').split(',') if n.strip()] or dflt
        lines.append('\t%s = { %s },' % (key, ', '.join('"%s"' % n for n in names)))
    for mode in ('land', 'space'):
        m = cfg[mode]
        lines += ['\t%s = {' % mode, '\t\tmap = "%s",' % m['map'],
                  '\t\tpoints_ok = %s,' % ('true' if m['points_ok'] else 'false'), '\t\tpts = {']
        lines += lua_points(m)
        lines += ['\t\t},', '\t},']
    lines += ['}', '']
    return '\n'.join(lines)


def freestore_text(game, cfg, source_zip=None):
    tail = lua_config(cfg) + '\n' + open(HOOK, encoding='ascii').read()
    return ted_owners.append_to_shipped(game, FREESTORE, tail, source_zip)


def text_file(game):
    """MasterTextFile_English.dat bytes = shipped pairs + TEXTS, or (None, why)."""
    try:
        from build_probe import dat_read, dat_bytes
    except ImportError as e:
        return None, 'build_probe import failed: %s' % e
    if not os.path.isdir(game):
        return None, 'no install'
    mt, src = ted_owners.GameFiles(game).read(MASTERTEXT)
    if mt is None:
        return None, 'MasterTextFile_English.dat not found'
    import tempfile
    fd, tmp = tempfile.mkstemp(suffix='.dat')
    os.close(fd)
    try:
        with open(tmp, 'wb') as f:
            f.write(mt)
        keys = {k for k, _ in TEXTS}
        pairs = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    finally:
        os.remove(tmp)
    return dat_bytes(pairs + TEXTS), src


def build(game, out, source_zip=None, land_map=DEFAULT_LAND_MAP, space_map=DEFAULT_SPACE_MAP,
          faction=DEFAULT_FACTION, config_out=None, with_text=True):
    cfg = make_config(game, land_map, space_map, faction)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an S7S8 probe folder' % out)
        shutil.rmtree(out)
    txt, tsrc = (text_file(game) if with_text else (None, 'disabled'))
    cfg['text'] = txt is not None
    cfg['text_source'] = tsrc
    fs, src = freestore_text(game, cfg, source_zip)
    p = os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(fs)
    if txt is not None:
        tp = os.path.join(out, 'Data', 'Text', 'MasterTextFile_English.dat')
        os.makedirs(os.path.dirname(tp), exist_ok=True)
        with open(tp, 'wb') as f:
            f.write(txt)
    with open(os.path.join(out, MARKER), 'wb') as f:
        f.write(b'S7/S8 probe mod (tools/probe_mod/s7s8_probes); removed by cleanup_s7s8.cmd\r\n')
    cfg['freestore_source'] = src
    if config_out:
        with open(config_out, 'w') as f:
            json.dump(cfg, f, indent=1, sort_keys=True)
    print('built %s (free store from %s; land %s points_ok=%s%s; space %s points_ok=%s%s; text=%s)' % (
        out, src, cfg['land']['map'], cfg['land']['points_ok'],
        (' note: ' + cfg['land']['note']) if cfg['land']['note'] else '', cfg['space']['map'],
        cfg['space']['points_ok'], (' note: ' + cfg['space']['note']) if cfg['space']['note'] else '', tsrc))
    return p, cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    ap.add_argument('--source-zip', help='read the shipped free-store script from this zip (offline check)')
    ap.add_argument('--land-map', default=DEFAULT_LAND_MAP)
    ap.add_argument('--space-map', default=DEFAULT_SPACE_MAP)
    ap.add_argument('--faction', default=DEFAULT_FACTION, help='faction of the AI player that runs the scenarios')
    ap.add_argument('--config-out', help='also write the sites as JSON')
    ap.add_argument('--no-text', action='store_true', help='do not build the Game_Message text file')
    ap.add_argument('--map-info', action='store_true', help='print the chosen sites and exit')
    ap.add_argument('--rank-land-maps', action='store_true', help='score every MP land map and exit')
    a = ap.parse_args()
    if a.rank_land_maps:
        for row in rank_land_maps(a.game):
            print('%-22s flat_range=%s start_dist=%s %s' % row)
        return 0
    if a.map_info:
        cfg = make_config(a.game, a.land_map, a.space_map, a.faction)
        print(json.dumps({k: cfg[k] for k in ('land', 'space')}, indent=1, sort_keys=True))
        return 0 if cfg['land']['points_ok'] and cfg['space']['points_ok'] else 1
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.source_zip, a.land_map,
          a.space_map, a.faction, a.config_out, not a.no_text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
