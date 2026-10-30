#!/usr/bin/env python3
"""Find exe RVAs a patch DLL targets by signature: for every 32-bit value V in the DLL that
lies in StarWarsG.exe .text, check whether the DLL also embeds the exe's original bytes at V
(>= 6 bytes). Such pairs (RVA + expected-bytes) are the hook table entries.
Usage: patch_sigs.py <exe> <code.pkl> <out.json> <dll>..."""
import sys, os, struct, json, pickle, re, bisect
import pefile
sys.path.insert(0, os.path.dirname(__file__))
import xrefs
def main(exe, pkl, out, dlls):
    db = xrefs.build(exe); code = pickle.load(open(pkl, 'rb')); S = db['strings']
    img = db['img']; tlo, thi = db['secs']['.text']
    funcs = db['functions']; starts = [f[0] for f in funcs]
    def fn_of(r):
        i = bisect.bisect_right(starts, r) - 1
        return funcs[i][0] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None
    def hints(f):
        if f not in code: return []
        return [S[e[2]]['s'][:90] for e in code[f][1] if e[0] in ('lea', 'mov') and len(e) == 4 and e[2] in S][:5]
    res = {}
    for d in dlls:
        data = open(d, 'rb').read()
        found = {}
        for i in range(0, len(data) - 4):
            v = struct.unpack_from('<I', data, i)[0]
            if not (tlo <= v < thi) or v in found: continue
            sig = bytes(img[v:v + 8])
            if len(set(sig)) < 3: continue
            j = data.find(sig[:6])
            if j >= 0:
                n = 6
                while n < 64 and data[j:j + n + 1] == bytes(img[v:v + n + 1]): n += 1
                f = fn_of(v)
                found[v] = {'rva': hex(v), 'sig_len': n, 'sig_at': hex(j), 'ref_at': hex(i),
                            'function': hex(f) if f is not None else None,
                            'offset_in_fn': hex(v - f) if f is not None else None, 'hints': hints(f)}
        res[os.path.basename(d)] = sorted(found.values(), key=lambda x: int(x['rva'], 16))
        print(os.path.basename(d), len(found))
    json.dump(res, open(out, 'w'), indent=1)
if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
