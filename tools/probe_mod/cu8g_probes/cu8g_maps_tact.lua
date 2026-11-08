-- ==== cu8g t08: tactical map fingerprint (tools/probe_mod/cu8g_probes/cu8g_maps_tact.lua) ====
-- Question (docs/re/CU8F_T03_T05_RE.md sec. 1.7, BATTLE_TRANSITIONS sec. 2.1 step 1.5): when the random map picker
-- finds nothing, the battle map becomes Data\Art\Maps\_Desert_L5_01.ted (land) / _Space_Temperate1.ted (space), which
-- the install does not have. What does the game do: load some map, refuse, auto-resolve, crash?
-- This hook only records that a tactical battle came up at all and which map it is: it rides on the shipped free store
-- of any AI player (no rate change, the AI plays normally) and, at mode frame >= 30 and again >= 600, records for each
-- faction player the object count, the bounding box of the object positions and up to 24 distinct type names. Map
-- objects of the Neutral / Hostile players (props, markers) identify the map; the patched Alderaan copy of the
-- maps-hit session is 3740 x 3740 and has Alderaan props. The galactic story script (cu8g_maps_story.lua) records the
-- campaign side.

C8M = { stage = 0 }
C8M_FACTIONS = { "Rebel", "Empire", "Neutral", "Hostile", "Pirates", "Underworld", "Hutts" }

function C8M_Fingerprint(tag)
	for _, name in ipairs(C8M_FACTIONS) do
		local p = C8_Player(name)
		if p then
			local list = nil
			pcall(function() list = Find_All_Objects_Of_Type(p) end)
			if type(list) ~= "table" then
				C8_Rec("MOBJ", "tag=" .. tag .. "|player=" .. name .. "|count=?")
			else
				local x0, y0, x1, y1 = nil, nil, nil, nil
				local seen, names, nn = {}, "", 0
				for _, o in ipairs(list) do
					local x, y = C8_XYZ(o)
					if type(x) == "number" and type(y) == "number" then
						if x0 == nil or x < x0 then x0 = x end
						if x1 == nil or x > x1 then x1 = x end
						if y0 == nil or y < y0 then y0 = y end
						if y1 == nil or y > y1 then y1 = y end
					end
					local ty = C8_Type(o)
					if not seen[ty] and nn < 24 then
						seen[ty] = true
						nn = nn + 1
						names = names .. ty .. ";"
					end
				end
				C8_Rec("MOBJ", "tag=" .. tag .. "|player=" .. name .. "|count=" .. table.getn(list) .. "|box=" ..
					C8_Num(x0, "%.0f") .. "," .. C8_Num(y0, "%.0f") .. "," .. C8_Num(x1, "%.0f") .. "," ..
					C8_Num(y1, "%.0f") .. "|types=" .. names)
			end
		end
	end
end

function C8M_Tick(F)
	if C8M.stage == 0 and F >= 30 then
		C8M.stage = 1
		C8_Rec("MSTART", "mode=" .. C8.mode .. "|fac=" .. C8.fac)
		C8M_Fingerprint("early")
	elseif C8M.stage == 1 and F >= 600 then
		C8M.stage = 2
		C8M_Fingerprint("later")
		C8_Finish("")
	end
end

C8_PROBES["Land"] = { name = "maps", tick = C8M_Tick, any_ai = true }
C8_PROBES["Space"] = { name = "maps", tick = C8M_Tick, any_ai = true }
-- ==== end of cu8g_maps_tact.lua ====
