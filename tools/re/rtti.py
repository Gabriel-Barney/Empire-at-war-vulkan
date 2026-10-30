#!/usr/bin/env python3
"""Recover MSVC x64 RTTI: TypeDescriptors, CompleteObjectLocators, vtables, base-class hierarchy.
Usage: rtti.py <pe> <out_prefix>
Writes <out_prefix>.txt (human list) and <out_prefix>.json."""
import sys, struct, re, json, subprocess
import pefile

def demangle_td(name):
    # '.?AVFoo@Bar@@' -> 'Bar::Foo' (best effort; templates left mangled-ish)
    n = name
    kind = 'struct' if n.startswith('.?AU') else 'class'
    body = n[4:]
    if body.endswith('@@'): body = body[:-2]
    if '?$' in body or '$' in body:
        return kind, try_undname(n) or body
    parts = [p for p in body.split('@') if p]
    return kind, '::'.join(reversed(parts))

_cache = {}
def try_undname(n):
    # crude template demangle: ?$Name@Args -> Name<Args>
    body = n[4:]
    try:
        m = re.match(r'\?\$([A-Za-z0-9_]+)@(.*)', body)
        if m:
            args = m.group(2).rstrip('@')
            args = re.sub(r'V([A-Za-z0-9_]+)@@', r'\1,', args)
            args = re.sub(r'U([A-Za-z0-9_]+)@@', r'\1,', args)
            return f"{m.group(1)}<{args.rstrip(',')}>"
    except Exception:
        pass
    return None

def main(path, prefix):
    pe = pefile.PE(path, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    secs = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections}
    def in_sec(rva, *names):
        return any(secs[n][0] <= rva < secs[n][1] for n in names if n in secs)
    u32 = lambda rva: struct.unpack_from('<I', img, rva)[0]
    i32 = lambda rva: struct.unpack_from('<i', img, rva)[0]
    u64 = lambda rva: struct.unpack_from('<Q', img, rva)[0]

    # 1. TypeDescriptors: name string at td+0x10
    tds = {}
    for m in re.finditer(rb'\.\?A[VU][^\0]{1,1000}?@@\0', img):
        name_rva = m.start(); td = name_rva - 0x10
        name = m.group()[:-1].decode('latin1')
        tds[td] = name
    # 2. COLs: scan rdata for signature 1 with pSelf == own rva
    cols = {}
    lo, hi = secs['.rdata']
    for rva in range(lo, hi - 24, 4):
        if u32(rva) != 1: continue
        if u32(rva + 20) != rva: continue
        tdr = u32(rva + 12)
        if tdr not in tds: continue
        cols[rva] = {'offset': u32(rva + 4), 'cd_offset': u32(rva + 8), 'td': tdr, 'chd': u32(rva + 16)}
    # 3. vtables: qword in rdata pointing to COL VA; vtable starts right after
    col_va = {base + c: c for c in cols}
    vtables = {}
    for rva in range(lo, hi - 8, 8):
        v = u64(rva)
        if v in col_va:
            vt = rva + 8
            # count slots pointing into .text
            n = 0
            while vt + 8 * n < hi:
                p = u64(vt + 8 * n) - base
                if not in_sec(p, '.text'): break
                n += 1
                if n > 2000: break
            vtables.setdefault(col_va[v], []).append((vt, n))
    # 4. class hierarchy descriptors
    def chd(rva):
        try:
            sig, attr, nb, arr = u32(rva), u32(rva + 4), u32(rva + 8), u32(rva + 12)
            bases = []
            for i in range(min(nb, 64)):
                bcd = u32(arr + 4 * i)
                tdr = u32(bcd)
                bases.append({'name': tds.get(tdr, f'?{tdr:x}'), 'nsub': u32(bcd + 4),
                              'mdisp': i32(bcd + 8), 'pdisp': i32(bcd + 12), 'vdisp': i32(bcd + 16), 'attr': u32(bcd + 20)})
            return {'attr': attr, 'bases': bases}
        except Exception:
            return None
    classes = {}
    for c, info in cols.items():
        nm = tds[info['td']]
        e = classes.setdefault(nm, {'mangled': nm, 'td_rva': hex(info['td']), 'vtables': [], 'hierarchy': None})
        for vt, n in vtables.get(c, []):
            e['vtables'].append({'rva': hex(vt), 'slots': n, 'this_offset': info['offset']})
        if e['hierarchy'] is None:
            h = chd(info['chd'])
            if h: e['hierarchy'] = h
    # TDs without COL (no vtable; e.g. exception-only / typeid-only types)
    for td, nm in tds.items():
        classes.setdefault(nm, {'mangled': nm, 'td_rva': hex(td), 'vtables': [], 'hierarchy': None})
    for nm, e in classes.items():
        k, d = demangle_td(nm); e['kind'] = k; e['name'] = d
        if e['hierarchy']:
            bl = e['hierarchy']['bases']
            e['bases'] = [demangle_td(b['name'])[1] for b in bl[1:]]
            # direct parent = first base after self whose nsub chain fits: approximation -> bl[1]
            e['parent'] = demangle_td(bl[1]['name'])[1] if len(bl) > 1 else None
    out = sorted(classes.values(), key=lambda e: e['name'].lower())
    json.dump(out, open(prefix + '.json', 'w'), indent=0)
    with open(prefix + '.txt', 'w') as f:
        f.write(f'# RTTI classes recovered from {path}\n# name | primary vtable RVA (slots) | parent | all bases\n')
        for e in out:
            e['vtables'].sort(key=lambda v: v['this_offset'])
            vt = e['vtables'][0] if e['vtables'] else None
            vts = f"{vt['rva']}({vt['slots']})" if vt else '-'
            if len(e['vtables']) > 1: vts += f" +{len(e['vtables'])-1}"
            f.write(f"{e['name']} | {vts} | {e.get('parent') or '-'} | {', '.join(e.get('bases', [])[:12])}\n")
    print(len(tds), 'type descriptors;', len(cols), 'COLs;', sum(len(v) for v in vtables.values()), 'vtables;', len(out), 'classes')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
