-- ==== cu8g t09a: LURE Is_Pulsing probe (tools/probe_mod/cu8g_probes/cu8g_lure.lua) ====
-- Question (docs/re/CU8F_T04_T06_RE.md sec. 3.1): `Is_Pulsing` (+0x2c) has no reader [M], so LURE always pulses:
-- every int(FPS x Pulse_Frequency_Secs) = 15 frames it orders each enemy of an exact Target_Types type within
-- Effective_Radius (350, 3D) whose attack target is not the lure owner to attack the owner. The mod copy sets
-- Is_Pulsing = No on Canderous_Assault_Tank only; Canderous_Assault_Tank_Captured keeps the shipped Yes (control).
--
-- Land skirmish, AI = C8_CFG.ai_faction (Rebel) runs it, the idle human (Empire) owns the victims. Per trial at the
-- site C8_CFG.lure.site (a dry, open square of the map, chosen by build_cu8g_mod.py):
--   L = AI lure tank (the trial's type) at (0, 0); D = AI decoy (T2B_Tank) at (decoy_dx, 0); victims = 3 human
--   AT_ST_Walker at (victim_dx, -60 / 0 / +60). Everything invulnerable, AI units Prevent_AI_Usage.
--   At +60 frames the victims are ordered Attack_Target(D); at +90 L.Activate_Ability("LURE") (argument kinds tried
--   in order: none, bool, object). Then every divert_every frames the victims are ordered Attack_Target(D) again.
-- Pulsing => after each divert a victim's target returns to L within one pulse (<= 15 frames + the 3-frame poll)
-- until the ability expires (6 s) or the pulse cap; one pulse only => it stays on D after the first divert.
-- Records: LTRIAL, LACT, DIVERT, LP (every poll: each victim's attack target role L / D / other / none; L's ability
-- flags), LEND.

C8L = { i = 0, cur = nil, next_f = nil }

function C8L_Rel(dx, dy)
	local s = C8_CFG.lure.site
	return C8_Pos(s.x + dx, s.y + dy, s.z)
end

function C8L_Start(F)
	local cfg = C8_CFG.lure
	C8L.i = C8L.i + 1
	local ty = cfg.trials[C8L.i]
	if ty == nil then
		C8_Say("TEXT_CU8G_LURE_DONE")
		C8_Finish("trials=" .. (C8L.i - 1))
		return
	end
	local human = C8_Player(cfg.human_faction)
	local t = { k = C8L.i, lure_type = ty, f0 = F, phase = "setup", victims = {}, ids = {}, types = {}, act_n = 0 }
	C8L.cur = t
	local how
	t.L, how = C8_Spawn(ty, C8L_Rel(0, 0), PlayerObject)
	t.D = C8_Spawn(cfg.decoy, C8L_Rel(cfg.decoy_dx, 0), PlayerObject)
	for _, o in ipairs({ t.L, t.D }) do
		C8_Call(o, "Make_Invulnerable", function() o.Make_Invulnerable(true) end)
		C8_Call(o, "Prevent_AI_Usage", function() o.Prevent_AI_Usage(true) end)
	end
	C8_Call(t.D, "Prevent_All_Fire", function() t.D.Prevent_All_Fire(true) end)
	local vs = ""
	for i = 1, 3 do
		local v = C8_Spawn(cfg.victim, C8L_Rel(cfg.victim_dx, (i - 2) * 60), human)
		if v then
			C8_Call(v, "Make_Invulnerable", function() v.Make_Invulnerable(true) end)
			table.insert(t.victims, v)
			vs = vs .. C8_Id(v) .. "@" .. C8_XY(v) .. ";"
		end
	end
	t.ids = { L = C8_Id(t.L), D = C8_Id(t.D) }
	t.types = { L = C8_Type(t.L), D = C8_Type(t.D) }
	C8_Rec("LTRIAL", "k=" .. t.k .. "|lure=" .. ty .. "|is_pulsing=" .. tostring(cfg.pulsing[ty]) .. "|L=" ..
		t.types.L .. ":" .. t.ids.L .. ":" .. tostring(how) .. "@" .. C8_XY(t.L) .. "|D=" .. t.types.D .. ":" ..
		t.ids.D .. "@" .. C8_XY(t.D) .. "|victims=" .. vs)
	if not C8_Valid(t.L) or not C8_Valid(t.D) or table.getn(t.victims) == 0 then
		C8_Rec("ERR", "what=spawn_failed|k=" .. t.k)
		t.phase = "end"
	end
end

function C8L_Divert(t, F, why)
	local s = ""
	for i, v in ipairs(t.victims) do
		s = s .. C8_Call(v, "Attack_Target", function() v.Attack_Target(t.D) end) .. ";"
	end
	t.last_divert = F
	C8_Rec("DIVERT", "k=" .. t.k .. "|why=" .. why .. "|res=" .. s)
end

function C8L_Activate(t, F)
	local kinds = { "none", "bool", "object" }
	t.act_n = t.act_n + 1
	local kind = kinds[t.act_n]
	if kind == nil then
		return false
	end
	local ab = C8_CFG.lure.ability
	local ready = C8_Flag(t.L, "Is_Ability_Ready", ab)
	local ok, err = pcall(function()
		if kind == "none" then
			return t.L.Activate_Ability(ab)
		elseif kind == "bool" then
			return t.L.Activate_Ability(ab, true)
		end
		return t.L.Activate_Ability(ab, t.victims[1])
	end)
	if not ok then
		C8_ApiFail("Activate_Ability:" .. ab .. ":" .. kind, err)
	end
	local active = C8_Flag(t.L, "Is_Ability_Active", ab)
	C8_Rec("LACT", "k=" .. t.k .. "|arg=" .. kind .. "|call=" .. (ok and "ok" or "error") .. "|ready_before=" .. ready ..
		"|active_after=" .. active .. "|ready_after=" .. C8_Flag(t.L, "Is_Ability_Ready", ab))
	if active == "1" then
		t.fact = F
		return true
	end
	return false
end

function C8L_Poll(t, F)
	local s = ""
	for i, v in ipairs(t.victims) do
		s = s .. C8_TargetRole(v, nil, t.ids, t.types) .. ";"
	end
	local ab = C8_CFG.lure.ability
	C8_Rec("LP", "k=" .. t.k .. "|ph=" .. t.phase .. "|vt=" .. s .. "|act=" .. C8_Flag(t.L, "Is_Ability_Active", ab) ..
		"|rdy=" .. C8_Flag(t.L, "Is_Ability_Ready", ab))
end

function C8L_Tick(F)
	local cfg = C8_CFG.lure
	local t = C8L.cur
	if t == nil then
		if C8L.next_f == nil then
			C8L.next_f = cfg.start_frame
			C8_Say("TEXT_CU8G_ABIL_WAIT")
		end
		if F >= C8L.next_f then
			C8L_Start(F)
		end
		return
	end
	local rel = F - t.f0
	if t.phase == "gap" then
		if F - t.fend >= cfg.gap then
			C8L.cur = nil
			C8L.next_f = F
		end
		return
	end
	if t.phase == "end" then
		C8_Despawn(t.L)
		C8_Despawn(t.D)
		for _, v in ipairs(t.victims) do
			C8_Despawn(v)
		end
		t.phase = "gap"
		t.fend = F
		C8_Rec("LEND", "k=" .. t.k)
		return
	end
	C8L_Poll(t, F)
	if t.phase == "setup" then
		if rel >= 60 and t.last_divert == nil then
			C8L_Divert(t, F, "initial")
		end
		if rel >= 90 then
			t.phase = "activate"
		end
	elseif t.phase == "activate" then
		if C8L_Activate(t, F) or t.act_n >= 3 then
			t.phase = "lure"
			t.fact = t.fact or F
		end
	elseif t.phase == "lure" then
		if F - t.last_divert >= cfg.divert_every then
			C8L_Divert(t, F, "periodic")
		end
		if F - t.fact >= cfg.watch then
			t.phase = "end"
		end
	end
end

C8_PROBES["Land"] = { name = "lure", tick = C8L_Tick, fast = true, quiet = true }
-- ==== end of cu8g_lure.lua ====
