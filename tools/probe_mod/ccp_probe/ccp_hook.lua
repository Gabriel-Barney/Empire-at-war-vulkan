-- ==== ccpgu t02 Capture Control Points probe (tools/probe_mod/ccp_probe/ccp_hook.lua), appended to the shipped
-- BusyTacticalFreeStore.lua by build_ccp_mod.py after a generated CCP_CFG table ====
-- CCPPROBE_SRC_MARK_V1
-- Question (docs/re/SKIRMISH_SETUP.md sec. 4.2): frames from the last completed capture of a Reinforcement_Point to the
-- battle end in a game type 4 (Capture Control Points) skirmish (port: 900 + 210), and the start owner of the map's
-- Reinforcement_Point objects. The exact frames come from the memory reader (ccp_reader.py: point owners, the mode's
-- domination timer +0x2b0..+0x2b8, the victory monitor); this script only makes the capture happen quickly and keeps a
-- 3-frame Lua view as a cross-check.
--
-- Arming: only the free store of the AI player whose faction is CCP_CFG.ai, in a land mode, and only while
-- Find_Player(CCP_CFG.human).Is_Human() is true (so the main-menu background battle never arms). Base_Definitions runs
-- before the engine sets PlayerObject (oi_abilities 2026-11-04), so the decision is retried from the service calls.
-- While armed: ServiceRate 0.05 s (the script runs on every free-store service, every 3rd frame) and the shipped
-- FreeStoreService / On_Unit_Service bodies are skipped (the AI's free units stay idle).
-- Steps: F >= enum_frame: list every Reinforcement_Point* object (relevant and faction-fixed types) with its owner (PT);
-- F >= start_frame: Spawn_Unit the CCP_CFG.squads for the human player on every relevant point (invulnerable), so the
-- human's units capture all of them through the capture service (Change_Owner would not arm the timer, sec. 4.2);
-- then every service: point owners (OWN on change, ALL when every relevant point is the human's), heartbeat HB.
--
-- Output channel: retail _CustomScriptMessage writes nothing, so every record is a Lua string
-- "CCP1|<KIND>|sid=<script id>|n=<n>|f=<mode frame>|...|END" kept in CCP_LOG; the reader finds them by a read-only heap
-- scan. f = GetCurrentTime() * 30 rounded. Lua 5.0: no '#', no math library (s78g), table.getn; every engine call is
-- pcall'd, but a failing engine call still stops a retail script silently (a2i 2026-11-06), so only calls with shipped
-- precedent and type names that exist in the FoC XML are used (the selftest checks the names).

CCP_LOG = {}
CCP_N = 0
CCP_SID = string.sub((string.gsub(tostring(CCP_LOG), "[^%w]", "")), -6)
CCP = { on = false, done = false, f = -1, tries = 0, pending = false, old_rate = nil, armed_once = false,
	enumerated = false, spawned = false, points = {}, human = nil, all_since = nil, hb = nil, step = "-" }
CCP_API_BAD = {}
CCP_MAX_RECORDS = 4000

if CCP_CFG == nil then
	CCP_CFG = { session = "?", map = "?", human = "Empire", ai = "REBEL", enum_frame = 30, start_frame = 150,
		stop_frame = 54000, hb_frames = 90, spawn = true, invulnerable = true,
		relevant = { "Reinforcement_Point" }, fixed = {}, squads = {}, players = { "Empire", "Rebel", "Neutral" } }
end

function CCP_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function CCP_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function CCP_Frame()
	local t = CCP_Now()
	if t == nil then
		return nil
	end
	return tonumber(string.format("%.0f", t * 30))
end

function CCP_Rec(kind, fields)
	pcall(function()
		if CCP_N >= CCP_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		CCP_N = CCP_N + 1
		local body = fields or ""
		if string.len(body) > 330 then
			body = string.sub(body, 1, 330) .. "~"
		end
		local f = CCP_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		CCP_LOG[CCP_N] = "CCP1|" .. kind .. "|sid=" .. CCP_SID .. "|n=" .. CCP_N .. "|f=" .. fs .. "|" .. body .. "|END"
	end)
end

function CCP_ApiFail(tag, err)
	if not CCP_API_BAD[tag] then
		CCP_API_BAD[tag] = true
		CCP_Rec("APIERR", "api=" .. CCP_Str(tag) .. "|err=" .. string.sub(CCP_Str(err), 1, 150))
	end
end

-- The step about to run, kept so a later record (or its absence) shows where a script stopped.
function CCP_Step(s)
	CCP.step = s
end

function CCP_Valid(o)
	if o == nil then
		return false
	end
	local ok, v = pcall(function() return o.Is_Valid() end)
	return ok and v == true
end

function CCP_Id(o)
	local id = nil
	pcall(function() id = o.Get_Object_ID() end)
	if id == nil then
		return "?"
	end
	return CCP_Str(id)
end

function CCP_XY(o)
	local ok, x, y = pcall(function()
		local a, b = o.Get_Position().Get_XYZ()
		if type(a) == "table" then
			return a[1], a[2]
		end
		return a, b
	end)
	if ok and type(x) == "number" and type(y) == "number" then
		return string.format("%.0f,%.0f", x, y)
	end
	return "?"
end

-- "<FACTION>:<player id>" of an object's owner ("?" parts when unreadable).
function CCP_Owner(o)
	local fac, id = "?", "?"
	pcall(function()
		local p = o.Get_Owner()
		pcall(function() fac = string.upper(tostring(p.Get_Faction_Name())) end)
		pcall(function() id = tostring(p.Get_ID()) end)
	end)
	return CCP_Str(fac) .. ":" .. CCP_Str(id)
end

function CCP_Player(name)
	local p = nil
	pcall(function() p = Find_Player(name) end)
	return p
end

-- ---- steps ------------------------------------------------------------------------------------------------------

function CCP_List(name, kind)
	CCP_Step("find:" .. name)
	local list = nil
	local ok, err = pcall(function() list = Find_All_Objects_Of_Type(name) end)
	if not ok then
		CCP_ApiFail("Find_All_Objects_Of_Type:" .. name, err)
		return 0
	end
	if type(list) ~= "table" then
		CCP_Rec("PTS", "type=" .. CCP_Str(name) .. "|cls=" .. kind .. "|count=nil")
		return 0
	end
	local n = table.getn(list)
	for i = 1, n do
		local o = list[i]
		if CCP_Valid(o) then
			local own = CCP_Owner(o)
			local id = CCP_Id(o)
			CCP_Rec("PT", "cls=" .. kind .. "|type=" .. CCP_Str(name) .. "|obj=" .. id .. "|owner=" .. own .. "|xy=" .. CCP_XY(o))
			if kind == "relevant" then
				table.insert(CCP.points, { obj = o, id = id, owner = own, type = name })
			end
		end
	end
	CCP_Rec("PTS", "type=" .. CCP_Str(name) .. "|cls=" .. kind .. "|count=" .. n)
	return n
end

function CCP_Enumerate()
	CCP.enumerated = true
	local pls = CCP_CFG.players or {}
	for i = 1, table.getn(pls) do
		local p = CCP_Player(pls[i])
		local id, fac, hum = "?", "?", "?"
		if p then
			pcall(function() id = tostring(p.Get_ID()) end)
			pcall(function() fac = tostring(p.Get_Faction_Name()) end)
			pcall(function() hum = tostring(p.Is_Human()) end)
		end
		CCP_Rec("PL", "name=" .. CCP_Str(pls[i]) .. "|id=" .. CCP_Str(id) .. "|faction=" .. CCP_Str(fac) .. "|human=" .. CCP_Str(hum))
	end
	local rel, fix = 0, 0
	for i = 1, table.getn(CCP_CFG.relevant) do
		rel = rel + CCP_List(CCP_CFG.relevant[i], "relevant")
	end
	for i = 1, table.getn(CCP_CFG.fixed or {}) do
		fix = fix + CCP_List(CCP_CFG.fixed[i], "fixed")
	end
	CCP_Rec("ENUM", "relevant=" .. rel .. "|fixed=" .. fix .. "|points=" .. table.getn(CCP.points))
end

function CCP_SpawnOne(pt, tname, k)
	CCP_Step("type:" .. tname)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if ty == nil then
		CCP_Rec("SPAWN", "pt=" .. pt.id .. "|type=" .. CCP_Str(tname) .. "|k=" .. k .. "|ok=0|why=no_type")
		return
	end
	CCP_Step("spawn:" .. tname)
	local r = nil
	local ok, err = pcall(function() r = Spawn_Unit(ty, pt.obj, CCP.human) end)
	if not ok then
		CCP_ApiFail("Spawn_Unit:" .. tname, err)
		CCP_Rec("SPAWN", "pt=" .. pt.id .. "|type=" .. CCP_Str(tname) .. "|k=" .. k .. "|ok=0|why=error")
		return
	end
	local objs, inv, first = 0, 0, nil
	if type(r) == "table" then
		for j = 1, table.getn(r) do
			local o = r[j]
			if CCP_Valid(o) then
				objs = objs + 1
				if first == nil then
					first = o
				end
				if CCP_CFG.invulnerable then
					CCP_Step("invulnerable:" .. tname)
					if pcall(function() o.Make_Invulnerable(true) end) then
						inv = inv + 1
					end
				end
			end
		end
	elseif CCP_Valid(r) then
		objs = 1
		first = r
		if CCP_CFG.invulnerable and pcall(function() r.Make_Invulnerable(true) end) then
			inv = 1
		end
	end
	local where = "?"
	if first then
		where = CCP_XY(first)
	end
	CCP_Rec("SPAWN", "pt=" .. pt.id .. "|type=" .. CCP_Str(tname) .. "|k=" .. k .. "|ok=" .. (objs > 0 and "1" or "0") ..
		"|objs=" .. objs .. "|inv=" .. inv .. "|owner=" .. (first and CCP_Owner(first) or "?") .. "|xy=" .. where)
end

function CCP_Spawn()
	CCP.spawned = true
	if not CCP_CFG.spawn then
		CCP_Rec("NOSPAWN", "why=cfg")
		return
	end
	if CCP.human == nil then
		CCP_Rec("NOSPAWN", "why=no_human_player")
		return
	end
	for i = 1, table.getn(CCP.points) do
		local pt = CCP.points[i]
		if CCP_Valid(pt.obj) then
			local sq = CCP_CFG.squads or {}
			for s = 1, table.getn(sq) do
				for k = 1, (sq[s].n or 1) do
					CCP_SpawnOne(pt, sq[s].type, k)
				end
			end
		end
	end
	CCP_Rec("SPAWNED", "points=" .. table.getn(CCP.points))
end

function CCP_Watch(F)
	local all, n, hum = true, 0, string.upper(CCP_CFG.human)
	local summary = ""
	for i = 1, table.getn(CCP.points) do
		local pt = CCP.points[i]
		CCP_Step("owner:" .. pt.id)
		local own = "gone"
		if CCP_Valid(pt.obj) then
			own = CCP_Owner(pt.obj)
		end
		if own ~= pt.owner then
			CCP_Rec("OWN", "pt=" .. pt.id .. "|from=" .. pt.owner .. "|to=" .. own)
			pt.owner = own
		end
		n = n + 1
		if string.sub(own, 1, string.len(hum) + 1) ~= hum .. ":" then
			all = false
		end
		if string.len(summary) < 200 then
			summary = summary .. pt.id .. "=" .. own .. ","
		end
	end
	if n > 0 and all and CCP.all_since == nil then
		CCP.all_since = F
		CCP_Rec("ALL", "owner=" .. CCP_Str(hum) .. "|points=" .. n)
	elseif not all and CCP.all_since ~= nil then
		CCP_Rec("LOST", "since=" .. CCP.all_since)
		CCP.all_since = nil
	end
	if CCP.hb == nil or F - CCP.hb >= (CCP_CFG.hb_frames or 90) then
		CCP.hb = F
		CCP_Rec("HB", "step=" .. CCP_Str(CCP.step) .. "|owners=" .. summary .. "|records=" .. CCP_N)
	end
end

function CCP_Finish(why)
	if CCP.done then
		return
	end
	CCP.done = true
	if CCP.old_rate ~= nil then
		ServiceRate = CCP.old_rate
	end
	local bad = ""
	for k, _ in pairs(CCP_API_BAD) do
		bad = bad .. k .. ","
	end
	CCP_Rec("DONE", "why=" .. why .. "|records=" .. CCP_N .. "|apierr=" .. string.sub(CCP_Str(bad), 1, 200))
end

function CCP_Run(F)
	if not CCP.enumerated and F >= (CCP_CFG.enum_frame or 30) then
		CCP_Enumerate()
	end
	if CCP.enumerated and not CCP.spawned and F >= (CCP_CFG.start_frame or 150) then
		CCP_Spawn()
	end
	CCP_Watch(F)
	if F >= (CCP_CFG.stop_frame or 54000) then
		CCP_Finish("stop_frame")
	end
end

-- ---- arming and the service drivers ----------------------------------------------------------------------------

function CCP_Arm()
	local fac, mode, me_human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() me_human = PlayerObject.Is_Human() end)
	if (fac == "?" or fac == "NIL") and CCP.tries < 200 then
		CCP.pending = true
		return
	end
	CCP.pending = false
	if mode ~= "Land" or fac ~= string.upper(CCP_CFG.ai) or me_human == true or CCP.armed_once then
		CCP_Rec("SKIP", "fac=" .. CCP_Str(fac) .. "|mode=" .. CCP_Str(mode) .. "|me_human=" .. tostring(me_human) ..
			"|armed_once=" .. tostring(CCP.armed_once) .. "|session=" .. CCP_Str(CCP_CFG.session))
		return
	end
	-- A kit with a human side arms only while that side is played by a human (cu8g t10): the main-menu background
	-- battle (two AI players) never arms.
	local hp = CCP_Player(CCP_CFG.human)
	local hh = false
	if hp then
		pcall(function() hh = hp.Is_Human() == true end)
	end
	if not hh then
		if CCP.tries < 60 then
			CCP.pending = true
			return
		end
		CCP_Rec("SKIP", "fac=" .. CCP_Str(fac) .. "|mode=" .. CCP_Str(mode) .. "|why=no_human_" .. CCP_Str(CCP_CFG.human) ..
			"|session=" .. CCP_Str(CCP_CFG.session))
		return
	end
	CCP.armed_once = true
	CCP.on = true
	CCP.human = hp
	CCP.old_rate = ServiceRate
	ServiceRate = 0.05
	local hid = "?"
	pcall(function() hid = tostring(hp.Get_ID()) end)
	CCP_Rec("ARM", "fac=" .. CCP_Str(fac) .. "|mode=" .. CCP_Str(mode) .. "|human=" .. CCP_Str(CCP_CFG.human) .. "|human_id=" ..
		CCP_Str(hid) .. "|map=" .. CCP_Str(CCP_CFG.map) .. "|session=" .. CCP_Str(CCP_CFG.session) .. "|rate=" ..
		CCP_Str(CCP.old_rate) .. "->0.05|tries=" .. CCP.tries)
end

function CCP_Tick(src)
	if CCP.pending then
		CCP.tries = CCP.tries + 1
		pcall(CCP_Arm)
	end
	if not CCP.on or CCP.done then
		return
	end
	local F = CCP_Frame()
	if F == nil or F == CCP.f then
		return
	end
	CCP.f = F
	local ok, err = pcall(CCP_Run, F)
	if not ok then
		CCP_Rec("ERR", "src=" .. src .. "|step=" .. CCP_Str(CCP.step) .. "|err=" .. string.sub(CCP_Str(err), 1, 200))
	end
end

function CCP_Quiet()
	return CCP.on and not CCP.done
end

CCP_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = nil
	if CCP_Orig_Base_Definitions then
		r = CCP_Orig_Base_Definitions()
	end
	pcall(CCP_Arm)
	return r
end

CCP_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(CCP_Tick, "fss")
	if CCP_Quiet() or CCP_Orig_FreeStoreService == nil then
		return
	end
	return CCP_Orig_FreeStoreService()
end

CCP_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(CCP_Tick, "ous")
	if CCP_Quiet() or CCP_Orig_On_Unit_Service == nil then
		return
	end
	return CCP_Orig_On_Unit_Service(object)
end

CCP_Rec("LOAD", "session=" .. CCP_Str(CCP_CFG.session) .. "|map=" .. CCP_Str(CCP_CFG.map) .. "|ai=" .. CCP_Str(CCP_CFG.ai) ..
	"|human=" .. CCP_Str(CCP_CFG.human))
-- ==== end of the ccpgu t02 probe ====
