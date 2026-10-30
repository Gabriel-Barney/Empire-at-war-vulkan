#!/usr/bin/env python3
"""Inventory third-party eaw_* patch DLLs: exports, imports, interesting strings, and every
StarWarsG.exe RVA they reference (immediate constants / strings like 0x385ae0 that fall in
the exe's .text), mapped to the containing exe function and that function's string hints.
Usage: patch_inventory.py <exe> <code.pkl> <out.json> <dll>...
Treats DLLs as data only (never loads/executes them)."""
import sys, os, re, json, struct, pickle, bisect
import pefile, capstone
from capstone import x86
sys.path.insert(0, os.path.dirname(__file__))
import xrefs

def exe_fn_hints(db, code, fn):
    S = db['strings']
    if fn not in code: return []
    return [S[e[2]]['s'][:80] for e in code[fn][1] if e[0] in ('lea', 'mov') and len(e) == 4 and e[2] in S][:6]

def main(exe, pkl, out, dlls):
    db = xrefs.build(exe); code = pickle.load(open(pkl, 'rb'))
    tlo, thi = db['secs']['.text']
    funcs = db['functions']; starts = [f[0] for f in funcs]
    def fn_of(r):
        i = bisect.bisect_right(starts, r) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None
    report = {}
    for d in dlls:
        pe = pefile.PE(d)
        img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
        r = {'size': os.path.getsize(d)}
        r['exports'] = [x.name.decode() if x.name else f'#{x.ordinal}' for x in getattr(getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None), 'symbols', [])] if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else []
        r['imports'] = {e.dll.decode(): [i.name.decode() if i.name else f'#{i.ordinal}' for i in e.imports] for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', [])}
        data = open(d, 'rb').read()
        strs = sorted(set(m.decode('latin1') for m in re.findall(rb'[\x20-\x7e]{5,}', data)))
        r['strings_interesting'] = [s for s in strs if re.search(r'(?i)hook|patch|rva|0x[0-9a-f]{5,}|caller|scratch|pool|ai_|perception|path|visib|render|mesh|material|modifier|geometry|simulation|target|object_state|memory|budget|tick|StarWarsG|\.ini|sig|pattern|detour|minhook|thread|queue|worker|tbb', s)][:400]
        # candidate exe RVAs: hex strings in data + 32-bit immediates in code within exe .text
        cands = set(int(m, 16) for m in re.findall(r'0x([0-9a-fA-F]{5,7})\b', ' '.join(strs)))
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
        for s in pe.sections:
            if not (s.Characteristics & 0x20000000): continue
            blob = bytes(img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize])
            for ins in md.disasm(blob, base + s.VirtualAddress):
                for op in ins.operands:
                    v = None
                    if op.type == x86.X86_OP_IMM: v = op.imm & 0xffffffffffffffff
                    elif op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0: v = op.mem.disp
                    if v is not None and 0x1000 <= v < 0xd00000: cands.add(v)
        # also raw dwords in .rdata/.data (tables of RVAs)
        for s in pe.sections:
            if s.Characteristics & 0x20000000: continue
            blob = bytes(img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize])
            for i in range(0, len(blob) - 4, 4):
                v = struct.unpack_from('<I', blob, i)[0]
                if 0x1000 <= v < 0xd00000: cands.add(v)
        hits = []
        for v in sorted(cands):
            if not (tlo <= v < thi): continue
            f = fn_of(v)
            if f == v:  # exact function starts are the strong signal
                hits.append({'rva': hex(v), 'kind': 'function_start', 'hints': exe_fn_hints(db, code, f)})
            elif f is not None and v - f < 0x4000 and any(c in ' '.join(strs) for c in (hex(v), f'{v:x}')):
                hits.append({'rva': hex(v), 'kind': f'inside {hex(f)}+{v-f:#x}', 'hints': exe_fn_hints(db, code, f)})
        r['exe_rva_refs'] = hits
        report[os.path.basename(d)] = r
        print(os.path.basename(d), len(r['exports']), 'exports;', len(hits), 'exe function refs')
    json.dump(report, open(out, 'w'), indent=1)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
