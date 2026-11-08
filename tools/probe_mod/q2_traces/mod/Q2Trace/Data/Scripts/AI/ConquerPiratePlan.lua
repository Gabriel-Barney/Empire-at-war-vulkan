-- $Id: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/ConquerPiratePlan.lua#1 $
--/////////////////////////////////////////////////////////////////////////////////////////////////
--
-- (C) Petroglyph Games, Inc.
--
--
--  *****           **                          *                   *
--  *   **          *                           *                   *
--  *    *          *                           *                   *
--  *    *          *     *                 *   *          *        *
--  *   *     *** ******  * **  ****      ***   * *      * *****    * ***
--  *  **    *  *   *     **   *   **   **  *   *  *    * **   **   **   *
--  ***     *****   *     *   *     *  *    *   *  *   **  *    *   *    *
--  *       *       *     *   *     *  *    *   *   *  *   *    *   *    *
--  *       *       *     *   *     *  *    *   *   * **   *   *    *    *
--  *       **       *    *   **   *   **   *   *    **    *  *     *   *
-- **        ****     **  *    ****     *****   *    **    ***      *   *
--                                          *        *     *
--                                          *        *     *
--                                          *       *      *
--                                      *  *        *      *
--                                      ****       *       *
--
--/////////////////////////////////////////////////////////////////////////////////////////////////
-- C O N F I D E N T I A L   S O U R C E   C O D E -- D O   N O T   D I S T R I B U T E
--/////////////////////////////////////////////////////////////////////////////////////////////////
--
--              $File: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/ConquerPiratePlan.lua $
--
--    Original Author: James Yarrow
--
--            $Author: Brian_Hayes $
--
--            $Change: 637819 $
--
--          $DateTime: 2017/03/22 10:16:16 $
--
--          $Revision: #1 $
--
--/////////////////////////////////////////////////////////////////////////////////////////////////

require("pgtaskforce")

-- Tell the script pooling system to pre-cache this number of scripts.
ScriptPoolCount = 4

function Definitions()	
	MinContrastScale = 1.1
	MaxContrastScale = 1.15
		
	Category = "Conquer_Pirate"
	TaskForce = {
	{
		"MainForce"						
		, "Infantry | Vehicle | Air | Fighter | Bomber | Corvette | Frigate | Super | Capital = 100%"
	}
	}
	RequiredCategories = { "Air | Infantry | Vehicle" }	
	
	LandSecured = false
end

function MainForce_Thread()
	
	-- Since we're using plan failure to adjust contrast, we're 
	-- only concerned with failures in battle. Default the 
	-- plan to successful and then 
	-- only on the event of our task force being killed is the
	-- plan set to a failed state.
	MainForce.Set_Plan_Result(true)	
	
	if MainForce.Are_All_Units_On_Free_Store() == true then
		AssembleForce(MainForce)
	else
		BlockOnCommand(MainForce.Produce_Force());
		return
	end
	
	BlockOnCommand(MainForce.Move_To(Target))
	if MainForce.Get_Force_Count() == 0 then
		MainForce.Set_Plan_Result(false)	
		ScriptExit()
	end
	if Invade(MainForce) == false then
		MainForce.Set_Plan_Result(false)			
		ScriptExit()
	end
	
	LandSecured = true
	MainForce.Release_Forces(1.0)
	FundBases(PlayerObject, Target)
		
	ScriptExit()
end

function MainForce_Production_Failed(failed_object_type)
	ScriptExit()
end

function MainForce_Original_Target_Owner_Changed(tf, old_owner, new_owner)	
	--Ignore changes to neutral - it might just be temporary on the way to
	--passing into my control.
	if new_owner ~= PlayerObject and new_owner.Is_Neutral() == false then
		ScriptExit()
	end
end

function MainForce_No_Units_Remaining()
	if not LandSecured then
		MainForce.Set_Plan_Result(false)			
		ScriptExit()
	end
end

-- Q2 reference traces (tools/probe_mod/q2_traces): AI plan probe (q2f t09, rewritten in q2f t08 2026-11-08), appended
-- to an unchanged copy of Data\Scripts\AI\CONQUERPIRATEPLAN.LUA from extracted/foc_data_text.zip. Records, each a Lua string
-- "Q2L1|P|<sid>|<seq>|<KIND>|<text>|END" that q2lua_reader.py finds by a read-only memory scan:
--   LOAD    once per script instance (pooled: ScriptPoolCount instances, one sid each)
--   PSTART  the task-force thread starts; n = run number in this instance, prev = how the previous run ended
--           (none = no end record: the thread was killed when the goal system released the plan)
--   PBLOCK  a BlockOnCommand returned (after Produce_Force: the units just built), k = its number in this run
--   PDONE   the thread function returned
--   PEXIT   the plan called ScriptExit (wrapped only where PGBase defines it)
--   PFAIL   the shipped <force>_Production_Failed handler was called (wrapped only where the script defines it)
--   ERR     a probe error (each distinct one once); the shipped code still runs
-- Every record carries time, frame, plan, faction, target, the goal's budget (Budget.Get_Spendable_Resources /
-- Get_Unallocated_Resources, as the a2f kit reads them), the player's credits and the task force's unit types.
--
-- Why it is written this way (2026-11-03 run, docs/re/LUA_SCRIPT_OUTPUT.md "Pooled scripts"):
-- * Releasing a pooled plan runs PGBase Flush_G, which deletes every global TABLE. The old probe kept its records in a
--   global table, so each release lost them, and the next record indexed nil: a Lua error that killed the plan's
--   thread before the shipped code ran, so retail's AI built far less (Rebel fleet departures 71 -> 19). The records
--   now live in a table held only by Q2L_Say's closure; functions, strings, numbers and booleans survive Flush_G.
-- * Every probe step runs inside pcall, and the shipped functions are called outside it (a yield must not cross a
--   pcall in Lua 5.0), so a probe error costs a record, never the plan.
-- * No math library (retail has none), no '#', no '%'. Only reads: no GameRandom, no commands, no event handler the
--   original does not define. Bindings used are ones that do not raise the script-error helper.
Q2P_PLAN = "Conquer_Pirate"

if Q2L_Say == nil then
	local recs = {}
	local seq = 0
	local errs = {}
	local needle = "Q2L" .. "1|P|"
	local sid = string.gsub(tostring(recs), "^table: ", "")
	function Q2L_Say(kind, text)
		if seq >= 4000 then
			return
		end
		seq = seq + 1
		local s = string.gsub(tostring(text), "[^ -~]", "?")
		s = string.gsub(s, "|", "/")
		s = string.sub(s, 1, 320)
		recs[seq] = needle .. sid .. "|" .. seq .. "|" .. kind .. "|" .. s .. "|END"
	end
	function Q2L_Error(where, err)
		local key = string.sub(where .. " " .. tostring(err), 1, 200)
		if errs[key] == nil then
			errs[key] = true
			Q2L_Say("ERR", key)
		end
	end
	function Q2L_Count()
		return seq
	end
	function Q2L_Get(i)
		return recs[i]
	end
	Q2P_N = 0
	Q2P_K = 0
	Q2P_End = "first"
	Q2L_Say("LOAD", "plan=" .. Q2P_PLAN)
end

function Q2P_Str(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return tostring(v)
	end
	return "?"
end

function Q2P_Units(tf)
	if tf == nil then
		return "nil"
	end
	local ok, t = pcall(function() return tf.Get_Unit_Table() end)
	if not ok or type(t) ~= "table" then
		return "?"
	end
	local n = 0
	local s = ""
	for _, u in pairs(t) do
		n = n + 1
		s = s .. " " .. Q2P_Str(function() return u.Get_Type().Get_Name() end)
	end
	return n .. s
end

function Q2P_Record(kind, extra)
	Q2L_Say(kind, "t=" .. Q2P_Str(GetCurrentTime) .. " frame=" .. Q2P_Str(function() return GetCurrentTime.Frame() end) ..
		" plan=" .. Q2P_PLAN .. " player=" .. Q2P_Str(function() return PlayerObject.Get_Faction_Name() end) ..
		" target=" .. Q2P_Str(function() return Target.Get_Game_Object().Get_Type().Get_Name() end) ..
		" n=" .. Q2P_N .. (extra or "") ..
		" spend=" .. Q2P_Str(function() return Budget.Get_Spendable_Resources() end) ..
		" unalloc=" .. Q2P_Str(function() return Budget.Get_Unallocated_Resources() end) ..
		" cr=" .. Q2P_Str(function() return PlayerObject.Get_Credits() end) ..
		" units=" .. Q2P_Units(MainForce))
end

-- Never raises: a failing record becomes one ERR record.
function Q2P_Safe(kind, extra)
	local ok, err = pcall(Q2P_Record, kind, extra)
	if not ok then
		pcall(Q2L_Error, kind, err)
	end
end

function Q2P_Ended(kind)
	if Q2P_End == "none" then
		Q2P_End = kind
	else
		Q2P_End = string.sub(Q2P_End .. "+" .. kind, 1, 40)
	end
end

-- The thread wrap is redone each time this chunk runs, always around the thread function the shipped part above just
-- defined. Flush_G keeps functions, so the wrap stays across releases.
Q2P_Thread = MainForce_Thread
function MainForce_Thread(...)
	local prev = Q2P_End
	Q2P_N = Q2P_N + 1
	Q2P_K = 0
	Q2P_End = "none"
	Q2P_Safe("PSTART", " prev=" .. prev)
	local r = { Q2P_Thread(unpack(arg)) }
	Q2P_Ended("PDONE")
	Q2P_Safe("PDONE")
	return unpack(r)
end

-- BlockOnCommand (PGBase) yields inside: the wrapper calls it outside any pcall, then records. Wrapped once per script
-- instance (require does not run PGBase again).
if Q2P_Block == nil and BlockOnCommand ~= nil then
	Q2P_Block = BlockOnCommand
	function BlockOnCommand(...)
		local r = { Q2P_Block(unpack(arg)) }
		Q2P_K = Q2P_K + 1
		local v = r[1]
		local ret = type(v)
		if ret == "boolean" then
			ret = tostring(v)
		end
		Q2P_Safe("PBLOCK", " k=" .. Q2P_K .. " ret=" .. ret)
		return unpack(r)
	end
end

-- ScriptExit (PGBase): _ScriptExit() and, in a thread, coroutine.yield(false). Record first, then a tail call.
if Q2P_Exit == nil and ScriptExit ~= nil then
	Q2P_Exit = ScriptExit
	function ScriptExit()
		Q2P_Ended("PEXIT")
		Q2P_Safe("PEXIT")
		return Q2P_Exit()
	end
end

-- <force>_Production_Failed: wrapped only because the shipped script defines it; redone each chunk run like the thread.
-- Arguments are recorded by their Get_Name() (the type that failed); the shipped handler runs as a tail call.
Q2P_Failed = MainForce_Production_Failed
function MainForce_Production_Failed(...)
	local ok, names = pcall(function()
		local s = " args=" .. table.getn(arg)
		for i = 1, table.getn(arg) do
			local a = arg[i]
			s = s .. " " .. Q2P_Str(function() return a.Get_Name() end)
		end
		return s
	end)
	if not ok then
		names = " args=?"
	end
	Q2P_Ended("PFAIL")
	Q2P_Safe("PFAIL", names)
	return Q2P_Failed(unpack(arg))
end
