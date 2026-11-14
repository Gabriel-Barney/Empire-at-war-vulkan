-- ==== a2f t07 budget probe: galactic free store hook (channel F) ====
-- Appended (after the generated header and a2fb_core.lua) to an unchanged copy of Data\Scripts\FreeStore\GalacticFreeStore.lua.
-- The engine runs one instance of that script per AI player (PlayerObject = that player) and resumes it every
-- ServiceRate = 8 s; its main() calls FreeStoreService() each time. The wrapper below takes one full sample of
-- PlayerObject first and then calls the shipped function exactly as before: no rate change, no extra call, so the
-- AI's own schedule is the shipped one. Samples start at the player's first free-store service (the AI context is
-- long initialised by then, so this channel shows the post-Init state every 8 s, independent of the story script).
-- 400 records = 100 full samples (PL SM BF SH) until a2g; 500 since a2h t02 so the Empire's extra MJ record keeps the
-- same 100 samples (about 13 minutes at 8 s). a2j t04 (30-minute run): 300 samples (40 minutes at 8 s), 1600 records
-- (the Empire's sample has 5 records: PL SM BF SH MJ).
A2FB_MAX = 1600
-- a2m t12: a saboteur build adds FSAB / FSABN / FADD / FSVC records (Underworld instance; retail run 20261108-212556:
-- 69 records in 48 s, 33 of them FADD), so the 30-minute saboteur run (--sab-long) gets more room
if A2FB_SAB then
	A2FB_MAX = 4000
end
A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_FreeStore.txt"
A2FB_FS_N = 0

A2FB_Say("LOAD", "galactic free store chunk ran modeq=" .. tostring(A2FB_MODEQ))
A2FB_Canary(A2FB_CANARY)

function A2FB_FS_Sample()
	if PlayerObject == nil then
		return
	end
	if A2FB_FS_N >= 300 then
		return
	end
	A2FB_FS_N = A2FB_FS_N + 1
	A2FB_Sample("FS", PlayerObject, true)
end

A2FB_FS_Orig = FreeStoreService
function FreeStoreService()
	local ok, err = pcall(A2FB_FS_Sample)
	if not ok then
		A2FB_Error("FSSample", err)
	end
	if A2FB_SAB then
		local oks, errs = pcall(A2FB_FS_Sab)
		if not oks then
			A2FB_Error("FSSab", errs)
		end
	end
	if A2FB_FS_Orig then
		return A2FB_FS_Orig()
	end
end

-- ==== a2k t05 (saboteur run): only when built with --saboteur (A2FB_SAB in the generated header) ====
-- docs/re/A2K_T08_RE_ANSWERS.md section 5 item 4 (H2 of Q4.2: does the starting saboteur come back to the store, and
-- when). In the Underworld's instance only (PlayerObject = Underworld), read-only, all inside pcall:
--   FSAB  at every FreeStoreService (8 s), per Underworld_Saboteur_Team the Underworld owns: id= Get_Object_ID,
--         pl= Get_Planet_Location, st= FreeStore.Is_Object_On_Free_Store (map membership, reserved entries too;
--         A1_READINGS section 28.5), tr= FreeStore.Is_Unit_In_Transit, read only when st is true (the shipped script
--         calls it on store units only; - otherwise). n= saboteurs found
--   FADD  every On_Unit_Added call (free-store admission, every type): type=, id=, pl=; then the shipped handler runs
--   FSVC  every On_Unit_Service call (the engine calls it for unreserved store entries only, +0x30 == 0; every type,
--         because a store entry can be the transport that carries the saboteur company): id=, pl=, type=,
--         tp= Is_Transport, safe= FreeStore.Is_Unit_Safe, tr= Is_Unit_In_Transit (all read by the shipped handler
--         itself); then the shipped handler runs
-- Each wrapper records first and then calls the shipped function with the same arguments: no rate change, no extra
-- order, no GameRandom draw. A binding error here would end the AI's free-store script, so every read is one the
-- shipped free store makes itself, except Is_Object_On_Free_Store (registered, never called by a shipped script),
-- which gets only a valid game object wrapper (TestValid first).
if A2FB_SAB then
	function A2FB_FS_IsUW()
		return PlayerObject ~= nil and A2FB_Fac(PlayerObject) == "UNDERWORLD"
	end

	function A2FB_FS_Name(o)
		if o == nil then
			return "nil"
		end
		return A2FB_Str(function() return string.upper(o.Get_Type().Get_Name()) end)
	end

	function A2FB_FS_Head(o)
		local t = A2FB_Now()
		local pl = nil
		pcall(function() pl = o.Get_Planet_Location() end)
		return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " fac=UNDERWORLD id=" ..
			A2FB_Str(function() return o.Get_Object_ID() end) .. " pl=" .. A2FB_FS_Name(pl)
	end

	function A2FB_FS_Sab()
		if not A2FB_FS_IsUW() then
			return
		end
		local ok, all = pcall(Find_All_Objects_Of_Type, A2FB_SABCFG.type)
		if not ok or type(all) ~= "table" then
			A2FB_Error("FSSabFind", all)
			return
		end
		local n = 0
		for _, o in pairs(all) do
			local own = A2FB_Str(function() return string.upper(o.Get_Owner().Get_Faction_Name()) end)
			local okv, valid = pcall(TestValid, o)
			if own == "UNDERWORLD" and okv and valid then
				n = n + 1
				A2FB_Rec("FSAB", function()
					local st = nil
					local okst = pcall(function() st = FreeStore.Is_Object_On_Free_Store(o) end)
					local tr = "-"
					if okst and st == true then
						tr = A2FB_Str(function() return FreeStore.Is_Unit_In_Transit(o) end)
					end
					local sts = tostring(st)
					if not okst then
						sts = "ERR"
					end
					return A2FB_FS_Head(o) .. " st=" .. sts .. " tr=" .. tr .. " i=" .. n
				end)
			end
		end
		A2FB_Rec("FSABN", function()
			local t = A2FB_Now()
			return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " fac=UNDERWORLD n=" .. n
		end)
	end

	A2FB_Added_Orig = On_Unit_Added
	if A2FB_Added_Orig ~= nil then
		function On_Unit_Added(object)
			if A2FB_FS_IsUW() then
				A2FB_Rec("FADD", function() return A2FB_FS_Head(object) .. " type=" .. A2FB_FS_Name(object) end)
			end
			return A2FB_Added_Orig(object)
		end
	end

	A2FB_Service_Orig = On_Unit_Service
	if A2FB_Service_Orig ~= nil then
		function On_Unit_Service(object)
			if A2FB_FS_IsUW() then
				A2FB_Rec("FSVC", function()
					return A2FB_FS_Head(object) .. " type=" .. A2FB_FS_Name(object) ..
						" tp=" .. A2FB_Str(function() return object.Is_Transport() end) ..
						" safe=" .. A2FB_Str(function() return FreeStore.Is_Unit_Safe(object) end) ..
						" tr=" .. A2FB_Str(function() return FreeStore.Is_Unit_In_Transit(object) end)
				end)
			end
			return A2FB_Service_Orig(object)
		end
	end
	A2FB_Say("SABLOAD", "free store saboteur hooks added=" .. tostring(A2FB_Added_Orig ~= nil) ..
		" service=" .. tostring(A2FB_Service_Orig ~= nil))
end
