# Animation playback and particle systems (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent for roadmap steps **R3 "Animation playback"** and **R4 "Particle system"**.
Clean-room: prose and tables only. The file has no code, no listings and no pseudo-code from the decompilation.
Addresses are RVAs in StarWarsG.exe. Names come from `docs/re/ghidra/functions.csv` where they exist, otherwise
`FUN_<rva>`. Object, type and model offsets are hex byte offsets.

Confidence tags: **[H]** read in the code and cross-checked (data or a second site); **[M]** read in the code,
intent inferred; **[L]** inferred, needs a capture or more RE.

Companion documents: `docs/formats/SPEC.md` §3 (particle chunks) and §4 (ALA decoding), RENDER.md §1.3/§4,
SPACE_AND_OPEN.md §B.9–B.10 and §D.7, OBJECT_RUNTIME.md §5.3, MAIN_LOOP.md §1.4 and §4.1,
TACTICAL_COMBAT.md (synced particle picks), MOVEMENT.md (Walk_Transition).

Asset numbers come from the shipped archives (EaW `GameData\Data` and FoC `corruption\Data`: models.meg + patch,
patch2, 64Patch, later archives override earlier ones). They were read with `tools/meg/megtool.py` and
`tools/formats/python/alo.py`. The survey scripts were throwaway and are not checked in.

---

## 0. Summary for the implementer

1. **Animation state is simulation state.** The clip, variant, frame, mode and crossfade of every object's
   model are advanced in the object's service step at sim time = frame/30. Behaviours read that state: idle
   timing, the weapon gate, death removal, move-clip phase, and `Walk_Transition` acceleration. Gameplay also
   reads animated bone transforms (projectile spawn points, Lua `Get_Bone_Position`, hardpoint death effects). So
   the **animation controller and the pose of every queried bone must be deterministic and live in the sim**. Only
   the full skinning palette is render-only. [H] (0x3a6b80, 0x265280, 0x12d2c0 callers)
2. **Variant choice and random start frames use the synchronized RNG** (0x140a13e24) for objects in a
   primary (sim) store. Objects in the secondary (local) store use the free RNG (0x140a13e20). [H] (0x3a92f0,
   0x3a4820, 0x62e570, 0x61f840)
3. **Fire timing is not tied to animation events.** The weapon creates the projectile first, then starts the
   `Attack` clip in the same tick. There are no frame-event tracks in ALA. [H] (0x54d090)
4. **Loops wrap at frame count − 1. One-shots stop at frame count − 2.** The last stored frame of a looping clip
   is never sampled (it is treated as frame 0). Planet idle clips (240 frames at 1 fps) therefore turn once per
   **239 s**, not 240 s. [H] (0x2650f0, 0x19f3e0; data)
5. **Crossfade = one previous clip blended into the current one.** Each bone uses nlerp of rotations
   (shortest arc, fast inverse square root) and lerp of translations. Weight = 1 − remaining/duration. A third
   **overlay** clip can replace non-excluded bones. [H] (0x265560, 0x146d50, 0x235750, 0x1467b0)
6. **Particles are purely visual.** They use the **free RNG** (plus CRT `rand()` for one shuffle). They advance
   in the scene `Update` with the render delta **d = 0.03 s per sim tick** (0 on frames without a tick). They draw
   in bucket 8 (Transparent), bucket 7 (Water) when the system is flagged, or bucket 11 (Heat) for heat emitters.
   [H] (0x164690, 0x199ea0, 0x172290; MAIN_LOOP R1)
7. **Particle *objects* are a separate, sim-side thing.** GameObjects of category PARTICLE (death explosions, hit
   effects) are created by the sim with synced picks and removed after `Particle_Lifetime_Frames`. Their model is
   a particle ALO, which is visual only. [H] (0x635cb0, TACTICAL_COMBAT §0)

---

## A. Animation

### A.1 Animation types (`ModelAnimType`, table built by 0x279010)

119 ids (0..118). Each id has an **XML/Lua name**, registered in 0x279010 and used by
`Specific_Death_Anim_Type`, `Attack_Animation`, Lua `Play_Animation`, and so on. Vanilla XML writes `DIE`,
`FL_DIE` and `Crushed` for the names `Die`, `FL_Die` and `Crushed`, so the lookup ignores case [M]. Each id also
has a **file suffix**, from a parallel string table at 0x140a15d80 built by 0x15a80, one 0x20-byte string per id.
[H] for both tables. Ids 0 and 2 are stored as inline literals that the decompiler does not show; `IDLE` and
`MOVE` are confirmed by 539 and 158 shipped files. Ids 90 and 91 have a suffix but no XML name.

| id | XML name | suffix | id | XML name | suffix |
|---:|---|---|---:|---|---|
| 0 | Idle | IDLE | 60 | Deployed_Die | DEPLOYED_DIE |
| 1 | Space Idle | SPACE_IDLE | 61 | FIRE_MOVE | FIRE_MOVE |
| 2 | Move | MOVE | 62 | Fire_Die | FIRE_DIE |
| 3 | Turn Left | TURNL | 63 | Pound_Attack | POUND_ATTACK |
| 4 | Turn Right | TURNR | 64 | Eat_Attack | EAT_ATTACK |
| 5 | Attack | ATTACK | 65 | Eaten_Die | EATEN_DIE |
| 6 | Attack Idle | ATTACKIDLE | 66 | Move by walking | WALKMOVE |
| 7 | Die | DIE | 67 | Move while crouched | CROUCHMOVE |
| 8 | Rotate | ROTATE | 68 | Structure Open | OPEN |
| 9–11 | Special A/B/C | SPECIAL_A/B/C | 69 | Structure Hold | HOLD |
| 12 | Transition To Left Turn | TURNL_BEGIN | 70 | Structure Close | CLOSE |
| 13 | Transition From Left Turn | TURNL_END | 71 | Idle Crouch | CROUCHIDLE |
| 14 | Transition To Right Turn | TURNR_BEGIN | 72 | Turn Left Crouch | CROUCHTURNL |
| 15 | Transition From Right Turn | TURNR_END | 73 | Turn Right Crouch | CROUCHTURNR |
| 16 | Transition To Move | MOVESTART | 74 | Build | BUILD |
| 17 | Transition From Move 0 | MOVE_ENDONE | 75 | Transition_Ownership | TRANS |
| 18 | Transition From Move 40 | MOVE_ENDTWO | 76 | Self_Destruct | SELF_DESTRUCT |
| 19 | Transition From Move 80 | MOVE_ENDTHREE | 77 | Attention | ATTENTION |
| 20 | Transition From Move 120 | MOVE_ENDFOUR | 78 | Celebrate | CELEBRATE |
| 21 | Turn Left Half | TURNL_HALF | 79–82 | Flinch_Left/Right/Front/Back | FLINCHL/R/F/B |
| 22 | Turn Left Quarter | TURNL_QUARTER | 83–86 | Attack_Flinch_Left/Right/Front/Back | ATTACKFLINCHL/R/F/B |
| 23 | Turn Right Half | TURNR_HALF | 87 | Talk | TALK |
| 24 | Turn Right Quarter | TURNR_QUARTER | 88 | Talk_Gesture | TALKGESTURE |
| 25 | Deploy | DEPLOY | 89 | Talk_Question | TALKQUESTION |
| 26 | Undeploy | UNDEPLOY | 90 | — | HACKING |
| 27 | Cinematic | CINEMATIC | 91 | — | REPAIRING |
| 28 | Block_Blaster | BLOCK_BLASTER | 92 | Choke | CHOKE |
| 29 | Redirect_Blaster | REDIRECT_BLASTER | 93 | Choke_Die | CHOKEDEATH |
| 30 | Idle_Blockblaster | IDLE_BLOCKBLASTER | 94 | Deploy Troopers | TROOPDROP |
| 31, 32 | FW_Attack, FW_Die | FW_ATTACK, FW_DIE | 95 | Rope Slide | ROPESLIDE |
| 33–36 | FTK_Attack/Hold/Release/Die | FTK_… | 96 | Rope Land | ROPELAND |
| 37–39 | FB_Attack/Hold/Release | FB_… | 97 | Drop Rope | ROPE_DROP |
| 40, 41 | FL_Attack, FL_Die | FL_… | 98 | Lift Rope | ROPE_LIFT |
| 42 | Force_Run | FORCE_RUN | 99 | Alarm | ALARM |
| 43 | Land | LAND | 100 | Warning | WARNING |
| 44 | Takeoff | TAKEOFF | 101 | Crushed | CRUSHED |
| 45 | Flame_Attack | FLAME_ATTACK | 102 | Power_Down | POWERDOWN |
| 46 | Demolition | DEMOLITION | 103 | Power_Up | POWERUP |
| 47 | Bombtoss | BOMBTOSS | 104 | SpinMove | SPINMOVE |
| 48 | Jump | JUMP | 105–107 | Force_Revel_Begin/Loop/End (sic) | FORCE_REVEAL_BEGIN/LOOP/END |
| 49 | FlyIdle | FLYIDLE | 108 | Saber_Throw | SWORD_THROW |
| 50 | FlyLand | FLYLAND | 109 | Saber_Control | SWORD_CONTROL |
| 51 | FlyLandIdle | FLYLANDIDLE | 110 | Saber_Catch | SWORD_CATCH |
| 52 | FlyLandDrop | FLYLANDDROP | 111 | SwordSpin | SWORDSPIN |
| 53–55 | HC_WIN / HC_LOSE / HC_DRAW | same | 112 | Contaminate_Attack | CONTAMINATE_ATTACK |
| 56, 57 | SHIELD_ON / SHIELD_OFF | same | 113 | Contaminate_Loop | **CONTAIMINATE_LOOP** (sic; the shipped file uses the same spelling) |
| 58 | CA_Die | CA_DIE | 114 | Contaminate_Release | CONTAMINATE_RELEASE |
| 59 | Deployed_CA_Die | DEPLOYED_CA_DIE | 115 | Deployed_Walk | WALK |
| | | | 116, 117 | Pad_Build, Pad_Sell | PAD_BUILD, PAD_SELL |
| | | | 118 | Heal | HEAL |

**Type groups the behaviours test** (switch predicates, [H] for the sets, [M] for the names):

| predicate | ids | used by |
|---|---|---|
| attack-like (0x27b4f0) | 5, 25, 26, 31, 33, 34, 35, 37, 38, 39, 40, 45, 46, 47, 63, 64 | idle, weapon, infantry locomotor |
| idle-like / interruptible (0x27b5e0) | 0, 1, 71, 77, 78, 79–82 | 13 callers (locomotors, idle, explosion) |
| turn (0x27b670) | 3, 4, 21–24, 72, 73 | infantry locomotor |
| crouch (0x27b570) | 67, 71, 72, 73 | idle (crouch-state change forces a new idle) |
| flinch (0x27b590) | 79–86 | weapon (no firing while flinching), targeting |
| one-shot-pose (0x278fa0) | 7, 9, 10, 11, 32, 36, 41, 58, 62 | 0x266340: while one of these plays, the shield/effect anchor follows the animated bone [M] |

### A.2 Clip discovery (`FUN_14025ec10`, at model load)

- The **base name** is the model file name without its extension. It is replaced by `Land_Model_Anim_Override_Name` /
  `Space_Model_Anim_Override_Name` when that is given (second argument). For each id 0..118 and for
  NN = 00, 01, 02, … the loader looks up `<dir>\<base>_<SUFFIX>_<NN>.ALA`, first in the asset cache and then on
  disk. It **stops at the first missing index**, so variants must be contiguous from `_00`. Found clips are
  registered under (id, NN) in the model's animation list (ModelClass +0xb0; 119 vectors of 0x18 bytes, plus a
  parallel vector of name CRCs at +0xb38). [H]
- Files without an index (`UI_IG88_CONTAMINATE.ALA`, 182 files in total such as `*_TRANSITION_00`, `*_DEATH_00`,
  `*_FLY_00`) are never loaded by this path. [H] for the rule, data for the count
- **Skeleton rule:** an ALA binds to a model only when the model's bone count = the ALA bone count + 1 (the
  ALA has no root bone). Otherwise the pose falls back to the bind pose: the clip is dropped in 0x12d020 /
  0x12d0f0 / 0x12d220. In vanilla data 2006 ALAs match their model and **27 do not** (for example
  `EB_COMMANDCENTER_IDLE_00` with 55 vs 59 bones, and the Dark Trooper `*FLINCHF_01` clips). Those clips play
  as bind pose in the original. [H]
- Shipped corpus (EaW + FoC): 2408 ALA files; 2226 follow the naming rule. By type: IDLE 539, DIE 301, ATTACK
  160, MOVE 158, ATTACKIDLE 108, CINEMATIC 77, BUILD 64, LAND 55, WALKMOVE 53, TALK 43, the rest fewer. Index
  counts: `_00` 1566, `_01` 383, `_02` 183, `_03` 60, then up to `_08`. Frame rates: 30 fps 2123, 1 fps 71
  (planets and props), 5 fps 16, 10 fps 8, 15 fps 8.

### A.3 Per-model playback state (`ModelClass`, ctor 0x263a20)

Three **slots** of 0x30 bytes. A slot is empty when its clip pointer is null; slots are initialised by 0x278e50 and
cleared by 0x278e80. [H]

| slot | offset | role |
|---|---|---|
| primary | +0xb8 | the current clip |
| previous | +0xe8 | the clip being faded out. Its timer fields hold the crossfade (+0x108 active, +0x10c duration, +0x110 remaining, +0x114 last weight) |
| overlay | +0x118 | `Attack_Animation_Is_Overlay` clips and similar; drives only non-excluded bones (A.6) |

| slot field | meaning |
|---|---|
| +0x00 | clip (refcounted) |
| +0x08 | clip name CRC |
| +0x0c | type id (−1 = none) |
| +0x10 | **mode**: 0 = one-shot, 1 = loop, 2 = finished/stopped |
| +0x14 | variant index |
| +0x18 | frame position (float) |
| +0x1c | speed multiplier |
| +0x20 / +0x28 | "timed" flag / seconds remaining. When the time runs out the slot becomes finished and speed is reset to 1 |

ModelClass +0x148 holds the sim time of the last advance; it is set by every play call. [H]

### A.4 Starting a clip

**`GameObjectClass::Play_Animation` 0x3a92f0** takes (object, type, index, loop, blend seconds, speed,
start frame, blend mode, lock, flag). Every behaviour, ability and Lua call goes through it (103 call sites). [H]

1. Special case: while a Berserker ability is active, any request other than `Attack`, `Attack Idle` or `Idle`
   becomes `SpinMove` (104), looped, at speed 1. The spin is left alone until it is within 2 frames of its end. [M]
2. **Lock:** object +0x3a0 bit 3 is set when a call passes lock = 1. While it is set, calls without lock are
   ignored. Every accepted call rewrites the bit. [H]
3. **Index 0xFFFF means a random variant:** synchronized-RNG integer in [0, count − 1] (0x1ffb40 on
   0x140a13e24), or 0 when the type has no clip. [H]
4. `ModelClass` play (0x265560): it fails (returns 0) when the index is out of range. Type −1 clears the primary
   and previous slots. If blend > 0 a crossfade starts, unless one is already running with weight ≤ 0.25. In that
   case the old "previous" clip is kept and only the primary is replaced. [H]
   - blend mode 0: previous ← current primary; duration = remaining = blend;
   - modes 2 and 3 rescale the remaining time when a fade is already running: remaining = (remaining/duration)·blend,
     or (1 − remaining/duration)·blend. These are used by the flinch and idle code. [H for the arithmetic, M for intent]
   - blend 0 clears the previous slot (hard cut).

   The new primary starts at frame 0, with mode = loop flag, the given speed, and the timed flag cleared. [H]
5. If start frame > 0, frame = start frame (0x265490). It is forced to 0 when the clip has fewer than 2 frames. [H]

Lua `GameObjectWrapper.Play_Animation(name, loop[, index])` (0x57e6c0) calls it with blend **0.33 s** and speed 1.
[H] for the constants, [M] for the default index.

### A.5 Advancing (`FUN_140265280` → `FUN_1402650f0`, once per sim tick)

- **When:** object Service (0x3a6b80, OBJECT_RUNTIME §5.3 step 6) calls it with **t = global frame / 30**
  (sim seconds), unless object +0x3a7 is set [L: frozen object]. Non-object scene models registered for animation
  are advanced in tick step T6 (0x2718e0) with the same t, or with wall time when the wall-time-animation flag is
  set (MAIN_LOOP R0). [H]
- **Per slot:** Δ = t − (model +0x148); frame += clip fps × Δ × slot speed. [H]
  - **loop:** while frame ≥ (frame count − 1), subtract (frame count − 1);
  - **one-shot:** when frame ≥ frame count − 2, frame = max(frame count − 2, 0) and mode = finished;
  - **finished:** the frame does not move;
  - **timed slot:** remaining −= Δ; at ≤ 0 the slot becomes finished and its speed resets to 1.
- Crossfade: the previous slot advances too. When its speed is 0 it inherits the primary's speed (or 1). Pose
  weight = 1 − remaining/duration, re-evaluated every advance. At remaining ≤ 0 the fade flag clears and the
  next pose uses the primary alone. [H]
- The model is marked dirty and handed to the pose request list (0x4775d0 on 0x140a2a120) only when the primary
  frame actually changed. Object flag +0x74 bit 3 clears once frame ≥ count − 2.5 ("near the end"). [H]
- There is no render-side interpolation (RENDER.md §4). A port that renders above 30 Hz may interpolate visually
  but must keep these sim values. [M]

### A.6 Pose evaluation (`alHModel::vf15` 0x12c7c0, lazy)

- **Modes:** 0 bind pose, 1 one clip, 2 crossfade. It runs only when the model is dirty (+0x30 bit 0x20). After
  the bone matrices it pushes each bone's world transform to the attached sub-objects (meshes, proxies, attached
  models) and sets each sub-object's "animated visible" bit from the clip's per-bone visibility. In a crossfade a
  bone is visible if either clip says visible. [H]
- **Sampling** (alQuantizedAnim vf13 0x1a02d0): a = floor(frame) and b = a + 1, both wrapped into
  [0, count − 1); t = frac(frame). Rotation = nlerp of the two dequantised quaternions (i16/32767); translation =
  lerp. A clip with fewer than 2 frames always samples frame 0. [H] for the formula; [M] for the exact wrap
  arguments (the decompiled argument list is garbled)
- **Crossfade** (0x146d50): per bone, sample both clips, lerp the translations and nlerp the rotations with the
  weight. [H]
- **nlerp** (0x235750): negate the second quaternion when the dot product < 0, lerp component-wise, then normalise
  with the "fast inverse square root" (bit trick, magic 0x5F375A86, two Newton steps). Bit-exact parity needs this
  exact approximation, not 1/sqrt. [H]
- **Overlay** (0x1467b0 path): for each bone whose ALA `overlay_excluded` (SPEC §4.2 mini 0A, vf15 0x1a1ca0) is
  0, the overlay clip's local transform at the overlay frame replaces the base result, and its visibility bit is
  used. [H]
- Bone composition: local (rotation, translation) × parent world, in bone order. Bones whose skeleton flags
  (+0x10 & 0x78) are 0x30 or 0x38 copy their parent instead. Billboard modes are applied later (SPEC §2.2). [M]
- **Gameplay bone reads** (0x12d2c0 → vtable +0x78 = this function) force an evaluation first. The 63 callers
  include the projectile spawn (0x54ade0, 0x54a270), Lua `Get_Bone_Position`, hardpoint death and attachment
  (0x386660, 0x3989a0), squadron spawn, deploy troopers, abilities and the ProjectileBehavior. [H]

### A.7 Which clip for which state

| situation | where | type / index | loop | blend | speed | notes |
|---|---|---|---|---|---|---|
| **model created** | 0x3a4820 (object), 0x3be4b0 (copy) | default idle (below), index 0 | `Loop_Idle_Anim_00` (type +0x30) | 0 | `Idle_Anim_00_Rate_Mod` (type +0x1640, default 1.0) | then **random start frame** in [0, count−1]: synced RNG when the object's store has GOM +0x63E set (primary store), free RNG otherwise; 0x3be4b0 always uses the free RNG. No draw when the type has no clip [H] |
| default idle type | 0x395920 | 71 Idle Crouch while unit ability 7 SPREAD_OUT is active, else the **presence-induced** idle (77 Attention / 78 Celebrate in vanilla) while a listed presence unit is near, else 0. There is no garrison/container mapping: see A.11 | | | | [L] |
| **idle behaviour** | 0x62e570 | idle type | **0 (one-shot)** | 0 if the type is unchanged, else global 0x140b39930 | 1 | runs when the current clip is finished (mode 2), absent, or the crouch state changed. When the idle timer (frame ≥ +8) expired: variant = synced int in [1, count−1] (0 when only one clip) and the next timer = now + synced int in [210, 450] frames (7–15 s); otherwise variant 0. So `_IDLE_00` is the base loop, re-triggered as a one-shot, and `_01+` are fidgets [H] |
| idle while engaged | 0x62e570 | 6 Attack Idle, index 0 | 0 | 0, or synced float [0.2, 0.75] when coming back from idle | 1 | entered when the current clip is Attack/Attack Idle/attack-like and finished. Combat timer = synced int [0, 30] + 90 frames [H] |
| **flinch** | 0x62dde0 (direct projectile hit), 0x62e040 (EXPLOSION objects) | 79–82 while idle-like, 83–86 while attack-like; **never in Attack Idle (6)**, which is in neither set; chosen by the angle between the facing and the hit direction: < 45° back, 45–135° left/right by sign, > 135° front | 1 | `Max_Flinch_Blend_Seconds` (+0x20e8, default 0.25) | 1 | random variant (synced). Hold `Max_Flinch_Hold_Seconds` (+0x20ec, 0.5), then a cooldown of `Idle_/Attack_Flinch_Interval_Seconds` (+0x20f4 1.0 / +0x20f0 3.0). Triggers and gates: A.12 [H] for the flow, [M] for the direction naming |
| **walk / move** | 0x61f840 (WalkLocomotor) | 2 Move; 67 when crouched; 61 FIRE_MOVE when object +0x3a1 & 2 is set (burning [M]); 115 Deployed_Walk (speed `Deployed_Walk_Anim_Speed`); 66 WALKMOVE when the type has it and speed is below a threshold (global 0x140b16d04 × max speed) [M] | 1 | global 0x140b16d18 | `Movement_Animation_Speed` (+0x370) × max(current speed / max speed, minimum) | Move↔Walk switch keeps the phase: new frame = old frame × new count / old count. Otherwise the start frame is synced random [0, count−1]. The clip is restarted only when the type changes. The speed is refreshed every tick (0x3a7b70) [H] |
| turn | 0x620190 | 3 / 4 by sign | 0 | computed | 1 | infantry turn time = angle / rate / 30 (MOVEMENT.md) [M] |
| deploy / undeploy | 0x61e930 | 25 / 26, random variant | 0 | 0.0333 | `Deployment_Anim_Rate` (+0x1668) | [H] |
| **attack** | 0x54d090 (WeaponBehavior) | 5, random variant | 0 | 0.4 (0x140a43a8c), or 0.1 when coming from a finished attack/Attack Idle | 1 | played **after** the projectile is created in the same tick. With `Attack_Animation_Is_Overlay` (+0x5c) the clip goes to the overlay slot instead (0x3a8ed0, timed 3.0 [M]). Muzzle sub-objects (per fire bone) are shown and re-hidden after 4 ticks [M] |
| **death** | 0x58cdb0 (DeathBehavior) | `Specific_Death_Anim_Type` (+0xd98, default **7 Die**), `Specific_Death_Anim_Index` (+0xd9c, default 0xFFFF = synced random) | 0 | 0.5 | 1 | when the clip is missing and `Remove_Upon_Death` (+0x9b) is set, the object is removed at once [H] |
| corpse removal | 0x58d430 | — | | | | the `Death_Persistence_Duration` (+0x2290, default −1 = forever) countdown starts only when the current clip is no longer the death type **or has finished**, then `Death_Fade_Time` (+0x2294, 0.25). **The length of the death clip is therefore sim-visible** [H] |
| Lua / cinematic | 0x57e6c0, 0x38de00 | given | given | 0.33 / `Cinematic_Anim_Blend_Seconds` | 1 / `Cinematic_Anim_Speed` | [H]/[M] |

The globals 0x140b39930, 0x140b16d18, 0x140b39900–0x140b39908 and 0x140b3993c are in .bss. No writer was found
in the decompilation (it is probably a data-driven table). A port should treat them as tunables that default to 0
(hard cut) until a capture settles them. [L]

### A.8 Sim-relevant vs visual

| item | sim-relevant? | why |
|---|---|---|
| current type, index, mode, frame, crossfade flag | **yes** | read by idle, flinch, weapon gate (0x54d090 refuses to fire during flinch and some attack/crouch states), infantry locomotor, abilities (30 callers of the mode getter 0x395660) [H] |
| clip frame counts and fps | **yes** | Walk_Transition acceleration (MOVEMENT.md), death timing, phase-preserving switches [H] |
| random variants and start frames | **yes** | synced RNG draws (primary-store objects) [H] |
| bone world transforms of queried bones | **yes** | projectile origins, death-effect positions, Lua [H] |
| skinning palette, per-bone visibility of meshes | no (render) | — |
| ALA step bits (`1008`) | no | never read (SPEC §4.3) |

**Animation events:** none. ALA has no event chunk, and no code path compares the frame against per-clip
markers. Firing is driven by the weapon timers. [H]

**RNG streams:** synchronized LCG for every behaviour-driven choice above. Free LCG only for secondary-store
objects and the model copy at 0x3be4b0. [H] Draw order follows the service order (OBJECT_RUNTIME §6). Model
creation draws its start frame when the model is created (object construction / hardpoint creation), so a
headless sim must still create the "model" state and make that draw. [M]

### A.9 Planet idle wrap (239 vs 240) — settled [H]

`w_planet_alderaan_high`, `_kuat_high` and `_moon_high` `_IDLE_00.ALA` have 240 frames at 1.0 fps. Bone `Hoth`
reads 0° at frame 0, 358.49° at frame 238 and 360° (= 0°) at frame 239: **frame 239 duplicates frame 0**, with
1.506°/frame. The engine wraps loops at count − 1 = 239 and never samples frame 239 (it maps to 0), so the
rotation is seamless and takes **239 s per revolution** at rate 1.0 (`Coruscant_Backdrop` at 0.25: 956 s).
**Correction to SPACE_AND_OPEN.md §D.7 item 4 (240 s → 239 s).** Kuat's `building_ring` was authored without
the duplicate frame (−1.5°/frame, frame 239 = −358.5°). In the original it therefore jumps 3° during the last
second of each cycle (frame 238 → 0); a faithful port reproduces that small hitch. The start phase is random
(A.7, model created). Background planets are primary-store objects, so their start phase is drawn from the
synced RNG [M].

**Capture check (g1f t07, 2026-10-31): confirmed.** `apitrace_20261031_175322` (Kuat space, planet draw `m_world` against `m_time`): 0.050207° per tick (360/239/30 = 0.050209), 239.0 s per revolution, the pose returns to the bind pose after 3 390 ticks and passes it without a pause (a 240-frame wrap would hold 30 ticks). See RENDER.md §7.3.

### A.10 Port notes for R3

- Sim side (deterministic, `/fp:precise`, no FMA): ALA metadata (frame count, fps, bone count, per-type
  variant lists), the three-slot state, the advance rules of A.5, all play calls of A.7 in service order, and the
  pose of queried bones (A.6 math, including the fast inverse square root).
- Render side: evaluate the full pose from the sim state (type, index, frame, previous clip and frame, weight,
  overlay) and upload the palette (RENDER.md §4). Visual interpolation between ticks is an enhancement.
- Drop clips whose bone count does not match (bind pose). Stop variant scanning at the first gap.

### A.11 Default idle type (0x395920): crouch and presence-induced idle, not a garrison table (R3 t02)

**There is no garrison/container mapping.** 0x395920 and its callees read no container, garrison slot or
container kind. The "container" is the IDLE behaviour's presence map, filled by nearby IMPOSING_PRESENCE units.
Agrees with TACTICAL_GAPS §6.2, with one correction (the movement/target gate, below). [L]

**Result, in order** [L]:
1. `Is_Unit_Ability_Active(obj, 7 SPREAD_OUT, 1)` (0x39b950, ABILITIES_HARDPOINTS §2.7) → **71 Idle Crouch**. "Crouched" is this
   ability, not a behaviour. 0x3a3500 (the SPREAD_OUT toggle) immediately replays the default idle (blend 0.4,
   loop = `Loop_Idle_Anim_00`).
2. Else, when the object has IDLE data (obj +0xd8) and its presence map is not empty: pick a winner (below) and return
   the anim type of the **first** `Presence_Induced_Animations` entry of the object's own type whose type list
   contains the winner's type (0x373530). If no entry matches, it returns −1. That cannot happen in practice,
   because entries are only created for listed types [M].
3. Else **0 Idle**. (In this function 0 is the default for every object. Space types get their idle elsewhere.)

**IDLE data (obj +0xd8), fields used here** [L]: +0x08 next fidget frame; +0x0c combat timer; **+0x10 presence
map**, a hash map presence-object id → remaining frames (node list head +0x18, size +0x20, buckets +0x28, mask
+0x40; node: next +0, key +0x10, value +0x14; hash = Park–Miller of (id XOR 0xDEADBEEF) & 0x7FFFFFFF, the same as the
GOM id map); +0x50 flinch timer, +0x58 flinch intensity, +0x5c pre-flinch anim type (A.12).

**Winner selection** (one pass over the map's node list, in list order [M: list order = insertion order, not
verified]) [L]:
- Resolve each key with the GOM id lookup (0x2aca60 on obj +0x2b8). Ids that are no longer live are skipped, but
  their entries stay until they time out.
- A candidate replaces the best if there is no best yet or its remaining frames are **strictly greater**.
- Otherwise (remaining ≤ best) it still replaces the best when the candidate's owner is allied with the object's owner
  (relation == 0, 0x2823e0; self counts as allied) **and** the current best's owner is not. The best's remaining value
  then drops to the candidate's, so a later non-allied entry with more frames can take over again. The result
  depends on list order. Owners: the object's own owner id is obj +0x58; a candidate uses +0x58, or the planet
  owner when it is a planet (+0x335 ≠ −1).
- No owner filter is applied anywhere else: enemy presence units induce the idle too.

**Writer: IMPOSING_PRESENCE service** (0x624ab0, interval 30, so it runs once a second; first due frame drawn from the synced RNG at attach,
TACTICAL_GAPS §6.4) [L]:
1. GOM-wide box query around the presence unit: centre = its position, half-extent = its `Presence_Range`
   (type +0x1628, default 0) on all three axes. The box is a cube, not a circle.
2. Each result that is not dying/removed (+0x3a0 & 0x42 clear), whose type lists the presence unit's type in any
   `Presence_Induced_Animations` entry (0x370630, type-pointer equality), and that has IDLE (+0x339 ≠ −1) goes
   to 0x62e450.
3. 0x62e450 tests the **affected unit**, not the presence unit (TACTICAL_GAPS §6.2 says "presence unit": wrong).
   The unit gets no entry when its TARGETING behaviour (9) has a target (vf +0xe0). A unit without its own TARGETING uses its parent's
   (obj +0x2b0) when its locomotor's vf +0x228 is true. The unit also gets no entry when it has a locomotor, its type has
   `Allow_Idle_When_Moving` (+0x85) false and the locomotor reports moving (vf +0x110).
4. Otherwise insert or refresh: map[presence id] = int(FPS × `Unit_Presence_Relaxation_Time`) (GameConstants
   +0x117a8, default **5.0** s and not set in vanilla XML, so **150 frames**).

**Maintenance in the IDLE service** (0x62e570, interval 1) [L]:
- When the unit has a locomotor, `Allow_Idle_When_Moving` is false and it is moving, the map is **cleared**
  (along with the flinch hold) and the service returns. Moving cancels Attention at once.
- When its own TARGETING (or its parent's, as above) has a target, the map is cleared and the combat timer is set
  (synced [0, FPS] + 90). This is the Attack Idle path of A.7.
- Otherwise every entry is decremented by 1 each frame and removed at < 1. A presence unit therefore keeps its
  entries between 120 and 150 frames old while it stays in range. They expire **150 frames** after it leaves,
  dies or starts moving out of range. While several presence units are in range, the most recently refreshed
  one usually wins.
- Early return **before** the decrement for `Deploys` types (type +0xd1) while deployed or (un)deploying (0x39a400,
  0x39b2a0, 0x39a390). Their entries are frozen, not aged. No vanilla list holder deploys [M].

**Crouch, timer and fidgets** (0x62e570, see the A.7 idle row) [L]:
- The new type T = 0x395920 is compared with the current primary type C. If T ≠ C **and** their crouch class
  (0x27b570: 67, 71, 72, 73) differs, the idle restarts at once, even mid-clip. Otherwise (for example 0 → 77,
  77 → 0, 71 ↔ 71) it waits until the current clip has finished (mode 2) or is absent. Leaving SPREAD_OUT
  is therefore immediate, and Attention starts only at the end of the current idle clip.
- The fidget timer applies to T unchanged. When the timer has expired, a synced variant in [1, count − 1] is drawn
  (0 when T has a single clip, as `ATTENTION`/`CELEBRATE` do in vanilla) and the timer is re-armed with
  synced [210, 450] frames. Otherwise variant 0 is used. Blend: 0 when T = C, else global 0x140b39930.
- No clip for T: the play fails (A.4 step 4) and is retried every frame, so the unit **holds the last frame of
  its finished idle** until the entries expire [M, derived from the play rules, not observed].

**Other callers** (29 call sites) [L]: model creation 0x3a4820 (A.7 row 1), the SPREAD_OUT toggle 0x3a3500,
the infantry locomotor stop and turn paths (0x61f510, 0x61f840, 0x61fc30, 0x6204f0, 0x62bef0, 0x62c420,
0x62ca10), 0x42c920, 0x63a8e0, 0x641910 / 0x641df0 / 0x642740, and the ability end paths (0x6eeff0, 0x6f80d0,
0x6f8260, 0x6fd880, 0x7003d0, 0x701c10, 0x70cb90 repair, 0x71ad50, 0x71ec30). Each one replays the default idle with
loop = `Loop_Idle_Anim_00`. A port must call the same function from all of them.

**XML fields** [L for the parser and offsets]:

| field | where | parse / default |
|---|---|---|
| `Presence_Induced_Animations` | affected type +0x1610 (vector of 0x28-byte entries: anim type id +0, type-ref list +8 with array +0x10 and count +0x18, 16-byte elements with the type pointer at +8) | code 0x35: comma tokens. A token that converts as an anim type name starts a new entry, and any other token is a type name appended to the last entry's list. Several entries per tag and several tags are allowed. A variant re-parse clears the vector once per tag. Default empty |
| `Presence_Range` | presence type +0x1628 | float, default 0 |
| `Allow_Idle_When_Moving` | affected type +0x85 | bool, default false [M] |
| `Unit_Presence_Relaxation_Time` | GameConstants +0x117a8 | float s, default 5.0 |
| `IMPOSING_PRESENCE` / `IDLE` | behaviour lists | the presence unit needs IMPOSING_PRESENCE (0x40); the affected unit needs IDLE (7) |

**Vanilla data** (FoC and EaW `GROUNDINFANTRY.XML` identical; data):

| affected type (and its `Variant_Of_Existing_Type` children, which inherit the list) | idle | presence types | clip |
|---|---|---|---|
| `Stormtrooper`, `Scout_Trooper_No_Bike` (anim override EI_TROOPER) | 77 Attention | `Emperor_Palpatine`, `Darth_Vader` (`Presence_Range` 100) | `EI_TROOPER_ATTENTION_00` |
| `Imperial_Guard_Non_AI` (EI_RoyalGaurd) | 77 Attention | same | **none**, so the unit holds a frozen pose (above) [M] |
| `Rebel_Trooper`, `Plex_Soldier` | 78 Celebrate | `AT_AT_Walker_Cable_Attack_Death_Clone`, `AT_AT_Walker_Death_Clone_00/01/02` (range 200; LandBehavior HIDE_WHEN_FOGGED, IMPOSING_PRESENCE) | `RI_INFANTRY_CELEBRATE_00`, `RI_PLEXSOLDIER_CELEBRATE_00` |

- Matching compares type pointers. These presence units match no list, so they induce nothing:
  `Darth_Vader_Expansion` (FoC, its own type, not a variant), `Darth_Vader_Bounty_Hunter_Immune` (a variant of
  `Darth_Vader`) and `UM05_ADEPTGENERIC` with its variants (range 0). [L + data]
- Because no owner filter exists, Rebel and Plex troopers celebrate next to any dying AT-AT clone, including one
  from a captured AT-AT (`AT_AT_Walker_Captured` is a variant of `AT_AT_Walker`) [M].
- **Port:** `SimAnimation::set_idle_type_hook` should implement steps 1–3 above. It should not look up a
  garrison. "Not garrisoned" has no meaning here. The hook's `kNoAnimType` corresponds only to the
  unreachable "no entry matched" case. Contained objects (obj +0x3a7) are frozen for animation (A.5). Whether
  they still collect presence entries was not traced [open].

### A.12 Which damage makes a unit flinch (0x62dde0, 0x62e040)

Only two code paths flinch a unit. Each has exactly one caller (no vtable references) [L]:

| source | path | victims | direction (D) | intensity → flinch +0x58 |
|---|---|---|---|---|
| **projectile hits an object** (sweep hit) | `Projectile::Step` 0x4d0080 or grenade flight 0x4ceea0 → `Process_Hit` 0x4cb280 → `Apply_Direct_Damage` 0x4c73d0 → 0x62dde0 | the object hit, if it has IDLE (+0x339 ≠ −1) | the projectile's last-move velocity (projectile data +0x98..+0xa0, S9_DEATH_CLONE_SHIELDS) | the projectile type's `Induced_Flinch_Intensity` (+0x20e4, default 1.0) |
| **EXPLOSION object created** | `ExplosionBehaviorClass` Initialise (vf9 0x622440: push-out 0x621bb0, then 0x62e040) | every object with IDLE in a GOM-wide **cube** of half-extent `Induced_Flinch_Range` (explosion type +0x20e0, default **100**) around the explosion. No owner, dying or self filter | victim position − explosion position (3D) | clamp(I × (1 − 0.5 × d²_xy / R²), **0.25**, 1), with I = the explosion's `Induced_Flinch_Intensity` |

- Vanilla EXPLOSION types (FoC `PARTICLES.XML`, 8): `Medium_/Large_/Huge_Explosion_Land`, `Remote_bomb`,
  `Ewok_Suicide_Bomb_Explosion`, `Veers_Maximum_Firepower_Explosion_Land`, `p_orbital_impact`,
  `p_orbital_impact_ion` (EaW: 4). No vanilla XML sets any `Induced_Flinch_*` or `*_Flinch_*` field, so the
  defaults apply everywhere (data). Initialise does not run on load (TACTICAL_GAPS §6.4).
- **No flinch** from these [L]:
  - area/splash damage 0x4c7f70: it calls 0x4c73d0 with "area" = 1 and no hit position, which skips the whole visual and flinch block;
  - ground or lifetime detonations (`On_Detonate` 0x4cb070, `Move_And_Expire` 0x4c7050, 0x4caea0) and remote-bomb detonation 0x280ec0, which only reach area damage;
  - burning, crush, ability damage (Force lightning and the like), hardpoint damage, death damage 0x58f210: all of them call `Take_Damage` or area damage directly.

  Hardpoints themselves have no IDLE. A projectile hit on a hardpointed unit flinches the unit, if the unit has IDLE and flinch clips.
- **Direct-hit gates** in 0x4c73d0 [L]: flinch only when the hit was **not absorbed by shields**. Shield hits take
  the shield-particle branch instead. With `Projectile_Damage_Delay_Secs` the flinch still happens at impact,
  before the queued damage lands. Invulnerable or zero-damage hits are not excluded [M].
- **Flinch gates** (both paths, the same code) [L]: IDLE data present; flinch timer +0x50 expired **and** +0x58 = 0
  (no flinch in progress); type is not `Deploys` (+0xd1); the current primary type C is attack-like (0x27b4f0) or
  idle-like (0x27b5e0: 0, 1, 71, 77, 78, 79–82). Moving, turning, Attack Idle (6), death, ability and cinematic
  clips never flinch. Flinch set: 83–86 when C is attack-like, else 79–82. Δ = wrap(yaw(D) − facing yaw obj
  +0x8c) into (−180°, 180°]: |Δ| < 45° → 82 / 86, 45°–135° → Δ > 0: 80 / 84, Δ < 0: 79 / 83, > 135° → 81 / 85. The model
  must have a clip of that type (0x38f9e0). Play: index 0xFFFF (synced random variant), loop 1, blend `Max_Flinch_Blend_Seconds`
  of the victim. On success: +0x58 = intensity, +0x5c = C.
- **Flinch progress** in the IDLE service [L flow, M for the weight meaning]: once the blend-in weight (primary
  slot +0x2c) reaches the intensity, or the crossfade has ended, the clip is frozen (speed 1e-4, 0x3a8120). The
  flinch timer is set to `Max_Flinch_Hold_Seconds` × intensity and +0x58 is cleared. When that timer expires, C is replayed (random variant,
  one-shot, blend `Max_Flinch_Blend_Seconds`, blend mode 3). When the model is back on C with the fade complete, the timer gets the cooldown
  (`Attack_Flinch_Interval_Seconds` when C is attack-like or 6, else `Idle_Flinch_Interval_Seconds`) and +0x5c is cleared to −1.
  An intensity below 1 is therefore a partial pose blend: explosion victims flinch weaker with distance, but at least 25 %.
- Vanilla models with flinch clips: EI_TROOPER, EI_DARKTROOPER_ONE, RI_INFANTRY, RI_PLEXSOLDIER, NI_PIRATEPLEX,
  NI_TWILEK_F, NI_TWILEK_F_NOGUN (13–14 files each; asset catalog).

---

## B. Particles

### B.1 Two layers

1. **Particle GameObjects (sim).** Types of category PARTICLE (XML `Particle` types, `Death_Explosion_Particles`,
   `Projectile_*_Particle`, hit-particle lists) are ordinary GameObjects. They are created by the sim (hit picks
   use the synced RNG, TACTICAL_COMBAT §0). `ParticleBehaviorClass` (0x635cb0) stores the death frame = mode frame
   + `Particle_Lifetime_Frames` (type +0x5e8) and kills the object then (0x635d00). Their model is a particle ALO.
   They emit no "created" signal. [H] for the lifetime, [M] for the scheduling
   - Model culling for these objects is local and never sim. With `Allow_Particle_Model_Culling` (type +0x3d) the
     model is not created when the local player's fog of war hides the position. With `No_Particle_Below_Detail_Level`
     (+0x1818) it is not created when the Particle Detail option is at or below that value. [H] (0x3a4820, 0x3751f0)
     Particle ALOs have no ALA clips, so skipping them skips no synced draw. A port must keep the rule "start-frame
     draw only when the start type has ≥ 1 clip" [M].
2. **Render particle systems (visual).** One system renderable per particle ALO instance (`alParticleGroup`:
   vtable 0x819b60, update vf7 0x171120, submit vf8 0x172290), holding one emitter instance per `700` chunk
   (vector +0xd0; instance desc at +0x118). They come from model proxies (B.3), particle GameObjects, Lua
   `Attach_Particle_Effect` and engine effects. [H]

### B.2 Data format additions to SPEC §3 (survey of shipped files)

Shipped particle ALOs (top chunk `900`): **FoC 179 files / 471 emitters, EaW 409 files / 1019 emitters**
(1–15 emitters per file, most often 1). In both corpora every emitter has chunks `02`, `03`, `16`, `29`, `01`;
FoC has `36` and `45` everywhere, EaW in 1001 and 986 emitters. [H]

| item | FoC | EaW | note |
|---|---|---|---|
| blend mode (`04`) | 1: 327, 2: 144 | 1: 588, 2: 410, **11: 21** | mode 11 = PrimParticleBumpAlpha, uses the `45` bump texture |
| use bursts | 233 | 646 | |
| object space | 119 | 168 | |
| weather (`3D`) | 0 | 5 | |
| heat (`3B`) | 47 | 127 | drawn in bucket 11 |
| has tail | 39 | 52 | |
| ground behaviour 1/2 | 5 / 1 | 53 / 10 | |
| spawn source 1/2/3 | 17/17/30 | 48/25/59 | 3 = one particle per parent-mesh vertex |
| inherit velocity | 83 | 326 | |
| wind / wind disturbance | 315 / 5 | 484 / 71 | |
| texture cells | 64: 421, 16: 50 | 64: 819, 16: 182, 4: 18 | |
| lifetime s (min/median/max) | 0.1 / 1.5 / 30 | 0.1 / 1.25 / 60 | |
| particles per second (max) | 2000 | 3500 | |
| per burst (max) / burst count 0 (unlimited) | 800 / 295 | 2000 / 493 | |
| skip time ≠ 0 / stop time ≠ 0 | 61 / 2 | 97 / 20 | |
| leave particles (`900/02`) = 1 | 159 of 179 | 316 of 382 | |

- **Textures:** 7 distinct in FoC, 11 in EaW. Dominant: `P_PARTICLE_MASTER.TGA` (405 / 754), then
  `P_PARTICLE_HEAT_MASTER.TGA`, `P_PARTICLE_EXPLOSION1/2/3`, `P_ASTEROIDS`. 64 EaW emitters store full authoring
  paths (`C:\PROJECTS\…\P_PARTICLE_MASTER.TGA`), so the loader must strip directories. The runtime preloads
  `p_particle_master.tga` and `p_particle_depth_master.tga` as defaults (0x19d200, 0x14fd40). [H] for the data,
  [M] for the default use
- **Links (`36`):** minis `37` / `39` hold a **4-byte emitter index, −1 = none** (not names). They are set in only
  3 (`37`, spawn-on-death) and 24 (`39`, spawn-during-life) emitters across both games. At runtime they become
  desc +0x1108 / +0x1110. A during-life child gets one child-emitter state per parent particle, positioned at the
  parent particle (0x161e60). The child's pool is multiplied by the parent's pool (0x163da0). [H] for the data,
  [M] for the mapping of 37/39
- **Tracks:** 7 per emitter. Interpolation mode (header mini `04`): tracks 0–2 and 6 always 0; alpha (3) and
  scale (4) use 1 in 123 / 130 emitters; track 5 (texture index) is always **2**. Keys: RGB tracks are keyed in 751
  of 1464 emitters, alpha in 467, scale in 307, texture in 91, rotation in 36; the rest are header-only. The
  meanings of modes 0/1/2 (linear / smooth / step) are not traced. [H] for counts, [L] for the meanings

### B.3 Attachment: proxies and bones

- **Proxy loading** (`603` connections, 0x12b920): mini `05` name, `06` bone index, `07` hidden, `08`. When the
  name contains `_ALT`, the digits after it are the proxy's **alt index** (sub-object +0xc4) and mini `08`
  sets sub-object +0x31 bit 0x20. The name is then **cut at `_LOD` and at `_ALT`**, and that truncated name is
  loaded as the model or particle file. The sub-object is attached to the bone; a bone below 0 drops it. When any
  sub-object has an alt index, the model's alt state is initialised to 0 and the alt/LOD filter (RENDER.md §5)
  hides proxies whose alt index differs. [H]
- Data check: of 5,685 proxies in shipped models, 3,345 name a particle file exactly, **2,206 only after stripping
  `_ALTn`/`_LODn`** (e.g. `P_EXPLOSION_MED01_ALT3` → `P_EXPLOSION_MED01`), and 134 resolve to nothing
  (`LENSFLARE0`, `P_DESERT_GROUND_DUST`, `P_SMOKE_SMALL_THIN2`, …). Those load nothing. [H]
- **Transform:** the pose evaluation (A.6) pushes the bone world transform to the system each time the model is
  re-posed. The system computes its velocity as (position − previous) / dt at update and uses it for inherit
  velocity and spawn interpolation (0x171120, B.4). Object-space emitters (`08`) move their particles with the
  emitter. [H]
- **Visibility gates:** the hidden bit (ALO `07`, `Damage_Particles`/`Engine_Particles` rules of SPACE_AND_OPEN
  §B.10, alt/LOD filter) suppresses drawing **but not updating**. Submit also returns early when the parent model
  is hidden or its fade alpha ≤ 0.01. [H] (0x172290)

### B.4 Simulation (`alParticleGroup::vf7` 0x171120, per scene update)

- **Clock:** every scene `Update(d)` (alScene vf7 0x199ea0, called from the mode `Pre_Render`) clamps d to ≤ 1 s,
  advances the scene `TIME` (wraps at 28,800 s) and updates every renderable. **d = 0.03 s per sim tick in that
  iteration, 0 otherwise** (MAIN_LOOP R1). A system with d = 0 does nothing. Particles therefore move at 0.9× real
  time at 30 ticks/s, scale with game speed, and freeze when paused. [H]
- **Throttle:** when global 0x140a12648 is set, a system that was not drawn last frame (+0xc9 clear) accumulates d
  and updates only once ≥ 0.1 s has gathered. No writer of that global was found; **it is statically 1 in the
  exe's initialised data, so the throttle is on** (corrected 2026-10-28, STRATEGIC_GAPS §6.2). [H]
- **Order inside one update:** smooth two scalar parameters (critically damped springs at +0x2d0 / +0x2a8); read
  the parent transform; update weather-style emitters (+0x1107) first; per emitter (0x163670) apply skip/stop
  rules, then spawn + integrate (0x1640d0); rebuild the system bounds from the emitter AABBs; remove finished
  emitters; the system kills itself when every emitter is done and it is not set to linger. [H]
- **Emitter time:** age += d (wraps at 14,400 s). **Pre-warm:** when age = 0 and skip time > 0, step 0.1 s
  repeatedly until age > skip time. **Stop:** when stop time > 0 and age + d > stop time, the emitter enters its
  stop path (0x16abe0). [H]
- **Spawning** (0x1640d0, 0x1655f0): the effective time is age − initial delay. Events happen at a fixed interval
  I: burst mode uses `25` burst delay; rate mode uses I = 1/`2A`, stored back into the burst-delay field at
  pool sizing. Each event spawns **1 particle (rate mode) or `26` particles (burst mode)**. Every candidate passes
  a free-RNG test against the system's emission scale (scalar 0, normally 1). Particles take free pool slots (a
  stack); **when the pool is empty the spawn is dropped**. Events missed inside one update are back-filled: each
  is pre-aged by (now − event time) and spawned with the emitter transform interpolated to that sub-step. Events
  older than the lifetime are skipped. `27` burst count limits the events in burst mode (0 = unlimited). Spawn
  source 3 spawns one particle per parent-mesh vertex, in an order shuffled with **CRT `rand()`**. [H]
- **Pool size** (0x163da0): rate = `2A` or `26`/`25`. When I ≤ lifetime (weather: 1 s), pool = ceil(rate ×
  lifetime), else pool = rate. Then × the parent pool for linked children, × the parent-mesh vertex count for
  spawn source 3. **Cap 5,003 particles per emitter.** Particle records are 0x70 bytes. [H]
- **Particle Detail** (option 0..1, global 0x140a12570): active slots = ceil(clamp(scale₂, 0, 1) × detail × pool),
  where scale₂ is system scalar 2. Slot i is active iff ((0xEDF1 + i·0xECF3) mod pool) < active, a fixed spread
  independent of the RNG. Spawn-source-3 emitters are exempt unless they are weather. It is re-evaluated when
  the option changes (0x161e60, 0x15e690). [H]
- **Per-particle init** (0x164690, free RNG): lifetime from the recomputed lifetime shape, i.e. L·(1−r)..L (SPEC
  §3.2); velocity / position from the shapes; outward speed; inherit velocity; random colour, rotation and spin
  (SPEC §3.1). [H] for the RNG, SPEC for the fields
- **Integration** (0x166020 → 0x166350 / 0x1676a0 / 0x167350): age += d; a particle dies when age ≥ its
  lifetime (0x16a7d0, which also triggers a spawn-on-death child). Otherwise the forces of SPEC §3.1 apply
  (acceleration, optionally object-space, gravity on z, outward acceleration, wind ½·d, disturbance spheres),
  then position += velocity·d. Ground behaviour 1/2 ray-tests the terrain and bounces with `30`. Weather particles
  (0x163a90) instead move with velocity + wind×5, wrap inside the camera cube, and wrap their age modulo the
  lifetime. [H] for the structure, [M] for the exact force order inside 0x166350 (not transcribed)
- **Tracks:** colour, alpha, scale, texture cell and spin are looked up from the 32-entry resampled tables at
  age/lifetime (SPEC §3.3). `48` freezes rotation at spawn. [M]

### B.5 Drawing (`alParticleGroup::vf8` 0x172290 → `alParticleGroupRenderTask`)

- The system is skipped when hidden, when global 0x140a12649 (particles enabled) is clear, or when the context's
  pass mask lacks bit 8 (Transparent). [H]
- Emitters with live particles are split by the heat flag (`3B`):
  - non-heat → one task in **bucket 8** (Transparent), or **bucket 7** (Water) when the system flag +0x338 is set;
  - heat → one task in **bucket 11** (Heat), only when the heat bit is in the mask;
  - in special passes (0x13ec90 true, e.g. reflection) all emitters go into one bucket-8 task.

  Drawing marks the system "drawn" (+0xc9). [H]
- Bucket 8 is sorted back-to-front **per renderable** (RENDER.md §1.4). Inside a task, emitters draw **in file
  order**, one draw call each (0x162a20), skipping emitters whose `15` enabled flag is 0. [H]
- **Per-particle sort:** only when the blend mode is 0, 2, 5 or 11 **and** `2B` is set (0x168810, camera
  distance). Otherwise particles keep pool order (0x168a10). [H]
- Geometry: CPU-expanded, 3 vertices (`05` = 0) or 4 (`05` = 1) per particle, into the dynamic buffer
  (HOTSPOTS: 0x162c60–0x1698fe). Tails stretch along velocity (SPEC `41`/`42`). Effects come from the blend-mode
  table in SPEC §3.1 (Engine\Prim*). [H]
- **Culling:** whole-system AABB vs frustum like any renderable (RENDER.md §5). The AABB comes from live
  particles (padded by 1.5 × tail size) or from the weather cube. Empty emitters use a unit box. [H] (0x163670)
- No global per-frame particle cap was found. The limits are the per-emitter cap (5,003), Particle Detail, and
  the allocation pools (`PooledObjectClass<alParticles,1024>`, `<alParticleGroup,128>`,
  `<alParticleGroupRenderTask,1024>`, which are pool block sizes, not caps). Debug stats string at 0x199cb0. [M]

### B.6 Determinism

Render particles are **not sim-relevant**: nothing in the sim reads particle positions, counts or RNG. They use
the free LCG (0x140a13e20; MAIN_LOOP §4.1) and CRT `rand()` (per-thread MS LCG, spawn-source-3 shuffle only).
Their clock is the render delta d. A GPU implementation (Gabriel's choice) may use any RNG and any float order.
Only the **sim-side particle GameObjects** (B.1.1) must follow the synced rules: creation, synced hit-particle
picks, and the lifetime in frames. [H]

### B.7 Port notes for R4 (GPU particles)

- Keep the CPU-side emitter bookkeeping for parity of counts and timing: interval events with back-filling,
  pool size and cap, the Particle Detail slot spread, burst limits, pre-warm, stop time, and per-tick d = 0.03.
  Simulate and expand on the GPU.
- Per-emitter draw in file order; per-particle sort only for blend modes {0, 2, 5, 11} with `2B`; buckets 8 / 7 / 11
  as in B.5.
- Proxy names: strip `_ALTn` / `_LODn` before resolving, keep the alt index for the visibility filter, and
  ignore unresolvable names.

---

## C. Open items

1. Values of the blend-time globals 0x140b39930 (idle), 0x140b16d18 (move), 0x140b39900–0x140b39908 and
   0x140b3993c: no writer found. Capture: dump them from a running game. [L]
2. Particle track interpolation modes 0/1/2 (B.2) and the exact force order inside 0x166350. [L]/[M]
3. ~~Writer of 0x140a12648 and of system scalars 0/2~~ answered: the throttle global is statically 1 (throttle on);
   scalars are set through 0x173220 by planet corruption (`Particle_*_Per_Corruption_Level` lerps), weather,
   SurfaceFX, Lure, TransportLanding and a model API (STRATEGIC_GAPS §6.2). [H]
4. Where `Space Idle` (1) is selected; the default-idle function never returns it. [L]
5. The meaning of object +0x3a7 (animation advance skipped). [L]
