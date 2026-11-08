-- ==== cu8g probes: shared helpers (tools/probe_mod/cu8g_probes/cu8g_common.lua) ====
-- build_cu8g_mod.py appends, to the shipped BusyTacticalFreeStore.lua: a generated C8_CFG table, this file, then the
-- probe hooks of the session (cu8g_stick.lua, cu8g_lure.lua, cu8g_cancel.lua, cu8g_maps_tact.lua). Each hook registers
-- C8_PROBES[<game mode>] = { name, tick(F), fast, quiet, any_ai }.
--
-- Arming: only one AI player's free store runs a probe: the one whose faction is C8_CFG.ai_faction (or any AI player
-- when the probe sets any_ai), in the mode the probe is registered for. Every other free store records SKIP and keeps
-- the shipped behaviour. Base_Definitions runs before the engine sets PlayerObject (oi_abilities run 2026-11-04), so the
-- decision is retried from the first service calls until the faction is readable.
-- fast:  ServiceRate is lowered to 0.05 s, so FreeStoreService runs on every free-store service (every 3rd frame,
--        AI_TACTICAL sec. 5.3); the probe is a frame-timed state machine driven from FreeStoreService (On_Unit_Service
--        as a fallback). No thread, no yield inside pcall (Lua 5.0).
-- quiet: while the probe runs the shipped FreeStoreService / On_Unit_Service bodies are skipped (the AI's free units
--        stay idle, as in the OI probe).
-- Output channel: retail _CustomScriptMessage writes nothing, so every record is a Lua string
-- "C8G|<KIND>|p=<probe>|sid=<script id>|n=<n>|f=<mode frame>|...|END" kept in C8_LOG; cu8g_run.py finds them with a
-- read-only heap scan. f = GetCurrentTime() * 30 rounded. Lua 5.0: no '#', no math library (s78g), table.getn.

C8_LOG = {}
C8_N = 0
C8_SID = string.sub((string.gsub(tostring(C8_LOG), "[^%w]", "")), -6)
C8 = { on = false, done = false, f = -1, probe = "-", mode = "?", fac = "?", tries = 0, pending = false,
	old_rate = nil, pr = nil }
C8_API_BAD = {}
C8_PROBES = {}
C8_MAX_RECORDS = 30000

if C8_CFG == nil then
	C8_CFG = { session = "?", ai_faction = "?" }
end

function C8_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function C8_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function C8_Frame()
	local t = C8_Now()
	if t == nil then
		return nil
	end
	return tonumber(string.format("%.0f", t * 30))
end

function C8_Rec(kind, fields)
	pcall(function()
		if C8_N >= C8_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		C8_N = C8_N + 1
		local body = fields or ""
		if string.len(body) > 360 then
			body = string.sub(body, 1, 360) .. "~"
		end
		local f = C8_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		C8_LOG[C8_N] = "C8G|" .. kind .. "|p=" .. C8.probe .. "|sid=" .. C8_SID .. "|n=" .. C8_N .. "|f=" .. fs .. "|" ..
			body .. "|END"
	end)
end

-- cu8g t10: step trace (one record before each engine call while C8.trace is on), to find a call that stops the
-- script silently (a failed engine read sets the script's failed flag; pcall cannot catch it, a2i 2026-11-06).
function C8_Step(what)
	if C8.trace then
		C8_Rec("ST", "s=" .. what)
	end
end

function C8_ApiFail(tag, err)
	if not C8_API_BAD[tag] then
		C8_API_BAD[tag] = true
		C8_Rec("APIERR", "api=" .. C8_Str(tag) .. "|err=" .. string.sub(C8_Str(err), 1, 150))
	end
end

-- pcall wrapper: up to 3 results, nil on error (the failing API is recorded once).
function C8_Try(tag, f)
	local ok, a, b, c = pcall(f)
	if not ok then
		C8_ApiFail(tag, a)
		return nil
	end
	return a, b, c
end

function C8_Say(id)
	C8_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

function C8_Valid(o)
	if o == nil then
		return false
	end
	local ok, v = pcall(function() return o.Is_Valid() end)
	return ok and v == true
end

-- Object id: Get_ID, else Get_Object_ID (LUA_API: the object method is not documented by a shipped call).
function C8_Id(o)
	local id = nil
	C8_Step("id:Get_ID")
	pcall(function() id = o.Get_ID() end)
	if id == nil then
		C8_Step("id:Get_Object_ID")
		pcall(function() id = o.Get_Object_ID() end)
	end
	if id == nil then
		return "?"
	end
	return C8_Str(id)
end

function C8_Type(o)
	local n = "?"
	C8_Step("Get_Type")
	pcall(function() n = o.Get_Type().Get_Name() end)
	return C8_Str(n)
end

function C8_XYZ(o)
	local ok, x, y, z = pcall(function()
		local a, b, c = o.Get_Position().Get_XYZ()
		if type(a) == "table" then
			return a[1], a[2], a[3]
		end
		return a, b, c
	end)
	if ok then
		return x, y, z
	end
	return nil
end

function C8_XY(o)
	local x, y = C8_XYZ(o)
	if type(x) == "number" and type(y) == "number" then
		return string.format("%.0f,%.0f", x, y)
	end
	return "?"
end

function C8_Num(v, fmt)
	if type(v) == "number" then
		return string.format(fmt or "%.3f", v)
	end
	return "?"
end

-- obj.method() as a number (nil when it fails or is not a number).
function C8_Get(o, method)
	local v = nil
	local ok = pcall(function() v = o[method]() end)
	if ok and type(v) == "number" then
		return v
	end
	return nil
end

function C8_Flag(o, method, arg)
	local ok, v = pcall(function() return o[method](arg) end)
	if ok then
		if v then
			return "1"
		end
		return "0"
	end
	return "?"
end

function C8_Dist(a, b)
	local ok, d = pcall(function() return a.Get_Distance(b) end)
	if ok and type(d) == "number" then
		return d
	end
	return nil
end

function C8_Pos(x, y, z)
	return C8_Try("Create_Position", function() return Create_Position(x, y, z or 0) end)
end

function C8_Player(name)
	local p = nil
	pcall(function() p = Find_Player(name) end)
	return p
end

-- Spawn one object of type tname at pos for player: Spawn_Unit first (returns a table), Create_Generic_Object as the
-- fallback (or first when generic). Returns object, how.
function C8_Spawn(tname, pos, player, generic)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if ty == nil then
		return nil, "no_type"
	end
	if pos == nil or player == nil then
		return nil, "no_pos_or_player"
	end
	if generic then
		local ok, o = pcall(Create_Generic_Object, ty, pos, player)
		if ok and C8_Valid(o) then
			return o, "generic"
		end
	end
	local ok, r = pcall(Spawn_Unit, ty, pos, player)
	if ok and type(r) == "table" and r[1] ~= nil then
		return r[1], "spawn_unit"
	end
	if ok and r ~= nil and type(r) ~= "table" and C8_Valid(r) then
		return r, "spawn_unit"
	end
	if not generic then
		local ok2, o = pcall(Create_Generic_Object, ty, pos, player)
		if ok2 and C8_Valid(o) then
			return o, "generic"
		end
	end
	if not ok then
		C8_ApiFail("Spawn_Unit:" .. tname, r)
	end
	return nil, "spawn_failed"
end

function C8_Call(o, tag, f)
	if not C8_Valid(o) then
		return "dead"
	end
	local ok, err = pcall(f)
	if ok then
		return "ok"
	end
	C8_ApiFail(tag, err)
	return "err"
end

function C8_Despawn(o)
	if C8_Valid(o) then
		pcall(function() o.Despawn() end)
	end
end

-- The attack target of o: (class, id, type). class is "none", "err" or "obj".
function C8_TargetOf(o)
	local tg = nil
	C8_Step("Get_Attack_Target")
	local ok = pcall(function() tg = o.Get_Attack_Target() end)
	if not ok then
		return "err", "?", "?"
	end
	if tg == nil or tg == false then
		return "none", "-", "-"
	end
	return "obj", C8_Id(tg), C8_Type(tg)
end

-- Which of the named objects (role -> object) is the attack target of o: the role, "none", "err" or "other:<type>".
-- Compared by id; when ids are not readable, by type name (each role has its own type in these probes).
function C8_TargetRole(o, roles, ids, types)
	local cls, id, ty = C8_TargetOf(o)
	if cls ~= "obj" then
		return cls
	end
	for role, rid in pairs(ids) do
		if rid ~= "?" and id ~= "?" and rid == id then
			return role
		end
	end
	if id == "?" then
		for role, rty in pairs(types) do
			if rty == ty then
				return role .. "~"
			end
		end
	end
	return "other:" .. ty
end

-- ---- arming and the service drivers ----------------------------------------------------------------------------

function C8_Arm()
	local fac, mode, human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() human = PlayerObject.Is_Human() end)
	if (fac == "?" or fac == "NIL") and C8.tries < 200 then
		C8.pending = true
		return
	end
	C8.pending = false
	C8.mode = C8_Str(mode)
	C8.fac = C8_Str(fac)
	local pr = C8_PROBES[mode]
	-- cu8g t10: a probe with a human side arms only when that side is played by a human, so the main-menu background
	-- battle (two AI players) never arms a kit.
	local human_ok = true
	if pr and pr.human then
		human_ok = false
		pcall(function() human_ok = Find_Player(pr.human).Is_Human() == true end)
	end
	if pr and not human_ok and C8.tries < 60 then
		C8.pending = true
		return
	end
	if pr and not human_ok then
		C8_Rec("SKIP", "fac=" .. C8_Str(fac) .. "|mode=" .. C8_Str(mode) .. "|why=no_human_" .. C8_Str(pr.human) .. "|session=" ..
			C8_Str(C8_CFG.session))
		return
	end
	if pr and (pr.any_ai or fac == C8_CFG.ai_faction) and human ~= true and not C8_ARMED_ONCE then
		C8_ARMED_ONCE = true
		C8.on = true
		C8.pr = pr
		C8.probe = pr.name
		C8.old_rate = ServiceRate
		if pr.fast then
			ServiceRate = 0.05
		end
		C8_Rec("ARM", "fac=" .. C8.fac .. "|mode=" .. C8.mode .. "|session=" .. C8_Str(C8_CFG.session) .. "|rate=" ..
			C8_Str(C8.old_rate) .. "|fast=" .. tostring(pr.fast == true) .. "|quiet=" .. tostring(pr.quiet == true) ..
			"|tries=" .. C8.tries)
	else
		C8_Rec("SKIP", "fac=" .. C8.fac .. "|mode=" .. C8.mode .. "|human=" .. tostring(human) .. "|session=" ..
			C8_Str(C8_CFG.session))
	end
end

function C8_Finish(extra)
	if C8.done then
		return
	end
	C8.done = true
	if C8.pr and C8.pr.fast and C8.old_rate ~= nil then
		ServiceRate = C8.old_rate
	end
	local bad = ""
	for k, _ in pairs(C8_API_BAD) do
		bad = bad .. k .. ","
	end
	C8_Rec("DONE", "records=" .. C8_N .. "|apierr=" .. string.sub(C8_Str(bad), 1, 220) .. "|" .. (extra or ""))
end

function C8_Tick(src)
	if C8.pending then
		C8.tries = C8.tries + 1
		pcall(C8_Arm)
	end
	if not C8.on or C8.done then
		return
	end
	local F = C8_Frame()
	if F == nil or F == C8.f then
		return
	end
	C8.f = F
	-- cu8g t10: a heartbeat every 30 frames tells a stopped free-store service from a probe that stopped recording.
	if C8.hb == nil or F - C8.hb >= 30 then
		C8.hb = F
		C8_Rec("HB", "src=" .. src)
	end
	local ok, err = pcall(C8.pr.tick, F)
	if not ok then
		C8_Rec("ERR", "where=tick|src=" .. src .. "|err=" .. string.sub(C8_Str(err), 1, 200))
	end
end

function C8_Quiet()
	return C8.on and not C8.done and C8.pr ~= nil and C8.pr.quiet == true
end

C8_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = nil
	if C8_Orig_Base_Definitions then
		r = C8_Orig_Base_Definitions()
	end
	pcall(C8_Arm)
	return r
end

C8_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(C8_Tick, "fss")
	if C8_Quiet() or C8_Orig_FreeStoreService == nil then
		return
	end
	return C8_Orig_FreeStoreService()
end

C8_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(C8_Tick, "ous")
	if C8_Quiet() or C8_Orig_On_Unit_Service == nil then
		return
	end
	return C8_Orig_On_Unit_Service(object)
end

C8_Rec("LOAD", "session=" .. C8_Str(C8_CFG.session) .. "|ai_faction=" .. C8_Str(C8_CFG.ai_faction))
-- ==== end of cu8g_common.lua ====
