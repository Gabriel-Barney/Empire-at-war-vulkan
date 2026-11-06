-- ==== Q6 t15 probe: story timing (tools/probe_mod/q6_t15_probes/q6_story_timing.lua) ====
-- Galactic plot script of the campaign "Q6 PROBE: STORY TIMING" (human Empire), installed by build_q6_mod.py as
-- Data\Scripts\Story\Q6_Story_Timing.lua. Plot events (Q6_Story_Timing.xml):
--   Q6S_Begin0   STORY_ELAPSED 0, no prereq (the shipped "<Mission>_Begin" pattern)        -> state Q6S_State_Begin0
--   Q6S_Begin3   STORY_ELAPSED 3, no prereq (the proven S10 start pattern, starts the thread) -> state Q6S_State_Begin3
--   Q6S_Once2    STORY_ELAPSED 2, not Perpetual (reference: first fire of a prereq-less timer)
--   Q6S_Cycle2 / Q6S_Cycle1 / Q6S_Cycle07 / Q6S_Cycle40   STORY_ELAPSED 2 / 1 / 0.7 / 40, Perpetual
-- Questions: docs/re/STORY.md sec. 4.7 (period of a Perpetual ELAPSED event; port: T + one story service, frames
-- 61 / 122 for T = 2) and STORY.md sec. 8 item 9 (does the original resume the plot's threads once at load, before
-- the frame-1 STORY_ELAPSED dispatch?).
-- Records: TOP (file top level), DEFS (Definitions), MAIN (first line of main = first resume of the main thread),
-- PUMP (Pump_Service = one PumpEvents of a thread, first 12 only, with LastService / ServiceRate), TRIG (every
-- Story_Event_Trigger call: the engine calls it synchronously inside Fire, STORY.md sec. 1.5 step 3, so its frame is
-- the fire frame), ENTER (state OnEnter), HB (every 10 s), DONE. The fire frames are frame exact:
-- f = round(GetCurrentTime() * 30).
-- Output channel: strings "Q6P|<KIND>|p=story|sid=..|n=..|f=..|t=..|...|END" kept in Q6S_LOG (q6_run.py finds them by a
-- read-only heap scan; retail _CustomScriptMessage / _MessagePopup show nothing). Lua 5.0: no '#', table.getn.
require("PGStoryMode")

Q6S_LOG = {}
Q6S_N = 0
Q6S_SID = string.sub((string.gsub(tostring(Q6S_LOG), "[^%w]", "")), -6)
Q6S_MainStarted = false
Q6S_Pumps = 0
Q6S_Started = false
Q6S_FireCount = {}
Q6S_MAX_FIRES = 40
Q6S_END_T = 130

function Q6S_Time()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function Q6S_Rec(kind, fields)
	pcall(function()
		Q6S_N = Q6S_N + 1
		local t = Q6S_Time()
		local fs, ts = "?", "?"
		if t then
			fs = string.format("%.0f", t * 30)
			ts = string.format("%.4f", t)
		end
		local s = "Q6P|" .. kind .. "|p=story|sid=" .. Q6S_SID .. "|n=" .. Q6S_N .. "|f=" .. fs .. "|t=" .. ts
		if fields and fields ~= "" then
			s = s .. "|" .. fields
		end
		Q6S_LOG[Q6S_N] = s .. "|END"
	end)
end

function Q6S_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function Q6S_States()
	return "cur=" .. Q6S_Str(CurrentState) .. "|next=" .. Q6S_Str(NextState) .. "|main=" .. tostring(Q6S_MainStarted)
end

function Q6S_Say(id)
	Q6S_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

function Definitions()
	Q6S_Rec("DEFS", Q6S_States() .. "|rate=" .. Q6S_Str(ServiceRate))
	StoryModeEvents = { Q6S_Begin0 = Q6S_State_Begin0, Q6S_Begin3 = Q6S_State_Begin3 }
end

function Q6S_State_Begin0(message)
	if message == OnEnter then
		Q6S_Rec("ENTER", "event=Q6S_Begin0|" .. Q6S_States())
	end
end

function Q6S_State_Begin3(message)
	if message == OnEnter then
		Q6S_Rec("ENTER", "event=Q6S_Begin3|" .. Q6S_States())
		if not Q6S_Started then
			Q6S_Started = true
			Create_Thread("Q6S_Watch")
		end
	end
end

function Q6S_Watch()
	Q6S_Say("TEXT_Q6_STORY_RUNNING")
	local t0 = Q6S_Time() or 0
	local next_hb = t0
	while true do
		local t = Q6S_Time() or 0
		if t >= next_hb then
			next_hb = next_hb + 10
			local c = ""
			for k, v in pairs(Q6S_FireCount) do
				c = c .. "|" .. k .. "=" .. v
			end
			Q6S_Rec("HB", "records=" .. Q6S_N .. c)
		end
		if t >= Q6S_END_T then
			break
		end
		Sleep(1)
	end
	Q6S_Rec("DONE", "records=" .. Q6S_N)
	Q6S_Say("TEXT_Q6_STORY_DONE")
end

-- The engine calls Story_Event_Trigger(name) inside Fire (synchronous); record, then the PGStoryMode body.
Q6S_Orig_Story_Event_Trigger = Story_Event_Trigger
function Story_Event_Trigger(name)
	local key = Q6S_Str(name)
	local c = (Q6S_FireCount[key] or 0) + 1
	Q6S_FireCount[key] = c
	if c <= Q6S_MAX_FIRES then
		Q6S_Rec("TRIG", "event=" .. key .. "|k=" .. c .. "|" .. Q6S_States())
	end
	if Q6S_Orig_Story_Event_Trigger then
		return Q6S_Orig_Story_Event_Trigger(name)
	end
end

Q6S_Orig_main = main
function main()
	Q6S_MainStarted = true
	Q6S_Rec("MAIN", Q6S_States() .. "|ls=" .. Q6S_Str(LastService) .. "|rate=" .. Q6S_Str(ServiceRate))
	return Q6S_Orig_main()
end

Q6S_Orig_Pump_Service = Pump_Service
function Pump_Service()
	Q6S_Pumps = Q6S_Pumps + 1
	if Q6S_Pumps <= 12 then
		local th = "?"
		pcall(function() th = tostring(GetThreadID()) end)
		Q6S_Rec("PUMP", "k=" .. Q6S_Pumps .. "|th=" .. Q6S_Str(th) .. "|ls=" .. Q6S_Str(LastService) .. "|rate=" ..
			Q6S_Str(ServiceRate) .. "|" .. Q6S_States())
	end
	if Q6S_Orig_Pump_Service then
		return Q6S_Orig_Pump_Service()
	end
end

Q6S_Rec("TOP", Q6S_States() .. "|ls=" .. Q6S_Str(LastService))
-- ==== end of the Q6 t15 story timing probe ====
