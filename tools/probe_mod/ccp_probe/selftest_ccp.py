#!/usr/bin/env python3
"""selftest_ccp.py - offline check of the ccpgu t02 probe kit (no game needed; the Steam install is optional).

  toolchain\\venv\\Scripts\\python.exe -I tools\\probe_mod\\ccp_probe\\selftest_ccp.py

1. The hook (CCP_CFG + ccp_hook.lua) runs under the stock Lua 5.0.2 (../common/lua50.py) with stub engine calls:
   armed run (ARM, PL, PT, ENUM, SPAWN x3 with invulnerability, OWN NEUTRAL -> EMPIRE, ALL, HB, ServiceRate 0.05, the
   shipped bodies skipped while armed), the main-menu case (no human Empire: SKIP, shipped bodies still run), the wrong
   AI faction (SKIP); its records go through the parser's record splitter.
2. The mod builder from extracted/foc_data_text.zip (offline): the generated BusyTacticalFreeStore.lua compiles under
   Lua 5.0.2 and carries the config and the hook.
3. Every type name the hook uses exists in the FoC XML; MARKERS.XML gives exactly the seven relevant point types.
4. ccp_reader.py --selftest (fake memory image) and ccp_parse.py --selftest (port model, deviations).
5. Cleanup (marker rule) on a temp game folder, the profile preset with a fake writer, the steps text.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_MOD = os.path.dirname(HERE)
ROOT = os.path.normpath(os.path.join(PROBE_MOD, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PROBE_MOD, 'common'))
import lua50  # noqa: E402
import build_ccp_mod  # noqa: E402
import ccp_cleanup  # noqa: E402
import ccp_maps  # noqa: E402
import ccp_parse  # noqa: E402
import ccp_run  # noqa: E402

ZIP = os.path.join(ROOT, 'extracted', 'foc_data_text.zip')

STUBS = r'''
-- ---- selftest stubs (Lua 5.0) ----
H_F = 0
H_SHIPPED_FSS = 0
H_SHIPPED_OUS = 0
H_INV = 0
function GetCurrentTime() return H_F / 30 end
function Get_Game_Mode() return H_MODE end
function H_Player(fac, id, human)
	local p = {}
	p.Get_Faction_Name = function() return fac end
	p.Get_ID = function() return id end
	p.Is_Human = function() return human end
	return p
end
H_PLAYERS = {}
function Find_Player(name)
	local p = H_PLAYERS[string.upper(name)]
	if p == nil then error("unknown faction " .. name) end
	return p
end
function H_Obj(id, owner, x, y)
	local o = { owner = owner }
	o.Is_Valid = function() return true end
	o.Get_Object_ID = function() return id end
	o.Get_Owner = function() return o.owner end
	o.Get_Position = function()
		local p = {}
		p.Get_XYZ = function() return x, y, 0 end
		return p
	end
	o.Make_Invulnerable = function(b)
		if type(b) ~= "boolean" then error("Make_Invulnerable expects a boolean") end
		H_INV = H_INV + 1
	end
	return o
end
H_TYPES = {}
function Find_Object_Type(name)
	if not H_TYPES[string.upper(name)] then return nil end
	return { name = name }
end
H_POINTS = {}
function Find_All_Objects_Of_Type(name)
	if not H_TYPES[string.upper(name)] then error("FindAllObjectsOfType -- unknown filter parameter 1.") end
	return H_POINTS[string.upper(name)] or {}
end
H_SPAWNS = 0
function Spawn_Unit(ty, where, player)
	if ty == nil or where == nil or player == nil then error("LuaSpawnUnitCommandClass -- Invalid parameter") end
	H_SPAWNS = H_SPAWNS + 1
	local x, y = where.Get_Position().Get_XYZ()
	return { H_Obj(9000 + H_SPAWNS, player, x + 10, y), H_Obj(9100 + H_SPAWNS, player, x - 10, y) }
end
ServiceRate = 20
function Base_Definitions() H_BASE = true end
function FreeStoreService() H_SHIPPED_FSS = H_SHIPPED_FSS + 1 end
function On_Unit_Service(o) H_SHIPPED_OUS = H_SHIPPED_OUS + 1 end
'''

DRIVER = r'''
-- ---- selftest driver ----
H_PLAYERS["REBEL"] = H_Player("REBEL", 0, false)
H_PLAYERS["EMPIRE"] = H_Player("EMPIRE", 1, H_EMPIRE_HUMAN)
H_PLAYERS["NEUTRAL"] = H_Player("NEUTRAL", 3, false)
for i = 1, table.getn(H_TYPE_NAMES) do H_TYPES[string.upper(H_TYPE_NAMES[i])] = true end
H_PT = H_Obj(82036, H_PLAYERS["NEUTRAL"], 3453.5, 2791.4)
H_POINTS["REINFORCEMENT_POINT"] = { H_PT }
H_POINTS["REINFORCEMENT_POINT_REBEL"] = { H_Obj(77, H_PLAYERS["REBEL"], 100, 100) }
Base_Definitions()
PlayerObject = H_Player(H_AI_FAC, 0, false)
H_RATE_ARMED = nil
for F = 0, H_FRAMES, 3 do
	H_F = F
	if F == 600 then H_PT.owner = H_PLAYERS["EMPIRE"] end
	FreeStoreService()
	On_Unit_Service(nil)
	if F == 300 then H_RATE_ARMED = ServiceRate end
end
print("H_STATE spawns=" .. H_SPAWNS .. " inv=" .. H_INV .. " shipped_fss=" .. H_SHIPPED_FSS .. " shipped_ous=" ..
	H_SHIPPED_OUS .. " rate_at_300=" .. tostring(H_RATE_ARMED) .. " base=" .. tostring(H_BASE))
for i = 1, CCP_N do print(CCP_LOG[i]) end
'''


def check(c, what, fails):
    print(('  ok    ' if c else '  FAIL  ') + what)
    if not c:
        fails.append(what)


def run_lua(lua, work, tag, cfg, ai_fac='REBEL', empire_human=True, frames=1200):
    names = list(cfg['relevant']) + list(cfg['fixed']) + [s['type'] for s in cfg['squads']]
    path = os.path.join(work, 'harness_%s.lua' % tag)
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        f.write(STUBS)
        f.write('H_MODE = "Land"\nH_AI_FAC = "%s"\nH_EMPIRE_HUMAN = %s\nH_FRAMES = %d\n' % (
            ai_fac, 'true' if empire_human else 'false', frames))
        f.write('H_TYPE_NAMES = { %s }\n' % ', '.join('"%s"' % n for n in names))
        f.write(build_ccp_mod.lua_config(cfg) + '\n')
        f.write(open(build_ccp_mod.HOOK, encoding='ascii').read())
        f.write(DRIVER)
    r = subprocess.run([lua, path], capture_output=True, text=True)
    text = r.stdout + r.stderr
    with open(os.path.join(work, 'lua_%s.txt' % tag), 'w') as f:
        f.write(text)
    recs = [ln for ln in r.stdout.splitlines() if ln.startswith('CCP1|')]
    state = next((ln for ln in r.stdout.splitlines() if ln.startswith('H_STATE')), '')
    return r.returncode, recs, state, text


def kinds(recs):
    out = {}
    for rec in recs:
        d = ccp_parse.lua_fields(rec)
        out.setdefault(d['kind'], []).append(d)
    return out


def main():
    fails = []
    work = tempfile.mkdtemp(prefix='ccp_selftest_')
    try:
        lua = lua50.find_lua()
        check(lua is not None, 'lua50.exe available (%s)' % (lua or lua50.BUILD_LOG), fails)
        cfg = build_ccp_mod.config('<no game>', build_ccp_mod.DEFAULT_MAP, 'EMPIRE', session='selftest')
        if lua:
            # 1a. armed run
            rc, recs, state, text = run_lua(lua, work, 'armed', cfg)
            k = kinds(recs)
            check(rc == 0 and 'H_STATE' in state, 'armed run: script ran to the end (rc %d)' % rc, fails)
            check(len(k.get('ARM', [])) == 1 and k['ARM'][0].get('human_id') == '1', 'ARM once, human id 1', fails)
            check(len(k.get('PL', [])) == 3, 'PL for Empire / Rebel / Neutral', fails)
            pts = k.get('PT', [])
            check(any(p.get('cls') == 'relevant' and p.get('obj') == '82036' and p.get('owner') == 'NEUTRAL:3' for p in pts)
                  and any(p.get('cls') == 'fixed' and p.get('owner') == 'REBEL:0' for p in pts),
                  'PT: relevant point Neutral, fixed point listed (%d)' % len(pts), fails)
            check(any(e.get('relevant') == '1' and e.get('points') == '1' for e in k.get('ENUM', [])), 'ENUM 1 point', fails)
            sp = k.get('SPAWN', [])
            check(len(sp) == 3 and all(s.get('ok') == '1' and s.get('inv') == '2' and s.get('owner') == 'EMPIRE:1' for s in sp),
                  'SPAWN x3 (2 squads + escort), Empire-owned, invulnerable (%s)' % [(s.get('type'), s.get('ok'), s.get('inv'))
                                                                                    for s in sp], fails)
            check(all(int(s['f']) >= cfg['start_frame'] for s in sp) and
                  all(int(p['f']) >= cfg['enum_frame'] for p in pts), 'enumerate / spawn not before their frames', fails)
            own = k.get('OWN', [])
            check(len(own) == 1 and own[0].get('from') == 'NEUTRAL:3' and own[0].get('to') == 'EMPIRE:1' and
                  own[0].get('f') == '600', 'OWN Neutral -> Empire at f=600 (%s)' % own, fails)
            check(len(k.get('ALL', [])) == 1 and k['ALL'][0].get('f') == '600', 'ALL at f=600', fails)
            check(len(k.get('HB', [])) >= 5, 'heartbeats', fails)
            check(not k.get('ERR') and not k.get('APIERR'), 'no ERR / APIERR (%s)' % (k.get('ERR'), ), fails)
            check('rate_at_300=0.05' in state and 'shipped_fss=0' in state and 'shipped_ous=0' in state and 'base=true' in state,
                  'ServiceRate 0.05 while armed, shipped bodies skipped, Base_Definitions chained (%s)' % state, fails)
            check(all(r.startswith('CCP1|') and r.endswith('|END') for r in recs), 'records are CCP1|...|END', fails)
            check(all(len(r) <= 400 for r in recs), 'records fit the reader regex length', fails)
            # 1b. main-menu background battle: no human Empire
            rc, recs, state, _ = run_lua(lua, work, 'menu', cfg, empire_human=False, frames=900)
            k = kinds(recs)
            check(rc == 0 and not k.get('ARM') and any(s.get('why') == 'no_human_Empire' for s in k.get('SKIP', [])),
                  'no human Empire: SKIP no_human_Empire, no ARM', fails)
            check('shipped_fss=0' not in state and 'rate_at_300=20' in state, 'no human: shipped bodies run, rate kept (%s)'
                  % state, fails)
            # 1c. the wrong AI faction
            rc, recs, state, _ = run_lua(lua, work, 'wrongai', cfg, ai_fac='EMPIRE', frames=300)
            k = kinds(recs)
            check(rc == 0 and not k.get('ARM') and k.get('SKIP'), 'AI faction EMPIRE: SKIP, no ARM', fails)
        # 2. builder offline
        if os.path.exists(ZIP):
            out = os.path.join(work, 'mod')
            p, bcfg = build_ccp_mod.build('<no game>', out, ZIP, build_ccp_mod.DEFAULT_MAP, 'EMPIRE',
                                          os.path.join(work, 'cfg.json'), session='selftest')
            body = open(p, 'rb').read()
            check(b'CCP_CFG = {' in body and b'CCPPROBE_SRC_MARK_V1' in body and b'\r\n' in body and
                  os.path.exists(os.path.join(out, build_ccp_mod.MARKER)), 'builder: config + hook appended (CRLF), marker', fails)
            check(body.find(b'function FreeStoreService') < body.find(b'CCP_Orig_FreeStoreService = FreeStoreService'),
                  'builder: the hook follows the shipped FreeStoreService', fails)
            if lua:
                chk = os.path.join(work, 'compile.lua')
                with open(chk, 'w') as f:
                    f.write('local f, e = loadfile(arg[1])\nif f then print("COMPILE_OK") else print("COMPILE_FAIL " .. tostring(e)) end\n')
                r = subprocess.run([lua, chk, p], capture_output=True, text=True)
                check('COMPILE_OK' in r.stdout, 'built BusyTacticalFreeStore.lua compiles under Lua 5.0.2 (%s)'
                      % (r.stdout.strip() or r.stderr.strip()), fails)
            # 3. type names
            names = set()
            with zipfile.ZipFile(ZIP) as zf:
                for n in zf.namelist():
                    if n.upper().endswith('.XML'):
                        names.update(x.upper() for x in re.findall(r'Name\s*=\s*"([^"]+)"', zf.read(n).decode('latin-1')))
            for side, (_h, _ai, squads) in build_ccp_mod.SIDES.items():
                for t, _n in squads:
                    check(t.upper() in names, 'type %s (%s side) exists in the FoC XML' % (t, side), fails)
            for t in ccp_maps.RELEVANT + ccp_maps.FIXED:
                check(t.upper() in names, 'marker type %s exists' % t, fails)
            flags = ccp_maps.marker_flags(ZIP)
            rel = sorted(n for n, f in flags.items() if f['relevant'] and f['capture_point'])
            check(rel == sorted(ccp_maps.RELEVANT), 'MARKERS.XML: exactly the seven relevant types (%s)' % rel, fails)
            check(not any(flags[n]['relevant'] or flags[n]['capture_point'] for n in ccp_maps.FIXED),
                  'faction-fixed points: no flag, no CAPTURE_POINT', fails)
        else:
            check(False, 'extracted/foc_data_text.zip present (builder / type checks)', fails)
        # 4. reader and parser selftests
        for script in ('ccp_reader.py', 'ccp_parse.py'):
            r = subprocess.run([sys.executable, '-I', os.path.join(HERE, script), '--selftest'], capture_output=True, text=True)
            last = (r.stdout.strip().splitlines() or ['-'])[-1]
            check(r.returncode == 0 and last.endswith('OK'), '%s --selftest: %s' % (script, last), fails)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
        # 5. cleanup marker rule, preset, steps
        game = os.path.join(work, 'game')
        mod = os.path.join(game, 'corruption', 'Mods', ccp_cleanup.MOD_NAME)
        os.makedirs(mod)
        rep = ccp_cleanup.cleanup(game)
        check(os.path.isdir(mod) and rep['ok'], 'cleanup leaves a folder without the marker', fails)
        open(os.path.join(mod, ccp_cleanup.MARKER), 'w').write('x')
        rep = ccp_cleanup.cleanup(game, dry_run=True)
        check(os.path.isdir(mod), 'cleanup --dry-run deletes nothing', fails)
        rep = ccp_cleanup.cleanup(game)
        check(not os.path.exists(mod) and rep['ok'], 'cleanup deletes the marked folder', fails)
        ok, msg = ccp_cleanup.restore_profile(os.path.join(work, 'nonexistent'))
        check(ok, 'profile restore without a run folder is a no-op (%s)' % msg, fails)
        wrote = []
        ccp_run.preset_profile('_mp_land_kamino', lambda n, k, v: wrote.append((n, k, v)))
        check(sorted(wrote) == [('LastInternetGameType', 'dword', 3),
                                ('LastInternetMapLand', 'sz', '.\\Data\\Art\\Maps\\_MP_LAND_KAMINO.TED')],
              'preset: game type 3 (= type 4) and the land map (%s)' % wrote, fails)
        txt = ccp_run.steps_text('_MP_LAND_KAMINO', 'EMPIRE', True, 'REBEL')
        check('Kamino' in txt and 'CAPTURE CONTROL POINTS' in txt and 'REBEL' in txt, 'steps text', fails)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('CCP SELFTEST %s' % ('OK' if not fails else 'FAILED (%d)' % len(fails)))
    return 0 if not fails else 1


if __name__ == '__main__':
    sys.exit(main())
