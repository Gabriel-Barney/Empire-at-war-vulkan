# Terrain, water, post effects and map environments (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-26 by the RE agent. Builds on `docs/re/RENDER.md` (frame phases, buckets, render
targets) and does not repeat it. Clean-room: prose and tables only, no code and no pseudo-code from
the decompilation. Addresses are RVAs in StarWarsG.exe; names are the renamed names from
`docs/re/ghidra/functions.csv` where one exists (several are misattributed by Ghidra, for example most
terrain code is labelled `LandModeClass::sub_*`; the address is authoritative).

Confidence tags: **[H]** read directly in the code and cross-checked against data, **[M]** read in the
code but intent inferred, **[L]** inferred, needs a capture.

Data validation: 26 land maps and 24 space maps extracted from `corruption\Data\maps.meg` with
`tools/meg/megtool.py` into a scratch folder (never into the game folder). Per-map numbers are in §1.9
and §5.4.

---

## 0. Summary for the implementer (the findings that matter)

1. **A map is one chunk file (`.ted`) with four top-level chunks**: `0` map info, `3` start positions,
   `2` preview object list, `1` the world. The world chunk holds the environment set (`0x100`), the
   terrain (`0x101`), the object list (`0x102`), the playable bounds (`0x103`), the per-cell passability
   (`0x10A`), AI zones (`0x10B`) and the tactical camera (`0x10C`). Same chunk container as ALO (SPEC §1).
   [H] (loader 0x3b8cd0, terrain loader 0x132a30)
2. **Terrain = a square vertex grid of (31·n + 1)² vertices, 20 world units apart, origin at vertex
   (0,0), heights are signed 16-bit × 0.048828125 (= 1/20.48).** Each vertex is 4 bytes: height,
   material index, brightness. Vanilla maps: 187² … 373² vertices (6×6 … 12×12 patches). [H]
   (ctor 0x130b60, 0x135a70; all 26 land maps)
3. **There are no per-layer blend weights in the file.** Each vertex carries one material index
   (0..63 into the map's embedded 64-slot material table, the TEM format of SPEC §6.1). At load the
   engine builds, per 31×31-quad patch, one **draw pass per material used in that patch**, each with its
   own index list and its own **32×32 A4R4G4B4 blend texture** (1 texel per vertex, white where the vertex
   has that material, plus full white on every quad where that material is the lowest index among the 4
   corners). Passes draw in ascending material index with SRCALPHA/INVSRCALPHA; bilinear filtering of the
   blend texture produces one-quad-wide transitions. Up to 24 passes per patch in vanilla. [H]
   (0x1b62a0, 0x1b7f20, 0x1b9430)
4. **Terrain effect = `TerrainRenderBump`, technique `bump` (DX8ATI, vs_1_1 + ps_1_4) at shader detail
   ≥ 2**; `TerrainRenderBumpDual` is loaded but never drawn. Vertex format is the 36-byte FVF
   XYZ|NORMAL|DIFFUSE|TEX1; world matrix identity; texture coordinates are generated from world position
   (`diffuseTexU/V` from the material's tile size and two rotations, `blendTexScale/Offset` per patch).
   [H] (0x18fe80, 0x190070, 0x194ca0)
5. **Water = one plane per map** (height and type in the terrain header, types none/water/lava/ice),
   drawn in bucket 7 **per patch that dips below the water height** with a mesh built from the terrain
   grid (shore alpha from depth). Effect `TerrainWater` t0 (DX9) needs a **planar reflection render
   target and a refraction render target** (square, `Water_Render_Target_Resolution` = 512 in vanilla
   GameConstants, A8R8G8B8 + D24S8), a 128² sky cube render target, and two scrolling bump maps. [H]
   (0x18ee70, 0x192ea0, 0x193280, 0x1bb220, 0x3ba400, 0x35c9c0)
6. **The water bucket's `_ALAMO_RENDER_PHASE` string is `Water`; the water FXOs declare `TerrainWater`,
   which does not match** — irrelevant in practice because water tasks are hard-wired to bucket 7 by the
   terrain renderer, not by effect metadata (RENDER.md §7.1 q1 resolved). [H] (0x1b0690, string 0x818fdc)
7. **Bloom constants come from the map environment**: TEE minis `23` = BloomStrength, `24` =
   BloomCutoff, `28` = BloomSize (defaults 1.0 / 0.9 / 0.25), copied into the scene's post block and then
   into the render context for normal passes only. Many vanilla space maps store BloomStrength = 0, i.e.
   no visible bloom. Bloom and heat are enabled per frame only in land and space mode. [H]
   (0x183a30, 0x277830, 0x1b2ae0, 0x3bac70, 0x4d8fa0)
8. **`RESOLUTION_CONSTANTS` is the size of the currently bound engine render target, not the back
   buffer** (quarter size during bloom passes 0 and 1). All full-screen passes draw a 4-vertex FLOAT2-only
   strip at ±1.005 and generate UVs in the vertex shader with an explicit half-texel term (§4.2). [H]
   (0x1ad710, 0x1784f0, 0x17dc80)
9. **Stencil darken variant**: `StencilDarken` (multiply by the shadow colour where stencil & 0x4F ≠ 0)
   when Soft Shadows is off or shader detail is 0; otherwise `StencilDarkenToAlpha` writes 0 into the
   back-buffer alpha where shadowed, the back buffer is copied to `SceneTexture`, and
   `StencilDarkenFinalBlur` multiplies by a 4-tap alpha blur of it. The shadow colour is TEE mini `17`.
   [H] (0x183580, 0x183260, 0x17e1a0)
10. **Lighting per map comes from the active environment record** (TEE format embedded in the map):
    sun colour × intensity, yaw/elevation, two fills, ambient, specular, fog, clear colour, shadow colour,
    skydome object names, clouds, wind, bloom. Real values replace the guessed space defaults in
    engine/render: e.g. Kuat space sun (0.918,1,0.988) × 0.646 at yaw 217.1°, elevation 12.5°, ambient
    (0.027,0.051,0.055); Endor land "Sunrise_Clear" sun (1,0.729,0.357) × 0.646 at 220°/33°, ambient
    (0.145,0.184,0.188). [H] (0x273b10, 0x276d80, 0x277830; §5)

---

## 1. Map file (`.ted`)

### 1.1 Container and top level

Same chunk container as ALO (SPEC §1): u32 id, u32 size with bit 31 = has children; leaves often hold
mini-chunk lists (u8 id, u8 size, payload). Verified: every vanilla map parses to the exact end. [H]

| top chunk | kind | content | evidence |
|---|---|---|---|
| `0` | mini list | map info for the map browser/lobby: `00` u32 format (513), `01` u32 map type, `02`–`07` u32 (player count, start count, …), `08` UTF-16 map-name text key (e.g. `TEXT_MAP_NAME_MP_LAND_10`), `09`/`0A` small vectors, `10`/`11` f32 world width/height (land: (W)·20, e.g. 6220 for a 311 grid), `12` u8, `0B` u8 | data; reader is the map-list code around 0x47cb40 (`*.TED`) [M] |
| `3` | leaf | start positions: u32 count, then count × (f32 x, f32 y) | data (Endor: 2 points) [M] |
| `2` | node of `B` | preview/minimap object list: `0C` u32 object id, `0D` u32 type CRC, `0E`/`0F` f32 position normalised by the header world size | data; `0D` matches the object list (§1.6) [M] |
| `1` | node | the world, parsed by the mode's map loader (land 0x3b8cd0; space uses the same ids minus terrain) | [H] |

### 1.2 World chunk `1` children (land loader 0x3b8cd0)

| id | content | notes |
|---|---|---|
| `0x100` | environment set + scene fog block | §5. Children `4` (TEE records `6`… end `5`), `2` (scene fog), `8` (`25` u32 start environment index, `2A` u32 = −1). Old maps may carry `0` (colours) and `1` (intensities/angles) instead of `4`. [H] (0x2734c0) |
| `0x101` | terrain | §1.3. Created on first sight (alTerrain, 0x2E8 bytes, ctor 0x130b60). [H] |
| `0x102` | object list | §1.6, parsed through the mode vtable (+0x68). [H] |
| `0x103` | playable area / camera limits | minis: `00`,`01` f32 and `03` f32[3] go to the tactical camera object (+0x10, +0x14, +0x18); `02` and `04` f32 override camera ranges (+0x30/+0x34 and +0x44/+0x48) when not −1; `05` f32[3] min corner, `06` f32[3] max corner → playable box centre/half-extents (mode +0x418…+0x42c); objects outside are flagged. Endor: (300,1160,−35.9)…(5920,5060,56.9). [H] |
| `0x104` | legacy passability, u8 per cell (0→1, 1→2, 2→4, else −1) | not in vanilla FoC maps [H] |
| `0x105` | ignored by the land loader (space maps use `0x105` for the camera block) | [H] |
| `0x109` | handled by 0x3b9cc0 (not traced) | not in sampled maps [L] |
| `0x10A` | per-cell passability, u32 bit set, one per terrain vertex | §1.5 [H] |
| `0x10B` | AI zones ("New Zone 1" …: header, list of zone links, list of (u16 x, u16 y) cells) | mode vtable +0x170; not render-relevant [M] |
| `0x10C` | tactical camera initial state (≈40 minis: angles, position, distances 1250, FOV-like 55, …) | 0x2cea90; not render-relevant beyond the start view [M] |

Space maps (`_MP_SPACE_*`, `_SPACE_PLANET_*`) have `0x100`, `0x102`, `0x103` (minis `04`,`05`,`07`,`08`
float3 box corners, ±6000 typical) and `0x105` (camera); no terrain. [H] (data)

### 1.3 Terrain chunk `0x101` (loader 0x132a30)

| child | content |
|---|---|
| `0` | header minis (table below) |
| `5` | vertex data: vertex count (header `04`) × 4 bytes, row-major (index = y·W + x). Byte 0–1 signed i16 height, byte 2 material index, byte 3 brightness. Missing vertices default to height 0, material 0, brightness 0xFF. [H] |
| `1` | legacy vertex data (4-byte entries, only bytes 0–2 copied; brightness stays 0xFF). Not in vanilla FoC maps. [H] |
| `2` | 64 × `3`: the terrain material table, identical to the TEM record (SPEC §6.1); slot = material index. [H] (0x1338b0, 0x134b20) |
| `6` | terrain decals (alTerrainDecal, 0x248 bytes each, loader 0x14f6b0); empty in sampled maps [M] |
| `7`, `8` | two **water scroll records**, each a child `0x0AA45BCD` with minis `00`..`03` (f32 each) → terrain +0x280 and +0x290. Meaning: `00` tile size in world units, `01` rotation angle (rad), `02` scroll speed (UV units/s), `03` scroll direction (rad). Endor: (112, 0, 0.05, 0) and (150, 0, 0.05, 0). [H] layout (0x15df60), [M] names (§3.4) |
| `9` | terrain tracks (alTerrainTrack, 0x278 bytes each, loader 0x1547a0); empty in sampled maps [M] |

Header minis (`0x101/0`), all little-endian:

| mini | type | field (terrain offset) | vanilla values | conf. |
|---|---|---|---|---|
| `00`, `01` | u32 | vertex grid width W, height H (+0x10, +0x14) | 187, 218, 249, 280, 311, 373 (always square, always 31·n+1) | H |
| `04` | u32 | vertex count for child `5` (= W·H) | | H |
| `05` | u32 | material slot count (64) | | H |
| `0E` | string | cloud texture (+0xE8) | `W_Clouds00.tga` | H |
| `0F` | f32 | cloud scale in world units (+0x108; default 500) — overridden by the environment (§5.3) | 1000 / 999 / 500 | H |
| `10` | f32[3] | cloud scroll velocity, world units/s (+0x10C; default (5,5,0)) — overridden by the environment | (30,0,0) | H |
| `11` | u8 | use vertex brightness as vertex colour (+0x124) | 0 in every vanilla map | H |
| `15` | f32 | water plane height (+0x200) | −200 … 208 | H |
| `16` | u32 | water type (+0x204): 0 none, 1 water, 2 lava, 3 ice | | H |
| `17` | f32 | reflection fraction r (+0x2C0): reflect colour = water colour·r, refract colour = water colour·(1−r) | 0.34 … 1.0 | H |
| `18` | f32 | reflection distortion (+0x2C8), used ×0.02 | 0.5 | H |
| `19` | f32 | refraction distortion (+0x2CC), used ×0.02 | 0.5 | H |
| `1A` | f32[3] | water colour (+0x2A0) | (1,1,1), Dagobah (0.80,0.88,0.84) | H |
| `1B` | f32[3] | present in files, **not read** by this build | (1,1,1) | H |
| `1D` | string | water texture 0 (+0x220 → texture +0x208): bump map for water; base for lava/ice | `W_WaterBump00.tga` | H |
| `1E` | string | water texture 1 (+0x240 → +0x210): DX8/FF base ("fallback") texture; lava crust, ice bump | `w_water_fallback_blue.tga` | H |
| `1F` | string | water texture 2 (+0x260 → +0x218): lava crust bump | empty | H |
| `20` | f32 | water alpha for the DX8/FF techniques (+0x2C4, `WaterColor.a`) | 0.5 … 0.9 | H |
| `21` | f32 | shore fade depth d (+0x2D4; +0x2D8 = 1/d) | 10 | H |
| `22` | f32 | minimum shore alpha (+0x2DC) | 0 / 0.001 | H |

Empty water texture names load `w_generic_white.tga`. [H] (0x132a30 tail)

A header whose W/H differ from the current grid resizes it to the next 31·n+1 (0x131980). [H]

### 1.4 Vertex grid conventions

- World position of vertex (x, y) = (20·x, 20·y, height·0.048828125). Spacing 20.0 (+0x9C) and height
  scale 0.048828125 (+0xA4) are constructor constants, not file fields. [H] (0x130b60, 0x135a70)
  Check: Endor and Geonosis peak raw height is exactly 4096 → 200.0 world units.
- Positive X = increasing column, positive Y = increasing row; Z up (RH, RENDER.md §3.2). The map
  covers [0, 20·(W−1)] on both axes. [H]
- Out-of-range vertex lookups clamp to the border. [H]
- Byte 3 (brightness) is used only when header `11` is set; no vanilla map sets it (several maps contain
  a 156×156 block of zeros there — an editor artefact). Treat as 0xFF. [H]

### 1.5 Passability `0x10A`

- One u32 per terrain vertex/cell (count = W·H), **stored column-major: file index = x·H + y**
  (loader order 0x3b8cd0 case 0x10A). Verified on Endor and Hoth: under this order, Clear cells have mean
  slope 0.10–0.15 and Steep cells 0.29–0.46; the row-major reading gives no separation. [H]
- Values (enum `CellPassabilityType`, 0x5dd090): Invalid −1, None 0, Clear 0x1, Steep 0x2, Obstacle 0x4,
  RebelWall 0x8, EmpireWall 0x10, Water 0x20, Shield 0x40, InfantryOnly 0x80, Impassable 0x100. Vanilla
  maps use 0x1, 0x2, 0x20, 0x80, 0x100 only. [H]
- Rendering uses it only for the passability debug overlay (`TerrainPassability`, §2.7). [M]

### 1.6 Object list `0x102`

`0x102/1` holds one `0x44C` per object: `0x454` (minis `00`,`01` u32 ids), `0x459` → `0x4B0` placement
minis, `0x4B9`, `0x4BA` (77 hardpoint/attachment slots, mostly `NONE`), and `0x476` (owner/AI minis).
`0x4B0`: `00` u32 object id, **`01` u32 type = CRC-32 of the upper-cased GameObjectType name** (verified:
`Skirmish_Build_Pad`, `Skirmish_Mineral_Processor_Pad`, `Team_01_Power_Generator`), `02` u32 owner
slot, `03` f32 **health fraction** (corrected 2026-10-27; earlier "scale" was wrong — see note below), **`04` f32[3] position**, **`05` f32[3] rotation in degrees (Euler, z = yaw)**,
`12` copy of the rotation, `15` f32[3] secondary point (rally/flag), then flags. [H] for 01/04/05 (data),
[M] for the rest. Props (trees, rocks) are ordinary objects; there is no separate prop list. Endor: 1514
objects.

**`03` correction (2026-10-27, RE agent).** `03` is the object's current health divided by its maximum
tactical health, not a scale. The writer (0x3a5ad0) stores health (object +0x5c) / Get_Max_Tactical_Health
(0x3727a0: type Tactical_Health × difficulty factor). The placement parser (0x39ce40) copies the 4 bytes raw
into +0x5c (the "int" in the decompile is only a shared copy path; the field is an f32). After the object's
chunks are read, the map loader (0x2b0a20) calls the post-load fixup (0x3a0930), which multiplies +0x5c by the
max health and calls Set_Health (0x3a89d0, clamps to 0..max) and the land-damage-state update (0x3ac290).
[H] for writer/reader/fixup, [M] for the fixup gate (skipped when the loader's last flag is set). Values:
every object on space maps, Dagobah and Hoth is 1.0 (full health). FoC `_MP_LAND_ENDOR` has exactly 4
others, all `Skirmish_Mineral_Processor_Pad` (owner slot 3, ids 28/33/53/71), raw bytes `EC D6 A3 3D` =
f32 0.0799998 (u32 1034147564); the EaW copy has the same 4 pads with `F7 D6 A3 3D` (f32 0.0799999). With
the pad's Tactical_Health 10000 they start at ~800 HP. **Rendering:** `03` never touches the transform. The
only visual effect of health is via the type's `Land_Damage_Thresholds`/`Land_Damage_Alternates` (0x3ac290
picks a model alternate) and the 75%/50% damage-particle checks on later damage; the pad type
(`W_Pedistal.alo`, Immune_To_Damage) has no damage thresholds, so these 4 render identically to full
health. [H] for the pad data, [M] for "no visible effect". The terrain reader must not multiply `03` into
the scale; keep it as the initial health fraction for the sim.

### 1.7 Material table (TEM records inside the map)

SPEC §6.1 applies; runtime slot layout (alTerrain::Mtl, 0xB0 bytes, ctor 0x1345f0, loader 0x134b20):
diffuse texture (+0x00), diffuse colour `0A` (+0x28, default 1,1,1), specular colour `0B` (+0x34, default
1,1,1), specular power `06` (+0x40, default 64), tile size `07` (+0x44, default 100), rotation about Z `08`
(+0x48), rotation about X `09` (+0x4C), derived `diffuseTexU` (+0x50) and `diffuseTexV` (+0x60), flag `0D`
(+0x80), full coverage `12` (+0x81), not rendered `23` (+0x82), surface type `14` (+0x84), normal texture
(+0x88). [H]

- `diffuseTexU/V` (float4 each, applied to the world position (x, y, z, 1)): start from U = (1/tile, 0,
  0, 0) and V = (0, 1/tile, 0, 0), rotate both by `09` about the X axis (mixes y and z components), then
  by `08` about the Z axis (mixes x and y). With zero angles: u = x/tile, v = y/tile. [H] (0x134e80)
- Normal texture = `13` if non-empty and the low-memory option (global 0x140a626bc) is off, otherwise
  `Generic_Flat_B.tga`. [H]
- Diffuse alpha = specular mask in the shader (§2.5).
- Surface type `14`: the water types map to surface types 4 (water), 8 (lava), 7 (ice) for cells under
  the water plane (table 0x140a123c8). [H]

### 1.8 Other embedded data that is not render-relevant

Zones `0x10B`, camera `0x10C`, start positions `3`, map info `0`. Documented above for completeness.

### 1.9 Validation over the install (26 land maps)

Note (added 2026-10-26): the table below is read from `maps.meg`. The game loads patched copies of four maps from
the Patch/Patch2 MEGs, which differ: `_MP_LAND_DATHOMIR` (18 materials, 767 objects), `_MP_LAND_HOTH` (357
objects), `_MP_LAND_HYPORI` (1076 objects) and `_MP_LAND_UTAPAU` (29 materials, 8.8/20 layers, no water, 781
objects). Confirmed by engine/terrain's real-data test.

All 26 extracted land maps parse fully, have square grids of 31·n+1 vertices, 64 material slots, and
vertex data size = W·H·4. "Layers per patch" counts distinct rendered material indices over each patch's
32×32 vertices (= draw passes per patch, §2.3).

| map | grid | patches | world size | raw height min..max | materials used | layers per patch avg / max | water | env. records | objects |
|---|---|---|---|---|---|---|---|---|---|
| _LAND_PLANET_DATHOMIR_01 | 249² | 8×8 | 4960 | −1026..6966 | 23 | 9.5 / 17 | none | 1 | 2134 |
| _LAND_PLANET_GEONOSIS_02 | 187² | 6×6 | 3720 | −3071..6540 | 26 | 13.0 / 21 | water −80 | 4 | 206 |
| _MP_LAND_ALDERAAN | 187² | 6×6 | 3720 | −1217..7409 | 31 | 11.9 / 18 | water −2 | 2 | 550 |
| _MP_LAND_BESPIN | 280² | 9×9 | 5580 | 0..5389 | 4 | 1.0 / 3 | none | 1 | 796 |
| _MP_LAND_BOTHAWUI | 187² | 6×6 | 3720 | −1023..5580 | 44 | 15.0 / 24 | water 0 | 2 | 1555 |
| _MP_LAND_CORELLIA | 311² | 10×10 | 6200 | −1297..12200 | 27 | 8.2 / 14 | water −7.5 | 4 | 2333 |
| **_MP_LAND_DAGOBAH** | 249² | 8×8 | 4960 | −1535..2003 | 16 | 5.9 / 11 | water 0 | 2 | 1388 |
| _MP_LAND_DANTOOINE | 311² | 10×10 | 6200 | −2998..3372 | 28 | 9.8 / 14 | water −115 | 5 | 1103 |
| _MP_LAND_DATHOMIR | 249² | 8×8 | 4960 | −1023..6966 | 16 | 6.3 / 11 | none | 1 | 746 |
| **_MP_LAND_ENDOR** | 311² | 10×10 | 6200 | −437..4096 | 25 | 8.4 / 17 | water 0 | 4 | 1514 |
| _MP_LAND_FELUCIA | 249² | 8×8 | 4960 | −204..9085 | 13 | 6.8 / 13 | water 0 | 1 | 1885 |
| _MP_LAND_GEONOSIS | 311² | 10×10 | 6200 | −5078..4096 | 30 | 13.0 / 22 | none | 3 | 842 |
| _MP_LAND_HONOGHR | 249² | 8×8 | 4960 | −426..8288 | 24 | 10.2 / 14 | water −4 | 1 | 1295 |
| **_MP_LAND_HOTH** | 218² | 7×7 | 4340 | −545..5898 | 28 | 10.7 / 19 | none | 3 | 359 |
| _MP_LAND_HYPORI | 373² | 12×12 | 7440 | −5119..5421 | 27 | 8.1 / 14 | none | 1 | 1060 |
| _MP_LAND_KAMINO | 249² | 8×8 | 4960 | 0..5340 | 22 | 2.1 / 6 | water 7 | 1 | 821 |
| _MP_LAND_KESSEL | 280² | 9×9 | 5580 | −4844..3582 | 21 | 8.8 / 13 | none | 2 | 408 |
| _MP_LAND_MANDALORE | 311² | 10×10 | 6200 | −1317..7570 | 26 | 6.9 / 15 | water 0 | 3 | 3982 |
| _MP_LAND_NABOO | 249² | 8×8 | 4960 | −4065..5428 | 19 | 9.0 / 14 | none | 2 | 1697 |
| _MP_LAND_POLUS | 187² | 6×6 | 3720 | −1334..5845 | 30 | 9.3 / 20 | ice −0.01 | 3 | 183 |
| _MP_LAND_RYLOTH | 218² | 7×7 | 4340 | −3829..9363 | 25 | 10.6 / 17 | none | 1 | 1051 |
| _MP_LAND_SALEUCAMI | 218² | 7×7 | 4340 | 4064..8691 | 22 | 6.6 / 16 | water 208 | 2 | 1386 |
| _MP_LAND_SHOLA | 187² | 6×6 | 3720 | −2250..5119 | 23 | 12.7 / 17 | lava −16 | 2 | 251 |
| _MP_LAND_TATOOINE | 187² | 6×6 | 3720 | −2636..3637 | 28 | 12.5 / 18 | none | 2 | 1290 |
| _MP_LAND_UTAPAU | 311² | 10×10 | 6200 | 0..12384 | 6 | 2.1 / 4 | water 0 | 1 | 38 |
| _MP_LAND_YAVIN | 218² | 7×7 | 4340 | −1003..3071 | 17 | 7.0 / 11 | none | 2 | 1826 |

Endor, Dagobah and Hoth were checked field by field (header, environments, passability order, object
CRCs); the rest by structure and statistics. "materials used" counts distinct indices in the vertex
data (slots named in the table but unused are not counted).

---

## 2. Terrain mesh build and draw

### 2.1 Normals

Per vertex, a Sobel-style 3×3 gradient of the clamped neighbour heights (weights 1-2-1 across each axis,
over a 2-cell baseline in world units) gives the normal, which is normalised and then **quantised to a
byte**: the index of the entry with the largest dot product in a fixed 256-entry table (stored per vertex
at terrain +0x50). The table (built once, 0x231e70): entries 0..254 are points of a 2D Halton sequence
(base 2 for u, base 3 for v, starting at index 0) mapped to the upper hemisphere as
(√(1−v)·cos 2πu, √(1−v)·sin 2πu, √v); entry 255 = (0,0,1). Vertex normals are fetched back from that
table. [H] (0x135b80, 0x231d60, 0x231de0, 0x231e70); [M] for the Halton start index 0.
Port: emulate the quantisation for parity, or use the float normal (differences are small, visible as
faint banding on slopes in the original).

### 2.2 Patches, vertex and index buffers

- Patch = 31×31 quads = 32×32 vertices; patch (i, j) covers vertices x ∈ [31i, 31i+31], y ∈ [31j, 31j+31];
  patch count = ((W−1)/31) × ((H−1)/31), stored row-major. [H] (0x18e060, 0x136150)
- Per-patch static vertex buffer, FVF XYZ|NORMAL|DIFFUSE|TEX1 (0x152), stride 36: world position,
  quantised normal, colour = brightness grey with alpha 0xFF when header `11` is set else 0xFFFFFFFF,
  TEX0 = world (x, y) (unused by the shaders, which project from position). [H] (0x1b62a0)
- Shared static index buffer (0x2D0C bytes, 16-bit): all 31×31 quads, triangles (v, v+1, v+33) and
  (v, v+33, v+32) with v = row·32 + column → winding (0,0)→(1,0)→(1,1), (0,0)→(1,1)→(0,1). [H] (0x18e060)
- A second per-patch vertex buffer of the same format carries debug colours for the passability overlay
  (+0x28). [M]
- **No LOD, no skirts, no stitching**: every patch is always drawn at full resolution. [H]
- Patch AABB from its vertices; patches are culled individually against the camera's full-far frustum
  (camera +0x1FC) each frame. [H] (0x18ee70)

### 2.3 Layer passes and blend textures (the splatting scheme)

At patch build (0x1b62a0):

1. Collect the material indices of the patch's 32×32 vertices, skipping slots flagged "not rendered"
   (`23`); sort ascending. One **pass** per index (0x1b7f20).
2. Each pass gets its own **16-bit index list** of the quads it touches: a quad is included when any of
   its 4 corners has the pass's material.
3. Each pass gets a **32×32, 1-mip, A4R4G4B4 managed texture** (texel (c, r) ↔ vertex (31i+c, 31j+r)),
   cleared to 0, then set to 0xFFFF (alpha 1):
   - at every vertex whose material equals the pass's material;
   - at all 4 corners of every included quad whose **lowest material index among its 4 corners** equals
     the pass's material, or of every included quad if the material has the full-coverage flag (`12`).
4. An extra "all rendered quads" index list (material −1, no texture) is kept only when the patch has
   holes (not-rendered materials); it is used by the decoration passes. [H]

Blend UV per patch: the patch's XY extent maps linearly to [1/64, 63/64] so that texel centres land on
vertices (`blendTexScale` = 62/64 / extent, `blendTexOffset` = 1/64 − min·scale, set per patch as float2).
[H] (0x1b62a0, constants 0.015625 / 0.984375)

Draw (per visible patch, passes in ascending material index): ALPHABLENDENABLE = 1, SRCBLEND =
SRCALPHA, DESTBLEND = INVSRCALPHA, set by the engine before every pass (the effect pass itself only sets
Z: ZENABLE, ZWRITEENABLE = 1, LESSEQUAL); DrawIndexedPrimitive over 1024 vertices with the pass's
triangle count. The lowest-index material of each quad is therefore opaque on that quad and higher
materials fade in over one quad. [H] (0x1b9430, render-state immediates 27/1, 19/5, 20/6)

### 2.4 Terrain effect and techniques

- Effects loaded by the terrain renderer (0x18fe80): `TerrainRenderBump`, `TerrainRenderBumpDual`,
  `TerrainPassability`, `TerrainWater`, `TerrainLava`, `TerrainIce`, `TerrainFogOfWar`,
  `TerrainRenderBaked`. [H]
- Draw path: `TerrainRenderBump` for normal frames; `TerrainRenderBaked` for the reflection/refraction
  passes (§2.8); `TerrainPassability` for the passability view. **The dual-layer path
  (`TerrainRenderBumpDual`, 0x1b5cf0) has no caller** — port as not needed. [H] (callgraph)
- Technique by shader detail (RENDER.md §6): `bump` (LOD DX8ATI = level 2, vs_1_1 81 + ps_1_4 21) at
  detail ≥ 2; `nobump` (DX8, ps_1_1) at 1; `t3` FIXEDFUNCTION at 0. [H]

`bump` shading, in words (CATALOG/lifted HLSL): world = identity, object = world space. Diffuse and
normal maps use `diffuseTexU/V`; a tangent frame is derived from the U/V projection axes and the vertex
normal; light and half vectors go to the pixel shader in that frame. Colour = (N·L·materialDiffuse·
light0Diffuse + SH fill irradiance·materialDiffuse) × diffuse × 2 + diffuse.a × (N·H)^16 ×
light0Specular·materialSpecular, then × cloud texture; alpha = blend texture alpha. Vertex fog from
`FOG_VALS`. [H]

### 2.5 Parameters the engine sets for terrain draws

| parameter | value / source | when | evidence |
|---|---|---|---|
| `diffuseTexture` | Mtl diffuse texture | per pass | 0x1b9430 [H] |
| `normalTexture` | Mtl normal texture (or `Generic_Flat_B.tga`); skipped if the technique has no such parameter | per pass | [H] |
| `blendTexture` | the pass's A4R4G4B4 texture | per pass | [H] |
| `diffuseTexU`, `diffuseTexV` | Mtl +0x50 / +0x60 (§1.7) | per pass | [H] |
| `materialDiffuse` | (Mtl diffuse colour, 1) | per pass | [H] |
| `materialSpecular` | (Mtl specular colour, 1) | per pass | [H] |
| `shininess` | Mtl specular power (unused by `bump`, which hard-codes ^16) | per pass | [H] |
| `blendTexScale`, `blendTexOffset` | per patch (§2.3) | per patch | 0x1b5c10 [H] |
| WORLD | identity | per task | 0x194ca0 [H] |
| `CLOUD_TEXTURE` | terrain cloud texture (header `0E`, or environment `2F`) | per frame | 0x18ee70, 0x196c10 [H] |
| `CLOUD_TEX_U/V` | (1/s, 0, 0, −ox/s) and (0, 1/s, 0, −oy/s): s = cloud scale, (ox, oy) = cloud offset accumulated as offset += velocity·dt each scene update | per frame | 0x18ec30, 0x1322d0 [H] |
| everything else | engine semantics as in RENDER.md §2.2 (SPH, light 0, fog, FOW) | | |

Material defaults in the FXO apply only to parameters the engine does not set (none of the above).

### 2.6 Submission and order

`alD3dTerrainRender::vf2` (0x18ee70), per scene render:
- bucket 0 (Terrain): one `alD3dTerrainRenderTask` (all visible patches, row-major patch order, all
  passes of a patch consecutively), gated by debug toggle 0x140a123a0 (default on); [H]
- bucket 0, normal passes only, each gated by a debug byte (0x140a12ad0…ad3, all default on), submitted
  in this order: decoration 3 (land tracks) and 2 (dynamic tracks), 1 (decals, `D_master_decal.tga`), 0
  (terrain shadow blobs), 4 (fog of war, `TerrainFogOfWar`); [M for the type names 0, 2, 3]
- bucket 7 (Water), normal passes only, when the water type ≠ 0 and toggle 0x140a123a1: the
  `alD3dWaterRenderTask` then decoration 5 (tracks flagged as on water). [H]
Decals, tracks and FOW add 0.3 to the camera near and far planes before drawing (a small depth offset)
and restore them afterwards. [H] (0x1946f0)

### 2.7 Fog of war on terrain and the passability view

- FOW is a terrain decoration, not a full-screen pass: each visible patch is redrawn with
  `TerrainFogOfWar` (DX9 vs_2_0/ps_2_0 filtered lookup of `FOW_TEXTURE`, blend DESTCOLOR/ZERO), only if
  the FOW option (0x140a12501) is on. [H]
- Passability view (terrain +0xB9/+0xBA set): `TerrainPassability` two passes with `materialDiffuse` =
  terrain colour at +0xA8, `materialSpecular` = 0, drawing the debug-colour vertex buffer. [M]

### 2.8 Baked terrain (reflection passes and tactical overview)

At load (and on change) every patch is rendered once from above (orthographic, near 10, far 2000, ambient
0.6) through the normal splatting passes into a square render target (terrain renderer +0x3F0 size,
A8R8G8B8) and copied into a mip-mapped texture per patch (0x1b7690). `TerrainRenderBaked` then draws one
textured pass per patch:
- in the special scene passes (water reflection/refraction, pass flags 2 or 4), and
- when the "use baked terrain" flag (0x140a62649) is set — set by the tactical camera when it enters the
  zoomed-out overview, cleared on return and on mode start. [H] for the switch (0x194ca0), [M] for the
  overview trigger (0x2cf2f0)

---

## 3. Water

### 3.1 Plane and geometry

- One plane per map at height header `15`; drawn only when the water type (`16`) is 1, 2 or 3. [H]
- Geometry built at patch build for every patch whose minimum Z is below the water height (0x1bb220):
  over the patch's 31×31 quads, a quad is included when any corner is below the water height or has a
  not-rendered material; its vertices sit at the terrain vertex XY with Z = water height. Vertex format
  `alD3dVertNC` (32 bytes): position, normal (0,0,1), colour = white with **alpha = shore factor**,
  second colour white. Shore factor = clamp(m + (1−m)·clamp((waterH − terrainH)/d, 0, 1), 0, 1) with d =
  header `21` (10) and m = header `22`. [H] (0x1bb220, 0x136ab0)
- Drawn per patch (identity world) after the effect is begun once; water type selects the effect. [H]
  (0x192ea0)
- A 17×17-vertex grid with patch-sized extent (0x190520) also exists; it is not used by the water task in
  this build. [M]

### 3.2 Effect selection and WaterLOD

- Type 1 `TerrainWater`, type 2 `TerrainLava`, type 3 `TerrainIce`. All three declare
  `_ALAMO_RENDER_PHASE = "TerrainWater"` and techniques `t0` (DX9, WaterLOD 2), `t1` (DX8, WaterLOD 1),
  `t2` (FIXEDFUNCTION, WaterLOD 0). [H] (FXO)
- Technique selection rejects a technique whose `WaterLOD` annotation exceeds the Water Detail option
  (global 0x140a12674); shader detail < 3 caps water detail at 1, < 1 forces 0 (RENDER.md §1.5). [H]
  (0x1aead0)
- A water task is in bucket 7 regardless of the effect's phase string (the effect phase string would map
  to Opaque because the bucket-7 comparison string is `Water`). [H]

### 3.3 TerrainWater t0 (DX9) inputs

| parameter | value / source | evidence |
|---|---|---|
| `Bump0Texture`, `Bump1Texture` | both = water texture 0 (header `1D`, `W_WaterBump00.tga`) | 0x193280 [H] |
| `ReflectionTexture` | reflection render target (§3.5) | [H] |
| `RefractionTexture` | refraction render target (§3.5) | [H] |
| `BaseTexU/V` | scroll record 7 (§3.4), unrotated | [H] |
| `Bump0TexU/V` | scroll record 7 with direction +45° | [H] |
| `Bump1TexU/V` | scroll record 7 with direction −45° | [H] |
| `ReflectionDistortionScale` | (h18·0.02, h18·0.02) | [H] |
| `RefractionDistortionScale` | (h19·0.02, h19·0.02) | [H] |
| `WaterReflectC` | (waterColour·r, 1), r = header `17` | [H] |
| `WaterRefractC` | (waterColour·(1−r), 1) | [H] |
| `m_texOffset` | **not set by the engine**: FXO default (scale 0.5, −0.5, bias 0.5, 0.5 — no half-texel term) | strings: no `m_texOffset` reference [H] |
| `SKY_CUBE_TEXTURE` | the land sky cube render target (§3.5) | 0x3ba400 [M] |
| `FOW_TEXTURE/U/V` | as RENDER.md | [H] |
| blend | effect pass: SRCALPHA/INVSRCALPHA, Z test + write | FXO [H] |

Shader behaviour in words: two bump samples (each ×2−1) are summed, a normal is built with the Z
component weighted ×15 and normalised; the eye vector (in the base-texture tangent frame) is reflected
about it to sample the sky cube; the bump XY × distortion scale × 200 offsets the projective reflection
and refraction lookups (clip-space position × `m_texOffset`); colour = sky × (reflection·reflectC +
refraction·refractC) × FOW; **alpha = vertex shore alpha**. [H] (lifted HLSL)

t1 (DX8): `Bump0Texture` = a bump-environment texture converted from water texture 0 (renderer +0x88,
built when water detail = 1), `BaseTexture` = water texture 1, reflection only, stage-1 BUMPENVMAT00/01/
10/11 = rotation by (water time × 3.14) scaled by h18·0.02, `WaterColor` = (colour, header `20`). t2 (FF):
`BaseTexture` = water texture 1, `BaseFFTM` = texture matrix from scroll record 8 and the inverse view,
`WaterColor` as t1. [H]

Lava (type 2): `BaseTexture` = texture 0, `CrustTexture` = texture 1, `CrustBumpTexture` = texture 2,
`CrustSpecular` = (water colour, 1), Base UV from record 7, Crust UV from record 8 (FF: `BaseFFTM`). Ice
(type 3): `IceTexture` = texture 0, `IceDiffuse` = (water colour, 1), `ReflectionTexture`, `BumpTexture` =
texture 1, Base UV record 7, Bump UV record 8, `IceReflectionDistortion` = h18 (not ×0.02). [H] (0x1938f0,
0x193b40)

### 3.4 Scroll records and time

Record (tile s, angle a, speed v, direction d) at water time t gives U = (cos a / s, sin a / s, 0,
v·cos d·t) and V = (−0, cos a / s, 0, v·sin d·t) (the V row's X term is written as negative zero in this
build, so the angle only rotates U — reproduce as is). Water time = terrain +0x2E0, advanced by the scene
update dt (clamped to ≤ 1 s per update) together with the cloud offset. [H] (0x15d880, 0x1322d0,
0x199ea0); [M] for the field names.

### 3.5 Extra passes and targets

Created by 0x35c9c0 at land mode setup:

| target | size | format | use |
|---|---|---|---|
| reflection RT (0x140b2c1f8) | N×N, N = clamp(`Water_Render_Target_Resolution`, 256, 1024); vanilla GameConstants 512 | A8R8G8B8 | `ReflectionTexture` |
| refraction RT (0x140b2c200) | N×N | A8R8G8B8 | `RefractionTexture` |
| shared depth (0x140b2c208) | N×N | D24S8 | both |
| sky cube RT (0x140b2c210) + depth (0x140b2c218) | 128×128 per face | A8R8G8B8 / D24S8 | `SKY_CUBE_TEXTURE` for water |

Per frame, before the land world render (0x3ba400, called from the land mode update):
1. **Sky cube**: re-rendered when the environment changed (0x3bc590): a temporary scene with the
   current light record containing only the two skydome objects of the environment (TEE `19`/`1A`), at the
   origin, cleared to the environment clear colour, rendered into all 6 faces; then bound as
   `SKY_CUBE_TEXTURE`. If the cube target failed, water detail is lowered instead. [M]
2. **Reflection** (only if the active water/ice technique uses the REFLECTION-semantic texture, effect
   +0x168 ≠ −1, 0x1af630): camera mirrored about z = waterH, user clip plane (0,0,1) at waterH −
   `Water_Clip_Plane_Offset` (0.0 in vanilla XML; GameConstants default 10), clear colour = environment
   clear colour — or white with the skydome objects temporarily shown when the water-detail-2 path is
   active (renderer vf12) — scene rendered through SceneClass::vf10 with argument 1 = alScene pass flag 2
   (bucket mask 0x12B, baked terrain, no decorations, no water, no heat/bloom). [H] (0x267c90 maps
   argument 0/1/2 to flag 1/2/4)
3. **Refraction** (only if the technique uses REFRACTION, effect +0x16C > 0): same camera, clip plane
   (0,0,1) at waterH + offset, environment clear colour, argument 2 = pass flag 4. [H]
4. Camera restored; the main scene renders normally and the water task samples both targets. [H]

---

## 4. Post and full-screen passes

### 4.1 Targets (addendum to RENDER.md §1.5)

Sizes are (int)(back-buffer width × scale), (int)(height × scale) taken from the back-buffer surface
description (0x1980c0); all A8R8G8B8, no depth of their own. [H]

| target | scale | content |
|---|---|---|
| `SceneTexture` (0x140a6fdd8) | 1 | back-buffer copy: after Transparent (heat/bloom input), and again after the stencil darken when Soft Shadows is on |
| `DistortionTexture` (0x140a6fd18) | 1 | back-buffer copy after the Heat bucket |
| transition (0x140a6fe30) | 1 | snapshot for frame effects (§4.6) |
| bloom A, B (0x140a6fd88/90) | 0.25 | bloom chain |

Back-buffer copies use StretchRect with point filtering (0x198380). Clears: frame clear colour alpha is
always 0xFF (0x13cf40); the heat clear is 0x008080FF (A=0, R=G=0x80, B=0xFF). [H]

### 4.2 Full-screen geometry and texcoord convention

- Vertex buffer (0x17dc80): 4 vertices, FLOAT2 position only (declaration: stream 0, offset 0, FLOAT2,
  POSITION), at (−1.005,−1.005), (1.005,−1.005), (−1.005,1.005), (1.005,1.005), drawn as a 2-triangle
  strip (DrawPrimitive TRIANGLESTRIP, 0, 2). The vertex shaders output (x, y, 0.5, 1). [H]
- UVs are generated in the vertex shaders: uv = (x·0.5 + 0.5, −y·0.5 + 0.5) plus a half-texel term from
  `RESOLUTION_CONSTANTS.zw` = (0.5/width, 0.5/height) of the **currently bound engine render target**
  (back buffer when none). SceneBloom **adds** the half texel; SceneHeat **subtracts** it (both
  techniques). [H] (0x1ad710, 0x1784f0, lifted VS) Exception: SceneHeat's `t1_ati`, drawn on AMD adapters
  (technique `t0` has `VENDOR_FILTER_0=4098`), subtracts only **¼** texel in V (g1f t06 capture, RENDER.md §7.3).
- Vulkan: pixel centres are already at +0.5, so the D3D9 +½-texel correction must be dropped for the
  bloom passes (sample at exactly uv) and SceneHeat's −½ texel becomes a −1 texel shift relative to
  Vulkan centres; keep it for parity. Use a single full-screen triangle and reproduce the ±1.005 overscan
  by clamping. [M]
- The stencil-darken passes use a different path: 6 pre-lit prim vertices (two triangles) with an
  orthographic camera (identity view, l/r/b/t −1/1/−1/1, near 0.1, far 2) and UVs (0,1) at (−1,−1) …
  (1,0) at (1,1), no half-texel term. [H] (0x183580)

### 4.3 Stencil shadows: darken and soft blur

After the Shadow bucket (RENDER.md §1.4): z-bias reset, then 0x183580:
1. Vertex colour = scene shadow colour (TEE `17` via the scene; default (0.5,0.5,0.5)) as RGB, converted to
   bytes by truncation (Kuat 0.5490196 → 0x8B, g1f t06 capture), alpha 0;
   texture stage 0 unbound; draw with prim mode 8. [H]
2. Mode 8 picks **`StencilDarken`** when Soft Shadows (0x140a626ba) is off or shader detail is 0:
   FIXEDFUNCTION, blend ZERO/SRCCOLOR (back buffer × shadow colour), stencil test NOTEQUAL ref 0 with
   read mask 79 (0x4F), write mask 0, Z ALWAYS, colour write RGBA. Otherwise **`StencilDarkenToAlpha`**:
   opaque, colour write **A only**, stencil NOTEQUAL ref 0 mask 63 (0x3F): writes alpha 0 where
   shadowed. [H] (0x183260; FXO)
3. Soft shadows only: copy the back buffer to `SceneTexture`, bind it to samplers 0–3 (texture only; the
   effect samplers have no states, so the D3D9 defaults/previous state apply), draw mode 9
   **`StencilDarkenFinalBlur`** (DX9): 4 taps at uv + 0.0015 × {(−1,0),(0,1),(1,0),(0,−1)}, a = mean
   alpha, output = lerp((shadow colour, 0), (1,1,1,0), a), blend ZERO/SRCCOLOR, stencil off; afterwards
   colour write is restored to RGB. Unbind samplers 0–3. [H]
Requirement for parity: unshadowed back-buffer alpha must stay 1, which holds because the frame clear
alpha is 0xFF and the `Engine\Global` baseline writes RGB only. [M]

### 4.4 Heat distortion

Order and clears are as in RENDER.md §1.4/1.5 and confirmed. Details: `SceneHeat` technique `t0`
carries `VENDOR_FILTER_0 = 4098` (0x1002, AMD/ATI) so AMD uses `t1_ati` (identical except that the V
offset is half as large: −¼ texel instead of −½); parameters set by the engine are only `SceneTexture` and `DistortionTexture`;
`DistortionAmount` stays at the FXO default 0.25. Shader: offset = (distortion.rg − 0.5) ×
2·DistortionAmount, scene sampled at uv + offset with CLAMP; opaque, Z ALWAYS. **The heat → transparent
remap flag (0x140a626bb) has no writer in this build, so heat is never remapped** (RENDER.md §7.1 q9). [H]
(0x180dc0, 0x13ec90)

### 4.5 Bloom

Enabled when the Bloom option (0x140a12503) is on and the render context's bloom byte is set; the
context gets the scene's post block (+0x210..+0x21C) on normal passes and defaults with bloom/heat off on
special passes (0x1b2ae0). Land and space mode set the scene's heat and bloom bytes every frame
(0x3bac70, 0x4d8fa0); galactic mode does not, so **galactic has no bloom or heat**. [H]

| step | target | effect pass | inputs and constants |
|---|---|---|---|
| save and unbind depth-stencil | | | |
| 1 | bloom A (¼) | `SceneBloom` p0 bright filter (ps_1_3) | `SceneTexture` = post-Transparent copy; `BloomCutoff` = context +0x24; `BloomStrength` (+0x28) and `BloomSize` (+0x2C) also set. Output = scene colour where luminance (0.299, 0.587, 0.114) > cutoff, else colour⁵. |
| 2…5 | B, A, B, A (iterations 0..3, count global 0x140a129f4 = 4) | p1 blur (ps_1_1) | `BloomIteration` = 0,1,2,3; `SceneTexture` = previous target. Four taps at uv ± (2i+1)·halfTexel·BloomSize along both diagonals, averaged. halfTexel = 0.5/quarter-size. |
| restore depth-stencil, clear Z = 1 and stencil 0 | | | |
| 6 | back buffer | p2 combine (ps_1_1) | `SceneTexture` = last target (A after 4 iterations); output = bloom × BloomStrength; blend ONE/INVSRCCOLOR (screen). |

Constant values: BloomCutoff = TEE `24`, BloomStrength = TEE `23`, BloomSize = TEE `28` of the active
environment (defaults 0.9, 1.0, 0.25; table in §5.4). ps_1_x precision applies (values clamp to [−1, 1]
inside the pixel shader). [H] (0x183a30 operand decode, 0x1a2080, 0x277830)

### 4.6 Other full-screen effects

- Frame effects (state 0x140a6fe80, driven by cinematic/transition code, 0x181c50): 1 =
  `FrameEffectWipe` (linear wipe from the transition snapshot, `WipeFraction`, gradient texture), 2 =
  `FrameEffectAlpha` (cross-fade from `PrevSceneTexture`), 3 = `FrameEffectFakeMotionBlur` (blend
  `PrevFrameTexture` with `BlendFactor` (0.95) / `RadialBlur`, then snapshot the back buffer for the next
  frame). Snapshot = StretchRect into the transition target (0x181c20). [M]
- Letterbox: Lua `Letter_Box_In/Out` and `Camera_FX_Manager_Letterbox_Height` (GameConstants); drawn by
  the camera FX manager, not traced (no dedicated effect found). [L]
- Gamma: profile option `Gamma`; no shader pass found — presumably the device gamma ramp. [L]
  > **Status 2026-10-30:** The port implements gamma as a full-screen GPU pass over the finished frame, a [port] choice (`engine/render/src/gamma.cpp`, `gamma.hpp`, `engine/render/shaders/gamma.hlsl`). The original's mechanism is still unconfirmed.
- Full-screen FOW: none on land (FOW is a terrain decoration, §2.7); space FOW uses `SpaceFogOfWar`
  (Transparent), drawer not traced. [L]

---

## 5. Environment and lighting

### 5.1 Where it lives

- Each map embeds its environment set in `0x100/4` (TEE records, SPEC §6.2) plus a scene-fog block
  `0x100/2/0` (`00` colour, `01` enable, `02` start, `03` end → SceneClass +0x68/+0x74/+0x60/+0x64) and the
  start index `0x100/8` mini `25`. Records are loaded into `RtsSceneClass::Environment` (0x198 bytes,
  table at RtsScene +0x88, current index +0xA0) by 0x273b10. [H]
- Lua `Set_New_Environment(index)` / `Set_Cinematic_Environment(bool)` switch at runtime (strings
  0x899170). [H]

### 5.2 TEE mini → environment field → use

| mini | field (env offset) | used for | default when absent | conf. |
|---|---|---|---|---|
| `00`,`01`,`02` | diffuse colour of sun, fill 1, fill 2 (+0x84, +0x90, +0x9C) | light record diffuse | (1,1,1) | H |
| `03` | sun specular (+0x60); fills get 0 | light 0 specular | (1,1,1) | H |
| `04` | ambient (+0x6C) | light record ambient, SH | (0,0,0) | H |
| `05`,`06`,`07` | intensities (+0x78, +0x7C, +0x80) | | 0.75 / 0.5 / 0.5 (ctor) | H |
| `08`,`09`,`0A` | yaw a (rad) (+0xA8…) | direction | 0 / 120° / 210° | H |
| `0B`,`0C`,`0D` | elevation e (rad) (+0xB4…) | direction = (cos e·sin a, −cos e·cos a, sin e), towards the light (+0xC0…) | 45° / −10° / −10° | H |
| `0F` | fog colour (+0xEC) | scene fog | (1,1,1) | H |
| `10` | fog enable (+0xF8) | | 0 | H |
| `11`,`12` | fog start/end (+0xE4/+0xE8, stored min/max) | | 1000 / 10000 | H |
| `13` | int (+0x100), clamped ≥ 0 | **weather type**: CLEAR 0, RAIN 1, SNOW 2, SANDSTORM 3, ASH 4; read by the typed random environment pick 0x2770e0 (land vf27, a synced pick among the environments of that type; no caller found) (CU8F_T03_T05_RE §2.2, 2026-11-07) | 1 | H |
| `14` | name | | "Environment N" | H |
| `15` | **clear colour** (+0x104) | frame/camera clear, reflection clear, sky-cube clear | (0.3,0.3,1.0) | H |
| `17` | **stencil shadow colour** (+0x110) → scene +0x1E4 | §4.3 | (0.5,0.5,0.5) | H |
| `19`,`1A` | skydome object type names (+0x20, +0x40) | background objects; sky cube | empty | M |
| `1B`,`1C` | skydome scales (+0x120, +0x12C); reset to (1,0,0) when the name is empty | | 1 | M |
| `1D`,`1F` / `1E`,`20` | skydome 1 / 2 angles in degrees (+0x124/+0x128, +0x130/+0x134) | | 0 | M |
| `21` | lightning flash intensity (+0x138): random white SH flashes with random timing | 0x277570 | 0 | M |
| `23` | **BloomStrength** (+0x140) | §4.5 | 1.0 | H |
| `24` | **BloomCutoff** (+0x13C) | §4.5 | 0.9 | H |
| `28` | **BloomSize** (+0x144) | §4.5 | 0.25 | H |
| `29` | weather effect name (+0x148) | | empty | M |
| `2B` | wind direction, degrees (+0x168) | cloud direction, scene wind | 0 | H |
| `2C` | wind speed (+0x16C) → scene wind vector (+0x1AC) = speed·(cos w, sin w, 0) | WIND_* params | 2 | H |
| `2D` | cloud scale (+0x170) | terrain cloud scale | 500 | H |
| `2E` | cloud speed (+0x174) → terrain cloud velocity = speed·(cos w, sin w, 0) | | 30 | H |
| `2F` | cloud texture (+0x178) | terrain cloud texture | `W_Clouds00` | H |

### 5.3 Application (0x276ff0 → 0x2774f0, 0x277830; land also 0x3bbed0)

On load end and on every environment change:
1. light record (scene +0x280, RENDER.md §2.3): ambient = `04`; light 0 = sun (`00`, `03`, `05`, angles);
   lights 1 and 2 = fills (colours `01`/`02`, specular 0, intensities `06`/`07`); SH9 = ambient + both
   fills (+ lightning flash when active). At shader detail 0 (fixed function) an extra RtsScene ambient
   (+0x12C) is added. [H]
2. fog: environment fog when the scene uses environment fog (RtsScene +0x78), otherwise the scene's own
   fog block; forced off by a global option (0x140b0a0d1). [M]
3. scene post block: bloom cutoff/strength/size from `24`/`23`/`28`; shadow colour from `17`. [H]
4. land only: terrain cloud scale/velocity/texture from `2D`/`2E`/`2B`/`2F` (overriding the terrain
   header), scene wind from `2C`/`2B`. [H]
5. Space mode zeroes the fill intensities and disables stencil shadows on its background scene only;
   the main space scene keeps both (corrected 2026-10-27, SPACE_AND_OPEN.md §A; RENDER.md §1.2). [H] The port
   draws main-scene space shadows since g1r t04 (2026-11-01).

Default record (ctor 0x271d20) for a scene without environments: sun white × 0.75 at yaw 0°,
elevation 45°; fills grey 0.75 × 0.5 at yaw 120°/210°, elevation −10°; ambient black; specular white;
fog off, 1000…10000, white; clear (0.3,0.3,0.7); shadow colour (0.5,0.5,0.5); bloom 0.9/1.0/0.25. [H]

### 5.4 Real values (first environment record of each map)

| map | env 0 | sun colour × intensity | yaw / elev (deg) | ambient | fill int. | bloom str/cut/size | fog |
|---|---|---|---|---|---|---|---|
| _MP_LAND_ENDOR | Sunrise_Clear | (1.000,0.729,0.357) × 0.646 | 220 / 33 | (0.145,0.184,0.188) | 0.33 / 0.50 | 1.0 / 0.9 / 1.0 | on, 0..3500, colour (0.729,0.812,0.722) |
| _MP_LAND_DAGOBAH | Sunrise_Rain | (0.851,0.706,0.510) × 0.448 | 220 / 33 | (0.145,0.184,0.188) | 0.33 / 0.50 | 1.0 / 0.9 / 1.0 | on, 0..3000 |
| _MP_LAND_HOTH | Sunrise_Snow | (0.851,0.706,0.510) × 0.573 | 220 / 31 | (0.145,0.184,0.188) | 0.33 / 0.50 | 1.0 / 0.9 / 1.0 | on, 0..3500 |
| _MP_LAND_TATOOINE | Sunrise_Sandstorm | (1.000,0.729,0.357) × 0.646 | 220 / 33 | (0.145,0.184,0.188) | 0.33 / 0.50 | 1.0 / 0.9 / 1.0 | on, 0..3500 |
| _MP_SPACE_KUAT | Environment 1 | (0.918,1.000,0.988) × 0.646 | 217.1 / 12.5 | (0.027,0.051,0.055) | (zeroed in space) | 1.0 / 0.9 / 0.25 | off |
| _MP_SPACE_CORUSCANT | Environment 1 | (1,1,1) × 0.649 | 218.1 / 21 | (0.059,0.059,0.059) | – | 0.6 / 0.9 / 0.25 | off |
| _MP_SPACE_ENDOR | Environment 1 | (0.816,0.878,0.812) × 0.660 | 138.1 / 32.5 | (0.059,0.094,0.094) | – | 1.0 / 0.9 / 0.25 | off |
| _MP_SPACE_ALDERAAN | Environment 1 | (1,1,1) × 0.500 | 153 / 35 | (0.039,0.039,0.039) | – | **0.0** / 0.9 / 0.1 | off |
| _MP_SPACE_UTAPAU | Environment 1 | (1,1,1) × 0.500 | 0 / 45 | (0,0,0) | – | **0.0** / 0.9 / 0.1 | off |

Across the 24 MP space maps: sun intensity 0.35–0.75 (median 0.5), elevation 12.5°–45°, ambient
0–0.13 per channel; 13 of 24 store BloomStrength 0. Endor land carries 4 records (Sunrise/Noon × Clear/Rain); the
Noon records use yaw 317°, elevation 44°, ambient 0, clear colour white. Sky objects: Endor
`DAY_STORM_SKY`; Kuat `STARS_LOW` + `STAR_BACKDROP_GREEN_ORANGE_2` (second skydome rotated by 210°). [H]

Recommended engine/render defaults until environments are wired: space = Kuat env 0 (above, sun
specular (0.455,0.667,0.796), fills off); land = Endor "Sunrise_Clear" with fills (0.745,0.733,0.851) ×
0.33 at yaw 349°, elev −18° and (0.851,0.808,0.733) × 0.5 at yaw 85°, elev −42°.

---

## 6. RENDER.md §7.1 questions — status

| # | question | status |
|---|---|---|
| 1 | water-phase string | **resolved**: `Water` (bucket 7); the water FXOs say `TerrainWater` (no match), irrelevant because water tasks are hard-wired to 7 [H] |
| 2 | StencilDarken variant, stencil func | **resolved**: §4.3 [H] |
| 3 | bloom constants | **resolved**: TEE `23`/`24`/`28` via scene and context, defaults 1.0/0.9/0.25 [H] |
| 4 | galactic camera FOV/near/far | open (not in scope) |
| 5 | background scenes content | partly: environment skydome objects `19`/`1A` (names are GameObjectTypes); mapping to the three background scene slots still open [M] |
| 6 | team colour path | open (not in scope) |
| 7 | secondary top-down pass | open; not the terrain bake (that uses its own camera, §2.8) |
| 8 | SHADOW_EXTRUSION_DISTANCE | **resolved**: global float at 0x140a12b28 = 200.0, no writer in this build → constant 200 [M] |
| 9 | heat→transparent remap flag | **resolved**: 0x140a626bb has no writer → never remapped [M] |
| 10 | effect-list order | open |
| 11 | LOD_Bias / min_geometry_lod | open |

> **Status 2026-10-30:** Rows 4, 6, 7, 10 and 11 are answered elsewhere: row 4 in SPACE_AND_OPEN.md §B.8 (implemented in `engine/ui/include/alamo/ui/camera.hpp:15, 96-97`), row 6 in SPACE_AND_OPEN.md §B.3, row 7 in DECORATIONS.md §7 (a debug view), row 10 in SPACE_AND_OPEN.md §D.1, row 11 in SPACE_AND_OPEN.md §B.4. Row 5 stays partly open.

---

## 7. Open questions and capture plan (extends RENDER.md §7.2; Gabriel runs the game)

Open:
1. Whether the reflection pass renders objects (bucket mask 0x12B includes Opaque and Transparent) or
   terrain only in practice; and how the skydome objects behave in it (water detail 2 path). [L]
2. The meaning of `0x10C`/`0x105` camera minis beyond position and the 1250/55 values.
3. Decoration types 0/2/3/5 naming (blobs / dynamic tracks / tracks / water tracks). [M/L] — **settled 2026-10-27, see `DECORATIONS.md` §0** (0 shadow blobs, 2 dynamic unit tracks, 3/5 map spline tracks on terrain / on the water plane; chunks 6/9 do occur in 23 FoC maps).
4. Soft-shadow sampler states (filter/address) for `StencilDarkenFinalBlur` — no states in the effect.
5. Whether the Halton table starts at index 0 (affects terrain normal quantisation bit-exactness).
6. Letterbox and gamma implementation.
   > **Status 2026-10-30:** Half resolved. Gamma: the port's own GPU pass is `engine/render/src/gamma.cpp` [port]. Letterbox: the level is tracked by the Lua presentation state (`engine/lua/include/alamo/lua/presentation.hpp`), but no letterbox draw pass exists in `engine/render`, and the original's drawing is still untraced.
7. Fog source per map in practice (environment fog vs the `0x100/2` scene block; flag RtsScene +0x78).

Additions to the capture plan (DXVK/RenderDoc, RENDER.md §7.2 B and C):

> **Status 2026-10-30:** The RenderDoc option (RENDER.md §7.2 plan B) is out of scope: CLAUDE.md says captures use apitrace. The apitrace and DXVK HUD items below still stand.

- **Land, Endor, water detail max, shader detail max**: confirm two 512² A8R8G8B8 render passes before
  the main pass (reflection with mirrored camera and clip plane, refraction), the 128² cube render after
  an environment change, per-patch terrain draws with up to ~17 alpha-blended passes each (Endor), each
  pass bound to a 32×32 A4R4G4B4 texture, followed by the water draws (one per under-water patch).
- apitrace on Endor: read `blendTexScale/Offset`, `diffuseTexU/V`, `CLOUD_TEX_U/V` per terrain draw and
  `ReflectionDistortionScale`, `WaterReflectC/RefractC`, `Bump0TexU/V` per water draw; compare with §2.5
  and §3.3 at a known water time.
- Space Kuat vs Alderaan with Bloom on: Kuat should show the 6 bloom draws with a visible composite;
  Alderaan the same draws with BloomStrength 0 (no visible change).
- Soft Shadows on/off on a land map: off → one draw `StencilDarken` (ZERO/SRCCOLOR, stencil NOTEQUAL);
  on → alpha-only draw, StretchRect, 4-sample blur draw.
- Toggle the tactical overview zoom: terrain should switch to one baked texture draw per patch.

---

## 8. Corrections made to other documents (2026-10-26)

- `docs/re/RENDER.md`: water-phase string (§1.3 table row 7 and §7.1 q1), StencilDarken variant (§1.5),
  bloom constant source (§1.5), `RESOLUTION_CONSTANTS` source and `SHADOW_EXTRUSION_DISTANCE` value
  (§2.2), heat remap flag (§1.3 row 11, §7.1 q9), bloom/heat only in land and space (§1.2).
- `docs/formats/SPEC.md` §6.2: TEE `15` (clear colour), `17` (shadow colour), `19`/`1A` (skydome
  names), `21` (lightning), `23`/`24`/`28` (bloom strength/cutoff/size — previously described as inert),
  `2B`–`2F` (wind and clouds); §6.1: water surface types; §1.1 pointer to the `.ted` layout here.
