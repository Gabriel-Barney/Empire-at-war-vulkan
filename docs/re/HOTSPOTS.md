# Hot-spot behavioural specs

These are the functions the installed patch hooks (see `PRIOR_PATCH.md`). Each spec comes from the Ghidra 12.2 decompilation and gives the inputs, outputs, data touched, loop structure, cost, and whether the function can run in parallel. The specs are clean-room: they describe what the code does and do not reproduce it. Names are the ones now saved in the Ghidra project (`tools/re/ghidra/renames*.csv`). RVAs are relative to image base 0x140000000.

Notation: N = number of objects in the queried set, G = number of goal types, T = target candidates per goal type, C = number of budget categories.

---

## 1. AI goal servicing (AIGoalSystemClass, GalacticGoalSystemClass / LandGoalSystemClass / SpaceGoalSystemClass)

### `AIGoalSystemClass::Service` - vftable slot 2, 0x6cacd0
- **When it runs:** once per AI service tick for each AI player's goal system, through ServicedAISystemClass.
- **What it does:**
  1. Calls the base service.
  2. If the frame-skip countdown at `+0x18c` is above zero, decrements it and returns.
  3. Otherwise runs one slice of the proposal pass (`Propose_Goals_Incremental`).
  4. If the proposal pass has finished (flag at `+0x184`):
     - recomputes budget-category weights (`AIBudgetClass::Recompute_Category_Weights` on the embedded budget at `+0x190`);
     - commits the proposed set into the active set (`Commit_Proposed_Goals`);
     - clears the flag;
     - recomputes the per-service evaluation quota `param[0x2f]` as `ceil(evaluations_last_pass / (FPS * 5))`, capped at `(int)(20 / max(1, players - active humans) + 0.5)` and at least 1 (players = the whole PlayerList; A2M_R2_RE_A §2);
     - resets the evaluation counter;
     - reloads the countdown from virtual slot 11 (`+0x58`).
- **Cost:** amortized and adaptive, so a full proposal pass is spread over several ticks. Every commit is a burst: weight recompute + sort + resolve.
- **Side effects:** mutates its own state and the AI player's goal and plan structures. It runs on the game thread.

### `AIGoalSystemClass::Propose_Goals_Incremental` - 0x6cb120
- **Inputs:** this; the AI player (`+0x28`), its perception system, the goal-type list for the current mode, and the global player list.
- **Loop:** a resumable two-level cursor over goal types (`+0x17c`) × targets (`+0x180`), capped at `param[0x2f]` evaluations per call. Global goal types (application kind 0x10) evaluate once with no target. Other goal types iterate the target list returned by the perception system's virtual slot 8 (`+0x40`), filtered by the target's virtual slot 4 (`+0x20`, "applies to goal type").
- **Work per candidate:**
  1. `operator_new(0x98)` and construct an `InstantiatedGoalClass`.
  2. Ask whether the goal is already active (virtual slot 9, `+0x48`).
  3. Build a stack `PerceptionContextClass` and bind 3 to 4 tokens (an id field at `+0x478` of the owning player object, then the enemy, global and target). Each bind inserts into an `unordered_map<int,...>`, which means heap nodes.
  4. `PerceptionFunctionClass::Evaluate` (in PerceptionFunctionG.dll).
  5. Add `count_a*w1 + count_b*w2`, where w1/w2 are goal-type floats at `+0xac`/`+0xb4` and the counts come from the plan or learning system.
  6. If the score is above 0, store it as desire and priority, ask whether the goal is proposable (virtual slot 10, `+0x50`), and insert it into the proposed set at `+0xd8`.
  7. Destroy the context and release the goal reference.
- **Allocations:** at least 4 heap allocations per candidate (goal 0x98, context node 0x20, bucket array 0x80, one 0x20 node per bound token). Most are freed immediately.
- **Complexity:** O(G·T) perception evaluations per full pass. This is the dominant galactic-map CPU cost.
- **Parallelizable:** the score of each (goal type, target) pair depends only on read-only world state and perception tokens. It is a pure function apart from the allocations and the final insert, so the pass can be split into parallel scoring jobs plus a serial ordered insert. Keep the insertion order, because downstream tie-breaking and the RNG depend on it.

### `AIGoalSystemClass::Commit_Proposed_Goals` - 0x6c8710
- **Inputs:** the proposed set, the active set (`+0x38`), and the global goal-category list.
- **Steps:**
  1. Clear and rehash the active-set containers.
  2. Collect category ids into a temporary `vector<int>`.
  3. Sort the categories by budget weight. `Sort_Categories_By_Budget` (0x6c76f0) is an introsort whose comparator re-queries the budget weight on every compare, so it does O(C log C) lookups.
  4. For each category, activate the plans of the already-active goals that have a plan at `+0x68` (0x6c0be0).
  5. Swap in the proposed set.
  6. Call `Resolve_Category` (0x6c8970) for each category.
- **Resolve_Category:** merges the old and new goal lists. It keeps goals whose desire beats the best proposed one, allocates budget (`AIBudgetClass` at `+0x190`), and releases rejected goals back to the planning system.
- **Complexity:** O(C·(Gc log Gc)) with many container copies (std vectors and hash maps).
- **Parallelism:** serial. It mutates the AI planning state, but runs only once per pass.

### `AIBudgetClass::Recompute_Category_Weights` - 0x614590
- **What it does:** builds one perception context and evaluates each budget category's perception function to set its weight.
- **Cost:** O(C) evaluations.
- **Parallelism:** pure apart from writing the weights, so it can run in parallel with a gather step.

### Containers and allocation (patched by `ai_pacing` / `memory`)
- 0x7218xx-0x722axx are the instantiated hash-map/vector helpers for the goal sets: clear, swap, find, insert, count.
- `vector_int_Emplace_Reallocate` (0x2c2bf0) grows the category vector.
- The pacing patch replaces the frame-skip countdown with a 6 ms wall-clock budget and catch-up rules.

---

## 2. Perception evaluation

### `PerceptionContextClass` - ctor 0x3ef160, dtor 0x7b260
- **What it is:** a 0x58-byte stack object: an unordered_map of token to value (8 initial buckets, max load factor 1.0) plus owner and target pointers. Layout is in `STRUCTS.md`.
- **Cost:** the constructor makes 2 heap allocations (list sentinel 0x20 and buckets 0x80). The destructor walks and frees the nodes.
- **Hash map** (`IntKeyHashMap_Emplace`, 0x241df0):
  - Key hash: minstd of `(key ^ 0xdeadbeef) & 0x7fffffff`, i.e. `(k % 127773)*16807 - (k / 127773)*2836`, wrapped positive.
  - The bucket index is that hash masked with `+0x30`.
  - Nodes are 0x20 bytes.
  - It rehashes ×8 when the load factor is exceeded (×2 above 512 buckets).
  - The same container type is reused in LuaScriptClass and the GameObjectManager. That is why the `ai_work` patch's "perception_scratch/binding_scratch" (a reused scratch context) gives a large win.
- **Parallelism:** fully thread-local. The cost is allocator contention: the process heap takes a lock.

### `PerceptionFunctionClass::Evaluate` (PerceptionFunctionG.dll)
- **What it does:** walks an expression tree (built by boost::spirit from `Data/XML/AI/PerceptualEquations`) and resolves token leaves through the exe callback, which calls the evaluator classes (`*PerceptualEvaluatorClass`).
- **Cost:** O(nodes × token cost). Token cost varies: some tokens run spatial queries such as "enemy force within range" through the cull tree, or galactic graph walks.
- **Side-effect free:** yes, apart from per-frame caches in some evaluators. This is the best candidate for worker threads.

### `FindTargetClass::Lua_Reachable_Target` - 0x698360 (Lua `FindTarget.Reachable_Target`)
Lua `FindTarget(...)` itself is `FindTargetClass::Lua_Call` (0x697a20), and `Best_Of` is 0x697450.
- **Inputs:** the calling plan/taskforce, a perception function name (looked up in `DatabaseObjectManagerClass<PerceptionFunctionClass>`), a goal application, a reachability mode, and an optional maximum range.
- **Loop:** over the candidate list from the AI player's perception system (galactic: planets or AITargetLocations).
- **Work per candidate:**
  1. Build a PerceptionContext and bind the owner, enemy, global and target tokens.
  2. Evaluate.
  3. Optionally reject by squared distance to the taskforce position against range².
  4. Keep the running best.
  5. Append `(candidate, cumulative_score)` pairs (16 bytes each) to a vector.
- **Selection:** `Weighted_Random_Pick` (0x698de0) draws `rand * total` from the synced GameRandom and binary-searches the cumulative array.
- **Output:** an AITargetLocation wrapper, or nil.
- **Complexity:** O(T) evaluations + O(log T) for the pick.
- **Determinism:** the result consumes the synchronized RNG. A rewrite must keep candidate order, the score math (float accumulation order) and exactly one RNG draw. The patch's `best_only_scoring` changes semantics (best instead of a weighted pick) and is not lockstep-safe.

---

## 3. Spatial tree (TreeCullClass) - culling and gameplay queries

- **Node layout (`TreeCullNode`):**
  - `+0x00` center (3 floats) and `+0x0c` half-extents (3 floats);
  - `+0x18..+0x28` a `std::vector` of entry pointers stored at the node;
  - `+0x30` and `+0x38` two children (binary tree; either can be null).
- **Entries:** each tree entry is a heap-allocated 0x48-byte **CullLink**. A CullLink holds `+0x08` a pointer to the CullObjectClass embedded in the GameObject at `+0x28`, `+0x18` the intrusive result `next` pointer, `+0x20` its node, `+0x28` its index in the node vector, and `+0x2c` a cached AABB.

### Classifiers (pure, inline-able)
- `Classify_AABB_vs_AABB` (0x20bb60): returns 1 (disjoint), 2 (A fully contains B) or 8 (straddle). It uses center/extent separating-axis tests on 3 axes.
- `Classify_AABB_vs_Frustum` (0x20be80): 6 planes of 16 bytes each, starting at frustum+0x30. It uses the "positive vertex" trick (`sign(n) * extent`) and an in/out bitmask that children inherit, so planes the box is fully inside are skipped.
- `Classify_Ray_vs_Node` / `Slab_Test` (0x20c6d0 / 0x20cd70): segment-versus-box slab test returning a hit.

### Queries
`Query_AABB` (0x20e780), `Query_Frustum` (0x20e9f0) and `Query_Ray` (0x20eac0), each with a recursive collector at 0x20fef0, 0x2100a0 and 0x2102d0.
- **Input:** a query object (`+0x08` result head, `+0x10` root) and a shape.
- **Algorithm:** a depth-first walk.
  - If the node is fully inside, append all its entries and recurse with "accept all" set.
  - If it straddles, test each entry's AABB (the next entry is prefetched) and recurse into the children.
  - If it is disjoint, prune.
- **Temporary storage:** results go into a temporary `vector<CullObject*>` (heap; grows through `vector_ptr_Emplace_Reallocate`). The vector is then turned into the intrusive list on the entries, and freed. A global per-query-type counter is incremented.
- **Complexity:** O(log N + k) for a balanced tree. The tree-rebuild policy lives in CullSystemClass and has not been analysed yet.

> **Status 2026-10-30:** The rebuild policy is analysed: see "Tree maintenance: insert, remove, refit and rebuild (pass 3)" below (dirty flag at more than 16 links in a node or when more than 20% of links have migrated; `TreeCullClass::Rebuild` at 0x20f180).
- **Thread safety: none.** Results are written into the shared entries (`+0x18`) and the query object, so two concurrent queries corrupt each other. A parallel engine needs read-only nodes and per-query result buffers. The patch's `visibility scratch_pool / bulk_append` removes the per-query vector allocation.

### `GameObjectManagerClass::Find_First_Object_Hit_By_Segment_All_Enemies` - 0x2abcf0
The `_For_Player` variant is 0x2acb90.
- **Used by:** ProjectileBehaviorClass for hit detection along a swept segment.
- **Outer loop:** for each player that is hostile to the shooter's player, run `Query_Ray` on that player's tree. The query objects are at GOM+0xd0 + player_id×0x38, so there is one tree per owner.
- **Per hit entry:**
  1. Recover the object (entry − 0x28) and skip the ignore object.
  2. Read the collidability flag from the type: `Collidable_By_Projectile_Living` at `+0x41`, or `_Dead` at `+0x42` if object flag `+0x3a0` bit 0x40 is set.
  3. Run the precise collision test (0x38f900). If it hits, return the object and fill the hit info.
  4. Otherwise, if the type's `Collision_Box_Modifier` (`+0x4e4`) is above 1, test the start, end and midpoint against a sphere of radius `max_extent × modifier` (3 `sqrt` calls).
- **Output:** the first hit object.
- **Complexity:** O(P·(log N + k)) per projectile per frame.
- **Parallelism:** it is read-only apart from the cull-tree result lists. With per-query buffers it can run across projectiles in parallel; the hits are then applied serially.

---

### Tree maintenance: insert, remove, refit and rebuild (pass 3)
- **`TreeCullClass::Insert` (vf1, 0x20e600):**
  1. `operator_new(0x48)` for the link, which is pushed onto the object's membership list.
  2. Copy the object's AABB (CullObject virtual slot 1).
  3. Place the link in the **smallest existing node that fully contains it** (`Find_Smallest_Containing_Node`, 0x210480, a DFS over both children, so O(nodes) in the worst case), or the root if none does.
  4. If that node now holds more than 16 links, set the tree's dirty flag (`+0x20`).
  5. Increment the link count (`+0x18`).
  - No split happens here.
- **`TreeCullClass::Remove` (vf2, 0x20e6d0):** finds this tree's link in the object's list, does an O(1) swap-remove from the node vector using the stored index, frees the link, and decrements the count. Nodes are never merged here.
- **`TreeCullClass::Update_Bounds` (vf4, 0x20f0a0):** called whenever an object moves or its bounds change.
  1. Store the new AABB in the link.
  2. If the current node still fully contains it, stop.
  3. Otherwise increment the moved counter (`+0x24`), remove the link from the node, **walk up the parents** (`+0x40`) to the first ancestor that contains it, and add it there.
  4. Mark the tree dirty when `link_count < 5 × moved`, i.e. once more than 20% of the links have migrated since the last rebuild.
  - Links therefore only migrate upward between rebuilds, and node bounds are fixed: there is no refit.
- **`TreeCullClass::Rebuild` (0x20f180):**
  1. Collapse the whole tree into the root (`Collapse_Subtree_Into`, 0x20f600, which frees the node blocks).
  2. Leave links whose largest half-extent exceeds the threshold (`+0x28`, default 500.0) at the root.
  3. Recursively `Split_Node` (0x20fc50) the rest. A node splits only when depth ≤ 16 and it holds at least 4 links.
     - The split axis is the node's longest extent.
     - `Partition_Links` (0x210610) finds the median of the link centers along that axis: by sort for fewer than 16 links, otherwise by a 16-bin histogram between the min and max.
     - Each child's bounds are the union of its links' AABBs.
     - Children come from 3-node blocks (0xf0 bytes).
  - It records the max depth (`+0x1c`) and resets the moved counter.
  - Cost: O(n log n), with heavy allocation.
- **When rebuilds happen:**
  - `TreeCullClass::Rebuild_All_Dirty(game_time)` (0x20f000) runs **once per main-loop iteration**, from the top-level loop in 0x5d990, just before the game-mode manager update. It walks the global registry of all trees (every TreeCullClass registers itself in its constructor) and rebuilds each **dirty** tree, at most once per second of game time per tree (`+0x2c` holds the last rebuild time).
  - Explicit rebuilds with a chosen threshold (`Rebuild_With_Threshold` 0x20e530, `Rebuild_Default_Threshold` 0x20e5e0) happen on map load and on GameObjectManager re-initialization (0x2af4f0), and from the scene setup (0x1b3be0).
  - There is no incremental rebalancing.
- **Implications for the port:**
  - Bursts of movement make trees dirty and trigger an O(n log n) rebuild on the main thread, at most once a second per tree. GameObjectManager has **one tree per player**, plus the render scene trees.
  - In between, queries degrade as moving units pile into upper nodes.
  - A replacement BVH should support cheap refit and run rebuilds off-thread with double-buffered read-only snapshots for the query jobs.

## 4. Allocator paths

- **`operator_new` (0x769c58):** a loop of `_malloc_base(size)` → `HeapAlloc(process heap)`. On failure it calls the `_callnewh` new-handler and throws `bad_alloc` if there is none. It has 2,850 call sites.
- **`operator_delete` (0x769c94):** a thunk to `free`.
- **No custom general allocator.** Pooled types use `PooledObjectClass<T,N,EmptyLockerClass>` free lists, which are not thread-safe.
- **`vector_ptr_Emplace_Reallocate` (0x45580 / 0x45480) and `vector_ptr_Change_Array` (0x48410):** MSVC `std::vector<T*>` growth. Capacity grows to `max(size+1, cap*1.5)`. MSVC's big-block alignment applies: blocks ≥4 KB carry a back-pointer at −8, checked against 0x1f slack.
- **`vector_48b_Emplace_Reallocate` (0x27da40):** the same for 0x30-byte elements (a vec3 plus a modifier list, copy-constructed by 0x4ad4a0 / destroyed by 0x4ad530).
- **Hot callers:** cull queries (one vector per query), perception contexts (2 + tokens per evaluation), goal proposal (0x98 per candidate), render submission (`alRenderableMesh::vf8` allocates a 0x80-byte `alMeshRenderTask` per mesh × pass × light every frame), and space path smoothing (`SpacePathFinderClass::Build_Smoothed_Path`, 0x597dc0, 5 temporary vectors per path).
- **Port recommendation:** frame/scratch arenas per thread, plus pooled render tasks.

---

## 5. Combat stat modifiers (patched by `direct modifier` / `modifier merge`)

Every stat read that can be affected by abilities, heroes, planets or difficulty follows the same pattern:
1. construct a stack `ModifierSet` (0x535cb0): 11 modifier lists × 0x20 bytes, each a red-black-tree-backed container;
2. `GameObjectClass::Gather_Modifiers` (0x395c70) fills it from every source affecting the object;
3. `ModifierSet_Merge` / `ModifierList_Merge` (0x536570 / 0x536330) append other sets, such as attacker and hardpoint sources;
4. `ModifierSet_Reduce_MaxThenSum` (0x33fb70) computes, for the requested kind, the maximum within each source (`GreaterThan<float>`) and then the sum across sources (`Plus<float>`);
5. destroy the set (0x535fb0).

Instances of this pattern:
- **`GameObjectTypeClass::Get_Max_Tactical_Health` (0x3727a0):** `Tactical_Health` (`+0xdcc`) × difficulty factor (game-mode 1 or 2 globals) × faction multiplier (player+0x360 → faction data +0x50) × (1 + modifiers).
- **`HardPointClass::Get_Max_Health` / `Get_Fire_Range` (0x385ae0 / 0x3857d0):** HardPoint `Health` (`+0x58`, through `HardPointDataClass::Get_Health_Difficulty_Scaled` 0x540070) or `Fire_Range_Distance` (`+0x238`), × (1 + modifiers).
- **`GameObjectClass::Get_Damage_Multiplier` (0x38e0c0):** (1 + attacker modifiers) × (1 − defender modifiers) × faction difficulty × the type's `Surface_Type_Cover_Damage_Shield` (`+0x1e48`) when the target stands on cover terrain.
- **`TargetingBehaviorClass` vftable slot 40 (0x566ca0):** the same reduce, used for target scoring.

**Cost:** every call allocates and frees tree nodes for 11 lists. These functions run per weapon shot, per target evaluation and per tooltip, so the cost is O(sources × modifiers) plus allocation.

**Purity:** pure functions of world state. They can be cached per object per frame, invalidated when the modifier set changes. That is effectively what the patch's `bulk` merge does.

---

## 6. Renderer CPU (for reference)

### Skinning and bone palette (A6a)
- **Splitting is baked by the exporter. The engine never splits meshes.**
- The ALO sub-mesh loader (0x187d40) reads the vertex chunk (0x10003) and the index chunk (0x10004) **straight into the D3D vertex/index buffers**. It applies no re-indexing.
- Chunk **0x10006** is read into a per-sub-mesh `vector<int32>` at sub-mesh `+0x80/+0x88`. This is the sub-mesh's **bone mapping**: palette slot → skeleton bone index. The per-vertex index in `NORMAL.w` addresses this palette.
- There is no code that counts bones per sub-mesh or partitions triangles. The 24-entry limit is the exporter's contract together with the shader's `SKINMATRIXARRAY` array size.
- **Upload:**
  1. At effect load (0x1af110) the parameter with semantic `SKINMATRIXARRAY` is looked up. Its handle goes to effect `+0xa8` and its declared element count (e.g. 24) to `+0xa4`.
  2. A technique annotation `CPUSKIN` (BOOL, effect `+0x8c`) marks techniques that need software skinning.
  3. Per draw, `alMeshRenderTask::vf2` binds the skeleton palette object stored in the task at `+0x78` through 0x17f6b0. The task constructor (0x15e0d0) takes it from the top of the render context's skeleton stack (context `+0x28 + count*8`, count at context `+0xb0`), which the enclosing `alHModel` pushed during submit; it is not indexed by LOD. (corrected 2026-10-26, see RENDER.md) The render-state singleton (0x140a6eb60) expands that palette's 3×4 bone transforms into 4×4 matrices in a scratch buffer at `+0x9a0` (0x1ac5f0), only when its dirty flag `+0x99c` is set.
  4. The effect commit (0x1af410) calls **`ID3DXEffect::SetMatrixPointerArray`** (vtable `+0x150`) with `+0xa4` matrices. An alternative path, chosen by a global flag, writes them as vertex-shader constants: 3 registers per bone, starting at c0.
  - SetMatrixArray proper (`+0x140`) is not used for skinning.
- **Port note:** upload the palette once per sub-mesh draw as a 24×float3x4 constant or structured buffer indexed by `NORMAL.w`. The bone mapping (chunk 0x10006) must be applied on the CPU (or in a compute pre-pass) when building the per-draw palette.


- **`alRenderableMesh::vf8` (0x14c270), submit:** for each sub-mesh whose effect's render-phase bit is in the render context's phase mask (context+0x14), it allocates an `alMeshRenderTask` (0x80 bytes) and queues it in that phase bucket. The constructor copies the context's current light-scale/tint float4 (the per-draw `LIGHT_SCALE`) into the task; the 4 floats copied at task+0x3c..+0x48 come from context+0x6c2d8 (bounds data written by `alHModel` for flagged models), not from fog. It then queues an extra task for each active **extra render pass** (`alExtraRenderPass`, e.g. the occluded-unit pass; context+0x1c8 array, count at +0x248) whose effect has the same vertex-processing id, in that pass effect's bucket. (corrected 2026-10-26, see RENDER.md)
- **`alMeshRenderTask::vf2` (0x15e1a0), execute:** sets the world matrix (identity if the mesh is skinned), binds the vertex and index buffers, then calls the effect's Begin, BeginPass×n, draw, EndPass and End through the material's virtual slots 7 to 11.
- **Particle preparation (0x162c60-0x1698fe, `alParticles`):** CPU-side billboard expansion and vertex writes. It is serial per emitter and has no allocations.
- **Patch effect:** pools the tasks, batches the vertex preparation and filters redundant effect-parameter sets. The probe CSV shows ~70% of sampled sets are redundant.
