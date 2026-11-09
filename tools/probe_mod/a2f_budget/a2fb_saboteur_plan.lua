-- ==== a2k t05 saboteur probe: AI_Plan_Underworld_DeploySaboteur extension of the plan hook (channel P) ====
-- Appended after a2fb_plan_hook.lua (build_a2f_mod.PLAN_EXTRA) to the unchanged shipped plan script, whose thread is
--   AssembleForce(MainForce)                      -- PGTaskForce: stage = MainForce.Get_Stage(); Produce_Force(stage)
--   BlockOnCommand(MainForce.Move_To(Target))
--   MainForce.Activate_Ability() ; Sleep(10) ; Set_Plan_Result(true) if Corrupt_Planet and Target.Is_Corrupted()
-- and whose MainForce_Production_Failed calls ScriptExit (docs/re/A2K_T08_RE_ANSWERS.md section 5, Q5 items 1 - 3).
-- Adds, read-only:
--   PSTART  + goal= (MainForce.Get_Goal_Type_Name) tgt= (Target type name) tgtc= (Target.Is_Corrupted)
--   PRET / PEXIT / PFAIL  + the same three keys and blk= (BlockOnCommand calls of this run)
--   PSTAGE  first BlockOnCommand of the run (inside AssembleForce): stage= the planet AssembleForce got from
--           MainForce.Get_Stage() (the script's own global `stage`, no extra engine call), blk= type of the
--           Produce_Force status (nil = Produce_Force returned nothing, BlockOnCommand then returns nil at once)
--   PPROD   its result (BlockOnCommand's return: true / false / nil) and dt= seconds it blocked
--   PMOVS   second BlockOnCommand (Move_To(Target)): tgt=, stage2= MainForce.Get_Stage() again, units=
--           MainForce.Get_Force_Count(), blk= type of the Move_To status
--   PMOVE   its result and dt=
-- BlockOnCommand (PGBase) yields; the wrapper calls the original directly (no pcall around it: a yield must not cross
-- a pcall in Lua 5.0) with the same arguments and returns its value, so the plan behaves as shipped. Every probe read
-- is one the shipped script or the shipped AI library already makes in a plan (Get_Goal_Type_Name, Is_Corrupted,
-- Get_Type().Get_Name, Get_Stage, Get_Force_Count): a binding error would end the plan script, so nothing else is read.
-- The goal's desire is not readable from a plan (no Lua API); channel U (a2fb_saboteur.lua) records the shipped
-- desire equations per target instead.
A2FB_SAB_BLK = 0
A2FB_SAB_BT = -1

function A2FB_SabName(o)
	if o == nil then
		return "nil"
	end
	return A2FB_Str(function() return o.Get_Type().Get_Name() end)
end

function A2FB_SabKeys()
	return " goal=" .. A2FB_Str(function() return MainForce.Get_Goal_Type_Name() end) ..
		" tgt=" .. A2FB_SabName(Target) ..
		" tgtc=" .. A2FB_Str(function() return Target.Is_Corrupted() end)
end

function A2FB_PlanStartX()
	A2FB_SAB_BLK = 0
	return A2FB_SabKeys()
end

function A2FB_PlanEndX(kind)
	return A2FB_SabKeys() .. " blk=" .. A2FB_SAB_BLK
end

function A2FB_SabHead(t)
	return A2FB_PlanHead(t) .. " n=" .. A2FB_PLAN_CUR
end

function A2FB_SabPre(block)
	A2FB_SAB_BLK = A2FB_SAB_BLK + 1
	local t = A2FB_Now()
	A2FB_SAB_BT = t
	if A2FB_SAB_BLK == 1 then
		A2FB_Rec("PSTAGE", function()
			return A2FB_SabHead(t) .. " stage=" .. A2FB_SabName(stage) .. " blk=" .. type(block)
		end)
	elseif A2FB_SAB_BLK == 2 then
		A2FB_Rec("PMOVS", function()
			return A2FB_SabHead(t) .. " tgt=" .. A2FB_SabName(Target) ..
				" stage2=" .. A2FB_Str(function() return MainForce.Get_Stage().Get_Type().Get_Name() end) ..
				" units=" .. A2FB_Str(function() return MainForce.Get_Force_Count() end) .. " blk=" .. type(block)
		end)
	end
	return A2FB_SAB_BLK
end

function A2FB_SabPost(k, r)
	local t = A2FB_Now()
	local dt = "nil"
	if A2FB_SAB_BT >= 0 and t >= 0 then
		dt = A2FB_Num(t - A2FB_SAB_BT)
	end
	local kind = "PBLK"
	if k == 1 then
		kind = "PPROD"
	elseif k == 2 then
		kind = "PMOVE"
	end
	A2FB_Rec(kind, function()
		return A2FB_SabHead(t) .. " k=" .. tostring(k) .. " res=" .. tostring(r) .. " dt=" .. dt
	end)
end

-- Wrapped once per script instance (PGBase is a library: require does not run it again; LUA_SCRIPT_OUTPUT.md section 4
-- rule 4). The wrapper's state is numbers only, which survive the Flush_G of a plan release.
if A2FB_Block_Orig == nil and BlockOnCommand ~= nil then
	A2FB_Block_Orig = BlockOnCommand
	function BlockOnCommand(block, max_duration, alternate_break_func)
		local okp, k = pcall(A2FB_SabPre, block)
		if not okp then
			pcall(A2FB_Error, "SabPre", k)
			k = -1
		end
		local r = A2FB_Block_Orig(block, max_duration, alternate_break_func)
		local ok2, err2 = pcall(A2FB_SabPost, k, r)
		if not ok2 then
			pcall(A2FB_Error, "SabPost", err2)
		end
		return r
	end
end
A2FB_Say("SABLOAD", "plan extension loaded block_wrapped=" .. tostring(A2FB_Block_Orig ~= nil))
