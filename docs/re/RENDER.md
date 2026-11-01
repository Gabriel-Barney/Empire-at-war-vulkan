# Render frame structure (StarWarsG.exe FoC x64 v1.121) — behaviour spec for the Vulkan port

Written 2026-10-26 by the RE agent (Phase 3 item C4). Clean-room: this file describes behaviour, constants,
orders and data layouts in prose and tables. It contains no code and no pseudo-code taken from the
decompilation. Addresses are RVAs in StarWarsG.exe; names are the renamed names from
`docs/re/ghidra/functions.csv` where one exists, otherwise `FUN_<rva>`. Several renamed names are
misleading (Ghidra attributed them to the wrong class, e.g. many render-state accessors are labelled
`LandModeClass::sub_*` or `alMeshRenderTask::sub_*`); the address is authoritative.

Confidence tags: **[H]** read directly in the code and cross-checked, **[M]** read in the code but the
intent is inferred, **[L]** inferred from surroundings, needs a capture or an in-game check.

Companion documents: `docs/shaders/CATALOG.md` (effects, semantics), `docs/shaders/PORTING.md`
(PSO keys, constant layout), `docs/re/HOTSPOTS.md` section 6 (skinning palette, render-task CPU cost).

---

## 0. Summary for the renderer engineer

1. **One frame = 12 fixed render buckets ("phases") executed in enum order**, each bracketed by an
   `ENGINE\Phase*.fxo` baseline effect (pass 0 before, pass 1 after). Buckets 8 (Transparent) and
   11 (Heat) are sorted back-to-front per object; all other buckets are grouped by effect, then by
   material instance. [H] (0x180dc0)
2. **The phase of every mesh draw comes from the effect's `_ALAMO_RENDER_PHASE` string**; engine
   primitives (terrain, water, particles, lasers, flares, blob shadows) are hard-wired to buckets 0, 7
   and 8. [H] (0x1b0690, 0x14c270, submitters listed in §1.3)
3. **Post chain**: stencil shadow volumes → full-screen stencil darken (optional soft blur) → … →
   after Transparent, copy back buffer to `SceneTexture` → Heat bucket draws distortion into the back
   buffer cleared to (128,128,255) → `SceneHeat` composite → `SceneBloom` (4 ping-pong blur
   iterations at 1/4 resolution) → depth+stencil cleared → UI. [H] (0x180dc0, 0x183580, 0x183a30)
4. **Engine math is right-handed, Z-up, column-vector, row-major 3×4 storage; camera looks down −Z.**
   D3D matrices are produced by transposing and by `D3DXMatrixPerspectiveFovRH` (depth 0..1). The
   camera FOV field is **horizontal**. Tactical FOV is Hor+ corrected to 16:9 when the display is wider
   than 4:3. [H] (0x232220, 0x17f1d0, 0x2d03f0)
5. **Lighting = one directional "sun" uploaded as `DIR_LIGHT_*_0`, plus two fill lights and ambient
   folded into 3rd-order spherical harmonics** (`SPH_LIGHT_FILL` = ambient + fills, `SPH_LIGHT_ALL` =
   fills + sun), optionally replaced per object by a local-light SH set. Only light 0 reaches the
   shaders as a vector. [H] (0x195bd0, 0x1971b0, 0x1af410)
6. **All engine parameters are re-set on every draw** (effect Begin, or CommitChanges in the batched
   path): there is no per-frame constant buffer in the original. [H] (0x1af410)
7. **Skinning is rigid, palette size = the FXO's `SKINMATRIXARRAY` element count (24 in vanilla)**,
   uploaded per draw; skinned draws use an identity world matrix. Animation time advances only on
   the 30 Hz logic tick (frame ÷ 30); there is no render-side interpolation of animation or object
   transforms. [H] for upload/identity (0x15e1a0, 0x1af410), [M] for no interpolation (0x3a6b80, 0x5d990)
8. **Visibility = whole-model AABB vs frustum (cull tree), far limit = `Object_Fade_End`**; LOD is
   per-model by projected screen size and the Mesh Detail option, selecting meshes whose name
   contains `_LOD<digit>`; no occlusion culling. [H] (0x1b33b0, 0x12e620, 0x14c970)
9. **Mod shaders**: each `MODPATH`/`STEAMMOD` adds `<mod>\Data\Art\Shaders\` and `...\Terrain\`
   ahead of the base directories; every `*.fx` and `*.fxo` found is loaded at boot, and `.fx` source
   wins over `.fxo` of the same name (compiled at runtime by D3DX). [H] (0x67d70, 0x18a850, 0x18a220)

---

## 1. Frame structure

### 1.1 Top-level frame (main loop and present)

Main loop: `Game_Boot_Init_Filesystem` 0x5d990 (it is the boot function and the main loop). [H]

- Logic ticks run when the FrameSynchronizer pacing allows (30 Hz, see PHASE2_ANSWERS Q12). The
  render function `FUN_60330` runs **once per main-loop iteration, independently of ticks**
  (whenever the window is active). In fast mode (+0xb0b5) presentation is skipped unless
  `frame % 4 == 0` (0x27d800). [H]
- Before rendering, per iteration: game-mode manager prep (0x28c580), 0x271840, HUD manager update
  (0x301750), 0x2ffb0, and the 2D scene clock update `FUN_25ca30` with an elapsed time in seconds
  derived from a millisecond counter (wraps at 28800 s) — UI/2D animation appears to use wall time,
  not sim time. [M]
- An optional frame limiter after present sleeps to cap at a configured FPS (global 0x140a10d80,
  minimum 15). [H] (0x60330 tail)

Order inside `FUN_60330` (each step gated by "present this frame"): [H for order, M for roles]

| # | call | role |
|---|---|---|
| 1 | 0x25fc80 | set clear colour (global 0x140a61ad0) |
| 2 | 0x25de30 | begin frame (0x13ce10 with three "clear" flags set: colour, depth, stencil) [M] |
| 3 | 0x28c500 → current mode vtable slot 21 | **3D world** of the active mode (§1.2) |
| 4 | 0x13d030(0,1,0) | **clear depth only** (D3DCLEAR_ZBUFFER) before UI |
| 5 | 0x3dda70, 0x302620, 0x300c00, 0x302520, 0x2fce50, 0x301190, 0x301120, 0x2ff5a0, 0x2ffcb0(0,1) | HUD/world-space UI layers (selection, health bars, reticles…) [L for individual roles] |
| 6 | 0x3023b0(1), 0x301100, 0x3010d0, 0x2717c0, 0x2ffcb0(2,4), 0x300dc0, 0x2ffcb0(5,6), 0x300980, 0x2ffcb0(7,8), 0x3023b0(0), 0x2ffcb0(9,10) | more HUD layers in fixed layer-pair order |
| 7 | 0x25c8c0(global 0x140a619f0) | 2D scene #1 (GUI); draws 6 layer lists in order |
| 8 | 0x2ffcb0(11,12), 0x271740 | top HUD layers |
| 9 | 0x25c8c0(global 0x140a619f8) | 2D scene #2 (cursor / topmost overlay) [L] |
| 10 | 0x262aa0, 0x25fb30, 0x28c520 | transitions / movie overlays [L] |
| 11 | 0x1781f0 | EndScene (device vtable +0x150), resolve; present follows |

Port note: the world pass ends with an explicit depth clear, and the bloom pass (§1.5) also clears
depth+stencil; nothing after the world relies on world depth. [H]

### 1.2 Per game mode

`GameModeClass::vf21` 0x362c30 is the common tail: render the mode's 3D scenes (0x2b6aa0), then the
mode's own 2D scene (0x25c8c0 on mode+0x48). [H]

- **Land** (`LandModeClass::vf21` 0x3bac70): apply fog of war (0x7dab30: FOW texture, FOW_TEX_U,
  FOW_TEX_V), set the clear colour from the active environment (0x2733e0), apply the tactical camera
  (0x2d03f0, §3.4) unless a cinematic camera is active, **clear colour + depth** in the camera
  viewport (0x261870(cam,1,1)),
  then the common tail. The sky cube engine texture is reset to none afterwards (0x13da00(0)). [H]
  Land and space vf21 also set the main scene's heat-allowed and bloom-allowed bytes (alScene +0x210,
  +0x211) every frame; galactic does not, so galactic frames have no heat or bloom (corrected 2026-10-26,
  see TERRAIN_WATER_POST.md §4.5). [H]
- **Space** (`SpaceModeClass::vf21` 0x4d8fa0): FOW as above; copies the active environment's lighting
  into the scene light record: ambient (env +0x6c), sun colour (env +0x84), sun intensity (env +0x78),
  sun specular (env +0x60), sun direction from two angles (env +0xa8 yaw, +0xb4 elevation; see §2.3),
  and sets fill-light 1 and 2 intensities to 0 and clears the shadow-volume flag (alScene +0x269 = 0)
  — **on the background scene only** (corrected 2026-10-27, SPACE_AND_OPEN.md §A): the main space scene
  keeps the environment's fill lights and keeps stencil shadows. [H]
- **Galactic** (`GalacticModeClass::vf21` 0x4b2bc0): set camera, common tail. [H]

`GameModeClass::sub_2b6aa0` 0x2b6aa0 renders up to four scenes from the mode's scene object:
three **background scenes** (scene vtable +0x108, +0x110, +0x118: backdrop/skydome/far layers) and
then the **main scene** (+0x100). For each background scene the camera is temporarily changed to
near = 10, far = 100000, distance fade off (−1/−1), and the game-side fade pair at +0x228/+0x22c of
the mode's scene object is set to 100000; afterwards the camera and the pair are restored. After the **first** background scene only, depth
is cleared (0x261870(cam,0,1)). The main scene uses the gameplay camera unchanged. [H]

`SceneClass::vf10` 0x267c90 (the scene "render" entry) copies the scene's fog enable (forced off if
the camera object says so), fog start/end and fog colour into the `alScene`, applies the camera
(0x13d820), then calls the alScene render with a pass flag: 1 = normal, 2 = special pass 1,
4 = special pass 2 (reflection/refraction style passes), then disables fog. [H]

### 1.3 Scene render: collection (`alSimpleScene::vf20` 0x1b2ae0)

1. A **render context** is built on the stack (0x148db0): it holds 12 phase buckets (each an
   intrusive list), the phase mask (default 0xbff), stacks for light-scale/tint, skeleton palettes
   and light contexts, and the active extra-render-pass list (max 16). [H]
2. `alScene::vf20` 0x199b80 applies per-scene state: fog enable (D3DRS_FOGENABLE), fog colour
   (D3DRS_FOGCOLOR), fog start/end (§2.4), and the **shadow colour** (scene +0x1e4, RGB) used by the
   stencil-darken pass. [H]
3. `TIME` is set from the scene clock (scene +0x1a8) (0x196fb0); the scene's light record
   (scene +0x280) is uploaded (0x195bd0, §2.3). [H]
4. Phase mask: normal pass = **0xbbf** (all buckets except SHADOW 0x40 and BLOOM 0x400); special
   passes (flag 2/4) = **0x12b** (TERRAIN, TERRAINMESH, OPAQUE, OPAQUE_NON_OCCLUDING, TRANSPARENT
   only) and a neutral tint. HEAT (0x800) is removed whenever heat distortion is disabled or the
   context does not allow it (0x149a90). [H]
5. Terrain (scene +0x208) submits its tasks (0x132350 → `alD3dTerrainRender::vf2` 0x18ee70). [H]
6. If scene +0x26a: **frustum collection** of renderables (scene vtable +0x40 =
   `alSimpleScene::vf8` 0x1b33b0, §5). [H]
7. **Shadow-caster collection** (normal pass only, scene +0x269 set): the AABB of everything
   collected (0x1496d0) is intersected with the terrain bounds (height range extended ×1.5), a
   light-extruded region is tested (0x140780), and if non-empty the scene is collected again with
   mask 0x40 so that shadow volumes of casters outside the view still land in the SHADOW bucket. [M]
8. Optional secondary top-down pass (flag byte at scene +0x268): a second camera looking straight
   down (yaw taken from the main camera), near 10 / far 10000, offset 500 units along the yaw
   direction, fog off; it re-queries the scene cull tree (+0x270) with its own frustum and submits
   two filtered groups (0x1b4120). Probably terrain decal/track rendering. [L]
9. Execute (0x180d90 → 0x180dc0, §1.4), then post-render callbacks (0x199f60). [H]

**Who fills which bucket** [H unless marked]:

| bucket | enum (string table 0x819598) | baseline effect | filled by |
|---:|---|---|---|
| 0 | PHASE_TERRAIN | `Engine\PhaseTerrain` | terrain patches (`alD3dTerrainRenderTask`), terrain decorations types 1–3 and others (0x18ee70) |
| 1 | PHASE_TERRAINMESH | **none** (table slot is null) | meshes whose effect says `TerrainMesh` |
| 2 | PHASE_FISSURE | `Engine\PhaseFissure` | effects saying `Fissure` |
| 3 | PHASE_OPAQUE | `Engine\PhaseOpaque` | effects saying `Opaque`, and every effect with no `_ALAMO_RENDER_PHASE` (default) |
| 4 | PHASE_OCCLUDED | `Engine\PhaseOccluded` | effects saying `Occluded` (the occluded-unit silhouette pass, see extra passes below) |
| 5 | PHASE_OPAQUE_NON_OCCLUDING | `Engine\PhaseOpaque` (same effect as 3) | no submitter found in vanilla [M] |
| 6 | PHASE_SHADOW | `Engine\PhaseShadow` | effects saying `Shadow` (MeshShadowVolume, RSkinShadowVolume) |
| 7 | PHASE_WATER | `Engine\PhaseWater` | water (`alD3dWaterRenderTask`), terrain decoration type 5, particle groups flagged at +0x338, effects whose phase string is `Water` (corrected 2026-10-26, see TERRAIN_WATER_POST.md: the compared string is `Water` at 0x818fdc; the water FXOs declare `TerrainWater`, which does not match and would fall back to Opaque, but water tasks are hard-wired to this bucket) [H] |
| 8 | PHASE_TRANSPARENT | `Engine\PhaseTransparent` | effects saying `Transparent`; particle groups; laser beams and kites; lens flares; splines; 3D cables; lightning bolts; blob shadows; simple render tasks (submitters at 0x172290, 0x7a8a50, 0x7aa2b0, 0x7abb40, 0x7ad950, 0x7b1ae0, 0x7e0bf0, 0x13a960, 0x3801d0) |
| 9 | PHASE_LASERS | none | no submitter (lasers go to 8) [M] |
| 10 | PHASE_BLOOM | `Engine\PhaseBloom` | nothing; never in the mask [M] |
| 11 | PHASE_HEAT | `Engine\PhaseHeat` | effects saying `Heat` (MeshHeat, RSkinHeat) and heat particle groups; **remapped to 8** when global 0x140a626bb is set (0x14c270) — that global has no writer in this build, so the remap never happens (corrected 2026-10-26, see TERRAIN_WATER_POST.md) |

Phase-name → bucket mapping parsed at effect load (0x1b0690): Opaque 3, TerrainMesh 1, Occluded 4,
Transparent 8, Shadow 6, Fissure 2, (water string) 7, Heat 11; anything else keeps the default 3. [H]
Consequence for the port: `Engine\PhaseTerrainMesh.fxo` ships but is never bound as a phase baseline;
bucket 1 inherits whatever state the previous phase's pass 1 left. [H for the table, M for the effect]

**Extra render passes (occluded units).** A model can carry `alExtraRenderPass` objects (e.g.
`OcclusionRenderPassClass`, 0x48a3e0). While the model is submitted they are pushed on the context
(max 16). For every submesh, each active extra pass whose phase bit is in the mask and whose effect has
the **same vertex-processing id** (`_ALAMO_VERTEX_PROC`: Mesh/RSkin/Grass/ShadowVolume/
RSkinShadowVolume/Tree) gets a second task with the pass's effect, in the pass effect's bucket.
[H] (0x14c270, 0x12be50). (corrects HOTSPOTS §6, which called these "lights")

### 1.4 Phase execution (`FUN_180dc0`)

[H unless marked]

1. Apply camera (0x13d820). Mark "in render" (global 0x140a62740).
2. **Dynamic geometry pre-pass**: walk all 12 buckets; every task reports how many pre-lit vertices,
   lit vertices and indices it will generate (+0x5c, +0x58, +0x60). Tasks are packed into shared
   dynamic buffers until any total would exceed 0xfffd, then the batch is filled (0x183e80). A task
   asking for ≥ 0xffff of anything is dropped with the log line "render task for %s exceeded the
   dynamic geometry limits". Buffers created at init (0x17e1a0): 0x17ffe8 bytes FVF
   XYZ|DIFFUSE|TEX1 (24 B, 65535 verts) and 0x23ffdc bytes FVF XYZ|NORMAL|DIFFUSE|TEX1 (36 B, 65535
   verts), both dynamic write-only, plus a static quad index buffer of 16384 quads (indices
   b,b+1,b+2,b+2,b+3,b). Particles, lasers etc. are CPU-expanded into these.
3. Fixed-function light setup once (0x197540): LIGHTING on, AMBIENT render state 0, FF lights 0..2
   enabled and filled from §2.3.
4. If **Shader Detail = 0** (fixed function), the SHADOW bucket is skipped.
5. For phase i = 0..11, if its bit is in the mask **and its bucket is non-empty**:
   - phase effect Begin + BeginPass(0) (baseline state);
   - phase 6 only: set the camera's z-bias field to 0.001 and rebuild the projection (§3.3);
   - draw the bucket:
     - phases 8 and 11: **back-to-front** (0x181800). Key per task = squared distance from the
       camera position to the task's reference point (the renderable's world AABB centre), or, for an
       orthographic camera, the view-space depth. Sorted descending with an unstable sort (0x185000),
       so submeshes of one model have no defined relative order.
     - other phases, with the "sort by effect" option on (global 0x140a12500, default on): tasks
       whose effect is a standard `alD3dEffect` are appended to that effect's batch; the batches are
       then flushed effect by effect in the global effect-list order (0x18a6c0); inside a batch the
       tasks are sorted ascending by a material-instance key (+0x5c) (0x1b0eb0). One D3DX Begin and
       BeginPass per effect pass, CommitChanges per draw (multi-pass effects therefore run pass-major
       across the batch). Tasks with other effect types draw afterwards in submission order.
     - with the option off: submission order.
   - EndPass, BeginPass(1)/EndPass (restore), End;
   - after phase 6: reset z-bias, rebuild projection, run the **stencil darken** (0x183580, §1.5).
   - after phase 8 (even if empty): if heat or bloom is active, **copy the back buffer into
     `SceneTexture`** (StretchRect, point filter, 0x198380); if heat is active (and not remapped),
     **clear the back buffer colour to 0x008080FF** (depth untouched).
6. After phase 11: heat composite (§1.5), then bloom (0x183a30), then reset the dynamic buffers.

### 1.5 Render targets and full-screen passes

Created at device init (0x17dc80); all A8R8G8B8 (D3DFMT 21), sizes relative to the back buffer: [H]

| global | name in effects | size | use |
|---|---|---|---|
| 0x140a6fdd8 | `SceneTexture` | 1.0 × 1.0 | snapshot of the back buffer after Transparent; input of heat, bloom and soft shadows |
| 0x140a6fd18 | `DistortionTexture` | 1.0 × 1.0 | snapshot of the back buffer after the Heat bucket |
| 0x140a6fe30 | (transition) | 1.0 × 1.0 | source for `alD3dTransitionEffectLinearWipe` / `AlphaFade` |
| 0x140a6fd88, 0x140a6fd90 | bloom ping-pong | 0.25 × 0.25 | bloom blur chain |

If a target fails to create, the dependent options are forced off (0x17f080): SceneTexture → soft
shadows, heat, bloom; DistortionTexture or PrimHeat or SceneHeat missing → heat; StencilDarkenToAlpha
or FinalBlur missing → soft shadows; SceneBloom or either bloom target missing → bloom. Shader detail
< 3 caps water detail at 1; shader detail < 1 forces water detail 0. [H]

Full-screen quad: 4 vertices at ±1.005 in clip space, drawn as a 2-triangle strip with a dedicated
vertex declaration. [H]

**Stencil shadows** [H for structure, M for which effect variant]:
- SHADOW bucket draws the volumes with `Engine\PhaseShadow` baseline (stencil ops come from the
  material/phase effects, see PORTING §4).
- Then 0x183580 draws a full-screen pair of triangles with an orthographic 2D camera (identity view,
  l/r/b/t = −1/1/−1/1, near 0.1, far 2) in the **scene shadow colour** using prim mode 8
  (`StencilDarken` when Soft Shadows is off or shader detail is 0, otherwise `StencilDarkenToAlpha`,
  which writes alpha 0 where stencil ≠ 0; corrected 2026-10-26, see TERRAIN_WATER_POST.md §4.3).
- If **Soft Shadows** is on: copy the back buffer to `SceneTexture`, bind it to samplers 0–3 and draw
  prim mode 9 (`StencilDarkenFinalBlur`), then unbind.

**Heat distortion** (option "Heat Distortions", global 0x140a12502, default on) [H]:
after Transparent the scene is saved to `SceneTexture` and the colour buffer is cleared to
(R128,G128,B255) = "no offset"; the Heat bucket draws distortion normals into the back buffer with the
scene depth; after phase 11 the back buffer is copied to `DistortionTexture`, and `Engine\SceneHeat`
(params `SceneTexture`, `DistortionTexture`) draws one full-screen quad back into the back buffer.

**Bloom** (option "Bloom", global 0x140a12503, default on, iterations global 0x140a129f4 = 4) [H]:
save the depth-stencil surface and unbind it; pass 0 of `Engine\SceneBloom` renders `SceneTexture`
into bloom target A with `BloomStrength`, `BloomCutoff`, `BloomSize`; pass 1 ping-pongs A↔B 4 times
setting `BloomIteration` and `SceneTexture` = previous target; restore the depth-stencil surface,
**clear depth+stencil** (z = 1, stencil 0), then pass 2 composites the last bloom target onto the back
buffer. Note bloom reads the scene **before** heat composite (the post-Transparent snapshot). The
bloom float values come from the render context (+0x24 cutoff, +0x28 strength, +0x2c size), which a
normal pass copies from the scene's post block, which the active environment fills from TEE minis
`24`/`23`/`28` (defaults 0.9 / 1.0 / 0.25); only land and space mode enable bloom/heat on the scene
(corrected 2026-10-26, see TERRAIN_WATER_POST.md §4.5). [H]

**Skydome / space backdrop**: there is no dedicated sky phase. Backdrops are separate background scenes
rendered before the main scene with a 10…100000 depth range (§1.2); `SKY_CUBE_TEXTURE` is an engine
texture set per scene (0x196e90). [H for mechanism, L for which assets live in which background scene]

**Fog of war**: land terrain draws a FOW pass with `TerrainFogOfWar` (0x18fe80); meshes receive
`FOW_TEXTURE` + `FOW_TEX_U/V` and project it themselves (FXLC texture transforms, see CATALOG). FOW
texture/U/V come from the game mode (0x7dab30: mode +0x360 or +0x368 texture, +0x18 U, +0x28 V).
[H for parameters, L for the space FOW draw (`SpaceFogOfWar`)]

---

## 2. Per-draw state

### 2.1 How effects receive state

- All rendering goes through D3DX effects (`alD3dEffect` = loaded effect, `alD3dEffectInstance` =
  material: effect + material parameter values). [H]
- **Engine parameters are bound by semantic** at load (`alD3dEffectClass::Bind_Engine_Params`
  0x1af110) against the table at 0x140a13200 (34 entries): MODEL, WORLD, VIEW, PROJECTION,
  VIEWINVERSE, VIEWPROJECTION, WORLDINVERSE, MODELINVERSE, WORLDINVERSETRANSPOSE,
  MODELINVERSETRANSPOSE, WORLDVIEW, MODELVIEW, WORLDVIEWPROJECTION, MODELVIEWPROJECTION,
  WORLDVIEWINVERSE, EYE_POSITION, EYE_OBJ_POSITION, CLOUD_TEXTURE, CLOUD_TEX_U, CLOUD_TEX_V,
  SPH_LIGHT_ALL, SPH_LIGHT_FILL, FOG_VALS, DISTANCE_FADE_VALS, TIME, FOW_TEXTURE, FOW_TEX_U,
  FOW_TEX_V, RESOLUTION_CONSTANTS, SHADOW_EXTRUSION_DISTANCE, LIGHT_SCALE, WIND_BEND_VECTOR,
  WIND_GRASS_PARAMS, SKY_CUBE_TEXTURE. Matching is case-insensitive. MODEL* are aliases of WORLD*. [H]
- Handled separately by name: `SKINMATRIXARRAY` (matrix array; element count read from the FXO),
  `GLOBAL_AMBIENT`, and `DIR_LIGHT_DIFFUSE_n`, `DIR_LIGHT_SPECULAR_n`, `DIR_LIGHT_VEC_n`,
  `DIR_LIGHT_VIEW_VEC_n`, `DIR_LIGHT_OBJ_VEC_n` for n = 0..2. **Only n = 0 is ever uploaded**
  (0x1af410). [H]
- Per draw (`alD3dEffectClass` 0x1af410, called from effect Begin, or from the material Begin in the
  batched path): remember the render context and the current task, set FF transforms if the chosen
  technique is FIXEDFUNCTION, run every bound engine parameter, upload the skin palette, upload
  GLOBAL_AMBIENT and the light-0 vectors. The material's own parameters (Diffuse, Colorization,
  textures…) are set by the material instance before that (0x18b570). Batched path: CommitChanges per
  draw (ID3DXEffect +0x208). Begin uses D3DXFX_DONOTSAVESTATE. [H]
- A pass whose annotation `AlamoCleanup` is true (last pass) is excluded from the per-draw pass count
  (0x1aead0). [H]
- Redundant render states are filtered by `alD3dEffectStateManager` (ID3DXEffectStateManager); the
  parameter setters themselves are not filtered (prior patch measured ~70% redundant sets). [H]

### 2.2 Parameter sources

Matrices are D3D row-vector convention 4×4, set with SetValue (64 bytes). [H]

| semantic | value | changes | evidence |
|---|---|---|---|
| WORLD | renderable world matrix transposed to row-vector form; **identity for skinned effects** | per draw | 0x17f510, 0x15e1a0 [H] |
| VIEW | inverse of the camera transform, transposed | per camera | 0x17f1d0 [H] |
| PROJECTION | `D3DXMatrixPerspectiveFovRH` / `OrthoRH`, see §3 | per camera | 0x17f1d0 [H] |
| VIEWINVERSE, VIEWPROJECTION, WORLDVIEW, WORLDVIEWPROJECTION, WORLDINVERSE, WORLDINVERSETRANSPOSE, WORLDVIEWINVERSE | derived lazily with dirty flags (render-state singleton 0x140a6eb60) | derived | 0x17f7c0…0x17f9e0 [H] |
| EYE_POSITION | camera position (float4, w = 1) | per camera | 0x17f1d0, 0x17fa60 [H] |
| EYE_OBJ_POSITION | camera position × inverse world | per draw | 0x17fa70 [H] |
| GLOBAL_AMBIENT | scene ambient RGB, w = 1 | per scene | 0x195bd0 [H] |
| DIR_LIGHT_VEC_0 | sun direction in world space, **pointing towards the light**, w = 0 | per scene | 0x196ff0 [H] |
| DIR_LIGHT_DIFFUSE_0 | sun colour × sun intensity, w = 1 | per scene | 0x197150 [H] |
| DIR_LIGHT_SPECULAR_0 | sun specular × 2 × intensity, w = 0 | per scene | 0x197170 [H] |
| DIR_LIGHT_VIEW_VEC_0 | DIR_LIGHT_VEC transformed by VIEW, normalised | per camera | 0x197010 [H] |
| DIR_LIGHT_OBJ_VEC_0 | sun direction in object space | per draw | 0x1970b0 [M] |
| SPH_LIGHT_FILL | 3 float4×4 (R,G,B) irradiance matrices from an SH9 set = ambient + fill lights 1 and 2 (or the scene record's precomputed SH at +0x94..+0xfc) | per scene / per object | 0x195bd0, 0x197980 [H] |
| SPH_LIGHT_ALL | the fill SH plus the sun (light 0) added as a directional SH lobe | per scene / per object | 0x1971b0 [H] |
| LIGHT_SCALE | float4 of the current task (task +0x2c); default (1,1,1,1). Product of the tint/alpha of all enclosing models (each `alHModel` multiplies the context top by its colour at +0x13c..+0x148); .w = fade alpha; a model whose resulting alpha ≤ 0 is not submitted | per draw | 0x1ad850, 0x15e0d0, 0x12be50 [H] |
| FOG_VALS | (1/(start−end), −end/(start−end)); default start 10000, end 15000 → (−0.0002, 3.0) | per scene | 0x196b20, 0x195820 [H] |
| DISTANCE_FADE_VALS | same form from the camera's fade begin/end (camera +0x40 / +0x3c); if either is ≤ −1 the default (−2e‑5, 3.0) is used (fade effectively off) | per camera | 0x197f10, 0x13d820 [H] |
| TIME | scene clock in seconds (scene +0x1a8); also broadcast as a float4 | per scene | 0x196fb0 [H] |
| FOW_TEXTURE / FOW_TEX_U / FOW_TEX_V | from the game mode, see §1.5 | per frame | 0x7dab30, 0x196db0, 0x196e20 [H] |
| CLOUD_TEXTURE / CLOUD_TEX_U / CLOUD_TEX_V | from the terrain render (terrain +0xd8, +0xe8) | per frame (land) | 0x18ee70, 0x196c70, 0x196ce0 [H] |
| SKY_CUBE_TEXTURE | set per scene (0x196e90); none if the object is not a cube texture | per scene | 0x13da00 [H] |
| RESOLUTION_CONSTANTS | (width, height, 0.5/width, 0.5/height) of the currently bound engine render target, or of the back buffer when none is bound (quarter size during bloom passes 0–1) (corrected 2026-10-26, see TERRAIN_WATER_POST.md §4.2) | per draw | 0x1ad710, 0x1784f0 [H] |
| SHADOW_EXTRUSION_DISTANCE | global float 0x140a12b28 = 200.0; no writer in this build, so constant (corrected 2026-10-26, see TERRAIN_WATER_POST.md) | global | 0x1ad7d0, 0x196980 [M] |
| WIND_BEND_VECTOR | xyz = scene wind vector × 0.5 × (1 + sin(2π·time/period + spatial phase from the object position)), w = 1/(object radius²) | per draw | 0x1ad940, 0x1997f0 [M] |
| WIND_GRASS_PARAMS | normalised scene wind direction | per scene | 0x1ada40 [H] |
| SKINMATRIXARRAY | §4 | per draw | 0x1af410 [H] |

Team colour: there is no engine semantic; `Colorization` is a **material parameter** set by the
material instance (per-object team colour is written into the instance by game code). [M] (not traced
to the player colour table in this pass)

### 2.3 Lights

Scene light record (the object at scene +0x280, uploaded by 0x195bd0): [H]

| offset | content |
|---|---|
| +0x10 | ambient RGB |
| +0x1c, +0x20, +0x24 | intensity of light 0, 1, 2 |
| +0x28, +0x34, +0x40 | diffuse RGB of light 0, 1, 2 |
| +0x4c, +0x58, +0x64 | specular RGB of light 0, 1, 2 |
| +0x70, +0x7c, +0x88 | direction (towards the light) of light 0, 1, 2 |
| +0x94 … +0xfc | 27 floats: precomputed SH9 × RGB of ambient + fills |

- Exactly **3 directional lights** (0 = sun, 1 and 2 = fills) and one ambient. No count limit beyond
  that; there are no point lights in the global path. [H]
- Space environments: direction from yaw a and elevation e as (cos e·sin a, −cos e·cos a, sin e);
  fills forced off (§1.2). The environment table lives at RtsScene +0x90, stride 0x198 per
  environment, current index at +0xa0. [H] (0x267ff0, 0x2730a0)
- Fixed-function lights (for FIXEDFUNCTION techniques): D3DLIGHT_DIRECTIONAL with the D3D direction
  = −(engine direction), diffuse = colour × intensity, specular = specular × 2 × intensity, ambient =
  scene ambient on light 0 only, range 1e6. [H] (0x197df0)
- **Per-object local lights**: when the option at 0x13ecf0 is on, a model gathers nearby
  `alLightSource`s into a 27-float SH set on the context (0x14af00); tasks carry it (+0x50) and it
  replaces the scene SH for that draw (0x196440). [M]

### 2.4 Fog

Linear vertex fog evaluated in the shaders from FOG_VALS (CATALOG/PORTING); D3DRS_FOGENABLE,
FOGCOLOR, FOGSTART, FOGEND are also set for fixed-function paths. Per scene: enable at alScene +0x204,
colour at +0x1f8, start +0x1f0, end +0x1f4, copied from the game `SceneClass` (enable +0x74,
start +0x60, end +0x64, colour +0x68..+0x70) at each render. Fog is disabled for the top-down secondary pass and after every scene
render. [H] (0x199b80, 0x267c90)

---

## 3. Camera

### 3.1 Camera object (0x308 bytes, default-initialised by 0x140100) [H]

| offset | field | default |
|---|---|---|
| +0x00 | camera→world transform, 3×4, rows with translation in the 4th column (position at +0x0c, +0x1c, +0x2c) | identity |
| +0x30 | near | 1.0 |
| +0x34 | far | 1000.0 |
| +0x38 | depth-bias fraction b (near and far both reduced by near·b) | 0 |
| +0x3c | distance-fade **end**; also the far plane of the culling frustum when ≠ −1 | −1 |
| +0x40 | distance-fade **begin** | −1 |
| +0x44 | projection type: 0 perspective, 1 orthographic | 0 |
| +0x48 | **horizontal** field of view, radians | π/4 |
| +0x4c | aspect (width/height) | 4/3 |
| +0x50..+0x5c | ortho left, right, bottom, top | −1, 1, −1, 1 |
| +0x60 | user clip plane enabled (oblique near plane for reflections) | off |
| +0x64 | clip plane (normalised) | |
| +0x74..+0x80 | viewport rectangle as fractions of the back buffer (x0, y0, x1, y1) | 0,0,1,1 |
| +0x84 | world→camera (view) transform, 3×4 | |
| +0xb4 | projection 4×4 (engine form) | |
| +0xf4 | culling frustum with far = fade end (6 planes at +0x30 inside it) | |
| +0x1fc | frustum with the real far plane | |

### 3.2 Conventions [H]

- World is right-handed, **Z up**; the camera looks along its local −Z (frustum construction negates
  near/far, 0x232c50).
- Engine matrices: column-vector convention, stored row-major as 3×4 (WW3D-style). The engine's own
  projection (0x232220) is the OpenGL form (x scale = cot(fov/2), y scale = aspect·cot(fov/2), clip z
  −1..1). It is used for engine-side math (frusta, screen-size LOD).
- D3D side: view = transpose of the engine view with (0,0,0,1) completed; projection =
  `D3DXMatrixPerspectiveFovRH(fovY, aspect, near', far')` with fovY = 2·atan(tan(fov/2)/aspect),
  i.e. the stored FOV is horizontal; orthographic = `D3DXMatrixOrthoRH(r−l, t−b, near', far')`
  (centred). Depth range 0..1 (D3D9). Viewport MinZ 0, MaxZ 1. (0x17f1d0)
- Vulkan port: keep the RH view, use a 0..1 RH projection, flip Y in the viewport, and keep the
  winding rules of PORTING §3.2.

### 3.3 Depth bias for shadow volumes [H]

During PHASE_SHADOW the camera field +0x38 is set to 0.001 and the projection is rebuilt, so near and
far are both moved by −0.001·near (this pushes the volume geometry very slightly back in depth);
reset to 0 after the phase. Port: emulate exactly (it changes stencil results at caster surfaces) or use
an equivalent constant depth offset in the shadow PSO.

### 3.4 Tactical camera (land and space)

Values from `Data\XML\TacticalCameras.xml` (parsed by `TacticalCameraConstantsClass::Parse_Database_Entry`
0x47fe60), applied by 0x2d03f0: [H for the application, values from vanilla FoC XML]

| | Land_Mode | Space_Mode | Unlocked (debug/editor) |
|---|---|---|---|
| Fov default/min/max (deg, defined for 4:3) | 55 / 25 / 55 | 55 / 55 / 55 | 55 |
| Pitch default/min/max (deg; 90 = straight down) | 50 / 50 / 50, splined 27→55 with zoom | 50 / −10 / 85 | 34 / 0 / 80 |
| Distance default/min/max | 460 / 170 / 460 (spline) | 1000 / 200 / 1900 | 900 / 0 / 60000 |
| Near_Clip / Far_Clip | 10 / 7000 | 10 / 7000 | 1 / 60000 |
| Object_Fade_Begin / End | 1100 / 1300 | 6000 / 7000 | 10000 / 11000 |
| Tactical overview (distance, pitch, FOV) | 1000, 65, 55; 1600, 80, 70 | 2200, –, 60; 2900, 80, 70 | 2500 |

- FOV: degrees → radians; **if the display aspect is wider than 4:3 the horizontal FOV becomes
  2·atan(tan(fov/2)·0.75·16/9)** (vertical FOV of 4:3 preserved for exactly 16:9; wider displays get
  the 16:9 value, so their vertical FOV shrinks). Aspect = display aspect (0x14ee30; the profile
  offers 4:3, 16:9 or a custom value, 0x25e390). [H]
- Fade begin/end go to camera +0x40/+0x3c (both 100000 when either is −1 or a cinematic flag is set)
  and to the mode's fog/fade pair (+0x228/+0x22c). [H]

### 3.5 Galactic camera [M]

`GalacticCameraClass` (ctor 0x3c2b20, update 0x3c2c00): rotation = +90° about X, then the pitch
(`GMC_InitialPitchAngleDegrees` −30, zoomed `GMC_ZoomedPitchAngleDegrees` −35); pulled back along the
view axis by `GMC_InitialPullbackDistance` 700, or by planet radius × `GMC_ZoomedPullbackPlanetRadiusFraction`
3.0 with offset `GMC_ZoomedPositionOffsetPlanetRadiusFractions` (0,0,1.05) when zoomed; zoom
transitions use smoothstep (3t²−2t³) over `GMC_ZoomTime` 0.65 s. The FOV is kept from the camera and
only the aspect is refreshed. The galactic near/far/FOV defaults were not located (the generic
`CameraClass` default is FOV 1.0 rad, aspect 4/3, near 1, far 1000) — open question.

> **Status 2026-10-30:** Answered by SPACE_AND_OPEN.md §B.8: the port uses galactic horizontal FOV atan(0.5), near 10, far 10000 (`engine/ui/include/alamo/ui/camera.hpp:15, 96-97`).

---

## 4. Skinning and animation

- **Upload** (see HOTSPOTS §6 A6a for the bone-map details): per draw the task binds the skeleton
  palette that was on top of the context's skeleton stack when the model was submitted (task +0x78).
  The render-state singleton expands the palette's 3×4 bone matrices to 4×4 only when dirty, and the
  effect receives `SKINMATRIXARRAY` via SetMatrixPointerArray with the FXO-declared element count
  (24 in all vanilla RSkin effects). In the batched path the palette goes straight to vertex shader
  constants c0.. (3 registers per bone). [H] (0x15e1a0, 0x17f6b0, 0x1af410)
- Skinned effects (effect "is skinned", vtable slot 7) get WORLD = identity; the palette is
  world-space (bone world × inverse bind). [H] (0x15e1a0, 0x146030)
- `CPUSKIN` technique annotation marks software-skinning techniques; `SWVP` annotation switches to
  software vertex processing for that technique (on devices with < 256 VS constants). [H] (0x1aead0, 0x1ae470)
- **Pose evaluation** (`alHModel::vf15` 0x12c7c0) runs lazily when the model is dirty: mode 0 = bind
  pose, 1 = one animation at a frame time, 2 = cross-fade of two animations with a weight. It then
  pushes bone transforms to attached sub-objects and toggles mesh visibility from per-bone visibility
  flags. [H]
- **Time base**: `GameObjectClass` update (0x3a6b80, once per logic tick) advances its model's
  animations (0x265280) with sim time = frame / 30 (FrameSynchronizer). Frame position += animation
  FPS × Δsim-time × speed; looping wraps, one-shots clamp at (frame count − 2); cross-fades use
  1 − elapsed/duration. [H]
- **No render interpolation was found**: animation frames and object transforms change only on ticks;
  the render loop draws the latest state as often as it runs. [M] (an old-transform copy exists at
  0x3a6b80 for some behaviour, but no render-side reader was found)
- Port implication: rendering faster than 30 Hz shows 30 Hz motion. Interpolation would be an
  enhancement, not parity.

---

## 5. Visibility and LOD

- **Culling granularity: whole renderable** (model AABB), never per submesh. `alSimpleScene::vf8`
  0x1b33b0 queries the scene cull tree with the camera's **fade-limited** frustum (camera +0xf4) and a
  second tree (scene +0x278: large/always-far objects) with the **full-far** frustum (+0x1fc). Without
  a tree it tests each renderable's AABB (positive-vertex test, `Classify_AABB_vs_Frustum` 0x20be80)
  and also requires (renderable pass mask +0x34) ⊇ (context pass id +0x1c). [H]
- Objects beyond `Object_Fade_End` are culled; between begin and end shaders fade them via
  DISTANCE_FADE_VALS. [H]
- **No occlusion culling.** `OcclusionRenderPassClass` is the occluded-unit *drawing* pass, not a
  culling mechanism. [H]
- **LOD** (`FUN_12e620`, per model, during submit): [H]
  - mesh LOD index = the single digit after `_LOD` in the mesh name, −1 if absent (0x14c970);
    damage/alt index = the digit after `_ALT`, −1 if absent;
  - detail d = Mesh Detail option (global 0x140a12528, 0..1). If dynamic LOD is enabled globally
    (0x140a12530) and on the model (+0x198, default on; Lua `Enable_Dynamic_LOD`), d = min(model LOD
    scale (+0x1a0) × screen coverage (+0x19c), (MeshDetail+1)/2);
  - screen coverage = max squared NDC distance between the projected AABB centre and the centre
    offset by each half-extent, × 3.5π (10.995574), clamped to [0,1] (0x12dd00);
  - chosen LOD = clamp(int((maxLOD+1)·d), 0, maxLOD) where maxLOD = highest `_LOD` digit present;
    **higher index = more detail**; meshes with index −1 or the chosen index are shown;
  - recomputed only when d moved by ≥ 0.1 or the alt index changed (hysteresis);
  - meshes whose `_ALT` index differs from the model's current alt are hidden.
- Per-type tuning exists in XML (`LOD_Bias` on GameObjectType +0x240, `min_geometry_lod` in
  GraphicDetails) — their wiring into the model LOD scale was not traced. [L]
- Low-RAM mode (global 0x140a626bc) skips loading shadow-phase meshes of unflagged models. [M] (0x14c970)

---

## 6. Effects and mod shaders at runtime

- **Search directories** (vector 0x140a12a08): `Parse_Command_Line` 0x67d70 / 0x72210 adds
  `<mod>\DATA\ART\SHADERS\` and `<mod>\DATA\ART\SHADERS\TERRAIN\` for every `MODPATH`/`STEAMMOD`;
  boot (0x25ea50) then adds `.\Data\Art\Shaders\` and `.\Data\Art\Shaders\Terrain\`. [H]
- **Preload**: at render init (0x18a850) every `*.fx` and then every `*.fxo` found in each directory
  (through the MEG-aware finder, so archived shaders count) is loaded by name. [H]
- **Load by name** (0x18a220): strip the extension, upper-case, return the already-loaded effect if
  the name matches; otherwise (main thread only) try `<name>.fx`, then `<name>.fxo`, first as given
  (e.g. `Engine\PhaseOpaque`, resolved by FileClass with the mod search list), then under each shader
  directory in order. **A `.fx` source file beats a `.fxo` of the same name** and is compiled at
  runtime ("Shader Compile Errors detected on effect"). On failure the default effect
  (global 0x140a6fec0) is returned. [H] (0x18ada0, 0x1ae720)
- **Technique selection** (`alD3dEffectClass::Select_Technique` 0x1aead0), techniques in file order:
  skip `max_viewport`; level from the `LOD` annotation: FIXEDFUNCTION 0, DX8 1, DX8ATI 2, anything
  else or missing 3 (so `FF` counts as 3); accept if level ≤ Shader Detail; reject if the `WaterLOD`
  int annotation exceeds the Water Detail option; reject if any `VENDOR_FILTER_0..9` int equals the
  adapter vendor id; then ValidateTechnique; first passing technique wins. Failure logs "Effect %s
  failed to validate". [H]
- **Metadata** (0x1b0690): `_ALAMO_RENDER_PHASE` → bucket (§1.3), `_ALAMO_VERTEX_TYPE` → vertex
  declaration id, `_ALAMO_VERTEX_PROC` → vertex-processing id (Mesh, RSkin, Grass, ShadowVolume,
  RSkinShadowVolume, Tree). Unknown phase strings silently become Opaque. [H]
- **Hot reload**: profile flag `ShaderReloadingEnabled` enables "Reloading shader: %s" (0x18a530). [H]
- Graphic options that change the frame (logged to `_GraphicsInit.txt` by 0x25d8e0; stored in the
  player profile): Shader detail 0–3, Water detail, Shadow volumes, Soft shadows, Heat distortions,
  Bloom, Particle detail (0..1, global 0x140a12570), Mesh detail (0..1, global 0x140a12528), Shadow
  detail, Texture reduction. [H]

Port consequences: mod effects must be translated at load (PORTING §0); the PSO key must include the
bucket (baseline state); a mod `.fx` source needs either an HLSL→SPIR-V path or a pre-build step;
semantics outside the 34-entry table plus the light/skin names are left at their FXO defaults.

---

## 7. Open questions and capture plan

### 7.1 Open questions

1. ~~Water-phase effect string~~ resolved: `Water` (corrected 2026-10-26, see TERRAIN_WATER_POST.md §6).
2. ~~StencilDarken variant / stencil func~~ resolved (TERRAIN_WATER_POST.md §4.3).
3. ~~Bloom constants~~ resolved: environment TEE `23`/`24`/`28` (TERRAIN_WATER_POST.md §4.5).
4. Galactic camera FOV / near / far. → see SPACE_AND_OPEN.md §B.8
   > **Status 2026-10-30:** Answered by SPACE_AND_OPEN.md §B.8 and implemented in `engine/ui/include/alamo/ui/camera.hpp:15, 96-97`.
5. What draws in the background scenes in space (skybox ALO, nebulae, planets) and land (skydome). → see SPACE_AND_OPEN.md §A.3
6. Whether team colour is pushed per object into `Colorization` or into a copied material instance. → see SPACE_AND_OPEN.md §B.3
7. Role of the secondary top-down pass (alSimpleScene +0x26c/+0x270) — terrain tracks/decals texture?
   > **Status 2026-10-30:** Answered by DECORATIONS.md §7: it is a debug view (frustum and cull-tree boxes drawn as wireframe), not a decal texture pass, and the port omits it.
8. ~~Setter of `SHADOW_EXTRUSION_DISTANCE`~~ resolved: constant 200.0 (TERRAIN_WATER_POST.md §6).
9. ~~Which global sets 0x140a626bb~~ resolved: nothing sets it; heat is never remapped (TERRAIN_WATER_POST.md §4.4).
10. Effect-list order used by the sort-by-effect flush (load order or reverse) — affects only overdraw,
    not the image, except for equal-depth coplanar opaque draws. → see SPACE_AND_OPEN.md §D.1
11. Whether `LOD_Bias` / `min_geometry_lod` feed the model LOD scale (+0x1a0). → see SPACE_AND_OPEN.md §B.4
12. Galactic scenes (main +0x320, background / midground / midforeground +0x328 / +0x330 / +0x338), the galactic light
    record (ambient 0.05, sun 0.5 from `Galactic_Zoom_Out_Light_Angle`, zoom lerp), TradeRouteLines parse layout, and the
    2D selector → `Engine\Prim*` effect table: → see STRATEGIC_GAPS.md §6.1 and §7.2. Still open: the trade-route
    zoom colour switch, starbase icon placement, `Fleet_ALTn` selection (capture).

### 7.2 Capture plan (prepared, not run — Gabriel runs the game)

Preconditions: the prior optimization patch chain in `corruption\` (proxy `d3dx9_43.dll` →
`eaw_*` DLLs) changes effect-setter traffic and render-task behaviour. For parity captures, disable it
first: back up `corruption\d3dx9_43.dll` and replace it with a copy of
`corruption\eaw_modifier_d3dx_original.dll` (the genuine Microsoft d3dx9_43, see PRIOR_PATCH.md);
restore afterwards.

**A. DXVK HUD + log (cheap, no extra tools).** In a shell: set
`DXVK_HUD=devinfo,fps,frametimes,drawcalls,submissions,pipelines,memory,api` and
`DXVK_LOG_LEVEL=info`, launch `StarWarsG.exe` from `corruption\`. Record draw calls and pipelines for:
(1) galactic map idle, (2) space battle with ~20 ships, (3) land battle with units near a building
(occluded silhouettes), with each of Bloom / Heat / Soft shadows toggled. Expected: the draw count drops
by roughly the shadow-volume count when shadow volumes are off; heat adds one full-screen draw plus
two back-buffer copies and a colour clear; bloom adds 6 full-screen draws (1 + 4 + 1). Log goes to `corruption\StarWarsG_d3d9.log`.

**B. Vulkan-level frame capture with RenderDoc on top of DXVK** (shows render passes, targets and
SPIR-V translated by DXVK; D3D9 names are lost but targets and clears are visible). Set
`ENABLE_VULKAN_RENDERDOC_CAPTURE=1`, launch through RenderDoc "Launch Application" with working
directory `corruption\`. Capture one frame per scenario above. Check: clears (colour+depth at begin,
depth-only before UI, depth+stencil inside bloom), the 0x8080FF clear before the heat bucket, the four
A8R8G8B8 targets (two at 1/4 size), the stencil-darken full-screen draw after the volumes, and the draw
order inside the Transparent bucket.

> **Status 2026-10-30:** Plan B is out of scope: RenderDoc is not used on this project (CLAUDE.md: captures use apitrace; see §7.3 for the apitrace results that replaced these checks).

**C. D3D9-level API trace with apitrace** (exact SetRenderState / SetTransform / shader constants per
draw; best for constant-level diffing per PORTING §6.2). apitrace's `d3d9.dll` wrapper must be the
first `d3d9.dll` loaded: temporarily rename DXVK's `corruption\d3d9.dll` and `dxgi.dll` (e.g. to
`*.dxvk.bak`), copy apitrace's wrappers `d3d9.dll` (and `d3dx9_43` is *not* traced, which is fine:
effects resolve to D3D9 calls), run, then restore the DXVK files. Keep captures short (a few seconds):
the game issues ~30 M effect calls per session. Replay with `qapitrace` to inspect per-draw constant
registers c0..c95 (VS) and c0..c31 (PS), and match them to §2.2.

**D. In-game probes (no tools)**: the `cam` debug command switches to the Unlocked camera (near 1, far
60000) — useful to see background scenes and cull distances; the `_GraphicsInit.txt` log confirms the
active detail settings for each capture.

Tooling note: if Gabriel wants scripted toggles, they belong under `tools/probe_mod/` with run and
cleanup scripts (CLAUDE.md rule 7); none were created in this pass.

### 7.3 Capture results (G1, 2026-10-29)

Kit: `tools/probe_mod/g1_capture` (README there). Run folders are under `tools/probe_mod/g1_capture/out/runs/`.
RenderDoc was dropped (Gabriel 2026-10-29), so the RenderDoc-only checks were re-planned onto the apitrace dump and
d3dretrace snapshots. Unless noted, the answers come from **`apitrace_20261027_103544`**. That is FoC 1.121 with the
patch chain switched off. It holds 30,980 frames:
- frames ~0–6000: intro;
- ~6000–10500: menus with the attract battle;
- ~11000–21930: loading screen;
- 21950–30978: a Kuat space skirmish as the Empire against one AI. Only the station, TIE squadrons and the planet appear; no Admonitor.

The analysis is in `analysis/report.md` and `facts.json`, with focus frames 21950–30978 and frame sequences
10500 / 22000 / 26000 / 30500.

- **Engine cull mode.** The first `SetRenderState(CULLMODE)` is CCW, at call 7508, before any draw. That is the
  device default the engine sets at startup, and it is also the D3D default.
  - In the battle, every lit mesh draw runs with **CW**. That covers the shader pairs `vs#122/ps#121`, `#156/#154`
    (Colorization), `#101/#100`, the planet `#85/#83` and its atmosphere `#126`, `#87`, and skinned `#81/#79`.
    So the effects set CW themselves.
  - Shadow volumes (`m_extrusion` shaders, rigid and skinned), the full-screen blur `#29`, `#38/#37` and almost all
    fixed-function draws (UI, particles: 370,852 NONE vs 42,217 CW) run with **NONE**.
  - Port consequence: `engine_device_defaults()` should start from CCW. Mesh effects must carry CW; do not rely on a
    CW device default.

  > **Status 2026-10-30:** This recommendation was not followed. `engine_device_defaults()` sets `D3DRS_CULLMODE = D3DCULL_CW` on purpose (`engine/materials/src/d3d9_state.cpp:99-103`; rationale in the comment at `engine/materials/include/alamo/materials/d3d9_state.hpp:151-153`: outward ALO faces are counter-clockwise on screen, so the game must cull clockwise faces).
- **Colorization / the blue cast on Imperial ships.** `Colorization` is set per draw to the owner's team colour.
  - The Empire player is (0.3058824, 0.5882353, 0.9294118, 1) on `ps_1_1#121` and `ps_2_0#154`. That is the MP
    palette blue (78, 150, 237).
  - The AI side's skinned units get red (0.9294118, 0.3058824, 0.3058824, 1) on `#79`.
  - `ps_2_0#154` alternates per draw between the team colour and (1, 1, 1, 1). Objects without a team colour get white.
  - The attract-mode menus also use (0.906, 0.867, 0.263) and (0.780, 0.412, 0.231).
  - The FXO defaults (0,1,0,1) / (0,0,0,1) never reach a draw. The blue cast on a lit Imperial ship is therefore real
    team colouring, not a missing default.
  - Final-image snapshots of battle frames give lit-pixel blue/red ratio 1.06–1.10 (`final_images`).
- **Space frame order** (all four sequence frames, also the menu battle at 10500). Every frame follows the same order:
  1. `Clear(TARGET|ZBUFFER|STENCIL, 0xFF000000)`.
  2. ~10 background draws (skybox / nebula / backdrop).
  3. `Clear(ZBUFFER|STENCIL)` over the full rect.
  4. ~44–46 scene draws: ships, planet, shadow volumes and the darken pass.
  5. `StretchRect` of the back buffer into a 1920×1080 A8R8G8B8 render-target texture (POINT).
  6. 5–11 draws (the effects that sample the copy).
  7. `Clear(ZBUFFER)` only.
  8. UI: 18–40 draws.
  9. `Present`.

  Nothing changes the render target in the battle: the trace has only 2 `SetRenderTarget` calls, both in loading
  frame 10935, onto a 512×512 target.
- **Render targets created.**
  - 6 × 1920×1080 A8R8G8B8 RT;
  - 4 × 480×270 (¼ size);
  - 3 × 512×512;
  - 1 × 128 cube;
  - D24S8 depth-stencil 512 and 128.

  The ¼-size bloom targets exist but are never bound, and no `Clear(0x008080FF)` heat pass appears (`heat_passes_found` 0).
  So in this session Bloom and Heat Distortions drew nothing: both are off in the player's options (see below). For the
  heat and bloom results see `apitrace_20261029_232301` below.
- **Shadow volumes in space: yes.** 179,312 battle draws (about 20 per frame) are shadow volumes, drawn with:
  - two-sided stencil, z-fail INCR (CW) / DECR (CCW);
  - colour writes 0, `ZFUNC LESS`, `CULL NONE`.

  Each frame then has one darken draw:
  - `STENCILFUNC NOTEQUAL`, ref 0, read mask 0x3F (the soft-shadow variant);
  - `COLORWRITEENABLE` 8 (**alpha only**), `ZFUNC ALWAYS`.

  The shadow term goes into the back buffer's alpha. A separate stencil bit 0x80 mask is used too: 6,535 FF draws
  write ref 128 with blend ZERO/SRCCOLOR and colour off, then 7,565 alpha-blended draws are tested NOTEQUAL 128.
- **Final pixel colour.** d3dretrace snapshots at the last draw before `EndScene`:
  - `final_0041418163.png`, `final_0048775302.png`, `final_0058634972.png` (battle);
  - `final_0017593143.png` (menu).

  They are the reference images for an `alamo_render` comparison of the same view. Battle frames have mean RGB
  ≈ (33, 40, 41) and lit mean ≈ (55, 68, 66).
- **Skydome.** The background is the run of ~10 draws between the full clear and the first depth+stencil clear (see the
  frame order above). Its rotation order was not read numerically; compare the final snapshots visually.
- **Planet spin.** The Planet.fx object (`UVOffset` shaders) turns by a median 0.0805° per change: 3,426 distinct
  angles over 24,626 frames, −209.8° in total, ≈ 4,471 steps per turn. The trace never covers a full turn, so
  **whether the clip wraps at 239 or 240 frames stays open.** The optional long planet-spin run was not made.

  > **Status 2026-10-30:** Settled in ANIM_PARTICLES.md §A.9 (wraps at count − 1 = 239, 239 s per revolution), and the port does this: `advance_loop` and `sample_keys` wrap at count − 1 (`engine/anim/src/anim_state.cpp:31-37, 50-60`).
  > **Status 2026-10-31 (g1f t07):** confirmed by capture (planet clip wrap bullet below): 239 s per revolution, no hold at the wrap. The 0.0805° / 4,471-steps figures above came from a non-planet object (`UVOffset` is not a Planet.fx register); the planet turns 0.0502° per tick.
- **Legacy vertex colour** (partly).
  - Every draw whose layout carries COLOR0 runs with `LIGHTING FALSE`, `COLORVERTEX FALSE` and
    `DIFFUSEMATERIALSOURCE MATERIAL`:
    - FF UI / particles;
    - `vs#38/ps#37`, `vs#126/ps#125`, `vs#29/ps#28`.
  - With lighting off, D3D9 passes the vertex diffuse straight through, so FF draws use COLOR0 as stored. Shader
    draws read it only if their shader does.
  - **COLOR0 values, decoded (g1f t02, `g1_analyze.py --vb-draws A-B`, apitrace `--blobs`).** The W_STARS_BACK02 draw
    (`vs#126/ps#125`, vertex buffer 7736447 in trace 231628) carries the **stored RGB through to the GPU with alpha
    forced to 0xFF**. This contradicts SPACE_AND_OPEN.md D.4 [H] (which should be corrected) and agrees with what the
    port does today. The star vertex buffer's V is also **+0.4012391 higher than the ALO's V on every vertex** (seen in
    231628; not checked in the other traces).
    - Method: a buffer fill is `Lock`, `memcpy` blob, `Unlock`. D3D9 handles are reused, so a draw's buffer is the
      latest `CreateVertexBuffer` before the draw. Lit 36-byte vertices are FVF XYZ|NORMAL|DIFFUSE|TEX1 (`<3f3fI2f`).
    - Not resolved: lock 51924263 (blob 9024 B vs lock size 44208 B).
- **Decal atlas corner: not answered, but narrowed (g1f t02).** Endor frames 27060–32377 contain **no fixed-function
  lit decal draws**. The only decal-like draws are `vs#40/ps#39` DECAL_BUMP_ALPHA (blend SRCCOLOR / DESTCOLOR) with a
  **64×64 DXT5 texture whose RGB is a flat normal (mean 129, 129, 255) and whose alpha is a blob mask**, UVs 0..1 over
  a quad (V −0.00013..1.00013). These are bump footprint/track decals, **not master-atlas slots**.
  `D_master_decal.DDS` slot UVs therefore still come from DECORATIONS.md. A capture that shows a master-atlas decal
  (scorch, crater) is needed. Texture decode: scratchpad only, not saved under `out/`.
- **Decal atlas capture attempt (g1f t05, 2026-10-31): not captured.** Scorch decals come from the `EXPLOSION_SCORCH_*`
  entries in `TERRAINDECALFX.XML` (slots `0,3` / `1,3`, `Random_Rotate`, `Random_Size_Offset` 0.1, DIFFUSE_ALPHA),
  spawned by the `EXPLOSION_SCORCH_MARK_*` particle effects, so a capture needs ground explosions (a unit or building
  dying). What was tried:
  - Window: with display scaling the game window opens 3840x2054 in real pixels; `g1_ui.fit_window` now fits to
    3840x2160 (the game then sees 1920x1080) instead of shrinking it, which made the game redraw its UI with oversized
    fonts. Screenshots are 3840 wide. Scene `EB` (Endor Xpress as Rebellion vs Easy AI Empire, shot every 30 s,
    DXVK `dxvk_20261031_100605`) now reaches the battle.
  - 6 minutes of Easy-AI play: the AI only captured build pads and its units fought neutral Ewoks off-screen (result
    screen: 2 Dark Troopers, 3 Ewoks lost). Nothing came near the player's base, so there was no fight to frame.
  - Land purchases: a structure queued on a build pad (850) is paid and its model appears, but the queue entry stays
    at a full bar for 3+ minutes, the same stuck-purchase picture as t01. No player units were obtained.
  - Not tried: a hand-played fight. Needs Gabriel (roadmap t06 style): play Endor Xpress under
    `g1.py apitrace --non-interactive` (no `--drive`), get a unit or building destroyed on open ground, keep it in view
    for 10 s, quit; then the `--vb-draws` decode of the lit FF draws with the master-atlas texture can run.
  - **Hand-played try (2026-10-31, Claude via computer use): no fight possible.** Run `apitrace_20261031_215822` (scene
    EH, kept open): the player starts with no units (population 0/9), only the base structures and build pads, and land
    purchases are stuck the same way as space ones (t06 above), so nothing could be destroyed on open ground. t05 waits on
    the same Steam "Verify integrity" step (roadmap g1f t08).
- **Final-image comparison (g1f t04).** `g1_analyze.py --compare NAME REF PORT --regions "name=x0,y0,x1,y1;..."`
  compares a d3dretrace final with an `alamo_render --sim --map X --png` image (1920×1080; world regions exclude the HUD,
  y < 650). Output: `analysis/compare_<name>.json` and `compare_<name>_side_by_side.png` under the run folder
  `out/runs/apitrace_20261029_231628/`. Port images: `out/compare/sim_kuat.png`, `out/compare/sim_endor.png`.
  - **The views do not match.** `alamo_render` has no numeric camera, so framing differs and only luminance, colour
    classes and visible features are comparable.
  - **Camera, recovered from the traces:** Endor in-game pitch is **55° at ~461 distance** (TACTICALCAMERAS.XML
    Land_Mode splines), the port uses 50° and the map centre as target. FOV (55° horizontal at 4:3), near and far match.
    Kuat pitch 50° matches. The game starts on the player's start; the port starts at the map centre.
  - **Kuat** (ref `final_0032670345.png`, call 18401): world mean luma ref 41.4, port 17.2 (**ratio 0.42**); sky 44.1 vs
    21.4 (0.49); low band 30.0 vs 11.3 (0.38). The in-game planet is bright and sunlit with atmosphere, the port's planet
    is dark. Side by side: `analysis/compare_kuat_side_by_side.png`.
  - **Endor** (ref `final_0053085895.png`, call 29215): world mean luma ref 88.7, port 177.9 (**ratio 2.01**); sky band 89.8
    vs 156.0 (1.74); low band 84.1 vs 201.9 (2.40). The port terrain is washed out (sand and grass close to white), in-game
    it is darker and saturated with baked shadow. Side by side: `analysis/compare_endor_side_by_side.png`.
  - Not run: the 232301 bloom/heat finals against the port, the other finals (`final_0037663759`, `final_0056183259`).
  - Candidate renderer tasks: land pitch 55° from the spline; camera starts at the player's start; Kuat planet lighting
    too dark; Endor terrain too bright (check lighting/shadow/tone mapping); a numeric camera option for `alamo_render`
    so views can be matched pixel-for-pixel.

    > **Status 2026-10-30:** "Land pitch 55°" applies to the game, not to `alamo_render`: the renderer's map scene still starts at pitch 50 (`kLandPitch = 50.0f` in `engine/render/include/alamo/render/map_scene.hpp:188`, used at `engine/render/src/map_scene.cpp:844-855`). The game-side camera is `engine/ui/include/alamo/ui/camera.hpp`; this renderer task is not done.
- **Admonitor close-up: not captured (g1f t01, 2026-10-30).** In five driven DXVK runs (`dxvk_20261030_152344`, `153453`,
  `154000`, `154357`, `155457`, one set with a level-4 start station, `--probe-mod admonitor4`) the Thrawn item and a plain
  2350-credit ship were queued, paid and finished, but no ship appeared on screen, on the minimap or in the select-all list.
  Extra TIE fighters bought at a level-1 station without a probe mod did not add squadrons either. So the cause is not
  specific to the Admonitor or the level-1 hack; it is unknown. No Bloom/Heat Thrawn trace exists.
  - **Narrowed 2026-10-31 (g1f t01, runs `dxvk_20261031_063002`, `063242`, `063806`, driven by hand through `g1_ui.py`
    with the game kept open).** In this install a **human player's tactical space purchase never arrives**: the item is
    paid, the queue reaches 100 % and empties, the population counter stays at 0/20 (0/25 as the Rebellion) and no
    unit appears. The result does not change with any of these:
    - map: Kuat and Naboo Blockade;
    - faction: Empire (TIE Interceptor squadron) and Rebellion (X-wing squadron);
    - frame rate: about 370, 118 and 60 FPS (`--extra-args FPSCAP=30`);
    - input: PostMessage and real SendInput clicks (the window was in the foreground);
    - probe mod: none.
    What does work: the station's starting garrison (two squadrons launch at battle start) and the AI's purchases
    (its ships reach and attack the player's station). So the build queue works and the delivery of the human's unit is
    where it stops; why is still unknown. A new probe mod `--probe-mod admonitor_spawn` (Admonitor added to the level-1 station's
    `Starting_Spawned_Units_Tech_0`) did **not** launch the ship either: after about 60 s only the two garrison squadrons
    were out.
  - The game folder matched the kit's checks: `StarWarsG.exe` SHA-256 07daeeae… (= PE_OVERVIEW.md), no loose XML
    in `corruption\Data\XML`, no prior patch chain. One file is newer than the Steam install of 2026-10-10:
    `corruption\PerceptionFunctionG.dll`, 2026-10-15, SHA-256 c3a4ae28…, size equal to PE_OVERVIEW.md's 1,074,176
    bytes; its provenance is unknown.
  - Open (Gabriel, roadmap g1f t06): does a hand-played Kuat skirmish deliver a bought ship? If not, Steam "Verify
    integrity of game files" and repeat. If a bought ship appears, re-run `run_3_apitrace.cmd` with Bloom/Heat on
    (`g1_options.py --set Bloom=1 HeatDistortion=1 -- apitrace ...`) for the Thrawn close-up.
  - **Hand-played check (g1f t06, 2026-10-31, Claude via computer use, real mouse input in the foreground): the bought
    ship does not arrive either.** Run `dxvk_20261031_215539` (scene KH, `--probe-mod admonitor`, kept open): the
    station was selected by hand and one X-wing squadron bought; it was paid, the queue reached 100 % and emptied in about
    20 s, and after another 40 s the population counter still read 0/25 with only the two garrison squadrons out. The
    station showed a Rebel garrison (X-wing, Y-wing) and a 25 cap, so this KH run played the Rebellion, not the Empire.
    So the stuck delivery is not caused by the kit's input method. Still open (Gabriel, roadmap g1f t08): Steam
    "Verify integrity of game files", then repeat. No Thrawn close-up was possible.
  - **apitrace runs end on their own about 6 minutes in (2026-10-31).** Run `apitrace_20261031_214805` (scene KH,
    Bloom/Heat on, `--drive-keep-open`, nobody playing) exited by itself about 6 minutes after launch (trace 10.7 GB),
    with no crash entry in the Windows Application log. So hand play under apitrace has to fit in roughly 5 minutes
    from battle start; check ship arrival with a DXVK run first.
- **Planet clip wrap: not observable in this trace (g1f t03, 2026-10-31).** `g1_planet_wrap.py extract` over the whole
  `apitrace_20261030_160054` (6.5 GB, 83 240 frames, 52.5 M filtered calls, about 20 min) now finishes: it holds 669 185
  `m_world` rows in 11 (shader, row-scale) groups, all on the one mesh shader (`vs#156`). Results of `survey` / `analyse`:
  - Only one object turns steadily: the scale-1.8 group, frames 2 100 to 2 928, about 0 to 177° unwrapped, 0.359° per drawn update
    (7.15 ticks of 0.0502°/tick, i.e. the rate matches 360/239° per clip frame at 30 Hz to within the 0.4% that separates 239
    from 240). It is then not drawn for 80 000 frames and reappears at frames 83 105 to 83 239 with an unrelated orientation.
  - The scale-1.0 group (80 443 frames) holds one angle for 78 490 frames (2 954 to 81 444): a static object.
  - So the trace covers under half a turn (about 118 s of the 239 s revolution) and no turn point. **Whether the clip holds one
    frame at the wrap (240) or loops seamlessly (239) is still not read from a capture.** The step-size fit cannot decide it
    either (see above), and `UVOffset` pairing was not needed.
  - Cause: this run was a Kuat idle scene with camera-wheel markers, not a planet view; the planet is not on screen for most of it.
  - **Candidate task (needs Gabriel to launch the game):** capture >= 260 s of one fixed planet view (galactic map planet zoomed
    in, or a planet-flyby-free space scene), then `python -I g1_planet_wrap.py extract <run>` and `analyse --scale <planet>`;
    a 240-frame wrap shows as a 30-tick hold (1.5°-equivalent pause) at the turn point against `UVOffset`. No engine change is
    indicated: ANIM_PARTICLES.md §A.9 (wrap at count - 1 = 239) is what `anim_state.cpp` does, and nothing here contradicts it.

  > **Status 2026-10-31:** extraction fixed by running it to the end (no script change was needed; the earlier run was cut short); the wrap question stays open on capture evidence, settled in the port by §A.9.
- **Planet clip wrap (g1f t07, 2026-10-31): wraps at 239, confirmed by capture.** `apitrace_20261031_175322` (scene PF: Kuat
  space skirmish, fixed camera; 48 272 frames, 28.2 M filtered calls). Two corrections first: the t03 / "Planet spin" series used
  the register `UVOffset`, which belongs to the generic bump-mesh vertex shader (`m_sphFill`, 62 instructions), so those
  "planets" were ordinary meshes (here: static station parts plus four debris-like objects turning 0.2000 deg per change).
  Planet.fx has no `UVOffset`; its vertex shader is the one that declares `CloudScrollRate` (c31) and `m_time` (c25), and
  `g1_planet_wrap.py extract` now selects on `CloudScrollRate` and stores `m_time` per row (the engine clock, so no frame-rate
  guesswork is left). Result of `extract` + `wrap` on the planet draw (scale 500, one object at (241, -57, -4400), 43 216 frames
  3270..46485, 6 460 pose changes):
  - `m_time` advances 0.03 per sim tick and the pose changes once per tick (two dropped ticks show as 0.06). m_time 0 to 193.85
    = 6 462 ticks = 215.4 s at 30 Hz, **324.4 deg of turn (90% of a revolution)**.
  - Angle per tick: median 0.050207 deg; 360/239/30 = 0.050209 (239 clip frames per turn), 360/240/30 = 0.050000. Linear fit
    1.67357 deg per m_time unit = 0.050207 deg per tick, max residual 0.0072 deg (0.14 tick) over the whole run: one revolution
    = 7 170.3 ticks = **239.0 s**.
  - Frames 3270 to 3686 draw the bind pose (m_time 0, before the first tick); at the first tick (frame 3687) the planet jumps
    170.16 deg to its random start phase (126.03 clip frames), then turns steadily. The angle returns to the bind pose (clip
    frame 239 = frame 0) 3 390 ticks later, at m_time 101.73 (frame 27 426); the steps around it are 0.0502 deg per tick with no
    pause (angles -0.0659, -0.0157, +0.0346, +0.0848), and the rate is the same on both sides (1.67349 before, 1.67370 after,
    deg per m_time unit).
  - A 240-frame wrap would hold the pose for 30 ticks (0.9 m_time units, a 1.5 deg deficit): the longest gap between pose changes
    in the whole run is 0.06 m_time units (a dropped tick), and the residual above rules out the deficit. So **the wrap is at
    count - 1 = 239, seamless, 239 s per revolution at rate 1.0**, as ANIM_PARTICLES.md §A.9 and `anim_state.cpp` have it.
  - Limit: the check rests on the pose at m_time 0 being clip frame 0 (bone rest, 0 deg), which fits the return to it after
    exactly (239 - 126.03) x 30 ticks. The 35.6 deg (708 ticks) of the turn the run did not cover cannot hide the wrap, because
    the wrap is at the bind pose. A longer hold (>= 240 s of sim time) is not needed.
  - Tool: `g1_planet_wrap.py wrap <planet.pkl> --scale 500` prints the above in 0.5 s; extraction took 18 m 47 s.

Unattended runs on 2026-10-29: the menus were driven by `g1_ui.py` with `--drive K,E` and `--probe-mod admonitor`,
which builds Thrawn's SD at station level 1.
- **`dxvk_20261029_231246`** reached both scenes:
  - Kuat as the Empire with max credits: the Admonitor was queued and built. It was **not brought on screen**; the
    camera stayed on the station.
  - Endor Xpress: the initial build-pad view and the Imperial base.

  It has DXVK HUD screenshots, including draw counts. Kuat battle: ~82–86 draws, 4–10 render passes. The restore
  was hash-verified.
- **`apitrace_20261029_231628`** is the same flow, traced: 33,873 frames, 3.2 GB. Kuat battle ≈ frames 15000–24870,
  Endor Xpress 27060–32377. The analysis focuses on 27060–32377. The restore was hash-verified. Its Kuat frames repeat the space
  frame order above.
  - **Land frame order (Endor):**
    1. Two 512×512 A8R8G8B8 passes, each with its own 512 D24S8 and cleared to white `0xFFFFFFFF`: 22 and 19
       draws. These are the water reflection / refraction targets that the water shader `ps_2_0#162` reads
       (`WaterReflectC`, `WaterRefractC`).
    2. The back buffer: `Clear(TARGET|Z|S, 0xFF000000)`, then `Clear(TARGET|Z|S, 0xFFFFFFFF)` over the viewport rect.
    3. ~86 scene draws.
    4. `StretchRect` back buffer → 1920×1080 texture.
    5. ~18 draws.
    6. `Clear(ZBUFFER)`, then UI: ~17 draws.
  - **Land cull:** CW for most mesh draws. NONE for shadow volumes, full-screen and FF draws. **CCW** for 97,702
    draws of mesh shaders (`#156/#154`, `#177/#176`, `#87`) that otherwise run CW: the mirrored water-reflection
    pass flips the winding.
  - **Land shadows:** the same z-fail two-sided stencil volumes (215,332 draws). One darken draw per frame:
    NOTEQUAL ref 0, mask 0x3F, alpha-only write. The player's options have Soft Shadows on.
  - **Land Colorization:** only the Empire blue appears (no AI units in view).
  - **Planet spin (Kuat part):** −412.5° over 30,138 frames, median step 0.1223° (994 distinct angles). The median
    step differs from the saved trace (0.0805°), so the per-frame angle follows render time, not a fixed per-tick
    step. The 239/240-frame clip wrap is still not read from this: it needs angle against sim time.
- **Player graphics options**, read from HKCU `Software\Petroglyph\StarWars FOC\Profiles\Profile0\Player`:
  - Bloom 0, HeatDistortion 0, SoftShadows 1, ShadowDetail 3, ShaderDetail 3.

  That is why the runs above have no bloom or heat passes.
- **`apitrace_20261029_232301`**: Kuat only, traced with Bloom = 1 and HeatDistortion = 1 set for the run by
  `g1_options.py`, then restored and verified.
  - 25,796 frames. Battle ≈ 16000–24403. Heat pass in 20,561 frames (also in the attract menus).
  - The game folder restore was hash-verified.
  - **Space frame order with heat and bloom on** (frames 18355, 24000):
    1. Background / depth+stencil clear / scene as above.
    2. `StretchRect` back buffer → texture A (1920×1080), then ~4 draws, then `StretchRect` → A again.
    3. **`Clear(TARGET, 0x008080FF)` on the back buffer itself.** No render-target switch: the heat bucket draws
       its distortion normals into the back buffer, whose depth still holds the scene.
    4. `StretchRect` back buffer → texture B.
    5. One full-screen `DrawPrimitive` TRIANGLESTRIP × 2 (SceneHeat): reads A and B, writes the back buffer.
    6. Bloom with the depth-stencil unbound: 5 single draws ping-ponging between the two 480×270 targets
       (a, b, a, b, a).
    7. Back-buffer target restored, `Clear(ZBUFFER|STENCIL, 0)`, one composite draw.
    8. `Clear(ZBUFFER)`, then UI, then `Present`.

    Frames with no heat-casting object on screen still do the whole chain, with a flat 0x80 distortion buffer.
    Frame 22148 is a no-scene frame (pause or result screen): it has neither chain.
  - **Heat 0x80 bias, measured.** Frame call 23145010 has no heat draws, so the distortion buffer is flat 0x808080.
    Comparing the scene (snapshot at call 23145004, before the copy) with SceneHeat's output (call 23145033) gives
    a shift of **+0.8 px in x and +0.3 px in y** at 1920×1080. That is a fitted sub-pixel shift, residual 1.2.
    - The replay ran under native D3D9 on the AMD RX 9070 XT, so it uses the technique the driver picked.
    - The bias is real but smaller than the precomputed ≈ 1.9 px − ½ texel. The x / y asymmetry fits a
      technique that scales the V term differently.
    - `analysis/heatfix/h00231450{04,33}.png`. The analyser's own number in `report.md` (−4.5 / −4.5) compared
      the wrong "before" image (the cleared heat buffer). This is fixed in `g1_analyze.py` (`scene_call`).
  - The 6 × 1920×1080 RT textures, the 4 × 480×270 targets and one more `CreateRenderTarget 1920x1080` are created
    the same way as in the bloom-off runs.
