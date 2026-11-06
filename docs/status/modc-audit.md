# Mod and base-game hard-coding in engine/ (modc t02 audit, t03 replacements, t04 lint)

Roadmap group `modc`, tasks t02 / t03 / t04 (2026-11-06). The rule is `docs/MODS.md` §1 (PR #92; until it merges, read it with
`git show origin/claude/modc-t01-contract-5863c0:docs/MODS.md`): no mod or base-game *content* name decides behaviour in
`engine/`; names the original executable itself hard-codes may stay, with a comment that cites the `docs/re/` spec and
address. This file is the audit list; it moves into `docs/MODS.md` as §9 once #92 is on `dev` (§7 "Known violations" there
is the short version of the same list). Status of the follow-up tasks lives on the roadmap only.

## How the audit was done

- `grep` over every `*.cpp` / `*.hpp` / `*.h` of `engine/` (tests and testdata excluded) for: the base factions (`Empire`,
  `Rebel`, `Underworld`, `Pirates`, `Hutts`, `Hostile`, `Neutral`, `Sarlacc`) in any case, the mod factions of Thrawn's
  Revenge (1125571106) and Fall of the Republic (1976399102) from their `FactionFiles.xml`, mod names and workshop ids,
  base unit / planet / map names (`Kuat`, `Coruscant`, `_MP_SPACE_`, `TIE_`, `Star_Destroyer`, ...), `is_stock_faction`
  style branches, fixed player and faction counts (`kMaxPlayers`, `max_players`, `players = 8`, fixed arrays).
- Comments that only cite where a behaviour was observed (e.g. "Thrawn's Revenge ships a 0-byte map") are allowed by
  MODS.md §1 and are not listed. Enum keywords that happen to be faction-like words (`Force_Alignment` `Neutral`,
  `AIGoalApplicationFlags` `Neutral`) are not faction names and are not listed.
- Whether the original hard-codes a name was checked against the executable's string table (`docs/re/ghidra/strings_xrefs.csv`):
  `Empire`, `Rebel`, `Underworld`, `Pirates`, `Pirate`, `Hutts`, `Neutral`, `EMPIRE`, `REBEL`, `BasicEmpire`, `BasicRebel`,
  `AI_Player_Underworld` and the four `Field_Com_*` / `Fleet_Com_*` team names are in it; `Hostile`, `Sarlacc`, `Kuat`,
  `Coruscant`, `_MP_SPACE_KUAT` are not, so a rule keyed on those is a port invention.

## Summary

**38 hits: 12 fixed (t03), 26 kept.** Of the kept ones: 6 are retail hard-codes (cited, the original treats a mod the same
way), 8 are test scenarios / synthetic data of the headless test host or port limits that are not faction rules, and 12 are
port rules whose replacement moves a base-FoC golden or lives in presentation code; those 12 are new roadmap tasks
(modc t17-t20). Base-FoC CRC goldens: unchanged.

Legend: **F** fixed in t03 (data-driven, base FoC unchanged), **R** kept: retail hard-code, **T** kept: test host / synthetic
data / limit, **P** kept: port rule, follow-up task.

### Sim code (engine/ except render, audio, video, UI; checked by the t04 lint)

| # | Where | What it did | Status and replacement |
|---|---|---|---|
| 1 | `galactic/src/campaign_setup.cpp` (Hostile player) | `find_faction("Hostile")` adds the native-owner player of a campaign | **F** `GalacticRules::native_owner_faction()`: the first faction that is neither `Is_Playable`, `Is_Neutral` nor `Can_Control_Planets` (FoC: Hostile, by data) |
| 2 | `apps/sim/src/tactical_battle_bt.cpp` `bt_map_owner` | natives without a player go to "the first player whose faction is named Hostile" | **F** the first player of `native_owner_faction()` |
| 3 | `apps/game/src/game_data.cpp` `make_skirmish_galaxy` | skirmish Hostile player by name | **F** `native_owner_faction()` |
| 4 | `apps/headless/skirmish_run.cpp` `make_galaxy` | skirmish Hostile player by name | **F** `native_owner_faction()` |
| 5 | `apps/headless/skirmish_run.cpp` `make_galaxy` | neutral player `find_faction("Neutral")` | **F** `GalacticRules::neutral_faction()` (first `Is_Neutral`) |
| 6 | `apps/sim/src/battle_ai.cpp` `default_tactical_ai_type` | tactical AI type by faction name (Empire / Rebel / Underworld) | **F** the faction's `Basic_AI` (+0x378, new `FactionRules::basic_ai`; AI_TACTICAL.md "Skirmish slots use Faction.Basic_AI"); "None" = no AI |
| 7 | `apps/game/rules/skirmish_rules.cpp` `ai_type_for`, `is_stock_faction`, `resolve_ai_type` | built-in AI types for three names; "stock" factions skipped the campaign's `AI_Player_Control` pairs | **F** both functions removed. `resolve_ai_type` reads `Basic_AI` for every faction; campaign pairs are read when a mod layer is loaded (`campaign_pairs`, `GameData::mod_layer`), a [port] rule keyed on a mechanism (MODS.md §6 row to add) |
| 8 | `apps/game/rules/skirmish_rules.cpp` `skirmish_default_forces` | built-in skirmish lists for "stock" factions (`is_stock_faction`) | **F** (condition) the lists apply only when no mod layer is loaded (`SkirmishForceOptions::base_game_lists`); a mod's factions always start from their data |
| 9 | `apps/game/include/alamo/game/game_data.hpp` `SkirmishSetup` | default slots `human_faction = "Rebel"`, `ai_faction = "Empire"` | **F** empty = `default_skirmish_faction`: first `Is_Playable` faction, then the first other one (FoC: Rebel, Empire) |
| 10 | `apps/game/src/game_session.cpp` attract demo | AI sides `find_faction("Rebel")`, `("Underworld")` | **F** faction indices 0 and 6, as 0x22f10 sets players 0 and 6 (GALAXY_MAP_PRESENTATION.md §6, MAIN_MENU_BACKGROUND.md step 6) |
| 11 | `ai/src/galactic_seed.cpp` (provisional seed) | default neutral = "Neutral", corruption = "Underworld", extra player "Neutral" | **F** `seed_options_from_factions(files)`: `Is_Neutral` / `Benefits_From_Corruption` from FactionFiles.xml; `alamo_headless` and the test pass it |
| 12 | `galactic/include/alamo/galactic/economy.hpp` | `good_side_name = "REBEL"` default | **F** empty default; `Good_Side_Name` comes from GameConstants (FoC and TR set it) |
| 13 | `apps/game/rules/skirmish_rules.cpp` `default_skirmish_fleet / _ground(faction)` | the [port] base-game unit lists themselves (Star_Destroyer, ...) | **P** kept for base FoC only (game_smoke_skirmish / _land hold them). Retail starts from `Space_ / Land_Skirmish_AI_Default_Forces` (SKIRMISH_SETUP §2 step 4). **modc t17** |
| 14 | `apps/game/src/game_session.cpp` `attract_demo_script` | Demo_Attract_Controller.lua emulated in C++: `Find_Player("Empire" / "Rebel" / "Underworld")`, ten excluded type names, the zone names | **P** a shipped data script hard-coded; run the real script through the object-script host instead. **modc t18** |
| 15 | `apps/game/src/game_data.cpp` `make_skirmish_galaxy` | skirmish galaxy planet type `Kuat`, then `Coruscant` | **P** not in the executable; deliberate (a map's own planet may forbid the setup). Moves game smoke goldens. **modc t19** |
| 16 | `apps/game/src/game_data.cpp` `skirmish_space_maps` | `_MP_SPACE_KUAT` first | **P** with #15 (UI default selection). **modc t19** |
| 17 | `galactic/src/conflicts.cpp` `map_faction_index` | battle map faction index by name (Rebel 0, Empire 1, Pirates 2, Underworld 4, Hutts 5) | **R** table 0x140b30568 (L1_S4_REMAINING.md, BATTLE_TRANSITIONS.md "defender faction index"); strings in the executable |
| 18 | `galactic/src/special_systems.cpp` `faction_pairing` | hero ability target pairing by name | **R** 0x48d900 (P4_SIM_SYSTEMS.md §1.2 step 1) |
| 19 | `galactic/src/visibility.cpp` `faction_default_visibility` | Good_Default for "Rebel" / "Underworld" | **R** 0x3fa490 (GALACTIC_REVEAL_CORRUPTION.md "Baseline") |
| 20 | `apps/sim/src/story_session.cpp` `campaign_plot_sets` | `Rebel_ / Empire_ / Underworld_Story_Name` for those factions' players | **R** `TheStoryMode::Load_Plots` reads these three campaign fields (STORY.md §2); mods use `Story_Name` pairs, which the port reads for any faction |
| 21 | `ai/src/galactic_pp.cpp` hero setup B | `Field_Com_Rebel_Team` / `_Empire_Team`, `Fleet_Com_*` | **R** 0x726410, strings 0x8ce5c8..0x8ce610 (AI_GALACTIC_3.md) |
| 22 | `apps/sim/include/alamo/sim/sim_host.hpp` `session_players` | PlayerList at least 8 | **T** size padding only (no rule per player); 8 keeps the base-FoC CRCs; a setup with more players gets that many |
| 23 | `commands/include/alamo/commands/command.hpp` `kMaxPlayerSlots = 32` | PlayerList cap | **T** [port] limit (retail: a vector). A setup over the cap is refused with a message (sim_host.cpp); TR's 22 players fit |
| 24 | `apps/headless/skirmish_run.cpp` `foc_session` | the foc-* battle scenarios: fixed unit lists, Utapau / Kuat / Coruscant, `_MP_SPACE_KUAT` | **T** named test scenarios of the headless test host (their goldens); `--skirmish-ai skirmish --ai A,B` is the data path for any faction |
| 25 | `apps/headless/skirmish_run.cpp` | synthetic faction XML; default slots Empire / Rebel; `skirmish_host_planet` Utapau / Coruscant | **T** synthetic data; the retail Geonosis trace's sides (skirmish_ai goldens); `--ai` names any factions |
| 26 | `apps/headless/save_run.cpp` | synthetic save-test galaxy (Empire / Underworld / Neutral) | **T** synthetic test data |
| 27 | `apps/headless/ai_run.cpp`, `map_run.cpp`, `save_run.cpp` | default campaigns `Sandbox_Heart_Of_The_Maw_Underworld`, `Full_Story_Campaign_Underworld` | **T** test-host defaults without `--map`; any campaign by name |
| 28 | `apps/headless/main.cpp` record stream, `commands` `SchedulerConfig::max_players = 8` | scheduler defaults | **T** defaults of the synthetic hosts; real sessions set it from the setup (sim_host.cpp) |
| 29 | `apps/game/src/game_session.cpp` campaign AI | FoC campaigns' `AI_Player_Control` pairs ignored (base game) | **P** (part of #7) retail reads the pairs; reading them for base FoC moves game_smoke_campaign. **modc t17** |

### Presentation (engine/apps/game/app, ui, render: MODS.md §1 rule 1 applies, the t04 lint does not scan them)

| # | Where | What it does | Status |
|---|---|---|---|
| 30 | `apps/game/app/app_hud.cpp` `galactic_faction_icon` | emblem index by name substring (rebel 0, empire 1, pirate 2, underworld 3, hutt 4) | **R** in part: 0x3d18f0 maps names to the emblem list (GALAXY_MAP_PRESENTATION.md, `Icon_Alternate`); the substring match is the port's. **modc t20** |
| 31 | `apps/game/app/app_input.cpp` StartSkirmish | default slot factions Rebel / Empire, AI swap by name | **P** **modc t20** (use `default_skirmish_faction`) |
| 32 | `apps/game/app/app_screens.cpp` | `TEXT_FACTION_REBELS` special case for Rebel | **P** read the faction's `Text_ID`. **modc t20** |
| 33 | `apps/game/app/app_session.cpp` attract | `human_faction = "Rebel"`, `ai_faction = "Underworld"` | **P** no effect since #10 (the sides are indices); drop. **modc t20** |
| 34 | `apps/game/app/game_info.cpp` | tech tree / black market faction by name | **P** **modc t20** |
| 35 | `apps/game/app/game_ui.cpp` `set_faction` | GUI shell alternate by faction name | **P** **modc t20** |
| 36 | `ui/include/alamo/ui/gui_menus.hpp`, `ui/src/gui_menus.cpp` | skirmish faction list `{"Rebel", "Empire", "Underworld"}`; `_MP_SPACE_KUAT` default map row | **P** list the data's `Is_Playable` factions; map with #15. **modc t20**, **t19** |
| 37 | `apps/game/app/game_render.hpp`, `render/galactic` `galactic_view_options` | `Evil_Side_Name` default "EMPIRE" (GameConstants overrides) | **P** **modc t20** |
| 38 | `render/galactic/src/galactic_session.cpp`, `render/src/lighting.cpp` | synthetic demo galaxy factions and colours; built-in Kuat environment when no map | **T** renderer demo / no-data fallbacks (real data reads `Factions.xml` colours, R2 t01) |

## The lint (t04)

`tools/lint/check_mod_names.py` (standard library only) scans the sim code of `engine/` (the modules of `check_sim_math.cmake`
plus `vfs`, `apps/headless`, `apps/game/src`, `apps/game/rules`, `apps/game/include`; never `tests/`, `testdata/`, `tools/`)
for string literals that contain a name of `tools/lint/mod_names.txt` as a whole token (letters and digits around the match
end it, `_` does not: `"Rebel_Infantry_Squad"` and `"AI_Player_Underworld"` match, `"BasicRebel"` does not), a `=` name
(`Neutral`, `Hostile`) as the whole literal, or a Steam workshop id (9-11 digits). Comments are not code. Exit 1 on a hit.

An allowed literal carries a marker comment with a reason (for a retail name: the spec and address; for a kept port rule:
the task that replaces it): `[name-ok: reason]` on the literal's line or in the comment block directly above the statement,
or `[name-ok-begin: reason]` ... `[name-ok-end]` around a block.

Runs: ctest `alamo_core_mod_names_lint` and `alamo_core_mod_names_lint_selftest` (engine/core/CMakeLists.txt, every build),
and `tools/lint/gate_summary.py` (the `lint` line of `build_gate.cmd`; a failure fails the gate). The self-test checks the
tokenizer and the fixture tree `tools/lint/fixture_mod_names/` (expected findings by line). Add a new mod's faction names to
`mod_names.txt` when a mod is tested.

## Verification (2026-11-06)

- Base FoC: no golden changed. MSVC 194/194 after two synthetic tests were adjusted to the data-driven rules
  (`test_special.cpp`: synthetic data sets `Good_Side_Name` itself; `test_battles.cpp`: Hostile now comes before the
  synthetic native faction, as in FoC's faction order). Gate: see the PR.
- Thrawn's Revenge (workshop 1125571106): `alamo_mod_check` OK (0 errors, 132 warnings, all the mod's own data);
  `alamo_headless --skirmish-ai skirmish --map _MP_SPACE_ATRAVIS_MADNESS` with `MODPATH` runs 900 ticks, 0 Lua errors, both
  slots `GenericAI` (the mod's `Basic_AI`), starting forces from the mod's data. Many of TR's `_MP_*` maps on disk are 0-byte
  placeholders (`_MP_SPACE_ALDERAAN`, `_MP_SPACE_BESPIN`, ...; the default `_MP_SPACE_GEONOSIS` is absent): the skirmish host
  now names the load error ("empty file") instead of only "not loaded".

## Rows for docs/MODS.md §6 (port rules that apply only when a mod layer is loaded)

To add when #92 is on `dev`, in the same table as `multiplayer_list_only`:

| Rule | Where | Why | Decided |
|---|---|---|---|
| Campaign AI types from the campaign's `AI_Player_Control` pairs (`resolve_ai_type` `campaign_pairs`); without a mod, `Basic_AI` | `engine/apps/game/rules`, `game_session.cpp` | base-FoC sessions have always played `Basic_AI` (game_smoke_campaign); keyed on the mod layer instead of the old faction-name test (modc t03) | modc t03, follows Gabriel 2026-11-04 (mod-only rules keep vanilla) |
| No base-game skirmish unit lists (`SkirmishForceOptions::base_game_lists` off) | `engine/apps/game/rules`, `game_data.cpp` | the lists are base-FoC content; a mod's factions start from their own `*_Skirmish_AI_Default_Forces` | same |
