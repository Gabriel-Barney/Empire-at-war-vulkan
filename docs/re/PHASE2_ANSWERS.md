# Phase 2 RE answers: galactic pathing, tick rate, XML duplicates, VFS order

Clean-room behavioural answers from the Ghidra decompilation of the 64-bit Steam `StarWarsG.exe` (image base 0x140000000). All addresses are RVAs. These answers describe behaviour only; no decompiled code is reproduced.

Confidence levels:
- **confirmed**: read directly from the decompilation, with no inference needed beyond the arithmetic shown.
- **likely**: read from the decompilation, but one link (a field's meaning or an enum value) is inferred.
- **unknown**: not resolved statically.

## Summary

| # | Question | Answer (short) | Conf. |
|---|---|---|---|
| 1 | Which planets are graph nodes | Both finders use the same object query: objects in the GameObjectManager's planet list that have a PLANET behaviour slot and an owner id other than -1. `In_Background` is **not** tested. `Galaxy_Core_Art_Model` is excluded only because its sole behaviour is IDLE. | likely |
| 2 | Free-hop test | A*: `distance <= MaximumFleetMovementDistance`, plain 3-D Euclidean distance (sqrt, not squared), float, read at graph build. AI finder: also inclusive (unlinked only when `distance > max`), but its distance adds the source type's `Layer_Z_Adjust` to the destination's z. | confirmed |
| 3 | Max HS factor and heuristic | Starts at 1.0f (constructor) and only rises: it is set when a route's HS_Speed_Factor is strictly greater. It is never reset on a rebuild. h = 3-D distance(neighbour, goal) / max; f = g + h; all float. | confirmed |
| 4 | A* open list | 1-based binary min-heap on f only, no g tie-break. Strict comparisons in both sift directions. Closed nodes are never reopened. An improvement pushes a duplicate entry (lazy decrease-key) without removing the old one. Goal test on pop. | confirmed |
| 5 | Flags 0/1 | Flag 0 = may pass planets whose owner is hidden, unowned or owned by a neutral faction. Flag 1 = may pass planets owned by a non-neutral, non-allied faction. The "enemy presence" bit is really the local player's visibility mask bit 0 (ownership visible). | likely |
| 6 | Edge_Cost order | fog gate → base cost → flag-4 destination accept → active-battle reject → passable/impassable branch → danger test. The "+100" is a **corruption** surcharge, not a blockade penalty: a dangerous planet is still accepted at +100.0f when it is corrupted and the mover's faction has Benefits_From_Corruption. Flag 2 skips the danger test only for passable planets. | confirmed |
| 7 | Node_Weight | The AI perception value (double), plus enemy strength when the AI does not hold the planet. Truncated toward zero to int64. Recompute takes the low 32 bits as **unsigned** and divides by 100 with unsigned integer division. Negative values wrap to about 42.9 M. No clamping. | confirmed (formula), likely (perception meaning) |
| 8 | Edge_Weight | Float distance (optionally divided by the route HS factor, float) × 10.0f, truncated toward zero, masked to 16 bits. The result is that plus node_cost(destination) × 65536, in 32-bit wrapping arithmetic. -1 when the AI player is fogged out of an endpoint, or when there is no route and distance > max. | confirmed |
| 9 | Recompute_All_Pairs | Diagonal: weight 0, predecessor -1. Off-diagonal: weight = Edge_Weight, predecessor = row index, or -1 when unlinked. Loop order k, i, j. An edge is skipped if either half is -1. Relaxes when the **unsigned 32-bit wrapping** sum is strictly less than the current value read as unsigned. On relaxation, pred[i][j] = pred[k][j]. | confirmed |
| 10 | Get_Path | Walks the predecessor row of the source backwards from the destination, inserting at the front. The weight vector is **per hop** (the all-pairs value between consecutive path nodes); the destination's entry is 0. src == dst returns [src, src] with weights [0, 0]. Unreachable or unknown planets return an empty list. | confirmed |
| 11 | 90-frame cadence | Constant 90 (0x5a). Counter = int at the current game mode's +0x10. Per AI player. The first recompute runs at creation, outside the cadence. The next one is staggered across AI players: frame + round(slot × 90 / K). | confirmed |
| 12 | Tick rate | One global FrameSynchronizer runs at 30 logic frames per second for both galactic and tactical (1/30 s per frame). Single-player wall pacing is 1000/30 = 33 ms (integer). Multiplayer paces at 1000/35 ms or 1000/40 ms. Single-player fast mode runs 120 frames per second and renders every 4th frame. | confirmed (constants), likely (mode semantics) |
| 13 | XML duplicates | GameObjectType: **the first definition wins** for name lookup, but every duplicate still becomes a type object. TradeRoute and Campaign: **the last definition wins** (whole-object replacement, not a field merge). GameObjectFiles.xml is parsed in list order. TradeRouteFiles.xml and CampaignFiles.xml are de-duplicated and parsed in **ascending CRC32 order of the listed file name**. | confirmed |
| 14 | VFS order | `MODPATH=` and `STEAMMOD=` are handled token by token, so they are **interleaved in command-line order** in one search list. MEGs named in MegaFiles.xml are opened through the normal FileClass open (not archive-only), so a mod's `Data\Config.meg` replaces the base one. | confirmed (static) |

## Corrections to existing docs (applied 2026-10-26 to GALACTIC.md, STRUCTS.md and SUBSYSTEMS.md; each spot is marked "corrected 2026-10-26")

Probe v2 confirmed at runtime that the later-listed MEG wins.

- **GALACTIC.md §3, fog bullet:** the condition is reversed. `player+0x62` is `Is_Human` (Lua `PlayerWrapper.Is_Human` at 0x603960 returns that byte). See Q6.
- **GALACTIC.md §3, flag 0/1 table rows, and "blockade penalty":** see Q5 and Q6. Faction `+0x10b` is `Benefits_From_Corruption`, not a free-passage flag. Faction `+0x108` is `Is_Neutral`.
- **STRUCTS.md, PlanetaryDataPack:**
  - `+0x2f4` is the **corruption type index** (0..7, -1 = not corrupted), not the owner. It indexes the GameConstants per-corruption-type income table (0x2c8140, used by the corruption income tooltip at 0x2f2ad0).
  - `+0x148` is the orbital starbase level (written when a starbase is built, 0x400b40).
  - `+0x1a8` is the local player's `tVisibilityLevelType` bitmask (0x3fa490).
- **SUBSYSTEMS.md, MEG list:** `Add_Megafile` (0x216b50) links the new archive at the **tail** of its list. It is not a head insert. The newest-wins lookup claim therefore depends on `Open_File` (0x219ff0) iterating in reverse or comparing load indices. This was not re-verified here.

---

## 1. Which planets are in the graph

**Answer.**
- **A\* graph build (0x3f1c10):** it fetches the galactic game mode, then runs the GameObjectManager object query (0x2a9ff0) with "category" 3 and no other filters (no distance limit, no limit on result count).
- **What that query does for category 3** (confirmed):
  - It walks the manager's dedicated list at manager +0x560, the list selected for category 3.
  - It keeps an object only when both of these hold:
    - its owner id at GameObject +0x58 is not -1;
    - its behaviour-slot byte for behaviour 3, at GameObject +0x335 (the per-behaviour slot table at +0x332 indexed by type), is not 0xFF. In other words, the object actually has behaviour 3.
- **Behaviour 3 is PLANET** (likely). The build immediately asks each node for behaviour 3 and passes it to `Find_Trade_Route_To` (0x3f95a0), a PlanetaryBehavior method that reads the trade-route link array.
- **The AI finder's `Initialize` (0x693e20)** issues exactly the same query with the same arguments, so the two node sets are identical.
- **Node order** is the order in which the query returns objects, i.e. the order of the manager's +0x560 list (probably object creation or registration order; not verified). Both finders index planets in that order.
- **`In_Background` is not consulted** (type field +0x3e is not read on either path).
- **`Galaxy_Core_Art_Model`:** in FoC `PLANETS.XML` its `<Behavior>` is only `IDLE`, while all 64 real planets have `SELECTABLE, PLANET, PRODUCTION`. So it has no PLANET slot and is excluded. A mod "background planet" that does declare PLANET behaviour **would** become a node.

**Evidence.** 0x3f1c10 (query call, vf+0x10 with argument 3, `Find_Trade_Route_To`), 0x2a9ff0 (list +0x560, owner != -1 test, behaviour-slot test), 0x693e20 (same query).

**Confidence.** The filter itself is confirmed. The equation "behaviour 3 = PLANET" is likely.

**Port.** Build the node set from objects that have the PLANET behaviour and a valid owner, in object-registration order. Do not filter on `In_Background`.

## 2. Free-hop test

**A\* (0x3f1c10)** (confirmed):
- Distance is `sqrtf(dx² + dy² + dz²)` over GameObject +0x78/+0x7c/+0x80: plain 3-D distance, not squared, in float.
- An edge (one entry in each direction) is added when a trade route exists between the pair, **or** when the distance is `<=` the GameConstants float at +0x108 (`MaximumFleetMovementDistance`, global 0x140b15908).
- The constant is read once per graph build.

**AI finder `Edge_Weight` (0x693c30)** (confirmed):
- The pair is unlinked only when there is no route **and** the distance is strictly `>` the copy at finder +0xa8 (taken from the same constant in `Initialize`). So a free hop is also inclusive (`<=`).
- Its distance differs from the A\* one: when the **source** planet's type has a non-zero `Layer_Z_Adjust` (type +0xdfc, float), that value is added to the **destination's** z before the difference is taken. The adjusted distance is used both for the free-hop test and for the cost.

**Port.** Use `<=`, float, 3-D, sqrt. In the AI matrix only, add the source type's Layer_Z_Adjust to the destination z. That makes W[i][j] potentially asymmetric.

## 3. Max HS speed factor (+0x130) and the heuristic

**Max HS factor** (confirmed):
- The constructor (0x3ef010) sets +0x130 to **1.0f**.
- During graph build, each route found for a pair replaces the tracker only if the route's HS_Speed_Factor is **strictly greater**.
- Rebuilds (0x3f1c10 is also called from galactic-mode start, 0x28dbe0, and from 0x3f12e0) never reset it, so it is monotonic for the lifetime of the singleton.
- Routes with HS < 1 never lower it.

**Heuristic** (confirmed):
- When a neighbour is pushed: h = `sqrtf` of the 3-D squared distance from the neighbour planet to the goal planet, divided by the tracker (float division).
- The stored f = h + g in float, with g = parent g + edge cost.
- The start entry has g = f = 0.

**Port.** Keep a monotonic `max(1.0f, every HS seen since startup)` and the float order of operations above.

## 4. A\* open list

All points are confirmed from `Find_Path` 0x3f0010 and the heap push 0x3f2340.

**Heap**
- It is a 1-based binary heap of pointers to 0x18-byte open entries: record pointer, g, f, depth.
- **Push:** append, then sift up while the parent's f is strictly greater than the new f. On equal f the new entry stays below the parent.
- **Pop:**
  - Take the root, move the last element to the root and shrink the heap.
  - Sift down: choose the left child if its f is strictly less than the current element's f. Then choose the right child if its f is strictly less than the f chosen so far. Swap and repeat.
  - Ties prefer the parent, then the left child.
- **Only f is compared.** There is no g tie-break.

**Capacity limits** (pathological cases only)
- The heap array holds 3N+1 slots. A push is silently skipped once size reaches capacity − 1.
- The path-record pool holds 3N records. Exhausting it dereferences null, so the original would crash.

**Expansion**
1. Pop. If the popped planet is the destination, succeed.
2. Otherwise set the node's closed bit.
3. For each edge, in the node's edge-vector order:
   - Skip the neighbour if it was stamped this search and is closed. **Closed nodes never reopen.**
   - Otherwise call `Edge_Cost`. If accepted, compute new g = parent g + cost.
   - Accept the new g if the neighbour has not been seen this search, or if new g is strictly less than its stored best g.
   - On acceptance, allocate a new record (planet, parent record, node index) and push a **new** heap entry. Stamp the node, store the best g and **clear** its closed bit.
   - Older heap entries for that node stay in the heap. When popped later they are expanded again with their stale, larger g, which cannot improve anything.
4. An empty heap means failure.

**Edge-vector order.** During the build, node i receives edges first from lower-index nodes (added while those nodes were the outer loop index), then to higher-index nodes. Both groups are in ascending index order, so the overall order is ascending neighbour index.

**Other behaviour**
- A request whose planets are missing from the node map fails.
- A same-planet request returns `[src]`.
- A per-search generation counter avoids clearing arrays. On wraparound to 0 it does a full reset and the counter restarts at 1.

**Port.** To reproduce paths bit-for-bit, including tie cases, replicate:
- the same heap discipline (strict comparisons, lazy duplicates, no reopen of closed nodes);
- ascending-index edge order;
- float g/f arithmetic.

Per-request scratch is fine.

## 5. Flags 0 and 1: what the predicates test

### Helper predicates
- **0x3956c0, "political control record" of an object** (confirmed):
  - For a planet it returns the data pack's `{float control, int owner id}` at +0x68.
  - For a non-planet whose owner's faction is not neutral it returns `{1.0, owner}`.
  - Otherwise it returns `{0, -1}`.
- **0x4ad9f0(record, player)** (confirmed): true when the record's owner id is not -1, **and** the player's relation table at player +0x370, indexed by owner id, is 0, **and** control > 0. In words: "held by me or an ally" (relation 0 = friendly, which includes self).
- **0x4ada70(record)** (confirmed): true when the owner id does not resolve to a player, or when the owner's faction has `Is_Neutral` (faction +0x108). In words: "unowned or neutral".
- **0x3f8b70(planet behaviour)** (confirmed read, likely meaning):
  - It returns the planet data pack's +0x1a8.
  - 0x3fa490 maintains that field **only for the local player** as a bitwise OR of `tVisibilityLevelType` contributions. The value is 0x9fff when the planet is the local player's own; per-faction defaults are 0x11 for Rebel and Underworld and 0 for Empire.
  - Bit 0 gates the ownership snapshot (when it drops, the last owner is cached at +0x9c), so bit 0 = "ownership visible". Bit 4 (0x10) = "orbiting units visible" (used by the danger test, Q6).

### `Is_Planet_Passable_For_Player` (0x3f2510)
1. If the planet is held by the moving player or an ally (0x4ad9f0), it is passable.
2. Else, if the planet is corrupted (data pack +0x2f4 != -1) and the mover's faction has `Benefits_From_Corruption` (faction +0x10b), it is passable.
3. Else, if the mover has no AI system (player +0x360 null) **or** is human (+0x62):
   - If visibility bit 0 is clear, return **flag 0**.
   - If the owner is unowned or neutral and flag 0 is set, it is passable.
   - If the owner is not unowned or neutral (i.e. a non-allied real faction) and flag 1 is set, it is passable.
   - Otherwise it is impassable.
4. Else (an AI-controlled, non-human player): evaluate the AI's perception function for this planet (planet token at data pack +0x354). The result is read as a player id.
   - If evaluation fails, it is passable.
   - If no such player exists, return flag 0.
   - If that player's faction is neutral, return flag 0; otherwise return flag 1.

### Meaning of the flags
- **Flag 0** (+0x120): "may route through planets whose ownership is unknown to me, unowned, or neutral-owned".
- **Flag 1** (+0x121): "may route through planets owned by a non-allied, non-neutral faction (enemy-held)".
- Neither flag concerns "enemy units present".
- The visibility mask belongs to the **local** player even when another player is pathing. This is a quirk to preserve for single-player parity. In multiplayer, the human branch would read the local machine's mask, which looks like a potential desync source in the original.

**Evidence.** 0x3f2510, 0x3956c0, 0x4ad9f0, 0x4ada70, 0x2823c0 (relation test), 0x3f8b70, 0x3fa490, Faction field table (`Is_Neutral` +0x108, `Benefits_From_Corruption` +0x10b).

**Confidence.** The predicates are confirmed. The meaning of the visibility bits is likely.

## 6. Edge_Cost (0x3ef4c0): exact order of checks

Inputs: hop source A, hop target B, the edge's route (may be null), the request flags. Output: accept or reject, plus a float cost.

1. **Fog gate.**
   - **Non-human player** (+0x62 == 0):
     - Gated only if flag 5 is set **and** the player has an AI system (+0x360 non-null). Both A and B must then pass `Is_Planet_Revealed` (0x3fa420).
     - `Is_Planet_Revealed` returns true in multiplayer (game type 1, 2 or 4 in the game-mode manager at +0x18). Otherwise it returns false if the planet has no data pack or its hidden byte (+0x2e0) is set. Otherwise it returns true if the galactic mode's fog-disabled byte (+0x340) is set (**correction 2026-10-28:** +0x340 is `Planet_Auto_Reveal`, not fog-disabled; GALACTIC_REVEAL_CORRUPTION.md §0), else the revealed byte (+0x2c9).
   - **Human player:**
     - In multiplayer there is no gate.
     - Otherwise B's revealed byte (+0x2c9) must be set.
     - Flag 5, the hidden byte and the fog option are ignored on this path.
   - A failed gate rejects the hop.
2. **Base cost.** Float 3-D distance A→B. If the edge has a route, cost = distance / HS_Speed_Factor (float).
3. **Flag 4.** If set and B is the request destination, accept with the base cost. No battle, passability, danger or surcharge test is applied.
4. **Active battle.** If B is in the galactic mode's conflict list (0x326ab0 over game-mode +0xa0), reject.
5. **Passable planet** (Q5 predicate true):
   - If flag 2 is set, accept with the base cost.
   - Else if `Is_Planet_Dangerous` is false, accept with the base cost.
   - Else go to step 7.
6. **Impassable planet:**
   - If flag 3 is clear, reject.
   - Else if not dangerous, accept with the base cost.
   - Else go to step 7.
7. **Dangerous fallback.** If B is corrupted (data pack +0x2f4 != -1) **and** the mover's faction has `Benefits_From_Corruption`, accept with cost + **100.0f** (a float add). Otherwise reject.

**Answers to the sub-questions**
- Flag 4 does skip the +100 surcharge, because it returns before step 7.
- Flag 2 does **not** let an impassable planet through. It applies only on the passable branch, and there it bypasses the danger test entirely, so it also bypasses the surcharge.
- A dangerous **impassable** planet is accepted only with flag 3 and the corruption condition.
- The +100 condition is not a blockade: it is "corrupted planet, and the mover benefits from corruption".

### `Is_Planet_Dangerous` (0x3f18c0)
- **The planet has an orbital starbase** (data pack +0x148, starbase level, != 0): dangerous unless the planet's control record is held by the mover or an ally (0x4ad9a0 with the mover's id).
- **No starbase, and the mover has an AI system:** evaluate the AI's danger perception for the planet token. Dangerous if the evaluation fails **or** the value is not exactly 0.0.
- **No starbase, and no AI system:**
  - If visibility bit 4 is set, iterate the units in orbit (data pack +0xc0). Dangerous as soon as one is not "held by me or an ally" under the Q5 0x4ad9f0 rule applied to the unit's record.
  - If none are found, or bit 4 is clear, it is not dangerous.

**Confidence.** Confirmed. The meaning of the conflict list is taken from GALACTIC.md.

## 7. Node_Weight (vf3, 0x693db0)

- If the finder's player has no AI system, it returns 0xFFFFFFFF.
- Otherwise it fetches the planet-target list of the AI's galactic sub-object and evaluates 0x5f2430 (the AI perception evaluator) for element **i** of that list. The **assumption** that element i is the same planet as finder index i is built into the original; it is not checked.
- **0x5f2430 sums, in double:**
  - the perception function's value for the target;
  - plus, **only if the planet is not held by the AI player or an ally** (0x4ad9f0), the float strength of the relevant opposing presence: per-object strengths (0x370e60) for objects whose type category mask (+0x1648) matches. The mask is "all" here.
- The double is **truncated toward zero to int64**.
- **In Recompute** the low 32 bits are taken as **unsigned** and divided by 100 with unsigned integer division. The result is stored as the node cost.
- There is no clamping. A negative perception total wraps (e.g. −50 gives 4294967246 / 100 = 42949672). Fractions are truncated twice: double→int, then integer division.

**Confidence.** The arithmetic is confirmed. "Perception + enemy strength" is likely; the perception token identities were not resolved.

**Port.** Compute `node = (uint32)(int64)trunc(value) / 100u`.

## 8. Edge_Weight (vf2, 0x693c30): exact arithmetic

For an ordered pair (i, j):
1. **Distance:** float 3-D distance with the Q2 z adjustment (source type's `Layer_Z_Adjust` added to the destination z when non-zero). Computed with `sqrtf`.
2. **Fog:** if the player has an AI system **and** is not human, and either endpoint fails `Is_Planet_Revealed`, return 0xFFFFFFFF (-1). The request flags are not involved.
3. **Route lookup:** `Find_Trade_Route_To(i, j)`.
   - No route and distance > max: return -1.
   - Route present: distance = distance / HS_Speed_Factor (float divide).
4. **Hop:** `hop = trunc_to_int64(distance * 10.0f) & 0xFFFF`. The multiply is in float, so the order is (distance / HS) × 10. Conversion truncates toward zero.
5. **Node cost** n = the stored node cost of the **destination** j.
   - Result = n × 65536 + hop, computed in 32-bit int and wrapping. Effectively `((n & 0xFFFF) << 16) | hop`.
   - When n == 0 the result is just hop. The original also calls an ownership helper (0x39a2c0) in that case and discards its result, so there is no behavioural effect.
6. A linked pair whose value happens to equal 0xFFFFFFFF (n & 0xFFFF == 0xFFFF and hop == 0xFFFF) is indistinguishable from "unlinked".

**Confidence.** Confirmed. That `10.0` is a float literal is read from the absence of a double promotion in the decompilation; that is likely rather than proven at instruction level.

## 9. Recompute_All_Pairs (0x6913e0)

All points are confirmed.

1. **Node pass.** For every node index in ascending order: node_cost[i] = Node_Weight(i) as unsigned, divided by 100 (Q7).
2. **Matrix initialisation**, row-major W[i·N + j] at finder +0x28 and P at +0x30:
   - Diagonal: W = 0, P = -1.
   - Off-diagonal: W = Edge_Weight(i, j). P = i when W != -1, else -1.
   - The fill order is i outer, j inner.
3. **Relaxation.** Loop order k outer, then i, then j.
   - W[i][k] is read for each j. If it is -1, skip.
   - W[k][j] is read. If it is -1, skip.
   - sum = W[i][k] + W[k][j] in **unsigned 32-bit wrapping** arithmetic.
   - If sum < W[i][j] compared **unsigned** (strict), set W[i][j] = sum and P[i][j] = P[k][j].
   - Because -1 is 0xFFFFFFFF, any finite sum beats "unlinked".
   - Wrapped sums are not detected. Large node costs can overflow and produce small "shortest" values.
4. **Meaning of P.** P[i][j] = the predecessor of j on the best path from i.

**Port.** Use uint32 arithmetic and the strict unsigned comparison. For bit-exactness, the k/i/j order matters only when ties or wraparound exist; any algorithm (e.g. N × Dijkstra) must reproduce identical W and P, including tie choices. The safest port of the semantics is the same triple loop, parallelised over i for a fixed k (rows are independent for a fixed k).

## 10. Get_Path (0x690f10)

All points are confirmed.

- The output path and weight vectors are cleared first.
- **src == dst:** path = [src, src], weights = [0, 0]. The planet is duplicated.
- **Either planet missing from the index map:** both vectors stay empty.
- **Otherwise:**
  1. Start with cur = dst and u = P[src][cur].
  2. While u >= 0 (signed): insert planet u at the **front** of the path, insert W[u][cur] at the **front** of the weights, set cur = u and u = P[src][u].
  3. The loop ends when it reaches src, because P[src][src] = -1.
  4. If anything was inserted, append dst to the path and 0 to the weights.
- **Unreachable destination** (P[src][dst] = -1): empty result.
- **Weights:** weights[k] is the post-Floyd–Warshall value between path[k] and path[k+1]. It is **per hop, not cumulative**; normally it equals that hop's edge weight. The final entry is 0.

## 11. The 90-frame cadence

All points are confirmed.

- **Per AI player.** Each `AIExecutionSystemClass` owns one finder at +0x48 (galactic type 0x690bf0 + Galactic vftable, or a land variant). It is created in 0x526120 (and lazily in 0x524c10). On creation the system calls `Initialize` and then **immediately runs one Recompute**, outside the cadence.
- **Stagger (0x526120).** Let K = (number of player slots) − (number of active human players: `+0x62` and `+0x180` set).
  - If K < 2: next_recompute = frame + 90.
  - Otherwise: next_recompute = frame + int(float(player +0x48) × (90.0f / float(K)) + 0.5f). This truncates after adding 0.5, i.e. rounds half up for non-negative values.
  - `player +0x48` is the first saved PlayerClass field; it is probably a slot ordinal (not identified).
- **Service (vf2, 0x526590).** When `next_recompute <= frame`, set next_recompute = frame + 90, then Recompute. The comparison is `>=` on the frame, so the recompute fires on the due frame.
- **Frame source.** The int at the **current game mode** object (global 0x140b15418) +0x10, or 0 if there is no mode. This is a per-mode counter. Its exact relation to the global FrameSynchronizer counter (Q12) was not verified, but both advance once per logic tick (likely).
- The lazy path (0x524c10) does not set next_recompute, which the constructor initialised to 0. So the first service call after a lazy creation recomputes again.

**Port.** Run the initial recompute at AI-system creation. Use the stagger formula with float math. Serve the results on exactly those frames, even if the computation is moved to a worker thread and double-buffered.

## 12. Simulation tick rate

The FrameSynchronizer is a global at 0x140b0a320 (reset at 0x27d730 / 0x27d020, pacing at 0x27c5f0, main loop at 0x5d990).

| field | value | role |
|---|---|---|
| +0x00 | — | the global logic frame counter |
| +0x18 | 30 | target logic frames per second used for pacing |
| +0x1c | 30 | rate, role unidentified |
| +0x20 | 30 | logic frames per second used for converting frames to seconds (sim time = frame / 30, float, passed each tick to 0x20f000 and 0x2718e0) |
| +0x24 | 1/30 (0x3d088889) | seconds per frame |

**Pacing (0x27c5f0)**
- **Single-player** (game type 0 or 3): a tick is allowed when at least 1000 / rate ms (integer division) have passed since the last tick.
  - At 30 fps that is **33 ms**, so wall-clock speed is about 30.3 frames per second.
  - If the fast flag (+0xb0b5) is set, the rate becomes **120**. If the flag at +0x37 is set, the rate is forced back to 30.
- **Multiplayer** (game type 1, 2 or 4):
  - Lockstep waits for every human peer's frame and command count.
  - It then paces at 1000 / (rate + 5) = **28 ms**, or 1000 / (rate + 10) = **25 ms** when the catch-up condition holds.
- **Fast mode** (+0xb0b5): set only in single-player by 0x2e41d0 (a UI or state transition from 1 to 2). In that mode presentation runs only when frame % 4 == 0 (0x27d800), i.e. 4× speed with render decimation.
- **Galactic vs tactical:** no separate rate constant was found. Both run on the same synchronizer at 30 Hz. How galactic calendar time scales with the galactic game-speed setting was **not** resolved.

**Confidence.** The constants are confirmed. The meaning of the fast flag and the +0x37 flag is likely.

**Port.** Fixed 30 Hz logic tick (dt = 1/30 s, sim time = frame / 30.0f) for both modes.

## 13. XML duplicate names and file list order

### GameObjectFiles.xml (0x408830 → per-file 0x408290)
- **Order.** Listed files are appended to a linked list **in list order** and parsed in that order. Duplicate list entries are parsed twice. The path is `.\Data\XML\` + name.
- **Definitions.** Every `<X Name=...>` element creates a new type object, which is appended to the type array (+0x30/+0x38).
- **Name lookup.** Types are inserted into the CRC map (+0x40; key = CRC32 of the upper-cased name at type +0x11c) **only if the key is absent**. Name lookup (0x407e00: upper-case, CRC, map) therefore returns the **first** definition.
- **Later duplicates** still exist in the type array, are counted, and are enumerable by index, but they are unreachable by name.
- **Variants.** Up to 10 extra passes re-parse the files that contain unresolved variants (types with `Variant_Of_Existing_Type` whose base has not been found yet). Each element is resolved through the name lookup, so for duplicated names the re-applied XML lands on the **first** object.

**Confidence.** Confirmed.

### TradeRouteFiles.xml (0x4715e0) and CampaignFiles.xml (0x479fe0)
Both loaders have the same structure.
1. **File list.** Each listed file name is put into a std::map keyed by the CRC32 of the name string **as written** (no upper-casing on this path; likely). Duplicate list entries collapse.
2. **Parse order.** The map is iterated in **ascending CRC order**. Files are therefore parsed in CRC order of their names, **not** list order.
3. **Entries.** Each entry is keyed by the CRC32 of the upper-cased Name.
   - On normal load, a new object is always created and the map slot is overwritten. **The last one processed wins**: it is a whole new object with defaults plus its own XML. There is no field merge, and the previous object is orphaned.
   - "Last" means the last in document order within a file, and across files the file with the higher name CRC.
   - On a reload pass (third argument set; only files with a newer timestamp are re-parsed), the existing object's fields are overwritten in place.

**Confidence.** Confirmed. The raw-name CRC for file names is likely.

**Port.**
- GameObjectTypes: first wins by name, but keep the duplicates in the index list.
- TradeRoute and Campaign: last wins.
- Sort TradeRoute and Campaign files by CRC32 of the listed file name, using the same CRC32 the engine uses everywhere (0x215a30).

## 14. VFS: MODPATH / STEAMMOD order and mod MEGs

### Command line (`Parse_Command_Line`, 0x67d70)
- The command line is tokenised left to right, and each token's keyword is upper-cased. Values keep their case.
- **`MODPATH=<dir>`:** immediately calls `Add_Mod_Search_Path` (0x214130 → 0x214940, a plain append to the search vector), then adds `<dir>DATA\ART\SHADERS\` and `<dir>DATA\ART\SHADERS\TERRAIN\` to the shader search.
- **`STEAMMOD=<id>`:** immediately calls the workshop mount (0x72210), which:
  - returns unless Steam and UGC are initialised;
  - parses the id;
  - pumps Steam callbacks until the subscribed-items query has completed;
  - finds the subscribed item with that id. If none exists, nothing is mounted.
  - Otherwise it calls the same `Add_Mod_Search_Path` with the item folder and adds the shader directories.
- So the two kinds of entry are **interleaved in command-line order** in one list.
- Because lookups try the list in registration order (SUBSYSTEMS.md, 0x214550), the earliest token wins.

### MegaFiles.xml MEGs
- The MegaFiles loader (0x217740) passes each listed name (with an empty prefix for the FoC list) to `Add_Megafile` (0x216b50).
- `Add_Megafile`:
  - skips a name that is already mounted (case-insensitive exact path string, 0x2171c0);
  - otherwise creates a MEG reader whose base object is a FileClass (0x23e0e0 → 0x213010);
  - opens it through the FileClass open virtual with "archive only" = 0 (0x23e6a0).
- A relative path such as `Data\Config.meg` therefore goes through the mod search list first. **A mod's `Data\Config.meg` is opened instead of the base one**, and the base copy is not mounted, because the same path string is already registered.
- The list file itself (`Data\MegaFiles.xml`) is opened the same way, so a mod's MegaFiles.xml replaces the FoC list.

**Confidence.** Confirmed statically. Probe D in SUBSYSTEMS.md would confirm STEAMMOD at runtime.

**Port.** Keep one ordered search list fed in token order. Resolve each MEG path through the full VFS (mod dirs first) before mounting. De-duplicate mounts by the requested path string, not the resolved one.

## Not resolved

> **Status 2026-10-30:** Three items below are answered. Perception tokens: 0x140a43c58 (the Target variable), 0x140a43c7c (`EnemyForce`), 0x140a43cfc (`HasSpaceForce`) and 0x140a43dc4 (`TargetPlayerID`) are named in the token table of GALACTIC_ECONOMY.md section 6.1 (leaf semantics in AI_GALACTIC_TOKENS.md). Frame counters: MAIN_LOOP.md section 0 item 7. MEG lookup order: in-game probe v2 confirmed that the later-listed MEG wins (`engine/vfs/include/alamo/vfs/vfs.hpp:10-14`); `Open_File` 0x219ff0 itself was still not read statically. The other items were not re-checked.

- **Planet order.** The exact population order of the GameObjectManager's planet list (+0x560), which fixes node indices: probably creation order.
- **Node_Weight index mapping.** Whether element i of the AI galactic planet-target list always matches finder index i. The original assumes it.
- **Perception tokens.** Which perception functions the tokens 0x140a43c58, 0x140a43c7c, 0x140a43cfc, 0x140a43dc4 name. These drive Node_Weight, AI passability and AI danger.
- **Stagger field.** The meaning of PlayerClass +0x48 used in the stagger.
- **Frame counters.** Whether the mode +0x10 counter equals the global synchronizer frame.
- **Galactic time scale.** How galactic game speed scales calendar time; the logic tick itself is 30 Hz.
- **Fast-mode trigger.** The exact UI meaning of the single-player fast mode (0x2e41d0) and of the +0x37 rate override (0x49b1a0).
- **MEG lookup order.** Whether MEG lookup is newest-first, given that `Add_Megafile` appends at the tail; this needs `Open_File` 0x219ff0.
