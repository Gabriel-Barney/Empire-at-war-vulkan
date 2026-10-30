#!/usr/bin/env python3
"""Recover static XML field-descriptor tables (Petroglyph 'parse table' pattern):
   struct FieldDesc { const char* tag; uint64 type_code; uint64 field_offset; void* extra; }  // stride 0x20
Each table maps an XML tag to the byte offset of the member it fills -> struct layout hints.
Type codes are characterized by sampling vanilla XML values for the tags that use them.
Usage: field_tables.py <exe> <code.pkl> <rtti.json> <xml_dir> <out.json> <out.md>"""
import sys, os, re, json, struct, collections, glob
sys.path.insert(0, os.path.dirname(__file__))
from name_fn import Namer
from locality import build_index, nearest

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]{1,63}$')

def xml_values(xml_dir):
    vals = collections.defaultdict(list)
    for p in glob.glob(os.path.join(xml_dir, '**', '*.[xX][mM][lL]'), recursive=True):
        try: txt = open(p, 'rb').read().decode('latin1')
        except Exception: continue
        for m in re.finditer(r'<([A-Za-z_][A-Za-z0-9_]*)>([^<]{0,80})</\1>', txt):
            if len(vals[m.group(1).lower()]) < 40: vals[m.group(1).lower()].append(m.group(2).strip())
    return vals

def xml_parents(xml_dir):
    """tag(lower) -> Counter of parent element names (the object kind that owns the field)."""
    import xml.etree.ElementTree as ET
    par = collections.defaultdict(collections.Counter)
    for p in glob.glob(os.path.join(xml_dir, '**', '*.[xX][mM][lL]'), recursive=True):
        try:
            raw = re.sub(rb'<\?xml[^>]*\?>', b'', open(p, 'rb').read())
            root = ET.fromstring(raw.decode('latin1'))
        except Exception:
            continue
        for el in root.iter():
            for ch in el:
                par[ch.tag.lower()][el.tag] += 1
    return par

GOT = {'SpaceUnit', 'UniqueUnit', 'GroundInfantry', 'GroundVehicle', 'GroundCompany', 'Squadron', 'HeroCompany', 'GroundStructure',
       'SpecialStructure', 'StarBase', 'SecondaryStructure', 'Projectile', 'Prop', 'Marker', 'TransportUnit', 'SpaceStructure',
       'GenericHeroUnit', 'Planet', 'MiscObject', 'Particle', 'UpgradeObject', 'ScriptMarker', 'MultiplayerStructureMarker',
       'GroundBuildable', 'CINProp', 'Container', 'LandBombingUnit', 'Indigenous_Unit', 'DeathClone', 'Debris', 'Building'}

def classify(vs):
    vs = [v for v in vs if v]
    if not vs: return 'no-sample'
    def allm(p): return all(re.fullmatch(p, v, re.I) for v in vs)
    if allm(r'(yes|no|true|false)'): return 'bool'
    if allm(r'-?\d+'): return 'int'
    if allm(r'-?\d*\.?\d+(e-?\d+)?f?'): return 'float'
    if allm(r'\s*-?[\d.]+\s*,\s*-?[\d.]+(\s*,\s*-?[\d.]+)+\s*'): return 'vector/list-of-numbers'
    if allm(r'[^,\s]+'): return 'name/string'
    return 'list/string'

def main(exe, pkl, rttij, xml_dir, outj, outmd):
    n = Namer(exe, pkl, rttij); lab, keys = build_index(n)
    db = n.db; img = db['img']; base = db['base']; S = n.S
    q = lambda a: struct.unpack_from('<Q', img, a)[0]
    entries = {}
    for sec in ('.data', '.rdata'):
        lo, hi = db['secs'][sec]
        for a in range(lo, hi - 0x20, 8):
            p = q(a) - base
            if p in S and IDENT.match(S[p]['s']) and q(a + 8) < 0x400 and q(a + 16) < 0x20000:
                entries[a] = (S[p]['s'], q(a + 8), q(a + 16), q(a + 24))
    # chain into tables (stride 0x20)
    tables = []; seen = set()
    for a in sorted(entries):
        if a in seen: continue
        run = [a]
        while run[-1] + 0x20 in entries: run.append(run[-1] + 0x20)
        if len(run) >= 4:
            seen.update(run); tables.append(run)
    # who references each table (code loads anywhere within table span, or up to 0x40 before)
    refs = collections.defaultdict(set)
    spans = [(t[0] - 0x40, t[-1] + 0x20, i) for i, t in enumerate(tables)]
    for f, (e, ev) in n.code.items():
        for x in ev:
            t = x[2] if (x[0] in ('lea', 'mov') and len(x) == 4) else None
            if t is None: continue
            for lo2, hi2, i in spans:
                if lo2 <= t < hi2: refs[i].add(f)
    vals = xml_values(xml_dir)
    parents = xml_parents(xml_dir)
    code_samples = collections.defaultdict(collections.Counter)
    out = []
    for i, t in enumerate(tables):
        fs = sorted(refs.get(i, []))
        owners = collections.Counter(nearest(lab, keys, f, 1)[0][1] for f in fs if nearest(lab, keys, f, 1))
        owner = owners.most_common(1)[0][0] if owners else '?'
        rows = []
        for a in t:
            tag, tc, off, extra = entries[a]
            c = classify(vals.get(tag.lower(), []))
            code_samples[tc][c] += 1
            pc = parents.get(tag.lower(), collections.Counter())
            kinds = set(pc)
            if kinds & GOT: owner_kind = 'GameObjectType'
            elif pc: owner_kind = pc.most_common(1)[0][0]
            else: owner_kind = None
            rows.append({'tag': tag, 'type_code': tc, 'offset': hex(off), 'extra': hex(extra) if extra else None, 'xml_sample_kind': c,
                         'xml_parent': owner_kind})
        out.append({'table_rva': hex(t[0]), 'entries': len(t), 'owner_guess': owner, 'referencing_functions': [hex(f) for f in fs[:6]], 'fields': rows})
    typemap = {tc: c.most_common(4) for tc, c in sorted(code_samples.items())}
    json.dump({'type_code_characterization': {hex(k): v for k, v in typemap.items()}, 'tables': out}, open(outj, 'w'), indent=1)
    with open(outmd, 'w') as fo:
        fo.write('# XML field-descriptor tables (struct layout hints)\n\n')
        fo.write('Generated by `tools/re/field_tables.py`. Each row: XML tag -> parser type code -> byte offset of the member it fills '
                 'in the owning class instance. `extra` points to a converter/enum table when present.\n\n')
        fo.write('## Type codes (characterized from vanilla XML values)\n\n| code | observed value kinds |\n|---|---|\n')
        for tc, c in typemap.items(): fo.write(f'| {tc:#x} | {", ".join(f"{k} ({v})" for k, v in c)} |\n')
        for tb in sorted(out, key=lambda x: -x['entries']):
            fo.write(f"\n## Table {tb['table_rva']} - {tb['owner_guess']} ({tb['entries']} fields)\n\n")
            # segment by dominant xml parent kind (contiguous runs)
            segs = []; cur = None
            for r in tb['fields']:
                k = r['xml_parent'] or (cur['kind'] if cur else '?')
                if cur is None or (r['xml_parent'] and r['xml_parent'] != cur['kind'] and
                                   sum(1 for x in tb['fields'][tb['fields'].index(r):tb['fields'].index(r) + 6] if x['xml_parent'] == r['xml_parent']) >= 3):
                    cur = {'kind': k, 'rows': []}; segs.append(cur)
                cur['rows'].append(r)
            for sg in segs:
                fo.write(f"\n### Segment: fields of `{sg['kind']}` objects ({len(sg['rows'])})\n\n| tag | type | offset | sample |\n|---|---|---|---|\n")
                for r in sg['rows']:
                    fo.write(f"| {r['tag']} | {r['type_code']:#x} | {r['offset']} | {r['xml_sample_kind']} |\n")
    print(len(tables), 'tables;', sum(len(t) for t in tables), 'fields')

if __name__ == '__main__':
    main(*sys.argv[1:7])
