# Movement and pathing (StarWarsG.exe FoC x64 v1.121): behaviour spec

This spec was recovered from the binary for roadmap step **S7, "Movement & pathing"**: land passability from `.ted` maps, space navigation, formations and collision. It was written for the implementer before any code exists.

**Rules for this document:**
- Prose only.
- Addresses are RVAs in StarWarsG.exe unless they begin with `0x140…`, which are absolute VAs of globals.
- Confidence tags: **[H]** read directly from the code, **[M]** inferred from structure or partial reading, **[L]** guess.
- Every gap is listed in §8.

**Related documents (not repeated here):**
- TERRAIN_WATER_POST.md §1.5: the `.ted` `0x10A` file format.
- HOTSPOTS.md §3: the TreeCull spatial tree.
- OBJECT_TYPES.md / OBJECT_RUNTIME.md: type fields and object layout.
- TACTICAL_COMBAT.md: weapons and targeting.
- MAIN_LOOP.md / PHASE2_ANSWERS.md Q12: the frame loop.

**Global addresses used throughout:**

| Address | What |
|---|---|
| `0x140b15800` | GameConstants block. Field offsets are in xml_field_tables.md, "GameConstants" segment. [H] |
| `0x140b15418` | Current game mode. Virtuals: +0xe0 mode type (0 galactic, 1 land, 2 space); +0x208 playable box (centre, half-extents); +0x228 MovementCoordinatorSystem; +0x238 object-tracking system. [H] |
| `0x140b0a320` | Global logic frame counter. [H] |
| `0x140a13e24` | The synced (lockstep) GameRandom stream, drawn through `0x1ffb40`. [H] |

---

## 0. Summary for the implementer

1. **Units are frames, not seconds.**
   - Logic runs at a fixed 30 Hz (PHASE2_ANSWERS Q12).
   - Every locomotor integrates once per logic frame with no dt factor:
     - `Max_Speed` is world units per frame. A Stormtrooper team's 0.8 is 24 u/s before multipliers.
     - `OverrideAcceleration`/`OverrideDeceleration` are units per frame².
     - `Max_Rate_Of_Turn` is degrees per frame.
   - Planner timestamps are in frames.
   - Port: keep integer frames and per-frame quantities. Do not multiply by a float dt. [H]
2. **The land passability grid is one cell per 20-unit terrain vertex, centred on the vertex.**
   - Each cell keeps a base value from `.ted` `0x10A` and an effective value. The effective value is the base OR'd with the passability of the objects stamped on the cell.
   - A unit may enter a cell iff **every bit set in the cell is in the unit's MovementClass mask** (MovementClassTypeDefs.xml). [H]
3. **The main land search is A\* over a rectangle decomposition of the grid, not over cells.**
   - The result is string-pulled, then recursively shortcut.
   - Large units re-plan low-clearance spans with a clearance radius.
   - A 4-connected cell A\* exists as a variant. [H]
4. **Space uses a different planner: a kinematic, time-parameterised A\*.**
   - The state lattice is (x cell, y cell, heading bucket). Operators are straight, arc, turn-in-place, speed-change and wait.
   - Collisions are checked in space-time against a time-sliced occupancy structure.
   - Ships then **replay the planned trajectory with cubic Hermite interpolation**. There is no reactive steering for capital ships. [H]
   - The same planner has land branches. [M]
5. **Local avoidance on land is a collision system plus a "dynamic" pathfinder.**
   - Moving and stopped units are paired into MovingVsMoving, MovingVsStopped and StoppedVsStopped collision objects.
   - Each pair is resolved by repulsion, push, speed slaving or a local visibility-graph repath.
   - The repath runs under a global complexity quota of 100 per 5 frames. [H]
6. **Formations.**
   - Selected units are split into spatial buckets by k-means, then into formations by recruitment rules.
   - Slots are assigned in rows: slowest and heroes in front; within a row units keep their lateral order.
   - Every random draw (jiggle, start delays, stagger) uses the synced RNG. [H]
7. **Determinism.**
   - Every planner and collision container is keyed by integers or object IDs and is probed, never iterated.
   - Open lists are binary heaps with exact tie rules (§1.4).
   - Two hazards must be designed out in the port:
     - **unstable `std::sort`** in formation slot ranking and in the collision priority list;
     - the **object-grid query order** of collision candidates (§5.2).
   - Hyperspace arrival staggering may read camera-derived globals (§2.6). [M]

---

## 1. Land pathfinding

### 1.1 Runtime passability grid [H]

**Grid header.** A global struct at `0x140b36b90` holds:
- width and height in cells (+0x00, +0x04);
- integer cell size **20** in x and y (+0x08, +0x0C);
- half cell **10** (+0x10, +0x14);
- a pointer to the cell array (+0x18).

It is initialised by `0x5116c0`, called from `0x3b98a0` right after the terrain chunk loads, with the terrain vertex dimensions and 20.0/20.0. So there is one cell per terrain vertex, cell (x, y) is centred on world (20x, 20y), and the grid size is W×H of the terrain (187² … 373² in vanilla).

**World to cell:** cell = floor((world + 10) / 20) on each axis (`0x5a51f0`, `0x4f88b0`).
- Some call sites use a truncating int cast: the obstacle stamper `0x4f88b0` and the locomotor commit check `0x50e280`, which reads its globals at `0x140b36b98…ba4`. These differ from floor only for negative inputs.
- Port: use floor everywhere, and treat negative coordinates as outside. [M]

**Cell record.** 0x1C bytes, row-major (index = y·W + x):

| offset | content |
|---|---|
| +0x00 | pointer to the "hard occupant" object list (count byte at list+0x28) |
| +0x08 | pointer to the "secondary occupant" list (count byte at +0x28) |
| +0x10 | u32 base passability, from the map |
| +0x14 | u32 effective passability |
| +0x18 / +0x1A | u16 cell x / y |

Before load, every cell is base = effective = Clear (1).

**Bit values (enum `CellPassabilityType`, `0x5dd090`):**
- Clear 0x1
- Steep 0x2
- Obstacle 0x4
- RebelWall 0x8
- EmpireWall 0x10
- Water 0x20
- Shield 0x40
- InfantryOnly 0x80
- Impassable 0x100

**Load** (land loader `0x3b8cd0`):
- `0x10A` holds one u32 per cell, stored column-major in the file (x outer, as noted in TERRAIN_WATER_POST §1.5). Each value is stored as the base through `0x59f4b0` with "recompute" off.
- Legacy `0x104` is one byte per cell, mapped 0→Clear, 1→Steep, 2→Obstacle.
- After load, each cell is recomputed by `0x59f110`. Cells whose centre is **outside the playable box** (mode +0x418 centre, +0x424 half-extent; `.ted` `0x103` minis 05/06) get **Obstacle forced on**. [M] for the exact box comparison.

**Effective passability** (`0x59f4b0` with recompute on, `0x59f110`):
1. Start from the base value.
2. OR in each hard occupant's passability. `0x397350` OR's `0x4c36b0` over the object's footprint entries. Each entry uses the type's `OverridePassability` (+0x1690) when it is non-zero, otherwise a behaviour-supplied default.
3. OR in each secondary occupant's `SecondaryOccupationPassability` (+0x1694).
4. If any bit other than Clear is set, clear Clear.
5. If the value changed, set the global "regions dirty" flag `0x140a2bed8`.

**Movement-class test** (`0x38e390`; inlined in `0x5a3020`, `0x5a51f0`, `0x50e280`):
- passable ⇔ (effective & mask) == effective.
- The mask is the MovementClass mask (§1.2) for the type's `MovementClass` (+0x1688).
- With no object, only a pure-Clear cell passes.
- An out-of-range cell reads as 0xFFFFFFFF, so it always fails.
- The locomotor's per-step check reads the occupation layer chosen by `OccupationStyle` (+0x1698). [H] for the read, [M] for the layer semantics.

### 1.2 Movement classes (`TheMovementClassManagerClass`) [H]

- **Construction:** singleton ctor `0x477900`. `0x477c10` loads `.\Data\XML\MovementClassTypeDefs.XML`. The enum names come from `Enum\MovementClassType.xml` through the dynamic enum `0x46ea70`.
- **Masks:** each class maps to a passability bitmask. The table below is the vanilla FoC data, copied from the file; enum values are in brackets.
- **Lookup:** `0x477b70` uses a hash map keyed by enum value. The hash is a Park–Miller step of (key XOR 0xDEADBEEF) & 0x7FFFFFFF, masked by the bucket count. It is only probed, never iterated.

| class | allowed bits |
|---|---|
| Infantry [0] | Clear, Shield, InfantryOnly |
| InfantryNoShield [1] | Clear, InfantryOnly |
| Wheeled [2] / Walker [4] / Tracked [7] | Clear, Shield |
| WheeledNoShield [3] / WalkerNoShield [5] / TrackedNoShield [8] | Clear |
| Flying [6] | Clear, Steep, Water, RebelWall, EmpireWall, Shield, Obstacle, InfantryOnly |
| Hover [9] | Clear, Water, Shield |
| HoverNoShield [10] | Clear, Water |
| LargeWalker [11] | Clear, RebelWall, Shield |
| LargeWalkerNoShield [12] | Clear, RebelWall |
| Space [13] | Clear, Steep, Obstacle, Water, RebelWall, EmpireWall, Shield, InfantryOnly |
| Lancet [14] | Clear, Water, RebelWall, EmpireWall, Shield, InfantryOnly, Steep |
| Spotter [15] | Clear, Shield, InfantryOnly, Water |
| LargeHover [16] | Clear, EmpireWall, Shield, Water |
| BombingRun [17] | everything, including Impassable |

**Consequences:**
- Water is entered only by the hover and flying classes.
- Steep is entered only by Flying, Space, Lancet and BombingRun.
- InfantryOnly is entered only by the classes that list it.
- The "walls" are faction-specific bits that a structure stamps through `OverridePassability`: a RebelWall cell blocks everything except LargeWalker and flyers.

### 1.3 Dynamic stamping of objects (`LandObstacleBehaviorClass`, `0x4f88b0`) [H]

**Footprint rectangle:**

| part | from the XML field, if set | otherwise |
|---|---|---|
| width | `Obstacle_Width` (+0x162C) | 2·\|model bbox half-x\|·`Scale_Factor` |
| height | `Obstacle_Height` (+0x1630) | the same rule on y |
| offset | `Obstacle_X_Offset` / `Obstacle_Y_Offset` (+0x1634 / +0x1638) | −half-extent·scale |
| vertical height | — | model bbox z·scale |

**Sampling:**
1. The rectangle is sampled every **10 world units** on both axes.
2. Each sample is rotated by the object's yaw (object +0x8C, degrees) and converted to a cell.
3. The object is added once to that cell's hard-occupant list (dedup `0x59efa0`, add `0x59f1e0`), and the cell is recomputed.

**Water rule:** a sample over a cell whose base has Water is stamped only if (water height + 5) ≤ (terrain height + obstacle height). Low objects that are submerged do not block.

**`Fine_Tune_Occupied_Passability`** (type +0x94, **default true**, OBJECT_TYPES §4.6): each sample additionally needs a vertical ray to hit the object's collision mesh. This trims footprints to the real mesh.
- The ray is a segment from (x, y, **−1000**) up to (x, y, **+1000**) in absolute world z (not relative to the terrain), tested with the model's ray query (vf +0x80). Corrected 2026-11-02 (S8 t07): the earlier text had the direction reversed; only hit / no hit matters. [H]
- It applies to **every** stamp, at map load and at run time alike: the one stamper `0x4f88b0` serves both (see "Lifecycle" below). [H]

**`Obstacle_Proxy_Type`** (lazy type ref, name at type +0xdb0, resolved pointer at +0xdb8; 77 tags in FoC, e.g. `E_Ground_Barracks` → `E_Ground_Barracks_Build_Pad`) [H] (2026-11-02, S8 t07):
- When set, the **proxy type** replaces the object's type for the footprint: its `Obstacle_Width` / `_Height` / offsets / model box and its `Fine_Tune_Occupied_Passability` byte. The object's own position and yaw are kept.
- The fine-tune ray then tests the **proxy's** model: the stamper loads the proxy's `Land_Model_Name` (+0x1e0), takes its ALO render object, gives it the object's transform with the 3×3 part scaled by proxy `Scale_Factor` / object `Scale_Factor` (+0x658), and releases it after the stamp. If that model has no ALO render object, no ray test is made (every sample stamps).
- Without a proxy, the ray tests the object's own render model (obj +0x2a0). **An object with fine-tune on, no proxy and no render model is not stamped at all** (the stamper returns before the loop and the "stamped" flag stays clear, so it retries at the next service).
- Object flag obj +0x3a1 & 8 replaces the type sizes by the live model's box (both in the stamp and the unstamp). Meaning of the flag not traced [open].

**Ray mesh set, proxy bits, missing model (S8 t13, 2026-11-02)** [H unless tagged]:
- **Which meshes the fine-tune ray tests** (vf +0x80). The request is mode 0, mask 1, with the segment z −1000 →
  +1000.
  - alHModel (vtable 0x818d90, vf +0x80 = `0x12c9c0`) needs model +0x30 bit 7, which is always set (`0x12b2e6` /
    `0x12b722`). It then ORs vf +0x80 over every child renderable. Non-mesh children use the base `0x13b7d0`, which
    returns 0.
  - alRenderableMesh (vtable 0x819778, `0x14c520`) skips a mesh when any of these holds:
    - its runtime hidden bit 1 is set, i.e. its attached bone is not visible (bit 1 = !(bone visibility bit 2),
      `0x12c7c0` / `0x13fc10`);
    - mesh +0x30 bit 7 is clear in mode 0. That bit is the ALO mesh-info (chunk 0x402) `is_collision_enabled` at
      offset 36 (loader `0x14c970`);
    - mask & mesh +0xc0 is 0 (+0xc0 defaults to 1, ctor `0x13f63a`).
  - Otherwise it runs a world AABB pretest, then tests every sub-mesh's triangles (`0x13e900`): the collision tree
    (+0x98) when present, else brute force over the CPU positions (+0xc0) and indices (+0xf0) that `0x188e50` builds
    for every sub-mesh.
  - So **only ALO meshes with the collision flag are hit**. The file's `is_hidden` flag (offset 32 → +0x30 bit 0)
    does **not** exclude a mesh. Example: `NB_Bunker`'s `Collision_ALTn` meshes are hidden **and** collision, and
    they are what the ray hits.
  - A mesh hidden at run time because its bone is hidden (damage states, ALT swaps through bone visibility) is
    skipped.
  - Without a proxy, the model is the live, posed model obj +0x2a0 → +0x48.
  - With a proxy, the model is loaded by name through `0x142d70` (asset manager `0x140a62700`, ".ALO" appended)
    and must be an alHModel. vf +0x58 sets its transform, and vf +0x10(1) releases it after the stamp.
- **Proxy footprint bits.** The bits written into the cells come from `0x397350(object)`, which loops over the
  **object's own** behaviours (+0x278, count +0x288) calling `0x4c36b0`.
  - Each behaviour yields the owner's **own type** `OverridePassability` (+0x1690) when it is non-zero, else its
    own vf +0xc8 default: LAND_OBSTACLE 4 (`0x1fc30`), WALL 0x10 (`0x7c040`), base 0 (`0x17d070`).
  - The proxy never supplies bits. It supplies only the footprint: sizes, offsets, `Fine_Tune` (+0x94) and the ray
    model.
- **A type without `Land_Model_Name`.**
  - The size getters `0x372d80` / `0x372ce0` / `0x372e20` / `0x372ec0` / `0x372f60` use `Obstacle_Width` /
    `_Height` / the offsets when they are non-zero. Otherwise they read the mode model (`0x3729f0`; land = type
    +0x2a0, loaded lazily from `Land_Model_Name` by `0x375380`). With no model, the size is 0.
  - obj +0x2a0 is never created (`0x3a4820` returns when the mode model is null).
  - Fine-tune on (the default) and no proxy: the stamper returns early. "Stamped" stays clear, so the object
    retries **every service forever** and never stamps a cell (interval 1 while not stamped).
  - Fine-tune off: the rectangle comes only from an explicit `Obstacle_Width` **and** `Obstacle_Height` (both
    non-zero). Otherwise there are zero samples and "stamped" := 1.
  - With a proxy: the proxy's sizes and model are used. A proxy without a model means no ray, so every sample
    stamps.
  - The obj +0x3a1 & 8 branch also needs obj +0x2a0.

*For the implementer:*
- The unstamp always recomputes the cell from the remaining occupants.
- The fine-tune ray hits collision-flagged meshes whose bone is visible, hidden-in-file ones included.
- The stamped bits come from the object's own type or behaviours, never from the proxy.
- No model, fine-tune on and no proxy means "never stamped", retried every service.

**Lifecycle of a stamp** (`LandObstacleBehaviorClass`, vftable `0x87b358`; behaviour id 0x14; data pack obj +0x208 = `LandObstacleDataPackClass`, byte +8 "stamped") [H] (2026-11-02, S8 t07):

| slot | RVA | role |
|---|---|---|
| vf5 (+0x28) | `0x4f8840` | service interval: 1 while not stamped, 0 once stamped (TACTICAL_GAPS §6.4) |
| vf6 (+0x30) | `0x4f90c0` | service: stamps (`0x4f88b0`) when the pack exists and "stamped" is clear |
| vf7 (+0x38) | `0x4f87a0` | attach: creates the data pack (`0x390a20`) |
| vf9 (+0x48) | `0x4f8880` | initialise: interval < 2, so the first service is due at once |
| vf13 (+0x68) | `0x4f8800` | "placed" broadcast: stamps (`0x4f88b0`) without testing the flag (per-cell dedup only) |
| vf15 (+0x78) | `0x4f90f0` | detach: unstamps |

- **Map load:** `0x2a8730` walks every object of the GOM and calls `0x394510`, which sends vf13 to every behaviour of the object. So map placements are stamped by the same stamper as everything else. vf13 is also broadcast by `0x42d8b0` / `0x42dbd0` / `0x42e890` (campaign restore and two related paths).
- **Run time:** `GOM::Create_Object` (`0x29f810`) attaches the behaviour (vf7, vf9); the object is stamped at its **first behaviour service** (vf6). The stamper works only in land mode (mode vf +0xe0 == 1, terrain present).
- **Unstamp** (vf15 `0x4f90f0`): re-samples the same rectangle at the object's current position and yaw (no water rule, no ray), removes the object from each sampled cell's hard-occupant list (`0x59efa0` / `0x59f020`) and clears "stamped".
  **Corrected 2026-11-02 (S8 t13)** [H]. The earlier text said that "the cell is recomputed only when no live
  occupant is left in it" and that "the recompute also needs global `0x140b15490` set", so that two overlapping
  obstacles would keep the removed one's bits. That was **wrong**:
  - For every sampled cell that holds the object, the unstamp calls `0x59f020` unconditionally. `0x59f020` removes
    the object (`0x59ef20`), frees the list when it becomes empty and **always recomputes the cell**.
  - The recompute: effective (cell +0x14) = base (+0x10), ORed with the `0x397350` bits of every remaining hard
    occupant (dead occupants included), ORed with the `+0x1694` bits of every soft occupant's type. Bit 0 is dropped
    when any other bit is set. A change sets the dirty flag `0x140a2bed8` = 1. The add path `0x59f1e0` →
    `0x59f260` uses the same recompute.
  - So overlapping obstacles keep only the **remaining** occupants' bits.
  - The global and the "no live occupant" test gate only `0x3bb6d0`. That function repaints a passability
    **debug-overlay** texture (land mode +0x338; colour from `0x3b8090`; the stamper always paints magenta).
  - `0x140b15490` is GameModeManager (`0x140b153e0`) +0xb0. Its only writes in StarWarsG.exe are zero
    initialisations (`0x17062`, ctor `0x289779`): never set [H for the static scan; M that it is a tool / editor
    flag].
  - Cell layout: stride 0x1c; +0 hard list, +8 soft list, +0x10 base, +0x14 effective, +0x18 / +0x1a x / y.
- **When detach runs:** only when the behaviour is removed. Death does not detach (`Die` `0x39bdb0` and `Kill` `0x391310` leave behaviours alone); the GameObject destructor (`0x38a310` → `0x3893e0`) calls `0x3a52f0`, which runs vf15 on every behaviour in reverse attach order. The destructor runs when the last reference drops, normally at the store's pending-delete flush at the end of the frame of death (OBJECT_RUNTIME §2.2) [H for the path, M for "normally": a Lua wrapper or another holder keeps the object, and its stamp, alive longer].

**Bridges:** no dedicated bridge code was found. Bridges are ordinary objects whose `OverridePassability` / secondary passability re-opens cells. [L]

**Death clones and wrecks** [H] (2026-11-02, S8 t07; replaces the earlier "not traced" note):
- A death clone is an ordinary object of the clone type, made by `0x29f270` through `Create_Object` (S9_DEATH_CLONE_SHIELDS §1.6). When no `Death_Clone` entry applies, the clone is of the **dying object's own type** (S9 §1.3), so every structure without a `Death_Clone` list leaves an own-type clone.
- Right after the create, `0x29f270` walks the clone's behaviours (last to first) and **detaches (vf15) and removes (`0x3a54c0`) every behaviour except**:
  - HIDE_WHEN_FOGGED (0x19);
  - IMPOSING_PRESENCE (0x40);
  - LAND_OBSTACLE (0x14), **only when the clone type's `Death_Clone_Is_Obstacle` (+0x92, default true) is set**.

  Then the DEATH behaviour (0x44) is attached. The test uses the interface query vf3 (+0x18) with the behaviour id.
- So **a death clone is stamped iff its type lists LAND_OBSTACLE (in `Behavior` or `LandBehavior`, after `Variant_Of_Existing_Type`) and `Death_Clone_Is_Obstacle` is true.** It is stamped at its first service, with its own type's footprint rules above (proxy and fine-tune included), and unstamped when the clone is destroyed (end of its DEATH behaviour).
- The dying object keeps its own stamp until it is destroyed (previous item), so the cell stays blocked across the hand-over; the original's unstamp removes only the original from the cell lists.
- `IsDeathCloneObstacle` (+0xcf) is **not** a passability flag. It gates the land **collision** registration of death clones (`0x504880`: an object with obj +0x3a0 & 0x80, i.e. a death clone, is registered only when the type sets +0xcf; `0x29f270` calls it when the clone type's `Space_Layer` is Land) and the MovingVsStopped decision (§5.4). The AT-AT / AT-ST / SPMAT / MPTL / Rancor clones are collision obstacles this way, but not stamped (their types have no LAND_OBSTACLE).
- FoC data (variant-resolved; default true applied):
  - Explicit clone types that are stamped: `Veers_AT_AT_Walker_Death_Clone`, `Veers_AT_AT_Walker_Deployed_Death_Clone`, `Gargantuan_Battle_Platform_Death_Clone`.
  - Own-type clones: 455 LAND_OBSTACLE types have no `Death_Clone` list. 451 keep the stamp (all ordinary structures, build pads, props); the 4 with `Death_Clone_Is_Obstacle` = false lose it: `Imperial_Prison_Dathomir` and its `_Singing_Mountain` / `_Frenzied_River` / `_Misty_Falls` variants.
  - `Empire_Gate_Large` (LAND_OBSTACLE, `Death_Clone_Is_Obstacle` false) dies into `Empire_Gate_Large_Death_Clone`, which has no LAND_OBSTACLE: the gate's cells open when the gate is destroyed.

**Port notes (2026-11-02, S8 t07):** `TacticalBattle::land_obstacles_sync` stamps every object in the LAND_OBSTACLE behaviour table, so it stamps the 4 Dathomir prison wrecks that retail does not; its "[inferred]" note for the Veers / Gargantuan clones is **confirmed**. Since S8 t09 / t13 the battle host supplies `MapStampHooks::ray_model` / `mesh_ray_hit` (`land_stamp_hooks`: the ray is `avoid_ray_any`, the collision-flagged meshes whose bone is visible; an empty `Land_Model_Name` is `StampRayModel::None`), and `land_obstacle_footprint` applies `Obstacle_Proxy_Type`, the per-getter sizes, the no-model rules above and the object's own bits (`land_obstacle_bits`). The unstamp always recomputes the cell, which **matches** retail (corrected 2026-11-02, above; the earlier note here said retail did not).

### 1.4 Region decomposition and region A\* (the main land search)

**Region map** (singleton `0x140a2be20`) [H]:
- A cell → region index map, the region table, and per-region adjacency lists (count followed by region indices).
- The accessor `0x50e810` returns −1 outside the map.
- Each region is 0x1C bytes: x, y, w, h, passability bits, flag.

**Build** (`0x50fcd0` → `0x50d2b0`, `0x50cf80`, `0x50cd20`, `0x50d540`, `0x50da90`) [M]:
1. Scan the grid in raster order (y outer, x inner).
2. At each unassigned cell, grow a rectangle of cells with **identical effective passability**:
   - grow width and height together;
   - then extend one side up to a **2:1 aspect limit**, only where the edges line up with already-assigned neighbours.
3. Collapse a 1×2 or 2×1 result to 1×1.
4. Assign region indices in creation order.
5. The last row and column are not covered (set to −1).

The build runs lazily from the query functions when the dirty flag `0x140a2bed8` is set. Where the flag is consumed was not traced. [L]

**Entry and callers:** the public entry `0x5a2830` (request setup `0x5a2e00`, region search `0x5a3020`) is called from:
- MovementCoordinator `0x417520` and `0x41c1a0`;
- LandMovementCoordinator `0x439360`, `0x43c1b0` and `0x43d6a0`;
- the dynamic pathfinder `0x679e20` and `0x687720`. [H]

**Request** (LandPathFinder +0x108…+0x158) [H]:

| field | content |
|---|---|
| +0x108 / +0x114 | start / goal xyz |
| +0x120 | object |
| +0x128 | MovementClass |
| +0x13C | threat factor |
| +0x140 | clearance radius |
| +0x144 | use region A\* |
| +0x145 | use cell A\* |
| +0x147 | smooth |
| +0x148 | threat-aware |
| +0x149 | post pass |
| +0x150 / +0x158 | start / goal cells, from snapping in `0x5a1910` (not traced) |

**Search** [H]:
1. If the start and goal regions are the same, return the direct path (`0x5a3ba0`).
2. A region is enterable only if its passability bits pass the mask rule of §1.1.
3. A node is (region, entry point). The entry point is the cell on the shared border closest to the current point, clamped to the overlap of the two rectangles (`0x5a4040`).
4. Edge cost (`0x5a1e70`) = Euclidean distance in cells between successive entry points.
   - If the clearance radius is > 20, the crossing is sampled every 20 units with the clearance probe `0x5a1660`, and the variant with more clearance is chosen.
   - In threat mode, threat-map samples (`0x140b31900`, `0x653ff0`) are added. The exact combination is [M].
5. f = g + Euclidean distance to the goal (admissible, in cells).
6. **Open list:** a 1-based binary min-heap on f.
   - Sift-up stops when parent ≤ child, so a newly pushed node never rises above an equal key.
   - Sift-down (`0x5a2120`) swaps only with a strictly smaller child. It takes the left child first, and the right child replaces it only if strictly smaller. **Replicate this exactly for bit-identical paths.**
7. **Closed set:** a hash keyed by (from-region, to-region), with buckets stamped by a per-search generation counter (+0xD8), so no clear is needed.
   - It stores g and a closed flag.
   - A neighbour is (re)pushed only when its new g is strictly smaller than the stored g.
   - Neighbours are visited in adjacency-list order, which is region build order.
8. Node and entry pools are fixed arrays. Exhausting them fails the search.
9. The search ends when the goal region is popped. The path is then built by `0x5a0550`.

**Path build and smoothing** [H] unless tagged:
1. Walk the parents back and emit border entry/exit cells as world points (cell·20). Prepend the exact start and append the exact goal.
2. If smoothing is on, **pass (a)** (`0x59fef0`): for each waypoint, binary-search the furthest later waypoint that is visible and drop the ones in between.
   - Visibility = region line-of-sight `0x5a5810` plus clearance probe `0x59fd90`.
3. **Pass (b)** (`0x5a4130`) is a recursive shortcut over arc-length intervals [a, b] along the path, applied only when b − a ≥ **15**.
   - Skip the span if both turns at its ends are nearly straight (dot products > **0.9**), or if a vertex is closer than **20**.
   - Otherwise test the direct shortcut with the grid DDA (`0x5a50d0` → `0x5a51f0`). This is an Amanatides–Woo cell walk: every crossed cell must pass the mask rule and, in threat mode, stay below max(endpoint threat)·factor(+0x13C). A second test, `0x5a4c40`, also applies.
   - On success, splice the shortcut in.
   - On failure, recurse on [a, a + 0.66·(b − a)] and [a + 0.33·(b − a), b], rescaled to the new length.
4. Waypoints are 0x34-byte records (`0x54e4a0`). The same record is used by the space planner (§2.4).

**Large units** (`0x5a2830`) [H]:
1. Plan once without clearance.
2. For each run of segments whose minimum clearance (`0x5a1490`, sampled every 20) is below (radius − 0.1), re-plan that sub-span with the clearance radius set and splice it in.
3. A fallback path is accepted only if its length ≤ 1.4× a reference length. [M]

**Cell A\* variant** (`0x5a2200`, request +0x145) [H]:
- 4-connected neighbours from the table at `0x140a43bc0`. The order of its 4 pairs needs a binary read (§8).
- Step cost 1, or the threat value in threat mode.
- Manhattan heuristic.
- Per-cell generation stamps, a g array and a closed bitset.
- Same heap rules as the region search.

`LandZonePathFinderClass` (zones from `.ted` `0x10B`, `0x3b54d0`) and the AI's `LandExecutionPathFinderClass` were not traced. `LandPositionFinderClass` does not exist in this build's RTTI.

### 1.5 Dynamic local pathfinder (`DynamicLandPathFinderClass`) [H]

- **Object:** singleton `0x140a2be10`, 0x198 bytes, ctor `0x673b10`, main `0x675240`.
- **Use:** it is the expensive resolution used by land collisions (§5).

**Quota:**
- The collision update `0x50a780` collects active collisions and sorts them ascending on a float priority (collision vf +0x60), using `std::sort`. The sort is unstable; its input is insertion order.
- For each collision, if the quota counter (+0x13C) is below **DynamicLandComplexityQuota = 100** (`0x679740`), it runs the repath (vf +0x20). Otherwise it runs the cheap resolution (vf +0x18).
- Each search adds its graph vertex count to the counter (`0x67b8a0`).
- The counter resets when frame % **DynamicLandQuotaResetInterval (5)** == 0, or every frame if that value is < 2 (`0x67cef0`).

**Window** (`0x67a0d0`):
- Start from the cell bounding box of start and goal.
- Expand it by **DynamicAvoidanceRectangleBound = 3** cells plus the unit's half-size in cells, plus 2 more cells unless UnitCollisionClass is Giant (9).
- Balance the aspect to within 1.5:1 and clamp it to the grid.
- On failure, grow the window by 3 cells per side, for up to **20** attempts (`0x675240`).

**Graph** [M]:
- A visibility graph over the corners of nearby units' rectangles that lie inside the window and pass the region map (`0x50e280`).
- Crossing another unit's rectangle is allowed at cost × **DynamicObstacleOverlapPenalty = 15**. Pairs that share a target or are in compatible states are exempt (`0x67a3d0`).
- Destination tolerances are **LandDestinationProximity = 60** and **LandTemporaryDestinationProximity = 20** (`0x67ad20`, `0x679aa0`).

*(s78n t19, 2026-11-10: the Window and Graph bullets above are the 2026-10 summary; §1.5.1 replaces them. Three of
their statements are wrong: the "+2 cells" applies **only to Giant**, not to everything but Giant; there is no
"share a target" exemption in `0x67a3d0`; and the two proximity constants are used the other way round (20 = how far
past the path end a rejoin point may go, 60 = how far the nearest passable point may be from a candidate).)*

#### 1.5.1 The Path Around call in full (s78n t19 RE, 2026-11-10) [H unless tagged]
Read from `0x5c80e0`, `0x5c6ba0`, `0x673ac0`, `0x67c9f0`, `0x67b310`, `0x675ed0`, `0x676c10`, `0x67ad20`, `0x679aa0`,
`0x679e20`, `0x67a0d0`, `0x67cf20`, `0x677e40`, `0x677340`, `0x67d580`, `0x679750`, `0x677650`, `0x678e80`,
`0x67a3d0`, `0x67ca20`, `0x67b8a0`, `0x674870`, `0x6744a0`, `0x67c1a0`, `0x67c0b0`, `0x67b4c0`, `0x440be0` /
`0x440f30` / `0x440f90`, `0x42cdd0` / `0x42c7d0` / `0x559410`, `0x554310`, `0x620820`. GameConstants values are filled
at run time, so names come from the XML order; offsets GC +0x14dc…+0x14e8 are confirmed by use.

**Entry `0x5c80e0`(record, unit u, obstacle o, flag, kind, flags)** → `0x67b310` (goal `0x675ed0`, graph `0x67ca20`,
search `0x67b8a0`). Callers: MovingVsStopped `0x685920` (flag 0, kind 4, flags p, then once more with p | 4);
MovingVsMoving `0x682e80` (flag 1, kind 4, flags 0), `0x683140` (flag 0, kind 4, or 7 for a scattering crush victim),
`0x681ef0` (flag = the two wanted headings differ by < 90°); TerrainCollision `0x687ff0` (no obstacle, kind 3).

1. **Request** (`0x673ac0` defaults, then the entry):
   - start = u's position; u itself.
   - Obstacle mask +0x40 := 0x10, or **0x11 when u is Infantry or Giant** (bit 0: Infantry-class objects that are
     not in a hero clash (+0x3b4) are **not obstacles**). Goal-spot mask +0x3c := 0x10.
   - +0x2c := o's id only when `flag` ≠ 0: a **forced obstacle** (always in the set), not an ignore id.
   - `MovementBoxExpansionFactor` f > 0 → the extra clearance f (item 3).
   - `flags`: bit 1 = a standing non-enemy unit (`0x2824f0` false) is an obstacle only if it shares with u a
     collision record that is still undecided or detouring (record vf +0x58: MvS state 0 / 1, MvM state 0 / 1 / 4);
     bit 2 = the same for every standing unit; bit 4 = standing units of collision classes 2-4 (the obstacle
     classes) are ignored. Deployed / deploying units are always obstacles (precise rule in item 3).
   - `kind` only becomes the pushed path's kind (3 TerrainCollision, 4 normal, 7 crush-flee); kind 3 adds **40.0**
     to D and skips the last-point drop of item 5.
2. **Distance D, base path, rejoin point.**
   - D: no obstacle → r_u (`0x3969f0`). flag 0 → |X − u.P| + r_u, X = where u's travel ray (the direction of its
     first approximation segment, else its facing) leaves o's swept rectangle (`0x67c1a0`); when that fails |u.P −
     o.P| + r_u + r_o. flag 1 → max(|u.P − o.P|, |F − u.P|) + r_u + r_o, F = the front centre of o's swept rectangle
     (`0x67c0b0`).
   - Base path (`0x676c10`): a copy of u's **top** path record; u must have a kind-1 (order / formation) record on
     its stack, else the call fails. A formation path is first converted to u's own slot path (`0x413340`); it
     fails when `0x41cf40`(formation) is true [L: meaning]. When the top record is not the kind-1 record, one point
     is appended 1 unit past the end along the kind-1 record's current segment.
   - Rejoin candidates (`0x67ad20`), in this order, at most **11** tried by `0x675ed0`:
     (a) path points at arc length s0, s0 + 10, s0 + 20, … while < the path length, s0 = (length up to the current
     waypoint) + clamp(projection of u on the current segment, 0, segment length) + D + **2.0**; the "next
     waypoint" N of an accepted candidate is the path point after that segment's start;
     (b) past the path end E: E + dir·s in steps of 10, dir / limit = toward u's current target up to its distance,
     else the last segment's direction up to GC +0x14e4 (`LandTemporaryDestinationProximity`, 20); a candidate
     needs a land path E → spot no longer than 2 × the straight distance (`0x679e20`); N = none;
     (c) the ring search `0x440f90` around E (occupancy margin `0x440f30` = max(6 × `FormationSpacing`, 0.25 r_u)):
     the first spot not behind E along dir, with the same 2 × test; N = none.
     It fails outright when the current waypoint has no next point (u is on its last waypoint).
   - A candidate is accepted (`0x679aa0`) when the nearest passable point Q (ring search) lies within GC +0x14e8
     (`LandDestinationProximity`, 60) of it and u's rectangle (hx, hy) placed at Q, axis along Q − E (u's facing
     when zero), overlaps no layer-0x10 record that is an obstacle under mask +0x3c. A goal inside any obstacle's
     grown rectangle, or one for which 5 window sizes fail, moves on to the next candidate.
   - So with a **blocked destination** the rejoin point slides forward along the path, then up to 20 past its end
     (or toward the target), then to the nearest free reachable spot around the end.
3. **Obstacle set** (`0x67d580` over the layer-0x10 box query `0x5d7a00` of the window; FIGHTER_FLYING §7.8: squad
   members are never in it).
   - Never: u itself; anything u can crush (`0x5c8900`); a pair that fails the class table of §5.2 item 6
     (`0x505020`); `0x509370` false [M: flying multi-locomotor objects].
   - Always: the forced id; the slaves of u's MovingVsMoving records in state 4 (`0x505e10`) [M: `0x505630` adds
     further MvM partners].
   - Otherwise by the relation code `0x679750` (§5.4.2): 0 standing → obstacle, subject to the `flags` of item 1;
     2 (moving but held at speed 0), 4 (head-on), 8 (crossing), 0x20 (same way, slower) → obstacle; 0x10 (same way,
     not slower) → **ignored**, unless its approximation reaches its path end (pack +0x28c), it is not behind u's
     facing and the pair has an undecided / detouring record.
   - **Order of the tests and the deployed rule (s78n t26 RE 2, `0x67d580`) [H].** (1) ignored: null, u, crushable,
     `0x509370` false, `0x505020` false; (2) obstacle: the forced id list (global `0x140b3a1a0`), then the MvM
     partners (`0x505630`); (3) the request byte +0x38 "skip a smaller collision class" (global `0x140a443c8`;
     default 1 in `0x673ac0`, but **`0x5c80e0` sets it to 0**, so it is off for every Path Around call); (4) mask
     bit 0 (Infantry, class 5, not in a hero clash +0x3b4) → ignored; (5) relation `0x679750` = 0 (standing: neither
     locomotor vf +0x1a8 nor vf +0x1b0 reports moving) **and** (deployed `0x39a390` or deploying `0x39a400`) →
     **obstacle unconditionally, before the `flags` bits 4 / 2 / 1, with no Movement_Speed (M ≤ 0) clause**;
     (6) everything else by relation as above. So "always" in item 1 means "a *standing* unit that is deployed or
     deploying". A walking-deployed droid (+0x2b3 set while it has a moving path) is not standing and is judged by
     its relation. In the collision code `0x39a390` is called only by `0x67d580`, the push `0x6859a0` and the SvS
     chooser `0x687010`; **only the last two** have the M ≤ 0 clause (type +0x23b0 ≤ 0 && (D || deploying)).
   - Shape (`0x677650`): the footprint rectangle `0x38de80` with **both half extents + 1.0**. A mover turning in
     place by more than 10°: a square of max(hx, hy). A mover closing on u (both max speeds > 0, closing speed c >
     0): the rectangle **swept** from its centre along its approximation by L = min(max(0, (d − r_u − r_o) / c), 30
     × `MovingVsMovingLookAheadTime`) × its max speed, hy widened to cover the approximation points passed; in
     relation 0x10 / 0x20 with the approximation reaching the path end, L = the whole approximation.
   - Clearance: rect1 = shape + (hy_u (`0x396b70`) + 0.5) on both axes; rect2 = rect1 + f; the temporary tracker
     records used for "inside an obstacle" tests = shape + min(hx_u, hy_u) + 0.5 + f. **u is a point; all obstacles
     are oriented rectangles; there are no circles.**
4. **Search.**
   - Window (`0x67a0d0`): the cell bounding box of start and goal ± (`DynamicAvoidanceRectangleBound` 3 + **2 only
     for Giant**); aspect > 1.5 → y0 −= k, y1 += k with k = int(ratio − 0.5); aspect < 2/3 → **x0 −= k and y1 += k**
     (as coded), k = int(1 / ratio − 0.5); clamped to the grid. `0x67cf20` then grows each side by one cell while a
     layer record overlaps a strip 10 units outside that side, up to 20 cells per dimension. Retries 2-5 add 3
     cells on every side (the plain variant `0x675240` with an explicit goal: up to 20 sizes).
   - Feasibility (`0x677e40`): a 4-neighbour flood fill over the window's cells passable for u's movement class,
     from the start cell; the goal cell must be reached.
   - Vertices, in this fixed order: (i) terrain corners: reached passable cells on a convex corner (diagonal
     neighbour blocked, both adjacent orthogonals free) or a concave corner of the impassable area, at cell index ×
     cell size, row-major [M: the staircase pruning masks]; (ii) per obstacle in query order, corners (+a +b), (+a
     −b), (−a −b), (−a +b) of rect1, each kept when the matching rect2 corner is passable (`0x50e280`) and inside no
     other obstacle's tracker rectangle; (iii) two "escape" vertices on the nearest edge when the start is inside a
     relation-0x10 obstacle's rect2 [M]; (iv) the start (inside another obstacle the search starts at the nearest
     vertex instead); (v) the goal (inside a rect2: the nearest vertex).
   - Edges (`0x678e80`), every pair i < j: not the two diagonals of one rectangle; between vertices of
     **different** obstacles (or a corner and the start / goal / a terrain corner) not when the direction from a
     corner lies within 44° of that corner's **inward** bisector (item 4a; *corrected s78n t20: this said
     "outward diagonal"*); the terrain line test `0x5a4c10` for u's movement class must pass.
   - Cost multiplier (`0x67a3d0`): 1, or **`DynamicObstacleOverlapPenalty` (15)** plus a "crossing" mark when the
     segment crosses any obstacle other than the two end points' own (item 4a); the front edge (corners +a ±b) of
     an obstacle in relation 2 / 4 / 8 / 0x20 is always 1.
   - A\* (`0x67b8a0`): g = Σ 3-D length × multiplier, h = straight distance to the goal; binary heap on f (insert
     stops at parent.f ≤ f; sift-down swaps only on a strictly smaller child, left before right); neighbours in
     ascending vertex index; closed vertices are not reopened. **Pass 1 skips every edge with the crossing mark;
     only when it fails, pass 2 (pathfinder +0x143 = 1) allows them at × 15.**
   - Budget: each pass adds the vertex count to the complexity counter +0x13c. **Nothing aborts a running search**;
     the quota only decides, in the driver (§5.4.1), whether a state-0 record may decide this frame.
   - Failure: u's max speed 0, no base path / no accepted rejoin candidate, 5 windows without a feasible flood
     fill, start = goal, the heap runs empty or the node pool (3 × vertices) is exhausted.
4a. **Edge and cost rules in full (s78n t20 RE, 2026-11-10; `0x675240` / `0x675ed0` vertex creation, `0x678e80`,
   `0x67a3d0`, tracker `0x6b4810` / `0x6b3b00` / `0x6b1d00`, rectangle helpers `0x20d1c0`…`0x20d2b0`, `0x20d690` /
   `0x20d710`) [H unless tagged].**
   - **Vertex record.** Type (+0x18): 3 = corner (+a +b), 2 = (+a −b), 1 = (−a −b), 0 = (−a +b) (created in this
     order; a = the obstacle rectangle's first axis, for a unit its facing); 5 = start; 6 = goal; 7 / 8 = the two
     escape vertices [M: 7 on the +b side, 8 on the −b side, from which corners they may link to]; terrain corners
     have another type with no obstacle. Obstacle pointer (+0x20): the obstacle for types 0-3 and 7 / 8, null
     otherwise. Two positions: the rect1 corner (+0x00) and the **rect2 corner (+0x0c)**; the edge tests and the
     crossing test use the rect2 corner (the same point while f = 0). Corner vector n (+0x30):
     the unit vector of (−sa·a − sb·b) for the corner at (sa·ha, sb·hb), sa, sb = ±1. It points from the corner
     **into** the rectangle at 45° to its axes (it is not the true diagonal to the centre unless the rectangle is
     square).
   - **Pairs with the same obstacle pointer** (two corners of one rectangle; also two obstacle-less vertices such
     as start and goal, or a terrain corner and the start): **no cone test.** No edge for the two diagonals (0-2,
     1-3). The four sides are edges. [M] escape vertices: 7 never links to corner 1, 2 or to 8; 8 never links to
     corner 0, 3 or to 7. Then the terrain line test.
   - **Pairs with different obstacle pointers** (corner ↔ corner of another obstacle, corner ↔ start / goal /
     terrain corner). With d = the 2-D unit vector from the corner V to the other vertex W (fast inverse square
     root, two Newton steps): **no edge when d · n_V ≥ cos(0.7679449 rad) = cos 44° = 0.71934** (`cosf` 0x776150 of
     the float 0x3f449809). Tested from V when V is a corner (types 0-3), and from W whenever W has an obstacle. So
     exactly one cone per corner is excluded: the **inward** one, 44° either side of n, i.e. the rectangle's
     interior quadrant less 1° at each side. The outward cone is **not** excluded. An escape vertex (7 / 8) as the
     lower index links only to the start; as the higher index it also needs the other vertex on the negative
     side of its own vector (dot < −0.001). Then the terrain line test.
   - [M] When the start lies inside some obstacle's rect2 and pathfinder +0x194 is set, corners and terrain
     corners get no edge to the start (only escape vertices and the goal do), and the direct start-goal edge
     costs × 15 **without** the crossing mark. Not read: what sets +0x194.
   - **Crossing test** (`0x6b1d00`): the segment as a zero-width oriented box against the temporary tracker
     records (built once per graph by `0x6b4810`: the shape of item 3 with both half extents + min(hx_u, hy_u) +
     0.5, + f when the expansion flag is set; **not rect1**). Separating-axis test with a closed comparison, so
     **touching counts**. Records whose id equals either end vertex's id are **skipped inside the query**: the id
     of a corner / escape vertex is its obstacle's id, of the start is u's own id, of the goal or a terrain corner
     none. The ids of the records met go to a list.
   - **Multiplier** (`0x67a3d0`, one call per direction; the result is the same both ways): 1.0 when the list is
     empty. Otherwise each listed object that is not u, not the from-vertex's obstacle and not the to-vertex's
     obstacle makes the edge **crossing**: multiplier := GC `DynamicObstacleOverlapPenalty` (15), crossing mark
     (edge +0x10) := 1. "Own" therefore = the obstacle objects of the edge's two end vertices plus u; the start
     and the goal own nothing else. Exceptions decided before the list is looked at: the side between corners 2
     and 3 of one obstacle whose relation code (`0x679750`) is 2, 4, 8 or 0x20 → 1.0; an edge between an escape
     vertex and the start → 1.0.
   - **Consequence.** An edge can never leave a corner into its own rectangle: the inward cone removes it in both
     passes, before any cost is looked at. The "own obstacle is free" rule only covers what the cone leaves: the
     four sides, and the 1° slivers along them. An edge that crosses a **third** obstacle exists, marked, at × 15,
     and is usable in pass 2 only.
   - **Worked example.** One standing obstacle, rect1 = rect2 = [−10, 10] × [−5, 5], a = +x, start S = (20, 0),
     goal G behind the centre, e.g. (−30, 0); corners c3 = (10, 5), c2 = (10, −5), c1 = (−10, −5), c0 = (−10, 5),
     n(c3) = (−0.7071, −0.7071), n(c0) = (0.7071, −0.7071).
     - S ↔ c3 (front corner): d from c3 = (0.894, −0.447), d · n = −0.316 < 0.7193 → **edge, × 1**.
     - S ↔ c0 (rear corner, the segment passes through the rectangle): d from c0 = (0.986, −0.164), d · n = 0.814
       ≥ 0.7193 → **no edge, in either pass**.
     - c3 ↔ G: d from c3 = (−0.992, −0.124), d · n = 0.789 ≥ 0.7193 → **no edge**.
     - c3 ↔ c0 (a side of one rectangle): **edge, × 1**. c0 ↔ G: d · n = −0.514 → **edge, × 1**.
     - S ↔ G direct: both obstacle-less, so an edge whenever the terrain line passes; the obstacle's tracker
       record is met → **× 15, crossing**, pass 2 only.
     - Pass 1 result: S, (10, 5), (−10, 5), G (or its mirror image; the tie falls to the A\* rules of item 4),
       cost = the sum of the three lengths, no crossing.
5. **Output and splice.**
   - Points (`0x674870`) = the vertex positions start → goal, **unsmoothed**; started from a nearest vertex: the
     real start and the foot of the start on the first segment first; goal inside an obstacle: one more point 1
     unit past the last.
   - A waypoint that is a front corner of obstacle O carries O's id (waypoint +0x2c) and a **hold line** (point =
     corner − (hx_O + hx_u)·a_O, direction a_O): what the MovingVsMoving states 1 / 4 / 16 test (§5.4.6).
   - `0x6744a0` fills the path's id list (what `0x5501d0` tests in §5.4.1 decision step 3) with every obstacle in
     the window that is not moving or lies behind the detour's end.
   - In `0x5c80e0`: the path needs ≥ 2 points and the record's vf +0x80 (true, except MvM state 4: the first
     segment must not oppose the master's facing, dot ≥ −0.5). **Last-point drop:** kind ≠ 3, more than 2 points,
     N exists, the terrain line from the second-to-last point P to N passes, and (flag 1 only) P is more than 60.0
     ahead of F − (hx_u + hx_o)·a_o along a_o → the rejoin point is removed (u heads from the last corner straight
     to its next original waypoint).
   - Claim (locomotor vf +0x178 = `0x42cdd0`): free, or the record's weight (vf +0x30) exceeds the owner's;
     otherwise the owner is released (vf +0x188 / +0x180) and the claim repeated. A claim by a new owner **removes
     every collision-owned record from u's path stack** (`0x559410`: an older detour, push or StoppedVsStopped
     path), stamping pack +0x288.
   - Push (vf +0x190 → `0x554310`) on top of the stack, started flag +0x6d := 1, a unit standing in state 0x16 /
     0x25 → state 0; record +0x58 := the detour's last point.
   - **The ordered path stays underneath, untouched.** When the detour's last waypoint is passed, the Walk step
     (`0x620820`) pops it and, in the same step, advances the old record's waypoint index past every waypoint line
     the unit has already crossed, then walks on along it.
6. **Spare point `0x5c6ba0`(a, b, &v)** → `0x67b4c0`: v = the **first accepted rejoin candidate of item 2**, with D =
   |a.P − b.P| + r_a + r_b (3-D distance, `0x3969f0` radii), both masks 0x10, no forced obstacle, no flags. No
   window, graph or search runs and no complexity is spent. Returns false with v = 0 when a or b is missing, a has
   no kind-1 record, its current waypoint is the last one, or no candidate passes. There is no single maximum
   distance: along the path to its end, then ≤ 20 past it (or to the target), then the 20-ring search.
7. **Failure as seen by the callers:** `0x5c80e0` returns false (no pack, pathfinder failure, < 2 points, vf +0x80
   false). The unit's path stack, owner and state are unchanged.

***Port spec (Path Around):*** a detour is a visibility-graph search from the unit (a point) to a rejoin point on
its own current path. Rejoin point: the first free spot at path distance ≥ progress + D + 2, stepping 10; then up
to 20 past the end (toward the target if any); then the nearest free reachable spot around the end; at most 11
candidates; none when the unit is already on its last waypoint. Obstacles: the layer objects of item 3 (never
squad members; crushable units, same-way-not-slower movers and, for Infantry / Giant movers, Infantry are ignored;
the named obstacle is forced when flag 1) as oriented rectangles of half extents (hx + 1, hy + 1), swept for a
closing mover, grown by hy_mover + 0.5 (+ `MovementBoxExpansionFactor`). Nodes: the grown rectangles' corners (kept
when passable and not inside another obstacle), convex / concave corner cells of impassable terrain in the window,
start and goal, in the order of item 4. Edges: terrain-passable segments, not a rectangle's diagonal, and (between
different obstacles, or a corner and the start / goal) not from a corner into its **inward** 44° cone (item 4a:
d · n ≥ 0.71934, n = the inward 45° bisector); a rectangle's own sides are always edges and its own rectangle
never makes an edge "crossing". A\* with the tie rules of item 4; first without segments that cross a third
obstacle, and only if that fails with them at × 15. Emit the corners unsmoothed, tag front-corner waypoints with
the obstacle id and hold line, drop the rejoin point when the last corner sees the next original waypoint. Push the
detour above the ordered path after removing older collision-owned paths; when it ends, resume the ordered path at
the first waypoint line not yet crossed. Failure leaves the unit untouched (the callers then push, hold or retry,
§5.4.1 / §5.4.6). [port] the plain window numbers and the corner-pruning masks may differ; record them.

*Not read:* the tracker's tree build and point query (`0x6b4cb0`, `0x6b3580`, `0x6b6070`; the record contents and
the segment query are in item 4a), `0x41cf40`, `0x509370`, where the escape vertices are placed and what sets
pathfinder +0x194, the exact terrain-corner pruning masks, whether the emitted points are the rect1 or the rect2
corners when f > 0.

### 1.6 Request budgets, staggering and re-path triggers

**No global per-tick cap on land path requests was found.** The only hard budget is the dynamic quota of §1.5. [M]

**Start staggering** [H] (corrected in cu8j, 2026-11-08, from 0x43c1b0 / 0x43e5b0 / 0x41c1a0; §4.6):
- In `0x43c1b0` (land split, coordinator vf12 → composition, §4.6.1), for each formation seed whose type has
  `PathFrameDelay` (+0x3d0) ≠ 0 (and the caller's stagger flag, 1 from 0x43e5b0): r' = synced `int(0,
  PathFrameDelay_seed)` (call 0x43c8b9: edx = 0, r8d = the seed type's +0x3d0), descriptor +0x1cc := now + acc + r' −
  2·FPS, then acc += the seed type's `PathFrameDelay`. acc starts at 0 per split. A seed whose type has
  `PathFrameDelay` 0 draws nothing and leaves descriptor +0x1cc = 0 (0x40d660 zeroes it). In FoC only `Snowspeeder` and
  `Lancet_Air_Artillery` set `PathFrameDelay` (4).
- The formation made from the descriptor gets F +0x1dc := descriptor +0x1cc, overwriting the ctor's now − 500.
  F +0x1dc (FormationData +0x1cc) is the **last plan frame**: plan `0x41c1a0` path-plans only when
  2·FPS < now − F +0x1dc (and on a failed plan with param_2 > 1 sets F +0x1dc := now). So a staggered formation
  first plans at the first frame > creation frame + acc + r'; an unstaggered land formation plans at once when it is
  created after frame 60, **and waits until frame 61 when it is created at frame ≤ 60** [M: consequence of the gate;
  no trace].
- The **FormationClass ctor draw** (`0x40d0d0`, `int(1, FPS)`, one per formation creation, §4.1) is a different
  draw. Its r is used only by the next-check timer F +0x1d8 := now + FPS + r (§4.6.3), never by the start frame.
  **Correction:** the cu8i reading "start = now + acc + (FPS + r) − 2·FPS" (docs/PHASE4.md cu8i t02, and this
  section until cu8j) mixed the two; the start offset is r' above, drawn only for `PathFrameDelay` seeds.
- Draw order of one land split: all r' draws (descriptor order, inside 0x43c1b0), then one ctor draw per
  descriptor in descriptor order (0x43e5b0). Both happen at the coordinator update (common tick step 10, §4.6.2),
  not when the order is given.
- The queued-order delay is drawn in plan `0x41c1a0`, not at the order: per member, synced `int(0,
  Max_Move_Frame_Delay = 6)` (GC 0x140b16d38) when the top block has flag 0x08, the member's pack speed is 0, it is
  not a team, and (member count = 1, or land with skip-formup and F +0x9c clear); stored to the member's path
  +0x54 := now + r.
- These draws consume the lockstep RNG, so their count and order must match.

**Re-plan gate:** a coordinator re-plans when (frame − last plan) > 2 × logic fps, i.e. at most every 60 frames. [M]

**Unread constants:** `MovementReevaluationFrameCount = 10` is used only by the space coordinator (§2.5). `MaxWaypointsPerPath = 6` is read only in `0x420f70` (not traced).

**Re-path triggers:**
- a new order;
- the coordinator's regroup flag (+0xa8, set when a member joins);
- the target dying (the goal becomes a position, §6.3);
- a failed per-step passability check (the locomotor reports to the collision system and stops, §3.5);
- collision resolution states "Path Around" (§5.4).

### 1.7 Land constants (vanilla GameConstants.xml)

| constant | value | read where |
|---|---|---|
| DynamicLandComplexityQuota | 100 | `0x679740` [H] |
| DynamicLandQuotaResetInterval | 5 | `0x67cef0` [H] |
| DynamicAvoidanceRectangleBound | 3 | `0x67a0d0` [H] |
| DynamicObstacleOverlapPenalty | 15.0 | `0x67a3d0` [M] |
| LandDestinationProximity / LandTemporaryDestinationProximity | 60 / 20 | `0x67ad20`, `0x679aa0` [M] |
| Max_Move_Frame_Delay | 6 | `0x41c1a0` [H] |
| MaxWaypointsPerPath | 6 | `0x420f70` [L] |
| XYExpansionDistanceLand | 60 | lattice planner, land branch (§2.1) [M] |
| MinObstacleCostLand / MaxObstacleCostLand | 3 / 3 | lattice planner, land branch [M]; Max is unread |
| LandWaitOperatorSpeedCoefficient | 0.65 | lattice planner, land branch [M] |
| ApproximationSmoothCosAngle / ApproximationForwardDistance | 0.9962 / 2.0 | path approximation `0x505fe0` (§5.2) [H] |
| FramesPerPositionApproximationRebuild | 8 | `0x508170` [H] |
| LandPredictionTimeInterval / MinLandPredictionDistance / SyncedFrameInterval | 2.0 / 60 / 75 | prediction code near `0x3972a0` [L] |
| Preferred_Pathfinder_Types | list of company types | galactic → tactical lead-unit choice, `0x2bd500` [L] |

---

## 2. Space navigation

### 2.1 Planner (`SpacePathFinderClass`): kinematic A\* [H] unless tagged

**Entry points:**

| address | role |
|---|---|
| `0x59a900` | search |
| `0x59d4a0` | per-request setup |
| `0x596d40` | operator generator |
| `0x599170` | distance → time integrator (trapezoidal accel/decel) |
| `0x599380` | operator cost modifier |
| `0x597dc0` | path reconstruction ("Build_Smoothed_Path") |
| `0x59e700` | trivial path, when start and goal share a cell |

The planner is planar: Z is not part of the state. Every branch on mode type 1 substitutes the land constants (XYExpansionDistanceLand, LandWaitOperatorSpeedCoefficient, MinObstacleCostLand). Whether land coordinators actually route ground units through it, rather than the region A\* of §1.4, is [M]. The coordinator entry `0x417520` contains both a land and a space branch.

**Request record** (defaults `0x5967d0`, filled from the object by `0x59d120`):

| field | content |
|---|---|
| +0xb8 / +0xc4 | start / goal xyz |
| +0xd0 | object |
| +0xe0 | start heading, degrees (object yaw +0x8c) |
| +0xec | goal facing |
| +0xf0 | max speed (locomotor vf +0x250) |
| +0xf4 | turn rate (locomotor vf +0x258) |
| +0xf8 | radius (`0x397780`) |
| +0xfc | expansion budget = SpacePathfindMaxExpansions |
| +0x110 | heuristic divisor, 1.0 |
| +0x114 | weighting distance, 0 |
| +0x118 | forward-step scale, 1.0 |
| +0x11c | rotation divisor, 1.0 |
| +0x120 | final facing required |
| +0x121 | threat mode |
| +0x123 | wait operators allowed, 1 |
| +0x128 | obstacle mask, 0x3f |
| +0x12d / +0x12e / +0x12f | flags, 1 / 1 / 1; +0x12e and +0x12f are forced false in land mode |

**Derived values** (`0x59d4a0`):
- **Heading buckets.** N = MaxRotationsSpace (24) / rotation divisor. Turn step = 360/N = **15°** by default. Time per turn step = step / turn rate.
- **Forward step D.**
  - D = max(speed, XYExpansionDistanceSpace (300) × step scale).
  - If the start–goal distance is < 2D, D = max(50, distance/2).
  - In threat mode, D = ThreatExpansionDistance (600).
- **Turn radius.** R = speed × (frames for 360°) / 2π. The slow-turn radius is R × the wait-speed coefficient: **WaitOperatorSpeedCoefficient 0.2** in space, **0.65** in land.
- **Accel/decel** come from the locomotor (vf +0x2d8 / +0x2e0, see §3.4). Default = speed.
- **Escape headings.** If the start overlaps an obstacle, probe headings in **10°** steps up to ±180° for the first clear one, on each side.
- **Occupancy maps** come from the mode's object-tracking system (§2.2): the map of the unit's own space layer, and layer 8, the static map.

**Quantisation:**
- cell = floor((p − start) × 1.5 / D) on x and y. This is floor, so negative values round down.
- bucket = floor(normalised heading × (N + 1) / 360).
- **Goal test:** the node is in the goal cell, and, when facing is required, also in the goal bucket or within **MatchFacingDeltaSpace = 10°** of the goal facing.

**Containers (determinism)** [H]:
- Nodes are 0x28 bytes from a fixed pool.
- The open list is a 1-based binary min-heap on f, with **the same tie rules as §1.4**: sift-up stops on parent ≤ child, and sift-down swaps only with a strictly smaller child. **A push into a full heap is silently dropped.**
- The visited set is a hash on the integer triple (cx, cy, bucket): 25 000 buckets, 50 000 entries (`0x59ebb0`), lazily invalidated by a generation stamp, storing best g and a closed flag.
- A closed state is re-expanded only by operators 1, 4, 5, 6, 7, 8, 10, 11, 14, 15 and 17–21, and only when the new g is not worse.

**Search loop:**
1. Pop the minimum-f node. If it is the goal, build the path.
2. Otherwise apply the active operators in index order:
   - default: 0–7;
   - 8–9 only on the first pop, when +0x12e is set;
   - facing mode: 10–24 on the first pop, then 10–22;
   - threat mode: 25–32.
3. Each successor that is produced is costed and pushed. The budget counter counts pushes, and the search fails when the counter reaches +0xfc or the heap empties.

**Operators** (`0x596d40`):

| op | effect |
|---|---|
| 0 | straight ahead by min(distance to goal, D) |
| 1 | arc toward the goal; only if the heading error is between 0.01° and one turn step |
| 2 / 13 | arc left one turn step |
| 3 / 14 | arc right one turn step |
| 4 | straight to the goal; only after op 1 |
| 5 / 6 / 19 / 20 | change speed to max, or down to wait speed; distance \|v² − v0²\|/(2a), time \|Δv\|/a |
| 7 / 18 | wait: travel WaitOperatorBaseFrameTime = 100 frames at wait speed (needs +0x123) |
| 8 / 23, 9 / 24 | turn in place to the escape heading (time = angle / turn rate); first expansion only |
| 10 | full step D |
| 11 / 12 | tangent onto the goal-facing circle, left / right (`0x59c640`) |
| 15 / 16 | follow-on tangents |
| 17 / 21 / 22 | final arcs onto the goal facing (`0x59c1c0`) |
| 25–32 | eight compass moves (table `0x140a43b80`) by min(distance, D); threat mode |

Ops 6, 7, 15–18 and 20–22 run at the wait speed (max × 0.2 in space).

**Cost:**
- g = parent g + Δtime (frames) × the operator multiplier.
- h = straight-line distance to the goal / speed.
- In facing mode, h = turn-arc length (radius × angle, × **FinalFacing180Penalty 1.5** when the angle > π) + tangent length × **SpecialAlignedOperatorBonus 0.6**.
- f = g + w·h, where w = 1/(+0x110) until the node's travelled distance reaches +0x114, then 1.

**Operator multipliers** (`0x599380`):

| ops | multiplier |
|---|---|
| 1 | 0.99 |
| 4 | 0.98 |
| 2, 3, 13, 14 | 1 + 0.5 / max(1, distance to goal / 400) |
| 7, 18 | WaitOperatorCostCoefficient 0.8 |
| 11, 12, 15, 16 | 0.6 |
| 17, 21, 22 | the wait-speed coefficient |
| 25–32 | (threat·weight + 1) when threat ≥ threshold (request +0x100…+0x10c; threat map `0x140b31900`) |

### 2.2 Obstacles, layers and the occupancy structure [H]

**Object-tracking system** (mode vf +0x238; slice setup `0x5d8140`):
- A ring of time slices: **SpaceObjectTrackingTreeCount = 45** slices × **SpaceObjectTrackingInterval = 90** frames in space (≈ 4050 frames of look-ahead), and 25 × 20 on land.
- There is one set per layer. Moving objects are inserted as swept capsules along their planned segment within each slice.
- Queries interpolate segments by time (`0x6b24f0`) and **OR the type bits of every hit**, so the result does not depend on traversal order.

**Entry bits = SpaceCollisionType** (`0x5eacd0`):

| bit | meaning |
|---|---|
| 1 | Moving (segment start ≠ end) |
| 2 | Static |
| 4 | Asteroid (type byte +0x54) |
| 8 | Ion_Storm (+0x56) |
| 0x10 | Nebula (+0x57) |
| 0x20 | impassable asteroid (`Is_Impassable_Asteroid` +0x55; name string not recovered) |

**Space footprint** is a circle centred at position + `Space_Obstacle_Offset` (+0x16e4) rotated by yaw.
- Radius getter `0x373b50` = Scale_Factor × 2R/20, where R is the first positive value of `Custom_Soft_Footprint_Radius` (+0x16b8), `Space_Obstacle_Radius` (+0x16f0), or the larger model x/y extent.
- The /20 conversion is unexplained. [M]
- **Model-extent fallback** (2026-11-01, S7 t08) [H]: the getter takes the type's model for the current mode (`0x3729f0`; in space, mode 2, type +0x2a8), asks it for its local box (ModelClass vf +0x58 = `0x264920`, the **collision bound**: the bind-pose union of the collision-enabled ALO meshes, SPACE_AVOIDANCE §3.2), and takes max(|half x|, |half y|). The two XML radii replace it when positive. A model without a collision mesh, or whose render object is not an ALO model, gives half 1; a type with no model for the mode gives radius 0.
  - So `Orbital_Construction_Pod` (`EB_construction_pod.alo`) and its capture-point variants `N_Orbital_Construction_Pod` / `Skirmish_Merchant_Dock` (no radius tag in FoC) are sized by their model.
- `SpaceObstacleBehaviorClass` (vftable `0x8b0988`; vf3/vf13 `0x632610`, vf9 `0x632670`, vf15 `0x6326b0`) inserts the object as a static entry on attach and removes it on detach.
  - The insert (`0x53acf0`) skips a contained object (obj +0x3a7) and, in space, removes a type with `Use_Special_Submit_Rules` (FoC: transports only). It does **not** read the model's collide switch, so capture points and orbital props stay planner obstacles (SPACE_AVOIDANCE §5.5).
  - Slots (vftable read from the exe, 2026-11-02): vf3 `0x632660` answers id 0x4C; vf6 = `0x17d070` (no service); vf9 `0x632670` (initialise: base `0x4c2f90`, then insert); vf13 `0x632610` (the "placed" broadcast of §1.3: insert again); vf15 `0x6326b0` (detach: remove, `0x539470`). [H]
- **Death clones and dying objects** (2026-11-02, S7 t16) [H unless tagged]:
  - **A death clone never holds a static entry.** `0x29f270` detaches and removes every behaviour of a new clone except HIDE_WHEN_FOGGED, IMPOSING_PRESENCE and (land only in effect) LAND_OBSTACLE (§1.3 "Death clones and wrecks"). SPACE_OBSTACLE (0x4C) is always among the removed ones: its vf9 insert at the create is undone by its vf15 remove in the same call. In FoC no clone type lists SPACE_OBSTACLE anyway (the starbase, station and ship `*_Death_Clone` types have no behaviour lists), but an **own-type** clone (a SPACE_OBSTACLE type without a `Death_Clone` list) would have it, and it is stripped the same way.
  - **The dying object keeps its static entry until it is destroyed.** `Die` / `Kill` do not detach behaviours; the entry is removed by SPACE_OBSTACLE's vf15 when the GameObject destructor (`0x3893e0` → `0x3a52f0`) runs, normally at the pending-delete flush at the end of the frame of death [M for "normally", as in §1.3]. Other removals of the entry: entering a container (`0x37ade0`) and the moving re-insert (`0x53a900`, `0x5d4730`).
  - Not part of SpaceOccupancy, for completeness: an own-type clone keeps its type's `Space_Layer`, so it can enter the fighter-avoidance tree at create (SPACE_AVOIDANCE §2.1 tests no death-clone flag; the clone flags are set after the create) [M].
  - **Port (S7 t16):** `space_obstacles_sync` walks battle units, not the behaviour table, so it never inserts a death clone: this matches retail. It removes a unit's entry at the first sync after the unit died or retreated, which matches the destructor at the end of the frame of death. No code change is needed. If the sync is ever switched to the SPACE_OBSTACLE behaviour table, it must skip death clones (store flag `kFlagDeathClone`).

**Layers** (`0x397900`):
- The unit's layer is `Space_Layer` (+0x16dc), or `Stationary_Space_Layer` (+0x16e0) while not moving.
- SpaceLayerType (`0x5e0c00`): Capital 1, Frigate 2, Corvette 4, StaticObject 8, Land 0x10, SuperCapital 0x20 (corrected per FIGHTER_FLYING §6.3.1).
- **A ship plans only around entries in its own layer, plus the static map (layer 8).** Ships in different layers pass through each other in planning.
- A `Uses_Multiple_Locomotors` unit whose current locomotor answers vf +0x240 (JetPack yes, Walk no) with kind 2 (JetPack pack state 0 or 0xF–0x13) uses layer 4 (`0x397900`; FIGHTER_FLYING §6.3.1, §3.1.1). [H]

**Obstacle test** (`0x59a630` → `0x59a860` → `0x5d7c00`):
1. Each motion is swept in space-time against the unit's layer map, plus the static map when the mask has any of the bits 0x3e. The static test is offset by SpaceStaticObstacleAvoidanceBonusDistance (0 in vanilla).
2. A speed-change operator that brakes early is split at the deceleration point.
3. Hits with bits outside the mask are free.
4. Hits inside the mask reject the step when the hit involves bits other than Moving, it lies beyond radius × **OccupationRadiusCoefficientSpace 1.2** + R from the start, and +0x12f is set. They also reject ops 4, 15 and 16.
5. Otherwise the step's cost is multiplied by **MinObstacleCostSpace / CurrentPathCostCoefficientSpace = 15 / 0.66**; on land it is 3 / 0.66.

Never read by code (no xrefs) [H]: **MaxObstacleCostSpace**, **ObstacleAreaOverlapForMaxSpace**, **UseLinearCollisionChecks**. Parse them and ignore them.

**Lua avoidance argument** (`Move_To` etc., `Move_Internal` `0x57da90`, SpaceTaskForce `0x6e4fc0`/`0x6e54d0`/`0x6e72c0`/`0x6e7940`): the string is parsed as a SpaceCollisionType bitfield, and the mask becomes its **complement**, so the listed types are ignored. Default 0x3f. [H]

### 2.3 Playable box [H]

The box is the mode's box (vf +0x208): `.ted` `0x103` corners, ±6000 typical in space.
- **Planner** (`0x59e5b0`):
  - A successor outside the box is rejected for ops 25–32.
  - It is also rejected when the start and goal are both inside and the step is far from the start (beyond radius × 1.2 + R + braking distance + 100), and for ops 4, 15 and 16.
  - Otherwise its cost is multiplied by **OffMapCostPenalty = 20**.
  - Net effect: excursions outside the box happen only near the endpoints or when an endpoint is itself outside.
- **Order destinations** are clamped to the box in x and y, and to ±5000 in z (`0x583150`, §6.1).
- **Land grid cells** outside the box are Obstacle (§1.1).
- **Reinforcement drops** must be inside the box (§2.6).

### 2.4 Retry policy, destination search and path output

**Retries** (`0x417520`, space branch) [H]. There are up to **SpacePathingTries = 6** attempts.
- **From try 2:**
  - rotation divisor += **0.5** (SpacePathFailureRotationExpansionIncrement), giving coarser headings;
  - step scale += **0.5** (…ForwardExpansionIncrement);
  - budget × **1.7** (…MaxExpansionsCoefficient; the base budget is **3500**);
  - the facing requirement is dropped.
- **From try 3:**
  - the weighting distance becomes max(previous + **0.25** (…DistanceCutoffCoefficient) × straight distance, furthest progress so far);
  - the heuristic divisor is multiplied by **CurrentPathCostCoefficientSpace 0.66** (greedier).
- **From try 4:** the mask becomes 3 (only Moving and Static block); +0x12c is set if the mask was 0x3f.
- **From try 5:** the request speed is halved. [M]
- Moves shorter than 40 units skip planning. [M]

**Destination search** (`0x59caa0`) [H]:
1. The clearance is radius × 1.2, optionally plus a turn-radius term.
2. If the goal is blocked, sample rings starting at the requested radius × 1.34.
3. A ring has floor(2πr / clearance) + 1 points. The point at angle a sits at r·(0.5 + 0.25(1 + cos a)), so the ring is biased toward the approaching unit.
4. A point is accepted if it is inside the box, free in the layer map and the static map at the current frame, and not inside another group member's reserved destination. The reservation list is walked in list order.
5. r grows by **DestinationSearchRadiusIncrementSpace = 50** per ring, for 40 rings. After that the original point is used.

**Path output** (`0x597dc0`) [M]:
- Walk the parent chain and reverse it into 0x34-byte waypoints: position, facing (cos, sin), timestamp in frames (+0x30), speed, time.
- Arc ops are densified into turn-step increments. Turn-in-place ops give zero-length segments.
- **There is no line-of-sight smoothing**; the smoothness comes from the arcs and from Hermite playback.

### 2.5 Ship motion (locomotors in space)

**Locomotor assignment** (vanilla XML) [H]:
- Corvettes, frigates, capitals, supers and transports use **SIMPLE_SPACE_LOCOMOTOR**.
- Fighters and bombers use **FIGHTER_LOCOMOTOR**.
- Squadrons use **TEAM_LOCOMOTOR**.
- STARSHIP_LOCOMOTOR is unused by vanilla XML: legacy, low priority.

**SimpleSpace update** (vf6 `0x626420`) [H]:
- In the path-following state (0x13), it finds the waypoint pair that brackets the current frame (`0x625990`) and evaluates a **cubic Hermite** curve.
  - Tangents = waypoint direction × speed × segment duration.
  - The basis matrix lives at `0x140b2f1f0…0x22c`; its values were not read, so assume the standard Hermite basis [M].
- New XY = curve value, speed = |derivative|, yaw = derivative direction. **Z is unchanged.** Zero-length segments interpolate yaw linearly.
- Banking/roll comes from `0x624c10` [M]. When stopped (state 0x16), roll decays toward 0 by `Max_Rate_Of_Roll` per frame.
- "Moving" ⇔ speed ≥ SpaceIdleMovementSpeed × 1.1.
- Engine effects scale with 0.8·v/vmax + 0.2.

**Turn to face** (SimpleSpace vf `0x625620`) [H]:
- Builds a 2-waypoint path with duration = |Δyaw| / turn rate × slowdown.
- Slowdown by layer:

| layer | constant | value |
|---|---|---|
| Corvette | TurnInPlaceSlowdownCorvette | 2.0 |
| Frigate | TurnInPlaceSlowdownFrigate | 3.0 |
| Capital / SuperCapital | TurnInPlaceSlowdownCapital | 4.0 |
| other | — | 1.0 |

**Group re-evaluation** [H]: the space coordinator (vf5 `0x5d51e0`) re-evaluates a member when objectID % **MovementReevaluationFrameCount (10)** == frame % 10.

**Group start stagger** [M]: each successive unit of a group starts its path **SpacePathfindFrameDelayDelta = 2** frames later, in the group's list order (`0x5d4730`).

**Legacy StarshipLocomotor** (vf6 `0x6236b0`, integrate `0x6224b0`, fly `0x622b80`) [M] is reactive steering:
- velocity += forward × `Max_Thrust` × throttle. Throttle is 0.05 at start, 0.8 when not aligned, 1.0 within half a turn rate, 0.4 while banking.
- |v| is clamped to max speed, lateral velocity is damped toward forward (0.01, or 0.005 when aligned), and then v × `Mass` (+0x398) acts as a per-frame drag factor.
- Z steps toward `Layer_Z_Adjust` (+0xdfc) by at most the vertical step getter (+0x3a0).
- Arrival is within max(extent, 10) of (target XY, Layer_Z_Adjust).

**Fighters** (vf6 `0x5cb830`) [M]:
- States 0x1B Moving, 0x1C Idle, 0x1D Idle_Combat, 0x1E Directed_Combat, 0x1F Dead_Stop.
- Directed_Combat is chosen when the squadron has an attack target.
- Pitch beyond ±90° is folded by a 180° flip.
- Attack-run and squadron-offset steering (`0x5ce010`, `0x5cd8e0`, `0x5cc220`, `0x5cbea0`) are not traced (§8).

**Layer_Z_Adjust in space:** formation slots in space take z = the member's `Layer_Z_Adjust` (§4.3). This is how layers are separated vertically.

### 2.6 Hyperspace

**Arrival** (state 4; SimpleSpace `0x626c80`, Fighter `0x5cc970`) [M]:
- Start offset = synced random(1..4) + 5·floor(4f), clamped to 0..24.
- The arrival counter runs to **150** frames, then finishes (`0x624dc0`).
- Position = start + velocity × profile(frame), with the profile table at `0x140b27a10`.
- Hidden for frames < 25. Arrival signal at frame 35. Component event at frame 120. Becomes visible within 7000 units of the destination. 115 frames is the nominal arrival time.
- **Determinism risk:** f is computed from globals `0x140b27b7c…bc0` and mode +0x628…+0x630, which look camera-related (debug strings "CLD/CV/HEP/IHyperD/MinD/MaxD"). Verify before porting. If these are camera values they must not affect the simulation. [L]

**Departure** (state 5 / 0x28 "Fly Hyperspace Away") [M]:
- Velocity = forward × **1500** per frame toward a point 7500 ahead.
- Timer = 120 + synced random(0..30) frames, then the finish virtual (vf +0x298).
- Lua `Hyperspace_Away` (`0x57a9d0`, space-only, optional bool) calls locomotor vf +0x2a8 [H].
- `Cinematic_Hyperspace_In` (`0x574e50`) and `Resume_Hyperspace_In` (`0x543570`) were not traced.
- The readers of `Hyperspace_Speed` (+0x648) were not found.

**Reinforcement drop validity** (`0x4d6fd0`) [H]:
1. The point must be inside the playable box (mode +0x3a8 centre, +0x3b4 half-extents).
2. Sweep from (position − direction × **Space_Reinforcement_Collision_Check_Distance = 200**) to the position over [now, now + 115 frames], against the layer map and static map 8. The direction is the player's reinforcement facing.
3. Fighter-type objects (layer 0) test as layer 4 with their members' combined extents.
4. Any blocker outside layer 4 makes the drop invalid.

### 2.7 Space constants (vanilla)

| constant | value | | constant | value |
|---|---|---|---|---|
| SpacePathfindMaxExpansions | 3500 | | MaxRotationsSpace | 24 |
| SpacePathingTries | 6 | | XYExpansionDistanceSpace | 300 |
| SpacePathFailureMaxExpansionsCoefficient | 1.7 | | MatchFacingDeltaSpace | 10° |
| SpacePathFailureRotationExpansionIncrement | 0.5 | | OccupationRadiusCoefficientSpace | 1.2 |
| SpacePathFailureForwardExpansionIncrement | 0.5 | | DestinationSearchRadiusIncrementSpace | 50 |
| SpacePathFailureDistanceCutoffCoefficient | 0.25 | | ThreatExpansionDistance | 600 |
| CurrentPathCostCoefficientSpace | 0.66 | | OffMapCostPenalty | 20 |
| MinObstacleCostSpace | 15 | | WaitOperatorCostCoefficient | 0.8 |
| SpaceStaticObstacleAvoidanceBonusDistance | 0 | | WaitOperatorBaseFrameTime | 100 |
| SpacePathfindFrameDelayDelta | 2 | | WaitOperatorSpeedCoefficient | 0.2 |
| FinalFacing180Penalty | 1.5 | | SpecialAlignedOperatorBonus | 0.6 |
| SpaceObjectTrackingTreeCount / Interval | 45 / 90 | | Land…TreeCount / Interval | 25 / 20 |
| SpaceLocomotorFacingLookaheadAcc | 1.0 (reader not traced) | | SpaceIdleMovementSpeed | 0.0 |

---

## 3. Locomotors and per-tick integration

### 3.1 Classes and XML tokens [H]

**Factory:**
- The behaviour factory table is at `0x140a2ac90`: 32-byte entries (name, create function, predicate), about 117 entries.
- `0x4c3520` upper-cases a `Behavior`/`LandBehavior`/`SpaceBehavior` token, matches it exactly, creates the behaviour and stores the token's CRC at behaviour +0x38.
- At spawn, `0x3a0580` loops the type's behaviour list through this table.
- **The locomotor always occupies behaviour slot 1.**

All locomotors derive from `LocomotorCommonClass` (ctor `0x42b9f0`, 97-slot vftable `0x869ea0`). Their state lives in `LocomotorDataPackClass` (ctor `0x553f00`).

| XML token | class (vftable) | vanilla users |
|---|---|---|
| WALK_LOCOMOTOR | WalkLocomotorBehaviorClass (`0x8ad798`; debug name "LargeWalk…", one class) | almost every ground unit: infantry, tanks (incl. Hover class), AT-ST, AT-AT, speeders, Lancet |
| BIKE_LOCOMOTOR | Bike (`0x8b23c0`) | Scout_Trooper |
| LAND_TEAM_CONTAINER_LOCOMOTOR | LandTeamContainer (`0x8a3630`) | infantry squad/team containers |
| LAND_TEAM_INFANTRY_LOCOMOTOR | LandTeamInfantry (`0x8af8d0`) | some squad members |
| FLYING_LOCOMOTOR | Flying (`0x8b2738`) | snowspeeder, gunships |
| JETPACK_LOCOMOTOR | JetPack (`0x8b2e80`) | Boba_Fett, Dark_Trooper_PhaseII / III, only as `Secondary_Locomotor_Name` (§3.1.1) |
| LANDBOMBER_LOCOMOTOR | LandBomber (`0x8b3568`) | bombing runs |
| FIGHTER_LOCOMOTOR | Fighter (`0x8a6198`) | space fighters/bombers; birds on land |
| SIMPLE_SPACE_LOCOMOTOR | SimpleSpace (`0x8aeaf8`) | corvettes … capitals |
| TEAM_LOCOMOTOR | Team (`0x8aff88`) | space squadrons |
| FLEET_LOCOMOTOR | Fleet (`0x899c58`) | galactic fleets |
| STARSHIP_LOCOMOTOR / SIMPLE_LOCOMOTOR | Starship (`0x8ae250`) / LocomotorBehaviorClass (`0x8adda0`) | none in vanilla FoC [M] |

**Two locomotors** (`0x3a9910`) apply only when `Uses_Multiple_Locomotors` (+0x74) is set: the unit swaps between
`Primary_Locomotor_Name` (+0x2058) and `Secondary_Locomotor_Name` (+0x2078). Details, callers and the alternate
speed / turn rate: §3.1.1.

#### 3.1.1 Locomotor swap `0x3a9910(obj, coordinatorSystem, toPrimary)` [H unless tagged]
(S7 t11, 2026-11-01.) `coordinatorSystem` is the mode's `MovementCoordinatorSystemClass` (mode vf +0x228); every caller
passes it and skips the call when the mode has none.

**It is a set, not a toggle.** Steps:
1. Type +0x74 (`Uses_Multiple_Locomotors`) clear → return.
2. **"Already there" test:** L = the current locomotor (behaviour slot 1). If L vf96 (+0x300) == `toPrimary`, return.
   vf96 is a class trait, not a record of which name was built: it answers **0 only for JetPack** (`0x13b7d0`) and 1
   for every other locomotor class (`0x17dc70`, LocomotorCommon and all twelve others). So "on the secondary" means
   "the current locomotor is a JETPACK_LOCOMOTOR".
   - Consequence for mods [H]: with a non-JetPack secondary, a to-secondary call rebuilds the secondary every time
     (vf96 = 1 ≠ 0) and a to-primary call never swaps back (1 = 1). With a JetPack primary, the jetpack's own swap-back
     (below) rebuilds the primary every stopped frame. Vanilla never does either.
3. Name = `Primary_Locomotor_Name` when `toPrimary`, else `Secondary_Locomotor_Name`. The null test on the name is
   dead (the std::string buffer is never null), so an empty name continues.
4. **To the secondary only** (`toPrimary` = 0), the order hand-over, done **before** the locomotor changes:
   - B = a default move-parameter block (§6.1 layout): destination = the unit's own position (obj +0x78…+0x80),
     facings 1e18, type 1 (position), mask 0x3f at +0x4c, +0x54 = 1e18, +0x60 = 0x1000001, everything else 0;
   - C = the coordinator holding the unit (`0x426140`); if there is one, B := C's current order block (coordinator
     +0x40…+0xa4, all fields);
   - the unit leaves every coordinator (`0x426d50`, coordinator vf4 +0x20);
   - a new coordinator is made (`0x426010`, land or space kind by the system's mode), the unit is added (vf3 +0x18,
     `0x438b40` → `0x4f6b60`) and **`Coordinator::Move_To(B)` (`0x4f8200`) re-issues the order**.

   So the saved order **is re-issued**: the unit continues its current move (or holds its position when it had no
   coordinator) alone in a coordinator of its own. `Move_To` itself draws nothing: the unit's new formation, and its
   one FormationClass ctor draw, come at the next coordinator update (§4.7.4, cu8j) [H]. This happens even when the
   name is empty.
5. Build the new locomotor with the behaviour factory (`0x4c3520`). It refuses names of length 0 or ≥ 256 and names
   that match no table row, and returns null. **Null → return**: no other change. An empty
   `Secondary_Locomotor_Name` therefore never swaps, but step 4 has already split the unit off with its order.
6. With the new locomotor N:
   - remove L (`0x3a54c0`: out of the behaviour array, slot table fixed up, reference released);
   - `Add_Behavior(N, loading = 0)` (`0x38c570`, OBJECT_RUNTIME §4.2): attach, then Initialise:
     - JetPack Initialise (`0x640ef0`): conform/commit at the current position (vf95), **pack state := 0 (None)**,
       pack +0x28d := 1;
     - Walk Initialise (`0x61dea0`): conform/commit at the current position (vf95, skipped when obj +0x3a0 bit 7 is
       set), **pack state := 0x16 (Walk Stopped)**, pack +0x268…+0x270 := the object's orientation (obj +0x84…+0x8c).

     The locomotor data pack (obj +0xa8: path stack, speed, formation pointer +0x1f0) belongs to the object and
     **survives the swap**; only the state is reset by the new locomotor.
   - engine sounds off: moving loop `0x3a7e30(obj, 0)`, idle loop `0x3a7cf0(obj, 0)` (STATION_HANGARS_SOUND §3.3).
     **Audio only** [H] (2026-11-02): the two functions only start / stop the SFX and set the flags obj +0x3B6 /
     +0x3B5, which are read only by audio code, save / load (`0x33a580` / `0x33c160`), the state copy `0x472560`
     and the TSW audio `0x42a7b0`. No sim result depends on them;
   - **weapons:** behaviours TARGETING (9), WEAPON (10) and SPECIAL_WEAPON (0x2F), each if present:
     - to the secondary: Disable (vf24 +0xc0: suspend count +0x3c + 1). TARGETING (vftable `0x8b0d88`) and
       SPECIAL_WEAPON (`0x8756e0`) use the base `0x4c30b0`. The WEAPON override `0x54ac30` [H] calls the base, then
       clears the unit's **own muzzle flash** (not hardpoints): when the weapon data pack (obj +0xf0) belongs to this
       behaviour (pack +0x80 == it), for each fire bone i < pack +0xd0 whose flash countdown (int array pack +0xc8)
       is > 0: countdown := 0, hide the `<bone>_FLASH` mesh (pack +0xa8[i]) and, when i < pack +0xe8, the
       `<bone>_LIGHT` mesh (pack +0xd8[i]). These are the meshes a shot shows for 4 frames. **Visual only**: the
       target (pack +8), the cooldown (pack +0x24), the pulse / burst counters and the fire-bone cycle index are
       not touched;
     - to the primary: Enable (vf23 +0xb8, `0x4c30a0`: count − 1, floor 0), then object signal **0x29** (`0x220ed0`
       on obj +0x38) = **"unit ability finished"** (below).

     A suspended behaviour is not serviced (§3.3), so **a unit on its jetpack neither targets nor fires**.

     **The weapon cooldown keeps elapsing while suspended** [H]. The WeaponBehavior service `0x54d090` does not count
     its cooldown down by one per service: it keeps the last serviced frame at pack +0x2c (rewritten every service) and,
     when the cooldown (pack +0x24, int frames) is non-zero and pack byte +0x3d is 0, takes delta = frame − pack +0x2c;
     delta > cooldown (unsigned compare) → cooldown := 0, else cooldown −= delta, and a non-zero result ends the
     service (no shot). A suspended behaviour is not serviced, so pack +0x2c stays at the last pre-flight frame and the
     first service after the swap back subtracts the whole flight time. A shot whose recharge ran out in the air is
     ready on the first frame back on the ground.

     **Signal 0x29 listeners** [H] (raised by every ability end, ABILITIES_HARDPOINTS §2.6, and here). Two kinds:
     - **Task forces** (TaskForceClass Receive_Signal `0x5fe5d0`, case 0x29). Add_Force `0x5f9a90` registers the task
       force on each member; it unregisters in vf1. Skipped when the unit has obj +0x3a0 bit 0x02 set (dying /
       being removed). Otherwise the unit is wrapped as a Lua value (`0x5751d0` / `0x256d40`) and the plan event
       **0x13 = `<TaskForce>_Unit_Ability_Finished(tf, unit)`** is dispatched (`0x6006b0`; AI_GALACTIC_2 §5.2 event
       19). PGEVENTS' `Default_Unit_Ability_Finished` is empty, so only plans that define the handler react.
     - **Lua blocking status of `Activate_Ability`** (AbilityBlockStatus, vftable `0x89a558`; its listener sub-object
       at +0x30 has vf1 `0x6ae4f0`). Made by `GameObjectWrapper.Activate_Ability` `0x570580` (pending count +0x40 = 1)
       and by TaskForce `Activate_Ability` `0x5f9890` (one per unit that activated). Each registers on its unit for
       signals **0x29, 0xb and 0x10** (`0x6ae570`); the first of them decrements the count and unregisters all three.
       `IsFinished` `0x57bab0` = (count == 0). TaskForce `Activate_Ability` also queues `Unit_Ability_Finished` at
       once for each unit whose activation failed. So `BlockOnCommand(unit.Activate_Ability("JET_PACK", target),
       timeout)` returns when the jetpack swaps back (or on 0xb / 0x10, meanings not traced here). FoC user:
       AI/LANDMODE/BOBAFETTFLAMEINFANTRY.LUA (lines 153 and 168).

     This is **sim-relevant** (AI Lua flow), not presentation.
7. The current **order is left alone on the to-primary path**: no coordinator change, the unit stays in the
   coordinator it was in; the walk locomotor starts stopped (0x16).

**Callers** (five functions, six call sites) [H]:

| caller | direction | when |
|---|---|---|
| JET_PACK ability `0x3a38d0` (REMAINING_GAPS §5.3) | to secondary | after the gates pass, when the unit has a locomotor (obj +0x333 ≠ −1) and type +0x74 = 1; then the ability's own group move (`0x2a7b80`, flags 0x04) moves the unit out of the step-4 coordinator into a new one |
| MoveToPosition execute `0x3ad100` (§6.1, §6.2) | to secondary | event +0x58 (the 1-bit field) = 1: every unit of the player's selection, in list order, with a locomotor and type +0x74; then the group move (`0x2a7b80`) or, with flag 0x08, the waypoint queue (`0x2a8240`), which appends to the step-4 coordinator so the re-issued order runs first. **No FoC sender sets the bit:** the event is made only by `0x3b090` and `0x3b360` (the UI move, via `0x3ad330(event, point, flags, 0)`), and the ctor `0x3acee0` clears it. The path is dead in vanilla |
| hero clash `MoveActionClass` vf5 `0x5e9ce0` (Begin) | to secondary | `Move_Use_Jetpack` (action +0xe4) = Yes, before the action's own move (type 1 `0x5e9ed0`; type 2 `0x5e9f30` + `0x5ea910`). Begin also saves "locomotor suspended" (vf66 +0x210, pack +0x260) at +0xe8 and resumes it (vf65) when the move starts |
| `MoveActionClass` vf5, failure path | to primary | the move could not start: swap back, and re-suspend the locomotor (vf64 +0x200) if it was suspended |
| `MoveActionClass` vf9 `0x5ea820` (End) | to primary | always when `Move_Use_Jetpack`, after releasing the movement lock (`0x3a8e50(obj, 0)`), **whether or not the unit has landed**; then re-suspend if it was suspended |
| JetPack vf6 `0x641910`, state 0x16 | to primary | below |

`MoveActionClass` (vftable `0x8a8460`) is an action of `ActionDrivenHeroClashClass` (`0x8bd978`; starts actions in
`0x6aae80`, ends them in vf2 `0x6ab6f0` / vf5 `0x6ab4d0`). The only FoC `Move_Use_Jetpack` user is the
`ObiWan_BobaFett` clash in HEROCLASH.XML (three `Move_Past_Target` actions). Hero clashes have no port code (TACTICAL_COMBAT
item 24).

**Swap back in the JetPack locomotor** (vf6 `0x641910`) [H]:
- vf6 runs only in land mode (mode vf +0xe0 = 1) and while the locomotor is not suspended (vf66 +0x210 false); the
  state must be in {0, 0xF…0x14, 0x16} (mask 0x5f8001), else it returns an error.
- **State 0x16 (every service):** pack +0x28d := 0, speed (+0xec) := 0, play the idle animation (`0x395920`), then,
  because JetPack vf96 = 0, call `0x3a9910(obj, system, 1)`. So the jetpack swaps back on the **first service in
  state 0x16**, with no other condition (no pending-order test).
- The handlers return "continue" (0) when they change state, and the switch re-dispatches in the same service. So the
  swap happens **in the same frame** the state becomes 0x16 from:
  - Moving 0x13 (`0x6420e0`) with no current path;
  - End Move 0x14 (`0x641df0`) when FlyLandDrop (0x34) has finished (after it snaps to the terrain).
- It happens **one service later** from:
  - None 0 (vf6 case 0) with no current path or path +0x6d clear: state := 0x16 and return;
  - Moving 0x13 when the frame's follower step left position and orientation unchanged: state := 0x16, and the path
    is not done (done → 0x14 overrides). This can swap a unit back while it is still in the air [H for the flow; M
    whether it occurs in play].
- The timed-path follower vf +0x1d8 (`0x641010`, Hermite playback of the 0x34-byte waypoints) never swaps.
- Pack state 0 after the to-secondary Initialise means **the jetpack needs a current path at its first service**,
  or it reverts on the next one. In the JET_PACK ability the group move runs right after the swap; whether
  `Formation::Set_Move` (`0x423c10`) has given the pack a path by the jetpack's first service was not traced [M].

**Alternate_Max_Speed (+0x380) / Alternate_Max_Rate_Of_Turn (+0x390)** [H]: read **only by the JetPack locomotor**,
and only while it is airborne-kind:
- JetPack vf74 (+0x250, `0x640c00`, max speed) and vf75 (+0x258, `0x640bb0`, turn rate) ask the kind (vf71 +0x238,
  `0x640f90`): kind 2 when there is no pack or the pack state is 0, 0xF, 0x10, 0x11, 0x12 or 0x13; kind 1 otherwise
  (0x14 End Move / landing, 0x16 Stopped).
  - kind 1: the normal values: max speed `0x370f00` (`Max_Speed`, capped by the team minimum, then the §3.4 chain
    `0x376990`); turn rate `0x372560` (`Max_Rate_Of_Turn`);
  - kind 2: `0x371060` = the same chain `0x376990` applied to `Alternate_Max_Speed` (no team-minimum cap); turn rate
    `0x370fd0` = `Alternate_Max_Rate_Of_Turn` × the mode multiplier × the global factor (the `0x372560` pattern).
- Walk and LocomotorCommon vf74 / vf75 are the normal getters (`0x42c5e0` / `0x42c5c0`), so a walking unit never
  uses the alternate values.
- The only callers of `0x371060` and `0x370fd0` are those two JetPack getters. **Where the getters are read:**
  - the planner request record (`0x59d120`, §2.1 "Request record" +0xf0 max speed / +0xf4 turn rate), filled when
    the path is requested. After the swap the pack state is 0 and the re-land move (0x640880) is issued in 0x13,
    both kind 2, so **the timed flight path is planned with the alternate speed and turn rate**, and the flight (0x13
    plays that path back) moves at the alternate speed [M: the exact frame of the request was not traced; a request
    made after the landing starts would get the normal values];
  - the formation speed (`0x41b2a0`, the slowest member's vf74; P1_DOUBLE_CLICK_MOVE): the alternate speed while the
    member is airborne-kind.
- **Not** read through them: the jetpack's take-off turn (0x10, `0x642740`) uses pack +0xe8 or, when that is ≤ 0,
  `Max_Rate_Of_Turn` (`0x372560`) directly (below); the landing (0x14) and the stopped state set no speed from them.
- **Acceleration / deceleration** [H] (2026-11-02): JetPack vf91 / vf92 are the common `0x42c400` / `0x42c4c0`.
  Acceleration = `OverrideAcceleration` (type +0x3c0) × (effective max speed / `Max_Speed`), where the effective
  max speed is `Max_Speed` capped by the team minimum and passed through the `0x376990` chain (via `0x372fe0`), i.e.
  the override scaled by the same modifier ratio as the **normal** speed. When that is ≤ 0 the result is `0x370f00`
  = the normal effective max speed. Deceleration is the same with +0x3c4. The alternate speed is **never** used
  for either, airborne or not. The planner request reads them in `0x59d4a0` (request +0x1ec := vf91,
  +0x1f0 := vf92; defaults: the max speed +0xf0); with `Walk_Transition` (type +0x35) the animation-based branch
  applies instead. Vanilla jetpack users all set an override (0.1 / 0.05), so the ≤ 0 fallback is mod-only.

**Take-off turn: pack +0xe8** [H] (2026-11-02). Pack +0xe8 is the locomotor data pack's float "turn rate of the
current turn-animation band". Readers: `0x640af0` (current turn rate: +0xe8 when > 0, else `0x372560`(type) =
`Max_Rate_Of_Turn` × mode multiplier × global factor = R), the JetPack turn `0x642740` and Walk. Writers: Walk
`0x6204f0`, JetPack `0x642740`; zeroed together with +0xec by the pack ctor `0x553f00` and LandTeamInfantry vf6
`0x62ca10`; and **JetPack state 0xF sets it to 0 right before entering 0x10** (vf6 `0x641910` case 0xF). So the first
take-off turn frame always turns at R, the normal rate (never the alternate one). `0x642740`, per service:
1. r = +0xe8 when > 0, else R.
2. e = the wrapped yaw error toward the path aim (`0x640cc0`); turn by min(|e|, r) toward it; r_left = r − |e| when
   |e| ≤ r, else 0. Set the orientation (roll 0, pitch 0) and commit (vf95).
3. path = the current path (`0x5569b0`); c = the animation is in its last 2 frames (`0x39a180`); anim = the current
   animation (`0x3956a0`).
4. If not (r_left ≤ 0 or c or path) → state 0x11 (End Turn).
5. Else if anim ∈ {3, 4, 0x15, 0x16, 0x17, 0x18} and not c → return (keep turning, the band rate stays).
6. Else if (no path or path +0x6d) and r_left ≤ 0, pick a turn animation, then return:
   - with `Has_Pre_Turn_Anim` (type +0x36): play idle, re-measure e after this frame's turn and band on |e| (the sign
     of e picks left / right):
     - |e| ≥ 45 → anim 3 / 4, +0xe8 := R;
     - 22.5 ≤ |e| < 45 → anim 0x15 / 0x17, +0xe8 := R × 0.5;
     - R ≤ |e| < 22.5 → anim 0x16 / 0x18, +0xe8 := R × 0.25 × |e| / 11.25;
     - |e| < R → no animation, +0xe8 := R, End Turn.

     The animation plays at the runtime constant speed `DAT_140b39958` with the blend time `0x3736a0`(type);
   - without +0x36: only when anim 3 exists (`0x38f9e0(obj, 3)`) and no turn animation is playing (`0x61df80`):
     anim 3 / 4 by the sign of the frame-start error at speed min(|e0| / current rate × 0.03333, GC `0x140b16d28`).
     +0xe8 is not written (stays 0, so R).
7. Else → End Turn.

So the band value is the **yaw rate of the following frames**, not an animation rate, and the state stays in 0x10
until the turn animation reaches its last 2 frames even once the unit is aligned. The vanilla jetpack users
(Boba_Fett, Dark_Trooper_PhaseII / III) do not set `Has_Pre_Turn_Anim`, so they turn at R throughout (vanilla
`Max_Rate_Of_Turn` 20; Boba's `Alternate_Max_Rate_Of_Turn` 6 is not used here). The "last 2 frames" test reads the
animation clock; a port uses the n × rate × speed rule of ABILITIES_HARDPOINTS §4.13 instead.
- Defaults (type ctor `0x367250`): `Alternate_Max_Speed` **1.0**, `Alternate_Max_Rate_Of_Turn` **2.0**. FoC data:
  Boba_Fett 2.5 / 6.0; Dark_Trooper_PhaseII 2.5 / unset (2.0); Dark_Trooper_PhaseIII 1.8 / unset (2.0). Bossk, IG-88,
  Silri, Tyber_Zann and Urai_Fen also set them (2.5–2.7 / 6.0) but never fly; Dark_Trooper_PhaseI sets only the turn.

**Spawn locomotor** [H]: only `0x3a9910` reads +0x2058 / +0x2078 (besides the type copy `0x36c000`). At spawn the unit
gets the locomotor of its behaviour list (§3.1 factory); `Primary_Locomotor_Name` is used only by the swap back. Every
FoC user lists WALK_LOCOMOTOR in its LandBehavior and names WALK_LOCOMOTOR as primary, so the two agree in vanilla.

**FoC users** (extracted XML, 2026-11-01): `Uses_Multiple_Locomotors` = Yes on Boba_Fett, Dark_Trooper_PhaseII,
Dark_Trooper_PhaseIII (Primary WALK, Secondary JETPACK; each has the JET_PACK ability) and on Bossk, IG-88, Silri,
Silri_No_Abilities, Tyber_Zann, Tyber_Zann_Prisoner, Urai_Fen (no `Secondary_Locomotor_Name`; Bossk and IG-88 name a
WALK primary). No FoC type lists JETPACK_LOCOMOTOR in a behaviour list.

**Open** [M]: whether a one-unit `Move_To` in step 4 draws synced RNG; whether the JET_PACK move's path exists at the
jetpack's first service; the meanings of signals 0xb / 0x10 that also release an `Activate_Ability` block.
(Answered 2026-11-02: signal 0x29, the WEAPON Disable side effects, pack +0xe8, the acceleration, the engine loops.)

### 3.2 Locomotor state (`LocomotorStateType`, enum `0x5e2e30`, data pack +0x48) [H]

| value | state |
|---|---|
| 0 | None |
| 1 | Fly Towards Target |
| 2 / 3 | Fly Turning L / R |
| 4 / 5 | Fly Exit / Start Hyperspace |
| 6 / 7 | Fly stopping / stopped |
| 8 | Move follow leader |
| 0xC–0xE | Hover Moving / Stopping / Stopped |
| 0xF–0x11 | Walk Start Turn / Turning / End Turn |
| 0x12–0x14 | Walk Start Move / Moving / End Move |
| 0x15 | Pre Stopped |
| 0x16 | Walk Stopped |
| 0x17 | Walk Stop For Turn |
| 0x18–0x1A | Deploying / Deployed / Undeploying |
| 0x1B–0x1F | Fighter states (§2.5) |
| 0x20–0x25 | Bike Turning In Place / Start Move / Moving / End Move / Pre Stopped / Stopped |
| 0x26 / 0x27 | Land_Fly Circling / No Path |
| 0x28 | Fly Hyperspace Away |
| 0x29 / 0x2A | Walk Recovery Turning / Moving |

Values 9–0xB are unused. SimpleSpace reuses 0x13 (following a path) and 0x16 (stopped).

**Data-pack fields:**

| offset | content |
|---|---|
| +0x138…+0x140 | a stack of 0x70-byte path records; the current path is the last one |
| path +0x18 | waypoints |
| path +0x5c | skip-passability flag |
| path +0x60 | current waypoint index |
| +0xec | current speed |
| +0x12c / +0x130 | formation jiggle offset |
| +0x158 | formation/override speed |
| +0x1f0 | owning formation |
| +0x274 / +0x275 | collision-check / path-approximation countdowns (§5.2) |
| +0x2a8 | hover state: 0 grounded, 1 raised (at `Hover_Height`), 2 rising, 3 lowering (§3.5 item 7, §5.3; the earlier "formation-member flag" was wrong, corrected 2026-11-10) |

**Deploy states (S8 t11, 2026-11-02)** [H unless tagged]. These are used by the weapon gates (TACTICAL_COMBAT §3.1.1
"Gate details").

*Seen in game (s78f t01, 2026-11-05, `tools/probe_mod/s7s8_probes/out/20261105-055142`):*
- An `SPMAT_Walker` given an attack order went 0x16 → 0x18 three frames later, playing clip 25 (Deploy) at rate 2.0.
- It reached 0x19 after 45 frames.
- At the end of the scenario it showed 0x1A (Undeploying) before it was despawned.
- The Lua DEPLOY block timings are in AI_TACTICAL §7.3.3 "In-game probe of the DEPLOY block".
- Only Walk implements the deploy vfuncs:
  - vf +0x118 = `Deploy(bool)` (`0x61d300`);
  - vf +0x120 = state == 0x19 (`0x61df40`);
  - vf +0x128 = state == 0x1A (`0x61dfd0`);
  - vf +0x130 = state == 0x18 (`0x61df60`).

  Every other locomotor answers false (`0x13b7d0`). The JetPack's +0x118 is `0x1fa90`.
- **Deploy(true).** It acts when the state is not 0x18 / 0x19, the pending flag (pack +0x2b3) is clear and
  obj +0x3a1 & 0x40 is clear:
  - stop (vf +0x140(0)), state := 0x18;
  - play animation 0x19 (Deploy) with blend 1/30 at `Deployment_Anim_Rate` (type +0x1668, default 1.0, ctor
    `0x3684a8`);
  - start frame = clipLen − curFrame − 1 (min 0) when reversing out of 0x1A, else 0;
  - reversing out of 0x1A while the TURRET is not at rest (`0x386170`) instead goes straight to 0x19, resets the
    turret and raises signal 0x29.
- **Deploy(false).** Unless it is blocked by the behaviour in slot +0x35b (id 0x29; `0x42e3b0` / `0x42e3d0`), a state
  of 0x18 / 0x19 or a set pending flag → state 0x1A.
- **Per frame** (vf6 `0x61e930`):
  - 0x18: when the current animation is 0x19 and has ended (`0x39a180`: frame ≥ len − 2 − step, or animation state
    2) → state 0x19, signal 0x29, `0x39fec0`. Otherwise the deploy animation is (re)started only once the TURRET
    (slot +0x33d) is at rest; until then `0x6319d0(turret, 0)` recentres it.
  - 0x1A: clear pending +0x2b3; if the animation is not 0x1A, play it at `Deployment_Anim_Rate` (start =
    len − cur − 1 − rate when reversing out of 0x19), plus `0x3a0580`, with the same turret wait. When it has
    ended → state 0 and signal 0x29.
  - 0x16 with pending set → 0x19 directly, with the animation on its last frame. This is the walk-while-deployed
    case (`Max_Dist_Walk_While_Deployed` > 0). *Corrected s78n t20: "no vanilla FoC unit sets that field" was
    wrong; `Destroyer_Droid` sets 150.0 and `Destroyer_Droid_Attack` inherits it (they are the only ones).* The
    whole walking-while-deployed rule set: §3.2.1 (s78n t26).
- **Window length** ≈ the turret recentre wait + ceil(max(count − 2, 0) / (clip rate × `Deployment_Anim_Rate`)) sim
  frames, following the ABILITIES_HARDPOINTS §4.13 convention [M on the exact end test].
- **FoC `Deploys = Yes`** (all WALK_LOCOMOTOR): AT_AT, Veers AT_AT, AT_AA (rate 2.0), SPMAT (2.0), MPTL (1.5),
  Abstract_Mobile_Defense_Unit (2.0), Dark_Trooper_PhaseIII (2.0; walk + jetpack), HAV_Juggernaut,
  Gallofree_HTT_Transport, Destroyer_Droid (3.0), MAL_Rocket_Vehicle, F9TZ_Cloaking_Transport (*s78n t26 RE 3:* the
  last two were missing; `Auto_Deploys` per type: §3.2.1).
- **A locomotor swap undeploys** [H]. The swap (§3.1.1) replaces Walk with a new locomotor. JetPack answers
  "not deployed", and the new Walk built at the swap back starts in 0x16. So a deployed `Dark_Trooper_PhaseIII` that
  uses JET_PACK counts as undeployed in the air and after landing. Its `HP_DarkTrooper_P3_*` missiles
  (`Fire_When_Deployed` Yes, `_Undeployed` No) stay refused until it deploys again.
  - Hardpoints are **not** suspended by the swap: only TARGETING, WEAPON and SPECIAL_WEAPON are. The hardpoints
    are held by the deploy flags. With TARGETING suspended, they also get no new target pushed.
  - The pending flag +0x2b3 lives in the pack, which survives the swap. *Corrected s78n t26: "vanilla never sets
    it" was wrong; the Destroyer Droid sets it (§3.2.1), and it has a single locomotor, so the swap case does not
    arise in FoC.*
  - [M] Whether the model's deploy pose is reset visually.
- **Who listens to these 0x29 raises (2026-11-02).** The Lua `Activate_Ability("DEPLOY", bool)` block
  (`AbilityBlockStatus`, e.g. the AI's `BuildMDUItem`) finishes on them, so it returns when the deploy / undeploy
  animation ends; a deploy refused before Walk vf +0x118 runs (gate 0x3a1ce0, incl. locomotor vf +0x2b0) raises
  nothing. See AI_TACTICAL §7.3.3 part 3.

**Turret recentre around deploy (RE, 2026-11-02)** [H unless tagged]. The turret state is in the targeting data pack
TD = obj +0xf0:
- current angles +0x48 / +0x4c / +0x50 and target angles +0x54 / +0x58 / +0x5c (the three components of
  `Turret_Rest_Angle`: x, elevation, yaw);
- +0x6c is the time the turret went idle, in seconds (frame / FPS);
- +0x70 is the "idle hold started" byte;
- +0xf6 is the "aiming at a target" byte.

Rest angle R of the type: `Turret_Rest_Angle` (+0x5b4), or `Turret_Deployed_Rest_Angle` (+0x5c0) when the type
`Deploys` (+0xd1) and the unit is deployed / deploy-pending (0x39a390) or **Deploying** (0x39a400: Walk state 0x18).
- **Motion** (0x631210, run by the TURRET service 0x632270, behaviour interval 1 = every frame):
  - Per frame, the elevation (+0x4c) moves linearly toward +0x58 by at most `Turret_Rotate_Speed` (+0x5a8,
    **degrees per frame**, no frame-time scaling).
  - The yaw (+0x50) moves by the shortest wrapped difference to +0x5c, by at most the same step. It is then wrapped
    and clamped to ±`Turret_Rotate_Extent_Degrees` (+0x5a0; deployed: `Deployed_Turret_Rotate_Extent_Degrees`
    +0x5d0).
  - Component +0x48 never moves.
  - The bone gets yaw (and elevation unless `Turret_XY_Only`).
- **Idle return:** while not aiming (+0xf6 = 0) and current ≠ R:
  - on the first such frame (+0x70 = 0) the turret holds: target := current, +0x6c := now;
  - when now − +0x6c > `Turret_Rotate_To_Rest_Delay_Sec` (+0x5cc, constructor default **2.0 s**, no FoC unit sets
    it), the service sets target := R and +0x6c := 0, and the turret turns to R.
  - At R, +0x70 is cleared.
- **0x6319d0(turret, 0) = forced recentre:** +0xf6 := 0, target := R (by the rule above), +0x6c := 0. It skips the
  2-s delay. If +0x70 was 0, the next turret service still freezes the target for that one frame; the Walk code
  calls 0x6319d0 again every frame [M: one lost frame, depends on behaviour order].
- **"At rest" (0x386170)** = every component of current (+0x48..+0x50) equals **`Turret_Rest_Angle`** within 0.0001,
  with negative values first wrapped by +360. The Walk tests always compare against the **undeployed** rest angle,
  even in state 0x18.
- **Where it gates:**
  - `Deploy(true)` from state 0/0x16 starts the deploy animation **at once**, with no turret wait.
  - The turret test applies:
    - when reversing out of 0x1A: not at rest → straight to 0x19 + signal 0x29;
    - in state 0x18 only when the current animation is not the deploy clip (it was overridden): wait at rest, else
      0x6319d0;
    - in state 0x1A before the undeploy clip starts: every undeploy waits until the turret is back at
      `Turret_Rest_Angle`, then plays the clip.
  - Wait ≈ ceil(max(|Δyaw| shortest, |Δelev|) / `Turret_Rotate_Speed`) frames, e.g. AT-AT (3°/frame, ±55°) ≤ 19
    frames; AT-AA 14°/frame.
- **Units (FoC, `Deploys` + TURRET, all WALK_LOCOMOTOR):**

  | unit | `Turret_Rotate_Speed` (°/frame) | `Turret_Rotate_Extent_Degrees` |
  |---|---|---|
  | AT_AT_Walker | 3 | 55 |
  | Veers_AT_AT_Walker | 2 | 55 |
  | AT_AA_Walker | 14 | 360 |
  | SPMAT_Walker | 3 | 360 |
  | MPTL | 4 | 360 |
  | HAV_Juggernaut | 5 | 200 |
  | Gallofree_HTT_Transport | 5 | 360 |
  | MAL_Rocket_Vehicle | 8 | 30 (deployed 360) |

  MAL_Rocket_Vehicle also `Deploys`, at `Deployment_Anim_Rate` 0.5; it is missing from the list above. Only SPMAT
  sets rest angles: rest (0,0,0), deployed rest (0,−45,0).
  - [M] An SPMAT whose deploy clip is interrupted while in 0x18 would recentre toward the deployed rest (elevation
    −45) but be tested against (0,0,0), so it would wait until undeployed. Not observed.

#### 3.2.1 Walking while deployed (`Max_Dist_Walk_While_Deployed`, s78n t26 RE, 2026-11-10) [H unless tagged]
Read from `0x3ac100`, the formation plan `0x41c1a0` (gate and member loop), Walk vf +0x148 `0x61e570`, `0x42ced0`,
vf6 `0x61e930` (states 0x16, 0x19, 0x1A), the speed controller `0x61ccd0`, the move step `0x620820`, state 0x13
`0x61f840`, `0x61e380`, `0x42c890`, `0x39a320` / `0x39a370` / `0x39a390` / `0x39a400`, `0x3986b0`, `0x3a9e30`, the
formation speed `0x41b2a0`, the push `0x6859a0`, the StoppedVsStopped chooser `0x687010`, the shield absorb `0x549ac0`.

**Fields and data.** Type: M = `Max_Dist_Walk_While_Deployed` (+0x23b0), `Deployed_Walk_Speed` (+0x23b8),
`Deployed_Walk_Anim_Speed` (+0x23b4, default 1.0), `Auto_Deploys` (+0xd0), `Deploys` (+0xd1). The locomotor pack
(obj +0xa8) byte **+0x2b3 = "walking deployed"** (the "pending flag" of §3.2). Only `Destroyer_Droid` sets M (150.0;
`Destroyer_Droid_Attack` is its variant): `Deployed_Walk_Speed` 2.5, `Deployed_Walk_Anim_Speed` 2, `Max_Speed` 3.6,
`Max_Rate_Of_Turn` = `Stopped_Rate_Of_Turn` 15.5, `OverrideAcceleration` 0.5, `Deploys` + `Auto_Deploys`,
`Deployment_Anim_Rate` 3.0, abilities ROCKET_ATTACK only (**no DEPLOY**), `Shielded_Only_When_Deployed` Yes (160
points), `Fire_Weapon_When_Deployed` Yes / `_Undeployed` No, no TURRET behaviour.

**Who is an auto-deployer (s78n t26 RE 3, 2026-11-10; corrects "the only other `Auto_Deploys` type is the MAL")
[H].** `Auto_Deploys` (tag type 0x0 bool, type +0xd0) **defaults to true**: the type constructor `0x367250` calls
the bool-block initialiser `0x367100` on type +0x10, whose dword write at block +0xbf = 0x100 sets +0xd0 = 1 and
`Deploys` +0xd1 = 0; nothing later in `0x367250` rewrites them, and the XML only overrides a tag that is present
(OBJECT_TYPES §4.5 bool converter, §4.6 defaults). So every `Deploys` type is an auto-deployer unless it writes `Auto_Deploys` No, and only
three FoC types do: `AT_AT_Walker`, `Veers_AT_AT_Walker`, `AT_AA_Walker`. Every vanilla FoC type with effective
`Deploys` Yes (Variant_Of_Existing_Type inherits the base's values; a variant's own `Unit_Abilities_Data` replaces
the base list [M: read from the MAL_Captured comment, not from the variant copy code]):

| type (+ variants) | `Auto_Deploys` | DEPLOY ability | M |
|---|---|---|---|
| `AT_AT_Walker` (+ `_REB09`, `_Captured`, `_Cable_Attack_Death_Clone`, `_Death_Clone_00..02`, `CIN_AT-AT_Vehicles`, `FIN_At-AT`, `FIN_At-AT_Death`) | **No** (own) | no (DEPLOY_TROOPERS; `_Captured` EJECT_VEHICLE_THIEF) | 0 |
| `Veers_AT_AT_Walker` (+ `_Deployed_Death_Clone`, `_Death_Clone`) | **No** (own) | no | 0 |
| `AT_AA_Walker` (+ `_Captured`) | **No** (own) | no (MISSILE_SHIELD) | 0 |
| `SPMAT_Walker` (+ `_Captured`, `_Death_Clone`, `_Telekinesis_Death_Clone`, `FIN_SPMAT`) | Yes (default) | yes | 0 |
| `MPTL` (+ `_Captured`, `_Death_Clone`, `_Telekinesis_Death_Clone`, `CIN_MPTL_Intro`, `FIN_MPTL`) | Yes (default) | yes | 0 |
| `Abstract_Mobile_Defense_Unit` (+ Underworld, Underworld_Prologue, Empire, Rebel MDU) | Yes (default) | yes | 0 |
| `Dark_Trooper_PhaseIII` | Yes (default) | yes (+ JET_PACK) | 0 |
| `MAL_Rocket_Vehicle` | Yes (own) | yes (+ SWAP_WEAPONS) | 0 |
| `MAL_Captured` | Yes (inherited) | **no** (SWAP_WEAPONS, EJECT_VEHICLE_THIEF) | 0 |
| `HAV_Juggernaut` (+ `_Captured`) | Yes (default) | no | 0 |
| `Gallofree_HTT_Transport` | Yes (default) | no | 0 |
| `F9TZ_Cloaking_Transport` (+ `_Captured`) | Yes (default) | no | 0 |
| `Destroyer_Droid` (+ `Destroyer_Droid_Attack`) | Yes (own) | no (ROCKET_ATTACK) | 150 |

So the manual deployers (`0x39a320`(x, 1) false) are **only the AT-AT family, Veers' AT-AT and the AT-AA**. Every
other row has M = 0 and is an auto-deployer, so a move order undeploys it (item 2 member loop → item 1 →
`Deploy(false)`), the HTT included. The rules that do not read `Auto_Deploys`: the 0x16 stop rule and `Take_Damage`
(item 3) need `Deploys`, M > 0 and (stop rule only) no DEPLOY ability, so in FoC they fire only for the destroyer
droids, whatever `Auto_Deploys` says; the cap test `0x3ac100` itself needs only `Deploys` (its callers choose).

**Helpers.** Deployed D(x) = `0x39a390`: `Deploys`, a locomotor, and (+0x2b3 set or Walk state 0x19). Deploying =
`0x39a400` (state 0x18). Auto-deployer = `0x39a320`(x, 1): type +0xd0 AND +0xd1 (`Auto_Deploys` and `Deploys`).

1. **The cap test `0x3ac100`(x, d)** (x `Deploys` with a locomotor slot): when the pack exists, d ≥ 0, x is deployed
   (D) or deploying, M > 0 and **d < M** → +0x2b3 := 1 and nothing else (no state change, no clip). Every other case
   → Walk `Deploy(false)` (vf +0x118(0), §3.2): 0x18 / 0x19 / flag set → 0x1A; a rolling unit is unchanged.
   Callers: the formation plan (item 2) with d = the order's path length; with d = 0 (so a deployed droid is set
   walking deployed instead of being undeployed): `0x3a1ce0` (DEPLOY turned off), `0x38a350` case 0x20
   (MISSILE_SHIELD), `0x591a90` (DEPLOY_TROOPERS state 8), `0x636090` / `0x6360b0`.
   *s78n t26 RE 2 [H]:* `0x636090` / `0x6360b0` are the vftable slots +0xF0 / +0xF8 of
   GarrisonVehicleBehaviorClass (vftable `0x8b1360`) and GarrisonHoverTransportBehaviorClass (`0x8b14d0`). +0xF0 is
   called after a unit has entered the host (`0x4446b0`): when the host pack +0x210 is null or its reserved count
   +0x34 is 0 → `0x3ac100`(host, 0). +0xF8 is called from the enter cancel `0x443560` (the unit in enter state 2 / 4)
   and from the move-finished handler `0x443d10` (enter states other than 1 / 2; [M] the exit) → `0x3ac100`(host, 0).
   For the HTT (M = 0) both are `Deploy(false)`: the host undeploys (rises) once nobody is still coming in.
2. **At a move order (formation plan `0x41c1a0`).**
   - *Gate* (the executing call, mode ≥ 2; modes 0 / 1 skip it): the plan returns 0 (the formation gets no path)
     when **every** member is a `Deploys` type that is deployed or deploying and **not** an auto-deployer. A
     formation with any rolling unit, any non-deployer or any auto-deployer is planned.
   - *Who undeploys a deployed manual deployer at a move order (s78n t26 RE 2, replaces an [M]) [H]:* **nobody.** The
     group move `0x2a7b80` and the waypoint queue `0x2a8240` admit a unit only when it is an auto-deployer
     (`0x39a320`(x, 1)) or not D (`0x39a390`). A D manual deployer (a `Deploys` type with `Auto_Deploys` No: in FoC
     only the AT-AT family, Veers' AT-AT and the AT-AA; *s78n t26 RE 3:* **not the HTT**, an auto-deployer by the
     default, see the table above; deployed or walking deployed) is **skipped entirely**: it is not added to the coordinator, its
     behaviour orders 5 / 0x22 / 0x29 / 6 are not cleared and it is not moved (`0x2adff0`). It stays deployed and the
     order goes to the other selected units; an all-deployed selection gets no move. Only the DEPLOY ability
     (`0x3a1ce0`; the AI's `Activate_Ability("DEPLOY", false)`, AI_TACTICAL §7.3.3) or the garrison hooks of item 1
     undeploy it. The group move is the path of the player move events (`0x3ad100`, `0x3ae090`, `0x3ae6c0`,
     `0x3aebb0`, `0x3af790`, garrison `0x3ad620`), of Lua / AI `Move_To` (`0x426010` / `0x4f8200` via `0x73bf40`) and
     of further callers (`0x58fb80`, `0x590bb0`, `0x492d60`, `0x4ca930`, `0x5031c0`, `0x5355d0`, `0x6218f0`,
     `0x628820`, `0x6327d0`, `0x6364e0`, `0x63d0a0`, `0x644330`, `0x712fd0`, `0x3a38d0`, `0x4261f0`, `0x391310`,
     `0x594dc0`, `0x595a30`).
   - D excludes 0x18, so a **deploying** manual deployer (state 0x18) is admitted. When every member of the formation
     is such a unit, the gate returns 0 with no state change and the formation update `0x422b70` (cases 0 / 2)
     retries the plan at every formation update: the formation **waits** (after the deploy clip the unit is D and
     the gate still holds, so it waits for a DEPLOY-off or garrison undeploy). With any other member the gate passes
     and the deploying unit is planned like the rest [L: what the Walk 0x18 handler then does with its path is
     unread]. Other formation-side D tests: the range test `0x4160a0` (D with
     M == 0 → 0), the guard re-issue `0x4192b0` (skips D / deploying members unless M ≠ 0), Formation remove member
     `0x421910` (state 1: any member with pack +0x160 == 0 that is D, deploying or loco vf +0x198 false → formation
     state 2).
   - Locomotor vf +0x160 (Walk `0x61e440`: after the claim test vf +0x170, states 0x18 / 0x19 / 0x1A or the flag set →
     `Deploy(false)`, else state := 0x16; Bike `0x639b70` sets 0; the container `0x5ad3b0` sets 0x16): **[L] unread
     who calls it** (no caller found in the decompile).
   - *Member loop* (member order): for an auto-deployer, `0x3ac100`(member, d) with **d = `0x54f610`(formation path)
     = the summed 3-D length of every segment of the formation's path** (its first point to the destination): not the
     member's own distance to its slot, not a radius around the place it deployed. Then the stack reset `0x554480`,
     the formation path is set (vf +0x190 = `0x42c680`, with the corner pass of §3.5.2 D) and the start:
     - one-member formation (or the global flag `0x140b16c9c` with formation +0x9c clear, land mode): vf +0x148 only
       when the formation type (+0x218) ≠ 1 (*s78n t26 RE 2:* the global is GameConstants +0x149c
       `ShouldSkipLandFormup`, **true in FoC**, and +0x9c is set to 1 only by `0x417520` for a block with flag 0x20,
       so every ordinary land formation takes this branch; the path start of a moving unit is in §3.5.2 G); then `0x410170` marks the path started only in states 0xf–0x16 /
       0x21–0x24 when aligned. Land formations have type 1 [M: read from the locomotor vf +0x238 `0x18ceb0` = 1 that
       the callers of the setter `0x4241a0` use, not from the land composer `0x43c1b0`], so a deployed droid's path is
       **not started by the plan**: the 0x19 branch of item 3 starts it on the next Walk frame.
     - several members, type 1: a lead-in path from the member to its slot (pathfinder `0x5a2830`, or a straight line
       with block flag 0x20; kind 2) is pushed with vf +0x190 and **vf +0x148(0)** runs on it; other types: vf +0x148(0).
   - *Walk vf +0x148 = `0x61e570`* (path start, after the claim test vf +0x170): state not 0x18 / 0x19 / 0x1A and
     +0x2b3 clear → plain start `0x42ced0`. Otherwise L = `0x5c5530`(top path, position) = the remaining length of the
     top path from the unit; **L < M** → +0x2b3 := 1 and plain start; else `Deploy(false)`. The plain start acts only
     when vf +0x150 (`0x42c890`) reports idle (0x19 counts when M > 0): top path +0x6d := 1, state := 0.
   - So the cap is a **path length tested afresh at each order** and at each path start through vf +0x148 (the
     StoppedVsStopped resolution `0x687720`, the guard re-issue `0x4192b0`, FIGHTER_FLYING §4.11 copy branch); a
     multi-member order tests the formation path and then the lead-in. Nothing accumulates over successive orders.
3. **Per frame (vf6 `0x61e930`).**
   - **0x19 Deployed:** speed +0xec / +0xf0 := 0. When M > 0 and a top path exists: path +0x6d := 1, state := 0x12,
     +0x2b3 := 1 (**no length test here**; the state-0 turn decision is skipped); the frame ends. On the next frame
     0x12 goes to 0x13 and runs the move step (no `Walk_Transition`).
   - **Moving with +0x2b3 set (0x12–0x15)** is the normal Walk move (§3.5, §3.5.2) except:
     - the speed controller `0x61ccd0` uses **vmax = `Deployed_Walk_Speed`** (raw: no mode multiplier and none of the
       §3.4 chain) in place of the effective max speed: in the waypoint-fraction cap, the brake-ahead and the final
       clamp, so v ≤ 2.5 even under a higher formation speed. Acceleration / deceleration (vf +0x2d8 / +0x2e0) are
       unchanged (droid: 0.5 × 5.616 / 3.6 = 0.78 per frame);
     - the turn rates are unchanged: moving 15.5 × 1.56 = 24.18°/frame (`0x372560`), in place at rest 15.5
       (`Stopped_Rate_Of_Turn`, `0x374310`);
     - the move step `0x620820` still takes f = v / the **undeployed** effective max speed (5.616) for the 0x12 / 0x14
       turn-budget scaling and the clip rate;
     - state 0x13 (`0x61f840`) plays clip 0x73 (Deployed_Walk) at `Deployed_Walk_Anim_Speed` instead of the move clip
       (visual);
     - the formation speed `0x41b2a0` (minimum over the members of locomotor vf +0x250 = Walk `0x42c5e0` =
       `0x370f00`; capped at `0x140b16d54` when the last block has flag 0x10) has **no deployed switch**: in a mixed
       formation the slots travel at the others' speed and the walking-deployed droid falls behind [M: consequence,
       not observed].
   - **0x16 Idle with no path** (after the arrival pop, or a stop):
     - +0x2b3 set → +0x2b3 := 0, state := 0x19 **at once**, the Deploy clip (0x19) set on its last frame (blend 0.2,
       rate 1.0); no signal 0x29, no `0x39fec0`; the 0x19 handler runs in the same frame.
     - +0x2b3 clear, `Deploys`, M > 0 and **no DEPLOY ability** (`0x3986b0`(obj, 2) false) → `Deploy(true)` (0x18,
       the Deploy clip at `Deployment_Anim_Rate`). So the droid **deploys at every stop**, whatever its target, and
       is idle only in 0x19. *s78n t26 RE 3 [H]:* the test (`0x61e930` case 0x16: `0x39a370` = type +0xd1, then
       type +0x23b0 > 0, then `0x3986b0`(obj, 2) false) **does not read `Auto_Deploys`**; only M > 0 limits it. In
       FoC every M > 0 type is also `Auto_Deploys` Yes, so the difference never shows; a mod with M > 0 and
       `Auto_Deploys` No would still deploy at every stop.
   - **0x1A:** +0x2b3 := 0 on every frame of the undeploy.
   - **0x1A entered from a walk (s78n t26 RE 2) [H unless tagged].** `Deploy(false)` (`0x61d300`) only writes state
     0x1A (after the slot +0x35b block test): no stop, no speed or path change. The 0x1A handler only clears +0x2b3,
     waits for the turret / plays the clip and, at the end, sets state 0 and signal 0x29 in the same loop. It runs
     **no move step, no turn and no speed step**, so the position and yaw are frozen for the whole clip while
     +0xec / +0xf0 keep their last walking values (the unit still counts as Moving, §5.2). Then state 0: the new path
     is not started (0x1A is not in the `0x410170` list, §3.5.2 G) → 0x16, the frame ends; the next frame starts the
     path, state 0 runs start test A → 0x12, and the controller starts from the **stale** v. The in-place turn 0x10
     uses `Stopped_Rate_Of_Turn` only when +0xec ≤ 0, so with a stale speed the moving rate R applies. Writers of
     +0xec scanned: `0x509090` (TerrainCollision report, zeroes it; reached only from the move step), 0x15, 0x19, the
     stop `0x5ad320`, the controller; none runs in 0x1A [M: the scan is by pattern, not exhaustive].
   - `Take_Damage` (`0x3a9e30`): M > 0, Walk state 0x16, `Deploys` and +0x2b3 clear → `Deploy(true)` (the same rule
     for a hit that lands before the Walk frame).
4. **What a move order does to the droid** (M = 150; d = the item 2 length):

   | state at the order | d | result |
   |---|---|---|
   | deployed (0x19) | < 150 | walks deployed at ≤ 2.5 with the shield up; at the end 0x19 at once (no clip) |
   | deployed (0x19) | ≥ 150 | 0x1A, undeploy clip at rate 3.0 (no turret wait), then 0 → 0x16; the next frame starts the path and it rolls at up to 5.616; at the end it deploys (0x18 clip → 0x19 + signal 0x29) |
   | deploying (0x18) | < 150 | flag set; the deploy clip finishes (0x19, signal 0x29), then the 0x19 branch walks deployed |
   | deploying (0x18) | ≥ 150 | 0x1A, the undeploy clip reversed from the current frame (§3.2) |
   | undeploying (0x1A) or rolling | any | `Deploy(false)` changes nothing: it rolls (after the undeploy) and deploys at the end |

   A rolling droid is never switched to walking deployed by an order, even a short one.
5. **Movement and collision rules.**
   - *Moving* (§5.2, `0x42cc60`): speed > 0 or any path. A walking-deployed droid is Moving; deployed and idle it is
     the stopped side.
   - *Push* (`0x6859a0`, §5.4.1): refused only when M ≤ 0 and the unit is deployed / deploying, so a deployed droid
     **is pushed**. The pushed path is marked started and the state stays 0x19, so the 0x19 branch walks it deployed,
     with no length test.
   - *StoppedVsStopped* (`0x687010` rule 3, §5.4.3): imm(x) needs M ≤ 0, so the droid is never immobile and can be the
     one moved; the chooser's cost uses `0x370f00` (the undeployed speed). The resolution starts the path through
     vf +0x148 (`0x61e570`): remaining length < 150 → walks deployed, else undeploys.
   - vf +0x1a0 (`0x61e380`, "can start"): 0x18 → false; 0x19 with +0x2b3 set or M ≤ 0 → false; else `0x42ccb0`.
     vf +0x150 (`0x42c890`): 0x19 is idle when M > 0 (the formation counts the member done, §5.4.1 item 4).
   - Footprint: D is true while walking deployed; the droid has no deployed extents, so nothing changes (§5.3).
6. **Combat while walking deployed** (only where it differs from rolling):
   - shield: `Shielded_Only_When_Deployed` gates the absorb `0x549ac0` (and the hit visual `0x31de70`) on D, so the
     droid's shield takes damage when deployed **and while walking deployed**, not while deploying (0x18 with the
     flag clear), undeploying or rolling;
   - the own-weapon gate (TACTICAL_COMBAT §3.1.1 "Step 3 correction") counts D, so `Fire_Weapon_When_Deployed`
     applies; step 2 still refuses 0x18 / 0x1A. `Service_Land` (WEAPON, no TURRET) treats a unit with a path as
     moving: state 4 only when the model has no Attack (type 5) clip, else 2 [M: the droid model's clip list was not
     checked];
   - with a target out of range, M > 0 keeps the target (state 2) instead of dropping it (`Service_Land`); the
     auto-deploy check `0x568e60` is false while D.

**Port: what must change** (read only; today nothing of this is modelled, `docs/PHASE4.md`: "a deployed unit never
walks"):
- `UnitDesc` (`engine/tactical/movement/include/alamo/tactical/movement/unit_params.hpp`, `src/unit_params.cpp`): add
  M, `Deployed_Walk_Speed` (raw), `Auto_Deploys` and "has the DEPLOY ability". `Deployed_Walk_Anim_Speed` already
  reaches the anim side (`kAnimDeployedWalk` 115, `anim_behaviors.cpp`).
- `MovementSystem::Unit`: the walking-deployed flag, saved (`movement_save.cpp`) and hashed (`hash_deploy`).
- `MovementSystem::move` / `move_land` (`movement_system.cpp`): per member in order, auto-deployers → item 1 with d =
  the formation path's total length (so every M = 0 auto-deployer of the table above, the MAL, SPMAT, MPTL, MDU,
  Dark Trooper P3, Juggernaut, HTT and F9TZ included, and a far-ordered droid undeploy; `Auto_Deploys` defaults to
  true); a formation of deployed manual deployers (AT-AT family / AT-AA) only → no plan; path starts follow the `0x61e570` rule (lead-in, collision and guard
  paths).
- The walk gate (`movement_system.cpp`, `u.deploy != LocoState::None` skips the walk): a 0x19 unit with a path and
  M > 0 starts walking deployed (item 3), with vmax = `Deployed_Walk_Speed` in the speed controller.
- `MovementSystem::deploy` / `tick_deploy` (`deploy_system.cpp`, the "[port] pending flag … not modelled" notes): the
  0x16 rules (walking deployed → 0x19 at once with no 0x29 event; `Deploy(true)` at every stop when M > 0 and no
  DEPLOY ability), 0x1A clears the flag, `Deploy(true)` does nothing while the flag is set.
- Collision: the push refusal and the StoppedVsStopped `immobile` lambda (`movement_system.cpp`) must read "deployed /
  deploying **and M ≤ 0**". *Corrected s78n t26 RE 2:* this bullet also listed the Path Around obstacle set
  (`around_obstacles`); that test has **no** M clause in retail (`0x67d580`, §1.5.1 item 3: a standing deployed /
  deploying unit is always an obstacle) and stays as it is.
- Orders (s78n t26 RE 2): the group move skips D manual deployers (item 2; FoC: AT-AT family and AT-AA only, s78n
  t26 RE 3); a formation with a deploying manual deployer waits.
- Combat: `CombatSystem::deployed_or_deploying` (`combat_system.cpp`, "vanilla never sets the pending flag") must
  include the flag; the shield absorb in `CombatSystem::take_damage_inner` has no `Shielded_Only_When_Deployed` gate;
  `TacticalBattle::deployed_half_extents` (`apps/sim/src/tactical_battle_bt.cpp`) includes the flag; the host's damage
  path needs the `0x3a9e30` deploy.
- Anim: clip 115 while walking deployed (`UnitView::deploy_state` stays 0x19, so the view needs the flag too).

### 3.3 Tick order [H]

GameObjectManager update (`0x2be640`) runs in this order:
1. Per-player tree upkeep (`0x20ed70`).
2. Walk the intrusive object list (GOM +0xf0) **in list order** (creation order, [M]) and service each object (`0x3a6b80`).
3. Transform flush for the dirty list (GOM +0x138, `0x3ac530`).
4. Delayed spawns.

Object service (`0x3a6b80`) does:
1. Snapshot the previous transform (data pack +0x194).
2. Update modifiers.
3. Service behaviours **in reverse array-index order** (array +0x278).
   - A behaviour runs only when its next-service frame (+0x30) ≤ now and its suspend count (+0x3c) is 0.
   - At the start of each service, next = now + interval (+0x34, default 1).
4. If the dirty flag is set (object +0x3a1 bit 0, set by the position setter `0x3a8f90`), recompute transform and bounds (`0x3ac530`). This feeds the cull tree's `Update_Bounds` (HOTSPOTS §3). [M] for the exact link.

Inside one locomotor service: path follow → speed controller → turn → integrate → passability check → terrain conform → set position.

The collision system is serviced by the land mode (§5.2). Its order relative to the object walk was not pinned down. [M]
*(s78n t23 RE, 2026-11-10) [H]: the land mode update `0x3639d0` runs the object walks `0x2be640` (both lists) first, then the
collision driver `0x50a780` (detection `0x508170` at its end), then the movement coordinators `0x426eb0`. So the speed
fold of frame N reads the record answers left by frame N − 1's driver (§5.4.1 "The stamp in full" item 6).*

### 3.4 Speed, acceleration and turn rate [H]

**Effective max speed** (`0x371160`, then `0x376990`):
- Base: `Max_Speed` (+0x37c).
- For a team/container (group behaviour 0x16), the base is capped at the **minimum Max_Speed of its members** (`0x4049e0`).
- **Team cap, exact (s78l t01, 2026-11-05) [H]:** `0x371160` / `0x370f00`(type T, object O): when O has a TEAM
  behaviour (vf +0x10 with 0x16), base = **min(T.`Max_Speed`, m)**, where m = `0x4049e0`(TEAM) = the minimum over
  the TEAM member list (container +0x110, list order, null entries and members without a type skipped) of
  `0x371160`(member type, member) = the member's **raw** `Max_Speed` (members have no TEAM slot). So the
  **container's own `Max_Speed` is in the minimum** (`fVar2 <= container ? fVar2 : container`). No member
  multiplier enters the base: not their SPEED products, fleeing, burning, mode scale. Every multiplier below is then
  applied **once, to the container object**. Edge cases straight from the code: member list pointer null → m = 0 →
  base 0; list present but no valid member → m = −1.0 (the start value) → base −1 [M: a live team never has
  this]. FoC data: `Stormtrooper_Team`, `Plex_Soldier_Team`, `Empire_Field_Commander_Team` and four civilian /
  pirate / Hutt teams have `Max_Speed` 0.8 while a member reaches 1.0, so the container caps them; for the other
  teams (e.g. `Rebel_Trooper_Team` 1.1, `Sandpeople_Team` 1.0 over 0.5 members) the members are slower.
- **Full team formula** (O = the container, all values of O unless named):
  `v = min(C.Max_Speed, min_i M_i.Max_Speed)` → v is **replaced** by the script override speed (0x397140) when pack +0x29c is set →
  × mode scale (mode +0x230; O is not a projectile) → × `Fleeing_Infantry_Speed_Bonus` when O flees (0x39a5b0) →
  × **SPEED(O)** = `0x398010`(O, 6): O's own unit-ability product when that is ≠ 1.0, else the minimum over the
  members' own products (TACTICAL_COMBAT §4.4) → the rest of the chain below (engines, retreat, `(1 + Σ bonus)` of
  O's modifier list, base shield, O burning). A port must **not** multiply a member's own SPEED or modifiers into
  a team move.
- Projectiles (behaviour slot 8, obj +0x33a ≠ −1) take a shorter chain: the mode scale only when the type's
  category (+0x1fe8) is 1, fleeing, and then the function returns unless that category is 1, 2, 3 or 9 [H].
- The base is then multiplied, in this order, by:
  1. a script override speed, when data-pack flag +0x29c is set;
  2. the mode's speed scale (mode +0x230);
  3. **Fleeing_Infantry_Speed_Bonus = 2** when fleeing;
  4. combat modifier 6;
  5. **Engines_Disabled_Speed_Modifier = 0.4** when engines are disabled;
  6. retreat pursuit factors from the faction (+0x20b4 space / +0x2194 land) [M];
  7. (1 + the sum of the speed bonuses), which is where `Movement_Speed_Bonus_Percentage` lands [M];
  8. **Base_Shield_Speed_Modifier = 0.5** when base shields are active [M];
  9. `On_Fire_Speed_Modifier` (+0x2040) when burning.

**Mode multipliers** (`Object_Max_Speed_Multiplier_*`) are galactic **5.0**, land **1.56**, space **1.2**. They are applied by the type getters for turn rate, roll, alternate turn rate, min speed and vertical step (`0x372440`, `0x3724d0`, `0x372560`, `0x370fd0`, `0x3728f0`), together with a global factor at `0x140a28568` whose value was not read. They are skipped when type flag (+0x790)→byte1 bit0 is set.

**Acceleration** (`0x372fe0`, locomotor vf91 `0x42c400`) = `OverrideAcceleration` (+0x3c0) × (effective speed / Max_Speed). Deceleration (`0x373080`, vf92 `0x42c4c0`) follows the same rule with `OverrideDeceleration` (+0x3c4).
- **If the override is ≤ 0, the value is the full effective max speed**, i.e. full speed in one frame.
- Units with `Walk_Transition` (+0x35) derive acceleration from the frame counts of their start/end-move animations instead.
  *s78n t26 RE 2 (replaces the [M]) [H unless tagged]:* V = the effective max speed `0x370f00`(type, obj); S =
  `0x372b90` = `Scale_Factor` (+0x658) × `Movement_Animation_Speed` (+0x370).
  - Acceleration, vf +0x2d8 `0x42c400` (shared by Bike, vftable `0x8b23c0`): n = the frame count of clip 16
    (MOVESTART, `0x38f9e0`(obj, 0x10)); n > 0 → **a = ((S / n) × (V / S)) × (V / S)**; n ≤ 0 → V.
  - Deceleration, vf +0x2e0 `0x42c4c0`: u = max(0.75 × V, v0), v0 = pack +0xf0 if > 0, else +0xec if > 0, else V;
    m = the frame count of clip 17 (MOVE_ENDONE); m ≠ 0 → **((S / (m − 1)) × (u / S)) × (u / S)**; m = 0 → u.
  - Units without the flag: `0x372fe0` / `0x373080` when > 0, else V (as above).
  - The frame counts are the model's (ALA) clip lengths [M: variant 0 of the clip].
  - The 0x12 / 0x14 transition states that use them: §3.5.2 F.
  - FoC types with `Walk_Transition` Yes (WALK_LOCOMOTOR): `AT_AT_Walker`, `Veers_AT_AT_Walker`, `AT_ST_Walker`,
    `AT_AA_Walker`, `SPMAT_Walker`, `Pod_Walker`, `Hutt_Pod_Walker`, `Scavenger_Droid`, `Rancor`,
    `Cuddles_The_Rancor`, `Night_Sister`, `UM06_Night_Sister`, and their variants (`*_Captured`,
    `AT_AT_Walker_REB09`, `Rancor_Dark_Ally`, `Pod_Walker_Rebel` / `_Empire` / `_Underworld` / `_Pirate`,
    `UM05_Cuddles_The_Rancor`, death clones, CIN / FIN props). BIKE_LOCOMOTOR: `Scout_Trooper` and
    `Scout_Trooper_Converted_To_Enemy` (same getters; [L] the Bike state handlers are unread). `TaunTaun` sets No.
  - Example inputs (`Scale_Factor` / `Movement_Animation_Speed` / `Max_Speed`): AT-AT 1.75 / 0.1366 / 0.5; AT-ST 1.6 /
    0.8117 / 1.2; AT-AA 1.5 / 0.25 / 0.65; SPMAT 1.25 / 0.2151 / 0.6; Pod_Walker 1.0 / 0.8117 / 1.1; Rancor 1.0 /
    0.75 / 0.75; Night_Sister 1.0 / 0.80 / 0.9; Cuddles 0.8 / 0.8 / 0.75; Scout_Trooper 1.5 / 3.0 / 5.0.

**Turn rate** (`0x372560`) = `Max_Rate_Of_Turn` (+0x38c) × the mode multiplier × the global factor. While
airborne-kind, the JetPack's speed / turn-rate getters (vf74 / vf75, read by the planner request and the formation
speed) answer `Alternate_Max_Speed` / `Alternate_Max_Rate_Of_Turn` instead (§3.1.1); no other locomotor reads them.
The infantry turn-animation time = angle / rate × 1/30 (`0x62bef0`).

**Defaults:** from OBJECT_TYPES.md, Max_Speed 0.25, Max_Rate_Of_Turn 2.3, Mass 0.995.

### 3.5 Walk locomotor per frame (vf6 `0x61e930`, land only) [H]

Items 1–3 are a summary; the exact start test, speed controller, move step and the corner speeds are in §3.5.2
(s78n t26). Walking while deployed: §3.2.1.

1. **Start.**
   - If the heading error ≤ one frame's turn, or ≤ `Close_Enough_Angle_For_Move_Start` (+0x16b4; GameConstants CloseEnoughAngleForMoveStart = 180), and the first waypoint's speed fraction is > 0: go to Start Move (0x12).
   - Otherwise go to Start Turn (0xF): turn in place first.
2. **Speed controller** (`0x61ccd0`):
   - Target = effective max speed, or `Deployed_Walk_Speed` (+0x23b8) when deployed, or the formation speed (data pack +0x158).
   - Cap the target by the current waypoint's speed fraction × max.
   - **Brake:** if the next waypoint's cap v_next < v and the distance to that waypoint ≤ (v² − v_next²)/(2·decel), the target becomes v_next.
   - Cap the target again by the collision system's speed cap (`0x509ea0`); this cap may force an instant set (speed slaving, §5.4).
   - Step: v moves toward the target by at most accel (up) or decel (down), clamped to [0, max].
3. **Move step** (`0x620820`):
   - The turn rate is scaled by the speed fraction during Start/End Move.
   - With `UseSecondaryFacing` (+0xd2), the turn coefficient = clamp(angle / `SecondaryTurnAngle` × speed fraction, `MinSecondaryTurnROTCoefficient`, `MaxSecondaryTurnROTCoefficient`).
   - Yaw steps toward the target by at most the rate (`0x61e730`).
   - Direction of travel (vf81 `0x61e0f0`) depends on `Movement_Directions_Count` (+0x23fc):

     | value | direction |
     |---|---|
     | ≤ 0 | the desired direction |
     | 1 | the current facing |
     | N | the best of N directions spaced 360/N from the facing (largest dot product) |

   - The distance budget v is spent along the waypoints. One frame may pass several waypoints; the remainder carries over to the next segment.
   - Switch to End Move when the remaining path ≤ v²/(2·decel).
4. **Passability check before commit.** Unless the path's skip flag is set, the new position must pass the movement-class test of §1.1 (`0x50e280`). On failure, report to the collision system (`0x509090`) and stop this frame: the position is not committed, the speed is set to 0 and a TerrainCollision record is made (§5.4.7).
5. **Normalisation.** Vector normalisation uses the fast inverse square root (magic 0x5f375a86, two Newton steps). **Reproduce this bit-exactly**: it feeds positions.
6. **Formation jiggle** (`0x42c270`). Each moving frame, the offset += synced random(±k) × speed fraction × `FormationRaggedness` (+0x16a8), clamped to ±`MaxJiggleDistance` (+0x16ac). Each call draws from the lockstep RNG.
7. **Hover rise/lower.** z moves by the effective max speed per frame toward terrain + `Hover_Height` (+0x23bc). Deploying states run at `Deployment_Anim_Rate`.

#### 3.5.1 Facing on arrival (s78k t01, 2026-11-05) [H unless tagged]

The Walk locomotor's wanted orientation (0x61d570 → 0x61d890 on the unit's current path, then 0x61ca90):
- while the path is started (path +0x6d) and not on its last waypoint: the travel direction;
- on the last waypoint (path +0x60 + 1 ≥ waypoint count, 0x5c5910): the last waypoint's facing vector (+0x0c) when
  it is non-zero, else the **current** orientation;
- 0x61ca90 then applies `Movement_Directions_Count` (+0x23fc, default 1; one FoC unit uses 2): 1 → that orientation;
  ≤ 0 → the current orientation; N → of the N yaws spaced 360/N starting at the wanted one, the one closest to the
  current facing.

End Move (state 0x14, 0x61f510) switches to state 0x15 (0x61fc30) once the speed is at or below one frame's
deceleration. State 0x15, on the last waypoint:
- Δ = |wanted yaw − current yaw| (current = obj +0x90…). The threshold is **10°**, or the turn rate (`0x372560`)
  when that is ≤ 10° and (path kind +0x48 = 3 or the current waypoint's speed cap is 0) (s78l t02 below: kind 3 =
  the TerrainCollision path; ordinary move orders use 10°).
- Δ > threshold → state 0xf: with `Has_Pre_Turn_Anim` (+0x36) and the clip present, play turn clip 0xc (Δ in
  (0°, 180°)) or 0xe and wait for it; then state 0x10 (0x620190): each frame the yaw steps toward the wanted yaw by
  at most the rate (0x61e730; the step that covers the remainder lands **exactly** on it). Rate: locomotor pack +0xe8
  when > 0 (it is zeroed on entering 0x10), else at speed 0 `Stopped_Rate_Of_Turn` (+0x3b0, raw, 0x374310) when > 0,
  else `Max_Rate_Of_Turn` × the mode multiplier (0x372560); × `SecondaryTurnInPlaceROTCoefficient` (+0x1670) for the
  secondary-facing step. When reached: 0x11 → 0x12 → back to 0x15, now within the threshold.
- Δ ≤ threshold → pop the path (0x557b10), play the default idle (when not a flinch, not burning, not deploying) and
  go to 0x16 (idle). The unit keeps its yaw, so a final facing within the threshold is **not** corrected.

So a move whose block has a final facing (+0x44 / +0x5c, §6.1) ends with a rate-limited turn-to onto that facing; a
move without one ends on the arrival heading. A path replaced or dropped before the arrival (new order, stop) never
turns to the old facing.

**Arrival turn, details (s78l t02, 2026-11-05)** [H unless tagged]. Read from 0x61fc30 (state 0x15), the dispatch
loop of vf6 0x61e930, 0x620190 (0x10), 0x6204f0 (turn band), 0x61d810, 0x61e730, 0x5c4af0 / 0x5c5ef0, 0x5070b0.
Angles are degrees; the yaw is obj +0x8c (the setter 0x3a8710 writes +0x84… and +0x90… together, so 0x61fc30's
read of +0x98 is the same yaw). Every Walk handler returns "done for this frame" (1) or "run the new state now" (0),
so 0x15 → 0xf → 0x10 → 0x11 → 0x12 can all run in one frame.
1. **State 0x15 entry**: pack speed +0xec and +0xf0 := 0. With a started current path: if the path index (+0x60) +
   2 = waypoint count (heading for the last point) and dot(pos − W, n) > **−5**, where W = the last waypoint and n =
   the unit segment direction (0x5c5060), the index steps on (0x5c4920: +0x60 += 1, +0x64 := 0, +0x6c := 0). Then
   not on the last waypoint → state 0 (Start: the move resumes). So a unit that stopped up to 5 units short of the
   last point (measured along the segment) counts as arrived.
2. **Δ and the threshold**: wanted yaw w (§3.5.1 above; when 0x61d570 returns the 1e18 triple the turn is skipped
   and the path pops). Δ = |wrap(w − yaw)|, wrap into (−180, 180]. Threshold = 10, **or** R = `Max_Rate_Of_Turn` ×
   mode multiplier × global factor (0x372560, type only) when R ≤ 10 **and** (path kind = 3 or the speed cap of
   waypoint +0x60 is 0). Δ > threshold → state 0xf; else pop.
   - (a) **The variant changes only the threshold, not the turn rate**: the turn itself (0x10) uses the rule of
     step 4 in both cases; with the variant a unit is turned until it is within one frame of normal turn
     (Δ ≤ R) instead of within 10°.
   - When it applies: the per-waypoint speed caps (path +0x00, 8 bytes per waypoint, cap at +4) default to
     **1.0** (0x5c5ef0), the land finaliser 0x5070b0 sets the last entry to (1, 1) and only lowers earlier ones (brake
     pass from count − 2 down), and an index past the end reads a static default [M: **(1.0, 1.0)** in the image at 0x140a440f8 / 0x140a44100, corrected 2026-11-10; the earlier "zero" was a guess]. The only cap-0 writer
     found is the TerrainCollision path (0x687ff0: entry 0 := (1.0, 0.0)), whose path is built by 0x5c80e0 with
     **kind 3**; the other kinds seen are 1 formation, 4 push, 7 crush push, 5 (three locomotor-internal paths).
     So an ordinary move order (formation path, last cap 1.0) always uses **10°**; the variant is for
     collision-resolution paths [M: no other kind-3 writer was searched for exhaustively].
3. **State 0xf (start turn)**: side = left when wrap360(w − yaw) ∈ (0, 180), else right. With `Has_Pre_Turn_Anim`
   (type +0x36) and the model has the side's **TURNL_BEGIN 12 / TURNR_BEGIN 14** clip (0x38f9e0): if the current
   clip is not that clip, play it (index 0xFFFF = synced variant draw when the clip has ≥ 2 variants, loop 0, blend
   global 0x140b398fc, speed 0x3736a0 = (`Stopped_Rate_Of_Turn`, or R when ≤ 0) / `Rotation_Animation_Speed`
   (+0x388) when both > 0, else 1) and end the frame; while it is the current clip and not within its last 2 frames
   (0x39a180: frame ≥ count − 2 − x, or mode 2 finished), wait. **The yaw does not move during the begin clip.**
   Then (or at once without +0x36 / without the clip) pack +0xe8 := 0 and state 0x10 in the same frame.
4. **State 0x10 (turn)**, per frame:
   - rate r = pack +0xe8 when > 0; else, since the state is 0x10, `Stopped_Rate_Of_Turn` (+0x3b0, raw, 0x374310)
     when the speed +0xec ≤ 0 (always at arrival, step 1 zeroed it) and +0x3b0 > 0, else R. Secondary rate r2 = r ×
     `SecondaryTurnInPlaceROTCoefficient` (+0x1670).
   - yaw step (0x61e730): e = wrap(w − yaw); if |e| > r the yaw moves r toward w and r_left = 0, else the yaw
     **lands exactly on w** and r_left = r − |e|. Roll and pitch := 0, position committed (vf +0x2f8). The secondary
     facing (pack +0x268…+0x270) steps the same way with r2 (0x61e840). reached = r_left > 0; on the last waypoint
     with `UseSecondaryFacing` (+0xd2) reached = r2_left > 0 instead.
   - c = the current clip is within its last 2 frames or finished (0x39a180); A = the current clip.
   - reached, not c and no path → 0x11. Else if A is a turn clip (3, 4, 21–24) and not c → stay (r stays).
     Else if (a path exists and is not started) or reached → **0x11**. Else pick a turn clip:
     - **without** `Has_Pre_Turn_Anim`: only when the model has clip 3 and A is not a turn clip: play TURNL 3 (e at
       frame start ≥ 0) or TURNR 4, index 0xFFFF, loop 0, blend min(|e0| / r × 0.03333, GC 0x140b16d28), speed
       1.0. +0xe8 is not written. (So the clip plays once; the yaw keeps turning at r.)
     - **with** `Has_Pre_Turn_Anim` (0x6204f0): e = the signed error **after** this frame's step, S = 0x374310
       (`Stopped_Rate_Of_Turn` or R), band threshold T = S, or **10** when a path exists, its kind ≠ 3 and the
       current waypoint cap > 0 (the normal arrival). For e ≥ T: e < 22.5 → TURNL_QUARTER 22, rate S × 0.25 × e /
       11.25; e < 45 → TURNL_HALF 21, rate S × 0.5; else TURNL 3, rate S. Mirror for e ≤ −T with 24 / 23 / 4.
       |e| < T → clip unchanged, rate S. Pack +0xe8 := that rate (used from the **next** frame; the first 0x10
       frame always turns at S). If the chosen clip is not idle-class (0x27b5e0: 0, 1, 71, 77–82) it is played
       (index 0xFFFF, loop 0, blend global 0x140b39900, speed 0x3736a0) and the state stays 0x10; if it is
       idle-class (|e| < T with an idle playing) → **0x11 with |e| < T left over** (up to 10° on an ordinary
       arrival, which step 2 then accepts).
   - The band is re-picked only when the previous turn clip reaches its last 2 frames, so the rate changes at clip
     boundaries.
5. **State 0x11 (end turn)**: with `Has_Pre_Turn_Anim` and the model has the end clip 0x61d810(A) (A ∈ {3, 12,
   13, 21, 22} → TURNL_END 13; A ∈ {4, 14, 15, 23, 24} → TURNR_END 15; otherwise none): play it (same arguments
   as the begin clip) and wait until it is in its last 2 frames. Then state 0x12 (same frame) → back to 0x15, where
   Δ is now within the threshold.
6. **Pop** (step 2 within the threshold): 0x557b10 pops the path; then, when the current clip is not idle-class
   (0x27b5e0), the object is not burning (+0x3a1 & 2) and pack +0x2b3 is clear, play the default idle type
   (0x395920; TACTICAL_COMBAT slot 47 "s78l t03") with index 0, loop = `Loop_Idle_Anim_00` (type +0x30), blend
   global 0x140b39908, speed 1. State 0x16.

FoC users of `Has_Pre_Turn_Anim` (15): AT_AT_Walker / Veers_AT_AT_Walker (`Stopped_Rate_Of_Turn` 1,
`Max_Rate_Of_Turn` 2, `Rotation_Animation_Speed` 0.5), AT_ST_Walker and Scavenger_Droid (7.5 / 7.5 / 2.2),
AT_AA_Walker (3.5 / 6 / 1.25), SPMAT_Walker (3.1 / 3.5 / 1.0), Pod_Walker and Hutt_Pod_Walker (3.75 / 4 / 1.5),
HAV_Juggernaut (– / 0.9 / –), and the creatures Rancor, Cuddles_The_Rancor, TaunTaun, Felucia_Beetle, Night_Sister,
UM06_Night_Sister (no stopped rate). Everyone else: no begin / end clips, TURNL / TURNR once, rate S throughout.

*Port spec (arrival turn):*
- Arrival = End Move finished (speed ≤ one frame's decel) and either on the last waypoint or within 5 units of the
  last point along the last segment (then step on); otherwise resume the move.
- Δ > 10° (or > R on a kind-3 / zero-cap path when R ≤ 10) → turn; else pop the path, play the default idle, idle.
- `Has_Pre_Turn_Anim` with a begin clip: hold the yaw while TURNx_BEGIN plays to its last 2 frames.
- Turn frames: first frame at S (`Stopped_Rate_Of_Turn`, else R); exact landing when the remainder ≤ rate.
  With `Has_Pre_Turn_Anim`: after each step pick the band (T = 10 on ordinary paths) and use its rate from the next
  frame; keep the band until the turn clip's last 2 frames; |e| < T with an idle clip → end the turn early.
  Without it: rate S every frame, one TURNL / TURNR clip.
- End: TURNx_END clip (with +0x36), then re-check Δ (now ≤ threshold) and pop.
- Synced draws: each play with index 0xFFFF of a clip with ≥ 2 variants draws one `Get_Int(0, n − 1)` (begin, band,
  TURNL / TURNR, end clips). A clip with one variant draws nothing (0x1ffb40 returns at once for an empty range).

**Port:** `MovementSystem::tick_land` (`engine/tactical/movement/src/movement_system.cpp`) ends a land move at
`r.arrived` without looking at `Unit::facing` (set from `MoveOrder::facing_deg` in `move_land`); only `tick_space`
turns on arrival. Missing: the arrival turn above (threshold 10°, stopped turn rate, exact landing,
`Movement_Directions_Count`).

#### 3.5.2 The Walk move in full: start test, speed controller, move step, corner speeds (s78n t26 RE, 2026-11-10) [H unless tagged]
Read from vf6 `0x61e930` (state 0), `0x61ff40` (0x12), `0x61f840` (0x13), the speed controller `0x61ccd0`, the move
step `0x620820`, the container move step `0x5adfb0`, `0x20afe0`, `0x54f700`, `0x371310`, the path set `0x42c680` and
the corner-speed pass `0x5070b0`. This completes §3.5 items 1–3, §5.4.1 "How Walk combines" item 4 and FIGHTER_FLYING
§7.7 item 7, and replaces FIGHTER_FLYING §8. Neither earlier text was complete.

Notation: R = `0x372560`(type) (`Max_Rate_Of_Turn` × mode × global, degrees per frame); vmax = the effective max
speed `0x370f00` (§3.4); a = deceleration (vf +0x2e0), acc = acceleration (vf +0x2d8); P = the top path record,
idx = P +0x60; wp[i] = its 0x34-byte waypoints, whose 8-byte entry holds (+0) a turn factor, always 1.0, and (+4) the
**speed fraction**; line(i) = the waypoint line `0x5c4ec0` (through wp[i], normal n = the bisector of its two
segments; the last waypoint: its segment's direction); d = unit(wp[idx+1] − wp[idx]) (`0x5c5850`, 2-D, fast
normalise); the wanted yaw = `0x61d570` (segment direction + lane term + collision vector, §5.4.1 items 1–3).

**A. Start (state 0).** No top path → the frame ends; a path not started → state 0x16, the frame ends. Else δ = the
angle from the facing to the wanted yaw; **|δ| ≤ R, or (|δ| ≤ CE and wp[idx].+4 > 0)** → 0x12 (Start Move), else 0xf
(turn in place: 0xf / 0x10 / 0x11 as in §3.5.1, then 0x12). CE = `0x371310` = `Close_Enough_Angle_For_Move_Start`
(+0x16b4) when > 0, else GameConstants `CloseEnoughAngleForMoveStart` (FoC 180). FoC values: `AT_AT_Walker` and
`Veers_AT_AT_Walker` 30, `SPMAT_Walker` 45, `Dust_Devil` / `Bugs Swamp` 180, `Ewok_Suicide_Bomber_For_Handler` 360.
With CE ≥ 180 a unit **never turns in place before a move** unless wp[idx].+4 is 0 (item D, or a Path Around
detour, §5.4.7 item 6).
- State 0x12 (`0x61ff40`): `Hover_Height` > 0 and not raised → rise first (§5.3 "Hover"); no path or on the last
  waypoint → 0x15. Started path without `Walk_Transition` (+0x35) → state := 0x13 and the move step **in the same
  call** (so the 0x12 budget scaling of item C never applies to such a unit); with it, the start-move clip (0x10) is
  played (speed 0 on its first frame) and the move step runs in 0x12 until the clip ends, then 0x13. A path not
  started → 0x15 (with `Walk_Transition`: 0x14 / 0x15 by the clip).

**B. Speed controller `0x61ccd0`** (first thing in each move step; its result never stops the step):
1. vmax as above; walking deployed (pack +0x2b3): `Deployed_Walk_Speed`, raw (§3.2.1).
2. T = vmax; on a formation path (vf +0x1c8 `0x42cc20`: top record kind +0x48 = 1) T = the formation speed (pack
   +0x158), and T = 0 sets state 0x14.
3. No top path, or not started: T = a.
4. **Corner cap:** P's turn-done byte +0x6c clear, the facing **not** within R of the wanted yaw (`0x20afe0`:
   |wrap(yaw − wanted)| < |R|, strict) and state ≠ 0x12 → T = min(T, wp[idx].+4 × vmax); a result of 0 sets 0x14.
5. **Brake-ahead** (state ≠ 0x12): v_n = wp[idx+1].+4 × vmax. When v_n < v and the distance D (item C.7) from the
   unit to line(idx+1) along d is ≤ (v² − v_n²) / (2a) (in double) → T = min(T, v_n); 0 sets 0x14.
6. Collision cap (state ≠ 0x12): T = min(T, `0x509ea0`), which may set the "at once" flag (§5.4).
7. State 0x14: T = v (hold) with `Walk_Transition` while the current clip is 2 or 0x42; else **T = a**.
8. Step: T > v → v += min(acc, T − v); T < v → v := T with the "at once" flag, else v −= min(a, v − T). Then
   v := min(v, vmax), or 0 when negative.

**C. Move step `0x620820`** (one call per frame in 0x13 and 0x14; in 0x12 only for `Walk_Transition` units):
1. pack +0x2b8 := 0. vmax_u = `0x370f00` (never the deployed speed); vmax_u = 0 → return. P not started → state
   0x14. Controller (B). v = pack +0xec, f = v / vmax_u. Clips 2 / 0x42 / 0x43 get rate f × `0x372ae0` (f ≤ 0: 1.0 in
   0x14, else 0) (visual).
2. Turn budget **B = R, × f in states 0x12 and 0x14**. `UseSecondaryFacing` (+0xd2): a second budget B2 =
   clamp(|angle| / `SecondaryTurnAngle` (+0x1674) × f, `MinSecondaryTurnROTCoefficient` (+0x1678),
   `MaxSecondaryTurnROTCoefficient` (+0x167c)) × R for the secondary facing (§3.5 item 3).
3. Formation jiggle `0x42c270` (§3.5 item 6). Distance budget s = v. Then the loop, items 4–11 (v = 0 → return):
4. **Nothing to walk** (P null, P on its last waypoint `0x5c5910`: idx + 1 ≥ count, or P not started): Q = the
   path below (`0x556960`). Q null, Q not started, or state 0x14 → state := 0x14 and **coast**: pos += s ×
   unit(pack +0x170 − +0x1a0, +0x180 − +0x1b0, 0) (*s78n t26 RE 2, replaces an [M]* [H]: the object service
   `0x3a6b80`, before the behaviours, copies pack +0x164..+0x190 to +0x194..+0x1c0 and then the object matrix obj
   +0x248..+0x274 to +0x164..+0x190; the matrix translation is the position +0x78 / +0x7c / +0x80, rebuilt by
   `0x3ac530` when dirty at the end of the object service / GOM flush. So the vector is **P(end of frame N−1) −
   P(end of frame N−2)**, the whole previous frame's 2-D displacement (z := 0), fast-normalised by `0x16f570`;
   commits earlier in the current frame do not change it; a zero vector stays zero, so the unit does not move but
   the point is still tested and committed. [edge] the snapshot is skipped when the service's param_3 ≠ 0 or (obj
   +0x3a0 & 8 and type +0x69)); commit when P is
   forced (+0x5c) or the point passes `0x50e280`, else report to the collision system (`0x509090`, TerrainCollision,
   §5.4.7) with no commit; return. Otherwise pop P (`0x557b10`), go on with Q, and **turn no more in this call**.
5. **Line crossing:** L0, n = line(idx+1). n·(pos − L0) ≥ −0.001 → idx += 1 (`0x5c4920`: +0x64 := 0, +0x6c := 0),
   back to 4. A waypoint is passed only by crossing its line, never by reaching its point.
6. **Turn** (when budget is left and nothing was popped): toward the wanted yaw by min(budget, |δ|) (`0x61e730`, the
   last step lands exactly); budget := the leftover, which carries over to the next waypoint of this call; leftover
   > 0 → P +0x6c := 1 (turn done); pack +0x2b8 := (R − leftover) / R (the 0x13 clip rate). The secondary facing
   likewise with B2 (`0x61e840`).
7. **Travel direction** t = vf +0x288 (`0x61e0f0`, §3.5 table: `Movement_Directions_Count` 1 (default) = the new
   facing; only `Gargantuan_Battle_Platform` sets 2), normalised. So the unit turns first and then moves along its
   facing, not toward the waypoint.
8. Degenerate segment: with d normalised exactly, |n.x·d.y − n.y·d.x| = 1.0 (the segment lies along the line) or
   d·n = 0 → idx += 1, back to 4.
9. **Distance to the line:** X = the point of line(idx+1) on the line through pos along d (μ = cross(L0 − pos, d) /
   (d·n), X = L0 + μ·(−n.y, n.x)); D = |pos − X| (2-D). τ = t·d (fast-normalised d); **τ = 0 → idx += 1** (quirk: a
   unit facing at right angles to its segment skips that waypoint), back to 4. **s' = |D / τ|**: the distance along t
   that covers D along d.
10. pos += t × min(s, s'). Not forced and `0x50e280` fails → report `0x509090`, no commit, return.
11. s ≤ s' (the budget ends before the line): commit (vf +0x2f8); rest = (s' − s) + `0x54f700`(wp, idx + 1) (the
    3-D lengths of the segments after wp[idx+1]); **v² / (2a) > rest → state 0x14 (End Move)** unless a started path
    lies below (then it walks on into that path without braking); return. s > s': idx += 1, s −= s', commit, back
    to 4 (several waypoints per frame).

**D. Corner-speed pass `0x5070b0`** (run by the Walk path set vf +0x190 `0x42c680`; the land team container's path
set `0x5ad140` too): only in land mode and only for a **formation path** (kind 1: the source path's +0x30, the
record's +0x48) of a unit in a formation.
Its input polyline is the unit's lane (`0x413340`: the formation path offset by the unit's slot).
*s78n t26 RE 2 (replaces "[M: one polyline point per path waypoint]") [H unless tagged]:* `0x413340`(F, formation
path, unit, out) writes **one point per formation-path waypoint**. Point i: an origin on wp[i] with a direction n
(i = 0: unit(wp1 − wp0), fast then exact normalise; i > 0: the waypoint line of `0x5c5060`(path, i − 1) = the
bisector at waypoint i; [L partly read] when the waypoint's arc length < |path +0x68| and the kind is 1 the point is
shifted along the segment by (len + +0x68)). Row k = min(int(s_i / 30), rows − 1), s_i = the arc length to i
(`0x54f870`), 30 = the step `0x140b2f0f4` (REMAINING_GAPS). Lateral offset L = F +0xb0[k].+4 + the unit's slot
offset (`0x556710`(pack, F +0xe0[k]) → +4); point = origin + L × (−n.y, n.x); while it is not passable for the unit
(`0x50e0c0`(grid, unit, point)) L −= L0 / 5, and once |L| ≤ |L0 / 5| L = 0. **z = 0** for every lane point. Every other path keeps the default entries (1.0, 1.0) (Path Around: (1.0, 0.0) on
waypoint 0). Values: vmax = `0x370f00`, vmin = `0x3728f0` (`Min_Speed` × mode), ω = R × π / 180 (radians per frame),
f_min = `Min_Speed_Fraction_For_Turn` (+0x3ac, default 0; FoC land types: `AT_AT_Walker`, `AT_ST_Walker`,
`Veers_AT_AT_Walker`, `TaunTaun`, `Destroyer_Droid`, `Scavenger_Droid` 0.1; `AT_AA_Walker`, `SPMAT_Walker` 0.25;
`Pod_Walker`, `Hutt_Pod_Walker` 0.4; `Scout_Trooper` 0).
1. **Splice:** when the unit's current top path is started and has ≥ 2 waypoints, its next waypoint (idx + 1) is
   appended to the polyline and to the new path for the computation (removed again in item 6).
2. For each corner j (j = 0 is the unit's own position): d_in = the facing (cos yaw, sin yaw) at j = 0, else
   unit(P_j − P_{j−1}); the leg goes to P_i, i = j + 1: L = |P_i − P_j|, d = unit(P_i − P_j), θ = acos(clamp(d_in·d)),
   side = sign(d_in × d).
3. **Merge (only when f_min > 0):** while L < L_turn = **2·vmax·sin|θ| / ω** (*corrected s78n t26 RE 2: this read
   2·vmax·θ / ω; the code takes sinf of the angle*: the chord of the full-speed turn arc) and a next point exists (the spliced path's points included): stop when the next point P_{i+1} lies on the
   other side (side × (d_in × unit(P_{i+1} − P_j)) < 0); else accept it: i += 1 and L, d, θ from P_j to it. (The
   loop's "θ > π" exit is fabs(acos) > 3.1415927 and can never fire; the spliced next point is taken from the started
   top path's waypoints, +0x18.) Then, still only when f_min > 0, if L > L_turn: the aim point A = B + d ×
   max(0, |P_i − B| − (L − L_turn)), B = the point accepted before P_i (P_j when nothing was merged), and L, θ are
   recomputed for A. Otherwise (and always when f_min = 0) A = P_i.
4. **Corner speed:** r = clamp(L / 2, 20, 100) (= max(20, min(L / 2, 100))); v = min(vmax, ω·r / (1 − cos θ),
   **L·ω / (2·sin|θ|)**); θ = 0 → vmax. (*Corrected s78n t26 RE 2: the third term read L·ω / (2|θ|); the code takes
   sinf.* Constants confirmed: 2.0, 0.5, 20.0, 100.0, 1.0. The third term is the speed whose arc v / ω has the chord
   L over a 2|θ| turn.)
5. **Probe** (while v > 0): the arc probe `0x505870`(unit, v, ω, signed θ (negative when d_in × d > 0), P_j, A); every
   probe segment must pass the static line test `0x5a4c10` (with type +0x1688). All pass and
   (v < 0.2·vmax and j ≠ 0, or the corridor test `0x5093c0` passes) → accept.
   *s78n t26 RE 2 (replaces "[M: its geometry]") [H]:*
   - Probe `0x505870`(unit, out, v, ω, θs, P_j, A): seg = max(2.0, 10 × `0x397830`(unit)); `0x397830` = for a TEAM
     container max(hx, hy) / 20, else `0x373b50` = `Scale_Factor` × 2R / 20, so for a single unit **seg = max(2,
     Scale × R)**, R = `Custom_Soft_Footprint_Radius`, else `Space_Obstacle_Radius`, else the larger model extent.
     N = (uint)(|θs| × v / ω / seg + 3). The output starts with P_j. t = fast-unit(A − P_j); θs ≥ 0 → n = (−t.y,
     t.x), s = +1; else n = (t.y, −t.x), s = −1. For k = 1 … N, u = k / N: **p_k = P_j + t·(sin|θ| − sin((1 −
     2u)|θ|))·(v / ω) + n·(cos((2u − 1)|θ|) − cos|θ|)·(v / ω)**: a circle of radius v / ω through P_j, leaving it
     along t turned by |θ| toward n (= d_in), total turn 2|θ|, chord along t. It stops before p_k when cross(A −
     last, p_k − last) × s < 0 (p_k has passed the aim line); then A is appended.
   - The static line test is `0x5a4c10`(pathfinder singleton `0x140a28800`, p_k, p_k+1, mask = type +0x1688 =
     **`MovementClass`**).
   - Corridor `0x5093c0`(probe polyline, unit): for each probe segment an oriented rectangle (centre = midpoint,
     half length |seg| / 2, half width 0, exact sqrt normalisation) queried with `0x538200` (layer 0x10, the unit's
     own id ignored, §5.3.2); any record with Static flag 2 → fail. It tests the probe polyline, not the lane. Otherwise v /= 1.5; when v < vmin or
   v < 0.1·vmax: v = vmin if the last static test passed, else 0, and accept.
6. f = v / vmax; **f < f_min → 0** (stop and turn in place at that corner, item A; the merge is then dropped: only
   the unmerged leg's start gets it). The entry (1.0 = ω/ω, f) is written into waypoints j … i − 1 and the next
   corner is i. The last waypoint gets (1.0, 1.0). With a splice the appended point is removed again, the spliced
   path's current waypoint takes the fraction of the new path's last corner and its +0x6c is cleared.
7. **Brake pass** (a > 0, vmax ≠ 0, at least 3 waypoints): v = 0 at the end; for k = count − 2 down to 0: v =
   sqrt(2·a·|seg k| + v²) (2-D: the segment from `0x5c5850` uses x and y), wp[k].+4 = min(wp[k].+4, v / vmax),
   v = vmax × wp[k].+4.
8. *Splice and last waypoint (s78n t26 RE 2) [H]:* without a splice the last waypoint gets (1, 1) through
   `0x5c5d90`; with one, the spliced path's current waypoint takes the fraction of the new path's waypoint count − 2
   and its +0x6c := 0. A fraction f < f_min writes 0 and the range reverts to the unmerged index.

*3-D and 2-D lengths (s78n t26 RE 2) [H unless tagged].* `0x54f610` (whole path) and `0x54f700` (rest after a
waypoint) sum **3-D** segment lengths (sqrt of dx² + dy² + dz²) over the stored waypoint xyz. The brake pass, the
lane, the probe, the move step's D / s' and the corner pass's L / d (from the lane, z = 0) are 2-D or work on z = 0
points. Path Around vertices are created with **z = 0** (obstacle corners, `0x675240`), and the pathfinder copies each
A* node's xyz into the waypoint (`0x674870`). So where every waypoint has z = 0 the 3-D length is **exactly** the
2-D one; a waypoint that carries a terrain height (the start / goal points taken from positions; [L] unread for the
grid pathfinder `0x5a2200` … `0x5a49c0` and for the start / goal vertices) makes the 3-D length ≥ the 2-D one by the
height differences. A port that sums 3-D lengths over the same stored points matches in both cases; a 2-D sum is
exact only for z = 0 paths and otherwise an approximation that is short on slopes.

**F. `Walk_Transition` states (s78n t26 RE 2) [H unless tagged].** Acceleration and deceleration: §3.4.
- **0x12** (`0x61ff40`), started path: the current clip ≠ 16 → play 16 (MOVESTART; variant index 0xFFFF = random
  draw, blend `0x140b39904`, rate `0x372ae0` = V / S, 1.0 when S = 0), +0xec := 0, the move step in 0x12, return.
  Clip 16 playing → +0xf4 := max(0, cur + step − (count − 2)); not ended (`0x39a180`) → the move step in 0x12; ended
  → state 0x13 **in the same frame**. Path not started: no transition → 0x15; with it → 0x14 when the current clip
  is 16, else 0x15.
- **0x13** (`0x61f840`): a transition unit starts the MOVE clip at frame int(+0xf4) (visual).
- **0x14** (`0x61f510`): the current clip MOVE (2) or WALKMOVE (0x42): k = the number of present end clips 17–20
  (`0x61d220`); end clip = 17 + (int(cur / (n_move / k)) % k), switched only when the phase ≤ the clip step, then
  +0xf4 := the phase; k = 0 → the default idle type. The current clip 16 → 17. When the end clip is current and has
  ended → 0x15 (no speed test). Else it is played (rate V / S, start frame rate + +0xf4) and +0xf0 := +0xec is
  latched. The move step runs every frame; B.7 holds T = v while the clip is 2 / 0x42.

**G. Starting a new path while moving (s78n t26 RE 2) [H unless tagged].**
- The plan member loop (§3.2.1 item 2): the stack reset `0x554480` removes every record; the path set vf +0x190
  (`0x42c680`: the lane `0x413340` only on land with kind 1 and the unit in a formation, then the corner pass D,
  push `0x554310`, which does **not** set +0x6d). FoC `ShouldSkipLandFormup` = true, so a land formation takes the
  one-member branch with no vf +0x148 (type 1). Start delay: speed +0xec == 0, obj +0x348 == −1 and block flag 8 →
  P +0x54 := synced rand(0, GC `0x140b16d38`) + frame; the Walk service returns early while the top path +0x54 >
  frame.
- `0x410170` ("aligned") → P +0x6d := 1; **the locomotor state is not changed.** It needs a top path and wp[0].+4 > 0;
  obj +0x348 slot ≠ −1 → aligned; states 0xf–0x16 or 0x21–0x23: the path has ≥ 2 points (`0x5c5850`), w = vf +0x1f0
  (Walk `0x61dff0` → `0x61d890` / `0x61ca90`, the wanted yaw for path index 0 from pos +0x78 / yaw +0x84), δ =
  `0x20b100` / `0x20b6d0` against obj +0x84..+0x8c, aligned when |δ| ≤ R or |δ| ≤ CE; state 0x24 → aligned; else not.
- **Aligned moving unit (0x12–0x14):** keeps its state and speed; the next frame continues in 0x13 with no start test
  A and no turn in place. The turn happens in the move step (C.6) and the corner cap B.4 uses wp[0].+4 (D's j = 0
  corner with d_in = the facing).
- **Not aligned:** in 0x13 the move step C.1 sees P not started → 0x14, T = a, coast (C.4). Then 0x15 (`0x61fc30`):
  no started path → idle clip, state 0x16, the frame ends. The Walk 0x16 handler: a top path not started → +0x6d :=
  1, state 0 in the same frame → start test A (a started path → 0x15). State 0 with an unstarted path → 0x16 and
  the frame ends.
- The formation update state 1 (`0x423760`) also calls vf +0x1a0 (can start) for each member, then vf +0x148.
  Generic can-start `0x42ccb0`: a top path and (not started, or state ∈ {0x16, 0x19, 0x25}); Walk `0x61e380`: 0x18
  → false, 0x19 with the flag clear and M > 0 → generic, other 0x19 → false. The plain start `0x42ced0` acts only when
  vf +0x150 reports idle (`0x42c890`: 0, 0x16, 0x1c, 0x1e, 0x1f, 0x25–0x27, and 0x19 when M > 0).

So with f_min = 0 (most units) each corner is slowed separately to v and never forced to 0 (a fraction of 0 comes
only from failed probes); with f_min > 0 sharp corners on short legs are merged, and a corner slower than f_min ×
vmax becomes a stop-and-turn. The first float is ω/ω = 1.0 (this resolves §5.4.7 item 6); no code path writes
another value.

**E. The land team container** (`0x5adfb0`, FIGHTER_FLYING §7.7) runs item C with these differences: budget B = R
(× 2, `Fleeing_Infantry_Speed_Bonus` `0x140b16d0c`, on a crush-flee path) with **no f scaling** and no secondary
facing; wanted yaw `0x5ab840` (segment direction + lane term, no collision vector); the yaw is set with pitch and
roll 0; the coast of item 4 and the travel of item 7 are always along the facing (cos yaw, sin yaw); at each waypoint
stepped in item 11 the slots are re-laid along the new segment (`0x5ac120`, or along the path below after a pop); the
speed step is `0x5aaf70` (FIGHTER_FLYING §7.7 items 1–5). Items 5, 8–11 (line crossing, the quirks, s', passability,
the End Move test, the pop onto a started path) are the same.

**Port: what must change** (read only): `walk_step` (`engine/tactical/movement/src/locomotor.cpp`) and the container
walk (`land_team.cpp`):
- the step moves toward the waypoint **point** and passes it on reaching it (the line only when steered), turns
  after moving, and snaps to the last point; retail turns first (budget R, × f in 0x12 / 0x14), moves along the
  facing t to the waypoint line by s' = |D / τ|, passes every waypoint by its line (−0.001), and on the last
  waypoint coasts along its last displacement in End Move (arrival within 5, §3.5.1);
- the speed target: retail's item B (target a while braking, the corner cap only while not aligned, the brake-ahead
  to the next waypoint's fraction, End Move when v² / (2a) > rest) replaces `sqrt(2·decel·remaining)`;
- the start test needs wp[idx].+4 > 0 for the CE shortcut;
- the land path record needs per-waypoint fractions, filled by item D for formation paths (with the probes and the
  brake pass); `land_team.cpp`'s note "the port's land paths carry no per-waypoint fractions" then goes.

### 3.6 Terrain conform and commit (vf95 `0x42d6a0` → `0x42bb40`) [H]

- z = terrain height at (x, y). If the movement mask contains Water, z = max(terrain, water plane): hover units ride the water.
- If `Overall_Length` (+0x1a20) × scale > 0:
  - sample the ground at ±L/2 along the yaw, shortened by a passability ray (`0x5a4c10`);
  - pitch = atan of the height difference;
  - z += |Δh| / 5.
- Roll is computed the same way with `Overall_Width` (+0x1a24) at yaw + 90°.
- Raised hover adds `Hover_Height`.
- The position setter stores the position and marks the object dirty.

### 3.7 Infantry teams [H]

- **Container:** a squad is a LAND_TEAM_CONTAINER object (e.g. Stormtrooper_Team: Max_Speed 0.8, Max_Rate_Of_Turn 12). Its speed is the minimum of its members (§3.4).
  Its acceleration and deceleration are **not** §3.4's rule on its own type: both are the minimum over its members of
  their type's `OverrideDeceleration` rule (0x5ab540), and its target speed is capped while a member trails more than
  HX + 2 behind it (0x5ab640); speed step 0x5aaf70: FIGHTER_FLYING §7.7 (s78n t14 retail run, 2026-11-10).
  The cap counts every member in the team list with no exclusion, is measured against the container (not the
  slot), goes down to 0, applies in the moving state only (not while braking) and is safe because a member's step
  is never passability-tested: §7.7 items 2-5 and §7.3 "Recovery in full" there. In collision detection and the
  dynamic repath a squad exists only as its container (members are never candidates): FIGHTER_FLYING §7.8.
- **Members** (LandTeamInfantry service `0x62ca10`):
  - copy the container's walk states 0x10/0x13/0x14 into their own data pack, and recompute idle states (`0x62bbf0`);
    what each copy drives, and the per-member start delay (0..`Max_Move_Frame_Delay` frames, synced draws): FIGHTER_FLYING §7.3;
  - steer to a slot by `0x62d620` (FIGHTER_FLYING §7). The slots are laid out by the container (`0x5ac120` →
    `ContainerArrangement` Rows `0x5ac970` / Dartboard `0x5ac2a0`) at its first service, at every move start, at every
    waypoint where the heading turns by more than acos 0.99, and after a SPREAD_OUT toggle: FIGHTER_FLYING §7.2;
  - add the collision steering vector `0x509810` from their **container's** collision objects; members are never
    collision checkers and their steps have no passability test: FIGHTER_FLYING §7.5.
- **Recovery:** when the team is idle and a member is farther than the threshold `0x140a4431c` from its slot, the member enters Recovery Turning (0x29) or Recovery Moving (0x2A) (decision rules: FIGHTER_FLYING §7.3 item 6). The recovery turn rate is multiplied by the fleeing bonus.
- **Fleeing** (s78n t14) = the container's current path is kind 7, the crush-flee push from a hostile crusher; it
  doubles (`Fleeing_Infantry_Speed_Bonus` 2) the container's and the members' speed and turn rate: FIGHTER_FLYING §7.1.
- **Garrison:** the container itself is garrisoned; it sits at the host's position while inside and leaves from the
  exit bone with a move to `Garrison_Exit_Dist`: FIGHTER_FLYING §7.4. Jetpack / multi-locomotor squads: §7.6 there.

### 3.8 Layer_Z_Adjust (+0xdfc) [M]

- **Land:** not a walker field.
- **Galactic:** fleet moves add it to the goal z (`0x39f580`).
- **Flying/legacy starship locomotors:** use it as the cruise altitude, or a floor for it (`0x622b80`).
- **Space formations:** use it as the slot z (§4.3).

### 3.9 XML fields with no runtime reader found

- `Walker_Acceleration_Magic_Number` (+0x1d50): parsed (`0x36c000`) and zeroed (`0x31b590`) only.
- `MaxObstacleCostSpace`, `ObstacleAreaOverlapForMaxSpace`, `UseLinearCollisionChecks`, `MaxObstacleCostLand`.
- `MaxLandFormationFormupFrames`, `Formation_Formup_Wait_Style`: not found in the formation code (§4.4).
- `RepushDistance` (GameConstants +0x1494) and `MinimumPushReturnDistance` (type +0x3cc): no reader (a1k t03, §5.4).

Any of these may have readers that were not found. Parse them and treat them as inert until a probe shows otherwise.

---

## 4. Formations

### 4.1 Objects [H]

`MovementCoordinatorSystemClass` (mode vf +0x228) owns a vector of coordinators:
- `LandMovementCoordinatorClass` (ctor `0x438ad0`) in land;
- `SpaceMovementCoordinatorClass` (`0x5cf3b0`) in space.

Both are 0xb8 bytes and are created per order by `0x426010`, appended in creation order. A coordinator owns a vector of `FormationClass` objects (0x290 bytes, ctor `0x40d0d0`, made by `0x4f6fb0`).
- Each formation gets a global incrementing ID (`0x140a2bcf0`, stored at +0x288).
- The ctor draws one synced `int(1, FPS)` (+0x1d8 := FPS + r + now, the next-check timer that 0x4192b0 / 0x410a60 gate on) [H, CU8G_T02_ENV_PICK_RE §7]. So every formation creation is one synced draw, including the Load_Map-tail GOM post-load pass (0x2b4440 → 0x3952e0 → 0x39bcb0), which gives each movable object without a coordinator a new one.
- The ctor has one caller, the factory `0x4f6fb0`, which has five callers: the land and space composition passes, `Issue_Degenerate_Move`, `Split_From_Formation` and the coordinator load. **A group move creates its formations at the next coordinator update, not at the order** (§4.6, cu8j 2026-11-08).
- Members are added by `0x40e0f0` into the vector at +0x10. This writes the formation pointer into the member's locomotor data (+0x1f0).
- Coordinators, formations and members are all **vectors in insertion order**.

### 4.2 Splitting a selection into formations (land; coordinator vf12 `0x43e5b0` → `0x43c1b0`) [H]

**Step 1: spatial buckets** (k-means, `0x43aac0`).
- k = floor(Σ over members of (4·halfX·halfY) / 400 / **Max_Formation_Area (14)**) + 1. The 400 is 20², i.e. the area is measured in cells.
- If k = 1 or k > member count, there is a single bucket.
- Initial assignment is round-robin: member i goes to bucket i mod k.
- Lloyd iterations follow:
  - centroids are summed in member order;
  - each member moves to the nearest centroid by strict "<", so ties keep the lower bucket index;
  - iteration stops when nothing moves or after the cap at `0x140a28bc8` (value not read).

**Step 2: formations.** Inside each bucket, repeat until the bucket is empty:
1. **Seed.** The remaining member nearest the bucket centroid (`0x4f7060`); ties keep the earlier one in the vector.
2. **Destination.** The clamped order point P (`0x583150`: the block's goal point, for an object goal the object's position, clamped to the playable box), **validated against the seed's zone** by `0x50f480`(P, the seed's position, the seed type's OccupationStyle, its MovementClass, 30) unless flag 0x20 is set. The rule is §4.3.1 item 7: P is kept when its cell is in the same zone as the seed's cell; otherwise it becomes the centre of the nearest-to-the-seed cell of the seed's zone on the first ring around P that has one. *(Corrected s78n t20: this said "validated as a passable cell"; the test is zone equality, not passability, and the result is used only for position goals.)* Locomotors that answer vf +0x240 use the position finder at `0x1409cf220` instead.
   - Validation fails (no such cell within 29 rings): the composition pass **returns at once**. Formations already made in this pass stay; this seed and every member not yet placed get no formation for this order, and the destination check of §4.3.1 is not run [H for the return, M for the effect].
   - Use: for a **position goal (block kind 1) without flag 0x20** whose validated point V differs from P, the formation's block is a fresh kind-1 block at V carrying only the order's flags word (+0x48); the order's final facing is not carried over (block +0x5c = 0) [H]. In every other case (V = P, an object goal, flag 0x20) the block is a copy of the order: for an object goal the validated point is thrown away.
3. **Stagger** (`PathFrameDelay`, synced random), see §1.6.
4. **Recruitment.** Only when the seed's `FormationGrouping` (+0x16a4) is not Solo. A candidate from the same bucket joins when all of these hold:
   - the seed is not an engaged short-range attacker;
   - same locomotor kind (vf +0x240 and vf +0x238 agree);
   - the grouping rule (enum `0x5de0f0`: Solo 0, SameOrder 1, Standard 2) passes:
     - Standard recruits only Standard;
     - SameOrder also needs an equal `FormationOrder` (+0x16a0);
   - both the seed and the candidate are zone-connected to the destination (`0x50ee10`);
   - with the double-click "run" flag (0x80): max speed / min speed ≤ **DoubleClickMoveMaxSpeedRatio = 1.25**;
   - the engagement distance is compatible;
   - the distance to the nearest current member is within the recruitment distance: **InfantryFormationRecruitmentDistance = 60** for UnitCollisionClass Infantry (5) that is not a team member, otherwise **VehicleFormationRecruitmentDistance = 100**;
   - a straight passable line exists (`0x5a4c10`), or a real path (`0x5a2830`) is ≤ 1.4 × the nearest-member distance.
5. **Teams.** Team members are split across formations only when ShouldInfantryTeamsSplitAcrossFormations (true). [M]
6. **Attack orders** (object goal without the guard flag; s78n t20: read from `0x43c1b0`, was [M]) [H]. With E_r = the seed's engagement range `0x4f6e40` (§4.3.2 item 1) and d = the 3-D distance seed to target: the seed goes **solo** (no recruitment) when it is already in max range of the target (TARGETING slot 44), or its type is melee (`0x372210`), or d < r_seed (`0x3969f0`) + E_r + **Solo_Attack_Range (GC +0x1548, 60)**. Otherwise "short-range" is set when d < E_r × **Short_Range_Attack_Formation_Coefficient (GC +0x1544, 1.6)**.

### 4.3 Shape and slot assignment (`0x41a740`, `0x41ece0`) [H]

**Size:**
- cell = max over members of (`FormationSpacing` (+0x16d4) + 2·radius/20).
- side = ceil(√n).
- half-extent in cells = max(side·cell + 0.25, √max(1, area × **Spread_Out_Spacing_Coefficient 1.8**)), where area comes from `0x41b4a0`.
- Width +0x254 and depth +0x250 are stored. The basic shape is a **filled box of rows, widest across the direction of travel**.

**Corridor squeeze** (`0x412210`) [M]:
- Lateral clearance (left/right) is sampled along the formation path at a fixed step (`0x140b2f0f4`, not read).
- A running minimum is taken over a look-ahead window.
- Runs shorter than 2.5 cells are smoothed out.
- The result is a vector of lateral shifts (+0xb8) that narrows the formation through chokepoints.
- Formation type +0x218 selects the sampler: 1 = `0x4138b0`, 2 = fixed width.

**Slot assignment order:**
1. **Rank the members.** Sort with `std::sort` (unstable) using comparator `0x41a080`:
   - members whose `CategoryMask` contains **Hero** come first;
   - then ascending max speed (`0x3749d0`), so the **slowest go in front**;
   - speeds within tolerance T rank as equal;
   - exactly equal speeds are ordered by a case-insensitive compare of the type name (type +0xf8);
   - T starts at 0 and grows by **15** (speed units) while there are ≥ 4 distinct ranks, up to a cap (`0x140a28b78`, not read).
2. **Build rows front to back.** A row is a run of equal rank.
   - Order the row's candidates by projection on the formation's forward axis, descending (`0x4207d0`; team members use their team leader's position).
   - Take candidates while their cumulative width ≤ 2 × half-width; the first always fits. Overflow returns to the front of the remaining list.
   - Re-sort the kept members by lateral projection (`0x420790`), so units keep their left/right order and never cross.
3. **Lateral placement.** gap = (2·half-width − Σ widths)/(count + 1). Member k is offset by (cumulative gaps + widths − half-width) × 20. The next row steps back by this row's largest width × 20.
4. **Store.**
   - Land: the member's **form-up point** (its slot in the form-up box at the path start, `0x414d90`) is stored as a world position in a second object-ID-keyed hash (formation +0x140…+0x170, `0x423ec0`). *(Corrected s78n t20: this said "quantised to the grid". Nothing is rounded: the box is kept in cell units, and the point is converted with world = cells × 20 − 10 as a float. `0x423ec0` only erases and re-inserts the hash entry.)*
   - Space: slot z = the member's `Layer_Z_Adjust`.
   - Offsets live in an **object-ID-keyed hash** (formation +0x108…+0x130; insert `0x423ae0`, lookup `0x41a8d0`). It is never iterated.
5. **Footprint check**: *(corrected s78n t19, 2026-11-10; the earlier text "`0x40eb90` → `0x43a070`, non-members
   overlap ≥ 15 %, retry once, then signal 10 and state 3" mixed three functions.)* `0x40eb90` is the **layout
   driver** and tests no destination; the destination check of a plain move is `0x439360` → `0x43d6a0`, run once
   at the order's composition pass; `0x43a070` is the attack-arc slot test. All three: §4.3.1.

*(Correction, s78n t19: the rank key `0x3749d0` in item 1 is the unit's **attack range** (TACTICAL_COMBAT §2: the
active ability's range, else `Targeting_Max_Attack_Distance` × the deployed / garrisoned multipliers), not its max
speed; `0x41a080` calls it for both members [H]. So rows are ordered heroes first, then **shortest range in
front**. The tolerance T is in range units.)*

#### 4.3.1 Destination check at the order, the layout driver and the arc slot test (s78n t19 RE, 2026-11-10) [H unless tagged]
Read from `0x40eb90`, `0x410e90`, `0x41c1a0`, `0x43c1b0`, `0x439360`, `0x43d6a0`, `0x43a7f0`, `0x438c10`,
`0x43d380`, `0x43b520`, `0x43a070`, `0x411640`.
1. **`0x40eb90`(F, mode, retry) lays the formation out; it checks nothing at the destination.** Callers: the plan
   `0x41c1a0` (after the path `0x417520` succeeded) and the guard path copy in `0x4192b0`. Steps: final facing
   `0x411980`; corridor squeeze `0x412210` (mode ≥ 2); the oriented form-up box `0x414d90` (at the path **start**);
   slot assignment `0x41ece0`; slot offsets copied to the members' packs `0x4252a0`. Its "retry" is `0x410e90`: for
   an object goal (block kind 3) the formation path is trimmed back in 10.0 steps to where the members can fire
   (guard: within `Land_Guard_Range`) and laid out again (in full: §4.3.2 item 7). "Signal 10 and state 3" happens only when `0x41ece0`
   returns 0 (a row pass consumed no member), which the first-candidate-always-fits rule makes practically
   unreachable [M]; if hit, the order ends as arrived and nobody moves. No member-count gate.
2. **When the destination check runs:** once, at the end of the land composition pass `0x43c1b0` (the coordinator
   update after the order, §4.6.2), on the split descriptors; never while moving and never at arrival. It is
   **skipped when the order has flag 0x20** (forced paths: garrison exit, unload, the garrison corrective move).
   Object goals (kind 3) go to `0x438c10` (item 5); every other kind to `0x439360`.
3. **`0x439360`** takes every descriptor whose first member's `Space_Layer` is Land (`0x397900`(m, 1) = 0x10).
   **No member-count gate: one squad container or one vehicle qualifies.** The descriptors are sorted nearest
   first to the group centroid C and the first, R, is test-planned (`0x41c1a0`(R, 1)); a path of fewer than 2 nodes
   ends the pass with nothing changed. Axis a = that path's last segment direction d1, or the order's facing when
   it has one; φ = the signed angle from the path's first to its last segment. Passes n = 0 … 40 over the
   **centre** Dn: D for n = 0, else D + 20·⌈n / 8⌉·(cos, sin)((n mod 8)·45°). **D = the goal point of R's own
   block** (`0x583150` on `0x41a4e0`(R)), i.e. for a position order the point **validated** for R's seed in §4.2
   step 2 (item 7), not the raw order point *(s78n t20; "the ordered goal" before)*. Pass 0 tests nothing about D
   itself. For n ≥ 1, Dn must be passable (`0x50e180`, R's formation class F +0x1c4), zone-connected to D
   (`0x50ee10`, item 7) and reachable by a straight line or a path ≤ 1.8 × the distance. Each
   formation i is placed by `0x43d6a0` with step direction v_i = normalise(rotate(centroid_i − C, φ)), or −d1 when
   it is the only formation or that offset is zero. Any formation failing → next n. After n = 40 one forced pass at
   D accepts everything. **The order is never refused.**
4. **`0x43d6a0`, one formation.** S = `BetweenFormationSpacing` (GC +0x14ec, 5), E = the destination query
   extension (GC +0x14f0, 60). (HX, HY) = the formation's half depth / half width in world units (the union of its
   slot rectangles, `0x411640`; for a lone squad container: its joint slot box). Rectangle R = centre, axes a / a⊥,
   half extents (HX + S, HY + S).
   - Up to 40 tries, t = 0 at first. Tries k with k mod 8 < 4: centre = Dn + t·v, then t += **7.0**; for the first
     formation, until impassable ground has been met, the centre is Dn + 0.5·t·v·(+1 for even k, −0.75 for odd k)
     (so 0, 2.6 beyond, 7 short, 7.9 beyond, … along the final direction). Tries k mod 8 = 4 … 7: Dn + (t − 28)·v
     (halved likewise) + min(HX + S, HY + S)·{(1, 0), (0, 1), (−1, 0), (0, −1)} (world axes).
   - A try is rejected when: with several formations, R contains Dn; R overlaps (`0x20d690`) a rectangle already
     taken in this order; the centre cell or R is impassable; after crossing impassable ground it is not
     zone-connected or the path is > 1.8 × straight; or the **object test** fails.
   - **Object test.** Candidates: the object tracker's objects (`0x539680`) in R grown by E. Skipped: the order's
     own objects; flying multi-locomotor objects (`0x509370`); non-container Infantry-class objects when the
     formation is all Infantry / Bike or all Giant and not all team containers. **No owner test and no moving
     test.** A candidate's rectangle is its footprint `0x38de80`, **moved to the end of its path with the final
     direction when it has a path** (`0x43a7f0`): other units count where they are going. It is a blocker when
     some member's slot rectangle (extents `0x396850` + S) overlaps it, the pair passes the UnitCollisionClass
     table (`0x505020`, §5.2 item 6) and the member cannot crush it (`0x5c8900`). **Reject iff Σ over blockers of
     hx·hy (their unexpanded half extents) > 0.15 × (HX + S)(HY + S)** (0.15 at `0x140865dc0`).
   - Accept: the formation's block goal := the centre (`0x423e70`), its rectangle is stored (formation +0x258) and
     appended to the taken list. The centre is used **as computed, unrounded** (s78n t20).
   - Ground state of the tries (s78n t20, [H]). "Met" is set by the first try whose centre cell or R is
     impassable; the next try on passable ground turns it into "crossed". Only while "crossed" is a try tested for
     zone connection **to Dn** and for the line / 1.8 × path from Dn. A try that fails after the ground tests
     (zone, path or object test) while "crossed", followed later by another impassable try, **ends the tries at
     once** (fail). Before "met", the first formation uses the halved, alternating offsets of the first bullet;
     from "met" on it uses the full step t.
   - Forced pass (the last argument set): the same 40 tries run and the first accepted one is taken as usual. Only
     when none is accepted the block goal := the centre of the **last try that reached the object test**, or Dn
     when no try got that far, and that rectangle goes to the taken list.
5. **`0x43a070`** is the attack-arc slot test of object goals (`0x438c10` → `0x43d380` → `0x43b520`, 5° steps,
   `Base_Land_Targeting_Arc_Angle_Coefficient`): a slot needs a passable point, zone connection and a clear line to
   the target, and passes iff Σ (hx + S)(hy + S) < 0.15·HX·HY over the objects in the rectangle (HX + E, HY + E)
   that are **not moving** (locomotor vf +0x1c0(0) false), the target and the order's own objects excluded.
   **In full: §4.3.2** (s78n t20). Note there: the "clear line" is the **terrain** line test to the target's
   position, so a target that stands on cells the formation cannot enter (a stamped structure) gets no slot.
6. **Single-member shortcut in the plan `0x41c1a0`:** block kind ≠ 3, flags without 0x10 and 0x20, and the 2-D
   distance to the goal < √(hx² + hy²) (`0x3969f0`): with one block the formation signals 10 (arrived), stops and
   goes to state 3; with more blocks the block is popped. (A unit ordered onto its own footprint does not move.)

7. **Zone validation `0x50f480`, zone connection `0x50ee10` and where the path ends (s78n t20 RE, 2026-11-10; read
   from `0x50f480`, `0x50ead0`, `0x50ecd0`, `0x50da90`, `0x50e180`, `0x50e280`, `0x43c1b0`, `0x439360`, `0x43d6a0`,
   `0x417520`, `0x414720`, `0x41c1a0`, `0x40e260`, `0x5a2e00`, `0x5a1910`, `0x5a3020`, `0x5a3ba0`, `0x5a49c0`,
   `0x41ece0`, `0x423ec0`) [H unless tagged].**
   - **Zones** (`0x50da90`, rebuilt with the regions): per OccupationStyle layer and per MovementClass every
     region gets a zone id: **0 when the region's passability bits fail the class mask**, else a component number
     ≥ 1 from a flood fill over adjacent passable regions [M: that the fill graph is the region adjacency of
     §1.4]. A cell's zone is its region's; "none" (−1) when the cell is outside the grid, in the uncovered last
     row / column, the class is −1, the mode is not land, or the **style is not 0**.
   - **`0x50ee10`(A, B, class, style)** "zone-connected": true outright when the mode is not land; otherwise true
     iff both cells have regions, **both regions pass the class mask** and their zone ids are equal. An impassable
     end point is never zone-connected to anything.
   - **`0x50f480`(P, Q, style, class, 30, out)**, cells by trunc((x + 10) / 20):
     1. zone(Q) = none and Q's cell lies outside the grid, **or zone(Q) = zone(P)** → out := P unchanged, true.
        Passability is not looked at: with Q on impassable ground (zone 0) an impassable P is kept; with style ≠
        0 both are "none" and P is always kept.
     2. Otherwise rings r = 1 … 29 (the 30 is an exclusive bound in **cells**: up to 580 world units) of the
        square around P's cell. Candidate order within a ring: for i = −r … r the cell (px + i, py + r) and then
        its mirror (px − i, py − r); then for j = 1 − r … r − 1 the cell (px + r, py + j) and then its mirror
        (px − r, py − j). A candidate is kept when it is inside the grid and **its zone id equals zone(Q)**. As
        decompiled, a mirror cell is only looked at when the first cell of its pair is inside the grid and has a
        region.
     3. The first ring with at least one kept candidate decides: out := the centre (cell × 20, z = 0) of the
        kept cell **nearest to Q** (straight distance to the cell centre; strict <, so the earliest candidate
        wins a tie), true. No later ring is looked at even when it holds a nearer cell.
     4. No ring has one → false, out untouched.
     5. Quirk [H as read, M for the effect]: with Q on impassable ground and P passable, step 2 looks for cells
        of zone 0, i.e. the result is an **impassable** cell near P.
   - **Callers.** (a) Composition, §4.2 step 2: Q = the seed's position, class / style of the seed's type. (b)
     The formation path `0x417520`, every (re)plan: goal G = the block's goal point for the lead (`0x5833e0`; for
     an object goal the object's position, or its hardpoint / stored point) after staging `0x40e260` (which only
     moves G when a member stands inside its minimum range of an object goal); when G is not zone-connected to
     the lead for the formation's class F +0x1c4: G := `0x50f480`(G, the lead's position, style 0, that class, 30),
     and the plan fails when that fails. So **a path to an object goal on impassable ground (a structure) ends
     on the reachable cell next to it that is nearest to the lead**, before any trimming (§4.3.2). (c) `0x414720`
     picks F +0x1c4 at every plan: the class with the most mask bits among those whose mask is contained in the
     intersection of all members' masks (`0x4779e0`); when the goal is zone-connected to the lead for it (or
     validates), and is also zone-connected for the best class without the Shield bit 0x40 (`0x477b00`), that
     class instead.
   - **What reaches the destination check.** D (item 3) is R's validated point, so it is normally passable and
     in R's zone, and the check runs as written. D can still be impassable only when R's seed itself stands on
     impassable ground (zone 0 = zone 0), when the seed's OccupationStyle is not 0, or when the seed is outside
     the grid. Then: every try of pass 0 fails (the centre is impassable, or after "crossed" it is not
     zone-connected to D), passes 1 … 40 fail their own zone test, and the forced pass, finding no try that
     reached the object test, **keeps D**. The check changes nothing for an impassable D.
   - **Test plan to an impassable goal** (`0x41c1a0`(R, 1) → `0x417520`): the lead on passable ground → G is
     re-validated as in (b) and the path has ≥ 2 nodes. The lead on impassable ground too → the region search
     `0x5a3020` fails unless start and goal share one region (it needs the **start region** to pass the class mask
     and never enters an impassable region), the path is left empty and the destination check ends with nothing
     changed. The single-member shortcut of item 6 (a unit ordered onto its own footprint) returns before any
     path as well.
   - **Where the formation path ends.** The request goal is G as a float point; `0x5a1910` only derives the start
     and goal **cell indices** from the request (goal cell = trunc((G + 10) / 20); the start cell likewise, or
     for an object-less / negative-turn-rate request the best-aligned of the up to four cells around the start).
     The same-region builder `0x5a3ba0` emits start, start cell centre, goal cell centre, **exact G**; the
     region-path builder `0x5a0550` appends the exact goal too (§1.4); the visibility pass `0x5a49c0` always
     keeps the last point. So **the path ends exactly on the block goal** (the
     accepted centre of item 4, the validated point, or the ring cell centre of step 3), not on a cell centre
     [H for the request and the raw point list, M that the shortcut pass `0x5a4130` never moves the last point].
     Slot offsets (hash +0x108, `0x423ae0`) are float world offsets (depth, lateral); nothing in `0x41ece0`
     rounds a slot to a cell.

***Port spec (zone validation):*** before the destination check, replace a position goal P that is not in the
lead's zone (impassable for its class, or cut off) by the cell centre of step 3 above: nearest to the unit among
the cells of the unit's zone on the first square ring (1 … 29 cells) around P that has any. Run the destination
check from that point. Do the same in the path builder for an object goal's point. Do not round an accepted
centre or a member slot to a cell.

***Port spec (destination check):*** at a land move order without the forced flag and without an object goal, for
every formation of the order (a lone squad or single vehicle included) whose lead is on the Land layer, test the
goal as in items 3-4 and **move the goal** to the first accepted centre; never refuse the order and never end it
early. A squad ordered onto a stopped unit therefore gets a displaced goal at order time (first 2.6 beyond, 7
short and 7.9 beyond along its final direction, then min(HX, HY) + 5 sideways on the world axes, then farther
back). Units count at their **destinations** when they have a path. The collision system's early-arrival cut
(§5.4.4) only handles what this pass lets through: blockers inside the 15 % allowance, units that stop there later,
forced-path moves, attack orders and non-Land formations. Nothing runs at arrival: a squad or unit simply ends its
path where the (possibly cut) path ends.

*Not read:* block +0x5f (set by `0x417520`; the object test then never rejects
and only records a per-member flag via `0x423fe0`) [L]; whether taken rectangles of a failed pass n are cleared
before pass n + 1 [M]; the rectangle passability test `0x17dc70`; the position finder `0x440f90` used for vf +0x240
locomotors and forced paths; `0x5a0550` beyond its last two points.

**Port note:** the rank sort is unstable. Use a stable sort with a final tie-break on object ID. Exact retail tie order cannot be reproduced without MSVC's introsort; ties are rare because of the name tie-break.

#### 4.3.2 Attack approach on land: arc slots, the slot leg and the fire trim (s78n t20 RE, 2026-11-10) [H unless tagged]
Read from `0x43c1b0`, `0x438c10`, `0x43d380`, `0x43b520`, `0x43a070`, `0x43b210`, `0x4f6e40`, `0x411640`,
`0x41fd30`, `0x41c1a0`, `0x417520`, `0x40e260`, `0x40eb90`, `0x410e90`, `0x40f7c0`, `0x40edd0`, `0x410a60`,
`0x5a4c10` / `0x5a50d0` / `0x5a5810` / `0x5a51f0`, `0x567350`, `0x568340`, `0x583150`, `0x5833e0`, `0x583930`.
B = the order's object block (kind 3), T = its object, C = T's goal point (`0x583150`: T's position, or its
hard point when the block names one). F = a formation; (HX, HY) = F +0x250 / +0x254, its half depth / half width
in world units, set at composition by `0x411640` (a mode-0 plan and layout, then the union of the slot
rectangles). S = `BetweenFormationSpacing` (GC +0x14ec, 5.0), E = `Destination_Collision_Query_Extension` (GC
+0x14f0, 60).

**Summary.** Three separate things happen, in this order:
- at the order (and at an auto-attack push) the formations that are not yet able to fire get a **polar slot**
  (angle, ring radius) around the target (items 1-4);
- at every plan the path is built to the target's point, and a formation with a slot gets a path **through its
  slot point and on to the target point** (item 6);
- the laid-out path is then **cut back to the first place from which every member can fire** (item 7). This
  cut, not the ring radius, is what stops a formation at weapon range.

1. **Engagement range `0x4f6e40`(unit, block).** r = the unit's attack range `0x3749d0`. With a pending ability
   whose own range (vf +0xd8) is > 0: min(r, that range), nothing added. Otherwise, when the block has an object:
   r += min(hx, hy) of the object's extents (`0x396850`) when the block names no hard point, else += `0x397780`
   [M: of which object; the argument is not visible]. Then for a guard block: min(r, `Space_Guard_Range` 750) in
   space, min(r, `Land_Guard_Range` 150, GC +0x11ec4) on land.
2. **`0x438c10`(B, formations, the order's units): who gets a slot, and when.** Callers (the only two): the land
   composition pass `0x43c1b0`, for an object goal without flag 0x20, on the new descriptors; and the block Push
   `0x41fd30` for a non-guard object block on a land formation (a one-descriptor copy of the formation; the slot
   comes back inside the pushed block). **It is not run at a re-plan.**
   - List L: every formation with **exactly one block**, a lead on the Land layer (`0x397900`(lead, 1) = 0x10)
     and, non-guard: **some member that is not in max range of T (TARGETING slot 44, `0x567350`) or is inside
     its min range (slot 45, `0x568340`)**; guard: its lead farther from C than min(the lead's attack range,
     `Land_Guard_Range`). A formation whose members can all fire already is left out and keeps the plain block.
   - L empty → return. R = the **minimum** of the engagement ranges (item 1) over all the order's units.
   - Reference formation = the one in L nearest (3-D, strict <, first wins) to the mean of L's centroids
     (`0x41a9b0`). It is test-planned (`0x41c1a0`(F, 1)); fewer than 2 path nodes, or a zero last segment →
     return with nothing written. d0 / d1 = the unit directions of that path's first / last segment (2-D).
   - L is sorted ascending by the projection of the formation centroid on d0 from the path start (`0x43b210`,
     `std::sort`): **rear-most first**.
   - Coefficient loop: f = GC +0x154c `Base_Land_Targeting_Arc_Angle_Coefficient` (**0.2**), then f − 0.2 each
     time while f > −1.0: in float32 exactly **six tries, f = 0.2, 0, −0.2, −0.4, −0.6, −0.8**. Each try runs
     `0x43d380`(L, B, R, d1, f) without committing; the first f that succeeds is run again with commit and the
     function returns. **No f succeeds → nothing is written: no slot, the blocks stay plain. The order is never
     refused here.**
3. **`0x43d380`: rings.** r_T = `0x3969f0`(T) = √(hx² + hy²). Lower radius limit R_min = max(r_T, R·f). Half
   arc A = (1 − f)·90°: **72°, 90°, 108°, 126°, 144°, 162°** for the six tries. Axis u = −d1 (from the target
   back along the approach). Outer radius ρ := R. Repeat while formations are left:
   - Find the longest prefix of the remaining list (rear-most first) that fits on one ring: for prefix length
     k = 1, 2, …: h = the largest HX in the prefix; ring radius ρ_k = ρ − h; stop extending when ρ − 2h < r_T or
     when `0x43b520`(prefix, ring ρ_k, A) fails.
   - No formation fits → the try fails. Otherwise (commit run) place that prefix on its ring, remove it, and
     ρ := ρ − 2h with **h of the last prefix tested** (the failing one when the extension stopped on a
     failure).
   - New ρ < R_min → stop. The try succeeds iff no formation is left.
   - So the rear-most formations take the outermost ring, whose far edge is at R: every unit of the order is
     then inside the shortest engagement range. Inner rings follow at 2h steps down to 0.2·R on the first try and
     down to the target's radius on the others.
4. **`0x43b520`: one ring.** The prefix is sorted ascending by centroid projection on the **right-hand normal of
   d0** (d0.y, −d0.x) (same comparator; left-most first), n formations. All angles are measured from u, counter-
   clockwise positive; a slot point is **P(θ) = C + ρ_k·rot(u, θ)**. step(a, b) = acos(√(ρ_k² − (a + b)²/4) / ρ_k),
   i.e. asin of half the chord a + b.
   - Middle formation m = index n >> 1. Its first angle θ = 0 for odd n, **2·step(HY_m, 0)** for even n (fail
     at once when that exceeds A). It is tried at θ, then alternately on both sides in **5°** steps (0, +5°, −5°,
     +10°, … ; from a non-zero start θ0: θ0, −θ0, θ0 + 5°, −(θ0 + 5°), …) while |θ| ≤ A, until the slot test
     (item 5) passes. None passes → fail.
   - Left neighbours, index m − 1 down to 0: the first one starts at θ_m − **2**·step(HY, HY_m); each further
     one at the previous accepted angle − **1**·step(HY, HY_prev) (as coded: the factor 2 is only on the first
     neighbour). A failed test moves that formation a further 5° outward (θ −= 5°); beyond −A → fail.
   - Right neighbours, index m + 1 up to n − 1: the mirror image from θ_m (first + 2·step, then + 1·step, + 5°
     on a failed test); beyond +A → fail.
   - **Commit, per formation:** its block 0 := a copy of B with **block +0x3c := θ (radians) and +0x40 := ρ_k**
     (`0x423e70`); F +0x258 := the slot rectangle of item 5. Nothing else is written (no position, no flag).
     There is no shared "taken" list: neighbours are separated only by the angular step.
5. **`0x43a070`(F, P): the slot test.** c = the formation's movement class (`0x4779e0`: the class with the most
   mask bits inside the intersection of the members' masks). All of:
   1. P's cell is passable for c (`0x50e280` with that mask).
   2. P is zone-connected (`0x50ee10`, §4.3.1 item 7) to F's lead (`0x41a9b0`).
   3. The **terrain line test `0x5a4c10` from P to T's position** passes for c: every cell the segment crosses,
      **the last one included**, must be enterable (same region as the start, else a cell walk for pieces under
      40 long). This is not a line-of-fire test. **A target whose own cell is not enterable for c, i.e. any
      structure or prop stamped under its origin (§1.3), fails this for every P, so it never gets slots**; a
      unit standing on open ground does.
   4. The object test. Rectangle: centre P, first axis a = unit(T.position − P), second (−a.y, a.x), half
      extents (HX, HY) (this rectangle is what the commit stores). Candidates: the tracker's objects
      (`0x539680`) in that rectangle grown by E on both axes. Skipped: T; every object of the order (the unit
      list); an object **with a path record on its stack** (locomotor vf +0x1c0(0) = `0x42cbd0`; so any unit
      that is under way, whatever its speed); a non-container Infantry-class object when the formation is all
      Infantry / Bike or all Giant and not all team containers. **An object without a locomotor always counts.**
      A candidate's rectangle is its footprint (`0x43a7f0`; no shift, since it has no path) with both half
      extents **+ S**. It is a blocker when some member's slot rectangle (centre P + a·depth + a⊥·lateral from
      the slot offsets, half extents `0x396850` + S) overlaps it (`0x20d690`, touching counts) and that member
      cannot crush it (`0x5c8900`). **No UnitCollisionClass table test here** (unlike §4.3.1 item 4).
      **Pass iff Σ over blockers of (hx + S)(hy + S) < 0.15 × HX × HY** (strict; the expanded blocker areas
      against the unexpanded formation rectangle, the reverse of §4.3.1 item 4).
6. **The plan with a slot (`0x41c1a0` → `0x417520`, every plan and re-plan).**
   - Before any path: with an object on the top block, no guard flag and **c true** (`0x40edd0`: every tested
     member can fire from where it stands; FIGHTER_FLYING §4.11 E) a real plan (mode ≥ 2) of a land formation
     **clears every member's path stack** (locomotor vf +0x138 with the current owner id: `0x42cfc0` →
     `0x554480`; the speed is not zeroed, the members brake through End Move) and returns success with no path
     [M: the formation then arrives at its next step]; a test plan returns "no path".
   - Goal G = the block's goal point for the lead (C). Staging `0x40e260` moves G only when the member with the
     largest minimum range is **inside** that range of T: then G := a spot at min + 0.2 × (attack range − min)
     from T on that member's side (position finder), or, for a one-member formation of a crusher against a
     crushable T, T's position. Then the zone re-validation of §4.3.1 item 7 (b): a G the lead cannot reach
     becomes the reachable cell next to it.
   - First path: start → G (start = the members' centroid when it is reachable from the lead by a straight line
     or a path ≤ 1.9 × the distance, else the lead's position).
   - **Slot leg, only when block +0x40 (ρ) > 0 and the first path has ≥ 2 nodes.** v = the unit direction of
     the first path's **own** last segment (3-D). Q = G + ρ·rot(−v, θ) (θ = block +0x3c; rotation about z).
     The terrain line test from G toward Q gives the distance q it gets (ρ when the line is clear) [M: its
     value when blocked at once]; slot point Q' = G + q·rot(−v, θ). A second path start → Q' is planned; when it
     succeeds and its first segment points the same way as the first path's (dot > 0), **G is appended to it
     and it replaces the formation path: start → … → Q' → G.** Otherwise the first path stays.
   - So the stored (θ, ρ) is re-applied to **this formation's own approach direction at each plan**, not to the
     reference formation's d1 used when the slot was tested; the two coincide for a formation that approaches
     along the reference path.
7. **The fire trim `0x410e90`** (the "retry" of the layout driver `0x40eb90`, run after the slots are laid
   out; on true and a path length > 0 the layout runs once more without it). Conditions: the top block is kind
   3 with an object, the formation is not a space formation, T is not fogged for the first member's owner and
   not stealthed (unless that member's type has `Target_Stealth_Units`); not for a guard block in space.
   - L = the path's length. s := L; "last good" g := L. Guard range = `Land_Guard_Range`, plus T's radius
     `0x397780` when T has the +0x346 slot [L: which behaviour]; in that case g starts at max(0, L − that range).
   - At each s, for every member that has TARGETING and a locomotor pack: its slot position p = the path point
     at arc length s + its depth offset (before the path start: the start extended backwards along the path
     direction; beyond L: the member is not tested at this s), shifted sideways by its lateral offset (with the
     corridor squeeze of §4.3). The member **fails** when: non-guard: p is inside its min
     range of T (TARGETING slot 46 at p) **or** p is not in max range of T (slot 43 at p; the rule of
     TACTICAL_COMBAT §2.8 B 4); guard: |p − T.position| (2-D) > the guard range; and, on land, in either case
     when its **line of fire** from p is blocked (`0x40f7c0` → `0x3b5ad0`: from p at terrain height, or water
     height for a Water-capable class, + the member type's `Ranged_Target_Z_Adjust` +0x4dc, to G raised by
     T's `Ranged_Target_Z_Adjust`).
   - Some member fails at s → **the path is cut to length g** (`0x54e780`, §5.4.4 item 5; the squeeze vector
     likewise) and the trim ends. All pass → g := s; s = 0 → the path is cut to its first node (the formation
     can fire from where it stands); else s := max(0, s − **10.0**) and repeat.
   - So the path is cut at the **outermost point, in 10.0 steps counted back from the path end, of the last
     unbroken stretch from which every member can fire**. When some member cannot fire even from the path end,
     nothing is cut.
8. **After the plan.** While the formation moves, `0x4192b0` every 30 frames: the land stop when everyone can
   fire, the re-path when the target moved more than half the base attack range or the formation is stopping
   short (FIGHTER_FLYING §4.11 E). After arrival, `0x410a60` every 30 frames: any tested member unable to fire →
   formation state 0 → a new plan (FIGHTER_FLYING §4.11 A). **Both go through item 6 and item 7 only; the slot
   (θ, ρ) in the block is kept and never re-tested.**
9. **Worked case: one ranged squad (attack range 250) ordered onto a standing structure 1000 away.** Take the
   squad at (0, 0), the structure centred on (1000, 0) with hard half extents 40 × 40 and stamped on cells 48-52,
   open ground, the squad's HX = 15 with its rear row 15 behind the centre [M for the numbers that depend on
   slot 43 and on the stamp].
   - Composition: the validated point is thrown away (object goal); the squad is not solo (1000 > r + 290 +
     60) and not short-range (1000 > 290 × 1.6); E_r = 250 + 40 = 290.
   - `0x438c10`: the squad is in L; R = 290. Reference path: G = (1000, 0) is not reachable, so it becomes the
     nearest ring cell of the squad's zone, (940, 0); path (0, 0) → (940, 0). All six tries fail in item 5 step
     3, because every line P → (1000, 0) ends on the structure's own cell. **No slot.**
   - Plan: path (0, 0) → (940, 0), L = 940, no slot leg. Trim: from s = 940 back in tens every member is in max
     range while its distance to the centre is ≤ 250 + 40 = 290; the rear row at s − 15 passes down to s = 730
     and fails at s = 720. **The path is cut to 730: the squad's centre stops 270 from the structure's centre**,
     its rear row 285 away, without having gone near the footprint.
   - The same squad against a **unit** standing on open ground at (1000, 0) with half extents 5: E_r = R = 255;
     the first try puts it at θ = 0 on the ring 255 − 15 = 240; the plan's path is (0, 0) → (760, 0) → (1000, 0)
     and the trim cuts it at 760, the slot point. A second formation of the same order would get ± the chord
     angle on that ring, walk start → its Q' → the target, and be cut near its own Q'.

***Port spec (attack approach):*** (1) At an attack order / auto-attack push on land, give each formation that
cannot yet fire a slot (θ, ρ) by items 2-5; none when the target's own cell is not enterable. (2) At every plan:
if every member can fire, stop and report arrival; else path to the target's point (re-validated when
unreachable), via the slot point when there is a slot. (3) Cut the path back from its end in steps of 10 to the
outermost point of the final stretch from which every member can fire (max range, min range, line of fire). (4)
Keep the 30-frame checks of FIGHTER_FLYING §4.11. The slot test's object rule: standing objects only (no path
record), expanded blocker areas, < 15 % of HX × HY.

*Not read:* TARGETING slots 43 / 46 at a point (taken from TACTICAL_COMBAT §2.8 / TACTICAL_GAPS §3.1); the
position finder `0x440f90` of the staging; the value the line test returns as "distance" when the first cell is
blocked; whether `0x411640`'s fallback size (mode-0 plan failed) is ever used for an attack block; block +0x5f.

### 4.4 Moving as a group

**Side-error correction** (LocomotorCommon `0x42d440`) [H]:
- error = (slot lateral offset along the formation path) − (the member's actual lateral position).
- If |error| > **FormationMinimumSideError = 0.1**, steer along the path normal by (error ∓ 0.1) / **FormationMaximumSideError = 30**. This is a proportional pull back to the slot.

**Final facing** (`0x411980`) [H]:
- If the angle between the last path segment and the requested facing is ≥ **FinalFormationFacingMinimumAngle = 10°**, the formation rotates while it arrives.
- The rotation is limited to **FinalFormationFacingDeltaCoefficient = 0.5** × path length / formation diagonal of the angle, spread over the remaining path at rate (target − limited)/length × 1.2.
- `Rotate_Formation_Facing_Moves` = true in vanilla.

**Speed matching:**
- Teams use the minimum member speed (§3.4) [H].
- Inside a formation, the slowest units are in front (§4.3), and a formation speed exists (data pack +0x158; debug labels FormationSpeed, FormationStagePoint and FormationYOffsetCoefficient in dumper `0x4eea10`).
- How that speed is computed was **not found**. [L] Recommended reading: it is the minimum effective speed of the members.
  *Resolved s78n t26 [H]:* `0x41b2a0` = the minimum over the members (list order, null members skipped) of locomotor
  vf +0x250 (Walk `0x42c5e0` = `0x370f00`, the effective max speed of §3.4, with no deployed switch), −1 with no
  member; capped at the constant `0x140b16d54` when the formation's last block has flag 0x10 [M: the constant's value
  and the flag's name]. The Walk speed controller uses it on a formation path (§3.5.2 B).

**Formup:**
- Land formations skip formup when **ShouldSkipLandFormup = true** (vanilla) and formation +0x9c is clear (`0x41c1a0`). In vanilla, land groups therefore start without waiting.
- FormationFormupWaitType (`0x5dddc0`): WaitForAll 0, WaitForFront 1, WaitNone 2. Neither it nor MaxLandFormationFormupFrames (240) was found being read. [L]

**Space formations:** see §4.6 (cu8j, 2026-11-08). The space composition pass makes one formation per ship with a
nonzero `Space_Layer` role and one per recruited group of role-0 objects (fighters, squadron containers).
`Set_Capital_Ship_Formation` `0x4fca30`, `Set_Lead_Formation` `0x4fd5b0`, `Set_Screen_Formation` `0x4fdfb0` and
`Set_Transport_Ship_Formation` `0x4fea10` (sole caller `0x4fbb10`) are **not** formation functions: they are the
campaign fleet placement at a space battle's start, which re-adopts each placed object (§4.7). `Merge_Formation`
`0x41fa90` creates nothing. `Squadron_Formation_Error_Tolerance` is still unread.

### 4.5 Formation constants (vanilla)

| constant | value |
|---|---|
| Max_Formation_Area | 14.0 |
| Spread_Out_Spacing_Coefficient | 1.8 |
| BetweenFormationSpacing | 5.0 |
| FormationMinimumSideError / FormationMaximumSideError | 0.1 / 30.0 |
| FinalFormationFacingMinimumAngle / …DeltaCoefficient | 10.0 / 0.5 |
| Rotate_Formation_Facing_Moves | true |
| ShouldSkipLandFormup | true |
| MaxLandFormationFormupFrames | 240 (no reader found) |
| ShouldInfantryTeamsSplitAcrossFormations | true |
| Vehicle/InfantryFormationRecruitmentDistance | 100 / 60 |
| DoubleClickMoveMaxSpeedRatio | 1.25 |
| Solo_Attack_Range | 60 |
| Short_Range_Attack_Formation_Coefficient | 1.6 |

### 4.6 When formations are created; space formations (cu8j t01, 2026-11-08) [H unless tagged]
Read from the factory `0x4f6fb0` and its callers, the coordinator vtables (base `0x87b278`, land `0x86b080`, space
`0x8a6520`), the system update `0x426eb0`, `0x4f8080`, `0x5d58f0`, `0x43e5b0`, `0x43c1b0`, `0x41c1a0`, `0x4192b0`,
`0x410a60`. F = the FormationClass, FPS = 30, now = the global frame (0x140b0a320).

#### 4.6.1 Every FormationClass creation
The ctor `0x40d0d0` has one caller, the factory `0x4f6fb0`; the factory has five callers. Each creation is **one**
synced `int(1, FPS)` (the ctor's call 0x40d11a, return 0x40d11f; factory → ctor return 0x4f6fd4).

| Creator | Return into it | When | Formations made |
|---|---|---|---|
| land composition `0x43e5b0` (land vf12) | 0x43e712 | coordinator update, when +0xa8 = 1 (§4.6.2) | one per split descriptor (§4.2, §1.6) |
| space composition `0x5d58f0` (space vf12) | 0x5d5c19 | same | one per ship with a role, one per role-0 group (§4.6.4) |
| `Issue_Degenerate_Move` `0x4f7360` | 0x4f738a | at once; only via vf8 (land `0x43b470`, space `0x5d2030` = vf3 add, then `0x4f7360`), only from the adopt `0x39bcb0` (§4.7) | one holding every object of the coordinator: move type 1 to their centroid (+0x40..+0x48), Set_Move, +0xa8 := 0, `0x419ee0` |
| `Split_From_Formation` `0x4f84e0` | 0x4f8518 | at once | one, in the same coordinator, for the split object |
| coordinator load `0x4f7530` (from `0x426690`) | 0x4f7708 | save load | one per saved formation (port saves are its own: no parity need) |

- `Split_From_Formation` is reached through `0x424850`, which acts only when the object's pack +0x1f0 is that
  formation. Callers of `0x424850`: `0x38a350` (ability activation, e.g. MISSILE_SHIELD with ability +0x10c set and
  lock count 0), `0x3a8e50` (movement lock counter), `0x42d0f0` / `0x42d2a0` (combat target splits), `0x4f6470`,
  `0x575710` (Divert), Lua `0x58d930` / `0x58e370` / `0x58dd10` / `0x58e8e0` [H for the list, M for the meanings].
- `Merge_Formation` `0x41fa90` (from space vf10 `0x5d1dd0` and vf9 `0x5d68c0`) moves members and creates nothing.

#### 4.6.2 When: the coordinator update, not the order
- `MovementCoordinatorSystem` update `0x426eb0` runs at common tick `0x3639d0` step 10, after the GOM service
  (MAIN_LOOP §1.2). Per coordinator, in vector (creation) order: vf5 (+0x28) service; vf9 against the later
  coordinators; empty coordinators deleted; `0x4f8670`.
- vf5 (base `0x4f8080`; space `0x5d51e0` calls it, then its own stagger): vf10, then repeat until the call returns
  true: +0xa8 = 0 → vf11 (`0x4f80c0`, formation steps `0x422b70`); +0xa8 = 1 → vf12 (composition).
- vf3 add (`0x4f6b60`; land `0x438b40` and space `0x5cf420` reach it) sets +0xa8 := 1. vf4 remove (`0x4f7d30`,
  space `0x5d5130`) does not.
- Group move `0x2a7b80` (§6.2): `0x426010` (new, empty coordinator), per admitted unit `0x426d50` + vf3, then
  `Move_To` `0x4f8200`, which only re-targets the coordinator's **existing** formations (none yet). About 35 other
  callers of `0x426010` follow the same pattern (vf3 adds, then `0x4f8200`).
- So the ctor draws of an order come at the **next step 10 after the order**:
  - player commands (executed before the tick: T2, or main-loop step 10 in SP; MAIN_LOOP §1.1) → the same frame;
  - Lua object scripts run during the GOM service → the same tick;
  - AI (T7) and story (T9) orders, issued after step 10 → the next tick [M: from the MAIN_LOOP §1.2 order].
- Draw order: coordinators in creation order; inside one, in composition order. Draws of other systems that run
  between the order and step 10 (object services: animation variants, weapons, …) come **before** them.

#### 4.6.3 What the ctor's r does
- Ctor: F +0x1d8 := now + FPS + r (r = `int(1, FPS)`), F +0x1dc := now − 500.
- F +0x1d8 is the **next-check timer** shared by:
  - `0x4192b0` (state 1: guard follow / path copy / re-path, FIGHTER_FLYING §4.11 A and G 2): no members → return;
    frame < F +0x1d8 → return; then F +0x1d8 := int(float(FPS) + float(frame));
  - `0x410a60` (states 3 / 4: the arrived re-check): runs when F +0x1d8 ≤ now, then F +0x1d8 := now +
    int(FPS + 0.5);
  - writer `0x419f20` (F +0x1d8 := now), whose only caller is Walk `0x61f510` (land).
- So a new formation's first check runs at its first service with frame ≥ creation frame + 30 + r (31 to 60 frames
  after creation), then every 30 frames. Land and space alike; for fighters it is the port's FighterFormation `next`.
- r is used nowhere else. The start / plan frame is F +0x1dc (§1.6): the ctor's now − 500 lets the first plan
  through at once; the land split overwrites it; the space composition and the other creators keep it.

#### 4.6.4 Space composition `0x5d58f0` (space vf12; log "Service_State_Composition_Changed, FID: %u, COID: %d")
1. Candidates = the coordinator's objects not already in a formation (`0x4f7260`), in object order. The goal point
   is computed once, from the first candidate (`0x583150` on the order block +0x40).
2. While candidates remain:
   1. Seed = the remaining candidate nearest the goal (strict <: ties keep the earlier one).
   2. Seed without a type (`0x3973b0`), without a locomotor, or with max speed (`0x370f00`) 0 → **return 1 without
      clearing +0xa8**: the pass runs again at the next update (with the objects not yet in a formation) [M for the
      re-run's effect on the formations already made].
   3. Remove the seed. Role = `Get_Space_Layer` `0x397900(seed, 0)`: `Stationary_Space_Layer` (+0x16e0) when the
      object is not moving and that value is ≠ 0, else `Space_Layer` (+0x16dc); 4 when `Uses_Multiple_Locomotors`
      (+0x74) and the locomotor's vf +0x240 is true and vf +0x238 = 2.
   4. **`0x4f6fb0`: new formation, one ctor draw.** Add the seed (`0x40e0f0`), set the formation kind (locomotor
      vf +0x238), `Set_Move` with a block variant chosen by the coordinator's move type (+0x64).
   5. Move type ≠ 4:
      - **role 0** (fighters, squadron containers, anything without a layer): append to the role-0 list. Unless the
        seed is locked (`0x39ad40`), recruit, in candidate order, every remaining candidate with all of: the same
        locomotor vf +0x238; |dx| ≤ max(2 × seed extent x, 0.33 × |seed → goal|) + candidate extent x, and the same
        for y (z unlimited, 1e18) [M: which extent]; role 0; max speed ≠ 0; min speed(candidate) / max
        speed(seed) ≤ 0.9 and min speed(seed) / max speed(candidate) ≤ 0.9 (min speed `0x3728f0` = `Min_Speed` ×
        the multipliers). Recruits join the formation and leave the candidates. No draw;
      - **roles 1 Capital, 0x20 SuperCapital, 2 Frigate, 4 Corvette:** append to that role's list. **One formation
        per ship.**
3. After the loop: unless flag 2 or +0x9d, each role-0 formation gets a vf +0x38 check, then Push `0x41fd30`. Then
   `0x5d4730` on the capital, frigate, corvette and supercapital lists, in that order (a group goal per role through
   `0x5cff60` / `0x5d2060` / `0x5d2a60`, per-ship timed-path starts offset by GC `0x140b16bfc` per ship, and an
   immediate formation step `0x422b70`; no synced draw found [M]). +0xa8 := 0, return 0.
4. **Draws:** exactly one ctor draw per formation, in seed order (nearest the goal first, roles interleaved). Plan
   `0x41c1a0` draws only for queued blocks (§1.6). The state-3 float draw in `0x423070` needs
   `ShouldUseSpaceIdleMovement` (GC 0x140b16d51), false in FoC.
- Count for one space group move: (admitted objects with a role) + (role-0 groups). A selected squadron is one
  object, its container (the group move replaces team members by the container, §6.2); squadrons ordered together
  usually merge into one role-0 formation [M: depends on the box and speed rule above].

#### 4.6.5 What the port needs (t01)
Port today (`engine/tactical/movement/src/movement_system.cpp`): `move_space` uses the land k-means and
`split_into_formations` and draws nothing; `fform_order` makes one FighterFormation per key (squadron container or
lone fighter) per order with `f.next = frame + 30`; `move_land` draws `formation_ctor_draw` per non-queued formation
**at the order**, with start = now + acc + r − 30 (cu8i).
1. Draw `int(1, FPS)` once per formation creation, at the movement coordinator update (retail common tick step 10,
   after the object services), coordinators in creation order, not inside `move()`. Same for land.
2. Space split: one formation (one draw) per admitted ship whose role (`Stationary_Space_Layer` when stopped and
   ≠ 0, else `Space_Layer`; 4 for a multi-locomotor kind-2 unit) is nonzero; role-0 objects grouped by the §4.6.4
   seed / recruit rule, one draw per group; seeds nearest the goal of the first candidate first.
3. Fighter formations: `f.next` = creation frame + 30 + r (the same r), then + 30 per check. The same timer gates the
   state-1 re-path / guard check and the arrived re-check of every formation.
4. Land start (§1.6): the plan gate is F +0x1dc = now + acc + r' − 60, r' = `int(0, PathFrameDelay_seed)` drawn only
   for seeds with `PathFrameDelay` ≠ 0, before the ctor draws; with no `PathFrameDelay` the formation plans at once,
   except that one created at frame ≤ 60 waits until frame 61 [M]. Replace the cu8i "start = now + acc + r − 30".
5. A seed with no locomotor or max speed 0 stops the space pass; it is retried every update (no draw for it).
6. Goldens move (space ones and the benchmark scenario; land too when the draw moves from the order to step 10).

### 4.7 Formation creations outside a move order (cu8j t02, 2026-11-08) [H unless tagged]

#### 4.7.1 The adopt `0x39bcb0(obj)`: one draw, at once
Returns without doing anything unless every check passes:
1. the mode has a coordinator system (mode vf +0x228) and mode vf +0xf8 is false;
2. the object's parent (+0x2b0) does not have TEAM (+0x348): **team members are never adopted**, only their
   container;
3. with a LOCOMOTOR (+0x333): its vf +0x238 = the mode kind (1 land, 2 space);
4. admission `0x425e30`: a locomotor with vf +0x220 true; not (vf +0x228 and the parent's locomotor vf +0x230); land
   with its own TEAM: needs vf +0x230; space: `0x39b1a0` (the object is a team member) false; not a
   TACTICAL_BUILD_OBJECTS (0x29, +0x35b) builder with `0x42e3b0` / `0x42e3d0` non-null; not in vf +0x2b0 states (4,
   5, 0x28, 0x2b, 0x2c: hyperspace);
5. no coordinator holds it yet (`0x426140` / `0x4f72e0`).

Then `0x426010` (new coordinator) and vf8 (call 0x39bd9c, return 0x39bda0) → `Issue_Degenerate_Move`: **one
formation, one ctor draw, immediately** (a degenerate move to the object's own position: it holds there).

Locomotor answers (vtable slots read with the exe):

| Locomotor | vf +0x220 | vf +0x228 | vf +0x230 | vf +0x238 |
|---|---|---|---|---|
| Walk, Bike | true | false | false | 1 |
| LandTeamInfantry | true | **true** | false | 1 |
| LandTeamContainer | true | false | **true** | 1 |
| SimpleSpace, Fighter, Flying, LandBomber | true | false | false | 2 |
| Team (`TEAM_LOCOMOTOR`, `0x6305a0`) | true iff any member's is true | false | false | 2 |
| JetPack | true | false | false | by state |
| Starship, Fleet, Simple (`LocomotorBehaviorClass`) | false | false | false | −1 |

So in vanilla every unit with a locomotor qualifies except team members, builders, hyperspacing objects and objects
already in a coordinator; land team containers qualify through LandTeamContainer; a `Team` container qualifies only
in space (kind 2).

#### 4.7.2 Callers of `0x39bcb0` (return address into each caller, for traces)

| Caller | Return | What |
|---|---|---|
| campaign spawn `0x2a00b0` | 0x2a1bcd | per member not in a team (§4.7.3) |
| team spawn helper `0x2a5a60` | 0x2a61d5 | per team container (§4.7.3) |
| garrison leave `0x37a060` | 0x37a369 | |
| ability activation `0x38a350` | 0x38a96c | |
| GOM post-load finish `0x3952e0` (from the Load_Map tail `0x2b4440`, return 0x2b44a7; load thread) | 0x395421 | per object, Load_Map tail (CU8G_T02_ENV_PICK_RE §7); `_MP_LAND_ENDOR`: the neutral Land_Speeder only, not the starting forces (CU8K_T02_T06_RE §1.1) |
| DEPLOY `0x3a1ce0` | 0x3a1f81 | |
| movement lock counter `0x3a8e50` | 0x3a8e9a | |
| stop and hold `0x3a9880` | (below) | |
| container eject `0x3abb80` | 0x3abdf8 | |
| StopMovement `0x4261f0` | 0x4265ce | only with type +0x3d4 (`MoveAttackWhileStopped`, unused in FoC XML) |
| SpecialAbility `0x48ce70` | 0x48cecb | |
| fleet placement `0x4fca30` / `0x4fd5b0` / `0x4fdfb0` / `0x4fea10` | 0x4fd0a3 / 0x4fdb3d / 0x4fe59d / 0x4fefc5 | per placed object (§4.7.3) |
| SpaceRetreatCoordinator vf15 `0x532ee0` | 0x533017 | |
| Fighter locomotor hyperspace finish `0x5c9d40` | 0x5c9e62 | |
| Landing vf6 `0x630fe0` | 0x63107f | |

**Stop and hold `0x3a9880(obj)`** (with a LOCOMOTOR): locomotor vf +0x138(vf +0x188()), then leave every
coordinator (`0x426d50`) and adopt (`0x39bcb0`): **one draw**. It is the "stop here" of many ability activations
(e.g. `0x713a90`, Force Reveal begin: stop, then play anim 0x69) and other callers: `0x3a4070`, `0x443733`,
`0x4446e1`, `0x58f7a6`, `0x5b07c0`, `0x5b09c8`, `0x630b52`, `0x635ee0` / `0x635f60` / `0x635fc0` / `0x636030`,
`0x6cc640`, `0x6f3800`, `0x700860`, `0x712600`, `0x719c90`, `0x71a50d`, `0x71adfa`, `0x71c430`, `0x71ce68`,
`0x71d370` [H for the list]. Some entries are return addresses; each caller's class, ability and situation, and the
other adopt sites: CU8K_T02_T06_RE §2 / §3 (the draw only happens when the stopped object passes the predicate, so
squad members draw nothing). The port has none of these draws.

#### 4.7.3 Unit creation paths
- **`0x29f810` (create object) adopts nothing** [H]. Exception: an ability that starts active goes through
  `0x3a82a0` → `0x38a350`, which can adopt or split [M].
- **Load_Map tail** (`0x2b4440` → `0x3952e0`): every object, in GOM order, through the full predicate; land and
  space alike [H code]. Traced: one draw on `_MP_LAND_ENDOR` (an Is_Neutral `Land_Speeder`, cu8i), none at Kashyyyk
  (CU8G §7). Skirmish starting forces and the campaign spawn come later, so it does not cover them.
- **Team spawn helper `0x2a5a60`:** splits the members into chunks of `Max_Squad_Size` (type +0x1e44, default 10).
  Per chunk: create the container (type `Create_Team_Type` +0x1e58, else `Team`: CONTAINERS.XML, behaviours TEAM,
  SELECTABLE, UNIT_AI, TEAM_LOCOMOTOR) through `0x29f810`, position it, `0x2b3670`, `0x3a82a0`, in space the fleet
  placement positions, then **`0x39bcb0(container)`: one draw per qualifying container**, before the next chunk.
  (cu8k t06: in space only. A land team container already owns a coordinator from its locomotor init `0x5abac0`, so
  the adopt draws nothing; it composes at the next step 10 instead: `docs/re/CU8K_T02_T06_RE.md` §4 rules 1 and 4.)
  Callers (return into each): skirmish company placer `0x29e960` (0x29ecb5), campaign `0x2a00b0` (0x2a1b51),
  garrison grouping `0x2a5790` (0x2a5a02), reinforcements `0x2bf2d0` (0x2bf743, 0x2bfc2f), REPLENISH_WINGMEN
  `0x3a2970` (0x3a2c80), SpawnIndigenousUnits `0x531eb0` (0x5320d4), SpawnSquadron / hangar `0x593960` (0x5940be),
  `0x595a30` (0x595cf9).
- **Skirmish starting forces** (`0x2a4fc0` → company placer `0x29e960`): the company's units are created first; the
  helper runs only when the first member's type has `Create_Team` (+0x67). In FoC that is infantry companies,
  indigenous units, slaves, some heroes (R2-D2, C-3PO, the field commanders) and fighter squadrons. **Land: no draw
  at creation** [H, traced 2026-11-09]: a land container's `LandTeamContainer` locomotor init (`0x5abac0`) already gave
  it a coordinator of its own (`0x426010`, vf3, Move_To its own position, no formation), so the helper's adopt fails
  the predicate's check 5; the container composes at the next coordinator update instead: **one land-composition draw
  per container at gf 1, step 10**, in creation order, after that tick's object services (then the containers
  spawned during gf 1, e.g. indigenous). **Space** [M, untraced]: squadron (`Team`) containers do not register
  themselves, so the helper's adopt draws at once, one per container after its creation draws; `SimpleSpace` ships
  register themselves (`0x625090`) and compose at gf 1. Other units (land vehicles, heroes outside a team) are not
  adopted and draw nothing until their first order. Details: CU8K_T02_T06_RE §1.
- **Campaign spawn `0x2a00b0`** (only caller `0x2a1eb0`, called twice). Per group:
  1. when `Create_Team`, or always in space: the helper (container draws); when the first member's `Ship_Class`
     (+0x650) < 7, register for fleet placement (`0x4fc7e0`, which maps a team member to its container);
  2. each member not in a team (`0x39b1a0` false) → `0x39bcb0`: one draw each (land vehicles, heroes, …);
  3. at the end, space only: fleet placement `0x4fbb10` (CU8K_T03_T08_T09_RE §1), per entry marker. By `Ship_Class`
     (the first member's, for a container): `capital` (5) → `Set_Capital_Ship_Formation` `0x4fca30` (the grid
     core); `corvette` / `frigate` (3, 4) → `Set_Screen_Formation` `0x4fdfb0`; `fighter` / `bomber` (0, 1) →
     `Set_Lead_Formation` `0x4fd5b0`; `transport` (2) → `Set_Transport_Ship_Formation` `0x4fea10`; `super` (6) → the
     grid with zero spacing (at the marker); without capitals the first non-empty class becomes the grid core. Each
     placed object: `0x426d50` (it leaves its degenerate coordinator, deleted empty at the next update) and
     `0x39bcb0`: **a second draw per placed object**, in placement order (supers, core, screen, leads, transports).
     No other synced draw in the placement.
  4. after all markers, only for the attacker with the arrival cinematic on: the hyperspace-in, LOCOMOTOR vf +0x290(0)
     per registered object in registration order. For a **team container** (`TeamLocomotor` `0x62f800`) it leaves
     the coordinator and takes the movement lock (`0x3a8e50`, count 0 → 1), which adopts: **a third draw per team
     container** (return 0x3a8e9a ← 0x62f856 ← 0x4fc34b); ships (SimpleSpace `0x6251d0`) draw nothing here.
  - So a space battle's start (attacker) takes one helper draw per container, one placement draw per placed object
    and one lock draw per container: Kashyyyk 8 = 3 per X-wing squadron + 1 per ship (traced). Each arrival's end
    (about 150 frames later) gives one more space composition per ship and per container. Jabiim (land):
    `0x2a1eb0` drew nothing (the attackers were in the reinforcement pool).
- **Reinforcements** (`0x2bf2d0`, hyperspace / landing): the helper, one draw per qualifying container; the landing
  and the fighter hyperspace finish adopt again (`0x630fe0`, `0x5c9d40`) [M: order relative to the helper].

#### 4.7.4 Jetpack swap re-issue and the hold (`0x3a9910`, §3.1.1 step 4)
- To the secondary only: the unit leaves every coordinator, a new coordinator is made (`0x426010`), the unit is added
  (vf3 → +0xa8 := 1) and `Move_To(B)` re-issues the saved order, or a position move to its own position when it had
  no coordinator (the hold). Move_To makes no formation.
- The formation comes at the **next coordinator update** (the land composition `0x43e5b0`): **one ctor draw** (plus
  r' when the type has `PathFrameDelay`, which no vanilla jetpack unit has). The JET_PACK ability (`0x3a38d0`) calls
  the swap during the object service, so the draw comes at step 10 of the same tick.
- The swap back to the primary makes no coordinator and no draw. The coordinator the unit left is not recomposed
  (vf4 does not set +0xa8).
- **Confirmed by the 2026-11-09 run** (CU8K_T04_COORDINATOR_TRACES, item 4 run 3): each JET_PACK activation gives
  exactly one land-composition ctor draw (0x43e712) at the coordinator update of the first frame in which the unit no
  longer runs its Walk locomotor, after that frame's object-service draws, for a moving and a standing unit alike;
  the flight, the landing and the swap back draw nothing.

#### 4.7.5 What the port needs (t02)
1. One `int(1, FPS)` per adopt (the §4.7.1 predicate), **at once**, at: each team container right after its chunk
   is created (skirmish starting forces with `Create_Team`, reinforcements, REPLENISH_WINGMEN, indigenous spawns,
   hangar squadron spawns); the campaign spawn (per non-team member, per container, and again per placed object in
   space); the Load_Map tail pass (land done in cu8i; space too); stop-and-hold `0x3a9880` in the ability activations
   that call it; DEPLOY, garrison leave, container eject, landing, fighter hyperspace finish, space retreat, lock
   counter. (cu8k t06: the land part of the team-container adopt is superseded: land team containers draw at the
   next step 10 after their creation, not at the adopt; `docs/re/CU8K_T02_T06_RE.md` §4 rules 1, 2 and 4.)
2. The jetpack swap to the secondary: one draw at the next movement update (step 10), not at the swap.
3. Team spawn order: retail creates each chunk's container and adopts it before the next chunk. The port's helper
   (`tactical_battle.cpp`) creates the containers after all members, in chunk order: the draws must interleave as
   retail's.
4. Keep the predicate: never team members, land `Team` containers (kind 2), builders, hyperspacing objects, or
   objects already in a coordinator.

#### 4.7.6 Trace plan for Gabriel (settles the [M] counts; optional)
Run 2026-11-09 (`CU8K_T04_COORDINATOR_TRACES.md`): item 2 differs (one degenerate draw through an adopt returning at
0x395421, then 18 land-composition draws at the first coordinator update, gf 1; no 0x2a61d5 / 0x2a4fc0 in any stack);
item 3 with two fighter squadrons gives 1 space-composition draw as expected. All four items are now run, results in
`CU8K_T04_COORDINATOR_TRACES.md`: item 1 settled with one addition (a lock-counter draw per team container at the
end of the fleet placement, below); item 3 with a capital ship, a frigate and two X-wing squadrons gave 4 draws (the
squadrons stayed two role-0 groups: CU8K_T03_T08_T09_RE §3, the recruit box test [M]); item 4 settled as expected
(§4.7.4).

Kit: `tools/probe_mod/cu8h_probes/` (`run_cu8h_trace.cmd`, `cu8h_dbg.py`; README there). No game change is needed.
Each ctor draw is one or more HIT lines (one per LCG step of the `int` draw) whose `stack=` (RVAs in hex without
`0x`, first entry = the PC inside the draw primitive) continues `40d11f,4f6fd4,`: the return inside the ctor, then
inside the factory. The next RVA names the creator: `43e712` land composition, `5d5c19` space composition, `4f738a`
degenerate (then the vf8 return `39bda0` and the adopt caller's return from §4.7.2), `4f8518` split, `4f7708` load.
1. **Kashyyyk space load** (the cu8h galactic space battle): `run_cu8h_trace.cmd --label kashyyyk
   --stop-mode-frame 120`; press Enter just before the battle starts. Expected under `0x2a1eb0`: one degenerate draw
   per container (adopt return 0x2a61d5) and one per placed object (0x4fd0a3 / 0x4fdb3d / 0x4fe59d / 0x4fefc5).
   **Run 2026-11-09: as expected, plus one draw per team container through the movement lock counter (0x3a8e9a ←
   `TeamLocomotor` vf82 0x62f856 ← 0x4fc34b) after all placement draws** (§4.7.3 campaign spawn step 4). Capital →
   0x4fd0a3 (`Set_Capital_Ship_Formation`), frigate → 0x4fe59d (`Set_Screen_Formation`), the two squadrons →
   0x4fdb3d (`Set_Lead_Formation`).
2. **Skirmish land load** (`_MP_LAND_ENDOR`, as cu8h t01): one draw per `Create_Team` chunk under `0x2a4fc0`
   (adopt return 0x2a61d5), after that company's creation draws. **Run 2026-11-09: differs, explained**
   (CU8K_T02_T06_RE §1): the one degenerate draw (0x395421) is the Load_Map-tail pass (the neutral Land_Speeder);
   land containers register themselves at creation, so the helper's adopt draws nothing and each container composes
   at gf 1 step 10 (0x43e712). The trace has **12** compositions at gf 1 (10 starting containers, then 2 indigenous
   ones spawned in gf 1) and 2 more after each later indigenous spawn (gf 22, 25, 29): CU8K_T04's "18" is the total
   to mode frame 120.
3. **Space group move** (skirmish space battle, `--arm-file`, arm just before the order): select one capital ship,
   one frigate and two fighter squadrons and give one move. Expected at step 10 of that frame (stack through
   `0x426eb0` and 0x5d5c19): 2 draws for the ships plus 1 per role-0 group (1 when the squadrons merge).
4. **Jetpack** (land skirmish, a Rocketeer company): activate JET_PACK while moving. Expected: one draw through
   0x43e712 in the same frame, after that frame's object-service draws. **Run 2026-11-09 (Dark_Trooper_PhaseII,
   marker targets): confirmed**: one land-composition draw at the coordinator update of the first frame without the
   Walk locomotor, moving or standing; none from the flight or the swap back (§4.7.4).

To list them: `python -c "import sys;[print(l.split(' mf=')[1].split()[0],k[3:7]) for l in open(sys.argv[1]) for k in [l.split('stack=')[-1].split()[0].split(',')] if k[1:3]==['40d11f','4f6fd4']]" <trace log>`
prints the mode frame and the four RVAs after the factory for each hit (an `int` draw whose rejection loop stepped
the LCG more than once prints once per step, with the same stack).

---

## 5. Collision (land)

### 5.1 System [H]

`CollisionSystemClass`:
- Created by the land mode (ctor `0x5045e0`, at `0x3b81b0` / `0x3b8950`).
- Keeps a reference list of registered movers (+0xd8); vf1 `0x50a240` removes one on signal 0x10.
- Keeps an **object-ID-keyed hash** (+0x80) from object to its active collision IDs (resolve `0x5080d0`). It is never iterated.
- Counts active collisions at +0xb8 (register `0x5049e0`).

Pair classes:

| class | vftable | factory |
|---|---|---|
| MovingVsMoving | `0x8b5090` | `0x680420` |
| MovingVsStopped | `0x8b52e8` | `0x6843b0` → `0x685480` |
| StoppedVsStopped | `0x8b5450` | `0x686de0` |
| TerrainCollisionClass | `0x8b5568` | `0x687db0`, from the failed-step report `0x509090` (§5.4.7) |

**There is no rigid-body physics.** Collision means detecting a future conflict along the planned paths and resolving it by behaviour.

### 5.2 Detection (`0x508170`, per frame) [H]
*(s78n, 2026-11-10: which units register (squad containers yes, their members no) is FIGHTER_FLYING §7.5. The candidate list of item 4 is tracking layer 0x10, which the tracker fills only from the registered mover list (`0x53a400`), so squad members are never candidates either: FIGHTER_FLYING §7.8. This corrects the earlier note "the candidate list has no team-member filter".)*

For each registered mover, **in registration-list order**:
1. Skip a multi-locomotor unit whose active locomotor answers vf +0x240.
2. **Path-approximation countdown** (locomotor data +0x275). At ≤ 0, rebuild the approximation (`0x505fe0`, using ApproximationSmoothCosAngle 0.9962 and ApproximationForwardDistance 2.0) and reset the countdown to **FramesPerPositionApproximationRebuild = 8**. Otherwise decrement it and run `0x50aad0`.
3. **Collision-check countdown** (+0x274). Check when it is ≤ 0 or the approximation was just rebuilt, then reset it to **FramesPerCollisionCheck = 4**. Each unit is therefore checked every 4 frames, phase-shifted by its own history.
4. **Candidates** come from the land mode's object grid (mode vf +0x238 = the object-tracking system, its layer with id 0x10 = Land, query `0x5d7a00`), **not from TreeCull**. That layer holds exactly the registered movers whose layer (`0x397900`) is Land and whose +0x3a7 is clear (tracker land update `0x53a400`, FIGHTER_FLYING §7.8):
   - an axis-aligned **160 × 160** square centred on the unit;
   - results go into a global vector (`0x140b31c50`) in grid order;
   - **the grid's internal order was not traced. A parallel port must sort the candidates canonically (e.g. by object ID) and accept that this may differ from retail.**
5. **Pair filter.** Skip the pair if any of these holds:
   - it is self;
   - neither side has a locomotor;
   - either side has flag +0x3a7;
   - one side is the build pad holding the other (pad pack object +0x150: +8 or +0x10 is the other side);
   - a collision already exists for the pair (`0x5c7900`);
   - the UnitCollisionClass rules below say no.
6. **UnitCollisionClass rules** (enum `0x5e1110`; −1 None, 0 Landing Transport, 1 Build Pad, 2 Low Obstacle, 3 Medium Obstacle, 4 High Obstacle, 5 Infantry, 6 Bike, 7 Vehicle, 8 Large Vehicle, 9 Giant Vehicle):

   | class | collides with |
   |---|---|
   | Build Pad | units with a locomotor, only when the faction test `0x2823e0` is false (enemies only) [M] |
   | Low Obstacle | Infantry only |
   | Medium Obstacle | classes 5–8 |
   | High Obstacle | classes 5–9 |
   | everything else | everything |

7. **Classification:**
   - mover moving and other moving → **MovingVsMoving**, when the predictor `0x442540` finds contact within **MovingVsMovingLookAheadTime = 1.5 s**;
   - mover moving and other stopped → **MovingVsStopped**, when `0x683790` hits;
   - both stopped → **StoppedVsStopped**, when `0x686500` hits;
   - stopped against moving → skipped (the other unit's iteration finds it).
   - "Moving" = locomotor vf +0x1a8 or vf +0x1b0.
   - **What "Moving" is (s78n t19, 2026-11-10) [H].** vf +0x1a8 = `0x42cc60` (the same function in the Walk,
     LandTeamContainer and LandTeamInfantry vftables): the unit has a locomotor pack, is not suspended (vf +0x210 =
     pack +0x260) and (**speed pack +0xec > 0 or its path stack is not empty**, `0x5569b0`). vf +0x1b0 = `0x42cd30`:
     not suspended and the state is 0xf, 0x10, 0x11 or 0x20 (turning in place). Neither reads an order, a formation
     or the path kind. So an **order-less unit on a pushed path** (kind 4 / 7), on a StoppedVsStopped path (§5.4.3)
     or on a Path Around detour **counts as Moving** from the frame the path is pushed (speed still 0) until the
     path is popped and its speed is 0 again; a unit whose path is not started yet (start delay) is Moving too. An
     object **without a locomotor** (a death-clone obstacle, a structure-like unit) is never Moving and is only
     ever the stopped side. The tracker's Static / Moving flag (§5.3.2) uses the same vf +0x1a8.
   - *Port spec:* moving(u) = not suspended and (speed > 0 or any path record on the stack, started or not) or
     turning in place; classify the pair from that, with no order test.

**Vanilla class counts:** Infantry 123, Vehicle 55, Medium 20, High 19, Landing Transport 10, Giant 9, Large 8, Build Pad 6, Low 3, Bike 1.

### 5.3 Contact tests [H]

**MovingVsStopped** (`0x683790`(mover a, obstacle b)):
- Gates: landing transport (on b), garrison host and garrisoned (both sides), raised hover. Rules: "Pair gates" below.
  (Corrected 2026-11-10: behaviour 0x36 is TRANSPORT_LANDING, not a shield barrier; +0x2a8 is the hover state, not a
  formation-member flag; there is no attachment test.)
- d = predicted distance along the path to contact (`0x441b60`, path +0x200). Hit when d / max speed < fps × max(MovingVsMovingLookAheadTime, the type's `MovementPredictionInterval` (+0x3c8)), plus 1.5 s if the mover can push the obstacle (`0x5c8900`, `0x5c8890`).

**StoppedVsStopped** (`0x686500`):
- Gates: landing transport and garrison host, both sides ("Pair gates" below); no garrisoned-state and no hover test.
- An Infantry-class unit that is not a team container (no TEAM slot), beside a Large or Giant Vehicle, needs a
  non-zero push vector (`0x5c6d40`).
- Then a footprint-box overlap ratio (`0x20d7a0`) > **MinimumStoppedVsStoppedOverlapCoefficient = 0.15**.

**Pair gates (s78n follow-up, 2026-11-10)** [H unless tagged]. Read from `0x508170`, `0x683790`, `0x442540`
(the MovingVsMoving predictor), `0x686500`, `0x644280`, `0x443d10`, `0x444310`, `0x61e930`, `0x42d6a0`. A pair that
fails a gate gets **no collision object**. The detection filters of §5.2 item 5 run first (self, no locomotor on
either side, **either side +0x3a7**, build pad holding the other, existing record, class table); then per test:

| gate | no collision object when | MovingVsStopped | MovingVsMoving | StoppedVsStopped |
|---|---|---|---|---|
| landing transport | X has TRANSPORT_LANDING (0x36, slot +0x368) and `0x644280`(X's behaviour, other) is false: X's TransportDataPack (obj +0xc8) state +0x50 is 3 (taking off), or `other` is in its unloaded list (+0xa8, count +0xb0) | X = the obstacle only | not tested | X = either side |
| garrison host | X has GARRISON_UNIT (0x51, slot +0x383) and a GarrisonDataPack (obj +0x210) whose host id +0xc is the other object's id | either side | either side | either side |
| garrisoned | X has GARRISON_UNIT and its pack state +8 is 7 (inside a host): no object with anything | either side | not tested | not tested |
| raised hover | locomotor pack +0x2a8 == 1 (raised to `Hover_Height`; 0 grounded, 2 rising, 3 lowering) | a raised mover needs a raised obstacle (a grounded mover takes any obstacle) | both raised or both not raised | not tested |

- **Garrison host id.** +0xc is set when the garrison move starts (`0x444310`: only from state 0; state 1, +0xc := host)
  and stays through 2 (at the approach point), 5 (entering), 7 (inside), 3 (eject requested) and 4 (exiting). So a unit
  takes no collision object with **its own host** from the order until the id is cleared, in all three tests.
  **When it is cleared (s78n t18 RE, 2026-11-10; `0x443d10`, `0x443ff0`, `0x443560`, `0x563d00`, `0x444dd0`,
  `0x38f5d0`, `0x38f770` read in full) [H]:** state := 0 and +0xc := 0x3FFFFF happen in two places only.
  1. The unit's signal handler `0x443d10`, on **signal 10** (formation arrival, A1_READINGS §33.4) in any state other
     than 1 or 2: in state 4 that is the arrival of the walk-out move. It needs the unit to be still subscribed to
     signal 10. The order `0x444310` subscribes (10 and 0x10); the per-frame service `0x443ff0` **unsubscribes 10**
     when it starts the entering itself (states 1-2, 3-D distance − radius ≤ the host's `Garrison_Radius`, the usual
     case: the test passes while the unit is still walking); only an entering started by the handler (approach move
     finished → state 2 → `0x443700`, or the next signal 10 in state 2) keeps the subscription.
  2. The cancel `0x443560` (unsubscribe 10 and 0x10, un-reserve, host vf +0xf8 when the state was 2 or 4, pack reset
     `0x563d00`). Callers: the handler (signal 0x10 = the unit died; host free slots < the unit's value; 10 fruitless
     approach retries; the host is moving), the service (capacity), `Leave_Garrison` while approaching (`0x57c880`),
     and **every new order of the unit**: `0x38f770` from the group move `0x2a7b80` (the common path of the move,
     attack and ability order events), `0x38f5d0` from the queued waypoint move `0x2a8240`, from the order events
     `0x3af790` / `0x3afe60` [M: which events], the EscortEvent `0x3b0040`, Lua `Attack_Target` `0x5730f0` and Lua
     `Move_Internal` `0x57da90` [M: whether the Stop order reaches one of them]. (A garrison order runs the group
     move, so the cancel, first and then `0x444310`.)
  The eject `0x444dd0` sets state 4 and gives the walk-out move straight to the coordinator (`0x4f8200`, flags 0x20),
  which does not run the cancel. State 4 itself only fades the model in (`0x443ff0`). So after leaving, the host id
  lasts **until the walk-out move arrives** (unit still subscribed) **or, for the usual unsubscribed unit, until its
  next order or death**: standing idle beside its former host it still forms no collision object with it. A unit in
  state 4 cannot take a new garrison order through `0x444310` until then; the group move in front of it clears it.
  *(s78n t19, 2026-11-10: the approach in full, i.e. the approach point, the 60.0 test, the corrective move, the
  retry counter (it lives on the behaviour, +0x40, and the cancel does not reset it) and the host-moving cancel, is
  FIGHTER_FLYING §7.4 "Garrison approach in full". `0x443700` also **stops the host**.)*
- **Hover state (+0x2a8): writers and initial value (s78n t18 RE) [H].** Byte scan of every store to `[reg + 0x2a8]`
  plus the decompile; no other writer exists (Flying, JetPack, Bike and the team locomotors never write it).
  - Data-pack constructor `0x553f00`: **0** (grounded). The field is saved and loaded (`0x557ca0` / `0x556dc0`).
  - Walk Start Move, state 0x12 (`0x61ff40`): type `Hover_Height` (+0x23bc) > 0 and the state ≠ 1 → := **2** (rising),
    and the frame ends there.
  - Walk service `0x61e930` runs its state machine only while +0x2a8 < 2. In 2: z += effective max speed
    (`0x370f00`) per frame until terrain + `Hover_Height`, then := **1** (raised) and speed := 0. In 3: z −= max speed
    until the terrain, then := **0** and speed := 0. After that the Start Move state runs again.
  - GARRISON_HOVER_TRANSPORT vf +0xe0 `0x635ee0` (a unit reserves a slot: garrison order `0x444310`) and vf +0xe8
    `0x635fc0` (eject request `0x444d90`): := **3** (lowering).
  - FoC types with `Hover_Height`: `Lancet_Air_Artillery` and `Gallofree_HTT_Transport` (both 80). So such a unit
    spawns grounded, is raised from its **first move** on and is never lowered again, except the HTT, which lowers
    whenever a unit is ordered into it or asked to leave and rises again at its next move.
  - *Port spec:* per hover unit a state {grounded, rising, raised, lowering}; `raised` = it has completed the rise of
    its first Start Move since spawn / since the last lowering. Rising and lowering count as "not raised" in the gates.
    Apply the table row: MovingVsMoving only between equal raised states; a raised mover's MovingVsStopped only
    against a raised obstacle; a grounded mover takes a raised stopped unit as an obstacle; StoppedVsStopped ignores
    the state.
- **Hover in full: z, the type list, every reader (s78n t19 RE, 2026-11-10) [H unless tagged].** Read from
  `0x61e930`, `0x61ff40`, `0x42d6a0` / `0x42bb40`, `0x635ee0`, `0x635fc0`, `0x391260`, `0x61d300`, `0x687010`,
  `0x567c50`, `0x5662b0`, `0x38d730`, `0x391310`, and both XML archives with comments stripped.
  - **Type list (corrects nothing: the two-type list above is right).** Only `Lancet_Air_Artillery`
    (UNITS_LAND_EMPIRE_LANCET.XML) and `Gallofree_HTT_Transport` (UNITS_LAND_REBEL_GALLOFREE_HTT.XML) carry
    `Hover_Height` (80.0 both): WALK_LOCOMOTOR, `UnitCollisionClass` Vehicle, `MovementClass` Lancet, `Space_Layer`
    Land; only the HTT has the garrison-hover behaviour and `Deploys`. EaW's files have none and no type inherits
    the tag through `Variant_Of_Existing_Type`. **`MAL_Rocket_Vehicle` does not hover:** line 35 of
    UNITS_LAND_UNDERWORLD_MAL.XML is `<!--<Hover_Height>3.0</Hover_Height> -->`, inside a comment (the designer
    note says the tag stops melee units attacking it and the low hover is faked in art). Its `MovementClass` is
    `Hover`, which is only a passability class. So type +0x23bc is 0 for the MAL and `MAL_Captured`, and every
    reader gates on > 0.
  - The XML behaviour token is `GARRISON_HOVER` (behaviour 0x56, object slot +0x388); "GARRISON_HOVER_TRANSPORT"
    above is the class name in an error string.
  - **Rising (2).** Entered by the first test of Start Move 0x12 (`0x61ff40`), before any path test: `Hover_Height`
    > 0 and state ≠ 1 → := 2, frame ends (a turn in place 0xf-0x11 therefore happens on the ground first; the pack
    state stays 0x12 through the rise). With +0x2a8 ≥ 2 the Walk state machine does not run at all: **no turn, no
    speed step, no horizontal move**. Per frame: T = the heightmap's bilinear height (`0x135140`) at the unit's
    (x, y); target = T + `Hover_Height`. z ≥ target on entry → z := target, state 1, speed := 0, frame ends. Else z
    += the effective max speed (`0x370f00`, all multipliers); z > target → z := target, state 1, speed 0; z ≤ target
    → still rising. (Landing exactly on the target costs one more frame. HTT: 80 / 2.5 = 32 steps, raised on frame
    33, first move step on frame 34. Effective max speed 0: the rise never ends.)
  - **Raised (1).** The position commit `0x42d6a0` writes **z = G + `Hover_Height`** on every committed move, no
    smoothing; G (`0x42bb40`) is the conformed ground height a grounded Walk unit gets (§3.6). Pitch / roll conform
    as on the ground. Standing still, z is not rewritten.
  - **Lowering (3).** Per frame: z ≤ T → z := T, state 0, speed 0; else z −= effective max speed, clamped at T with
    state 0 and speed 0. Writers `0x635ee0` (slot reserve) and `0x635fc0` (eject request): both first **stop the
    host** (`0x3a9880`: locomotor stop, removed from its coordinator), raise signal 0xb and call `0x391260`, which
    **deploys** a `Deploys` type (Walk vf +0x118(1) = `0x61d300`: state 0x18 Deploying). So the HTT lowers with its
    state machine frozen and then continues in 0x18 → 0x19 (Deployed), not in Start Move; its next move undeploys
    (0x1a → 0) and state 0x12 sets rising again. *s78n t26 RE 3 (2026-11-10; withdraws the s78n t26 RE 2 note that a
    move order does not undeploy it) [H]:* the HTT is **not** a manual deployer: it has no `Auto_Deploys` tag and the
    type default is true (§3.2.1 table, `0x367100`). So the group move admits it while deployed, the formation plan's
    member loop calls `0x3ac100`(HTT, path length) and, with M = 0, `Deploy(false)`: a move order undeploys it
    (0x1a → 0) and it then moves, as written above. The garrison hooks `0x636090` / `0x6360b0`
    (GarrisonHoverTransportBehaviorClass slots +0xF0 / +0xF8: after an entry with no reservation left, at an enter
    cancel or at the end of an exit → `0x3ac100`(host, 0) = `Deploy(false)` for M = 0) also undeploy it; it has no
    DEPLOY ability. (This corrects "after that the Start Move state runs again" above for the lowering case.) The same holds for the non-hover garrison hosts `HAV_Juggernaut` and
    `F9TZ_Cloaking_Transport` (auto-deployers, M = 0), whose deploy at slot reserve / eject request goes through
    `0x635f60` / `0x636030` (GarrisonVehicleBehaviorClass; stop, signal 0xb, `0x391260`) [M: slot meaning by
    symmetry with `0x635ee0` / `0x635fc0`].
  - **Readers of the state / the tag** (decompile scan for +0x2a8 and 0x23bc): the Walk service, Start Move and
    commit above; the pair gates `0x683790` / `0x442540` (table above, confirmed); the StoppedVsStopped chooser
    `0x687010` (a lowering unit is never the one moved, §5.4.3); the targeting filters `0x567c50` / `0x5662b0` and
    the can-attack test `0x38d730`: a **melee attacker** (`Targeting_Max_Attack_Distance` ≤ `Melee_Cutoff_Range`,
    51) rejects a target with `Hover_Height` > 0 in state 1; death `0x391310`: a GARRISON_HOVER host that is not
    grounded (state ≠ 0) does not eject, its occupants are destroyed (`0x379df0`); the UI garrison / eject commands
    `0x443400` / `0x4432d0` [M]. No reader in fog, projectile, selection or pathfinding code, none in the garrison
    service (an entering unit does not wait for the host to be down; only its range test delays it [M]).
  - **What a MAL collides with:** it is always grounded, so the raised-hover gate never excludes it on its own
    side. As mover: MovingVsStopped against any stopped unit, a raised Lancet / HTT included. As stopped obstacle:
    it blocks grounded, rising and lowering movers; a raised mover ignores it. MovingVsMoving: with every
    non-raised mover, never with a raised one. StoppedVsStopped: with anything. (Were the 3.0 tag live it would
    behave like the Lancet row: z + 3 after a 3-frame rise at its first move, immune to melee attackers.)
  - *Port spec:* hover exists only for types with `Hover_Height` > 0 (FoC: Lancet, HTT; **not the MAL**). At a move
    start with state ≠ raised: enter rising and end the tick. Rising / lowering: skip the whole locomotor step; z
    moves by the effective max speed per tick toward heightmap(x, y) + H (rising) or heightmap(x, y) (lowering),
    clamped; the tick that reaches or passes the target sets the new state and speed 0; an exact landing takes one
    more tick. Raised: every position commit sets z = conformed ground height + H. Slot reserve and eject request
    on a GARRISON_HOVER host: stop it, start Deploying if the type `Deploys`, enter lowering. Melee attackers
    cannot target a raised unit; a hover host dying while not grounded kills its occupants; a lowering unit is
    never the one displaced in StoppedVsStopped.
- **Squads.** (Capacity, approach radius and the squad's host-id lifetime: FIGHTER_FLYING §7.4, 2026-11-10.) The team
  container carries GARRISON_UNIT and garrisons as one object (FIGHTER_FLYING §7.4), so the pair
  "container vs the host its squad is walking to, entering, inside or leaving" is exactly the garrison-host gate. While
  inside, the container and every member have +0x3a7 (and are out of the tracking layer), so no pair forms at all.
  There is no "some members garrisoned" state to handle.
- **No attachment and no team test.** None of `0x508170`, `0x683790`, `0x442540`, `0x686500` reads
  `AttachParentObject` (+0x2e0), `ParentContainerObject` (+0x2b0) or a TEAM list. The only holder / content exclusion
  is the build pad of §5.2 item 5 (pad pack obj +0x150: +8 under construction, +0x10 built). A container and its own
  members never meet in a test because members are never candidates (FIGHTER_FLYING §7.8).
- *Port spec:* before creating a collision object for (a, b): skip when either is hidden / garrisoned (+0x3a7); skip
  when either one's garrison host is the other, in any garrison state from "moving to host" to "exiting" (all three
  tests), where "exiting" lasts until the walk-out move arrives or the unit's next order, whichever the unit's
  subscription allows ("Garrison host id" above; "until the next order, or the walk-out arrival if earlier" is a
  safe port rule; a **new order clears it at once**). A gate that starts later does not by itself cancel a live
  record: the gates sit inside the contact tests, so a record ends on them only where its state re-runs the test
  (§5.4.1 decision step 1 and state 4; §5.4.2 states 0, 2, 6, 12); a garrison entry stops the unit (path stack
  cleared, locomotor suspended), which ends its Repulsion / MovingVsMoving records through their normal end tests.
  MovingVsStopped also skips when either is in the inside state; skip a landing transport that is taking off
  and the pair (landing transport, a unit it unloaded) (as obstacle in MovingVsStopped, either side in
  StoppedVsStopped); hover: MovingVsMoving needs equal "raised" state, a raised mover's MovingVsStopped needs a raised
  obstacle. No parent / child or own-team rule.

**Footprints:** soft radius `Custom_Soft_Footprint_Radius` (+0x16b8); hard box `Custom_Hard_XExtent`/`YExtent` (+0x16bc/+0x16c0) with offsets (+0x16c4/+0x16c8) and deployed variants (+0x16cc/+0x16d0); `MovementBoxExpansionFactor` (+0x16d8). How the three tests use the hard box: §5.3.1 (2026-11-10). The soft radius is the "radius" `0x397780` of a unit that is not a team container (§5.3.1 item 6). [H]

**When the deployed pair is used (s78n t19 RE, 2026-11-10; `0x396850`, `0x396b70`, `0x371d20`, `0x372100`,
`0x39a390`, `0x39a400`; also TACTICAL_COMBAT §5.4.6, A1_READINGS §35.4) [H]:**
- **Switch** (in `0x396850` and, separately, in `0x396b70`): the type has `Deploys` (+0xd1), the object has a
  locomotor, and it is **deploying** (`0x39a400`: locomotor vf +0x130, Walk state 0x18) or **deployed**
  (`0x39a390`: vf +0x120, Walk state 0x19, or pack +0x2b3 set = walking while deployed). Undeploying (0x1a) uses
  the normal extents; non-Walk locomotors answer false; a land team container takes its joint slot box before the
  switch is looked at.
- **Rectangle (`0x396850` → `0x371d20`):** the deployed pair × `Scale_Factor` replaces (hx, hy) **only when
  `Custom_Hard_XExtent_Deployed` > 0** (the Y branch tests the X field too, at 0x371da6); otherwise the normal
  `0x371be0` pair (the customs × scale; the model's box per missing axis; (0, 0) without a model). X set with Y
  absent gives hy = 0. The offsets never switch (`0x372020` / `0x3721f0`).
- **Helpers:** `0x3969f0` = √(hx² + hy²) of `0x396850`, so it follows the rectangle rule; the half width
  `0x396b70` (lanes, the slave-speed side test, push clearances) uses the per-axis `0x372100`: **`YExtent_Deployed`
  > 0 alone is enough**; `0x397780` has no deployed switch.
- **FoC data:** no type sets `Custom_Hard_XExtent_Deployed`. `Custom_Hard_YExtent_Deployed` 24.0 is set by `MPTL`
  (scale 1.1; inherited by `MPTL_Captured`, the two MPTL death clones, `CIN_MPTL_Intro`, `FIN_MPTL`) and by
  `MAL_Rocket_Vehicle` (scale 0.7; inherited by `MAL_Captured`). So **the collision rectangle `0x38de80` and the
  radius `0x3969f0` never change in FoC**; only the half width `0x396b70` does while deploying or deployed (MPTL
  16.5 → 26.4, MAL 10.5 → 16.8).
- *Port spec:* rectangle = the normal hard extents in every state (use the deployed pair only for a type that sets
  the deployed **X**, while deploying / deployed / walking deployed). The "half width across" value = scale ×
  `Custom_Hard_YExtent_Deployed` when that tag is > 0 and the unit is deploying / deployed / walking deployed, else
  scale × `Custom_Hard_YExtent`.
- *Correction to TACTICAL_COMBAT §5.4.6 item 4.3:* "`Max_Dist_Walk_While_Deployed` > 0, which no FoC unit sets" is
  wrong: `Destroyer_Droid` and `Destroyer_Droid_Attack` set 150.0, so the walking-while-deployed flag +0x2b3 is
  live; they have no deployed extents, so their footprint is unaffected.

#### 5.3.1 Footprint geometry of the three contact tests (s78n t18 RE, 2026-11-10) [H unless tagged]
Read from `0x38de80`, `0x396850`, `0x20d080`, `0x20d140`, `0x20d690` / `0x20d710`, `0x20d7a0` / `0x20d830`,
`0x20dc10` / `0x20dc90`, `0x441b60`, `0x4422f0`, `0x442540`, `0x686500`, `0x67f130`, `0x684710`, `0x685e10`,
`0x5c71a0`, `0x397780`, `0x3969f0`.

**The rectangle `0x38de80`(obj)** (layout of `0x20d080`: axis a at +0, axis b at +8, centre at +0x10, half extents at
+0x18): a = (cos yaw, sin yaw), b = (−sin yaw, cos yaw), centre C = P + ox·a + oy·b, half extents (hx along a, hy
along b).
- (hx, hy) = `0x396850`: for a land team container (TEAM behaviour and locomotor vf +0x230) the **joint slot box**
  (HX, HY) of `0x404450` (TACTICAL_COMBAT §5.4.4; live member positions are not read); for any other object the
  type's hard extents × `Scale_Factor` (`0x371be0`: a missing extent falls back to the model's local box), or the
  deployed extents (`0x371d20`) while deployed / deploying.
- (ox, oy) = `Scale_Factor` × (`Custom_Hard_XExtent_Offset`, `Custom_Hard_YExtent_Offset`) of the object's own type
  (`0x372020` / `0x3721f0`; a container uses its own type's, 0 in FoC).

**Primitives on two rectangles** (Δ = C2 − C1; R(rect, n) = |a·n|·hx + |b·n|·hy, `0x20d140`):
- **overlap** `0x20d690`: |Δ·n| ≤ R1(n) + R2(n) on all four axes n ∈ {a1, b1, a2, b2} (separating-axis test;
  touching counts as overlap);
- **overlap coefficient** `0x20d7a0` = min(1, min over the four axes of 1 − |Δ·n| / (R1 + R2)); an axis on which the
  two are apart, or with R1 + R2 = 0, gives 0;
- **gap** `0x20dc10` = max over the four axes of max(0, |Δ·n| − (R1 + R2)) (0 when they touch or overlap).

**The tests:**
1. **StoppedVsStopped** `0x686500`: overlap coefficient(rect a, rect b) > `MinimumStoppedVsStoppedOverlapCoefficient`
   (0.15). A true rectangle-rectangle test; a squad enters with its joint box rectangle.
2. **MovingVsStopped** `0x441b60`(a, b, a's path approximation pack +0x200, out d): **not** a rectangle test. Each
   footprint is reduced to a **directional radius** ρ(h, c) = hy + (hx − hy)·c², c = |cos| of the angle between the
   rectangle's forward axis and the line to the other party. Cb = b's rectangle centre (b's offsets applied), f_b =
   (cos yaw_b, sin yaw_b). For each approximation segment A → B (t = its unit direction), at each end E ∈ {A, B}:
   u = normalise(Cb − E), R_E = ρ(h_a, |u·t|) + ρ(h_b, |u·f_b|). A point moving A → B against the fixed point Cb with
   the contact radius running linearly R_A → R_B (`0x4422f0`, in doubles): the first s ∈ [0, 1] with
   |A + s·(B − A) − Cb| ≤ R_A + s·(R_B − R_A); already inside at s = 0 → s = 0. d = path length from the start of the
   approximation to that point (`0x54f9d0`).
   - a's forward axis is its **path segment direction**, not its facing; a's own offsets are not used (a is its path
     point). On a zero-length segment while a turns in place (vf +0x1b0): (hx_a, hy_a) both move toward max(hx_a,
     hy_a) by k = clamp(|heading error| / 90°, 0, 1) (the box swells to a square while it turns) and t = a's facing.
   - Contact distance (centre a to Cb): head-on hx_a + ρ_b; beside the path hy_a + ρ_b, where ρ_b is hx_b seen
     end-on, hy_b seen from the side and the cos² blend in between. Corners are rounded off: a diagonal approach
     gives less than the rectangle's corner distance.
3. **MovingVsMoving** `0x442540`(p, q, T, margin) (T = `MovingVsMovingLookAheadTime` 1.5 s; margin 0 at every call
   seen). The same directional radii, on two moving points.
   - Gates first (garrison host either side, equal hover state, §5.3 table). T += 1.5 s when either can crush the
     other (`0x5c8900`) and that other scatters (`0x5c8890`).
   - Assumed speeds, with dir = normalise(q.P − p.P): s_p = effective max speed when p faces toward q (f_p·dir ≥ 0),
     else p's current speed (pack +0xec); s_q = max speed when f_q·dir ≤ 0, else q's current speed.
   - Both approximations are walked in step. A step ends when the first of the two reaches its next approximation
     point (dt = that time; + 0.5 frame when both are in mid-segment); a unit at the end of its approximation stands.
     Per step, with starts S_p, S_q and ends E_p, E_q: t_p = normalise(E_p − S_p), t_q likewise; u_s = normalise(S_q −
     S_p), u_e = normalise(E_q − E_p) (fast inverse square root); r_p = ρ(h_p, |u_s·t_p|) at the start and ρ(h_p,
     |u_e·t_p|) at the end, r_q likewise with t_q. **Lane rule:** when the other party is at least hy to the side of
     this one's travel line at the start (|cross(t, other − self)| ≥ hy) and stays on the same side at the end, this
     one's radius is hy at both ends. (A standing unit has t = 0, so its radius is hy.) R = max(0, r_p + r_q −
     margin) at each end; contact = `0x4422f0` on the relative motion with R_s → R_e.
   - Time is counted in whole frames (each dt truncated). Result: contact at elapsed + trunc(dt·s) < trunc(fps × T)
     (45 frames; 90 for a crusher pair). With both approximations empty of motion at the very start the result is
     the rectangle **overlap** `0x20d690` of the two footprints.
4. **Slave-speed touch** (`0x67f130`, §5.4.2): gap(rect x, rect y) = 0.
5. **Destination tests** (`0x684710`, `0x685e10`, §5.4.1): rectangle overlap of the mover's rectangle placed at the
   tested point against each stopped unit's rectangle; `0x685e10` uses half extents (hx + 5, hy) turned along the
   path, where (hx, hy) are the mover **type's** extents `0x371be0`, not `0x396850` (no joint box for a container:
   §5.4.4).
6. **Radii used by the "is it behind me" ends and the side-steps:** `0x3969f0` = √(hx² + hy²) of `0x396850` (a
   container: √(HX² + HY²)); `0x397780` = the same for a land team container, but for any other unit `0x373b30` =
   `Scale_Factor` × the **soft** footprint radius (`Custom_Soft_Footprint_Radius`, else `Space_Obstacle_Radius`,
   else the larger model x / y half extent; §2.2). (FIGHTER_FLYING §7.5 calls `0x397780` "hard radius"; that is
   right only for containers.)

***Port spec:*** footprint of a unit = oriented rectangle (hx, hy, yaw, offset); a squad container = its joint slot
box. (a) StoppedVsStopped: the separating-axis overlap coefficient above > 0.15. (b) MovingVsStopped: walk the
mover's path polyline; per segment solve the swept-point test with R(E) = ρ(h_mover, cos to the segment direction) +
ρ(h_obstacle, cos to the obstacle's facing) at both ends, linear in between; the contact distance is the path length
to the first solution. (c) MovingVsMoving: the same radii for both movers along their predicted polylines at the
assumed speeds, with the lane rule, inside 45 frames. A circle unit is the case hx = hy. The plan's box-extent sums
(L = ext_a(t) + ext_b(t), W = ext_a(n) + ext_b(n)) are **not** retail: they keep the corners that retail rounds off
and use the mover's facing where retail uses its path direction.

#### 5.3.2 Tracking layer 0x10: what a record stores, and the occupancy query `0x538200` (s78n t18 RE) [H unless tagged]
Read from `0x53a400`, `0x539b20`, `0x5d8620`, `0x5d8140`, `0x6b4810`, `0x6b5cb0`, `0x6b3b00`, `0x6b0e50`,
`0x6b2fb0`, `0x5d7a00`, `0x5d7ae0`, `0x538200`, `0x5093c0`, `0x6420e0`, `0x5e9f30`.
- **Record** (`0x6b3b00`; 0x5c bytes) for a land mover: +0x58 object id; +0x00 flag; +0x04 centre at the slice
  start, +0x0c centre at the slice end (both = the object's current position on land, plus its rotated hard-extent
  offsets); +0x14 r = √(hx² + hy²); +0x18 / +0x1c half extents (hx, hy) = `0x396850` (**the same footprint rectangle
  as `0x38de80`: a squad container's joint slot box**; + a margin argument that is 0 at the layer builds seen); +0x20
  axis a = the object's facing (cos yaw, sin yaw) (the travel direction when start ≠ end); +0x28 the axis-aligned box
  centre ± r (what box queries test). It is a **rectangle**, not a circle; the circle r only bounds it.
- **Flag** on land: **2 (Static)** unless the object has a locomotor that is "moving" (vf +0x1a8 `0x42cc60`: not
  suspended and (speed > 0 or a path on its stack)), then **1 (Moving)**. (The asteroid / nebula type bits of §2.2
  can replace it; no land unit has them.)
- **Freshness:** the tracker update `0x539b20` (once per mode frame) stores each mover's current position
  (`0x53a400`) and then re-fits **every record of every slice** from it (`0x5d8620` → `0x6b5cb0` → `0x6b3b00`), so
  rectangles and flags are current each frame. Which objects are in the layer changes only at a full rebuild
  (`0x5d8140`): when a mover first appears or changes layer (that frame), when one is removed (`0x539470`: hidden /
  garrisoned / destroyed), and every 30 frames (tracker timer +0x6c) [M: order of this update against the collision
  update inside the frame was not traced].
- **Box query** `0x5d7a00` → `0x6b0e50`(box x, y, w, h): every object whose record box (centre ± r) overlaps the box
  (strict inequalities), each once. Used for collision candidates (160 × 160, §5.2), `0x684710` / `0x685e10`
  (160 × 160 around the path end) and the push-spot occupancy (A1_READINGS §40.2).
- **Occupancy query `0x538200`(tracker, rect, ignore A, ignore B)**: land mode only; layer 0x10; → `0x5d7ae0` →
  `0x6b2fb0`: the **OR of the flags** of every record, other than the two ignored ids, whose rectangle (centre,
  (hx, hy), axis) **overlaps** the query rectangle (separating-axis `0x20d690`). Callers:
  - JetPack landing (`0x6420e0`, REMAINING_GAPS §4): rect = the lander's own footprint rectangle (`0x38de80`) moved
    to the landing point; nothing ignored (0x3FFFFF twice; the flying lander is on layer 4, §2.2); **any** flag,
    Moving or Static, refuses the spot.
  - `0x5093c0` (a path-corridor test): per path segment a rectangle of half length = half the segment and half width
    **0**, ignoring the pathing unit; blocked only by a **Static** record (flag & 2).
  - hero-clash move `0x5e9f30`: the unit's rectangle, ignoring its own id.
- So layer 0x10 holds, per registered land mover (FIGHTER_FLYING §7.8: containers yes, squad members never), one
  rectangle. A landing spot inside a squad's joint box is occupied; a spot where only a squad member stands (outside
  no other record) is free.

***Port spec:*** one predicate "layer object" (registered mover, Land layer, not hidden, FIGHTER_FLYING §7.8) with
its footprint rectangle of §5.3.1 and a moving / static flag (static = speed 0 and no path). `land_spot_free` =
no layer object's rectangle overlaps the lander's rectangle at the spot (moving ones count too). Candidate and
push-spot box queries test the bounding box centre ± √(hx² + hy²). The plan's [port] "treat `0x538200` as a
layer-0x10 query" is confirmed.

### 5.4 Resolution [H] for the branch order, [M] for meanings
*(s78n t19, 2026-11-10: StoppedVsStopped in full §5.4.3; the early-arrival cut §5.4.4; the decision's target clause,
death clones, hero clash and crushers §5.4.5; MovingVsMoving for vehicle pairs §5.4.6; the Path Around call §1.5.1.)*

**State names** (debug strings):

| class | states |
|---|---|
| MovingVsStopped | 0 None, 1 Path Around, 2 Repulsion, 3 Existing Path, 5 Resolved, 6 Unresolvable, 7 Try Again Later (4 and 8 unnamed) |
| MovingVsMoving | 0 None, 1 Path Around, 2 Speed Slave Only, 3 Repulse, 4 Pass, 5 Infantry Push, 6 Moving With Speed Only, 8 Resolved, 9 Unresolvable, 10 Try Again Later |
| StoppedVsStopped | 0 None, 1 Resolving, 2 Resolved, 3 Unresolvable, 4 Try Again Later (re-check frame at +0x80) |

**MovingVsStopped decision** (`0x684f90`), first match wins. *(This list is the 2026-10 summary; steps 1 and 2 are
imprecise. The exact conditions, the per-frame driver, when a record ends and the MovingVsMoving state machine are
in §5.4.1 / §5.4.2, 2026-11-10, which replace it where they differ.)*
1. Infantry vs infantry, or a mover that is slower or stopped and blocked by its own target → **Repulsion**.
2. The obstacle is a death-clone obstacle (type +0xcf) or has a locomotor, and its movement state (+0x5c) is set:
   - mover not infantry: **push** it (`0x6859a0`) → Resolved, or Try Again Later;
   - infantry vs infantry: Repulsion.
3. Giant Vehicle vs infantry or bike, when it cannot push → push/crush attempt (`0x6859a0`).
4. The mover cannot push and side-stepping applies (`0x684710` → `0x6835c0`, push vector `0x5c6ba0`) → try a re-route (`0x685e10`): **Resolved** on success, otherwise **Path Around** (`0x685920`, via the dynamic pathfinder §1.5).
5. Enemy pairs (faction test `0x2824f0`), or bike vs infantry → push the blocker aside (`0x6859a0`).
6. The mover can push → state 4.
7. Vehicle or larger (class > 6) → Path Around, or `0x685490`.
8. Otherwise → **Try Again Later**.

**Speed slaving:** the collision system's speed cap (`0x509ea0`) is applied in the walk speed controller (§3.5). This is how "Speed Slave Only" and "Moving With Speed Only" take effect. [M] The values each record answers (vf +0x40), the "at once" flag and which record's flag wins: FIGHTER_FLYING §7.7 item 5 (2026-11-10).

**Processing order:**
- Each frame, active collisions are sorted by a priority float and resolved in that order under the dynamic quota (§1.5).
  The driver in full: §5.4.1.
- The MovingVsMoving resolution methods (`0x67f7b0…0x683260`) are traced in §5.4.2 (2026-11-10).

**Push `0x6859a0` and the pushed unit (a1k t03, A1_READINGS §36.4 / §40)** [H]:
- Direction d = the pusher's heading turned ±45° toward the blocker's side.
- Free-position search `0x440f90` (set up by `0x440be0`, cone mode 2): rings around the blocker, radius 0, 1, 2.8, 5.4, …
  (step +0.8 per ring up to min(1.5 r, 10), r = footprint radius √(hx² + hy²) of the hard extents), at most 20 rings, n = trunc(R·2π / r) + 1 samples
  (≥ 4 after ring 0) at angles θ = 2πi / n from d. Only θ ≤ π/4 is sampled at the ring radius (the rest at 0.33 from
  the blocker). First sample in that order that is inside the map box, **reachable** (same connected region for its
  movement class, `0x50ecd0`), **unoccupied** (no other layer object within r + 5 × `FormationSpacing` + its
  directional radius; exact test: §5.4.3 "Occupancy", 2026-11-10) and
  **off the paths** of the movers in its MovingVsStopped collisions (clearance = mover hard Y extent + r + 4) wins. None →
  the push fails.
- The blocker gets a land path to the spot as a new record on top of its **path stack** (pack +0x138, kind 4, or 7 when
  the pusher could crush it); its own records stay underneath. When the pushed path ends it is popped and an
  unfinished path below resumes from its waypoint index; an idle unit stays at the spot.
- **There is no return move:** `MinimumPushReturnDistance` (type +0x3cc) and `RepushDistance` (GameConstants +0x1494)
  have no reader in the binary.

#### 5.4.1 The driver and MovingVsStopped in full (s78n t18 RE, 2026-11-10) [H unless tagged]
Read from `0x50a780`, `0x679740`, the four vftables (`0x8b5090`, `0x8b52e8`, `0x8b5450`, `0x8b5568`), `0x684cf0`
(with its jump table `0x684f64`), `0x684f90`, `0x684450`, `0x683ee0`, `0x684660`, `0x6845c0`, `0x684710`, `0x6835c0`,
`0x5c6ba0`, `0x685e10`, `0x684100`, `0x685920`, `0x685490`, `0x6859a0`, `0x554310`, `0x50a020`, `0x5c7cb0`,
`0x61d890`, `0x61d570`, `0x620820`, `0x41dbb0`, and a scan of every `call` / `jmp` to `0x509810`.

**Virtual slots of a collision record** (all four classes): +0x18 service (per state); +0x20 decide; +0x28
"finished"; +0x30 weight; +0x38 steering vector; +0x40 speed and "at once" flag; +0x48 kind (Terrain 1, SvS 2, MvS
3, MvM 4); +0x50 "takes the quota pass"; +0x78 "re-decide when my partner's path changes".
**vf +0x50 is not a keep test** (it was read as one before): MvS always true; MvM true only in state 0; SvS false;
Terrain true in state 0. **The end test is vf +0x28:** MvS state 5 or 6, MvM state 8 or 9, SvS and Terrain state 2
or 3.

**Per-frame driver `0x50a780`** (then the detection `0x508170`):
1. Every record, in record-id (creation) order: vf +0x50 false → vf +0x20 at once (for a record whose state ≠ 0
   that is its service); true → collected.
2. The collected ones are sorted by the priority float; each gets vf +0x20 (decide) while the dynamic pathfinder's
   used complexity (+0x13c) is below `DynamicLandComplexityQuota` (`0x679740`), else vf +0x18 (service). A decide
   call on a record whose state ≠ 0 just runs its service. So **every record is serviced every frame**; only a
   state-0 decision waits for quota (a state-0 MvS service does nothing).
3. Every record whose vf +0x28 is true is unlinked (`0x50a2c0`) and destroyed.
- **Re-decide stamp (record +0x70):** every service starts with "+0x70 ≠ 0 and ≤ NOW → +0x70 := 0, each
  participant's locomotor gives up the record id (vf +0x180: pack +0x248 := 0 when it is this record), state := 0".
  Its only writer is the path-push hook: `0x554310` (a path record is pushed on a unit's stack; it also zeroes the
  approximation countdown pack +0x275 and stamps pack **+0x288 := NOW**, as the pop `0x557b10` does) → `0x50a020` →
  `0x5c7cb0`: +0x70 := NOW + 2 when vf +0x78(partner) is true. vf +0x78 is false for MvS, SvS and Terrain; for MvM
  it is true in state 1 when the partner is the slave and in state 4 when the partner is the master. **A new path of
  either unit never ends or resets a MovingVsStopped record.**
- **The stamp in full (s78n t23 RE, 2026-11-10; `0x554310`, `0x50a020`, `0x5c7cb0`, `0x5c7100`, `0x680ef0`,
  `0x682d00`, `0x67f7b0`, `0x6811c0`, `0x6814f0`, `0x5c6970`, `0x42cdd0`, `0x554480`, `0x41c1a0`, `0x3639d0`) [H
  unless tagged].**
  1. **Order inside the push `0x554310`(pack, path, unit):** the record is appended to the stack, pack +0x275 := 0,
     then the stamp `0x50a020`(system, unit), then `0x559260`, pack +0x288 := NOW, pack +0x1e8 := 0. `0x50a020` is
     called from nowhere else, and `0x554310` only from the locomotor's vf +0x190 `0x42c680` (every land locomotor;
     the team container's `0x5ad140` ends in it). Pops and removals (`0x557b10`, `0x559410`, `0x554480`) stamp
     pack +0x288 but **never** +0x70.
  2. **Who is stamped:** `0x50a020` takes every record listed for the unit; for each, `0x5c7cb0` takes the partner
     = the first other participant (`0x5c7100`) and calls vf +0x78(record, partner), which compares **live**
     fields: state +0x78 = 1 and partner id = +0x80 (S), or state = 4 and partner id = +0x7c (M). There is no
     separate "partner is master" byte. So the new path has landed on **M in state 1** (the master that holds at
     its line) or on **S in state 4** (the passed unit that holds at line B): in both states the unit that is
     *held*. (A phrasing "unit = slave in state 1, master in state 4" is the wrong way round.)
  3. **The record that pushed is not stamped, by the role test, not by an exclusion.** Nothing in `0x50a020` /
     `0x5c7cb0` compares the path's owner id (+0x4c) with the record. The chooser writes state, M and S straight
     into +0x78 / +0x7c / +0x80 (`0x682d00` passes those addresses to `0x67f7b0`) **before** `0x682e80` /
     `0x683140` run, and the swapped retries of mode 2 swap +0x7c / +0x80 before each call. The detour of
     `0x683140` lands on S of a state-1 record (partner M ≠ S: false); the detour of `0x682e80` lands on M of a
     state-4 record (partner S ≠ M: false). So a record is never stamped by its own detour, and a detour pushed by
     record c onto unit X stamps **every other** record of X in which X is the held unit (state 1 with X = M, state 4
     with X = S); records in any other state (0, 2, 3, 6, 7, 10-16) are not stamped. The claim before the push
     (`0x42cdd0`: owner pack +0x248 := c, `0x559410` removes older collision paths) does not stamp +0x70.
  4. **Debounce:** each stamp overwrites +0x70 with NOW + 2, so a record that is stamped again before it fires is
     postponed; one stamped every frame never fires.
  5. **When it fires (Q2):** the stamp is tested only at the top of the service `0x6814f0`. A record in state 1 / 4
     has vf +0x50 false, so the driver's first pass (record-id order) calls vf +0x20 → `0x682d00`, which for a
     state ≠ 0 jumps to the service. There: +0x70 := 0, `0x5c6970` (both participants' locomotors give up this
     record id: the detourer's owner +0x248 := 0; **its detour stays on its stack**), state := 0, and the switch
     falls into case 0: M, S := the participants in list order, both speeds 0, both flags 1, return. **No
     decision in that frame.** Next frame, vf +0x50 (`0x6811c0`: state 0 → run the chooser without side effects,
     answer "mode ≠ 0") decides: a mode-0 result (states 2, 3, 6, 7, 8, 11, 13, 15) is decided at once in the
     first pass, without quota; a Pass / Path Around set-up (mode 1 / 2) waits for the quota pass, and without
     quota the record gets case 0 again and waits. *(Correction to "vf +0x50" above: MvM answers true only in
     state 0 **and** when the chooser asks for a set-up.)*
  6. **Speeds while re-decided:** the speed fold runs in the object walk, **before** the collision driver of the
     same frame (land mode update `0x3639d0`: `0x2be640` for both object lists, then `0x50a780`, then the
     coordinators `0x426eb0`). A record in state 0 answers max speed (`0x680d00`). So a stamp that fires in frame
     N's driver frees the held unit in the fold of frame N + 1 (from this record), and the decision in frame
     N + 1's driver at the earliest holds it again from frame N + 2: **at least one free fold per re-decision**.
     [M] that fold may even be "at once": case 0 set both flags to 1 and the flag of the last record asked wins
     (FIGHTER_FLYING §7.7 item 5).
  7. **A new order (Q4) stamps only when no collision owns the unit.** The formation plan `0x41c1a0` (the move /
     formation order) calls `0x554480`(pack, unit, **pack +0x248**) and then vf +0x190 with the order path.
     - Owner 0: the whole stack is destroyed, the order path is pushed on top through the main branch of
       `0x554310` → **stamp**: the unit's records where it is held (state 1 as M, state 4 as S) re-decide at
       NOW + 2.
     - Owner ≠ 0 (a collision record claimed the unit: it is the detourer of a state-1 record, the passer of a
       state-4 record, a pushed or SvS-moved unit): `0x554480` with id ≠ 0 destroys **every** path record, the
       detour included (+0x288 := NOW); `0x554310` then sees owner ≠ path +0x4c = 0 [M: an order path's +0x4c is
       0; a non-zero one would be refused] and inserts the order path through `0x553cc0` at the owner's record
       (none left: the bottom) **without the stamp** and without the +0x275 / +0x1e8 resets; the owner stays set.
       The records in which the unit is held are not re-decided; the record that owned it ends at its next
       service (state 1: no pending tagged waypoint on S's top path and no path of the record → 8; state 4 the
       same test on M), which releases the unit it held.

**MovingVsStopped record:** +0x78 state, +0x7c mover id, +0x80 next retry frame, +0x84 "at once" flag.
**Validity `0x684450`** (first step of the decision and of every state): fewer than two live participants → 5; the
mover has no locomotor, or is gone → 6; a participant whose top path is not forced (vf +0x1d0 = path +0x5c) stands
on a cell that is impassable for it (`0x50e280`) → that unit is **stopped** (vf +0x140(record id)) and → 6.

**The impassable-cell rule in full (s78n t20 RE, 2026-11-10; `0x684450`, `0x680650`, `0x42cbf0`, `0x5ad320`,
`0x42c920`, `0x554480`, `0x42cba0`, `0x423760`, `0x42c890`, `0x5569f0`) [H unless tagged].**
1. **Order of the test.** Participant 0 first: it has a locomotor, its "forced" answer (locomotor vf +0x1d0 =
   `0x42cbf0`) is false and its own position fails `0x50e280` for its own class → **it** is stopped and the record
   goes to state 6. Participant 1 is tested the same way **only when participant 0 passed**. At most one unit is
   stopped per call.
2. **"Forced"** = byte +0x5c of the unit's **top** path record; **a unit with no path record answers false.** So a
   standing obstacle with no path that stands on an impassable cell is "stopped" (a no-op apart from the idle
   clip) and the record still goes to 6: a mover never gets a MovingVsStopped answer from an obstacle that stands
   on impassable ground, and a mover that itself stands on impassable ground with an ordinary path loses its
   whole path stack. Paths with +0x5c set: the forced (flag 0x20) paths of §4.3.1 item 2 [M: other writers of
   +0x5c were not enumerated].
3. **The stop** is the locomotor's vf +0x140(record id). The Walk locomotor (vftable `0x8ad798`) has **the same
   function `0x5ad320`** in that slot as the land team container, so FIGHTER_FLYING §7.7's description holds for
   every Walk unit: `0x42c920` clears the path stack through `0x554480`(pack, unit, id), and, when vf +0x170(id)
   (`0x42cba0`: the pack's path owner +0x248 is 0 or equals id) agrees, plays the default idle clip; then, under
   the same test, state := 0x16 and pack +0xec / +0xf0 := 0 **at once**.
   - With id ≠ 0 (the collision's call) `0x554480` destroys **every** record: detours, pushes and the order /
     formation path alike. (With id 0 and an owner in +0x248 it keeps the owner's record and the ones above it and
     erases those below [M: the destination argument of the internal move is not visible in the decompiled
     call]; with id 0 and no owner it destroys every record.)
   - When the unit's path is owned by **another** collision record (+0x248 neither 0 nor this id) the stack is
     still emptied, but state and speed are left: the next move step finds no record and goes to End Move (brake
     while coasting, then idle).
4. **The formation afterwards.** The formation's state-1 step `0x423760` counts a member as done when its
   locomotor vf +0x150 (`0x42c890`: pack state 0, 0x16, 0x1c, 0x1e, 0x1f, 0x25, 0x26 or 0x27, or 0x19 for a type
   with `Max_Dist_Walk_While_Deployed` > 0) is true and its vf +0x1c0(0) (a path record on the stack) is false.
   The stopped unit is both from the same frame. So once **every** member is done, a one-block formation raises
   signal 10 on all members and goes to state 3 **exactly as for a normal arrival**; an attack block then falls to
   the 30-frame re-check `0x410a60` (FIGHTER_FLYING §4.11 A), which re-plans it.
5. **How such a unit leaves the cell: nothing in the planner helps it** [H for what was read, M for "nothing"].
   - `0x5a1910` only derives cell indices (§4.3.1 item 7); neither it, `0x5a2e00`, `0x5a3020` nor `0x417520` sets
     +0x5c or exempts the first waypoints.
   - A new position order is first run through the zone validation with the unit on zone 0: an impassable goal
     is kept, a passable goal is replaced by an **impassable** cell near it (§4.3.1 item 7, step 5).
   - The region search then needs the start region to pass the class mask and never enters an impassable region,
     so the plan fails unless start and goal lie in one region (`0x41c1a0` returns 0, stamps F +0x1cc and is
     not tried again for 2 × FPS frames [M: the formation's state after a failed plan was not traced]).
   - Even with a path, every Walk step is tested at its **new** position (§3.5 step 4) unless the path is forced:
     a step that ends on a passable cell is accepted whatever the cell it starts from, a step that ends on an
     impassable one is reported (§5.4.7).
   - What does move it: any **forced** path (flag 0x20: garrison exit, unload, the garrison corrective move), which
     is a straight two-point path walked without passability tests.
   - Not found: any code that evicts units when an obstacle is stamped under them (§1.3), or a rescue move.


**Decision `0x684f90`** (state 0; a = mover, b = obstacle). First match wins:
1. Validity; a or b missing → 6; the contact test `0x683790`(a, b) no longer holds → **5**.
2. a's top path P exists and is **forced** (+0x5c, the flag-0x20 path: garrison exit, unload) and b has a locomotor
   behaviour or is a death-clone obstacle (type `IsDeathCloneObstacle` +0xcf): both classes Infantry → **2
   Repulsion**; otherwise push b (`0x6859a0`) → 5, or 7 when the push fails.
3. P exists, b's pack +0x288 (frame of b's last path push or pop; 0 without a pack) < P +0x50 and P's waypoint list
   carries b's id (`0x5501d0`) → **5** (the path was already planned around b after b last changed paths).
4. tgt := a's current target (`0x397a40`) is b and (a's base attack range `0x3749d0` < b's radius `0x3969f0`, or a is
   short-range `0x372210`). If a's class ∈ {Infantry, Bike} and ((b is Infantry and a is **not a team container**
   (locomotor vf +0x230) and (b is not a team container or tgt)) or (a is Infantry and tgt)) → **2 Repulsion**.
   So a single infantry unit or a bike takes Repulsion against single infantry at once; a **squad container does
   not** (unless it is a melee squad closing on its target). (The range, "short-range" and "current target" in
   full: §5.4.5 item 1.)
5. crush := `0x5c8900`(a, b); scat := crush and `0x5c8890`(b). If a is a Giant Vehicle, not crush, and (b is Infantry
   / Bike or a is turning in place, vf +0x1b0): push b; success → 5 (a failure falls through; no second push in
   step 8).
6. If not crush and the **destination is blocked** (`0x684710`, below):
   - if `0x6835c0` allows it (a holds no collision id, or none of its other records is a StoppedVsStopped or a
     MovingVsStopped in which a is the obstacle; and not (a Vehicle / Large / Giant with b Infantry / Bike)):
     alt := `0x5c6ba0`(a, b, &v) (the dynamic pathfinder looks for a spare point v within |a − b| + r_a + r_b
     [M: its rule]); **early arrival** `0x685e10`(a, b, v) (below) succeeds → +0x84 := 1 and **5**; when alt is
     false go to step 8;
   - Path Around `0x685920`(a, b, 0) (dynamic pathfinder `0x5c80e0`, kind 4; retried once with flag 4) → **1**.
   Otherwise (destination free, or crush): both classes Infantry → **2 Repulsion**; else, when not crush, Path
   Around as above → 1.
7. (reached when step 6 produced nothing)
8. friendly := a's and b's owners are not hostile (`0x2824f0`), or scat. shove_ok := friendly and a is neither a
   single (non-container) Infantry unit nor a Bike against Infantry. If (shove_ok or b is a death-clone obstacle)
   and (not crush or scat) and b is not in a hero clash (+0x3b4) and step 5 did not already try: **push** b
   (`0x6859a0`) → 5 on success.
9. crush → **4** (unnamed: the crusher drives on).
10. a's or b's class above Bike and alt: Path Around `0x685920`(a, b, 1) → 1; else `0x685490` → **8** (finds a
    detour that ignores units and, when a hostile unit sits on it, gives a's formation an attack block on that unit
    [M]).
11. → **7 Try Again Later**.

- **Destination blocked `0x684710`(a, b):** a's approximation is not empty, a has a path, the approximation reaches
  the end of its path stack (pack +0x28c, written by the rebuild `0x505fe0`), its last point E lies at or beyond the
  current waypoint line (n·(E − W) ≥ −0.1), and a's rectangle placed at E **overlaps** (§5.3.1) the rectangle of b or
  of any layer-0x10 object within the 160 × 160 box around E that is **not moving** (neither vf +0x1a8 nor vf
  +0x1b0). In words: the mover is on its final leg and something stands where it wants to stop.
- **Early arrival `0x685e10`(a, b, v)** *(exact geometry: §5.4.4, s78n t19)*: from the end of the approximation backwards in steps of 5: the first path
  point where a's rectangle (hx + 5, hy), turned along the path there, overlaps no stopped layer object near E (nor
  b), is no farther from E than v when v ≠ 0, and, when a has a target, lies within a's attack range of it (+ the
  target's radius when a weapon check passes [M]). On success `0x684100` cuts a's path by (length behind that point
  + 3.0); for a formation path (kind 1) it also cuts the paths of the formation members whose slot is behind a's and
  within the two half widths sideways. Returns true when b is at or ahead of the stop point along the path.
- **Push `0x6859a0`** (§5.4 "Push"): additionally, a b **without a locomotor** is not pushed: a death-clone obstacle
  is **killed** (`0x391310`) and the call succeeds, anything else fails; a b whose type has
  `Max_Dist_Walk_While_Deployed` ≤ 0 and that is deployed / deploying (`0x39a390` / `0x39a400`) is not pushed. The
  pushed path is marked started (+0x6d) and a b standing in state 0x16 / 0x25 goes to state 0 at once. A deployed
  Destroyer Droid (M = 150) is pushed and walks the push path deployed (§3.2.1 item 5).

**Per-state service `0x684cf0`** (every frame; validity first in every state; a = mover):
| state | does | ends |
|---|---|---|
| 0 None | nothing (waits for the decision) | |
| 1 Path Around | | a no longer holds a path made for this record (vf +0x1c0(record id) false) → 5 |
| 2 Repulsion | nothing but the end tests | `0x683ee0`: b lies behind a, (b.P − a.P)·(cos, sin)(a's yaw) < −(r_a + r_b), r = `0x3969f0` → **5**; a has no path left (vf +0x1c0(0): empty stack) → **5** |
| 3 Existing Path [M: no writer found] | | contact gone → 5; a has no path → 0; b's pack +0x288 ≥ path +0x50 → 6 |
| 4 (crusher) | | the contact test `0x683790`(a, b) fails → 5; **never re-decided** |
| 7 Try Again Later | b behind a (`0x683ee0`) → 5 | NOW ≥ +0x80 → +0x80 := NOW + 30, state := 0: **re-decided at most every 30 frames** (the first time at once) |
| 8 | | → 5 at the next service |
| 5 Resolved / 6 Unresolvable | removed by the driver the same frame | |

- **Repulsion does not re-test the contact and has no timer.** The record lives until the mover has passed the
  obstacle by the sum of the two radii along its own facing, or has no path left (arrived, stopped), or validity
  fails. While it lives, detection makes no second record for the pair (§5.2 item 5).
- **Speed and flag (vf +0x40 `0x684660`):** flag := +0x84 (1 only after an early arrival). For the mover when it
  cannot crush b: state 7 → **0**; state 1 → the formation-follow speed `0x5c69f0`; every other state, Repulsion
  included → max speed. For the obstacle, and for a mover that can crush: max speed.
- **Vector (vf +0x38 `0x6845c0`)**, weight 2.0: the mover gets one only in state 2; the obstacle in every state
  (FIGHTER_FLYING §7.5).

**Who uses the steering vector `0x509810`** [H: a scan of the whole `.text` for `call` / `jmp` rel32 to it finds four
sites]: Walk `0x61d890` (at 0x61dca4), LandTeamInfantry `0x62d620` (0x62db2e), Bike `0x639550` (0x6397a6), JetPack
`0x640cc0` (0x640e75). **No LandTeamContainer function** (0x5aa880–0x5ae600) calls it, and the container's wanted
facing `0x5ab840` adds only the lane term below. So a squad container in Repulsion (MvS state 2) or Repulse (MvM
state 3) **walks on along its path at its normal target speed**, straight through the obstacle's footprint; its
only own test is the passability of its own point (FIGHTER_FLYING §7.8 item 4). Its members side-step.

**How Walk combines the vector with its path step** (`0x61d570` → `0x61d890`, then the move step `0x620820`):
1. No vector at all when the unit has no path, the path is not started, or it is **on its last waypoint** (there the
   wanted facing is the final facing or the current one, §3.5.1).
2. Otherwise dir = normalise(next waypoint − current waypoint) + k·(−d.y, d.x), d that unit direction, k = the **lane
   term** pack +0x15c. The formation update `0x41dbb0` writes it each frame for a member with a path: e = (its slot
   side offset + jiggle, on a formation path; else 0) − (signed side offset of the unit from its current segment);
   |e| ≤ `FormationMinimumSideError` (GC +0x14cc, FoC 0.1) → 0, else (e ∓ 0.1) / `FormationMaximumSideError` (GC
   +0x14d0, FoC 30). This is what brings a side-stepped unit back to its line.
3. dir is renormalised (fast normalise) and **dir += `0x509810`(dir, unit)**; wanted yaw = the heading of dir
   (`0x20acd0`). The vector is a sum of side-steps of length ≤ 1 each (× the unit's speed ratio), so one obstacle
   bends the heading by at most 45°.
4. The move step turns toward that yaw by the turn rate and moves the speed budget along the **travel direction**
   (vf +0x288 `0x61e0f0`: the current facing for `Movement_Directions_Count` 1, the segment direction for ≤ 0), up
   to the current waypoint's **line** (through the waypoint, normal = the bisector of the two segments, `0x5c5060`);
   crossing the line steps the waypoint index. A waypoint is passed by crossing its line, not by reaching its point,
   so a deflected unit does not turn back for it. Speed: the normal controller; a Repulsion record answers max speed.
   The step is passability-tested (unless the path is forced); a failure reports to the collision system
   (`0x509090`) and the unit does not move that frame. *Not complete (s78n t26): the full step (turn first, then
   travel along the facing by s' = |D / τ| to the line, the coast on the last waypoint, the End Move test, the τ = 0
   quirk) and the controller and corner fractions that feed it are §3.5.2 B–D.*

**Shove:** *(StoppedVsStopped in full: §5.4.3, s78n t19.)* retail has **no instant displacement** in its collision code [H]. No function of the collision system or
of the four record classes (0x504000–0x50b200, 0x5c5f00–0x5c8a80, 0x677000–0x688800) calls a position setter
(`0x3a8f90`, `0x3a8f80`, `0x3abb80`, vf +0x2f8). A stopped blocker moves only by **walking** a path it is given: the
push `0x6859a0` (kind 4 / 7) or the StoppedVsStopped resolution `0x687720` (free spot `0x440f90`, land path
`0x5a2830`, kind 4; failure → Try Again Later, retried every 30 frames).

***Port spec (MovingVsStopped):***
1. Record life: a record is serviced every frame; decisions (state None) wait for the dynamic quota in priority
   order. End a record only by its state (Resolved / Unresolvable), never because the pair stopped predicting
   contact, except in the states that test it (decision step 1, state 4).
2. Infantry-class mover against an Infantry-class stopped unit:
   - single infantry (not a container) against single infantry → Repulsion at once;
   - a **container** mover, or any infantry mover against a container: Repulsion only when the mover's path is
     forced, when it is a short-range unit whose target is the obstacle, or when **its destination is not blocked**.
     With a blocked destination (final leg, its footprint at the path end overlaps a stopped unit): early-arrival
     cut (then Resolved with the "at once" flag), else Path Around, else (friendly pair, container mover) push the
     blocker, else Try Again Later (speed 0 at the braking rate, re-decided every 30 frames).
   The plan's rule "container and both Infantry → Repulsion, always" is wrong for the blocked-destination case: a
   squad ordered onto a spot where another unit stands does not walk into it.
3. Repulsion: mover and obstacle at max speed, no hold, no push, no repath, no nudge. The record ends when the
   obstacle is behind the mover's facing by more than √(hx² + hy²)_a + √(hx² + hy²)_b, or the mover has no path
   left. It is **not** ended by the contact prediction ("`collision_applies`").
4. A container never uses a vector; a single Walk infantry mover adds it to its heading as in "How Walk combines"
   (items 1-4), which is what the port needs before single infantry can use Repulsion instead of Path Around.
5. Crushing mover (step 9): a push is tried once, in the decision, and only when the blocker scatters
   (`Scatters_From_Crushers`, SPREAD_OUT off) and is not in a hero clash; on failure (or without the try) the record
   goes to state 4: the crusher keeps full speed, there is no hold, no arrival cut, no repath and **no retry of the
   push**; the record ends when the contact test fails. The plan's "[port] retry each check" has no retail
   counterpart: one attempt per record.
6. No shove: drop the instant displacement for every pair (also the legacy non-container one); a blocker moves only
   by a push path.

#### 5.4.2 MovingVsMoving: the state machine (s78n t18 RE, 2026-11-10) [H unless tagged]
Read from `0x680420`, `0x682d00`, `0x67f7b0`, `0x679750`, `0x6814f0`, `0x67f080`, `0x67f130`, `0x67f590`, `0x67eb90`,
`0x67ea40`, `0x680650`, `0x680510`, `0x6807e0`, `0x680c00`, `0x680c40`, `0x680d00`, `0x680ef0`, `0x681d30`,
`0x681ef0`, `0x682170`, `0x682510`, `0x6826f0`, `0x682930`, `0x682e80`, `0x683140`, `0x683260` and the debug text
`0x67e260` ("Master" = +0x7c, "Slave" = +0x80, "Master speed" +0x84, "Slave speed" +0x88, "Failures" +0xb8).

**Record:** +0x78 state; +0x7c master id M; +0x80 slave id S; +0x84 / +0x88 the speeds answered to M / S; +0x8c…+0xa8
two hold lines (point, direction) of state 4; +0xac / +0xb0 starvation (next master, countdown from 30); +0xb4 retry
frame; +0xb8 failures; +0xbc the gate of the early detour pop `0x6807e0` (not a path-around kind: §5.4.6); **+0xbd / +0xbe "at once" flags of M / S**. Constructor: state
0, flags 0, +0xb0 = 30. States 11-16 have no debug name. States **5 (Infantry Push) and 14 are never entered**: no
code writes them.

**Answer to a participant (vf +0x40 `0x680d00`):** flag := +0xbd for M, +0xbe for S; speed := +0x84 / +0x88 when
the state ≠ 0, else its max speed. **Vector (vf +0x38 `0x680c40`):** only in states **3** and **11**, for either
participant (and so for its squad members), `0x5c6ca0`(…, 1.0), weight 1.0 (FIGHTER_FLYING §7.5).
`0x67f080` ("reset") sets both speeds to the max speeds and both flags to 0.

**Validity `0x680650`** (start of most states): fewer than two participants → 8; a participant without a
locomotor → 9; a participant whose top path is not forced stands on an impassable cell → it is stopped (vf +0x140;
the rule in full is in §5.4.1, "The impassable-cell rule") and → 9; either participant **not moving** (neither vf +0x1a8: speed > 0 or a path on the stack, nor vf +0x1b0:
turning in place) → **9**. So the record dies the frame one of the two has no speed and no path.

**Decision `0x682d00`** (state 0, under quota): the chooser `0x67f7b0` gives (state, M, S, mode, swap_ok); state 0 →
wait. mode 0 → run the new state's service at once. mode 1 (Pass set-up): `0x682e80` path-arounds M around S
(`0x5c80e0`, kind 4) and stores the two hold lines from S's swept box (`0x677650`); failure → failures += 1, retry
frame := NOW + 30 + synced random [0, 15] (`0x1ffb40` on the sim generator), M and S swap, state 12. mode 2 (Path
Around set-up): `0x683140` path-arounds S around M (kind 4, or 7 when M can crush S and S scatters); failure:
swap_ok false → state 7; else retried with the roles swapped, then both ways with flag 1; all four fail → state 10
(retry frame as above).

**Chooser `0x67f7b0`** (p0, p1 = the record's participants in list order; start M = p0, S = p1):
1. The predictor `0x442540`(p0, p1, 1.5 s, 0) (§5.3.1) fails → **8 Resolved**.
2. Starvation: +0xb0 = 0 and +0xac set → **15** with M = +0xac.
3. f0, f1 = unit facings (the locomotor's wanted facing, vf +0x1f8, while it turns in place); ahead0 = (p1.P −
   p0.P)·f0, ahead1 = (p0.P − p1.P)·f1; **inf** = both classes ∈ {Infantry, Bike} (a team container is Infantry: no
   container test anywhere in the chooser).
4. **Same formation** (p1's pack +0x1f0 is p0's formation): s0, s1 = their slot offsets along the formation
   (`0x5566a0`). When the unit with the front slot is behind the other (s1 > s0 and ahead0 < −0.001: M = p1, S = p0;
   or s1 < s0 and ahead1 < −0.001: M = p0, S = p1): inf → **11**; else `0x67ea40` (same type, or same
   `FormationOrder` (+0x16a0) and footprint half extents that differ by at most half a formation spacing, 10 ×
   `FormationSpacing`, each way) → **13** (the two swap formation slots, `0x554660`, then 8); else M's pack +0x110
   = 0 → **4 Pass**, mode 1. In every other case → **2 Speed Slave Only**,
   M = the one farther ahead (ahead0 < ahead1 → M = p0).
5. Exactly one of the two is on a **forced path** (vf +0x1d0): M = that one, S = the other, **1 Path Around**, mode
   2, no swap.
6. rel = the relation code `0x679750`(p0, p1) (below).
7. **inf: rel ∈ {0x10, 0x20} → 2 Speed Slave Only** (M = the one farther ahead); **anything else → 3 Repulse** (M =
   p1, S = p0; the roles are not used in state 3).
8. Other pairs *(this item is the 2026-11-10 first reading; the full ordered branch list, read from the
   disassembly, is §5.4.6, which replaces it: the head-on tie-break and the crossing cases below are incomplete)*
   [M]: c0 / c1 = "p0 can crush p1" / the reverse, sc = that and the
   victim scatters. Same general direction (f0·f1 > 0) and the rear one cannot crush: the overtake test `0x683260`
   → **4 Pass** (rear = M, mode 1) or **6 Moving With Speed Only** (front = M). A Giant against a non-Giant that it
   cannot crush or that scatters → 1 (Giant = M, mode 2, no swap). Then by rel: 0 / 2 → 1 (M = p1) or **7** when p1
   can crush p0; 4 (head-on) → 7 when one can crush a non-scattering other, else 1 with M = the higher collision
   class (equal class: by formation size, then speed, then ids); 8 (crossing) → 1 with M = the one farther ahead,
   or 7 for a crusher whose victim does not scatter; 0x10 / 0x20 → 6 with M = the one farther ahead.

**Relation code `0x679750`(p0, p1)** (p0 must be moving, else 0): p0's collision speed cap over all its records
(`0x509ea0`) is 0 → **2**. c = h0·h1 (h = unit wanted facings, vf +0x1f8). c > 0.5 (within 60°): **0x20** when p0's
speed (formation speed if it has a formation, else max speed) < p1's, else **0x10**. c ≤ 0.5: when p1 is at least
its half width (`0x396b70`) ahead of p0 along h0, or the pair's own record is a MovingVsMoving in state 0, 1 or 4
(always true at the decision), → **4** if c ≤ −0.5 (head-on), else **8** (crossing); otherwise **0x10**.
So at the decision: **headings within 60° → 0x10 / 0x20; otherwise 4 / 8** (or 2 when p0 is held).

**States (`0x6814f0`; "reset" = `0x67f080`; M / S speeds; "ends" besides validity):**
| state | per frame | ends |
|---|---|---|
| 0 None | (service without quota:) M, S := the participants, speeds 0, **both flags 1**; the answer is max speed | decision |
| 1 Path Around | reset; M := max speed if it can crush S, else the slave-speed `0x67f590`(M, S); S := the formation-follow speed `0x5c69f0`; when M is at or past its hold line (a waypoint of S's detour tagged with M) and cannot crush: **M := 0, flag M := 1** | S no longer holds the detour path → 8; `0x6807e0`: S behind M with a clear rest of the way → pop the detour, 8 |
| 2 Speed Slave Only | re-run the chooser (≠ 2 → state 0); reset; M := max; **S := `0x67f590`(S, M)**, and **flag S := 1 when that is 0** | `0x67eb90` → 8; starvation `0x681d30` → 0 / 15 |
| 3 Repulse | M := max, S := max. **No reset: the flags keep their old value** | `0x67eb90` → **8** |
| 4 Pass | reset; M := max; S := max if S can crush M or is before hold line A; **0 with flag S := 1** at or past line B (or at / past the detour's hold waypoint); else min(`0x67f130`(S, M), ½ M's current speed) | M's detour gone, or S behind M by r_M + r_S (`0x3969f0`) → 8 |
| 6 Moving With Speed Only | reset; chooser must give 6 again (else 0); M := max; S := max if it can crush M, else `0x67f130`(S, M) | predictor fails → 8 |
| 7 | reset; both max (a crusher pair) | validity only |
| 10 Try Again Later | reset; retry frame reached → state 0, new retry frame; speeds: **both 0** (flags 0, so braking), except: M is behind S (along S's heading) → S := max; else S is behind M → M := max (face to face: both stand) | |
| 11 | M := max; S := `0x67f130`(S, M) (no reset); vectors as in 3 | S behind M by r_S + r_M (`0x3969f0`) along M's heading → 8 |
| 12 | reset; M := max; S := `0x67f130`(S, M) | retry frame → 0; predictor fails → 8 |
| 13 | swap the two formation slots | → 8 |
| 15 | reset; **M := 0** (flag 0), S := max; when M has exactly one collision: path-around M (`0x5c80e0`) → 16 | |
| 16 | reset; M := max, **S := 0** | M's detour finished → 8 |
| 8 Resolved / 9 Unresolvable | removed by the driver | |

- **`0x67eb90` (states 2, 3):** R = 1.3 × (r0 + r1), r = `0x397780` (§5.3.1 item 6); true when either is behind
  the other: (p1.P − p0.P)·(cos, sin)(yaw0) < −R or (p0.P − p1.P)·(cos, sin)(yaw1) < −R.
- **Slave speed `0x67f590`(x, y):** the two facings are within 10° of parallel or anti-parallel (|f_x·f_y| ≥
  0.9848) and y is at least hy_x + hy_y (`0x396b70`) to the side of x's line → x's max speed (separate lanes). Else
  `0x67f130`(x, y).
- **`0x67f130`(x, y)**, with v = x's speed, vmax / vmin = x's max speed / `Min_Speed` × mode factor (`0x3728f0`),
  d = normalise(y.P − x.P) (2-D), cy = (y's current speed)·(f_y·d), cx = −vmax·(f_x·d): cx + cy ≥ 0 (x is not
  closing even at full speed) → vmax. Else gap = the rectangle gap (§5.3.1); **gap = 0 → x's "at once" flag := 1**
  (+0xbe when x is the slave, +0xbd otherwise). τ = gap / −(cx + cy); follow = 0 when cx = 0, else clamp(|cy / cx|
  × vmax × 0.8, vmin, vmax); when follow < v and x's deceleration dec (vf +0x2e0) > 0: τ := max(0, τ − (v − follow)
  / dec); k = clamp(τ / (30 × `MovingVsMovingLookAheadTime`), 0, 1) (45 frames). y moving away along d (cy > 0):
  gap ≤ 0 → **0**, else follow + (vmax − follow)·k. y not moving away: vmin + (vmax − vmin)·k.
- **Starvation `0x681d30`** (state 2): each frame a unit's answered speed is below its own acceleration per frame
  (`0x372fe0`; 0.05 when that is 0) while the other's collision cap is too, the countdown +0xb0 drops; at 0 the
  starved unit becomes M of state 15 (it stops, then is path-arounded) [M: exact comparison].

**Two Infantry-class movers (container vs container, container vs single Walk infantry, infantry vs infantry):**
1. One on a forced path (garrison exit, unload) and the other not → **1 Path Around**: the other one (S) detours.
2. Both members of one formation → **2 Speed Slave Only** (front one master), or **11** while the unit with the
   front slot is behind: master max speed, the other the slave speed, both sides get vectors until it has fallen
   back behind.
3. Headings within 60° (a squad following or overtaking another) → **2 Speed Slave Only**: master = the one farther
   ahead, max speed; slave = `0x67f590`. Side by side (parallel within 10°, lanes at least hy + hy apart): max speed.
   Otherwise it slows toward 0.8 × (the leader's speed along the line / its own) and, with the two rectangles
   touching while the leader moves away, **0 with the "at once" flag** (the "footprints touch" hold). No vectors.
4. Any other geometry (crossing, head-on) → **3 Repulse**: **both at max speed**; **`0x67f130` is not called, so no
   footprint-touch hold**; flags untouched (0 for a record decided on its first frame; (1, 1) if it sat one frame in
   state 0 without quota, which only makes another cap, such as the straggler cap, bite at once); each side gets
   vectors of weight 1.0: a single Walk infantry for itself, a container's **members** (the container itself uses
   none, §5.4.1): Σ side-steps away from the other participant, or from each of its members when that is a
   container, × clamp(speed / max speed).
   Ends: one is behind the other's facing by 1.3 × (r0 + r1) → Resolved; either stops moving → removed.

***Port spec (MovingVsMoving, Infantry-class pairs):*** choose per record at its first resolution: forced-path
case → the other one path-arounds (existing behaviour); same formation or headings within 60° (dot of the two
wanted facings > 0.5) → **Speed Slave Only**: leader (the one farther ahead along its own heading) unaffected,
follower's target speed = the slave-speed rule above, taken "at once" only when it is 0; else → **Repulse**: no
speed change, no hold, no nudge, no repath for either side, vectors (weight 1.0) for both sides' members / the
single infantry. Speed Slave Only re-chooses every frame and turns into Repulse when the headings diverge. A record
ends by `0x67eb90` or when either participant has neither speed nor path. The plan's "[port] Repulse for every
Infantry pair" is right for crossing and head-on squads and wrong for squads moving the same way (retail slaves the
rear one to the front one and stops it at contact). [port] the starvation states 15 / 16 and the (1, 1) flag quirk
may be left out; record that.

#### 5.4.3 StoppedVsStopped in full (s78n t19 RE, 2026-11-10) [H unless tagged]
Read from the vftable `0x8b5450`, `0x686de0`, `0x687570`, `0x687720`, `0x686e80`, `0x687010`, `0x686b20`,
`0x5c74b0`, `0x5c62a0`, `0x5c6650` / `0x505490`, `0x440be0`, `0x441750`, `0x440f90`, `0x538330` / `0x5d7c00` /
`0x6b24f0`, `0x5a2830`, `0x686f80`, `0x5c78e0`, the Walk slots `0x42c7d0`, `0x42cba0`, `0x42cdd0`, `0x42d060`,
`0x42c680`, `0x61e570`, `0x61e380`, `0x42ced0`, `0x42cbd0`.

**Record** (0x88 bytes): +0x78 state (0 None, 1 Resolving, 2 Resolved, 3 Unresolvable, 4 Try Again Later); +0x7c
the id of the unit being moved (0x3FFFFF until one is chosen); +0x80 next retry frame (constructor: NOW + 30).
Participants p0 = the checker, p1 = the candidate of the detection (§5.2). Weight 3.0 (vf +0x30); vf +0x50 false,
so the driver services it every frame without quota; vf +0x28 ends it in state 2 or 3; speed answer vf +0x40 =
max speed, flag untouched (`0x5c78e0`); vf +0x78 false (a new path of either unit does not re-decide it).

**Service `0x687570`** (states loop in one call):
- 0 → the resolution `0x687720` below (the first service after creation runs it at once).
- 1 Resolving, each frame: validity; the moved unit X (+0x7c) gone or without a locomotor → 3; X's locomotor owned
  by another collision (vf +0x170(id) false: pack +0x248 neither 0 nor this id) → 4; **X holds no path record
  owned by this record any more** (vf +0x1c0(id) false: the path was walked to its end and popped, or dropped by
  another claim) → **2 Resolved**; else, when the path can be started (vf +0x1a0 `0x61e380`: top path not started,
  or the unit stands in state 0x16 / 0x19 / 0x25) → start it (vf +0x148(id) `0x61e570`: started flag +0x6d := 1,
  state := 0; a deployed unit first undeploys unless the path is shorter than `Max_Dist_Walk_While_Deployed`).
- 4 Try Again Later: NOW ≥ +0x80 → state 0 and +0x80 := NOW + 30 (the resolution runs on the next frame). **Retry
  every 30 frames, for as long as both units live.**
- 2, 3, 4: X's locomotor gives up the record id (vf +0x180); 2 / 3 are removed by the driver the same frame.
- **Validity `0x686e80`:** fewer than two live participants → 2; neither has a locomotor, or one that has a
  locomotor stands on a cell impassable for it (`0x50e280`) → 3 (nobody is stopped, unlike §5.4.1).
- **The contact test is never re-run.** A record whose resolution failed stays in the 4 → 0 → 4 cycle even after
  the two have separated by other means: the ring search then returns X's own position (ring 0), which counts as
  "no spot". While it lives, the detection makes no other record for the pair (§5.2 item 5) [M: consequence, not
  observed in a run].

**Which unit moves: `0x687010`** (first match; c = UnitCollisionClass):
1. p0 is a Landing Transport (class 0) or has no locomotor → p1. Else p1 is one / has none → p0.
2. p0's hover state is lowering (pack +0x2a8 = 3) → p1; p1 lowering → p0.
3. imm(x) = type `Max_Dist_Walk_While_Deployed` (+0x23b0) ≤ 0 and x is deployed or deploying (`0x39a390` /
   `0x39a400`). Both → **nobody** (state 4). imm(p0) → p1. imm(p1) → p0. (The Destroyer Droid, M = 150, is never
   imm; when moved it walks deployed or undeploys by the remaining path length, §3.2.1 item 5.)
4. Exactly one has a current target (`0x397a40`, §5.4.5 item 1) → **the one without a target moves**.
5. Both Infantry class and both have targets → **nobody** (state 4).
6. Not both Infantry, the classes differ and the higher one is Giant (9) → the candidate list is the lower-class
   unit only.
7. Otherwise both are candidates, **cheaper first**: cost(p1) ≤ cost(p0) → [p1, p0], else [p0, p1].
8. (6-7 only) the first candidate whose locomotor accepts the claim (vf +0x168(id) `0x42c7d0`: no owner, this
   record, or an owner of lower weight, i.e. a MovingVsMoving or MovingVsStopped record) moves; none → nobody.
   (Rules 1-4 return their unit without this test; the claim itself is tried later.)
- **Cost `0x686b20`(x, y, d)**, d = x's escape direction below as a unit vector, v = x's effective max speed
  (`0x370f00`), Δ = y.P − x.P (2-D), r = √(hx² + hy²) of each **type's** hard extents (`0x371df0`), R = r_x + r_y,
  along = v·(d·Δ), side = v·(d.y·Δ.x − d.x·Δ.y). t := none. along > 0 → t = (|Δ| + R) / along. along < 0 → t = 0
  when |Δ| > R, else (R − |Δ|) / |along|. side ≠ 0 → ts = R / side (signed, as coded); t := ts when t is none or ts
  < t. t none or < 0 → cost 0. Else cost = |wrap(yaw(d) − yaw_x)| / turn rate (`0x372560`) + t.

**Escape direction `0x5c74b0`(record, X)** (as an orientation; O = the other participant):
- X has a current target → the direction **from X to its target**.
- Else, X is Vehicle / Large / Giant class, O's class is higher than X's and X faces away from O ((X.P − O.P)·f_X
  > 0) → **X's own facing** (it drives straight on).
- Else → the direction **from O to X** (straight away from the other unit).

**The spot: the ring search `0x440f90`** (A1_READINGS §40.2), set up by `0x687720`: object = X, centre = X's
position, **mode 2 (cone)**, yaw = the escape direction, occupancy margin 5 × `FormationSpacing`(X) (`0x440be0`),
reachability on; path list = the movers of X's MovingVsStopped records (`0x5c6650`, kind 3) with fresh
approximations and clearances (`0x441750`, A1_READINGS §40.1); then `0x5c62a0`: **Infantry class: cone angle π,
outside radius 1.0; every other class: cone 3π/4, outside radius 0.75** (the push uses π/4 and 0.33).
- Rings R = 0, 1, 2.8, 5.4, 8.8, 13, … (step 1.0, then + 0.8 per ring, capped at min(1.5 r_X, 10); r_X =
  `0x3969f0`; at most 20 rings); n = trunc(2πR / r_X) + 1 samples (≥ 4 after ring 0) at θ_i = 2π·i / n measured
  **counter-clockwise from the escape direction**; a sample with θ ≤ the cone angle lies at radius R, the others
  at the outside radius from X.
- First sample, in that order, that is inside the map box, in the same connected region as X for its movement
  class (`0x50ecd0`), **unoccupied**, and off the listed movers' paths wins.
- **Occupancy, settled (`0x538330` → `0x5d7c00` → `0x6b24f0`; this replaces A1_READINGS §40.4's [M] "box of half
  size r + margin")**: a layer-0x10 record other than X's blocks the sample when its centre C comes within
  (r_X + margin) + ρ_rec of the sample S, ρ_rec = hy + (hx − hy)·cos² of the angle between the record's axis and
  the line S → C (the directional radius of §5.3.1); the query point is swept one unit along world +x (S → S + (1,
  0)), a quirk of the query set-up that widens the test by up to 1 on that side. So the spot is the first sample
  at least r_X + 5·`FormationSpacing` + ρ away from every other layer object, O included.
- No spot in 20 rings, **or the spot is X's own position** (ring 0 passes: nothing is near any more) → state 4.

**The move** (`0x687720`, after the spot):
- A land path X.P → spot from the **main land pathfinder** (`0x5a2830`, singleton `0x140a28800`; movement class
  +0x1688; not the dynamic pathfinder the push uses) [M: its request flags (1.0, 1.5, the two set bytes) were not
  decoded]. No path → state 4.
- Path kind **4**, owner = this record. Claim (vf +0x178): refused by an owner of equal or higher weight → state 4;
  a successful claim by a new owner removes X's other collision-owned path records (`0x559410`).
- Pushed on top of X's path stack (vf +0x190 → `0x554310`; X's own records stay underneath), started (vf +0x148),
  +0x7c := X's id, state 1.
- X then **walks**: the normal Walk / container state machine (re-lay, turn in place, accelerate), at its normal
  speed (the record answers max speed, no "at once" flag), with a steering vector for X only (vf +0x38
  `0x686f80`: state 1 and the asker is X → `0x5c6ca0`, weight 3.0: the side-step away from O, FIGHTER_FLYING §7.5;
  a squad container reads no vector, its members do). O does nothing.
- When the path ends it is popped; the next service finds no path of this record → Resolved, removed. X stays at
  the spot (or resumes an unfinished path below, A1_READINGS §40.3). If the two still overlap by more than 0.15,
  the next detection of either (≤ 4 frames) makes a new record.
- While X walks it is Moving (§5.2), so the pair (X, O) is not re-detected as StoppedVsStopped, and the live
  record blocks any other record for the pair.

***Port spec (StoppedVsStopped):*** no instant displacement. Per record: choose the mover by rules 1-8 (no
locomotor / landing transport / lowering / deployed-immobile units never move; a unit with a target stays and the
target-less one moves; two targeting infantry units both stay; against a Giant the smaller one moves; otherwise
the cheaper of the two by turn time + clearing time). Escape direction: toward its own target if it has one, else
its own facing for a vehicle facing away from a bigger unit, else straight away from the other. Spot: the first
free sample of the ring search around the mover itself, rings 0, 1, 2.8, 5.4, 8.8, … (cap min(1.5 r, 10), 20
rings), samples counter-clockwise from the escape direction, only the first 180° (Infantry) or 135° (others) of
each ring at the ring radius; free = in the map, same region, no other layer object within r + 5 ×
`FormationSpacing` + its directional radius, off the paths of units heading into the mover. Then a normal land
path to the spot, pushed above the unit's own paths as a collision path (kind 4), walked at normal speed with
normal acceleration and turn. The record resolves when that path is gone; a missing spot, a spot equal to the
unit's position, a missing path or a refused claim → Try Again Later, retried every 30 frames (first decision on
the record's first service). [port] the retry cycle never ends in retail once the pair has separated; a port may
end the record when the overlap coefficient is ≤ 0.15 at a retry, and must say so.

*Not read:* the `0x5a2830` request flags; the sound event `0x42ced0` raises at the path start.

#### 5.4.4 The early-arrival cut `0x685e10` in full (s78n t19 RE, 2026-11-10) [H unless tagged]
Read from `0x685e10`, `0x684100`, `0x54eee0`, `0x54f610`, `0x54e780`, `0x371be0`, `0x3749d0`, `0x373670`,
`0x397780`, `0x20d080`, `0x20d690`, `0x5566a0`. Called only from decision step 6 of §5.4.1, i.e. after `0x684710`
found the destination blocked and `0x6835c0` allowed it; a = the mover, b = the obstacle, v = the spare point of
§1.5.1 item 6 (0 when none).

1. **Inputs.** A = a's path approximation (pack +0x200; a's position forward along its path stack, here reaching
   the stack's end), L = its length, E = its last point. tgt = a's current target (`0x397a40`, §5.4.5 item 1).
   range = 0 without a target; else a's attack range `0x3749d0` (§5.4.5 item 1), **plus the target's radius
   `0x397780`** (soft radius; joint-box diagonal for a container) when a's type has a weapon list (type +0xc48 > 0)
   whose entry for a's current slot (obj +0x324) exists.
2. **Obstacle list.** The layer-0x10 objects whose record box overlaps the **160 × 160 square centred on E**
   (`0x5d7a00`), plus b; then every object **without a locomotor** and every **moving** one (vf +0x1a8 or vf
   +0x1b0) is dropped. So only standing units with a locomotor count; a locomotor-less death clone never does
   (not even as b), and a itself is dropped because it is moving.
3. **Footprint.** (hx, hy) = **`0x371be0`(a's type)**: the type's `Custom_Hard_XExtent` / `YExtent` × `Scale_Factor`,
   the model's box per missing axis, (0, 0) without a model. **Not `0x396850`:** no deployed switch and **no joint
   slot box**. FoC's 41 team container types set no hard extents, so a squad container is tested with its model
   box, i.e. (0, 0) when it has no model [M: container models not checked]: a line of half length 5 along the
   path. Then **hx += 5.0**.
4. **Scan.** s := L, stepping back **5.0** (s := max(0, s − 5); a test at s = 0 is the last one). At each s: P =
   the point of A at arc length s, n = the unit direction of A's segment there (`0x54eee0`; a failure ends the
   scan). Rectangle Q = centre P, axes n and n⊥ = (−n.y, n.x), half extents (hx + 5, hy) (`0x20d080`).
   - Q **overlaps** (separating-axis `0x20d690`: touching counts) the footprint rectangle `0x38de80` of any
     listed object → next s.
   - a has a target and the 2-D distance |P − tgt.P| > range → next s.
   - Otherwise P is the stop point: when v ≠ 0 and |P − E| > |v − E| (2-D) → **fail** (the free point lies farther
     back than the spare point is from the end; the decision then prefers Path Around). Else cut and return.
   - s = 0 failed too → fail (nothing changed).
5. **Cut `0x684100`(record, a, c)** with **c = (L − s) + 3.0**: the path's length is reduced by c from its end
   (`0x54e780`), so **a's new path end is 3.0 before P along the path** (at s = L, where E itself was clear for
   the wider box: 3.0 before the old end).
   - **`0x54e780`(points, keep) in full (s78n t20 RE, 2026-11-10) [H].** It keeps the arc length `keep` measured
     from the path's **first point** (the record's whole point list, the part already walked included);
     `0x684100` passes keep = the list's total length (`0x54f610`) − c, per record. Walking the segments from the
     start (zero-length segments are skipped), at the first segment with keep < length so far + segment length:
     every point after the segment's first point is erased; then, when keep > the length so far, one new last
     point is appended at that arc length (linear interpolation; it takes the facing vector +0x0c of the point
     that followed, id tag 0x3fffff, every other field 0). keep exactly on an existing point → no point is added.
     **keep ≤ 0 (c ≥ the whole list's length) → only the first point is left: a one-node path.** keep ≥ the total
     length → nothing changes. A list of fewer than 2 points is left alone.
   - **The record's waypoint index (+0x60) is not touched by the cut.** The cut point can lie behind the unit: c
     counts from the end of the path **stack** (L is the approximation's length), so when a detour or push record
     lies on top of an order path, or s < 3.0, keep can fall short of what the unit has already walked on the
     record that is cut, or below zero.
   - **What the unit then does** (Walk step `0x620820`; the container step `0x5adfb0` has the same branch) [H]: a top record that is "on its last waypoint" (`0x5c5910`: index + 1 ≥ point count, which a one-node or
     over-cut list always is; a cut inside the current segment behind the unit gets there in the same step,
     because the unit is already past the new point's waypoint line and the index steps on) is treated as
     finished. With a **started record below** it on the stack and the unit not in End Move: the top is popped
     (`0x557b10`) in the same step and the unit walks on along the lower record. Otherwise the unit goes to **End
     Move (0x14)**: it coasts straight along its stored travel direction while the speed controller brakes at its
     normal deceleration (each coast step passability-tested unless the record is forced), then Pre-Stopped (0x15)
     → arrival turn test → pop → idle. So it does **not** stop dead where it stands; it brakes to a stop from
     there (unless a collision record's "at once" flag sets the speed, item 6).
   - a's top path is not a formation path (kind ≠ 1), or a is in no formation: a's **top** record only.
   - Formation path: a's kind-1 record, and the kind-1 record of every formation member m whose slot is **behind**
     a's (slot x offset `0x5566a0`: x_m < x_a) and within the two half widths sideways (|y_m − y_a| ≤ hy_m + hy_a,
     `0x396b70`), each cut by the same c.
6. **Result:** true iff b is at or ahead of the stop point: (b.P − P)·n ≥ −0.001. True → the record's "at once" flag
   +0x84 := 1 and state 5 (the unit's speed controller may then take a lower target in one frame, FIGHTER_FLYING
   §7.7 item 5, and brakes for the nearer end). **False after a cut** (b lies behind P) leaves the path cut and
   the decision goes on to Path Around / push with the shortened path.
7. **Where the unit ends:** it walks the shortened path normally and arrives by the usual arrival rule of its
   locomotor (container: within 5.0 before the last waypoint's line, FIGHTER_FLYING §7.7 item 6): its centre about
   3 before P, its front (hx) at least 5 + 3 short of the first standing footprint along the path. There is no
   snap and no further test at arrival. With a target, the stop point is also inside attack range of it.

***Port spec (early arrival):*** on the mover's final leg with a blocked destination (§5.4.1 `0x684710`): walk
back from the path end in steps of 5 along the remaining path; at each point test the oriented rectangle (type hx
+ 5, type hy) (for a squad container the container **type's** extents, zero when it has none, not the joint box)
aligned with the path against the footprint rectangles of all standing locomotor units within the 160 square
around the path end (touching = overlap); with a target also require distance to the target ≤ attack range (+
target radius for a weapon unit); take the first clear point, fail if it is farther from the path end than the
spare point (when one exists) or if even the unit's own position fails; shorten the path so that it ends 3.0
before that point (and the same amount for formation members queued behind in the same lane); Resolved with the
"at once" flag when the obstacle is not behind the stop point.

#### 5.4.5 Decision `0x684f90` leftovers (s78n t19 RE, 2026-11-10) [H unless tagged]
Read from `0x684f90` (again, in full), `0x3749d0`, `0x372210`, `0x397a40`, `0x41a2e0`, `0x6859a0`, `0x684710`,
`0x685e10`, `0x6835c0`, `0x5c8900`, `0x5c8890`, `0x391310`. The step numbers are those of §5.4.1.

1. **Step 4, the target clause.** tgt is true when **a's current target is b** and (a's attack range < b's radius
   `0x3969f0`, or a is short-range).
   - **Short-range** `0x372210`: type `Targeting_Max_Attack_Distance` (+0x500) ≤ GameConstants `Melee_Cutoff_Range`
     (`0x140b27340`, 51.0). It is the same "melee" test as the targeting filters; no other tag.
   - **Attack range** `0x3749d0`(type, obj) (TACTICAL_COMBAT §2): the range (+0x108) of the type's first, else
     second, ability when that ability is active on the object and the value is > 0; else, for a lobbed
     projectile type (`Projectile_Category` 3, own or of the current weapon slot), v² / (g terms); else
     `Targeting_Max_Attack_Distance` × `Deployed_Max_Attack_Distance_Multiplier` when deployed, × the garrisoned
     multiplier (1.5) when hidden in a garrison.
   - **Current target** `0x397a40`(a, 0): the target of the top usable block of a's **formation** (a team member:
     its container's formation; `0x41a2e0`: the last block with a target object that is not finished and that a's
     lead member may attack); when the formation gives none, the object's **own** target record (obj +0xf0 → +8:
     the auto-acquired target). So an attack order's target and an idle unit's acquired target both count.
   - Result unchanged from §5.4.1: Infantry / Bike mover → Repulsion at once when (b is Infantry class and a is not
     a team container and (b is not a team container or tgt)) or (a is Infantry class and tgt).
2. **Death-clone obstacles** (type `IsDeathCloneObstacle` +0xcf; FoC: the AT-AT, AT-ST, SPMA-T, MPTL and Rancor
   death clones, `Proximity_Mine`, `Proj_Remote_Bomb`). They have no locomotor (obj +0x333 = −1), are never Moving
   and are only ever the obstacle b.
   - Step 2 (a on a **forced** path) applies to b with a locomotor **or** a death clone: both Infantry class →
     Repulsion; else the push → 5, or 7 when it fails.
   - **The push of a locomotor-less b** (`0x6859a0`, first test): a death-clone obstacle is **killed**
     (`0x391310`(b, 1), the normal death) and the push succeeds; any other locomotor-less b → the push fails. No
     spot search, no path.
   - Step 6: `0x684710` and `0x685e10` count only objects **with** a locomotor, so a death clone never "blocks the
     destination": the branch is "destination free" → both Infantry class (mine / bomb against infantry) →
     Repulsion; else Path Around → state 1.
   - Step 8: reached when the Path Around failed. "(shove_ok **or b is a death clone**)" lets **any** mover push,
     hostile or not, single infantry included, provided (not crush or scat), b not in a hero clash and step 5 did
     not already try: the clone is killed → 5. So a wreck that cannot be driven around is destroyed by the unit
     that needs to pass; a Giant (step 5) kills an Infantry / Bike-class clone, or any clone while it turns in
     place, without trying to go around.
3. **Hero clash in step 8:** the byte b +0x3b4 (set while b is paired in a hero clash, HERO_CLASH.md) ≠ 0 → no push
   at step 8, whatever else holds. (Only b is tested, and only here: the Giant push of step 5 and the forced-path
   push of step 2 ignore it. The dynamic pathfinder's Infantry exemption also skips units in a hero clash, §1.5.1.)
4. **A crusher against a unit it can crush** (crush = `0x5c8900`(a, b): the owners are enemies, a has SQUASH, b's
   type is `Is_Squashable`, or a is `Is_Supercrusher` and b `Is_Squashable_By_Supercrusher`; no class test; scat =
   b's faction has `Scatters_From_Crushers` and b's SPREAD_OUT ability is not active, `0x5c8890`). The same code
   for a single unit and for a squad container:
   - **A record is made** like for any pair (the detection has no crush test), with the look-ahead + 1.5 s
     (§5.3). Step 4 can still give Repulsion (an Infantry / Bike-class crusher does not exist in FoC).
   - Steps 5-6 are skipped for it: no Giant push, **no destination test, no early-arrival cut, no Path Around**.
   - Step 8: with scat (and b not in a hero clash): **one push** of b (kind-7 crush-flee path: spot search in the
     ±45° cone beside the crusher's heading, A1_READINGS §40; FIGHTER_FLYING §7.1) → success → state 5, the record
     is removed; b is now Moving, so the pair's next record is a MovingVsMoving one (§5.4.6: the crusher is master
     and b detours on a kind-7 path, or both run on in state 7).
   - Push failed, or b does not scatter, or b is in a hero clash → step 9: **state 4**. The crusher answers max
     speed and gets no vector; b gets the obstacle vector (weight 2.0), which only matters once it moves. The
     record is never re-decided and **the push is not retried**; it ends when the contact test `0x683790` fails
     (the crusher has passed, stopped, or b died) or validity fails. The crush itself is §5.5.
5. **Order-less unit on a pushed path = Moving:** confirmed, §5.2 "What Moving is".

***Port spec (decision leftovers):*** (1) melee = `Targeting_Max_Attack_Distance` ≤ 51; "target" = the unit's
formation target, else its own acquired target; the clause also holds for a ranged unit whose attack range is
shorter than the obstacle's footprint radius. (2) A death-clone obstacle is a standing obstacle that never blocks a
destination; a push on it kills it; it is pushed (killed) by a forced-path mover, by a Giant per step 5, and by
any mover whose Path Around failed. (3) No step-8 push on a unit in a hero clash. (4) Crusher: record yes; exactly
one push attempt, only when the victim scatters; then (or otherwise) the crusher drives on at full speed in state
4 until the contact prediction fails; no hold, cut, repath or retry.

#### 5.4.6 MovingVsMoving for pairs that are not two Infantry / Bike movers (s78n t19 RE, 2026-11-10) [H unless tagged]
Read from `0x67f7b0` (decompile and the disassembly of the whole non-Infantry part with its jump table `0x6803e0`
/ `0x6803f4`), `0x682d00`, `0x683140`, `0x682e80`, `0x677650`, `0x683260`, `0x679750`, `0x6814f0`, `0x680510`,
`0x6807e0`, `0x680650`, `0x680c00`, `0x681d30`, `0x681ef0`, `0x682170`, `0x682510`, `0x6826f0`, `0x682930`,
`0x67ee60`, `0x5c69f0`, `0x5c8900`, `0x5c8890`, `0x1ffb40`, `0x5566a0`, `0x421cc0`, `0x41eab0`, `0x554660`,
`0x559410`, `0x556840`, `0x504ed0`. This replaces chooser step 8 of §5.4.2 and corrects some of its state rows
(list at the end). "Non-Infantry pair" = not both classes in {Infantry 5, Bike 6}.

**Notation** (as §5.4.2): p0, p1 in list order; f = facing (the wanted facing vf +0x1f8 while turning in place);
ahead0 = (p1.P − p0.P)·f0, ahead1 = (p0.P − p1.P)·f1; c0 = `0x5c8900`(p0, p1), sc0 = c0 and `0x5c8890`(p1); c1,
sc1 mirrored (the crush tests: §5.4.5 item 4; **allied pairs never crush**); cls = UnitCollisionClass. Outputs
start as mode 0, swap_ok 1. Steps 1-2 of §5.4.2 are unchanged (predictor fails → 8; starvation → 15).

**Chooser, in evaluation order:**
1. **Same formation** (p1's pack +0x1f0 is p0's formation), slot offsets s0, s1 (`0x5566a0`). "Front-slot unit
   behind": s1 > s0 and ahead0 < −0.001 (F = p1, B = p0), else s1 < s0 and ahead1 < −0.001 (F = p0, B = p1).
   - `0x67ea40`(F, B) (interchangeable, §5.4.2 step 4) → **13**, M = F, S = B (slot swap, then 8).
   - else F's pack +0x110 = 0 → **4 Pass**, M = F, S = B, mode 1. (+0x110 = the index of the member's current slot
     layout among the corridor-squeeze layouts, set by `0x421cc0`; 0 without alternative layouts.)
   - every other case → **2 Speed Slave Only**, M = p0 if ahead0 < ahead1, else p1.
2. **Exactly one on a forced path** (vf +0x1d0) → **1**, M = the forced one, mode 2, swap_ok 0.
3. rel = `0x679750`(p0, p1) (§5.4.2; on the re-runs from states 2 and 6 its "own record in state 0 / 1 / 4" clause
   is false, so 4 / 8 then need p1 at least its half width ahead of p0 along h0, else 0x10).
4. **Same direction** (f0·f1 > 0):
   - ahead0 < −0.001 (p1 is the rear one): not c1 → overtake(p1, p0) true → **4**, M = p1, S = p0, mode 1; false →
     **6**, M = p0, S = p1. With c1 → step 5 (the other order is not tried).
   - else ahead1 < −0.001 and not c0: overtake(p0, p1) true → **4**, M = p0, mode 1; false → **6**, M = p1, S = p0.
5. **Giant:** cls0 = 9 ≠ cls1 and (not c0 or sc0) → **1**, M = p0, mode 2, swap_ok 0. Else cls1 = 9 ≠ cls0 and
   (not c1 or sc1) → **1**, M = p1, mode 2, swap_ok 0.
6. **By rel:**
   - **0 / 2** (p0 standing or held): M = p1, S = p0; c1 → **7**; else **1**, mode 2, swap_ok = not c0.
   - **4** (head-on): c0 and not sc0 → **7** (M = p0); else c1 and not sc1 → **7** (M = p1); else **1**, mode 2,
     swap_ok = neither can crush. M = the higher class. Equal class, both in formations: the larger formation
     extent (`0x41a5c0`, half extent in cells); equal → p0, unless formation 0's first member id < formation 1's
     (then p1). Equal class, either without a formation: the one with the **lower max speed** (`0x370f00`; tie p0).
   - **8** (crossing): c0 and sc0 → **1**, M = p0, swap_ok 0. c0 and not sc0: c1 and sc1 → **1**, M = p1, swap_ok
     0; otherwise **7** (M = p0). Not c0 but c1: sc1 → **1**, M = p1, swap_ok 0; not sc1 → **7** (M = p0, S = p1).
     Neither: **1**, mode 2, swap_ok 1, M = p1 if ahead0 > ahead1, else p0.
   - **0x10 / 0x20** (headings within 60°, reached when step 4 did not decide): **6**, M = p0 if ahead1 > ahead0,
     else p1.
   - any other value: the state stays 0 (decided again next frame).
   In every state-1 result the **slave S detours around the master M** (mode 2 below).

**Overtake test `0x683260`(a = rear, b = front).** False when either has the object flag +0x3a0 bit 2, or a is
Giant. Va, Vb = the formation speed (formation +0x21c; negative → max speed) or the max speed. It goes on only if
Vb ≤ Va or both are held by the same movement coordinator (`0x426140`). vs = min(Vb, 0.5·Va). It needs Va ≠ 0, Va −
vs ≠ 0 and a's pack +0x28c = 0 (a's approximation does not reach its path end). D = a's remaining length over its
whole path stack (`0x556840`), dist = the 3-D distance, r = `0x397780`. **True when (r_a + r_b + dist) / (Va − vs)
≤ D / Va** (a can get past, with b slowed to vs, before its own path ends). Exception: same coordinator and both
in formations → true only when b's formation order index (formation +0x23c, the rank of `0x439360`) > a's.

**Decision `0x682d00` and its failures.**
- Mode 0: the new state is serviced at once.
- Mode 1 (Pass): `0x682e80` path-arounds M around S (flag 1, kind 4; §1.5.1) and stores the hold lines below. It
  fails → `0x680c00` (below), M and S swap, state **12**, serviced at once.
- Mode 2 (Path Around): `0x683140` path-arounds **S around M** (flag 0; kind 7 if M can crush S and S scatters,
  else 4) and sets +0xbc when rel(M, S) after it is 4, 8 or 0x10. Failure with swap_ok 0 → **state 7**. Else:
  roles swapped with flag 0, swapped back with flag 1, swapped with flag 1. All four fail → `0x680c00`, no service
  this frame, **the roles are left swapped**.
- **`0x680c00`:** state := 10; failures +0xb8 += 1 (no cap and no reader but the debug text); retry frame := NOW +
  30 + `0x1ffb40`(&`0x140a13e24`, 0, 15): **one draw of the synced generator, inclusive [0, 15]**. States 10 and
  12 draw again the same way each time their retry frame passes.
- Record vf +0x80 (`0x67ee60`): in state 4 only, a detour whose first segment makes more than 120° with M's
  facing (dot < −0.5) is refused, so that path-around fails.

**States such pairs reach** ("reset" = `0x67f080`; validity `0x680650` first, §5.4.2):
| state | per frame | ends |
|---|---|---|
| 1 Path Around | reset; M := max speed if M can crush S, else the slave speed `0x67f590`(M, S); S := `0x5c69f0` (below). Hold line = the last waypoint of S's top path tagged with M's id (§1.5.1 item 5), used only while S's waypoint index is below it (`0x680510`). **(M.P − point)·dir ≥ −0.001 and M cannot crush → M := 0, flag M := 1** | `0x6807e0` (below) → 8; no pending tagged waypoint and S holds no path of this record (vf +0x1c0) → 8 |
| 4 Pass | reset; M := max. S := max when S can crush M or S is before line A ((S.P − A.pt)·a < −0.001); **0 with flag S** at or past line B ((S.P − B.pt)·a ≥ −0.001); between them min(`0x67f130`(S, M), 0.5 × M's current speed). Also **0 with flag S** when S is at or past M's pending tagged waypoint line and cannot crush | no pending tagged waypoint on M's top path and M holds no path of the record → 8; (S.P − M.P)·f_M ≤ −(r_M + r_S) (`0x3969f0`, M's actual facing) → 8 |
| 6 Moving With Speed Only | reset; the chooser re-run must give 6, else state 0 (roles not refreshed); M := max; S := max if S can crush M, else `0x67f130`(S, M) | predictor(M, S) false → 8 |
| 7 | reset; both max speed, no vectors | validity only (one of them stops, or is gone); no predictor, no timer |
| 10 Try Again Later | reset; retry frame ≤ NOW → state 0 and a new retry frame (the speeds below are still written, but never read: see "Retry frame", s78n t23 RE); both 0, flags 0 (braking); (M.P − S.P)·f_S < −0.001 → S := max; else (S.P − M.P)·f_M < −0.001 → M := max | |
| 11 | no reset; M := max, S := `0x67f130`(S, M); vectors for both | (S.P − M.P)·f_M < −(r_S + r_M) → 8 |
| 12 | reset; M := max; S := `0x67f130`(S, M) | retry frame ≤ NOW → new retry frame, state 0; else predictor false → 8 |
| 13 | `0x554660`: swaps the two members' slot offsets, slot lists, +0x110 and the formation path's +0x68; zeroes both approximation countdowns | → 8 |
| 15 | countdown +0xb0 = 0 → 30; reset; M := 0 (flag 0), S := max; **every frame** while M has exactly one collision record: path-around(M around S, flag = wanted headings differ by < 90°, kind 4) | success → 16 |
| 16 | reset; M := max, S := 0 (flag 0) | no pending tagged waypoint and M holds no path of the record → 8 |
| 2 Speed Slave Only | as §5.4.2 | |

- **State 1 slave speed `0x5c69f0`:** max speed; only when S holds this record's path, is in a formation with
  speed Vf > 0 and the path has ≥ 2 points: min(max speed, Vf × path length / straight distance first-to-last
  point) (it makes up the detour's extra length).
- **Early detour pop `0x6807e0`** (state 1, only with +0xbc set): (S.P − M.P)·f_M < −(hx_S + hx_M) (`0x396a30`);
  S's locomotor owner is 0 or this record; and a layer-0x10 query along S.P → the detour's last point (record
  +0x58) with S's half extents, M and S excluded, returns no object S has a collision record with (`0x504ed0`)
  [M: the query's field roles]. Then S gives up the record id, **every collision-owned path of S is removed**
  (`0x559410`), state 8: S is back on its ordered path at once.
- **Pass hold lines (`0x682e80`):** B = S's swept box `0x677650` (§1.5.1 item 3: S's rectangle with both half
  extents + 1; a square of the larger extent along the wanted facing while S turns in place by more than 10°;
  else, when the closing speed along the line M → S is > 0, stretched from S's centre along its approximation by L
  = clamp((dist − r_M − r_S) / closing, 0, 45 frames) × S's max speed, axis a along that chord). E = hx_S + hx_M.
  **A.pt = B's rear centre + E·a, B.pt = B's front centre − E·a**, both with direction a: A lies hx_M − 1 ahead of
  S's start, B (chord + 1 − hx_M) ahead.
- **Starvation `0x681d30`** (state 2, after `0x67eb90`), exact: +0xb0 = 0 → true (state 0; the chooser then gives
  15). Else +0xac := none; aM, aS = acceleration per frame (`0x372fe0`; ≤ 0 → 0.05). Answered M (+0x84) < aM: when
  S's overall collision cap (`0x509ea0`) < aS → +0xb0 −= 1, +0xac := M. Else answered S (+0x88) < aS: when M's
  overall cap < aM → +0xb0 −= 1, +0xac := S. The countdown is **cumulative** (a good frame does not reset it);
  only state 15 re-arms it to 30.

**Corrections to §5.4.2:** (1) head-on tie-break: formation extent, then first-member ids, only when both are in
formations; otherwise the **slower** unit is M (not "formation size, then speed, then ids"). (2) Crossing: the
scattering-victim cases (crusher = M, state 1, no swap) were missing. (3) Record +0xbc is the gate of `0x6807e0`,
not a path-around kind. (4) State 1: "S no longer holds the detour → 8" applies only when no tagged waypoint is
pending; `0x6807e0` removes all of S's collision-owned paths. (5) State 7 is also the result of a failed
non-swappable Path Around (forced path, Giant, crusher), not only a crusher pair. (6) The mode-2 all-fail exit
leaves M and S swapped. (7) Starvation as above; state 15 retries its path-around every frame. (8) The relation
code's "always true at the decision" does not hold for the state 2 / 6 re-runs. (9) New: backward detours are
refused in state 4 (`0x67ee60`).

***Port spec (MovingVsMoving, vehicle pairs):*** decide once per record, in this order. (1) Same formation: the
front-slot unit is behind → swap slots if interchangeable, else it passes; otherwise slave the rear one. (2) One on
a forced path → the other detours, never swapped. (3) Same direction (facing dot > 0) and the rear one cannot
crush the front: the rear one **passes** when (r_a + r_b + distance) / (V_rear − min(V_front, V_rear / 2)) ≤
remaining path / V_rear, V_front ≤ V_rear, it is not Giant and not on its final approximation; otherwise it is
**slaved** to the front one (state 6: `0x67f130` speed, no hold line). (4) Giant against a non-Giant it cannot
crush, or that scatters → the other detours, never swapped. (5) By relation: p0 held or standing → p0 detours
(both run on when p1 can crush p0); head-on → a crusher with a non-scattering victim: both run on (state 7); else
the lower class detours (ties: the smaller formation, else the faster single unit); crossing → the one less far
ahead detours (crusher rules as listed); headings within 60° → slave the rear one. **Path Around:** the detourer
runs at max speed; the master keeps the slave-speed rule and **stops at once on its hold line** until the detourer
has passed the tagged corner. **Pass:** the passed unit runs freely for hx_M − 1, then at ≤ half the passer's
speed, and stops at once at (swept chord + 1 − hx_M) until it is r_M + r_S behind the passer. A failed pass →
follow at the slave speed until NOW + 30 + rnd[0, 15] (one synced draw); a failed detour (4 attempts, or 1 when
not swappable → state 7: both run on) → Try Again Later with the same timer (both brake to 0, the one that is
behind the other may run). [port] the +0xbc early pop, the starvation states 15 / 16 and pack +0x110 (0 without
corridor squeeze) may be simplified; record that.

**Retry frame of states 10 and 12 (s78n t23 RE, 2026-11-10; `0x682930`, `0x6814f0` case 12, `0x680d00`,
`0x3639d0`) [H].** In the service of the frame whose retry frame has passed, state 10 sets state 0 and the new
retry frame first and then still writes +0x84 / +0x88 (0 / max as in the row); state 12 writes M := max, S :=
`0x67f130` and then sets state 0. Neither runs case 0 that frame (the roles stay, possibly swapped; flags 0 from
the reset). These speeds are **dead**: the answer `0x680d00` returns the max speed whenever state = 0 (it still
writes the flag, 0 here), and the next reader is the speed fold of the **next** frame's object walk, which runs
before that frame's collision driver (`0x3639d0`: `0x2be640` ×2, then `0x50a780`); the choosers and starvation
tests of other records later in the same driver pass also read through `0x680d00`. So at a passed retry frame
both units get **max speed (not "at once") in the next frame's fold**, and the record decides in that frame's
driver (at once for a mode-0 result, under quota for a set-up, §5.4.1 "The stamp in full" item 5). The earlier
"this frame's speeds still apply" was wrong.

**A ring of held movers (s78n t23 RE, 2026-11-10; `0x682510`, `0x682170`, `0x6807e0`, `0x681d30`, `0x680ef0`,
`0x50a020`, §5.4.1 "The stamp in full") [H for each step, M for the long-run outcome: not observed in a run].**
- Roles. In state 1 the **held** unit H is M (speed 0 at its hold line, the waypoint of S's detour tagged with
  M) and the detourer D is S; in state 4 H is S (0 at line B) and D is M (the passer). The detour always lives on
  D, the claim (owner) is on D, and vf +0x78 is true exactly when a new path lands on H.
- A ring = records r1 … rn with D(rk) = H(rk+1) and D(rn) = H(r1), every H at or past its line, so every unit
  stands. Nothing inside states 1 / 4 ends it: no timer or retry frame; starvation `0x681d30` runs only from state
  2 (and the never-entered 14); the early pop `0x6807e0` (state 1, only with +0xbc) needs S behind M by hx_S + hx_M
  along M's facing, which a standing S never reaches; the Pass lines (+0x8c…+0xa8) are written only by `0x682e80`
  at a decision; validity holds (every unit has a path on its stack); the state-1 / 4 end tests need D to pass
  the tagged corner or M to pass S.
- **But a ring cannot be static in retail.** It forms by decisions, each pushing a detour onto its D. The push of
  the record decided last lands on the H of a record already in state 1 / 4 (D(rk) = H(rk+1)), so that record is
  stamped; it fires 2 frames later, the record is in state 0 for at least one fold (its H gets max speed from it,
  §5.4.1 item 6), re-decides (same roles when the geometry still gives them), re-claims D and pushes a new detour
  onto D = H of the next record, which is stamped in turn. The stamp walks round the ring forever, about 3 frames
  per record (more when the quota delays a set-up): each H gets at least one free fold per lap of ≈ 3n frames,
  and the hold lines are recomputed on each new detour. A record whose re-decision fails goes to 10 (both stand
  face to face; no push, the chain pauses) and re-decides after 30 + rnd[0, 15] frames, which restarts it; a
  re-decision into 2 / 3 / 6 / 7 / 12 replaces that link's hold line by that state's speed rule.
- **Other breakers:** a new order to any ring member (it owns a claim as some record's D, so `0x554480` destroys
  its detour without a stamp; that record ends at its next service and its H walks on, which unwinds the ring);
  the death or stop of a member (validity 8 / 9).
- **Port consequence.** A permanent three-unit hold in the port means its stamp chain is broken somewhere:
  excluding records by "who pushed" instead of by the role test above, stamping only when the record is not in
  state 1 / 4, deciding at once inside the stamped service (no free fold), keeping the held answer while the
  record is in state 0, or not stamping a re-push of the same detour. Retail re-stamps on **every** push onto H,
  including a re-decided record's fresh detour onto its D.

*Not read:* the id list `0x140b3a1a0` tested in `0x677650`; `0x677650` reads the pathfinder's request global as
"the other unit" [M: stale after an early return]; `0x509ea0` and `0x67f130` were not re-derived.

#### 5.4.7 TerrainCollision in full (s78n t20 RE, 2026-11-10) [H unless tagged]
Read from `0x509090`, `0x687db0`, `0x5c5f70`, `0x5c6240`, `0x5049e0`, the vftable `0x8b5568` (slots read from the
image: +0x08 `0x5c7d10`, +0x18 `0x687ea0`, +0x20 `0x687ff0`, +0x28 `0x6873f0`, +0x30 `0x687e80`, +0x38 `0x42c630`,
+0x40 `0x5c78e0`, +0x48 `0x18ceb0`, +0x50 `0x687e90`, +0x58 / +0x78 `0x13b7d0` (false), +0x60 `0x5c7700`, +0x68
`0x6874c0` save, +0x70 `0x687400` load, +0x10 / +0x80 `0x17dc70` (true)), the diagnostics node `0x687ae0`, the
driver `0x50a780`, `0x5c80e0`, `0x556780`, `0x5c5d90`, `0x5c4b80` / `0x5c4af0`, the Walk speed controller `0x61ccd0`
and move step `0x620820`, the container step `0x5adfb0`, the Bike step `0x63abe0`.

**What it is.** A one-participant collision record made when a unit's own step would end on ground it may not
enter. Its only action is one Path Around call (kind 3, no obstacle) back onto the unit's path; when that is not
possible the unit is **stopped and loses its paths**. There is no retry.

1. **Trigger: the failed-step report `0x509090`(collision system, unit).** Callers: the Walk move step `0x620820`
   (the normal step and the End Move coast), the land team container step `0x5adfb0` (the same two places) and the
   Bike step `0x63abe0`: a step whose **new position** fails `0x50e280` for the unit's class while the path it
   walks is not forced (+0x5c clear; a coasting unit: its top record, if any, not forced). The position is not
   committed that frame. Then, in this order:
   - the unit's pack speed **+0xec := 0** (at once; +0xf0 is left);
   - the system's record counter +0xb8 += 1 and a record is made by `0x687db0`(unit, id) [M: the id argument is
     not visible in the decompiled call; every other factory takes the fresh counter value];
   - the record is registered (`0x5049e0`: id-keyed list, and the unit's entry in the object-to-records hash).
     Factory failure → the unit is stopped (vf +0x140(0)).
   - **No count and no duplicate rule:** the first failed step makes a record, and every further failed step
     makes another one, whatever records the unit already has. Since the speed is zeroed each time and the
     decision normally comes at the next driver pass, a second record appears only when the first has to wait
     for quota [M].
2. **Record** (0x80 bytes; base `CollisionClass`): +0x38 id; +0x40 participants (the unit only; the record listens
   to the unit's signals and drops it from the list when it is destroyed, vf +0x08); +0x64 creation frame; +0x68 /
   +0x6c cached priority and its refresh frame; +0x70 re-decide stamp (never set for this class: vf +0x78 is
   false); **+0x78 state; +0x7c a frame stamp written with state 4 and never read**. Saved: the base, then chunk
   0x10000 with field 0 = +0x78 and field 1 = +0x7c (4 bytes each). Kind value (vf +0x48) 1.
   - States, with the names of the diagnostics node `0x687ae0`: **0 "None"** (new, waits for the quota pass),
     **1 "Resolving"** (its detour is on the unit's stack), **2 "Unresolvable"**, **3 "Resolved"**, and **4, which
     has no name and no handler** (item 5).
   - Answers to the unit: weight (vf +0x30) **4.0**, the highest of the four classes, so its Path Around claim
     always takes the unit's path ownership (§1.5.1 item 5); steering vector (vf +0x38) zero in every state;
     speed (vf +0x40) the unit's max speed with the "at once" flag untouched (FIGHTER_FLYING §7.7); "undecided or
     detouring" (vf +0x58) false; "takes the quota pass" (vf +0x50) true only in state 0; finished (vf +0x28) in
     states **2 and 3 only**.
   - Priority (vf +0x60, the base `0x5c7700` shared by all four classes): cached crowd − age in frames. The
     crowd term needs two participants, so for this class it is 0: **priority = −(NOW − creation frame)**, older
     records first in the ascending sort.
3. **Decision `0x687ff0`** (vf +0x20; the driver calls it in the quota pass while the dynamic pathfinder has
   quota, §5.4.1):
   1. state ≠ 0 → run the service (item 4) and return.
   2. No participant → state 3.
   3. The unit has no locomotor → state 2.
   4. The unit's **own position** fails `0x50e280` for its class → Stop (locomotor vf +0x140(record id), §5.4.1
      "The impassable-cell rule" item 3: the whole path stack is cleared) → state 2.
   5. Path Around `0x5c80e0`(record, unit, **obstacle = none, flag = 0, kind = 3, flags = 0**) (§1.5.1): the
      rejoin distance is D = r_u (`0x3969f0`) + **40.0**, so the first rejoin candidate is at path distance
      progress + r_u + 42; no forced obstacle; every standing unit is an obstacle (flags 0); the last-point drop
      is skipped; the pushed record has kind 3, owner = this record's id, is **not forced** and is started.
      - **Failed** (§1.5.1 item 7 and item 4 "Failure": among others the unit has **no kind-1 record** on its
        stack, e.g. an order-less pushed unit; it is on its **last waypoint**; no rejoin candidate is free; the
        flood fill does not reach the goal) → +0x7c := NOW + **45**, Stop (vf +0x140(record id)), state := **4**.
      - **Succeeded** → the unit's path record owned by this id (`0x556780`) gets its **waypoint entry 0 :=
        (1.0, 0.0)** (`0x5c5d90`; item 6) and state := 1.
        (No pack, or no record owned by this id after all: the state stays 0 and the
        decision runs again at the next pass.)
4. **Service `0x687ea0`** (vf +0x18; every frame for a record that is not in the quota pass, and for a state-0
   record when the quota is used up):
   - state 0 (waiting): no participant → 3; no locomotor → 2; the unit's position still passable → wait; else
     Stop (vf +0x140(id)) → 2.
   - state 1: no participant → 3; the unit has no locomotor pack → 2; some record on the unit's path stack is
     still owned by this id (+0x4c) → stay; else → **3**. So the record ends when its detour has been walked to
     its end and popped (§1.5.1 item 5), or was removed by a later claim, a new order or a stop.
   - states 2, 3, 4: nothing.
5. **Ends.** The driver destroys a record in state 2 or 3 at the end of the same pass. **A record in state 4 is
   never serviced and never finished**: no code reads +0x7c or leaves state 4, so the "45 frames" is not a retry
   timer in effect and the record stays in the system (with an emptied participant list once the unit dies)
   until the battle's collision system is destroyed. It still answers items 2's values for its unit: no speed
   cap, zero vector. [M] Side effect on the steering sum of FIGHTER_FLYING §7.5: the zero vector has the largest
   weight, so while such a record is kept as "best" no other record's vector is excluded by the −0.5 rule.
6. **The (1.0, 0.0) entry on waypoint 0.** A path record has one 8-byte entry per waypoint (defaults (1.0, 1.0);
   the first float is always 1.0: the corner pass writes ω / ω, §3.5.2 D). The second float (+4) is the waypoint's
   speed fraction. The speed controller
   `0x61ccd0` uses it twice: it brakes toward entry[index + 1].+4 × max speed ahead of the next waypoint, and it
   caps the speed at entry[index].+4 × max speed **while the unit is not yet aligned with the current segment**
   (the record's "aligned" byte +0x6c clear, the heading more than one frame's turn off the wanted one, state not
   Start Move); a cap of 0 sends the unit to End Move. With 0 on waypoint 0 the unit therefore **turns in place
   onto the detour's first leg before it moves**: at Start (§3.5 item 1) the "speed fraction > 0" test fails, so
   it goes to Start Turn even inside `Close_Enough_Angle_For_Move_Start`, and state 0x10 turns it exactly onto
   the heading (§3.5.1 item 4). Later waypoints of the detour keep (1.0, 1.0). Kind 3 also selects the tighter
   arrival-turn threshold of §3.5.1 item 2 when the unit stops on this path.
7. **What the unit and its formation see.**
   - Success: speed 0 at the failed step, then turn in place, then the detour; the ordered path stays underneath
     and is resumed at the first waypoint line not yet crossed (§1.5.1 item 5). Older collision-owned records
     (a MovingVsStopped / MovingVsMoving detour, a push path) are removed by the claim.
   - Stop (state 2 or 4): every path record is destroyed, the order path included. When the unit's path owner
     was free or this record: state 0x16 and speed 0 at once; when another collision record owned its path (an
     order-less pushed unit): the stack is emptied and the unit brakes through End Move. The formation then
     counts the member as done and, when all are, **signals arrival** as usual (§5.4.1 "The impassable-cell
     rule" item 4). No other event is raised [M: none was found]. A position move is over at that point; an
     attack block is re-planned by the 30-frame re-check.
   - There is no counter of blocked frames and no re-plan of the ordered path anywhere in this class.

***Port spec (TerrainCollision):*** when a passability-tested step fails: do not move, set the speed to 0, and
create a one-unit record (creation order = id order; no duplicate test). Decide it in the quota pass, oldest
first: unit gone → drop; own position impassable → stop the unit (clear all its paths) and drop; otherwise Path
Around with no obstacle, kind 3, D = r_u + 40: on success mark the detour's first waypoint "turn in place first"
and keep the record until no path of the unit is owned by it; on failure stop the unit (clear all its paths). A
record waiting for quota stops the unit when its position has become impassable. Retail keeps a failed record
for ever with no effect but item 5's; [port] dropping it at once is the simple choice (record it). The record
answers max speed, no vector, weight 4.

*Not read:* the frame order of the locomotor steps against the driver (whether a record made by a step is decided
in the same frame); `0x5c6970` (the base "give up ownership" helper, unused by this class); the Bike step beyond
its report call; whether anything outside the class walks the system's record list and would meet a state-4
record (the three other classes' partner tests need two participants).

### 5.5 Crush (`SquashBehaviorClass`, vf6 `0x629fa0`; vftable `0x8af300`) [H]

The `SQUASH` behaviour token is on AT-AT, AT-ST and MAL. Crush runs only while the crusher is moving (`0x39ade0` → locomotor vf +0x1c0).

**Crusher boxes:** the AABBs of the model's `CRUSHER` bones, or the type cache (+0x2d8) when `Cache_Crusher_Boxes` (+0x1b) is set. They are transformed to world space with the object matrix. Query radius = half of max(union of the boxes, object bbox).

**Victims:** for each player **in player-index order** whose relation to the crusher's owner is hostile (player +0x370 table == 1 [M]):
1. Query that player's objects around the crusher (`0x2a8fb0`).
2. A victim qualifies if (it has `Is_Squashable_By_Supercrusher` (+0x1a) and the crusher has `Is_Supercrusher` (+0x19)) or it has `Is_Squashable` (+0x18).
3. Unless the crusher is a supercrusher, a per-state permission (`0x397f60`) must also pass. [L]

**Effect:** if the victim's world AABB straddles any crusher box (classifier `0x20bb60` returns 8):
- the victim faces away from the crusher;
- it takes **1,000,000 damage** of `Squash_Damage_Type` (+0x36c) from the crusher (`0x3a9e30`).

Order does not matter because every hit kills.

**Not traced:** the `Damage_Crush` type (`0x5e1ac0`). `Scatters_From_Crushers` (faction +0x2f88) is read by `0x5c8890` (the victim's faction has it and the victim's SPREAD_OUT ability is off): it gates the collision system's crush-flee push and the MovingVsMoving crusher branches (§5.4.5 item 4, §5.4.6; s78n t19, 2026-11-10).

### 5.6 Spatial queries

- **Collision candidates** use the land object grid (§5.2).
- **Crush and other proximity queries** use GameObjectManager queries. These are the per-player TreeCull trees: HOTSPOTS §3, one tree per player at GOM +0xd0 + i·0x38.
- **Space planning** uses the time-sliced object-tracking trees (§2.2).

Three separate spatial structures exist. The port can unify them only if every query keeps its result semantics: the ORed bitmask for tracking, and a canonical order for the collision candidates.

---

## 6. Orders relevant to movement

### 6.1 Events (lockstep commands) [H]

**Wire format:**
- Every order is a `ScheduledEventClass` with the player ID at +0x24.
- Positions are quantised floats: codebook indices 1/2/3 for x/y/z and 6 for a facing (write `0x204bd0`, read `0x204800`, dequantise `0x23bc10`).
- A dequantised |z| < 1e-4 is forced to 0.

| event | read / execute | payload |
|---|---|---|
| MoveToPosition | `0x3acf70` / `0x3ad100` | position, u8 flags, 1 bit "switch locomotor" (+0x58; no FoC sender sets it, §3.1.1) |
| MoveToPositionFacing | `0x3ade20` | position, facing, flags, u32 count + IDs |
| MoveToRay | `0x3ae4e0` / `0x3ae6c0` | two points, flags (formation laid along a dragged line) |
| MoveToRayFacing | `0x3ae990` / `0x3aebb0` | two points, facing, flags |
| MoveToObject | `0x3aeee0` / `0x3af090` | target ID, count, unit IDs |
| MoveObjectToObject | `0x3af290` ctor, vf4 `0x3af410` | not decoded |
| MoveThroughObjects | `0x3c42a0` | u8 count + u32 IDs |
| MoveToGarrison | `0x3ad440` | position, flags, three IDs |
| StopMovement | vf4 `0x427190` | none (the player's selection) |
| SetupPhaseMove | `0x5aea90` | not decoded |

**Move-parameter block** (0x68 bytes) [H] for offsets, [M] for names:

| offset | content |
|---|---|
| +0x00 | destination |
| +0x0c | secondary point (ray start) |
| +0x18 | facing; 1e18 means "none" |
| +0x24 | type: 1 position, 2 ray, 3 object |
| +0x28 | target object |
| +0x48 | flags |
| +0x44 | final facing, yaw in degrees; read only when +0x5c is set (s78k t01) [H] |
| +0x4c | 0x3F (the avoidance mask) |
| +0x54 | 1e18 |
| +0x5c | byte: final facing set (s78k t01) [H] |

**Final facing (+0x44 / +0x5c), s78k t01** [H]. The path builder 0x417520 (formation entry, current block = the last
of the formation's block stack, 0x41a4e0) reads +0x44 only when byte +0x5c is set:
- land (formation kind 1): after the path is built, the facing vector (cos +0x44, sin +0x44, 0) is written into the
  facing (+0x0c) of the path's **last waypoint** (0x550bd0). The flag-0x20 branch (below) does the same on its
  2-point path. The unit's own path is a copy of the formation path (locomotor 0x42c680 → 0x5c46a0, which copies the
  waypoints with their facings), and the Walk locomotor turns to it on arrival (§3.5.1);
- space (kind 2): the planner request gets goal facing = +0x44 and "final facing required" (§2.1 record +0xec /
  +0x120); with +0x5c set the arrival slow-down distance is tripled;
- the formation's final rotation (0x411980, §4.4) also uses the last block's +0x44 when its +0x5c is set.
With +0x5c clear every waypoint facing stays (0, 0, 0) and no final turn happens. The +0x18 triple (1e18 = none) is
**not** what the path builder reads for the final facing [M: its role was not re-traced]. Writers seen: the
TransportLanding vehicle exit move (0x644330: +0x44 = the landing's yaw, +0x5c = 1); the team exit move writes 0.

**Flag 0x20 path (land)** [H, s78k t01]: 0x417520 builds no route. The formation path is the 2 points start →
destination (when 0x50e180 rejects the destination, 0x440f90 must place it, else the move fails), and the waypoint
list's byte +0x44 := 1, which becomes path +0x5c, the skip-passability flag of §3.5 step 4 (0x5c5df0 / the copy in
0x54e4e0). This is the "forced-move" path flag that the unload finish clears (0x3ac210, A1_READINGS §28.9).

**Forced-path destination check and fallback (s78l t02 c, 2026-11-05)** [H unless tagged]. In 0x417520's flag-0x20
branch, D = the formation destination (block +0x00, after the formation offset):
1. **Check** 0x50e180(grid, D, class = formation +0x1c4, layer 0): cell (x, y) = trunc((origin + D) / cell size)
   with the globals 0x140b36b98…ba4 (as 0x50e280, §1.1); a cell outside the grid reads 0xFFFFFFFF. Passes when
   (cell bits & class mask 0x477b70(class)) = cell bits, i.e. every bit set in the cell is allowed for the class. Only
   the point is tested: no line, no occupancy.
2. **Fails → place**: a search record (0x440be0, A1_READINGS §40.1) for the formation's object (param 2) with:
   mode **1** (full circle: no cone, no minimum distance), centre = D (x, y, z), start angle = the object's yaw
   (+0x8c, from the ctor), +0x5d = 1, **+0x5e = 1 (skip occupancy and path tests)**, +0x5f = 0 (point test
   0x50e180 instead of reachability), movement class +0xa8 = formation +0x1c4, margin = `FormationSpacing` × 5
   (unused here). 0x440f90 then (A1_READINGS §40.2): land mode only (else it fails); rings while index < 20:
   R = 0, 1, 2.8, 5.4, 8.8, … (R += step; step := min(step + 0.8, min(1.5 r, 10)), r = the object's footprint
   radius √(ex² + ey²) of its hard extents, 5 when 0); samples n = trunc(R × 2π / r) + 1 (ring 0: one sample = D),
   from ring 1 on n = max(4, n); sample i at angle yaw + i × 360 / n, counter-clockwise, at distance R from D.
   The first sample that is inside the map box (land vf +0x1f8 centre / half sizes) and passes the point test of
   step 1 (skipped when the class is −1) is taken; **no occupancy or path test** (+0x5e). The found point keeps D's z.
   No synced draws. r = 0 with R > 0 would divide by zero; land units always have extents [M].
3. **The path** is start → the found point (D itself when step 1 passed), 2 waypoints, skip-passability flag set; a
   final facing (+0x5c) goes on waypoint 1 as above.
4. **No point in 20 rings** (or not land): 0x417520 returns 0 and the formation gets **no path**: the move fails
   and the unit does not move [M: what the caller does next with the failed formation was not traced].
*Port spec:* test D's cell against the movement class; when blocked, walk the rings above around D from the unit's
yaw and take the first in-map passable sample; otherwise fail the forced move; the path then skips the per-frame
passability check on the way (§3.5 step 4).

**Flag bits:**
- 0x02: attack/engage. [M]
- 0x08: queue as a waypoint (shift); also enables the random start delay of §1.6. [H]
- 0x20: skip destination validation. [M]
- 0x80: double-click "run" with speed-matched recruitment (§4.2). [H] Input, wire and full bit table: P1_DOUBLE_CLICK_MOVE.md (it corrects 0x02 to guard-move; attack-move is 0x01).
- Also (2026-11-03, FIGHTER_FLYING §4.11): 0x04 player order (added by the waypoint append; decides pops on arrival and in the formation update); 0x10 space idle-movement block (speed capped at `SpaceIdleMovementSpeed`; `ShouldUseSpaceIdleMovement` is false in FoC); 0x40 raise signal 0x2a on pop. Bytes: +0x50 push class [M], +0x58 expiry frame with +0x61 "timed", +0x5d each member closes at its own speed, +0x5e patrol loop. [H for the readers]

### 6.2 Dispatch [H]

**MoveToPosition and MoveToRay:**
- With flag 0x08 clear, they call the **group move** (`0x2a7b80`).
- With it set, they call the **waypoint-queue move** (`0x2a8240`).
- Either way they then record the order for waypoint display (`0x32ea20`).
- With the 1-bit flag set (MoveToPosition only), every selected unit with a locomotor and `Uses_Multiple_Locomotors` first swaps to its secondary (`0x3a9910(unit, system, 0)`, §3.1.1). Vanilla never sets the bit.

**Group move** (`0x2a7b80`):
1. Create a coordinator.
2. Walk the player's **selection list in list order** (`0x2ad080`). For each unit:
   - skip it if it is movement-locked or its locomotor is suspended;
   - *s78n t26 RE 2 [H]:* skip it entirely when it is deployed (`0x39a390`: `Deploys`, a locomotor, +0x2b3 set or
     Walk state 0x19) and not an auto-deployer (`0x39a320`(x, 1)): no order cleared, no coordinator, no `0x2adff0`
     (the waypoint queue `0x2a8240` has the same test; details §3.2.1 item 2);
   - clear behaviour orders 5, 0x22 and 0x29, and 6 when the attack is cleared;
   - replace a team member by its team container (object +0x2b0);
   - run the admission test `0x425e30` (has a locomotor, not a locked container member, not garrisoning).
3. Admitted units leave every other coordinator (`0x426d50`) and join this one. Others are moved without a coordinator (`0x2adff0`).
4. For a position move in land, the destination snaps to the centre of a selected object whose type has `Snap_Movement_Orders_To_Center` (+0x8d) when the click falls within its radius.
5. Finally `Coordinator::Move_To` (`0x4f8200`):
   - refuse if any member is locked (`0x39ad40`);
   - copy the parameters;
   - for object targets, subscribe to the target's death and replacement signals;
   - for each formation in order, `0x4105a0` then `Formation::Set_Move` (`0x423c10`).

**Waypoint queue** (`0x2a8240`): a unit already in a coordinator (`0x426140`) appends the parameters to that coordinator (vf +0x30), once per distinct coordinator in first-seen order. Units with no coordinator form a new one.
- vf +0x30 (`0x4f7770`, land and space) calls `0x420f70` on each formation. Each formation keeps a **stack** of move blocks (F +0xf8; the last block is the current one). `0x420f70` inserts the block at the **bottom** with flag 0x04, so it runs after everything already queued. Limit `MaxWaypointsPerPath` (GC +0x13f0, FoC 6) blocks with 0x08; a click within 20 (land) / 80 (space) of an earlier player waypoint closes a patrol loop (byte +0x5e) instead. Push, pop, arrival and the guard pop: FIGHTER_FLYING §4.11 B (s78f t02, 2026-11-03). [H]

**MoveToObject:** no coordinator. Each unit (the listed ones, or the whole selection when the count is 0) gets `0x39f580(unit, target)` directly.

**StopMovement:** the selection, copied in list order, is passed to `MovementCoordinatorSystem` `0x4261f0` (cu8j, 2026-11-08) [H]. For each object: vf4 remove on every coordinator; then, in land or for a space object whose role (`0x397900(obj, 1)`) is 0, restart the locomotor and, only when the type has +0x3d4 (`MoveAttackWhileStopped`, in no FoC XML), adopt it (`0x39bcb0`, §4.7: one ctor draw at once); a space object with a nonzero role instead gets a self-move through the group move `0x2a7b80` (a new coordinator; its formation and ctor draw come at the next coordinator update, §4.6.2).

### 6.3 Target death, attack-move, guard

- **Target died** (coordinator vf1 `0x4f77e0`):
  - on a replacement signal, the new target is swapped into every formation's goal;
  - on death, the goal becomes a position: the last waypoint of each formation's path. [H]
- **Attack-move** (Lua `Attack_Move` `0x5730e0`) and **Move_To** (`Move_Internal` `0x57da90`) build the same parameter block (type 1, or 3 for an object), create one coordinator, add the units and call `0x4f8200`. [H]
  - The attack-while-moving behaviour comes from flag 0x02 and from per-formation attack staging (`0x40e260`) [M].
  - `MoveAttackWhileStopped` (+0x3d4) and `Is_Stationary_When_Attacking` (+0x26) are type flags that the combat side consults (TACTICAL_COMBAT.md).
- **Guard** (`SpaceTaskForce` `0x6e72c0`) and **Divert** (`0x575710`; fails when movement is locked) were **not traced**.
- `Disable_Movement` / `Suspend_Locomotor` (`0x5815e0`) suspend the locomotor behaviour (behaviour suspend count, §3.3).

---

## 7. Determinism checklist (for the port)

| item | retail behaviour | port requirement |
|---|---|---|
| Path open lists | binary heap: sift-up stops at ≤, sift-down needs strictly smaller, left child wins ties | replicate exactly, or every path may differ |
| Closed/visited sets | integer/ID-keyed hashes with generation stamps; probed only | any container is fine as long as it is never iterated |
| Region build order | raster scan, creation-order indices, adjacency in build order | replicate the order (it fixes neighbour order) |
| Formation rank sort | MSVC `std::sort` (unstable) | stable sort plus object-ID tie-break; may diverge from retail on exact ties |
| Collision priority sort | `std::sort` ascending by crowd − age (TACTICAL_GAPS.md §6.7); insertion sort ≤ 32 entries | stable sort by (priority, insertion order) — exact up to 32 per frame |
| Collision candidates | land object-grid order (not traced) | sort by object ID |
| Object service order | object list order; behaviours in reverse index order | keep both orders; parallel jobs must write per-object outputs and commit in this order |
| RNG draws | synced stream `0x140a13e24`: jiggle per moving frame, start delays, PathFrameDelay stagger, hyperspace offsets | same count and order of draws |
| Normalisation | fast inverse sqrt (0x5f375a86, 2 Newton steps), `sqrtf` | replicate bit-exactly; no FMA |
| Planner scratch | static globals (`0x140b39420…478`, `0x140a43b70…78`): not re-entrant | per-request scratch for parallel planning; commit results in request order |
| Hyperspace arrival offset | may read camera-derived globals | confirm by probe; must be sim-only |
| Pointer-keyed iteration | **none found** in any movement, formation or collision path | keep it that way |

---

## 8. Coverage and gaps

**Named entry points.** SUBSYSTEMS.md lists 0 named entry points for pathfinding_movement and physics_collision. This document names:

| area | entry points |
|---|---|
| Passability | grid `0x5116c0`, test `0x38e390`, recompute `0x59f4b0` / `0x59f110`, stamping `0x4f88b0` (service `0x4f90c0`, unstamp `0x4f90f0`, load broadcast `0x2a8730` → `0x394510`), death-clone behaviour strip `0x29f270`, movement classes `0x477900` / `0x477c10` / `0x477b70` |
| Land search | `0x5a2830`, `0x5a2e00`, `0x5a3020`, `0x5a2200`, `0x5a0550`, `0x59fef0`, `0x5a4130`, `0x5a50d0`, region build `0x50fcd0` |
| Dynamic pathfinder | `0x673b10`, `0x675240`, `0x67a0d0`, `0x67a3d0`, `0x679740`, `0x67cef0` |
| Space planner | `0x59a900`, `0x59d4a0`, `0x596d40`, `0x599170`, `0x599380`, `0x597dc0`, `0x59caa0`, `0x59e5b0`, retries `0x417520` |
| Locomotors | factory `0x4c3520`, Walk vf6 `0x61e930`, speed `0x61ccd0`, move `0x620820`, conform `0x42bb40`, SimpleSpace vf6 `0x626420`, Fighter vf6 `0x5cb830`, speed getters `0x371160` / `0x376990` / `0x372fe0` / `0x372560` |
| Formations | `0x43e5b0`, `0x43c1b0`, `0x43aac0`, `0x41a740`, `0x41ece0`, `0x411980`, `0x42d440`; creation (cu8j): ctor `0x40d0d0`, factory `0x4f6fb0`, `0x426eb0`, `0x4f8080`, space vf12 `0x5d58f0`, `0x5d4730`, `0x4f7360`, `0x4f84e0`, `0x4f7530`, adopt `0x39bcb0`, `0x425e30`, `0x3a9880`, `0x2a5a60`, `0x29e960`, `0x2a00b0`, `0x4fbb10`, `0x4261f0` |
| Collision | `0x508170`, `0x50a780`, `0x683790`, `0x686500`, `0x684f90`; crush `0x629fa0` |
| Orders | §6.1 table, `0x2a7b80`, `0x2a8240`, `0x4f8200` |

**Not traced or unresolved:**
1. `LandZonePathFinderClass` (`.ted` `0x10B` zones, `0x3b54d0`) and the zone-connectivity test `0x50ee10`. (The AI land zone graph's Edge_Weight / Node_Weight are resolved: TACTICAL_GAPS.md §6.1.)
2. Start/goal snapping `0x5a1910`.
3. Path post-pass `0x5a0d20`, region line-of-sight `0x5a5810`, clearance probe `0x59fd90`.
4. Where the region-dirty flag is consumed.
5. Data tables that need a binary read:
   - 4-direction table `0x140a43bc0` and 8-direction table `0x140a43b80`;
   - Hermite basis `0x140b2f1f0`;
   - hyperspace profile `0x140b27a10`;
   - k-means cap `0x140a28bc8`;
   - rank-tolerance cap `0x140a28b78`;
   - corridor step `0x140b2f0f4`;
   - global speed factor `0x140a28568`;
   - team slot threshold `0x140a4431c`.
6. Whether land ground units ever use the lattice planner of §2.1 (its land branches exist).
7. Formation speed computation; formup wait styles; `MaxLandFormationFormupFrames`.
8. ~~Space formations, `Merge_Formation`, `Split_From_Formation`~~ (cu8j: §4.6, §4.7). Still open: the fleet placement geometry of `0x4fca30` / `0x4fdfb0` / `0x4fd5b0` / `0x4fea10` (only their adopt draws are read), `Squadron_Formation_Error_Tolerance`, the space pass re-run after a max-speed-0 seed (§4.6.4).
9. ~~MovingVsMoving resolution~~ (resolved 2026-11-10: §5.4.2; MovingVsStopped in full, the driver and the Walk use of the steering vector: §5.4.1), (push and return: resolved, §5.4 / A1_READINGS §40), ~~`TerrainCollisionClass` trigger~~ (resolved 2026-11-10, s78n t20: §5.4.7, the whole class), ~~footprint field semantics~~ (the three contact tests and the layer-0x10 records: §5.3.1 / §5.3.2). Resolved 2026-11-10 (s78n t19): the vehicle branches of the MovingVsMoving chooser (§5.4.6), the spare point `0x5c6ba0` / `0x67b4c0` and the whole Path Around call (§1.5.1), StoppedVsStopped (§5.4.3), the early-arrival cut (§5.4.4), the decision's leftovers (§5.4.5), the order-time destination check (§4.3.1). Still open there: `0x685490` (state 8), the frame order of the tracker update against the collision update, the `0x5a2830` request flags of the StoppedVsStopped path, the dynamic pathfinder's tracker internals and terrain-corner pruning masks (§1.5.1 "Not read"). (The collision **priority sort** of §5.4 is resolved: ascending priority = crowd − age, crowd = 5 × objects in a 160 × 160 box; TACTICAL_GAPS.md §6.7.)
10. Bike, Flying, JetPack and LandBomber locomotors, LocomotorCommon `0x5070b0` (the shared turn core that reads `Min_Speed_Fraction_For_Turn` +0x3ac), `Stopped_Rate_Of_Turn` (+0x3b0) readers, fighter attack runs and team slot geometry `0x62d620`.
11. Hyperspace: the camera-dependence of arrival staggering, `Hyperspace_Speed` readers, `Cinematic_Hyperspace_In` / `Resume_Hyperspace_In`.
12. Guard, Divert, MoveObjectToObject and SetupPhaseMove payloads.
13. The service order of the collision system relative to the object walk within a frame.

Answered later in REMAINING_GAPS: §4.1 corridor squeeze (step `0x140b2f0f4` = 30); §4.2 ops 25–32, threat mode and the
8-direction table; §4.3–§4.7 the jetpack, flying, bike, land-bomber and fighter locomotors (items 6 and 10). Fighter attack
runs are still open (§4.8).
Fighter attack runs, dogfights, Idle / Dead_Stop, the cell grids, flying attitude / altitude / circling, the timed
follower vf +0x1d8, slot geometry `0x62d620` and a summary of `0x5070b0`: FIGHTER_FLYING.md (also resolves the Hermite
basis, `0x140a28568` = 1.0 and `0x140a4431c` = 4.0 of item 5).

**Probe suggestions** (Gabriel launches; `tools/probe_mod/`):
- Breakpoint `0x5a3020` and log the region path for a fixed Endor move. This checks the region build and the tie rules.
- Log `0x59a900` expansions for a capital ship.
- Read the static tables listed in item 5 from a memory dump.
