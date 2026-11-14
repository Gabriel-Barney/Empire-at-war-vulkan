# A2m t04: RE answers for Setup A exclusions, Production setup_c, reach thresholds, Produce_Force, paid() and Land_Units (2026-11-09)

Roadmap a2m t04 asked six questions. They come from the open ends of `A2L_T03_RE_ANSWERS.md` (Q4, Q5, open item 2),
`A2L_T01_RE_ANSWERS.md` and the a2l port notes:
1. the Setup A exclusions for galactic units (obj +0x210 (+8 / +0x34), +0x3A7, +0x3B4, lock 0x39ad40, the
   TACTICAL_BUILD_OBJECTS attach +0x35B → 0x42e3b0 / 0x42e3d0), and which planet the in-transit fleet's +0x50 means;
2. Production setup_c 0x664210: does it test "revealed" 0x3fa420 or "surface accessible" 0x375220 (the port vetoes
   Interceptor4 at The_Maw; retail builds it at tick 88);
3. the GameConstants reach thresholds for levels 7-9;
4. `Produce_Force` with an explicit nil, and the clear of stage-object byte +0x4d;
5. `paid()` (+s, 0x5f9f90) and the stored build-task charge;
6. `Land_Units` 0x6cec80 at an enemy planet with an accessible surface: does it land or refuse.

It also covers, for t05, the hardpoint minimum in the Setup A health test (what exactly is compared).

Source: the Ghidra decompilation of the FoC x64 `StarWarsG.exe` (RVAs relative to 0x140000000; GameConstants
base 0x140b15800), vtables and constants read from the same exe, and a capstone disassembly where the decompiler
dropped call arguments (Setup C's flag at 0x725db2). The shipped XML and scripts were also read (`GAMECONSTANTS.XML`,
the GARRISON_* behaviour lists, every `Lock_Current_Orders` / `Set_In_Limbo` / `Reachable_Target` caller). Behaviour
is described in prose; no decompiled code is reproduced. Tags: [H] read directly; [M] one inferred link;
[L] hypothesis.

Object fields used below: behaviour slot bytes sit at obj +0x332 + id (−1 = absent): +0x335 PLANET (3), +0x336 FLEET
(4), +0x337 TRANSPORT (5), +0x348 TEAM (0x16), +0x35b TACTICAL_BUILD_OBJECTS (0x29), +0x361 SPECIAL_WEAPON (0x2F),
+0x369 LOBBING_SUPERWEAPON (0x37), +0x383 GARRISON_UNIT (0x51). Also obj +0x2b0 parent object, +0x78 / +0x7c
position x / y, +0x298 type, +0xb8 planet data (D), +0xc0 FleetDataPack, +0x2d0 hardpoint list, +0x5c health.
Diplomatic relation: player +0x370[other owner] is 0 for self or ally, 1 enemy, 2 neutral.

Short answers:
- **Q1.** None of the garrison / limbo / hero-clash / lock / build-pad tests can exclude a galactic unit: the
  fields are written only by tactical code (garrison packs, transport landing, tactical battle setup, hero clash,
  hyperspace arrival, tactical Lua). A unit in a **moving fleet is not excluded**. In transit, fleet pack +0x50 is
  the move's **final destination** (not the source), and Setup A uses it as the unit's planet.
- **Q2.** 0x664210 tests **revealed** (0x3fa420), a production object at the planet (+0xd0) and the full can-build
  gate 0x2804d0. There is **no surface test** of its own; the surface clause inside 0x2804d0 lets every unit through
  unless the type is a land-only kind (0x5B / 0x5D / 0x5F) and not a generic hero. Interceptor4 at The_Maw passes.
  The port's `surface_accessible` veto is wrong, in setup_c and in the ETA.
- **Q3.** Levels 7 / 8 / 9 read GC +0x157c / +0x1580 / +0x1584 = `Low_` / `Medium_` / `High_Threat_Reachability_Tolerance`
  = **1000.0 / 3000.0 / 5000.0** in FoC. A level outside 0..10 has threshold **0.0**, not 1e18. No shipped galactic
  caller uses 7-9.
- **Q4.** `Produce_Force(nil)` is a **script error** ("Expected a planet object for parameter 1"): no status, no
  build tasks, the script stops. Argument 1 cannot be skipped. With no argument and in galactic mode,
  `Produce_Force` writes 0 to the PotentialPlan's byte +0x4d = the `Test_Target_Contrast` flag (default 1):
  "stop re-testing target contrast while this plan runs".
- **Q5.** 0x5f9f90 is **not a budget call**: it counts the produced type in a per-task-force map (TF +0xa8) that
  nothing reads. The build task stores **no charge amount**, only a "charged" byte BT+0xd0. The charge moves the
  cost from the goal's required to its allocated amount at task-list time; the committed spend `s` grows by the
  full cost when the market **starts** production.
- **Q6.** At a planet whose ground owner is not the player or an ally, `Land_Units` **refuses to land** any
  transport. It only fires `Ground_Activated` special abilities (one per transport's cargo, plus non-transport units
  with such an ability). Landing (fleet split + land order) happens only at a friendly planet.
- **Health (t05).** The Setup A health is min(hull fraction, destroyable-hardpoint fraction) when the type has
  `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` and at least one destroyable hardpoint, else the hull fraction.
  All destroyable hardpoints at 0 gives 0 and the unit is dropped. A port without galactic hardpoint health should
  use the hull fraction alone.

---

## Q1. Setup A exclusions for galactic units, and the in-transit fleet planet (0x7287a0)

### Q1.1 Full order per free-store entry (galactic)

Setup A walks the free-store entries. For each entry, in this order [H]:
1. **Reservation.** The entry is considered only if PP+0x104 == 0 or entry +0x30 (the reserving plan) == 0.
2. **Health.** 0x396470(unit) (see "Health and the hardpoint minimum") < GC 0x140b27338
   `Health_Low_Percent_Threshold` (0.33) → skip the entry.
3. **Key slot reset.** key[idx] = 0.
4. **Stage key.**
   - No goal target: go to step 5 with key 0.
   - Target present but the unit is hidden (0x3a9760): go to step 5 with key 0.
   - Target present, galactic (PP+0x198 == 0):
     1. Planet: walk obj → +0x2b0 parent → ... until an object with +0x335 != −1 (the planet). If none is found and
        the unit's **direct** parent has FLEET (+0x336 != −1), the planet is 0x432c30(parent) = that fleet's pack
        (+0xc0) field **+0x50** (see Q1.3). Otherwise drop the unit.
     2. The planet must be revealed (0x3fa420, local-player view), else drop the unit.
     3. The unit's reach object is the reachability list entry indexed by 0x3f8500(planet's PLANET behaviour).
     4. Level = 3, or 4 when the goal's Reachability (+0x2c) == 4.
     5. Per task-force stage i: c = stage-reach slot 7 (vtable +0x38) of (unit reach, level, TF def i); the 4th
        argument is ignored by 0x663810. If c != 1e18: sum += c² and mark "finite".
     6. For every stage (finite or not), add the 2-D squared distance from **the unit object's own** +0x78 / +0x7c
        to the stage's position (vtable +0x68).
     7. No finite c for any stage → drop the unit.
     8. key = sum / nStages; store key[idx]; raise the running maximum (0x140b3a520) to key.
   - The maximum is raised **before** the exclusion tests of step 5, so a unit excluded there still raises it.
5. **Exclusions** (any failure skips the entry):
   - Clause A passes if any of: entry +0x30 == this PP; PlanDef +0xd1 `AllowEngagedUnits`; the unit has no TEAM;
     obj +0xa8 null; pack +0x1c4 == −1.
   - Clause B passes if either: no SPECIAL_WEAPON (+0x361) and no LOBBING_SUPERWEAPON (+0x369); or (+0x158, the
     0x4914e0 count ≥ 1, and the mode test).
   - Garrison: obj +0x210 null, or (+0x210 → +8 == 0 and +0x210 → +0x34 == 0).
   - Limbo: obj +0x3a7 == 0.
   - Hero clash: obj +0x3b4 == 0.
   - Lock: 0x39ad40(unit) false.
   - Type: 0x370320(type of 0x3973b0(unit), 0x20 INDIGENOUS_UNIT, PP+0x198) false.
   - Build pad: if +0x35b != −1, the TACTICAL_BUILD_OBJECTS behaviour's 0x42e3b0 or 0x42e3d0 non-null → skip.
6. Append the unit.

### Q1.2 What each excluded field is, and why it never fires in galactic

- **obj +0x210 = GarrisonDataPack** [H for the pack, M for "never galactic"]. Vtable 0x899b18; ctor 0x563b30 sets
  +8 state = 0, +0x34 occupants = 0, +0xc = 0x3fffff. It is allocated lazily, only by GarrisonUnit / Garrisonable
  behaviour code (0x38c6e0 adds n occupants; also 0x38f7b0, 0x390830; callers 0x443560, 0x443690, 0x444310,
  0x444440, 0x444aa0).
  - +8 = garrison state (1 / 2 = garrisoned or entering; 3 and 7 also seen).
  - +0x34 = occupant count (`GarrisonSlotsAvailable` 0x731390 = capacity − +0x34).
  - In FoC XML the GARRISON_* behaviours appear only in `LandBehavior` lists, never in `Behavior` or
    `GalacticBehavior`, so a galactic object never gets the pack.
- **obj +0x3a7 = "in limbo"** [H for the writers, M for "never galactic"]. Set to 1 by:
  - 0x37ade0 (containment, through 0x379d30 / 0x379df0), called from garrison 0x4446b0, transport landing 0x644330,
    tactical battle setup 0x2a1eb0 / 0x2a00b0 / 0x2afdb0, Lua spawn 0x2bfdd0, vehicle thief 0x63fae0;
  - TransportLanding 0x643a30 / 0x643b00;
  - Lua `Set_In_Limbo` 0x580460 (shipped use: only `STORY_UNDERWORLD_ACTIII_M11_LAND.LUA`).
  Cleared by 0x37a060, 0x37a8d0, 0x642c70 and at init 0x388720 / 0x388b60; copied by 0x472560. No galactic writer.
- **obj +0x3b4 (byte) = hero clash** [H]. Set by MeleeHeroClash / ActionDrivenHeroClash (0x6ab200, 0x6ac780),
  cleared by 0x6ab4d0, 0x6ac940, 0x6acb80, 0x6acc30 and at init. Tactical only.
- **Lock 0x39ad40** [H for the rule, M for "false in galactic"]. True when any of:
  - lock count obj +0x3a8 > 0 (adders: tactical hyperspace arrival, abilities, Lua `Lock_Current_Orders`);
  - the locomotor's vtable +0x1d0 (path-record byte +0x5c) is set; the Fleet locomotor answers 0;
  - the unit is a land team container.
  Shipped `Lock_Current_Orders` users: the library scripts PGCOMMANDS / PGEVENTS / PGTASKFORCE (tactical garrison,
  healer, kite, flee helpers) and the REBEL M07 space, M10 land and M00 land story scripts. No galactic plan uses it.
- **+0x35b TACTICAL_BUILD_OBJECTS** [H for the test, M for "absent in galactic"]: the land build pad behaviour;
  0x42e3b0 / 0x42e3d0 return the object being built or attached. Galactic unit types do not carry it.

**Result:** in galactic, the only exclusions that can fire are reservation, health, unrevealed planet, no finite
stage cost, and the INDIGENOUS type test. Clauses A and B pass as in `A2L_T03_RE_ANSWERS.md` Q4. Earlier notes that
read these tests as "not moving / not building" were wrong: a unit in a moving fleet is kept.

### Q1.3 The in-transit fleet: pack +0x50 is the final destination

- FleetDataPack +0x50 is written by 0x435690. The fleet move function 0x431c80 first departs the source (0x4358c0:
  clears pack +0x40 the orbit planet, sets the fleet's parent +0x2b0 = null via 0x3a8f80, puts the fleet at the
  source planet's position), then sets pack +0x50 = **the final destination planet of the move** [H].
- The fleet locomotor service 0x56a1e0 does nothing while +0x50 is null. At each hop it compares the route's next
  planet with +0x50; on arrival at +0x50 it calls 0x435690(..., null), clearing it [H].
- Blocked hop (0x569980): if the blocked hop is the destination, the new destination is route[idx − 1] (the fleet
  retreats to the previous planet) and +0x50 is rewritten to it [H].
- Placing a fleet at a planet (0x400f30 / 0x401660 / 0x5af260) clears +0x50. 0x38e360 treats +0x50 != 0 as "fleet
  is moving" [H].
- So in transit, Setup A's planet (and so its reach row and the revealed test) is the **destination**.
- The distance term uses the **unit's own** +0x78 / +0x7c. The fleet move updates the fleet object's position only:
  the position setter 0x3a8f90 does not propagate to contained objects (0x3ac530) [M]. So a unit in a moving fleet
  keeps a stale position, most likely the one set when it last joined or was placed [L].
- Only the **direct** parent is tested for FLEET. A unit inside a transport inside a fleet (parent = transport,
  which has no PLANET ancestor while the fleet is in hyperspace) finds no planet and is dropped [H for the rule].

**Q1 port change**
- Remove the "moving / building / locked / garrisoned / limbo / hero-clash / build-pad" exclusions from the galactic
  Setup A (`FreeStoreM::setup_a`, `galactic_pp.cpp` ~l.307-385) or make them constant-false in galactic; keep
  reservation, health, revealed, finite cost and INDIGENOUS.
- In transit, use the fleet's **destination** (`FleetState` destination, not `from`) as the unit's planet, for the
  reach row and the revealed test.
- For the distance term, use the unit's own stored position; if the port's galactic store moves contained units with
  the fleet, document the difference and keep it until the open probe settles it (Open item 1).
- Raise the max key before the exclusions, as listed in Q1.1 step 4.
- A unit whose direct parent is neither a planet chain nor a fleet (cargo of a transport in a moving fleet) is
  dropped.

---

## Q2. Production setup_c: revealed, not surface (0x725c50 → 0x664210)

### Q2.1 The call

Setup C 0x725c50 calls 0x664210 for every candidate type that is not a structure (and not kind 0x5B / 0x5C; with a
target, also not 0x5F / 0x60) with [H]:
- reach = the goal target's reach object (target vtable +0x38), or the player's default reach object with no target;
- the type T;
- level = goal Reachability (+0x2c), or 5 with no target;
- flag = byte 0x140b3a524, read from the disassembly at 0x725db2: 1 during the first solve (0x6c0d00), 0 in the
  re-solves (0x6c47c0 / 0x6c4a50).

### Q2.2 0x664210

1. If level < 6, run 0x665160(reach, level, 0) (refresh the reach lists).
2. For each planet in list[level + 1] (entry vtable +0x30 gives the planet), the planet counts only if **all** hold:
   - 0x3fa420(planet's PLANET behaviour, planet) = **revealed** in the local player's view (auto-reveal or
     D+0x2c9 set; D+0x2e0 set → false);
   - planet +0xd0 (its production object) is non-null;
   - 0x2804d0(player object, T, planet, 0, 1, 0, 0) = the full can-build gate.
3. Value of a passing planet = build time 0x371230(T, planet) + queue time:
   - flag 0: q = 0x559b40(planet +0xd0, sel), then 0x53bb20(q, 1, 0);
   - flag 1: 0x401ba0(planet's PRODUCTION behaviour, planet, sel);
   - sel = 1 if (T +0x86c < 1, or (T +0xf0c == 0 and T is not kind 0x5F)), else 0.
4. Result = mean of the passing values; no passing planet → 1e18. Setup C then subtracts the base build time
   btime(T, none).

### Q2.3 Where the surface test is

There is no direct 0x375220 call in 0x664210 [H]. The surface clause is step 24 of the can-build gate 0x2804d0
(`STRATEGIC_GAPS.md` build gate): pass when
- (0x375220(planet type, planet) = surface accessible, **or** T has none of the kinds 0x5B / 0x5D / 0x5F, **or**
  T +0x14 `Is_Generic_Hero`), **and**
- (the planet has no planet data, or T +0xc3 == 0, or 0x4b83d0).

Interceptor4 is a space unit (none of 0x5B / 0x5D / 0x5F), so it passes at The_Maw whatever the surface [H]. The
ETA 0x6639f0 uses the same per-planet tests (0x3fa420 and 0x2804d0) with no separate surface test; its candidate loop
also has no +0xd0 check [H].

**Q2 port change**
- In `ProductionM::setup_c` (`galactic_pp.cpp` ~l.616-658): remove the `surface_accessible` veto; require revealed,
  a production object at the planet, and the full can-build gate (which already carries the step-24 surface rule).
- In `recompute_eta` (~l.175-254): same; remove the surface test and add the revealed test; no +0xd0 test there.
- Pass the first-solve flag (1 in the first solve, 0 in the re-solves) to pick the queue-time source.

---

## Q3. Reach thresholds for levels 7-9 (slot 7, 0x663810)

| Level | Threshold | Notes |
|---|---|---|
| 0, 2, 3, 6 | 1e-4 (0x38d1b717) | sum over [0, 1000) |
| 4 | 1e-4 | sum over [0, nPlanets − 2) |
| 1, 5, 10 | 1e18 | always pass |
| 7 | GC +0x157c (0x140b16d7c) `Low_Threat_Reachability_Tolerance` = **1000.0** | |
| 8 | GC +0x1580 (0x140b16d80) `Medium_Threat_Reachability_Tolerance` = **3000.0** | |
| 9 | GC +0x1584 (0x140b16d84) `High_Threat_Reachability_Tolerance` = **5000.0** | |
| any other (negative or > 10) | **0.0** | |

- Values: FoC `GAMECONSTANTS.XML` l.769-771; the GameConstants ctor default is the same (1000 / 3000 / 5000) [H].
  (The ctor's 8-byte store at +0x1584 also zeroes +0x1588 `Overrun_Auto_Resolve_Multiple`.)
- Accept rule at 7-9: let S = the uint32 hop sum; accepted when float((S >> 16) × 100) ≤ threshold [H].
- An empty path sums to −1 (0xFFFFFFFF): (0xFFFF × 100) = 6 553 500 > 5000, so it is rejected at 7-9 [H].
- Galactic callers: Setup A uses levels 3 / 4 only. Shipped `Reachable_Target` level arguments are Any, Any_Threat,
  Enemy_Undefended, No_Threat, Friendly_Only and `threat_threshold` variables in the tactical Tyber scripts. Goals
  with Low / Medium / High_Threat reachability exist (19 / 2 / 1) but are tactical; in galactic, slot 6 with a
  level > 5 blocks them. No shipped galactic caller reaches 7-9 [M].

**Q3 port change**
- Load the three tolerances from GameConstants (defaults 1000 / 3000 / 5000) into `ai_reach_path_cost`
  (`ai_free_store_path.hpp`), apply the (S >> 16) × 100 ≤ threshold rule, and give unknown levels 0.0 instead of
  1e18.

---

## Q4. `Produce_Force` with an explicit nil, and the +0x4d clear (0x5fe240)

### Q4.1 Arguments

n = argument count; a Lua nil counts as a present argument (as elsewhere in the binding layer) [H].
- n ≥ 1: argument 1 must be either a GameObjectWrapper (class token 0x140a43b18 → object via 0x5791e0) or an
  AITargetLocationWrapper (0x140a44280 → wrapper +0x38, vtable +0x30), and the resolved object must have the PLANET
  behaviour (vtable +0x10 with 3 non-null). Anything else, **including nil** or a non-planet object, raises the
  script error 0x248190 "TaskForceClass::Produce_Force -- Expected a planet object for parameter 1": no status, no
  build tasks, nothing returned; the script stops at the end of the service pass [H].
- n ≥ 2: argument 2 must be a LuaBool (0x140a157b0), giving sync = its byte +0x10. Otherwise the script error
  "TaskForceClass::Prioduce_Force -- invalid type for parameter 2.  Expected bool" (typo in the exe) [H].
- So argument 1 cannot be skipped: `Produce_Force(true)` is an error [H].
- n == 0: destination null, sync 0.

### Q4.2 Body

1. Create a status object 0x6c6500 (0x58 bytes), +0x20 = 0x80b; 0x7d9080(status, TF).
2. Manager = 0x5f6fb0(plan) +0x28 → +0x50.
3. **If the destination is null and the mode (0x5f6fb0(plan) +0x28 → +0x34) == 0 (galactic):** write 0 to byte
   +0x4d of the PotentialPlan reached by TF +0xf0 (plan) → +0x88 (goal) → +0x68 (PP). No null checks [H].
4. For each wanted entry i (TF +0x60 list, count TF +0x90[i]): 0x524ee0(manager, type 0x6021a0(entry), destination,
   TF, count, sync), and add the result to the status.
5. Push the status as the Lua return.

### Q4.3 What PP +0x4d is

- PP +0x4d is the **`Test_Target_Contrast`** flag: Lua `Test_Target_Contrast` (0x6008f0) writes it; it takes
  exactly one boolean (missing plan / goal / PP → warnings). The PP ctor 0x6be9c0 writes 0x0101 to +0x4c, so +0x4c
  (valid) = 1 and **+0x4d = 1 by default** [H].
- Reader: 0x6c47c0 (PP re-validation, run from the goal-system passes 0x6c8970 and 0x722a30) [H]. When PP +0x100
  (plan running) is set:
  1. +0x4c = 0x6c45e0 (validity);
  2. if PlanDef (PP +0x50) +0xaa == 0 and +0x4d != 0: recompute the enemy contrast at the target (0x588980, galactic
     list 0x5f1990 into 0x140b3a600), commit (0x6c4b20, PP +0x1b0 / +0x1c8), then the contrast marshaller (singleton
     0x140b3ae98, slot 6 = 0x72af80) finalises; this can set +0x4c = 0 when remaining / target of an active layer
     exceeds the tolerance 0x140b3a544 = 1 − (n·k + a) / (n·k + b) (PlanDef +0xd4 / +0xa0 / +0xa4);
  3. a false result makes the caller drop the plan when PlanDef (+0x60) +0xa8 == 0.
- So the `Produce_Force()` clear means: "this plan builds without a destination; stop re-testing target contrast
  while it runs" [H for the write, M for the intent].

**Q4 port change**
- `m_tf_produce_force` (`bind_ai.cpp` l.169): argument 1 present and not a planet (nil, bool, non-planet object)
  → script error and stop (`Call::script_error_stop`), no status, no tasks; argument 2 present and not a bool →
  script error. Do not accept bool / userdata in any position.
- `tf_produce_force` (`galactic_ai.cpp` l.456): with no destination in galactic, set the PP's test_contrast = false.
  Initialise test_contrast = true at PP creation (not `pp.contrast_ok`).
- The flag has no effect until the running-plan contrast re-test (0x6c47c0 step 2) is ported; keep it stored.

---

## Q5. `paid()` (0x5f9f90) and the stored build-task charge

### Q5.1 0x5f9f90 is a counter, not a budget call

- 0x5f9f90(TF, type) increments a per-task-force hash map at TF +0xa8 {type → int count} (insert 0x2220f0) [H].
- Callers [H]:
  - 0x6498f0 (galactic hand-over of a built unit to the task force);
  - 0x64aec0 WAIT_FOR_FREESTORE_ALLOC, when BT +0x98 is null;
  - PRODUCTION_END (build-task state 6), when BT +0xd8 == 0.
- No reader was found: none among the TF methods; a disassembly scan for loads of +0xa8 followed by a call found
  only the dtors 0x5f9440 / 0x5f9710 [M: write-only counter].

### Q5.2 Where the money moves

- **Charge at task-list time.** BUILD_TASK_LIST 0x648980: when BT +0xc8 != 0, BT +0xd0 == 0, BT +0xa8 == 0 and
  cost 0x3711c0(T, 0) > 0 [H]:
  - BT +0xd0 = 1 (the only stored marker; **no amount is stored** on the build task);
  - 0x691950(goal = plan +0x88, (float)cost) → 0x6110e0: goal budget required (+0x28) −= cost, allocated (+0x2c)
    += cost.
- **Committed spend s (+0x40)** is written only by [H]:
  - 0x64abf0 = bind at market start of production (called only by the market 0x53cd60): s += (float)cost(T, none),
    unconditionally;
  - 0x64adb0 = pay-as-you-go instalment: s += n − have when short;
  - 0x614310 (through 0x693170), 0x6bd600 (ProduceObjectClass) and 0x615700 also write it.

**Q5 port change**
- Remove `paid()` at PRODUCTION_END (`galactic_ai.cpp` l.1262, `galactic_build.cpp` l.577) and drop `t.charge`.
- At task creation (`create_build_tasks`, `galactic_build.cpp` l.55-110, the [open] comment at l.75): keep a
  `charged` flag; when charging, move cost(T, 0) from the goal's required to its allocated amount.
- Add s += cost(T, none) when production **starts** (the market bind), not at the end.
- Optionally keep a per-TF {type → count} counter for parity; nothing reads it.

---

## Q6. `Land_Units` at an enemy planet with an accessible surface (0x6cec80)

Order [H unless tagged]:
1. Requires the TF's elected fleet (TF +0x120) to resolve to an object not being removed (+0x3a0 bit 2) and the TF
   unit list (+0x78..+0x80) to be non-empty. Otherwise warning 0x248620 "GalacticTaskForceClass::Lua_Land_Units --
   No units in TaskForce %s, or no FleetObject" and return nothing.
2. Planet X = 0x432c40 = fleet pack +0x40 (orbit planet). No null check: retail assumes the fleet is in orbit.
3. Create a status 0x6cd400 (+0x20 = 0x80c); 0x72c090(status, TF, fleet, 1).
4. If 0x375220(X's type, X) (surface accessible) is false: return the status; nothing else happens.
5. If 0x3f9530(X) = number of entries in X's D+0xE0 list not being removed (any owner; the surface unit list [M])
   **==** `Max_Ground_Forces_On_Planet` (DAT_140b168c8, 10): return the status. The test is `==`, so an over-full
   surface still proceeds.
6. Walk the fleet's contents from the last index down to 0 (count read once), stopping when that surface count ==
   the maximum. A unit U is handled when (0x398670(U, X) **or** U has TRANSPORT +0x337) **and** U's parent != X:
   - **U not a transport:** 0x3a00b0(U, X) = fire U's `Ground_Activated` special ability at X
     (`P4_SIM_SYSTEMS.md` §6.1). No landing.
   - **U a transport:**
     - if 0x39a2c0(X, plan player 0x5f7020, 1) is true, i.e. X's **ground owner D+0x74** is the player or an ally
       (relation 0; no owner → false): 0x72bfe0(status, U, 1); FleetManagement op 2 (planet id X +0x50, fleet id,
       transport id), executed (0x5af260); 0x647b90; op 5 (land), executed. This is the landing;
     - otherwise: walk U's cargo (0x397bf0 list) and fire 0x3a00b0 for the **first** cargo unit with 0x398670(cargo,
       X) true, then stop the cargo walk. No landing;
     - after either path, stop the fleet walk if TF +0x120 has become null.
7. Return the status.

0x398670(U, X) = 0x37bf50(U's ability pack, 6, X) > 0: the type's abilities (+0x1a98) of style +0x3c == 6
(`Ground_Activated`) that are ready (0x37be10 == 1), accept X (vtable +0x28) and pass 0x39b0b0.

**Answer:** at an enemy (or neutral, or unowned) planet with an accessible surface, `Land_Units` **refuses to land**:
no fleet split, no land order, no invasion, no battle. Only `Ground_Activated` abilities fire (slicers, saboteurs,
hacks). The "ground units land directly" reading in `AI_GALACTIC_2.md` §5.4 is wrong. Invasions go through other
paths (e.g. `Invade`), not `Land_Units`.

**Q6 port change**
- Port the order above: fleet + units check and warning; orbit planet; surface accessible; `==` max ground forces;
  reverse walk with the max-count stop; transports land only when the ground owner is the player or an ally; else
  fire the first cargo unit's `Ground_Activated` ability; non-transports fire their own.
- In transit (no orbit planet) retail would dereference null; the port should return the status without action and
  log it (Open item 5).

---

## Health and the hardpoint minimum (for t05)

- **Hull fraction 0x396df0** [H]: without TEAM, max = 0x3727a0(type, obj); health obj +0x5c is clamped to max (and
  written back); result health / max. With TEAM: 0x405230(team) (the team aggregate).
- **0x396470** (the value Setup A compares with 0.33) [H]:
  1. h = hull fraction;
  2. if the hardpoint list obj +0x2d0 (array +8, count +0x10) holds at least one hardpoint whose type
     (hp +0x20) +0x4d == 1 (`Is_Destroyable`) **and** the unit type's +0xa8
     `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` is set: h = min(h, 0x396070(obj));
  3. clamp: h ≥ 1 → 1.0; h ≤ 0 → 0.0.
- **0x396070** (hardpoint fraction) [H]: n = number of destroyable hardpoints. If n > 0 and n != 0x38e030 (the number
  of destroyable hardpoints with health 0x385a70 ≤ 0): Σ hardpoint health (hp +0x28) / Σ hardpoint max 0x385ae0.
  Otherwise 0.0, so when **all destroyable hardpoints are destroyed** the health is 0 and Setup A drops the unit.
  Hardpoint max = type health 0x540070 × the mode multiplier (land GC 0x140b16dcc, space GC 0x140b16dc8) × the
  ModifierSet factor.
- **Do galactic objects have hardpoints?** Hardpoints are created in object init 0x3989a0 only when the object has a
  model (obj +0x2a0) and the type lists hardpoints (type +0xe38 / +0xe40) (`ABILITIES_HARDPOINTS.md` §1.1).
  Whether a galactic unit object (inside a fleet) has a model was not settled [M]. Galactic damage is carried by the
  hull value; hardpoint damage does not persist between battles in shipped play [L].

**Port change (t05)**
- With no hardpoint health in the galactic store, use the hull fraction alone (min = hull), clamped to [0, 1]. This
  matches retail whenever the unit has no hardpoint list or no destroyable hardpoint, and whenever every hardpoint
  is at full health relative to the hull. Mark it [M] (Open item 2).

---

## Open items

| # | Item | What settles it |
|---|---|---|
| 1 | Q1.3: the position of a unit inside a moving fleet (stale, or moved with the fleet) [M/L] | probe: a free-store unit in a moving fleet when a plan's Setup A runs; log the unit's +0x78 / +0x7c, the fleet's position and the stored stage key each tick of the move |
| 2 | Health: do galactic unit objects have a model and a hardpoint list (+0x2d0) [M] | probe: read obj +0x2a0 and +0x2d0 of a galactic capital ship (e.g. a Star Destroyer in a fleet) after a space battle that destroyed hardpoints; log 0x396470 vs 0x396df0 |
| 3 | Q5.1: TF +0xa8 type counter has no reader [M] | a write/read breakpoint on a TF's +0xa8 map during a galactic build plan |
| 4 | Q1.2: no galactic writer of +0x3a7 limbo, +0x3a8 lock count, +0x210 garrison pack [M] | write breakpoints on those fields during a galactic game with story scripts running (Underworld act III, Rebel M07) |
| 5 | Q6 step 2: `Land_Units` with a fleet not in orbit (null +0x40) [H: no null check] | none needed for parity; retail would crash, the port returns quietly |
| 6 | Q3: no shipped galactic caller of levels 7-9 [M] | a breakpoint on 0x663810 with level ≥ 7 during a galactic game |

## Confidence summary

| Point | Tag |
|---|---|
| Setup A order, key before exclusions, max raised before exclusions | H |
| +0x210 = GarrisonDataPack (+8 state, +0x34 occupants), lazily allocated by garrison code | H |
| garrison / limbo / hero-clash / lock / build-pad never set in galactic | M |
| fleet pack +0x50 = final destination of the move; cleared on arrival; retreat rewrites it | H |
| unit position not updated during the fleet move | M (stale value L) |
| only the direct parent is tested for FLEET | H |
| 0x664210: revealed + production object + can-build gate; no direct surface test | H |
| Interceptor4 passes the step-24 surface clause | H |
| levels 7-9 = GC +0x157c / +0x1580 / +0x1584 = 1000 / 3000 / 5000; other levels 0.0 | H |
| no shipped galactic caller of 7-9 | M |
| `Produce_Force(nil)` and non-planet → script error, nothing created | H |
| PP +0x4d = Test_Target_Contrast, default 1, cleared by `Produce_Force()` in galactic | H |
| 0x5f9f90 = per-TF type counter, not budget | H (no reader: M) |
| charge = req −= / alloc += at task list, BT +0xd0 marker, no amount; s += at production start | H |
| `Land_Units` lands only at a player / ally ground owner; else Ground_Activated only | H |
| surface-count test is `==` | H |
| Setup A health = min(hull, destroyable-hardpoint fraction) under the conditions above | H |
| galactic units have no hardpoint list | M |
