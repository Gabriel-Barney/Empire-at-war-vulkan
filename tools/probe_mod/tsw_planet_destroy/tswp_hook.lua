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
-- Output: one _MessagePopup per event ("TSWP ..."), Game_Message proof lines. No orders except the Fire.
-- Lua 5.0 (no '#').

TSWP_TARGET = "@@TARGET@@"
TSWP_DUMMY = "@@DUMMY@@"
TSWP_RATE = @@RATE@@
TSWP_Space = false
TSWP_Battle = 0

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
end
TSWP_Reset()

function TSWP_Frame()
	local ok, t = pcall(GetCurrentTime)
	if ok and type(t) == "number" then
		return math.floor(t * 30 + 0.5)
	end
	return -1
end

function TSWP_Say(s)
	pcall(_MessagePopup, "TSWP " .. s)
	pcall(_OuputDebug, "TSWP " .. s .. "\n")
end

function TSWP_PlanetDestroyed()
	local ok, v = pcall(function() return FindPlanet(TSWP_TARGET).Is_Planet_Destroyed() end)
	if ok then
		return v and true or false
	end
	return nil
end

function TSWP_FindDS()
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
	elseif TSWP_DummySeen and TSWP_DummyGone < 0 and TSWP_Fire >= 0 then
		TSWP_DummyGone = f
		TSWP_Say(string.format("dummy %s gone at frame %d = F+%d (spec F+348 vanilla, +-1)", TSWP_DUMMY, f, f - TSWP_Fire))
	end
	if TSWP_PlanetGone < 0 and TSWP_Fire >= 0 and TSWP_PlanetDestroyed() then
		TSWP_PlanetGone = f
		TSWP_Say(string.format("Is_Planet_Destroyed true at frame %d = F+%d", f, f - TSWP_Fire))
	end
	if TSWP_Fire < 0 then
		if TSWP_DS == nil then
			TSWP_DS = TSWP_FindDS()
		end
		if TSWP_DS then
			local ok, r = pcall(function() return TSWP_DS.Is_Tactical_Superweapon_Ready() end)
			if ok and r then
				if TSWP_Ready < 0 then
					TSWP_Ready = f
				end
				local okf, res = pcall(function() return TSWP_DS.Fire_Tactical_Superweapon() end)
				TSWP_FireOk = tostring(okf) .. "/" .. tostring(res)
				if okf then
					TSWP_Fire = f
					TSWP_Say(string.format("fired at frame F=%d (ready seen %d) result=%s; dummy seen=%s planet destroyed at start=%s",
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
end

TSWP_Orig_Game_Mode_Starting_Event = Game_Mode_Starting_Event
function Game_Mode_Starting_Event(mode_name, map_name)
	TSWP_Space = StringCompare(tostring(mode_name), "Space") and true or false
	TSWP_Reset()
	if TSWP_Space then
		TSWP_Battle = TSWP_Battle + 1
		pcall(Game_Message, "TEXT_TSWP_HOOK_ALIVE")
		TSWP_Say(string.format("hook alive battle=%d map=%s start frame=%d", TSWP_Battle, tostring(map_name), TSWP_Frame()))
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
		TSWP_Say(string.format("battle %d END frame=%d = F+%d (spec: not before F+732); F=%d dummy gone=%d planet gone=%d; " ..
			"services=%d min step=%d frames", TSWP_Battle, f, rel, TSWP_Fire, TSWP_DummyGone, TSWP_PlanetGone, TSWP_Services,
			TSWP_MinStep))
	end
	TSWP_Space = false
	TSWP_Orig_Game_Mode_Ending_Event(mode_name)
end

TSWP_Orig_GameService = GameService
function GameService()
	pcall(TSWP_Service)
	TSWP_Orig_GameService()
end
-- ==== end TSW planet-destroy probe ====
