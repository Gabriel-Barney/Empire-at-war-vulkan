# q2f t09: Rebel galactic production in Heart of the Maw (2026-11-02)

Why the port's Rebel AI places about half of retail's production orders in the HotM Lua trace
(`20261030-061228-galactic-vanilla-lua`, compared with `q2trace.py compare --port-exe`). Behaviour is in
AI_GALACTIC.md (Commit) and AI_GALACTIC_2.md (Contrast); this page keeps the diagnosis, the numbers and what is
still open. Confidence tags as in the other `docs/re` pages.

## Findings fixed in the port

**F1. Goal-set extension size comes from the AIPlayerType, default 2** [H]. Commit / Resolve_Category (0x721b60)
truncates a category's proposals to `extension + |active goals|`; the greedy and the random phase use the same limit.
`extension` is read through goal system +0x80 → +0x28 → +0x28 → +0x30 → **+0xE8 of the AIPlayerType** (XML tag
`GoalSetExtensionSize`; ctor 0x60f400 sets 2, parser 0x60f840 does `atoi` and an unsigned `min(v, 5)`). No vanilla
XML sets the tag, so every galactic AI uses 2. The port used 1: it had read +0xE8 as a field of the AI template, whose
ctor (0x6709a0) puts an unordered_map's mask (7) at that offset. With 1, the Rebel kept only one proposal beyond its
active goals per category, so Build_Ground_Forces / Conquer_Pirate proposals were cut before the budget saw them.

**F2. Contrast counts a pair only when it can still add something** [H]. Per-entry contrast 0x72b6a0 (called by
0x72b940): with `P = power × health`, nothing counts when `P ≤ 0`; a (unit, entry) pair counts only when its weight
`w > 0` **and** the entry's remaining requirement `R > 0`, and only then is the "tactical or free unit, else target ×
tolerance < R and the pop cap allows" test applied. The port ran the tolerance / pop test first and let a pair with
`R = 0` reach the score, so a filled entry still scored `s = (1 − (d/m)²)·w` with `d = wP, m = wP` → 0 and could veto
the plan. That was the "Conquer_Pirate fails on Contrast" item. The contained-units variant 0x72b3e0 has the same rule;
the port does not model contained units yet (open item O3).

## Findings that are retail behaviour (no change)

- **No Defensive goals for the Rebel.** BasicDefensiveGalacticSet has no defend goal; planets are defended by the
  GalacticFreeStore Lua. The port matches.
- **`Game.EnemyForce.SpaceTotalUnnormalized` drops the caller's parameters** (AI_GALACTIC_TOKENS, 0x6e3a10), so
  `Enemy_Global_Space_Unit_Raw_Total` = X − X = 0 and `Has_Enough_Space_Force_Globally` = 1 in both games. With it,
  BasicRebelOffensiveBudgetAllocation = 7.2 + PlanetsControlled + 6·(1 − 1) = 7.533 at t1 and Infrastructure = 19.9,
  so category 0 gets 2196.8 of the 8000 starting credits.
- **The budget income rate is 0 until the first fiscal tick** (1351 = 45 s × 30 frames) in both games.

## Retail vs port, early Rebel game

Retail queue (production.q2q of the trace; Naboo costs are 75 % of list price under Mon Mothma's
Reduce_Production_Price 0.25, Kamino infantry costs 1):

| frame | planet | item | cost |
|---|---|---|---|
| 89 / 98 | Mon Calamari / Kamino | mining facility | |
| 109 | Naboo | Corellian gunboat | 1013 |
| 140 | Yavin | light tank | |
| 156 | Naboo | tank buster | 56 |
| 559 | Naboo | infantry | 39 |
| 807 | Naboo | mining facility | 338 |
| 1225 | Yavin | turbolasers | |

That is Build_Ground_Forces' 3-unit variant (627 net). The light vehicle factory comes at frame 1800.

Port (main and after F1/F2): Build_Space_Forces (an IgnoreTarget plan, scored by an RNG float draw in
`[0, power/500]`) picks a Marauder (1800) instead of a gunboat (1350 at list price), which leaves category 0 at 396.8,
so Build_Ground_Forces (1127, or 627) is vetoed by the budget. The port also spends 4000 on Yavin turbolasers at
frame 155 (retail 1225). At frame 1351 retail's Rebel holds 4080 credits, the port's 1342. Which unit wins depends on
the RNG stream, and the port already draws 84 values at tick 1 where retail draws 130 (q2f t03).

## Measurements (HotM Lua trace, 7200 frames)

| | retail | main (before) | this change |
|---|---|---|---|
| Rebel fleet departures | 71 | 47 | 33 |
| Underworld fleet departures | 56 | 24 | 44 |
| Rebel production orders | 50 | 25 | 12 |
| Underworld production orders | 24 | 23 | 20 |
| owner changes | 1 (t7498 Shola UW→Rebel) | 2 (Yavin lost t978) | 1 (t2121 Shola UW→Rebel) |
| final credits Rebel / Underworld | 812 / 2500 | 1448 / 4640 | 1940 / 2689 |
| RNG draws at tick 1 | 130 | 84 | 84 |

Split: F1 alone gives 12 Rebel orders and still loses Yavin (t753); F2 alone (extension 1) gives 9 orders, no owner
change and 6614 Underworld credits. Both together keep Yavin and bring the Underworld's credits close to retail.
The Rebel order count drops, most likely because the wider proposal set lets Build_Space_Forces' Marauder and the
turbolasers take category 0 earlier (see above; not traced order by order). It should only recover once the unit
choice matches (O1).

HotM with a human Rebel (`--human Rebel --human-script galactic_human_hotm.txt`, 72000 frames): main has 2 owner
changes (the Underworld's Conquer_Opponent takes the human's Yavin at t36762); this change has 1 (Felucia 5→1 at
t699) and no human planet is taken. **a2f t05 stays open.**

## Open items

- **O1.** Build_Space_Forces' unit choice (Marauder vs gunboat) and the 4000-credit turbolasers at frame 155. Needs
  the plan probe below, then a look at the IgnoreTarget score and the RNG stream (t03).
- **O2.** Whether the port applies Mon Mothma's Reduce_Production_Price and the Kamino infantry bonus to AI costs is
  not checked.
- **O3.** Contained-units contrast pass (0x72b3e0) is not modelled.
- **O4.** The tactical AI still uses an extension size of 1 (`engine/ai/tactical`, other owner); its goldens move
  with 2.
- **O5.** The human Empire's final credits in the HotM trace are now 19465 vs retail 19675 (equal before).

## Retail probe runs

**2026-11-03 (`20261103-152619-galactic-vanilla-lua`): not a valid reference for production.** The plan hooks of
that kit lost their records table to PGBase `Flush_G` at the first plan release, and the next record's Lua error
killed the plan threads (LUA_SCRIPT_OUTPUT §4). Retail's own AI built less: Rebel departures 19 and 32 queue starts
(15 Y-wings), against 71 / 50 in the run without plan hooks. The retail queue above (2026-10-30 run) still stands;
the 11-03 queue and departure numbers do not. The kit was fixed in q2f t08 (2026-11-08).

## Retail probe (fixed 2026-11-08, q2f t08)

`tools/probe_mod/q2_traces`, `run_lua_probe.cmd` (README "Lua probe run"): the mod now also carries unchanged copies
of `BuildSpaceForcesPlan.lua`, `BuildGroundForcesPlan.lua` and `ConquerPiratePlan.lua` with a read-only block that
records `PSTART` / `PBLOCK` / `PDONE` / `PEXIT` (time, frame, plan, faction, target, the goal's budget, credits,
task-force unit types) and Conquer_Pirate's `PFAIL`. It must show:

1. the Rebel's first Build_Space_Forces `PBLOCK` and its unit types (port: Marauder; retail's queue suggests a
   Corellian gunboat at frame 109);
2. the first Build_Ground_Forces `PSTART` / `PBLOCK` and units (retail queue: light tank, tank buster, infantry by
   frame 559);
3. every Conquer_Pirate start and production failure, with target, for both AI players;
4. that the plans run at all under the mod (`lua.plans` verdict `loaded: N plan scripts` with N ≥ 1).

## 2026-11-08 rerun (`20261108-170824-galactic-vanilla-lua`, q2f t10) against the port at dev 03940b96

The fixed kit ran clean: no seq gaps, no probe errors, 10 galactic cycles with 240 perception rows, and retail's own
AI undisturbed (the run is a q4h `hotm_galactic` reference). The plan records it kept are thin: 12 `PSTART`, 11
`PBLOCK` / `PDONE`, all before about t=420 s, `units=0` in every `PBLOCK` (the task force was already released into
the queue when `BlockOnCommand` returned), and one Rebel `Conquer_Pirate` `PSTART` (t=184, target not readable) with no
end. They do not name the plan behind later production; the production queue (`production.q2q`) does.

Rebel production after frame 3000, retail vs port (port: `alamo_headless --ai-log`, seed of the q4h scenario):

| Window | Retail | Port |
|---|---|---|
| Shola attack | 3 Y-wings at MonCalimari, starts 5565 / 5968 / 6371; Shola taken at 7498 | 9 Y-wings at MonCalimari, 4190 .. 7631 (same ~403-frame cadence, Conquer_Pirate @Shola from 4135 / 5550); Shola taken at 8214 |
| After Shola | 18 infantry squads + 5 tank busters, 9549 .. 12157, built in parallel at Naboo, MonCalimari and Kamino (Kamino cost 1) | 9 tank busters at Kamino one after another (9302 .. 12991, ~465 frames apart), 2 light tanks at Naboo; 1 infantry squad |

What this means for the failing metrics:

1. **Y-wing count and Shola timing.** The port's Conquer_Pirate @Shola starts producing ~1400 frames earlier and keeps
   asking for Y-wings until its force is large enough (9), where retail's goes with 3. The port's "force sufficient"
   test (enemy contrast vs task-force power) asks for more than retail's for the same Shola defenders. That is what
   delays the port's Shola capture (8214 vs 7498) and its Shola / Naboo shuttling, which is most of the
   `fleet_departures` (30 vs 71) and `fleet_targets` gap.
2. **Infantry vs tank busters after Shola.** The port's Conquer_Opponent ground force (@Tatooine from 9253, GroundForce
   14+10) picks tank busters where retail picks infantry squads, and the port builds one unit at a time at one planet
   where retail queues at three planets at once. That is the `build_mix` gap (22 infantry vs 2).

Next steps (engine/ai/galactic, each a behaviour change with golden regeneration; roadmap q2f t09):
the task-force sufficiency test against retail's (docs/re/AI_GALACTIC, "Task force"), the ground unit pick for
Conquer_Opponent against Tatooine's defenders (contrast / IgnoreTarget scores), and per-planet parallel production.

The table above is the port at dev `03940b96`. At dev `432b502e` (after a2l t01-t03) the harness reads Rebel builds
19 vs retail 50..51 and fleet departures 16 vs 55..63 (3 retail runs): the a2l changes cut Rebel production further,
so measure again at the current dev before working on the items above.

## 2026-11-08 diagnosis, second pass (RE answers `Q2F_T09_RE_ANSWERS.md`)

- **Retail's Shola attack is most likely `Conquer_To_Reconnect`, not Conquer_Pirate [M].** The trace's hops: wave 1 =
  17 one-unit fleets from Kamino, Yavin, Naboo and MonCalimari flying straight into Shola, departures staggered so all
  arrive on tick 7161; wave 2 = 11 transports arriving 7465-7468; capture 7498; the arriving fleets are merged (7175,
  7478). That is `SynchronizedAssemble` at stage = target, which only ContrastToReconnect (level 4) and LiftBlockadePlan
  call. A Conquer_Pirate gather (level 2) stages at a held neighbour and cannot end on an enemy planet in mode 0.
- **Why it starts at ~6941 in retail:** `Need_To_Reconnect_Islands` = ConnectsLargestIslands x First_Attack_Allowed x
  (...). The port computes the same two Rebel islands ({Naboo}, {Kamino, MonCalimari, Yavin}) with Shola on the path;
  `First_Attack_Allowed` = Game.Age > `Initial_Grace_Period` = 200 + 7 x planets (Normal) ~ 230 s = tick ~6900.
- **Why the port never gets there:** its Conquer_Pirate @Shola is instantiated at tick 2798 (retail's PSTART: ~5518,
  same net cost 1470 = 7 Y-wings) and takes Shola at 5970, before the grace period ends. Retail's Conquer_Pirate had
  built only 3 of its 7 Y-wings when the reconnect wave took Shola. So the open difference is **when Conquer_Pirate
  @Shola is proposed / funded** (`Should_Conquer_Pirate_Planet` = `Is_Good_Invasion_Target`; many terms), not the
  gather. The extended probe kit (GOALPERC rows for Shola / Naboo, hooks on ContrastToReconnect, LiftBlockadePlan,
  ConquerOpponentPlan, CrushPlan) gives the retail values.
- **The synchronized gather is now ported** (docs/PHASE4.md, 2026-11-08 q2f t09); it does not change HotM seed 0 yet.
- **The Rebel's 40 -> 19 build drop at dev 432b502e is seed noise**, not an a2l regression: over seeds 0-7 the mean
  is 29.1 builds at 03940b96 and 27.0 at 432b502e. Seed 0 diverges at tick 348 through a2l t03 Q4's in-transit
  candidate rule (an Underworld saboteur plan), and the run drifts from there. Parity checks of AI counts should
  average several seeds.
- After Shola the port's Rebel builds almost nothing: Conquer_Opponent @Tatooine fails its solve on the Offensive
  budget (about 1174 available vs 1950-3360 needed), and CrushPlan exits at once on Normal by design.

## 2026-11-08 t11 retail run (`20261108-183748-galactic-vanilla-lua`, extended probe kit)

Clean run: 10 cycles, 30 GOALPERC rows, 16 plan scripts loaded, no probe errors, 72 Rebel fleet departures.

- **Rebel goal values for Shola** (`Should_Conquer_Pirate_Planet` = `Is_Good_Invasion_Target`, same value in every
  row): 0.173 in cycle 0, 2.255 in cycles 1-5, 17.58 from cycle 6 on. The jump comes with `First_Attack_Allowed` 0 -> 1
  and `Need_To_Reconnect_Islands` 0 -> 15 (`Initial_Grace_Period` 228 s, then 235 s). Naboo: 11.1 -> 12.7 -> 13.3, reconnect 0.
- **Plan starts (PSTART, t x 30 = tick):** Conquer_Pirate @Shola at tick 5518 (unalloc 1470, credits 1314). It
  never finishes its first block. Conquer_To_Reconnect @Shola (Space + Ground force, both staged at Shola by
  `SynchronizedAssemble`) at 6925 with nothing to buy (unalloc 0): 11 ships incl. HOME_ONE and SUNDERED_HEART, then
  11 + 7 ground units. Space force done at 7639; Shola changes owner at 7498. So the Shola attack is
  Conquer_To_Reconnect [H], confirming the second-pass reading. Later: Crush @Tatooine at 8533 / 12541 exits at once;
  Conquer_Opponent @Tatooine at 9241 / 9817 / 12778.
- **Why retail's Conquer_Pirate starts late: money.** Rebel Build_Space_Forces 55 -> 598 (net 1350), Build_Ground_Forces
  97 -> 1228 and 1486 -> 3769 (627 each). Credits: 8000 at 87, 4080 at 1351, 594 at 2701, **39 at 3769-4051**,
  1314 at 5518. The 4000-credit `R_Galactic_Turbolaser_Tower_Defenses` at Yavin starts at tick 1225.
- **Port at dev f88d6c1d** (HotM seed 0, Empire human): Build_Space_Forces at 42 builds a
  `Marauder_Missile_Cruiser` (net 1800), Build_Turbolasers @Yavin at 122 (queued 182), Build_Ground_Forces only at
  919, Conquer_Pirate @Shola at 2798 (17 + 7 Y-wings, net 1470), all 24 units at 5724, invasion at 5968.
- **Retail is not one-way here.** Early Rebel production in the four retail runs of this campaign and seed: 10-30,
  11-08 17:08 and 11-08 18:37 are identical (gunboat at 109, light tank 140, tank buster 156, turbolaser 1225). The
  11-03 run (kit with the Flush_G bug, dropped from the references in t08) builds the turbolaser at 140 and Y-wings
  from 1659, like the port, and takes no planet. So "turbolaser first, then an early Conquer_Pirate" is a path
  retail can also take. It is not on its own a port bug.
- **The stable first difference:** all four retail runs build a `Corellian_Gunboat` (1350) as the first Rebel
  Build_Space_Forces unit at Naboo (tick 109); the port builds a `Marauder_Missile_Cruiser` (1800, tick 105). Both are
  tech 1, `Build_Initially_Locked`, star base 2, and match the TF's `Corvette` / `Frigate` / `Capital` (0..4) slots, so
  buildability is not the cause: it is the PotentialPlan solver's unit score (engine/ai/src/galactic_pp.cpp; the 450
  credits it saves may be part of what puts Build_Ground_Forces ahead of the turbolaser in retail [L]). Next step for t09:
  the Build_Space_Forces solve at tick 42 (scores per candidate type, synced draws), against the RE of the solver.

## 2026-11-08 t09: the first-space-unit difference is draw noise; the reconnect wave arrived piecemeal

- **Gunboat vs Marauder is not a scoring bug.** Build_Space_Forces is `IgnoreTarget`, so the Contrast score of every
  candidate is one synced float in [0, AI_Combat_Power / 500] (Marauder 1400, Gunboat 1350, Corvette 1250 are nearly
  tied), and the port's synced draws already differ from retail at tick 1 (130 vs 86 draws). Port HotM Empire-human
  seeds 0-11: Gunboat 7, Marauder 3 (seeds 0, 6, 10), Corvette 2. On the gunboat / corvette seeds the Rebel funds
  Build_Ground_Forces at 122 like retail's 97, Conquer_Pirate @Shola starts at 3987-5550 (retail 5518) and
  Conquer_To_Reconnect @Shola at 7074-8443 (retail 6925). Seed 0 (the q4h parity seed) is a Marauder seed.
- **Why the port's reconnect then failed:** the synchronized gather sent only one unit per shared fleet per AI pass
  (the fleet was marked busy after one split), so a planet's units left 3 ticks apart and the wave landed in up to five
  groups (seed 3: conflicts at Shola 7313 / 7316 / 7319 / 7322 / 7325), each auto-resolved and lost alone. Retail lands
  all 17 on 7161. Fixed (docs/PHASE4.md, q2f t09 follow-up): seed 3 now takes Shola at 7626, seed 4 at 7647, and 7 of 12
  seeds take it by 13501 (before: 2). The wave still lands in two groups 3-4 ticks apart (seed 3: 7310 / 7313; cause not
  traced) [open].
- **Still open:** in the other 5 seeds the reconnect solve wants units built (GroundForce 17+4, SpaceForce 19+8 ...),
  so `Are_All_Units_On_Free_Store()` is false and the thread only produces. Retail's reconnect had nothing to buy. The
  port's Shola is held more strongly: Rebel view at 6860 (seed 1) ground total 2722 / space 8275 (with structures),
  units 1962 ground (two MAL_Rocket_Vehicle_Companies, mercs) and 2025 space (Interceptor4, StarViper, Skipray); retail's
  members-only leaf `GroundTotalUnnormalized` is 322 -> 772 over cycles 0-5, space 3275. The port's Underworld builds
  MAL companies at Shola through Build_Ground_Forces (1474, 2993, 5566); retail's Underworld queued an MDU company there
  that stalled on installments for ~1300 ticks (1538-2827). Next: the Underworld's Build_Ground_Forces pick and the
  pay-as-you-go stall against retail.
