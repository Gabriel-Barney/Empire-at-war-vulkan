#!/usr/bin/env python3
"""Build the engine Lua API spec from (1) exe registration tables (lua_reg.py output),
(2) EmmyLua docs shipped with the mod, (3) usage mining across script corpora.
Usage: lua_api.py <lua_reg.json> <exe_strings xrefs.json> <out.json> <out.md> <corpus_name=dir>...
Corpus dirs are scanned for *.lua (case-insensitive)."""
import sys, os, re, json, collections, glob

LUA50_STD = set('''assert collectgarbage dofile error getfenv getmetatable gcinfo ipairs loadfile loadlib loadstring next pairs pcall print
rawequal rawget rawset require setfenv setmetatable tonumber tostring type unpack xpcall _G _VERSION _LOADED _REQUIREDNAME
coroutine string table math io os debug create resume status wrap yield byte char dump find format gfind gsub len lower rep sub upper
concat foreach foreachi getn insert remove setn sort abs acos asin atan atan2 ceil cos deg exp floor frexp ldexp log log10 max min mod
pi pow rad random randomseed sin sqrt tan'''.split())
KEYWORDS = set('and break do else elseif end false for function if in local nil not or repeat return then true until while'.split())

def strip(src):
    src = re.sub(r'--\[\[.*?\]\]', ' ', src, flags=re.S)
    src = re.sub(r'--[^\n]*', ' ', src)
    src = re.sub(r'\[\[.*?\]\]', '""', src, flags=re.S)
    src = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', src)
    src = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", src)
    return src

def scan(path):
    src = strip(open(path, 'rb').read().decode('latin1'))
    defs = set(re.findall(r'\bfunction\s+([A-Za-z_][\w]*)\s*\(', src))
    defs |= set(re.findall(r'\blocal\s+function\s+([A-Za-z_]\w*)', src))
    defs |= set(re.findall(r'^\s*(?:local\s+)?([A-Za-z_]\w*)\s*=\s*function\b', src, re.M))
    mdefs = set(m[1] for m in re.findall(r'\bfunction\s+[A-Za-z_][\w.]*[.:]([A-Za-z_]\w*)\s*\(', src))
    mdefs |= set(re.findall(r'[.:]\s*([A-Za-z_]\w*)\s*=\s*function\b', src))
    gvars = set(re.findall(r'^\s*([A-Za-z_]\w*)\s*=(?!=)', src, re.M))
    calls = collections.Counter()
    for m in re.finditer(r'(?<![.:\w])([A-Za-z_]\w*)\s*\(', src):
        n = m.group(1)
        if n not in KEYWORDS: calls[n] += 1
    mcalls = collections.Counter(m.group(1) for m in re.finditer(r'[.:]\s*([A-Za-z_]\w*)\s*\(', src))
    # Story/AI callback entrypoints the engine calls into scripts
    return defs, mdefs, gvars, calls, mcalls

def main(regj, xrefj, outj, outmd, corpora):
    reg = json.load(open(regj))
    strings = {v['s'] for v in json.load(open(xrefj))['strings'].values()}
    glob_reg = {}; mem_reg = collections.defaultdict(dict)
    for t in reg['tables']:
        for r in t['registrations']:
            if r['registrar'] == '0x247000':
                glob_reg.setdefault(r['name'], {'class': r['object_class'], 'registered_in': t['function'], 'site': r['site']})
            elif r['registrar'] == '0x24be40' and r.get('wrapper'):
                owner = re.sub(r'^LuaMemberFunctionWrapper<V?(.*?)>$', r'\1', r['wrapper'])
                mem_reg[owner].setdefault(r['name'], {'impl': r['impl'], 'site': r['site']})
    # EmmyLua docs (optional, inside corpora)
    emmy_g, emmy_m = set(), collections.defaultdict(set)
    per = {}
    alldefs, allmdefs, allg = set(), set(), set()
    usage = collections.defaultdict(lambda: collections.Counter()); musage = collections.defaultdict(lambda: collections.Counter())
    for spec in corpora:
        name, d = spec.split('=', 1)
        files = [p for p in glob.glob(os.path.join(d, '**', '*'), recursive=True) if p.lower().endswith('.lua')]
        per[name] = len(files)
        for p in files:
            rel = os.path.relpath(p, d).replace('\\', '/')
            if 'emmyluadoc' in rel:
                src = open(p, 'rb').read().decode('latin1')
                emmy_g |= set(re.findall(r'^function\s+([A-Za-z_]\w*)\s*\(', src, re.M))
                for c, m in re.findall(r'^function\s+([A-Za-z_]\w*)[.:]([A-Za-z_]\w*)\s*\(', src, re.M): emmy_m[c].add(m)
                continue
            parts = rel.split('/')
            sub = parts[0] if len(parts) > 1 else '(root)'
            if sub.upper() == 'AI' and len(parts) > 2: sub = 'AI/' + parts[1]
            bucket = f'{name}:{sub.upper()}'
            defs, mdefs, gvars, calls, mcalls = scan(p)
            alldefs |= defs; allmdefs |= mdefs; allg |= gvars
            for k, v in calls.items(): usage[k][bucket] += v
            for k, v in mcalls.items(): musage[k][bucket] += v
    exe_member_names = {m for d in mem_reg.values() for m in d}
    api = []
    # globals: registered OR (called, not defined anywhere, present as exe string or documented)
    cand = set(glob_reg) | {n for n in usage if n not in alldefs and n not in LUA50_STD and (n in strings or n in emmy_g)}
    for n in sorted(cand, key=str.lower):
        u = usage.get(n, {})
        api.append({'kind': 'global', 'name': n, 'registered': n in glob_reg,
                    'class': glob_reg.get(n, {}).get('class'), 'registered_in': glob_reg.get(n, {}).get('registered_in'),
                    'in_exe_strings': n in strings, 'documented_emmylua': n in emmy_g, 'defined_in_scripts': n in alldefs,
                    'calls_total': sum(u.values()), 'calls_by_dir': dict(sorted(u.items(), key=lambda x: -x[1]))})
    for owner in sorted(mem_reg):
        for m in sorted(mem_reg[owner], key=str.lower):
            u = musage.get(m, {})
            api.append({'kind': 'method', 'owner': owner, 'name': m, 'impl': mem_reg[owner][m]['impl'],
                        'documented_emmylua': any(m in s for s in emmy_m.values()),
                        'calls_total_any_receiver': sum(u.values()), 'calls_by_dir': dict(sorted(u.items(), key=lambda x: -x[1])[:8])})
    # method names called in scripts that are not script-defined, not registered, but are exe strings -> likely engine methods not captured
    unresolved_m = sorted(n for n in musage if n not in allmdefs and n not in exe_member_names and n not in LUA50_STD and n in strings and n not in glob_reg)
    callbacks = sorted(n for n in alldefs if n in strings and n not in glob_reg and len(n) > 3)
    unresolved_g = sorted(n for n in usage if n not in alldefs and n not in LUA50_STD and n not in cand and n[0].isupper())
    json.dump({'corpora_file_counts': per, 'lua_version': '5.0.2 (statically linked)',
               'registrar_functions': {'global': '0x247000 (LuaScriptClass::Register_Global-like: rcx=script, rdx=LuaVar object, r8=name)',
                                       'member': '0x24be40 (LuaUserVar member table add: rcx=table, rdx=name, r8=LuaMemberFunctionWrapper<T>)'},
               'api': api, 'unresolved_method_names_in_exe_strings': unresolved_m,
               'unresolved_global_calls': unresolved_g[:400],
               'script_functions_named_in_exe (engine->script callbacks)': callbacks,
               'emmylua_classes': {k: sorted(v) for k, v in emmy_m.items()}}, open(outj, 'w'), indent=1)
    # markdown
    G = [a for a in api if a['kind'] == 'global']; M = [a for a in api if a['kind'] == 'method']
    with open(outmd, 'w') as f:
        f.write('# Engine Lua API surface (StarWarsG.exe FoC x64, Lua 5.0.2)\n\n')
        f.write('Generated by `tools/re/lua_api.py` from the exe registration tables (`tools/re/lua_reg.py`), the EmmyLua stubs shipped with '
                "Thrawn's Revenge, and call-site mining over: " + ', '.join(f'{k} ({v} files)' for k, v in per.items()) + '.\n\n')
        f.write('## How the engine registers Lua bindings\n\n'
                '- Every exported global is a heap-allocated **LuaVar/LuaUserVar subclass object** (one C++ class per function, e.g. '
                '`FindPlanetClass`, `LuaCreateThread`, `SpawnFromReinforcePoolClass`) registered by name with registrar `0x140247000` '
                '(`rcx`=LuaScriptClass, `rdx`=object, `r8`=name). Calling it from Lua invokes the object\'s virtual call operator, which '
                'receives a `LuaScriptClass*` and a `LuaTable` of params and returns a `LuaTable` of results.\n'
                '- Methods on userdata objects (GameObject, Player, TaskForce, ...) are registered per wrapper class via registrar `0x14024be40` '
                '(`rdx`=method name, `r8`=`LuaMemberFunctionWrapper<T>` holding a pointer-to-member). Each wrapper class has one registration '
                'function (e.g. GameObjectWrapper: `0x14056d4c0`, 131 methods).\n'
                '- Argument validation errors are reported with strings of the form `Class::Lua_Method -- invalid type for parameter N` '
                '(see `out/xrefs`); these give exact parameter-count/type contracts and are the best source for signatures.\n'
                '- Script search path: `LUA_PATH` default `./?.lua;./?.lc`; script roots `.\\Data\\Scripts\\{AI,AI\\SpaceMode,AI\\LandMode,'
                'AI\\StoryArc,Interventions,Evaluators,GameObject,Story,Library,FreeStore,Miscellaneous}\\`.\n\n')
        f.write(f'## Globals ({len(G)})\n\nColumns: registered in exe | EmmyLua-documented | total call sites | top dirs.\n\n')
        f.write('| name | impl class | reg | doc | calls | used in |\n|---|---|---|---|---|---|\n')
        for a in sorted(G, key=lambda a: -a['calls_total']):
            dirs = ', '.join(f'{k}:{v}' for k, v in list(a['calls_by_dir'].items())[:4])
            f.write(f"| `{a['name']}` | {a['class'] or ''} | {'Y' if a['registered'] else '-'} | {'Y' if a['documented_emmylua'] else '-'} | {a['calls_total']} | {dirs} |\n")
        by = collections.defaultdict(list)
        for a in M: by[a['owner']].append(a)
        f.write(f'\n## Methods by wrapper class ({len(M)})\n\n`calls` counts call sites of that method name on any receiver (name collisions across classes are possible).\n')
        for o in sorted(by, key=lambda o: -len(by[o])):
            f.write(f'\n### {o} ({len(by[o])})\n\n| method | impl RVA | doc | calls | used in |\n|---|---|---|---|---|\n')
            for a in sorted(by[o], key=lambda a: -a['calls_total_any_receiver']):
                dirs = ', '.join(f'{k}:{v}' for k, v in list(a['calls_by_dir'].items())[:3])
                f.write(f"| `{a['name']}` | {a['impl'] or ''} | {'Y' if a['documented_emmylua'] else '-'} | {a['calls_total_any_receiver']} | {dirs} |\n")
        f.write(f'\n## Engine -> script callbacks\n\nScript-defined function/table names that also appear as exe strings, i.e. names the engine looks up and calls '
                f'or reads ({len(callbacks)}): ' + ', '.join(f'`{x}`' for x in callbacks) + '\n')
        f.write(f'\n## Gaps\n\n- Method names called in scripts that exist as exe strings but were not captured by the member-registration '
                f'scan ({len(unresolved_m)}; may be methods of wrappers registered through an inlined path, or false positives such as '
                f'table fields): ' + ', '.join(f'`{x}`' for x in unresolved_m[:200]) + '\n')
        f.write(f'- Capitalized global calls neither defined in scripts nor found in exe ({len(unresolved_g)}; mostly functions defined in '
                'Library files not staged because they are nested > 7 folders deep on the PC, i.e. TR `deepcore`/`eawx-plugins`): '
                + ', '.join(f'`{x}`' for x in unresolved_g[:150]) + '\n')
    print(len(G), 'globals;', len(M), 'methods;', len(unresolved_m), 'unresolved methods;', len(unresolved_g), 'unresolved globals')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
