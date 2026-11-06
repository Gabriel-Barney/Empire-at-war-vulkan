-- ==== a2f t07 budget probe: galactic free store hook (channel F) ====
-- Appended (after the generated header and a2fb_core.lua) to an unchanged copy of Data\Scripts\FreeStore\GalacticFreeStore.lua.
-- The engine runs one instance of that script per AI player (PlayerObject = that player) and resumes it every
-- ServiceRate = 8 s; its main() calls FreeStoreService() each time. The wrapper below takes one full sample of
-- PlayerObject first and then calls the shipped function exactly as before: no rate change, no extra call, so the
-- AI's own schedule is the shipped one. Samples start at the player's first free-store service (the AI context is
-- long initialised by then, so this channel shows the post-Init state every 8 s, independent of the story script).
A2FB_MAX = 400
A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_FreeStore.txt"
A2FB_FS_N = 0

A2FB_Say("LOAD", "galactic free store chunk ran modeq=" .. tostring(A2FB_MODEQ))
A2FB_Canary(A2FB_CANARY)

function A2FB_FS_Sample()
	if PlayerObject == nil then
		return
	end
	if A2FB_FS_N >= 200 then
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
	if A2FB_FS_Orig then
		return A2FB_FS_Orig()
	end
end
