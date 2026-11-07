# Script and XML loader leniency for mods (modc t13 / t14)

Question (roadmap `modc` t13, t14): the load report (`alamo_mod_check`, `docs/MODS.md` §5) found three kinds of problems
in installed workshop mods that the original might load anyway. For each: does the original load it (then the port
has a gap and fixes the general mechanism) or does the mod ship something the original cannot use either (then the
port keeps reporting it)?

Answer, from an in-game probe on Steam FoC x64 1.121 (2026-11-06, kit `tools/probe_mod/modc_loader/`, run by Claude
with computer use at Gabriel's request):

| # | Finding | Original | Port before | Port now |
|---|---|---|---|---|
| 1 | Lua script starting with a UTF-8 BOM | **does not load it** [H] | compile error, reported | unchanged (reported) |
| 2 | XML element name starting with a digit (`<181st>`, `<80s_Visor_Man>`) | **loads it** and the types inside [H] | file root lost, types not findable | parsed as an element |
| 3 | Campaign without planets (`Locations`) | **listed in the Galactic Conquest menu** [H]; starting it not tested | loaded, report error | loaded; report level: see §4 |

Confidence: [H] = seen in the probe with a control; nothing here comes from the decompilation.

## 1. Probe

Mod `Mods\ModcLoaderProbe`, launched with `MODPATH=Mods\ModcLoaderProbe` (README of the kit). Output channel: the
probe scripts call `_CustomScriptMessage(file, text)`, which deletes `file` on its first call
([`LUA_SCRIPT_OUTPUT.md`](LUA_SCRIPT_OUTPUT.md) §2); the kit checks which canary files are gone.

| Check | Built from | Result |
|---|---|---|
| control: plain story script (`MODC PROBE 2`) runs its main chunk | `Modc_Probe_Plain.lua`, no BOM | canary deleted: hook works |
| t13: the same script with EF BB BF in front (`MODC PROBE 1`) | `Modc_Probe_Bom.lua` | canary **present** after about 20 s of galactic time; the plot itself loaded (its story-XML `SCREEN_TEXT` line showed) |
| control: unit from a normal-root unit file | `<Modc_Probe_Units>` | `Find_Object_Type` finds it |
| t14: unit from a file whose root is `<80s_Modc_Probe_Units>` (as `Units_Hero_Minors_CSA_Tagge.xml`, no BOM) | | **found** |
| t14: unit from a BOM file whose root is `<181st_Modc_Probe_Units>` (as `Units_Hero_Empire_181st_Fighter_Wing.xml`) | | **found** |
| t14: campaign file with root `<1Modc_Probe_Campaigns>` (`MODC PROBE 3`) | | **listed**, and its galaxy preview shows its planets (the whole campaign parsed) |
| t14: campaign with only menu fields (`MODC PROBE 4`: Campaign_Set, Sort_Order, Text_ID, Description_Text, Starting_Active_Player) | as `Campaigns_Dummies.xml` | **listed**, empty galaxy preview; not started |

Run record: `tools/probe_mod/modc_loader/out/<run>/report.txt` (not committed). The two scripts differ only in the
BOM and the `@@KIND@@` string, so the BOM alone stopped the script.

## 2. Lua: UTF-8 BOM (t13)

- The original statically links Lua 5.0.2 ([`PE_OVERVIEW.md`](PE_OVERVIEW.md) "Lua"). Stock 5.0.2 has no BOM handling
  (`toolchain/lua-5.0.2/src/lib/lauxlib.c`), and the original's script loader adds none: the BOM
  script never ran. Its error goes to the unset script-error callback ([`LUA_SCRIPT_OUTPUT.md`](LUA_SCRIPT_OUTPUT.md)
  §1), so a player sees nothing; the script just does not run.
- No shipped FoC or EaW script has a BOM (`extracted/*_data_text.zip`, 0 of every `.lua`), while 59 shipped FoC XML files
  do, and those load.
- Port: unchanged. `alamo_mod_check` keeps listing the script as "does not compile" (warning, `MODS.md` §5 "A Lua script
  does not compile"), with the compiler message `unexpected symbol near` the first BOM byte.
- Workshop 2858787558: 2 such files, both `Data\Scripts\AI\SpaceMode\BuildStructureSpace.lua` (one in a
  `_backups_<date>` folder, which no root searches). In the original that AI plan does not run either. The same mod's
  other 95 "does not compile" findings are `Data\Scripts\Story\*.lua` shipped as retail 32-bit bytecode ("bad
  signature"): the x64 original rejects them too ([`SUBSYSTEMS.md`](SUBSYSTEMS.md) "Lua loading"), so the loose copies
  hide the text sources in `64Patch.meg` and those story scripts do not run in the original. `MODS.md` §4 lists 32-bit
  bytecode as not supported.

## 3. XML: element names that start with a digit (t14)

- The original's XML reader (its own `XMLDatabase` classes, RTTI `XMLNodeClass@XMLDatabase`) accepts a name that starts
  with a digit, at least as the root element, and reads the types inside as usual (probe rows above). XML 1.0 forbids
  it; the port followed XML 1.0, treated `<181st>` as text, made the first child the root and lost the file's types.
- Port change: `engine/data/src/xml.cpp` `is_name_start` accepts `0`-`9`. `<5...` is now markup; `< 5` (with a space)
  stays text, as before. No shipped FoC / EaW XML has `<` followed by a digit, and of the 13 installed workshop mods only
  1770851727 does (its two unit files and a `_BU` backup that no list names), so base data and goldens are unaffected.
  Test: `test_data.cpp` `test_xml_recovery` "element names may start with a digit".
- Workshop 1770851727: `Units_Hero_Empire_181st_Fighter_Wing.xml` (BOM + `<181st>`: 14 types, the 181st hero chain
  that its story timers spawn and upgrade) and `Units_Hero_Minors_CSA_Tagge.xml` (`<80s_Visor_Man>`: `Tagge_Lucrehulk`)
  now load; the "'<' does not start markup" recoveries and the "without a Name" lines are gone.
- Not tested: a digit-led name for a nested element, other characters (`-`, `.`) at the start of a name. The port
  accepts digits at every level.

## 4. Campaigns without planets (t14)

- Workshop 1770851727 `Campaigns_Dummies.xml` defines 8 campaigns with menu fields only: `Web_Way_Casual` (Sort_Order 2),
  `Web_Way` (5), `Web_Way_AI` (15), `Dummy1` (20), `Dummy2` (21), `Language` / `Language1` / `Language2` (23-25, Text_IDs
  `IF_YOU_CAN_READ_THIS`, `YOU_NEED_TO_PLAY`, `THE_GAME_IN_ENGLISH`). Each sits just before a real campaign set
  (`Casual2` 4, `Web_Way2..4` 6-8, `Web_Way50` 16): they are menu headers and a notice, not campaigns to play.
- The original loads them and lists them (probe row `MODC PROBE 4`). What happens when one is started was not tested.
- The port already loads them (`campaigns 23` in the report); nothing changes in the loader.
- The load report counts each as an error (`MODS.md` §5 "A campaign has no planets": "Unplayable campaign", level
  Error), so the whole mod reads "ERRORS 8" although its real campaigns are fine. Since the original lists them without
  complaint, a campaign without planets is better a warning ("menu entry only, cannot be started"), and an error only
  when no campaign of the mod has planets. That is the report's code (`engine/apps/game/src/mod_check.cpp`, lane
  modc-report2), not changed here.

## 5. Other findings in the same mods (not decided here)

- 1770851727: `Projectile_Stun_Victims_Category_Mask` value with a double space before `|`
  ("Failed to convert value"), and `Freighter_Acclamator_E` as a variant of itself.
- 1770851727 trade routes `WW_064`, `WW_126`, `WW_127` name `Allanteen_WW`, which no campaign lists (rows skipped,
  warning).

## 6. `require` of a script no layer has (modc t24)

Question: every main-chunk error left in the load report after modc t12 is a `require` of a script the mod does not
ship (Thrawn's Revenge `GameScoring_old.lua` -> `BoardingHandler`, `Targeting.lua` -> `LayerTeleport`, ...). Does the
original raise an error there (the script stops), or does `require` return nil and the script run on?

Answer, from the docs and the Lua 5.0.2 sources (no probe needed):

- The original links stock Lua 5.0.2 ([`PE_OVERVIEW.md`](PE_OVERVIEW.md) "Lua", [`SUBSYSTEMS.md`](SUBSYSTEMS.md)
  "Lua loading"), and its `require` is the stock `luaB_require`: the exe's strings "could not load package `%s' from
  path `%s'", "error loading package `%s' (%s)" and "`_LOADED' is not a table" are all referenced by one function,
  0x7cfc60 ([`ghidra/strings_xrefs.csv`](ghidra/strings_xrefs.csv)), the three messages of `luaB_require` in
  `toolchain/lua-5.0.2/src/lib/lbaselib.c`. [H]
- Stock `luaB_require` walks `LUA_PATH`; when no entry finds a file (`LUA_ERRFILE`) it raises
  `luaL_error("could not load package ...")`. It never returns nil for a missing script. [H, source]
- So the error unwinds the requiring chunk at the `require` line. Unless the script itself wraps the call in `pcall`
  (none of the mods' cases do), the main chunk fails, the definitions after that line never run, and the engine reports
  the load failure (`LuaScript Error in %s.lua!\n\n%s`, 0x246ad0, the string with the error text). [H for the error,
  M for the engine's handling: a failing chunk is a script that did not load, as for a compile error (§2)]

Port: `engine/lua` `require` already raises an error for a missing script (`lua_vm.cpp`, "cannot find script '<name>'
in Data\Scripts"), the same outcome. The load report keeps such a script as a warning when the data loads it (an XML
script tag names it, another script requires it, a base script of the same path, or a root the original enumerates or
whose loading is not traced), and lists it as an **unused-script lead** (info) when it is a mod's own script under a
by-name root (GameObject, Story, FreeStore, Miscellaneous) that nothing names: that script never runs in retail either
(`docs/MODS.md` §5).
