# A2h t01: port vs retail Empire budget, re-run with A2g F1 + F2 (2026-11-06)

Port: `d0087c4` (A2g, F1 ActiveGoals category filter, F2 reset flag on template switch), MSVC Release, trace-patched export
(`tools/analysis/a2g_trace_patch.py`). Retail: the a2g t01 run `20261105-195851` (20 min, `A2G_T01_RESET_FLAG_PROBE.md`).
Scenario `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel, no human orders, seed 1, 24000 frames.

```
alamo_headless --economy --galactic-ai --campaign Sandbox_Heart_Of_The_Maw_Rebel --human Rebel --ticks 24000 --workers 0 --quiet --ai-log rb.log 2> rb.err
  (ALAMO_A2G_TRACE=3, ALAMO_A2G_EQS=Needs_More_Space_Force_Globally,Need_To_Flush_MajorItem_Budget,Can_Reclaim_Excess_MajorItem_Budget)
python tools/analysis/a2g_port_vs_retail.py budget --retail-samples tools/probe_mod/a2f_budget/out/20261105-195851/samples.tsv --port-trace rb.err
```

## Results

| Item | Retail | Port (F1 + F2) | Verdict |
|---|---|---|---|
| Init pools | 3105.3 / 4000.3 / 894.3 | 3106.8 / 3998.4 / 894.8 | unchanged from a2g (Infrastructure weight 10.7250 vs 10.7351: t03) |
| `Can_Reclaim_Excess_MajorItem_Budget` | 0 (pool never dips) | **0 in every sample**, no `Reclaim_Excess_MajorItem_Budget` start | **fixed by F1** (was 1 from frame 124, 43 starts) |
| `Need_To_Flush_MajorItem_Budget` | 0 or never activated (pool never dips) | 1 from frame 1 (`Needs_More_Space_Force_Globally` = 1 throughout) | **still differs**: 144 `Flush_MajorItem_Budget` plan lines in 24000 frames |
| MajorItem A at the cycle ends | 969 -> 1044 -> 1118 -> 1189 -> 1239 -> 1297 -> 1372 | 389 -> 487 -> 333 -> 577 -> 0 -> 0 -> 0 | differs: accrues one cycle, flushed the next (flush, t02) |
| Income | 667.5 per cycle until Yavin 4 is taken (day 9) | 670 at 1351, then 585 (Felucia taken from the pirates at 901, lost to the Underworld at 2301-2721) | differs: trajectory (port's frame-42 `Conquer_Pirate @Felucia`; t02 hooks ConquerPiratePlan) |
| Template switch to `Tech_Upgrade` | at mode frame 17569, back at 21042 (reset flag set, P = Σ × w re-split at the next tick) | never: `Empire_Tech_Trigger` = 0 in every sample to 24000 (`Empire_Tech_Untrigger` toggles 0/1) | **not comparable here**: the trigger needs `Empire_Should_Upgrade_Tech`, i.e. BudgetFractionToBuild thresholds (0.7 / 0.9) the port's Empire cannot reach on 1.1-2.4k credits (retail 5-5.6k at the switch) |
| Infrastructure A sign | never negative | never negative | match |

F2 itself is covered by `test_budget_reset_on_template_switch` and runs in the HotM-human (Underworld entry) golden, where the
Empire's Tech_Upgrade template does switch; this scenario does not reach a switch in the port.

Note (a2h t02, `A2H_T02_SPACE_TOTAL.md`): this command has no `--story`, so no story hero (the Empire's Accuser, the
Rebel's Home One / Sundered Heart / Rogue Squadron) exists in it; compare with retail using `--story`. The NMS = 1 above
was mostly the port counting starbases in `SpaceTotalUnnormalized` (fixed in a2h).

## Open (carried to the other a2h tasks)

- Why `Needs_More_Space_Force_Globally` is 1 from frame 1 (the t02 probe evaluates it and its two SpaceTotalUnnormalized inputs in retail).
- The early Felucia conquest / loss: the t02 probe hooks `ConquerPiratePlan`.
- The Tech_Upgrade switch depends on BudgetFractionToBuild: t07 (remaining-time min, in-progress cost) and the credit trajectory.
