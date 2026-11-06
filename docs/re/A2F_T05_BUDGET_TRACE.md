# A2f t05 / t04: the Offensive pool against Conquer_Opponent, and the Contrast solver (2026-11-05)

Scenario: HotM Underworld campaign, scripted human Rebel (`alamo_headless_galactic_ai_human`), the Empire AI (p1) attacks Naboo.
Base: `ddfe132` (clean tree) plus the engine/ai changes below. Tags as in the other `docs/re` pages.
Method: temporary stderr traces in a scratch tree (budget row per fiscal tick, PotentialPlan solve per proposal, equation values);
removed again, nothing of them is in the change. No Q2 trace holds the Empire AI (the human is the Empire there), so retail's Offensive pool
cannot be read; what the Q2 trace gives is the Rebel / Underworld credit curve and orders (see 4).

## 1. Result

| 72000 ticks, HotM-human | before (`ddfe132`) | goal-time fix (RE read 1.4 item 1) | + token relation + exact player lists |
|---|---|---|---|
| Conquer_Opponent accepted | 1 | 11 | 15 |
| rejected orders | 0 | 8 | 0 |
| planet owner changes | 1 | 6 | 3 |
| human planet captured | none | **Naboo, t43059** (Empire) | **Naboo, t49929** (Empire) |
| 36000 ticks (the ctest run) | 1 plan, 1 change | not run | 9 accepted, 2 changes (Felucia, NalHutta from the Underworld), no Naboo yet |

So the cause was not the budget. It was the time-drop stand-in (`galactic_maintenance_eta` added `elapsed` even when the plan's
production time was 0, so every plan with no unfinished build task was dropped as soon as `elapsed > AllowedTime`). With the original rule
(`if (0 < p) p += elapsed`, A2F_RE_READS §1.3) plans that already hold their units survive and land.
The token relation change (Neutral = 2) did not move this scenario at all (identical CRC), because no Neutral-held planet is involved;
the exact player-level member list moved it (11 -> 15 accepted, Naboo a little later).

## 2. The Offensive pool (trace, before the change)

Empire AI, 36000 ticks. Inputs read at every fiscal tick (every 1350 frames):
- Income per fiscal cycle 810 (rate 18/s), `Credit_Multiplier` not applied to the accrual input, as §3.1.
- Template `Basic_Empire_Default`: Offensive `BasicEmpireOffensiveBudgetAllocation`, Infrastructure `BasicEmpireInfrastructureBudgetAllocation`, Information, MajorItem; Defensive / Always / Hero / StoryArc are `Zero`.
- Normalised weights: Offensive 0.408 (0.421 at times), Infrastructure 0.485, MajorItem 0.107, Information 0 (`Global_Scout_Need` = 0).
  Offensive = 5 + 10 x PlanetsControlled + 6 x (1 - Has_Enough_Space_Force_Globally); `Has_Enough_Space_Force_Globally` = 1 (the
  caller-dropped-parameters leaf, Q2F_T09). Infrastructure keeps its large share because the Empire has no mining facility
  (+10 each for a missing light vehicle factory, barracks and mining facility).
- Offensive rate R = 7.35 credits/s, available A 0..1500: A fills for 2-3 cycles and is drained back to 0 by Build_Space_Forces (net 1250) / Build_Ground_Forces plans.
  Σ P equals the player's credits at every tick (the drift term works), refund accumulator 0.
- At the 303 Conquer_Opponent solves of the run the Offensive A was: 94 x < 250, 83 x 250-500, 47 x 500-750, 59 x 750-1000, 20 x > 1000. All 301 failures end with "last veto Budget".
  The failing candidates are the extra produced units (Star Destroyer class, 1.5k+): gross 7-15k is mostly free-store units (net 180-1400), the
  few produced units do not fit `money = horizon x R + A`.
- Accepted solves after the fix (28 of 214): avail 155-1700, rate 3-23/s, net 0-4560, eta 0-180 s. A plan is accepted exactly when the free store holds the force and net <= money.

Budget math itself (§3.1-§3.4) is unchanged and unchallenged: `ai_budget.cpp` reproduces the formulas, ΣP == credits, `can_fund` / `PP.money` use R and A as the spec.

Conclusion for t05: no concrete deviation in the budget math or its inputs was found that the spec could fix. Budget is the *last* veto because it is the
last constraint on units beyond the free store; the accepted-plan counts are decided by (a) plans being dropped by maintenance (fixed) and (b) the free
store's force against Naboo's strength (t03 work).

## 3. Sensitivity of the unknown reset flag (+0x74), for t07

Experiment (scratch only): set the budget reset flag on every income tick, so each tick redistributes P = Σ x w before the drift.
- Before the goal-time fix: Conquer_Opponent accepted 1 -> 29, but no Empire capture; the Underworld took the human's Yavin (t34148).
- After the fix: accepted 15 -> 4, only Felucia and NalHutta change hands. Worse.
So a per-tick reset is not the missing piece; the pools hoarding in Infrastructure (P 3555 at the first tick, 65 % of the credits) does not decide the
outcome. The template-switch hint in BASICEMPIRETEMPLATES.XML ("the template switch alone causes the entire budget to be redistributed") would
set the flag at Tech_Upgrade on / off only (the Empire switched at frames 1376 and 1829); not tested, left to the t07 probe.
Infrastructure plans also fail repeatedly early (Build_Base_Component_Heavy_Vehicle_Factory at Tatooine 14 times, an `Empire_Star_Base_N` order at Kashyyyk
16 times "production dropped"); that is the `build_check` area of t08 (engine/galactic), not the budget.

## 4. Q2 trace cross-check (HotM, human Empire, 13501 ticks, trace 20261103-152619)

Port Rebel / Underworld AI before vs after the three changes (`q2trace.py compare --port-exe`):
| | retail | before | after |
|---|---|---|---|
| Rebel fleet departures | 19 (sampled windows) | 33 | 22 |
| Underworld departures | 52 | 44 | 39 |
| owner changes | 0 | 1 (Shola t2121) | 0 (match) |
| planets owned every 1800 ticks | | diverged t3600 | match (7 samples) |
| Rebel production orders / queue starts | 32 | 12 | 8 |
| final credits Rebel / Underworld | 1686 / 3084 | 1940 / 2689 | 780 / 5804 |

Ownership now matches retail for the whole trace, and the Rebel departures are close. Rebel production orders are still far below retail (retail queues 15 Y-wing squadrons, 5 tank
busters; the port picks one Corellian corvette): that is the Build_Space_Forces unit choice (Q2F_T09 O1), not the budget; the Rebel's credits end below retail
and the Underworld's above, so the budget is not what starves production.

## 5. t04 (Contrast solver feeds over-satisfied rows)

Decision: not touched. The condition "touch only if the Rebel is still idle after t01-t02" is false: in the Q2 HotM trace the Rebel makes 22 departures against
retail's 19 and planet ownership matches; in HotM-human the Empire now conquers Naboo. Q2F_T09 F2 already fixed the per-entry rule that matters (a pair counts only when
remaining R > 0, 0x72b6a0); the remaining difference (remaining subtracted from the chosen entry only) has no spec reading.

## 6. Open

- ~~0x525200's AllowedTime base (A2F_RE_READS §1.4 item 2) is not applied~~: applied in A2f t09, see 7.
- Reset flag +0x74 setter and the pre-Init pool: t07 probe, still needed to rule the template-switch reading in or out.
- Rebel / Underworld production order count versus retail (Q2F_T09 O1).

## 7. A2f t09: AllowedTime base (2026-11-05)

Change (A2F_RE_READS §1.4 item 2): `GoalSystem::allowed_time` with a PP returns `Goal::nominal_time x Build_Time_Delay_Tolerance` for the galactic AI
(`GoalSystemConfig::nominal_allowed_time`), no PP: `Time_Limit` as before. `nominal_time` = 0x6c0450(PP): the float sum, in selection order, of
`btime_none(T)` over every selected type (free-store entries included) x the plan player's `Galactic_Build_Time_Multiplier` (sim player difficulty
index into `rules.difficulties`; 1.0 when none). Set at accept next to the ETA and refreshed when 0x6c4a50 re-solves the PP ([M]: the re-solve
rewrites the selection that 0x692860 reads live). Saved (goal record version 3) and hashed (`ai.goals`, galactic only; the tactical goal hash and
its ETA base are unchanged). The `at > 0` maintenance guard is kept. Before, the accept test could never fail (ETA <= ETA x tolerance); now a plan whose
queue-pool ETA exceeds its nominal time x tolerance is rejected, and a stalled one is dropped against the nominal base.

Same method as 1 (`--galactic-ai --human Rebel --human-script galactic_human_hotm.txt --ticks 72000`, counts from `--ai-log` and the proposal stats):

| 72000 ticks, HotM-human | t05 (`f13a592`) | t09 |
|---|---|---|
| Conquer_Opponent PP solved ("accepted" stat) | 15 | 9 |
| Conquer_Opponent plans started | 13 | 9 |
| rejected orders | 0 | 0 |
| planet owner changes | 3 (Felucia t777, NalHutta t4743, Naboo t49929) | 3 (Felucia t777, NalHutta t6447, **Naboo t36540**) |
| first Conquer_Opponent plan | t7392 (eta 64) | t24435 (eta 12) |
| Empire (p1) plans started / ended "failure (released)" | 475 / 48 | 501 / 96 |
| Empire fleet orders launch / move | 122 / 117 | 96 / 100 |
| Underworld (p5) fleet orders launch / move | 8 / 220 | 4 / 249 |
| final credits Rebel / Empire / Underworld | 20256 / 1099 / 3867 | 19446 / 4227 / 4279 |

The Empire takes Naboo 13389 ticks earlier with fewer Conquer_Opponent plans: the early long plans (t7392 eta 64, t9610 eta 180 with 19 produced
ships) are no longer accepted, so the Offensive pool is not tied up and the later plans that land are mostly free-store forces. The doubled
"failure (released)" count is Lift_Blockade (18 -> 31) and base-component builds (Heavy_Vehicle_Factory 15 -> 31, Advanced 0 -> 8, Barracks 3 -> 7):
single-structure plans whose ETA includes a queue wait now exceed their nominal build time x tolerance (`Normal_Default` multiplier 0.8 here).
36000 ticks (the ctest run): owner changes as before (Felucia, NalHutta), 8 Conquer_Opponent plans as before, orders 442 -> 397.

CRC columns (36000-tick HotM-human, t05 vs t09): `ai.goals` moves at tick 1 (the new hashed field), the synced `rng` at tick 557 (the AI's
draws after a different accept), then `objects` / `galactic.state` / `galactic.conflicts` at 768 from the different orders (the other columns follow later).
