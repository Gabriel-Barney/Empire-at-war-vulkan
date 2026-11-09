# A2M t07: retail Rebel slice run (Advance_Tech_Rebel)

Run `tools/probe_mod/q2_traces/out/20261108-212955-rebel-slice` (plan hook, memory watcher) with its q2 trace
`out/20261108-212956-galactic-custom` (trace.q2t). Sandbox_Heart_Of_The_Maw_Empire, human Empire, Medium, hands-off,
12 fiscal cycles (17094 mode frames), no battles. Report: `slice_report.txt` / `slice_plans.tsv`, regenerated offline with the
fixed `rebel_slice/slice_report.py --run-dir ... --trace-dir ...`.

Rebel (player 0): tech 2 at galactic start, max tech 4, 11 entries in the unlocked list. Owners at start:
Empire = Corellia, Kuat, Kashyyyk, Tatooine; Underworld = Felucia, Nal Hutta, Shola, The_Maw. Shola became Rebel at mode frame 7498.

## 1. Per plan run

Frames are mode frames. cr = Rebel credits read by the plan hook just after `Move_To` returned and just after `Land_Units()` ran.
"Price" = the credit drop on the landing frame. The watcher shows the same value once the per-frame production drain is subtracted.

| run | start | target (owner) | cr before | cr after | tech | result |
|---|---|---|---|---|---|---|
| 1 | 97 | Nal Hutta (U) | 7011.90 | 6311.90 | 2 | slice, 700, unlocked #12 |
| 2 | 2296 | The_Maw (U) | 759.91 | 759.91 | 2 | landed, nothing |
| 3 | 2857 | Kuat (E) | 1006.69 | 306.69 | 2 | slice, 700, #13 |
| 4 | 5437 | Tatooine (E) | 1114.01 | 1114.01 | 2 | landed, nothing |
| 5 | 6070 | Kuat (E) | 927.34 | 227.34 | 2 -> 3 | slice, 700, #14, tech raised |
| 6 | 8149 | Tatooine (E) | 1826.47 | 1826.47 | 3 | landed, nothing |
| 7 | 8761 | Nal Hutta (U) | 1274.49 | 374.49 | 3 | slice, 900, #15 |
| 8 | 11368 | The_Maw (U) | 1468.68 | 1468.68 | 3 | landed, nothing |
| 9 | 11623 | Kuat (E) | 1468.68 | 468.68 | 3 | slice, 1000, #16 |
| 10 | 14104 | Tatooine (E) | 1848.93 | 1848.93 | 3 | landed, nothing |
| 11 | 14698 | Nal Hutta (U) | 2230.68 | 2230.68 | 3 | landed, nothing (unexplained, see 3) |
| 12 | 15292 | Felucia (U) | 45.03 | 45.03 | 3 | landed, nothing |

## 2. Mechanism observed

- The slice fires at the landing, on the frame `Move_To` returns. `LandUnits()` (PGTASKFORCE.LUA) calls
  `taskforce.Land_Units()` before its own `BlockOnCommand`. The hook therefore writes PLANDPRE after the trigger has
  already run, and PEXIT comes before PLANDPRE on the same frame. So landing on a sliceable planet consumes the droids: force count 1 -> 0,
  the plan exits, and `Land_Units` never completes. In the report (point c), run 5 "exiting without landing" is this case.
- On that frame the watcher sees all of these together: one credit drop of exactly the price, the unlocked-list count +1, and for run 5 the tech change 2 -> 3. The trace has
  4 synced draws on that tick for runs 1, 7 and 9 (R, AI pick, s, the trigger's vf12 draw: REMAINING_GAPS 1.3 step 8 + P4_SIM_SYSTEMS 6.1).
  Run 5 has 5 draws and run 3 has 6; these may include background draws.
- The tech advance is not a separate purchase. Run 5 sliced the last tech-2 candidate ("only one" entry, REMAINING_GAPS 1.3
  step 7.5), which raised the tech to 3 on the same frame. No other tech change occurs.
- Prices match `(CostCurve(Planet_Tech_Availability_Rating) + affinity) x Slice_Cost_Credits`. Kuat and Nal Hutta have
  rating 5, so the curve gives 0.5. Inferred types (this watcher read no names):
  - runs 1, 3, 5: 0.5 x 1400, the three tech-2 candidates (Ground_Infiltrator_Facility, Alliance_Assault_Frigate,
    Rebel_Heavy_Tank_Brigade);
  - run 7: 0.5 x 1800 = Gallofree_HTT_Company;
  - run 9: 0.5 x 2000 = A_Wing_Squadron or Rebel_Speeder_Wing. Calamari_Cruiser would cost (0.5 - 0.25) x 2000 = 500 at Kuat (affinity).
- Landings with no slice:
  - Tatooine (rating 2, x1.25: 1750 at tech 2, 2250 / 2500 at tech 3) was never affordable.
  - Felucia (x1.25) had 45 credits.
  - At these landings the credits, the list and the tech did not change.
- Target choice: `FindTarget(TechForce, "Is_Good_Slice_Target", "Enemy", 0.1)` uses `1 - IsFaction{Pirates, Neutral}`, a flat
  weight that ignores price and affordability. Even so, the 12 retail picks never hit Corellia, Kashyyyk or (before 7498) Shola.
  The chance of that under a uniform pick is about 1%, so the candidate list is probably filtered [M].

## 3. The_Maw verdict

Confirmed: **a trip to The_Maw never slices.** Runs 2 and 8 landed (`Land_Units` returned, PLANDX, count stays 1). Credits are unchanged
apart from the production drain (run 8: 1468.68 throughout), the unlocked list and the tech are unchanged, and there is no draw burst. Two reasons:
- The_Maw has `Planet_Surface_Accessible` = No.
- Its rating 0 gives x1.75: 2450 at tech 2 and 3150 / 3500 at tech 3, above the credits at both landings.

Open: run 11 (Nal Hutta, tech 3, 2230 credits). By REMAINING_GAPS 1.3, two entries at 1000 were affordable (Calamari_Cruiser and the
A-Wing / Speeder left over from run 9). Nothing happened: count 1, no credit, list or tech change. Runs 1 and 7 at the same planet sliced,
with a similar Underworld orbit.

## 4. Port comparison

The port implements the spec 1:1:
- `engine/galactic/src/special_systems.cpp`: `slice_candidates` 765-776, `slice_accepts` 1325-1330, prices
  `slice_entries` 1332-1359, `apply_slicer` (tech-raise loop, R draw, AI affordable pick) 1361-1400, `slice_execute`
  1402-1429;
- tech sweep: `engine/galactic/src/campaign.cpp` 1716-1745;
- Land_Units fires the trigger: `engine/ai/src/galactic_ai.cpp` 1561-1598. It skips surface-inaccessible planets (The_Maw) at 1574-1579.

Port run: build-wt-msvc `alamo_headless --economy --galactic-ai --story --campaign Sandbox_Heart_Of_The_Maw_Empire --human Empire
--seed 1 --workers 0 --ticks 17100 --ai-log/--trace-out`. The build is 19:12, before dev d3237074.

| | retail | port |
|---|---|---|
| plan runs | 12 (last start 15292) | 7 (122, 2296, 4091, 4496, 5472, 5796, 8164; none after 8326) |
| targets | NalHutta 3, Kuat 3, Tatooine 3, The_Maw 2, Felucia 1 | Felucia 2, Shola 2, Corellia 2, Tatooine 1; never The_Maw |
| slices | 700, 700, 700 (tech -> 3 at 6271), 900, 1000 | Felucia 1750 (409), Corellia 1050 (6046), Corellia 1050 (8323; tech -> 3) |

Differences:
1. **Target picks.** These come from the synced RNG (a2m t03: draw counts differ). The port can pick Corellia / Kashyyyk / Shola, which retail never
   did in 12 picks. It sliced at Felucia (1750) where retail went to Nal Hutta (700).
2. **No Advance_Tech_Rebel after 8326.** The droids respawn at 10124, and Rebel credits are above 1000 in cycles 10-13.
   Can_Afford_Steal_Tech = `(Credits > 1000) x ((NetIncome > 500) + (Credits > 2500))`. The port Rebel never takes Shola (0 owner
   changes; retail took it at 7498), so the likely cause is NetIncome <= 500. Not verified.
3. **R draw at landings that slice nothing.** The port draws R before the affordability filter
   (`special_systems.cpp:1379-1380`; `inline_range_float` always draws, even for 60/60 or 70/70). Retail Felucia run 12 shows no draw within ±3 ticks of
   the landing. Tatooine run 10 shows one draw at +1, which is ambiguous.

## 5. Kit changes (offline)

- `slice_watch.py`: the unlocked / locked lists are DynamicVectors `{T** data, i32 count, i32 capacity}`. The raw data had
  count 11 -> 16 and capacity 20. `decode_raw()` now decodes them, with std::vector as the fallback, so the next run names the types. A stale player address after a mode change
  (garbage tech) is skipped.
- `slice_report.py`:
  - target keys drop the hook's `obj:` prefix, which fixes the "The_Maw: no plan run" bug;
  - list additions come from the raw count (`#n`);
  - prices are net of the production drain;
  - a Lua-only "team despawned at the landing" detection is added;
  - planet owners are read from trace.q2t.
- `selftest_slice.py`: now 22 checks, all ok.

## 6. Proposed follow-ups

1. Rerun the kit with the fixed watcher (any campaign) to get type names; the inferred types in section 2 then become confirmed.
2. RE / probe the unexplained run 11, Nal Hutta at tech 3 with 2230 credits and no slice: the ability "ready" gate (0x37be10), the
   0x48d5a0 block, and whether Land_Units fires 0x3a00b0 at an occupied enemy planet. This joins a2m t04's "Land_Units 0x6cec80 at an enemy
   planet with an accessible surface".
3. RE the `FindTarget` candidate list for `"Enemy", 0.1`: why retail never picks Corellia / Kashyyyk / Shola.
4. Port check: the port's Rebel `Can_Afford_Steal_Tech` inputs (NetIncomeUnnormalized) after tick 8326. This probably follows from t03's
   Shola capture.
5. RE: whether R (step 4) is drawn when Min = Max, or for an AI with no affordable entry; then align `apply_slicer`.
