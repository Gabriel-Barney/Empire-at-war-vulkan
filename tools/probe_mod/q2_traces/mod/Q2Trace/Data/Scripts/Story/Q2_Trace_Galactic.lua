-- Q2 reference traces: optional galactic Lua probe (vanilla FoC, Heart of the Maw, human = Empire).
-- Attached as the plot-level Lua_Script of Story_Plots_Sandbox_12U_Empire.xml (mod override).
-- Once per fiscal cycle (45 s of galactic time) and at start it records credits and tech level per player, every
-- planet's owner, and for each AI player the galactic perception values of GALACTIC_ECONOMY.md §6.4 (Q2_Space_Total,
-- Q2_Ground_Total, Q2_Has_Space_Force, Q2_Target_Player, defined in the mod's AI_Equations_ExpansionGeneric_Galactic.xml)
-- plus the shipped Roaming_Empire_Fleet_Target as a control that needs no mod equation.
-- It only reads state: no GameRandom, no spawning, no story events.
--
-- Output channel (docs/re/LUA_SCRIPT_OUTPUT.md): retail 1.121 writes no file for _CustomScriptMessage (its writer is an
-- empty function), so every line is kept as a Lua string "Q2L1|G|<sid>|<seq>|<KIND>|<text>|END" in Q2L_Records, and
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

if Q2L_Records == nil then
	Q2L_Records = {}
	Q2L_Seq = 0
	Q2L_Sid = string.gsub(tostring(Q2L_Records), "^table: ", "")
	Q2L_Errors = {}
end

-- One record. Text is kept printable, without '|', and short enough for one scan hit (400 bytes).
function Q2L_Say(kind, text)
	if Q2L_Seq >= Q2L_MAX then
		return
	end
	Q2L_Seq = Q2L_Seq + 1
	local s = string.gsub(tostring(text), "[^ -~]", "?")
	s = string.gsub(s, "|", "/")
	s = string.sub(s, 1, 320)
	Q2L_Records[Q2L_Seq] = Q2L_NEEDLE .. Q2L_Sid .. "|" .. Q2L_Seq .. "|" .. kind .. "|" .. s .. "|END"
end

-- Each distinct error once (pcall messages carry no frame, so repeats are dropped).
function Q2L_Error(where, err)
	local key = where .. " " .. tostring(err)
	if Q2L_Errors[key] == nil then
		Q2L_Errors[key] = true
		Q2L_Say("ERR", key)
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
	q2_last_cycle = -1
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
	for _, planet in pairs(planets) do
		local pname = Q2_Name(planet)
		Q2L_Say("PLANET", "cycle=" .. cycle .. " planet=" .. pname .. " owner=" .. Q2_Owner(planet))
		for _, ai in ipairs(Q2_AI) do
			local p = Find_Player(ai)
			if p then
				local vals = ""
				for _, eq in ipairs(Q2_EQUATIONS) do
					local okv, v = pcall(EvaluatePerception, eq, p, planet)
					if okv then
						vals = vals .. " " .. eq .. "=" .. tostring(v)
					else
						vals = vals .. " " .. eq .. "=ERR"
						Q2L_Error("EvaluatePerception " .. eq, v)
					end
				end
				Q2L_Say("PERCEPTION", "cycle=" .. cycle .. " ai=" .. ai .. " planet=" .. pname .. vals)
			end
		end
	end
	Q2L_Say("CYCLEEND", head)
end

function Q2_State_Trace(message)
	if message == OnEnter then
		Q2L_Say("ENTER", "time=" .. Q2L_Str(GetCurrentTime))
	elseif message == OnUpdate then
		local t = GetCurrentTime()
		local cycle = math.floor(t / Q2_CYCLE_SECONDS)
		if cycle ~= q2_last_cycle then
			q2_last_cycle = cycle
			q2_samples = q2_samples + 1
			local ok, err = pcall(Q2_Sample, t, cycle)
			if not ok then
				Q2L_Error("Q2_Sample", err)
			end
		end
	end
end
