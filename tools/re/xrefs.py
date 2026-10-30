#!/usr/bin/env python3
"""String extraction + code xrefs (RIP-relative LEA/MOV) + function mapping via .pdata.
Usage: xrefs.py <pe> <out.json>
Output JSON: {"functions": [[start,end],...], "strings": {rva: {"s": str, "enc": "a"|"w", "xrefs": [fn_start,...]}}}
Library use: import xrefs; db = xrefs.load(pe)"""
import sys, struct, re, json, bisect
import pefile

def build(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections}
    # functions from .pdata (RUNTIME_FUNCTION); merge chained unwind (same start kept)
    funcs = sorted({(e.struct.BeginAddress, e.struct.EndAddress) for e in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', [])})
    starts = [f[0] for f in funcs]
    def fn_of(rva):
        i = bisect.bisect_right(starts, rva) - 1
        if i >= 0 and funcs[i][0] <= rva < funcs[i][1]: return funcs[i][0]
        return None
    # strings in .rdata/.data
    strings = {}
    for sec in ('.rdata', '.data'):
        if sec not in secs: continue
        lo, hi = secs[sec]
        blob = img[lo:hi]
        for m in re.finditer(rb'[\x20-\x7e\t\r\n]{4,}\0', blob):
            strings[lo + m.start()] = {'s': m.group()[:-1].decode('latin1'), 'enc': 'a', 'xrefs': []}
        for m in re.finditer(rb'(?:[\x20-\x7e]\0){4,}\0\0', blob):
            if (lo + m.start()) % 2: continue
            strings[lo + m.start()] = {'s': m.group()[:-2].decode('utf-16le'), 'enc': 'w', 'xrefs': []}
    # RIP-relative references: [REX] (8D|8B|...) modrm(mod=00,rm=101) disp32
    tlo, thi = secs['.text']
    text = img[tlo:thi]
    refs = {}
    pat = re.compile(rb'[\x48\x4c](?:\x8d|\x8b)[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
    for m in pat.finditer(text):
        off = m.start()
        disp = struct.unpack_from('<i', text, off + 3)[0]
        tgt = tlo + off + 7 + disp
        if tgt in strings or tgt in refs or True:
            refs.setdefault(tgt, []).append(tlo + off)
    for rva, s in strings.items():
        for site in refs.get(rva, []):
            f = fn_of(site)
            s['xrefs'].append(f if f is not None else -site)
    return {'functions': funcs, 'strings': strings, 'refs': refs, 'fn_of': fn_of, 'img': img, 'base': base, 'secs': secs, 'pe': pe}

def main(path, out):
    db = build(path)
    j = {'functions': db['functions'],
         'strings': {hex(k): v for k, v in db['strings'].items()}}
    json.dump(j, open(out, 'w'))
    n_x = sum(1 for v in db['strings'].values() if v['xrefs'])
    print(len(db['functions']), 'functions;', len(db['strings']), 'strings;', n_x, 'with code xrefs')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
