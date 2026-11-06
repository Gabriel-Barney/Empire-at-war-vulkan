# A2h t03 / t04 / t07: ActiveGoals, BudgetFractionToBuild, Maintenance (StarWarsG.exe FoC x64, all RVAs; 2026-11-06)

Tags as in the other `docs/re` pages: [H] read from code/data, [M] one inferred link, [L] unverified. No decompiled code reproduced.
"Port" = `engine/ai/src/galactic_ai_tokens.cpp` / `galactic_ai.cpp` at `d0087c4`. FPS = 30 (`DAT_140b0a340`, the same divisor
turns frames into seconds in the queue timer 0x4ad310). "Self" = the binding set's Self (bindings +0x58 -> +0x50); "root player" =
the player the token is evaluated on (evaluator +0x98).

## 1. t04: ActiveGoals 0x6e0490

**Retail** [H unless tagged]
- Parameter read: 0x5f4780(bindings, `Parameter_Category` id `DAT_140a43c60`) is a plain lookup with **no default**: it succeeds only
  when the parameter is present and holds a single scalar value (the `LuaValue<void*,10>` kind that 0x5f3aa0 creates for this id).
  When it fails, 0x6e0490 returns **false: the token (and so the equation) fails**. It does not mean "all categories". The "default
  all bits" of the unit-category tokens comes from the other reader 0x5f4530, which ActiveGoals does not use.
- Count: 0x721f70(goal system +0x38, (int)value) is an **exact key lookup** in the goal system's active set (a hash map
  category id -> vector of InstantiatedGoal*, STRUCTS.md "+0x38 current set"; +0xd8 is the proposed set). It returns that vector's size,
  or 0 when the key is absent. It is not a mask test. The key is the goal **type's** `Category` (goal type +0x28, the key Insert / Commit
  file goals under, AI_GALACTIC §1.4-1.6), not the plan's.
- The value is the parsed `AIGoalCategoryType[...]` expression. That enum is a plain id list (Offensive 0 ... MajorItem 11 ... Map_Control
  18; `Enum/AIGoalCategoryType.xml`), unlike the bitfield `GameObjectCategoryType`. So `AIGoalCategoryType[MajorItem]` = 11 and counts
  only category 11. A `[A|B]` form would OR the ids (the DLL's bracket `|`, as in the shipped `GameObjectCategoryType[Fighter|Bomber]`)
  and look up that single number [M: the OR is the DLL's, not read; e.g. `[Offensive|Infrastructure]` = 0|3 = 3 = Infrastructure only].
  A repeated `Parameter_Category = ...` would make a list value, and the single-value lookup fails, so the token fails [M].
- Unknown name (`AIGoalCategoryType[Foo]`): the name is resolved by PerceptionFunctionG.dll through the exe's
  `DynamicEnumConversionClass<AIGoalCategoryType>` (loaded by 0x46b950 from `Data\XML\Enum\`) while the equation is parsed. The exe-side
  token never sees a name. Most likely an equation parse error (equation unusable, so every evaluation fails) [L]. No shipped equation does
  this.
- States counted: every entry of the active vector. There is no state test (0x721f70 only takes the vector length). Goals enter the
  vector at Resolve's finalize (AI_GALACTIC §1.6 step 7) and leave at the next commit (maintenance drops Released / plan-less goals,
  Resolve releases displaced ones). So Active (1), Executing (2) and a goal Released (3) since the last commit all count [M: no other
  remover of +0x38 was looked for].
- Scope: the root player's own goal system for the **current mode**. 0x4b06d0(root player +0x360) returns the AI context whose mode equals
  the current game mode (0x28a950), and the token reads its +0x40 goal system. There is no Self check, so `Variable_Enemy.ActiveGoals` would
  read the enemy AI's goals. A root player without an AI system, or without a context for the current mode, is dereferenced without a null
  check (crash) [M]. The only shipped use is `Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[MajorItem]}`
  (`Can_Reclaim_Excess_MajorItem_Budget`). Leaf lifetime 0 (live).

**Port change.** In `ACTIVEGOALS` / `GalacticAiWorld::Impl::active_goals`:
1. No `Parameter_Category`, or more than one `Parameter_Category` entry: return **failure** (not "every category").
2. One parameter with one or more names: OR their **enum values** (`AiDefinitions::category_value`, not the list index) and count the
   active goals of the category whose enum value equals the result (0 when none has it).
3. Unknown name: fail [L]. Fail too for a root player without a PlayerAi.

Steps 2 and 3 give the same result as today for every shipped equation. Only step 1 changes behaviour, and only for equations that no
shipped file contains.

## 2. t07: BudgetFractionToBuild 0x667120

**Retail** [H unless tagged]
1. Root player ≠ Self: 0.0 (success). No goal system (bindings +0x58 -> +0x40 perception system -> +0x28 context -> +0x40): fail.
2. `Parameter_Category`: 0x5f4780 with default -1, result ignored, so absent = -1 = "all". `Parameter_Type`: the string 0x5f41e0, then the type
   by name 0x407c00. Absent or unknown type: **fail**.
3. Start values: R = (float) cost(type, no planet) (0x3711c0 = `Build_Cost_Credits`), T = btime(type, no planet) (0x371230, the T0 of
   GALACTIC_ECONOMY §2.3), inprog = false. Queue index q = 0 when cost ≥ 1 and (`Required_Ground_Base_Level` +0xF0C ≠ 0 or behaviour
   0x5F DUMMY_GROUND_STRUCTURE), else 1 (the same split as the planet queues, GALACTIC_ECONOMY §2.1).
4. In-progress scan: for every planet in the perception target list (system vf+0x40, list order), on its production behaviour
   (behaviour 6), take the **head entry of queue q only** (0x401960; an empty queue is skipped). The entry matches when its type is this
   type and its player id (+0x20) is Self's (+0x4c). For a match:
   - p = clamp(1 − entry.remaining_cached (+0x1c) / entry.build_time (+0x10), 0, 1) (0x498fd0; the remaining seconds cached at the last
     advance, not live);
   - rem = max(0, entry.timer.start + entry.timer.duration − now) / 30 (0x4ad310 on +0x14 / +0x18; live);
   - Q = the planet's AIPlanetBuildQueue for queue q (0x559b40(planet +0xD0, q)), task = Q +0x18 (the AI build task producing there).
     - **No task: the whole token returns 0.0 at once.**
     - Task ended (+0xA8 set by 0x64ad00): if it was reclaimed by the PAYG reclaim (+0xD9) the whole token returns 0.0, else this entry is
       ignored and the scan continues.
     - Otherwise let G = task +0xA0 -> +0xF0 -> +0x88 (the plan's goal) and S = G's BudgetedGoalStruct (G +0x80 budget, 0x612bd0):
       - R = min(R, (1 − p) × cost)  (float);
       - R = min(R, S.alloc (+0x2C))  (0x6928c0; 0 when G has no budget);
       - R = R − min(S.alloc, S.a (+0x38) + S.c (+0x3C))  (0x692ca0 = 0x613690, the "have" of the installment rule);
       - T = min(T, rem);  T = min(T, max(S.h (+0x30), 0))  (0x692840; 0 without a budget);
       - inprog = true.
   Several matches (the same type at the head of several planets) apply these steps one after another in target-list order.
5. Rate and pool, **category fields +0x50 and +0x4C**:
   - category -1: rate = budget +0x68 (income per second), pool = player credits (+0x70);
   - else rate = category I (+0x50, income rate = applied weight × rate, 0x6134c0) and pool = category P (+0x4C, the pool, 0x6135e0). It is
     **not** A (+0x44) / R (+0x48). A category id with no budget entry gives 0 for both.
   - D = (double)(T × rate) (float product); if not inprog, D += pool (as double).
6. Result: R < 0.0001 -> 0.0; else D > 0 -> R / D (double); else `(double)1e18f` = 999999984306749440.0.

Consequences: an item of this type that is already building lowers the cost still to fund (cost not yet built, capped at the goal's
allocation, minus what the goal already holds) and the time (time left, capped by the goal's horizon), and drops the pool term. A goal
whose horizon is 0 makes D = 0, so the token returns 1e18 unless R is already below 0.0001.

**Probe caveat (a2f t07 / a2g t02).** The kit decodes "available" and "rate" from this token (`A2F_T07_BUDGET_PROBE.md`; D = b × rate +
pool). So the retail "A" / "R" columns in `A2G_T02_T04_PORT_VS_RETAIL.md` §1 are **P (+0x4C) and I (+0x50)**, not A / R. The port trace
columns there are A / R. At Init P = A and I = R = 0, so the Init rows stand. Later rows compare different fields (e.g. retail "MajorItem A
accrues 1.66/s" is P growing by I = 0.1118 × 14.83; R would be lower whenever goals hold a required rate).

**Port change** (`BUDGETFRACTIONTOBUILD` and the `budget` hook):
1. Hook: return `budget.category(c).I` and `budget.pool(c)` (P) instead of `rate(c)` (R) and `available(c)` (A). Keep the all-categories
   branch (`income_per_second()`, player credits). Unknown category name: rate = pool = 0 (the token then gives 1e18), not failure [M:
   the name -> id step is the DLL's; an id outside the budget map gives 0, H].
2. Add the scan of step 4 over all planets, using the head `BuildEntry` of the queue index from step 3 (`build_time`, `remaining`,
   `start` + `duration`, `player`). It needs the planet queue's current AI build task, the task's ended / reclaimed flags, and its goal's
   budget entry (`alloc`, `a`, `c`, `h`): early 0.0 returns as in step 4.
3. Do the arithmetic in float as above (R, T, T × rate). D and the division in double. Use `(double)1e18f` for D ≤ 0.

## 3. t03: the Empire's Init Infrastructure weight (HotM, `Sandbox_Heart_Of_The_Maw_Rebel`)

**Equation** (`BasicEmpireInfrastructureBudgetAllocation`, BUDGETINGEQUATIONS.XML) [H]:
1.1 × (2 + 0.5·TechLevel − MaxedStarbases − MaxedGroundbases) × (1 + Maintenance) × (1 + 2·(0.5 > Mines / PlanetsControlledUnn))
+ 6·(1 − Can_Build_Offensive_Space) + 10·Need_Tech_Upgrade_Unblocked + 10·(no LVF + no barracks + no mining facility).

At Init (Empire: 4 planets Kashyyyk / Kuat / Corellia / Tatooine, tech 3, one mining facility at Corellia, LVF at Kuat / Tatooine, barracks
everywhere, a level-3 starbase at Kuat): the last three terms are 0 (all three structures exist; Can_Build_Offensive_Space = 1; MajorItem
weight 2.4 = 0.8 × (7 − 3 − 0 − 1) needs Need_Tech = 0). The mine factor is 3 (1/4 < 0.5). So
**w = 3.3 × (3.5 − MS − MG) × (1 + M)**. The port's 10.7250 = 3.3 × 3.25 with M = 0 (the port returns 0 for `MAINTENANCE`), so
MS + MG = 0.25 [H arithmetic]. This corrects `A2G_T02_T04_PORT_VS_RETAIL.md` §1 cause 2, which split it as 10 + 0.725. The later port
values confirm it: 3.575 = 1.1 × 3.25 × 1 after a second mine, and 10.45 = 3.3 × 3.1667 with 3 planets. MaxedStarbases / Groundbases are
fractions k / 4 and TechLevel, the structure counts and planet count are discrete, so none of them can move w by 0.0101.
**Maintenance is the only continuous input**, and retail's M = 10.7351 / 10.725 − 1 = 0.000942 (band 0.000937 .. 0.000946 from the
4-decimal value) [H].

**Maintenance (player root, 0x669110)** [H]: g = `<root>.GrossIncomeUnnormalized` (failure -> failure); g = 0 -> 0; else
`<root>.MaintenanceUnnormalized` / g. GrossIncomeUnnormalized 0x667fb0 = Σ over target-list planets whose `TargetPlayerID` is the root
player of `Target.IncomeUnnormalized` (= Planet_Income(P, gross = false), 0x65d6e0 -> 0x3f68b0), i.e. **credits per income cycle** (667.5
for this Empire). NetIncomeUnnormalized 0x66a420 = Gross − MaintenanceUnnormalized [H].

**MaintenanceUnnormalized 0x669420** [H unless tagged]:
- Root player's faction `Is_Neutral` (+0x108): 0.0.
- Root = Self: walk Self's objects with the FLEET behaviour (0x2a9ff0(current-mode GOM, behaviour 4, player id, ...), per-player list order).
  Skip a fleet whose container (+0x2B0) is itself a fleet [M meaning of +0x2B0]. For each member returned by 0x3965f0(fleet, list, 1)
  [L whether flag 1 also returns nested contents], with T = 0x3973b0(member) (a transported company counts as its company type):
  k = T.`Maintenance_Cost` (type +0x798) × owner faction `Maintenance_Cost` (faction +0x3AC) (float). If k ≠ 0 and btime(T, no planet) ≠ 0:
  add ((float)cost(T, no planet) / btime / 30) × k (float division, double sum). The unit is credits **per frame** of build-rate, scaled
  by the upkeep factors. It is not per second and not per cycle, and the original divides it by a per-cycle gross anyway. Only fleets
  count: ground units on a surface, starbases and structures add nothing.
- Root ≠ Self: Σ over the target list of `Target.EnemyForce.MaintenanceUnnormalized` (planet-force leaf 0x6daf20, body not read). Any
  failure fails. Both leaves have lifetime 7 s. Shipped equations use only `Variable_Self.Maintenance` (3×) and
  `Variable_Self.NetIncomeUnnormalized` (1×).

**Arithmetic for the gap** (Empire faction `Maintenance_Cost` 0.25; per unit cost / btime × unit factor):
| fleet member at Init | cost / btime | unit factor | term |
|---|---|---|---|
| 2 × Acclamator_Assault_Ship | 2500 / 30 | 0.1 | 16.667 |
| 4 × Tartan_Patrol_Cruiser | 1250 / 15 | 0.05 | 16.667 |
| 2 × Victory_Destroyer (variant of _No_Fighters) | 3800 / 30 | 0.1 | 25.333 |
| Accuser_Star_Destroyer (Piett, story `Spawn_Piett` at tech 3; variant of Generic_Star_Destroyer) | 5000 / 90 | 0.3 | 16.667 |

Σ = 75.333; × 0.25 / 30 = 0.62778; M = 0.62778 / 667.5 = 0.00094049; w = 10.725 × 1.00094049 = **10.73509 -> 10.7351**, retail's value
to the last digit [H arithmetic, M that the Accuser is in an Empire fleet at Init: without it M = 0.000732 and w = 10.7329]. The ground
companies (0.0157/s if they counted) are not needed and would overshoot (10.7353). Retail's drift to 10.7364 is M = 0.001063, i.e.
MaintenanceUnnormalized +0.082/s (+9.8 ± 0.4 in the table's cost/btime × factor units) at an unchanged 667.5 income. That fits units
produced in the first 5.5 minutes (an Acclamator is 8.33) [L: which units].

**Port change** (`MAINTENANCE`, `MAINTENANCEUNNORMALIZED`, `NETINCOMEUNNORMALIZED`, data):
1. Parse `Maintenance_Cost` for object types (+0x798, default 0, inherited by `Variant_Of_Existing_Type`) and for factions (+0x3AC).
2. MaintenanceUnnormalized: 0 for a neutral faction. For Self, the fleet sum above, over the player's fleets in the current mode (orbiting
   and in transit, every member, a company by its company type). For another root, the planet-force sum. Leaf lifetime 7.
3. Maintenance = Unn / GrossIncomeUnnormalized (0 when the gross is 0). NetIncomeUnnormalized = Gross − Unn.
4. Check that the port's Rebel-entry Empire has the Accuser in a fleet at the first AI service (the port's story spawns it for the human
   Empire in `test_game_session.cpp`). The expected Init weight is then 10.73509.

## 4. Not resolved

- PerceptionFunctionG.dll: the unknown-enum-name behaviour, and the `[A|B]` OR for a non-bitfield enum (§1) [L].
- 0x3965f0 flag 1 (nested contents) and fleet +0x2B0 (§3) [L/M]. Neither matters for the HotM Init value.
- Planet-force `EnemyForce.MaintenanceUnnormalized` 0x6dabc0 / 0x6daf20 bodies (non-Self roots; unused by shipped equations).
- Which retail units make the 10.7351 -> 10.7364 drift.
