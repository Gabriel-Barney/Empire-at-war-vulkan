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
- **Squads and teams:** `TeamTargetingBehaviorClass` (0x8afd90; 0x62ee00 → 0x62eeb0) shares the helpers below.
  Correction (S7 t25 RE, 2026-11-02) [H]: it runs only when the container's locomotor kind (vf +0x238) is 1 (Walk,
  Bike, LandTeamContainer, LandTeamInfantry), i.e. ground team containers only, not "in space only". Space squadron
  containers (Team locomotor, kind 2) never scan; their target comes from the members' own TARGETING.
  What the ground container pushes to its members (state, ordered flag, attack-idle pose; not the target): §3.1.1
  "Targeting states, slots 47 / 48 and team copies" (s78f, 2026-11-03).
- **Shared squadron target (2026-11-02)** [H]. A member whose own target is empty and whose due scan picks t sets
  the team target (0x406370, team data = container +0x110: +0x30 = t, +0x38 = hard point) and calls `Set_Target` on
  **every** member in index order, overwriting their targets; `Order_Attack`'s set path does the same. A member that
  already has a target and switches on a rescan (§2.5) changes only its own. Signal 0x28 on the team target clears
  +0x30 / +0x38. Full rules and the clear paths: FIGHTER_FLYING §4.10 B.
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
  - players marked inactive (player +0x68 → +0x108) — this is the faction's `Is_Neutral` flag (2026-11-02, §2.10);
  - players whose relation in attacker player +0x370[id] is not "enemy" (1);
  - all players, when the attacker sits inside an `Is_Nebula` object and the fog test 0x35f3b0 applies [M].
- **Query:** for each visited player, `Scan_Player` (0x5662b0) runs `Query_AABB` on that player's tree with a
  cube of half-extent R centred on the attacker (R + the formation's chase / response range while the unit is in a
  formation move, 0x566f50, §2.8). The scan never does a sphere test itself.
  Rule of 0x566f50 (S7 t25 RE, 2026-11-02) [H]: R is the member's own `Get_Max_Attack_Range`; deployed → R; in space
  the formation is the squadron container's. No formation → R. 0x4160a0(F, u, none) ≠ 0 → **R + Rr**, Rr chosen by
  the first block's flags (0x02 `Guard_Chase_Range`, else 0x01 `Attack_Move_Response_Range`, else `Idle_Chase_Range`).
  Otherwise R in space; on land R with a TURRET slot, else 0 (no scan). In space only `Space_Layer` 0 units (fighters,
  squadrons) get R + Rr, and a plain position move gets it only after arrival. Branch details: FIGHTER_FLYING §4.10 A. Per-candidate range acceptance goes
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
  candidate becomes the current best. **Correction (s78g t04, §2.10):** each player's first passing candidate
  replaces the best unconditionally, so the pick comes from the last visited player that has a candidate.
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

### 2.10 Player relation table: initial values and readers (RE, 2026-11-02) [H unless tagged]
**Table.** Player +0x370 is an int per player id; values **0 friendly (self, allies), 1 enemy, 2 neutral**. The player
constructor fills every entry with **2**. The only writer is 0x288800(player, id, value). Readers: "friendly" =
value 0 (0x2823c0 / 0x2823e0), "enemy" = value 1 (0x2824d0 / 0x2824f0, Lua `PlayerWrapper.Is_Enemy` 0x603760 →
0x2824f0). Objects use their owner (capture points and planets their controller): hostile 0x39a450 / 0x39a4e0
(value 1), allied 0x39a230 (value 0). So a relation-2 player is **neither enemy nor ally**.

**Who sets the values:**
- **Campaign / galactic** (`Create_Default_Players` 0x293390 → 0x294360; re-run on galactic resume 0x4b2c70). For
  each ordered pair (row P, column Q), with F = P's faction and G = Q's faction:
  1. self → 0;
  2. F or G `Is_Neutral` (faction +0x108) → **2**;
  3. F = G → 0;
  4. G's name in F's `Allies` list (+0x3e0) → 0; then G's name in F's `Enemies` list (+0x3f8) → 1 (Enemies wins
     over Allies; names compared case-insensitively);
  5. in neither list → no write, the constructor's **2** stays ("undefined feelings" in the log).

  The table can be **asymmetric**. FoC data: everything ↔ Neutral = 2; Rebel / Empire / Pirates / Sarlacc /
  Underworld / Hutts → Hostile = 1; Hostile → those = 1, except **Hostile → Hutts = 2** (not in Hostile's
  `Enemies`), while Hutts → Hostile = 1. Hostile's `Allies` = Neutral has no effect (rule 2 comes first).
- **Skirmish** (0x294fa0 → 0x297300; all three callers pass 0): runs `Create_Default_Players` first, then overwrites
  every pair: P's faction `Is_Neutral` → 2 towards everyone (**its own entry too**); else self → 0; Q's faction
  `Is_Neutral` → 2; same faction → 0; otherwise 1. The XML lists are ignored, so Hostile is an enemy of every
  non-neutral faction both ways.
  - **In-game, seen against this (s78f probe, 2026-11-05)** [M: cause not traced]. In a Geonosis skirmish
    (Empire human vs Rebel AI), T4Bs spawned by Lua for `Find_Player("Hostile")` (player 4) were never targeted by
    the AI's units: targeting state 0 throughout. That held for the SPMAT, six `Pod_Walker`s and a garrisoned Merc
    squad, both on auto and after an explicit Lua `Attack_Target` (which returned without error)
    (`out/20261105-045131`, `-051443`). The same T4Bs with the same setup (`Prevent_All_Fire`, `Prevent_AI_Usage`,
    `Suspend_Locomotor`), spawned for the human's Empire player, were acquired within frames (`out/20261105-055142`,
    `-060346`). So, for targeting, either the skirmish Hostile player is not set up by 0x297300 like the slot
    players, or something before the relation test drops it. Check that before relying on "Hostile is an enemy" in
    skirmish.
    Relation check (s78f t12, 2026-11-05, `out/20261105-062322`, REL records): Lua `Is_Enemy` between the Rebel AI
    and Hostile (player 4) is **true both ways**, in land and in space (Empire: true; Neutral and Rebel: false). So
    the relation Lua sees is "enemy", and the drop happens elsewhere [M: the candidate scan skips the Hostile
    player's objects, or they are not in the per-player lists the scan reads]. **Superseded by s78g t04 below:
    there is no drop.**
  - **s78g t04 (RE and raw-log re-read, 2026-11-05): nothing drops Hostile-owned objects** [H for the code paths
    and the logged states unless tagged].
    1. *Binary.* The only owner-dependent tests on the target paths are these:
       - Auto scan `Acquire_Target` 0x568a00, per visited player: skip when the player's faction (player +0x68)
         has `Is_Neutral` (+0x108), or when the attacker player's +0x370[id] ≠ 1 (0x2824f0). `Hostile` is
         `Is_Neutral` No (FACTIONS.XML), and 0x297300 gives it 1 (the same table Lua `Is_Enemy` 0x603760 reads).
       - `Scan_Player` 0x5662b0 and `Validate_Current_Target` 0x567c50 (§2.3) test the owner only in the stealth
         exception (same owner) and in the fog test 0x35f470, where an ally of the attacker counts as visible
         (0x39a230). Nothing else about the owner is read.
       - `Order_Attack` 0x38d730, which Lua `GameObjectWrapper.Attack_Target` 0x5730f0 calls with (unit, t, 0,
         ordered 1, 0, −1, force 0): 0x2824d0(attacker's player, target +0x58) must be 1. The other refusals
         (stealth, melee vs airborne, garrisoned, +0x3a0 & 0x42) do not depend on the owner.
       - No path tests the player type, human or AI, `Is_Playable`, `Create_Player_In_Multiplayer_Games` or any
         other player flag.
       - The player trees exist for Hostile. 0x2af130 allocates GOM+0xd0 for `DAT_140a16fb0` (the maximum player
         count). `Register_Object` 0x2b5ad0 inserts every collidable object at GOM+0xd0 + owner × 0x38, and the
         scan queries tree player +0x4c.
    2. *Logged states* (`tools/probe_mod/s7s8_probes/out/20261105-051443/reader.log`, Geonosis, Rebel AI = player 1,
       victims owner 4 = Hostile):
       - IDLE47 phase b: the six `Pod_Walker`s (82269 … 82279, owner 1) took the held Hostile T4Bs (82270 …
         82280, owner 4) as targets 7 frames after the spawn (F 4061). They reached state 3, then state 4 by
         F 4086, and were still in state 4 in the last samples (F ≈ 4650).
       - REINF: `MZ8_Pulse_Cannon_Tank` 82938 took Hostile T4B 82901 two frames after its spawn (F 12498, state
         3), reached state 4 at +40 and fired 8 shots.
       - The report's "attackers 6 ts {}" for IDLE47 b is a parser miss on sparse ST samples, not what the game did.
    3. *Why the s78f cases saw state 0.* Neither case compares owners on equal terms.
       - **SPMAT** (`-045131`, `-051443`): this was the DEPLOY scenario. The attack order at F 480 was followed by
         `deploy1` at F 495, which ran a deploy block (ret = block, finished +45, deploy clip 25). In the Empire
         control (`-055142`) the same call returned nil, with no transition, and the SPMAT took the target. So the
         unit state differed between the runs [M: what keeps the deploying SPMAT at state 0 is not traced].
       - **Garrisoned mercs** (`-051443` BUNKER): the Hostile T4B stood at (3180, 4640), 300 from the bunker
         centre. That is beyond the merc's 172.5 reach, which is measured from the bunker centre (LOB_SOLVER,
         BUNKER2 probe). This is a range result.
    4. *Side finding: selection across players* [H from the decompiled code; not re-checked in disassembly]. This
       corrects §2.4 "the best score is shared across all players". `Scan_Player` starts each player with no local
       best. It accepts that player's first passing candidate unconditionally (the test is "shared score = −1
       **or** no local best") and overwrites the shared score with it. Later candidates of the same player use the
       §2.4 replacement rule. 0x568a00 replaces its result with every non-null per-player return. **The pick is
       therefore the best candidate of the last player, in the cyclic visit order from the random start player, that
       has a passing candidate.** A better candidate of an earlier player is lost. The early exit (current target
       scoring exactly 1.0) fires only once that candidate has been accepted as the local best. The scan then stops
       at that player.
    5. *Index and ID* [M]. 0x568a00 counts players by the player vector (list +0 / +8). It fetches each player
       **by ID** (0x294bc0, the by-ID table at list +0x20 / +0x28), and it skips the ID equal to its own player's
       list index (+0x48). 0x291a50 gives each new player the lowest free ID, so ID = index in skirmish and
       campaign, and the mix does no harm there. In the decompiled loop, a null player entry, or a fog failure while
       the attacker is inside an `Is_Nebula` object (0x35f3b0), does not advance the player index. The remaining
       iterations then repeat that entry, so later players are not visited.
    - *Port* (read-only check): `TacticalBattle` sets `PlayerInfo.active = !Is_Neutral` and builds the relation
      from `bt_relation`. `CombatSystem::gather` / `decide_target` (`engine/tactical/combat/src/combat_system.cpp`) visits every active enemy player. Hostile is targeted, as in retail. The port
      differs in item 4: it carries one best across all players. To match retail, restart the best at each
      visited player that has a candidate. Also take the early exit only after the candidate has been accepted
      as best.
    - *Optional probe* to close the SPMAT [M]: the SPMAT scenario without the `DEPLOY` calls, run twice at the same
      frame offset, once with Hostile victims and once with Empire victims. Both should reach state 3 / 4 on v275.
- **Skirmish with the map / game record byte +0xb5 set** (0x295cf0 → 0x297470) [M: a team-game setting]: same, but
  allies are self or the same team id (player +0x54 ≠ −1); faction is not used.
- **At run time:** Lua `PlayerWrapper.Make_Ally` (0x6046a0) writes 0, `Make_Enemy` (0x604780) writes 1 (one
  direction: the caller's row only); command 35 `Ally` (0x689be0) toggles 0 ↔ 1 (any other value: no change), shows
  the allegiance text and re-issues 0x38d730 with no target to the issuer's units [M: clears their attack targets].
  Nothing writes 2 after start.

**So `Is_Enemy(p, Neutral)` is false** in campaign and skirmish. Unless a script calls `Make_Enemy`, nothing treats
the Neutral player as an enemy.

**Every combat path reads this one table:**
- auto-targeting scan (§2.2): only players whose entry is 1, and not `Is_Neutral` factions;
- `Formation_Attack_Target` / goal-object promotion (§2.8): object owner hostile (1);
- **human right-click attack:** the order resolver 0x2ad280 returns the attack cursor (5) only when the local
  player's entry for the object's owner is 1; any other value gives a move / other cursor;
- projectile segment hits, area damage and stun (§3.4, §3.5, TACTICAL_GAPS): the "enemy players" lists 0x294910 /
  0x2824f0 (value 1);
- the turret service drops a target whose owner is not hostile (0x632270 → TARGETING vf +0xd8).

**Task-force `Attack_Move` on a non-hostile object** is **not refused**:
- `LandTaskForceClass.Attack_Move` (0x6e9440): for a game object or an AI target whose object is hostile to the TF
  player (0x39a4e0) the goal is the object; otherwise the goal is the object's (or the AI target's) **position**,
  an attack-move to that point.
- `SpaceTaskForceClass.Attack_Move` (0x6e4fc0) has no relation test: the object is the goal.
- In both cases the units move there and auto-engage on the way. They never attack the relation-2 object itself,
  because §2.8 path B needs relation 1. A neutral capture point is taken by presence, not by attack.

Port: the combat matrix must treat value 2 like 0 for targeting, hits and area damage (neither side attacks). It
must not refuse an AI `Attack_Move` whose goal is neutral-owned; the move goes to the point.

---

## 3. Weapons, projectiles, damage (S8)

### 3.1 Hardpoint fire cycle [H unless tagged]
**Construction** (0x381a90):
- The first cooldown (+0x58) = **synced** `Get_Int(0, int(FPS × Fire_Max_Recharge_Seconds))`, drawn when the
  hardpoint instance is created. Creation order is the owner's `HardPoints` list order.
- Pulses remaining (+0x5c) = `Fire_Pulse_Count`.

**`Update_Weapon`** (0x387400), weapon types 5–10 only, once per frame per hardpoint:
1. Return early when weapons are globally disabled, the hardpoint is disabled (+0x6e) or destroyed, or an owner
   state gate applies (deploy transition; after the cooldown, `Does_Weapon_State_Allow_Firing_At` `0x384850`: §3.1.1).
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
      **A1j t03 [H]:** the three draws are made only when the scaled d > 0, the XY distance > 0 and
      `Get_Fire_Range` > 0; otherwise no draw and no offset (no table entry for the target's category means d = 0).
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

#### 3.1.1 Units with both weapon hardpoints and `Projectile_Types` (S8 t05, 2026-11-02) [H unless tagged]
**Rule: the two weapon sources are independent, and the XML flags decide which one may fire in which state.**
Nothing in `WeaponBehaviorClass` reads the hardpoints: not the service (vf6 `0x54d090`), the attach (vf7 `0x54ab00`),
the initialise (vf9 `0x54c210`), the recharge reset (`0x54cef0`) or the fire gate (`0x54c910`). The hardpoints fire
through the object update (`0x3a76b0` → `Update_Weapon` `0x387400`), which does not read `Projectile_Types`.

**When the unit's own weapon exists:** the object has the WEAPON behaviour (id 0x0A, from `Behavior` or the mode list
`LandBehavior` / `SpaceBehavior`) **and** `Projectile_Types` is not empty (vf6 returns at once when the count at type
+0xc48 is < 1). Weapon hardpoints play no part. A type with `Projectile_Types` but no WEAPON behaviour never fires
that list (it can still be read by abilities and Lua, e.g. `Get_Current_Projectile_Type`).

**Own-weapon gate** (`0x54c910`, checked every service before the shot), in order:
1. the behaviour is enabled (`0x4c3700`), the global weapons switch `0x140a43a88` is on, obj +0x3a0 & 0x10 is clear;
2. **no deploy transition**: for a `Deploys` type with a locomotor, the locomotor's vf +0x130 (Walk `0x61df60`: state
   0x18 Deploying) and vf +0x128 (Walk `0x61dfd0`: state 0x1A Undeploying) are false (`0x39a400`, `0x39b2a0`);
3. `Is_Deployed_Or_Deploying` (`0x39a390`, S9_DEATH_CLONE_SHIELDS §1.2) → `Fire_Weapon_When_Deployed` (type +0xae),
   otherwise `Fire_Weapon_When_Undeployed` (+0xaf), must be set;
4. unit ability ROCKET_ATTACK (11) active (`0x39b950(obj, 11)`) → `Fire_Weapon_When_In_Rocket_Attack_Mode` (+0xb0),
   otherwise `Fire_Weapon_When_In_Normal_Attack_Mode` (+0xb1), must be set;
5. `0x397f60(obj, 1)`: each active unit-ability mode in slots 0 / 1 must allow it (`0x565970`): `WEAPONS_ENABLED_FLAG`, see "Gate details" below (2026-11-02);
6. unless type byte +0x8a is set, no fire in locomotor state 0x1C; a unit whose current locomotor answers vf +0x240
   does not fire in state 0x16 (Walk Stopped) ("Gate details" below, 2026-11-02);
7. unless a fire-at-position order is pending (TD +0x3c / +0x3d), the targeting state TD +0x18 must be 4 (the
   "attacking" state of slot 31; confirmed in "Gate details" below, 2026-11-02; who sets the order: "Fire-at-position
   order" below, 2026-11-02).
All four `Fire_Weapon_When_*` flags default to true (OBJECT_TYPES §4.6).

**Hardpoint gate** (`HardPointClass::Does_Weapon_State_Allow_Firing_At`, `0x384850`; the name is in its error
string), called by `Update_Weapon` after the cooldown with the primary target (+0x40):
1. a target that is dead, delete-pending or a death clone (obj +0x3a0 & 0x42, or bit 0x80) is refused;
2. weapon types 5–10 only; a destroyable hardpoint with health ≤ 0, or a disabled one (+0x6e), is refused;
3. a target matching `Fire_Category_Restrictions` (`0x540140`) is refused [M on the arguments];
4. for each of the owner's unit-ability slots 0 / 1 whose mode m (1…0x4c) is active: the hardpoint type byte
   **+0x384 + m** must be set (FoC tags: `Fire_When_In_Defend_Mode` +0x385, `Fire_When_In_Turbo_Mode` +0x38d,
   `Fire_When_In_Power_To_Weapons_Mode` +0x3a1), and `0x397f60(owner, 1)` must pass;
5. `Fire_When_Deployed` (+0x3d1) clear and the owner deployed or deploying → refused; `Fire_When_Undeployed`
   (+0x3d2) clear and the owner not → refused;
6. `Fire_When_In_Normal_Attack_Mode` (+0x3d4) clear and ROCKET_ATTACK not active → refused;
   `Fire_When_In_Rocket_Attack_Mode` (+0x3d3) clear and ROCKET_ATTACK active → refused.

The bytes +0x3d1…+0x3d4 default to true (§5.1). `Update_Weapon` itself also returns early in the deploy transitions
(`0x39a400` / `0x39b2a0`, as step 2 above). **When the gate refuses, `Update_Weapon` returns: no `Fire_At` and no
opportunity fire that frame.** With no primary target the gate runs with a null target (steps 4–6 only).

**Gate details (S8 t11 / t12, 2026-11-02)** [H unless tagged]. These resolve the [M] tags in the own-weapon steps
above.
- **Step 3 correction.** 0x39a390 is true when the locomotor pack's pending flag +0x2b3 is set, or when the
  locomotor's vf +0x120 is true. Walk's vf +0x120 is 0x61df40, state == **0x19 Deployed** only. So the name
  "Is_Deployed_Or_Deploying" is loose: Deploying (0x18) does not count. Step 2 has already refused 0x18 / 0x1A, so
  the gate result is unchanged. Every other locomotor (JetPack included) has vf +0x120 = 0x13b7d0, which returns
  false, so it counts as undeployed.
- **Deploy transition window (step 2).** It is the Walk locomotor state (pack +0x48):
  - 0x18 Deploying and 0x1A Undeploying;
  - only Walk implements vf +0x130 (0x61df60: == 0x18) and vf +0x128 (0x61dfd0: == 0x1A);
  - 0x39a400 / 0x39b2a0 also need the type's `Deploys` (+0xd1) and a locomotor slot.

  The state machine and its duration are in MOVEMENT §3.2 (2026-11-02 "Deploy states" note). The hardpoint
  `Update_Weapon` returns early on the same two tests.
- **Step 5** = 0x397f60(obj, 1):
  - it reads `Mod_Flag` index 1 = `WEAPONS_ENABLED_FLAG` (enum table 0x140a434a0: 1 WEAPONS_ENABLED_FLAG,
    2 CAN_BE_SQUASHED, 3 ALLOW_TARGET_DIMINISHING_FIREPOWER);
  - it ANDs that flag over the active modes of `Unit_Abilities_Data` entries 0 and 1 (0x374b50(type, i)), read
    through 0x565970: record +0xe0 + m when m < 13, else 1;
  - the record ctor sets the flag bytes +0xe0…+0xe3 to 1 (0x5655ad), so the default is "allowed";
  - vanilla FoC never sets `WEAPONS_ENABLED_FLAG`: the XML sets only ALLOW_TARGET_DIMINISHING_FIREPOWER, twice. So
    step 5 never refuses in vanilla.
- **Step 6.** With a locomotor data pack (obj +0xa8):
  - when `Can_Fighter_Fire_When_Idle` (type +0x8a) is clear: no fire in state **0x1C (fighter Idle)**;
  - in all cases: no fire in state **0x16** when the slot-1 locomotor's vf +0x240 is true. That vfunc is 0x17dc70
    (returns 1) for Flying, JetPack, LandBomber and Starship, and 0x13b7d0 (false) for the others (Walk included).

  So a walker standing still (0x16) does fire, and a jetpack unit at rest in state 0x16 does not. JetPack swaps
  back to Walk in its first 0x16 service (MOVEMENT §3.1.1), and WEAPON is suspended during the flight anyway.
- **Step 7.** TD = obj +0xf0.
  - If the unit has LOBBING_SUPERWEAPON (slot byte +0x369 ≠ −1) and TD +0x3d is clear → no fire.
  - Else, when neither TD +0x3d nor +0x3c is set, TD +0x18 must be **4** and the current target (0x54c190) must
    be non-null. Otherwise TD +0x28 := the current weapon's `Projectile_Fire_Pulse_Count` (+0x4f0) and no fire
    (the burst restarts).
  - TD +0x18 is the targeting state, written by slot 31 (0x568c80; the TeamTargeting override 0x62f550 copies it to
    the members). Entering 4 raises signal 0x1E. **4 = attacking**: the target is in range (slot 44) and not inside
    the min range (slot 45), set by `Service_Land` 0x634810 / `_Space` / 0x62eeb0. State 3 = in range but the turret
    or facing is not ready yet (slot 48 0x568230 is true while the facing is off the target direction by more than
half of 0x372560 = `Max_Rate_Of_Turn` (+0x38c) × the speed / difficulty factor; 0x20af60 compare; units and the full
    state table: "Targeting states, slots 47 / 48 and team copies (s78f, 2026-11-03)" below). State 2 = out of
    range. This is the fire-time range gate (LOB_SOLVER §8 item 22).
  - After a shot from a LOBBING_SUPERWEAPON object, TD +0x3c / +0x3d and +0x30…+0x38 are cleared.
- **ROCKET_ATTACK detection (step 4, hardpoint step 6).** 0x39b950(obj, 11, 1) reads the per-type slot
  obj +0x3d0 + 16·t. These slots are filled from **every** `Unit_Abilities_Data` entry (a later duplicate replaces an
  earlier one), so ROCKET_ATTACK is detected in any list position. Team containers use the all-members rule. Only
  the per-mode allow (own step 5, hardpoint step 4) is limited to entries 0 / 1.
- **Per-mode `Fire_When_In_<Mode>_Mode` defaults (hardpoint step 4).** The HardPoint type ctor 0x53e3b0 writes
  0x01010101 dwords over +0x384…+0x3d0, so **every mode byte defaults to 1** (allowed). It also writes 1 to +0x3d1…+0x3d4
  (`Fire_When_Deployed` / `_Undeployed` / `_In_Rocket_Attack_Mode` / `_In_Normal_Attack_Mode`) and to +0x3d5…+0x3d7
  (`Allow_Opportunity_Fire_When_Idle` / `_When_Targeting`, `Engine_Death_Hide_Engine_Particles`). A hardpoint
  refuses in a mode only when the XML sets that mode's tag to No.
- **First service after spawn (cooldown).** The pack ctor 0x380540 zeroes TD +0x24…+0x2f, so the last-service
  frame TD +0x2c starts at **0**. Only the WEAPON service writes it (0x54d090: elapsed = frame − TD +0x2c, then
  TD +0x2c := frame).
  - The attach vf9 0x54c210 sets the initial delay TD +0x24 = `Get_Int(0, int(FPS × recharge))` (one synced draw).
  - The first service then subtracts the **whole current frame number**, not 1 frame. When frame > delay
    (unsigned compare), the delay is zeroed.
  - So the random first-shot delay survives only for objects whose first service comes at a frame ≤ their drawn
    delay, i.e. units present at battle start. Units spawned later (reinforcements, builds) can fire on their
    first service.
  - [M] Whether `frame` is the mode's frame counter starting at 0 for the battle.
- **Lua `Get_Current_Projectile_Type`** (0x578470).
  - It reads obj +0x324 (the current weapon index; GameObject ctor 0x348a50 sets it to 0; unit ability 0x3c toggles
    it 0 ↔ 1 when the list has more than one entry) and indexes the type's `Projectile_Types` list (0x373670).
  - It does **not** check for a WEAPON behaviour. A type with `Projectile_Types` but no WEAPON still returns its
    first entry. An empty list or an out-of-range index returns nothing.
  - A team container (slot byte +0x348 ≠ −1) is first replaced by its first member (0x4ec820 on team +0x110: list
    +0x10, count +0x18). An empty team returns nothing.
- **In-game probe (s78f t01, 2026-11-05; `tools/probe_mod/s7s8_probes/out/20261105-045131`, `-051443`, `-055142`).**
  - *Deploy window* [H for what was seen]: an ordered `SPMAT_Walker` threw nothing in 0x18 (45 frames). Targeting
    state went 1 → 3 at the 0x19 frame and 4 eighteen frames later, with the first grenade on that same frame.
  - *Muzzle types in space* [H for what was seen]: `Underworld_Infantry_Droid_Shuttle`, `UM07_CloudCity_Refinery`,
    `UM07_Cloud_City_Flyover`, `Moldy_Crow`, `Moldy_Crow_Landing` and `Luke_Jedi_Xwing_Landing` fired.
    `Tyber_Zann_Boarding_Shuttle_Prologue` and `Underworld_Infantry_Droid_Shuttle_Landing` stayed silent, but their
    targeting state never reached 4 (0, or 0 / 2), so that is not a muzzle failure. The land control ran only against
    the Hostile-owned victims that nothing targets (relation item above), so it proves nothing. (s78g t04, §2.10:
    Hostile-owned victims are targeted. The land control failed for other reasons.)
  - *First shot after a late spawn*: one `MZ8_Pulse_Cannon_Tank` reached state 4 and fired 40 frames after spawning.
    Its second did not spawn, so a surviving random delay is not ruled out (one sample, needs at least four).
  - *First shot after a late spawn, measured* (s78g t05, 2026-11-05, `out/20261105-073814`) [M]. REINF (re-parsed
    t12 run and this run): four `MZ8_Pulse_Cannon_Tank`s spawned 150 frames apart, each ordered at once to attack a
    held tank at 200. First-shot delays after spawn were 18 / 29 / 41 / 52. REINF2: six spawned at irregular gaps
    (200 / 172 / 249 / 141 / 288): delays 9 / 11 / 29 / 41 / 52 / 61. So the delay grows by about **11.3 frames
    per unit spawned earlier** (least-squares slope; residual 5.9), whatever the gap. It is a per-unit stagger, not
    a random initial delay (a random one would spread up to 159). The REINF run's first shots also sit on a
    161-frame clock, but REINF2 does not, so that clock was a coincidence of the equal gaps. Explained by s78h t05
    (next item): the stagger is the hull turn from the spawn facing, not a count of units or services.
  - *What the "stagger" is* (s78h t05, 2026-11-05) [H]. It counts nothing. It is the **body turn from the
    `Spawn_Unit` facing to the target**, and it only looked per-unit because the probe placed each later spawn
    further along the line, so each one had to turn further.
    - **`Spawn_Unit` facing** (`LuaSpawnUnitCommandClass::Lua_Call` 0x6a1f90, tactical branch) [H]. The position
      comes from parameter 2 (0x4990b0; an object gives its position). The orientation is built by 0x20acd0(position,
      (0, 0, 0)), i.e. the angles **looking from the spawn point at the world origin**: yaw = atan2(0 − y, 0 − x) in
      degrees, mapped to [0, 360) (yaw 0 when x = y = 0); pitch = −atan2(0 − z, horizontal distance); roll 0. Then
      0x29e960(type, position, that orientation, player) creates the units. Each one gets it through 0x2b3be0
      parameter 4. A team container formed by 0x2a5a60 takes the first member's +0x84. An object given as the
      location does **not** lend its own facing. The galactic branch (mode vf +0xf0 false → 0x6a2520) is a
      different path.
    - **The turn.** The order's target is set 1–2 frames after the spawn, and the state becomes 3 (slot 48: off the
      target yaw by more than half a step). Each service then turns the hull by one step R = `Max_Rate_Of_Turn` ×
      `Object_Max_Speed_Multiplier_Land` (MZ8: 1.0 × 1.56 = 1.56° per frame; MOVEMENT §3.4). The state becomes 4,
      and the shot follows at once, when the remaining error is ≤ R / 2. The WEAPON initial delay plays no part,
      because it is zeroed on the first service, as the item above says.
    - **Check against both runs.** Victim at (3280, 4640); the attackers' x = 3480 (REINF2 unit 2 at 3387.0, 4730.7).
      Yaw to the origin is about 233°. Turn needed (frames in state 3 before state 4):
      - REINF: 26.0° (16), 43.4° (27), 62.8° (39), 80.2° (50).
      - REINF2: 12.9° (7), 14.1° (8), 43.4° (28), 62.8° (39), 80.2° (50), 93.3° (59).

      All ten fit angle / 1.56 within 1–2 frames. REINF2 units 4 / 5 stand where REINF units 3 / 4 stood and took the
      same 39 / 50 frames, although 3 / 4 units were alive before them instead of 2 / 3. A facing of 270° (−Y), or
      the port's +X, does not fit.
    - **Port check (2026-11-05).** The weapon side matches. `fire_unit_weapon` subtracts the frames since the last
      service, which start at 0, so the drawn initial delay is gone on a late spawn's first service. The state 3 / 4
      gate and the turn step also match: `needs_turn_to` uses tolerance R / 2, and `face_toward` makes one
      clamp(±R) step per service. `turn_step` = `turn_rate` × `Object_Max_Speed_Multiplier_Land` (1.56).
      **Mismatch:** the port's Lua `Spawn_Unit` (`engine/lua/src/bind_objects.cpp` `spawn()`) gives a position spawn
      facing +X, and an object-location spawn that object's facing. **Change:** in the tactical store, set
      `sp.facing` to the unit vector (−x, −y, 0) / |(−x, −y)| from the spawn position, with (1, 0, 0) when x = y = 0.
      This applies to an object location too, using its position. Keep the company members on that same facing.
      [port] The 0.3° pitch is dropped, because land units stay level. Not checked: `Create_Generic_Object`,
      `Reinforce_Unit` and the AI / story spawn paths, which use their own callers (answered by the next item).
  - *Spawn facing of the other Lua and reinforcement spawns* (s78i t02, 2026-11-05). The orientation is the
    3-float angle triple an object keeps at obj +0x84 (+0x8c = yaw in degrees, yaw 0 = +X, yaw = atan2(y, x) of
    the facing; +0x88 = pitch). The object constructor 0x388b60 copies it as given. Its "param 7 = 1" adds type
    +0xdfc to the **position z** only, never to the angles.
    - **`Create_Generic_Object`** (`LuaCreateGenericObjectClass` 0x6a1d20) [H]. 0x4990b0 reduces parameter 2 to
      a position; an object given there lends **only its position**. 0x29f810(type, owner, position,
      orientation (0, 0, 0), 1, 0) then creates **one** object (no company members, no container), so it faces
      **yaw 0 = +X**, level. There is **no mode check**: the same holds in galactic, land and space.
    - **`Spawn_From_Reinforcement_Pool`** (0x6aa200) [H].
      After it removes one entry of that type from the player's pool, it builds the orientation exactly as
      `Spawn_Unit` does: 0x20acd0(position, (0, 0, 0)) = **facing the world origin** (+X at x = y = 0), then
      0x29e960 creates the company with it. No mode check, no reinforcement drop, no transport.
    - **`Reinforce_Unit`** (`LuaReinforceCommandClass` 0x6a0400, tactical only) [H for the creator and the
      orientation source]. It does not use the Lua position as a facing target. It queues a real
      reinforcement:
      - land (mode 1): a free drop point near the position (0x69fc40, radius 1000), then 0x2b5fe0, the same
        creator the player's reinforcement drop uses;
      - space (mode 2): a reinforcement block (0x7235c0 / 0x722e80), which later calls 0x2bf2d0, the same
        creator the player's space reinforcement uses.
    - **Player reinforcement drop** (event handler 0x403cb0: land → 0x2b5fe0, space → 0x2bf2d0) [H]. Both
      creators read the orientation from a **per-player table** in the tactical mode object (mode +0x288,
      12 bytes per player, indexed by player +0x48) and pass that one triple to every object they create:
      - land 0x2b5fe0: the landing object (the type's +0x1e68 landing type when set, else the unit type), at the
        drop point raised by a height offset (z only);
      - space 0x2bf2d0: the unit, or each company member at its own spread position (0x2a9670).
      The land drop-zone check 0x4d6fd0 lays out the footprint with the same table yaw.
    - **Where the per-player table comes from** [H for the writes, M for which marker wins].
      - Skirmish (`Creating_Starting_Forces_For_Skirmish`, 0x2a4fc0): the orientation (+0x84) of the first
        `PLAYER_<n>_SPAWN_POINT_MARKER` matched for that player (the marker the starting forces use).
      - Battles from the campaign (0x2a1eb0): the defender gets the first "DEFENDING FORCES POSITION" marker's
        orientation. The attacker gets the chosen "ATTACKER ENTRY POSITION" marker's orientation, picked by its
        distance to the defending-forces markers.
      - A player with no such marker keeps the table's initial value, **(0, 0, 0) = yaw 0 = +X, level** [H, s78j t01].
        `GameModeClass` ctor 0x35a5e0 builds the `DynamicVectorClass<Vector3>` (mode +0x280, data +0x288, count
        +0x290) with one entry per player (count 0x140a16fb0) and zeroes every entry. The writers only overwrite an
        entry when a marker matched (TACTICAL_GAPS §4.2.2 item 1), so a skirmish player with no matching spawn marker,
        a campaign attacker with no start units or no entry marker, and every third player (neutral, hostile,
        pirates) drop at yaw 0.
    - **Units leaving a land landing object** (s78j t01, 2026-11-05; TransportLanding unload step 0x644330, offsets
      0x643130, A1_READINGS §35.2) [H unless tagged]. Let ψ = the landing object's orientation (obj +0x84, i.e. the
      per-player table entry it was created with) and O_j = the unload offset of step j (pack +0x90 = j, one per team or
      vehicle; already rotated by ψ's yaw in 0x643130). 0x20acd0(from, to) gives yaw = atan2(to.y − from.y,
      to.x − from.x) in [0, 360) (0 when the xy are equal) and pitch = −atan2(dz, horizontal distance).
      - **Infantry team (member has `Create_Team`).** The n members of step j are created at the landing's position
        with the orientation 0x20acd0((0, 0, 0), O_j): **facing outward from the landing centre along the team's
        offset**. Step **j = 0 is overridden** with the landing's own orientation ψ (roll / pitch / yaw copied from
        obj +0x84…+0x8c), whatever O_0 is. Every member of the step gets the same triple. Then (not instant) one group
        move 0x2a7b80 to landing position + O_j; its move block carries no final-facing value (block +0x40…+0x47 are 0;
        [M] whether the formation then keeps the travel heading or applies something else on arrival). The walk from
        the centre to O_j points along the created facing anyway, except for team 0.
      - **Vehicle (no `Create_Team`).** Created at the landing's position with 0x20acd0((0, 0, 0), O_j) (outward
        along its offset; **no** j = 0 override; a `Unique_Ground_Unit` substitute and its rider get the same triple).
        Then, not instant:
        - with a locomotor of the current mode and type byte +0xd4 set: coordinator move 0x4f8200 to landing position +
          O_j (snapped for the movement class +0x1688), move block +0x44 = **ψ's yaw** (landing +0x8c), forced-move
          byte +0x5c = 1 [M: that +0x44 is the arrival facing the formation turns to; it is the only facing written];
        - otherwise: placed at landing position + O_j at once (0x3abb80) and its orientation **set to ψ** (0x3a8710 with
          landing +0x84).
      - **Instant-unload mode** (0x35e4c0 +0x30 set, [M] which mode): no move, no placement; every unit stays at the
        landing's position with its created orientation.
      - So after the unload: teams face outward along their offset (team 0: the entry facing), vehicles end on the entry
        facing ψ (placed: exactly; moved: [M] via the block facing).
    - **Summary for the port:**

      | Spawn | Facing |
      |---|---|
      | `Spawn_Unit` (tactical) | toward the world origin (item above) |
      | `Spawn_From_Reinforcement_Pool` | toward the world origin |
      | `Create_Generic_Object` | +X (yaw 0), any mode, even when given an object |
      | `Reinforce_Unit`, player reinforcement drop | the landing object and a drop without one: the owner's start-marker facing (per-player table; yaw 0 when no marker) |
      | units leaving the landing object | team step j: yaw of O_j (j = 0: the landing's facing); vehicle: created at yaw of O_j, ends on the landing's facing |

**Targeting states, slots 47 / 48 and team copies (s78f, 2026-11-03)** [H unless tagged]. TD = obj +0xf0; the TD
pack is shared by TARGETING, WEAPON (TD +0x80 = the WeaponBehavior, stored by its attach 0x54ceb0) and TURRET
(TD +0x88 = the TurretBehavior, stored by its attach 0x632120).

*States of TD +0x18* (written only through slot 31 and the team copy of it, below; the writers found are 0x565f90,
0x569160, the land / space / team services and 0x62f550):

| state | written by | meaning |
|---|---|---|
| 0 | target clear 0x565f90 | no target |
| 1 | `Set_Target` 0x569160 | target just set, not yet evaluated |
| 2 | the services | target out of range (slot 44 false or slot 45 true), or "hold fire" (below), or a moving unit that may not fire on the move |
| 3 | the services | in range, but the turret has not reached its aim, or the body must still turn to face the target |
| 4 | the services | attacking: the own-weapon gate step 7 passes |

*Land unit service* (`Service_Land` 0x634810), after the target checks, when slot 44 is true and slot 45 false:
1. **TURRET present (TD +0x88):** nothing happens while the unit is in a Walk deploy transition (0x18 / 0x1A), or
   while it is an `Auto_Deploys` (+0xd0) `Deploys` (+0xd1) unit that is not deployed (0x39a390) and has
   `Fire_Weapon_When_Undeployed` (+0xaf) clear: the state is left as it was. Otherwise:
   - the turret can aim at the target (0x631cc0) → aim it (0x632050): angles reached → **4**, else **3**;
   - it cannot aim (outside `Turret_Rotate_Extent` / `_Elevate_Extent`, or no lob arc) → if the unit is deployed,
     or the target is not the formation's attack target (0x41a2e0), or the target type's
     `Should_Cause_Limited_Turrets_To_Turn` (+0x80) is clear: the target is dropped (slot 27) and the service ends;
     else the body turns: unless the unit is moving, locomotor vf +0x218 gets the vector to the target (a team
     target: its member, 0x566aa0) (a turn-in-place request: Walk 0x61e4b0 pushes a one-waypoint path at the
     unit's own position facing that vector, kind 5, +0x6d set; the locomotor turns in its later frames; s78f t08
     [H]); state **3**.
2. **No TURRET, WEAPON present (TD +0x80):**
   - **moving** (0x39ade0 = locomotor vf +0x1c0(0)): state **4** when the model has **no** Attack (type 5) clip
     (0x397ae0(obj, 5) = the clip count), else **2**. So infantry with attack animations do not fire while they
     move; vehicles without them do.
   - **not moving:** t = slot 48 (needs to turn). An auto-deploy check (0x568e60) may request the deploy
     (0x391260). Then:
     - t true and (`Deploys` clear, or `Must_Face_Target_To_Attack_When_Deployed` (+0xa6), or
       `Fire_Weapon_When_Undeployed` (+0xaf)): slot 47(1), state **3**; then the locomotor's vf +0x2e8(target) (turn
       request: an **immediate** yaw step of at most the turn rate, s78f t08 [H]) and slot 48 again: still off →
       stays 3; now facing → **4**;
     - otherwise: when `Deploys` is clear the facing is **snapped** to the target (0x394440: yaw and pitch from
       obj +0x78 to the target position (a team target: its member); pitch 0 when
       `Targeting_Land_Model_Stay_Horiz_Flat` (+0xad); with the flag clear, locomotor vf +0x2f8 is then called:
       the terrain conform 0x42d6a0, which re-derives pitch / roll from the ground and keeps the yaw, s78f t08
       [H]); state **4**. A deployer that need not face (no +0xa6, no +0xaf) gets 4 without the snap.
3. **Neither TURRET nor WEAPON:** state **4**.

After that, in every case: state 4 → slot 47(1). Out of range (slot 44 false or slot 45 true): the turret aim is
released (0x6319d0(turret, 1): "aim requested" TD +0xf6 := 0); with a target, a unit that is neither deployed nor
deploying, or whose `Max_Dist_Walk_While_Deployed` (+0x23b0) > 0, gets state **2**; a deployed / deploying one drops
the target. **Hold fire:** last, state 4 with target == the pending-ability target (TD +0x148) and the target type's
`Should_Attacker_Hold_Fire_For_Special_Ability` (+0x9e) set → state **2** (land, space and team services).

*Space unit service* (0x633ae0): in range → no TURRET: **4**; TURRET: can aim → aimed **4** / not yet **3**; cannot aim
→ drop the target. Slot 48 is not used in space; there is no facing state.

*Slot 48* (0x568230, "needs to turn"; read-only, no state of its own). True when |wrap(yaw(obj +0x78 → target
+0x78) − obj yaw (+0x8c))| > tol, tol = 0x372560(type) / 2 = `Max_Rate_Of_Turn` (+0x38c, degrees **per frame**
(per turn call); no frame-time factor anywhere, s78f t08 [H]) × the
mode multiplier (land `Object_Max_Speed_Multiplier_Land` 1.56; factor 1 when type flag (+0x790) byte 1 bit 0) / 2,
**except tol ≥ 90 → 0.01°** (0x20af60). Yaw from 0x20acd0 (degrees, [0, 360)); the difference is wrapped to
(−180, 180]. Target redirect: a team-container target with a shooter whose slot byte +0x33b ≠ −1 is replaced by
the pending-ability target when that is one of its members, else by its member nearest the shooter (0x404fc0).
**Readers:** only `Service_Land` (own-weapon branch, twice) and the TeamTargeting land service 0x62eeb0 (twice,
through its override). No other caller (WEAPON, the gate, Lua and space never read it).

*Slot 31* (0x568c80, set state s):
- s = 4: the first time (current ≠ 4) signal **0x1E** is raised on the object; for a member of a team with a
  member list, only the team's first member (0x4ec820) raises it, on the container. (Its only listener is the
  AI task force: Lua `Target_In_Range`, s78f t08 below.) Then the pending-ability pack
  (obj +0x100) is told the target (0x37c9a0).
- s = 4 is **not written** (the old state stays) when the pending-ability target and the current target (each
  replaced by its container when it has one) are the same object and a pending ability is set (TD +0x150 ≠ 0), or
  when the type has `Is_Stationary_When_Attacking` (+0x26) and the locomotor's vf +0x1a8 ("moving") is true.
- Any other s is written as is.

*Slot 47* (0x569540, attack-idle pose; called with 1 at state 4 and on the own-weapon "needs to turn" path):
- an `Auto_Deploys` + `Deploys` unit without `Fire_Weapon_When_Undeployed`: when TD +0xf4 (the ordered-target flag
  written by slot 32 from `Set_Target`) is set and the current clip is not 25 (Deploy), request the deploy
  (0x391260);
- any other unit: when (argument 0 or `Locomotor_Has_Animation_Priority` (+0x37) clear), the locomotor is not moving
  (vf +0x1a8), the current clip is not a flinch (79–86, 0x27b590), not 5 (Attack) and not 6 (Attack Idle), and the
  animation gate 0x397040 is false: play clip 6 Attack Idle, index 0, looping, **blend = one synced
  `Get_Float(0.2, 0.75)`** (0x1401ffbb0 on 0x140a13e24), speed 1.0. So entering the pose costs one synced draw;
  while clip 5 or 6 plays, nothing is drawn.
  - **Order of the tests (s78j t02, re-read 2026-11-05)** [H]. 0x569540 and the per-member 0x569400 have the same body:
    the current clip is read first (0x3956a0: obj +0x2a0 → +0xc4 when +0xb8 is set, else −1). The draw and the play
    sit **inside one conjunction**, evaluated left to right with short-circuit: (arg 0 or animation priority clear) →
    not moving → not a flinch (0x27b590: 79–86) → **clip − 5 > 1 (unsigned), i.e. not 5 and not 6** → 0x397040 (obj
    +0x2a0 → byte +0x108) false → `Get_Float(0.2, 0.75)` → 0x3a92f0(obj, 6, 0, loop 1, blend, 1.0). There is no else
    branch for the animation case. So while the current clip is ATTACK (5) or ATTACKIDLE (6) retail **neither draws
    nor restarts the clip**; the same holds when 0x397040 is true (no draw). A model without clip 6 stays on its old
    clip and so draws every service (the t11 result below).

*Team targeting copies* (`TeamTargetingBehaviorClass`, vtable 0x8afd90, on ground team containers only: its service
0x62ee00 runs 0x62eeb0 when the container's locomotor kind is 1 and (obj +0x3a1 & 2 clear or
`Targeting_Allowed_When_Burning` (+0x83))). Member order = the team list order (0x405300 count, 0x4052d0 index).
Overrides:
- **slot 31** (0x62f550): the base on the container, then the **same state written raw** to every member's
  TD +0x18 (no signal, no hold rule). So members can be in state 4 while the base kept the container's old state.
- **slot 32** (0x62f4c0): the base (TD +0xf4 := flag, when a target exists or the flag is 0), then the flag written
  raw to every member's TD +0xf4.
- **slot 47** (0x62f5e0): the container itself does nothing; 0x569400 (the slot-47 body above) runs on **each
  member** in order, so each member entering the pose draws its own `Get_Float(0.2, 0.75)`.
- **slot 48** (0x62ed60): true when **any** member needs to turn (0x568120 per member, same rule).
- **slot 42** (0x62ecc0, projectile for the range slots): the first member whose `Projectile_Types` has the index.
- **Not copied:** the target TD +8 and the hardpoint TD +0x10. Land team members (`Squad_*` types:
  `LAND_TEAM_INFANTRY_LOCOMOTOR`, WEAPON, no TARGETING) keep TD +8 null; their WEAPON service and 0x54c190 fall back
  to the container's TD +8 when their own is null and the container's locomotor answers vf +0x230 (LandTeamContainer
  only) [M: no other writer of a land member's TD +8 was looked for].
- *Team service differences* (0x62eeb0, in range): a **moving** container → state 4 only while SPREAD_OUT (7) is
  active (0x39b950(container, 7, 1)), else 2. Not moving: no member needs to turn → every member's facing is
  snapped to the target (0x394440; a team target: its member) and state 4; else slot 47(0) on the members, state 3,
  the container's locomotor vf +0x2e8(target), and 4 if no member needs to turn any more. No TURRET branch.
- Space squadrons have no team targeting service; their shared target is FIGHTER_FLYING §4.10 B (team data +0x30,
  `Set_Target` on every member).
- *When the container is serviced, and a member's death in the same frame* (s78h t06, 2026-11-05).
  - **Its own entry** [H]. TeamTargeting is a behaviour of the container object. It runs from the container's own
    object service (0x3a6b80, behaviours in attach order), at the container's own place in the store's serviced
    list (OBJECT_RUNTIME §5.3). It is never called through a member or a leader. 0x62ee00 reads only the container
    (param_1[5]): its locomotor kind, its +0x3a1 burning bit and its TD. 0x62eeb0 walks the members only to push
    state, facing and pose.
  - **A member's death does not touch it** [H for one service per frame from the service structure; M that no
    death hook writes the container's TD, because the death path (OBJECT_RUNTIME §2.2) was not re-read for one].
    Nothing on the death path re-services the container or changes its
    TD (target TD +8, state TD +0x18, scan timer TD +0x20). So a container alive at its slot gets exactly **one**
    TeamTargeting service that frame, whichever members died before or after its slot. There is no re-service and
    no hand-over. A container that dies earlier in the walk keeps its list entry with zero serviced behaviours until
    the flush (OBJECT_RUNTIME §5.3), so it gets no service.
  - **Its place relative to its members** [H creation order, M list direction]. 0x29e960 (the creator behind Lua
    `Spawn_Unit`; other spawn paths that reach 0x2a5a60 were not traced) first creates every company unit (type list +0x958 / +0x978 → 0x2b3be0). Only then, when
    the first unit's type has +0x67 set, does 0x2a5a60 build the team container (0x29f810 at the members' mean
    position, then members added by 0x404190). The container therefore registers **after** its members. The serviced
    list is walked newest first, so the container is serviced **before all of its members** each frame. Their
    WEAPON services then read the state it pushed that same frame.
  - **Port (2026-11-05).** The port has no container unit. The team's lowest-index member (`team_leader`, set in
    `engine/apps/sim/src/tactical_battle.cpp`) stands in for it, and phase D walks `units_` from the highest index
    down. That gives two differences from the above:
    1. The head service runs **after** the other members' slots, so their `update_weapons` read the previous
       tick's team state (one tick late).
    2. The s78g t06 hand-over goes to the lowest-id live member. That member always has a higher index than the dead
       leader, so its slot was passed earlier in the walk. A leader killed inside phase D, from a slot between the
       heir's and its own (or by its own burning / infection service), leaves the team with no TeamTargeting that
       tick. Retail never skips one.

    **Recommended change** (in `combat_system.cpp` phase D): run each land team's TeamTargeting, meaning the
    head's `decide` (on `cands_[head index]`) followed by `team_targeting_service`, **once per tick at the team's
    container slot**. That slot is just before the team's highest-index member, reached first in the descending
    walk. Run it even when that member is already dead, as long as the team still has a live head. Keep a per-team
    "serviced this tick" flag, and drop the head's own-slot team call. A hand-over after the slot then needs no
    re-service, and one before it is picked up at the slot. `hand_over_team` keeps the record copy, and its in-tick
    re-gather is still needed only for the copied scan flags. This moves team draws (scan, slot-47 blends) earlier
    in the tick, so the land team goldens and CRCs change. A smaller fix that closes only gap 2: at the dead former
    leader's own slot, if its team has not been serviced this tick, service the current head there.

  - **Port (s78i t03, 2026-11-05).** A land team keeps TeamTargeting while its head lives, whatever its member count:
    a stand-in `team_leader` left with no live member, or a team of one, is still serviced at its container slot by
    `decide` + `team_targeting_service`, never by its own Service_Land. This matches retail, where the container's
    TeamTargeting runs while the container lives. `CombatSystem::land_team_lead` returns the head for any team unit,
    the head included. A team whose head dies with no member left has no heir and ends.

*For the implementer:* keep one state per object (0–4) and gate the own weapon on 4. Land: turret units use "turret
aimed"; weapon units use "facing within half a frame of turn" (snap when within, turn request when not); moving
units fire only without Attack clips. Team containers compute the state and push it raw to the members; members use
the container's target. Draw `Get_Float(0.2, 0.75)` each time a unit (or each team member) enters Attack Idle
through slot 47.

**s78f t08 answers (2026-11-04)** [H unless tagged]. R below = the turn rate 0x372560(type) =
`Max_Rate_Of_Turn` (+0x38c) × M × D, where M = the mode multiplier (`Object_Max_Speed_Multiplier_Land` 1.56 in land,
galactic 5.0, space 1.2, 1.0 for any other mode) and D = the debug game-speed factor 0x140a28568 (1.0); M and D are
both skipped (factor 1) when type flag (+0x790) byte 1 bit 0 is set. The vanilla default `Max_Rate_Of_Turn` 2.3
gives R = 3.588° and a slot-48 tolerance of 1.794° in land.

1. **Turn request vf +0x2e8 is immediate.** Walk (vtable 0x8ad798) and LandTeamInfantry (0x8af8d0) share 0x42d7b0:
   with an owner and a target, the target is replaced by its member (0x566aa0) when it is a team container and the
   shooter's slot byte +0x33b ≠ −1; the facing toward it is computed (0x20acd0, obj +0x78 → target +0x78) and
   handed to vf +0x2f0, which turns **in the same call**:
   - Walk 0x620710: e = wrap(target yaw − obj yaw +0x8c) to (−180, 180]; yaw += clamp(e, −R, +R) (no step when
     e = 0); roll and pitch are not stepped (the target pitch is ignored). The new orientation is written at once to
     both copies, obj +0x84… and +0x90… (0x3a8710; skipped while obj +0x3a0 bit 0x10 is set), then vf +0x2f8 = the
     terrain conform 0x42d6a0 re-derives pitch / roll from the ground with the new yaw and sets the height.
   - LandTeamInfantry 0x62d400: the same, with R × GC `Fleeing_Infantry_Speed_Bonus` (+0x150c) while the member's
     (or its team's) current path is kind 7 (0x39a5b0).
   - LandTeamContainer (0x8a3630) vf +0x2e8 = 0x5adec0: resolves a team target to its member once, then calls each
     member's own locomotor vf +0x2e8 in team-list order (0x405300 / 0x4052d0), so every member steps by its own R.

   So the second slot-48 check in the same service sees the stepped yaw. A unit e degrees off (e > R/2) reaches
   state 4 in the same service when e ≤ 1.5 R; in general after ceil((e − R/2) / R) services, one step per service.
   The leftover error (≤ R/2) is removed by the snap 0x394440 on the next service (slot 48 false → snap branch).
   No goal is stored for later frames [M: an idle Walk vf6 that re-faces the unit on its own was not looked for].
   The other turn call, vf +0x218 in the TURRET "limited turret" path, differs: it is a turn-in-place path that the
   locomotor executes over later frames.
2. **Unit of the tolerance.** Degrees per call of the turn function, i.e. per targeting service; the targeting
   service runs on every behaviour service call (§2.1), so per sim frame (30 fps). There is no division by the
   frame rate or multiplication by a frame time in 0x372560, 0x620710, 0x62d400 or 0x20af60. Exact rule: needs to
   turn ⇔ |wrap(yaw(obj → target) − obj +0x8c)| > tol, tol = R / 2, except tol := 0.01° when R / 2 ≥ 90.
3. **No locomotor (structures with WEAPON and no TURRET).** Slot 48 does not use the locomotor (only the owner's
   +0x78, +0x8c and type), so it works as for any unit. In `Service_Land`, 0x39ade0 ("moving") is false when the
   locomotor slot byte obj +0x333 is −1, so the not-moving branch runs; `Deploys` is clear, so the auto-deploy check
   returns false (and 0x391260 would do nothing). Then:
   - target within R/2 of the current facing → the snap 0x394440 turns the structure to face the target exactly
     (yaw and pitch; the conform is skipped without a locomotor) and state **4**;
   - otherwise slot 47(1) (the Attack Idle pose, with its synced draw) and state **3**; the turn call is skipped
     (vf +0x10(obj, 1) returns null) and the service ends in state 3. Nothing turns the structure, so it stays in
     **3 as long as the target stays outside the ±R/2 cone**, and fires once a target is inside it.

   FoC example (scan of the FoC XML): `Empire_Offensive_Sensor_Node` (GroundStructure; LandBehavior TARGETING,
   WEAPON, no TURRET; no `Max_Rate_Of_Turn`, so 2.3 and a cone of ±1.794°) [M: the in-game effect was not observed].
4. **Signal 0x1E listeners.** The image has one subscription to 0x1E (every 0x220e90 / 0x2406c0 call site was
   checked, including the register-valued ones: 0x6afe00 is called with 0x12, 0x72bfe0 uses 5 / 8, 0x2215b0 only
   re-subscribes existing listeners after a load): `TaskForceClass::Add_Force` 0x5f9a90 subscribes the AI task force
   to signals 0x1A, 0x10, 0x1E, 0x1F, 0x20, 0x21, 0x2B, 0x2C, 0x29 and 0x2D of each unit it takes (unsubscribed on
   0x10 / 0x1A). Its handler 0x5fe5d0, case 0x1E: target = 0x397a40(unit) (the formation's attack target 0x41a2e0,
   else the TD +8 target); none → nothing; else it raises task-force plan event **12 `Target_In_Range`** (0x6006b0,
   not queued, so `<TF>_Target_In_Range` / `Default_Target_In_Range` runs synchronously) with (unit, target).
   Dispatch (0x240940) calls only the listeners of that id; there is no catch-all listener. So no animation, sound or
   other engine code reacts to 0x1E: it matters only for units in an AI task force, through the AI's Lua micro
   (AI_TACTICAL §8). For a team member it is raised on the container (first member only), so the event names the
   container.
5. **Auto-deploy check 0x568e60** (this = TARGETING, owner = obj): false when TD is null, `Deploys` (+0xd1) or
   `Auto_Deploys` (+0xd0) is clear, `Fire_Weapon_When_Undeployed` (+0xaf) is set, or the unit is deployed (0x39a390)
   or deploying (0x39a400 = locomotor vf +0x130, Walk 0x61df60). Otherwise **true** when any of:
   - the target is ordered (slot 33 0x566b40, TD +0xf4);
   - the owner player (player list 0x140a16fd0, index obj +0x58) has an AI (player +0x360 ≠ 0) and is not human
     (player +0x62 = 0): an AI's auto-deployers deploy for any target in range;
   - the current target (TD +8) is the formation's attack target (0x41a2e0 on locomotor pack +0x1f0; no formation →
     false) and either `Max_Dist_Walk_While_Deployed` (+0x23b0) > 0, or the formation has a current block
     (0x41afb0 count > 0, 0x41a580) whose flags have attack-move (0x01) or guard (0x02).

   Else false: a human player's auto-acquired target outside such an order does not deploy the unit. `Service_Land`
   then calls 0x391260 = the deploy request: needs `Deploys`, a locomotor slot and locomotor pack +0x2b3 clear; then
   locomotor vf +0x118(1) (Walk 0x61d300, which starts the Deploy transition unless already in 0x18 / 0x19). The
   check runs in the TURRET branch only while not moving, and in the WEAPON branch after slot 48. **Slot 47
   confirmed:** with `Auto_Deploys` + `Deploys` set and `Fire_Weapon_When_Undeployed` clear, 0x569540 / 0x569400 call
   0x391260 when TD +0xf4 (ordered) is set and the current clip (0x3956a0) is not 25 (Deploy); no pose is played.
   0x569540 (not the per-member 0x569400) first returns when the behaviour's counter +0x3c > 0 (0x4c36f0)
   [M: the counter's name; probably the behaviour-disable count].
6. **TURRET units without `Deploys`: no shortcut.** The deploy tests in the TURRET branch only decide whether the aim
   runs; the helpers read deploy state only to choose the deployed extents / rest angles, which 0x39a390 gives only
   to `Deploys` units. 0x631cc0(turret, target) = can aim (0x631a00 on the aim point 0x398440 = target position +
   type z +0x4dc): false with no TD, no turret bone (TD +0x40 = −1; set at attach 0x632120 from `Turret_Bone_Name`)
   or no type; false when a lobbed (category 3) current projectile has no lob solution from the weapon's next fire
   bone; false when |yaw relative to the body (obj +0x90…)| > `Turret_Rotate_Extent_Degrees` (+0x5a0) or, unless
   `Turret_XY_Only` (+0xb3), |pitch| > `Turret_Elevate_Extent_Degrees` (+0x5a4); else true. 0x632050(turret, target)
   = aim: false with no TD or no turret bone; else aim point → TD +0x60, "aim requested" TD +0xf6 := 1, one slew step
   0x631210 (target angles TD +0x54 := (0, pitch, yaw); current TD +0x48 moved by at most `Turret_Rotate_Speed`
   (+0x5a8) degrees per call per axis, no overshoot); a 0 from the step (out of extent, no lob solution) →
   +0xf6 := 0, false; else true **only when current = target within 0.0001° per axis** after that step
   (LOB_SOLVER §3.6). So "aimed" needs the turret to reach the angles; that can happen in the first service when the
   remaining error ≤ `Turret_Rotate_Speed`. The turret's own service 0x632270 does not slew while +0xf6 = 1, so there
   is exactly one slew step per targeting service. A TURRET unit whose model lacks the turret bone never aims: its
   target is dropped (or the limited-turret body turn applies) every service.
7. **Pending ability (TD +0x148 target, +0x150 ability name CRC, +0x154 type).** Only slot 38 (0x5688f0) sets them:
   when no target is pending or the new one is non-null, it stores (target, CRC, type), TD +0x10 := the hardpoint,
   and subscribes to the target's signals 0x28 and 1; when one is pending and null is passed, it raises 0x29 on the
   owner and clears the target (CRC / type take the passed values). Callers:
   - `Order_Attack` 0x38d730 with an ability CRC ≠ 0: command 40 / 57 `TacticalSpecialAbility` and Lua
     `Activate_Ability` with a target (ABILITIES_HARDPOINTS §2.9) → (target, hardpoint, CRC, type);
   - `Order_Attack` without a CRC (auto acquisition and attack orders) first runs the autofire driver 0x37d280 (owner
     pack obj +0x100): when it picks an ability (one synced `Get_Int` when several qualify) it stores
     (target, 0, CRC, −1); when it picks none and the order is ordered (player / script), slot 38(0, 0, 0, −1) clears
     the pending ability.

   Cleared to (null, 0, −1) by: 0x565de0 when the ability fires (first step of every targeting service); slot 27
   0x565f90 with its flag set (`Service_Land` passes "target == pending target" when it drops a target); 0x5661f0
   (vf +0x80) when the pending target is the released object (raises 0x29). So TD +0x148 is non-null exactly while a
   targeted ability (ordered, or chosen by the autofire driver) waits for range, and +0x150 is then non-zero.
   Effect of the slot-31 rule: while the pending target (or its team) is the current target (or its team), state 4
   is never written and the old state stays. A unit that was not yet attacking holds fire (2 / 3) until the ability
   fires; a unit already in 4 keeps firing, unless the target type has `Should_Attacker_Hold_Fire_For_Special_Ability`
   (+0x9e), which the end of the land / space / team services turns into **2**.

*For the implementer:*
- Make the facing turn immediate: one clamp step of R per targeting service, then re-test slot 48 in the same
  service (state 4 at once when e ≤ 1.5 R). Teams: one step per member, then re-test "any member off".
- R is degrees per frame: `Max_Rate_Of_Turn` × mode multiplier (× 1.0), no frame-time scaling; tol = R / 2, 0.01°
  when R / 2 ≥ 90.
- Units without a locomotor never turn: state 3 until the target is within ±R/2, then snap and 4.
- Send "entered state 4" only to AI task forces (`Target_In_Range(unit, target)`, synchronous); nothing else listens.
- Auto-deploy when: ordered target, or AI player, or formation attack target with walk-while-deployed or an
  attack-move / guard block.
- A turret is aimed only when the slewed angles equal the target angles; no deploy-flag shortcut; no turret bone
  means never.
- The pending ability comes only from slot 38 (`Order_Attack` with a CRC, or the autofire driver); while it names the
  current target, the state cannot rise to 4.
- Slot 47 on a model without an Attack Idle clip (s78f t11, in-game probe 2026-11-05) [M]: the current clip **stays
  unchanged**, so every slot-47 service in state 3 / 4 draws `Get_Float(0.2, 0.75)` again. Setup: six AI-owned
  `Pod_Walker`s (`EV_PODWALKER`, no `ATTACKIDLE` clip, no TURRET) with the locomotor suspended, ordered to attack
  held Empire T4Bs.
  - Clip: through hundreds of frames in state 3 and 4, the walkers' current clip stays 0 (Idle, index 0) and its
    frame keeps counting up (`out/20261105-055142`, IDLE47). Clip 6 is never current, so the "not 5 and not 6" gate
    passes on every service.
  - Draws: when the six walkers (and their held victims) despawn, the steady floor of synced draws per frame falls
    by exactly 6. That is 46 / 48 → 40 / 42 on a quiet background (`out/20261105-060346`, IDLE47T, F 3750), and
    448 → 442 in the earlier run (F 2251): one draw per walker per frame.
  - [M] because the despawn also removed the victims, and the planned on/off toggle failed: `Stop()` did not clear
    the walkers' targeting, which stayed at 4.
  - Control: the `Rebel_Infiltrator` phase never took a target (state 0), so there is no with-clip comparison yet.
  - With-clip control and victim toggle (s78f t12, 2026-11-05, `out/20261105-062322`, IDLE47V) [M]. Six attackers
    held in place; each 300-frame cycle spawns six held victims and orders the attacks ("on"), and despawns the
    victims 150 frames later ("off"); five cycles per phase. Median step of the synced draws per frame across the
    edges: `Pod_Walker` (no ATTACKIDLE) on **+7** / off **−6.5**; `Rebel_Infiltrator` (has ATTACKIDLE) on +5 / off
    −1; victims only, no attackers: on −2 / off +89 (bursts of about 100 steps in the background make single edges
    noisy). At on + 60 the walkers were in state 4 on clip 0; the infiltrators were mostly in state 0 (one in state 4
    on clip 6). So the about-6 step comes with six no-clip walkers in state 4, as a per-service slot-47 redraw
    predicts, and the victims alone do not produce it. The with-clip side is still not clean, because the
    infiltrators mostly did not take their targets.
  - With-clip control again (s78g t03, 2026-11-05) [M: not separated]. The with-clip attacker was
    `Dark_Trooper_PhaseII`; clip ids 5 / 6 are ATTACK / ATTACKIDLE. The troopers took their targets (state 4,
    2730 of 3750 attacker-frames in ATTACK, 1020 in ATTACKIDLE). The Pod_Walkers stayed on clip 0. The background is
    about 440 synced draws per frame and drifts ±100 over 150 frames, which swamps a 6–12 draw effect.
    - IDLE47W (`out/20261105-073814`, 6 attackers): per-cycle on-minus-off was P +1.9 ± 31.8, D +13.5 ± 25.6,
      V −48.6 ± 39.9.
    - IDLE47X (`out/20261105-092438`, 12 attackers, 15 cycles per phase, drift-cancelled): P−V −42.3 ± 24.4,
      D−V +27.7 ± 17.4, P−D −70.0 ± 25.4.
    Neither run separates the phases at 3 sigma, so whether a model with an ATTACKIDLE clip skips the per-service
    draw is still open. The draw-count method needs a quiet background (no AI army) or a per-object read of the
    slot-47 call to settle it.
    - IDLE47Q (s78h t02, `out/20261105-104750`, Claude drove the game): the same three phases after a Lua sweep that
      despawns both sides' mobile units, every 300 frames. The background fell from 210 to **4.5 draws per frame**.
      Per on/off step (12 attackers), the medians were **P (Pod_Walker, no clip, clip 0 throughout) +13 / −13**,
      **D (Dark_Trooper_PhaseII, in ATTACK 5 or ATTACKIDLE 6 throughout) +11.5 / −12**, and V (victims only) 0 / +0.5.
      The per-cycle means were P −1.8 ± 6.5 and D +17.5 ± 3.5. P is pulled down by two periodic −86 steps, which do
      not come from the attackers. Kit verdict `WITH_CLIP_ALSO_DRAWS`.
      [M] Both models add about one synced draw per attacker per frame, so the with-clip model does not go silent.
      That is one draw per service in state 4. It does not fit the port's gate, which skips the draw while the current
      clip is ATTACK / ATTACKIDLE (`tactical_battle.cpp` `attack_idle`).
      This is not settled. The attackers fire at the invulnerable victims (`Attack_Target`), and the Dark Troopers'
      shots may draw too, so part of D's draws may be fire, not slot 47. Leave the port unchanged until the next run.
      Next run: the same setup with the attackers' fire blocked but targeting kept (follow-up group s78i). If D
      still adds ~1 draw per attacker per frame there, drop the clip 5 / 6 condition from the draw.
    - IDLE47N (s78i t01, `out/20261105-125642`, Claude drove the game): IDLE47Q's quiet background (557 → 3.2 draws
      per frame after the sweep), 12 attackers, 15 cycles. Fire blocked with Lua `Prevent_All_Fire(true)`.
      - Set at placement, before any attack order (Pod_Walker phase P, Dark_Trooper phase D): the attackers **never
        target** (state 0, clip 0 Idle throughout), so this way of blocking fire also blocks targeting.
      - Set 6 frames after each cycle's `Attack_Target` (phase E, Dark_Trooper_PhaseII): state 4 for 93% of the
        attacker-frames, clip ATTACKIDLE 82% / ATTACK 3%, **0 shots** from on + 10 to off + 5 (115 shots in the first
        10 frames, before the flag). On / off steps were +8…+13 / −7…−13 (13 of 15 edges each; two ±90 steps are
        background bursts): median **+10.5 / −10.5**, V subtracted **0.85 draws per attacker per frame**. Firing
        reference F: 236 shots, median +23 / −13.5.
      - The kit printed `UNCLEAR` only because the two background bursts widen the per-cycle mean
        (−0.53 ± 1.22); the step medians are clean.
      [M] A Dark Trooper in ATTACK / ATTACKIDLE that does not fire still draws about once per frame, so the slot-47
      draw does not skip clips 5 / 6. **Port (2026-11-05):** `tactical_battle.cpp` `attack_idle` spends the draw on
      every service (not moving, no animation priority, not in a flinch); it only skips the clip restart while the
      current clip is 5 / 6. Whether retail also restarts the clip then is not measured.
      - **s78j t02 (RE, 2026-11-05): IDLE47N phase E did not measure slot 47** [H]. Lua `Prevent_All_Fire(true)`
        (0x57ee20) suspends behaviours **9 TARGETING and 10 WEAPON** (vf +0xc0, suspend count +0x3c + 1) on the object
        (and on each member of its behaviour-0x16 list when it has one), plus 0x392570 on its attached hardpoints. The
        object service runs a behaviour only while that count is 0 (MOVEMENT §3.3), and 0x569540 also returns when it
        is > 0 (0x4c36f0). So from the flag on, the Dark Troopers' targeting service, and slot 47 with it, did not run:
        the "state 4 / clip ATTACKIDLE" the kit read was the value left from before the flag. The ~0.85 synced draws per
        attacker per frame come from somewhere else [L: source not found; not targeting, not WEAPON]. The static gate
        above is unambiguous: **retail spends no draw and does not restart the clip while the clip is 5 / 6**. The
        s78i t01 port change (draw on every service) is therefore wrong; return before the draw when the clip is 5 / 6,
        as the original s78f t03 version did.
  - *Port:* when clip 6 is missing, leave the current clip as it is and draw the blend each service in state 3 / 4,
    as retail does.

*For the implementer:*
- Gate the own weapon on targeting state 4 (in range and outside min range), not on an XY distance.
- Treat the Walk states 0x18 / 0x1A as "no fire", and count only 0x19 (or the pending flag) as deployed.
- Default every per-mode hardpoint flag to true.
- Start TD's last-service frame at 0, so a late spawn's first service clears its initial delay.
- Make `Get_Current_Projectile_Type` independent of WEAPON.

**Fire-at-position order (lobbing superweapons; RE, 2026-11-02)** [H unless tagged]. TD = obj +0xf0. TD +0x30…+0x38 =
the ordered ground point, +0x3c = "order pending", +0x3d = "charged, fire now". Every reader and writer found:
- **Set** only by `Fire_Lobbing_At_Position` 0x4ea3f0, a method of `LobbingSuperweaponBehaviorClass` (vtable 0x879e98;
  it is reached only through the object's LOBBING_SUPERWEAPON 0x37 behaviour). It needs the owner's tactical player
  data and, for `Requires_Base_Power` (+0x8b), base power (0x527520). It does **not** check readiness, range, alliance
  or fog. It writes the point to TD +0x30…+0x38, TD +0x3c := 1, and restarts the special-weapon pack (obj +0x158):
  +0x28 := 0, start +8 := now, ready +0xc := now + int(0x373630(type, 0) × FPS); then it starts the ABILITY_COUNTDOWN
  display (behaviour 0x46) for ability 0x2f. A second order before the shot just overwrites the point.
  Callers (all check readiness first):
  - network command 25 `SpecialWeaponFire` execute 0x40a3b0 (COMMANDS.md): readiness (0x4914e0) ≥ **0.99**; point =
    the command's position when its "at position" bit is set or there is no target object, else the target object's
    bare position (obj +0x78, no `Ranged_Target_Z_Adjust`). `SpecialWeaponsAllowed` is not checked for a
    lobbing-only object. Afterwards the destroy-after sweep (behaviour 0x67) for the weapon's owner. The human UI path
    that queues command 25 (ability button `FIRE_LOBBING_SUPERWEAPON` 47 → targeting cursor) was not traced [M];
  - Lua `GameObject.Fire_Special_Weapon(target, player)` 0x576220: readiness ≥ 1.0 (SCRIPT_GAPS §1.1);
  - Lua `LandTaskForce.Fire_Special_Weapon(type_name, target)` 0x6eb7f0: the first task-force unit whose
    representative type (0x3973b0) **is** the named type and is ready (≥ 1.0); point = the target object's bare
    position, or the AI target's position (SCRIPT_GAPS §1.4; a region AI target with no object is accepted and fires
    at the region centre: AI_TACTICAL §7.3.4, s78f 2026-11-03). The only shipped AI user is the skirmish/campaign land
    plan `FireMagnepulse.lua` (goal `Fire_Magnepulse`, BasicLandGoalSet), which names `Ground_Magnepulse_Cannon`.
    The skirmish build `Skirmish_Ground_Magnepulse_Cannon_E` is a `Variant_Of_Existing_Type` of it, i.e. a separate
    type object, so the type test should fail and the AI should never fire the skirmish cannon through this plan [M:
    variant identity under the task-force match and 0x3973b0 not checked at run time; in-game probe below].
- **Charge-up** in the behaviour service 0x4ea6f0 (vf6), every service while +0x3c is set and the charge-up stamp
  (pack +0x24) is 0: aim the TURRET behaviour (0xb) at the point with 0x631fe0 (LOB_SOLVER §3.6: lob solution and
  turret angles reached; the turret **slews** at `Turret_Rotate_Speed` degrees per service before "aimed", and a unit
  without deploy states always uses the undeployed limits: LOB_SOLVER §3.6 "Charge-up aim", s78f 2026-11-03). No
  TURRET, no solution, or not yet aimed → wait (retried next service; no timeout). Once
  aimed: play `Lobbing_Superweapon_SFXEvent_Chargeup` (type +0x2328), start `Lobbing_Superweapon_Chargeup_Particle`
  on its bone (+0x2300 / +0x2308), stamp pack +0x24 := now. When now ≥ stamp + `Lobbing_Superweapon_Chargeup_Frames`
  (+0x2348; vanilla 90): play `Lobbing_Superweapon_SFXEvent_Fire` (+0x2338), **TD +0x3d := 1**, stamp := 0. The same
  service also plays `SFXEvent_Special_Weapon_Ready` (+0x1310) on the frame the pack becomes ready, for the local
  player's own weapon only (presentation).
- **Fire** in the WEAPON service 0x54d090:
  - while TD +0x3d is set the recharge countdown TD +0x24 is **skipped** (no wait);
  - gate step 7: an object with LOBBING_SUPERWEAPON fires **only** while TD +0x3d is set, and then needs neither
    targeting state 4 nor a target; an object without it would pass step 7 on +0x3c or +0x3d alone, but nothing sets
    them on such an object, so in practice the bypass is lobbing-only;
  - the "no target" early return is skipped while +0x3c is set; the request is the ground point TD +0x30…+0x38 with
    **no target object** (so no lead, no bone pick: "Own-weapon aim point" below, ground-point branch);
  - after a successful spawn, for a LOBBING_SUPERWEAPON object: TD +0x3c and +0x3d := 0 (one 16-bit store) and
    TD +0x30…+0x38 := 0. A failed spawn (no lob solution, LOB_SOLVER §5.2) skips this, so the order stays charged and is
    retried next service.
- **Cleared** only there and by the TD ctor 0x380540. A target's death, a new move order or the end of the readiness
  do not clear it [H for the writer search: every store to TD +0x3c / +0x3d in the decompile; M that the save code
  keeps it].
- Consequence: a lobbing superweapon never fires its `Projectile_Types` at what its own TARGETING picks; it fires
  once per order, after the turret is aimed and the charge-up has run.

*For the implementer:* keep a per-object order (point, pending, charged). `Fire_Special_Weapon` (Lua, task force,
command 25) sets point + pending and restarts the readiness timer; the lobbing service aims the turret, then counts
`Lobbing_Superweapon_Chargeup_Frames` and sets charged; the WEAPON service, for lobbing objects, fires only when charged,
ignores its recharge then, aims at the point with no target, and clears the order after a successful spawn.

**Own-weapon aim point, `Target_Bones` (RE, 2026-11-02)** [H unless tagged]. The WeaponBehavior shot spawns through
`Spawn_Projectile` 0x54ade0, which (for every projectile category except 2 and 9) calls the aim 0x54a270(shooter,
projectile type, muzzle transform, target, …). The aim point P is chosen **per shot**:
1. **No target object** (ground point, e.g. the fire-at-position order): P = the request's point, unchanged. Inaccuracy
   is applied only when request byte +0x4c is clear (it marks a dry-run spawn whose result goes to +0x4d [M]) and the
   fixed radius getter 0x374890 is non-zero; no lead.
2. **The shooter's current target is this object and has a target hardpoint**: 0x397a40(shooter) returns the formation
   attack target (0x41a2e0, when the shooter's locomotor pack +0x1f0 holds a formation) or TD +8 with the hardpoint
   index from TD +0x10 → +0x18. When that object is the request's target and the index is not −1: P = the hardpoint's
   bone world translation (0x396ce0; an index out of range, a hardpoint whose type flag +0x4c is clear, or a failed bone
   lookup falls back to the **bare** position, no z adjust). The index is passed in r8 (disassembly 0x14054a43e) [H].
3. **Otherwise, target type with `Target_Bones`**: one **synced** `Get_Int(0, n − 1)` on 0x140a13e24 (no draw when
   n = 1) picks entry i of the type's resolved bone list (+0xe68, count +0xe70). P = that bone's world translation on
   the target's current model (0x12d2c0). If the target has no model instance (obj +0x2a0 / 0x2648b0 null) or the bone
   transform fails, P = position + `Ranged_Target_Z_Adjust` (the draw is still consumed).
4. **Otherwise**: P = 0x398440(target) = position (obj +0x78…+0x80) + (0, 0, target type `Ranged_Target_Z_Adjust`
   +0x4dc). **So the port's "position + Ranged_Target_Z_Adjust" for a bone-less target is the retail rule** (the
   [M/port] tag can become [H]). `M1_Repulsor_Tank` sets 12.0; types without the tag get the default 6.0.

Then lead 0x399450 (speed 0x370f00(type, 0)) and inaccuracy 0x549f80 run on P (§3.1 steps 9–10);
categories 0, 3, 4, 6, 7 (mask 0xd9) aim at the inaccurate point, categories 1, 2, 5 keep the led point and carry the
miss as an offset. With a team-container target, `Spawn_Projectile` first replaces it by its member nearest the
launch point and resets the hardpoint index (LOB_SOLVER §5.1), so step 2 no longer matches and the member's own type
decides steps 3–4.

**Bone list.** XML `Target_Bones` (comma list, type +0xe48 strings, count +0xe58) is resolved at type model load
0x375380 into bone indices on the type's model (+0xe68 / +0xe70, built by 0x12d2a0 name lookup); names missing on
the model are **dropped** (so n counts only found bones, and the draw range shrinks). [M: which model (land / space)
when a type has both.] Vanilla FoC users: `AT_AT_Walker` and `Veers_AT_AT_Walker` (`MISSILE_TARGET00`–`02`),
`Nebulon_B_Frigate`, `Eclipse_Super_Star_Destroyer`, `CIN_Corvette_fast`; no other land unit.

**Hardpoint aim for comparison** (`Fire_At` step 5 object aim 0x383f70) [H]: the target's `Target_Bones` are tried
**in list order** (no draw): the first bone within `Get_Fire_Range` (+ target radius when the owner has
`Projectile_Types`) of the fire-bone reference point and inside the fire arc (0x383ba0) wins. Then, when the target
has hardpoints, one synced `Get_Int(0, n − 1)` picks a start index and the hardpoints are tried cyclically with the same
tests (destroyed destroyable ones skipped; bone = +0x2c, else +0x38). Last, position + `Ranged_Target_Z_Adjust` if in
range and arc. With no model instance on the target: position + `Ranged_Target_Z_Adjust`, no draw.

*For the implementer:* per own-weapon shot at an object: ordered hardpoint → its bone; else `Target_Bones` → one
synced pick per shot among the found bones (bone translation, fallback position + z adjust); else position +
`Ranged_Target_Z_Adjust`. Then lead and inaccuracy. A ground-point shot uses the point as given.

**Targets.** Both sources use the owner's current target, targeting data pack TD (obj +0xf0) +8, chosen by the
unit's TARGETING behaviour with the unit's own range (§2.2: `Targeting_Max_Attack_Distance`, or the lob reach for a
grenade weapon, LOB_SOLVER §4). The owner pushes it to every hardpoint (`HP_Set_Target`, §2.6). The own weapon fires
only at that target (or at a fire-at-position point); a hardpoint that cannot shoot it may opportunity-fire at its own
scanned target (§2.6), subject to the gate above.

**Cooldowns.** Nothing is shared. The own weapon keeps its recharge in TD +0x24 and its pulses in TD +0x28 (first
delay at attach: one synced `Get_Int(0, int(FPS × Projectile_Fire_Recharge_Seconds))` in vf9 `0x54c210`; reset as in
"Non-hardpoint weapons" above). Each hardpoint keeps its own +0x58 / +0x5c (§3.1). A shot from one source does not
delay the other. In a frame the WEAPON behaviour runs in the behaviour pass, before the hardpoint updates
(ABILITIES_HARDPOINTS §6.2).

**FoC / EaW units with both sources** (variant-resolved scan of the vanilla XML; EaW has the T4B and SPMAT rows):

| Unit | Own weapon (`Projectile_Types`) fires when | Weapon hardpoints fire when |
|---|---|---|
| `SPMAT_Walker`, `SPMAT_Walker_Captured` | deployed (`Proj_Ground_SPMAT_Grenade`: FWW_Deployed Yes, _Undeployed No) | undeployed (`HP_SPMAT_00`–`03`, `Proj_Blaster_Cannon_Green`, range 150, not vs Air / Structure) |
| `T4B_Tank`, `T4B_Tank_Captured`, `E3_T4B_Tank` | normal mode (`Proj_Ground_Laser_Cannon_Red_T4B`: Rocket No, Normal Yes) | ROCKET_ATTACK mode (`HP_T4B_Tank_03`–`05`, `Proj_T4B_Missile`: Rocket Yes, Normal No) |
| `Dark_Trooper_PhaseIII` | undeployed (`Proj_Ground_Laser_Cannon_Red_DT3`: Deployed No, Undeployed Yes) | deployed (`HP_DarkTrooper_P3_00`–`05`, `Proj_Dark_Trooper_Missile`) |
| `Destroyer_Droid`, `Destroyer_Droid_Attack` | deployed and normal mode (`Proj_Destroyer_Droid_Blaster_Cannon`) | deployed and ROCKET_ATTACK mode (`HP_Destroyer_Droid_Ion_00`–`02`, `Proj_Destroyer_Droid_Ion_Cannon`, not vs Air) |
| cinematic variants `FIN_SPMAT`, `FIN_T4B_Tank`, `CIN_T-4B_Vehicles` | as their parents | as their parents |

So in every vanilla case the flags make the two sources **alternate** (deploy state or ROCKET_ATTACK mode picks one);
they never fire together. Not in the table: `SPMAT_Walker_Death_Clone`, `CIN_Corvette_fast` and `Cin_Y-Wing_Intro` list
both but have no WEAPON behaviour (hardpoints only); `SPMAT_Walker_Telekinesis_Death_Clone` lists WEAPON but, as a
death clone, loses it (S9_DEATH_CLONE_SHIELDS §1.6). 22 more FoC types list `Projectile_Types` with neither WEAPON nor
weapon hardpoints (the AT-AT / AT-ST / MPTL / Rancor death clones, `Boba_Fett_NoStealth`, …): they have no weapon.

**Port (S8 t05):** `BattleCatalog::unit` (engine/apps/sim/src/battle_catalog.cpp) drops `Projectile_Types` when the unit
has a weapon hardpoint. That rule is **wrong**: it should key on the WEAPON behaviour, not on hardpoints. The
combat system already runs both sources side by side (`CombatSystem::update_weapons`), but implements neither gate
above, so with the catalog fixed it would also need the deploy / rocket-mode gates to keep the sources alternating.

The behaviour is attached by the type's `WEAPON` behaviour (0x0A) and is serviced like any behaviour, besides the
hard points [M: no code path that turns it off when weapon hard points exist was found]. So a type that lists both
fires its `Projectile_Types` too. FoC: `M1_Repulsor_Tank` is the only such type; its `HP_2M_Tank_00` hard point excludes
Air / Vehicle / Structure, so only the WeaponBehavior shot hurts vehicles (a1h t01; port: `battle_catalog.cpp`).

**Implementation checklist for the own weapon (s78f, 2026-11-03).** One WeaponBehavior service, in retail order; the
details are in the blocks above and in LOB_SOLVER §5.
1. Flash timers; elapsed = frame − TD +0x2c, TD +0x2c := frame; recharge countdown TD +0x24 (skipped while the
   lobbing order is charged). Pulse count reset when it differs from `Projectile_Fire_Pulse_Count`.
2. Gate 0x54c910 (steps 1–7). Step 7 reads the **state** set by the last TARGETING / TeamTargeting service
   ("Targeting states" above); team members read the state the container pushed and the container's target.
3. Target relation, animation, TURRET-slot and facing gates; energy check (no draws in 1–3).
4. Advance the fire-bone cycle index (it stays advanced when the spawn fails).
5. `Spawn_Projectile` (LOB_SOLVER §5, "Order and draws", s78f): early refusals → launch point (**garrison pick: one
   synced `Get_Int` when ≥ 2 host bones qualify**) → team-target redirect → **lob solve, return null on no arc** →
   aim (`Target_Bones` `Get_Int`, inaccuracy draws) → create.
6. Null spawn: stop here (no flash, animation, pulse or recharge change; retried next service).
7. Success: flash, Attack clip (0.4 / 0.1 blend), SFX, pulse / recharge (0x54d8f0), lobbing order cleared.

Data the port needs for steps 2 and 5:
- the garrison bones as **world matrices** of the host's current pose (translation = launch point, column 0 = the
  x axis whose yaw is tested), from the host's current damage ALT (LOB_SOLVER §5.1 "Garrison pick inputs", s78f);
- the targeting state per object, with the team copy rules (slots 31 / 32 / 47 / 48) and slot 47's synced
  `Get_Float(0.2, 0.75)` when a unit enters Attack Idle;
- for lobbing superweapons: the turret slew (LOB_SOLVER §3.6 "Charge-up aim") before the charge-up starts.

#### 3.1.2 Own-weapon (`Projectile_Types`) inaccuracy 0x549f80 (A1j t03, 2026-11-02) [H unless tagged]
Called from the own-weapon fire path after the aim point (0x54a270, §3.1.1 "Own-weapon aim point") and the lead.
Disabled by the same debug flag as step 10.
1. **Fixed radius:** r = `Targeting_Fire_Inaccuracy_Fixed_Radius` (type +0x1ff8, getter 0x374890: scaled ×(2 − m)
   on land, ×2 in space, as step 10). [M] An ability override replaces r unscaled (the object's behaviour slot
   obj +0x38a / data obj +0x230 +0x14; the port keeps the step 10 ability override). If r > 0:
   - `Get_Float(−r, r)` for X, then for Y;
   - **the Z draw only in space** (mode ≠ land). Land shots draw **2**, space shots 3.
2. **Otherwise, with a target object:** d = the shooter type's **`Targeting_Fire_Inaccuracy`** table (type +0x508,
   getter 0x3747c0; XML `<category>, <distance>` pairs, kept in XML order): the **first** entry whose category mask
   hits the target's `CategoryMask`, scaled like r (×(2 − m) on land, ×2 in space; the port also applies its land
   weather factor, as for r and step 10).
   [M] When the type has more than one weapon entry the larger of this and the current weapon's own table
   (0x378e50 / 0x373670) is used; not modelled (FoC own-weapon types have one entry).
   - L = the 3D distance from the muzzle to the **led** aim point; if d > 0 and L ≠ 0:
     s = d × L / `Targeting_Max_Attack_Distance` (type +0x500) and 3 draws `Get_Float(−s, s)` (X, Y, Z).
     [port] A zero `Targeting_Max_Attack_Distance` keeps the draws with s = 0 (retail would divide by zero).
   - No entry for the target's category, or no target object: no draw.
3. Categories DEFAULT / GRENADE / ROCKET / MPTL_ROCKET / LASER (mask 0xd9) aim at the offset point. MISSILE / BOMB /
   STATIC_BOMB keep the led point and carry the offset in the projectile pack instead [M; port: the offset is applied
   to the aim point for every category].

FoC data: 437 `Targeting_Fire_Inaccuracy` entries (infantry, vehicles, fighters), against 5 types with a fixed radius.
Before A1j the port ignored the table, so most land units fired with no spread. Example: AT-AT `Infantry, 30.0` with
`Targeting_Max_Attack_Distance` 300: at 150 units s = 15 (30 at AI accuracy 0), so infantry are often missed.

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
1. Run a ground test (0x4d0630) on the step's segment. **A1j t03 [H]:**
   - land only (no test in space);
   - `Projectile_Category` DEFAULT (0) and unknown (−1): the test is **skipped while the target point is still
     ahead**, i.e. the sign of (target point − position) equals the sign of the step on all three axes (sign(0)
     counts as +). Target point = the targeted hardpoint's world position (0x396ce0) when the pack has a hardpoint
     index, else the target's position + `Ranged_Target_Z_Adjust` (0x398440). No target object: tested;
   - every other category (LASER 7 included) is tested on every step;
   - a ground (or water) hit with no earlier object hit detonates the projectile at the ground point
     (On_Detonate 0x4cb070: no direct damage, area damage and effects there).
2. Run `GOM::Find_First_Object_Hit_By_Segment_All_Enemies` (0x2abcf0; HOTSPOTS §3 has the loop). Collision
   mask: 7 if `Projectile_Does_Shield_Damage`, else 5.
3. If nothing was hit and the projectile has either `Friendly_Damage_Amount` (+0x23ec) > 0 or
   `Causes_Infection` (+0xa7), also run the own-player variant (0x2acb90).
4. An object hit wins when its segment fraction ≤ the ground fraction.

The segment query visits **hostile players in player-list order** and returns the **first** object whose
precise test (0x38f900) or `Collision_Box_Modifier` sphere test passes. "First" means in `Query_Ray` result
order, not nearest along the segment. The port should sort by segment fraction, then by object id.

**Own-player variant 0x2acb90 (S8 t10, 2026-11-02)** [H]. This corrects the port's "skip the shooter" rule.
- The query:
  - it runs over the **projectile owner's** tree (GOM +0xd0, stride 0x38);
  - it skips only the projectile itself (its last argument), **not the shooter**;
  - a candidate needs type +0x41 == 1 (or +0x42 when obj +0x3a0 & 0x40);
  - it returns the **first** candidate that passes the precise test 0x38f900, or the `Collision_Box_Modifier`
    (+0x4e4 > 1) sphere test at the segment start, end or midpoint.
- `Sweep` then filters the hit:
  - same player as the projectile, a projectile data pack (obj +0xe8) present, and hit ≠ pack +8 (the projectile's
    target) → **rejected**;
  - otherwise a hit with TRANSPORT_LANDING (slot byte +0x368 ≠ −1) → rejected.
- **An own-player hit counts only when it is the projectile's own target.** When the first own-player candidate in
  query order is not the target, there is no own hit at all: the query does not go on to the next candidate.
- *Port:* `combat_system.cpp` (own-player sweep) calls `scan(p.owner, p.shooter)`, skipping the shooter, with a [port]
  comment. It should instead take the first own-player candidate (in the same order as the hostile scan) and accept
  it only when it is `p.target`, then apply the TRANSPORT_LANDING rejection.

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

**Convert and its team rule** (`Projectile_Convert_Enemy_*`; 0x4ca020 → per victim 0x4ca2f0) (s78h t04,
2026-11-05) [H unless tagged]:
1. **Gate (0x4ca020):** the projectile type has the convert flag, the detonating object (+0x28) is a projectile
   of that type, and `Projectile_Convert_Enemy_Radius` (type +0x1f10, read as a float) > 0. New owner = the
   projectile object's owner (+0x58).
2. **Victims:** the players from 0x294910(owner) [M: the owner's enemies], each in list order. For each one,
   the GOM radius query 0x2a8fb0 on that player's tree at the projectile position, taken in query order. A
   victim must pass 0x395c10:
   - its type category (+0x1648) matches `..._Targets_Category_Mask` (+0x1f28);
   - obj +0x3a2 bit 3 is clear;
   - slot byte +0x387 is −1;
   - type +0x23ec ≤ 0 and type +0x13 = 0;
   - obj +0x150 is null or both its +8 and +0x10 are null [M meaning].
   **There is no mode gate** and no test of the victim's team.
3. **Per victim (0x4ca2f0):**
   - **Skip** when the victim's owner already is the new owner. After the first member of a team converts, the
     rest of that team is skipped, so each team converts once.
   - **"Is a team member" is 0x39b1a0:** the victim's parent (obj +0x2b0) is non-null **and** has a TEAM slot
     (parent +0x348 ≠ −1). This is the same container test FIGHTER_FLYING §1.5 uses for squadrons, so **in
     space the fighter squadron container is the team, exactly as the infantry team container is on land**.
   - **Converted flag** (+0x3a2 |= 4, the decay of ABILITIES_HARDPOINTS §4.10):
     - not a member: set on the victim only;
     - member: set on the **container**, then on every non-null entry of the container's TEAM member list
       (container +0x110: array +0x10, count +0x18, list order). The victim is one of those entries.
4. **Owner change:** `Change_Owner` 0x38e5a0(victim, owner, 0). It propagates through the team by itself:
   - a non-container whose parent is a TEAM container calls it on the container;
   - a TEAM container calls it on each member in list order;
   - an object that already has the new owner returns at once.
   So the container and **all** its members change owner, whatever their position, category or distance.
5. **Notifications, in this order:**
   - for the victim: store / parent (0x2c0340 when 0x372230 on its type, 0x2b2cc0 when obj +0x54 ≠ 0x3fffff);
   - the victim's type is swapped to type +0x1ad0 when that is set and differs [M meaning];
   - the spawn effect (0x38d580) when +0x1f20 is set;
   - behaviour 0x20 vf+0x78, then 0x3a54c0.
   For a member, the same steps then run for the container (0x2c0340 and behaviour 0x20) and for each other
   member (all of them, including the type swap and the spawn effect).
6. **Last:** the victim's slot 0x50 / 0x4f / 0x56 (the first that exists, checked in that order) gets 0x444ab0
   [L meaning].

> **Port (s78h t04 / s78i t04, 2026-11-05).** The team is the 0x39b1a0 container: `damage_team` for a member (team
> on land, the squadron in space), or the victim's own id when it is a combat-unit container (its `damage_team` is 0
> and live units carry its id). A victim already owned by the new owner is skipped. Change_Owner covers the container
> and every live member, in notification order (step 5): the victim, then the container when it is a combat unit,
> then the other members in ascending id. Each object raises one `UnitEffect::Converted`. `CombatSystem::set_owner`
> (Lua / story) does not spread through the team; the host applies owner changes per event.

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
  - **Who the "squad leader" is (s78h t03, 2026-11-05) [H]:** the **team container itself**, the object with a
    TEAM slot (obj +0x348 ≠ −1, behaviour 0x16). It is never one of its members.
    - Slots: the own product uses the object's own type (+0x298) and its own ability state (+0x3d0 + 16 × t),
      and an ability type ≥ 0x4d or = 0x22 (LUCKY_SHOT) counts 1.0.
    - Trigger: when that own product is **exactly 1.0** and the object has a TEAM slot, it fetches the TEAM
      behaviour (vf+0x10 with 0x16).
    - Walk: the TEAM member list (count 0x405300, entry 0x4052d0: the container +0x110 vector, list order).
      It calls 0x398010 **recursively on each non-null member**, so a member contributes its full value, which
      is its own product, because members have no TEAM slot.
    - Minimum: kept with `<=` from a 1e18 start. Every member counts, including members whose product is 1.0
      (inactive), so one member at 1.0 caps the team at ≤ 1.0. **The container's own value (1.0) is not in
      the minimum**; it is multiplied by it (1.0 × min = min).
    - Edge cases: an empty list, or no non-null member, leaves 1.0. A min > 1 is used as is (all members at 1.5
      give the container 1.5).
    - A container whose own product ≠ 1 keeps its own product. Its own slots are active only for per-unit
      (U = 1) abilities, because U = 0 activations are forwarded to the members (ABILITIES_HARDPOINTS §2.3).
    - **A member never takes the squad value.** Without a TEAM slot it returns its own product. Every reader
      that passes a member therefore reads that member's own product: fire recharge 0x54cef0 / 0x54d8f0 /
      0x3825b0, shields 0x549250 / 0x5494b0, energy 0x56bfd0 / 0x56c030, `Take_Damage` kinds 7 / 8, blast
      radius 0x4c7f70, ion / lucky shot 0x704b50 / 0x7064e0 / 0x706d90.
    - The squad minimum reaches only callers that pass the container [M which ones]. The candidates are the
      SPEED reads 0x3728f0 / 0x376990, for a team locomotor (MOVEMENT §3.4).
    - **No mode gate:** a space fighter squadron container (TEAM slot) gets the same rule.
  - **Port gap (2026-11-05):** the port has no container unit, so a `team_leader` member stands in for it
    (`apps/sim/src/tactical_battle.cpp` deploy sets the chunk's first member; `tactical_battle_bt.cpp` the
    same).
    - Phase A of `CombatSystem::tick` (`engine/tactical/combat/src/combat_system.cpp`, "Squad rule (serial)")
      overwrites that member's `mult` with the minimum over the *other* members. The stand-in's own product is
      both left out of the minimum and replaced for its own weapons, shields, damage and speed.
    - `mod_multiplier()` / `mult()` therefore return the squad value for one real member. Space squadrons get no
      rule, because `team` is land only.
    - **Rule to implement:** every unit, the `team_leader` included, keeps `mult = own_mult`. Add a separate
      per-team value, keyed by the container id (`team`, or `damage_team` for a space squadron), equal to the
      container's own product (1.0 while the port has no container abilities) when that is ≠ 1, else the
      minimum over **all** live members of the team (stand-in included, 1.0-valued members included, 1.0 when
      none).
    - Only container-level readers use the per-team value (a team locomotor's SPEED, when the host models one).
      Drop `squad_seen`, and change `squad_multiplier` (`modifiers.cpp`) so the caller passes every member.
    - (s78i t04, 2026-11-05) `CombatSystem::team_mod_multiplier` still has no caller: the port has no team
      locomotor (movement models fighter squadron formations only; `TEAM_LOCOMOTOR` is just a behaviour name in
      objects), so no container-level SPEED read exists to wire it to.
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
19. Land facing and arc logic (`Base_Land_Targeting_Arc_Angle_Coefficient`, 0x633ae0). **Partly answered (s78f,
    2026-11-03):** the land facing test (slot 48, half a frame of turn), the snap / turn request and the state 3 rules
    are in §3.1.1 "Targeting states, slots 47 / 48 and team copies"; the arc coefficient itself is still open.
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
