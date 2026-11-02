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

-- Q2 reference traces (tools/probe_mod/q2_traces): AI plan probe (q2f t09), appended to an unchanged copy of
-- Data\Scripts\AI\CONQUERPIRATEPLAN.LUA from extracted/foc_data_text.zip. It records when the plan's task-force thread starts and
-- ends (galactic time, frame, faction, target, the units in the task force) and each production failure with the names of its arguments (the type that failed), each as a Lua string
-- "Q2L1|P|<sid>|<seq>|<KIND>|<text>|END" in Q2L_Records that q2lua_reader.py finds by a read-only memory scan.
-- Plan scripts are pooled (ScriptPoolCount), so one sid covers several plan instances; every record names its plan.
-- It only reads: no GameRandom, no commands, no event handler the original does not define. The wrap is redone each
-- time this chunk runs, always around the thread function the original part above just defined.
Q2P_PLAN = "Conquer_Pirate"
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

Q2P_Thread = MainForce_Thread
function MainForce_Thread(...)
	Q2P_Record("PSTART", MainForce)
	Q2P_Thread(unpack(arg))
	Q2P_Record("PDONE", MainForce)
end

Q2P_Failed = MainForce_Production_Failed
function MainForce_Production_Failed(...)
	local names = ""
	for i = 1, table.getn(arg) do
		local a = arg[i]
		names = names .. " " .. Q2P_Str(function() return a.Get_Name() end)
	end
	Q2P_Record("PFAIL", MainForce, " args=" .. table.getn(arg) .. names)
	Q2P_Failed(unpack(arg))
end
