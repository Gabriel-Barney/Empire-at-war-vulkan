#!/usr/bin/env python3
"""Map every Lua global/method name -> C++ implementation RVA -> owning class, plus the
argument contract recovered from the implementation's diagnostic strings.
Globals: each global is an instance of a LuaUserVar subclass; vftable slot 10 is the call
operator (LuaUserVar default 0x17d070 = 'not callable'). Methods: pointer-to-member captured
at the LuaMemberFunctionWrapper<T> registration (slot 10 of the wrapper = generic invoker 0x244d70).
Usage: lua_bind_map.py <re_work_dir> <ghidra_functions.csv>   (updates out/lua_api.json, docs/re/lua_api.json, appends to docs/re/LUA_API.md)"""
import sys, os, json, csv, struct, re, collections, bisect
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import xrefs

def main(rew, gfuncs):
    api_path = f'{rew}/out/lua_api.json'
    J = json.load(open(api_path)); api = J['api']
    rtti = json.load(open(f'{rew}/out/rtti_foc.json'))
    db = xrefs.build(f'{rew}/bin/StarWarsG_foc.exe'); img = db['img']; base = db['base']
    q = lambda a: struct.unpack_from('<Q', img, a)[0]
    F = {int(r['rva'], 16): r for r in csv.DictReader(open(gfuncs))}
    starts = sorted(F)
    def gfn(site):
        i = bisect.bisect_right(starts, site) - 1
        return starts[i] if i >= 0 and site < starts[i] + int(F[starts[i]]['size']) + 16 else None
    # strings per ghidra function
    fstr = collections.defaultdict(list)
    for s, v in db['strings'].items():
        for x in v['xrefs']:
            if x is not None and x >= 0:
                g = gfn(x)
                if g is not None: fstr[g].append(v['s'])
    byname = {c['name']: c for c in rtti}
    def slot10(cname):
        c = byname.get(cname)
        if not c or not c['vtables']: return None
        v = sorted(c['vtables'], key=lambda v: v['this_offset'])[0]
        if v['slots'] <= 10: return None
        t = q(int(v['rva'], 16) + 80) - base
        return t
    def contract(fn):
        ss = fstr.get(fn, [])
        out = []
        for s in ss:
            if re.search(r'(?i)param|expected|argument|invalid type|missing', s):
                out.append(re.sub(r'\s+', ' ', s.strip())[:140])
        return sorted(set(out))[:8]
    def vcall(t):
        b = bytes(img[t:t + 8])
        if b[:3] == b'\x48\x8b\x01' and b[3:5] == b'\xff\xa0': return struct.unpack_from('<i', img, t + 5)[0]
        if b[:3] == b'\x48\x8b\x01' and b[3:5] == b'\xff\x60': return b[5]
        if b[:3] == b'\x48\x8b\x01' and b[3:5] == b'\xff\x20': return 0
        return None
    derived = collections.defaultdict(list)
    for c in rtti:
        for b in (c.get('bases') or []): derived[b].append(c['name'])
    def slot_target(cname, off):
        c = byname.get(cname)
        if not c or not c['vtables']: return None
        v = sorted(c['vtables'], key=lambda v: v['this_offset'])[0]
        if off // 8 >= v['slots']: return None
        return q(int(v['rva'], 16) + off) - base
    n_vc = 0
    n_g = n_m = 0
    for a in api:
        if a['kind'] == 'global':
            t = slot10(a['class']) if a.get('class') else None
            a['impl_rva'] = hex(t) if t and t != 0x17d070 else None
            a['impl_note'] = 'vftable slot 10 (call operator) of the registered LuaUserVar subclass' if a['impl_rva'] else \
                ('class does not override the call slot (value/table object, e.g. FreeStore, PlayerObject)' if a.get('class') else 'registration object class not resolved')
            if a['impl_rva']:
                n_g += 1
                g = gfn(t); a['impl_ghidra_name'] = (F[g]['namespace'] + '::' + F[g]['name']) if g in F else None
                a['arg_contract'] = contract(g if g is not None else t)
        else:
            if a.get('impl'):
                t = int(a['impl'], 16); n_m += 1
                a['impl_rva'] = a['impl']
                off = vcall(t)
                if off is not None:
                    n_vc += 1
                    a['vcall_thunk'] = a['impl']; a['vtable_slot'] = off // 8
                    r = slot_target(a['owner'], off)
                    if r: a['impl_rva'] = hex(r); t = r
                    ov = {}
                    for d in derived.get(a['owner'], []):
                        x = slot_target(d, off)
                        if x and x != r: ov[d] = hex(x)
                    if ov: a['overrides'] = ov
                g = gfn(t)
                a['impl_ghidra_name'] = (F[g]['namespace'] + '::' + F[g]['name']) if g in F else None
                a['arg_contract'] = contract(g if g is not None else t)
    J['binding_mechanism'] = {
        'globals': 'Each Lua global is a heap object (LuaUserVar subclass) registered by name via LuaScriptClass::Register_Global_Var (0x247000). '
                   'Invocation dispatches through vftable slot 10 (offset +0x50): Lua_Call(LuaScriptClass* script, LuaTable* params) -> LuaTable* results. '
                   'Slot 2 = scalar deleting destructor, slot 4 = per-class type hook.',
        'methods': 'Per wrapper class T, a LuaMemberFunctionWrapper<T> object holding a pointer-to-member (+0x28/+0x30, adjustor at +0x38, flag +0x40) '
                   'is added to T\'s member table via LuaUserVar::Register_Member (0x24be40). Its slot 10 (0x244d70) is the shared invoker that '
                   'unwraps `this` from the Lua userdata and calls the member with (LuaScriptClass*, LuaTable* params). When the registered member is virtual, '
                   'the stored pointer is an MSVC vcall thunk (`mov rax,[rcx]; jmp [rax+N]`); impl_rva then gives the owner class slot N/8 and '
                   '`overrides` the derived-class implementations (e.g. TaskForceClass -> Galactic/Space/LandTaskForceClass).'}
    json.dump(J, open(api_path, 'w'), indent=1)
    json.dump(J, open(f'{rew}/docs/re/lua_api.json', 'w'), indent=1)
    # markdown section
    md = open(f'{rew}/docs/re/LUA_API.md').read()
    md = md.split('\n## Binding map (Lua name -> C++)')[0]
    L = ['', '## Binding map (Lua name -> C++)', '',
         J['binding_mechanism']['globals'], '', J['binding_mechanism']['methods'], '',
         f'Resolved: {n_g} globals and {n_m} methods to implementation RVAs (names as persisted in the Ghidra project). '
         'The "contract" column lists the implementation\'s own argument-validation messages: these are the authoritative '
         'parameter counts/types for the bridge.', '', '### Globals', '', '| Lua | impl RVA | class | contract (from diagnostics) |', '|---|---|---|---|']
    for a in sorted([a for a in api if a['kind'] == 'global'], key=lambda a: a['name'].lower()):
        L.append(f"| `{a['name']}` | {a.get('impl_rva') or '-'} | {a.get('class') or '?'} | {'<br>'.join(x.replace('|', '/') for x in a.get('arg_contract', [])[:3])} |")
    L += ['', '### Methods', '', '| Lua | impl RVA | contract (from diagnostics) |', '|---|---|---|']
    for a in sorted([a for a in api if a['kind'] == 'method'], key=lambda a: (a['owner'], a['name'])):
        virt = f" (virtual slot {a['vtable_slot']}; overrides: {', '.join(f'{k} {v}' for k, v in a.get('overrides', {}).items())})" if 'vtable_slot' in a else ''
        L.append(f"| `{a['owner']}.{a['name']}` | {a.get('impl_rva') or '-'}{virt} | {'<br>'.join(x.replace('|', '/') for x in a.get('arg_contract', [])[:3])} |")
    open(f'{rew}/docs/re/LUA_API.md', 'w').write(md + '\n'.join(L) + '\n')
    print('globals resolved', n_g, 'methods', n_m, 'virtual (vcall thunk) methods', n_vc)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
