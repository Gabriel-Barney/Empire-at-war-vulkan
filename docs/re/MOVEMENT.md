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

**`Fine_Tune_Occupied_Passability`** (type +0x94): each sample additionally needs a vertical ray (z +1000 → −1000) to hit the object's collision mesh. This trims footprints to the real mesh.

**Bridges:** no dedicated bridge code was found. Bridges are ordinary objects whose `OverridePassability` / secondary passability re-opens cells. [L]

**Death clones:** `IsDeathCloneObstacle` (+0xcf) and `Death_Clone_Is_Obstacle` (+0x92) make wrecks obstacles. They are handled by the collision decision (§5.4). Grid stamping of wrecks was not traced. [M]

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

### 1.6 Request budgets, staggering and re-path triggers

**No global per-tick cap on land path requests was found.** The only hard budget is the dynamic quota of §1.5. [M]

**Start staggering** [H]:
- In `0x43c1b0`, each formation's start frame = now + accumulated delay + a synced random offset − 2·fps. The delay accumulator grows by each member type's `PathFrameDelay` (+0x3D0). [M] for the exact formula.
- With the queue/shift flag (§6.1), `0x41c1a0` gives each unit a synced random start delay in [0, **Max_Move_Frame_Delay = 6**] frames.
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
- `SpaceObstacleBehaviorClass` (vftable `0x8b0988`; vf3/vf13 `0x632610`, vf15 `0x6326b0`) inserts the object as a static entry on attach and removes it on detach.

**Layers** (`0x397900`):
- The unit's layer is `Space_Layer` (+0x16dc), or `Stationary_Space_Layer` (+0x16e0) while not moving.
- SpaceLayerType (`0x5e0c00`): Capital 1, Frigate 2, Corvette 4, StaticObject 8, Land 0x10, SuperCapital 0x20 (corrected per FIGHTER_FLYING §6.3.1).
- **A ship plans only around entries in its own layer, plus the static map (layer 8).** Ships in different layers pass through each other in planning.
- A multi-locomotor unit whose secondary locomotor is active uses layer 4. [M]

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
| JETPACK_LOCOMOTOR | JetPack (`0x8b2e80`) | Boba Fett, Bossk, IG-88 (secondary) |
| LANDBOMBER_LOCOMOTOR | LandBomber (`0x8b3568`) | bombing runs |
| FIGHTER_LOCOMOTOR | Fighter (`0x8a6198`) | space fighters/bombers; birds on land |
| SIMPLE_SPACE_LOCOMOTOR | SimpleSpace (`0x8aeaf8`) | corvettes … capitals |
| TEAM_LOCOMOTOR | Team (`0x8aff88`) | space squadrons |
| FLEET_LOCOMOTOR | Fleet (`0x899c58`) | galactic fleets |
| STARSHIP_LOCOMOTOR / SIMPLE_LOCOMOTOR | Starship (`0x8ae250`) / LocomotorBehaviorClass (`0x8adda0`) | none in vanilla FoC [M] |

**Two locomotors** (`0x3a9910`) apply only when `Uses_Multiple_Locomotors` (+0x74) is set:
- The unit swaps between `Primary_Locomotor_Name` (+0x2058) and `Secondary_Locomotor_Name` (+0x2078).
- Going to the secondary saves the unit's coordinator order and leaves the coordinator.
- It is triggered by the 1-bit flag of MoveToPosition (§6.1).

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
| +0x2a8 | formation-member flag (§5.3) |

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

### 3.4 Speed, acceleration and turn rate [H]

**Effective max speed** (`0x371160`, then `0x376990`):
- Base: `Max_Speed` (+0x37c).
- For a team/container (group behaviour 0x16), the base is capped at the **minimum Max_Speed of its members** (`0x4049e0`).
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
- Units with `Walk_Transition` (+0x35) derive acceleration from the frame counts of their start/end-move animations instead. [M]

**Turn rate** (`0x372560`) = `Max_Rate_Of_Turn` (+0x38c) × the mode multiplier × the global factor. The infantry turn-animation time = angle / rate × 1/30 (`0x62bef0`).

**Defaults:** from OBJECT_TYPES.md, Max_Speed 0.25, Max_Rate_Of_Turn 2.3, Mass 0.995.

### 3.5 Walk locomotor per frame (vf6 `0x61e930`, land only) [H]

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
4. **Passability check before commit.** Unless the path's skip flag is set, the new position must pass the movement-class test of §1.1 (`0x50e280`). On failure, report to the collision system (`0x509090`) and stop this frame.
5. **Normalisation.** Vector normalisation uses the fast inverse square root (magic 0x5f375a86, two Newton steps). **Reproduce this bit-exactly**: it feeds positions.
6. **Formation jiggle** (`0x42c270`). Each moving frame, the offset += synced random(±k) × speed fraction × `FormationRaggedness` (+0x16a8), clamped to ±`MaxJiggleDistance` (+0x16ac). Each call draws from the lockstep RNG.
7. **Hover rise/lower.** z moves by the effective max speed per frame toward terrain + `Hover_Height` (+0x23bc). Deploying states run at `Deployment_Anim_Rate`.

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
- **Members** (LandTeamInfantry service `0x62ca10`):
  - copy the container's walk states 0x10/0x13/0x14 into their own data pack, and recompute idle states (`0x62bbf0`);
  - steer to a slot computed by `0x62d620` (slot geometry not traced).
- **Recovery:** when the team is idle and a member is farther than the threshold `0x140a4431c` from its slot, the member enters Recovery Turning (0x29) or Recovery Moving (0x2A). The recovery turn rate is multiplied by the fleeing bonus.

### 3.8 Layer_Z_Adjust (+0xdfc) [M]

- **Land:** not a walker field.
- **Galactic:** fleet moves add it to the goal z (`0x39f580`).
- **Flying/legacy starship locomotors:** use it as the cruise altitude, or a floor for it (`0x622b80`).
- **Space formations:** use it as the slot z (§4.3).

### 3.9 XML fields with no runtime reader found

- `Walker_Acceleration_Magic_Number` (+0x1d50): parsed (`0x36c000`) and zeroed (`0x31b590`) only.
- `MaxObstacleCostSpace`, `ObstacleAreaOverlapForMaxSpace`, `UseLinearCollisionChecks`, `MaxObstacleCostLand`.
- `MaxLandFormationFormupFrames`, `Formation_Formup_Wait_Style`: not found in the formation code (§4.4).
- `RepushDistance`: no reference at its expected address.

Any of these may have readers that were not found. Parse them and treat them as inert until a probe shows otherwise.

---

## 4. Formations

### 4.1 Objects [H]

`MovementCoordinatorSystemClass` (mode vf +0x228) owns a vector of coordinators:
- `LandMovementCoordinatorClass` (ctor `0x438ad0`) in land;
- `SpaceMovementCoordinatorClass` (`0x5cf3b0`) in space.

Both are 0xb8 bytes and are created per order by `0x426010`, appended in creation order. A coordinator owns a vector of `FormationClass` objects (0x290 bytes, ctor `0x40d0d0`, made by `0x4f6fb0`).
- Each formation gets a global incrementing ID (`0x140a2bcf0`, stored at +0x288).
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
2. **Destination.** The clamped order point, validated as a passable cell for the seed's MovementClass and OccupationStyle (`0x50f480`, search radius 30) unless flag 0x20 is set. Locomotors that answer vf +0x240 use the position finder at `0x1409cf220` instead.
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
6. **Attack orders.** Units already within weapon range + **Solo_Attack_Range = 60** go solo. "Short-range" is set when distance < range × **Short_Range_Attack_Formation_Coefficient = 1.6**. [M]

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
   - Land: the slot's world position is quantised to the grid (`0x423ec0`).
   - Space: slot z = the member's `Layer_Z_Adjust`.
   - Offsets live in an **object-ID-keyed hash** (formation +0x108…+0x130; insert `0x423ae0`, lookup `0x41a8d0`). It is never iterated.
5. **Footprint check** (`0x40eb90` → `0x43a070`) [M]:
   - The destination footprint fails if non-members overlap ≥ **15%** of the formation area, with **BetweenFormationSpacing = 5** added to each footprint.
   - On failure, retry once. If that also fails, signal the members (signal 10) and set formation state 3.

**Port note:** the rank sort is unstable. Use a stable sort with a final tie-break on object ID. Exact retail tie order cannot be reproduced without MSVC's introsort; ties are rare because of the name tie-break.

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

**Formup:**
- Land formations skip formup when **ShouldSkipLandFormup = true** (vanilla) and formation +0x9c is clear (`0x41c1a0`). In vanilla, land groups therefore start without waiting.
- FormationFormupWaitType (`0x5dddc0`): WaitForAll 0, WaitForFront 1, WaitNone 2. Neither it nor MaxLandFormationFormupFrames (240) was found being read. [L]

**Space formations:** `Set_Capital_Ship_Formation` `0x4fca30`, `Set_Screen_Formation` `0x4fdfb0`, `Set_Lead_Formation` `0x4fd5b0` and `Set_Transport_Ship_Formation` `0x4fea10` (sole caller `0x4fbb10`), and `Merge_Formation` `0x41fa90` / `Split_From_Formation` `0x4f84e0`, were **not read**. The debug strings show that space groups lay out capital, screen, lead and transport roles. `Squadron_Formation_Error_Tolerance` is unread. Gap, §8.

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
| TerrainCollisionClass | `0x8b5568` | `0x687db0` (trigger not traced) |

**There is no rigid-body physics.** Collision means detecting a future conflict along the planned paths and resolving it by behaviour.

### 5.2 Detection (`0x508170`, per frame) [H]

For each registered mover, **in registration-list order**:
1. Skip a multi-locomotor unit whose active locomotor answers vf +0x240.
2. **Path-approximation countdown** (locomotor data +0x275). At ≤ 0, rebuild the approximation (`0x505fe0`, using ApproximationSmoothCosAngle 0.9962 and ApproximationForwardDistance 2.0) and reset the countdown to **FramesPerPositionApproximationRebuild = 8**. Otherwise decrement it and run `0x50aad0`.
3. **Collision-check countdown** (+0x274). Check when it is ≤ 0 or the approximation was just rebuilt, then reset it to **FramesPerCollisionCheck = 4**. Each unit is therefore checked every 4 frames, phase-shifted by its own history.
4. **Candidates** come from the land mode's object grid (mode vf +0x238, sub-object +0x10, query `0x5d7a00`), **not from TreeCull**:
   - an axis-aligned **160 × 160** square centred on the unit;
   - results go into a global vector (`0x140b31c50`) in grid order;
   - **the grid's internal order was not traced. A parallel port must sort the candidates canonically (e.g. by object ID) and accept that this may differ from retail.**
5. **Pair filter.** Skip the pair if any of these holds:
   - it is self;
   - neither side has a locomotor;
   - either side has flag +0x3a7;
   - one side is attached to the other (object +0x150);
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

**Vanilla class counts:** Infantry 123, Vehicle 55, Medium 20, High 19, Landing Transport 10, Giant 9, Large 8, Build Pad 6, Low 3, Bike 1.

### 5.3 Contact tests [H]

**MovingVsStopped** (`0x683790`):
- Gates: shield barriers (behaviour 0x36) and garrison/attachment exclusions.
- Formation members (+0x2a8 == 1) collide only with stopped formation members.
- d = predicted distance along the path to contact (`0x441b60`, path +0x200). Hit when d / max speed < fps × max(MovingVsMovingLookAheadTime, the type's `MovementPredictionInterval` (+0x3c8)), plus 1.5 s if the mover can push the obstacle (`0x5c8900`, `0x5c8890`).

**StoppedVsStopped** (`0x686500`):
- Same gates.
- Infantry beside a non-team Large or Giant vehicle needs a non-zero push vector (`0x5c6d40`).
- Then a footprint-box overlap ratio (`0x20d7a0`) > **MinimumStoppedVsStoppedOverlapCoefficient = 0.15**.

**Footprints:** soft radius `Custom_Soft_Footprint_Radius` (+0x16b8); hard box `Custom_Hard_XExtent`/`YExtent` (+0x16bc/+0x16c0) with offsets (+0x16c4/+0x16c8) and deployed variants (+0x16cc/+0x16d0); `MovementBoxExpansionFactor` (+0x16d8). The exact use of each was not traced. [M]

### 5.4 Resolution [H] for the branch order, [M] for meanings

**State names** (debug strings):

| class | states |
|---|---|
| MovingVsStopped | 0 None, 1 Path Around, 2 Repulsion, 3 Existing Path, 5 Resolved, 6 Unresolvable, 7 Try Again Later (4 and 8 unnamed) |
| MovingVsMoving | 0 None, 1 Path Around, 2 Speed Slave Only, 3 Repulse, 4 Pass, 5 Infantry Push, 6 Moving With Speed Only, 8 Resolved, 9 Unresolvable, 10 Try Again Later |
| StoppedVsStopped | 0 None, 1 Resolving, 2 Resolved, 3 Unresolvable, 4 Try Again Later (re-check frame at +0x80) |

**MovingVsStopped decision** (`0x684f90`), first match wins:
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

**Speed slaving:** the collision system's speed cap (`0x509ea0`) is applied in the walk speed controller (§3.5). This is how "Speed Slave Only" and "Moving With Speed Only" take effect. [M]

**Processing order:**
- Each frame, active collisions are sorted by a priority float and resolved in that order under the dynamic quota (§1.5).
- The MovingVsMoving resolution methods (`0x677650…0x6814f0`), `RepushDistance = 6` and `MinimumPushReturnDistance` (type +0x3cc, readers `0x1d6290`/`0x1d7cf0`) were **not traced**.
- Recommended behaviour pending a probe: a pushed unit steps aside along the push vector and returns once the pusher has passed by at least MinimumPushReturnDistance. [L]

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

**Not traced:** `Scatters_From_Crushers` (+0x2f88), which presumably makes infantry flee ahead of a crusher, and the `Damage_Crush` type (`0x5e1ac0`).

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
| MoveToPosition | `0x3acf70` / `0x3ad100` | position, u8 flags, 1 bit "switch locomotor" |
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
| +0x4c | 0x3F (the avoidance mask) |
| +0x54 | 1e18 |

**Flag bits:**
- 0x02: attack/engage. [M]
- 0x08: queue as a waypoint (shift); also enables the random start delay of §1.6. [H]
- 0x20: skip destination validation. [M]
- 0x80: double-click "run" with speed-matched recruitment (§4.2). [H] Input, wire and full bit table: P1_DOUBLE_CLICK_MOVE.md (it corrects 0x02 to guard-move; attack-move is 0x01).

### 6.2 Dispatch [H]

**MoveToPosition and MoveToRay:**
- With flag 0x08 clear, they call the **group move** (`0x2a7b80`).
- With it set, they call the **waypoint-queue move** (`0x2a8240`).
- Either way they then record the order for waypoint display (`0x32ea20`).
- With the 1-bit flag set, multi-locomotor units swap locomotor first (§3.1).

**Group move** (`0x2a7b80`):
1. Create a coordinator.
2. Walk the player's **selection list in list order** (`0x2ad080`). For each unit:
   - skip it if it is movement-locked or its locomotor is suspended;
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

**MoveToObject:** no coordinator. Each unit (the listed ones, or the whole selection when the count is 0) gets `0x39f580(unit, target)` directly.

**StopMovement:** the selection, copied in list order, is passed to `MovementCoordinatorSystem` `0x4261f0`, which removes the units from their coordinators and stops their locomotors. [M] for the stop details.

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
| Passability | grid `0x5116c0`, test `0x38e390`, recompute `0x59f4b0` / `0x59f110`, stamping `0x4f88b0`, movement classes `0x477900` / `0x477c10` / `0x477b70` |
| Land search | `0x5a2830`, `0x5a2e00`, `0x5a3020`, `0x5a2200`, `0x5a0550`, `0x59fef0`, `0x5a4130`, `0x5a50d0`, region build `0x50fcd0` |
| Dynamic pathfinder | `0x673b10`, `0x675240`, `0x67a0d0`, `0x67a3d0`, `0x679740`, `0x67cef0` |
| Space planner | `0x59a900`, `0x59d4a0`, `0x596d40`, `0x599170`, `0x599380`, `0x597dc0`, `0x59caa0`, `0x59e5b0`, retries `0x417520` |
| Locomotors | factory `0x4c3520`, Walk vf6 `0x61e930`, speed `0x61ccd0`, move `0x620820`, conform `0x42bb40`, SimpleSpace vf6 `0x626420`, Fighter vf6 `0x5cb830`, speed getters `0x371160` / `0x376990` / `0x372fe0` / `0x372560` |
| Formations | `0x43e5b0`, `0x43c1b0`, `0x43aac0`, `0x41a740`, `0x41ece0`, `0x411980`, `0x42d440` |
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
8. Space formation layouts (`0x4fca30`, `0x4fdfb0`, `0x4fd5b0`, `0x4fea10`), `Merge_Formation`, `Split_From_Formation`, `Squadron_Formation_Error_Tolerance`.
9. MovingVsMoving resolution (`0x677650…0x6814f0`), push and return (`RepushDistance`, `MinimumPushReturnDistance`), `TerrainCollisionClass` trigger, footprint field semantics. (The collision **priority sort** of §5.4 is resolved: ascending priority = crowd − age, crowd = 5 × objects in a 160 × 160 box; TACTICAL_GAPS.md §6.7.)
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
