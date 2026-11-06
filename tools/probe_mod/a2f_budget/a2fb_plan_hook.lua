-- ==== a2f t07 budget probe: AI plan hook (channel P) ====
-- Appended (after the generated header and a2fb_core.lua) to an unchanged copy of a shipped plan script from
-- Data\Scripts\AI (the Infrastructure plans, the Empire tech plan and, since a2h t02, the heavy / advanced vehicle
-- factory, Lift_Blockade, FlushMajorItemBudget and ConquerPirate plans). A plan's task-force thread starts after the
-- goal is accepted and its BudgetedGoalStruct exists, so BudgetWrapper (the Lua global Budget, plans only, acts on
-- the calling plan's goal) answers about that goal:
--   Get_Spendable_Resources   min(alloc, covered + accrued)   AI_GALACTIC 1.8
--   Get_Unallocated_Resources req
-- Both are read-only. The record PSTART carries them, the plan name, frame and the player's credits; a small
-- sample of PlayerObject follows (same SM record as the other channels; a full one for @@FULL@@ plans). The shipped
-- thread then runs unchanged.
-- a2h t02: the end of a run is recorded too (same fields, n = the PSTART it belongs to, age = seconds since it):
--   PRET   the wrapped thread function returned (rare: ScriptExit yields and never comes back)
--   PEXIT  the plan script called ScriptExit (PGBase): normal end or abandon; by= / line= name the caller
--   PFAIL  the shipped <force>_Production_Failed handler was called (only wrapped when the script defines it)
-- A run that the goal system releases has its thread killed: no end record. PSTART's prev= says how the previous
-- run of the same script instance ended (none = no end record = killed / released), so a lost end record is seen.
-- No start cap (a2f had 40 per script instance).
A2FB_MAX = 3000
A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_Plan.txt"
A2FB_PLAN_N = 0
A2FB_PLAN_CUR = 0
A2FB_PLAN_T0 = -1
A2FB_PLAN_END = "none"
A2FB_PLAN_FULL = @@FULL@@

A2FB_Say("LOAD", "plan=@@PLAN@@ modeq=" .. tostring(A2FB_MODEQ))
A2FB_Canary(A2FB_CANARY)

function A2FB_PlanHead(t)
	local fac = "?"
	if PlayerObject ~= nil then
		fac = A2FB_Fac(PlayerObject)
	end
	return "plan=@@PLAN@@ t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " fac=" .. fac
end

function A2FB_PlanBudget()
	return " spend=" .. A2FB_Str(function() return Budget.Get_Spendable_Resources() end) ..
		" unalloc=" .. A2FB_Str(function() return Budget.Get_Unallocated_Resources() end) ..
		" cr=" .. A2FB_Str(function() return PlayerObject.Get_Credits() end)
end

function A2FB_PlanStart()
	local prev = A2FB_PLAN_END
	if A2FB_PLAN_N == 0 then
		prev = "first"
	end
	A2FB_PLAN_N = A2FB_PLAN_N + 1
	A2FB_PLAN_CUR = A2FB_PLAN_N
	A2FB_PLAN_END = "none"
	local t = A2FB_Now()
	A2FB_PLAN_T0 = t
	A2FB_Say("PSTART", A2FB_PlanHead(t) .. " budget=" .. type(Budget) .. A2FB_PlanBudget() ..
		" n=" .. A2FB_PLAN_N .. " prev=" .. prev)
	if PlayerObject ~= nil then
		A2FB_Sample("PS", PlayerObject, A2FB_PLAN_FULL)
	end
end

function A2FB_PlanEnd(kind, extra)
	local t = A2FB_Now()
	local age = "nil"
	if A2FB_PLAN_T0 >= 0 and t >= 0 then
		age = A2FB_Num(t - A2FB_PLAN_T0)
	end
	if A2FB_PLAN_END == "none" then
		A2FB_PLAN_END = kind
	else
		A2FB_PLAN_END = string.sub(A2FB_PLAN_END .. "+" .. kind, 1, 40)
	end
	A2FB_Say(kind, A2FB_PlanHead(t) .. " n=" .. A2FB_PLAN_CUR .. " age=" .. age .. A2FB_PlanBudget() .. extra)
	if A2FB_PLAN_FULL and kind ~= "PFAIL" and PlayerObject ~= nil then
		A2FB_Sample("PE", PlayerObject, true)
	end
end

-- Name and line of the function that called ScriptExit (debug library, if the game has it).
function A2FB_Caller()
	if debug == nil or debug.getinfo == nil then
		return " by=nodebug"
	end
	-- levels: 0 getinfo, 1 pcall, 2 this function, 3 the ScriptExit wrapper, 4 the function that called ScriptExit
	local ok, info = pcall(debug.getinfo, 4, "nl")
	if not ok or type(info) ~= "table" then
		return " by=?"
	end
	return " by=" .. tostring(info.name) .. " line=" .. tostring(info.currentline)
end

A2FB_Thread_Orig = @@THREAD@@
function @@THREAD@@(...)
	local ok, err = pcall(A2FB_PlanStart)
	if not ok then
		A2FB_Error("PlanStart", err)
	end
	local r = { A2FB_Thread_Orig(unpack(arg)) }
	local ok2, err2 = pcall(A2FB_PlanEnd, "PRET", "")
	if not ok2 then
		A2FB_Error("PlanRet", err2)
	end
	return unpack(r)
end

-- ScriptExit (PGBase): _ScriptExit() and, inside a thread, coroutine.yield(false). The wrapper records first and then
-- calls the original as a tail call (no pcall around it: a yield must not cross a pcall in Lua 5.0).
A2FB_Exit_Orig = ScriptExit
if A2FB_Exit_Orig ~= nil then
	function ScriptExit()
		local by = A2FB_Caller()
		local ok, err = pcall(A2FB_PlanEnd, "PEXIT", by)
		if not ok then
			A2FB_Error("PlanExit", err)
		end
		return A2FB_Exit_Orig()
	end
end

-- <force>_Production_Failed: wrapped only when the shipped script defines it (an added handler would change what
-- the engine calls). Arguments differ per script (tf, type) or (type); each one's Get_Name() is recorded.
A2FB_Fail_Orig = @@FORCE@@_Production_Failed
if A2FB_Fail_Orig ~= nil then
	@@FORCE@@_Production_Failed = function(...)
		local ok, err = pcall(function()
			local names = ""
			for i = 1, table.getn(arg) do
				local a = arg[i]
				names = names .. " " .. A2FB_Str(function() return a.Get_Name() end)
			end
			A2FB_PlanEnd("PFAIL", " args=" .. table.getn(arg) .. names)
		end)
		if not ok then
			A2FB_Error("PlanFail", err)
		end
		return A2FB_Fail_Orig(unpack(arg))
	end
end
