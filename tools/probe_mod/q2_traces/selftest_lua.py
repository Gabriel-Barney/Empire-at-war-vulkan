#!/usr/bin/env python3
"""selftest_lua.py - runs the Q2 Lua probe scripts under a stock Lua 5.0.2 with stubbed engine functions (no game).

  python -I selftest_lua.py        ends with SELFTEST_LUA OK / FAILED / SKIPPED

Galactic: mod\\Q2Trace\\Data\\Scripts\\Story\\Q2_Trace_Galactic.lua is loaded, Definitions() runs, the harness plays
PGStateMachine's Process_States (OnEnter, then OnUpdate once per game second for 10 fiscal cycles). Checks: the
canary call names Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt with two strings, StoryModeEvents is a table, LOAD / DEF /
ENTER, 11 cycles with players, planets and perception rows for both AI players, a failing equation gives ERR once.
Scoring: the probe part of mod\\...\\Miscellaneous\\GameScoring.lua (after its marker) runs on stub originals; one
space mode with a thread, kills and a build. Checks the records, that the shipped handlers are still called and the
shipped GameService still runs every 10th service.
Galactic also: GOALPERC rows (Rebel goal equations vs Shola / Naboo, Initial_Grace_Period without a target, nil = failed).
Plans: the seven mod\\...\\AI\\*.lua hooks, each through two plan runs across an emulated PGBase Flush_G.
Build_Space_Forces, Build_Ground_Forces and Conquer_Pirate call the thread directly; Conquer_Opponent,
Conquer_To_Reconnect (two threads each), Crush and Lift_Blockade run as coroutines with PGBase's yielding
BlockOnCommand / Sleep / ScriptExit (PSTAGE, per-task-force records, tf=? outside a thread). Then make_plan_hooks.py --check (the files match the generator; skipped without extracted\\foc_data_text.zip).
Live memory: while lua50.exe still holds its records, q2lua_reader's parser over an a1mem scan of that process
(ReadProcessMemory) must find exactly the records the script printed.
Lua: tools\\probe_mod\\out\\tools\\lua50.exe (common\\lua50.py; built with the project MSVC at /Od on first use).
Without the toolchain the test is SKIPPED.
"""
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'common'))
from lua50 import find_lua  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'a1_tactical'))
import q2lua_reader  # noqa: E402

MOD = os.path.join(HERE, 'mod', 'Q2Trace', 'Data', 'Scripts')
OUT = os.path.join(HERE, 'out', 'selftest', 'lua')
TOOLS = os.path.join(HERE, 'out', 'tools')
FAILS = []

COMMON = r'''
-- retail Lua has no math library (s78g; no shipped script uses it): the probe must not need it
math = nil
Q2S = { t = 0, frame = 0, canary = {}, calls = {} }
GetCurrentTime = setmetatable({ Frame = function() return Q2S.frame end }, { __call = function() return Q2S.t end })
function require(name) end
function _CustomScriptMessage(a, b)
	if type(a) ~= "string" or type(b) ~= "string" then error("LuaCustomScriptMessage -- expected two string parameters.") end
	table.insert(Q2S.canary, a)
end
function Q2S_Player(name, id, credits)
	return { Get_ID = function() return id end, Get_Credits = function() return credits end,
		Get_Tech_Level = function() return 2 end, Get_Faction_Name = function() return string.upper(name) end }
end
Q2S_Players = { Empire = Q2S_Player("Empire", 1, 10000), Rebel = Q2S_Player("Rebel", 0, 8000),
	Underworld = Q2S_Player("Underworld", 6, 7000) }
function Find_Player(name) return Q2S_Players[name] end
function Q2S_Dump()
	for i = 1, Q2L_Count() do print(Q2L_Get(i)) end
	io.write("READY\n")
	io.flush()
	io.read("*l")
end
'''

GALACTIC = r'''
function Q2S_Planet(name, owner)
	return { Get_Type = function() return { Get_Name = function() return name end } end,
		Get_Owner = function() return Q2S_Players[owner] end }
end
FindPlanet = { Get_All_Planets = function()
	return { Q2S_Planet("Nal_Hutta", "Underworld"), Q2S_Planet("Coruscant", "Empire"), Q2S_Planet("Yavin", "Rebel"),
		Q2S_Planet("Shola", "Underworld"), Q2S_Planet("Naboo", "Rebel") }
end }
function EvaluatePerception(eq, player, target)
	if type(eq) ~= "string" or player == nil then error("Evaluate_Perception: Invalid parameter type") end
	if eq == "Initial_Grace_Period" then
		if target ~= nil then error("Initial_Grace_Period takes no target") end
		return 200
	end
	if target == nil then error("Variable_Target unbound in " .. eq) end
	if eq == "Q2_Target_Player" and target.Get_Type().Get_Name() == "Yavin" then error("bad equation") end
	-- a failing leaf: retail's binding returns nothing (and flags the script)
	if eq == "Need_To_Reconnect_Islands" and target.Get_Type().Get_Name() == "Naboo" then return end
	return string.len(eq) + player.Get_ID()
end
-- PGStateMachine / PGStoryMode as shipped: Base_Definitions -> Definitions, PG_Story_Mode_Init, main -> Process_States
StateMachine = {}
OnEnter = 1  OnUpdate = 2  OnExit = 3
NextState = nil
function Define_State(state, f) StateMachine[state] = f; if NextState == nil then NextState = state end end
dofile(Q2S_SCRIPT)
Definitions()
assert(type(StoryModeEvents) == "table", "StoryModeEvents must be a table for PG_Story_Mode_Init")
for k, v in pairs(StoryModeEvents) do Define_State(k, v) end
Define_State("PG_Story_State_Init", function() end)
assert(NextState == "Q2_State_Trace", "first state must be Q2_State_Trace, got " .. tostring(NextState))
local f = StateMachine[NextState]
f(OnEnter)
for s = 0, 460 do
	Q2S.t = s + 0.0333
	Q2S.frame = s * 30 + 1
	f(OnUpdate)
end
print("CANARY_CALLS " .. table.getn(Q2S.canary) .. " " .. tostring(Q2S.canary[1]))
Q2S_Dump()
'''

SCORING = r'''
Q2S.orig = { start = 0, ending = 0, kill = 0, built = 0, service = 0 }
function StringCompare(a, b) return string.upper(a) == string.upper(b) end
function Game_Mode_Starting_Event(mode, map) LastModeName = mode; Q2S.orig.start = Q2S.orig.start + 1 end
function Game_Mode_Ending_Event(mode) Q2S.orig.ending = Q2S.orig.ending + 1 end
function Tactical_Unit_Destroyed_Event(o, k) Q2S.orig.kill = Q2S.orig.kill + 1 end
function Tactical_Production_End_Event(t, p, l) Q2S.orig.built = Q2S.orig.built + 1 end
function GameService() Q2S.orig.service = Q2S.orig.service + 1 end
function Find_All_Objects_Of_Type(p) return { 1, 2, 3 } end
Q2S.threads = {}
function Create_Thread(name, param) table.insert(Q2S.threads, { name, param }) end
Q2S.sleeps = 0
function Sleep(s)
	Q2S.sleeps = Q2S.sleeps + 1
	Q2S.t = Q2S.t + s
	Q2S.frame = Q2S.frame + 30 * s
	if Q2S.sleeps == 6 then Game_Mode_Ending_Event("Space") end
end
dofile(Q2S_SCRIPT)
Definitions()
for i = 1, 20 do GameService() end
Game_Mode_Starting_Event("Space", "_SPACE_PLANET_GEONOSIS_01")
local unit = { Get_Type = function() return { Get_Name = function() return "X_WING" end } end,
	Get_Owner = function() return Q2S_Players.Rebel end }
Q2S.t = Q2S.t + 0.5
Tactical_Unit_Destroyed_Event(unit, Q2S_Players.Empire)
Tactical_Production_End_Event({ Get_Name = function() return "TIE_FIGHTER" end }, Q2S_Players.Empire, nil)
local th = Q2S.threads[1]
getfenv(0)[th[1]](th[2])
print("ORIG " .. Q2S.orig.start .. " " .. Q2S.orig.ending .. " " .. Q2S.orig.kill .. " " .. Q2S.orig.built .. " " ..
	Q2S.orig.service .. " THREADS " .. table.getn(Q2S.threads))
print("CANARY_CALLS " .. table.getn(Q2S.canary) .. " " .. tostring(Q2S.canary[1]))
Q2S_Dump()
'''

# One plan copy (whole file: the shipped part and the probe block). The chunk runs twice as a pooled script state may
# run it again: the wraps must not stack. Then two plan runs with a release between them: PGBase Flush_G (vanilla:
# every global table goes except the keep list) and Base_Definitions, then the engine attaches the next plan (task
# force, PlayerObject, Target, Budget). Records from both runs must survive (the 2026-11-03 retail run lost them, and
# the second run's thread died before the shipped code). Each thread produces one unit; Conquer_Pirate also gets a
# production failure.
PLAN = r'''
function ScriptExit() Q2S.exits = (Q2S.exits or 0) + 1 end
function BlockOnCommand(c) Q2S.t = Q2S.t + 20; Q2S.frame = Q2S.frame + 600; Q2S.blocks = (Q2S.blocks or 0) + 1 end
function Q2S_Named(n) return { Get_Name = function() return n end } end
Q2S.units = {}
Q2S.produced = 0
Q2S_Force = {
	Set_As_Goal_System_Removable = function() end,
	Produce_Force = function()
		Q2S.produced = Q2S.produced + 1
		table.insert(Q2S.units, { Get_Type = function() return Q2S_Named("CORELLIAN_GUNBOAT") end })
		return "cmd"
	end,
	Set_Plan_Result = function(r) Q2S.result = r end,
	Are_All_Units_On_Free_Store = function() return false end,
	Get_Unit_Table = function() return Q2S.units end,
	Get_Force_Count = function() return table.getn(Q2S.units) end }
function Q2S_Attach()
	ReserveForce = Q2S_Force
	MainForce = Q2S_Force
	PlayerObject = Q2S_Players.Rebel
	-- the goal's planet as a game object wrapper (AI_GALACTIC 1.9.4): Get_Type / Get_Name, no Get_Game_Object
	Target = { Get_Type = function() return Q2S_Named("Kashyyyk") end, Get_Name = function() return "Kashyyyk_Obj" end }
	Budget = { Get_Spendable_Resources = function() return 1234.5 end, Get_Unallocated_Resources = function() return 50 end }
end
-- PGBase Flush_G (vanilla): delete every global table except the keep list (the harness keeps its own Q2S* state
-- and io for the dump; retail has neither). GetCurrentTime is a table here but an engine object in retail, which
-- Flush_G keeps (the shipped BlockOnCommand / Sleep call it in reused plan scripts).
function Q2S_Flush_G()
	local keep = { _LOADED = true, coroutine = true, string = true, LuaWrapperMetaTable = true, _G = true,
		security = true, table = true, io = true, GetCurrentTime = true }
	local dead = {}
	for k, v in pairs(_G) do
		if type(v) == "table" and not keep[k] and string.sub(k, 1, 3) ~= "Q2S" then
			table.insert(dead, k)
		end
	end
	for _, k in ipairs(dead) do _G[k] = nil end
	Q2S.flushed = table.getn(dead)
end
Q2S_Attach()
dofile(Q2S_SCRIPT)
Definitions()
dofile(Q2S_SCRIPT)
Definitions()
getfenv(0)[Q2S_TF .. "_Thread"]()
Q2S_Flush_G()
Definitions()
Q2S_Attach()
getfenv(0)[Q2S_TF .. "_Thread"]()
if Q2S_TF == "MainForce" then MainForce_Production_Failed(MainForce, Q2S_Named("PIRATE_FIGHTER")) end
print("RESULT " .. tostring(Q2S.result) .. " PRODUCED " .. Q2S.produced .. " BLOCKS " .. tostring(Q2S.blocks) ..
	" EXITS " .. tostring(Q2S.exits or 0) .. " CATEGORY " .. tostring(Category) .. " FLUSHED " .. tostring(Q2S.flushed))
Q2S_Dump()
'''
PLANS = [('BuildSpaceForcesPlan.lua', 'Build_Space_Forces', 'ReserveForce'),
         ('BuildGroundForcesPlan.lua', 'Build_Ground_Forces', 'ReserveForce'),
         ('ConquerPiratePlan.lua', 'Conquer_Pirate', 'MainForce')]

# Plans whose threads block, move and invade (Conquer_Opponent: two task-force threads in one plan; Crush). Here the
# threads are real coroutines with the PGBase shapes of BlockOnCommand / Sleep / ScriptExit, resumed in creation order
# once per Planning service (AI_GALACTIC 1.9.4): a probe pcall around a yield would fail ("attempt to yield across
# metamethod/C-call boundary"), and Conquer_Opponent's two threads interleave, so each record must name its own task
# force (GetThreadID -> task force). Two plan runs across an emulated Flush_G as above; run 2 (and Crush run 1) has
# every unit on the free store, so AssembleForce stages it (PSTAGE). Then a production failure arrives outside the
# threads (its ScriptExit records tf=?).
PLAN2 = r'''
Q2S.tid = -1
Q2S.svc = 0
Q2S.exits = 0
Q2S.produced = 0
Q2S.exitflag = false
function GetThreadID() return Q2S.tid end
function _ScriptExit() Q2S.exitflag = true end
-- PGBase as shipped
function ScriptExit()
	_ScriptExit()
	Q2S.exits = Q2S.exits + 1
	if GetThreadID() >= 0 then
		coroutine.yield(false)
	end
end
function PumpEvents() coroutine.yield(true) end
function BlockOnCommand(block, max_duration, alternate_break_func)
	PumpEvents()
	if not block then return nil end
	repeat PumpEvents() until block.IsFinished() == true
	return block.Result()
end
function Sleep(time)
	local start = GetCurrentTime()
	while GetCurrentTime() - start < time do PumpEvents() end
end
function DebugMessage() end
function DifficultyBasedMinPause(d) return 2 end
function GalacticAttackAllowed(d, n) return true end
function EvaluatePerception(eq, player, target) return 1 end
-- PGTaskForce as shipped (without its DebugMessage calls)
function AssembleForce(taskforce, stage_at_strongest_space)
	if stage_at_strongest_space then stage = nil else stage = taskforce.Get_Stage() end
	if not stage then ScriptExit() end
	BlockOnCommand(taskforce.Produce_Force(stage))
end
function SynchronizedAssemble(taskforce, stage_at_strongest_space)
	if stage_at_strongest_space then stage = nil else stage = taskforce.Get_Stage() end
	if not stage then ScriptExit() end
	BlockOnCommand(taskforce.Produce_Force(stage, true))
end
function LandUnits(taskforce) BlockOnCommand(taskforce.Land_Units()) end
function LaunchUnits(taskforce)
	local block = taskforce.Launch_Units()
	if not block then return false end
	BlockOnCommand(block)
	return true
end
function Invade(taskforce)
	local s = taskforce.Invade()
	if s and BlockOnCommand(s) then
		LandUnits(taskforce)
		return true
	end
	return false
end
function FundBases(player, target) BlockOnCommand(Q2S_Block(1, true)) end
function Q2S_Named(n) return { Get_Name = function() return n end } end
function Q2S_Block(services, result)
	local at = Q2S.svc + services
	return { IsFinished = function() return Q2S.svc >= at end, Result = function() return result end }
end
-- Get_Stage gives an AI target location (Get_Game_Object -> the planet)
Q2S_Stage = { Get_Game_Object = function() return { Get_Type = function() return Q2S_Named("Mon_Calamari") end } end }
function Q2S_MakeForce(utype)
	local f = {}
	local units = {}
	f.Set_As_Goal_System_Removable = function() end
	f.Set_Plan_Result = function(r) Q2S.result = r end
	f.Are_All_Units_On_Free_Store = function() return Q2S.free end
	f.Produce_Force = function(stage)
		Q2S.produced = Q2S.produced + 1
		table.insert(units, { Get_Type = function() return Q2S_Named(utype) end })
		return Q2S_Block(3, true)
	end
	f.Get_Unit_Table = function() return units end
	f.Get_Force_Count = function() return table.getn(units) end
	f.Get_Stage = function() return Q2S_Stage end
	f.Move_To = function() return Q2S_Block(2, true) end
	f.Land_Units = function() return Q2S_Block(1, true) end
	f.Launch_Units = function() return Q2S_Block(1, true) end
	f.Invade = function() return Q2S_Block(2, true) end
	f.Release_Forces = function() end
	f.Test_Target_Contrast = function() end
	return f
end
function Q2S_Attach()
	for _, tf in ipairs(Q2S_TFS) do
		getfenv(0)[tf] = Q2S_MakeForce(Q2S_UNIT[tf])
	end
	Q2S_Players.Rebel.Get_Difficulty = function() return "Hard" end
	PlayerObject = Q2S_Players.Rebel
	Target = { Get_Type = function() return Q2S_Named("Shola") end, Get_Name = function() return "Shola_Obj" end }
	AITarget = { Get_Game_Object = function() return Target end }
	Budget = { Get_Spendable_Resources = function() return 1234.5 end, Get_Unallocated_Resources = function() return 50 end }
end
function Q2S_Flush_G()
	local keep = { _LOADED = true, coroutine = true, string = true, LuaWrapperMetaTable = true, _G = true,
		security = true, table = true, io = true, GetCurrentTime = true }
	local dead = {}
	for k, v in pairs(_G) do
		if type(v) == "table" and not keep[k] and string.sub(k, 1, 3) ~= "Q2S" then
			table.insert(dead, k)
		end
	end
	for _, k in ipairs(dead) do _G[k] = nil end
	Q2S.flushed = table.getn(dead)
end
-- One plan run: a coroutine per task force (ids 0, 1, ... again in every run), resumed in creation order once per
-- service; true keeps a thread, false or a return ends it; ScriptExit's flag ends the pass and the plan.
function Q2S_RunPlan()
	Q2S.exitflag = false
	local th = {}
	for i, tf in ipairs(Q2S_TFS) do
		th[i] = { id = i - 1, co = coroutine.create(getfenv(0)[tf .. "_Thread"]), alive = true }
	end
	for pass = 1, 2000 do
		local live = false
		for _, t in ipairs(th) do
			if t.alive and not Q2S.exitflag then
				Q2S.tid = t.id
				local ok, v = coroutine.resume(t.co)
				Q2S.tid = -1
				if not ok then error("thread " .. t.id .. ": " .. tostring(v)) end
				if v ~= true or coroutine.status(t.co) == "dead" then t.alive = false end
			end
			live = live or t.alive
		end
		if Q2S.exitflag or not live then return pass end
		Q2S.svc = Q2S.svc + 1
		Q2S.t = Q2S.t + 0.1
		Q2S.frame = Q2S.frame + 3
	end
	error("plan did not end")
end
Q2S_Attach()
dofile(Q2S_SCRIPT)
Definitions()
dofile(Q2S_SCRIPT)
Definitions()
Q2S.free = Q2S_RUN1_FREE
Q2S_RunPlan()
Q2S_Flush_G()
Definitions()
Q2S_Attach()
Q2S.free = true
Q2S_RunPlan()
local last = Q2S_TFS[table.getn(Q2S_TFS)]
getfenv(0)[last .. "_Production_Failed"](getfenv(0)[last], Q2S_Named("REBEL_INFANTRY_SQUAD"))
print("RESULT exits=" .. Q2S.exits .. " produced=" .. Q2S.produced .. " landsecured=" .. tostring(LandSecured) ..
	" category=" .. tostring(Category) .. " flushed=" .. tostring(Q2S.flushed))
Q2S_Dump()
'''
# (file, plan label, task forces, run 1 on the free store, expected kind counts, RESULT line, PSTAGE via, how run 1
# ended: PSTART prev of run 2)
PLANS2 = [('ConquerOpponentPlan.lua', 'Conquer_Opponent', ['SpaceForce', 'GroundForce'], False,
           {'LOAD': 1, 'PSTART': 4, 'PSTAGE': 2, 'PBLOCK': 11, 'PDONE': 2, 'PEXIT': 2, 'PFAIL': 1},
           'RESULT exits=2 produced=4 landsecured=true category=Conquer_Opponent flushed=', 'AssembleForce', 'PDONE'),
          ('CrushPlan.lua', 'Crush', ['MainForce'], True,
           {'LOAD': 1, 'PSTART': 2, 'PSTAGE': 1, 'PBLOCK': 5, 'PEXIT': 3, 'PFAIL': 1},
           'RESULT exits=3 produced=1 landsecured=false category=Conquer_Opponent flushed=', 'AssembleForce', 'PEXIT'),
          # both threads return (no ScriptExit): the space thread waits in Sleep until the ground force has landed
          ('ContrastToReconnect.lua', 'Conquer_To_Reconnect', ['SpaceForce', 'GroundForce'], False,
           {'LOAD': 1, 'PSTART': 4, 'PSTAGE': 2, 'PBLOCK': 7, 'PDONE': 4, 'PEXIT': 1, 'PFAIL': 1},
           'RESULT exits=1 produced=4 landsecured=true category=Conquer_To_Reconnect flushed=', 'SynchronizedAssemble',
           'PDONE'),
          ('LiftBlockadePlan.lua', 'Lift_Blockade', ['MainForce'], True,
           {'LOAD': 1, 'PSTART': 2, 'PSTAGE': 2, 'PBLOCK': 2, 'PEXIT': 3, 'PFAIL': 1},
           'RESULT exits=3 produced=2 landsecured=nil category=Lift_Blockade flushed=', 'SynchronizedAssemble', 'PEXIT')]
# skai t14: the land attack plans (LandMode\*.lua). PLAN2's coroutine driver with land stubs: AITarget is a location
# whose Get_Game_Object is the target structure; BurnUnitsLand's Find_Nearest gives a structure that dies after two
# Attack_Move blocks (TestValid false), so its while loop ends; DestroyStructure's GuardForce escorts until MainForce's
# ScriptExit ends the plan. Records must carry tgt= / near= as type#id, and near=dead once the structure is gone.
PLAN3 = r'''
Q2S.tid = -1
Q2S.svc = 0
Q2S.exits = 0
Q2S.exitflag = false
Q2S.attacks = 0
function GetThreadID() return Q2S.tid end
function _ScriptExit() Q2S.exitflag = true end
function ScriptExit()
	_ScriptExit()
	Q2S.exits = Q2S.exits + 1
	if GetThreadID() >= 0 then coroutine.yield(false) end
end
function PumpEvents() coroutine.yield(true) end
function BlockOnCommand(block, max_duration, alternate_break_func)
	PumpEvents()
	if not block then return nil end
	repeat PumpEvents() until block.IsFinished() == true
	return block.Result()
end
function Sleep(time)
	local start = GetCurrentTime()
	while GetCurrentTime() - start < time do PumpEvents() end
end
function TestValid(wrapper)
	if wrapper == nil then return false end
	if wrapper.Is_Valid == nil then return false end
	return wrapper.Is_Valid()
end
function EvaluatePerception(eq, player, target) return 0 end
function Purge_Goals() end
function Find_All_Objects_Of_Type() return {} end
function Set_Land_AI_Targeting_Priorities(tf) end
function QuickReinforce() end
function Escort(a, b) PumpEvents() end
function Q2S_Block(services, result)
	local at = Q2S.svc + services
	return { IsFinished = function() return Q2S.svc >= at end, Result = function() return result end }
end
function Q2S_Obj(tname, id, alive_fn)
	return { Get_Type = function() return { Get_Name = function() return tname end } end,
		Get_Object_ID = function() return id end, Is_Valid = alive_fn }
end
Q2S_CC = Q2S_Obj("IMPERIAL_COMMAND_CENTER", 1501, function() return Q2S.attacks < 2 end)
function Find_Nearest(tf, a, b, c) return Q2S_CC end
function Q2S_Unit(utype)
	return { Get_Type = function() return { Get_Name = function() return utype end } end }
end
function Q2S_MakeForce(utype)
	local f = {}
	local units = {}
	f.Set_As_Goal_System_Removable = function() end
	f.Set_Plan_Result = function(r) Q2S.result = r end
	f.Produce_Force = function()
		table.insert(units, Q2S_Unit(utype))
		return Q2S_Block(2, true)
	end
	f.Collect_All_Free_Units = function()
		if table.getn(units) == 0 then table.insert(units, Q2S_Unit(utype)) end
	end
	f.Activate_Ability = function(name, on) end
	f.Attack_Move = function() Q2S.attacks = Q2S.attacks + 1 return Q2S_Block(2, true) end
	f.Get_Self_Threat_Max = function() return 10 end
	f.Guard_Target = function() end
	f.Set_Targeting_Priorities = function() end
	f.Release_Forces = function() end
	f.Get_Unit_Table = function() return units end
	f.Get_Force_Count = function() return table.getn(units) end
	return f
end
function Q2S_Attach()
	for _, tf in ipairs(Q2S_TFS) do getfenv(0)[tf] = Q2S_MakeForce(Q2S_UNIT[tf]) end
	PlayerObject = Q2S_Players.Rebel
	Target = nil
	if Q2S_HAS_TARGET then
		local bar = Q2S_Obj("E_GROUND_BARRACKS", 1502, function() return true end)
		AITarget = setmetatable({ Get_Game_Object = function() return bar end },
			{ __tostring = function() return "E Ground Barracks" end })
	else
		AITarget = nil
	end
end
function Q2S_RunPlan()
	Q2S.exitflag = false
	local th = {}
	for i, tf in ipairs(Q2S_TFS) do
		th[i] = { id = i - 1, co = coroutine.create(getfenv(0)[tf .. "_Thread"]), alive = true }
	end
	for pass = 1, 2000 do
		local live = false
		for _, t in ipairs(th) do
			if t.alive and not Q2S.exitflag then
				Q2S.tid = t.id
				local ok, v = coroutine.resume(t.co)
				Q2S.tid = -1
				if not ok then error("thread " .. t.id .. ": " .. tostring(v)) end
				if v ~= true or coroutine.status(t.co) == "dead" then t.alive = false end
			end
			live = live or t.alive
		end
		if Q2S.exitflag or not live then return pass end
		Q2S.svc = Q2S.svc + 1
		Q2S.t = Q2S.t + 0.1
		Q2S.frame = Q2S.frame + 3
	end
	error("plan did not end")
end
Q2S_Attach()
dofile(Q2S_SCRIPT)
Definitions()
Q2S_RunPlan()
print("RESULT exits=" .. Q2S.exits .. " attacks=" .. Q2S.attacks .. " category=" .. tostring(Category))
Q2S_Dump()
'''
# (file, plan label, task forces, AITarget set, expected kind counts, RESULT line)
PLANS3 = [('LandMode/BurnUnitsLand.lua', 'Burn_Units_Land', ['MainForce'], False,
           {'LOAD': 1, 'PSTART': 1, 'PBLOCK': 2, 'PDONE': 1}, 'RESULT exits=0 attacks=2 category=Burn_Units_Land'),
          ('LandMode/DestroyStructure.lua', 'Destroy_Structure', ['MainForce', 'GuardForce'], True,
           {'LOAD': 1, 'PSTART': 2, 'PBLOCK': 3, 'PEXIT': 1}, 'RESULT exits=1 attacks=1 category=Destroy_Structure')]
UNITS = {'SpaceForce': 'CORELLIAN_GUNBOAT', 'GroundForce': 'REBEL_INFANTRY_SQUAD', 'MainForce': 'REBEL_INFANTRY_SQUAD'}


def check(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        FAILS.append(what)


def lua_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def run_harness(lua, name, script_path, body):
    """Runs one harness; returns (printed lines, records found in the live process, scan ok)."""
    os.makedirs(OUT, exist_ok=True)
    h = os.path.join(OUT, 'harness_%s.lua' % name)
    with open(h, 'w', encoding='ascii', newline='\n') as f:
        f.write('Q2S_SCRIPT = %s\n' % lua_str(script_path) + COMMON + body)
    p = subprocess.Popen([lua, h], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, cwd=OUT)
    lines = []
    while True:
        line = p.stdout.readline()
        if not line:
            break
        line = line.rstrip('\n')
        if line == 'READY':
            break
        lines.append(line)
    found = None
    if p.poll() is None:
        try:
            import a1mem
            proc = a1mem.Proc(p.pid, test_base=0)
            hits, _st = proc.scan([q2lua_reader.NEEDLE])
            found = {r[5] for r in q2lua_reader.parse_hits(hits[q2lua_reader.NEEDLE])}
            proc.close()
        except (OSError, ImportError) as e:
            print('  note  live scan unavailable: %s' % e)
        try:
            p.stdin.write('\n')
            p.stdin.flush()
        except OSError:  # the harness died after its last line (a Lua error): reported below
            pass
    try:
        _out, err = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        _out, err = p.communicate()
    except OSError:  # stdin already closed by a harness that died
        p.wait(timeout=30)
        err = p.stderr.read() if p.stderr else ''
    check(p.returncode == 0, '%s harness exit 0 %s' % (name, err.strip()[-300:] if err else ''))
    return lines, found


def records_of(lines):
    recs = [r for r in q2lua_reader.parse_hits([(0, ln.encode('ascii', 'replace')) for ln in lines])]
    return recs, [ln for ln in lines if ln.startswith('Q2L1|')]


def main():
    lua = find_lua()
    if not lua:
        print('SELFTEST_LUA SKIPPED (no lua50.exe and no toolchain to build it)')
        return 0
    print('lua: %s' % lua)

    gal = os.path.join(MOD, 'Story', 'Q2_Trace_Galactic.lua')
    lines, found = run_harness(lua, 'galactic', gal, GALACTIC)
    recs, raw = records_of(lines)
    check(len(recs) == len(raw) and recs, 'galactic: every printed record parses (%d)' % len(recs))
    check(all(len(r) <= 400 for r in raw), 'galactic: every record fits one scan hit')
    kinds = {}
    for r in recs:
        kinds[r[3]] = kinds.get(r[3], 0) + 1
    check(all(r[0] == 'G' for r in recs), 'galactic: src G')
    check(kinds.get('LOAD') == 1 and kinds.get('DEF') == 1 and kinds.get('ENTER') == 1, 'galactic: LOAD, DEF, ENTER')
    check(any(ln == 'CANARY_CALLS 1 Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt' for ln in lines),
          'galactic: one canary call on Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt')
    check(kinds.get('CANARY') == 1, 'galactic: CANARY record')
    check(kinds.get('CYCLE') == 11 and kinds.get('CYCLEEND') == 11, 'galactic: 11 cycles (0..10) (%s)' % kinds.get('CYCLE'))
    check(kinds.get('PLAYER') == 33, 'galactic: 3 players x 11 cycles')
    check(kinds.get('PLANET') == 55 and kinds.get('PERCEPTION') == 110, 'galactic: planets and 2 AI x 5 planets x 11')
    check(kinds.get('ERR') == 1, 'galactic: failing equation reported once')
    check(kinds.get('EVAL') == 10, 'galactic: EVAL once per equation (5 + 5 goal) in the first sample (%s)' %
          kinds.get('EVAL'))
    check(kinds.get('GOALPERC') == 33, 'galactic: GOALPERC Shola, Naboo and no-target rows x 11 (%s)' %
          kinds.get('GOALPERC'))
    grows, geqs = q2lua_reader.goal_rows([r[:5] for r in recs])
    check(geqs == ['Should_Conquer_Pirate_Planet', 'Is_Good_Invasion_Target', 'Need_To_Reconnect_Islands',
                   'First_Attack_Allowed', 'Initial_Grace_Period'], 'galactic: goal columns %s' % geqs)
    last = {d.get('planet'): d for _sid, _seq, d in grows if d.get('cycle') == '10'}
    check(last.get('SHOLA', {}).get('Should_Conquer_Pirate_Planet') == '28' and
          last.get('SHOLA', {}).get('First_Attack_Allowed') == '20' and
          last.get('NABOO', {}).get('Need_To_Reconnect_Islands') == 'nil' and
          last.get('none', {}).get('Initial_Grace_Period') == '200' and all(d.get('ai') == 'Rebel' for d in last.values()),
          'galactic: GOALPERC values (nil = failed evaluation) %s' % last)
    _u, rows, eqs, s = q2lua_reader.summarize([r[:5] for r in recs])
    check(eqs == ['Q2_Space_Total', 'Q2_Ground_Total', 'Q2_Has_Space_Force', 'Q2_Target_Player',
                  'Roaming_Empire_Fleet_Target'], 'galactic: perception columns %s' % eqs)
    yav = [d for _sid, _seq, d in rows if d.get('planet') == 'Yavin' and d.get('ai') == 'Rebel']
    check(yav and yav[0].get('Q2_Target_Player') == 'ERR' and yav[0].get('Q2_Space_Total') == '14',
          'galactic: values and ERR per equation')
    check(s['scripts']['G']['verdict'].startswith('ran: 11 cycles'), 'galactic: verdict "%s"' % s['scripts']['G']['verdict'])
    if found is not None:
        check(found == set(raw), 'galactic: live memory scan finds exactly the printed records (%d of %d)' % (
            len(found & set(raw)), len(raw)))

    sc_full = os.path.join(MOD, 'Miscellaneous', 'GameScoring.lua')
    with open(sc_full, encoding='latin-1') as f:
        text = f.read()
    mark = text.find('-- Q2 reference traces (tools/probe_mod/q2_traces): tactical Lua probe')
    check(mark > 0, 'scoring: probe marker found')
    probe = os.path.join(OUT, 'scoring_probe_part.lua')
    with open(probe, 'w', encoding='latin-1', newline='\n') as f:
        f.write(text[mark:])
    lines, found = run_harness(lua, 'scoring', probe, SCORING)
    recs, raw = records_of(lines)
    kinds = {}
    for r in recs:
        kinds[r[3]] = kinds.get(r[3], 0) + 1
    check(len(recs) == len(raw) and recs and all(r[0] == 'T' for r in recs), 'scoring: records parse, src T (%d)' % len(recs))
    check(any(ln == 'CANARY_CALLS 1 Mods\\Q2Trace\\Q2Trace_Canary_Scoring.txt' for ln in lines),
          'scoring: one canary call on Mods\\Q2Trace\\Q2Trace_Canary_Scoring.txt')
    check(kinds.get('LOAD') == 1 and kinds.get('DEF') == 1 and kinds.get('SERVICE') == 1, 'scoring: LOAD, DEF, SERVICE')
    check(kinds.get('TMODE') == 2 and kinds.get('TKILL') == 1 and kinds.get('TBUILT') == 1, 'scoring: mode, kill, build')
    check(kinds.get('THREAD') == 2, 'scoring: thread start and end')
    check(kinds.get('TSAMPLE', 0) >= 6 and kinds.get('TPLAYER') == 3 * kinds.get('TSAMPLE', 0),
          'scoring: samples with 3 players each (%s)' % kinds.get('TSAMPLE'))
    check(not kinds.get('ERR'), 'scoring: no ERR')
    check(any(ln == 'ORIG 1 1 1 1 2 THREADS 1' for ln in lines), 'scoring: shipped handlers called, GameService every 10th')
    if found is not None:
        check(found == set(raw), 'scoring: live memory scan finds exactly the printed records')

    for fname, plan, tf in PLANS:
        lines, found = run_harness(lua, 'plan_' + plan, os.path.join(MOD, 'AI', fname),
                                   'Q2S_TF = %s\n' % lua_str(tf) + PLAN)
        recs, raw = records_of(lines)
        kinds = {}
        for r in recs:
            kinds[r[3]] = kinds.get(r[3], 0) + 1
        pirate = tf == 'MainForce'
        check(len(recs) == len(raw) and recs and all(r[0] == 'P' for r in recs),
              '%s: records parse, src P (%d)' % (plan, len(recs)))
        check(kinds.get('LOAD') == 1 and kinds.get('PSTART') == 2 and kinds.get('PBLOCK') == 2 and
              kinds.get('PDONE') == 2 and kinds.get('PFAIL', 0) == (1 if pirate else 0) and kinds.get('PEXIT', 0) ==
              (1 if pirate else 0) and not kinds.get('ERR'),
              '%s: LOAD, two runs across a Flush_G (start, block, done), failures %s' % (plan, kinds))
        check(any(ln.startswith('RESULT true PRODUCED 2 BLOCKS 2 EXITS %d CATEGORY %s FLUSHED ' % (
            1 if pirate else 0, plan)) and not ln.endswith('FLUSHED 0') for ln in lines),
              '%s: shipped threads and handlers ran, Flush_G deleted tables (%s)' % (
                  plan, [ln for ln in lines if ln.startswith('RESULT')]))
        seqs = sorted(r[2] for r in recs)
        check(seqs == list(range(1, len(seqs) + 1)) and len({r[1] for r in recs}) == 1,
              '%s: one sid, no seq gaps (%s)' % (plan, seqs))
        starts = [r[4] for r in recs if r[3] == 'PSTART']
        check(len(starts) == 2 and 'target=Kashyyyk tf=%s n=1 prev=first tname=Kashyyyk_Obj ' % tf in starts[0] and
              'n=2 prev=PDONE ' in starts[1], '%s: PSTART run numbers, prev, target %s' % (plan, starts))
        block = [r[4] for r in recs if r[3] == 'PBLOCK']
        check(block and ('plan=%s player=REBEL target=Kashyyyk tf=%s n=1 k=1 ret=nil spend=1234.5 unalloc=50 cr=8000' % (
            plan, tf)) in block[0] and block[0].endswith('count=1 units=1 CORELLIAN_GUNBOAT'),
              '%s: PBLOCK text %s' % (plan, block))
        done = [r[4] for r in recs if r[3] == 'PDONE']
        check(len(done) == 2 and done[1].startswith('t=40 frame=1200 ') and
              done[1].endswith('count=2 units=2 CORELLIAN_GUNBOAT CORELLIAN_GUNBOAT'),
              '%s: PDONE after the second run %s' % (plan, done))
        if pirate:
            fail = [r[4] for r in recs if r[3] == 'PFAIL']
            check(fail and 'args=2 tf PIRATE_FIGHTER' in fail[0], '%s: PFAIL names the failed type %s' % (plan, fail))
            ex = [r[4] for r in recs if r[3] == 'PEXIT']
            check(ex and ' n=2 ' in ex[0], '%s: PEXIT from the failure handler %s' % (plan, ex))
        if found is not None:
            check(found == set(raw), '%s: live memory scan finds exactly the printed records' % plan)

    for fname, plan, tfs, free1, want, result, via, prev2 in PLANS2:
        body = ('Q2S_TFS = { %s }\nQ2S_RUN1_FREE = %s\nQ2S_UNIT = { %s }\n' % (
            ', '.join(lua_str(t) for t in tfs), 'true' if free1 else 'false',
            ', '.join('%s = %s' % (t, lua_str(UNITS[t])) for t in tfs)) + PLAN2)
        lines, found = run_harness(lua, 'plan_' + plan, os.path.join(MOD, 'AI', fname), body)
        recs, raw = records_of(lines)
        kinds = {}
        for r in recs:
            kinds[r[3]] = kinds.get(r[3], 0) + 1
        check(len(recs) == len(raw) and recs and all(r[0] == 'P' for r in recs),
              '%s: records parse, src P (%d)' % (plan, len(recs)))
        check(kinds == want, '%s: two threaded runs across a Flush_G, then a failure %s' % (plan, kinds))
        check(any(ln.startswith(result) and not ln.endswith('flushed=0') for ln in lines),
              '%s: shipped threads ran to their ends (%s)' % (plan, [ln for ln in lines if ln.startswith('RESULT')]))
        seqs = sorted(r[2] for r in recs)
        check(seqs == list(range(1, len(seqs) + 1)) and len({r[1] for r in recs}) == 1,
              '%s: one sid, no seq gaps' % plan)
        d = [(r[3], q2lua_reader.kv(r[4])) for r in recs]
        check(d[0][1].get('tfs') == ','.join(tfs), '%s: LOAD names the task forces %s' % (plan, recs[0][4]))
        starts = [x for k, x in d if k == 'PSTART']
        check([(x.get('tf'), x.get('n'), x.get('prev')) for x in starts] ==
              [(t, '1', 'first') for t in tfs] + [(t, '2', prev2) for t in tfs] and
              all(x.get('target') == 'Shola' and x.get('tname') == 'Shola_Obj' for x in starts),
              '%s: PSTART per task force, run numbers, prev, target %s' % (plan, [r[4] for r in recs if r[3] == 'PSTART']))
        stages = [x for k, x in d if k == 'PSTAGE']
        check({x.get('tf') for x in stages} == set(tfs) and
              all(x.get('stage') == 'Mon_Calamari' and x.get('via') == via and x.get('strongest') == 'nil'
                  and x.get('count') == '0' and x.get('stagestr', '').startswith('table:') for x in stages),
              '%s: PSTAGE per task force before Produce_Force %s' % (plan, [r[4] for r in recs if r[3] == 'PSTAGE']))
        blocks = [x for k, x in d if k == 'PBLOCK']
        check(blocks and all(x.get('tf') in tfs and x.get('count', '').isdigit() and int(x['count']) >= 1 and
                             (' %s' % UNITS[x['tf']]) in r[4] for (k, x), r in zip(d, recs) if k == 'PBLOCK'),
              '%s: every PBLOCK names its own task force, count and unit type' % plan)
        if len(tfs) > 1:
            first = [(x.get('tf'), x.get('k')) for x in blocks[:2]]
            check(first == [('SpaceForce', '1'), ('GroundForce', '1')],
                  '%s: interleaved threads keep their task force and k %s' % (plan, first))
        fail = [r[4] for r in recs if r[3] == 'PFAIL']
        check(fail and ' tf=%s ' % tfs[-1] in fail[0] and 'args=2 tf REBEL_INFANTRY_SQUAD' in fail[0],
              '%s: PFAIL %s' % (plan, fail))
        ex = [x for k, x in d if k == 'PEXIT']
        check(ex and ex[-1].get('tf') == '?' and ex[-1].get('units') == '?' and
              ex[-1].get('count') == ','.join('%s:1' % t for t in tfs) if len(tfs) > 1 else ex[-1].get('tf') == tfs[0],
              '%s: PEXIT outside the threads %s' % (plan, [r[4] for r in recs if r[3] == 'PEXIT'][-1:]))
        check(all(len(r) <= 400 for r in raw), '%s: every record fits one scan hit' % plan)
        if found is not None:
            check(found == set(raw), '%s: live memory scan finds exactly the printed records' % plan)

    for fname, plan, tfs, has_target, want, result in PLANS3:
        body = ('Q2S_TFS = { %s }\nQ2S_HAS_TARGET = %s\nQ2S_UNIT = { %s }\n' % (
            ', '.join(lua_str(t) for t in tfs), 'true' if has_target else 'false',
            ', '.join('%s = %s' % (t, lua_str('REBEL_INFANTRY_SQUAD')) for t in tfs)) + PLAN3)
        lines, found = run_harness(lua, 'plan_' + plan, os.path.join(MOD, 'AI', *fname.split('/')), body)
        recs, raw = records_of(lines)
        kinds = {}
        for r in recs:
            kinds[r[3]] = kinds.get(r[3], 0) + 1
        check(len(recs) == len(raw) and recs and all(r[0] == 'P' for r in recs),
              '%s: records parse, src P (%d)' % (plan, len(recs)))
        check(kinds == want, '%s: one land plan run %s' % (plan, kinds))
        check(any(ln.startswith(result) for ln in lines),
              '%s: shipped threads ran to their ends (%s)' % (plan, [ln for ln in lines if ln.startswith('RESULT')]))
        d = [(r[3], q2lua_reader.kv(r[4])) for r in recs]
        blocks = [x for k, x in d if k == 'PBLOCK']
        if has_target:
            check(all(x.get('target') == 'E_Ground_Barracks' and x.get('tgt') == 'E_GROUND_BARRACKS#1502'
                      for k, x in d if k != 'LOAD'), '%s: target / tgt on every record %s' % (plan, recs[1][4]))
            check(any(x.get('tf') == 'MainForce' and x.get('count') == '1' for x in blocks),
                  '%s: MainForce PBLOCK after Produce_Force %s' % (plan, [r[4] for r in recs if r[3] == 'PBLOCK']))
        else:
            check([x.get('near') for x in blocks] == ['IMPERIAL_COMMAND_CENTER#1501', 'dead'] and
                  all(x.get('target') == 'none' and 'tgt' not in x for x in blocks),
                  '%s: near= the Find_Nearest structure, then dead %s' % (plan, [r[4] for r in recs if r[3] == 'PBLOCK']))
        check(all(len(r) <= 400 for r in raw), '%s: every record fits one scan hit' % plan)
        if found is not None:
            check(found == set(raw), '%s: live memory scan finds exactly the printed records' % plan)

    hooks = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'make_plan_hooks.py'), '--check'],
                           capture_output=True, text=True)
    print('  note  ' + hooks.stdout.strip())
    check(hooks.returncode in (0, 2), 'plan hooks match make_plan_hooks.py output (or the zip is absent)')

    print('SELFTEST_LUA ' + ('FAILED (%d)' % len(FAILS) if FAILS else 'OK'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
