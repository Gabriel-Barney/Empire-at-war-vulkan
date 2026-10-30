-- Q2 reference traces: optional galactic Lua probe (vanilla FoC, Heart of the Maw, human = Empire).
-- Attached as the plot-level Lua_Script of Story_Plots_Sandbox_12U_Empire.xml (mod override).
-- Once per fiscal cycle (45 s of galactic time) and at start it writes, through _CustomScriptMessage:
--   credits and tech level per player, every planet's owner, and for each AI player the three galactic
--   perception values of GALACTIC_ECONOMY.md §6.4 (Q2_Space_Total, Q2_Has_Space_Force, Q2_Target_Player,
--   plus Q2_Ground_Total), defined in the mod's AI_Equations_ExpansionGeneric_Galactic.xml.
-- It only reads state: no GameRandom, no spawning, no story events.
require("PGStateMachine")

Q2_FILE = "Q2Trace_Lua.txt"
Q2_CYCLE_SECONDS = 45.0
Q2_FACTIONS = { "Empire", "Rebel", "Underworld", "Pirates" }
Q2_AI = { "Rebel", "Underworld" }
Q2_EQUATIONS = { "Q2_Space_Total", "Q2_Ground_Total", "Q2_Has_Space_Force", "Q2_Target_Player" }

function Definitions()
	ServiceRate = 1
	Define_State("Q2_State_Trace", Q2_State_Trace)
	NextState = "Q2_State_Trace"
	q2_last_cycle = -1
	q2_samples = 0
end

function Q2_Log(line)
	_CustomScriptMessage(Q2_FILE, line .. "\n")
end

function Q2_Name(obj)
	if obj == nil then
		return "nil"
	end
	local ok, n = pcall(function() return obj.Get_Type().Get_Name() end)
	if ok and n then
		return tostring(n)
	end
	return "?"
end

function Q2_Owner(obj)
	local ok, p = pcall(function() return obj.Get_Owner() end)
	if ok and p then
		local ok2, f = pcall(function() return p.Get_Faction_Name() end)
		if ok2 and f then
			return tostring(f)
		end
	end
	return "none"
end

function Q2_Sample(t, cycle)
	Q2_Log(string.format("CYCLE %d time %.3f", cycle, t))
	for _, name in ipairs(Q2_FACTIONS) do
		local p = Find_Player(name)
		if p then
			local ok, credits = pcall(function() return p.Get_Credits() end)
			local ok2, tech = pcall(function() return p.Get_Tech_Level() end)
			Q2_Log(string.format("PLAYER %s credits %s tech %s", name, tostring(ok and credits), tostring(ok2 and tech)))
		end
	end
	local planets = FindPlanet.Get_All_Planets()
	if planets == nil then
		return
	end
	for _, planet in pairs(planets) do
		local pname = Q2_Name(planet)
		Q2_Log(string.format("PLANET %s owner %s", pname, Q2_Owner(planet)))
		for _, ai in ipairs(Q2_AI) do
			local p = Find_Player(ai)
			if p then
				local vals = ""
				for _, eq in ipairs(Q2_EQUATIONS) do
					local ok, v = pcall(EvaluatePerception, eq, p, planet)
					vals = vals .. " " .. eq .. "=" .. tostring(ok and v)
				end
				Q2_Log(string.format("PERCEPTION %s %s%s", ai, pname, vals))
			end
		end
	end
end

function Q2_State_Trace(message)
	if message == OnEnter then
		Q2_Log("Q2LUA 1 start time " .. tostring(GetCurrentTime()))
	elseif message == OnUpdate then
		local t = GetCurrentTime()
		local cycle = math.floor(t / Q2_CYCLE_SECONDS)
		if cycle ~= q2_last_cycle then
			q2_last_cycle = cycle
			q2_samples = q2_samples + 1
			Q2_Sample(t, cycle)
		end
	end
end
