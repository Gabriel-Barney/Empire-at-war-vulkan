# Alamo asset formats: byte-level specification (as verified)

Scope: the binary/data formats the new engine must ingest natively: the generic **chunk container**,
**ALO** (models and particle systems), **ALA** (skeletal animation), **MTD** (texture atlas), **DAT**
(string tables), and **TEM/TEE** (terrain material sets and environment sets).

Sections 1, 2, 4, 5 and 7 were established from the data files alone. For the particle emitter
property block (section 3) and the TEM/TEE field names (section 6), field *meanings* were also
cross-checked against the binary-analysis stream's Ghidra decompilation of `StarWarsG.exe`
(loader functions and the runtime code that consumes each field). That decompilation was used as
reference only; no code was copied. Every statement marked **verified** holds for every file in the
validation corpora (see `validation.md`): 2,183 files from FoC `corruption/Data/models.meg` plus
137 loose samples (mod `Legeds_ships_transfer`, text/material/environment sets). Statements marked
*hypothesis* fit the data but have not been proven, and the parsers keep the raw values for those
fields. Particle and TEM/TEE field names carry a confidence of **high** (runtime use is
unambiguous), **medium** (runtime use and data agree, but the name is an inference) or **low**
(type known, meaning not).

Reference implementation: `tools/formats` (C++20 library `alamo_formats`) and
`tools/formats/python/alo.py`. Both produce identical vertex, triangle, bone and mesh counts on all
504 vanilla models.

Conventions: all integers and floats are **little-endian**, floats are IEEE-754 binary32, strings
are 8-bit ASCII and **NUL-terminated inside their chunk**, and there is no alignment padding unless
stated. Offsets are in bytes. Chunk IDs are hex.

---

## 1. Chunk container (ALO, ALA, TEM, TEE)

```
chunk:
  +0  u32 id
  +4  u32 size_and_flag      size = value & 0x7FFFFFFF  (payload bytes, header excluded)
                             bit 31 set   -> payload is a sequence of child chunks
                             bit 31 clear -> payload is leaf data
  +8  payload[size]
```

- A file is a sequence of top-level chunks with no file header. **Verified:** every child sequence
  ends exactly at its parent's payload end, and the top-level sequence ends exactly at EOF.
- The meaning of an ID depends on its parent. ID `0x0`, `0x1`, `0x2` are reused at several levels
  (particle systems). The parsers and the validator therefore address chunks by **path**, for
  example `400/10000/10007`.
- **Mini-chunks:** many leaf chunks contain a list of `u8 id, u8 size, payload[size]` records that
  fills the payload exactly. Mini-chunk payloads are at most 255 bytes. Which leaves use mini-chunks
  is fixed per path (see the tables below). Integers in mini-chunks are u32 unless noted. Strings
  include their NUL.

### 1.1 Chunk catalogue (all paths observed; no unknown IDs found)

| Path | Leaf? | Content | Status |
|---|---|---|---|
| `200` | no | skeleton | verified |
| `200/201` | yes | u32 bone_count + 124 bytes zero padding (128 total) | verified |
| `200/202` | no | bone | verified |
| `200/202/203` | yes | bone name (string) | verified |
| `200/202/206` | yes | bone v2: i32 parent, u32 visible, u32 billboard_mode, f32[12] matrix (60 bytes) | verified |
| `200/202/205` | yes | bone v1: i32 parent, u32 visible, f32[12] matrix (56 bytes) | not observed; supported |
| `400` | no | mesh | verified |
| `400/401` | yes | mesh name | verified |
| `400/402` | yes | mesh info (128 bytes, see 2.3) | verified |
| `400/10100` | no | material for the next submesh | verified |
| `400/10100/10101` | yes | shader / effect file name, e.g. `MeshBumpColorize.fx` | verified |
| `400/10100/10102` | mini | int parameter: `01` name, `02` i32 | verified |
| `400/10100/10103` | mini | float parameter: `01` name, `02` f32 | verified |
| `400/10100/10104` | mini | float3 parameter: `01` name, `02` f32[3] | verified |
| `400/10100/10105` | mini | texture parameter: `01` name, `02` file name string | verified |
| `400/10100/10106` | mini | float4 parameter: `01` name, `02` f32[4] | verified |
| `400/10000` | no | submesh data | verified |
| `400/10000/10001` | yes | u32 vertex_count, u32 primitive_count + 120 zero bytes (128 total) | verified |
| `400/10000/10002` | yes | vertex format name, e.g. `alD3dVertNU2U3U3` | verified |
| `400/10000/10007` | yes | vertex buffer, vertex_count × 144 bytes (see 2.5) | verified |
| `400/10000/10005` | yes | legacy vertex buffer, vertex_count × 128 bytes (see 2.5); 144-byte records also accepted | verified (EaW GameData, 153 submeshes in 51 models) |
| `400/10000/10004` | yes | index buffer, u16 × (3 × primitive_count), triangle list | verified |
| `400/10000/10006` | yes | skin bone map, u32 × n (local skin index → skeleton bone) | verified |
| `400/10000/1200` | no | collision tree (see 2.7) | verified |
| `400/10000/1200/1201` | mini | `00` f32[3] bbox min, `01` f32[3] bbox max, `02` u32 node count, `03` u32 triangle count | verified |
| `400/10000/1200/1202` | yes | nodes, 10 bytes each | verified |
| `400/10000/1200/1203` | yes | u16 triangle (primitive) indices | verified |
| `1300` | no | light | verified |
| `1300/1301` | yes | light name | verified |
| `1300/1302` | yes | light info (36 bytes, see 2.8) | verified |
| `600` | no | connections (always the last top-level chunk of a model) | verified |
| `600/601` | mini | `01` u32 connection count, `04` u32 proxy count, `09` u32 (dazzle count?, seen once, value 0) | verified / `09` hypothesis |
| `600/602` | mini | `02` u32 object index, `03` u32 bone index | verified |
| `600/603` | mini | `05` proxy name, `06` u32 bone, `07` u32 hidden, `08` u32 alt-decrease-stay-hidden | verified (07/08 names: hypothesis) |
| `900` | no | particle system (see 3) | verified (structure) |
| `900/0` | yes | system name | verified |
| `900/1` | yes | u32 particle-editor system ID (used only by the editor's save/lookup path); 0 in shipped data | medium |
| `900/2` | yes | u8 leave particles: keep emitted particles alive when the system is detached | medium |
| `900/800` | no | emitter list | verified |
| `900/800/700` | no | emitter | verified |
| `900/800/700/2` | mini | emitter property block (section 3.1); 239/245/248/254/260 bytes = optional IDs | verified |
| `900/800/700/3` | yes | color texture name | verified |
| `900/800/700/16` | yes | emitter name | verified |
| `900/800/700/29` | no | 3 spawn shapes (velocity, lifetime, position): `1100` { `1101` 64 bytes } (section 3.2) | verified |
| `900/800/700/1` | no | 7 tracks, each a pair (`0` header, `1` keys) | verified |
| `900/800/700/1/0` | mini | `02` start, `03` end (u8 for tracks 0-3, f32 for tracks 4-6), `04` u32 interpolation | verified |
| `900/800/700/1/1` | mini | n × `05` { f32 time (0..100), f32 value } | verified |
| `900/800/700/36` | mini | `37` u32 spawn-on-death emitter index, `39` u32 spawn-during-life emitter index (0xFFFFFFFF = none) | high |
| `900/800/700/45` | yes | bump / depth texture name | verified |
| `1000` | no | animation (see 4) | verified |
| `1000/1001` | mini | `01` u32 frames, `02` f32 fps, `03` u32 bones, [`0B` u32 rot block, `0C` u32 trans block, `0D` u32 scale block] | verified |
| `1000/1002` | no | per-bone animation | verified |
| `1000/1002/1003` | mini | bone track info (see 4.2) | verified |
| `1000/1002/1004` | yes | per-bone translation, u16[3] × frames | verified |
| `1000/1002/1005` | yes | per-bone track with 1-byte elements (the runtime loader sizes it in bytes, unlike 1004/1006) | not observed |
| `1000/1002/1006` | yes | per-bone rotation, i16[4] × 1 (constant) or × frames | verified |
| `1000/1002/1007` | yes | visibility bitfield, ceil(frames/8) bytes | verified |
| `1000/1002/1008` | yes | step-frame bitfield, ceil(frames/8) bytes | size verified; meaning hypothesis |
| `1000/1009` | yes | rotation block data, i16 × frames × rot_block | verified |
| `1000/100A` | yes | translation block data, u16 × frames × trans_block | verified |
| `1000/100B` | yes | scale block data, u16 (the runtime loader reads u16 elements) | not observed (scale_block = 0 in every file) |
| `2` | no | TEM terrain material set | verified |
| `2/3` | mini | one terrain material (see 6) | container verified |
| `6` | mini | TEE environment/lighting record (see 6) | container verified |
| `5` | yes | TEE end marker, size 0 | verified |

Map files (`.ted`) use the same container; their chunk layout (map info, world, terrain `0x101`,
passability `0x10A`, objects `0x102`, embedded TEM/TEE records) is documented in
`docs/re/TERRAIN_WATER_POST.md` §1 (added 2026-10-26); the per-object chunks, including `0x482` (the object's hint name
read by Lua `Find_Hint`), are in its §1.6 (2026-11-09).

---

## 2. ALO model

### 2.1 Top-level layout

A model file is `200` (skeleton, exactly one), then any interleaving of `400` (mesh) and `1300`
(light), then `600` (connections, exactly one). A particle-system ALO has a single top-level `900`
instead (section 3). Telling the two apart from the first chunk ID is reliable.

**Objects.** Meshes and lights are numbered together in file order as "objects" 0..N-1.
`600/602` connections refer to these object indices. **Verified:** connection count equals
mesh count plus light count in every file (536 of 536 models), `600` is always last, bone 0 is
always `Root`, and every object index and bone index is in range.

### 2.2 Skeleton

- `201.bone_count` equals the number of `202` children (**verified**).
- Bone 0 is always `Root` with parent −1. A bone's `parent` is always less than its own index
  (**verified**), so a single forward pass computes world matrices.
- Matrix: 12 floats, **row-major 3×4**. `m[r][0..2]` is the rotation/scale row r and `m[r][3]` is
  the translation. A point transforms as `p'[r] = Σc m[r][c]·p[c] + m[r][3]`. The matrix is
  the transform relative to the parent bone, so `world(b) = world(parent) · local(b)`.
- `billboard_mode` (u32), **high** confidence: it is stored in bone-flag bits 3-6, and the runtime
  switches on it every frame (`alo::BillboardMode`).

  | Value | Mode | Runtime behaviour | Bones |
  |---:|---|---|---:|
  | 0 | none | | |
  | 1 | parallel | Aligned to the camera's right and up axes | 2 |
  | 2 | face | Faces the camera position | 0 |
  | 3 | z_axis_view | Rotates about local Z toward the view direction | 0 |
  | 4 | z_axis_wind | Rotates about local Z toward the scene wind vector | 55 |
  | 5 | z_axis_light | Rotates about local Z toward the sun direction | 3 |
  | 6 | sun_glow | Placed along the sun direction at the bone's distance from its parent; translation zeroed on load | 7 |
  | 7 | sun | Placed at camera + sun direction × bone distance; translation zeroed on load | 13 |

  The runtime also accepts a bone chunk `204` (52 bytes: parent, then matrix; visible = 1).

### 2.3 Mesh info `402` (128 bytes)

| Off | Type | Field |
|---|---|---|
| 0 | u32 | submesh count (= number of `10100`/`10000` pairs, **verified**) |
| 4 | f32[3] | bounding box min |
| 16 | f32[3] | bounding box max |
| 28 | u32 | always 0; the runtime loader reads the 128-byte block but never uses this field (high) |
| 32 | u32 | is_hidden |
| 36 | u32 | is_collision_enabled |
| 40 | 88 bytes | zero (**verified**) |

Inside `400`, each submesh is a `10100` material chunk immediately followed by its `10000` data
chunk.

### 2.4 Materials

Shader names observed, with counts across the corpus: alDefault.fx 945, MeshBumpColorize.fx 700,
MeshAlpha.fx 643, MeshShadowVolume.fx 515, MeshAdditive.fx 441, MeshCollision.fx 319,
RSkinBumpColorize.fx 152, Planet.fx 99, RSkinShadowVolume.fx 96, RSkinGloss.fx 88, Tree.fx 80,
MeshBumpLight.fx 67, MeshGloss.fx 58, RSkinGlossColorize.fx 49, MeshGlossColorize.fx 38,
RSkinAdditive.fx 34, Default.fx 21, MeshAdditiveOffset.fx 21, RSkinAlpha.fx 19,
MeshAdditiveVColor.fx 17, MeshShield.fx 16, TerrainMeshBump.fx 11, MeshAlphaGloss.fx 9,
Skydome.fx 7, MeshAdditiveReflection.fx 3, Grass.fx 2, TerrainMeshGloss.fx 2, MeshAlphaScroll.fx 2,
BatchMeshAlpha.fx 1.

Common parameters: `Emissive`, `Diffuse`, `Specular`, `Colorization`, `UVOffset`, `Color`,
`DebugColor`, `UVScrollRate`, `Atmosphere`, `CityColor`, `k_a/k_d/k_s` (float4); `Shininess`,
`CloudScrollRate`, `AtmospherePower`, `BendScale`, `EdgeBrightness`, `*UVScale`, `*UVScrollRate`,
`CloudScale` (float); `DiffuseColor` (float3); `lightDir`, `n` (int); `BaseTexture`,
`NormalTexture`, `GlossTexture`, `CloudTexture`, `CloudNormalTexture`, `WaveTexture`,
`DistortionTexture` (texture).

### 2.5 Vertex buffer `10007` (144 bytes) or `10005` (128 bytes): one layout for every vertex format

**Verified:** `size == vertex_count × 144` in all 4,455 submeshes of the original corpus. The format
name selects which fields the runtime uses. The stored layout is always the following:

| Off | Type | Field |
|---|---|---|
| 0 | f32[3] | position |
| 12 | f32[3] | normal (unit length, except zero on shadow-volume fin vertices) |
| 24 | f32[2] × 4 | texcoord 0..3 (only set 0 used by shipped data) |
| 56 | f32[3] | tangent |
| 68 | f32[3] | binormal |
| 80 | f32[4] | color RGBA (observed 1,1,1,1 by default) |
| 96 | f32[4] | unknown / unused (observed 1,1,1,1) |
| 112 | u32[4] | bone indices (index into the submesh's `10006` bone map) |
| 128 | f32[4] | bone weights |

**Legacy `10005`, 128-byte records** (verified on the FoC + EaW GameData install through the VFS:
51 of 1,379 model ALOs, 153 submeshes, all EaW-era files). The same fields without the unknown
float4 at +96; everything after it moves up 16 bytes:

| Off | Type | Field |
|---|---|---|
| 0 | f32[3] | position |
| 12 | f32[3] | normal |
| 24 | f32[2] × 4 | texcoord 0..3 |
| 56 | f32[3] | tangent |
| 68 | f32[3] | binormal |
| 80 | f32[4] | color RGBA. All zero except in colour formats: the one `alD3dVertNU2C` submesh (star backdrop `W_STARS_BACK02`, `MeshAdditiveVColor.fx`) stores grey levels 0..1 with alpha 0 (320 of 1,984 vertices are 0,0,0,0) |
| 96 | u32[4] | bone indices |
| 112 | f32[4] | bone weights (1,0,0,0 observed) |

Formats in `10005` chunks: `alD3dVertN` 38, `NU2` 75, `NU2U3U3` 37, `NU2C` 1, `B4I4NU2` 1,
`B4I4NU2U3U3` 1. Evidence: unit normals at +12, UVs at +24, a valid tangent frame at +56/+68,
bone indices 0..n−1 at +96. `alamo_formats` decodes these records into the same `VertexBuffer`
(`SubMesh::vertex_stride` = 128; `extra`, which is not stored, reads 1,1,1,1). A 128-byte stride
is accepted only in `10005`; `10005` with 144-byte records is also accepted.

**Runtime use (engine rule, docs/re/SPACE_AND_OPEN.md D.2/D.4/D.5; implemented in `engine/assets`):**
the original's 128-byte converter copies position, normal, the 4 UVs, tangent, binormal and the bone
fields but **overwrites the colour with 1,1,1,1**, so the stored legacy colour above never reaches the
GPU (`W_STARS_BACK02` draws at full brightness). 144-byte `10007` colours are used as stored. Tangent
and binormal are uploaded as stored for formats with a frame (zero stays zero) and are never generated
or repaired. The vertex format used for packing is the material effect's `_ALAMO_VERTEX_TYPE` when it
declares one, not necessarily the `10002` name (e.g. `Planet.fx` makes alD3dVertN records NU2U3U3).
This replaces the earlier "legacy colour used as stored" decision.

### 2.6 Vertex format table

| Name | Count in corpus | Normal | UV sets | Tangent frame | Color | Skin | Notes |
|---|---:|---|---:|---|---|---|---|
| `alD3dVertN` | 920 | yes | 0 | no | no | no | shadow/collision/untextured |
| `alD3dVertNU2` | 2217 | yes | 1 | no | no | no | most common |
| `alD3dVertNU2C` | 43 | yes | 1 | no | yes | no | vertex color |
| `alD3dVertNU2U3U3` | 835 | yes | 1 | yes | no | no | bump-mapped |
| `alD3dVertRSkinNU2` | 286 | yes | 1 | no | no | rigid | |
| `alD3dVertRSkinNU2U3U3` | 152 | yes | 1 | yes | no | rigid | |
| `alD3dVertGrass` | 2 | yes | 1 | no | no | no | |
| `alD3dVertNU2U3U3C` | 17 † | yes | 1 | yes | yes | no | bump + vertex color |
| `alD3dVertB4I4NU2` | 47 † | yes | 1 | no | no | 4-bone | EaW-era; weights 1,0,0,0 in every vertex seen |
| `alD3dVertB4I4NU2U3U3` | 5 † | yes | 1 | yes | no | 4-bone | same |
| `alD3dVertNC` | 0 | yes | 0 | no | yes | no | name only; not in data |

† Counts from the full FoC + EaW GameData install through the VFS (1,379 models, 9,294 submeshes);
the other counts are from the original 536-model corpus. On the full install the totals are N 2054,
NU2 4455, NU2C 104, NU2U3U3 1977, Grass 14, RSkinNU2 423, RSkinNU2U3U3 198.

`alamo_formats` (`alo::vertex_format_info`) holds this table, case-insensitively; `bone_influences`
is 0 (static), 1 (RSkin) or 4 (B4I4). An unknown name gets `known = false` and fields inferred from
its tokens: `U2` → 1 UV set, `U3U3` → tangent frame, `B4I4` → 4 influences, `Skin` → 1, trailing
`C` → color. The parser range-checks `bone_index[0]` for RSkin and every index with a non-zero
weight for B4I4 against the `10006` bone map.

**B4I4 (four-bone skinning):** `bone_index[0..3]` index the `10006` bone map and `weight[0..3]` are
the blend weights. In the shipped data (52 submeshes, 47,987 vertices, in both `10005` and `10007`
chunks) every vertex has weights 1,0,0,0 to within float rounding (222 are not bit-exact; the
heaviest weight is ≥ 0.999 in all), so it renders as rigid skinning. The engine's asset loader uses
the heaviest weight's bone.

**RSkin (rigid skinning), verified visually:** each vertex is bound to one bone,
`bone_map[bone_index[0]]`, with `weight[0] = 1`. **Skinned vertices are stored in model (bind)
space.** They are *not* bone-local. To skin, compute `world_anim(b) · inverse(world_bind(b)) · v`.
For every skinned submesh in the corpus, the bone map size and the indices are in range.

**Static (non-skinned) meshes are stored in the space of the bone their mesh object is connected
to** (`600/602`). To draw one, compute `world(bone) · v`. This is verified visually: turret barrels
sit correctly on their barrel bones.

**Winding:** under a right-handed interpretation of the stored coordinates, triangles are
counter-clockwise with respect to the stored normals (2,843 of 2,846 sampled triangles agree). This
means glTF can take the data as stored. Alamo itself is Z-up.

### 2.7 Collision tree `1200`

This is a binary AABB tree over the submesh's triangles. It appears on 1,096 submeshes, typically
the hidden collision meshes.

- `1201` (mini-chunks): root box min/max (f32[3] each), node count, triangle count. The counts match
  `1202` and `1203` (**verified**).
- `1202` node, 10 bytes:

  | Off | Type | Field |
  |---|---|---|
  | 0 | u8[3] | quantised min x,y,z |
  | 3 | u8[3] | quantised max x,y,z |
  | 6 | u16 | `count`: 0 = internal node, >0 = leaf with `count` triangles |
  | 8 | u16 | internal: index of first child (children are `index`, `index+1`); leaf: first entry in `1203` |

- Box dequantisation is **relative to the parent node's box**:
  `min = pmin + (pmax − pmin) · q / 255`. The root uses the `1201` box. **Verified:** across 40
  models, 253,395 of 253,395 triangle vertices fall inside their leaf box under this rule. Under a
  root-relative interpretation, 149,243 fall outside.
- `1203`: u16 primitive indices (triangle `t` = indices `3t..3t+2` of the submesh). All entries are
  less than the primitive count (**verified**).

### 2.8 Light `1302` (36 bytes)

| Off | Type | Field |
|---|---|---|
| 0 | u32 | type: 0 omni, 1 directional, 2 spot (observed: 6 × 0, 2 × 1, 13 × 2) |
| 4 | f32[3] | color |
| 16 | f32 | intensity |
| 20 | f32 | far attenuation end |
| 24 | f32 | far attenuation start |
| 28 | f32 | hotspot size |
| 32 | f32 | falloff size |

Lights are present in 7 vanilla models, 21 lights in total. The field names after `intensity`
follow the order exposed by the art tools; the numeric ranges are plausible.

### 2.9 Proxies `603`

A proxy is a named attachment point that spawns another object, usually a particle `.alo` such as
an engine glow or a damage effect, at a bone. Proxy names have no extension. Of 2,621 proxies, 98
set `07` and 792 set `08`.

---

## 3. ALO particle system (`900`)

```
900
  0      name
  1      u32 editor system ID (0 in shipped data)
  800    emitters
    700  emitter ×N
      2    property block: mini-chunk list (3.1)
      3    color texture
      16   emitter name
      29   3 spawn shapes: 1100 { 1101: 64-byte record } (3.2)
      1    tracks: 7 × (0 header, 1 keys) (3.3)
      36   links: mini 37 spawn-on-death emitter, mini 39 spawn-during-life emitter
      45   bump/depth texture
  2      u8 leave particles
```

The IDs of the emitter's child chunks (`03`, `16`, `29`, `36`, `45`) and of the property-block
mini-chunks form **one property-ID space**. The editor writes every property as one ID, and gaps in
the block's ID list are exactly the IDs stored as chunks.

### 3.1 Emitter property block (`700/02`)

**Verified:** the block is a mini-chunk list in all 891 emitters (471 vanilla, 420 mod). The five
sizes are the same ordered list with optional IDs. `06` appears only in the 260-byte variant
(394 emitters), `49` in the 254/260 variants, `47` in all but the 239 variant, and `48` in the
248/254/260 variants. `06` and `49` are not read by the game's loader. Exposed as
`particle::EmitterDesc`; the catalogue `particle::emitter_fields()` carries the same table.

| ID | Type | Field | Conf. | Meaning / evidence |
|---|---|---|---|---|
| 04 | u32 | blend_mode | high | Index of the engine "Prim" effect (table below). Observed 1 (577), 2 (299), 4 (14), 5 (1). |
| 05 | u32 | geometry | medium | 0 → 3 vertices per particle, 1 → 4 (always 1 in data) |
| 06 | u32 | tool_field_06 | low | 0 or 40; skipped by the game loader |
| 07 | bool | use_bursts | high | Selects burst emission (25/26/27) over a rate (2A) |
| 08 | bool | object_space | medium | Particles are transformed with the emitter matrix (move with it) |
| 09 | f32 | outward_speed | medium | Initial speed along the normalised spawn position (radial) |
| 0A | f32[3] | acceleration | high | Constant acceleration added each step |
| 0B | f32 | outward_acceleration | medium | Acceleration along (particle − emitter), so negative = inward |
| 0C | f32 | gravity | high | Subtracted from acceleration.z (negative values make smoke rise) |
| 0F | f32 | lifetime | high | Seconds. Also sizes the particle pool: count = rate × lifetime |
| 10 | u32 | texture_cells | high | Atlas cell count; columns = √n (64 = 8×8). Indexed by track 5. |
| 11 | f32 | random_color_legacy | medium | Legacy only (not in data): one RGB variance, replaced by 2C |
| 12 | f32 | random_initial_rotation | medium | Initial rotation drawn uniformly from [−r, r] |
| 13 | f32 | random_lifetime | high | Lifetime drawn from [L·(1−r), L] |
| 14 | u32 | index | high | Emitter index; equals file position in 891/891 emitters |
| 15 | bool | enabled | medium | Always 1; the loader also forces it to 1 |
| 17 | f32 | random_rotation_speed | high | Rotation-speed variance fraction, clamped to 0..1 |
| 23 | bool | random_rotation_direction | high | Half of the particles (random) spin in reverse |
| 24 | f32 | initial_delay | high | Seconds subtracted from emitter time before emitting |
| 25 | f32 | burst_delay | high | Seconds between bursts |
| 26 | u32 | particles_per_burst | high | |
| 27 | u32 | burst_count | high | 0 = unlimited |
| 28 | f32 | inherit_velocity_factor | high | Fraction of emitter velocity added (when 43 is set) |
| 2A | u32 | particles_per_second | high | Continuous rate (when 07 is clear) |
| 2B | bool | depth_sort | medium | Back-to-front sort; only honoured for blend-type modes |
| 2C | f32[4] | random_color | high | Per-particle RGBA offsets in [0, c·100] (alpha is subtracted) |
| 2D | bool | random_color_uniform | high | One random value shared by R, G and B |
| 2E | bool | alternate_vertex_layout | low | Selects an alternate static vertex/UV table at render |
| 2F | u32 | ground_behavior | medium | 0 = free; 1/2 = particles ray-test against terrain each step |
| 30 | f32 | bounciness | medium | Response factor used by the terrain collision path |
| 31 | bool | affected_by_wind | high | Adds the global wind vector (½·dt) to velocity |
| 32 | f32 | stop_time | high | Emitter stops after N s (0 = never) |
| 33 | f32 | skip_time | high | Pre-simulates N s in 0.1 s steps on start (pre-warm) |
| 34 | u32 | spawn_source | medium | 0 = spawn shapes; 1/2 = point on the parent mesh; 3 = one particle per parent mesh vertex |
| 35 | bool | object_space_acceleration | high | Acceleration is rotated by the emitter orientation |
| 3B | bool | is_heat_particle | medium | Drawn with mode 2 (PrimAlpha) in the heat-distortion pass |
| 3C | f32 | mesh_normal_offset | medium | Offset along the mesh normal for spawn_source 1/2 |
| 3D | bool | is_weather_particle | high | Camera-relative weather cube. Disables gravity, acceleration, bursts and ground behaviour; random initial age. |
| 3E | f32 | weather_cube_size | high | Cube edge; the position shape is set to a cube of half this |
| 3F | f32 | weather_fade_start | medium | Distance at which weather particles begin to fade |
| 40 | f32 | weather_fade_length | medium | Fade range |
| 41 | bool | has_tail | high | Quads stretched along velocity |
| 42 | f32 | tail_size | high | Stretch = 1 + \|v\|·size / max(\|v\|, v_max); also pads the bounds by 1.5·size |
| 43 | bool | inherit_velocity | high | Enables 28 |
| 44 | bool | affected_by_wind_disturbance | medium | Pushed by local disturbance spheres (radius, strength); requires 31 |
| 46 | bool | render_flag_46 | low | Forwarded to the particle draw call; bypasses the sorted path |
| 47 | f32 | weather_camera_distance | medium | Weather-cube centre offset along the camera axis (600/700) |
| 48 | bool | rotation_fixed_at_spawn | medium | Rotation is evaluated once at spawn instead of every frame |
| 49 | f32 | tool_field_49 | low | Skipped by the game loader; values look like uninitialised memory |

**Blend modes (high).** The runtime's mode → effect table is shown below. Blend states come from
`docs/shaders/catalog.json`. Fades through RGB apply to modes 1 and 4; fades through alpha apply to
modes 2, 5 and 7. Depth sorting (`2B`) is honoured for 0, 2, 5 and 11.

| Mode | Effect | Blend (src/dst) |
|---:|---|---|
| 0 | Engine\PrimOpaque | none, z-write on |
| 1 | Engine\PrimAdditive | ONE / ONE |
| 2 | Engine\PrimAlpha | SRCALPHA / INVSRCALPHA |
| 3 | Engine\PrimModulate | ZERO / SRCCOLOR |
| 4 | Engine\PrimDepthSpriteAdditive | ONE / ONE (soft depth sprite, vs_2_0/ps_2_0) |
| 5 | Engine\PrimDepthSpriteAlpha | SRCALPHA / INVSRCALPHA (soft depth sprite) |
| 6 | Engine\PrimDepthSpriteModulate | soft depth sprite, modulate |
| 7 | Engine\PrimDiffuseAlpha | SRCALPHA / INVSRCALPHA |
| 8 | Engine\StencilDarken (or …ToAlpha) | shadow phase |
| 9 | Engine\StencilDarkenFinalBlur | shadow phase |
| 10 | Engine\PrimHeat | heat-distortion phase |
| 11 | Engine\PrimParticleBumpAlpha | SRCALPHA / INVSRCALPHA |
| 12 | Engine\PrimDecalBumpAlpha | |
| 13 | Engine\PrimAlphaScanlines | SRCALPHA / INVSRCALPHA |

Mini IDs `19`, `1A` and `1B` inside a leaf emitter chunk `00` are a legacy encoding of the three
spawn shapes. The loader accepts them, but they do not appear in the data.

### 3.2 Spawn shapes (`700/29`)

There are always three `1100` groups (**verified**: 891/891 emitters). In file order they are:
**velocity** (initial velocity vector), **lifetime** (scalar, read from `box_min.x`..`box_max.x`)
and **position** (spawn offset). The runtime **recomputes the lifetime shape** from `0F`/`13` at
load time. The stored lifetime shape is stale in 383 of 891 emitters, so engines must recompute it
too.

| Off | Type | Field |
|---|---|---|
| 0 | u32 | kind: 0 point, 1 box, 2 cube, 3 sphere, 4 cylinder, 5 (accepted, not observed) |
| 4 | f32[3] | box min (kind 1; kind ≠ 0 scalar sampling uses .x) |
| 16 | f32[3] | box max |
| 28 | f32 | cube half-size (kind 2: uniform in [−h, h]³) |
| 32 | f32 | sphere radius (kind 3) |
| 36 | u8 + 3 pad | sphere: on surface (1) or filled (0) |
| 40 | f32 | cylinder radius (kind 4) |
| 44 | u8 + 3 pad | cylinder: on rim (1) or filled (0) |
| 48 | f32 | cylinder height (z uniform in [0, h]) |
| 52 | f32[3] | point value (kind 0) |

Kind usage in the data: velocity is point 315 / sphere 402 / box 100 / cylinder 72 / cube 2;
position is point 307 / sphere 358 / cylinder 136 / box 80 / cube 10; lifetime is box 891.

### 3.3 Tracks (`700/01`)

Seven tracks in fixed order (**verified**): `0` red, `1` green, `2` blue, `3` alpha (byte start and
end values, header minis `02` and `03` are u8), `4` scale (*medium*), `5` texture index (high: it
selects an atlas cell, `cell / columns`, `cell % columns`), and `6` rotation speed (high:
revolutions over the particle life × 2π). Header mini `04` is the u32 interpolation mode. Each key
is a mini `05` of 8 bytes: **value first, then time** (f32, the time as a 0..1 fraction of particle
life). On the byte tracks (0–3) the value is a **u32** 0..255, not an f32; on the float tracks (4–6)
it is an f32. **Verified** (2026-10-27, R4 survey of all 1,490 shipped FoC + EaW emitters: every key
time is in [0, 1] and ascending, every byte-track value read as f32 is a denormal = small integer).
(Corrected: earlier versions of this section said "(time in percent, value)".) The runtime resamples
every track into 32 entries.

---

## 4. ALA animation

### 4.1 Info `1001`

| Mini | Type | Field |
|---|---|---|
| 01 | u32 | frame count |
| 02 | f32 | frames per second (15, 30, ...) |
| 03 | u32 | bone count (= number of `1002`, **verified**) |
| 0B | u32 | rotation block size in i16 elements per frame (= 4 × animated-rotation bones) |
| 0C | u32 | translation block size in u16 elements per frame (= 3 × animated-translation bones) |
| 0D | u32 | scale block size (always 0 in data) |

Minis 0B-0D are present in 1,461 of 1,500 vanilla files, the **block layout** (FoC-era). The other
39 files use the **per-bone layout** (EaW-era). Both layouts occur in the FoC archive.

### 4.2 Bone info `1003`

| Mini | Type | Field |
|---|---|---|
| 04 | string | bone name. Matches the skeleton bone name. |
| 05 | u32 | skeleton bone index. Equals the index of the same-named ALO bone (verified on samples). |
| 0A | u32 | overlay_excluded (medium-high): when this animation is layered over another (`alHModel` overlay path), bones with 1 are not driven by it. About 40% of bones are set. |
| 06 | f32[3] | translation offset |
| 07 | f32[3] | translation scale |
| 08 | f32[3] | scale offset |
| 09 | f32[3] | scale scale |
| 0E | u16 | translation index into block (0xFFFF = constant)  *(block layout only)* |
| 0F | u16 | scale index (always 0xFFFF in data) *(block layout only)* |
| 10 | u16 | rotation index into block (0xFFFF = constant) *(block layout only)* |
| 11 | i16[4] | default (constant) rotation quaternion *(block layout only)* |

### 4.3 Decoding (verified by posing skinned characters: Royal Guard idle/attack, Dark Trooper jetpack)

- **Translation** (bone-local, replaces the bind translation):
  `t = offset + scale · u16` per component.
  Block layout: `u16 = data100A[frame · trans_block + translation_index + c]`.
  Per-bone layout: `u16 = data1004[frame · 3 + c]`.
  Not animated: `t = offset` (the scale is 0).
- **Rotation** (bone-local, replaces the bind rotation): quaternion `(x, y, z, w) = i16[4] / 32767`,
  then normalise. Block layout reads `data1009[frame · rot_block + rotation_index + c]`, or mini
  `11` when the index is 0xFFFF. Per-bone layout reads `data1006`, which is either 4 values
  (constant) or 4 × frames.
  **Verified:** indices pack densely (0, 3, 6, ... for translation and 0, 4, 8, ... for rotation),
  and stored quaternions have magnitude 1 ± 0.05 in every file.
- **Scale**: `offset + scale · u16` by the same scheme. Scale is never animated in the shipped data.
- **Visibility** `1007`: ceil(frames/8) bytes, LSB-first bit per frame, 1 = visible. A bone without
  `1007` is always visible.
- **Step frames** `1008`: same bitfield size. The runtime loads it into the track's second bit
  array, but no `alQuantizedAnim` method reads it, so it can be ignored (medium). The likely origin
  is "step key" flags from the exporter.

---

## 5. MTD texture atlas

```
u32 count
count × {                       81 bytes, packed
  char name[64]                 NUL-padded, e.g. "ADVANCED_GAME_OPTIONS.TGA"
  u32  x, y, width, height      pixel rectangle in the atlas texture (MT_<name>.tga)
  u8   alpha                    0/1
}
```

**Verified:** `MT_commandbar.MTD` holds 3,577 entries and `4 + 81·n` equals the file size.

## 6. TEM / TEE (chunked mini-chunk records)

Typed views: `minifile::decode_terrain_material()` and `minifile::decode_environment()`.

### 6.1 .tem terrain material set

One `2` chunk holding exactly 64 leaf `3` chunks (**verified**: 24/24 files). Each `3` chunk is one
terrain texture slot; 701 of 1,536 slots are unused (empty texture names).

| Mini | Type | Field | Conf. | Evidence |
|---|---|---|---|---|
| 0C | string | diffuse_texture | high | Loaded as the slot's base texture |
| 13 | string | normal_texture | high | Loaded as the bump map; the runtime falls back to `Generic_Flat_B.tga` |
| 07 | f32 | tile_size | high | UV projection scale = 1/x (runtime default 100; data 100, 215, ...) |
| 08 | f32 | uv_rotation_z | medium | Radians; rotates the UV projection about Z |
| 09 | f32 | uv_rotation_x | medium | Radians; rotates the UV projection about X (applied first) |
| 06 | f32 | specular_power | low | Runtime default 64; data 64 / 64.24 / 123.4 |
| 0A | f32[3] | diffuse_color | medium | Runtime default (1,1,1) |
| 0B | f32[3] | specular_color | medium | Runtime default (1,1,1) |
| 14 | u32 | surface_type | high (use) / medium (names) | Read per terrain cell by SurfaceFXBehaviorClass and GameObjectClass::Get_Damage_Multiplier (types 9-11 special-cased). Observed 0 default, 1 grass, 2 dirt/gravel, 3 snow, 5 rock/concrete, 6 mud, 7 ice, 8 lava; names inferred from texture names. Cells under a map's water plane report 4 (water), 8 (lava) or 7 (ice) by water type (table 0x140a123c8; corrected 2026-10-26, see TERRAIN_WATER_POST.md). |
| 0D | u8 | flag_0d | low | 1 in 7 slots; no runtime reader found |
| 12 | u8 | full_coverage | medium | The terrain renderer forces this layer's blend weights to full (0x0F/0xFF) wherever it appears. 1 in 48 slots. |
| 23 | u8 | not_rendered | medium | When 1, the terrain renderer skips the layer when collecting a patch's layers. Newer files only; always 0. |

### 6.2 .tee environment set

A sequence of leaf `6` chunks, one per lighting environment, terminated by an empty `5` chunk
(**verified**). The runtime loads each record into an `RtsSceneClass::Environment` (RTTI name
present in the binary).

| Mini | Type | Field | Conf. |
|---|---|---|---|
| 14 | string | name (e.g. `Blizzard`) | high |
| 00, 01, 02 | f32[3] | light_color[0..2]: three directional lights (sun + two fills) | medium |
| 05, 06, 07 | f32 | light_intensity[0..2] | medium |
| 08, 09, 0A | f32 | light yaw a[0..2] (radians, 0..2π) | high |
| 0B, 0C, 0D | f32 | light elevation e[0..2] (radians); direction towards the light = (cos e·sin a, −cos e·cos a, sin e) | high |
| 03 | f32[3] | specular colour of light 0 (sun) | high |
| 04 | f32[3] | ambient colour | high |
| 0F | f32[3] | fog colour | high |
| 10 | u8 | fog enabled | high |
| 11, 12 | f32 | fog start / end. The loader stores min/max of the pair (data: 100 / 5000). | high |
| 19, 1A | string | skydome 1 / 2 object type names (e.g. `DAY_STORM_SKY`, `STARS_LOW`); rendered into the land water sky cube | medium (corrected 2026-10-26, see TERRAIN_WATER_POST.md §5.2) |
| 1B, 1C | f32 | skydome 1 / 2 scale (reset to 1 when the name is empty) | medium |
| 1D, 1F / 1E, 20 | f32 | skydome 1 / 2 angles, authored in **degrees** (the loader converts to radians) | medium |
| 29 | string | weather effect (e.g. `BLIZZARD_RANDOM`) | low |
| 2B | f32 | wind direction, degrees: rotates the cloud velocity and the scene wind vector | high |
| 2C | f32 | wind speed → scene wind vector (WIND_* parameters) | high |
| 2D | f32 | cloud scale (world units) → terrain `CLOUD_TEX_U/V` (overrides the terrain header) | high |
| 2E | f32 | cloud scroll speed (world units/s) | high |
| 2F | string | cloud texture | high |
| 15 | f32[3] | clear (background) colour of the scene, the water reflection and the sky cube | high (corrected 2026-10-26) |
| 17 | f32[3] | stencil-shadow darken colour (scene +0x1E4 → `StencilDarken*`) | high (corrected 2026-10-26) |
| 21 | f32 | lightning flash intensity (random white SH flashes) | medium |
| 23, 24, 28 | f32 | **BloomStrength, BloomCutoff, BloomSize** (defaults 1.0 / 0.9 / 0.25), copied to the scene's post block and used by `SceneBloom` — not inert (corrected 2026-10-26, see TERRAIN_WATER_POST.md §4.5) | high |
| 13 | u32 | stored at environment +0x100, clamped ≥ 0; reader not traced | low |
| 16, 18 | various | loaded and re-saved; no runtime reader found | low |

## 7. DAT string table

```
u32 count
count × { u32 crc; u32 value_len_chars; u32 key_len_chars; }
values: concatenation of UTF-16LE strings (no terminators), in record order
keys:   concatenation of 8-bit strings (no terminators), in record order
```

- `crc` = standard CRC-32 (IEEE, reflected 0xEDB88320, init/xorout 0xFFFFFFFF) of the **key
  bytes exactly as stored**. The key is *not* upper-cased first. **Verified:** 0 mismatches over
  33,677 strings. 79 mixed-case keys, such as `MBRM09_Lost_and_Found`, would fail under an
  upper-casing rule.
- `mastertextfile_*.dat` is sorted by CRC, so a lookup can binary-search.
  `creditstext_*.dat` is unsorted and contains repeated keys, so record order is significant there.
- **Verified:** the string pool sizes add up exactly to the file size.

---

## 8. Unknowns and open questions

Resolved (revision 3, closeout A5):
- Bone `billboard_mode` (2.2, high) and particle blend modes 0-13 (3.1, high).
- `402`+28: never read by the runtime (high).
- ALA `0A` = overlay_excluded (medium-high); `1008` loaded but unused (medium).
- `900/1` = editor system ID (medium).
- TEE `03`/`04`/`0F`-`12` = specular, ambient, fog colour, fog enable and fog start/end (high).
- TEM `12` = full_coverage and `23` = not_rendered (medium); TEM `14` use confirmed.

Still open:
1. Particle fields `2E`, `46`, `06` and `49` (low).
2. TEE `13`, `16`, `18`, plus TEM `0D` (the others were resolved 2026-10-26, see
   TERRAIN_WATER_POST.md §5.2). They are loaded, but no reader was found. Resolving them needs a reference trace from the `RtsSceneClass::Environment` /
   `alTerrain::Mtl` pointers (not array indexing) or a runtime capture.
3. Chunks absent from this corpus: `205` bone v1, `604` dazzles. (`10005` 128-byte legacy vertices
   and `B4I4` four-bone skinning were found in the EaW GameData models on 2026-10-27 and are now
   specified in 2.5 and 2.6.) Still open: whether the runtime uses the alpha of `10005` vertex
   colours (stored as 0) and whether any B4I4 file with real blend weights exists.
4. Coordinate handedness at runtime. The asset data is self-consistent as right-handed Z-up with
   counter-clockwise winding, so do not flip the winding.

## 9. Library and tools quick reference

```
cmake -S tools/formats -B build && cmake --build build
build/alodump [--full] [--max N] file.(alo|ala|mtd|dat|tem|tee)     # JSON summary
build/alo2gltf [--anim x.ala] [--all] [--all-lods] [--no-skeleton] in.alo out.(gltf|glb)
build/formats_validate [--title T] [--csv out.csv] [--no-files] files...  # Markdown report
build/alamo_formats_tests                                            # synthetic unit tests
python -I tools/formats/python/alo.py files...                      # reference parser
python -I tools/formats/python/render_gltf.py in.gltf out.png [--frame F]
```

Public headers live in `include/alamo/formats/`: `chunk.hpp`, `alo.hpp`, `particle.hpp`,
`ala.hpp`, `text.hpp` (MTD and DAT) and `minichunk_file.hpp` (TEM and TEE). Parsers take a
`std::span<const uint8_t>` and return value types. Vertex, skeleton and atlas data use a
structure-of-arrays layout. Malformed input throws `alamo::FormatError` with the absolute file
offset. Every count and index is bounds-checked against its declaring header.
