# A2j t02 / t04: retail probe results (Heart of the Maw, Rebel human, 2026-11-06)

Run: `tools/probe_mod/a2f_budget/out/20261106-074145`, the a2f_budget kit with the a2j channels D (perception) and Q (production events), lane E's kit
(`A2J_T02_PROBE_PLAN.md`). Claude drove it: Heart of the Maw, REBEL, Medium, hands off, and two Yavin battles auto-resolved (space and land, about day 8).

**Coverage.** Galactic day 19, frames 0..24772, about 13.8 min of galactic time out of the planned 30. The recorder (the kit's scanner in a Claude terminal tab)
stopped when the Claude session was cut off at 08:00; the game then lost focus and paused. Gabriel chose to stop there.
- t02 D4 window (0..600) and the D3 window (0..1500): fully covered.
- t04: 18 fiscal cycles of the planned 40.

Port side of the comparison: lane E's reference run of commit 5509c2a (a2i), 54000 ticks with the perception trace (`port_reference_a2j.tsv`, `a2j_port.py`). It
predates the a2j market fix (lane A, 9e0dd0e).

Confidence tags: [H] read directly from the run; [M] inferred; [L] hypothesis.

**Pre-fix recorder (a2m t12).** This run (074145) and run 040725 were recorded before the kit's 2026-11-08 Flush_G fix
(`tools/probe_mod/a2f_budget/README.md`, "a2k t05 fix"). Until then, the second and later runs of every pooled instance of
the hooked plans died at their first record, so the AI was disturbed after each hooked plan's first release. Sections 1
and 3 mark what that makes suspect. Still valid: the D4 perception findings and the D3 production in frames
0..1500. Run 20261108-212556 (fixed kit) reproduces that production frame for frame. See
`A2K_T05_SABOTEUR_RUN.md` section 5.

## 1. D4: Underworld -> Kashyyyk (report section 6)

| frame | retail SCP | port SCP | retail markup | port markup |
|---|---|---|---|---|
| 0..210 | -0.3067 | -0.3067 | 0 | 0 |
| 240 | -0.0133 | -0.0133 | 0.2933 | 0.2933 |
| 270..360 | 0.3111 | 0.3111 | 0.6178 | 0.6178 |
| 390..1200 | 0.3111 | **0.7056** | 0.6178 | **1.0122** |

- Every non-markup term is equal in both games through frame 24600: GPV 0.5867, CIP 1, ONF 1, ACT 1, TR 1, IGN 0 [H].
- **The first two markups match exactly**: Kashyyyk's own value at 240, then the Kuat path at 270 [H].
- **The port adds a third markup at 390** (the Corellia path, PathBonusForDistantTargets.lua). Retail never adds it in 0..24600 [H].
- **Retail's desire is positive (0.311) from 270 on, yet retail starts no Underworld ConquerPirate** before 54583 (run 040725), and Kashyyyk stays with the Empire in this
  run [H]. So the sign of the desire is not what decides it. Either retail's accept step rejects a desire of 0.311 that the port's accepts at about 0.7 (a threshold, or
  the competition at commit step 5), or the force / plan solve fails in retail [M for "accept side", L for which].
  **Pre-fix recorder artefact (a2m t12):** "no Underworld ConquerPirate before 54583" and "Kashyyyk stays with the
  Empire" are suspect. Both come from pre-fix runs whose AI the kit disturbed after each hooked plan's first release.
  The fixed-kit run 20261108-212556 agrees only within frames 0..1600 (no Underworld ConquerPirate start). They need a
  30-minute run with the fixed kit (`run_a2f_sab.cmd --sab-long`). See `A2K_T05_SABOTEUR_RUN.md` section 5.
- Empire -> Felucia: equal (5.94, then 6.25 after one markup). Retail's markup arrives 30 frames later (390 vs 360) [H]; no consequence seen.
- Later rows (port only, after its Kashyyyk conquest) are not comparable.

**Port work implied (a2k):**
1. Find why the port's `Distant_System_Path_Bonus` applies the Corellia-path markup. Check the path `Find_Path(_FindStageArea, planet)` returns and the one-planet-per-`Sleep(1)` loop
   against retail's two steps.
2. Trace the accept decision for UW Conquer_Pirate @Kashyyyk with desire 0.311 (the retail value) to see whether the port would still accept it.

## 2. D3: who starts what in frames 0..1500 (report section 7)

| frame | retail | port at 5509c2a (A2I_T03 run3) | port after the a2j market fix (lane A) |
|---|---|---|---|
| 54 / 80 | UW U_Ground_Palace @NalHutta / @Shola | 62 / 62 | 53 / 54 |
| 97 | UW Crusader @Felucia | 93 | 99 |
| 102 | **Empire Turbolaser_Tower_Defenses @Tatooine (ground) and Broadside @Corellia (space)** | AIB @Tatooine (93) | AIB @Tatooine (89) |
| 139 | Empire Tartan @Kuat (space) | | |
| 612 / 619 / 666 | first Empire ground units: Heavy_Scout @Kuat, Stormtroopers @Kashyyyk, @Corellia | 93 / 186 | |
| 705 / 731 | UW Destroyer_Droid_Company @NalHutta, Saboteur_Team @Shola | | |
| 1487 | Empire E_Ground_Light_Vehicle_Factory @Kashyyyk | | |

- Everything below is [H] unless marked.
- **Retail's Empire starts no ground unit before frame 612.** Its first three items are the Turbolaser (Infrastructure) and the two space ships
  (Offensive). That is the build A2I_T03 §3.2 inferred.
- In the port, the Tatooine queue goes to a ground-unit task made at 43. That task is a Build_Ground_Forces / Conquer_Pirate unit (lane A, `A2J_T01_BUILD_MARKET.md` §5).
- So D3's root cause is in goal and plan choice, not in the market: the port's Empire plans early ground forces that retail does not plan until about 600 [H for
  retail, M for "plan choice"]. Lane A's market fix (queue phases, bound tasks) is still right, since its RE spec is [H], and it already moves the
  Underworld's start frames to within 1–26 frames of retail.
- The phases match retail's assumption: the Empire's Tatooine ground queue and Corellia space queue both run at 102 [H].
- `cr=8000` at 102 confirms that retail spends nothing before 102.
- Each Begin in this run fell on an idle queue, so this probe cannot tell whether Begin is the enqueue or the build start.
- The Heavy Vehicle Factory never starts in 0..24772 in retail either, as in the a2f runs.

**Port work implied (a2k):** trace why the port's Empire has Build_Ground_Forces / Conquer_Pirate ground tasks at frame 43, and retail none until 612: desire, the
budget at commit, and the template.

## 3. t04: per fiscal cycle P / I (report section 8)

**Pre-fix recorder artefact (a2m t12):** retail's per-cycle P / I after the first cycles is suspect. The kit disturbed
the retail AI after each hooked plan's first release (header note; `A2K_T05_SABOTEUR_RUN.md` section 5).

- **Empire, cycle 1 (frames 1351..2700) [H]:** the per-category income rates are equal: Offensive 5.76 / 5.76, Infrastructure 7.42 / 7.42, MajorItem 1.66 / 1.66. The 667.5
  payment is equal, as lane B found (`A2J_T03_INCOME.md`).
- **Empire, from cycle 2 [H port, M cause]:**
  - Retail stays at 667.5 for all 18 cycles; the port pays 702.5.
  - From cycle 3 the port's MajorItem rate jumps to 12.84 (retail 1.66) while Infrastructure falls to 1.04 (retail 7.42). That is a template / fraction switch in
    the port that retail does not make within 13.8 min.
  - The port's Empire also holds 1–3 times less cash from cycle 5 on.
  - This is the trajectory gap, and its likely source is D3 / D4: the port's Empire conquers NalHutta and Shola (lane B), retail's does not.
- **Underworld [H]:**
  - The Offensive and Infrastructure pools are 2–10 times larger in the port at every cycle start (for example cycle 1: Offensive 103 / 1307, Infrastructure
    405 / 1421).
  - The port's credits are higher at most cycle starts.
  - Retail's Underworld spends Offensive down to 0 by about the end of most cycles; the port's keeps 1300–2400.
  - Retail's first payment is 2150 against the port's 2105: the 45 is Naboo's Intimidation share, per lane B.
- **Not covered:** cycles 19..40 (the rest of the 30 minutes).

## 4. Doc corrections (from lane B, `A2J_T03_INCOME.md`)

- A2I_T03 §1 L2 and §4.4 item 3: the port's 585 is the Kashyyyk loss (82.5 per cycle), not a gap with "the same four planets".
- GE 2.8: the starbase-owner reset belongs to Change_Owner and the revolt (through 0x4bda40), not to the derived-owner change 0x3fb040.
