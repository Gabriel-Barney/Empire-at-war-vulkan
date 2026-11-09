# s78m t06: retail Tactical_Bribe run 20261108-215525 (2026-11-08)

Retail run `tools/probe_mod/s78m_bribe/out/20261108-215525/` (`run_s78m.cmd`, kit README): Dagobah land skirmish,
human Underworld vs Empire AI, hands off, scripted trials 1-3 and the optional manual window (not used). 3236 records,
`DONE` at f=7227, cleanup and profile restore done. The report was rebuilt offline with the 2026-11-08 parser
(`s78m_run.py --report-only`): price corrected for income, range thresholds, affordable window. Spec:
`TACTICAL_COMBAT.md` §5.4.1. Frames are mode frames read at the free-store poll (every 3 frames), so ±3.
Tags: [H] read from the run, [M] one inferred link.

Setup: invulnerable `Tyber_Zann` spawned for the human at (3760, 1200), bribe autofire off;
`Imperial_Stormtrooper_Squad` spawned for the AI per trial (team A = target, team B = invulnerable control).
Skirmish income is **+1 credit per poll** (every 3 frames): 2153 of the 2155 credit edges are +1.00. The other two are
the payments.

## Verdicts

| # | Question | Retail | Port | Verdict |
|---|---|---|---|---|
| Q1 | price | net −24.00 on the payment poll in both trials (f=531, f=2265); every poll within ±15 frames shows +1.00, so the poll holds one income tick: **price 25** [H]. `Get_Bribe_Cost(tyber)` = 25.000 for `Stormtrooper_Team`, `Squad_Stormtrooper`, `Stormtrooper` and both objects: the skirmish planet counts as uncorrupted | 25 × 1.0 | MATCH |
| Q2 | whole team | trial 1: 6/6 members + container; trial 2: 9/9 + container; control team B untouched in both | container + every live member | MATCH |
| Q3 | timing | order f=528 → payment, ready drop and all 7 owner changes on the same poll f=531 (≤ 3 frames, no animation wait). Payment is not inside the `Activate_Ability` call (credits unchanged on return) | one frame, at the next targeting service | MATCH |
| Q4 | recharge | ready again 1500 frames after the payment (531 → 2031; 2265 → 3765) | 1500 | MATCH |
| Q5 | container as target | `Activate_Ability(container)` returned nil, Tyber did not move, no conversion in 60 frames; the member order 60 frames later walked and converted | refused | MATCH |
| Q6 | range | conversion at **centre distance 108.8–113.5** (below) | XY centre distance ≤ 100 | **DIFFERS** |
| Q7 | too few credits | credits 5.00 at the order (f=3843): no payment, no owner change, no recharge. Income lifted the credits to 25 at f=3903; for 10 polls (f=3903..3930, credits 25..34) Tyber was ready, team A alive and 60 away, and **no bribe**: the refused order is used up [H] | no pay / convert / recharge, order used up (`ability_classes.cpp:652-655` returns true) | MATCH |
| Q8 | by hand | not done (optional) | - | - |

Q7 answers the t05 corner "whether a refused bribe uses up the order" (`TACTICAL_COMBAT.md` §5.4.1, "[M: whether that
consumes the order]"): **it does**. Team A then died at f=3933 (all 8 members and the container on one poll, 90
frames after the order; it is not invulnerable), so the kit's "converted after the money came back" check (restore at
f=4143) is void and the f=3903..3930 window is the evidence.

## Q6 range (trial 2)

Target = member 69817 at (4010, 1200), the same point as the team container 69835; nearest member 69818 at
(3995.5, 1207.8). Tyber stood still after the container order (f=2109) and walked from the member order (f=2169) along
y = 1200 at 1.567 per frame [H]:

| poll | Tyber x | centre to target / container | nearest member |
|---|---|---|---|
| 2259 | 3891.8 | 118.2 | 104.0 |
| 2262 | 3896.5 | 113.5 | 99.4 |
| 2265 (conversion seen) | 3901.2 | 108.8 | 94.7 |
| 2274 (stopped) | 3908.2 | 101.8 | - |

Tyber braked after the conversion and stopped at 101.8: the centre distance never reached 100. So the retail test
fires at a centre distance in [108.8, 113.5) when the pending order is retried every frame, or up to about 118 if a
retry lags up to 3 frames (trial 1 bounds the order → conversion delay at ≤ 3). Port radii (`unit_params.cpp:81-86`:
`Custom_Soft_Footprint_Radius` × `Scale_Factor`): Tyber 6.5 × 1.75 = 11.4, `Squad_Stormtrooper` 3.0 × 1.5 = 4.5.

| Rule | Centre threshold | First in range | Fits? |
|---|---|---|---|
| centre ≤ 100 (port), target or container | 100 | never | no |
| centre ≤ 100 + target radius (the weapon rule) | 104.5 | ≈ f=2270, after the conversion | no |
| centre ≤ 100 + user (Tyber) radius | 111.4 | f=2264 | yes, 0-1 frame lag |
| centre ≤ 100 + both radii | 115.9 | f=2261 | only with a 2-4 frame lag |
| nearest live member of the team ≤ 100 | 114.2 | f=2262 | yes, 1-3 frame lag |

The run rules out the port's rule but cannot pick among the last three. `Activation_Max_Range` is 100.0 in
`Tyber_Tactical_Bribe` (`UNITS_HERO_UNDERWORLD_TYBER_ZANN.XML`, `Activation_Min_Range` 0.0, no `Must_Close_Range`
tag), so the XML value is right and the formula is the gap.

### Port changes

1. `engine/tactical/abilities/src/ability_system.cpp:805-814` (`in_range`, the `Activation_Max_Range` branch at :812):
   bare XY centre distance. It needs the radius term of `Target_In_Range` 0x48d820 once RE pins it (provisionally the
   user's radius, the best fit here). `in_range` is shared by every ability with `Activation_Max_Range`, and so is
   0x48d820 in retail (some 25 callers), so fix it in one place. Goldens may move.
2. `engine/tactical/abilities/src/ability_classes.cpp:645-647`: the bribe tests range on the target only ("[port] on
   the target only"). §5.4.1 says `Target_In_Range` on T (the team container) **or** X (the member). Test both; with
   a member-wise rule for a container this is the "nearest member" candidate.
3. The walk-to-range must use the same test, so Tyber stops where retail fires (retail stopped at 101.8 after braking).
4. No change for Q1-Q5 and Q7. The comment at `ability_classes.cpp:649` "[M: whether the order is consumed]" can
   cite this run.

## Kit lessons

- The first attempt `out/20261108-214221` read object ids with `Get_ID`, which is a PlayerWrapper method: every id
  was `?`, trial 1 found no team, then a nil `fact` raised an error every tick (2056 `ERR`) and no bribe was ordered.
  Fixed in the kit: `Get_Object_ID`, team matching by parent and position, per-trial error and time limits.
- Skirmish income lands on every poll: a credit drop on one poll is price − income. The parser now adds the
  neighbouring polls' income back.
- An unprotected target team can die before a late check. Judge "order used up" on the affordable window, not after
  the restore.

## Follow-up tasks

- [RE] `Target_In_Range` 0x48d820 / `Must_Close_Range` 0x48e100 with `Activation_Max_Range` set: the exact XY test
  (user radius, target radius, both, or member-wise for a team container) and the pending-ability retry cadence.
  Record it in `TACTICAL_COMBAT.md` §5.4.1 and mark the Q7 [M] there as confirmed (s78m t06).
- [port] Bribe range: `in_range` (`ability_system.cpp:805-814`) gets the radius term from the RE task; the bribe
  apply (`ability_classes.cpp:645-647`) tests T or X; the walk-to-range uses the same test. Unit test: Tyber 111 from
  a trooper's centre converts, 117 does not (adjust to the RE result). Regenerate goldens if they move.
- [probe] Only if the RE leaves it open: a static range ladder. Squad spawned with the target member's centre at
  104 / 108 / 112 / 116 / 120 from a ready Tyber, once targeting the nearest member and once the farthest; record whether he converts without
  walking. Needs Gabriel.
