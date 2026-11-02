# A2 t03: why Conquer_Opponent against the human is rarely solvable (diagnosis)

Status 2026-11-01. Diagnosis only: no engine file was edited, nothing committed. Experiments ran in a
gitignored scratch copy (`build-a2diag-src/`, instrumented with stderr traces and env switches) and a faithful
build of the real tree (`build-a2diag-msvc/`). Clean-room: only `docs/re/`, the engine and the game's XML/Lua
data were read.

Tags: [port] = port bug or port stand-in, [M] = model of an unspecified/uncertain original behaviour, [L] =
learned from data/Lua, [H] = high confidence in the spec.

## 1. How Conquer_Opponent is evaluated and solved (static trace)

1. Desire. `Should_Conquer_Opponent_Planet` (`Data/XML/AI/PerceptualEquations/OffensiveGalaticEquations.xml:417`)
   = 1.25 x Is_Good_Invasion_Target x Variable_Target.IsHumanControlled x (...) x First_Attack_Allowed x
   (1 - Unrestricted_Grab_Space). `First_Attack_Allowed` = `Game.Age > Initial_Grace_Period` (xml:450), which is
   200 + 7 x planets controlled seconds on Normal (228 s with 4 planets), matching the first positive evaluation
   at frame 6854. [H]
2. Goal slice, `GoalSystem::proposal_slice` (`engine/ai/src/ai_goals.cpp`), gates in order: proposable,
   applies (`galactic_ai.cpp` ~281, [L]), skip (active / `blocked` / `reachable`, `galactic_ai.cpp:437-471`),
   evaluate, desire > 0, `hooks_->accept(g)` (ai_goals.cpp:109), then
   `(at <= 0 || at >= g.eta) && hooks_->can_fund(g)` (ai_goals.cpp:114-115). `allowed_time` (ai_goals.cpp:239)
   = eta x `Build_Time_Delay_Tolerance` (1.3) or `Time_Limit` (240) without a PP.
3. Reachability `Enemy_destination`: flood from the AI's own territory (`galactic_ai.cpp:458`,
   `ai_free_store_path.cpp:12-45`, `engine/galactic/src/fleet_path.cpp:21-30 for_ai_mode`). Only planets adjacent
   to the AI's territory are reachable. `for_ai_mode` sets `allow_enemy_owned == allow_unknown_or_neutral` in
   every mode, so the t01 `ai_perceived_non_neutral` bit does not change the flood.
4. `PlayerAi::accept` (`galactic_ai.cpp:508`): Find_Plan_For_Goal (weighted pick among template-enabled plans,
   one synced RNG draw), then the PotentialPlan (PP) solve.
5. PP solver (`engine/ai/src/galactic_pp.cpp`): nine galactic marshallers in order FreeStore, Budget, TechTree,
   Plan, Production, Hero, TerrainType, Contrast, Survival. Score 1e18 = veto (Plan and Production vetoes are
   permanent); greedy strict-max pick, lowest index on ties; the Contrast veto normally ends the solve.
6. `PlayerAi::can_fund` (`galactic_ai.cpp:554`): max(max(min(eta, TimeLimit), T) x rate + available, 0) >= cost
   against the goal's category (Offensive) budget.
7. Commit, then the plan's Lua thread (ConquerOpponentPlan.lua / CrushPlan.lua).

### Every gate that can make it unsolvable

| # | Gate | Where | Observed |
|---|---|---|---|
| G1 | Grace period / desire <= 0 | OffensiveGalaticEquations.xml:417,450 | passes after 228 s |
| G2 | Reachability (Enemy_destination) | galactic_ai.cpp:437-471 | only planets adjacent to AI land; no t01 effect |
| G3 | Find_Plan_For_Goal picks CrushPlan; its Lua ScriptExits on Easy/Normal | CRUSHPLAN.LUA | about 50% of picks wasted (376 of 797) |
| G4 | PP Plan marshaller minimums (ConquerOpponentPlan SpaceForce 4 units / 2500 force, GroundForce 4 / 750, RequiredCategories Infantry plus Corvette/Frigate/Capital/Super; CrushPlan MainForce 10 / 8000) | galactic_pp.cpp PlanM finalize | Yavin (no starbase) still fails: no ships in the free store |
| G5 | PP Budget marshaller veto `available < c + net` | galactic_pp.cpp:409 | last veto in about 94% of failed attempts |
| G6 | PP Contrast marshaller (T = 1 - Min/Max = 0.2857, needs about 1.375x enemy weighted power) | galactic_pp.cpp:791-890 | the veto that kills 100% of attempts when budget is waived |
| G7 | goal-level `allowed_time` | ai_goals.cpp:114 | not binding |
| G8 | goal-level `can_fund` | galactic_ai.cpp:554 | second independent wall: net costs 10k-51k vs Offensive pool at most 7.4k |
| G9 | Lua pacing (GalacticAttackAllowed etc.) | AI_GALACTIC_2 sec. 6 | never reached in baseline |

## 2. Baseline run (real tree)

HotM, Rebel scripted human, 36000 ticks, Normal AI, golden `engine/apps/headless/testdata/galactic_ai_hotm_human_golden.txt`
(0 Conquer_Opponent plans, no owner change).

- 797 positive-desire Conquer_Opponent evaluations, first at frame 6854; 0 accepted PPs (421 ConquerOpponentPlan,
  376 CrushPlan "no potential plan").
- Failing marshallers: Contrast about 60%, Plan+Contrast about 40%; the last veto is Budget in about 94%, Contrast
  otherwise.
- Targets: Empire (p1) only Naboo; Underworld (p5) MonCalimari, Naboo, Yavin.
- Enemy totals: Naboo S=10000 (level-2 starbase) G=1210; MonCalimari S=15000-17500 G=900; typed space targets
  16.5k-33.7k.
- Offensive category: pool 3.9k-10.7k, available 160-7000, rate about 7.5/s (Empire 6.5/s, weight 0.439, credits
  about 0; Underworld weight 0.179 while its credits grow 13k to 53k unspent in other categories).
- Free store: p5 about 17 units / 3000 power (mostly ground); p1 6 units / 1134 power.

Q2 cross-check (`probe_mod/q2_traces`, HotM Empire vs idle human Empire, 13501 ticks): the original Rebel starts
moving at tick 104, makes 71 departures (39 to Shola) and takes Shola from the Underworld at tick 7498; original
Rebel / Underworld spend credits down (812 / 2500 final). The port keeps 3318 / 4075, orders almost no units and
has no owner changes.

## 3. Ranked causes

### C1 (dominant) The typed contrast rows include the starbase's whole AI_Combat_Power. [port stand-in, spec conflict]

- Code: `galactic_pp.cpp:239-262 enemy_strength` adds `w.power_of(starbase)` for any mask containing the Capital
  bit (line 254, tagged "[port] starbase members = its own power"); `ContrastM::setup_a` (line 791-840) uses it for
  the unscaled entries 0/1 (lines 807-808) and for every typed row (line 815) times
  `(max_contrast_scale + fp) x Galactic_AI_Contrast_Multiplier (1.1)`. The same stand-in is in
  `galactic_ai_tokens.cpp:543-566 force_raw`.
- Starbase AI_Combat_Power by level: 5000 / 10000 / 15000 / 20000 / 35000 (`Data/XML/STARBASES.XML`), category
  mask Capital. So one level-2 starbase adds about 10k x (1.75+f) x 1.1 to the Capital typed row, which the
  free store (mostly ground units) cannot cover.
- Spec: `GALACTIC_ECONOMY.md` sec. 6.2 and `PHASE2_ANSWERS.md` sec. 7 [H] put "perception value + starbase
  AI_Combat_Power" only in helper 0x5f2430 with category mask "all" (the untyped Node_Weight consumer).
  `AI_GALACTIC_TOKENS.md` sec. 2 says the per-type SpaceTotalUnnormalized leaf adds the starbase's per-tech
  member units "(not the starbase's own power)"; StarbaseTotalUnnormalized is the separate own-power token. So the
  typed rows should hold the starbase's spawned squadrons (`Starting_Spawned_Units_Tech_N`), not its 5k-35k power.
- Experiments (scratch copy):
  - `CO_NOSB` (drop starbase power from every total): 4 Conquer_Opponent PPs accepted in 20 min, 2 plans started,
    1 ended success.
  - `CO_SBUNTYPED` (own power only in entries 0/1, not typed rows; the spec reading): HotM-human gets 4 accepted
    Conquer_Opponent plans (3 CrushPlan, 1 ConquerOpponentPlan at frame 33876, net 3790, can_fund=1) and Felucia
    and NalHutta change hands from the Underworld. Q2 scenario (`--campaign Sandbox_Heart_Of_The_Maw_Empire
    --human Empire --seed 0 --ticks 13501`): Rebel launches Conquer_Pirate vs Shola at frame 352 with 21 free units
    (net 0), 56 offensive PPs total, versus none in the baseline port; the original Rebel attacks Shola.
- Fix proposal: in `enemy_strength`, add the starbase's own power only when `mask == ~0` (untyped), and for typed
  masks add the power of the starbase's tech-level spawned units (data: `Starting_Spawned_Units_Tech_N` from
  STARBASES.XML) by category; keep ground structure garrisons likewise. Mirror in `force_raw`
  (`galactic_ai_tokens.cpp:543-566`). A probe of one Naboo-style attack would confirm the member list.
- Golden impact: large but contained to the galactic AI. Regenerate `galactic_ai_hotm_golden.txt`,
  `galactic_ai_hotm_human_golden.txt`, `engine/ai/tests/golden/ai_galactic_synthetic_crc.txt`,
  `ai_underworld_crc.txt` (and `ai_golden.txt` only if a galactic path feeds it), plus any save test that drives
  `GalacticAiWorld` (`headless/save_run.cpp`). Coordinator regenerates (one apps/sim owner). Needs t01 first:
  no.

### C2 Offensive budget starvation and unused money elsewhere. [M], partly [port]

- `BudgetM::score` veto (`galactic_pp.cpp:409`) and goal `can_fund` (`galactic_ai.cpp:554`) both gate on the
  Offensive category, whose weight comes from the BudgetingEquations (Empire 0.439, Underworld 0.179,
  rate about 7.5/s). Experiment `CO_NOBUDGET`: all 797 PPs succeed, but `can_fund` then rejects every one
  (net 10k-51k vs pool at most 7.4k); so budget is a second, independent wall behind C1.
- Underlying port issue (AI_GALACTIC_2 sec. 6.1): money sits unspent. Underworld infrastructure plans
  (`Should_Build_Underworld_*` evaluate non-positive; `Build_*` plans fail at Plan with a Budget veto at gross 0)
  never fire, and blind production (Build_Space_Forces / Build_Ground_Forces) makes only 3-7 small plans per 20 min
  against 37-50 queue starts in the original. This is the most likely reason the port AI is passive in Q2.
- Fix proposal: do not tune weights. First investigate why the infrastructure and blind-production plans do not
  fire (separate task). Only after that re-measure C1's effect.
- Golden impact: large (all galactic AI goldens). Needs t01 first: no.

### C3 Contrast solver lets an over-satisfied row keep attracting free units. [M]

- `ContrastM::score` (galactic_pp.cpp ~850-890): pair term `d = max(R - wP, 0); m = max(R, wP);
  s = (1 - (d/m)^2) x w`. With R <= 0 the term stays w, so a free unit joins an already-satisfied typed row
  instead of the unsatisfied Capital row (Q2 Shola, f=276: rows S0/S1/S4 at r=-19493, Capital row S5 stays
  6325/6325). `recompute_remaining` (line 279) subtracts a unit only from the chosen entry (the [M] reading of
  0x6bf840).
- Experiments: `CO_POSR` (skip pairs with remaining <= 0) alone reduces S5 to 2399, still unsatisfied; with
  `CO_SBUNTYPED` it gives 24 offensive PPs in Q2 (vs 56 with C1 alone, so it is not a gain). `CO_ALLROWS`
  (subtract from all rows) stops the solver early, identical CRC to baseline; not viable.
- Fix proposal: none now. The spec does not say; needs a probe or Q2 evidence. Leave as is.
- Golden impact: none if untouched.

### C4 Plan marshaller minimums and category requirements. [L], original behaviour

- ConquerOpponentPlan: SpaceForce min 4 units / 2500 force, GroundForce min 4 / 750, RequiredCategories
  Infantry plus Corvette|Frigate|Capital|Super. CrushPlan MainForce min 10 / 8000. Yavin (S=0) still fails
  because the free store holds no ships and Offensive available (about 1.5k) cannot buy them.
- Fix: none (data); improves automatically once C2 is fixed.

### C5 CrushPlan wasted picks. [L], original behaviour

- CrushPlan ScriptExits on Easy/Normal difficulty; the port treats every AI as Normal, so about half of the
  accepted Conquer_Opponent plan picks are wasted (pick weights from learning ratio, AI_GALACTIC_2). Not a bug.

### C6 Grace period and t01. Not contributing.

- First_Attack_Allowed at 228 s is [H] and not a blocker (first evaluation at frame 6854).
- t01 (`player_view` / `ai_perceived_non_neutral`): with the concurrent uncommitted `campaign.cpp` patch the HotM
  golden still MATCHES, and `for_ai_mode` has equal allow flags, so the bit cannot change the Enemy_destination
  flood. t01 is neither a cause nor a prerequisite of any fix above; it may matter once reachability is changed to
  separate neutral from enemy-owned.

## 4. Recommended order

1. C1 (starbase power only in untyped totals; spawned squadrons in typed rows). Cheapest, biggest effect,
   spec-backed. Regenerate AI goldens afterwards.
2. C2 investigation (why infrastructure / blind production never fire), then re-measure offensive spending
   against the Q2 trace (credits spent down, 71 Rebel departures).
3. C3 only if Q2 still shows the Rebel idle after 1 and 2.

Reproduce: the scratch copy's env switches are CO_NOSB, CO_SBUNTYPED, CO_NOBUDGET, CO_SCALE, CO_POSR,
CO_ALLROWS, CO_ALL; `[CO]` / `[GATE]` lines on stderr. The copy is gitignored (`/build-*/`) and is not part of
the change.
