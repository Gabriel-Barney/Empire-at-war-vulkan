#!/usr/bin/env python3
"""Recover the engine's Lua registration tables from StarWarsG.exe.
Needs out/code_*.pkl from scan_code.py and rtti json from rtti.py.
Usage: lua_reg.py <pe> <code.pkl> <rtti.json> <out.json>

Heuristics (validated on FoC x64 v1.121):
  * Registration callees are discovered automatically: call targets where an
    identifier-like string ("Foo_Bar") is in rdx or r8 at the call site, in functions
    that do this >= 8 times.
  * For each such call we record: name string, the most recent vtable (RTTI-mapped)
    loaded since the previous registration call (the LuaVar/wrapper object being
    registered), and the most recent .text pointer (member impl for wrapper objects).
  * Constructors called between registrations are followed one level to find the
    vtable they install.
"""
import sys, os, json, pickle, re, collections
sys.path.insert(0, os.path.dirname(__file__))
import xrefs

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]{2,63}$')

def main(pe, pkl, rttij, out):
    db = xrefs.build(pe)
    code = pickle.load(open(pkl, 'rb'))
    S = db['strings']; tlo, thi = db['secs']['.text']
    rtti = json.load(open(rttij))
    vt2cls = {}
    for c in rtti:
        for v in c['vtables']:
            vt2cls[int(v['rva'], 16)] = c['name']
    def ctor_vtable(fn, depth=0):
        if fn not in code: return None
        for e in code[fn][1][:40]:
            if e[0] == 'lea' and e[2] in vt2cls: return e[2]
        return None
    # pass 1: discover registration callees
    hits = collections.Counter(); fn_hits = collections.defaultdict(collections.Counter)
    for fn, (end, ev) in code.items():
        regs = {}
        for e in ev:
            if e[0] in ('lea', 'mov') and len(e) == 4:
                regs[e[1]] = e[2]
            elif e[0] == 'mr':
                if e[2] in regs: regs[e[1]] = regs[e[2]]
                else: regs.pop(e[1], None)
            elif e[0] == 'call':
                for r in ('rdx', 'r8'):
                    t = regs.get(r)
                    if t in S and IDENT.match(S[t]['s']):
                        hits[(e[1], r)] += 1; fn_hits[(e[1], r)][fn] += 1
                regs = {k: v for k, v in regs.items() if k in ('rbx', 'rbp', 'rsi', 'rdi', 'r12', 'r13', 'r14', 'r15')}
    callees = {k for k, n in hits.items() if n >= 40 and len(fn_hits[k]) >= 3}
    # pass 2: extract registrations
    regs_out = []
    for fn, (end, ev) in code.items():
        regs = {}; last_vt = None; last_txt = None; nreg = 0; recs = []; wrap_vt = None
        for e in ev:
            if e[0] in ('lea', 'mov') and len(e) == 4:
                regs[e[1]] = e[2]
                if e[2] in vt2cls:
                    last_vt = e[2]
                    if vt2cls[e[2]].startswith('LuaMemberFunctionWrapper'): wrap_vt = e[2]
                elif tlo <= e[2] < thi and e[0] == 'lea': last_txt = e[2]
            elif e[0] == 'mr':
                if e[2] in regs: regs[e[1]] = regs[e[2]]
                else: regs.pop(e[1], None)
            elif e[0] == 'call':
                matched = False
                for r in ('rdx', 'r8'):
                    if (e[1], r) in callees:
                        t = regs.get(r)
                        if t in S and IDENT.match(S[t]['s']):
                            recs.append({'name': S[t]['s'], 'registrar': hex(e[1]), 'arg': r, 'site': hex(e[2]),
                                         'object_class': vt2cls.get(last_vt) if last_vt else None,
                                         'impl': hex(last_txt) if last_txt else None,
                                         'wrapper': vt2cls.get(wrap_vt) if wrap_vt else None})
                            last_vt = None; last_txt = None; matched = True
                            break
                if not matched:
                    v = ctor_vtable(e[1])
                    if v: last_vt = v
                regs = {k: v for k, v in regs.items() if k in ('rbx', 'rbp', 'rsi', 'rdi', 'r12', 'r13', 'r14', 'r15')}
        if len(recs) >= 1:
            regs_out.append({'function': hex(fn), 'count': len(recs), 'registrations': recs})
    json.dump({'callees': sorted([[hex(a), r, hits[(a, r)]] for a, r in callees], key=lambda x: -x[2]),
               'tables': regs_out}, open(out, 'w'), indent=1)
    print('registrar callees:', sorted([(hex(a), r, hits[(a, r)]) for a, r in callees], key=lambda x: -x[2]))
    print(len(regs_out), 'registration functions;', sum(t['count'] for t in regs_out), 'registrations')

if __name__ == '__main__':
    main(*sys.argv[1:5])
