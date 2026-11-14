#!/usr/bin/env python3
"""build_s78m_mod.py - builds the s78m t06 Tactical_Bribe probe mod (Mods\\S78MBribeProbe) from the installed game.

  build_s78m_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]
                    [--no-manual] [--config-out FILE]

Land skirmish _MP_LAND_DAGOBAH, human UNDERWORLD vs AI EMPIRE. Files of the mod (MODPATH=Mods\\S78MBribeProbe):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   shipped script + generated S8_CFG + s78m_bribe.lua
  Data\\Text\\MasterTextFile_English.dat               shipped texts + the three TEXT_S78M_* cue strings
  S78M_PROBE_MARKER.txt                               marker: cleanup deletes the folder only when it is there
No XML is changed. Nothing from the game is stored in the repository: shipped files are read from the install
(loose files / MEGs in the FoC order, ../common/ted_owners.py GameFiles) or, for the offline check, from the text
zip (no .dat: --no-text; the probe site then falls back to a fixed point).
The probe site is a dry, open square of the map far from both starts (../oi_abilities/build_oi_mod.py terrain
reader); fallback (4120, 840), the dry Dagobah point of the OI and cu8g probes.
"""
import argparse
import json
import os
import re
import shutil
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
sys.path.insert(0, os.path.join(PROBE_MOD, 'oi_abilities'))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'S78MBribeProbe'
MARKER = 'S78M_PROBE_MARKER.txt'
DEFAULT_GAME = ted_owners.DEFAULT_GAME
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
LAND_MAP = '_MP_LAND_DAGOBAH'
LAND_FALLBACK_SITE = {'x': 4120.0, 'y': 840.0, 'z': 0.0}

# The probe configuration (becomes the S8_CFG Lua table). Frames are mode frames (30 per second).
CFG = {
    'session': 'bribe',
    'ai_faction': 'EMPIRE',            # the AI whose free store runs the probe
    'human_faction': 'Underworld',     # the driver's side; Tyber Zann belongs to it
    'ability': 'TACTICAL_BRIBE',       # Unit_Ability type of Tyber_Tactical_Bribe (ability id 59)
    'hero': 'Tyber_Zann', 'hero_alt': 'Tyber_Zann_Team',
    'squad': 'Imperial_Stormtrooper_Squad',            # 18 Squad_Stormtrooper in two Stormtrooper_Team of 9
    'squad_alt': ['Empire_Survivors_Large', 'Empire_Survivors_Medium'],
    'member_type': 'Squad_Stormtrooper', 'team_type': 'Stormtrooper_Team',
    'start_frame': 450,      # Tyber appears 15 s into the battle
    'gap': 30,               # pause between trials
    'settle': 45,            # frames after a squad spawn before its teams are read
    'near': 200.0,           # a new trooper is one within this XY distance of the trial's spawn point
    'slow_every': 30,        # P records every 30 frames outside a trial's fast window (edges are always recorded)
    'cooldown_max': 1800,    # wait at most 60 s for the bribe to be ready again (Recharge_Seconds 50)
    'trials': [
        # 1: a member as the target, inside Activation_Max_Range 100: cost, whole team, timing, recharge,
        #    Get_Bribe_Cost(tyber) of the team / member types
        {'k': 1, 'name': 'near_member', 'target': 'member', 'dx': 60.0, 'fast': 240, 'watch': 450, 'costs': True},
        # 2: the team container as the target, 250 away: is a container accepted, does Tyber walk, at what distance
        #    (to the container / the members) does it fire. Retried on a member when nothing changed owner: after 60
        #    frames when the container order returned no block (refused), after 600 when it returned one.
        {'k': 2, 'name': 'far_container', 'target': 'container', 'dx': 250.0, 'fast': 900, 'watch': 1200,
         'fallback_after': 60, 'fallback_late': 600},
        # 3: credits set to 5 (Give_Money) before the order, restored 300 frames later: paid? converted? recharge
        #    started? does the order survive until the money is back?
        {'k': 3, 'name': 'refused_poor', 'target': 'member', 'dx': 60.0, 'fast': 600, 'watch': 600, 'poor': 5.0,
         'restore_after': 300},
    ],
    # 4 (optional, by hand): a fresh squad 80 from Tyber, the camera on Tyber; the driver may bribe it through the UI
    'manual': {'on': True, 'k': 4, 'name': 'manual', 'target': 'member', 'dx': 80.0, 'fast': 0, 'watch': 2700,
               'manual': True},
}

TEXTS = [
    ('TEXT_S78M_ARMED', 'S78M PROBE: bribe probe armed. Give no orders. Tyber Zann and the stormtroopers appear at '
                        '15 s; the scripted trials take about 4 minutes.'),
    ('TEXT_S78M_MANUAL', 'S78M PROBE: scripted bribes recorded. Optional: select Tyber Zann and bribe the new '
                         'stormtroopers within 90 s, or quit now.'),
    ('TEXT_S78M_DONE', 'S78M PROBE: bribe run recorded, quit now.'),
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


def kit_text(name):
    return open(os.path.join(HERE, name), encoding='ascii').read()


def prepare(out):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not an s78m probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER), b's78m t06 bribe probe mod (tools/probe_mod/s78m_bribe); removed by '
                                     b'cleanup_s78m.cmd\r\n')


def land_site(game, name):
    """A dry, open 500 x 500 square far from the starts (cu8g / OI method), else the fixed Dagobah point."""
    try:
        import build_oi_mod
        t = build_oi_mod.load_terrain(game, name)
        if t is None:
            raise ValueError('map not readable')
        g = build_oi_mod.Grid(t)
        cell = build_oi_mod.CELL
        best = None
        for y in range(0, g.H, 2):
            for x in range(0, g.W, 2):
                if not g.in_box(x, y, 400.0) or not g.square_all(g.sat_land, x, y, 12):
                    continue
                if g.wh is not None and g.dist_water[y][x] * cell < 300:
                    continue
                far = g.far(x, y)
                if best is None or far > best[0]:
                    best = (far, x, y)
        if best is None:
            raise ValueError('no dry 500 x 500 square')
        p = g.point(best[1], best[2])
        return {'x': p['x'], 'y': p['y'], 'z': p['z']}, '%s (%s): dry square, %.0f from the nearest start' % (
            name, t['source'], best[0])
    except (OSError, ValueError, KeyError, StopIteration, struct.error, ImportError, AttributeError) as e:
        return dict(LAND_FALLBACK_SITE), 'fallback (%s: %s)' % (name, e)


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
    raise TypeError(type(v))


def lua_config(cfg):
    lua_cfg = {k: v for k, v in cfg.items() if k not in ('files', 'freestore_source', 'site_note', 'map')}
    return '-- generated by build_s78m_mod.py\nS8_CFG = %s\n' % lua_value(lua_cfg, '')


def config(game, offline, manual=True):
    cfg = json.loads(json.dumps(CFG))
    cfg['manual']['on'] = bool(manual)
    site, note = (dict(LAND_FALLBACK_SITE), 'fallback (offline)') if offline else land_site(game, LAND_MAP)
    cfg['site'] = site
    cfg['site_note'] = note
    cfg['map'] = LAND_MAP
    return cfg


def freestore(src, cfg):
    shipped, fs_src = src.need(FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    return crlf(text + '\n\n' + lua_config(cfg) + '\n' + kit_text('s78m_bribe.lua')), fs_src


def texts_dat(src, pairs):
    mt, _ = src.need(MASTERTEXT)
    tmp = os.path.join(os.environ.get('TEMP', HERE), 's78m_mt_%d.tmp' % os.getpid())
    with open(tmp, 'wb') as f:
        f.write(mt)
    try:
        keys = {k for k, _ in pairs}
        base = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    finally:
        os.remove(tmp)
    return dat_bytes(base + list(pairs))


def build(game, out, source_zip=None, no_text=False, manual=True):
    """Build Mods\\S78MBribeProbe into `out`, reading shipped files from `game` or the zip. Returns the config."""
    src = Source(game, source_zip)
    cfg = config(game, src.zip is not None, manual)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--no-manual', action='store_true')
    ap.add_argument('--config-out')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.game, out, a.source_zip, a.no_text, not a.no_manual)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s: %s' % (out, ', '.join(cfg['files'])))
    print('site:', cfg['site_note'])


if __name__ == '__main__':
    main()
