# Tactical combat: spatial tree, targeting, weapons, modifiers, hardpoints and abilities (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by an RE agent, ahead of roadmap steps **S6 "Spatial tree"**, **S8 "Combat & modifiers"** and
**S9 "Hardpoints & abilities"**. This is a clean-room behavioural spec from the Ghidra 12.2 decompilation. It
contains no code. RVAs are relative to image base 0x140000000. Confidence: **[H]** read directly and
cross-checked, **[M]** read once or partly inferred, **[L]** inferred or guessed. "Synced RNG" means the
lockstep GameRandom (§0.2). FPS means the integer logic rate global at 0x140b0a340, which is **30** [M/H; two
independent readers agree, and every seconds→frames conversion below uses it].

Related docs, not repeated here: `HOTSPOTS.md` §3 (tree internals, cost) and §5 (modifier call sites),
`STRUCTS.md` (TreeCullNode/TreeCullQuery/CullLink layouts), `SPACE_AND_OPEN.md` §B.9–B.10 (hardpoint
destruction **visuals**), `PRIOR_PATCH.md` (which of these functions the old patch hooked).

The roadmap risk said these subsystems had no named entry points. §7 lists the roughly 110 entry points
identified in this pass. §8 lists what is still open.

---

## 0. Cross-cutting rules

### 0.1 Determinism summary (read this first)
- **Every gameplay random draw in combat uses the synced RNG**, including some that look cosmetic: hit-particle
  picks, shield-hit-particle picks and the first-shot cooldown. The port must make every draw listed in §0.2,
  in the order given. Skipping a "visual" draw desynchronizes everything after it.
- **Several choices are "first found wins on exact ties"**: target acquisition, area-damage victim order,
  first-hit projectile, opportunity fire and the priority-set property match. In the original these depend on
  the cull tree's traversal order (§1.6). The tree's layout itself depends on insert/remove history, because of
  swap-remove and rebuild timing.
- **Recommendation for the port** (we need determinism across our own clients, not bit parity with
  StarWarsG.exe): every gameplay query should return its candidates in a canonical order (ascending object id,
  §1.5) before any first-wins scan, sort or capped take. The tree can then be any structure, including a
  parallel BVH. Results stay order-stable because the sorts are by id, never by pointer. The only behavioural
  difference from vanilla is which of two exactly-tied candidates gets picked.
- **Float sums with an order:** modifier reduction sums stacking categories in ascending category id (§4.2).
  Damage multipliers apply in the fixed order of §3.6. Keep both orders. No FMA (project rule 3).

### 0.2 Synced RNG [H]
- **Generator:** the synced generator is the global state at 0x140a13e24. The unsynced Free_Random is at
  0x140a13e20 (0x540770) and is used by none of the paths below.
- **Step:** state ← state × 0x41C64E6D + 0xBDF, modulo 2³². The 15-bit output is (state >> 10) & 0x7FFF.
- **`Get_Int(lo, hi)`** (0x1ffb40), inclusive:
  - The bounds are swapped when they arrive reversed.
  - When lo = hi there is no draw.
  - Otherwise the output is masked to the smallest 2^k − 1 that covers the span and redrawn until the value
    fits. One call can therefore consume several steps.
  - The span is capped at 0x7FFE.
- **`Get_Float(lo, hi)`** (0x1ffbb0): one step. The result is lo + (hi − lo) × (out / 32767), with the bounds
  swapped first if reversed.
- **`Percent(p)`** (0x1ffdb0):
  - No draw when p is 0 or 100.
  - Otherwise it draws (state >> 10) & 0x7F and redraws while the value is above 100. It succeeds when the
    value is below p.

### 0.3 Game modes
The game-mode id (game-mode manager vfunc +0xe0) is 0 galactic, 1 land, 2 space [H]. Many constants come in a
land/space pair selected by this id.

---

## 1. Spatial tree — TreeCullClass (S6)

### 1.1 Structure [H]
- **Shape:** a binary AABB tree. Node and link layouts are in `STRUCTS.md`. Each node has a centre/half-extent
  box, a vector of links stored at the node, two optional children and a parent pointer (+0x40).
- **Links can sit at interior nodes:** large objects stay at the root, and moved links migrate up (§1.3).
- **Links:** each tree entry is a 0x48-byte heap link. It holds the object's AABB (centre + half-extents) cached
  at +0x2c, its node, and its index in the node vector.
- **Allocation:** nodes come in 3-node blocks (0xf0 bytes: a head plus its two children). A port can ignore this.
- **Registry:** every tree registers itself in a global registry at construction (0x140a7c680..688).

### 1.2 Rebuild — the only time the tree is shaped [H]
`Rebuild` (0x20f180):
1. Collapse every link into the root (0x20f600, which frees all child blocks).
2. Split the root's links in two by size. A link is **large** when its largest half-extent is above the tree's
   threshold (+0x28; the default is 500.0, and map-load rebuilds pass a map-provided value, §1.4). Large links
   are set aside.
3. Put the small links back in the root, keeping their order, and call `Split_Node(root, root, depth 0)`. It
   returns the maximum depth, stored at +0x1c.
4. Append the large links back into the root's vector, after the split, so they stay at the root.
5. Multiply the root's half-extents by 1.1, and inflate every descendant's half-extents by 1.1 recursively
   (0x210d30). This slack is what lets objects move a little before they migrate.
6. Reset the moved counter (+0x24) and the dirty flag (+0x20).

`Split_Node(node, axis_source, depth)` (0x20fc50):
- **Leaf rule:** no split when depth > 16 or the node holds fewer than 4 links.
- **Axis:** taken from the half-extents of `axis_source`. For the root that is the root itself. For a child it
  is **the parent**, because the recursive call passes the current node as the children's `axis_source`. This
  quirk is real [M], and harmless to drop in a port.
  - X is chosen only when its extent is strictly greater than both Y and Z.
  - Otherwise Y is chosen when Y is strictly greater than Z, else Z.
- **Partition** (`Partition_Links`, 0x210610):
  - The key is the link's AABB **centre** on the axis.
  - With fewer than 16 links, they are sorted by key (0x211290) and the first ⌊n/2⌋ go to child 0, the rest to
    child 1.
  - With 16 or more, the links go into 16 equal-width bins between the minimum and maximum key. The bin is
    ⌊(key − min) × 16 / (max − min)⌋, clamped to 0..15, and max = min + 1 when every key is equal. Links keep
    their input order within a bin. Walking the bins in order, the first ⌊n/2⌋ links go to child 0 and the rest
    to child 1. This splits by count at an approximate median, not at a bin boundary.
  - The node's own vector is emptied: every link moves to a child.
- **Child bounds:** each child's box starts as its first link's AABB and grows by union with each link it
  receives (0x20f960). The child is then split recursively.

### 1.3 Insert, remove, update — between rebuilds [H]
- **Insert (vf1, 0x20e600):**
  1. Allocate a link, copy the object's current AABB, and push the link onto the object's membership list.
  2. Find the target node with `Find_Smallest_Containing_Node` (0x210480). It recurses into child 0 first, then
     child 1, and returns the first node found depth-first whose box **fully contains** the AABB. It is
     therefore the deepest containing node with a child-0 preference, not strictly the smallest. If nothing
     contains the AABB, the root is used.
  3. Append the link to that node. If the node now holds more than 16 links, mark the tree dirty.
- **Remove (vf2, 0x20e6d0):** O(1) **swap-remove**. The node's last link moves into the removed slot, which
  changes node-vector order. Nodes are never merged.
- **Update_Bounds (vf4, 0x20f0a0):** called when an object moves or its bounds change.
  1. Store the new AABB in the link.
  2. If the current node still contains it, stop.
  3. Otherwise remove the link from the node, walk **up** the parents to the first ancestor that contains it,
     append it there, and increment the moved counter.
  4. Mark the tree dirty when link count < 5 × moved, that is, once more than 20% of links have migrated.
  - Links never move down between rebuilds, and node boxes never refit.
- **Owner change** (0x2b3ac0, `ObjectOwnerChanged`): the object is removed from the old owner's player tree and
  inserted into the new owner's tree.

### 1.4 Cadence [H]
- **Per-frame pass:** `Rebuild_All_Dirty(game_time_seconds)` (0x20f000) runs once per main-loop iteration
  (from 0x5d990, just before the game-mode update). For each registered tree, in registry order, it rebuilds a
  tree that is dirty **and** whose last rebuild (+0x2c) was more than 1.0 s of game time ago.
- **Map load and GOM reinit** (0x2af4f0):
  - With no map bounds available, every tree is rebuilt with the default threshold of 500.
  - Otherwise each tree gets `Rebuild_With_Threshold` using the map bounds inflated ×1.1, plus a map-supplied
    threshold (map vfuncs +0x120/+0x128, and +0x130/+0x138 for the space-only tree). Exactly how those two
    values are used was lost in decompilation [M].
- **No incremental rebalancing.**

### 1.5 Which trees exist and what is in them [H unless tagged]
`GameObjectManagerClass` owns several trees. Objects join them in `GOM::Register_Object` (0x2b5ad0):

| Tree | Contents | Used by |
|---|---|---|
| GOM+0xd0 + player_id × 0x38, one per player | the player's objects whose type is `Collidable_By_Projectile_Living` (type +0x41), or `_Dead` (+0x42) for objects in the dead/death-clone state (obj +0x3a0 bit 0x40). Objects that are not collidable are **not** in any player tree, so they cannot be targeted, hit by projectiles or hit by area damage. | targeting scans, opportunity fire, area damage, projectile hits, auras, AI threat queries |
| GOM+0xd8, all players | the same membership rule, every owner | radius query with player −1, placement search (0x2a9670), explosions |
| GOM+0xe0 | objects for which `0x39ac80` is true: any behaviour says so (behaviour vfunc +0xb0), or `Is_Impassable_Asteroid` (+0x55), or both `Collidable_By_Projectile_Living` and `Is_Valid_Target` (+0x6b) | [L] purpose not pinned (likely obstacle/selection) |
| GOM+0x558, space mode only | objects that are not `Is_Asteroid_Field` (+0x54), `Is_Ion_Storm` (+0x56) or `Is_Nebula` (+0x57), and that have a space layer (0x397900) other than bit 0x10 | fighter locomotor avoidance (0x5c8b70) |
| render scene trees (alScene/alSimpleScene) | render objects | frustum culling only |

- **Object id:** `Register_Object` also assigns the object id, a sequential counter at GOM+0x620 written to
  obj +0x50 (it skips 0x3ffffe and is replayed from the save on load). This id is the natural canonical sort
  key for §0.1.
- **Unregister** (0x2b68d0) and **destroy** (0x2a6a00) remove the object from all of its trees.

### 1.6 Queries [H]
- **`Query_AABB`** (0x20e780, with recursive collector 0x20fef0):
  - It walks the tree depth-first. At each node it first handles the node's **own links**: all of them when
    the node is fully inside the query box, otherwise those whose AABB is not disjoint from the box. It then
    recurses into child 0, then child 1. Disjoint nodes are pruned.
  - Collection order is therefore pre-order with own links first.
  - The collected vector is then pushed one by one onto the **head** of an intrusive list. The list the caller
    walks is in **reverse** collection order: the last child-1 leaf link comes first and the root's own links
    come last.
  - Results are stored on the shared links, so the query is **not reentrant**.
- **`Query_Frustum`** (0x20e9f0): render culling only (alSimpleScene, 0x36600). No gameplay caller.
- **`Query_Ray`** (0x20eac0): a segment slab test, used only by projectile sweeps through the GOM segment
  queries (§3.4) and by one scene picking path.
- **GOM radius query** (0x2a8fb0). Arguments: tree selector, player filter, centre, r, maximum count, sort flag.
  It is used by area damage, auras/abilities, cash points, squash, infection, gravity fields and projectile
  submodes.
  1. Pick the tree: the player tree for that player, or GOM+0xd8 when the player is −1.
  2. Run `Query_AABB` with a box of centre ± r in X and Y and **±1e18 in Z**, so the query is effectively 2D.
  3. Walk the results in list order. Skip garrisoned objects (obj +0x3a7). When a player filter is given,
     skip objects whose per-player byte at obj +0x332 + filter is −1 [M: meaning not pinned].
  4. **Accept** when the XY distance² from the centre is at most r².
  5. Otherwise run an exact test: the query sphere (centre, r) against the object's oriented box (local box,
     with the transform at obj +0x248; 0x20c000 returns 8 on intersection). Objects with a non-default
     +0x36b use a box built from their XY footprint instead [M].
  6. Append accepted objects to a **shared** GOM buffer (GOM+0x450), stopping at the maximum count. The cap is
     applied in traversal order.
  7. If the sort flag is set, `std::sort` by distance to the centre (comparator 0x2c0720 → 0x397640). The sort
     is **not stable**, so ties are implementation-defined.
  8. The buffer is returned. It is overwritten by the next call.
- **Does any gameplay result depend on traversal order?** Yes:
  - target acquisition ties (§2.4);
  - opportunity-fire "first exact 1.0";
  - area-damage victim order and the `Max_Victims` cap (§3.5);
  - the first projectile hit when two hits tie (§3.4);
  - any capped radius query.
- **Port rule:** sort query results by object id before the caller scans them (§0.1). A multithreaded port
  needs per-query result buffers (HOTSPOTS §3), and the rebuild can run off-thread on a snapshot as long as
  the snapshot swap happens at a fixed point in the frame.

---

## 2. Targeting (S8)

### 2.1 Behaviours and cadence [H]
- **Units:** `TargetingBehaviorClass` (vtable 0x8b0d88, 49 slots). Its service (slot 6, 0x633a30) dispatches to
  `Service_Land` (0x634810) in mode 1 or `Service_Space` (0x633ae0) in mode 2 (dispatcher 0x633a30 compares the mode with 1, then 2; labels corrected in refixf t06 round 2).
- **Squads and teams:** `TeamTargetingBehaviorClass` (0x8afd90; 0x62ee00 → 0x62eeb0) runs in space only and
  shares the helpers below.
- **The service runs every behaviour service call.** It writes the next service frame to +0x30 as the frame
  plus +0x34 (0x4c2f70) [M].
- **The acquisition scan is throttled per unit.** `Acquire_Target` (0x568a00) runs only when the current frame
  reaches the unit's next-scan frame (data pack +0x20). It then reschedules to now + FPS + `Get_Int(0, FPS/2)`,
  which is 30 + 0..15 frames, **one synced draw** per scan.
- **Skips:** the scan is skipped while data pack (obj +0x100) byte +0x2a4 is set.
- **When a unit already has a target**, it rescans only if that target is invalid, or is neither sticky (§2.5)
  nor scored exactly 1.0.

### 2.2 Range and query [H]
- **Scan range R** = `Get_Max_Attack_Range` (slot 40, 0x566ca0) = base × (1 + reduce of ModifierSet list 8, "fire
  range", §4).
- **Base** (`Get_Base_Attack_Range`, 0x3749d0) is the first of these that applies:
  1. An active unit-ability slot 0 or 1 whose ability data has a range override (data +0x108).
  2. For lobbed projectiles (Projectile_Category 3): Max_Speed² / (Grenade_Gravity × Lob_Mod).
  3. Otherwise `Targeting_Max_Attack_Distance` (type +0x500), multiplied by:
     - `Deployed_Max_Attack_Distance_Multiplier` (+0x23f8) when deployed, or
     - the constant at 0x140b278ac (`Garrisoned_Max_Attack_Distance_Multiplier` [M]) when garrisoned (obj
       +0x3a7).
  4. A transport or container contribution is then added (0x4160a0).
- **Players visited:** start at `Get_Int(0, nPlayers − 1)` (**one synced draw**) and walk the global player
  list cyclically, skipping:
  - the attacker's own player;
  - players marked inactive (player +0x68 → +0x108);
  - players whose relation in attacker player +0x370[id] is not "enemy" (1);
  - all players, when the attacker sits inside an `Is_Nebula` object and the fog test 0x35f3b0 applies [M].
- **Query:** for each visited player, `Scan_Player` (0x5662b0) runs `Query_AABB` on that player's tree with a
  cube of half-extent R centred on the attacker (R + the formation's chase / response range while the unit is in a
  formation move, 0x566f50, §2.8). The scan never does a sphere test itself. Per-candidate range acceptance goes
  through vfunc slots 44/45 (TACTICAL_GAPS §3.1) or the formation tests of §2.8.

### 2.3 Candidate filters, in order [H]
These apply in `Scan_Player` and again in `Validate_Current_Target` (0x567c50).
1. Skip if obj +0x3a1 bit 0x10.
2. The candidate must be targetable: a team member, or both `Collidable_By_Projectile_Living` and
   `Is_Valid_Target`.
3. Skip garrisoned (+0x3a7), dead (+0x3a0 & 0x40), +0x3b4, inside a transport (0x397060), and the +0x361/+0x38e
   test.
4. Skip if fully fogged for the attacker's player (0x35f470), unless obj +0x343 is set.
5. Short-range attackers only (max attack distance ≤ the constant at 0x140b27340):
   - skip when the space-layer masks `Space_Layer` (+0x16dc) / `Stationary_Space_Layer` (+0x16e0) are both
     non-zero and disjoint;
   - skip hovering targets in state 1.
6. Skip when the attacker's `Attack_Category_Restrictions` (+0x518) overlaps the target's `CategoryMask`
   (+0x1648).
7. The attacker (or its squad leader) must have a non-hardpoint weapon (+0x100 → +0x21c), or a weapon hardpoint
   for which `Does_Weapon_State_Allow_Firing_At` (0x384850) is true (§5.1).
8. The priority score (§2.4) must not be −1.
9. The candidate must be in range, or be the container's target.
10. **Stealth:** reject a stealthed target (0x39b140) unless the attacker type has `Target_Stealth_Units`
    (+0xa4) or both have the same owner.

### 2.4 Priority score and selection [H]
- **`Get_Priority_Score`** (slot 35, 0x566ba0) returns −1 when an attack-category restriction applies.
- **Which priority set:**
  - the Lua override at data pack +0x160, if present;
  - otherwise the type's `Targeting_Priority_Set` (type +0x520);
  - for hardpoints and team members, the parent's or leader's type.
  - With no set, the score is **1.0**.
- **Files:** `Targeting_Priority_Sets` are loaded from the files listed in `TargetingPrioritySetFiles.xml`
  (vanilla: Turret-, SpaceUnit- and GroundUnitTargetingPriorities.xml) by 0x483f10.
  - A `<Priority_Set Name=…>` holds `Attack_Priorities` (name-or-category, weight pairs), `Unit_Exclusions`,
    `Category_Exclusions`, `Property_Exclusions`, `Hard_Point_Priorities` and `Hard_Point_Exclusions`.
  - Set layout: +0x20 attack priorities, +0x30/+0x38 unit exclusions, +0x48 category exclusions, +0x50 property
    exclusions, +0x58 hard-point priorities, +0x70 hard-point exclusions.
  - **Hard-point names (refixf t06) [H].** The tag table at 0x140a409f0 gives `Hard_Point_Priorities` /
    `Hard_Point_Exclusions` field code 0x45 (comma-separated enum list; each token trimmed, converted, appended
    in file order) with the converter `EnumConversionClass<HardPointType>` at **0x140b310a0** (ctor 0x53f7b0).
    That converter holds **only the short names**: `Engine` 0, `Shield_Generator` 1, `Gravity_Well` 2,
    `Fighter_Bay` 3, `Tractor_Beam` 4, `Weapon_Laser` 5, `Weapon_Missile` 6, `Weapon_Torpedo` 7,
    `Weapon_Ion_Cannon` 8, `Weapon_Mass_Driver` 9, `Weapon_Special` 10, `Dummy_Art` 12 (value 11 has no name).
    Both the stored keys and the looked-up token are trimmed and upper-cased (0x200350, 0x200ad0), so the match
    is **case-insensitive** (`FIGHTER_BAY`, `WEAPON_LASER` resolve). A `HARD_POINT_*` token does **not**
    resolve: it logs "Failed to convert value" and is skipped (the list keeps only the converted tokens, in
    order, so ranks are 1-based positions among them). The long names `HARD_POINT_ENGINE` …
    `HARD_POINT_DUMMY_ART` (13, with `HARD_POINT_ENABLE_SPECIAL_ABILITY` = 11) are a separate string table at
    0x140a436d0, matched with `_stricmp` only by the hardpoint XML `Type` tag (0x53e3b0, 0x540000). Shipped
    FoC `SpaceUnitTargetingPriorities.xml` uses `Shield_Generator` and `Shield_Generator, Engine, FIGHTER_BAY,
    WEAPON_LASER, WEAPON_ION_CANNON, TRACTOR_BEAM`; no `Hard_Point_Exclusions`.
  - **Lua hard-point strings** use the same converter (lookup vf4 0x1dc260, also trimmed + upper-cased):
    `SpaceTaskForce.Attack_Target(obj, "Shield_Generator" [, repeat bool] [, threat number] [, avoidance
    string])` (0x6e54d0; vanilla SYSTEMATICDISABLE.LUA passes `"Shield_Generator", 500` and `"Engine", 500`,
    500 being the threat tolerance). The literal `"Random"` (exact case, 6 chars) instead draws a **synced**
    `Get_Int(0, 13)`, 13 → no hard point. Any other unconverted string is an error ("Unrecognized hard point
    type"), except in a 2-argument call, where it is parsed as the avoidance `SpaceCollisionType` instead.
    The hard-point type then selects the target's live hardpoint of that type (§2.8 step A.1).
- **`PrioritySet_Score`** (0x54dc00). **Lower is better.** Evaluated in this order:
  1. An `Attack_Priorities` entry that names the target's exact **type** returns its weight immediately.
  2. Otherwise the running value starts at **1e18**. Every category entry whose mask hits the target
     `CategoryMask` takes the **minimum** weight.
  3. A **property** entry hitting `Property_Flags` (+0x1650): the first hit **overwrites** the running value,
     later hits take the minimum. This makes it order-dependent: parse order of the XML list.
  4. `Unit_Exclusions` → −1.
  5. `Category_Exclusions` → −1, unless a property entry matched.
  6. `Property_Exclusions` → −1.
- **Selection inside one scan.** The best score is shared across all players visited. The first valid
  candidate becomes the current best.
  - **Early exit:** if a candidate is the current target and scores exactly 1.0, the scan returns immediately.
  - **Replacement rule:** a later candidate replaces the best when the first of these applies:
    1. exactly one of the two is at or below the health-fraction threshold (constant 0x140b27338; the fraction
       is min(hull fraction, destroyable-hardpoint fraction), 0x396470): that one wins;
    2. otherwise its score is strictly lower;
    3. otherwise, if the attacker has hardpoints (+0x33c) and the dry-run aim test 0x54a970 differs between
       the two: the aimable one wins;
    4. otherwise its **XY-only** squared distance is strictly smaller.
  - **Missing equality guard — verified in disassembly [H]** (llvm-objdump on the retail FoC exe, 0x5668f5):
    step 2 is a single `comiss` + `ja` that jumps only when the new score is **strictly lower**. Equal **and
    worse** scores both fall through to steps 3 and 4. A worse-priority candidate therefore **replaces** the
    best when it is aimable while the best is not (step 3), or when it is closer in XY (step 4). In practice,
    the score decides only when a candidate improves on it. Implement it exactly this way to keep vanilla
    behaviour. `Is_Better_Target` (0x567110) was not re-checked in disassembly [M].
  - **Ties:** exact ties keep the earlier candidate, so the result is traversal-order dependent (§1.6).
  - `Is_Better_Target` (0x567110) uses the same rule for retarget decisions.
- **Synced draws per scan:**
  - 2: the reschedule draw and the start player;
  - plus 1 `Get_Int(0, nHP − 1)` in `HP_Choose_Aim_Point` (0x383f70) for each candidate that has hardpoints.

### 2.5 Switching and stickiness [M]
- **Two target slots:** an **ordered** target (player or script, +0x148) and an **auto** target (+8). An order
  drops the auto target (0x565e50), and finished orders are cleared (0x565de0).
  - **Correction (refixf t06) [H]:** in the targeting data pack (obj +0xf0) +0x148 / +0x150 / +0x154 are the
    **pending targeted ability** (target, ability name CRC, ability type; written by slot 38 0x5688f0, ABILITIES
    §2.9), not a general ordered-target slot. A player / script / formation order lives in the normal current
    target **+8** with the **lock byte +0xf4** set (slot 33 0x566b40 reads it, slot 32 0x568c30 writes it).
    While the lock is set the service never rescans (§2.8 B). 0x565e50 clears +8 (slot 27) only when +8 differs
    from the pending-ability target and is neither hostile to the owner nor a MARKER object; 0x565de0 fires a
    pending ability. Neither one handles attack orders.
- **Sticky** (0x569000): a target is sticky when its estimated time-to-kill is at most the threshold.
  - The threshold is the Lua override (+0x158) or the type's `Targeting_Stickiness_Time_Threshold` (+0x528).
  - Time-to-kill = (current health, or the sum of destroyable-hardpoint health, plus 0x396420) / the damage
    rate from `DamageTrackingBehavior` (0x58ba40, +0xc).
- **Auto targets** are issued through 0x38d730. `Set_Target` (0x569160) and `Replace_Target_Ref` (0x569310)
  manage the references. 0x38d730 is the general attack order (`Order_Attack`, §2.8 C): auto acquisition calls
  it with ordered = 0, orders with ordered = 1.

### 2.6 Capital ships: per-hardpoint targeting [H]
- **Update order:** the object update 0x3a76b0 updates every hardpoint each frame **in `HardPoints` list
  order**, first through `HP_Update` (0x387010) and then `HP_Opportunity_Update` / `Update_Weapon` (0x387400).
- **Owner's target:** the owner pushes its target, and a chosen sub-hardpoint, to each hardpoint
  (`HP_Set_Target`, 0x387b70).
- **Sub-hardpoint choice** (`Choose_Target_Hardpoint`, 0x394610):
  - With a priority set, each live destroyable hardpoint of the target gets a rank: its 1-based position in
    `Hard_Point_Priorities` (0x54dd80), −1 if listed in `Hard_Point_Exclusions`, otherwise 1e18. The lowest
    rank wins, and equal ranks go to the closest in 3D (using `Ranged_Target_Z_Adjust`).
  - Without a set, it picks the closest `Is_Targetable` live hardpoint.
- **Opportunity fire** (`HP_Scan_Player`, 0x385190), used when the hardpoint has no shot at the owner's
  target. It is allowed when:
  - the hardpoint is idle and `Allow_Opportunity_Fire_When_Idle` (+0x3d5) is set (0x385620); or
  - `Allow_Opportunity_Fire_When_Targeting` (+0x3d6) is set; or
  - the owner is an AI player and `Fire_Max_Recharge_Seconds` ≤ the constant at 0x140b27048 (3.0;
    `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` [M]).

  How the scan works:
  - The current opportunity target (+0x50) is kept while it can still be fired at.
  - Otherwise the hardpoint rescans at most every FPS × 0.5 = 15 frames. The rescan uses the same
    random-start player loop (**one synced draw**) and a box of `Get_Fire_Range` × 1.1.
  - Candidates are filtered by `Fire_Category_Restrictions` (mask +0x408, 0x540140) and, for turrets, the fire
    cone.
  - Scoring uses the owner's slot 35, and `HP_Choose_Aim_Point` must succeed. The scan stops at the first
    candidate scoring exactly 1.0 and otherwise keeps the best [M].
  - Acquiring an opportunity target raises signal 0x21.
- **Fire cone** (0x383ba0): the aim point is transformed into the fire bone's local frame. It needs
  |yaw| ≤ `Fire_Cone_Width`/2 and |pitch| ≤ `Fire_Cone_Height`/2. `Is_Turret` hardpoints use
  `Turret_Rotate_Extent_Degrees` instead.

### 2.7 Line of fire [M]
There is **no line-of-fire or terrain line-of-sight test before firing**, in space or on land; no candidate
function or string was found. Obstruction happens only in flight: the projectile's swept segment hits the first
enemy-tree object (§3.4), the ground test on land, or a passive missile shield deflects it (§3.3).

### 2.8 Attack_Move on an object: ordered target and staging (refixf t06) [H unless tagged]
Question: when `SpaceTaskForce.Attack_Move(obj)` (0x6e4fc0) or `unit.Attack_Move(obj)` (0x5730e0) gets an
**enemy object**, do the units take it as their target? **Yes, but never through `Scan_Player`.** The object
reaches the units by two paths, and neither applies the priority score (§2.4), `Category_Exclusions`,
`Unit_Exclusions`, `Property_Exclusions` or `Attack_Category_Restrictions`:
- **A, at order time** (task-force orders only): the movement block status issues `Order_Attack(unit, obj,
  ordered = 1)` (0x38d730) to every member. In space this aims the members' **weapon hardpoints** at the object
  at once; squadron containers get it as a locked target.
- **B, every targeting service** (all object-goal attack orders, land and space): the formation's goal object
  becomes the unit's **locked current target** (+8 with lock +0xf4) as soon as it is inside the unit's weapon
  range. The lock stops all rescans until the object dies or becomes untargetable.

So a Frigate whose `Attack_Move` set excludes Capital, ordered `Attack_Move(station)`, still locks the station
once it is in weapon range; until then its hardpoints shoot targets of opportunity chosen by the scan (which
does apply the set). This is how vanilla AI kills stations; a port that only feeds the object to the scan
cannot.

**Data used below.**
- Targeting data pack TD = obj +0xf0: +8 current target, +0x10 its target hardpoint, +0x18 state (slot 31),
  +0x20 next scan frame, +0xf4 **lock** (slot 33 0x566b40 reads; slot 32 0x568c30 writes, and only when +8 ≠ null
  or the new value is 0), +0xf5 order flag of the last `Set_Target` (Service_Space re-applies it on a hardpoint re-target, §2.9)
  [M meaning], +0x148.. pending ability (§2.5 correction).
- Movement parameter block, 0x68 bytes (MOVEMENT §6.2): +0x00 position, +0x24 kind (1 position, 3 object),
  +0x28 object, +0x30 target hardpoint, +0x48 flags (0x01 attack-move, 0x02 guard; P1_DOUBLE_CLICK §3), +0x4c
  avoidance mask, +0x50 int [L], +0x54 threat tolerance (1e18 = none), +0x5c..+0x63 bytes.
- A unit's formation = locomotor pack (obj +0xa8) +0x1f0; its queued blocks are at formation +0xf8..+0x100.
- Type flags: +0x41 `Collidable_By_Projectile_Living`, +0x6b `Is_Valid_Target`, +0x88 / +0x89
  `Last_` / `Initial_State_Visible_Under_FOW`, +0xa4 `Target_Stealth_Units`, +0xaf `Fire_Weapon_When_Undeployed`,
  +0xd1 `Deploys`, +0xe9 `Disregard_Space_Layer_For_Guard_Attack_Move`, +0x504 `Targeting_Min_Attack_Distance`,
  +0x52c / +0x530 / +0x538 `Guard_Chase_Range` / `Idle_Chase_Range` / `Attack_Move_Response_Range`.

**Formation attack target** `Formation_Attack_Target(F, &hp, i)` (0x41a2e0):
1. F needs at least one member; P = the first member's owner.
2. Walk the queued blocks from index min(i, last) down to 0 (i = −1 means last). The first block that has an
   object, has guard bit 0x02 **clear**, and whose object is **hostile to P** (relation table == 1, 0x39a450;
   planets / capture points use their controller) is the answer. hp = that block's hardpoint index when the
   hardpoint exists and is not destroyed (destroyable +0x4d and health ≤ 0), else −1.
3. Otherwise walk all blocks from last to 0 with the same tests plus block +0x50 == 1 [M].
4. Otherwise null.
- The targeting service calls it with i = 0 (the current goal); `Scan_Player` / `Validate_Current_Target` with
  i = −1.
- Allied, own and relation-neutral objects never qualify. An `Is_Neutral` player whose relation is enemy **does**
  qualify: the "inactive player" skip of §2.2 belongs to the scan, not to this path. This is the vanilla source
  of the port's a1f t07 neutral-target rule.

**B. Promotion in the targeting service.** At the top of `Service_Land` (0x634810) and, identically, of
`Service_Space` (0x633ae0), after the pending-ability step (0x565de0):
1. Skip unless the lock is clear (slot 33 false) and the unit has a formation.
2. t, hp = `Formation_Attack_Target(F, &hp, 0)`; skip if null.
3. t must pass 0x5680b0: a TEAM container, or type +0x41 and +0x6b set; and not garrisoned (+0x3a7), not dead
   (+0x3a0 & 0x40), not inside a transport (0x397060), not +0x3b4, and the +0x361 / +0x38e test (§2.3 steps 2–3).
   **No fog, stealth, `Attack_Category_Restrictions`, weapon-state or priority test here.**
4. Range: slot 44 "in max range" true **and** slot 45 "inside min range" false (TACTICAL_GAPS §3.1: XY distance
   to the target (or to its hardpoint) ≤ `Get_Max_Attack_Range` + the target's footprint half-extent along the
   line of sight; min range = `Targeting_Min_Attack_Distance`, ignored when 0). `Attack_Move_Response_Range` is
   **not** used here.
5. `Set_Target(t, hp, order 1, lock 1)` (0x569160): a target that is a team member is replaced by its
   container; refused (no change) when t is garrisoned, dead, not `Is_Valid_Target`, or +0x3b4. Otherwise
   unsubscribe the old +8, store t / hp, subscribe to t's signals 0x28 and 1, +0xf5 := 1, slot 32(1) → lock
   +0xf4 := 1, slot 31(1). This **replaces** any auto target the unit had.
6. Then every hardpoint of type 5..10 (weapons), in list order: `HP_Set_Target(hp_i, t, hp)` (0x387b70).
- **While locked** the rest of the service skips the whole validate / rescan block (§2.1): its gate is "lock clear
  **and** (deployed (0x39a390) **or** the type does not `Deploys` **or** `Fire_Weapon_When_Undeployed`)". The
  target is only dropped (slot 27, which also clears the lock through slot 32(0)) when it is garrisoned, +0x3b4,
  dying or dead (+0x3a0 & 0x42), stealthed (`Is_Stealthed` 0x39b140) for an attacker without
  `Target_Stealth_Units`, or hidden by fog for the attacker's player (0x35f470, force 1). When the target dies
  the subscription clears +8 as well. A locked target that leaves weapon range is kept (state 2) [M].
- After the target is dropped, the normal scan resumes at the next due scan frame; the dead object's formation
  goal has become a position (MOVEMENT §6.3), so step 2 finds nothing.

**A. Order-time issue (task-force orders).** `SpaceTaskForce.Attack_Move(dest [, threat] [, avoidance])`
(0x6e4fc0): dest = object, task force or position; threat default 1e18 (none); avoidance string → complement of
the `SpaceCollisionType` bits, default 0x3f. It creates a `SpaceMovementBlockStatus` (0x738870, Lua id 0x817,
0x80 bytes) and inits it: object 0x7390e0, task force 0x738f80 (its current object, else its position), position
0x738ee0. The object init stores (issueAttack +0x41 = 1, repeat +0x42 = 1, hardpoint type = −1, threat +0x60,
flags +0x64 = **0x01 attack-move**, avoidance +0x68), subscribes to the object's signals 0x1a and 0x10, then
0x738b90(obj, collect = 1, hpType):
1. Only when issueAttack and hpType < 13 (Attack_Target with a hard point): the target hardpoint (+0x58) = the
   object's hardpoint of type hpType with +0x4c set and the **lowest health fraction** (0x385a70; first minimum in
   `HardPoints` order). None → release the object and finish the block (nobody moves).
2. Collect members (0x739200): each TF member whose type byte +0x1e is clear [L meaning], that is not
   order-locked (0x39ad40) and is admitted to coordinators (0x425e30); subscribe the block to its signals 9, 10,
   0xb, 0x10; list +0x78.
3. For each member in list order: if issueAttack and the member has TEAM (+0x348) or TARGETING (+0x33b):
   `Order_Attack(member, obj, hpKey (the chosen hardpoint's type +0x20, else 0), ordered 1, ability 0, −1, force
   0)`. A refusal (return ≠ 1) **removes the member from the order: it does not move**. Accepted members leave
   other coordinators and join one new coordinator (moving list +0x70).
4. Coordinator `Move_To` (0x4f8200) with {kind 3, object, chosen hardpoint or null, flags 0x01, avoidance,
   threat (1e18 → none)}.
- `LandTaskForce.Attack_Move` (0x6e9440 → 0x73c4f0 → 0x73c140) does the same (attack flag +0x56, Order_Attack
  per member, refusals dropped, kind 3).
- `unit.Attack_Move(dest)` (0x5730e0) is `Move_Internal` (0x57da90) with flags 1: an object that is not the unit
  itself or its container gives a kind 3 block with flags 0x01. **It does not call `Order_Attack`**: only path B
  applies. `unit.Move_To(obj)` (flags 0) moves to a **snapshot of the object's position** (kind 1): no target.

**C. `Order_Attack` (0x38d730, listed in §7 as `Issue_Auto_Target`).** Arguments (unit, target, hpKey,
ordered, abilityCRC, abilityType, force).
1. Refusals (return 0): the target's owner is not an enemy of the unit's owner (unless force); the target is
   stealthed (inline `Is_Stealthed`) and the unit type lacks `Target_Stealth_Units` and the owners differ; a
   melee unit (`Melee_Cutoff_Range`, 0x372210) against a target whose locomotor pack +0x2a8 == 1 [M: airborne];
   target garrisoned or +0x3a0 & 0x42; with no ability argument, the autofire fallback (TACTICAL_GAPS §1.3)
   triggered an ability.
2. **Weapon path**, taken in space (mode 2) unless the unit's BARRAGE (5) is active, and on land when ordered = 1:
   clear the unit's team target if it is a container; slot 32(0) (lock **cleared**); clear the pending ability
   (ordered and no autofire pick) or store it (ability call); every weapon hardpoint (type 5..10) whose
   `Allow_Opportunity_Fire_When_Targeting` (+0x3d6) is false gets `HP_Set_Target(target, hp)`. Then a unit that is
   not a team container **returns 1 without touching +8**, unless its LUCKY_SHOT (34) is active [M on the id
   name]; containers, LUCKY_SHOT units and units inside a team go on to step 3.
3. **Set path** (also the direct path for land with ordered = 0 and for space BARRAGE units): the team (own
   container, or the parent container's TEAM) gets its target slots set (0x4061f0(0), 0x4061d0(ordered),
   0x406370(target, hp index)); then `Set_Target(target, hp, ordered, ordered)` (lock = ordered) and every weapon
   hardpoint `HP_Set_Target`.
- Consequence in space: a ship given `TF.Attack_Move(station)` points its weapon hardpoints at the station at
  once, but its unit target and lock come only from B. Until B fires, the next due scan (§2.1) issues
  `Order_Attack(unit, scanPick, ordered 0)`, which re-aims those hardpoints at the scan's pick: **targets of
  opportunity on the way are the scan's choices** (priority set applied). Fighter / bomber squadron containers
  get a locked team target immediately.

**Targets of opportunity and `Attack_Move_Response_Range`** (the scan, §2.2–2.4):
- Scan cube half-extent (0x566f50): R = `Get_Max_Attack_Range`; when deployed, R; otherwise, for a unit in a
  formation (a space squadron member uses its container's), R + Rr when 0x4160a0(F, unit, none) ≠ 0; otherwise R
  in space, and on land R only for units with a TURRET slot (+0x33d), else 0 [M for land].
- Rr and the answer of 0x4160a0(F, unit, candidate) [M for the geometry of the non-attack-move branch]:
  1. 0 when: the unit is not in F; F has no blocks; candidate = the object of the first or last block (the goal
     is handled by B / by step 9 below); deployed with type +0x23b0 = 0; unit order-locked (0x39ad40); in space,
     the unit has a space layer (0x397900(unit, 1) ≠ 0) and its type lacks
     `Disregard_Space_Layer_For_Guard_Attack_Move`; last block flag 0x40.
  2. Rr from the **first** block's flags: 0x02 → `Guard_Chase_Range`, else 0x01 → `Attack_Move_Response_Range`,
     else `Idle_Chase_Range`; Rr < 0 → 0.
  3. Guard of an object: a candidate attacking the guarded object adds type +0x211c to Rr.
  4. F state +0x1d0 ≠ 3 and (first block 0x01 or last block +0x5e): 2 when the unit is within Rr (XY) of F's
     current path point (0x5c4ee0 on F +0x230, else the last path node), else 0.
  5. Otherwise (guard / idle): distance from the slot goal, then whether the candidate is reachable within Rr
     along the line (0x5672b0).
- §2.3 step 9 "in range" is: candidate == `Formation_Attack_Target(F, −1)` (or TD+8 when the unit has no
  formation), **or** slot 44 && !slot 45, **or** 0x4160a0 ≠ 0. The goal object therefore bypasses the range test
  in the scan but **not** the score: a set that scores it −1 (`Category_Exclusions`) or 1e18 loses it there.
  Path B is what makes it the target.

**Staging and movement** [M unless tagged].
- The formation goal is the object. In the path set-up 0x417520, a formation of type +0x218 = 2 heads for the
  object's position; in space the end point is snapped to a free spot by 0x59caa0.
- Staging 0x40e260 runs always for type 1 formations, and for type 2 formations **only when more than one block
  is queued** [H for the condition]; a single TF.Attack_Move in space is therefore not staged [M: that space
  formations are type 2]. If the last block is an object and not guard: take the member M with the
  largest `Targeting_Min_Attack_Distance` (or a larger pending-ability min range); if M is **inside** that min
  range of the target (slot 45), the goal becomes a point at d = min + 0.2 × (`Get_Base_Attack_Range`(M) − min)
  from the target (type 1: path request with stand-off d; otherwise 0x59caa0 ring). Type 1 special case: a one-member
  formation whose member has the +0x358 slot, and target `Is_Squashable`, or member `Is_Supercrusher` and target
  `Is_Squashable_By_Supercrusher`: goal = the target's position (crush). Staging only backs off min-range units;
  it does not stop a formation at weapon range. Stopping is not traced here [L].

**Completion of the task-force order** (`SpaceMovementBlockStatus`, what Lua `BlockOnCommand` waits on):
- `IsFinished` (vf19 0x7394f0): if the chosen target hardpoint is destroyed: repeat (+0x42) → pick another of the
  same type (0x738b90, collect = 0), else finish. Finished when +0x40 is set or the task force is empty
  (0x5fca10).
- Signal handler (0x739890):
  - object signal 0x10 (destroyed) or 0xb: finish, release the object, TF signal 0xb (0x5f6c60);
  - object signal 0x1a [M: owner change]: finish when issueAttack is set and the object is no longer hostile
    (or, with flags 0x02, when it became hostile);
  - unit signal 10 (movement finished): remove the unit from the moving list, TF Lua event 0x12 for it; when the
    moving list is empty: finish if there is no object; otherwise (flags 0x02 clear) finish only when the object
    is no longer hostile, fails 0x5680b0, or (type +0x88 and +0x89 both 0) is fogged for the TF's player.
    **Otherwise the block stays open after arrival**;
  - unit signals 0xb / 0x10 / 0x1a: remove the unit from both lists; same tests when the moving list empties;
    finish when the TF list empties.
- So `BlockOnCommand(TF.Attack_Move(station))` returns when the station dies (or turns invalid), not when the
  ships arrive. BurnUnits' `while TestValid(target) do Attack_Move(target)` therefore re-issues only after a
  failure.

**Summary by order** [H unless tagged]

| Order | Block (kind, flags) | `Order_Attack` at order time | Path B | Response range used by the scan |
|---|---|---|---|---|
| TF.Attack_Move(enemy object), space or land | 3, 0x01 | yes, ordered 1; refusing members do not move | yes | `Attack_Move_Response_Range` |
| TF.Attack_Move(position) | 1, 0x01 | no | no (no object) | `Attack_Move_Response_Range` |
| TF.Attack_Move(non-hostile object) | 3, 0x01 | refused for every armed member → only unarmed members move [M on the block's end] | no | — |
| unit.Attack_Move(object) | 3, 0x01 | no | yes | `Attack_Move_Response_Range` |
| unit.Move_To(object) | 1 (position snapshot) | no | no | `Idle_Chase_Range` |
| TF.Attack_Target(obj [, hpType] [, repeat] [, threat] [, avoid]) (0x6e54d0) | 3, **0x00**, hardpoint +0x30 = chosen | yes, with the hardpoint key | yes, with hp: the lock carries the hard point | `Idle_Chase_Range` |
| unit.Attack_Target(obj) (0x5730f0) | 3, 0x00 | yes, ordered 1, hp 0, for each listed unit sorted by object id | yes | `Idle_Chase_Range` |

- A hard-point target (Attack_Target with a type) differs only by the chosen hardpoint: it goes into the block
  (+0x30), so `Formation_Attack_Target` returns its index while it lives and the lock holds that hardpoint (TD
  +0x10). Without one, path A/B hand the weapon hardpoints **no** sub-hardpoint; `Service_Space` then picks one
  by the set's `Hard_Point_Priorities` in the same service (§2.9 Q3), and until then each shot takes the target's
  live targetable hardpoint closest to the shooter.
- Land vs space: path B is identical. Path A differs only in C step 2's condition (space: always the weapon path;
  land: weapon path for ordered = 1, set path for auto targets).

**Port notes (refixf t06).** (1) Add path B: per targeting service, before the scan, if the unit holds no locked
target and its formation's current goal is a hostile object that passes 0x5680b0 and is inside weapon range (max
range + footprint half-extent, outside min range), make it the locked target with no score / exclusion /
category test, and keep it until it is invalid (list above). (2) TF.Attack_Move(object) must not complete on
arrival; it completes when the object dies or turns invalid. (3) Units for which `Order_Attack` refuses (non-enemy
target, stealth) are dropped from the move.

### 2.9 Damage to hardpointed targets and close-range fire (refixf t06) [H unless tagged]
Context: capitals locked on a station (§2.8) whose shield falls to 0 while almost no damage reaches the hull or
the hardpoints. Mode ids: 1 land, 2 space (§0.3; confirmed by Take_Damage choosing
`Space_Retreat_Units_Damaged_Mod_Factor` (GC +0x20b8) for mode 2).

**Q1. Where a hit on a hardpointed object goes.**
1. **Hit test** (`Sweep` 0x4c97a0 → 0x2abcf0): the precise model test (0x38f900, SPACE_AVOIDANCE §5) leaves the
   **render sub-object it hit** in the query (+0x08); 0x2abcf0 stores that sub-object's name (its vf +0x30) in
   the hit record +0x28. `Process_Hit` (0x4cb280) passes this **mesh name** to the direct-damage step 0x4c73d0 as
   the hardpoint name (the `Collision_Box_Modifier` sphere fallback passes the name only if a render object was
   recorded).
2. **Random hardpoints** (0x4c73d0): with `Projectile_Damages_Random_Hard_Points` (type +0x49) the name is
   replaced by the `Collision_Mesh` of the hardpoint picked by 0x395190 (one synced `Get_Int`, §3.5).
3. **Name override in `Take_Damage`** (0x3a9e30, only when its "explicit name" argument (13th) is 0, as for direct
   hits; pack = source +0xe8):
   - pack target (+8) == victim **and** pack target-hardpoint index (+0x10) ≥ 0 → name = **that hardpoint's
     `Collision_Mesh`**, whatever mesh was physically hit and even if a random pick was made in step 2;
   - otherwise, **land only** (mode 1): name = `Collision_Mesh` of the victim's hardpoint closest (3D) to the
     **source's position** (projectile +0x78) among those that are `Is_Targetable` and not (destroyable and dead)
     (0x394a80(victim, pos, 0, 0));
   - otherwise (space): the hit mesh name stays.
4. **Lookup** (0x394d40): the first hardpoint in `HardPoints` order whose type +0xa0 (**`Collision_Mesh`**) equals
   the name, `_stricmp`. A lucky shot (ability 0x22) whose name finds nothing switches to the closest hardpoint as
   in §5.1.
5. **Routing** (after shield absorption and armor, §3.6 steps 14–16):
   - found, `Is_Destroyable`, health > 0 → `HP_Take_Damage` (0x387f50) takes **all** of it, the hull nothing;
   - found, `Is_Destroyable`, health ≤ 0 → **no damage at all** (neither hardpoint nor hull; the shield
     absorption already done stays);
   - found but not destroyable (e.g. `DUMMY_ART`), or no match, or no name → **hull** (0x3ab890);
   - killing the last live destroyable hardpoint of a type with +0xa8 set → hull health := 0 and the hull path runs
     (death).
6. Shields come **first** for every hit (§3.6 step 15): the station's shield absorbs while it has points; a
   hardpoint is damaged only by the part that gets through. Destroying the last `SHIELD_GENERATOR` zeroes the
   shield and blocks regeneration (§5.1); while one lives, regeneration is `Shield_Refresh_Rate` × the live
   generator fraction, with the 5 s / 0.25-point depleted lockout (§3.7).
- **`Should_Be_Destroyed_When_All_Hardpoints_Destroyed` (type +0xa8, bool tag at 0x140a32550) defaults to TRUE.**
  The type ctor 0x367250 calls the flag-block initialiser 0x367100 with this = type + 0x10; its `movl $0x10100,
  0x97(%rcx)` sets type +0xa8 = 1. Shipped FoC XML sets it only once (`No`, SPECIALSTRUCTURES_UNDERWORLD.XML), so
  **every star base has the coupling** of §5.1: each frame (0x3a6b80) hull fraction ≤ min(1, hardpoint fraction +
  `Hull_Vs_Hard_Points_Health_Constraint` 0.2), and hardpoint fraction ≤ hull fraction + 0.2 (0x3a76b0).
- **So for a station:** hull damage is not blocked, but direct hits from hardpoint-targeting weapons almost never
  reach the hull (step 3 sends them to the targeted hardpoint). The station's health bar falls because the hull is
  capped at hardpoint fraction + 0.2 as hardpoints lose health, and it dies when its last destroyable hardpoint
  dies. Lua `Get_Hull` reports min(hull fraction, hardpoint fraction) (ABILITIES_HARDPOINTS §1.3).
- **No station-only rule** on this path: `Take_Damage` reads no `Base_Level` (type +0x8a0) [M: absent from the
  decompiled body]; the only gates are the invulnerable flag (+0x37d), `Immune_To_Damage` (type +0xa3) and the
  global damage switch (§3.6 step 3). There is no `Damage_To_Hull` tag in the executable.
- Area damage on a hardpointed victim is split across its live destroyable hardpoints in range and never reaches
  the hull (§3.5).

**Q2. Firing at close range.** `Fire_At` (0x3825b0) has **no unit-level minimum range**:
`Targeting_Min_Attack_Distance` (+0x504) is read only by the scan / promotion (slot 45, §2.8 B step 4). Per shot:
1. **Target hardpoint:** the one pushed by `HP_Set_Target` (hp +0x48). `Update_Weapon` (0x387400) clears it first
   when it belongs to another object or is destroyable and dead. If none: the target's hardpoint closest (3D) to
   the shooter's position (0x398440) among those that are `Is_Targetable` and not (destroyable and dead)
   (0x394a80(target, pos, 0, 0)), **chosen per shot**.
2. **Turret pre-check** (`Is_Turret`): the X axis of the turret bone (hp +0x94, resolved from `Turret_Bone_Name`
   by `HP_Update` 0x387010) · the unit direction from that bone to the **target object's position** ≥
   cos(`Fire_Cone_Width`°) (full width, not half), else no shot [M: axis choice].
3. **Aim point:** with a target hardpoint, its bone's world position (0x385c70: attachment bone hp +0x2c, else the
   bone at hp +0x38; **(0, 0, 0)** if neither resolves). Without one, 0x383f70: the first entry of a type bone
   list (+0xe68, count +0xe70) [L: tag], then the hardpoints from a **synced random start** (`Get_Int(0, n − 1)`,
   wrapping, skipping destroyable dead ones), then the object's position, each accepted only when its 3D distance
   from the fire-bone reference ≤ `Get_Fire_Range` (+ target radius when the shooter type has weapon list +0xc48)
   and it passes the cone; none → no shot. **Never the nearest surface point.**
4. **Range, XY:** d = XY distance from the fire-bone reference (bone A, or the A–B midpoint) to the aim point;
   need `Fire_Min_Range_Distance` ≤ d ≤ `Get_Fire_Range` + target radius (0x397780). Hardpoint type defaults
   (ctor 0x53e3b0): `Fire_Range_Distance` 0, `Fire_Min_Range_Distance` 0, `Fire_Cone_Width` / `Height` **0**,
   `Turret_Rotate_Extent_Degrees` 180. No FoC capital or star-base weapon hardpoint sets a min range, so in
   vanilla a ship at any distance passes this test as long as the targeted hardpoint is within range.
5. **Lead** (0x399450) on the aim point, then the **cone test on the lead point** (0x383ba0):
   - Not a turret: the point is transformed into the **fire bone's full world frame** (the matrix 0x385e70 returns:
     bone orientation × the ship's yaw, pitch and roll); yaw (wrapped to ±180) must be ≤ `Fire_Cone_Width` / 2 and
     pitch ≤ `Fire_Cone_Height` / 2.
   - Turret: the point in the attachment-bone frame; |yaw + 90°| (normalised) ≤ `Turret_Rotate_Extent_Degrees`;
     no elevation limit here.
   - Fail → `Fire_At` returns 0 (the cooldown stays expired; opportunity fire may shoot something else, §2.6).
6. Then muzzle, inaccuracy, spawn as in §3.1.

So a very close ship **does** fire while each targeted hardpoint bone stays inside the firing bone's cone. Vanilla
side batteries are narrow vertically (e.g. `Calamari_Cruiser` 160–170° × 130°, `Alliance_Assault_Frigate` 160–170°
× 90°), so a target hardpoint far above or below the firing bone (pitch over 65° / 45°), or on the far side of
the ship, blocks that battery. Ships do not sit inside a station in vanilla: stations carry `SPACE_OBSTACLE` and
the movement / avoidance code keeps hulls apart (SPACE_AVOIDANCE) [M: not re-traced here].

**Q3. Which hardpoint a capital aims at, and after it dies.**
- **Choice** `Choose_Target_Hardpoint(target, shooter)` (0x394610):
  - set = the shooter's Lua override (TD +0x160, e.g. `SetClassPriorities(tf, "Attack_Move")` in the space AI
    plans → `Capital_Attack_Move`), else the type's `Targeting_Priority_Set` (0x566ed0). Shooters without
    targeting, or without a set, use the no-set branch.
  - **With a set:** candidates = destroyable and alive (`Is_Targetable` **not** required); rank (0x54dd80) = 1-based
    position of the hardpoint's **type** in `Hard_Point_Priorities`, −1 (skipped) if in `Hard_Point_Exclusions`,
    else 1e18; lowest rank wins; equal rank → smaller 3D distance from (shooter x, y, z +
    `Ranged_Target_Z_Adjust` (type +0x4dc)) to the hardpoint position 0x396ce0; first in list order on exact ties.
    `Capital_Attack_Move` = Shield_Generator, Engine, Fighter_Bay, Weapon_Laser, Weapon_Ion_Cannon, Tractor_Beam;
    missiles, torpedoes etc. rank 1e18 (closest first) after them.
  - **Without a set:** the closest `Is_Targetable` hardpoint that is not (destroyable and dead), position 0x385c70.
- **When it is chosen:** only when a target is set: the scan (0x568a00 calls it for its pick) → `Set_Target`; and
  `Service_Space` (0x633ae0) after the keep / switch step, **only when TD +0x10 is null or a dead destroyable
  hardpoint and the unit has a formation** (locomotor pack +0x1f0): hp = the formation goal's hardpoint index if
  that hardpoint is not dead, else `Choose_Target_Hardpoint(target, unit)`; then `Set_Target(target, hp, +0xf5,
  lock)` and `HP_Set_Target(·, target, hp)` on every weapon hardpoint. A rescan that finds the **same** target does
  not re-set it, and `Service_Land` (0x634810) has no such step.
  - `Order_Attack` (path A) with hpKey 0 and the path-B promotion with no goal hardpoint push **no**
    sub-hardpoint, so the formation case above fills it in the same service.
- **After the hardpoint dies:**
  - each weapon hardpoint's `Update_Weapon` drops its +0x48 at once, and `Fire_At` then shoots, **per shot**, the
    target's live targetable hardpoint **closest to the shooter**, not the next priority;
  - for a unit in a formation, `Service_Space` re-chooses **by priority** on its next run (the next service, not
    every frame) and pushes it to all weapon hardpoints;
  - projectiles already in flight toward the dead hardpoint keep its index, so their hits do **no damage** (Q1
    step 5).
- When the target object dies the subscriptions (signals 1 / 0x28) clear the targets (§2.8).

**Port differences (combat_system.cpp, refixf t06 round 2).**
1. `process_hit`: hardpoint = `p.target_hp` only when victim == target, else −1 → hull. Vanilla space keeps the
   **physically hit mesh** (a hit on a hardpoint's `Collision_Mesh` damages that hardpoint); vanilla land uses the
   closest targetable live hardpoint to the projectile. The port has no meshes: route a non-target hit in space to
   the hardpoint whose collision mesh contains the impact [port decision], else hull.
2. `process_hit` random hardpoints: vanilla's targeted hardpoint (victim == target, index ≥ 0) **overrides** the
   random pick (the synced draw is still made); the port lets the random pick win.
3. `take_damage` step 17: a named **dead** destroyable hardpoint must swallow the damage (no hull damage); the port
   sends it to the hull. A named non-destroyable hardpoint → hull (port agrees).
4. `fire_hardpoint` fallback (`thp < 0`): vanilla requires `Is_Targetable` (0x394a80 with 0, 0); the port calls
   `closest_hardpoint(t, u.pos, false)`, which can pick a non-targetable `DUMMY_ART` hardpoint (stations have
   `*_Comm_Array_Dummy` / `*_Supply_Dock_Dummy`): its hits then go to the **hull**. Use `true`.
5. `update_weapons`: re-chooses the sub-hardpoint by priority **every frame**. Vanilla pins it at `Set_Target` and
   re-chooses only per §Q3 (formation units, next `Service_Space`); otherwise closest-to-shooter per shot.
6. `choose_target_hardpoint`: tie distance from the shooter position + `Ranged_Target_Z_Adjust`, not `a.pos`; the
   no-set branch also accepts targetable **non-destroyable** hardpoints (they are never dead).
7. `in_cone`: vanilla uses the fire bone's full 3D frame (ship pitch / roll and bone orientation) and the
   **lead-adjusted** point; the port uses unit yaw + `fire_yaw_deg` [L] on the pre-lead point, and treats a cone of
   ≥ 360 × ≥ 180 as open. Unset `Fire_Cone_Width` / `Height` are **0** in vanilla (fires only dead ahead). Turrets
   also need the barrel-direction pre-check (Q2 step 2).
8. Aim point without a target hardpoint: vanilla 0x383f70 (bone list, random-start hardpoints with one synced
   draw, then centre; each must be in 3D range and cone) instead of `t.pos`.
9. The observed run (ships 30–900 units from the station centre) is outside vanilla geometry: from inside the
   station, many target bones fall outside the 130° / 90° vertical cones (Q2), so few shots fire; and every hit
   that resolves to a dummy hardpoint (item 4) lands on the hull.

---

## 3. Weapons, projectiles, damage (S8)

### 3.1 Hardpoint fire cycle [H unless tagged]
**Construction** (0x381a90):
- The first cooldown (+0x58) = **synced** `Get_Int(0, int(FPS × Fire_Max_Recharge_Seconds))`, drawn when the
  hardpoint instance is created. Creation order is the owner's `HardPoints` list order.
- Pulses remaining (+0x5c) = `Fire_Pulse_Count`.

**`Update_Weapon`** (0x387400), weapon types 5–10 only, once per frame per hardpoint:
1. Return early when weapons are globally disabled, the hardpoint is disabled (+0x6e) or destroyed, or an owner
   state gate applies.
2. Subtract the elapsed frames from the cooldown. Return while it is still above 0.
3. A manual-target hardpoint drops its manual target (+0xc0) 300 frames after the weapon state stops allowing
   fire [M].
4. Try `Fire_At` the primary target (+0x40) with its target hardpoint (+0x48).
5. If that did not fire, try opportunity fire (§2.6).

**`Fire_At`** (0x3825b0), in order:
1. **Gates:** target dead or stealthed; the special-weapon state counter (owner +0x100 → +0x394);
   `Fire_Category_Restrictions`; alliance.
2. **Target hardpoint:** if none was given, use the target's hardpoint closest to the shooter (0x394a80).
3. **Turrets:** refuse when dot(barrel direction, direction to target) < cos(`Fire_Cone_Width`°) [M].
4. **Projectile type,** first that applies: the hardpoint's `Fire_Projectile_Type` (+0x210);
   `Blast_Ability_Fire_Projectile_Type` (+0x220) in the special state; an ability override (0x397e00) [M].
5. **Aim point:** the targeted hardpoint's world position (0x385c70), otherwise the object's aim point
   (0x383f70) [M].
6. **Fire bones** (0x385e70): only `Fire_Bone_A` (+0x38) and `Fire_Bone_B` (+0x3c) are used. **C and D are never
   read on this path.** The reference point is bone A, or the A–B midpoint when B exists.
7. **Range, XY only:** `Fire_Min_Range_Distance` ≤ distance ≤ `Get_Fire_Range` + target radius (0x397780).
   `Get_Fire_Range` (0x3857d0) = `Fire_Range_Distance` (+0x238) × (1 + list 8).
8. **Muzzle:**
   - `Randomize_Between_Fire_Bones` (default **true**): **3 synced `Get_Float`** draws, one per axis between
     the two bones. The exact arguments were lost in decompilation [M].
   - Otherwise, with no bone B: bone A, no draw.
   - Otherwise `Percent(50)` (one draw) picks A or B.
9. **Lead** (0x399450) [M]:
   - The target's last three positions are fitted. A straight track gives linear lead (0x399e20).
   - A curving track uses its circumcircle and an iterative root solve (0x7d68e0: bounds 0.1–1000, tolerance
     0.001, 15 iterations). Circles with a radius above 1000 fall back to linear lead.
   - Projectile speed = `Max_Speed` (projectile type +0x37c, per frame) × FPS.
10. **Inaccuracy** (0x381dc0; a debug flag at 0x140b3934d disables it):
    - Fixed radius r = the shooter type's `Targeting_Fire_Inaccuracy_Fixed_Radius` (+0x1ff8), or the ability
      override (ability data +0x1b4). If r > 0: **3 synced `Get_Float(−r, r)`**, X then Y then Z.
    - Otherwise d = the hardpoint's `Fire_Inaccuracy_Distance` entry whose category mask matches the target.
      Then s = d × distance / `Get_Fire_Range`, and 3 synced `Get_Float(−s, s)` (X, Y, Z).
    - **Doubling verified in disassembly [H]** (getters 0x374890 for r and 0x53ff30 for d). Each loads 1.0
      from 0x1407ffaf8. In land mode only, it subtracts the AI-difficulty accuracy (0x468750, which returns 1.0
      when it doesn't apply), giving f. It returns v × f + v (`mulss` then `addss`).
      - **In space f = 1**, so the effective inaccuracy is **2 × the XML value**.
      - On land it is (2 − accuracy) × the XML value.
      - An ability override radius (ability data +0x1b4) is returned **unscaled**.
11. **Spawn** the projectile (0x29f810).
    - Fire sound: the hardpoint's `Fire_SFXEvent`, otherwise the owner type's `SFXEvent_Fire` (+0x1240). It is
      delayed when `Projectile_Appearance_Delay_Frames` > 0.
    - The projectile data pack (projectile +0xe8) is filled:
      - shooter id (+0x58);
      - damage (+0x64) = the hardpoint's `Projectile_Damage` (+0x478) if above 0, else the projectile type's
        (+0x474). In the blast-ability state it is multiplied by ability-0x46 data +0x17c;
      - damage type (+0x68) = the hardpoint's `Damage_Type` CRC, else projectile type +0x440;
      - maximum travel (+0x6c) = `Get_Fire_Range`, or `Projectile_Max_Flight_Distance` when that is ≤ 0, plus
        the target radius (and the parent radius), plus |aim.z − muzzle.z|;
      - target (+0x08) and target hardpoint index (+0x10);
      - homing data or a target-motion snapshot (§3.3).
12. **Recharge after the shot.** Two factors are used:
    - **W**, weapon delay: the product over the active unit-ability slots of the hardpoint type's per-mode delay
      table (+0x250 + ability_type × 4; the vanilla tags are `Defend_Mode_`, `Turbo_Mode_`,
      `Power_To_Weapons_Mode_` and `Full_Salvo_Weapon_Delay_Multiplier`), × the unit-ability `WEAPON_DELAY`
      multiplier (kind 1, §4.4).
    - **F**, fire rate: the unit-ability `FIRE_RATE` multiplier (kind 9) × (1 + ModifierSet list 6).

    **When pulses remain** after decrementing:
    1. frames = int(FPS × `Fire_Pulse_Delay_Seconds`).
    2. frames = int(frames × W + 0.5).
    3. If F ≤ 0, frames and pulses become 0. Otherwise frames = int(frames / F + 0.5).

    **When pulses reach 0:**
    1. frames = ⌊trunc(**synced** `Get_Int(trunc(min × 100), trunc(max × 100))` × FPS) / 100⌋. Here min and
       max are `Fire_Min/Max_Recharge_Seconds`. The bounds are **truncated**, not rounded (`cvttss2si`), and
       the last step is an unsigned integer divide by 100 [H, disassembly 0x1403831ee–0x140383231]. Stamp
       +0x68.
    2. Apply W only when W < 1.
    3. Apply F only when 0 < F < 1, as frames / F + 0.5.
    4. pulses = int(`Fire_Pulse_Count` × max(F, 0) + 0.5).

    **W gating verified in disassembly [H]** (0x1403832f8):
    - **When pulses reach 0**, W is applied **only when W < 1**. The check is `comiss` against 1.0 with `jbe`
      skip, so W ≥ 1 leaves the recharge unscaled: delay multipliers can only shorten a full recharge.
    - **On the pulse-delay branch**, W is applied unconditionally.

    Normal fire is suppressed while ION_CANNON_SHOT or LUCKY_SHOT is active.

**Non-hardpoint weapons** (`WeaponBehaviorClass`, 0x54cef0 reset; pulse delay 0x54d8f0; range 0x54d090 uses
list 8):
1. frames = int(`Projectile_Fire_Recharge_Seconds` × FPS) + **synced** `Get_Int(0, 10)`.
2. Apply W only when W < 1.
3. pulses = `Projectile_Fire_Pulse_Count`.
4. frames = trunc(frames / F) and pulses = trunc(F × pulses), with **no** +0.5 rounding [H].

The behaviour is attached by the type's `WEAPON` behaviour (0x0A) and is serviced like any behaviour, besides the
hard points [M: no code path that turns it off when weapon hard points exist was found]. So a type that lists both
fires its `Projectile_Types` too. FoC: `M1_Repulsor_Tank` is the only such type; its `HP_2M_Tank_00` hard point excludes
Air / Vehicle / Structure, so only the WeaponBehavior shot hurts vehicles (a1h t01; port: `battle_catalog.cpp`).

### 3.2 Damage and armor tables [H]
- **Where they are:** GAMECONSTANTS.XML holds `<Damage_Types>`, `<Armor_Types>` and
  `<Damage_To_Armor_Mod> damage, armor, factor </Damage_To_Armor_Mod>`. The GameConstants instance is at
  0x140b15800.
- **Ids:** each type name is turned into an id with CRC32 (0x215a30).
- **Matrix layout:** float[damage][armor] at +0x16e8, row stride 128. The damage CRC list has its count at
  +0x16a8 and array at +0x16b0; the armor list has its count at +0x16c8 and array at +0x16d0.
- **Lookup** (0x2c7d80): an unknown type or a negative cell gives **1.0**.
- **Object fields:** `Armor_Type` at type +0x438, `Shield_Armor_Type` at type +0x43c [M]. An active unit ability
  whose data has a non-zero armor id (+0x168) overrides the armor; slot 1 is checked before slot 0 (0x395850)
  [M].

### 3.3 Projectile flight (`ProjectileBehaviorClass`, vtable 0x878d58; vf6 0x4ce6a0) [M unless tagged]
- **Behaviour per `Projectile_Category`** (type +0x1fe8). The numbers are known, the names are not mapped:

  | Category | Behaviour | Function |
  |---|---|---|
  | 1 | homing | 0x4cf870 |
  | 2, 5, 9 | ballistic, `Bomb_Fall_Accel` (+0x1e88) | — |
  | 3 | grenade/lob gravity (+0x1e80/+0x1e84); closed-form parabola, LOB_SOLVER.md §6 | 0x4ceea0 |
  | 4, 6 | spline | 0x4cfc40 |
  | 8 | spline with an area pulse | 0x4cf460 |
  | anything else | straight | 0x4cec80 |

- **Straight step [H]:** position += speed (+0x90, initialised from `Max_Speed` per frame) along yaw +0x8c and
  pitch +0x88. `Projectile_Acceleration_Per_Frame` has no observed effect (the update adds 0.0) [L].
- **Passive missile shields** (0x4c6e30), for straight and homing shots:
  - Shield sources are tested in global list order.
  - The first non-allied shield that contains the projectile turns its heading outward.
  - The radius is `Passive_Missile_Shield_Radius` (+0x23e8), or ability 0x20/0x3f data +0x20.
- **Lost targets:** homing missiles that lose their target **never re-acquire one**, because the best distance
  starts at 0 in the scan. This affects `Projectile_Max_Scan_Range` (+0x4c4). The port should keep the
  behaviour.
- **Per step** (0x4d0080), in this order: sweep (§3.4) → hit processing (§3.5) → move and expire (0x4c7050).
- **Expiry [H]:**
  - Distance travelled (+0x5c) increases by speed each step. The projectile expires when travelled ≥ +0x6c.
  - When +0x6c is 0, or for category 3 with a lifetime, it expires once (frames since spawn) / FPS >
    `Projectile_Max_Lifetime`.
  - On expiry: lifetime detonation particle, area damage centred on the projectile, on-detonation effects,
    then destroy.

### 3.4 Hit detection [M]
`Sweep` (0x4c97a0):
1. Run a ground test (0x4d0630).
2. Run `GOM::Find_First_Object_Hit_By_Segment_All_Enemies` (0x2abcf0; HOTSPOTS §3 has the loop). Collision
   mask: 7 if `Projectile_Does_Shield_Damage`, else 5.
3. If nothing was hit and the projectile has either `Friendly_Damage_Amount` (+0x23ec) > 0 or
   `Causes_Infection` (+0xa7), also run the own-player variant (0x2acb90).
4. An object hit wins when its segment fraction ≤ the ground fraction.

The segment query visits **hostile players in player-list order** and returns the **first** object whose
precise test (0x38f900) or `Collision_Box_Modifier` sphere test passes. "First" means in `Query_Ray` result
order, not nearest along the segment. The port should sort by segment fraction, then by object id.

### 3.5 Hit processing, area damage [H unless tagged]
**`Process_Hit`** (0x4cb280; direct damage 0x4c73d0):
1. Damage = pack +0x64. If that is 0, use `Projectile_Blast_Area_Damage`.
2. `Projectile_Damages_Random_Hard_Points` (0x395190): **synced** `Get_Int(0, count − 1)`, then probe forward
   with wrap-around to the first live destroyable hardpoint.
3. **Delayed or immediate damage:**
   - If `Projectile_Damage_Delay_Secs` = d > 0: delay = **synced** `Get_Float(0.25 × d, d)`. The hit is queued
     on the victim (0x3ab650) for max(1, int(FPS × delay)) frames, and the queue drains in the victim's update
     (0x3a6b80).
   - Otherwise `Take_Damage` is called now, with the damage kind from pack +0x70.
4. **Visuals [M]:**
   - Not absorbed: the detonation particle (the armor-reduced variant when the hull armor factor ≤ 0.75), then
     a **synced `Get_Int`** pick from the victim type's hit-particle list (+0xc80).
   - Absorbed by shields: `Projectile_Absorbed_By_Shields_Particle`, then a **synced `Get_Int`** pick from the
     shield-hit list (+0xcc0).
5. Detonation sound, the armor-reduced one when the factor ≤ 0.75.
6. Area damage, if blast damage and blast range are both > 0 and the hit was not cancelled.
7. On-detonation effects, in this order: stun, distract, heal, convert, invulnerability, combat mod, ion stun
   (single target when its radius is 0), infection.

**Area damage** (0x4c7f70):
1. Players are visited in index order. R = `Projectile_Blast_Area_Range` × the shooter's `SCATTER_RADIUS`
   unit-ability multiplier (kind 10).
2. A player is processed if it is an enemy, or if `Immune_Faction` is set; in the second case members of the
   immune faction are skipped.
3. Victims come from the GOM radius query on that player's tree (§1.6), taken in query order. Each victim must
   be `Collidable_By_Projectile_Living`. The directly hit object is skipped unless it has hardpoints.
4. d = 2D distance from the blast. With `Dropoff` and R > 0, d is cut to the segment collision point, or set to
   0 when the detonation is inside the victim.
5. factor = clamp(1 − ⌊d × tiers / R⌋ / (tiers + 1), 0, 1).
6. delay = d / R × `Max_Secs_For_AE_Delayed_Damage` when d ≥ 1, otherwise 0.
7. **Applying it:**
   - A victim without hardpoints takes factor × blast damage.
   - A victim with hardpoints has the damage split **equally** across its live destroyable hardpoints within R
     (3D distance), each with its own delay.
8. Stop when the victim count reaches `Projectile_Blast_Area_Max_Victims`.

### 3.6 `Take_Damage` (0x3a9e30), order of operations
Arguments: victim, damage kind, amount, source, hardpoint name, out-absorbed, out-armor, no-team flag, raw
flag, out-invulnerable.

**Redirection and gates**
1. **Parent container:** if the victim's parent container (+0x150) is alive, the damage goes to it instead [M].
2. **Teams [H]:**
   - `Redirect_Damage_To_Teammates`: amount/(n − 1) to each other member.
   - `Share_Damage_With_Teammates`: each member takes memberMax × amount / selfMax.
3. **Gates [M]:**
   - invulnerable (+0x37d): only damage kinds 0 and 13 get through;
   - `Immune_To_Damage`;
   - the global damage-enabled flag: only kinds 0, 8 and 13 get through.

**Multipliers, applied in this order [H]**
4. × the victim's `TAKE_DAMAGE` unit-ability multiplier (kind 7).
5. × the attacker's `CAUSE_DAMAGE` multiplier (kind 8). For a projectile, the shooter's value always applies.
   For any other source, it applies only when that source's obj +0x3a1 bit 0x10 is set.
6. × `Base_Shield_Vulnerability_Modifier` when the victim is inside a base shield.
7. **Diminishing firepower:** for kind 1, when the attacker's `ALLOW_TARGET_DIMINISHING_FIREPOWER` flag is set
   (it defaults to true), × the `Diminishing_Firepower` curve evaluated at the seconds since the victim was
   last hit (999999 on the first hit).
8. × `Under_Construction_Damage_Multiplier`.
9. × `Space/Land_Retreat_Units_Damaged_Mod_Factor` while the victim is retreating.
10. **Hardpoint name [H, §2.9 Q1]:** the projectile's targeted hardpoint when the victim is its target and the index
    is ≥ 0; otherwise on land the targetable live hardpoint closest to the projectile, in space the `Collision_Mesh`
    of the mesh the projectile hit.
11. **Lucky shot** (ability id 0x22) on the shooter: it is consumed and its CAUSE multiplier applies [M].
12. × **`Get_Damage_Multiplier`** (0x38e0c0), the product of:
    - (1 + attacker list 1, "damage");
    - (1 − victim list 0, "defense"), where list 0 is clamped ≤ 0.75 (§4.3);
    - the AI-difficulty `Damage_Multiplier` when the attacker is an AI player;
    - `Surface_Type_Cover_Damage_Shield` (+0x1e48) when the victim stands on land cover cells (cell types
      9–11).
13. Damage-event listeners (event 10, e.g. Redirect Blaster) may cancel the damage.

**Shields, armor and hull [H]**
14. x = amount × shield-armor factor (`Damage_To_Armor_Mod[kind][Shield_Armor_Type]`).
15. **Absorption,** skipped for kind 22. In this order:
    - A base-shield "SHIELD" hardpoint absorbs everything.
    - `ShieldBehavior::Absorb` (0x549ac0) applies only to projectiles with `Projectile_Does_Shield_Damage`
      (type +0x45), or to non-projectile sources. It absorbs min(shield, x), and nothing during an ion-storm or
      depleted lockout.
    - The energy pool (0x56c230), only for `Does_Energy_Damage`; it can disable engines.
16. **Armor conversion** of the unabsorbed rest: x = x / shield-armor factor × hull-armor factor
    (`Damage_To_Armor_Mod[kind][Armor_Type]`). `Projectile_Does_Hitpoint_Damage` (+0x47) false sets x = 0.
17. **Hardpoint routing** (§5.1): a named live destroyable hardpoint takes x and the hull takes nothing.
18. **Hull** (0x3ab890):
    - A combustible victim hit with kind 7 (fire) ignites instead of taking damage.
    - Otherwise health −= x.
    - If health ≤ 0 and obj +0x3a1 bit 0x80 (cannot die) is set, health stays at max(1, old).
    - Otherwise `Die` (0x39bdb0).
    - Damage-state models swap when health crosses 75% and 50%.

**Damage kinds** (`tDamageType`, registered at 0x5e1ac0) [M]: 0 Debug, 1 Normal, 7 Fire, 13 Vehicle_Thief, 22
shield-bypassing. The full numbering is not read (§8).

### 3.7 Shields and energy [M unless tagged]
- **Cadence** (0x5494b0): regeneration runs every int(FPS × `SHIELD_REGEN_INTERVAL` × 3.0 + 0.5) frames.
  `SHIELD_REGEN_INTERVAL` is unit-ability kind 3, and 3.0 is `ShieldRechargeIntervalInSecs`.
- **Amount** (0x549250) = `Shield_Refresh_Rate` × `SHIELD_REGEN` (kind 2) × (live SHIELD_GENERATOR hardpoints /
  total SHIELD_GENERATOR hardpoints).
- **Clamps — verified in disassembly [H]** (0x140549360–0x14054938a):
  1. new = clamp(current + amount, 0, maximum shield).
  2. While the depleted lockout is active, new = min(new, `Depleted_Shield_Regen_Cap`) (GameConstants
     +0x1183c, vanilla **0.25**). This is a plain `minss` with the constant, so the cap is **absolute shield
     points, not a fraction** of the maximum.
  - The lockout is active (0x380ad0) while (frame − depletion frame at combat data +0x108) / FPS <
    `Depleted_Shield_Disable_Time` (+0x11820, vanilla 5.0). In vanilla a depleted shield stays at ≤ 0.25
    points for 5 s.
- **Energy cost [H]:** if the unit has an energy pool, regeneration must pay delta ×
  `EnergyToShieldExchangeRate`, or nothing regenerates.
- **Depletion [H]:** reaching 0 starts a `Depleted_Shield_Disable_Time` lockout.
- **Maxima (§4.3):** maximum shield (0x3725f0) = `Shield_Points` × faction factor (player faction data +0x54) ×
  (1 + list 3). Maximum energy (0x372320) = `Energy_Capacity` × (1 + list 4).
- Energy-pool regeneration itself was not analysed.

### 3.8 Lightsaber and deflection (0x709c90) [M]
- Applies only to projectile categories 0, 5 and 7, and only inside `Reaction_Arc`.
- Redirect when **synced** `Get_Float(0, 1)` < `Projectile_Redirect_Chance_Modifier` × redirect chance.
- Otherwise a second draw: block when `Get_Float(0, 1)` < `Projectile_Block_Chance_Modifier` × block chance.

### 3.9 Death
- **Die** (0x39bdb0): health ≤ 0 without the cannot-die flag.
- **Hull and hardpoints are coupled every frame** (§5.1). For types with
  `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` (type +0xa8), killing the last live destroyable hardpoint
  sets hull health to 0, and the hull path then kills the object [H].
- **Destroyed hardpoints:** visuals in `SPACE_AND_OPEN.md` §B.10; gameplay effects in §5.1.
- **`DeathBehaviorClass`** (0x8a2868), death clones and survivors (`Destruction_Survivors`, min/max count at
  type +0x1d48/+0x1d4c) were not analysed in this pass (§8).

---

## 4. Combat-stat modifiers (S8)

There are **two independent systems**. Do not merge them.

### 4.1 System A: the ModifierSet (additive, per stacking category) [H]
**Structure** (constructor 0x535cb0, destructor 0x535fb0, saved as chunks 0 and 2..11):
- A ModifierSet has **11 lists × 0x20 bytes**.
- Each list is an ordered map from **stacking category (int) → vector of 0x30-byte entries**, plus an optional
  minimum clamp (+0x10, flag +0x18) and maximum clamp (+0x14, flag +0x19).
- **An entry** holds:
  - +0 the value (float);
  - +4 the expiry frame: −1 means permanent; otherwise now + int(fps × seconds + 0.5) (0x35cd20); a duration
    ≤ 0 stores 0;
  - +8 the CRC32 of the source name;
  - +0x10 a handle to the source object.
- **Insert** (`ModifierList_Insert`, 0x38c850) appends to the category's vector, creating the category if
  needed (reserve 16).
- **Where sets live:**
  - each object has a set at data pack (obj +0xf0) → +0x138;
  - each player has a tactical set, returned by game-mode vfunc +0x220(player id). The ModifierSet is at its
    start, and a separate player-level list is at +0x1f8 (§4.3, list 7).

### 4.2 The reduction — "ordered max-then-sum", confirmed [H]
`ModifierSet_Reduce_MaxThenSum` (0x33fb70) is called with a list, a combiner and a comparator. Every combat
call site passes the combiner `Plus<float>` (0x27f1f0) and the comparator `GreaterThan<float>` (0x33fa70).
1. Visit stacking categories in **ascending integer order** (std::map order).
2. Within one category, take the **maximum value** (strictly greater replaces, so the first of equal values is
   kept). This is the signed maximum: two debuffs in the same category give the **less negative** one.
3. Seed the total with the first non-empty category's maximum, then add each later category's maximum in
   order. An empty list gives 0.
4. Apply the list's minimum clamp (if flagged) and then its maximum clamp.

The stat is then base × (1 + r), except defense, which is used as (1 − r).

One exception: the player-level reveal-range list at +0x1f8 uses `MostExtreme<float>` (0x35b0c0; largest
|value|, first kept on ties) in place of GreaterThan (0x35d1b0).

**`Gather_Modifiers`** (0x395c70) builds a temporary set for an object:
1. a copy of the **owner player's** tactical set;
2. merged with the **object's own** set (`ModifierSet_Merge` 0x536570 → `ModifierList_Merge` 0x536330, which
   appends entries into the matching categories).

It returns false when neither exists, and callers then use the base value. The merge appends in the same
category order. Because step 2 is a pure maximum, the merge order never changes the result.

### 4.3 The 11 lists [H for the list ↔ reader pairs, M for the names]

| List | Stat | Default clamp | Readers | Main writers |
|---|---|---|---|---|
| 0 | defense (damage taken reduction), used as (1 − r) | max **0.75** | `Get_Damage_Multiplier` 0x38e0c0 (victim side) | CombatBonus `Defense` field; `Elevated_Vulnerability` (0x3a8550, constant land/space value, stacking category 0) |
| 1 | damage dealt | min −1.0 | 0x38e0c0 (attacker side) | CombatBonus `Damage` |
| 2 | max hull health | min −0.99 | `Get_Max_Tactical_Health` 0x3727a0; HP `Get_Max_Health` 0x385ae0 | CombatBonus `Health` (on apply, current health += new max − old max, via 0x3a89d0) |
| 3 | max shield | min −1.0 | 0x3725f0 | CombatBonus `Shield` |
| 4 | max energy | min −1.0 | 0x372320 | CombatBonus `Energy` |
| 5 | max speed | min −0.99 | 0x376990 | CombatBonus `Speed`; tractor beam (−`Target_Speed_Decrease_Percent`, or −2× that under a category condition) [M] |
| 6 | weapon fire rate | min −0.99 | `Fire_At` 0x3825b0, `WeaponBehavior` 0x54cef0/0x54d8f0, ion-cannon and lucky-shot abilities | CombatBonus fire-rate field [M] |
| 7 | reveal (FOW) range | none | 0x35d1b0 | — |
| 8 | weapon/attack range | none | slot 40 0x566ca0, HP `Get_Fire_Range` 0x3857d0, `WeaponBehavior` 0x54d090 | CombatBonus `Fire range` |
| 9 | ability recharge time | min −0.99 | ability countdown (§5.3) | CombatBonus `Recharge` |
| 10 | build time | min −0.99 | 0x496a20/0x496d80/0x496f00 | Reduce-production abilities (0x70a3e0, 0x70b1a0) |

**Full stat formulas:**
- **Max hull** (0x3727a0) = `Tactical_Health` (type +0xdcc) × difficulty health multiplier × faction factor ×
  (1 + list 2).
  - The difficulty multiplier is the GameConstants pair at 0x140b16dcc (land, mode 1) and 0x140b16dc8 (space,
    mode 2); [M] that these are `Object_Max_Health_Multiplier_Land/Space`.
  - The faction factor is player +0x360 → faction data +0x50.
- **Hardpoint max health** (0x385ae0) = `Health` × the mode multiplier (0x540070: Land 1.0 in mode 1, Space 1.5
  in mode 2) × (1 + list 2).
- **Damage** (0x38e0c0) = (1 + list1_attacker) × (1 − list0_victim) × AI difficulty × cover, as in §3.6 step 12.

**Writers that put categories in.** `CombatBonusAbilityClass::vf30` (0x6f42d0) inserts one entry per non-zero
field into the **target object's** set. The stacking category is the ability's `Stacking_Category`, and the
source is the ability's owner. `BattlefieldModifierAbilityClass` (0x6f02c0) inserts (multiplier − 1) into each
affected **player's** set, keyed by its stacking category (+0x178), permanently (−1). Hero auras are
CombatBonus abilities. Practical consequence:
- the same aura from two heroes, sharing a stacking category, counts **once** (the maximum);
- different stacking categories **add up**.

**Expiry [H]:** the reduce does not look at expiry frames. Expired entries are removed by
`ModifierSet_Purge_Expired` (0x5369e0), which runs `ModifierList_Purge` (0x497bd0) on all 11 lists.
- **Rule:** an entry is erased when its expiry frame < the current frame, compared **unsigned**. So −1 (stored
  as 0xFFFFFFFF) never expires, and a stored 0 (a duration ≤ 0) is erased at the next purge. The remaining
  entries in the vector are shifted down in order.
- **Object sets** are purged once per object update, in `GameObject::Update` (0x3a6b80, on data pack +0x138).
- **Player-level sets** are purged by 0x527dd0 [M on its cadence].
- **Reads between purges:** an entry whose expiry frame has passed but which has not been purged yet still
  counts.
- **Explicit removal:** abilities also remove their entries through the remove slot (vf31, matched by source).
- A sibling single-list type (0x281960/0x285e40) purges the same way inline before summing, and floors
  (1 + r) at 0.0001.

**Caching:** none. Every read builds and destroys a stack set (11 trees). The port may cache per object per
tick, invalidated on insert, remove and expiry. The result is identical as long as the per-category maximum
and ascending-category summation are kept.

### 4.4 System B: unit-ability mode multipliers (`Unit_Abilities_Data` → `Mod_Multiplier`) [H]
- **Data:** each `Unit_Ability` record (type +0x1ab0 vector) has a `Mod_Multiplier` float array (ability data
  +0xac, default 1.0) and a `Mod_Flag` byte array (+0xe0).
- **XML form:** the XML gives `<Mod_Multiplier>KIND, value</Mod_Multiplier>` lines.
- **Kinds,** 1-based:

  | Kind | Name | Kind | Name |
  |---|---|---|---|
  | 1 | WEAPON_DELAY | 7 | TAKE_DAMAGE |
  | 2 | SHIELD_REGEN | 8 | CAUSE_DAMAGE |
  | 3 | SHIELD_REGEN_INTERVAL | 9 | FIRE_RATE |
  | 4 | ENERGY_REGEN | 10 | SCATTER_RADIUS |
  | 5 | ENERGY_REGEN_INTERVAL | 11 | TARGET_SPEED |
  | 6 | SPEED | 12 | TARGET_TAKE_DAMAGE |

- **Flags:** WEAPONS_ENABLED_FLAG, CAN_BE_SQUASHED, ALLOW_TARGET_DIMINISHING_FIREPOWER (flag 3) [M for the flag
  numbers].
- **Per-object state:** a slot at obj +0x3d0 + 16 × ability_type, holding the data pointer and an active byte
  at +8.
- **Multiplier** (0x398010) = the **product** over the type's first two ability slots, counted only where the
  ability is active, with LUCKY_SHOT skipped.
  - For a squad leader whose own product is 1, the result is the **minimum** over squad members.
- **Flag** (0x397f60) = AND across the active slots, default true.
- **Where the kinds apply:**

  | Kind | Applied at |
  |---|---|
  | 1, 9 | fire recharge (§3.1) |
  | 2, 3 | shields (§3.7) |
  | 7, 8 | `Take_Damage` (§3.6) |
  | 10 | blast radius (§3.5) |
  | 6, 11, 12 | speed and target effects (§8) |

---

## 5. Hardpoints and abilities (S9)

### 5.1 Hardpoints [H unless tagged]
- **Instance layout:** owner +0x10, type +0x20, health +0x28, disabled flag +0x6e.
- **Type defaults** (constructor 0x53e3b0) [M]:
  - `Is_Targetable`, `Is_Destroyable` and `Is_Turret` default to false;
  - `Randomize_Between_Fire_Bones` defaults to true;
  - `Repair_Amount_Per_Frame` defaults to 3.0;
  - bytes 0x3d1–0x3d7 (the Fire_When / opportunity / engine-hide flags) default to true.
- **Type enum:** as in SPACE_AND_OPEN §B.10: ENGINE 0, SHIELD_GENERATOR 1, GRAVITY_WELL 2, FIGHTER_BAY 3,
  TRACTOR_BEAM 4, weapons 5–10, ENABLE_SPECIAL_ABILITY 11, DUMMY_ART 12.
- **Health:** maximum health as in §4.3. HP `Take_Damage` (0x387f50) applies only when the hardpoint is
  `Is_Destroyable`, the owner matches, health > 0 and amount > 0. It subtracts with **no clamp** and then calls
  the destruction check 0x386660. Set-health (0x387ca0) clamps to [0, max].
- **Routing from `Take_Damage`:**
  - A named hardpoint is looked up by name (0x394d40).
  - A lucky shot at the hull switches to the hardpoint closest to the impact (0x394a80).
  - A live destroyable hardpoint takes **all** of the damage; a **dead** destroyable one takes it and nothing else
    does (the damage is lost, §2.9); a non-destroyable one passes it to the hull.
  - Killing the last one with type +0xa8 set zeroes the hull.
  - Otherwise the damage goes to the hull.
- **Targetability:** with `Is_Targetable`, the target position is the hardpoint's attachment-bone position
  (0x396ce0), otherwise the object centre. Only targetable live hardpoints are picked when there is no priority
  set (§2.6).
- **Hull and hardpoint coupling,** with C = `Hull_Vs_Hard_Points_Health_Constraint` (0.2, GameConstants
  +0x1578):
  - **Hull cap** (in 0x3a6b80, only when type +0xa8 is set): the hardpoint fraction (0x396070) is Σ current /
    Σ max over destroyable hardpoints, 0 when all are dead. The hull fraction is capped at min(1, hardpoint
    fraction + C).
  - **Hardpoint cap** (every frame in 0x3a76b0, before the per-hardpoint updates):
    - T = Σ difficulty-scaled base health, without modifiers;
    - excess = (hardpoint fraction − min(1, hull fraction + C)) × T;
    - when excess is positive, each live hardpoint i loses (health_i / T) × excess.
- **Repair** (0x387170):
  - It is started by network command `RepairHardpointEvent` (id 45, 0x4308b0).
  - Each frame, for each paying player in the list at +0x78: if credits ≥ `Repair_Cost_Per_Frame`, deduct the
    cost, then health = clamp(health + `Repair_Amount_Per_Frame`, 0, max). A player who cannot pay is dropped.
  - If the hull fraction is below the hardpoint fraction, the hull is set to hull × (1 + difference).
  - At full health the system comes back online.
  - **A destroyed hardpoint cannot be repaired.**
- **System offline and online** (`Set_System_Online`, 0x384d00), called on destruction and on repair:
  - **ENGINE**, only when no live engine hardpoint remains:
    - the locomotor engines go off (0x5545a0; permanent if destroyed);
    - TURBO and SPOILER_LOCK are forced off;
    - maximum speed × `Engines_Disabled_Speed_Modifier` (0.4) (0x3728f0).
  - **SHIELD_GENERATOR**, only when none remain [M]: the shield is zeroed, regeneration is blocked (data +0xf0
    → +0x131) and DEFEND is forced off. While some generators remain, regeneration is scaled by the live
    fraction (§3.7).
  - **GRAVITY_WELL:** INTERDICT goes off, and +0x3ca is set, which blocks INTERDICT.
  - **Weapons [M]:** the weapon summary is rebuilt (0x4f4c60); destroyed weapon hardpoints simply stop firing
    (§3.1 step 1). A "last of its weapon class" owner event plays (SPACE_AND_OPEN §B.10 (a)) [L].
  - **ENABLE_SPECIAL_ABILITY [M]:** toggles the hardpoint's `Special_Ability_Name` for the owner's type at
    player level (0x288890).
  - **FIGHTER_BAY and TRACTOR_BEAM** have no branch here. Live counts gate abilities instead (0x39b480):
    TRACTOR_BEAM needs a live type-4 hardpoint, BLAST and LEECH_SHIELDS need a live type-10 one, and
    DEPLOY_SQUAD checks type 3.
- **Destruction visuals and spawned effects:** SPACE_AND_OPEN §B.10. Destruction is not reversible.

### 5.2 Unit-ability modes (`Unit_Abilities_Data`) [H unless tagged]
- **What they are:** toggled stances defined **purely by `Mod_Multiplier`/`Mod_Flag` values in XML** plus
  per-hardpoint delay tables (§3.1, §4.4).
- **Activation:** every activation goes through network command `SetUnitAbilityModeEvent` (id 43, 0x430270). It
  checks `Can_Activate_Unit_Ability` (0x39b480: hardpoint gates, engines, gravity well, …) and that the ability
  is recharged, then calls `Set_Unit_Ability_Active` (0x38a350), which switches on the type.
  - Several types forward to the special ability whose +0x38 field equals the unit-ability type.
  - Autofire is a separate command, `SetAbilityAutofireEvent` (id 58).
- **Enum** (`UnitAbilityType`, 0x5dea20, 1..75); the space-relevant ids are:

  | Id | Ability | Id | Ability |
  |---|---|---|---|
  | 1 | DEFEND | 41 | ION_CANNON_SHOT |
  | 3 | INTERDICT | 43 | SPOILER_LOCK |
  | 9 | TURBO | 48 | STEALTH |
  | 29 | POWER_TO_WEAPONS | 56 | LASER_DEFENSE |
  | 30 | TRACTOR_BEAM | 62 | FULL_SALVO |
  | 32 | MISSILE_SHIELD | 63 | SENSOR_JAMMING |
  | 33 | INVULNERABILITY | | |

  Ids 10 and 49 are LURE and STUN, order unknown [M].
- **Vanilla space usage,** by count: POWER_TO_WEAPONS 18, HUNT 18, SPREAD_OUT 16, SPOILER_LOCK 11, STEALTH 10,
  ION_CANNON_SHOT 10, DEFEND 7, TRACTOR_BEAM 5, TURBO 5, then MISSILE_SHIELD, LASER_DEFENSE, INTERDICT and
  others.
- **Implementation order for S9:**
  1. multiplier-only modes: POWER_TO_WEAPONS, DEFEND, TURBO, SPOILER_LOCK, FULL_SALVO;
  2. the hardpoint-gated ones: TRACTOR_BEAM, INTERDICT;
  3. ION_CANNON_SHOT, STEALTH, MISSILE_SHIELD and LASER_DEFENSE, whose class internals are still open (§8).

### 5.3 Ability timers (AbilityCountdownBehavior, behaviour 0x46, data at obj +0x1e8) [H]
- **State:** per ability type, an int counter, an int maximum and a byte flag. The update (0x42f910) runs on the
  frame delta.
- **While active:** the counter counts up to `Expiration_Seconds` × 30. On expiry the ability deactivates and the
  recharge starts (0x42fb70).
- **Recharge frames** = int(30 × `Recharge_Seconds` × (1 + ModifierSet list 9)).
- **Cancelled early:** the recharge becomes round(R × elapsed / duration).
- All timers are integer frames, so they are deterministic.

### 5.4 Special abilities (the `Abilities` list) [M]
- **Framework:** the list is at type +0x1a98. The base class is `SpecialAbilityClass` (vtable 0x874e28,
  32 slots):

  | Slot | Role |
  |---|---|
  | 28 | parse tags |
  | 29 | validate |
  | 30 | apply (owner, target) |
  | 31 | remove |
  | 18 / 19 | apply / remove on unit add / remove, gated by mode |
  | 21 | `Take_Damage` hook |

- **Activation styles** (`Activation_Style` enum, 0x5e42e0): 4 = Space_Automatic (mode 2), 5 = Ground_Automatic
  (mode 1).
- **Enabling:** an ability is enabled by a player override, otherwise by `Initially_Enabled`
  (`Set_Ability_Enabled`, 0x37df00).
- **Manager:** `SpecialAbilityManagerClass` (0x865c20) is the signal hub.
- **Auras** (CombatBonus and the like) are **event-driven**: they apply and remove on unit add, remove and
  mode signals. **No radius scan and no periodic recheck** was found [M]. The modifier semantics are in §4.3.
- About 92 ability classes exist (SUBSYSTEMS.md "abilities"). Only CombatBonus, BattlefieldModifier, tractor
  beam and the reduce-production pair were read in this pass.

---

## 6. Port checklist (implementer summary)
1. **Tree (S6):** any BVH. Keep the membership rules of §1.5 and the 2D radius semantics of §1.6. Sort every
   gameplay result by object id. Use per-query buffers. Rebuild or refit on our own schedule (the vanilla
   cadence is not observable gameplay, except through result order, which the id sort removes).
2. **RNG:** one synced stream. Draws in this order:

   | When | Draws |
   |---|---|
   | per target scan | 2 |
   | per candidate with hardpoints | 1 |
   | per hardpoint at creation | 1 |
   | per shot | recharge 1 at pulse end, muzzle 3 or 1, inaccuracy 3 |
   | per hit | random-hardpoint 1, delay 1, particle pick 1 |
   | non-hardpoint weapon reset | 1 |
   | deflection | 1–2 |
   | per opportunity scan | 1 |

3. **Modifiers:** two systems.
   - The ModifierSet: per-category maximum, then the sum in ascending category id, then the list clamp;
     base × (1 + r), defense (1 − r).
   - The unit-ability `Mod_Multiplier`: a product over the active slots, with a minimum over the squad.
4. **Damage pipeline:** exactly the order of §3.6. Shield absorption happens in shield-armor space and the rest
   is converted to hull-armor space.
5. **Hardpoints:** list-order updates, the hull/hardpoint 0.2 coupling, the offline rules of §5.1, no repair of
   destroyed hardpoints.

---

## 7. Named entry points (this pass)

Names are proposals. The Ghidra project was not updated. Grouped by section:

**TreeCull / GOM**

| RVA | Proposed name |
|---|---|
| 0x20e600 | `Insert` |
| 0x20e6d0 | `Remove` |
| 0x20f0a0 | `Update_Bounds` |
| 0x20f180 | `Rebuild` |
| 0x20fc50 | `Split_Node` |
| 0x210610 | `Partition_Links` |
| 0x210480 | `Find_Deepest_Containing_Node` |
| 0x210d30 | `Inflate_Subtree` |
| 0x20f000 | `Rebuild_All_Dirty` |
| 0x20e780 | `Query_AABB` |
| 0x20fef0 | `Collect_AABB` |
| 0x20c000 | `Classify_OBB_vs_Sphere` |
| 0x2a8fb0 | `GOM::Query_Radius` |
| 0x2c0720 | `Radius_Sort_Compare` |
| 0x2a9670 | `GOM::Find_Free_Placement` |
| 0x2b5ad0 | `GOM::Register_Object` |
| 0x2b68d0 | `GOM::Unregister_Object` |
| 0x2a6a00 | `GOM::Destroy_Object` (tree removal part) |
| 0x2b3ac0 | `GOM::Change_Owner_Trees` |
| 0x2af4f0 | `GOM::Rebuild_All_Trees` |
| 0x2acb60 | `GOM::Player_Tree` |
| 0x39ac80 | `Is_In_Tree_E0` |

**Targeting**

| RVA | Proposed name |
|---|---|
| 0x633a30 / 0x634810 / 0x633ae0 | `Targeting::Service` / `_Land` (mode 1) / `_Space` (mode 2) |
| 0x62ee00 / 0x62eeb0 | `TeamTargeting::Service` / `_Space` |
| 0x568a00 | `Acquire_Target` |
| 0x5662b0 | `Scan_Player` |
| 0x566f50 | `Get_Scan_Range` |
| 0x566ca0 | `Get_Max_Attack_Range` (slot 40) |
| 0x3749d0 | `Get_Base_Attack_Range` |
| 0x566ba0 | `Get_Priority_Score` (slot 35) |
| 0x566ed0 | `Get_Priority_Set` |
| 0x54dc00 | `PrioritySet_Score` |
| 0x54dd80 | `PrioritySet_Hardpoint_Rank` |
| 0x483f10 | `PrioritySetManager_Load_Files` |
| 0x567c50 | `Validate_Current_Target` |
| 0x567110 | `Is_Better_Target` |
| 0x569000 | `Is_Target_Sticky` |
| 0x58ba40 | `DamageTracking_Time_To_Kill` |
| 0x565e50 / 0x565de0 | order reconciliation |
| 0x569160 | `Set_Target` |
| 0x569310 | `Replace_Target_Ref` |
| 0x38d730 | `Issue_Auto_Target` = `Order_Attack` (auto and ordered attacks, §2.8 C) |
| 0x566b40 / 0x568c30 | targeting slots 33 / 32: read / write the order lock TD +0xf4 (§2.8) |
| 0x41a2e0 | `Formation_Attack_Target` (§2.8) |
| 0x4160a0 | formation response-range test (§2.8) |
| 0x566f50 | scan cube half-extent (§2.8) |
| 0x5680b0 | formation-target targetability test (§2.8 B) |
| 0x40e260 | formation attack staging (§2.8) |
| 0x738870 / 0x7390e0 / 0x738b90 / 0x739890 / 0x7394f0 | `SpaceMovementBlockStatus` ctor / object init / issue / signal handler / IsFinished (§2.8) |
| 0x53f7b0 | `EnumConversionClass<HardPointType>` ctor (global 0x140b310a0, §2.4) |
| 0x394610 | `Choose_Target_Hardpoint` |
| 0x387b70 | `HP_Set_Target` |
| 0x385190 | `HP_Scan_Player` |
| 0x385620 | `HP_Allows_Opportunity_When_Idle` |
| 0x383ba0 | `HP_In_Fire_Cone` |
| 0x383f70 | `HP_Choose_Aim_Point` |
| 0x54a970 | `Can_Aim_At` |
| 0x540140 | `HPType_Category_Restricted` |
| 0x384850 | `Does_Weapon_State_Allow_Firing_At` |
| 0x2824f0 | `Player_Is_Enemy` |
| 0x35f470 | `FOW_Object_Hidden` |
| 0x35f3b0 | `FOW_Point_Test` |
| 0x39aa60 | `Is_In_Nebula` |
| 0x39b140 | `Is_Stealthed` |
| 0x372210 | `Is_Short_Range` |
| 0x397900 | `Get_Space_Layer` |

**RNG**

| RVA | Proposed name |
|---|---|
| 0x1ffb40 | `GameRandom::Get_Int` |
| 0x1ffbb0 | `GameRandom::Get_Float` |
| 0x1ffdb0 | `GameRandom::Percent` |

**Weapons and damage**

| RVA | Proposed name |
|---|---|
| 0x381a90 | `HardPoint::ctor` |
| 0x387010 | `HP_Update` |
| 0x387400 | `HP_Update_Weapon` |
| 0x3825b0 | `HP_Fire_At` |
| 0x381dc0 | `HP_Apply_Inaccuracy` |
| 0x385e70 | `HP_Get_Fire_Bones` |
| 0x3857d0 | `HP_Get_Fire_Range` |
| 0x385ae0 | `HP_Get_Max_Health` |
| 0x540070 | `HPType_Health_Mode_Scaled` |
| 0x399450 | `Compute_Lead_Point` |
| 0x29f810 | `GOM::Spawn_Projectile_Object` |
| 0x54ade0 | `Spawn_Projectile` |
| 0x54cef0 / 0x54d8f0 / 0x54d090 | `WeaponBehavior::Reset_Recharge` / `Pulse_Delay` / `Service` |
| 0x4ce6a0 | `Projectile::Service` |
| 0x4d0080 | `Projectile::Step` |
| 0x4c97a0 | `Projectile::Sweep` |
| 0x4cb280 | `Projectile::Process_Hit` |
| 0x4c73d0 | `Projectile::Apply_Direct_Damage` |
| 0x4c7f70 | `Projectile::Apply_Area_Damage` |
| 0x4c7050 | `Projectile::Move_And_Expire` |
| 0x4c6e30 | `Projectile::Check_Missile_Shield` |
| 0x4cf870 / 0x4ceea0 / 0x4cfc40 / 0x4cf460 / 0x4cec80 | flight modes |
| 0x3a9e30 | `GameObject::Take_Damage` (SUBSYSTEMS' 0x3aa636 lies inside it) |
| 0x38e0c0 | `Get_Damage_Multiplier` |
| 0x395190 | `Pick_Random_Hardpoint` |
| 0x395850 | `Get_Armor_Factor` |
| 0x2c7d80 | `GameConstants::Damage_To_Armor_Mod` |
| 0x3ab890 | `Apply_Hull_Damage` |
| 0x3ab650 | `Queue_Delayed_Damage` |
| 0x3a89d0 | `Set_Health` |
| 0x39bdb0 | `Die` |
| 0x549ac0 | `Shield::Absorb` |
| 0x549250 | `Shield::Regenerate` |
| 0x5494b0 | `Shield::Service` |
| 0x549810 | `Shield::Set` |
| 0x56c230 / 0x56be20 | `EnergyPool::Absorb` / `Spend` |
| 0x709c90 | `Saber_Deflect` |
| 0x5e1ac0 | `tDamageType` enum registration |

**Modifiers**

| RVA | Proposed name |
|---|---|
| 0x535cb0 / 0x535fb0 | `ModifierSet` ctor / dtor |
| 0x38c850 | `ModifierList_Insert` |
| 0x35b380 | `ModifierList_Insert_Simple` |
| 0x35cd20 | `Duration_To_Expiry_Frame` |
| 0x536570 / 0x536330 | `ModifierSet_Merge` / `ModifierList_Merge` |
| 0x33fb70 | `ModifierSet_Reduce_MaxThenSum` |
| 0x33fa70 | `GreaterThan<float>` |
| 0x35b0c0 | `MostExtreme<float>` |
| 0x27f1f0 | `Plus<float>` |
| 0x395c70 | `Gather_Modifiers` |
| 0x3727a0 | `Get_Max_Tactical_Health` |
| 0x3725f0 | `Get_Max_Shield` |
| 0x372320 | `Get_Max_Energy` |
| 0x376990 | speed reader |
| 0x35d1b0 | `Update_Reveal_Range` |
| 0x281960 | `SimpleModifierList_Reduce` |
| 0x5369e0 / 0x497bd0 | `ModifierSet_Purge_Expired` / `ModifierList_Purge` |
| 0x374890 / 0x53ff30 | `Get_Fixed_Inaccuracy_Radius` / `HPType_Get_Inaccuracy_Distance` |
| 0x380ad0 | `Shield_Depleted_Lockout_Active` |
| 0x3a8550 | `Apply_Elevated_Vulnerability` |
| 0x5366b0 / 0x5367e0 | `ModifierSet` load / save |
| 0x398010 | `Get_Unit_Ability_Mod_Multiplier` |
| 0x397f60 | `Get_Unit_Ability_Mod_Flag` |
| 0x565990 / 0x565970 | `AbilityData` multiplier / flag accessors |

**Hardpoints and abilities**

| RVA | Proposed name |
|---|---|
| 0x387f50 | `HP_Take_Damage` |
| 0x387ca0 | `HP_Set_Health` |
| 0x386660 | `HP_Check_Destroyed` |
| 0x384d00 | `HP_Set_System_Online` |
| 0x387170 | `HP_Service_Repair` |
| 0x3a76b0 | `Update_Hardpoints_And_Constrain` |
| 0x3a6b80 | `GameObject::Update` (hull cap, delayed damage) |
| 0x396070 | `Get_Hardpoint_Health_Fraction` |
| 0x396470 | `Get_Min_Health_Fraction` |
| 0x397c10 | `Count_Live_Hardpoints_Of_Type` |
| 0x394d40 / 0x394a80 | hardpoint by `Collision_Mesh` name (`_stricmp`) / closest (args: include dead destroyable, include non-targetable) (§2.9) |
| 0x367250 / 0x367100 | GameObjectType ctor / its flag-block initialiser (this = type + 0x10; sets +0xa8 = 1, §2.9) |
| 0x383f70 | HP aim point on an object without a target hardpoint (bone list, random-start hardpoints, centre; §2.9) |
| 0x385c70 | hardpoint world position (attachment bone, else hp +0x38 bone, else 0) |
| 0x566ed0 | priority set of a targeting behaviour (Lua override TD +0x160, else type +0x520) |
| 0x54dd80 | hard-point rank in a priority set (1-based, −1 excluded, 1e18 unlisted) |
| 0x396ce0 | `Get_Target_Position` |
| 0x3728f0 | `Get_Max_Speed` |
| 0x5545a0 | `Locomotor_Set_Engines` |
| 0x39b480 | `Can_Activate_Unit_Ability` |
| 0x38a350 | `Set_Unit_Ability_Active` |
| 0x39b950 | `Is_Unit_Ability_Active` |
| 0x42f910 / 0x42fb70 | `AbilityCountdown::Update` / `Start_Recharge` |
| 0x430270 / 0x4308b0 | `SetUnitAbilityModeEvent` / `RepairHardpointEvent` |
| 0x5dea20 / 0x5e42e0 | `UnitAbilityType` / `Activation_Style` enum registration |
| 0x37df00 | `Set_Ability_Enabled` |
| 0x6f42d0 | `CombatBonusAbility::Apply` |
| 0x6f02c0 | `BattlefieldModifierAbility::Apply` |
| 0x288890 | `Player_Toggle_Special_Ability` |

---

## 8. Coverage gaps and items to verify

**Resolved 2026-10-27 by disassembly** (llvm-objdump, retail `corruption\StarWarsG.exe`)
1. Targeting step 2 has no guard: equal or worse scores fall through to the aim and distance tie-breaks (§2.4)
   [H].
2. Inaccuracy is doubled in space (§3.1 step 10) [H].
3. `Depleted_Shield_Regen_Cap` is absolute shield points, 0.25 in vanilla (§3.7) [H].
4. At pulse end W is applied only when W < 1; on the pulse-delay branch it is unconditional (§3.1) [H].

**Arguments lost in decompilation**
5. The three `Randomize_Between_Fire_Bones` draws.
6. The map-supplied rebuild threshold and cell parameters (§1.4).
7. The per-frame death roll for converted units in 0x3a6b80. It consumes a synced draw **every frame**.

> **Status 2026-10-30:** Item 7 is implemented: `CombatSystem::converted_decay` (`engine/tactical/combat/src/combat_system.cpp:2011-2022`) makes one synced Get_Float per frame for a converted unit (draw bounds +-d/10 are marked [M] there).

**Enums not read from their tables**
8. Damage kinds (only 0, 1, 7, 13 and 22 are known), `Projectile_Category` names, `Mod_Flag` numbering, and the
   LURE/STUN ids.

**Not identified**
9. Targeting vfunc slots 33 (probably "is player ordered"), 44 and 45 (range and can-attack tests). **33
   resolved** (refixf t06): it returns the order lock TD +0xf4 (§2.8). **44 / 45
   resolved** (max-range test with footprint extent, min-range test): TACTICAL_GAPS.md §3.1. Note: the
   targeting FOW filter (§2.3 step 4) calls 0x35f470 with **force = 1**, and obj +0x343 is the MARKER slot
   (bombing-run markers skip the FOW test): TACTICAL_GAPS §5.8.
10. The meaning of the GOM+0xe0 tree, and of the per-player byte at obj +0x332 + player.
11. Which way round the results of 0x35f3b0 and 0x35f470 go (true meaning hidden or visible).
12. ~~The constant values at 0x140b27338, 0x140b27340, 0x140b278ac and 0x140b27048~~ — resolved: GameConstants
    fields `Health_Low_Percent_Threshold` (FoC 0.33, TR 0.50), `Melee_Cutoff_Range` (51),
    `Garrisoned_Max_Attack_Distance_Multiplier` (1.5), `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` (FoC 3.0,
    TR 5.0); the diminishing-firepower curve is a natural cubic spline over (x, y) pairs: TACTICAL_GAPS.md
    §3.2–3.3. §3.6 step 18's space "model swap" is a damage-smoke attach; `_ALT` rule: TACTICAL_GAPS §6.3.
13. ~~The ModifierSet expiry purge~~ — resolved: 0x5369e0, per object update (§4.3).

**Not analysed**
14. `Does_Weapon_State_Allow_Firing_At` beyond its gate list.
15. The GOM precise collision test (0x38f900) and its mask bits.
16. Energy-pool regeneration.
17. Spline and rocket projectile categories (0x4cfc40, 0x4cf460), only skimmed.
18. `DeathBehaviorClass`, death clones and destruction survivors. (Spin-away and survivors: TACTICAL_GAPS.md §1.9.)

> **Status 2026-10-30:** Item 18 is partly done. Spin-away, `Destruction_Survivors` and death clones are in `engine/tactical/combat/src/combat_system.cpp:1556-1599` (`death_spawns`, the death-clone loop, the spin-away draw). What remains: the deploy / infection key remaps of the death clone are not modelled (comment at `combat_system.cpp:1558`).
19. Land facing and arc logic (`Base_Land_Targeting_Arc_Angle_Coefficient`, 0x633ae0).
20. ~~AI autofire~~ — in the sim (`Issue_Auto_Target` 0x37d280): TACTICAL_GAPS.md §1.3.

> **Status 2026-10-30:** Item 20 is implemented in the abilities module: the autofire driver and its synced candidate pick are in `engine/tactical/abilities/src/ability_behaviors.cpp:160-237`.
21. What fires the apply slots (18/19) for units present at battle start.
22. How kinds 6, 11 and 12 (SPEED, TARGET_SPEED, TARGET_TAKE_DAMAGE) are consumed.
23. Per-class internals of ION_CANNON_SHOT, LASER_DEFENSE, STEALTH, MISSILE_SHIELD and the other roughly 85
    ability classes. (Tactical classes used by FoC / TR: ABILITIES_HARDPOINTS §3 and TACTICAL_GAPS.md §2.)
24. Hero clash (`HeroClashManagerClass`).
25. Fighter-bay spawning.

> **Status 2026-10-30:** Item 25 is implemented for space carriers and stations (SPAWN_SQUADRON behaviour 0x1A) in `engine/tactical/hangar/include/alamo/tactical/hangar/hangar_system.hpp` (spec: STATION_HANGARS_SOUND §1-§2). The land structure spawner path (bones, Structure Open / Hold / Close states, `Spawn_Garrison_On_Load`) is not modelled there. Item 24 (hero clash) has no engine code and stays open.

**Runtime probes worth preparing** (`tools/probe_mod/`, Gabriel launches)
- Log the synced RNG state at the start and end of a single capital-ship volley, to confirm the per-shot draw
  count.
- Two heroes with the same aura, to confirm the per-category maximum in game.
