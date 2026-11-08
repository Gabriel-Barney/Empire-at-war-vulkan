# engine/apps/sim split (WF1 t12 analysis 2026-11-06, done in WF1b 2026-11-08)

Goal: lanes stop queueing on `engine/apps`. Until WF1b `alamo_sim` was one static library (27 sources) with the feature definitions
(`ALAMO_SIM_HAVE_*`) PUBLIC on it, and the roadmap's overlap check counted every `apps/sim` path as `apps`.

## Header layers (acyclic)
| Layer | Headers | Depends on |
|---|---|---|
| 0 base | `game_modes`, `pacing`, `save_session`, `story_session`, `battle_catalog`, `battle_ai` | nothing in sim |
| 1 | `map_spawn`, `sim_anim` (-> `game_modes`), `tactical_battle` (`battle_catalog`, `game_modes`), `l1_loaders` (-> `tactical_battle`) | layer 0 |
| 2 | `sim_host` (`game_modes`, `pacing`, `save_session`, `story_session`), `scenario` (`map_spawn`, `sim_host`) | layers 0-1 |
| 3 | `campaign_battles` (`battle_ai`, `battle_catalog`, `l1_loaders`, `sim_host`, `tactical_battle`) | layers 0-2 |

Consumers outside the folder: `apps/game`, `apps/headless`, `render` and `render/galactic`. They link `alamo_sim` and need no change.

## Targets (engine/apps/CMakeLists.txt)
| Target | Sources | Links |
|---|---|---|
| `alamo_sim_config` (INTERFACE) | include dir `sim/include`, `ALAMO_SIM_HAVE_{STORY,ANIM,BATTLES,AI_TACTICAL,TACTICAL_AI}` | |
| `alamo_sim_base` | `pacing`, `game_modes`, `sim_host`, `sim_host_save`, `save_session`, `map_spawn`, `scenario`, `object_scripts`, `bench_*`, `story_session*`, `sim_anim` | config, objects, commands, tactical, tactical/movement, terrain, jobs, galactic, (lua, anim) |
| `alamo_sim_battle` | `battle_catalog`, `tactical_battle*.cpp` (12 files, the L1 loaders included), `battle_ai` | base, tactical/movement, combat, abilities, fog, garrison, hangar, (ai_tactical, ai_tactical_world) |
| `alamo_sim_campaign` | `campaign_battles`, `campaign_battles_ai` | battle |
| `alamo_sim` / `alamo::sim` (INTERFACE) | aggregate of all of them | base (+ campaign when battles are configured) |

The definitions stay PUBLIC through `alamo_sim_config`, so every sim source and consumer sees the same set as before (they change
class layouts, e.g. `ALAMO_SIM_HAVE_ANIM` in `tactical_battle.hpp`, so splitting them per target would risk ODR mismatches).
`ALAMO_SIM_HAVE_ASSETS` stays PRIVATE, and every sim library gets the private dependencies the single target had (`alamo_lua_vm`,
`alamo_data`, `alamo_assets`), so no source sees a different include path or definition.

## Cycles (both were .cpp-level)
1. `tactical_battle_l1.cpp` / `tactical_battle_l1_strikes.cpp` included `campaign_battles.hpp` only for the declarations of the four
   loaders they define (`load_retreat_increments`, `load_retreat_flight_increments`, `load_l1_constants`, `load_l1_factions`). Those
   moved to the new `l1_loaders.hpp` (layer 1); `campaign_battles.hpp` includes it. The L1 sources stay in `alamo_sim_battle`
   (the plan had them in campaign; not needed once the include is gone).
2. `battle_ai.cpp` includes `sim_host.hpp`. With `battle_ai.cpp` in `alamo_sim_battle`, which links `alamo_sim_base`, this is no cycle
   between targets, so no interface was added.

## Tests
`test_battles.cpp` was split by theme (every test there goes through `CampaignBattles`, so a split by target would not separate
anything): `test_battles.cpp` (`alamo_sim_battle_tests`: space / land / retreat / orders / fog / anim / BT pass / map owners / station
production / gates / obstacles / victory monitor / OI objects / FoC data), `test_battles_story.cpp` (`alamo_sim_battle_story_tests`),
`test_battles_ai.cpp` (`alamo_sim_battle_ai_tests`: BattleAi, land teams, landing objects), `test_battles_fighters.cpp`
(`alamo_sim_battle_fighter_tests`). The shared fixture (synthetic data, maps, `run_campaign`) is `tests/battle_test_fixture.hpp`,
header-only (`inline`), one translation unit per exe; `g_temp_prefix` gives each exe its own `%TEMP%` folders for `ctest -j`. All 68
test calls of the old `main` are kept; the goldens are all in `alamo_sim_battle_tests` and unchanged.

## Roadmap touches
`roadmap_store.py` maps a file below `apps/sim` to `apps/sim/<target>/<stem>` (`SIM_TARGET_PREFIXES`; tests `apps/sim/tests/<stem>`);
a bare `apps/sim` still counts as all of `apps` (docs/ROADMAP_PROTOCOL.md, "Claims and parallel batches"). WF1b t02 gave the open
apps/sim groups file-level `touches`.
