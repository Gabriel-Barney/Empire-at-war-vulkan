#!/usr/bin/env python3
"""ted_owners.py - list the map-placed objects of shipped .ted maps with their raw owner id (S10 owner probe).

Reads the maps straight from the Steam install (FoC loose Data\\Art\\Maps, FoC MEGs, EaW loose, EaW MEGs, in the
file-priority order of docs/PHASE2.md) and resolves type CRCs with the GameObject names in extracted/*_data_text.zip.
Read-only; standard library + tools/meg/megtool.py.

  ted_owners.py scan                      owner-id histogram of every land map (finds good probe maps)
  ted_owners.py list <MAP> [<MAP>...]     every placement of the maps: index, type, raw owner, position, kind
  ted_owners.py expected [--json out]     the expected-object list for the maps in maps.json (README table)

Placement layout (engine/terrain ted_read.cpp, docs/formats): world chunk 1 > 0x102 > 1 > 0x44C per object >
0x459 > 0x4B0 minis: 00 object id, 01 type CRC (zlib CRC-32 of the upper-case name), 02 owner, 04 position.
"""
import argparse
import json
import os
import re
import struct
import sys

sys.dont_write_bytecode = True
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'meg'))
from megtool import MegArchive  # noqa: E402

DEFAULT_GAME = r'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'
FACTION_ORDER = ['Rebel', 'Empire', 'Pirates', 'Neutral', 'Hostile', 'Sarlacc', 'Underworld', 'Hutts']


def crc(name):
    return zlib.crc32(name.upper().encode('latin-1')) & 0xFFFFFFFF


# ---- chunk reading -------------------------------------------------------------------------------------------------

def chunks(b):
    out, pos = [], 0
    while pos < len(b):
        if len(b) - pos < 8:
            raise ValueError('truncated chunk header')
        cid, sz = struct.unpack_from('<II', b, pos)
        size = sz & 0x7FFFFFFF
        if len(b) - pos - 8 < size:
            raise ValueError('chunk overruns parent')
        out.append((cid, bool(sz & 0x80000000), b[pos + 8:pos + 8 + size]))
        pos += 8 + size
    return out


def minis(b):
    out, pos = {}, 0
    while pos < len(b):
        mid, size = b[pos], b[pos + 1]
        out.setdefault(mid, b[pos + 2:pos + 2 + size])
        pos += 2 + size
    return out


def u32(m):
    return int.from_bytes(m[:4].ljust(4, b'\0'), 'little') if m else None


def parse_placements(data):
    """[(index, object_id, type_crc, owner, (x, y, z))] in map order."""
    world = next((p for cid, kids, p in chunks(data) if cid == 1 and kids), None)
    if world is None:
        raise ValueError('no world chunk')
    out = []
    terrain = False
    for cid, kids, payload in chunks(world):
        if cid == 0x101:
            terrain = True
        if cid != 0x102 or not kids:
            continue
        for lid, lkids, lp in chunks(payload):
            if lid != 1 or not lkids:
                continue
            for oid, okids, op in chunks(lp):
                if oid != 0x44C or not okids:
                    continue
                for k, kk, kp in chunks(op):
                    if k != 0x459 or not kk:
                        continue
                    for s, sk, sp in chunks(kp):
                        if s != 0x4B0 or sk:
                            continue
                        m = minis(sp)
                        pos = struct.unpack_from('<3f', m[4]) if 4 in m and len(m[4]) >= 12 else (0.0, 0.0, 0.0)
                        out.append((len(out), u32(m.get(0)), u32(m.get(1)), u32(m.get(2)), pos))
    return out, terrain


# ---- game files ----------------------------------------------------------------------------------------------------

class GameFiles:
    """Data\\ file lookup in the FoC order: FoC loose > FoC MEGs (later in megafiles.xml / patches win) > EaW loose >
    EaW MEGs."""

    def __init__(self, game):
        self.game = game
        self.loose = [os.path.join(game, 'corruption'), os.path.join(game, 'GameData')]
        self.megs = []  # (label, archive, {name: entry}) highest priority first
        for sub in ('corruption', 'GameData'):
            d = os.path.join(game, sub, 'Data')
            names = [n for n in ('64Patch.meg', 'Patch2.meg', 'Patch.meg', 'Maps.meg', 'Config.meg') if
                     os.path.exists(os.path.join(d, n))]
            for n in names:
                self.megs.append((sub + ':' + n, os.path.join(d, n), None))
        self._open = {}

    def _arch(self, i):
        label, path, _ = self.megs[i]
        if i not in self._open:
            a = MegArchive(path)
            self._open[i] = (a, {e.name.upper().replace('/', '\\'): e for e in a.entries})
        return self._open[i]

    def read(self, name):
        name = name.upper().replace('/', '\\')
        for base in self.loose[:1]:
            p = os.path.join(base, name)
            if os.path.isfile(p):
                return open(p, 'rb').read(), 'loose:' + base
        for i in range(len(self.megs)):
            if not self.megs[i][0].startswith('corruption'):
                continue
            a, idx = self._arch(i)
            e = idx.get(name)
            if e is not None:
                return a.data[e.offset:e.offset + e.size], self.megs[i][0]
        p = os.path.join(self.loose[1], name)
        if os.path.isfile(p):
            return open(p, 'rb').read(), 'loose:GameData'
        for i in range(len(self.megs)):
            if self.megs[i][0].startswith('corruption'):
                continue
            a, idx = self._arch(i)
            e = idx.get(name)
            if e is not None:
                return a.data[e.offset:e.offset + e.size], self.megs[i][0]
        return None, None

    def map_names(self):
        names = set()
        for i in range(len(self.megs)):
            a, idx = self._arch(i)
            names.update(n for n in idx if n.startswith('DATA\\ART\\MAPS\\') and n.endswith('.TED'))
        for base in self.loose:
            d = os.path.join(base, 'Data', 'Art', 'Maps')
            if os.path.isdir(d):
                names.update('DATA\\ART\\MAPS\\' + f.upper() for f in os.listdir(d) if f.upper().endswith('.TED'))
        return sorted(names)


# ---- type names ----------------------------------------------------------------------------------------------------

TYPE_RE = re.compile(r'<(\w+)\s+Name\s*=\s*"([^"]+)"\s*>(.*?)</\1\s*>', re.S)


def load_types():
    """{crc: (name, xml tag, set of behaviours, affiliation)} from the extracted XML (FoC wins over EaW)."""
    raw = {}
    for z in ('eaw_data_text.zip', 'foc_data_text.zip'):
        p = os.path.join(ROOT, 'extracted', z)
        if not os.path.exists(p):
            continue
        with zipfile.ZipFile(p) as zf:
            for n in zf.namelist():
                if not n.upper().endswith('.XML') or '\\XML\\' not in n.upper().replace('/', '\\'):
                    continue
                text = zf.read(n).decode('latin-1', 'replace')
                for tag, name, body in TYPE_RE.findall(text):
                    beh = set()
                    for t in ('Behavior', 'LandBehavior', 'SpaceBehavior'):
                        for v in re.findall(r'<%s>(.*?)</%s>' % (t, t), body, re.S):
                            beh.update(x.strip().upper() for x in v.replace('\n', ',').split(',') if x.strip())
                    var = re.search(r'<Variant_Of_Existing_Type>\s*([^<\s]+)\s*<', body)
                    aff = re.search(r'<Affiliation>(.*?)</Affiliation>', body, re.S)
                    raw[name.upper()] = (name, tag, beh, var.group(1).upper() if var else None,
                                         aff.group(1).strip() if aff else '')
    out = {}
    for key, (name, tag, beh, var, aff) in raw.items():
        seen = 0
        while var and var in raw and seen < 5:
            beh = beh | raw[var][2]
            aff = aff or raw[var][4]
            var = raw[var][3]
            seen += 1
        out[crc(key)] = (name, tag, beh, aff)
    return out


def kind_of(t):
    if t is None:
        return 'unknown'
    name, tag, beh, _ = t
    if 'REINFORCEMENT_POINT' in beh:
        return 'reinforcement_point'
    if 'CAPTURE_POINT' in beh:
        return 'capture_point'
    if tag.upper() == 'MARKER' or 'MARKER' in beh:
        return 'marker'
    if 'SPAWN_INDIGENOUS_UNITS' in beh:
        return 'spawner'
    if tag.upper() in ('GROUNDSTRUCTURE', 'GROUNDBASE', 'SPECIALSTRUCTURE', 'STARBASE', 'SECONDARYSTRUCTURE',
                       'GROUNDBUILDABLE', 'GROUNDCOMPANY'):
        return tag.lower()
    if beh & {'LOCOMOTOR', 'SELECTABLE', 'WALK_LOCOMOTOR', 'TEAM'} or tag.upper() in (
            'GROUNDINFANTRY', 'GROUNDVEHICLE', 'UNIQUEUNIT', 'HEROUNIT', 'SQUADRON', 'SPACEUNIT'):
        return 'unit:' + tag.lower()
    if tag.upper() in ('PROP', 'TRANSPORTUNIT', 'MULTIPLAYERSTRUCTUREMARKER') or 'PROP' in beh:
        return 'prop'
    return tag.lower()


def owned_kind(k):
    return k != 'prop'


# ---- commands ------------------------------------------------------------------------------------------------------

def load_map(gf, name):
    n = name.upper()
    if not n.endswith('.TED'):
        n += '.TED'
    if '\\' not in n:
        n = 'DATA\\ART\\MAPS\\' + n
    data, src = gf.read(n)
    if data is None:
        raise SystemExit('map not found: ' + n)
    pl, terrain = parse_placements(data)
    return n, src, pl, terrain


def cmd_scan(a):
    gf, types = GameFiles(a.game), load_types()
    for n in gf.map_names():
        if not re.search(a.filter, n, re.I):
            continue
        try:
            _, src, pl, terrain = load_map(gf, n)
        except (ValueError, IndexError, struct.error) as e:
            print('%-50s ERROR %s' % (n, e))
            continue
        if not terrain:
            continue
        hist, rp = {}, {}
        for _, _, tc, ow, _ in pl:
            k = kind_of(types.get(tc))
            if k == 'prop':
                continue
            hist[ow] = hist.get(ow, 0) + 1
            if k in ('reinforcement_point', 'capture_point'):
                rp[ow] = rp.get(ow, 0) + 1
        print('%-50s owners(non-prop) %s  points %s' % (n.split('\\')[-1], dict(sorted(hist.items())),
                                                      dict(sorted(rp.items()))))


def describe(types, tc):
    t = types.get(tc)
    return (t[0] if t else '?%08X' % tc), kind_of(t)


def cmd_list(a):
    gf, types = GameFiles(a.game), load_types()
    for m in a.maps:
        n, src, pl, _ = load_map(gf, m)
        print('# %s (%s): %d placements' % (n, src, len(pl)))
        for i, oid, tc, ow, pos in pl:
            name, k = describe(types, tc)
            if k == 'prop' and not a.all:
                continue
            print('%4d id=%-6s owner=%-3s %-14s %-44s (%9.1f %9.1f %7.1f)' % (i, oid, ow, k, name, *pos))


def expected(gf, types, maps):
    out = {}
    for m in maps:
        n, src, pl, _ = load_map(gf, m)
        rows = []
        for i, oid, tc, ow, pos in pl:
            name, k = describe(types, tc)
            if k == 'prop':
                continue
            rows.append({'index': i, 'map_id': oid, 'type': name, 'type_crc': '%08X' % tc, 'owner': ow,
                         'kind': k, 'pos': [round(v, 2) for v in pos]})
        out[n.split('\\')[-1][:-4]] = {'source': src, 'objects': rows}
    return out


def cmd_expected(a):
    gf, types = GameFiles(a.game), load_types()
    cfg = json.load(open(os.path.join(HERE, 'maps.json')))
    maps = sorted({s['map'] for s in cfg['scenarios']})
    exp = expected(gf, types, maps)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(exp, f, indent=1)
    for m, d in exp.items():
        print('## %s (%s)' % (m, d['source']))
        by = {}
        for r in d['objects']:
            by.setdefault((r['owner'], r['kind']), []).append(r['type'])
        for (ow, k), names in sorted(by.items(), key=lambda x: (x[0][0], x[0][1])):
            uniq = {}
            for x in names:
                uniq[x] = uniq.get(x, 0) + 1
            print('  owner %-3s %-20s %3d  %s' % (ow, k, len(names),
                                                  ', '.join('%s x%d' % (x, c) if c > 1 else x for x, c in
                                                            sorted(uniq.items()))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('--filter', default=r'LAND')
    s = sub.add_parser('list')
    s.add_argument('maps', nargs='+')
    s.add_argument('--all', action='store_true', help='include props')
    s = sub.add_parser('expected')
    s.add_argument('--json')
    a = ap.parse_args()
    {'scan': cmd_scan, 'list': cmd_list, 'expected': cmd_expected}[a.cmd](a)


if __name__ == '__main__':
    main()


# ---- shared by the mod builders (cu18 t09, 2026-11-07: was copied into build_a1 / fow / oi / s7s8_mod.py)
def read_shipped(game, name, source_zip=None):
    """(bytes, label) of a shipped Data file: the extracted text zip when given, else the install in FoC order.
    (None, None) when it is not found."""
    if source_zip:
        with zipfile.ZipFile(source_zip) as z:
            want = name.upper().replace('/', '\\')
            for n in z.namelist():
                if n.upper().replace('/', '\\') == want:
                    return z.read(n), source_zip + ':' + n
        return None, None
    if not os.path.isdir(game):
        return None, None
    return GameFiles(game).read(name)


def append_to_shipped(game, name, tail, source_zip=None):
    """(bytes, label): the shipped script `name` (latin-1, CRLF) with `tail` appended after one blank line, as the
    probe kits hook BusyTacticalFreeStore.lua. Exits when the shipped file is not found."""
    data, src = read_shipped(game, name, source_zip)
    if data is None:
        raise SystemExit('shipped %s not found (game %s, zip %s)' % (name.replace('/', '\\').split('\\')[-1], game,
                                                                     source_zip))
    text = data.decode('latin-1').replace('\r\n', '\n')
    out = text.rstrip('\n') + '\n\n' + tail
    return out.replace('\n', '\r\n').encode('latin-1'), src
