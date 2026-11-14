-- ==== cu8k t05: jetpack formation draw probe (tools/probe_mod/cu8h_probes/cu8k_jetpack.lua) ====
-- Question (docs/re/MOVEMENT.md sec. 4.7.4 / 4.7.6 item 4): the JET_PACK swap to the secondary locomotor makes a new
-- coordinator; its formation, and so one FormationClass ctor draw int(1, FPS), is expected at the next coordinator
-- update (land composition 0x43e5b0, return 0x43e712) in the same frame, after that frame's object-service draws. The
-- swap back makes no draw. The synced draws are traced by cu8h_dbg.py (a debugger) that cu8h_run.py arms when it finds
-- the DBGARM record below and stops at DBGSTOP; this script only stages the trials and records what it ordered.
--
-- Land skirmish, AI = C8_CFG.ai_faction (Rebel) runs it; the idle human (Empire) owns the units. At C8_CFG.jet.site:
--   A, B, C = Dark_Trooper_PhaseII for the human player at (0, 0), (0, dy), (0, 2 dy), invulnerable (start_frame).
--   DBGARM `arm_after` frames later; `arm_lead` frames after DBGARM:
--   trial A: A.Move_To(site + (move_dx, 0)); `jet_delay` frames later A.Activate_Ability(JET_PACK, <marker TA>)
--            while moving (sec. 4.7.4: the swap re-issues the saved move);
--   trial B: `trial_gap` frames after A's activation: B.Activate_Ability(JET_PACK, <marker TB>) standing;
--   trial C: `trial_gap` frames later C.Move_To(site + (move_dx, 2 dy)); `jet_delay` frames later
--            C.Activate_Ability(JET_PACK, <marker TC>);
--   DBGSTOP `after` frames after C's activation; the end `end_after` frames later.
-- JET_PACK targets are game objects, as in the shipped scripts (Activate_Ability("JET_PACK", <Find_Hint marker>);
-- STORY_UNDERWORLD_ACTI_M02_SPACE.LUA spawns its STORY_TRIGGER_ZONE marker with Create_Generic_Object): TA at
-- site + (move_dx, 0), TB at site + (jump_dx, dy), TC at site + (jump_dx, 2 dy), spawned with the units (marker type
-- C8_CFG.jet.target, fallback target_fallback). A trial whose marker is missing is skipped, never given a position:
-- an engine error in Activate_Ability ends the whole probe script (pcall does not catch it). Run 1 of 2026-11-09
-- died at A.Activate_Ability(JET_PACK, true); the rerun (06:29) at A's (or B's) Activate_Ability with a
-- Create_Position target (records stop after f 573, C never walked). Each activation's 'before' record carries
-- Has_Ability / Is_Ability_Ready / Is_Ability_Active, so a further death is placed by the last record.
-- Records: JSPAWN, JTARGET, JORDER (what, frame, result; flags before), JST (every `poll_every` frames: Is_Ability_Active / position of each
-- unit), DBGARM, DBGSTOP, JPHASE, JEND.

C8J = { phase = "wait", pf = nil, units = {}, targets = {} }

function C8J_Rel(dx, dy)
	local s = C8_CFG.jet.site
	return C8_Pos(s.x + dx, s.y + dy, s.z)
end

function C8J_Phase(name, F)
	C8J.phase = name
	C8J.pf = F
	C8_Rec("JPHASE", "phase=" .. name)
end

function C8J_Order(who, what, f)
	local o = C8J.units[who]
	local ab = C8_CFG.jet.ability
	C8_Rec("JORDER", "who=" .. who .. "|what=" .. what .. "|step=before|has=" .. C8_Flag(o, "Has_Ability", ab) ..
		"|ready=" .. C8_Flag(o, "Is_Ability_Ready", ab) .. "|active=" .. C8_Flag(o, "Is_Ability_Active", ab))
	local res = C8_Call(o, who .. "." .. what, f)
	C8_Rec("JORDER", "who=" .. who .. "|what=" .. what .. "|res=" .. res .. "|at=" .. (C8_Valid(o) and C8_XY(o) or "-"))
	return res
end

-- A marker object at site + (dx, dy) for player: the JET_PACK target of one trial.
function C8J_Target(tag, dx, dy, player)
	local cfg = C8_CFG.jet
	local pos = C8J_Rel(dx, dy)
	local o, how = nil, "no_type"
	for _, tname in ipairs({ cfg.target, cfg.target_fallback }) do
		local ty = nil
		pcall(function() ty = Find_Object_Type(tname) end)
		if ty ~= nil and pos ~= nil then
			local ok, r = pcall(Create_Generic_Object, ty, pos, player)
			if ok and C8_Valid(r) then
				o, how = r, tname
				break
			end
			how = "spawn_failed"
		end
	end
	C8J.targets[tag] = o
	C8_Rec("JTARGET", "tag=" .. tag .. "|how=" .. C8_Str(how) .. "|id=" .. (o and C8_Id(o) or "-") .. "|at=" ..
		(o and C8_XY(o) or "-"))
end

-- who.Activate_Ability(JET_PACK, <marker tag>), skipped when the marker is missing.
function C8J_Jet(who, tag)
	local tg = C8J.targets[tag]
	if not C8_Valid(tg) then
		C8_Rec("JORDER", "who=" .. who .. "|what=Activate_Ability|res=no_target|tag=" .. tag)
		return
	end
	C8J_Order(who, "Activate_Ability", function() C8J.units[who].Activate_Ability(C8_CFG.jet.ability, tg) end)
end

function C8J_Poll()
	local s = ""
	for _, who in ipairs({ "A", "B", "C" }) do
		local o = C8J.units[who]
		if C8_Valid(o) then
			s = s .. who .. "=" .. C8_Flag(o, "Is_Ability_Active", C8_CFG.jet.ability) .. ":" .. C8_XY(o) .. ";"
		end
	end
	if s ~= "" then
		C8_Rec("JST", s)
	end
end

function C8J_Tick(F)
	local cfg = C8_CFG.jet
	if C8J.f0 == nil then
		C8J.f0 = F
		C8_Say("TEXT_CU8K_JET_WAIT")
	end
	local rel = F - (C8J.pf or F)
	if C8J.phase ~= "wait" and C8J.phase ~= "end" and (C8J.lastpoll == nil or F - C8J.lastpoll >= cfg.poll_every) then
		C8J.lastpoll = F
		C8J_Poll()
	end
	if C8J.phase == "wait" then
		if F >= cfg.start_frame then
			local player = C8_Player(cfg.human_faction)
			for i, who in ipairs({ "A", "B", "C" }) do
				local o, how = C8_Spawn(cfg.unit, C8J_Rel(0, (i - 1) * cfg.spacing_y), player, true)
				if o then
					C8_Call(o, "Make_Invulnerable", function() o.Make_Invulnerable(true) end)
				end
				C8J.units[who] = o
				C8_Rec("JSPAWN", "who=" .. who .. "|type=" .. C8_Str(cfg.unit) .. "|how=" .. C8_Str(how) .. "|id=" ..
					(o and C8_Id(o) or "-") .. "|at=" .. (o and C8_XY(o) or "-"))
			end
			C8J_Target("TA", cfg.move_dx, 0, player)
			C8J_Target("TB", cfg.jump_dx, cfg.spacing_y, player)
			C8J_Target("TC", cfg.jump_dx, 2 * cfg.spacing_y, player)
			if not C8_Valid(C8J.units.A) then
				C8_Rec("ERR", "what=jet_spawn_failed")
				C8_Finish("jet=spawn_failed")
				return
			end
			C8J_Phase("spawned", F)
		end
	elseif C8J.phase == "spawned" then
		if rel >= cfg.arm_after then
			C8_Rec("DBGARM", "lead=" .. cfg.arm_lead)
			C8_Say("TEXT_CU8K_JET_ARM")
			C8J_Phase("lead", F)
		end
	elseif C8J.phase == "lead" then
		if rel >= cfg.arm_lead then
			C8J_Order("A", "Move_To", function() C8J.units.A.Move_To(C8J_Rel(cfg.move_dx, 0)) end)
			C8J_Phase("a_moving", F)
		end
	elseif C8J.phase == "a_moving" then
		if rel >= cfg.jet_delay then
			C8J_Jet("A", "TA")
			C8J_Phase("a_jet", F)
		end
	elseif C8J.phase == "a_jet" then
		if rel >= cfg.trial_gap then
			C8J_Jet("B", "TB")
			C8J_Phase("b_jet", F)
		end
	elseif C8J.phase == "b_jet" then
		if rel >= cfg.trial_gap then
			C8J_Order("C", "Move_To", function() C8J.units.C.Move_To(C8J_Rel(cfg.move_dx, 2 * cfg.spacing_y)) end)
			C8J_Phase("c_moving", F)
		end
	elseif C8J.phase == "c_moving" then
		if rel >= cfg.jet_delay then
			C8J_Jet("C", "TC")
			C8J_Phase("c_jet", F)
		end
	elseif C8J.phase == "c_jet" then
		if rel >= cfg.after then
			C8_Rec("DBGSTOP", "f=" .. F)
			C8J_Phase("end", F)
		end
	elseif C8J.phase == "end" then
		if rel >= cfg.end_after then
			C8_Say("TEXT_CU8K_JET_DONE")
			C8_Rec("JEND", "f=" .. F)
			C8_Finish("jet=done")
		end
	end
end

C8_PROBES["Land"] = { name = "jetpack", tick = C8J_Tick, fast = true, quiet = true,
	human = C8_CFG.jet and C8_CFG.jet.human_faction }
-- ==== end of cu8k_jetpack.lua ====
