# Porting the Alamo effects to Vulkan (SPIR-V, HLSL SM6)

Companion to `CATALOG.md` / `catalog.json`. Effect numbers come from the FoC catalog (82 effects after
patch overrides, 291 unique bytecode blobs). Usage numbers come from three ALO corpora (CATALOG section 4):
FoC (462 mesh ALOs), EaW (971) and **combined** (1,333: FoC shadowing EaW by name, which is what FoC loads
if it mounts `GameData` underneath, as the exe's `..\GameData\` string suggests). Every claim about a
shader can be checked in `shaders_decompiled.zip` (`<game>/<EFFECT>/effect.fx.txt`, `*.asm`, `*.hlsl`).

## 0. What we are porting

| fact | consequence |
|---|---|
| All programs are SM1-SM2: vs_1_1 (177), vs_2_0 (4), ps_1_1 (63), ps_1_3 (4), ps_1_4 (11), ps_2_0 (32). No SM3, no flow control, no vertex texture fetch, no MRT. | Shaders are short (max 108 VS / 46 PS instructions). The cost is in draw-call and state overhead, not ALU. A straight 1:1 translation is enough for fidelity. |
| Each effect ships a fallback ladder: `DX9` (ps_2_0) > `DX8ATI` (ps_1_4) > `DX8` (ps_1_1..1_3) > `FIXEDFUNCTION`; `max_viewport` is the 3ds Max preview. | Port only the **best** technique of each effect. Everything else is dead weight on modern GPUs. |
| 25 effects (and the best technique of `MeshAlpha`, `MeshAlphaScroll`, and the particle effects `PrimAlpha`, `PrimAdditive`, `PrimDiffuseAlpha`, `PrimOpaque`, `PrimAlphaScanlines`) are **fixed-function only**: texture-stage states plus D3D lighting. | A fixed-function emulation path is mandatory. `MeshAlpha` is the #2 material by submesh count (629 visible), and alpha particles use these effects every frame. |
| Effects set only the states they change. `ENGINE\Phase*.fxo` pass 0 establishes the baseline at the start of each render phase. | Effective state = D3D9 defaults <- phase pass 0 <- material pass. Resolve this offline into a PSO key (section 3). |
| Some states are CPU expressions (FXLC), e.g. `AlphaBlendEnable = (m_lightScale.w < 1)`, `TextureFactor = Colorization`, `MaterialDiffuse = m_lightScale * Diffuse`, FOW texture transforms built from `m_FOWTexU/V * m_viewInv`. | Evaluate them on the CPU per draw, as D3DX did. The only expression that changes the PSO is the fade alpha-blend switch, so precompile 2 variants (opaque, fading). |
| Only the three Patch2 FXOs (`Grass`, `MeshShadowVolume`, `RSkinHeat`) have preshaders, because they were recompiled with a newer compiler. | Fold the preshaders into the per-draw CPU constant setup, or move them into the vertex shader (they are tiny). |
| Mods (Thrawn's Revenge, FotR, ...) ship their own `.fxo` built on the same framework (`_ALAMO_*` metadata, same semantics). | The engine must translate unknown FXOs **at load time**, not only the vanilla set. Use the same pipeline (section 1B) at runtime, with a disk cache. |

## 1. Translation strategy (two tracks)

**A. Hand-written SM6 HLSL for the hot set (section 5).** Use the lifted `*.hlsl` pseudocode as the
reference. Write a small number of HLSL "ubershaders" with permutations and compile them with DXC
(`-spirv -fvk-use-dx-layout`). This gives readable sources, removes the 24-bone and 96-register limits,
and lets us batch.

**B. Automatic translation for everything else, and for mod FXOs.**

1. `fx2.py` (or the C++ `fx2_reader`) parses the effect, picks the best technique, and reads the pass
   states, sampler states, CTAB and bytecode.
2. Bytecode to SPIR-V. There are two options:
   - **vkd3d-shader** (LGPL-2.1; `tools/shaders/build_vkd3d.sh` builds it from the Wine tree). Today it
     translates 230 of the 291 blobs (79%) d3dbc -> SPIR-V. It fails on 61: vs_1_1 `expp` (35 blobs; the compiler uses `expp.y` as
     `frac`), the ps_1_x `_bx2` source modifier (16), an unhandled ps_1_x register form (4), ps_1_x `cnd`
     (2), `texdepth` (2) and `texm3x2pad` (2). The per-blob list is in `validation.json`. It is a good
     fallback and a reference for interface conventions. Link it dynamically if we ship it.
   - **Our own backend:** turn `lift.py` (it already handles every opcode and modifier in the corpus,
     including `expp`/`logp` vs_1_1 semantics, `_bx2`, ps_1_x `tex*` and `cnd`) into a real HLSL emitter.
     The emitter would declare inputs from `dcl`, map `c#` to a cbuffer laid out like the CTAB registers,
     and map `s#` to combined image samplers. Compile the result with DXC. This is the recommended long-term
     path because it covers 100% of the corpus.
3. Fixed-function passes go to the FF ubershader (section 3.4).
4. Cache the SPIR-V by (blob hash, interface key) on disk.

**Interface convention for translated SM1-3 code:** keep the D3D9 register file as UBOs, so that
translated shaders and the engine agree without per-shader reflection:

```
set 0 binding 0 : VSConstants { float4 c[256]; }   // D3D9 vertex float constants (CTAB register order)
set 0 binding 1 : PSConstants { float4 c[32];  }   // ps_2_0 has 32, ps_1_x 8
set 1 binding 0..15 : combined image samplers s0..s15 (ps_1_x: sampler n = texture stage n)
push constant  : { uint alphaFunc; float alphaRef; uint fogMode; ... }   // emulated FF bits
```

The CPU side fills the register files from the effect parameters through the CTAB (exactly what
`ID3DXEffect::CommitChanges` did). For translated shaders this is a memcpy per parameter. The hot-set
shaders from track A use the structured layout in section 2 instead.

## 2. Constant buffers

### 2.1 What the effects declare

All 82 effects include the same global block of 32 engine parameters, bound by semantic (CATALOG section 3).
Materials add at most 6 numeric parameters for meshes (`Emissive`, `Diffuse`, `Specular`, `Shininess`,
`Colorization`, `UVOffset` / `UVScrollRate` / `Color` / `BendScale`...). The terrain and water effects add
more (TerrainWater: 16 parameters, 120 scalars).

### 2.2 Proposed layout (std140 / `-fvk-use-dx-layout`, float4-aligned)

```hlsl
// set 0, binding 0 - per view (changes once per render target / phase)
cbuffer FrameCB : register(b0, space0) {
    float4x4 View, Proj, ViewProj, ViewInv;      // VIEW, PROJECTION, VIEWPROJECTION, VIEWINVERSE
    float4   EyePos;                             // EYE_POSITION
    float4   ResolutionConstants;                // RESOLUTION_CONSTANTS
    float4   FogVals_FadeVals;                   // FOG_VALS.xy, DISTANCE_FADE_VALS.xy
    float4   Time_pad;                           // TIME
    float4   LightAmbient;                       // GLOBAL_AMBIENT
    float4   Light0Vector, Light0Diffuse, Light0Specular;  // DIR_LIGHT_*_0 (world space)
    float4   FOWTexU, FOWTexV, CloudTexU, CloudTexV;
    float4   BendVector, WindGrassVector;        // WIND_BEND_VECTOR, WIND_GRASS_PARAMS
    float4   FogColor;                           // was D3DRS_FOGCOLOR (fixed-function fog)
};

// per draw: dynamic-offset UBO, or an SSBO indexed by instance id (preferred, enables instancing)
struct DrawData {
    float3x4 World;                              // WORLD (WORLDVIEW/WVP/WORLDINVERSE derived)
    float4   LightScale;                         // LIGHT_SCALE (.w = fade alpha)
    float4x4 SphAll[3];                          // SPH_LIGHT_ALL  (per object: local lights differ)
    float4x4 SphFill[3];                         // SPH_LIGHT_FILL
    uint     MaterialIndex, BoneBase, Flags, pad;
};
```

- `WORLDVIEW`, `WORLDVIEWPROJECTION`, `WORLDINVERSE`, `WORLDVIEWINVERSE`, `EYE_OBJ_POSITION` and
  `DIR_LIGHT_OBJ_VEC_0` are derived values. Compute them in the vertex shader from `World` and the frame
  CB (`mul(ViewProj, World)` per vertex costs nothing at these vertex counts), or once per draw on the
  CPU. Do not store them per material.
- The SPH matrices take 384 bytes per object. If profiling shows the engine sets them per light-group
  rather than per object, move them to a light-group table indexed by `DrawData`.

### 2.3 Per-material buffer with deduplication

Measured on drawn submeshes:

| corpus | drawn material instances | distinct (effect, all params incl. textures) | distinct numeric-constant sets |
|---|---:|---:|---:|
| FoC | 2,865 | 582 (80% redundant) | 230 (92% redundant) |
| EaW | 4,857 | 985 (80% redundant) | 425 (91% redundant) |
| **combined** | **6,911** | **1,337 (81% redundant)** | **546 (92% redundant)** |

This matches (and exceeds) the ~70% the prior patch found in live `SetValue` traffic.

- At model load, hash `(effect, numeric params in declaration order)` into a global `MaterialTable`
  (one SSBO, 1 record per unique set). Textures go through bindless indices
  (`VK_EXT_descriptor_indexing`) stored in the same record, so a record is one distinct full set (about
  1,340 records for the whole game with EaW assets mounted, 1,337 measured).
- Record layout for the whole mesh family (Mesh*/RSkin*/Batch*/Tree/Planet use subsets of it), 96 bytes:

```hlsl
struct MeshMaterial {
    float4 Diffuse;          // .xyz Diffuse, .w Diffuse.w where float4 (BatchMeshAlpha)
    float4 Emissive;         // .xyz Emissive, .w Shininess
    float4 Specular;         // .xyz Specular, .w BendScale (Tree/Grass)
    float4 Colorization;     // team color (TFACTOR in FF path)
    float4 UVOffset_Scroll;  // .xy UVOffset, .zw UVScrollRate
    uint4  Tex;              // bindless: Base, Normal/Gloss, Cloud, CloudNormal
};
```

  The other effects (terrain, water, shield, nebula, planet atmosphere) get a per-effect record generated
  from `catalog.json` `material_parameters` (declaration order, float4 packing). The largest
  (TerrainWater, 120 scalars) fits in 512 bytes.
- Engine-driven per-frame material changes (e.g. `UVScrollRate * m_time` scrolling) stay in the shader:
  the material record holds the rate, and the frame CB holds the time. Nothing has to be re-uploaded.

### 2.4 Skinning palette

- Original: `m_skinMatrixArray : SKINMATRIXARRAY`, `float3x4[24]` in c0..c71, rigid skinning
  (`_ALAMO_BONES_PER_VERTEX = 1`). The VS reads the bone index from **NORMAL.w** (the engine packs the ALO
  bone index there when it builds the GPU vertex buffer), truncates it, multiplies by 3 and indexes
  `c[a0.x]`. The index is submesh-local (it goes through the ALO `0x10006` bone map), which is how the
  24-entry palette limit is respected. The exporter does the splitting; the engine never splits
  meshes (corrected 2026-10-26, see RENDER.md and HOTSPOTS §6 A6a).
- Vulkan:
  - one per-frame `BonePalette` SSBO of `float3x4` (48 B/bone) holding every skinned instance's
    world-space bone matrices, written by the animation jobs (multi-threaded, data-oriented);
  - `DrawData.BoneBase` points at the instance's slice;
  - the vertex stream carries the **skeleton** bone index as `uint8/uint16`, resolved at load through the
    `0x10006` map (file offset 112 in the 144-byte vertex), not NORMAL.w;
  - the 24-bone limit disappears, so submeshes no longer need splitting and draws can merge.
  - Because skinning is rigid (one bone, weight 1), `RSkin*` equals `Mesh*` with a per-vertex world
    matrix. One permutation bit (`SKINNED`) covers all 11 RSkin effects.

## 3. Fixed-function and render states -> PSO keys

### 3.1 Resolve order

`state = D3D9 defaults -> Phase<phase>.fxo pass 0 -> material technique pass`. The phase comes from
`_ALAMO_RENDER_PHASE` (Opaque, Transparent, Shadow, TerrainMesh, TerrainWater, Heat, Occluded, plus the
engine-only Terrain/Water/Bloom/Fissure phases). Pass 1 of a phase effect restores state and has no
Vulkan equivalent (PSOs are immutable). (corrected 2026-10-26, see RENDER.md §1.3) The engine has 12
buckets; `Fissure` is also a valid effect phase string; the TerrainMesh bucket has **no** baseline
effect (the engine's phase table slot is empty, so `PhaseTerrainMesh.fxo` is never bound), the
OPAQUE_NON_OCCLUDING bucket reuses `PhaseOpaque`, and an effect without the annotation goes to Opaque.

### 3.2 PSO key

```c++
struct PsoKey {                          // packed, hashed
    uint32 vsHash, psHash;               // or FF-ubershader permutation id (section 3.4)
    uint8  vertexLayout;                 // alD3dVert* / alPrimVert / terrain
    uint8  renderPass;                   // attachment formats + MSAA
    // blend: ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP (+ SEPARATEALPHA..., unused in vanilla)
    uint8  blendEnable, srcColor, dstColor, colorOp;
    uint8  colorWriteMask;               // COLORWRITEENABLE (0, RGB, A, RGBA seen)
    uint8  fillMode;                     // FILLMODE (wireframe used by debug effects)
    // with VK_EXT_extended_dynamic_state these become dynamic and leave the key:
    uint8  cull;                         // CULLMODE  (NONE, CW, CCW)
    uint8  depthTest, depthWrite, depthFunc;      // ZENABLE, ZWRITEENABLE, ZFUNC
    uint8  stencilEnable, twoSided;      // + ops/func per face (STENCIL*, CCW_STENCIL*)
    uint8  alphaTestFunc;                // ALPHATESTENABLE/ALPHAFUNC -> spec constant in PS
};
```

- **D3D state -> Vulkan mapping.** `D3DBLEND_*` -> `VK_BLEND_FACTOR_*` 1:1 (`BOTHSRCALPHA` is unused).
  `D3DCMP_*` -> `VK_COMPARE_OP_*`. `D3DSTENCILOP_*` -> `VK_STENCIL_OP_*` (`INCR` -> `INCREMENT_AND_WRAP`,
  `INCRSAT` -> `INCREMENT_AND_CLAMP`). Cull: D3D9 treats clockwise as front-facing, and `D3DCULL_CCW`
  (the default) culls counter-clockwise faces, so use `frontFace = CLOCKWISE`, `cullMode = BACK`.
  Re-check after the Y-flip (negative viewport height keeps the winding).
- **Alpha test** has no Vulkan equivalent. It is `discard` in the PS with func and ref as specialization
  constants. Vanilla uses only `GREATER 8` (particles, grass) and `GREATER 128` (trees, blob shadows).
- **Stencil reference/masks, depth bias, blend constants** are dynamic state. `DEPTHBIAS` and
  `SLOPESCALEDEPTHBIAS` appear in one effect only.
- **Fade switch.** 11 opaque mesh/tree/planet effects compute `AlphaBlendEnable = (m_lightScale.w < 1)`. Build both
  PSOs at load and pick per draw.
- **Fog.** Vertex shaders write `oFog = saturate(FOG_VALS.x * |clip.xyz| + FOG_VALS.y)` and the D3D9 FF
  fog unit blended to `FOGCOLOR`. The port must apply `lerp(FogColor, color, fog)` at the end of every PS.
  Make this a PS epilogue shared by translated and hand-written shaders.
- **Color space.** Everything is gamma-space UNORM with no `SRGBTexture` or `SRGBWRITEENABLE`. Keep UNORM
  render targets and textures for fidelity, and make linear lighting an opt-in later.

Count: the best techniques of all 82 effects produce **52 distinct partial render-state keys** (before
merging with the phase baseline). With 2 fade variants and about 10 vertex layouts, the whole vanilla game
needs a few hundred PSOs. Precompile them all at load (or into a `VkPipelineCache` on first run) to
remove hitching in large battles. Mods add their own and go through the same cache.

### 3.3 Samplers

There are 10 distinct sampler-state blocks (WRAP/CLAMP x LINEAR/POINT, mip LINEAR). 56 samplers specify
no states and rely on device state set by the engine (graphic-detail options probably force
anisotropy). Keep a `VkSampler` cache keyed by (filters, addressing, aniso, LOD bias). In the hot set,
replace effect samplers with the engine's quality-controlled samplers.

### 3.4 Fixed-function ubershader

44 distinct texture-stage configurations occur in FIXEDFUNCTION passes (most in fallback techniques we
can drop). The ones that must work are the best techniques of `MeshAlpha`, `MeshAlphaScroll`, `Prim*`,
`Phase*`, `FrameEffect*`, `SpaceFogOfWar`, `TerrainPassability`, `StencilDarken*` and `BlobStencilMasked`.

- Generate a VS/PS pair from a key: up to 2-3 stages x {COLOROP, ALPHAOP, ARG1/2 (DIFFUSE, TEXTURE,
  TFACTOR, CURRENT, SPECULAR, with COMPLEMENT/ALPHAREPLICATE)}, `TEXCOORDINDEX` (incl.
  `CAMERASPACEPOSITION` for fog-of-war projection), `TEXTURETRANSFORMFLAGS` + `D3DTS_TEXTUREn`, and
  `LIGHTING` on/off.
- Lighting = D3D9 FF model with the engine's directional light 0 plus ambient. Material colors come from
  the FXLC expressions (`MaterialDiffuse = m_lightScale * Diffuse`...) and `DIFFUSEMATERIALSOURCE` /
  `COLORVERTEX` choose vertex color or material.
- `MODULATE2X` (MeshAlpha) and `BLENDTEXTUREALPHA` (team-color FF fallback) must be exact: they define
  the look of glass, cockpit and alpha decals.
- Key = a hash of these fields. Generate the HLSL text and compile it with DXC at load, or prebuild every
  vanilla key. The same generator handles mod effects that only have FF techniques.

## 4. Shadows, post effects, particles

- **Stencil shadow volumes** (`MeshShadowVolume` 472 submeshes and `RSkinShadowVolume` 96 submeshes, about
  1.18 M triangles, the largest triangle load in the game). The vertex shader moves every vertex whose
  normal faces away from the light (`dot(N, L_obj) < 0`) along the object-space light direction by
  `SHADOW_EXTRUSION_DISTANCE` (the Patch2 preshader computes `m_extrusion * m_light0ObjVector`). The
  shadow meshes carry pre-built degenerate fin quads. The effect has a two-sided-stencil technique and a
  two-pass fallback. Vulkan: the single-pass two-sided variant (separate front/back stencil ops), then
  `StencilDarken`, `StencilDarkenToAlpha` and `StencilDarkenFinalBlur` (screen-space darken/blur). Shadow
  maps can replace this later behind a toggle; keep the volume path for parity tests.
- **Post:** `SceneBloom` (3 passes, ps_1_x), `SceneHeat` (distortion from `HEAT_TEXTURE`),
  `FrameEffect*` (wipe, fake motion blur, fade). Each is a full-screen pass that maps cleanly onto compute
  or full-screen triangles. They read the previous scene through `SceneTexture` / `PrevSceneTexture`
  (render-target copies, so use image barriers instead of StretchRect).
- **Particles** draw through `Prim*` effects (FF `PrimAlpha` / `PrimAdditive` / `PrimModulate`, and
  `PrimDepthSprite*` vs_2_0/ps_2_0 depth sprites: the PS samples a per-sprite depth map (`DepthSampler`),
  scaled by `DEPTH_SCALE`, and writes `oDepth`, so sprites intersect geometry correctly; in Vulkan this is
  `SV_Depth`, so keep these draws out of early-Z-sensitive batches). Particle
  vertices are CPU-generated (`alPrimVert`). Use one ubershader with a blend-mode key plus GPU-side
  vertex generation (compute writes to a ring buffer). This is the biggest draw-call reducer in space
  battles.

## 5. Shortlist: the shaders that cover most draw calls

Ranked by drawn ALO submeshes in the **combined** corpus (one draw per model instance per submesh; total
6,911). The first 10 cover **91.4%** (FoC alone: 91.7%; EaW alone: 92.1%), and the first 12 cover 94.6%.
`alDefault.fx`, `Default.fx` (no FXO) and `MeshCollision.fx` (debug) are excluded: they are collision/proxy
geometry and are not drawn (CATALOG 4.2). Engine-driven effects (terrain, particles, post) draw every frame
and come after them.

| # | effect | combined | FoC | EaW | best tech (VS/PS) | port as |
|---:|---|---:|---:|---:|---|---|
| 1 | `MeshBumpColorize` | 1,682 | 667 | 1,202 | DX9 vs_1_1(62) / ps_2_0(24) | `AlamoMesh.hlsl` BUMP+COLORIZE |
| 2 | `MeshAlpha` | 1,264 | 629 | 747 | FIXEDFUNCTION (MODULATE2X tex x lit diffuse) | `AlamoMesh.hlsl` ALPHA, FF-equivalent |
| 3 | `MeshShadowVolume` | 1,219 | 472 | 931 | DX8 vs_1_1(11) / ps_1_1(1) | `ShadowVolume.hlsl` |
| 4 | `MeshAdditive` | 844 | 281 | 649 | DX8 vs_1_1(12) / ps_1_1(2) | `AlamoMesh.hlsl` ADDITIVE (+UV scroll) |
| 5 | `Planet` | 296 | 99 | 201 | DX9 vs_1_1(83) / ps_2_0(35) | `Planet.hlsl` (atmosphere, clouds, city lights) |
| 6 | `MeshGloss` | 260 | 54 | 213 | DX8 vs_1_1(57) / ps_1_1(4) | `AlamoMesh.hlsl` GLOSS |
| 7 | `RSkinBumpColorize` | 203 | 152 | 70 | DX9 vs_1_1(80) / ps_2_0(24) | = #1 + SKINNED |
| 8 | `MeshGlossColorize` | 185 | 64 | 160 | DX8 vs_1_1(58) / ps_1_1(8) | `AlamoMesh.hlsl` GLOSS+COLORIZE |
| 9 | `Tree` | 185 | 80 | 174 | DX8 vs_1_1(63) / ps_1_1(5), atest 128 | `AlamoMesh.hlsl` BEND + ALPHATEST |
| 10 | `RSkinShadowVolume` | 182 | 96 | 125 | DX8 vs_1_1(30) / ps_1_1(1) | = #3 + SKINNED |
| 11 | `RSkinGloss` / `RSkinGlossColorize` | 146 / 69 | 88 / 49 | 69 / 54 | DX8 | same ubershader + SKINNED |
| 12 | `BatchMeshGloss` / `TerrainMeshBump` | 55 / 55 | 0 / 11 | 55 / 44 | DX8 / DX9 | same ubershader |

EaW counts have no hidden flag (the export lacks it), so EaW `MeshAdditive`/`MeshShield` are upper bounds:
in FoC, 34% of MeshAdditive submeshes are runtime-toggled muzzle flashes and engine glows.
| E1 | `Prim*` particle family | every frame | FF + vs_2_0/ps_2_0 depth sprites | `Particles.hlsl` |
| E2 | `TerrainRenderBump`, `TerrainRenderBumpDual`, `TerrainRenderBaked` | every land frame | DX8ATI / DX9 (108/46, the largest shader) | `Terrain.hlsl` |
| E3 | `TerrainWater` (+Ice, Lava) | land maps | DX9 vs_1_1(54) / ps_2_0(32) | `Water.hlsl` |
| E4 | `SceneBloom`, `StencilDarken*`, `SceneHeat` | every frame | ps_1_x / ps_2_0 | post / shadow passes |

**One ubershader covers 15+ effects.** Mesh*, RSkin*, Batch* and Tree share one vertex lighting
routine: per-vertex SH irradiance `n^T M n` with `m_sphAll`/`m_sphFill` (3 matrices = RGB), times
`Diffuse * m_lightScale`, plus `Emissive`; fog; `UVOffset`/scroll. They differ only in these bits:

- `SKINNED`: palette lookup;
- `BUMP`: tangent-space light and half vectors packed `*0.5+0.5` into TEXCOORD2/3, normal map, ps_2_0
  specular `pow(N.H, 16)` (fixed exponent: the DX9 path ignores `Shininess`) masked by the normal-map
  alpha;
- `GLOSS`: per-vertex specular (COLOR1, from `Specular`/`Shininess`) masked by base-texture alpha;
- `COLORIZE`: team color `lerp(base.rgb, base.rgb * Colorization.rgb, base.a)`;
- blend mode (opaque/fade, alpha, additive) and alpha test;
- `BEND`: tree/grass wind, from `m_bendVector` / `m_windGrassVector`, `BendScale`.

Port that ubershader first and validate it against the lifted `.hlsl` (`MESHBUMPCOLORIZE/*`,
`RSKINBUMPCOLORIZE/*`). It renders more than 85% of unit and ship geometry.

## 6. Validation plan

1. **Golden images.** Use the same camera, light and SPH inputs for the D3D9 original (RenderDoc capture,
   or a d3d9 proxy dumping constants) and the Vulkan port, per effect. Compare with a per-pixel tolerance
   of 2/255 (ps_1_x precision is about 8-10 bits, so allow more for `_bx2`-heavy DX8 paths if we port
   those).
2. **Constant-level check.** Log the CTAB-mapped register file per draw in both renderers and diff it.
   This catches missing semantics and wrong FXLC evaluation before any pixel is compared.
3. **Mod sweep.** Run every `.fxo` from the supported mods through the parser (no unknown ops allowed)
   and the translator. Log unknown `_ALAMO_*` tags, ALO params not declared by the effect (the CATALOG
   lists the vanilla ones), and missing techniques into the mod diagnostics log (Phase 5).
