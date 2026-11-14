-- ==== cu8g t09b: Cancel_Special_Ability restore probe (tools/probe_mod/cu8g_probes/cu8g_cancel.lua) ====
-- Question (docs/re/CU8F_T01_T02_T07_RE.md sec. 1.2 / 1.4): Cancel_Special_Ability's apply(user, target) sets the
-- TARGET's SAP +0x28 (every special-ability entry point returns early, unit abilities fail gate 5); its remove (user
-- death / removal, mode exit [M]) clears it first, then re-staggers every interval ability with one synced draw
-- Get_Int(now, now + interval) and re-applies the automatic effects. Vanilla has no user: the probe mod un-comments
-- Akbar_Cancel_Special_Ability (Combat_Automatic) on Home_One. Which events fire a Combat_Automatic apply is [L].
--
-- Space skirmish, AI = C8_CFG.ai_faction (Rebel) runs it; the idle human (Empire) owns the target. Per trial at the
-- site C8_CFG.cancel.site:
--   T = human Accuser_Star_Destroyer (TRACTOR_BEAM unit ability, Tractor_Beam_Attack_Ability = interval-1 SAP class,
--       two Combat_Bonus auras) at (0, 0); W = AI Corellian_Corvette at (victim_dx, 0); both invulnerable,
--       W Prevent_All_Fire.
--   base     (+60):  T.Activate_Ability("TRACTOR_BEAM", W); the pull is the T-W distance shrinking every frame.
--   cancel   (+base_len): H = AI Home_One (the mod copy, with the Cancel ability) spawns at (user_dx, 0), invulnerable.
--            Every 30 frames T.Activate_Ability("TRACTOR_BEAM", W) is tried (its 20 s recharge ends inside this phase).
--   kill     (+cancel_len): H is made vulnerable and given Take_Damage until it is dead (cu8g t10: no Despawn, the
--            first run crashed there).
--   restore  (frame R = the first poll with H dead): Every poll T.Activate_Ability is tried until T reports the
--            ability active or W reports Is_Under_Effects_Of_Ability("TRACTOR_BEAM") (SCRIPT_GAPS sec. 2.1: one of the
--            two names that call supports).
-- Records: CTRIAL, CPH (phase start), CACT (activation attempts), CP (every poll: T-W distance, W tractor flag, T's
-- TRACTOR_BEAM / ENERGY_WEAPON ready + active flags, H valid), CEND. The answer is the frame of the first restored
-- flag / pull after R; the spec predicts the manager is enabled on the removal frame and interval services resume
-- at R or R + 1.

C8C = { i = 0, cur = nil, next_f = nil }

function C8C_Rel(dx, dy)
	local s = C8_CFG.cancel.site
	return C8_Pos(s.x + dx, s.y + dy, s.z)
end

function C8C_Start(F)
	local cfg = C8_CFG.cancel
	C8C.i = C8C.i + 1
	if C8C.i > cfg.trials then
		C8_Say("TEXT_CU8G_CANCEL_DONE")
		C8_Finish("trials=" .. (C8C.i - 1))
		return
	end
	local human = C8_Player(cfg.human_faction)
	local t = { k = C8C.i, f0 = F, phase = "setup", pf = F }
	C8C.cur = t
	local how
	t.T, how = C8_Spawn(cfg.target, C8C_Rel(0, 0), human)
	t.W = C8_Spawn(cfg.victim, C8C_Rel(cfg.victim_dx, 0), PlayerObject)
	C8_Call(t.T, "Make_Invulnerable", function() t.T.Make_Invulnerable(true) end)
	C8_Call(t.W, "Make_Invulnerable", function() t.W.Make_Invulnerable(true) end)
	C8_Call(t.W, "Prevent_All_Fire", function() t.W.Prevent_All_Fire(true) end)
	C8_Call(t.W, "Prevent_AI_Usage", function() t.W.Prevent_AI_Usage(true) end)
	C8_Rec("CTRIAL", "k=" .. t.k .. "|T=" .. C8_Type(t.T) .. ":" .. C8_Id(t.T) .. ":" .. tostring(how) .. "@" ..
		C8_XY(t.T) .. "|W=" .. C8_Type(t.W) .. ":" .. C8_Id(t.W) .. "@" .. C8_XY(t.W))
	if not C8_Valid(t.T) or not C8_Valid(t.W) then
		C8_Rec("ERR", "what=spawn_failed|k=" .. t.k)
		t.phase = "end"
	end
end

function C8C_Phase(t, F, ph)
	t.phase = ph
	t.pf = F
	C8_Rec("CPH", "k=" .. t.k .. "|ph=" .. ph)
end

function C8C_Act(t, F, why)
	local ab = C8_CFG.cancel.ability
	local ready = C8_Flag(t.T, "Is_Ability_Ready", ab)
	local ok, err = pcall(function() return t.T.Activate_Ability(ab, t.W) end)
	if not ok then
		C8_ApiFail("Activate_Ability:" .. ab, err)
	end
	local active = C8_Flag(t.T, "Is_Ability_Active", ab)
	if why ~= "restore" or active == "1" or t.act_logged == nil then
		t.act_logged = true
		C8_Rec("CACT", "k=" .. t.k .. "|ph=" .. t.phase .. "|why=" .. why .. "|call=" .. (ok and "ok" or "error") ..
			"|ready_before=" .. ready .. "|active_after=" .. active)
	end
	return active
end

function C8C_Poll(t, F)
	local cfg = C8_CFG.cancel
	local h = "-"
	if t.H then
		h = "0"
		if C8_Valid(t.H) then
			h = "1"
		end
	end
	local tb = C8_Flag(t.T, "Is_Ability_Ready", cfg.ability) .. C8_Flag(t.T, "Is_Ability_Active", cfg.ability)
	local ew = C8_Flag(t.T, "Is_Ability_Ready", cfg.ability2) .. C8_Flag(t.T, "Is_Ability_Active", cfg.ability2)
	local wf = C8_Flag(t.W, "Is_Under_Effects_Of_Ability", cfg.ability)
	t.wflag = wf
	t.tb = tb
	C8_Rec("CP", "k=" .. t.k .. "|ph=" .. t.phase .. "|d=" .. C8_Num(C8_Dist(t.T, t.W), "%.1f") .. "|wtb=" .. wf ..
		"|tb=" .. tb .. "|ew=" .. ew .. "|h=" .. h)
end

function C8C_Tick(F)
	local cfg = C8_CFG.cancel
	local t = C8C.cur
	if t == nil then
		if C8C.next_f == nil then
			C8C.next_f = cfg.start_frame
			C8_Say("TEXT_CU8G_ABIL_WAIT")
		end
		if F >= C8C.next_f then
			C8C_Start(F)
		end
		return
	end
	if t.phase == "gap" then
		if F - t.fend >= cfg.gap then
			C8C.cur = nil
			C8C.next_f = F
		end
		return
	end
	if t.phase == "end" then
		C8_Despawn(t.T)
		C8_Despawn(t.W)
		t.phase = "gap"
		t.fend = F
		C8_Rec("CEND", "k=" .. t.k)
		return
	end
	C8C_Poll(t, F)
	local rel = F - t.pf
	if t.phase == "setup" then
		if rel >= 60 then
			C8C_Phase(t, F, "base")
			C8C_Act(t, F, "base")
		end
	elseif t.phase == "base" then
		if rel >= cfg.base_len then
			-- cu8g t10: end the beam before the user arrives. Run 2 (2026-11-08 07:35) crashed at +0x7107c8 (inside the
			-- tractor-beam Update 0x710710) when H died with the beam still active on W, like run 1 at the despawn.
			-- W is replaced by a fresh corvette W2 at the same spot, so the beam is off when H arrives and dies.
			C8_Despawn(t.W)
			t.W = C8_Spawn(cfg.victim, C8C_Rel(cfg.victim_dx, 0), PlayerObject)
			C8_Call(t.W, "Make_Invulnerable", function() t.W.Make_Invulnerable(true) end)
			C8_Call(t.W, "Prevent_All_Fire", function() t.W.Prevent_All_Fire(true) end)
			C8_Call(t.W, "Prevent_AI_Usage", function() t.W.Prevent_AI_Usage(true) end)
			C8_Rec("CSWAP", "k=" .. t.k .. "|W2=" .. C8_Type(t.W) .. ":" .. C8_Id(t.W) .. "|tb=" ..
				C8_Flag(t.T, "Is_Ability_Ready", cfg.ability) .. C8_Flag(t.T, "Is_Ability_Active", cfg.ability))
			C8C_Phase(t, F, "cancel")
			local how
			t.H, how = C8_Spawn(cfg.user, C8C_Rel(cfg.user_dx, 0), PlayerObject)
			C8_Call(t.H, "Make_Invulnerable", function() t.H.Make_Invulnerable(true) end)
			C8_Call(t.H, "Prevent_AI_Usage", function() t.H.Prevent_AI_Usage(true) end)
			-- cu8g t10: Combat_Automatic needs H in combat (2026-11-08: an idle H blocked nothing), so H attacks T.
			C8_Call(t.H, "Attack_Target", function() t.H.Attack_Target(t.T) end)
			C8_Rec("CUSER", "k=" .. t.k .. "|type=" .. C8_Type(t.H) .. "|id=" .. C8_Id(t.H) .. "|how=" .. tostring(how) ..
				"|xy=" .. C8_XY(t.H) .. "|dTH=" .. C8_Num(C8_Dist(t.T, t.H), "%.0f"))
		end
	elseif t.phase == "cancel" then
		if t.last_try == nil or F - t.last_try >= 30 then
			t.last_try = F
			C8C_Act(t, F, "cancel")
			C8_Call(t.H, "Attack_Target", function() t.H.Attack_Target(t.T) end)
		end
		if rel >= cfg.cancel_len then
			-- cu8g t10: kill H instead of Despawn (the 2026-11-08 run crashed at the Home One despawn, c0000005).
			C8C_Phase(t, F, "kill")
			C8_Call(t.H, "Make_Invulnerable", function() t.H.Make_Invulnerable(false) end)
		end
	elseif t.phase == "kill" then
		if C8_Valid(t.H) then
			local r = C8_Call(t.H, "Take_Damage", function() t.H.Take_Damage(10000000) end)
			if t.kill_n == nil then
				t.kill_n = 0
				C8_Rec("CKILL", "k=" .. t.k .. "|res=" .. r)
			end
			t.kill_n = t.kill_n + 1
			if rel >= 300 then
				C8_Rec("ERR", "what=user_not_killed|k=" .. t.k)
				t.phase = "end"
			end
		else
			C8C_Phase(t, F, "restore")
			t.fr = F
		end
	elseif t.phase == "restore" then
		local a = C8C_Act(t, F, "restore")
		if (a == "1" or t.wflag == "1") and t.frestored == nil then
			t.frestored = F
			C8_Rec("CRESTORED", "k=" .. t.k .. "|after=" .. (F - t.fr) .. "|active=" .. a .. "|wtb=" .. t.wflag)
		end
		if (t.frestored and F - t.frestored >= 150) or rel >= cfg.restore_len then
			t.phase = "end"
		end
	end
end

C8_PROBES["Space"] = { name = "cancel", tick = C8C_Tick, fast = true, quiet = true,
	human = C8_CFG.cancel and C8_CFG.cancel.human_faction }
-- ==== end of cu8g_cancel.lua ====
