# A2k t05: retail Underworld saboteur run 20261108-212556 (2026-11-08)

Retail run `tools/probe_mod/a2f_budget/out/20261108-212556/` (`run_a2f_sab.cmd`, README section 10): Heart of the Maw,
human Rebel, Medium, hands off, story on; channel U to frame 1410 (`UEND` / `SABDONE`), plan and Q records to about
1600. 4404 records, every canary deleted (all scripts ran), cleanup and profile restore verified, exit 0. Kit with the
2026-11-08 Flush_G recorder fix (records survive a pooled plan release). Analysis inputs: `t05_analysis.txt` in the
run folder; report section 9; `saboteur.tsv`, `plans.tsv`, `production.tsv`.

Port reference: `out/port_a2k_t05_d3237074/` (HEAD d3237074, build-wt-msvc `alamo_headless --economy --galactic-ai
--story --human Rebel --seed 1`, 1500 ticks, final CRC 405f8298c394ae53), plus re-runs of the same binary at shorter
tick counts for the credit table (section 4). Tags: [H] read from the run, [M] one inferred link, [L] hypothesis.

## 1. What retail did with the saboteur (frames 0..1600) [H]

| frame | event |
|---|---|
| 0 | Starting saboteur id 323 admitted to the free store at NalHutta. |
| 103 | Two Corrupt_Planet plans start in one pass: **@Corellia** (instance A, takes saboteur 323) and **@MonCalimari** (instance B, `unalloc` 1000 = a saboteur to build). No Naboo plan. |
| 120..240 | 323 in transit, alone: fleet 551, then 552 from 240, `cnt=1` at every sample. |
| 235 | Corellia: `Produce_Force(stage)` returns true; `Move_To(Corellia)` starts (`units=1`). |
| 241 | `FSAB st=false` (held by the task force). |
| 248 | Corellia plan **ScriptExit** (PEXIT, no PFAIL, no PMOVE, `tgtc=false`), 13 frames into Move_To; 323 re-admitted to the free store **in transit** (`FADD pl=nil`). |
| 330..390 | 323 sits at **Naboo** in fleet 552 (`cnt=1`, Naboo is Rebel). No `FSVC`: the store never serviced or moved it (its services at 241 and 481 bracket the stay). |
| <397 | The MonCalimari plan is released without an end record (`prev=none` at instance B's next start). No saboteur was ever queued for it. |
| 397 | Instance B restarts as Corrupt_Planet **@Naboo** (`unalloc` 0: uses 323); instance A as @Kuat (build). |
| 420..510 | 323 travels Naboo → **Shola** (at Shola 510, fleet 559, `cnt=1`); `Produce_Force` true at 517. So the Naboo stage is Shola [M]. |
| 616 | `Move_To(Naboo)` returns (result is a `GALACTIC_FLEET` GameObject, not true); plan PEXIT at once, `tgtc=false`. |
| 630 | Owned saboteurs 1 → 0: 323 consumed by the corruption. Naboo is corrupted by 1576 (Sabotage_Planet @Naboo starts with `tgtc=true`); first payment 2150 = 2105 + Naboo's 45, as in run 074145. |
| 691 | Instance B: Corrupt_Planet **@Yavin** (build); saboteur queued at **Shola** 731, done 1332 (id 606), produced at the stage 1429, Move_To returns 1495. |
| 1495 / 1576 | Corrupt_Planet @Tatooine (build, `prev=none`: Kuat released); Sabotage_Planet @Naboo (new instance). |

Desire functions (UD, raw, every 30 frames, 0..600) [H]: Should_Corrupt_Planet is constant for the targets connected to
corruption (MonCalimari 25.5, Naboo 24, Yavin 22, Kashyyyk 21) and **toggles between 0 and its value** for the others
(Corellia 41, Kuat 23, Kamino 18, Tatooine 16.5): non-zero at 30, 60, 120, 240, 270, 480, 510, 540, zero at the other
samples. Should_Sabotage_Planet is 0 for every planet through 600 (nothing corrupted yet).

Other Q5 answers: Q1, the saboteur's fleet is never merged (`cnt=1` at every sample, also after each stage arrival),
as the a2k t08 port of `arrive()` assumes [H]. Q2, Move_To moves that one-unit fleet [H]. Q3, `Get_Stage()` could not
be named (`stage=ERR` in all 10 PSTAGE / PMOVS records: it returns an AI target, which has no `Get_Type`); Shola for
Naboo is inferred from the route [M]; for Corellia the 235 stage arrival matches the port's NalHutta → Shola jump
time (225) [L]. `Is_Object_On_Free_Store` works in retail (FSAB records go on to 1453) [H].

## 2. Verdict on Q4.2: H1, in a form neither hypothesis named

- **H1 holds [H for the ranking, M for the cause]:** retail's first pass ranks a target the port never proposes. It
  is Corellia (41), not Naboo. Corellia outranks MonCalimari (25.5) and takes the starting saboteur by the Q4.1 rule
  (higher effective desire first). The cause is the clock term of `Can_Corrupt_Planet` (section 3).
- **H2 as stated is refuted [H]:** no Naboo plan existed when the saboteur came back to the store (248), and no
  pending build task claimed it at admission. MonCalimari's build task was pending then and did not get it either
  (the unit idled at Naboo, MonCalimari produced nothing and was released) [M]. Naboo got the saboteur through a
  **fresh proposal at the next pass (397)**, with the unit already at Naboo.
- The store-return part of H2 did happen, but through a different path: the Corellia plan ends by ScriptExit at 248.
  Which handler calls it (`Default_No_Units_Remaining`, `Default_Original_Target_Owner_Changed`, or an engine path) is
  not recorded (`by=nodebug`) [L].
- H3 is not needed.

## 3. The clock term of Can_Corrupt_Planet [M, strong]

`Can_Corrupt_Planet` (AI_EQUATIONS_UNDERWORLD_GALACTIC.XML) needs
`(IsConnectedToCorruption + (Game.Age > Variable_Target.TimeSinceCorruptionChange)) > 0`. For a never-corrupted planet
both clocks count from frame 0. The port computes both as `frame / 30.0` in double, so the term is always 0 and
non-connected planets are never corruption targets until something nearby is corrupted.

Retail's toggling fits one rounding rule exactly. All 1225 combinations of 5 formulas × frame offsets −3..3 per side
were tried; one reproduces all 21 samples (`t05_analysis.txt`):
- `Age = (double)f × (double)(1.0f/30.0f)`;
- `TimeSince = (double)(float)((float)(f − stamp) × (1.0f/30.0f))`, stamp D+0x2F0 = 0.

The same rule gives "on" at the plan-start frames 103 (Corellia), 397 (Kuat) and 1495 (Tatooine), and "off" at 691,
where retail picks Yavin (22) over Corellia (41). That is 4 of 4. About 49% of frames are "on".

The token table lists both leaves with lifetime 0 (live), so a cache cannot explain the pattern
(AI_GALACTIC_TOKENS.md: Age 0x6e1f20, TimeSinceCorruptionChange 0x65f110). The precision of each is not read yet.

Side effect: with that Age, `Game.Age > 120` turns true at frame 3600, the port at 3601. Every constant `Game.Age`
threshold in the equations moves by one frame.

## 4. Underworld credits, frames 0..1500 [H]

| frame | 103 | 250 | 397 | 544 | 691 | 847 | 1006 | 1171 | 1333 | 1495 |
|---|---|---|---|---|---|---|---|---|---|---|
| retail (cr= at plan start) | 7705 | 5534 | 4400 | 3420 | 1800 | 1133 | 338 | 0 | 0 | 2150 |
| port d3237074 (end of tick) | 7493 | 5338 | 4218 | 1932 | 869 | 515 | 250 | 0 | 0 | 2145 |

- Both run dry by 1171, and the first payment (1351) is equal within 5. So the credits_at_9000 gap (q4h 324 vs
  retail 2462..2849; a2m t01) does not exist yet at 1500, and the budget rule shows no fault in this window.
- The port spends earlier (544..847). Retail's Underworld production in 0..1500 is only these, identical to run
  074145: palaces 54 / 80, Crusader 97, Destroyer_Droid_Company @NalHutta 705, saboteur @Shola 731 and 1537. The port
  adds Build_Ground_Forces at 271 (MDU company @Felucia 330), retail has none, and builds its saboteur @NalHutta 782.
- What 0..1400 does show is a plan-choice divergence that compounds later:
  - corruption target: MonCalimari at 456 in the port, Naboo at about 620 in retail;
  - second saboteur: the port's goal is Kashyyyk (built at NalHutta), retail's is Yavin (built at Shola);
  - the port's Yavin plan runs at 271 and is released at 347.

  All of these follow from the section 3 term at the first pass. This supports a2m t01's "plan choice, not the budget
  rule" [M].
- Whether the gap closes needs the section 3 port and a q4h re-run. Frames 0..1400 cannot tell.

## 5. Earlier findings that were recorder artefacts

Before the fix, the second and later runs of every pooled instance of a hooked plan died at their first record
(README fix note). Their PSTART strings survived only as orphans. This run records every restart (`prev=PEXIT` /
`prev=none` chains, FlushMajorItemBudget n = 1..8, each with PEXIT). Against that:

- **Artefacts:**
  - README section 8: "each Empire plan instance after its first sample (its later PSTART lines have no sample after
    them)". This is the recorder bug, not an equation failure.
  - README section 8: the plan-channel sequence gaps of 20261105-195851 ("cause unknown"). Flush_G deleted the
    records table.
  - `A2H_T02_FLUSH_PROBE.md` section 2 (run 040725): "every start ended without PRET / PEXIT / PFAIL: the goal system
    released them". No end record is not evidence of a release there, and the `confirmed` (`prev=none`) column is not
    either: Flush_G wiped the previous-run state too.
  - The ConquerPirate restart cadence (~240 frames) and the start counts of 040725 / 195851 (`A2G_T01_RESET_FLAG_PROBE.md`
    section 3) are from plans that were dying at once, so they are disturbed.
  - The Underworld Flush count "21 starts, 847..7198, no PEXIT": retail starts it from frame 52 and every run ends
    with PEXIT in the same frame.
- **Suspect, needs a long fixed run:** in runs 040725 / 074145 / 195851 the AI was disturbed after each hooked plan's
  first release. That covers these long-horizon retail statements:
  - "no Underworld ConquerPirate before 54583" and "Kashyyyk stays with the Empire" (`A2J_T02_T04_PROBE_RESULTS.md`
    section 1, `A2I_T03_TRAJECTORY.md` section 5.2);
  - per-cycle P / I after the first cycles.

  Tonight's run agrees only within 0..1600 (no Underworld ConquerPirate start).
- **Still valid:** the 0..1500 production (D3) and perception (D4) findings of 074145. This run reproduces its
  production frame for frame for both AIs (Empire 102 / 102 / 139 / 612 / 619 / 666 / 1487; Underworld above). The
  story-channel stop of 040725 was a real equation failure (`A2I_T02_PROBE.md`); that script is not pooled.

## 6. Port vs retail differences

| # | Difference | Port code | Evidence |
|---|---|---|---|
| 1 | `Game.Age` and `TimeSinceCorruptionChange` are equal double clocks in the port, so non-connected planets are never corruption targets. Retail mixes float and double; Corellia wins the first pass | `engine/ai/src/galactic_ai_tokens.cpp:2045` (AGE), `:1532-1533` (TIMESINCECORRUPTIONCHANGE), `:654` (stamp 0) | [M] section 3 |
| 2 | First saboteur pass: port 119 MonCalimari (free) + Naboo (build); retail 103 Corellia (free) + MonCalimari (build) | consequence of 1 (desires); pass frame: goal service `engine/ai/src/ai_context.cpp:45` | [H] |
| 3 | Underworld plan-start frames: retail 52, 103, 250, 397, 544, 691, 847, 1006, 1171, 1333, 1495; port 42, 119, 195, 271, 347, 500, 654, 815, 976, 1138, 1300, 1462 (Flush_MajorItem_Budget: retail 52 / 250 / 544 / 847 then about every 160; port about every 153 from 42) | goal proposal timing, not located (`ai_context.cpp:45` → `goals.service`) | [H] frames, cause open |
| 4 | Retail plan ends by ScriptExit 13 frames into Move_To (Corellia, 248) and the released in-transit unit stops at the next planet (Naboo). The port has no such end in this window | `engine/ai/src/galactic_ai.cpp:1464` (MoveTo status), unit release `tf_release_unit` | [H] retail, cause [L] |
| 5 | Admission claims by pending build tasks are not modelled in the port. This run shows no retail claim either (MonCalimari's pending task did not take the readmitted unit) | `engine/ai/src/galactic_build.cpp:146`, `galactic_ai.cpp:1186` | [M], so no change yet |
| 6 | `BlockOnCommand(Move_To(...))` result: retail returns the fleet GameObject, the port returns true / false (no shipped script compares it with `== true`, so no behaviour change) | `engine/ai/src/galactic_ai.cpp:1290-1294` | [H] |
| 7 | Early Underworld Build_Ground_Forces (271, MDU @Felucia) and the earlier spend (544..1006); retail's first ground unit is the Destroyer company at 705 | plan choice (a2m t01) | [H] |

## 7. Open items / proposed follow-up tasks

1. **[RE]** Read Game.Age 0x6e1f20 and TimeSinceCorruptionChange 0x65f110: the float or double precision of each,
   which frame each reads, and D+0x2F0's initial value and writers. Also read the other TimeSince* leaves
   (0x65f1b0 / 0x65f160 / 0x65f0c0). This confirms or refutes the section 3 fit (docs/re only).
2. **[port]** After 1, give `AGE` / `TIMESINCECORRUPTIONCHANGE` retail's precision with explicit float casts
   (deterministic on MSVC and clang). Expect:
   - the first Underworld DeploySaboteur pass takes Corellia;
   - the starting saboteur corrupts Naboo at about 620;
   - the saboteur is built at Shola for Yavin;
   - goldens move.

   Then re-run the q4h baseline (Underworld credits_at_4500 / 9000 / 13500) and the a2m t01 bisect on top.
3. **[RE]** Why retail's Corrupt_Planet @Corellia plan ends by ScriptExit at 248, 13 frames into Move_To: the event
   and handler, or an engine path. Also: does a unit released in transit stop at the next planet?
4. **[port check]** Underworld proposal cadence: retail plan starts at 103 / 250 / 397 / 544 / 691 (147-frame steps),
   the port's at 119 / 195 / 271 / 347. Also Flush_MajorItem_Budget proposal timing.
5. **[kit]** PSTAGE / PMOVS: name the stage through the AI target (`tostring(stage)`, or the target's game object)
   instead of `Get_Type`; extend the UD samples past 600. Then one 30-minute run with the fixed recorder (could share
   Gabriel's a2m t07 session) to re-check the section 5 long-horizon statements.
6. **[docs]** Mark the section 5 artefacts in the README section 8, `A2H_T02_FLUSH_PROBE.md` section 2,
   `A2G_T01_RESET_FLAG_PROBE.md` section 3, `A2J_T02_T04_PROBE_RESULTS.md` section 1 and `A2I_T03_TRAJECTORY.md`
   section 5.2 as pre-fix (Flush_G) data.
