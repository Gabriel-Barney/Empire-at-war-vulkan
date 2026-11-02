#!/usr/bin/env python3
"""build_oi_mod.py - builds the OI ability probe mod (Mods\\OIAbilityProbe) from the installed game's own files.

  build_oi_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--map _MP_LAND_DAGOBAH]
                  [--faction REBEL] [--config-out oi_config.json]
  build_oi_mod.py --map-info [--map NAME] [--game DIR]     prints the chosen spawn points and exits

What it writes (nothing from the game is stored in the repository):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   the shipped script, unchanged, + a generated OIP_CFG table
                                                       (spawn points) + oi_hook.lua appended
  OI_PROBE_MARKER.txt                                  lets cleanup (oi_cleanup.py) recognise the folder as ours
and, with --config-out, the same points as JSON for oi_reader.py / oi_parse.py.

Spawn points come from the map file itself (docs/re/TERRAIN_WATER_POST.md §1): the .ted is read from the install
(FoC file order, tools/probe_mod/s10_map_owners/ted_owners.py), and the script picks, far from every start position
and inside the playable box:
  mal_land / han_land        a dry Clear patch (no water cell within 200) for the land control MAL and its Han
  mal_shallow / han_shallow  a Water-passable cell (0x10A bit 0x20) 3..10 units under the water plane + dry spot nearby
  mal_deep / han_deep        the deepest Water-passable cell with water all around + the nearest dry spot 60..140 away
  arena1..3                  three dry Clear squares (240 x 240 units) for the Rancor, grenade and Berserker scenarios
Without a readable map (offline check, --source-zip and no install) the config says points_ok = false and the hook
falls back to positions next to the AI's first unit (the water scenarios then cannot answer §4.12).
"""
import argparse
import json
import math
import os
import shutil
import struct
import sys
import zipfile
from collections import deque

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 's10_map_owners'))
import ted_owners  # noqa: E402

MOD_NAME = 'OIAbilityProbe'
MARKER = 'OI_PROBE_MARKER.txt'
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
HOOK = os.path.join(HERE, 'oi_hook.lua')
DEFAULT_MAP = '_MP_LAND_DAGOBAH'
DEFAULT_FACTION = 'REBEL'
START_FRAME = 450          # scenarios start 15 s into the battle

# Types the reader watches (upper case, as in the type's name string) and the role each plays.
ATTACKER_TYPES = ('RANCOR', 'HAN_SOLO', 'YODA', 'REBEL_INFILTRATOR')
VICTIM_TYPES = ('MAL_ROCKET_VEHICLE', 'T4B_TANK', 'STORMTROOPER')
PROJECTILE_TYPES = ('PROJ_STICKY_BOMB', 'PROJ_SPECIAL_HAN_SOLO_STUN_BLAST')

CLEAR, WATER = 0x1, 0x20
CELL = 20.0
HEIGHT_SCALE = 0.048828125


# ---- shipped files -------------------------------------------------------------------------------------------------

def read_shipped(game, name, source_zip=None):
    """(bytes, label) of a shipped Data file: the extracted text zip when given, else the install in FoC order."""
    if source_zip:
        with zipfile.ZipFile(source_zip) as z:
            want = name.upper().replace('/', '\\')
            for n in z.namelist():
                if n.upper().replace('/', '\\') == want:
                    return z.read(n), source_zip + ':' + n
        return None, None
    if not os.path.isdir(game):
        return None, None
    return ted_owners.GameFiles(game).read(name)


# ---- map analysis --------------------------------------------------------------------------------------------------

def load_terrain(game, map_name):
    """{W, H, h (row-major, world z), pas (column-major x*H+y), wh, wtype, starts, box} or None."""
    n = map_name.upper()
    if not n.endswith('.TED'):
        n += '.TED'
    if '\\' not in n:
        n = 'DATA\\ART\\MAPS\\' + n
    if not os.path.isdir(game):
        return None
    data, src = ted_owners.GameFiles(game).read(n)
    if data is None:
        return None
    top = ted_owners.chunks(data)
    starts = []
    for cid, kids, p in top:
        if cid == 3 and not kids and len(p) >= 4:
            c = struct.unpack_from('<I', p, 0)[0]
            starts = [struct.unpack_from('<2f', p, 4 + 8 * i) for i in range(c)]
    world = next(p for cid, kids, p in top if cid == 1 and kids)
    t = {'name': n, 'source': src, 'starts': starts, 'box': None, 'wh': None, 'wtype': 0}
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
        elif cid == 0x103 and not kids:
            m = ted_owners.minis(p)
            if 5 in m and 6 in m and len(m[5]) >= 12 and len(m[6]) >= 12:
                t['box'] = (struct.unpack('<3f', m[5][:12]), struct.unpack('<3f', m[6][:12]))
    W, H = t['W'], t['H']
    t['h'] = [struct.unpack_from('<h', verts, 4 * i)[0] * HEIGHT_SCALE if verts and 4 * i + 2 <= len(verts) else 0.0
              for i in range(W * H)]
    t['pas'] = [struct.unpack_from('<I', pas, 4 * i)[0] for i in range(W * H)] if pas else [CLEAR] * (W * H)
    return t


class Grid:
    def __init__(self, t):
        self.t = t
        self.W, self.H = t['W'], t['H']
        wet = t['wtype'] == 1 and t['wh'] is not None
        self.wh = t['wh'] if wet else None
        W, H = self.W, self.H
        self.water = [[False] * W for _ in range(H)]   # [y][x]
        self.land = [[False] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                p = t['pas'][x * H + y]
                h = t['h'][y * W + x]
                self.water[y][x] = bool(p & WATER) and wet
                self.land[y][x] = p == CLEAR and (not wet or h > self.wh + 4.0)
        self.sat_land = self._sat(self.land)
        self.sat_water = self._sat(self.water)
        self.dist_water = self._bfs(self.water)
        box = t['box']
        if box:
            self.bx0, self.by0, self.bx1, self.by1 = box[0][0], box[0][1], box[1][0], box[1][1]
        else:
            self.bx0 = self.by0 = 0.0
            self.bx1, self.by1 = CELL * (W - 1), CELL * (H - 1)

    def _sat(self, g):
        W, H = self.W, self.H
        s = [[0] * (W + 1) for _ in range(H + 1)]
        for y in range(H):
            row = 0
            for x in range(W):
                row += 1 if g[y][x] else 0
                s[y + 1][x + 1] = s[y][x + 1] + row
        return s

    def square_all(self, sat, x, y, r):
        x0, y0, x1, y1 = x - r, y - r, x + r, y + r
        if x0 < 0 or y0 < 0 or x1 >= self.W or y1 >= self.H:
            return False
        tot = sat[y1 + 1][x1 + 1] - sat[y0][x1 + 1] - sat[y1 + 1][x0] + sat[y0][x0]
        return tot == (2 * r + 1) ** 2

    def _bfs(self, g):
        W, H = self.W, self.H
        inf = 10 ** 9
        d = [[inf] * W for _ in range(H)]
        q = deque()
        for y in range(H):
            for x in range(W):
                if g[y][x]:
                    d[y][x] = 0
                    q.append((x, y))
        while q:
            x, y = q.popleft()
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and d[ny][nx] > d[y][x] + 1:
                    d[ny][nx] = d[y][x] + 1
                    q.append((nx, ny))
        return d

    def in_box(self, x, y, margin=200.0):
        wx, wy = x * CELL, y * CELL
        return self.bx0 + margin <= wx <= self.bx1 - margin and self.by0 + margin <= wy <= self.by1 - margin

    def far(self, x, y):
        st = self.t['starts']
        if not st:
            return 0.0
        return min(math.hypot(x * CELL - sx, y * CELL - sy) for sx, sy in st)

    def depth(self, x, y):
        return (self.wh - self.t['h'][y * self.W + x]) if self.wh is not None else None

    def point(self, x, y, on_water=False):
        h = self.t['h'][y * self.W + x]
        z = self.wh if on_water and self.wh is not None else h
        d = self.depth(x, y)
        return {'x': round(x * CELL, 1), 'y': round(y * CELL, 1), 'z': round(z, 2), 'terrain_z': round(h, 2),
                'depth': None if d is None else round(d, 2), 'cell': [x, y]}


def _dist(a, b):
    return math.hypot(a['x'] - b['x'], a['y'] - b['y'])


def nearest_dry(g, x, y, dmin=60.0, dmax=140.0):
    best = None
    r = int(dmax // CELL) + 1
    for yy in range(max(0, y - r), min(g.H, y + r + 1)):
        for xx in range(max(0, x - r), min(g.W, x + r + 1)):
            d = math.hypot((xx - x) * CELL, (yy - y) * CELL)
            if dmin <= d <= dmax and g.square_all(g.sat_land, xx, yy, 1):
                if best is None or d < best[0]:
                    best = (d, xx, yy)
    return best


def pick_points(t):
    """Chosen spawn points (dict name -> point) for the scenarios; raises ValueError if a part cannot be found."""
    g = Grid(t)
    pts = {}
    if g.wh is None:
        raise ValueError('map %s has no water plane (water type %s)' % (t['name'], t['wtype']))
    deep, shallow = [], []
    for y in range(0, g.H):
        for x in range(0, g.W):
            if not g.water[y][x] or not g.in_box(x, y) or not g.square_all(g.sat_water, x, y, 2):
                continue
            d = g.depth(x, y)
            far = g.far(x, y)
            if d >= 20.0:
                deep.append((d, far, x, y))
            elif 3.0 <= d <= 10.0:
                shallow.append((d, far, x, y))
    if not deep or not shallow:
        raise ValueError('no deep (%d) or shallow (%d) water candidates' % (len(deep), len(shallow)))

    def choose_water(cands, key, avoid):
        for _d, _f, x, y in sorted(cands, key=key):
            if any(math.hypot(x * CELL - p['x'], y * CELL - p['y']) < 600 for p in avoid):
                continue
            dry = nearest_dry(g, x, y)
            if dry is None:
                continue
            return g.point(x, y, on_water=True), g.point(dry[1], dry[2])
        return None, None

    # deep: deepest first among points >= 900 from every start (else the farthest)
    far_ok = [c for c in deep if c[1] >= 900] or deep
    pts['mal_deep'], pts['han_deep'] = choose_water(far_ok, lambda c: (-round(c[0]), -c[1]), [])
    if pts['mal_deep'] is None:
        # deep water with no dry spot within 140: take the deepest cell that has one within 140 at any depth >= 20
        raise ValueError('no deep water cell with a dry spot 60..140 units away')
    far_ok = [c for c in shallow if c[1] >= 900] or shallow
    pts['mal_shallow'], pts['han_shallow'] = choose_water(far_ok, lambda c: (-c[1], c[0]), [pts['mal_deep']])
    if pts['mal_shallow'] is None:
        raise ValueError('no shallow water cell with a dry spot nearby')

    used = [pts['mal_deep'], pts['mal_shallow']]
    lands = []
    for y in range(0, g.H, 2):
        for x in range(0, g.W, 2):
            if g.in_box(x, y, 300.0) and g.square_all(g.sat_land, x, y, 6) and g.dist_water[y][x] * CELL >= 200:
                lands.append((g.far(x, y), x, y))
    lands.sort(key=lambda c: -c[0])

    def take_land(name, min_sep=500.0):
        for _f, x, y in lands:
            p = g.point(x, y)
            if all(_dist(p, q) >= min_sep for q in used):
                used.append(p)
                pts[name] = p
                return p
        raise ValueError('no dry square for ' + name)

    take_land('mal_land')
    lx, ly = pts['mal_land']['cell']
    han = None
    for dx, dy in ((4, 0), (-4, 0), (0, 4), (0, -4), (3, 3)):
        if 0 <= lx + dx < g.W and 0 <= ly + dy < g.H and g.square_all(g.sat_land, lx + dx, ly + dy, 1):
            han = g.point(lx + dx, ly + dy)
            break
    if han is None:
        raise ValueError('no dry spot next to mal_land')
    pts['han_land'] = han
    for a in ('arena1', 'arena2', 'arena3'):
        take_land(a, 400.0)
    return pts


def map_config(game, map_name, faction):
    cfg = {'map': map_name.upper(), 'faction': faction.upper(), 'start_frame': START_FRAME, 'points_ok': False,
           'points': {}, 'water_height': None, 'starts': [], 'map_source': None, 'note': '',
           'watch': {'attackers': list(ATTACKER_TYPES), 'victims': list(VICTIM_TYPES),
                     'projectiles': list(PROJECTILE_TYPES)}}
    t = None
    try:
        t = load_terrain(game, map_name)
    except (OSError, ValueError, KeyError, StopIteration, struct.error) as e:
        cfg['note'] = 'map read failed: %s' % e
    if t is None:
        cfg['note'] = cfg['note'] or 'map %s not readable from %s (offline build?)' % (map_name, game)
        return cfg
    cfg['map_source'] = '%s (%s)' % (t['name'], t['source'])
    cfg['water_height'] = t['wh']
    cfg['starts'] = [[round(a, 1), round(b, 1)] for a, b in t['starts']]
    try:
        cfg['points'] = pick_points(t)
        cfg['points_ok'] = True
    except ValueError as e:
        cfg['note'] = str(e)
    return cfg


# ---- Lua generation ------------------------------------------------------------------------------------------------

def lua_num(v):
    return 'nil' if v is None else repr(float(v))


def lua_config(cfg):
    lines = ['-- generated by build_oi_mod.py (map %s): spawn points for the OI ability probe' % cfg['map'],
             'OIP_CFG = {',
             '\tmap = "%s",' % cfg['map'],
             '\tfaction = "%s",' % cfg['faction'],
             '\tstart_frame = %d,' % cfg['start_frame'],
             '\tpoints_ok = %s,' % ('true' if cfg['points_ok'] else 'false'),
             '\twater_height = %s,' % lua_num(cfg['water_height']),
             '\tpts = {']
    for k in sorted(cfg['points']):
        p = cfg['points'][k]
        lines.append('\t\t%s = { x = %s, y = %s, z = %s, depth = %s },' % (
            k, lua_num(p['x']), lua_num(p['y']), lua_num(p['z']), lua_num(p['depth'])))
    lines += ['\t},', '}', '']
    return '\n'.join(lines)


def freestore_text(game, cfg, source_zip=None):
    data, src = read_shipped(game, FREESTORE, source_zip)
    if data is None:
        raise SystemExit('shipped BusyTacticalFreeStore.lua not found (game %s, zip %s)' % (game, source_zip))
    text = data.decode('latin-1').replace('\r\n', '\n')
    hook = open(HOOK, encoding='ascii').read()
    out = text.rstrip('\n') + '\n\n' + lua_config(cfg) + '\n' + hook
    return out.replace('\n', '\r\n').encode('latin-1'), src


def build(game, out, source_zip=None, map_name=DEFAULT_MAP, faction=DEFAULT_FACTION, config_out=None):
    cfg = map_config(game, map_name, faction)
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an OI probe folder' % out)
        shutil.rmtree(out)
    fs, src = freestore_text(game, cfg, source_zip)
    p = os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(fs)
    with open(os.path.join(out, MARKER), 'wb') as f:
        f.write(b'OI ability probe mod (tools/probe_mod/oi_abilities); removed by cleanup_oi.cmd\r\n')
    cfg['freestore_source'] = src
    if config_out:
        with open(config_out, 'w') as f:
            json.dump(cfg, f, indent=1, sort_keys=True)
    print('built %s (BusyTacticalFreeStore.lua from %s; map %s points_ok=%s%s)' % (
        out, src, cfg['map'], cfg['points_ok'], (' note: ' + cfg['note']) if cfg['note'] else ''))
    return p, cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    ap.add_argument('--source-zip', help='read the shipped free-store script from this zip (offline check)')
    ap.add_argument('--map', default=DEFAULT_MAP)
    ap.add_argument('--faction', default=DEFAULT_FACTION, help='faction of the AI player that runs the scenarios')
    ap.add_argument('--config-out', help='also write the spawn points as JSON')
    ap.add_argument('--map-info', action='store_true', help='print the chosen points and exit')
    a = ap.parse_args()
    if a.map_info:
        cfg = map_config(a.game, a.map, a.faction)
        print(json.dumps(cfg, indent=1, sort_keys=True))
        return 0 if cfg['points_ok'] else 1
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.source_zip, a.map, a.faction,
          a.config_out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
