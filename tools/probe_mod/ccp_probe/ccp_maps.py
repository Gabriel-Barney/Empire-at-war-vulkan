#!/usr/bin/env python3
"""ccp_maps.py - offline answer to ccpgu t02 part 2: which shipped _MP_LAND_* maps place Reinforcement_Point objects
(the only types with Control_Point_Domination_Condition_Relevant, SKIRMISH_SETUP 4.2) and with which raw owner.

Reads the maps from the Steam install in the FoC file order (tools/probe_mod/common/ted_owners.py) and the marker
types from extracted/foc_data_text.zip (MARKERS.XML). Read-only, standard library only.

  ccp_maps.py table [--game DIR] [--json OUT] [--md OUT]   per-map table (written to findings_maps.md by default)
  ccp_maps.py points <MAP> [--game DIR]                    every relevant point of one map with position
  ccp_maps.py choose [--game DIR]                          the map the probe uses by default and why

What it reports per map: the mini 0A mode tag (UI.md 9.4: the Capture Control Points list shows the land maps whose
upper-cased 0A contains CONTROL), the relevant points (Reinforcement_Point and its six _PlusN_Cap variants) by raw
owner, the faction-fixed Reinforcement_Point_<Faction>* placements (no CAPTURE_POINT, never relevant), the start
positions (top chunk 3) and each start's distance to its nearest relevant point.

Raw owner -> start owner (SKIRMISH_SETUP 2.1, S10 probe REMAINING_GAPS 3.3): the raw id is the map-time player in
faction order Rebel 0, Empire 1, Pirates 2, Neutral 3, Hostile 4, Sarlacc 5, Underworld 6, Hutts 7; the remap gives
the object to the first battle player of that faction; with none, a non-decoration type (Reinforcement_Point has
Is_Decoration No) is removed. Raw 3 -> the Neutral player, which every skirmish has.
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
ROOT = os.path.normpath(os.path.join(PROBE_MOD, '..', '..'))
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
import ted_owners  # noqa: E402

RELEVANT = ('Reinforcement_Point', 'Reinforcement_Point_Plus1_Cap', 'Reinforcement_Point_Plus2_Cap',
            'Reinforcement_Point_Plus3_Cap', 'Reinforcement_Point_Plus4_Cap', 'Reinforcement_Point_Plus5_Cap',
            'Reinforcement_Point_Plus10_Cap')
FIXED = ('Reinforcement_Point_Rebel', 'Reinforcement_Point_Rebel_Plus4_Cap', 'Reinforcement_Point_Empire',
         'Reinforcement_Point_Empire_Plus4_Cap', 'Reinforcement_Point_Underworld',
         'Reinforcement_Point_Underworld_Plus4_Cap')
FACTION_ORDER = ted_owners.FACTION_ORDER
# Shipped mini 0A tags per SKIRMISH_SETUP 4.2 (used when the install is not readable, e.g. in the selftest).
TAGGED_SPEC = ('BESPIN', 'BOTHAWUI', 'CORELLIA', 'DAGOBAH', 'DANTOOINE', 'ENDOR', 'FELUCIA', 'GEONOSIS', 'HONOGHR', 'HOTH',
               'HYPORI', 'KAMINO', 'KESSEL', 'MANDALORE', 'NABOO', 'SALEUCAMI', 'TATOOINE', 'YAVIN')
DEFAULT_MAP = '_MP_LAND_KAMINO'


def marker_flags(zip_path=None):
    """{type name: {'relevant': bool, 'capture_point': bool, 'variant_of': str|None}} for every Reinforcement_Point*
    marker in MARKERS.XML (Variant_Of_Existing_Type inherits the flag and the behaviours)."""
    zip_path = zip_path or os.path.join(ROOT, 'extracted', 'foc_data_text.zip')
    text = None
    with zipfile.ZipFile(zip_path) as zf:
        for n in zf.namelist():
            if n.upper().replace('\\', '/').endswith('DATA/XML/MARKERS.XML'):
                text = zf.read(n).decode('latin-1')
    if text is None:
        raise SystemExit('MARKERS.XML not in ' + zip_path)
    raw = {}
    for tag, name, body in re.findall(r'<(\w+)\s+Name\s*=\s*"(Reinforcement_Point[^"]*)"\s*>(.*?)</\1\s*>', text, re.S):
        beh = ' '.join(re.findall(r'<Behavior>(.*?)</Behavior>', body, re.S)).upper()
        rel = re.search(r'<Control_Point_Domination_Condition_Relevant>\s*(\w+)\s*<', body)
        var = re.search(r'<Variant_Of_Existing_Type>\s*([^<\s]+)\s*<', body)
        raw[name] = {'relevant': (rel.group(1).lower() in ('yes', 'true', '1')) if rel else None,
                     'capture_point': ('CAPTURE_POINT' in beh) if beh else None,
                     'variant_of': var.group(1) if var else None}
    out = {}
    for name, d in raw.items():
        rel, cap, v, seen = d['relevant'], d['capture_point'], d['variant_of'], 0
        while (rel is None or cap is None) and v in raw and seen < 5:
            rel = raw[v]['relevant'] if rel is None else rel
            cap = raw[v]['capture_point'] if cap is None else cap
            v = raw[v]['variant_of']
            seen += 1
        out[name] = {'relevant': bool(rel), 'capture_point': bool(cap), 'variant_of': d['variant_of']}
    return out


def team_markers(zip_path=None):
    """{marker name: [faction-fixed types]} for the skirmish team markers `Team_NN_Reinforcement_Point*` of
    MULTIPLAYER_STRUCTURE_MARKERS.XML (Marker_For_Specific_Object_Type). At game start the skirmish setup replaces each
    by the listed type whose Affiliation is slot NN's faction, owned by slot NN's player (ccpgu t02 retail run
    20261108-214851 on Kamino; port tactical_battle_bt.cpp team_pad)."""
    zip_path = zip_path or os.path.join(ROOT, 'extracted', 'foc_data_text.zip')
    out = {}
    with zipfile.ZipFile(zip_path) as zf:
        for n in zf.namelist():
            if n.upper().replace('\\', '/').endswith('DATA/XML/MULTIPLAYER_STRUCTURE_MARKERS.XML'):
                text = zf.read(n).decode('latin-1')
                for _tag, name, body in re.findall(
                        r'<(\w+)\s+Name\s*=\s*"(Team_\d\d_Reinforcement_Point[^"]*)"\s*>(.*?)</\1\s*>', text, re.S):
                    mf = re.search(r'<Marker_For_Specific_Object_Type>(.*?)<', body, re.S)
                    out[name] = [t.strip() for t in mf.group(1).split(',') if t.strip()] if mf else []
    return out


def utf16(b):
    try:
        return b.decode('utf-16-le').rstrip('\0')
    except UnicodeDecodeError:
        return ''


def map_info(data):
    """{'tag', 'name_key', 'players', 'kind', 'starts'} from top chunks 0 (info minis) and 3 (start positions)."""
    info = {'tag': '', 'name_key': '', 'players': None, 'kind': None, 'starts': []}
    for cid, kids, p in ted_owners.chunks(data):
        if cid == 0 and not kids:
            m = ted_owners.minis(p)
            info['tag'] = utf16(m.get(0x0A, b''))
            info['name_key'] = utf16(m.get(0x08, b''))
            info['kind'] = ted_owners.u32(m.get(0x01))
            info['players'] = ted_owners.u32(m.get(0x02))
        elif cid == 3 and not kids and len(p) >= 4:
            c = struct.unpack_from('<I', p, 0)[0]
            if c <= (len(p) - 4) // 8:
                info['starts'] = [list(struct.unpack_from('<2f', p, 4 + 8 * i)) for i in range(c)]
    return info


def analyse(game, flags, team=None):
    gf = ted_owners.GameFiles(game)
    crc_of = {ted_owners.crc(n): n for n in flags}
    team = team or {}
    team_of = {ted_owners.crc(n): n for n in team}
    out = {}
    for path in gf.map_names():
        stem = path.split('\\')[-1][:-4]
        if not stem.startswith('_MP_LAND_'):
            continue
        data, src = gf.read(path)
        if data is None:
            continue
        info = map_info(data)
        pl, _terrain = ted_owners.parse_placements(data)
        rel, fixed, teams = [], [], []
        natives = sum(1 for _i, _o, _t, ow, _p in pl if ow in (4, 5, 7))
        for i, oid, tc, ow, pos in pl:
            tname = team_of.get(tc)
            if tname is not None:
                teams.append({'index': i, 'map_id': oid, 'marker': tname, 'slot': int(tname[5:7]),
                              'becomes': team[tname], 'x': round(pos[0], 1), 'y': round(pos[1], 1)})
                continue
            name = crc_of.get(tc)
            if name is None:
                continue
            row = {'index': i, 'map_id': oid, 'type': name, 'raw_owner': ow,
                   'raw_faction': FACTION_ORDER[ow] if ow is not None and 0 <= ow < len(FACTION_ORDER) else '?',
                   'x': round(pos[0], 1), 'y': round(pos[1], 1), 'z': round(pos[2], 1)}
            (rel if flags[name]['relevant'] and flags[name]['capture_point'] else fixed).append(row)
        starts = []
        for sx, sy in info['starts']:
            d = min((math.hypot(r['x'] - sx, r['y'] - sy) for r in rel), default=None)
            starts.append({'x': round(sx, 1), 'y': round(sy, 1), 'nearest_point': None if d is None else round(d)})
        out[stem] = {'source': src, 'tag': info['tag'], 'control': 'CONTROL' in info['tag'].upper(),
                     'players': info['players'], 'kind': info['kind'], 'name_key': info['name_key'],
                     'natives': natives, 'starts': starts, 'relevant': rel, 'fixed': fixed, 'team': teams}
    return out


def owners_summary(rows):
    by = {}
    for r in rows:
        k = '%s(%s)' % (r['raw_faction'], r['raw_owner'])
        by[k] = by.get(k, 0) + 1
    return ', '.join('%s x%d' % (k, v) for k, v in sorted(by.items())) or '-'


def types_summary(rows):
    by = {}
    for r in rows:
        by[r['type']] = by.get(r['type'], 0) + 1
    return ', '.join('%s x%d' % (k.replace('Reinforcement_Point', 'RP'), v) for k, v in sorted(by.items())) or '-'


def start_owner(rows):
    """Expected owner at frame 0 under the 2.1 remap, for a 2-player Empire (human) vs Rebel (AI) skirmish."""
    outs = set()
    for r in rows:
        f = r['raw_faction']
        if f == 'Neutral':
            outs.add('Neutral')
        elif f in ('Rebel', 'Empire'):
            outs.add('%s slot player' % f)
        else:
            outs.add('removed (no %s player)' % f)
    return ', '.join(sorted(outs)) or '-'


def choose(res):
    """Default probe map: a CONTROL-tagged map with the fewest relevant points, all raw Neutral; then no Hostile /
    Sarlacc / Hutts placements (raw 4 / 5 / 7: natives could contest the point); then the start positions farthest from
    their nearest point (so the AI's start force does not sit on one); ties by name."""
    cands = []
    for m, d in res.items():
        if not d['control'] or not d['relevant']:
            continue
        if any(r['raw_faction'] != 'Neutral' for r in d['relevant']):
            continue
        near = min((s['nearest_point'] for s in d['starts'] if s['nearest_point'] is not None), default=0)
        cands.append((len(d['relevant']), d['natives'] > 0, -near, m, near))
    cands.sort()
    return cands


def team_summary(d):
    """'slot N @ dist' per team marker (distance to the nearest start position)."""
    parts = []
    for r in sorted(d.get('team', []), key=lambda r: (r['slot'], r['index'])):
        dist = min((math.hypot(r['x'] - s['x'], r['y'] - s['y']) for s in d['starts']), default=None)
        parts.append('slot %d%s' % (r['slot'], '' if dist is None else ' @%d' % round(dist)))
    return ', '.join(parts) or '-'


def table_md(res, flags):
    L = ['# ccpgu t02 part 2 (offline): Reinforcement_Point placements on the shipped `_MP_LAND_*` maps', '',
         'Generated by `ccp_maps.py table` from the Steam install (FoC file order: loose > patch MEGs > maps.meg) and',
         '`extracted/foc_data_text.zip` MARKERS.XML. Relevant = CAPTURE_POINT and `Control_Point_Domination_Condition_Relevant`',
         '(own or inherited through `Variant_Of_Existing_Type`): ' +
         ', '.join('`%s`' % n for n in sorted(n for n, f in flags.items() if f['relevant'] and f['capture_point'])) + '.',
         'Faction-fixed `Reinforcement_Point_<Faction>*` (no CAPTURE_POINT, not relevant) are listed separately.', '',
         'Raw owner = placement mini 02 (faction order Rebel 0, Empire 1, Pirates 2, Neutral 3, Hostile 4, Sarlacc 5,',
         'Underworld 6, Hutts 7; REMAINING_GAPS 3.3). Start owner = SKIRMISH_SETUP 2.1 remap: raw 3 goes to the Neutral',
         'player. `CONTROL` = the map is offered under Capture Control Points (UI.md 9.4). `start->point` = distance from each',
         'start position to its nearest relevant point.', '',
         'Team markers = `Team_NN_Reinforcement_Point_Plus2_Cap` placements (MULTIPLAYER_STRUCTURE_MARKERS.XML), with the',
         'distance to the nearest start position: at game start each becomes the faction-fixed point of slot NN (below).', '',
         '| map | 0A tag | CONTROL | players | relevant points | by raw owner | start owner | types | fixed RP_<Faction> | team markers | start->point |',
         '|---|---|---|---|---|---|---|---|---|---|---|']
    n_maps = n_pts = 0
    for m in sorted(res):
        d = res[m]
        n_maps += 1 if d['relevant'] else 0
        n_pts += len(d['relevant'])
        L.append('| %s | %s | %s | %s | %d | %s | %s | %s | %s | %s | %s |' % (
            m, d['tag'] or '(empty)', 'yes' if d['control'] else 'no', d['players'], len(d['relevant']),
            owners_summary(d['relevant']), start_owner(d['relevant']), types_summary(d['relevant']),
            owners_summary(d['fixed']) if d['fixed'] else '-', team_summary(d),
            ' / '.join(str(s['nearest_point']) for s in d['starts']) or '-'))
    tagged = sorted(m for m, d in res.items() if d['control'])
    nonneutral = [(m, r) for m, d in res.items() for r in d['relevant'] if r['raw_faction'] != 'Neutral']
    L += ['', '## Summary', '',
          '- %d of %d `_MP_LAND_*` maps place relevant points (%d placements in all).' % (n_maps, len(res), n_pts),
          '- CONTROL-tagged maps (%d): %s.' % (len(tagged), ', '.join(t.replace('_MP_LAND_', '') for t in tagged)),
          '- Faction-fixed `Reinforcement_Point_<Faction>*` placed directly in the map files: %d. They do appear in game:'
          ' %d maps carry %d `Team_NN_Reinforcement_Point_Plus2_Cap` team markers, and the skirmish setup replaces each by'
          ' `Reinforcement_Point_<slot NN faction>` owned by slot NN (retail run 20261108-214851 on Kamino: Lua saw'
          ' `Reinforcement_Point_Rebel` owned by Rebel and `Reinforcement_Point_Empire` owned by Empire next to the two'
          ' starts; port: tactical_battle_bt.cpp team_pad). They have no CAPTURE_POINT and no relevant flag, so they never'
          ' count for 0x15.' % (sum(len(d['fixed']) for d in res.values()), sum(1 for d in res.values() if d.get('team')),
                               sum(len(d.get('team', [])) for d in res.values())),
          '- Relevant points with a raw owner other than Neutral (3): %s.' % (
              '; '.join('%s #%s %s raw %s (%s)' % (m, r['index'], r['type'], r['raw_owner'], r['raw_faction'])
                        for m, r in nonneutral) or 'none'),
          '- So, if the 2.1 remap holds, every relevant point on every map starts **Neutral** (the [M] of SKIRMISH_SETUP 4.2),'
          ' except the ones listed in the line above. The in-game run checks the start owner on the probe map from memory'
          ' (reader `POINT` lines at the first land frame) and from Lua (`PT` records).',
          '- Not answerable offline: whether the engine changes a point owner between the remap and frame 0, and the owner'
          ' of a non-Neutral raw point when its faction plays; the probe run covers the first for its map.']
    c = choose(res)
    if c:
        L += ['', '## Probe map', '',
              'Default `%s` (fewest relevant points among CONTROL maps whose points are all raw Neutral, then the farthest'
              ' nearest point from a start, no native placements first). Candidates (points / nearest start->point / native'
              ' placements): %s.' % (c[0][3], ', '.join('%s %d/%d/%d' % (m.replace('_MP_LAND_', ''), n, near, res[m]['natives'])
                                                    for n, _nat, _neg, m, near in c[:8]))]
    return '\n'.join(L) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=ted_owners.DEFAULT_GAME)
    ap.add_argument('--zip', default=None, help='foc_data_text.zip (default extracted/foc_data_text.zip)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('table')
    s.add_argument('--json')
    s.add_argument('--md', default=os.path.join(HERE, 'findings_maps.md'))
    s = sub.add_parser('points')
    s.add_argument('map')
    sub.add_parser('choose')
    a = ap.parse_args()
    flags = marker_flags(a.zip)
    res = analyse(a.game, flags, team_markers(a.zip))
    if not res:
        raise SystemExit('no _MP_LAND_ maps found under ' + a.game)
    if a.cmd == 'table':
        md = table_md(res, flags)
        with open(a.md, 'w', encoding='utf-8', newline='\n') as f:
            f.write(md)
        if a.json:
            with open(a.json, 'w') as f:
                json.dump(res, f, indent=1, sort_keys=True)
        print(md)
    elif a.cmd == 'points':
        m = a.map.upper()
        d = res.get(m) or res.get('_MP_LAND_' + m)
        if d is None:
            raise SystemExit('unknown map ' + a.map)
        print(json.dumps(d, indent=1))
    else:
        for n, _nat, _neg, m, near in choose(res):
            print('%-22s points=%-3d nearest start->point=%-5d natives=%d name_key=%s' % (
                m, n, near, res[m]['natives'], res[m]['name_key']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
