# Space avoidance tree (GOM +0x558) and the precise model test (StarWarsG.exe FoC x64 v1.121)

This spec closes the open rule behind the fighter locomotor's obstacle-avoidance hook: which objects the avoidance
query sees, with what boxes, in what order, and what the per-candidate precise test needs.

It **supplements** (does not replace):
- `FIGHTER_FLYING.md` §3.5 (avoid, 0x5c8b70, steps 2–4 and the step 6 headless note) and §11;
- `TACTICAL_COMBAT.md` §1.5 (tree table, GOM+0x558 row), §1.6 (query order) and open item 15 (the precise test
  0x38f900 and its mask bits).

Sources: the Ghidra 12.2 export in `re_reference/`, vtables read from `corruption\StarWarsG.exe`, and a scan of the
FoC XML (`extracted/foc_data_text.zip`) and the ALO models in the FoC/EaW MEGs (`tools/meg`, `tools/formats/python/alo.py`).

**Conventions** (same as FIGHTER_FLYING.md): addresses are RVAs; **[H]** read directly, **[M]** inferred from
structure or partly read, **[L]** guess. "GOM" = GameObjectManagerClass (current game mode `0x140b15418` +0x18).
P = object position (obj +0x78). "Model" = the object's ModelClass (obj +0x2a0); "render model" = the ModelClass's
render object (ModelClass +0x48, an `alHModel` for ALO models). **[port]** marks a recommendation, not binary behaviour.

---

## 0. Summary for the implementer

1. **Membership** (§2) [H]. An object is in the space avoidance tree iff, when it is created or registered in a
   **space** battle:
   - its type is not `Is_Asteroid_Field`, `Is_Ion_Storm` or `Is_Nebula`, and
   - its type's `Space_Layer` bitfield has any bit other than `Land` (16), i.e. one of Capital 1, Frigate 2,
     Corvette 4, StaticObject 8, SuperCapital 32.

   Membership is decided once and only ends when the object is destroyed. No owner, alive/dead, hidden, garrison or
   visibility test is applied at insertion or in the query.
2. **In FoC data** (§2.5): capital ships, frigates, corvettes, transports, non-fighter heroes, star bases, defence
   satellites, mining / sensor / jamming stations, docks, construction props and asteroids (`ASTEROID *`,
   `DESTROYABLE_ASTEROID_*`, `FAKE ASTEROID FIELD *_NONPASSABLE`). **Not members:** every fighter, bomber and
   squadron (all 35 `FIGHTER_LOCOMOTOR` users have no `Space_Layer`), death clones, asteroid fields, nebulae, ion
   storms, backdrops, projectiles, hard points. So **the fighter itself is never a candidate**; no self-exclusion is
   needed.
3. **Member box** (§3) [H]: the world AABB of the model's **collision bound**. That bound is the union of the
   collision-enabled ALO meshes' boxes, in bind pose and model space (it includes the hidden `shield` bubble mesh). It
   is transformed by the object's world transform, scale included. An object without a model gets a 0.1-unit box at
   P. Boxes are refreshed whenever the object's transform is flushed (§3.3).
4. **Query** (§4) [H]: the box is centred on the fighter with half-extent |V| on every axis. Overlap is inclusive.
   Results come back in TACTICAL_COMBAT §1.6 order (reverse collection order), which is confirmed for this tree. So
   **only obstacles whose AABB is within one frame's travel of the fighter are even tested**: avoidance engages when
   the fighter is at or inside an obstacle's bounding box.
5. **Precise test 0x38f900** (§5) [H] for each candidate:
   - **No model:** never a hit.
   - **The model has a `COLLISIONBOX` mesh:** segment vs that mesh's box, as an oriented box in model space. A start
     point inside the box sets the hit record's "inside" byte. Among FoC members only `EM11_HYPERVADER` (EV_TIEADVANCED)
     has one.
   - **Otherwise:** segment vs the **triangles** of every collision-enabled ALO mesh whose runtime collision mask has
     bit 1. Hull meshes keep the default mask 1. The `SHIELD` mesh of a unit with the shield behaviour gets mask 2 or 0,
     so the shield bubble is ignored. This path is gated by a model-level "collide" switch: on by default, off while
     the object is inside a container or garrison.
   - A hit needs a fraction t in [0, 1) (box) or (0, 1) (triangles). The candidate's **position**, not the hit point,
     is what avoid steers away from.
6. **Hit-record byte 1 is not "the fighter itself"** (§5.6, corrects FIGHTER_FLYING §3.5 step 4 [M] and the
   `FighterAvoidFn` comment). It is the "segment starts inside a COLLISIONBOX" flag. The hit record is shared across
   candidates and never reset, so once one candidate sets byte 1, no later candidate can trigger avoidance in that
   call.
7. **[port] stand-in** (§7): use the ALO collision meshes: triangles if `alamo_assets` exposes them, otherwise
   per-mesh oriented boxes. Exclude the `SHIELD` mesh for units with the shield behaviour, and order candidates by
   ascending object id. Without model data the hook can still run on the type's model bounding box (a coarser
   stand-in). Without any model data it stays unset, which matches "no model ⇒ no hit".

---

## 1. The tree [H]

- **Owner:** GOM +0x558, a `TreeCullClass` (0x38 bytes, ctor 0x20e370). It is the same class as the player trees
  (TACTICAL_COMBAT §1.2–§1.4: insert vf1 0x20e600, remove vf2 0x20e6d0, Update_Bounds vf4 0x20f0a0, Query_AABB
  0x20e780).
- **Creation:** GOM init 0x2af130 allocates it only when the game-mode id (mode vf +0xe0) is 2, i.e. space.
  - In land and galactic GOMs the pointer stays null and avoid returns false (FIGHTER_FLYING §3.5).
  - The pointer is cleared in the GOM ctor and dtor (0x2c4370, 0x2c5b30).
- **Rebuild / clear:**
  - map load and reinit 0x2af4f0 (rebuild with the map box ×1.1 and the space-only thresholds from map vfuncs
    +0x130/+0x138);
  - GOM reset 0x29c800 (clear via 0x20e550);
  - the per-frame `Rebuild_All_Dirty` pass, as for every tree (TACTICAL_COMBAT §1.4).
- **Readers:** the fighter avoid 0x5c8b70 is the only reader of GOM +0x558 found. The full-text scan for `+ 0x558)`
  on the GOM finds only the sites in §2 and §1. So "space avoidance" is its whole purpose.
- **Per-frame GOM update 0x2be640** runs the debug dump 0x20ed70 on the player trees and GOM+0xd8, but not on +0x558.
  This has no gameplay effect.

## 2. Membership

### 2.1 Insertion [H]
There are exactly two insertion sites, both with the same condition:
- `GOM::Create_Object` 0x29f810 (every runtime spawn);
- `GOM::Register_Object` 0x2b5ad0 (save load 0x2b0a20, objects re-registered into a GOM).

```
L = Get_Space_Layer(obj, stationary_ok = 1)        (0x397900, §2.2)
insert iff  tree != null
        and type.Is_Asteroid_Field (+0x54) == 0
        and type.Is_Ion_Storm     (+0x56) == 0
        and type.Is_Nebula        (+0x57) == 0
        and (L & ~0x10) != 0                        (any layer bit except Land)
```

- The inserted item is the object's CullObject sub-object (obj +0x28). Readers recover the object as item − 0x28.
- The test is evaluated **once**. No other site inserts into +0x558. Owner change (0x2b3ac0) only moves player-tree
  membership.
- A later type, layer, locomotor or state change never adds or removes membership. Upgrades that swap objects (UC_
  → built) create a new object [M].

### 2.2 `Get_Space_Layer` 0x397900 with stationary_ok = 1 [H]
1. No type → 0.
2. If the type has `Uses_Multiple_Locomotors` (+0x74), the object's locomotor behaviour (behaviour slot 1) answers
   true to vf +0x240, and its vf +0x238 returns 2 → return 4.
   - This is a land multi-locomotor case. No FoC space unit uses it [M].
3. Otherwise return `Space_Layer` (type +0x16dc).
   - With argument 1 the `Stationary_Space_Layer` (+0x16e0) branch is never taken.

`Space_Layer` facts:
- It is an XML bitfield: None 0, Capital 1, Frigate 2, Corvette 4, StaticObject 8, Land 16, SuperCapital 32
  (OBJECT_TYPES §757).
- The type reset 0x367250 sets the default to **0**.
- `Variant_Of_Existing_Type` copies it from the parent (0x36c000).
- There is no other writer, so a type without the tag has layer 0 and is never a member.

### 2.3 Removal [H]
- **Destroy:** `GOM::Destroy_Object` 0x2a6a00 calls the tree's remove (vf2) for +0x558, together with the player and
  all-player trees and GOM+0xe0. Remove is a swap-remove, and a no-op when the object has no link in that tree
  (0x20e6d0 walks the object's membership list).
- **Unregister:** `GOM::Unregister_Object` 0x2b68d0 does **not** touch +0x558 or +0xe0. It removes only the player
  trees, the all-player tree, the id map and the master list.
  - Its callers are two niche paths (0x3bac00 and 0x6f5d50).
  - An object unregistered this way keeps its +0x558 link until destroyed [H for the code, M for any gameplay
    effect].
- **GOM reset** 0x29c800 clears the tree.

### 2.4 What does *not* affect membership [H unless tagged]
- **Death:** the death handler 0x39bdb0 sets obj +0x3a0 bit 0x40 and does not touch the trees. A dying object stays a
  candidate until destroyed, and its model keeps colliding. Death-clone types (`*_DEATH_CLONE`) have no
  `Space_Layer`, so a clone is never a member.
- **Garrison / containment:** 0x37ade0 (enter container) leaves the object in the tree. It switches the model-level
  collide flag off (§5.5), so the precise test fails. The flag is switched back on by 0x37a060 / 0x37a8d0.
- **Ownership, fog, stealth, selection, hyperspace states:** no test anywhere on this path. Friendly and enemy
  obstacles are avoided alike.
- **Under construction:** under-construction types (`UC_*`) are ordinary members with their own build models.

### 2.5 FoC data [H, from the XML and ALO scan]
- **Member types:** 493 types pass the rule. Many are ground structures whose `Space_Layer` is non-zero, but they are
  never spawned in space. The space-side members are:

| group | examples | layer |
|---|---|---|
| capital / super | Star Destroyer, Mon Cal Cruiser, Executor, Eclipse, Kedalbe, Krayt | 1, 32 |
| frigates | Nebulon-B, Acclamator, Victory, Interdictor, Assault Frigate, MC30, Vengeance, Interceptor IV, Venator / Jedi cruiser | 2 |
| corvettes / transports | Corellian corvette & gunboat, Tartan, IPV, Broadside, Marauder, Crusader, shuttles, landing craft, AT-AT barge, freighters | 4 |
| heroes (non-fighter) | Home One, Accuser, Admonitor, Arc Hammer, Moldy Crow, Sundered Heart, Peacebringer, Tantive IV | 1, 2, 4 |
| static | star bases (all factions, skirmish), defence satellites (laser, missile, repair, tractor, DBM, plasma, sensor), mineral extractors and pads, sensor pods, gravity-well / jamming stations, long-range scanners, docks, construction pods, `STAR_DESTROYER_CONSTRUCTION`, `MON_CAL_CRUISER_UNDER_REPAIR`, `ECLIPSE_PROP`, `DEATH_STAR_CHUNKS`, asteroids (`ASTEROID SMALL/MEDIUM/LARGE/HUGE`, `COLLIDE_ASTEROID_HUGE00`, `DESTROYABLE_ASTEROID_*`), `FAKE ASTEROID FIELD LARGE/MEDIUM_NONPASSABLE` | 8 |

- **Not members:** all fighters, bombers and their squadrons, including the hero fighters (Slave I, IG-2000, Rogue
  Squadron, …) and every type using `FIGHTER_LOCOMOTOR` (35 references, none with a layer). Also death clones,
  backdrops, planets and moons, asteroid fields, nebulae, ion storms, projectiles and hard-point types.
- **Member models:** the space-side members use 82 distinct ALO models.
  - 81 have at least one collision-enabled mesh (ALO mesh info `402` +36). All 169 such meshes carry a `1200`
    collision tree.
  - 70 models have a hidden, collision-only mesh; 11 collide on visible meshes only.
  - 34 models carry a hidden collision-enabled `shield` bubble mesh. Its runtime mask is §5.4.
  - `w_planet_DeathStar02_II_chunks.alo` has no collision mesh, so it is never hit.
  - Only `EV_TIEADVANCED.ALO` (type `EM11_HYPERVADER`) has a `COLLISIONBOX` mesh. Across all 1,957 ALOs, 58 have one,
    mostly fighters, infantry and small vehicles, which are not members.

## 3. Member box and updates

### 3.1 The box stored in the tree [H]
Insert and Update_Bounds copy `GameObject::Get_World_AABB` (CullObject vf1 = 0x395c00 → 0x3982b0), as centre and
half-extents:
1. If the object has a TEAM behaviour (slot 0x16): the team list's box (C +0x110 → +0x8c centre, +0x98 half; zero
   with no list). Team containers have no layer, so this case never applies to this tree.
2. Else if there is no model (obj +0x2a0 == 0): centre = P + 0.05 and half = 0.05 on each axis, i.e. the box
   [P, P + 0.1]. It is computed as ((P + 0.1) + P)·0.5 and |(P + 0.1) − P|·0.5.
3. Else ModelClass 0x264ab0, an OBB → AABB conversion:
   - local box (c, h) = ModelClass vf +0x58 (0x264920) = the render model's **collision bound** (alHModel +0x120,
     §3.2). It is centre 0 and half 1 when the render object is not an alHModel;
   - M = ModelClass vf +0x38 (0x264a60) = the render model's world matrix (3×4). It includes the object scale
     (0x265d80 multiplies the scale into the matrix);
   - world centre_i = M_i0·c.x + M_i1·c.y + M_i2·c.z + M_i3;
   - world half_i = |M_i0·h.x| + |M_i1·h.y| + |M_i2·h.z|.

### 3.2 The collision bound (alHModel 0x12def0) [H]
- alHModel keeps two local boxes, recomputed by 0x12def0 after load, user-bound changes and base-shield toggles:
  - **+0x108, render bound:** the union over all sub-objects except lights, plus the type's `User_Bound_Min/Max`
    when set (0x12db90, called from 0x3a4820);
  - **+0x120, collision bound:** the union over sub-objects whose collision bit (+0x30 bit 7) is set. User bounds are
    not included. With no collision sub-object it is centre 0, half 1.
- Each sub-object box is its local mesh box (ALO `402` bounding box) taken through its bone's bind-pose transform.
  So the collision bound is the **bind-pose union of the collision-enabled meshes**, including the hidden `shield`
  bubble when the model has one.
- The scale comes from type `Scale_Factor` (+0x658), or from obj +0x240 → +0x40 when that override exists (set in
  0x3ac530 through 0x265d80).

### 3.3 When the stored box changes [H; M for the list identities]
- **Insert:** the box at creation. The model already exists, because the object ctor 0x388b60 runs 0x3989a0, which
  creates the model (0x3a4820) and flushes the transform.
- **Transform flush** 0x3ac530:
  1. rebuilds the object matrix at obj +0x248 from P and the orientation copy at +0x90..+0x98;
  2. pushes it and the scale into the model;
  3. walks the object's tree memberships (obj +0x30 list) and calls each tree's **Update_Bounds** with the new world
     AABB.
  - Only objects **with a model** update their boxes; a model-less object keeps the box from insertion.
- **When the flush runs:**
  - in the per-object update 0x3a6b80, right after the object's own behaviours (locomotor included), when its
    transform-dirty bit (obj +0x3a1 bit 0, set by the non-forced position and orientation setters) is set;
  - unconditionally for the GOM list +0x138 in the GOM update 0x2be640 [M: which objects that list holds];
  - forced (argument 1) by teleports, model swaps and attachment.
- **Consequence:** during one frame, an obstacle updated earlier in GOM update order shows this frame's pose and one
  updated later shows last frame's pose. The model's world matrix used by the precise test (§5) follows the same
  rule.
- Update_Bounds itself (TACTICAL_COMBAT §1.3) only moves links up the tree; node boxes never refit.

## 4. The query and its order [H]
- **Box:** avoid builds centre = ((P − s) + (P + s))·0.5 and half = |(P + s) − (P − s)|·0.5 per axis, with
  s = |V| (FIGHTER_FLYING §3.5 step 2).
- **Overlap test** (0x20bb60, used for nodes and links):
  - disjoint iff on some axis |c_q − c_l| > h_q + h_l (strict), so touching boxes are candidates;
  - "node fully inside the query" iff on all axes |c_q − c_n| < h_q − h_n (strict).
- **Order:** TACTICAL_COMBAT §1.6 holds for this tree unchanged.
  - The collector 0x20e780 / 0x20fef0 walks pre-order with the node's own links first, then child 0, then child 1.
  - It pushes each result onto the head of the tree's result list at tree +0x08 (link +0x18 = next), so avoid walks
    them in **reverse collection order**.
  - Within a node the order is node-vector order, which swap-removes and migrations permute. The result is
    effectively arbitrary but deterministic for a given history.
- **Reach:** with s ≈ 1–10 units, a candidate's AABB must lie within s of the fighter's position. So the long
  look-ahead segment (Tl·V, FIGHTER_FLYING §3.5 step 3) is only tested against obstacles the fighter is already
  touching or inside, by bounding box.
- **Not reentrant:** results live on the shared links. There is no owner filter, no garrison filter, and no
  fighter-specific filter.

## 5. The precise test 0x38f900 (ModelClass collision) [H unless tagged]

### 5.1 Query block and hit record as built by avoid
- **Query block** (passed by pointer):

| off | value in avoid | meaning |
|---|---|---|
| +0x00 | → hit record | shared by every candidate in the loop |
| +0x08 | 0 | out: the render object that was hit |
| +0x10 | 0 | "bounds only" flag (alHModel tests its render box instead of meshes) |
| +0x14 | 1 | **collision mask** |
| +0x18 | segment P → E | origin, end, delta, unit dir, len², len (0x14e100) |

- **Hit record:**

| off | init | meaning |
|---|---|---|
| +0x00 | 0 | want hit point (0: no point written) |
| +0x01 | 0 | **inside**: the segment start is inside a tested box (§5.6) |
| +0x04 | 1.0 | best fraction t so far; a hit must be strictly below it |
| +0x0c | (0, 0, 1) | hit normal (world) |
| +0x18 | 0 | hit point (written only when +0x00 ≠ 0) |

### 5.2 Dispatch
1. **No model** (obj +0x2a0 == 0) → false.
2. **COLLISIONBOX flag set** (ModelClass +0x8c) → box path (§5.3).
3. **Otherwise** → the render object's collide, vf +0x80. For an alHModel that is 0x12c9c0 (§5.4). If the ModelClass
   has no render object → false.
4. On a hit the render object is stored in query +0x08.

### 5.3 COLLISIONBOX path (0x20c850)
- **Setup** (0x2660f0, when the ModelClass gets its render object [M: timing]):
  - if the render object is an alHModel with a top-level sub-object named `COLLISIONBOX` (case-insensitive), the
    ModelClass stores that sub-object's box at +0x90 (centre, half);
  - the box is computed with the model's root set to identity, so it is in model space; the root is then restored;
  - +0x8c = 1.
  - The same pass records the indices of the `SHIELD` and `SURFACEFX` sub-objects (+0xa8, +0xac).
- **Test:**
  1. Invert the model world matrix (0x22d410; it divides by each column's squared length, so scaled rotations are
     handled).
  2. Transform the segment into model space.
  3. Run a slab ray-box test (0x20cd70) against [c − h, c + h], with the direction = the segment delta (not
     normalised).
- **Inside:** the start point is in the box on all three axes (inclusive) → set hit-record byte 1 = 1 and return
  **true**. t and the normal are not changed.
- **Outside:**
  - for each axis, the candidate plane is min if o < min, max if o > max, else none;
  - t = (plane − o) / d (−1 when there is no plane or d = 0);
  - take the largest t (first maximum); miss if t < 0;
  - the hit point on the other two axes must lie in [min, max] (inclusive).
  - Then hit only if t < record.t. On a hit, store t and the face normal (table 0x140a14c60), transform the normal
    back to world, and return true.
- The model-level collide switch and the mesh flags are **not** consulted on this path.

### 5.4 Mesh path: alHModel 0x12c9c0 → alRenderableMesh 0x14c520 → submesh 0x13e900
1. **alHModel:**
   - if its collide switch (+0x30 bit 7, §5.5) is off and the query is not "bounds only" → false;
   - otherwise call collide (vf +0x80) on **every** top-level sub-object in list order, and OR the results;
   - lights and other non-mesh sub-objects return false (0x13b7d0).
2. **alRenderableMesh** (one ALO mesh). It returns false when any of these holds:
   - the runtime flag +0x30 bit 1 is set [M: a runtime hide; the ALO `is_hidden` bit is bit 0 and does **not**
     block collision];
   - the mesh's collision bit (+0x30 bit 7, from ALO `402` `is_collision_enabled`) is clear;
   - (query mask & mesh mask +0xc0) == 0.

   Otherwise:
   - **Broad phase:** the segment against the mesh's world AABB (0x20c6d0 with a private record, t₀ = 1, inside
     counts). A miss → false.
   - The segment is transformed into the mesh's local space (inverse of its world matrix).
   - Each submesh is tested (0x13e900):
     - with a collision tree (ALO `1200`; all FoC member collision meshes have one), by tree traversal
       (0x1a26f0) [M: traversal not read; it yields the triangles whose leaf boxes the segment touches];
     - without one, by brute force over all triangles.
   - Per triangle (0x20c410): a two-sided segment–triangle test.
     - |det| ≥ 1e-4 (absolute);
     - barycentrics u ∈ [0, 1], v ≥ 0, u + v ≤ 1;
     - **0 < t < record.t**;
     - on a hit, store t and the normal (cross product of the two edges, normalised) and return true.
   - Hits are ORed over submeshes. The hit point and normal are transformed back to world. Byte 1 is never set on
     this path.
3. All tests use the shared record, so within one candidate the surviving hit is the nearest. Only the boolean
   matters to avoid.

### 5.5 Model-level collide switch (alHModel +0x30 bit 7) [H]
- **Default on:** the alHModel ctors 0x12b080 and 0x12b330 set it, and clones copy it (0x13f660).
- **Switched by** ModelClass 0x264670 (and save/load 0x264cd0):
  - **off** when the object enters a container or garrison (0x37ade0, which also hides the model and sets obj
    +0x3a7), and in a few object-specific paths (projectile 0x4ceea0; others at 0x2faae0, 0x3d3200, 0x3ed800 and
    0x4eb020 not traced);
  - **on** again when it leaves (0x37a060, 0x37a8d0).
- The switch gates only the mesh path (§5.4), not the COLLISIONBOX path.

### 5.6 Collision mask bits (mesh +0xc0) [H; M for bit 4]
- **Default:** every renderable starts with mask 1 (ctor 0x13f590; clones copy it). Avoidance queries with mask 1,
  so it tests **hull** meshes.
- **The `SHIELD` mesh of a unit with the shield behaviour** (ShieldBehaviorClass, 0x549cd0, called from its
  vf9/vf10 and its update 0x549810): mask = 2 while shield points > 0 and shields are not disabled, else 0. So the
  shield bubble never takes part in avoidance, though it is part of the collision bound (§3.2).
- **Base shields** (BaseShieldBehaviorClass 0x56b470 / 0x56b540) set 2 or 4 on the `SHIELD` mesh, and switch its
  collision bit and visibility.
- **Projectiles** query with 7 (`Projectile_Does_Shield_Damage`) or 5 (TACTICAL_COMBAT §3.4). That is consistent
  with bit 1 = hull, bit 2 = ship shield, bit 4 = the base-shield variant [M for 4].
- A `SHIELD` mesh on a model whose type has no shield behaviour keeps mask 1 and is avoided like hull [M; no such
  FoC member was checked individually].
- Meshes merely named `shield_*` (for example `shield_wing_l`) are not the `SHIELD` sub-object and keep mask 1.

### 5.7 The meaning of hit-record byte 1 (correction) [H]
- Byte 1 is set only by the box tests 0x20c850 and 0x20c6d0 when the segment **starts inside the box**. In the
  avoid call it comes only from a COLLISIONBOX candidate; the mesh broad phase uses a private record.
- avoid's record is created once before the loop and never reset, and an accepted hit returns at once. So between
  candidates record.t stays 1.0, and byte 1 is the only state carried over.
- **Effective rule:** walk the candidates in list order. The first candidate whose precise test returns true decides:
  - byte 1 == 0 → avoid it;
  - byte 1 == 1 (the fighter starts inside a COLLISIONBOX) → skip it, and since byte 1 stays set, every later hit is
    rejected too. Avoidance does nothing this frame.
- Fighters are never members (§2.5), so byte 1 is **not** a self-exclusion. In FoC it can only come from
  `EM11_HYPERVADER`.

### 5.8 Edge cases [H]
- **Zero velocity** (s = 0): E = P. The triangle and slab tests reject a zero-length segment, except the "inside" case
  of a COLLISIONBOX, which latches byte 1. So a stopped fighter never avoids.
- **Model without an alHModel render object:** the bound is ±1 around the model origin (§3.1), and collide is that
  class's vf +0x80, false for non-mesh classes.

## 6. avoid 0x5c8b70 restated with this document [H]
1. If GOM +0x558 is null (not space) or the pack is missing → false.
2. Run Query_AABB(box centred on P, half |V|) on +0x558 (§4).
3. Segment S = P → P + Tl·V (FIGHTER_FLYING §3.5 steps 1 and 3). The record and query are §5.1.
4. For each link in list order, with c = link item − 0x28:
   - if precise(c, S) (§5) and byte 1 == 0: steer away from c.P (P − c.P, or (1, 0, 0) when equal), set the hold
     counter to 15 and return true.
5. Otherwise run the hold-counter fallback (FIGHTER_FLYING §3.5 step 5) and return false.

## 7. Port recommendations [port]
Inputs available headless: types (XML), positions and orientations, `Scale_Factor`, and the ALO assets through
`alamo_assets` (mesh info `402`: bbox, hidden, collision; bones; the `1200` trees and triangles).

1. **Membership set:** apply §2.1 to the type at spawn, in space battles only. Remove on destroy. Do not re-evaluate.
   Fighters, squadrons, projectiles and hard points never qualify, so no self-exclusion is needed.
   - Keep garrisoned or contained members, but make their precise test false (§5.5).
2. **Candidate box:** the world AABB of the bind-pose union of the type model's collision-enabled meshes (§3.2),
   under the object transform with `Scale_Factor`.
   - A model with no collision mesh: ±1 around the model origin. No model: [P, P + 0.1].
   - The port's `MapStampHooks::model_half_extents` source (model bounding box) is an acceptable coarser substitute
     when collision meshes are not loaded.
   - Use the committed positions at query time. Retail mixes this frame's and last frame's poses (§3.3), and that
     cannot be reproduced in a parallel-safe way.
3. **Candidate order:** sort the overlapping members by ascending object id (TACTICAL_COMBAT §0.1 / §1.6 port rule).
   Then apply §5.7's first-hit rule: the first candidate whose test hits decides.
   - The retail order is tree-shape dependent, and the port's TreeSet will not match it. The difference only shows
     when one segment crosses two obstacles that both touch the fighter's box in the same frame.
4. **Precise test:**
   - **Exact:** segment vs the triangles of each collision-enabled mesh of the model, in bind pose (bones' rest
     transforms), under the object transform. Skip the `SHIELD` mesh when the object has the shield behaviour.
     Hit iff 0 < t < 1, two-sided, |det| ≥ 1e-4.
   - **Acceptable stand-in:** the same meshes as oriented boxes (each mesh's `402` bbox in model space). A box hit is
     t ∈ [0, 1) from outside; a start inside the box counts as a hit with t = 0.
     - Do **not** latch byte 1 for these boxes: in retail the inside-latch exists only for COLLISIONBOX models.
     - For mesh boxes the retail behaviour would be "hit" whenever a triangle is crossed, which a box over-reports
       slightly.
   - **COLLISIONBOX models** (only `EM11_HYPERVADER` among FoC members): implement §5.3 including the byte-1 latch,
     or treat it like the stand-in [port, negligible].
   - **No model or no collision mesh:** no hit.
5. **Hook result:** return the candidate's position (obj P), as `FighterAvoidFn::hit_pos` already expects. The hit
   point is not used.
6. **Determinism:** all of the above is pure arithmetic on committed state in id order, so it satisfies rule 3. The
   triangle test has no data-dependent iteration order once candidates and meshes are ordered (mesh order = ALO
   order).

## 8. Corrections to existing notes
- **FIGHTER_FLYING §3.5 step 4** "[M: excludes a segment starting inside, i.e. the fighter itself]": wrong reason.
  Byte 1 is the "starts inside a COLLISIONBOX" flag and it latches for the rest of the loop (§5.7). Fighters are not
  in the tree. The same correction applies to the comment on `FighterAvoidFn` in
  `engine/tactical/movement/include/alamo/tactical/movement/fighter.hpp`.
- **FIGHTER_FLYING §3.5 step 6** "the precise test needs model collision meshes": correct. More exactly, it needs
  collision-enabled ALO meshes with mask bit 1, or a COLLISIONBOX mesh (§5).
- **TACTICAL_COMBAT §1.5 GOM+0x558 row:** correct. Add: decided once at create/register; removed only by destroy
  (not by Unregister 0x2b68d0).
- **TACTICAL_COMBAT open item 15** (the precise test and its mask bits): answered by §5 (mask 1 hull, 2 ship
  shield, 4 base-shield variant [M]).
- **FIGHTER_FLYING §1.5 team list `+0x8c..+0x94` "spread vector S" [M]:** in 0x405320, team list +0x8c..+0xa0 is
  the team's AABB (centre +0x8c, half +0x98), returned as the TEAM container's world box. The S in §4.6 may be that
  box's centre rather than a spread vector [M; owner of FIGHTER_FLYING to check].

## 9. Still open
1. 0x1a26f0, the collision-tree traversal: it is assumed to visit exactly the triangles whose leaf boxes the segment
   touches. Whether it stops early or prunes by record.t affects nothing avoid uses (the boolean is the same) [M].
2. The meaning and writers of mesh +0x30 bit 1 (the runtime "no collide / hidden" bit checked by 0x13fc10) [M].
3. The identity of GOM list +0x138 (objects whose transform is flushed unconditionally every frame) [M]. It does not
   affect the port recommendation.
4. The exact meaning of mask bit 4 (base shield, "both flags" case of 0x56b540) [M].
5. When exactly 0x2660f0 runs relative to the first tree insert (COLLISIONBOX setup) [M]. Only EM11_HYPERVADER is
   affected.
6. Nothing here blocks implementing the hook. The only data dependency is that the port's asset layer exposes, per
   model, the collision-enabled meshes (bbox, bone, triangles) and the `SHIELD` sub-object.
