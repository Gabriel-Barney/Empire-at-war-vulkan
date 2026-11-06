-- ==== a2j t02 (D4) perception probe: AI story plot script (channel D) ====
-- Plot-level Lua_Script of one AI faction's own story plot file (mod copy of Story_Plots_Underworld_AI_Heroes.xml or
-- Story_Plots_Empire_AI_Heroes_EXP.xml, whose shipped Lua_Script is empty). With the generated header (A2FB_PERC: the
-- faction, its target planet and the equation lists, a2fb_spec.perc_lua) and a2fb_core.lua in front.
--
-- One script per (faction, planet) pair, on purpose: a failed engine evaluation sets the calling script's failed flag
-- and retail stops that script with no Lua error (a2i t02, README section 8). If one of these terms fails in retail,
-- only this pair's channel stops, not the story (S) channel of the budget probe or the other pair.
--
-- Every PERC_DENSE_STEP frames up to PERC_DENSE_END, then every PERC_STEP frames for as long as the run lasts, two
-- records with the same head (tag=D4 t f fac tgt own):
--   DT  the token equations A2FB_D4_* (mod equations: Markup, ConnectsIsolatedPlanetsByForce, TradeRoutes, ...)
--   DS  the shipped equations with the planet as target (Should_Conquer_Pirate_Planet, Is_Good_Invasion_Target, ...)
-- Tokens first: if one of them fails, the record still completes (the failing key is nil) and the script stops after
-- this service, so the last DT / DS record names the term. A key "skip" = needs a human player and there is none.
-- Read-only: no GameRandom, no orders, no story events; no math, '#', '%' or string.rep.
require("PGStoryMode")

A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_Percept@@SUF@@.txt"
A2FB_MAX = 4000
A2FB_D_NEXT = 0
A2FB_D_PLANET = nil
A2FB_D_FIRST = true

A2FB_Rec("LOAD", function()
	return "perception plot script fac=" .. A2FB_PERC.fac .. " tgt=" .. A2FB_PERC.planet .. " modeq=" .. tostring(A2FB_MODEQ)
end)
A2FB_Canary(A2FB_CANARY)

function Definitions()
	A2FB_Rec("DEF", function() return "Definitions ran fac=" .. A2FB_PERC.fac end)
	ServiceRate = 0.01
	StoryModeEvents = {}
	Define_State("A2FB_DState", A2FB_DState)
end

-- The first sample frame after frame f (a multiple of the step that applies after f).
function A2FB_D_After(f)
	local step = A2FB_PERC_STEP
	if f < A2FB_PERC_DENSE_END then
		step = A2FB_PERC_DENSE_STEP
	end
	local k = tonumber(string.format("%d", f / step))
	return (k + 1) * step
end

-- " KEY=value" for every entry { key, equation, needs a human } with the planet as the target.
function A2FB_JoinT(head, list, p, target)
	local s = head
	local hx = A2FB_HumanExists()
	for i = 1, table.getn(list) do
		local e = list[i]
		if e[3] and not hx then
			s = s .. " " .. e[1] .. "=skip"
		else
			local ok, v = pcall(EvaluatePerception, e[2], p, target)
			if ok then
				s = s .. " " .. e[1] .. "=" .. A2FB_Num(v)
			else
				s = s .. " " .. e[1] .. "=ERR"
			end
		end
	end
	return s
end

function A2FB_D_Sample(t, f)
	local okp, p = pcall(Find_Player, A2FB_PERC.player)
	if not okp or p == nil then
		A2FB_Error("Player", A2FB_PERC.player)
		return
	end
	if A2FB_D_PLANET == nil then
		local ok, pl = pcall(FindPlanet, A2FB_PERC.planet)
		if ok and pl ~= nil then
			A2FB_D_PLANET = pl
		else
			A2FB_Error("FindPlanet", A2FB_PERC.planet)
			return
		end
	end
	local pl = A2FB_D_PLANET
	local own = A2FB_Str(function() return string.upper(pl.Get_Owner().Get_Faction_Name()) end)
	local head = "tag=D4 t=" .. A2FB_Num(t) .. " f=" .. f .. " fac=" .. A2FB_PERC.fac .. " tgt=" .. A2FB_PERC.planet ..
		" own=" .. own
	if table.getn(A2FB_PERC.tokens) > 0 then
		A2FB_Rec("DT", function() return A2FB_JoinT(head, A2FB_PERC.tokens, p, pl) end)
	end
	A2FB_Rec("DS", function() return A2FB_JoinT(head, A2FB_PERC.shipped, p, pl) end)
end

function A2FB_D_Update()
	local t = A2FB_Now()
	if t < 0 then
		return
	end
	local f = A2FB_Frame(t)
	if A2FB_D_FIRST then
		A2FB_D_FIRST = false
		A2FB_Rec("FIRST", function()
			return "t=" .. A2FB_Num(t) .. " f=" .. f .. " fac=" .. A2FB_PERC.fac .. " humanexists=" .. tostring(A2FB_HumanExists())
		end)
	end
	if f >= A2FB_D_NEXT then
		local ok, err = pcall(A2FB_D_Sample, t, f)
		if not ok then
			A2FB_Error("Sample", err)
		end
		A2FB_D_NEXT = A2FB_D_After(f)
	end
	-- be serviced again on the next sample frame (the engine tests now - last > rate)
	local rate = (A2FB_D_NEXT - f) / 30 - 0.02
	if rate < 0.01 then
		rate = 0.01
	end
	ServiceRate = rate
end

function A2FB_DState(message)
	if message == OnEnter then
		A2FB_Rec("ENTER", function() return "t=" .. A2FB_Num(A2FB_Now()) .. " fac=" .. A2FB_PERC.fac end)
	elseif message == OnUpdate then
		local ok, err = pcall(A2FB_D_Update)
		if not ok then
			A2FB_Error("Update", err)
		end
	end
end
