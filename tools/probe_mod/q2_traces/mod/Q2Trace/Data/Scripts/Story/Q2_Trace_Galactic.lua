-- Q2 reference traces: optional galactic Lua probe (vanilla FoC, Heart of the Maw, human = Empire).
-- Attached as the plot-level Lua_Script of Story_Plots_Sandbox_12U_Empire.xml (mod override).
-- Once per fiscal cycle (45 s of galactic time) and at start it records credits and tech level per player, every
-- planet's owner, and for each AI player the galactic perception values of GALACTIC_ECONOMY.md §6.4 (Q2_Space_Total,
-- Q2_Ground_Total, Q2_Has_Space_Force, Q2_Target_Player, defined in the mod's AI_Equations_ExpansionGeneric_Galactic.xml)
-- plus the shipped Roaming_Empire_Fleet_Target as a control that needs no mod equation, then (GOALPERC, 2026-11-08) the
-- shipped goal equations of the Rebel's Shola attack for the Rebel against Shola and Naboo.
-- It only reads state: no GameRandom, no spawning, no story events.
--
-- Output channel (docs/re/LUA_SCRIPT_OUTPUT.md): retail 1.121 writes no file for _CustomScriptMessage (its writer is an
-- empty function), so every line is kept as a Lua string "Q2L1|G|<sid>|<seq>|<KIND>|<text>|END" in a table, and
-- q2lua_reader.py finds them by a read-only scan of the game's memory. The one thing _CustomScriptMessage does in
-- retail is DeleteFileA(<name>) on its first call for that name: the probe calls it once on the canary file the kit
-- installs (Mods\Q2Trace\Q2Trace_Canary_Galactic.txt), so a missing canary after the run proves the script ran.
-- The needle is built by concatenation so the script source itself never contains it.
require("PGStoryMode")

Q2L_NEEDLE = "Q2L" .. "1|G|"
Q2L_CANARY = "Mods\\Q2Trace\\Q2Trace_Canary_Galactic.txt"
Q2L_MAX = 20000
Q2_CYCLE_SECONDS = 45.0
Q2_FACTIONS = { "Empire", "Rebel", "Underworld", "Pirates" }
Q2_AI = { "Rebel", "Underworld" }
Q2_EQUATIONS = { "Q2_Space_Total", "Q2_Ground_Total", "Q2_Has_Space_Force", "Q2_Target_Player",
	"Roaming_Empire_Fleet_Target" }
-- Goal equations behind the Rebel's attack on Shola (shipped OffensiveGalaticEquations.xml; 2026-11-08): GOALPERC rows
-- per cycle for the Rebel against Shola and Naboo (control), and one row without a target for Initial_Grace_Period.
-- They run after the cycle's other rows. A failing evaluation returns nil (row value "nil") and, being a binding
-- error, ends this script after the current service pass: a "nil" in the last GOALPERC row names the culprit.
Q2_GOAL_AI = "Rebel"
Q2_GOAL_PLANETS = { "SHOLA", "NABOO" }
Q2_GOAL_EQUATIONS = { "Should_Conquer_Pirate_Planet", "Is_Good_Invasion_Target", "Need_To_Reconnect_Islands",
	"First_Attack_Allowed" }
Q2_GOAL_NOTARGET = { "Initial_Grace_Period" }

-- Records live in a table held only by these closures (q2f t08): PGBase Flush_G deletes every global table when a
-- pooled script is released, and a global records table would go with it (the AI plan probe lost its records that
-- way). Functions, strings and numbers survive. Retail has no math library: nothing here calls math.*, '#' or '%'.
if Q2L_Say == nil then
	local recs = {}
	local seq = 0
	local errs = {}
	local sid = string.gsub(tostring(recs), "^table: ", "")
	-- One record. Text is kept printable, without '|', and short enough for one scan hit (400 bytes).
	function Q2L_Say(kind, text)
		if seq >= Q2L_MAX then
			return
		end
		seq = seq + 1
		local s = string.gsub(tostring(text), "[^ -~]", "?")
		s = string.gsub(s, "|", "/")
		s = string.sub(s, 1, 320)
		recs[seq] = Q2L_NEEDLE .. sid .. "|" .. seq .. "|" .. kind .. "|" .. s .. "|END"
	end
	-- Each distinct error once (pcall messages carry no frame, so repeats are dropped).
	function Q2L_Error(where, err)
		local key = string.sub(where .. " " .. tostring(err), 1, 200)
		if errs[key] == nil then
			errs[key] = true
			Q2L_Say("ERR", key)
		end
	end
	function Q2L_Count()
		return seq
	end
	function Q2L_Get(i)
		return recs[i]
	end
end

function Q2L_Str(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return tostring(v)
	end
	return "?"
end

function Q2L_Canary()
	if Q2L_CanaryDone then
		return
	end
	Q2L_CanaryDone = true
	local ok, err = pcall(_CustomScriptMessage, Q2L_CANARY, "q2 canary\n")
	Q2L_Say("CANARY", "file=" .. Q2L_CANARY .. " ok=" .. tostring(ok) .. " err=" .. tostring(err))
end

Q2L_Say("LOAD", "galactic plot script chunk ran")
Q2L_Canary()

function Definitions()
	Q2L_Say("DEF", "Definitions ran")
	ServiceRate = 1
	StoryModeEvents = {}
	Define_State("Q2_State_Trace", Q2_State_Trace)
	q2_cycle = -1
	q2_next_t = 0
	q2_samples = 0
end

function Q2_Name(obj)
	if obj == nil then
		return "nil"
	end
	return Q2L_Str(function() return obj.Get_Type().Get_Name() end)
end

function Q2_Owner(obj)
	return Q2L_Str(function() return obj.Get_Owner().Get_Faction_Name() end)
end

function Q2_Sample(t, cycle)
	local head = "cycle=" .. cycle .. " time=" .. tostring(t)
	Q2L_Say("CYCLE", head .. " lframe=" .. Q2L_Str(function() return GetCurrentTime.Frame() end))
	for _, name in ipairs(Q2_FACTIONS) do
		local p = Find_Player(name)
		if p then
			Q2L_Say("PLAYER", "cycle=" .. cycle .. " faction=" .. name ..
				" id=" .. Q2L_Str(function() return p.Get_ID() end) ..
				" credits=" .. Q2L_Str(function() return p.Get_Credits() end) ..
				" tech=" .. Q2L_Str(function() return p.Get_Tech_Level() end))
		end
	end
	local ok, planets = pcall(function() return FindPlanet.Get_All_Planets() end)
	if not ok or planets == nil then
		Q2L_Error("Get_All_Planets", planets)
		return
	end
	-- In retail an equation that fails inside the engine returns nil (seen here as "nil") and sets the script's failed
	-- flag, which ends the script after this service pass (docs/re/LUA_API.md "What an error does to the script").
	-- The first sample names each equation in an EVAL record just before its first evaluation, so if the script stops,
	-- the last EVAL names the equation that stopped it.
	local first = (q2_samples == 1)
	local goal = {}
	for _, planet in pairs(planets) do
		local pname = Q2_Name(planet)
		goal[string.upper(pname)] = planet
		Q2L_Say("PLANET", "cycle=" .. cycle .. " planet=" .. pname .. " owner=" .. Q2_Owner(planet))
		for _, ai in ipairs(Q2_AI) do
			local p = Find_Player(ai)
			if p then
				local vals = ""
				for _, eq in ipairs(Q2_EQUATIONS) do
					if first then
						Q2L_Say("EVAL", "eq=" .. eq .. " ai=" .. ai .. " planet=" .. pname)
					end
					local okv, v = pcall(EvaluatePerception, eq, p, planet)
					if okv then
						vals = vals .. " " .. eq .. "=" .. tostring(v)
					else
						vals = vals .. " " .. eq .. "=ERR"
						Q2L_Error("EvaluatePerception " .. eq, v)
					end
				end
				Q2L_Say("PERCEPTION", "cycle=" .. cycle .. " ai=" .. ai .. " planet=" .. pname .. vals)
				first = false
			end
		end
	end
	local okg, errg = pcall(Q2_Goal_Sample, cycle, goal)
	if not okg then
		Q2L_Error("Q2_Goal_Sample", errg)
	end
	Q2L_Say("CYCLEEND", head)
end

-- One equation, pcall'd; the first sample names each equation once in an EVAL record before evaluating it (as the
-- PERCEPTION equations; named = the first planet it is evaluated for).
function Q2_Goal_Eval(eq, p, planet, pname, named)
	if named and q2_samples == 1 then
		Q2L_Say("EVAL", "eq=" .. eq .. " ai=" .. Q2_GOAL_AI .. " planet=" .. pname)
	end
	local okv, v
	if planet == nil then
		okv, v = pcall(EvaluatePerception, eq, p)
	else
		okv, v = pcall(EvaluatePerception, eq, p, planet)
	end
	if okv then
		return " " .. eq .. "=" .. tostring(v)
	end
	Q2L_Error("EvaluatePerception " .. eq, v)
	return " " .. eq .. "=ERR"
end

function Q2_Goal_Sample(cycle, goal)
	local p = Find_Player(Q2_GOAL_AI)
	if not p then
		return
	end
	for i, key in ipairs(Q2_GOAL_PLANETS) do
		local planet = goal[key]
		local vals = ""
		if planet == nil then
			vals = " missing=1"
		else
			for _, eq in ipairs(Q2_GOAL_EQUATIONS) do
				vals = vals .. Q2_Goal_Eval(eq, p, planet, key, i == 1)
			end
		end
		Q2L_Say("GOALPERC", "cycle=" .. cycle .. " ai=" .. Q2_GOAL_AI .. " planet=" .. key .. vals)
	end
	local vals = ""
	for _, eq in ipairs(Q2_GOAL_NOTARGET) do
		vals = vals .. Q2_Goal_Eval(eq, p, nil, "none", true)
	end
	Q2L_Say("GOALPERC", "cycle=" .. cycle .. " ai=" .. Q2_GOAL_AI .. " planet=none" .. vals)
end

function Q2_State_Trace(message)
	if message == OnEnter then
		Q2L_Say("ENTER", "time=" .. Q2L_Str(GetCurrentTime))
	elseif message == OnUpdate then
		local ok, t = pcall(GetCurrentTime)
		if ok and type(t) == "number" and t >= q2_next_t then
			-- cycle = floor(t / Q2_CYCLE_SECONDS) without the math library
			while t >= q2_next_t do
				q2_cycle = q2_cycle + 1
				q2_next_t = q2_next_t + Q2_CYCLE_SECONDS
			end
			q2_samples = q2_samples + 1
			local ok2, err = pcall(Q2_Sample, t, q2_cycle)
			if not ok2 then
				Q2L_Error("Q2_Sample", err)
			end
		end
	end
end
