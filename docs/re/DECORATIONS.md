# Terrain decorations: fog of war, decals, shadow blobs, tracks (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent. Builds on `docs/re/RENDER.md` and `docs/re/TERRAIN_WATER_POST.md` (§1.3
terrain chunks 6/9, §2.6 submission order, §2.7 FOW) and does not repeat them. Clean-room: prose and
tables only. Addresses are RVAs in StarWarsG.exe; Ghidra often files this code under `LandModeClass` or
`Global`, so the address is authoritative.

Confidence tags: **[H]** read directly in the code and cross-checked against data, **[M]** read in the
code but intent inferred, **[L]** inferred, needs a capture.

Data validation: all 439 `.ted` files in FoC `maps.meg`/`Patch*.meg` and EaW `maps.meg`/`patch*.meg` of the
install (`F:\SteamLibrary\steamapps\common\Star Wars Empire at War`) were scanned in memory (scratch script,
nothing extracted into the game folder). Counts in §6.

---

## 0. Summary for the implementer

1. **Five decoration types, all drawn by `alD3dTerrainDecorationRenderTask` (vf2 0x1946f0, geometry fill
   vf3 0x194ef0)**: 0 = **shadow blobs** (`alTerrainShadowBlob`), 1 = **decals** (`alTerrainDecal`, derives
   from the blob class), 2 = **dynamic tracks** (`alDynamicTerrainTrack`, unit trails), 3 = **map tracks on
   land** (`alTerrainTrack` whose height mode ≠ 0), 4 = **fog of war**, 5 = **map tracks on the water plane**
   (`alTerrainTrack` with height mode 0, bucket 7 after the water). This settles TERRAIN_WATER_POST open
   question 3. [H] (0x18ee70, 0x194080, RTTI)
2. **Order in bucket 0 (normal passes only)**: terrain patches, then decoration 3, 2, 1, 0, 4 in submission
   order (decoration tasks are not standard-effect tasks, so the effect batching of RENDER.md §1.4 does not
   reorder them). Type 5 goes to bucket 7 after the water task. [H] submission, [M] that nothing reorders.
3. **Depth trick**: types 0–4 copy the camera, add **0.3 to the near and the far plane**, rebuild the
   projection, draw, and restore the camera. Type 5 does not. Geometry is also physically lifted: blobs
   +0.1, decals +1.0, dynamic tracks +0.1, water-plane tracks waterH+1. [H] (0x1946f0, 0x191720, 0x1924a0)
4. **Fog of war** is a W×H 32-bit texture, **one texel per FOW cell** (cell = `DesiredLandFOWCellSize` 15
   world units on land, `DesiredSpaceFOWCellSize` 100 in space, W and H = ceil(extent / cell) clamped to
   512), covering the **playable box** of the map. Texel = palette[intensity], palette = per-channel lerp
   between a fog colour and a visible colour. Land: visible = white, fog = `LandFOWColor` (130,130,130,0).
   The terrain pass multiplies the frame buffer by it (`TerrainFogOfWar`, **DESTCOLOR/ZERO confirmed**).
   [H] (0x7da510, 0x7db070, 0x7dbc70, 0x4c0890, FXO)
5. **FOW_TEX_U = (1/(x1−x0), 0, 0, −x0/(x1−x0)), FOW_TEX_V = (0, −1/(y1−y0), 0, y1/(y1−y0))** with
   (x0,y0)–(x1,y1) the playable box; **V is flipped** (v = 0 at the max-y edge). [H] (0x7daec0)
6. **Decals use one atlas, `D_master_decal.tga`**, a √N × √N grid of slots (N = `Master_Decal_Slot_Count`,
   default 16 → 4×4). Geometry = the terrain triangles under the decal rectangle, **clipped to the
   rectangle's four edges** (Sutherland–Hodgman), lifted 1.0, lit vertices; UV = decal-local position mapped
   into the chosen slot. Fade = linear over `Fadeout_Time`. Draw: one batch per render mode (14 modes,
   ascending), texture WRAP. [H] (0x150760, 0x14fb60, 0x1946f0)
7. **Map tracks are spline ribbons** placed in the editor (shore waves, rivers, lights, girders): control
   points + per-point width, a cross-section alpha profile, a texture with UV scroll, drawn as lit
   triangles with render mode DIFFUSE_ALPHA. 491 tracks in 23 FoC maps incl. Patch2 (vanilla MP: Bothawui, Mandalore,
   Bespin, Kamino, Naboo, Dantooine, Utapau). [H] (0x1547a0, 0x156c20, 0x154f00; data)
8. **Dynamic tracks** come from `DynamicTrackFX.xml`: points dropped every `segment_length` behind a unit,
   a ribbon of ±`width` around them conforming to the terrain, alpha fading with distance behind the head
   (smoothstep from `fade_begin_distance` to `fade_end_distance`); the head distance also grows by
   `fade_distance_per_second` × dt so standing tracks fade out. [H] (0x159970, 0x158ab0, 0x159100)
9. **RENDER.md §7.1 q7 (secondary top-down pass, alSimpleScene +0x268/+0x270) is a debug view**: it draws
   the cull frustum and the scene cull-tree node boxes as coloured wireframe. Not a decoration pass;
   not needed. [H] (0x1b2ae0 tail, 0x1b4120)

---

## 1. Common machinery

### 1.1 Render modes (`Render_Mode` strings) and prim effects

The decal, blob, dynamic-track and track code all use the engine "prim mode" table (prim draw 0x180a40 for
lit vertices, 0x1805b0/0x180730 for pre-lit; mode switch at 0x182c00 region). String → mode is 0x427740.
[H]

| mode | string | effect | blend (FXO) | vertex colour use |
|---|---|---|---|---|
| 0 | `OPAQUE` (also any unknown string) | `Engine\PrimOpaque` | opaque | |
| 1 | `ADDITIVE` | `Engine\PrimAdditive` | ONE/ONE | colour × texture |
| 2 | `ALPHA` | `Engine\PrimAlpha` | SRCALPHA/INVSRCALPHA, FF, lighting off | colour × texture, alpha × alpha |
| 3 | `MODULATE` | `Engine\PrimModulate` | ZERO/SRCCOLOR | **texture + colour** (saturated), then multiplies the frame buffer |
| 4–6 | `DEPTHSPRITE_ADDITIVE/ALPHA/MODULATE` | `Engine\PrimDepthSprite*` | as 1/2/3 | |
| 7 | `DIFFUSE_ALPHA` | `Engine\PrimDiffuseAlpha` | SRCALPHA/INVSRCALPHA, **FF lighting on**, vertex colour = diffuse material, MODULATE2X texture × lit colour, specular 0.15 / 32 | |
| 8 | `STENCIL_DARKEN` | stencil darken (RENDER.md §1.5) | | |
| 12 | `DECAL_BUMP_ALPHA` | `Engine\PrimDecalBumpAlpha` (DX9 only) | DESTCOLOR/SRCCOLOR (= 2 × src × dst) | RGB = packed tangent, A = alpha |
| 9, 10, 11, 13 | (no string) | FinalBlur, Heat, ParticleBumpAlpha, AlphaScanlines | | |

- DECAL_BUMP_ALPHA objects are skipped when the bump effect is unavailable (shader detail too low): decals
  need detail ≥ 3 to be counted (0x14ff60) and ≥ 1 to draw (0x1501c0); dynamic tracks ask the effect
  (0x182880). [H]
- PrimDecalBumpAlpha shading in words: the vertex shader turns the vertex colour (×2−1) into a tangent,
  builds the bitangent with the vertex normal, and moves the sun vector into that frame; the pixel shader
  takes N·L of the normal-map texel, combines it with the flat-terrain term, scales by sun colour and
  (texture alpha × vertex alpha) and adds 0.5, so 0.5 is neutral under the 2×src×dst blend: the decal
  only darkens or brightens relief. Distance fade multiplies the vertex alpha. Use the lifted shader in
  `docs/shaders` for the exact formula. [M]
- Texture addressing for stage 0 is set per draw: argument 0 → WRAP, 1 → CLAMP, separately for U and V
  (0x13d400). [H]

### 1.2 Vertex formats

- Pre-lit 24 bytes: position, D3DCOLOR, UV (FVF XYZ|DIFFUSE|TEX1). Used by **shadow blobs**. [H]
- Lit 36 bytes: position, normal, D3DCOLOR, UV (FVF XYZ|NORMAL|DIFFUSE|TEX1). Used by **decals, dynamic
  tracks, map tracks**. The lit draw path sets LIGHTING = 1. [H] (0x180a40)
- Both are written into the shared dynamic buffers of RENDER.md §1.4 during the pre-pass: the task's
  constructor (0x194080) reports vertex/index counts, vf3 (0x194ef0) fills, vf2 draws. [H]

### 1.3 The 0.3 near/far offset

For types 0, 1, 2, 3, 4 the draw function saves the whole camera, adds 0.3 to the near distance (camera
+0x30) and to the far distance (+0x34), rebuilds the projection and view-projection, applies the camera,
draws, then restores the saved camera. Effect: a tiny distance-dependent depth bias towards the viewer so
coplanar decorations win the LESSEQUAL test against the terrain. Port: build the decoration projection
with near+0.3 / far+0.3 (bit-faithful), or use a small constant depth bias (visually equivalent). [H]

---

## 2. Fog of war

### 2.1 Objects

- Game mode +0x1A0 (land; space uses the same class) holds the **FOW renderer** (0x1390 bytes, ctor
  0x7da510). Fields: +0x00..+0x0C playable box (x0, y0, x1, y1), +0x10 space-plane height delta,
  +0x14 blur flag (default 1), **+0x18 FOW_TEX_U, +0x28 FOW_TEX_V** (rows of the inverted box matrix),
  +0x350 `alRenderablePlane` (space FOW plane), **+0x358/+0x35C texture W/H**, **+0x360 FOW texture**,
  +0x368 `W_Generic_White.tga` fallback, +0x370 `SpaceFogOfWar` effect, +0x378 W×H intensity bytes,
  +0x380/+0x388 `W_Space_FOW_Grid.tga` / `W_Space_Reinforce_FOW_Grid.tga`, palettes of 256 colours at
  +0x390 (0, space), +0x790 (1, land), +0xB90 (2, land setup phase), +0xF90 (3, space reinforcement).
  [H] (0x7da510, 0x7da9e0, 0x7dab80, 0x7dbc70)
  (RENDER.md §1.5 says "mode +0x360 or +0x368": those offsets are inside this object, not the mode.)
- Per player a **FOW grid** (0x58 bytes, ctor 0x4c0890, array at mode +0x198): +0x00 state bytes W×H,
  +0x08 u16 per cell, +0x10 second byte array, +0x20 cell count, +0x24 W, +0x28 H, +0x2C/+0x30 actual cell
  size x/y, +0x34 y1, +0x38 x0, +0x3C y0, +0x40 x1, +0x44 last update frame, +0x48 player. [H] layout,
  [M] meaning of +0x08/+0x10.

### 2.2 Grid and texture size

- Box = the map's playable area (chunk `0x103` minis `05`/`06`, TERRAIN_WATER_POST §1.2) as centre ±
  half-extent. Cell size = `DesiredLandFOWCellSize` (GameConstants, vanilla 15.0) on land,
  `DesiredSpaceFOWCellSize` (100.0) in space. W = ceil((x1−x0)/cell), H = ceil((y1−y0)/cell), each clamped
  to [0, 512]; the actual cell size is then recomputed as extent / count. [H] (0x4c0890, ceil 0x22f9f0)
- Example Endor land: box (300,1160)–(5920,5060) → 375 × 260 texels.
- Texture: created at W×H, **1 mip, 4 bytes per texel, managed pool**, written texel by texel through a
  lock (0x13e1b0 → D3DXCreateTexture, 0x18dd30). Format from a table entry; 4-byte ARGB = A8R8G8B8. [M]
  D3DX may round non-power-of-two sizes on old hardware; the shaders assume the texture spans [0,1]. [L]

### 2.3 What a texel means

Per cell the sim keeps a state byte; the renderer converts it to an **intensity** 0..255 (0 = fogged,
255 = clear) and then to a colour. [H] for the conversion steps, [M] for the sim meaning.

1. **State → intensity** (0x7db070, every land frame from 0x3ba400 when the FOW option is on): let v =
   state, p = second array value, f = sim frames since the grid's last update (0..15, 0x4c0dc0),
   r = (grid +0x4C)/16. If v < 239: v' = max(0, v − f·r) (fog regrowing between sim updates); else v' =
   min(255, v − p + f) (becoming visible). Intensity = table[v'] with the table built in the ctor:
   entries 0..15 = 0, 16, 32 … 239 (i × 255/16), entries 16..238 = 255, entries 239..255 = 28 … 255
   ((i−237) × 14.17). So any state 16..238 still renders fully clear, states below 16 fade to fog over
   16 steps, and freshly revealed cells fade in from 28 to 255. [H] arithmetic, [M] interpretation.
2. **Border**: the first and last row and column of the intensity buffer are forced to 0 (fogged). [H]
   (0x7dc220)
3. **Blur** (flag +0x14, default on): a separable 1-2-1 × 1-2-1 binomial filter, (sum + 8) >> 4, over
   the updated rectangle. [H] (0x7dba80)
4. **Palette**: entry i (0..255) = per channel (A·i + B·(255−i)) >> 8, A = visible colour, B = fog colour
   (both D3DCOLOR ARGB). Note i = 255 gives 254, not 255, for a white channel. [H] (0x7db510)
   - Palette 1 (land): A = 0xFFFFFFFF, B = `LandFOWColor` packed as A=4th, R=1st, G=2nd, B=3rd component
     → vanilla (A 0, R 130, G 130, B 130). Palette 2 (land setup phase, used when the mode reports the
     setup phase): A = 0xFFFFFFFF, B = `SetupPhaseFOWColor`. Palette 0 (space): A = 0x00000000, B =
     `SpaceFOWColor`. Palette 3 (space reinforcement overlay): A = 0, B = `SpaceReinforceFOWColor`.
     Ctor defaults before GameConstants are applied: land B = 0xD0FFFFFF, space B = 0xFF002040, reinforce
     B = 0xFF600000. [H] (0x35e450-area setter, globals 0x140b1690c… = GameConstants +0x110C…)
5. **Upload**: only the rectangle of cells the camera can see (the camera frustum intersected with the FOW
   plane, expanded by 2 cells and clamped) is recomputed and written each frame. [M] (0x7dbdf0)

For a renderer test: feed a synthetic W×H intensity grid (0 fogged, 255 clear), apply border, blur and the
land palette, and compare the terrain darkening (fogged ground × 130/255 ≈ 0.51).

### 2.4 Texture coordinates

0x7daec0 builds an affine matrix that maps unit (u, v) to world: x = x0 + u·(x1−x0), y = y1 + v·(y0−y1),
z unchanged, inverts it and stores rows 0 and 1 as FOW_TEX_U and FOW_TEX_V (float4 each, applied to the
world position (x, y, z, 1)):

- FOW_TEX_U = (1/(x1−x0), 0, 0, −x0/(x1−x0))
- FOW_TEX_V = (0, −1/(y1−y0), 0, y1/(y1−y0))

So texel row 0 is the max-y edge of the playable box. [H] matrix, [M] that grid row 0 is the max-y edge
(consistent with the grid storing y1 first). Bound per frame by 0x7dab30 together with the texture: the
FOW texture when it exists **and** the FOW option (global 0x140a12501) is on, otherwise the white
fallback, so FOW-aware shaders show no fog when the option is off. [H]

### 2.5 Terrain FOW pass (decoration type 4)

- Submitted in bucket 0 last, normal passes only, gated by debug byte 0x140a12ad3 and drawn only when the
  FOW option is on. [H]
- Effect `TerrainFogOfWar` (terrain renderer +0x108): one Begin/BeginPass, then every visible patch is
  redrawn with its full 31×31-quad index list (1024 vertices, 1922 triangles), or with the patch's
  "all rendered quads" list when it has holes. Identity world. [H] (0x1946f0 case 4, 0x1b6110)
- Blend **DESTCOLOR/ZERO** (frame buffer × FOW colour), Z write off, Z test from the phase baseline. [H]
  (FXO). t0 (DX9): four bilinear taps at uv ± `FilterKernelSize` (FXO default 0.001237624, not set by the
  engine) along +x, +y, −x, −y, averaged; sampler CLAMP, linear min/mag/mip. t1 (DX8): one tap. t2
  (fixed function): texgen from camera-space position. The VS also outputs vertex fog from FOG_VALS. [H]
  Whether fixed-function fog then tints this multiplicative pass is not checked. [L]
- Because the pass runs after decals, blobs and tracks in bucket 0, those are fogged too. [H] order.

### 2.6 Other users of FOW_TEXTURE

Only these FXOs sample it (they draw outside bucket 0 and are not covered by the terrain pass):
`TerrainMeshGloss`, `TerrainMeshBump`, `Grass`, `BatchMeshAlpha`, `BatchMeshGloss`, `TerrainWater`,
`TerrainLava`, `TerrainIce`. Each computes uv = (FOW_TEX_U·P, FOW_TEX_V·P) from the world position in the
vertex shader and multiplies its colour by the sample. Ordinary unit meshes do not use FOW (units under
fog are hidden by game logic: `HideWhenFoggedBehaviorClass`, `FoggedModelManager`). Water-plane tracks
(type 5) draw after the water and are therefore not fogged. [H] list (decompiled FXOs), [M] the rest.

### 2.7 Space FOW

Same renderer object and texture (palettes 0 and 3). Drawn as an `alRenderablePlane` (+0x350) covering
the box at z = `SpaceFOWHeight` (−80), effect `SpaceFogOfWar` (Transparent, fixed function, SRCALPHA/
INVSRCALPHA, no cull): stage 0 = FOW texture via texgen, stage 1 = grid texture (`W_Space_FOW_Grid.tga`)
modulating alpha. Space visible colour has alpha 0, so only fogged areas show the grid. [H] objects and
FXO, [M] look.

### 2.8 Game-side inputs (enough to fake them)

Reveal radius per object type `Land_FOW_Reveal_Range` / `Space_FOW_Reveal_Range` (×
`Dense_FOW_Reveal_Range_Multiplier`, `FOW_Reveal_Range_Multiplier`), regrow `LandFOWRegrowTime` 6.0 /
`SpaceFOWRegrowTime` 6.0, Lua `FOWReveal`/`Enable_Fog`. The sim writes the state bytes; the renderer only
reads them. [M]

---

## 3. Shadow blobs (decoration type 0, `alTerrainShadowBlob`, 0x148 bytes, ctor 0x12f3e0)

### 3.1 Materials

`ShadowBlobMaterials.xml` (`Material name` → `Texture_Name`, `Render_Mode`, optional effect with
`Texture0`/`Texture1`), loaded by 0x4280b0/0x427e20 into the blob material table (global 0x140a62728).
Two built-in entries: `w_shadow_blob.tga` MODULATE and `w_light_blob.tga` ADDITIVE (0x14e810). The blob's
material index (+0xE0) selects texture and render mode (copied to +0x28). [H]

### 3.2 Placement record (fields set by the owner each frame)

| offset | field |
|---|---|
| +0x20 | terrain |
| +0x28 | render mode (from material) |
| +0x2C, +0x2D | visible, draw flags |
| +0x30..+0x5C | 3×4 frame: rotation about Z by +0xA0, X axis × width, Y axis × −height, origin = position − (X+Y)/2 |
| +0x60..+0x8C | inverse frame (world → unit square) |
| +0x90 | highest terrain height under the blob |
| +0x98, +0x9C | width, height (world units) |
| +0xA0 | rotation (rad) |
| +0xA4..+0xAC | position (z forced to 0) |
| +0xB0 | intensity / alpha |
| +0xB8..+0xC4 | colour RGBA (default white) |
| +0xC8, +0xCC, +0xD4..+0xDC | AABB half-extents and centre |
| +0xE8 / +0xF0 | vertex / index arrays; +0x110 / +0x114 counts |
| +0xFC | packed vertex colour |
| +0x100..+0x10C | covered terrain vertex range x0, x1, y0, y1 |
| +0x118..+0x144 | the four world corners |

[H] (0x12f570, 0x130230)

### 3.3 Geometry

One vertex per terrain grid vertex inside the blob's AABB (range from 0x136320): position = terrain
vertex + (0, 0, 0.1), colour = +0xFC, u = inverse-frame row 0 · P, v = 1 − inverse-frame row 1 · P (the
texture spans the rectangle exactly). Quads (a, a+1, b+1), (a, b+1, b) with a = row·w + col, b = a + w.
No clipping: texture CLAMP does the edges. Rebuilt when position, size or rotation change; only the
colour is recomputed when the intensity changes. [H] (0x130230, 0x12ff60)

Colour (0x12ff60), α = +0xB0, c = colour: ADDITIVE → rgb = c·α and alpha = α²; ALPHA → alpha = c.a;
MODULATE → per channel 0 if c = 1, 1 if c = 0, else c + (1 − α) (so a white blob is always full
strength); DECAL_BUMP_ALPHA → packed tangent; otherwise rgb = c, alpha = α. [H]

### 3.4 Visibility and draw

- Hidden when entirely under a water plane (unless flag +0x94), or beyond the camera's far distance;
  small blobs (both half-extents < 35) use the terrain patch visibility, larger ones a frustum/AABB test.
  [H] (0x12fbe0)
- Visible blobs are grouped per material (table of 0x80-byte groups at 0x140a12390). Per group: bind the
  material texture (U CLAMP), draw the pre-lit batch with the material's render mode; materials with a
  custom effect draw through that effect instead. Camera 0.3 offset applied. [H] (0x190f00, 0x191720)

---

## 4. Terrain decals (decoration type 1, `alTerrainDecal`, 0x248 bytes, ctor 0x14ee40)

### 4.1 Map record (`.ted` terrain chunk `0x101/6`)

Chunk 6 holds a sequence of child chunks `0x10`, one per decal, each a mini-chunk list. Reader 0x14f6b0,
writer 0x14f420. [H]

| mini | size | field (offset) | notes / UM11 sample |
|---|---|---|---|
| `00` | u32 | +0x220 | always 0 in data [L] |
| `01` | u8 | +0x224 permanent | 1 |
| `02` | u32 | +0x21C render mode (§1.1), default 3 MODULATE | 3 (one decal 6) |
| `03` | f32[3] | +0xA4 position | |
| `04` | f32 | +0xA0 rotation about Z (rad) | 0 |
| `05` | f32[2] | +0x98 width, height | 15 × 15 |
| `06` | f32 | +0xB0 intensity (alpha) | 1.0 |
| `07` | u32 | +0x154 atlas slot column | 0 |
| `08` | u32 | +0x158 atlas slot row | 1 |
| `09` | f64 | +0x150 fade time (legacy double form) | |
| `0A` | f32 | +0x150 fade time (s); ≤ 0 = never fades | 0 |
| `0B` | u32 | +0x160 slot scale (cells spanned) | 1 |

Defaults (ctor): mode 3, slot count +0x15C = 16, slot scale 1, +0x228 radius 1.0. On load the terrain
attaches each decal (0x14f060), stores the terrain clock as birth time (+0x148) and builds the geometry.
[H]

### 4.2 Runtime decals (`TerrainDecalFX.xml`)

Spawned by 0x5f0d40 from a `Decal` entry (`xml_field_tables.md` Decal segment): Intensity → +0xB0,
Scale (w, h) × a random factor in [1 − `Random_Size_Offset`, 1 + `Random_Size_Offset`] (same factor on
both axes; **capture 2026-10-31 (RENDER.md §7.3, g1f t05): every drawn decal is exactly its Scale, ~700 decals
of `EXPLOSION_SCORCH_SMALL` 25×30 and `_TINY` 10×10 within 0.005, so the factor is 1 in this build**; the port
applies no factor since g1r t01 and still parses the field), slot = a random pair from `UV_Slot`, `Slot_Scale` → +0x160, `Fadeout_Time` → +0x150,
`Permanent` → +0x224, `Render_Mode` → +0x21C, slot count = GameConstants `Master_Decal_Slot_Count` (not
in vanilla XML → keeps 16 [M]). Rotation = the spawning transform's heading in the XY plane, or uniform
random in [−2π, 2π] when `Random_Rotate`; **the `Z_Angle` field is overwritten and has no effect**.
`Texture_Name` is read but the draw always binds `D_master_decal.tga`. [H]
Rejected when |terrain height − spawn z| > 0.53 × max(w, h), or when at least two live decals with
current alpha ≥ 0.3 already cover the point. [H]

### 4.3 Geometry (0x150760)

1. Build the blob frame (§3.2) from position, size, rotation; the covered terrain vertex range comes from
   the AABB. [H]
2. **Atlas UV matrix** (0x14fb60): with s = √N (N = slot count) the decal's unit square maps to
   u = (column + scale·lx)/s, v = (row + scale·ly)/s, up to an integer offset that WRAP addressing
   ignores (lx, ly = decal-local coordinates from the inverse frame; ly runs against the height axis).
   Stored as two rows at +0x170/+0x180 applied to the lifted world position. [H] matrix; **slot origin
   settled by capture (2026-10-31, g1f t05) [H]:** with X̂ = (cos θ, sin θ), Ŷ = X̂ turned +90° about +z,
   lx = (P−C)·X̂, ly = (P−C)·Ŷ, w, h = Scale: `u = (col + 0.5 + lx/w)/4 − 1`, `v = (row + 0.5 − ly/h)/4 − 1`
   (every decoded vertex within 2.4e-5 uv). So `UV_Slot` "col,row" is a 4×4 grid with row 0 at the top of the
   DDS (v = 0); the slot's (u min, v min) corner sits at the decal's (−X, +Y) corner; u runs along +X, v along −Y
   (upright, not mirrored, seen from above); no half-texel inset (the rectangle covers the whole slot); the
   integer offset is −1 on both axes (harmless under WRAP). The port's matrix matches (`test_decal_atlas_uv` in
  `alamo_terrain_tests`: 5 headings × 4 slots, worst error 3e-7, g1r t01).
3. For every terrain quad in the range, both triangles (same winding as the terrain, TERRAIN_WATER_POST
   §2.2) are clipped against the four vertical planes through the rectangle's edges (each plane through
   two adjacent corners and a point 100 units above their midpoint). The clipped polygon is fan-
   triangulated; every vertex is raised by **+1.0**, keeps the interpolated terrain normal, gets UV from
   the matrix, and is de-duplicated by exact position. [H] (0x152d60 clipper)
4. Bump decals: +0x218 = tangent of the decal X axis projected onto the terrain plane at the decal centre,
   packed (t + 1) × 127.5 into RGB. [H]

### 4.4 Fade and colour (0x14ff60)

age = terrain clock − birth; with L = fade time > 0: alpha = intensity × (L − age)/L, and once age > L
the decal is flagged dead (+0xE4) and no longer drawn. Colour, c = decal colour (white), α = alpha:
ADDITIVE/DEPTHSPRITE_ADDITIVE → rgb = c + α − 1 (white → α); MODULATE/DEPTHSPRITE_MODULATE → rgb =
(1 − c) + (1 − α) (white at α = 1 → 0, i.e. pure texture multiply; values above 1 wrap in the byte);
DECAL_BUMP_ALPHA → rgb = packed tangent; other modes → rgb = c; alpha byte = α in all cases. [H]

### 4.5 Draw

Per frame the task constructor (0x194080 case 1) runs the terrain decal update (0x1371d0), tests each
decal (0x12fbe0, as blobs) and sorts visible ones into **14 lists by render mode**. Draw (0x1946f0 case
1): camera 0.3 offset, bind `D_master_decal.tga` (WRAP/WRAP), then for mode 0..13 one lit-vertex draw
with that mode. [H] Removal of dead decals happens in the update; not traced further. [L]

---

## 5. Tracks

### 5.1 Map tracks (`alTerrainTrack`, 0x278 bytes, ctor 0x153be0, loader 0x1547a0, writer 0x154270)

Terrain chunk `0x101/9` holds a sequence of `0x100` chunks, one per track:

| child | content |
|---|---|
| `0x101` | minis (below) |
| `0x102` | one mini `07` f32[3] per control point (z ≈ 0 in data) |
| `0x103` | texture file name (zero-terminated) |
| `0x104` | one mini `08` f32 per control point: **width** |
| `0x105` | minis `09` f32: **cross-section alpha profile**, one value per vertex across (default 1, 1, 1) |

| mini | field (offset) | default | data (values seen in a sample; flags, profile and height mode counted over all 491) |
|---|---|---|---|
| `00` | +0xE8 | 10 | always 10 (editor default width?) [L] |
| `01` | +0xFC flags: 1 control-point markers, 2 spline lines, 4 textured ribbon | 7 | 4 (417), 5 (71) |
| `02` | +0x1A4 spline sample spacing (world units) | 50 | 11…56 |
| `03` | +0x1E8 U scroll speed (1/s) | 0 | −1 … 0.4 |
| `04` | +0x1EC V scroll speed (1/s) | 0 | −0.31 … 0.64 |
| `05` | +0x248 opacity | 1 | 0.17 … 1 |
| `06` | control point count (check only) | | 3 … 17 |
| `0A` | +0x268 alpha multiplier of the first row | 0 | 0 or 1 |
| `0B` | +0x26C alpha multiplier of the last row | 0 | 0 or 1 |
| `0C` | +0x100 height offset (mode 2) | 0 | 0 … 9 |
| `0D` | +0x1E4 texture repeat length along the track (0 = stretch once over the whole length) | 0 | 0 … 281 |
| `0E`, `0F` | +0x270, +0x271 u8: texture U / V clamp (0 = WRAP, 1 = CLAMP) | 0 | 0, 0 (two V clamps) |
| `10` | +0x1A8 **height mode**: 0 = water plane + 1, 1 = terrain height, 2 = terrain + offset | 1 | 363 × 1, 107 × 0, 21 × 2 |

[H] layout (reader/writer), [H] for 01/02/03/04/05/0A/0B/0C/0D/0E/0F/10 usage below, [L] for `00`.

Geometry (0x156720 → 0x156c20): the control points feed a `SplineClass` (piecewise cubic, sampled every
+0x1A4 units, each sample = position, tangent, arc length); widths are interpolated per sample. Per
sample a row of n = profile-count vertices runs across the track, from +width/2 to −width/2 along the
horizontal perpendicular of the tangent; z from the height mode; normal = terrain normal (water: up).
UV: u = arc length / +0x1E4 (or arc length / total length when +0x1E4 = 0), v = k/(n−1) across. Colour
white, alpha = profile[k] × opacity, × first/last-row multipliers on the first/last row. Indices: quads
between rows, diagonal alternating per column. [H]

Draw (0x154f00): in game only the ribbon (a global "game, not editor" flag 0x140a12554 suppresses the
debug markers and lines); texture stage 0 with the track's clamp flags; **texture matrix** = identity with
translation (frac(t·U speed), frac(t·V speed)), t = terrain clock − attach time (0x1540f0); render mode 7
DIFFUSE_ALPHA, lit vertices. Type 3 = tracks with height mode ≠ 0 (bucket 0, camera 0.3 offset), type 5 =
height mode 0 (bucket 7 after the water, no offset). [H]

Textures in the data (sampled): `w_waves00/01/02.tga` (shore waves, profile 0-1-0, scrolling V),
`W_River_01/07/12.tga`, `w_river_blue00`, `W_waterfall_01` (rivers, scrolling U), `EB_stationlights`,
`W_GirderBig`, `W_R2_Terminal`, `W_blast01`. [H] (data)

### 5.2 Dynamic tracks (`alDynamicTerrainTrack`, 0x148 bytes, ctor 0x158580)

Created per unit from a `DynamicTrack` entry of `DynamicTrackFX.xml` (0x5eb4b0 → 0x158580):

| XML (FX offset) | track field | FX default | vanilla |
|---|---|---|---|
| `texture_name` | +0x20/+0x28 texture | | `Trk_Dirt.tga`, `Trk_Grass.tga`, `w_tank_track_01.tga` |
| `opacity` | +0x48 | 1 | 0.1 … 1 |
| `width` | +0x5C (offset of each edge from the centre line) | 20 | 2.5 … 20 |
| `segment_length` | +0x60 point spacing and texture repeat | 10 | 1 … 20 |
| `Render_Mode` | +0x64 | ALPHA | DIFFUSE_ALPHA or DECAL_BUMP_ALPHA |
| `fade_begin_distance` | +0x68 | 40 | 100 … 220 |
| `fade_end_distance` | +0x6C | 80 | 150 … 280 |
| `fade_distance_per_second` | +0x70 | 20 | 10 |
| `min_geometry_lod` | not copied | | 0.25 |

[H] (0x5eb4b0 defaults, copy at the end of the creator, `xml_field_tables.md`)

Point list (0x2C-byte records: position, quad count, edge data; plus per-point colour, distance and a
"gap" byte). Behaviour [H] (0x159970, 0x15cbb0, 0x158ab0):
- Each update with the unit position: if the last two points are closer than the spacing, the last one is
  replaced; new points are inserted every `segment_length` along the way; the current position is always
  the moving head point. The head distance (+0x74) accumulates the travelled length.
- Every frame the head distance also grows by dt × `fade_distance_per_second`; points whose distance
  behind the head exceeds `fade_end_distance` are removed from the tail.
- Point alpha = opacity × (1 − smoothstep(begin, end, head − point distance)) with smoothstep(t) =
  (3 − 2t)t²; a gap point gets 0.
- Point colour RGB = direction of the following segment, ((dx+1)·127.5, (dy+1)·127.5, 0x7F): the tangent
  for DECAL_BUMP_ALPHA. It is also used for DIFFUSE_ALPHA tracks, where it becomes the diffuse material;
  as read that would tint them. [H] code, [L] visible effect, capture needed.

Geometry (0x15a2d0, 0x15a650, 0x15a8f0, 0x159100): left and right edges at ±`width` from the centre
line, mitred at joints (scaled by 2/|n1 − n2|), z = terrain height + 0.1, normals from the terrain; long
segments are split into several quads to follow the terrain (count stored per point). Quads: u = 0 / 1
across, v = 0.5 ∓ (edge length / `segment_length`)/2 along (one texture repeat per segment length,
centred), vertex alpha from the two end points. Drawn as a quad list with the static quad index buffer,
lit vertices, the track texture (WRAP), its render mode, camera 0.3 offset; culled by AABB. [H]
geometry, [L] for the exact terrain subdivision rule.

---

## 6. Data in the install

| archive | .ted | with chunk 6 (decals) | with chunk 9 (tracks) |
|---|---|---|---|
| FoC `maps.meg` | 212 | 1 (UM11_RAIDERS_OF_THE_LOST_HOLOCRON: 65 decals) | 22 |
| FoC `Patch.meg` / `Patch2.meg` | 15 / 16 | 0 | 1 / 2 (Mandalore 9, Utapau 18) |
| EaW `maps.meg` / `patch.meg` / `patch2.meg` | 169 / 17 / 10 | 0 | 9 / 1 / 0 |

Over the 269 distinct map names: 491 tracks (height mode 1: 363, 0: 107, 2: 21) and 65 decals (64 ×
MODULATE 15×15 slot (0,1), 1 × mode 6 19×19; all intensity 1, no fade, permanent). Vanilla MP land maps
with tracks: Bothawui 3, Mandalore 9, Bespin 4, Kamino 12, Naboo 4, Dantooine 7, Utapau 18 (Patch2). Good
test maps: `_LAND_PLANET_NABOO_02` (20), `_MP_LAND_UTAPAU` (Patch2, 18), `UM11` (decals + 231 tracks).
(TERRAIN_WATER_POST §1.3 "empty in sampled maps" holds for its 26-map sample only.)

---

## 7. RENDER.md §7.1 q7 — secondary top-down pass

alSimpleScene +0x268 is a debug flag. After the buckets execute, the scene draws the frustum of the camera
(0x140780) and asks the cull tree at +0x270 for the nodes inside and outside that frustum, drawing their
boxes as wireframe (24 line vertices per box, one colour per list, 0x1b4120). The "second camera looking
straight down" in RENDER.md §1.3 item 8 is this debug view. Not used by decorations; omit. [H] drawing,
[M] which list gets which colour.

---

## 8. Open questions and capture plan

1. Whether DIFFUSE_ALPHA dynamic tracks really show the direction tint (§5.2). Capture `Trk_Dirt` tracks
   behind a land vehicle; read the vertex colours. [L]
2. FOW texture format bits and D3DX size rounding (§2.2); and whether fixed-function fog affects the
   multiplicative FOW pass (§2.5). [L]
3. The sim's FOW state encoding (grid +0x08 / +0x10, +0x4C regrow rate) for a sim-faithful port; the
   renderer only needs intensities. [M]
4. Map-track mini `00` (always 10) and decal mini `00` (always 0). [L]
5. Removal of dead decals and any cap on decal count. [L]
6. Dynamic-track terrain subdivision rule (quad count per point, 0x15a8f0). [L]

Capture additions (DXVK/apitrace; RenderDoc is out of scope): on Naboo 02, find the bucket-0 draws after the terrain: lit-vertex
ribbons with `w_waves00` and a translating texture matrix (type 3), then the FOW pass (DESTCOLOR/ZERO,
one draw per visible patch, a ~375×260-class texture); in bucket 7 after the water, the water-plane
tracks. On UM11, one `D_master_decal.tga` draw with ZERO/SRCCOLOR (MODULATE).
