-- ==== TSW planet-destroy probe (appended to the shipped GameScoring.lua by tools/probe_mod/tsw_planet_destroy/build_mod.py) ====
-- docs/re/TSW_PLANET_DESTROY.md sec. 3 / sec. 4 (frame offsets) and sec. 2 (victory hold). In every SPACE battle:
--   * fires the Death Star (any object whose Is_Tactical_Superweapon_Ready() is true) at the first service where it is
--     ready, and stamps the Lua frame F = round(GetCurrentTime() x 30) of that call (AI_TACTICAL: GetCurrentTime is
--     mode time F/30, frame exact);
--   * at every later service stamps the first frame where (a) the target dummy @@DUMMY@@ is gone (the state-9 kill,
--     sec. 4 step 6) and (b) FindPlanet("@@TARGET@@").Is_Planet_Destroyed() is true (D +0x2c8, sec. 4 step 9);
--   * stamps the frame of Game_Mode_Ending_Event (the battle end; spec: not before F + 732 with vanilla data).
-- Service granularity: ServiceRate is set to @@RATE@@ s; the popup reports the smallest service step seen, so every
-- stamp is "at or before this frame, after the previous service".
-- Output: retail _MessagePopup / _OuputDebug show nothing (2026-10-31 run), so every event is kept as a Lua string
-- "TSWP1|<KIND>|<seq>|<text>|END" in the global table TSWP_Records (alive until the game exits) and tswp_reader.py
-- finds it by scanning the game's heap; Game_Message proof lines show on screen. No orders except the Fire.
-- Lua 5.0 (no '#').

TSWP_TARGET = "@@TARGET@@"
TSWP_DUMMY = "@@DUMMY@@"
TSWP_RATE = @@RATE@@
TSWP_Space = false
TSWP_Battle = 0
if TSWP_Records == nil then
	TSWP_Records = {}
	TSWP_Seq = 0
end

function TSWP_Reset()
	TSWP_Services = 0
	TSWP_LastFrame = -1
	TSWP_MinStep = 1000000
	TSWP_Fire = -1
	TSWP_FireOk = "?"
	TSWP_Ready = -1
	TSWP_DummySeen = false
	TSWP_DummyGone = -1
	TSWP_PlanetGone = -1
	TSWP_PlanetAtStart = "?"
	TSWP_DS = nil
	TSWP_DSHow = "none"
	TSWP_Diag = false
	TSWP_Unready = -1
end
TSWP_Reset()

function TSWP_Frame()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return math.floor(t * 30 + 0.5)
	end
	return -1
end

function TSWP_Say(kind, s)
	-- self-initialising and pcall'd: in the 2026-10-31 run the on-screen ALIVE line showed but no record reached the
	-- heap, so an error here must not take the caller (Game_Mode_Starting_Event / the service) down with it
	pcall(function()
		if TSWP_Records == nil or TSWP_Seq == nil then
			TSWP_Records = {}
			TSWP_Seq = 0
		end
		TSWP_Seq = TSWP_Seq + 1
		local s2 = "TSWP1|" .. kind .. "|" .. TSWP_Seq .. "|battle=" .. tostring(TSWP_Battle) .. " " .. tostring(s) .. "|END"
		table.insert(TSWP_Records, s2)
	end)
end

function TSWP_PlanetDestroyed()
	local ok, v = pcall(function() return FindPlanet(TSWP_TARGET).Is_Planet_Destroyed() end)
	if ok then
		return v and true or false
	end
	return nil
end

function TSWP_FindDS()
	-- by name first: vanilla AI/SpaceMode/FireDeathStar.lua uses Find_First_Object("Death_Star"); in FoC
	-- Death_Star_II is itself the space unit (SpaceBehavior TACTICAL_SUPER_WEAPON)
	TSWP_DSHow = "none"
	for _, name in ipairs({ "Death_Star_II", "Death_Star" }) do
		local ok, obj = pcall(Find_First_Object, name)
		if ok and obj ~= nil then
			local okv, v = pcall(TestValid, obj)
			if okv and v then
				TSWP_DSHow = "Find_First_Object(" .. name .. ")"
				return obj
			end
		end
		TSWP_DSHow = TSWP_DSHow .. " " .. name .. ":" .. tostring(ok) .. "/" .. tostring(obj)
	end
	-- the first object that answers Is_Tactical_Superweapon_Ready (only TACTICAL_SUPER_WEAPON objects do)
	for _, fac in ipairs({ "Empire", "Rebel", "Underworld" }) do
		local p = nil
		pcall(function() p = Find_Player(fac) end)
		if p then
			local ok, list = pcall(Find_All_Objects_Of_Type, p)
			if ok and type(list) == "table" then
				for _, obj in pairs(list) do
					local ok2, r = pcall(function() return obj.Is_Tactical_Superweapon_Ready() end)
					if ok2 and r ~= nil then
						local okn, n = pcall(function() return obj.Get_Type().Get_Name() end)
						if okn and n and string.find(string.upper(n), "DEATH_STAR") then
							return obj
						end
					end
				end
			end
		end
	end
	return nil
end

function TSWP_DummyAlive()
	local ok, obj = pcall(Find_First_Object, TSWP_DUMMY)
	if not ok or obj == nil then
		return false
	end
	local okv, v = pcall(TestValid, obj)
	return okv and v and true or false
end

function TSWP_Service()
	if not TSWP_Space then
		return
	end
	local f = TSWP_Frame()
	TSWP_Services = TSWP_Services + 1
	if TSWP_LastFrame >= 0 and f > TSWP_LastFrame and f - TSWP_LastFrame < TSWP_MinStep then
		TSWP_MinStep = f - TSWP_LastFrame
	end
	TSWP_LastFrame = f
	if TSWP_Services == 1 then
		TSWP_PlanetAtStart = tostring(TSWP_PlanetDestroyed())
	end
	local alive = TSWP_DummyAlive()
	if alive then
		TSWP_DummySeen = true
	elseif TSWP_DummySeen and TSWP_DummyGone < 0 then
		TSWP_DummyGone = f
		TSWP_Say("DUMMY", string.format("dummy %s gone at frame %d = F+%d (spec F+348 vanilla, +-1)", TSWP_DUMMY, f, f - TSWP_Fire))
	end
	if TSWP_PlanetGone < 0 and TSWP_Services > 1 and TSWP_PlanetDestroyed() then
		TSWP_PlanetGone = f
		TSWP_Say("PLANET", string.format("Is_Planet_Destroyed true at frame %d = F+%d", f, f - TSWP_Fire))
	end
	if TSWP_Services == 300 then
		TSWP_Say("STATE", string.format("frame %d: dummy seen=%s planet destroyed at start=%s death star=%s", f,
			tostring(TSWP_DummySeen), TSWP_PlanetAtStart, TSWP_DSHow))
	end
	if TSWP_Fire < 0 then
		if TSWP_DS == nil then
			TSWP_DS = TSWP_FindDS()
		end
		if TSWP_DS == nil and not TSWP_Diag and TSWP_Services >= 300 then
			TSWP_Diag = true
			TSWP_Say("NODS", string.format("frame %d: no Death Star found: %s", f, TSWP_DSHow))
		end
		if TSWP_DS then
			local ok, r = pcall(function() return TSWP_DS.Is_Tactical_Superweapon_Ready() end)
			if not ok and not TSWP_Diag then
				TSWP_Diag = true
				TSWP_Say("READYERR", string.format("frame %d: Is_Tactical_Superweapon_Ready error %s (%s)", f, tostring(r),
					TSWP_DSHow))
			end
			if ok and not r and TSWP_Ready >= 0 and TSWP_Unready < 0 then
				-- ready went false without the hook's Fire: fired by hand (the frame is F + 0..1 service)
				TSWP_Unready = f
				TSWP_Say("UNREADY", string.format("ready dropped at frame %d without the hook's Fire (manual Fire?)", f))
			end
			if ok and r then
				if TSWP_Ready < 0 then
					TSWP_Ready = f
					TSWP_Say("READY", string.format("ready at frame %d (%s)", f, TSWP_DSHow))
				end
				local okf, res = pcall(function() return TSWP_DS.Fire_Tactical_Superweapon() end)
				TSWP_FireOk = tostring(okf) .. "/" .. tostring(res)
				if not okf and math.mod(TSWP_Services, 300) == 0 then
					TSWP_Say("FIREERR", string.format("frame %d: Fire_Tactical_Superweapon error %s", f, tostring(res)))
				end
				if okf then
					TSWP_Fire = f
					TSWP_Say("FIRED", string.format("fired at frame F=%d (ready seen %d) result=%s; dummy seen=%s planet destroyed at start=%s",
						f, TSWP_Ready, TSWP_FireOk, tostring(TSWP_DummySeen), TSWP_PlanetAtStart))
					pcall(Game_Message, "TEXT_TSWP_FIRED")
				end
			end
		end
	end
end

TSWP_Orig_Base_Definitions = Base_Definitions
function Base_Definitions()
	TSWP_Orig_Base_Definitions()
	ServiceRate = TSWP_RATE
	TSWP_Say("BASEDEF", "Base_Definitions ran, ServiceRate=" .. tostring(ServiceRate))
end

TSWP_Orig_Game_Mode_Starting_Event = Game_Mode_Starting_Event
function Game_Mode_Starting_Event(mode_name, map_name)
	TSWP_Space = StringCompare(tostring(mode_name), "Space") and true or false
	TSWP_Reset()
	if TSWP_Space then
		TSWP_Battle = TSWP_Battle + 1
		pcall(Game_Message, "TEXT_TSWP_HOOK_ALIVE")
		TSWP_Say("ALIVE", string.format("hook alive battle=%d map=%s start frame=%d", TSWP_Battle, tostring(map_name), TSWP_Frame()))
	end
	TSWP_Orig_Game_Mode_Starting_Event(mode_name, map_name)
end

TSWP_Orig_Game_Mode_Ending_Event = Game_Mode_Ending_Event
function Game_Mode_Ending_Event(mode_name)
	if TSWP_Space then
		local f = TSWP_Frame()
		local rel = -1
		if TSWP_Fire >= 0 then
			rel = f - TSWP_Fire
		end
		TSWP_Say("END", string.format("battle %d END frame=%d = F+%d (spec: not before F+732); F=%d dummy gone=%d planet gone=%d; " ..
			"services=%d min step=%d frames", TSWP_Battle, f, rel, TSWP_Fire, TSWP_DummyGone, TSWP_PlanetGone, TSWP_Services,
			TSWP_MinStep))
	end
	TSWP_Space = false
	TSWP_Orig_Game_Mode_Ending_Event(mode_name)
end

TSWP_Orig_GameService = GameService
TSWP_GameServiceCalls = 0
function GameService()
	TSWP_GameServiceCalls = TSWP_GameServiceCalls + 1
	if TSWP_GameServiceCalls == 1 or math.mod(TSWP_GameServiceCalls, 1800) == 0 then
		TSWP_Say("SVC", string.format("GameService call %d frame=%d space=%s", TSWP_GameServiceCalls, TSWP_Frame(),
			tostring(TSWP_Space)))
	end
	local ok, err = pcall(TSWP_Service)
	if not ok and not TSWP_ServiceErr then
		TSWP_ServiceErr = true
		TSWP_Say("SVCERR", "TSWP_Service error: " .. tostring(err))
	end
	if TSWP_Orig_GameService then
		TSWP_Orig_GameService()
	end
end
TSWP_Say("LOAD", "hook file executed")
-- ==== end TSW planet-destroy probe ====
