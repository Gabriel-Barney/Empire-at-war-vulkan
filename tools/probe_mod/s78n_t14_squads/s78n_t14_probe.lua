-- ==== s78n t14: land-squad formation in retail (FIGHTER_FLYING sec. 7 - 7.4) ====
-- tools/probe_mod/s78n_t14_squads/s78n_t14_probe.lua. build_s78n_t14_mod.py appends, to the shipped
-- BusyTacticalFreeStore.lua, a generated SN_CFG table and this file.
--
-- Questions (roadmap s78n t03 / t09 / t14, docs/PHASE4.md):
--   Q1 member lag: while the container marches a long straight leg, how far do the members trail their slots, and do
--      they close up after the stop? (the port: member vmax = team speed, so a lag from the start delay or a turn is
--      never made up; its fixture squad ends ~30 behind)
--   Q2 start delays: each member's start frame after the order (0..Max_Move_Frame_Delay = 6 frames per member)
--   Q3 layout: Rows (Stormtrooper_Team) vs Dartboard (Rebel_Trooper_Team) slots relative to the container after a
--      move, a straight continuation (no re-lay), a 30 deg turn (re-lay) and a 5 deg turn (no re-lay)
-- Per trial one squad (company SN_CFG.sq.squads[key].company) is spawned for the human player at the corridor start;
-- its team container (Get_Parent_Object of the new members) gets four Move_To orders (SN_CFG.sq.legs), each after the
-- container has stood still for `hold` frames. Every poll a P record holds the container's and every member's XY.
--
-- Arming: only the free store of the AI player whose faction is SN_CFG.ai_faction (REBEL) runs it, in Land mode,
-- and only when Find_Player(SN_CFG.human_faction) (Empire) is human, so the main-menu background battle never arms
-- it. ServiceRate is lowered to 0.05 s (every free-store service, every 3rd frame); while it runs the shipped
-- FreeStoreService / On_Unit_Service bodies are skipped.
-- Output: Lua strings "S8Q|<KIND>|sid=<script id>|n=<n>|f=<mode frame>|...|END" kept in SN_LOG; s78n_t14_run.py
-- finds them with a read-only heap scan. f = GetCurrentTime() * 30 rounded.
-- Retail rules kept (earlier runs): Lua 5.0 (no '#', no '%', no math library); every engine call on an object is
-- gated by Is_Valid (a failed engine read stops a retail script, pcall cannot catch it); objects use Get_Object_ID
-- (Get_ID is a player method); Spawn_Unit of a company returns its team containers (s78m run 20261108-215525);
-- Game_Message for the driver's cues; Point_Camera_At with a position; Move_To with a position only.

SN_LOG = {}
SN_N = 0
SN_SID = string.sub((string.gsub(tostring(SN_LOG), "[^%w]", "")), -6)
SN = { on = false, done = false, f = -1, mode = "?", fac = "?", tries = 0, pending = false, old_rate = nil,
	hb = nil, errs = 0 }
SN_API_BAD = {}
SN_MAX_RECORDS = 30000

if SN_CFG == nil then
	SN_CFG = { session = "?", ai_faction = "?", human_faction = "?" }
end

-- ---- records ----------------------------------------------------------------------------------------------------

function SN_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:,]", "_"))
end

function SN_Frame()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return tonumber(string.format("%.0f", t * 30))
	end
	return nil
end

function SN_Rec(kind, fields)
	pcall(function()
		if SN_N >= SN_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		SN_N = SN_N + 1
		local body = fields or ""
		if string.len(body) > 440 then
			body = string.sub(body, 1, 440) .. "~"
		end
		local f = SN_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		SN_LOG[SN_N] = "S8Q|" .. kind .. "|sid=" .. SN_SID .. "|n=" .. SN_N .. "|f=" .. fs .. "|" .. body .. "|END"
	end)
end

function SN_ApiFail(tag, err)
	if not SN_API_BAD[tag] then
		SN_API_BAD[tag] = true
		SN_Rec("APIERR", "api=" .. SN_Str(tag) .. "|err=" .. string.sub(SN_Str(err), 1, 150))
	end
end

function SN_Say(id)
	SN_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

-- ---- engine reads (all gated) -------------------------------------------------------------------------------------

function SN_Valid(o)
	if o == nil then
		return false
	end
	local ok, v = pcall(function() return o.Is_Valid() end)
	return ok and v == true
end

function SN_Id(o)
	if not SN_Valid(o) then
		return "?"
	end
	local id = nil
	pcall(function() id = o.Get_Object_ID() end)
	if id == nil then
		return "?"
	end
	return SN_Str(id)
end

function SN_Type(o)
	if not SN_Valid(o) then
		return "?"
	end
	local n = "?"
	pcall(function() n = o.Get_Type().Get_Name() end)
	return SN_Str(n)
end

function SN_XYn(o)
	if not SN_Valid(o) then
		return nil
	end
	local ok, x, y = pcall(function()
		local a, b, c = o.Get_Position().Get_XYZ()
		if type(a) == "table" then
			return a[1], a[2]
		end
		return a, b
	end)
	if ok and type(x) == "number" and type(y) == "number" then
		return x, y
	end
	return nil
end

function SN_XY(o)
	local x, y = SN_XYn(o)
	if x then
		return string.format("%.2f,%.2f", x, y)
	end
	return "?"
end

function SN_Owner(o)
	if not SN_Valid(o) then
		return "dead"
	end
	local n = "?"
	pcall(function() n = string.upper(tostring(o.Get_Owner().Get_Faction_Name())) end)
	return SN_Str(n)
end

function SN_Call(o, tag, f)
	if not SN_Valid(o) then
		return "dead"
	end
	local ok, err = pcall(f)
	if ok then
		return "ok"
	end
	SN_ApiFail(tag, err)
	return "err"
end

function SN_Pos(x, y, z)
	local ok, p = pcall(Create_Position, x, y, z or 0)
	if ok then
		return p
	end
	SN_ApiFail("Create_Position", p)
	return nil
end

function SN_Player(name)
	local p = nil
	pcall(function() p = Find_Player(name) end)
	return p
end

function SN_NumId(id)
	local n = tonumber(id)
	if n == nil then
		return 1e18
	end
	return n
end

function SN_FindAll(tname)
	local ok, r = pcall(Find_All_Objects_Of_Type, tname)
	if ok and type(r) == "table" then
		return r
	end
	return {}
end

function SN_SpawnList(tname, pos, player)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if ty == nil then
		return {}, "no_type"
	end
	if pos == nil or player == nil then
		return {}, "no_pos_or_player"
	end
	local ok, r = pcall(Spawn_Unit, ty, pos, player)
	if ok and type(r) == "table" then
		return r, "spawn_unit"
	end
	if ok and r ~= nil and SN_Valid(r) then
		return { r }, "spawn_unit_single"
	end
	if not ok then
		SN_ApiFail("Spawn_Unit:" .. tname, r)
	end
	return {}, "spawn_failed"
end

function SN_Despawn(o)
	if SN_Valid(o) then
		pcall(function() o.Despawn() end)
	end
end

function SN_Camera(x, y, z)
	if not SN_CFG.sq.camera then
		return
	end
	local p = SN_Pos(x, y, z)
	if p ~= nil then
		pcall(Point_Camera_At, p)
	end
end

-- ================================================================================================================
-- the squad trials
-- ================================================================================================================

SQ = { phase = "init", ti = 0, cur = nil, next_f = nil }

-- ids of every existing member / container of this squad kind (the next spawn's objects are the new ones)
function SQ_Snapshot(sq)
	local s = {}
	for _, tn in ipairs({ sq.member, sq.team }) do
		for _, o in ipairs(SN_FindAll(tn)) do
			local id = SN_Id(o)
			if id ~= "?" then
				s[id] = true
			end
		end
	end
	return s
end

function SQ_Owner()
	if SN_CFG.sq.owner == "ai" then
		return PlayerObject
	end
	return SN_Player(SN_CFG.human_faction)
end

function SQ_Start(F, k, key)
	local cfg = SN_CFG.sq
	local sq = cfg.squads[key]
	local site = cfg.site
	local t = { k = k, key = key, sq = sq, f0 = F, phase = "ident", leg = 0, errs = 0, old = SQ_Snapshot(sq) }
	SQ.cur = t
	local owner = SQ_Owner()
	local comp = sq.company
	local r, how = SN_SpawnList(comp, SN_Pos(site.x, site.y, site.z), owner)
	if table.getn(r) == 0 then
		for _, alt in ipairs(sq.alts) do
			if table.getn(r) == 0 then
				comp = alt
				r, how = SN_SpawnList(alt, SN_Pos(site.x, site.y, site.z), owner)
			end
		end
	end
	t.company = comp
	t.ret = r
	local s = ""
	for _, o in ipairs(r) do
		s = s .. SN_Type(o) .. ":" .. SN_Id(o) .. "@" .. SN_XY(o) .. ";"
	end
	SN_Rec("TRIAL", "k=" .. k .. "|key=" .. key .. "|arr=" .. SN_Str(sq.arrangement) .. "|company=" .. comp .. "|how=" ..
		how .. "|owner=" .. SN_Str(cfg.owner) .. "|site=" .. string.format("%.1f,%.1f", site.x, site.y) .. "|ret=" ..
		string.sub(s, 1, 260))
	t.fi = F + cfg.ident_wait
	SN_Camera(site.x, site.y, site.z)
end

-- the new members, grouped by their team container; the container from Spawn_Unit's list with the most members
function SQ_Ident(t, F)
	local cfg = SN_CFG.sq
	local sq = t.sq
	local groups = {}
	local keys = {}
	local nnew = 0
	for _, o in ipairs(SN_FindAll(sq.member)) do
		local id = SN_Id(o)
		if SN_Valid(o) and id ~= "?" and not t.old[id] then
			nnew = nnew + 1
			local p = nil
			pcall(function() p = o.Get_Parent_Object() end)
			local pid = "none"
			if SN_Valid(p) then
				pid = SN_Id(p)
			else
				p = nil
			end
			if groups[pid] == nil then
				groups[pid] = { pid = pid, c = p, members = {} }
				table.insert(keys, pid)
			end
			table.insert(groups[pid].members, { o = o, id = id })
		end
	end
	-- prefer a container returned by Spawn_Unit
	local best = nil
	for _, o in ipairs(t.ret) do
		local g = groups[SN_Id(o)]
		if g ~= nil and g.pid ~= "none" and (best == nil or table.getn(g.members) > table.getn(best.members)) then
			best = g
			best.c = o
			best.src = "ret"
		end
	end
	if best == nil then
		for _, pid in ipairs(keys) do
			local g = groups[pid]
			if pid ~= "none" and (best == nil or table.getn(g.members) > table.getn(best.members)) then
				best = g
				best.src = "parent"
			end
		end
	end
	SN_Rec("IDENT", "k=" .. t.k .. "|new=" .. nnew .. "|groups=" .. table.getn(keys) .. "|src=" ..
		SN_Str(best and best.src or "none"))
	if best == nil then
		SN_Rec("ERR", "what=no_team|k=" .. t.k)
		t.phase = "end"
		return
	end
	-- other new teams of this spawn (a company of two teams): removed so they do not walk into the trial
	local extra = 0
	for _, pid in ipairs(keys) do
		local g = groups[pid]
		if g ~= best then
			for _, m in ipairs(g.members) do
				SN_Despawn(m.o)
				extra = extra + 1
			end
			if g.c ~= nil then
				SN_Despawn(g.c)
			end
		end
	end
	for _, o in ipairs(t.ret) do
		if o ~= best.c and SN_Id(o) ~= best.pid then
			SN_Despawn(o)
		end
	end
	table.sort(best.members, function(a, b) return SN_NumId(a.id) < SN_NumId(b.id) end)
	t.c = best.c
	t.cid = best.pid
	t.members = best.members
	local ids, xys = "", ""
	for _, m in ipairs(t.members) do
		ids = ids .. m.id .. ";"
		xys = xys .. SN_XY(m.o) .. ";"
	end
	SN_Rec("TEAM", "k=" .. t.k .. "|cid=" .. SN_Str(t.cid) .. "|ctype=" .. SN_Type(t.c) .. "|cown=" .. SN_Owner(t.c) ..
		"|cxy=" .. SN_XY(t.c) .. "|n=" .. table.getn(t.members) .. "|mtype=" .. SN_Type(t.members[1].o) .. "|extra=" ..
		extra .. "|ids=" .. string.sub(ids, 1, 120) .. "|xy=" .. string.sub(xys, 1, 220))
	for _, m in ipairs(t.members) do
		local o = m.o
		if cfg.invuln then
			SN_Call(o, "Make_Invulnerable", function() o.Make_Invulnerable(true) end)
		end
		if cfg.nofire then
			SN_Call(o, "Prevent_All_Fire", function() o.Prevent_All_Fire(true) end)
		end
		if cfg.owner == "ai" then
			SN_Call(o, "Prevent_AI_Usage", function() o.Prevent_AI_Usage(true) end)
		end
	end
	if cfg.invuln then
		SN_Call(t.c, "Make_Invulnerable", function() t.c.Make_Invulnerable(true) end)
	end
	if cfg.owner == "ai" then
		SN_Call(t.c, "Prevent_AI_Usage", function() t.c.Prevent_AI_Usage(true) end)
	end
	t.phase = "settle"
	t.fs = F
end

-- the container's position, else the members' centroid (src "c" / "m")
function SQ_Centre(t)
	local x, y = SN_XYn(t.c)
	if x ~= nil then
		return x, y, "c"
	end
	local sx, sy, n = 0, 0, 0
	for _, m in ipairs(t.members) do
		local mx, my = SN_XYn(m.o)
		if mx ~= nil then
			sx, sy, n = sx + mx, sy + my, n + 1
		end
	end
	if n > 0 then
		return sx / n, sy / n, "m"
	end
	return nil
end

function SQ_Poll(t)
	local ms = ""
	local alive = 0
	for _, m in ipairs(t.members) do
		local x, y = SN_XYn(m.o)
		if x ~= nil then
			ms = ms .. string.format("%.2f,%.2f", x, y) .. ";"
			alive = alive + 1
		else
			ms = ms .. "?;"
		end
	end
	SN_Rec("P", "k=" .. t.k .. "|leg=" .. t.leg .. "|ph=" .. t.phase .. "|c=" .. SN_XY(t.c) .. "|m=" .. ms)
	return alive
end

function SQ_Order(t, F, i, via)
	local cfg = SN_CFG.sq
	local lg = cfg.legs[i]
	local cx, cy, src = SQ_Centre(t)
	if cx == nil then
		SN_Rec("ERR", "what=no_position|k=" .. t.k .. "|leg=" .. i)
		t.phase = "end"
		return
	end
	local tx = cx + lg.dx * lg.len
	local ty = cy + lg.dy * lg.len
	local p = SN_Pos(tx, ty, cfg.site.z)
	local who = nil
	local whoname = "container"
	if via ~= "member" and SN_Valid(t.c) then
		who = t.c
	else
		whoname = "member"
		for _, m in ipairs(t.members) do
			if who == nil and SN_Valid(m.o) then
				who = m.o
			end
		end
	end
	local ret = "dead"
	if who ~= nil and p ~= nil then
		local ok, r = pcall(function() return who.Move_To(p) end)
		if not ok then
			SN_ApiFail("Move_To", r)
			ret = "error"
		elseif r == nil then
			ret = "nil"
		else
			ret = "obj"
		end
	end
	if via ~= "member" then
		t.leg = i
		t.fo = F
		t.sx, t.sy = cx, cy
		t.tx, t.ty = tx, ty
		t.moved = false
		t.still = 0
		t.lx, t.ly = cx, cy
		t.reordered = false
		t.phase = "move"
		SN_Camera(cx + lg.dx * 0.5 * lg.len, cy + lg.dy * 0.5 * lg.len, cfg.site.z)
	end
	SN_Rec("LEG", "k=" .. t.k .. "|leg=" .. i .. "|name=" .. lg.name .. "|turn=" .. SN_Str(lg.turn) .. "|deg=" ..
		SN_Str(lg.deg) .. "|from=" .. string.format("%.2f,%.2f", cx, cy) .. "|src=" .. src .. "|to=" ..
		string.format("%.2f,%.2f", tx, ty) .. "|who=" .. whoname .. "|ret=" .. ret .. "|via=" .. tostring(via))
end

function SQ_Move(t, F)
	local cfg = SN_CFG.sq
	local cx, cy = SQ_Centre(t)
	if cx == nil then
		SN_Rec("ERR", "what=no_position|k=" .. t.k .. "|leg=" .. t.leg)
		t.phase = "end"
		return
	end
	local dsx, dsy = cx - t.sx, cy - t.sy
	local dlx, dly = cx - t.lx, cy - t.ly
	t.lx, t.ly = cx, cy
	if not t.moved then
		if dsx * dsx + dsy * dsy > cfg.moved_min * cfg.moved_min then
			t.moved = true
			SN_Rec("CSTART", "k=" .. t.k .. "|leg=" .. t.leg .. "|r=" .. (F - t.fo))
		elseif not t.reordered and F - t.fo >= cfg.reorder_wait then
			t.reordered = true
			SN_Rec("REORDER", "k=" .. t.k .. "|leg=" .. t.leg .. "|r=" .. (F - t.fo))
			SQ_Order(t, F, t.leg, "member")
		end
	else
		if dlx * dlx + dly * dly < 0.02 * 0.02 then
			t.still = t.still + 1
		else
			t.still = 0
		end
		if t.still >= cfg.still_polls then
			local ex, ey = cx - t.tx, cy - t.ty
			SN_Rec("CSTOP", "k=" .. t.k .. "|leg=" .. t.leg .. "|r=" .. (F - t.fo) .. "|at=" ..
				string.format("%.2f,%.2f", cx, cy) .. "|miss=" .. string.format("%.2f", ex * ex + ey * ey))
			t.phase = "hold"
			t.fh = F
			return
		end
	end
	if F - t.fo >= cfg.leg_max then
		SN_Rec("LEGTO", "k=" .. t.k .. "|leg=" .. t.leg .. "|moved=" .. tostring(t.moved))
		t.phase = "hold"
		t.fh = F
	end
end

function SQ_TrialTick(t, F)
	local cfg = SN_CFG.sq
	if t.phase == "ident" then
		if F >= t.fi then
			SQ_Ident(t, F)
			if t.phase == "settle" then
				SQ_Poll(t)
			end
		end
		return
	end
	local alive = SQ_Poll(t)
	if alive == 0 then
		SN_Rec("ERR", "what=all_members_gone|k=" .. t.k .. "|leg=" .. t.leg)
		t.phase = "end"
		return
	end
	if t.phase == "settle" then
		if F - t.fs >= cfg.settle then
			SQ_Order(t, F, 1, nil)
		end
		return
	end
	if t.phase == "move" then
		SQ_Move(t, F)
		return
	end
	if t.phase == "hold" then
		if F - t.fh >= cfg.hold then
			if t.leg < table.getn(cfg.legs) then
				SQ_Order(t, F, t.leg + 1, nil)
			else
				t.phase = "end"
			end
		end
	end
end

function SQ_EndTrial(t, F)
	local cfg = SN_CFG.sq
	for _, m in ipairs(t.members or {}) do
		SN_Despawn(m.o)
	end
	SN_Despawn(t.c)
	SN_Rec("TEND", "k=" .. t.k .. "|key=" .. t.key .. "|legs=" .. t.leg .. "|aborted=" .. tostring(t.aborted))
	SQ.cur = nil
	SQ.phase = "pause"
	SQ.next_f = F + cfg.gap
end

function SQ_Tick(F)
	local cfg = SN_CFG.sq
	if SQ.phase == "init" then
		if SQ.next_f == nil then
			SQ.next_f = cfg.start_frame
			SN_Say("TEXT_S78N_T14_ARMED")
			local legs = ""
			for _, lg in ipairs(cfg.legs) do
				legs = legs .. lg.name .. ":" .. SN_Str(lg.deg) .. ":" .. SN_Str(lg.len) .. ";"
			end
			SN_Rec("SCFG", "map=" .. SN_Str(cfg.map) .. "|site=" .. string.format("%.1f,%.1f,%.1f", cfg.site.x, cfg.site.y,
				cfg.site.z) .. "|order=" .. table.concat(cfg.order, ",") .. "|legs=" .. legs .. "|hold=" .. cfg.hold ..
				"|settle=" .. cfg.settle .. "|owner=" .. SN_Str(cfg.owner) .. "|invuln=" .. tostring(cfg.invuln) ..
				"|nofire=" .. tostring(cfg.nofire))
		end
		if F >= SQ.next_f then
			SQ.phase = "pause"
		end
		return
	end
	if SQ.phase == "pause" then
		if F >= SQ.next_f then
			SQ.ti = SQ.ti + 1
			local key = cfg.order[SQ.ti]
			if key == nil then
				SN_Say("TEXT_S78N_T14_DONE")
				SN_Finish("trials=" .. (SQ.ti - 1))
				return
			end
			SQ.phase = "trial"
			SQ_Start(F, SQ.ti, key)
		end
		return
	end
	local t = SQ.cur
	if t == nil then
		return
	end
	if t.phase ~= "end" then
		local ok, err = pcall(SQ_TrialTick, t, F)
		if not ok then
			t.errs = t.errs + 1
			if t.errs <= 3 then
				SN_Rec("ERR", "where=trial|k=" .. t.k .. "|ph=" .. tostring(t.phase) .. "|err=" .. string.sub(SN_Str(err), 1, 200))
			end
			if t.errs >= 3 then
				t.phase = "end"
				t.aborted = "errors"
			end
		end
		local lim = cfg.ident_wait + cfg.settle + 60
		for _, lg in ipairs(cfg.legs) do
			lim = lim + cfg.leg_max + cfg.hold + 30
		end
		if t.phase ~= "end" and F - t.f0 > lim then
			t.phase = "end"
			t.aborted = "timeout"
		end
	end
	if t.phase == "end" then
		SQ_EndTrial(t, F)
	end
end

-- ---- arming and the service drivers -------------------------------------------------------------------------------

function SN_Arm()
	local fac, mode, human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() human = PlayerObject.Is_Human() end)
	if (fac == "?" or fac == "NIL") and SN.tries < 200 then
		SN.pending = true
		return
	end
	SN.pending = false
	SN.mode = SN_Str(mode)
	SN.fac = SN_Str(fac)
	local human_ok = false
	pcall(function() human_ok = Find_Player(SN_CFG.human_faction).Is_Human() == true end)
	if mode == "Land" and not human_ok and SN.tries < 60 then
		SN.pending = true
		return
	end
	if mode == "Land" and fac == SN_CFG.ai_faction and human ~= true and human_ok and not SN_ARMED_ONCE then
		SN_ARMED_ONCE = true
		SN.on = true
		SN.old_rate = ServiceRate
		ServiceRate = 0.05
		SN_Rec("ARM", "fac=" .. SN.fac .. "|mode=" .. SN.mode .. "|session=" .. SN_Str(SN_CFG.session) .. "|rate=" ..
			SN_Str(SN.old_rate) .. "|tries=" .. SN.tries)
	else
		SN_Rec("SKIP", "fac=" .. SN.fac .. "|mode=" .. SN.mode .. "|human=" .. tostring(human) .. "|human_side=" ..
			tostring(human_ok) .. "|session=" .. SN_Str(SN_CFG.session))
	end
end

function SN_Finish(extra)
	if SN.done then
		return
	end
	SN.done = true
	if SN.old_rate ~= nil then
		ServiceRate = SN.old_rate
	end
	local bad = ""
	for k, _ in pairs(SN_API_BAD) do
		bad = bad .. k .. ","
	end
	SN_Rec("DONE", "records=" .. SN_N .. "|apierr=" .. string.sub(SN_Str(bad), 1, 220) .. "|" .. (extra or ""))
end

function SN_Tick(src)
	if SN.pending then
		SN.tries = SN.tries + 1
		pcall(SN_Arm)
	end
	if not SN.on or SN.done then
		return
	end
	local F = SN_Frame()
	if F == nil or F == SN.f then
		return
	end
	SN.f = F
	if SN.hb == nil or F - SN.hb >= 30 then
		SN.hb = F
		SN_Rec("HB", "src=" .. src)
	end
	local ok, err = pcall(SQ_Tick, F)
	if not ok then
		local msg = string.sub(SN_Str(err), 1, 200)
		SN.errs = SN.errs + 1
		if SN.errseen == nil then
			SN.errseen = {}
		end
		if SN.errseen[msg] == nil then
			SN.errseen[msg] = true
			SN_Rec("ERR", "where=tick|src=" .. src .. "|n_err=" .. SN.errs .. "|err=" .. msg)
		end
		if SN.errs >= 100 then
			SN_Finish("why=tick_errors|n_err=" .. SN.errs)
		end
	end
end

function SN_Quiet()
	return SN.on and not SN.done
end

SN_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = nil
	if SN_Orig_Base_Definitions then
		r = SN_Orig_Base_Definitions()
	end
	SN.pending = true
	pcall(SN_Arm)
	return r
end

SN_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(SN_Tick, "fss")
	if SN_Quiet() or SN_Orig_FreeStoreService == nil then
		return
	end
	return SN_Orig_FreeStoreService()
end

SN_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(SN_Tick, "ous")
	if SN_Quiet() or SN_Orig_On_Unit_Service == nil then
		return
	end
	return SN_Orig_On_Unit_Service(object)
end

SN_Rec("LOAD", "session=" .. SN_Str(SN_CFG.session) .. "|ai_faction=" .. SN_Str(SN_CFG.ai_faction) .. "|human=" ..
	SN_Str(SN_CFG.human_faction))
-- ==== end of s78n_t14_probe.lua ====
