-- ==== A1 free-store timing probe (appended to the shipped BusyTacticalFreeStore.lua by
-- tools/probe_mod/a1_tactical/freestore_timing/build_fs_mod.py) ====
-- A1PROBE_SRC_MARK_FREESTORE_V1
-- Logging only: the shipped functions run unchanged; the wrappers below only note when the engine calls them.
-- No GameRandom, no spawning, no orders. Lua 5.0 (no '#', varargs via 'arg').
--
-- Output channel: retail _CustomScriptMessage and _OuputDebug write nothing (S10 / Q2 runs, 2026-10-29 / 30), so every
-- record is a Lua string "A1P1|<KIND>|k=v|...|END" kept alive in the table A1P_LOG, and the external reader
-- (a1_reader.py) finds the strings by scanning the game's heap. Fields of every record:
--   sid   this script instance (address of A1P_LOG; one instance per AI player and tactical mode)
--   pid / fac / mode   PlayerObject.Get_ID(), PlayerObject.Get_Faction_Name(), Get_Game_Mode()
--   t     GetCurrentTime() printed with %.9g (mode time; 20.0000019 = (float)600 * (1/30) in float)
--   f30   t * 30 (the mode frame if t = F / 30)        gf   GetCurrentTime.Frame()
-- Kinds: LOAD (file executed), BASEDEF (Base_Definitions), MAIN (main() entered), FSS n=k (k-th FreeStoreService
-- call, first A1P_MAX_FSS), OUS batch=k units_prev=m unit=TYPE (first On_Unit_Service call of a new service time:
-- the k-th unit service pass; m = units serviced in the previous pass; first A1P_MAX_OUS passes), ADD n=k unit=TYPE
-- (k-th On_Unit_Added, first A1P_MAX_ADD), FAC (Find_Player id of every faction, at the first FSS / OUS).

A1P_LOG = {}
A1P_SID = "?"
A1P_FSS_N = 0
A1P_OUS_BATCH = 0
A1P_OUS_UNITS = 0
A1P_OUS_LAST_T = nil
A1P_ADD_N = 0
A1P_FAC_DONE = false
A1P_MAX_FSS = 12
A1P_MAX_OUS = 16
A1P_MAX_ADD = 6
A1P_FACTIONS = { "Rebel", "Empire", "Pirates", "Neutral", "Hostile", "Sarlacc", "Underworld", "Hutts" }

function A1P_Clean(v)
	local s = tostring(v)
	s = string.gsub(s, "[^%w%._%-:]", "_")
	return s
end

function A1P_Num(v)
	if type(v) == "number" then
		return string.format("%.9g", v)
	end
	return "?"
end

function A1P_Time()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function A1P_Frame()
	local ok, f = pcall(function() return GetCurrentTime.Frame() end)
	if ok and type(f) == "number" then
		return f
	end
	return nil
end

function A1P_Call(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return A1P_Clean(v)
	end
	return "?"
end

function A1P_Rec(kind, fields)
	pcall(function()
		local t = A1P_Time()
		local f30 = "?"
		if t then
			f30 = string.format("%.4f", t * 30)
		end
		local s = "A1P1|" .. kind .. "|sid=" .. A1P_SID ..
			"|pid=" .. A1P_Call(function() return PlayerObject.Get_ID() end) ..
			"|fac=" .. A1P_Call(function() return PlayerObject.Get_Faction_Name() end) ..
			"|mode=" .. A1P_Call(Get_Game_Mode) ..
			"|t=" .. A1P_Num(t) .. "|f30=" .. f30 .. "|gf=" .. A1P_Num(A1P_Frame()) ..
			(fields or "") .. "|END"
		table.insert(A1P_LOG, s)
	end)
end

function A1P_UnitType(object)
	return A1P_Call(function() return object.Get_Type().Get_Name() end)
end

function A1P_Factions()
	if A1P_FAC_DONE then
		return
	end
	A1P_FAC_DONE = true
	local s = ""
	for i, name in ipairs(A1P_FACTIONS) do
		local id = A1P_Call(function()
			local p = Find_Player(name)
			if p then
				return p.Get_ID()
			end
			return "none"
		end)
		s = s .. "|" .. name .. "=" .. id
	end
	A1P_Rec("FAC", s)
end

A1P_SID = A1P_Clean(string.gsub(tostring(A1P_LOG), "^table: ", ""))

A1P_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	A1P_Rec("BASEDEF", "")
	return A1P_Orig_Base_Definitions()
end

A1P_Orig_main = main
function main()
	A1P_Rec("MAIN", "")
	return A1P_Orig_main()
end

A1P_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	A1P_FSS_N = A1P_FSS_N + 1
	if A1P_FSS_N <= A1P_MAX_FSS then
		A1P_Rec("FSS", "|n=" .. A1P_FSS_N)
	end
	A1P_Factions()
	return A1P_Orig_FreeStoreService()
end

A1P_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(function()
		local t = A1P_Time()
		if A1P_OUS_LAST_T == nil or t ~= A1P_OUS_LAST_T then
			A1P_OUS_BATCH = A1P_OUS_BATCH + 1
			if A1P_OUS_BATCH <= A1P_MAX_OUS then
				A1P_Rec("OUS", "|batch=" .. A1P_OUS_BATCH .. "|units_prev=" .. A1P_OUS_UNITS .. "|unit=" ..
					A1P_UnitType(object))
			end
			A1P_OUS_LAST_T = t
			A1P_OUS_UNITS = 0
		end
		A1P_OUS_UNITS = A1P_OUS_UNITS + 1
	end)
	A1P_Factions()
	return A1P_Orig_On_Unit_Service(object)
end

A1P_Orig_On_Unit_Added = On_Unit_Added
function On_Unit_Added(object)
	A1P_ADD_N = A1P_ADD_N + 1
	if A1P_ADD_N <= A1P_MAX_ADD then
		A1P_Rec("ADD", "|n=" .. A1P_ADD_N .. "|unit=" .. A1P_UnitType(object))
	end
	return A1P_Orig_On_Unit_Added(object)
end

A1P_Rec("LOAD", "")
-- ==== end of the A1 free-store timing probe ====
