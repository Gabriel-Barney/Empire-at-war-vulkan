-- $Id: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/LandMode/Droids.lua#1 $
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
--              $File: //depot/Projects/StarWars_Steam/FOC/Run/Data/Scripts/AI/LandMode/Droids.lua $
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

function Definitions()
	
	Category = "Droids"
	TaskForce = {
	{
		"MainForce"					
		,"DenyHeroAttach"
		,"Tactical_R2_3PO_Team = 1"
	}
	}
	
	IgnoreTarget = true
	AllowEngagedUnits = true

	turret_seek_range = 500
	repair_seek_range = 500
end

function MainForce_Thread()
	BlockOnCommand(MainForce.Produce_Force())
	
	QuickReinforce(PlayerObject, AITarget, MainForce)

	MainForce.Set_As_Goal_System_Removable(false)
	
	droids = MainForce.Get_Unit_Table()[1]
	if not TestValid(droids) then
		MessageBox("unexpected state; droids unavailable")
	end

	-- Continuously try to use abilities and recharge or heal
	while true do

		ConsiderRepair(droids)

		-- If we're ready and there is a turret nearby, hack it
		if droids.Is_Ability_Ready("TARGETED_HACK") then
			nearest_turret = Find_Nearest(MainForce, "Turret", PlayerObject, false)
			if TestValid(nearest_turret) and MainForce.Get_Distance(nearest_turret) < turret_seek_range then
									 
				BlockOnCommand(MainForce.Activate_Ability("TARGETED_HACK", nearest_turret))
			end
		end

		
		ConsiderRepair(droids)

			
		-- Are we ready to find a vehicle to repair?
		if droids.Is_Ability_Ready("TARGETED_REPAIR") then
					
			-- Try a nearby one first
			repair_target = FindTarget(MainForce, "Needs_Repair", "Friendly_Unit", 1.0, repair_seek_range)
			if  TestValid(repair_target) then
			
				BlockOnCommand(MainForce.Activate_Ability("TARGETED_REPAIR", repair_target))

			else
			
				-- Find any vehicle to repair
				repair_target = FindTarget(MainForce, "Needs_Repair", "Friendly_Unit", 1.0)
				if TestValid(repair_target) then
					BlockOnCommand(MainForce.Activate_Ability("TARGETED_REPAIR", repair_target))
				end
			end
		end
		
		ConsiderRepair(droids)
		
		-- Generally move toward the most significant friendly vehicle
		big_vehicle = FindTarget(MainForce, "Vehicle_Needs_Escort", "Friendly_Unit", 1.0)
		if TestValid(big_vehicle) then
		
			-- Time out frequently, so that we can acquire other targets
			BlockOnCommand(MainForce.Move_To(big_vehicle), 5)
		end

		MainForce.Set_Plan_Result(true)
		
		-- Make sure that they have at least some non-offensive orders (because the above may cause no behavior)
		flee_loc = FindTarget(MainForce, "Space_Area_Is_Hidden", "Tactical_Location", 1.0, 5000.0)
		if flee_loc then
			BlockOnCommand(MainForce.Move_To(flee_loc))
		end

		-- Make sure the loop always yields
		Sleep(1)
	
	end

end

-- Q2 reference traces (tools/probe_mod/q2_traces): AI plan probe (q2f t09, rewritten in q2f t08 2026-11-08, extended
-- 2026-11-08 with target, stage, force count, Conquer_Opponent and Crush), appended by make_plan_hooks.py to an unchanged
-- copy of Data\Scripts\AI\LANDMODE\DROIDS.LUA from extracted/foc_data_text.zip (edit the generator, not this file). Records, each
-- a Lua string "Q2L1|P|<sid>|<seq>|<KIND>|<text>|END" that q2lua_reader.py finds by a read-only memory scan:
--   LOAD    once per script instance (pooled: ScriptPoolCount instances, one sid each); tfs = the task forces hooked
--   PSTART  a task-force thread starts; n = run number of that task force in this instance, prev = how its previous
--           run ended (none = no end record: the thread was killed when the goal system released the plan),
--           tname = Target.Get_Name()
--   PSTAGE  AssembleForce / SynchronizedAssemble was called (wrapped where PGTaskForce defines them), recorded before
--           the call goes through (it blocks in Produce_Force inside): via, strongest = its second argument (when set,
--           the shipped code stages at a FindTarget result instead), stage = the planet of <tf>.Get_Stage() (nil = no
--           stage: the shipped code then calls ScriptExit), stagestr = tostring of it (as the shipped DebugMessage does)
--   PBLOCK  a BlockOnCommand returned (after Produce_Force: the units just built), k = its number in this task force's run
--   PDONE   the thread function returned
--   PEXIT   the plan called ScriptExit (wrapped only where PGBase defines it)
--   PFAIL   the shipped <force>_Production_Failed handler was called (wrapped only where the script defines it)
--   FNEAR   a Find_Nearest call returned (BurnUnitsLand only): fn_src = the task force passed (? = not one of the
--           plan's), fn_count = its Get_Force_Count at the call, fn_filter, fn_ret = the result as type#id@owner or nil.
--           When it is nil and the source is a task force, two more reads with the same source: fn_any = the call
--           without the player (no fog test, any owner: nil means the task force has no position), fn_foe = with the
--           other faction's own player and true (its own objects, under its own fog: set while fn_ret is nil means the
--           plan player's fog or relation test dropped them)
--   ERR     a probe error (each distinct one once); the shipped code still runs
-- Every record carries time, frame, plan, faction, target (Target.Get_Type().Get_Name(): Target is the goal's planet as
-- a game object wrapper; none = the goal has no target), tf (the task force), n, the goal's budget
-- (Budget.Get_Spendable_Resources / Get_Unallocated_Resources, as the a2f kit reads them), the player's credits,
-- count = <tf>.Get_Force_Count() and the task force's unit types (Get_Unit_Table; often 0 in retail even when count is
-- not). A plan with one task force always knows it. With several (Conquer_Opponent) the thread is told by GetThreadID()
-- (PGBase's ScriptExit calls it too; Lua 5.0 has no coroutine.running): each thread start maps its id to its task force.
-- tf=? = not known (no thread, or an id never mapped); count= then lists every task force (SpaceForce:4,GroundForce:0).
--
-- Why it is written this way (2026-11-03 run, docs/re/LUA_SCRIPT_OUTPUT.md "Pooled scripts"):
-- * Releasing a pooled plan runs PGBase Flush_G, which deletes every global TABLE. The old probe kept its records in a
--   global table, so each release lost them, and the next record indexed nil: a Lua error that killed the plan's
--   thread before the shipped code ran, so retail's AI built far less (Rebel fleet departures 71 -> 19). The records
--   and the per task force state now live in tables held only by closures; functions, strings, numbers and booleans
--   survive Flush_G.
-- * Every probe step runs inside pcall, and the shipped functions are called outside it (a yield must not cross a
--   pcall in Lua 5.0), so a probe error costs a record, never the plan.
-- * No math library (retail has none), no '#', no '%'. Only reads: no GameRandom, no commands, no event handler the
--   original does not define. Bindings used are ones that do not raise the script-error helper, called only on the
--   wrapper classes that have them (pcall does not catch a binding error): GameObjectWrapper Get_Type / Get_Name,
--   GameObjectTypeWrapper Get_Name, TaskForce Get_Force_Count / Get_Unit_Table / Get_Stage, AITargetLocationWrapper
--   Get_Game_Object (Get_Stage's result), PlayerWrapper Get_Faction_Name / Get_Credits, GetThreadID.
Q2P_PLAN = "Droids"
Q2P_TFS = "MainForce"
Q2P_ONE = "MainForce"

if Q2L_Say == nil then
	local recs = {}
	local seq = 0
	local errs = {}
	local st = {}
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
	-- Per task force run state ("<tf> n", "<tf> k", "<tf> end") and thread id -> task force ("tid <id>").
	function Q2P_Get(key, default)
		local v = st[key]
		if v == nil then
			return default
		end
		return v
	end
	function Q2P_Set(key, v)
		st[key] = v
	end
	Q2L_Say("LOAD", "plan=" .. Q2P_PLAN .. " tfs=" .. Q2P_TFS)
end

function Q2P_Str(f)
	local ok, v = pcall(f)
	if ok and v ~= nil then
		return tostring(v)
	end
	return "?"
end

-- Runs a probe step; an error becomes one ERR record.
function Q2P_Try(where, f, a, b, c)
	local ok, err = pcall(f, a, b, c)
	if not ok then
		pcall(Q2L_Error, where, err)
	end
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

-- The plan's task force by name (nil for ? or an unknown name).
function Q2P_Force(name)
	if name == "MainForce" then
		return MainForce
	end
	return nil
end

-- The name of a task force object (AssembleForce's argument); ? when it is none of the plan's.
function Q2P_ForceName(force)
	if force == nil then
		return "?"
	end
	if rawequal(force, MainForce) then
		return "MainForce"
	end
	return "?"
end

-- Every task force as name:count, for a record whose task force is not known.
function Q2P_AllCounts()
	local s = ""
	s = s .. ",MainForce:" .. Q2P_Str(function() return MainForce.Get_Force_Count() end)
	return string.sub(s, 2)
end

-- The task force of the running thread.
function Q2P_CurTF()
	if Q2P_ONE ~= "" then
		return Q2P_ONE
	end
	local ok, id = pcall(GetThreadID)
	if not ok or id == nil then
		return "?"
	end
	return Q2P_Get("tid " .. tostring(id), "?")
end

function Q2P_Clean(s)
	return string.gsub(string.sub(tostring(s), 1, 60), " ", "_")
end

function Q2P_Target()
	if AITarget == nil then
		return "none"
	end
	return Q2P_Str(function() return Q2P_Clean(tostring(AITarget)) end)
end

function Q2P_TName()
	if Target == nil then
		return "none"
	end
	return Q2P_Str(function() return Q2P_Clean(tostring(Target)) end)
end

function Q2P_ObjId(o)
	return Q2P_Str(function() return o.Get_Type().Get_Name() end) .. "#" .. Q2P_Str(function() return o.Get_Object_ID() end)
end

function Q2P_Extra()
	local s = ""
	if AITarget ~= nil then
		local ok, o = pcall(function() return AITarget.Get_Game_Object() end)
		if ok and o ~= nil then
			local okv, valid = pcall(TestValid, o)
			if okv and valid then
				s = s .. " tgt=" .. Q2P_ObjId(o)
			else
				s = s .. " tgt=dead"
			end
		end
	end
	if nearest_enemy ~= nil then
		local okv, valid = pcall(TestValid, nearest_enemy)
		if okv and valid then
			s = s .. " near=" .. Q2P_ObjId(nearest_enemy)
		else
			s = s .. " near=dead"
		end
	end
	return s
end

-- skai t12 run 3: perception values at each plan start (PSTART only). Shipped equations the land skirmish goal system
-- itself evaluates for this player every pass, with no target and no '#' draw: nsf = Non_Structure_Force_Unnormalized
-- (Variable_Self.FriendlyForceUnnormalized minus its Structure part), ut = Skirmish_Upgrade_Tech, init =
-- Is_Initial_Skirmish_State_Established, nr = Num_Refineries, sav = Is_Saving_For_Refineries_Land, burn =
-- Should_Burn_Units_Land. ? = the call returned nothing.
function Q2P_Perc()
	return " nsf=" .. Q2P_Str(function() return EvaluatePerception("Non_Structure_Force_Unnormalized", PlayerObject) end) ..
		" ut=" .. Q2P_Str(function() return EvaluatePerception("Skirmish_Upgrade_Tech", PlayerObject) end) ..
		" init=" .. Q2P_Str(function() return EvaluatePerception("Is_Initial_Skirmish_State_Established", PlayerObject) end) ..
		" nr=" .. Q2P_Str(function() return EvaluatePerception("Num_Refineries", PlayerObject) end) ..
		" sav=" .. Q2P_Str(function() return EvaluatePerception("Is_Saving_For_Refineries_Land", PlayerObject) end) ..
		" burn=" .. Q2P_Str(function() return EvaluatePerception("Should_Burn_Units_Land", PlayerObject) end)
end


function Q2P_Record(kind, tf, extra)
	local n = "?"
	if tf ~= "?" then
		n = Q2P_Get(tf .. " n", 0)
	end
	local force = Q2P_Force(tf)
	local tail
	if force ~= nil then
		tail = " count=" .. Q2P_Str(function() return force.Get_Force_Count() end) .. " units=" .. Q2P_Units(force)
	elseif tf == "?" then
		tail = " count=" .. Q2P_AllCounts() .. " units=?"
	else
		tail = " count=nil units=nil"
	end
	Q2L_Say(kind, "t=" .. Q2P_Str(GetCurrentTime) .. " frame=" .. Q2P_Str(function() return GetCurrentTime.Frame() end) ..
		" plan=" .. Q2P_PLAN .. " player=" .. Q2P_Str(function() return PlayerObject.Get_Faction_Name() end) ..
		" target=" .. Q2P_Target() .. Q2P_Extra() .. " tf=" .. tf .. " n=" .. n .. (extra or "") ..
		" spend=" .. Q2P_Str(function() return Budget.Get_Spendable_Resources() end) ..
		" unalloc=" .. Q2P_Str(function() return Budget.Get_Unallocated_Resources() end) ..
		" cr=" .. Q2P_Str(function() return PlayerObject.Get_Credits() end) .. tail)
end

-- Never raises: a failing record becomes one ERR record.
function Q2P_Safe(kind, tf, extra)
	Q2P_Try(kind, Q2P_Record, kind, tf, extra)
end

function Q2P_Ended(tf, kind)
	local e = Q2P_Get(tf .. " end", "first")
	if e == "none" then
		e = kind
	else
		e = string.sub(e .. "+" .. kind, 1, 40)
	end
	Q2P_Set(tf .. " end", e)
end

function Q2P_EndAll(kind)
	Q2P_Ended("MainForce", kind)
end

function Q2P_Begin(tf)
	local prev = Q2P_Get(tf .. " end", "first")
	Q2P_Set(tf .. " n", Q2P_Get(tf .. " n", 0) + 1)
	Q2P_Set(tf .. " k", 0)
	Q2P_Set(tf .. " end", "none")
	if Q2P_ONE == "" then
		local ok, id = pcall(GetThreadID)
		if ok and id ~= nil then
			Q2P_Set("tid " .. tostring(id), tf)
		end
	end
	Q2P_Safe("PSTART", tf, " prev=" .. prev .. " tname=" .. Q2P_TName() .. Q2P_Perc())
end

function Q2P_Done(tf)
	Q2P_Ended(tf, "PDONE")
	Q2P_Safe("PDONE", tf)
end

function Q2P_Blocked(tf, v)
	local k = "?"
	if tf ~= "?" then
		k = Q2P_Get(tf .. " k", 0) + 1
		Q2P_Set(tf .. " k", k)
	end
	local ret = type(v)
	if ret == "boolean" then
		ret = tostring(v)
	end
	Q2P_Safe("PBLOCK", tf, " k=" .. k .. " ret=" .. ret)
end

function Q2P_Stage(via, force, strongest)
	local tf = Q2P_ForceName(force)
	if tf == "?" then
		tf = Q2P_CurTF()
	end
	local stage = "nil"
	local str = "nil"
	if force == nil then
		stage = "?"
		str = "?"
	else
		local ok, s = pcall(function() return force.Get_Stage() end)
		if not ok then
			stage = "?"
			str = "?"
		elseif s ~= nil then
			stage = Q2P_Str(function() return s.Get_Game_Object().Get_Type().Get_Name() end)
			str = string.gsub(string.sub(Q2P_Str(function() return tostring(s) end), 1, 40), " ", "_")
		end
	end
	Q2P_Safe("PSTAGE", tf, " via=" .. via .. " strongest=" .. tostring(strongest) .. " stage=" .. stage ..
		" stagestr=" .. str)
end

function Q2P_Exiting()
	local tf = Q2P_CurTF()
	Q2P_EndAll("PEXIT")
	Q2P_Safe("PEXIT", tf)
end

-- Event arguments are [tf, params...] (AI_GALACTIC 1.9.4): the first is the task force, which has no Get_Name; the
-- others (the type that failed) are recorded by their Get_Name().
function Q2P_Fail(tf, args)
	local s = " args=" .. table.getn(args)
	for i = 1, table.getn(args) do
		local a = args[i]
		if i == 1 then
			s = s .. " tf"
		else
			s = s .. " " .. Q2P_Str(function() return a.Get_Name() end)
		end
	end
	Q2P_Ended(tf, "PFAIL")
	Q2P_Safe("PFAIL", tf, s)
end

-- The thread wraps are redone each time this chunk runs, always around the thread function the shipped part above
-- just defined. Flush_G keeps functions, so the wraps stay across releases.
Q2P_Thread_MainForce = MainForce_Thread
function MainForce_Thread(...)
	Q2P_Try("PSTART", Q2P_Begin, "MainForce")
	local r = { Q2P_Thread_MainForce(unpack(arg)) }
	Q2P_Try("PDONE", Q2P_Done, "MainForce")
	return unpack(r)
end

-- BlockOnCommand (PGBase) yields inside: the wrapper calls it outside any pcall, then records. Wrapped once per script
-- instance (require does not run PGBase again).
if Q2P_Block == nil and BlockOnCommand ~= nil then
	Q2P_Block = BlockOnCommand
	function BlockOnCommand(...)
		local ok, tf = pcall(Q2P_CurTF)
		if not ok then
			tf = "?"
		end
		local r = { Q2P_Block(unpack(arg)) }
		Q2P_Try("PBLOCK", Q2P_Blocked, tf, r[1])
		return unpack(r)
	end
end

-- AssembleForce / SynchronizedAssemble (PGTaskForce) set the global stage and block in Produce_Force(stage): the
-- wrapper records the stage first, then a tail call. Wrapped once per script instance, like BlockOnCommand.
if Q2P_Asm == nil and AssembleForce ~= nil then
	Q2P_Asm = AssembleForce
	function AssembleForce(...)
		Q2P_Try("PSTAGE", Q2P_Stage, "AssembleForce", arg[1], arg[2])
		return Q2P_Asm(unpack(arg))
	end
end
if Q2P_SyncAsm == nil and SynchronizedAssemble ~= nil then
	Q2P_SyncAsm = SynchronizedAssemble
	function SynchronizedAssemble(...)
		Q2P_Try("PSTAGE", Q2P_Stage, "SynchronizedAssemble", arg[1], arg[2])
		return Q2P_SyncAsm(unpack(arg))
	end
end

-- ScriptExit (PGBase): _ScriptExit() and, in a thread, coroutine.yield(false); the whole plan ends. Record first, then
-- a tail call.
if Q2P_Exit == nil and ScriptExit ~= nil then
	Q2P_Exit = ScriptExit
	function ScriptExit()
		Q2P_Try("PEXIT", Q2P_Exiting)
		return Q2P_Exit()
	end
end
