-- ==== OI ability probe (tools/probe_mod/oi_abilities/oi_hook.lua), appended to the shipped
-- BusyTacticalFreeStore.lua by build_oi_mod.py after a generated OIP_CFG table (spawn points of the map) ====
-- OIPROBE_SRC_MARK_V1
-- Questions (docs/re/ABILITIES_HARDPOINTS.md): sec. 4.12 does a stunned MAL_Rocket_Vehicle die over water (at once, at
-- the stun end, only on deep water, never)? sec. 4.13 sim frames of the Rancor pound / eat events, the Infiltrator
-- grenade spawn and toss end, and Yoda's Berserker damage passes.
--
-- Only the free store of the AI player whose faction is OIP_CFG.faction, in a land battle, runs the scenarios.
-- That instance lowers ServiceRate to 0.05 s so the engine resumes the script on every free-store service (every
-- 3rd frame, AI_TACTICAL sec. 5.3) instead of every 20 s, and while the probe runs it skips the shipped
-- FreeStoreService / On_Unit_Service bodies (the AI's free units stay idle). Engine threads (Create_Thread) are
-- resumed at that same service, so the scenarios are a frame-timed state machine driven from FreeStoreService
-- (On_Unit_Service as a fallback driver) instead of a thread: no yield across pcall (Lua 5.0), every step pcall'd.
-- The exact frames come from the memory reader (oi_reader.py); these records give the scenario timeline, what was
-- spawned where, which calls failed and a 3-frame Lua view of hull / validity / position / ability flags.
--
-- Output channel (same as the A1 probe): retail _CustomScriptMessage writes nothing, so every record is a Lua
-- string "OIP1|<KIND>|r=<run>|f=<mode frame>|...|END" kept in OIP_LOG; oi_reader.py finds them by a read-only heap
-- scan. f = GetCurrentTime() * 30 rounded (mode frame; A1 run 2026-10-31: t = F / 30).
-- Kinds: LOAD, ARM / SKIP, TICK (first resumes: cadence), HB (heartbeat), SCN (start / end), SPAWN, SETUP, ACT
-- (ability call), FIRED (ready -> not ready or active), ORDER, P (3-frame poll, on change), APIERR, ERR, DONE.
-- Lua 5.0: no '#', no math library assumptions, varargs via 'arg', table.getn.

OIP_LOG = {}
OIP_SID = "?"
OIP_RUN = 0
OIP_ON = false
OIP_DONE = false
OIP_LAST_F = nil
OIP_TICKS = 0
OIP_HB_NEXT = 100
OIP_SCN_I = 0
OIP_CUR = nil
OIP_KEEP = {}
OIP_ANCHOR = nil
OIP_API_BAD = {}
OIP_MAX_RECORDS = 20000
OIP_VICTIM_PLAYER = nil
OIP_VICTIM_FAC = "?"

if OIP_CFG == nil then
	OIP_CFG = { map = "?", faction = "REBEL", start_frame = 450, points_ok = false, pts = {} }
end

function OIP_Clean(v)
	local s = tostring(v)
	s = string.gsub(s, "[^%w%._%-:]", "_")
	return s
end

function OIP_Time()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function OIP_Frame()
	local t = OIP_Time()
	if t == nil then
		return nil
	end
	return tonumber(string.format("%.0f", t * 30))
end

function OIP_Rec(kind, fields)
	pcall(function()
		if table.getn(OIP_LOG) >= OIP_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		local f = OIP_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		local body = fields or ""
		if string.len(body) > 330 then
			body = string.sub(body, 1, 330) .. "~"
		end
		table.insert(OIP_LOG, "OIP1|" .. kind .. "|r=" .. OIP_SID .. "." .. OIP_RUN .. "|f=" .. fs .. "|" .. body .. "|END")
	end)
end

function OIP_ScnName()
	if OIP_CUR then
		return OIP_CUR.name
	end
	return "-"
end

function OIP_ApiFail(tag, err)
	local key = OIP_ScnName() .. ":" .. tag
	if not OIP_API_BAD[key] then
		OIP_API_BAD[key] = true
		OIP_Rec("APIERR", "scn=" .. OIP_ScnName() .. "|api=" .. OIP_Clean(tag) .. "|err=" ..
			string.sub(OIP_Clean(err), 1, 150))
	end
end

-- pcall wrapper: returns up to 3 results, nil on error (and records the failing API once per scenario).
function OIP_Try(tag, f)
	local ok, a, b, c = pcall(f)
	if not ok then
		OIP_ApiFail(tag, a)
		return nil
	end
	return a, b, c
end

function OIP_Num(v, fmt)
	if type(v) == "number" then
		return string.format(fmt or "%.0f", v)
	end
	return "?"
end

function OIP_Valid(obj)
	if obj == nil then
		return false
	end
	local ok, v = pcall(function() return obj.Is_Valid() end)
	return ok and v == true
end

function OIP_XYZ(obj)
	local ok, x, y, z = pcall(function()
		local p = obj.Get_Position()
		local a, b, c = p.Get_XYZ()
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

function OIP_OwnerId(obj)
	local ok, v = pcall(function() return obj.Get_Owner().Get_ID() end)
	if ok then
		return tostring(v)
	end
	return "?"
end

-- A spawn point: config point + offset (Create_Position), else the fallback anchor (an AI unit).
function OIP_Where(name, dx, dy)
	local p = OIP_CFG.pts[name]
	if OIP_CFG.points_ok and p then
		local pos = OIP_Try("Create_Position", function() return Create_Position(p.x + (dx or 0), p.y + (dy or 0), p.z) end)
		if pos then
			return pos, "cfg"
		end
	end
	if OIP_Valid(OIP_ANCHOR) then
		return OIP_ANCHOR, "anchor"
	end
	return nil, "none"
end

function OIP_WhereNear(obj, dx, dy)
	local x, y, z = OIP_XYZ(obj)
	if type(x) == "number" and type(y) == "number" then
		local pos = OIP_Try("Create_Position", function() return Create_Position(x + dx, y + dy, z) end)
		if pos then
			return pos, "near"
		end
	end
	return obj, "obj"
end

function OIP_Spawn(tag, tname, where, how, player, generic)
	local s = OIP_CUR
	local t = OIP_Try("Find_Object_Type:" .. tname, function() return Find_Object_Type(tname) end)
	if t == nil or where == nil then
		OIP_Rec("SPAWN", "scn=" .. s.name .. "|tag=" .. tag .. "|type=" .. tname .. "|ok=0|why=" ..
			((t == nil) and "no_type" or "no_position"))
		return nil
	end
	local r = nil
	if generic then
		-- Create_Generic_Object places at the position; Spawn_Unit moves land units off water cells (retail 2026-11-04)
		r = OIP_Try("Create_Generic_Object:" .. tname, function() return Create_Generic_Object(t, where, player) end)
		if OIP_Valid(r) then
			how = how .. "+generic"
		else
			r = nil
		end
	end
	if r == nil then
		r = OIP_Try("Spawn_Unit:" .. tname, function() return Spawn_Unit(t, where, player) end)
	end
	local obj = r
	if type(r) == "table" and r[1] ~= nil then
		obj = r[1]
	end
	if not OIP_Valid(obj) then
		OIP_Rec("SPAWN", "scn=" .. s.name .. "|tag=" .. tag .. "|type=" .. tname .. "|ok=0|why=spawn_failed")
		return nil
	end
	s.objs[tag] = obj
	table.insert(s.order, tag)
	table.insert(s.spawned, obj)
	local x, y, z = OIP_XYZ(obj)
	OIP_Rec("SPAWN", "scn=" .. s.name .. "|tag=" .. tag .. "|type=" .. tname .. "|ok=1|how=" .. how ..
		"|owner=" .. OIP_OwnerId(obj) .. "|x=" .. OIP_Num(x, "%.1f") .. "|y=" .. OIP_Num(y, "%.1f") ..
		"|z=" .. OIP_Num(z, "%.1f"))
	return obj
end

function OIP_Setup(tag, obj, calls)
	local s = ""
	for i, c in ipairs(calls) do
		local name, val = c[1], c[2]
		local ok = pcall(function() obj[name](val) end)
		if not ok then
			OIP_ApiFail(name, "call failed")
		end
		s = s .. name .. "=" .. (ok and "1" or "0") .. ","
	end
	OIP_Rec("SETUP", "scn=" .. OIP_ScnName() .. "|tag=" .. tag .. "|" .. s)
end

function OIP_Attacker(tag, obj)
	if obj then
		OIP_Setup(tag, obj, { { "Make_Invulnerable", true }, { "Prevent_AI_Usage", true } })
	end
end

function OIP_Victim(tag, obj, hold)
	if obj then
		local calls = { { "Prevent_All_Fire", true }, { "Prevent_AI_Usage", true } }
		if hold then
			table.insert(calls, { "Suspend_Locomotor", true })
		end
		OIP_Setup(tag, obj, calls)
	end
end

function OIP_Flag(obj, method, ability)
	local ok, v = pcall(function() return obj[method](ability) end)
	if ok then
		if v then
			return "1"
		end
		return "0"
	end
	return "?"
end

-- Activate_Ability with one argument kind; records the call and the ready / active flags around it.
function OIP_Act(tag, ability, argkind, arg)
	local s = OIP_CUR
	local obj = s.objs[tag]
	if not OIP_Valid(obj) then
		OIP_Rec("ACT", "scn=" .. s.name .. "|tag=" .. tag .. "|ab=" .. ability .. "|arg=" .. argkind .. "|call=no_unit")
		return
	end
	local rb = OIP_Flag(obj, "Is_Ability_Ready", ability)
	local ok, err = pcall(function()
		if argkind == "none" then
			return obj.Activate_Ability(ability)
		end
		return obj.Activate_Ability(ability, arg)
	end)
	if not ok then
		OIP_ApiFail("Activate_Ability:" .. ability .. ":" .. argkind, err)
	end
	OIP_Rec("ACT", "scn=" .. s.name .. "|tag=" .. tag .. "|ab=" .. ability .. "|arg=" .. argkind .. "|call=" ..
		(ok and "ok" or "error") .. "|ready_before=" .. rb .. "|ready_after=" .. OIP_Flag(obj, "Is_Ability_Ready", ability) ..
		"|active_after=" .. OIP_Flag(obj, "Is_Ability_Active", ability))
end

-- One activation attempt per call until the ability is seen fired (s.fired[tag]); the argument kinds are tried in
-- order because the unit-ability type families take different arguments (ABILITIES_HARDPOINTS sec. 2.9).
function OIP_TryAct(tag, ability, target, n)
	local s = OIP_CUR
	if s.fired[tag] then
		return
	end
	local kinds = s.argkinds[tag] or { "object", "position", "none", "bool" }
	local k = kinds[n]
	if k == nil then
		return
	end
	local arg = nil
	if k == "object" then
		arg = target
	elseif k == "position" then
		arg = OIP_Try("Get_Position", function() return target.Get_Position() end)
	elseif k == "bool" then
		arg = true
	end
	OIP_Act(tag, ability, k, arg)
end

function OIP_Order(tag, what, f)
	local ok = pcall(f)
	OIP_Rec("ORDER", "scn=" .. OIP_ScnName() .. "|tag=" .. tag .. "|order=" .. what .. "|ok=" .. (ok and "1" or "0"))
end

function OIP_CountType(tname)
	local ok, n = pcall(function()
		local l = Find_All_Objects_Of_Type(tname)
		if type(l) == "table" then
			return table.getn(l)
		end
		return 0
	end)
	if ok then
		return tostring(n)
	end
	return "?"
end

-- ---- per-tick poll ------------------------------------------------------------------------------------------------

function OIP_Poll()
	local s = OIP_CUR
	if s == nil then
		return
	end
	local parts = ""
	for i, tag in ipairs(s.order) do
		local obj = s.objs[tag]
		local v = OIP_Valid(obj)
		local e = tag .. "="
		if v then
			local okh, h = pcall(function() return obj.Get_Hull() end)
			local x, y = OIP_XYZ(obj)
			e = e .. "1:" .. ((okh and type(h) == "number") and string.format("%.3f", h) or "?") .. ":" ..
				OIP_Num(x) .. ":" .. OIP_Num(y)
			local ab = s.abilities[tag]
			if ab then
				local a = OIP_Flag(obj, "Is_Ability_Active", ab)
				local r = OIP_Flag(obj, "Is_Ability_Ready", ab)
				e = e .. ":" .. a .. ":" .. r
				local F = OIP_Frame()
				if not s.fired[tag] then
					if a == "1" or (s.ready_seen[tag] and r == "0") then
						s.fired[tag] = F
						OIP_Rec("FIRED", "scn=" .. s.name .. "|tag=" .. tag .. "|ab=" .. ab .. "|active=" .. a .. "|ready=" .. r)
					end
				end
				if r == "1" then
					s.ready_seen[tag] = true
				end
			end
		else
			e = e .. "0"
		end
		parts = parts .. e .. ";"
	end
	if s.proj then
		parts = parts .. "proj=" .. OIP_CountType(s.proj) .. ";"
	end
	if parts ~= s.last_poll then
		s.last_poll = parts
		OIP_Rec("P", "scn=" .. s.name .. "|" .. parts)
	end
end

-- ---- scenarios ----------------------------------------------------------------------------------------------------
-- Each scenario: name, dur (frames), steps = { { at = rel frame | nil, when = fn(s) | nil, run = fn(s) } }, run once
-- each, in order of readiness. Attackers belong to the probe's AI player (invulnerable, kept from AI use); victims to
-- Hostile (no fire, held in place unless they must move).

-- MAL at its point: Spawn_Unit moves a land unit off water cells (retail run 2026-11-04: the "shallow" and "deep"
-- MALs appeared on the shore, 88 and 80 units away), so the MAL (a Hover vehicle) is spawned, then driven onto the
-- configured point. Han comes once it is there (or after 400 frames), within 120 of it: the stun blast is centred on
-- Han with Projectile_Stun_Radius 150 (the 2026-11-04 land Han stood 180 away and missed).
function OIP_MalDist2(s, mal)
	local p = OIP_CFG.pts[mal]
	local x, y = OIP_XYZ(s.objs.mal)
	if p == nil or type(x) ~= "number" or type(y) ~= "number" then
		return nil
	end
	return (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y)
end

function OIP_MalScenario(kind)
	local mal, han = "mal_" .. kind, "han_" .. kind
	local function after(k)
		return function(s) return s.han_at and OIP_Frame() >= s.han_at + k end
	end
	return {
		name = "MAL_" .. string.upper(kind),
		dur = 1300,
		abilities = { han = "AREA_EFFECT_STUN" },
		argkinds = { han = { "position", "object", "none", "bool" } },
		proj = "Proj_Special_Han_Solo_Stun_Blast",
		point = mal,
		steps = {
			{ at = 0, run = function(s)
				local w, how = OIP_Where(mal, 0, 0)
				local m = OIP_Spawn("mal", "MAL_Rocket_Vehicle", w, how, OIP_VICTIM_PLAYER, true)
				OIP_Victim("mal", m, false)
				local d = OIP_CFG.pts[mal]
				OIP_Rec("WATER", "scn=" .. s.name .. "|depth_cfg=" .. OIP_Num(d and d.depth, "%.2f") ..
					"|water_height=" .. OIP_Num(OIP_CFG.water_height, "%.2f"))
			end },
			{ at = 3, run = function(s)
				local d2 = OIP_MalDist2(s, mal)
				if d2 ~= nil and d2 > 225 then
					local w = OIP_Where(mal, 0, 0)
					OIP_Order("mal", "drive_to_point", function() s.objs.mal.Move_To(w) end)
				end
			end },
			{ at = 6, when = function(s)
				if OIP_Frame() >= s.f0 + 400 then
					return true
				end
				local d2 = OIP_MalDist2(s, mal)
				return d2 ~= nil and d2 <= 225
			end, run = function(s)
				local x, y, z = OIP_XYZ(s.objs.mal)
				OIP_Rec("MALPOS", "scn=" .. s.name .. "|dist2=" .. OIP_Num(OIP_MalDist2(s, mal), "%.0f") .. "|x=" ..
					OIP_Num(x, "%.1f") .. "|y=" .. OIP_Num(y, "%.1f") .. "|z=" .. OIP_Num(z, "%.1f"))
				local w, how = OIP_Where(han, 0, 0)
				local p = OIP_CFG.pts[han]
				if p == nil or type(x) ~= "number" or (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) > 120 * 120 then
					w, how = OIP_WhereNear(s.objs.mal, 60, 0)
				end
				local h = OIP_Spawn("han", "Han_Solo", w, how, PlayerObject)
				if h == nil then
					h = OIP_Spawn("han", "Han_Solo_Team", w, how, PlayerObject)
				end
				OIP_Attacker("han", h)
				if h then
					OIP_Setup("han", h, { { "Prevent_All_Fire", true } })
				end
				s.han_at = OIP_Frame()
			end },
			{ when = after(42), run = function(s) OIP_TryAct("han", "AREA_EFFECT_STUN", s.objs.mal, 1) end },
			{ when = after(72), run = function(s) OIP_TryAct("han", "AREA_EFFECT_STUN", s.objs.mal, 2) end },
			{ when = after(102), run = function(s) OIP_TryAct("han", "AREA_EFFECT_STUN", s.objs.mal, 3) end },
			{ when = after(132), run = function(s) OIP_TryAct("han", "AREA_EFFECT_STUN", s.objs.mal, 4) end },
			-- a stunned unit cannot move: order the MAL toward Han after the stun and again after its end (600 frames)
			{ when = function(s) return s.fired.han and OIP_Frame() >= s.fired.han + 30 end, run = function(s)
				OIP_Order("mal", "move_to_han", function() s.objs.mal.Move_To(s.objs.han) end)
			end },
			{ when = function(s) return s.fired.han and OIP_Frame() >= s.fired.han + 330 end, run = function(s)
				OIP_Order("mal", "move_to_han", function() s.objs.mal.Move_To(s.objs.han) end)
			end },
			{ when = function(s) return s.fired.han and OIP_Frame() >= s.fired.han + 615 end, run = function(s)
				OIP_Order("mal", "move_to_han", function() s.objs.mal.Move_To(s.objs.han) end)
			end },
		},
	}
end

function OIP_RancorPound()
	return {
		name = "RANCOR_POUND",
		dur = 600,
		abilities = {},
		point = "arena1",
		keep = { rancor = true },
		steps = {
			{ at = 0, run = function(s)
				local w, how = OIP_Where("arena1", 0, 0)
				local r = OIP_Spawn("rancor", "Rancor", w, how, PlayerObject)
				OIP_Attacker("rancor", r)
				local w2, how2 = OIP_Where("arena1", 40, 0)
				local t = OIP_Spawn("tank", "T4B_Tank", w2, how2, OIP_VICTIM_PLAYER)
				OIP_Victim("tank", t, true)
			end },
			{ at = 15, run = function(s)
				OIP_Order("rancor", "attack_tank", function() s.objs.rancor.Attack_Target(s.objs.tank) end)
			end },
			{ at = 300, run = function(s)
				if OIP_Valid(s.objs.tank) then
					OIP_Order("rancor", "attack_tank", function() s.objs.rancor.Attack_Target(s.objs.tank) end)
				end
			end },
		},
	}
end

function OIP_EatStep(n, dx, dy)
	return { at = (n - 1) * 300, run = function(s)
		local r = s.objs.rancor
		if not OIP_Valid(r) then
			OIP_Rec("ERR", "scn=" .. s.name .. "|msg=no_rancor")
			return
		end
		local w, how = OIP_WhereNear(r, dx, dy)
		local tag = "trooper" .. n
		local v = OIP_Spawn(tag, "Stormtrooper", w, how, OIP_VICTIM_PLAYER)
		OIP_Victim(tag, v, true)
		if v then
			OIP_Order("rancor", "attack_" .. tag, function() r.Attack_Target(v) end)
		end
	end }
end

function OIP_RancorEat()
	return {
		name = "RANCOR_EAT",
		dur = 900,
		abilities = {},
		point = "arena1",
		inherit = { "rancor" },
		steps = { OIP_EatStep(1, 0, 47), OIP_EatStep(2, -47, 0), OIP_EatStep(3, 0, -47) },
	}
end

function OIP_Grenade()
	return {
		name = "GRENADE",
		dur = 600,
		abilities = { infiltrator = "STICKY_BOMB" },
		argkinds = { infiltrator = { "object", "position", "none" } },
		proj = "Proj_Sticky_Bomb",
		point = "arena2",
		steps = {
			{ at = 0, run = function(s)
				local w, how = OIP_Where("arena2", 0, 0)
				local i = OIP_Spawn("infiltrator", "Rebel_Infiltrator", w, how, PlayerObject)
				OIP_Attacker("infiltrator", i)
				local w2, how2 = OIP_Where("arena2", 55, 0)
				local t = OIP_Spawn("tank", "T4B_Tank", w2, how2, OIP_VICTIM_PLAYER)
				OIP_Victim("tank", t, true)
			end },
			{ at = 30, run = function(s) OIP_TryAct("infiltrator", "STICKY_BOMB", s.objs.tank, 1) end },
			{ at = 60, run = function(s) OIP_TryAct("infiltrator", "STICKY_BOMB", s.objs.tank, 2) end },
			{ at = 90, run = function(s) OIP_TryAct("infiltrator", "STICKY_BOMB", s.objs.tank, 3) end },
		},
	}
end

function OIP_Berserker()
	return {
		name = "BERSERKER",
		dur = 450,
		abilities = { yoda = "BERSERKER" },
		argkinds = { yoda = { "object", "position", "none" } },
		point = "arena3",
		steps = {
			{ at = 0, run = function(s)
				local w, how = OIP_Where("arena3", 0, 0)
				local y = OIP_Spawn("yoda", "Yoda", w, how, PlayerObject)
				if y == nil then
					y = OIP_Spawn("yoda", "Yoda_Team", w, how, PlayerObject)
				end
				OIP_Attacker("yoda", y)
				local w2, how2 = OIP_Where("arena3", 60, 0)
				local t = OIP_Spawn("tank", "T4B_Tank", w2, how2, OIP_VICTIM_PLAYER)
				OIP_Victim("tank", t, true)
				local w3, how3 = OIP_Where("arena3", 60, 30)
				local t2 = OIP_Spawn("tank2", "T4B_Tank", w3, how3, OIP_VICTIM_PLAYER)
				OIP_Victim("tank2", t2, true)
			end },
			{ at = 30, run = function(s) OIP_TryAct("yoda", "BERSERKER", s.objs.tank, 1) end },
			{ at = 45, run = function(s) OIP_TryAct("yoda", "BERSERKER", s.objs.tank, 2) end },
			{ at = 60, run = function(s) OIP_TryAct("yoda", "BERSERKER", s.objs.tank, 3) end },
		},
	}
end

function OIP_Scenarios()
	return { OIP_MalScenario("land"), OIP_MalScenario("shallow"), OIP_MalScenario("deep"), OIP_RancorPound(),
		OIP_RancorEat(), OIP_Grenade(), OIP_Berserker() }
end

-- ---- driver -------------------------------------------------------------------------------------------------------

function OIP_StartScenario(def, F)
	def.f0 = F
	def.objs = {}
	def.order = {}
	def.spawned = {}
	def.fired = {}
	def.ready_seen = {}
	def.done = {}
	def.last_poll = nil
	def.abilities = def.abilities or {}
	if def.inherit then
		for i, tag in ipairs(def.inherit) do
			if OIP_KEEP[tag] then
				def.objs[tag] = OIP_KEEP[tag]
				table.insert(def.order, tag)
				table.insert(def.spawned, OIP_KEEP[tag])
				OIP_KEEP[tag] = nil
			end
		end
	end
	OIP_CUR = def
	local p = OIP_CFG.pts[def.point or ""] or {}
	OIP_Rec("SCN", "scn=" .. def.name .. "|ev=start|i=" .. OIP_SCN_I .. "|dur=" .. def.dur .. "|point=" ..
		tostring(def.point) .. "|x=" .. OIP_Num(p.x) .. "|y=" .. OIP_Num(p.y))
end

function OIP_EndScenario(why)
	local s = OIP_CUR
	pcall(OIP_Poll)
	local kept = ""
	for tag, obj in pairs(s.objs) do
		if s.keep and s.keep[tag] and OIP_Valid(obj) then
			OIP_KEEP[tag] = obj
			kept = kept .. tag .. ","
		end
	end
	local gone = 0
	for i, obj in ipairs(s.spawned) do
		local keepit = false
		for tag, kobj in pairs(OIP_KEEP) do
			if kobj == obj then
				keepit = true
			end
		end
		if not keepit and OIP_Valid(obj) then
			if pcall(function() obj.Despawn() end) then
				gone = gone + 1
			end
		end
	end
	OIP_Rec("SCN", "scn=" .. s.name .. "|ev=end|why=" .. why .. "|despawned=" .. gone .. "|kept=" .. kept)
	OIP_CUR = nil
end

function OIP_Step(F)
	local s = OIP_CUR
	local rel = F - s.f0
	for i, st in ipairs(s.steps) do
		if not s.done[i] then
			local ready = (st.at == nil or rel >= st.at)
			if ready and st.when then
				local ok, v = pcall(st.when, s)
				ready = ok and v
			end
			if ready then
				s.done[i] = true
				local ok, err = pcall(st.run, s)
				if not ok then
					OIP_Rec("ERR", "scn=" .. s.name .. "|step=" .. i .. "|msg=" .. string.sub(OIP_Clean(err), 1, 200))
				end
			end
		end
	end
	pcall(OIP_Poll)
	if rel >= s.dur then
		OIP_EndScenario("time")
	end
end

function OIP_Finish()
	OIP_DONE = true
	OIP_ON = false
	ServiceRate = 20
	local bad = ""
	for k, v in pairs(OIP_API_BAD) do
		bad = bad .. k .. ","
	end
	OIP_Rec("DONE", "scenarios=" .. OIP_SCN_I .. "|apierr=" .. string.sub(OIP_Clean(bad), 1, 250))
end

function OIP_Tick(src)
	if not OIP_ON or OIP_DONE then
		return
	end
	local F = OIP_Frame()
	if F == nil or F == OIP_LAST_F then
		return
	end
	OIP_LAST_F = F
	OIP_TICKS = OIP_TICKS + 1
	if OIP_TICKS <= 12 then
		OIP_Rec("TICK", "n=" .. OIP_TICKS .. "|src=" .. src)
	elseif OIP_TICKS >= OIP_HB_NEXT then
		OIP_HB_NEXT = OIP_TICKS + 100
		OIP_Rec("HB", "n=" .. OIP_TICKS .. "|src=" .. src .. "|scn=" .. OIP_ScnName() .. "|records=" .. table.getn(OIP_LOG))
	end
	if F < OIP_CFG.start_frame then
		return
	end
	if OIP_CUR == nil then
		if OIP_VICTIM_PLAYER == nil then
			OIP_FindVictimPlayer()
		end
		local list = OIP_Scenarios()
		OIP_SCN_I = OIP_SCN_I + 1
		local def = list[OIP_SCN_I]
		if def == nil then
			OIP_Finish()
			return
		end
		OIP_StartScenario(def, F)
	end
	OIP_Step(F)
end

function OIP_FindVictimPlayer()
	local names = { "Hostile", "Empire", "Underworld", "Pirates" }
	for i, n in ipairs(names) do
		if string.upper(n) ~= OIP_CFG.faction then
			local p = OIP_Try("Find_Player:" .. n, function() return Find_Player(n) end)
			if p then
				OIP_VICTIM_PLAYER = p
				OIP_VICTIM_FAC = n
				break
			end
		end
	end
	local id = "?"
	if OIP_VICTIM_PLAYER then
		local ok, v = pcall(function() return OIP_VICTIM_PLAYER.Get_ID() end)
		if ok then
			id = tostring(v)
		end
	end
	OIP_Rec("VICTIMS", "faction=" .. OIP_VICTIM_FAC .. "|id=" .. id)
end

OIP_ARM_PENDING = false
OIP_ARM_TRIES = 0

function OIP_ArmRetry()
	if OIP_ARM_PENDING then
		OIP_ARM_TRIES = OIP_ARM_TRIES + 1
		pcall(OIP_Arm)
	end
end

function OIP_Arm()
	local fac = "?"
	local mode = "?"
	local pid = "?"
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() pid = tostring(PlayerObject.Get_ID()) end)
	OIP_ON = false
	-- Base_Definitions runs at script load, before the engine sets PlayerObject (retail run 2026-11-04 recorded
	-- SKIP pid=? fac=?). Undecided until the faction is readable: retried from the first service calls.
	if fac == "?" and OIP_ARM_TRIES < 200 then
		OIP_ARM_PENDING = true
		return
	end
	OIP_ARM_PENDING = false
	if fac == OIP_CFG.faction and mode == "Land" then
		OIP_RUN = OIP_RUN + 1
		OIP_ON = true
		OIP_DONE = false
		OIP_LAST_F = nil
		OIP_TICKS = 0
		OIP_HB_NEXT = 100
		OIP_SCN_I = 0
		OIP_CUR = nil
		OIP_KEEP = {}
		OIP_API_BAD = {}
		OIP_VICTIM_PLAYER = nil
		local old = tostring(ServiceRate)
		ServiceRate = 0.05
		OIP_Rec("ARM", "pid=" .. pid .. "|fac=" .. fac .. "|mode=" .. mode .. "|map=" .. OIP_CFG.map .. "|points_ok=" ..
			tostring(OIP_CFG.points_ok) .. "|service_rate=" .. old .. "->0.05|create_thread=" .. type(Create_Thread) ..
			"|create_position=" .. type(Create_Position) .. "|spawn_unit=" .. type(Spawn_Unit))
	else
		OIP_Rec("SKIP", "pid=" .. pid .. "|fac=" .. fac .. "|mode=" .. mode)
	end
end

OIP_SID = OIP_Clean(string.gsub(tostring(OIP_LOG), "^table: ", ""))
OIP_SID = string.sub(OIP_SID, -6)

OIP_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = OIP_Orig_Base_Definitions()
	pcall(OIP_Arm)
	return r
end

OIP_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	OIP_ArmRetry()
	if OIP_ON then
		pcall(OIP_Tick, "fss")
		return
	end
	return OIP_Orig_FreeStoreService()
end

OIP_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	OIP_ArmRetry()
	if OIP_ON then
		if OIP_ANCHOR == nil and OIP_Valid(object) then
			OIP_ANCHOR = object
		end
		pcall(OIP_Tick, "ous")
		return
	end
	return OIP_Orig_On_Unit_Service(object)
end

OIP_Orig_On_Unit_Added = On_Unit_Added
function On_Unit_Added(object)
	if OIP_ANCHOR == nil and OIP_Valid(object) then
		OIP_ANCHOR = object
	end
	return OIP_Orig_On_Unit_Added(object)
end

OIP_Rec("LOAD", "map=" .. OIP_CFG.map .. "|faction=" .. OIP_CFG.faction .. "|points_ok=" .. tostring(OIP_CFG.points_ok))
-- ==== end of the OI ability probe ====
