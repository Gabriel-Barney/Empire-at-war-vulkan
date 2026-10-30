#!/usr/bin/env python3
"""Index a DecompileAll.java output dir: rva -> (file, name, start, end). Library + CLI:
   decomp_index.py <decomp_dir> <rva>...   prints the decompiled function containing each RVA (for local analysis only)."""
import sys, os, re, glob, bisect, pickle
HDR = re.compile(r'^// ===== (.*) @ RVA 0x([0-9a-f]+) =====$', re.M)
def build(d):
    cache = os.path.join(d, '_rva_index.pkl')
    if os.path.exists(cache): return pickle.load(open(cache, 'rb'))
    idx = {}
    for p in glob.glob(os.path.join(os.path.abspath(d), '*.c')):
        txt = open(p, encoding='latin1').read()
        ms = list(HDR.finditer(txt))
        for i, m in enumerate(ms):
            end = ms[i + 1].start() if i + 1 < len(ms) else len(txt)
            idx[int(m.group(2), 16)] = (p, m.group(1), m.start(), end)
    pickle.dump(idx, open(cache, 'wb')); return idx
def body(idx, rva):
    p, n, s, e = idx[rva]
    with open(p, encoding='latin1') as f: f.seek(0); return f.read()[s:e]
if __name__ == '__main__':
    d = sys.argv[1]; idx = build(d); ks = sorted(idx)
    for a in sys.argv[2:]:
        r = int(a, 16); i = bisect.bisect_right(ks, r) - 1
        print(body(idx, ks[i]))
