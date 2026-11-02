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
-- Lua 5.0: no '#', table.getn, math.mod.

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

if S78_CFG == nil then
	S78_CFG = { faction = "REBEL", human_faction = "EMPIRE", start_frame = 450, text = false,
		land = { map = "?", points_ok = false, pts = {} }, space = { map = "?", points_ok = false, pts = {} } }
end

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
			end },
			{ at = 45, run = function(s) S78_Act("spmat", "deploy1", "DEPLOY", "bool", true) end },
			{ at = 585, run = function(s)
				S78_Despawn("v275")
				local w, how = S78_At("site", 547, 0)
				S78_Victim("v547", S78_Spawn("v547", "T4B_Tank", w, how, S78_VICTIM_PLAYER), false)
				S78_Mark("edge")
			end },
			{ at = 1185, run = function(s)
				local w, how = S78_At("site", 530, 0)
				S78_Victim("v530", S78_Spawn("v530", "T4B_Tank", w, how, S78_VICTIM_PLAYER), false)
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
					S78_Kill("carrier")
					if S78_Valid(s.objs.carrier) then
						S78_Despawn("carrier")
					end
					S78_Mark("carrier_dead", "x=" .. S78_Num(x) .. "|y=" .. S78_Num(y) .. "|escorts=" ..
						S78_Count(S78_Escorts()))
				end },
			{ at = 90, when = function(s) return s.marks.carrier_dead and S78_Frame() >= s.marks.carrier_dead + 1200 end,
				run = function(s)
					local cx, cy = S78_Centroid(S78_Escorts())
					local pos = nil
					if cx then
						pos = S78_Try("Create_Position", function()
							local p = S78_Pts().pts.esc
							local ux, uy = (p and p.ux) or 1.0, (p and p.uy) or 0.0
							return Create_Position(cx + 700 * ux, cy + 700 * uy, 0)
						end)
					end
					S78_Rec("MARK", "scn=" .. s.name .. "|mark=guard_point|x=" .. S78_Num(cx) .. "|y=" .. S78_Num(cy))
					S78_Victim("intruder", S78_Spawn("intruder", "Tartan_Patrol_Cruiser", pos, cx and "centroid" or "none",
						S78_VICTIM_PLAYER), false)
					S78_Mark("intruder")
				end },
			{ at = 120, when = function(s) return s.marks.intruder and S78_Frame() >= s.marks.intruder + 900 end,
				run = function(s)
					S78_Despawn("intruder")
					S78_Mark("intruder_gone")
				end },
		},
	}
end

function S78_Scenarios()
	if S78_MODE == "Space" then
		return { S78_ScnMuzzleSpace(), S78_ScnSquad(), S78_ScnEscort() }
	end
	return { S78_ScnSpmat(), S78_ScnAtat(), S78_ScnMdu(), S78_ScnSticky(), S78_ScnDt3(), S78_ScnReinf(),
		S78_ScnMuzzleLand() }
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
	local names = { "Hostile", "Empire", "Underworld", "Pirates" }
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
end

function S78_Arm()
	local fac = "?"
	local mode = "?"
	local pid = "?"
	pcall(function() fac = string.upper(tostring(PlayerObject.Get_Faction_Name())) end)
	pcall(function() mode = tostring(Get_Game_Mode()) end)
	pcall(function() pid = tostring(PlayerObject.Get_ID()) end)
	S78_ON = false
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

S78_Orig_FreeStoreService = FreeStoreService
function FreeStoreService()
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
