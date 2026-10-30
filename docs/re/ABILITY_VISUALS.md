# Ability visuals: stealth, tractor beam, sensor jamming, energy beams (StarWarsG.exe FoC x64 v1.121)

Written 2026-10-29 by the RE agent for roadmap groups R5 / S9 "render ability visuals". It covers the render side of
the tactical special abilities whose sim side is in ABILITIES_HARDPOINTS.md §3 (tractor §3.2, energy weapon §3.6,
stealth §3.7, sensor jamming §3.8, laser defense §3.10). It builds on RENDER.md, ANIM_PARTICLES.md, DECORATIONS.md,
PROJECTILE_NEBULA_DRAW.md, FOG_OF_WAR.md §5.4, TACTICAL_GAPS.md §1.5 and HUD_GAPS.md, and does not repeat them.
Clean-room: prose and tables only.

Conventions: addresses are RVAs (image base 0x140000000). [H] read directly, [M] inferred from strong evidence,
[L] guess. **probe** = needs an in-game run (Gabriel launches; in-game probes are paused). GC = GameConstants
(base 0x140b15800). FPS = 30. SAP = the object's special-ability data block (obj +0x100). XML values are vanilla FoC
(`extracted/foc_data_text.zip`).

## 0. Status

| § | Item | Confidence | Port change |
|---|---|---|---|
| 1.1 | Stealth fade spring: critically damped spring, value 0→1 over `Stealth_Transition_Time` | [H] | add per-object spring (render-only) |
| 1.2 | It drives a **colour/alpha multiply** (lerp white → `Stealth_Color`) on the object's model tint | [H] code, [M] which objects | multiply `light_scale` |
| 1.3 | Mesh swap also hides **hardpoint meshes**; stencil shadow switched off while stealthed | [H] code, [M] meaning | hide hardpoint meshes; drop shadow caster |
| 1.4 | Owner / ally / enemy / detected visibility | [H] | replace the linear fog fade with the 0.25 s spring |
| 2 | Tractor beam: GC `Tractor_Beam_*` ribbon from `HP_trac_bone` to target centre, 4 travelling dots, `Tractor_Beam` lightning helix | [H] data path, [M] geometry details | new beam pass |
| 3 | Sensor jamming: jammer particle or radius blob/decal; jammed units have no visual, they drop off the enemy radar | [H] | blob/decal + radar filter |
| 4.1 | Energy weapon (Piet / Accuser): same ribbon class, GC `Energy_Beam_*` or per-ability `Beam_*`, end at the target hardpoint, 10 dots, `Piet_Energy_Weapon` lightning | [H] data path, [M] geometry | same beam pass |
| 4.2 | Super laser (Death Star, Eclipse): tiled scrolling ribbon, `Line_*` fields, no dots, no lightning | [H] data path, [M] geometry | same beam pass, mode 1 |
| 4.3 | Laser defense zap, Leech Shields | [H] laser defense, [L] leech | same beam pass |
| 4.4 | No other continuous-beam projectile class: projectile `Projectile_Custom_Render` 1/2 are moving bolts (PROJECTILE_NEBULA_DRAW §1) | [M] | none |

> **Status 2026-10-30:** Every "Port change" entry above is implemented in `engine/render/`. Stealth tint spring, hardpoint-mesh hiding, shadow drop and the 0.25 s HideWhenFogged spring: `stealth_fx.cpp` / `stealth_fx.hpp`, `sim_view.cpp:116-118, 187, 254, 336-366`. The beam pass (tractor, energy weapon, super laser, laser-defense zap, leech): `beams.cpp`, `beams_draw.cpp`, `sim_beams.cpp`. Sensor jamming blob and radar filter: `jam_fx.cpp` (§8).

---

## 1. Stealth (`Stealth_Ability` 48, `Force_Cloak_Ability` 54)

### 1.1 Fade spring (update 0x592e80 stealth, 0x714b40 force cloak) [H]
SAP fields (render-only floats, never CRC'd):

| SAP | Meaning |
|---|---|
| +0x290 | value v (0 = normal look, 1 = full stealth look) |
| +0x294 | target: **1.0** on apply (0x5926e0), **0.0** on break (0x592d00) and on remove (0x5931b0) |
| +0x298 | velocity |
| +0x29c / +0x2a0 | smooth time T and 1/T. Apply and remove use the ability's `Stealth_Transition_Time` (+0x188; `Force_Cloak_Transition_In_Secs` for 54); the break path uses its own argument (0.3 s when firing, stun, bribe, earthquake) |
| +0x2a8..+0x2b4 | output RGBA tint |

Every update, in this order: ω = 2/T, x = ω·dt, k = 1 / (1 + x + 0.48x² + 0.235x³);
c = v − target; t = (vel + ω·c)·dt; vel := (vel − ω·t)·k; v := target + (c + t)·k.
This is the usual critically damped "smooth damp" approximation: about 90 % of the way after 2T.
dt is the global frame-time float 0x140b0a344 ([M] = 1/FPS, because the update runs on sim frames).
Then tint = white + (`Stealth_Color` − white)·v per channel, RGBA. White is a static (1,1,1,1).

`Stealth_Color` (+0x178, 4 floats, 0–255 in XML, so 0–1 in memory [M]). Vanilla values:

| Units | `Stealth_Color` | T |
|---|---|---|
| Tyber Zann, TIE Phantom, Vengeance, F9TZ transport | 255,255,255,255 (tint does nothing) | 0.3–0.8 |
| Noghri | 255,255,255,64 (≈25 % opacity) | 0.5 |
| Urai Fen, Hutt unit | 255,64,64,120 (red, ≈47 % opacity) | 0.6 |
| Luke (Force_Cloak_Color) | 255,255,255,255 | 0.4 |

So for most stealth units the visible effect is only the mesh swap (§1.3). The STEALTH mesh carries its own
(model-specific) material.

### 1.2 Where the tint is applied [H code, M scope]
The per-model colour callback 0x3a0c20 (installed as model +0x150; it builds the model's RGBA multiplier, which is
LIGHT_SCALE, RENDER.md) multiplies the colour by SAP +0x2a8..+0x2b4 when **v > 0.01**. It does this for an object
that has STEALTH or FORCE_CLOAK active, or for the members of a team/squad container (behaviour 0x16 list) that has
it [M]. It reads the object's own SAP. Neither the tint nor the mesh swap depends on the viewer, so allies and
detected enemies see the same look.

### 1.3 Mesh and shadow changes [H code, M meaning]
- Apply: every mesh whose name contains `BASEMESH` is hidden (0x3a8280, substring match [M]), every mesh containing
  `STEALTH` is shown. **Hardpoint meshes are hidden too** (0x3a8140): for each attached hardpoint, the mesh named
  like the hardpoint's attached model file (upper-cased, cut at `.ALO`) gets the hidden flag. Break and remove
  reverse all three. The swap is instant. Only the tint fades.
- Shadow: every frame while the stealth duration counter runs, the model's stencil-shadow bit (model +0x31 bit 0;
  when clear, the SHADOW bucket 0x40 is removed from the model's phase mask, RENDER.md §1.3) is cleared.
  - Force cloak sets it again once target = 0 and |v| < 0.01.
  - Stealth only sets it again when, with target 0, the previous v was < 0.01 **and** the new v is ≥ 0.01. Apart
    from a break in the first frames after apply, that never happens. The bit only comes back at the next model
    set-up (0x3a4820 / 0x3a8ff0 / 0x3be4b0, which set it from the type's shadow mode 0x373ac0: 0 none, 1 blob,
    2 stencil). So in retail a stealth unit probably keeps no stencil shadow after its first stealth.
    **probe** (Phantom in space with shadows on).

### 1.4 Who sees what [H]
| Viewer | While stealthed (`Is_Stealthed`) | Stealth flag set but detected (last-detected + `Stealth_Detection_Time` ≥ now) |
|---|---|---|
| owner / allies | visible: STEALTH mesh, tint, no shadow | same |
| enemy of the local player | **hidden** by `HideWhenFoggedBehavior` (0x53ddc0): its visibility target is 0 for a hostile `Is_Stealthed` object | visible with the stealth look; `SFX_Stealth_Reveal` plays on the transition (0x592ae0) |

- HideWhenFogged visibility (behaviour +0x40) uses the **same spring** with ω = 2 × 4.0 = 8/s (T = 0.25 s, ctor
  0x53da00). Instant when type +0x88 is set. The model is hidden below 0.025. The alpha multiplies the model alpha
  (0x3a0c20 reads behaviour 0x19 +0x40).
- The service runs when its 30-frame timer expires or while the alpha is still moving. A newly stealthed enemy can
  therefore stay visible up to 1 s before it starts to fade.
- Radar blip, health bar and hardpoint reticles are hidden for hostile stealthed objects (TACTICAL_GAPS §1.5 0x35b4d0,
  HUD_GAPS).

---

## 2. Tractor beam visual (0x710c70, called by apply 0x710130 and every update 0x710710) [H unless tagged]
- **Gate:** nothing is drawn when the game mode has no render scene (mode vf +0x100 = null).
- **Ribbon:** a `LineSubClass` (0x37e3a0, mode 0) kept in SAP +0x238.
  - Texture GC `Tractor_Beam_Texture` (`tractor_beam00.tga`), width GC `Tractor_Beam_Width` (10).
  - Colour GC `Tractor_Beam_Color` (0,75,0, a dark green), animation enabled with GC `Tractor_Beam_Frames` (10).
  - Render prim mode 1 = ADDITIVE (`Engine\PrimAdditive`, ONE/ONE), so alpha is irrelevant.
  - Submitted to bucket 8 (TRANSPARENT) by 0x3801d0.
- **Endpoints**, refreshed on every update (0x37eaa0 rebuilds the geometry):
  - start = world position of bone `HP_trac_bone` in the owner model (0x397b60), else the owner position;
  - end = target position + (0, 0, target type `Ranged_Target_Z_Adjust` +0x4dc) (0x398440). There is no hardpoint
    targeting for the tractor.
- **Mode 0 geometry** (0x37eaa0):
  - one quad: local ±W/2 across, 0..L along the start→end axis;
  - U runs across the width (1 → 0), V runs along the length (1 at the start, 0 at the end): the texture is
    stretched once over the whole beam;
  - vertex colour = the beam colour.
  - Local frame: built from start→end with local Z along the beam (0x22d600). At draw (0x37fa50) it is
    re-orthonormalised against the fixed vector (0,0,1) (0x148700). So the ribbon's width axis is horizontal and
    the ribbon faces up. It is not a camera billboard [M]. **probe** from a low side view.
- **Dots:** 4 billboard quads (0x37e650), size 3.5 × 3.5, texture `w_galaxy_dot.tga`, also additive.
  - Colour = clamp(0.7 × beam colour) (0x1f9860).
  - Start fractions 0.45, 0.70, 0.50, 0.89 along the beam. The negative sign in the call makes them **travel from
    the target towards the owner**: 1/50 of the beam length per refresh (counter step −1, period 50), wrapped into
    [0, L] (0x37f6f0).
  - They lie in the ribbon plane. UVs come from the fixed table 0x140a28610.
- **Lightning helix:** a `LightningEffectClass` named `Tractor_Beam` (LightningEffectTypes.xml) is created between
  the same endpoints (0x481ce0) and moved each update (0x7dd470).
  - Data: Width 1, Displace 0.01, Detail 0.1, Color_Start = Color_End = 255,255,0,0, Fadeout 100–101 s,
    Update_All, Cycles_Per_Distance −0.01, Rotations_Per_Second 3, Radius 5, 3 bolts at intervals 0–0.1 s.
  - Reading: three thin yellow strands spiralling at radius 5 around the beam axis at 3 rev/s [M].
  - Bolt creation (0x7dcd40) uses the **render** LCG 0x140a13e20, not the synced one.
  - The bolts are drawn through the spline renderer with the texture forced to `lightning_default.tga` (0x7e0cf0).
  - The blend mode was not traced [L: additive]. Whether colour alpha 0 hides the strands needs a **probe**.
- **Target side:** no particle and no tint. The unit-ability record's SFX loop (+0x1a0) is started on owner and
  target (0x3a7f70, audio). The "tractored" flag (target CD +0x132) has no visual reader.
- **Remove** (0x710880): line, line render object and lightning effect are destroyed (only when a scene exists).

---

## 3. Sensor jamming visual (apply 0x719000, update 0x719620, remove 0x719840) [H unless tagged]
- **Jammer with `Owner_Particle_Effect`** (Skipray, Houndstooth):
  - Apply creates the particle object (0x38d230 → Create_Object 0x29f810) at the first `Owner_Particle_Bone_Name`
    bone found, else `ROOT`. Its id is kept in SAP +0x39c and removed on remove.
  - Vanilla: `Sensor_Jamming_Effect(_Skipray)` = `p_sensorjammer.alo`, scale 1.
  - This is a real game object (it takes an object id): sim-visible, see §6.
- **Jammer without it** (the two Underworld jammer structures, `COMBAT_AUTOMATIC`):
  - What it draws: a **radius blob** (0x719180, refreshed every update by 0x7198d0). Material
    `Blob_Material_Name` (+0x180) comes from ShadowBlobMaterials.xml (vanilla `Sensor_Jamming_Blob` =
    `i_circle00.tga`, ALPHA); colour `Blob_Color` (+0x1a0, vanilla 200,50,200,128, purple at 50 %), alpha 1.
    It is a square of side **2 × radius** centred on the jammer position, no rotation [M: side vs half-side].
    The radius is unit ability 63 `Effective_Radius`, else the type's `Passive_Missile_Shield_Radius` (+0x23e8;
    vanilla 300 / 1000).
  - Land mode: a terrain decal (0x12f3e0, added to the terrain decal list; DECORATIONS.md §4).
  - Space mode: an `alBlob` (0x13a2d0) added to the scene.
  - Who sees it: drawn only when the **local player is allied** with the jammer's owner. With
    `Show_Blob_To_Enemies` (+0x1b0) it is also drawn for others, except while the jammer position is fogged for the
    local player (then it is removed).
- **Jammed units** (CD +0x194 stamped, `Is_Jammed` < 0.5 s): no model, tint or particle change.
  - The radar/minimap filter 0x35b4d0 drops the blip of an object that is **hostile to the local player and
    jammed** (the same test also drops stealthed ones). The jammer stamps its own side, so the enemy loses those
    units from the radar.
  - The other effect is sim-side (homing missiles fly straight, TACTICAL_GAPS §1.5).

---

## 4. Energy beams

### 4.1 `LineSubClass` (the shared beam renderer) [H structure, M geometry]
| Field | Meaning |
|---|---|
| +0xcc / +0xd8 | start / end (world) |
| +0xc8 | width W (≤ 0 → a default from 0x271cd0) |
| +0x124 | colour RGBA, packed into the vertex colour |
| +0x140 / +0x138 | ribbon texture / dot texture |
| +0x968 | mode: 0 = one stretched quad + dots, 1 = tiled scrolling ribbon |
| +0x114 / +0x118 / +0x11c / +0x120 | animation on / frame / frame count / reverse |
| +0x948 / +0x94c | U / V offset added at draw |
| +0x950 | dot records (0x74 bytes: fraction, period 50, size, colour, counter, 6 vertices, step ±1) |
| +0x98c | prim render mode (all ability callers pass 1 = ADDITIVE) |

- **Mode 1** (0x37eaa0):
  - Segments of length 200·W, each a quad with U from 0 to 200 along the length, so **one texture repeat per W
    world units**. V runs 1 → 0 across the width. The last partial segment has U = remainder / W.
  - Each `0x380110` tick advances the frame (wrap at frame count, or backwards when reversed), and U offset =
    frame / frames. **The texture scrolls one repeat every `frames` ticks**, start → end [M direction].
- **Mode 0:** U offset 0; the dots advance instead (§2).
- **Draw** (0x37fa50): world matrix from §2; ribbon vertices with the packed colour; prim mode 1; then the dot
  quads with the dot texture, mode 1. One `alSimpleRenderTask` per line in bucket 8.

### 4.2 Energy weapon (`Energy_Weapon_Attack_Ability` 31; Piett's Accuser), visual 0x6fa340 [H unless tagged]
- **Ribbon:** a `LineSubClass`, mode 0, in SAP +0x248. Per-ability fields override GC:

  | Property | Per-ability field | GC fallback |
  |---|---|---|
  | colour | `Beam_Color` (+0x184), used if any of RGB ≠ 0 | `Energy_Beam_Color` 255,75,0 (orange) |
  | width | `Beam_Width` (+0x194), used if ≠ 0 | `Energy_Beam_Width` 30 |
  | frames | `Beam_Frames` (+0x198), used if ≠ 0 | `Energy_Beam_Frames` 10 |
  | texture | `Beam_Texture` (+0x1a0), used if non-empty | `Energy_Beam_Texture` `tractor_beam00.tga` |

  (xml_field_tables labels this segment "Leech_Shields_Ability (5)"; the reader is the energy weapon.)
  Vanilla Piett uses the GC values.
- **Dots:** 10. Fractions come from a van der Corput base-2 sequence (0x241830; start index not read [L]); they are
  positive, so they **travel owner → target**. Size 3.5, colour 0.7 × beam colour.
- **Endpoints:**
  - start = `HP_trac_bone` or the owner position;
  - end = the **targeted hardpoint's bone** world position (0x385c70; hardpoint id in SAP +0x258, taken from the
    attacker's current hardpoint target or the best hardpoint 0x394610), else the target centre + `Ranged_Target_Z_Adjust`.
- **Lightning:** `Piet_Energy_Weapon` (width 0.3–3, displace 0.05, detail 2, colour 255,75,0,0 → 255,255,0,0,
  fade 0–0.1 s, 5 bolts at 0–0.1 s, Update_All): a flickering orange/yellow jagged arc along the beam [M].
- **Owner particle:** every visual refresh calls 0x48e6b0, which **creates** `Owner_Particle_Effect`
  (`Piet_Powerup_Particle_Effect` = `P_piet_powerup.alo`, scale 20, lifetime 60 frames) at the first existing
  `Owner_Particle_Bone_Name` bone (`HP_trac_bone`), else `ROOT`, via Create_Object. A new one is made on every
  update [H for the call, M that the update runs every frame].
- **Remove** (0x6fa120): line, render object and lightning destroyed; target hardpoint cleared.

### 4.3 Super laser (`Super_Laser_Ability` 55; Death Star, Eclipse) [H unless tagged]
- **Timeline:**
  - Apply (0x714fa0): earliest-fire frame (SAP +0x304) = now + int(FPS × `Fire_Delay`) (vanilla 0); game event
    `super_laser_fired`.
  - Without `Face_Target`: the attack animation (type 5, sub-index `Attack_Anim_Sub_Index`) starts immediately.
  - With `Face_Target` (Death Star): the facing behaviour (0x2c) gets the target and `Turn_Rate`; update 0x715450
    starts the animation once the target is inside `Firing_Cone`.
  - Firing starts once the animation frame ≥ `Fire_Frame` (60 Death Star, 106 Eclipse). From then on each update
    applies `Damage_Per_Frame` and draws the beam.
  - When the target is lost, the beam keeps drawing until SAP +0x308 = now + FPS × `Fire_Continuation` (0.01 s).
    It goes to the remembered point SAP +0x30c = start + **2 × (target − start)**, i.e. it extends past the old
    target to twice the distance.
- **Visual** (0x715a70):
  - a `LineSubClass` in **mode 1** (SAP +0x2f0);
  - texture `Beam_Texture_Name` (`tractor_beam01.tga`), width `Line_Width` (35 / 45), colour `Line_Color`
    (0,237,119,255 green), frame count `Num_Anim_Frames` (5), so the texture scrolls a full repeat every 5 updates;
  - **no dots** (none are added) and **no lightning**: SAP +0x2f8 is freed on remove but never created [M];
  - start = the first `Owner_Particle_Bone_Name` bone (`MuzzleA_00`), else the owner position; end = target centre
    + `Ranged_Target_Z_Adjust`.
  - 0x48e6b0 also runs here; the Death Star has `Owner_Particle_Effect` commented out, the Eclipse spawns
    `Piet_Powerup_Particle_Effect` at `MuzzleA_00`.
- **Remove** (0x715350): attack animation replaced by idle.
- The Death Star planet-destruction shot (galactic / cinematic) is a different system and is not covered here.

### 4.4 Laser defense zap and Leech Shields
- **Laser defense** (0x716850) [H]: `LineSubClass` mode 0 without dots, colour `Laser_Color` (Crusader 34,34,235),
  width `Laser_Beam_Width` (4), texture `Laser_Beam_Texture` (`tractor_beam00.tga`).
  - From the nearest `Bone_Names` bone (SAP +0x338) to the destroyed projectile's position (SAP +0x320).
  - **Lifetime:** it is removed when the active-frame counter is a multiple of `Laser_Beam_Frames` (2), so it lasts
    ≤ 2 frames.
  - The same 0x48e6b0 owner-particle call applies. `Lightning_Effect_Name` was not seen being used [L].
- **Leech Shields** (0x7182d0) builds the same kind of line with `Beam_*` fields (+0x18c colour, +0x1c4 width,
  +0x1c8 texture, `Beam_Bone_Name`) plus the `Leech_Shields` lightning effect [L, not traced].

---

## 5. What the port draws today (engine/render, read only)

> **Status 2026-10-30:** This section describes the port before R5 and is obsolete. The port now has the stealth tint spring, hidden hardpoint meshes and dropped shadow (`stealth_fx.cpp`, `sim_view.cpp:254, 336-366`), the HideWhenFogged spring instead of the linear 15-tick fade (`sim_view.cpp:187`), a beam / lightning pass in the Transparent bucket (`beams.cpp`, `beams_draw.cpp`, `sim_beams.cpp`) and the sensor-jamming blob (`jam_fx.cpp`). Projectile lasers are still `lasers.cpp`.
- Stealth (`sim_view.cpp`, `kSnapStealthMeshes`): instant BASEMESH / STEALTH swap. Missing: hardpoint meshes are not
  hidden, there is no tint spring, and the shadow is not removed.
- Enemy hide: linear `fog_fade_ticks_` = 15 ticks. Retail uses the 0.25 s critically damped spring and its own
  30-frame service.
- Ability `Particle_Effect` snapshot effects (R4).
- No line / beam / lightning objects for any ability. Projectile lasers (`lasers.cpp`) are the bolts of
  PROJECTILE_NEBULA_DRAW.

---

## 6. Port-affecting findings

1. **Sim snapshot inputs needed** (per object, presentation only):
   - Stealth: the applied flag (SAP +0x2a4) and the fade target (1 on apply, 0 on break/remove). Also the transition
     time T: `Stealth_Transition_Time` / `Force_Cloak_Transition_In_Secs` on apply and remove, or 0.3 s on a
     firing / stun / bribe / earthquake break. The type's `Stealth_Color` goes in the type table.
     `Is_Stealthed` per local player already feeds `kSnapFogHidden`.
   - Beams: one record per active tractor (30), energy weapon (31), super laser (55, also during the continuation
     window), laser-defense zap (56) and leech: kind, owner id, target id (or a fixed end point for the zap /
     continuation), the energy weapon's target hardpoint index, and the ability's beam fields resolved to
     texture / width / colour / frames (GC fallbacks as §4.2).
   - Jamming: per active jammer its radius and the `Blob_*` fields (or "has owner particle");
     `Show_Blob_To_Enemies`. A per-object `Is_Jammed` bit for the radar filter.
2. **Render-only state** (never CRC'd, rule 3 untouched):
   - the stealth spring (v, vel, target, T);
   - the HideWhenFogged spring (T = 0.25 s);
   - per-line animation frame and dot counters;
   - lightning bolts (use a render RNG, as retail uses 0x140a13e20).
   Step the springs with dt = 1/30 once per sim tick, as retail does.
3. **Draw pass:**
   - **Model tint:** `light_scale` ×= lerp(1, `Stealth_Color`, v) when v > 0.01.
   - **Mesh mask:** also hide hardpoint meshes (named after the hardpoint's attached model, `.ALO` stripped) and
     clear the shadow-caster flag while the stealth counter runs. Matching retail means not restoring it for
     `Stealth_Ability` until a model rebuild (**probe**); Force_Cloak restores it when v < 0.01.
   - **Beams:** a new transparent-bucket (8) additive pass. It draws ribbons (mode 0 stretched / mode 1 tiled +
     scrolling), billboard dots (`w_galaxy_dot.tga`, 3.5 units, 0.7 × colour, 50-tick traversal; tractor towards
     the owner, energy weapon towards the target) and the LightningEffect types (`Tractor_Beam` helix,
     `Piet_Energy_Weapon` arc).
   - **Endpoints:** `HP_trac_bone` (tractor, energy) / first `Owner_Particle_Bone_Name` (super laser) / nearest
     `Bone_Names` (zap) on the owner. The far end is the target centre + `Ranged_Target_Z_Adjust`, or the
     hardpoint bone (energy weapon).
   - **Jamming:** an ALPHA blob/decal of side 2r using ShadowBlobMaterials, shown to allies (and to enemies with
     `Show_Blob_To_Enemies` when not fogged). The radar drops hostile jammed blips.
4. **Sim side effect to hand to the sim owner:** energy weapon, super laser and laser defense call
   `Owner_Particle_Effect` creation (Create_Object) on every visual refresh. Sensor jamming does it once on apply.
   - These are store objects that consume ids. In retail they are skipped when the game mode has no render scene,
     so the retail object set depends on rendering.
   - The port should either create them deterministically in the sim, or keep them render-side as particles and
     accept the id divergence (parity is by behaviour, not CRC).
   - **Decision (coordinator, 2026-10-30, applying Gabriel's parity-by-behaviour rule; open to reversal): render-side.** Energy weapon / super laser / laser defense / jamming
     `Owner_Particle_Effect` visuals are render particles (SnapshotEffect through the R4 attach path); no sim objects are
     created and no object ids are consumed. The id divergence from retail is accepted (parity is by behaviour, CLAUDE.md).
     Implemented for jamming (t05, §8); the beam abilities follow with the beam pass (R5-B).

> **Status 2026-10-30:** The beam pass (R5-B) exists: `engine/render/src/beams.cpp`, `beams_draw.cpp` and `sim_beams.cpp` (`SnapshotBeam` records resolved to world endpoints per frame, drawn additive in the Transparent bucket).

5. **Open / probe:**
   - ribbon orientation (faces up, not a camera billboard);
   - whether the `Tractor_Beam` lightning (alpha 0) is visible;
   - stealth shadow loss;
   - the lightning spline blend mode;
   - the Halton start index of the energy-weapon dots;
   - the special-ability update interval (assumed every frame).

## 7. Function index
| RVA | Function |
|---|---|
| 0x5926e0 / 0x592e80 / 0x592d00 / 0x5931b0 | stealth apply / update (spring) / break / remove |
| 0x714b40 | force cloak update (spring, shadow restore) |
| 0x3a8280 / 0x3a8140 | hide meshes by name / hide hardpoint meshes |
| 0x3a0c20 | per-model colour callback (stealth tint, HideWhenFogged alpha, …) |
| 0x53da00 / 0x53ddc0 | HideWhenFogged ctor / service |
| 0x373ac0 | type shadow mode (0 none, 1 blob, 2 stencil) |
| 0x710c70 / 0x710880 | tractor visual / remove |
| 0x397b60 | `HP_trac_bone` world position |
| 0x398440 | target centre + `Ranged_Target_Z_Adjust` |
| 0x37e3a0 / 0x37eaa0 / 0x37e650 / 0x37e910 | LineSubClass ctor / create line / add dot / set frames |
| 0x37f6f0 / 0x380110 / 0x37fa50 / 0x3801d0 | LineSubClass animate / tick / draw / submit (bucket 8) |
| 0x481ce0 / 0x7dd470 / 0x7dcd00 | lightning effect create / move / destroy |
| 0x7dcd40 / 0x7e01f0 / 0x7e0cf0 | LightningEffectClass update / alLightningBolt ctor / draw |
| 0x6f9ad0 / 0x6f9e60 / 0x6fa340 / 0x6fa120 | energy weapon apply / update / visual / remove |
| 0x714fa0 / 0x715450 / 0x715a70 / 0x715350 | super laser apply / update / visual / remove |
| 0x716730 / 0x716850 | laser defense update / zap visual |
| 0x719000 / 0x719620 / 0x719180 / 0x7198d0 / 0x7194b0 / 0x719840 | jamming apply / update / blob create / blob refresh / blob destroy / remove |
| 0x48e6b0 / 0x38d230 | owner particle at bone / create attached particle object |
| 0x35b4d0 | radar blip filter |

## 8. Port implementation: sensor jamming (R5 t05, 2026-10-30)
- Sim input (presentation only, no CRC change): `SpecialAbilityDesc` parses `Blob_Material_Name`, `Blob_Color` (0-255 -> 0-1),
  `Show_Blob_To_Enemies`, `Owner_Particle_Effect`, first `Owner_Particle_Bone_Name` for Sensor_Jamming_Ability only;
  `AbilitySystem::jamming_special(id, radius)` is a const accessor (applied jammer + the radius its update jams with).
- Snapshot (`apply_battle_jamming`, `SnapshotJamming`): `jammed` = ids with `CombatSystem::is_jammed`; `jammers` = one record per
  applied jammer without an owner particle (radius, allied-to-local = not `is_enemy`, `Show_Blob_To_Enemies`, material, colour).
  A jammer with `Owner_Particle_Effect` becomes a `SnapshotEffect` (`kEffectJamming`, bone) instead: the existing R4 particle path
  spawns it on the jammer, no blob. The digest hashes the section only when it is non-empty.
- Draw (`render/jam_fx.hpp`): a flat quad of side 2 x radius at the jammer position (+0.5 z lift; land: the ground height
  under it), ALPHA material (ShadowBlobMaterials, vanilla `Sensor_Jamming_Blob` = `i_circle00.tga`), colour `Blob_Color`.
  Shown to the jammer's allies; to others only with `Show_Blob_To_Enemies` and while the jammer is not fogged (`kSnapFogHidden`).
  **[M]** the material is the standard `MeshAlpha.fx` (texture x Diffuse = Blob_Color, scene-lit) instead of the original's PrimAlpha
  prim path, and a land blob is flat at the jammer's ground height instead of draped over the terrain grid. Visual check pending.
- Radar: `ui::HudUnit::jammed` (from `SnapshotJamming::is_jammed`); `TacticalHud::build_radar` drops the blip of a unit that is hostile
  to the local player and jammed (0x35b4d0). The same filter drops hostile stealthed units (2026-10-30): `SnapshotJamming::stealthed` (hostile and
  `AbilitySystem::is_stealthed`, so a unit that is detected stays visible) -> `HudUnit::stealthed` -> the same `build_radar` test. The same flag hides the health bars (`WorldHud::build`, and the port's
  world unit icon that carries one) and the hard-point reticles (`filter_hardpoints` skips a hovered stealthed ship); the selection
  bracket is kept [M: the spec names only radar blip, health bar and reticles].
