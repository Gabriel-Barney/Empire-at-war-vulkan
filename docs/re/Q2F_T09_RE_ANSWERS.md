# q2f t09: RE answers, the Rebel attack on Shola in Heart of the Maw (2026-11-08)

Roadmap q2f t09 asked three questions about retail's Rebel attack on Shola in the HotM Lua trace
(`tools/probe_mod/q2_traces/out/20261108-170824-galactic-vanilla-lua`, campaign `Sandbox_Heart_Of_The_Maw_Empire`,
human Empire). The port gathers its Conquer_Pirate force at a held planet next to Shola (the Setup A stage),
merges it and moves one fleet. Retail flies every unit straight from its own planet to Shola, and all units arrive
on the same tick.

This page builds on:
- `A2K_T08_RE_ANSWERS.md` Q2 / Q3 (build-task gather, Setup A stage loop 0x7287a0, slot 6, 0x665160, 0x3f12e0);
- `A2L_T03_RE_ANSWERS.md` Q3 / Q5 (level 4, the `Produce_Force` destination 0x5fe240);
- `PHASE2_ANSWERS.md` §§5–6 (passability 0x3f2510, danger 0x3f18c0, Edge_Cost);
- `GALACTIC_ECONOMY.md` §6.1 (danger token path);
- `FIXPASS_GALACTIC.md` §1 (free-store moves).

Sources:
- the Ghidra decompilation of the FoC x64 `StarWarsG.exe` (RVAs relative to 0x140000000);
- the GalacticTaskForce vtable read from the exe (0x8bfec0: slot 20 0x6cd1a0 Add_Force, 21 0x5fe240 Produce_Force,
  23 0x6cd5b0 Form_Units, 24 0x6cf370 Move_To);
- the shipped scripts (`CONQUERPIRATEPLAN.LUA`, `CONTRASTTORECONNECT.LUA`, `LIFTBLOCKADEPLAN.LUA`, `PGTASKFORCE.LUA`);
- `OFFENSIVEGALACTICGOALS.XML`, `CAMPAIGNS_UNDERWORLD_GC.XML`;
- a re-read of the trace's per-unit parent fleets (the `O` records; planet ids 4 Naboo, 5 Kamino, 8 MonCalimari,
  10 Shola, 13 Yavin).

Behaviour is described in prose; no decompiled code is reproduced. Tags: [H] read directly; [M] one inferred link;
[L] hypothesis.

Short answers:
- **Q1.** For `Conquer_Pirate` (level 2) at an enemy-held Shola, Setup A gives the **first Rebel-held neighbour of
  Shola that is passable at level 2** (Naboo or MonCalimari, both with Rebel starbases, so never "dangerous"). It does
  not give Shola. Even if the stage were Shola, the plan's plain `AssembleForce` gathers in movement mode 0, and mode 0
  cannot end a path on an enemy-held planet: the build task would fail with Production_Failed.
  - Retail's pattern (synchronized arrival at the target, a space wave and then a ground wave, release about one 5 s
    poll after capture) is what **`Conquer_To_Reconnect` / `ContrastToReconnect.lua`** produces. Its Reachability is
    `Single_Hop_Disconnected` (level 4), where slot 6 returns the target itself, so stage = Shola.
    `SynchronizedAssemble` calls `Produce_Force(stage, true)`.
  - The plan attribution is [M]: the probe kit hooks only ConquerPiratePlan, BuildSpace and BuildGround.
- **Q2.** Sync mode (`Produce_Force(stage, true)`, used only by `SynchronizedAssemble`):
  - each unit waits at WAIT_FOR_SYNC_MOVE until every build task of its task force is there;
  - each task computes its own ETA: all-pairs path length divided by (speed × FPS × multipliers). The task force's
    countdown is set to the largest ETA;
  - a unit departs when the countdown falls to its own ETA, so departures are staggered and arrivals coincide. It is
    split out of its fleet first if that fleet holds more than one object;
  - it moves in mode 3 (5 if hidden).

  After arrival, the units **are merged**: WAIT_FOR_ALL, then TRANSPORTS_TO_ORBIT, then READY_TO_MERGE. The task
  whose unit sits in the TF FleetObject then calls `Form_Units` (op 3 per member fleet). The trace shows it: the 11
  surviving space units become one fleet (129) at t7175, and the 11 transports become fleet 755 at t7478. The later
  separate departures are the free store splitting them (Q3).
  Without the sync flag, each unit departs at once in mode 0 (no countdown); the merge path after arrival is the same.
- **Q3.** `ContrastToReconnect`'s SpaceForce thread polls `LandSecured` with `Sleep(5)` and then calls
  `Release_Forces(1.0)`. The 142-tick gap after the capture fits one poll [M]. On admission, `GalacticFreeStore.lua`
  `On_Unit_Added` picks a Friendly_Only target (`FIXPASS_GALACTIC` §1.1). `Move_Object` splits each unit out of the
  shared fleet (op 2 + op 0) and moves it with a mode-0 FreeStoreMovementBlockStatus (§1.2). The last unit keeps
  fleet 129.

---

## What the trace shows (re-read for this page) [H]

Unit → parent fleet → fleet's planet, from the `O` records (one sample per mode frame):

| Tick | Event |
|---|---|
| ≤ 6800 | Every Rebel space unit used below sits alone in its own fleet: Kamino 2, Yavin 3, Naboo 5, MonCalimari 7 (17 units) |
| 6941 | Kamino's 2 fleets (squadron, corvette) depart; they reach MonCalimari at 7087 and leave it at 7088 (pass-through hop) |
| 7013 / 7016 | Yavin's corvette / 2 squadrons depart |
| 7073 | Naboo's 5 fleets depart (Marauder, Sundered Heart, squadron, gunboat, Home One) |
| 7088 | MonCalimari's 7 fleets depart, with the 2 Kamino fleets passing through |
| **7161** | **all 17 fleets reach Shola on the same tick** |
| 7162 | 6 units are gone (Marauder, Yavin's 3, 2 MonCalimari squadrons): the space battle |
| **7175** | **the 10 other surviving fleets merge into fleet 129** (Kamino's corvette fleet): one fleet of 11 units |
| 7238 / 7241 | Kamino: a transport fleet (755) appears and departs 3 ticks later, via MonCalimari (7387 / 7388) |
| 7370 / 7373 | Naboo: 3 transport fleets appear and depart |
| 7382–7388 | MonCalimari: 7 transport fleets appear and depart (one at 7385, six at 7388) |
| 7464 / 7467 | the transports arrive (the one that left at 7385 arrives at 7464, the rest at 7467) |
| **7478** | **the 10 other transport fleets merge into fleet 755** |
| 7497 / 7498 | 4 transports are gone (landed); Shola changes owner |
| 7640–7641 | fleet 129 splits: 10 new single-unit fleets (827–836) and 129 with its last unit all leave for Naboo |

The task brief counted 11 fleets in the first wave. The re-read finds 17 departures, 6 losses at Shola and 11
survivors, the same 11 that later leave for Naboo. The trace does not support "no merge at Shola": both waves were
merged within about 14 ticks of arriving.

---

## Q1. Why does retail gather at Shola itself?

**Answer: under `Conquer_Pirate` (level 2) it would not.**
- Setup A floods from Shola's first passable neighbour and returns the first Rebel-held planet it emits. In HotM that
  is Naboo or MonCalimari. Both have a Rebel starbase, so they are never "dangerous".
- The level-2 result is null only in the cases of Q1.4, and none of them holds here.
- Even with a stage of Shola, ConquerPiratePlan's `AssembleForce` gathers in mode 0, which cannot path into an
  enemy-held planet. The gather would end in Production_Failed and `ScriptExit`.
- The retail behaviour matches **`Conquer_To_Reconnect`** (ContrastToReconnect.lua). Its level 4 makes slot 6 return
  the target, and its `SynchronizedAssemble` sets the sync flag.

[H for each code step; M for which plan retail ran]

### Q1.1 Setup A → slot 6 → 0x665160 for level 2 [H]
- **Setup A stage loop** (0x7287a0). It runs only with AllowFreeStoreUnits set and MagicPlan clear. For each TF:
  - no goal target → stage null;
  - reach = target vf+0x38, else the location object's vf+0x38;
  - **no reach object → stage = target**;
  - otherwise stage = reach slot 6(goal Reachability, TF mask, exclude, include), and **a null result → stage =
    target** (A2K_T08 Q3.1, re-read).
- **Slot 6** (0x664450):
  - level 4 → the reachability's own target (+0x10);
  - level < 6 → 0x665160(level), then the first list entry not excluded (and included, when an include list is
    given);
  - level ≥ 6, or no such entry → null.
- **0x665160 request for level 2.** The 10 s per-level cache applies.
  - The request is built by 0x3eefd0: player = the reachability's AI player, destination = the target object. Every
    flag starts at 1.
  - The level then sets flag0 = flag1 = (level == 5), flag2 = (level 1 or 5), flag3 = level > 2, flag4 = level > 1.
    Flag5 (fog) stays 1.
  - **Level 2: flag0 = flag1 = flag2 = flag3 = 0, flag4 = 1, flag5 = 1.**
  - The flood output is mapped through each planet's graph index (0x3f8500) into the per-level list.

### Q1.2 The flood 0x3f12e0 for an enemy-held target [H]
1. Request with no player or no destination → 0 (empty list). The request is copied into the finder at +0x108..+0x125.
   The byte flags sit at +0x120..+0x125.
2. With flag4 clear, a target not held by the player or an ally (0x4ad9f0) gives an empty list. Flag4 is set at level
   2, so the test is skipped.
3. The target's graph node is looked up (0x1ff780). Not found → 0.
4. **Target held by the player or an ally.** It is flooded from itself, unless flag2 is clear, the target is not
   "corrupted + Benefits_From_Corruption", and the target is dangerous (0x3f18c0). In that case it is treated like an
   unheld target (step 5).
5. **Target not held (Shola for the Rebel).** Each graph neighbour of the target, in edge order, is tested:
   - one already closed in this pass is skipped;
   - one that fails passability 0x3f2400 is closed and skipped;
   - otherwise a BFS (0x3f0ce0) runs from it into one of two alternating buffers;
   - the kept result is the largest flood (strictly greater, so the first wins ties);
   - the closed set is shared, so the first passable neighbour's BFS normally covers its whole connected component.
6. **The target itself is never emitted and never entered.** It is closed in this pass only if a BFS reaches it as a
   neighbour, and it is queued only if it passes 0x3f2400, which an enemy planet fails at level 2.

### Q1.3 Passability and danger at level 2 [H]
**BFS 0x3f0ce0.**
- It emits a popped planet when the player or an ally holds it (0x4ad9f0). **The start planet is emitted too.**
- It queues a neighbour, in edge order, only when that neighbour passes 0x3f2400. Visited planets are closed either
  way.

**Passable 0x3f2400(planet)**, at level 2:
- **Fog gate (flag5):**
  - for an AI-controlled, non-human player, a planet that fails 0x3fa420 is impassable;
  - 0x3fa420 takes no player: it is false for a hidden planet (PD +0x2e0); otherwise it is true when the campaign's
    `Planet_Auto_Reveal` (mode +0x340) is set, else it is the local player's revealed byte PD +0x2c9
    (GALACTIC_REVEAL_CORRUPTION §1.1);
  - `Sandbox_Heart_Of_The_Maw_Empire` does not set `Planet_Auto_Reveal`, so it is true (the default) and only hidden
    planets fail.
- **Owner-passable (0x3f2510)** is true when any of these holds:
  - the planet is held by the player or an ally;
  - it is corrupted and the faction has Benefits_From_Corruption;
  - for an AI player, the perception `Target.TargetPlayerID` fails to evaluate;
  - for an AI player, that perception names no player or a neutral player, and flag0 is set;
  - for an AI player, it names a non-neutral player and flag1 is set.

  At level 2, flag0 and flag1 are clear, so an enemy-held planet (Underworld) is **not** owner-passable.
- **Owner-passable branch:** passable if flag2 (clear at level 2), or corrupted + Benefits_From_Corruption, or **not
  dangerous**.
- **Other branch:** passable only with flag3 (clear at level 2). So enemy planets (Shola, The_Maw) are impassable at
  level 2.

**Dangerous 0x3f18c0(planet):**
- **With a starbase** (PD +0x148 ≠ 0): dangerous unless the planet's control record (behaviour vf+0x98) is held by
  the player or an ally (0x4ad9a0 with the player id). So **a Rebel planet with a Rebel starbase is never dangerous**,
  whatever is in orbit.
- **No starbase, AI player:** evaluate `Variable_Target.EnemyForce.HasSpaceForce` (tokens 0x140a43c58 / c7c / cfc)
  with the AI's galactic evaluator. Dangerous if the evaluation fails or the value is ≠ 0. `HasSpaceForce` = 1 when
  any orbit object that is neither removed nor hidden (0x3a9760) passes the enemy side filter (AI_GALACTIC_TOKENS
  row `HasSpaceForce`).
- **No starbase, no AI system:** with visibility bit 4 set, dangerous when any orbit unit is not held by the player or
  an ally.

### Q1.4 Every way the level-2 result is null, so stage = target [H]
1. The goal target has no reachability object and no location fallback (Setup A, not the flood).
2. The request has no player or no destination object.
3. The target is not found in the planet graph.
4. **Unheld target: no graph neighbour passes 0x3f2400.** At level 2 a neighbour passes only if it is owner-passable
   (held by the Rebel or an ally, or the `TargetPlayerID` evaluation fails) **and** not dangerous (an own starbase,
   or no starbase and no enemy space unit in orbit), and it is not hidden.
5. A passable neighbour exists but its BFS emits nothing. This cannot happen at level 2: the start is emitted when
   held, and an owner-passable start is held unless the evaluation-failure case applies.
6. Every emitted planet is excluded (RequireUniqueStage) or not included (RequireMatchingStage). Conquer_Pirate has
   one TF, so the lists are empty.
7. Level ≥ 6 (not galactic).

For Shola in HotM, Shola's graph neighbours include Naboo (trade route `Shola_Naboo`), MonCalimari
(`MonCalimari_Shola`) and The_Maw (`Shola_The_Maw`), plus any non-route links within the maximum distance.
- Naboo has `Rebel_Star_Base_2` and MonCalimari `Rebel_Star_Base_3` at start. Neither can be dangerous while the Rebel
  holds the starbase.
- So case 4 does not hold, and the stage is whichever of Naboo / MonCalimari comes first in Shola's edge order [M:
  edge order not read; the port picked MonCalimari].
- **No reading of Setup A at level 2 gives Shola in this situation.**

### Q1.5 What `Produce_Force(stage)` sends the units to, and in which mode [H]
- `Produce_Force` (0x5fe240) sets BT +0x98 = **its argument** (a planet, or a planet location wrapper). It never
  reads the goal target or TF +0x100.
- The second argument, a bool, is the sync flag: BT +0xda via 0x524ee0 → 0x649200 parameter 6. Any other type is a
  script error. With no second argument, +0xda = 0.
- `AssembleForce(tf)` (PGTASKFORCE.LUA) calls `Produce_Force(stage)`, so sync is 0. `SynchronizedAssemble(tf)` calls
  `Produce_Force(stage, true)`. The two shipped callers of `SynchronizedAssemble` are `ContrastToReconnect.lua`
  (category Conquer_To_Reconnect) and `LiftBlockadePlan.lua` (Lift_Blockade).
- **PATHING_DEST mode** (0x64aec0 state 0xb, enter):
  - 5 if the unit is hidden (0x3a9760);
  - else 3 with +0xda;
  - else **0**.

  The MovementBlockStatus (0x6af230 → Get_Next_Hop 0x6aedc0) builds the A* request from the mode: flag0 = flag1 =
  flag2 = (mode 5), flag3 = mode > 2, flag4 = mode > 1.
- **Mode 0 cannot end on an enemy planet.**
  - Edge_Cost (PHASE2_ANSWERS §6) accepts the destination unconditionally only with flag4.
  - Otherwise the destination needs owner-passability (an AI evaluates `TargetPlayerID`: Underworld is non-neutral,
    and flag1 is 0) or flag3. Both are clear in mode 0, so the hop is rejected.
  - Get_Next_Hop then finds no path and logs "Unable to find a path". PATHING_DEST sees the unit not at the
    destination with +0xda clear and goes to **PRODUCTION_FAILED** (7). That dispatches `MainForce_Production_Failed`,
    which in ConquerPiratePlan is `ScriptExit()`.
- So a ConquerPiratePlan gather can only ever stage at a passable planet. Retail's direct flight into Shola, with the
  units' departures staggered to the tick, needs mode 3 and the sync countdown (Q2).

### Q1.6 Which plan retail ran [M]
Five observations fit `Conquer_To_Reconnect` (ContrastToReconnect.lua) and not Conquer_Pirate:
1. **Stage = target.** Level 4 (`Single_Hop_Disconnected`) makes slot 6 return the target itself, so `Get_Stage()` =
   Shola. No flood or danger test is involved.
2. **Synchronized arrival** (17 fleets on t7161; 11 transports on t7467, one 3 ticks early) needs +0xda. Only
   `SynchronizedAssemble` sets it.
3. **Two waves.**
   - The plan has a SpaceForce (Frigate | Capital | Corvette | Bomber | Fighter) and a GroundForce (Vehicle |
     Infantry | Air).
   - The ground thread waits until `SpaceSecured` before its own `SynchronizedAssemble`.
   - The trace has a space wave, then about 80 ticks later the ground wave, all in transports.
   - ConquerPiratePlan has one mixed MainForce and one gather.
4. **Release about 142 ticks after capture.**
   - The SpaceForce thread waits with `while not LandSecured do Sleep(5) end` and then calls `Release_Forces(1.0)`.
   - `LandSecured` is set after `Invade(GroundForce)` returns.
   - ConquerPiratePlan releases right after `Invade` and has no poll.
5. **Shola cuts the Rebel graph.**
   - The listed HotM routes are `Shola_Naboo`, `MonCalimari_Shola`, `MonCalimari_Yavin`, `The_Maw_Yavin` and
     `Shola_The_Maw` (Kamino has no listed route; the trace shows Kamino reaching MonCalimari over a non-route link).
   - Naboo's only listed route is `Shola_Naboo`, so unless a non-route link exists, Naboo reaches the other Rebel
     planets only through Shola. That is what `Need_To_Reconnect_Islands` (`Variable_Target.ConnectsLargestIslands`
     × …) rewards.
   - [M: the perception value was not evaluated.]

What remains for Conquer_Pirate:
- The Y-wings built at MonCalimari (5565 / 5968 / 6371) fit Conquer_Pirate's other branch: not all units on the free
  store, so `Produce_Force()` with a null destination, then `return`.
- The produced units go to the free store, where the reconnect plan can claim them [M].
- The probe's only Rebel Conquer_Pirate `PSTART` (t = 184 s) has no end record. That fits a plan that ended through
  that `return` path [L].

### Q1.7 Port change
1. **Keep the level-2 stage rule** of A2K_T08 Q3.6:
   - flood from the unheld target's first passable neighbour;
   - emit planets the player or an ally holds;
   - at level 2, own and allied planets pass when they are not dangerous;
   - an own starbase makes a planet never dangerous;
   - with no starbase, danger is "an enemy space unit in orbit that is not hidden".

   For HotM that gives Naboo or MonCalimari, which is retail's answer for Conquer_Pirate. Do **not** change
   Conquer_Pirate to stage at the target.
2. **A non-sync gather (movement mode 0) must not be able to end on an enemy-held planet.** If a stage ever falls back
   to an enemy target (Q1.4), the build task fails with Production_Failed. The plan's handler then decides
   (ConquerPiratePlan exits).
3. **Check that the port's Rebel proposes and accepts `Conquer_To_Reconnect` @Shola**:
   - `Need_To_Reconnect_Islands` with `ConnectsLargestIslands` and `ConnectsIsolatedPlanetsByForce`;
   - the goal's `Global_Exclusions` (`Conquer_Opponent, Raid, Conquer_To_Reconnect, Death_Star_Use`);
   - ContrastToReconnect's two threads.

   If the port runs Conquer_Pirate @Shola where retail runs Conquer_To_Reconnect, the desire or proposal pass differs
   before any gather code runs. Compare the two goals' desires for Shola in `--ai-log`.
4. For level 4, `Get_Stage()` returns the target. `SynchronizedAssemble` then gathers at the target with the sync
   countdown (Q2). It does not stage at a neighbour.

---

## Q2. The free-store gather in the AIBuildTask FSM (0x64aec0)

**Answer.**
- **Non-sync (`Produce_Force(stage)`):** each claimed unit leaves as soon as its task is ready. It is split into a
  fleet of its own first if its fleet holds more than one object, then paths to the destination in mode 0.
- **Sync (`Produce_Force(stage, true)`):**
  - every task of the task force waits until all of them are ready;
  - each computes its own ETA, and the task force's countdown is set to the largest ETA;
  - a unit departs when the countdown falls to its own ETA, so all arrive together. It is split or launched first, as
    above, and moves in mode 3.

Either way, once every unit is at the destination, the build task whose unit sits in the TF FleetObject calls
`Form_Units`, which merges every other member's fleet into the FleetObject. **The arriving fleets are merged.** The
trace confirms it (t7175, t7478); the separate departures later are the free store's splits (Q3). [H for the FSM; M
for the units of the countdown]

### Q2.1 Entry: WAIT_FOR_FREESTORE_ALLOC (0x16), update [H]
1. Wait while the unit has no parent, or while its parent is neither a planet nor a fleet at a planet.
2. Wait while the free store still holds the unit (0x68efb0).
3. **No destination** (+0x98 == 0) → PRODUCTION_FINISHED (A2L_T03 Q5.3).
4. **Sync flag** (+0xda) → **WAIT_FOR_SYNC_MOVE (0x18)**, with **no split and no move yet**.
5. Otherwise, a unit that is not a landed ground unit, in a fleet: fleet object count 0x432730 > 1 →
   **PRE_PATH_SEPERATE (0xe)**, else FREESTORE_ALLOC (3) → PRODUCTION_END (6) → PATHING_DEST.
6. A landed ground unit → GROUND_UNIT_PRODUCTION (4): launch into a transport, then path.

### Q2.2 PRE_PATH_SEPERATE (0xe), enter [H]
- It re-binds the unit's signals 5 and 6, then executes directly FleetManagement **op 2** (planet, fleet, unit:
  detach) and **op 0** (planet, unit: form a new fleet).
- It does not change state itself. The unit's new fleet raises the signals that move the task on to PATHING_DEST
  [M: transition path].
- So **every unit is pathed in its own fleet, independently**, in both modes.

### Q2.3 PATHING_DEST (0xb) [H]
- **Enter.**
  - The unit's planet is its parent fleet's planet in galactic.
  - Already at the destination → WAIT_FOR_ALL (0xf).
  - Otherwise create a MovementBlockStatus (0x808) for the **unit's own fleet** to the destination. The mode is 5 if
    the unit is hidden, else 3 with +0xda, else 0 (Q1.5).
  - Each hop is a MoveObjectToObject of that fleet. An intermediate own planet is passed through on the next tick
    (Kamino → MonCalimari 7087 → 7088 in the trace).
- **Update** when the block reports done:
  - **At the destination:** a landed ground unit with a free orbit slot is launched into orbit (op 2, then op 5). Then
    → WAIT_FOR_ALL.
  - **Not at the destination:**
    - with +0xda, the task **finishes** (0x64ad00 → ALL_FINISHED, no Production_Failed);
    - without it → **PRODUCTION_FAILED**.

### Q2.4 WAIT_FOR_SYNC_MOVE (0x18) and SYNC_MOVE_COUNTDOWN (0x19) [H code; M units]
**The barrier 0x64c110(task, state).**
- It walks the AI's build-task set (AI mode object +0x40) and returns false when any task of the **same task force**
  (task +0xa0) has a current state (+0x10) other than `state`.
- WAIT_FOR_SYNC_MOVE moves to 0x19 only when every sync task of the TF is in 0x18. Tasks still in production
  (WAIT_FOR_CREDITS, PRODUCTION_QUEUED, …) hold everybody back. In practice the sync path needs every unit already on
  the free store, which is why the shipped plans call `SynchronizedAssemble` only after `Are_All_Units_On_Free_Store()`.

**SYNC_MOVE_COUNTDOWN, enter: own ETA 0x648e80.**
1. Fail (→ PRODUCTION_FAILED) when:
   - there is no destination or no unit;
   - the unit's top-most ancestor (its planet) is not a planet;
   - the speed below is ≤ 0.
2. **Speed** = s × f × FPS × m, where:
   - **s** = the unit type's +0x648 (the build task's type +0x88). For a type with a variant list (+0x940 ≥ 1, entry
     +0x938[1] set), the first variant's +0x648 is used, as in the build-market travel term (AI_GALACTIC_2 §2.1,
     A2K_T01);
   - **f** = the owner faction's +0x3b0 speed multiplier;
   - **FPS** = DAT_140b0a340 (logic FPS);
   - **m** = the float at unit +0x2b8 → +0x20 → +0x230. Unit +0x2b8 is the owning object store; m reads as a per-mode
     speed scale [M: field not named]. The build-market travel term has no such factor.
3. **Distance** = the AI all-pairs path from that planet to the destination (0x690f10 on the AI mode object +0x48),
   costed by 0x691370 = (low 16 bits of the summed hop weights) / 10.
   - Each hop weight's low half is trunc(distance × 10), with the distance divided by `HS_Speed_Factor` on a trade
     route (PHASE2_ANSWERS §8; A1_READINGS §3.4).
   - So the distance is the summed hop length in world units, with trade routes shortened.
4. **Own ETA** +0xdc = distance / speed, in seconds when s is per frame [M].
5. **Raise the TF countdown** (0x64c1b0): for every build task of the same TF, +0xe0 = max(+0xe0, own ETA), raised
   only when strictly greater. The ctor sets +0xe0 = **−1.0**.
   - Every task does this on its own enter, so after all TF tasks entered 0x19 each one's +0xe0 is the TF's
     **largest ETA**.
   - [M: all tasks enter on the same frame, because the barrier releases them together. A task that entered a frame
     later would raise counters the others had already decremented once.]

**SYNC_MOVE_COUNTDOWN, update.**
- +0xe0 −= the AI mode object's vf+0x18 (elapsed time per service, seconds [M]).
- While +0xe0 > +0xdc, wait. When +0xe0 ≤ +0xdc, **depart (0x64a1f0)**:
  - no parent → PRODUCTION_FAILED;
  - parent not a fleet:
    - a landed ground unit (+0x337 set) → GROUND_UNIT_PRODUCTION (launch a transport, then path);
    - a unit that is itself a fleet → PRODUCTION_END (6);
    - else PRODUCTION_FAILED;
  - parent fleet holds more than one object → PRE_PATH_SEPERATE;
  - else PRODUCTION_END (6) → PATHING_DEST.

So the unit with the largest ETA departs on the first countdown update, and every other unit departs when the
remaining time equals its own ETA. Arrivals coincide to within the ETA rounding.

**Check against the trace (wave 1).** Departures relative to Kamino's (6941, the largest ETA):

| Unit | Departs | Countdown offset | Actual travel |
|---|---|---|---|
| Yavin corvette | 7013 | +72 | 148 |
| Yavin squadrons | 7016 | +75 | 145 |
| Naboo | 7073 | +132 | 88 |
| MonCalimari | 7088 | +147 | 73 |

Each offset is 220 − actual travel, and all arrive at 7161.
- In wave 2 the transport that left 3 ticks early (7385, unit 148) also arrived 3 ticks early (7464).
- That fits the ETA using the **carried unit's** type speed while the transport flies at its own speed [M].
- Each transport fleet appears 3 ticks before it departs: GROUND_UNIT_PRODUCTION launches it, then PATHING_DEST runs.

### Q2.5 After arrival: WAIT_FOR_ALL → TRANSPORTS_TO_ORBIT → READY_TO_MERGE → merge → JOIN [H]
1. **WAIT_FOR_ALL (0xf):** barrier on state 0xf for the TF, then → TRANSPORTS_TO_ORBIT (0x13) in galactic (8 in
   tactical).
2. **TRANSPORTS_TO_ORBIT, enter:**
   - a unit already in a fleet → READY_TO_MERGE (0x14);
   - a landed unit on a planet that is not blockaded (0x3fa370) → launched (op 4, then op 0);
   - a blockaded planet → PRODUCTION_FAILED.
3. **READY_TO_MERGE (0x14):**
   - barrier on 0x14;
   - with task +0x80 set → JOIN_TASK_FORCE (0xd). Otherwise, for a GalacticTaskForce (type id 0x140a44a40), wait
     until TF +0x120 (0x6cd930, a plain getter) is set;
   - the unit's parent fleet == the TF FleetObject → **DO_TASK_FORCE_MERGE (0x10)**, else → JOIN_TASK_FORCE (0xd).
4. **DO_TASK_FORCE_MERGE, enter:** TF vtable slot 23 (vf+0xb8) = **`Form_Units` 0x6cd5b0**(TF, player wrapper).
   - It fails (logs, returns nil) without units, without a live FleetObject, or when the FleetObject is not at a
     planet.
   - It creates a FleetMerge block (0x807) and, **for each TF member in member order**, takes the member's parent. If
     that parent is neither the FleetObject nor the planet and was not seen yet, it executes FleetManagement **op 3**
     (planet, FleetObject, member fleet) directly. The member fleet's contents move into the FleetObject.
   - The block is kept in BT +0xb8.
   - Update waits for the block (0x512160), then → JOIN_TASK_FORCE.
5. **JOIN_TASK_FORCE (0xd):** barrier, then PRODUCTION_FINISHED (8) → 0x64ad00 → ALL_FINISHED (0x15).
   - [M: the barrier's argument was dropped by the decompiler; read as state 0xd.]
6. **Which fleet is the FleetObject:** TF +0x120, the parent fleet of the first member added by Add_Force, re-elected
   when it dies (A2K_T08 Q2.2).
   - In the trace it is the Kamino corvette's fleet (129) for the space wave and the Kamino transport's fleet (755)
     for the ground wave.
   - For a ground TF whose members start on a planet surface, how +0x120 gets set to the first transport fleet was
     not traced [M].

So **yes, the arriving fleets are merged**, about 11–14 ticks after the last arrival (t7175 and t7478). The 11 fleets
that later leave Shola separately are split again by the free store (Q3), not by the build task.

### Q2.6 Port change
1. **Sync gather.** When `Produce_Force` gets `true` as its second argument:
   - do not split or move a claimed unit at once. Hold it until every build task of the same task force is waiting
     for the synchronized move;
   - each task then computes its ETA = path length / (unit type speed × faction speed multiplier × logic FPS × mode
     speed scale):
     - the path length is the AI all-pairs path from the unit's planet to the destination, as summed hop lengths in
       world units, with trade-route hops divided by `HS_Speed_Factor` and the 16-bit / tenths rounding of 0x691370;
     - for a type with variants, the speed is the first variant's;
   - the task force's countdown starts at the largest ETA (initial −1, raised only by a strictly larger value) and
     drops by the elapsed seconds on each AI service;
   - a unit departs when the countdown is at or below its own ETA:
     - a landed ground unit is launched into a transport first;
     - a unit in a multi-object fleet is split first (detach, then form a fleet);
     - it then paths in mode 3, or mode 5 if hidden;
   - a sync move that stops short of the destination ends the build task without Production_Failed.
2. **Non-sync gather:** depart at once, split when the fleet holds more than one object, path in mode 0 (5 if
   hidden). A move that does not reach the destination is Production_Failed.
3. **After arrival:**
   - wait for all tasks of the TF, launch landed units into orbit (fail on a blockade), and wait again;
   - the task whose unit is in the TF fleet then merges every other member's fleet into the TF fleet, in member order
     and once per fleet;
   - wait for all, then finish.

   Keep this for both modes. The port's "gather, merge into one fleet" is right for the merge. What differs from retail
   is the stage (Q1) and the missing sync countdown.
4. `Move_To` after the gather then moves the single merged FleetObject, as now.

---

## Q3. After the capture: who moves the 11 units to Naboo, and when? [M]

**Answer.** The departures after the capture are free-store moves, not plan moves (FIXPASS_GALACTIC §1, already
settled).
- **Release.** In ContrastToReconnect:
  - the GroundForce thread sets `LandSecured` after `Invade(GroundForce)` returns (Land_Invade block plus
    `LandUnits`), then releases the ground force and calls `FundBases`;
  - the SpaceForce thread polls `LandSecured` with `Sleep(5)` (150 ticks at 30 FPS) and then calls
    `SpaceForce.Release_Forces(1.0)`;
  - capture at 7498 and departure at 7640 (142 ticks) fit one poll [M].
- **Target.** Each released unit is admitted to the free store. `GalacticFreeStore.lua` `On_Unit_Added` → `MoveUnit`
  → `Find_Space_Unit_Target`:
  - the faction leader's planet first;
  - then `FindTarget.Reachable_Target(..., "Friendly", "Friendly_Only", …)` with `Space_Priority_Defense_Score`
    (0.1), then `Low_Space_Defense_Score` (1.0);
  - gated by `Enemy_Present == 0` and the force checks (§1.1).

  Naboo is that pick for all 11 at that moment [M: the perception values were not evaluated].
- **Move.** `FreeStore.Move_Object` (0x68f5b0):
  - a unit whose fleet holds more than one object is split (op 2, then op 0);
  - a FreeStoreMovementBlockStatus (0x814, mode 0) is created and its first hop runs at once (§1.2);
  - the units are admitted one after another, so fleet 129 loses one unit per move;
  - its last unit (731) leaves in fleet 129 itself, and the other 10 leave in new fleets 827–836 (t7640–7641).
- **Delay.** There is no extra free-store delay beyond the plan's release time. `Move_Object` refuses only while the
  game is paused or a battle is pending (§1.2).

Port change: none beyond Q1 / Q2 if the port already runs `GalacticFreeStore.lua` `On_Unit_Added` → `Move_Object` with
the split (FIXPASS_GALACTIC §1.6). With ContrastToReconnect running, the release comes from the SpaceForce thread's
5 s `LandSecured` poll, and Lua `Sleep` must count galactic game time.

---

## Open items

| # | Item | What settles it |
|---|---|---|
| 1 | Which plan retail ran against Shola (Conquer_To_Reconnect vs Conquer_Pirate) [M] | add unchanged `ContrastToReconnect.lua` (and `LiftBlockadePlan.lua`) with PSTART / PBLOCK / PEXIT records to `tools/probe_mod/q2_traces`; or log `Need_To_Reconnect_Islands` for Shola in the port and compare with the probe's perception rows |
| 2 | Shola's graph-edge order (Naboo vs MonCalimari first) for the Conquer_Pirate stage | the port's planet graph built from `Sandbox_Heart_Of_The_Maw_Empire` (routes plus distance links), or log `Get_Stage()` in the existing ConquerPiratePlan probe |
| 3 | The speed factor at unit +0x2b8 → +0x20 → +0x230, and the AI mode object's vf+0x18 (the countdown decrement) | read the AI mode object's vtable (its ctor) and the object store's +0x20 owner; the trace's departure offsets (Q2.4) already match "seconds of travel" |
| 4 | Whether all TF tasks enter SYNC_MOVE_COUNTDOWN on the same frame | read the FSM driver around 0x64aec0 (when +0x10 changes relative to the barrier test); one-tick effects only |
| 5 | JOIN_TASK_FORCE barrier argument (decompiler dropped it) | capstone disassembly at the 0xd branch of 0x64aec0 |
| 6 | How a ground TF's FleetObject (+0x120) is set when its members start on a planet surface | trace Add_Force / signal 0x6cf810 when GROUND_UNIT_PRODUCTION launches the first transport |
| 7 | PRE_PATH_SEPERATE → PATHING_DEST transition (the enter does not set a state) | read the signal handler bound to signals 5 / 6 at task +0x40 |

## Confidence summary

| Point | Tag |
|---|---|
| Level-2 request flags (f0–f3 = 0, f4 = 1, f5 = 1) | H |
| Unheld target: per-neighbour BFS, largest flood kept, target itself never emitted | H |
| Level-2 passability: own or allied and not dangerous; enemy planets impassable | H |
| Danger = starbase not held by me or an ally, else `Target.EnemyForce.HasSpaceForce` ≠ 0 or evaluation failure | H |
| Null cases of Q1.4; none holds for Shola in HotM (Rebel starbases at Naboo / MonCalimari) | H code, M map state |
| `Produce_Force` destination = its argument; second argument = sync flag; `AssembleForce` is non-sync | H |
| Mode-0 gather cannot path into an enemy-held planet → Production_Failed | H |
| Retail's Shola attack is ContrastToReconnect (level 4, `SynchronizedAssemble`) | M |
| Sync barrier, ETA = all-pairs distance / (speed × faction × FPS × m), countdown = TF max, depart when ≤ own ETA | H code, M units |
| Split (op 2 + op 0) happens at departure, after the countdown, in both modes | H |
| Arrival merge through READY_TO_MERGE → DO_TASK_FORCE_MERGE → `Form_Units` (vtable slot 23, op 3 per member fleet) | H |
| Trace: 17 departures, 6 lost, 11 merged into fleet 129 at t7175; transports merged into 755 at t7478 | H |
| Post-capture moves are free-store `Move_Object` splits; release delay = the SpaceForce `Sleep(5)` poll | M |
