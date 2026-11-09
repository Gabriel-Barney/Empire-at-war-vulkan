# Runtime game objects: identity, lifetime, containment, behaviours, tick order (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent for roadmap step **S1** (game object model: stable entity IDs, versioned
references, an object store with one mutation owner). Clean-room: prose and tables only, no code or pseudo-code
from the decompilation. Addresses are RVAs in StarWarsG.exe; renamed function names in `docs/re/ghidra/` are
often wrong, so the address is authoritative. How `GameObjectType` XML is parsed is covered by
`docs/re/OBJECT_TYPES.md` (another agent) and is not repeated here; this file starts where a type exists.

Confidence: **[H]** read in the code and cross-checked, **[M]** read in the code, intent inferred,
**[L]** inferred or only partly traced, needs more RE or a probe.

---

## 0. Summary for the implementer

1. **One C++ class for every object in every mode.** Planets, fleets, galactic heroes, starships, infantry,
   projectiles, particles, hardpoints, markers and UI props are all `GameObjectClass` instances. What an object
   *is* comes from its type plus the behaviours attached to it. [H]
2. **Every game mode owns two object stores** (`GameObjectManagerClass`, "GOM", 0x640 bytes):
   - the *primary* store (mode +0x18) holds all synchronised simulation objects;
   - the *secondary* store (mode +0x20) holds local cosmetic objects (move compass, locally-visible effect copies).
   
   Galactic, land and space each have their own pair. [H] for the two stores, [M] for the meaning of the
   secondary store.
3. **Object IDs** are signed 32-bit ints at object +0x50.
   - Primary stores draw IDs from **one counter shared by all modes** (GameModeManager +0x80, reset to 1 at game
     start), so an ID is unique across galactic and tactical objects alive at the same time. Secondary stores use
     their own per-store counter (GOM +0x620).
   - IDs are **post-incremented and never reused** within a session.
   - `0x3FFFFF` means "no object".
   - IDs are saved and restored with the object.
   
   [H]
4. **References are raw pointers protected by intrusive reference counts plus a "dead" flag.** There are no
   generational handles.
   - Containers that own an object (store lists, Lua wrappers) take a reference.
   - On death the object is flagged (+0x3A0 bit 0x02), broadcast (signal 0x10), and actively cleared out of every
     other object's pointer fields.
   - The memory lives until the last reference drops.
   
   [H]
5. **Destruction is deferred to the end of the store's update.** Killing an object flags it and queues it in the
   store's pending-delete vector at once. Unlinking from all store lists, trees and the ID map happens in the
   flush at the end of that store's `Update`. [H]
6. **Behaviours are a per-object array (max 127) with a 0x6A-entry type→slot index.**
   - Serviced behaviours sit at the front and are ticked in attach order. Attach order is: the type's `Behavior`
     list, then the mode-specific list (`GalacticBehavior` / `LandBehavior` / `SpaceBehavior`), then runtime
     additions.
   - Per-behaviour state lives in "data packs" at fixed object slots.
   
   [H]
7. **Only the active mode ticks.** During a tactical battle the galactic mode is not updated. Each mode has its own
   frame counter (mode +0x10). [M]
8. **The frame CRC is order-independent.** It is the sum over live primary-store objects of a CRC32 of
   (position xor facing bits) and health, plus the RNG and player CRCs. The secondary store is not checked. [H]
9. **Iteration orders that matter** are listed in §6. The two to copy exactly:
   - store lists are walked newest-first, and so Lua `Find_All_Objects_Of_Type` returns newest-first;
   - behaviours are ticked in attach order.
   
   [M]

---

## 1. Object creation, IDs and handles

### 1.1 Factory

- **Entry point:** `GameObjectManagerClass::Create_Object` 0x29f810, called from about 90 sites. Arguments are
  (store, type, owner player id, position, facing, two flags). [H]
- **Allocation:** 0x8A0 bytes per object. The class is declared `PooledObjectClass<GameObjectClass,64>`. [H]
  - The `min_size 0x23b0` in STRUCTS.md is polluted by type-object accesses. **The real object size is 0x8A0.**
- **Construction (0x388b60, base part 0x388720).** [H] Steps, in order:
  1. Set ID, parent-mode ID and owner. The ID is allocated by 0x2ac980.
  2. Set the type pointer (+0x298), the owning store (+0x2B8), position (+0x78) and facing (+0x84). One flag adds
     the type's height offset (type +0xDFC) to z. The other flag sets +0x3A0 bits 0xC0.
  3. Attach behaviours (§4.2).
  4. If a serviced behaviour was attached, register in the store's serviced list.
- **Store registration (back in 0x29f810).** [H] The new object is added to:
  - the all-objects list (GOM +0x38);
  - the ID map (GOM +0x80);
  - the per-owner list (GOM +0xC8 + owner×0x48);
  - the per-owner and global spatial trees (GOM +0xD0 + owner×0x38, GOM +0xD8), but only if the type is
    collidable (type +0x41, or +0x42 for death clones);
  - category lists, each holding a reference ([H] for the list/condition pairs, [L] for the names):

    | GOM list | condition |
    |---|---|
    | +0x2A0 | has SELECTABLE |
    | +0x560 | has PLANET |
    | +0x5A8 | named heroes (type +0x13) |
    | +0x408 | HINT (type has behaviour 0x4A); no reference taken |
    | vector +0x3C0 | TACTICAL_SUPER_WEAPON |
    | +0x330 | TRANSPORT_LANDING |
    | +0x378 | DUMMY_UPGRADE |
    | +0x1C8, +0x210, +0x258, +0x2E8 | type-data predicates |

- **Signals:** a "created" signal 0x1C goes out from the store, except for PROJECTILE and PARTICLE objects. [H]
- **Delayed creation queue (GOM +0x5F0).** Entries are 0x30 bytes: due frame, type, owner, position, facing,
  flags. They are created in the store's update once due (§5.2). Respawn-after-death (type +0x21D0 seconds) uses
  this queue via 0x2bd230. [H]

### 1.2 ID allocation

- **Allocator 0x2ac980.** [H]
  - If the store's "use global IDs" byte (GOM +0x63E) is set, it defers to the GameModeManager allocator
    0x28a9e0, whose counter is at manager +0x80 = global 0x140b15460.
  - Otherwise it uses the store's own counter at GOM +0x620.
  - Both return the current counter and then increment it.
  - Edge case: when the counter reaches 0x3FFFFE, the value returned is 0x3FFFFF (the "none" sentinel). This is
    a latent bug after about 4.19 million objects. A port should simply never hand out 0x3FFFFF.
- **Which store uses which counter.** `GameModeClass` construction (vf1 0x35e560, rebuild path in vf13 0x35f970)
  creates both stores. The primary gets "global IDs = 1"; the secondary gets 0. [H]
- **Reset:** the global counter is reset to 1 by six game-start paths (0x16fa0, 0x69040, 0xbd380, 0xc7680,
  0xc7ca0, 0xef700). [H]
- **Save/load:** whether the counter itself is saved was not traced. Since IDs are saved and restored with
  objects (§2.4), a port must save it. [L]

> **Status 2026-10-30:** The port saves the counter: `ObjectStore::write` stores `ids_->next()` (`engine/objects/src/object_store.cpp:470`) and `ObjectStore::read` restores it with `ids_->set_next` (`object_store.cpp:514, 687`).

- **No reuse:** IDs are never reused while the counter runs. [H]
- **Other consumer:** 0x553f00 stores (counter mod N) into an object at creation, probably a service-phase
  stagger. This value is therefore part of the deterministic state. [M]

### 1.3 ID → object lookup

- **Map:** each store keeps an `unordered_map<int, GameObject*>` at GOM +0x80. Its list sentinel is at +0x88, the
  bucket vector at +0x98 and the mask at +0xB0. [H]
- **Hash:** the hash of id is one Park–Miller "minimal standard" step (16807 mod 2³¹−1) applied to
  `(id XOR 0xDEADBEEF) & 0x7FFFFFFF`. [H]
- **Uses:**
  - lookup 0x2aca60;
  - erase on removal (0x2c07e0 inside 0x2a6a00);
  - insert on creation.
  
  No iteration over this map was found on simulation paths. [M]
- **Port:** any O(1) ID→slot table works, as long as it is never iterated for game logic.

### 1.4 Parent-mode ID (+0x54)

- **What it is:** a tactical object spawned from a galactic object stores that galactic object's ID at +0x54.
  Otherwise +0x54 is 0x3FFFFF. [H]
- **"Parent mode":** the neighbouring entry on the GameModeManager mode stack (array at manager +0x48, count
  +0x50). The lookup is 0x28aa50, which returns that mode's primary store (+0x18) and then looks up by ID. [H]
- **Why it is unambiguous:** IDs are global across primary stores. [H]
- **Lua:** exposed as `Get_Parent_Mode_Object_ID` (0x579480) and `Get_Parent_Mode_Object` (0x5793f0). [H]

### 1.5 What Lua sees

- **Handles:** a Lua object handle is a `GameObjectWrapper` userdata (pooled, 0x78 bytes). +0x60 holds the raw
  `GameObject*`. Constructor/attach is 0x57ac10. [H]
- **Attaching a wrapper:** [H]
  - takes a reference on the object;
  - subscribes the wrapper's listener (+0x28) to the object's signal generator (object +0x38) for **signal 0x10**;
  - caches the object's position wrapper at +0x58.
  - If the object is already dead (+0x3A0 bit 0x02), it releases at once and leaves +0x60 null.
- **Wrapper reuse:** wrappers are cached per (object pointer, script) in the map at global 0x140b39380, so one
  script gets the same userdata for the same object. This map is pointer-keyed. It is only used for lookup in
  the traced code. [H] for the key, [M] for "lookup only"
- **After death:** when the object dies, signal 0x10 nulls the wrapper's pointer. [M] (the signal is raised in
  0x3a8380 [H]; the wrapper-side handler was not read.) From then on:
  - methods fail with `"... -- this object is dead!"`;
  - `Is_Valid` (0x57c7d0) returns false. It tests only "+0x60 non-null".
- **IDs in Lua:** `Get_Object_ID` (0x5791f0) returns +0x50 as a number, or nil for 0x3FFFFF. It is registered but
  absent from the shipped docs. Scripts mostly hold wrappers, not IDs. [H]
- **Lua-visible order:** `Find_All_Objects_Of_Type` (0x540b20) walks the primary store's all-objects list of the
  current mode, newest-first (§6). Scripts commonly index `[1]` into the result, so a port must return
  **newest-first**. [M]

---

## 2. Lifetime

### 2.1 States and flags (object +0x3A0..+0x3A7)

| bit / byte | meaning | conf |
|---|---|---|
| +0x3A0 & 0x02 | dead/removed. Set by Detach 0x3a8380. Tested everywhere: `Get_Parent_Object` returns nil for such a parent; the store CRC skips it; Destroy returns early | H |
| +0x3A0 & 0x04 | "marked for death by child mode": a tactical counterpart died (§2.5) | H |
| +0x3A0 & 0x08 | skips the service pre-step when the type has +0x69 set | L |
| +0x3A0 & 0x40 / 0x80 | 0x40 = dying / dead (set by `Die` 0x39bdb0; selects the `*_Dead` collidability); 0x80 = death clone, set together with 0x40 only when the death-clone spawner 0x29f270 creates the clone (create flag). Neither is ever cleared (TACTICAL_COMBAT §5.4.3) | H |
| +0x39E | "registered in the serviced list" | H |
| +0x3A1 & 1 | needs the late update (0x3ac530) during service | M |
| +0x3A5 | "unlink from stores on flush" (constructor sets 1) | M |

### 2.2 Death and destruction sequence

**Kill (0x391310)** is the common "destroy this object" entry, with about 95 callers (Lua `Despawn`, damage, story,
AI, retreat, …). [H] It does the following:

1. Returns early if the object is already dead.
2. Runs death effects: death clone and explosion types, and pending delayed damage cleared.
3. Spawns `Destruction_Survivors`. The count is drawn from the **synchronised RNG** (0x1ffb40 on the game
   random stream) between type +0x1D48 and +0x1D4C.
4. Schedules a respawn when the type has a respawn time.
5. Notifies its garrison/container and the player.
6. Calls **`GameObjectManagerClass::Destroy_Object` (0x2a64f0)**. This step:
   1. returns if the object is already in the pending-delete vector (GOM +0x3D8), so it is idempotent;
   2. applies the parent-mode death marking when +0x54 is set (§2.5);
   3. takes a reference and appends the object to the pending vector;
   4. runs **Detach (0x3a8380)**: set dead bit 0x02, zero the serviced-behaviour count, stop the model, release
      special links, and raise **signal 0x10**;
   5. removes the object from the mode's selection manager (mode +0xA0) if it is selectable;
   6. for galactic-type objects, runs 0x2c0340.
7. If a parent container (+0x2B0) holds the object as a structure or orbital dummy, it calls the container's
   removal hooks (0x4bb420 / 0x4bb470).

**Flush (0x2a62d0)** is the last step of the store's update (§5.2). For every pending object with +0x3A5 set it
calls **`Remove_Object` (0x2a6a00)** on the current mode's primary and secondary stores, then clears the vector. Each
list removal releases one reference and clearing the vector releases the last store reference. [H]

**`Remove_Object` (0x2a6a00), in order** [H]:

1. Raise signal 0x28 on the object.
2. **Unless the object has PROJECTILE**, walk *every* object in the store and call 0x3920c0(other, dying). That
   clears in each other object:
   - its garrison data pack entries (+0xF8);
   - the targeting/attacker set in data pack +0x100;
   - `ParentContainerObject` (+0x2B0) if it equals the dying object;
   - +0x2C0 if equal;
   - the +0x1A8 data pack's +0x30 pointer;
   - and if `AttachParentObject` (+0x2E0) is the dying object, **the attached object is killed too**. This is
     how hardpoints and attached children cascade.
3. Unlink from these, releasing a reference for each list:
   - the serviced list, every category list and the per-owner lists;
   - all spatial trees;
   - the per-owner lists +0x1C0/+0x548/+0x550. When an owner's +0x550 list becomes empty, the player is
     notified (0x280c50).
4. Notify, in order:
   - two audio/SFX managers (0x2e9d60, 0x2d4250);
   - the mode (vtable +0x258);
   - the RetreatCoordinator (mode +0x30, 0x33fd40);
   - the BombingRunManager (mode +0x38, 0x492910);
   - an SFX manager (0x2cbb00).
5. In each per-owner ID group (GOM +0x540; records of 10 object IDs, probably control or hot-key groups [L]),
   replace the dying ID with 0x3FFFFF and free records that become empty. **These groups are weak references by
   ID.**
6. Call every function in the global removal-hook list (0x140a17060, count 0x140a17068).
7. Erase the ID from the ID map and remove the object from the all-objects list.

### 2.3 What happens to references held elsewhere

| holder | mechanism | conf |
|---|---|---|
| other GameObjects (container, attach parent, target sets, garrison) | O(N) pointer scrub in `Remove_Object` (§2.2 step 2) | H |
| Lua `GameObjectWrapper` | strong reference + signal 0x10 → pointer nulled → "this object is dead!" | H / M |
| per-player ID groups | ID slot overwritten with 0x3FFFFF | H |
| selection | removed at Destroy time (mode +0xA0, 0x32e7d0) | M |
| fleets / transports / teams (contents vectors in data packs) | raw `GameObject*` vectors; removal path not traced (probably signal listeners or behaviour hooks) | L |
| AI (task forces, goals, perception) | not traced in this pass; `TaskForceClass.Is_Valid` exists, which suggests refcount + dead-flag checks | L |
| galactic counterpart | parent-mode death marking (§2.5) | H |

### 2.4 Save / load

- **What is saved:** the object Save/Load (0x39ce40) writes the ID as micro-chunk 0 and the parent-mode ID as
  micro-chunk 0xF. [H]
- **Missing owner:** an object whose owner player id no longer exists is destroyed on load ("An object was saved
  with a Player ID that no longer exists!"). [H]
- **Store load (0x2b0a20):** reconstructs objects with the base constructor 0x388720 and their saved IDs. [H]
- **Pointers:** remapped through `SaveLoadClass::Map_Pointer` (see SUBSYSTEMS.md).
- **Port:** save IDs and the counters, and rebuild references by ID.

### 2.5 Parent-mode death marking (0x2b2cc0)

When an object with a parent-mode ID is destroyed in a tactical store, the counterpart in the parent mode is
found by ID. [H] (Not done while the store is shutting down, GOM +0x63D.)

**If the counterpart has the same type:**
- it gets +0x3A0 |= 0x04;
- its byte +0x330 is copied from the dying object (meaning not identified [L]).

**Otherwise the counterpart is a container:**
- a FLEET (FleetDataPack at +0xC0), or a TRANSPORT (TransportDataPack at +0xC8; galactic transports carry ground
  companies);
- the first not-yet-marked child of the same type gets the flag.
- The +0x330 bytes are reconciled by majority over the children.

**Exception:** for types with type +0xE3 set, marking is skipped while another live object of the same type and
owner still exists in the tactical store. [M]

**Lua:** `Mark_Parent_Mode_Object_For_Death` calls the same routine.

**When flagged galactic objects are actually destroyed on return to galactic was not traced.** [L]

> **Status 2026-10-30:** The port has the flag (`kFlagMarkedByChildMode`, `engine/objects/include/alamo/objects/object_store.hpp:100`), set by `mark_parent_mode_object_for_death` (`engine/objects/src/object_store.cpp:696-707`) and hooked to a tactical store's object deaths in `engine/apps/sim/src/game_modes.cpp:490`. What remains open: when flagged galactic objects are destroyed on return to galactic is not confirmed.

---

## 3. Ownership and containment

| relation | representation | conf |
|---|---|---|
| owner player | int player id at +0x58. Per-owner store lists and per-owner spatial trees are indexed by it. Planets report the owner from their planetary data instead (0x5792e0) | H |
| container (team/company → member; fleet → ship; galactic location chain) | each member has `ParentContainerObject` raw pointer +0x2B0 (debug-dump label) | H |
| team / company members | TEAM behaviour (type 0x16); member vector in the data pack at object +0x110 (array +0x10, count +0x18); behaviour +0x28 = owning object | H |
| fleet contents | FleetDataPack at +0xC0, contained-object vector at +0x08 (array +0x08, count +0x10) | M |
| transport contents | TransportDataPack at +0xC8, same vector shape | M |
| garrison | GarrisonDataPack-like pointer at +0xF8; GARRISON_VEHICLE / STRUCTURE / HOVER behaviours (0x4F/0x50/0x56) | M |
| hardpoints and other attached objects | separate GameObjects with `AttachParentObject` +0x2E0 and `AttachBoneIndex` +0x2F0; they die with the parent (§2.2). Destruction rules are in SPACE_AND_OPEN.md §B.10 | H |
| galactic location | `Get_Planet_Location` climbs the +0x2B0 chain until it finds an object with PLANET behaviour (galactic only). So fleet → planet and ship → fleet → planet is all +0x2B0 | H |

**Two Lua idioms built on this:**
- `Despawn` on an object with TEAM behaviour despawns its members (in reverse member order) instead of itself.
  [H] (0x5752a0)
- `Get_Parent_Object` returns +0x2B0 unless the parent is dead. [H]

---

## 4. Behaviours and data packs

### 4.1 Per-object layout

| field | meaning | conf |
|---|---|---|
| +0x278 / +0x288 / +0x289 | behaviour pointer array / count (signed byte) / capacity (signed byte) | H |
| +0x290 | `NumServicedBehaviors` (the first N array entries) | H |
| +0x332 + t (t = 1..0x69) | index into the array of the behaviour of type t, or 0xFF if absent. Type 0 means the object itself (`Get_Behavior` = vtable slot 2, 0x395ac0) | H |
| +0xA8 … +0x1A8 | data-pack pointers, one per behaviour family (table below) | H for the listed slots |
| +0x2D8 | per-object Lua script (`ServiceRate` / `LastService` keys) | H |
| +0x2C8 | last computed object CRC | H |
| +0x60 / +0x68 | vector of 0x38-byte delayed-damage events (countdown, then `Take_Damage`) | M |

Known data-pack slots:

| slot | data pack | conf |
|---|---|---|
| +0xA8 | Locomotor; also holds the previous-frame transform | H |
| +0xB8 | Planetary | H |
| +0xC0 | Fleet | H |
| +0xC8 | Transport | M |
| +0xE8 | Projectile | H |
| +0xF0 | tactical combatant / targeting; its +0x138 holds the modifier sources (STRUCTS.md) | H |
| +0xF8 | garrison | M |
| +0x110 | Team | H |

### 4.2 Attaching behaviours

**Construction attaches** [H]:
1. the type's `Behavior` list (type +0x6D0);
2. the mode-specific list chosen by the mode's kind (mode vtable +0xE0):
   - 0 galactic → `GalacticBehavior` (+0x6E8);
   - 1 land → `LandBehavior` (+0x718);
   - 2 space → `SpaceBehavior` (+0x700);
3. unit abilities (type +0x1AD8, 0x38cf30).

This happens in 0x3989a0 → 0x38cb30, in list order. `DeployedBehavior` (+0x730) is added later on deploy
(0x39fec0). Deploying warns if the object already has that behaviour type.

**Name table.** XML names resolve through the table at 0x140a2ac90: 116 rows, each holding the name, a factory
and a "type id" predicate. [H]

**`Add_Behavior` (0x38c570)** [H]:
- If the behaviour reports that it needs servicing (vtable +0x20):
  - it is **inserted at index 0**, shifting the others;
  - `NumServicedBehaviors` goes up;
  - the object is registered in the store's serviced list the first time;
  - the service interval is cached (0x4c3710: behaviour +0x34 = vtable +0x28).
- Otherwise it is appended at the end.
- The type→index table is then fixed up. The behaviour's own type is the highest t for which its vtable +0x18
  answers non-null.
- Then Attach (vtable +0x38) runs, and Initialise (vtable +0x48) unless loading.

### 4.3 XML name → runtime behaviour type id

The type id is the slot index into the +0x332 table. All rows [H] (read from the predicate functions).

**Locomotors** — `LAND_TEAM_CONTAINER_LOCOMOTOR`, `LAND_TEAM_INFANTRY_LOCOMOTOR`, `SIMPLE_LOCOMOTOR`,
`STARSHIP_LOCOMOTOR`, `SIMPLE_SPACE_LOCOMOTOR`, `FLEET_LOCOMOTOR`, `WALK_LOCOMOTOR`, `BIKE_LOCOMOTOR`,
`FIGHTER_LOCOMOTOR`, `TEAM_LOCOMOTOR`, `FLYING_LOCOMOTOR`, `JETPACK_LOCOMOTOR`, `LANDBOMBER_LOCOMOTOR` all share
**type 0x01**, so an object has at most one locomotor.

**Shared ids** — `TARGETING` and `TEAM_TARGETING` share 0x09; `INDIGENOUS_UNIT` and `EXPLOSION` share 0x20.

| id | XML name(s) | id | XML name(s) | id | XML name(s) |
|---|---|---|---|---|---|
| 0x02 | SELECTABLE | 0x1D | BARRAGE | 0x3C | CASH_POINT |
| 0x03 | PLANET | 0x1E | LANDING | 0x3D | CAPTURE_POINT |
| 0x04 | FLEET | 0x20 | INDIGENOUS_UNIT, EXPLOSION | 0x3E | DAMAGE_TRACKING |
| 0x05 | TRANSPORT | 0x21 | HUNT | 0x3F | TERRAIN_TEXTURE_MODIFICATION |
| 0x06 | PRODUCTION | 0x22 | GUARD_SPAWN_HOUSE | 0x40 | IMPOSING_PRESENCE |
| 0x07 | IDLE | 0x23 | LURE | 0x41 | HARASS |
| 0x08 | PROJECTILE | 0x25 | TREAD_SCROLL | 0x42 | AVOID_DANGER |
| 0x09 | TARGETING, TEAM_TARGETING | 0x26 | SQUASH | 0x43 | SELF_DESTRUCT |
| 0x0A | WEAPON | 0x27 | AMBIENT_SFX | 0x44 | DEATH |
| 0x0B | TURRET | 0x28 | DEBRIS | 0x45 | ION_STUN_EFFECT |
| 0x0C | PARTICLE | 0x29 | TACTICAL_BUILD_OBJECTS | 0x46 | ABILITY_COUNTDOWN |
| 0x0D | SURFACE_FX | 0x2A | TACTICAL_UNDER_CONSTRUCTION | 0x47 | AFFECTED_BY_SHIELD |
| 0x0E | WIND_DISTURBANCE | 0x2B | TACTICAL_SELL | 0x48 | NEBULA |
| 0x0F | POWERED | 0x2C | TACTICAL_SUPER_WEAPON | 0x49 | REINFORCEMENT_POINT |
| 0x10 | SHIELDED | 0x2D | BURNING | 0x4A | HINT |
| 0x11 | MARKER | 0x2E | ASTEROID_FIELD_DAMAGE | 0x4B | INVULNERABLE |
| 0x12 | UNIT_AI | 0x2F | SPECIAL_WEAPON | 0x4C | SPACE_OBSTACLE |
| 0x13 | STARSHIP_COMBATANT | 0x30 | TELEKINESIS_TARGET | 0x4D | MULTIPLAYER_BEACON |
| 0x14 | LAND_OBSTACLE | 0x31 | EARTHQUAKE_TARGET | 0x4E | HOLSTER_WEAPON |
| 0x15 | SQUAD_LEADER | 0x32 | VEHICLE_THIEF | 0x4F | GARRISON_VEHICLE |
| 0x16 | TEAM | 0x33 | DEMOLITION | 0x50 | GARRISON_STRUCTURE |
| 0x17 | WALL | 0x34 | BOMB | 0x51 | GARRISON_UNIT |
| 0x18 | REVEAL | 0x36 | TRANSPORT_LANDING | 0x52 | DISABLE_FORCE_ABILITIES |
| 0x19 | HIDE_WHEN_FOGGED | 0x37 | LOBBING_SUPERWEAPON | 0x53 | CONFUSE |
| 0x1A | SPAWN_SQUADRON | 0x38 | STUNNABLE | 0x54 | INFECTION |
| 0x1B | SPAWN_INDIGENOUS_UNITS | 0x39 | BASE_SHIELD | 0x55 | PROXIMITY_MINE |
| 0x1C | DEPLOY_TROOPERS | 0x3A | GRAVITY_CONTROL_FIELD | 0x56 | GARRISON_HOVER |
| | | 0x3B | SKY_DOME | 0x57 | BUZZ_DROIDS |
| | | | | 0x58 | CORRUPT_SYSTEMS |
| | | | | 0x59 | BOARDABLE |
| | | | | 0x5A | DYNAMIC_TRANSFORM |

**Dummy behaviours** (0x5B–0x69) — DUMMY_GROUND_BASE, DUMMY_STAR_BASE, DUMMY_GROUND_COMPANY,
DUMMY_SPACE_FIGHTER_SQUADRON, DUMMY_GROUND_STRUCTURE, DUMMY_ORBITAL_STRUCTURE, DUMMY_STARSHIP,
DUMMY_LAND_BASE_LEVEL_COMPONENT, DUMMY_SPECIAL_WEAPON_SOURCE, DUMMY_TACTICAL_SUPER_WEAPON_TARGET, DUMMY_UPGRADE,
DUMMY_DESTROY_AFTER_BOMBING_RUN, DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED, DUMMY_TOOLTIP,
DUMMY_DESTROY_AFTER_PLANETARY_BOMBARD, in that order.

**Ids with no XML name** — 0x1F, 0x24 and 0x35 are internal behaviours (probably abilities, energy pool, beacon)
[L].

**Runtime classes** follow the obvious naming (SELECTABLE → `SelectBehaviorClass`, TEAM → `TeamBehaviorClass`, …;
see the class list in SUBSYSTEMS.md §gameobject_behaviors). The factory addresses are in the table at
0x140a2ac90.

### 4.4 Per-tick behaviour service

Inside **object Service (0x3a6b80)**, behaviours are visited from index `NumServicedBehaviors−1` down to 0. Because
serviced behaviours are inserted at the front, **this is attach order**. A behaviour is serviced when both hold:
- next-service frame (behaviour +0x30) ≤ mode frame;
- disable counter (behaviour +0x3C) = 0.

Then vtable +0x30 Service(object, frame) runs. [H] for the loop and the tests; [M] for who advances +0x30.

---

## 5. Tick order

### 5.1 Frame (main loop 0x5d990, per allowed 30 Hz logic tick) [M]

1. Frame-sync begin (0x27c360).
2. **Execute scheduled command events due this frame** (0x2c0c70, queue at 0x140b15690):
   - first the replay/recorded queue;
   - then, for player index 0..N−1, that player's events in queue order (event vtable +0x20 = Execute).
3. Every 100th frame, a periodic task (0x4767c0).
4. 0x20f000(sim time) (see PHASE2_ANSWERS §12).
5. **GameModeManager update (0x28d400):**
   - scoring-manager service;
   - then **only the active mode** (manager +0x38) runs `Update` (vtable slot 22).
6. 0x2718e0(sim time).
7. Player list update (0x297e30), in player index order.
8. Mode +0xA0 / +0x9C group and selection managers.
9. 0x44a3f0.
10. Pending mode transitions (0x28a4c0).
11. Frame end (0x27cb70).

The frame CRC (0x27c3d0) adds three parts:
- the primary-store CRC of the current mode;
- the sync-RNG CRC;
- the player CRC.

### 5.2 Mode update (`GameModeClass` 0x3639d0; called by Land/Galactic/Space vf22 0x3bb440 / 0x4b2ef0 / 0x4d95a0) [M]

1. Per-frame type-manager tick (0x4098d0).
2. While running: speech/hologram (0x364b20) and audio (0x364920) ticks.
3. **Per-player staggered service every 16 frames.** Player p is serviced when (p.id mod 16) = (frame mod 16)
   (0x4c1670; likely the AI player). Then the fogged-model manager runs (0x4ec2e0).
4. Object tracking system (0x539b20).
5. **Primary store `Update`, then secondary store `Update`** (0x2be640).
6. Other systems, in this order:
   - 0x490580;
   - vtable +0x230 and +0x228 systems;
   - setup/tutorial trigger (mode +0x1F0);
   - RetreatCoordinator (+0x30);
   - story manager (+0x28);
   - BombingRunManager (+0x38);
   - PlanetaryBombardManager (+0x40);
   - +0x298 system;
   - then end-of-battle bookkeeping.

### 5.3 Store update (`GameObjectManagerClass` 0x2be640) [H]

1. Refit the spatial trees: the global tree and each per-player tree.
2. **Serviced-object loop:** walk the serviced list (GOM +0xE8) **newest-registered first** and run object Service
   0x3a6b80. Service does, in order:
   1. save the previous transform into the locomotor data pack; if it moved, raise signal 0x17;
   2. modifier service (combatant data pack);
   3. delayed-damage countdowns;
   4. **behaviours in attach order** (§4.4);
   5. late update if +0x3A1 & 1;
   6. model/animation time;
   7. the object's own Lua script at its `ServiceRate`;
   8. attached-object list service;
   9. decay of +0x320;
   10. RNG-driven regeneration check (type +0x234C).
   
   A "limited" flag skips most steps (condition depends on global multiplayer and setup state [L]).
3. Galactic stores only: planet production service (0x3fc750) for each serviced object with PLANET.
4. List GOM +0x130: late update 0x3ac530 for each member.
5. **Delayed-creation queue:** scanned from last entry to first; due entries are created and removed.
6. End-of-battle state machine (GOM +0x618) and conflict detection (0x2ad100 / 0x2b92e0, 0x2b76a0). [L]
7. **Deferred-delete flush** (§2.2).

**Objects created during the loop** are registered at the "newest" end of the list, which the walk has already
passed, so **they are not serviced until next frame**. [M]

**Objects killed during the loop** stay in the list with zero serviced behaviours until the flush. [H]

---

## 6. Determinism-relevant iteration orders

| what | order | conf | port rule |
|---|---|---|---|
| store lists (`ReferenceListClass` / `MultiLinkedList`): all objects GOM +0x38, serviced +0xE8, category lists | intrusive list; append at tail, traversal starts at the tail → **newest first** | M (direction read from the link writes) | keep an insertion-ordered dense list; iterate in reverse insertion order where the original does |
| object service order | serviced list, newest-registered first. Registration happens when the first serviced behaviour is attached, i.e. during construction | M | — |
| behaviour service order | attach order (XML `Behavior` list, then mode list, then runtime adds) | H | — |
| Lua `Find_All_Objects_Of_Type` | primary store all-objects list, newest first | M | — |
| command events | per frame: replay queue, then player 0..N−1, each in queue order | H | — |
| per-player loops (AI stagger, player update, per-owner lists) | player index | H | — |
| delayed-creation queue | last-to-first among due entries | H | — |
| pending-delete flush | order of `Destroy_Object` calls | H | — |
| `Remove_Object` pointer scrub | all-objects list order. **Cascaded kills of attached children happen in this order, before the dying object is unlinked** | H | — |
| ID map (GOM +0x80) | hash of the ID only (no pointers); never iterated on traced paths | H | do not iterate |
| Lua wrapper cache (0x140b39380) | **pointer-keyed** (object, script) | H key, M lookup-only | must never be iterated for logic |
| spatial-tree queries | tree structure order (insertion plus rebuild cadence; HOTSPOTS.md §3) | — | **flag for S1/S2:** gameplay queries that take the "first hit" depend on it |
| frame CRC | sum of per-object CRCs, so order-independent; skips dead objects; primary store only | H | — |
| per-object CRC (0x4ee890) | CRC32 table over (pos.x ^ pos.y ^ pos.z ^ facing.x ^ facing.y ^ facing.z) as raw bits, then health bits if non-zero, then (debug flag only) the locomotor data CRC; cached at +0x2C8 | H | — |
| secondary store | local, cosmetic, created on the local player's visibility (0x4ca6c0, 0x40040) | M | keep it out of the sim store and out of the sim RNG |

> **Status 2026-10-30:** The port's spatial queries return results sorted by object id (ascending handle; ray queries by t_enter then handle), independent of tree shape and insertion history, so "first hit" callers do not depend on tree order (`engine/tactical/include/alamo/tactical/spatial_tree.hpp:1-45`).

**No iteration over pointer-keyed or seeded-hash containers was found on the object-update paths read here.**
AI internals (goal hash maps keyed by category, HOTSPOTS §1) were not re-checked in this pass.

---

## 7. Galactic vs tactical objects

- **Same class family.** One `GameObjectClass`; mode-specific behaviour comes only from type data and behaviours:
  - planets: PLANET + `PlanetaryDataPack` (+0xB8);
  - galactic fleets: FLEET + `FleetDataPack` (+0xC0) holding ship objects;
  - galactic transports: TRANSPORT + `TransportDataPack`;
  - galactic heroes and companies are ordinary objects contained via +0x2B0.
  
  [H]
- **Separate stores per mode.** The galactic mode's stores hold the galactic objects; a tactical mode pushed on
  the stack creates new objects in its own stores. They link back through the parent-mode ID (+0x54). [H]
- **Lifecycle links:** death marking flows tactical → galactic (§2.5); `Despawn` and scoring notify the parent too
  (0x5752a0). [H]
- **Mode kind** (mode vtable +0xE0): 0 galactic, 1 land, 2 space. It selects the behaviour list (§4.2) and
  galactic-only steps such as planet production service. [H]

---

## 8. Open items

1. The wrapper-side handler for signal 0x10 (expected: null +0x60 and unsubscribe; 0x57f310). [M→H]
2. How fleet, team and transport content vectors and AI task forces drop dead members. Listener vs explicit hook
   is unknown. [L]
3. When objects flagged +0x3A0 & 0x04 in the galactic store are actually destroyed on mode pop, and the meaning of
   byte +0x330. [L]

> **Status 2026-10-30:** The flag exists in the port (`engine/objects/include/alamo/objects/object_store.hpp:100`, hook at `engine/apps/sim/src/game_modes.cpp:490`). Still open: when flagged galactic objects are destroyed on mode pop, and the meaning of byte +0x330.

4. Whether the global ID counter (GameModeManager +0x80) is saved and restored. [L]

> **Status 2026-10-30:** The port saves and restores the id counter (`engine/objects/src/object_store.cpp:470, 514, 687`).

5. The "limited service" condition at the top of the store's serviced loop (multiplayer/setup globals
   0x140b1540c, 0x140b154d0, 0x140b2766c). [L]
6. Meaning of the remaining category lists (GOM +0x1C8/+0x210/+0x258/+0x2E8) and of data packs +0x100/+0x1A8. [L]
7. ~~How the next-service frame (behaviour +0x30) is advanced~~ — resolved [H]: base Service 0x4c2f70 sets
   +0x30 = frame + +0x34; Initialise draws the first due frame with a **synced `Get_Int(0, interval − 1)`** for
   interval ≥ 2 (due at once below 2); "computed at attach" intervals and the disable counter (+0x3c) are in
   TACTICAL_GAPS.md §6.4 / §6.6; field codes 0x43 / 0x52 §6.5.

---

## 9. Service flags, duplicate attach, cascade flush (2026-10-27)

Answers to the S1 implementer's assumptions (OBJECT_TYPES.md §9.6). Read from the decompilation and from the
behaviour classes' vtables in the retail exe (RTTI vftables from `docs/re/ghidra/classes.csv`).

### 9.1 Which behaviours are serviced [H]
"Serviced" is decided per behaviour class by vtable +0x20 (a constant in every class checked). The service
interval is vtable +0x28, cached into behaviour +0x34 when attached (0x4c3710).

**Never serviced** (appended at the back of the array, not counted in `NumServicedBehaviors`, the object is
not put in the serviced list because of them):
- `TACTICAL_BUILD_OBJECTS`, `MARKER`, `HINT`, `TRANSPORT`, `TACTICAL_SELL`, `TERRAIN_TEXTURE_MODIFICATION`,
  `WALL`, `SPACE_OBSTACLE`;
- `STARSHIP_COMBATANT` (`CombatantBehaviorClass`);
- all 15 `DUMMY_*` behaviours.

**Serviced, interval 1:** every other behaviour, except those below.

**Serviced, other interval:**

| interval | behaviours |
|---|---|
| 0 | `EXPLOSION` |
| 4 | `CAPTURE_POINT`, `CASH_POINT` |
| 30 | `SPAWN_INDIGENOUS_UNITS`, `DAMAGE_TRACKING`, `HARASS`, `AVOID_DANGER`, `BARRAGE`, `SPAWN_SQUADRON`, `BURNING`, `IMPOSING_PRESENCE`, `AMBIENT_SFX`, `INVULNERABLE` |
| 60 | `GRAVITY_CONTROL_FIELD` |
| computed at attach | `LAND_OBSTACLE`, `SHIELDED`, `POWERED` (energy pool [M] for the class mapping), `BASE_SHIELD` (a global / 2, 0x140b0a340), `INDIGENOUS_UNIT`, `HUNT`, `GUARD_SPAWN_HOUSE` (depend on the owner's +0x3A1 flags) |

The interval feeds the next-service-frame test of §4.4; how +0x30 is advanced and the computed intervals:
TACTICAL_GAPS.md §6.4 (open item 7 resolved).

### 9.2 Duplicate behaviours [H]
The attach loop (0x38cb30) creates one behaviour per name in each list, **with no duplicate check**. Two names
with the same type id, or the same name twice, give two behaviour instances.
- The type→index table (+0x332) ends up pointing at the **later** one.
- Each instance is serviced if its class is.

Vanilla produces this: many variants name `STARSHIP_COMBATANT` twice (OBJECT_TYPES §9.3). That class is not
serviced, so the visible effect is limited to its Attach/Initialise running twice.

Also: when an attached behaviour answers non-null for several type ids, only the **highest** id's table slot is
set to it.

### 9.3 Cascaded kills [H]
The flush (0x2a62d0) re-reads the pending-delete count on every iteration. An object killed during the flush,
e.g. an attached child killed by `Remove_Object`'s pointer scrub, is appended to the same vector and **removed in
the same flush**, after the objects already queued. The vector is cleared only after the loop.
