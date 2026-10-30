#!/usr/bin/env python3
"""Address-locality class attribution: MSVC emits a translation unit's functions contiguously,
so the nearest functions carrying 'Class::Method' strings or RTTI vtable slots name the TU.
Usage: locality.py <exe> <code.pkl> <rtti.json> <rva>...  ; import: build_index(), nearest()"""
import sys, os, re, bisect, json
sys.path.insert(0, os.path.dirname(__file__))
from name_fn import Namer
PAT = re.compile(r'\b([A-Z][A-Za-z0-9_]+)::([A-Za-z_][A-Za-z0-9_]*)')
def build_index(n):
    lab = {}
    for f in n.code:
        tags = []
        for s in n.strs(f, 40):
            m = PAT.search(s)
            if m: tags.append(m.group(1))
        for v in n.slot.get(f, []):
            tags.append(v.split('[')[0])
        if tags: lab[f] = max(set(tags), key=tags.count)
    keys = sorted(lab)
    return lab, keys
def nearest(lab, keys, f, k=3):
    i = bisect.bisect_left(keys, f)
    cand = keys[max(0, i - k):i + k]
    return [(hex(c), lab[c], hex(abs(c - f))) for c in sorted(cand, key=lambda c: abs(c - f))[:k]]
if __name__ == '__main__':
    n = Namer(*sys.argv[1:4]); lab, keys = build_index(n)
    for a in sys.argv[4:]: print(a, nearest(lab, keys, int(a, 16)))
