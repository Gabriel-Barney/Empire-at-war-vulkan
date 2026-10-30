# Galactic connectivity: trade routes, planet adjacency and galactic path finding

Clean-room behavioural description from the Ghidra 12.2 pass. Function names are the ones saved in the project (`docs/re/ghidra/renames_pass3.csv`). RVAs are relative to image base 0x140000000.

## 1. Data model

| object | where | layout (see STRUCTS.md) |
|---|---|---|
| `TradeRouteClass` | one per XML `<TradeRoute>`, owned by TradeRouteManager | `+0x28` Point_A, `+0x38` Point_B (planet names), `+0x48` Political_Control_Gain, `+0x4c` Credit_Gain_Factor, `+0x50` **HS_Speed_Factor**, `+0x58` Visible_Line_Name |
| planet node | a GameObject whose `+0xb8` is a `PlanetaryDataPackClass` | `+0x78/+0x7c/+0x80` galactic position |
| route links (adjacency) | `PlanetaryDataPackClass+0x58` (DynamicVector), count `+0x60`, capacity `+0x64` | 0x20-byte `TradeRouteLinkEntryClass`: `+0x00` route key/CRC, `+0x08` TradeRouteClass*, `+0x10` neighbour object id, `+0x18` neighbour planet GameObject* |
| route lines (visuals) | TradeRouteLineManager (TradeRouteLineClass, `TradeRouteLines.xml`) | rendering only |

The adjacency is undirected and duplicated: `PlanetaryBehaviorClass::Create_Trade_Route_Link` (0x3f5e10) appends an entry to both planets' link arrays. It skips the add if the neighbour is already linked (unless forced) and logs `### Created trade route link between %s and %s`.

`PlanetaryBehaviorClass::Find_Trade_Route_To(a, b)` (0x3f95a0) is a linear scan of a's links, matching `+0x18 == b`. It returns the TradeRouteClass* or null. With typically fewer than 8 links per planet this is cheap, but it is called O(N²) times by the AI path finder.

## 2. Building from XML

1. **System init** (0x4b250, the database bootstrap): `TradeRouteManager.System_Initialize` loads `.\Data\XML\TradeRouteFiles.xml`. `Create_Trade_Routes_From_XML` (0x4715e0) parses each listed file, creates one `TradeRouteClass` per entry keyed by name CRC (`--- Created new TradeRouteClass for key '%s' - CRC %u`) and fills fields through `TradeRouteClass::Parse_Database_Entry` (0x5c3840), driven by the TradeRoute field table (`xml_field_tables.md`). `TheTradeRouteLineManager` loads `TradeRouteLines.xml` (0x467740).
2. **Galactic map load:** the campaign's `<Trade_Routes>` list names which routes exist. For each one, Point_A and Point_B are resolved to planet objects and `Create_Trade_Route_Link` is called. Fix-ups on load or story changes are logged by `PlanetaryBehaviorClass::vf11` (`- Successfully fixed up trade routes for planet %s (route %s)`). Story rewards can add or remove routes at runtime, which re-calls the link functions.
3. **Path finder graphs** are derived lazily from the links (sections 3 and 4). They are not stored in save games; rebuilding them is the cost of changing connectivity.

## 3. Fleet movement path finding: `GalacticPathFinderClass` (A*)

- **Entry points:** `MovementBlockStatus::Get_Next_Hop` (0x6aedc0), `FreeStoreMovementBlockStatus::Get_Next_Hop` (0x71f710) and the player's fleet move orders. Each builds a request with `Path_Request_Init` (0x3eefd0) holding player, source, destination and flags, then calls `GalacticPathFinderClass::Find_Path` (0x3f0010) on a lazily created singleton.
- **Graph** (`Build_Planet_Graph`, 0x3f1c10), built on first use:
  1. Query all planets from the GameObjectManager (category 3). The query walks the manager's planet list and keeps objects that have a PLANET behaviour slot and an owner id other than -1. `In_Background` is not tested, so `Galaxy_Core_Art_Model` is excluded only because its sole behaviour is IDLE. (corrected 2026-10-26, see PHASE2_ANSWERS Q1)
  2. Wrap each planet in a 0x20-byte node (vector at `+0x40`; planet→index map at `+0x00`).
  3. For every unordered pair, compute the Euclidean distance (plain 3-D float sqrt) and look up `Find_Trade_Route_To`. Add an edge if a trade route exists or the distance is `<=` **`GameConstants.MaximumFleetMovementDistance`** (inclusive; corrected 2026-10-26, see PHASE2_ANSWERS Q2) (GameConstants singleton at 0x140b15800, field offset 0x108; vanilla `GAMECONSTANTS.XML` = 100.0, Hyper/AITest constants = 150.0; also copied into the AI finder at `+0xa8`).
  4. Track the largest HS_Speed_Factor seen at `+0x130`. The constructor sets it to 1.0f. It is replaced only by a strictly greater route factor and is never reset on rebuild, so it is monotonic for the singleton's lifetime. (corrected 2026-10-26, see PHASE2_ANSWERS Q3)
  - Edges are 16-byte `{neighbour node, route}` pairs in per-node vectors, in ascending neighbour-index order.
  - Cost: O(N²) distance + route lookups once per graph build.
- **Search:**
  - Textbook A*:
    - binary min-heap open list at `+0x78`, size at `+0x90`;
    - node records from a pool at `+0x58` (0x18 bytes each);
    - per-planet best g at `+0xb8`;
    - closed bitset at `+0xd0`.
  - A per-search **generation stamp** at `+0x128`, compared against per-planet stamps at `+0xf0`, avoids clearing arrays. On wraparound it does a full reset (0x3f21e0).
  - Heuristic: straight-line distance to the goal divided by the maximum route speed factor (`+0x130`), so it stays admissible. f = g + h, all in float.
  - **Heap discipline** (corrected 2026-10-26, see PHASE2_ANSWERS Q4):
    - Ordering: the heap compares f only, with no g tie-break.
    - Push: sift up while the parent's f is strictly greater.
    - Pop: move the last element to the root, then prefer the left child, then the right child, each only if strictly smaller.
    - Goal test: on pop.
    - Closed nodes: never reopened.
    - A g improvement pushes a duplicate entry. Stale entries stay in the heap and are re-expanded harmlessly.
    - Capacity: the heap holds 3N+1 slots and pushes beyond it are silently dropped. The record pool holds 3N; exhausting it would crash the original.
  - Output: a vector of planet GameObject* from source to destination. A same-planet request returns `[src]`.
- **Path request** (0x20 bytes, `Path_Request_Init` 0x3eefd0): `+0x00` player, `+0x08` source, `+0x10` destination, then six bool flags at `+0x18..+0x1d`. All default to 1. The finder copies them to `+0x120..+0x125`.

  | flag | finder off | meaning (from usage) | set by fleet orders (`MovementBlockStatus+0x58` = move mode m) |
  |---|---|---|---|
  | 0 | +0x120 | may route through planets that are unowned, owned by an `Is_Neutral` faction, or whose ownership is hidden from the local player (visibility bit 0 clear) (corrected 2026-10-26, see PHASE2_ANSWERS Q5) | m == 5 |
  | 1 | +0x121 | may route through planets owned by a non-allied, non-neutral faction, i.e. enemy-held (corrected 2026-10-26, see PHASE2_ANSWERS Q5) | m == 5 |
  | 2 | +0x122 | **skip the danger check** on passable planets only; it does not let impassable planets through (corrected 2026-10-26, see PHASE2_ANSWERS Q6) | m == 5 |
  | 3 | +0x123 | **allow transit through impassable (hostile) planets**, provided the danger check passes | m > 2 |
  | 4 | +0x124 | **always accept the destination planet**, even if hostile, dangerous or blockaded | m > 1 |
  | 5 | +0x125 | **fog rule**: both hop endpoints must be revealed to the player (`PlanetData+0x2c9` revealed, `+0x2e0` hidden, unless fog is disabled by the game option at mode+0x340; **correction 2026-10-28: mode+0x340 is `Planet_Auto_Reveal` (default true), and the human hop gate reads the raw byte, see GALACTIC_REVEAL_CORRUPTION.md §1.1**) | 1 only for players carrying a specific perception token; otherwise as initialized |

  The AI-side request builder (0x665160) derives the same flags from its own mode (0..5): flag0 = flag1 = (m == 5), flag2 = (m == 1 or m == 5), flag3 = (m > 2), flag4 = (m > 1) (corrected 2026-10-28, see FIXPASS_GALACTIC.md §1; the earlier reading had flags 2 and 4 swapped).
- **Edge cost** (`Edge_Cost`, 0x3ef4c0). The checks run in this order (corrected 2026-10-26, see PHASE2_ANSWERS Q6):
  1. **Fog gate.** `player+0x62` is **Is_Human**.
     - **Non-human player:** gated only when flag 5 is set and the player has an AI system (`+0x360`). Both endpoints must then pass `Is_Planet_Revealed` (0x3fa420), which is always true in multiplayer.
     - **Human player:** in single-player the hop target's revealed byte (`+0x2c9`) must be set. There is no gate in multiplayer.
  2. **Base cost:** float `distance / route.HS_Speed_Factor`, or plain distance with no route.
  3. **Destination short-circuit (flag 4):** the destination is accepted at base cost with no further checks.
  4. **Planets in an active battle are rejected** (0x326ab0 scans the game modes' conflict list).
  5. **Passability:** `Is_Planet_Passable_For_Player` (0x3f2510) decides. A planet is passable when:
     - it is held by the player or an ally (relation 0 and control > 0); or
     - it is corrupted (data pack `+0x2f4 != -1`) and the faction has `Benefits_From_Corruption` (`+0x10b`); or
     - flags 0/1 allow it (see the table). AI-controlled players use a perception-estimated owner instead of the visibility mask.
  6. **Danger check:** `Is_Planet_Dangerous` (0x3f18c0).
     - If the planet has a starbase (data pack `+0x148`), it is dangerous unless the player or an ally holds the planet.
     - Otherwise, for players with an AI system, it is dangerous if the AI perception evaluation fails or returns non-zero.
     - Otherwise it is dangerous if a visible orbiting unit is not friendly.
     - Passable planets are accepted if flag 2 is set or they are not dangerous. Impassable planets need flag 3 and must not be dangerous.
  7. **Corruption surcharge (was described as a "blockade penalty"):** a planet that failed the danger check is still accepted at cost + 100.0f (float) when it is corrupted (`+0x2f4` = corruption type, not owner) and the mover's faction has `Benefits_From_Corruption` (`+0x10b`). Otherwise it is rejected.
- **Cost:** O((N+E) log N) per request. It is serial and mutates singleton scratch state, so it is not re-entrant. A port should give each request its own scratch, which makes it parallel-safe.
- **Movement timing:** `Get_Fleet_Hyperspace_Route_Speed_Factor` (0x431c80) multiplies hop travel time by the route's HS_Speed_Factor and by fleet-composition modifiers (logs `Fleet HS trade route speed factor (%s to %s) = %.2f`).

## 4. AI travel planning: `AIExecutionPathFinderClass` / `GalacticExecutionPathFinderClass` (all-pairs)

- **Owner:** each AI player's `AIExecutionSystemClass` (reached through `player+0x360`).
- **`Initialize`** (vf1, 0x693e20): enumerates the planets into an array of 16-byte entries at `+0x08`, builds a planet→index map at `+0x68`, and allocates two N×N int matrices: weights/hops at `+0x28` and predecessors at `+0x30`. The maximum free-hop distance goes to `+0xa8`.
- **`Recompute_All_Pairs`** (0x6913e0), a **Floyd–Warshall** pass:
  1. Node weights: `Node_Weight` (vf3, 0x693db0) evaluates the AI's perception function for every planet, i.e. N perception evaluations.
     - The value is the perception value plus enemy strength when the AI does not hold the planet, in double.
     - It is truncated to int64, its low 32 bits are taken unsigned, and it is divided by 100 with unsigned integer division.
     - No clamping; negative values wrap. (corrected 2026-10-26, see PHASE2_ANSWERS Q7)
  2. Edge weights: `Edge_Weight` (vf2, 0x693c30) for every ordered pair. (corrected 2026-10-26, see PHASE2_ANSWERS Q8)
     - hop = truncate((distance [/ HS_Speed_Factor]) × 10.0f) masked to 16 bits, all in float.
     - The value is `destination_node_cost × 65536 + hop` in wrapping 32-bit arithmetic.
     - Distance adds the **source** planet type's `Layer_Z_Adjust` (`type+0xdfc`) to the destination's z, so the matrix can be asymmetric.
     - The free hop is inclusive: a pair counts as unlinked only when it has no route and distance > max.
     - The edge is -1 when the pair is not linked, or when a non-human AI player has an unrevealed endpoint.
  3. Matrix setup (corrected 2026-10-26, see PHASE2_ANSWERS Q9):
     - Diagonal: weight 0, predecessor -1.
     - Linked pairs: the predecessor is the row index. Unlinked pairs: -1.
  4. Relax with k outer, then i, then j, so N³ compare/adds (corrected 2026-10-26, see PHASE2_ANSWERS Q9):
     - A step is skipped when either half is -1.
     - The sum is unsigned 32-bit and wraps.
     - The update happens when the sum is strictly less than the current value, compared unsigned. It then sets `pred[i][j] = pred[k][j]`.
  - **Schedule:** `AIExecutionSystemClass` vf2 (0x526590, its Service) calls this whenever `frame >= next_recompute`, then sets `next_recompute = frame + 90` (the frame counter lives in the game-mode object at `+0x10`). So it runs **every 90 frames for every AI player**.
    - **First runs and stagger** (corrected 2026-10-26, see PHASE2_ANSWERS Q11):
      - Creating the finder (0x526120) immediately runs one recompute, outside the cadence.
      - The first scheduled recompute is staggered across AI players: `frame + int(float(player+0x48) × (90.0f / K) + 0.5f)`, where K = player slots − active humans. When K < 2 it is `frame + 90` instead.
  - **Cost:** O(N³) + O(N²) edge evaluations (each with a `sqrt` and a linear route scan) + O(N) perception evaluations. With ~200-planet campaigns (Thrawn's Revenge) that is ~8M inner iterations per AI per recompute.
  - **Not covered by the installed patch.** It is a prime suspect for the periodic galactic-map hitches.
- **`Get_Path`** (0x690f10, used by Lua `Find_Path` 0x69da70 and the AI movement code): walks the predecessor matrix back from the destination. It builds the planet list by inserting at the front, which is O(L²) memmove for path length L, and it also returns weights in a parallel vector (corrected 2026-10-26, see PHASE2_ANSWERS Q10):
  - The weights are **per hop, not cumulative**: entry k is the all-pairs value between path[k] and path[k+1], and the final entry is 0.
  - src == dst returns `[src, src]` with weights `[0, 0]`.
  - An unreachable or unknown planet returns empty vectors.
- **Port notes:**
  - The matrix is a pure function of (planet set, links, fog state, AI perception values), so it can be computed on a worker thread and double-buffered.
  - Floyd–Warshall can be replaced by N Dijkstra runs over the sparse graph (E ≈ 3N), roughly O(N² log N).
  - Recompute only when ownership, fog or links change, but **preserve the 90-frame cadence semantics** for lockstep: results must be identical at the frames where the AI reads them.

## 5. Other consumers

- **Perception tokens:** `TradeRoutes`, `ActiveTradeRoutes`, `IsConnectedTo` and `IsConnectedToCorruption` (registered at 0x5b43d0) read the link arrays.
- **Lua:** `Find_Path(player, from, to)` (galactic mode only; non-AI players are rejected) returns the AI finder's path. `GalacticFreeStore.Move_Object` and `TaskForce.Move_To` drive `MovementBlockStatus` → A*.
- **UI:** trade-route income panels (`IDC_TRADE_ROUTE_INCOME_%d`).

## 6. Open items

- The meaning of flags 0 and 1 is now resolved statically (corrected 2026-10-26, see PHASE2_ANSWERS Q5):
  - 0x3f8b70 reads the local player's visibility bitmask, not an "enemy presence" bit.
  - 0x4ad9f0 tests "held by me or an ally".
  - 0x4ada70 tests "unowned or neutral".
  - A runtime check is still useful: A runtime check: order a fleet through an enemy-held planet with Lua `TaskForce.Move_To` in each move mode and log the returned path.
