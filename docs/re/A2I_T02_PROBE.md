# A2 i t02: a2f_budget story channel fix and per-cycle P / I comparison (2026-11-06)

## 1. Why the story channel stopped after LOAD

- **Symptom.** In run 20261106-040725 the kit wrote LOAD / DEF and then nothing more from the story channel: no story samples and no LONGDONE.
- **Cause [H].** The story script asked for `A2FB_ActiveGoals` (key AG) with no `Parameter_Category`. In retail that evaluation fails (A2H_GALACTIC_AI_RE.md §1, A2I_TOKENS_RE.md t07).
  - A failed engine evaluation in a retail Lua script does not raise a Lua error.
  - Instead it sets the script's failed flag (byte +0x121), and the scheduler stops running the script.
  - `pcall` cannot catch it, so the first story sample silently ended the channel.
- **Fix** (`tools/probe_mod/a2f_budget/`):
  - `A2FB_ActiveGoals` is removed from `a2fb_spec.py`.
  - Every engine read now goes through `A2FB_Rec` (`a2fb_core.lua`), which wraps it in `pcall` and writes a one-time ERR record per key.
  - The story loop pcalls each player separately (`a2fb_story.lua`).
- **Test.** `selftest_a2f.py` now emulates the retail failed flag. The old kit fails 29 of 101 checks under it; the new kit passes 101 / 101.

## 2. Run 20261106-053820 (in game, Empire, Rebel entry)

- **Coverage.** Frames 0..7525, about 4 minutes (5 fiscal cycles): S 5499, F 283 and P 69 records, with 650 Empire samples.
- **Story channel works.** Samples arrive every cycle and the run ends with LONGDONE.
- **Clean exit.** `run_status` is done, exit 0, and cleanup restored the mod folder and profile.
- **Why it stopped there.** The planned 30-minute run was not made: Gabriel stopped computer use mid-run. A longer run moves to the follow-up group.

## 3. Port vs retail

Port run: HEAD (a2h) traced export, `alamo_headless --story --ticks 7600`, `ALAMO_A2G_TRACE=3`. Compared with:

```
python tools/analysis/a2g_port_vs_retail.py budget --port-field P \
  --retail-samples tools/probe_mod/a2f_budget/out/20261106-053820/samples.tsv --port-trace rb.err
```

New in this task:
- `a2g_trace_patch.py` now prints each category's income rate I.
- `a2g_port_vs_retail.py budget --port-field {A,P}` picks which port field to compare. The retail columns are P / I (BudgetFractionToBuild reads pool + income rate), so `P` is the right one.

**Init (frame 0 / 1): matches.**

| Category | retail P | port P |
|---|---|---|
| Offensive | 3105.3 (0.3882) | 3105.7 (0.3882) |
| Infrastructure | 4000.3 (0.5000) | 3999.9 (0.5000) |
| MajorItem | — | 894.4 (0.1118) |

- Offensive and Infrastructure agree within 0.4 credits, with identical weights.
- So the a2h fixes (SpaceTotal without galactic starbases; Maintenance) hold in game.

**Fiscal payments.**
- **Retail:** a constant 667.5 per 45 s (14.83 / s).
- **Port:**
  - 585 at frames 1351 and 2701;
  - 667.5 at 4051;
  - 1055.5 at 5401;
  - 945.7 at 6751.
- [M] The port's income first runs 82.5 below retail's, then above it. That points to a different set of owned planets over time: the trajectory (t03). The planet lists were not compared here.

**Per cycle P, first sample after each payment (retail | port).**

| k | frame | OFF | INFRA | MAJOR | credits at end |
|---|---|---|---|---|---|
| 1 | 1351 | 288.3 \| 2231.6 | 591.9 \| 3526.5 | 894.3 \| 157.7 | 1965.9 \| 2001.9 |
| 2 | 2701 | 329.4 \| 381.5 | 0.0 \| 1977.1 | 969.0 \| 226.6 | 2633.4 \| 2053.3 |
| 3 | 4051 | 588.5 \| 1174.7 | 333.8 \| 1286.5 | 1126.9 \| 259.4 | 2864.1 \| 3401.3 |
| 4 | 5401 | 412.6 \| 2559.7 | 667.6 \| 1637.0 | 1189.4 \| 259.4 | 2368.3 \| 676.1 |
| 5 | 6796 | 192.5 \| 409.2 | 986.3 \| 773.6 | 1189.4 \| 360.5 | 2754.7 \| 1045.9 |

**What the table shows.**
- **Credits at payment.** In both games the category pools add up to the player's credits (retail at frame 1329: 288.3 + 591.9 + 894.3 = 1774.6 credits; port `sumP` = credits).
  - Retail spends about 6250 of its starting 8000 credits before the first payment: credits fall steadily from 2978 at frame 926 to 1750 at frame 1337.
  - The port spends only about 2670: it holds 5331 credits before the frame-1351 payment.
  - The pools then follow the credits. The first lead for t03 is that the port's Empire starts fewer or cheaper builds in the first 45 s.
- **MajorItem.** Retail keeps adding to MajorItem every cycle (894 → 1241). The port's MajorItem drops to 158 at the first payment.
  - This fits a2h's finding that retail never flushes MajorItem (Needs_More_Space_Force_Globally = 0).
  - It also fits the port's Needs_More_Space_Force_Globally = 1 in part of the early samples, which come from its divergent trajectory.
- **Conclusion.** These differences come from the game trajectory, not the budget arithmetic. Init and the fiscal formula match. The trajectory itself is a2i t03 (A2I_T03_TRAJECTORY.md).

## 4. Open

- [M] A 30-minute in-game run with the fixed kit, compared over more cycles after the t01 / t03 changes.
- [L] Retail's frame-0 sample has no MajorItem row, while the port's Init has one (894.4). This is probably sample timing in the kit; not checked.
