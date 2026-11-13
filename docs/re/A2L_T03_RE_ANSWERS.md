# A2l t03: RE answers for lane H, galactic reachability, free-store stage and contrast (2026-11-08)

Roadmap a2l t03 (lane H) asked six questions. They come from the open ends of `A2K_T02_RE_ANSWERS.md` (Q1, open item 7),
`A1_READINGS.md` §5 / §25.3 and `A2K_T08_RE_ANSWERS.md`:
1. per-target reachability (slot 7 vs slot 5);
2. galactic `blocked()`;
3. the level-4 "keep-all" mode;
4. the Setup A health constant and engaged reads;
5. the null no-target stage and the gather's no-destination path;
6. the contrast lookup.

Source: the Ghidra decompilation of the FoC x64 `StarWarsG.exe` (RVAs relative to 0x140000000), vtables and constants
read from the same exe, and a capstone disassembly where the decompiler dropped call arguments. The shipped scripts
were also read (`PGTASKFORCE.LUA`, `PGAICOMMANDS.LUA`). Behaviour is described in prose; no decompiled code is
reproduced. Tags: [H] read directly; [M] one inferred link; [L] hypothesis.

Reachability levels (AIGoalReachabilityType): 0 Friendly_only, 1 Friendly_Ignore_Threat, 2 Enemy_destination,
3 Enemy_undefended, 4 Single_Hop_Disconnected, 5 Any, 6..10 the tactical threat levels.
PlanetReachabilityClass vtable slots: 5 = 0x664560 (bitset test), 6 = 0x664450 (stage pick), 7 = 0x663810 (path
cost from a source).

Short answers:
- **Q1.** `FindTarget.Reachable_Target` uses **slot 7** (path cost from a source), and **only when argument 6 gives a
  source**. Slot 5 is never called by it. Without argument 6 there is no reachability test at all. The port's
  player-level flood filter in `find_target` is not retail.
- **Q2.** Galactic `blocked()` is slot 9 = **0x6b84f0**. It asks the goal target's slot 6 at the goal's level and
  skips the goal when the answer is null. This is the per-target, 10 s cached flood, not the port's per-frame flood
  from the player's planets.
- **Q3.** Level 4 makes the per-target BFS **append** instead of clear. The result is the union of the floods from
  every passable neighbour of an unheld target. Only slot 5 and the ETA read that list; slot 6 returns the target
  itself at level 4.
- **Q4.** 0x140b27338 = GameConstants +0x11b38 `Health_Low_Percent_Threshold` (0.33 in FoC). Setup A drops a
  candidate below it. The "engaged" reads are tactical-only state (dogfight cell, special-weapon recharge) and always
  pass in galactic.
- **Q5.** With no goal target the stage is **null**:
  - `Get_Stage()` returns nil, so `AssembleForce` / `SynchronizedAssemble` call `ScriptExit()` and the plan is
    abandoned.
  - `Produce_Force()` with no argument has a **null destination even when the task force has a stage**, so nothing
    gathers.
  - The port gathers at `t.stage` in both cases.
- **Q6.** One **function**, 0x5f1f90(own type, table, key), over one global table loaded from `PGAICommands`. Its
  result: the exact-type weight; else the mean of the matching category weights that are not 1.0; else 1.0 if any
  category matched; else 0.
  - Auto-resolve keys it by a row's enemy category mask.
  - `Is_Good_Against` keys it by each category bit of the target, takes the maximum, and compares it with > 1.0.
  - Both port callers use the first-match `ContrastTable::contrast`, which is not retail.

---

## Q1. Per-target reachability in `Reachable_Target`: slot 7 with a source, or slot 5?

### Q1.1 What `Reachable_Target` does per candidate (0x698360) [H]

`FindTarget.Reachable_Target(player, evaluator, mask, level, scale[, source[, range]])` walks the candidate list in
target-list order. For each candidate:
1. Applies test (candidate vf+0x20). A candidate that fails is skipped.
2. **Reachability, only with a source.** The test runs only when argument 6 resolved to a source object and the
   candidate has a reachability object (vf+0x38). It calls that object's **slot 7(source, level = argument 4, 0)**.
   A result of 1e18 skips the candidate.
3. Bindings and Evaluate, as A2K_T02 Q1(b).
4. Range test, only with argument 7: 3-D distance² from the candidate to the source position (source vf+0x68).
5. Selection, as A2K_T02 Q1(c).

Argument 6 is resolved like this:
- an `AITargetLocationWrapper` gives wrapper +0x38;
- a `GameObject` is looked up through the context (+0x40 vf+0x40) [M];
- anything else raises the script error "Parameter 6 is not a valid AITargetLocationWrapper";
- with fewer than six arguments there is no source and **no reachability test**.

### Q1.2 Galactic slot 7 = 0x663810(reach, source, level) [H]

1. The result is 1e18 when any of these holds:
   - the source is null;
   - the player's AI path finder is missing (reach +0x10 → +0x10 → +0x28 → +0x50 → +0x48);
   - the source planet (source vf+0x30) is null.
2. The result is 0 when the source planet is this target planet.
3. Otherwise the path is 0x690f10(finder, source planet, target planet). It is accepted by 0x6913a0 when
   (summed packed cost >> 16) × 100 ≤ the level's threshold:

| Level | Threshold | Hops summed |
|---|---|---|
| 0, 2, 3, 6 | 1e-4 | [0, 1000) |
| 4 | 1e-4 | [0, nPlanets − 2): the final hop is not counted |
| 1, 5, 10 | 1e18 (always pass) | |
| 7, 8, 9 | GC 0x140b16d7c / 0x140b16d80 / 0x140b16d84 | |

4. Pass: cost = (summed low 16 bits) / 10. Fail: 1e18.

### Q1.3 Who passes a source (shipped scripts) [H]

| Caller | Level | Argument 6 |
|---|---|---|
| GalacticFreeStore | Friendly_Only | the unit object |
| GalacticHeroFreeStore | No_Threat | the unit object |
| `PGTaskForce` `_FindStageArea` path | Enemy_undefended | `AITarget` |
| Story Underworld scripts | Any / Enemy_Undefended | an object |

All goal evaluators and the other story calls pass no source and get no reachability test.

### Q1.4 Slot 5 [M]

Slot 5 (0x664560, the per-target flood bitset) is **not** called by `Reachable_Target`, which settles A2K_T02 open
item 7. A decompiler scan found one four-argument vf+0x28 call: the land `TaskForce.Reinforce` at 0x6eaf60. It found
no galactic slot-5 caller outside the level-4 ETA (Q3). The [M] covers calls the decompiler may have lost.

### Q1 port change

`GalacticAiWorld::find_target` (engine/ai/src/galactic_ai.cpp ~l.1787):
- Remove the unconditional `source_planets(m)` flood filter.
- When argument 6 is present: resolve it to a source object (or its planet). For each candidate planet run the
  slot-7 rule of Q1.2 from the source planet to the candidate, using the same path finder and cost as the free-store
  path (`ai_free_store_path`). Skip the candidate when the path is rejected or the source planet is missing.
- When argument 6 is absent: apply no reachability test.

---

## Q2. Galactic `blocked()` (goal-system slot 9)

### Q2.1 0x6b84f0 [H]

The GalacticGoalSystem vtable is 0x8be298. Its slot 9 is 0x6b84f0. The space goal system's slot 9 (0x6b8900) only
does step 1 below. The land version is 0x6b8710.

1. If the goal is in the active set (+0x38, lookup 0x721a50): **skipped** (slot 9 returns 1). (Corrected 2026-11-12, A2M_R2_RE_A §4: this line said "not blocked"; AI_GALACTIC §1.5 and the port have it right.)
2. If the goal target (+0x58) is null: not blocked.
3. Otherwise: reach = target vf+0x38. Unlike land, there is no null check. Then call
   **slot 6(level = goal type +0x2c, link mask −1, no exclude list, no include list)**.
   A null result means blocked (the goal is skipped).

Effect of step 3, by level:
- **Level 4**: never blocked, because slot 6 returns the target itself (Q3).
- **Levels ≤ 5 (except 4)**: blocked when the per-target flood list from 0x665160 is empty. That happens when:
  - f4 is clear (levels 0 and 1) and the target is neither the player's nor an ally's; or
  - no planet owned by the player is reached by the BFS through passable planets, starting from the target (when
    held) or from its first passable neighbours.
- **Levels > 5**: always blocked in galactic. Slot 6 has no galactic answer for the tactical levels.
- The list is cached for **10 s per (target, level)**. A goal can stay blocked or unblocked for up to 10 s after the
  map changes.

### Q2 port change

`PlayerAi::blocked` (galactic_ai.cpp l.572) floods once per frame from the player's planets and tests whether the
target is reached. Replace it with the per-target slot-6 answer. It differs in three ways:
- the BFS starts at the target or its passable neighbours;
- owned planets are passable;
- the answer is cached 10 s per (target, level).

`ai_free_store_path` already has the per-target list for Setup A. Use that list and its cache, and add the
"level > 5 → blocked" and "level 4 → never blocked" ends.

---

## Q3. The level-4 "keep-all" mode

### Q3.1 Where it lives [H]

keepAll = (level == 4) is computed in 0x665160 and passed down: 0x3f12e0 parameter 6, then BFS 0x3f0ce0 parameter 7.
- In the BFS, keepAll **skips clearing** the output list and the visited bitset at entry.
- 0x3f12e0 handles an unheld target by running one BFS per passable neighbour and normally keeps the largest result.
  With keepAll, every neighbour's BFS appends into buffer 0. The list is then the **union** of all neighbour floods,
  in neighbour order, each planet once (the bitset is shared). The function's returned count is 0.
- For a held target, keepAll has no effect, because the buffers are cleared at the function's start.

### Q3.2 Who reads the level-4 list [H]

| Reader | Reads the list? |
|---|---|
| slot 5 at level 4 | yes, as a bitset |
| ETA 0x6639f0 for level-4 goals (`Conquer_To_Reconnect`, `Lift_Blockade`) | yes: their production source planets are the keep-all list |
| slot 6 at level 4 | no: returns the target |
| `_FindStageArea` and the Setup A stage at level 4 | no: return the target |

### Q3 port change

`ai_free_store_path.hpp` does not model keep-all. Only the ETA source set of level-4 goals needs it. For level 4 and
an unheld target, return the union of the neighbour floods in neighbour order; do not pick the largest.

---

## Q4. Setup A: the health constant and the "engaged" reads (0x7287a0)

### Q4.1 Health [H]

- 0x140b27338 = GameConstants (base 0x140b15800) **+0x11b38 `Health_Low_Percent_Threshold`**. FoC ships **0.33**
  (TACTICAL_GAPS §3.2); Thrawn's Revenge sets 0.50.
- A candidate is skipped when 0x396470(unit) < this value. 0x396470 is the hull fraction (0x396df0). When the object
  has hardpoints in its +0x2d0 list with flag (+0x20 → +0x4d == 1) and type +0xa8 set, the value is the min of the
  hull fraction and 0x396070 (the hardpoint health).
- The health test runs only for candidates with PP +0x104 == 0 or entry +0x30 == 0. A candidate already reserved
  (entry +0x30 set) skips the health test when PP +0x104 is set. What PP +0x104 means is not settled here.

### Q4.2 The "engaged" tests, in order [H]

**Clause A** passes if any of:
- entry +0x30 == this PP;
- PlanDef +0xd1 `AllowEngagedUnits`;
- the object has no TEAM behaviour (+0x348 == −1);
- the locomotor pack (obj +0xa8) is null;
- pack +0x1c4 == −1 (no dogfight cell, i.e. not FIGHTER_FLYING).

**Clause B** passes if either:
- the object has neither SPECIAL_WEAPON (+0x361) nor LOBBING_SUPERWEAPON (+0x369); or
- obj +0x158 is set, the 0x4914e0 recharge progress (now − start) / (end − start) is ≥ 1.0, and at least one of:
  - there is no mode object for PP +0x198;
  - the mode's vf+0x220(player id) is null;
  - 0x527620 is true.

**Then**, the unit must be neither moving nor building nor locked:
- obj +0x210 is null, or both +8 == 0 and +0x34 == 0;
- +0x3a7 == 0;
- +0x3b4 == 0;
- 0x39ad40 (lock) is false.

**Then** the INDIGENOUS (behaviour 0x20) type test for the mode, and the attach test (+0x35b, behaviour 0x29,
0x42e3b0 / 0x42e3d0), as A2K_T02 Q2(c) item 7.

In galactic, dogfight cells and special-weapon recharge are tactical state. Clauses A and B therefore pass for every
galactic unit [M]. The tests that matter in galactic are health, moving / building / lock, and attach.

### Q4.3 The galactic key and in-transit units [H; M on one field]

- The unit's planet is found by walking parents up to the planet slot (+0x335).
- If the parent is a fleet, the planet is 0x432c30 = FleetDataPack (fleet +0xc0) +0x50. This is the fleet's planet,
  [M] the in-transit source or destination.
- **Retail keeps in-transit units** (the port drops them). The planet must be revealed to the player (0x3fa420).
- The key uses path level 3, or 4 when the goal's Reachability is 4.
- Hidden units (0x3a9760) skip the key and keep key 0.

### Q4 port change

`FreeStoreM::setup_a` (engine/ai/src/galactic_pp.cpp):
- add the health filter (0.33, GameConstants), with the PP +0x104 / entry +0x30 condition;
- add moving / building / lock and attach;
- keep in-transit units, keyed by the fleet's planet;
- leave the engaged clauses out for galactic, because they always pass. This is the part already marked
  "not modelled" there.

---

## Q5. The null no-target stage and the no-destination gather

### Q5.1 Stage of a task force without a goal target [H]

- **Setup A stage loop** (0x7287a0): when the goal target (+0x58) is null, stage[i] = **null**. Otherwise the stage is
  the target's slot 6, or the goal target when the result is null.
- With no target, the distance key is skipped (key 0), and the stage is never dereferenced.
- **Copy to the task force** (plan instantiation 0x5f7090): for TF i,
  TF +0x100 = 0x6c09b0(plan +0x88 → +0x68, i). That returns stage entry i (stride 0x50, field +0x40), or null past the
  end. **So TF +0x100 is null for a goal without a target.**
- **`Get_Stage`** (0x5fda50): returns nil when TF +0x100 is null; otherwise a wrapper for it.

### Q5.2 What the shipped scripts do with a nil stage [H]

`PGTASKFORCE.LUA` `AssembleForce(tf, false)` and `SynchronizedAssemble(tf, false)` call `tf.Get_Stage()`.
- If it returns nil, they log "Unable to find a staging area" and call **`ScriptExit()`**: the plan is abandoned
  before `Produce_Force` runs.
- Otherwise they call `Produce_Force(stage)` or `Produce_Force(stage, true)`.

The `stage_at_strongest_space` variant takes a `Reachable_Target` / `FindTarget` result instead. A nil result also
exits there.

Shipped use: `AssembleForce(X)` about 40 times across the plan scripts, `SynchronizedAssemble` 3 times, plain
`Produce_Force()` 82 times.

### Q5.3 `Produce_Force` destination (0x5fe240) [H]

| Arguments | Destination |
|---|---|
| none | **null**. TF +0x100 is **not** read. |
| a planet object, or an `AITargetLocationWrapper` whose object is a planet | that planet |
| anything else, including an explicit nil | script error "Expected a planet object for parameter 1" |

- With a null destination in galactic mode it also clears byte +0x4d of the plan's stage object (+0xf0 → +0x88 →
  +0x68).
- It creates one AIPlanetBuildTask per wanted type (0x524ee0 → 0x649200), with:
  - BT +0x98 = destination;
  - +0x88 type;
  - +0xa0 the TF;
  - +0x90 the TF's per-entry int (TF +0x90[i]);
  - +0xda the synchronize flag;
  - state 0x11.
- A2K_T08 Q2.3: in WAIT_FOR_FREESTORE_ALLOC with BT +0x98 == 0, the task goes straight to PRODUCTION_FINISHED. There
  is no split, no path and no gather.

### Q5.4 What the port does, and why the a2k attempt moved every golden [H for retail; M for the attribution]

Port, engine/ai/src/galactic_ai.cpp:
- **Stage at instantiation** (l.938-948): a goal without a target gets `nearest_owned(player)`. Retail gives null.
- **`tf_produce_force`** (l.437):
  - `Produce_Force()` with no argument keeps `t.stage`;
  - `t.stage` is the Setup A stage, or `nearest_owned` for a goal without a target;
  - the ProduceForce status (l.1266-1277) then gathers the units at `t.stage`, with pickups, splits and moves.
- **In retail, plain `Produce_Force()` never gathers, even when the goal has a target and the TF has a stage.** The
  82 plain calls leave the units where they were produced or found. The plan then moves them itself (`Move_To`,
  `LaunchUnits`, …).
- **Build destination** (`create_build_tasks`, galactic_build.cpp l.69-70): `b.dest = stage` when valid, else the
  selection's source. With a null `Produce_Force` destination, retail's BT +0x98 is null. Which planet builds the
  unit is chosen without +0x98 [M]; the port's `source` fallback is the closest match.
  **Settled 2026-11-13 (`BKG3_RE_ANSWERS.md` Q7.1, retail run):** such a task is offered to every planet with no
  path test and no travel term; the port no longer uses the source planet (magic plans excepted).

So two changes are needed together. Replacing only the `nearest_owned` fallback changes which plans exit in
`AssembleForce`, but plain `Produce_Force()` still gathers everywhere. Any retail-faithful version of either change
moves the galactic goldens; the check is whether it moves them toward the retail probes.

### Q5 port change

1. A goal without a target: `tf.stage = -1` (null). Make `Get_Stage` return nil for it, so the shipped
   `AssembleForce` exits the plan.
2. `tf_produce_force`: the destination is **the argument only**. With no argument (or a null destination), do not
   gather: the ProduceForce status finishes when the builds are done and the TF has units (the existing `t.stage < 0`
   path). Keep `t.stage` for `Get_Stage` only.
3. `Produce_Force(nil)` (explicit nil) is a script error in retail. A plan that does this stops.
4. `create_build_tasks`: with a null destination, build at the selection's source. This is already the fallback.

---

## Q6. The contrast lookup (auto-resolve and Lua `Is_Good_Against`)

### Q6.1 Table: one global map, loaded once (0x5f2b80) [H]

On first use, 0x5f2b80 loads the Lua script `PGAICommands` and runs `Base_Definitions` with `PlanDefinitionLoad =
true`. It then reads these globals:
- `MinContrastScale` / `MaxContrastScale` (1.1 / 1.5 in FoC);
- `EnemyContrastTypes[i]` (a category name);
- `FriendlyContrastTypes[i]` (a `WeightedTypeList`).

0x5f1610 builds the global hash map at 0x140b396b8 / c8 / e0:
- key = the category mask of `EnemyContrastTypes[i]`;
- value = the weighted list of row i.

Each list entry (stride 0x28) holds:
- +0x00: weight (double);
- +0x08: category mask (0 for a type entry);
- +0x18: the game-object type (null for a category entry).

A plan's own map (owner +0x68 / +0x78 / +0x90) replaces the global one when a non-null owner is passed (galactic
PP commit 0x6bf840, tactical 0x72b240 / 0x72b3e0). Auto-resolve and `Is_Good_Against` pass null, so they use the
global map. In FoC the eleven shipped enemy categories are distinct single categories: Fighter, Bomber, Transport,
Corvette, Frigate, Capital, Infantry, Vehicle, Air, Structure, LandHero.

### Q6.2 Function: 0x5f1f90(T = own type, owner, K = key) → double [H, from the disassembly]

1. Look up K in the map: the owner's map, or the global one when owner is null. The lookup is an exact 64-bit key
   match: hash (K ^ 0xdeadbeef) & 0x7fffffff, Park-Miller ×16807, ordered bucket chain. **No entry: 0.0.**
2. Walk the list in order:
   - entry type == T: **return that weight at once**;
   - otherwise, if T's category mask (+0x1648) intersects the entry's mask: matched = true; if the weight is not
     1.0, add it to the sum and count it.
3. count > 0: return sum / count (double). Else return 1.0 if matched, else 0.0.

This matches A2K_T02 Q2(b), which gave the same rule for the PP commit, and the port's `ContrastTable::ai_weight`.

### Q6.3 Auto-resolve callers [H]

- Target choice 0x60c200: for each category entry j of the defender's vector with value > 0,
  c_j = 0x5f1f90(attacker piece type, null, **K = entry j's category mask**). Then the score of BATTLE_TRANSITIONS
  §4.4 step 1, with (1 − x²) clamped at ≥ 0.
- Apply 0x60ad90 calls the same function with the same key for the chosen entry.
- The second factor (0x5f22c0) is never applied, because +0x50 is always −1 (BATTLE_TRANSITIONS open item 7,
  resolved).

### Q6.4 `Is_Good_Against` (0x57bb60) [H; the key was M, now H from the disassembly]

`unit.Is_Good_Against(x)`, with x a GameObject or an `AITargetLocationWrapper` that represents an object:
1. If x is a wrapper that does not represent an object: script error "Is_Good_Against -- ai target %s does not
   represent a game object". If the caller's object (+0x60) is null: return nothing.
2. Find both types: 0x396380(obj), or the cached obj +0x298 when the object has behaviour slot +0x337.
3. For bit = 0..63 with bit set in **x's** category mask (+0x1648): w = 0x5f1f90(**caller's type**, null,
   **K = 1 << bit**). The disassembly loads 1 into r8 and shifts it by the bit index before the call. Keep the
   maximum as a float32, starting at 0.
4. Return the boolean **max > 1.0** (strict).

A category bit that is not one of the shipped enemy categories gives 0.

### Q6 port change

- **`ContrastTable::contrast`** (engine/galactic/src/auto_resolve.cpp l.47) returns the **first** entry that matches
  by mask or name. Retail is 0x5f1f90:
  - an exact type match returns at once;
  - otherwise the mean of the non-1.0 category weights;
  - otherwise 1.0 if any category matched;
  - otherwise 0.

  `ContrastTable::ai_weight` (l.56) already implements this. Use it in `auto_resolve` (l.262), or make `contrast` call
  it. This changes auto-resolve results for a type that matches a 1.0 entry before a non-1.0 one, or that matches
  two different non-1.0 entries. A2K_T02 Q2(b) shows one: Kedalbe against the Frigate row gives 3.0, and the first
  match also gives 3.0 there, but the rule differs elsewhere.
- **`tactical::is_good_against`** (engine/ai/tactical/src/perception_lua.cpp l.99):
  - use `ai_weight`, not `contrast`;
  - select the row by exact key: enemy mask == bit, not enemy mask & bit. The two agree for the shipped single-bit
    categories;
  - a bit with no row contributes 0, so the maximum starts at 0, not at "no value". The port's `any &&` gives the
    same boolean.
- Change the tag in `perception_lua.hpp` from "[M: per-bit key]" to [H].

---

## Open items

| # | Item | What settles it |
|---|---|---|
| 1 | Q1.4: no galactic slot-5 caller besides the level-4 ETA [M] | a breakpoint on 0x664560 during a galactic game, logging the caller |
| 2 | Q4.3: fleet +0xc0 +0x50 is the in-transit fleet's planet: source or destination [M] | probe: a free-store unit in a moving fleet when a plan's Setup A runs; log the chosen stage key and that field |
| 3 | Q5.3: which planet builds a BT with a null +0x98 [M] | read the AIPlanetBuildTask production state (0x648980 family) for the build planet; or probe a plain `Produce_Force()` build and log the queue planet |
| 4 | Q5.4: which goals in the shipped goal sets have no target and reach `AssembleForce` | a port-side list from the goal definitions; a retail probe logging "Unable to find a staging area" lines, already in the plan scripts' `DebugMessage` |
| 5 | Q4.2: clauses A and B always pass in galactic [M] | probe: a galactic unit with a special weapon (none shipped in FoC galactic) |

## Confidence summary

| Point | Tag |
|---|---|
| `Reachable_Target` uses slot 7 with argument 6 only; no test without it | H |
| slot-7 thresholds and sum ranges per level | H |
| slot 5 has no galactic caller besides the ETA | M |
| galactic `blocked` = slot 9 0x6b84f0 = target slot 6 null test, 10 s cache | H |
| keep-all = append and union in the unheld-target branch; readers are slot 5 and the ETA | H |
| Health_Low_Percent_Threshold 0.33; health test conditions | H |
| engaged clauses pass in galactic | M |
| in-transit units kept, keyed by the fleet's planet | H rule, M field meaning |
| no target → null stage → `Get_Stage` nil → `AssembleForce` exits | H |
| `Produce_Force()` has a null destination regardless of TF +0x100; no gather | H |
| contrast = function 0x5f1f90 over the global `PGAICommands` map; rule as Q6.2 | H |
| `Is_Good_Against` key = 1 << bit of the target's mask, caller's type, max > 1.0 | H |
