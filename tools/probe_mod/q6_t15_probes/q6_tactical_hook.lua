-- ==== Q6 t15 probe: tactical spawner (tools/probe_mod/q6_t15_probes/q6_tactical_hook.lua) ====
-- Appended by build_q6_mod.py to the shipped BusyTacticalFreeStore.lua (FoC uses it for land and space). Runs inside
-- the first AI player's free store of a skirmish battle (the human's side has no free store). Two jobs:
--   OBJ  (OBJECT_SCRIPTS sec. 3.2 / sec. 8 item 1): spawns, for the AI player, groups of vanilla types that carry an
--        object script (their scripts are the copies with q6_objscript_hook.lua appended). Wave 1 at mode frame >= 150
--        (5 per type), wave 2 at frame >= 2700 (2 per type: objects created late). They get Prevent_AI_Usage,
--        Prevent_All_Fire and Make_Invulnerable so they stay alive and out of the AI's hands.
--   SEL  (COMMANDS sec. 2.4.1, Select All): spawns a test set for the HUMAN player next to the human's units: turrets,
--        base structures, a hero, an infantry company, a plain unit, a proximity mine (land); a star destroyer with
--        its hangar, a hero ship, the Eclipse (Not_Really_Selectable), a fighter squadron, a defense satellite
--        (space). The human's own starting units / station stay. Then it lists every object the human owns (OWN).
--        The selection itself is read from Gabriel's screenshots after Ctrl+A (no Lua query reads a selection).
-- Cadence: the hook only rides on the shipped calls and changes no rate: On_Unit_Service passes come every ~60 frames
-- from frame 63, FreeStoreService every 20 s from frame 600 (A1 probe, docs/re/probe_results/a1_tactical_20261031.txt).
-- PlayerObject is still nil inside Base_Definitions (AI_GALACTIC sec. 1, same A1 run), so the hook arms at its first
-- call after that. The AI's own behaviour is unchanged.
-- Records "Q6P|<KIND>|p=tact|sid=..|n=..|f=..|...|END" in Q6T_LOG (heap scan). Lua 5.0: table.getn, no '#'.

Q6T_LOG = {}
Q6T_N = 0
Q6T_SID = string.sub((string.gsub(tostring(Q6T_LOG), "[^%w]", "")), -6)
Q6T = { phase = "wait", f = -1, armed = false, mode = "?" }
Q6T_FACTIONS = { "Rebel", "Empire", "Underworld", "Pirates", "Hostile", "Neutral", "Hutts", "Sarlacc" }
Q6T_WAVE1_F = 150
Q6T_WAVE2_F = 2700
Q6T_OWN2_F = 3600
Q6T_GROUPS = {
	Land = { { "AT_AA_Walker", "ObjectScript_MissileShield" }, { "TIE_Crawler", "ObjectScript_SelfDestructLand" },
		{ "HAV_Juggernaut", "ObjectScript_PointDefense" } },
	Space = { { "Nebulon_B_Frigate", "ObjectScript_PowerToShields" }, { "MC30_Frigate", "ObjectScript_MC30" },
		{ "Crusader_Gunship", "ObjectScript_PointDefense" }, { "Interdictor_Cruiser", "Interdictor" } },
}
-- { type, how ("unit" = Spawn_Unit, "object" = Create_Generic_Object first), COMMANDS 2.4.1 expectation }
Q6T_SEL = {
	Land = { { "Empire_Anti_Infantry_Turret", "object", "IN" }, { "Empire_Anti_Vehicle_Turret", "object", "IN" },
		{ "E_Ground_Barracks", "object", "OUT" }, { "E_Ground_Turbolaser_Tower", "object", "OUT" },
		{ "Darth_Vader", "unit", "IN" }, { "Imperial_Stormtrooper_Squad", "unit", "IN" }, { "AT_ST_Walker", "unit", "IN" },
		{ "Proximity_Mine", "object", "OUT" } },
	Space = { { "Star_Destroyer", "unit", "IN" }, { "Accuser_Star_Destroyer", "unit", "IN" },
		{ "Eclipse_Super_Star_Destroyer", "unit", "IN" }, { "TIE_Fighter_Squadron", "unit", "IN" },
		{ "Empire_Defense_Satellite_Laser", "object", "OUT" } },
}

function Q6T_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function Q6T_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function Q6T_Rec(kind, fields)
	pcall(function()
		Q6T_N = Q6T_N + 1
		local body = fields or ""
		if string.len(body) > 300 then
			body = string.sub(body, 1, 300) .. "~"
		end
		Q6T_LOG[Q6T_N] = "Q6P|" .. kind .. "|p=tact|sid=" .. Q6T_SID .. "|n=" .. Q6T_N .. "|f=" .. tostring(Q6T.f) ..
			"|mode=" .. Q6T.mode .. "|" .. body .. "|END"
	end)
end

function Q6T_Say(id)
	Q6T_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

function Q6T_Id(o)
	local id = "?"
	pcall(function() id = tostring(o.Get_ID()) end)
	if id == "?" then
		pcall(function() id = tostring(o.Get_Object_ID()) end)
	end
	return Q6T_Str(id)
end

function Q6T_Type(o)
	local n = "?"
	pcall(function() n = o.Get_Type().Get_Name() end)
	return Q6T_Str(n)
end

function Q6T_XY(o)
	local s = "?"
	pcall(function()
		local a, b = o.Get_Position().Get_XYZ()
		if type(a) == "table" then
			a, b = a[1], a[2]
		end
		s = string.format("%.0f,%.0f", a, b)
	end)
	return s
end

function Q6T_Objects(player)
	local ok, list = pcall(Find_All_Objects_Of_Type, player)
	if ok and type(list) == "table" then
		return list
	end
	return {}
end

function Q6T_Offset(pos, dx, dy)
	local p = nil
	pcall(function()
		local x, y, z = pos.Get_XYZ()
		if type(x) == "table" then
			x, y, z = x[1], x[2], x[3]
		end
		p = Create_Position(x + dx, y + dy, z)
	end)
	return p or pos
end

function Q6T_Spawn(tname, pos, player, how)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if ty == nil then
		return nil, "no_type"
	end
	if how == "object" then
		local ok, o = pcall(Create_Generic_Object, ty, pos, player)
		if ok and o then
			return o, "generic"
		end
	end
	local ok, r = pcall(Spawn_Unit, ty, pos, player)
	if ok and type(r) == "table" and r[1] ~= nil then
		return r[1], "spawn_unit"
	end
	if ok and r ~= nil and type(r) ~= "table" then
		return r, "spawn_unit"
	end
	if how ~= "object" then
		local ok2, o = pcall(Create_Generic_Object, ty, pos, player)
		if ok2 and o then
			return o, "generic"
		end
	end
	return nil, "spawn_failed"
end

function Q6T_Calm(o)
	pcall(function() o.Prevent_AI_Usage(true) end)
	pcall(function() o.Prevent_All_Fire(true) end)
	pcall(function() o.Make_Invulnerable(true) end)
end

function Q6T_FindHuman()
	for _, name in ipairs(Q6T_FACTIONS) do
		local p = nil
		pcall(function() p = Find_Player(name) end)
		if p then
			local h = false
			pcall(function() h = p.Is_Human() end)
			if h then
				return p, name
			end
		end
	end
	return nil, "?"
end

function Q6T_Anchor(player)
	for _, o in ipairs(Q6T_Objects(player)) do
		local ok, pos = pcall(function() return o.Get_Position() end)
		if ok and pos then
			return o, pos
		end
	end
	return nil, nil
end

function Q6T_Wave(wave, per)
	local a, pos = Q6T_Anchor(PlayerObject)
	if pos == nil then
		Q6T_Rec("ERR", "what=no_ai_anchor|wave=" .. wave)
		return
	end
	local step = 120
	if Q6T.mode == "Space" then
		step = 450
	end
	for gi, g in ipairs(Q6T_GROUPS[Q6T.mode] or {}) do
		for k = 1, per do
			local o, how = Q6T_Spawn(g[1], Q6T_Offset(pos, (k - 1) * step, (gi + 1) * step), PlayerObject, "unit")
			if o then
				Q6T_Calm(o)
				Q6T_Rec("SPAWN", "wave=" .. wave .. "|type=" .. g[1] .. "|script=" .. g[2] .. "|k=" .. k .. "|id=" .. Q6T_Id(o) ..
					"|how=" .. how .. "|xy=" .. Q6T_XY(o))
			else
				Q6T_Rec("SPAWN", "wave=" .. wave .. "|type=" .. g[1] .. "|script=" .. g[2] .. "|k=" .. k .. "|id=-|how=" .. how)
			end
		end
	end
end

function Q6T_SelSet()
	local h, hname = Q6T.human, Q6T.human_name
	local a, pos = Q6T_Anchor(h)
	if pos == nil then
		Q6T_Rec("ERR", "what=no_human_anchor")
		return
	end
	local step = 150
	if Q6T.mode == "Space" then
		step = 600
	end
	for i, s in ipairs(Q6T_SEL[Q6T.mode] or {}) do
		local o, how = Q6T_Spawn(s[1], Q6T_Offset(pos, step * (i - 1) - step * 3, step), h, s[2])
		if o then
			pcall(function() o.Make_Invulnerable(true) end)
			Q6T_Rec("HSPAWN", "type=" .. s[1] .. "|expect=" .. s[3] .. "|id=" .. Q6T_Id(o) .. "|owner=" .. hname .. "|how=" ..
				how .. "|xy=" .. Q6T_XY(o))
		else
			Q6T_Rec("HSPAWN", "type=" .. s[1] .. "|expect=" .. s[3] .. "|id=-|how=" .. how)
		end
	end
	Q6T_Say("TEXT_Q6_SEL_READY_" .. string.upper(Q6T.mode))
end

-- Every object the human owns: type, id, parent type (a TEAM member or a docked / garrisoned unit has one).
function Q6T_Own(tag)
	local list = Q6T_Objects(Q6T.human)
	Q6T_Rec("OWNN", "tag=" .. tag .. "|count=" .. table.getn(list))
	local line, k = "", 0
	for i, o in ipairs(list) do
		local parent = "-"
		pcall(function()
			local p = o.Get_Parent_Object()
			if p then
				parent = Q6T_Type(p)
			end
		end)
		line = line .. Q6T_Type(o) .. ":" .. Q6T_Id(o) .. ":" .. parent .. ";"
		k = k + 1
		if k >= 6 or i == table.getn(list) then
			Q6T_Rec("OWN", "tag=" .. tag .. "|i=" .. i .. "|list=" .. line)
			line, k = "", 0
		end
	end
end

function Q6T_Step()
	if not Q6T.checked and PlayerObject ~= nil then
		Q6T.checked = true
		Q6T_Arm()
	end
	if not Q6T.armed then
		return
	end
	local t = Q6T_Now()
	if t == nil then
		return
	end
	local F = tonumber(string.format("%.0f", t * 30))
	if F == Q6T.f then
		return
	end
	Q6T.f = F
	if Q6T.phase == "wait" and F >= Q6T_WAVE1_F then
		Q6T.phase = "wave1"
		Q6T.human, Q6T.human_name = Q6T_FindHuman()
		Q6T_Rec("START", "ai=" .. Q6T_Str(Q6T.ai_fac) .. "|human=" .. Q6T.human_name)
		Q6T_Wave(1, 5)
		if Q6T.human then
			Q6T_SelSet()
		end
		Q6T_Say("TEXT_Q6_OBJ_WAVE1")
		Q6T.own1 = F + 300
	elseif Q6T.phase == "wave1" and Q6T.own1 and F >= Q6T.own1 then
		Q6T.own1 = nil
		if Q6T.human then
			Q6T_Own("after_spawn")
		end
	elseif Q6T.phase == "wave1" and F >= Q6T_WAVE2_F then
		Q6T.phase = "wave2"
		Q6T_Wave(2, 2)
	elseif Q6T.phase == "wave2" and F >= Q6T_OWN2_F then
		Q6T.phase = "done"
		if Q6T.human then
			Q6T_Own("later")
		end
		Q6T_Rec("DONE", "records=" .. Q6T_N)
		Q6T_Say("TEXT_Q6_TACT_DONE")
	end
end

function Q6T_Arm()
	local fac, mode, human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() human = PlayerObject.Is_Human() end)
	Q6T.mode = Q6T_Str(mode)
	if (mode == "Land" or mode == "Space") and not human and not Q6T_ARMED_ONCE then
		Q6T_ARMED_ONCE = true
		Q6T.armed = true
		Q6T.ai_fac = fac
		Q6T_Rec("ARM", "fac=" .. Q6T_Str(fac) .. "|rate=" .. Q6T_Str(ServiceRate) .. "|urate=" .. Q6T_Str(UnitServiceRate))
	else
		Q6T_Rec("SKIP", "fac=" .. Q6T_Str(fac) .. "|human=" .. tostring(human))
	end
end

Q6T_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(Q6T_Step)
	return Q6T_Orig_FreeStoreService()
end

Q6T_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(Q6T_Step)
	return Q6T_Orig_On_Unit_Service(object)
end

Q6T_Rec("LOAD", "groups_land=" .. table.getn(Q6T_GROUPS.Land) .. "|groups_space=" .. table.getn(Q6T_GROUPS.Space))
-- ==== end of the Q6 t15 tactical spawner ====
