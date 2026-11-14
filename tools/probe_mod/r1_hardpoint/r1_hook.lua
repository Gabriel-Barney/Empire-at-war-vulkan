-- ==== R1 hardpoint probe (tools/probe_mod/r1_hardpoint/r1_hook.lua) ====
-- build_r1_mod.py appends a generated R1_CFG table and this file to the shipped BusyTacticalFreeStore.lua.
-- Question (roadmap r1 t01, docs/re/SPACE_AND_OPEN.md B.10, ABILITIES_HARDPOINTS 1.3, TACTICAL_COMBAT 2.9): what a
-- destroyed hardpoint looks like in retail (screenshots, taken by the driver) and three Lua-measurable facts:
--   (1) Take_Damage(n, "<HardPoint type name>") kills that hardpoint (vanilla story scripts rely on it);
--   (2) the same with a lower-case name and with the hardpoint's Collision_Mesh name (lookup rule);
--   (3) owner death kills every live hardpoint: one Death_Breakoff_Prop per live hardpoint that has one.
-- Name mode: the first kill (FL, type name) is checked at kill + R1_CFG.decide frames (its breakoff prop appeared, or
-- Get_Hull fell by about its hardpoint share, not by a hull hit). If not, the later A stages use the hardpoints'
-- Collision_Mesh names (stage.alt) instead, so the visual stages still kill hardpoints (record MODE).
-- Evidence per stage: Get_Hull (min(hull, hardpoint fraction) for types with Should_Be_Destroyed_When_All_Hardpoints_
-- Destroyed, default true), Get_Shield, and the number of objects of each breakoff-prop / death-clone / explosion type.
--
-- Arming: only the free store of the AI player whose faction is R1_CFG.ai_faction, in a Space battle, and only while
-- Find_Player(R1_CFG.human_faction).Is_Human() (the main-menu background battle has no human: SKIP). Base_Definitions
-- runs before PlayerObject is set, so the decision is retried from the first services (cu8g / oi lesson).
-- ServiceRate 0.05 (FreeStoreService on every free-store service, every 3rd frame); the shipped free store is skipped
-- while the probe runs (the AI's free units stay idle). No thread, no yield, every engine call in pcall. Methods are
-- only called on objects whose Is_Valid() is true (a failed engine read stops a retail script silently).
-- Output: strings "R1P|<KIND>|sid=..|n=..|f=<frame>|...|END" kept in R1_LOG; r1_run.py finds them with a read-only
-- heap scan. f = GetCurrentTime() * 30 rounded. Lua 5.0: no '#', no math library, table.getn.

R1_LOG = {}
R1_LOG2 = {}
R1_N = 0
R1_SID = string.sub((string.gsub(tostring(R1_LOG), "[^%w]", "")), -6)
R1 = { on = false, done = false, f = -1, tries = 0, pending = false, old_rate = nil, fac = "?", mode = "?",
	phase = "start", stage = 0, name_mode = "type", ships = {}, types = {}, last_poll = -1000, fast_until = -1 }
R1_API_BAD = {}
R1_MAX_RECORDS = 20000

if R1_CFG == nil then
	R1_CFG = { session = "?", ai_faction = "?", human_faction = "?", stages = {} }
end

function R1_Str(v)
	return (string.gsub(tostring(v), "[^%w%._%-:]", "_"))
end

function R1_Frame()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return tonumber(string.format("%.0f", t * 30))
	end
	return nil
end

function R1_Rec(kind, fields)
	pcall(function()
		if R1_N >= R1_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		R1_N = R1_N + 1
		local body = fields or ""
		if string.len(body) > 700 then
			body = string.sub(body, 1, 700) .. "~"
		end
		local f = R1_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		local base = "R1P|" .. kind .. "|sid=" .. R1_SID .. "|n=" .. R1_N .. "|f=" .. fs .. "|" .. body
		R1_LOG[R1_N] = base .. "|END"
		-- A second copy with different text (equal strings are one object in Lua), kept in another allocation: the heap
		-- scan lost single records in the r1b t07 runs (the parser merges the copies by sid + n).
		R1_LOG2[R1_N] = base .. "|dup=1|END"
	end)
end

function R1_ApiFail(tag, err)
	if not R1_API_BAD[tag] then
		R1_API_BAD[tag] = true
		R1_Rec("APIERR", "api=" .. R1_Str(tag) .. "|err=" .. string.sub(R1_Str(err), 1, 150))
	end
end

function R1_Say(id)
	R1_Rec("SAY", "text=" .. id)
	pcall(Game_Message, id)
end

function R1_Valid(o)
	if o == nil then
		return false
	end
	local ok, v = pcall(function() return o.Is_Valid() end)
	return ok and v == true
end

function R1_Num(v, fmt)
	if type(v) == "number" then
		return string.format(fmt or "%.4f", v)
	end
	return "?"
end

-- o.method() as a number, only on a valid object (nil otherwise).
function R1_Get(o, method)
	if not R1_Valid(o) then
		return nil
	end
	local v = nil
	local ok = pcall(function() v = o[method]() end)
	if ok and type(v) == "number" then
		return v
	end
	return nil
end

function R1_XY(o)
	if not R1_Valid(o) then
		return "?"
	end
	local ok, x, y = pcall(function()
		local a, b, c = o.Get_Position().Get_XYZ()
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

function R1_Id(o)
	if not R1_Valid(o) then
		return "?"
	end
	local id = nil
	pcall(function() id = o.Get_ID() end)
	if id == nil then
		return "?"
	end
	return R1_Str(id)
end

function R1_Pos(x, y, z)
	local ok, p = pcall(function() return Create_Position(x, y, z or 0) end)
	if ok then
		return p
	end
	R1_ApiFail("Create_Position", p)
	return nil
end

function R1_Player(name)
	local p = nil
	pcall(function() p = Find_Player(name) end)
	return p
end

-- Find_Object_Type once per name (nil: the type does not exist; then it is never counted).
function R1_Type(name)
	if name == nil or name == "" then
		return nil
	end
	local e = R1.types[name]
	if e == nil then
		local ty = nil
		pcall(function() ty = Find_Object_Type(name) end)
		e = { ty = ty }
		R1.types[name] = e
		if ty == nil then
			R1_Rec("NOTYPE", "type=" .. R1_Str(name))
		end
	end
	return e.ty
end

-- Number of live objects of a type (Find_All_Objects_Of_Type with the type wrapper: a type filter, LUA_API). "?" on
-- failure, "-" when the type does not exist.
function R1_Count(name)
	local ty = R1_Type(name)
	if ty == nil then
		return "-"
	end
	local ok, t = pcall(Find_All_Objects_Of_Type, ty)
	if ok and type(t) == "table" then
		return tostring(table.getn(t))
	end
	if not ok then
		R1_ApiFail("Find_All_Objects_Of_Type:" .. name, t)
	end
	return "?"
end

function R1_CountN(name)
	local c = tonumber(R1_Count(name))
	return c
end

-- "short=count;..." for the counted types (R1_CFG.counted: breakoff props, death clone DC, explosion EX).
function R1_Counts()
	local s = ""
	for _, c in ipairs(R1_CFG.counted or {}) do
		s = s .. c.short .. "=" .. R1_Count(c.name) .. ";"
	end
	return s
end

-- One ship's state: v=<valid> h=<Get_Hull> s=<Get_Shield> xy.
function R1_ShipState(tag)
	local o = R1.ships[tag]
	if o == nil then
		return tag .. "=none"
	end
	if not R1_Valid(o) then
		return tag .. "=dead"
	end
	return tag .. "=1:" .. R1_Num(R1_Get(o, "Get_Hull")) .. ":" .. R1_Num(R1_Get(o, "Get_Shield")) .. ":" .. R1_XY(o)
end

function R1_Poll(F, why)
	R1.last_poll = F
	R1_Rec("P", "why=" .. why .. "|st=" .. R1.stage .. "|" .. R1_ShipState("A") .. "|" .. R1_ShipState("B") .. "|c=" ..
		R1_Counts())
end

function R1_Spawn(tag, dx, dy)
	local cfg = R1_CFG
	local human = R1_Player(cfg.human_faction)
	local ty = R1_Type(cfg.unit)
	local pos = R1_Pos(cfg.site.x + dx, cfg.site.y + dy, cfg.site.z)
	local o, how = nil, "?"
	if ty == nil or human == nil or pos == nil then
		how = "no_type_player_or_pos"
	else
		local ok, r = pcall(Spawn_Unit, ty, pos, human)
		if ok and type(r) == "table" and r[1] ~= nil then
			o, how = r[1], "spawn_unit"
		elseif ok and r ~= nil and type(r) ~= "table" then
			o, how = r, "spawn_unit_obj"
		else
			if not ok then
				R1_ApiFail("Spawn_Unit", r)
			end
			local ok2, g = pcall(Create_Generic_Object, ty, pos, human)
			if ok2 and g ~= nil then
				o, how = g, "generic"
			else
				how = "failed"
				if not ok2 then
					R1_ApiFail("Create_Generic_Object", g)
				end
			end
		end
	end
	R1.ships[tag] = o
	R1_Rec("SPAWN", "ship=" .. tag .. "|type=" .. R1_Str(cfg.unit) .. "|how=" .. how .. "|valid=" .. tostring(R1_Valid(o)) ..
		"|id=" .. R1_Id(o) .. "|xy=" .. R1_XY(o) .. "|hull=" .. R1_Num(R1_Get(o, "Get_Hull")) .. "|shield=" ..
		R1_Num(R1_Get(o, "Get_Shield")))
end

-- Take_Damage on ship `tag`; arg "" = hull damage (one argument).
function R1_Damage(tag, amount, arg, label)
	local o = R1.ships[tag]
	local res = "dead"
	R1_Rec("CALL", "label=" .. label .. "|ship=" .. tag .. "|amount=" .. R1_Num(amount, "%.0f") .. "|arg=" .. R1_Str(arg) ..
		"|valid=" .. tostring(R1_Valid(o)) .. "|" .. R1_ShipState("A"))
	if R1_Valid(o) then
		local ok, err = pcall(function()
			if arg == nil or arg == "" then
				o.Take_Damage(amount)
			else
				o.Take_Damage(amount, arg)
			end
		end)
		if ok then
			res = "ok"
		else
			res = "err"
			R1_ApiFail("Take_Damage:" .. label, err)
		end
	end
	R1_Rec("KILL", "label=" .. label .. "|ship=" .. tag .. "|amount=" .. R1_Num(amount, "%.0f") .. "|arg=" .. R1_Str(arg) ..
		"|res=" .. res .. "|" .. R1_ShipState("A") .. "|" .. R1_ShipState("B") .. "|c=" .. R1_Counts())
	return res
end

-- Optional (R1_CFG.lua_camera): centre the camera on ship A. Point_Camera_At from a free-store script did not move
-- the camera in the s78 run, so this is a trial; every call is pcall'd and recorded (CAM). The driver frames by hand.
function R1_Camera(s)
	if not R1_CFG.lua_camera then
		return
	end
	local o = R1.ships.A
	if not R1_Valid(o) then
		return
	end
	local ok, err = pcall(function() Point_Camera_At(o.Get_Position()) end)
	R1_Rec("CAM", "stage=" .. s.key .. "|view=" .. R1_Str(s.view) .. "|res=" .. tostring(ok) .. "|err=" ..
		string.sub(R1_Str(err), 1, 100))
end

-- Optional (R1_CFG.turn_dy): order ship A to move to (x, y + turn_dy) so that it faces away from a camera in the south
-- (stern view without rotating the camera). Trial: pcall'd, recorded (TURN); the polls' xy show whether it moved.
function R1_Turn(F)
	if R1_CFG.turn_dy == nil then
		return
	end
	local o = R1.ships.A
	if not R1_Valid(o) then
		return
	end
	local pos = R1_Pos(R1_CFG.site.x + (R1_CFG.turn_dx or 0), R1_CFG.site.y + R1_CFG.turn_dy, R1_CFG.site.z)
	local ok, err = false, "nopos"
	if pos then
		ok, err = pcall(function() o.Move_To(pos) end)
	end
	R1_Rec("TURN", "res=" .. tostring(ok) .. "|dy=" .. R1_Num(R1_CFG.turn_dy, "%.0f") .. "|err=" ..
		string.sub(R1_Str(err), 1, 100) .. "|" .. R1_ShipState("A"))
end

function R1_StageTimes(i)
	local kill = R1_CFG.first_kill + (i - 1) * R1_CFG.period
	return kill - R1_CFG.pre, kill
end

function R1_Tick(F)
	local cfg = R1_CFG
	if R1.phase == "start" then
		R1.phase = "wait_spawn"
		R1_Say("TEXT_R1P_ARMED")
		R1_Rec("PLAN", "site=" .. R1_Num(cfg.site.x, "%.0f") .. "," .. R1_Num(cfg.site.y, "%.0f") .. "|b_dy=" ..
			R1_Num(cfg.b_dy, "%.0f") .. "|stages=" .. table.getn(cfg.stages) .. "|first_kill=" .. cfg.first_kill ..
			"|period=" .. cfg.period .. "|profile=" .. R1_Str(cfg.profile) .. "|lua_camera=" .. tostring(cfg.lua_camera) ..
			"|turn=" .. tostring(cfg.turn_dy ~= nil))
	end
	if R1.phase == "wait_spawn" then
		if F >= cfg.spawn_frame then
			R1_Spawn("A", 0, 0)
			R1_Spawn("B", 0, cfg.b_dy)
			R1_Poll(F, "spawn")
			R1_Say("TEXT_R1P_SPAWNED")
			R1.phase = "stages"
			R1.turn_at = F + 30
			R1.stage = 1
			R1.said_pre = false
			R1.killed = false
		end
		return
	elseif R1.phase == "stages" then
		if R1.turn_at and F >= R1.turn_at then
			R1.turn_at = nil
			R1_Turn(F)
		end
		local s = cfg.stages[R1.stage]
		if s == nil then
			R1.phase = "tail"
			R1.tail_from = F
		else
			local t_pre, t_kill = R1_StageTimes(R1.stage)
			if not R1.said_pre and F >= t_pre then
				R1.said_pre = true
				R1_Say("TEXT_R1P_" .. s.key .. "_PRE")
				R1_Rec("STAGE", "st=" .. R1.stage .. "|key=" .. s.key .. "|ship=" .. s.ship .. "|kind=" .. s.kind ..
					"|hp=" .. tostring(s.hp) .. "|arg=" .. R1_Str(s.arg) .. "|alt=" .. R1_Str(s.alt) .. "|amount=" ..
					R1_Num(s.amount, "%.0f") .. "|t_kill=" .. t_kill .. "|mode=" .. R1.name_mode)
				R1_Poll(F, "pre")
				R1_Camera(s)
			end
			if not R1.killed and F >= t_kill then
				R1.killed = true
				R1_Poll(F, "before")
				local arg = s.arg
				if s.ship == "A" and R1.name_mode == "coll" and s.alt ~= nil and s.alt ~= "" then
					arg = s.alt
				end
				local amount = s.amount
				if s.shield_scaled then
					local sh = R1_Get(R1.ships[s.ship], "Get_Shield")
					if type(sh) == "number" and sh > 0 then
						if sh <= 1.0 then
							sh = sh * cfg.shield_points
						end
						amount = amount + 1.1 * sh
					end
				end
				if R1.stage == 1 then
					R1.fl = { hull = R1_Get(R1.ships.A, "Get_Hull"), c = R1_CountN(cfg.stages[1].breakoff or ""), f = F }
				end
				-- The CALL record is written before the call: a CALL without a KILL after it shows a script stopped in the call.
				R1_Damage(s.ship, amount, arg, s.key)
				R1_Say("TEXT_R1P_" .. s.key .. "_DONE")
				R1.fast_until = F + cfg.fast
			end
			if R1.killed and R1.stage == 1 and R1.fl and not R1.fl.decided and F >= R1.fl.f + cfg.decide then
				R1.fl.decided = true
				local c = R1_CountN(cfg.stages[1].breakoff or "")
				local h = R1_Get(R1.ships.A, "Get_Hull")
				local by_prop = R1.fl.c ~= nil and c ~= nil and c > R1.fl.c
				local drop = nil
				if R1.fl.hull and h then
					drop = R1.fl.hull - h
				end
				local by_hull = drop ~= nil and drop >= 0.05 and drop <= 0.2
				if by_prop or by_hull then
					R1.name_mode = "type"
				else
					R1.name_mode = "coll"
				end
				R1_Rec("MODE", "mode=" .. R1.name_mode .. "|prop=" .. tostring(R1.fl.c) .. ">" .. tostring(c) .. "|drop=" ..
					R1_Num(drop) .. "|valid=" .. tostring(R1_Valid(R1.ships.A)))
			end
			local wait_end = cfg.period - cfg.pre
			if s.final then
				wait_end = 450   -- the last step: 15 s of polls after the call, then the tail
			end
			if R1.killed and F >= t_kill + wait_end then
				R1_Poll(F, "end")
				R1_Rec("SEND", "st=" .. R1.stage .. "|key=" .. s.key)
				R1.stage = R1.stage + 1
				R1.said_pre = false
				R1.killed = false
			end
		end
	elseif R1.phase == "tail" then
		if F - R1.tail_from >= cfg.tail then
			R1_Poll(F, "final")
			R1_Say("TEXT_R1P_DONE")
			R1_Finish("stages=" .. (R1.stage - 1))
			return
		end
	end
	if F <= R1.fast_until or F - R1.last_poll >= cfg.slow then
		local why = "slow"
		if F <= R1.fast_until then
			why = "fast"
		end
		R1_Poll(F, why)
	end
end

-- ---- arming and the service drivers ------------------------------------------------------------------------------

function R1_Arm()
	local fac, mode, me_human = "?", "?", true
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() me_human = PlayerObject.Is_Human() end)
	if (fac == "?" or fac == "NIL") and R1.tries < 200 then
		R1.pending = true
		return
	end
	R1.pending = false
	R1.fac = R1_Str(fac)
	R1.mode = R1_Str(mode)
	if mode ~= "Space" or fac ~= R1_CFG.ai_faction or me_human == true or R1_ARMED_ONCE then
		R1_Rec("SKIP", "fac=" .. R1.fac .. "|mode=" .. R1.mode .. "|human=" .. tostring(me_human))
		return
	end
	local human_ok = false
	pcall(function() human_ok = Find_Player(R1_CFG.human_faction).Is_Human() == true end)
	if not human_ok and R1.tries < 60 then
		R1.pending = true
		return
	end
	if not human_ok then
		R1_Rec("SKIP", "fac=" .. R1.fac .. "|mode=" .. R1.mode .. "|why=no_human_" .. R1_Str(R1_CFG.human_faction))
		return
	end
	R1_ARMED_ONCE = true
	R1.on = true
	R1.old_rate = ServiceRate
	ServiceRate = 0.05
	R1_Rec("ARM", "fac=" .. R1.fac .. "|mode=" .. R1.mode .. "|rate=" .. R1_Str(R1.old_rate) .. "|tries=" .. R1.tries)
end

function R1_Finish(extra)
	if R1.done then
		return
	end
	R1.done = true
	if R1.old_rate ~= nil then
		ServiceRate = R1.old_rate
	end
	local bad = ""
	for k, _ in pairs(R1_API_BAD) do
		bad = bad .. k .. ","
	end
	R1_Rec("DONE", "records=" .. R1_N .. "|apierr=" .. string.sub(R1_Str(bad), 1, 220) .. "|" .. (extra or ""))
end

function R1_Service(src)
	if R1.pending then
		R1.tries = R1.tries + 1
		pcall(R1_Arm)
	end
	if not R1.on or R1.done then
		return
	end
	local F = R1_Frame()
	if F == nil or F == R1.f then
		return
	end
	R1.f = F
	if R1.hb == nil or F - R1.hb >= 300 then
		R1.hb = F
		R1_Rec("HB", "src=" .. src .. "|phase=" .. R1.phase .. "|st=" .. R1.stage)
	end
	local ok, err = pcall(R1_Tick, F)
	if not ok then
		R1_Rec("ERR", "where=tick|src=" .. src .. "|err=" .. string.sub(R1_Str(err), 1, 200))
	end
end

function R1_Quiet()
	return R1.on and not R1.done
end

R1_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = nil
	if R1_Orig_Base_Definitions then
		r = R1_Orig_Base_Definitions()
	end
	pcall(R1_Arm)
	return r
end

R1_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	pcall(R1_Service, "fss")
	if R1_Quiet() or R1_Orig_FreeStoreService == nil then
		return
	end
	return R1_Orig_FreeStoreService()
end

R1_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	pcall(R1_Service, "ous")
	if R1_Quiet() or R1_Orig_On_Unit_Service == nil then
		return
	end
	return R1_Orig_On_Unit_Service(object)
end

R1_Rec("LOAD", "session=" .. R1_Str(R1_CFG.session) .. "|ai_faction=" .. R1_Str(R1_CFG.ai_faction))
-- ==== end of r1_hook.lua ====
