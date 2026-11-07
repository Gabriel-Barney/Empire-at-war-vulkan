# A2k t08: answers to the saboteur RE questions (2026-11-06)

Answers to Q1–Q4 of `A2K_T08_RE_QUESTIONS.md` §3, read from the decompiled binary (RVAs relative to 0x140000000).
Q5 is a probe request; §5 restates what the kit has to log. Tags: [H] read directly; [M] inferred from code that
was read only in part, or from names; [L] hypothesis.

Object layout used below (all [H] from the functions cited):
- game object +0x2b0 = parent (a unit's fleet, a fleet's planet); +0x335 / +0x336 = planet / fleet behaviour
  index (−1 = none); +0x3a0 bit 2 = being removed; +0x50 = object id; +0x58 = owner player.
- FleetDataPack = fleet object +0xC0 (ctor 0x5528e0): +0x10 / +0x18 contents vector / count; +0x40 orbit planet;
  **+0x48 orbit slot**; **+0x4c preferred slot**; +0x80 raid target; **+0x90 requested slot** (from a FleetManagement
  op 8); +0x96 "continue" flag. The ctor sets +0x48, +0x4c and +0x90 to **−2**.

---

## Q1. Does a hidden arriving fleet merge into a visible friendly orbit fleet?

**Answer: no, and stealth plays no part. A fleet whose move did not request a slot never merges on arrival. It takes
a slot of its own and stays a separate FleetObject. Merging on arrival happens only for a requested slot (FleetManagement
op 8, which only the galaxy-map GUI issues) of 1 or 2 that a fleet of the same owner already holds.** [H]

### Q1.1 Orbit entry 0x4338d0(fleetBeh, fleet, planet, slot, noConflictTest)
1. It requires a fleet, a planet and a planet data pack. The fleet goes on the planet's orbit list (D +0xc0).
2. Fleet data +0x40 = planet. If `slot < 0` (every movement caller passes −2), slot = **0x3f7d70(planet, fleet,
   preferred +0x4c)**. If the slot changed, it is written to +0x48 and the same planet+slot is written to every
   content's data. Position = planet + (18, 10, 0), facing 90°.
3. **Conflict test.** If the fleet is hidden (0x3a9760), the test is **skipped** and the function returns "no
   conflict". Otherwise a conflict is reported when the planet has a starbase (D +0x148 > 0) whose owner is hostile
   to the fleet owner (0x2824f0), or when some orbit fleet with a not-removed content is not hidden and is hostile
   (0x39a450). Event 0x327560 fires in both cases.
4. 0x4338d0 never merges.

### Q1.2 Slot choice 0x3f7d70(planet, fleet, preferred)
- The candidates are, in order, `preferred, 1, 2, 0`. A candidate > 2 (unsigned, so −2 too) is skipped.
- A candidate is taken unless **another** orbit fleet with the **same owner player** already has that slot
  (0x4327b0 = fleet data +0x48).
- If all candidates are taken, the result is **0**: the fleet then shares slot 0. No stealth test, no merge.
- So an AI fleet (preferred −2) gets the first of slots 1, 2, 0 that no other own fleet holds.

### Q1.3 Arrival 0x56a1e0 (fleet locomotor service, final hop or stopped)
The order of operations after the snap to the hop point:
1. Inaccessible planet → turn back (0x569980), return.
2. conflict = 0x4338d0(beh, fleet, planet, −2, false).
3. Intermediate hop: as in GE §3.3 (continue, or Underworld runs through).
4. Final hop or stopped (LAB_14056a70b):
   1. speed 0 (0x4355f0), 0x435690, path cleared, +0x96 = 0.
   2. **No conflict:**
      1. If the fleet is raid capable (0x432e20) and its raid target (+0x80) is this planet, start the raid (0x433eb0).
         For a hidden fleet, 0x3f9270 (planet's controlling player) is evaluated first, without other effect.
      2. `req` = fleet data +0x90; +0x90 = −2 (0x4355c0).
      3. If `req == −2`: slot = 0x3f7d70(planet, fleet, +0x4c), and **no merge lookup is made**.
      4. If `req ≥ 0`: list the orbit fleets whose slot is `req` (0x3f9370, orbit order, any owner).
         - If the list is not empty and `req == 0`: only the `merge_fleets` notification fires. No merge.
         - If `req` is 1 or 2: the merge partner is the **first** fleet in that list.
      5. If slot ≥ 0: write the slot (0x435600).
      6. If there is a merge partner other than the fleet itself: `merge_fleets` event, then **0x433dd0(partner, fleet)**.
      7. Pending AI arrival callbacks (0x432740 → 0x3cf3c0), and 0x435680 when 0x432c80.
   3. **Conflict:** 0x435680, 0x4355a0. If no raid on this planet: pull nearby incoming fleets (0x569700), slot =
      0x3f7d70, write it, mark the planet (0x3fa9c0), and `Initiate_Space_Conflict` (0x2af7c0). **No merge.**
5. **Merge 0x433dd0(into, from):** only when `into ≠ from` and both owners are the same player. It moves the contents
   of `from` one by one, last first, out of `from` (0x434d30) and into `into` (0x430f30). The emptied fleet dies
   through the normal empty-fleet path.

### Q1.4 Who sets +0x90
- Its only writer outside the arrival reset is FleetManagementEvent op 8 (0x5af260: `+0x90 = event +0x5c`).
- Op 8 is issued only by the galaxy-map GUI drop (0x3d4c80: a fleet dropped on another planet's pad, with the pad's
  slot).
- AI code issues ops 0, 2, 3, 4, 5 (Land / Launch / Form_Units, AIBuildTask, Move_Object) but never op 8. An AI
  fleet therefore always arrives with `req == −2` [H for the callers found: all 0x5afb60 calls with op 8 are in
  0x3d4c80].
- GALAXY_MAP_PRESENTATION §7.1 ("first non-stealthed orbit fleet") and UI.md ("slot-0 fleets merged into one pane")
  describe presentation only. Several fleets of one owner can share slot 0 and are drawn as one pane, but they stay
  separate objects.

### Q1.5 Port change
`engine/galactic/src/campaign.cpp`, `GalacticSim::arrive`, step 4:
- **Without a requested slot, drop the merge into "the first friendly non-moving fleet in orbit".** Give the fleet
  its own slot by the 0x3f7d70 order (preferred, 1, 2, 0; skip a slot another own fleet holds; slot 0 when all are
  held).
- With a requested slot: merge only when the slot is 1 or 2 and the first fleet in that slot (orbit order) has the
  same owner. For slot 0, write the slot and send the notification only.
- Reset the request after every final arrival.
- The conflict branch never merges.
- The hidden state does not enter this decision. It enters only the conflict test of 0x4338d0 (a hidden fleet causes
  no conflict, also on an intermediate hop).
- Check that the Playtest 3 t07 rule (human GUI drop with a slot) still holds: it is the `req` 1 / 2 case above.
- With this change the frame-212 merge at Felucia cannot happen. The saboteur stays alone, hidden, in its own slot.

---

## Q2. What does `GalacticTaskForce.Move_To` move?

**Answer: the task force's FleetObject (TF +0x120) as a whole, with every object in it, including objects no task
force owns. Move_To does not split. The engine keeps foreign units out of that fleet in two places: the build task
splits a claimed unit out of a shared fleet before pathing it to the stage, and arrival does not merge (Q1).** [H]

### Q2.1 Move_To 0x6cf370(TF, L, args)
1. Fail (log, return nil) if TF +0x120 is null or does not resolve (0x5791e0), the fleet is being removed (+0x3a0
   bit 2), or the TF has no units (+0x78..+0x80 empty).
2. Fail if the fleet is not at a planet (+0x2b0 == 0), or if argument 1 is missing or not a planet target
   (DAT_140a43b18).
3. 0x435750(fleet, 0): the fleet's raid target +0x80 = 0 (Raid sets it instead).
4. Create MovementBlockStatus 0x6aed50(0x808), then 0x6af230(block, TF, **fleet = TF +0x120**, destination,
   **mode = goal type Reachability** (TF +0xf0 → +0x88 → +0x48 → +0x2c)). The block registers for the fleet's
   signals 9, 10, 11, 0x10, 0x11 and 0x1b and runs its first hop (0x6aebf0).
5. Each hop (0x6aedc0 Get_Next_Hop):
   - wait while the galactic pause counters are > 0;
   - the fleet must be at a planet; done when that planet is the destination;
   - A* from the fleet's planet with the request flags of the mode (flag0 = flag1 = flag2 = (m == 5); flag3 = m > 2;
     flag4 = m > 1);
   - take path[1]. If that hop is held by another player (0x3f9270) and is not the destination: a faction with
     `Benefits_From_Corruption` sets fleet +0x96 = 1 and goes on; otherwise the move stops when m < 2;
   - the hop is a MoveObjectToObject of the **fleet object** (0x3af450 with the fleet's id).
   - For Corrupt_Planet, Reachability is `Any` = 5, so the move passes enemy planets.

### Q2.2 Which fleet is TF +0x120 [H]
- `Add_Force` (0x6cd1a0): if +0x120 is null, it becomes the fleet wrapper (0x5751d0) of the **parent** (+0x2b0) of
  the added unit, if that parent is a fleet. The TF listens to that fleet's signals 0x10 and 0x1b.
- Signal handler 0x6cf810:
  - on 0x10 from the TF fleet, or when the fleet no longer resolves, the fleet is dropped and **re-elected**: the
    parent fleet of the first TF member (member list order) whose parent is a live fleet;
  - with no members left, the TF result is 3 / 7 (No_Units_Remaining).
- So the FleetObject is whatever fleet holds the first member. If that fleet also holds other units, Move_To moves
  them too. The port's frame-219 move of four units is therefore what retail would do **after** a merge. The
  difference is the merge itself (Q1).

### Q2.3 Produce_Force(stage) for a free-store unit (AIBuildTask FSM 0x64aec0) [H states, M two state names]
1. **WAIT_FOR_FREESTORE_ALLOC (0x16), when the claimed unit is ready:**
   - no destination (+0x98 == 0) → PRODUCTION_FINISHED;
   - +0xda (sync move) → 0x18;
   - the unit is on a planet surface → GROUND_UNIT_PRODUCTION (4);
   - the unit is in a fleet: count = 0x432730(parent fleet). **count > 1 → PRE_PATH_SEPERATE (0xe)**, else
     FREESTORE_ALLOC (3) → PRODUCTION_END (6) → PATHING_DEST.
2. **PRE_PATH_SEPERATE:** FleetManagement op 2 (detach the unit from its fleet), then op 0 (form a new fleet), both
   executed directly. The unit now has a fleet of its own.
3. **PATHING_DEST (0xb):**
   - the unit's fleet already at the stage → WAIT_FOR_ALL;
   - otherwise a MovementBlockStatus moves the **unit's own fleet** to the stage. Mode **5** if the unit is hidden
     (0x3a9760), 3 with +0xda, else 0.
4. **On arrival at the stage:** WAIT_FOR_ALL (0xf) → (galactic) TRANSPORTS_TO_ORBIT (0x13) → READY_TO_MERGE (0x14).
   There the build task compares the unit's parent fleet with the TF FleetObject (0x6cd930):
   - equal → 0x10 (DO_TASK_FORCE_MERGE: a TF vf+0xb8 block, awaited through 0x512160);
   - otherwise → 0xd (JOIN_TASK_FORCE: wait for the others, then FINISHED).
   - [M] Which branch merges is not settled. Either way only fleets that hold TF members are merged, through
     `Form_Units`-type op 3 into the TF FleetObject.
5. `Form_Units` (0x6cd5b0) merges the parent fleet of each TF member that is not the FleetObject and not a planet,
   once per distinct fleet (op 3, in member order). It would also carry the non-TF units of such a fleet, but the
   split in step 2 means the fleet holds only TF units.
6. **Answer to the third bullet:** the unit stays in its own task-force fleet at the stage. It does not join the
   fleet already in orbit, because arrival does not merge (Q1) and no build-task state merges with non-TF fleets.

### Q2.4 Port change
- `engine/ai/src/galactic_ai.cpp`, task-force move: keep moving the whole FleetObject. Check two things:
  - the TF fleet is chosen as the parent fleet of the first live member and re-elected when that fleet dies or
    empties;
  - the `ProduceForce` gather splits a claimed free-store unit out of a fleet with more than one object (op 2 +
    op 0) **before** it paths to the stage, and merges only TF-member fleets at the stage.
- The move mode for a hidden unit's gather is 5.
- No change to Move_To itself is needed once Q1 is fixed.

---

## Q3. Does IgnoreTarget change the Setup A stage of a plan with a planet target?

**Answer: no. FreeStore Setup A (0x7287a0) never reads PlanDefinition +0xAA. The stage is computed from the goal
target whenever the goal has one: the first planet owned by the AI player (or an ally) in the target's reachability
flood. IgnoreTarget only switches the contrast terms (AI_GALACTIC_2 §1.4).** [H for the code; M for the stage
planet in HotM]

### Q3.1 Stage loop of Setup A 0x7287a0
- The loop runs only when AllowFreeStoreUnits (+0xD0) is set and MagicPlan (+0xA8) is clear. The function's only
  PlanDefinition reads are +0xD0, +0xA8 and +0xD1.
- For each TF i (TF definitions in order), with goal = PP +0x20 and target = goal +0x58:
  1. no target → stage[i] = **0** (null);
  2. reach = target vf+0x38. If null: the target list's location object at the target's position (vf+0x48 with
     target vf+0x68), then its vf+0x38;
  3. if reach is found: `stage = reach->vf+0x30(goal type Reachability (goal +0x48 → +0x2c), TF def +0x94,
     exclude, include)`. Exclude = the stages chosen so far when TF def +0x98 (RequireUniqueStage). Include = them
     when +0x99 (RequireMatchingStage) and not +0x98;
  4. a null result → stage[i] = **the goal target itself**.
- The stages go to 0x140b3a660[i]. Setup B (0x72a3b0) copies them into the PP slot records. The instantiated TF
  gets the stage of its slot; `Get_Stage()` (0x5fda50) returns TF +0x100 as an AITarget [M: the copy from slot to
  TF +0x100 was not traced].

### Q3.2 Galactic reach vf+0x30 = PlanetReachabilityClass vf6 (0x664450)
- level == 4 (`Single_Hop_Disconnected`) → the target itself (+0x10).
- level < 6 (other values) → refresh the per-level planet list (0x665160), then return its **first** entry that is
  not in `exclude` and (when `include` is non-empty) is in `include`. The TF mask (+0x94) is **not used** in
  galactic.
- level ≥ 6, or no entry → null, so Setup A uses the target.

### Q3.3 Per-level list 0x665160(reach, level, force) and the flood 0x3f12e0
1. **Cache.** A level is recomputed only when forced or when its stamp (+0x1d8 + 4·level) is older than 10 × FPS
   frames.
2. **Request.**
   - Player = the reachability's AI player; destination = the target planet.
   - Flags: flag0 = flag1 = (level == 5); flag2 = (level 1 or 5); flag3 = level > 2; flag4 = level > 1;
     flag5 (fog) left at 1.
   - Corrupt_Planet and Sabotage_Planet have `Reachability Any` = 5 (AI_GOALS_UNDERWORLD_GALACTIC.XML), so every
     pass-through flag is set.
3. **Flood 0x3f12e0(finder, request, surfaceTest = 0, out, bitset, level == 4).**
   - With flag4 set, the start is not refused.
   - **Target held by the player or an ally (0x4ad9f0):** one breadth-first flood from the target.
   - **Target not held (the Naboo / MonCalimari case):**
     - each graph neighbour of the target, in ascending planet-graph index (edge order), that is passable and not
       yet visited starts its own flood;
     - the closed set is shared, so in practice the first passable neighbour's flood covers the whole connected
       graph;
     - the kept result is the flood with the **most** emitted planets (strictly greater, so the first wins ties).
4. **Passable (0x3f2400) at level 5.**
   - An AI player (not human) needs the planet revealed (0x3fa420), because of the fog flag.
   - Otherwise every planet passes: a passable owner is accepted with flag2; an impassable planet needs flag3, and
     the danger check or Underworld corruption.
5. **Flood 0x3f0ce0** is a breadth-first walk from its start:
   - each popped planet is **emitted** to the output list when it is held by the player or an ally (0x4ad9f0);
   - neighbours are queued in edge order, ascending planet index [M: exact queue order inside a layer].
6. So **stage = the planet held by the Underworld (or an ally) that is fewest hops from the target's first passable
   neighbour**. Ties go to BFS order. The hop is counted from that neighbour, not from the target. The neighbour
   itself is the stage when the Underworld holds it.

### Q3.4 Which planet in HotM Rebel
- Both targets are enemy-held, so the stage is the Underworld-held planet nearest (BFS) to the first revealed graph
  neighbour of Naboo or of MonCalimari.
- The port lane's own "stage from the target" try (`nearest_owned(p, target)`, hops from the target) gave **Shola**
  for both. Retail builds its early saboteurs at Shola (731, 1537). So Shola is the likely stage for both targets
  [M]. A hop tie between owned planets, or an unrevealed neighbour, could change it. The rule above settles it once
  the port runs it on the real graph.
- **Fog caveat.** If no neighbour of the target is revealed to the Underworld, the flood emits nothing and the stage
  becomes the **target itself** (Setup A's null fallback). The gather then paths to the target [M: reveal state at
  frame 116 unknown].

### Q3.5 Setup A distance key, galactic branch (PP +0x198 == 0) [H]
This is relevant to Q4, and some of it is new against AI_GALACTIC_2 §1.4:
- **A hidden unit (0x3a9760 true) gets no key: key = 0**, the best score. The unit is still eligible.
- No goal target → key 0.
- Otherwise the unit's planet is found by walking parents to a planet, or through the unit's fleet (0x432c30). The
  planet must be revealed (0x3fa420), else **the unit is not eligible**.
- key = Σ over TF stages (stage reach vf+0x38 cost from that planet, mode 4 if the goal Reachability is 4 else 3)²,
  counting only finite (≠ 1e18) costs, + Σ 2-D distance² (unit +0x78/+0x7c − stage position).
- When at least one cost was finite, key /= number of TFs. **When none was finite, the unit is not eligible**
  (galactic). Tactical uses the distance to the target instead.

### Q3.6 Port change
- `engine/ai/src/galactic_ai.cpp` line ~823 (`nearest_owned(p.player, def.ignore_target ? -1 : g.target)`):
  **drop the IgnoreTarget case.** Compute the stage from the goal target with the 0x665160 / 0x3f12e0 rule:
  - per-level flags; for an unheld target, flood from its first passable neighbour in graph-edge order and keep the
    largest flood;
  - emit planets held by the player or an ally in BFS order; take the first one not excluded (RequireUniqueStage)
    and, if required, included (RequireMatchingStage);
  - level 4 → the target; level ≥ 6, or nothing emitted → the target;
  - no goal target → no stage.
- This replaces `nearest_owned`'s "hops from the target, lowest index on ties" (`galactic_ai.cpp:311`), which
  differs whenever the target's first neighbour is not on the shortest route.
- Keep the 10 s per-level cache, or prove that recomputing gives the same list.
- `engine/ai/src/galactic_pp.cpp` (Setup A key):
  - a hidden unit has key 0;
  - a unit at an unrevealed planet, or with no finite stage cost, is not eligible.
- The Lua `_FindStageArea` / `find_stage_area` (`galactic_ai_impl.hpp:527`) is a different entry point and was not
  read here.

---

## Q4. Which of two same-pass Corrupt_Planet proposals keeps the free saboteur?

**Answer: the proposal accepted first in Resolve_Category, which is the higher effective desire: MonCalimari (25.5)
before Naboo (24.0). The distance key does not decide between goals. So the port's frame-116 assignment
matches the retail rules.** [H for the rules; the desire values are the port's]

### Q4.1 Order of operations
1. **Proposal** (Find_Plan_For_Goal → Init 0x6c0d00, solve counter PP +0x104 = 0).
   - Setup A ignores reservations on the first solve, so **both** PPs select the saboteur (net cost 0 each).
   - Nothing is reserved yet (A1_READINGS §28.6).
2. **Commit start** (0x6c8710 step 4, 0x6c0be0): every active PP's +0x30 reservations are cleared.
3. **Resolve_Category(Infrastructure):**
   - Pool = OldCand + proposals, sorted by effective desire, descending. Both proposals are new, so there is no
     incumbent bonus. std::sort is an insertion sort below 33 entries, so equal desires keep proposal order.
   - The greedy phase takes Pool[0] = **MonCalimari**. 0x6c4a50 re-solves (counter 1); the saboteur's +0x30 is 0,
     so the saboteur is selected. 0x6c2d60 then sets +0x30 = MonCalimari's PP.
   - The next accept is **Naboo** (greedy or weighted-random phase). Its re-solve skips the saboteur (+0x30 ≠ 0 and
     counter ≥ 1), so the type goes into PP +0x108 as null ("to be produced", net 1000). It is accepted only when
     the category can fund it.
4. **Distance key.** It is used only inside one PP, to choose among several eligible units of the same type
   (score `1 − key/maxKey + 2 (+2)`, best kept, ties to the earlier unit). It is not compared across goals. For a
   hidden saboteur the key is 0 in both PPs (Q3.5).

### Q4.2 Why retail still used the starting saboteur for Naboo (open; settled by Q5)
- **H1 [L]: different desires.** Retail's perception values give Naboo ≥ MonCalimari at that pass, or MonCalimari
  is not proposed then (Applies, Time_Limit, learning). The probe's desire log settles it.
- **H2 [M mechanism, L that it happened]: the saboteur comes back to the free store and Naboo's build task claims
  it.**
  - The MonCalimari plan can end early without corrupting anything:
    - Production_Failed → `ScriptExit`;
    - the gather or Move_To finds no path because of fog: with flag5 set, an AI path needs revealed endpoints;
    - unrevealed neighbours make the stage = MonCalimari itself (Q3.4).
  - Its TF then releases the unit (0x5ff610 → 0x6c27d0, re-admitted).
  - On free-store **admission** (0x4d2c90), every AIBuildTask of the AI that has no unit yet (task +0x68 == 0) and
    has not started production (0x6492e0 false), whose type (+0x88) is the unit's type, and whose plan allows
    free-store units (plan def +0xD0), **claims** the unit (0x6491f0). The claim is stored in the free-store
    entry's +0x18 vector. Naboo's saboteur build task, still waiting for credits or queued and not started, would
    take the starting saboteur this way. Retail would then build its first saboteur only later, for another goal
    (731, Shola), which matches the probe.
- **H3 [L]:** the MonCalimari plan reaches MonCalimari, but its corruption is not applied. This contradicts the
  first payment (+45 = Naboo's 90 × 0.5, not MonCalimari's 56.25), so it is unlikely.

### Q4.3 Port change
- None to the accept order.
- Check that `engine/ai` has the admission claim: a free-store admission hands the unit to a pending,
  not-yet-started build task of the same type in a plan with AllowFreeStoreUnits (AIBuildTask WAIT_FOR_FREESTORE_ALLOC
  path).
- Check the Setup A key rules of Q3.5.
- After Q1 and Q3 are fixed, rerun the trace. If the MonCalimari plan then reaches MonCalimari and corrupts it,
  retail differs earlier (H1) and the probe is needed.

---

## 5. Q5: what the a2f_budget probe kit has to log (Gabriel launches; restated, not done here)

Add `AI_Plan_Underworld_DeploySaboteur` to the kit's P channel (`tools/probe_mod/a2f_budget`). Run
`Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel with no orders, `--story` equivalent, frames 0..1400.
1. **PSTART:** frame, plan id, `MainForce.Get_Goal_Type_Name()`, the target planet name, the
   `MainForce.Get_Stage()` planet name (after AssembleForce computes it), and the desire if the kit's goal records
   expose it.
2. **Progress:** the results of `Produce_Force(stage)` and `BlockOnCommand(Move_To(Target))` (true / false / nil),
   and `MainForce_Production_Failed` if it fires.
3. **PEXIT:** frame, plan id, result (`Set_Plan_Result`, or the ScriptExit path), and `Target.Is_Corrupted()`.
4. **Every 30 frames** for each `Underworld_Saboteur_Team` the Underworld owns:
   - object id, planet (`Get_Planet_Location`), in transit (`Is_Unit_In_Transit`);
   - its parent fleet id and the fleet's member count, and whether the fleet is hidden;
   - `Is_Object_On_Free_Store` (H2 needs the moment it returns to the store).
5. **Proposal pass:** the Corrupt_Planet / Sabotage_Planet proposals, with target, desire and accepted or not, at
   the pass near frame 116 and the next two passes (H1).

The run settles Q1 (fleet member count after the stage arrival), Q2 (what Move_To carries), Q3 (Get_Stage) and Q4 /
H1–H2 (which plan holds the saboteur, and whether it re-entered the store). The 074145 run has no DeploySaboteur
record.

---

## 6. Open items and the evidence that settles them

| # | Open | Evidence |
|---|---|---|
| 1 | Which READY_TO_MERGE branch (0x10 or 0xd) does the TF merge, and what TF vf+0xb8 is | read the GalacticTaskForce vtable 0x8bfec0 slot 23 [M now; it does not change Q2's answer] |
| 2 | The copy of the Setup A stage into TF +0x100 (Get_Stage) | trace PP slot +0x40 → plan instantiation 0x6bad60 → TF +0x100 |
| 3 | BFS order inside a layer of 0x3f0ce0, and 0x4ad9f0 = "held by the player or an ally" | finish reading 0x3f0ce0's queue handling and 0x4ad9f0; or log `Get_Stage` in the probe (Q5 item 1) |
| 4 | The stage planet for Naboo / MonCalimari in HotM, and the Underworld's reveal state at frame 116 | run the Q3.6 rule on the port's graph; the probe's PSTART stage |
| 5 | H1 vs H2 (why retail's Naboo got the starting saboteur) | probe items 4 and 5 |
| 6 | Meaning of 0x6492e0 ("production started") in the admission claim | read 0x6492e0 / 0x6491f0 |
| 7 | Fleet data +0x4c (preferred slot): who writes it | search for writers of FleetDataPack +0x4c (only the ctor's −2 was found) |
