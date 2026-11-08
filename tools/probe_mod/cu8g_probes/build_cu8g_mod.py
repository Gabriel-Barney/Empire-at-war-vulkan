#!/usr/bin/env python3
"""build_cu8g_mod.py - builds the cu8g probe mod (roadmap group cu8g, tasks t07 / t08 / t09) from the installed game.

  build_cu8g_mod.py <session> [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]
                    [--map-loose] [--config-out FILE]

Sessions (one mod folder, Mods\\CU8GProbe, rebuilt for each session; MODPATH=Mods\\CU8GProbe):
  stick      t07 stickiness (CU8F_T04_T06_RE.md sec. 1.5). Space skirmish _MP_SPACE_KUAT (the map of the port's
             --skirmish-ai foc-station test), human REBEL vs AI EMPIRE. Files: BusyTacticalFreeStore.lua (shipped +
             C8_CFG + cu8g_common.lua + cu8g_stick.lua), MasterTextFile_English.dat (+ probe strings). No XML changed.
  abil       t09 (optional): a) LURE Is_Pulsing (CU8F_T04_T06_RE.md sec. 3.1), land skirmish _MP_LAND_DAGOBAH;
             b) Cancel_Special_Ability restore timing (CU8F_T01_T02_T07_RE.md sec. 1), space skirmish _MP_SPACE_KUAT;
             human EMPIRE vs AI REBEL in both. Files: the free store (+ cu8g_lure.lua + cu8g_cancel.lua),
             UNITS_LAND_UNDERWORLD_CANDEROUS_TANK.XML (Is_Pulsing No on Canderous_Assault_Tank only),
             UNIQUEUNITS.XML (Akbar_Cancel_Special_Ability un-commented on Home_One), texts.
  maps-miss  t08 (CU8F_T03_T05_RE.md sec. 1.7): campaign "CU8G PROBE: MISSING MAPS"; Planets.xml with Jabiim's
             Land_Tactical_Map and Thyferra's Space_Tactical_Map emptied and both Terrain = Temperate. Files:
             CampaignFiles.xml, CU8G_Probe_Campaigns.xml, story plot + story XML + CU8G_Story_Maps.lua, Planets.xml,
             the free store (+ cu8g_maps_tact.lua), texts.
  maps-hit   as maps-miss plus _MP_LAND_CU8G_ALDERAAN.TED: a copy of _MP_LAND_ALDERAAN.TED (Temperate, 2 players) with
             map-info mini 05 (faction) patched 3 -> 1 (Empire), which the picker should then choose for Jabiim. In
             the mod's Data\\Art\\Maps by default; with --map-loose in <game>\\corruption\\Data\\Art\\Maps (a new file,
             with a .cu8g_probe.txt marker next to it; cu8g_cleanup.py removes both).

Nothing from the game is stored in the repository: shipped files are read from the install (loose files / MEGs in the
FoC order, ../common/ted_owners.py GameFiles) or, for the offline check, from the text zip (no .dat, no .ted: map sites
fall back to fixed points and maps-hit cannot be built). The probe site of each battle is chosen from the map file:
space = the point farthest from both start positions and from every map object inside the central 70 % of the map;
land = a dry, open square far from the starts (../oi_abilities/build_oi_mod.py terrain reader).
"""
import argparse
import hashlib
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
sys.path.insert(0, os.path.join(PROBE_MOD, 'oi_abilities'))
import ted_owners  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'CU8GProbe'
MARKER = 'CU8G_PROBE_MARKER.txt'
SESSIONS = ('stick', 'abil', 'maps-miss', 'maps-hit')
DEFAULT_GAME = ted_owners.DEFAULT_GAME

FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
CAMPAIGNFILES = 'DATA\\XML\\CAMPAIGNFILES.XML'
PLANETS = 'DATA\\XML\\PLANETS.XML'
CANDEROUS = 'DATA\\XML\\UNITS_LAND_UNDERWORLD_CANDEROUS_TANK.XML'
UNIQUEUNITS = 'DATA\\XML\\UNIQUEUNITS.XML'
HIT_SOURCE_MAP = '_MP_LAND_ALDERAAN.TED'
HIT_MAP = '_MP_LAND_CU8G_ALDERAAN.TED'
LOOSE_SUFFIX = '.cu8g_probe.txt'

SPACE_MAP = '_MP_SPACE_KUAT'
LAND_MAP = '_MP_LAND_DAGOBAH'
SPACE_FALLBACK_SITE = {'x': -3500.0, 'y': 3500.0, 'z': 0.0}    # Kuat: the empty diagonal (starts are at -4000,-4100 / 4300,4250)
LAND_FALLBACK_SITE = {'x': 4120.0, 'y': 840.0, 'z': 0.0}       # Dagobah: the OI probe's dry MAL_LAND point (run 2026-11-04)

# ---- per-session probe configuration (becomes the C8_CFG Lua table) ------------------------------------------------

STICK = {
    'human_faction': 'Rebel',
    'trials': ['A', 'B', 'C', 'A', 'B', 'C'],
    'values': {'B': -1.0, 'C': 1000.0},          # A: no call
    'frigate': 'Nebulon_B_Frigate',              # Targeting_Stickiness_Time_Threshold 5.0, priority set Frigate
    'victim': 'Tartan_Patrol_Cruiser',           # Corvette: Frigate-set score 3.0; hull 500, shield 800
    'victim_dx': 800.0,                          # inside the frigate's Targeting_Max_Attack_Distance 1000
    'helper': 'Nebulon_B_Frigate', 'helpers': 2, 'helper_dx': 400.0, 'helper_dy': 400.0,
    'newcomer': 'TIE_Bomber', 'newcomer_alt': 'TIE_Bomber_Squadron',   # Bomber: Frigate-set score 2.0
    'newcomer_dx': -350.0,                       # closer to F than V (XY rule, TACTICAL_COMBAT sec. 2.4)
    'start_frame': 300, 'acquire_max': 300, 'ttk_trigger': 4.0, 'hull_min': 0.5, 'wait_max': 600,
    'watch_max': 900, 'after_death': 90, 'gap': 120,
}
LURE = {
    'human_faction': 'Empire',
    'trials': ['Canderous_Assault_Tank', 'Canderous_Assault_Tank_Captured', 'Canderous_Assault_Tank'],
    'pulsing': {'Canderous_Assault_Tank': 'No (mod)', 'Canderous_Assault_Tank_Captured': 'Yes (shipped)'},
    'ability': 'LURE', 'decoy': 'T2B_Tank', 'decoy_dx': 330.0, 'victim': 'AT_ST_Walker', 'victim_dx': 180.0,
    'divert_every': 40, 'watch': 300, 'start_frame': 450, 'gap': 150,
}
CANCEL = {
    'human_faction': 'Empire', 'trials': 2,
    'target': 'Accuser_Star_Destroyer', 'ability': 'TRACTOR_BEAM', 'ability2': 'ENERGY_WEAPON',
    'victim': 'Corellian_Corvette', 'victim_dx': 700.0, 'user': 'Home_One', 'user_dx': -1000.0,
    'base_len': 300, 'cancel_len': 750, 'restore_len': 900, 'start_frame': 450, 'gap': 150,
}

TEXTS = {
    'stick': [
        ('TEXT_CU8G_STICK_WAIT', 'CU8G PROBE: stickiness probe armed. Give no orders; the trials start at 10 s.'),
        ('TEXT_CU8G_STICK_RUNNING', 'CU8G PROBE: stickiness trials running (about 3 minutes). Give no orders.'),
        ('TEXT_CU8G_STICK_DONE', 'CU8G PROBE: stickiness trials done. Quit the battle and the game.'),
    ],
    'abil': [
        ('TEXT_CU8G_ABIL_WAIT', 'CU8G PROBE: ability probe armed. Give no orders; it starts at 15 s.'),
        ('TEXT_CU8G_LURE_DONE', 'CU8G PROBE: lure trials done. Quit this battle; next: the SPACE skirmish on Kuat.'),
        ('TEXT_CU8G_CANCEL_DONE', 'CU8G PROBE: cancel trials done. Quit the battle and the game.'),
    ],
    'maps': [
        ('TEXT_CU8G_MAPS_BEGIN', 'CU8G PROBE: missing-maps campaign loaded'),
        ('TEXT_CU8G_MAPS_RUNNING', 'CU8G PROBE: move ground units to Jabiim (land), the fleet to Thyferra (space); choose Fight.'),
        ('TEXT_CU8G_CAMP_NAME', 'CU8G PROBE: MISSING MAPS'),
        ('TEXT_CU8G_CAMP_DESC', 'cu8g t08 probe. Play the REBEL. Invade Jabiim with the ground units (land battle) and '
                                'attack Thyferra with the fleet (space battle); choose Fight, never auto-resolve. '
                                'Never save.'),
    ],
}

CAMPAIGN = r'''<?xml version="1.0"?>
<Campaigns>
	<!-- cu8g t08 probe (tools/probe_mod/cu8g_probes) -->
	<Campaign Name="CU8G_Probe_Maps">
		<Campaign_Set> CU8G_Probe_Maps_Set </Campaign_Set>
		<Sort_Order>0</Sort_Order>
		<Text_ID> TEXT_CU8G_CAMP_NAME </Text_ID>
		<Description_Text> TEXT_CU8G_CAMP_DESC </Description_Text>
		<Camera_Shift_X> 0.0 </Camera_Shift_X>
		<Camera_Shift_Y> 0.0 </Camera_Shift_Y>
		<Camera_Distance> 1200.0 </Camera_Distance>
		<Locations>
				Galaxy_Core_Art_Model,
				Abregado_Rae,
				Jabiim,
				Thyferra
		</Locations>
		<Trade_Routes>
				Abregado_Rae_Jabiim,
				Thyferra_Abregado_Rae
		</Trade_Routes>
		<Home_Location> Rebel, Abregado_Rae </Home_Location>
		<Home_Location> Empire, Jabiim </Home_Location>
		<Starting_Active_Player> Rebel </Starting_Active_Player>
		<Rebel_Story_Name>CU8G_Story_Plots_Maps.xml</Rebel_Story_Name>
		<AI_Player_Control> Empire, None </AI_Player_Control>
		<AI_Player_Control> Underworld, None </AI_Player_Control>
		<AI_Player_Control> Pirates, None </AI_Player_Control>
		<Human_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</Human_Victory_Conditions>
		<AI_Victory_Conditions>
				Galactic_All_Planets_Controlled
		</AI_Victory_Conditions>
		<Markup_Filename>Empire, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Pirates, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Rebel, DefaultGalacticHints </Markup_Filename>
		<Markup_Filename>Underworld, DefaultGalacticHints </Markup_Filename>
		<Supports_Custom_Settings>False</Supports_Custom_Settings>
		<Show_Completed_Tab>False</Show_Completed_Tab>
		<Starting_Credits> Rebel, 0 </Starting_Credits>
		<Starting_Tech_Level> Rebel, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Rebel, 1 </Max_Tech_Level>
		<Starting_Credits> Empire, 0 </Starting_Credits>
		<Starting_Tech_Level> Empire, 1 </Starting_Tech_Level>
		<Max_Tech_Level> Empire, 1 </Max_Tech_Level>
		<Starting_Forces> Rebel, Abregado_Rae, R_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Rebel_Infantry_Squad </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Rebel_Infantry_Squad </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Rebel_Light_Tank_Brigade </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Nebulon_B_Frigate </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Nebulon_B_Frigate </Starting_Forces>
		<Starting_Forces> Rebel, Abregado_Rae, Corellian_Corvette </Starting_Forces>
		<Starting_Forces> Empire, Jabiim, E_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Empire, Jabiim, Imperial_Stormtrooper_Squad </Starting_Forces>
		<Starting_Forces> Empire, Thyferra, E_Ground_Barracks </Starting_Forces>
		<Starting_Forces> Empire, Thyferra, Tartan_Patrol_Cruiser </Starting_Forces>
	</Campaign>
</Campaigns>
'''

PLOTS = '''<?xml version="1.0" ?>
<Story_Mode_Plots>
	<Lua_Script>CU8G_Story_Maps</Lua_Script>
	<Active_Plot>CU8G_Story_Maps.xml</Active_Plot>
	<Suspended_Plot />
</Story_Mode_Plots>
'''

STORY = '''<?xml version="1.0" ?>
<!-- cu8g t08 probe (tools/probe_mod/cu8g_probes) -->
<Story>
	<Event Name="C8G_Begin">
		<Event_Type>STORY_ELAPSED</Event_Type>
		<Event_Param1>1</Event_Param1>
		<Reward_Type>SCREEN_TEXT</Reward_Type>
		<Reward_Param1>TEXT_CU8G_MAPS_BEGIN</Reward_Param1>
		<Reward_Param2>10</Reward_Param2>
	</Event>
</Story>
'''

# (planet, tag emptied, new Terrain)
PLANET_PATCHES = [('Jabiim', 'Land_Tactical_Map', 'Temperate'), ('Thyferra', 'Space_Tactical_Map', 'Temperate')]


# ---- shipped files ---------------------------------------------------------------------------------------------------

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


def prepare(out, label):
    if os.path.exists(out):
        if not os.path.exists(os.path.join(out, MARKER)):
            raise SystemExit('refusing to overwrite %s: not a cu8g probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER), ('cu8g probe mod (session %s, tools/probe_mod/cu8g_probes); removed by '
                                      'cleanup_cu8g.cmd\r\n' % label).encode('ascii'))


# ---- probe sites -------------------------------------------------------------------------------------------------------

def map_data(src, name):
    if src.zip:
        return None, None
    return src.read('DATA\\ART\\MAPS\\%s.TED' % name.upper())


def space_site(src, name):
    """Point of the central 70 % of the map farthest from both start positions and every map object."""
    data, msrc = map_data(src, name)
    if data is None:
        return dict(SPACE_FALLBACK_SITE), 'fallback (map %s not readable)' % name
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
            score = min(d_obj, d_start / 3.0)   # starts weigh three times: the bases and fleets are there
            if best is None or score > best[0] + 1e-6:
                best = (score, x, y, d_obj, d_start)
    _s, x, y, d_obj, d_start = best
    return ({'x': x, 'y': y, 'z': 0.0},
            '%s (%s): %.0f x %.0f, %d objects; clearance %.0f to objects, %.0f to starts' % (
                name, msrc, w, h, len(pl), d_obj, d_start))


def land_site(game, name):
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
    except (OSError, ValueError, KeyError, StopIteration, struct.error, ImportError) as e:
        return dict(LAND_FALLBACK_SITE), 'fallback (%s: %s)' % (name, e)


# ---- Lua config ----------------------------------------------------------------------------------------------------

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
    return '-- generated by build_cu8g_mod.py (session %s)\nC8_CFG = %s\n' % (cfg['session'], lua_value(cfg, ''))


def freestore(src, cfg, hooks):
    shipped, fs_src = src.need(FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    tail = lua_config(cfg) + '\n' + kit_text('cu8g_common.lua') + '\n' + '\n'.join(kit_text(h) for h in hooks)
    return crlf(text + '\n\n' + tail), fs_src


def texts_dat(src, pairs):
    mt, _ = src.need(MASTERTEXT)
    tmp = os.path.join(os.environ.get('TEMP', HERE), 'cu8g_mt_%d.tmp' % os.getpid())
    with open(tmp, 'wb') as f:
        f.write(mt)
    try:
        keys = {k for k, _ in pairs}
        base = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    finally:
        os.remove(tmp)
    return dat_bytes(base + list(pairs))


# ---- XML patches -----------------------------------------------------------------------------------------------------

def patch_entity(text, tag, name, fn):
    """Apply fn to the body of <tag Name="name">...</tag> (exactly one such element)."""
    rx = re.compile(r'(<%s\s+Name\s*=\s*"%s"\s*>)(.*?)(</%s\s*>)' % (tag, re.escape(name), tag), re.S)
    ms = list(rx.finditer(text))
    if len(ms) != 1:
        raise SystemExit('expected one <%s Name="%s">, found %d' % (tag, name, len(ms)))
    m = ms[0]
    return text[:m.start(2)] + fn(m.group(2)) + text[m.end(2):]


def sub_once(body, rx, repl, what):
    new, n = re.subn(rx, repl, body, count=1, flags=re.S)
    if n != 1:
        raise SystemExit('patch failed: ' + what)
    return new


def patch_planets(data):
    text = data.decode('latin-1')
    for planet, tag, terrain in PLANET_PATCHES:
        def fn(body, tag=tag, terrain=terrain, planet=planet):
            body = sub_once(body, r'<%s\s*>[^<]*</%s\s*>|<%s\s*/>' % (tag, tag, tag), '<%s />' % tag,
                            '%s %s' % (planet, tag))
            return sub_once(body, r'<Terrain>[^<]*</Terrain>', '<Terrain>%s</Terrain>' % terrain, planet + ' Terrain')
        text = patch_entity(text, 'Planet', planet, fn)
    return text.encode('latin-1')


def patch_canderous(data):
    text = data.decode('latin-1')
    text = patch_entity(text, 'GroundVehicle', 'Canderous_Assault_Tank',
                        lambda b: sub_once(b, r'<Is_Pulsing>\s*Yes\s*</Is_Pulsing>', '<Is_Pulsing>No</Is_Pulsing>',
                                           'Canderous Is_Pulsing'))
    return text.encode('latin-1')


def patch_home_one(data):
    text = data.decode('latin-1')

    def fn(body):
        rx = r'<!--\s*(<Cancel_Special_Ability Name="Akbar_Cancel_Special_Ability">.*?</Cancel_Special_Ability>)\s*-->'
        return sub_once(body, rx, r'\1', 'Home_One Akbar_Cancel_Special_Ability')
    text = patch_entity(text, 'UniqueUnit', 'Home_One', fn)
    return text.encode('latin-1')


def patch_hit_map(data):
    """The map with chunk 0 mini 05 (faction) := 1. Checks type 1 (land), terrain 0, players >= 2, custom flag 0."""
    b = bytearray(data)
    cid, size = struct.unpack_from('<II', b, 0)
    if cid != 0 or size & 0x80000000:
        raise SystemExit('%s: first chunk is not the map-info chunk 0' % HIT_SOURCE_MAP)
    pos, end = 8, 8 + (size & 0x7FFFFFFF)
    found = {}
    while pos < end:
        mid, msz = b[pos], b[pos + 1]
        found[mid] = (pos + 2, msz)
        pos += 2 + msz
    get = lambda k: struct.unpack_from('<I', b, found[k][0])[0] if k in found and found[k][1] == 4 else None
    if get(1) != 1 or get(6) != 0 or (get(2) or 0) < 2 or get(5) != 3:
        raise SystemExit('%s: unexpected map info (type %s terrain %s players %s faction %s)' % (
            HIT_SOURCE_MAP, get(1), get(6), get(2), get(5)))
    if 0xB in found and b[found[0xB][0]] != 0:
        raise SystemExit('%s: custom flag set' % HIT_SOURCE_MAP)
    struct.pack_into('<I', b, found[5][0], 1)
    return bytes(b)


def loose_map_paths(game):
    d = os.path.join(game, 'corruption', 'Data', 'Art', 'Maps')
    return d, os.path.join(d, HIT_MAP), os.path.join(d, HIT_MAP + LOOSE_SUFFIX)


def install_loose_map(game, data):
    d, path, marker = loose_map_paths(game)
    if os.path.exists(path) and not os.path.exists(marker):
        raise SystemExit('refusing to overwrite %s: no probe marker' % path)
    created = []
    p = d
    while not os.path.isdir(p):
        created.append(p)
        p = os.path.dirname(p)
    os.makedirs(d, exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    with open(marker, 'w') as f:
        json.dump({'sha256': hashlib.sha256(data).hexdigest(), 'created_dirs': created,
                   'note': 'cu8g t08 probe map; removed by tools/probe_mod/cu8g_probes/cleanup_cu8g.cmd'}, f, indent=1)
    return path


# ---- build -------------------------------------------------------------------------------------------------------------

def build(session, game, out, source_zip=None, no_text=False, map_loose=False, install_game=None):
    """Build Mods\\CU8GProbe for `session` into `out` (reading shipped files from `game` or the zip). With map_loose
    (maps-hit) the patched map goes to <install_game>\\corruption\\Data\\Art\\Maps. Returns the config dict."""
    if session not in SESSIONS:
        raise SystemExit('unknown session ' + session)
    src = Source(game, source_zip)
    prepare(out, session)
    cfg = {'session': session}
    files = []

    def put(rel, data):
        write(os.path.join(out, rel), data)
        files.append(rel)

    if session == 'stick':
        site, note = space_site(src, SPACE_MAP)
        cfg.update({'ai_faction': 'EMPIRE', 'map': SPACE_MAP, 'stick': dict(STICK, site=site), 'site_note': note})
        fs, fs_src = freestore(src, cfg, ['cu8g_stick.lua'])
        texts = TEXTS['stick']
    elif session == 'abil':
        lsite, lnote = land_site(game, LAND_MAP) if not src.zip else (dict(LAND_FALLBACK_SITE), 'fallback (offline)')
        ssite, snote = space_site(src, SPACE_MAP)
        cfg.update({'ai_faction': 'REBEL', 'map': LAND_MAP + ' + ' + SPACE_MAP, 'lure': dict(LURE, site=lsite),
                    'cancel': dict(CANCEL, site=ssite), 'site_note': 'land ' + lnote + '; space ' + snote})
        fs, fs_src = freestore(src, cfg, ['cu8g_lure.lua', 'cu8g_cancel.lua'])
        put('Data\\XML\\Units_Land_Underworld_Canderous_Tank.xml', patch_canderous(src.need(CANDEROUS)[0]))
        put('Data\\XML\\UniqueUnits.xml', patch_home_one(src.need(UNIQUEUNITS)[0]))
        texts = TEXTS['abil']
    else:
        cfg.update({'ai_faction': 'ANY', 'map': 'campaign CU8G_Probe_Maps'})
        fs, fs_src = freestore(src, cfg, ['cu8g_maps_tact.lua'])
        cf = src.need(CAMPAIGNFILES)[0].decode('latin-1')
        i = cf.index('</Campaign_Files>')
        put('Data\\XML\\CampaignFiles.xml', (cf[:i] + '\t<File>CU8G_Probe_Campaigns.xml</File>\r\n' + cf[i:]).encode('latin-1'))
        put('Data\\XML\\CU8G_Probe_Campaigns.xml', crlf(CAMPAIGN))
        put('Data\\XML\\CU8G_Story_Plots_Maps.xml', crlf(PLOTS))
        put('Data\\XML\\CU8G_Story_Maps.xml', crlf(STORY))
        put('Data\\Scripts\\Story\\CU8G_Story_Maps.lua', crlf(kit_text('cu8g_maps_story.lua').replace('@@SESSION@@', session)))
        put('Data\\XML\\Planets.xml', patch_planets(src.need(PLANETS)[0]))
        texts = TEXTS['maps']
        cfg['planet_patches'] = ['%s: %s emptied, Terrain %s' % p for p in PLANET_PATCHES]
        if session == 'maps-hit':
            data, msrc = map_data(src, HIT_SOURCE_MAP[:-4])
            if data is None:
                raise SystemExit('maps-hit needs the installed %s (not in the text zip)' % HIT_SOURCE_MAP)
            patched = patch_hit_map(data)
            cfg['hit_map'] = {'source': '%s (%s)' % (HIT_SOURCE_MAP, msrc), 'name': HIT_MAP,
                              'sha256': hashlib.sha256(patched).hexdigest()}
            if map_loose:
                cfg['hit_map']['where'] = install_loose_map(install_game or game, patched)
            else:
                put('Data\\Art\\Maps\\' + HIT_MAP, patched)
                cfg['hit_map']['where'] = os.path.join(out, 'Data', 'Art', 'Maps', HIT_MAP)
    put('Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua', fs)
    if not no_text:
        put('Data\\Text\\MasterTextFile_English.dat', texts_dat(src, texts))
    cfg['files'] = files
    cfg['freestore_source'] = fs_src
    return cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('session', choices=SESSIONS)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--map-loose', action='store_true')
    ap.add_argument('--config-out')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.session, a.game, out, a.source_zip, a.no_text, a.map_loose)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s (%s): %s' % (out, a.session, ', '.join(cfg['files'])))
    if 'site_note' in cfg:
        print('site:', cfg['site_note'])


if __name__ == '__main__':
    main()
