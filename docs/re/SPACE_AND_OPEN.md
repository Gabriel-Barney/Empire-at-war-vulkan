# Space tactical rendering and open render questions (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent (Phase 3 open items wave 1). Builds on `docs/re/RENDER.md` and
`docs/re/TERRAIN_WATER_POST.md` and does not repeat them. Clean-room: prose and tables only, no code or
pseudo-code from the decompilation. Addresses are RVAs in StarWarsG.exe (renamed names in
`docs/re/ghidra/functions.csv` are often misattributed; the address is authoritative). Vtable slots were read
from the shipped executable; XML values are vanilla FoC (`extracted/foc_data_text.zip`).

Confidence: **[H]** read in the code and cross-checked, **[M]** read in the code, intent inferred,
**[L]** inferred, needs a capture.

---

## 0. Summary for the implementer

1. **A space frame renders two scenes**: a *background* scene (all objects whose type has
   `In_Background = yes`: skydomes/starfields, nebula backdrops, planet backdrops, the black hole, the Death
   Star, plus the space fog-of-war plane) with near 10 / far 100000 and no distance fade, then a **depth-only
   clear**, then the *main* scene (everything else) with the `Space_Mode` tactical camera (near 10, far 7000,
   fade 6000→7000). [H] (0x2b6aa0, 0x397680, vtable 0x8792f8 slots 32/33)
2. **The "fills zeroed, no stencil shadows" rule of `SpaceModeClass::vf21` applies to the background scene
   only.** The main space scene keeps the environment's two fill lights and keeps stencil shadows enabled
   (subject to the Shadow option). This corrects RENDER.md §1.2 and TERRAIN_WATER_POST.md §5.3 item 5. [H]
   (0x4d8fa0 writes to mode +0x320 = the scene returned by vtable slot +0x108)
3. **No viewport clear in space.** Land clears colour+depth with the environment clear colour (TEE `15`);
   space relies on the frame-start clear whose colour is the global (0,0,0) — **space clears to black**, the
   TEE clear colour is unused in space. [H] for the missing call (0x4d8fa0 vs 0x3bac70), [M] for black
   (0x140a61ad0 initialised to 0 at 0x124a0, no other writer)
4. A space `.ted` has the same four top-level chunks as a land map; the world chunk holds only `0x100`
   (environments), `0x102` (objects), `0x103` (bounds/camera box) and `0x105` (tactical camera). [H] (0x4d80b0)
5. **Scene fog always comes from the active environment** (TEE `0F`/`10`/`11`/`12`); the map's `0x100/2`
   scene-fog block is parsed but overwritten on every environment apply and every main-scene render. §B.1. [H]
6. Texture lookup: as named, then `./Data/art/Textures/<file>`, then the same two with the extension replaced
   by `.dds`; cache key = lower-case base name without extension. §B.2. [H]
7. Team colour is written per model into **copied** material instances, parameter `Colorization` =
   (player colour RGB, 1); palette = 9 GameConstants colours `MP_Color_Blue … MP_Color_Eight`. §B.3. [H]
8. `LOD_Bias` (default 4.0) is the per-model LOD scale multiplied with screen coverage. §B.4. [H]
9. The engine never generates tangent frames for ALO meshes (stored T/B used verbatim). §B.7. [H]
10. `HP_*_Blast` meshes are hidden at model setup because each is named by a hardpoint's `Damage_Decal`;
    meshes whose names contain `STEALTH`, `buildup` or `charged` are hidden too. §B.9. [H]

---

## A. Space mode rendering

### A.1 Map file

Same container and top level as land (TERRAIN_WATER_POST.md §1.1). World chunk `1` children handled by the
space loader `SpaceModeClass::vf15` 0x4d80b0: [H]

| id | content |
|---|---|
| `0x100` | environment set + scene-fog block, parsed by the RtsScene loader 0x2734c0 (identical to land) |
| `0x102` | object list via mode vtable +0x68 (same records as land, §1.6 there) |
| `0x103` | minis: `00`, `01` f32 → tactical camera +0x10 (pitch) / +0x14 (yaw); `03` f32[3] → camera +0x18 (look-at); `02` f32 → initial distance, `06` f32 → initial FOV (both ignored when −1); `04`/`05` f32[3] = playable box min/max (default ±5000 each axis) → mode +0x390… (centre/half-extent); `07`/`08` f32[3] = camera box min/max (default: playable box shrunk by 400 in X and Y) → mode +0x3a8… |
| `0x105` | tactical camera state, parsed by 0x2cea90 (same reader as land `0x10C`) |
| others | ignored; there is no `0x101`, so no terrain, water, decals or terrain FOW |

After parsing, the tactical camera is reset to the `Space_Mode` constants (0x2cee30 with the reset flag),
which overwrites the `0x103` `00`/`01`/`02`/`06` values just read, then the camera box is applied (0x2d0350).
[M] (order read in 0x4d80b0; whether the overwrite is intended is unclear)

Scene setup at map load (`vf13` 0x4d7da0): a new `RtsSceneClass` (main scene, mode +0x328), a new
`SimpleSceneClass` (background scene, mode +0x320; fog off, 1000…10000, white), a tactical camera object
(mode +0x330) bound to the `Space_Mode` constants; the main scene's cull tree is built over the scene bounds
× 1.1 (0x2753e0 with 100 / 1000) [L for the parameters' meaning]. The camera is first set to FOV atan(0.5),
aspect 4/3, near 10, far 10000 (0x261ab0) and then overridden by the tactical camera each frame. [H]

### A.2 Camera (`Space_Mode`, TacticalCameras.xml; constants block 0x140a2a4f0)

The name "Space_Mode" selects the block at 0x140a2a4f0 (0x47ffe0; "Unlocked" → 0x140a2a6f0). Applied every
frame by 0x2d03f0 exactly as for land (RENDER.md §3.4: degrees, Hor+ correction to 16:9 above 4:3). [H]

| field | value | note |
|---|---|---|
| FOV default / min / max | 55 / 55 / 55 deg (horizontal, defined for 4:3) | fixed; overview 60 (2200) and 70 (2900, pitch 80) |
| Pitch default / min / max | 50 / −10 / 85 deg | `Pitch_Per_Zoom_Unit` 0 (no zoom spline in space) |
| Yaw default / limits | 0 / ±1000 deg | effectively unlimited |
| Distance default / min / max | 1000 / 200 / 1900 | `Distance_Per_Mouse_Unit` 500, smooth 0.1 s |
| Near / Far | 10 / 7000 | camera +0x30 / +0x34 |
| Object_Fade_Begin / End | 6000 / 7000 | camera +0x40 / +0x3c; culling far = 7000 |
| Bounds buffers | side/top/bottom −300 | against the `0x103` camera box |
| `Location_Follows_Terrain` | 0 | |

`DISTANCE_FADE_VALS` for the main space scene = (1/(6000−7000), −7000/(6000−7000)) = **(−0.001, 7.0)**;
background scenes use the off value (−2e‑5, 3.0) because their fade pair is set to −1. [H] (formula RENDER.md §2.2)

### A.3 Scenes and layers

Which scene an object joins is decided by three GameObjectType booleans (0x397680): `In_Background`
(+0x3e) → mode vtable +0x108, `In_Midground` (+0x3f) → +0x110, `In_Midforeground` (+0x40) → +0x118,
otherwise +0x100 (main). [H]

| mode | +0x100 (main) | +0x108 (background) | +0x110 / +0x118 |
|---|---|---|---|
| Space | RtsScene (mode +0x328) | SimpleScene (mode +0x320) | none (null) |
| Land | RtsScene (+0x328) | none | none |
| Galactic | scene at +0x320 | scene at +0x328 | +0x330 / 0x4b2050 |

So **land has no background scenes**: its skydome objects are drawn in the main scene. In space, objects
flagged midground/midforeground get no scene (only a few galactic particles use those flags). [H] (vtables
0x8792f8, 0x8673a0, 0x8762c0)

Background content in vanilla FoC (In_Background = yes): `SpacePrimarySkydomes` (Stars_Low/Medium/High
`w_stars_*.alo`, scale 1, behaviour SKY_DOME; cinematic green/black/purple screens, scale 20),
`SpaceSecondarySkydomes` (13 nebula backdrops `w_stars_nebula_*.alo` and `w_sky_bespin.alo`, scale 25), 37
planet backdrops in SpaceProps (`w_planet_*_high.alo`, scale 500; moons 150/300), `Black_hole` (scale 7),
`Death_Star` (scale 2.5), `Destroyed_Planet_Remains_Part_1`, `Tactical_Planet_Explode_300x_Particle`. [H] (XML)

**Environment skydomes** (`vf59` 0x4d98c0, run on environment change): removes the previous ones (vtable
+0x1e0), then spawns the GameObjectTypes named by TEE `19` and `1A` at the origin, owned by the neutral
player, with orientation (TEE `1D`, 0, `1F`) for the first and (`1E`, 0, `20`) for the second (degrees;
rotation order of the object system). TEE `1B`/`1C` (skydome scale) are **not** used here; the type's
`Scale_Factor` applies. Unknown names clear the stored name. These objects are In_Background types, so they
land in the background scene. [H] for the spawn and angles, [M] for the axis meaning of the angle triple

`SKY_DOME` behaviour (0x61c9f0) only caches the model's `SUN` bone index; it does not move the dome to the
camera. Domes stay at the world origin (the space camera never leaves ±~2000 of it). [H] for the code, [L] for
who reads the SUN bone (lens flare candidate)

### A.4 Per-frame work (`SpaceModeClass::vf21` 0x4d8fa0)

1. If the mode has a FOW object (+0x1a0): bind `FOW_TEXTURE` / `FOW_TEX_U/V` (0x7dab30). [H]
2. Main scene's alScene: heat-allowed and bloom-allowed bytes (+0x210/+0x211) = 1. [H]
3. Copy from the **active environment** of the main RtsScene into the **background** scene's light record:
   ambient (env +0x6c), sun intensity (+0x78), sun colour (+0x84), sun specular (+0x60), sun direction from
   yaw/elevation (+0xa8/+0xb4); fill 1 and fill 2 intensity = 0; background alScene shadow-volume flag
   (+0x269) = 0. [H]
4. Apply the camera (0x13d820) and run the common tail (background scenes, main scene, mode 2D scene). [H]

No clear-colour set and no viewport clear (see §0 item 3). The main scene's own lighting, fog and post block
are refreshed by `RtsSceneClass::vf10` 0x2754f0, which re-applies the environment (0x277830) right before
every render: sun, **both fills from the environment**, ambient, SH, fog, bloom constants, shadow colour. [H]

Background scene properties: alScene post block defaults (heat 0, bloom 0 → no heat/bloom work inside it,
0x1a2080), fog off (SimpleScene default, nothing enables it), no stencil shadows, distance fade off, near 10
/ far 100000. Bloom/heat of the main scene operate on the whole back buffer, so they include the backdrop. [H]

**Space fog of war** (`SpaceFogOfWar`, Transparent phase): one renderable created by 0x7da510 (effect
`SpaceFogOfWar`, `W_Generic_White.tga`) and added to the **background** scene's alScene (0x4d7d10); its plane
height is GameConstants `SpaceFOWHeight` (−80) (0x7dae70). It therefore tints the backdrop only, before the
depth clear; ships are never covered by it. Grid size/UV mapping not traced. [H] for creation and scene, [M]
for the height meaning, [L] for the rest

**Space dust / nebula visuals**: no engine feature found (no strings, no submitter). Nebula fields are
gameplay (`NebulaBehaviorClass`) plus ordinary props/particles in the main scene; backdrop nebulae are the
skydome objects above. [M]

### A.5 Draw order (space vs land)

| step | land | space |
|---|---|---|
| frame start | clear colour (global, black) + depth + stencil | same |
| mode prologue | FOW params, clear colour = TEE `15`, tactical camera, **viewport clear colour+depth** | FOW params, background light copy, camera; **no clear** |
| background scenes | none | background scene (near 10/far 100000, fade off, buckets as RENDER.md §1.4 with mask 0xbbf; no heat/bloom), then **depth-only clear** |
| main scene | terrain/water/decorations + objects; stencil shadows; heat; bloom | objects only (no buckets 0/7 submitters); stencil shadows (if enabled and casters have shadow meshes); heat; bloom (BloomStrength often 0 in space maps) |
| after world | depth clear, HUD, 2D | same |

---

## B. Settled open questions

### B.1 Fog source and precedence — **answered** [H]

On every environment apply (load end, `Set_New_Environment`, 0x276ff0) and before every main-scene render
(0x2754f0 → 0x277830) the RtsScene writes the scene fog fields (SceneClass +0x60…+0x74):
- RtsScene +0xbd = 0 → fog off;
- else RtsScene +0x78 = 1 → **environment fog**: enable = TEE `10`, colour = TEE `0F`, start =
  lerp(TEE `11`, TEE `12` − 0.0001, env +0xfc), end = TEE `12`. env +0xfc defaults to 0 and is only animated
  by the land weather system (0x468980, 0x46a0c0);
- else → the RtsScene's cached copy of the scene fog taken at construction (SceneClass defaults).
Both flags are set to 1 by the RtsScene constructor (0x272100) and no writer clearing them was found, so the
environment always wins. The `0x100/2` block (0x267af0) only fills SceneClass fields that are overwritten.
Finally a global option (0x140b0a0d1, set by 0x25fd00 from 0x28a4d0) forces fog off. The background space
scene never gets fog. [H] for the code path, [M] for "no clearer" (found by pattern search)

### B.2 Texture lookup (`x.tga` named, `x.dds` shipped) — **answered** [H]

`alGraphicsDriver::Load_Texture` (0x1427f0 → 0x13db20 → 0x13f450):
1. Cache key = name after the last `/` or `\`, cut at the first `.`, lower-cased; a hit returns the cached
   texture (so `x.tga` and `x.dds` share one entry).
2. Try the name **as given** (upper-cased) through FileClass (mod paths, loose files, MEGs).
3. Try each texture directory in order — only `./Data/art/Textures/` is registered (0x4df10) — joined with
   the part after the last `\`.
4. If neither exists: cut the name at the first `.`, append `.dds`, repeat steps 2–3.
5. Still missing: log "Failed to find texture (tga/dds)"; when the caller allows a fallback, return a shared
   256×256 texture built from an image embedded in the exe (0x186210, global 0x140a626b0).
So the **named extension is tried first, `.dds` second**. [H]

### B.3 Team colour → `Colorization`, MP palette — **answered** [H]/[M]

- Writer: 0x266480 walks every sub-object of the model; for each material slot whose effect has a
  `Colorization` parameter it first **replaces the slot with a copy of the material instance** (0x14d4a0,
  instance vtable +0x28 = clone), then sets `Colorization` = (r, g, b, **1.0**). Called from 0x266040, which
  stores the colour at model +0x58 and skips unchanged colours. Answers RENDER.md §7.1 q6. [H]
- Colour choice (0x3935b0, object colourise; normal path): owner's faction `Is_Neutral` (faction +0x108)
  false → the owner player's colour (player +0x50 index → table built by 0x297630); otherwise the type's
  `No_Colorization_Color` (+0x2f0) if non-zero, else the faction's `No_Colorization_Color` (+0xd8); missing
  owner → colour left unchanged. A second branch (game state flag at 0x140b153e0, type +0x27) uses faction HUD colours
  (0x33e340). [M] for the branch conditions
- Palette (GameConstants, byte RGB /255, alpha 1), index order 0…8: Blue (78,150,237), Red (237,78,78),
  Green (119,237,78), Orange (237,149,78), Cyan (111,217,224), Purple (224,111,209), Yellow (224,215,111),
  Gray (109,75,5), Eight (65,129,42). **9 entries**, not 8. [H] (0x297630 copies GameConstants
  +0x11a7c…+0x11afc, XML values)
- An object never colourised keeps the FXO default of `Colorization`. [M]

### B.4 `LOD_Bias` and LOD selection — **answered** [H]

`LOD_Bias` (GameObjectType +0x240, **default 4.0**, ctor 0x367250; vanilla values 0.5–10, mostly 5) is copied
into the model's LOD scale (+0x1a0) when the object's model is created (0x3a4820). Selection (0x12e620): when
dynamic LOD is on globally (0x140a12530) and per model (+0x198), d = min(LOD_Bias × coverage, (MeshDetail+1)/2),
else d = MeshDetail; coverage per RENDER.md §5 (0x12dd00); chosen = clamp(int((maxLOD+1)·d), 0, maxLOD);
meshes with `_LOD` index −1 or the chosen index are shown (hide bit 0x4 on the sub-object). `min_geometry_lod`
belongs to DynamicTrackFX (tracks), not to ALO LOD. Answers RENDER.md §7.1 q11. [H]

### B.5 Shadow volumes: stencil state and extrusion — **answered** [H]

- Volumes (`MeshShadowVolume`/`RSkinShadowVolume`, technique `t0_zfail`): **z-fail, two-sided**: colour
  write 0, Z test LESS, Z write off, cull NONE, stencil ref 1, read/write mask 0x3F, func ALWAYS; CW (front)
  faces: z-fail INCR, CCW faces: z-fail DECR, pass/fail KEEP. Fallback `t0_2pass_zfail`: pass 0 cull CCW
  with INCR, pass 1 cull CW with DECR. INCR/DECR are the wrapping ops. [H] (FXO states, catalog.json)
- Phase baseline `PhaseShadow` p0: colour write 0, Z LESS; p1 restores RGB, LESSEQUAL, Z write on, stencil
  off. Projection z-bias 0.001 during the phase (RENDER.md §3.3). [H]
- Darken: `StencilDarken` (NOTEQUAL ref 0, read mask 0x4F, ZERO/SRCCOLOR) when Soft Shadows is off or shader
  detail 0, else `StencilDarkenToAlpha` (mask 0x3F) + `StencilDarkenFinalBlur` (TERRAIN_WATER_POST.md §4.3). [H]
- `SHADOW_EXTRUSION_DISTANCE` = global float 0x140a12b28 = **200.0**; the only reference is the getter
  0x196980, so it is a constant. [H]

### B.6 Heat → Transparent remap flag — **answered** [H]

Global 0x140a626bb: only reference is the getter 0x13ec90; the neighbouring option bytes have setters
(0x13ec40, 0x13ed00) but this one has none. Heat is never remapped; Heat bucket 11 always draws as such
(when Heat Distortions is on and the scene allows heat).

### B.7 Generated binormal handedness — **answered: the engine generates none** [H]

The ALO submesh loader (0x187d40) accepts raw vertex data in the declared format (`0x10003`), 0x90-byte
generic records (`0x10007` → 0x189800) or legacy 0x80-byte records (`0x10005` → 0x1abdc0, which copies
position, normal, 4 UV sets, tangent and binormal verbatim and fills the two colours with 1.0); all are packed
by 0x17c550 without computing anything. There is no D3DXComputeTangent import and `_ALAMO_TANGENT_SPACE` is
never read by the exe. Bump shaders use the stored tangent and binormal as two independent vectors (no
per-vertex sign, no cross product). Terrain derives its frame in the shader from the U/V projection axes
(TERRAIN_WATER_POST.md §2.4). Port: use stored T/B as is; for vertex formats without them the original has
no frame either (zero/undefined), so any generated frame is an enhancement, not parity. [H]

### B.8 Water phase string, bloom constants, galactic camera

- Water phase string `Water` (TERRAIN_WATER_POST.md §0 item 6) and bloom constants TEE `23`/`24`/`28` (§4.5
  there) — already settled, unchanged. [H]
- **Galactic camera** (`GalacticModeClass::vf1` 0x4b21f0 / `vf13` 0x4b23c0): the camera is set with
  l/r/t/b = −0.25/0.25/0.1875/−0.1875, near **10**, far **10000** (0x261ab0), which stores horizontal FOV =
  atan(r − l) = atan(0.5) = **0.4636 rad (26.57°)** and aspect 4/3; viewport full screen. The galactic
  camera update (0x3c2c00) keeps that FOV and only refreshes the aspect to the display aspect; no Hor+
  correction (that exists only in the tactical camera, 0x2d03f0). No distance fade (−1). Galactic
  background scenes use near 10 / far 100000 like space. Answers RENDER.md §7.1 q4 / §3.5. [H] for values,
  [M] for "no other near/far writer" (searched the clip/FOV setter callers only)

### B.9 Hardpoint damage meshes (`HP_*_Blast`) — **answered** [H]

Default hiding happens at GameObjectType model setup (0x375380, per mode): for each hardpoint of the type,
after attaching `Model_To_Attach` at `Attachment_Bone`, the sub-object whose **mesh name equals
`Damage_Decal`** (case-insensitive, e.g. `HP_F-L_BLAST`) gets its hidden bit (sub-object +0x30 bit 0) set.
The same function hides every mesh whose name **contains** `STEALTH`, `buildup` or `charged`
(case-insensitive substring, 0x265950). When the hardpoint is destroyed (0x386660) the `Damage_Decal` mesh
is un-hidden, meshes under the hardpoint's attachment bone are shown and, if the type flag +0x3d7 is set,
meshes under its other bone are hidden. Base rule at ALO load: mesh-info chunk `0x402` hidden u32 → bit 0,
collision u32 → bit 7 (0x14c970). Renderer default: apply the ALO hidden flag, then hide the `Damage_Decal`
names from Hardpoints XML of the object's type and the three substrings. [H] (hide propagation from the
type's prototype model to instance models [M])

### B.10 Hardpoint destruction — exact rule — **answered** [H] unless tagged

Written 2026-10-27 (RE agent). Refines B.9: the bone whose subtree is *shown* on destruction is **not**
`Attachment_Bone` but the bone named by `Damage_Particles`; the "other bone" is the one named by
`Engine_Particles`. All lookups below are on the **owner's** model (the object the hardpoints belong to),
except the fire bones, which are looked up in the attached `Model_To_Attach` model.

**Fields involved** (all on the `<HardPoint>` type, HardPoints*.xml; offsets are the HardPointType object,
xml_field_tables.md):

| XML tag | type offset | role |
|---|---|---|
| `Model_To_Attach` | 0x60 | ALO under `Data\Art\Models\`, attached at `Attachment_Bone` at type model setup (0x375380) |
| `Attachment_Bone` | 0x80 | bone in the owner model; its index is cached per hardpoint instance (instance +0x2c) and used as the position of the death effects |
| `Damage_Decal` | 0xc0 | **sub-object (mesh) name**, not a bone; hidden at setup, shown on destruction |
| `Damage_Particles` | 0xe0 | **bone name** (e.g. `HP_F-L_EMITDAMAGE`); its subtree is hidden when the hardpoint instance is created and shown on destruction (instance +0x30) |
| `Engine_Particles` | 0x100 | **bone name** (e.g. `HP_E_MAINENGINES`); its subtree is hidden on destruction when the flag below is true (instance +0x34) |
| `Engine_Death_Hide_Engine_Particles` | 0x3d7 (bool) | gate for the `Engine_Particles` hide |
| `Death_Explosion_Particles` | 0x410 | object type spawned at the `Attachment_Bone` position on destruction |
| `Death_Breakoff_Prop` | 0x438 | object type spawned at the `Attachment_Bone` position on live destruction |
| `Death_Explosion_SFXEvent` | 0x160 | sound played on destruction [M] (which resolved pointer is used was not pinned) |
| `Fire_Bone_A` / `Fire_Bone_B` | 0x180 / 0x1a0 | bones in the attached model (instance +0x38 / +0x3c); targeting only, **not** used by the visibility rule |
| `Collision_Mesh` | 0xa0 | not read by the destruction path |
| `Is_Destroyable` | 0x4d | damage (0x387f50) and set-health (0x387ca0) do nothing when false |
| `Type` | 0x48 (enum) | `HARD_POINT_DUMMY_ART` (enum 12) returns immediately from 0x386660: dummy-art hardpoints never change visuals |

Enum order of `Type` (table at 0x140a436d0, strings 0x891840…): ENGINE 0, SHIELD_GENERATOR 1, GRAVITY_WELL 2,
FIGHTER_BAY 3, TRACTOR_BEAM 4, WEAPON_LASER 5, WEAPON_MISSILE 6, WEAPON_TORPEDO 7, WEAPON_ION_CANNON 8,
WEAPON_MASS_DRIVER 9, WEAPON_SPECIAL 10, ENABLE_SPECIAL_ABILITY 11, DUMMY_ART 12. [H]

**Answers.**

1. **"The other bone" is `Engine_Particles`** (HardPoint tag `<Engine_Particles>`, a bone name in the owner
   model, resolved case-insensitively at hardpoint instance creation 0x3989a0, -1 when empty or not found).
   [H] (0x3989a0 reads type +0x100; 0x386660 reads instance +0x34)
2. **+0x3d7 is `Engine_Death_Hide_Engine_Particles`, owned by the HardPoint type** (not GameObjectType).
   **Default true**: the HardPointType constructor 0x53e3b0 sets bytes 0x3d1–0x3d7 to 1 (the reset routine
   0x53ee10 also sets 0x3d7 = 1; the copy routine 0x472560 copies it). **No vanilla XML sets it** (0 hits in
   `foc_data_text.zip` and `eaw_data_text.zip`), so in vanilla every hardpoint with a non-empty
   `Engine_Particles` hides that subtree on death. Vanilla `Engine_Particles` users: 21 FoC hardpoints, all
   `HARD_POINT_ENGINE`, bone `HP_E_MAINENGINES` (two FoC Underworld ones use `HP_E_00_MAINENGINES` /
   `HP_E_01_MAINENGINES`). [H]
3. **Whole bone subtree, every sub-object kind.** The test (0x12dae0) takes the sub-object's attachment bone
   and walks parent links up to (not including) bone 0; it matches when it meets the target bone. So a
   sub-object matches when its own bone **is** the target bone **or any descendant** of it. Sub-objects are the
   entries of the model's attachment list (model +0xf0, 16-byte entries: bone index, object): ALO meshes,
   ALO particle proxies [M: proxies are in this list], and models attached by `Model_To_Attach` (0x12d770
   inserts into the same list). Edge cases: anything attached to bone 0, and a target bone of 0, never match.
   The bit changed is **sub-object +0x30 bit 0 (hidden)** — the same bit the ALO mesh-info `0x402` hidden u32
   writes at load (B.9). It is overwritten, not combined: **a mesh hidden in the ALO that lies under the
   `Damage_Particles` bone becomes visible on destruction**, and anything under the `Engine_Particles` bone is
   hidden regardless of its ALO flag. No other bit (e.g. bit 7 collision, bit 5) is touched. [H]
4. **`Model_To_Attach` is left as is**: 0x386660 and the gameplay routine it calls (0x384d00: engines/
   shields/gravity well/weapons/special ability off-line, debug strings only) neither hide, detach nor swap
   the attached model; there is **no** `Model_To_Attach_Destroyed`-style tag (the full vanilla HardPoint tag
   set is listed below). It is affected only if its `Attachment_Bone` lies inside the `Damage_Particles` or
   `Engine_Particles` subtree (not the case in vanilla, where `*_EMITDAMAGE` is a child of the attachment
   bone). [H] for those functions, [M] that no other path hides it. Effects on destruction, in order:
   (a) if the hardpoint is a weapon (enum 5–10) and was the last live one of its weapon class on the owner, an
   owner-type event is played [L]; (b) gameplay off-line (0x384d00); (c) **`Death_Explosion_Particles`**
   object spawned at the translation of the `Attachment_Bone` transform (fallback: `Fire_Bone_A` index; origin
   when neither) [M]; (d) death sound [M]; (e) **`Death_Breakoff_Prop`** object spawned at the same position,
   **only on live destruction** (third argument 0; the post-load re-apply 0x385590 passes 1 and skips it) [H];
   (f) owner bookkeeping (0x3a5700); (g) the visibility changes, in this order: `Damage_Decal` shown → 
   `Damage_Particles` subtree shown → `Engine_Particles` subtree hidden (if flag). "Damage particles" are thus
   the ALO proxies the artist placed under the `*_EMITDAMAGE` bone (e.g. `p_hp_imperial_damage`); a renderer
   that skips particles can skip them, but must still apply the mesh part. [H] for order
5. **Not reversible.** 0x386660 returns at once while hardpoint health > 0, so raising health (set-health
   0x387ca0, repair) runs nothing; there is no code path that re-hides the decal or the `Damage_Particles`
   subtree or re-shows the `Engine_Particles` subtree (the only three callers of the subtree test are
   instance creation and the two loops in 0x386660). There is also no partial-damage stage: nothing changes
   until health reaches 0. After a save-game load, 0x385590 rescales the saved health fraction and re-runs
   0x386660 with no breakoff prop, so destroyed hardpoints re-apply the same visuals. [H] for the functions
   named, [M] for "no other path"
6. **Order.** All writes are idempotent set/clear of the same bit, last writer wins. Instance creation
   hides each hardpoint's `Damage_Particles` subtree in `HardPoints` list order (after the type setup hid the
   decals). On destruction, a later destroyed hardpoint can re-show what an earlier one's `Engine_Particles`
   hid if the subtrees overlap, and vice versa; hardpoints that share bones (vanilla: `*_Supply_Dock` and
   `*_Supply_Dock_Dummy`, `*_Comm_Array` and `*_Comm_Array_Dummy` share `Attachment_Bone`, `Damage_Decal`
   and `Damage_Particles`) act on the same sub-objects — the dummy is never destroyed (DUMMY_ART returns,
   `Is_Destroyable = No`). There is no hardpoint hierarchy: destroying one never changes another's state.
   `Damage_Decal` resolves to the **first** sub-object with that name (0x12d520), so a duplicate-named mesh
   or a second attached copy of the same model is not reached. [H]

**Vanilla examples (FoC, checked against the shipped ALOs from `Models.meg`):**

| Type (model) | Hardpoint | `Attachment_Bone` | `Damage_Decal` | `Damage_Particles` | `Engine_Particles` | flag |
|---|---|---|---|---|---|---|
| `Star_Destroyer` (`EV_StarDestroyer.ALO`) | `HP_Star_Destroyer_Weapon_FL` | `HP_F-L_BONE` (bone 9) | `HP_F-L_BLAST` | `HP_F-L_EMITDAMAGE` (bone 10) | — | true (default) |
| `Star_Destroyer` | `HP_Star_Destroyer_Engines` | `HP_E_BONE` (51) | — | `HP_E_EMITDAMAGE` (52) | `HP_E_MAINENGINES` (56) | true (default) |
| `Empire_Star_Base_1` (`EB_STATION_01.ALO`) | `HP_Empire_Station_One_01` | `HP01_LC_BONE` (37) | `HP01_LC_BLAST` | `HP01_LC_EMITDAMAGE` (38) | — | true (default) |

Expected visibility (bone indices from the ALO; meshes carry ALO hidden = false unless noted):
- SD `Weapon_FL`: before death the mesh `HP_F-L_Blast` (on bone 15 `HP_F-L_Blast`, child of `HP_F-L_Bone`) is
  hidden and the two `p_hp_imperial_damage` proxies (bones 11, 12, children of `HP_F-L_EmitDamage`) are
  hidden; after death both are visible. `EV_StarDestroyer_HP00_L-F.alo` (attached at bone 9) stays visible and
  `Hardpoint_Breakoff_Star_Dest_Weapon_FL` is spawned.
- SD `Engines`: after death the three `p_hp_imperial_damage` proxies (bones 53–55 under `HP_E_EmitDamage`)
  become visible; `HP_E_MainEngines` has **no** attachments in this ALO, so the hide changes nothing — the
  engine glow `pe_stardestroyerengines` (bone 60 under `engines_big`, mesh `engines_big` ALO-hidden) is not
  under it and keeps burning. No decal, no breakoff prop.
- Station `HP_Empire_Station_One_01`: mesh `HP01_LC_Blast` (bone 45, child of `HP01_LC_Bone`) hidden → shown;
  proxies `p_RB_Station_Damage` (bones 41, 42 under `HP01_LC_EmitDamage`) hidden → shown; breakoff
  `Hardpoint_Breakoff_Empire_Station_One_Weapon_LC`.

**Tags the renderer/sim needs to parse for this rule:** `Type`, `Is_Destroyable`, `Health`, `Model_To_Attach`,
`Attachment_Bone`, `Damage_Decal`, `Damage_Particles`, `Engine_Particles`,
`Engine_Death_Hide_Engine_Particles` (bool, default true), plus for effects `Death_Explosion_Particles`,
`Death_Explosion_SFXEvent`, `Death_Breakoff_Prop`; the owner's `HardPoints` list gives the order. Full vanilla
HardPoint tag set (no destroyed-model tag exists): Allow_Opportunity_Fire_When_Idle/_When_Targeting,
Allows_Special_Weapon_Use, Attachment_Bone, Collision_Mesh, Damage_Decal, Damage_Particles, Damage_Type,
Death_Breakoff_Prop, Death_Explosion_Particles, Death_Explosion_SFXEvent, Engine_Particles,
Fighter_Bay_Flyout_Distance, Fire_Bone_A–D, Fire_Category_Restrictions, Fire_Cone_Height/Width,
Fire_Inaccuracy_Distance, Fire_Max/Min_Recharge_Seconds, Fire_Projectile_Type, Fire_Pulse_Count,
Fire_Pulse_Delay_Seconds, Fire_Range_Distance, Fire_SFXEvent, Fire_When_* (4), Health, Is_Destroyable,
Is_Targetable, Model_To_Attach, Projectile_Damage, Repair_Amount_Per_Frame, Repair_Cost_Per_Frame,
Special_Ability_Name, Tooltip_Text, Type.

---

## C. Open items (need capture or further RE)

1. Space FOW plane geometry and grid mapping (0x7ddfc0 renderable, 0x7de700); whether it draws at all when
   FOW is disabled for the map. [L]

> **Status 2026-10-30:** The space FOW plane is drawn: `WorldRenderer` loads `SpaceFogOfWar` and `W_Space_FOW_Grid.tga` (`engine/render/src/decorations.cpp:128, 223`), builds the plane at `kSpaceFowHeight` (`decorations.cpp:292-321`) and draws it in `Renderer::world_draw_space_fow` (`decorations.cpp:787`), which returns early unless a FOW grid has been set (`decorations.cpp:793`). The "does it draw when FOW is disabled for the map" part is not settled by the port.
2. Reader of the `SUN` bone cached by the SKY_DOME behaviour (lens flare / sun glare). [L]
3. Rotation order for the environment skydome angles (TEE `1D`/`1F`, `1E`/`20`). Capture: Kuat (second dome
   rotated 210°). [M]
4. Whether vanilla space ships actually carry shadow-volume meshes that cast in space (main scene shadows are
   enabled). Capture: DXVK draw count in a space battle with Shadow volumes on/off. [L]

> **Status 2026-10-30:** Answered by the apitrace capture in RENDER.md ("Shadow volumes in space: yes", `docs/re/RENDER.md:667`): about 20 shadow-volume draws per frame in a space battle, with two-sided stencil and colour writes off.
5. RENDER.md §7.1 q7 (top-down secondary pass) and q10 (effect-list order) not addressed here.

Capture additions (RENDER.md §7.2): space battle frame should show background-scene draws (stars, nebula,
planet) then one depth-only clear, then ships; no colour clear except the frame start (black).

---

## D. Remaining renderer questions (2026-10-27)

Written 2026-10-27 by the RE agent. Same rules as above (prose only, RVAs, [H]/[M]/[L]). Shader statements are
from the FXO disassembly in `docs/shaders/shaders_decompiled.zip`; asset statements were checked on the FoC
`models.meg` (+ Patch/Patch2/64Patch) with `tools/formats/python/alo.py`.

### D.1 Opaque grouping order (RENDER.md §7.1 q10) — **answered** [H] unless tagged

Inside every non-sorted bucket (all except 8 and 11) with "sort by effect" on, the draw order is:

1. **Effect order = the global effect list in creation order.** The list (`ReferenceListClass<alD3dEffect>`,
   global 0x140a12a20) is a doubly linked list; every new effect object is **appended at the tail** (0x20aa60)
   and the flush (0x18a6c0) walks it head → tail. Effects are never re-sorted or moved; a name that is
   already loaded returns the existing object (0x18a220), so the position is fixed by the **first load**.
   Creation order in a normal boot:
   1. the built-in default effect (0x189fa0, see D.3), then `Engine\Global` and `AlamoEngine` (same function);
   2. device-init engine effects in code order (0x17e1a0): `Engine\PrimOpaque`, `PrimAdditive`, `PrimAlpha`,
      `PrimModulate`, `PrimDepthSpriteAdditive`, `PrimDepthSpriteAlpha`, `PrimDepthSpriteModulate`,
      `StencilDarken`, `StencilDarkenToAlpha`, `StencilDarkenFinalBlur`, `PrimDiffuseAlpha`, `PrimHeat`,
      `PrimParticleBumpAlpha`, `PrimDecalBumpAlpha`, `PrimAlphaScanlines`, then `SceneBloom`, `SceneHeat`,
      `FrameEffectFakeMotionBlur` (the `Engine\Phase*` baselines are loaded by name too but never hold tasks);
   3. the boot preload (0x18a850 → 0x18a880): **pattern-major, then directory-major**: first `*.fx` in every
      shader directory in search order (each mod's `Shaders\`, `Shaders\Terrain\`, …, then
      `.\Data\Art\Shaders\`, `.\Data\Art\Shaders\Terrain\`), then `*.fxo` in the same directory order. A
      `.fxo` whose base name was already loaded as `.fx` is skipped (same object);
   4. anything loaded later by name (a material naming an effect outside the shader directories) is appended
      when first requested.
   Within one directory the finder (0x217e80 / 0x218e40) returns **loose files first** (FindFirstFile/
   FindNextFile, i.e. NTFS name order) and **then archive entries**: each mounted MEG in mount order, entries in
   table order (MEG tables are sorted ascending by CRC-32 of the upper-case path, docs/assets/README.md),
   names already returned are skipped. [H] for loose-then-archive and table order, [L] for the MEG mount order
   (vanilla shaders live in `shaders.meg`, `Patch.meg`, `Patch2.meg`, `64Patch.meg`; the iteration order of
   the mounted-archive list was not pinned).
   **Port recipe (vanilla, no loose shaders):** default → `Engine\Global` → the device-init list above → shader
   effects of `Data\Art\Shaders\` ordered by CRC-32 of `DATA\ART\SHADERS\<NAME>.FXO` (first archive that has
   the name) → `...\TERRAIN\` likewise. With loose mod shaders: loose names in case-insensitive name order
   before archived ones, per directory, mod directories first.
2. **Inside one effect: sort ascending by the material-instance key** (task +0x18 → instance +0x5c)
   (0x1b0eb0). The key is set by 0x18b690: a CRC-32 (0x215a70, table 0x140a14d20) over the 8-byte pointer
   stored at instance +0x28, chained over the **texture object pointer** of every texture-type parameter of the
   instance. It therefore depends on heap addresses and is **not reproducible across runs**; it only clusters
   instances that share textures. [H] for the computation, [M] that +0x28 is a pointer (8 bytes hashed as is).
   Port: use any stable key built from the effect id and the bound texture ids — the original order between
   different materials is itself run-dependent.
3. **Inside one key: submission order**, as long as the effect's batch has ≤ 32 tasks (insertion sort with a
   strict less-than, stable). Larger batches use an introsort (quicksort partition + heapsort fallback), so
   equal keys lose submission order. [H] (threshold 0x100 bytes of 8-byte pointers in 0x1b0eb0)
   Submission order = bucket list order (append, 0x149490 → 0x20aa60) = per collected renderable (scene
   collection order, RENDER.md §1.3 step 6), per mesh sub-object, per material slot (mesh +0xf0, 16-byte
   entries), and each slot's extra-pass tasks right after it.
4. Passes run **pass-major** over the whole batch (Begin; for each pass: BeginPass, every task, EndPass; End);
   tasks whose material is not a standard `alD3dEffect` instance (vtable +0x20 ≠ 0x140a12a70) are drawn after
   all batches, in submission order. [H] (0x1b03e0, 0x180dc0)

Aside: a mesh vtable method (0x14c8f0, caller not traced) flips every material slot of a mesh between bucket 3
(Opaque) and bucket 5 (OpaqueNonOccluding) and records the choice in mesh +0x31 bit 6 — this is the bucket 5
submitter RENDER.md §1.3 did not find. [M]

### D.2 Planet rendering — **answered** [H] unless tagged

- **Which meshes.** All 42 `alD3dVertN` submeshes with `Planet.fx` are in **one** model,
  `W_PLANET_DEATHSTAR_II_DESTROYED.ALO` (the Death Star II hull and its `chunk*`/`smallchunks*`/`Object*`
  pieces). Every other `Planet.fx` submesh (W_PLANET_MUSTAFAR_HIGH, NALHUTTA01, UTAPAU_HIGH/LOW, DEATHSTAR02,
  DEATHSTAR02_II, _II_CHUNKS, KESSEL_CINE, W_KESSEL_SHIPS) is `alD3dVertNU2U3U3` with a full frame. The
  destroyed model is not referenced by any vanilla XML or Lua text (it may be placed by a map or cinematic). [H]
  for the asset scan, [M] for "unreferenced"
- **The records do carry UVs.** These submeshes use 144-byte `0x10007` records; texcoord 0 is non-zero in every
  vertex, only the **tangent and binormal are zero** in every vertex. (Correction to docs/PHASE3.md "no
  tangents/uv".) [H]
- **The effect's vertex type wins.** While loading a submesh (0x187d40), if its material's effect is a standard
  `alD3dEffect` with a known `_ALAMO_VERTEX_TYPE`, that type **replaces** the ALO's vertex-format string; the
  records are then packed into the effect's layout (0x189800 → 0x17c550). So these meshes are uploaded as
  `alD3dVertNU2U3U3` with the stored UVs and **T = B = 0**. No special planet path exists. [H]
- **What `Planet.fx` computes** (`sph_t3`: vs_1_1 + ps_2_0; `sph_t2`: vs_1_1 + ps_1_4; FF `t0`): UV0 → base
  and normal map; cloud UV = (u + `TIME` × `CloudScrollRate`, v); the sun vector `DIR_LIGHT_OBJ_VEC_0` and the
  half vector (eye from `EYE_OBJ_POSITION`) are projected onto (T, B, N) in the VS, normalised and biased into
  texcoords; per pixel: light = sat(n·L)·`Diffuse`·`DIR_LIGHT_DIFFUSE_0` + the SH fill term (VS:
  `SPH_LIGHT_FILL` with the world normal × `Diffuse` × `LIGHT_SCALE`.rgb + `Emissive`); surface = (base rgb +
  `Atmosphere`.rgb × rim) × light × 2, rim = 2 × (1 − N·V)^`AtmospherePower` × `Atmosphere`.w (per vertex);
  + city lights = `CityColor` × base alpha × night factor sat(−32·N·L) (per vertex) × (1 − cloud alpha);
  + specular = sat(n·H)^16 × `DIR_LIGHT_SPECULAR_0` × `Specular` × **normal-map alpha**; clouds = 2 × (cloud
  rgb × sat(cloud-normal·L) × `DIR_LIGHT_DIFFUSE_0` + SH fill), blended over the result by cloud alpha; output
  alpha = `LIGHT_SCALE`.w. Constants: the material set (Emissive, Diffuse, Specular, Atmosphere, CityColor,
  AtmospherePower, CloudScrollRate, Base/Normal/Cloud/CloudNormal textures) plus engine semantics WORLD,
  WORLDVIEWPROJECTION, EYE_POSITION, EYE_OBJ_POSITION, DIR_LIGHT_OBJ_VEC_0, DIR_LIGHT_DIFFUSE_0/SPECULAR_0,
  SPH_LIGHT_FILL, LIGHT_SCALE, FOG_VALS, TIME. There is **no** PLANET_* engine constant and nothing
  sphere-mapped. [H]
- **Effect of T = B = 0** (the original image, reproduce it as is): the tangent-space light and half vectors
  collapse to (0, 0, ±1) — +1 where N·L > 0, −1 on the night side — so the per-pixel sun term is
  sat(±n_map.z): **flat, full-strength sun over the whole lit hemisphere with a hard terminator**, no
  normal-map relief, and specular is uniform where N·H > 0. The SH fill, atmosphere rim, night/city factor and
  clouds' UVs use N and UV and are unaffected. Exactly at N·L = 0 the normalisation divides by zero (D3D9
  vs_1_1 rsq(0) = +inf); the result is a sub-pixel seam. [H] for the algebra, [M] for the seam's appearance
- **How planets are drawn.** Ordinary models: `Planet.fx` declares `Opaque` → **bucket 3**; blending only when
  `LIGHT_SCALE`.w < 1 (SRCALPHA/INVSRCALPHA), Z write on, LESSEQUAL. Space backdrops are SpaceProps with
  `In_Background = yes` (§A.3) → background scene; `Scale_Factor` 500 (moons 150/300); 
  `Exclude_From_Distance_Fade` true; behaviour `IDLE` with `Loop_Idle_Anim_00 = yes`, so **any rotation comes
  from the model's own idle animation** — no engine spin code was found. Galactic planets
  (`Galactic_Model_Name`, e.g. `W_planet_Temperate01.alo`, Scale_Factor 1.8) go through the same model path
  in the galactic scene. [H] for bucket/blend/scene, [M] for "no engine spin", [L] for the meaning of
  `Layer_Z_Adjust` (−2500) and `Sort_Order_Adjust` (−1) (not traced; needs capture) → see §D.7 (placement,
  scale, background camera; both tags settled there)

### D.3 Missing or failing effect — **answered** [H]

Load by name (0x18a220) returns the **default effect** (global 0x140a6fec0) when neither `<name>.fx` nor
`<name>.fxo` is found, when the effect's technique selection fails (0x1ae720 returns the validation error and
0x18ada0 deletes the object), and also when a not-yet-loaded name is requested **off the main thread**. The
mesh is still drawn. The default effect (0x189fa0 → 0x1ae9c0) is compiled from a source string embedded in the
exe: one fixed-function technique `TNoShader` (LOD FIXEDFUNCTION), material ambient/diffuse/specular/emissive
all = (0, 1, 0, 1), stage 0 colour and alpha = DIFFUSE, stage 1 disabled, AlphaBlendEnable FALSE, SrcBlend ONE,
DestBlend ZERO, no vertex or pixel shader, no texture. With FF lighting on (0x197540) the emissive green
saturates: **solid opaque bright green (0, 1, 0)**, untextured. It has no `_ALAMO_*` annotations → bucket 3
(Opaque), vertex-processing id 0, so occlusion extra passes (which need a matching non-zero id, RENDER.md
§1.3) never attach to it. A file that exists but fails D3DXCreateEffect logs "Failed to load effect" and is
re-created from the **same green source** under its own name (0x1ae720). No warning is logged for a missing
file. So `FissureMeshGloss.fx` (no FXO shipped) renders as green, opaque, in bucket 3. [H] for the code, [M]
for the green being fully saturated (depends on the inherited lighting state, which 0x197540 sets on)

### D.4 Vertex colour in legacy `0x10005` models — **answered** [H]

- The legacy 128-byte record converter (0x1abdc0, called per vertex by 0x189630) copies position, normal,
  4 UVs, tangent and binormal (bytes 0–79) and the bone indices/weights (bytes 96–127), and **overwrites both
  colour float4s with 1.0**. The stored colour at +80 is discarded. So in the original every `0x10005` mesh
  has vertex colour **(1, 1, 1, 1)**; the stored zeros / grey levels / alpha 0 never reach the GPU.
- `W_STARS_BACK02` (`alD3dVertNU2C`, `MeshAdditiveVColor.fx`, `0x10005`) therefore renders at full texture
  brightness everywhere; nothing is black or transparent because of its stored colour.
- Shader use for `0x10007` (144-byte) meshes, where the stored colour **is** used: `MeshAdditiveVColor`,
  `RSkinAdditiveVColor`, `Nebula` use vertex **RGB only** (× `LIGHT_SCALE`.rgb × `LIGHT_SCALE`.w; output
  alpha forced to 1; additive ONE/ONE); `Skydome`, `MeshAdditiveReflection`, `MeshShield`, `MeshHeat` also
  pass vertex **alpha** through. Port: for `0x10005`, set colour = 1; for `0x10007`, use the stored colour.

### D.5 Tangent frames when the format has none — **answered** [H]

Unchanged from B.7: the engine never generates a frame, so there is no handedness rule. New detail: because
the effect's vertex type replaces the ALO's (D.2), a bump effect on a mesh exported without a frame still gets
the bump layout, filled from the record's tangent/binormal fields as stored — **zero** in vanilla data (the
42 Death Star II submeshes; the exporter writes zeros). The consequence is the flat-lit (0, 0, ±1)
tangent-space result described in D.2. Legacy `0x10005` records copy T/B verbatim. A raw `0x10003` vertex
blob is read with the (possibly replaced) type's stride, not converted. [H] for 0x187d40/0x189800/0x1abdc0,
[M] for the `0x10003` mismatch case (no vanilla example checked)

### D.6 Unlit / fallback draw path — **answered: none beyond D.3** [H]/[M]

There is no separate "unlit" renderer or string. The fallbacks are: (a) per effect, technique selection drops
to `DX8ATI`/`DX8`/`FIXEDFUNCTION` techniques by Shader Detail (RENDER.md §6); (b) an effect with no passing
technique becomes the green default effect (D.3). The default effect sets no alpha-test, fog, Z or cull state,
so those are inherited from the bucket's baseline (`Engine\PhaseOpaque` pass 0: alpha blend off, Z write on)
and whatever the previous effect left; fog follows the scene's fixed-function fog state (RENDER.md §2.4). [H]
for the absence of state in the source, [M] for the inherited alpha-test value (needs capture)

### D.7 Background objects: placement, scale and depth — **answered** [H] unless tagged

Written 2026-10-27 by the RE agent. Prompted by "W_PLANET_DEATHSTAR_II_DESTROYED at Scale_Factor 500 is ~2.1M
units, far past far = 100000". The §A/§D.2 reading is right; the premise is not: **no vanilla type puts that
model at scale 500.** Asset numbers are from the shipped ALOs (EaW `GameData` + FoC `models.meg`), placements
from the `.ted` object records (`0x4B0`, §1.6 of TERRAIN_WATER_POST.md).

**1. Placement and scale: plain world-space objects.**
- **No special background camera, no scaled-down world, no depth compression.** The background scene is drawn
  with the **same camera object** as the main scene: the space mode's Set_Camera (vtable 0x8792f8 +0xC0 =
  0x4d9710) hands the one camera (mode +0x90) to both the main RtsScene (+0x328) and the background SimpleScene
  (+0x320, its +0x10, setter 0x267f50). [H]
- `Scale_Factor` (type +0x658) is a **uniform scale of the object's model transform** (0x265d80 multiplies
  the model's 3×3 by it). The engine itself assumes a planet's world radius is `Scale_Factor` × 4.92: the
  galactic zoom (0x3c3ae0) computes exactly that. 4.92 is the radius of the `Hoth` sphere mesh that every
  `w_planet_*` model shares. So a 500 backdrop is a sphere of **radius 2460** world units. [H]
- **Position = the `.ted` value as stored.** On map load (0x2b0a20 case `0x459` → ctor 0x388720 → placement
  parser 0x39ce40) mini `04` is copied to the object position without any adjustment. Mini `05` is the Euler
  rotation (degrees). Mini `03` is read as a 4-byte integer into object +0x5c, **not a scale**: TERRAIN_WATER_POST
  §1.6 "03 f32 scale" is wrong for the loader. [H] for the loader, [L] for the meaning of +0x5c
  → Settled 2026-10-27: +0x5c is current health (f32); `03` is health / max tactical health, rescaled after
  load. Details and the 4 Endor objects in TERRAIN_WATER_POST §1.6 ("`03` correction"). [H]
- **`Layer_Z_Adjust`** (type +0xDFC) is a gameplay layer height, not a render parameter. It is added to the
  spawn z only when an object is created through the spawn constructor 0x388b60 with its 7th argument set
  (e.g. replacement objects, 0x29f270). Map-loaded objects never get it. Other readers: the starship and
  fighter locomotors (0x622b80, 0x6236b0, 0x5cbea0, 0x5cc220, 0x5ce010, 0x5ce4f0: flight layer), the
  galactic AI edge weight (0x693c30), and destination z in 0x39f580 / 0x39f800. No render-side reader was
  found. **Renderer: ignore it and use the `.ted` z.** The map editor already lowered vanilla planets to
  z −3700…−5000 (moons down to −12500). [H] for the listed readers, [M] for "no render reader"
- **`Sort_Order_Adjust` is ignored**: the string does not exist in StarWarsG.exe. `Exclude_From_Distance_Fade`
  (+0x86) makes no difference here, because fade is already off in the background scene. No
  `Background_*` / `Is_In_Background` tags exist: the only related string is `In_Background`. TEE `1B`/`1C`
  (skydome scale) are unused (§A.3). [H]
- **The Death Star II wreck.** `W_PLANET_DEATHSTAR_II_DESTROYED.ALO` has the same geometry and bounds as
  `w_planet_DeathStar02_II_chunks.alo`: x −1072…1146, y −1146…354, z −1683…1146 in model units, which are
  already world units. The only vanilla user of that geometry is `Death_Star_Chunks` (SpacePropsUnderworld.xml):
  `Scale_Factor` **1.0**, **not** `In_Background` (main scene), `Space_Obstacle_Radius` 1600. It is placed
  once, in FoC `_SPACE_PLANET_YAVIN_01` at (−4746, 2561, −3251), rotation z 346°. The DESTROYED model itself is
  referenced by no XML or Lua (§D.2). **Draw it at scale 1** if a map or tool shows it. The live
  `Death_Star_II` is `w_planet_deathstar02_II.alo` at scale 3.0 (hull radius 385 → 1156) and `Death_Star` is
  scale 2.5 (radius 963). Both are In_Background. [H]

**2. Background camera.**
- **Position and rotation are the tactical camera's, unchanged**: full translation, full parallax, no
  rotation-only mode. FOV is the same: `Space_Mode` 55° horizontal at 4:3 with the Hor+ rule of RENDER.md §3.4
  (vertical 42.67°, horizontal 69.5° at 16:9). [H]
- Per frame, the common tail (0x2b6aa0) temporarily sets that camera's **near 10 / far 100000**, rebuilds
  the projection (0x141260, 0x1417c0), sets fade begin/end to −1 (off) and the mode's fade pair to 100000,
  renders the background scene (vtable +0x50, 0x267c90), then restores the camera's near/far (0x2bec70) and
  fade. [H]
- Camera pose (0x2d0df0): start from identity (looking down −Z), translate to the look-at, rotate +90° about
  local X, rotate −yaw about the up axis, rotate −pitch about local X, then move `distance` along local +Z. With
  yaw 0 and pitch 50 the camera sits at look-at + distance × (0, −0.643, 0.766) and looks along (0, 0.643, −0.766),
  i.e. toward +Y and 50° down. Default space camera: distance 1000 → **766 above and 643 behind** the look-at
  point. [H] for the composition order (0x2cf8d0 is a post-multiplied rotation about local X), [M] for the yaw
  axis
- **Depth is tested and written normally** inside the background scene: per-effect Z state, phase baselines
  LESSEQUAL. The depth-only clear after the scene (§A.5) throws the result away. Depth precision at 10/100000
  is sufficient: vanilla backdrop surfaces lie 700–32000 from the camera. [H]
- Why it works: planets lie 2700–8300 from a default camera, so they cross the main far plane (7000) and the
  6000–7000 fade, but not 100000. Star domes (`w_stars_*`, radius **31729**, scale 1) and nebula domes
  (`w_stars_nebula_*`, radius 1248 × 25 = **31198**) are centred on the world origin. The camera never gets
  more than about 7600 from the origin (camera box ±5600 plus the 1900 maximum distance), so domes always
  enclose it with little visible parallax. [H] for data, [M] for "little parallax" (not measured)

**3. Draw order inside the background scene.** One SimpleScene render with the bucket mask 0xbbf (§A.5).
Within a bucket, effects run in creation order (§D.1) and depth resolves visibility. [H] unless tagged
- **Opaque bucket first**: the star-dome `stars` mesh (`MeshGloss.fx`, Z write on, Emissive 0.39), planet
  spheres (`Planet.fx`, Z write on), opaque planet extras such as Kuat's `building_ring` (`MeshGloss.fx`). Land
  `Skydome.fx` domes (opaque, Z write off) are not used by space content.
- **Transparent bucket**: nebula domes (`MeshAdditiveVColor.fx`, ONE/ONE, Z write off, cull none), planet
  `Glow` discs and the star-dome `sun_billboard` (`MeshAdditive.fx`, ONE/ONE, Z write off), black hole
  (`MeshAdditiveVColor` + `MeshAlpha`), and the **SpaceFogOfWar** plane (SRCALPHA/INVSRCALPHA, Z write off,
  cull none). The FOW plane is in the same alScene. It sits at z = `SpaceFOWHeight` (−80), between the camera
  and the planets, so it tints planets as well as stars. Additive layers are order-independent. Only the FOW
  plane's position in the sorted list matters, and it was not traced. [M]
- Nebula domes (radius 31198) lie just inside the star dome (31729), so they pass the depth test against it
  and add on top. Planets occlude both by depth.

**4. Animation and lighting.**
- **Idle spin = the model's `_IDLE_00` ALA**, looped (`Loop_Idle_Anim_00`). For `w_planet_alderaan_high`,
  `w_planet_kuat_high` and `w_planet_moon_high`: 240 frames at ALA fps **1.0**, with bone `Hoth` turning +360°
  about its local Z. Kuat's `building_ring` turns −360° over the same frames. Time base (RENDER.md §4):
  frame += fps × Δt × `Idle_Anim_00_Rate_Mod`, so **one revolution per 239 s** at rate 1.0: frame 239
  duplicates frame 0 and loops wrap at frame count − 1 = 239 (ANIM_PARTICLES.md §A.9)
  (`Coruscant_Backdrop` uses rate 0.25 → **956 s**). Kuat's `building_ring` lacks the duplicate frame, so it
  jumps 3° once per cycle (frame 238 → 0), as in the original. Frames advance only on 30 Hz ticks. The spin axis is local Z
  after the placement rotation (e.g. Kuat MP (348°, 22°, 6.5°)). [H] for the ALA data, [M] that the stored fps
  is used unmodified
- **Lighting**: the background light record is the active environment's ambient and sun (intensity, colour,
  specular, direction); **fill 1 and fill 2 are 0**, so `Planet.fx`'s SH fill term is ambient only. No
  stencil shadows, no fog, no fade (§A.4). [H]
- Billboards (SPEC §2.2 modes): the planet `Glow` bone is mode 6 (`sun_glow`). It is placed along the sun
  direction at the bone's parent distance: 0.656 model units in `w_planet_alderaan_high` → **≈328** world units
  at scale 500. The star dome's `sun_billboard` is mode 7 (`sun`). It is placed at **camera + sun direction ×
  22968**, so the sun disc follows the camera and always sits in the environment sun direction. [H] for the
  bone data, [M] for the runtime placement (from SPEC)

**Worked numbers.** All cases use the default `Space_Mode` camera, look-at (0, 0, 0), yaw 0, 16:9. NDC y is
+1 at the top; the size is the disc diameter in screen heights. Other look-at points shift these by parallax.
[H] for the inputs, [M] for the projected values (computed, not captured)

| map / object | `.ted` position | world radius | distance from camera | centre NDC (x, y) | angular radius | on screen |
|---|---|---|---|---|---|---|
| `_MP_SPACE_KUAT`, `Kuat_Backdrop_Large` (scale 500) | (241, −57, −4400) | sphere 2460, ring 3850, glow 3145 | 5205 (depth 2745–7665) | (0.08, −1.70) | 28.2° | centre below the bottom edge; the top limb is 5.4° below screen centre (NDC y ≈ −0.24), so the planet fills the lower ~38% of the screen; diameter ≈ 1.37 screen heights |
| `_MP_SPACE_ENDOR`, `Endor_Backdrop_Large` | (1623, 1150, −4500) | 2460 | 5795 | (0.45, −0.99) | 25.1° | large disc centred on the bottom edge, right of centre; ≈ 1.2 screen heights |
| `_MP_SPACE_ENDOR`, `Gas_Backdrop_Large` | (−5578, 4170, −5000) | 2460 | 9355 | (−1.07, −0.01) | 15.2° | half-visible at the left edge, mid-height; ≈ 0.70 screen heights |
| `_MP_SPACE_KESSEL`, `Moon_Large` (scale 300) | (−2024, 6796, −4500) | 1476 | 9336 | (−0.33, 0.67) | 9.1° | upper left; ≈ 0.41 screen heights |
| `_SPACE_PLANET_YAVIN_01`, `Death_Star_Chunks` (scale 1, **main scene**) | (−4746, 2561, −3251) | ≈1700 | 6995 (depth 5295–8695) | (−1.33, −0.06) | 14.1° | off the left edge at start; when in view it is clipped by the main far plane (7000) and fades from 6000 to 7000 |
| (wrong) DESTROYED at scale 500 | — | ≈850 000 | — | — | — | encloses the camera, which is the reported symptom |

Other vanilla MP planets follow the same pattern: x/y within ±5600, z −3700…−5000. Moons on `_SPACE_PLANET_*`
maps go down to −12500 (e.g. Dantooine `Moon_Large` at (−2976, 668, −12500)).

Port rule: draw `In_Background` objects with their XML `Scale_Factor` and `.ted` transform, using the
tactical camera unchanged except near 10 / far 100000. Use normal depth test/write per effect, environment
sun + ambient (fills 0), no fog and no fade. Then clear depth and draw the main scene.
`Layer_Z_Adjust` and `Sort_Order_Adjust` have no render effect.
