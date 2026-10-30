#!/usr/bin/env python3
"""Context for naming an exe function: vtable slots (RTTI class), own string refs, caller and
callee string refs, and Class::Method names harvested from nearby error strings.
Usage: name_fn.py <exe> <code.pkl> <rtti.json> <rva_hex>...   (or import and call describe())"""
import sys, os, json, pickle, re, struct, collections
sys.path.insert(0, os.path.dirname(__file__))
import xrefs

class Namer:
    def __init__(self, exe, pkl, rttij):
        self.db = xrefs.build(exe); self.code = pickle.load(open(pkl, 'rb'))
        self.S = self.db['strings']; img = self.db['img']; base = self.db['base']
        self.slot = collections.defaultdict(list)
        for c in json.load(open(rttij)):
            for v in c['vtables']:
                vt = int(v['rva'], 16)
                for i in range(v['slots']):
                    f = struct.unpack_from('<Q', img, vt + 8 * i)[0] - base
                    self.slot[f].append(f"{c['name']}[{i}]")
        self.callers = collections.defaultdict(set)
        for f, (e, ev) in self.code.items():
            for x in ev:
                if x[0] in ('call', 'jmp'): self.callers[x[1]].add(f)
    def strs(self, f, n=6):
        if f not in self.code: return []
        return [self.S[e[2]]['s'][:90] for e in self.code[f][1] if e[0] in ('lea', 'mov') and len(e) == 4 and e[2] in self.S][:n]
    def owner_guess(self, ss):
        for s in ss:
            m = re.match(r'([A-Za-z_][A-Za-z0-9_]*(?:Class|Wrapper|Manager|System|Behavior)?)::([A-Za-z_][A-Za-z0-9_]*)', s)
            if m: return f'{m.group(1)}::{m.group(2)}'
        return None
    def describe(self, f):
        cs = sorted(self.callers.get(f, []))[:8]
        callees = [x[1] for x in self.code.get(f, (0, []))[1] if x[0] == 'call'][:30]
        own = self.strs(f)
        d = {'rva': hex(f), 'vtable_slots': self.slot.get(f, [])[:6], 'strings': own,
             'name_guess': self.owner_guess(own),
             'callers': [{'rva': hex(c), 'vt': self.slot.get(c, [])[:2], 'strings': self.strs(c, 3), 'guess': self.owner_guess(self.strs(c, 12))} for c in cs],
             'callee_strings': sorted({s for c in callees for s in self.strs(c, 2)})[:8],
             'callee_vt': sorted({v for c in callees for v in self.slot.get(c, [])[:1]})[:8]}
        return d

if __name__ == '__main__':
    n = Namer(*sys.argv[1:4])
    for a in sys.argv[4:]:
        print(json.dumps(n.describe(int(a, 16)), indent=1))
