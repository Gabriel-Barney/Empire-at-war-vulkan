#!/usr/bin/env python3
"""selftest_hook.py - runs the built probe script (shipped BusyTacticalFreeStore.lua + fow_hook.lua) under a stock
Lua 5.0.2 with stubbed engine functions (no game needed).

  python -I selftest_hook.py [--source-zip extracted\\foc_data_text.zip]     ends with SELFTEST_HOOK OK / FAILED / SKIPPED

The harness plays the engine's free-store service every 3rd frame (AI_TACTICAL §5.3) with the AI_GALACTIC §1.10
gates read from the script's own ServiceRate / UnitServiceRate globals, for 120 s of a land battle. Checks: the hook
loads under Lua 5.0, lowers both rates, steps every 3 frames, finds the human, spawns two objects, runs 4 regrow
trials (each undo at the wanted decay phase) and 10 top-edge reveals with matching Undo_Reveal calls, samples
visibility, ends with DONE and no ERR record; the shipped FreeStoreService still runs on its 20 s schedule and the
shipped On_Unit_Service on its 2 s schedule, never for the probe's own objects.
Lua: out\\tools\\lua50.exe (built here with the project MSVC at /Od on first use, as a1_tactical does; MSVC /O2
miscompiles stock 5.0.2) or a1_tactical\\out\\tools\\lua50.exe. Without the toolchain the test is SKIPPED.
"""
import argparse
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import build_fow_mod  # noqa: E402

OUT = os.path.join(HERE, 'out', 'selftest')
TOOLS = os.path.join(HERE, 'out', 'tools')
LUA_SRC = ['lapi', 'lcode', 'ldebug', 'ldo', 'ldump', 'lfunc', 'lgc', 'llex', 'lmem', 'lobject', 'lopcodes', 'lparser',
           'lstate', 'lstring', 'ltable', 'ltm', 'lundump', 'lvm', 'lzio']
LUA_LIB = ['lauxlib', 'lbaselib', 'ldblib', 'liolib', 'lmathlib', 'loadlib', 'lstrlib', 'ltablib']
FAILS = []

STUBS = r'''
FWT = { F = 0, now = 0, revealed = 0, undo_f = -1000, calls = { fss = 0, ous = 0, ous_mine = 0 }, rev = {}, undo = {} }
GetCurrentTime = setmetatable({ Frame = function() return FWT.F end }, { __call = function() return FWT.now end })
function require(name) end
function Common_Base_Definitions() end
function DebugMessage() end
function ScriptExit() end
function PumpEvents() coroutine.yield() end
FindTarget = { Reachable_Target = function() FWT.calls.fss = FWT.calls.fss + 1 return nil end }
function EvaluatePerception() return 1 end
function Get_Game_Mode() return "Land" end
function TestValid(o) return o ~= nil end
function make_pos(x, y) return { x = x, y = y } end
function Create_Position(x, y, z) return make_pos(x, y) end
function dist(a, b)
	local pa, pb = a.pos or a, b.pos or b
	return math.sqrt((pa.x - pb.x) * (pa.x - pb.x) + (pa.y - pb.y) * (pa.y - pb.y))
end
function make_obj(tname, x, y, owner)
	local o = { pos = make_pos(x, y), tname = tname, owner = owner }
	o.Get_Position = function() return o.pos end
	o.Get_Distance = function(other) return dist(o, other) end
	o.Get_Type = function() return { Get_Name = function() return tname end } end
	o.Prevent_AI_Usage = function(b) o.noai = b end
	o.Prevent_All_Fire = function(b) end
	o.Make_Invulnerable = function(b) end
	o.Is_Category = function(c) return false end
	o.Get_Attack_Target = function() return nil end
	o.Has_Active_Orders = function()
		if string.upper(tname) == "MPTL" and FWT.F > 180 then FWT.calls.ous_mine = FWT.calls.ous_mine + 1 end
		FWT.calls.ous = FWT.calls.ous + 1
		return true
	end
	return o
end
HUMAN = { Get_ID = function() return 0 end, Is_Human = function() return true end, Get_Faction_Name = function() return "EMPIRE" end }
PlayerObject = { Get_ID = function() return 1 end, Is_Human = function() return false end,
	Get_Faction_Name = function() return "Rebel" end, Get_Space_Station = function() return nil end }
FWT.players = { Empire = HUMAN, Rebel = PlayerObject }
function Find_Player(name) return FWT.players[name] end
FWT.hobjs = { make_obj("E_GROUND_HQ", 700, 3000, HUMAN), make_obj("STORMTROOPER_TEAM", 760, 2950, HUMAN) }
FWT.aobjs = { make_obj("R_GROUND_HQ", 3100, 650, PlayerObject), make_obj("REBEL_INFANTRY_SQUAD", 2600, 900, PlayerObject) }
FWT.spawned = {}
function Find_All_Objects_Of_Type(p)
	if p == HUMAN then return FWT.hobjs end
	if p == PlayerObject then return FWT.aobjs end
	return {}
end
function Find_Object_Type(name) return { name = name } end
function Create_Generic_Object(ty, pos, player)
	local o = make_obj(ty.name, pos.x + 8 * table.getn(FWT.spawned), pos.y, player)
	table.insert(FWT.spawned, o)
	return o
end
function Find_Nearest(src, a2, a3, a4)
	if a3 ~= HUMAN or a4 ~= false then return nil end
	local tgt = FWT.spawned[2]
	if FWT.revealed > 0 or FWT.F < FWT.undo_f + 200 then return tgt end
	return nil
end
FogOfWar = {
	Reveal = function(p, target, r)
		if p ~= HUMAN or type(r) ~= "number" then error("bad reveal args") end
		FWT.revealed = FWT.revealed + 1
		local pos = target.pos or target
		table.insert(FWT.rev, string.format("%d:%.1f:%.1f:%g", FWT.F, pos.x, pos.y, r))
		local cells = {}
		cells.Undo_Reveal = function()
			if not cells.done then
				cells.done = true
				FWT.revealed = FWT.revealed - 1
				FWT.undo_f = FWT.F
				table.insert(FWT.undo, FWT.F)
			end
		end
		return cells
	end
}
'''

DRIVER = r'''
dofile(FWT_SCRIPT)
Base_Definitions()
print("RATES " .. tostring(ServiceRate) .. " " .. tostring(UnitServiceRate))
local units = { make_obj("REBEL_INFANTRY_SQUAD", 2600, 900, PlayerObject), make_obj("MPTL", 3100, 650, PlayerObject) }
for i, u in ipairs(units) do On_Unit_Added(u) end
local co = coroutine.create(main)
local last_s, last_u = 0, 0
local inv30 = 1 / 30
for F = 0, 3600, 3 do
	FWT.F = F
	FWT.now = F * inv30
	if FWT.now - last_s > ServiceRate then
		last_s = FWT.now
		local ok, err = coroutine.resume(co)
		if not ok then print("RESUME_ERROR " .. tostring(err)) end
	end
	if FWT.now - last_u > UnitServiceRate then
		last_u = FWT.now
		for i, u in ipairs(units) do On_Unit_Service(u) end
	end
end
for i, r in ipairs(FWR_LOG) do print(r) end
print("CALLS fss=" .. FWT.calls.fss .. " ous=" .. FWT.calls.ous .. " ous_mine=" .. FWT.calls.ous_mine)
print("REVEALS " .. table.concat(FWT.rev, " "))
print("UNDOS " .. table.concat(FWT.undo, " "))
print("SPAWNED " .. table.getn(FWT.spawned) .. " noai=" .. tostring(FWT.spawned[1] and FWT.spawned[1].noai))
'''


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        FAILS.append(what)


def find_lua():
    for p in (os.path.join(TOOLS, 'lua50.exe'), os.path.join(HERE, '..', 'a1_tactical', 'out', 'tools', 'lua50.exe')):
        if os.path.isfile(p):
            return p
    setup = os.path.join(REPO, 'toolchain', 'msvc', 'setup_x64.bat')
    src = os.path.join(REPO, 'toolchain', 'lua-5.0.2')
    if not (os.path.isfile(setup) and os.path.isfile(os.path.join(src, 'src', 'lapi.c'))):
        return None
    os.makedirs(TOOLS, exist_ok=True)
    files = ['"%s"' % os.path.join(src, 'src', n + '.c') for n in LUA_SRC] + \
        ['"%s"' % os.path.join(src, 'src', 'lib', n + '.c') for n in LUA_LIB] + \
        ['"%s"' % os.path.join(src, 'src', 'lua', 'lua.c')]
    cmd = 'call "%s" >nul && cl /nologo /Od /W0 /MT /D_CRT_SECURE_NO_WARNINGS /I"%s" /I"%s" %s /Fe"%s"' % (
        setup, os.path.join(src, 'include'), os.path.join(src, 'src'), ' '.join(files),
        os.path.join(TOOLS, 'lua50.exe'))
    bat = os.path.join(TOOLS, 'build_lua50.cmd')
    with open(bat, 'w') as f:
        f.write('@echo off\r\n' + cmd + '\r\n')
    r = subprocess.run(['cmd', '/c', bat], cwd=TOOLS, capture_output=True, text=True)
    with open(os.path.join(TOOLS, 'lua50_build.txt'), 'w') as f:
        f.write(r.stdout + r.stderr)
    for n in os.listdir(TOOLS):
        if n.endswith('.obj'):
            os.remove(os.path.join(TOOLS, n))
    p = os.path.join(TOOLS, 'lua50.exe')
    return p if os.path.isfile(p) else None


def script_text(source_zip):
    info = {'W': 187, 'H': 187}
    if source_zip and os.path.isfile(source_zip):
        data, src = build_fow_mod.freestore_text(None, '_MP_LAND_POLUS', info, source_zip)
        return data.decode('latin-1').replace('\r\n', '\n'), src
    # stand-in with the shipped structure (Base_Definitions / main loop / service functions)
    shipped = ('function Base_Definitions()\n\tServiceRate = 20\n\tUnitServiceRate = 2\nend\n'
               'function main()\n\twhile 1 do\n\t\tFreeStoreService()\n\t\tPumpEvents()\n\tend\nend\n'
               'function On_Unit_Added(object)\nend\n'
               'function FreeStoreService()\n\tFindTarget.Reachable_Target(PlayerObject)\nend\n'
               'function On_Unit_Service(object)\n\tif not object.Has_Active_Orders() then return end\nend\n')
    return shipped + '\n' + build_fow_mod.hook_text('_MP_LAND_POLUS', info), 'stand-in'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source-zip', default=os.path.join(REPO, 'extracted', 'foc_data_text.zip'))
    a = ap.parse_args()
    lua = find_lua()
    if lua is None:
        print('SELFTEST_HOOK SKIPPED (no lua50.exe and no toolchain to build it)')
        return 0
    os.makedirs(OUT, exist_ok=True)
    text, src = script_text(a.source_zip)
    print('script: shipped BusyTacticalFreeStore.lua from %s + fow_hook.lua; Lua %s' % (src, lua))
    sp = os.path.join(OUT, 'hooked_freestore.lua')
    with open(sp, 'w', newline='\n') as f:
        f.write(text)
    hp = os.path.join(OUT, 'harness.lua')
    with open(hp, 'w', newline='\n') as f:
        f.write(STUBS + '\nFWT_SCRIPT = [[%s]]\n' % sp + DRIVER)
    r = subprocess.run([lua, hp], capture_output=True, text=True)
    out = r.stdout + r.stderr
    with open(os.path.join(OUT, 'hook_run.txt'), 'w') as f:
        f.write(out)
    recs = [line for line in r.stdout.splitlines() if line.startswith('FWR1|')]
    kinds = {}
    for t in recs:
        kinds.setdefault(t.split('|')[1], []).append(t)

    def field(t, k):
        m = re.search(r'\|%s=([^|]*)' % k, t)
        return m.group(1) if m else None

    check(r.returncode == 0 and 'RESUME_ERROR' not in out and 'error' not in r.stderr.lower(),
          'runs under Lua 5.0.2 without errors (exit %d)' % r.returncode)
    check('RATES 0.05 0.05' in out, 'ServiceRate / UnitServiceRate lowered to 0.05')
    check(not kinds.get('ERR'), 'no ERR record %s' % kinds.get('ERR', ''))
    for k in ('LOAD', 'START', 'SPAWN', 'EDGEX', 'CAD', 'DONE'):
        check(k in kinds, '%s record present' % k)
    cad = field(kinds.get('CAD', ['|frames=|'])[0], 'frames') or ''
    fr = [int(x) for x in cad.split(',') if x]
    check(fr and sorted(set(b - a_ for a_, b in zip(fr, fr[1:]))) == [3], 'probe step every 3 frames (%s...)' % cad[:30])
    check(field(kinds.get('START', [''])[0], 'hid') == '0' and field(kinds.get('START', [''])[0], 'aid') == '1',
          'human id 0 found, AI id 1')
    sp_ = kinds.get('SPAWN', [''])[0]
    check(field(sp_, 'ok') == '1' and field(sp_, 'type') == 'MPTL', 'two MPTL spawned for the Rebel AI (%s)' % sp_)
    check('SPAWNED 2 noai=true' in out, 'spawned objects kept out of AI use')
    rev_r = [t for t in kinds.get('REV', []) if field(t, 'kind') == 'regrow']
    und_r = [t for t in kinds.get('UNDO', []) if field(t, 'kind') == 'regrow']
    check(len(rev_r) == 4 and len(und_r) == 4, '4 regrow reveals and undos')
    check(all(field(t, 'd1') == field(t, 'want') for t in und_r), 'each undo at the wanted decay phase (%s)' % [
        (field(t, 'd1'), field(t, 'want')) for t in und_r])
    rev_e = [t for t in kinds.get('REV', []) if field(t, 'kind') == 'edge']
    und_e = [t for t in kinds.get('UNDO', []) if field(t, 'kind') == 'edge']
    check(len(rev_e) == 10 and len(und_e) == 10, '10 edge reveals and undos')
    check(all(abs(float(field(t, 'y')) - (3740.0 - float(field(t, 'd')))) < 0.01 for t in rev_e),
          'edge y = 20 H - d (Polus 3740)')
    check(field(kinds.get('EDGEX', [''])[0], 'x') == '3179.00', 'edge x = the candidate farthest from the human (0.85 W)')
    reveals = (re.search(r'^REVEALS (.*)$', out, re.M) or [None, ''])[1].split()
    undos = (re.search(r'^UNDOS (.*)$', out, re.M) or [None, ''])[1].split()
    check(len(reveals) == 14 and len(undos) == 14, 'FogOfWar.Reveal x14 and Undo_Reveal x14 reached the engine stub')
    vis = kinds.get('VIS', [])
    check(any(field(t, 'ph') == 'post' and field(t, 'vis') == '0' and field(t, 'prevvis') == '1' for t in vis),
          'visibility flip recorded after an undo')
    calls = re.search(r'CALLS fss=(\d+) ous=(\d+) ous_mine=(\d+)', out)
    fss, ous, mine = (int(calls.group(i)) for i in (1, 2, 3)) if calls else (0, 0, 0)
    check(fss in (10, 12), 'shipped FreeStoreService ran %d times in 120 s (20 s schedule: 5-6; 2 stub calls each)' % (
        fss // 2))
    check(55 <= ous <= 61 and mine == 0, 'shipped On_Unit_Service ran %d times for the one AI unit (2 s schedule), %d '
          'times for the probe type after its spawn' % (ous, mine))
    print('SELFTEST_HOOK %s' % ('OK' if not FAILS else 'FAILED (%d)' % len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == '__main__':
    sys.exit(main())
