#!/usr/bin/env python3
"""Plan class-attribution renames for the Ghidra project (applied by ApplyRenames.java).

Sources, highest priority first (a function gets the first rule that names it):
  lua_member  : impl pointer captured at each LuaMemberFunctionWrapper registration -> Owner::Lua_<Name>
  string      : function references 'Class::Method --'-style diagnostic strings naming exactly one (Class,Method)
  lua_global  : vftable slot 10 ('call' operator) of each registered Lua global class -> Class::Lua_Call (+ Lua name in comment)
  ctor_dtor   : functions that store a class vftable pointer (ctor/dtor/scalar deleting dtor)
  vftable     : remaining FUN_ functions in vftable slots -> Owner::vf<N> (owner = most-base class sharing that slot target)
Only FUN_/thunk_FUN_ (default-named) functions are renamed.
Usage: plan_renames.py <ghidra_out_dir> <re_work_dir> <out_renames.csv> <out_stats.json>
"""
import sys, os, csv, json, struct, pickle, bisect, re, collections
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import xrefs

OP_NEW, OP_DELETE = 0x769c58, 0x769c94
FIXED = {OP_NEW: ('', 'operator_new', 'callers=2850; size-only arg, returns heap block'),
         OP_DELETE: ('', 'operator_delete', 'thunk to free'),
         0x247000: ('LuaScriptClass', 'Register_Global_Var', 'rcx=script rdx=LuaVar* r8=name'),
         0x24be40: ('LuaUserVar', 'Register_Member', 'rcx=member table rdx=name r8=LuaMemberFunctionWrapper*'),
         0x249c20: ('LuaUserVar', 'LuaUserVar', 'base ctor used by every Lua binding object'),
         0x216b50: ('MegaFileManagerClass', 'Add_Megafile', 'string-proven'),
         0x217740: ('MegaFileManagerClass', 'Load_Megafiles_XML', 'builds <prefix>Data\\MegaFiles.xml'),
         0x21dce0: ('', 'Find_EaW_Fallback_Path', 'logs Setting FALLBACK_PATH'),
         0x5d990: ('', 'Game_Boot_Init_Filesystem', 'mount order, fonts, registry'),
         0x22e490: ('ThreadClass', 'Execute', 'only CreateThread path'),
         0x546c8b: ('', 'Register_Lua_Global_Commands', '~100 Lua globals'),
         }
PAT = re.compile(r'\b([A-Z][A-Za-z0-9_]*(?:<[^>]*>)?)::(~?[A-Za-z_][A-Za-z0-9_]*)\b')

def split_ns(full):
    """split 'A::B<C::D>::E' on top-level '::'"""
    parts, depth, cur, i = [], 0, '', 0
    while i < len(full):
        ch = full[i]
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        if depth == 0 and full.startswith('::', i):
            parts.append(cur); cur = ''; i += 2; continue
        cur += ch; i += 1
    parts.append(cur)
    return parts

def main(gout, rew, out_csv, out_stats):
    F = {int(r['rva'], 16): r for r in csv.DictReader(open(f'{gout}/functions.csv'))}
    starts = sorted(F)
    def gfn(site):
        i = bisect.bisect_right(starts, site) - 1
        if i < 0: return None
        s = starts[i]
        return s if site < s + max(int(F[s]['size']), 1) + 16 else None
    is_default = lambda r: r['name'].startswith(('FUN_', 'thunk_FUN_'))
    gcls = {}
    for r in csv.DictReader(open(f'{gout}/classes.csv')):
        for v in r['vftable_rvas'].split(): gcls[int(v, 16)] = r['class']
    rtti = json.load(open(f'{rew}/out/rtti_foc.json'))
    db = xrefs.build(f'{rew}/bin/StarWarsG_foc.exe'); img = db['img']; base = db['base']
    tlo, thi = db['secs']['.text']
    q = lambda a: struct.unpack_from('<Q', img, a)[0]
    # class info keyed by ghidra name
    bases_of = {}; vts = collections.defaultdict(list)
    mang2g = {}
    for c in rtti:
        for v in c['vtables']:
            g = gcls.get(int(v['rva'], 16))
            if g: mang2g[c['mangled']] = g
    for c in rtti:
        g = mang2g.get(c['mangled'])
        if not g: continue
        bl = (c.get('hierarchy') or {}).get('bases', [])[1:]
        bases_of[g] = {mang2g.get(b['name'], b['name']) for b in bl}
        for v in c['vtables']:
            vts[g].append((int(v['rva'], 16), v['slots'], v['this_offset']))
    vt2cls = {vt: g for g, l in vts.items() for vt, n, o in l}
    # slot occupancy
    occ = collections.defaultdict(list)  # fn -> [(class, this_off, slot)]
    for g, l in vts.items():
        for vt, n, off in l:
            for i in range(n):
                t = q(vt + 8 * i) - base
                if tlo <= t < thi: occ[t].append((g, off, i))
    plan = {}  # rva -> (ns, name, source, conf, note)
    def put(rva, ns, name, src, conf, note=''):
        if rva in plan: return False
        r = F.get(rva)
        if r is not None and not is_default(r): return False
        plan[rva] = (ns, name, src, conf, note); return True
    for rva, (ns, nm, note) in FIXED.items(): put(rva, ns, nm, 'fixed', 'high', note)
    # lua members
    api = json.load(open(f'{rew}/out/lua_api.json'))['api']
    for a in api:
        if a['kind'] == 'method' and a.get('impl'):
            put(int(a['impl'], 16), a['owner'] if a['owner'] in vts or a['owner'] in bases_of else a['owner'], 'Lua_' + a['name'], 'lua_member', 'high',
                f"Lua method {a['owner']}.{a['name']}")
    # string-proven names
    S = db['strings']
    fn_pairs = collections.defaultdict(set)
    for srva, v in S.items():
        for m in PAT.finditer(v['s']):
            cl, me = m.group(1), m.group(2)
            if me in ('ERROR', 'ALERT') or len(me) < 3: continue
            for x in v['xrefs']:
                if x is None or x < 0: continue
                g = gfn(x)
                if g is not None: fn_pairs[g].add((cl, me))
    for g, pairs in fn_pairs.items():
        if len(pairs) == 1:
            cl, me = next(iter(pairs))
            put(g, cl, me, 'string', 'high')
    # lua globals: slot 10 of registered classes
    for a in api:
        if a['kind'] == 'global' and a.get('class'):
            cands = [g for g in vts if g == a['class'] or g.replace('<', '').startswith(a['class'])]
            for g in cands:
                vt = sorted(vts[g], key=lambda x: x[2])[0]
                if vt[1] > 10:
                    t = q(vt[0] + 80) - base
                    if t != 0x17d070:
                        put(t, g, 'Lua_Call', 'lua_global', 'high', f"Lua global {a['name']}")
    # ctor / dtor from vftable stores
    code = pickle.load(open(f'{rew}/out/code_foc.pkl', 'rb'))
    gref = collections.defaultdict(list)   # ghidra fn -> ordered vtable classes referenced
    gcalls = collections.defaultdict(list)
    for f, (e, ev) in code.items():
        g = gfn(f)
        if g is None: continue
        for x in ev:
            if x[0] == 'lea' and len(x) == 4 and x[2] in vt2cls: gref[g].append(vt2cls[x[2]])
            if x[0] == 'call': gcalls[g].append(x[1])
    # callers that call new right before calling g
    new_then = collections.Counter()
    for f, (e, ev) in code.items():
        calls = [x[1] for x in ev if x[0] == 'call']
        for i, c in enumerate(calls):
            if c == OP_NEW and i + 1 < len(calls): new_then[calls[i + 1]] += 1
            if c == OP_NEW and i + 2 < len(calls): new_then[calls[i + 2]] += 0.5
    deleting = {}
    for g, cl in gref.items():
        cls = list(dict.fromkeys(cl))
        if len(cls) > 4 or int(F[g]['size']) > 0x600: continue
        derived = [c for c in cls if all(o == c or o in bases_of.get(c, set()) for o in cls)]
        if not derived: continue
        C = derived[0]
        in_slot = any(o[0] == C or C in bases_of.get(o[0], set()) or o[0] in bases_of.get(C, set()) for o in occ.get(g, []))
        if in_slot and OP_DELETE in gcalls[g]:
            if put(g, C, "`scalar_deleting_destructor'", 'ctor_dtor', 'high'): deleting[g] = C
        elif new_then.get(g, 0) >= 1:
            put(g, C, C.split('::')[-1].split('<')[0], 'ctor_dtor', 'high', 'called right after operator_new')
    for g, C in deleting.items():
        for c in gcalls[g]:
            if c != OP_DELETE and c in gref and C in gref[c]:
                put(c, C, '~' + C.split('::')[-1].split('<')[0], 'ctor_dtor', 'high', 'called from scalar deleting dtor')
    for g, cl in gref.items():
        if g in plan: continue
        cls = list(dict.fromkeys(cl))
        if len(cls) > 4 or int(F[g]['size']) > 0x600: continue
        derived = [c for c in cls if all(o == c or o in bases_of.get(c, set()) for o in cls)]
        if not derived: continue
        C = derived[0]; short = C.split('::')[-1].split('<')[0]
        if len(cls) > 1:
            if cl[0] == C: put(g, C, '~' + short, 'ctor_dtor', 'medium', 'own vftable stored before base vftables')
            else: put(g, C, short, 'ctor_dtor', 'medium', 'base vftables stored before own')
        else:
            put(g, C, 'ctor_or_dtor', 'ctor_dtor', 'low')
    # vftable slots
    icf = 0
    for t, lst in occ.items():
        if t not in F and not (tlo <= t < thi): continue
        owners = {g for g, off, i in lst}
        root = [g for g in owners if all(o == g or g in bases_of.get(o, set()) for o in owners)]
        if not root:
            icf += 1; continue
        R = root[0]
        slots = sorted({(off, i) for g, off, i in lst if g == R}) or sorted({(off, i) for g, off, i in lst})
        off, i = slots[0]
        put(t, R, f'vf{i}' + (f'_this{off:x}' if off else ''), 'vftable', 'medium')
    # write
    rows = []
    for rva, (ns, nm, src, conf, note) in sorted(plan.items()):
        exists = rva in F
        rows.append([hex(rva), ns, nm, src, conf, 'existing' if exists else 'create', note])
    with open(out_csv, 'w', newline='') as fo:
        w = csv.writer(fo); w.writerow(['rva', 'namespace', 'name', 'source', 'confidence', 'function', 'note'])
        w.writerows(rows)
    st = {'by_source': collections.Counter(r[3] for r in rows), 'to_create': sum(1 for r in rows if r[5] == 'create'),
          'icf_shared_slots_skipped': icf, 'default_named_before': sum(1 for r in F.values() if is_default(r)), 'total_functions': len(F)}
    json.dump(st, open(out_stats, 'w'), indent=1); print(st)

if __name__ == '__main__':
    main(*sys.argv[1:5])
