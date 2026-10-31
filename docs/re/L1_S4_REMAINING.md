# L1 / S4 remaining bindings and story sources (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by an RE agent for the L1 (Lua bindings) and S4 (story events) code agents.
Clean-room: prose and tables only, no decompiled code. Tags: **[H]** read directly in the decompile,
**[M]** inferred (confidence given), **[L]** guess. RVAs are image-relative (add 0x140000000 for the
Ghidra name). Conventions (error / warning helpers, "player", "AI target", readiness, destroy-after
sweep) are those of SCRIPT_GAPS.md (header). Behaviour-slot numbers are OBJECT_RUNTIME.md §4.3 ids.

Inputs: `logs/lua_binding_coverage.txt` (13 unbound globals, 50 unbound methods), LUA_API.md binding map.

> **Status 2026-10-30:** The coverage log now reads globals 143 (implemented 135, host values 8, unbound 0) and methods 323 (implemented 323, unbound 0), with all 26460 RE call sites covered (`logs/lua_binding_coverage.txt`); the entries counted as unbound here are bound, for example `Find_Best_Local_Threat_Center`, `Get_All_Projectile_Types` and `Give_Random_Sliceable_Tech` in `engine/lua/src/bind_script_remaining.cpp`. The 13 and 50 counts in this file are historical.

## Status

| § | Content | Status |
|---|---|---|
| 0 | Index: brief item → section | complete |
| 1 | Unbound globals (A), all 13 | complete |
| 2 | Unbound methods (A), all 50 | complete (zero-call items partly [M]) |
| 3 | Backing systems (B) | complete |
| 4 | Story event sources (C) | complete |
| 5 | Open items and in-game probes | complete |

## 0. Index (brief item → section)

| Brief item | Section |
|---|---|
| 13 unbound globals / 50 unbound methods | §1 / §2 (tables at the top of each) |
| Tactical special weapons, superweapons, station weapons, upgrade gate | SCRIPT_GAPS §1 (§3.1) |
| Hyperspace out (retreat, `Hyperspace_Away`), hyperspace in, teleport | §3.4 |
| Timed engine disables | SCRIPT_GAPS §2.2 |
| `Force_Weather`, tactical weather | §3.2 |
| Nebula / ion storm / asteroid membership and effects | §3.3, §1.6, §2.7 |
| Orbital bombardment, bombing runs | §3.8, §3.7, §2.10 |
| Garrison (`Can_Garrison`, `Garrison`, `Can_Garrison_Fire`, fire rules) | §3.5, §2.5 |
| Build pads (contents, lock) | §3.6, §2.2 |
| `Divert` / `Is_On_Diversion` / `Explore_Area` | §2.3 |
| `Release` | §2.12 |
| `Get_Time_Till_Dead` / `Get_Rate_Of_Damage_Taken` / `Get_Final_Blow_Player` | §2.4 |
| `Give_Random_Sliceable_Tech`, `Get_Bribe_Cost` | §2.9, §2.8 |
| `DiscreteDistribution`, `WeightedTypeList`, `GlobalValue`, `Budget`, `SFXManager`, `_ProduceObject` | §1.4, §1.8, §1.5, §1.3, §1.7, §1.9 |
| UnitCommanded / UnitGuarding sources | §4.0 (SCRIPT_GAPS §4.3–§4.4) |
| Garrisoned, StructureCaptured (+ capture rules), HeroCaptured, LoadTacticalMap | §4.1–§4.4 |
| Corruption mission start / success / failure | §4.5 |

Abbreviations: **GC** = GameConstants (base 0x140b15800); **FPS** = 0x140b0a340 (30); **now** = the
current mode frame (mode +0x10); **synced float(a, b)** = 0x532480 / 0x1ffbb0 and **synced int(a, b)** =
0x1ffb40, both on the synced LCG 0x140a13e24 (MOVEMENT §7); **skirmish** = 0x28afb0 (stand-alone
tactical game); **SW option** = game-options bit 0x140b154bc & 4 (skirmish superweapons).
Return-value convention of the bindings: "returns X" = one Lua value; "returns nothing" = zero values.
Type-test tags used below: *object* = GameObjectWrapper, *AI target* = AITargetLocationWrapper,
*player*, *type* = GameObjectTypeWrapper, *TF* = TaskForceClass; "position-convertible" = anything the
position extractor 0x4990b0 accepts (object, AI target, position, task force).

---

## 1. Unbound globals (A)

> **Status 2026-10-30:** The "unbound" title and the 13 count are historical: every global in this section is now bound (coverage log: unbound 0; `engine/lua/src/bind_script_remaining.cpp`). The behaviour specs below remain valid.

| Global | Calls | Spec |
|---|---|---|
| `Find_Best_Local_Threat_Center` | 10 | §1.1 |
| `Find_Nearest_Space_Field` | 6 | §1.6 |
| `Force_Weather` | 1 | §3.2 (land, one phase advance, no args, no result) |
| `Are_On_Opposite_Sides_Of_Shield` | 0 | §1.2 |
| `Budget` | 0 | §1.3 |
| `DiscreteDistribution` | 0 | §1.4 |
| `GlobalValue` | 0 | §1.5 |
| `Is_Point_In_Asteroid_Field` / `_Ion_Storm` / `_Nebula` | 0 | §1.6 |
| `SFXManager` | 0 | §1.7 |
| `WeightedTypeList` | 0 | §1.8 |
| `_ProduceObject` | 0 | §1.9 |

### 1.1 `Find_Best_Local_Threat_Center(objects, radius)` 0x6a95e0 [H]

- Exactly 2 args (error otherwise); arg 1 a table (error "Expected table"), arg 2 a number (error).
- Each table entry must be an object: other entries → **warning** "invalid type for entry at index %d",
  skipped; objects whose wrapper is dead are skipped silently. Collected list is then **bubble-sorted by
  object id** (+0x50, ascending). Empty → warning "no objects supplied", returns nothing.
- **Clusters** (0x6a8eb0), n = object count, k = min(10, ceil(n / 4)):
  - k ≤ 1 (n ≤ 4): one centre = the mean 2D position; **no RNG**.
  - k ≥ 2: bounding box of the objects (x, y); each centre gets **two synced draws**: first y =
    synced float(minY, maxY), then x = synced float(minX, maxX) (centres in index order). Then exactly 5
    Lloyd iterations: assign each object (list order) to the nearest centre by 2D squared distance
    (strict <, first centre wins ties); for each centre with ≥ 1 member, centre := mean of members;
    empty centres keep their previous position.
- **Score** of a centre = Σ over objects with 2D distance² ≤ radius² (compared in double) of the AI
  combat power of the object's type (0x370e60 on 0x396380: the team's representative type, else the
  original type; `AI_Combat_Power` +0xdf0, scaled by squad fill in tactical — §2.4 note). Best = highest
  score, **ties go to the later centre** (≥).
- Best score ≤ 0 → returns nothing. Else returns **two values**: a Position (x, y, **z = 0**) and the
  score (number).
- Determinism: the 2k draws happen whenever k ≥ 2, even if nothing is returned.

### 1.2 `Are_On_Opposite_Sides_Of_Shield(a, b [, player, bool])` 0x6a44e0 [H unless tagged]

- 2 or 4 args (error otherwise). a and b position-convertible (errors "could not extract a position from
  parameter 1/2"); when a / b are objects they are also passed as objects. With 4 args: arg 3 player
  (error), arg 4 bool (error).
- Requires a land mode in the mode stack (0x28a950(GMM, 1); error "only valid in land mode"); returns a
  bool from 0x3b5ad0:
  - Gate [M]: only when both a and b are objects, collect the projectile types a would fire at b
    (0x394eb0: current projectile, plus weapon hard points that can fire at b); if any has
    `Projectile_Does_Shield_Damage` (+0x45) == false, only shields whose shield pack +0xc is set count.
  - Walk the land store's object list (+0x2f0): an object with BASE_SHIELD (0x39) whose shield is active
    (shield pack +0x28 → +0x1c8, +8 == 1), effective radius r = `Base_Shield_Radius` (+0x21f4) > 0, and
    (no player given, or 0x39a2c0(shield object, player) == bool [M: "shield is friendly to player"]).
    If exactly one of the two points is strictly inside (3D distance² < r²) → **true** (first such
    shield). None → false.
- No shipped calls.

### 1.3 `Budget` [H]

Not a function: a per-script global holding a `BudgetWrapper` (id 0x80e) bound to the calling AI plan's
goal. It is registered by the plan-script set-up 0x5f7090 (with `Target`, `AITarget`, `PlayerObject`)
**only when the current mode is not tactical** (mode vf +0xf0 false), so in tactical plans `Budget` is
nil. Methods: AI_GALACTIC "BudgetWrapper" table and AI_GALACTIC_2 §1 (Allocate / Flush_* / Get_* /
Give / Take / Wait_For_*). `_ProduceObject` (§1.9) looks the variable up by name.

### 1.4 `DiscreteDistribution` [H]

The global is a factory object (`LuaDiscreteDistributionClass`, 0x6a3a50) with three members:
- `DiscreteDistribution.Create()` (0x6a39c0): returns a **new** distribution object (id 0x823) with the
  same three members. No args checked.
- `d.Insert(value, weight)` (0x6a3be0): exactly 2 args (error), weight a number (error); value is any
  Lua value (a reference is kept). weight ≤ 0 → ignored. Else appends (value, cumulative weight =
  previous cumulative + weight). Returns nothing.
- `d.Sample()` (0x6a3f80): empty or total ≤ 0 → nothing. Else r = **synced float(0, total)** (0x532480;
  the range arguments are lost in the decompile, [M high] (0, total)); binary search for the first entry
  with cumulative ≥ r (entries with cumulative < r skipped); returns that entry's value. One synced draw
  per call.
- Shipped use: library scripts (`GameRandom`-style weighted picks; AI_TACTICAL §10 lists it as a synced
  draw).

### 1.5 `GlobalValue` [H unless tagged]

A global object (`GlobalValue`, registered by 0x2567b0 into every script with flag +0x122) with:
- `GlobalValue.Get(name)` (0x255880): exactly 1 string arg (errors). Returns the stored value, or
  nothing when the name is unknown.
- `GlobalValue.Set(name, value)` (0x256f80): exactly 2 args, name a string (errors); a **userdata value
  (LuaUserVar: objects, players, types, …) is rejected** with error "cannot be a Lua User Variable".
  Numbers, strings, bools, tables and nil are stored (find-or-insert, 0x254890 / 0x254840). Returns
  nothing.
- Storage: **one process-wide map** (0x140b0a060, string → value) shared by all scripts and modes. It is
  saved and loaded with the game (0x2566e0 from GMM save 0x28c900; 0x2563a0 from GMM load 0x28b080) and
  cleared on game teardown / new game (0x2566d0 from 0x28d940, 0x28f320; MAIN_LOOP §2) [H calls, M
  caller roles]. Map iteration (save) is in the map's own order — keep insertion-independent ordering
  (e.g. sorted by name) in the port and only compare semantics.
- Shipped use: `GalacticAttackAllowed` counters (AI_GALACTIC "Pacing between attacks").

### 1.6 Space-field globals [H]

- `Is_Point_In_Nebula(pos)` 0x6a4d20, `Is_Point_In_Ion_Storm(pos)` 0x6a4b50,
  `Is_Point_In_Asteroid_Field(pos)` 0x6a4980: exactly 1 position-convertible arg (errors are worded
  "Get_Static_Colliders -- …"; a mode without a static-collider set also errors "nor supported in this
  game mode"). Return true iff some static collider whose footprint contains the 2D point (§3.3) has
  `Is_Nebula` (+0x57) / `Is_Ion_Storm` (+0x56) / `Is_Asteroid_Field` (+0x54); the asteroid test ignores
  `Is_Impassable_Asteroid`.
- `Find_Nearest_Space_Field(source [, field_types])` 0x69f060: space mode only (error "may only be used
  in space mode" first); 1 or 2 args (error text says "Expected 1 or 21"); source position-convertible
  (error); arg 2 a string parsed by the SpaceCollisionType bitfield enum (§3.3; unknown → error "unknown
  field type %s"; non-string → error). Default mask 0x1c (Asteroid | Ion_Storm | Nebula). Over **all**
  static colliders of the mode (list kind 8, list order), consider those with (Nebula bit and +0x57) or
  (Asteroid bit and +0x54) or (Ion_Storm bit and +0x56); return the nearest by 2D squared distance from
  the source position (strict <, first wins ties; start 1e18) as an object, else nothing. Shipped:
  `Find_Nearest_Space_Field(tf, "Asteroid")`, `(tf, "Nebula")`, `(Target, space_fields)`.

### 1.7 `SFXManager` [H]

A global `LuaSFXCommandsClassClass` object; calling it (`SFXManager()`, 0x698f80) returns the object
itself. Its members `Allow_Ambient_VO`, `Allow_Enemy_Sighted_VO`, `Allow_HUD_VO`,
`Allow_Localized_SFXEvents`, `Allow_Unit_Reponse_VO` (sic) set the SFX manager permission flags (AUDIO.md
"Permission flags of the SFX manager"). Binding it = registering the existing member set under this
global name.

### 1.8 `WeightedTypeList` [H]

Factory global (`WeightedTypeListClass`, id 0x813, 0x68da30) with:
- `WeightedTypeList.Create()` (0x68dee0): returns a new empty list object (members Create, Parse).
- `l.Parse(types, weights)` (0x68df70): ≥ 2 args (error "Not enough parameters"); both tables (else
  error "Invalid parameters"). Clears the list, then for each index i of `types`: i beyond `weights` →
  error "Types and Weights are mismatched!" and stop; `types[i]` must be a string and `weights[i]` a
  number (else error "Invalid Entry: %d, for Weight list.!", entry skipped). Each entry (0x28 bytes):
  weight, and the name resolved, in order, as a GameObjectCategoryType mask (enum 0x140b31020), else
  through enum 0x140b31068 [M: an AI unit-class enum], else as a type-name list (0x4c5360); nothing found
  → error "Unable to find type or category: %s!". Returns nothing.
- Consumer: `EvaluateTypeList(player, …, list)` (bound) and the AI contrast tables (AI_GALACTIC "Contrast
  types"). No shipped calls of the global.

### 1.9 `_ProduceObject(player, type, planet)` 0x6960e0 [H unless tagged]

- Exactly 3 args (otherwise an error is logged **but execution continues**). player; type as a type
  wrapper or a type name; planet as an object. Any missing → error "Invalid parameter, Player: %s,
  Object: %s, Planet: %s" (Ok / Invalid each), returns nothing.
- Creates a `ProductionBlockStatus` (id 0x805, 0x696830) owned by the script; stores player, type,
  planet, a task id = the global production counter 0x140a44488 & 0x7f, and the script's `Budget`
  variable (§1.3) if present. With a budget: 0x3711c0(type, no planet) (= the type's
  `Build_Cost_Credits` +0x86c) is computed and 0x6bd600 → 0x693170 is called on the goal budget [M:
  reserves the cost from the goal]. Status vf +0x80(0).
- Production check 0x2804d0(player, type, planet object, 0, true, …): the galactic can-produce rules
  (affiliation, cost > 0, build-locked / obsolete lists, tech level, planet ownership, required
  structures, population / base-level limits — GALACTIC_ECONOMY production). Failure → counter += 1,
  error "Scheduling Production Event Failed!", returns nothing.
- Success: builds a Production event (COMMANDS.md command 7; 0x5244e0 with the type's index (+0xf0), the
  planet id (+0x50), the task id), player := the player's id (+0x4c), **executes it directly**
  (0x524130, the AI path of COMMANDS.md), counter += 1, returns the ProductionBlockStatus.

---

## 2. Unbound methods (A)

> **Status 2026-10-30:** The "unbound" title and the 50 count are historical: every method in this section is now bound (coverage log: methods 323, unbound 0; `engine/lua/src/bind_script_remaining.cpp`). The behaviour specs below remain valid.

Ordered by shipped call count. "dead" = the wrapper's object is gone; unless stated the method then
returns nothing without an error.

| Method | Calls | § |
|---|---|---|
| GameObjectWrapper.Get_All_Projectile_Types | 12 | 2.1 |
| GameObjectWrapper.Get_Build_Pad_Contents | 11 | 2.2 |
| GameObjectWrapper.Divert | 10 | 2.3 |
| GameObjectWrapper.Build | 8 | 2.2 |
| TaskForceClass.Garrison | 8 | 2.5 |
| GameObjectWrapper.Is_On_Diversion | 6 | 2.3 |
| TaskForceClass.Can_Garrison / Leave_Garrison | 6 / 6 | 2.5 |
| GameObjectWrapper.Lock_Build_Pad_Contents | 5 | 2.2 |
| GameObjectTypeWrapper.Is_Obsolete | 4 | 2.8 |
| GameObjectWrapper.Get_Rate_Of_Damage_Taken / Get_Time_Till_Dead | 4 / 4 | 2.4 |
| GameObjectWrapper.Enable_Behavior | 3 | 2.6 |
| GameObjectWrapper.Is_In_Ion_Storm / Is_In_Nebula / Is_In_Asteroid_Field | 3 / 3 / 2 | 2.7 |
| LuaScriptWrapper.Is_Valid | 3 | 2.11 |
| TaskForceClass.Set_Single_Ability_Autofire | 3 | 2.5 |
| GameObjectWrapper.Enable_Dynamic_LOD | 2 | 2.6 |
| GameObjectWrapper.Explore_Area (and Land / Space TF) | 2 | 2.3 |
| GameObjectTypeWrapper.Get_Bribe_Cost | 1 | 2.8 |
| GameObjectWrapper.Get_Current_Projectile_Type | 1 | 2.1 |
| GameObjectWrapper.Get_Final_Blow_Player | 1 | 2.4 |
| LandTaskForceClass.Bombing_Run / Fire_Orbital_Bombardment | 1 / 1 | 2.10 |
| PlayerWrapper.Give_Random_Sliceable_Tech | 1 | 2.9 |
| zero-call methods | 0 | 2.12 |

### 2.1 Projectile queries [H]

Both use the object itself, or for a TEAM container (behaviour 0x16) its representative member
(0x4ec820; none → returns nothing).
- `Get_All_Projectile_Types()` 0x577890: returns a table of type wrappers: first, for each hard point i
  of the object (obj +0x2d0 count; record via 0x396cb0) whose hard-point type exists and (is not
  `Is_Destroyable` (+0x4d) or has health +0x28 > 0) and has a projectile (hard-point type +0x210), that
  projectile type; then every entry of the object type's projectile list (`Projectile_Types`, count type
  +0xc48, 0x373670). Order as listed; duplicates kept. Dead → nothing.
- `Get_Current_Projectile_Type()` 0x578470: index obj +0x324 (current weapon / projectile index, changed
  by weapon switching) into the type's projectile list; in range → that type wrapper, else nothing.
- Shipped: MissileShield / PointDefense object scripts and PGEvents (`attacker.Get_Current_Projectile_
  Type()` fed to `Is_Affected_By_Laser_Defense` etc.).

### 2.2 Build pads (Lua side; rules in §3.6) [H]

- `Get_Build_Pad_Contents()` 0x577e20: dead → **error** "this object is already dead", nothing. Without
  TACTICAL_BUILD_OBJECTS → **warning** "object %s is not a build pad", nothing. Contents = the built
  object (+0x10), else the UC object (+8); none → nothing. A dying content (+0x3a0 & 2) yields a wrapper
  bound to no object (a dead wrapper). The wrapper comes from the per-script wrapper cache (keyed by
  (object, script), 0x140b39380) so repeated calls return the same wrapper; its +0x70 flag := 1.
- `Lock_Build_Pad_Contents(bool)` 0x57cbe0: exactly 1 bool (errors). Reads the pad pack (obj +0x150)
  directly: none → error "%s is not a build pad"; empty pad (+8 and +0x10 null) → warning "cannot lock an
  empty build pad"; else lock byte +0x2c := bool. Effect: the built object cannot be sold; cleared when
  it dies (§3.6). Returns nothing. Shipped: story missions lock pre-built turrets.
- `Build(type [, check_rules])` 0x5739c0 (on the **pad** object): 1 or 2 args (error). Arg 1 a type
  wrapper or type name (unknown name → error "unknown game object type %s"; neither → error). The object
  must have TACTICAL_BUILD_OBJECTS (error "not a valid build location") and be empty (error "already has
  something built on it"). With arg 2: must be a bool (error); when **true**, the type must be in the
  pad's buildable list for the **pad owner's** faction (0x372c10 / 0x374420, §3.6) **and** the build
  rule 0x42e400 must pass; otherwise **warning** "game rules prevent building %s at %s" and returns
  **false**. Then start build 0x42dbd0(behaviour, pad, type, pad owner id, charge = 0, sfx = 0): failure
  → error "unknown error starting build…", nothing; success → returns **true**. So `Build` without
  `true` ignores faction lists, enemy proximity and credits (no charge, no SFX); cooldown and friendliness
  (0x39a2c0) are still enforced by 0x42dbd0. Shipped: `pad.Build("UC_…")`, `mdu.Build(name)`.

### 2.3 Diversion and exploration [H unless tagged]

- `Divert(pos [, number])` 0x575710: no locomotor (slot 1) → error "this object cannot move", nothing.
  The locomotor's formation (locomotor pack +0x1f0) null → **warning** "unable to divert - object %s is
  between moves", returns **false**. 1 or 2 args (error). Movement locked (0x39ad40: lock counter obj
  +0x3a8 > 0, or locomotor vf +0x1d0, or a locked team container) → warning "has had its movement
  locked", **false**. Arg 1 position-convertible (error); arg 2 a number (error) stored in move-params
  +0x54 (default 1e18; MOVEMENT §6.1 table) [M: a distance limit]. Builds a move block (MOVEMENT §6.1):
  destination = pos, type 1 (position), avoidance mask 0x3f, **flags 0x40 (diversion)**, +0x5c..+0x5f =
  1,0,0,1. Then 0x424850 (formation's coordinator detaches the unit into its own formation, 0x4f84e0) and
  0x41fd30 (formation Set_Move: abort if any member is locked; members with ability 5 active get it
  cancelled (0x38a350); pops queued moves per the waypoint rules; starts the move). Returns **true**.
- `Is_On_Diversion()` 0x57c1f0: no locomotor → error, nothing; no formation → false; else the flags of the
  formation's **current (last) move** (formation +0xf8 list of 0x68-byte moves, 0x41af10) bit 0x40.
  So it is true until that diversion move is replaced or finished [M: the AI event
  `Unit_Diversion_Finished` (AI_GALACTIC_2 event table) marks the end].
- Shipped pattern (PGEvents / PGCommands): `unit.Divert(kite_pos)` then poll `Is_On_Diversion()`.
- `Explore_Area(ai_target)` on an object 0x576000, `LandTaskForceClass` 0x6eaba0, `SpaceTaskForceClass`
  0x6e66a0 (identical bodies): exactly 1 AI target (errors; dead target → error "target is dead").
  Returns an `ExploreAreaBlockStatus` (id 0x82b, 0x6afec0): +0x48 target, +0x54 start frame. Init
  0x6b0320: unit list = the task force's units (TF) or the object. Start 0x6b06f0: owner player of the
  first unit; the exploration helper 0x140b31900 picks a point near the target (0x6558b0 for a target
  position / 0x655be0 for a target object, with the player and start frame) [M: least-recently-seen FOW
  area]; a valid point (≠ 1e18) → a new movement coordinator (mode vf +0x228 → 0x426010), every unit
  leaves its coordinator and joins, Move_To(point) (0x4f8200; type 1, mask 0x3f); +0x50 := 0. No point
  → +0x51 := 1 (finished). [M: status service re-picks points; IsFinished = +0x51.] Shipped: LandScout /
  SpaceScout `BlockOnCommand(MainForce.Explore_Area(AITarget), 10)`.

### 2.4 Damage statistics [H]

`DAMAGE_TRACKING` (0x3e) pack (obj +0x1a0): +8 damage accumulated since the last service, +0xc **rate**
= FPS × (+8) / service interval (frames), recomputed and +8 reset at each service; the per-attacker map
decays (AI_TACTICAL §6.3).
- `Get_Rate_Of_Damage_Taken()` 0x579820: dead → **warning**, returns 0. Non-team: the tracker rate (0 if
  no tracker). TEAM container: Σ over members with a tracker. Returns a number (damage per second).
- `Get_Time_Till_Dead()` 0x579bf0: dead → warning, returns 0. Non-team: tracker 0x58ba40 = (hull + shield
  (0x396420)) / rate, where hull = Σ max(0, health) of hard points that are `Is_Destroyable` when there
  is at least one, else obj health +0x5c; no tracker or rate 0 → 1e18. TEAM: Σ member health +0x5c / Σ
  member rates (members with a tracker); Σ rate 0 → 1e18; a team object without its TEAM behaviour →
  nothing. Returns seconds.
- `Get_Final_Blow_Player()` 0x5787f0: obj **+0x330 = final-blow player id** (byte; −1 = none). It is set
  by the damage / death path (0x39bdb0 with the attacker's player, also copied to the team container;
  0x3a01f0 at death: the killer's owner, else inherited from the container), by planet owner changes
  (0x4bb4a0 with the conquering player — `GameScoring` calls `planet.Get_Final_Blow_Player()`), by
  0x7112f0, and copied to the parent-mode counterpart by 0x2b2cc0 (resolves OBJECT_RUNTIME §2.5 "byte
  +0x330"). Returns the player with that id, or the **neutral player** (0x294aa0) when −1; nothing if
  the player does not exist.

### 2.5 Task-force garrison and autofire [H]

All iterate the TF unit list (+0x78) in order and call the object-level behaviour (§3.5,
ABILITIES_HARDPOINTS autofire); per-unit errors are reported per unit.
- `Garrison(host)` 0x5fc440 → `GameObjectWrapper.Garrison` for each unit. Returns nothing.
- `Leave_Garrison()` 0x5fd3e0 → per unit 0x57c880. Returns nothing.
- `Set_All_Abilities_Autofire(bool)` 0x5fff40 / `Set_Single_Ability_Autofire(name, bool)` 0x5fffd0 → per
  unit 0x57ff10 / 0x580750 (ability name via UnitAbilityType enum 0x140b310c0; the object version loops
  ability slots 0..0x4c it has (0x3986b0) and sets autofire 0x3a78b0). Returns nothing.
- `Can_Garrison(host)` 0x5fbcb0: exactly 1 arg, object or AI target with object (error; dead → error
  "target is already dead", nothing). False when the host has no host behaviour, or the host owner
  exists, is not allied with the TF's player (0x5f7020) and its faction byte +0x108 is 0. Otherwise true
  iff **some** unit is alive, not garrisoned (+0x3a7), has GARRISON_UNIT, masks match (0x38f9a0) and
  free (0x3954b0) − reserved (host +0x210 → +0x34) − its `Garrison_Value` > 0 (the stricter Lua rule,
  §3.5). Returns a bool.
- The object-level `Can_Garrison` (bound) uses the same capacity rule.

### 2.6 Behaviour and LOD switches [H]

- `Enable_Behavior(id, bool)` 0x575a20: exactly 2 args; id a number (error) with 0 ≤ id ≤ 0x69 (error
  "invalid behavior"); arg 2 bool (error). The object's behaviour of type id (OBJECT_RUNTIME §4.3), if
  any: true → vf +0xb8 (resume), false → vf +0xc0 (suspend) (0x393570). Missing behaviour → silently
  nothing. Returns nothing. Shipped: `Enable_Behavior(78, …)` (0x4E HOLSTER_WEAPON) and `(24, true)`
  (0x18 REVEAL).
- `Enable_Dynamic_LOD(bool)` 0x575b80: exactly 1 arg (error "command takes a single boolean value"), bool
  (error). Model (obj +0x2a0) render object +0x48, if of the LOD-capable class, byte +0x198 := bool
  (0x264690). Presentation only. Returns nothing.

### 2.7 Field membership of an object [H]

`Is_In_Nebula()` 0x57c110 (→ 0x39aa60), `Is_In_Ion_Storm()` 0x57c030 (→ 0x39a840),
`Is_In_Asteroid_Field()` 0x57be30 (→ 0x39a620): dead → nothing; else a bool, decided in order:
(1) a TEAM container is "in" if any member is (recursive); (2) an object **without** the matching
victim behaviour (NEBULA 0x48 / SHIELDED 0x10 / ASTEROID_FIELD_DAMAGE 0x2E) gets the geometric point test
at its 2D position (§1.6); (3) with the behaviour and a combatant pack (+0xf0) it answers from the
behaviour's time stamp (§3.3): nebula = lockout active (true up to 5 s after leaving), ion storm =
stamp within `Ion_Storm_Shield_Disable_Time`, asteroid = stamp ≥ 0.

### 2.8 Type queries [H]

- `GameObjectTypeWrapper.Is_Obsolete(player)` 0x606130: exactly 1 player (errors). True iff the type is
  in the player's **obsolete list** (player +0x1c8, count +0x1d0; 0x282550). Shipped: PGInterventions.
- `GameObjectTypeWrapper.Get_Bribe_Cost([briber])` 0x6053e0: base = `Tactical_Bribe_Cost` (type
  +0x2358, int). With no args, or when there is no **battle planet in the parent galactic mode**
  (0x28a890: current mode tactical, parent mode galactic, battle planet id → planet object with PLANET),
  returns base. Else arg 1 must be an object (error), alive (error "bribing object is dead"); planet
  without planetary data → nothing; factor = briber type's `Bribe_Modifier_On_Uncorrupted_Planets`
  (+0x23e0) when planet data +0x2f4 == −1 [M: "no corruption"], else `…_On_Corrupted_Planets` (+0x23e4);
  returns base × factor. Shipped: Tyber AI `Get_Type().Get_Bribe_Cost(tyber)`.

### 2.9 `PlayerWrapper.Give_Random_Sliceable_Tech()` 0x6031f0 [H unless tagged]

- Candidates: every object type (type manager order, 0x408150 / 0x408130) that is affiliated with the
  player's faction (0x3751a0, `Affiliation` list type +0xef0), has `Tech_Level` (+0x89c) ≤ the player's
  tech level (+0x84), `Build_Initially_Locked` (+0xc1), is **not** already in the player's unlocked list
  (+0x1b0, 0x282580), has `Build_Can_Be_Unlocked_By_Slicer` (+0xc2), and weight w = 1 /
  `Slice_Cost_Credits` (+0x868, float) > 0. Cumulative weights in that order.
- No candidate: if the player's tech level is 0, raise it (0x282190: +1 when below the max +0x88) and
  **retry**; else returns nothing.
- Else (total > 0): r = synced float(0, total) [M high on the range], binary search the first cumulative
  ≥ r. If there was exactly **one** candidate, the player's tech level is raised (0x282190) instead of
  unlocking; otherwise the type is appended to the unlocked list (0x27f860, no duplicates). Returns the
  chosen type wrapper in both cases. One synced draw.
- Shipped: TechPowerUp object script.
- The R2D2 slicer, the tech-level sweep and every other unlock source: REMAINING_GAPS §1.

### 2.10 Land task-force bombing and bombardment [H]

- `LandTaskForceClass.Bombing_Run(target)` 0x6e9d90: exactly 1 arg, object or AI target (error; dead →
  **warning** "target is already dead", nothing). Player = TF player; position = the object's position
  or the AI target's (vf +0x68). Target invalid (0x494540, §3.7) → warning "Target position is within an
  invalid area. Aborting.", nothing; not available (0x494410) → warning "No bombing run available…",
  nothing. Else start (0x492d60) and return a `BombingBlockStatus` (id 0x81c, 0x73db90) over the
  record's bombers: `IsFinished` = no bomber left alive; `Result` = undamaged / total (fraction not shot
  down; 0/0 with no bombers). Shipped: LandBombing `BlockOnCommand(MainForce.Bombing_Run(AITarget))`.
- `LandTaskForceClass.Fire_Orbital_Bombardment(pos)` 0x6eb730: exactly 1 position-convertible arg
  (errors); if the land mode has a bombard manager (+0x40), 0x528140(manager, TF player id, pos) (§3.8).
  **Returns nothing**; every failure is silent.

### 2.11 `LuaScriptWrapper` members [H]

Wrapper of another script (+0x38 = its LuaScriptClass; creator not identified, 0 shipped uses besides
`Is_Valid`). All return nothing when the script pointer is null.
- `Is_Valid()` 0x258770: bool (script pointer non-null). Shared by StoryEvent / StoryPlot /
  AITargetLocation wrappers (same body).
- `Call_Function(name, ...)` 0x258040: ≥ 1 arg, name a string (errors). The remaining args are passed as
  the parameter table; the named global of the wrapped script, if it is a Lua function, is called
  (0x243640) and its result table is returned (all results); otherwise nothing.
- `Get_Variable(name)` 0x258450: exactly 1 string (errors); returns the wrapped script's global, or
  nothing.
- `Set_Variable(name, value)` 0x258bd0: exactly 2 args, name a string (errors); registers value as that
  script's global (0x247000). Returns nothing.

### 2.12 Zero-call methods [H unless tagged]

- `FindTargetClass.Best_Of(tf, targets, perception_name)` 0x697450: exactly 3 args: a TF (error), a table
  (error), a string naming a perception function (error "unrecognized perception function"). For each
  table entry in order: object or AI target (other → warning, skipped; dead → warning, skipped) is
  evaluated with a perception context holding the TF player (+0x478 AI id), the TF context's enemy player
  (0x5f55c0, if any), the player from 0x294a00 [M], and the target (planet → planet data +0x354; other
  objects → name 0x4dc3a0; AI target → vf +0x28). Returns the entry with the **highest value > 0**
  (strict; first wins ties), else nothing.
- `GalacticTaskForceClass.Refit_To_Definition(planet, n)` 0x6cfbe0 [M]: only for TF definitions that
  specify units by percentages (definition +0xb4; error otherwise); exactly 2 args (planet object,
  number; errors — the second message says "Instantiate_Force"). Creates a block status (id 0x80b,
  0x6c6500). n = 0 → returns it unstarted. Else per definition category it counts existing units and
  their costs, computes the share of categories with buildable types, and with n > 0 schedules builds
  (0x6cd950 / 0x6cdf60), n ≤ 0 releases surplus (0x6d0680 / 0x6d1100; with no units 0x691930(−n)).
  Returns the status.
- `GameObjectWrapper.Force_Test_Space_Conflict()` 0x576db0: only fleets (FLEET 0x4) or planets (PLANET
  0x3), error otherwise. Planet → its first orbiting fleet (planet data +0xc8 list); fleet → its parent
  planet. Re-runs the fleet's arrival at the planet: leave (0x4358c0), arrive (0x4338d0(…, −2, false):
  conflict test against enemy fleets / starbase). Conflict → pull in nearby attacker / defender fleets
  (0x569700), `Initiate_Space_Conflict` (0x2af7c0(GMM, planet, fleet owner, −1, 0, 0)), return a
  `GenericSignalBlockStatus` (id 0x827) waiting for signal 0x12 on the planet. No conflict → nothing.
- `GameObjectWrapper.Get_AI_Power_Vs_Unit(target)` 0x577230 [M on 0x5f1f90]: 1 arg object or AI target
  with object (errors; dead → error). Types: object type (or 0x396380 representative when the object
  lacks TRANSPORT). Result = P_s × H_s × M_s − P_t × H_t × M_t, P = AI combat power (0x370e60), H = health
  fraction (0x396df0), M_s = max over the target type's category bits of the AI power-versus weight of
  the self type (0x5f1f90, a WeightedTypeList-backed table), M_t likewise reversed.
  `TaskForceClass.Get_AI_Power_Vs_Unit(target)` 0x5fc4d0: tactical only (error); Σ over TF units of
  P_u × max_c(weight(u, c)) using the TF context's table, minus P_target (no health terms).
- `GameObjectWrapper.Get_Affiliated_Indigenous_Type(player)` 0x577600: planets only (error); exactly 1
  player (errors). Over the planet type's `Potential_Indigenous_Power` entries (type +0x20c8, 0x18-byte
  entries: type +8, power int +0x10) in order, keep the entry with the highest power (strict >) whose
  type is affiliated with the faction of some player allied with the argument (0x2823e0 relation 0,
  0x3751a0); returns that type wrapper or nothing.
- `GameObjectWrapper.Get_Energy()` 0x578660: tactical only (error); dead → 0. Else the energy fraction
  0x396560: needs POWERED (0x0f) and a combatant pack; clamp(CD +0xf8 / max energy (0x372320), 0, 1);
  0 when max ≤ 0 (TACTICAL_COMBAT §3.7).
- `GameObjectWrapper.Release()` 0x57f2a0: unsubscribes the wrapper from the object's death signal 0x10,
  removes it from the wrapper cache (0x140b39380, keyed (script, object)), drops the object reference
  (wrapper becomes dead). Returns nothing.
- `GameObjectWrapper.Set_Prefer_Ground_Over_Space(bool)` 0x57d460: needs ≥ 1 arg (error "Missing required
  boolean"); needs UNIT_AI (0x12) (error "Object invalid or missing Unit_AI behavior"); AI pack (+0x108)
  byte +0x39 := bool (0x4f6460) [M: galactic AI deploy preference]. A non-bool arg dereferences null in
  the original; port: treat as the missing-argument error.
- `GameObjectWrapper.Set_Targeting_Stickiness_Time_Threshold(seconds)` 0x580ac0: dead → warning; exactly
  1 number (errors). TARGETING (9) override CD +0x158 := seconds (0x568df0) on the object and on every
  TEAM member; read by `Is_Target_Sticky` (TACTICAL_COMBAT §2.5).
  `TaskForceClass.Set_Targeting_Stickiness_Time_Threshold(seconds [, filter])` 0x6003a0: 1 or 2 args;
  arg 1 number (the error text wrongly says "string"); arg 2 a GameObjectCategoryType name (enum
  0x140b31020) or a type name (error if neither). For each unit with UNIT_AI (slot +0x344) whose type
  (representative / company / squad member type: type +0x980 → +0x978[0], +0x960 → +0x958[0]) matches
  the category mask (default all) and the type (if given): set the override on it and its team members.
- `TaskForceClass.Add_Force(object)` 0x5f9a90 (virtual; overrides Galactic 0x6cd1a0, Land 0x6e9240,
  Space 0x6e4e30): base subscribes the TF to the object's signals 0x1a, 0x10, 0x1e, 0x1f, 0x20, 0x21,
  0x2b, 0x2c, 0x29, 0x2d and appends it to the unit list (+0x78; duplicates not checked); non-object →
  error. Galactic: base, then signal 0x1b; if the TF has no fleet (+0x120) and the object's parent is a
  FLEET, adopt it (subscribe to its 0x10 and 0x1b). Land (≥ 1 arg): signals 0x18 and 0x2a on the object
  and 0x2a on each TEAM member, then base. Space: signals 0x18, 0x2a, UNIT_AI 0x4f6450(ai, 0) [M], then
  base. Returns nothing.
- `TaskForceClass.Get_Type_Of_Unit(i)` 0x5fcfe0: i a number (error); returns element int(i) of the TF's
  +0x60 vector [M: the definition's unit types] if in range and non-null, else nothing (negative i is
  unchecked in the original; port: nothing).
- `TaskForceClass.Add_ / Remove_ / Clear_Opportunity_Fire_Event_Subscription(s)` 0x5f9dc0 / 0x5ff430 /
  0x5fc010: arg = a hard-point type name (HardPointType enum 0x140b310a0; unknown → error); only weapon
  hard-point types (values 5..10) are accepted (error "not a weapon hard point type"). The TF keeps an
  int list (+0x108, saved as micro-chunk 0xd); Add appends (duplicate → warning), Remove erases
  (missing → warning, worded "Add_…"), Clear empties. [M: the TF's signal handler raises the Lua event
  `Hardpoint_Opportunity_Target_Acquired` only for subscribed hard-point types; consumer not traced.]
- `LandTaskForceClass.Find_Closest_Enemy([category])` 0x6ead30 / `SpaceTaskForceClass` 0x6e68a0: 0 or 1
  arg (error says "1 or 2"); arg a GameObjectCategoryType name (unknown → error) setting a global mask
  (default all bits). Store query 0x2a8c40 around the TF position (vf +0x170), kind 2 (land) / 0x12
  (space), enemies of the TF player, filter (0x6ea9c0 / 0x6e65e0): type `Is_Valid_Target` (+0x6b), not
  fogged for the player (0x35f470 force 1 false) and category ∩ mask ≠ 0. Returns the nearest as an
  object, else nothing.
- `PlayerWrapper.Get_GameSpy_Stats_Player_ID()` 0x602ae0: player +0x398 as a number; no player → −1.

---

## 3. Backing systems (B)

### 3.1 Already specified elsewhere (no new work)

| System | Where |
|---|---|
| Tactical special weapons (readiness, begin-targeting toggle, valid targets, upgrade gate `SpecialWeaponsAllowed`), lobbing superweapons, space-station hard-point weapons, task-force variants, `Spawn_Special_Weapon` | SCRIPT_GAPS §1.1–§1.6 |
| Tactical superweapon (TSW) state machine 0..10, readiness = state 4, firing, planet destruction | SCRIPT_GAPS §1.2; network command 38 in COMMANDS.md |
| Timed engine disables (`Locomotor_Set_Engines`, permanent on engine hard-point loss, timed re-enable frame) | SCRIPT_GAPS §2.2; TACTICAL_COMBAT §3 "System offline" |
| Cinematic space retreat line-up | SCRIPT_GAPS §3.2 (timing completed in §3.4 below) |
| Battle retreat coordinator, write-back to galactic | BATTLE_TRANSITIONS §3.3–§3.4 |
| Ability autofire (`Set_*_Autofire` per object) | ABILITIES_HARDPOINTS (autofire section); task-force forms in §2 below |
| AI tactical building through task forces (`Build`, `Build_All`, reservations) | AI_TACTICAL §7.5 |

### 3.2 Tactical weather (land only) [H unless tagged]

**Data.** `WeatherScenarios.xml`: `Weather_Scenario name=…` with `emitter_name` (upper-cased, scenario
+0x20) and `randomize_phase_selection` (bool, +0x40; parsed but never read by 0x468a50 / 0x4690c0, so
phases always run in file order [H for those two, M that nothing else reads it]). Child `Phase` entries
(vector +0x48), each: +0x0 `Emitter_Intensity`, +0x4 `Lightning_Intensity`, +0x8
`Fog_Plane_Interpolation_Alpha`, +0xc weather type, +0x10 `Duration`, +0x14 `Ease_Out_Duration` (int
seconds). Weather type from the phase name (`LandModeClass::tLandWeather`, 0x6b6e90): CLEAR 0, RAIN 1,
SNOW 2, SANDSTORM 3, ASH 4; other names log "Invalid Weather Phase Name … defaulting to CLEAR".
`WeatherModifiers.xml` (0x469480) fills one 0xd0-byte block per weather type (system + type × 0xd0):
+0x10 `Weapon_Accuracy_Modifier` map by projectile category (DEFAULT 0, MISSILE 1, BOMB 2, GRENADE 3,
ROCKET 4, STATIC_BOMB 5, MPTL_ROCKET 6, LASER 7, LIGHT_SABER 8, CLUSTER_BOMB 9), +0x20 `Speed_Modifier`
and +0x30 `Sight_Range_Modifier` maps by `Weather_Category`, +0x40…+0xc0 five UI strings [M names];
their getters (0x468860..0x468940) return the CLEAR block while intensity ≤ 0.5.

**Scenario choice.** The weather system (global 0x140b30b40) reads the map environment (system +0x480,
set by 0x46a2f0 when land mode is created, 0x3b81b0): records 0x198 bytes apart, active index env
+0xa0, scenario name at record +0x148 (0x273440), looked up in the WeatherScenario database
(0x140a2a980, 0x489580). **Init / re-init** 0x468a50(sys, force) runs every service: nothing when the
environment index and name are unchanged and force = 0; a changed environment (`Set_New_Environment`,
0x360970 → 0x46a2f0) restarts the weather; a missing scenario or one with 0 phases turns weather off
(weather audio → CLEAR via 0x323110(0, 1.0), emitter removed). Otherwise: phase index +0x438 := 0,
current +0x428 := phase 0, next +0x430 := phase 1 (phase 0 if only one), weather type +0x43c := phase
type, intensity +0x448 := `Emitter_Intensity`, transition flag +0x454 := 0, weather audio set, then
**start phase** (0x468980): phase timer +0x44c (start frame, length) := synced float(0.3 × Duration,
Duration) × FPS frames; environment lightning (record +0x138) := `Lightning_Intensity`; fog-plane alpha
(record +0xfc, when the record has a fog plane +0xf8) := `Fog_Plane_Interpolation_Alpha`; emitter
intensity := `Emitter_Intensity`. The `emitter_name` particle is added to the scene.

**Service** (0x46a0c0, from land-mode service 0x3bb440 when land mode +0x328 ≠ 0):
1. Return unless re-init (force 0) reports a scenario and a current phase.
2. While transitioning, t = min(1, (now − start) / length): lightning, emitter intensity I and fog alpha
   = lerp(current, next); I also goes to weather audio (0x323340) and into +0x448. When I crosses 0.5
   either way, every REVEAL (0x18) object of the store is marked dirty (+0x68 := 1, 0x2b3a30; only when
   store flag +0x63e is set and 0x140b15490 is clear) so sight radii are recomputed.
3. Timer expired: not transitioning → transition flag := 1 and timer := synced float(0.3 ×
   Ease_Out_Duration, Ease_Out_Duration) s; transitioning → **advance**.
4. **Advance** (0x4690c0): index := index + 1 wrapping to 0; current = phase[index], next =
   phase[index + 1 wrapping]; type := current type; +0x448 := its `Emitter_Intensity`; transition := 0;
   audio refreshed; start phase (one synced draw); if the type changed, the FOW-dirty pass of step 2.

**`Force_Weather()`** (0x542370 → 0x468500 → 0x4690c0): acts only when the current mode type is 1
(land); arguments are ignored; returns nothing. It performs exactly **one advance**: an immediate jump
(no ease-out blend) to the next phase in file order, re-randomising its duration (one synced draw). It
cannot choose a weather type; a one-phase scenario restarts its phase. Shipped use: 1 story call.

**Game-logic effects.** Every modifier is 1.0 unless intensity +0x448 > 0.5 **and** the object's owner is
weather-affected (0x282290(player): true when the player's timed bool list +0x410 is empty after expiry,
else that list AND-combined with clamps +0x420..+0x423 [M: a weather-immunity modifier]).
- Sight (0x468510): sight map of the current type by `Weather_Category` (−1 → 0), fallback "Default",
  missing → 1.0; multiplies `Land_FOW_Reveal_Range` (FOG_OF_WAR §3.1.1).
- Accuracy (0x468750, land only): accuracy map by projectile category (out of range → 0); inaccuracy /
  spread are scaled by (2 − m) (applied in 0x3747c0, 0x53ff30 to per-category inaccuracy lists +0x508 /
  +0x3e0 and in 0x374890 to projectile type +0x1ff8). XML comment "0.4 → ×1.6" matches.
- Speed (0x468630): 0x4098d0 (land, from 0x3639d0) builds a table over all `ObjectWeatherCategoryType`
  values at manager +0x90 (owner not tested; ≤ 0.0001 → 1.0). Reader not traced [M]; every shipped
  `Speed_Modifier` is 1.0, so no vanilla effect.
- AI helper 0x2f9950 (via 0x31de70): true when the accuracy modifier for the unit's category 4, 6 or 7
  (or behaviour 0xb) is below GC 0x140b16bbc, or the sight modifier (category 0) is below 0x140b16bc0
  [M: "weather is bad for this unit"].
- Presentation only: emitter, lightning, fog-plane alpha (env +0xfc), weather audio
  (`WeatherAudioManagerClass` 0x140b2b3b0; its intermittent sounds use the **free** RNG 0x140a13e20).

**Save / determinism.** Land-mode chunk 0x14 (save 0x469ff0, load 0x468f80): micro 0 phase index, 1 phase
timer, 2 scenario name. Load: re-init with force = 1, index := saved − 1, advance (one synced draw;
transition flag reset), then overwrite the timer with the saved one — an ease-out in progress is **not**
restored. Sim state: index, timer, type, intensity +0x448; synced draws at every phase start and every
ease-out start.

### 3.3 Space fields (nebula, ion storm, asteroid field) [H unless tagged]

**Field objects.** Space props with type flags `Is_Asteroid_Field` (+0x54), `Is_Impassable_Asteroid`
(+0x55), `Is_Ion_Storm` (+0x56), `Is_Nebula` (+0x57) and a SPACE_OBSTACLE (0x4c) behaviour, held in the
space mode's static-collider set. Footprint = MOVEMENT circle (position + `Space_Obstacle_Offset`,
radius 0x373b50).

**Point membership** (`Get_Static_Colliders`, 0x6a4ef0): every object of the static-collider list
(0x537e60(…, kind 8)) whose footprint (0x38de80) contains the 2D point (0x20da80), in list order.
**Object membership** (behaviours): grid query with the unit's position, yaw and box (0x5d7c00 on
0x5387f0(mode, 8)), then the hit's type flag.

**SpaceCollisionType** (bitfield enum, 0x5eacd0; map 0x140b310e8): None 0, Moving 1, Static 2,
Asteroid 4, Ion_Storm 8, Nebula 0x10, All 0x3f. Combined strings ("Nebula | Ion_Storm") are a
bitfield-converter feature [M].

**Nebula** (`NebulaBehaviorClass` vf6 0x437b60, behaviour 0x48 on the affected ship): smooths a visual
value CD +0x11c toward target CD +0x120 (rate CD +0x12c) [M: tint fade]. While the lockout is active
(0x380bb0) it only snaps the value to 1. Otherwise, if the locomotor is not busy (vf +0x2b0), it queries
the grid: on a nebula hit — if stamp CD +0x104 was −1 (entry) it **cancels every active ability** of set
0x140b2f2e0 (0x4376f0: ability slots 0..76 owned and active get deactivated + signal 0x2b), target value
1 — then stamp := NOW; with no hit and a set stamp: stamp := −1, 0x39fb40(obj) [M: restore], target 0.
Lockout 0x380bb0: stamp ≥ 0 and (NOW − stamp) / FPS < `Nebula_Ability_Disable_Time` (GC +0x1181c,
5.0). NOW here is the **global logic frame 0x140b0a320**, not the mode frame. Since the stamp is only
refreshed after the lockout lapses, "in nebula" stays true for up to 5 s after leaving, and every
ability gate that calls 0x380bb0 refuses meanwhile (ABILITIES_HARDPOINTS §2.4 gate 2; 0x38a350,
0x3a1930, 0x3a3260). AI target pickers (0x387400, 0x568a00, 0x633ae0) treat in-nebula objects
specially unless FOW-visible (0x35f3b0) [M].

**Ion storm** (SHIELDED behaviour service 0x5495e0, skipped when obj +0x3a2 & 4): after regeneration
(0x549250) query the grid; on a +0x56 hit CD +0x100 := NOW (refreshed every service while inside), else
−1. While stamped and within `Ion_Storm_Shield_Disable_Time` (GC +0x11818, 5.0; 0x380b70) shields neither
absorb (0x549ac0) nor count as up (0x549cd0), 0x41ca0 treats the target as unshielded, and DEFEND is
blocked (ABILITIES_HARDPOINTS §2.4) [M for each caller's role]. Objects without SHIELDED are never
affected.

**Asteroid field** (`AsteroidFieldDamageBehaviorClass` vf6 0x437310, behaviour 0x2E): acts only when the
space layer (0x397900) is Capital 1, Frigate 2 or SuperCapital 0x20 (not corvettes / fighters), the
locomotor is moving (vf +0x1a8 true, +0x1b8 false, +0x2b0 false), and GC `Asteroid_Field_Damage`
(GC +0x1560) and `Asteroid_Field_Damage_Rate` (+0x1564) are both non-zero. For **each** overlapping
asteroid field (+0x54) in grid-result order: synced float(0, 1) ≤ Rate → hit (0x436920); CD +0x10c :=
NOW. No overlap → CD +0x10c := −1. A hit plays `SFXEvent_Damaged_By_Asteroid` (type +0x13d0), calls
Take_Damage (0x3a9e30) [M: amount = `Asteroid_Field_Damage`; damage type lost in the decompile], and
spawns one `Asteroid_Damage_Hit_Particles` entry (type +0xc98 list, count +0xca8) chosen with synced
int, placed at a random bone (another synced draw) or the model centre + 90 z. The particle is visual,
but **both draws consume the synced RNG**.

Lua queries over this: §1.6 (`Is_Point_In_*`, `Find_Nearest_Space_Field`) and §2.7 (`Is_In_*`).

### 3.4 Hyperspace out / in and teleport (space) [H unless tagged]

**Implementers** (vtables Starship 0x8ae250, Fighter 0x8a6198, SimpleSpace 0x8aeaf8): vf +0x2a8
hyperspace out = 0x5caf40 for Fighter and SimpleSpace, **empty stub for STARSHIP_LOCOMOTOR**; vf +0x2b8
"can hyperspace in" true / false; vf +0x2c0 cinematic hyperspace in 0x5c9a80 (none for Starship); vf
+0x2d0 cancel 0x5c9800 (stub for Starship); vf +0x2b0 busy = locomotor state (pack +0x48) ∈ {4, 5,
0x28, 0x2b, 0x2c} (0x42c640). So a `STARSHIP_LOCOMOTOR` object ignores `Hyperspace_Away`,
`Cinematic_Hyperspace_In` and `Cancel_Hyperspace`.

**Hyperspace out** +0x2a8(keep_parent_link, delay) (0x5caf40): stop the locomotor (vf +0x138(0), vf
+0x200); state := 0x28; timer (pack +0x5c) := delay + 90; if keep_parent_link is false, obj +0x54 :=
0x3FFFFF (parent-mode id cleared). State 0x28 each frame (0x5cd680): non-forced teleport step
(0x3a8f90) along the yaw (obj +0x98) — 8.125 per frame while timer > 90; at timer == 90 play
`SFXEvent_Retreat_Engines_Jump` (type +0x1270) or else the faction's `SFXEvent_Exit_Into_Hyperspace`
(+0x6c0) (0x2d5240) and start the model stretch effect (0x265ae0); from 90 down the step is the owner
faction's `Space_Retreat_Flight_Move_Increment` (+0x20bc); at 0 the object is **killed** (0x391310, flag
1). Total delay + 90 frames, **no RNG**. With the parent link kept the galactic counterpart is
death-marked (OBJECT_RUNTIME §2.5); with it cleared it survives [H code, M consequence].
- `Hyperspace_Away([bool = true])` (0x57a9d0, space only, 0 or 1 args, error without locomotor) =
  +0x2a8(bool, 0); returns nothing. `(false)` keeps the galactic unit alive.
- `Start_Cinematic_Space_Retreat` (SCRIPT_GAPS §3.2) passes keep = false and per-unit delays, so each
  unit is removed `delay + 90` frames after the call and survives in galactic (closes SCRIPT_GAPS open
  item 4).
- `Cancel_Hyperspace()` (0x574b90 → +0x2d0, 0x5c9800) acts only on **arrivals** (state 4, 5, 0x2c) of an
  enabled locomotor: teleport to the arrival destination (pack +0x2c..+0x34), clear pack +0x2b2, state 0,
  restore orientation, show model, clear obj +0x3a2 bit 0, release the movement lock (obj +0x3a8,
  0x3a8e50(…, 0)), vf +0x208, vf +0x138(0), reset arrival min/max globals (0x140b27bbc := FLT_MAX,
  0x140b27bc0 := 0), raise **signal 0x19** (arrival complete), leave the coordinator and stop. A
  departure (0x28) cannot be cancelled.

**Battle retreat** does **not** use +0x2a8: the space retreat coordinator's jump-out (0x533ac0) moves the
retreat list (+0x38, count +0x40) itself with teleport steps along the retreat direction, phased by the
hyperspace-sequence counter 0x140b27b64 (sequence 0x140b27a10; done past 0x59) [M: counter driven by
the retreat cinematic]; units are preserved and written back (BATTLE_TRANSITIONS §3.3–§3.4).

**Hyperspace in.** Reinforcement arrival (states 5 → 4): state 5 (0x627590) sets destination := current
position, gets start point and velocity from vf +0x2a0, teleports to the start, movement lock +1, stores
velocity (pack +0x14..+0x1c), hides the model (unless sync-test playback 0x140b1574e), widens the
arrival min/max globals from the store's hyperspace point (store +0x628..+0x630) and `IHyperD`
(0x140b27b7c), timer := −1, state 4. State 4 first frame: f = clamp(1 − (d − Min)/(Max − Min), 0, 1)
(0.5 when Max − Min ≤ 1); timer := clamp(**synced int(1, 4)** + 5·int(4f), 0, 24) — one draw. Frozen
while hold flag 0x140b27d5f is set. Each frame timer += 1, position += v̂ × profile(timer) (table
0x140b27a10, 0x49d400); < 25 hidden and REVEAL suspended; 35 `SFXEvent_Arrive_From_Hyperspace` (faction
+0x6b0) and visible (except enemy HIDE_WHEN_FOGGED objects fogged for the local player); 120 REVEAL
resumed; > 149 finish (0x624dc0: as cancel, plus REVEAL pack +0x56 := 1) only if pack +0x260 == 1 [M:
setter]. `Cinematic_Hyperspace_In(delay_frames)` (0x574e50, space, 1 number; needs type `Hyperspace`
+0x24 and vf +0x2b8) → 0x5c9a80: lock +1, leave coordinator, destination := position, start point from
vf +0x2a0, timer := 25 − delay, hidden, state 0x2c (like state 4 but **no draw, no hold**; SFX at 45,
finish ≥ 150), so it lands delay + 125 frames after the call. `Resume_Hyperspace_In()` (0x543570)
clears hold flag 0x140b27d5f. Correction 2026-10-31 [H]: its writer is 0x2cbb50 (16-bit store at 0x140b27a10 +0x34e /
+0x34f), called with LINK_TACTICAL Param9 by 0x28f590 and on save load (0x52d10): Param9 = 2 sets the hold, so story
missions with Param9 = 2 keep reinforcements in hyperspace until the script resumes them (BATTLE_TRANSITIONS §2.9.1).

**End-cinematic flag obj +0x3c9**: written only by Lua `In_End_Cinematic(bool)` (0x57ab30), the
cinematic retreat (:= 1) and the clone path 0x472560; nothing clears it. `Do_End_Cinematic_Cleanup`
(0x541e20) calls 0x38ed10 for objects of list kind 1 of the player resolved from faction 0x140a172d0;
objects with +0x3c9 == 0 get every behaviour suspended, model hidden, scene-list removal and a forced
teleport to (1, 1, 0).

**Teleport** (`Teleport(pos)` 0x5819e0, tactical, 1 position arg): a garrisoned unit is first removed
from its host (0x3a56b0 → 0x37a8d0); then 0x3abb80(pos): with a locomotor — leave the coordinator,
signal 0xB, locomotor idle (vf +0x138(vf +0x188())), space z += `Layer_Z_Adjust` (type +0xdfc); a TEAM
container teleports each member to pos + (member − container offset) recursively; without locomotor set
position and rebuild the transform (0x3ac530, force); with locomotor vf +0x2f8(pos), rebuild, previous
position := pos (no velocity carry-over), pack +0x1c4 / +0x1cc := −1 [M: path cache], 0x39bcb0. No
explicit FOW call (REVEAL refreshes on its next service). `Teleport_And_Face(obj)` (0x581ac0): same,
argument must be a game object; facing and orientation := obj's (+0x84, +0x90; skipped when +0x3a0 &
0x10). The engine's non-forced move 0x3a8f90 only writes the position and sets +0x3a1 |= 1.

Corrections: MOVEMENT §2.6 "Departure" figures do not match the Fighter / SimpleSpace 0x28 path above
[M: they may belong to another path]; `Cancel_Hyperspace` affects arrivals only.

### 3.5 Garrisons [H unless tagged]

**Unit side.** GARRISON_UNIT (0x51, `GarrisonUnitBehaviorClass`); `GarrisonDataPackClass` at obj +0x210
(reset 0x563d00): +8 state, +0xc host id (0x3FFFFF none), +0x10..+0x18 position when entering began,
+0x1c fade alpha, +0x20 second float (1.0), +0x28 / +0x2c fade parameters, +0x30 chosen host bone (−1).
States: 0 none, 1 moving to host, 2 at approach point, 3 eject requested, 4 exiting (fade in), 5 entering
(fade out), 7 inside ("is in garrison" 0x443cc0 = state 7). obj +0x3a7 = garrisoned flag (such units
cannot be targeted, TACTICAL_COMBAT).

**Host side.** GARRISON_VEHICLE (0x4F), GARRISON_STRUCTURE (0x50), GARRISON_HOVER_TRANSPORT (0x56); most
code looks them up in the order 0x50, 0x4F, 0x56 (`Eject_Garrison` checks 0x4F first). Occupants: contents
list at host obj +0xF8 (items +0x20, count +0x28; created by 0x390740). The host's own +0x210 pack uses
only **+0x34 = units reserved (walking in)**: +1 via host vf +0xE0 (0x444aa0 → 0x38c6e0), −1 via
0x38f7b0 when a unit in state 1 or 2 stops approaching or starts entering.

**Masks / capacity / hostility.**
- Mask (0x38f9a0): unit `Garrison_Category` (type +0x1658) ∩ host +0x1658 ≠ 0; a zero mask on either side
  always matches.
- Free space (0x3954b0) = `Num_Garrison_Slots` (+0x2360) − Σ `Garrison_Value` (+0x23ac) of occupants
  **without** DUMMY_UPGRADE (0x65). The engine's entry rule (0x4446b0) is free ≥ unit value (ignores
  reserved).
- Relations: player +0x370[other id] (0 = allied, 1 = enemy). 0x2823c0 / 0x2823e0 = "relation == 0";
  0x39a230(obj, player id) applies it to the object's owner (planets: planetary owner).

**Order** (`GameObjectWrapper.Garrison(host)` 0x576f80): needs GARRISON_UNIT (else warning, nil); 1 arg,
object or AI target with object (dead → error "target is already dead"); host owner must be allied
(0x39a230) else error "target is hostile"; host needs a host behaviour (else error) and matching masks
(else warning "garrison masks do not match"); builds MoveToGarrison (command 55, ctor 0x3ad3b0) via
0x3ad880(event, host, unit, flags 0) and executes it at once (0x3ad620) — no network round trip from Lua.
Returns nothing.

**Approach point** (0x3ad880): over the host's resolved `Garrison_Bone_Names` bones (type +0x2390, count
+0x2398; unresolvable names dropped at type load 0x375380): bone world position, pushed outward by
`Garrison_Enter_Dist` (+0x2368) along the 2D host→bone direction when > 0; keep the bone nearest (3D) to
the unit that passes pathability 0x50eeb0. No usable bone → host position + (cos, sin)(host yaw +0x8c) ×
(Enter_Dist if > 0, else half the host box diagonal), bone −1.

**Execute** (0x3ad620): group move (0x2a7b80; waypoint queue 0x2a8240 with flag 0x08) to the point, then
0x444310: in state 0 subscribe to the unit's signals 10 (move finished) and 0x10 (death), state 1, host
id, host vf +0xE0 (reserve; vehicle / hover hosts also stop, hover sets locomotor +0x2a8 := 3); bone :=
event +0x60.

**Approach.** Signal handler 0x443d10: death, or host free < unit value → cancel (0x443560). Move
finished in state 1 → recompute the point (0x443a90); same point 10 times in a row → give up; 3D distance
− radius ≤ constant 0x140a28bcc → state 2 and 0x443700 (land: capacity check; if 2D distance − radius >
`Garrison_Radius` (+0x236c) issue a short coordinator move (flag 0x20), else start entering); host
locomotor refuses (0x39ade0) → cancel; else re-issue MoveToGarrison with flags 4. Move finished in state
2 → start entering. Unit service vf6 (0x443ff0) states 1–2: cancel on lack of capacity; distance − radius
≤ `Garrison_Radius` → record position, start entering. **Start entering** (0x444440): un-reserve, state
5, alpha 1, fade (0.03, 33.33). State 5: alpha falls; at ≤ 0.1 → state 7 and entry 0x4446b0 (§4.1).

**Entry** (0x379d30 → 0x37ade0, for the unit and each TEAM member): add to host contents, parent +0x2b0
:= host, hide model, suspend TURRET (0xB) and locomotor (vf +0x200), re-register in store spatial lists
(0x2a6430), +0x3a7 := 1, leave coordinators, refresh reveal (0x539470), signal 0x28 on the host. If host
behaviour vf +0xD8 returns 0, TARGETING (9) and WEAPON (10) are also suspended and 0x392570 is called —
occupants fire only when vf +0xD8 is true [open]. Then host vf +0xF0, STORY_GARRISON_UNIT, local-player
`SFXEvent_Bunker_Garrisoned` (faction +0x740).

**Leaving.** `Leave_Garrison()` on a unit (0x57c880): if its parent has a host behaviour (0x444490) →
eject request (0x444d90: state 3, host vf +0xE8); else cancel the approach (0x443560: unsubscribe 10 and
0x10, un-reserve, host vf +0xF8 if state was 2 or 4, reset pack). Returns nothing. `Eject_Garrison()` on
a host (0x5759b0) and network command 52 with unit id 0x3FFFFF (0x440b10): eject request for every
occupant (0x444ab0(…, 0)); command 52 with a unit id does the per-unit leave. State 3 ejects once the
host locomotor state (host +0xA8 → +0x48) ∈ {0, 7, 14, 22, 29, 37} [M: stopped / landed]; hosts without
locomotor eject at once. **Eject** (0x444dd0): remove from contents (0x37a8d0: show, resume weapons and
locomotor, +0x3a7 := 0), state 4 with fade-in (0.2, 5.0), exit point (0x444830: from host position and
yaw, or from the unit's bone outward; distance `Garrison_Exit_Dist` (+0x2364) if > 0 else the full box
diagonal); if the movement system admits the unit, teleport to the start, bone −1 and move to the exit
(flag 0x20), else teleport to host vf +0xD0's position (structures: the unit's recorded entry position);
local-player SFX faction +0x750. Vehicle / hover vf +0xE8 (0x636030 / 0x635fc0) stops the host (hover
also lands, +0x2a8 := 3); 0x636090 / 0x6360b0 restart it when no unit is incoming [M].

**Host death** (Kill 0x391310, host with contents): occupants are **ejected immediately** (0x444c40 →
0x444dd0) when the host has a garrison behaviour, except a hover transport whose locomotor +0x2a8 ≠ 0
[M: airborne]; otherwise 0x379df0 **kills** every occupant and team member (kill credit to the killer).
DUMMY_UPGRADE contents move to another live object of the same type and owner, else are removed from the
galactic planet list and killed. Host owner change (mass change 0x4ca2f0 [M: trigger]) and the bribe
ability (0x5b07c0) eject all occupants.

**Garrison fire.** Occupants keep their own targeting when allowed (vf +0xD8); range × the
`Garrisoned_Max_Attack_Distance_Multiplier` constant (TACTICAL_COMBAT §2.2). The fire path (0x54ade0)
replaces the muzzle when the shooter is garrisoned (host = parent, or grandparent for team members): if
host type `Num_Garrison_Guns` (+0x23a8) ≠ 0, use the gun-bone set (type +0x23a0, built at type load from
names `FP_0<i>_ALT<a>` / `FP_<i>_ALT<a>` for i ≥ 10, i < Num_Garrison_Guns) for the model's current ALT
state (model +0xc4), keep bones within 60° (horizontal) of the host→target direction and pick one with
**synced int(0, n−1)** — one draw per shot; no candidate → normal muzzle [M]. Entry bones (+0x2390) are
never used for firing.

### 3.6 Build pads [H unless tagged]

**Data pack** (`TacticalBuildObjectsDataPackClass`, obj +0x150; ctor 0x561270; save micro 0–6 0x5612d0):
+8 under-construction (UC) object, +0x10 built object, +0x18 pre-placed type (saved as index), +0x20
builder player id (−1 after completion), +0x24 cooldown start frame, +0x28 cooldown end frame, +0x2c
**lock** byte. Buildable now (0x42daf0): +0x28 ≤ now.

**Buildable types.** Pad type `Tactical_Buildable_Objects_Multiplayer` (+0x9d0) in skirmish or when
0x140b15490 is set, else `Tactical_Buildable_Objects_Campaign` (+0x9b8); per-faction 0x28-byte rows
(faction, entries type at +8 stride 0x10, count +0x18); 0x372c10(pad type, faction) = count (or the
entry's int for a given type), 0x374420(pad type, faction, i) = i-th type. Listed types are UC types;
`Tactical_Buildable_Constructed` (+0x9f0) names the final type.

**Rules.**
- Lua / AI rule 0x42e400(pad): pad empty (+8 and +0x10 null) and no **blocking** object within
  `Capture_Point_Radius` (pad type +0x1fc0, 3D; search box = pad box grown to that radius). Blocking =
  alive (+0x3a0 & 0x42 clear, +0x3a0 bit 7 clear), `Influences_Capture_Point` (+0xe0), no behaviour in
  slots +0x36b / +0x391 [M], and **not allied** with the pad owner (0x39a230).
- Player build (`TacticalBuildEvent`, command 33, 0x42edf0(behaviour, pad, type, player)) refuses, in
  order (local player hears error SFX 0x140b301c8): UC present; built present; pad fogged for the player
  (0x35f470); pad not allied-owned (0x39a230); cooldown active; type lacks TACTICAL_UNDER_CONSTRUCTION
  (0x2a). Then **AI players** (0x2814d0 and player +0x62 == 0) build at once via 0x42dbd0 **without
  being charged**; others are refused when cost > credits, else the event is queued and its execute
  (0x5d78a0) calls 0x42dbd0(charge 1, sfx 1).
- Cost 0x3744e0: skirmish `Tactical_Build_Cost_Multiplayer` (+0x9fc); else `Tactical_Build_Cost_Campaign`
  (+0x9f8), falling back to the multiplayer cost when ≤ 0, × planet factor 0x33e440(faction, 1) when the
  battle planet passes 0x3a0910 [M: corruption build-pad price multiplier].

**Start build** (0x42dbd0(behaviour, pad, type, player, charge, sfx)): empty pad, cooldown elapsed (+0x28
≤ now), player exists, 0x39a2c0(pad, player) [M: friendly]; charge → credit check ("Can't afford") and
deduct (0x27f370); create the UC type at `Tactical_Build_Attachment_Bone_Name` (+0xa38, 0x42e280; pad
position if missing) with the pad's orientation; +8 := UC, +0x20 := player; parent link, 0x3912c0(pad,
player), reveal update; UC behaviour links the pad (0x5d7500, 0x5d75e0); pad anim 0x74; sfx → local
`SFXEvent_Tactical_Build_Started` (+0x1490 / +0x1450) and faction speech (+0x2638, +0x2648); +0x24 :=
+0x28 := 0. Returns true.

**Construction** (`TacticalUnderConstructionBehaviorClass`): init vf9 0x5d70f0: build frames =
`Tactical_Build_Time_Seconds` (+0xa00) × per-mode player multiplier (player +0x360 → 0x4b0500: +0x2c
space, +0x30 land) [M: AI difficulty] × 30; end frame +0x18, health rate, anim 0x4a. Service vf6
0x5d7350: health += rate (or max at once if 0x35f790). **Completion** 0x42e890 at the end frame: local
`SFXEvent_Tactical_Build_Complete`; story dispatch 0x326cd0(final type); speech +0x2648; create the final
type for the builder at the bone (+0.1 z), +0x10 := it, +0x20 := −1; speech +0x2658; carry the health
fraction; pad type `Tactically_Built_Child_Object_Persists` (+0xcc) → persist into the galactic planet
(0x42f160 → 0x4b6c70); signal 0x22 on the pad; kill allied objects of the final type's +0xa60 list [L];
signal 1 (replacement) on the UC and kill it. Campaign restore (0x42d8b0 from pad vf13 0x42df80): a
persisted object recorded on the battle planet at the pad's (x, y) (0x4b7c00) is recreated at once.
Pre-placed +0x18 types spawn at start (UC → final type); in skirmish only with option bit 0x140b154bc &
8 and pad type `Respects_Skirmish_Pre_Build_Bases_Option` (+0xe5); else pad anim 0x75.

**Child goes away** (0x42e6d0 from 0x39bdb0): clear +8 or +0x10; if it was the built object also **clear
the lock +0x2c** and remove its galactic persistence (0x42f160 → 0x4bb3a0); pad type
`Destroy_When_Child_Dies` (+0x99) → kill the pad; otherwise reset the pad's CAPTURE_POINT to neutral
(0x501050), refresh reveal, and if `Minimum_Time_Before_Pad_Can_Build_Again` (+0xa0c) > 0 set +0x24 :=
now, +0x28 := now + that × FPS (0x27c9a0); anim 0x75.

**Selling** (`TacticalSellBehaviorClass` 0x5248b0, command 34): **silently refused when the built
object's pad has the lock byte set** — the only reader of the lock found. Else refund = int(cost (UC
entry whose final type matches, 0x374590) × `Tactical_Sell_Percentage` (+0xa08)); credit, local SFX
(+0x1480), faction sell SFX (+0x2668); release the pad (0x42ecd0: +0x10 := 0, remove persistence, reveal,
anim 0x75, **no cooldown**); kill the object. So `Lock_Build_Pad_Contents(true)` = "cannot be sold";
it clears when the object dies [M: no other reader].

**Visibility** (vf20 0x42e610, local player): an occupied pad is hidden with `Hides_When_Built_On`
(+0x97); an empty pad is visible to enemies only with `Visible_To_Enemies_When_Empty` (+0x98).
**Ownership:** the pad is a capture point (§4.2); one with contents keeps its owner (capture rule (a));
after its child dies the point resets to neutral.

### 3.7 Bombing runs (land) [H unless tagged]

**Manager** `BombingRunManagerClass` at land mode +0x38: +0 enabled, +8 mode, +0x10 id of the marker-owner
player (resolved from faction 0x140a172d0 by 0x294d30 [M: neutral "system" player]), +0x14 default
interval = int(FPS × GC `Max_Bombing_Run_Interval_Seconds` +0x11e80), +0x18 vector of 0x78-byte
`PlayerBombingRunClass` records per player index (+0x20 array, +0x28 count). Record: +0 enabled, +1
available, +0x28 in progress, +4 ready frame, +8 interval, +0xc last start frame, +0x10 squadron
reduction, +0x14 percent-modifier sum, +0x18 time factor, +0x20 bomber type, +0x30 bombers, +0x48
markers, +0x60 bomber states.

**Init** (0x493d50, land only; all records disabled first).
- Skirmish (no parent mode): every player whose faction has `Is_Playable` (+0x20a8) and a skirmish bomber
  type (faction +0x2af8 [M on the slot vs `Skirmish_Land_Bomber` +0x2af0]) gets enabled, available,
  type, interval = mgr +0x14, ready = now + interval.
- Campaign: battle record (0x2ac5a0) [0] planet, [1] player, [2] second player [M: attacker / defender].
  m = Σ `Bombing_Run_Percent_Modifier` (+0x1f90) over land-store objects of player [2] whose type has
  `Modifies_Bombing_Run_Time` (+0x73). In the battle planet's galactic orbit list (0x395900) count player
  [1]'s objects (contents expanded, 0x3965f0) with `Ship_Class` (+0x650) == 1 [M: bomber], including
  company members up to the player's tech level. If any: n = max(count, 3); r = (n/3 − 1) × GC
  `Bombing_Run_Reduction_Per_Squadron_Percent` (+0x11e88); f = max(0, m + 1 − r); interval =
  max(int(FPS × GC `Min_Bombing_Run_Interval_Seconds` +0x11e84), int(mgr+0x14 × f)); type = the bomber's
  `Land_Bomber_Type` (+0x1f88 [M slot]); ready = now + interval; +0xc = now; enabled. **Only player [1]
  can call bombing runs in a campaign battle.**

**Available** (0x494410): manager enabled, 0x140b15490 clear, (not skirmish or SW option), record enabled,
record available, not in progress.

**Service** (0x495dd0, every frame; only when record count = player count and a local player exists):
not in progress → if tactical data (mode vf +0x220(i)) says not allowed (0x527550: AND of bool list +0x1c8
/ count +0x1d0; empty = allowed except in skirmish) available := 0; else multiplayer game types (0x28af80:
GMM +0x18 ∈ {1, 2, 4} and +0x20 ≠ 1) or type 3 (0x28b060) → available := 1, else available := (ready ≤
now). On the 0→1 edge with 0x494410 true and now > 0 the local player hears `SFXEvent_Bombing_Run_
Available` (+0x1fd0) / `_Ally_` (+0x1fe0) / `_Enemy_` (+0x1ff0). In progress, per bomber i: retarget to
marker i if needed (state < 2); state 0 and targeting state (vf +0xf0) 4 → 1; state 1 and vf +0x160(marker)
false → vf +0xd8(1), state 2 [M: pass done]. The bombs are the bomber type's own weapon / BOMB behaviour
firing at the marker; the manager deals no damage.

**Target validity** (0x494540(mgr, player, pos)): point **not fogged** for the player (FOW point test,
force 1); no BASE_SHIELD (0x39) object with `Base_Shield_Radius` (+0x21f4) > 0 and an active shield
(shield pack +0x28 → +0x1c8, +8 == 1) within 3D distance < radius (any owner, own shields included); no
object **not owned by the player** (allies and neutral included) with `Bombing_Run_Prevention_Radius`
(+0x1dfc) > 0 within ≤ that radius (0x2a9ff0 excluding the player).

**Start** (0x492d60(mgr, player, pos)): silent return when disabled or the target is invalid; not
available → log, local error SFX 0x140b301c8, stop. Else mode vf +0x260(0, 0) [L], available := 0, in
progress := 1, +0xc := now; path (0x4935e0) = straight line through the target from map edge to opposite
edge (mode vf +0x208 bounds), z = terrain (raised to water); **three bombers** in a wedge, path-frame
offsets (0,0,0), (−70,+70,0), (−70,−70,0) rotated by the path yaw: create the record's bomber type at
entry + offset (0x29f810), notify the player's AI context (player +0x360 → 0x4b06d0 mode 1 → +0x50 →
+0x38, vf +0xa0), create `IN_GAME_BOMBING_RUN_MARKER` (owner mgr +0x10) at target + offset, state 0,
group move (0x2a7b80) entry → exit, target := marker. Allied with the local player → faction
`SFXEvent_Bombing_Run_Begin_Crosstalk` (+0x2010). **No RNG.**

**End** (Remove_Object notify 0x492910): when bomber i or marker i is removed, kill bomber i and drop
entry i; when no bombers remain: kill every DUMMY_DESTROY_AFTER_BOMBING_RUN (0x66) object of that
player, ready := now + interval, available := 0, in progress := 0 [M: LandBomber removes itself at the
exit]. Save: manager chunk 5 (micro 0 enabled, 1 mode, 2 +0x10, 3 +0x14, 6 count), record chunk 4
(0x4958c0), type saved by CRC. UI query 0x4950f0 (frames until available).

Lua: `LandTaskForceClass.Bombing_Run` §2.10; `PlayerWrapper.Disable_Bombing_Run(bool)` (0x601aa0, bound)
pushes the bool with no expiry into tactical list +0x1c8 under key 0x1407ff8a8 (0x3a8cb0) [M: whether
stored negated; shipped `Disable_Bombing_Run(true)` must disable]. Network command 28: dequantise, start
if 0x494540 passes.

### 3.8 Orbital bombardment (land) [H unless tagged]

**Manager** `PlanetaryBombardManagerClass` at land mode +0x40: +0 enabled, +8 mode, +0x10 int(FPS × GC
`Max_Bombard_Interval_Seconds`) (unused by records), +0x18 vector of 0x48-byte records, +0x30 lighting
flashes, +0x48 faction override [M: setter], +0x50 bombard-delay override seconds (story reward 0x7a
`BOMBARD_DELAY`). Record: +0 enabled, +1 available, +0x20 in progress, +4 ready, +8 interval, +0xc
deferred start frame (−1 none), +0x10 start frame, +0x18 required ship type, +0x24 next shot frame, +0x28
shot index, +0x2c salvo index, +0x30..+0x38 target, +0x3c distribution cursor, +0x40 timer-only flag.
Faction data: `Bombardment_Required_Orbital_Ships` (+0x2778), `Bombardment_Projectile` (+0x2790),
`Bombardment_Particle` (+0x27a8), `Number_Of_Salvos` +0x27b8, `Shots_Per_Salvo` +0x27bc, `Shot_Delay`
+0x27c0, `Salvo_Delay` +0x27c4, `Initial_Delay` +0x27c8, lighting lists +0x27d8 / +0x27f0 / +0x2808.

**Init** (0x5284b0): all records disabled, timer-only 1, ready −1. Skirmish: each playable faction with a
non-empty required-ships list → enabled, available, interval = int(FPS × GC
`Min_Bombard_Interval_Seconds`), ready = now + interval, timer-only 0. Campaign: only battle-record player
[1], if any of its orbit objects (expanded) has a required-ship type → enabled, interval Min, ready = now
+ interval, start = now, timer-only 0 (ship count irrelevant).

**Available** (0x528970): as bombing runs. **Service** (0x52a110): not in progress → timer-only: available
:= ready ≤ now; else as bombing runs using tactical list +0x250 (0x5275a0); edge SFX
`SFXEvent_Bombard_Available` / `_Ally_` / `_Enemy_` (+0x2020 / +0x2030 / +0x2040). In progress: deferred
start (+0xc ≠ −1 and < now) → spawn particle[salvo] at the target, +0xc := −1. Shot when next shot < now:
projectile = Projectile[salvo] (last entry if shorter); aim = target, scattered when Shots_Per_Salvo > 1,
not the first shot of the first salvo and GC `Bombardment_Distribution` (+0x11ea0, count 0x140b276b0)
non-empty: q = Distribution[cursor], cursor += 1 wrapping; angle = synced float(q × 90°, (q + 1) × 90° −
1°); radius = second synced float [M: range lost]; aim += (cos, sin) × radius. Spawn point = aim + GC
`Bombardment_Offset` (+0x11e94); fire through the weapon-event path (0x549d70 / 0x54ade0) owned by the
player, no target object; queue a lighting flash. Advance: shot += 1 (next += Shot_Delay frames) / next
salvo (shot := 0, next += Salvo_Delay frames, spawn particle) / finish (ready := now + interval, in
progress := 0, kill every DUMMY_DESTROY_AFTER_PLANETARY_BOMBARD (0x69) object of the player).

**Start** (0x528140(mgr, player id, pos)): silent return when disabled, the point is fogged for the player
(force 1) or an active base shield of any owner covers it (**no** prevention-radius test); not available
→ log + local error SFX, stop. Else mode vf +0x260(0, 0); available 0, in progress 1, start = now, shot =
salvo = 0, target = pos; init = int(FPS × Initial_Delay); with mgr +0x50 > 0: d = int(FPS × +0x50), extra
= max(0, d − init), else d = init, extra = 0; next shot = now + int(FPS × Shot_Delay) + d; **cursor =
synced int(0, count − 1)** (drawn even when no scatter follows); extra == 0 → spawn particle[0] now, else
+0xc := now + extra; raise "start_bombard" on the mode's event manager (0x32edd0 on mode +0xa0) [M:
consumer].

Lua: `LandTaskForceClass.Fire_Orbital_Bombardment` §2.10; `PlayerWrapper.Disable_Orbital_Bombardment(bool)`
(0x601be0, bound): writes (not arg, no expiry) into tactical keyed list +0x250 (key 0x1407ff8a8);
`Remove_Orbital_Bombardment(bool)` (0x604860, bound): record enabled := not arg (0x52aee0). AI perception
`IsOrbitalBombardmentAvailable` / `IsBombingRunAvailable` (0x6d43d0 / 0x6d4350) = 1.0 iff 0x528970 /
0x494410. Network command 54: dequantise, start if 0x528a70 (fog + shield) passes. Save: manager chunk 4
(0x529c90), record chunk 3 (0x529e00).


---

## 4. Story event sources (C)

Dispatch plumbing (direct vs queued delivery, gates, Fire) is STORY.md §1–§4; the param-matching classes
are STORY.md §5 and SCRIPT_GAPS §4. All 59 call sites of the two dispatch functions (0x32e5f0, 0x5306b0)
were checked for the types below.

### 4.0 Already specified

| Event / input | Where |
|---|---|
| STORY_COMMAND_UNIT (0x25) "UnitCommanded": raised by the move-order executors 0x3ad100, 0x3ae090, 0x3ae6c0, 0x3aebb0, per **local-player** unit of the order's selection (data1 = unit type name, data2 = destination) | SCRIPT_GAPS §4.3 |
| STORY_GUARD_UNIT (0x29) "UnitGuarding": raised by the guard order 0x3c100 per local-player unit (data1 = unit type, data2 = guarded object's type) | SCRIPT_GAPS §4.4 |
| SPACE / LAND_TACTICAL, VICTORY, DESTROY / TACTICAL_DESTROY, DIFFICULTY_LEVEL, UNIT_PROXIMITY, CHECK_DESTROYED | SCRIPT_GAPS §4.1–§4.8 |
| CORRUPTION_CHANGED, CORRUPTION_TACTICAL_COMPLETE / _FAILED (battle-end side) | SCRIPT_GAPS §4.7 |

Note for S4: COMMAND_UNIT and GUARD_UNIT are raised only from **player orders** (the network command
executors), not from Lua `Move_To` (0x57da90 goes straight to the coordinator 0x4f8200) [H] nor, as far
as traced, from Lua `Guard_Target` or AI task-force orders [M: the four executors and 0x3c100 (called
from the event switch 0x41f70) are the only callers]. A port that wants them in scripted battles must route the local player's
UI orders through the same executors.

### 4.1 STORY_GARRISON_UNIT (0x37) "Garrisoned" [H]

- **Dispatcher** 0x327cd0(story manager, unit), non-null unit only → 0x32e5f0 with type 0x37, no player,
  data1 = the unit object, data2 = 0; current mode's story manager (DAT_140b15418 +0xa0), every player's
  active plots, usual direct / queued rule.
- **Raised by** 0x4446b0 (GarrisonUnitBehaviorClass entry), called from the unit service vf6 0x443ff0 in
  state 5 when the fade alpha (+0x1c) ≤ 0.1 (state → 7). Host = object id in pack +0xc (pack obj +0x210);
  host behaviour GARRISON_STRUCTURE, else VEHICLE, else HOVER. Order inside 0x4446b0: capacity check
  (free ≥ the unit's `Garrison_Value`, §3.5) → stop locomotor, move to the host position, hide, add to the
  garrison list (0x379d30) → **dispatch** → local-player `SFXEvent_Bunker_Garrisoned`. So it fires once per
  unit at the moment it actually enters, for any owner (filtered by the event).
- **Match** (vf3 0x4555f0): SingleObjectName rule (STORY.md §5): Param1 type names, Param2 counts, Param3
  filter; default filter FILTER_FRIENDLY_ONLY, tested on the unit's owner. No shipped FoC / EaW XML uses
  it.

### 4.2 STORY_CAPTURE_STRUCTURE (0x35) "StructureCaptured" [H]

- **Params** (vf2 0x4641e0): Param1 = structure type names (+0x360, upper-cased; unknown names log
  "invalid object name" but stay); Param2 = a faction name resolved at parse time (0x3303d0) to the
  faction object (+0x378) when non-empty.
- **Dispatcher** 0x32e7a0(story, structure, new owner): data1 = structure, data2 = new owner's faction
  (player +0x68); current mode's story, all plots, no player restriction.
- **Match** (vf3 0x4540b0): Fire (no object) when data1's type name equals a Param1 entry **and** data2 ==
  the Param2 faction. No filter, no counter. Empty Param1 or Param2 → never fires.
- **Shipped use**: SANDBOX_LAND_CAPTURE.XML `Sandbox_Objective_2_Complete` has empty params; the sandbox
  objective builder 0x32bd10 fills them at runtime (vf2(0) with the target type list, then vf2(1) or vf2(2)
  with a second list by a flag; this class ignores index 2) [H calls, M list contents]. 0x329530 picks
  sandbox_land_capture.xml instead of sandbox_land_destroy.xml when the sandbox target has CAPTURE_POINT
  (0x3d).
- **Raised by** capture completion 0x500e30 (below).

**Land capture rules** (`CapturePointBehaviorClass`, service vf6 0x501980; `CapturePointDataPackClass` at
obj +0x1e0: +8 pending controller player id (−1 none), +0xc progress 0..1) [H unless tagged]:
- **Start gate**: capture logic runs only when now ≥ int(30 × countdown + 0.5) (countdown = GC
  `Land_Capture_Allowed_Countdown_Seconds` +0x11e74 in land, `Space_Capture_Allowed_Countdown_Seconds`
  +0x11e70 in space; 0 in skirmish), or the owner is null, or the owner's faction byte +0x108 is set [M:
  neutral faction].
- **Controller this tick** (0x500a00), in order: (a) the object has TACTICAL_BUILD_OBJECTS (0x29) and its
  pack (+0x150) is non-empty → owner keeps it; (b) GARRISON_STRUCTURE with occupants → the first
  occupant's owner; (c) otherwise scan objects in an axis-aligned box then a 3D sphere of
  `Capture_Point_Radius` (type +0x1fc0): candidates have `Influences_Capture_Point` (+0xe0), are not the
  point, have an owner with a faction, are alive (+0x3a0 & 0x42 clear), not garrisoned (+0x3a7), and the
  point's type passes 0x3751a0 [M: capturable by that faction]. Owner is the neutral player (0x294aa0):
  the first candidate's owner controls it, a second candidate not allied with it (0x39a230) → neutral id
  (contested). Owner not neutral: any candidate not allied with the owner → neutral id + contested flag;
  else the owner keeps it. (d) no candidates: the owner keeps it if the point has REINFORCEMENT_POINT
  (0x49) or `Ownership_Sticks` (+0x95), else neutral id (decay).
- **Progress** per service: step = (1 / (T × m)) / 30 × the behaviour's service interval (vf +0x28); T =
  `Capture_Point_Transition_Time_Seconds` (+0x1fc4), T = 0 → step 1 (instant); m = 1 + the controller's
  tactical modifier list +0x288 (`Capture_Point_Time_Multiplier` of `Battlefield_Modifier_Ability`; max
  within a source then summed, clamped by +0x298 / +0x29c; m ≤ 0 → 1; decay uses m = 1). Controller ==
  owner → progress −= step (min 0); at 0 refresh team colour and pending := owner. Other controller →
  pending := it; from 0 play anim 0x4b with colour, local `Begin_Control_Transition_Radar_Event` (type
  +0x1758 / +0x1768), and for a local-owned REINFORCEMENT_POINT being neutralised the faction speech
  `SFXEvent_HUD_Reinforcement_Point_Contested` (+0x2628); progress += step (max 1). At 1: **capture
  complete** (0x500e30, old → new owner), owner change (0x38e5a0 with pending), progress := 0. An
  enemy-held point therefore goes owner → neutral → capturer in two full transitions. Shader
  "MeshAdditiveOffset" UV offset shows progress (1 − progress when the controller is neutral).
- **Capture complete** 0x500e30, in order: refresh team colour; mode notification 0x360d90(mode, object,
  old, new) [M: stats / victory]; **dispatch STORY_CAPTURE_STRUCTURE**; local SFX (type +0x1788 via
  0x355ca0) when the local player is old or new owner; faction speech from faction tables (0x33e8b0,
  0x33e7d0, 0x33e840, 0x33e760; lists at faction +0x22f8, 0x50-byte entries keyed by object type) chosen
  by lost / gained and old / new owner neutral.

### 4.3 STORY_CAPTURE_HERO (0x11) "HeroCaptured" [H]

**No dispatcher**: none of the 59 dispatch calls passes 0x11, so it can never fire in 1.121. Class
SingleObjectName (parses like CONQUER). No shipped XML uses it. **Port rule: parse, never evaluate.**
The related live event is STORY_DEFEAT_HERO (0x12): dispatched by 0x326f80 from the death path 0x3a01f0
(param 2 = killer) for types with `Is_Named_Hero` (+0x13); data1 = the dead hero's type name, data2 =
the first ancestor of the container chain (+0x2b0) with PLANET; logs "STORY - HERO DEFEATED"; without a
planet ancestor no dispatch, so in practice galactic only [M: a tactical death reaches it through the
parent-mode object]. Shipped: UNDERWORLD_M02 `Tyber_Has_Been_Killed`, `Random_Story_Triggers`. Hero
"neutralize" (bounty abilities, `Galactic_Neutralized_Event` callback) dispatches no story type [M].

### 4.4 STORY_LOAD_TACTICAL_MAP (0x2d) "LoadTacticalMap" [H]

- **Params** (vf2 0x466370): Param1 = planet names (+0x378; unknown → "invalid planet name"); Param2 =
  hero names (+0x360; unknown logged, kept); Param3 = keyword via the story name→int table 0x45c3f0 into
  +0x390: 0 = ground (land), 1 = space [M high; table strings include GROUND and EITHER]; unknown / absent
  → 0 (land).
- **Dispatcher** 0x32a310(story, planet, &kind) from `BattleLoadDialogClass::Activate` (0x8d930), when the
  battle-load screen opens, before the map name is fixed; kind 0 for a land battle (load type 1), 1 for
  space (load type 2); current mode's (galactic) story, all plots; data1 = battle planet, data2 = &kind.
- **Match** (vf3 0x455130): data2 == Param3 and the planet's type name in Param1; then Param2 empty →
  Fire(planet); else for hero entry i: all objects of that type in the current store (0x2a87d0), take
  element i (original quirk: indexes the instance list by the Param2 position; port: treat i ≥ count as no
  match), walk its parents to the first PLANET; if it is the battle planet → Fire(planet). vf10 (0x452880)
  is a land-only query (planet in Param1, object type in Param2); callers not traced.
- **Purpose: map override.** Reward SET_TACTICAL_MAP (0x461d90): Reward_Param1 = map file; Reward_Param2
  = location via the CRC keyword map 0x140b30728 (1 → space slot 0x140b30430; 0 / missing → land slot
  0x140b30450; else "failed due to invalid location (GROUND/SPACE)"); Reward_Param3 = faction name into
  0x140b30568 (Rebel 0, Empire 1, Pirates 2, Underworld 4, Hutts 5, else −1) [M: forced faction]. Right
  after the dispatch 0x8d930 copies the land / space slot (by battle kind) into the battle map name when
  non-empty and sets the "story map" flag 0x140a61d3b := 1; battle start 0x28fda0 reads and clears the
  slots. So LOAD_TACTICAL_MAP + SET_TACTICAL_MAP replaces the map of that same battle. No shipped FoC /
  EaW XML uses either (corruption missions use planet-type fields, §4.5). Correction for
  BATTLE_TRANSITIONS §2.7: this event does not start the queued battle record.

### 4.5 FoC corruption tactical missions [H unless tagged]

**Entry**: corruption action executor 0x52bf50(choice, defiler id, planet), from the human command
`SpecialAbilityEventClass` vf4 case 6 (0x4b4d70, the Defiler's corrupt ability) and the AI picker
0x52b160 (weighted choice over `CorruptionChoiceProperties` [M]). Choice record: +0 corruption type, +4
credit cost (float), +0x20 count.

**Steps**: (1) the defiler must exist (else log "Cannot complete corruption action because the defiler
no longer exists!"); (2) planet data pack exists, the planet has PLANET, planet data bytes +0x2e2 and
+0x2e3 are 0 [M: not destroyed / not blocked], planet data +0x2f4 == −1 [M: no corruption in progress];
(3) owner credits ≥ cost, deducted when > 0; (4) requirement check 0x523950 [M: `Corruption_N_Required_
Unit_Type` / `_Count` owned and alive, plus mission and planet requirements]; (5) optional side effect
0x52b870 when count > 0 [L].

**Mission or direct.** A tactical mission is used only when all hold: the planet type's slot for this
corruption type (`Corruption_1/2/3_Type` at +0x2464 / +0x2608 / +0x27a8, stride 0x1a4) has a non-empty
`Corruption_N_Tactical_Map_Name` (slot +0x5c) and `Corruption_N_Tactical_Game_Mode` (slot +0x9c) Land
(1) or Space (2); the corrupting player is not AI [M: 0x52bdf0 "AI" when 0x2814d0 and player +0x62 == 0];
GMM predicate 0x28af30 false [M: multiplayer / online]; debug flag 0x140b39070 clear.
- **Direct path**: apply at once 0x3f6810(planet behaviour, planet, defiler, 5.0, type) (failure → local
  error event 0x140b301c8); player stat 0x2820a0; story events 0x3284c0 (LAND_ON and DEPLOY for the
  defiler at the planet); with planet data +0x2e4 set and a local player, `Corruption_N_Success_Speech_
  Event` and `Success_Bink_Hologram_Name` (0x362510); log "Corruption underway at %s"; the defiler departs
  (0x48eb10, 0x48cf20). This path dispatches STORY_CORRUPTION_CHANGED through the planet change (SCRIPT_GAPS
  §4.7).
- **Mission path**: queue a battle 0x28bd70 into the queued battle record GMM +0xf0 (script at +0x148)
  with planet id, mode, attacker = the defiler's owner id, map = Tactical_Map_Name, the planet owner's
  faction name, script = `Corruption_N_Tactical_Script_Name` (a Story_Plots file, e.g.
  Story_Plots_Underworld_Kessel_Piracy.xml); start it at once (0x28f590); set the **pending corruption
  mission** (0x28bbf0): GMM +0x1f4 = corrupting player id, +0x1f8 = corruption type, +0x1fc = 5.0; the
  defiler departs. Correction for BATTLE_TRANSITIONS: GMM +0x1f4..+0x1fc is this pending mission.
- **During the battle**: `Initiate_Space_Conflict` (0x2af7c0) / `Land_Invade` (0x2b0520) play
  `Corruption_N_Mission_Commencing_Speech_Event` (slot +0x174) for the local corrupter instead of the
  faction default (+0x650 / +0x640); 0x2b0a20 (force admission) tests +0x1f8 [M: limits joining units];
  the Tactical_Script_Name plot provides objectives [M high: loaded as the battle's story plot like
  STORY_*_TACTICAL Param1]; the victory monitor 0x343b90 shows TEXT_WIN_CORRUPTION_MISSION /
  TEXT_LOSE_CORRUPTION_MISSION while +0x1f4 ≠ −1 (0x28ae30).
- **Success / failure** = whether the battle winner is the corrupting player: battle end (0x28bf10)
  applies the corruption and dispatches CORRUPTION_TACTICAL_COMPLETE, else _FAILED, then +0x1f4 := −1
  (SCRIPT_GAPS §4.7).
- Shipped data: 29 slots have a Land or Space mission (space: Kessel, Mandalore, MonCalamari Piracy;
  land: NalHutta, Kuat, Utapau, Thyferra, Muunilinst Kidnapping; Taris, Saleucami, Honoghr
  Intimidation, …).

---

## 5. Open items and in-game probes

### 5.1 Open items
1. Weather: reader of the speed-modifier table (manager +0x90, 0x4098d0); meaning of the per-player list
   +0x410 / clamps +0x420..+0x423 (0x282290); whether anything reads `randomize_phase_selection`.
2. Space fields: exact Take_Damage arguments of an asteroid hit; service cadence of the three field
   behaviours; ability set 0x140b2f2e0 cancelled in nebulae and the exit step 0x39fb40; whether
   combined strings parse in `Find_Nearest_Space_Field`.
3. Hyperspace: setter of locomotor pack +0x260 (arrival finish) and +0x2b2; ~~writer of hold flag
   0x140b27d5f~~ (0x2cbb50, LINK_TACTICAL Param9 = 2, §3.4); whether the departure kill counts as TACTICAL_DESTROY / stats; driver of the retreat
   counter 0x140b27b64 and whether the arrival min/max globals are sim state (determinism risk, feeds a
   synced draw count).
4. Garrison: host vf +0xD8 (may occupants fire?); arrival constant 0x140a28bcc; eject locomotor-state
   set; mass owner change 0x4ca2f0 trigger; `Can_Garrison` vs `Garrison` for neutral hosts.
5. Build pads: predicate 0x39a2c0; whether AI tactical builds are charged elsewhere; +0xa60 kill list;
   any other reader of the lock byte (UI sell button).
6. Bombing / bombard: what removes bombers at the path end; `BomberStateType` semantics; scatter radius
   range; who sets bombard manager +0x48; mode vf +0x260(0, 0); battle-record slot [1] vs [2]
   (attacker / defender — also SCRIPT_GAPS open item 1). `Disable_Bombing_Run` stores the negated value
   [M high: shipped Underworld missions call `Disable_Bombing_Run(false)` to enable runs].
7. Story: capture predicates (+0x150 pack gate, +0x3a7, 0x3751a0) and 0x360d90; LOAD_TACTICAL_MAP
   Param3 keyword strings; use of 0x140b30568; corruption requirement rules 0x523950 / 0x52b870 and
   planet data +0x2e2 / +0x2e3 / +0x2e4 / +0x2f4; how the tactical mode loads `Tactical_Script_Name`.
8. Methods: `Divert` arg 2 meaning (move params +0x54) and the end-of-diversion rule; ExploreAreaBlockStatus
   service / IsFinished; `Are_On_Opposite_Sides_Of_Shield` projectile gate and shield pack +0xc; the
   0x5f1f90 power-versus table; creator of `LuaScriptWrapper`; consumer of opportunity-fire
   subscriptions; `Get_Type_Of_Unit` vector; `Refit_To_Definition` internals; range arguments of the
   synced draws in `DiscreteDistribution.Sample` and `Give_Random_Sliceable_Tech` (assumed (0, total)).

### 5.2 In-game probes (prepare under tools/probe_mod/ with run + cleanup scripts; not run)
- **W1** Land map with a multi-phase scenario: `Force_Weather()` → immediate phase jump, no fade; save /
  load mid-ease-out → ease-out restarts. **W2** Snow vs clear: infantry sight × modifier, laser spread
  × (2 − m).
- **S1** Frigate in / out of a nebula polling `Is_In_Nebula` each second (true up to 5 s after exit;
  active ability cancelled on entry). **S2** Shielded ship in an ion storm (no absorb / regen;
  `Is_In_Ion_Storm` false right after exit). **S3** Corvette vs frigate through an asteroid field (only
  the frigate is hit; hit rate vs `Asteroid_Field_Damage_Rate`). **S4** `Find_Nearest_Space_Field(pos,
  "Nebula | Ion_Storm")` and `"All"`.
- **H1** `Hyperspace_Away()` → removal after 90 frames; `Start_Cinematic_Space_Retreat(id, 2)` per-unit
  removal frames. **H2** `Hyperspace_Away(true)` vs `(false)` in a galactic battle, then check galactic.
  **H3** `Cinematic_Hyperspace_In(30)` lands at frame 155, synced seed unchanged. **H4**
  `Cancel_Hyperspace()` mid-arrival / mid-departure. **H5** `Teleport` a garrisoned unit and a fighter
  squadron.
- **G1** 2-slot bunker: `Can_Garrison` false once one unit is walking in, second unit still enters.
  **G2** Garrisoned bunker destroyed (occupants eject) vs airborne hover transport (occupants die).
  **G3** Projectile origin bones of garrison fire (`FP_0x_ALTy`, within 60°).
- **B1** `Lock_Build_Pad_Contents(true)` blocks selling; destroy + rebuild → lock gone, cooldown =
  `Minimum_Time_Before_Pad_Can_Build_Again`. **B2** `Build(type, true)` with an enemy inside
  `Capture_Point_Radius` → false + warning; `Build(type)` succeeds without charging. **B3** AI credits
  across an AI tactical build.
- **BR1** Campaign land battle with bombers in orbit: `TF.Bombing_Run` → 3 bombers wedge, `Result` =
  surviving fraction, interval formula. **BR2** targets under a shield / near a prevention radius / in
  fog → "invalid area". **BR3** skirmish with the superweapon option off / on.
- **OB1** `TF.Fire_Orbital_Bombardment(pos)` salvo / shot counts and delays, first shot exact, later
  scattered; save / load mid-bombard. **OB2** `Disable_Orbital_Bombardment(true/false)`,
  `Remove_Orbital_Bombardment(true)`, `BOMBARD_DELAY` reward. **OB3** fogged point → silent no-op.
- **T1** `Find_Best_Local_Threat_Center` with 4 vs 5 objects: synced seed unchanged for 4, 2k draws for 5;
  returned z = 0. **T2** `DiscreteDistribution` and `Give_Random_Sliceable_Tech` consume one synced draw
  each; a single candidate raises tech level. **T3** `GlobalValue.Set("x", 1)` in galactic, read in a
  tactical script and after save / load; `Set("x", unit)` errors.
- **D1** `Get_Rate_Of_Damage_Taken` / `Get_Time_Till_Dead` under constant fire vs FPS × damage / service
  interval. **D2** `Get_Final_Blow_Player` on a dead unit's wrapper and on a conquered planet.
- **ST1** Capture timing owner → neutral → enemy (two transitions), contested freeze, campaign start delay.
  **ST2** STORY_CAPTURE_STRUCTURE with / without Param2 faction. **ST3** STORY_GARRISON_UNIT fires per
  entering unit. **ST4** STORY_LOAD_TACTICAL_MAP + SET_TACTICAL_MAP map override (GROUND / SPACE).
  **ST5** STORY_CAPTURE_HERO never fires. **ST6** Defiler corrupts Kessel (Piracy): mission map + plot;
  win → COMPLETE, lose → FAILED; AI Underworld corrupts directly. **ST7** a scripted `unit.Move_To`
  does not trigger STORY_COMMAND_UNIT; a player click does.
