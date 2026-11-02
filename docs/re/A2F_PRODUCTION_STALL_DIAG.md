# A2f t02: why accepted Conquer_Opponent plans never capture Naboo (diagnosis)

Date: 2026-11-02. Diagnosis only. No engine code was changed. Base: `ee0e416` (merged as `14177b1`).
Scenario: HotM headless, human Rebel at Naboo (ctest `alamo_headless_galactic_ai_human`, 72000 ticks).
t01 (the negative Infrastructure budget and the `Build_Base_Component` vetoes) is being fixed separately.
This note takes it as given and marks which causes depend on it.
Prior diagnosis: `docs/re/A2_CONQUER_OPPONENT_DIAG.md` (C1-C6).

## Summary (ranked causes)

| # | Cause | Class | Depends on t01 | Minimal fix | Golden impact |
|---|---|---|---|---|---|
| R1 | The PotentialPlan selects produced types that no Empire planet can ever build: Veers AT-AT (cost 0), TIE / Vader TIE (Build_Initially_Locked), Heavy Assault / Artillery (missing factory). Their build tasks wait at the market forever (no timeout, by spec). | Port stand-in plus a spec gap. The spec's Production score has no veto for t = 1e18, and the port's "cost > 0 unless hero" team filter lets hero units through. | Partly. The factory part (Heavy Assault, Artillery, Smuggler / Bounty Hunter teams) goes away once the AI builds base components. The Veers / TIE / Vader part does **not**. | `ProductionM::score`: for non-magic plans, veto a produced candidate whose `prod_t >= kInf`, i.e. no source planet (one line after `if (r.free_unit >= 0) return 0.5f;`). | `ai_galactic_synthetic_crc` MATCH. `ai_underworld_crc` DIFFERS. HotM / HotM-human headless AI goldens change. |
| R2 | The maintenance time drop (`AllowedTime < elapsed`) never fires, because the port stand-in `elapsed_eta = g.eta` means a plan is never late. A stalled plan lives until another goal displaces it. | Port stand-in (open item `PHASE4.md:220`, 0x525200 not recovered) | No | Make `elapsed_eta` elapsed-based, i.e. `eta + (now - created)` (the experiment that worked). The exact form needs an RE of 0x525200. | Synthetic and underworld AI CRCs DIFFER. The N1 save synthetic decision count changes (27 → 50). HotM goldens change. |
| R3 | Every Conquer_Opponent plan was displaced in Resolve_Category by Lift_Blockade@Felucia (same Offensive category, eff 35.2 vs 12.2), 198-290 s after acceptance. Lift_Blockade's desire then falls to 0 within about 5.6 s. | Spec-faithful (AI_GALACTIC §1.6 [H]); the trigger may be a perception issue. | No | None in the goal system. Check that the transient Underworld fleets at Felucia should count as a blockade for the Empire (perception, not goals). | Only if the perception changes. |
| R4 | A mixed plan hangs. ConquerOpponentPlan.lua: if SpaceForce must produce, its thread returns after `Produce_Force`. If GroundForce is all on the free store, its thread loops `while not SpaceSecured` forever. | Original Lua behaviour; the port is faithful. | No | None in Lua. R2 recovers it (time drop → re-proposal). | Covered by R2. |
| R5 | Maintenance releases with why=2 (`can_fund` false) partway through a plan. | Budget (C2 / t01 area) | Yes | Covered by t01. | Covered by t01. |

"success (released)" is **not a bug**. Both task forces call `Set_Plan_Result(true)` at the start of their threads, and the goal system then releases the plan (R3). `end_plan` (galactic_ai.cpp:775-804) prints the Lua result plus " (released)". The label is misleading but faithful to the original: learning records a success for a plan that was displaced.

## Q1: why the types are cannot_build

Method: build-task instrumentation in a gitignored copy (see Reproduction). For each pending build task it prints the `GalacticSim::build_check` step, reason and detail for every Empire planet (strategic.cpp:156-276, STRATEGIC_GAPS §1.3, 0x2804d0). With the instrumentation on, the run still MATCHes the golden.

Empire state at f=7147 and at f=13114: tech 3.

| Planet | Ground base level (gbl) | Space base level (sbl) | Structures |
|---|---|---|---|
| Corellia | 2 | 2 | Barracks, Mining |
| Kuat | 3 | 3 | Barracks, Light Vehicle Factory, Research |
| Kashyyyk | 1 | 1 | Barracks |
| Tatooine | 2 | 1 | Barracks, Light Vehicle Factory |
| Felucia | 0 | 0 | none |

There is no Heavy or Advanced Vehicle Factory, no Cantina and no Hutt Palace anywhere.

| Type | First failing build_check step at every planet | Data | Fixed by t01? |
|---|---|---|---|
| `Imperial_Heavy_Assault_Company` | s14 Required_Special_Structures: `E_Ground_Advanced_Vehicle_Factory` | GROUNDCOMPANIESEMPIRE.XML, tech 3 | Yes, once the factory is built |
| `Imperial_Artillery_Corp` | s14: `E_Ground_Heavy_Vehicle_Factory` | tech 2 | Yes |
| `Smuggler_Team_E` / `Bounty_Hunter_Team_E` | s14: `Ground_Cantina_E` / `Ground_Hutt_Palace_E` | | Yes |
| `Veers_AT_AT_Walker` (+ `_Deployed_Death_Clone` in plan 12) | s3: cost < 1 | UNIQUEUNITS.XML:180: Is_Named_Hero, no Build_Cost, AI_Combat_Power 1000, Vehicle. The galactic hero is `General_Veers` (NAMEDHEROUNITS.XML:753), whose Unique_Ground_Unit is this type. | **No.** It can never be built. |
| `TIE_Fighter_Squadron` | s9: Build_Initially_Locked, never unlocked in HotM | SQUADRONS.XML:215 | **No** |
| `Darth_Vader_TIE_Fighter_Squadron` | s9 (inherits the lock as a variant of TIE_Fighter_Squadron) | SQUADRONS.XML:1177 | **No** |

How each type gets into the PP:
- **Veers / Death_Clone**: they enter category-mask team lists through the port's exception in `team_type_list` (galactic_pp.cpp:936-940): `x.build_cost <= 0 && !hero` excludes zero-cost types except heroes. `Veers_AT_AT_Walker` is flagged as a hero, so it stays in the list. The death-clone filter (`clone`) does not catch the `_Deployed_Death_Clone` variant in every case (plan 12 selected it).
- **TIE / Vader TIE** pass the Create_Team / +0x29 / +0x21 flags and cost > 0.
- **Heavy Assault / Artillery** pass every list filter.

Nothing at plan time checks locks, required structures or cost, so the PP picks them.

## Q2: plan-time veto versus the task-time market gate

At plan time (PotentialPlan marshallers, galactic_pp.cpp):
- **TechTreeM** (428-441) vetoes only `tech > player tech`.
- **PlanM** (443-559) takes affiliation and generic types into account.
- **ProductionM** (561-660):
  - `setup_c` (565-607) sets `prod_t` = 1e18 for a type with no source planet (AI_GALACTIC_3 §8 row 372, 0x664210).
  - `score` (608-660) vetoes only:
    - a structure with no free slots;
    - production-disabled types;
    - the pop cap;
    - for MagicPlan only: cost < 1, `build_locked`, transport and similar (lines 647-654).
  - The normalised score is `clamp(..., -0.5, 0.5)`, or **0 when spread == 0** (605-606).
- `galactic_ai_player_can_build` (galactic_ai_tokens.cpp:183-195) checks cost, production_disabled, affiliation and tech. It is not used by the PP marshallers.

What the trace shows:
- In every Conquer_Opponent PP, `prod_score` was 0.00 for every candidate.
  - The build queues were idle, so M ≤ 0 and spread = 0, and the 1e18 types received no penalty at all.
  - Even at -0.5 they would still win. TechTree gives +0.4 per tech level, and Contrast rewards Veers' 1000 power.
- Plan 99 selection:
  - Free: VD, VD, Acclamator, Broadside.
  - Produced:
    - TIE scout (src Corellia)
    - Stormtrooper (src Tatooine)
    - Heavy Assault, Artillery, 2× Veers (src **NONE**).
  - eta 20.
- `recompute_eta` (galactic_pp.cpp:158-236) skips types with no source (`if (best < 0) continue;`), so the ETA ignores exactly the types that block the plan.
  - Plan 168 had eta 0, because every produced type was unbuildable. Then AllowedTime = 0 × 1.3 = 0, which means no time limit (ai_goals.cpp:239-243).

At task time, the market gate:
- `market_planet` step 4 (galactic_build.cpp:254-412) bids only from planets where `can_build_at` holds. That is `galactic_ai_can_build` → `GalacticSim::buildable` = cost ≥ 1 && queue kind && `build_check` (ai_hooks.cpp:68-72).
- If no planet bids, the task stays pending. AI_GALACTIC_2 §2.3 has no market timeout, so this is faithful.
- `ProduceForce` (galactic_ai.cpp:1052-1128) waits for all tasks with no timeout of its own.

Against the spec:
- AI_GALACTIC_2 §1.4 (lines 228-246) gives the Production marshaller's veto list for produced, non-magic candidates. It contains only the structure slots and the pop cap. It does not cover t = 1e18.
- Read literally, the original therefore also selects unbuildable types. It most likely got away with it because:
  1. the AI did build its base components, so the factory types become buildable (t01), and
  2. the original's 1e18 probably reaches the score through a non-zero spread, or through a cut that the spec does not record.
- This is a **spec gap**. Next RE step: re-read how 0x725c50 (Setup C) and 0x726010 (Score) handle a candidate with t = 1e18. Is it dropped from the mean/max, given -0.5, or vetoed?
- The hero part (Veers) is a **port stand-in**. The original team filter has no "cost > 0 unless hero" rule. That rule is the port's own addition, and the hero exemption is what lets Veers in.

Recommendation:
- The planner should not pick types that no planet can build for a non-magic plan.
- The smallest deterministic fix is a veto in `ProductionM::score` for `r.free_unit < 0 && r.prod_t >= kInf` (experiment A2F_PRODVETO=1). Mark it `[port]` until the RE re-read settles it.
- Alternatives:
  1. Veto `build_locked || cost < 1` only (A2F_PRODVETO=2). This leaves the factory types to t01.
  2. Drop the hero exemption in `team_type_list` for types the galactic layer cannot produce (the tactical Veers unit). This is narrower, but it does not cover TIE / Vader.
- Do **not** add a market timeout. The spec has none.

## Q3: plan lifecycle

The relevant rules:
- **AllowedTime** = eta × `Build_Time_Delay_Tolerance` (1.3 for Conquer_Opponent), or `Time_Limit` (240) when there is no PP (ai_goals.cpp:239-243).
- **Maintenance** (ai_goals.cpp:286-308) drops a goal when `AllowedTime < 0x525200 + elapsed` (AI_GALACTIC §1.6).
  - The port's `elapsed_eta` (galactic_ai_impl.hpp:563) returns `g.eta` ("[port] on schedule: never late"), so the time drop (294-297) never fires unless AllowedTime is below eta, which it cannot be.
  - So a plan blocked at the market lives until something else ends it. This is **R2, a stand-in**.
- **Resolve_Category** (ai_goals.cpp:358-480):
  - An incumbent whose eff is below the best proposal, and which is replaceable, becomes OldCand (382-393).
  - The greedy pass (419-436) and the random pass (438-465) both exclude kept and active goals, so an OldCand is always released at finalize (474-479). This matches AI_GALACTIC §1.6 [H].
  - All three Conquer_Opponent plans were displaced this way by Lift_Blockade@Felucia, at f=13020, 21747 and 30733 (ages 198-290 s).
    - Lift_Blockade is also Offensive category (DEFENSIVEGALACTICGOALS.XML: Time_Limit 500, tolerance 5.0).
    - Its desire was 0 again within about 5.6 s (transient Underworld traffic at Felucia).
    - Conquer_Opponent was proposed again about 92 frames later and accepted. Then the same thing happened again. This is **R3**.
- **Re-proposal and cooldown**:
  - The Conquer_Opponent goal has Global_Exclusions on itself, Per_Activation_Failure_Desire_Adjust -8, Activation_Tracking_Duration 120 and Tracking_Duration 300.
  - A release counts as a success here, because of the Lua default result. So there is no failure penalty, and re-proposal is immediate. This is faithful.
  - `Set_As_Goal_System_Removable(false)` is called only after a successful Invade, so the plan can be displaced at any point before it lands.
- Is "success (released)" a port bug? **No**, see the summary. It means the Lua default `Set_Plan_Result(true)` (both TF threads, at the start) plus a goal-system release.

## Q4: other things between acceptance and launch

1. **R4**, the mixed free / produce hang (ConquerOpponentPlan.lua). This was seen in experiment v1: plan 226 was stuck 623 s after its production finished, and was then displaced.
2. Staging at `nearest_owned` (galactic_ai.cpp:742), and the `ProduceForce` gather at the stage (1052-1128). Neither has a timeout.
3. PGTASKFORCE.LUA exits:
   - `Is_Good_Ground_Grab_Target` false;
   - `LaunchUnits` returns false;
   - the space battle is lost;
   - the target changes owner.
4. Displacement before Invade (R3).
5. A `can_fund` release (why=2, R5).
6. CrushPlan ScriptExit on Normal (prior C5, unchanged).

## Experiments (72000 ticks, HotM human, one run each; chaotic, so these are indicative only)

| Run | Switches | Owner changes | Naboo captured? | Notes |
|---|---|---|---|---|
| v0 | none (baseline, MATCHes the golden) | 1 | no | |
| v1 | PRODVETO=1 | 7 | no | Kashyyyk, Felucia, Shola, Nal Hutta. Plan 226 hit the R4 hang, then was displaced. |
| v2 | PRODVETO=2 | 4 | no | |
| v01 | ELAPSED=1 | 2 | no | 7 displacements |
| v11 | PRODVETO=1 + ELAPSED=1 | 8 | no | All-free plan 861 launched at 66022 and was displaced at 67826 (failure). |
| v12 | PRODVETO=1 + ELAPSED=2 | changed | **yes, f=38451** | Plan 487 had every unit on the free store (SpaceForce 25+0, GroundForce 12+0, eta 0). It launched at 38152 and invaded at 38449, after a run of 17 quick time releases (why=5). |

`alamo_ai_galactic_tests` under the switches:

| Switch | Synthetic CRC | Underworld CRC | Other |
|---|---|---|---|
| none | MATCH | MATCH | |
| PRODVETO=1 | MATCH | DIFFERS | |
| ELAPSED=1 | DIFFERS | DIFFERS | N1 save synthetic decision count changes, 27 → 50 |

The save-run tests (`save_run`) were not checked.

Conclusion: R1 alone more than triples captures, but it does not reach Naboo. R1 plus R2 (eta + elapsed) produced the first capture of the human's Naboo, through a plan that needed no production. Once t01 lands, R1's factory part disappears. The Veers / TIE / Vader part of R1, and R2, remain.

## Suggested order for fixes

1. **t01** (in progress).
2. **R1**: the `ProductionM` veto for `prod_t >= kInf` (non-magic, produced). Mark it `[port]` pending the 0x725c50 / 0x726010 re-read. Regenerate the underworld, HotM and HotM-human goldens.
3. **R2**: an elapsed-based `elapsed_eta`. Do the RE of 0x525200 first if possible: the ETA term and the units of elapsed (frames or seconds). Regenerate the synthetic, underworld, save and HotM goldens. It also closes the PHASE4.md:220 open item.
4. **R3**: check the Empire's blockade perception at Felucia against Underworld transients (no goal-system change).

RE and probe items:
- 0x725c50 / 0x726010: what Score does when t = 1e18.
- 0x525200: the elapsed-ETA term.
- In game: does the original Empire AI ever queue `Veers_AT_AT_Walker` or a locked TIE in HotM? Probe: log the AI's build queue (`tools/probe_mod/`).

## Reproduction

Scratch copy `build-a2f-t02-src/` (gitignored by `/build-*/`; its `toolchain` is a junction):
- `build.cmd` configures `bld\` with `ALAMO_COMPILER_CACHE=OFF` and the modules core;vfs;data;jobs;galactic;assets;objects;anim;commands;tactical;terrain;lua;ai;apps.
- `run.cmd`: golden run.
- `runx.cmd <veto> <ticks> <elapsed>`: experiment run.
- `buildtests.cmd`: builds and runs `alamo_ai_galactic_tests`.

Environment switches in the copy only:
- `A2F_TRACE`: the [A2F] build-task, [A2F-PL] planet, [A2F-PP] selection and [A2F-GOAL] maintenance / displacement traces.
- `A2F_PRODVETO`:
  - =1 vetoes `prod_t >= kInf`;
  - =2 vetoes cost < 1 or build_locked.
- `A2F_ELAPSED`:
  - =1 returns (now - created) / 30;
  - =2 returns eta + (now - created) / 30.

## Outcome (2026-11-02)

- R1 implemented as a [port] veto in `ProductionM::score`: a produced candidate whose Setup C time is 1e18 is vetoed (covers Veers cost 0, Build_Initially_Locked TIE / Vader TIE, missing factories, and Build_Base_Component structures whose prerequisites fail at the target). The port-only "cost > 0 unless hero" team-list filter was removed.
- R2 not implemented as "PP ETA + elapsed": measured, it dropped every on-schedule build (Build_Base_Component successes 5 -> 0). Implemented instead: goal ETA = remaining time of the plan's unfinished build tasks (stand-in for 0x525200 [M], inferred from its callees) + (NOW - created)/FPS [H]. Needs an RE read of 0x525200.
- HotM-human, 72000 ticks: Conquer_Opponent accepted 11 -> 60, rejected orders 85 -> 0, failed build tasks 144 -> 2; still no capture of the human's planets (Budget vetoes most potential plans).
