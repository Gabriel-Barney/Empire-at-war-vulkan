# A2l t01: RE answers for the Underworld credit gap (2026-11-08)

Roadmap a2l t01: Underworld credits at frame 9000 are 324 in the port and 2462..2849 in retail; the port's Underworld
spends about 2400 per fiscal cycle, retail about 1400. Three candidate causes were handed to RE: the saboteur
free-store admission claim (H1 / H2 of `A2K_T08_RE_ANSWERS.md` Q4.2), what `GalacticTaskForce.Move_To` moves, and the
budget pool sizes (port cycle 1 Offensive 886 / Infrastructure 1309, retail 103 / 405).

Source: the Ghidra decompilation of the FoC x64 `StarWarsG.exe` (RVAs relative to 0x140000000) and the vtables read
from the same exe. Behaviour is described in prose; no decompiled code is reproduced. Tags: [H] read directly; [M] one
inferred link; [L] hypothesis.

Short answers:
- **Q7.** Admission claims are registrations only. They cost and reserve nothing. A claim is honoured only for a
  unit that the task's own PotentialPlan already reserves. So **H2 cannot happen** in retail, and the port's "take
  any free unit of the type" in `create_build_tasks` is not retail.
- **Q8.** Move_To moves **one** fleet, the elected FleetObject (TF +0x120). It never moves every fleet of the task
  force.
- **Q9.** The pool formula is the one in AI_GALACTIC_2 §3.1, re-read here unchanged. The retail "cycle start" values
  are **cached up to 5 s** and so are pre-payment values. The 2–9x gap is a measurement artefact, not a budget rule.

---

## Q7. The free-store admission claim (0x4d2c90), 0x6492e0, and H1 vs H2

### Q7.1 What admits a unit (galactic) [H]

Galactic Add_Object is GalacticFreeStoreClass vf19 = vtable 0x8b63f8 slot 19 (+0x98) = **0x68ece0**. It calls the
base admission 0x4d2c90 only when all of these hold:
1. The object's owner (political record 0x3956c0 +4) is the free store's player.
2. The object is not a planet (+0x335 == −1).
3. It is not a fleet (+0x336 == −1), or it is a fleet whose parent exists and is not a planet (a fleet at a planet is
   refused).
4. It has no DUMMY_GROUND_STRUCTURE (0x5F, +0x391) and no DUMMY_ORBITAL_STRUCTURE (0x60, +0x392) behaviour.
5. Its type is neither a squadron member (type +0x29) nor a company member (type +0x2a).

Callers of vf19:
- the initial fill;
- the PP release 0x6c27d0, when a unit leaves a task force: `Release_Unit`, `Release_Forces`, plan end, or a build
  task that took the unit (A1_READINGS §28.4);
- 0x6bf3d0 (land Reinforce).

A re-admitted unit is a **new** admission. It goes through every step below again, including the Lua call.

### Q7.2 What 0x4d2c90 does [H]

Admission is refused (returns 0, nothing happens) when the object is null, has +0x3a0 bits 0x02 / 0x40 / 0x80 set, or
has +0x3c8 == 0. Otherwise:
1. Insert into the unit map (free store +0x40, key = object id). If the id is already present: return 0, no other
   effect.
2. Subscribe the free store to the object's signals 0x10 (destroyed) and 0x1a (owner changed).
3. Per-type counter (map at free store +0xa0, key = type +0x11c of 0x4d3900(object)) += 1. 0x4d3900 is the unit's
   representative type: its squadron, company or team type when it has one, else its own type (+0x298).
4. **Claim scan.** For each AIBuildTask in the execution system's task list (free store +0x38 → +0x40, list order),
   the task is a claimant when:
   - it has no unit yet (BT +0x68 == 0);
   - its production order has not been issued (0x6492e0 false, §Q7.3);
   - its type (BT +0x88) equals the representative type;
   - it has a task force (BT +0xa0), whose plan's definition has AllowFreeStoreUnits (TF +0xf0 → +0x60 → +0xd0).

   For **every** claimant (there is no break), 0x6491f0 runs: BT claim count +0xd4 += 1 and BT +0x7c ("to be
   produced") = 0. The task is then appended to the new entry's claimant vector (node +0x18).
5. Subclass hook vf+0xb8. In galactic it is an empty function (vtable slot 23 = 0x1fa90).
6. Lua: when the free store's context mode equals the current game mode (DAT_140b15418 vf+0xe0) and the free store has
   a script (+0x80), `On_Unit_Added(unit)` is called synchronously. The shipped `GalacticFreeStore.lua` calls
   `MoveUnit(object)` there: `Find_Custom_Target` for a hero, then `Find_Ground_Unit_Target` / `Find_Space_Unit_Target`,
   then `FreeStore.Move_Object`.
7. Returns 1.

The new entry's reservation (node +0x30) is 0 (A1_READINGS §28.1).

**Credits.** Admission makes no budget call and no credit change [H]. The only budget side effect is indirect: a
claimant's +0x7c is cleared, so its AIPlanetBuildTask stops bidding in the market (A2J_RE_BUILD_MARKET §3) until the
claim lapses.

### Q7.3 0x6492e0 and 0x6491f0 [H]

- **0x6492e0(BT) = (BT +0x78 ≠ −1)**: the task's **production order has been issued**. BT +0x78 is the production
  request id. Two places write it:
  - 0x64abf0, called by the planet market 0x53cd60 when it starts the task's production. It also drops the task's
    AIPlanetBuildTask (+0xc8), stores the producing object at +0x70, subscribes to its signals 2 / 3 / 4, calls
    0x693170 on the goal budget, and issues the Production event with that id (0x5244e0).
  - BUILD_TASK_LIST 0x648980, in its other branch (`FUN_14028afb0` true and AllowFreeStoreUnits off): a global
    counter DAT_140b3995c.

  So "not started" means not yet handed to a factory queue. A task that is still bidding or waiting in the market has
  +0x78 = −1 and can claim. This settles A2K_T08 open item 6.
- **0x6491f0(BT)**: claim count +0xd4 += 1; +0x7c = 0.
- **0x649070(BT)** (a claim lapses): if +0xd4 > 0, decrement it. When it reaches 0:
  - 0x64ab50(BT, 1) sets +0x7c = 1 again. When the task has an AIPlanetBuildTask (+0xc8), has not been charged (+0xd0
    == 0) and has not ended (+0xa8 == 0), it charges the goal budget once (Allocate 0x691950 → 0x6110e0, the type's
    build cost, +0xd0 = 1).
  - Tactical only (no +0xc8 and the plan's context mode ≠ 0): the task fails (+0x28 = 1, state 7).

### Q7.4 When a claim is honoured: the free-store service (0x4d5a70, every 3 frames) [H]

For each map entry (list order), for each claimant in its vector (vector order = claim order), skipping claimants that
ended (+0xa8), already have a unit, or whose production order was issued:
1. r = vf+0xb0(store, unit, BT, entry). Galactic = **0x690240**:
   - **2** when the entry's reservation (node +0x30) is not the PotentialPlan of the task's plan (TF +0xf0 → +0x88 →
     +0x68);
   - else **0** when the unit has a parent that is a planet, or a fleet that is at a planet, and the unit is not in
     transit (`Is_Unit_In_Transit` 0x68efb0 false);
   - else **1**.
2. r == 2: the claim lapses (0x649070) and the task leaves the vector.
3. r == 1: keep waiting (next service).
4. r == 0: **0x647df0(BT, unit)** gives the unit to the task:
   - BT +0x68 = unit;
   - subscribe to its signal 0x10;
   - drop the task's AIPlanetBuildTask (0x695560, then release);
   - if the task had been charged (+0xd0), give the charge back (0x6931e0 on the goal budget);
   - state = 0x16 WAIT_FOR_FREESTORE_ALLOC in galactic (3 in tactical).

   Every other claimant of the entry gets 0x649070 (its claim lapses). Because galactic free store +0x48 = 1 (ctor
   0x4d22b0 with argument 1, from 0x68eaf0), the entry is erased: signals unsubscribed, counter maps updated. The unit
   leaves the store.

**The other claim site, 0x4d2bd0(store, BT)**, called by BUILD_TASK_LIST 0x648980 when AllowFreeStoreUnits is set:
- for every map entry whose representative type is the task's type, the task claims it (0x6491f0, appended to that
  entry's vector). Reservations are not checked;
- it returns 1 if any entry matched. The task then goes to PRODUCTION_QUEUED (state 2) **uncharged**, with +0x7c = 0
  and its AIPlanetBuildTask on the pending list;
- if no entry matched, the task is charged and +0x7c = 1, as in AI_GALACTIC_2 §2.3.

### Q7.5 What this means for H1 / H2 (A2K_T08 Q4.2)

**H2 is refuted [H].**
- When the MonCalimari plan releases the starting saboteur, 0x6c27d0 re-admits it with a fresh entry, reservation 0.
  The pending Naboo saboteur task does claim it, because it has no unit and no issued production. That claim clears
  the task's +0x7c for at most one service interval (≤ 3 frames).
- At the next service, 0x690240 compares the entry's reservation (0) with Naboo's PotentialPlan and returns 2. The
  claim lapses. +0x7c = 1 again, and no second charge is made, because the task was charged at BUILD_TASK_LIST.
- A unit reaches a build task this way only when the task's own PP already reserves it. A PP reserves only the units in
  its PP+0x108 list (0x6c2b20 at accept and at each kept-goal re-assert, A1_READINGS §28.4). Naboo's list holds null
  ("to be produced") for that slot. No path re-solves a running goal's PP to pick up a newly freed unit [M: 0x722a30
  maintenance was not re-read here, but §28.4 lists every +0x30 writer].
- So the H2 path ends with the saboteur on the store, unreserved. On_Unit_Added sends it to a GalacticFreeStore
  target.

**H1 remains** (Naboo was proposed with the higher desire, or MonCalimari was not proposed at that pass). Only the
a2k t05 probe (`run_a2f_sab.cmd`) can tell, through its desire channel and the DeploySaboteur PSTART target.

### Q7.6 Port differences found while reading (for t01)

1. **`create_build_tasks` (engine/ai/src/galactic_build.cpp ~l.119, `free_store_supply`) takes any free unit of the
   type off the store, at once and with no charge.** Retail rules (§Q7.4):
   - claim every matching entry;
   - stay uncharged and out of the market while a claim is pending;
   - hand over only a unit reserved by this task's own PP that sits at a planet and is not in transit;
   - otherwise let every claim lapse at the next 3-frame service, then charge the budget (if not yet charged) and
     produce.

   The port's version steals units reserved by other plans: those plans' validity check 0x6c45e0 then fails. It also
   steals unreserved units that retail would leave to the free store. And it skips production that retail pays for.
   This changes both plan choice and spending.
2. **`On_Unit_Added` on re-admission.** The port calls it only the first time an object is seen
   (`claim_units`, `known` set). Retail calls it on every admission, including a unit a task force releases
   (§Q7.1, step 6), so a released unit is moved by `MoveUnit` at once.
3. The free-store service is the only place that hands claimed units over, and it runs every 3 frames (0x4d5a70 from
   Execution, AI_GALACTIC §1.10).

---

## Q8. Galactic Move_To for a task force: every fleet, or one elected fleet?

**Answer: one fleet, the task force's FleetObject (TF +0x120), with everything in it.** Members in other fleets are not
moved [H]. A2K_T08 Q2.1 / Q2.2 is confirmed and extended here.

### Q8.1 Move_To 0x6cf370 [H]
1. Fails with "No units in TaskForce %s, or no FleetObject" when:
   - TF +0x120 is null or does not resolve (0x5791e0);
   - the fleet has +0x3a0 bit 2 set (being removed);
   - the member vector (TF +0x78..+0x80) is empty.
2. Fails when the fleet has no parent ("not at any planet") or the argument is not a planet target (DAT_140a43b18).
3. Clears the fleet behaviour's raid target (0x435750(fleet behaviour, fleet, 0)).
4. Creates one MovementBlockStatus (0x6aed50(0x808)) and calls 0x6af230(block, TF, **TF +0x120**, destination,
   mode = goal type Reachability).
5. 0x6af230 stores that single fleet at block +0x48 and subscribes to the fleet's signals 9, 10, 0xb, 0x11, 0x10 and
   0x1b. Then 0x6aebf0 issues the first hop. Every hop moves that fleet only (A2K_T08 Q2.1).

Nothing in Move_To or in the block walks the member list.

### Q8.2 How the fleet is chosen [H]

1. **First choice:** `Add_Force` 0x6cd1a0. When TF +0x120 is null and the added unit's parent is a fleet (+0x336 ≠
   −1), +0x120 = that fleet's wrapper. The TF subscribes to the fleet's signals 0x10 and 0x1b, and always to the
   unit's 0x1b. Later adds do not change +0x120.
2. **Re-election:** signal handler 0x6cf810, signal 0x10 (destroyed).
   - When it comes from the FleetObject, or the FleetObject no longer resolves, the TF unsubscribes, drops +0x120 and
     walks the member list (TF +0x78, add order).
   - The new FleetObject is the parent fleet of the **first** member whose parent is a fleet that is not being
     removed (+0x3a0 bit 2 clear). The TF subscribes to its signals 0x10 / 0x1b.
   - Before the walk the TF calls 0x6006b0(TF, 3, 0) [M: a TF event, not read]. With the member list empty, no fleet
     is elected.
3. Signal 0x1b [M: "container changed"]:
   - from the FleetObject → TF vf+0xb0(1.0);
   - from a member → TF vf+0x188(member), then result 7 when no member is left.

### Q8.3 How several fleets become one

Units of one task force end up in one fleet only through the merges, not through Move_To:
- `Form_Units` (0x6cd5b0) merges the parent fleet of each member that is not the FleetObject, once per distinct fleet,
  op 3 in member order;
- the Produce_Force build task's DO_TASK_FORCE_MERGE / JOIN_TASK_FORCE at the stage (A2K_T08 Q2.3).

A plan that calls Move_To with members in several fleets leaves all but the FleetObject behind. They stay in the task
force and are still counted by `Get_Force_Count`.

**Port change:** the galactic task-force move should move only the elected FleetObject: the parent fleet of the first
added unit, re-elected on its destruction as in Q8.2. Today the port moves every fleet that holds a member.

---

## Q9. What sets the per-cycle Offensive / Infrastructure pools

### Q9.1 The rule (re-read, unchanged) [H]

The accrual is 0x614340(budget, income). It runs once per fiscal payment, right after the player's own `Add_Credits`
(notify off), only in galactic and only when the income period T (0x4b2010) is > 0. The code matches AI_GALACTIC_2
§3.1 line by line:
- `credits` = player +0x70, read on entry, so it includes this payment;
- Σ starts from the refund accumulator +0x80;
- per category: P = A; I = applied weight × income / T; then the per-goal terms;
- second pass: P += (credits − Σ) × w, with w = the normalised weight +0x3c (P = Σ × w first when the reset flag
  +0x74 is set).

The port's `AiBudget::tick` is a faithful copy.

Two identities follow from the code. They are what the port should compare.
1. **Right after the payment, Σ over the member categories of P = credits − (+0x80).** The refund accumulator was 0 in
   every retail watcher run (A2F_T07 §1.5, A2G_T01). So the pools add up to the credits just paid into.
2. **Between payments P is a ledger.**
   - At the next commit the reweight sets A = P − Σa. Each re-add then takes need = req⁺ + alloc − a. The shortfall and
     rate clamps of 0x610d30 move amounts between A and c only. So the next accrual's A + Σ(c + a_old) gives back
     exactly what the commit took.
   - Summing the per-goal terms, P_k(t) = P_k(t−1) − paid_k + Σ_{g∈k} share_g·T + w_k × drift. paid_k = the
     installments the category's goals paid (0x615700: P −= p). drift = credits − Σ.
   - A zero-weight category (the Underworld's Always / Defensive / Information / Hero / StoryArc are `Zero` in
     `AI_TEMPLATES_UNDERWORLD.XML`) has I = 0 and gets no drift. Its P only falls by its goals' installments.
   - Notify-on `Add_Credits` adds x × w to every P (A2K_T10).
   - Spends that bypass the budget move only the credits; the next drift takes them out of the weighted pools by w.
     Examples: the corruption charge 800 at frame ~600 (A2K_T08 §1) and the no-goal installment branch, which takes
     the money from +0x80 instead of a pool.

So for the Underworld (two weighted categories), right after a payment:
P_off + P_infra = credits − Σ P_zero (≥ credits, since every P_zero ≤ 0), split by the normalised weights of
`Underworld_Offensive_Budget_Allocation` / `Underworld_Infrastructure_Budget_Allocation` (plus each category's own
share and installment history).

The *available* credits a commit can give to goals are A = P − Σ_{goals}(req⁺ + alloc) (AI_GALACTIC_2 §3.2–3.3). They
are not P.

### Q9.2 Why "port 886 / 1309 vs retail 103 / 405" is not a budget gap [H mechanism, M for the specific rows]

The retail pools are decoded from `BudgetFractionToBuild` (0x667120). That leaf reads P (+0x4c) and I (+0x50)
(A2H_GALACTIC_AI_RE §2) and has a **5 s cache lifetime** (AI_GALACTIC_TOKENS table, lifetime column). A sample taken
within 5 s of the last recompute returns the old P. The retail samples show this directly in
`probe_results/a2f_budget_20261105.txt` (Empire, first payment at 1351):

| frame | credits | decoded OFF | decoded INFRA | decoded MAJOR | note |
|---|---|---|---|---|---|
| 1334 | 1759.92 | 293.31 | 680.83 | (894.34) | before the payment |
| 1358 / 1389 / 1419 | 2357.0 / 2266.04 / 2178.02 | 293.31 | 680.83 | 894.34 → 968.95 | after the payment, OFF / INFRA unchanged = **cached** |
| 1453 | 2078.26 | 528.02 | 581.30 | 968.95 | refreshed: 528.02 + 581.30 + 968.95 = **2078.27 = credits** |
| 3637 | 1965.86 | 588.50 | 333.79 | 1043.57 | sum 1965.86 = credits |
| 4123 | 2633.36 | 847.59 | 667.59 | 1118.19 | sum 2633.37 = credits |

So retail obeys identity 1 whenever every category's value is fresh.

The a2j report (`a2f_report.py` `render_cycles`) takes "the first retail sample at or after each payment". With the
kit sampling every frame from 44.5 s, that sample usually holds the P cached **before** the payment. The port column is
the port's own P at that frame, **after** its payment. That is why the port's cycle-1 pair adds up to its payment
(886 + 1309 = 2195, the port's first Underworld payment, A2K t08), while retail's adds up to 508 with 2150 just paid.

The 2–9x ratio therefore compares a post-payment value with a pre-payment one [M for the 074145 rows, which were not
re-read here; H for the cache and for the identity in the 20261105 run].

### Q9.3 What to compare instead (t01)

1. Compare like with like:
   - retail: the first sample whose decoded value **changed** after the payment (the refresh), or the last sample
     before the payment;
   - port: its P at the same frame.
2. A real difference then comes only from:
   - credits at the payment;
   - the normalised weights (the two Underworld equations: `PlanetsControlled` vs the human, `Has_Enough_Space_Force`,
     `PlanetsCorrupted`, structure presence, `Should_Save_For_Corruption`);
   - each category's installments paid (paid_k);
   - spends that bypass the budget.
3. The port's Underworld entering each payment near 0 credits while retail holds 2462..2849 at 9000 is a **spending**
   difference. The pool rule turns any credit level into pools that add up to it. Look for what the port pays and retail
   does not:
   - production retail never starts: Q7.6 item 1 decides which units retail produces and which it takes from the
     store;
   - plans retail does not accept;
   - Move_To taking extra fleets into battles (Q8): lost units mean rebuilds.

### Q9.4 Probe, if the decoded values are still needed

`BudgetFractionToBuild` cannot be read faster than its 5 s cache. A kit that wants P right after a payment must
evaluate it for the first time in the cycle **after** the payment. For example: sample once at payment + 1 frame with a
binding set (probe type / category) not used in the 5 s before. Or read +0x4c through the watcher (`a2f_watch.py`
already follows player +0x360 → … → budget +0x190; the category list is budget +0x30, each node +0x18 → category, P at
+0x4c).

---

## Open items

| # | Open | Evidence that settles it |
|---|---|---|
| 1 | H1 (desire / proposal order) for the Naboo corruption | a2k t05 probe (`run_a2f_sab.cmd`): desire channel and DeploySaboteur PSTART target |
| 2 | Meaning of signal 0x1b in the TF handler (container change) | read the emitters of signal 0x1b |
| 3 | No re-solve of a running goal's PP picks up a newly freed unit | re-read 0x722a30 maintenance |
| 4 | The 074145 cycle rows themselves (Q9.2 uses the 20261105 run) | re-run `a2f_report.py` section 8 with the refreshed-sample rule of Q9.3 |
