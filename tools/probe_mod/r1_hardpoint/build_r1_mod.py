#!/usr/bin/env python3
"""build_r1_mod.py - builds the R1 hardpoint probe mod (roadmap group r1, t01 "Provisional hardpoint-destroyed rule")
from the installed game.

  build_r1_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text] [--config-out F]

Mod folder Mods\\R1HardpointProbe (MODPATH=Mods\\R1HardpointProbe). Files:
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   the shipped free store + generated R1_CFG + r1_hook.lua
  Data\\Text\\MasterTextFile_English.dat                the shipped text + the TEXT_R1P_* probe messages
No XML is changed. Space skirmish _MP_SPACE_KUAT, human EMPIRE vs AI REBEL; only the Rebel AI's free store arms the
probe (and only while Find_Player("Empire").Is_Human()). Two Star_Destroyers owned by the human: A (visual stages and
the owner death) at the probe site, B (Lua name-lookup tests) 2500 south of it (b_dy). The probe site is the point of the central
70 % of the map farthest from both start positions and from every map object (as the cu8g stick probe).

Nothing from the game is stored in the repository: shipped files come from the install (../common/ted_owners.py
GameFiles) or, offline, from the text zip (no .dat / .ted: fixed site, no message texts).
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
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
sys.path.insert(0, PROBE_MOD)
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'R1HardpointProbe'
MARKER = 'R1_PROBE_MARKER.txt'
DEFAULT_GAME = ted_owners.DEFAULT_GAME
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
SPACE_MAP = '_MP_SPACE_KUAT'
FALLBACK_SITE = {'x': -3500.0, 'y': 3500.0, 'z': 0.0}   # Kuat's empty diagonal (starts at -4000,-4100 / 4300,4250)

# Star_Destroyer hardpoints (HardPoints list order = index; health and breakoff from Hardpoints.xml, FoC 1.121).
HARDPOINTS = [
    ('HP_Star_Destroyer_Weapon_FL', 350.0, 'Hardpoint_Breakoff_Star_Dest_Weapon_FL'),
    ('HP_Star_Destroyer_Weapon_FR', 350.0, 'Hardpoint_Breakoff_Star_Dest_Weapon_FR'),
    ('HP_Star_Destroyer_Weapon_BL', 350.0, 'Hardpoint_Breakoff_Star_Dest_Weapon_BL'),
    ('HP_Star_Destroyer_Weapon_BR', 350.0, 'Hardpoint_Breakoff_Star_Dest_Weapon_BR'),
    ('HP_Star_Destroyer_Weapon_ML', 350.0, None),
    ('HP_Star_Destroyer_Weapon_MR', 350.0, None),
    ('HP_Star_Destroyer_Shield_Generator', 375.0, 'Hardpoint_Breakoff_Star_Dest_Shield_Gen'),
    ('HP_Star_Destroyer_Engines', 325.0, None),
    ('HP_Star_Destroyer_Tractor_Beam', 225.0, None),
    ('HP_Star_Destroyer_Fighter_Bay', 250.0, None),
]

# Stages. ship A or B; arg = the Take_Damage second argument ("" / None: hull damage, the owner-death stage); alt = the
# hardpoint's Collision_Mesh, used for the later A stages instead of arg when the first type-name kill (FL) did not kill
# its hardpoint (the hook decides at kill + decide frames); hp = the hardpoint index the stage expects to die (None: none
# / owner death). amount: A stages 5000 (a full 2000-point shield still lets >= 1000 through; a miss leaves the hull
# alive); B tests: base + 1.1 x the shield points left (Get_Shield, fraction x shield_points), so a miss costs ~1000 hull.
STAGE_DEFS = {d['key']: d for d in [
    {'key': 'FL', 'ship': 'A', 'amount': 5000.0, 'arg': 'HP_Star_Destroyer_Weapon_FL', 'alt': 'HP_F-L_COLL', 'hp': 0,
     'kind': 'type_name', 'view': 'side'},
    {'key': 'ML', 'ship': 'A', 'amount': 5000.0, 'arg': 'HP_Star_Destroyer_Weapon_ML', 'alt': 'HP_M-L_COLL', 'hp': 4,
     'kind': 'type_name', 'view': 'side'},
    {'key': 'MR', 'ship': 'A', 'amount': 5000.0, 'arg': 'HP_Star_Destroyer_Weapon_MR', 'alt': 'HP_M-R_COLL', 'hp': 5,
     'kind': 'type_name', 'view': 'side'},
    {'key': 'ENG', 'ship': 'A', 'amount': 5000.0, 'arg': 'HP_Star_Destroyer_Engines', 'alt': 'HP_E_COLL', 'hp': 7,
     'kind': 'type_name', 'view': 'stern'},
    {'key': 'SHLD', 'ship': 'A', 'amount': 5000.0, 'arg': 'HP_Star_Destroyer_Shield_Generator', 'alt': 'HP_M-C_COLL',
     'hp': 6, 'kind': 'type_name'},
    {'key': 'TRAC', 'ship': 'A', 'amount': 5000.0, 'arg': 'HP_Star_Destroyer_Tractor_Beam', 'alt': 'HP_trac_bone',
     'hp': 8, 'kind': 'type_name'},
    {'key': 'DEATH', 'ship': 'A', 'amount': 1000000.0, 'arg': None, 'hp': None, 'kind': 'owner_death'},
    {'key': 'LOWER', 'ship': 'B', 'amount': 1000.0, 'shield_scaled': True, 'arg': 'hp_star_destroyer_weapon_br', 'hp': 3,
     'kind': 'lower_case'},
    {'key': 'COLL', 'ship': 'B', 'amount': 1000.0, 'shield_scaled': True, 'arg': 'HP_F-R_COLL', 'hp': 1,
     'kind': 'collision_mesh'},
    # The bogus name: a hardpoint name that does not exist. Retail may kill the Lua script on the call (it is run LAST,
    # the hook writes its DONE record first). A miss costs ~1000 hull (shield_scaled), so the ship survives it.
    {'key': 'BOGUS', 'ship': 'A', 'amount': 1000.0, 'shield_scaled': True, 'arg': 'HP_NO_SUCH', 'hp': None,
     'kind': 'bogus_name', 'final': True},
]}

# Profiles: 'short' is the r1b t07 rerun (about 6 minutes of battle), 'full' is the original r1 t01 run (9 minutes).
PROFILES = {
    'short': {'stages': ['FL', 'ML', 'MR', 'ENG', 'BOGUS'],
              'cfg': {'first_kill': 2700, 'period': 1800, 'pre': 450, 'tail': 450}},   # 1:30, then every minute
    # 'tail': only the open points of the 2026-11-09 run: ENG with a stern view, then the bogus name. Ship A is ordered to
    # move north (turn_dy) so a camera in the south sees its stern. Both kills early (1:00, 1:45).
    'tail': {'stages': ['ENG', 'BOGUS'],
             'cfg': {'first_kill': 1800, 'period': 1350, 'pre': 450, 'tail': 300, 'turn_dy': 3000.0}},
    'full': {'stages': ['FL', 'ENG', 'SHLD', 'TRAC', 'DEATH', 'LOWER', 'COLL'], 'cfg': {}},
}
DEFAULT_PROFILE = 'short'
STAGES = [STAGE_DEFS[k] for k in PROFILES[DEFAULT_PROFILE]['stages']]   # default profile (kept for the selftest)


def stages_for(profile):
    return [STAGE_DEFS[k] for k in PROFILES[profile]['stages']]


CFG = {
    'session': 'r1',
    'ai_faction': 'REBEL',
    'human_faction': 'Empire',
    'unit': 'Star_Destroyer',
    'death_clone': 'Star_Destroyer_Death_Clone',
    'explosion': 'Large_Explosion_Space_Empire',
    'shield_points': 2000.0,    # Generic_Star_Destroyer Shield_Points
    'b_dy': -2500.0,            # B 2500 south of A (toward the human start, away from the map edge)
    'spawn_frame': 300,         # 0:10
    'first_kill': 3600,         # 2:00
    'period': 1800,             # one stage per minute
    'pre': 300,                 # pre-message 10 s before each kill
    'decide': 60,               # FL kill + 60 frames: type-name or Collision_Mesh mode for the other A stages
    'fast': 90,                 # poll every service (3 frames) for 90 frames after a kill
    'slow': 30,                 # otherwise every 30 frames
    'tail': 450,                # after the last stage
    'lua_camera': False,        # --lua-camera: the hook calls Point_Camera_At(ship A) at each pre-message (unproven)
}

TEXTS = [
    ('TEXT_R1P_ARMED', 'R1 PROBE: armed. Two Star Destroyers (yours) appear at 0:10 in the empty corner. Give no orders.'),
    ('TEXT_R1P_SPAWNED', 'R1 PROBE: Star Destroyers A and B are here (your blips in the empty corner). Frame A, take '
                         'the BEFORE shots. First kill at 2:00.'),
    ('TEXT_R1P_FL_PRE', 'R1 PROBE: in 10 s, kill 1: ship A FRONT-LEFT turbolaser (HP_..._Weapon_FL).'),
    ('TEXT_R1P_FL_DONE', 'R1 PROBE: kill 1 done (A front-left turret). Take the AFTER shots now.'),
    ('TEXT_R1P_ENG_PRE', 'R1 PROBE: in 10 s, kill 4: ship A ENGINES (stern view).'),
    ('TEXT_R1P_ENG_DONE', 'R1 PROBE: kill 4 done (A engines). Take the AFTER shots of the stern now.'),
    ('TEXT_R1P_ML_PRE', 'R1 PROBE: in 10 s, kill 2: ship A MID-LEFT turret (side view of the left hull edge).'),
    ('TEXT_R1P_ML_DONE', 'R1 PROBE: kill 2 done (A mid-left). Take the AFTER shots now.'),
    ('TEXT_R1P_MR_PRE', 'R1 PROBE: in 10 s, kill 3: ship A MID-RIGHT turret (side view of the right hull edge).'),
    ('TEXT_R1P_MR_DONE', 'R1 PROBE: kill 3 done (A mid-right). Take the AFTER shots now.'),
    ('TEXT_R1P_BOGUS_PRE', 'R1 PROBE: in 10 s, LAST step: ship A gets a bogus name HP_NO_SUCH. The script may stop; '
                           'take the last shots, then quit.'),
    ('TEXT_R1P_BOGUS_DONE', 'R1 PROBE: bogus-name step done and the script is still running. Esc > Quit.'),
    ('TEXT_R1P_SHLD_PRE', 'R1 PROBE: in 10 s, kill 3: ship A SHIELD GENERATOR (domes on the bridge tower).'),
    ('TEXT_R1P_SHLD_DONE', 'R1 PROBE: kill 3 done (A shield generator). Take the AFTER shots of the tower now.'),
    ('TEXT_R1P_TRAC_PRE', 'R1 PROBE: in 10 s, kill 4: ship A TRACTOR BEAM.'),
    ('TEXT_R1P_TRAC_DONE', 'R1 PROBE: kill 4 done (A tractor beam). Take the AFTER shots now.'),
    ('TEXT_R1P_DEATH_PRE', 'R1 PROBE: in 10 s ship A is DESTROYED. Screenshot every 2 s for 20 s.'),
    ('TEXT_R1P_DEATH_DONE', 'R1 PROBE: ship A destroyed. Next: frame ship B (2500 south of A, toward your base) within 50 s.'),
    ('TEXT_R1P_LOWER_PRE', 'R1 PROBE: in 10 s, test 6: ship B REAR-RIGHT turret (lower-case name).'),
    ('TEXT_R1P_LOWER_DONE', 'R1 PROBE: test 6 done (B rear-right). Take the AFTER shots now.'),
    ('TEXT_R1P_COLL_PRE', 'R1 PROBE: in 10 s, test 7: ship B FRONT-RIGHT turret (collision-mesh name).'),
    ('TEXT_R1P_COLL_DONE', 'R1 PROBE: test 7 done (B front-right). Take the AFTER shots now.'),
    ('TEXT_R1P_DONE', 'R1 PROBE: all done. Esc > Quit, then quit the game.'),
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
            raise SystemExit('refusing to overwrite %s: not an R1 probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER), b'R1 hardpoint probe mod (tools/probe_mod/r1_hardpoint); removed by cleanup_r1.cmd\r\n')


def space_site(src, name):
    """Point of the central 70 % of the map farthest from both start positions and every map object (starts weigh 3x).
    Returns (site, note, map info {w, h, starts})."""
    data, msrc = (None, None) if src.zip else src.read('DATA\\ART\\MAPS\\%s.TED' % name.upper())
    if data is None:
        return dict(FALLBACK_SITE), 'fallback (map %s not readable)' % name, None
    top = ted_owners.chunks(data)
    c0 = next(p for cid, k, p in top if cid == 0)
    m = ted_owners.minis(c0)
    w = struct.unpack('<f', m[0x10][:4])[0] if 0x10 in m else 8000.0
    h = struct.unpack('<f', m[0x11][:4])[0] if 0x11 in m else 8000.0
    starts = []
    for cid, kids, p in top:
        if cid == 3 and not kids and len(p) >= 4:
            n = struct.unpack_from('<I', p, 0)[0]
            starts = [struct.unpack_from('<2f', p, 4 + 8 * i) for i in range(n)]
    pl, _ = ted_owners.parse_placements(data)
    pts = [(x, y) for _i, _o, _t, _w, (x, y, _z) in pl] + list(starts)
    best = None
    step = 250.0
    nx, ny = int(0.35 * w // step), int(0.35 * h // step)
    for i in range(-nx, nx + 1):
        for j in range(-ny, ny + 1):
            x, y = i * step, j * step
            d_obj = min((math.hypot(x - a, y - b) for a, b in pts), default=1e9)
            d_start = min((math.hypot(x - a, y - b) for a, b in starts), default=1e9)
            score = min(d_obj, d_start / 3.0)
            if best is None or score > best[0] + 1e-6:
                best = (score, x, y, d_obj, d_start)
    _s, x, y, d_obj, d_start = best
    info = {'w': w, 'h': h, 'starts': [list(s) for s in starts]}
    return ({'x': x, 'y': y, 'z': 0.0}, '%s (%s): %.0f x %.0f, %d objects; clearance %.0f to objects, %.0f to starts' % (
        name, msrc, w, h, len(pl), d_obj, d_start), info)


def minimap_hint(site, info, b_dy):
    """Where the site sits on the minimap, as fractions of the map (x from the left, y from the bottom; the minimap is
    assumed north-up = +y up)."""
    if not info:
        return 'map size unknown (offline build)'
    w, h = info['w'], info['h']
    fx = 0.5 + site['x'] / w
    fy = 0.5 + site['y'] / h
    fyb = 0.5 + (site['y'] + b_dy) / h
    return ('A at %.0f%% from the left, %.0f%% from the bottom of the map; B at %.0f%% from the bottom (if +y is up); '
            'starts at %s' % (100 * fx, 100 * fy, 100 * fyb,
                              ', '.join('(%.0f, %.0f)' % tuple(s) for s in info['starts'])))


def counted():
    """(type name, short key) of every object type the hook counts: the breakoff props, the death clone, the explosion."""
    out = [(b, b.replace('Hardpoint_Breakoff_Star_Dest_', '')) for b in sorted({b for _n, _h, b in HARDPOINTS if b})]
    return out + [(CFG['death_clone'], 'DC'), (CFG['explosion'], 'EX')]


def lua_value(v, indent='\t'):
    if v is None:
        return 'nil'
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, float)):
        return repr(float(v)) if isinstance(v, float) else str(v)
    if isinstance(v, str):
        return '"%s"' % v.replace('\\', '\\\\').replace('"', '\\"')
    if isinstance(v, (list, tuple)):
        return '{ ' + ', '.join(lua_value(x, indent) for x in v) + ' }'
    if isinstance(v, dict):
        inner = indent + '\t'
        items = []
        for k in sorted(v):
            if v[k] is None:
                continue
            key = k if re.match(r'^[A-Za-z_]\w*$', k) else '["%s"]' % k
            items.append('%s%s = %s,' % (inner, key, lua_value(v[k], inner)))
        return '{\n' + '\n'.join(items) + '\n' + indent + '}'
    raise TypeError(type(v))


def config(site, note, hint, profile=DEFAULT_PROFILE, lua_camera=False):
    cfg = dict(CFG)
    cfg.update(PROFILES[profile]['cfg'])
    cfg['profile'] = profile
    cfg['lua_camera'] = bool(lua_camera)
    cfg['site'] = site
    cfg['site_note'] = note
    cfg['minimap'] = hint
    cfg['hardpoints'] = [{'name': n, 'health': h, 'breakoff': b or ''} for n, h, b in HARDPOINTS]
    cfg['counted'] = [{'name': n, 'short': k} for n, k in counted()]
    cfg['stages'] = [dict(s, arg=s['arg'] or '', alt=s.get('alt', ''), shield_scaled=s.get('shield_scaled', False),
                          hp=-1 if s['hp'] is None else s['hp'], view=s.get('view', ''), final=s.get('final', False),
                          breakoff=(HARDPOINTS[s['hp']][2] or '') if s['hp'] is not None else '') for s in stages_for(profile)]
    return cfg


def lua_config(cfg):
    return '-- generated by build_r1_mod.py\nR1_CFG = %s\n' % lua_value(cfg, '')


def freestore(src, cfg):
    shipped, fs_src = src.need(FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    tail = lua_config(cfg) + '\n' + kit_text('r1_hook.lua')
    return crlf(text + '\n\n' + tail), fs_src


def texts_dat(src, pairs):
    mt, _ = src.need(MASTERTEXT)
    tmp = os.path.join(os.environ.get('TEMP', HERE), 'r1_mt_%d.tmp' % os.getpid())
    with open(tmp, 'wb') as f:
        f.write(mt)
    try:
        keys = {k for k, _ in pairs}
        base = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    finally:
        os.remove(tmp)
    return dat_bytes(base + list(pairs))


def build(game, out, source_zip=None, no_text=False, profile=DEFAULT_PROFILE, lua_camera=False):
    """Build Mods\\R1HardpointProbe into `out` (shipped files from `game` or the zip). Returns the config dict."""
    src = Source(game, source_zip)
    site, note, info = space_site(src, SPACE_MAP)
    cfg = config(site, note, minimap_hint(site, info, CFG['b_dy']), profile, lua_camera)
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
    cfg['map'] = SPACE_MAP
    return cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--config-out')
    ap.add_argument('--profile', choices=sorted(PROFILES), default=DEFAULT_PROFILE)
    ap.add_argument('--lua-camera', action='store_true')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.game, out, a.source_zip, a.no_text, a.profile, a.lua_camera)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s: %s' % (out, ', '.join(cfg['files'])))
    print('site:', cfg['site_note'])
    print('minimap:', cfg['minimap'])


if __name__ == '__main__':
    main()
