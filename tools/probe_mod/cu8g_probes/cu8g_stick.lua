-- ==== cu8g t07: targeting stickiness probe (tools/probe_mod/cu8g_probes/cu8g_stick.lua) ====
-- Question (docs/re/CU8F_T04_T06_RE.md sec. 1.3 / 1.5): every unit starts with the Lua override TD +0x158 = 0.0, so it
-- is never sticky unless Lua or a task-force release writes the field; a negative value means "use the type's
-- Targeting_Stickiness_Time_Threshold" (Nebulon_B_Frigate: 5.0 s).
--
-- Space skirmish, AI = C8_CFG.ai_faction (Empire) runs it, the idle human (Rebel) owns the frigate. One trial per entry
-- of C8_CFG.stick.trials, all at the same empty site of the map (C8_CFG.stick.site, chosen by build_cu8g_mod.py away
-- from both start positions and every map object), objects despawned between trials:
--   arm A  no Lua call (the spawn default)
--   arm B  F.Set_Targeting_Stickiness_Time_Threshold(-1)    (= the XML 5 s)
--   arm C  F.Set_Targeting_Stickiness_Time_Threshold(1000)  (control: always sticky while the victim lives)
-- Layout (site-relative): F = human Nebulon_B_Frigate at (0, 0); V = AI Tartan_Patrol_Cruiser (Corvette, Frigate-set
-- score 3.0) at (victim_dx, 0), Prevent_All_Fire, Prevent_AI_Usage; helpers = human frigates at (helper_dx, +-helper_dy)
-- with Attack_Target(V) (an ordered target: locked, never rescans) so V's damage rate is high and its time-to-kill
-- small while its hull is still above Health_Low_Percent_Threshold 0.33 (TACTICAL_COMBAT sec. 2.4 replacement rule 1:
-- a low-health candidate always wins, so the test must happen before that).
-- Once F's attack target is V and V's Get_Time_Till_Dead (the same tracker 0x58ba40 the sticky test reads,
-- L1_S4_REMAINING sec. 2.4) is <= ttk_trigger with hull >= hull_min, the newcomer N = AI TIE_Bomber (Bomber: score 2.0,
-- strictly better than the corvette) appears at (newcomer_dx, 0): closer to F than V, so it wins F's next rescan by
-- both the score and the XY rule, unless V is sticky. N: invulnerable, Prevent_All_Fire, Suspend_Locomotor.
-- Records: TRIAL, ACQ, NEWCOMER, P (every poll: F's attack target role, V hull / shield / time-till-dead / damage rate,
-- N hull), VDEAD, TEND. Expected if sec. 1.3 holds: A switches to N, B and C keep V until it dies.

C8S = { i = 0, cur = nil, next_f = nil }

function C8S_Rel(dx, dy)
	local s = C8_CFG.stick.site
	return C8_Pos(s.x + dx, s.y + dy, s.z)
end

function C8S_Num(o, m, fmt)
	return C8_Num(C8_Get(o, m), fmt)
end

function C8S_Start(F)
	local cfg = C8_CFG.stick
	C8S.i = C8S.i + 1
	local arm = cfg.trials[C8S.i]
	if arm == nil then
		C8_Say("TEXT_CU8G_STICK_DONE")
		C8_Finish("trials=" .. (C8S.i - 1))
		return
	end
	local human = C8_Player(cfg.human_faction)
	local t = { arm = arm, k = C8S.i, f0 = F, phase = "acquire", ids = {}, types = {}, helpers = {} }
	C8S.cur = t
	if human == nil then
		C8_Rec("ERR", "what=no_human_player|faction=" .. cfg.human_faction)
	end
	local how
	t.F, how = C8_Spawn(cfg.frigate, C8S_Rel(0, 0), human)
	local val = cfg.values[arm]
	local setres = "none"
	if val ~= nil then
		setres = C8_Call(t.F, "Set_Targeting_Stickiness_Time_Threshold",
			function() t.F.Set_Targeting_Stickiness_Time_Threshold(val) end)
	end
	local vhow
	t.V, vhow = C8_Spawn(cfg.victim, C8S_Rel(cfg.victim_dx, 0), PlayerObject)
	C8_Call(t.V, "Prevent_AI_Usage", function() t.V.Prevent_AI_Usage(true) end)
	C8_Call(t.V, "Prevent_All_Fire", function() t.V.Prevent_All_Fire(true) end)
	local hs = ""
	for i = 1, cfg.helpers do
		-- helpers 1, 2 at (dx, +dy), (dx, -dy); 3, 4 at (dx, +2dy), (dx, -2dy)
		local sign, row = 1, 1
		if i == 2 or i == 4 then
			sign = -1
		end
		if i > 2 then
			row = 2
		end
		local h = C8_Spawn(cfg.helper, C8S_Rel(cfg.helper_dx, sign * row * cfg.helper_dy), human)
		if h then
			table.insert(t.helpers, h)
			local r = C8_Call(h, "Attack_Target", function() h.Attack_Target(t.V) end)
			hs = hs .. C8_Id(h) .. ":" .. r .. ";"
		end
	end
	t.ids = { V = C8_Id(t.V), H = "?" }
	t.types = { V = C8_Type(t.V) }
	C8_Rec("TRIAL", "k=" .. t.k .. "|arm=" .. arm .. "|value=" .. tostring(val) .. "|set=" .. setres .. "|F=" ..
		C8_Type(t.F) .. ":" .. C8_Id(t.F) .. ":" .. tostring(how) .. "|V=" .. C8_Type(t.V) .. ":" .. t.ids.V .. ":" ..
		tostring(vhow) .. "|Fxy=" .. C8_XY(t.F) .. "|Vxy=" .. C8_XY(t.V) .. "|helpers=" .. hs)
	if not C8_Valid(t.F) or not C8_Valid(t.V) then
		C8_Rec("ERR", "what=spawn_failed|k=" .. t.k)
		t.phase = "cleanup"
	end
end

function C8S_SpawnNewcomer(t, F, cond)
	local cfg = C8_CFG.stick
	local how, ty = nil, cfg.newcomer
	t.N, how = C8_Spawn(cfg.newcomer, C8S_Rel(cfg.newcomer_dx, 0), PlayerObject, true)
	if t.N == nil and cfg.newcomer_alt then
		ty = cfg.newcomer_alt
		t.N, how = C8_Spawn(cfg.newcomer_alt, C8S_Rel(cfg.newcomer_dx, 0), PlayerObject)
	end
	C8_Call(t.N, "Make_Invulnerable", function() t.N.Make_Invulnerable(true) end)
	C8_Call(t.N, "Prevent_All_Fire", function() t.N.Prevent_All_Fire(true) end)
	C8_Call(t.N, "Prevent_AI_Usage", function() t.N.Prevent_AI_Usage(true) end)
	C8_Call(t.N, "Suspend_Locomotor", function() t.N.Suspend_Locomotor(true) end)
	t.ids.N = C8_Id(t.N)
	t.types.N = C8_Type(t.N)
	t.fn = F
	t.phase = "watch"
	C8_Rec("NEWCOMER", "k=" .. t.k .. "|arm=" .. t.arm .. "|cond=" .. cond .. "|type=" .. ty .. "|id=" .. t.ids.N ..
		"|how=" .. tostring(how) .. "|Nxy=" .. C8_XY(t.N) .. "|dFN=" .. C8_Num(C8_Dist(t.F, t.N), "%.0f") .. "|dFV=" ..
		C8_Num(C8_Dist(t.F, t.V), "%.0f") .. "|vhull=" .. C8S_Num(t.V, "Get_Hull") .. "|vttd=" ..
		C8S_Num(t.V, "Get_Time_Till_Dead", "%.2f") .. "|tgt=" .. t.tgt)
end

function C8S_Poll(t, F)
	t.tgt = C8_TargetRole(t.F, nil, t.ids, t.types)
	t.v_valid = C8_Valid(t.V)
	t.v_hull = C8_Get(t.V, "Get_Hull")
	t.v_ttd = C8_Get(t.V, "Get_Time_Till_Dead")
	local v = "0"
	if t.v_valid then
		v = "1:" .. C8_Num(t.v_hull) .. ":" .. C8S_Num(t.V, "Get_Shield") .. ":" .. C8_Num(t.v_ttd, "%.2f") .. ":" ..
			C8S_Num(t.V, "Get_Rate_Of_Damage_Taken", "%.1f")
	end
	local n = "-"
	if t.N then
		n = "0"
		if C8_Valid(t.N) then
			n = "1:" .. C8S_Num(t.N, "Get_Hull")
		end
	end
	C8_Rec("P", "k=" .. t.k .. "|arm=" .. t.arm .. "|ph=" .. t.phase .. "|tgt=" .. t.tgt .. "|v=" .. v .. "|nc=" .. n ..
		"|fhull=" .. C8S_Num(t.F, "Get_Hull"))
end

function C8S_Cleanup(t, F, why)
	C8_Despawn(t.F)
	C8_Despawn(t.V)
	C8_Despawn(t.N)
	for _, h in ipairs(t.helpers) do
		C8_Despawn(h)
	end
	t.phase = "gap"
	t.fend = F
	C8_Rec("TEND", "k=" .. t.k .. "|arm=" .. t.arm .. "|why=" .. why)
end

function C8S_Tick(F)
	local cfg = C8_CFG.stick
	local t = C8S.cur
	if t == nil then
		if C8S.next_f == nil then
			C8S.next_f = cfg.start_frame
			C8_Say("TEXT_CU8G_STICK_WAIT")
		end
		if F >= C8S.next_f then
			if C8S.i == 0 then
				C8_Say("TEXT_CU8G_STICK_RUNNING")
			end
			C8S_Start(F)
		end
		return
	end
	if t.phase == "gap" then
		if F - t.fend >= cfg.gap then
			C8S.cur = nil
			C8S.next_f = F
		end
		return
	end
	if t.phase == "cleanup" then
		C8S_Cleanup(t, F, "spawn_failed")
		return
	end
	C8S_Poll(t, F)
	if t.phase == "acquire" then
		if t.tgt == "V" or t.tgt == "V~" then
			t.phase = "wait"
			t.facq = F
			C8_Rec("ACQ", "k=" .. t.k .. "|arm=" .. t.arm .. "|after=" .. (F - t.f0))
		elseif F - t.f0 > cfg.acquire_max then
			t.phase = "wait"
			t.facq = F
			C8_Rec("ACQ", "k=" .. t.k .. "|arm=" .. t.arm .. "|after=timeout|tgt=" .. t.tgt)
		end
	elseif t.phase == "wait" then
		local cond = nil
		if not t.v_valid then
			cond = "vdead"
		elseif t.v_ttd and t.v_ttd <= cfg.ttk_trigger and t.v_hull and t.v_hull >= cfg.hull_min then
			cond = "ttk"
		elseif t.v_hull and t.v_hull < cfg.hull_min then
			cond = "hull"
		elseif F - t.facq > cfg.wait_max then
			cond = "timeout"
		end
		if cond then
			C8S_SpawnNewcomer(t, F, cond)
		end
	elseif t.phase == "watch" then
		if not t.v_valid and t.fvdead == nil then
			t.fvdead = F
			C8_Rec("VDEAD", "k=" .. t.k .. "|arm=" .. t.arm .. "|after_n=" .. (F - t.fn) .. "|tgt=" .. t.tgt)
		end
		if (t.fvdead and F - t.fvdead >= cfg.after_death) or F - t.fn >= cfg.watch_max then
			local why = "time"
			if t.fvdead then
				why = "vdead"
			end
			C8S_Cleanup(t, F, why)
		end
	end
end

C8_PROBES["Space"] = { name = "stick", tick = C8S_Tick, fast = true, quiet = true,
	human = C8_CFG.stick and C8_CFG.stick.human_faction }
-- ==== end of cu8g_stick.lua ====
