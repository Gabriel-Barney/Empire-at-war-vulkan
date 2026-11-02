-- ==== FOW regrow probe (appended to the shipped BusyTacticalFreeStore.lua by tools/probe_mod/fow_regrow/build_fow_mod.py) ====
-- FWRPROBE_SRC_MARK_V1
-- Runs inside the AI player's tactical free store (land only). It never orders the AI's own units; it spawns two
-- objects for the AI player (kept out of the AI's hands), makes counted reveals for the HUMAN player with
-- FogOfWar.Reveal and undoes them with Undo_Reveal at recorded frames, and samples whether the human can see the
-- spawned object with Find_Nearest(src, type, human, false) (FOW_Object_Hidden with force = 1, FOG_OF_WAR.md sec. 5.2).
-- No GameRandom. Lua 5.0 (no '#', no '%', varargs via 'arg').
--
-- Cadence: the free store is serviced every 3rd frame; its threads resume only when now - LastService > ServiceRate
-- and units are serviced when now - LastUnitService > UnitServiceRate (AI_GALACTIC sec. 1.10, AI_TACTICAL sec. 5.3). The
-- wrappers below set both rates to 0.05 s so the probe step runs at every service, and call the shipped
-- FreeStoreService / On_Unit_Service only on the shipped 20 s / 2 s schedule, so the AI plays as before.
--
-- Output: records "FWR1|<KIND>|sid=..|f=<mode frame>|...|END" kept in FWR_LOG; fow_reader.py finds them by heap
-- scan (retail _CustomScriptMessage writes nothing). Kinds: LOAD, START, SPAWN, EDGEX, REV, UNDO, VIS, TEND, CAD,
-- DONE, ERR. f = round(GetCurrentTime() * 30) (mode frame; GetCurrentTime() = (float)F * (1/30)).

FWR_MAP = "@@MAP@@"
FWR_X1 = @@X1@@
FWR_Y1 = @@Y1@@
FWR_R = 90
FWR_START_F = 180
FWR_TRIAL_D1 = { 16, 1, 8, 12 }
FWR_EDGE_D = { -300, 7.5, 22.5, 37.5, 52.5, 67.5, 82.5, 97.5, 112.5, 127.5 }
FWR_EDGE_X = { 0.5, 0.3, 0.7, 0.15, 0.85 }
FWR_TYPES = {
	REBEL = { "MPTL", "T4B_Tank", "T2B_Tank" },
	EMPIRE = { "SPMAT_Walker", "AT_AA_Walker", "AT_ST_Walker" },
	UNDERWORLD = { "Canderous_Assault_Tank", "MZ8_Pulse_Cannon_Tank" },
}
FWR_ANY = { "MPTL", "SPMAT_Walker", "Canderous_Assault_Tank", "T2B_Tank", "AT_ST_Walker" }
FWR_FACTIONS = { "Rebel", "Empire", "Pirates", "Neutral", "Hostile", "Sarlacc", "Underworld", "Hutts" }
FWR_RATE = 0.05

FWR_LOG = {}
FWR = { phase = "wait", f = -1, lastf = -1, steps = 0, cad = "", cadn = 0, ti = 0, ei = 0 }

function FWR_Clean(v)
	local s = string.gsub(tostring(v), "[^%w%._%-:]", "_")
	return s
end

function FWR_Rec(kind, fields)
	pcall(function()
		table.insert(FWR_LOG, "FWR1|" .. kind .. "|sid=" .. FWR_SID .. "|f=" .. tostring(FWR.f) .. (fields or "") .. "|END")
	end)
end

function FWR_Err(what, err)
	FWR_Rec("ERR", "|what=" .. what .. "|msg=" .. string.sub(FWR_Clean(err), 1, 120))
end

function FWR_Num(v)
	if type(v) == "number" then
		return string.format("%.2f", v)
	end
	return "?"
end

function FWR_Now()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return t
	end
	return nil
end

function FWR_Mod(a, b)
	return a - math.floor(a / b) * b
end

-- frames from frame F to the human grid's next decay (FOG_OF_WAR.md sec. 4.4: decay when id == F mod 16, before Lua)
function FWR_D1(F)
	return FWR_Mod(FWR.hid - F - 1, 16) + 1
end

function FWR_Dist(a, b)
	local ok, d = pcall(function() return a.Get_Distance(b) end)
	if ok and type(d) == "number" then
		return d
	end
	return nil
end

function FWR_Objects(player, max)
	local out = {}
	local ok, list = pcall(Find_All_Objects_Of_Type, player)
	if ok and type(list) == "table" then
		for i, o in ipairs(list) do
			if table.getn(out) >= max then
				break
			end
			table.insert(out, o)
		end
	end
	return out
end

function FWR_MinDist(p, list)
	local best = 1.0e9
	for i, o in ipairs(list) do
		local d = FWR_Dist(o, p)
		if d and d < best then
			best = d
		end
	end
	return best
end

function FWR_Spawn(tname, pos)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if not ty then
		return nil
	end
	local ok, o = pcall(Create_Generic_Object, ty, pos, PlayerObject)
	if ok and o then
		return o
	end
	ok, o = pcall(Spawn_Unit, ty, pos, PlayerObject)
	if ok and type(o) == "table" then
		return o[1]
	end
	return nil
end

function FWR_Calm(o)
	pcall(function() o.Prevent_AI_Usage(true) end)
	pcall(function() o.Prevent_All_Fire(true) end)
	pcall(function() o.Make_Invulnerable(true) end)
end

function FWR_Setup()
	local isHuman = false
	pcall(function() isHuman = PlayerObject.Is_Human() end)
	if isHuman then
		FWR.phase = "off"
		return
	end
	for i, name in ipairs(FWR_FACTIONS) do
		local p = nil
		pcall(function() p = Find_Player(name) end)
		if p then
			local h = false
			pcall(function() h = p.Is_Human() end)
			if h and FWR.H == nil then
				FWR.H = p
			end
		end
	end
	if FWR.H == nil then
		FWR_Rec("ERR", "|what=no_human_player")
		FWR.phase = "done"
		return
	end
	FWR.hid = FWR.H.Get_ID()
	local afac, aid = "?", "?"
	pcall(function() afac = string.upper(PlayerObject.Get_Faction_Name()) end)
	pcall(function() aid = PlayerObject.Get_ID() end)
	local hobjs = FWR_Objects(FWR.H, 40)
	local aobjs = FWR_Objects(PlayerObject, 80)
	-- anchor: the AI object farthest from every human object (the human cannot see there)
	local bestd = -1
	for i, a in ipairs(aobjs) do
		local d = FWR_MinDist(a, hobjs)
		if d > bestd then
			FWR.anchor, bestd = a, d
		end
	end
	FWR_Rec("START", "|map=" .. FWR_MAP .. "|hid=" .. tostring(FWR.hid) .. "|aid=" .. FWR_Clean(aid) ..
		"|afac=" .. FWR_Clean(afac) .. "|R=" .. FWR_R .. "|x1=" .. FWR_Num(FWR_X1) .. "|y1=" .. FWR_Num(FWR_Y1) ..
		"|hobjs=" .. table.getn(hobjs) .. "|aobjs=" .. table.getn(aobjs) .. "|anchor_hdist=" .. FWR_Num(bestd) ..
		"|d1now=" .. FWR_D1(FWR.f))
	if FWR.anchor == nil then
		FWR_Rec("ERR", "|what=no_ai_object")
		FWR.phase = "done"
		return
	end
	local pos = nil
	pcall(function() pos = FWR.anchor.Get_Position() end)
	local list = FWR_TYPES[afac] or FWR_ANY
	for pass = 1, 2 do
		for i, tname in ipairs(list) do
			if FWR.src == nil and pos then
				local a = FWR_Spawn(tname, pos)
				if a then
					local b = FWR_Spawn(tname, pos)
					if b then
						FWR.src, FWR.tgt, FWR.tname = a, b, tname
					end
				end
			end
		end
		list = FWR_ANY
	end
	if FWR.src then
		FWR_Calm(FWR.src)
		FWR_Calm(FWR.tgt)
		FWR_Rec("SPAWN", "|ok=1|type=" .. FWR.tname .. "|d_src_tgt=" .. FWR_Num(FWR_Dist(FWR.src, FWR.tgt)) ..
			"|d_anchor=" .. FWR_Num(FWR_Dist(FWR.src, FWR.anchor)) .. "|hdist=" .. FWR_Num(FWR_MinDist(FWR.src, hobjs)))
	else
		FWR_Rec("SPAWN", "|ok=0")
	end
	-- top-edge x: the candidate farthest from the human objects
	local bestx = -1
	for i, c in ipairs(FWR_EDGE_X) do
		local p = Create_Position(FWR_X1 * c, FWR_Y1 - 60, 0)
		local d = FWR_MinDist(p, hobjs)
		if d > bestx then
			FWR.ex, bestx = FWR_X1 * c, d
		end
	end
	FWR_Rec("EDGEX", "|x=" .. FWR_Num(FWR.ex) .. "|hdist=" .. FWR_Num(bestx))
	FWR.phase = "trial"
	FWR.ti = 1
	FWR.tr = { ph = "pre", t0 = FWR.f, n = 0 }
end

-- 1 = the human sees the target object, 0 = it does not (or nil, reason)
function FWR_Vis()
	if FWR.src == nil then
		return nil, "nospawn"
	end
	local ok, res = pcall(Find_Nearest, FWR.src, FWR.tname, FWR.H, false)
	local how = "type"
	if not ok then
		ok, res = pcall(Find_Nearest, FWR.src, FWR.H, false)
		how = "any"
	end
	if not ok then
		return nil, "error"
	end
	if res == nil then
		return 0, how .. "_nil"
	end
	local d = FWR_Dist(res, FWR.tgt)
	if res == FWR.tgt or (d and d < 3) then
		return 1, how .. "_tgt"
	end
	return 0, how .. "_other_" .. FWR_Num(FWR_Dist(res, FWR.src))
end

function FWR_Sample(ph)
	local tr = FWR.tr
	local v, why = FWR_Vis()
	tr.n = tr.n + 1
	if v ~= tr.v or ph ~= tr.vph then
		FWR_Rec("VIS", "|trial=" .. FWR.ti .. "|ph=" .. ph .. "|vis=" .. tostring(v) .. "|why=" .. FWR_Clean(why) ..
			"|prev=" .. tostring(tr.vf or -1) .. "|prevvis=" .. tostring(tr.v) .. "|n=" .. tr.n)
	end
	tr.v, tr.vph, tr.vf = v, ph, FWR.f
end

function FWR_Reveal(target)
	local ok, cells = pcall(function() return FogOfWar.Reveal(FWR.H, target, FWR_R) end)
	if not ok then
		FWR_Err("reveal", cells)
		return nil
	end
	return cells
end

function FWR_Undo(cells)
	if cells then
		local ok, err = pcall(function() cells.Undo_Reveal() end)
		if not ok then
			FWR_Err("undo", err)
		end
	end
end

function FWR_Trial()
	local tr, F = FWR.tr, FWR.f
	if tr.ph == "pre" then
		FWR_Sample("pre")
		if F >= tr.t0 + 15 then
			tr.cells = FWR_Reveal(FWR.src or FWR.anchor)
			tr.fr = F
			tr.ph = "hold"
			FWR_Rec("REV", "|kind=regrow|trial=" .. FWR.ti .. "|R=" .. FWR_R .. "|on=" .. (FWR.src and "src" or "anchor") ..
				"|ok=" .. (tr.cells and 1 or 0))
		end
	elseif tr.ph == "hold" then
		FWR_Sample("hold")
		local want = FWR_TRIAL_D1[FWR.ti]
		if F >= tr.fr + 45 and (FWR_D1(F) == want or F >= tr.fr + 205) then
			FWR_Undo(tr.cells)
			tr.fu = F
			tr.ph = "post"
			FWR_Rec("UNDO", "|kind=regrow|trial=" .. FWR.ti .. "|d1=" .. FWR_D1(F) .. "|want=" .. want)
		end
	elseif tr.ph == "post" then
		FWR_Sample("post")
		if F >= tr.fu + 270 then
			FWR_Rec("TEND", "|trial=" .. FWR.ti .. "|samples=" .. tr.n .. "|lastvis=" .. tostring(tr.v))
			FWR.ti = FWR.ti + 1
			if FWR.ti > table.getn(FWR_TRIAL_D1) then
				FWR.phase = "edge"
				FWR.ei = 1
				FWR.e = { ph = "rev" }
			else
				FWR.tr = { ph = "pre", t0 = F, n = 0 }
			end
		end
	end
end

function FWR_Edge()
	local e, F = FWR.e, FWR.f
	if e.ph == "rev" then
		local d = FWR_EDGE_D[FWR.ei]
		local y = FWR_Y1 - d
		local pos = Create_Position(FWR.ex, y, 0)
		e.cells = FWR_Reveal(pos)
		e.fr = F
		e.ph = "hold"
		FWR_Rec("REV", "|kind=edge|i=" .. FWR.ei .. "|d=" .. FWR_Num(d) .. "|x=" .. FWR_Num(FWR.ex) .. "|y=" ..
			FWR_Num(y) .. "|R=" .. FWR_R .. "|ok=" .. (e.cells and 1 or 0))
	elseif e.ph == "hold" and F >= e.fr + 9 then
		FWR_Undo(e.cells)
		e.fu = F
		e.ph = "gap"
		FWR_Rec("UNDO", "|kind=edge|i=" .. FWR.ei)
	elseif e.ph == "gap" and F >= e.fu + 9 then
		FWR.ei = FWR.ei + 1
		if FWR.ei > table.getn(FWR_EDGE_D) then
			FWR.phase = "done"
			FWR_Rec("DONE", "|steps=" .. FWR.steps)
		else
			e.ph = "rev"
		end
	end
end

function FWR_Step()
	local t = FWR_Now()
	if t == nil then
		return
	end
	local F = math.floor(t * 30 + 0.5)
	if F == FWR.lastf then
		return
	end
	local mode = "?"
	pcall(function() mode = string.lower(tostring(Get_Game_Mode())) end)
	if mode ~= "land" or FWR.phase == "off" then
		return
	end
	FWR.lastf, FWR.f = F, F
	FWR.steps = FWR.steps + 1
	if FWR.cadn < 40 then
		FWR.cad = FWR.cad .. "," .. F
		FWR.cadn = FWR.cadn + 1
		if FWR.cadn == 40 then
			FWR_Rec("CAD", "|frames=" .. string.sub(FWR.cad, 2))
		end
	end
	local ok, err = pcall(function()
		if FWR.phase == "wait" then
			if F >= FWR_START_F then
				FWR_Setup()
			end
		elseif FWR.phase == "trial" then
			FWR_Trial()
		elseif FWR.phase == "edge" then
			FWR_Edge()
		end
	end)
	if not ok then
		FWR_Err("step_" .. FWR.phase, err)
		FWR.phase = "done"
	end
end

function FWR_Mine(object)
	if FWR.tname == nil then
		return false
	end
	local n = nil
	pcall(function() n = object.Get_Type().Get_Name() end)
	return n ~= nil and string.upper(n) == string.upper(FWR.tname)
end

FWR_SID = FWR_Clean((string.gsub(tostring(FWR_LOG), "^table: ", "")))

FWR_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	FWR_Orig_Base_Definitions()
	FWR.rate, FWR.urate = ServiceRate, UnitServiceRate
	ServiceRate = FWR_RATE
	UnitServiceRate = FWR_RATE
end

FWR.fss_last = 0
FWR_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	FWR_Step()
	local t = FWR_Now() or 0
	if t - FWR.fss_last > (FWR.rate or 20) then
		FWR.fss_last = t
		return FWR_Orig_FreeStoreService()
	end
end

FWR.ous_last = 0
FWR_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	FWR_Step()
	local t = FWR_Now() or 0
	if t ~= FWR.ous_t then
		FWR.ous_t = t
		FWR.ous_run = t - FWR.ous_last > (FWR.urate or 2)
		if FWR.ous_run then
			FWR.ous_last = t
		end
	end
	if FWR.ous_run and not FWR_Mine(object) then
		return FWR_Orig_On_Unit_Service(object)
	end
end

FWR_Orig_On_Unit_Added = On_Unit_Added
function On_Unit_Added(object)
	if not FWR_Mine(object) then
		return FWR_Orig_On_Unit_Added(object)
	end
end

FWR_Rec("LOAD", "|map=" .. FWR_MAP)
-- ==== end of the FOW regrow probe ====
