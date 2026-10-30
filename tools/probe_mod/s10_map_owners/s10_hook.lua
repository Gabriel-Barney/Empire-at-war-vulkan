
-- ==== S10 map-owner probe (appended to the shipped GameScoring.lua by tools/probe_mod/s10_map_owners/build_mod.py) ====
-- Read-only: no GameRandom, no spawning, no orders. For every land battle it logs every player (Find_Player per faction)
-- and every object it can enumerate (Find_All_Objects_Of_Type(player) for each player, plus the map's interesting type
-- names), with type, owner (id / faction / name / human) and position, at the first script service after the mode
-- starts and again ~10 s and ~40 s later (after the 30 s capture countdown). Output: _CustomScriptMessage(S10_FILE)
-- (was a no-op in the 2026-10-29 retail run) and, unless built with --no-popup, a _MessagePopup per land battle start
-- and per dump ("S10 <tag> <map> FACTION=player_id:object_count ..."), which s10reader records and dismisses.
-- Proof that the hook runs: Game_Message("TEXT_S10_HOOK_ALIVE") on screen at each land battle start + the popups.
-- Lua 5.0 (no '#', varargs via 'arg').

S10_FILE = "_S10MapOwners.txt"
S10_FACTIONS = { "Rebel", "Empire", "Pirates", "Neutral", "Hostile", "Sarlacc", "Underworld", "Hutts" }
S10_TYPES = { --@@TYPES@@
}
S10_DUMP_AT = { 0, 10, 40 }
S10_POPUP = true --@@POPUP@@
S10_Land = false
S10_Mode = "?"
S10_Map = "?"
S10_T0 = nil
S10_Done = {}
S10_Services = 0
S10_Battle = 0
S10_Refs = nil

function S10_Log(s)
	pcall(_CustomScriptMessage, S10_FILE, s .. "\n")
	pcall(_OuputDebug, "S10 " .. s .. "\n")
end

function S10_Str(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return string.gsub(tostring(v), "%s", "_")
	end
	return "?"
end

function S10_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return -1
end

function S10_Frame()
	local ok, f = pcall(GetCurrentTime.Frame)
	if ok and f then
		return tostring(f)
	end
	return "?"
end

function S10_PlayerStr(p)
	if p == nil then
		return "id=nil faction=nil name=nil human=nil neutral=nil"
	end
	return "id=" .. S10_Str(function() return p.Get_ID() end) ..
		" faction=" .. S10_Str(function() return p.Get_Faction_Name() end) ..
		" name=" .. S10_Str(function() return p.Get_Name() end) ..
		" human=" .. S10_Str(function() return p.Is_Human() end) ..
		" neutral=" .. S10_Str(function() return p.Is_Neutral() end)
end

function S10_PosStr(obj)
	local ok, pos = pcall(function() return obj.Get_Position() end)
	if ok and pos then
		local ok2, x, y, z = pcall(function() return pos.Get_XYZ() end)
		if ok2 and type(x) == "table" then
			x, y, z = x[1], x[2], x[3]
		end
		if ok2 and type(x) == "number" and type(y) == "number" then
			return string.format("pos=%.2f,%.2f,%.2f", x, y, z or 0)
		end
	end
	-- fallback: distances to three reference positions (the parser trilaterates x, y)
	if S10_Refs == nil then
		S10_Refs = {}
		pcall(function()
			S10_Refs[1] = Create_Position(0, 0, 0)
			S10_Refs[2] = Create_Position(1000, 0, 0)
			S10_Refs[3] = Create_Position(0, 1000, 0)
		end)
	end
	local d = {}
	for i = 1, 3 do
		local okd, v = pcall(function() return obj.Get_Distance(S10_Refs[i]) end)
		if okd and type(v) == "number" then
			d[i] = string.format("%.2f", v)
		else
			d[i] = "?"
		end
	end
	return "dist=" .. d[1] .. "," .. d[2] .. "," .. d[3]
end

function S10_ObjLine(src, obj)
	local tname = S10_Str(function() return obj.Get_Type().Get_Name() end)
	local owner = nil
	pcall(function() owner = obj.Get_Owner() end)
	return "OBJ src=" .. src .. " type=" .. tname .. " " .. S10_PosStr(obj) ..
		" hull=" .. S10_Str(function() return obj.Get_Hull() end) .. " owner:" .. S10_PlayerStr(owner)
end

function S10_Dump(tag)
	S10_Log(string.format("DUMP battle=%d tag=%s mode=%s map=%s time=%.3f frame=%s services=%d", S10_Battle, tag,
		S10_Mode, S10_Map, S10_Now(), S10_Frame(), S10_Services))
	local loc = nil
	pcall(function() loc = Find_Player("local") end)
	S10_Log("LOCAL " .. S10_PlayerStr(loc))
	local n = 0
	local summary = ""
	for _, fac in ipairs(S10_FACTIONS) do
		local p = nil
		pcall(function() p = Find_Player(fac) end)
		S10_Log("PLAYER query=" .. fac .. " " .. S10_PlayerStr(p))
		if p then
			local ok, list = pcall(Find_All_Objects_Of_Type, p)
			local c = 0
			if ok and type(list) == "table" then
				for _, obj in pairs(list) do
					S10_Log(S10_ObjLine("P:" .. fac, obj))
					c = c + 1
				end
			end
			S10_Log("PLAYER_OBJECTS query=" .. fac .. " ok=" .. tostring(ok) .. " count=" .. c)
			summary = summary .. fac .. "=" .. S10_Str(function() return p.Get_ID() end) .. ":" .. c .. " "
			n = n + c
		end
	end
	for _, tname in ipairs(S10_TYPES) do
		local ok, list = pcall(Find_All_Objects_Of_Type, tname)
		if ok and type(list) == "table" then
			for _, obj in pairs(list) do
				S10_Log(S10_ObjLine("T", obj))
				n = n + 1
			end
		end
	end
	S10_Log(string.format("END battle=%d tag=%s lines=%d", S10_Battle, tag, n))
	if S10_POPUP then
		pcall(_MessagePopup, "S10 " .. tag .. " " .. S10_Map .. " " .. summary)
	end
end

function S10_Service()
	if not S10_Land then
		return
	end
	S10_Services = S10_Services + 1
	local t = S10_Now()
	if S10_T0 == nil then
		S10_T0 = t
	end
	for i, at in ipairs(S10_DUMP_AT) do
		if not S10_Done[i] and (i == 1 or (t >= 0 and t - S10_T0 >= at)) then
			S10_Done[i] = true
			pcall(S10_Dump, "t" .. at)
		end
	end
end

S10_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	S10_Orig_Base_Definitions()
	ServiceRate = 1
	S10_Log("S10HOOK 1 loaded")
end

S10_Orig_Game_Mode_Starting_Event = Game_Mode_Starting_Event
function Game_Mode_Starting_Event(mode_name, map_name)
	S10_Mode = tostring(mode_name)
	S10_Map = tostring(map_name)
	S10_Land = StringCompare(S10_Mode, "Land") and true or false
	S10_T0 = nil
	S10_Done = {}
	S10_Services = 0
	if S10_Land then
		S10_Battle = S10_Battle + 1
	end
	S10_Log(string.format("MODE_START battle=%d mode=%s map=%s time=%.3f frame=%s", S10_Battle, S10_Mode, S10_Map,
		S10_Now(), S10_Frame()))
	for _, fac in ipairs(S10_FACTIONS) do
		local p = nil
		pcall(function() p = Find_Player(fac) end)
		S10_Log("PLAYER query=" .. fac .. " " .. S10_PlayerStr(p))
	end
	if S10_Land then
		pcall(Game_Message, "TEXT_S10_HOOK_ALIVE")
		if S10_POPUP then
			pcall(_MessagePopup, "S10 hook alive battle=" .. S10_Battle .. " mode=" .. S10_Mode .. " map=" .. S10_Map)
		end
	end
	S10_Orig_Game_Mode_Starting_Event(mode_name, map_name)
end

S10_Orig_Game_Mode_Ending_Event = Game_Mode_Ending_Event
function Game_Mode_Ending_Event(mode_name)
	S10_Log(string.format("MODE_END mode=%s time=%.3f services=%d", tostring(mode_name), S10_Now(), S10_Services))
	if S10_Land and not S10_Done[1] then
		pcall(S10_Dump, "end")
	end
	S10_Land = false
	S10_Orig_Game_Mode_Ending_Event(mode_name)
end

S10_Orig_GameService = GameService
function GameService()
	pcall(S10_Service)
	S10_Orig_GameService()
end
-- ==== end S10 probe ====
