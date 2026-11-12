# Fog of war: simulation side (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by an RE agent for the code agents that implement the sim fog of war, the R5 renderer
input ("FOW grid written by the sim"), the tactical AI threat grid hook and the Lua / story bindings.
Clean-room: prose and tables only. Tags: **[H]** read directly in the decompile or disassembly,
**[M]** inferred from structure, data or call sites, **[L]** guess. RVAs are image-relative.

Builds on, and does not repeat: DECORATIONS.md §2 (renderer object, texture, palette, blur, FOW_TEX_U/V),
TERRAIN_WATER_POST.md §2.7 (terrain FOW pass), AI_TACTICAL_PERCEPTION.md §4 (threat grid, fog memory,
refog snapshot), STORY.md (event plumbing), LUA_API.md (binding RVAs), ABILITIES_HARDPOINTS.md §3.7
(stealth), TACTICAL_COMBAT.md §2.3 (targeting filter), MAIN_LOOP.md §3.3 (tick order).

---

## 0. Summary

1. Fog of war exists only in **land and space tactical modes** (mode vtable +0x240 returns 1 for
   Land/Space, 0 for Galactic; 0x17dc70 / 0x13b7d0) [H]. Galactic "fog" is the planet revealed/hidden
   bytes (PHASE2_ANSWERS Q6), not a grid. Their writers, the visibility masks and the correction that mode
   +0x340 is `Planet_Auto_Reveal` are in GALACTIC_REVEAL_CORRUPTION.md (2026-10-28).
2. Every player except the first player of a neutral faction gets **one grid of W×H cells** with three
   per-cell arrays: a **state byte**, a **u16 reveal count**, and a **fade byte** (§1). All grids of a
   mode share the same geometry and one shared "dense" mask (§1.3).
3. Reveal sources **increment** the count of the cells inside a filled midpoint circle and remember the
   cell list; moving or dying sources **decrement** with that list (§2, §3). Flash reveals write the
   state directly without counting (§2.4).
4. Every 16 frames per player (staggered by player id) the **decay** step runs: counted cells become
   "visible" (state ≥ 240), uncounted cells regrow toward 0 by a fixed step (§4). Game logic calls a
   point **fogged only when its state is 0**, so a unit stays visible for 6.4–6.9 s after the last
   source stops seeing it (`*FOWRegrowTime` 6.0 rounded to whole decay periods).
5. The transition "state > 0 → 0" raises the **refog event** into the AI threat grid (§4.3).
6. Game logic tests are `FOW_Point_Test` 0x35f3b0 and `FOW_Object_Hidden` 0x35f470 (§5). Both say
   "not fogged" for a **non-human** player while `AIUsesFogOfWar{Land,Space}` is False (shipped), unless
   the caller passes force = 1.
7. The fog is **sim state** (it filters human-owned units' targeting, special-weapon targeting, story
   events, build placement, Find_Nearest, AI target selection with force). It uses **no RNG**, it is
   **not in the frame CRC** and **not saved** (rebuilt after load) [M]. Its result is independent of
   the order in which sources are serviced within a frame (§7).

---

## 1. Grid

### 1.1 Object layout (`FOWGrid`, 0x58 bytes, ctor 0x4c0890, dtor 0x4c0ac0) [H]

| Off | Type | Meaning |
|---|---|---|
| +0x00 | u8[N] | **state** per cell (§4.1) |
| +0x08 | u16[N] | **reveal count** per cell; saturates at 0xFFFF on increment, clamps at 0 on decrement |
| +0x10 | u8[N] | **fade byte** per cell (frames-into-period at reveal; renderer only, §4.2) |
| +0x18 | u8* | shared **dense mask** (W×H bytes, owned by the mode at +0x1b0), null if none (§1.3) |
| +0x20 | i32 | N = W × H |
| +0x24 / +0x28 | i32 | W (columns) / H (rows), each clamped to 0..512 |
| +0x2c / +0x30 | f32 | actual cell width / height = extent / count |
| +0x34, +0x38, +0x3c, +0x40 | f32 | y1 (top), x0 (left), y0 (bottom), x1 (right) |
| +0x44 | i32 | mode frame of the last decay (0 = never) |
| +0x48 | i32 | owning player id |
| +0x4c | i32 | cached regrow step (−1 until the first decay) |
| +0x50 | u8 | **Reveal_All active** (set by Reveal_All, cleared by its undo) |

DECORATIONS.md §2.1 labels +0x08 / +0x10 as unknown; they are the reveal count and the fade byte.

### 1.2 Geometry [H]

- Box: the mode's map extents (mode vtable +0x1f8: centre c, half size h): x0 = cx − hx, x1 = cx + hx,
  y0 = cy − hy, y1 = cy + hy (0x35eb40). Same box as the threat grid (AI_TACTICAL_PERCEPTION §4.1).
  When all six values are 0 no grid is created at all. [H, 2026-11-01] The slot per mode:
  - **Land** (0x3b8020): the terrain (land mode +0x338, alTerrain) box 0x1320d0, cached at the first call: with
    W, H = the vertex grid size (terrain +0x10 / +0x14, TERRAIN_WATER_POST §1.3) and spacing 20 (+0x9c):
    fx = (float)W × 20, fy = (float)H × 20; centre (fx × 0.5, fy × 0.5, (zmax + zmin) × 0.5), half (|fx| × 0.5,
    |fy| × 0.5, |zmax − zmin| × 0.5) with zmin / zmax over every vertex height × 0.048828125, both started at 0.
    So the land fog box is **[0, 20·W] × [0, 20·H], the whole terrain, not the playable box** (Endor: [0, 6220]²
    against the playable (300, 1160)–(5920, 5060)). No terrain → the zero box → no grids.
    Port status (FOWF t01, 2026-11-08): `TacticalBattle::map_extents` gives this box to the fog and to the AI threat
    grid / perception map (BattleAi `cfg.map`, `w.map`; AI_TACTICAL_PERCEPTION §4.1) together. [port] A land map
    without terrain (synthetic test maps) keeps the playable box instead of the zero box. Movement destination clamps
    still use the playable box.
  - **Space** (0x4d7780): mode +0x390.., the **playable box** (.ted 0x103 minis 04/05, SPACE_AND_OPEN). (The camera
    box +0x3a8, minis 07/08, is vf +0x208.)
  - Base `GameModeClass` (0x35e390): the zero box (galactic).
- Desired cell = `DesiredLandFOWCellSize` (GameConstants +0x11c0, shipped **15.0**) on land,
  `DesiredSpaceFOWCellSize` (+0x11c4, shipped **100.0**) in space (selected by mode type 1 / 2). The ctor
  takes explicit cell sizes but the only caller passes 0 → desired sizes are used.
- **W = ceil((x1 − x0) / cell), H = ceil((y1 − y0) / cell)** (0x22f9f0 is a ceiling: `(int)v + 1` when
  `v > (float)(int)v`), each clamped to [0, 512]; then cellW = (x1 − x0) / W, cellH = (y1 − y0) / H.
  AI_TACTICAL_PERCEPTION §4.1 says "round"; it is ceil. If either cell size is not > 0, no arrays are
  allocated.
- All three arrays start at 0 (fully fogged, count 0).
- **Cell of a point** (0x4c0e00, 0x4c1000): clamp x into [x0 + 0.001, x1 − 0.001] and y into
  [y0 + 0.001, y1 − 0.001]; col = (int)((x − x0) / cellW), row = (int)((y1 − y) / cellH) (truncation;
  row 0 is the max-y edge, as the renderer's FOW_TEX_V assumes); index = row × W + col, clamped to
  [0, N − 1].

### 1.3 Grid creation (mode slot 73 `GameModeClass` 0x35eb40; Land 0x3b86f0, Space 0x4d7d10) [H]

At tactical mode start:
1. Land only: FOW-active flag 0x140a284c4 := 1 and render option 0x140a12501 := 1. Space does **not**
   set them (it only clears both when 0x140b15490 is set; land also clears them then) — see §8.
2. For every player in player-list order: skip (null slot) the player returned by 0x331cc0 (the first
   player whose faction has `Is_Neutral`, faction +0x108); otherwise allocate a grid at
   mode +0x198 [player id] (count at +0x190).
3. Re-apply saved Reveal_All: for each player id in the mode's loaded list (+0x1c0 / +0x1c8, save chunk
   0x17 in 0x35f970) call Reveal_All on that grid, then free the list.
4. Create the threat grid (0x4dcfd0) with ceil(W / k) × ceil(H / k), k = `AI_FogCellsPerThreatCell`;
   create the FOW renderer (0x7da510) and its box matrix (0x7daec0).
5. For every object with REVEAL (behaviour 0x18): mark its reveal list empty and dirty (0x537190) so it
   reveals on its next service. Unless 0x140b15490: every HIDE_WHEN_FOGGED (0x19) object is re-inited.
6. Run and clear the deferred FOW callback list (0x140b2c1e0; filled by `LuaFOWCellsClass` load, §6.4).

**Dense mask** (0x35e870, called by `Load_Map` 0x56480 after the grids exist; skipped while
0x140b15490): one W×H byte array; for every object whose type has any of `Is_Asteroid_Field` (+0x54),
`Is_Impassable_Asteroid` (+0x55), `Is_Ion_Storm` (+0x56), `Is_Nebula` (+0x57) **and** a SPACE_OBSTACLE
behaviour (0x4c), collect (no counting) the circle at position + `Space_Obstacle_Offset` (+0x16e4) with
radius `Space_Obstacle_Radius` (+0x16f0) and set those mask bytes to 1. The same pointer is stored in
every grid (+0x18). Land maps normally have none, so the mask is all 0. [H]

Destroyed with the mode (0x365540 / 0x35ad70).

---

## 2. Reveal primitive (0x4c1000) and wrappers

### 2.1 Arguments [H]

`Reveal(grid, pos, radius, outList, fill, unmaskedOK, maskedOK, apply)`:
- `fill` (only honoured when `apply`): write state 0xFF directly over the row span (no counting, no list).
- `unmaskedOK` / `maskedOK`: whether cells with dense mask 0 / 1 are taken.
- `apply`: 1 = count the cells (§2.3) and append their indices to `outList`; 0 = only append indices.

### 2.2 Shape [H]

- Centre cell (col, row) as §1.2 (clamped point). **Radius in cells r = (int)(radius / cellW + 0.5)**
  (cellW for both axes).
- Rows rowA = row − r, rowB = row + r, each clamped to [0, H − 1]. A span array of
  (clampedB − clampedA + 1) entries is initialised to (min = W − 1, max = 0).
- Midpoint circle: x = 0, y = r, d = 1 − r, dE = 3, dSE = 5 − 2r; plot8(0, r); then while x < y:
  if d ≥ 0 then y −= 1; x += 1; plot8(x, y); d += (old d ≥ 0 ? dSE : dE); dSE += (old d < 0 ? 2 : 4);
  dE += 2. (If r ≤ 0 only plot8(0, 0).)
- plot8(x, y) widens the span of row offset k (k = r ± y and r ± x) with column col ± x or col ± y — the
  usual eight octant points: offsets (r+y: col±x), (r+x: col±y), (r−x: col±y), (r−y: col±x). The span
  index is the **unclamped** offset k (0..2r); writes outside the array are dropped.
- Fill: for i = 0 .. count−1, absolute row = clampedA + i uses **span[i]**; columns clamped to
  [0, W − 1]; cells from row × W + min to row × W + max inclusive, in increasing index order.
- **Top-edge quirk** [H]: when row − r < 0, span[0] belongs to row (row − r) but is drawn at row 0, so the
  circle is shifted toward +row (toward −y) by (r − row) rows and its far part is cut. Near the bottom
  edge rows are simply cut. Ports must keep this to match.

### 2.3 Counting one cell (0x4c0f40, also Lua reload 0x6a5120) [H]

If the cell's count is 0 before the increment:
- state < 16 (fogged or almost): state := 0xEF if it was 0, else 255 − state (240..254); fade byte :=
  clamp(F − grid.lastDecay − 1, 0, 15) (0 if the grid never decayed) (0x4c0dc0);
- 16 ≤ state < 239: state := 0xFF, fade := 0;
- state ≥ 239: unchanged.

Then count += 1 unless it is 0xFFFF. Undo (0x4c19b0) is count −= 1 when > 0, no state change.
A count-reset helper (0x4c0d90) sets the listed counts to 0.

### 2.4 Wrappers [H]

| RVA | Call | Effect |
|---|---|---|
| 0x4c0ec0 | RevealDense(grid, pos, rNormal, rDense, list) | if rNormal > rDense: Reveal(pos, rNormal, unmasked only, apply), then Reveal(pos, rDense, masked only, apply); else Reveal(pos, rDense, both, apply). So **ordinary cells are revealed within rNormal and dense (nebula / asteroid / ion storm) cells only within rDense**. Both passes append to one list (the cells of the first pass and of the second never overlap) |
| 0x4c0b20 | CollectCircle | indices only (mask building) |
| 0x4c18f0 | **Flash**(grid, pos, r) | fill = 1: state := 0xFF over the circle, counts untouched, temporary list discarded → visible now, regrows from the next decay |
| 0x4c1560 | **Reveal_All** | count += 1 on every cell (same per-cell rule), +0x50 := 1 |
| 0x4c1970 | Undo Reveal_All | count −= 1 (if > 0) on every cell, +0x50 := 0 |
| 0x4c17f0 | Reset(keepState) | counts := 0; unless keepState, state := 0 and fade := 0 |
| 0x4c1830 | Binarise(g, other) | setup phase only, §3.4 |

---

## 3. Reveal sources

### 3.1 Radius of an object for a player (`GameMode` 0x35d1b0) [H]

`RevealObject(mode, player p, obj, list, force)`; does nothing outside FOW modes or without a grid for p.
- **Footprint mode** — when force = 0 and the object's owner is **not allied** to p
  (0x39a230: p's relation table entry for the owner ≠ 0; planets use their planet owner): centre =
  object position + model bounding-box centre (obj +0x2a0 vtable +0x58), r = max of the two box half
  extents (10 when the object has no model), rDense = r. Used to show an enemy object itself.
  [H, 2026-11-01] The box is ModelClass vf +0x58 = 0x264920: the render model's **local collision bound**
  (alHModel +0x120, SPACE_AVOIDANCE §3.2: bind-pose union of the collision meshes; centre 0, half 1 when the render
  object is not an alHModel), **not scaled and not rotated** (the world AABB 0x264ab0 is a different call). Centre =
  P + (c.x, c.y, c.z), r = max(h.x, h.y). Without a model the centre local is **never written** (stack garbage); the
  flash 0x365630 and Temporary_Reveal use the object position there — a port uses the position.
- **Sight mode** — otherwise:
  - base = land: `Land_FOW_Reveal_Range` (type +0x1d68) × weather factor (0x3722a0 → 0x468510, §3.1.1);
    space: `Space_FOW_Reveal_Range` (+0x1d6c). Default 0.
  - × (1 + Σ player list) where the list is the owner's tactical player data +0x1f8 (combined by
    max-within-source then sum, 0x33fb70 with `MostExtreme`/`Plus`) — the `Battlefield_Modifier_Ability`
    `FOW_Reveal_Range_Multiplier` list [M: same 0x38 spacing as the reinforcement (+0x230) and stealth
    (+0x268) lists of ABILITIES_HARDPOINTS §3.7; stored value per entry not traced, open item 2].
  - × (1 + Σ object modifier list) where the object's combat modifiers (0x395c70 merges the player's
    and the object's own) are read with `GreaterThan`/`Plus` [M: `COMBAT_MOD_FOW_REVEAL_RANGE`].
  - r = max(10, result); rDense = r × `Dense_FOW_Reveal_Range_Multiplier` (+0x1d70, default 0.5,
    clamped to [0, 1] at load); centre = object position (no model offset).
- **Battle scale** (both modes, and Lua position reveals 0x35d080): if p is the battle record's player
  at +8 (land record 0x2ac5a0) or +4 (space record 0x2ad0e0), both radii × mode +0x14. Mode +0x14 is set
  at battle start (0x35cd90): 1 + Σ `Reveal_Range_Modifier` (+0x1f94) over the other side's objects whose
  type has `Modifies_Reveal_Range` (+0x76), clamped to [0.25, 1.0] (shipped: −0.5 / −0.75 on special
  structures and a turret) [H formula, M which side is which].
- Then RevealDense(grid[p], centre, r, rDense, list).

#### 3.1.1 Weather factor (land) [H unless tagged]
Weather system 0x140b30b40: when its intensity (+0x448) > 0.5 and (no owner, or the owner's
weather-affected flag 0x282290 is true — true when the owner's timed list is empty), look up the current
weather's `Sight_Range_Modifier` map by the type's `Weather_Category` (+0x168c, −1 → 0), falling back
to category 0 ("Default"); missing → 1.0; a result ≤ 0.0001 → 1.0 (WEATHERMODIFIERS.XML, e.g.
Infantry 0.70 in some weathers). [M for the flag meaning]

### 3.2 REVEAL behaviour (`RevealBehaviorClass`, behaviour 0x18, 0x70 bytes) [H]

Fields: +0x40/+0x44 x, y at the last reveal; +0x48 squared move threshold; +0x50..+0x60 cell list;
+0x68 dirty; +0x69 has revealed once.
- **Init / load** (vf11 0x537010, vf9 0x5370e0): threshold = ((cellW + cellH) / 2)² of the **local
  player's** grid (0x35e330), 100.0 if that player has no grid; then Service(now). (Grids share geometry,
  so the value is the same on every machine except a local player without a grid — use the mode's cell
  size, open item 6.)
- **Service** (vf6 0x5373c0; base interval 1 frame, never overridden [M]):
  1. If the type has `Reveal_During_Setup_Phase_Only` (+0x58) and the land setup phase is not active
     (setup manager mode +0x1f0 → +0x30, 0x35f790) → return.
  2. If the object is garrisoned / contained (obj +0x3a7): undo the list on every player allied to the
     object (0x39a2c0, relation entry = 0, the owner included), clear the list, return. Dirty, x₀, y₀ and
     +0x69 are left as they were (an empty list returns at once) [H, 2026-11-01].
  3. Unless dirty, return when (x − x₀)² + (y − y₀)² < threshold (2D).
  4. For every player in player-list order allied to the object: undo the stored list on that player's
     grid, then RevealObject(p, obj, tmp, force = 0) (sight mode) into a temporary list cleared before
     each player. Store the **last** player's list, x₀, y₀; clear dirty; set +0x69 (also set when step 3
     returns).
  So **allies share vision**, and reveals refresh only after moving about one cell.
- **Garrison / transport exit** [H, 2026-11-01]: the eject (`0x37a8d0`, L1_S4_REMAINING §4) only clears +0x3a7
  (and resumes weapons / locomotor); nothing calls into REVEAL. The source re-reveals on the first service where
  it is one cell (the threshold) from x₀, y₀ — the position of its last reveal before it entered. An exit point
  closer than that leaves the unit unrevealing until it moves. The only writer of the dirty flag (0x537190) is
  grid creation 0x35eb40 (call graph), so nothing else forces a re-reveal either (Battlefield_Modifier list
  changes, §3.1, are picked up at each source's next re-reveal).
- **Owner change** (vf18 0x5371c0, after the first service): undo on players allied to the old owner,
  reveal for players allied to the new owner (same per-player loop), store list and position.
- **Removal** (vf15 0x537660): undo the list on every player allied to the object.
- **Attacked / fired hook** (vf26 0x537390, (obj, other)): **Flash** the object's footprint for the
  other object's owner (0x365630, r = footprint). This is how a hidden attacker becomes visible to its
  victim [M for which event calls slot 26].
- Quirk: when allied players get different radii (battle scale, player lists), the stored list is the
  last player's; undo on the others decrements the wrong cells (clamped at 0) and can leave counts
  behind. Keep it.
- Port note (a1h t01, 2026-11-02): the battle host serviced REVEAL only for battle units, so map markers and capture
  points with REVEAL (FoC `Reinforcement_Point*`: `Land_FOW_Reveal_Range` 275) never revealed for their owner. Since
  a1g's land drop check tests fog, an AI could never drop at a captured reinforcement point with no unit near it. Every
  live non-unit REVEAL object is now a source (`tactical_battle_fog.cpp`), and a capture-point owner change runs vf18.

### 3.3 Other sources [H unless tagged]

| Source | Rule |
|---|---|
| **Force Sight** (`ForceSightAbilityClass` 0x713ba0 / 0x713d20, land) | while channelling: Reveal_All for the owner, flag +0x2d4; on end: Undo Reveal_All |
| **AI firesale** (shipped Lua `BurnUnitsLand.lua`, 2026-11-02) | a land AI that wants to retreat but cannot, after 300 s, with no command centre / palace, no refinery and < 2000 credits: `FogOfWar.Reveal_All(Self)`, and in a non-multiplayer skirmish also for the local player; never undone (AI_TACTICAL §5.6) |
| **Overrun reveal** (0x364920, common tick step 3) | every 15 frames in a tactical battle with mode +0x2cf set, game type (mode +0xc) in {1, 2, 4} (the multiplayer types, the same set GMM 0x28af60 tests on GMM +0x18), `Tactical_Overrun_Multiple` (+0x15ac, 3.0) > 0, frame ≥ 30 × `Minimum_Tactical_Overrun_Time_In_Secs` (+0x15a8, 30): A = Σ `AI_Combat_Power` of record player +8's objects, B = same for +4 (0x2ab6c0: living, not flagged 0x40/0x80/0x02, counting types only, space excludes contained objects); if min(A, B) × multiple < max(A, B), the stronger player gets **Reveal_All once** (flags +0x2cd for +4, +0x2ce for +8; saved) and a speech event if it is the local player. Game types 1, 2, 4 are the networked ones; local skirmish is 3 and campaign 0 (A1_READINGS §30.3, COMMANDS.md), so **overrun never runs outside multiplayer** [H, 2026-11-01] |
| **FOW_REVEAL_PING** unit ability | spawns `Spawned_Object_Type` (shipped `Proj_Special_Land_FOW_Reveal_Ping_Blast`); the spawned object reveals through its own REVEAL behaviour [M] |
| **Stealth detection** | does not touch the grid (ABILITIES_HARDPOINTS §3.7 uses `*_FOW_Reveal_Range` as detection range only) |
| **Setup phase** | §3.4 |
| **Lua / story** | §6 |

### 3.4 Land setup phase (setup manager, mode +0x1f0) [M flow, H calls]

`SetupPhaseEnabled` (GameConstants +0x2ef) gates it (0x3c05d0). While the setup phase is active the
decay (§4) does not run and the renderer uses palette 2 (`SetupPhaseFOWColor`).
- **Setup reveal** (0x3c01e0), at start and at end: for every object (GOM order), target player = owner,
  or the record's +4 player when the type has `Reveal_For_Attacker` (+0x5a) and +4 ≠ −1, else +8 when
  `Reveal_For_Defender` (+0x5b) and +8 ≠ −1 (then force = 1). The object reveals for that player (RevealObject with
  that force, into a static list that is never undone) when it has `Reveal_During_Setup_Phase(_Only)`
  (+0x58 / +0x59), or when **force, or it has both a REVEAL behaviour and a locomotor** [H, 2026-11-01: an earlier
  reading "locomotor and (force or REVEAL)" dropped forced objects without a locomotor]. Only an object that revealed
  then checks the prevention circle [H, 2026-11-01]: `Reinforcement_Prevention_Radius` (+0x1df8) ≠ 0, **target
  player** = +8, +4 ≠ −1 and (the object has no POWERED behaviour (obj +0x341 = −1) or the **target player has base
  power**: 0x527520 on the player's tactical data, mode vf +0x220, TACTICAL_GAPS §7.3 — a per-player flag, true while
  an allied Base_Power_Ability source lives; empty list = false). Then it reveals a circle of that radius **for the +4
  player** through the position reveal 0x35d080 (battle scale applies; rNormal = radius, rDense = 0, so on land, with
  no dense mask, the full circle) and that list's counts are zeroed (0x4c0d90).
  Then, while setup continues: Binarise(grid[+8], grid[+4]) (0x4c1830) — +8's counted cells → state 0xFF,
  others → 0 (counts kept); on +4's grid every cell +8 sees is forced to state 0 / count 0, +4's counted cells
  elsewhere become state 0xFF / count 1, and its uncounted cells elsewhere state 0. **Every fade byte of both
  grids := 0** [H, 2026-11-01]. Without a record every grid is binarised alone.
- **Setup end** (0x3bfba0): behaviours are reset; every grid Reset(keepState = 0); setup reveal again;
  every grid Reset(keepState = 1) (counts 0, states kept); "end_setup" event.
- **Start / end triggers** [H, 2026-11-01 unless tagged]: setup start 0x3c05d0 runs only while the manager is
  idle (+0x30 = 0) and the global 0x140b15aef is set [M: `SetupPhaseEnabled`]; it disables the behaviours once
  (+0x31), runs the setup reveal and sets +0x30. The common tick 0x3639d0 calls 0x3bfba0 **every frame** when
  the mode has a setup manager (mode +0x1f0); it ends the phase when +0x30 is set and either +0x36 (every human
  player ready) or +0x35 (countdown armed) with the countdown timer (+0x20, 0x4ad350) expired. The ready
  handler 0x3bed70 (network command 32 `SetupPhaseTriggerEnd` via 0x5aef60, COMMANDS.md) arms the countdown on
  its first call with 0x140b169c8 [M: `SetupPhaseCountdownSeconds`, shipped 5] and sets +0x36 := the AND of
  player +0x184 (ready) over the human players (+0x62). So the first ready player starts a 5 s countdown and
  the phase ends at its expiry or as soon as every human is ready. `SetupPhaseEnabled` is False in FoC and TR.
- **Manager details** [H, 2026-11-01]: 0x140b169c8 = GameConstants +0x11c8 = `SetupPhaseCountdownSeconds` (float,
  shipped 5; 0x140b15aef = GC +0x2ef = `SetupPhaseEnabled`). The countdown (0x4ad3a0) stores the mode frame F₀ and
  (int)(30 × seconds) frames; expired (0x4ad350) when F₀ + frames ≤ F. Command 32 (0x5aef60) sets the issuer's ready
  byte, then, when the mode has a setup manager, runs 0x3bed70 — no "phase active" test; the armed flag +0x35 is never
  cleared. The end test runs at common-tick step 11 (MAIN_LOOP §3.3, after the GOM service).
- **Command 31 `SetupPhaseMove`** (execute 0x5aec70, payload id32, q1–q3, q6): with a live object of that id
  (no owner or phase test in the execute) the position and angle are dequantised, the object is placed there
  (0x3bff00: terrain snap 0x42bb40 for z and tilt, facing = the angle, forced transform flush 0x3a8f90), space
  formation members follow their container, then the manager's pick-up / drop pair 0x3bfb10 / 0x3c0020 (0) runs on
  the object's setup group: selectable again, HIDE_WHEN_FOGGED re-enabled, model shown (presentation). The drag
  validity colour (`SetupPhaseInvalidDragColor`) is client side.
- Open item 4: after setup end the counts of all sources are 0 but REVEAL behaviours only re-reveal
  after moving one cell, so stationary units' cells would regrow after 6 s. Something not traced
  (behaviour vtable +0xb8 called by 0x3bfba0) probably marks them dirty.

---

## 4. Decay (0x4c1670) and cadence

### 4.1 State encoding [H arithmetic, M names]

| State | Meaning | Game logic | Renderer (DECORATIONS §2.3) |
|---|---|---|---|
| 0 | fogged (never seen, or regrown) | **fogged** | fog |
| 1..15 | last regrow steps | visible | fading to fog |
| 16..238 | seen recently, regrowing | visible | clear |
| 239..255 | currently revealed | visible | clear (fade-in) |

### 4.2 Decay step [H]

`Decay(grid, F)` (F = mode frame):
1. step = (int)(238 / (FPS × t / 16)), FPS = 30 (0x140b0a340), t = `LandFOWRegrowTime` (+0x11cc) or
   `SpaceFOWRegrowTime` (+0x11d0) by mode type (both shipped 6.0 → step = (int)21.155… = **21**). When it
   differs from grid +0x4c, rebuild the shared 256-byte table: T[0] = 0, T[i] = max(0, i − step) for
   i = 1..238, T[239..255] = 238.
2. For each cell i in index order:
   - count = 0: s = state; state := T[s]; if s ≠ 0 and the new state is 0 and the threat grid exists,
     call **threat-grid slot 3 (grid, i)** (refog, §4.3);
   - count > 0: state := 255 − fade; fade := 0.
3. grid +0x44 := F.

So a cell left uncounted goes 239+ → 238 (first decay, 1..16 frames after the count dropped) → 217 →
… → 7 → 0 (12 more decays = 192 frames): **6.4–6.9 s** after its last count, fogged for game logic
only at 0. The fade byte only moves the renderer's interpolation inside the
16-frame window.

### 4.3 Refog hook [H]

The callback is `TacticalPerceptionGridClass` slot 3 (0x657bb0) with the FOW grid and the FOW cell
index: the threat cell containing that FOW cell takes its last-seen stamp, memory list and snapshot
(AI_TACTICAL_PERCEPTION §4.7). Grids of AI players decay too, so AI players raise it as well.

### 4.4 Where in the tick [H]

Common tick 0x3639d0 step 4 (MAIN_LOOP §3.3): in FOW modes with grids and no active setup phase, for
each player in player-list order whose **id ≡ F (mod 16)**, Decay(grid[id], F); then the fogged-model
manager (mode +0x2a0, 0x4ec2e0; presentation). REVEAL behaviours run later in the same frame inside the
GOM service (step 8). Lua and story run after the mode tick (AI T7, story T9).

---

## 5. Queries

### 5.1 `FOW_Point_Test(mode, p, pos, force)` 0x35f3b0 [H]
Returns 0 (not fogged) when the mode has no FOW, the FOW-active flag is 0, or (force = 0 and p is not
human (player +0x62) and `AIUsesFogOfWar` for the mode is False — galactic +0x2ec, space +0x2ed,
land +0x2ee, all False shipped), or p has no grid (neutral). Otherwise **fogged iff the clamped point
lies outside the box, or its cell's state is 0** (0x4c0e00).

### 5.2 `FOW_Object_Hidden(mode, p, obj, force)` 0x35f470 [H]
Returns 0 (visible) when the FOW-active flag is 0, obj is null, the object has **neither TEAM (0x16)
nor HIDE_WHEN_FOGGED (0x19)**, the object is allied to p **and** has REVEAL, p has no grid, or p's grid
has Reveal_All active (+0x50). Otherwise sample points (0x35dec0): the object position (z kept), plus,
with `Multisample_FOW_Check` (type +0x87), eight points from the object's bounding box (0x38de80 corners
via 0x20d1c0…0x20d380, z = 0). The object is **hidden iff every sample point is fogged** under §5.1
(same AI gate and force).

**The "bounding box" is the oriented 2-D footprint rectangle 0x38de80**, not the model box [H, 2026-11-01] (the same
rectangle the space static colliders use, L1_S4_REMAINING §3.3):
- θ = obj +0x8c (facing yaw, degrees) × 0.017453292; a = (cos θ, sin θ), b = (−sin θ, cos θ).
- Centre c = P + ox·a + oy·b with (ox, oy) = `Scale_Factor` × (`Custom_Hard_XExtent_Offset`, `_YExtent_Offset`)
  (type +0x658 × +0x16c4 / +0x16c8).
- Half extents (hx, hy) = 0x396850: a TEAM object with a team box uses its members' box (0x404450); otherwise the
  type's 0x371be0: `Scale_Factor` × (`Custom_Hard_XExtent`, `Custom_Hard_YExtent`) (+0x16bc / +0x16c0) when both are
  > 0; else each one that is ≤ 0 comes from |half extent| of the type's model for the current mode (0x3729f0, ModelClass
  vf +0x58 = the local collision bound, §3.1) × `Scale_Factor`, and both are 0 without a model. Deployed units
  (`Deploys` +0xd1 with the locomotor in its deployed state) use 0x371d20 (`Custom_Hard_X/YExtent_Deployed`, same
  fallback) [M for the state test].
- The eight points: c ± hx·a ± hy·b (four corners), then c + hx·a, c − hy·b, c − hx·a, c + hy·b (edge midpoints).
  Their order does not matter (all must be fogged).

### 5.3 Consumers (for reference) [H for the call, M for the role]
Targeting filter (TACTICAL_COMBAT §2.3, **force 1** per disassembly — AI players are fogged too;
TACTICAL_GAPS.md §5.8), AI target selection
(AI_TACTICAL §2, force 1), UNIT_AI fogged bytes (AI_TACTICAL_PERCEPTION §4.7), space-station special
weapons (0x386220 / 0x3822e0, force 1: they never fire at hidden targets), tactical build placement
("cannot build … under that player's FOW", 0x42edf0), `Find_Nearest` (0x69e7c0), movement block statuses
(0x6addf0, 0x739890, 0x73ce80), story FOG events (§6.5), audio (AUDIO.md), radar
(`Visible_On_Radar_When_Fogged`, UI.md).

### 5.4 Presentation (local player only, not sim) [H calls, M meaning]
`HideWhenFoggedBehaviorClass` (0x53daa0 init, 0x53ddc0 service every 30 frames or while fading) tests
`FOW_Object_Hidden(local player, obj, 0)` (skipped for objects in state 0x39b950(obj, 3), while the
debug flag 0x140b1574e is set, and during some cinematics), fades a visibility alpha with a damped spring
(instant with type +0x88), hides the model below 0.025, hides enemy objects that are `Is_Stealthed`
(ABILITIES_HARDPOINTS §3.7), and hands hidden objects to the fogged-model manager (last-seen ghosts;
`Initial_State_Visible_Under_FOW` +0x89 adds the ghost at creation). It also raises the "enemy sighted"
mode call (vtable +0x168) for types with `Play_SFXEvent_On_Sighting` (+0x8f). Because it uses the local
player, none of this may feed the sim. **Stealth never changes the grid**; stealthed units still reveal.

---

## 6. Lua and story API

### 6.1 Globals [H]
`FogOfWar` is a `LuaFOWRevealCommandClass` object (registered 0x6a51b0) with `Reveal`, `Reveal_All`,
`Temporary_Reveal`, `Disable_Rendering`. All except Disable_Rendering's flag write require a tactical
mode ("Command only valid in a tactical game"). `Enable_Fog(bool)` (0x541f10, land only) is **scene
distance fog** (RtsScene +0xbd via 0x44b0e0), not fog of war (DECORATIONS §2.8 lists it as FOW).

### 6.2 `FogOfWar.Reveal(player, target [, radius [, denseRadius]])` 0x6a5700 [H]
- Fewer than 2 args → error. Arg 1 must be a player.
- Arg 2 a **game object** and exactly 2 args → RevealObject(player, obj, cells, force = 0): the object's
  own sight rules when it is allied to the player, its footprint otherwise (§3.1). One snapshot at the
  object's current position; it does not follow the object.
- Otherwise (arg 2 any position-convertible value: object, position, marker…) a radius (arg 3, number)
  is required (missing → error; present but not a number → the original reads through a null pointer,
  treat as an error); denseRadius = arg 4 when given (must be a number, same null read otherwise), else radius; reveal at that position via 0x35d080 (battle
  scale applies, then RevealDense).
- Returns a new **`LuaFOWCellsClass`** holding the player id and the counted cell list. If no position
  can be obtained from arg 2 → error "Undable to get a valid position", nothing returned.

### 6.3 `cells.Undo_Reveal()` 0x6a5560 [H]
Decrements the listed cells on that player's grid (clamped) and empties the list, so a second call
does nothing. Garbage collection of the cells object does **not** undo (dtor 0x6a5320 frees only).

### 6.4 Persistence of Lua reveals [H]
`LuaFOWCellsClass` saves player id + cell list (0x6a5bf0) and on load registers a deferred callback
(0x362ad0) that re-counts every listed cell once the grids exist (0x6a5120, §1.3 step 6).

### 6.5 Others [H]
| Call | Behaviour |
|---|---|
| `FogOfWar.Reveal_All(player)` 0x6a5b00 | Reveal_All on the player's grid (counts stack). **Returns nothing** — shipped scripts that call `x.Undo_Reveal()` on its result (e.g. STORY_EMPIRE_ACTIV_M10_SPACE.LUA) raise a Lua error in the original. No Lua way to undo it |
| `FogOfWar.Temporary_Reveal(player, object [, radius])` 0x6a5cf0 | **Flash** at the object footprint centre (position + model box centre), radius = arg 3 if non-zero, else the larger box half extent (10 without a model). Arg 3, when present, must be a number. Returns nothing |
| `FogOfWar.Disable_Rendering(bool)` 0x6a53b0 | FOW-active flag 0x140a284c4 := **not** arg, render option := same (no arg → false → fog **off**). This turns the fog off for **game logic too** (§5.1/§5.2 return "not fogged", UNIT_AI stops updating fogged bytes), not only for rendering |

### 6.6 Story events (`StoryEventFogRevealClass`, types 0x1f / 0x20) [H]
- **STORY_FOG_POSITION_REVEAL** (0x20): Event_Param1 is a list of three numbers (x, y, z; list entries
  0, 1, 2 parsed with atof into +0x360/+0x364/+0x368, vf2 0x465b80). Polled every T9 (0x3271d0); fires
  when `FOW_Point_Test(plot player, pos, force 0)` is **not fogged** (log "Position (%f,%f,%f)
  unfogged").
- **STORY_FOG_OBJECT_REVEAL** (0x1f): Event_Param1 = one object type name (+0x370, invalid name logged).
  Each T9: for every object of that type in the current mode (0x2a87d0), if `FOW_Object_Hidden(plot
  player, obj, 0)` is 0 → Fire (log "Object %s unfogged"). Fire may run once per matching object in the
  same poll.
- Consequences of §5: both fire at once when FOW is disabled, for non-human plot players (AI gate), for
  points/objects revealed in the last ~7 s, and the object form fires at once for types without TEAM or
  HIDE_WHEN_FOGGED. No shipped FoC event uses either type.
- Story reward **ENABLE_FOW** (reward 0x1c, 0x456970): no Reward_Param1 → FOW-active := 0; atoi(P1) = 0 →
  FOW-active := 0 and render option := 0; else both := 1. Galactic rewards REVEAL_PLANET (51),
  REVEAL_ALL_PLANETS, ENABLE_GALACTIC_REVEAL act on planet flags (GALACTIC.md), not on grids.

---

## 7. Determinism, CRC, save, threading

- **No RNG** in any FOW function read here [H]. (Behaviour load randomises service phase through the
  base behaviour 0x4c2f90; REVEAL has interval 1, so the draw is from a range of size 0 [M].)
- **Order independence** [H by construction]: a counted cell always has state ≥ 239 (reveal sets ≥ 239,
  decay sets 255 − fade ≥ 240, flash sets 255), so the state rewrite in §2.3 is a no-op whenever the
  count was > 0; increments and clamped decrements commute except when a decrement hits 0 before an
  increment in the same frame, and both orders then leave state ≥ 239. Sources may therefore be applied
  in any order within a frame, and per-player grids may be updated in parallel (per-index writes).
  Keep the decay's refog callbacks in cell-index order per grid and grids in player-list order.
- **Float math**: cell sizes, cell indices (truncating divides), r = (int)(R/cellW + 0.5), radius
  multipliers and the step formula must match bit for bit (no FMA).
- **CRC**: the frame CRC is store + sync RNG + players (OBJECT_RUNTIME §5.1); the grids are not part of
  it [M]. A divergence shows up indirectly through targeting.
- **Save**: grids are not saved [M: no grid chunk in mode save/load 0x35f970]; saved are the Reveal_All
  player list (chunk 0x17), the overrun flags, the LuaFOWCells objects and the REVEAL behaviours (which
  re-reveal when dirty). After load every cell starts at state 0.
- **Renderer contract** (R5): per frame, for the local player's grid, hand the renderer the state array,
  the fade array, the grid's last-decay frame (for f = clamp(F − last − 1, 0, 15)) and the regrow step
  (grid +0x4c); DECORATIONS §2.3 converts them. The renderer never writes the grid.

---

## 8. Corrections to other docs

1. AI_TACTICAL_PERCEPTION §4.1: FOW cell counts use **ceil** (0x22f9f0), not round.
2. AI_TACTICAL_PERCEPTION §4.7: the space mode does **not** clear the FOW-active flag in normal play; it
   only clears it (with the land mode) while 0x140b15490 is set. The flag's static initial value is 1;
   land sets 1 at every battle start; space keeps the last value, so a `Disable_Rendering(true)` or
   ENABLE_FOW 0 from an earlier space battle stays in force in the next space battle.
3. DECORATIONS §2.1: grid +0x08 = u16 reveal count, +0x10 = fade byte, +0x18 dense mask, +0x4c regrow
   step, +0x50 Reveal_All flag. §2.8: the Lua names are `FogOfWar.*`; `Enable_Fog` is scene fog.

4. (2026-11-01) The FOW **draw** is gated by its own byte, the render option 0x140a12501 (set by `0x13ecc0`, read by
   `0x13ecd0`), not by the FOW-active flag 0x140a284c4: every cinematic shot start (`0x49b1a0(camera, 0)`) clears it and the
   cinematic end restores it, so no fog is drawn during a cinematic and in the main-menu attract demo; the sim (§5) still uses
   the grids. MAIN_MENU_BACKGROUND.md §4a.

---

## 9. Open items

1. ~~Which events call REVEAL slot 26~~ — resolved [H]: only firing (post-shot hook 0x3a06a0), on the target
   owner's grid. TACTICAL_GAPS.md §5.1.
2. ~~Value stored in the +0x1f8 list~~ — resolved [H]: multiplier − 1, so the (1 + Σ) formula is right.
   TACTICAL_GAPS §5.2.
3. ~~Record slots~~ — resolved [H]: +4 attacker, +8 defender; land scales the **defender** by the attacker's
   `Modifies_Reveal_Range` objects, space the attacker by the defender's. TACTICAL_GAPS §5.3.
4. ~~Post-setup re-reveal~~ — resolved [H]: setup end only re-enables behaviours (+0xb8 = Enable); nothing marks
   REVEAL dirty, so stationary cells regrow (quirk; setup phase is off in FoC and TR). TACTICAL_GAPS §5.4.
   Also: the targeting filter calls 0x35f470 with **force = 1** (§5.3 "force 0" is wrong for targeting):
   TACTICAL_GAPS §5.8.
5. Fogged-model manager (0x4ec2e0, 0x4eb020) ghost rules, and `HideWhenFogged` state 0x39b950(obj, 3).
6. REVEAL threshold when the local player has no grid (spectator / neutral): 100 instead of the cell
   size; harmless only if no such machine exists in MP.
7. ~~Space reinforcement FOW sampler and palette 3~~ — resolved [H]: render-only prevention-radius overlay; the
   sim rule is SpaceMode vf80. TACTICAL_GAPS.md §5.5 (drop validity §4.2).
8. Probe (Gabriel launches; kit under tools/probe_mod/fow_regrow/): a land skirmish script that every 15 frames logs
   `FogOfWar`-independent visibility of an enemy unit walking out of sight (via a STORY_FOG_OBJECT_REVEAL
   on a human plot player), to confirm the ~6.9 s regrow window and the top-edge circle shift.
9. ~~Garrison exit path, binarise of the +4 grid, setup-phase end trigger, overrun game types~~ — resolved
   2026-11-01 [H]: §3.2 (exit does not mark dirty), §3.4 (fade bytes zeroed; start / end triggers), §3.3
   (multiplayer only).
10. ~~Port stand-ins (FOWF t01–t05)~~ — resolved 2026-11-01 [H]: map extents per mode (§1.2: land = whole terrain),
    footprint box = local collision bound (§3.1), Multisample = oriented Custom_Hard rectangle (§5.2), setup "powered"
    = the target player's base power and the corrected reveal condition (§3.4), setup manager countdown / commands 31,
    32 (§3.4). **GOM list +0x2f0** (list object +0x2e8, used by the prevention test 0x4d8d10 and the land drop
    validity): every object registered in the store (Create_Object 0x29f810, re-registration 0x2b5ad0 on load) whose
    type has `Reinforcement_Enabling_Radius` (+0x1e20) > 0, `Reinforcement_Prevention_Radius` (+0x1df8) > 0 or
    `Base_Shield_Radius` (+0x21f4) > 0, appended in creation order; removed only by Remove_Object 0x2a6a00 at the
    store flush. No alive / owner test on membership: dead objects stay until their flush, and death clones are members
    when their own type qualifies. The prevention test filters radius > 0 and non-allied owners (0x39a230), 2-D, strict <.

---

## 10. Exact rules for code agents (short report)

1. One grid per non-neutral player in tactical modes; W = ceil(extent / 15 or 100), clamp 512; arrays
   state u8, count u16, fade u8, all 0 at start; cell = (int)((x − x0)/cw), (int)((y1 − y)/ch) after a
   0.001 inset clamp; index clamped.
2. Reveal = filled midpoint circle of r = (int)(R/cw + 0.5) cells with the unclamped span index quirk;
   counted reveal per cell: if count was 0 then (state 0 → 239; 1..15 → 255 − s; 16..238 → 255);
   count++ (sat). Undo: count−− (clamp 0). Flash: state = 255 only.
3. Dense cells (space obstacles with nebula / asteroid / ion storm flags) use rDense =
   r × Dense_FOW_Reveal_Range_Multiplier.
4. Object sight radius = max(10, base × weather × (1 + player FOW list) × (1 + object FOW mods)) ×
   battle scale; enemy targets of a Reveal get their footprint only.
5. REVEAL behaviour: allies share vision; refresh on owner change, death, garrison, and when moved
   ≥ one average cell (squared 2D test); undo old list, reveal new, per allied player in list order.
   Leaving a garrison / transport does not force a refresh: the next one comes when it is one cell from its
   last reveal position.
6. Decay for player p when p.id ≡ F (mod 16), at common-tick step 4, before object services:
   count > 0 → state = 255 − fade, fade = 0; else state = T[state] with step (int)(238/(30·6/16)) = 21;
   a nonzero → 0 transition calls the threat grid refog for that cell.
7. Fogged ⇔ state == 0 (or outside the box); object hidden ⇔ all samples fogged, and never for objects
   without TEAM/HIDE_WHEN_FOGGED, allied REVEAL objects, Reveal_All grids, disabled FOW, or (force 0)
   non-human players while AIUsesFogOfWar is False.
8. Lua: Reveal returns an Undo-able cells object (snapshot); Reveal_All returns nothing and cannot be
   undone from Lua; Temporary_Reveal flashes the footprint; Disable_Rendering(b) sets FOW-active = !b
   for sim and render. Story FOG events poll in T9 and fire on "not fogged" for the plot's player.
9. No RNG, not in the CRC, not saved; recompute after load from REVEAL behaviours (dirty), saved
   Reveal_All players and LuaFOWCells.
