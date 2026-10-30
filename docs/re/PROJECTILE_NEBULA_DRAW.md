# Laser projectile visuals and space nebula draw order (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-28 by the RE agent, prompted by two playtest items in docs/PHASE4.md: "laser bolts stretched to
Projectile_Length [port: length scale unverified]" and "Endor nebula through planets". Builds on RENDER.md,
SPACE_AND_OPEN.md (§A, §D.7) and docs/shaders/CATALOG.md and does not repeat them. Clean-room: prose and tables
only. Addresses are RVAs in StarWarsG.exe; XML values are vanilla FoC (`extracted/foc_data_text.zip`), map and model
facts were read from the shipped `maps.meg` / `models.meg`.

Confidence: **[H]** read in the code or data and cross-checked, **[M]** read in the code, intent inferred,
**[L]** inferred, needs a capture.

---

## 0. Summary for the implementer

1. **`Projectile_Custom_Render` 1 or 2 replaces the model.** For these types the model loader (0x375380, land and
   space branches alike) never loads `Space_Model_Name` / `Land_Model_Name`; it builds a procedural render
   object instead: **1 = laser beam** (`alLaserBeam`, 0x7a8860), **2 = laser kite** (`alLaserKite`, 0x7a9fc0).
   Type 0 (or no tag) loads the named ALO as usual. The `W_LASER_*.ALO` files named by the vanilla type-1 lasers are
   therefore **never drawn**. [H]
2. **Beam (type 1)**: a camera-facing six-vertex "pill" on texture `W_Laser_Pill.tga`. Its core segment is
   `Projectile_Length` world units long, **centred on the projectile position** along the render object's local
   ±Y. Half-width and end-cap length are both `k = Projectile_Width × f`, with the distance factor
   `f = 1 + Laser_Beam_Z_Scale_Factor × (d − near) / (far − near)`. Here d is the view depth of the segment midpoint.
   GameConstants `Laser_Beam_Z_Scale_Factor` = **8.0**. The on-screen length is `L + 2k` and the width is `2k`. [H]
   for the formula, [M] for the axis (see §1.4).
3. **Kite (type 2)**: a camera-facing four-vertex diamond on `p_particle_master.tga`, tinted by
   `Projectile_Laser_Color`. It is centred on the projectile position. Half-width and tail length are
   `k = Projectile_Width × f_k`, the nose sits `k × max(1, L⊥)` ahead (L⊥ = the length projected onto the view plane),
   and `f_k = 1 + Laser_Kite_Z_Scale_Factor (1.2) × (d − near)/(far − near)`. [H]
4. Both draw in the **Transparent bucket (8)**, batched (one flush task per scene per frame), with
   `Engine\PrimAdditive` (ONE/ONE, no Z write), cull none and world-space vertices. Kites always use Z test
   LESSEQUAL. Beams use LESSEQUAL in space; in land mode they switch to ALWAYS when the camera looks down steeply.
   [H] for the states, [M] for the land camera condition.
5. **Endor's nebula is a main-scene object.** `_MP_SPACE_ENDOR` places three `Nebula Large Ion` props
   (`w_nebula01_ion.alo`, `Nebula.fx`) at z = −100 near the map centre. They are **not** `In_Background`. The planets
   (`Endor_Backdrop_Large`, `Gas_Backdrop_Large`) are. The space frame draws the background scene, then
   **clears depth**, then the main scene (SPACE_AND_OPEN §A.5). So in the original the nebula is **never
   depth-tested against planets**; it is added on top of whatever the background left. Geometrically it is in front
   of the planets anyway (planet surfaces lie at z ≤ −2040). **A nebula over a planet is original behaviour.** [H]
   for the data and order, [M] for the visual conclusion (no capture).
6. What makes the original nebula look soft rather than a solid sheet is `Nebula.fx`'s vertex term
   `2·n_z⁸ + 0.1`, where n_z is the view-space normal z. Only surfaces facing the camera are bright; silhouettes drop
   to 10 %. The positions also wobble with DistortionScale/SFreq/TFreq (§2.3). Check this before treating
   "nebula over planet" as a bug. [H] (FXO disassembly)

---

## 1. Laser projectile visuals

### 1.1 XML inputs (type fields, docs/re/xml_field_tables.md)

| tag | type offset | use |
|---|---|---|
| `Projectile_Custom_Render` | +0x23c int | 0 = model, 1 = beam, 2 = kite |
| `Projectile_Width` | +0x464 f32 | W, world units |
| `Projectile_Length` | +0x468 f32 | L, world units |
| `Projectile_Texture_Slot` | +0x46c 2 numbers | atlas cell (column, row) |
| `Projectile_Laser_Color` | +0x454 RGBA 0–255 | kite tint (beams ignore it) |
| GameConstants `Laser_Beam_Z_Scale_Factor` | +0x11e4 | 8.0 in vanilla, copied to a global at constants load (0x2cb210) |
| GameConstants `Laser_Kite_Z_Scale_Factor` | +0x11e8 | 1.2 in vanilla |

Vanilla FoC `Projectiles.xml` contains 26 type-1 entries (W/L = 1.0/6, 1.2/12, 1.6/20, slots `0,0` red / `3,0`
green) and 12 type-2 entries (turbolasers, ion cannons, proton torpedoes; W/L = 2/5 or 5/10, slot `2,0`, colours
such as 139,230,104 green and 238,71,54 red). **None of the 12 type-2 entries names any model.** `Max_Speed` plays
no part in the visual size. [H]

### 1.2 Beam (`alLaserBeam`, type 1) [H] unless tagged

- **Construction (0x7a8860):** stores W and L, an axis-aligned box from (0, −L/2, 0) to (0, +L/2, 0) in the render
  object's local space, a bounding sphere of radius L/2, texture `W_Laser_Pill.tga`, and a UV offset of
  (slot.col × 0.25, slot.row × 0.25). The texture is a 4 × 4 atlas.
- **Per frame (0x7a93f0, run by the batch flush 0x7a9320 for every queued beam):**
  1. A = world(0, −L/2, 0) and B = world(0, +L/2, 0), using the render object's world transform. That transform
     carries the model scale, so `Scale_Factor` multiplies L; it is 1.0 for every vanilla laser. [M] for the scale
  2. Project A and B to the screen and build a third screen point P = A + (−(B−A).y, (B−A).x), i.e. the screen
     perpendicular. Unproject all three. **a** = normalise(B′ − A′) (the beam axis as seen on screen) and
     **p** = normalise(P′ − A′) (the screen-perpendicular direction), both in world space.
  3. Midpoint view depth: z = view-row-2 · (A+B)/2 + t (negative in front; the camera looks down −Z).
     `f = 1 − ((z + near) / (far − near)) × Laser_Beam_Z_Scale_Factor = 1 + 8 (d − near)/(far − near)`, d = −z.
     near/far are the drawing camera's (main space scene: 10 / 7000; land: the land camera's).
  4. `k = W × f`. The six vertices, in this order:

| # | position | UV (before the slot offset) |
|---|---|---|
| 0 | A − **a**·k (tail cap tip) | (0.25, 0) |
| 1 | A + **p**·k | (0.125, 0) |
| 2 | A − **p**·k | (0.25, 0.125) |
| 3 | B + **p**·k | (0, 0.125) |
| 4 | B − **p**·k | (0.125, 0.25) |
| 5 | B + **a**·k (head cap tip) | (0, 0.25) |

  Indices (constant table 0x140a457b0): (0,1,2) (1,2,3) (2,3,4) (3,4,5). Vertex colour 0xFFFFFFFF; positions are
  world space (world matrix identity). The pill is laid **diagonally** across its 0.25 cell: the tail tip is at the
  cell's (u max, v 0) corner and the head tip at (u 0, v max). UV table values were read from the static
  initialiser at 0x1d7e0. [H]
- **Resulting size:** length on screen = L + 2k along the projected axis, width = 2k. Worked values in the main
  space scene (near 10, far 7000):

| type (W / L) | d = 1000 (f 2.133) | d = 2000 (f 3.278) | d = 6000 (f 7.856) |
|---|---|---|---|
| small 1.0 / 6 | 10.3 × 4.3 | 12.6 × 6.6 | 21.7 × 15.7 |
| medium 1.2 / 12 | 17.1 × 5.1 | 19.9 × 7.9 | 30.9 × 18.9 |
| large 1.6 / 20 | 26.8 × 6.8 | 30.5 × 10.5 | 45.1 × 25.1 |

  (length × width, world units). The factor grows linearly with distance, so far bolts keep a near-constant
  minimum pixel width (k/d tends to 8W/(far − near)).
- **Draw (0x7a9190):** one task in bucket 8 per scene (0x7a8a50 → 0x149490 with bucket 8); all beams of the frame
  go into one vertex/index batch. `Engine\PrimAdditive` (FF, ONE/ONE, ZWRITE 0), cull **none**, texture stage 0 =
  the pill. Z function: **LESSEQUAL**, or **ALWAYS** when the global "beam z-always" flag is set *and*
  |camera-to-world [2][2]| ≥ 0.5 (camera tilted more than ~30° from horizontal). The flag is set to 1 by the land
  mode scene setup (0x3b81b0, 0x3b8950) and to 0 by the space mode setup (0x4d7da0, 0x4d7820). **In space the
  beams are always depth-tested** (they hide behind ships) and never write depth. [H] for the states, [M] for
  reading +0x28 as the camera's z-axis z component

### 1.3 Kite (`alLaserKite`, type 2) [H] unless tagged

- **Construction (0x7a9fc0):** W, L and the same ±L/2 box as the beam; texture `p_particle_master.tga` with
  UV offset (slot × 0.125) (an 8 × 8 atlas); colour white, then replaced by `Projectile_Laser_Color` (0x7aa210).
- **Per frame (0x7aaab0):** A, B, their screen projections and the unprojected directions as for the beam (axis
  **a** points A→B, i.e. toward local +Y; **p** = screen perpendicular). Additionally:
  - L⊥ = √(((A−B)·view_x)² + ((A−B)·view_y)²): the segment length projected onto the view plane, in world units,
    **clamped to ≥ 1**.
  - `f_k = 1 + Laser_Kite_Z_Scale_Factor × (d − near)/(far − near)` (same depth term as the beam, factor 1.2).
  - `k = W × f_k`. Vertices are relative to **O = the render object's origin** (the projectile position), not to A/B:

| # | position | UV (before the slot offset) |
|---|---|---|
| 0 | O + **p**·k | (0, 0.125) |
| 1 | O + **a**·k·L⊥ (nose) | (0, 0) |
| 2 | O − **p**·k | (0.125, 0) |
| 3 | O − **a**·k (tail) | (0.125, 0.125) |

  If L = 0 the vertices keep their last values (an initial unit diamond). The UV table was read from the static
  initialiser at 0x1d820.
- **Size:** total length k·(L⊥ + 1), width 2k. Turbolaser (W 5, L 10) seen side-on at d = 1000: f_k = 1.170,
  k = 5.85, so the nose is 58.5 ahead of the position, the tail 5.85 behind and the width 11.7. Seen head-on, L⊥ → 1
  and the kite collapses to a 2k diamond.
- **Draw (0x7ab260):** one bucket-8 task per scene; quads via the shared quad index buffer, `Engine\PrimAdditive`,
  cull none, Z test **LESSEQUAL** always (the z-always flag is not passed), no Z write. Vertex colour = the laser
  colour (its alpha is multiplied in by the FF stage, but additive ONE/ONE ignores alpha). [H] for the call
  arguments, [M] for the colour/alpha combination (FF stage state not traced)

### 1.4 Orientation [M]

Both objects place the segment on the render object's **local Y axis, head at +Y**. For the bolts to point along
the flight path, the projectile's render frame must have local +Y = direction of flight. That frame is built by
the projectile/model code, which was not traced here. The port's existing mapping (local Y = flight direction) is
the only reading consistent with the original image; keep it.

### 1.5 Port differences

> **Status 2026-10-30:** The table below describes the port of 2026-10-28 and every row is obsolete. Projectile types with `Projectile_Custom_Render` 1 or 2 now draw as procedural camera-facing beam and kite quads, additive in the Transparent bucket, with width and caps scaled by view depth and type-2 projectiles included: `engine/render/src/lasers.cpp`, `engine/render/include/alamo/render/lasers.hpp`, type fields read at `engine/render/src/sim_snapshot.cpp:158-169`, selection at `engine/render/src/sim_view.cpp:461-471`.

| port (2026-10-28) | original |
|---|---|
| draws the type's ALO (`W_LASER_*.ALO`) stretched to W × L × W | ALO ignored; procedural camera-facing quads |
| bolt head at the projectile position, tail one length behind | beam centred on the position (±L/2) plus caps of k; kite nose ahead of the position |
| size constant in world units | width and caps scale with view depth (×8 beam, ×1.2 kite over near…far) |
| type-2 projectiles have no model → `SimInstanceBuilder` skips them (`if (!m) continue`) | turbolasers, ion cannons and proton torpedoes draw as tinted kites |
| mesh effect of the ALO | `PrimAdditive`, bucket 8, cull none, no Z write, LESSEQUAL (space) |

---

## 2. Space backdrop, planets and nebulae

### 2.1 What is on the Endor maps [H] (from the `.ted` `0x4B0` records)

| map | object | scene | position (z) | model / effect |
|---|---|---|---|---|
| `_MP_SPACE_ENDOR` | env dome 1 `STARS_MEDIUM` | background | origin | `w_stars_*`, MeshGloss stars + MeshAdditive sun |
| | env dome 2 `STAR_BACKDROP_GREEN` | background | origin | `w_stars_nebula_green.alo` × 25, MeshAdditiveVColor |
| | `Endor_Backdrop_Large` | background | (1623, 1150, −4500) | `w_planet_endor_high.alo` × 500: `Planet.fx` sphere + `MeshAdditive` glow (billboard 6) |
| | `Gas_Backdrop_Large` | background | (−5578, 4170, −5000) | `w_planet_gas00.alo` × 500 |
| | 3 × `Nebula Large Ion` | **main** | (230, 432, −100), (−295, −91, −100), (216, −170, −100) | `w_nebula01_ion.alo` × 1: `Nebula.fx` ellipsoid of about ±600 × ±250 (6000 vertices) + particle bone `p_nebula_inside` |
| `_SPACE_PLANET_ENDOR_01` | `Endor_Backdrop_Large` | background | (2664, −276, −4000) | as above |
| | `Gas_Silver_Backdrop_Large` | background | (−3200, 3990, −5665) | |
| | `Nebula Large` | **main** | (−1723, 1854, −100) | `w_nebula01.alo` |

All `Nebula *` SpaceProps (10 types, `Is_Nebula` yes, `SPACE_OBSTACLE, IDLE`) lack `In_Background`. They are
gameplay volumes with a visual and live in the main RtsScene. `Layer_Z_Adjust` (−100) and `Sort_Order_Adjust` (−1)
have no render effect (SPACE_AND_OPEN §D.7).

### 2.2 Frame order in space (restated from SPACE_AND_OPEN §A.5 / §D.7 with the nebula placed) [H]

1. Frame start: clear colour (black), depth, stencil.
2. **Background scene**, same camera with near 10 / far 100000, no fade, no fog, no stencil shadows. Buckets in
   order, mask 0xbbf:
   - bucket 3 Opaque: star-dome `stars` mesh (Z write on), **planet spheres** (`Planet.fx`, Z write on, LESSEQUAL);
   - bucket 8 Transparent, back to front: nebula **domes** (`MeshAdditiveVColor`, ONE/ONE, Z write off, LESSEQUAL,
     cull none), planet glow discs and the star-dome sun billboard (`MeshAdditive`), and the **SpaceFogOfWar plane**
     at z = −80 (SRCALPHA/INVSRCALPHA, Z write off). Planets hide the domes by depth.
3. **Depth-only clear.**
4. **Main scene**, tactical camera near 10 / far 7000, fade 6000→7000: ships and props by bucket. Nebula props are
   bucket 8 (`Nebula.fx`, `_ALAMO_Z_SORT`), sorted back to front with the other transparent tasks and particles.
   They are depth-tested (LESSEQUAL) only against **main-scene** depth (ships, stations, asteroids) and write no
   depth. Their `p_nebula_inside` particles are bucket 8 too. Laser beams/kites (§1) are in the same bucket.
5. Heat, bloom, depth clear, HUD.

Consequences:
- **A main-scene nebula always composites over background planets.** Nothing in the main scene can be occluded by
  a planet, because the planet's depth was cleared in step 3. That holds for ships too (a ship never passes
  "behind" a planet). [H]
- Background nebula domes never draw over planets (depth, step 2). If they do in the port, that is a port bug.
  `test_render_space.cpp` already checks it.
- There is **no space-dust layer**: no string or submitter exists (SPACE_AND_OPEN §A.4). The only other
  full-screen space layer is the FOW plane, which lives in the background scene and tints the stars and planets but
  not the main-scene nebula. [M] for "no dust", [H] for the FOW scene

### 2.3 `Nebula.fx` appearance (FoC FXO, `t0`, vs_1_1 / ps_1_1) [H]

- States: ZWRITE 0, ZFUNC LESSEQUAL, ONE/ONE additive, alpha test off, cull NONE.
- Vertex position = model position + DistortionScale × (c(x), c(y), c(z)), where each c is a cosine (polynomial
  approximation) of `frac(frac(SFreq × world_axis + TFreq × TIME) × 0.9995 + 0.25) × 2π − π`, evaluated per world
  axis. FoC `w_nebula01_ion` uses DistortionScale 25, SFreq 0.002, TFreq 0.05, so the surface ripples by up to ±25
  units slowly.
- Colour = vertex RGB × `LIGHT_SCALE`.rgb × `LIGHT_SCALE`.w × (2 · n_z⁸ + 0.1). n_z is the z component of the
  normalised view-space normal, so faces toward the camera reach about ×2.1 and silhouettes ×0.1. Pixel = texture
  × colour; the UVs scroll by `UVScrollRate` × TIME × 3.
- The result is a bright core with a soft rim. Additive over a planet, it **brightens** the planet; it never hides
  it.

### 2.4 What to check in the port for "nebula through planets"

1. Is the visible layer the main-scene `Nebula Large Ion` prop or a background dome? If it is the prop, overlaying
   the planet is original behaviour (§2.2). Compare its look instead: the n_z⁸ falloff, the 0.1 floor, the
   distortion, and `LIGHT_SCALE` = (1,1,1,1) for unfaded props.
2. If a background dome shows on a planet's disc, check the background pass: planet Z write on, dome Z test on,
   both in one depth buffer before the clear (renderer.cpp `render` step 1).
3. Capture to settle it (Gabriel launches the game; `tools/probe_mod`): `_MP_SPACE_ENDOR`, default camera
   panned toward the Endor planet so a `Nebula Large Ion` overlaps its disc. Expected: nebula colour added on top
   of the planet; nebula dome stars not visible on the disc.

---

## 3. Port files (read, not edited)

> **Status 2026-10-30:** The first three rows (procedural beam / kite including type-2 projectiles with no model, the snapshot fields for slot, colour, width and length, and the additive no-Z-write bucket-8 quad batch) are done in `engine/render/src/lasers.cpp`, `engine/render/src/sim_snapshot.cpp:158-169` and `engine/render/src/sim_view.cpp:461-471`. The test row and the nebula row were not re-checked in this pass.

| item | files |
|---|---|
| procedural beam/kite instead of the stretched ALO; draw type-2 projectiles that have no model | `engine/render/src/sim_view.cpp` (`projectile_transform`, the projectile loop in the builder), `engine/render/include/alamo/render/sim_view.hpp` |
| carry slot, colour and the two GameConstants | `engine/render/src/sim_snapshot.cpp` (type info: also `Projectile_Texture_Slot`, `Projectile_Laser_Color`), `engine/render/include/alamo/render/sim_snapshot.hpp` |
| an additive, no-Z-write, cull-none world-space quad batch in bucket 8 (texture `W_Laser_Pill.tga` / `p_particle_master.tga`) | `engine/render/src/renderer.cpp` (or the R4 particle scene hook, which already draws world-space quads in bucket 8) |
| test expectations (head at the position, Y extent = L) | `engine/render/tests/test_render_sim.cpp` |
| nebula: order already matches §2.2 (background, depth clear, main); check the `Nebula.fx` translation (n_z⁸ term, distortion) | `engine/render/src/renderer.cpp` (`render`, `build_sky`), `engine/shaderxlat` output for `NEBULA`, `engine/render/tests/test_render_space.cpp` |

## 4. Open items

1. The projectile render frame (which code makes local +Y the flight direction). §1.4. [M]
2. The FF texture/colour stage for `PrimAdditive` kites (whether the laser alpha scales the colour). [M]
3. Where the single beam/kite flush task sorts among other bucket-8 tasks (its sort point was not traced). [L]
4. In-game capture for §2.4 item 3.
