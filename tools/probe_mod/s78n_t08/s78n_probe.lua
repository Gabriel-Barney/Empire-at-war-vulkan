-- ==== s78n t08: tractor-beam range footprint (A) and Combat_Bonus aura on an allied player (B) ====
-- tools/probe_mod/s78n_t08/s78n_probe.lua. build_s78n_mod.py appends, to the shipped BusyTacticalFreeStore.lua, a
-- generated SN_CFG table and this file.
--
-- (A) Space (docs/re/TACTICAL_COMBAT.md sec. 5.4.5, second version after run 20261109-200249): does a
--     Star_Destroyer's TRACTOR_BEAM start at XY centre distance d <= R + E, E = the target's collision-bound
--     model Y half x scale when its facing is along the line to the user (|f.u| > 0.7071), else the X half?
--     Per trial a fresh human Star_Destroyer U and an AI Nebulon_B_Frigate X teleported to exact spots d apart
--     along +x, X turned at once (Face_Immediate) to face U ("along") or +y ("across"), both locomotors
--     suspended, X's facing read from two model bones until settled, then U.Activate_Ability("TRACTOR_BEAM",
--     X). In place or not; without a beam after `hold` frames X is stepped closer (TV_* below).
-- (B) Land (sec. 5.4.3 item 4; not registered with SN_CFG.space_only): does Emperor_Palpatine's
--     Emperor_Land_Combat_Bonus (Health_Bonus 0.25) reach the AT_ST_Walker of an ALLIED AI player (Underworld,
--     same team), as the human's own (control) and not the enemy's (control), near the Emperor and about 1200+ away (the port has no aura radius)? Observable: Get_Hull after a
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
-- (Get_ID is a player method); Game_Message for the driver's cues; Point_Camera_At with a position;
-- Get_Bone_Position only with bone names read from the install's model (a failed engine read stops the script).

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
-- (A) tractor beam, second version (TACTICAL_COMBAT sec. 5.4.5; run 20261109-200249 left the target's facing open)
-- ================================================================================================================
-- Per trial: spawn U (human Star_Destroyer) and X (AI Nebulon_B_Frigate); `place` frames later Teleport both to their
-- exact spots (U at user_at, X at user_at + (d, 0)), U.Face_Immediate(X's spot), X.Face_Immediate(U's spot) ("along")
-- or X.Face_Immediate(X's spot + (0, face_dist)) ("across", exactly 90 deg), Suspend_Locomotor(true) on X and on U.
-- X's facing is read every settle_poll frames from two model bones (Get_Bone_Position, front - rear); the order is
-- given when two consecutive reads agree (after settle_min), else after settle_max / settle_nobone. Then
-- U.Activate_Ability("TRACTOR_BEAM", X) and a poll record per service. No beam after `hold` frames with U still:
-- X is teleported `step` closer per poll (facing kept) and the order repeated, so the threshold is also bracketed.

TV = { phase = "init", ti = 0, cur = nil, next_f = nil, ulock = nil }

function TV_At(dx, dy)
	local u = SN_CFG.tv.user_at
	return SN_Pos(u.x + dx, u.y + dy, u.z)
end

function TV_D(ax, ay, bx, by)
	if ax == nil or bx == nil then
		return nil
	end
	return SN_Sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by))
end

-- x, y of a bone of o (names from the install's model only: a failed engine read can stop a retail script)
function TV_Bone(o, name)
	if name == nil or name == "" or not SN_Valid(o) then
		return nil
	end
	local ok, x, y = pcall(function()
		local a, b, c = o.Get_Bone_Position(name).Get_XYZ()
		if type(a) == "table" then
			return a[1], a[2]
		end
		return a, b
	end)
	if ok and type(x) == "number" and type(y) == "number" then
		return x, y
	end
	if not ok then
		SN_ApiFail("Get_Bone_Position", x)
	end
	return nil
end

-- X's facing from its front and rear bones: { fx, fy (unit, rear -> front), ax, ay (front), bx, by (rear) } or nil
function TV_Facing(o)
	local cfg = SN_CFG.tv
	local ax, ay = TV_Bone(o, cfg.bone_front)
	local bx, by = TV_Bone(o, cfg.bone_rear)
	if ax == nil or bx == nil then
		return nil
	end
	local l = TV_D(ax, ay, bx, by)
	if l == nil or l < 1 then
		return nil
	end
	return { fx = (ax - bx) / l, fy = (ay - by) / l, ax = ax, ay = ay, bx = bx, by = by }
end

-- record fields: fu = |f . u| with u the unit vector X -> U, the facing and the raw bone positions
function TV_FaceStr(t, fc)
	if fc == nil then
		return "fu=?|fx=?|fy=?"
	end
	local fu = "?"
	local ux, uy = SN_XYn(t.U)
	local xx, xy = SN_XYn(t.X)
	local l = TV_D(ux, uy, xx, xy)
	if l ~= nil and l > 0 then
		local v = (fc.fx * (ux - xx) + fc.fy * (uy - xy)) / l
		if v < 0 then
			v = -v
		end
		fu = SN_Num(v, "%.4f")
	end
	return "fu=" .. fu .. "|fx=" .. SN_Num(fc.fx, "%.4f") .. "|fy=" .. SN_Num(fc.fy, "%.4f") .. "|bf=" ..
		string.format("%.2f,%.2f", fc.ax, fc.ay) .. "|br=" .. string.format("%.2f,%.2f", fc.bx, fc.by)
end

function TV_Activate(t)
	local cfg = SN_CFG.tv
	local ret = "dead"
	if SN_Valid(t.U) and SN_Valid(t.X) then
		-- the form that worked in run 20261109-200249: (name, game object)
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

function TV_Start(F, tc)
	local cfg = SN_CFG.tv
	local u = cfg.user_at
	local t = { k = tc.k, name = tc.name, face = tc.face, d = tc.d, f0 = F, phase = "place", errs = 0, nread = 0,
		nact = 0, polls = 0 }
	TV.cur = t
	local human = SN_Player(SN_CFG.human_faction)
	local how_u, how_x
	t.U, how_u = SN_Spawn(cfg.user, TV_At(0, 0), human)
	t.X, how_x = SN_Spawn(cfg.target, TV_At(tc.d, 0), PlayerObject)
	local af0 = SN_Flag(t.U, "Is_Ability_Autofire", cfg.ability)
	SN_Call(t.U, "Make_Invulnerable", function() t.U.Make_Invulnerable(true) end)
	local afc = SN_Call(t.U, "Set_Single_Ability_Autofire", function() t.U.Set_Single_Ability_Autofire(cfg.ability, false) end)
	SN_Call(t.X, "Make_Invulnerable", function() t.X.Make_Invulnerable(true) end)
	SN_Call(t.X, "Prevent_All_Fire", function() t.X.Prevent_All_Fire(true) end)
	SN_Call(t.X, "Prevent_AI_Usage", function() t.X.Prevent_AI_Usage(true) end)
	SN_Rec("VTRIAL", "k=" .. t.k .. "|name=" .. t.name .. "|face=" .. t.face .. "|d=" .. SN_Num(t.d, "%.2f") ..
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

-- exact spots, facings and locks
function TV_Place(t, F)
	local cfg = SN_CFG.tv
	local pu = TV_At(0, 0)
	local px = TV_At(t.d, 0)
	local rtu = SN_Call(t.U, "Teleport", function() t.U.Teleport(pu) end)
	local rtx = SN_Call(t.X, "Teleport", function() t.X.Teleport(px) end)
	local aim_u = TV_At(t.d, 0)
	local aim_x = TV_At(0, 0)
	if t.face == "across" then
		aim_x = TV_At(t.d, cfg.face_dist)
	end
	local rfu = SN_Call(t.U, "Face_Immediate", function() t.U.Face_Immediate(aim_u) end)
	local rfx = SN_Call(t.X, "Face_Immediate", function() t.X.Face_Immediate(aim_x) end)
	local rlx, rlu = "-", "-"
	if cfg.xlock then
		rlx = SN_Call(t.X, "Suspend_Locomotor", function() t.X.Suspend_Locomotor(true) end)
	end
	t.ulocked = TV.ulock == true
	if t.ulocked then
		rlu = SN_Call(t.U, "Suspend_Locomotor", function() t.U.Suspend_Locomotor(true) end)
	end
	SN_Rec("PLACE", "k=" .. t.k .. "|tele_u=" .. rtu .. "|tele_x=" .. rtx .. "|face_u=" .. rfu .. "|face_x=" .. rfx ..
		"|lock_x=" .. rlx .. "|lock_u=" .. rlu .. "|u=" .. SN_XY(t.U) .. "|x=" .. SN_XY(t.X) .. "|d=" ..
		SN_Num(SN_Dxy(t.U, t.X), "%.2f"))
	t.phase = "settle"
	t.fp = F
	t.next_read = F
end

function TV_Poll(t)
	local cfg = SN_CFG.tv
	local d = SN_Dxy(t.U, t.X)
	local act = SN_Flag(t.U, "Is_Ability_Active", cfg.ability)
	local eff = SN_Flag(t.X, "Is_Under_Effects_Of_Ability", cfg.ability)
	local rdy = SN_Flag(t.U, "Is_Ability_Ready", cfg.ability)
	return d, act, eff, rdy
end

function TV_Settle(t, F)
	local cfg = SN_CFG.tv
	if F < t.next_read then
		return
	end
	t.next_read = F + cfg.settle_poll
	local since = F - t.fp
	local fc = TV_Facing(t.X)
	local xx, xy = SN_XYn(t.X)
	t.nread = t.nread + 1
	local same = "0"
	if fc ~= nil and t.lastfc ~= nil and xx ~= nil and t.lx ~= nil then
		local d1 = TV_D(fc.ax, fc.ay, t.lastfc.ax, t.lastfc.ay)
		local d2 = TV_D(fc.bx, fc.by, t.lastfc.bx, t.lastfc.by)
		local d3 = TV_D(xx, xy, t.lx, t.ly)
		if d1 <= cfg.settle_tol and d2 <= cfg.settle_tol and d3 <= cfg.settle_tol then
			same = "1"
		end
	end
	SN_Rec("FACE", "k=" .. t.k .. "|w=settle|i=" .. t.nread .. "|since=" .. since .. "|same=" .. same .. "|u=" ..
		SN_XY(t.U) .. "|x=" .. SN_XY(t.X) .. "|" .. TV_FaceStr(t, fc))
	t.lastfc = fc
	t.lx, t.ly = xx, xy
	local why = nil
	if since >= cfg.settle_min and same == "1" then
		why = "agree"
	elseif fc == nil and since >= cfg.settle_nobone then
		why = "no_facing_read"
	elseif since >= cfg.settle_max then
		why = "timeout"
	end
	if why == nil then
		return
	end
	SN_Rec("SETTLE", "k=" .. t.k .. "|why=" .. why .. "|since=" .. since .. "|reads=" .. t.nread)
	-- the order
	local d, act, eff, rdy = TV_Poll(t)
	t.ux0, t.uy0 = SN_XYn(t.U)
	SN_Rec("PRE", "k=" .. t.k .. "|u=" .. SN_XY(t.U) .. "|x=" .. SN_XY(t.X) .. "|d=" .. SN_Num(d, "%.2f") .. "|act=" ..
		act .. "|eff=" .. eff .. "|rdy=" .. rdy .. "|ulock=" .. tostring(t.ulocked) .. "|" .. TV_FaceStr(t, TV_Facing(t.X)))
	t.fact = F
	t.ret = TV_Activate(t)
	t.nact = 1
	SN_Rec("ACT", "k=" .. t.k .. "|why=order|ret=" .. t.ret .. "|rdy_after=" ..
		SN_Flag(t.U, "Is_Ability_Ready", cfg.ability) .. "|act_after=" .. SN_Flag(t.U, "Is_Ability_Active", cfg.ability))
	t.phase = "watch"
	t.prev_d = d
	t.prev2_d = d
end

function TV_Watch(t, F)
	local cfg = SN_CFG.tv
	local r = F - t.fact
	local d, act, eff, rdy = TV_Poll(t)
	local ux, uy = SN_XYn(t.U)
	local umv = nil
	if ux ~= nil and t.ux0 ~= nil then
		umv = TV_D(ux, uy, t.ux0, t.uy0)
	end
	local um = SN_Num(umv, "%.2f")
	t.polls = t.polls + 1
	local fs = ""
	if t.polls >= cfg.face_every then
		t.polls = 0
		fs = "|" .. TV_FaceStr(t, TV_Facing(t.X))
	end
	SN_Rec("P", "k=" .. t.k .. "|r=" .. r .. "|u=" .. SN_XY(t.U) .. "|x=" .. SN_XY(t.X) .. "|d=" .. SN_Num(d, "%.2f") ..
		"|um=" .. um .. "|act=" .. act .. "|eff=" .. eff .. "|rdy=" .. rdy .. "|st=" .. tostring(t.stepping == true) .. fs)
	if t.eng == nil and (act == "1" or eff == "1") then
		t.eng = F
		SN_Rec("ENG", "k=" .. t.k .. "|r=" .. r .. "|d=" .. SN_Num(d, "%.2f") .. "|prev_d=" .. SN_Num(t.prev_d, "%.2f") ..
			"|prev2_d=" .. SN_Num(t.prev2_d, "%.2f") .. "|um=" .. um .. "|act=" .. act .. "|eff=" .. eff .. "|stepping=" ..
			tostring(t.stepping == true) .. "|nact=" .. t.nact .. "|ulock=" .. tostring(t.ulocked) .. "|" ..
			TV_FaceStr(t, TV_Facing(t.X)))
	end
	t.prev2_d = t.prev_d
	t.prev_d = d
	if t.eng ~= nil then
		if F - t.eng >= cfg.tail then
			t.phase = "end"
		end
		return
	end
	-- a locked user that does not start the beam on a control at R - 50: the lock blocks it; unlock for good
	if t.ulocked and string.sub(t.name, 1, 4) == "ctrl" and r >= cfg.ctrl_wait then
		t.ulocked = false
		TV.ulock = false
		local c = SN_Call(t.U, "Suspend_Locomotor", function() t.U.Suspend_Locomotor(false) end)
		local ret = TV_Activate(t)
		t.nact = t.nact + 1
		SN_Rec("ULOCK", "k=" .. t.k .. "|off=1|call=" .. c .. "|r=" .. r .. "|d=" .. SN_Num(d, "%.2f") .. "|ret=" .. ret)
		return
	end
	-- no beam at the fixed distance: step X toward U (Teleport keeps the facing), repeat the order every poll
	if r >= cfg.hold and not t.stepping and umv ~= nil and umv < 1 then
		t.stepping = true
		SN_Rec("STEP", "k=" .. t.k .. "|start=1|r=" .. r .. "|d=" .. SN_Num(d, "%.2f") .. "|um=" .. um .. "|" ..
			TV_FaceStr(t, TV_Facing(t.X)))
	end
	if t.stepping and d ~= nil and d > 0 then
		local xx, xy = SN_XYn(t.X)
		if xx ~= nil and ux ~= nil then
			local s = cfg.step / d
			local p = SN_Pos(xx + (ux - xx) * s, xy + (uy - xy) * s, cfg.user_at.z)
			SN_Call(t.X, "Teleport", function() t.X.Teleport(p) end)
		end
		local ret = TV_Activate(t)
		t.nact = t.nact + 1
		if ret ~= t.last_step_ret then
			t.last_step_ret = ret
			SN_Rec("ACT", "k=" .. t.k .. "|why=step|ret=" .. ret .. "|nact=" .. t.nact)
		end
	end
	if r >= cfg.watch or (t.stepping and d ~= nil and d < cfg.R - cfg.stop_below) then
		SN_Rec("NOENG", "k=" .. t.k .. "|r=" .. r .. "|d=" .. SN_Num(d, "%.2f") .. "|um=" .. um .. "|nact=" .. t.nact ..
			"|" .. TV_FaceStr(t, TV_Facing(t.X)))
		t.phase = "end"
	end
end

function TV_TrialTick(t, F)
	local cfg = SN_CFG.tv
	if t.phase == "place" then
		if F - t.f0 >= cfg.place then
			TV_Place(t, F)
		end
		return
	end
	if t.phase == "settle" then
		TV_Settle(t, F)
		return
	end
	if t.phase == "watch" then
		TV_Watch(t, F)
	end
end

function TV_EndTrial(t, F)
	local cfg = SN_CFG.tv
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
		TV.cur = nil
		TV.phase = "pause"
		TV.next_f = F + cfg.gap
	end
end

function TV_Tick(F)
	local cfg = SN_CFG.tv
	if TV.phase == "init" then
		if TV.next_f == nil then
			TV.next_f = cfg.start_frame
			TV.ulock = cfg.ulock == true
			SN_Say("TEXT_S78N_TB_ARMED")
			SN_Rec("VCFG", "R=" .. SN_Num(cfg.R, "%.2f") .. "|ea=" .. SN_Num(cfg.e_along, "%.3f") .. "|ec=" ..
				SN_Num(cfg.e_across, "%.3f") .. "|oa=" .. SN_Num(cfg.old_along, "%.3f") .. "|oc=" ..
				SN_Num(cfg.old_across, "%.3f") .. "|bf=" .. SN_Str(cfg.bone_front) .. "|br=" .. SN_Str(cfg.bone_rear) ..
				"|user=" .. cfg.user .. "|target=" .. cfg.target .. "|trials=" .. table.getn(cfg.trials) .. "|ulock=" ..
				tostring(cfg.ulock) .. "|xlock=" .. tostring(cfg.xlock) .. "|space_only=" .. tostring(SN_CFG.space_only))
		end
		if F >= TV.next_f then
			TV.phase = "pause"
		end
		return
	end
	if TV.phase == "pause" then
		if F >= TV.next_f then
			TV.ti = TV.ti + 1
			local tc = cfg.trials[TV.ti]
			if tc == nil then
				SN_Say("TEXT_S78N_TB_DONE")
				SN_Finish("trials=" .. (TV.ti - 1))
				return
			end
			TV.phase = "trial"
			TV_Start(F, tc)
		end
		return
	end
	local t = TV.cur
	if t == nil then
		return
	end
	if t.phase ~= "end" and t.phase ~= "remove" then
		local ok, err = pcall(TV_TrialTick, t, F)
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
		if t.phase ~= "end" and t.phase ~= "remove" and F - t.f0 > cfg.place + cfg.settle_max + cfg.watch + cfg.tail + 300 then
			t.phase = "end"
			t.aborted = "timeout"
		end
	end
	if t.phase == "end" or t.phase == "remove" then
		TV_EndTrial(t, F)
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

SN_PROBES["Space"] = { name = "tv", tick = TV_Tick }
if not SN_CFG.space_only then
	SN_PROBES["Land"] = { name = "cb", tick = CB_Tick }
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
