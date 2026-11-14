# bkg1: galactic AI RE answers (2026-11-12)

Roadmap group `bkg1` (node AIVIEW, session bk-track-b): the eight RE backlog entries b0098 b0102 b0109 b0117 b0122
b0138 b0001 b0002. Source: the Ghidra decompilation of the FoC x64 `StarWarsG.exe` (RVAs relative to 0x140000000),
`dumpbin` disassembly of the retail exe where Ghidra left a range out (the TimeSince leaves, the Lua clock opcodes, the
goal-service quota), and the shipped XML / Lua. Behaviour is described in prose; no decompiled code is reproduced.
Tags: [H] read directly; [M] one inferred link; [L] hypothesis. Each question was read by its own subagent; the
sections below are their write-ups, merged without changes of substance.

Spec pages corrected in the same change: `SKIRMISH_SETUP.md` §3 (0x289050: the build manager never charges the AI) and
§6 (what the land cap's "used" counts), `L1_S4_REMAINING.md` §4.2 (empty-pad revert).

## Index: answers and port consequences

| entry | question | answer (short) | port consequence |
|---|---|---|---|
| b0098 | who writes TF +0x120 (FleetObject); does Launch_Units elect it; 0x1b vs 0x10 | [H] seven writers: ctor / dtor, Add_Force (set while null), the set-if-null helper 0x6d05c0 called from the Launch_Units block's signal-5 handler 0x72c130 and the AIBuildTask launch state 0x13 (0x649be0), the signal handler 0x6cf810 (re-elect on 0x10 only, while non-null), and 0x6d0320 / 0x6d0370 (drop when no member is left). Launch_Units elects the first launched member's new fleet, so Move_To then moves it (no 'No units in TaskForce'). 0x1b never re-elects: a FleetObject that lives on without a TF member keeps its place. | bkg2 **b0097**: make the election stored state (elect while null in tf_join, Launch_Units and the stage launch; re-elect only on fleet death / removal; never re-elect a live fleet without members; null when the member list empties). No PHASE4 [port] note needed for the lazy election: it is ported away in bkg2. |
| b0117 | why retail never magic-spawns Millennium_Falcon_Space / _Ground | [H] the magic-spawn Score 0x726010 vetoes every Is_Named_Hero type with a Space_Model_Name (0x3729f0(T, 2) returns type +0x2a8); the Falcon variants inherit both. Also vetoed: unmet Required_Planets / Required_Orbiting_Units (+0xF60 / +0xF80), and no Hyperspace + no Company_Units + not a structure. AI_GALACTIC_2 §1.4's "hero already present" reading is wrong. | backlog **b0142** (S2 T1) |
| b0102 | TimeSince player leaves and the Lua clock: multiply by spf or divide by FPS | [H] the four leaves are (double)(int32)(NOW − stamp) / (double)30, already bit-exact in the port (names: 0x66b950 SpaceDefender, 0x66b900 SpaceAttacker, 0x66b8b0 GroundDefender, 0x66b860 GroundAttacker). The Lua `GetCurrentTime` 0x5428b0 is (double)((float)(int32)modeFrame × spf), a float multiply, which the port's now_seconds (frame / 30) does not match. | backlog **b0152** (S2 T1) |
| b0109 | goal-pass cadence: evalsLastPass, quota, cap, what counts; Applies; slot 9 reach | [H] quota = max(1, min(ceilf(E / 150), (int)(20 / max(1, N − H) + 0.5))), float; N = the whole player list (8), H = active humans; E cleared at pass end, seeded with sources × targets. Only candidates that reach Evaluate count. Applies 0x665a30: revealed and not hidden, then owner self → Friendly, neutral faction → Neutral, ally → nothing, else Enemy; Contested never tested. Slot 9 0x6b84f0: skip a goal already active for (type, target) or with an empty per-target reach flood at its level (cached 10 s). [M] none of the port's Applies differences clearly explains the 3..5 extra Underworld evaluations. | backlog **b0144** (S2 T1, port Applies) and **b0145** (Gabriel probe: per-candidate evaluation trace) |
| b0122 | 0x53bee0 / 0x53bda0 slot flags; slot removal before production start; slot reserved at A produced at B | [H] slots (type, owner PP, flag) are positional; flag 1 from commit start until 0x6c2d60 re-asserts the goal; 0x53bda0 sums live flag-0 slots and cuts the vector after the last live flag-0 slot; step 5 clears the first (type, plan) slot of the producing queue only, so the [H] reading (A's slot stays until the PP is released) is confirmed. Removal: plan destroyed (0x6bec20 → 0x6c2520 → 0x53c730) and the tail cut. | backlog **b0146** (S2 T1) |
| b0138 | does the AI pay pad builds in campaign land battles | [H] yes, in every mode: escrow at plan accept (0x6c2d60 takes PP+0x44), drawn down per Build match (0x6c1e80), refunded at plan end; campaign price Tactical_Build_Cost_Campaign (fallback Multiplayer) × planet factor. Overturns L1_S4 3.6 [port]. | backlog **b0147** (S2 T1, tactical: outside the galactic AI track) |
| b0001 | land cap 0x2ac5c0 count; pool drops stop after 15232; 0x289050; empty-pad revert | [H] cap = min(capacity, curve(N)) = 9, but "used" is per-member Population_Value shares rounded up, without start forces; drops stop when used reaches 9 (15422). 0x289050 is true for every non-human, so the build manager never charges the AI (see b0138's escrow). Empty owned pads decay to Neutral in 150 frames (151 services); the port already has the rule (capture.cpp). | backlog **b0148** (S2 T1, population count); escrow in b0147; pad revert: none |
| b0002 | Setup A exclusions, in-transit +0x50, setup_c revealed vs surface, reach 7-9, Produce_Force(nil), paid(), Land_Units at an enemy planet | [H] re-read confirms A2M_T04_RE_ANSWERS with five corrections (lock 0x39ad40 third clause = the parent team's locomotor lock; 0x42e3b0 / 0x42e3d0 = occupied build pad; setup_c returns 1e18 for level ≥ 6; the goal-XML census for 7-9; the 0x647df0 charge reversal). Fleet +0x50 = final destination; setup_c tests revealed (0x3fa420), never 0x375220, so Interceptor4 is estimated at The_Maw; levels 7/8/9 = Low / Medium / High_Threat_Reachability_Tolerance 1000 / 3000 / 5000; Produce_Force(nil) is a script error; paid() is a TF type counter, not money; Land_Units at an enemy planet neither lands nor fails. | none for tier 1/2 (the port already follows it); the level ≥ 6 early exit is cosmetic: backlog **b0154** (S3 T3) |

| b0125 (bkg2) | why political control is 1.0 on every port planet (retail 1 / 2 / 3) | [H] the target is a breakdown: the largest entry per group summed over the groups, clamped to [1 with a physical owner else 0, the faction's Planet_Max_Political_Control_For_Faction or 5], then [0, 5]; with FoC data an owned planet gets max(1, ground-base level, starbase level, troops). The vanilla income curve makes control >= 1 income-neutral; the AI token TargetPoliticalControl reads it. Side finding: an empty planet reverts to neutral unless Planet_Retains_Residual_Influence; the port's leaning follows captures. | ported in bkg2 (campaign.cpp target_political_control); the owner rule: backlog (bkg2) |
| b0121 (bkg2) | does a hostile fleet fail an AI build in production; does a released goal cancel its builds | [H] no: only a starbase head is cancelled (signal 3 -> Production_Failed, no refund), which the port has; the blockade test applies after production (GROUND_UNIT_PRODUCTION 0x648120). A release ends the tasks without Production_Failed; a producing PAYG item is cancelled with the paid-fraction refund unless reclaimed (already in the port). | ported in bkg2 (claim_units: blockaded GROUND_UNIT_PRODUCTION fails the task) |

Still open (no backlog entry, [L] / probe-only, listed in the sections): the company type field +0x22E0 the member
count skips and the population registrations 0x2a00b0 / 0x2bf2d0 (7); whether the 0x6c2d60 guard takes the escrow once
per plan (7); where a unit inside a moving fleet is positioned, whether galactic code writes +0x3a7 / +0x3a8 / +0x210,
and whether anything reads TF +0xa8 (8, from A2M_T04).

---

## Sections 1-2. b0098 (TF +0x120 writers) and b0117 (magic-spawn hero veto)
Source: Ghidra decompilation of retail StarWarsG.exe (FoC). Tags: [H] read directly, [M] one inferred link,
[L] hypothesis. Behaviour in prose only.

### Short answers

- **b0098 (TF +0x120).** Retail writes the elected FleetObject in seven places (list in §1.1). Besides `Add_Force`
  and the 0x10 re-election there is a third way to elect it: a set-if-null helper 0x6d05c0 that runs when a unit
  enters a fleet. It has two callers: the `LandUnitsBlockStatus` that `Launch_Units` creates (0x72c130, signal 5),
  and the AIBuildTask launch state 0x13 (0x649be0, signal 5) [H].
- **Surface-added TF, then `Launch_Units`:** +0x120 is null after the adds, because `Add_Force` elects only when
  the unit's parent is a fleet. `Launch_Units` runs FleetManagement op 4 (launch) and op 0 (form a new fleet)
  directly, member by member. Op 0 raises signal 5 on the unit, and the block's handler then sets +0x120 to that
  unit's new fleet. So the following `Move_To` **moves that fleet**: no "No units in TaskForce" warning and no nil
  return. The fleet chosen is the fleet of the first member that launched. Exceptions: nothing launched (every
  member blockaded or not a transport on a planet, or FleetManagement ignored while a battle is pending) [H code; M
  that the payload ordering is as read].
- **0x1b never re-elects** [H]. From the FleetObject, 0x1b calls vf +0xb0(1.0): Release_Forces, then +0x120 is
  dropped to null if no member is left. From a member, 0x1b removes that member. Re-election happens only on 0x10,
  and only while +0x120 is non-null: either the sender is the FleetObject, or the FleetObject no longer resolves. A
  FleetObject that lives on without holding any TF member keeps its place, and `Move_To` moves it [H by absence of
  any other writer; M that this case happens in play].
- **b0117 (Falcon).** In the MagicPlan veto of the Production marshaller (0x726010), one rule rejects any **named
  hero (type +0x13) that has a space-mode model**: 0x3729f0(T, mode 2) returns type +0x2a8, which is loaded from
  `Space_Model_Name` +0x200 [H]. AI_GALACTIC_2 §1.4 read this rule as "hero already present", which is wrong.
  `Millennium_Falcon_Space` and `_Ground` are `Variant_Of_Existing_Type` = `Millennium_Falcon`. They therefore
  inherit `Is_Named_Hero` = Yes and `Space_Model_Name` = RV_MFalcon.ALO, and the rule vetoes them [H XML; H
  inheritance per OBJECT_TYPES §3]. +0xF60 / +0xF80 are `Required_Planets` / `Required_Orbiting_Units` counts. Both
  are 0 for the Falcon, so they do not trip.

---

### 1. b0098: who writes GalacticTaskForce +0x120

#### 1.1 Every writer found [H]
I searched every function of GalacticTaskForceClass (functions.csv) and every `+0x120` / `[0x24]` store in
Global.c. I also searched the stores the handler makes through its listener sub-object at TF +0x28, where the field
is +0xf8.

1. **0x6cce20, constructor:** sets +0x120 to null. **0x6cd110, destructor:** releases it.
2. **0x6cd1a0 `Add_Force`:** always subscribes to the unit's 0x1b. Then, only while +0x120 is null and the unit's
   parent (+0x2b0) is a fleet (+0x336 ≠ −1): +0x120 = the wrapper of that fleet (0x5751d0), and the TF subscribes
   to the fleet's 0x10 / 0x1b. A unit on a planet surface (parent = the planet) elects nothing.
3. **0x6d05c0 (set if null):** while +0x120 is null and the argument is non-null, stores the argument and subscribes
   to the fleet's 0x1b / 0x10. It does not check that the argument is a fleet. Callers (callgraph.csv):
   - **0x72c130, `LandUnitsBlockStatus` signal handler.** On signal 5 from a unit the block registered for signal 5
     (0x72bfe0 with flag 0; flag 1 registers signal 8 instead):
     - the unit's id is removed from the block's pending list;
     - the block's owner (block +0x28, 0x6021a0) is type-checked as GalacticTaskForceClass;
     - if its +0x120 is null (getter 0x6cd930), it becomes the wrapper of the unit's parent (+0x2b0), via 0x6d05c0.
   - **0x649be0, AIBuildTask signal handler.** Signal 5 in task state 0x13 (the launch state after WAIT_FOR_ALL,
     "TRANSPORTS_TO_ORBIT" [M name]): if the task's TF (task +0xa0) is galactic and its +0x120 is null, +0x120 =
     the wrapper of the fleet whose id is in the signal payload (FleetMergeResult +8). The state then moves to 0x14.
4. **0x6cf810, signal handler (listener TF +0x28), signal 0x10:**
   - (a) If the member list is empty and +0x120 is set: event 3, unsubscribe 0x1b / 0x10, drop +0x120.
   - (b) If +0x120 is set and either the sender is the FleetObject or the FleetObject no longer resolves: event 3,
     unsubscribe, drop, then walk the members (TF +0x78, add order). The first member whose parent is a fleet not
     being removed (+0x3a0 bit 2 clear) gives the new +0x120, and the TF subscribes to its 0x10 / 0x1b.
   - With +0x120 already null, 0x10 elects nothing.
5. **0x6d0320 (vf +0xb0):** Release_Forces 0x5fefd0, then drops +0x120 if the member list is empty.
6. **0x6d0370 (vf +0x188, remove member):**
   - unsubscribes the member's 0x1b;
   - calls the base remove 0x5ff610;
   - if no member is left: unsubscribes the fleet's 0x1b / 0x10, drops +0x120, and ends the TF's build tasks
     (0x64ad00).
7. Base TaskForceClass (0x5f9xxx–0x600xxx) never stores to +0x120. The field belongs to the galactic subclass (the
   constructor sets it after the base constructor 0x5f8760). 0x6d0530 serializes +0x120 for the save (0x24b650).
   The matching load was not traced [L: it restores the saved pointer].

No other store was found. Every other GalacticTaskForce function only reads +0x120: Form_Units 0x6cd5b0, Move_To
0x6cf370, Land_Units 0x6cec80, 0x6ce600, 0x6cf590, 0x6cd440, 0x6ceaa0, 0x6d0680, 0x6d1100 and 0x6cd180 [H].

#### 1.2 Launch_Units 0x6cf070 does not write +0x120 itself, but its block does [H]
1. Launch_Units creates a `LandUnitsBlockStatus` (0x72bf90, id 0x80c) and inits it with owner = the TF and fleet =
   null (0x72c090).
2. It walks the members (TF +0x78, in order). A member qualifies when it has behaviour 5 (TRANSPORT, the galactic
   ground company) and its parent is a planet. For each one:
   - If the planet is blockaded (0x3fa370), it logs "...is now under blockade, can't launch units..." and returns
     without a block. Members launched before that one stay launched.
   - Otherwise it registers the unit for signal 5 (0x72bfe0, flag 0). Then it runs **directly** (0x5af260) a
     FleetManagement **op 4** (launch) and then an **op 0** with fleet 0x3FFFFF (new fleet) for that unit.
3. In the executor 0x5af260, op 0 (and ops 1 / 6 / 7) adds the unit to the fleet (0x430d90). It then raises
   **signal 5** on the unit (FleetMergeResult, +8 = fleet id) and on the fleet (+8 = unit id).
4. That signal reaches 0x72c130 during the Launch_Units call. The first member to land in a fleet therefore sets the
   TF's +0x120 to its new fleet. Later members do not change it.
5. Exception: while the battle-pending counter 0x140b1540c > 0 (COMMANDS §8), FleetManagement is ignored. Nothing
   launches and nothing is elected [M].
6. Whether every launched unit gets its own new `Galactic_Fleet` or joins one at the orbit slot (0x4338d0 / 0x2af7c0)
   was not settled [L]. Either way `Move_To` moves only the elected fleet (A2L_T01 Q8.1).

#### 1.3 The case to settle [H chain, M payload order]
Every member was added on the surface, so +0x120 is null. `Launch_Units` launches them and elects the fleet of the
first launched member. A later `Move_To` (0x6cf370) finds the FleetObject, which resolves and is not being removed,
and the member list is not empty, so it **moves that fleet**. The 'No units in TaskForce %s, or no FleetObject'
failure happens only if nothing was launched (all blockaded, no transport member on a planet, or a battle pending).
It also happens when units reach a fleet through a path that neither `Add_Force` nor the two signal-5 handlers
cover. Retail never elects lazily when +0x120 is read.

#### 1.4 The 0x1b / 0x10 contradiction [H]
0x6cf810 handles 0x1b in two branches only:
- **From the FleetObject:** vf +0xb0(1.0), i.e. 0x6d0320. Forces are released, and +0x120 is dropped only when no
  member is left.
- **From anything else:** vf +0x188(sender), which removes the member. Then event 7 if TF +0x60 is empty.

Neither branch elects. Per A2M_T08 t10.3, 0x1b is raised only by the retreat functions 0x2ba6c0 / 0x2ba850.
Re-election is 0x10-only (§1.1 item 4b).

A FleetObject that still lives but holds no TF member keeps its place in +0x120. A member that left by FleetManagement
op 2 (detach, signal 6) or op 5 (land, signal 8) does not change it, because the TF listens to neither signal. When the
last unit leaves the fleet, the fleet is removed and the 0x10 re-election runs [M]. The port's [M] comment (a member's
0x1b re-elects when the FleetObject holds no member) has no support in the binary.

**Port consequence:** `GalacticAiWorld::Impl::tf_fleet_object` (engine/ai/src/galactic_ai.cpp:1321; the brief's
engine/galactic path is wrong) must stop electing on read. Keep `tf.fleet` as stored state with these rules:
- **Elect while null** in:
  - `tf_join` (Add_Force, already right);
  - `LaunchUnits` (galactic_ai.cpp:1820): the new fleet of the first member actually launched;
  - the stage launch at galactic_ai.cpp:1596 (build-task state 0x13 analogue): the fleet that launched member
    formed into.
- **Re-elect** only when the stored fleet is destroyed, merged away or removed (0x10), and only if it was non-null.
- **Do not re-elect** when the fleet is alive but holds no member.
- **Drop to null** when the member list empties (0x6d0320 / 0x6d0370 / 0x10a).

`Move_To` / `Land_Units` then read the stored value as it is: null means fail with nil. Goldens may change.

---

### 2. b0117: why retail never magic-spawns the Falcon

#### 2.1 The filter chain, Production marshaller Score 0x726010 [H]
1. **Free unit:** the record's unit index (rec +4) ≠ −1 → 0.5. No veto.
2. **Galactic structure checks** (PP +0x198 == 0):
   - ground structure (behaviour 0x5F): needs a planet with a surface, the per-planet cap (+0x87C), and ground slots;
   - orbital structure (0x60): needs orbital slots.
3. **Not a MagicPlan** (PlanDef +0xA8 == 0): pop-cap test, then the production score is returned.
4. **MagicPlan in galactic** (PP +0x198 == 0): **veto (1e18)** if any of these holds. Otherwise one synced float
   draw (0x5324d0).
   - a. `Is_2D`/company-member flag +0x2A, or squadron-member flag +0x29;
   - b. not a named hero (+0x13 == 0) and cost 0x3711c0(T, none) < 1 (`Build_Cost_Credits` +0x86C);
   - c. behaviour 5 (TRANSPORT) in the current mode (0x370320(T, 5, −1));
   - d. `Tech_Level` (+0x89C) > 5;
   - e. "Super" category (0x375270, `CategoryMask` +0x1648);
   - f. **named hero (+0x13) and 0x3729f0(T, 2, 0) ≠ null**;
   - g. the player's locked list 0x282550, or the production-disabled list 0x282510;
   - h. `Build_Initially_Locked` (+0xC1) without an unlock (0x282580);
   - i. behaviour 0x5C or 0x5B (DUMMY_STAR_BASE / DUMMY_GROUND_BASE);
   - j. `Required_Planets` count +0xF60 > 0, or `Required_Orbiting_Units` count +0xF80 > 0 (OBJECT_TYPES /
     STRATEGIC_GAPS §19, §15). A non-empty list means "only buildable at certain planets / with certain orbiters",
     so the type is never spawned;
   - k. `Hyperspace` (+0x24) false **and** `Company_Units` count (+0x960) == 0 **and** not a 0x5F / 0x60 structure.

Notes on rule f [H]:
- 0x3729f0(T, mode) is the "type's model for the mode" getter: 0 gives +0x288 / +0x298, 1 gives +0x2a0 (land),
  2 gives +0x2a8 (space).
- First it calls the lazy loader 0x375380, which fills +0x2a8 from `Space_Model_Name` (+0x200) and +0x2a0 from
  `Land_Model_Name` (+0x1e0). The loader returns early only for `Is_Marker` (+0x11) types.
- So rule f reads "**a named hero with a space model is not spawnable**". It is not a presence test.
- It is the only hero-specific veto. A named hero without `Space_Model_Name` (for example a hero company or a
  land-only hero) passes it [M].

#### 2.2 What the Falcon XML trips [H XML]
`UNIQUEUNITS.XML` (FoC and EaW are the same here):
- `Millennium_Falcon` sets `Is_Named_Hero` Yes, `Space_Model_Name` RV_MFalcon.ALO, `Land_Model_Name`
  RV_MFalcon_Land.ALO, `Hyperspace` Yes, `Build_Cost_Credits` 3000 and `Behavior` TRANSPORT. It sets no
  `Tech_Level`, no `Build_Initially_Locked` and no `Required_*`.
- `Millennium_Falcon_Space` sets only `Variant_Of_Existing_Type` Millennium_Falcon, `Behavior` DUMMY_STARSHIP,
  SELECTABLE, POWERED, SHIELDED and `Icon_Name`.
- `Millennium_Falcon_Ground` sets `Variant_Of_Existing_Type` Millennium_Falcon, `Behavior` DUMMY_STARSHIP,
  `Is_Visible_On_Radar` No, `Icon_Name` and `Scale_Factor` 2.3.
- Neither variant appears in any `Company_Units` / `Squadron_Units` list, so +0x2A / +0x29 are clear.

Variants copy the whole parent and then re-parse their own tags (OBJECT_TYPES §3.2). Both variants therefore keep
`Is_Named_Hero` = Yes and `Space_Model_Name` = RV_MFalcon.ALO, and **rule f vetoes them** [H]. That the ALO loads
in galactic mode is [M]; it is a shipped model.
- The parent `Millennium_Falcon` is also vetoed by rule c (TRANSPORT) [M: mode −1 in galactic reads `Behavior`].
- Rule j does not apply: +0xF60 = +0xF80 = 0.
- Rule k does not apply: `Hyperspace` = Yes is inherited.

The same rule f keeps every named space hero ship out of magic spawns: Slave_I, Home_One, the Executor-type hero
ships, and others [M].

**Port consequence:** `ProductionM::score` (engine/ai/src/galactic_pp.cpp, about lines 737–756) needs four changes:
- **(1)** Replace the "hero the player already has" loop (`player_units` type match) with: named hero (`tinfo.hero`
  / `Is_Named_Hero`) **and** a non-empty `Space_Model_Name` → veto. This needs a new type field, for example a
  `has_space_model` flag in gal::TypeInfo or GalacticAiTypeInfo, set from `Space_Model_Name` after variant
  inheritance. [port] Treat a non-empty name as a loaded model.
- **(2)** Add veto j: `required_planets` or `required_orbiting_units` non-empty. Both are already in gal::TypeInfo,
  galactic_rules.hpp:103–104.
- **(3)** Add veto k: !`hyperspace` && `company_units`.empty() && not a ground or orbital structure.
- **(4)** Fix AI_GALACTIC_2 §1.4 line 248 in docs/re: "a hero already present 0x3729f0" should read "a named hero
  with a space model (0x3729f0 mode 2)", and the "+0xF60 / +0xF80 counts" should be named.

The draw count changes for vetoed candidates (no float is drawn for them), so goldens may change.

---

## Sections 3-4. b0102 (TimeSince leaves, Lua clock) and b0109 (goal-pass cadence)
Sources: Ghidra decompilation (Global.c) and, where Ghidra left the bodies out (0x66b860..0x66b9a0) or for the exact
float / double instructions, `dumpbin /disasm` of the retail FoC StarWarsG.exe (image base 0x140000000). Vtable slots
were read from the exe's .rdata. Tags: [H] read directly; [M] one inferred link; [L] hypothesis.

### Short answers

- **b0102, the four player leaves: the port is already bit-exact.** They divide by FPS in double and do not multiply
  by spf: `(double)(int32)(NOW - stamp) / (double)(int32)FPS`, where NOW is the global frame 0x140b0a320 and FPS is the
  int 0x140b0a340 (30). A stamp of -1 gives `(double)1e18f`. [H]
- **The leaf names were partly swapped in the question.** 0x66b950 = TimeSinceSpaceDefender (+0xA8),
  0x66b900 = TimeSinceSpaceAttacker (+0xB0), 0x66b8b0 = TimeSinceGroundDefender (+0xAC),
  0x66b860 = TimeSinceGroundAttacker (+0xB4). [H]
- **b0102, the Lua clock: the port is NOT bit-exact.** `GetCurrentTime()` (0x5428b0) returns
  `(double)((float)(int32)modeFrame * spf)`. modeFrame is the current game mode's frame (mode +0x10). spf is the float
  0x140b0a344 (= 1.0f/30.0f = 0.033333335f). The multiply is done in float and the result is widened to double. [H]
  The port's `now_seconds` (`(double)frame / 30.0`) differs on most frames (for example frame 150 gives 5.000000477 in
  retail and exactly 5.0 in the port). [H arithmetic]
- `GetCurrentTime.Frame()` (0x5406b0) = `(double)((float)(uint32)NOW / (float)FPS)`, a float divide.
  `GetCurrentTime.Galactic_Time()` (0x5460a0) = `(double)((float)(int32)galacticModeFrame * spf)`. [H]
- **b0109, quota arithmetic is all float** (this corrects the "double" note in A2M_R2_RE_A §2.1; the result is the
  same for every reachable integer input): `quota = max(1, min((int)ceilf((float)E / ((float)FPS * 5.0f)),
  (int)(20.0f / (float)max(1, N - H) + 0.5f)))`. [H]
  - N = the PlayerList size and H = the active humans, so the cap scales with the player list, not with the number of
    AI players. [H]
  - E (+0x188) is zeroed only at the end of a pass. Init seeds it with S x max(1, T). [H]
- **What counts as an evaluation.** Exactly the candidates that reach `Evaluate` count. Applies failing, slot 9 (already
  active, or reach lookup null) and non-proposable sources are free. An Evaluate that returns false, a desire <= 0 and
  a rejection by slot 10 all count. The port's GoalSystem matches this. [H]
- **Applies 0x665a30 [H]** (closes AI_GALACTIC §1.4 [L]):
  - the planet must be revealed (0x3fa420);
  - then the owner is read through `TargetPlayerID`:
    - self → Friendly bit;
    - a player of a neutral faction → Neutral bit;
    - an ally → **never**;
    - otherwise → Enemy bit;
  - Contested is never tested.
- **The port's `applies()` is not retail on three points:**
  - its Friendly includes allies;
  - it tests Contested;
  - it has no reveal / hidden test.
- **Slot 9 [H].** The goal is skipped when (type, target) is active, or when the target's PlanetReachability slot 6
  at the goal's `Reachability` level is null (a per-(player, target, level) flood, cached 10 s). The port's
  `PlayerAi::blocked` already follows this.
- **Missing retail data.** A per-candidate list of retail Evaluate calls, which retail traces do not contain. §B.8
  names a hardware-breakpoint probe that records it.

---

### A. b0102: TimeSince player leaves and the script clock

#### A.1 The four GalacticPlayer leaves [H]

Registration 0x66b9f0 binds the leaf functions to token ids. The ids are named by the token-init function, which
passes the strings to the perception token registry:

| Token (id address) | Leaf | Stamp field (GalacticPlayer evaluator) |
|---|---|---|
| TimeSinceSpaceDefender (0x140a43eac) | 0x66b950 | +0xA8 |
| TimeSinceSpaceAttacker (0x140a43eb0) | 0x66b900 | +0xB0 |
| TimeSinceGroundDefender (0x140a43eb4) | 0x66b8b0 | +0xAC |
| TimeSinceGroundAttacker (0x140a43eb8) | 0x66b860 | +0xB4 |

All four bodies are the same 0x43-byte shape (disassembly):
1. Load the int32 stamp. If it equals -1, store the f64 constant 0x14085b8c0 (999999984306749440.0 = (double)1e18f) and
   return true.
2. Otherwise:
   - eax = [0x140b0a320] − stamp (32-bit integer subtract);
   - FPS = [0x140b0a340] converted with cvtdq2pd (int32 to double);
   - d = eax converted with cvtdq2pd (signed int32 to double);
   - result = **divsd d / FPS** (double divide), stored to the output;
   - return true.
3. The leaf never fails, and the stamp test is exactly == -1 (not < 0).

So TimeSince*er = `(double)(int32)(NOW − stamp) / (double)FPS`. NOW is the global logic frame (FrameSynchronizer +0,
0x140b0a320), not the galactic mode frame. These leaves are therefore unlike the TimeSinceSpaceConflict /
GroundConflict / Conversion family (0x65f1b0 / 0x65f160 / 0x65f0c0). That family multiplies by the widened float spf
(A2M_T08 t08.4) and tests stamp < 0. Both families exist side by side. [H]

**Arithmetic note** [H]. (double)k / 30.0 is correctly rounded, and the port's
`static_cast<double>(r.logic_frame_ - s) / static_cast<double>(kAiFps)` computes the same expression. The only edge is
the stamp test: retail tests == -1, the port tests s < 0. Stamps are frames or -1, so the two agree.

#### A.2 The Lua script clock [H]

`GetCurrentTime` is a `LuaGetTime` object (vtable 0x898530), registered in the game-command table 0x546c70.
- That table is installed by the AI script loaders among others [H callgraph]:
  - the goal system 0x5f7090;
  - the perception 0x5e7cb0;
  - planning 0x5f7660;
  - story, scoring and object scripts.
- So the galactic plan scripts, evaluator scripts and `Sleep` (which polls `GetCurrentTime()` in Lua) all use it. [M]
- The vtable slots, read from .rdata:
  - +0x50 = 0x5428b0, the plain call `GetCurrentTime()` [M: +0x50 is the LuaUserVar call slot; FIGHTER_FLYING agrees];
  - +0x98 = 0x5406b0, bound as member `Frame`;
  - +0xA0 = 0x5460a0, bound as member `Galactic_Time` (the thunks 0x254a1c / 0x254a28 jump to +0x98 / +0xA0).

| Lua call | Function | Exact value |
|---|---|---|
| `GetCurrentTime()` | 0x5428b0 | mf = current mode (0x140b15418) +0x10, or 0 with no mode; cvtdq2ps (signed int32 to float); mulss by [0x140b0a344]; cvtps2pd. `(double)((float)mf * spf)` |
| `GetCurrentTime.Frame()` | 0x5406b0 | (float)(uint32)NOW (cvtsi2ss from the zero-extended 64-bit value) **divss** (float)FPS, then cvtps2pd. `(double)((float)NOW / 30.0f)` |
| `GetCurrentTime.Galactic_Time()` | 0x5460a0 | galactic mode = 0x28a950(GMM, 0); none → returns with no value [M]; else `(double)((float)galacticMode+0x10 * spf)` |

- spf = 0.033333335f. It is the float 1/FPS written once at start-up (A2M_T08 t08.1). [H]
- In a galactic-only game the current mode is the galactic mode, and its frame counts like the global frame
  (A2M_T08 t08.6). [M]

Retail against the port's `now_seconds`, `(double)frame / 30.0` [H, computed]:

| frame | retail GetCurrentTime | port | retail > port |
|---|---|---|---|
| 1 | 0.03333333507180214 | 0.0333333333333 | yes |
| 3 | 0.10000000894069672 | 0.1 | yes |
| 30, 60 | 1.0, 2.0 | 1.0, 2.0 | equal |
| 150 | 5.000000476837158 | 5.0 | yes |
| 1800 | 60.000003814697266 | 60.0 | yes |

So a Lua test `GetCurrentTime() - t0 > d` can fire one frame earlier in retail than in the port.

The multiply form and the divide form also differ from each other:
- `(float)n * spf` and `(float)n / 30.0f` give different floats on 12 431 of the frames 1..19 999 (3, 6, 7, 12, ...);
- so `Frame()` and `GetCurrentTime()` are not interchangeable.

For reference, the other AI clocks are already ported per A2M_T08 [H]:
- `Game.Age` (0x6e1f20) = `(double)frame * (double)spf`;
- `TimeSinceCorruptionChange` = `(double)(float)((float)(uint32)d * spf)`.

**Port consequence:**
1. `engine/ai/src/galactic_ai_impl.hpp:45` `now_seconds`: return
   `static_cast<double>(static_cast<float>(static_cast<int32_t>(mode_frame)) * (1.0f / 30.0f))`. It feeds
   `vm->set_time` at galactic_ai.cpp:198, 1029, 1201, 2305 and 2403. Use the galactic mode frame (the same as
   `sim.frame()` while no battle runs [M]).
2. The player TimeSince leaves (galactic_ai_tokens.cpp:2041-2046): **none**. They are already retail.
3. If the port ever exposes `GetCurrentTime.Frame()` / `Galactic_Time()`, use the two other forms above.
4. Outside b0102 but the same rule [M]: `engine/apps/sim/src/sim_host.cpp:279/333` gives the scoring VM the float
   *divide* (`(float)frame / 30.0f`). Scoring scripts also get 0x546c70 `GetCurrentTime`, so retail would be the
   float *multiply* of the mode frame. The owner of that file should check it. The tactical
   `free_store_mode_time` is already the multiply form.

---

### B. b0109: galactic goal-pass cadence

#### B.1 Service 0x6cacd0 (disassembly 0x1406cad2b..0x1406cadc8) [H]

Fields: +0x178 quota, +0x17C source cursor, +0x180 target cursor, +0x184 pass-complete flag, +0x188 E (evaluations),
+0x18C countdown (A2M_R2_RE_A §2.1, re-read).

1. 0x64c4d0, then the countdown: if it is > 0, decrement it and return.
2. Run one slice, 0x6cb120.
3. If +0x184 is set:
   - budget weights 0x614590, then Commit 0x6c8710 (call at 0x1406cad26, rdi = goal system);
   - clear +0x184;
   - need = cvttss2si(ceilf((float)E / ((float)FPS * 5.0f))). All float: cvtdq2ps, mulss by 5.0f (0x140803520),
     divss, then the CRT ceilf 0x7760c4;
   - E is zeroed **here** (at 0x1406cad90), after it has been read;
   - n = max(1, N − H), with N = (PlayerList end − begin) / 8 (0x140a16fd0) and H = 0x294b80 (active humans);
   - cap = cvttss2si(20.0f / (float)n + 0.5f), with 20.0f at 0x1408007e8, 0.5f at 0x1408007c0, and divss / addss in
     float;
   - quota = max(1, min(need, cap));
   - countdown = vf+0x58 (galactic: FPS × `Galactic_AI_Goal_Cycle_Sleep_Duration`, 0 on every shipped difficulty).

The cap therefore does scale with the player count, but N is the whole PlayerList: one player per faction, FoC 8
(A2M_R2_RE_A §2.1). The number of AI players is not used. HotM with a human: cap = (int)(20/7 + 0.5) = 3. [H]

#### B.2 Init 0x6ca1f0 [H, re-read]

- E = S × max(1, T):
  - S = the number of (goal type, function) sources, 0x60f6c0(player type, mode), the same list the slice walks;
  - T = the size of the perception target list (vf+0x40), unfiltered;
- quota = the same float formula on that E;
- cursors, flag and countdown = 0; E is not cleared.

So pass 1's evaluations add to the seed. The quota after pass 1 comes from (seed + E1), which in HotM gives the cap.
The quota after pass 2 comes from E2 alone. The port's `retail_cadence` branch already does this.
- [port] It seeds on the first service, not at Init. That is equivalent if no slice runs in between.
- T is the planet target list (one target per planet, AI_GALACTIC §2.2). The port's `target_count()` is
  `sim.planets().size()`, which includes the non-planet scenery rows (`is_planet == false`). This changes only the seed,
  and with the seed far above 150 it does not change a quota in HotM. [M]

#### B.3 The slice 0x6cb120: what is counted [H]

The slice reads the source list (0x60f6c0) and the target list (perception vf+0x40) at the start. It then loops with
a counter c from 0 while c < quota. Every step that is not an evaluation decrements c before the loop's increment, so
it is free. Per step:

1. Source cursor at the end of the source list: reset both cursors (one 8-byte write of 0 to +0x17C), set +0x184 = 1,
   return. That visit is free and ends the slice at once.
2. Source not proposable (0x6ca110: list +0x2a0 empty, template switch > 0, exclusion counter 0): next source, target
   cursor = 0. Free.
3. **Global source** (goal type +0x20 == 0x10 exactly, not a bit test):
   - build the goal with a null target (0x691710) and call slot 9;
   - if slot 9 says skip: next source, free;
   - otherwise **E += 1 (0x1406cb2ae), then Evaluate**. The step counts whatever follows (Evaluate false, desire <= 0,
     slot 10 rejecting, or the insert). Then the next source.
4. **Targeted source:**
   - target cursor at the end of the list: as in step 2 (next source, free);
   - target vf+0x20 Applies(flags, AI player) false: next target, free;
   - otherwise build the goal and call slot 9. If it says skip: next target, free;
   - otherwise **E += 1 (0x1406cb511), then Evaluate**. The step counts; then the next target.

Consequence: a slice ends as soon as c reaches the quota (the end of the list is found on a later frame). A pass of
E evaluations at quota q takes floor(E/q) + 1 service frames. Shipped galactic goals use only Friendly, Enemy,
Neutral, Friendly|Enemy|Neutral, Enemy|Neutral and Global (my tally of the 60 `AIGoalApplicationFlags` in the
*GALACTIC* and SYSTEMGOALS XML). So the "== 0x10" global test and the port's bit test agree on shipped data. [H data]

#### B.4 Applies, PlanetAITargetLocationClass slot 4 = 0x665a30 [H]

The vtable 0x8b4780 entry +0x20 is 0x665a30. Its arguments are (flags, AI player).
1. False if the target has no planet (target +0x28) or there is no AI player.
2. The planet's behaviour 3 must exist, and Is_Planet_Revealed 0x3fa420 must hold. That function ignores the player:
   - true in multiplayer;
   - false with no PlanetData, or with the hidden byte PD +0x2E0 set;
   - true if the galactic mode's `Planet_Auto_Reveal` (+0x340, default true, not set by any shipped GC) is set;
   - else PD +0x2C9 (the local human's reveal byte).
3. o = (int) of the `TargetPlayerID` token (0x140a43dc4), evaluated on the target's evaluator (vf+0x28, then vf+0x8),
   i.e. the planet's owner id.
4. Result, each case giving one bit of `flags`:

   | Owner o | Result |
   |---|---|
   | o == the AI player's id (player +0x4C) | flags & 1 (Friendly) |
   | player o exists and its faction (+0x68) has the neutral byte +0x108 | (flags >> 2) & 1 (Neutral) |
   | 0x2823c0(self, o): relation table self +0x370 [o] == 0 | **false** for every flag ([M]: the 0 entry means allied) |
   | otherwise | (flags >> 1) & 1 (Enemy) |

5. The Contested (0x8) bit is never read.

This is the same body as A2M_T08 t14.2 (Reachable_Target's Applies); AI_GALACTIC §1.4 [L] can be closed.

#### B.5 Slot 9, the galactic skip 0x6b84f0, and the reach lookup [H]

1. 0x721a50: the active set (+0x38) is hashed by target pointer (goal +0x58). Each bucket holds per-target entries
   that are searched by goal type (0x6b83c0). A hit → skip. So the key is (goal type, target), whatever the source
   function.
2. A null target (global goal) → not skipped.
3. Otherwise reach = target vf+0x38 (0x665760: target +0x30, the PlanetReachabilityClass). It calls slot 6 (0x664450)
   with (goal type +0x2C `Reachability`, mask −1, null, null). A null result → skip.

Slot 6 (A2K_T02 Q1(b), A2L_T03 Q2) [H]:
- level 4 → the target itself, so never skipped;
- level > 5 → null, so always skipped;
- otherwise the first entry of the per-level list 0x665160(reach, level). That list is rebuilt when it is more than
  10 × FPS frames old.
- The list is the flood 0x3f12e0 toward that target, for that AI player. It holds the planets owned by the player or
  an ally:
  - a BFS from the target, when the target is owned by the player or an ally;
  - otherwise a BFS from each passable neighbour of the target (node order; the largest result is kept; visited
    marks are shared);
  - through planets that pass 0x3f2400 for the level's flags (f0 = f1 = level 5, f2 = level 1 or 5, f3 = level > 2,
    f4 = level > 1, fog on).
- With f4 clear (levels 0 and 1), a target not held by the player or an ally gives an empty list, so it is skipped.

So a planet is a candidate target for (player, goal type) when all of these hold:
- it is revealed or auto-revealed, and not hidden;
- its owner relation matches a set application bit (self / neutral faction / non-allied other);
- (type, planet) is not active;
- some planet owned by the player or an ally is flood-reachable toward it at the goal's level, under the 10 s cache.

There is no hop limit, and trade routes are links of the graph (adjacency = trade route or distance <=
`MaximumFleetMovementDistance` 100, 0x3f1c10). [H]

#### B.6 The count, for the port to reproduce

E for one pass is the sum over sources s, in 0x60f6c0 order, with each term taken on the frame of the slice that
visits it:
- 0 if s is not proposable;
- for a global s: 1 if (type, null) is not active;
- for a targeted s: the number of targets t, in list order, with Applies(t, flags) and not active(type, t) and
  slot 6(t, level) != null.

The proposability, active-set and 10 s flood states are taken on the frame that visits them. A plan that ends
mid-pass, or a flood cache that expires mid-pass, can change later terms of the same pass. [H]

#### B.7 Port vs retail differences found in this read

`engine/ai/src/galactic_ai.cpp:445` `GalacticAiWorld::Impl::applies` (its comment still says [L]):
1. **Friendly includes allies in the port.** Retail gives false for an allied owner under every flag. This adds
   evaluations for any player with allies. [H]
2. **Contested via `in_battle` in the port.** Retail never tests it. There is no effect on shipped galactic goals.
   [H data]
3. **No reveal / hidden test in the port.** Retail rejects hidden planets (PD +0x2E0) and, without
   Planet_Auto_Reveal, planets the local human has not revealed. [H]
4. **Neutral.** The port treats an unowned planet as neutral. Retail reads the owner's faction neutral byte, and every
   planet has an owner player [M]. These agree if the port's "unowned" is retail's Neutral player.

`GoalSystem` (ai_goals.cpp): the counting, the end-of-slice rule, the seed and the quota formula match retail. The
port's cap uses float and need uses double; retail uses float for both, with the same results for E < 2^24.
`PlayerAi::blocked`: it follows slot 9 / slot 6. Its 10 s cache uses `w->now`, which must be the frame clock that
retail's 0x665160 reads. [M]

None of 1-4 is shown to be the 3..5 extra Underworld evaluations: in HotM the Underworld has no allies and
auto-reveal is on [M]. The remaining suspects are:
- which planets are hidden (PD +0x2E0) in HotM;
- the active-set state at visit time;
- the 10 s flood-cache phase (when each (target, level) list was last built), which decides when a newly reachable or
  unreachable planet changes the count.

#### B.8 The probe that would settle it

No retail trace has per-candidate goal evaluations (q2_traces records plan starts only). Use a variant of the
project-local debugger `tools/probe_mod/cu8h_probes/cu8h_dbg.py`: the same attach, arm-file and stop handling, but
**hardware execute breakpoints** (DR7 R/W = 00, LEN = 00) instead of the LCG write watchpoint, at:

| Address | Event | Read on hit |
|---|---|---|
| 0x1406cb511 | targeted evaluation | rdi = goal system; r15 = goal type; r12 = function; rsi = PlanetAITargetLocation (planet object = [rsi+0x28], planet index byte = object +0x335 [M]) |
| 0x1406cb2ae | global evaluation | rdi; r15 = goal type |
| 0x1406cad26 | Commit (pass end) | rdi; [rdi+0x188] = E of the pass just ending (before the reset); [rdi+0x178] = the quota the pass ran at |

- Log on every hit: the global frame [0x140b0a320], the mode frame, and the owner player id
  [[[rdi+0x28]+0x28]+0x28]+0x4C [M: the chain read in 0x6cb120 / 0x665a30].
- Resolve the goal type names once from the goal-type pointers. The type's name string offset is not yet read [L]; a
  first run can log the pointers and match them by the order of the sources.
- Run: HotM, human Empire, the same difficulty and seed as the port's parity run, stopping at mode frame 700. This
  covers Naboo corruption at 617.
- The output is per-pass (frame, E, quota) plus the ordered (type, planet) list. Diff that against the port's
  GoalSystem with the same columns.
- Prepare it under `tools/probe_mod/` with run and cleanup scripts (rule 7). Gabriel launches the game.

**Port consequence:**
1. Fix `GalacticAiWorld::Impl::applies` (galactic_ai.cpp:445) to the B.4 table:
   - Friendly = owner is self only;
   - an allied owner gives false;
   - drop Contested;
   - add the Is_Planet_Revealed / hidden gate (the port has `planet_auto_reveal` and the reveal state in
     GalacticSnapshot);
   - check that `kAiFps` / `w->now` in the 10 s cache are the frame clock.
2. Optionally make `need` float (no result change).
3. Then compare the Underworld per-pass E against the B.8 probe. That probe is the missing data for per-(source,
   target) parity.

---

## Sections 5-6. b0122 (AIPlanetBuildQueue slots) and b0138 (AI pad-build payment)
Source: Ghidra decompilation of retail StarWarsG.exe (Global.c), call graph `docs/re/ghidra/callgraph.csv`. Names:
PP = PotentialPlanClass, Q = AIPlanetBuildQueueClass, BT = AIBuildTask, TF = task force. Tags: [H] read directly,
[M] one inferred link, [L] hypothesis.

### Short answers

- **b0122.** 0x53bee0(Q, index, PP, flag) writes the slot's flag byte only when the index is in range, the slot is live
  and its owner is that PP [H]. 0x53bda0(Q) sums the build times of live slots with flag 0 and leaves flag-1 slots out
  [H]. It also **cuts the vector after the last live flag-0 slot**, which deletes trailing empty slots and trailing flag-1
  slots for good [H].
- Slots are removed by three paths [H]: market step 5 at the producing queue (first slot matching (type, PP), this
  queue only); the PP destructor (0x6bec20 → 0x6c2520(PP, 0) → 0x53c730 at each selection's own source queue); and the
  0x53bda0 tail cut. The tail cut can run before the production start. Inside a commit it drops flag-1 slots of goals
  that are not yet re-asserted [H mechanism, M that it happens in practice].
- **The [H] reading is confirmed.** A slot made at planet A for a build that is produced at planet B stays at A until the
  PP is destroyed (or the tail cut removes it) [H]. Step 5 at B clears a B slot with the same (type, PP), if there is one,
  and never touches A [H].
- **b0138.** Retail **charges the AI for pad builds in every mode, campaign included** [H code path]. The payment is
  not made at the build. It is made when the tactical plan is accepted: 0x6c2d60, tactical branch, takes the PP's net
  cost PP+0x44 from the player's credits and keeps it as an escrow PP+0x1A4 [H]. PP+0x44 includes every pad structure
  the plan picked, priced by 0x3744e0 [H]. The build itself, 0x42edf0 → 0x42dbd0 with charge 0, is free for an AI player
  because it has already paid [H].
- Cost field [H]: skirmish (0x28afb0 true) uses `Tactical_Build_Cost_Multiplayer` (+0x9fc). Otherwise
  `Tactical_Build_Cost_Campaign` (+0x9f8) is used, falling back to the multiplayer value when below 1, and multiplied by
  the faction planet factor 0x33e440(faction, 1) when the battle planet passes 0x3a0910 (corruption [M]). No branch tests
  AI vs human, MP or campaign for whether the AI pays. The game mode only picks the field.

### b0122: AIPlanetBuildQueue slots

**Layout** [H]: Q+0x38/+0x40/+0x48 is a vector of 0x18-byte slots. +0 = object type (0 = empty), +8 = owner PP,
+0x10 = flag byte. Q+0x50 is the queue's running time, Q+0x18 the BT currently producing, Q+0x54 the queue index.
Save/load (0x53c4f0 / 0x53bf30) writes and reads all three fields per slot, flag included [H].

**Created by** [H]: 0x53b810(Q, type, PP), whose only caller is 0x6c2d60. It reuses the **first empty slot** and
otherwise appends. It sets flag 0 and returns the index, which 0x6c2d60 stores in PP+0x168[i]. This happens on the first
0x6c2d60 call for a PP (PP+0x100 still 0), once per selection that has a source planet (PP+0x180[i] +0xD0 ≠ 0) and is
not on the skip list 0x140b3a5d0. 0x559b40(planet queues, space/ground bit of the type) picks the queue. Indices stay
stable: no path compacts the vector, except the tail cut below.

**Flag values** [H]: 0 = counts; 1 = suspended. 0x53bee0 has two callers:
- 0x6c0be0 (Commit_Proposed_Goals step 4, once per PP: PP+0x100 set and PP+0x4E clear, then PP+0x4E := 1) writes 1 to
  every PP+0x168 slot, then calls 0x6c2320 (the free-store +0x30 unreserve);
- 0x6c2d60 on a kept goal (PP+0x4E set) writes 0 again, clears PP+0x4E and re-asserts +0x30 (0x6c2b20).
Both writes are no-ops when the index is out of range, the slot is empty, or the slot's owner is another PP [H]. Every
surviving active goal gets 0x6c2d60 in its category's Resolve_Category or maintenance, so the flag is 0 between commits
[M, from AI_GALACTIC §1.6].

**Who reads the flag** [H]: only 0x53bda0, reached from 0x53bb20(Q, extra = 1, …). Callers with extra = 1 are 0x6639f0
(PP ETA, from 0x6bfcf0) and 0x664210 (per-type production time), both only when the first-solve byte 0x140b3a524 is 0.
Init 0x6c0d00 sets that byte to 1; the re-validation 0x6c47c0 and the accept re-solve 0x6c4a50 set it to 0. So **only
re-solves count slots**, and both re-solves run inside Resolve_Category. A first solve uses the
production queue time 0x401ba0 without slots [H]. All other 0x53bb20 callers pass extra = 0 (0x525200 goal production
time, 0x65ba10, 0x667750, 0x669b00, 0x695c90) and never see slots [H].

**0x53bda0 in full** [H]: for each slot that is live and has flag 0, add 0x371230(type, player), the build time, and
remember its index L. Afterwards, if some L exists and L + 1 < size, resize the vector to L + 1 (0x53d530 is a plain
resize). The effects:
- trailing empty slots are trimmed (harmless);
- trailing **flag-1** slots are deleted. If no live flag-0 slot exists, nothing is cut. Inside a commit, between step 4
  and the category that re-asserts a kept goal, an accept re-solve of an earlier category can therefore delete that
  goal's trailing slots. The later 0x53bee0(…, 0) then fails its range or owner check silently, so the reservation is
  lost for the rest of the PP's life and stops counting in ETAs. Nothing else breaks: the BT is independent, step 5 finds
  no slot, and release is a no-op [H per link; M that retail commits hit this order].
- A new 0x53b810 may later append at the same index. The owner checks in 0x53bee0 / 0x53c730 keep other PPs from
  touching it [H].

**Removed by**:
1. **Market step 5** (0x53cd60, the production start, after the least-slack pick, before 0x64abf0) [H]: in **this**
   queue only, the first slot whose type equals BT+0x88 and whose owner equals the BT's PP (BT+0xA0 TF → +0xF0 plan →
   +0x88 goal → +0x68 PP) gets type := 0 and owner := 0. The flag byte is left as it is, one slot is cleared, then break.
   No other planet's queue is scanned.
2. **PP destruction** (0x6bec20 → 0x6c2520(PP, 0)) [H]: for every selection with PP+0x168[i] ≠ −1, set the index to −1
   and call 0x53c730(source queue, index, PP). That clears type and owner when the slot is still live and owned by the
   PP. 0x6c2520 has a second branch (argument ≠ 0: only the selections of one PP+0xE8 entry), but no direct caller passes
   ≠ 0 (decomp and call graph) [H for direct calls; L whether it is reachable indirectly]. The PP is destroyed when its
   goal is released or dropped, or when its plan ends.
3. **The 0x53bda0 tail cut** above [H].
4. Q destructor 0x53b720 frees the vector (planet object gone) [H].

No other function writes slot +0 / +8 (the only writers are 0x53b810, 0x53c730, the inline step-5 clear, 0x53bda0's
resize and load) [H, from the call graph: 0x53c730's only caller is 0x6c2520; 0x53b810's only caller is 0x6c2d60].

**The planet A / planet B case** [H]: a slot reserved at A is cleared only by 0x6c2520 at PP destruction (or by the tail
cut). The production start at B scans B's queue for (type, PP). If the same PP also reserved a slot of that type at B
(another selection with source B), **that** slot is consumed, and A's stays. So retail matches by (type, PP), not by
selection.

**Port consequence:** the port (engine/ai/src/galactic_pp.cpp `reserve_pp_queues` / `flag_pp_slots` / `release_pp`,
galactic_build.cpp step 5) already matches creation at accept, the flags, the step-5 (type, PP) clear in the producing
queue and release at PP release. Differences to fix (tier 1, AI timing):
- (a) Model the slot vector by position. Step 5 and release null a slot in place (not `erase`), and insertion reuses
  the first empty slot (0x53b810).
- (b) Add the 0x53bda0 tail cut to the re-solve read of planet queue time. Today `planet_queue_time` is const and
  called on non-first solves. It must resize after the last live flag-0 slot, so trailing suspended slots are deleted
  and the later re-flag of that PP is a no-op.
- (c) Comment fix: the queue-slot release is 0x6c2520 (from the PP destructor 0x6bec20), not 0x6c2320. 0x6c2320 is the
  free-store +0x30 unreserve.
- The flag is saved in retail. The port's "flag always 0 between commits, not saved" holds [M], so no save change is
  needed.

### b0138: does the AI pay for pad builds in campaign land battles?

**Price functions** [H]: 0x3743f0(T) returns +0x9fc in skirmish, otherwise +0x9f8, falling back to +0x9fc when below 1.
It has no planet factor. 0x3744e0(T, player) is the same choice, and outside skirmish it also multiplies by
0x33e440(player+0x68 faction, 1) when the battle planet (0x28a890) passes 0x3a0910 [H; M: corruption]. 0x28afb0(&0x140b153e0)
is the skirmish test (SKIRMISH_SETUP §0).

**AI purchase path (land and space, every mode)** [H]:
1. Solve, TacticalBuild Score 0x727d30: c = 0x3744e0 for a 0x2A (TACTICAL_UNDER_CONSTRUCTION) type. Veto if c ≤ 0 or
   c + PP+0x44 > player credits (+0x70). Commit 0x728000: unless MagicPlan (goal +0x50 → +0xA8), and when neither a
   free unit nor a pool unit fills the record, PP+0x44 += c. The pad is pushed to PP+0x150 and the type to PP+0x138.
2. Accept, 0x6c2d60 first call (PP+0x100 0 → 1), tactical branch (PP+0x198 ≠ 0): 0x27f370(player = PP+0x28 → +0x28,
   −PP+0x44, notify 1), then PP+0x1A4 := PP+0x44 (escrow) and PP+0x44 := 0. A kept goal's later 0x6c2d60 calls take the
   PP+0x4E path and charge nothing.
3. Lua TF `Build` / `Build_All` (land 0x6ea030 / 0x6ea620, space 0x6e5c50 / 0x6e6240): the friendly, rules (0x42e400)
   and cooldown (0x42daf0) checks, then 0x6c1e80(PP, T, pad). That function must find T in PP+0x138 and the pad in
   PP+0x150, removes both, releases the free-store pad reservation (0x4d6300) and does **escrow −= 0x3743f0(T)**,
   clamped at 0 (BUILD_TASK_LIST 0x648980 uses the same function with pad = null).
4. 0x42edf0(pad behaviour, pad, T, player id): for a player with 0x2814d0 true and player+0x62 == 0 (AI, not human [M])
   it calls 0x42dbd0(…, charge 0, sfx 0). No credit test, no deduction. Human players get the "Can't afford" test with
   0x3744e0 and a queued TacticalBuildEvent, whose execute 0x5d78a0 calls 0x42dbd0(charge 1), deducting 0x3744e0.
5. If no UC object exists afterwards, the TF function raises signal 0x22 on the pad and credits 0x3743f0(T) back
   (0x27f370(…, +cost, 0)).
6. Plan end 0x5f6a50 credits the remaining escrow PP+0x1A4 back (notify 1) before the PP is released.

**Mode dependence** [H]: none of 0x727d30, 0x728000, 0x6c2d60's tactical branch, 0x6c1e80, 0x6ea030 or 0x42edf0 reads
the skirmish test, the MP flag 0x140b15490 or a campaign flag to decide whether the AI pays. Only the price function
picks the field. So in a campaign land battle the AI pays `Tactical_Build_Cost_Campaign` (or `_Multiplayer` when
Campaign < 1) × planet factor at plan accept, and is refunded at plan end what the per-build escrow debits (base price
0x3743f0, no factor) did not use [H]. With factor f ≥ 1 the net cost after plan end is the base price, and the
surplus is held until then. With f < 1 the escrow runs out first, so the net is c·f [M: arithmetic of the clamp].
Exceptions: MagicPlan plans pay nothing (step 1). Lua `pad.Build` (0x5739c0, charge 0) is free for anyone, but it is
the story-script call, not the AI plan path [H].

**Fit with the skirmish trace** (SKAI_T06_LAND_RUNS "Credits") [M]: the AI paid pad builds "in one step", and the
anti-vehicle turret + Mineral Processor came to −1650 in a single frame. That is one plan's PP+0x44 charged at accept.
The same accept charge explains the tactical production trace: −4520 for five units in one frame, while the second
brigade only starts much later. The 0x498170 service never charges the AI (SKIRMISH_SETUP §3), because the plan already
paid [M].

**Port consequence:** replace the build-time charge with the plan-accept charge, in both modes:
- engine/ai/tactical/src/tactical_ai.cpp `TacticalAiWorld::Impl::reserve_pp`: on the first call, take the tactical
  PP's net cost (tactical_pp.cpp `p_.net`, which Commit already sums) from the player's credits (notify 1) and keep it
  as the escrow. Its comment says "the tactical credits reservation is not modelled".
- At the TF `Build` / `Build_All` reservation match (0x6c1e80): escrow −= base price (no planet factor), clamped at 0.
  Credit the base price back when no UC object appears. Refund the remaining escrow at plan end (0x5f6a50).
- engine/apps/sim/src/tactical_battle_bt.cpp `TacticalBattle::tactical_build`: change `if (!ai || skirmish_land())`
  to charge humans only. Otherwise skirmish double-charges once the accept charge exists. This also removes the
  [port] "campaign AI pad builds free" choice of L1_S4 §3.6 and SKIRMISH_SETUP (retail charges in campaign).
- `TacticalBattle::build_cost` / battle_ai.cpp `tactical_cost`: add the campaign planet factor 0x33e440 (0x3744e0) for
  the Score / Commit price.
- Check: the skirmish AI production charge at enqueue (battle_ai.cpp, skai t09) is probably the same accept charge
  (PP+0x44 also sums 0x496d80 prices of non-pad TacticalBuild picks). Move it to accept as well, or it is paid twice
  [M].
- Docs: L1_S4_REMAINING §3.6 and AI_TACTICAL §7.5 say "builds without being charged". That is true of 0x42edf0, but the
  AI has already paid at accept.

---

## Section 7. b0001 (land unit cap, pool-drop stop, 0x289050, empty-pad revert)
Sources: Ghidra decompilation of StarWarsG.exe (RVAs below), FoC XML (`FACTIONS.XML`, `EXPANSION_FACTIONS.XML`,
`GROUNDCOMPANIES*.XML`, `HEROCOMPANIES.XML`, `MARKERS.XML`, `GROUNDBUILDABLESSKIRMISH.XML`, `GAMECONSTANTS.XML`), and the
retail trace `tools/probe_mod/q2_traces/out/20261108-220755-skirmish-vanilla-lua/trace.q2t` (SKAI_T06_LAND_RUNS run 1,
Utapau, Rebel AI p1) re-read for the population sums. Tags: [H] read directly, [M] one inferred link, [L] hypothesis.

### Short answers

1. 0x2ac5c0 is right as SKIRMISH_SETUP §6 has it: cap = min(player capacity +0x404, int(curve(N))), which is 9 for FoC with
   2 players [H]. What it is compared against is not a company count. The "used" value 0x2aba70 is a float sum of
   per-object population shares in the owner's population list, rounded up. Each company member carries
   Population_Value / member count. Start forces are never put in that list, so they do not count [H]. With the 2 start
   infantry companies left out, the retail field at frame 15232 sums to 8.8, which rounds up to 9 = the cap. That
   explains "13 companies with a cap of 9" [H code + trace].
2. The pool drops stop because the population room test fails: used 9 >= cap 9 from frame 15422 (the last speeder wing
   unloads). After that no counted unit dies, so the test never passes again [H code, M trace fit]. Credits (20859
   piled up), timers and reinforcement-point ownership play no part. The capacity (>= 100 from REINFORCEMENT_POINT_REBEL)
   is never the limit [H].
3. 0x289050 returns true for every player without the is-human byte (+0x62), and GameConstants Pay_As_You_Go for a human
   [H]. In the tactical build manager, "true" means it charges nothing at enqueue and nothing per frame for a player
   that has an AI [H]. So the AI does not pay inside the build manager. It pays in the AI layer: when a tactical plan is
   instantiated, the plan's net TacticalBuild cost (PP+0x44) is taken from its credits in one Add_Credits step
   (0x6c2d60) and held as an escrow (PP+0x1A4). Each enqueue or pad reservation draws the escrow down (0x6c1e80), and at
   plan end the unspent escrow is paid back, multiplied by Credit_Multiplier (0x5f6a50) [H code, M that this is the
   traced -4520 step].
4. Empty-pad revert: a CAPTURE_POINT pad that is owned and empty (no built or under-construction child), with no
   candidate unit in its Capture_Point_Radius, decays to the Neutral player. It goes through one full transition of
   Capture_Point_Transition_Time_Seconds x 30 frames: 150 for SKIRMISH_BUILD_PAD, BUNKER_PAD and
   SKIRMISH_MINERAL_PROCESSOR_PAD (T = 5), and 240 for SKIRMISH_OUTPOST_PAD (T = 8) [H]. REINFORCEMENT_POINT and
   Ownership_Sticks types never decay, and faction base pads have no CAPTURE_POINT behaviour [H]. The port already
   has this rule (`engine/tactical/garrison/src/capture.cpp:88`).

### 1. What 0x2ac5c0 counts and against what

**Cap side (0x2ac5c0) [H].**
- In skirmish land: cap = min(player +0x404, (int) faction `Land_Skirmish_Unit_Cap_By_Player_Count`(N)).
  - The interpolator at faction +0x2CD8 is LinearInterpolator vf2 0x2c8660.
  - N = 0x294af0: players that are human (+0x62) or have an AI (0x2814d0) and have +0x180 set [M: +0x180 = active].
  - FoC Rebel / Empire / Underworld curve: (0,10) (1,10) (8,5). N = 2 gives 9.29, so the cap is 9.
- Space: faction `Space_Tactical_Unit_Cap`. Campaign land: `Max_Ground_Forces_On_Planet` (and min with +0x404 for a
  non-defender).
- Player +0x404 is refreshed by 0x289010 on object add / remove and owner change (0x29f810, 0x38e5a0, ...). It holds the
  capacity 0x2ac700: faction `Base_Population_Capacity` (+0x2EE8, 0 by default: zeroed in the faction ctor 0x33a580
  [M: that ctor is the faction's]) plus `Additional_Population_Capacity` (+0x2120) of the player's live objects.
  - It skips objects flagged +0x3A0 & 0x42.
  - In skirmish it also adds the capacity of allied players' objects.
- The capacity is per player plus allies. The curve is per faction at the global N. Used is per player only.
- FoC data: only markers carry capacity. Reinforcement_Point, `_Rebel`, `_Empire` and `_Underworld` give 100;
  `_PlusN_Cap` gives N. Every slot owns a REINFORCEMENT_POINT_<FACTION> from frame 0 (trace: object 161311 is p1's,
  161285 is p0's). So +0x404 is >= 100 and the curve value 9 is what binds [H].
- A player that loses all of its reinforcement-point markers would have cap 0 [H code, M gameplay].

**Used side (0x2aba70) [H].**
- It walks the population list of the owning player: GOM +0x548, stride 0x48, indexed by the object's owner +0x58.
- It sums the float share stored at object +0xF0 -> +0x198.
- The result is (int)sum, plus 1 when sum - floor(sum) > GC `Allow_Reinforcement_Percentage_Normalized` (FoC 0.0). Any
  fraction therefore rounds the total up.
- Remaining room = cap - max(used, 0) (0x2acb10). The room test 0x2b03e0(type): (int)(pop(T) + 0.5) <= cap - used; with no
  type it uses 1.0.

**Who is in the list: 0x29bcd0(GOM, object, company type) [H].**
- Share rules:
  - pop = pop(company, mode) (0x373500): `Land_Override_Population_Value` (+0x212C) if >= 0, else `Population_Value`
    (+0x2124).
  - The GameObjectType ctor 0x367250 sets Population_Value 0, the overrides -1 and Additional_Population_Capacity 0.
    A type without the tag therefore costs 0. `Droids_Team` has no tag and is free.
  - If the object's type is the company type, or 0x3973b0(object) is the company type, the object gets the full pop.
    0x3973b0 gives the carried company of a landing transport (A1_READINGS §32).
  - Otherwise the object gets pop / (number of `Company_Units` entries), or pop / `Squadron_Units` count, when its type is
    listed there.
  - An entry equal to the company's +0x22E0 type is not counted [L: field unidentified].
  - A share of 0 is not stored.
- Callers:
  - 0x2b5fe0, the land reinforcement drop. It registers the landing object (GALLOFREE / MOLDY_CROW /
    MILLENNIUM_FALCON _LANDING) with the carried company. An in-flight company therefore counts in full from the drop.
  - 0x6438a0, TransportLandingBehavior unload. It removes the landing object from the list (0x2b6a20 zeroes its share)
    and registers each spawned member with the company's share. Members spawned while the transport unloads count only
    when the unload finishes. Until then the transport carries the full pop.
  - 0x29e960, company placement, only when its argument 8 is set.
  - 0x2a00b0 and 0x2bf2d0 [L: not read].
- **Start forces: 0x2a4fc0 places `Land/Space_Skirmish_AI_Default_Forces` with 0x29e960(..., arg8 0, arg9 0, arg10 1).**
  They join the garrison list (+0x550) but never the population list, so they cost no population for the human or the
  AI [H].
- Heroes count like any company (Han_Solo_Team_Land_MP and Katarn_Team are 1 via the variant copy 0x36c000).
- Structures, pads and markers are never registered by these paths; Population_Value 0 by default as well [M].
- Units in the reinforcement pool (player +0x2C8, types only) count nothing until dropped [H].

**Trace fit (run 20261108-220755, p1 Rebel) [H trace, shares from XML].**
- Shares used: Squad_Rebel_Trooper 1/15 (15 Company_Units), T2B 1/5, MPTL / MPTL_Spotter 1/4, Rebel_Infiltrator 1/2,
  Snowspeeder 1/3, Han / Chewbacca 1/2.
- At 15240 the field holds 51 squads, 12 T2B, 4 infiltrators, 2 + 2 MPTL, 3 snowspeeders, Han + Chewie and the droids.
  The share sum is 10.8.
  - All 30 start squads (ids present at frame 0) are alive from 0 to 36000; they never fought. Leaving them out gives
    8.8, which rounds up to 9 = cap.
- Every drop fits:
  - 3856: 2 companies dropped, used 1 (Katarn).
  - 13948: 3 companies dropped, used 4.93 -> 5, remaining 4.
  - 14833: speeder wing dropped, used 7.8 -> 8, room 1.
  - 15422: unload, used 8.8 -> 9, no room.
  - With start forces counted, the 14833 drop would have been refused (used 10 > 9).
- Float32 note: the sum is float32, accumulated in list order. 30 x float(1/15) = 2.0000007, which rounds up to 3 if
  start forces were counted [H arithmetic]. Order and precision matter near integers.

**Overshoot rule (land TF.Reinforce, 0x6ed5f0 -> 0x6ee010) [H].**
- Entry needs room for one (0x2b03e0 with no type), else the call fails at once.
- The remaining room 0x2acb10 is read once. Every pending unit, last to first, whose pop(T, 1) <= that value is spawned;
  the value is not decremented.
- So one call with remaining 1 can drop several pop-1 companies.

Port consequence:
- `BattleAi::population_used` (`engine/apps/sim/src/battle_ai.cpp:1376`) and `TacticalBattle::population_used`
  (`engine/apps/sim/src/tactical_battle.cpp:2479`) count a whole participant's pop while any member lives, start forces
  included. Change them to:
  - per-member shares (pop / Company_Units count, summed over live registered members);
  - start-force participants excluded;
  - an in-flight landing counted at the full company pop until unload;
  - float32 sum, then round up.
- Keep `skirmish_land_unit_cap` / `unit_cap_for_players` (9) and the capacity code as they are.
- Check that the land TF.Reinforce loop (`engine/ai/tactical/src/tactical_ai.cpp` near :3104) reads the remaining room
  once and does not re-test per unit.
- Regenerate the land skirmish goldens.

### 2. Why pool drops stop after frame 15232

- The only engine gate on a land skirmish drop is the population room. 0x6ed5f0 checks 0x2b03e0 (room for 1); then
  0x6ee010 checks the remaining room per unit (§1) [H].
- No credit test: bought units are already paid and sit in the pool. No timer: `Skirmish_Reinforcement_Delay_Frames` is
  not on this path [M]. The point-zone rule (0x6eaf60) only chooses landing positions [H].
- In the run, p1 holds REINFORCEMENT_POINT_REBEL and REINFORCEMENT_POINT the whole time [H trace]. Credits climb to 20859
  [H, SKAI_T06].
- From 15422 counted used = 9 = cap. Between 15240 and 36000 the Rebel object list does not change (152-153 objects, same
  counts at 20000). No counted member dies, so room never returns. Every QuickReinforce -> TF.Reinforce returns the
  failing ReinforceBlockStatus (0x81a) [H code, M trace].
- The 26 later purchases pile up in the pool. Tactical production does not test the population: the AI keeps buying
  [M trace; no pop test in 0x497140].

Port consequence: none beyond §1. Once used follows retail, the stop should come out by itself; check it with an
idle-human Utapau run (last drop near 15232, then none).

### 3. Correction for SKIRMISH_SETUP §3: what 0x289050 does

(Replaces the "Pay-as-you-go test" bullet and the "Net effect" sentence of §3.)

- **0x289050(player) [H]:** returns 1 when the player's is-human byte +0x62 is 0 (every AI and non-playable player).
  Otherwise it returns GameConstants `Pay_As_You_Go` (FoC false). The name means "not charged up front".
- **Enqueue 0x496a20 [H]:** when 0x289050 is false (FoC humans), it fails if credits < cost, else it takes the full cost
  at once (Add_Credits(-cost)). When true, nothing is charged at enqueue.
- **Service 0x498170 [H]:** a player that has an AI and is not human, or any non-PAYG player, only advances the timer
  (0x499070). Only a PAYG human pays (slice / total) x cost per frame (0x498ec0) and stalls when it cannot pay
  (0x499090). Completion 0x498f70: a non-PAYG entry completes on its timer, a PAYG entry when its remaining time is 0.
  **The build manager never charges an AI player.**
- **Refund on cancel 0x498820 [H]:**
  - It refunds 0x499000(entry): the full entry cost for a non-PAYG payer, and clamp(1 - remaining/total, 0, 1) x cost for
    a PAYG payer (the paid part), via Add_Credits(..., 1).
  - Player cancels through ProductionEvent (0x524130) always refund. The auto-cancel in the service (re-validation
    0x497140 fails) refunds only non-human players.
  - An AI that was never charged therefore gains progress x cost on an auto-cancel [M effect]. Clearing the queues
    (0x496830) refunds every entry the same way.
- **Where the AI pays (land and space tactical) [H code; M timing vs trace]:**
  - When a tactical plan's PP is instantiated, 0x6c2d60 runs. It is guarded by PP+0x100 / +0x4E: once per instantiation
    [M]. With PP mode +0x198 not 0 (tactical) it calls Add_Credits(player, -PP+0x44, 1), sets the escrow PP+0x1A4 to
    that amount and clears PP+0x44.
    - PP+0x44 is the plan's net cost, which in tactical holds only TacticalBuild marshaller costs (AI_GALACTIC_3 §1,
      Reset 0x6c3080).
  - AIBuildTask 0x648980 (skirmish branch) then enqueues through 0x496a20 for each builder until one accepts. On success
    0x6c1e80(PP, type, 0) lowers the escrow by the type's MP cost (0x3743f0, clamped at 0); pad reservations do the same.
  - At plan end 0x5f6a50 the unspent escrow goes back with Add_Credits(+PP+0x1A4, 1). Being positive and for an AI
    player, Add_Credits multiplies it by the difficulty Credit_Multiplier [H 0x27f370].
  - Pad builds from `Build` / `Build_All` (0x6ea030 / 0x6ea620) are issued through 0x42edf0 with no charge. When no
    under-construction object appears, they credit +cost back to the player directly (not to the escrow) [H].
  - Result as seen in the trace: the AI pays the full `Tactical_Build_Cost_Multiplayer` of a plan's whole shopping list in
    one step, before the items build. Example: 2295 -4520 = Katarn 1300 + 2 x 1100 + 320 + 700, while the second brigade
    starts building only at 3735.

Port consequence:
- The port charges the AI per order (skai t09: `battle_ai.cpp:1289`, `tactical_battle.cpp:5529`,
  `tactical_battle_bt.cpp:304`). Per-order charging gives the same totals while every order succeeds. For tier-1 parity,
  model the escrow:
  - take the plan's net cost at plan start;
  - draw it down per accepted order;
  - refund the rest at plan end times Credit_Multiplier;
  - refund a failed pad build to credits;
  - never charge AI entries in the build manager, but do refund the progress fraction on an AI auto-cancel.
- File this under backlog if the fixed-total behaviour already passes the land goldens.

### 4. Addition for L1_S4_REMAINING §4.2: empty-pad revert [H unless tagged]

(Paste after the "Controller this tick" bullet.)

- **Empty owned pad decays to Neutral.**
  - Rule (a) keeps the owner only while the pad's TACTICAL_BUILD_OBJECTS pack (+0x150) holds a child at +8 or +0x10
    [M: built object / under-construction object].
  - When the pad is empty and no candidate is inside `Capture_Point_Radius`, rule (d) returns the Neutral player (0x294aa0),
    unless the pad has REINFORCEMENT_POINT (0x49) or its type has `Ownership_Sticks` (+0x95) (0x500a00 tail).
  - The service (0x501980) then treats Neutral as a foreign controller: pending := Neutral and progress += 1 / (T x 30)
    per service (interval 1; decay uses m = 1).
  - When progress reaches 1, capture completes (0x500e30) with old owner -> Neutral. Owner change goes through 0x38e5a0,
    which refreshes capacity +0x404 through 0x289010 [H callgraph]. Progress := 0, the team colour is refreshed, the mode
    is notified (0x360d90), STORY_CAPTURE_STRUCTURE fires with the Neutral faction, and the local SFX / "lost" speech
    plays.
- **Timer:** T = `Capture_Point_Transition_Time_Seconds`.
  - SKIRMISH_BUILD_PAD, BUNKER_PAD and SKIRMISH_MINERAL_PROCESSOR_PAD: 5.0 s = 150 frames. SKIRMISH_OUTPOST_PAD: 8.0 s =
    240 frames. The Reinforcement_Point has 10.0 s but never decays (REINFORCEMENT_POINT).
  - Float32 accumulation of 1/150 needs 151 services to reach 1.0 [H arithmetic]. The retail trace saw 148-151 frames
    after the owner's last unit left (four bunker events, SKAI_T06 §2) [H trace].
- **What resets or pauses the timer:**
  - A candidate of the owner (or an ally) in the radius makes the controller the owner, so progress falls by the same
    step and partial decay is undone gradually, not at once.
  - A non-allied candidate makes it contested (Neutral plus flag) and the decay goes on. Afterwards an enemy candidate on
    the now-Neutral pad captures it in a second T.
  - Candidates are alive (+0x3A0 & 0x42 clear), not garrisoned, `Influences_Capture_Point` units with a faction. The pads
    themselves have `Influences_Capture_Point` false.
- **Destroyed child vs empty pad:** the existing rule still holds (SKIRMISH_SETUP §8): when the pad's child dies the pad
  goes to Neutral at once with no transition. The 150-frame decay covers pads that are owned but empty: captured and
  never built, built child sold (the sale leaves the owner), or the pad owner's units walking away.
- **Pads that never revert:** `<FACTION>_BUILD_PAD` and the base `*_BUILD_PAD` types (Rebel_Build_Pad,
  R_Ground_Barracks_Build_Pad, ...) have no CAPTURE_POINT behaviour, so they keep their owner when empty [H XML].

Port consequence: none in logic. `engine/tactical/garrison/src/capture.cpp` (`controller` :66-89, `service` :92-118)
already applies (a)/(d), the sticky rule (`garrison_types.cpp:55`) and the float step. Check in a port run that an empty
owned BUNKER_PAD / SKIRMISH_BUILD_PAD goes Neutral 151 frames after the owner's last unit leaves.

### Open items

- Company type field +0x22E0 that 0x29bcd0 leaves out of the member count [L].
- 0x2a00b0 / 0x2bf2d0 (other population registrations) not read; possibly survivors / Reinforce_Unit [L].
- 0x6c2d60 guard flags PP+0x100 / +0x4E: confirm the escrow is taken once per plan instantiation and not at every refresh
  [M].

---

## Section 8. b0002 (Setup A, setup_c, reach 7-9, Produce_Force(nil), paid(), Land_Units)
Second, independent pass over the six a2m t04 questions. The first pass is `A2M_T04_RE_ANSWERS.md` (2026-11-09), and
a2m t05 ported it. This pass re-read every function it relies on in the Ghidra decompilation of the FoC x64
`StarWarsG.exe` (RVAs relative to 0x140000000). It also read the shipped FoC XML (`PLANETS.XML`,
`GAMECONSTANTS.XML`, `AI/GOALS/*.XML`, `UNITS_SPACE_UNDERWORLD_INTERCEPTOR4.XML`) and compared the results with the
current port (`engine/ai/src/galactic_pp.cpp`, `galactic_ai.cpp`, `galactic_build.cpp`, `engine/lua/src/bind_ai.cpp`,
`engine/galactic/src/strategic.cpp`). Behaviour is described in prose; no decompiled code is reproduced.
Tags: [H] read directly; [M] one inferred link; [L] hypothesis.

Result: the first pass holds on every point. This pass adds five corrections or precisions:
- the third clause of the lock test 0x39ad40 (Q1);
- what 0x42e3b0 / 0x42e3d0 return (Q1);
- the level ≥ 6 early exit of 0x664210, which the port's setup_c does not follow (Q2);
- the shipped goal-XML census for levels 7-9 (Q3);
- the refund of a build-task charge in 0x647df0 (Q5).

### Short answers

1. **Setup A exclusions.** These fields are tactical-only state:
   - +0x210 = garrison data pack (+8 garrison state, +0x34 occupant count);
   - +0x3A7 = in limbo / hidden in a container;
   - +0x3B4 = in a hero clash;
   - 0x39ad40 = movement lock (lock count +0x3A8 > 0, the unit's own locomotor lock, or the lock of its team
     container's locomotor);
   - +0x35B → 0x42e3b0 / 0x42e3d0 = a build pad that holds a built structure (+0x150 → +0x10) or one under
     construction (+0x150 → +8).

   None of these is set for a galactic unit, so a unit in a moving fleet is **not** excluded. For a unit in a moving
   fleet, the fleet pack +0x50 is the move's **final destination**. [H for the rules, M for "never set in galactic"]
2. **setup_c test.** 0x664210 tests **revealed** (0x3fa420), a production object at the planet (+0xd0) and the full
   can-build gate 0x2804d0. It has **no surface test of its own** [H]. The only surface rule is step 24 of 0x2804d0:
   - it applies to ground kinds only (DUMMY_GROUND_BASE 0x5B, DUMMY_GROUND_COMPANY 0x5D, DUMMY_GROUND_STRUCTURE 0x5F;
     generic heroes are exempt);
   - space units pass whatever the surface.

   The_Maw has `Planet_Surface_Accessible No`, so 0x375220 returns false there:
   - Interceptor4_Frigate (DUMMY_STARSHIP) passes and is estimated at The_Maw;
   - ground companies and ground structures are vetoed there.

   New: for a level ≥ 6, 0x664210 returns 1e18 at once [H].
3. **Reach levels 7-9.** Slot 7 (0x663810) uses these thresholds [H]:

   | Level | GameConstants field | Offset | FoC value |
   |---|---|---|---|
   | 7 | `Low_Threat_Reachability_Tolerance` | +0x157c | 1000.0 |
   | 8 | `Medium_Threat_Reachability_Tolerance` | +0x1580 | 3000.0 |
   | 9 | `High_Threat_Reachability_Tolerance` | +0x1584 | 5000.0 |

   A path is accepted when (high 16 bits of the hop sum) × 100 ≤ the threshold. No shipped galactic goal uses levels
   7-9 [H, XML census]. Tactical FreeStore Setup A always uses level 7 [H].
4. **Produce_Force(nil).** An explicit nil, or a nil stage variable, is the script error "Expected a planet object for
   parameter 1": nothing is built, no status is returned and the script stops [H; nil counted as present: M]. Only a
   call with **no** argument, in galactic mode, clears PotentialPlan +0x4d (`Test_Target_Contrast`, default 1) [H].
5. **paid().** 0x5f9f90 adds +1 to a per-task-force {type → count} map at TF +0xa8 that nothing reads. It is not
   money [H].
   - The build task stores no amount, only the "charged" byte BT +0xd0 [H].
   - The charge happens at BUILD_TASK_LIST time (or when every free-store claim lapses): it moves cost(T, 0) from the
     goal's required (+0x28) to its allocated (+0x2c) [H].
   - The spend `s` (+0x40) grows by cost(T, 0) when the market starts production (0x64abf0) [H].
   - New: the charge is reversed only when a free-store unit fills a charged task (0x647df0: alloc −= min(cost,
     alloc); required is not restored) [H].
6. **Land_Units at an enemy planet with an accessible surface.** It does **not land and does not start an invasion**
   [H]. It returns its status object (no warning, no false). It only fires `Ground_Activated` abilities: those of
   non-transport units, and of the first eligible cargo unit of each transport. Transports land only when the
   planet's owner is the player or an ally [H].

---

### 1. Setup A exclusions (FreeStore Setup A 0x7287a0) and the in-transit planet

#### 1.1 Order per free-store entry (re-read) [H]
1. **Reservation.** The entry is considered only when PP +0x104 == 0 (first solve) or entry +0x30 == 0.
2. **Health.** 0x396470(unit) < `Health_Low_Percent_Threshold` (0x140b27338) → skip.
3. **Key slot.** The key slot is set to 0.
4. **Stage key**, only with a goal target and when 0x3a9760 (hidden) is false:
   1. Find the planet:
      - walk the unit, then its +0x2b0 parents, to the first object with PLANET (+0x335 != −1);
      - else, if the unit's **direct** parent has FLEET (+0x336 != −1), the planet is 0x432c30 = that fleet's pack
        (+0xc0) **+0x50**;
      - else drop the entry.
   2. The planet must pass `Is_Planet_Revealed` 0x3fa420, else drop the entry.
   3. Unit reach object = reach list[0x3f8500(planet)]. Level = 3, or 4 when the goal's Reachability is 4.
   4. Per stage:
      - only when **both** the stage's reach object and the unit's are non-null (new precision): add c² when slot 7
        c != 1e18, and add the squared 2-D distance from the unit object's own +0x78 / +0x7c to the stage position;
      - no finite c in galactic → drop the entry;
      - otherwise key = sum / nStages, and the running maximum 0x140b3a520 is raised **before** step 5.
5. **Exclusions.** Clause A (engaged), clause B (special weapon), then each of the following skips the entry:
   - **garrison:** +0x210 non-null with (+8 != 0 or +0x34 != 0);
   - **limbo:** +0x3a7 != 0;
   - **hero clash:** +0x3b4 != 0;
   - **lock:** 0x39ad40 true;
   - **indigenous:** 0x370320(type, 0x20 INDIGENOUS_UNIT, mode) true;
   - **build pad:** +0x35b != −1 and (0x42e3b0 or 0x42e3d0) non-null.

   An entry that passes them all is appended.

#### 1.2 Meaning of each excluded field
- **+0x210 GarrisonDataPack** [H, `A2M_T04` Q1.2]:
  - +8 = garrison state (entering or garrisoned);
  - +0x34 = occupant count (`GarrisonSlotsAvailable` = capacity − +0x34);
  - the pack is allocated lazily by GARRISON_* behaviour code only. In the FoC XML these behaviours appear only in
    `LandBehavior` lists, so a galactic object never has it [M].
  - Excluded: a garrison building with occupants, or a unit inside or entering a garrison.
- **+0x3a7 "in limbo"** [H writers]:
  - set by containment 0x37ade0 (garrison enter, transport landing, tactical battle setup, Lua spawn, vehicle thief),
    by TransportLanding and by Lua `Set_In_Limbo`;
  - no galactic writer [M].
  - Excluded: a unit hidden inside a container (for example cargo waiting in a landing transport).
- **+0x3b4 hero clash** [H]: set on both duelling heroes by MeleeHeroClash / ActionDrivenHeroClash. Tactical only.
- **Lock 0x39ad40** [H, re-read], true when any of these holds:
  1. the lock count +0x3a8 > 0 (raised by `Lock_Current_Orders`, abilities and hyperspace arrival);
  2. the unit's own locomotor (behaviour slot 1, +0x333) answers vf+0x1d0 true;
  3. **correction:** the unit's parent (+0x2b0) has TEAM (+0x348 != −1) and a locomotor whose vf+0x230 and vf+0x1d0
     are both true. So a team *member* counts as locked when its team container's locomotor is locked. The first pass
     wrote "the unit is a land team container".

  In galactic, a unit's parent is a fleet or a planet, neither of which has TEAM, and no shipped galactic script calls
  `Lock_Current_Orders` [M].
- **+0x35b → 0x42e3b0 / 0x42e3d0** [H, re-read]:
  - both helpers ignore the behaviour argument and read the **unit's own** data pack +0x150 (the build-pad pack);
  - 0x42e3b0 returns pack +0x10 (the finished structure) and 0x42e3d0 returns pack +8 (the structure under
    construction), as `TACTICAL_COMBAT.md` l.1994 / l.2051 read them;
  - so Setup A excludes a TACTICAL_BUILD_OBJECTS pad that is occupied. This is a land-only behaviour.

So in galactic the only exclusions that can fire are reservation, health, unrevealed planet, no finite stage cost
and INDIGENOUS [M]. A unit in a moving fleet is kept [H].

#### 1.3 In-transit fleet +0x50 = final destination [H, re-read]
- Fleet move 0x431c80:
  - refuses when the orbit planet (pack +0x40) is null or +0x50 is already set (the fleet is already moving);
  - otherwise departs the source (0x4358c0 clears +0x40 and nulls the fleet's parent);
  - then stores the move's target argument (the planet it was asked to go to) in pack +0x50.
- The locomotor service 0x56a1e0 clears +0x50 (0x435690 with null) on arrival.
- A blocked hop (0x569980) rewrites +0x50 to the previous route planet (retreat).

So +0x50 is the **destination**: neither the origin nor the next hop. While the fleet is moving, the unit's own
position is the stale pre-move one [M/L, A2M_T04 open item 1].

**Port consequence:** none in the exclusions. `FreeStoreM::setup_a` (`galactic_pp.cpp` ~l.355-410) already uses
`path.back()` and has none of the tactical exclusions. The remaining known difference is the distance term: the port
uses `fs.position` (the fleet's position), retail uses the unit's own stale position. This is recorded as [port]
there; leave it until the A2M_T04 open item 1 probe settles it.

---

### 2. Production setup_c 0x725c50 → 0x664210: revealed, not surface

#### 2.1 0x664210 (re-read) [H]
- **Level ≥ 6 returns 1e18 immediately** (new precision: the whole body is under level < 6).
- Otherwise it refreshes the level's lists (0x665160) and walks list[level + 1] of the target's reach object. A planet
  counts only when all three hold:
  1. 0x3fa420 revealed: true in multiplayer; false without planet data or with PD +0x2e0 set; true with galactic
     auto-reveal (mode +0x340); else PD +0x2c9, the one per-planet reveal byte of the local human;
  2. planet +0xd0 (its production object) is non-null;
  3. 0x2804d0(player, T, planet, 0, 1, 0, 0) passes.
- Value of a counting planet = btime 0x371230(T, planet) + queue time. The flag argument picks the queue time source:
  - flag 0: the AIPlanetBuildQueue time, 0x559b40 then 0x53bb20;
  - flag ≠ 0: 0x401ba0 on the PRODUCTION behaviour.
- Result = the mean over counting planets; no counting planet gives 1e18.

#### 2.2 Where the surface is tested [H]
0x664210 never calls 0x375220. Step 24 of 0x2804d0 passes when both of these hold:
- 0x375220 is true, **or** T has none of 0x5B / 0x5D / 0x5F, **or** T +0x14 `Is_Generic_Hero` is set;
- PD +0xb8 null, or T +0xc3 == 0, or 0x4b83d0.

0x375220(planet type, planet) itself returns false when PD +0x2c8 (destroyed) == 1. Otherwise it returns type +0xb9
`Planet_Surface_Accessible`.

#### 2.3 Space units vs ground units at The_Maw
- `PLANETS.XML`, The_Maw: `<Planet_Surface_Accessible> No </Planet_Surface_Accessible>`. 0x375220 is false whether or
  not the planet is destroyed [H].
- **Space units** (Interceptor4_Frigate: `Behavior DUMMY_STARSHIP, ...`, none of 0x5B / 0x5D / 0x5F) pass step 24
  through the "no ground kind" clause. They count at The_Maw whenever it is revealed, has a production object and
  passes the other build-gate steps [H]. Retail building Interceptor4 at The_Maw at tick 88
  (`A2M_R3_REBEL_UW_TRACE.md` row 47) shows that The_Maw was revealed then [M].
- **Ground companies and ground structures** (0x5D / 0x5F, and 0x5B bases) fail step 24 at The_Maw unless the type is
  a generic hero [H].
- 0x3fa420 does not depend on the XML surface field. It depends on the reveal state (and on `Planet_Auto_Reveal`,
  type +0xcb, through the mode flag) [H].

**Port consequence:** most of this is already done.
- setup_c (`galactic_pp.cpp` ~l.644-668) and `recompute_eta` (~l.240) use `is_planet_revealed` + `can_build_at`, and
  `GalacticSim::can_build` step 24 (`strategic.cpp` ~l.262-266) has the ground-kind surface rule. So Interceptor4 is
  no longer vetoed at The_Maw.
- One small difference remains: setup_c's `source_list()` clamps the level to 5 (`std::min(reach_level, 5)`), where
  retail returns 1e18 for every non-structure candidate when the goal Reachability is ≥ 6. No shipped galactic goal
  has such a level (§3.3), so there is no parity effect today. The fix is to set `prod_t = kInf` when
  `reach_level > 5`, as `recompute_eta` already does.

---

### 3. GameConstants reach thresholds for levels 7-9 (slot 7, 0x663810)

#### 3.1 Thresholds (re-read) [H]
- Early exits:
  - no path finder or no planet → 1e18;
  - source planet == target planet → 0.
- Otherwise the path is the all-pairs path (0x690f10). The threshold per level:

| Level (AIGoalReachabilityType) | Threshold | Summed range |
|---|---|---|
| 0 Friendly_Only, 2 Enemy_Destination, 3 Enemy_Undefended, 6 No_Threat | 1e-4 | [0, 1000) |
| 4 Single_Hop_Disconnected | 1e-4 | [0, path length − 2) |
| 1, 5 Any, 10 Any_Threat | 1e18 (always passes) | [0, 1000) |
| 7 Low_Threat | GC +0x157c (0x140b16d7c) `Low_Threat_Reachability_Tolerance` | [0, 1000) |
| 8 Medium_Threat | GC +0x1580 (0x140b16d80) `Medium_Threat_Reachability_Tolerance` | [0, 1000) |
| 9 High_Threat | GC +0x1584 (0x140b16d84) `High_Threat_Reachability_Tolerance` | [0, 1000) |
| any other level | 0.0 | [0, 1000) |

- Values: FoC `GAMECONSTANTS.XML` l.769-771 give 1000.0 / 3000.0 / 5000.0, and the ctor defaults are the same
  [H, xml_field_tables.md l.1470].

#### 3.2 How the thresholds are used [H]
- 0x6913a0 accepts when float((S >> 16) × 100) ≤ threshold, where S is the 0x691250 uint32 hop-weight sum. An empty
  path gives S = 0xFFFFFFFF, which levels 7-9 reject.
- Accepted: the cost is 0x691370 = (S & 0xFFFF) / 10. Rejected: 1e18.
- In galactic, the high half is the summed node threat in hundredths of strength (`A2K_T02` Q2(c)). So level 7 accepts
  a path whose summed enemy strength is about ≤ 1000, level 8 ≤ 3000 and level 9 ≤ 5000 [M].

#### 3.3 Who reaches 7-9
- Galactic Setup A passes level 3 or 4 only. Tactical Setup A (PP +0x198 != 0) passes level 7 for every unit [H].
- 0x664210 and the ETA 0x6639f0 return 1e18 for levels > 5 [H].
- Shipped goal `Reachability` census [H, XML]: the galactic goal files use only Any, Friendly_Only, Enemy_destination,
  Enemy_Undefended and Single_Hop_Disconnected. Low / Medium / High_Threat appear only in land and space goal files
  (`LANDGOALS`, `AI_GOALS_*_LAND`, `OFFENSIVESPACEGOALS`). So no shipped galactic goal reaches levels 7-9 through
  PlanetReachability.

**Port consequence:** none. `ai_reach_path_cost` (`ai_free_store_path.hpp` l.70-82) already takes the three
tolerances from `rules().constants.threat_reachability_tolerance` (defaults 1000 / 3000 / 5000), applies the
(S >> 16) × 100 rule and gives other levels 0.0. Tactical: `engine/ai/tactical/src/constants.cpp` loads the same
fields.

---

### 4. Produce_Force with an explicit nil, and the +0x4d clear (0x5fe240)

#### 4.1 Arguments [H]
- **n ≥ 1.** Argument 1 must be a GameObjectWrapper (class token 0x140a43b18, object via 0x5791e0) or an
  AITargetLocationWrapper (0x140a44280, wrapper +0x38 → vf+0x30). The object must have the PLANET behaviour
  (vf+0x10 with id 3).
  - Anything else raises the script error 0x248190 "TaskForceClass::Produce_Force -- Expected a planet object for
    parameter 1": a null slot, a nil value, a bool, a number or a non-planet object. 0x248190 sets the script's error
    byte (+0x121), and the script stops at the end of its pass.
  - The function then returns 0 results: no status object is created and no build task.
- **n ≥ 2.** Argument 2 must be a LuaBool (0x140a157b0), else the error "...Prioduce_Force -- invalid type for
  parameter 2. Expected bool" (typo in the exe).
- **nil counts as present.** The binding layer counts a Lua nil as a present argument (`LUA_API.md` l.1740) [M]. So
  `Produce_Force(nil)`, and `Produce_Force(stage)` with `stage == nil`, take the error path, not the no-argument path.
  Either way of passing the nil (a null slot or a nil object) reaches the same error [H].

#### 4.2 The +0x4d clear [H]
- With **no** argument (destination null) and mode type 0 (galactic, plan → +0x28 → +0x34 == 0), the function writes
  0 to PotentialPlan +0x4d. The path is TF +0xf0 plan → +0x88 goal → +0x68 PP, with no null checks.
- PP +0x4d is `Test_Target_Contrast`:
  - the PP ctor 0x6be9c0 writes 0x0101 to +0x4c, so it is 1 by default;
  - Lua `Test_Target_Contrast` 0x6008f0 writes it.
- Reader 0x6c47c0: for a running plan (+0x100) whose PlanDef +0xaa (IgnoreTarget) is 0, +0x4d != 0 makes the
  enemy-contrast re-test run (0x588980 / 0x5f1990 / contrast marshaller). That re-test can set the validity byte +0x4c
  to 0 and drop the plan.
- So a destination-less `Produce_Force()` in galactic stops the contrast re-test for that plan.
- The nil error path writes nothing [H].

**Port consequence:** none. `m_tf_produce_force` (`bind_ai.cpp` l.169-191) raises `script_error_stop` for a present
non-planet argument 1 (nil included) and for a non-bool argument 2. `tf_produce_force` (`galactic_ai.cpp` ~l.497-508)
clears `test_contrast` on the no-argument path, and PP creation sets it to true (l.1025). The flag has no effect until
0x6c47c0's contrast re-test is ported (already noted there).

---

### 5. paid() (+s at 0x5f9f90) and the stored build-task charge

#### 5.1 0x5f9f90 [H]
- It inserts {type, 1} into the TF +0xa8 hash map (0x2220f0), or increments the existing count. That is all it does.
- Callers: 0x6498f0 (galactic hand-over), WAIT_FOR_FREESTORE_ALLOC 0x64aec0, and PRODUCTION_END (three sites, l.1088734
  / 1092211 / 1092319 of Global.c).
- No reader exists apart from the dtors [M, A2M_T04 Q5.1].
- It is **not** a budget call, and nothing is added to `s` there.

#### 5.2 When the charge is stored, spent and reversed [H]
- **Charge.** The charge is made in BUILD_TASK_LIST 0x648980 or, after every claim lapsed, in 0x64ab50 (from
  0x649070). It needs all of:
  - the AIPlanetBuildTask BT +0xc8 is non-null;
  - BT +0xd0 == 0 (not charged yet);
  - BT +0xa8 == 0;
  - cost 0x3711c0(T, 0) > 0.

  It then sets BT +0xd0 = 1 and calls 0x691950(goal) → 0x6110e0: goal required (+0x28) −= cost, allocated (+0x2c)
  += cost. **No amount is stored on the task.** The cost is recomputed from the type each time it is needed.
- **Charged branches of 0x648980:**
  1. PlanDef +0xa8 (MagicPlan) — but magic tasks have no BT +0xc8, so no charge [M];
  2. orbital / ground base or structure types (0x5C / 0x5B / 0x60 / 0x5F);
  3. type +0x894 != 0;
  4. PlanDef +0xd0 (AllowFreeStoreUnits) == 0;
  5. 0x4d2bd0 found no free-store entry to claim.

  A claiming task stays uncharged.
- **Spend s.** 0x64abf0, the market binding a production object to the task:
  - stores the producer at BT +0x70 and subscribes to its signals 2 / 4 / 3;
  - then calls 0x693170(goal, cost(T, 0)) → 0x614310, which makes s (+0x40) += cost.

  This happens at production **start**, unconditionally.
- **Reversal (new).** 0x647df0 gives a free-store unit to the task (called from the free-store service 0x4d5a70):
  1. it sets BT +0x68 = unit and drops the AIPlanetBuildTask (+0xc8);
  2. **if BT +0xd0 is set**, it calls 0x6931e0(goal, cost(T, 0)) → 0x614840: allocated −= min(cost, allocated);
     required is not given back;
  3. it sets the next state (0x16 in galactic, 3 otherwise).

  Because claiming tasks are uncharged, this path is normally not reached [M].
- **Other readers of +0xd0:** 0x648980 / 0x64ab50 (no double charge), 0x647df0 (reversal) and save/load 0x6492f0
  (chunk 0x16). `Flush_All_Resources` 0x6bcdf0 clears goal amounts wholesale (alloc −= min, req = 0).

**Port consequence:** none for the main path. `paid()` is gone from PRODUCTION_END (`galactic_ai.cpp` l.1413). The
charge lives in `create_build_tasks` / `resolve_claim` (`galactic_build.cpp` l.75-84, l.178-181) as a `charged` flag.
Optional, for safety: if any port path ever hands a free-store unit to a task with `charged == true`, it must release
the allocation, alloc −= min(cost, alloc), and leave required alone (0x647df0).

---

### 6. Land_Units (0x6cec80) at an enemy planet whose surface is accessible

#### 6.1 Checks in order [H, re-read]
1. **Fleet and units.** TF +0x120 (the elected fleet) must resolve (0x5791e0) to an object without the "being
   removed" bit (+0x3a0 & 2), and the TF unit list (+0x78..+0x80) must be non-empty. Otherwise: warning 0x248620
   "GalacticTaskForceClass::Lua_Land_Units -- No units in TaskForce %s, or no FleetObject", and the function returns
   0 values.
2. **Planet.** X = 0x432c40 = fleet pack +0x40, the orbit planet. There is no null check.
3. **Status.** It creates a status 0x6cd400(0x80c) and calls 0x72c090(status, TF, fleet, 1).
4. **Surface.** 0x375220(X type, X) false → the status is returned.
5. **Surface count.** 0x3f9530(X) counts the entries of PD +0xe0 that are not being removed (any owner). If it is
   **==** `Max_Ground_Forces_On_Planet` (DAT_140b168c8), the status is returned.
6. **Walk.** The fleet contents are walked from the last index down. Before each unit the surface count is recounted,
   and the walk breaks when it equals the maximum. A unit U is handled only when both hold:
   - 0x398670(U, X) (U has a ready `Ground_Activated` ability that accepts X) **or** U has TRANSPORT (+0x337 != −1);
   - U's parent is not X.

   How U is handled:
   - **U not a transport:** 0x3a00b0(U, X) fires its ability. No landing.
   - **U a transport and 0x39a2c0(X, plan player, 1) true:** the planet's owner (PD +0x74) is the player or an ally
     (relation 0 in player +0x370; no owner → false). Then:
     1. 0x72bfe0(status, U, 1);
     2. FleetManagement op 2 (planet X +0x50, fleet, transport) is executed (0x5af260);
     3. 0x647b90, then op 5 (land) is executed.

     This is the only landing.
   - **U a transport and the planet not friendly:** the function walks U's cargo (0x397bf0). It fires 0x3a00b0 for
     the **first** cargo unit with 0x398670 true and stops that cargo walk. Then it breaks the fleet walk if TF +0x120
     became null.
7. **Return.** The status is returned through 0x256d40.

#### 6.2 Answer
At an **enemy** planet with an accessible surface, `Land_Units` does not land any unit, does not split the fleet,
starts no invasion and no land battle [H]. It does not warn and does not return false. It returns the normal status
object, and its only effect is firing `Ground_Activated` abilities (slicers, saboteurs and similar) [H]. The same holds
for neutral and unowned planets (0x39a2c0 is false without an owner) [H]. Invasions go through `Invade` 0x6ce600, not
`Land_Units` [M].

**Port consequence:** none in the logic. `AiTfOrder::LandUnits` (`galactic_ai.cpp` l.1756-1818) follows the order
above. Two small differences, both [port] and both noted there:
- the port adds 1 to `count` per land order instead of recounting the surface (the port's land order may apply later);
- it reads `planets()[x].owner` for PD +0x74. PD +0x74 is the planet owner as the other RE docs read it
  (`GALACTIC_REVEAL_CORRUPTION` baseline table, `A2M_R2_RE_A` §1), so this matches [M].

Keep the fleet-not-in-orbit guard (retail would dereference null).

---

### Corrections to earlier docs
| Doc | Point | Correction |
|---|---|---|
| A2M_T04_RE_ANSWERS Q1.2 / A1_READINGS §28 | 0x39ad40 third clause | the unit's **parent** has TEAM and a locomotor answering vf+0x230 and vf+0x1d0 (a team member whose container is locked), not "the unit is a land team container" [H] |
| A2M_T04_RE_ANSWERS Q1.2 | 0x42e3b0 / 0x42e3d0 | they read the unit's own pad pack +0x150: +0x10 built structure, +8 under construction; the behaviour argument is unused [H] |
| A2M_T04_RE_ANSWERS Q1.1 step 4.6 | distance term "for every stage" | it is added only when both the stage's and the unit's reach objects are non-null (same guard as the cost) [H] |
| A2M_T04_RE_ANSWERS Q2.2 step 1 | 0x664210 level ≥ 6 | returns 1e18 at once; no list is walked [H] |
| A2M_T04_RE_ANSWERS Q3 | "no shipped galactic caller of 7-9 [M]" | upgraded to [H] for goal data: galactic goal XML uses levels 0 / 2 / 3 / 4 / 5 only |
| A2M_T04_RE_ANSWERS Q5.2 | refund | 0x647df0 reverses a charge (alloc −= min(cost, alloc), required kept) when a free-store unit fills a charged task [H] |

### Open items (unchanged from A2M_T04, still open)
- Position of a unit inside a moving fleet (stale or moved) [M/L]: probe.
- No galactic writer of +0x3a7 / +0x3a8 / +0x210 [M]: write breakpoints with story scripts running.
- No reader of TF +0xa8 [M]: read breakpoint during a galactic build plan.

---

## Sections 9-10. b0125 (target political control) and b0121 (AI builds under a hostile fleet; release) (bkg2, 2026-11-12)
Date: 2026-11-12. Source: Ghidra decompile of Steam FoC x64 (`Global.c`), plus llvm-objdump of the retail exe for two
call sites. Shipped FoC XML from `extracted/foc_data_text.zip`. Behaviour only, no code copied. Addresses are RVAs. D = the
planet data pack (planet object +0xB8). Tags: [H] read directly, [M] one inferred link, [L] hypothesis.

### Short answers

- **b0125.** The target is not "1 + route gains". 0x3f6af0 / 0x3f6df0 keep a per-planet breakdown map (D +0xA0). Each
  key (a group) holds entries of the form (value, expiry frame, tooltip, source id). The target is built in three steps:
  - take the maximum value within each group;
  - sum those maxima over the groups;
  - clamp the sum to [min, max], then to [0, 5]. min = 1 when the planet has an owner from the physical sources,
    otherwise 0. max = `Planet_Max_Political_Control_For_Faction` for the owner's faction, default 5. [H]
  
  The groups:
  - leaning;
  - one "presence" group whose entries are the ground-base level, ground troops (0/1), the starbase level, the orbital
    claim (0/1), capital ships (0/1) and residual influence (0/1);
  - enemy fleets (−1);
  - up to 8 structure groups (Political_Control of each structure that has one);
  - trade routes. [H]
  
  With vanilla data, every `Political_Control` is 0, routes give 0 and no faction maximum is listed. So the target is
  **max(ground-base level, starbase level, 1 if any visible ground unit, …)**, with a floor of 1 for an owned planet. In
  Heart of the Maw this gives 3 at Kuat / Mon Calamari / The Maw, 2 at Corellia / Tatooine / Naboo / Nal Hutta /
  Felucia / Shola, and 1 at Kashyyyk / Kamino / Yavin. [H rule, M per-planet result]
  
  On the first service, control is set to float(target) directly. The vanilla income curve makes control ≥ 1 income
  neutral. The AI token `TargetPoliticalControl` (control / 5) is the reader for which 1 / 2 / 3 matters. [H]
- **b0121 (a).** A hostile fleet arriving does **not** fail, cancel or pause a unit that is already queued or in
  production. [H]
  - Only a **starbase** head (flag 0x5C) is cancelled by an orbiting fleet: no refund, planet signal 3. The AI build
    task hears signal 3 and goes to PRODUCTION_FAILED, which dispatches Production_Failed. [H]
  - The blockade test 0x3fa370 is applied only after the unit exists: a produced landed ground unit with a destination
    off the build planet fails the task (GROUND_UNIT_PRODUCTION 0x648120). [H]
- **b0121 (b).** On a goal release the build is handled by what A2M_R2_RE_B §1 describes, confirmed here:
  - a not-yet-producing task is only a bid, so nothing is cancelled and no money moves on the planet;
  - a producing task that the PAYG reclaim took (+0xD9) finishes to the free store;
  - any other producing task of a PAYG owner is cancelled at the next service with the paid-fraction refund (notify
    1).
  
  No Production_Failed event fires on these paths, because the task's signal bindings were already removed. [H / M]

---

### b0125: target political control

#### 1. Owner and breakdown, 0x3f3830(planet behaviour, planet, D, out owner)

The breakdown map at D +0xA0 is keyed by an int group id; each node holds a vector of 0x30-byte entries:
- +0 value (int);
- +4 expiry frame, −1 = never;
- +8 tooltip id;
- +0x10 source id (the behaviour plus a source kind).

An existing entry with the same source id is updated in place; a new one is appended. [H]

Group 1, written in this order. The owner is the first non-empty result. [H]
1. GROUND_BASE: value = ground-base level D +0x130 when it is > 0, else 0. The owner is D +0x124 when the level is > 0.
   - The level is the count of D +0x158 ground structures whose object flags +0x3A0 & 0x42 are clear, clamped to
     1..5 (0x4bdaa0). [H]
2. GROUND_TROOPS: value = 1 if any non-hidden (+0x3A0 bit 2 clear) object is in the surface list (D +0xE0, count
   +0xE8), else 0. The owner is the first such object's owner. [H]
3. STARBASE: value = starbase level D +0x148 (0 if none). The owner is D +0x13C when the level is > 0. [H]
4. With no owner yet: the owner of the head entry of production queue 0, then of queue 1 (0x401960, entry +0x20). This
   step writes no entry. [H]
5. ORBITAL_CONTROL: value = 1 when the owner is still unknown, the surface test 0x375220 fails (inaccessible or
   destroyed) and an orbiting, non-stealthed (0x3a9760) object with a live player claims the planet; else 0. [H]
6. Fleet sum over every orbit object (D +0xC8, skipping +0x3A0 bit 2): for each contained unit (0x3965f0, recursive),
   add its type's `Political_Control` (type +0x79C, via 0x3734d0 with no object) when the unit's owner is the derived
   owner, and subtract it otherwise. Let s be the sum. [H]
   - s ≥ 0: CAPITAL_SHIPS = min(s, 1) in group 1, and the group-2 entry is removed.
   - s ≤ −1: ENEMY_FLEETS = −1 in **group 2**, and the group-1 capital entry is removed (0x3fda70). [H]
7. RESIDUAL_INFLUENCE: value 0. It becomes 1 and the owner is kept as the previous applied owner (D +0x6C) when all of
   these hold: no owner yet, the planet is not destroyed (D +0x2C8 = 0), the planet type has
   `Planet_Retains_Residual_Influence` (+0xBA; vanilla only Alderaan, The_Maw and VergessoAsteroids), and D +0x6C ≠ −1. [H]

All of these entries have expiry −1. The GROUND_BASE and ORBITAL_CONTROL entries take theirs from 0x35cd20(−1.0),
which also gives −1. [H]

#### 2. 0x3f6af0: purge, clamp bounds, leaning, structures, reduction

1. Every entry whose expiry (as unsigned) is < the current frame is removed first. Entries with −1 stay. [H]
2. 0x3f3830 runs (§1).
3. Bounds (stored in the map header: +0x10 min, +0x14 max, flags +0x18 / +0x19):
   - no owner: min = 0, max = 5;
   - owner found: min = 1; when that player and its faction exist, max = 0x3733f0(planet type, the owner's faction). That
     function searches the `Planet_Max_Political_Control_For_Faction` pairs (type +0x830, list code 0x3c) by faction
     name, case-insensitive, and returns 5 when the faction is absent. A result < min is replaced by 5. [H; M that the
     second argument is the faction's name string]
4. POLITICAL_LEANING, 0x3f5710, **group 0**. v = the planet type's `Political_Control` (+0x79C), clamped to 0..5; the
   leaning faction is `Political_Faction` (+0x7C0).
   - No owner and v > 0: the owner becomes that faction's player.
   - Owner known and the owner's faction ≠ the leaning faction: the value is −v.
   - Expiry −1. [H]
5. Still no owner: the first `Is_Neutral` faction's player (0x331cc0 → 0x294d30). [H]
6. Structures, 0x3f5980. Each ground structure (D +0x158) and then each orbital structure (D +0x170) whose type has
   `Political_Control` ≠ 0 gets **its own group** (ids 5, 6, … up to 12; the walk stops after the 8th).
   - The value is that `Political_Control`, unsigned, with no owner test.
   - The expiry is 0, so the entry is purged and re-added every call. [H]
7. Reduction 0x3f7130 with GreaterThan / Plus:
   - per group, the maximum entry value;
   - sum over the groups in key order (int);
   - then the min bound if its flag is set, then the max bound if its flag is set. [H; M that GreaterThan picks the
     larger value]

#### 3. 0x3f6df0: trade routes and final clamp

- TRADE_ROUTES, **group 13** (expiry −1): the value is the sum of `Political_Control_Gain` (route +0x48) over P's links
  (D +0x58, stride 0x20; the route is at +8, the neighbour at +0x18). A link counts when all of these hold:
  - the neighbour's control record passes 0x4ad9a0: the neighbour's owner is self or ally of the derived owner
    (0x2823c0), and its control is > 0;
  - P's control ≥ 1;
  - the neighbour's control ≥ 1.
  
  [H; M "self or ally"]
- The reduction (§2.7) runs again, then the result is clamped to 0..5 (int). The derived owner is returned. [H]

#### 4. Applying it, the first value, and the readers

- 0x3fe1b0 passes the int target as **float(target)** in xmm3 to 0x3fb040 (`movd eax→xmm3; cvtdq2ps` at 0x3fe529). [H]
- First call (D +0x1C9 = 0): control D +0x68 = float(target) and owner D +0x6C = the derived owner, with no ramp. So
  Kuat starts at 3.0. [H]
- Afterwards, control ramps toward the target at Δ = 1 / (30 · m · 7 s) per frame, both up and down (GE §4.2). After a
  capture it ramps 0 → target, so reaching 3 takes about 3 × 7 s × m. [H, from GE §4.2]
- Readers:
  - Income (GE §1.4): term = (q − 1) · base, with q = `Political_Income_Curve`(control). The FoC GameConstants curve is
    "0,0, 1,1, 5,1" ("Disabling increased income per base level"), so any control ≥ 1 gives a term of 0. Only the
    0 < c < 1 ramp after a capture costs income. [H data, H formula]
    - `HYPERGAMECONSTANTS.XML` has "0,0, 1,1, 5,3". It is not the normal constants file. [M]
  - AI token `TargetPoliticalControl` = (int)|control| / `MaximumPoliticalControl` (5), negated for a non-Self owner
    (AI_GALACTIC_TOKENS 0x65f000). This is where 1 / 2 / 3 changes AI evaluation. [H, from that doc]
  - The control record's "held by self or ally with control > 0" test (0x4ad9a0 / 0x4ad9f0): visibility and the route
    gain above. [H]
  - The ≥ 1 → < 1 drop calls 0x403490 for both queues (GE §4.2). [H call]
  - The Political_Control_Breakdown UI (visibility 0x800) lists these tooltip entries. [M]

#### 5. Vanilla data and Heart of the Maw

- Every `<Political_Control>` in FoC XML is 0 (237 hits; 7 commented-out 1s).
- 10 planets have `Political_Faction` Empire and 4 have Rebel, so the leaning value is 0 everywhere.
- All 329 `Political_Control_Gain` values are 0.
- `Planet_Max_Political_Control_For_Faction` never appears.

So the vanilla target is: owned planet → max(1, ground-base level, starbase level, troops 1); neutral with visible
indigenous units → 1; empty neutral → 0. [H data, H rule]

`Sandbox_Heart_Of_The_Maw_Underworld` starting forces (starbase level / ground structures → target):

| Planets | Starbase / ground structures | Target |
|---|---|---|
| Kuat | SB3 / 3 | 3 |
| Mon Calamari | SB3 / 2 | 3 |
| The_Maw | SB3 / 0 | 3 |
| Corellia, Tatooine, Nal Hutta, Felucia, Shola | various / 2 | 2 |
| Naboo | SB2 / 1 | 2 |
| Kashyyyk, Kamino | SB1 / 1 | 1 |
| Yavin | SB1 / 1 | 1 |

[M: assumes the starting structures are in D +0x158 and that D +0x148 = the starbase type's Base_Level.] The Rebel and
Empire HotM variants (`CAMPAIGNS_UNDERWORLD_GC.XML` lines 4759 / 5020) need the same count.

Side finding (owner rule, tier 1). An empty planet without `Planet_Retains_Residual_Influence` reverts to the neutral
player: no units, no structures, no starbase, no production. The port's `derive_owner` step 6 instead uses a leaning
that follows every capture ([L] in the port). Retail uses the leaning only when the type's `Political_Control` > 0. [H]

**Port consequence:**
- `engine/galactic/src/campaign.cpp` `apply_ownership`: compute the target as follows.
  - g1 = max(ground-base level (live structure count, clamped 5), starbase level, any visible surface unit ? 1 : 0,
    orbital claim ? 1 : 0, min(max(fleet PC sum, 0), 1), residual ? 1 : 0).
  - g2 = −1 if the fleet PC sum ≤ −1, else nothing.
  - Add the leaning ±v, one term per structure with PC ≠ 0 (at most 8), and the route gains: both controls ≥ 1, the
    neighbour owned by self or an ally with control > 0.
  - Clamp to [owned ? 1 : 0, faction max or 5], then to [0, 5].
  - The first call keeps `control = target`.
- `derive_owner`: put residual influence (type flag, not destroyed, previous owner) and the leaning (only when PC > 0)
  in retail's order, then the neutral player.
- Rules data: planet PC +0x79C, Political_Faction +0x7C0, Retains_Residual_Influence +0xBA, the +0x830 per-faction
  maximum list, and unit / structure PC.

---

### b0121: AI builds under a hostile fleet; builds of a released goal

#### 6. (a) Planet queue service 0x402840 and the AI build task

- The service first runs the AI queue service 0x53cd60 for queues 0 and 1. Then, per queue, only the head is checked.
  [H]
  1. A starbase-family head (flags 0x5E / 0x60 / 4 / 0x61) while D +0x13C ≠ the entry's player is dropped, with no
     refund.
  2. **A head with flag 0x5C (DUMMY_STAR_BASE) is cancelled** when the planet is not in an active battle (0x2b0470)
     and some orbit object has all of these:
     - fleet data (+0xC0) present;
     - not being removed;
     - **owner id ≠ the entry's player** (a plain compare, no ally exemption; GE §2.5 says "or an ally", which the code
       does not show);
     - not stealthed;
     - fleet data byte +0x94 = 1.
     
     The cancel logs "...starbase in production and enemy fleet in orbit - Production cancelled", raises **planet
     signal 3** with the entry, removes it with **no refund** (0x1400475e0, then 0x4033c0 starts the next), shows
     TEXT_TOOLTIP_INTERRUPTED_PRODUCTION to the local owner, and returns for this frame. [H; L that +0x94 means
     "armed / hostile-capable"]
- Rule 2 and the starbase step of buildability (STRATEGIC_GAPS §1.3 step 20) are the only orbit tests on the
  production path. Every other type keeps counting down or paying installments while a hostile fleet sits in orbit.
  There is no pause: the only PAYG pause is a refused installment (0x53b900). [H]
- The AI task while queued or producing (FSM 0x64aec0, PRODUCTION_QUEUED 2 / PRODUCTION_BEGIN 5) polls only these
  [H]:
  - player-level buildability 0x2ac320;
  - ground / orbital structure slot counts for 0x5F / 0x60 types.
  
  It has no blockade test.
- The task's signal binding (production start 0x64abf0 binds planet signals 2, 4, 3; handler 0x649be0) [H]:
  - 2 (started) → PRODUCTION_BEGIN;
  - 4 (finished) → claim the object (0x6498f0);
  - **3 (removed unfinished) → unbind, then state 7 PRODUCTION_FAILED → Production_Failed(tf, type) (0x5fdd90)**.
    
    The type (+0xEC), the player and the entry id must match. So the starbase cancel above, and any UI-style cancel of
    a live task's entry, produce Production_Failed (the port's plan event 4). [H]
- The AI planet queue's own listener 0x53c4f0 on signal 3 / 4 only hands a produced object of a gone task to the free
  store and then clears Q +0x18 / +0x20. It sends nothing to the task. [H]
- **Where 0x3fa370 is used.** After production, WAIT_FOR_FREESTORE_ALLOC (0x16) sends a landed ground unit (with a
  destination) to GROUND_UNIT_PRODUCTION (4, 0x648120). [H]
  - unit's parent = the destination → PRODUCTION_END (6);
  - else the build planet is blockaded (0x3fa370: the first visible orbit entry is not self or ally of the planet
    owner) → **PRODUCTION_FAILED** (7);
  - else it launches (FleetManagement op 5 / 0).
  
  The money is already spent. The task end (0x64ad00) re-admits the unit to the free store unless +0xD8. [H path; M
  re-admit on this path]
- Other blockade users: HIJACK_UNIT 0x648370 (magic), FreeStore `Move_Object` 0x68f5b0, TaskForce `Launch_Units`
  0x6cf070. [H]

**Port consequence:**
- `engine/ai/src/galactic_build.cpp`: add the 0x648120 rule to the produced-unit hand-over. A produced ground unit on
  the surface, with a destination ≠ its build planet, and `sim.ai_blockaded(build planet)` → `fail_build(b,
  "blockaded")` (Production_Failed), with the unit back to the free store. Today only the magic spawn checks.
- Keep "production dropped → fail_build" for the starbase cancel: it matches signal 3 → state 7.
- `engine/galactic/src/campaign.cpp` `enemy_fleet_in_orbit` (used by service_production rule 2): retail tests owner ≠
  entry player (allies included), not stealthed, not removed, and fleet flag +0x94. It has no `ai_combat_power > 0`
  test. Aligning it needs the meaning of +0x94 [L]; a Q2 probe can settle it (an allied fleet over an AI starbase
  build).

#### 7. (b) Release of a goal (0x614880 → reclaim 0x5f7d40, then plan end 0x5f6a50)

- An AI task is in the planet queue only from production start onward. 0x53cd60 starts a new task only when Q +0x20 = −1,
  so each AI planet queue has at most one entry in flight. "Queued, not producing" is a bid in Q +0x10 with no planet
  entry and no planet money. [H for the −1 gate; M "at most one"]
- 0x64ad00, the end of every task, does the following [H]:
  - sets +0xA8 and state 0x15;
  - frees the bid record (0x695560 on +0xC8), which removes the pending bid;
  - unregisters all of the task's signal bindings (0x220ef0 on task +0x40);
  - re-admits the claimed unit.
  
  The goal's money comes back through the release itself (A += c + a, A2M_R2_RE_B §1.3). [H]
- Producing task (A2M_R2_RE_B §1.1–1.5, re-read) [H]:
  - **reclaimed** (+0xD9 set by 0x5f7d40: started, remaining cost in (0, amount], oldest first, greedy) → production
    continues and the object goes to the free store;
  - **not reclaimed, PAYG owner** → at the planet's next production service, 0x53ba60 cancels the head through 0x403900
    with refund = clamp(1 − remaining/T, 0, 1) × cost (the paid fraction), Add_Credits notify 1 (`Credit_Multiplier`
    again, budget P += x′·w);
  - not PAYG (human) → production continues to the free store.
  
  0x403900 also raises signal 3. The ended task is no longer bound, so there is **no Production_Failed** and no plan
  event on a release. [H unbind; M that 0x220ef0 removes every binding of that listener]
- A goal that is replaced (dropped at the resolve 0x6c8970) takes the same release, so the same rule applies. A plan that
  ends by its own script does no reclaim, so every producing build of a PAYG owner is cancelled with the refund. [H]

**Port consequence:**
- `end_plan_builds` / `cancel_ended_production` already follow this. Check two things:
  - the reclaim runs only on the goal-system path (A2M_R2_RE_B §1.6 item 1);
  - no `ProductionFailed` tf_event is emitted on release or cancel.
- The open part of A2M_R2_RE_B §1.6 item 3 still holds: retail 0x6d0370 (task force emptied) also ends producing tasks.
