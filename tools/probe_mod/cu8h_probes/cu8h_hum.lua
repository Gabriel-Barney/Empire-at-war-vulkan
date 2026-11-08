-- ==== cu8h t02: lightsaber hum / holster probe (tools/probe_mod/cu8h_probes/cu8h_hum.lua) ====
-- Question (docs/re/CU8F_T01_T02_T07_RE.md sec. 2.3 / 2.5): a HOLSTER_WEAPON hero starts with its engine loops (the
-- lightsaber hum) suppressed (obj +0x3b9 = 1); every 15th frame the behaviour decides drawn / holstered (engaged = has
-- an attack target, ...); a draw transition (5 frames) ends with unsuppress; a holster starts 450 frames after the
-- last engaged frame and suppresses again. Saber_Throw snaps the bone at the throw and at the catch, both with
-- suppress(0), so after a catch the hum stays off until a later draw transition completes [M].
--
-- Land skirmish, AI = C8_CFG.ai_faction (Rebel) runs it; the idle human (Empire) owns the heroes. At the site
-- C8_CFG.hum.site (a dry, open square of the map, chosen by build_cu8h_mod.py):
--   V  = Darth_Vader for the human player at (0, 0), invulnerable. Idle `idle` frames (no enemy near).
--   T  = Rebel_Trooper for the AI at (target_dx, 0), invulnerable; V.Attack_Target(T) every 30 frames for `engage`
--        frames; then T is despawned and V waits `after` frames.
--   L  = Luke_Skywalker_Jedi for the human player at (0, luke_dy), invulnerable, SABER_THROW autofire on; after
--        `luke_idle` frames T2 = an AI Rebel_Trooper at (luke_target_dx, luke_dy), invulnerable,
--        L.Attack_Target(T2) every 30 frames; at `luke_fallback` frames
--        without a recorded throw L.Activate_Ability("SABER_THROW", T2) is tried once (last, it may stop the script);
--        after `luke_watch` frames T2 is despawned and L waits `luke_after` frames.
-- The frame-exact state (obj +0x3b9, the behaviour's drawn flag and transition frames) is read from memory by
-- cu8h_run.py; this script records what it ordered and, every 3 frames, the heroes' attack targets.
-- Records: HSPAWN, HORDER, HP (every service: each hero's attack target class and type), HPHASE, HEND.

C8H = { phase = "wait", f0 = nil }

function C8H_Rel(dx, dy)
	local s = C8_CFG.hum.site
	return C8_Pos(s.x + dx, s.y + dy, s.z)
end

function C8H_Spawn(role, tname, alt, dx, dy, player)
	local o, how = C8_Spawn(tname, C8H_Rel(dx, dy), player, true)
	if o == nil and alt then
		o, how = C8_Spawn(alt, C8H_Rel(dx, dy), player, false)
		tname = alt
	end
	if o then
		C8_Call(o, "Make_Invulnerable", function() o.Make_Invulnerable(true) end)
	end
	C8_Rec("HSPAWN", "role=" .. role .. "|type=" .. C8_Str(tname) .. "|how=" .. C8_Str(how) .. "|id=" ..
		(o and C8_Id(o) or "-") .. "|at=" .. (o and C8_XY(o) or "-"))
	return o
end

function C8H_Phase(name, F)
	C8H.phase = name
	C8H.pf = F
	C8_Rec("HPHASE", "phase=" .. name)
end

function C8H_Poll(F)
	-- every free-store service (every 3rd frame with the fast service rate)
	local s = ""
	for _, r in ipairs({ { "V", C8H.V }, { "L", C8H.L } }) do
		if C8_Valid(r[2]) then
			local cls, id, ty = C8_TargetOf(r[2])
			s = s .. r[1] .. "=" .. cls .. ":" .. ty .. ";"
		end
	end
	if s ~= "" then
		C8_Rec("HP", s)
	end
end

function C8H_Attack(who, tgt, tag)
	C8H.last_order = C8H.cur_f
	return C8_Call(who, tag, function() who.Attack_Target(tgt) end)
end

function C8H_Tick(F)
	local cfg = C8_CFG.hum
	C8H.cur_f = F
	if C8H.f0 == nil then
		C8H.f0 = F
		C8_Say("TEXT_CU8H_HUM_WAIT")
	end
	C8H_Poll(F)
	local rel = F - (C8H.pf or F)
	if C8H.phase == "wait" then
		if F >= cfg.start_frame and cfg.luke_only then
			-- session luke: no Vader; straight to Luke
			C8H_Phase("after", F - cfg.after)
		elseif F >= cfg.start_frame then
			C8H.V = C8H_Spawn("V", cfg.vader, nil, 0, 0, C8_Player(cfg.human_faction))
			if not C8_Valid(C8H.V) then
				C8_Rec("ERR", "what=vader_spawn_failed")
				C8_Finish("vader=failed")
				return
			end
			C8H_Phase("idle", F)
		end
	elseif C8H.phase == "idle" then
		if rel >= cfg.idle then
			C8H.T = C8H_Spawn("T", cfg.target, cfg.target_alt, cfg.target_dx, 0, PlayerObject)
			C8_Call(C8H.T, "Prevent_AI_Usage", function() C8H.T.Prevent_AI_Usage(true) end)
			C8_Rec("HORDER", "who=V|what=attack|res=" .. C8H_Attack(C8H.V, C8H.T, "V.Attack_Target"))
			C8H_Phase("engage", F)
		end
	elseif C8H.phase == "engage" then
		if F - C8H.last_order >= 30 and rel < cfg.engage then
			C8H_Attack(C8H.V, C8H.T, "V.Attack_Target")
		end
		if rel >= cfg.engage then
			C8_Despawn(C8H.T)
			C8_Rec("HORDER", "who=T|what=despawn|valid_after=" .. tostring(C8_Valid(C8H.T)))
			C8H_Phase("after", F)
		end
	elseif C8H.phase == "after" then
		if rel >= cfg.after then
			C8_Say("TEXT_CU8H_HUM_LUKE")
			C8H.L = C8H_Spawn("L", cfg.luke, nil, 0, cfg.luke_dy, C8_Player(cfg.human_faction))
			if not C8_Valid(C8H.L) then
				C8_Rec("ERR", "what=luke_spawn_failed")
				C8H_Phase("end", F)
				return
			end
			C8_Rec("HORDER", "who=L|what=autofire|res=" .. C8_Call(C8H.L, "Set_Single_Ability_Autofire",
				function() C8H.L.Set_Single_Ability_Autofire("SABER_THROW", true) end))
			C8H_Phase("luke_idle", F)
		end
	elseif C8H.phase == "luke_idle" then
		if rel >= cfg.luke_idle then
			C8H.T2 = C8H_Spawn("T2", cfg.target, cfg.target_alt, cfg.luke_target_dx, cfg.luke_dy, PlayerObject)
			C8_Call(C8H.T2, "Prevent_AI_Usage", function() C8H.T2.Prevent_AI_Usage(true) end)
			C8_Rec("HORDER", "who=L|what=attack|res=" .. C8H_Attack(C8H.L, C8H.T2, "L.Attack_Target"))
			C8H_Phase("luke", F)
		end
	elseif C8H.phase == "luke" then
		if cfg.luke_second and rel >= cfg.luke_second and not C8H.T3 then
			-- session luke: a throw while drawn (T3 near T2, Luke onto T3, then SABER_THROW at T3)
			C8H.T3 = C8H_Spawn("T3", cfg.target, cfg.target_alt, cfg.luke_target_dx, cfg.luke_dy + cfg.luke_second_dy,
				PlayerObject)
			C8_Call(C8H.T3, "Prevent_AI_Usage", function() C8H.T3.Prevent_AI_Usage(true) end)
			C8_Rec("HORDER", "who=L|what=attack_t3|res=" .. C8H_Attack(C8H.L, C8H.T3, "L.Attack_Target"))
		end
		if C8H.T3 and not C8H.second_done and rel >= cfg.luke_second + cfg.luke_second_throw then
			C8H.second_done = true
			C8_Rec("HORDER", "who=L|what=second_saber_throw|step=before")
			local res = C8_Call(C8H.L, "Activate_Ability", function() C8H.L.Activate_Ability("SABER_THROW", C8H.T3) end)
			C8_Rec("HORDER", "who=L|what=second_saber_throw|res=" .. res)
		end
		if F - C8H.last_order >= 30 and rel < cfg.luke_watch then
			C8H_Attack(C8H.L, C8H.T3 or C8H.T2, "L.Attack_Target")
		end
		if rel >= cfg.luke_fallback and not C8H.fallback_done then
			C8H.fallback_done = true
			C8_Rec("HORDER", "who=L|what=activate_saber_throw|step=before")
			local res = C8_Call(C8H.L, "Activate_Ability", function() C8H.L.Activate_Ability("SABER_THROW", C8H.T2) end)
			C8_Rec("HORDER", "who=L|what=activate_saber_throw|res=" .. res)
		end
		if rel >= cfg.luke_watch then
			C8_Despawn(C8H.T2)
			C8_Rec("HORDER", "who=T2|what=despawn")
			if C8H.T3 then
				C8_Despawn(C8H.T3)
				C8_Rec("HORDER", "who=T3|what=despawn")
			end
			C8H_Phase("luke_after", F)
		end
	elseif C8H.phase == "luke_after" then
		if rel >= cfg.luke_after then
			C8H_Phase("end", F)
		end
	elseif C8H.phase == "end" then
		C8_Say("TEXT_CU8H_HUM_DONE")
		C8_Rec("HEND", "f=" .. F)
		C8_Finish("hum=done")
	end
end

C8_PROBES["Land"] = { name = "hum", tick = C8H_Tick, fast = true, quiet = true,
	human = C8_CFG.hum and C8_CFG.hum.human_faction }
-- ==== end of cu8h_hum.lua ====
