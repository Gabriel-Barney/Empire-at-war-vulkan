# modc loader probe (modc t13 / t14)

Asks the original game (Steam FoC x64 1.121) four loader questions that `docs/re/` does not answer. Findings go to
[`docs/re/MODC_LOADER.md`](../../../docs/re/MODC_LOADER.md) ("Probe"), status to the roadmap (`modc` t13 / t14).

| # | Question | Seen in | How the probe tells |
|---|---|---|---|
| 1 | Does the script loader skip a UTF-8 BOM (EF BB BF) at the start of a `.lua` file? Stock Lua 5.0.2 does not | workshop 2858787558 `Data\Scripts\AI\SpaceMode\BuildStructureSpace.lua` | `MODC PROBE 1: BOM SCRIPT` runs a story script that starts with a BOM; if it runs it deletes `canary_bom_script.txt` and shows "the BOM script RAN" |
| 2 | Does the XML parser accept an element name that starts with a digit (`<80s_...>`, `<181st_...>` as a unit file's root)? | workshop 1770851727 `Units_Hero_Minors_CSA_Tagge.xml`, `Units_Hero_Empire_181st_Fighter_Wing.xml` | `MODC PROBE 2: CONTROL` looks up one unit from each probe unit file (`Find_Object_Type`) and deletes that unit's canary when it exists |
| 3 | The same for a campaign file (root `<1Modc_Probe_Campaigns>`) | as 2 | `MODC PROBE 3: DIGIT ROOT` is in the Galactic Conquest list only if the file was accepted |
| 4 | Is a campaign with no `Locations` (menu fields only) listed? | workshop 1770851727 `Campaigns_Dummies.xml` (Web_Way, Web_Way_AI, Web_Way_Casual, Dummy1/2, Language/1/2) | `MODC PROBE 4: NO PLANETS` in the list or not |

Controls: the plain story script deletes `canary_plain_script.txt` (the hook works) and the unit from a normal-root
file deletes `canary_unit_plain_root.txt`; without them the run is inconclusive. The output channel is the canary
file `_CustomScriptMessage` deletes on its first call (`docs/re/LUA_SCRIPT_OUTPUT.md` §2); no memory reader is needed.

## Run

Steam running, the game closed. Gabriel launches the game (rule 7 in `CLAUDE.md`); a Claude session may drive it
with computer use only after asking (`--windowed --no-launch`, then launch as printed).

```
tools\probe_mod\modc_loader\run_modc.cmd
```

The script builds `corruption\Mods\ModcLoaderProbe` from the install, launches `StarWarsG.exe
MODPATH=Mods\ModcLoaderProbe` and waits. In the game:

1. Single Player -> Galactic Conquest. Are `MODC PROBE 3: DIGIT ROOT` and `MODC PROBE 4: NO PLANETS` in the list?
   Screenshot the list. Do not start 3 or 4.
2. Start `MODC PROBE 1: BOM SCRIPT` as the Empire, click nothing for 15 s, note the screen lines ("plot 1 loaded" comes
   from the story XML; "the BOM script RAN" only if the script ran). Quit to the main menu without saving.
3. Start `MODC PROBE 2: CONTROL` as the Empire, click nothing for 15 s, note the lines, quit without saving.
4. Exit the game. The script asks the two list questions (or pass `--digit-listed yes|no --empty-listed yes|no`),
   writes `out\<run>\report.txt` and `result.json`, and removes the mod.

Exit codes: 0 report written, 10 control script did not run (inconclusive), 20 setup problem, 30 interrupted, 31 error.

## Cleanup

`cleanup_modc.cmd` (`--dry-run` to see what it would do) removes `corruption\Mods\ModcLoaderProbe`, only if it holds
`MODC_LOADER_PROBE_MARKER.txt`. Every run ends with the same cleanup. No retail file is changed; do not save a probe
campaign.

## Offline check

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\modc_loader\selftest_modc.py [--mod-check <alamo_mod_check.exe> --game <corruption dir>]
```

Builds the mod from `extracted\foc_data_text.zip` into a temp folder, checks the BOM bytes, root elements, list entries
and the verdict logic; with `--mod-check` it also prints what the port does with the probe mod.
