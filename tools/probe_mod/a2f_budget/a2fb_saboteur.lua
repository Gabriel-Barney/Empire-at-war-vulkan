-- ==== a2k t05 saboteur probe: Underworld AI story plot script (channel U) ====
-- Built only with build_a2f_mod.py --saboteur (run_a2f_sab.cmd): the Lua_Script of the mod copy of
-- Story_Plots_Underworld_AI_Heroes.xml (the slot channel D's A2fb_PerceptU uses in the other runs; an AI plot's
-- Lua_Script ran in retail run 20261106-074145). With the generated header (A2FB_SABCFG, build_a2f_mod.sab_lua) and
-- a2fb_core.lua in front. Questions: docs/re/A2K_T08_RE_ANSWERS.md section 5 (Q5 items 4 and 5, H1 / H2 of Q4.2).
--
-- Every A2FB_SABCFG.step frames in frames 0 .. A2FB_SABCFG.stop (one sample on the first service at or after each
-- multiple of the step; the record carries the frame it ran in):
--   UN  head: n= saboteurs the Underworld owns, other= saboteurs of other owners, all= objects found
--   US  per Underworld saboteur (Find_All_Objects_Of_Type(<type>), owner checked here): id= Get_Object_ID,
--       type=, pl= Get_Planet_Location, par= parent type (Get_Parent_Object; a fleet is GALACTIC_FLEET), parid=,
--       cnt= the parent's Get_Contained_Object_Count, parpl= the parent's Get_Planet_Location (nil = in transit:
--       a fleet between planets has no planet), ok= Is_Valid; when the parent has a parent itself (the type filter
--       matches a company and the transport that carries it: Find_All_Objects_Of_Type uses the representative type,
--       docs/re/LUA_API.md), gp= / gpid= / gpcnt= / gppl= the same for the grandparent (then the fleet)
-- Every A2FB_SABCFG.step frames in frames 0 .. A2FB_SABCFG.des_stop (Q5 item 5, H1), per planet of A2FB_SABCFG.planets:
--   UD  tgt=, own= owner faction, cor= Is_Corrupted, CP= EvaluatePerception("Should_Corrupt_Planet", Underworld,
--       planet), SP= the same for "Should_Sabotage_Planet" (the desire functions of the Corrupt_Planet and
--       Sabotage_Planet goals, AI_GOALSET_UNDERWORLD_GALACTIC.XML). Evaluated only for planets the Underworld does not
--       own (both goals apply to Enemy targets), so only (goal, target) pairs the goal system itself scores; own=
--       UNDERWORLD gives CP=own SP=own. Neither closure reads Variable_Human or uses '#' (selftest).
-- What Lua cannot see: the proposals themselves and whether each was accepted (no goal-system API), the effective
-- desire (learning / Per_Failure_Desire_Adjust, incumbent bonus), and whether a fleet is hidden (no binding reads
-- it). The DeploySaboteur PSTART records (channel P) show which proposal was accepted: the plan that started.
-- Is_Object_On_Free_Store / Is_Unit_In_Transit are free-store methods: channel F records them (8 s, a2fb_freestore_hook).
-- Read-only: no GameRandom, no orders, no story events; no math, '#', '%' or string.rep. A binding that fails ends this
-- script only (its own plot script), never the story (S) or the AI's own scripts; the last sample then shows nil.
require("PGStoryMode")

A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_Saboteur.txt"
A2FB_MAX = 6000
A2FB_U_NEXT = 0
A2FB_U_FIRST = true
A2FB_U_DONE = false
A2FB_U_PL = {}

A2FB_Rec("LOAD", function()
	return "saboteur plot script type=" .. A2FB_SABCFG.type .. " step=" .. A2FB_SABCFG.step .. " stop=" ..
		A2FB_SABCFG.stop .. " des_stop=" .. A2FB_SABCFG.des_stop .. " planets=" .. table.getn(A2FB_SABCFG.planets) ..
		" modeq=" .. tostring(A2FB_MODEQ)
end)
A2FB_Canary(A2FB_CANARY)

function Definitions()
	A2FB_Rec("DEF", function() return "Definitions ran" end)
	ServiceRate = 0.01
	StoryModeEvents = {}
	Define_State("A2FB_UState", A2FB_UState)
end

function A2FB_U_Name(o)
	if o == nil then
		return "nil"
	end
	return A2FB_Str(function() return string.upper(o.Get_Type().Get_Name()) end)
end

function A2FB_U_Id(o)
	if o == nil then
		return "nil"
	end
	return A2FB_Str(function() return o.Get_Object_ID() end)
end

function A2FB_U_Owner(o)
	return A2FB_Str(function() return string.upper(o.Get_Owner().Get_Faction_Name()) end)
end

function A2FB_U_Saboteurs(t, f)
	local ok, all = pcall(Find_All_Objects_Of_Type, A2FB_SABCFG.type)
	if not ok or type(all) ~= "table" then
		A2FB_Rec("UN", function() return "t=" .. A2FB_Num(t) .. " f=" .. f .. " n=nil find=" .. tostring(all) end)
		return
	end
	local mine = {}
	local other = 0
	local total = 0
	for _, o in pairs(all) do
		total = total + 1
		if A2FB_U_Owner(o) == "UNDERWORLD" then
			table.insert(mine, o)
		else
			other = other + 1
		end
	end
	A2FB_Rec("UN", function()
		return "t=" .. A2FB_Num(t) .. " f=" .. f .. " n=" .. table.getn(mine) .. " other=" .. other .. " all=" .. total
	end)
	for i = 1, table.getn(mine) do
		local o = mine[i]
		A2FB_Rec("US", function()
			local par = nil
			local pl = nil
			pcall(function() pl = o.Get_Planet_Location() end)
			pcall(function() par = o.Get_Parent_Object() end)
			local s = "t=" .. A2FB_Num(t) .. " f=" .. f .. " i=" .. i .. " id=" .. A2FB_U_Id(o) .. " type=" .. A2FB_U_Name(o) ..
				" ok=" .. A2FB_Str(function() return o.Is_Valid() end) .. " pl=" .. A2FB_U_Name(pl)
			if par == nil then
				return s .. " par=nil"
			end
			local parpl = nil
			local gp = nil
			pcall(function() parpl = par.Get_Planet_Location() end)
			pcall(function() gp = par.Get_Parent_Object() end)
			s = s .. " par=" .. A2FB_U_Name(par) .. " parid=" .. A2FB_U_Id(par) ..
				" cnt=" .. A2FB_Str(function() return par.Get_Contained_Object_Count() end) ..
				" parpl=" .. A2FB_U_Name(parpl)
			if gp == nil then
				return s
			end
			local gppl = nil
			pcall(function() gppl = gp.Get_Planet_Location() end)
			return s .. " gp=" .. A2FB_U_Name(gp) .. " gpid=" .. A2FB_U_Id(gp) ..
				" gpcnt=" .. A2FB_Str(function() return gp.Get_Contained_Object_Count() end) .. " gppl=" .. A2FB_U_Name(gppl)
		end)
	end
end

function A2FB_U_Planet(name)
	local pl = A2FB_U_PL[name]
	if pl == nil then
		local ok, p = pcall(FindPlanet, name)
		if ok and p ~= nil then
			A2FB_U_PL[name] = p
			pl = p
		end
	end
	return pl
end

function A2FB_U_Desire(t, f)
	local okp, uw = pcall(Find_Player, "Underworld")
	if not okp or uw == nil then
		A2FB_Error("Player", "Underworld")
		return
	end
	for i = 1, table.getn(A2FB_SABCFG.planets) do
		local name = A2FB_SABCFG.planets[i]
		local pl = A2FB_U_Planet(name)
		if pl == nil then
			A2FB_Error("FindPlanet", name)
		else
			A2FB_Rec("UD", function()
				local own = A2FB_U_Owner(pl)
				local s = "t=" .. A2FB_Num(t) .. " f=" .. f .. " tgt=" .. string.upper(name) .. " own=" .. own ..
					" cor=" .. A2FB_Str(function() return pl.Is_Corrupted() end)
				if own == "UNDERWORLD" or own == "ERR" or own == "nil" then
					return s .. " CP=own SP=own"
				end
				return s .. " CP=" .. A2FB_Eval3("Should_Corrupt_Planet", uw, pl) ..
					" SP=" .. A2FB_Eval3("Should_Sabotage_Planet", uw, pl)
			end)
		end
	end
end

function A2FB_Eval3(name, p, target)
	local ok, v = pcall(EvaluatePerception, name, p, target)
	if not ok then
		return "ERR"
	end
	return A2FB_Num(v)
end

function A2FB_U_After(f)
	local step = A2FB_SABCFG.step
	local k = tonumber(string.format("%d", f / step))
	return (k + 1) * step
end

function A2FB_U_Update()
	local t = A2FB_Now()
	if t < 0 then
		return
	end
	local f = A2FB_Frame(t)
	if A2FB_U_FIRST then
		A2FB_U_FIRST = false
		A2FB_Rec("FIRST", function() return "t=" .. A2FB_Num(t) .. " f=" .. f .. " humanexists=" .. tostring(A2FB_HumanExists()) end)
	end
	if f > A2FB_SABCFG.stop then
		if not A2FB_U_DONE then
			A2FB_U_DONE = true
			A2FB_Rec("UEND", function() return "t=" .. A2FB_Num(t) .. " f=" .. f .. " records=" .. A2FB_Seq end)
		end
		ServiceRate = 30
		return
	end
	if f >= A2FB_U_NEXT then
		local ok, err = pcall(A2FB_U_Saboteurs, t, f)
		if not ok then
			A2FB_Error("Saboteurs", err)
		end
		if f <= A2FB_SABCFG.des_stop then
			local ok2, err2 = pcall(A2FB_U_Desire, t, f)
			if not ok2 then
				A2FB_Error("Desire", err2)
			end
		end
		A2FB_U_NEXT = A2FB_U_After(f)
	end
	-- be serviced again on the next sample frame (the engine tests now - last > rate)
	local rate = (A2FB_U_NEXT - f) / 30 - 0.02
	if rate < 0.01 then
		rate = 0.01
	end
	ServiceRate = rate
end

function A2FB_UState(message)
	if message == OnEnter then
		A2FB_Rec("ENTER", function() return "t=" .. A2FB_Num(A2FB_Now()) end)
	elseif message == OnUpdate then
		local ok, err = pcall(A2FB_U_Update)
		if not ok then
			A2FB_Error("Update", err)
		end
	end
end
