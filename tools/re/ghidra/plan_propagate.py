#!/usr/bin/env python3
"""Second-round naming by propagation (applied with ApplyRenames.java).
Rules for still-default (FUN_) functions, in order:
  callgraph : every caller (>=1) lives in the same class namespace C, or the function is called only by one named
              function whose namespace is C              -> C::sub_<rva>   (medium)
  callee_of : called by exactly one named function N (any namespace)  -> <N.ns>::<N.name>_helper_<k> (low)
  locality  : nearest named neighbours below and above (by address, within 0x800 bytes) share class namespace C
              (MSVC emits a translation unit contiguously)                      -> C::sub_<rva>   (low)
Usage: plan_propagate.py <functions.csv> <callgraph.csv> <out_renames.csv>"""
import sys, csv, collections, bisect

def main(fcsv, cgcsv, out):
    F = {int(r['rva'], 16): r for r in csv.DictReader(open(fcsv))}
    dflt = lambda r: r['name'].startswith(('FUN_', 'thunk_FUN_'))
    callers = collections.defaultdict(set)
    for r in csv.DictReader(open(cgcsv)):
        a, b = int(r['caller_rva'], 16), int(r['callee_rva'], 16)
        if a != b: callers[b].add(a)
    isclass = lambda ns: ns not in ('Global', '', 'std') and not ns.endswith('.DLL')
    named = sorted(a for a, r in F.items() if not dflt(r) and isclass(r['namespace']))
    plan = {}
    helper_n = collections.Counter()
    for a, r in sorted(F.items()):
        if not dflt(r): continue
        cs = [c for c in callers.get(a, []) if c in F]
        nss = {F[c]['namespace'] for c in cs}
        if cs and len(nss) == 1 and all(not dflt(F[c]) or F[c]['namespace'] != 'Global' for c in cs) and isclass(next(iter(nss))):
            plan[a] = (next(iter(nss)), f'sub_{a:x}', 'callgraph', 'medium', f'all {len(cs)} callers in this class')
            continue
        ncs = [c for c in cs if not dflt(F[c])]
        if len(cs) == 1 and ncs:
            n = F[ncs[0]]; helper_n[ncs[0]] += 1
            ns = n['namespace'] if n['namespace'] != 'Global' else ''
            plan[a] = (ns, f"{n['name'].replace('`', '').replace(chr(39), '')}_helper_{helper_n[ncs[0]]}", 'callee_of', 'low', f'only caller {n["namespace"]}::{n["name"]}')
            continue
        i = bisect.bisect_left(named, a)
        lo = named[i - 1] if i > 0 else None; hi = named[i] if i < len(named) else None
        if lo is not None and hi is not None and a - lo < 0x800 and hi - a < 0x800 and F[lo]['namespace'] == F[hi]['namespace']:
            plan[a] = (F[lo]['namespace'], f'sub_{a:x}', 'locality', 'low', 'between two methods of this class')
    with open(out, 'w', newline='') as fo:
        w = csv.writer(fo); w.writerow(['rva', 'namespace', 'name', 'source', 'confidence', 'function', 'note'])
        for a, (ns, nm, src, conf, note) in sorted(plan.items()):
            w.writerow([hex(a), ns, nm, src, conf, 'existing', note])
    print(collections.Counter(v[2] for v in plan.values()), 'default before', sum(1 for r in F.values() if dflt(r)))

if __name__ == '__main__':
    main(*sys.argv[1:4])
