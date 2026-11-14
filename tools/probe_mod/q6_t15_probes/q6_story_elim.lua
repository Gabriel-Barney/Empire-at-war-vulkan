-- ==== Q6 t15 probe: elimination of a player that owns nothing (tools/probe_mod/q6_t15_probes/q6_story_elim.lua) ====
-- Galactic plot script of the campaigns "Q6 PROBE: ELIM A / B / C" (human Empire, nothing owned at start), installed by
-- build_q6_mod.py as Data\Scripts\Story\Q6_Story_Elim_<A|B|C>.lua with Q6E_MODE replaced:
--   A NOTHING  the human owns nothing all game; victory conditions Galactic_All_Planets_Controlled only (as TR)
--   B LOSE     as A, but this script gives the human Abregado Rae at t+20 s (Change_Owner) and takes it back to the
--              Neutral player at t+50 s: the human owned something once and then owns nothing again
--   C CONTROL  as A, but the AI side's condition is Galactic_Opponent_Controls_No_Planets (control: another player's
--              condition passes at once)
--   D AI_GAIN  as A, but this script gives the neutral Abregado Rae to the Rebel at t+20 s: an owner change while the
--              human owns nothing (q6 t25: retail is expected to end it about 1 s later through the AI's implicit
--              Opponent_Controls_No_Planets, docs/re/Q6_T15_RETAIL_RUN.md "ELIM C")
--   E FLEET    the human starts with a Star_Destroyer at Tatooine (a starting fleet does not give the planet: run
--              20261109-082854), this script gives Tatooine to the human at t+10 s and makes it neutral again at t+40 s:
--              does a player with units but no planet lose? (q6 t25: expected yes, units do not count)
-- After the D / E event the heartbeat runs every second for 15 s, so the end time is known to about 1 s.
-- Port rule (engine/galactic victory.hpp, q6 EliminationLatch): a player is eliminated only after a check has seen it own
-- something. Port result per campaign: A goes on; B ends (human eliminated) at the first check after t+50 s; C ends at
-- once (Rebel's Opponent_Controls_No_Planets passes). Literal original reading (STRATEGIC_GAPS sec. 1.4, no
-- elimination test at all): A and B go on, C ends at once.
-- The thread starts from Q6E_Begin (STORY_ELAPSED 1) and records every 5 s who owns each planet (HB). When the
-- campaign ends the galactic mode goes and the HB records stop; DONE comes at t+170 s if it never ended.
-- Output channel: strings "Q6P|<KIND>|p=elim|mode=..|sid=..|n=..|f=..|t=..|...|END" in Q6E_LOG (heap scan).
require("PGStoryMode")

Q6E_MODE = "@@MODE@@"
Q6E_PLANETS = { "Tatooine", "Byss", "Abregado_Rae", "Jabiim" }
Q6E_GIVE_T = 20
Q6E_TAKE_T = 50
Q6E_D_T = 20
Q6E_E_GIVE_T = 10
Q6E_E_T = 40
Q6E_END_T = 170

Q6E_LOG = {}
Q6E_N = 0
Q6E_SID = string.sub((string.gsub(tostring(Q6E_LOG), "[^%w]", "")), -6)
Q6E_Started = false

function Q6E_Time()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function Q6E_Rec(kind, fields)
	pcall(function()
		Q6E_N = Q6E_N + 1
		local t = Q6E_Time()
		local fs, ts = "?", "?"
		if t then
			fs = string.format("%.0f", t * 30)
			ts = string.format("%.2f", t)
		end
		local s = "Q6P|" .. kind .. "|p=elim|mode=" .. Q6E_MODE .. "|sid=" .. Q6E_SID .. "|n=" .. Q6E_N .. "|f=" .. fs ..
			"|t=" .. ts
		if fields and fields ~= "" then
			s = s .. "|" .. fields
		end
		Q6E_LOG[Q6E_N] = s .. "|END"
	end)
end

function Q6E_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function Q6E_Say(id)
	Q6E_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

function Q6E_Faction(p)
	if p == nil then
		return "nil"
	end
	local ok, n = pcall(function() return p.Get_Faction_Name() end)
	if ok and n ~= nil then
		return Q6E_Str(n)
	end
	return "?"
end

function Q6E_Owner(planet_name)
	local ok, o = pcall(function() return FindPlanet(planet_name).Get_Owner() end)
	if ok then
		return o
	end
	return nil
end

function Q6E_Count(list)
	if type(list) ~= "table" then
		return "?"
	end
	return tostring(table.getn(list))
end

-- "Tatooine=NEUTRAL|Byss=..." plus how many planets / objects the human owns.
function Q6E_Owners(empire)
	local s = ""
	local owned = 0
	local me = string.upper(Q6E_Faction(empire))
	for _, n in ipairs(Q6E_PLANETS) do
		local f = Q6E_Faction(Q6E_Owner(n))
		s = s .. "|" .. n .. "=" .. f
		if string.upper(f) == me then  -- compare by faction name: two wrappers of one player need not be ==
			owned = owned + 1
		end
	end
	local objs = "?"
	pcall(function() objs = Q6E_Count(Find_All_Objects_Of_Type(empire)) end)
	return "owned=" .. owned .. "|objs=" .. objs .. s
end

function Definitions()
	StoryModeEvents = { Q6E_Begin = Q6E_State_Begin }
end

function Q6E_State_Begin(message)
	if message == OnEnter and not Q6E_Started then
		Q6E_Started = true
		Create_Thread("Q6E_Run")
	end
end

function Q6E_Run()
	local empire = Find_Player("Empire")
	local neutral = Find_Player("Neutral")
	local rebel = Find_Player("Rebel")
	local human = "?"
	pcall(function() human = tostring(empire.Is_Human()) end)
	Q6E_Rec("START", "human=" .. human .. "|" .. Q6E_Owners(empire))
	Q6E_Say("TEXT_Q6_ELIM_RUNNING_" .. Q6E_MODE)
	local t0 = Q6E_Time() or 0
	local next_hb = t0
	local given, taken = false, false
	local event_done = false
	local fast_until = -1
	while true do
		local dt = (Q6E_Time() or 0) - t0
		if Q6E_MODE == "B" and not given and dt >= Q6E_GIVE_T then
			given = true
			local ok, err = pcall(function() FindPlanet("Abregado_Rae").Change_Owner(empire) end)
			Q6E_Rec("GIVE", "ok=" .. tostring(ok) .. "|err=" .. string.sub(Q6E_Str(err), 1, 80) .. "|" .. Q6E_Owners(empire))
			Q6E_Say("TEXT_Q6_ELIM_GIVEN")
		end
		if Q6E_MODE == "B" and given and not taken and dt >= Q6E_TAKE_T then
			taken = true
			local ok, err = pcall(function() FindPlanet("Abregado_Rae").Change_Owner(neutral) end)
			Q6E_Rec("TAKE", "ok=" .. tostring(ok) .. "|err=" .. string.sub(Q6E_Str(err), 1, 80) .. "|" .. Q6E_Owners(empire))
			Q6E_Say("TEXT_Q6_ELIM_TAKEN")
		end
		if Q6E_MODE == "D" and not event_done and dt >= Q6E_D_T then
			event_done = true
			local ok, err = pcall(function() FindPlanet("Abregado_Rae").Change_Owner(rebel) end)
			Q6E_Rec("AIGIVE", "ok=" .. tostring(ok) .. "|err=" .. string.sub(Q6E_Str(err), 1, 80) .. "|" .. Q6E_Owners(empire))
			Q6E_Say("TEXT_Q6_ELIM_AI_GIVEN")
			fast_until = dt + 15
			next_hb = Q6E_Time() or 0
		end
		if Q6E_MODE == "E" and not given and dt >= Q6E_E_GIVE_T then
			given = true
			local ok, err = pcall(function() FindPlanet("Tatooine").Change_Owner(empire) end)
			Q6E_Rec("GIVE", "ok=" .. tostring(ok) .. "|err=" .. string.sub(Q6E_Str(err), 1, 80) .. "|" .. Q6E_Owners(empire))
			Q6E_Say("TEXT_Q6_ELIM_E_GIVEN")
		end
		if Q6E_MODE == "E" and not event_done and dt >= Q6E_E_T then
			event_done = true
			local ok, err = pcall(function() FindPlanet("Tatooine").Change_Owner(neutral) end)
			Q6E_Rec("TAKE", "ok=" .. tostring(ok) .. "|err=" .. string.sub(Q6E_Str(err), 1, 80) .. "|" .. Q6E_Owners(empire))
			Q6E_Say("TEXT_Q6_ELIM_E_TAKEN")
			fast_until = dt + 15
			next_hb = Q6E_Time() or 0
		end
		if (Q6E_Time() or 0) >= next_hb then
			if dt < fast_until then
				next_hb = next_hb + 1
			else
				next_hb = next_hb + 5
			end
			Q6E_Rec("HB", "dt=" .. string.format("%.1f", dt) .. "|" .. Q6E_Owners(empire))
		end
		if dt >= Q6E_END_T then
			break
		end
		Sleep(1)
	end
	Q6E_Rec("DONE", "records=" .. Q6E_N .. "|" .. Q6E_Owners(empire))
	Q6E_Say("TEXT_Q6_ELIM_DONE")
end

Q6E_Rec("TOP", "")
-- ==== end of the Q6 t15 elimination probe ====
