# Lua / story items left open by L1/S4 wave 2a (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by an RE agent for the L1/S4 code agents (docs/PHASE4.md, "L1/S4 wave 2a" open item:
"RE (bodies not in docs/re)"). Clean-room: prose and tables only. Tags: **[H]** read directly in the
decompile or disassembly, **[M]** inferred, **[L]** guess. RVAs are image-relative. Behaviour-slot
numbers are the obj +0x332 table ids of OBJECT_RUNTIME.md §4.3. Fog-of-war bindings are in
FOG_OF_WAR.md §6. Story plumbing (dispatch, gates, Fire) is STORY.md.

Common conventions in the bindings below [H]:
- "error" = the script error helper 0x248190, "warning" = 0x248620. Neither unwinds by itself: unless
  stated otherwise the binding returns at once with no value; where it carries on (argument defaults
  in §3.1) that is stated.
- A "player" argument is a PlayerWrapper; an "AI target" is an AITargetLocation wrapper (its object, if
  any, via vtable +0x30, its position via +0x68).
- Readiness of a special weapon data pack (obj +0x158, 0x4914e0): 1.0 when now ≥ ready frame (+0xc),
  else clamp((now − start (+0x8)) / (ready − start), 0, 1). "Ready" means readiness ≥ 1.0.
- "Destroy-after sweep": every object of the given player with behaviour 0x67
  (DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED) is destroyed (0x391310(obj, 1)).

---

## 1. Special weapons and superweapons

### 1.1 `GameObjectWrapper.Fire_Special_Weapon(target, player)` 0x576220 [H]

1. Exactly 2 args, live object. The object must have SPECIAL_WEAPON (0x2f) or LOBBING_SUPERWEAPON (0x37)
   or DUMMY_STAR_BASE (0x5c, a space station) — else error "object %s is not a special weapon".
2. target = AI target or game object (dead → error "already dead", neither → error "invalid target").
   Arg 2 must be a player (else error).
3. **Lobbing superweapon** (0x37 present, no 0x2f, not a station): readiness of obj +0x158 (no pack →
   error) < 1 → warning "not ready", nil. Else fire at the target **position** (0x4ea3f0, §1.6).
4. **Special weapon** (0x2f, not a station):
   - an AI target without an object → error "may only be fired at objects";
   - readiness (0x491f30) < 1 → warning "not ready", nil;
   - begin targeting (0x491b40, §1.6) fails → error;
   - **upgrade gate**: if a space mode exists in the mode stack (0x28a950(GMM, 2)) and the player has
     tactical data there, `SpecialWeaponsAllowed` (0x527620) false → warning "special weapons not
     available. Purchase a special weapon upgrade", nil;
   - fire at the target object (0x491cd0, §1.6); failure → error "Unknown and confusing error".
5. **Space station** (DUMMY_STAR_BASE): its special-weapon hard point = the first hard point whose type
   has the special-weapon flag (+0x50) (0x394e60; none → warning). Readiness 0x385c00: 0 unless the
   hard point state (+0x48) is 5..10; 1 when no recharge is pending (+0x58 and +0x5c both 0), else
   a progress ratio from the recharge frames (formula in 0x385c00). < 1 → warning. Upgrade gate as above. Can-fire test 0x386220 (target enemy of the station owner, not
   protected (0x540140), inside the fire arc (0x383ba0), **not FOW-hidden for the owner with force 1**,
   2D distance from the fire bone (mid-point of the two bones when both exist) ≤ max range + target
   radius and ≥ the hard point's minimum range (+0x23c)) fails → warning "unable to fire at target".
   Else 0x3822e0 assigns the target (hard point +0xc0, firing player +0xc8, frame +0xcc).
6. On every success: destroy-after sweep for **the player argument**, return the weapon object itself.

`SpecialWeaponsAllowed(playerData)` (0x527620) [H]: false if the battle planet is destroyed
(planet +0xb8 → +0x2c8); in a stand-alone tactical game (0x28afb0: skirmish) false unless the player's
`Enable_Special_Weapons` modifier list (+0x1e0, AND-combined) is true and +0x2f0 is set [M names];
true otherwise (campaign).

### 1.2 `GameObjectWrapper.Fire_Tactical_Superweapon()` 0x576a40 and `Is_Tactical_Superweapon_Ready()` 0x57c4d0 [H]

Both need TACTICAL_SUPER_WEAPON (0x2c) (else error "%s is not a tactical superweapon"; dead object →
nil). Ready = TSW state (data pack obj +0x148, +0x20) == **4**. Fire (0x42a200) returns true only when:
the battle planet exists in the parent (galactic) store, is not destroyed (+0x2c8) and its type has
`Planet_Is_Destroyable_By_TSW` (+0xbf) (0x42a720), state == 4 and a target dummy is set (+0x10). Then:
0x2ccf30 (start the firing sequence for the cinematic/camera manager), state := 5, countdown (+0x30) :=
int(30 × `TSW_Power_Up_Countdown_Seconds`), play `TSW_SFXEvent_Activate_Voice`,
`TSW_SFXEvent_Weapon_Power_Up` and the type's +0x21a0 event, and notify the victory monitor
(0x341fe0). Otherwise false.

TSW states (vf6 0x42a7b0) [M names]: 0 idle (find the target dummy with
DUMMY_TACTICAL_SUPER_WEAPON_TARGET 0x64 → 2), 1 search, 2 target acquired → 3, 3 arming (progress
0x42b560) → 4 + "superweapon ready" speech (own / enemy text by relation), **4 ready**, 5 power-up
countdown → 6 → 7 attack animation (anim 5) + `SFXEvent_Fire`, 8 countdown
`TSW_Attack_Anim_Duration_Seconds` → 9 planet destruction (debris type picked with the synced RNG
0x1ffb40 when the planet lists several), 10+ aftermath. On its first service, when a target dummy exists the TSW is
teleported toward it to a distance of +0x213c, or `TSW_Attack_Distance_From_Target` (+0x2140) when the
GMM predicate 0x28aec0(owner) holds [M: owner is the battle attacker]; in that case it starts directly in
state 4 with the target set, otherwise it acquires the target (state 2). Network command 38 (COMMANDS.md)
drives the same transitions for players.

### 1.3 `Spawn_Special_Weapon(type_name, player)` 0x6aa790 [H]

Space mode only (error otherwise); exactly 2 args. The type must exist, have `Is_Special_Weapon_In_Space`
(+0x1e) and `Special_Weapon_Index` (+0x1de4) ≥ 0 (errors otherwise). Player must be in the game. Takes the
**first** object of the mode with DUMMY_SPECIAL_WEAPON_SOURCE (0x63) (none → error "unable to find an
object … with SPECIAL_WEAPON_SOURCE behavior"), creates the type for that player at that object's
position (+0x78) and orientation (+0x84) (0x29f810, flag 1), registers it with the mode (0x35c520), and
returns the new object.

### 1.4 `LandTaskForceClass.Fire_Special_Weapon(type_name, target)` 0x6eb7f0 [H]

Arg 1: an object type that has LOBBING_SUPERWEAPON (0x370320(type, 0x37)) — else error "not a land
special weapon". Arg 2: AI target or game object (dead → warning "target is already dead", nil);
position = the object's position, else the AI target's. Walk the task force's unit list (+0x78) in
order: the **first** unit whose original type (0x3973b0) is that type, has 0x37 and is ready fires at the
position (0x4ea3f0); destroy-after sweep for that unit's owner; return that unit. No ready unit →
returns **false**.

### 1.5 `SpaceTaskForceClass.Fire_Special_Weapon(type_name, target)` 0x6e6ad0 [H]

Arg 1: a type with `Is_Special_Weapon_In_Space` (+0x1e) — else error. Arg 2: an AI target that has an
object, or a live game object — else error "Expected game object". Upgrade gate (§1.1) for the task
force's player (0x5f7020): failure → warning, returns false. The target type's category mask (+0x1648)
must intersect `Special_Weapon_Valid_Targets` (+0x1df0), else returns false. Walk the unit list: the
first unit of that original type with SPECIAL_WEAPON (0x2f) that is ready: begin targeting (failure →
error), fire (0x491cd0; failure → error), destroy-after sweep for its owner, return that unit. None →
false.

### 1.6 Internals shared by the above [H]

- **Begin targeting** (0x491b40, SPECIAL_WEAPON): if the pack's targeting flag (+0x28) is already 1,
  clear it and fail (a second call toggles off). In space mode, fail (with a local-player "already
  targeting" sound) if another object of the same owner with SPECIAL_WEAPON already has +0x28 = 1.
  Else +0x28 := 1, success.
- **Fire at object** (0x491cd0): needs pack ready, both types, target not flagged (+0x3a0 & 0x40), target
  **not allied** to the weapon owner, target categories ∩ `Special_Weapon_Valid_Targets` ≠ 0, target ≠
  self. Fires the weapon event (0x549d70 / 0x54ade0), plays `SFXEvent_Fire` at the weapon, restarts the
  pack (0x491770: start = now, ready = now + recharge (0x373630), with `Projectile_Fire_Pulse_Count` /
  `Projectile_Fire_Pulse_Delay_Seconds`), +0x28 := 0, and plays the faction speech
  `Special_Weapon_Target_Action_Index` (+0x1de8) for the local player (own / allied list faction +0x2518,
  enemy list +0x2560).
- **Fire lobbing at position** (0x4ea3f0): needs the owner's tactical data and, when the type has
  `Requires_Base_Power` (+0x8b), base power (0x527520). Writes the target position into the combatant
  pack (+0x30..+0x38) with the fire request (+0x3c := 1), +0x28 := 0, start = now, ready = now +
  int(30 × recharge); starts the ABILITY_COUNTDOWN display (behaviour 0x46, ability 0x2f) if present.
  No range, ally or fog check.

### 1.7 `GameObjectTypeWrapper.Is_Affected_By_Laser_Defense()` 0x605bc0 [H]

True iff the type's `Projectile_Category` (+0x1fe8) is MISSILE (1), ROCKET (4) or MPTL_ROCKET (6)
(bit mask 0x52). Hard-coded; it does not read any `Projectile_Types_Targeted` list (the shipped laser
defence lists the same three).

---

## 2. Ability state queries

### 2.1 `GameObjectWrapper.Is_Under_Effects_Of_Ability(name)` 0x57c600 [H]

Exactly 1 string arg, mapped through the UnitAbilityType enum (0x140b310c0; unknown → error). Only two
abilities are supported:
- **TRACTOR_BEAM** (30): the object's combatant pack (obj +0xf0) flag +0x132, set on the **victim** by
  `TractorBeamAttackAbilityClass` apply (0x710710) → true while being held.
- **ION_CANNON_SHOT** (41): if the object has ION_STUN_EFFECT (0x45): true iff that behaviour's disable
  counter (+0x3c) is 0 (i.e. the stun effect is active) [M meaning]; no such behaviour → false.
- Any other name → error "ability %s is not yet supported" (no value). The L1 mapping for SENSOR_JAMMING,
  STEALTH, INTERDICT and the "else active" fallback is not in the original.

### 2.2 `GameObjectWrapper.Are_Engines_Online()` 0x572d90 [H]

No locomotor data (obj +0xa8) → true. Else the locomotor byte +0xd0. It is written by
`Locomotor_Set_Engines` (0x5545a0): engine hard point destroyed → +0xd0 = 0 and +0xd1 = 1 (permanent);
disable for t seconds → +0xd0 = 0 with re-enable frame +0xd4 = max(+0xd4, now + int(30 t)); t < 0 →
off indefinitely (TACTICAL_COMBAT §3 "System offline"; energy-pool drains). So it is **false while
engines are disabled** (destroyed engines or a timed disable), not "no ENGINE hardpoint".

---

## 3. Cinematic transport and retreat

### 3.1 `Create_Cinematic_Transport(type, player_id, pos, z_angle, landing_mode, anim_delta, idle_time, persist [, hint])` 0x5414b0 → 0x44af20 → 0x2cb7a0 [H]

Land tactical only (errors otherwise); fewer than 8 args → error. Argument handling:

| # | Name | Type | Default if wrong type | Meaning |
|---|---|---|---|---|
| 1 | type | string (required) | error | object type name (must exist, else nil) |
| 2 | player_id | number | 0 + error message | owner id (int) |
| 3 | pos | position-convertible (required) | warning, nil | landing position |
| 4 | z_angle | number | 0 | facing (z rotation) given to the spawn |
| 5 | landing_mode | number | 1 | start phase of the landing state machine (below) |
| 6 | anim_delta | number | 0 | start point inside the Land / Takeoff animation, fraction 0..1 |
| 7 | idle_time | number | 2.0 | seconds to stay landed |
| 8 | persist | number | false | `> 0` → never takes off |
| 9 | hint | string, optional | error message, created without a hint | hint name to attach |

Wrong-typed args 2 and 4–8 only log the error and keep the default; arg 3 without a position logs a
warning and returns nothing.

Body (0x2cb7a0): the type must have TRANSPORT (0x05) and type byte +0x65 must be 0 [M: the field table
names +0x65 `Has_Space_Evaluator`]. z := max(z, terrain height, water height at pos) + `Hover_Offset`
(+0x1b68). Create the object (0x29f810, facing (0, 0, z_angle)); attach TRANSPORT_LANDING (0x36) by name
if missing; in its landing pack (obj +0xc8): cinematic flag +0xbd := 1, landing mode +0x50, anim delta
+0xc0, idle time +0xc4, persist +0xc9, initialised +0xc8 := 0. With a hint: attach HINT, set the hint
name and register it (0x29bc30). Returns the transport object.

**Cinematic landing state machine** (`TransportLandingBehaviorClass` service 0x643c30) [H code, M names]:
- First service with +0xbd set and not initialised (0x644040), by landing_mode:
  0, 1 or 4 → start landing (state 1, 0x642c70) and play **Land** (anim 43) from frame
  int((frames − 1) × anim_delta); 2 → already landed: Land at its last frame, idle timer := idle_time;
  3 → taking off: **Takeoff** (anim 44) from frame int((frames − 1) × anim_delta), timer 0; other →
  nothing. Then initialised := 1.
- State 1 (landing): when the animation passes 98 % → state 2; cinematic: idle timer := idle_time
  (non-cinematic transports instead unload their cargo, 0x643130, with a timer of 2 s × (1 + player
  modifier)).
- State 2 (landed): when the timer expires — cinematic and not persist → state 3 and play Takeoff at
  speed 1.15; persist → stays landed. (Non-cinematic: unload steps 0x644330.)
- State 3 (taking off): past 50 % the cargo link (+0x58) is released; past 98 % the transport's parent
  id is cleared and it is destroyed.
So shipped `Create_Cinematic_Transport("Shuttle_Tyderium_Landing", id, pos, 0, 1, 0.25, 20, 1)` lands
from 25 % into the Land animation and stays (persist).

### 3.2 `Start_Cinematic_Space_Retreat(player_id [, delay_seconds])` 0x544ae0 [H]

Space tactical only (errors otherwise); arg 1 number (int player id; wrong type → error message, id 0);
arg 2 optional number: delay = int(seconds × 30). Uses the mode's retreat coordinator (mode +0x30;
vtable +0x30 must say "space coordinator", else error). Then:
1. coordinator vtable +0x40 (player): build the retreating-units list (BATTLE_TRANSITIONS §3.4 vf8);
2. 0x534110 arranges it: sort by a type speed value (0x3727a0, descending) [M], find the MARKER named
   "ATTACKER ENTRY POSITION", stop each unit's locomotor, give it a formation slot around that marker
   (0x2a9670, spacing 2500), turn it to face away (marker facing + 180°) and **teleport** it there
   (0x3a8f90); re-sort the list by distance to a point 10000 units out along that facing;
3. for each unit in that order: obj +0x3c9 := 1 (the "in end cinematic" flag that Lua
   `In_End_Cinematic` also sets; it keeps hyperspaced objects from being hidden/removed, 0x38ed10), and
   locomotor vtable +0x2a8(0, delay) = hyperspace out after `delay` frames; delay +=
   `Space_Retreat_Unit_Increment_Wait_Frames` (player faction +0x20c0).
The coordinator state machine is not started, so there is no countdown, no post-retreat destruction
and no retreat victory [M]. Returns nothing.

---

## 4. Story event types

All rules below are the original vf2 (parameter parsing) and vf3 (evaluation) bodies. Names in lists are
upper-cased at parse time (except where noted) and compared with the object type name by exact length +
memcmp (type names are stored upper-case). Fire = STORY.md §1.5 (no armed / triggered check inside
Fire).

### 4.1 STORY_SPACE_TACTICAL (0x18) / STORY_LAND_TACTICAL (0x19) — `StoryEventStartTacticalClass` [H]
- Params: **Event_Param1** = Story_Plots file name to load for the battle (+0x378, kept as written);
  **Event_Param2** = planet names (list, upper-cased, +0x360).
- Dispatch: when a space / land tactical mode is created (0x4d7820 / 0x3b81b0) and the game is **not
  multiplayer** (GMM +0x18 not in {1, 2, 4}) and neither 0x140b15490 nor sync-test playback 0x140b1574e
  is set: dispatched to the **current mode's** story manager (the galactic one while the battle is
  being created), all players' plots, data1 = the new tactical mode's story manager (mode +0xa0),
  data2 = the battle planet object (0x140b30428, set by the battle start 0x28fda0).
- vf3 (0x455710): nothing if data1 is null. If Param2 is empty or data2 is null: Fire(no object), then if
  Param1 is non-empty load that Story_Plots file into data1 for the plot's player (0x329aa0). Else for
  each Param2 name equal to data2's type name: Fire(data2) and load Param1 likewise (once per matching
  entry).
- Note STORY.md §4.1: these types are dropped, not queued, when sent to a non-running mode.

### 4.2 STORY_VICTORY (0x2f) — `StoryEventVictoryClass` [H]
- Param1 = faction name, resolved at parse time to the faction object (0x3303d0, `_stricmp`). An
  **empty Param1 is never parsed**, so the faction stays null and vf3 logs "couldn't find the player at
  startup" and **never fires** (4 shipped sandbox events have an empty Param1). Unknown faction →
  logged, never fires.
- Dispatch (0x32f3e0 from the victory monitor 0x343b90): when the monitor declares a winner with
  victory type 11..22, STORY_VICTORY is dispatched to the current mode's story with data1 = the winning
  player's faction (player +0x68). vf3: fire iff data1 == the event's faction.
- Same call: if the monitor has a MISSION_LOST name and the winner is not the local player and the
  parent mode is galactic, STORY_MISSION_LOST (0x32) is dispatched to the galactic story with that name.

### 4.3 STORY_COMMAND_UNIT (0x25) — `StoryEventCommandUnitClass` [H]
- Params: Param1 = unit type names (+0x360), Param2 = target type names (+0x378), Param3 = distance
  (atof, +0x390, default 0).
- Dispatch (0x32ea20, from the move-order executors 0x3ad100, 0x3ae090, 0x3ae6c0, 0x3aebb0): for each
  unit of the order's selection set owned by the **local player**: data1 = the unit's type name,
  data2 = the order's destination (x, y, z). Sent directly to every active plot of the mode's story
  manager when that manager's mode is the running mode and the story is live
  (*0x140b2b5d0, multiplayer, or 0x140b1574e), else queued as a delayed event; skipped when 0x140b155c4
  is set or 0x140b2b5d0 is null.
- vf3 (0x454190): for the first Param1 entry equal to data1: if Param2 is empty or Param3 == 0 → Fire.
  Else for each Param2 type, every object of that type in the current mode: Fire if the 3D distance
  from the object to the destination is **< Param3**. (Shipped: tutorial "move the Nebulon-B within
  4000 / 2100 of Empire_Star_Base_1".)

### 4.4 STORY_GUARD_UNIT (0x29) — `StoryEventGuardUnitClass` [H]
- Params: Param1 = guarding unit types (+0x360), Param2 = guarded target types (+0x378).
- Dispatch (0x32ee40, from the guard order 0x3c100): per local-player unit of the selection: data1 =
  unit type name, data2 = the guarded object's type name; same direct / queued rule as §4.3.
- vf3 (0x454e70): for each i with Param1[i] == data1, compare **Param2[i]** (the same index, repeated
  count(Param2) times) with data2 → Fire. So entries pair by position; an empty Param2 never fires;
  i ≥ count(Param2) reads past the list (undefined in the original — treat as no match).

### 4.5 STORY_DESTROY (6) and STORY_TACTICAL_DESTROY (8) — `StoryEventDestroyClass` [H]
- Params: Param1 = destroyed type names (+0x360, each with a counter = 1); Param2 = second name list
  (+0x378, upper-cased, not validated); Param3 = counts (atoi per entry, written into counters and
  their reset copies; the arrays are re-sized only when Param3 has more entries than they hold);
  Param4 = Event_Filter (default **0 = FILTER_NONE**, ctor).
- Dispatch:
  - TACTICAL_DESTROY (0x32e7d0, object death in tactical): data1 = the dead object's type, data2 = 0;
    again with its original type (0x3973b0) when different; and, when the object has TRANSPORT (0x05),
    once per contained object whose type has flag +0x13, unless equal to the original type.
  - DESTROY (0x327070, galactic): only for objects inside a container chain that reaches a PLANET
    (0x03); data1 = type (and original type), data2 = that planet.
- vf3 (0x454780): i = first Param1 entry equal to data1's name (none → return).
  - Param2 empty: counter[i] −= 1; Fire(data2) when counter[i] ≤ 0. **No filter is applied.**
  - Param2 non-empty: for the first j with Param2[j] == data2's type name **and** the filter passing on
    data2: counter[**j**] −= 1 (the Param2 index, not i); Fire when ≤ 0.
  - Consequences: TACTICAL_DESTROY with a non-empty Param2 **never fires** (data2 is null), and its
    filter is never used. For DESTROY, Param2 is a planet list and the filter tests the planet's owner.
- Reset (vf4 0x45e4b0): counters := reset copies.
- Filter (0x455ce0) compares **factions**, not players: plot player's faction vs the object owner's
  faction (0x3956c0 → 0x331b70): 0 any; 1 same faction; 2 owner faction neutral; 3 different and
  not neutral; 4 neutral or same; 5 neutral or different; 6 not neutral.

### 4.6 STORY_DIFFICULTY_LEVEL (0x2b) — `StoryEventDifficultyClass` [H]
- Param1 = "Easy" / "Normal" / "Hard" through the DifficultyLevelType enum (0x5de400: 0, 1, 2; unknown →
  warning "Difficulty level %s unrecognized").
- vf3 (0x454940): Fire iff the global difficulty 0x140b15498 (static default 1) equals Param1.
- **No dispatcher**: no call site sends type 0x2b (all 51 callers of the dispatch functions checked;
  the type bucket is never read by a poll). The event never fires in 1.121. No shipped event uses it.
  Port rule: parse it, never evaluate (or evaluate once at plot activation only as a documented [port]
  extension).

### 4.7 STORY_CORRUPTION_CHANGED (0x36), CORRUPTION_TACTICAL_COMPLETE (0x3a), _FAILED (0x3b) — `StoryEventCorruptionLevelClass` [H]
- Params: Param1 = planet names (+0x368, upper-cased); Param2 = corruption type name through the
  CRC-keyed story keyword map (0x140b30728; not upper-cased): CORRUPTION_NONE −1, INTIMIDATION 0,
  PIRACY 1, KIDNAPPING 2, RACKETEERING 3, CORRUPT_MILITIA 4, BRIBERY 5, BLACK_MARKET 6,
  BONDED_CITIZENS 7, CORRUPTION_ANY 8; unknown → 0. Default (ctor) **8 = any**.
- vf3 (0x4544e0)(planet, &type): match when type == Param2, or Param2 == 8 and type ≠ −1; then Fire(planet)
  if the planet's type name is in Param1 (an **empty Param1 never fires**).
- Dispatch:
  - CHANGED (0x326e50, from 0x3fc5b0): planet corruption changes, data = (planet, new type).
  - TACTICAL_COMPLETE / FAILED (0x326f20 / 0x326f50 from 0x28bf10, GMM battle end): when a corruption
    mission is pending (GMM +0x1f4 = corrupting player id, +0x1f8 = corruption type): if the battle
    winner is that player → apply the corruption to the planet (0x3f6810), player stat, local-player
    message, then **COMPLETE**; otherwise **FAILED**; both with (planet of the battle, type), sent to
    the galactic story manager (GMM +0x38 → +0xa0). The pending mission is then cleared (+0x1f4 := −1).
  No shipped FoC event uses 0x3a / 0x3b.

### 4.8 Also [inferred] in wave 2a: UNIT_PROXIMITY (0x2a) and CHECK_DESTROYED (0x2e) [H]
- **UNIT_PROXIMITY**: polled each T9 for every local-player object without PROJECTILE (0x08), restricted
  to plots of the local player, data = the object. vf3 (0x455480): if Param1 is empty, test that
  object; else only when its type is in Param1. Test (0x452030): for each Param2 **type** name, every
  object of that type in the mode: Fire if the 3D distance < Param3 (+0x390). Hint names are not looked
  up (a non-type name finds no objects).
- **CHECK_DESTROYED** (vf2 0x463c90, vf3 0x453ce0): Param1 = faction name → the first player of that
  faction (unknown → logged; vf3 does nothing without it); Param2 keyword: DESTROY_ALL 1,
  DESTROY_ALL_UNITS 2, DESTROY_ALL_STRUCTURES 3, DESTROY_ALL_INDIGENOUS_SPAWNERS 4; Param3 = object type
  (for 4). Polled each T9. Fire when:
  1: the player has no SELECTABLE (0x02) object of a counting type (0x372230) that is alive (not
     +0x3a0 & 0x42);
  2: same but objects with behaviour 0x62 (DUMMY_LAND_BASE_LEVEL_COMPONENT) are ignored (units only);
  3: the player has no 0x62 object, or exactly one and it is dying (+0x3a0 & 0x42);
  4: no live object in the global list 0x140a43690 [M: indigenous spawners] has type Param3 (with no
     Param3, any live entry blocks).

---

## 5. Other globals and methods

### 5.1 `GameObjectWrapper.Get_Parent_Mode_Object()` 0x5793f0 [H]
obj +0x54 = the id of the matching object in the parent mode (0x3fffff = none). The parent mode is the
entry after the current mode in the GMM mode stack (0x28aa00 returns the next element); look the id up
in that mode's store (+0x18, 0x2aca60) and return a wrapper bound to it (0x5751d0), else nil. The
wrapper must be able to reference an object of another store (the port's "one store per wrapper set"
blocker). `Get_Parent_Mode_Object_ID` (0x579480) returns the raw id (OBJECT_RUNTIME).

### 5.2 `Apply_Markup(player, target, amount [, handle])` 0x69d110 [H]
A real global. 3 or 4 args (else error). Arg 1 player (else error). Arg 3 number (else error).
- Handle: arg 4 if it is a `MarkupBlockStatus` (and there are exactly 4 args), else a new one (0x69d0a0).
  Reusing a handle first **removes its previous markup**: every recorded target entry gets
  value (+0x80, a double) −= the handle's previous amount (+0x40), and the record list is cleared.
- The handle's amount := arg 3. Then by the type of arg 2 (checked in this order):
  - game object → the player's AI (player +0x360) context for the current mode type (0x4b06d0) → its
    target registry (+0x38) → the AI target of that object (planets: planet data +0x354; other objects:
    looked up by name in 0x140b310a8) (0x69cea0);
  - AI target → that target (0x69d000);
  - player → that player's target (player +0x478) in the caller player's registry (0x69cb00);
  - table → each element (0x69cbb0).
  Each affected AI target: value (+0x80) += amount (as double), recorded in the handle (0x69ca70).
- Returns the handle. Wrong arg 2 type → error. The value is the AI perception **Markup** token of
  that target (AI_GALACTIC / A2: "Markup = 0" placeholder). Shipped use: `PathBonusForDistantTargets.lua`
  keeps one handle per planet and re-applies it each pass.

### 5.3 `FogOfWar.*`, STORY_FOG_*_REVEAL
See FOG_OF_WAR.md §6.

---

## 6. Corrections to other docs

1. PHASE4 wave 2a [inferred] mappings: Is_Under_Effects_Of_Ability supports only TRACTOR_BEAM and
   ION_CANNON_SHOT (§2.1); Are_Engines_Online is the locomotor engines flag, false while disabled (§2.2);
   TACTICAL_DESTROY's Param2 is a second name list (planet list for DESTROY) that makes the tactical
   event unfireable, its Param3 are per-entry counts, Param4 the filter (§4.5); VICTORY with empty
   Param1 never fires (§4.2); STORY_*_TACTICAL load Param1 for the plot's player into the new mode's
   story (§4.1); UNIT_PROXIMITY has no hint lookup (§4.8).
2. STORY.md §5 FILTER table: the comparisons are by **faction** (§4.5).

---

## 7. Open items

1. Which battle-record slot (+4 / +8) is attacker vs defender (TSW auto-ready side §1.2, setup reveal
   FOG_OF_WAR §3.4).
2. The +0x21a0 TSW type field name and the victory-monitor notification 0x341fe0.
3. `Enable_Special_Weapons` modifier list (+0x1e0) and the +0x2f0 byte in the player tactical data.
4. Locomotor vtable +0x2a8 (cinematic hyperspace-out) timing and what clears obj +0x3c9.
5. The global list 0x140a43690 used by CHECK_DESTROYED case 4.
6. Victory types 11..22 that dispatch STORY_VICTORY (victory monitor enum, BATTLE_TRANSITIONS).

---

## 8. Exact rules for code agents (short report)

1. Fire_Special_Weapon(target, player): three paths (lobbing → fire at position, special weapon → begin
   targeting + fire at a non-allied object of a valid category, space station → hard-point target with
   range / arc / FOW(force 1) checks); "not ready" and "upgrade" cases are warnings returning nil;
   success sweeps DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED for the player argument and returns self.
2. Task-force variants fire the first ready member of the named type and return it, else false.
3. Fire_Tactical_Superweapon fires only from TSW state 4 with a target and a TSW-destroyable, intact
   battle planet; Is_Tactical_Superweapon_Ready = state 4.
4. Spawn_Special_Weapon: space only, type needs Is_Special_Weapon_In_Space and Special_Weapon_Index ≥ 0;
   spawn at the first DUMMY_SPECIAL_WEAPON_SOURCE.
5. Is_Affected_By_Laser_Defense = Projectile_Category in {MISSILE, ROCKET, MPTL_ROCKET}.
6. Is_Under_Effects_Of_Ability: TRACTOR_BEAM (victim flag), ION_CANNON_SHOT (ion-stun behaviour
   enabled); anything else is a script error. Are_Engines_Online = locomotor engines flag (true without
   locomotor).
7. Create_Cinematic_Transport: land only, 8 or 9 args, defaults per §3.1 table; landing state machine
   start phase from landing_mode, persist keeps it landed. Start_Cinematic_Space_Retreat: line the
   player's retreatable units up at "ATTACKER ENTRY POSITION" and hyperspace them out one by one with
   delay += Space_Retreat_Unit_Increment_Wait_Frames.
8. Story: SPACE/LAND_TACTICAL load Param1 plots for matching Param2 planets (single player only);
   VICTORY = winner faction == Param1 (empty never fires); COMMAND_UNIT / GUARD_UNIT come from local
   player orders (distance < Param3; GUARD pairs by index); DESTROY / TACTICAL_DESTROY per §4.5;
   DIFFICULTY_LEVEL is never dispatched; CORRUPTION_TACTICAL_COMPLETE / FAILED fire at battle end of a
   pending corruption mission depending on whether the corrupting player won.
9. Get_Parent_Mode_Object needs a cross-store wrapper; Apply_Markup adds `amount` to AI target markup
   values, undone by reusing the returned handle.
