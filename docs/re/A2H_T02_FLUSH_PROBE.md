# A2h t02: why retail never flushes the Empire's MajorItem budget (in-game probe, 2026-11-06)

Kit: `tools/probe_mod/a2f_budget/` with the a2h t02 additions (README), run `20261106-040725`, driven by Claude with computer use.
Retail FoC 1.121, Galactic Conquest > Heart of the Maw > Rebel, Medium (start screen: tech 4, 13000 credits). Report:
`probe_results/a2h_t02_flush_20261106.txt`.

Run: mode frames 1 .. 56050 (about 31 min of galactic time). No battle prompt this time; the Empire lost a planet at
mode frame 17551 (income 14.83 -> 13.0 /s) and another at 44551 (8.33 /s). The Claude session ended while the game
ran; the kit's watcher still wrote the report, removed the mod and verified the profile restore (exit 0), and the game
was gone afterwards.

## 1. Answers

1. **`Needs_More_Space_Force_Globally` is 0 for the retail Empire, so `Flush_MajorItem_Budget` never activates** [H].
   All four Empire MJ samples (mode frames 1435, 5416, 17179, 51511):

   | f | NMS | NTF | CRM | HES | HST (Human SpaceTotalUnn.) | SST (Self) | FRT | ERT | AG (no category) | AGM (MajorItem) | MNT | MNTU |
   |---|---|---|---|---|---|---|---|---|---|---|---|---|
   | 1435 | 0 | 0 | 0 | 1 | 21135 | 24250 | 20250 | 0 | nil | 0 | 0.00106742 | 0.7125 |
   | 5416 | 0 | 0 | 0 | 1 | 21135 | 24250 | 20250 | 0 | nil | 0 | 0.00106742 | 0.7125 |
   | 17179 | 0 | 0 | 0 | 1 | 21135 | 24250 | 20250 | 0 | nil | 0 | 0.00122222 | 0.715 |
   | 51511 | 0 | 0 | 0 | 1 | 21135 | 26750 | 22750 | 0 | nil | 0 | 0.00209185 | 0.784444 |

   The port evaluates NMS = 1 from frame 1 with HST 55775 and SST = FRT 52600 (`A2H_T01_RERUN.md`), so it starts
   `Flush_MajorItem_Budget` all run (144 plan lines in 24000 frames). Cause and port change: `A2H_T02_SPACE_TOTAL.md`.
   No `FlushMajorItemBudget` start for the Empire in 31 min; the Underworld ran it 21 times (frames 847 .. 7198).
2. **SST − FRT = 4000 = one SpaceHero** (the Accuser, AI_Combat_Power 4000) is already in the retail Empire's total at
   mode frame 1435; the port has no SpaceHero at its first services (SST = FRT).
3. **`ActiveGoals` without `Parameter_Category` fails** (AG = nil, i.e. the equation had no answer) and
   `ActiveGoals{MajorItem}` = 0, as `A2H_GALACTIC_AI_RE.md` §1 reads from the binary [H, now measured].
4. **Maintenance**: MNTU 0.7125 at 1435 = the Init upkeep 0.6278 + one Tartan (0.0347) + one Broadside (0.0500), and
   MNT = 0.7125 / 667.5 = 0.00106742, as the fleet-upkeep formula of `A2H_T03_INFRA_WEIGHT.md` / `A2H_GALACTIC_AI_RE.md` §3
   predicts [H]. At 17179 the gross is 585 (MNT = 0.715 / 585).
5. **Reset flag +0x74**: no template switch and no flag transition in this run (the Empire never reached Tech_Upgrade).

## 2. Plan starts (Empire, 31 min)

ConquerPiratePlan 36 (from frame 17404, every ~240 frames, all released without an end record), BuildBaseComponent-
HeavyVehicleFactory 21 (3457 .. 24877), BuildEconomicStructurePlan 34 (44554 .. 55237), UpgradeStarbasePlan 21,
BuildBaseComponentBarracks 17, BuildBaseComponentLightVehicleFactory 5; no LiftBlockade, no AdvancedVehicleFactory.
Every start ended without PRET / PEXIT / PFAIL: the goal system released them.

## 3. Kit defect found

The story channel recorded only its `LOAD` and `DEF` records, no samples: the story script stopped after loading in
retail (the offline selftest passes, so a call retail's Lua lacks is the likely cause). The MJ data above comes from
the plan-channel samples. Section 1 and the 25-minute LONGDONE mark of the report are therefore empty for the Empire;
fix before the next run (a2i).
