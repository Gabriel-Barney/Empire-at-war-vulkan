# A2k t08: Underworld saboteur and the Naboo corruption (2026-11-06)

Scenario: `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel with no orders, `--story`, seed 1, `--workers 0`.
Retail: the a2j probe run `tools/probe_mod/a2f_budget/out/20261106-074145` (`A2J_T02_T04_PROBE_RESULTS.md`): `samples.tsv`,
`production.tsv`, the `SH` records of `records.txt`. Port: e267571 (`claude/a2k-laneC`), traced with temporary `fprintf`
patches (fleet arrivals, task-force events, status results, corruption applies, fiscal payments). The patches were removed.

Confidence tags: [H] read or measured directly; [M] inferred from several consistent measurements; [L] hypothesis.

## 1. Retail: the corruption cost is not reserved

- **The charge.** Between the samples at frames 585 and 616 the Underworld's credits fall by 1006.66 (3146.53 → 2139.87). The two
  palaces account for 206.66 of that (6.667 per frame, 31 frames), so **800 credits leave in one step**. 800 is
  Intimidation's `Corruption_Choice_Credit_Cost` [H].
- **No budget pool pays it.** Offensive stays at 1302.98 and Infrastructure at 2463.62 across the step. ALL (= credits at each
  pool recompute) falls 3766.52 → 1933.20 at 647. That is the palaces' 1033.3 for Infrastructure plus the 800, which no category is
  charged for [H].
- **The planet is Naboo, and the state follows.**
  - The Underworld Offensive weight rises 6 → 6.25 between the SH records at 771 and 894. That is `3 × PlanetsCorrupted`
    rising by one planet in 12, after the 5 s transition [H].
  - The Infrastructure weight gains `+3 × Should_Save_For_Corruption` at 771. So the Underworld holds no saboteur from then
    on: the starting saboteur was consumed (`Causes_Despawn`) [H].
  - The first payment is 2150.0 exactly (credits 0 → 2150 at 1350): the port's 2105 plus Naboo's Intimidation share
    90 × 0.5 = 45 (`A2J_T03_INCOME.md` §1.4) [H].
- **The plan does not reserve either.** `AI_Plan_Underworld_DeploySaboteur.lua` assembles, moves, calls `Activate_Ability()`
  and sleeps. It makes no `Budget` call, and the plan's PP cost is the saboteur only (net 0 with the free one, 1000 with a build).
  The apply (vf30 0x52b160 → executor 0x52bf50, P4 5.5) charges the credits at that moment and fails when they are short [H].
- **Which saboteur.** Retail's first saboteur build starts at 731 (Shola) and ends at 1332. So Naboo was corrupted at about 600 by
  the **starting saboteur** (`Starting_Forces Underworld, NalHutta, Underworld_Saboteur_Team`), with 3146 credits in hand [H].

**Answer to t08:** retail neither reserves nor budgets the Corrupt_Planet cost. It had enough credits because its saboteur
arrived at about frame 600, before the palaces and the Destroyer_Droid_Company had used the credits up (0 by 1111).

## 2. Port at e267571

| frame | event |
|---|---|
| 116 | plan 10 Corrupt_Planet @MonCalimari (desire 25.5) takes the starting saboteur (net 0); plan 11 @Naboo (desire 24.0) needs a build (net 1000) |
| 117–212 | the saboteur is staged at **Felucia**, the stage the port gives a plan with IgnoreTarget: the player's first planet with a starbase (`galactic_ai.cpp`, `nearest_owned(p, -1)`, a port rule). It travels NalHutta → Kashyyyk (passes, hidden) → Felucia |
| 212 | arrival at Felucia: the saboteur's fleet is merged into the Felucia fleet (StarViper, Crusader, Vengeance; GE §3.3 step 4). The fleet is no longer hidden, because a fleet is hidden only when every member is (SG 2.2) |
| 219 | `Move_To(MonCalimari)` moves the task force's fleet, which now has 4 units, to Kashyyyk (Empire starbase 1 + Acclamator) |
| 286–291 | space conflict at Kashyyyk, and the whole fleet is destroyed. `No_Units_Remaining`, plan 10 fails |
| 154–714 | a new saboteur is built at Felucia for plan 11 and travels alone (hidden) Felucia → Kashyyyk → Kuat → Corellia → Tatooine → Naboo |
| 993 | Intimidation is applied at Naboo with **3218.6** credits |
| 1350 | Underworld payment **1150** = 2105 + 45 (Naboo) − 1000 (NalHutta, which the Empire took before 1350: Empire 870 = 667.5 + 202.5) |

- **The "688 < 800" of `A2J_T03_INCOME.md` §1.4 is gone.** It was the 5509c2a trajectory. Since the a2j market fix (lane A)
  the Underworld holds 3218 credits at the saboteur's arrival, so Naboo is corrupted and its 45 share is in the first payment
  [H]. No code change was needed for that outcome.
- **What still differs from retail:**
  1. The starting saboteur dies at Kashyyyk (frame 286) instead of corrupting Naboo by about 600.
  2. The port builds a saboteur at Felucia at 154; retail builds its first at 731, at Shola.
  3. The corruption lands about 390 frames late (993 vs 585..616).

  The NalHutta loss before 1350 is the D3 / D4 trajectory gap (the Empire's ground goals and Conquer_Pirate), which belongs to
  other lanes.
- **Tried, not kept: a stage from the target.** Staging at the owned planet nearest the target (Shola for both targets) changes
  nothing that matters. The saboteur jumps NalHutta → Shola (225), merges into the Shola fleet (Crusader, Interceptor4), and
  `Move_To(MonCalimari)` takes that fleet into MonCalimari, where it is destroyed (315). The Naboo plan is released at 410, and
  no planet is corrupted by 2000. That is not retail either, so the port rule stays until Q3 is answered.

## 3. RE questions (decompiled binary; not answerable from the specs or the probe)

1. **Q1 (orbit merge of a hidden fleet).**
   - In the orbit entry 0x4338d0 and the arrival merge (slot 0x3f7d70 / 0x3f9370, merge 0x433dd0, GE §3.3 step 4): when the
     arriving fleet is hidden (0x3a9760: every member stealthed, e.g. a lone `Underworld_Saboteur_Team` with
     `Saboteur_Galactic_Stealth`) and a visible friendly fleet holds the slot, is it merged into that fleet?
   - Or does a hidden fleet get a slot of its own and stay separate? GALAXY_MAP_PRESENTATION §7.1 mentions "the planet's
     first non-stealthed orbit fleet" [M].
2. **Q2 (what Move_To moves).**
   - `GalacticTaskForce.Move_To` (0x6cf370, MovementBlockStatus 0x6aed50): is the moved object the task force's FleetObject
     (+0x120) with all its contents, including units no task force owns?
   - Or are the task-force members first split into a fleet of their own (AIBuildTask PRE_PATH_SEPERATE 0xe /
     DO_TASK_FORCE_MERGE / JOIN_TASK_FORCE, AI_GALACTIC §2.4)?
   - At the end of `Produce_Force(stage)`: does a free-store unit that reaches the stage stay in its own task-force fleet, or
     does it join the fleet already in orbit?
3. **Q3 (stage of an IgnoreTarget plan with a target).**
   - FreeStore Setup A 0x7287a0 computes the stage 0x140b3a660[tf] "through the target's reachability object" (AI_GALACTIC_2
     §1.4). Does PlanDefinition +0xAA (IgnoreTarget) change that for a plan that has a planet target
     (`AI_Plan_Underworld_DeploySaboteur`: IgnoreTarget = true, Corrupt_Planet / Sabotage_Planet targets)?
   - Which planet does `MainForce.Get_Stage()` return for Corrupt_Planet @Naboo and @MonCalimari in HotM Rebel? Retail
     builds every early saboteur at Shola (731, 1537), which suggests a target-based stage [M].
4. **Q4 (who keeps the free saboteur).**
   - Two Corrupt_Planet proposals in the same pass want the one free saboteur: MonCalimari (desire 25.5 = 13 + 5 + 5 × 112.5 /
     75) and Naboo (24.0 = 13 + 5 + 5 × 90 / 75).
   - Under Resolve_Category and the accept re-solve 0x6c4a50 (PP+0x104 / unit +0x30 reservations, A1_READINGS §28), which
     goal keeps the unit and which one needs a build?
   - Is the free unit's distance key (Setup A: stage cost² + distance²) used to give it to the nearer target?
5. **Q5 (probe, Gabriel launches).** Add DeploySaboteur to the a2f_budget kit's P channel: PSTART / PEXIT with the goal type
   and target, the saboteur's planet or fleet each 30 frames over 0..1400, and the fleet it is in (member count). That settles
   Q1, Q2 and Q4 by behaviour. The 074145 run has no DeploySaboteur record.

## 4. Test

`test_underworld_special` (`engine/ai/tests/test_ai_galactic.cpp`, synthetic, no data) now also checks §1. The AI update that
applies the first corruption takes exactly the choice's cost (500) from the credits and leaves every budget category's pool and
available amount unchanged.
