#!/usr/bin/env python3
"""build_fow_mod.py - builds the FOW regrow probe mod (Mods\\FowRegrowProbe) from the installed game's own files.

  build_fow_mod.py [--game DIR] [--out MODDIR] [--map _MP_LAND_POLUS] [--source-zip extracted\\foc_data_text.zip]
  build_fow_mod.py --geometry-only [--map NAME]          print the map's terrain size / playable box / FOW geometry

Mod contents (nothing from the game is stored in the repository):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   the shipped script, unchanged, + fow_hook.lua appended with
                                                       @@MAP@@ / @@X1@@ / @@Y1@@ = the map name and its terrain
                                                       extent 20 W, 20 H (.ted chunk 0x101 minis 00 / 01)
  FOW_PROBE_MARKER.txt                                 lets cleanup recognise the folder as ours
The free-store hook path (loose Lua in a MODPATH mod replacing BusyTacticalFreeStore.lua, records read by heap scan)
is the one the A1 probe proved in the retail game on 2026-10-31 (docs/re/probe_results/a1_tactical_20261031.txt).
"""
import argparse
import os
import shutil
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
S10 = os.path.join(os.path.dirname(HERE), 's10_map_owners')
sys.path.insert(0, HERE)
sys.path.insert(0, S10)
import ted_owners  # noqa: E402  (MEG / .ted chunk reader of the S10 kit)
import fow_model  # noqa: E402

MOD_NAME = 'FowRegrowProbe'
MARKER = 'FOW_PROBE_MARKER.txt'
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
HOOK = os.path.join(HERE, 'fow_hook.lua')
DEFAULT_MAP = '_MP_LAND_POLUS'


def read_shipped(game, name, source_zip=None):
    if source_zip:
        z = zipfile.ZipFile(source_zip)
        want = name.upper().replace('/', '\\')
        for n in z.namelist():
            if n.upper().replace('/', '\\') == want:
                return z.read(n), source_zip + ':' + n
        return None, None
    return ted_owners.GameFiles(game).read(name)


def map_info(game, map_name):
    """{'W', 'H' (vertex grid), 'play' ((min x, y), (max x, y)) or None} from the map's .ted."""
    data, src = ted_owners.GameFiles(game).read('DATA\\ART\\MAPS\\%s.TED' % map_name.upper())
    if data is None:
        raise SystemExit('map %s not found in %s' % (map_name, game))
    world = next(p for cid, kids, p in ted_owners.chunks(data) if cid == 1 and kids)
    info = {'W': None, 'H': None, 'play': None, 'src': src}
    for cid, kids, p in ted_owners.chunks(world):
        if cid == 0x101 and kids:
            for c2, k2, p2 in ted_owners.chunks(p):
                if c2 == 0 and not k2:
                    m = ted_owners.minis(p2)
                    info['W'], info['H'] = ted_owners.u32(m.get(0)), ted_owners.u32(m.get(1))
        if cid == 0x103 and not kids:
            m = ted_owners.minis(p)
            if 5 in m and 6 in m and len(m[5]) >= 12 and len(m[6]) >= 12:
                info['play'] = (struct.unpack_from('<2f', m[5]), struct.unpack_from('<2f', m[6]))
    if not info['W']:
        raise SystemExit('map %s: no terrain header' % map_name)
    return info


def hook_text(map_name, info):
    text = open(HOOK, encoding='ascii').read()
    return (text.replace('@@MAP@@', map_name.upper()).replace('@@X1@@', '%.1f' % (20.0 * info['W']))
            .replace('@@Y1@@', '%.1f' % (20.0 * info['H'])))


def freestore_text(game, map_name, info, source_zip=None):
    data, src = read_shipped(game, FREESTORE, source_zip)
    if data is None:
        raise SystemExit('shipped BusyTacticalFreeStore.lua not found (game %s, zip %s)' % (game, source_zip))
    text = data.decode('latin-1').replace('\r\n', '\n')
    out = text.rstrip('\n') + '\n\n' + hook_text(map_name, info)
    return out.replace('\n', '\r\n').encode('latin-1'), src


def build(game, out, map_name=DEFAULT_MAP, source_zip=None, info=None):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a FOW probe folder' % out)
        shutil.rmtree(out)
    info = info or map_info(game, map_name)
    fs, src = freestore_text(game, map_name, info, source_zip)
    p = os.path.join(out, 'Data', 'Scripts', 'FreeStore', 'BusyTacticalFreeStore.lua')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(fs)
    with open(os.path.join(out, MARKER), 'wb') as f:
        f.write(b'FOW regrow probe mod (tools/probe_mod/fow_regrow); removed by cleanup_fow.cmd\r\n')
    print('built %s (BusyTacticalFreeStore.lua from %s, map %s W=%d H=%d)' % (out, src, map_name, info['W'], info['H']))
    return p


def describe(map_name, info):
    lines = ['map %s (%s): terrain vertices %d x %d, playable box %s' % (map_name, info['src'], info['W'], info['H'],
                                                                        info['play'])]
    for ext in ('W', 'W-1'):
        g = fow_model.Geometry.from_terrain(info['W'], info['H'], ext)
        lines.append('  FOW box if the extent is 20*(%s): %s, R 90 -> r %d' % (ext, g.describe(), g.radius_cells(90)))
        if info['play']:
            (_, _), (_, py1) = info['play']
            lines.append('    playable max y %.0f is %.0f units = row %d from the top edge' % (
                py1, g.y1 - py1, g.cell(g.x1 / 2, py1)[0]))
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--out', help='mod folder (default <game>\\corruption\\Mods\\%s)' % MOD_NAME)
    ap.add_argument('--map', default=DEFAULT_MAP)
    ap.add_argument('--source-zip', help='read the shipped free-store script from this zip (offline check)')
    ap.add_argument('--geometry-only', action='store_true')
    a = ap.parse_args()
    info = map_info(a.game, a.map)
    if a.geometry_only:
        print('\n'.join(describe(a.map, info)))
        return
    build(a.game, a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME), a.map, a.source_zip, info)


if __name__ == '__main__':
    main()
