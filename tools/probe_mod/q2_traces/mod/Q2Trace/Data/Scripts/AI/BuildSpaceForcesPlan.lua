-- $Id: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/BuildSpaceForcesPlan.lua#1 $
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
--              $File: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/BuildSpaceForcesPlan.lua $
--
--    Original Author: Steve_Copeland
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

require("pgevents")

-- Tell the script pooling system to pre-cache this number of scripts.
ScriptPoolCount = 4

function Definitions()
	Category = "Build_Space_Forces"
	IgnoreTarget = true
	
	--Fighters are omitted deliberately.  Since they're cheap, build fast and are quickly killed it's typically better
	--to build them as we need them to attack.  For defensive purposes we'll rely on space station garrisons for our fighter
	--needs
	TaskForce = {
	{
		"ReserveForce"
		,"DenyHeroAttach"
		,"Corvette = 0,4"
		,"Frigate = 0,4"
		,"Capital = 0,4"
	}
	}
	RequiredCategories = { "Corvette | Frigate | Capital" }
	AllowFreeStoreUnits = false
end

function ReserveForce_Thread()	
	ReserveForce.Set_As_Goal_System_Removable(false)
	BlockOnCommand(ReserveForce.Produce_Force())
	ReserveForce.Set_Plan_Result(true)
end

-- Q2 reference traces (tools/probe_mod/q2_traces): AI plan probe (q2f t09, rewritten in q2f t08 2026-11-08), appended
-- to an unchanged copy of Data\Scripts\AI\BUILDSPACEFORCESPLAN.LUA from extracted/foc_data_text.zip. Records, each a Lua string
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
Q2P_PLAN = "Build_Space_Forces"

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
		" units=" .. Q2P_Units(ReserveForce))
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
Q2P_Thread = ReserveForce_Thread
function ReserveForce_Thread(...)
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
