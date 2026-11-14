#!/usr/bin/env python3
"""selftest_a1h.py - offline check of the a1h t09 probe: runs a1h_story.lua under a stock Lua 5.0.2 interpreter
with the game's own script library (Data\\Scripts\\Library from extracted/foc_data_text.zip: PGStoryMode, PGBase,
PGSpawnUnits, ...) and stubbed engine functions, then builds report.txt from the records it produced.

  selftest_a1h.py --lua <lua50.exe>   (tools\\probe_mod\\common\\build_lua50.cmd builds one)

Two engine models: "launch" (Assemble_Fleet puts every unit into one new fleet in orbit, the port's rule) must give
LAUNCHED for every case; "keep" (Assemble_Fleet takes only units already in orbit, i.e. the ship of case D) must give
NOFLEET for A / B / C and PARTIAL for D. Checks the state machine, the spawns, the records and the report verdicts.
"""
import argparse
import os
import subprocess
import sys
import zipfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import a1h_run  # noqa: E402
import build_a1h_mod  # noqa: E402

STUBS = r'''
A1H_OUT = io.open(A1H_LOG, "w")
function A1H_W(s) A1H_OUT:write(s .. "\n") end
Script = "A1h_Story"
A1H_T = 0
function GetCurrentTime() return A1H_T end
A1H_TV = {}
local function tv()
	local co = A1H_CUR or "main"
	if not A1H_TV[co] then A1H_TV[co] = {} end
	return A1H_TV[co]
end
ThreadValue = setmetatable({ Set = function(k, v) tv()[k] = v end, Reset = function() end },
	{ __call = function(_, k) return tv()[k] end })
GetEvent = setmetatable({ Reset = function() end, Params = function() return nil end },
	{ __call = function() return nil end })
function _ScriptMessage(s) end
function _OuputDebug(s) end
function _MessagePopup(s) A1H_W("POPUP " .. s) end
function Game_Message(s) A1H_W("GAME_MESSAGE " .. s) end
function DumpCallStack() return debug.traceback() end
function ScriptExit() A1H_W("SCRIPT_EXIT") error("ScriptExit") end

local function obj(type_name, fields)
	local o = fields or {}
	o.Is_Valid = function() return true end
	o.Get_Type = function() return { Get_Name = function() return type_name end } end
	o.Get_Contained_Object_Count = function() return o.contents and table.getn(o.contents) or 0 end
	o.Get_Parent_Object = function() return o.parent end
	o.Get_Planet_Location = function() return o.planet end
	o.Get_Owner = function() return o.owner end
	return o
end
A1H_PLAYERS = {}
for _, n in ipairs({ "Rebel", "Empire", "Underworld", "Pirates", "Neutral" }) do A1H_PLAYERS[n] = { name = n } end
function Find_Player(n) return A1H_PLAYERS[n] end
A1H_PLANETS = {}
for _, n in ipairs({ "Tatooine", "Byss", "Abregado_Rae", "Jabiim" }) do A1H_PLANETS[n] = obj(n, {}) end
function FindPlanet(n) return A1H_PLANETS[n] end
function Find_Object_Type(t) return { name = t } end
A1H_ALL = {}
local function unit(type_name, planet, player)
	local space = type_name == "Tartan_Patrol_Cruiser"
	local u = obj(type_name, { owner = player, planet = planet, space = space })
	u.parent = planet
	if space then  -- a ship spawned at a planet sits in an orbit fleet of its own
		local f = obj("Galactic_Fleet", { owner = player, planet = planet, contents = { u } })
		f.parent = planet
		u.parent = f
	end
	u.Prevent_AI_Usage = function(b) end
	u.Mark_Parent_Mode_Object_For_Death = function() end
	table.insert(A1H_ALL, u)
	return u
end
function Spawn_Unit(t, where, player)
	A1H_W("SPAWN " .. t.name .. " at=" .. where.Get_Type().Get_Name() .. " player=" .. player.name)
	return { unit(t.name, where, player) }
end
-- Starting_Forces of the probe campaign: two stormtrooper squads on Tatooine's surface (Empire).
unit("Imperial_Stormtrooper_Squad", A1H_PLANETS.Tatooine, A1H_PLAYERS.Empire)
unit("Imperial_Stormtrooper_Squad", A1H_PLANETS.Tatooine, A1H_PLAYERS.Empire)
function Find_All_Objects_Of_Type(name)
	local out = {}
	for _, u in ipairs(A1H_ALL) do if u.Get_Type().Get_Name() == name then table.insert(out, u) end end
	return out
end
function Assemble_Fleet(units)
	local take = {}
	for _, u in pairs(units) do
		if A1H_MODEL == "launch" or u.space then table.insert(take, u) end
	end
	A1H_W("ASSEMBLE " .. table.getn(take))
	if table.getn(take) == 0 then return nil end
	local f = obj("Galactic_Fleet", { owner = take[1].owner, planet = take[1].planet, contents = take })
	f.parent = take[1].planet
	for _, u in ipairs(take) do u.parent = f end
	return f
end
A1H_THREADS = {}
function Create_Thread(name, param)
	A1H_W("CREATE_THREAD " .. name)
	table.insert(A1H_THREADS, coroutine.create(function() _G[name](param) end))
end
'''

DRIVER = r'''
Base_Definitions()
local main_co = coroutine.create(main)
local triggered = false
for step = 0, 600 do
	A1H_T = step * 0.2
	if A1H_T >= 3 and not triggered then
		triggered = true
		Story_Event_Trigger("A1H_Begin")
	end
	A1H_CUR = main_co
	local ok, err = coroutine.resume(main_co)
	if not ok then A1H_W("MAIN_ERROR " .. tostring(err)) break end
	for _, co in ipairs(A1H_THREADS) do
		if coroutine.status(co) == "suspended" then
			A1H_CUR = co
			local tok, terr = coroutine.resume(co)
			if not tok then A1H_W("THREAD_ERROR " .. tostring(terr)) end
		end
	end
end
A1H_W("STATE " .. tostring(CurrentState))
A1H_OUT:close()
'''

EXPECT = {'launch': {'C': 'LAUNCHED', 'A': 'LAUNCHED', 'B': 'LAUNCHED', 'D': 'LAUNCHED'},
          'keep': {'C': 'NOFLEET', 'A': 'NOFLEET', 'B': 'NOFLEET', 'D': 'PARTIAL'}}


def library(dst):
    z = zipfile.ZipFile(os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    n = 0
    for info in z.infolist():
        p = info.filename.replace('\\', '/').upper()
        if '/SCRIPTS/LIBRARY/' in p and p.endswith('.LUA'):
            with open(os.path.join(dst, os.path.basename(info.filename)), 'wb') as f:
                f.write(z.read(info))
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lua', required=True)
    a = ap.parse_args()
    out_dir = os.path.join(HERE, 'out', 'selftest')
    lib = os.path.join(out_dir, 'library')
    os.makedirs(lib, exist_ok=True)
    ok_all = library(lib) > 0
    for model, expect in EXPECT.items():
        log = os.path.join(out_dir, 'story_%s.txt' % model)
        lua = os.path.join(out_dir, 'story_test_%s.lua' % model)
        with open(lua, 'w', encoding='ascii') as f:
            f.write('A1H_LOG = "%s"\nA1H_MODEL = "%s"\n' % (log.replace('\\', '\\\\'), model))
            f.write(STUBS)
            f.write(build_a1h_mod.story_lua())
            f.write(DRIVER)
        env = dict(os.environ, LUA_PATH=os.path.join(lib, '?.lua'))
        r = subprocess.run([a.lua, lua], capture_output=True, text=True, env=env)
        sys.stdout.write(r.stdout + r.stderr)
        text = open(log).read() if os.path.exists(log) else ''
        # The records as the reader would log them (the popups carry the same strings).
        records = os.path.join(out_dir, 'records_%s.log' % model)
        with open(records, 'w') as f:
            for ln in text.splitlines():
                if ln.startswith('POPUP A1HR|'):
                    f.write(ln[len('POPUP '):] + '\n')
        verdicts = a1h_run.report(records, os.path.join(out_dir, 'report_%s.txt' % model))
        spawns = [ln for ln in text.splitlines() if ln.startswith('SPAWN ')]
        checks = {
            'lua exit 0': r.returncode == 0,
            'no errors': 'ERROR' not in text and 'SCRIPT_EXIT' not in text,
            'state A1H_Begin': 'STATE A1H_Begin' in text,
            'one thread': text.count('CREATE_THREAD A1H_Run') == 1,
            'six spawns': len(spawns) == 6,
            'B spawns for Underworld at Byss': sum(1 for s in spawns if 'at=Byss player=Underworld' in s) == 2,
            'game messages': 'GAME_MESSAGE TEXT_A1H_STORY_LUA' in text and 'GAME_MESSAGE TEXT_A1H_DONE' in text,
            'verdicts': verdicts == expect,
            'texts exist': all(('TEXT_A1H_%s_%s' % (c, v)) in dict(build_a1h_mod.TEXTS) for c, v in expect.items()),
        }
        bad = [k for k, v in checks.items() if not v]
        print('model %s: verdicts %s -> %s' % (model, verdicts, 'OK' if not bad else 'FAILED ' + ', '.join(bad)))
        ok_all = ok_all and not bad
    print('A1H SELFTEST OK' if ok_all else 'A1H SELFTEST FAILED')
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())
