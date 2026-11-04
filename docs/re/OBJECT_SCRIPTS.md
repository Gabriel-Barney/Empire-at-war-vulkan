# Game-object Lua scripts: attach, pool, service, events, release (q6)

Question: how does retail FoC host the per-type `<Lua_Script>` game-object scripts (`Data\Scripts\GameObject\*.lua`),
`Execute_Script_On_Type`, and the per-object script slot (object +0x2D8, AI_GALACTIC_3 §2.1 item 4)? The port runs
none of them. The immediate need is Thrawn's Revenge (TR): its ground companies are a `*_Company_Dummy` object whose
`GenericCompanySpawner` script spawns the real units.

Answer in one paragraph:
- Every object whose type has a non-empty `Lua_Script` gets **its own script instance** when the object is constructed,
  in every mode (galactic, land, space) and in both stores.
- The script is a `LuaScriptClass` with its own `lua_State`, taken from a **pool keyed by the script name**.
- The engine runs the file's top level, then `Base_Definitions()`, creates the `main` coroutine, and only then sets the
  global `Object`.
- The object's own service (inside the GOM service of its mode) resumes the script's threads when the
  `ServiceRate` / `LastService` gate opens. On the first pump it draws one value from the **synced RNG**.
- The engine queues **no events** to object scripts. The only engine-driven callbacks are:
  - the proximity callbacks of `Event_Object_In_Range`, fired synchronously by `Service_Wrapper()` from
    `PumpEvents`;
  - the AI's hero-setup calls (`Evaluate_Attack_Ability`, `Get_Escort_Ability_Weights`).
- `ScriptExit()`, a binding error, or the end of the object frees the script. A freed pooled instance runs
  `Flush_G()` and `Base_Definitions()` and goes back to the pool.
- `Execute_Script_On_Type` adds one more instance per **type**. It is never serviced and has no `Object`; only the
  galactic AI reads it.
- TR's company spawner needs nothing company-specific from the engine: just this hosting plus about 25 ordinary
  bindings (§6).

Source:
- Ghidra decompile of Steam FoC x64 1.121 (`re_reference/ghidra/StarWarsG_decompiled.zip`, `Global.c`). RVAs are
  relative to the image base. Function starts are as in `ghidra/renames.csv`.
- Scripts: vanilla FoC `Data\Scripts` (`extracted/foc_data_text.zip`) and TR (workshop 1125571106).

Conventions:
- **[H]** read in the code; **[M]** read, intent inferred; **[L]** inferred or partly traced.
- `LuaScriptClass` fields:
  - +0x40 thread vector (0x40-byte entries);
  - +0x58 `lua_State*`;
  - +0x60 current thread index;
  - +0x68 name;
  - +0xD8 the GetEvent store;
  - +0x121 "exited";
  - +0x122 "needs Base_Definitions";
  - +0x123 "in use";
  - +0x124 "poolable".

  The flags are as in GAME_SCORING §1.

Related docs:
- GAME_SCORING.md: the same `LuaScriptClass` machinery for the scoring script.
- OBJECT_RUNTIME.md §1, §2.2, §5.3: object construction, death, the store update.
- MAIN_LOOP.md §3.3–3.4.
- LUA_SCRIPT_OUTPUT.md §1: the error helper.
- AI_GALACTIC.md "Service": the shared thread pump.
- AI_GALACTIC_3.md §2.1: hero scripts in the AI.

---

## 0. Summary for the implementer

1. **One script per object.**
   - The type tag `Lua_Script` (type +0x1AD8, inherited through `Variant_Of_Existing_Type`) names the script.
   - Object construction attaches it (0x388b60 → 0x3989a0 → **Attach_Object_Script 0x38cf30**) after the
     behaviour lists and before the object enters the store lists.
   - The instance lives at object +0x2D8.
   - Same rule in galactic, land and space; there is no mode filter in the engine. Scripts filter themselves with
     `Get_Game_Mode()` + `ScriptExit()`.

   [H]
2. **Attach order in the fresh state:**
   1. the file's top level (at pool load);
   2. the registration of `GlobalValue` and the base globals (0x2567b0) and the 119 game commands (0x546c70);
   3. `Base_Definitions()` (fresh instance only);
   4. create the `main` coroutine (it does not run yet);
   5. set the global **`Object`** (the object's GameObjectWrapper).

   So `Object` is nil during the top level and `Base_Definitions` / `Definitions`. [H]
3. **Pool by name (0x244170).**
   - `ScriptPoolCount` is read once, when the name is first used. Absent means a pool of 1; N means N preloaded;
     0 means no pool (every attach loads a fresh state).
   - A pooled instance freed by its object runs `Flush_G()` then `Base_Definitions()` at once and waits for the next
     object. When it is reused, `Base_Definitions` is **not** run again.
   - Vanilla object scripts: only FieldCommander / FleetCommander set a count (4). Every other object script is
     pooled with no count set (pool of 1, grows on demand). [H]
4. **Service.**
   - It runs in the object's Service (0x3a6b80), step 7 of OBJECT_RUNTIME §5.3: after behaviours, model time and
     late update, before hard points.
   - Skipped when the object is dead (+0x3A0 & 2) or the store is paused (galactic battle pending / story block,
     A1_READINGS a1j t05).
   - Gate: `ServiceRate` must be a number. The pump runs when `LastService` is not a number, or when
     (now − LastService) > ServiceRate (strict). now = (float)mode frame × 0.033333335f; the subtraction is done in
     double.
   - Pump = resume every thread once, in creation order (0x247a90).
   - After the pump, `LastService` := now. **First pump only: `LastService` := −(synced RNG float)**, one draw on the
     game LCG 0x140a13e24 per script attach. [H draw and sign; L for the upper bound]
5. **Termination.**
   - `ScriptExit()` (+0x121) or any binding error (the error helper 0x248190 sets +0x121 in retail) frees the script
     at the end of that pump: threads are killed and the pool is returned or the state closed. The object drops
     +0x2D8 for good.
   - A Lua runtime error in a thread kills only that thread. [H / M]
6. **Death.**
   - Service stops at once (dead bit). The script is not told.
   - The instance is freed when the GameObjectClass is destroyed (0x3893e0), at the deferred delete or when the
     last reference goes.

   [H stop; M timing]
7. **Events.**
   - Object scripts get **no queued engine events**: the GetEvent enqueue 0x25ab50 has one caller, the AI task-force
     dispatch 0x6006b0. [H]
   - `Register_Timer` / `Register_Death_Event` / `Register_Attacked_Event` / `Register_Prox` are Lua tables polled by
     `Pump_Service()` inside `PumpEvents()`.
   - The engine side is `Event_Object_In_Range` (0x575c50), `Cancel_Event_Object_In_Range` (0x574a30) and
     **`Service_Wrapper` (0x57f7e0)**. Service_Wrapper calls each registered function synchronously with
     `(prox_wrapper, found_object_wrapper)` for every object in an axis-aligned box of half-size `range`.
   - `PumpEvents` calls `Object.Service_Wrapper()` before every yield. [H]
8. **Execute_Script_On_Type** (type +0x5D, 8 vanilla generic-hero types) loads one more instance into type +0x1AF8 at
   type finalisation.
   - It has `Base_Definitions` but no `main` and no `Object`, and is never serviced.
   - Its only reader is the AI hero setup C (0x726710), as the fallback when the unit has no object script. [H]
9. **TR `GenericCompanySpawner`** (§6) needs:
   - the attach / service / exit machinery;
   - Lua 5.0 `require` over nested `Library` folders;
   - `GlobalValue` "MOD_ID", already written by GameScoring;
   - `Get_Game_Mode`, `Find_Object_Type`, `Find_All_Objects_Of_Type`, `Spawn_Unit`, `Create_Position`,
     `GetCurrentTime`;
   - the object methods `Get_Type().Get_Name`, `Get_Position().Get_XYZ`, `Get_Owner().Get_Faction_Name`,
     `Prevent_AI_Usage`, `Set_Selectable`, `Make_Invulnerable`, `Hide`, `Set_Importance`, `Teleport`,
     `Take_Damage`, `Get_Parent_Mode_Object_ID`, `Service_Wrapper`.

   No engine event is involved.

---

## 1. Which objects get a script, and when

### 1.1 XML tags and fields

| tag | type field | meaning | conf |
|---|---|---|---|
| `Lua_Script` | +0x1AD8 (string) | script name, `.lua` optional; resolved by the normal script loader (LUA_API "Script search path"; `GameObject` is one of the roots). The type manager also records `.\Data\Scripts\GameObject\<name>` in its list +0x78 (OBJECT_TYPES §5.1, used for file watching [L]) | H field, M lookup root |
| `Execute_Script_On_Type` | +0x5D (bool) | also load a **type-level** instance into +0x1AF8 (§1.5) | H |
| (none) | object +0x2D8 | the object's own `LuaScriptClass*` (reference counted) | H |

- Both tags are inherited by variants (OBJECT_TYPES §3.3), so TR's `Infantry_Dummy_Template` → `Vehicle_Dummy_Template`
  → every `*_Company_Dummy` carries `GenericCompanySpawner`. [H]
- The story XML also has a `<Lua_Script>` tag (plot files, 0x329aa0). That is the plot script, not an object script
  (STORY.md). [H]
- No other XML tag selects or configures object scripts. `ScriptPoolCount`, `ServiceRate` and `LastService` are Lua
  globals. [H for the strings: "ScriptPoolCount" is referenced only by 0x244170; "ServiceRate" / "LastService" only
  by the five service owners in MAIN_LOOP §3.4]

### 1.2 The attach point: object init 0x3989a0(obj, from_save, attach_on_load)

0x3989a0 is the object's behaviour / data-pack initialiser. Its steps:
1. reset per-object fields;
2. attach the generic `Behavior` list (type +0x6D0) and the mode list (`GalacticBehavior` / `LandBehavior` /
   `SpaceBehavior`, chosen by the mode type of the object's store);
3. **if the type's `Lua_Script` is non-empty: Attach_Object_Script 0x38cf30(obj, name, 0)**;
4. later: hard points (+0x2D0), the combatant pack, and so on.

Steps 2 and 3 run only when `from_save` = 0 or `attach_on_load` ≠ 0. [H]

| caller | when | flags | script |
|---|---|---|---|
| constructor 0x388b60 | every `Create_Object` (OBJECT_RUNTIME §1.1): spawns, reinforcements, company / team members landed in tactical, galactic production, map objects at map load, story `Spawn_Unit`, ... | (0, 1) | attached |
| object load 0x39ce40 | save-game load of an object | (1, param) | not attached here; re-attached by the 0x4B5 chunk (§2.6) |
| type re-init 0x39fc90 (from 0x2b3960: every object of one type in a store) | type hot reload (debug file watcher) [L] | (0, 0) | re-attached; the old instance is only dereferenced, not explicitly released [L] |
| death clone 0x29f270 | `Death_Clone` creation | — | the clone is created with its type's script, which is **released at once** (0x2488e0, slot nulled): death clones never run a script [H] |

Timing consequences [H]:
- `Base_Definitions` runs **inside `Create_Object`**, before the object is in the store lists (OBJECT_RUNTIME §1.1
  registers them after construction). A `Definitions()` that searches the store does not see the object itself.
- The attach registers the object in the store's serviced list (0x29bfe0, sets +0x39E) if it is not there yet. So a
  script makes an otherwise unserviced object serviced (markers like `DEMO_CONTROLLER`).
- Hard points do not exist yet during `Base_Definitions`.

### 1.3 Attach_Object_Script 0x38cf30(obj, name, from_save)

Normal path (`from_save` = 0) [H]:
1. Get an instance by name from the pool (0x244170(name, flush = 0), §1.4). Failure (file missing or does not
   compile) → the object has no script; the function returns false.
2. Store it at +0x2D8, taking a reference. A previous instance is dereferenced.
3. Register the base globals again, including `GlobalValue` (0x2567b0), and the 119 game commands (0x546c70). Same
   sets as GameScoring §1.1 step 3. See §1.6.
4. If +0x122 (fresh instance): call the Lua function `Base_Definitions()` by name (missing → nothing).
5. Create the `main` thread (0x244740, string 0x140866674 = "main", the same call as GameScoring's Attach): a
   coroutine for the global `main` if it is a function. It does not run yet.
6. Create the object's wrapper (0x5751d0(obj, script, 1): the per-(object, script) cached GameObjectWrapper, null if
   the object is already dead) and register it as the global **`Object`**.
7. Register in the serviced list (above).

`from_save` ≠ 0 path: plain new instance by name (0x2440f0, not pooled), base globals + commands, then the serviced
list. There is **no** `Base_Definitions`, no `main` and no `Object`; the saved state supplies them. Ghidra drops the
third argument at the load call site, so which path the load uses is [L].

### 1.4 One per object; the pool

- **Per object.** Each object has its own instance and Lua state; two Stormtroopers with a script share no Lua
  globals. Engine-side state is shared: `GlobalValue`, the game objects. [H]
- **The pool (0x244170, global map 0x140b09d80, name → list of instances)** [H]:

  | situation | result |
  |---|---|
  | name never seen | load a new instance (0x245e30: new `lua_State`, file top level runs). Read the global `ScriptPoolCount`. **Absent → pool of 1**, this instance poolable. **N > 1** → also load N − 1 extra instances now (top level runs for each), marked fresh and poolable, held only by the pool. **0** → this instance is not poolable and no pool entry is made (every later attach loads a fresh state again) |
  | name known, a free instance exists (+0x123 = 0) | reuse it (the last free one in list order). +0x122 is left as is, so `Base_Definitions` is not re-run |
  | name known, none free | load a new one, append it to the pool, fresh, poolable. Pools grow to the peak number of live objects with that script |
  | flush flag (not used by object attach) | every pooled instance made non-poolable; free ones released; list cleared |

  Every handed-out instance gets +0x123 = 1 (in use), +0x121 = 0, +0x60 = −1.
- **Vanilla counts:** `FieldCommander.lua` and `FleetCommander.lua` have `ScriptPoolCount = 4` (also TR). Every
  other vanilla object script has no count. TR's `GenericCompanySpawner` has no count. `PGStoryMode.lua`'s 0 does
  not apply: object scripts that `require("PGStoryMode")` get the 0 only after the pool entry already exists, and the
  count is read on the first load only. [H data; the PGStoryMode remark is M]

### 1.5 `Execute_Script_On_Type`: the type instance

- **Load (0x375040):**
  - called from type derivation 0x3782a0, 0x376cc0 and 0x4093c0 (type load and hot reload);
  - frees any previous +0x1AF8 instance (0x2488e0);
  - then, if +0x5D and the name is non-empty: pool get by name → +0x1AF8, followed by base globals (0x2567b0) and
    commands (0x546c70).

  [H]
- **Base_Definitions:** called by the per-pass type fix-up 0x377500 while +0x122 is set. It is not cleared there, so
  it may run once per load pass (≤ 10) [M]. No `main` thread and no `Object`. It is **never serviced**.
- **Reader:** AI hero Setup C 0x726710 (AI_GALACTIC_3 §2.1). It uses the unit's object script if there is one, else
  the type instance. It writes `PlayerObject` into that script and calls `MinPlanAttachCost` / `MaxPlanAttachCost`
  (reads), `Evaluate_Attack_Ability(target, goal_type_name)` and `Get_Escort_Ability_Weights(goal)`. [H]
- **End of game** (End_All 0x28d940 → 0x4080d0): each type instance is only garbage-collected (0x244000:
  `lua_setgcthreshold(0)` on the state and its threads). It is not reset, so its globals survive into the next game.
  [H]
- The type instance comes from the same name pool as the object instances. With `ScriptPoolCount = 4` the type load
  creates the pool (1 in use by the type, 3 free for objects). [H]
- Vanilla users (`Execute_Script_On_Type` true):
  - `Generic_Field_Commander_Rebel` / `_Empire`, `Generic_Fleet_Commander_Rebel` / `_Empire`,
    `R_General_Rieekan_Commander` (GenericHeroUnits.xml);
  - `Hutt_Field_Commander` (Hutt_Faction_Units.xml);
  - `Underworld_Saboteur`, `Underworld_Saboteur_Exec_Demo` (Minor_Heroes_Expansion.xml).

  [H data]

### 1.6 Globals an object script sees

| global | set by | when | conf |
|---|---|---|---|
| standard Lua 5.0 libraries, `_ALERT`, `LUA_PATH`, **`Script`** (the LuaScriptClass userdata; `tostring(Script)` names it; `Script.Debug_Should_Issue_Event_Alert()`) | LuaScriptClass ctor 0x2457f0 | instance creation | H |
| `StringCompare`, `_ScriptExit`, `_ScriptMessage`, `_DebugBreak`, `_CustomScriptMessage`, `ThreadValue`, **`GlobalValue`**, `_MessagePopup`, `_OuputDebug`, `GetThreadID`, `DumpCallStack`, `Create_Thread`, `Thread`, `GetEvent` | 0x2567b0 | every attach | H |
| the 119 game commands (`Find_Object_Type`, `Spawn_Unit`, `Find_All_Objects_Of_Type`, `Get_Game_Mode`, `GetCurrentTime`, `GameRandom`, `FogOfWar`, `Create_Position`, `FindDeadlyEnemy`, `WeightedTypeList`, `EvaluateTypeList`, `Story_Event`, camera / cinematic / GUI commands, ...: the full LUA_API "Globals" list minus the AI-only ones) | 0x546c70 | every attach | H |
| **`Object`** | 0x38cf30 step 6 | after `Base_Definitions` and the `main` thread | H |
| `PlayerObject` | AI hero Setup C (0x726710) | whenever the AI evaluates the hero; not set for other object scripts | H |

`Target`, `FreeStore`, `Budget` and `AITarget` are **not** set for object scripts (`Common_Base_Definitions` sets
them nil). [H for the engine side: no other writes into +0x2D8 scripts]

### 1.7 What uses object scripts in vanilla FoC

About 60 `Lua_Script` names on unit, structure and marker types, in five groups:
- ability autofire helpers (`ObjectScript_MissileShield`, `_PointDefense`, `_PowerToShields`, `_SelfDestructLand`);
- hero scripts (`DarthVader`, `HanSolo`, `Palpatine`, `Tarkin`, `Veers`, `BobaFettPlan`, `*Plan`, `ObjectScript_Yoda`,
  `_IG88`, `_MaraJade`, `_Silri`, `_Urai`, ...);
- generic heroes (`FieldCommander`, `FleetCommander`, `GenericBountyHunter`);
- pickups and triggers (`TechPowerUp`, `CreditPowerUp`, `ProximitySpawnTrigger[_R]`, `MarkerTrigger`, `UMP_*`,
  `UM02_*`);
- map controllers on markers (`Demo_Attract_Controller` on `DEMO_CONTROLLER`, `Demo_Land_Controller*`,
  `E3_Felucia_Zone`, `Training_Facility`, `Secondary*`).

Almost all use `PGStateMachine` (`main` = `Process_States`). Hero scripts also `require("HeroPlanAttach")`; PGStateMachine is required last, so its `Base_Definitions` / `main` win and `HeroService` never loops for them. [H data]

---

## 2. Lifecycle

### 2.1 Order inside one instance

| step | when | what runs | `Object` |
|---|---|---|---|
| 1 | pool load (first attach of that instance) | the file's top level: `require`s (PGStateMachine → pgcommands → PGBase → PGDebug, PGBaseDefinitions), function definitions, `ScriptPoolCount = n` | nil |
| 2 | attach (inside `Create_Object`) | `Base_Definitions()`. PGStateMachine's version: `Common_Base_Definitions()` (ThreadValue / GetEvent reset, tables reset, `Object = nil`, `LastService = nil`), the state tables, `ServiceRate = 0.2`, `Definitions()` if defined, `PG_Story_Mode_Init()` if PGStoryMode was required | nil |
| 3 | attach | `main` coroutine created (not run) | set at the end of attach |
| 4 | the object's first due service (normally the next frame, OBJECT_RUNTIME §5.3 "created during the loop") | `main()` first resume → `Process_States()` → first state `OnEnter`, then `OnUpdate` / `PumpEvents()` loops; each `PumpEvents` yields once (one pump = one `OnUpdate`) | set |
| 5 | later pumps | resume from the last yield; after the resume `PumpEvents` drains this thread's GetEvent queue (always empty for object scripts, §4.1) | set |

Extra threads made by `Create_Thread` are appended to the thread list and resumed after `main`, in creation order.
[H, the shared pump]

### 2.2 Ending a script

| cause | mechanism | result | conf |
|---|---|---|---|
| `ScriptExit()` | Lua `_ScriptExit()` (0x255610) sets +0x121; in a thread it then yields false | the pump stops after this thread, releases the script (0x2488e0); the object service drops +0x2D8 | H |
| `main` returns (PGStateMachine: `NextState = nil`) | it calls `ScriptExit()` itself | same | H data |
| binding error (any `GameObjectWrapper::... -- invalid ...` message), `ScriptError` path | error helper 0x248190: with no logger installed (retail) it **only sets +0x121** | the script ends at the end of the current pump. So in retail a wrong argument to any binding kills the whole object script | H (LUA_SCRIPT_OUTPUT §1) |
| Lua runtime error in a thread (`lua_resume` ≠ 0) | 0x246940 reports (no logger) and the thread is removed (0x248d10) | **only that thread dies**; the script stays attached with fewer threads (with PGStateMachine: no threads left, an idle attached script) | M (no +0x121 write found on that path) |
| thread yields anything but `true`, or returns | thread removed | as above | H |
| `ScriptExit()` from `Base_Definitions` (no thread; `GetThreadID() < 0`) | flag only | the first due pump releases it before running anything | H |

Many vanilla scripts exit at once outside their mode (MissileShield / PointDefense: `Get_Game_Mode() == "Galactic"`;
DarthVader: not "Land"). The **galactic object** of such a hero therefore loses its script on its first galactic pump.
After that, AI hero Setup C finds neither an object script nor (without `Execute_Script_On_Type`) a type script, and
the hero is no longer a hero record (AI_GALACTIC_3 §2.1 item 4). [M: the consequence follows from the code paths;
not probed]

### 2.3 Release 0x2488e0

Steps [H]:
1. Set +0x121.
2. Kill all threads (0x2490c0).
3. Clear the `LuaThreadTable` registry entry.
4. Unregister from a global table (0x220ed0, kind 0x2F) [L: what that table is].
5. Then one of two branches:
   - **Poolable** (+0x124):
     1. call the Lua `Flush_G()` if it is a function. Vanilla PGBase: delete every global table except `_LOADED`,
        `coroutine`, `string`, `LuaWrapperMetaTable`, `_G`, `security`, `table`, and every userdata whose
        `Is_Pool_Safe()` is false. All wrapper classes (GameObject, Player, TaskForce, block statuses, ...) return
        false (0x2586d0). The command objects use the base `LuaUserVar` true (0x245ca0) and stay. Functions,
        numbers, strings and booleans stay;
     2. call `Base_Definitions()` **now**, so `Definitions()` runs at release time, not at the next attach;
     3. reset the Lua stack;
     4. clear +0x122 / +0x123;
     5. GC.

     The instance is now free in its pool.
   - **Not poolable:** close the `lua_State`, remove it from the global script registry 0x140b09dc0, notify the
     debugger hook.

TR replaces `Flush_G` with a copy that also does `_LOADED = {}`, so on reuse every `require` runs its module again.
Vanilla keeps `_LOADED`: required libraries are not re-run, but their global tables (e.g. `StateMachine`) were deleted
and are rebuilt by `Base_Definitions`. [H data]

### 2.4 Object death

- **Destroy** (OBJECT_RUNTIME §2.2) sets the dead bit 0x02 at once. The script service checks this bit (§3.2), so no
  thread runs again, not even in the same frame for objects later in the list. [H]
- **No callback** reaches the dying object's own script. Other scripts notice through their own polling
  (`Register_Death_Event` → `TestValid`). The object's `Object` wrapper is nulled by signal 0x10 (OBJECT_RUNTIME §2.3),
  so `TestValid(Object)` is false. [H / M]
- **Freed:** the GameObjectClass destructor 0x3893e0 calls the release 0x2488e0 on +0x2D8 and drops the reference.
  This happens when the last object reference goes (normally the deferred-delete flush at the end of the store update,
  plus any Lua wrappers still holding it). [H release; M timing]
- **Despawn from its own script** (TechPowerUp: `Object.Despawn()` inside its proximity callback) completes the current
  resume up to its next yield. After that the script is never resumed. [M]

### 2.5 Mode end, mode changes, company spawn

- **Tactical mode end:** the land / space stores are torn down with their objects, so their scripts are freed
  (destructor path), pooled ones back to their pools. [M]
- **Pool teardown:**
  - the pools themselves survive until the Lua system shutdown 0x248c60 (`Free_Script_Pool` 0x244cc0, warning
    "still in use"), called on a new galactic campaign "when asked" (GAME_SCORING §1.3 step 1);
  - End_All only GCs the type instances (§1.5).

  [M]
- **Galactic ↔ tactical:** objects never move between modes. A battle creates new tactical objects that carry the
  parent-mode ID of their galactic counterpart (OBJECT_RUNTIME §2.5). Each new object gets a **fresh** script by its
  type, and nothing is copied: the only code that touches another object's +0x2D8 is the death clone, which releases
  it. [H]
  - The galactic objects keep their scripts across the battle.
  - Those scripts do not run while the galactic store is paused (battle pending / story block, §3.2) or while galactic
    is not the ticking mode.
  - Their clock is the galactic mode frame, which is frozen meanwhile, so no `ServiceRate` time passes.

  [H gate; M "not ticking"]
- **Company spawn:** a landing `GroundCompany` creates its `Company_Units` members with `Create_Object`. Each member
  gets its type's script like any object (§1.2). The company type itself normally has no script.
  - TR's companies list exactly one `*_Company_Dummy` member, so landing a TR company creates one dummy and its
    `GenericCompanySpawner`.
  - The units the spawner creates with `Spawn_Unit` get their own types' scripts in turn.
  - There is no engine re-attach step.

  [H data, M engine path for the landing (STRATEGIC_GAPS §1.7, BATTLE_TRANSITIONS)]

### 2.6 Save / load

- **Save** (object save 0x3a5ad0, unless one of two flags is set):
  - micro-chunk **0x4B6** = the script name (instance +0x68);
  - micro-chunk **0x4B5** = the instance's state (LuaScriptClass save 0x247e50).

  [H]
- **Load** (0x39c3a0), gated by the flag 0x140b2c37b being 0 [L: meaning]:
  1. 0x4B6 reads the name;
  2. 0x4B5 re-attaches by name (0x38cf30, §1.3);
  3. then LuaScriptClass load 0x246310 restores the instance:
     - the thread list is rebuilt from the saved thread function names (0x244740 per name);
     - each thread's coroutine image and the main state are **unpersisted** (0x7b9e60, a Lua-state persistence
       reader);
     - the pending-resume values are restored;
     - the global `ScriptShouldCRC` is read into +0x125.

  [H for the chunk flow; M for the persistence reading]
- **Port:** the port uses its own save format (N1). The behaviour to keep is "after a load, each object's script
  continues where it was": same globals, same thread positions, and `LastService` kept so that no new RNG draw
  happens.

---

## 3. Service

### 3.1 Who and where in the frame

- The mode tick runs the **GOM service** (MAIN_LOOP §3.3 step 8: primary store, then secondary) → store update
  0x2be640.
- The store update walks the **serviced list (GOM +0xE8) newest-registered first** and calls the object Service
  0x3a6b80 on each (OBJECT_RUNTIME §5.3).
- The script step is step 7 of that Service: after behaviours, late update and model time, before the hard-point
  service.
- Only the ticking mode's stores run, so galactic object scripts never run during a battle. [H]
- **Order between objects:**
  - registration order, newest first. It is not sorted by ID and is not pointer-keyed, so it is deterministic.
  - Registration happens at construction (the attach registers too), so for objects created in the session the order
    is construction order reversed, which is descending ID.
  - Objects that registered late (a serviced behaviour added later) or that were restored by a load follow their own
    registration time.

  [M]

### 3.2 Gate (object Service 0x3a6b80, script part) [H]

1. **Skip** the whole step when:
   - the "paused" service flag is set: the store is galactic and the battle-pending counter 0x140b1540c > 0, or the
     story block; then the objects are serviced in paused mode only when 0x140b2766c is set, otherwise not at all
     (A1_READINGS a1j t05);
   - the object has no script;
   - or the object is dead (+0x3A0 & 2).
2. Read the globals `ServiceRate` and `LastService`. Only Lua numbers count; any other type counts as absent.
3. `ServiceRate` absent → nothing (no pump, no `LastService` write, no exit check).
4. now = (float)(mode frame, store +0x20 → +0x10) × 0.033333335f (the float at 0x140b0a344).
5. Pump when `LastService` is absent, or when (double)now − LastService **>** ServiceRate (strict; double
   arithmetic on the float now).
   - Because of the frame granularity and the float rounding, `ServiceRate = 1` gives a 30- or 31-frame period; the
     port must use the same float × constant, then double subtraction.
6. Pump (0x247a90, §3.3).
7. After the pump:
   - +0x121 clear → write `LastService`:
     - if it was absent, **−(synced float draw)** (0x1ffbb0 on the game LCG 0x140a13e24: one LCG step,
       `((s>>10)&0x7fff)/32767 × (hi − lo) + lo`, lo = 0). hi is the value in xmm2 at the call, which the decompile
       does not show [L: probably `ServiceRate`];
     - else now.
   - +0x121 set → drop the reference, +0x2D8 = null. The pump has already released the instance.
8. Consequence of the negative first value: the second pump comes when now + r > ServiceRate.
   - For an object created at a mode frame near 0, that spreads the second pumps randomly over the first period.
   - For an object created later, the second pump comes on the very next service, then the period settles.

   [H for the arithmetic]

**Determinism:**
- The first-pump draw is on the **synced** stream, so every object script attach shifts all later synced draws (AI,
  combat, spawns). The port must make this draw at the same point, in object-service order.
- An object script that never pumps (no `ServiceRate`) makes no draw.

[H]

### 3.3 The pump 0x247a90 (shared with AI plans, GameScoring, story)

- If +0x121 is already set: do not resume anything; release (0x2488e0).
- Otherwise, for each live thread in list order:
  1. push the pending argument (thread entry +0x38) if any;
  2. `lua_resume`;
  3. resume error → report, remove the thread;
  4. finished, or yielded anything but `true` → remove the thread;
  5. yielded `true` → keep it;
  6. +0x121 now set → stop the pass.
- After the pass, if +0x121 is set: release (0x2488e0).
- Each `PumpEvents()` is one `coroutine.yield(true)`, so one pump advances each thread by one `PumpEvents` / `Sleep`
  step.

[H]

### 3.4 +0x121 and the release

+0x121 ("exited / failed") is set by:
- `_ScriptExit`;
- the error helper 0x248190 when no logger is installed (always in retail);
- the release itself.

Once it is set, the next pump releases the instance and the object service nulls +0x2D8. Nothing re-attaches a script
to that object afterwards (only a type re-init or a load would). [H]

---

## 4. Events delivered to object scripts

### 4.1 No queued engine events [H]

- `GetEvent()` (0x25a190) pops from the per-thread queue in the script's GetEvent store (+0xD8), indexed by the
  current thread (+0x60).
- The only function that appends to it is 0x25ab50, called only by the task-force event dispatch 0x6006b0 (AI plan
  scripts, AI_GALACTIC "Events").
- So `PumpEvents`' drain loop is always empty in an object script. No signal-to-Lua-function table exists for object
  scripts.

### 4.2 Lua-level registrations (PGCommands.lua), all polled

`Pump_Service()` runs inside every `PumpEvents()` before the yield, in this order: `Process_Timers`,
`Process_Death_Events`, `Process_Proximities`, `Process_Attacked_Events`, then `Process_Reinforcements` and
`Story_Mode_Service` when they are defined. [H data]

| function | storage | fired when (checked each pump) | callback args |
|---|---|---|---|
| `Register_Timer(func, timeout, param)` | `TimerTable[func]` list | `timeout + start < GetCurrentTime()` | `(param)` |
| `Register_Death_Event(obj, func)` | `DeathTable[obj]` | `not TestValid(obj)` | none |
| `Register_Attacked_Event(obj, func)` | `AttackedTable[obj]` | `FindDeadlyEnemy(obj)` becomes non-nil / nil | `(true, enemy, obj)` / `(false, nil, obj)` |
| `Register_Prox(obj, func, range, player)` | `obj.Event_Object_In_Range(func, range, player)` + `ProxTable[obj]` (refused in galactic) | via `obj.Service_Wrapper()` | see §4.3 |

These tables are keyed by functions or wrappers and iterated with `pairs`, so their order is Lua's hash order. That
order is deterministic for a given Lua build and insertion history, but it is an order the port's Lua must reproduce
(the vendored Lua 5.0.2 does). [M]

### 4.3 `Event_Object_In_Range` / `Service_Wrapper` / `Cancel_Event_Object_In_Range` [H unless tagged]

- **Storage.** The registrations live on the **wrapper**, not on the object: vector at wrapper +0x40 of 0x28-byte
  entries, each holding range (+0), function (+8), optional type filter (+0x10), optional player filter (+0x18) and a
  per-pass "done" byte (+0x20). Wrappers are cached per (object, script) (OBJECT_RUNTIME §2.3), so each script keeps
  its own list for the same object.
- **`Event_Object_In_Range(func, range [, a3 [, a4]])` (0x575c50):**
  - errors (→ +0x121): fewer than 2 args, arg 1 not a function, arg 2 not a number;
  - a3 / a4 may each be a PlayerWrapper (player filter) or a GameObjectTypeWrapper (type filter), in either order;
  - appends an entry and sets the "list changed" byte (wrapper +0x38);
  - a dead object → nothing.
- **`Cancel_Event_Object_In_Range(func)` (0x574a30):** removes every entry whose function equals `func`, and sets
  "list changed". With no args → error.
- **`Service_Wrapper()` (0x57f7e0):** for each entry not yet done in this call:
  1. Query the store's spatial tree with an **axis-aligned box** of half-size `range` in x, y and z around the
     object's position (0x2ab640 → `Query_AABB` 0x20e780). It is not a sphere. [H for three equal extents, M for
     which tree]
  2. For each hit, in query order, skip it if it is: dead, an `Is_Decoration` type (+0x3B), a projectile (behaviour
     0x08), a particle (0x0C), or the object itself.
  3. If a player filter is set, skip hits whose owner is not allied with the filter player (0x2823e0, relation 0:
     same player or ally).
  4. For a hit with a garrison (+0xF8), visit the garrisoned objects first, last to first. A dead garrisoned object is
     passed as an empty wrapper.
  5. Each visited object (garrisoned, then the hit itself) that passes the type filter (exact type) triggers
     **`func(prox_wrapper, object_wrapper)`**, called **synchronously** inside `Service_Wrapper`. The wrapper is the
     cached one for (object, calling script).
  6. If the callback changed the list (register / cancel) the scan restarts from the first entry. Entries already
     done are skipped, so every entry fires at most once per call.
  7. Every in-range object fires **every** call. There is no enter / leave edge: a callback that should fire once
     must cancel itself (TechPowerUp, PGCommands users).
- **Who calls `Service_Wrapper`:**
  - `PumpEvents` (`Object.Service_Wrapper()` before every yield, for the script's own object);
  - `Process_Proximities` for every `Register_Prox` object;
  - so a proximity check runs once per pump, at the `ServiceRate` of the script that registered it.

**Determinism:** the hit order is the spatial tree's query order. The port has to define and keep a fixed order
(e.g. the tree's own insertion order, or ascending ID) and document it; the original order is not specified beyond
the tree walk. [L]

### 4.4 Engine calls into an object script by name [H]

From all callers of the call-by-name helper 0x2437c0:
- `Base_Definitions` (attach, §1.3; type load, §1.5);
- `Flush_G` / `Base_Definitions` (release, §2.3);
- `Evaluate_Attack_Ability` / `Get_Escort_Ability_Weights` (AI hero Setup C, plus reading `MinPlanAttachCost` /
  `MaxPlanAttachCost` and writing `PlayerObject`).

The other names in that list belong to GameScoring, story, free store, evaluators and AI plans. Cross-script calls
through `LuaScriptWrapper.Call_Function` / `Get_Variable` / `Set_Variable` exist (STRATEGIC_GAPS §5.2), but no shipped
script uses them on object scripts.

---

## 5. Functions object scripts typically use, and the port's bindings

`docs/re/LUA_API.md` lists the exe's API but carries no port-status column. So the check below is a name search
(2026-11-03) of the hand-written registration tables in `engine/lua/src/bind_*.cpp`, excluding the generated
`binding_table.inc`. "Registered" means a Lua name is bound to a port function. How faithful each binding is was not
checked.

**Used only by object scripts** (they need `Object` or the object-script service):

| function | RVA | object-script use | port |
|---|---|---|---|
| `GameObjectWrapper.Service_Wrapper` | 0x57f7e0 | every `PumpEvents` | registered |
| `GameObjectWrapper.Event_Object_In_Range` | 0x575c50 | TechPowerUp, triggers, `Register_Prox` | registered |
| `GameObjectWrapper.Cancel_Event_Object_In_Range` | 0x574a30 | 20 sites | registered |
| `ScriptExit` → `_ScriptExit` | 0x255610 | 33 sites | registered |
| `GetThreadID`, `ThreadValue`, `GetEvent` (+`Params`, `Reset`) | 0x255560, —, 0x25a190 | PGBase `Sleep` / `PumpEvents` / `ScriptExit` | registered |
| `Object` (the global) | 0x38cf30 | everywhere | the name is present in `bindings.cpp`; there is no object-script host |
| `ServiceRate` / `LastService` (globals read by the engine) | — | every script | `LastService` is referenced in `bindings.cpp`; `ServiceRate` is not |

**Also used, shared with story / AI scripts** (vanilla `GameObject\*.lua` call counts):
- `Get_Owner` 93, `Is_Ability_Autofire` 37, `Activate_Ability` 37, `Is_Human` 36, `Get_Position` 35,
  `Get_Parent_Object` 27, `Find_Nearest` 27, `Is_Category` 26, `Get_Type` 26, `Find_All_Objects_Of_Type` 26,
  `Get_Game_Mode` 25, `Attack_Move` 25, `Find_First_Object` 22, `Reveal_All` 18, `Is_Enemy` 18, `Get_Hull` 18,
  `Is_Ability_Ready` 16, `Suspend_AI` 16, `Lock_Controls` 16, `Play_SFX_Event` 15, `GameRandom` 14,
  `Is_In_Garrison` 13, `Find_All_Objects_With_Hint` 13, `Create_Generic_Object` 12, `Change_Owner` 11,
  `Get_Distance` 10, `FindDeadlyEnemy` 9, `Teleport_And_Face` 7, `Take_Damage` 7;
- `Set_Garrison_Spawn`, `Move_To`, `Give_Money`, `Get_Shield`, `Get_Combat_Rating`, `Set_Selectable`,
  `Get_All_Projectile_Types`, `Is_Affected_By_Missile_Shield`, `Is_Affected_By_Laser_Defense`,
  `Attach_Particle_Effect`, `Prevent_AI_Usage`, `Is_Ability_Active`, `Guard_Target`, `Despawn`,
  `Suspend_Locomotor`, `Set_Cannot_Be_Killed`, `Override_Max_Speed`, `Get_Rate_Of_Damage_Taken`,
  `Get_Contained_Object_Count`, `Disable_Capture`, `Has_Active_Orders`, `Give_Random_Sliceable_Tech`, `Highlight`,
  `Make_Invulnerable`, `Kill`;
- `WeightedTypeList.Create` / `.Parse`, `EvaluateTypeList` (hero scripts);
- cinematic commands (UM / demo controllers).

Every one of these names is **registered** in the port's bind files.

---

## 6. TR `GenericCompanySpawner`: what it needs

Files (TR `Data\Scripts`):
- `GameObject\GenericCompanySpawner.lua`;
- `Library\eawx-std\ModContentLoader.lua`;
- `Library\deepcore\std\{deepcore,PluginLoader,plugin_updatecycle,plugintargets,class,callable,Observable,Queue}.lua`;
- `Library\deepcore\crossplot\{crossplot,KeyValueStoreBasedEventBus,GlobalValueKeyValueStore,MessageQueue,serializer,context}.lua`;
- `Library\deepcore\galaxy\*` (required, only class definitions);
- `Library\eawx-plugins-gameobject-land\company-spawn\{init,CompanySpawn}.lua`;
- `Library\eawx-util\StoryUtil.lua`;
- `Library\GroundCompanyList.lua`;
- `Library\eawx-mod-icw\GameConstants.lua`;
- `Library\eawx-mod-icw\gameobjects\ground\company-objects\<NAME>.lua` (113 entries);
- `Library\TacticalSwapLibrary.lua` (HeroSwap only).

### 6.1 Flow

1. **Definitions** (at attach): `ServiceRate = 1`; one state, `State_Init`.
2. **First pump**, `State_Init(OnEnter)`:
   1. `Get_Game_Mode()` is "Galactic" or "Space" → `ScriptExit()`.
   2. `ModContentLoader = require("eawx-std/ModContentLoader")`.
   3. `ModContentLoader.get("GameObjectLibrary")` → `require("eawx-mod-" .. lower(GlobalValue.Get("MOD_ID")) ..
      "/GameObjectLibrary")`.
   4. `get_ground_company_object_script(Object.Get_Type().Get_Name())`:
      - `GroundCompanyList[name] == true` (keys are **upper-case** type names, so `Get_Name` must return the
        upper-cased stored name, OBJECT_TYPES §3.1);
      - then `require(<mod path>/gameobjects/ground/company-objects/<NAME>)`, which returns
        `{Spawn_Units = {...}, Scripts = {"company-spawn"}}`;
      - `Flags.FULLINHERIT` / `COMPANYINHERIT` are followed recursively.
   5. No entry, or no `Scripts` → `ScriptExit()`.
   6. `deepcore:game_object{plugins = entry.Scripts, plugin_folder = "eawx-plugins-gameobject-land"}`:
      - `crossplot:game_object()`: an event bus named `tostring(Object)` over a GlobalValue key-value store
        (serialised with `loadstring`);
      - `PluginLoader:load` → `require("eawx-plugins-gameobject-land/company-spawn/init")` → `init()` →
        `CompanySpawn(entry)`.
   7. `CompanySpawn:new(entry)`:
      1. `Object.Prevent_AI_Usage(true)`, `Set_Selectable(false)`, `Make_Invulnerable(true)`, `Hide(true)`,
         `Set_Importance(0)`;
      2. `Object.Get_Position()`, then a 5 × 5 grid of spawn points 5 apart (`Get_XYZ`, `Create_Position`);
      3. `Object.Get_Owner()`; per spawn type, the faction row `Get_Faction_Name()`, else `GameConstants.ALIASES`,
         else `DEFAULT`; `ResearchType` checks `GlobalValue.Get("FIGHTER_RESEARCH")` (a table);
      4. for unit i, `Register_Timer(CompanySpawn.spawn, 1 + i, {self, data})`;
      5. `Object.Teleport(Create_Position(-1000, -1000, 0))`.
   8. `Sleep(5)`: `PumpEvents` loops; the timers fire during it.
3. **Timer callback** `CompanySpawn.spawn`: `Find_Object_Type(name)`, then
   `Spawn_Unit(type, grid_point, original_owner)[1]`. It re-applies `Prevent_AI_Usage` / `Make_Invulnerable` /
   `Set_Selectable` on the dummy and stores the unit.
4. **Every later pump**, `OnUpdate` → `EawXObj:update()`:
   1. `crossplot:update()`: `GlobalValue.Get("busid:<tostring(Object)>:notify")`, which must return nil (or "") when
      unset; the message queue is empty.
   2. `CompanySpawn:check_company()`:
      - drop units with `not TestValid(unit)`;
      - if alive / total ≤ 0.4 → `Object.Take_Damage(999999)`. Lua `Take_Damage` uses damage kind 0, which passes
        invulnerability (TACTICAL_COMBAT §3.6), so the hidden invulnerable dummy dies. The dummy is
        `Victory_Relevant`, so this matters for the battle end;
      - HeroSwap entries (`KYLE_KATARN_DUMMY` only): `Find_All_Objects_Of_Type(hero_name, owner)` per hero form; the
        dummy dies when none are left after one was seen.
5. **Random_Types** entries (none in this TR build) seed from `Object.Get_Parent_Mode_Object_ID()`.

### 6.2 Engine requirements list

| need | detail | conf |
|---|---|---|
| attach | inherited `Lua_Script` on the dummy; attach at `Create_Object` in land; `Object` set after `Base_Definitions` | H |
| service | `ServiceRate = 1` gate (§3.2) including the first-pump RNG draw; PGStateMachine `main` | H |
| exit | `ScriptExit()` from a thread stops at once (yield false) and frees the script; also used in Galactic / Space | H |
| `GlobalValue` | process-wide; `MOD_ID` written by GameScoring's `Base_Definitions` at session start (GAME_SCORING §1.2), read at the spawner's first pump; string keys, table and string values; `Get` of an unset key → nil | H (GAME_SCORING §4) |
| Lua library | Lua 5.0 `require` with `_LOADED` as a **global** table (TR's `Flush_G` assigns `_LOADED = {}`); `LUA_PATH` over the `Library` root with `/` subfolders and case-insensitive file names (`eawx-std/ModContentLoader`, `deepcore/std/deepcore`, ...); `loadstring`, `string.format/sub/find/gsub/lower/dump`, `table.insert/remove/getn`, `unpack`, `setmetatable`, the `arg` vararg table | M (Lua 5.0 semantics) |
| globals | `Get_Game_Mode`, `Find_Object_Type`, `Find_All_Objects_Of_Type(name, player)`, `Spawn_Unit(type, position, player)` → table, `Create_Position`, `GetCurrentTime` (timers, `Sleep`), `ThreadValue`, `GetEvent`, `GetThreadID`, `_ScriptExit`, `_ScriptMessage` / `_OuputDebug` (DebugMessage) | H |
| `Object` methods | `Get_Type` (+ type `Get_Name`), `Get_Position` (+ `Get_XYZ`), `Get_Owner` (+ `Get_Faction_Name`), `Prevent_AI_Usage`, `Set_Selectable`, `Make_Invulnerable`, `Hide`, `Set_Importance`, `Teleport`, `Take_Damage`, `Get_Parent_Mode_Object_ID`, `Service_Wrapper` (no registrations → no-op); `tostring(Object)` (wrapper `__tostring`) | H |
| on spawned units | `TestValid` (wrapper validity; false once dead) | H |
| events | none from the engine | H |
| pooling | the script has no `ScriptPoolCount` → pooled, the pool grows to the number of live dummies; reuse runs TR's `Flush_G` (`_LOADED = {}`) + `Base_Definitions`, so deepcore is re-required for each reused dummy | H |

Every function in the table is registered in the port's bind files (§5).

---

## 7. Port notes

### 7.1 Minimum for company spawners and the shipped FoC object scripts

1. **Host:**
   - a per-object script slot, attached in object construction after the behaviour lists;
   - all modes and both stores; failure to load = no script;
   - death clones strip it.
2. **Instance:**
   - its own Lua state per object script;
   - the attach order of §1.3, **`Object` last**;
   - register the same base and command globals as every other script.
3. **Pool:**
   - Simplest correct start: no pooling; a fresh state per attach.
   - Differences against retail are limited to what survives `Flush_G` in a reused instance: functions, scalar
     globals, and vanilla's `_LOADED` cache.
   - Shipped scripts reset everything they read in `Definitions`, so a fresh state is behaviour-equivalent for them.
   - Add the pool later for load cost (TR re-requires deepcore per dummy anyway). If pooling is added, it must call
     `Flush_G()` + `Base_Definitions()` at release, as §2.3. [M]
4. **Service:**
   - in the object's service, after behaviours;
   - skipped when dead or paused;
   - the exact gate of §3.2: float time; strict `>`; the double compare; the **synced RNG draw on the first pump**;
     `LastService` written back as a Lua number.
   - Use the shared thread pump the port already needs for AI plans and GameScoring (resume in creation order; stop
     on +0x121; release).
5. **Exit:**
   - `_ScriptExit` and binding errors set the exit flag;
   - the release frees the slot;
   - a Lua runtime error kills only the thread.
6. **Death:** stop servicing at the dead flag; free the script when the object is freed; no callback.
7. **Proximity:**
   - `Event_Object_In_Range` / `Cancel_Event_Object_In_Range` / `Service_Wrapper` with per-(object, script) wrapper
     lists;
   - AABB half-size `range`; the filters and exclusions; garrisoned objects; synchronous callbacks
     `(prox, object)`; fire on every call;
   - restart the scan on list change.
   - Choose and document a deterministic hit order.
8. **AI hooks:** Setup C reads hero scripts from the unit's object script, else the type instance. The galactic AI
   port needs the object slot, the type instance (`Execute_Script_On_Type`) and the by-name call.
9. **Determinism tests:**
   - the per-worker CRC tests must include a map with scripted objects (the RNG draw and the service order are
     sim-visible through `GameRandom`, spawns and `Activate_Ability`);
   - the scripts run on the sim thread only, in store order.

### 7.2 Can wait

| item | why it can wait |
|---|---|
| Pooling (`ScriptPoolCount`, reuse, `Flush_G` at release) | see item 3 |
| Save / load of script state | needs Lua-state persistence in the port's own format. Until then, a load could re-attach fresh scripts, but that re-runs `OnEnter` (TR would spawn the company again), so mark saves with live object scripts unsupported rather than silently wrong |
| `Execute_Script_On_Type` outside the AI | no other reader |
| Type hot-reload re-attach (0x39fc90) | debug only |
| `LuaScriptWrapper` cross-script calls | no shipped object-script use |
| Galactic-mode object scripts | vanilla's exit in galactic (state machines) or idle (`HeroService` empty). Still attach and service them (one RNG draw each and the AI hero-record consequence of §2.2), but their bodies do nothing |

---

## 8. Open items

1. **Upper bound of the first-pump RNG draw** (§3.2 step 7). The decompile shows lo = 0, but hi is in xmm2 at the
   call. Probably `ServiceRate`. Needs the disassembly of 0x3a6b80 around the `LastService` creation. [L]
2. **Flag used by the load re-attach** (0x39c3a0 → 0x38cf30: normal vs from-save path). The third argument is dropped
   by Ghidra. Only matters for retail save compatibility, which is out of scope. [L]
3. **Meaning of 0x140b2c37b** (gates the model creation in object init and the script chunk on load; set and cleared
   by 0x27c9c0). [L]
4. **Spatial tree used by `Service_Wrapper`** (0x2ab640 on the store: global or per-player tree) and its hit order.
   [M / L]
5. **Lua runtime error path**: confirm that 0x246940 / 0x248d10 never set +0x121 (only the thread dies). A probe
   could check this: an object script with a deliberate `nil` call in a second thread, and `main` still running
   afterwards. [M]

---

## 9. Port status (q6 t03, 2026-11-03; q6 t12, 2026-11-04)

Implemented in `engine/apps/sim` (`object_scripts.hpp` / `.cpp`, `ObjectScriptHost`, owned by `SimHost`), with hooks in
`engine/objects` (`ObjectStore::on_created`) and `engine/lua` (proximity events, `ObjectBridge::spawn_unit`,
`LuaVmOptions::upper_type_names`). Tests: `alamo_sim_tests` (`object_script_test`), `alamo_lua_objects_tests`
(proximity).

**Opt-in, off by default.** `SimConfig::object_scripts` (false), `alamo_headless --object-scripts`. With the switch off
nothing attaches and no RNG is drawn, so every existing golden and CRC stream is unchanged.
**[port interim rule, kept by Gabriel 2026-11-04]** `alamo_game` (`GameSession`) turns the switch on only when a mod layer is loaded (`MODPATH`,
the VFS has mod directories). Vanilla FoC runs keep the old streams until the object-script service is checked
against the original by an in-game probe; then the rule goes and the switch is always on.

Port choices (`[port]`, each also commented in the code):

| Item | Choice |
|---|---|
| Stores | Only each mode's primary store attaches scripts; secondary (local / cosmetic) stores attach nothing |
| Pool | None: a fresh VM per object (§1.4) |
| Attach order | top level (require), GlobalValue + player world + object bridge, `Base_Definitions`, `main` thread created (not run), `Object` set last |
| Death clones | No script (`TacticalMode::spawn_death_clone` suppresses the hook); `Base_Definitions` does not run for them |
| Service point | After the whole primary store update of the mode (`GameMode::common_tick`; galactic after `GalacticSim::update`), not inside each object's service; newest-attached first; never while the mode is paused |
| Gate | now = (double)((float)mode frame x 0.033333335f); pump when `now - LastService > ServiceRate` (strict, double) |
| First-pump draw (§8 item 1) | `LastService = -(synced range_float(0, ServiceRate))`; the bound lives in `first_pump_draw_hi` only |
| Release | ScriptExit or a binding script error (+0x121) after the pass; death / gone object at once (no deferred delete); mode end |
| Lua runtime error (§8 item 5) | Kills only that thread, the script stays |
| Proximity (§4.3, §8 item 4) | Synchronous callbacks inside `Service_Wrapper`; candidates in ascending object id; a callback that registers or cancels restarts the scan |
| `Get_Name` | Upper-case on object-script VMs only (`upper_type_names`); story / AI VMs keep the raw name until their goldens are regenerated |
| `Spawn_Unit` in a battle (q6 t12) | Through `TacticalBattle::script_spawn`: a type outside the battle tables is added to them first (`script_add_types`: `types_used` + `BattleCatalog::extend_battle` + the ability types), so it is a real battle unit (movement, combat, AI, the owner's lists), not a prop. A battle save stores the extended list; the load accepts it when the rebuilt list is its prefix |
| Company spawns (q6 t12) | A type with `Company_Units` goes to `ObjectBridge::spawn_company` → `TacticalBattle::script_spawn_company`: a built participant of the owner, members by `spawn_types` (`ConflictRules` groups) through `deploy()`, the REMAINING_GAPS §3.4 TEAM containers (Create_Team / Create_Team_Type / Max_Squad_Size). Returns the first TEAM container, else the first member. **[port]** member placement: the reinforcement drop layout (`land_drop_layout`) around the given point, each slot snapped passable; tactical spawn placement of a company is not in docs/re |
| `Take_Damage(amount)` (q6 t12) | Damage kind 0 (§6.1 step 4, TACTICAL_COMBAT §3.6 step 3): passes `Make_Invulnerable` on every host. In a battle (`ObjectBridge::take_damage` → `TacticalBattle::script_damage`) it is a `CombatSystem::take_damage` request; `Set_Cannot_Be_Killed` sets `kUnitCannotDie` for that call; the deaths it causes are processed at once (`process_death`: store removal, death clone, team, victory monitor), since the combat death list is cleared at the next combat tick. The hard-point form (`Take_Damage(n, name)`) and non-battle hosts keep the store path |
| `Teleport` (q6 t12) | L1_S4_REMAINING §3.4: through `TacticalScriptHooks::teleport` (`TacticalBattle::teleport`: store, movement, combat, trees), the facing kept. `Teleport_And_Face` still moves the store only; the garrison eject step of 0x3abb80 is not done |
| Dead wrapper (q6 t12) | `Event_Object_In_Range`, `Cancel_Event_Object_In_Range` and `Service_Wrapper` on a dead object are silent no-ops, not the generic "this object is dead!" thread error: the exe has no dead-object string for them (§4.3), and TR's spawner pumps once more after its dummy's `Take_Damage` kill |
| Saves | `save_allowed` refuses while any object script is live (§2.6: no script state is saved yet) |
| Not done | `Execute_Script_On_Type` (§1.5), pooling (§1.4), hot-reload re-attach, script state in saves (§2.6), the garrison step of `Teleport`, hangar / fighter types of late-added types (their hangar tables are built at battle start) |

Open RE questions (q6 t12; not in docs/re, so the port keeps the binding-state behaviour):

1. `Prevent_AI_Usage(true)` on a battle unit: which engine flag it sets (object +offset), and which AI systems skip
   such an object (tactical AI perception / goal targets, the threat grid, auto-targeting). The port only records it
   in `ScriptObjectState`; TR dummies stay AI-visible (`DUMMY` behaviour objects are not `UNIT_AI` in the port).
2. `Make_Invulnerable(true)`: the combat flag it sets and which damage kinds it blocks (all but kind 0?). The port
   only stores it in `ScriptObjectState`; battle combat does not read it yet.
3. `Hide(true)`: whether a hidden object is skipped by targeting, fog reveal and selection, or only by the renderer.
4. Tactical placement of a script-spawned company's members (`Spawn_Unit` → company create in land): formation
   offsets and the TEAM container position, versus the reinforcement drop layout the port uses.
