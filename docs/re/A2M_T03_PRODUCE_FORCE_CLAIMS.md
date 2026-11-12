# a2m t03: plan instantiation, plain `Produce_Force()` and free-store claims (2026-11-12)

Question set: why retail's Conquer_Pirate (Rebel, Heart of the Maw, frame 5518) leaves its free-store units available to the
Conquer_To_Reconnect plan started at 6925, while the port's copy takes them into its task force at instantiation.

Sources: `re_reference/ghidra` decompile (FoC 1.121 StarWarsG.exe), vtables read from the shipped exe. Builds on
A1_READINGS §28.4-28.6 and §33.2, A2K_T08 Q2.3, A2L_T01 Q7, A2L_T03 Q5.3, A2M_R2_RE_B §1 and §3.4, AI_GALACTIC §1.6.
Tags: [H] read in code, [M] one inferred link, [L] hypothesis.

Names used: PP = PotentialPlanClass; plan = PlanBehaviorClass (+0x60 plan definition, +0x88 goal); goal +0x68 = PP;
TF = task force (+0xf0 plan); BT = AIBuildTaskClass; entry = free-store map node (A1 §28.1, +0x30 reservation).

---

## Q1. Plan instantiation (0x5f7090): the free-store selections stay on the free store

Call path: goal goes to Planning, 0x6bad60 creates a plan (0x5f6870) and calls 0x5f7090(plan, goal +0x68 = PP). On
success it stores the plan in goal +0x60 (0x693970) and adds it to the execution list (0x725910). Neither touches units. [H]

0x5f7090, per task-force definition of the plan definition (PP +0x50 → +0x28 list), calls 0x66fb30(tfdef, plan, PP):
1. 0x6c0100(PP, tfdef) builds a list of (type, index) pairs: for every PP stage entry (PP +0xe8, stride 0x50) whose
   definition is tfdef, for every selection index j listed in it (entry +0x10, deduplicated by a bitset), the pair is
   (PP +0x58[j] = the selection's **type**, ordinal of that stage entry). In galactic (PP +0x198 == 0) every selection
   is listed, whether PP +0x108[j] holds a free unit or null ("to be produced"). Tactical lists only selections with a
   unit or with PP +0x120[j] == −1. [H]
2. 0x6c0750 (the reinforcement-pool list) is empty in galactic. [H]
3. The TF is created (galactic: 0x6cd360 → GalacticTaskForceClass, init vf+0x98 = 0x5fd2c0 stores only the definition
   at TF +0xe8 and the plan at TF +0xf0). [H]
4. For each pair, 0x5f9fd0(TF, type, index) appends a type wrapper (0x6051b0, class tag 0x804; the type is at wrapper
   +0x28) to **TF +0x60** and the index to TF +0x90. [H]
5. Back in 0x5f7090: TF +0x100 = stage of entry i (0x6c09b0), the TF goes into plan +0x68 and is bound to the Lua
   script by its name. [H]

Nothing in this path reads PP +0x108, calls Add_Force (TF member list TF +0x78, written by 0x5f9a90), or removes a unit
from the free store (vf+0xa0). **At instantiation a TF holds one wanted type per selection and no members.** The
selected units stay on the galactic free store with entry +0x30 = this PP (set at accept by 0x6c2d60 and re-asserted at
every commit while the goal is kept, A1 §28.4). [H]

`Are_All_Units_On_Free_Store` (Lua wrapper 0x5fa090) returns "PP +0x108 has no null slot" (0x6bffe0). It does not look
at the free store or the TF. [H]

## Q2. `Produce_Force` (0x5fe240) makes one build task per TF +0x60 entry

0x5fe240 (GalacticTaskForceClass vf21) loops over **all TF +0x60 entries** and calls
0x524ee0(execution system, type, destination, TF, TF +0x90[i], sync flag) for each, adding every task to a new
ProduceForceBlockStatus (0x6c6500, tag 0x80b) through 0x6c6630. It does not skip types that a free unit will fill. [H]

0x524ee0 allocates the BT, 0x649200 fills it (+0x88 type, +0x98 destination, +0xa0 TF, +0x90 index, +0xda sync) and
runs its FSM at once up to 50 transitions: state 0x11 WAIT_FOR_CREDITS enter → 0x12 BUILD_TASK_LIST enter → 0x648980. [H]

BUILD_TASK_LIST 0x648980, galactic:
- plan definition +0xa8 set → BUILD_STATE_HIJACK_UNIT (0x17), no market entry. [H]
- otherwise it creates the task's AIPlanetBuildTask (+0xc8, 0x695720 with type and destination); structure types
  (behaviours 0x5b / 0x5c / 0x5f / 0x60, or type +0x894) are charged and go to PRODUCTION_QUEUED without a claim. [H]
- otherwise, with AllowFreeStoreUnits (plan definition +0xd0), 0x4d2bd0 makes the task a claimant of **every** map
  entry whose representative type equals the task's type, with no reservation test. Any match: state 2, uncharged,
  +0x7c = 0. No match: charged when possible, +0x7c = 1, state 2. [H]

So with 7 Y-wings to build and k free units of other types, Produce_Force makes 7 + k tasks. A task for a free
selection is not tied to "its" unit: every task of a type claims every entry of that type. Which task receives a unit
is decided by the service (Q3). [H]

## Q3. A claim task with no destination (`Produce_Force()`, BT +0x98 == 0)

**Hand-over.** The free-store service 0x4d5a70 (every 3 frames) walks the entries in list order and each entry's
claimants in claim order, skipping claimants that ended, already hold a unit or have issued production. Galactic
vf+0xb0 = 0x690240 returns 2 (claim lapses, 0x649070) when entry +0x30 is not the claimant's PP (TF +0xf0 → +0x88 →
+0x68), 0 when the unit's parent is a planet or a fleet at a planet and the unit is not in transit (0x68efb0), else 1
(wait). On 0 it calls 0x647df0(BT, unit): BT +0x68 = unit, the AIPlanetBuildTask is dropped and a charge refunded,
state = 0x16 WAIT_FOR_FREESTORE_ALLOC; all other claims on the entry lapse and the entry is **erased** (galactic free
store +0x48 = 1). [H, A2L_T01 Q7.4 re-read]

So a reserved unit is handed over only while its reservation is this PP and it sits at a planet, not in transit. A unit
on the move keeps its reservation and its claims until it arrives. [H]

**WAIT_FOR_FREESTORE_ALLOC update (FSM 0x64aec0, state 0x16)** waits while the unit has no parent, a parent that is
neither planet nor fleet, a fleet with no parent, or is in transit. Then, with +0x98 == 0: [H]
1. 0x5f9f90(TF, type): TF +0xa8 per-type counter += 1 (map insert or increment). [H code; M "delivered count"]
2. state = 8 PRODUCTION_FINISHED.
3. 0x6c27d0(PP, unit). The unit is not in the map (erased at hand-over), so it is **re-admitted** (free store vf+0x98:
   fresh entry, +0x30 = 0, claim scan, Lua `On_Unit_Added` → GalacticFreeStore `MoveUnit`). Then the unit is removed
   from PP +0x108 together with its slot in PP +0x58 / +0x70 / +0xb8 / +0x120 / +0x168 / +0x180, and the stage index
   lists are renumbered. If the goal has no PP, the unit is just admitted. [H]

The unit is **never a TF member** on this path: Add_Force is called only by PRODUCTION_END (0x647ee0 → TF vf+0xa0 =
0x6cd1a0 → 0x5f9a90), which the null-destination branch never reaches. It is off the store for at most the time
between the service hand-over and the next FSM update. [H]

**PRODUCTION_FINISHED enter** → 0x64ad00: drops the AIPlanetBuildTask, sets +0xa8 = 1 (ended), unsubscribes, state
0x15 ALL_FINISHED, and since +0xD8 == 0 (no Add_Force happened) calls free store vf+0x98 on the unit again. The unit is
already on the store, so the insert is refused and nothing happens. [H; no-op per A2L_T01 Q7.2 step 1]

**Produced units with no destination** (production-complete signal 4 → 0x6498f0): the new object (through 0x432710
when it has behaviour 4) is admitted to the free store, TF +0xa8[type] += 1, state 8. BT +0x68 stays 0, so 0x64ad00
does not re-admit. Produced units also never join the TF. [H] (With a destination the same function removes the unit
from the store, vf+0xa0, and goes to PRODUCTION_END → Add_Force, +0xD8 = 1, so 0x64ad00 does not re-admit it.) [H]

**What `BlockOnCommand(Produce_Force())` waits for.** ProduceForceBlockStatus IsFinished (vf19 = 0x6c6720; slot role
per A1 §33.2) drops every task with +0xa8 set and reports finished when the list is empty. So it waits until **every**
task has ended: the claim tasks (a few frames once their units are at a planet), the produced ones (production
finished or failed; PRODUCTION_FAILED also calls 0x5fdd90(TF, type), [M] the TF's production-failed event). [H]

Consequence for the Rebel Shola run [M]: within a few frames of 5518, every free unit Conquer_Pirate had selected and
that sat at a planet was back on the free store, unreserved and gone from its PP, while the thread stayed blocked on
the Y-wing builds. That is why Conquer_To_Reconnect at 6925 found the frigates, corvettes and heroes on the store
with goal cost 0.

## Q4. Can a newer goal take units of a plan that is still blocked?

Setup A (0x7287a0) iterates **only the free-store map** (free store +0x40) and admits an entry when PP +0x104 == 0
(first solve) or entry +0x30 == 0. A unit not on the store (a TF member, or a unit between hand-over and
re-admission) is never a candidate of any PP. [H]

Two cases for a unit of the blocked plan:
1. **Already handed back (Q3)**: the unit is on the store with +0x30 = 0 and is no longer in the old PP +0x108. Any
   goal's Init solve and re-solve may select it; the accept reserves it; the old goal's validity test 0x6c45e0 does not
   look at it, so the old goal is not invalidated. [H]
2. **Still reserved** (claimed but in transit, or not at a planet): an Init solve (counter 0) may select it. At accept,
   the new PP's re-solve 0x6c4a50 (PP +0x100 == 0 → full solve 0x6c3da0, counter ≥ 1) can keep it only if entry
   +0x30 == 0 at that moment. All reservations are cleared at commit start (0x6c0be0, step 4) and restored only when
   the old goal is processed. So the steal succeeds when the new goal is accepted **before** the old goal is
   re-asserted: an earlier category in the commit order, or the same category while the old goal sits in OldCand
   (replaceable and effective desire below the best proposal; only kept goals get 0x6c2d60 at classification). [H]
   After the steal, 0x6c2d60 sets +0x30 = new PP. When the old goal is then checked (keep test 0x6c47c0 → 0x6c45e0, or
   0x6c4a50 with PP +0x100 set = validity only when an OldCand is re-accepted from the pool), the stolen unit has
   +0x30 ≠ 0 and ≠ old PP → invalid → released (0x614880, deactivation), unless plan definition +0xa8 is set. A
   released goal's plan ends and its Lua thread is gone. [H mechanism; M "thread killed" via plan end]

A new goal accepted in a later category than the old one, or after the old one was kept in the same category, cannot
take its reserved units: the old reservation is already back and the re-solve skips +0x30 ≠ 0. [H]

## Q5. Other things that decide whether a running plan's units can be taken

- **Replaceable** 0x692740(goal): false only when the goal has a plan whose +0x98 == 0 (`Set_As_Goal_System_Removable
  (false)`, 0x5fdbd0 → 0x5f8360) and plan +0x80 == 0. Plan constructor sets +0x98 = 1, so plans are replaceable by
  default. Neither CONQUERPIRATEPLAN.LUA nor CONTRASTTORECONNECT.LUA calls it. A replaceable goal whose effective
  desire is below the best proposal becomes OldCand: unreserved during that category's greedy phase, and released at
  finalize when not re-accepted. [H]
- **Plan definition +0xa8**: keeps a goal whose PP is invalid (Resolve 0x6c8970 and maintenance 0x722a30) and sends
  its build tasks to HIJACK_UNIT. [H] Which XML tag sets it was not read. [open]
- **PP +0x4d**: plain `Produce_Force()` clears it (galactic, null destination). 0x6c47c0 then skips, for that PP, the
  per-commit recompute of its force-versus-enemy tables (0x6c4b20 and the diagnostics call). Validity (+0x4c) is not
  affected. [H code; M purpose]
- **PP +0x100 / +0x4e** (A1 §28.3): +0x100 makes 0x6c4a50 a validity test instead of a re-solve; +0x4e only guards
  double suspension. No other "units committed" flag on the plan or PP is read by Setup A or the validity test. [H]
- **Lock_Current_Orders** (obj +0x3a8): a locked unit is not a Setup A candidate (A1 §28.2). [H, earlier read]

---

## Port rule

1. **Instantiation**: create each task force with one wanted entry (type) per selection of its PP, free or to be
   produced, and **no members**. Leave the selected units on the free store, reserved by the PP (+0x30), and keep
   re-asserting the reservation at each commit while the goal is kept. `Are_All_Units_On_Free_Store` = "the PP has no
   to-be-produced slot".
2. **Produce_Force**: one build task per wanted entry. Each task with AllowFreeStoreUnits claims every free-store unit
   of its type; claimed tasks stay uncharged and out of the market. The 3-frame service hands a unit to its first
   claimant whose PP holds the reservation, only when the unit is at a planet and not in transit; other claims on it
   lapse (lapsed tasks are charged and bid).
3. **No destination (`Produce_Force()`)**: a task that receives a free unit puts it straight back on the free store at
   its next update (fresh admission, unreserved, `On_Unit_Added` runs), removes that selection from the PP, counts it
   as delivered and ends. A produced unit goes to the free store too. The task force never gets members. The blocked
   thread resumes when every task has ended (all claims handed back and all builds finished or failed).
   **With a destination** (`AssembleForce` → `Produce_Force(stage)`): units leave the store and join the task force
   (Add_Force), as before.
4. **Newer goals**: Setup A sees only free-store units. Handed-back units are plain free units for everyone. A unit
   still reserved by a blocked plan can be taken only by a goal accepted before the old goal's reservation is
   restored in that commit; the old goal then fails its validity test at its keep / re-accept check, is released, and
   its plan and thread end. Non-replaceable plans and plan-definition +0xa8 change this as in Q5.

## Open

- Why retail released Conquer_Pirate before its last builds finished (no PDONE / PEXIT): case 1 means stolen
  handed-back units cannot invalidate it. Candidates [L]: OldCand not re-accepted after Conquer_To_Reconnect was
  proposed with a higher effective desire; maintenance (funding 0x6129c0 or AllowedTime); or a unit still in transit
  stolen per Q4 case 2. A probe that logs the Conquer_Pirate goal's desire, state and release reason around 6925 would
  settle it.
- Reader of TF +0xa8 (per-type delivered counter) not read; it may feed `Get_Force_Count` or the production-done test.
- Writer of plan +0x80 (second "not replaceable" input) and the XML tag behind plan definition +0xa8 not read.
- A claimed unit destroyed or captured before hand-over (entry erased, 0x4d5150 prune): whether the task then
  produces or fails was not read.
- Order of the BT FSM update relative to the free-store service within a frame (decides whether the hand-back is the
  same or the next frame) not read [M: next update].
