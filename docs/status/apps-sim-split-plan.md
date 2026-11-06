# engine/apps/sim split plan (WF1 t12, 2026-11-06)

Goal: lanes stop queueing on `engine/apps`. Today `alamo_sim` is one static library (27 sources, 1.7 MB, `engine/apps/CMakeLists.txt`
lines 7-83) with feature definitions (`ALAMO_SIM_HAVE_*`) set PUBLIC on the one target, and `engine/apps` is single-owner (CLAUDE.md).
This is the analysis; the split itself is not done (it needs the two cycle breaks below and a full gate, see "Why not yet").

## Header layers (acyclic)
| Layer | Headers | Depends on |
|---|---|---|
| 0 base | `game_modes`, `pacing`, `save_session`, `story_session`, `battle_catalog`, `battle_ai` | nothing in sim |
| 1 | `map_spawn`, `sim_anim` (-> `game_modes`), `tactical_battle` (1,737 lines: `battle_catalog`, `game_modes`) | layer 0 |
| 2 | `sim_host` (`game_modes`, `pacing`, `save_session`, `story_session`), `scenario` (`map_spawn`, `sim_host`) | layers 0-1 |
| 3 | `campaign_battles` (`battle_ai`, `battle_catalog`, `sim_host`, `tactical_battle`) | layers 0-2 |

Consumers outside the folder: `apps/game` (15 files), `apps/headless` (6), `render/galactic` and `render` (3).

## Proposed targets and owners
| Target | Sources | Owner area (roadmap `mod`) |
|---|---|---|
| `alamo_sim_base` | `game_modes`, `pacing`, `save_session`, `map_spawn`, `scenario`, `sim_host`, `sim_host_save`, `story_session*`, `sim_anim` | galactic host / story |
| `alamo_sim_battle` | `tactical_battle*.cpp` (12 files, 269 KB core + parts), `battle_catalog`, `battle_ai` | tactical / abilities / fog / AI |
| `alamo_sim_campaign` | `campaign_battles`, `campaign_battles_ai`, `tactical_battle_l1*.cpp` (they need `campaign_battles.hpp`) | campaign battles |

Tests follow: `test_battles.cpp` (453 KB) splits into the matching three test exes; `test_battle_l1.cpp` goes with campaign.

## Cycles to break first (both are .cpp-level, the headers are clean)
1. `tactical_battle_l1.cpp` and `tactical_battle_l1_strikes.cpp` include `campaign_battles.hpp` (layer 3) although they define `TacticalBattle`
   members (layer 1). Either move them into `alamo_sim_campaign` and make the battle library link it back (circular static libs, avoid),
   or move the small amount they use from `campaign_battles.hpp` (the L1 data types) down into a `l1_types.hpp` in layer 1.
2. `battle_ai.cpp` includes `sim_host.hpp` (layer 2) while `battle_ai.hpp` is layer 0. Pass what it reads from `SimHost` through a small
   interface declared in `battle_ai.hpp`, or move `battle_ai.cpp` up into the campaign target.

## Compile definitions
`ALAMO_SIM_HAVE_{STORY,ANIM,BATTLES,ASSETS,FOG,GARRISON,AI_TACTICAL,TACTICAL_AI}` are PUBLIC on `alamo_sim`, so every consumer sees them.
After the split each goes on the target whose sources test it, and the consumers that test them (`apps/game`, `apps/headless`) link the
aggregate `alamo::sim` INTERFACE library that re-exports all three, so no consumer code changes.

## Why not yet
- Cycle 1 touches `tactical_battle.hpp` / `campaign_battles.hpp`, which every open sim lane edits; do it when few lanes are open.
- Determinism: moving code between libraries must not change link or init order of static registries; run the full gate (MSVC, clang,
  ASan) and the CRC selfchecks, goldens must be unchanged.
- Steps: (1) `l1_types.hpp` and the `battle_ai` interface (header-only moves, no CMake change); (2) three targets plus the `alamo::sim`
  INTERFACE alias; (3) split the tests; (4) set `touches` of the sim roadmap groups to the new target names so `batch` can place two
  lanes in one module.
- Cheaper interim: set `touches` per source group (for example `apps/sim/tactical_battle_bt`) instead of `apps/sim`, so `batch`
  treats lanes that edit different files as disjoint. The overlap rule is "equal path, or one is a folder of the other", so two
  different `apps/sim/<file>` entries do not overlap, while a bare `apps/sim` still overlaps all of them: the groups with a bare
  `apps/sim` in `mod` need file-level `touches` (`claim --touches a,b`) to benefit.
