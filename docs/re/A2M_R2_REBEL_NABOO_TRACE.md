# A2m round 2: the Underworld's early Naboo corruption and sabotage (retail traces vs port experiments)

Written 2026-11-12 by the a2m r2 Rebel fix lane (branch `claude/a2m-fix-rebel-r2`, base dev b39baeb5). Sources: the
retail q2 traces `20261108-170824`, `20261030-061228`, `20261108-204711` (production.q2q, trace.q2t; identical through
tick 9600) and port runs with env-gated experiment switches that are **not** in the port. No decompiled code was read:
this is behaviour from traces, and the open questions at the end are for an RE task. Plan:
`plans/a2m_r2_rebel_diag.md` (session scratchpad), cause 1.

## 1. Why it matters for the Rebel

- Retail sabotages Naboo at 1809 and removes the Rebel mining facility (object 622, alive since 1493). Rebel income is
  then 637 every cycle until the Shola capture (GALACTIC_ECONOMY §1.3/1.4: the mine's ability row is 2 x base 90).
- The port keeps the mine in most seeds, so the Rebel gets +180 (or +150) a cycle from cycle 2 on, Conquer_Pirate @Shola
  passes its money test about two cycles early, and Shola falls at 5853..5934 (retail 7498..9898).

## 2. Retail Underworld sequence [M: three identical runs]

| frame | event |
|---|---|
| 64 / 80 | U_Ground_Palace queued at Felucia / Shola (602 frames each) |
| 147 | Underworld_Saboteur_Team queued at NalHutta |
| before 1340 | Naboo corrupted (the starting saboteur) |
| 1809 | Naboo sabotaged (second saboteur, via Shola 1691, Naboo 1803); the Rebel mine is removed |
| < 9000 | **no U_Ground_Barracks is ever queued**; first SB upgrade SB2@NalHutta at 11059 |

## 3. Port first divergence (dev b39baeb5, seed 3, Underworld goal trace) [H for the port]

- Infrastructure (category 3) holds Corrupt_Planet, the palaces and the barracks. At the pass-2 commit (119) the port
  accepts Corrupt_Planet @Corellia (free saboteur) and **Build_Underworld_Barracks @Felucia (desire 34.92, cost 1000)**;
  the next proposal, Corrupt_Planet @MonCalimari with a saboteur to build, then fails funding (296.6 left). Retail queues
  that saboteur at 147.
- The barracks goal is dropped one pass later (maintenance: elapsed ETA 68 > AllowedTime 17.28, the Felucia queue is
  busy with the palace) and re-proposed at the next pass with the same desire. Each time it outranks the running
  Corrupt goals: Corrupt_Planet @Naboo (24.64) is released "displaced" at 653 before its saboteur lands.
- Should_Build_Underworld_Barracks (AI_EQUATIONS_UNDERWORLD_GALACTIC.XML) is 9 x (NGU + 3) at Felucia unless
  Should_Underworld_Save_For_Desirable_Structures = (GroundBaseLevelUnnormalized > 1) x (VF > 15 + Palace > 15) or
  Should_Save_For_Corruption (needs 2 complete palaces, so 0 before 666) vetoes it. With the port's GroundBaseLevel
  (live ground-structure count, `GalacticSim::ground_base_level`, [M]) Felucia has 1 until its palace completes at 666.

## 4. Port experiments (12 seeds, hotm_galactic, harness distribution mode; not port behaviour)

| experiment | Underworld saboteur queued | Naboo corrupted / sabotaged | Shola captures (retail 4/4, 7498..9898) | harness FAIL (dev 18) |
|---|---|---|---|---|
| dev b39baeb5 | ~780 | 2145 / 4530 (seed 3) | 7/12, median 5934 | 18 |
| barracks desire forced to 0 | 157..181 | 384 / 1653 in most seeds | 9/12, median 5934 | 16 |
| quota 1 after the first pass (147-frame passes, a2m t11) | ~855 | 1563..1572 / - | 7/12, median 6246 | 24 |
| GroundBaseLevel + 1 while the ground queue's current item is a structure | 162..333 | 384 / 1653 in 2 of the 4 seeds checked | 2/12 | 26 |

- With the barracks out of the way the port reproduces retail's sequence closely (saboteur 147, sabotage 1809). The
  pass cadence alone does not.
- The sabotage target is a random affordable structure for the AI (P4_SIM_SYSTEMS §2), so the port removes the mine in
  only some seeds (Rebel income 637 from cycle 2 in 5 of 12 seeds of the barracks experiment, from cycle 4 in a sixth,
  788 / 818 in the others);
  retail's three runs are one draw sequence, so they cannot show that distribution. The Rebel mine also completes before
  the cycle at 1351 in half the port seeds (cycle-1 income 815 vs retail 634; retail completes it at 1493).
- So the barracks veto is necessary for the Rebel cause but not sufficient: early captures remain where the mine survives,
  and the never-capture seeds (Conquer_Pirate / Conquer_To_Reconnect only produce, Contrast "last veto Hero") are
  unchanged.

## 5. Open RE questions (for one RE task; none answered here)

1. **D+0x130 (GroundbaseLevel) writer [needed]:** what retail's ground-base level is at HotM start and around frame 100
   for Felucia (1 barracks, palace in production), NalHutta (palace, barracks, droid works) and Shola (VF, palace in
   production). Retail must veto Build_Underworld_Barracks @Felucia/@Shola from about 100 to 666 (no barracks is queued),
   yet NalHutta's starbase is not upgraded before 11059 (Needs_Starbase_Upgrade reads GroundbaseLevel vs StarbaseLevel).
   Shared with the Underworld r2 diagnosis cause 3.
2. If D+0x130 does not explain it: whether a Build_* goal whose plan was dropped by maintenance keeps a per-(type,
   target) activation failure that lowers its next desire (Per_Activation_Failure_Desire_Adjust, AI_GALACTIC §1.5),
   which the port may not record for a maintenance drop.
3. a2m t11: what Goal Service 0x6cacd0 counts as `evalsLastPass`, the first-pass quota and the player count of the cap
   (port passes take 77 frames, retail 147; section 4 shows this is secondary).
