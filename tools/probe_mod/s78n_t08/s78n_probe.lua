-- ==== s78n t08: tractor-beam range footprint (A) and Combat_Bonus aura on an allied player (B) ====
-- tools/probe_mod/s78n_t08/s78n_probe.lua. build_s78n_mod.py appends, to the shipped BusyTacticalFreeStore.lua, a
-- generated SN_CFG table and this file.
--
-- (A) Space (docs/re/TACTICAL_COMBAT.md sec. 5.4.2): may a Star_Destroyer's TRACTOR_BEAM start when the XY centre
--     distance d <= R + E (R = Activation_Max_Range, E = the target's X or Y half-extent by its facing)? Per trial a
--     fresh human Star_Destroyer U and an AI target X (Nebulon_B_Frigate) at a set distance d along +x; X turns to face
--     U ("toward") or is turned facing U from a point north of U and then teleported onto the line ("side"). Then
--     U.Activate_Ability("TRACTOR_BEAM", X). Every poll: both positions, U's TRACTOR_BEAM ready / active, X's
--     Is_Under_Effects_Of_Ability("TRACTOR_BEAM"). The first poll with the beam on is the engagement. When U has not
--     moved 90 frames after the order (no walk), X is teleported 3 units closer per poll and the order is repeated, so
--     a threshold is still crossed slowly.
-- (B) Land (sec. 5.4.3 item 4): does Emperor_Palpatine's Emperor_Land_Combat_Bonus (Health_Bonus 0.25) reach the
--     AT_ST_Walker of an ALLIED AI player (Underworld, same team), as the human's own (control) and not the enemy's
--     (control), near the Emperor and about 1200+ away (the port has no aura radius)? Observable: Get_Hull after a
--     fixed Lua Take_Damage(0.3 x health): a drop of 0.30 without the bonus, 0.24 with it (max health x 1.25); and,
--     for units damaged before the Emperor arrives, a hull rise 0.70 -> 0.76 when the aura is applied to them.
--
-- Arming: only the free store of the AI player whose faction is SN_CFG.ai_faction (REBEL) runs it, and only when
-- Find_Player(SN_CFG.human_faction) (Empire) is human, so the main-menu background battle never arms it. Base_
-- Definitions runs before PlayerObject is set, so arming is retried from the service calls. ServiceRate is lowered
-- to 0.05 s (every free-store service, every 3rd frame); while it runs the shipped FreeStoreService / On_Unit_Service
-- bodies are skipped.
-- Output: Lua strings "S8N|<KIND>|p=<probe>|sid=<script id>|n=<n>|f=<mode frame>|...|END" kept in SN_LOG;
-- s78n_run.py finds them with a read-only heap scan. f = GetCurrentTime() * 30 rounded.
-- Retail rules kept (earlier runs): Lua 5.0 (no '#', no '%', no math library); every engine call on an object is
-- gated by Is_Valid; Activate_Ability always gets a game object as its second argument; objects use Get_Object_ID
-- (Get_ID is a player method); Game_Message for the driver's cues; Point_Camera_At with a position.

SN_LOG = {}
SN_N = 0
SN_SID = string.sub((string.gsub(tostring(SN_LOG), "[^%w]", "")), -6)
SN = { on = false, done = false, f = -1, mode = "?", fac = "?", tries = 0, pending = false, old_rate = nil,
	hb = nil, probe = "-", pr = nil, errs = 0 }
SN_API_BAD = {}
SN_PROBES = {}
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
		if string.len(body) > 420 then
			body = string.sub(body, 1, 420) .. "~"
		end
		local f = SN_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		SN_LOG[SN_N] = "S8N|" .. kind .. "|p=" .. SN.probe .. "|sid=" .. SN_SID .. "|n=" .. SN_N .. "|f=" .. fs .. "|" ..
			body .. "|END"
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

-- x, y of an object (numbers) or nil
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

function SN_Flag(o, method, arg)
	if not SN_Valid(o) then
		return "dead"
	end
	local ok, v = pcall(function() return o[method](arg) end)
	if ok then
		if v then
			return "1"
		end
		return "0"
	end
	SN_ApiFail(method, v)
	return "?"
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

function SN_Num(v, fmt)
	if type(v) == "number" then
		return string.format(fmt or "%.3f", v)
	end
	return "?"
end

-- square root without the math library (Newton)
function SN_Sqrt(v)
	if type(v) ~= "number" or v <= 0 then
		return 0
	end
	local x = v
	if x < 1 then
		x = 1
	end
	for i = 1, 60 do
		x = 0.5 * (x + v / x)
	end
	return x
end

function SN_Dxy(a, b)
	local ax, ay = SN_XYn(a)
	local bx, by = SN_XYn(b)
	if ax == nil or bx == nil then
		return nil
	end
	return SN_Sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by))
end

function SN_Hull(o)
	if not SN_Valid(o) then
		return nil
	end
	local ok, v = pcall(function() return o.Get_Hull() end)
	if ok and type(v) == "number" then
		return v
	end
	if not ok then
		SN_ApiFail("Get_Hull", v)
	end
	return nil
end

function SN_FindAll(tname)
	local ok, r = pcall(Find_All_Objects_Of_Type, tname)
	if ok and type(r) == "table" then
		return r
	end
	return {}
end

-- One object of type tname at pos for player: Spawn_Unit (a table; the entry of type tname is taken), else
-- Create_Generic_Object. Returns object, how.
function SN_Spawn(tname, pos, player)
	local ty = nil
	pcall(function() ty = Find_Object_Type(tname) end)
	if ty == nil then
		return nil, "no_type"
	end
	if pos == nil or player == nil then
		return nil, "no_pos_or_player"
	end
	local ok, r = pcall(Spawn_Unit, ty, pos, player)
	if ok and type(r) == "table" then
		local first = nil
		for _, o in ipairs(r) do
			if SN_Valid(o) then
				if string.upper(SN_Type(o)) == string.upper(tname) then
					return o, "spawn_unit"
				end
				if first == nil then
					first = o
				end
			end
		end
		if first ~= nil then
			return first, "spawn_unit_other:" .. SN_Type(first)
		end
	elseif ok and SN_Valid(r) then
		return r, "spawn_unit_single"
	end
	if not ok then
		SN_ApiFail("Spawn_Unit:" .. tname, r)
	end
	local ok2, o = pcall(Create_Generic_Object, ty, pos, player)
	if ok2 and SN_Valid(o) then
		return o, "generic"
	end
	if not ok2 then
		SN_ApiFail("Create_Generic_Object:" .. tname, o)
	end
	return nil, "spawn_failed"
end

function SN_Despawn(o)
	if SN_Valid(o) then
		pcall(function() o.Despawn() end)
	end
end

function SN_Camera(x, y, z)
	-- framing only, a position (Point_Camera_At(position) from a free-store hook worked, 2026-11-09)
	local p = SN_Pos(x, y, z)
	if p ~= nil then
		pcall(Point_Camera_At, p)
	end
end

-- ================================================================================================================
-- (A) tractor beam
-- ================================================================================================================

TB = { phase = "init", ti = 0, cur = nil, next_f = nil }

function TB_Base(dx, dy)
	local u = SN_CFG.tb.user_at
	return SN_Pos(u.x + dx, u.y + dy, u.z)
end

function TB_Start(F, tc)
	local cfg = SN_CFG.tb
	local u = cfg.user_at
	local t = { k = tc.k, name = tc.name, face = tc.face, d = tc.d, tc = tc, f0 = F, phase = "turn", errs = 0 }
	TB.cur = t
	local human = SN_Player(SN_CFG.human_faction)
	local how_u, how_x
	t.U, how_u = SN_Spawn(cfg.user, TB_Base(0, 0), human)
	-- toward: X on the line (+x); side: X north of U, turned to face U there, then teleported onto the line
	if tc.face == "side" then
		t.X, how_x = SN_Spawn(cfg.target, TB_Base(0, tc.d), PlayerObject)
	else
		t.X, how_x = SN_Spawn(cfg.target, TB_Base(tc.d, 0), PlayerObject)
	end
	local af0 = SN_Flag(t.U, "Is_Ability_Autofire", cfg.ability)
	SN_Call(t.U, "Make_Invulnerable", function() t.U.Make_Invulnerable(true) end)
	local afc = SN_Call(t.U, "Set_Single_Ability_Autofire", function() t.U.Set_Single_Ability_Autofire(cfg.ability, false) end)
	SN_Call(t.X, "Make_Invulnerable", function() t.X.Make_Invulnerable(true) end)
	SN_Call(t.X, "Prevent_All_Fire", function() t.X.Prevent_All_Fire(true) end)
	SN_Call(t.X, "Prevent_AI_Usage", function() t.X.Prevent_AI_Usage(true) end)
	SN_Rec("TTRIAL", "k=" .. t.k .. "|name=" .. t.name .. "|face=" .. t.face .. "|d=" .. SN_Num(t.d, "%.2f") ..
		"|U=" .. SN_Type(t.U) .. ":" .. SN_Id(t.U) .. ":" .. SN_Str(how_u) .. "@" .. SN_XY(t.U) .. "|X=" .. SN_Type(t.X) ..
		":" .. SN_Id(t.X) .. ":" .. SN_Str(how_x) .. "@" .. SN_XY(t.X) .. "|uown=" .. SN_Owner(t.U) .. "|xown=" ..
		SN_Owner(t.X) .. "|af0=" .. af0 .. "|afset=" .. afc .. "|af1=" .. SN_Flag(t.U, "Is_Ability_Autofire", cfg.ability) ..
		"|rdy=" .. SN_Flag(t.U, "Is_Ability_Ready", cfg.ability))
	if not SN_Valid(t.U) or not SN_Valid(t.X) then
		SN_Rec("ERR", "what=spawn_failed|k=" .. t.k)
		t.phase = "end"
		return
	end
	if cfg.camera then
		SN_Camera(u.x + 0.5 * tc.d, u.y, u.z)
	end
end

function TB_Activate(t, F, why)
	local cfg = SN_CFG.tb
	local ret = "dead"
	if SN_Valid(t.U) and SN_Valid(t.X) then
		local ok, r = pcall(function() return t.U.Activate_Ability(cfg.ability, t.X) end)
		if not ok then
			SN_ApiFail("Activate_Ability", r)
			ret = "error"
		elseif r == nil then
			ret = "nil"
		elseif r == false then
			ret = "false"
		elseif r == true then
			ret = "true"
		else
			ret = "obj"
		end
	end
	return ret
end

-- one poll of the trial: returns d, act, eff, rdy (strings for the flags)
function TB_Poll(t, F)
	local cfg = SN_CFG.tb
	local d = SN_Dxy(t.U, t.X)
	local act = SN_Flag(t.U, "Is_Ability_Active", cfg.ability)
	local eff = SN_Flag(t.X, "Is_Under_Effects_Of_Ability", cfg.ability)
	local rdy = SN_Flag(t.U, "Is_Ability_Ready", cfg.ability)
	return d, act, eff, rdy
end

function TB_TrialTick(t, F)
	local cfg = SN_CFG.tb
	local rel = F - t.f0
	if t.phase == "turn" then
		if rel >= cfg.face_delay and not t.turned then
			t.turned = true
			local r = SN_Call(t.X, "Turn_To_Face", function() t.X.Turn_To_Face(t.U) end)
			SN_Rec("TURN", "k=" .. t.k .. "|call=" .. r .. "|x=" .. SN_XY(t.X))
		end
		if rel >= cfg.turn_frames then
			if t.face == "side" then
				local u = cfg.user_at
				local p = SN_Pos(u.x + t.d, u.y, u.z)
				local r = SN_Call(t.X, "Teleport", function() t.X.Teleport(p) end)
				SN_Rec("TELE", "k=" .. t.k .. "|call=" .. r .. "|x=" .. SN_XY(t.X) .. "|u=" .. SN_XY(t.U))
			end
			t.phase = "settle"
			t.fs = F
		end
		return
	end
	if t.phase == "settle" then
		if F - t.fs >= cfg.post_tele then
			local d, act, eff, rdy = TB_Poll(t, F)
			t.ux0, t.uy0 = SN_XYn(t.U)
			SN_Rec("PRE", "k=" .. t.k .. "|u=" .. SN_XY(t.U) .. "|x=" .. SN_XY(t.X) .. "|d=" .. SN_Num(d, "%.2f") .. "|act=" ..
				act .. "|eff=" .. eff .. "|rdy=" .. rdy)
			t.fact = F
			t.ret = TB_Activate(t, F, "order")
			t.nact = 1
			SN_Rec("ACT", "k=" .. t.k .. "|why=order|ret=" .. t.ret .. "|rdy_after=" ..
				SN_Flag(t.U, "Is_Ability_Ready", cfg.ability) .. "|act_after=" .. SN_Flag(t.U, "Is_Ability_Active", cfg.ability))
			t.phase = "watch"
			t.prev_d = d
		end
		return
	end
	if t.phase == "watch" then
		local r = F - t.fact
		local d, act, eff, rdy = TB_Poll(t, F)
		local ux, uy = SN_XYn(t.U)
		local um = "?"
		if ux ~= nil and t.ux0 ~= nil then
			um = SN_Num(SN_Sqrt((ux - t.ux0) * (ux - t.ux0) + (uy - t.uy0) * (uy - t.uy0)), "%.2f")
		end
		SN_Rec("P", "k=" .. t.k .. "|r=" .. r .. "|u=" .. SN_XY(t.U) .. "|x=" .. SN_XY(t.X) .. "|d=" .. SN_Num(d, "%.2f") ..
			"|um=" .. um .. "|act=" .. act .. "|eff=" .. eff .. "|rdy=" .. rdy .. "|st=" .. tostring(t.stepping == true))
		if t.eng == nil and (act == "1" or eff == "1") then
			t.eng = F
			SN_Rec("ENG", "k=" .. t.k .. "|r=" .. r .. "|d=" .. SN_Num(d, "%.2f") .. "|prev_d=" .. SN_Num(t.prev_d, "%.2f") ..
				"|um=" .. um .. "|act=" .. act .. "|eff=" .. eff .. "|stepping=" .. tostring(t.stepping == true) .. "|nact=" ..
				t.nact)
		end
		t.prev_d = d
		if t.eng ~= nil then
			if F - t.eng >= cfg.tail then
				t.phase = "end"
			end
			return
		end
		-- no walk: step X toward U (keeps X's facing), repeat the order every poll
		if r >= cfg.step_after and not t.stepping and ux ~= nil and t.ux0 ~= nil and
			(ux - t.ux0) * (ux - t.ux0) + (uy - t.uy0) * (uy - t.uy0) < 25 then
			t.stepping = true
			SN_Rec("STEP", "k=" .. t.k .. "|start=1|d=" .. SN_Num(d, "%.2f") .. "|um=" .. um)
		end
		if t.stepping and d ~= nil and d > 0 then
			local xx, xy = SN_XYn(t.X)
			if xx ~= nil then
				local s = cfg.step / d
				local p = SN_Pos(xx + (ux - xx) * s, xy + (uy - xy) * s, SN_CFG.tb.user_at.z)
				SN_Call(t.X, "Teleport", function() t.X.Teleport(p) end)
			end
			local ret = TB_Activate(t, F, "step")
			t.nact = t.nact + 1
			if ret ~= t.last_step_ret then
				t.last_step_ret = ret
				SN_Rec("ACT", "k=" .. t.k .. "|why=step|ret=" .. ret .. "|nact=" .. t.nact)
			end
		end
		if r >= cfg.watch or (d ~= nil and d < SN_CFG.tb.R - cfg.stop_below) then
			SN_Rec("NOENG", "k=" .. t.k .. "|r=" .. r .. "|d=" .. SN_Num(d, "%.2f") .. "|um=" .. um .. "|nact=" .. t.nact)
			t.phase = "end"
		end
		return
	end
end

function TB_EndTrial(t, F)
	local cfg = SN_CFG.tb
	if t.phase == "end" then
		-- the target first (the beam ends with it), the user 15 frames later
		SN_Despawn(t.X)
		t.phase = "remove"
		t.fr = F
		return
	end
	if t.phase == "remove" and F - t.fr >= 15 then
		SN_Despawn(t.U)
		SN_Rec("TEND", "k=" .. t.k .. "|name=" .. t.name .. "|eng=" .. tostring(t.eng) .. "|fact=" .. tostring(t.fact) ..
			"|aborted=" .. tostring(t.aborted))
		TB.cur = nil
		TB.phase = "pause"
		TB.next_f = F + cfg.gap
	end
end

function TB_Tick(F)
	local cfg = SN_CFG.tb
	if TB.phase == "init" then
		if TB.next_f == nil then
			TB.next_f = cfg.start_frame
			SN_Say("TEXT_S78N_TB_ARMED")
			SN_Rec("TCFG", "R=" .. SN_Num(cfg.R, "%.2f") .. "|ex=" .. SN_Num(cfg.ex, "%.2f") .. "|ey=" .. SN_Num(cfg.ey, "%.2f") ..
				"|user=" .. cfg.user .. "|target=" .. cfg.target .. "|trials=" .. table.getn(cfg.trials))
		end
		if F >= TB.next_f then
			TB.phase = "pause"
		end
		return
	end
	if TB.phase == "pause" then
		if F >= TB.next_f then
			TB.ti = TB.ti + 1
			local tc = cfg.trials[TB.ti]
			if tc == nil then
				SN_Say("TEXT_S78N_TB_DONE")
				SN_Finish("trials=" .. (TB.ti - 1))
				return
			end
			TB.phase = "trial"
			TB_Start(F, tc)
		end
		return
	end
	local t = TB.cur
	if t == nil then
		return
	end
	if t.phase ~= "end" and t.phase ~= "remove" then
		local ok, err = pcall(TB_TrialTick, t, F)
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
		if t.phase ~= "end" and F - t.f0 > cfg.turn_frames + cfg.post_tele + cfg.watch + cfg.tail + 300 then
			t.phase = "end"
			t.aborted = "timeout"
		end
	end
	if t.phase == "end" or t.phase == "remove" then
		TB_EndTrial(t, F)
	end
end

-- ================================================================================================================
-- (B) Combat_Bonus aura on an allied player's units
-- ================================================================================================================

CB = { phase = "init", f0 = nil, units = {}, hero = nil, last = {}, done_steps = {}, invuln = nil }

function CB_Rel(site, off)
	return SN_Pos(site.x + off[1], site.y + off[2], site.z)
end

function CB_Owners()
	return { own = SN_Player(SN_CFG.human_faction), ally = SN_Player(SN_CFG.ally_faction), enemy = PlayerObject }
end

function CB_Rel_Name(a, b)
	-- "1" allied, "0" not, "?" unreadable
	if a == nil or b == nil then
		return "-"
	end
	local ok, v = pcall(function() return a.Is_Ally(b) end)
	if ok then
		if v then
			return "1"
		end
		return "0"
	end
	SN_ApiFail("Is_Ally", v)
	return "?"
end

function CB_AllyRecord(kind)
	local o = CB_Owners()
	local hum = "?"
	pcall(function() hum = tostring(o.ally.Is_Human()) end)
	SN_Rec(kind, "own_ally=" .. CB_Rel_Name(o.own, o.ally) .. "|ally_own=" .. CB_Rel_Name(o.ally, o.own) .. "|own_enemy=" ..
		CB_Rel_Name(o.own, o.enemy) .. "|ally_enemy=" .. CB_Rel_Name(o.ally, o.enemy) .. "|own_own=" ..
		CB_Rel_Name(o.own, o.own) .. "|ally_player=" .. tostring(o.ally ~= nil) .. "|ally_human=" .. SN_Str(hum))
	return CB_Rel_Name(o.own, o.ally)
end

function CB_SpawnSet(set, site)
	local cfg = SN_CFG.cb
	local o = CB_Owners()
	for _, role in ipairs({ "own", "ally", "enemy" }) do
		local off = cfg[set][role]
		local u, how = SN_Spawn(cfg.unit, CB_Rel(site, off), o[role])
		local e = { o = u, role = role, set = set, key = set .. "_" .. role }
		table.insert(CB.units, e)
		SN_Rec("UNIT", "key=" .. e.key .. "|id=" .. SN_Id(u) .. "|type=" .. SN_Type(u) .. "|how=" .. SN_Str(how) .. "|owner=" ..
			SN_Owner(u) .. "|xy=" .. SN_XY(u) .. "|want=" .. string.format("%.1f,%.1f", site.x + off[1], site.y + off[2]) ..
			"|hull=" .. SN_Num(SN_Hull(u), "%.4f"))
	end
end

function CB_Protect(set)
	local cfg = SN_CFG.cb
	for _, e in ipairs(CB.units) do
		if e.set == set then
			local u = e.o
			SN_Call(u, "Prevent_AI_Usage", function() u.Prevent_AI_Usage(true) end)
			SN_Call(u, "Prevent_All_Fire", function() u.Prevent_All_Fire(true) end)
			SN_Call(u, "Make_Invulnerable", function() u.Make_Invulnerable(CB.invuln) end)
			e.prot = true
		end
	end
end

function CB_Damage(set)
	local cfg = SN_CFG.cb
	for _, e in ipairs(CB.units) do
		if e.set == set then
			local u = e.o
			local h0 = SN_Hull(u)
			local r = SN_Call(u, "Take_Damage", function() u.Take_Damage(cfg.dmg) end)
			SN_Rec("DMG", "key=" .. e.key .. "|amount=" .. SN_Num(cfg.dmg, "%.1f") .. "|call=" .. r .. "|hull_before=" ..
				SN_Num(h0, "%.4f") .. "|invuln=" .. tostring(CB.invuln))
		end
	end
end

function CB_Snap(kind, set)
	for _, e in ipairs(CB.units) do
		if set == nil or e.set == set then
			SN_Rec(kind, "key=" .. e.key .. "|hull=" .. SN_Num(SN_Hull(e.o), "%.4f") .. "|owner=" .. SN_Owner(e.o) .. "|xy=" ..
				SN_XY(e.o) .. "|dhero=" .. SN_Num(SN_Dxy(e.o, CB.hero), "%.1f"))
		end
	end
end

-- every poll: a HULL record for every hull change (> 0.0005) of a test unit
function CB_Watch(F)
	for _, e in ipairs(CB.units) do
		local h = SN_Hull(e.o)
		local hs = SN_Num(h, "%.4f")
		local last = CB.last[e.key]
		if last ~= nil and hs ~= last.s then
			local big = true
			if h ~= nil and last.v ~= nil then
				local dv = h - last.v
				if dv < 0 then
					dv = -dv
				end
				big = dv > 0.0005
			end
			if big then
				SN_Rec("HULL", "key=" .. e.key .. "|from=" .. last.s .. "|to=" .. hs)
			end
		end
		CB.last[e.key] = { s = hs, v = h }
	end
end

function CB_SpawnHero()
	local cfg = SN_CFG.cb
	local o = CB_Owners()
	local h, how = nil, "none"
	for _, name in ipairs({ cfg.hero, cfg.hero_alt }) do
		if h == nil then
			local u, hw = SN_Spawn(name, CB_Rel(cfg.site, { 0, 0 }), o.own)
			how = name .. ":" .. hw
			if u ~= nil and string.upper(SN_Type(u)) == string.upper(cfg.hero) then
				h = u
			end
			if h == nil then
				for _, x in ipairs(SN_FindAll(cfg.hero)) do
					if h == nil and SN_Valid(x) and SN_Owner(x) == string.upper(SN_CFG.human_faction) then
						h = x
					end
				end
			end
		end
	end
	CB.hero = h
	if h ~= nil then
		SN_Call(h, "Make_Invulnerable", function() h.Make_Invulnerable(true) end)
		SN_Call(h, "Prevent_All_Fire", function() h.Prevent_All_Fire(true) end)
		SN_Call(h, "Prevent_AI_Usage", function() h.Prevent_AI_Usage(true) end)
	end
	SN_Rec("HERO", "type=" .. SN_Type(h) .. "|id=" .. SN_Id(h) .. "|how=" .. SN_Str(how) .. "|owner=" .. SN_Owner(h) ..
		"|xy=" .. SN_XY(h))
end

function CB_Step(name, F)
	-- each named step runs once, at its time
	local cfg = SN_CFG.cb
	if CB.done_steps[name] then
		return false
	end
	if F - CB.f0 < cfg["t_" .. name] + (CB.shift or 0) then
		return false
	end
	CB.done_steps[name] = true
	SN_Rec("CSTEP", "s=" .. name)
	return true
end

function CB_Tick(F)
	local cfg = SN_CFG.cb
	if CB.phase == "init" then
		if CB.next_f == nil then
			CB.next_f = cfg.start_frame
			CB.invuln = cfg.invuln
			SN_Say("TEXT_S78N_CB_ARMED")
			SN_Rec("CCFG", "unit=" .. cfg.unit .. "|health=" .. SN_Num(cfg.health, "%.1f") .. "|dmg=" .. SN_Num(cfg.dmg, "%.1f") ..
				"|bonus=" .. SN_Num(cfg.bonus, "%.3f") .. "|hero=" .. cfg.hero .. "|site=" ..
				string.format("%.0f,%.0f", cfg.site.x, cfg.site.y) .. "|far=" ..
				string.format("%.0f,%.0f", cfg.far_site.x, cfg.far_site.y))
		end
		if F >= CB.next_f then
			CB.phase = "run"
			CB.f0 = F
			local rel = CB_AllyRecord("ALLY")
			if rel ~= "1" and cfg.fix_ally then
				local o = CB_Owners()
				local r1, r2 = "-", "-"
				if o.own ~= nil and o.ally ~= nil then
					local ok1, e1 = pcall(function() o.own.Make_Ally(o.ally) end)
					local ok2, e2 = pcall(function() o.ally.Make_Ally(o.own) end)
					r1, r2 = tostring(ok1), tostring(ok2)
					if not ok1 then SN_ApiFail("Make_Ally", e1) end
					if not ok2 then SN_ApiFail("Make_Ally", e2) end
				end
				SN_Rec("ALLYFIX", "own_make=" .. r1 .. "|ally_make=" .. r2)
				CB_AllyRecord("ALLY2")
			end
			SN_Camera(cfg.site.x, cfg.site.y, cfg.site.z)
		end
		return
	end
	if CB.phase ~= "run" then
		return
	end
	CB_Watch(F)
	if CB_Step("pre_spawn", F) then
		CB_SpawnSet("pre", cfg.site)
	end
	if CB_Step("pre_protect", F) then
		CB_Protect("pre")
		CB_Snap("H", "pre")
	end
	if CB_Step("pre_dmg", F) then
		CB_Damage("pre")
	end
	if CB_Step("pre_check", F) then
		CB_Snap("H", "pre")
		-- invulnerability blocked the Lua damage (no drop on the own unit): vulnerable from now on, damage again
		local own = nil
		for _, e in ipairs(CB.units) do
			if e.key == "pre_own" then
				own = e.o
			end
		end
		local h = SN_Hull(own)
		if CB.invuln and h ~= nil and h > 0.999 then
			CB.invuln = false
			SN_Rec("INVULN_BLOCKS", "hull=" .. SN_Num(h, "%.4f"))
			CB_Protect("pre")
			CB_Damage("pre")
			CB.shift = cfg.recheck
		end
	end
	if CB_Step("hero", F) then
		CB_Snap("H", "pre")
		CB_SpawnHero()
	end
	if CB_Step("post_spawn", F) then
		CB_Snap("H", "pre")
		CB_SpawnSet("near", cfg.site)
		CB_SpawnSet("far", cfg.far_site)
	end
	if CB_Step("post_protect", F) then
		CB_Protect("near")
		CB_Protect("far")
	end
	if CB_Step("post_h0", F) then
		CB_Snap("H0", nil)
	end
	if CB_Step("post_dmg", F) then
		CB_Damage("near")
		CB_Damage("far")
	end
	if CB_Step("post_h1", F) then
		CB_Snap("H1", nil)
	end
	if CB_Step("hero_off", F) then
		SN_Rec("HEROOFF", "valid=" .. tostring(SN_Valid(CB.hero)))
		SN_Despawn(CB.hero)
	end
	if CB_Step("end", F) then
		CB_Snap("HEND", nil)
		for _, e in ipairs(CB.units) do
			SN_Despawn(e.o)
		end
		CB.phase = "done"
		SN_Say("TEXT_S78N_CB_DONE")
		SN_Finish("units=" .. table.getn(CB.units))
	end
end

SN_PROBES["Space"] = { name = "tb", tick = TB_Tick }
SN_PROBES["Land"] = { name = "cb", tick = CB_Tick }

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
	local pr = SN_PROBES[mode]
	local human_ok = false
	pcall(function() human_ok = Find_Player(SN_CFG.human_faction).Is_Human() == true end)
	if pr and not human_ok and SN.tries < 60 then
		SN.pending = true
		return
	end
	if pr and fac == SN_CFG.ai_faction and human ~= true and human_ok and not SN_ARMED_ONCE then
		SN_ARMED_ONCE = true
		SN.on = true
		SN.pr = pr
		SN.probe = pr.name
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
	local ok, err = pcall(SN.pr.tick, F)
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
-- ==== end of s78n_probe.lua ====
