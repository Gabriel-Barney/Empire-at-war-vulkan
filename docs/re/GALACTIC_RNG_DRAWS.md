# Synced RNG draws at galactic start (q2f t03, 2026-11-02)

What draws from the synced LCG (`DAT_140a13e24`, the port's `SimRng` behind `host.random().synced()`) between the start
of a galactic sandbox and the end of its first mode frame, and how the port compares. Measured on Heart of the Maw,
Empire human hands-off, seed 0: Q2 traces `20261027-101057-galactic-vanilla` and `20261030-061228-galactic-vanilla-lua`
(`tools/probe_mod/q2_traces/out/`), port runs from `q2trace.py compare --port-exe`.

The trace's "tick 1" is mode frame 1 and includes the galaxy build: retail writes the mode-frame-0 F line before the
galaxy objects exist, so everything the build draws lands in tick 1 (Q2TRACE format, `port_trace.hpp`).

## 1. Counts

| | Retail | Port before q2f | after t02 | after t03 (`--story`) |
|---|---|---|---|---|
| Draws in tick 1 | **130** | 94 | 92 | 96 |
| Ticks with equal draw counts (Lua trace, 13500 ticks) | | | 4917 | 4954 |
| Total draws over the Lua trace | 11087 | 12220 | 11836 | 11927 |

Ticks 2–12, retail / port (same in both traces and with or without `--story`):
8/6, 2/3, 0/4, 0/2, 3/0, 7/0, 2/0, 0/3, 0/5, 0/8, 0/5.

## 2. Inventory of the draws in tick 1

| # | Source | Retail | Port | Tag |
|---|---|---|---|---|
| 1 | Mode-frame-0 fiscal cycle (GALACTIC_ECONOMY §1.2) | **0**: it runs before the galaxy is built, pays nobody and rolls nothing | 2 before t02; **0** since t02 (`GalacticSim::start` keeps only the cycle bookkeeping) | H |
| 2 | Galaxy build: behaviour attach (TACTICAL_GAPS §6.4, base Initialise 0x4c2f90). One `Get_Int(0, interval − 1)` per attached behaviour with interval ≥ 2, in attach order. In the galactic sandbox the drawing behaviours are SHIELDED (150 frames) and POWERED (90 frames) on capital ships and stations | ~66: the retail frame-1 population run through the port's attach model (an estimate, not a measurement) | **59** (`GalacticSim::start` → `create_unit` → `ObjectStore::create` / `attach`) | H rule, M retail count |
| 3 | Story plots: start-of-game rewards (`SPAWN_HERO` and friends at `STORY_ELAPSED 0` / tech-level triggers in `Story_Plots_Sandbox_12U_Empire.xml`, `Story_Plots_Rebel_AI_Heroes_EXP.xml`, `Story_Plots_Underworld_AI_Heroes.xml`). Each spawned unit draws as in row 2 | in tick 1; heroes present at frame 1: Han Solo + Millennium Falcon, Obi-Wan, Rogue Squadron (Wedge + Rogue 2/4/7/10/11), Veers, Boba Fett + Slave I, Accuser, Sundered Heart | **4** with `--story` (`StorySession::apply_reward` → `GalacticSim::spawn_unit` → attach); 0 without (the `--galactic-ai` host had no story session before t03) | H source, M count |
| 4 | Galactic AI first proposal pass (AI_GALACTIC_3 §7.4): Find_Plan_For_Goal weighted pick, PP solver Plan Setup C / Contrast / Production | the rest: about 130 − 66 − (row 3) ≈ 55–60, not separable in the trace | **33**: 4 weighted picks (`PlayerAi::accept`) + 29 PP solver (`run_pp_solver` via `solve_potential_plan`) | H formula, counts need probe P1 |
| 5 | Free store (On_Unit_Service) | 0: never runs, vanilla tag `UnitServciceRate` is misspelt | 0 | H |
| 6 | Perception equations, `#` operator | 0 (AI_GALACTIC_3 §7.3) | 0 | H |

## 3. What is left (open)

- **Row 4 is the gap.** About 25–30 draws of the 34 still missing are in the first AI proposal pass. That is a count
  difference (more candidates, goals or solver iterations in retail), not an order difference, so it belongs to the
  galactic AI work (A2 / q2f t01), and the per-frame number needs debugger probe P1 (AI_GALACTIC_3 §10 probe table:
  hit counter on 0x1ffb40 / 0x532480 / 0x5324d0, bucketed per frame). No draw-order change was made in `engine/ai`.
- **Row 3: story timing.** With `--story` the port spawns more heroes by tick 1 than retail shows at frame 1
  (also Bossk, IG-88, Tyber Zann, Urai Fen, Silri, Emperor, Tarkin, Mon Mothma, R2/3PO). Only the space units draw, so this
  changes few draws, but the trigger timing of those plots differs (engine/apps/sim story session).
- **Row 2: object model.** Retail's galactic store holds company members (stormtrooper squads, AT-STs, X-wings,
  `GALACTIC_SQUADRON`) and no starbase objects; the port holds one object per company plus starbases. The build
  counts will only match by construction if the port's galactic objects follow retail's model.
- Draw **order** within tick 1 cannot be read from the trace (it records per-tick counts and the LCG state only).
  Once the counts match, an equal LCG state at the end of tick 1 shows that the ranges also matched.

## 4. Port switches

- `alamo_headless --economy --galactic-ai --story`: runs the campaign's story plot sets in the AI host, as the retail
  sandbox does (q2f t03). Off by default, so the AI goldens are unchanged by it.
- `q2trace.py compare --port-exe` passes `--story` (opt out with `--no-port-story`).
