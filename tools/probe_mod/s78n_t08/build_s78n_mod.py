#!/usr/bin/env python3
"""build_s78n_mod.py - builds the s78n t08 probe mod (Mods\\S78NProbe) from the installed game.

  build_s78n_mod.py [--game DIR] [--out MODDIR] [--source-zip extracted\\foc_data_text.zip] [--no-text]
                    [--config-out FILE] [--space-only] [--no-bone-read]

Two retail checks of port behaviour that had no retail trace (roadmap s78n t08), one mod, two skirmish battles:
  (A) space, _MP_SPACE_KUAT, human EMPIRE vs AI REBEL: a Star_Destroyer's TRACTOR_BEAM (SD_Tractor_Beam_Attack_Ability,
      Activation_Max_Range R) on a Nebulon_B_Frigate set at a fixed centre distance d with a fixed facing, along the
      line to the user or exactly across it (docs/re/TACTICAL_COMBAT.md sec. 5.4.5: E along = the collision-bound
      model Y half x Scale_Factor, across = the X half; threshold R + E): in place or not, and where the beam starts
      when the target is stepped closer. Second version (2026-11-10, after run 20261109-200249): spawn, Teleport to the
      exact spots, Face_Immediate, Suspend_Locomotor, facing read through two model bones (Get_Bone_Position) until
      it is settled.
  (B) land, _MP_LAND_DAGOBAH (3 start positions), human EMPIRE + allied AI UNDERWORLD (team 1) vs AI REBEL (team 2):
      Emperor_Palpatine's Emperor_Land_Combat_Bonus (Ground_Automatic, Vehicle | Infantry | Air, Health_Bonus 0.25) on
      AT_ST_Walker units of the human, the allied AI and the enemy AI, near the Emperor and far from him (sec. 5.4.3
      item 4: the accept takes units allied with the source owner, own player included; the port has no radius).
  --space-only builds (A) only (the land probe is not registered; run 20261109-200249 already answered (B));
  --no-bone-read leaves out the facing read (then each trial waits settle_nobone frames before the order).
Files of the mod (MODPATH=Mods\\S78NProbe):
  Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua   shipped script + generated SN_CFG + s78n_probe.lua
  Data\\Text\\MasterTextFile_English.dat               shipped texts + the TEXT_S78N_* cue strings
  S78N_PROBE_MARKER.txt                               marker: cleanup deletes the folder only when it is there
No XML is changed. Nothing from the game is stored in the repository: shipped files are read from the install
(../common/ted_owners.py GameFiles; models from Models.meg, s78n_extent.py) or, for the offline check, from the text
zip (no .dat: --no-text; fixed sites and the extents in FALLBACK_EXTENTS).
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
sys.path.insert(0, os.path.join(PROBE_MOD, 'cu8g_probes'))
import ted_owners  # noqa: E402
import s78n_extent  # noqa: E402
from build_probe import dat_read, dat_bytes  # noqa: E402

MOD_NAME = 'S78NProbe'
MARKER = 'S78N_PROBE_MARKER.txt'
DEFAULT_GAME = ted_owners.DEFAULT_GAME
FREESTORE = 'DATA\\SCRIPTS\\FREESTORE\\BUSYTACTICALFREESTORE.LUA'
MASTERTEXT = 'DATA\\TEXT\\MASTERTEXTFILE_ENGLISH.DAT'
FOC_ZIP = os.path.join(ROOT, 'extracted', 'foc_data_text.zip')
SPACE_MAP = '_MP_SPACE_KUAT'
LAND_MAP = '_MP_LAND_DAGOBAH'
SPACE_FALLBACK_SITE = {'x': -3500.0, 'y': 3500.0, 'z': 0.0}     # cu8g's Kuat fallback (the empty diagonal)
LAND_FALLBACK_SITE = {'x': 4120.0, 'y': 840.0, 'z': 0.0}        # the dry Dagobah point of the OI / cu8g / s78m probes
LAND_FALLBACK_FAR = {'x': 2900.0, 'y': 1500.0, 'z': 0.0}        # [offline only] a second point about 1400 away
# Offline (text zip, no models): the values s78n_extent.py read from the install on 2026-11-09 / 2026-11-10.
# The old port extents (|model bbox max.x|, |max.y| x scale), printed for reference only:
FALLBACK_EXTENTS = {'Nebulon_B_Frigate': (23.48, 111.86), 'Corellian_Corvette': (17.0, 40.1)}
# sec. 5.4.5 extents (along the facing, across it) and the facing bones (front, rear) of the model
FALLBACK_RANGE = {'Nebulon_B_Frigate': (109.95, 23.48), 'Corellian_Corvette': (41.82, 17.75)}
FALLBACK_BONES = {'Nebulon_B_Frigate': ('missile_target00', 'missile_target01')}

# The probe configuration (becomes the SN_CFG Lua table). Frames are mode frames (30 per second).
CFG = {
    'session': 's78n_t08',
    'ai_faction': 'REBEL',            # the AI whose free store runs the probe in both battles (the enemy AI)
    'human_faction': 'Empire',        # the driver's side
    'ally_faction': 'Underworld',     # (B) the allied AI (same team as the human in the skirmish menu)
    'tv': {
        'user': 'Star_Destroyer', 'ability': 'TRACTOR_BEAM', 'target': 'Nebulon_B_Frigate',
        'start_frame': 450,   # 15 s into the battle
        'place': 10,          # frames after the spawn: Teleport both to their exact spots, Face_Immediate, lock
        'settle_min': 60,     # frames after placing before the order, at least
        'settle_poll': 15,    # frames between facing reads while settling
        'settle_tol': 0.5,    # settled: two consecutive reads of both bones (and of X) within this distance
        'settle_max': 600,    # order anyway after this many frames (recorded SETTLE timeout=1)
        'settle_nobone': 600,  # frames to wait before the order when there is no facing read (bones off)
        'face_dist': 600.0,   # "across": X faces the point this far from it along +y (exactly 90 deg to the line)
        'ctrl_wait': 90,      # control trial, user locked, no beam this long after the order: unlock the user (ULOCK)
        'inplace': 30,        # a beam within this many frames of the order (nothing moved) is "in place"
        'hold': 150,          # frames at the fixed distance before X is stepped closer (user still)
        'step': 1.0,          # distance per poll X is teleported toward the user (facing kept)
        'stop_below': 20.0,   # stepping ends when the centre distance falls under R - this
        'watch': 900,         # frames after the order before a trial without a beam ends
        'tail': 30,           # frames recorded after the engagement
        'gap': 45,            # frames between trials
        'face_every': 10,     # facing read every this many polls while watching (and at the beam start)
        'ulock': True,        # Suspend_Locomotor(true) on the user (no walking: a clean in / out at d)
        'xlock': True,        # Suspend_Locomotor(true) on the target after Face_Immediate (no turning)
        'camera': True,       # Point_Camera_At(position) at each trial start (framing only)
        'ctrl_d': 950.0,
        'along_offsets': [-15.0, -5.0, 5.0, 15.0],    # around R + E_along (1109.95): 1095, 1105, 1115, 1125
        'across_offsets': [-13.5, -5.5, 5.5, 13.5],   # around R + E_across (1023.48): 1010, 1018, 1029, 1037
    },
    'cb': {
        'hero': 'Emperor_Palpatine', 'hero_alt': 'Emperor_Palpatine_Team', 'aura': 'Emperor_Land_Combat_Bonus',
        'unit': 'AT_ST_Walker',
        'start_frame': 450,
        'fix_ally': True,     # Make_Ally both ways when the menu did not ally Empire and Underworld (recorded)
        'invuln': True,       # test units invulnerable (Lua Take_Damage is damage kind 0, which passes invulnerability)
        # offsets from the near site S (the Emperor stands on S) and the far site S2
        'pre': {'own': [140.0, 70.0], 'ally': [-140.0, 70.0], 'enemy': [0.0, -170.0]},
        'near': {'own': [140.0, -70.0], 'ally': [-140.0, -70.0], 'enemy': [0.0, 170.0]},
        'far': {'own': [70.0, 0.0], 'ally': [-70.0, 0.0], 'enemy': [0.0, 90.0]},
        # schedule, frames after the start
        't_pre_spawn': 0, 't_pre_protect': 45, 't_pre_dmg': 60, 't_pre_check': 90, 't_hero': 150,
        't_post_spawn': 210, 't_post_protect': 255, 't_post_h0': 285, 't_post_dmg': 290, 't_post_h1': 320,
        't_hero_off': 360, 't_end': 450, 'recheck': 30,
    },
}

TEXTS = [
    ('TEXT_S78N_TB_ARMED', 'S78N PROBE: tractor beam trials armed. Give no orders, do not pause. About 3 minutes.'),
    ('TEXT_S78N_TB_DONE', 'S78N PROBE: tractor beam trials recorded. Esc > Quit to the main menu; next: the LAND '
                          'skirmish on Dagobah.'),
    ('TEXT_S78N_CB_ARMED', 'S78N PROBE: aura trials armed. Give no orders, do not pause. About 2 minutes.'),
    ('TEXT_S78N_CB_DONE', 'S78N PROBE: aura trials recorded. Esc > Quit, then quit the game.'),
]
TEXTS_SPACE_ONLY = [(k, 'S78N PROBE: tractor beam trials recorded. Esc > Quit, then quit the game.')
                    if k == 'TEXT_S78N_TB_DONE' else (k, v) for k, v in TEXTS]


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
            raise SystemExit('refusing to overwrite %s: not an s78n probe folder' % out)
        shutil.rmtree(out)
    os.makedirs(out)
    write(os.path.join(out, MARKER), b's78n t08 probe mod (tools/probe_mod/s78n_t08); removed by cleanup_s78n.cmd\r\n')


# ---- XML values -----------------------------------------------------------------------------------------------------

def _type_body(texts, name):
    for t in texts:
        m = re.search(r'<(\w+)\s+Name="%s"\s*>(.*?)</\1\s*>' % re.escape(name), t, re.S)
        if m:
            return m.group(2)
    return None


def xml_values(zpath=FOC_ZIP):
    """R (the user's tractor Activation_Max_Range), the test unit's Tactical_Health, the aura's Health_Bonus_Percentage
    and filter, read from the extracted FoC XML (the shipped game has the same text)."""
    with zipfile.ZipFile(zpath) as z:
        texts = [z.read(n).decode('latin-1') for n in z.namelist() if n.lower().endswith('.xml')]
    out = {}
    user = CFG['tv']['user']
    body = _type_body(texts, user)
    seen = 0
    while body is not None and 'Tractor_Beam_Attack_Ability' not in body and seen < 5:
        var = re.search(r'<Variant_Of_Existing_Type>\s*([^<\s]+)\s*<', body)
        body = _type_body(texts, var.group(1)) if var else None
        seen += 1
    m = re.search(r'<Tractor_Beam_Attack_Ability\s+Name="([^"]+)"\s*>(.*?)</Tractor_Beam_Attack_Ability>', body or '',
                  re.S)
    if not m:
        raise SystemExit('%s: no Tractor_Beam_Attack_Ability found' % user)
    out['tb_ability_name'] = m.group(1)
    out['R'] = float(re.search(r'<Activation_Max_Range>\s*([-\d.]+)', m.group(2)).group(1))
    rmin = re.search(r'<Activation_Min_Range>\s*([-\d.]+)', m.group(2))
    out['R_min'] = float(rmin.group(1)) if rmin else 0.0
    cats = re.search(r'<Applicable_Unit_Categories>(.*?)</Applicable_Unit_Categories>', m.group(2), re.S)
    out['tb_categories'] = ' '.join(cats.group(1).split()) if cats else ''
    unit = _type_body(texts, CFG['cb']['unit'])
    out['unit_health'] = float(re.search(r'<Tactical_Health>\s*([-\d.]+)', unit).group(1))
    sh = re.search(r'<Shield_Points>\s*([-\d.]+)', unit)
    out['unit_shield'] = float(sh.group(1)) if sh else 0.0
    hero = _type_body(texts, CFG['cb']['hero'])
    a = re.search(r'<Combat_Bonus_Ability\s+Name="%s"\s*>(.*?)</Combat_Bonus_Ability>' % CFG['cb']['aura'], hero or '',
                  re.S)
    if not a:
        raise SystemExit('%s: no %s found' % (CFG['cb']['hero'], CFG['cb']['aura']))
    out['aura_health_bonus'] = float(re.search(r'<Health_Bonus_Percentage>\s*([-\d.]+)', a.group(1)).group(1))
    out['aura_style'] = re.search(r'<Activation_Style>\s*(\w+)', a.group(1)).group(1)
    out['aura_categories'] = ' '.join(re.search(r'<Applicable_Unit_Categories>(.*?)<', a.group(1), re.S).group(1).split())
    return out


# ---- sites ----------------------------------------------------------------------------------------------------------

def space_site(src):
    """cu8g's space site (the central-70 % point farthest from the starts and every map object)."""
    try:
        import build_cu8g_mod
        return build_cu8g_mod.space_site(src, SPACE_MAP)
    except (OSError, ValueError, KeyError, StopIteration, struct.error, ImportError) as e:
        return dict(SPACE_FALLBACK_SITE), 'fallback (%s)' % e


def land_sites(game):
    """S: the dry, open 500 x 500 square farthest from the starts (s78m method); S2: the dry square farthest from the
    starts at least 1200 from S. Fallbacks: the fixed Dagobah points."""
    try:
        import build_oi_mod
        t = build_oi_mod.load_terrain(game, LAND_MAP)
        if t is None:
            raise ValueError('map not readable')
        g = build_oi_mod.Grid(t)
        cell = build_oi_mod.CELL
        cands = []
        for y in range(0, g.H, 2):
            for x in range(0, g.W, 2):
                if not g.in_box(x, y, 400.0) or not g.square_all(g.sat_land, x, y, 12):
                    continue
                if g.wh is not None and g.dist_water[y][x] * cell < 300:
                    continue
                cands.append((g.far(x, y), x, y))
        if not cands:
            raise ValueError('no dry 500 x 500 square')
        cands.sort(reverse=True)
        _, x1, y1 = cands[0]
        s1 = g.point(x1, y1)
        far = [c for c in cands if math.hypot((c[1] - x1) * cell, (c[2] - y1) * cell) >= 1200.0]
        if not far:
            raise ValueError('no second dry square 1200 from the first')
        _, x2, y2 = far[0]
        s2 = g.point(x2, y2)
        note = '%s (%s): S %.0f from the nearest start, S2 %.0f from the nearest start, %.0f from S' % (
            LAND_MAP, t['source'], cands[0][0], far[0][0], math.hypot(s1['x'] - s2['x'], s1['y'] - s2['y']))
        return ({'x': s1['x'], 'y': s1['y'], 'z': s1['z']}, {'x': s2['x'], 'y': s2['y'], 'z': s2['z']}, note)
    except (OSError, ValueError, KeyError, StopIteration, struct.error, ImportError, AttributeError) as e:
        return dict(LAND_FALLBACK_SITE), dict(LAND_FALLBACK_FAR), 'fallback (%s: %s)' % (LAND_MAP, e)


# ---- the trial list -------------------------------------------------------------------------------------------------

def tv_trials(tv):
    """Ten placements along +x from the user (exact, by Teleport). "along": X faces the user (|f.u| = 1, sec. 5.4.5
    E = e_along); "across": X faces +y (f.u = 0, E = e_across). Per facing a control at ctrl_d, then four distances
    around R + E of that facing (integers)."""
    R = tv['R']
    out = [{'name': 'ctrl_along', 'face': 'along', 'd': float(tv['ctrl_d'])}]
    for o in tv['along_offsets']:
        d = float(round(R + tv['e_along'] + o))
        out.append({'name': 'along_%d' % d, 'face': 'along', 'd': d})
    out.append({'name': 'ctrl_across', 'face': 'across', 'd': float(tv['ctrl_d'])})
    for o in tv['across_offsets']:
        d = float(round(R + tv['e_across'] + o))
        out.append({'name': 'across_%d' % d, 'face': 'across', 'd': d})
    for i, t in enumerate(out):
        t['k'] = i + 1
    return out


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


LUA_SKIP = ('files', 'freestore_source', 'site_note', 'map', 'extent', 'xml')


def lua_config(cfg):
    lua_cfg = {k: v for k, v in cfg.items() if k not in LUA_SKIP}
    return '-- generated by build_s78n_mod.py\nSN_CFG = %s\n' % lua_value(lua_cfg, '')


def config(game, offline, space_only=False, bone_read=True):
    cfg = json.loads(json.dumps(CFG))
    cfg['space_only'] = bool(space_only)
    xv = xml_values()
    cfg['xml'] = xv
    tv = cfg['tv']
    tgt = tv['target']
    if offline:
        ex, ey = FALLBACK_EXTENTS[tgt]
        ea, ec = FALLBACK_RANGE[tgt]
        bones = FALLBACK_BONES.get(tgt)
        ext = {'type': tgt, 'ex': ex, 'ey': ey, 'along': ea, 'across': ec, 'bones': bones and list(bones),
               'note': 'offline fallback (s78n_extent.py values of 2026-11-09 / 2026-11-10)'}
    else:
        old = s78n_extent.extents(game, tgt)
        new = s78n_extent.range_extents(game, tgt)
        bones = new['bones'] and list(new['bones'][:2])
        ext = {'type': tgt, 'ex': old['ex'], 'ey': old['ey'], 'along': new['along'], 'across': new['across'],
               'half': new['half'], 'bones': bones, 'bone_xy': new['bones'] and new['bones'][2],
               'note': new['note'] + '; old port extents: ' + old['note']}
        if ext['ex'] is None:
            ext['ex'], ext['ey'] = FALLBACK_EXTENTS[tgt]
            ext['note'] += '; old port extents: fallback'
        if ext['along'] is None:
            ext['along'], ext['across'] = FALLBACK_RANGE[tgt]
            ext['note'] += '; sec. 5.4.5 extents: fallback, no facing bones'
            bones = None
    cfg['extent'] = ext
    tv['R'] = xv['R']
    tv['e_along'] = round(float(ext['along']), 3)
    tv['e_across'] = round(float(ext['across']), 3)
    tv['old_along'] = round(float(ext['ex']), 3)     # the old port: |max.x| x scale along the facing
    tv['old_across'] = round(float(ext['ey']), 3)    # |max.y| x scale across it
    tv['bone_front'], tv['bone_rear'] = (bones[0], bones[1]) if bones and bone_read else ('', '')
    tv['trials'] = tv_trials(tv)
    cb = cfg['cb']
    cb['health'] = xv['unit_health']
    cb['bonus'] = xv['aura_health_bonus']
    cb['dmg'] = round(0.3 * xv['unit_health'], 1)
    notes = []
    if offline:
        ssite, snote = dict(SPACE_FALLBACK_SITE), 'fallback (offline)'
        s1, s2, lnote = dict(LAND_FALLBACK_SITE), dict(LAND_FALLBACK_FAR), 'fallback (offline)'
    else:
        ssite, snote = space_site(Source(game))
        if space_only:
            s1, s2, lnote = dict(LAND_FALLBACK_SITE), dict(LAND_FALLBACK_FAR), 'not used (space only)'
        else:
            s1, s2, lnote = land_sites(game)
    # the user sits left of / below the space site, so the targets (up to R + E_along + 15 along +x, an across
    # target's facing point face_dist along +y from it) stay around the site
    tv['user_at'] = {'x': round(ssite['x'] - 0.55 * tv['R'], 1), 'y': round(ssite['y'] - 0.3 * tv['R'], 1),
                     'z': ssite.get('z', 0.0)}
    cb['site'] = s1
    cb['far_site'] = s2
    notes.append('space ' + snote)
    notes.append('land ' + lnote)
    cfg['site_note'] = '; '.join(notes)
    cfg['map'] = SPACE_MAP + ('' if space_only else ' + ' + LAND_MAP)
    return cfg


def freestore(src, cfg):
    shipped, fs_src = src.need(FREESTORE)
    text = shipped.decode('latin-1').replace('\r\n', '\n').rstrip('\n')
    return crlf(text + '\n\n' + lua_config(cfg) + '\n' + kit_text('s78n_probe.lua')), fs_src


def texts_dat(src, pairs):
    mt, _ = src.need(MASTERTEXT)
    tmp = os.path.join(os.environ.get('TEMP', HERE), 's78n_mt_%d.tmp' % os.getpid())
    with open(tmp, 'wb') as f:
        f.write(mt)
    try:
        keys = {k for k, _ in pairs}
        base = [kv for kv in dat_read(tmp) if kv[0] not in keys]
    finally:
        os.remove(tmp)
    return dat_bytes(base + list(pairs))


def build(game, out, source_zip=None, no_text=False, space_only=False, bone_read=True):
    """Build Mods\\S78NProbe into `out`, reading shipped files from `game` or the zip. Returns the config."""
    src = Source(game, source_zip)
    cfg = config(game, src.zip is not None, space_only, bone_read)
    prepare(out)
    files = []

    def put(rel, data):
        write(os.path.join(out, rel), data)
        files.append(rel)

    fs, fs_src = freestore(src, cfg)
    put('Data\\Scripts\\FreeStore\\BusyTacticalFreeStore.lua', fs)
    if not no_text:
        put('Data\\Text\\MasterTextFile_English.dat', texts_dat(src, TEXTS_SPACE_ONLY if space_only else TEXTS))
    cfg['files'] = files
    cfg['freestore_source'] = fs_src
    return cfg


def describe(cfg):
    """Console lines: the (A) numbers and the trial list."""
    tv = cfg['tv']
    R = tv['R']
    out = ['tractor: R %.1f, %s E along %.2f / across %.2f (sec. 5.4.5) -> thresholds %.2f / %.2f (old port %.2f / '
           '%.2f); facing bones %s' % (R, tv['target'], tv['e_along'], tv['e_across'], R + tv['e_along'],
                                       R + tv['e_across'], R + tv['old_along'], R + tv['old_across'],
                                       '%s / %s' % (tv['bone_front'], tv['bone_rear']) if tv['bone_front'] else 'OFF'),
           '  (%s)' % cfg['extent']['note']]
    for t in tv['trials']:
        out.append('  trial %2d %-12s %-6s d %.2f' % (t['k'], t['name'], t['face'], t['d']))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out')
    ap.add_argument('--source-zip')
    ap.add_argument('--no-text', action='store_true')
    ap.add_argument('--config-out')
    ap.add_argument('--space-only', action='store_true')
    ap.add_argument('--no-bone-read', action='store_true')
    a = ap.parse_args()
    out = a.out or os.path.join(a.game, 'corruption', 'Mods', MOD_NAME)
    cfg = build(a.game, out, a.source_zip, a.no_text, a.space_only, not a.no_bone_read)
    if a.config_out:
        with open(a.config_out, 'w') as f:
            json.dump(cfg, f, indent=1)
    print('built %s (%s): %s' % (out, 'space only' if a.space_only else 'space + land', ', '.join(cfg['files'])))
    print('sites:', cfg['site_note'])
    print('\n'.join(describe(cfg)))
    if not a.space_only:
        print('aura: %s on %s (health %.0f), Take_Damage %.1f, Health_Bonus %.2f' % (
            cfg['cb']['aura'], cfg['cb']['unit'], cfg['cb']['health'], cfg['cb']['dmg'], cfg['cb']['bonus']))


if __name__ == '__main__':
    main()
