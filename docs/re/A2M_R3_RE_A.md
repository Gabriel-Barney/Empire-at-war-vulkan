# a2m round 3, RE answers A: Underworld starbase upgrade, ground forces, saboteur plan ends, cycle-1 income

Clean-room RE answers for the blocked a2m round-3 Rebel fix lane (2026-11-12). The questions are those in the r3 RE
request and in `docs/re/A2M_R3_REBEL_UW_TRACE.md` §6 on `claude/a2m-fix-rebel-r3` (fca618d9). The decompiled output was
read for this note. It holds behaviour only, no code. Tags: [H] read in the code, [M] inferred, [L] guess. Data facts come
from the shipped FoC XML (`extracted/foc_data_text.zip`).

Retail evidence is from the three HotM human-Empire traces in `tools/probe_mod/q2_traces/out/`, mainly run
`20261108-170824-galactic-vanilla-lua` (`trace.q2t`, `production.q2q`, `lua_records.txt`). The other two runs agree on
every event used here, so the three runs count as one sample. Port evidence is from the r3 lane's seed-0 dumps (`pp.txt`,
`goals.txt`).

Corrections to existing docs (made in this commit):
- `AI_GALACTIC.md`, budget, "Can fund": the accept-time fund test (slot 10) is 0x6139f0 and reads **P / I** (pool and
  income rate). The A / R test 0x6129c0 is used only by commit, resolve and maintenance. See §1.1.
- `AI_GALACTIC_2.md` §1.2: the generic-to-planet replacement 0x3f8fd0 is now spelled out (§1.2 below). It returns the
  planet's **next-level** starbase, makes no can-build test, and dedupes every expanded team type into one candidate.

Short answers:

| # | Question | Answer | Confidence |
|---|---|---|---|
| Q1a | Accept test for Upgrade_Starbase | Slot 10 accepts when a plan solves and the gross test 0x6139f0 passes: `P + max(min(ETA, TL), T) × I ≥ goal cost`. There is no "upgrade available" test of its own. The port accepts with A / R (0x6129c0), which is stricter. | rule [H]; effect on 11059 [M] |
| Q1b | What 0x3f8fd0 returns | The next-level starbase of the starbase already on the target planet (Underworld_Star_Base_1 → Underworld_Star_Base_2), after a level gate. On a planet with no starbase: Rebel_Star_Base_1 / Empire_Star_Base_1 for those two factions only, otherwise nothing. IgnoreTarget plans still use the goal's target planet. | [H] |
| Q1c | Type queued at run time; can a task wait forever? | The replaced type. A bound structure task never bids and is never released, but it fails as soon as 0x2804d0 refuses the type at its planet, so it cannot hold the plan forever. | [H] |
| Q1d | What changed by 11059 | Not settled by RE. The strongest lead is the goal-set truncation: competing Infrastructure goals that retail accepts and the port rejects. | [M/L]; needs a retail run |
| Q1e | Budget category and cost | Infrastructure. Budget marshaller uses cost(T, target) and btime(T, target), clamped to Time_Limit 30. The ETA uses the space queue, which is empty at NalHutta, so the r2 queue-wait rule does not gate it. | [H] |
| Q2 | What keeps MAL / Destroyer / Canderous out before 9664 | No admission rule. The Budget marshaller vetoes on the Offensive category's A / R, and the contrast float decides which of the affordable units are taken. Queued but unpaid PAYG items keep their allocation reserved out of A until paid. | rule [H]; per-frame amounts [M] |
| Q3 | Saboteur plans that end 8 / 105 / 118 frames after queuing | Tag 65 (2839): blockade by Rebel fleet 675 at Shola → Production_Failed → script exit → cancel with refund [M]. Tags 60 / 68: a goal-system release that reclaims the build [M]. The maintenance time drop cannot cause it. | [M]; needs a retail run |
| Q4 | Which planet pays at cycle 1 (1351) | Naboo, corrupted by the starting saboteur at 617. 2150 = 2105 base income + 90 × 0.5 (Intimidation). Retail corrupts nothing else until MonCalimari at 10802. | [H] trace |

---

## 1. Q1: the Underworld Upgrade_Starbase goal

### 1.1 Accept (slot 10, 0x6b8660) [H]

1. Find_Plan_For_Goal (0x6bae50) picks a plan and solves its PotentialPlan. If no plan solves, the goal is rejected.
2. If AllowedTime (0x692860) ≤ 0, or AllowedTime ≥ the goal ETA (+0x7c), the gross fund test 0x6139f0 must pass:
   - If the budget is switched off for the player, the test passes.
   - If the goal already has a budget record (0x612bd0), the test uses that record's own horizon and request.
   - Otherwise: horizon = goal ETA, replaced by Time_Limit when Time_Limit ≠ −1 and Time_Limit ≤ ETA. Then
     horizon = max(horizon, budget +0x70), where +0x70 is the budget period T (45 s).
     money = max(horizon × I + P, 0), where P = category +0x4c (the pool) and I = category +0x50 (the income rate).
     The test passes when money ≥ the goal's cost (0x692780).
3. There is no separate "upgrade available" test. Availability is decided only by whether the PotentialPlan solves
   (§1.2 and §1.4).

The A / R test 0x6129c0 (`max(horizon, lastDt) × R + A ≥ need`) is used at commit (0x6c8970), in the greedy and weighted
phases and by maintenance (0x722a30). It is **not** the accept test. [H]

Port: `PlayerAi::can_fund` (`engine/ai/src/galactic_ai.cpp`) calls `Budget::can_fund`, which is the A / R test. Accept
must use P / I with the horizon rule above. **Tier-1 difference.** The port's accept therefore rejects goals that retail
accepts whenever A is far below P, which happens once goals hold allocations.

### 1.2 Candidate replacement, 0x3f8fd0(planet behaviour, planet) [H]

Called by the candidate builder 0x6c1fb0 for every team type with behaviour 0x5C (DUMMY_STAR_BASE). 0x3f8c40 does the
same for 0x5B ground bases.

- The faction is the faction of the **planet's owner**, not the faction of the team entry. So the entry
  `Rebel_Star_Base_1` in UPGRADESTARBASEPLAN.LUA is only a generic placeholder.
- No starbase on the planet: Rebel owner → Rebel_Star_Base_1; Empire owner → Empire_Star_Base_1; any other owner,
  Underworld included → no replacement (0).
- A starbase on the planet: the candidate is that starbase type's Next_Level_Base (type +0x8c0).
- Level gate on the candidate:
  - If a starbase item (behaviour 0x5C) is in either production queue of the planet (0x401db0), the allowed level is 0.
  - Else, if the starbase has no owner (D+0x13c = −1), the allowed level is 1 when the planet's MaxStarbaseLevel is not 0.
  - Else, if the current level is below 5, the allowed level is level + 1, set to 0 when MaxStarbaseLevel (0x372780:
    the planet data D+0x238 from `Max_Space_Base`) is below level + 1.
  - The candidate is returned when its Base_Level (+0x8a0) ≤ the allowed level; otherwise nothing.
- **No can-build test (0x2804d0) is made here.** Buildability is checked later, in Production Setup C.
- The planet passed in is the goal's target planet, also for IgnoreTarget plans: IgnoreTarget changes contrast and
  costing, not the planet used for replacement.
- The team expansion (0x66fcc0 → 0x407b60) yields one type per faction and level. Every 0x5C type in it maps to the same
  result, and 0x6c1fb0 dedupes through its set, so the plan gets **one** starbase candidate.

HotM data: NalHutta and Shola have Max_Space_Base 5; Underworld_Star_Base_2 has Tech_Level 0, Base_Level 2,
Required_Star_Base_Level 1, cost 1600 and build time 20 s. So at NalHutta (Underworld_Star_Base_1, empty space queue)
0x3f8fd0 returns Underworld_Star_Base_2 from the start. The level gate does not explain the late start. [H]

Port: `GalacticAiWorld::Impl::next_starbase_type` returns the **first StarBase type for which `can_build_at` holds**. This
differs from retail in three ways:
1. It is not keyed on the current starbase's Next_Level_Base.
2. On an Underworld planet with no starbase, it can return Underworld_Star_Base_1, where retail returns nothing.
3. It applies the can-build gate early, so a temporary refusal (a hostile fleet in orbit, a starbase already queued)
   removes the candidate instead of letting Production Setup C veto it.

The port dump also shows "6 candidates" for UPGRADESTARBASEPLAN, where retail would have one plus the non-structure
types. The lane should check that the port dedupes. [M]

### 1.3 Run time (Produce_Force → AIBuildTask → AIPlanetBuildTask) [H]

- The type queued is the **replaced** type from the PotentialPlan selection, here Underworld_Star_Base_2 at the target.
- When 0x3f8fd0 returned nothing, the generic expansions themselves stay in the list. Production Setup C vetoes them
  permanently when 0x2804d0 refuses them (step 2, wrong faction, or step 20, the DUMMY_STAR_BASE level rule).
- A structure task is bound to its planet (build market 0x53cd60). It never bids for a slot and is never released by the
  market. It fails when 0x2804d0 no longer allows the type at that planet. So **a task cannot wait forever and hold the
  plan**: it is either buildable at its planet or it fails. (See `A2J_RE_BUILD_MARKET.md`.)

### 1.4 Budget category and cost [H]

- UpgradeStarbasePlan's goal is an Infrastructure goal. It is budgeted in the **Infrastructure** category.
- Budget marshaller: cost = cost(T, target planet), time = btime(T, target planet) for the replaced type, with the
  horizon clamped to the goal's Time_Limit (30).
- ETA 0x6639f0 puts a starbase in the **space** queue pool (q1). NalHutta's space queue is empty, so the r2 rule
  (`A2M_R2_RE_A.md` 1b: the ETA includes the queue wait at the target) does not delay this goal.
- The port's dump has cost 1600 and ETA 16.00 at NalHutta. Retail's ETA would use btime 20 s with the build-speed
  multiplier, as the port does. No difference is known here.

### 1.5 Why retail starts it only at 11059 [M/L]

Nothing in 0x3f8fd0, the accept test, the budget category or the run-time path blocks the goal at NalHutta. Retail
proposes it every pass with desire 34. What remains is competition in the commit:

- Commit truncates the accepted set to GoalSetExtensionSize (2) + |active| and then runs greedy and weighted phases by
  effective desire (`AI_GALACTIC.md` §1.6). A goal with desire 34 is cut whenever two or more accepted Infrastructure
  goals rank above it.
- Retail keeps proposing vehicle-factory goals (cost 2400, desire estimated at about 90 to 110, above 34). It finishes a
  second NalHutta vehicle factory at 9623, so it keeps those goals alive for a long time. With retail's lenient P / I
  accept (§1.1) these goals pass accept and fill the truncation slots. [M]
- The port shows the other side. From frame 74 to 5460, Build_Underworld_Vehicle_Factory at @2 / @8 / @9 and
  Upgrade_Starbase all fail accept with "fail Plan (6 candidates, 0 selected, last veto Plan)". At 5460 Upgrade_Starbase
  passes accept, and at 5512 it is taken greedily. Retail takes it at 11059. The port's Plan veto on the vehicle factory
  removes the competitor that retail keeps. [M]
- At 9626 NalHutta's ground level reaches 5, the maximum. NextGroundUpgrade becomes 0 and the vehicle-factory desire at
  NalHutta falls to about 18. The vehicle factories at Shola and Felucia were estimated still above 34, so this does not
  close the gap to 11059 on its own. [L]

Port actions:
1. Replace the accept test with 0x6139f0 (P / I, horizon rule of §1.1).
2. Make `next_starbase_type` follow 0x3f8fd0 (§1.2). Keep the can-build veto in Setup C, not in the replacement.
3. Find out why the port's Plan marshaller vetoes the vehicle factory from frame 74. Retail keeps proposing it and
   builds a second one at 9623, so retail does not veto a second vehicle factory on the same planet.

Then re-measure. If the port still starts the upgrade before about 11000, use the retail probe in §5 to read retail's
Infrastructure proposals around 5000-11000.

---

## 2. Q2: Underworld Build_Ground_Forces before 9664

**Rule [H].** Setup B has no admission rule that removes MAL_Rocket_Vehicle_Company, Destroyer_Droid_Company or
Canderous_Assault_Tank_Company:
- They are Underworld-affiliated, so 0x729550 admits them.
- Production Setup C finds Shola (U_Ground_Vehicle_Factory) and NalHutta (U_Ground_Droid_Works) as sources.
- TechTree passes.

The Budget marshaller then vetoes any unit whose cost does not fit in the Offensive category's money by the horizon:
`A + horizon × R`. Among the affordable units, the contrast float decides what is taken (`AI_GALACTIC_2.md` §1.5,
0x72b940).

**Retail amounts [M, from the trace].** `lua_records.txt` PSTART lines record the plan's unallocated amount, which equals
the net cost of its selection:

| Frame | Unallocated = net cost |
|---|---|
| 1501 | 1320 |
| 4174 | 1640 |
| 5785 | 1320 |
| 8368 | 2920 |

These sums fit MDU and Merc companies only. A MAL, Destroyer or Canderous company would push the selection past the
category's money at those frames.

**Port [M].** At 1412 the port has already vetoed MAL on budget, as retail would. The Destroyer company survives the
budget and wins the contrast float. So the port's Offensive A / R at that frame is higher than retail's. The likely cause
is the PAYG reservation below, or the accept-test difference of §1.1 changing which goals hold allocations.

**PAYG sub-question [H].** Queued but unpaid PAYG items **do** reduce the category's available amount:
- Add goal (0x610d30) computes need = req + alloc − a and takes it from A.
- The allocation stays reserved until the item is paid.
- Cancel (0x53ba60) refunds only the paid fraction; the unpaid part goes back to the goal's request.

Port action: check that the Offensive category's A in the port subtracts the allocations of queued-but-unpaid items, the
same way Add goal does. Then compare A and R at 1351 to 4300 against the retail probe of §5.

---

## 3. Q3: saboteur plans that end soon after queuing

Retail tags in run 170824: 60 queued at 1573 ended after 8 frames; 68 at 4146 after 105 frames; 65 at 2839 after 118
frames.

- **Maintenance time drop: impossible.** Once production has started, the goal production-time estimate 0x525200 is 0,
  so the maintenance step (0x722a30) cannot drop the goal for being too slow. [H]
- **Tag 65 (2839 → 2957) [M].** Rebel fleet 675 passes through Shola and arrives at 2945. A hostile fleet in orbit
  blockades the build planet. The plan gets Production_Failed, its script exits, and the plan-end cancel refunds the
  paid fraction (`A2M_R2_RE_B.md` §1).
- **Tags 60 and 68 [M].** The trace shows no hostile object near the source or target planet in that time. The remaining
  path is a goal-system release: the goal is dropped in resolve or replaced in the weighted phase. The release reclaim
  0x5f7d40 sets the plan's +0xD9 flag, and the queued saboteur is cancelled with refund.
- The trace cannot tell which goal-system path ends 60 and 68, because the goal type, target and desire are not
  recorded.

Port action: none from RE alone. Check that the port's blockade sends Production_Failed to the plan, and that a released
goal cancels its queued build. The exact path for 60 and 68 needs the retail probe in §5.

---

## 4. Q4: cycle-1 income

Retail [H, trace]:
- Before 1351 the Underworld corrupts **only Naboo**: the starting saboteur at frame 617.
- At 1351 the Underworld gets 2150. That is 2105 base income plus Naboo's corruption income: 90 (Planet_Income gross) ×
  0.5 (Corruption_Choice_Income_Percentage for Intimidation).
- The second starting saboteur, 595, dies at 1808 without corrupting anything.
- The next corruptions are MonCalimari at 10802 and Kuat at 12749.

Port: seed 0 corrupts **MonCalimari at 444**, because its Corrupt_Planet desire (25.5) is above Naboo's (24.0). This is
where the port's 2282 at cycle 1 comes from. The difference is in the Corrupt_Planet desire or target choice, not in the
income formula. The port's corruption income formula matches retail. [M]

The three retail runs make the same choices, so they are one effective sample. Whether retail **ever** picks MonCalimari
first needs more seeds, or the probe of §5.

---

## 5. Retail probe for the open parts (Gabriel launches it; not run)

Kit: a variant of `tools/probe_mod/a2f_budget` with a **human Empire** (today the kit is fixed to a human Rebel), HotM,
run to frame 12000. Channels, written to `lua_records.txt` every fiscal cycle and on every event:
- Underworld budget categories (Infrastructure, Offensive): P, I, A, R.
- PSTART / PEXIT of UpgradeStarbasePlan, BuildGroundForcesPlan, BuildUnderworldVehicleFactory and
  AI_PLAN_UNDERWORLD_DEPLOYSABOTEUR, each with goal type, target planet, desire and exit reason.
- Corrupted planets and their corruption type at each cycle.

This answers Q1d (which Infrastructure goals rank above Upgrade_Starbase from 5000 to 11059), the exact paths for
saboteur tags 60 and 68 (Q3), and the Offensive A / R per cycle (Q2).

## 6. Other note

The contrast-float question from the earlier r3 diagnosis is already answered in `AI_GALACTIC_2.md` §1.5. It was
re-checked in 0x72b940 and needs no change. [H]

The commit's greedy-phase "already active" check (0x6c8970) runs after the active set has been cleared for the new
commit. A goal that was active in the last commit is not skipped as "already active". [M]
