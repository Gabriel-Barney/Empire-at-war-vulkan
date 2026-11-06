-- ==== a2j t02 (D3) production probe: GameScoring hook (channel Q) ====
-- Appended (after the generated header and a2fb_core.lua) to an unchanged copy of Data\Scripts\Miscellaneous\
-- GameScoring.lua. The engine calls that script's Galactic_Production_Begin_Event(planet, object_type),
-- Galactic_Production_Canceled_Event(planet, object_type) and Galactic_Production_End_Event(planet, object) for every
-- player's galactic production (the names are exe strings, LUA_API.md). The Lua API has no query for a planet's build
-- queue, so these events are the probe's view of the queue heads: one record per event, then the shipped function
-- runs exactly as before.
--   QB  begin (the item was scheduled at that planet)    QC  cancelled    QE  finished
-- Fields: t f fac=<planet owner> pl=<planet type> type=<object type> cost=<Get_Build_Cost, type wrappers only>
-- cr=<the owner's Get_Credits when the event runs (before or after the engine's charge is not known: compare with the
-- story channel's credit samples)>.
-- Every call here is one the shipped GameScoring makes on the same arguments (planet.Get_Type().Get_Name(),
-- planet.Get_Owner(), object_type.Get_Name(), Get_Build_Cost(), the Get_Type == nil test), plus Get_Faction_Name and
-- Get_Credits on a player and GetCurrentTime, which the other channels (and shipped AI Lua, for Get_Credits) use: no read
-- that can fail inside the engine. No perceptual equation is evaluated here, so no AI leaf cache is touched.
A2FB_MAX = 6000
A2FB_CANARY = "Mods\\A2fBudgetProbe\\A2FB_Canary_Scoring.txt"

A2FB_Say("LOAD", "GameScoring production hook chunk ran")
A2FB_Canary(A2FB_CANARY)

function A2FB_Q_Name(o)
	if o == nil then
		return "nil"
	end
	if o.Get_Type == nil then
		return A2FB_Str(function() return o.Get_Name() end)
	end
	return A2FB_Str(function() return o.Get_Type().Get_Name() end)
end

function A2FB_Q_Rec(kind, planet, o)
	A2FB_Rec(kind, function()
		local t = A2FB_Now()
		local pn, own, cr = "nil", "nil", "nil"
		if planet ~= nil then
			pn = A2FB_Str(function() return planet.Get_Type().Get_Name() end)
			own = A2FB_Str(function() return string.upper(planet.Get_Owner().Get_Faction_Name()) end)
			cr = A2FB_Str(function() return planet.Get_Owner().Get_Credits() end)
		end
		local cost = "-"
		if o ~= nil and o.Get_Type == nil then
			cost = A2FB_Str(function() return o.Get_Build_Cost() end)
		end
		return "t=" .. A2FB_Num(t) .. " f=" .. A2FB_Frame(t) .. " fac=" .. own .. " pl=" .. pn .. " type=" .. A2FB_Q_Name(o) ..
			" cost=" .. cost .. " cr=" .. cr
	end)
end

A2FB_Q_Begin = Galactic_Production_Begin_Event
function Galactic_Production_Begin_Event(planet, object_type)
	A2FB_Q_Rec("QB", planet, object_type)
	if A2FB_Q_Begin then
		return A2FB_Q_Begin(planet, object_type)
	end
end

A2FB_Q_Cancel = Galactic_Production_Canceled_Event
function Galactic_Production_Canceled_Event(planet, object_type)
	A2FB_Q_Rec("QC", planet, object_type)
	if A2FB_Q_Cancel then
		return A2FB_Q_Cancel(planet, object_type)
	end
end

A2FB_Q_End = Galactic_Production_End_Event
function Galactic_Production_End_Event(planet, object)
	A2FB_Q_Rec("QE", planet, object)
	if A2FB_Q_End then
		return A2FB_Q_End(planet, object)
	end
end
