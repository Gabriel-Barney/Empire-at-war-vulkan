# A2g t02 / t04: the port's Empire budget and single-structure plan releases against retail (2026-11-05)

Base: `0b18b45` (PR #84, a2f t07 / t09), MSVC Release. Retail: the a2f t07 probe (`A2F_T07_BUDGET_PROBE.md`, runs `20261105-191238` and
`20261105-192142` in `tools/probe_mod/a2f_budget/out/`). Tags as in the other `docs/re` pages.

Method. The engine was exported (`git archive 0b18b45`) into a scratch folder and patched there with env-gated stderr traces
(`tools/analysis/a2g_trace_patch.py`: budget row per AI player per frame, accepts with ETA / nominal time, maintenance drops with reason,
Resolve releases; `ALAMO_A2G_T09OFF=1` switches `nominal_allowed_time` off for the pre-t09 comparison). The traces change nothing: the
patched exe still MATCHes `galactic_ai_hotm_human_golden.txt` and gives the same final CRCs. Nothing under `engine/` in the worktree was
touched. The comparison script is `tools/analysis/a2g_port_vs_retail.py` (`budget`, `plans`, `drops`).

## 0. The scenario has to be the Rebel campaign entry

The retail probe ran Galactic Conquest > Heart of the Maw > **Rebel**, i.e. `Sandbox_Heart_Of_The_Maw_Rebel` (CAMPAIGNS_UNDERWORLD_GC.XML).
The port's `galactic_ai_hotm_human` test runs `Sandbox_Heart_Of_The_Maw_Underworld` with the Rebel forced human and 12 scripted commands.
The two entries differ in more than the human: Underworld credits 13000 vs 8000 (the port's Underworld starts with 13000, retail's with
8000), Rebel 8000 / tech 2 vs 13000 / tech 3, different Empire, Rebel and Underworld starting forces, Underworld AI player type, story plots.
Only the Empire's 8000 credits and planets are the same. So every comparison below uses the port run
`--economy --galactic-ai --campaign Sandbox_Heart_Of_The_Maw_Rebel --human Rebel` with no human script (retail had no orders), seed 1 and
seed 0; the HotM-human (Underworld entry) numbers are given only where t04 asks for them.

## 1. t02: Empire budget, port vs retail

Retail: decoded `BudgetFractionToBuild` samples (A = available pool +0x44, R = rate +0x48). Port: the same fields from the per-frame trace,
read at the retail sample frames. Fiscal payments are at mode frame 1351 + 1350 k on both sides.

| Item | Retail | Port (Rebel entry, seed 1, t09) | Verdict |
|---|---|---|---|
| Init frame | initialised byte 1 at mode frame 1; story sample at frame 0 already sees pools | Init at frame 1 (first AiContext service) | **match** by frame 1; whether retail's frame-0 story sample runs before or after frame 1's AI service cannot be told from the probe |
| Init credits | 8000 | 8000 | match |
| Offensive A at Init | 3105.34 (0.3882) | 3106.80 (0.3883) | differs by +1.46 (0.05 %) |
| Infrastructure A at Init | 4000.32 (0.5000) | 3998.45 (0.4998) | differs by -1.87 (0.05 %) |
| MajorItem A at Init | 894.34 (0.1118) | 894.76 (0.1118) | differs by +0.42 |
| Defensive / Information | empty (weight 0) | not in the template / weight 0 | match |
| Raw weights at Init | wOFF 8.33333, wINFRA 10.7351, wMAJOR 2.4 | 8.3333, **10.7250**, 2.4 | Offensive and MajorItem match; Infrastructure -0.0101 |
| Template | base (L1 distance 0) | `Basic_Empire_Default` | match |
| Income at the first payment | 667.5 = 14.8333/s | 670 = 14.8889/s | differs by +2.5 / cycle |
| Income later | 14.83/s every cycle (planets unchanged) | 13.0/s from frame 2701 (585 / cycle) | differs by -82.5 / cycle |
| T | 45 s, written at frame 1351 | 45 s, written at frame 1351 (0 before) | match |
| Infrastructure A sign | never negative; min -0.0008 (spent to 0 at 2634) | never negative; min 0.0 (spent to 0 in cycles 1, 3-4, 6) | **match** |
| MajorItem A over time | 894 -> 969 -> 1044 -> 1127 -> 1189 -> 1239 -> 1304 -> 1380 (accrues R 1.66/s, never spent) | 0 from frame 43 on (894.76 restored at each reweight, back to 0 within 1-2 frames) | **differs** (structural) |
| Normalised weights over time | 0.388 / 0.500 / 0.112 in all 7 cycles (R 5.76 / 7.42 / 1.66) | 0.582 / 0.250 / 0.168 (cycle 1), 0.369 / 0.514 / 0.118 (2-4), 0.390 / 0.486 / 0.125 (5-7) | differs |
| Offensive / Infrastructure A per cycle | see the table below | | differs (values), same order of magnitude |

Per cycle (A at the first retail sample after the payment / the last one before the next; retail | port):

| k | pay | OFF first | INFRA first | OFF last | INFRA last | credits at last sample |
|---|---|---|---|---|---|---|
| 1 | 1351 | 293 \| 161 | 681 \| 424 | 329 \| 0 | 0 \| 221 | 1966 \| 1496 |
| 2 | 2701 | 329 \| 0 | 0 \| 221 | 589 \| 291 | 334 \| 502 | 2633 \| 1007 |
| 3 | 4051 | 589 \| 291 | 334 \| 502 | 413 \| 531 | 668 \| 0 | 2864 \| 792 |
| 4 | 5401 | 413 \| 531 | 668 \| 0 | 196 \| 683 | 894 \| 323 | 2279 \| 1205 |
| 5 | 6751 | 196 \| 683 | 894 \| 323 | 147 \| 930 | 231 \| 631 | 1617 \| 1790 |
| 6 | 8101 | 446 \| 930 | 488 \| 631 | 187 \| 0 | 488 \| 954 | 1971 \| 1263 |
| 7 | 9451 | 446 \| 0 | 821 \| 954 | 270 \| 273 | 821 \| 233 | 2463 \| 1488 |

### Causes

1. **MajorItem flushed every commit (bug, spec-backed).** Two Offensive-category goals run `FlushMajorItemBudget.lua`
   (`Budget.Flush_Category("MajorItem")`, A = 0) for the Empire from frame 42: in frames 0..10200, 43 x `Flush_MajorItem_Budget` and
   43 x `Reclaim_Excess_MajorItem_Budget` (Underworld entry: 2 and 53). Retail's MajorItem pool grows without a dip, so neither ran there.
   - `Reclaim_Excess_MajorItem_Budget` = `Can_Reclaim_Excess_MajorItem_Budget` = `Variable_Self.ActiveGoals {Parameter_Category =
     AIGoalCategoryType[MajorItem]} > 0`. The port's ActiveGoals ignores `Parameter_Category` and counts every active goal
     (`GalacticAiWorld::Impl::active_goals`, token `ACTIVEGOALS`), so the equation is 1 from the first commit (traced: 0 at frames 1 / 42,
     1 from 124). AI_GALACTIC_TOKENS.md: "number of active goals whose category is in the `Parameter_Category` mask" [H].
     `Basic_Empire_Default` does not turn the MajorItem goal category on, so retail's value is 0. Fix: section 3, F1. With the fix in the
     scratch build the Underworld-entry Empire's MajorItem pool accrues like retail's (895 -> 1194 -> 1212 -> 1279 -> 1582 -> 2100 -> 3316,
     until its tech plan spends it), Reclaim starts 53 -> 6 (those 6 after its Tech_Upgrade template turned MajorItem goals on).
   - `Flush_MajorItem_Budget` = `Need_To_Flush_MajorItem_Budget` = `Needs_More_Space_Force_Globally` (`Variable_Human.SpaceTotalUnnormalized
     > Variable_Self.SpaceTotalUnnormalized`) + `HasUnit(Death_Star)`. The port evaluates it 1 from frame 1 in the Rebel entry (Empire
     `Friendly_Global_Space_Unit_Raw_Total` 52600). Retail never flushed, so it was 0 there or the goal never activated. [L] cause: the
     port's SpaceTotal content for the human (the Rebel entry's human has Rebel_Star_Base_2 and a large starting fleet) or a difference in
     Variable_Human. Not fixable without retail values: needs the probe additions in section 4.
2. **Infrastructure raw weight 10.7250 vs 10.7351 (Init split off by 0.05 %).** The +10 "missing structure" term is the same (one of
   LVF / barracks / mining facility missing on both sides); the first term differs, 0.7250 vs 0.7351 =
   1.1 x (2 + 0.5 TechLevel - MaxedStarbases - MaxedGroundbases) x (1 + Maintenance) x (1 + 2 x (mines / planets < 0.5)). A 1.4 % gap in
   one factor, most likely `Variable_Self.Maintenance` (normalised upkeep, AI_GALACTIC_TOKENS [M] formula) or the Maxed* normalisation [L].
   Retail's own value drifted to 10.7364 over 5.5 min, which fits Maintenance (the only input that moves without a structure change).
3. **Income 670 vs 667.5, then 585.** The port's base income is right: an economy-only run of the same entry pays the Empire 667.5
   (`8000 -> 8668` printed with %.0f). The difference is the trajectory: the port's Empire starts `Conquer_Pirate @Felucia` at frame 42
   (Felucia is Underworld-held; the goal applies to any non-human Enemy / Neutral planet), moves the Kashyyyk fleet there, takes Felucia
   at frame 870 and loses the uncovered Kashyyyk to the Underworld at 878 (+2.5 income), then loses Felucia again at 2414 (-82.5). Retail's
   Empire income stayed 667.5 for 7 cycles, so it held its 4 planets. Seed 0 and the pre-t09 build give the same 870 / 878 swap, so it is
   not t09. Which part differs (Conquer_Pirate accept, the space battle's auto-resolve, or the Underworld's counter-move) is not covered by
   any retail trace: the Q2 traces have a human Empire, and the a2f kit does not hook ConquerPiratePlan (section 4).
4. **Per-cycle Offensive / Infrastructure values and weights** follow from 1 and 3: the planet swap changes PlanetsControlled and the
   Empire's space force (52600 -> 32500 by frame 931, so `Has_Enough_Space_Force_Globally` and the MajorItem / Offensive terms move), the
   port builds the LVF / barracks early (wINFRA 10.725 -> 3.575 at 1153, back to 10.45 at 2550 when Kashyyyk's structures are gone),
   and the MajorItem share is redistributed by the flush. Retail spent 6240 credits by frame 1334 against the port's 3708 by frame 1300, also a production
   difference, not a budget-formula one. The budget formulas themselves (Init, tick, drift, reweight) show no deviation: the Init split
   matches to the equation inputs, ΣP equals credits at every tick, T and the income-rate write match.

## 2. t04: single-structure plan releases

### Retail data

Only the a2f t07 plan hook covers the Empire AI with a human Rebel (the Q2 traces have a human Empire and hook only
BuildSpaceForces / BuildGroundForces / ConquerPirate for the Rebel and Underworld AIs). PSTART per hooked plan, frames 0..10140
(5.63 galactic minutes), both runs merged:

| Plan (hooked) | Retail starts | per min | Port, Rebel entry, same window: seed 1 t09 / seed 1 pre-t09 / seed 0 t09 / seed 0 pre-t09 |
|---|---|---|---|
| BuildBaseComponentLightVehicleFactory (Empire) | 10 seen (frames 1435, 8230, 8392, 8554, 8878, 9100, 9322, 9544, 9763, 9985); the script's sequence numbers 3..14 imply 12 | 1.78 (2.13) | 2 / 2 / 7 / 7 (released 2 / 1 / 4 / 4) |
| BuildBaseComponentBarracks (Empire) | 0 | 0 | 3 / 3 / 4 / 4 (all released) |
| BuildBaseComponentResearchFacility | 0 | 0 | 0 / 0 / 0 / 0 |
| BuildEconomicStructurePlan (Empire) | 0 | 0 | 2 / 2 / 2 / 2 |
| EmpireAdvanceTechPlan | 0 | 0 | 0 / 0 / 0 / 0 |
| UpgradeStarbasePlan | Empire 0, Underworld 1 (frame 2794) | 0.18 (UW) | Empire 1 / 5 / 3 / 3, Underworld 2 / 2 / 2 / 2 |
| BuildBaseComponentHeavyVehicleFactory | not hooked | - | 11 / 18 / 17 / 17 (released 8 / 16 / 15 / 15) |
| BuildBaseComponentAdvancedVehicleFactory | not hooked | - | 0 / 5 / 0 / 0 |
| LiftBlockadePlan | not hooked | - | 0 / 1 / 0 / 0 |

Retail's LVF plan restarts in a loop: from frame 8230 every 162 frames (5.4 s), later every 222 frames (7.4 s), each with
`Get_Spendable_Resources` 0 and `Get_Unallocated_Resources` 500 at start. That is the port's pattern for single-structure plans: the plan is
accepted (HVF: ETA 48 = nominal 48, AllowedTime 72), queues its structure, and at the first maintenance 3-4 s later 0x525200 + elapsed is
150-190 s (queue wait with the funds time of a 1000-credit item on a 7-9 credits/s Infrastructure rate), so it is time-dropped ("failure
(released)") and re-proposed; port HVF loops have periods of 161-163 and 212 frames. Retail releasing a single-structure plan every
5-7 s for minutes at a time is therefore normal behaviour, and the port's rates in the same scenario are of the same order as retail's
(hooked base-component plans LVF + barracks + research facility: retail 10-12, port 5-11 starts in the window; LVF lower in the port,
barracks / economic structure higher; adding the unhooked HVF / Advanced VF the port has 16-28).

### The 48 -> 96 doubling (HotM-human, Underworld entry, 72000 ticks) is not caused by the t09 AllowedTime base

| 72000 ticks, Empire | pre-t09 | t09 |
|---|---|---|
| plans started / "(released)" | 475 / 53 | 501 / 99 |
| HVF starts / released (maintenance time drops) | 22 / 15 (15) | 35 / 31 (29) |
| Lift_Blockade starts / released | 18 / 18 (10 desire drops, 8 displaced in Resolve) | 31 / 31 (12 desire, 19 displaced) |
| Advanced VF / Barracks / LVF released | 0 / 3 / 5 | 8 / 7 / 6 |
| releases by a failed Resolve time test ("random-time") | 0 | 0 |

- For every single-structure accept in both runs the PP ETA equals the nominal time (one selected structure: ETA = build time x 0.8 =
  0x6c0450 x 0.8), so AllowedTime = 1.5 x 48 = 72 s for HVF before and after t09. t09 changes the AllowedTime of these plans by nothing.
- The first 8 HVF loops (frames 124 .. 1324) are identical in both runs. t09 then has more and longer loop bursts (9580-11067, 20411-21682,
  31250-32044) where pre-t09 has fewer (20388-20812, 63610-64506): the loops run whenever the Empire wants a heavy factory and its
  Infrastructure rate cannot fund one, which depends on the game state.
- Lift_Blockade releases are desire drops (the blockade ended) and Resolve displacements, never time drops; their number follows the
  number of blockades of Empire planets.
- So the doubling is a trajectory effect of t09's other result (fewer, shorter Conquer_Opponent plans, Naboo 13389 ticks earlier, a
  different fleet / credit history), not over-release by the new base. The explanation in A2F_T05_BUDGET_TRACE.md section 7 ("single-structure
  plans whose ETA includes a queue wait now exceed their nominal build time x tolerance") does not hold: their ETA and nominal time are equal.
- In the Rebel entry (the retail scenario) t09 lowers the early HVF releases (seed 1: 16 -> 8) or leaves them unchanged (seed 0).

Verdict: the port does not over-release single-structure plans compared with the available retail data; the t09 change is not the cause
of the larger count. Open: HVF, Advanced VF, Barracks timing and Lift_Blockade have no retail counts (section 4).

## 3. Proposed engine fixes (engine/ai owner)

**F1: ActiveGoals honours `Parameter_Category`** (AI_GALACTIC_TOKENS.md, ActiveGoals 0x6e0490 [H]). Exact change as tested in the
scratch build (a one-off patch against `0b18b45`, behind an env switch; it is now the real fix, A2g F1, with the category list read as a mask):
- `engine/ai/include/alamo/ai/galactic_ai_tokens.hpp`: `std::function<int32_t(int32_t player)> active_goals;` ->
  `std::function<int32_t(int32_t player, std::span<const std::string_view> categories)> active_goals;`
- `engine/ai/src/galactic_ai_tokens.cpp`, token `ACTIVEGOALS`:
  `const std::vector<std::string_view> cats = param_texts(params, "Parameter_Category"); return out = r.hooks_.active_goals ? r.hooks_.active_goals(pl, cats) : 0.0, true;`
- `engine/ai/src/galactic_ai_impl.hpp`: `int32_t active_goals(int32_t p, std::span<const std::string_view> cats) const;`
- `engine/ai/src/galactic_ai.cpp`: hook `[this](int32_t p, std::span<const std::string_view> cats) { return active_goals(p, cats); }`; body:
  no category given -> every active goal (today's behaviour); else the sum of `pa->ctx.goals.active()[c].size()` over the category indices
  `c = defs.find_category(name)` of the given names (`active()` is indexed by AIGoalCategoryType; `param_texts` already turns
  `AIGoalCategoryType[MajorItem]` into `MajorItem`).
- Effect: HotM-human golden DIFFERS (goldens to regenerate); the only shipped use is `Can_Reclaim_Excess_MajorItem_Budget`.

No fix is proposed for t09 (section 2).

## 4. Probe additions for the running / next a2f_budget run

- Hook more plans in `build_a2f_mod.py` PLANS: `BuildBaseComponentHeavyVehicleFactory` (BaseForce_Thread),
  `BuildBaseComponentAdvancedVehicleFactory` (BaseForce_Thread), `LiftBlockadePlan` (MainForce_Thread), `FlushMajorItemBudget`
  (MainForce_Thread; tells whether retail ever flushes MajorItem), `ConquerPiratePlan` (MainForce_Thread; the port's frame-42
  Conquer_Pirate @Felucia), optionally `ConquerOpponentPlan` (SpaceForce_Thread).
- Record the end too: a `PRET` record when the wrapped thread returns (a plan the goal system releases has its thread killed, so no PRET),
  and wrap `BaseForce_Production_Failed` (`PFAIL`, as the Q2 kit does). Lift the per-script cap `A2FB_PLAN_N >= 40` for long runs (the LVF
  loop alone makes about 8 starts per minute while it runs).
- Evaluate for the Empire (shipped, no `#`): `Needs_More_Space_Force_Globally`, `Need_To_Flush_MajorItem_Budget`,
  `Can_Reclaim_Excess_MajorItem_Budget`, `Has_Enough_Space_Force_Globally`; mod equations `Variable_Human.SpaceTotalUnnormalized`,
  `Variable_Self.SpaceTotalUnnormalized`, `Variable_Self.ActiveGoals` with and without `Parameter_Category = AIGoalCategoryType[MajorItem]`,
  `Variable_Self.Maintenance` (cause 2) and `Variable_Self.PlanetsControlled`.

## 5. Reproduce

```
alamo_headless --economy --galactic-ai --campaign Sandbox_Heart_Of_The_Maw_Rebel --human Rebel --ticks 10200 --workers 0 --quiet --ai-log rb.log 2> rb.err   (ALAMO_A2G_TRACE=3, patched build)
python tools/analysis/a2g_port_vs_retail.py budget --retail-samples tools/probe_mod/a2f_budget/out/20261105-192142/samples.tsv --port-trace rb.err
python tools/analysis/a2g_port_vs_retail.py plans --retail-records <both runs>/records.txt --port-log rb.log
python tools/analysis/a2g_port_vs_retail.py drops --port-trace rb.err
```
