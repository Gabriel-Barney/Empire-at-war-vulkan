-- ==== Q6 t15 probe: TR land end time (tools/probe_mod/q6_t15_probes/q6_tr_land_hook.lua) ====
-- Appended by build_q6_mod.py to Thrawn's Revenge's own LandTacticalFreeStore.lua (TR's AI players use it for land,
-- AI_TACTICAL sec. 7.7.6) in the overlay mod Mods\Q6T15ProbeTR, which is loaded in front of TR
-- (MODPATH=Mods\Q6T15ProbeTR MODPATH=<TR>). Read-only: it gives no orders, spawns nothing and leaves ServiceRate /
-- UnitServiceRate as TR sets them, so the AI plays exactly as in plain TR. It only looks at the battle from the AI's
-- free-store calls (FreeStoreService every 20 s, On_Unit_Service every 2 s per idle unit); it arms at its first call
-- with PlayerObject set (PlayerObject is still nil inside Base_Definitions):
--   every >= 150 frames (5 s): for every other player's objects (the human's side), the structures, and each command
--   centre (type name contains COMMAND_CENTER, Is_Category Structure): hull, how many of this AI's units are within
--   600 and 2884 of it, the nearest one.
--   GONE when an enemy structure disappears (type, id, frame), BASEGONE when the enemy has no structure left but a
--   command centre, CCGONE when the last command centre is gone.
-- Question (AI_TACTICAL sec. 7.7.6 / sec. 12 item 12): how long does the last Rebel command centre survive once the base
-- is gone? Port (alamo_headless, TR Blood Gulch, Empire AI vs Rebel AI, object scripts on): Empire wins at frame
-- 45,143; the last CC self-repairs.
-- Records "Q6P|<KIND>|p=trland|sid=..|n=..|f=..|...|END" in Q6L_LOG (heap scan). Lua 5.0.
Q6L_FACTIONS = { @@FACTIONS@@ }
Q6L_LOG = {}
Q6L_N = 0
Q6L_SID = string.sub((string.gsub(tostring(Q6L_LOG), "[^%w]", "")), -6)
Q6L = { armed = false, f = -1, last = -100000, known = {}, cc_alive = nil, base_gone = false, samples = 0 }
Q6L_EVERY = 150
Q6L_MAX = 20000

function Q6L_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function Q6L_Rec(kind, fields)
	pcall(function()
		if Q6L_N >= Q6L_MAX then
			return
		end
		Q6L_N = Q6L_N + 1
		local body = fields or ""
		if string.len(body) > 300 then
			body = string.sub(body, 1, 300) .. "~"
		end
		Q6L_LOG[Q6L_N] = "Q6P|" .. kind .. "|p=trland|sid=" .. Q6L_SID .. "|n=" .. Q6L_N .. "|f=" .. tostring(Q6L.f) .. "|" ..
			body .. "|END"
	end)
end

function Q6L_Objects(player)
	local ok, list = pcall(Find_All_Objects_Of_Type, player)
	if ok and type(list) == "table" then
		return list
	end
	return {}
end

function Q6L_Type(o)
	local n = "?"
	pcall(function() n = o.Get_Type().Get_Name() end)
	return Q6L_Str(n)
end

function Q6L_Id(o)
	local id = "?"
	pcall(function() id = tostring(o.Get_ID()) end)
	return Q6L_Str(id)
end

function Q6L_IsStructure(o)
	local ok, v = pcall(function() return o.Is_Category("Structure") end)
	return ok and v == true
end

function Q6L_Num(v, fmt)
	if type(v) == "number" then
		return string.format(fmt or "%.3f", v)
	end
	return "?"
end

function Q6L_Arm()
	local fac, mode, human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() human = PlayerObject.Is_Human() end)
	if mode == "Land" and not human then
		Q6L.armed = true
		Q6L.fac = fac
		Q6L.enemies = {}
		for _, n in ipairs(Q6L_FACTIONS) do
			local p = nil
			pcall(function() p = Find_Player(n) end)
			local pf = "?"
			pcall(function() pf = string.upper(tostring(p.Get_Faction_Name())) end)
			if p and pf ~= fac then  -- by faction name: two wrappers of one player need not be ==
				local enemy = false
				pcall(function() enemy = PlayerObject.Is_Enemy(p) end)
				local h = false
				pcall(function() h = p.Is_Human() end)
				if enemy or h then
					table.insert(Q6L.enemies, { p = p, name = n, human = h })
				end
			end
		end
		local names = ""
		for _, e in ipairs(Q6L.enemies) do
			names = names .. e.name .. (e.human and "(H)" or "") .. ","
		end
		Q6L_Rec("ARM", "fac=" .. Q6L_Str(fac) .. "|rate=" .. Q6L_Str(ServiceRate) .. "|urate=" .. Q6L_Str(UnitServiceRate) ..
			"|enemies=" .. Q6L_Str(names))
	else
		Q6L_Rec("SKIP", "fac=" .. Q6L_Str(fac) .. "|mode=" .. Q6L_Str(mode) .. "|human=" .. tostring(human))
	end
end

function Q6L_Sample()
	local own = Q6L_Objects(PlayerObject)
	local own_units = {}
	for _, o in ipairs(own) do
		if not Q6L_IsStructure(o) then
			table.insert(own_units, o)
		end
	end
	local seen = {}
	local structs, others, ccs = 0, 0, {}
	for _, e in ipairs(Q6L.enemies or {}) do
		for _, o in ipairs(Q6L_Objects(e.p)) do
			local id = Q6L_Id(o)
			if Q6L_IsStructure(o) then
				local tn = Q6L_Type(o)
				seen[id] = tn
				if Q6L.known[id] == nil then
					Q6L.known[id] = tn
					if Q6L.samples > 0 then
						Q6L_Rec("NEW", "id=" .. id .. "|type=" .. tn .. "|owner=" .. e.name)
					end
				end
				if string.find(string.upper(tn), "COMMAND_CENTER", 1, true) then
					table.insert(ccs, o)
				else
					structs = structs + 1
				end
			else
				others = others + 1
			end
		end
	end
	for id, tn in pairs(Q6L.known) do
		if seen[id] == nil then
			Q6L.known[id] = nil
			Q6L_Rec("GONE", "id=" .. id .. "|type=" .. tn)
		end
	end
	if Q6L.samples == 0 then
		local s = ""
		for id, tn in pairs(Q6L.known) do
			s = s .. tn .. ":" .. id .. ";"
		end
		Q6L_Rec("STRUCTS", "list=" .. s)
	end
	Q6L.samples = Q6L.samples + 1
	local cc_text = ""
	for i, c in ipairs(ccs) do
		local hull = nil
		pcall(function() hull = c.Get_Hull() end)
		local n600, n2884, best = 0, 0, nil
		for _, u in ipairs(own_units) do
			local ok, d = pcall(function() return u.Get_Distance(c) end)
			if ok and type(d) == "number" then
				if d <= 600 then
					n600 = n600 + 1
				end
				if d <= 2884 then
					n2884 = n2884 + 1
				end
				if best == nil or d < best then
					best = d
				end
			end
		end
		if i <= 3 then
			cc_text = cc_text .. "|cc" .. i .. "=" .. Q6L_Type(c) .. ":" .. Q6L_Id(c) .. ":hull=" .. Q6L_Num(hull) .. ":n600=" ..
				n600 .. ":n2884=" .. n2884 .. ":near=" .. Q6L_Num(best, "%.0f")
		end
	end
	Q6L_Rec("S", "structs=" .. structs .. "|ccs=" .. table.getn(ccs) .. "|enemy_units=" .. others .. "|own_units=" ..
		table.getn(own_units) .. cc_text)
	if not Q6L.base_gone and structs == 0 and table.getn(ccs) > 0 then
		Q6L.base_gone = true
		Q6L_Rec("BASEGONE", "ccs=" .. table.getn(ccs))
	end
	if Q6L.cc_alive and table.getn(ccs) == 0 then
		Q6L_Rec("CCGONE", "structs=" .. structs .. "|enemy_units=" .. others)
	end
	Q6L.cc_alive = table.getn(ccs) > 0
end

function Q6L_Step()
	if not Q6L.checked and PlayerObject ~= nil then
		Q6L.checked = true
		Q6L_Arm()
	end
	if not Q6L.armed then
		return
	end
	local ok, t = pcall(GetCurrentTime)
	if not ok or type(t) ~= "number" then
		return
	end
	local F = tonumber(string.format("%.0f", t * 30))
	Q6L.f = F
	if F - Q6L.last >= Q6L_EVERY then
		Q6L.last = F
		Q6L_Sample()
	end
end

Q6L_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(Q6L_Step)
	return Q6L_Orig_FreeStoreService()
end

Q6L_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(Q6L_Step)
	return Q6L_Orig_On_Unit_Service(object)
end

Q6L_Rec("LOAD", "factions=" .. table.getn(Q6L_FACTIONS))
-- ==== end of the Q6 t15 TR land probe ====
