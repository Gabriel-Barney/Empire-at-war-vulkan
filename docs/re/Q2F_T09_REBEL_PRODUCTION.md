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
