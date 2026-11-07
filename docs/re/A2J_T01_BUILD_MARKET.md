# A2j t01 / t08: the Empire's Tatooine build market (D3) and the Underworld's Upgrade_Starbase (D5)

Scenario: `Sandbox_Heart_Of_The_Maw_Rebel`, human Rebel with no orders, `--story`, seed 1, `--workers 0`
(command line of A2I_T03_TRAJECTORY §2). Port: branch `claude/a2j-laneA` at 5509c2a (a2i, D1 fixed). Retail: run
`tools/probe_mod/a2f_budget/out/20261106-053820` (`samples.tsv`, per-frame credits in frames 0..120; `plans.tsv`).

Method: temporary `ALAMO_A2J_*` stderr traces in `galactic_build.cpp` (market runs: queue time, own tasks, pending tasks,
bids, claims, starts; AI installments), `ai_goals.cpp` (candidate value, accept tail, commit truncation, resolve pool),
`galactic_pp.cpp` (PP failure + avail / rate) and `galactic_ai.cpp` (starbase build_check). None of them is committed.

Tags: [H] read or measured directly; [M] inferred from several consistent measurements; [L] hypothesis.

## 1. Summary

| # | Finding | Kind | Conf. |
|---|---|---|---|
| J1 | At HEAD the Empire's first Infrastructure goal is `Build_Turbolasers @Tatooine` (plan 42, task at 76), not Build_Weapon. The Heavy Scout Squad of Build_Ground_Forces starts on Tatooine's ground queue at 93 (32 s), the Turbolaser only at 1085 and ends at 2533. No other Empire Infrastructure plan starts before it ends (first Barracks plan at 2868). | port trajectory | H |
| J2 | Retail starts a 4000-credit / 48-s item at about frame 102 (first installment at 103): the Infrastructure pool falls 83.3 cr/s from 103, which is exactly the Turbolaser's PAYG rate. So retail's Tatooine ground queue starts the Turbolaser first. | retail | H rate, M item |
| J3 | The port's bids, claims and start follow AI_GALACTIC_2 §2.3 step by step (values in §2.2). Nothing in the spec lets a pending structure task block a ground-unit bid on its planet: step 4 has no such exclusion. | spec check | H |
| J4 | What decides J1 vs J2 is not in the spec: (a) the order in which step 3 scans the global pending list (port: oldest first), or (b) whether binding a structure task (0x6956f0) puts it straight into the planet queue's list. Either one starts the Turbolaser at the port's 93 (experiment E2, §2.3). | open, RE needed | M |
| J5 | Retail's planet queues do not share one market phase: Underworld palace starts at 54 and 80, a Crusader at 96, the Empire item at 102. The port runs every queue at 31, 62, 93, ... | open, RE needed | H retail, L cause |
| J6 | The 34-frame task delay is the plan script: `BuildTurbolasers.lua` / `BuildWeaponStructurePlan.lua` call `Sleep(1)` before `AssembleForce` (1 s = 30 frames + the 3-frame Planning service). Not a deviation. | script | H |
| J7 | Superseded by §5: the PAYG term is max(fundsTime, btime), not min (A2J_RE_BUILD_MARKET S1). | port fix | H |
| D5 | At HEAD the Underworld's Upgrade_Starbase plan comes at 1460 (@Shola), earlier than retail's 2794. At 41 it is cut by commit step 5 (truncation to GoalSetExtensionSize 2 + 0 active: the two palaces, desire 85 / 82, outrank it, 34). From 69 to 1415 every accept fails in the PotentialPlan solve: Infrastructure A = 1297, then 297, with R = 0 until the first income. No spec deviation found; the remaining gap is goal choice after the first income (§3.3). | timing / downstream | H port, L retail cause |

## 2. t01: D3, the Tatooine ground queue

### 2.1 Port market, frames 31..186 [H]

Market runs at `now` = 31, 62, 93, 124, ... for every planet queue (`Q.last_run` starts at 0, strict > 30).

| frame | Tatooine queue 0 | other planets |
|---|---|---|
| 43 | tasks 3 (Acclamator), 4 (HSS), 5 / 7 (Stormtrooper), 6 (AIB) made, pending | |
| 62 | bids: HSS 75.09 = (0 + 32 + 5.54) × 2, Stormtrooper 35.09, AIB 59.09 | Kuat bids HSS 85.74 = (0 + 24 + 4.58) × 3; Kashyyyk takes the Stormtroopers (19.10) |
| 76 | task 8 Turbolaser made, bound to Tatooine (bid planet = Tatooine) | |
| 93 | claim one: task 4 (HSS, first in the pending list); queue idle → start HSS (slack −33.7) | Kashyyyk starts a Stormtrooper, Corellia the Acclamator |
| 124 | claim AIB (6) | |
| 155 | re-check: AIB released (fundsTime 0 < queueTime 29.97); claim the Turbolaser | |
| 186 | re-check: Turbolaser released (0 < 28.93) and claimed again in the same run; queue busy | Kuat starts AIB 6 |

The Turbolaser stays in Tatooine's list (released and re-claimed every run) until HSS finishes (93 + 960 frames) and starts
at 1085. Level factor: 1 + (ground base level − Required_Ground_Base_Level) × K with K = 1 (AI_GALACTIC_3 §8), e.g.
Tatooine 2, Kuat 3 for the HSS. Build times differ per planet (HSS 32 s at Tatooine, 24 s at Kuat).

### 2.2 Spec check, §2.3 steps 1–5 [H]

- Step 1 queue time, step 2 keep test (`fundsTime ≥ queueTime`, reachable, buildable), step 3 one claim per run,
  step 4 exclusions (path, buildable, ground slots, `fundsTime < queueTime`) and the bid product, step 5 least slack:
  the port matches. A bound structure task does not bid (port reading of "+0x7C set").
- **Can a ground-unit task bid on a queue a pending structure task waits for?** Yes, per the spec: none of the step-4
  exclusions refers to structure tasks. At 62 the Turbolaser task did not exist yet anyway (it is made at 76).
- Step 2 PAYG term: J7, corrected in §5 (max, not min).
- Port reading, not spec: after a release (0x695a30 "planet = null") a bound task keeps its target as bid planet; the
  literal reading would leave it with no planet and, if bound tasks never bid, never claimed again. See RE question Q3.

### 2.3 Experiment E2: claim newest first [H port]

Scanning the pending list from the newest entry in step 3 (scratch only) gives: 93 Turbolaser @Tatooine (with the
Stormtrooper @Kashyyyk and the Acclamator @Corellia), HSS @Kuat at 775, Build_Turbolasers ended at 1540. That is the
retail pattern of J2 (start about 102, 4000 / 48 s, end about 1543), 9 frames early (J5).

### 2.4 Retail per-frame credits [H]

- Empire: flat 8000 to 102, then −6.5275 per frame from 103 (195.8 cr/s = the Turbolaser's 83.3 + 112.5 from other
  items); the Infrastructure pool falls 430 per 155 frames from about 103 to 1329 (A2I_T03 §3.2).
- Underworld: −3.333 per frame from 55 (one palace, 2000 / 20 s), −6.667 from 81 (two), −14.77 from 98 (plus about
  243 cr/s, the space build). Port: both palaces at 62, the Crusader at 93.

## 3. t08: D5, Underworld Upgrade_Starbase

### 3.1 Port at HEAD [H]

| frame | event |
|---|---|
| 17 | accept: Upgrade_Starbase @NalHutta / @Shola desire 34, net 1600, ETA 14.4, AllowedTime 19.2, fundable (@Felucia 17, net 3000) |
| 41 | commit step 5: Infrastructure proposals sorted (palaces 85.26 / 81.72, vehicle factories 50.45 / 39.73 / 38.49, Upgrade_Starbase 34 / 34, Corrupt_Planet 25.5 ...) and truncated to 2 + 0 active: only the palaces reach Resolve |
| 69..1415 | every new accept of Upgrade_Starbase (and Build_Underworld_Vehicle_Factory) fails in the PP solve: Infrastructure A = 1297 (after the palaces' 2 × 2000), from 301 on 297 (1000 less between 172 and 301 [M: the Corrupt_Planet @Naboo goal, net 1000]), R = 0.000, so the Budget marshaller vetoes the 1600-credit starbase (the diagnostic names the last veto, Plan, which comes from the free-store candidates) |
| 1416 | first accept that passes (after the first fiscal payment) |
| 1460 | Resolve: pool Upgrade_Starbase 34 (new), Corrupt_Planet 22.72 / 21.76 (active, no cost), Corrupt_Planet 5.5 / 4.0 (new, 1000 each, lowered by Per_Failure_Desire_Adjust −20); greedy takes Upgrade_Starbase; plan 22 @Shola, Underworld_Star_Base_2 ordered at 1488 |

The starbase type is buildable at NalHutta and Shola the whole time (build_check: Underworld_Star_Base_2 ok); the veto is
money only.

### 3.2 Spec check [H]

Commit steps 1–6, the truncation to `extension + |active|` (extension 2, AI_GALACTIC §1.6 step 5), Resolve_Category
steps 1–7 and the PP Budget marshaller (`available = money + t × rate`, 0x728330) are as specified. The resolve order
(category list sorted by normalised weight) does not matter here: Upgrade_Starbase never shares a pool with the palaces
after 41. **No spec deviation.** The a2h result "no plan in 3000 frames" no longer reproduces at 5509c2a.

### 3.3 Port vs retail after the first income [L]

- Retail Infrastructure (decoded P, A2I_T02_PROBE): 1338 at 1487 with a rate of 31.6 cr/s, then it falls about
  258 per 155 frames (50 cr/s) to 388 at 2107 and stays flat; the Upgrade_Starbase plan follows only at 2794 (credits 3416).
- 50 cr/s for about 950 credits fits a 1000-credit / 20-s build, i.e. most likely a saboteur for a Corrupt_Planet goal [L].
  In the port the Corrupt_Planet proposals that need a build have desire 5.5 / 4.0 at 1460, because the earlier
  DEPLOYSABOTEUR plans failed (291, 1293: "ability Underworld_Saboteur_Team ... not applied" at 993) and each failure
  costs −20 desire. So the gap is downstream of the saboteur plan outcome (and of D4), not of budget timing.

## 4. RE questions (follow-up)

1. **Q1 (pending-list order).** In the AIPlanetBuildQueue market 0x53cd60 step 3 (claim one pending task that chose this
   planet): in which order is the global pending list 0x140b39330 scanned, and where does BUILD_TASK_LIST (0x648980) /
   0x695a30 insert a task (front or back)? Is the container a vector, a linked list (push_front?) or something else?
2. **Q2 (bind).** Does 0x6956f0 (bind the build planet = goal target for structure / generic tasks) only set the
   AIPlanetBuildTask's planet field (+0x28), or does it also put the task into that planet queue's own list (+0x10)
   (via 0x695720 → 0x53b7d0, AI_GALACTIC §1.9)? Is the bound task on the global pending list at all?
3. **Q3 (+0x7C and release).** What is AIPlanetBuildTask +0x7C (step 4 bids only on tasks with it set), and is it clear
   for bound tasks? After a step-2 release (0x695a30: planet = null) of a bound task, what gives it a planet again?
4. **Q4 (market phase).** What is the initial value of an AIPlanetBuildQueue's last-run frame (ctor, or set when the
   queue is created lazily, e.g. by 0x559b40 from the PP ETA 0x53bb20), and is ProductionBehavior::Service (0x402840)
   called every frame for every planet? Retail starts are at 54 / 80 / 96 / 102 (§2.4), the port's at 62 / 62 / 93 / 93.
5. **Probe (retail, lane E):** log `Galactic_Production_Begin_Event` (type, planet, frame) for every AI player in frames
   0..1500 of this scenario. It would show which items start at 54 / 80 / 96 / 102 / 103 and whether the Empire's HSS
   goes to Tatooine or Kuat, and it would settle J4 / J5 and the D5 Infrastructure item of §3.3.
6. **Answers to Q1–Q4:** `A2J_RE_BUILD_MARKET.md` (2026-11-06). The pending list is scanned oldest first; bound tasks are never released; each queue's last-run frame starts at a synced Get_Int(1, 30). Also: PAYG queue time is max, not min, and there is a step-5 slack correction.

## 5. Fix (a2j lane A follow-up, after A2J_RE_BUILD_MARKET)

### 5.1 Port changes (`engine/ai/src/galactic_build.cpp`, `galactic_ai.cpp`, `galactic_ai_impl.hpp`) [H spec]

| RE item | Change |
|---|---|
| §4 phase | `init_queue_phases()` (AI world ctor): per planet in index order, queue 0 then queue 1, `last_run` = synced `range_int(1, 30)` (0x1ffb40 rules). The market gate stays `now − last_run > 30`, and an unowned or neutral planet still returns before the stamp. `last_run` was already in the AI save, so the save format is unchanged. |
| §1 order | Unchanged: oldest-first scan in step 3, append at the back on release. E2 is dropped. |
| §2 / §3 bound | The bind sets bid −1 when the goal has a target planet. With no target the task stays unbound (bid 1e18) and bids. Step 2 always keeps a bid < 0 task: it adds the queue term when the task is buildable here and fails it (Production_Failed) when it is not. It is never released. Signal 0xE fails a listed bid < 0 task; an unbound one returns to the pending list with no choice (a bound one no longer keeps its target). |
| S1 | Queue term in steps 2 and 3: PAYG max(fundsTime, btime); otherwise btime when fundsTime ≤ 0, else 0. This replaces J7's min. The step-3 claim adds the same term (it used to add btime). |
| §3 / S3 step 4 | The step skips tasks with bid < 0. The +0x7C flag is always set on the port's pending tasks, because free-store supplies finish at once. Every refusal (not buildable / production disabled, no ground slot, path, fundsTime < queueTime) clears the task's choice when that choice is this planet. |
| S2 step 5 | Candidates: PAYG or fundsTime ≤ 0, and buildable here. Slack = goal ETA − (now − goal created) / 30 − btime (`goal_of_plan`); a task with no goal gets 1e18. |

[port] Retail draws at the PRODUCTION attach during galaxy build, interleaved with the other attach draws (GALACTIC_RNG_DRAWS
row 2). The port draws when the AI world is built, which is after `GalacticSim::start` and after the load-time story pass.
The count (24 draws on HotM) is right, but the stream position is not, so the per-queue values differ from retail's.
Exact placement needs a hook in `engine/galactic`'s galaxy build (open item below).

A2k t04 (2026-11-06): done. `GalacticSim::start` draws both phases right after each planet object is made (row order,
before the starting forces), keeps them in `PlanetState::queue_phase` (galactic save v4 since the a2k merge onto dev,
2026-11-07) and exposes them as `GalacticSim::build_queue_phase(planet, queue)`. `init_queue_phases()` copies them and no longer draws.

### 5.2 HotM Rebel-human run after the fix [H port]

Same command as §2 (3000 ticks). Synced stream changed: the Empire's Build_Space_Forces now picks Tartan + Broadside
(3050), which is the retail build A2I_T03 §3.2 inferred.

| item | port before | port after | retail |
|---|---|---|---|
| queue phases (q0 / q1) | all 0 | e.g. Tatooine 27 / 6, Corellia 21 / 16, NalHutta 23 / 21, Shola 22 / 28, Felucia 30 / 6 | Tatooine q0 9 [M] |
| Underworld palaces | 62 / 62 | 53 (Shola) / 54 (NalHutta) | 54 / 80 |
| Underworld Crusader (Felucia, q1) | 93 | 99 | 96 |
| Empire Tatooine queue 0 | HSS at 93 | AIB at 89 (Build_Ground_Forces, task 43, Tatooine choice from its run at 58) | Turbolaser at about 102 |
| Empire Turbolaser start | 1085 | **833** | about 102 |
| Empire Infrastructure spent by 1350 | about 680 (estimated: 4000 / 48 s from 1085) | **1436** | about 3400 |
| Empire Offensive spent by 1350 | not measured at 5509c2a | 3058 | about 2817 by 1329 |
| `Build_Base_Component_Heavy_Vehicle_Factory @Tatooine` time-drops | 610, 772, 922, 1168 (a2h) | **1693, 1880, 2069, 2259** (eta_el 167.0 / 160.8 / 154.6 / 148.3 s against AllowedTime 72) | still retried at 6172 |

### 5.3 Why the Turbolaser still does not start near 102 [H port, L retail]

- **E3:** forcing Tatooine queue 0 to r = 9 (retail's phase; runs at 40, 71, 102) only moves the AIB to 102. It bids at
  Tatooine at 71 (59.09 against Kuat 67.74), is older than the Turbolaser task (76), and so is claimed first at 102. The
  Turbolaser follows at 846. The phase alone cannot give the retail start.
- In the port, Build_Ground_Forces is released at 124 by maintenance because its budget cannot fund it (`can_fund` false:
  Offensive holds Tartan + Broadside 3050 + ground 450 against about 3105). Its AIB is already producing by then, so it
  holds Tatooine until 809.
- Retail start at 102 under the RE rules needs no older ground task with a Tatooine choice at 102 (A2J_RE_BUILD_MARKET §1).
  Candidates [L]:
  - retail releases Build_Ground_Forces (or never plans it) before 102;
  - its ground tasks are made after 71;
  - another planet outbids Tatooine.
- The lane E probe (production-begin events with credits) settles this.

### 5.4 Goldens (not regenerated; the coordinator regenerates them)

The synced draws and the market rules change every galactic-AI trajectory:
- `alamo_ai_galactic_tests` (`ai_galactic_synthetic_crc.txt`, test_ai_galactic.cpp:1324; `ai_underworld_crc.txt`, :4090);
- `alamo_headless_galactic_ai` (`galactic_ai_hotm_golden.txt`);
- `alamo_headless_galactic_ai_human` (`galactic_ai_hotm_human_golden.txt`); its `_w1 / _w4 / _w15 / _w16` entries are skipped because the w0 fixture fails.

Worker-count and compiler identity still hold:
- the thread / worker comparisons inside `alamo_ai_galactic_tests` pass;
- the HotM human run gives the same AI log and final CRC (feb291b33bbb0561) with 0 and 4 workers on MSVC and with 0 workers on clang;
- the N1 save / load lockstep checks pass.
