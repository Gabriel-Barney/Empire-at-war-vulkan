-- ==== S7/S8 probe (tools/probe_mod/s7s8_probes/s7s8_hook.lua), appended to the shipped BusyTacticalFreeStore.lua by
-- build_s7s8_mod.py after a generated S78_CFG table (test sites of the land and space maps) ====
-- S78PROBE_SRC_MARK_V1
-- Questions (docs/re): LOB_SOLVER sec. 9 / sec. 8 item 22 (SPMAT arc and range edge, Remote_Bomb ground marker),
-- TACTICAL_COMBAT sec. 3.1.1 "Gate details" (deploy window, first shot after spawn, muzzle bones),
-- AI_TACTICAL sec. 7.3.3 / sec. 12 (Activate_Ability blocks: DEPLOY, object target that dies),
-- FIGHTER_FLYING sec. 4.10 B / E (squadron team target, escorts of a dead carrier), MOVEMENT sec. 3.2 (deploy states),
-- ABILITIES_HARDPOINTS (Dark_Trooper_PhaseIII jet pack and its deployed-only missiles).
--
-- Only the free store of the AI player whose faction is S78_CFG.faction runs the scenarios, in land and in space.
-- That instance lowers ServiceRate to 0.05 s so the engine resumes the script on every free-store service (every 3rd
-- frame) and, while the probe runs, skips the shipped FreeStoreService / On_Unit_Service bodies (the AI's own units
-- stay idle). The scenarios are a frame-timed state machine driven from FreeStoreService (On_Unit_Service as a
-- fallback driver): no thread, no yield inside pcall (Lua 5.0), every engine call pcall'd.
-- Land also starts a "human" track at once: an Underworld_Saboteur for the human player next to the human's units,
-- for the Remote_Bomb ground-marker steps the player does by hand (README steps H1-H3).
--
-- Output channel (as the A1 / OI probes): retail _CustomScriptMessage writes nothing, so every record is a Lua string
-- "S78P1|<KIND>|r=<run>|f=<mode frame>|...|END" kept in S78_LOG; s7s8_reader.py finds them by a read-only heap scan.
-- f = GetCurrentTime() * 30 rounded (mode frame). Game_Message lines (TEXT_S78_*) only when S78_CFG.text is true.
-- Kinds: LOAD, ARM / SKIP, TICK, HB, VICTIMS, HUMAN, SCN (start / end), SPAWN, SETUP, ACT (ability call, with the
-- returned block), BLK (block finished), ORDER, MARK, COUNT, P (3-frame poll, on change), APIERR, ERR, DONE.
-- Lua 5.0: no '#', table.getn. The retail free-store Lua has NO math library (s78g 2026-11-05: "attempt to index
-- global `math`"): arithmetic only; the selftest stub removes `math` to catch any use.

S78_LOG = {}
S78_SID = "?"
S78_RUN = 0
S78_ON = false
S78_DONE = false
S78_MODE = "?"
S78_LAST_F = nil
S78_TICKS = 0
S78_HB_NEXT = 100
S78_SCN_I = 0
S78_CUR = nil
S78_ANCHOR = nil
S78_API_BAD = {}
S78_MAX_RECORDS = 30000
S78_VICTIM_PLAYER = nil
S78_VICTIM_FAC = "?"
S78_HUMAN = nil

function S78_Clean(v)
	local s = tostring(v)
	s = string.gsub(s, "[^%w%._%-:]", "_")
	return s
end

function S78_Frame()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return tonumber(string.format("%.0f", t * 30))
	end
	return nil
end

function S78_Rec(kind, fields)
	pcall(function()
		if table.getn(S78_LOG) >= S78_MAX_RECORDS and kind ~= "DONE" then
			return
		end
		local f = S78_Frame()
		local fs = "?"
		if f then
			fs = tostring(f)
		end
		local body = fields or ""
		if string.len(body) > 330 then
			body = string.sub(body, 1, 330) .. "~"
		end
		table.insert(S78_LOG, "S78P1|" .. kind .. "|r=" .. S78_SID .. "." .. S78_RUN .. "|f=" .. fs .. "|" .. body .. "|END")
	end)
end

function S78_Msg(id)
	if S78_CFG.text then
		pcall(Game_Message, id)
	end
end

function S78_ScnName()
	if S78_CUR then
		return S78_CUR.name
	end
	return "-"
end

function S78_ApiFail(tag, err)
	local key = S78_ScnName() .. ":" .. tag
	if not S78_API_BAD[key] then
		S78_API_BAD[key] = true
		S78_Rec("APIERR", "scn=" .. S78_ScnName() .. "|api=" .. S78_Clean(tag) .. "|err=" ..
			string.sub(S78_Clean(err), 1, 150))
	end
end

function S78_Try(tag, f)
	local ok, a, b, c = pcall(f)
	if not ok then
		S78_ApiFail(tag, a)
		return nil
	end
	return a, b, c
end

function S78_Num(v, fmt)
	if type(v) == "number" then
		return string.format(fmt or "%.0f", v)
	end
	return "?"
end

function S78_Valid(obj)
	if obj == nil then
		return false
	end
	local ok, v = pcall(function() return obj.Is_Valid() end)
	return ok and v == true
end

function S78_XYZ(obj)
	local ok, x, y, z = pcall(function()
		local p = obj
		if obj.Get_Position then
			p = obj.Get_Position()
		end
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

function S78_ObjId(obj)
	local ok, v = pcall(function() return obj.Get_Object_ID() end)
	if ok and v ~= nil then
		return tostring(v)
	end
	return "?"
end

function S78_OwnerId(obj)
	local ok, v = pcall(function() return obj.Get_Owner().Get_ID() end)
	if ok then
		return tostring(v)
	end
	return "?"
end

function S78_Count(list)
	if type(list) == "table" then
		return table.getn(list)
	end
	return 0
end

-- ---- sites ---------------------------------------------------------------------------------------------------------

function S78_Pts()
	if S78_MODE == "Space" then
		return S78_CFG.space
	end
	return S78_CFG.land
end

-- Site point + along * u + side * v (v = u turned +90 degrees); the fallback is the anchor (an AI unit).
function S78_At(site, along, side)
	local m = S78_Pts()
	local p = m.pts[site]
	local bx, by, bz, ux, uy
	if m.points_ok and p then
		bx, by, bz, ux, uy = p.x, p.y, p.z, p.ux or 1.0, p.uy or 0.0
	else
		bx, by, bz = S78_XYZ(S78_ANCHOR)
		if type(bx) ~= "number" then
			return nil, "none"
		end
		bx = bx + 600
		ux, uy = 1.0, 0.0
	end
	local x = bx + (along or 0) * ux - (side or 0) * uy
	local y = by + (along or 0) * uy + (side or 0) * ux
	local pos = S78_Try("Create_Position", function() return Create_Position(x, y, bz or 0) end)
	if pos then
		if m.points_ok and p then
			return pos, "cfg"
		end
		return pos, "anchor"
	end
	return nil, "none"
end

-- ---- spawning and set-up -------------------------------------------------------------------------------------------

function S78_Spawn(tag, tname, where, how, player)
	local s = S78_CUR
	local t = S78_Try("Find_Object_Type:" .. tname, function() return Find_Object_Type(tname) end)
	if t == nil or where == nil then
		S78_Rec("SPAWN", "scn=" .. s.name .. "|tag=" .. tag .. "|type=" .. tname .. "|ok=0|why=" ..
			((t == nil) and "no_type" or "no_position"))
		return nil
	end
	local r = S78_Try("Spawn_Unit:" .. tname, function() return Spawn_Unit(t, where, player) end)
	local obj = r
	if type(r) == "table" and r[1] ~= nil then
		obj = r[1]
	end
	if not S78_Valid(obj) then
		S78_Rec("SPAWN", "scn=" .. s.name .. "|tag=" .. tag .. "|type=" .. tname .. "|ok=0|why=spawn_failed")
		return nil
	end
	s.objs[tag] = obj
	table.insert(s.order, tag)
	table.insert(s.spawned, obj)
	local x, y, z = S78_XYZ(obj)
	S78_Rec("SPAWN", "scn=" .. s.name .. "|tag=" .. tag .. "|type=" .. tname .. "|ok=1|how=" .. how ..
		"|id=" .. S78_ObjId(obj) .. "|n=" .. S78_Count(r) .. "|owner=" .. S78_OwnerId(obj) .. "|x=" .. S78_Num(x, "%.1f") ..
		"|y=" .. S78_Num(y, "%.1f") .. "|z=" .. S78_Num(z, "%.1f"))
	return obj
end

function S78_Setup(tag, obj, calls)
	local s = ""
	for i, c in ipairs(calls) do
		local name, a1, a2 = c[1], c[2], c[3]
		local ok = pcall(function()
			if a2 ~= nil then
				obj[name](a1, a2)
			else
				obj[name](a1)
			end
		end)
		if not ok then
			S78_ApiFail(name, "call failed")
		end
		s = s .. name .. "=" .. (ok and "1" or "0") .. ","
	end
	S78_Rec("SETUP", "scn=" .. S78_ScnName() .. "|tag=" .. tag .. "|" .. s)
end

-- Probe attackers: invulnerable, kept from AI plans.
function S78_Attacker(tag, obj, mortal)
	if obj then
		local calls = { { "Prevent_AI_Usage", true } }
		if not mortal then
			table.insert(calls, { "Make_Invulnerable", true })
		end
		S78_Setup(tag, obj, calls)
	end
end

-- Victims: never fire, kept from AI plans, held in place; invulnerable unless they must die.
function S78_Victim(tag, obj, mortal)
	if obj then
		local calls = { { "Prevent_All_Fire", true }, { "Prevent_AI_Usage", true }, { "Suspend_Locomotor", true } }
		if not mortal then
			table.insert(calls, { "Make_Invulnerable", true })
		end
		S78_Setup(tag, obj, calls)
	end
end

function S78_Order(tag, what, f)
	local ok, err = pcall(f)
	if not ok then
		S78_ApiFail("order:" .. what, err)
	end
	S78_Rec("ORDER", "scn=" .. S78_ScnName() .. "|tag=" .. tag .. "|order=" .. what .. "|ok=" .. (ok and "1" or "0"))
	return ok
end

function S78_Mark(name, extra)
	local s = S78_CUR
	s.marks[name] = S78_Frame()
	S78_Rec("MARK", "scn=" .. s.name .. "|mark=" .. name .. "|" .. (extra or ""))
end

function S78_Flag(obj, method, ability)
	local ok, v = pcall(function() return obj[method](ability) end)
	if ok then
		if v then
			return "1"
		end
		return "0"
	end
	return "?"
end

-- Activate_Ability(ability [, arg]); records what it returned (a block status, or nil) and, for a block, polls
-- IsFinished() on every tick (S78_PollBlocks) until it is true. argkind: none / bool / obj / pos.
function S78_Act(tag, blk, ability, argkind, arg)
	local s = S78_CUR
	local obj = s.objs[tag]
	if not S78_Valid(obj) then
		S78_Rec("ACT", "scn=" .. s.name .. "|tag=" .. tag .. "|blk=" .. blk .. "|ab=" .. ability .. "|arg=" .. argkind ..
			"|call=no_unit")
		return nil
	end
	local rb = S78_Flag(obj, "Is_Ability_Ready", ability)
	S78_Rec("ACTC", "scn=" .. s.name .. "|tag=" .. tag .. "|blk=" .. blk .. "|ab=" .. ability .. "|arg=" .. argkind)
	local ok, ret = pcall(function()
		if argkind == "none" then
			return obj.Activate_Ability(ability)
		end
		return obj.Activate_Ability(ability, arg)
	end)
	if not ok then
		S78_ApiFail("Activate_Ability:" .. ability .. ":" .. argkind, ret)
	end
	local rk = "nil"
	if ok and ret ~= nil then
		rk = type(ret)
		local okf, fin = pcall(function() return ret.IsFinished() end)
		if okf then
			rk = "block"
			s.blocks[blk] = { b = ret, f0 = S78_Frame(), done = false }
			if fin == true then
				s.blocks[blk].done = true
				S78_Rec("BLK", "scn=" .. s.name .. "|blk=" .. blk .. "|ev=finished_at_once|res=" ..
					S78_Clean(S78_Try("Result", function() return ret.Result() end)))
			end
		end
	end
	S78_Rec("ACT", "scn=" .. s.name .. "|tag=" .. tag .. "|blk=" .. blk .. "|ab=" .. ability .. "|arg=" .. argkind ..
		"|call=" .. (ok and "ok" or "error") .. "|ret=" .. rk .. "|ready_before=" .. rb .. "|ready_after=" ..
		S78_Flag(obj, "Is_Ability_Ready", ability) .. "|active_after=" .. S78_Flag(obj, "Is_Ability_Active", ability))
	return ok and ret
end

function S78_PollBlocks()
	local s = S78_CUR
	for name, b in pairs(s.blocks) do
		if not b.done then
			local ok, fin = pcall(function() return b.b.IsFinished() end)
			if not ok then
				b.done = true
				S78_Rec("BLK", "scn=" .. s.name .. "|blk=" .. name .. "|ev=error")
			elseif fin == true then
				b.done = true
				local okr, res = pcall(function() return b.b.Result() end)
				S78_Rec("BLK", "scn=" .. s.name .. "|blk=" .. name .. "|ev=finished|res=" ..
					S78_Clean(okr and res or "?") .. "|after=" .. S78_Num((S78_Frame() or 0) - (b.f0 or 0)))
			end
		end
	end
end

function S78_Kill(tag)
	local s = S78_CUR
	local obj = s.objs[tag]
	if not S78_Valid(obj) then
		S78_Rec("ORDER", "scn=" .. s.name .. "|tag=" .. tag .. "|order=kill|ok=0|why=invalid")
		return
	end
	pcall(function() obj.Make_Invulnerable(false) end)
	pcall(function() obj.Set_Cannot_Be_Killed(false) end)
	local n = 0
	while n < 6 and S78_Valid(obj) do
		n = n + 1
		pcall(function() obj.Take_Damage(1000000) end)
	end
	S78_Rec("ORDER", "scn=" .. s.name .. "|tag=" .. tag .. "|order=kill|ok=1|calls=" .. n .. "|valid_after=" ..
		(S78_Valid(obj) and "1" or "0"))
end

function S78_Despawn(tag)
	local s = S78_CUR
	local obj = s.objs[tag]
	local ok = S78_Valid(obj) and pcall(function() obj.Despawn() end)
	S78_Rec("ORDER", "scn=" .. s.name .. "|tag=" .. tag .. "|order=despawn|ok=" .. (ok and "1" or "0"))
end

-- Objects of a type owned by a player (Lua view; the reader has the exact frames). Owners are compared by id:
-- two wrappers of one player need not be equal.
function S78_OwnedOfType(tname, player)
	local out = {}
	local pid = nil
	if player ~= nil then
		local okp, v = pcall(function() return player.Get_ID() end)
		pid = okp and tostring(v) or "?"
	end
	local ok, l = pcall(function() return Find_All_Objects_Of_Type(tname) end)
	if ok and type(l) == "table" then
		for i, o in ipairs(l) do
			if S78_Valid(o) and (pid == nil or S78_OwnerId(o) == pid) then
				table.insert(out, o)
			end
		end
	end
	return out
end

function S78_Centroid(list)
	local n, sx, sy = 0, 0, 0
	for i, o in ipairs(list) do
		local x, y = S78_XYZ(o)
		if type(x) == "number" and type(y) == "number" then
			n = n + 1
			sx = sx + x
			sy = sy + y
		end
	end
	if n == 0 then
		return nil
	end
	return sx / n, sy / n
end

-- ---- per-tick poll -------------------------------------------------------------------------------------------------

function S78_Poll()
	local s = S78_CUR
	if s == nil then
		return
	end
	S78_PollBlocks()
	local parts = ""
	for i, tag in ipairs(s.order) do
		local obj = s.objs[tag]
		local e = tag .. "="
		if S78_Valid(obj) then
			local okh, h = pcall(function() return obj.Get_Hull() end)
			local x, y = S78_XYZ(obj)
			e = e .. "1:" .. ((okh and type(h) == "number") and string.format("%.2f", h) or "?") .. ":" ..
				S78_Num(x) .. ":" .. S78_Num(y)
			local ab = s.abilities and s.abilities[tag]
			if ab then
				e = e .. ":" .. S78_Flag(obj, "Is_Ability_Active", ab) .. S78_Flag(obj, "Is_Ability_Ready", ab)
			end
		else
			e = e .. "0"
		end
		parts = parts .. e .. ";"
	end
	if parts ~= s.last_poll then
		s.last_poll = parts
		S78_Rec("P", "scn=" .. s.name .. "|" .. parts)
	end
end

-- ---- land scenarios ------------------------------------------------------------------------------------------------
-- Each scenario: name, dur (frames), steps = { { at = rel frame, when = fn(s) | nil, run = fn(s) } }, each run once,
-- in readiness order. Every scenario spawns at the land site (or the space sites) and despawns its objects at the end.

-- Items 1, 3 (SPMAT), 5 (DEPLOY block): deploy, arc at 275, range edge at 547 (no shot) and 530 (shots), undeploy,
-- then deploy -> undeploy after 12 frames (reversal of Deploying) -> deploy after 12 more (reversal of Undeploying).
function S78_ScnSpmat()
	return {
		name = "SPMAT",
		dur = 2400,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 0, 0)
				S78_Attacker("spmat", S78_Spawn("spmat", "SPMAT_Walker", w, how, PlayerObject))
			end },
			{ at = 6, run = function(s)
				local w = S78_At("site", 300, 0)
				if not S78_Order("spmat", "face_immediate", function() s.objs.spmat.Face_Immediate(w) end) then
					S78_Order("spmat", "turn_to_face", function() s.objs.spmat.Turn_To_Face(w) end)
				end
			end },
			{ at = 30, run = function(s)
				local w, how = S78_At("site", 275, 0)
				S78_Victim("v275", S78_Spawn("v275", "T4B_Tank", w, how, S78_VICTIM_PLAYER), false)
				if s.objs.v275 then
					S78_Order("spmat", "attack_v275", function() s.objs.spmat.Attack_Target(s.objs.v275) end)
				end
			end },
			{ at = 45, run = function(s) S78_Act("spmat", "deploy1", "DEPLOY", "bool", true) end },
			{ at = 585, run = function(s)
				S78_Despawn("v275")
				local w, how = S78_At("site", 547, 0)
				S78_Victim("v547", S78_Spawn("v547", "T4B_Tank", w, how, S78_VICTIM_PLAYER), true)
				S78_Mark("edge")
			end },
			{ at = 1185, run = function(s)
				local w, how = S78_At("site", 530, 0)
				S78_Victim("v530", S78_Spawn("v530", "T4B_Tank", w, how, S78_VICTIM_PLAYER), true)
				S78_Mark("control")
			end },
			{ at = 1635, run = function(s) S78_Act("spmat", "undeploy1", "DEPLOY", "bool", false) end },
			{ at = 1875, run = function(s) S78_Act("spmat", "deploy2", "DEPLOY", "bool", true) end },
			{ at = 1887, run = function(s) S78_Act("spmat", "undeploy2", "DEPLOY", "bool", false) end },
			{ at = 1899, run = function(s) S78_Act("spmat", "deploy3", "DEPLOY", "bool", true) end },
		},
	}
end

-- Item 3 (AT-AT): firing at a held victim, then DEPLOY (fallback DEPLOY_TROOPERS), undeploy, and the reversal pair.
function S78_AtatDeploy(blk, on)
	return function(s)
		local r = S78_Act("atat", blk, "DEPLOY", "bool", on)
		if not r and on then
			S78_Act("atat", blk .. "_troopers", "DEPLOY_TROOPERS", "none", nil)
		end
	end
end

function S78_ScnAtat()
	return {
		name = "ATAT",
		dur = 1500,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 0, 0)
				S78_Attacker("atat", S78_Spawn("atat", "AT_AT_Walker", w, how, PlayerObject))
				local w2, how2 = S78_At("site", 250, 0)
				S78_Victim("victim", S78_Spawn("victim", "T4B_Tank", w2, how2, S78_VICTIM_PLAYER), false)
			end },
			{ at = 30, run = function(s)
				S78_Order("atat", "attack_victim", function() s.objs.atat.Attack_Target(s.objs.victim) end)
			end },
			{ at = 330, run = S78_AtatDeploy("deploy1", true) },
			{ at = 780, run = S78_AtatDeploy("undeploy1", false) },
			{ at = 1080, run = S78_AtatDeploy("deploy2", true) },
			{ at = 1092, run = S78_AtatDeploy("undeploy2", false) },
			{ at = 1104, run = S78_AtatDeploy("deploy3", true) },
		},
	}
end

-- Item 5 (DEPLOY on an MDU): block of a deploy, a second DEPLOY true when deployed (nil expected), undeploy, then
-- DEPLOY while moving (the [M] "moving" gate vf +0x2b0) and a new move order 300 frames later (signal 0xb).
function S78_ScnMdu()
	return {
		name = "MDU",
		dur = 1200,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 0, 0)
				S78_Attacker("mdu", S78_Spawn("mdu", "Rebel_Mobile_Defense_Unit", w, how, PlayerObject))
			end },
			{ at = 30, run = function(s) S78_Act("mdu", "deploy", "DEPLOY", "bool", true) end },
			{ at = 300, run = function(s) S78_Act("mdu", "deploy_again", "DEPLOY", "bool", true) end },
			{ at = 330, run = function(s) S78_Act("mdu", "undeploy", "DEPLOY", "bool", false) end },
			{ at = 600, run = function(s)
				local w = S78_At("site", 450, 0)
				S78_Order("mdu", "move_far", function() s.objs.mdu.Move_To(w) end)
			end },
			{ at = 630, run = function(s) S78_Act("mdu", "deploy_moving", "DEPLOY", "bool", true) end },
			{ at = 930, run = function(s)
				local w = S78_At("site", 150, 0)
				S78_Order("mdu", "move_back", function() s.objs.mdu.Move_To(w) end)
			end },
		},
	}
end

-- Item 5 (object-target ability whose target dies): control (live target 120 away) and a target 450 away killed
-- 60 frames after the order, then a move order at +720 (signal 0xb) if the block is still pending.
function S78_ScnSticky()
	return {
		name = "STICKY",
		dur = 1500,
		abilities = { inf1 = "STICKY_BOMB", inf2 = "STICKY_BOMB" },
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 0, -60)
				local i1 = S78_Spawn("inf1", "Rebel_Infiltrator", w, how, PlayerObject)
				S78_Attacker("inf1", i1)
				local w2, how2 = S78_At("site", 0, 60)
				local i2 = S78_Spawn("inf2", "Rebel_Infiltrator", w2, how2, PlayerObject)
				S78_Attacker("inf2", i2)
				for i, t in ipairs({ "inf1", "inf2" }) do
					if s.objs[t] then
						S78_Setup(t, s.objs[t], { { "Set_Single_Ability_Autofire", "STICKY_BOMB", false } })
					end
				end
				local w3, how3 = S78_At("site", 120, -60)
				S78_Victim("live", S78_Spawn("live", "T4B_Tank", w3, how3, S78_VICTIM_PLAYER), true)
			end },
			{ at = 30, run = function(s) S78_Act("inf1", "sb_live", "STICKY_BOMB", "obj", s.objs.live) end },
			{ at = 450, run = function(s)
				local w, how = S78_At("site", 450, 60)
				S78_Victim("doomed", S78_Spawn("doomed", "T4B_Tank", w, how, S78_VICTIM_PLAYER), true)
			end },
			{ at = 480, run = function(s) S78_Act("inf2", "sb_dead", "STICKY_BOMB", "obj", s.objs.doomed) end },
			{ at = 540, run = function(s)
				S78_Kill("doomed")
				S78_Mark("target_killed")
			end },
			{ at = 1200, run = function(s)
				local b = s.blocks.sb_dead
				S78_Rec("MARK", "scn=" .. s.name .. "|mark=before_move|sb_dead=" ..
					((b == nil) and "none" or (b.done and "finished" or "pending")))
				local w = S78_At("site", -100, 60)
				S78_Order("inf2", "move_away", function() s.objs.inf2.Move_To(w) end)
			end },
		},
	}
end

-- Item 8 (Dark_Trooper_PhaseIII): lasers undeployed, missiles deployed, jet pack, missiles after landing (expected
-- silent), redeploy (missiles again).
function S78_ScnDt3()
	return {
		name = "DT3",
		dur = 2100,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 0, 0)
				S78_Attacker("dt3", S78_Spawn("dt3", "Dark_Trooper_PhaseIII", w, how, PlayerObject))
				local w2, how2 = S78_At("site", 200, 0)
				S78_Victim("victim", S78_Spawn("victim", "T4B_Tank", w2, how2, S78_VICTIM_PLAYER), false)
			end },
			{ at = 30, run = function(s)
				S78_Order("dt3", "attack_victim", function() s.objs.dt3.Attack_Target(s.objs.victim) end)
			end },
			{ at = 300, run = function(s) S78_Act("dt3", "deploy1", "DEPLOY", "bool", true) end },
			{ at = 750, run = function(s)
				local w = S78_At("site", -120, 60)
				if not S78_Act("dt3", "jet", "JET_PACK", "pos", w) then
					S78_Mark("jet_nil")
				end
			end },
			{ at = 1200, run = function(s)
				S78_Order("dt3", "attack_victim2", function() s.objs.dt3.Attack_Target(s.objs.victim) end)
			end },
			{ at = 1650, run = function(s) S78_Act("dt3", "deploy2", "DEPLOY", "bool", true) end },
		},
	}
end

-- Item 8 (first shot after spawn): four MZ8 pulse tanks (recharge 5.3 s: a surviving random first-shot delay would
-- spread up to 159 frames) spawned 150 frames apart, each ordered to attack a held victim at once.
function S78_ReinfStep(k)
	return { at = 30 + (k - 1) * 150, run = function(s)
		local tag = "mz8_" .. k
		local w, how = S78_At("site", 0, (k - 2.5) * 70)
		local u = S78_Spawn(tag, "MZ8_Pulse_Cannon_Tank", w, how, PlayerObject)
		S78_Attacker(tag, u)
		if u then
			S78_Order(tag, "attack_victim", function() u.Attack_Target(s.objs.victim) end)
		end
	end }
end

function S78_ScnReinf()
	return {
		name = "REINF",
		dur = 780,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 200, 0)
				S78_Victim("victim", S78_Spawn("victim", "T4B_Tank", w, how, S78_VICTIM_PLAYER), false)
			end },
			S78_ReinfStep(1), S78_ReinfStep(2), S78_ReinfStep(3), S78_ReinfStep(4),
		},
	}
end

-- Item 4 (muzzle bones): the 8 types in two groups of four along the site, each next to its own held victim.
S78_MUZZLE = { "Moldy_Crow", "Moldy_Crow_Landing", "Luke_Jedi_Xwing_Landing", "Underworld_Infantry_Droid_Shuttle",
	"Underworld_Infantry_Droid_Shuttle_Landing", "Tyber_Zann_Boarding_Shuttle_Prologue", "UM07_Cloud_City_Flyover",
	"UM07_CloudCity_Refinery" }

function S78_MuzzleGroup(g, at, site, step, vside)
	return { at = at, run = function(s)
		if g == 2 then
			for k = 1, 4 do
				S78_Despawn("mz" .. k)
				S78_Despawn("mv" .. k)
			end
		end
		for k = 1, 4 do
			local i = (g - 1) * 4 + k
			local tname = S78_MUZZLE[i]
			local tag = "mz" .. i
			local w, how = S78_At(site, (k - 1) * step, 0)
			local u = S78_Spawn(tag, tname, w, how, PlayerObject)
			S78_Attacker(tag, u)
			local w2, how2 = S78_At(site, (k - 1) * step, vside)
			local v = S78_Spawn("mv" .. i, S78_MuzzleVictim(), w2, how2, S78_VICTIM_PLAYER)
			S78_Victim("mv" .. i, v, false)
			if u and v then
				S78_Order(tag, "attack_victim", function() u.Attack_Target(v) end)
			end
		end
	end }
end

function S78_MuzzleVictim()
	if S78_MODE == "Space" then
		return "Tartan_Patrol_Cruiser"
	end
	return "T4B_Tank"
end

function S78_ScnMuzzleLand()
	return {
		name = "MUZZLE_LAND",
		dur = 660,
		steps = { S78_MuzzleGroup(1, 0, "site", 220, 150), S78_MuzzleGroup(2, 330, "site", 220, 150) },
	}
end

-- ---- space scenarios -----------------------------------------------------------------------------------------------

function S78_ScnMuzzleSpace()
	return {
		name = "MUZZLE_SPACE",
		dur = 660,
		steps = { S78_MuzzleGroup(1, 0, "muz", 600, 350), S78_MuzzleGroup(2, 330, "muz", 600, 350) },
	}
end

-- Item 6 (squadron team target): one X-Wing squadron, a short move (gives it a formation), three held targets, then
-- the target the squadron is on is despawned twice (each time every member's target must go).
function S78_SquadDrop(n)
	return { at = 600 + n * 600, run = function(s)
		local sq = s.objs.squad
		local cur = nil
		pcall(function() cur = sq.Get_Attack_Target() end)
		local which = nil
		for i, t in ipairs({ "t1", "t2", "t3" }) do
			if s.objs[t] and cur and s.objs[t] == cur then
				which = t
			end
		end
		if which == nil then
			for i, t in ipairs({ "t2", "t1", "t3" }) do
				if which == nil and S78_Valid(s.objs[t]) then
					which = t
				end
			end
		end
		S78_Rec("MARK", "scn=" .. s.name .. "|mark=drop" .. n .. "|target=" .. tostring(which) .. "|lua_target=" ..
			(cur and S78_ObjId(cur) or "none"))
		if which then
			S78_Despawn(which)
		end
	end }
end

function S78_ScnSquad()
	return {
		name = "SQUAD",
		dur = 2100,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("sq", 0, 0)
				S78_Attacker("squad", S78_Spawn("squad", "Rebel_X-Wing_Squadron", w, how, PlayerObject), true)
			end },
			{ at = 15, run = function(s)
				local w = S78_At("sq", 250, 0)
				S78_Order("squad", "move_short", function() s.objs.squad.Move_To(w) end)
			end },
			{ at = 300, run = function(s)
				local offs = { { 650, -250 }, { 700, 0 }, { 650, 250 } }
				for i = 1, 3 do
					local w, how = S78_At("sq", offs[i][1], offs[i][2])
					S78_Victim("t" .. i, S78_Spawn("t" .. i, "Tartan_Patrol_Cruiser", w, how, S78_VICTIM_PLAYER), false)
				end
			end },
			S78_SquadDrop(1), S78_SquadDrop(2),
		},
	}
end

-- Item 7 (escorts of a carrier that dies): a Star_Destroyer launches its squadrons, moves 900, is killed; 1200 frames
-- later a held target appears 700 from the escorts' centroid, and goes again 900 frames later.
S78_ESCORT_TYPES = { "TIE_Interceptor_Squadron", "TIE_Bomber_Squadron", "TIE_Fighter_Squadron", "TIE_Interceptor",
	"TIE_Bomber", "TIE_Fighter" }

-- first = 1: squadron containers only (types 1..3); first = 4: fighters only; nil: both.
function S78_Escorts(first, last)
	local out = {}
	for i, t in ipairs(S78_ESCORT_TYPES) do
		if i >= (first or 1) and i <= (last or 6) then
			for j, o in ipairs(S78_OwnedOfType(t, PlayerObject)) do
				table.insert(out, o)
			end
		end
	end
	return out
end

function S78_ScnEscort()
	return {
		name = "ESCORT",
		dur = 4500,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("esc", 0, 0)
				S78_Attacker("carrier", S78_Spawn("carrier", "Star_Destroyer", w, how, PlayerObject))
			end },
			{ at = 30, when = function(s)
				return S78_Count(S78_Escorts(1, 3)) >= 2 or S78_Count(S78_Escorts(4, 6)) >= 6 or
					(S78_Frame() - s.f0) >= 900
			end, run = function(s)
				S78_Rec("COUNT", "scn=" .. s.name .. "|what=escorts_launched|squadrons=" .. S78_Count(S78_Escorts(1, 3)) ..
					"|fighters=" .. S78_Count(S78_Escorts(4, 6)))
				local w = S78_At("esc", 900, 0)
				S78_Order("carrier", "move", function() s.objs.carrier.Move_To(w) end)
				S78_Mark("moved")
			end },
			{ at = 60, when = function(s) return s.marks.moved and S78_Frame() >= s.marks.moved + 600 end,
				run = function(s)
					local x, y = S78_XYZ(s.objs.carrier)
					s.dx, s.dy = x, y
					S78_Kill("carrier")
					if S78_Valid(s.objs.carrier) then
						S78_Despawn("carrier")
					end
					S78_Mark("carrier_dead", "x=" .. S78_Num(x) .. "|y=" .. S78_Num(y) .. "|escorts=" ..
						S78_Count(S78_Escorts()))
				end },
			-- s78g t05: the intruder goes 700 from the death point (the guarded position), not from the escorts'
			-- centroid (in the s78f t12 run the escorts had flown 4000 away, so the intruder tested nothing)
			{ at = 90, when = function(s) return s.marks.carrier_dead and S78_Frame() >= s.marks.carrier_dead + 1200 end,
				run = function(s)
					local cx, cy = S78_Centroid(S78_Escorts())
					local bx, by = s.dx, s.dy
					local how = "death_point"
					if type(bx) ~= "number" then
						bx, by, how = cx, cy, "centroid"
					end
					local pos = nil
					if type(bx) == "number" then
						pos = S78_Try("Create_Position", function()
							local p = S78_Pts().pts.esc
							local ux, uy = (p and p.ux) or 1.0, (p and p.uy) or 0.0
							return Create_Position(bx + 700 * ux, by + 700 * uy, 0)
						end)
					end
					S78_Rec("MARK", "scn=" .. s.name .. "|mark=guard_point|x=" .. S78_Num(cx) .. "|y=" .. S78_Num(cy) ..
						"|dx=" .. S78_Num(s.dx) .. "|dy=" .. S78_Num(s.dy))
					S78_Victim("intruder", S78_Spawn("intruder", "Tartan_Patrol_Cruiser", pos, pos and how or "none",
						S78_VICTIM_PLAYER), false)
					S78_Mark("intruder")
				end },
			{ at = 120, when = function(s) return s.marks.intruder and S78_Frame() >= s.marks.intruder + 900 end,
				run = function(s)
					S78_Despawn("intruder")
					S78_Mark("intruder_gone")
				end },
		},
		-- s78g t05: the launched escorts belong to the AI player; keep its plans off them (Prevent_AI_Usage on every
		-- new escort, checked every 15 frames) and log their centroid every 90 frames (Lua-side fallback for the
		-- parser). In the s78f t12 run they flew 4000 away toward the AI's own start after the carrier died.
		tick = function(s, rel)
			if rel < (s.esc_next or 0) then
				return
			end
			s.esc_next = rel + 15
			s.esc_seen = s.esc_seen or {}
			local list = S78_Escorts()
			local newn = 0
			for i, o in ipairs(list) do
				local id = S78_ObjId(o)
				if not s.esc_seen[id] then
					s.esc_seen[id] = true
					newn = newn + 1
					pcall(function() o.Prevent_AI_Usage(true) end)
				end
			end
			if newn > 0 then
				S78_Rec("COUNT", "scn=" .. s.name .. "|what=escorts_held|new=" .. newn .. "|total=" .. S78_Count(list))
			end
			if rel >= (s.esc_cnext or 0) then
				s.esc_cnext = rel + 90
				local cx, cy = S78_Centroid(S78_Escorts(4, 6))
				S78_Rec("COUNT", "scn=" .. s.name .. "|what=escort_centroid|n=" .. S78_Count(S78_Escorts(4, 6)) ..
					"|x=" .. S78_Num(cx) .. "|y=" .. S78_Num(cy))
			end
		end,
	}
end

-- ---- s78f t09 / t11 scenarios (2026-11-05) -------------------------------------------------------------------------

-- t09: Magnepulse slew. Three Fire_Special_Weapon calls, each once the weapon is ready: at tank A (side +x of the
-- cannon), then at tank B on the opposite side (about 180 degrees of yaw), then at B again (turret should hold its
-- aim: charge-up at once). The reader has the Magnepulse_Blob spawn frames; delay = blob frame - FIRE record frame.
S78_MAG_TYPES = { "Ground_Magnepulse_Cannon", "Skirmish_Ground_Magnepulse_Cannon_E" }

function S78_MagFire(s, tag)
	local c = s.objs.mag
	local t = s.objs[tag]
	local ready = S78_Flag(c, "Is_Ability_Ready", "FIRE_LOBBING_SUPERWEAPON")
	local ok, ret = pcall(function() return c.Fire_Special_Weapon(t, PlayerObject) end)
	S78_Rec("FIRE", "scn=" .. s.name .. "|tag=" .. tag .. "|i=" .. s.mag_i .. "|ok=" .. (ok and "1" or "0") .. "|ret=" ..
		S78_Clean(ret) .. "|ready=" .. ready)
	return ok
end

function S78_ScnMagnepulse()
	return {
		name = "MAGNEPULSE",
		dur = 3600,
		steps = {
			{ at = 0, run = function(s)
				local w, how = S78_At("site", 0, 0)
				for i, tn in ipairs(S78_MAG_TYPES) do
					if s.objs.mag == nil then
						S78_Attacker("mag", S78_Spawn("mag", tn, w, how, PlayerObject))
					end
				end
				local wa, hwa = S78_At("site", -800, 0)
				S78_Victim("va", S78_Spawn("va", "T4B_Tank", wa, hwa, S78_VICTIM_PLAYER), false)
				local wb, hwb = S78_At("site", 800, 0)
				S78_Victim("vb", S78_Spawn("vb", "T4B_Tank", wb, hwb, S78_VICTIM_PLAYER), false)
				s.mag_i = 0
				s.mag_next = 60
				s.mag_plan = { "va", "vb", "vb" }
				s.mag_last_ready = nil
			end },
		},
		tick = function(s, rel)
			if s.objs.mag == nil or s.mag_i >= 3 then
				return
			end
			local ready = S78_Flag(s.objs.mag, "Is_Ability_Ready", "FIRE_LOBBING_SUPERWEAPON")
			if ready ~= s.mag_last_ready then
				s.mag_last_ready = ready
				S78_Rec("COUNT", "scn=" .. s.name .. "|what=mag_ready|ready=" .. ready)
			end
			-- fire when ready; with an unreadable flag, once every 30 frames (only a ready call fires)
			-- (retail free-store Lua has no math library: s78g 2026-11-05, so no math.mod)
			local slot = ready == "?" and rel >= (s.mag_try or 0)
			if slot then
				s.mag_try = rel + 30
			end
			if rel >= s.mag_next and (ready == "1" or slot) then
				local tag = s.mag_plan[s.mag_i + 1]
				S78_MagFire(s, tag)
				if ready == "1" then
					s.mag_i = s.mag_i + 1
					s.mag_next = rel + 300
				end
			end
		end,
	}
end

-- t09: a Merc squad garrisoned in a bunker; one mortal target at a time, stepped out across the grenade reach
-- (300..440 by 10, 150 frames each). Per window the parser counts grenades and synced RNG draws (reader RNG lines).
S78_BUNKER_D = { 0, 300, 320, 340, 360, 370, 380, 385, 390, 395, 400, 405, 410, 420, 440 }
-- BUNKER2 (s78f 2026-11-05): the first pass threw nothing at 300..440; the merc's flat-ground solver reach is 172.5
-- (LOB_SOLVER sec. 4), so step across it
S78_BUNKER2_D = { 0, 120, 150, 165, 170, 175, 180, 190, 200, 220, 240, 280 }

function S78_BunkerStep(i, D)
	return { at = 120 + (i - 1) * 150, run = function(s)
		S78_Despawn("bv")
		s.objs.bv = nil
		local d = D[i]
		if d > 0 then
			local w, how = S78_At("site", d, 0)
			S78_Victim("bv", S78_Spawn("bv", "T4B_Tank", w, how, S78_VICTIM_PLAYER), true)
		end
		S78_Mark("win" .. i, "d=" .. d)
	end }
end

function S78_ScnBunker()
	return S78_ScnBunkerD("BUNKER", S78_BUNKER_D)
end

function S78_ScnBunker2()
	return S78_ScnBunkerD("BUNKER2", S78_BUNKER2_D)
end

function S78_ScnBunkerD(name, D)
	local steps = {
		{ at = 0, run = function(s)
			local w, how = S78_At("site", 0, 0)
			S78_Attacker("bunker", S78_Spawn("bunker", "Garrison_Bunker_Rebel", w, how, PlayerObject))
			local w2, how2 = S78_At("site", 0, 90)
			local t = S78_Try("Find_Object_Type:Underworld_Merc_Squad", function() return Find_Object_Type("Underworld_Merc_Squad") end)
			local r = t and w2 and S78_Try("Spawn_Unit:Underworld_Merc_Squad", function() return Spawn_Unit(t, w2, PlayerObject) end)
			s.mercs = {}
			if type(r) == "table" then
				for k, o in ipairs(r) do
					if S78_Valid(o) then
						table.insert(s.mercs, o)
						table.insert(s.spawned, o)
						s.objs["merc" .. k] = o
						table.insert(s.order, "merc" .. k)
						pcall(function() o.Make_Invulnerable(true) end)
						pcall(function() o.Prevent_AI_Usage(true) end)
					end
				end
			end
			S78_Rec("SPAWN", "scn=" .. s.name .. "|tag=mercs|type=Underworld_Merc_Squad|ok=" .. ((table.getn(s.mercs) > 0) and "1" or "0") ..
				"|n=" .. table.getn(s.mercs) .. "|ids=" .. (function()
					local ids = ""
					for k, o in ipairs(s.mercs) do ids = ids .. S78_ObjId(o) .. "," end
					return ids
				end)())
		end },
		{ at = 30, run = function(s)
			for k, o in ipairs(s.mercs or {}) do
				local b = s.objs.bunker
				local ok, ret = pcall(function() return o.Garrison(b) end)
				S78_Rec("ORDER", "scn=" .. s.name .. "|tag=merc" .. k .. "|order=garrison|ok=" .. (ok and "1" or "0") .. "|ret=" .. S78_Clean(ret))
			end
		end },
		{ at = 110, run = function(s)
			for k, o in ipairs(s.mercs or {}) do
				local ok, v = pcall(function() return o.Is_In_Garrison() end)
				S78_Rec("COUNT", "scn=" .. s.name .. "|what=in_garrison|tag=merc" .. k .. "|v=" .. S78_Clean(ok and v or "?"))
			end
		end },
	}
	for i = 1, table.getn(D) do
		table.insert(steps, S78_BunkerStep(i, D))
	end
	return { name = name, dur = 120 + table.getn(D) * 150, steps = steps }
end

-- t11 toggle (s78f 2026-11-05): six Pod_Walkers (no ATTACKIDLE clip) facing away from held invulnerable targets,
-- locomotor suspended. Every 300 frames: "on" = Attack_Target on each (targeting state 3 / 4), 150 frames later
-- "off" = Stop() on each. If slot 47 redraws Get_Float(0.2, 0.75) on every service in state 3 / 4, the synced draws
-- per frame step up by about 6 (one per walker per service) at each "on" and down at each "off".
S78_IDLE47T_CYCLES = 10

function S78_ScnIdle47T()
	local steps = {
		{ at = 0, run = function(s)
			for k = 1, 6 do
				local w, how = S78_At("site", (k - 1) * 90, 0)
				local u = S78_Spawn("i" .. k, "Pod_Walker", w, how, PlayerObject)
				S78_Attacker("i" .. k, u)
				if u then
					local f = S78_At("site", (k - 1) * 90, 600)
					S78_Order("i" .. k, "face", function() u.Face_Immediate(f) end)
					pcall(function() u.Suspend_Locomotor(true) end)
				end
				local wv, hv = S78_At("site", (k - 1) * 90, -160)
				S78_Victim("iv" .. k, S78_Spawn("iv" .. k, "T4B_Tank", wv, hv, S78_VICTIM_PLAYER), false)
			end
		end },
	}
	for c0 = 1, S78_IDLE47T_CYCLES do
		local c = c0
		table.insert(steps, { at = c * 300, run = function(s)
			for k = 1, 6 do
				local u, v = s.objs["i" .. k], s.objs["iv" .. k]
				if u and v then
					S78_Order("i" .. k, "attack", function() u.Attack_Target(v) end)
				end
			end
			S78_Mark("on" .. c)
		end })
		table.insert(steps, { at = c * 300 + 150, run = function(s)
			for k = 1, 6 do
				local u = s.objs["i" .. k]
				if u then
					S78_Order("i" .. k, "stop", function() u.Stop() end)
				end
			end
			S78_Mark("off" .. c)
		end })
	end
	return { name = "IDLE47T", dur = (S78_IDLE47T_CYCLES + 1) * 300, steps = steps }
end

-- t11: slot 47 on a model without an Attack Idle clip. Six attackers per phase, locomotor suspended, facing away from
-- their (held, invulnerable) targets, ordered to attack them: they stay in targeting state 3 (needs to turn).
-- Phases: C0 (nothing), A (Rebel_Infiltrator, has ATTACKIDLE), B (Pod_Walker, EV_PODWALKER has no ATTACKIDLE), C1.
-- The parser compares the synced RNG draws per frame between the phases.
function S78_Idle47Phase(name, tname, at)
	return { at = at, run = function(s)
		for k = 1, 6 do
			S78_Despawn("i" .. k)
			S78_Despawn("iv" .. k)
		end
		if tname then
			for k = 1, 6 do
				local w, how = S78_At("site", (k - 1) * 90, 0)
				local u = S78_Spawn("i" .. k, tname, w, how, PlayerObject)
				S78_Attacker("i" .. k, u)
				if u then
					local f = S78_At("site", (k - 1) * 90, 600)
					S78_Order("i" .. k, "face", function() u.Face_Immediate(f) end)
					pcall(function() u.Suspend_Locomotor(true) end)
				end
				local wv, hv = S78_At("site", (k - 1) * 90, -160)
				local v = S78_Spawn("iv" .. k, "T4B_Tank", wv, hv, S78_VICTIM_PLAYER)
				S78_Victim("iv" .. k, v, false)
				if u and v then
					S78_Order("i" .. k, "attack", function() u.Attack_Target(v) end)
				end
			end
		end
		S78_Mark(name)
	end }
end

function S78_ScnIdle47()
	return {
		name = "IDLE47",
		dur = 2400,
		steps = {
			S78_Idle47Phase("c0", nil, 0),
			S78_Idle47Phase("a", "Rebel_Infiltrator", 600),
			S78_Idle47Phase("b", "Pod_Walker", 1200),
			S78_Idle47Phase("c1", nil, 1800),
		},
	}
end

-- t09 (space): a human-owned Tartan G and a TIE squadron next to the human's fleet. Leg 1: the squadron gets
-- Guard_Target(G) while G is at rest (+60, "guard_rest"), then G moves +3000 at +600 ("g_move1"): does the guard
-- follow a frigate that starts moving? Leg 2: G moves back at +3000 ("g_move2") and the squadron gets
-- Guard_Target(G) again 90 frames later, while G is moving ("guard_moving"). From +3300 the player may select the
-- squadron and shift+right-click a waypoint (README "G1"); G stops by +5400, and from +5700 ("g2_window") the
-- player repeats it on the stationary G (control "G2", expected pop within 30 frames).
function S78_ScnGuardShift()
	return {
		name = "GUARD_SHIFT",
		dur = 8400,
		steps = {
			{ at = 0, run = function(s)
				local hp = S78_Try("Find_Player:human", function() return Find_Player(S78_CFG.human_faction) end)
				s.hp = hp
				-- next to the human's own fleet (the camera starts there; Point_Camera_At from this script did not
				-- move it in the 2026-11-05 run)
				local units = {}
				local okl, l = pcall(function() return Find_All_Objects_Of_Type(hp) end)
				if okl and type(l) == "table" then
					for i, o in ipairs(l) do
						if i <= 12 and S78_Valid(o) then
							table.insert(units, o)
						end
					end
				end
				local cx, cy = S78_Centroid(units)
				s.gx, s.gy = cx, cy
				local w, how = S78_At("sq", 0, 0)
				local w2, how2 = S78_At("sq", -250, 250)
				if cx then
					w = S78_Try("Create_Position", function() return Create_Position(cx + 400, cy, 0) end)
					w2 = S78_Try("Create_Position", function() return Create_Position(cx + 150, cy + 250, 0) end)
					how, how2 = "human", "human"
				end
				local g = S78_Spawn("g", "Tartan_Patrol_Cruiser", w, how, hp)
				if g then
					pcall(function() g.Make_Invulnerable(true) end)
				end
				local sq = S78_Spawn("squad", "TIE_Fighter_Squadron", w2, how2, hp)
				if sq then
					pcall(function() sq.Make_Invulnerable(true) end)
				end
			end },
			{ at = 60, run = function(s)
				S78_Order("squad", "guard_rest", function() s.objs.squad.Guard_Target(s.objs.g) end)
				S78_Mark("guard_rest")
			end },
			{ at = 600, run = function(s)
				local w = S78_At("sq", 3000, 0)
				if s.gx then
					w = S78_Try("Create_Position", function() return Create_Position(s.gx + 3400, s.gy, 0) end)
				end
				S78_Order("g", "move_far", function() s.objs.g.Move_To(w) end)
				S78_Mark("g_move1")
			end },
			{ at = 3000, run = function(s)
				local w = S78_At("sq", 0, 0)
				if s.gx then
					w = S78_Try("Create_Position", function() return Create_Position(s.gx + 400, s.gy, 0) end)
				end
				S78_Order("g", "move_back", function() s.objs.g.Move_To(w) end)
				S78_Mark("g_move2")
			end },
			{ at = 3090, run = function(s)
				S78_Order("squad", "guard_moving", function() s.objs.squad.Guard_Target(s.objs.g) end)
				S78_Mark("guard_moving")
			end },
			{ at = 5700, run = function(s)
				S78_Mark("g2_window")
			end },
		},
	}
end

-- s78f t12: a deployed SPMAT (locomotor suspended) given an explicit Attack_Target on one held T4B at a time, d from
-- its centre (LOB_SOLVER sec. 4: scan R = 550, flat-ground solver reach 544.0 from a ground-level shooter; the real
-- fire bone is 90 above the ground). 240 frames per window.
S78_SPMAT_EDGE_D = { 500, 530, 540, 544, 546, 548, 550, 553, 560, 575 }

function S78_ScnSpmatEdge()
	local steps = {
		{ at = 0, run = function(s)
			local w, how = S78_At("site", 0, 0)
			S78_Attacker("spmat", S78_Spawn("spmat", "SPMAT_Walker", w, how, PlayerObject))
			local f = S78_At("site", 600, 0)
			S78_Order("spmat", "face", function() s.objs.spmat.Face_Immediate(f) end)
		end },
		{ at = 30, run = function(s) S78_Act("spmat", "deploy", "DEPLOY", "bool", true) end },
		{ at = 120, run = function(s)
			pcall(function() s.objs.spmat.Suspend_Locomotor(true) end)
		end },
	}
	for i = 1, table.getn(S78_SPMAT_EDGE_D) do
		local ii = i
		table.insert(steps, { at = 150 + (ii - 1) * 240, run = function(s)
			S78_Despawn("ev")
			s.objs.ev = nil
			local d = S78_SPMAT_EDGE_D[ii]
			local w, how = S78_At("site", d, 0)
			local v = S78_Spawn("ev", "T4B_Tank", w, how, S78_VICTIM_PLAYER)
			S78_Victim("ev", v, false)
			if v and s.objs.spmat then
				S78_Order("spmat", "attack_ev", function() s.objs.spmat.Attack_Target(v) end)
			end
			S78_Mark("win" .. ii, "d=" .. d)
		end })
	end
	return { name = "SPMAT_EDGE", dur = 150 + table.getn(S78_SPMAT_EDGE_D) * 240, steps = steps }
end

-- s78f t12 (t11 follow-up): slot 47 with and without an Attack Idle clip, toggled by the victims. Six attackers of one
-- type stay (locomotor suspended, facing away); each 300-frame cycle "on" spawns six held victims and orders the
-- attacks, "off" (+150) despawns the victims. Phases P (Pod_Walker, no ATTACKIDLE), I (Rebel_Infiltrator, has
-- ATTACKIDLE), V (victims only, no attackers: the victims' own draws). The parser compares the on / off steps.
S78_IDLE47V_CYCLES = 5

-- noauto (s78g): abilities whose autofire is switched off on each attacker (Dark_Trooper_PhaseII: JET_PACK)
-- opt (s78g IDLE47X): { n = attackers (max 12), gap = spacing along the strip, along0 = first position,
-- cycles = on / off cycles, period = cycle length (victims for the first half) }; defaults are the IDLE47V / W set-up
-- (6 attackers 90 apart from 0, 5 cycles of 300)
-- opt.nofire (s78i IDLE47N): "spawn" = Prevent_All_Fire(true) on each attacker when it is placed, before any attack
-- order; "order" = Prevent_All_Fire(true) on every attacker opt.nofire_at (default 6) frames after each cycle's attack
-- orders, and (false) when the victims go. nil = attackers fire (the IDLE47V / W / X / Q set-up).
function S78_Idle47NoFire(s, name, n, on, cc)
	local ok = 0
	for k = 1, n do
		local u = s.objs["i" .. k]
		if S78_Valid(u) and pcall(function() u.Prevent_All_Fire(on) end) then
			ok = ok + 1
		end
	end
	S78_Rec("ORDER", "scn=" .. s.name .. "|tag=" .. name .. "_c" .. cc .. "|order=" ..
		(on and "nofire_on" or "nofire_off") .. "|ok=" .. ok .. "|n=" .. n)
end

function S78_Idle47VPhase(steps, name, tname, t0, noauto, opt)
	opt = opt or {}
	local n, gap, a0 = opt.n or 6, opt.gap or 90, opt.along0 or 0
	local cycles = opt.cycles or S78_IDLE47V_CYCLES
	local per = opt.period or 300
	table.insert(steps, { at = t0, run = function(s)
		for k = 1, 12 do
			S78_Despawn("i" .. k)
			S78_Despawn("iv" .. k)
			s.objs["i" .. k] = nil
			s.objs["iv" .. k] = nil
		end
		for k = 1, n do
			if tname then
				local w, how = S78_At("site", a0 + (k - 1) * gap, 0)
				local u = S78_Spawn("i" .. k, tname, w, how, PlayerObject)
				S78_Attacker("i" .. k, u)
				if u then
					local f = S78_At("site", a0 + (k - 1) * gap, 600)
					S78_Order("i" .. k, "face", function() u.Face_Immediate(f) end)
					pcall(function() u.Suspend_Locomotor(true) end)
					for j, ab in ipairs(noauto or {}) do
						S78_Setup("i" .. k, u, { { "Set_Single_Ability_Autofire", ab, false } })
					end
					if opt.nofire == "spawn" then
						S78_Setup("i" .. k, u, { { "Prevent_All_Fire", true } })
					end
				end
			end
		end
		S78_Mark("phase_" .. name)
	end })
	for c = 1, cycles do
		local cc = c
		table.insert(steps, { at = t0 + cc * per, run = function(s)
			for k = 1, n do
				local wv, hv = S78_At("site", a0 + (k - 1) * gap, -160)
				local v = S78_Spawn("iv" .. k, "T4B_Tank", wv, hv, S78_VICTIM_PLAYER)
				S78_Victim("iv" .. k, v, false)
				local u = s.objs["i" .. k]
				if u and v then
					S78_Order("i" .. k, "attack", function() u.Attack_Target(v) end)
				end
			end
			S78_Mark(name .. "_on" .. cc)
		end })
		if opt.nofire == "order" then
			table.insert(steps, { at = t0 + cc * per + (opt.nofire_at or 6), run = function(s)
				S78_Idle47NoFire(s, name, n, true, cc)
			end })
		end
		table.insert(steps, { at = t0 + cc * per + per / 2, run = function(s)
			for k = 1, n do
				S78_Despawn("iv" .. k)
				s.objs["iv" .. k] = nil
			end
			-- after the victims are gone (fire allowed again only once there is nothing to shoot at)
			if opt.nofire == "order" then
				S78_Idle47NoFire(s, name, n, false, cc)
			end
			S78_Mark(name .. "_off" .. cc)
		end })
	end
end

function S78_ScnIdle47V()
	local steps = {}
	local len = (S78_IDLE47V_CYCLES + 1) * 300
	S78_Idle47VPhase(steps, "P", "Pod_Walker", 0)
	S78_Idle47VPhase(steps, "I", "Rebel_Infiltrator", len)
	S78_Idle47VPhase(steps, "V", nil, 2 * len)
	return { name = "IDLE47V", dur = 3 * len, steps = steps }
end

-- s78f t12 (t09 follow-up, space, by hand): guard ordered WHILE the frigate moves, then a shift waypoint while it still
-- moves (FIGHTER_FLYING sec. 4.10 B 3 predicts the waypoint waits until G stops). G moves +4500 at +60 (about 1070
-- frames at 4.2 / frame); the squadron gets Guard_Target(G) at +150; the player shift-clicks a waypoint between
-- "shift1" (+300) and G's stop. Second leg: G moves back at +1500, guard at +1590, "shift2" at +1700.
function S78_ScnGuardMove()
	local def = S78_ScnGuardShift()
	local spawn = def.steps[1]
	return {
		name = "GUARD_MOVE",
		dur = 3300,
		steps = {
			spawn,
			{ at = 60, run = function(s)
				local w = S78_Try("Create_Position", function() return Create_Position(s.gx + 4900, s.gy, 0) end)
				S78_Order("g", "move_far", function() s.objs.g.Move_To(w) end)
				S78_Mark("g_move1")
			end },
			{ at = 150, run = function(s)
				S78_Order("squad", "guard_moving1", function() s.objs.squad.Guard_Target(s.objs.g) end)
				S78_Mark("guard_moving1")
			end },
			{ at = 300, run = function(s) S78_Mark("shift1") end },
			{ at = 1500, run = function(s)
				local w = S78_Try("Create_Position", function() return Create_Position(s.gx + 400, s.gy, 0) end)
				S78_Order("g", "move_back", function() s.objs.g.Move_To(w) end)
				S78_Mark("g_move2")
			end },
			{ at = 1590, run = function(s)
				S78_Order("squad", "guard_moving2", function() s.objs.squad.Guard_Target(s.objs.g) end)
				S78_Mark("guard_moving2")
			end },
			{ at = 1700, run = function(s) S78_Mark("shift2") end },
		},
	}
end

-- ---- s78g scenarios (2026-11-05) -----------------------------------------------------------------------------------

-- s78g t01: SPMAT range edge, again (LOB_SOLVER sec. 9). SPMAT_EDGE suspended the deployed walker's locomotor: the
-- first Attack_Target sent it to 0x1a (undeploying) and it stuck there, so nothing fired. Here every window has a
-- fresh SPMAT_Walker, no Suspend_Locomotor: faced down the strip, ordered to attack a held T4B at 275 (the s78f t01
-- set-up that fired) and deployed; at +150 the 275 tank is despawned and a held T4B at d on the same line (in front
-- of the walker) gets an explicit Attack_Target. 450 frames per window.
S78_EDGE2_D = { 500, 530, 540, 544, 546, 548, 550, 553, 560 }
S78_EDGE2_WIN = 450

-- auto (s78g EDGE3): no order at +150, the edge tank is left to the walker's own target scan; dlist = the distances
function S78_Edge2Window(steps, i, auto, dlist)
	dlist = dlist or S78_EDGE2_D
	local t0 = (i - 1) * S78_EDGE2_WIN
	local sp, aim, ev = "sp" .. i, "aim" .. i, "ev" .. i
	table.insert(steps, { at = t0, run = function(s)
		if i > 1 then
			S78_Despawn("sp" .. (i - 1))
			S78_Despawn("aim" .. (i - 1))
			S78_Despawn("ev" .. (i - 1))
		end
		local w, how = S78_At("site", 0, 0)
		local u = S78_Spawn(sp, "SPMAT_Walker", w, how, PlayerObject)
		S78_Attacker(sp, u)
		if u then
			local f = S78_At("site", 300, 0)
			if not S78_Order(sp, "face_immediate", function() u.Face_Immediate(f) end) then
				S78_Order(sp, "turn_to_face", function() u.Turn_To_Face(f) end)
			end
		end
	end })
	table.insert(steps, { at = t0 + 6, run = function(s)
		local w, how = S78_At("site", 275, 0)
		local v = S78_Spawn(aim, "T4B_Tank", w, how, S78_VICTIM_PLAYER)
		S78_Victim(aim, v, false)
		if v and s.objs[sp] then
			S78_Order(sp, "attack_aim", function() s.objs[sp].Attack_Target(v) end)
		end
		S78_Mark("aim" .. i)
	end })
	table.insert(steps, { at = t0 + 21, run = function(s) S78_Act(sp, "deploy" .. i, "DEPLOY", "bool", true) end })
	table.insert(steps, { at = t0 + 150, run = function(s)
		S78_Despawn(aim)
		local d = dlist[i]
		local w, how = S78_At("site", d, 0)
		local v = S78_Spawn(ev, "T4B_Tank", w, how, S78_VICTIM_PLAYER)
		S78_Victim(ev, v, false)
		if v and s.objs[sp] and not auto then
			S78_Order(sp, "attack_ev", function() s.objs[sp].Attack_Target(v) end)
		end
		S78_Mark("win" .. i, "d=" .. d)
	end })
end

function S78_ScnSpmatEdge2()
	local steps = {}
	for i = 1, table.getn(S78_EDGE2_D) do
		S78_Edge2Window(steps, i)
	end
	return { name = "SPMAT_EDGE2", dur = table.getn(S78_EDGE2_D) * S78_EDGE2_WIN, steps = steps }
end

-- s78g t01 rerun: in SPMAT_EDGE2 the explicit Attack_Target on the edge tank made the deployed walker undeploy and
-- drive about 209 units towards it, so the range edge was never measured. SPMAT_EDGE3 deploys the same way (attack the
-- 275 tank, DEPLOY), then at +150 swaps the 275 tank for one at d with NO order: the deployed walker has to acquire it
-- by itself. 450 is an in-range control. The parser checks loco 0x19 is held and the walker did not move.
S78_EDGE3_D = { 450, 500, 530, 540, 544, 546, 548, 550, 553, 560 }

function S78_ScnSpmatEdge3()
	local steps = {}
	for i = 1, table.getn(S78_EDGE3_D) do
		S78_Edge2Window(steps, i, true, S78_EDGE3_D)
	end
	return { name = "SPMAT_EDGE3", dur = table.getn(S78_EDGE3_D) * S78_EDGE2_WIN, steps = steps }
end

-- s78g t04 control (coordinator 2026-11-05): an undeployed SPMAT, no DEPLOY call, ordered to attack a held T4B at
-- 275; window H with a Hostile-owned tank, window E with one of the human's faction, same frame offsets (600 each,
-- fresh walker per window). Answers "does the owner of the victim matter" without the deploy transition.
function S78_SpmatCtrlWindow(steps, k, fac, t0)
	local sp, v = "csp" .. k, "cv" .. k
	table.insert(steps, { at = t0, run = function(s)
		if k > 1 then
			S78_Despawn("csp" .. (k - 1))
			S78_Despawn("cv" .. (k - 1))
		end
		local w, how = S78_At("site", 0, 0)
		local u = S78_Spawn(sp, "SPMAT_Walker", w, how, PlayerObject)
		S78_Attacker(sp, u)
		if u then
			local f = S78_At("site", 300, 0)
			if not S78_Order(sp, "face_immediate", function() u.Face_Immediate(f) end) then
				S78_Order(sp, "turn_to_face", function() u.Turn_To_Face(f) end)
			end
		end
	end })
	table.insert(steps, { at = t0 + 30, run = function(s)
		local p = S78_Try("Find_Player:" .. fac, function() return Find_Player(fac) end)
		local w, how = S78_At("site", 275, 0)
		local t = S78_Spawn(v, "T4B_Tank", w, how, p)
		S78_Victim(v, t, false)
		if t and s.objs[sp] then
			S78_Order(sp, "attack_" .. string.lower(fac), function() s.objs[sp].Attack_Target(t) end)
		end
		S78_Mark("ctrl" .. k, "fac=" .. fac .. "|player=" .. (p and "1" or "0"))
	end })
end

function S78_ScnSpmatCtrl()
	local steps = {}
	S78_SpmatCtrlWindow(steps, 1, "Hostile", 0)
	S78_SpmatCtrlWindow(steps, 2, S78_CFG.human_faction or "Empire", 600)
	return { name = "SPMAT_CTRL", dur = 1200, steps = steps }
end

-- s78g t03: slot 47 with-clip control. Dark_Trooper_PhaseII (single GroundVehicle, own TARGETING + WEAPON, no TURRET,
-- no Deploys, range 180; model EI_DARKTROOPER_TWO has ATTACKIDLE_00 / _01 and ATTACK clips) replaces the
-- Rebel_Infiltrator. Same toggle as IDLE47V: phases P (Pod_Walker, no ATTACKIDLE), D (Dark_Trooper_PhaseII, JET_PACK
-- autofire off), V (victims only).
function S78_ScnIdle47W()
	local steps = {}
	local len = (S78_IDLE47V_CYCLES + 1) * 300
	S78_Idle47VPhase(steps, "P", "Pod_Walker", 0)
	S78_Idle47VPhase(steps, "D", "Dark_Trooper_PhaseII", len, { "JET_PACK" })
	S78_Idle47VPhase(steps, "V", nil, 2 * len)
	return { name = "IDLE47W", dur = 3 * len, steps = steps }
end

-- s78g t03 rerun: IDLE47W was noise-bound (on / off steps scattered +-100..180 against an effect of about 20; the
-- land background is ~440 draws / frame and drifts). IDLE47X doubles the attackers (12, 75 apart from -200) and uses
-- 15 shorter cycles per phase (200 frames: 100 with victims, 100 without; the W attackers reached targeting state 4
-- within 20 frames). The parser reports the per-cycle on-minus-off draws / frame as mean +- stderr per phase and
-- says UNCLEAR when P, D and V cannot be told apart at 3 sigma.
S78_IDLE47X_OPT = { n = 12, gap = 75, along0 = -200, cycles = 15, period = 200 }

function S78_ScnIdle47X()
	local steps = {}
	local len = (S78_IDLE47X_OPT.cycles + 1) * S78_IDLE47X_OPT.period
	S78_Idle47VPhase(steps, "P", "Pod_Walker", 0, nil, S78_IDLE47X_OPT)
	S78_Idle47VPhase(steps, "D", "Dark_Trooper_PhaseII", len, { "JET_PACK" }, S78_IDLE47X_OPT)
	S78_Idle47VPhase(steps, "V", nil, 2 * len, nil, S78_IDLE47X_OPT)
	return { name = "IDLE47X", dur = 3 * len, steps = steps }
end

-- s78g t05: first shot after a late spawn, irregular spawn gaps. The s78f t12 REINF run gave delays 18 / 29 / 41 / 52
-- with first shots 161 / 162 / 161 frames apart (spawns 150 apart): a shared ~161-frame clock, not a per-unit random
-- delay? Six MZ8s at gaps 200 / 170 / 250 / 140 / 290 (not multiples of 161), each attacking the held victim at once.
S78_REINF2_AT = { 30, 230, 400, 650, 790, 1080 }

function S78_ScnReinf2()
	local steps = { { at = 0, run = function(s)
		local w, how = S78_At("site", 200, 0)
		S78_Victim("victim", S78_Spawn("victim", "T4B_Tank", w, how, S78_VICTIM_PLAYER), false)
	end } }
	for k = 1, table.getn(S78_REINF2_AT) do
		local kk = k
		table.insert(steps, { at = S78_REINF2_AT[kk], run = function(s)
			local tag = "mz8_" .. kk
			local w, how = S78_At("site", 0, (kk - 3.5) * 70)
			local u = S78_Spawn(tag, "MZ8_Pulse_Cannon_Tank", w, how, PlayerObject)
			S78_Attacker(tag, u)
			if u then
				S78_Order(tag, "attack_victim", function() u.Attack_Target(s.objs.victim) end)
			end
		end })
	end
	return { name = "REINF2", dur = 1380, steps = steps }
end

-- s78g t02 (space, orders by hand): guard ordered while G moves, then a shift waypoint while it still moves
-- (FIGHTER_FLYING sec. 4.10 B 3 / E). At the map centre ('muz', one click on the minimap centre, far from both fleets
-- and from the map edge): a human-owned Broadside_Class_Cruiser G (no hangar; max speed 1.8 x 1.2 = 2.16 / frame)
-- shuttles along the line through the centre perpendicular to the start line (side +1500 <-> -1500, about 1400 frames
-- a leg; the next leg is ordered 250 before the end of the current one, so G never stops) and a human-owned
-- TIE_Fighter_Squadron waits at the centre. Game_Messages say when to right-click G (guard) and when to shift-right-
-- click a waypoint, twice; G stops at +3900. A COUNT gh record every 30 frames has G's and the squadron's Lua position.
S78_GH_LEG = 1500

function S78_SiteXY(site, along, side)
	local m = S78_Pts()
	local p = m.pts[site]
	if not (m.points_ok and p) then
		return nil
	end
	local ux, uy = p.ux or 1.0, p.uy or 0.0
	return p.x + along * ux - side * uy, p.y + along * uy + side * ux
end

function S78_GhNextLeg(s, rel)
	s.gh_leg = (s.gh_leg or 0) + 1
	-- odd legs to +1500, even legs to -1500 (no math library in the retail free-store Lua)
	s.gh_side = -(s.gh_side or -S78_GH_LEG)
	local side = s.gh_side
	local w = S78_At("muz", 0, side)
	s.gh_tx, s.gh_ty = S78_SiteXY("muz", 0, side)
	s.gh_t0 = rel
	S78_Order("g", "leg" .. s.gh_leg, function() s.objs.g.Move_To(w) end)
end

function S78_GhMsg(s, id, mark)
	S78_Msg(id)
	S78_Mark(mark)
end

function S78_ScnGuardHand()
	return {
		name = "GUARD_HAND",
		dur = 4500,
		steps = {
			{ at = 0, run = function(s)
				local hp = S78_Try("Find_Player:human", function() return Find_Player(S78_CFG.human_faction) end)
				local wg, hg = S78_At("muz", 0, -300)
				local g = S78_Spawn("g", "Broadside_Class_Cruiser", wg, hg, hp)
				if g then
					pcall(function() g.Make_Invulnerable(true) end)
				end
				local ws, hs = S78_At("muz", -300, 0)
				local sq = S78_Spawn("squad", "TIE_Fighter_Squadron", ws, hs, hp)
				if sq then
					pcall(function() sq.Make_Invulnerable(true) end)
					pcall(function() Point_Camera_At(sq) end)
				end
				S78_GhMsg(s, "TEXT_S78_GUARD_FIND", "find")
			end },
			{ at = 30, run = function(s)
				S78_GhNextLeg(s, 30)
				S78_Mark("g_move")
			end },
			{ at = 450, run = function(s) S78_GhMsg(s, "TEXT_S78_GUARD_GO", "guard_go") end },
			{ at = 1350, run = function(s) S78_GhMsg(s, "TEXT_S78_SHIFT_GO", "shift_go") end },
			{ at = 2250, run = function(s) S78_GhMsg(s, "TEXT_S78_GUARD_GO2", "guard_go2") end },
			{ at = 3000, run = function(s) S78_GhMsg(s, "TEXT_S78_SHIFT_GO2", "shift_go2") end },
			{ at = 3900, run = function(s)
				s.gh_stopped = true
				local g = s.objs.g
				if not S78_Order("g", "stop", function() g.Stop() end) then
					local x, y, z = S78_XYZ(g)
					S78_Order("g", "stop_move_here", function() g.Move_To(Create_Position(x, y, z or 0)) end)
				end
				S78_GhMsg(s, "TEXT_S78_G_STOP", "g_stop")
			end },
		},
		tick = function(s, rel)
			local g = s.objs.g
			if not S78_Valid(g) then
				return
			end
			local gx, gy = S78_XYZ(g)
			if not s.gh_stopped and s.gh_leg and type(gx) == "number" and type(s.gh_tx) == "number" then
				local d2 = (gx - s.gh_tx) * (gx - s.gh_tx) + (gy - s.gh_ty) * (gy - s.gh_ty)
				if d2 < 250 * 250 or rel - s.gh_t0 > 1800 then
					S78_GhNextLeg(s, rel)
				end
			end
			if rel >= (s.gh_next or 0) then
				s.gh_next = rel + 30
				local sx, sy = S78_XYZ(s.objs.squad)
				S78_Rec("COUNT", "scn=" .. s.name .. "|what=gh|gx=" .. S78_Num(gx) .. "|gy=" .. S78_Num(gy) .. "|sx=" ..
					S78_Num(sx) .. "|sy=" .. S78_Num(sy) .. "|leg=" .. tostring(s.gh_leg))
			end
		end,
	}
end

-- ---- s78h scenarios (2026-11-05) -----------------------------------------------------------------------------------

-- s78h t01: SPMAT range edge by auto-acquire (LOB_SOLVER sec. 9). In SPMAT_EDGE3 (run 20261105-092438) the deployed
-- walker acquired nothing, not even the in-range control at 450: a deployed AI unit without an order may not acquire a
-- target outside the AI's vision. SPMAT_EDGE4 is EDGE3 plus vision at the edge tank: an AI-owned spotter
-- (Rebel_Infiltrator: invulnerable, no fire, locomotor suspended, 100 to the side of the tank spot, spawned with the
-- 275 aim tank so it sees the spot before the tank appears) and FogOfWar.Reveal(PlayerObject, tank, 250, 250), undone
-- at the next window. No order at the edge tank. Two passes, the 450 control first in each; the victims are the
-- human's (S78_VICTIM_PLAYER), never Hostile.
S78_EDGE4_D = { 450, 540, 544, 547, 550, 556, 450, 556, 550, 547, 544, 540 }
S78_EDGE4_SPOT_SIDE = 100

function S78_Edge4Unreveal(s)
	if s.e4_reveal then
		local h = s.e4_reveal
		s.e4_reveal = nil
		S78_Order("reveal", "undo_reveal", function() h.Undo_Reveal() end)
	end
end

function S78_Edge4Window(steps, i)
	S78_Edge2Window(steps, i, true, S78_EDGE4_D)
	local t0 = (i - 1) * S78_EDGE2_WIN
	local spot = "spot" .. i
	table.insert(steps, { at = t0 + 6, run = function(s)
		S78_Edge4Unreveal(s)
		if i > 1 then
			S78_Despawn("spot" .. (i - 1))
		end
		local d = S78_EDGE4_D[i]
		local w, how = S78_At("site", d, S78_EDGE4_SPOT_SIDE)
		local u = S78_Spawn(spot, "Rebel_Infiltrator", w, how, PlayerObject)
		if u then
			S78_Setup(spot, u, { { "Prevent_AI_Usage", true }, { "Make_Invulnerable", true },
				{ "Prevent_All_Fire", true }, { "Suspend_Locomotor", true } })
		end
		S78_Mark("spot" .. i, "d=" .. d .. "|side=" .. S78_EDGE4_SPOT_SIDE .. "|ok=" .. (u and "1" or "0"))
	end })
	-- after the edge tank appears (+150): reveal around it for the AI player
	table.insert(steps, { at = t0 + 152, run = function(s)
		local ev = s.objs["ev" .. i]
		local h = nil
		if S78_Valid(ev) then
			h = S78_Try("FogOfWar.Reveal", function() return FogOfWar.Reveal(PlayerObject, ev, 250, 250) end)
		end
		s.e4_reveal = h
		S78_Rec("ORDER", "scn=" .. s.name .. "|tag=ev" .. i .. "|order=reveal|ok=" .. (h and "1" or "0"))
	end })
	if i == table.getn(S78_EDGE4_D) then
		table.insert(steps, { at = t0 + S78_EDGE2_WIN - 6, run = function(s) S78_Edge4Unreveal(s) end })
	end
end

function S78_ScnSpmatEdge4()
	local steps = {}
	for i = 1, table.getn(S78_EDGE4_D) do
		S78_Edge4Window(steps, i)
	end
	return { name = "SPMAT_EDGE4", dur = table.getn(S78_EDGE4_D) * S78_EDGE2_WIN, steps = steps }
end

-- s78h t02: slot 47 with-clip comparison on a quiet background. In IDLE47X (092438) the background was ~440 synced
-- draws / frame +-100, mostly the armies' own units. IDLE47Q first measures the background (300 frames), then
-- despawns the AI player's mobile units (Is_Category Infantry / Vehicle / Air / LandHero; structures and anything
-- else stay) except the one farthest from the site (so the AI is never left without units), and the human's except
-- the Saboteur (S78_CFG.quiet_human, default on). The sweep repeats every 300 frames (AI reinforcements and builds).
-- Then the IDLE47X phases P / D / V run unchanged (12 attackers, 15 cycles of 200). The parser reports the
-- background before and after the sweep and the IDLE47X statistic.
S78_QUIET_CATS = { "Infantry", "Vehicle", "Air", "LandHero" }
S78_IDLE47Q_PRE = 900

function S78_QuietMobile(o)
	for i, c in ipairs(S78_QUIET_CATS) do
		local ok, v = pcall(function() return o.Is_Category(c) end)
		if ok and v == true then
			return true
		end
	end
	return false
end

function S78_QuietSweep(s, who, player, keep_farthest)
	if player == nil then
		S78_Rec("COUNT", "scn=" .. s.name .. "|what=quiet|who=" .. who .. "|player=0")
		return
	end
	local ok, l = pcall(function() return Find_All_Objects_Of_Type(player) end)
	if not (ok and type(l) == "table") then
		S78_Rec("COUNT", "scn=" .. s.name .. "|what=quiet|who=" .. who .. "|list=0")
		return
	end
	local mine = {}
	for i, o in ipairs(s.spawned) do
		mine[S78_ObjId(o)] = true
	end
	local sx, sy = S78_SiteXY("site", 0, 0)
	local mob, other = {}, 0
	for i, o in ipairs(l) do
		local hum = S78_HUMAN ~= nil and (o == S78_HUMAN or S78_ObjId(o) == S78_ObjId(S78_HUMAN))
		if S78_Valid(o) and not mine[S78_ObjId(o)] and not hum then
			if S78_QuietMobile(o) then
				table.insert(mob, o)
			else
				other = other + 1
			end
		end
	end
	local keep = nil
	if keep_farthest and type(sx) == "number" then
		local best = -1
		for i, o in ipairs(mob) do
			local x, y = S78_XYZ(o)
			if type(x) == "number" then
				local d2 = (x - sx) * (x - sx) + (y - sy) * (y - sy)
				if d2 > best then
					best = d2
					keep = o
				end
			end
		end
	elseif keep_farthest then
		keep = mob[1]
	end
	local gone = 0
	for i, o in ipairs(mob) do
		if o ~= keep and pcall(function() o.Despawn() end) then
			gone = gone + 1
		end
	end
	S78_Rec("COUNT", "scn=" .. s.name .. "|what=quiet|who=" .. who .. "|objects=" .. S78_Count(l) .. "|mobile=" ..
		table.getn(mob) .. "|despawned=" .. gone .. "|kept=" .. (keep and S78_ObjId(keep) or "-") .. "|other=" .. other)
end

-- the quiet-background stages shared by IDLE47Q and IDLE47N: bg_pre (0..300), the first sweep (+300, repeated every
-- 300 frames by S78_QuietTick), bg_post (+450 .. the first phase at S78_IDLE47Q_PRE)
function S78_QuietSteps(steps)
	table.insert(steps, { at = 0, run = function(s) S78_Mark("bg_pre") end })
	table.insert(steps, { at = 300, run = function(s)
		s.q_next = 300
		S78_Mark("sweep")
	end })
	table.insert(steps, { at = 450, run = function(s) S78_Mark("bg_post") end })
end

function S78_QuietTick(s, rel)
	if s.q_next and rel >= s.q_next then
		s.q_next = rel + 300
		S78_QuietSweep(s, "ai", PlayerObject, true)
		if S78_CFG.quiet_human ~= false then
			local hp = S78_Try("Find_Player:human", function() return Find_Player(S78_CFG.human_faction) end)
			-- the Saboteur is the human's kept unit; without it keep the farthest one
			S78_QuietSweep(s, "human", hp, not S78_Valid(S78_HUMAN))
		end
	end
end

function S78_ScnIdle47Q()
	local steps = {}
	local pre = S78_IDLE47Q_PRE
	local len = (S78_IDLE47X_OPT.cycles + 1) * S78_IDLE47X_OPT.period
	S78_QuietSteps(steps)
	S78_Idle47VPhase(steps, "P", "Pod_Walker", pre, nil, S78_IDLE47X_OPT)
	S78_Idle47VPhase(steps, "D", "Dark_Trooper_PhaseII", pre + len, { "JET_PACK" }, S78_IDLE47X_OPT)
	S78_Idle47VPhase(steps, "V", nil, pre + 2 * len, nil, S78_IDLE47X_OPT)
	return { name = "IDLE47Q", dur = pre + 3 * len, steps = steps, tick = S78_QuietTick }
end

-- s78i t01: slot 47 with the attackers' fire blocked but targeting kept. In IDLE47Q (104750) Pod_Walker and
-- Dark_Trooper_PhaseII both added ~1 synced draw per attacker per frame, but the attackers fired at their
-- (invulnerable) victims, and a land shot draws 2 (own-weapon inaccuracy, TACTICAL_COMBAT 3.1.2), so part of D may be
-- fire. IDLE47N is IDLE47Q (same quiet background and sweeps, 12 attackers, 200-frame cycles) with Prevent_All_Fire on
-- the attackers. The retail flag's effect on targeting is not mapped, so it is set two ways:
--   P  Pod_Walker (no ATTACKIDLE clip), flag set when placed: the no-clip control (should still draw ~1 / attacker);
--   D  Dark_Trooper_PhaseII (JET_PACK autofire off), flag set when placed, before any attack order;
--   E  Dark_Trooper_PhaseII, flag set 6 frames (two or more free-store services) after each cycle's attack orders,
--      cleared when the victims go (in case a flag set before the order stops the attack being taken);
--   F  Dark_Trooper_PhaseII firing (no flag), 3 cycles only: shows that its shots are seen as PROJ_ objects, so
--      "0 shots" in D / E means blocked;
--   V  victims only.
-- The parser reads, per phase, targeting state and clip while the victims exist, shots near the attackers, and the
-- draws per attacker per frame (on minus off, V subtracted).
S78_IDLE47N_SPAWN = { n = 12, gap = 75, along0 = -200, cycles = 15, period = 200, nofire = "spawn" }
S78_IDLE47N_ORDER = { n = 12, gap = 75, along0 = -200, cycles = 15, period = 200, nofire = "order", nofire_at = 6 }
S78_IDLE47N_FIRE = { n = 12, gap = 75, along0 = -200, cycles = 3, period = 200 }

function S78_ScnIdle47N()
	local steps = {}
	local t = S78_IDLE47Q_PRE
	S78_QuietSteps(steps)
	local phases = {
		{ "P", "Pod_Walker", nil, S78_IDLE47N_SPAWN },
		{ "D", "Dark_Trooper_PhaseII", { "JET_PACK" }, S78_IDLE47N_SPAWN },
		{ "E", "Dark_Trooper_PhaseII", { "JET_PACK" }, S78_IDLE47N_ORDER },
		{ "F", "Dark_Trooper_PhaseII", { "JET_PACK" }, S78_IDLE47N_FIRE },
		{ "V", nil, nil, S78_IDLE47N_SPAWN },
	}
	for i, p in ipairs(phases) do
		S78_Idle47VPhase(steps, p[1], p[2], t, p[3], p[4])
		t = t + (p[4].cycles + 1) * p[4].period
	end
	return { name = "IDLE47N", dur = t, steps = steps, tick = S78_QuietTick }
end

S78_SCN_BY_NAME = {
	SPMAT = S78_ScnSpmat, ATAT = S78_ScnAtat, MDU = S78_ScnMdu, STICKY = S78_ScnSticky, DT3 = S78_ScnDt3,
	REINF = S78_ScnReinf, MUZZLE_LAND = S78_ScnMuzzleLand, MAGNEPULSE = S78_ScnMagnepulse, BUNKER = S78_ScnBunker,
	IDLE47 = S78_ScnIdle47, MUZZLE_SPACE = S78_ScnMuzzleSpace, SQUAD = S78_ScnSquad, ESCORT = S78_ScnEscort,
	GUARD_SHIFT = S78_ScnGuardShift, BUNKER2 = S78_ScnBunker2, IDLE47T = S78_ScnIdle47T,
	SPMAT_EDGE = S78_ScnSpmatEdge, IDLE47V = S78_ScnIdle47V, GUARD_MOVE = S78_ScnGuardMove,
	SPMAT_EDGE2 = S78_ScnSpmatEdge2, SPMAT_CTRL = S78_ScnSpmatCtrl, IDLE47W = S78_ScnIdle47W, REINF2 = S78_ScnReinf2,
	GUARD_HAND = S78_ScnGuardHand, SPMAT_EDGE3 = S78_ScnSpmatEdge3, IDLE47X = S78_ScnIdle47X,
	SPMAT_EDGE4 = S78_ScnSpmatEdge4, IDLE47Q = S78_ScnIdle47Q, IDLE47N = S78_ScnIdle47N,
}

-- The scenario lists come from S78_CFG.land_list / space_list, which build_s7s8_mod.py always writes. A missing or
-- empty list, or an unknown name (reported on the first call only), is an ERR record (the run then finishes with no scenario), never a silent default.
function S78_Scenarios()
	local key = "land_list"
	if S78_MODE == "Space" then
		key = "space_list"
	end
	local names = S78_CFG[key]
	local out = {}
	if type(names) ~= "table" or names[1] == nil then
		S78_Rec("ERR", "scn=none|step=list|msg=S78_CFG." .. key .. "_missing_or_empty")
		return out
	end
	for i, n in ipairs(names) do
		local f = S78_SCN_BY_NAME[n]
		if f then
			table.insert(out, f())
		elseif S78_SCN_I == 0 then
			S78_Rec("ERR", "scn=none|step=list|msg=unknown_scenario_" .. S78_Clean(n))
		end
	end
	return out
end

-- ---- human track (land): the Saboteur for the Remote_Bomb ground-marker steps --------------------------------------

S78_HUMAN_DONE = false

function S78_HumanTrack()
	if S78_HUMAN_DONE or S78_MODE ~= "Land" then
		return
	end
	S78_HUMAN_DONE = true
	local hp = S78_Try("Find_Player:human", function() return Find_Player(S78_CFG.human_faction) end)
	local units = {}
	if hp then
		local ok, l = pcall(function() return Find_All_Objects_Of_Type(hp) end)
		if ok and type(l) == "table" then
			for i, o in ipairs(l) do
				if i <= 12 and S78_Valid(o) then
					table.insert(units, o)
				end
			end
		end
	end
	local cx, cy = S78_Centroid(units)
	local pos = nil
	local z = 0
	if units[1] then
		local _x, _y, zz = S78_XYZ(units[1])
		z = zz or 0
	end
	if cx then
		pos = S78_Try("Create_Position", function() return Create_Position(cx + 120, cy + 120, z) end)
	end
	local sab = nil
	if pos and hp then
		local t = S78_Try("Find_Object_Type:Underworld_Saboteur", function() return Find_Object_Type("Underworld_Saboteur") end)
		if t then
			local r = S78_Try("Spawn_Unit:Underworld_Saboteur", function() return Spawn_Unit(t, pos, hp) end)
			sab = r
			if type(r) == "table" then
				sab = r[1]
			end
		end
	end
	local x, y = nil, nil
	if S78_Valid(sab) then
		pcall(function() sab.Make_Invulnerable(true) end)
		pcall(function() Point_Camera_At(sab) end)
		x, y = S78_XYZ(sab)
		S78_HUMAN = sab
		S78_Msg("TEXT_S78_HUMAN_GO")
	end
	S78_Rec("HUMAN", "player=" .. (hp and "1" or "0") .. "|units=" .. table.getn(units) .. "|saboteur=" ..
		(S78_Valid(sab) and "1" or "0") .. "|id=" .. (sab and S78_ObjId(sab) or "?") .. "|x=" .. S78_Num(x, "%.1f") ..
		"|y=" .. S78_Num(y, "%.1f"))
end

S78_MARKERS_LAST = -1

function S78_HumanPoll()
	if S78_MODE ~= "Land" then
		return
	end
	local n = S78_Count(S78_OwnedOfType("Dummy_Barrage_Target", nil))
	local b = S78_Count(S78_OwnedOfType("Proj_Remote_Bomb", nil))
	local key = n * 1000 + b
	if key ~= S78_MARKERS_LAST then
		S78_MARKERS_LAST = key
		S78_Rec("COUNT", "scn=HUMAN|what=markers_bombs|markers=" .. n .. "|bombs=" .. b)
	end
end

-- ---- driver --------------------------------------------------------------------------------------------------------

function S78_StartScenario(def, F)
	def.f0 = F
	def.objs = {}
	def.order = {}
	def.spawned = {}
	def.blocks = {}
	def.marks = {}
	def.done = {}
	def.last_poll = nil
	S78_CUR = def
	local m = S78_Pts()
	S78_Rec("SCN", "scn=" .. def.name .. "|ev=start|i=" .. S78_SCN_I .. "|dur=" .. def.dur .. "|mode=" .. S78_MODE ..
		"|points_ok=" .. tostring(m.points_ok))
end

function S78_EndScenario(why)
	local s = S78_CUR
	pcall(S78_Poll)
	local pend = ""
	for name, b in pairs(s.blocks) do
		if not b.done then
			pend = pend .. name .. ","
		end
	end
	local gone = 0
	for i, obj in ipairs(s.spawned) do
		if S78_Valid(obj) then
			if pcall(function() obj.Despawn() end) then
				gone = gone + 1
			end
		end
	end
	S78_Rec("SCN", "scn=" .. s.name .. "|ev=end|why=" .. why .. "|despawned=" .. gone .. "|pending_blocks=" .. pend)
	S78_CUR = nil
end

function S78_Step(F)
	local s = S78_CUR
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
					S78_Rec("ERR", "scn=" .. s.name .. "|step=" .. i .. "|msg=" .. string.sub(S78_Clean(err), 1, 200))
				end
			end
		end
	end
	if s.tick then
		local ok, err = pcall(s.tick, s, rel)
		if not ok then
			S78_Rec("ERR", "scn=" .. s.name .. "|step=tick|msg=" .. string.sub(S78_Clean(err), 1, 200))
		end
	end
	pcall(S78_Poll)
	if rel >= s.dur then
		S78_EndScenario("time")
	end
end

function S78_Finish()
	S78_DONE = true
	S78_ON = false
	ServiceRate = 20
	local bad = ""
	for k, v in pairs(S78_API_BAD) do
		bad = bad .. k .. ","
	end
	if S78_MODE == "Space" then
		S78_Msg("TEXT_S78_SPACE_DONE")
	else
		S78_Msg("TEXT_S78_LAND_DONE")
	end
	S78_Rec("DONE", "mode=" .. S78_MODE .. "|scenarios=" .. S78_SCN_I .. "|apierr=" .. string.sub(S78_Clean(bad), 1, 250))
end

function S78_Tick(src)
	if not S78_ON then
		return
	end
	local F = S78_Frame()
	if F == nil or F == S78_LAST_F then
		return
	end
	S78_LAST_F = F
	S78_TICKS = S78_TICKS + 1
	if S78_TICKS <= 12 then
		S78_Rec("TICK", "n=" .. S78_TICKS .. "|src=" .. src)
	elseif S78_TICKS >= S78_HB_NEXT then
		S78_HB_NEXT = S78_TICKS + 100
		S78_Rec("HB", "n=" .. S78_TICKS .. "|src=" .. src .. "|scn=" .. S78_ScnName() .. "|records=" .. table.getn(S78_LOG))
	end
	if F < S78_CFG.start_frame then
		return
	end
	pcall(S78_HumanTrack)
	pcall(S78_HumanPoll)
	if S78_DONE then
		return
	end
	if S78_CUR == nil then
		if S78_VICTIM_PLAYER == nil then
			S78_FindVictimPlayer()
		end
		local list = S78_Scenarios()
		S78_SCN_I = S78_SCN_I + 1
		local def = list[S78_SCN_I]
		if def == nil then
			S78_Finish()
			-- the land human track keeps polling the marker count after DONE (S78_ON stays true there)
			if S78_MODE == "Land" then
				S78_ON = true
			end
			return
		end
		S78_StartScenario(def, F)
	end
	S78_Step(F)
end

function S78_FindVictimPlayer()
	-- the human's faction first. Either faction works: the s78f "AI never targets Hostile-owned victims" note was
	-- wrong (s78g t04, TACTICAL_COMBAT sec. 2.10: Pod_Walkers did target Hostile T4Bs; the IDLE47 parser missed the
	-- sparse state samples and the SPMAT's DEPLOY call changed its state). Kept as is for continuity with t12.
	local names = { S78_CFG.human_faction or "Empire", "Hostile", "Underworld", "Pirates" }
	for i, n in ipairs(names) do
		if string.upper(n) ~= S78_CFG.faction then
			local p = S78_Try("Find_Player:" .. n, function() return Find_Player(n) end)
			if p then
				S78_VICTIM_PLAYER = p
				S78_VICTIM_FAC = n
				break
			end
		end
	end
	local id = "?"
	if S78_VICTIM_PLAYER then
		local ok, v = pcall(function() return S78_VICTIM_PLAYER.Get_ID() end)
		if ok then
			id = tostring(v)
		end
	end
	S78_Rec("VICTIMS", "faction=" .. S78_VICTIM_FAC .. "|id=" .. id)
	for i, n in ipairs({ "Hostile", "Empire", "Rebel", "Underworld", "Neutral" }) do
		local q = S78_Try("Find_Player:" .. n, function() return Find_Player(n) end)
		if q then
			local a = S78_Try("Is_Enemy", function() return PlayerObject.Is_Enemy(q) end)
			local b = S78_Try("Is_Enemy", function() return q.Is_Enemy(PlayerObject) end)
			local qid = S78_Try("Get_ID", function() return q.Get_ID() end)
			S78_Rec("REL", "other=" .. n .. "|other_id=" .. S78_Clean(qid) .. "|ai_enemy_of_other=" .. S78_Clean(a) ..
				"|other_enemy_of_ai=" .. S78_Clean(b))
		end
	end
end

S78_ARM_PENDING = false
S78_ARM_TRIES = 0

function S78_ArmRetry()
	if S78_ARM_PENDING then
		S78_ARM_TRIES = S78_ARM_TRIES + 1
		pcall(S78_Arm)
	end
end

function S78_Arm()
	local fac = "?"
	local mode = "?"
	local pid = "?"
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() pid = tostring(PlayerObject.Get_ID()) end)
	S78_ON = false
	-- Base_Definitions runs at script load, before the engine sets PlayerObject (retail runs 2026-11-04 / 2026-11-05
	-- recorded SKIP pid=? fac=?). Undecided until the faction is readable: retried from the first service calls.
	if fac == "?" and S78_ARM_TRIES < 200 then
		S78_ARM_PENDING = true
		return
	end
	S78_ARM_PENDING = false
	if fac == S78_CFG.faction and (mode == "Land" or mode == "Space") then
		S78_RUN = S78_RUN + 1
		S78_ON = true
		S78_DONE = false
		S78_MODE = mode
		S78_LAST_F = nil
		S78_TICKS = 0
		S78_HB_NEXT = 100
		S78_SCN_I = 0
		S78_CUR = nil
		S78_API_BAD = {}
		S78_VICTIM_PLAYER = nil
		S78_HUMAN_DONE = false
		S78_MARKERS_LAST = -1
		local old = tostring(ServiceRate)
		ServiceRate = 0.05
		local m = S78_Pts()
		S78_Rec("ARM", "pid=" .. pid .. "|fac=" .. fac .. "|mode=" .. mode .. "|map=" .. m.map .. "|points_ok=" ..
			tostring(m.points_ok) .. "|service_rate=" .. old .. "->0.05|spawn_unit=" .. type(Spawn_Unit) ..
			"|create_position=" .. type(Create_Position) .. "|text=" .. tostring(S78_CFG.text))
		if mode == "Space" then
			S78_Msg("TEXT_S78_ARMED_SPACE")
		else
			S78_Msg("TEXT_S78_ARMED_LAND")
		end
	else
		S78_Rec("SKIP", "pid=" .. pid .. "|fac=" .. fac .. "|mode=" .. mode)
	end
end

S78_SID = S78_Clean(string.gsub(tostring(S78_LOG), "^table: ", ""))
S78_SID = string.sub(S78_SID, -6)

S78_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	local r = S78_Orig_Base_Definitions()
	pcall(S78_Arm)
	return r
end

S78_EVERR = 0

function PumpEvents()
	if Object and type(Object) == "userdata" then
		pcall(function() Object.Service_Wrapper() end)
	end
	if Pump_Service and type(Pump_Service) == "function" then
		pcall(Pump_Service)
	end
	YieldCount = (YieldCount or 0) + 1
	coroutine.yield(true)
	if type(GetEvent) ~= "function" then
		return
	end
	CurrentEvent = GetEvent()
	while CurrentEvent do
		EventParams = GetEvent.Params()
		local ok, err
		if EventParams then
			ok, err = pcall(CurrentEvent, unpack(EventParams))
		else
			ok, err = pcall(CurrentEvent)
		end
		if not ok then
			S78_EVERR = S78_EVERR + 1
			if S78_EVERR <= 50 then
				S78_Rec("EVERR", "n=" .. S78_EVERR .. "|msg=" .. string.sub(S78_Clean(err), 1, 200))
			end
		end
		CurrentEvent = GetEvent()
	end
end

S78_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
	S78_ArmRetry()
	if S78_ON then
		pcall(S78_Tick, "fss")
		if not S78_DONE then
			return
		end
	end
	return S78_Orig_FreeStoreService()
end

S78_Orig_On_Unit_Service = On_Unit_Service
function On_Unit_Service(object)
	S78_ArmRetry()
	if S78_ON then
		if S78_ANCHOR == nil and S78_Valid(object) then
			S78_ANCHOR = object
		end
		pcall(S78_Tick, "ous")
		if not S78_DONE then
			return
		end
	end
	return S78_Orig_On_Unit_Service(object)
end

S78_Orig_On_Unit_Added = On_Unit_Added
function On_Unit_Added(object)
	if S78_ANCHOR == nil and S78_Valid(object) then
		S78_ANCHOR = object
	end
	return S78_Orig_On_Unit_Added(object)
end

S78_Rec("LOAD", "land=" .. S78_CFG.land.map .. "|space=" .. S78_CFG.space.map .. "|faction=" .. S78_CFG.faction ..
	"|land_ok=" .. tostring(S78_CFG.land.points_ok) .. "|space_ok=" .. tostring(S78_CFG.space.points_ok))
-- ==== end of the S7/S8 probe ====
