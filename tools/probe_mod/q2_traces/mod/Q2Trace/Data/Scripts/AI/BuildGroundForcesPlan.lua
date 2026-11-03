-- $Id: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/BuildGroundForcesPlan.lua#1 $
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
--              $File: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/BuildGroundForcesPlan.lua $
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

require("pgevents")


function Definitions()	
	Category = "Build_Ground_Forces"
	IgnoreTarget = true
	
	TaskForce = {
	{
		"ReserveForce"
		,"Infantry = 0,2"
		,"Vehicle = 0,2"
		,"Air = 0,2"
		,"-F9TZ_Cloaking_Transport_Company"
		,"-HAV_Juggernaut_Company"
		,"-Gallofree_HTT_Company"		
		,"-Field_Com_Rebel_Team"
		,"-Field_Com_Empire_Team"
	}
	}
	RequiredCategories = { "Vehicle" }
	AllowFreeStoreUnits = false
end

function ReserveForce_Thread()		
	ReserveForce.Set_As_Goal_System_Removable(false)
	BlockOnCommand(ReserveForce.Produce_Force())
	ReserveForce.Set_Plan_Result(true)	
end

-- Q2 reference traces (tools/probe_mod/q2_traces): AI plan probe (q2f t09), appended to an unchanged copy of
-- Data\Scripts\AI\BUILDGROUNDFORCESPLAN.LUA from extracted/foc_data_text.zip. It records when the plan's task-force thread starts and
-- ends (galactic time, frame, faction, target, the units in the task force), each as a Lua string
-- "Q2L1|P|<sid>|<seq>|<KIND>|<text>|END" in Q2L_Records that q2lua_reader.py finds by a read-only memory scan.
-- Plan scripts are pooled (ScriptPoolCount), so one sid covers several plan instances; every record names its plan.
-- It only reads: no GameRandom, no commands, no event handler the original does not define. The wrap is redone each
-- time this chunk runs, always around the thread function the original part above just defined.
Q2P_PLAN = "Build_Ground_Forces"
Q2L_NEEDLE = "Q2L" .. "1|P|"
Q2L_MAX = 4000

if Q2L_Records == nil then
	Q2L_Records = {}
	Q2L_Seq = 0
	Q2L_Sid = string.gsub(tostring(Q2L_Records), "^table: ", "")
	Q2L_Say_Load = true
end

function Q2L_Say(kind, text)
	if Q2L_Seq >= Q2L_MAX then
		return
	end
	Q2L_Seq = Q2L_Seq + 1
	local s = string.gsub(tostring(text), "[^ -~]", "?")
	s = string.gsub(s, "|", "/")
	s = string.sub(s, 1, 320)
	Q2L_Records[Q2L_Seq] = Q2L_NEEDLE .. Q2L_Sid .. "|" .. Q2L_Seq .. "|" .. kind .. "|" .. s .. "|END"
end

function Q2P_Str(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return tostring(v)
	end
	return "?"
end

function Q2P_Units(tf)
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

function Q2P_Record(kind, tf, extra)
	Q2L_Say(kind, "t=" .. Q2P_Str(GetCurrentTime) .. " frame=" .. Q2P_Str(function() return GetCurrentTime.Frame() end) ..
		" plan=" .. Q2P_PLAN .. " player=" .. Q2P_Str(function() return PlayerObject.Get_Faction_Name() end) ..
		" target=" .. Q2P_Str(function() return Target.Get_Game_Object().Get_Type().Get_Name() end) ..
		(extra or "") .. " units=" .. Q2P_Units(tf))
end

if Q2L_Say_Load then
	Q2L_Say_Load = false
	Q2L_Say("LOAD", "plan=" .. Q2P_PLAN)
end

Q2P_Thread = ReserveForce_Thread
function ReserveForce_Thread(...)
	Q2P_Record("PSTART", ReserveForce)
	Q2P_Thread(unpack(arg))
	Q2P_Record("PDONE", ReserveForce)
end
