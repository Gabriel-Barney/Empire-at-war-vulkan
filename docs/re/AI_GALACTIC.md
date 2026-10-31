# AI framework and galactic AI (roadmap A2; framework shared with A1)

Clean-room behavioural spec from the Ghidra 12.2 decompilation of the 64-bit Steam `StarWarsG.exe` (FoC v1.121, image base 0x140000000; all addresses are RVAs) and from the shipped FoC data (`Data/XML/AI/*`, `Data/XML/DifficultyAdjustments.xml`, `Data/Scripts/{AI,Library,FreeStore}/*.lua`). No decompiled code is reproduced.

Confidence tags:
- **[H]** read directly from the code or data.
- **[M]** one link inferred: a field meaning, a dropped register argument, or a name.
- **[L]** plausible reading that has not been verified.

This document builds on these existing docs and does not repeat them:
- HOTSPOTS.md §1–2: Service, Propose_Goals_Incremental, Commit, Recompute_Category_Weights, PerceptionContext, FindTarget.
- PHASE2_ANSWERS.md §5–11: AI path finder, Node_Weight, 90-frame all-pairs, stagger.
- GALACTIC.md, GALACTIC_ECONOMY.md: economy, PAYG, Credit_Multiplier, perception tokens §6.
- COMMANDS.md: AI and Lua orders execute directly.
- BATTLE_TRANSITIONS.md: auto-resolve.
- LUA_API.md: binding RVAs.
- PRIOR_PATCH.md: `ai_pacing`.

The engine already implements perception equations and galactic perception (PHASE4 S2) and the Lua players family (PHASE4 L1 players).

Notation:
- **FPS** is the integer logic rate global 0x140b0a340 (30).
- **NOW** is the logic frame 0x140b0a320.
- **Synced RNG** is the LCG at 0x140a13e24 (`s = s·0x41C64E6D + 0xBDF`). Its float draw 0x532480 is `((s>>10)&0x7fff)/32767 · (hi−lo) + lo` (MAIN_LOOP.md). Its int draw is 0x1ffb40.

**AI_TACTICAL.md should reference §1 (the framework). Everything in §1 applies to all three modes unless it is marked galactic.**

---

# §1 Framework (shared by galactic, space and land AI)

## 1.1 Object model

| Object | RVA / vtable | Role |
|---|---|---|
| `TheAIClass` | vt 0x8795f8, ctor 0x4d9c80, global 0x140a16d90 | **Enable gate only**, with no Service method. It holds per-mode bytes +0x8/+0x9/+0xa (Galactic/Land/Space) and a master byte +0xb. Is-enabled (0x4d9f80) = master && perMode[mode]. Start_Game resets all four bytes to 1 (0x4d9d40). [H] |
| `AIPlayerClass` | vt 0x8761f8, 0x60 B, ctor 0x4af810 | Held at PlayerClass+0x360. Fields: +0x28 PlayerClass*, +0x30 AIPlayerType*, +0x38 vector of per-mode contexts, +0x50 a galactic-only object (unidentified), +0x58 difficulty index (−1 when constructed). [H] |
| Per-mode context = `TacticalAIManagerClass` | vt 0x8a8948, 0x70 B, ctor 0x5f4be0 | One per mode: +0x34 mode (0 Galactic, 1 Land, 2 Space), +0x30 opponent player index. Subsystems: +0x38 Perception, +0x40 Goal, +0x48 Planning, +0x50 Execution, +0x58 Learning, +0x60 Template. +0x68 actor flag, +0x69 initialised. [H] |
| `ServicedAISystemClass` | vt 0x8b3f40 | Base of every subsystem. Slots: 2 Service, 3 Get_Service_Interval (float s), 4 Save, 5 Load. +0x28 is the owning context; +0x30 is the absolute next-service frame. [H] |

**Subsystem factories.** Context Init (0x5f55f0) calls them in this order: Perception (0x5f52e0), GoalSystem (0x5f5230: Galactic 0x6b8480, Land 0x6b86e0, Space 0x6b88d0), Planning (0x5f5360 → 0x6bac00), Execution (0x5f51e0 → 0x524ce0), Learning (0x5f52b0 → 0x585d00), Template (0x5f5390 → 0x6bb9e0). Any null result fails the AIPlayer. [H]

**Context lifetime**
- A context is added when its mode begins (0x4b0950) and removed when the mode ends (0x4b0a10). [H]
- A tactical context is also dropped when the player is not an actor, owns nothing in that mode, and the session type ≠ 3 (0x27ff80). [M]
- `PlayerWrapper.Enable_As_Actor` forces a context to exist (0x4b0250). [H]

**First service of a context** (0x5f5ab0 while +0x69 == 0)
1. Pick the opponent by scanning players in index order for enemies (0x2824f0) that own objects in the mode and whose faction byte +0x108 (`Is_Neutral`) is 0.
   - Galactic: a human wins immediately; otherwise the last player with faction byte +0x20a8 set. [M]
   - Tactical: prefer the mode's attacker or defender. [M]
2. Call slot 6 (first-service init) on Perception, Goal, Planning, Execution and Learning, then Template-init 0x6bbed0. [H]

## 1.2 Who gets an AI, difficulty, Suspend_AI

**Assignment** goes through `Player::Set_AI_Type` (0x2886d0 → 0x4aff90).
- `"Human"` deletes the AIPlayer. An unknown type name logs "Unrecognized AI player type" and creates no AI. [H]
- `None` (`PLAYER_NONE.XML`) is a real AIPlayerType with tactical goal sets and a `None_Template` galactic template. It is not "no AI". [H]
- Who calls it:
  - Skirmish/MP slots get `Faction.Basic_AI` (Faction+0x378) plus the lobby difficulty (`P%d_AIDIFFICULTY`). [H]
  - Campaigns use `AI_Player_Control` "Faction, Type" pairs. [M]
  - The story reward case 0x30 sets the type, then the difficulty from global 0x140b15498. [M]
  - Set_Difficulty is skipped for players with +0x62 (`Is_Human`) set. [M]

**AIPlayerType XML** (`Data/XML/AI/Players/*.xml`):

| Tag | Meaning |
|---|---|
| `Name` | Type name |
| `Personality/Focus`, `Aggressiveness` | Parsed. No consumer was found, and the shipped Underworld file comments them "Meaningless". [L] |
| `GalacticFreeStoreScript`, `SpaceFreeStoreScript`, `LandFreeStoreScript` | Free-store Lua script names (§1.10) |
| `GoalProposalFunctionSets` | Ordered list of GoalFunctions sets |
| `Templates/{Space,Land,Galactic}` | Ordered template lists. **Element 0 is the base template** (§1.7). |
| `Difficulty_Adjustments/{Easy,Normal,Hard}` | Names of DifficultyAdjustments entries (parser 0x60f840) |

**Difficulty**
- `GetAdj` (0x4b0500) returns AIPlayerType+0xd0[index]: 0 Easy, 1 Normal, 2 Hard. A missing entry returns an all-1.0 dummy. [H]
- `Set_Difficulty` (0x4b0dc0) rescales the health, hard-point health and shields of every object the player owns in the current mode by new/old multiplier. The AIPlayer is created at Normal (1). [H]

| DifficultyAdjustment field | Offset | Reader | Effect |
|---|---|---|---|
| Credit_Multiplier | +0x20 | Player::Add_Credits 0x27f370 | multiplies positive credit gains of AI players [H] |
| Galactic / Space / Land_AI_Contrast_Multiplier | +0x34 / +0x38 / +0x3c | 0x5f1990 (galactic), 0x5f1d00 (Land if mode 1, else Space) | scales the enemy force a plan must contrast (§1.9.3) [H] |
| Galactic / Space / Land_AI_Goal_Cycle_Sleep_Duration | +0x40 / +0x44 / +0x48 | goal-system slot 11 (0x6b8630 / 0x6b8950 / 0x6b87b0) | frames slept after each commit = `(int)(FPS × s)` [H] |
| Bribe_Cost_Multiplier | +0x4c | not traced | [L] |
| Health / Shield multipliers | +0x50 / +0x54 | Set_Difficulty | [H] |

Shipped values: the sleep is 0 everywhere except Easy space/land (15 s = 450 frames). Galactic contrast is 0.67 / 1.1 / 1.15. Credit multiplier is 0.5 / 1.0 / 1.2.

**Suspend_AI(x)** (Lua 0x544ec0) stores `x > 0` in story-manager byte +0x82 and sets TheAI master = !suspended. A missing argument un-suspends. [H]

**Galactic pause.** When the game-mode manager's galactic pause counter goes 0→1, galactic AI is disabled, and it is re-enabled on 1→0 (0x28b080, 0x28bb80, 0x28c720, 0x28f590). [H]

## 1.3 Update cadence and time-slicing

**Call chain** [H]
1. Main loop 0x5d990 step T7 runs the PlayerList update 0x297e30 in **player index order**.
2. That calls `Player::Update` 0x287e70, then `AIPlayer::Update` 0x4b0ca0.
3. `AIPlayer::Update` services **only the context of the current top game mode** (0x5f5ab0).

So the galactic AI does not run during a tactical battle. [H]

**Order inside a context each frame** (if TheAI is enabled for the mode): Perception → Goal → Planning → Execution → Learning → Template. A subsystem's Service runs only when `next (+0x30) <= NOW`. Service first reschedules `next = NOW + max(1, (int)(interval × FPS))` (0x64c4d0). [H]

| Subsystem | Interval (slot 3) | Cadence |
|---|---|---|
| Goal | 0.0 s | every frame |
| Template | 0.0 s | every frame |
| Planning | 0.1 s | every 3 frames |
| Execution | 0.1 s | every 3 frames |
| Perception | 10 s | every 300 frames; its Service only reschedules [H] |
| Learning | 10 s | every 300 frames (pruning) |

**AI disabled.** When the mode's AI is disabled or suspended, nothing runs, with one exception: in galactic mode, while the AutoResolve object 0x140b31400 reports busy (its virtual at +0xe0) and Planning is due, Planning alone is serviced. [M]

**Goal Service (0x6cacd0)** extends HOTSPOTS §1:
1. If the countdown +0x18c > 0, decrement it and return.
2. Otherwise run one proposal slice.
3. When the pass is complete, run budget weights (0x614590), then Commit (0x6c8710), then set the per-slice quota:

   `quota = clamp(min(ceil(evalsLastPass / (FPS·5)), (int)(20 / max(1, players − activeHumans) + 0.5)), ≥1)`

   Then reset the countdown from slot 11.

   **Active humans** are players with both +0x62 and +0x180 set (0x294b80). [M]
- **Effect:** a full proposal pass is spread to finish in about 5 s (150 frames) of game time. A commit follows each pass, and then the difficulty sleep applies. [H]

**Wall clock.** The original AI scheduling is **frame-driven**. No QueryPerformanceCounter or timeGetTime call reaches a decision path. [H]
- The prior patch's 6 ms budget and catch-up (PRIOR_PATCH.md) are **not original**. They change which frame a proposal slice or commit lands on, so they are not lockstep-safe. Do not port them.
- **One wall-clock read exists** [H]:
  - The state-machine helper used by Execution blocking statuses and AIBuildTask (0x526590, 0x649200) stamps the state-entry time `(timeGetTime() − base)/1000` into FSM+0x2c.
  - The only reader found is comparator 0x5f83c0, which has no callers. It is probably a diagnostics sort. [L]
  - The port should use frame time (§3, open item 1).

**Port guidance**
- The proposal pass is a pure scoring of (goal type × target) pairs. It may run on workers, provided both of these hold:
  - the **set of candidates scored per frame** (the quota cursor) and the insertion order are reproduced exactly;
  - the RNG-consuming step (plan pick at proposal, §1.5) is done serially in cursor order.
- Commit, Resolve, budget and plan execution are serial.

## 1.4 Goal types, categories and application flags

**Sources**
- Goal types come from `Data/XML/AI/Goals/*.xml`. Each element's name is the goal type.
- Desire functions come from `GoalFunctions/*.xml` sets: `<X><Goal>type</Goal><Function>perception</Function></X>`.
- A player's active goal types are the union of its `GoalProposalFunctionSets`, in list order, filtered by `GameMode`. [M on order]
- One goal type can be fed by several sets. For example `Upgrade_Starbase` gets `Needs_Starbase_Upgrade` from the basic set and `Should_Underworld_Build_Starbase` from the Underworld set. Each (type, function) pair is a separate proposal source; duplicate proposals merge (§1.5). [M]

**Goal-type fields** (table "Destroy_Unit" segment in xml_field_tables.md):

| Tag | Off | Meaning |
|---|---|---|
| AIGoalApplicationFlags | 0x20 | target filter bitfield (below). 0x10 Global = no target |
| GameMode | 0x24 | Galactic / Land / Space |
| Category | 0x28 | AIGoalCategoryType id: budget category and template switch key |
| Reachability | 0x2c | AIGoalReachabilityType; also the galactic move mode (§2.5) |
| Time_Limit | 0x30 | float s, −1 = none. AllowedTime (0x692860) = Time_Limit if no plan, else plan ETA × Build_Time_Delay_Tolerance. It also caps the budget horizon. [H] |
| Build_Time_Delay_Tolerance | 0x34 | multiplier on plan ETA [H] |
| Is_Like | 0x38 | type-equivalence list; stops Resolve's greedy phase (§1.6) [M] |
| Global_Exclusions | 0x90 | while a goal of this type is Active, each named type's exclusion counter (goal system map +0x260) is incremented: excluded types cannot be proposed or activated. Release decrements it (0x693550 / 0x693750 / 0x6ca8c0 / 0x6cb070). Scope: one goal system, i.e. one player and mode. [H] |
| Tracking_Duration | 0xa8 | s; lifetime of goal-outcome history |
| Per_Failure_Desire_Adjust | 0xac | w1, added per recent failure |
| Activation_Tracking_Duration | 0xb0 | s (read as float) |
| Per_Activation_Failure_Desire_Adjust | 0xb4 | w2, added per recent failed activation |

Shipped XML repeats `Tracking_Duration` and `Global_Exclusions` in some goals. Parse them as the parser does: the last write wins for scalars [L]; whether lists append was not checked.

**AIGoalApplicationType bits** (0x5e4f20) [H]:

| Bit | Name | Bit | Name |
|---|---|---|---|
| 0x1 | Friendly | 0x100 | Enemy_Structure |
| 0x2 | Enemy | 0x200 | Tactical_Location |
| 0x4 | Neutral | 0x400 | Friendly_Build_Pad |
| 0x8 | Contested | 0x800 | Enemy_Build_Pad |
| 0x10 | Global | 0x1000 | Friendly_Cash_Point |
| 0x20 | Friendly_Unit | 0x2000 | Enemy_Cash_Point |
| 0x40 | Enemy_Unit | 0x4000 | Friendly_Reinforcement_Point |
| 0x80 | Friendly_Structure | 0x8000 | Enemy_Reinforcement_Point |

- The target's virtual slot 4 `Applies(flags, player)` performs the filter.
- The tactical unit target is 0x724ef0 (A1 doc).
- The galactic planet target's body was not located. The expected test is planet owner relation vs Friendly/Enemy/Neutral/Contested. [L]

**AIGoalReachabilityType** (0x5e70b0, values read from the registration calls) [H]:

| Value | Name |
|---|---|
| 0 | Friendly_only |
| 1 | Friendly_Ignore_Threat [M: heap string, registered last] |
| 2 | Enemy_destination |
| 3 | Enemy_undefended |
| 4 | Single_Hop_Disconnected |
| 5 | Any |
| 6 | No_Threat |
| 7 | Low_Threat |
| 8–10 | Medium_Threat, High_Threat, Any_Threat [M: heap strings, by order] |

Enum names are matched case-insensitively (shipped data writes `Enemy_Undefended`, `Friendly_Only`). [M]

**AIGoalCategoryType** (`Enum/AIGoalCategoryType.xml`, DynamicEnum; list order = XML order) [M]:

| Id | Category | Id | Category |
|---|---|---|---|
| 0 | Offensive | 10 | Macro_Goal |
| 1 | Defensive | 11 | MajorItem |
| 2 | Information | 12 | StoryArc |
| 3 | Infrastructure | 13 | StoryArcHuman |
| 5 | Tactical_Targeted | 14 | Interventions |
| 6 | Tactical_Untargeted | 15 | High_Priority |
| 7 | NoBudget | 16 | Sandbox_Events |
| 8 | Hero | 17 | Med_Priority |
| 9 | Always | 18 | Map_Control |

## 1.5 Proposal (extends HOTSPOTS §1 Propose_Goals_Incremental 0x6cb120)

**Cursor.** Goal types × targets, resumable, capped at `quota` evaluations per call.
- Global types (flag 0x10) evaluate once with no target.
- Others iterate the perception system's target list (slot 8) filtered by `Applies`.
- Candidates skipped by slot 9 do not count against the quota. [H]

**Desire** [H]:

`desire = Evaluate(function, ctx){double} + (double)((float)c1·w1 + (float)c2·w2)`

- c1 is the number of recent **failed** goal outcomes for (type, target), from Learning +0x188 via 0x588870.
- c2 is the number of recent failed **activations** for (type, target), from +0x1c8 via 0x588760.
- The candidate continues only if desire > 0. Desire and priority are stored as `(float)desire`, clamped to [0, 1e18] (0x693730 / 0x693950).
- Note that w1 and w2 are usually negative in the data (−5, −8, −10). Conquer_Pirate's w1 is +1.0.

**Proposability** of a type (0x6ca110) [H]. All three must hold:
- the goal system list +0x2a0 is empty (unknown list; while it is non-empty nothing is proposed or activated);
- the template switch for the type is > 0 (map +0x220, §1.7);
- the type's exclusion counter is 0.

**Slot 9, skip.** Galactic 0x6b84f0 and land 0x6b8710 skip when either holds:
- the goal (type, target) is already in the active set;
- a per-target lookup keyed by the goal's Reachability returns null. This is probably the `Block_Goal_Proposal` mechanism. [L]

Space 0x6b8900 has only the active-set test. [H]

**Slot 10, accept.** Galactic 0x6b8660 [H]:
1. Run `Find_Plan_For_Goal` (0x6bae50, §1.9.2) on the Planning system. This **chooses a plan and solves its PotentialPlan now**, at proposal time. If no plan is available, the candidate is rejected.
2. Then, if `AllowedTime ≤ 0` or `AllowedTime ≥ goal ETA (+0x7c)`, the budget gross-affordability test 0x6139f0 must pass. Otherwise the candidate is rejected.

Consequence: **every accepted-desire candidate consumes one synced RNG draw** (plan pick) during proposal, in cursor order. [M: the call chain is H; the draw is per call, M]

**Insert** (0x721480) [H]
- The proposed set is per category, with a (target → type) index.
- A duplicate (same target and type) is not added twice. The existing entry keeps its position and takes the higher desire (and that source's function pair).

**GiveDesireBonus(player, goalTypeName, target, bonus, seconds)** (0x699ab0) [H]
- It returns a blocking status with `deadline = NOW + (int)(FPS × seconds)`.
- Each poll (0x699eb0) looks for (type, target) in the **proposed** set. If found, it adds `bonus` once (clamped) and finishes with true; otherwise it finishes false at the deadline.
- It is a one-shot additive bump on the next matching proposal. There is no decay.
- The arity check is 5 arguments, although the error message says 4.

**Purge_Goals(player)** (0x69c750)
- Walks the global running-plan list. For each plan that is not the caller's and that passes 0x5f7650, it ends the plan (0x5f6a50, which records the outcome) and removes it.
- Player scoping beyond "player has a planning system" is not visible. [L]
- Used by the tech plans.

**Declare_Goal_Type** is not registered by the exe (LUA_API.md). Keep it as a no-op.

## 1.6 Commit and Resolve_Category (0x6c8710 / 0x6c8970)

**Effective desire** (0x6927f0) is used in every sort and comparison [H]:
- for an already-active goal (state > 0) with 0.01 < d < 40: `d + (1 − (d − 0.01)/39.99) × 1.6`;
- otherwise d.

This is hysteresis favouring incumbents.

**Commit order** [H]
1. Re-evaluate each active goal's function (no learning terms) if its target still Applies, or, for global goals, if the type has 0x10. Overwrite its desire. (0x7222c0)
2. Sort each active per-category vector by effective desire, descending. std::sort (0x720eb0): insertion sort up to 32 elements, so stable; unstable above that.
3. Sort category ids (global list order) by normalized budget weight (cat+0x3c). The direction is probably descending. [M]
4. For each category in that order, each active goal with a PotentialPlan gets 0x6c0be0. That queues production for PP units that need building, once per PP (§1.9.3).
5. Sort each proposed category and truncate it to `template[+0xe8] + |active(cat)|` (0x721b60). Template = AIPlayer+0x28→+0x30. [M that +0xe8 is a template int]
6. Run Resolve_Category for each category in sorted order. Then clear the proposed set.

**Resolve_Category(cat)** [H unless noted]

Notation: A = active vector, P = proposals, `limit = template[+0xe8] + |A|`.

1. **No surviving proposals.** Run *maintenance* (0x722a30) on A and return. Maintenance drops a goal (releasing its budget) when any of these holds:
   - it is Released, or has no plan link;
   - the budget cannot fund it (0x6129c0);
   - effective desire ≤ 0 and it is replaceable;
   - it has a plan that is invalid (0x6c47c0), unless the template flag +0x60→+0xa8 is set;
   - **galactic only:** AllowedTime < ETA and it is replaceable. ETA = 0x525200 + elapsed (NOW − created)/FPS.

   Survivors get their plan refreshed (0x6c2d60) and are re-added to the budget. "Replaceable" (0x692740) means: no running plan, or the plan has not called `Set_As_Goal_System_Removable(false)`.
2. **Filter P.** Keep proposals whose target still Applies, then sort P.
3. **Classify A** against `best = eff(P[0])`:
   - Keep a goal if `eff ≥ best` or it is not replaceable (an invalid plan releases it).
   - Otherwise it goes to OldCand.
4. **Pool.** Pool = OldCand + P, re-sorted. `lastOld` = the lowest OldCand entry.
5. **Greedy phase.** While the category's available credits (cat+0x44) > 0, take Pool[0]. **Stop** on the first of:
   - +0x2a0 non-empty, type disabled, or type excluded;
   - the goal is already kept or active;
   - the plan cannot be activated (0x6c4a50) or the budget cannot fund it (records an activation failure);
   - `0 < AllowedTime < ETA` (records an activation failure);
   - the goal is Is_Like `lastOld`;
   - |Keep| ≥ limit.

   Otherwise accept: refresh the plan; record an activation success if it is new; Activate (state 1, exclusions applied); add it to the budget in galactic mode.
6. **Weighted random phase.** While |Keep| < limit:
   - Build cumulative weights over Pool entries (Pool order) that are fundable, enabled, not excluded, not kept or active, and have eff > 0. Weight = eff, summed in float.
   - If the total is ≤ 0, stop.
   - Otherwise make **one synced draw r ∈ [0, total)** and binary-search the first cum ≥ r. [H draw; M hi = total]
   - Re-check the plan, fund and time. A failure releases the goal and records an activation failure. A success keeps and activates it as above.
7. **Finalize.** Each kept goal goes to Planning (0x6bad60 instantiates its plan, §1.9.4) and into the active set. Pool leftovers that were active are released. New proposals that were not selected are discarded.

The AI budget is only touched in galactic mode (`context+0x34 == 0`; mode 0 = galactic per the context table, [M]). Tactical goals are not money-limited by AIBudgetClass.

**Goal states** (+0x74): 0 proposed, 1 Active, 2 Executing, 3 Released (InstantiatedGoalStateType 0x5e5890). [H]

InstantiatedGoal fields: +0x48 {type, function}, +0x50 desire, +0x54 priority, +0x58 target, +0x60 running plan, +0x68 PotentialPlan, +0x70 creation frame, +0x78 cost, +0x7c ETA, +0x80 budget, +0x90 goal system. [H/M]

## 1.7 Templates (AITemplateSystemClass vt 0x8be5c0; AITemplateClass parser 0x6716f0)

**Template XML** (`Data/XML/AI/Templates/*.xml`):
- `Priority` (default 1)
- `Trigger` and optional `Un_Trigger` (perception names)
- `Budget/<Category>` → budget-weight equation name
- `Tactical_Budget_Allowance/<Category>` → number
- `Turn_On` / `Turn_Off`, each with:
  - `Goals/Category`
  - `Goals/Goal_Type`
  - `Plans/Goal_Category`
  - `Plans/Name`

[H for the tags; M for the struct offsets in the helper notes.]

**Evaluation** (perception context bound to self, the opponent and the first human) [H]:
- Trigger fires when its value is > 0.
- Un-trigger: if `Un_Trigger` exists, when it is > 0; otherwise when Trigger ≤ 0.
- The base template (list element 0) is always active and never un-triggered.

**Init** (0x6bbed0) [H]
1. Activate the base, then every other triggering template.
2. Set the per-frame check count to `max(1, (int)(N / (FPS·10)))`.
3. Initialise the budget (0x6137b0).

**Service** (0x6bc5c0, every frame) [H]
1. Check `count` templates round-robin from the cursor. Activate any inactive template that triggers and append it to the active list.
2. Check every active non-base template. Remove those that un-trigger.
3. If anything was removed, reset all budget category equations (0x612ac0) and **re-apply every remaining active template in list order**.

**Activate** (0x670ed0) writes with priority p = template Priority. A target keeps a value only if `p ≥` the stored priority. [H] The targets are:
- budget category equation (category +0x30, priority +0x40);
- tactical allowance (+0x5c / +0x60);
- goal category or type switch, stored as a signed priority (+p on, −p off) (0x6caf30 / 0x6cadf0);
- plan switches (0x6bb710, 0x6bb670, 0x6bb570).

Deactivate writes −1 everywhere it touched; the re-apply then restores what the other active templates assert. [M]

Goals and plans with no template entry stay at −1, i.e. off. In particular, **a goal category not turned on by any active template is never proposed**, and a plan is only a candidate when its availability value is > 0 (§1.9.2). [M]

## 1.8 Budget (AIBudgetClass embedded at goal system +0x190; galactic only in practice)

**Weights** (0x614590 at each commit; 0x6137b0 at init) [H]
- For each category in list order: `w = Evaluate(templateEquation, initial = previous weight)`. Negatives become 0. Sum in float.
- Normalise `w/sum`, or `1/count` when sum = 0.
- Init sets each category's pool to `playerCredits × w`.
- Shipped budget equations are in `PerceptualEquations/BudgetingEquations.xml` and `AI_Equations_Underworld_Budgeting.xml` (S2 already evaluates them).

**Per-category state:** pool (+0x4c), available credits (+0x44) = pool minus goal allocations, income rate (+0x50), available rate (+0x48), normalized weight (+0x3c).

**Per-goal state** (BudgetedGoalStruct): unallocated request, allocated spend authority, horizon = min(ETA, Time_Limit unless −1), required rate, accrued, covered, bonus.

**Add goal** (0x610d30): the need is first taken from available credits; any shortfall is converted into a required income rate over the horizon, taken from the available rate. [H arithmetic]

**Can fund** (0x6129c0): `max(horizon, lastDt) × availRate + avail ≥ need`. [H]

**Per-tick update** (0x614340, from 0x4b2ef0, galactic only; dt from the game clock) [H/L]:
- Rates are redistributed by weight and goals accrue their share.
- Drift between real player credits and the summed pools is redistributed by weight.
- The exact sign handling of the accrual step is unresolved [L]. A first implementation may use the simpler invariant: Σ pools = player credits, split by weight.

**BudgetWrapper** (Lua `Budget`, galactic plans only) acts on the calling plan's goal [H]:

| Method | Behaviour |
|---|---|
| Allocate_Resources(n) | req −= n; alloc += n |
| Flush_All_Resources | release alloc down to cost; req = 0 |
| Flush_Unallocated_Resources | req = 0 |
| Flush_Category(name) | that category's available = 0 (used by FlushMajorItemBudget) |
| Get_Spendable_Resources | min(alloc, covered + accrued) |
| Get_Unallocated_Resources | req |
| Give / Take_Resources_To / From_Goal(n, type) | moves n between this goal and the same-target active goal of `type` |
| Wait_For_* | blocking statuses, done when value − n ≥ −0.0001 |

**Release** (0x614880): the goal's covered + accrued returns to the category. Committed spend is reclaimed through 0x5f7d40. [M]

**Tactical allowance** (0x6147d0; `PlayerWrapper.Release_Credits_For_Tactical`) [M]
- Credits up to `allowance − released` go to the player.
- While the tactical flag is set, spendable = remaining allowance.
- The shipped Basic_Empire_Default comments its allowance out; None_Template sets 0.

## 1.9 Plans

### 1.9.1 Definitions and loading

**Loading** (0x4e7e30, all modes into one manager) [H]
- Every `*.lua` in `Data\Scripts\AI\`, `AI\SpaceMode\`, `AI\LandMode\`, `AI\StoryArc\` and `Data\Scripts\Interventions\` is loaded, in that folder order and in filesystem enumeration order within a folder.
- The plan name is the file base name.
- **Port:** sort by name within a folder for reproducibility, and record it as a deviation if enumeration order matters. It matters only for plan candidate order, which affects the RNG pick.

**Per file** (0x4e7170) [H]
1. Take a pooled script.
2. Set `PlanDefinitionLoad = true` and call `Base_Definitions()` (PGAICommands.lua: defaults, `Set_Contrast_Values()`, then `Definitions()`).
3. Read these globals:

| Global | Meaning / default |
|---|---|
| `Category` | `" \| "`-separated goal type names. Unknown names are dropped; a plan with none is discarded. |
| `TaskForce` | required; a table of task-force definitions |
| `IgnoreTarget` | no contrast, target ignored |
| `MagicPlan` | default false |
| `MagicPlanStealing` | default true |
| `AdditionalCost`, `MinimumCost` | floats |
| `RequiredCategories` | category masks |
| `AllowFreeStoreUnits` | default true |
| `AllowEngagedUnits` | default true |
| `PerFailureContrastAdjust` | float |
| `MinContrastScale`, `MaxContrastScale` | library defaults 1.1 / 1.5 |
| `EnemyContrastTypes[i]` ↔ `FriendlyContrastTypes[i]` | WeightedTypeList map, paired by index |

**TaskForce entry syntax** (0x66fcc0). The first string is the TF name: the Lua global, the `<Name>_Thread` function and the event-handler prefix. Later strings are `lhs = rhs`, where rhs is `a,b` (min, max), `N%` (percent-based TF) or `N`. Keywords [H]:

| Keyword | Effect |
|---|---|
| `DenyHeroAttach` | no hero attach |
| `EscortForce` | escort task force |
| `TaskForceRequired` | required TF |
| `DenySpecialWeaponAttach` | no special-weapon attach |
| `RequireUniqueStage`, `RequireMatchingStage` | staging constraints |
| `MinimumTotalSize = N` | int |
| `MinimumTotalForce = N` | float |
| `-TypeName` | excludes that type |
| a mask name | unit filter |
| anything else | a team: a category mask (`Frigate \| Capital`) or a `\|` list of type names. Candidate types exclude flagged or non-buildable families. A team with no candidates is dropped. |

**Script pool** (0x244170 / 0x2488e0) [H]
- `ScriptPoolCount`: missing = 1 instance; N = preload N; 0 = not poolable.
- On release: kill the threads, call `Flush_G()` and `Base_Definitions()`, and return the instance to the pool.
- **Port note:** script reuse makes global leftovers part of behaviour. Keep the pool semantics, including `Flush_G` keeping userdata that is `Is_Pool_Safe`.

### 1.9.2 Choosing a plan for a goal: `Find_Plan_For_Goal` (0x6bae50)

Called from proposal slot 10 (§1.5). [H]
1. **Candidates:** plans in the Planning availability list (map +0x48, list +0x50, list order) whose value is **> 0** (set by templates, §1.7) and whose Category list contains the goal type.
2. **Weight** = learning success ratio for the plan (successes ÷ attempts, default 1.0; 0x588b60) + 1.0, accumulated as a cumulative array.
3. **No candidate:** release the goal's PotentialPlan and return false.
4. **Otherwise:** one synced draw in [0, total], then a binary search.
5. Construct a `PotentialPlanClass` (0x210 B) for the chosen plan and run `Init` (0x6c0d00). The goal's PP (+0x68) is replaced only if Init succeeds.

### 1.9.3 PotentialPlan solve (feasibility, unit selection, contrast)

**Init** (0x6c0d00) [H]
- Records the goal, player and mode, plus a per-mode attribute (galactic: a planet attribute, probably for the TerrainType marshaller [L]).
- Runs the solver 0x6c3da0 over the per-mode list of **instantiation marshallers**: Budget, Contrast, FreeStore, Hero, Plan, Production, Reinforcement, Survival, TacticalBuild, TechTree, TerrainType (vtables 0x8bf120–0x8bf3f8, 0x8ce6b0).
- On success: goal ETA (+0x7c) = PP+0x48 and goal cost (+0x78) = PP+0x44.

**Solver** (0x6c3da0, greedy, no RNG) [H]
1. Each marshaller sets up.
2. Loop:
   - For every candidate unit record, sum the marshaller scores.
   - A score of exactly 1e18 is a veto; a marshaller may make it permanent.
   - A candidate whose object already fills ≥ 50 % of the selection is vetoed while alternatives exist.
   - Pick the strictly highest score; ties go to the **lowest index**.
   - Commit it to all marshallers.
   - Repeat until nothing is viable.
3. Each marshaller finalizes. Success is the PP flag +0x4c.

The per-mode marshaller order (data at 0x140a449a0) and each marshaller's score and veto rules are **not yet specified** (open item 3).

**Contrast requirement** (0x6c5de0) [H]
- No target, or `IgnoreTarget` → no contrast.
- Otherwise `adj = recentPlanFailures(plan, target) × PerFailureContrastAdjust` (Learning 0x588980) and `tolerance = 1 − (adj + Min)/(adj + Max)`.
- The enemy force is computed by 0x5f1990 (galactic) or 0x5f1d00 (tactical; a second pass for EscortForce).
- The difficulty contrast multiplier is applied there: `(MinContrastScale + fails × PerFailureContrastAdjust) × multiplier` scales the enemy strength that becomes the desired force. [H per helper; M exact composition]

**Production queue at commit** (0x6c0be0, from Commit step 4) [H]: once per PP, for each selected unit with a production slot, enqueue it on the build queue (0x53bee0).

### 1.9.4 Instantiation and execution (PlanBehaviorClass vt 0x8a8b10)

**Instantiate** (0x6bad60 → 0x5f7090) [H]
1. Get a pooled script.
2. If the goal has a target, create `AITarget` (AI target wrapper) and `Target` (game object wrapper), and listen to the target's signals 0x1a (owner changed) and 0x10 (destroyed).
3. If the script is fresh, call `Base_Definitions()`. Note that `PlayerObject` is still nil at that point. [M]
4. Set `Target` and `AITarget`.
5. **Galactic only** (gate `DAT_140b15418` vf+0xf0 [M]): charge `AdditionalCost`, then create the `Budget` userdata.
6. For each TaskForce definition, in order:
   - create the TaskForce and set its stage = the PP's location for that TF;
   - set the Lua global named after the TF, and `PlayerObject`;
   - create the coroutine `<Name>_Thread`. A missing function removes the TF.
7. Register the TF's event handlers.

There is **no main thread**. The plan joins the Planning system's list.

**Service** (Planning slot 2, 0x6bb550 → 0x725bc0, every 3 frames) [H]
- For each plan in list order: resume all its threads (0x247a90).
- Threads are resumed **in creation order**, each with a pending argument pushed first.
- A thread that yields `true` stays alive (PumpEvents yields true). `false`, a return or an error kills it.
- If `ScriptExit()` sets the exit flag mid-pass, the pass stops immediately.
- A plan ends when the script has exited or has no live threads.

**Lua side** (PGBase.lua, shipped) [H]
- `Sleep(t)` polls `GetCurrentTime()` (game seconds). `BlockOnCommand(block, max, break_fn)` polls `block.IsFinished()` once per PumpEvents.
- **Each PumpEvents is one yield**, so a waiting plan advances once per Planning service (3 frames).
- After resume, PumpEvents drains the thread's `GetEvent` queue in FIFO order.

**Blocking statuses**
- They live in Execution (+0x40) and are serviced every 3 frames (0x526590) in list order.
- Each is an FSM: update, then up to 50 exit/enter transitions per service. Done statuses are removed. [H]
- Execution's Service also runs the 90-frame path-finder recompute (PHASE2_ANSWERS §11) first and the free store second. [H]

**Events** [H mechanism / M names]
- Handler name = `<TFName>_<EventName>` (PGEvents.lua falls back to `Default_<EventName>`).
- Dispatch 0x6006b0(tf, code, params, queued):
  - arguments are `[tf, params…]`;
  - if not queued, the handler is called synchronously; if queued, it is appended to that thread's GetEvent queue.
  - Dispatch is skipped when the script is exiting.
- 24 codes. Names in string order: Production_Failed, Unit_Destroyed, No_Units_Remaining, Original_Target_Owner_Changed, Current_Target_Owner_Changed, Special_Weapon_Online, Hardpoint_Target_In_Range, Hardpoint_Opportunity_Target_Acquired, Unit_Move_Finished, Unit_Ability_Finished, Unit_Diversion_Finished, Unit_Ability_Cancelled, Unit_Ability_Ready, Unit_No_Threat, Target_In_Range, Unit_Damaged, Original_Target_Destroyed, Current_Target_Destroyed.
- Known codes: owner change = 5 (old, new as player wrappers); target destroyed = 10; galactic TF empty = 7 (≈ No_Units_Remaining) [L].

**Plan end** (0x5f6a50) [H]
1. Destroy the plan's blocking statuses.
2. Clean up its free-store entries.
3. `Release_Forces(1.0)` on each TF in order, which returns its units to the free store.
4. Mark the plan finished.
5. Record the learning outcome (0x58a720, flag = the `Set_Plan_Result` value, plan+0xa8, last write wins).
6. Release the PP production reservation.
7. Detach the plan from the goal and return the script to the pool.

**Generic TaskForce methods** (0x5f95..0x600 range; see LUA_API.md for RVAs)

| Method | Behaviour |
|---|---|
| Set_Plan_Result(b) | writes plan+0xa8 |
| Set_As_Goal_System_Removable(b) | false makes the goal non-replaceable (§1.6) |
| Get_Stage() | AITarget of the TF stage |
| Release_Forces(f), 0 ≤ f ≤ 1 | returns a fraction of the units to the free store |
| Produce_Force([location], [bool]) (0x5fe240) | creates a ProduceForce blocking status (id 0x80b) with one **AIBuildTask** per missing unit (0x524ee0; FSM §2.4) |
| Are_All_Units_On_Free_Store, Get_Force_Count, Test_Target_Contrast(b), Block / Unblock_Goal_Proposal | bodies not analysed [L] |

## 1.10 Free store (AIFreeStoreClass vt 0x878f80; Galactic/Space/Land subclasses)

The free store is the pool of the player's units that no plan owns. Plans draw from it (FreeStore marshaller) and release into it.

**Script** (0x4d31e0) [H]
- Pooled script named by the AIPlayerType.
- Main thread `main`.
- Globals `FreeStore`, `PlayerObject`, `LastService = 0`, `LastUnitService = 0`.

**Service** (0x4d5a70, from Execution every 3 frames) [H]
1. Process pending relocations through subclass hooks, issuing moves.
2. `now = NOW × secondsPerFrame` (game time).
3. If the script global `ServiceRate` exists and `now − LastService > ServiceRate`: resume the script threads (`FreeStoreService()`) and set `LastService = now`.
4. If `UnitServiceRate` exists and the same strict test passes: call `On_Unit_Service(unit)` synchronously for every free unit in list order.
5. `On_Unit_Added` is called when a unit is admitted (vf19).

| Shipped script | ServiceRate | UnitServiceRate |
|---|---|---|
| GalacticFreeStore | 8 | none: the script misspells it `UnitServciceRate`, so per-unit service never runs in vanilla galactic. **Preserve this.** [H] |
| GenericFreeStore | 10 | 10 |
| BusyTacticalFreeStore | 20 | 2 |

## 1.11 Learning (AILearningSystemClass vt 0x8a2448)

**Record goal outcome** (0x58a720, at plan end) [H]
- Lifetime attempt/success counters keyed by (goalType, target), goal type, and plan.
- If Tracking_Duration ≠ 0, recent records {success, expiry} are appended keyed by (plan, target) (+0x148) and by (goalType, target) (+0x188).
- Expiry = `NOW + (int)(FPS × dur)`, or −1 (permanent) for dur < 0.

**Record activation** (0x58a5d0, from Resolve) [H]: the same scheme with Activation_Tracking_Duration into +0x1c8.

**Queries** count recent **failures**: goal 0x588870, activation 0x588760, plan 0x588980 (contrast). Success ratios: per type 0x588a90, per plan 0x588b60. [H]

**Service** (0x58b410, every 10 s): prune each recent list up to the first live record. [M]

**Containers.** The maps are pointer-keyed in the original. Iteration effects are order-independent (pruning, saving), but **the port must key them by stable ids** (type index, target index, plan index). [M]

## 1.12 How AI decisions become game actions

- AI and Lua orders are **built as EventData and executed directly** at the current frame. They never enter the network command queue (COMMANDS.md, PHASE4 L1 objects `execute_direct`). [H]
- The issuer is the AI player. Battle-pending FleetManagement blocks still apply.
- Galactic order types used: MoveObjectToObject per hop, FleetManagement ops (land, launch, detach, merge), Production (via AIBuildTask), Invade (`Land_Invade` 0x2b0520), Activate_Ability. [H]
- In the port, AI actions are therefore **deterministic consequences of synced state**. Every machine runs every AI player's logic identically: AI is not owned by one peer.

## 1.13 RNG use (synced stream unless noted), in frame order

All draws use the single synced LCG shared with the rest of the simulation. Their position in the frame is: player index order, then within a player the subsystem order Goal → Planning → Execution → (Learning, Template draw nothing).

| Where | When | Count | Conf |
|---|---|---|---|
| Find_Plan_For_Goal (proposal slot 10, galactic/land) | per candidate with desire > 0 that passes slot 9 and has ≥ 1 candidate plan, in cursor order | 1 float | H chain, M per-call |
| Resolve_Category random phase | categories in budget-weight order; one per iteration while cum total > 0 | 1 float each | H |
| Perception `#` operator | inside any equation evaluation (proposal, commit re-eval, budget weights, template triggers, Lua EvaluatePerception) | per use | H (S2) |
| Lua `FindTarget` / `Reachable_Target` | plan or free-store script | 1 per call (HOTSPOTS §2) | H |
| Lua `GameRandom(...)`, `GameRandom.Get_Float()`, `DiscreteDistribution.Sample` | scripts (e.g. `GalacticAttackAllowed`, free store about 10 % unit moves) | per call | H |
| Refit_To_Definition fill loops (galactic) | team pick float + type pick int per unit | variable | H |
| `GameRandom.Free_Random` | scripts | **unsynced** stream; must not affect sim | H |

None draw in: proposal scoring, commit sort, the greedy phase, the PP solver, budget, templates, or learning. [H]

## 1.14 Determinism and the lockstep CRC

- **The CRC has no AI term.** The sync CRC is Σ object CRCs + CRC-32(RNG state) + a player term (COMMANDS.md). AI internals are not hashed, but every AI draw moves the RNG state and every AI order changes objects, so **AI divergence shows up in the CRC on the next check**. Single player never computes the CRC (PHASE4 Q2). [H]
- **Port rules:**
  1. Per-frame cadence exactly as §1.3, including the proposal quota formula (it depends on the evaluation count of the previous pass).
  2. Candidate, plan, category and Pool orders exactly as specified; stable/unstable sort behaviour as noted. For sorts above 32 elements, use a stable sort with a documented tie rule and treat it as a deviation that is safe only if both sides use it.
  3. Float accumulation in list order; desire math in double and then float as in §1.5.
  4. Pointer-keyed containers → id-keyed.
  5. The FSM wall-clock stamp → frame time.
  6. Coroutine resume order = creation order.
  7. The Lua VM uses creation-serial hashing (L1), so `pairs()` order in scripts is reproducible.
- The engine's existing **"ai.perception" CRC hasher** (S2) should be complemented by an "ai.goals" hasher (active goals by category: type, target, desire bits, state) for the A2 test goldens. That is a port-only diagnostic, not original.

## 1.15 Save/load (brief)

| Object | Save / load | Contents |
|---|---|---|
| AIPlayer | 0x4b0af0 / 0x4b0750 | type name, difficulty, then one chunk per context [H] |
| Context | 0x5f5830 / 0x5f5680 | initialised flag, mode, opponent, actor, then subsystem chunks 1–6 in factory order [H] |
| ServicedAI base | — | next-service frame [H] |
| Template | — | cursor, count, base name, active names; it does not save the base next-frame [M] |
| Goal system | — | cursors +0x178..+0x18c (chunk 131075) [H]; goals chunk 0xb, PP chunk 0xc [H] |
| Learning | — | saved, then rebuilt after load (pointer keys) [M] |

Loading original saves is out of scope (Gabriel, 2026-10-29; PHASE4 Decisions), so the original AI chunk layout needs no RE pass.

---

# §2 Galactic AI

## 2.1 Galactic goal set (shipped FoC data)

**Which sets are active**
- BasicEmpire / BasicRebel use the Basic* sets plus ExpansionGeneric.
- AI_Player_Underworld uses AI_GoalSet_Underworld_Galactic plus the basic offensive and defensive sets (no Basic infrastructure or information).
- *NoGalacticAI* and None have no galactic sets and use `None_Template`.

**Templates**
- `Basic_Empire_Default` (base) turns on the goal and plan categories Always, Offensive, Defensive, Information, Infrastructure, Hero and StoryArc, and gives budget equations to Offensive, Infrastructure, Information and MajorItem (Defensive, Hero and Always are Zero).
- `Tech_Upgrade` (priority 4) is triggered by `Empire_Tech_Trigger` and un-triggered by `Empire_Tech_Untrigger`. It turns on MajorItem and shifts the budget.
- `Shutdown_Offense` (priority 3) is triggered by `Should_Shutdown_Offense_For_Story_Campaign` and turns off the conquest goal types.

Goal types (Category | Application | Reachability | Time_Limit s → desire function → plans):

| Goal | Cat | Apply | Reach | TL | Function | Plan(s) |
|---|---|---|---|---|---|---|
| Conquer_Opponent | Offensive | Enemy | Enemy_destination | 240 | Should_Conquer_Opponent_Planet | ConquerOpponentPlan, CrushPlan |
| Conquer_Pirate | Offensive | Enemy\|Neutral | Enemy_destination | 90 | Should_Conquer_Pirate_Planet | ConquerPiratePlan |
| Conquer_To_Reconnect | Offensive | Enemy\|Neutral | Single_Hop_Disconnected | 90 | Need_To_Reconnect_Islands | ContrastToReconnect |
| Raid | Offensive | Enemy\|Neutral | Enemy_Undefended | 180 | Is_Good_Raid_Target | RaidPlan |
| Weaken_Planet | Offensive | Enemy | Any | 60 | Is_Good_Weaken_Target | SabotagePoliticalControlPlan, SiphonCreditsPlan |
| Unrestricted_Grab_Space / _Land | Offensive | Enemy | Enemy_destination | 240 | Should_Perform_Unrestricted_Grab_* | UnrestrictedGrabSpace / Land |
| Death_Star_Use | Offensive | Enemy | Enemy_destination | 240 | Want_To_Fire_DS | DeathStarUsePlan |
| Lift_Blockade | Offensive | Friendly | Single_Hop_Disconnected | 500 | Needs_Blockade_Lifted | LiftBlockadePlan |
| Build_Space_Forces / Build_Ground_Forces | Offensive | Global | Any | 200 | Allow_Blind_*_Production | BuildSpaceForcesPlan / BuildGroundForcesPlan |
| Flush_MajorItem_Budget, Reclaim_Excess_MajorItem_Budget | Offensive | Global | Any | 0 | Need_To_Flush… / Can_Reclaim… | FlushMajorItemBudget |
| Advance_Tech_Empire | MajorItem | Global | Any | 120 | One | EmpireAdvanceTechPlan |
| Advance_Tech_Rebel | Offensive | Global | Any | 0 | Can_Afford_Steal_Tech | RebelAdvanceTechPlan |
| Distant_System_Path_Bonus | Always | Global | Any | 0 | One | PathBonusForDistantTargets |
| Scout_Planet | Information | Enemy\|Neutral | Any | 200 | Needs_Scouting | PlantSpyPlan |
| Upgrade_Starbase, Build_Initial_Starbase_Only | Infrastructure | Friendly | Any | 30 | Needs_Starbase_Upgrade / Needs_Initial_Starbase (+Underworld variants) | UpgradeStarbasePlan |
| Build_Initial_Groundbase_Only, Build_Base_Component_* (Barracks, Light / Heavy / Advanced Vehicle Factory, Officer Academy, Comm Array, Research Facility 1 (MajorItem) / 2), Build_Economic_Structure, Build_Cantina, Build_Scanner, Build_Infiltrator_Facility, Build_Turbolasers, Build_Base_Shield, Build_Jamming_Station, Build_Weapon | Infrastructure (Research_Facility = MajorItem) | Friendly | Any | 30 | Need_* / Needs_* | Build*Plan of the same name |
| Generate_Magic_{Space,Ground}_Defense[_Easy\|_Hard], Generate_Magic_Ground_Structure, Generate_Magic_Cash_Drop | Defensive | Friendly / Global | Any | 30 / −1 | **Zero** in the basic set | GenerateMagic* (`MagicPlan = true`). They are never proposed by the basic set; story or other sets drive them. [M] |
| Remove_Smuggler | Hero | Friendly | Enemy_Undefended | – | Needs_Smuggler_Removed | DeployUnitOnTarget |
| Remove_Corruption | Hero | Friendly | Friendly_Only | 60 | Should_Remove_Corruption | AI_Plan_ExpansionGeneric_RemoveCorruption |
| Underworld: Corrupt_Planet (Infra), Sabotage_Planet (Off) | | Enemy | Any | 60 / 90 | Should_* | AI_Plan_Underworld_DeploySaboteur |
| Underworld: Black_Market_Purchase, Hack_Super_Weapon | Hero | Enemy | Any | 0 | Should_* | …_BlackMarket, …_HackSuperWeapon |
| Underworld: Build_Underworld_{Palace, Barracks, Vehicle_Factory, Droid_Works} | Infrastructure | Friendly | Any | 30 | Should_* | AI_Plan_Underworld_Build* |

Notes:
- Several shipped plans have `Category = "AlwaysOff"` (BuildGroundDefensiveStructure, BuildHuttPalace, BuildSpaceDefensiveStructure, UpgradeGroundBase). They are dead.
- The Hero category has budget **Zero** in the base templates, so Hero goals can only be served by plans that need no money. The XML comment says the Death Star "never receives any budget" and relies on MajorItem purchasing.

## 2.2 Targets

- **Galactic perception target list** (perception slot 8 = 0x4e1e00; built in 0x4e2060) has exactly **one PlanetAITargetLocation per planet** (0x665530, 0x38 B, vt 0x8b4780), in GOM category-3 order. This is the same node order as the path finders (PHASE2_ANSWERS §1). **There are no fleet targets in galactic mode.** [H]
- Each target owns a PlanetReachabilityClass (0x663530) with 6 levels; a level above 5 means unreachable (1e18). [H]
- Goal-system slot 8 (0x6b8590) maps any object to its planet target: the parent planet, the fleet's planet, or itself. Used by GiveDesireBonus and others. [H]
- The `Reachability` of a goal is mapped onto PlanetReachability levels for unit filtering (Refit, 0x664210). The mapping of threat levels 6–10 onto levels 0..5 is open. [L]

## 2.3 Galactic perception inputs

- All desire, budget, template-trigger and contrast numbers come from perception equations. The S2 evaluator already parses all 320 FoC equations.
- Equation files used by the galactic goal set: `BasicGalacticEquations`, `OffensiveGalaticEquations` (sic), `DefensiveGalacticEquations`, `InfrastructureGalacticEquations`, `InformationGalacticEquations`, `BudgetingEquations`, `HeroEquations`, `Story_Empire_Equations`, and the Underworld / ExpansionGeneric galactic files.
- Script-only perceptions are called from plans and the free store, e.g. `Is_Good_Ground_Grab_Target`, `Num_Enemy_Land_Territories`, `Num_Enemy_Space_Territories`, `FriendlySpaceForceStrength`.
- Each evaluation binds **self, opponent (context +0x30), global and target**.
- **What A2 needs from S2/S3:** the token families those equations reference, in particular per-planet force strengths, ownership, starbase level, structures, income, connectivity islands, tech level, credits, and the fogged "last seen" memory. The open S2 items (EnemyForce provisional, Game.Age and others) block exact desire parity.

## 2.4 Production and build decisions

There is no central "build order" logic. Production comes from goals:

**Blind production**
- `Build_Space_Forces` / `Build_Ground_Forces` (Global, desire from `Allow_Blind_*_Production`).
- Their plans define a percent-free `ReserveForce` with `Corvette = 0,4`, `Frigate = 0,4`, `Capital = 0,4` and `AllowFreeStoreUnits = false`.
- They call `Produce_Force()` and `Set_As_Goal_System_Removable(false)`.
- Produced units land in the free store for later offensive goals.

**Structures and upgrades:** the Build* / Upgrade* plans on Friendly planet targets.

**Force for attacks**
- The conquest plans' `Produce_Force` builds what the PotentialPlan selected but could not take from the free store.
- When all units are on the free store, `AssembleForce(tf)` stages them: `Get_Stage()`, then `Produce_Force(stage)` gathers them at the stage.

**AIBuildTask** (0x6478c0 ctor, FSM 0x64aec0, 0xe8 B) [H for the states]
- Main states: INIT_TASK 1 → WAIT_FOR_CREDITS 0x11 → BUILD_TASK_LIST 0x12 → PRODUCTION_QUEUED 2 → … → FREESTORE_ALLOC 3 / HIJACK_UNIT 0x17 → PATHING_DEST → FLEET_MERGE → … → PRODUCTION_FINISHED / PRODUCTION_FAILED 7 → ALL_FINISHED.
- Other states: PRE_PATH_SEPERATE 0xe, WAIT_FOR_FREESTORE_ALLOC 0x16, GROUND_UNIT_PRODUCTION, PRODUCTION_BEGIN / END, DEPENDANT_WAIT, WAIT_FOR_ALL, TRANSPORTS_TO_ORBIT, READY_TO_MERGE, DO_TASK_FORCE_MERGE, JOIN_TASK_FORCE, WAIT_FOR_SYNC_MOVE, SYNC_MOVE_COUNTDOWN.
- In galactic mode, BUILD_TASK_LIST submits a planet build request (0x6954b0 / 0x695720 → 0x53b7d0, AIPlanetBuildQueue).
- PRODUCTION_QUEUED fails with Production_Failed when:
  - the type cannot be built (0x2ac320); or
  - the planet's structure slots are full (0x3f8af0 / 0x3f8b10 vs 0x372740 / 0x372760).
- Payment is the AI PAYG path (GALACTIC_ECONOMY.md); the Production command executes directly.
- **The planet choice inside AIPlanetBuildQueue is not yet traced** (open item 4).

**Refit_To_Definition(planet, budget)** (0x6cfbe0; percent-based TFs only) [H]
- `N = int(W/Wb × Max_Ground_Forces_On_Planet)`.
- With budget > 0 it fills:
  1. pick a team by synced float roulette (N < 1: cost-proportional weights; else deficit weights);
  2. pick a type uniformly with the synced int, rejecting types that are unbuildable for the faction, cost ≤ 0, above the player's tech level, or unreachable for the goal;
  3. stop when the budget is below the unit cost.

**Tech**
- Empire: `Tech_Upgrade` template + `Advance_Tech_Empire` (MajorItem) → EmpireAdvanceTechPlan uses `Purge_Goals` and `Produce_Force` of tech objects.
- Rebel: `Advance_Tech_Rebel` (Offensive, `Can_Afford_Steal_Tech`) → RebelAdvanceTechPlan.
- FlushMajorItemBudget releases MajorItem money with `Budget.Flush_Category("MajorItem")`.

**Magic plans** (`MagicPlan = true`) spawn defenses or cash (`PlayerObject.Give_Money`) without production. They are difficulty-specific variants.

## 2.5 Fleet movement and invasion

**GalacticTaskForceClass** (vt 0x8bfec0) tracks a FleetObject (+0x120), adopted from the first unit's fleet and re-elected when fleets die (signal handler 0x6cf810). Every order first requires a valid FleetObject and a non-empty TF; otherwise it logs and returns nil. [H]

**Move_To(dest)** (0x6cf370) creates a MovementBlockStatus (0x6aed50, id 0x808) with **mode = goal Reachability**. [H]
- Each service waits while the galactic pause counters are > 0. It then runs the A* `Find_Path` (0x3f0010) with request flags derived from the mode (GALACTIC.md §3 table) and takes **one hop**, issued as a **MoveObjectToObject executed directly**. [H; M exact mode→flag table]
- If the next hop holds a hostile presence and is not the destination, the move stops when mode < 2. A faction with `Benefits_From_Corruption` instead marks the fleet and continues. [H]
- On arrival it runs the enemy-presence test and `Initiate_Space_Conflict` (0x2af7c0). `Force_Test_Space_Conflict` (0x6cd440) does the same on demand and waits for signal 0x12. [H]

**Other galactic orders**

| Order | RVA | Behaviour |
|---|---|---|
| `Raid` / `Is_Raid_Capable` | 0x6cf590 / 0x6ceaa0 | Move_To with the fleet raid target set. The capability test 0x432e20 applies the Raid_Force limits. [H / M] |
| `Invade()` | 0x6ce600 | Requires the fleet in orbit and an accessible surface. `GameObjectManager::Land_Invade` (0x2b0520) with the fleet's transport ids. Returns InvadeBlockStatus (id 0x80a). [H] |
| `Land_Units()` | 0x6cec80 | Up to Max_Ground_Forces_On_Planet: units land directly, or transports detach (FleetManagement op 2) then land (op 5). [H] |
| `Launch_Units()` | 0x6cf070 | Launch (op 4) then form a new fleet (op 0). Aborts if blockaded. [H] |
| `Form_Units()` | 0x6cd5b0 | Merge other fleets into FleetObject (op 3). [M op name] |

FleetManagement op names are inferred [M].

**Shipped conquest flow** (ConquerOpponentPlan; Pirate, Reconnect and the Grab plans are similar). Task forces:
- SpaceForce: ≥ 4 units, force ≥ 2500, any ship class.
- GroundForce: ≥ 4 units, force ≥ 750.

`RequiredCategories` = Infantry plus a capital-class ship. The two threads interact as follows:
1. **Space:** assemble or produce; `Move_To(Target)`. Losing everything gives result = false.
2. **Ground:** assemble; `Land_Units`; wait for space secured; `Launch_Units`; re-check `Is_Good_Ground_Grab_Target`; `Move_To(Target)`; `Invade`.
3. **On success:** non-removable; `Release_Forces`; `FundBases` (GiveDesireBonus +15 for 5 s to `Build_Initial_Starbase_Only`, then wait for the starbase, then the same for groundbase).
4. **Owner change of the target to a non-neutral third party:** exit (on Hard, always).
5. **Pacing between attacks:** `GalacticAttackAllowed(difficulty)` uses GlobalValue counters and `GameRandom`. The AI waits until the opponent gains N territories, where:

   | Difficulty | N range | Attacks granted |
   |---|---|---|
   | Easy | 4–8 | up to 2 |
   | Normal | 3–6 | up to 2 |
   | Hard | 1–2 | up to 4 |

   Otherwise it `Sleep`s `DifficultyBasedMinPause` = 300 / 200 / 45 s after a conflict. **This is the main galactic difficulty knob, and it lives in Lua.**

## 2.6 Conflicts (AI vs AI, AI vs human)

- AI attacks start only from MovementBlockStatus arrival or `Force_Test_Space_Conflict` (space), and from `Invade` (land). [H]
- Resolution is in BATTLE_TRANSITIONS.md:
  - **AI vs AI always auto-resolves immediately.**
  - With a human involved, only the human gets the battle dialog.
  - In MP with an AI, the battle auto-resolves.
- While auto-resolve is busy, only the galactic Planning subsystem keeps running (§1.3). [M]
- **No galactic retreat decision exists for the AI.** Plans give up through `Set_Plan_Result(false)` / `ScriptExit`; tactical retreat belongs to A1. [M]

## 2.7 Heroes, Underworld and special plans

There is no hero-specific engine code in GalacticTaskForceClass. Hero plans are ordinary plans whose effects come from unit abilities on arrival. [M]

| Plan | What it does |
|---|---|
| PlantSpy, SiphonCredits, SabotagePoliticalControl | `Move_To` |
| DeploySaboteur, BlackMarket | `Move_To` + `Activate_Ability` (direct command) |
| RemoveCorruption | `Activate_Ability` |
| HackSuperWeapon | `Move_To` + `Produce_Force` |
| DeathStarUse | `Move_To`, then the normal battle path |

- Hero placement while idle: GalacticHeroFreeStore (`Is_Home_Planet`, `No_Threat`).
- Hero-category goals get budget Zero (§2.1).

## 2.8 Galactic free store

**Admission** (0x68ece0): own units that are not planets and not fleets at a planet. **Initial fill** walks planets in GOM order (surface objects, then orbiting fleet members). [H]

**Unit hook** (0x68fde0) [H]
1. Prune finished moves.
2. **AI only** (`!Is_Human`): auto-land transports that are safe, not in transit, at a landable planet under the ground cap.

**Lua helpers**
- `Is_Unit_In_Transit` (0x68efb0): true with no parent, in a fleet not at a planet, with a hyperspace destination, or with a pending free-store move.
- `Is_Unit_Safe` (0x6902b0): only ground units in orbiting fleets can be unsafe.
- `Move_Object(unit, dest)` (0x68f5b0):
  - fails while paused or a battle is pending, or when 0x3fa420 on the current planet is false (`Is_Planet_Revealed`: the planet is not revealed in the local reveal state; always true in multiplayer; GALACTIC_REVEAL_CORRUPTION §1.1, A2GAL_READINGS §2.3; formerly read here as "restricted planet");
  - launches landed transports;
  - **splits the unit into its own fleet**;
  - moves hop by hop with the **AI execution path finder** (PHASE2_ANSWERS §10, mode Friendly_only), executing MoveObjectToObject directly, only through hops with no other owner present.

[H]

**GalacticFreeStore.lua** (ServiceRate 8 s) moves about 10 % of free units per service, chosen by `GameRandom.Get_Float`, toward perception and `FindTarget.Reachable_Target(..., "Friendly_Only")` targets.

## 2.9 AI vs human differences (galactic)

| Difference | Source |
|---|---|
| PAYG production | GALACTIC_ECONOMY.md |
| Credit_Multiplier on gains | GALACTIC_ECONOMY.md |
| Galactic_Build_Time_Multiplier | GALACTIC_ECONOMY.md |
| No Max_Build_Queue | GALACTIC_ECONOMY.md |
| AI refund × multiplier quirk | GALACTIC_ECONOMY.md |
| Fog: the AI branch of `Is_Planet_Passable_For_Player` / `Edge_Cost` uses perception, and `AIUsesFogOfWarGalactic = False` | GALACTIC.md §3, PHASE2_ANSWERS §5–6 |
| Health/shield/damage multipliers | Set_Difficulty / modifiers |
| Goal-cycle sleep, contrast multiplier | difficulty (§1.2) |
| Free-store auto-landing | AI only (§2.8) |
| Attack pacing | Lua (§2.5) |

AI orders bypass the command queue but obey the same rules as human orders. [H]

---

# §3 Open items

> **Status 2026-10-30:** Items 3, 4, 6, 7 and 8 are closed by AI_GALACTIC_2 (its line 7: marshallers §1, production §2, Block_Goal_Proposal §5.1, budget §3, event table §5.2) and AI_GALACTIC_3 (HIJACK_UNIT §4, planet order §5, terrain §6). The marshallers exist in `engine/ai/src/galactic_pp.cpp` (FreeStore, Budget, TechTree, Plan, Production, Hero, TerrainType, Contrast, Survival; list at lines 977-978).

1. **Wall-clock FSM stamp** (FSM+0x2c via timeGetTime in 0x526590 / 0x649200): confirm that no plan or Lua path reads it. Port: use frame time.
2. **Plan pick at proposal:** confirm `0x532480` hi = cumulative total in both pick sites (0x6bae50, Resolve), and that 0x6bae50 runs once per slot-10 call including single-candidate cases.
3. **Instantiation marshallers:** the per-mode order (data 0x140a449a0) and each marshaller's score and veto rules: FreeStore (AllowFreeStoreUnits / AllowEngagedUnits / MagicPlanStealing), Budget (MinimumCost / AdditionalCost), Contrast (tolerance DAT_140b3a544, MinimumTotalSize / Force, RequiredCategories), Production, Hero, TechTree, TerrainType. This is the largest remaining gap for A2 parity.
4. **AIPlanetBuildQueue** planet choice (0x53b7d0 / 0x695720), AIBuildTask hijack (0x648370) and ground production (0x648120).
5. PlanetAITargetLocation `Applies` body (planet ownership vs application flags); the consumer of Reachability levels; the threat-level → PlanetReachability mapping (0x664210).
6. Goal-system list +0x2a0 (blocks all proposals when non-empty); goal ETA writer and 0x525200; the template int +0xe8 (extra goals per category).
7. Budget per-tick accrual sign (0x614340); release refund path 0x5f7d40 / 0x289050.
8. Event code ↔ name table (DAT_140b3a208, static initialisers).
9. Bodies of Test_Target_Contrast, Block / Unblock_Goal_Proposal, Collect_All_Free_Units, Are_All_Units_On_Free_Store.
10. Opponent selection details (faction +0x20a8); AIPlayer+0x50; TheAI+0xc; Purge_Goals player scope.
11. Plan file enumeration order: the original uses filesystem order. Decide the port order and record it.
12. A probe (tools/probe_mod) logging the per-frame synced RNG count during a galactic AI-vs-AI run would validate §1.13. Gabriel must launch it.

# §4 Summary for the implementer

**Key rules**
- **Frame-driven, single-threaded semantics.**
  - Only the current mode's context runs, players in index order.
  - Inside a context: Perception → Goal → Planning → Execution → Learning → Template.
  - Goal and Template run every frame; Planning and Execution every 3 frames; Perception and Learning every 300 frames.
  - The proposal pass is time-sliced by an evaluation quota (about 5 s per pass), followed by a commit and a difficulty sleep (0 in vanilla galactic).
  - There is no wall-clock budget in the original.
- **Goal lifecycle**
  1. Propose: desire = perception + learning penalties; a plan is picked and solved at proposal.
  2. Commit: re-score incumbents with a +1.6 hysteresis.
  3. Resolve per category in budget-weight order: greedy, then weighted-random; capped at template extra + active count.
  4. Plans run as Lua coroutines, one per task force.
  5. Outcomes feed learning.
- **Templates switch everything:** goal and plan categories are off unless a template turns them on, and budget weights come from template equations.
- **Determinism:**
  - all choices draw from the one synced LCG in a fixed order (§1.13);
  - AI orders execute directly and identically on every machine;
  - the CRC sees AI only through RNG and object state.

**What to build first**
> **Status 2026-10-30:** This is a finished build order. Its pieces exist in `engine/ai/src` (`ai_goals.cpp`, `ai_templates.cpp`, `ai_budget.cpp`, `ai_plans.cpp`, `ai_learning.cpp`, `ai_lua_runtime.cpp`, `ai_free_store_path.cpp`, `galactic_pp.cpp`, `galactic_build.cpp`), and `alamo_headless_galactic_ai` / `alamo_headless_galactic_ai_human` are registered in `engine/apps/CMakeLists.txt:127-134`.

1. AIPlayer, context and subsystem scaffolding with the exact cadence (§1.1–1.3), plus AIPlayerType / DifficultyAdjustments / Templates / Goals / GoalFunctions loaders.
2. Goal system: proposal with the quota cursor, commit and resolve, Global_Exclusions, learning counters (§1.4–1.6, §1.11), driven by the existing S2 perception evaluator.
3. Template system and budget weights (§1.7–1.8). Start with the simple "pools = credits × weight" invariant and refine per open item 7.
4. Plan loader, script pool, Find_Plan_For_Goal and a **first-cut PotentialPlan solver** (greedy, lowest-index ties) with FreeStore + Production + Contrast marshallers. This is open item 3 and should get an RE pass before parity claims.
5. Plan execution on the L1 Lua bridge: TaskForce globals, `<Name>_Thread` coroutines, events, BlockingStatus FSMs, `Release_Forces`, then the galactic TaskForce orders (§2.5) through `execute_direct`, and AIBuildTask through the S3 production queues.
6. Galactic free store (§2.8).
7. **Exit test for A2:** a headless AI-vs-AI campaign (auto-resolve) with per-frame CRC goldens identical across thread counts and compilers. Players must only issue legal orders: every order goes through the same validated EventData paths as commands.
