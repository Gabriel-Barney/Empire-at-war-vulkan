#!/usr/bin/env python3
"""Infer member layouts from this-relative accesses in Ghidra decompiler output.

For each target struct we collect accesses `*(T *)(base + off)`, `base[idx]` (scaled by the declared
element type) and nested `*(T *)(*(ptr *)(base + a) + b)` where `base` is
  - param_1 of every function in the struct's own class namespace (thiscall), or
  - param_1 of explicitly listed functions (hot spots that take the object as first arg), or
  - a pointer member reached through a declared link (src_struct, offset) -> dst_struct
    (e.g. GameObjectWrapper+0x60 -> GameObjectClass), including simple local aliases.
XML field-descriptor tables (field_tables.json) contribute names for XML-backed classes.
Usage: struct_infer.py <decomp_dir> <re_work_dir> <out.json> <out.md>
Docs contain offsets/types/names only - no decompiled code."""
import sys, os, re, json, glob, collections

SIZES = {'longlong': 8, 'ulonglong': 8, 'undefined8': 8, 'double': 8, 'code': 8, 'int': 4, 'uint': 4, 'float': 4, 'undefined4': 4,
         'short': 2, 'ushort': 2, 'undefined2': 2, 'wchar_t': 2, 'char': 1, 'byte': 1, 'bool': 1, 'undefined1': 1, 'undefined': 1}
def tsize(t):
    t = t.strip()
    if t.endswith('*'): return 8
    return SIZES.get(t.split()[-1], None)


STRUCTS = {
 'GameObjectClass': dict(classes=['GameObjectClass'], funcs=[0x2abcf0, 0x2acb90]),
 'GameObjectTypeClass': dict(classes=['GameObjectTypeClass'], funcs=[0x3727a0]),
 'TreeCullNode': dict(classes=[], funcs=[0x20fef0, 0x2100a0, 0x2102d0]),
 'TreeCullQuery': dict(classes=['TreeCullClass'], funcs=[0x20e780, 0x20e9f0, 0x20eac0]),
 'AIGoalSystemClass': dict(classes=['AIGoalSystemClass', 'GalacticGoalSystemClass', 'LandGoalSystemClass', 'SpaceGoalSystemClass'], funcs=[0x6cb120, 0x6c8710, 0x6c8970]),
 'InstantiatedGoalClass': dict(classes=['InstantiatedGoalClass'], funcs=[]),
 'AIPerceptionSystemClass': dict(classes=['AIPerceptionSystemClass', 'GalacticPerceptionSystemClass', 'LandPerceptionSystemClass', 'SpacePerceptionSystemClass'], funcs=[]),
 'PerceptualEvaluatorClass': dict(classes=['PerceptualEvaluatorClass', 'GameObjectPerceptualEvaluatorClass', 'PlanetPerceptualEvaluatorClass', 'PlayerPerceptualEvaluatorClass'], funcs=[]),
 'PlayerClass': dict(classes=['PlayerClass'], funcs=[]),
 'TradeRouteClass': dict(classes=['TradeRouteClass'], funcs=[0x5c3840]),
 'PlanetaryDataPackClass': dict(classes=['PlanetaryDataPackClass'], funcs=[]),
 'PerceptionContextClass': dict(classes=[], funcs=[0x3ef160, 0x7b260, 0x5b41c0, 0x5b42c0]),
 'AIBudgetClass': dict(classes=['AIBudgetClass'], funcs=[0x614590]),
 'PlanetaryBehaviorClass': dict(classes=['PlanetaryBehaviorClass'], funcs=[]),
 'GameObjectWrapper': dict(classes=['GameObjectWrapper'], funcs=[]),
 'TaskForceClass': dict(classes=['TaskForceClass'], funcs=[]),
 'CullLink': dict(classes=[], funcs=[]),
}

LINKS = {('GameObjectWrapper', 0x60): 'GameObjectClass', ('PlayerWrapper', 0x28): 'PlayerClass',
         ('GameObjectTypeWrapper', 0x28): 'GameObjectTypeClass', ('GameObjectClass', 0x298): 'GameObjectTypeClass', ('GameObjectClass', 0xb8): 'PlanetaryDataPackClass',
         ('TreeCullQuery', 0x10): 'TreeCullNode', ('PlayerWrapper', 0x28): 'PlayerClass', ('TreeCullNode', 0x30): 'TreeCullNode', ('TreeCullNode', 0x38): 'TreeCullNode'}

LINK_SRC_CLASSES = {'GameObjectWrapper': ['GameObjectWrapper'], 'PlayerWrapper': ['PlayerWrapper'], 'GameObjectTypeWrapper': ['GameObjectTypeWrapper']}

XML_KIND = {'GameObjectTypeClass': 'GameObjectType', 'PlayerClass': None, 'FactionClass': 'Faction'}

HDR = re.compile(r'^// ===== (.*) @ RVA 0x([0-9a-f]+) =====$', re.M)

NUM = r'(0x[0-9a-f]+|\d+)'

def parse_funcs(d):
    for p in glob.glob(os.path.join(d, '*.c')):
        txt = open(p, encoding='latin1').read()
        ms = list(HDR.finditer(txt))
        for i, m in enumerate(ms):
            yield m.group(1), int(m.group(2), 16), txt[m.end():ms[i + 1].start() if i + 1 < len(ms) else len(txt)]

def decls(body):
    """var -> declared type (params + locals)"""
    out = {}
    sig = re.search(r'\(([^)]*)\)\s*\n\s*\{', body)
    if sig:
        for p in sig.group(1).split(','):
            m = re.match(r'\s*(.*?)(\w+)\s*$', p)
            if m: out[m.group(2)] = m.group(1).strip()
    bi = body.find('{')
    be = body.find('\n  \n', bi)
    block = body[bi:be if be > 0 else bi]
    for m in re.finditer(r'^\s{2}([\w<>:, ]+?\s*\**)\s*(\w+)(?:\s*\[[^\]]*\])?;\s*$', block, re.M):
        if m.group(1).strip() in ('return', 'goto'): continue
        out[m.group(2)] = m.group(1).strip()
    return out

def scan(body, base_var, decl, rec, struct, fname, depth=0):
    bt = decl.get(base_var, 'longlong')
    elem = tsize(bt[:-1]) if bt.endswith('*') else None
    v = re.escape(base_var)
    # *(T *)(base + off) and *(T *)((longlong)base + off)
    for m in re.finditer(r'\*\(([\w ]+?\*?) \*\)\((\(longlong\))?' + v + r' \+ ' + NUM + r'\)(\s*=(?!=))?', body):
        t, cast, off, w = m.group(1), m.group(2), int(m.group(3), 0), m.group(4)
        if not cast and elem: off *= elem
        rec[struct][off].append((t, bool(w), fname))
    # *(T *)base  (offset 0)
    for m in re.finditer(r'\*\(([\w ]+?\*?) \*\)' + v + r'\b(\s*=(?!=))?', body):
        rec[struct][0].append((m.group(1), bool(m.group(2)), fname))
    # base[idx]
    if elem:
        for m in re.finditer(r'\b' + v + r'\[' + NUM + r'\](\s*=(?!=))?', body):
            rec[struct][int(m.group(1), 0) * elem].append((bt[:-1].strip(), bool(m.group(2)), fname))
    # links: alias = *(ptr *)(base + off)  /  alias = base[idx]   and nested derefs
    if depth < 2:
        for (src, loff), dst in LINKS.items():
            if src != struct: continue
            pats = [r'(\w+) = \*\((?:[\w ]+ \*+)\)\((?:\(longlong\))?' + v + r' \+ ' + hex(loff if not elem else loff) + r'\);']
            if elem and loff % elem == 0: pats.append(r'(\w+) = \(?[\w *]*\)?' + v + r'\[' + hex(loff // elem) + r'\];')
            for pat in pats:
                for m in re.finditer(pat, body):
                    scan(body, m.group(1), decl, rec, dst, fname, depth + 1)
            for m in re.finditer(r'\*\(([\w ]+?\*?) \*\)\(\*\((?:longlong|undefined8|[\w ]+ \*)\s*\*\)\((?:\(longlong\))?' + v + r' \+ ' + hex(loff) + r'\) \+ ' + NUM + r'\)(\s*=(?!=))?', body):
                rec[dst][int(m.group(2), 0)].append((m.group(1), bool(m.group(3)), fname))

def main(ddir, rew, outj, outmd):
    rec = collections.defaultdict(lambda: collections.defaultdict(list))
    nfun = collections.Counter()
    cls2struct = collections.defaultdict(list)
    for s, cfg in STRUCTS.items():
        for c in cfg['classes']: cls2struct[c].append(s)
    for src, cl in LINK_SRC_CLASSES.items():
        for c in cl: cls2struct[c].append(src)
    func2struct = {r: s for s, cfg in STRUCTS.items() for r in cfg['funcs']}
    # pass 0: arg propagation - non-virtual callees receiving a linked pointer as their first argument
    allf = list(parse_funcs(ddir))
    byname = {n.split('::')[-1]: r for n, r, b in allf if n.startswith('FUN_')}
    prop = collections.Counter()
    for name, rva, body in allf:
        ns = name.rsplit('::', 1)[0] if '::' in name else ''
        for (src, loff), dst in LINKS.items():
            if ns not in LINK_SRC_CLASSES.get(src, [src]): continue
            for m in re.finditer(r'(FUN_14[0-9a-f]{7})\(\*\((?:undefined8|longlong) \*\)\(param_1 \+ ' + hex(loff) + r'\)[,)]', body):
                r = byname.get(m.group(1))
                if r is not None and r not in func2struct: prop[(r, dst)] += 1
    for (r, dst), n in prop.items():
        func2struct.setdefault(r, dst)
    print('arg-propagated functions:', len(prop))
    for name, rva, body in allf:
        ns = name.rsplit('::', 1)[0] if '::' in name else ''
        if '::sub_' in name or '_helper_' in name: ns = ''   # propagated (low-confidence) names: param_1 is not proven to be this
        targets = list(dict.fromkeys(cls2struct.get(ns, []) + ([func2struct[rva]] if rva in func2struct else [])))
        if not targets: continue
        d = decls(body)
        if 'param_1' not in d: continue
        for s in targets:
            nfun[s] += 1
            scan(body, 'param_1', d, rec, s, name)
    # XML names
    ft = json.load(open(f'{rew}/out/field_tables.json'))
    xml = collections.defaultdict(dict)
    for tb in ft['tables']:
        for f in tb['fields']:
            if f['xml_parent'] == 'GameObjectType':
                xml['GameObjectTypeClass'].setdefault(int(f['offset'], 16), []).append((f['tag'], f['type_code']))
            if f['xml_parent'] == 'TradeRoute':
                xml['TradeRouteClass'].setdefault(int(f['offset'], 16), []).append((f['tag'], f['type_code']))
            if f['xml_parent'] == 'Faction':
                xml['FactionClass'].setdefault(int(f['offset'], 16), []).append((f['tag'], f['type_code']))
    TC = {0: ('bool', 1), 8: ('float', 4), 5: ('int', 4), 6: ('int', 4), 0x17: ('std::string', 0x20), 0x1f: ('type/name ref', 8),
          0x1d: ('enum/name', 4), 0x10: ('vector3', 12), 0x16: ('float list', 0x18), 0xe: ('name', 0x20)}
    ser = {}
    sp = f'{rew}/out/ghidra/serializers.json'
    if os.path.exists(sp):
        for k, v in json.load(open(sp)).items():
            for f in v['fields']:
                ser.setdefault(k, {}).setdefault(f['offset'], (f['size'], f'{f["chunk"]}/{f["micro"]}', f['kind']))
            for f in v['subobjects']:
                ser.setdefault(k, {}).setdefault(f['offset'], (None, f'{f["chunk"]}/{f["micro"]}', 'subobject via ' + f['saver']))
    out = {}
    for s in list(STRUCTS) + [x for x in rec if x not in STRUCTS]:
        offs = rec.get(s, {})
        rows = []
        allo = sorted(set(offs) | set(xml.get(s, {})) | set(ser.get(s, {})))
        for o in allo:
            acc = offs.get(o, [])
            types = collections.Counter(t for t, w, f in acc)
            fns = {f for t, w, f in acc}
            writes = sum(1 for t, w, f in acc if w)
            xn = xml.get(s, {}).get(o, [])
            width = None
            if types:
                t0 = types.most_common(1)[0][0]; width = tsize(t0)
            if xn and width is None:
                width = TC.get(xn[0][1], ('?', None))[1]
            sv = ser.get(s, {}).get(o)
            if sv and sv[0]: width = sv[0]
            conf = 'high' if (len(fns) >= 3 or (xn and fns) or sv) else 'medium' if (len(fns) == 2 or xn) else 'low'
            rows.append({'offset': o, 'types': dict(types.most_common(3)), 'width': width, 'functions': len(fns), 'reads': len(acc) - writes,
                         'writes': writes, 'xml_tags': [x[0] for x in xn][:3], 'xml_type': TC.get(xn[0][1], (hex(xn[0][1]),))[0] if xn else None,
                         'confidence': conf, 'example_functions': sorted(fns)[:3],
                         'saved': (f'chunk/micro {sv[1]}' + ('' if sv[2] == 'raw' else f' ({sv[2]})')) if sv else None})
        mx = max((r['offset'] + (r['width'] or 1) for r in rows), default=0)
        out[s] = {'functions_scanned': nfun.get(s, 0), 'min_size': mx, 'fields': rows}
    json.dump(out, open(outj, 'w'), indent=1)
    write_md(out, outmd)
    for s, v in out.items(): print(f"{s:28s} fns={v['functions_scanned']:4d} fields={len(v['fields']):4d} min_size={v['min_size']:#x}")

NOTES = {
 'GameObjectClass': 'Reached via own methods, GameObjectWrapper+0x60 and the GameObjectManager segment queries. Cross-validated: +0x298 is the GameObjectTypeClass* (the type fields read through it at +0x41/+0x42/+0x4e4 are exactly the XML tags Collidable_By_Projectile_Living / _Dead / Collision_Box_Modifier). +0x28 is the embedded CullObjectClass sub-object (cull-tree entries point at object+0x28; object = entry-0x28), +0x78/+0x7c/+0x80 = world position (Get_Position copies these), +0x3a0 bit 0x40 = dead/death-clone state (selects the *_Dead collidability flag), +0x337 = transport-related byte (Is_Transport: != 0xFF). +0x58 = owner player id (looked up in the global player list by HardPoint/damage code), +0xf0 = behavior/data-pack pointer whose +0x138 holds per-object modifier sources. GameObjectClass is pooled (PooledObjectClass<GameObjectClass,64>), with bases RootClass, MultiLinkedListMember, CullObjectClass, SignalGeneratorClass.',
 'GameObjectTypeClass': 'Offsets of ~1000 fields are known exactly from the XML field-descriptor table (tag -> offset); listed here merged with code accesses. Max XML offset ~0x2948, so sizeof >= ~0x2950.\n\nCode-accessed fields (59) are shown; the full XML-backed list (786 offsets) is in `xml_field_tables.md`.',
 'TreeCullNode': 'Binary bounding-volume tree node. +0x00 center (3 floats) +0x0c half-extents (3 floats) [AABB as center/extent, tested with |dc| vs ea+eb]; +0x18/+0x20/+0x28 = std::vector<CullObject*> begin/end/capacity of objects stored at this node; +0x30/+0x38 = child pointers (either may be null). Objects keep their own AABB at entry+0x2c (center+extent) and an intrusive `next` at entry+0x18 used to chain query results.',
 'TreeCullQuery': 'This is TreeCullClass itself (0x38 bytes; GameObjectManager keeps an array of them at GOM+0xd0, one per player id). +0x00 vftable (vf1 Insert, vf2 Remove, vf3 Get_Flag30, vf4 Update_Bounds), +0x08 head of the intrusive result list, +0x10 root node, +0x18 link count, +0x1c max depth reached by last rebuild, +0x20 dirty flag, +0x24 moved-out-of-node counter since last rebuild, +0x28 large-object threshold (default 500.0: objects whose largest half-extent exceeds it stay at the root), +0x2c time of last rebuild, +0x30 byte flag. All trees register themselves in a global vector (used by the per-frame rebuild pass).',
 'AIGoalSystemClass': 'Derived from ServicedAISystemClass. +0x28 owning AIPlayer/context, +0x38 and +0xd8 = two goal containers (current set and proposed set; hash map category->vector<InstantiatedGoal*>), +0x178 goal-type cursor, +0x17c target cursor, +0x180 per-pass target index, +0x184 "pass complete" flag, +0x188 evaluation count this pass, +0x18c service countdown (frames to skip), +0x190 embedded AIBudgetClass, +0x228 list of goal-type names (saved). Offsets as 8-byte slots: param_1[0x2f]=evaluations per service, [0x30]=target index, [0x31]=evaluations done.',
 'PlayerClass': 'Sources: PlayerClass::Save (0x286760, chunk 0 micro-chunks 1..0x42 - every persistent member with exact width), PlayerWrapper+0x28 accesses, and 53 non-virtual functions receiving the player pointer from wrapper methods (argument propagation). sizeof >= 0x488. +0x4c = player id (used as index into per-player arrays such as GOM+0xd0 + id*0x38), +0x360 = faction/AI-type pointer (difficulty multipliers at +0x24/+0x50 of the object returned from it), +0x62 = byte flag tested before applying the defender difficulty multiplier, +0x70 = credits (float, Get_Credits).',
 'TradeRouteClass': 'From the TradeRoute XML field table: +0x28 Point_A (planet name/ref, 0x10), +0x38 Point_B, +0x48 Political_Control_Gain (int), +0x4c Credit_Gain_Factor (float), +0x50 HS_Speed_Factor (float; divides hop cost in both path finders), +0x58 Visible_Line_Name (string). See GALACTIC.md.',
 'PlanetaryDataPackClass': 'Planet GameObjects keep this at GameObject+0xb8. Trade-route links: +0x58 array of 0x20-byte TradeRouteLinkEntryClass {+0x08 TradeRouteClass*, +0x18 neighbour planet GameObject*}, +0x60 count. +0x2c9 byte = revealed (fog), +0x2e0 byte = hidden/inaccessible, +0x2f4 int = owner (-1 none), +0x354 = id token used by the AI danger perception (GALACTIC.md). Serialized fields (save ids) below come from its Save (vftable slot 0). Original note: Per-planet runtime data pack attached to planet GameObjects (galactic node). Only 2 attributed methods; the static planet XML fields (Planet segment of the GameObjectType table) live in GameObjectTypeClass. Galactic connectivity (trade routes) is held by TradeRouteClass/GalacticModeClass and is not covered yet.',
 'PerceptionContextClass': 'Stack object (0x58 bytes) built per evaluation: +0x00 byte, +0x04 float max_load_factor (1.0), +0x08 list head (std::list sentinel, 0x20-byte nodes), +0x18..+0x28 bucket vector (initial 8 buckets = 0x80 bytes), +0x30 bucket mask (7), +0x38 bucket count (8), +0x40 owner/player context, +0x48/+0x50 target object pointers. I.e. an unordered_map<token,int/ptr> plus two context pointers.',
}

MANUAL = [
 '## GalacticPathRequest (manual, A6)', '',
 'Stack struct passed to GalacticPathFinderClass::Find_Path; initialised by Path_Request_Init (0x3eefd0, all flags = 1). '
 'Find_Path copies it into the finder (player +0x108, src +0x110, dst +0x118, flags +0x120..+0x125). Flag meanings: GALACTIC.md section 3.', '',
 '| offset | width | field | conf |', '|---|---|---|---|',
 '| 0x00 | 8 | PlayerClass* requester | high |', '| 0x08 | 8 | source planet GameObject* | high |', '| 0x10 | 8 | destination planet GameObject* | high |',
 '| 0x18 | 1 | allow enemy-occupied, not-owned planets | medium |', '| 0x19 | 1 | allow enemy-occupied, non-enemy-owned planets | medium |',
 '| 0x1a | 1 | skip danger check on passable planets | medium |', '| 0x1b | 1 | allow impassable planets if not dangerous | medium |',
 '| 0x1c | 1 | always accept destination | high |', '| 0x1d | 1 | require revealed endpoints (fog) | high |', '',
 '## GameConstantsClass (manual, A6)', '',
 'Singleton instance at VA 0x140b15800 (468 code references match offsets of the GameConstants XML field table, so field offsets are '
 'those in xml_field_tables.md). Example: +0x108 float MaximumFleetMovementDistance (galactic free-hop radius; vanilla 100.0).', '']

def write_md(out, path):
    L = ['# Core struct layouts (inferred)', '',
         'Generated by `tools/re/ghidra/struct_infer.py` from the Ghidra 12.2 decompilation (after class attribution), plus the XML '
         'field-descriptor tables. Each field: byte offset, observed access widths/types, number of distinct functions touching it, reads/writes, '
         'XML tag (if any), confidence (high = >=3 functions or XML-proven and accessed; medium = 2 functions or XML-only; low = 1 function). '
         'Gaps between offsets are unknown, not padding. `min_size` = end of highest observed field (the true sizeof can be larger; pooled '
         'allocation sizes are noted where known). "save id" = chunk/micro-chunk id in the class Save() serializer '
         '(`tools/re/ghidra/serializer_fields.py`); serialized fields are high confidence with exact width. No decompiled code is reproduced.', '']
    for s, v in out.items():
        L += [f'## {s}', '', f"Functions scanned: {v['functions_scanned']}; fields observed: {len(v['fields'])}; min_size: {v['min_size']:#x}.", '']
        if s in NOTES: L += [NOTES[s], '']
        rows = v['fields']
        if s in ('GameObjectTypeClass', 'FactionClass'):
            code_rows = [r for r in rows if r['functions']]
            L.append(f"Code-accessed fields ({len(code_rows)}) are shown; the full XML-backed list ({sum(1 for r in rows if r['xml_tags'])} offsets) is in "
                     '`xml_field_tables.md`.\n')
            rows = code_rows
        L += ['| offset | width | types | fns | r/w | XML tag / save id | conf |', '|---|---|---|---|---|---|---|']
        for r in rows[:400]:
            ty = ', '.join(f'{k}x{n}' for k, n in r['types'].items())
            L.append(f"| {r['offset']:#x} | {r['width'] or '?'} | {ty} | {r['functions']} | {r['reads']}/{r['writes']} | {' / '.join(r['xml_tags'] + ([r['saved']] if r.get('saved') else []))} | {r['confidence']} |")
        L.append('')
    L += MANUAL
    open(path, 'w').write('\n'.join(L) + '\n')

if __name__ == '__main__':
    main(*sys.argv[1:5])

