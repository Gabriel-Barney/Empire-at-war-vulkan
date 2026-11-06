-- ==== modc t13 / t14 probe: script and XML loader leniency (tools/probe_mod/modc_loader/modc_probe_story.lua) ====
-- Galactic plot script of the campaigns "MODC PROBE 1: BOM SCRIPT" and "MODC PROBE 2: CONTROL", installed by
-- build_modc_mod.py as Data\Scripts\Story\Modc_Probe_<Bom|Plain>.lua with @@KIND@@ replaced. The BOM copy starts with
-- the three bytes EF BB BF (UTF-8 byte order mark) in front of this comment; the Plain copy does not.
-- Questions:
--   t13  does the original's script loader skip a UTF-8 BOM? Stock Lua 5.0.2 does not ("unexpected symbol near"
--        the first BOM byte). If the BOM copy runs, its canary is deleted and its screen text shows.
--   t14  does the original's XML parser accept an element name that starts with a digit (workshop mod 1770851727:
--        <181st>, <80s_Visor_Man> as root elements of unit files)? The Plain copy looks up one unit from each probe
--        unit file and deletes that unit's canary when the type exists.
-- Output channel: _CustomScriptMessage(file, text) deletes `file` (relative to corruption\) on its first call
-- (docs/re/LUA_SCRIPT_OUTPUT.md section 2); modc_run.py checks which canaries are gone. Game_Message shows a line too.
require("PGStoryMode")

MODC_KIND = "@@KIND@@"
MODC_DIR = "Mods\\ModcLoaderProbe\\"
MODC_Started = false

function Modc_Canary(name)
	pcall(_CustomScriptMessage, MODC_DIR .. name, "modc probe " .. MODC_KIND)
end

-- Main chunk: runs when the plot loads the script, before any story event.
Modc_Canary("canary_" .. string.lower(MODC_KIND) .. "_script.txt")

function Definitions()
	StoryModeEvents = { Modc_Begin = Modc_State_Begin }
end

function Modc_State_Begin(message)
	if message == OnEnter and not MODC_Started then
		MODC_Started = true
		pcall(Game_Message, "TEXT_MODC_" .. MODC_KIND .. "_RAN")
		if MODC_KIND == "PLAIN" then
			-- type name -> canary (deleted when the type exists)
			local units = {
				{ "Modc_Probe_Plain_Unit", "canary_unit_plain_root.txt", "TEXT_MODC_UNIT_PLAIN" },
				{ "Modc_Probe_Digit_Unit", "canary_unit_digit_root.txt", "TEXT_MODC_UNIT_DIGIT" },
				{ "Modc_Probe_Bom_Digit_Unit", "canary_unit_bom_digit_root.txt", "TEXT_MODC_UNIT_BOM_DIGIT" },
			}
			for _, u in ipairs(units) do
				local ok, t = pcall(Find_Object_Type, u[1])
				if ok and t ~= nil then
					Modc_Canary(u[2])
					pcall(Game_Message, u[3])
				end
			end
		end
		pcall(Game_Message, "TEXT_MODC_DONE")
	end
end
-- ==== end of the modc loader probe ====
