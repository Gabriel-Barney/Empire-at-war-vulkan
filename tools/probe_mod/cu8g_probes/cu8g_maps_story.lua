-- ==== cu8g t08: missing fallback maps, galactic recorder (tools/probe_mod/cu8g_probes/cu8g_maps_story.lua) ====
-- Galactic plot script of the campaign "CU8G PROBE: MISSING MAPS" (human Rebel at Abregado Rae; AI Empire with
-- AI_Player_Control None holds Jabiim (land target: Land_Tactical_Map emptied, Terrain Temperate, ground forces only)
-- and Thyferra (space target: Space_Tactical_Map emptied, Terrain Temperate, one Tartan_Patrol_Cruiser)). Installed by
-- build_cu8g_mod.py as Data\Scripts\Story\CU8G_Story_Maps.lua; @@SESSION@@ is replaced by the session name.
-- The thread starts from the plot's STORY_ELAPSED 1 event and records every 2 s who owns each planet and how many
-- objects the Rebel and the Empire have (HB). While a tactical battle runs the galactic mode does not service the plot,
-- so the HB records pause; an owner change without a tactical record (cu8g_maps_tact.lua) between two HB records is an
-- auto-resolve. Output: strings "C8G|<KIND>|p=gal|...|END" in C8G_LOG (heap scan, cu8g_run.py).
require("PGStoryMode")

C8G_SESSION = "@@SESSION@@"
C8G_PLANETS = { "Abregado_Rae", "Jabiim", "Thyferra" }
C8G_END_T = 1800

C8G_LOG = {}
C8G_N = 0
C8G_SID = string.sub((string.gsub(tostring(C8G_LOG), "[^%w]", "")), -6)
C8G_Started = false

function C8G_Time()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function C8G_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function C8G_Rec(kind, fields)
	pcall(function()
		C8G_N = C8G_N + 1
		local t = C8G_Time()
		local fs, ts = "?", "?"
		if t then
			fs = string.format("%.0f", t * 30)
			ts = string.format("%.1f", t)
		end
		local s = "C8G|" .. kind .. "|p=gal|sid=" .. C8G_SID .. "|n=" .. C8G_N .. "|f=" .. fs .. "|t=" .. ts ..
			"|session=" .. C8G_SESSION
		if fields and fields ~= "" then
			s = s .. "|" .. fields
		end
		C8G_LOG[C8G_N] = s .. "|END"
	end)
end

function C8G_Say(id)
	C8G_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

function C8G_Faction(p)
	if p == nil then
		return "nil"
	end
	local ok, n = pcall(function() return p.Get_Faction_Name() end)
	if ok and n ~= nil then
		return C8G_Str(n)
	end
	return "?"
end

function C8G_Count(p)
	local n = "?"
	pcall(function()
		local l = Find_All_Objects_Of_Type(p)
		if type(l) == "table" then
			n = tostring(table.getn(l))
		end
	end)
	return n
end

function C8G_State()
	local s = ""
	for _, n in ipairs(C8G_PLANETS) do
		local f = "?"
		pcall(function() f = C8G_Faction(FindPlanet(n).Get_Owner()) end)
		s = s .. "|" .. n .. "=" .. f
	end
	return "rebel_objs=" .. C8G_Count(Find_Player("Rebel")) .. "|empire_objs=" .. C8G_Count(Find_Player("Empire")) .. s
end

function Definitions()
	StoryModeEvents = { C8G_Begin = C8G_State_Begin }
end

function C8G_State_Begin(message)
	if message == OnEnter and not C8G_Started then
		C8G_Started = true
		Create_Thread("C8G_Run")
	end
end

function C8G_Run()
	local human = "?"
	pcall(function() human = tostring(Find_Player("Rebel").Is_Human()) end)
	C8G_Rec("START", "rebel_human=" .. human .. "|" .. C8G_State())
	C8G_Say("TEXT_CU8G_MAPS_RUNNING")
	local t0 = C8G_Time() or 0
	local last = ""
	while true do
		local dt = (C8G_Time() or 0) - t0
		local st = C8G_State()
		local kind = "HB"
		if st ~= last then
			kind = "CHG"
			last = st
		end
		C8G_Rec(kind, "dt=" .. string.format("%.1f", dt) .. "|" .. st)
		if dt >= C8G_END_T then
			break
		end
		Sleep(2)
	end
	C8G_Rec("DONE", "records=" .. C8G_N)
end

C8G_Rec("TOP", "")
-- ==== end of cu8g_maps_story.lua ====
