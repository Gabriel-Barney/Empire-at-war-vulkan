# a2m round 3: the Underworld's builds before the Rebel Shola capture (retail traces vs port)

Written 2026-11-12 by the a2m r3 Rebel fix lane (branch `claude/a2m-fix-rebel-r3`, base dev 1d35e9f8). Sources: the
retail Q2 traces of `hotm_galactic` (`20261108-170824`, `20261030-061228`, `20261108-204711`: production.q2q and
trace.q2t, Heart of the Maw, human Empire) and the port's 12-seed runs of dev 1d35e9f8 (harness distribution mode, plus
the r3 diagnosis dumps). **No decompiled code was read**: this is behaviour from traces. The open questions at the end are
for an RE task. Diagnosis: session scratchpad `plans/a2m_r3_rebel_diag.md`.

Tags: [H] read from a trace or a port log, [M] one inferred link, [L] hypothesis.

## 1. Retail Underworld production, ticks 0..13500 [H]

Retail Underworld = trace player 6. The three runs are identical through tick 9600; `20261108-204711` differs after
that (other Build_Ground_Forces picks), yet its first starbase upgrade is on the **same frame**.

| tag | queued | planet | type | in queue (frames) | build frames | note |
|---|---|---|---|---|---|---|
| 45 / 46 | 64 / 80 | Felucia / Shola | U_Ground_Palace | 600 | 600 | |
| 47 | 88 | The_Maw | Interceptor4_Frigate | 414 | 414 | Build_Space_Forces; the only space build before 11059 |
| 52 | 147 | NalHutta | Underworld_Saboteur_Team | 600 | 600 | |
| 58 | 1537 | Shola | Underworld_MDU_Company | **1290** | 480 | PAYG stalls (start frame keeps moving) |
| 59 | 1552 | Felucia | Underworld_Merc_Squad | 192 | 192 | |
| 60 | 1573 | NalHutta | Underworld_Saboteur_Team | **8** | 600 | removed after 8 frames |
| 62 | 1697 | NalHutta | Underworld_Saboteur_Team | 600 | 600 | |
| 65 | 2839 | Shola | Underworld_Saboteur_Team | 118 | 600 | removed (refund +195 at 2959, r2) |
| 66 | 2844 | NalHutta | Underworld_Saboteur_Team | 600 | 600 | |
| 67 | 3087 | Shola | Underworld_Saboteur_Team | 1175 | 600 | |
| 68 | 4146 | NalHutta | Underworld_Saboteur_Team | 105 | 600 | removed (refund +171.7 at 4253, r2) |
| 69 | 4218 | Felucia | Underworld_Merc_Squad | 192 | 192 | |
| 70 | 4265 | Shola | Underworld_MDU_Company | **1402** | 480 | PAYG stalls |
| 71 | 4301 | NalHutta | Underworld_Merc_Squad | 192 | 192 | |
| 73 | 5572 | NalHutta | U_Ground_Vehicle_Factory | 1468 | 840 | goal-budget pause (r2 UW diag) |
| 74 | 5846 | Shola | Underworld_MDU_Company | 480 | 480 | |
| 77 | 7091 | NalHutta | Underworld_Merc_Squad | 192 | 192 | |
| 83 | 8393 | NalHutta | U_Ground_Vehicle_Factory | 1232 | 840 | second factory at NalHutta |
| 89 / 91 / 95 | 9664 / 9912 / 10191 | NalHutta | Merc, **MAL_Rocket_Vehicle_Company**, **Destroyer_Droid_Company** | | | run 204711: Disruptor, Merc, Canderous |
| 107 | **11059** | NalHutta | **Underworld_Star_Base_2** | 480 | 480 | all three runs, same frame |
| 115 | 12299 | NalHutta | Crusader_Gunship | 172 | 172 | run 204711: Felucia 12280 |

Facts that matter for the Rebel's Shola attack:

1. Every Underworld ground company before 9664 is an **MDU company at Shola** (cost 1000) or a Merc squad (320). No
   MAL (1400), Destroyer (1200) or Canderous company is queued before 9912, although Shola has a U_Ground_Vehicle_Factory
   from the start (MAL needs that structure on the build planet) and NalHutta has U_Ground_Droid_Works from the start
   (Destroyer needs it). [H]
2. No starbase upgrade before 11059, in any run, including the run whose builds differ after 9600. A frame this stable
   across runs that diverged 1500 frames earlier points at a deterministic gate rather than at money that depends on
   the earlier picks [M].
3. Retail's Underworld is goal-budget bound from about 1500: its MDU companies sit in the queue 2.7 to 2.9 times their
   build time, the NalHutta vehicle factory 1.7 times (r2 diagnosis: paused 628 frames by the goal budget while the player
   had credits). [H for the queue times, M for the cause]

## 2. Port Underworld production, dev 1d35e9f8, 12 seeds [H]

Port Underworld = player 5. From the `x_base` runs of the r3 diagnosis (identical to the harness runs of dev).

| seed | saboteurs ordered < 4300 | starbase orders (< 13500) | ground companies (< 9000) | Shola captured |
|---|---|---|---|---|
| 0 | 3 | SB2 NalHutta 5524; SB1 Shola 12322 | MDU Felucia 1524, MAL Shola 4177, Destroyer NalHutta 6960 | no |
| 1 | 2 | SB2 NalHutta 2860, Shola 4224 | MDU Shola 730, Destroyer NalHutta 1588 / 4254, MDU Felucia 5631, MAL Shola 8294 | no |
| 2 | 2 | SB2 NalHutta 2859, Shola 4142 | Destroyer NalHutta 1511, MAL Shola 2970 | no |
| 3 | 3 | SB2 NalHutta 2872, Shola 5528 | Destroyer NalHutta 1497 / 4194, MAL Shola 5567 / 7055 | no |
| 4 | 2 | SB2 NalHutta 2843, Shola 6916 | Destroyer NalHutta 1514, MAL Shola 2983 / 5680 / 8284 | no |
| 5 | 2 | SB2 NalHutta 1466, Shola 4147 | Destroyer NalHutta 808, MAL Shola 2801 / 4258 / 8226, Destroyer NalHutta 5644 | no |
| 6 | 4 | SB2 NalHutta 4213, 5515 | Destroyer NalHutta 816 / 1498 / 6923 | 7560 |
| 7 | 4 | SB2 NalHutta 4164 | Destroyer NalHutta 1498, MAL Shola 3516, MDU Felucia 4355, Canderous Shola 6864 | no |
| 8 | 3 | SB2 NalHutta 1456, Shola 5731 / 6847; SB3 NalHutta 11655 | Destroyer NalHutta 817, MDU Felucia 1501 / 6895 | 7713 |
| 9 | 3 | SB2 NalHutta 1472, Shola 5720 | Destroyer NalHutta 817, MDU Felucia 1501 / 7019 | 5961 |
| 10 | 3 | SB2 NalHutta 4166 | MDU Felucia 1516, MAL Shola 4190 | no |
| 11 | 3 | SB2 NalHutta 2869, Shola 10938 | Destroyer NalHutta 1501, MAL Shola 2978, MDU Felucia 7018 | no |

- Every seed upgrades NalHutta between 1456 and 5524 (retail 11059); 9 of 12 also upgrade Shola. [H]
- 10 of 12 seeds queue a Destroyer company at NalHutta before 1600 (harness: `build.ticks[DESTROYER_DROID_COMPANY@NALHUTTA]`
  seen in 11/12 seeds, FAIL); 8 of 12 queue a MAL company at Shola. Retail's first ground company is an MDU at Shola. [H]
- Of the three captures, seed 6 (7560) has neither a Shola SB2 nor a Shola MAL; seeds 8 (7713) and 9 (5961) have a
  Shola SB2 queued but no Shola MAL. No seed with a Shola MAL captures. So neither factor alone decides a seed; the r3
  diagnosis experiments (no Underworld Upgrade_Starbase 6/12, MDU-only 7/12, both 11/12) show they add up. [H]

## 3. How the port picks MAL over MDU (seed 0, frame 4084, Build_Ground_Forces solve) [H]

Solver dump `r3d/p5/seed_0/pp.txt` (diagnosis hook, not in dev): `avail = 1290.1`, `rate = 26.507`, first solve.

| candidate | Plan | Contrast float (IgnoreTarget, [0, power/500]) | total |
|---|---|---|---|
| MAL_Rocket_Vehicle_Company | 1.0 | 1.427 | 3.427 (picked) |
| Underworld_Merc_Squad | 1.0 | 0.085 | 2.085 |
| Destroyer_Droid_Company | 1.0 | 0.039 | 2.039 |
| Underworld_MDU_Company | 1.0 | 0.021 | 2.021 |

- FreeStore, Budget, TechTree, Production, Hero and Terrain score 0 for all four; Survival 1.0. So the pick is decided by
  the IgnoreTarget contrast float, whose range grows with the type's power (AI_GALACTIC_2 §1.4 Contrast Score, [H] in
  that doc). A high-power company wins most draws **unless the Budget marshaller vetoes it**.
- Budget test (AI_GALACTIC_2 §1.4, corrected by A2K_T01_BUILD_COST_RE §3.1): an ordinary unit uses
  `available = avail + 0.5 x btime(T, none) x rate`. MAL: 1290.1 + 8 x 26.5 = 1502 >= 1400, so MAL passes. [H port]
- For retail's pick at 4265 (MDU, 1000) to be a budget outcome, retail's Offensive category would have to hold less than
  about 1400 - 212 = 1188 there (and less than 1200 - 0.5 x btime(Destroyer) x rate for the Destroyer). [M]
- The contrast float being drawn once per candidate **per solver iteration** is already in AI_GALACTIC_2 §1.5 item 4 and
  the §7 table ([H] there); the port draws it in `ContrastM::score`, once per scored candidate. So the r3 diagnosis's
  question 3 is answered by the existing spec, and the port follows it.

## 4. The Underworld's money, cycle 1 [H]

Harness `econ.income_cycle1[Underworld]`: port 2282.5 +- 53 (12 seeds, 2150..2315), retail 2150 (FAIL). In the Rebel-human
retail run of A2K_T05 the first payment was 2150 = 2105 + Naboo's corruption 45. The port's extra 130..165 in most seeds
comes before 1351, so it is corruption income from a planet the port corrupts earlier than retail (seed 0: MonCalimari,
ability applied at 444; retail corrupts Naboo at about 620 in the Rebel-human run) [M]. Every later category pool of the
port starts from that surplus.

## 5. Conclusions for the fix lanes

- Causes 1 and 2 of the r3 diagnosis are both "the port's Underworld can afford it and retail's cannot, or retail has a
  gate the port lacks". The retail traces cannot tell which: they have no budget pools for the Empire-human campaign (the
  a2f_budget kit records them only for HotM with a human Rebel, `tools/probe_mod/a2f_budget/a2f_run.py`).
- Section 1 fact 2 (11059 in all three runs) favours a deterministic gate for the starbase.
- Neither cause has a rule in `docs/re/` today that the port is missing; tuning desire, budget or scores is not allowed.
  The questions below are for RE.

## 6. Open RE questions

1. **Upgrade_Starbase for the Underworld (r3 diagnosis 1a-d).** What keeps retail's Underworld from building
   Underworld_Star_Base_2 at NalHutta before 11059, and Shola never? In order:
   - (a) the slot-10 accept for an Infrastructure goal whose plan is IgnoreTarget (UPGRADESTARBASEPLAN.LUA), including any
     planet "upgrade available" test (next type's `Required_Star_Base_Level`, `Required_Ground_Base_Level`, tech, the
     STRATEGIC_GAPS §1.2 step 20 DUMMY_STAR_BASE level rule, the step 20 hostile-fleet block);
   - (b) the candidate list 0x6c1fb0: AI_GALACTIC_2 §1.2 says a 0x5C generic is replaced by the planet-specific variant
     (0x3f8fd0 on the target planet's surface). What does 0x3f8fd0 return for an Underworld planet with SB1, given the
     TaskForce entry `Rebel_Star_Base_1` (affiliation Rebel)? Is it keyed on the target planet's owner, the entry's own
     faction, the planet's ground-base type (D+0x128), or the next level's type of the planet's current starbase? Does an
     IgnoreTarget plan have a target planet for this replacement at all? The port uses the first StarBase-kind type the
     player can build at the target (`GalacticAiWorld::Impl::next_starbase_type`, [M]);
   - (c) the run time: does BUILD_TASK_LIST / the AIPlanetBuildTask market bid on the replaced type or on the generic
     `Rebel_Star_Base_1` (which the Underworld cannot build: affiliation, step 20 level rule)? A task no planet bids on
     waits forever (AI_GALACTIC_2 §2.3 "no time-out"), which would hold the plan without building;
   - (d) whatever changes for NalHutta at 11059 (second NalHutta vehicle factory finished at 9623 / 9626; ground-base level
     5 from then; tech; game age 368.7 s).
2. **Underworld Build_Ground_Forces candidates (r3 diagnosis 2).** Before 9664 retail queues only MDU (Shola) and Merc
   companies. Which rule removes MAL / Destroyer / Canderous from the solve or vetoes them: the Budget marshaller with a
   smaller Offensive pool (then: how the Offensive category's available amount and rate are formed for the Underworld
   between 1351 and 4300), a Production / TechTree rule, or a per-planet rule?
3. **Saboteur entries removed after 8 / 105 / 118 frames** (tags 60, 65, 68): which plan-end path ends those plans that
   quickly (the r2 cancel rule, A2M_R2_RE_B §1, covers the refund, not why the plan ends).
4. **Underworld cycle-1 income** (section 4): which planet's corruption income retail counts at 1351 in the Empire-human
   campaign, and when its first saboteur lands (a probe question if RE cannot answer it).

A retail probe can answer 1(d), 2 and 4 directly: the a2f_budget kit with a human **Empire** (it is fixed to a human Rebel
today), logging the Underworld category pools, the UpgradeStarbasePlan / BuildGroundForcesPlan starts and the corrupted
planets every cycle to frame 12000.
