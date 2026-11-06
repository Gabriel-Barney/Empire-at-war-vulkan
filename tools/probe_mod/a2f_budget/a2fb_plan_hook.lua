-- ==== a2f t07 budget probe: AI plan hook (channel P) ====
-- Appended (after the generated header and a2fb_core.lua) to an unchanged copy of a shipped plan script from
-- Data\Scripts\AI (the Infrastructure plans and the Empire tech plan). A plan's task-force thread starts after the
-- goal is accepted and its BudgetedGoalStruct exists, so BudgetWrapper (the Lua global Budget, plans only, acts on
-- the calling plan's goal) answers about that goal:
--   Get_Spendable_Resources   min(alloc, covered + accrued)   AI_GALACTIC 1.8
--   Get_Unallocated_Resources req
-- Both are read-only. The record PSTART carries them, the plan name, frame and the player's credits; a small
-- sample of PlayerObject follows (same SM record as the other channels). The shipped thread then runs unchanged.
A2FB_MAX = 300
A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_Plan.txt"
A2FB_PLAN_N = 0

A2FB_Say("LOAD", "plan=@@PLAN@@ modeq=" .. tostring(A2FB_MODEQ))
A2FB_Canary(A2FB_CANARY)

function A2FB_PlanStart()
	if A2FB_PLAN_N >= 40 then
		return
	end
	A2FB_PLAN_N = A2FB_PLAN_N + 1
	local t = A2FB_Now()
	local fac = "?"
	if PlayerObject ~= nil then
		fac = A2FB_Fac(PlayerObject)
	end
	A2FB_Say("PSTART", "plan=@@PLAN@@ t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " fac=" .. fac ..
		" budget=" .. type(Budget) ..
		" spend=" .. A2FB_Str(function() return Budget.Get_Spendable_Resources() end) ..
		" unalloc=" .. A2FB_Str(function() return Budget.Get_Unallocated_Resources() end) ..
		" cr=" .. A2FB_Str(function() return PlayerObject.Get_Credits() end))
	if PlayerObject ~= nil then
		A2FB_Sample("PS", PlayerObject, false)
	end
end

A2FB_Thread_Orig = @@THREAD@@
function @@THREAD@@(...)
	local ok, err = pcall(A2FB_PlanStart)
	if not ok then
		A2FB_Error("PlanStart", err)
	end
	return A2FB_Thread_Orig(unpack(arg))
end
