# Hardpoints and abilities (StarWarsG.exe FoC x64 v1.121) — behaviour spec for roadmap S9

Written 2026-10-27 by an RE agent, ahead of roadmap step **S9 "Hardpoints & abilities"** (engine/tactical).
Clean-room behavioural spec from the Ghidra 12.2 decompilation (`re_reference/ghidra`). It contains no code.
RVAs are relative to image base 0x140000000. Confidence: **[H]** read directly and cross-checked, **[M]** read
once or partly inferred, **[L]** inferred or guessed. FPS = the logic rate global (30). "Synced RNG" = the
lockstep GameRandom of TACTICAL_COMBAT.md §0.2. Vanilla counts come from `extracted/foc_data_text.zip`.

**This document builds on, and does not repeat:**
- `TACTICAL_COMBAT.md` §3.6 (Take_Damage order), §3.7 (shields), §3.8 (saber deflection), §4 (both modifier
  systems), §5 (hardpoint damage routing, hull/hardpoint 0.2 coupling, repair, offline rules, unit-ability
  modes, countdown summary, special-ability framework summary). Sections below that say "delta" only add to it.
- `SPACE_AND_OPEN.md` §B.9–B.10 (hardpoint bones, show/hide and destruction visuals).
- `OBJECT_RUNTIME.md` §4.3 (behaviour type ids), §4.4 (behaviour service order), `COMMANDS.md` §2.2 (wire
  layouts of commands 40, 43, 45, 57, 58), `xml_field_tables.md` (field offsets).

Object field names used below: obj +0xf0 = combat data ("CD"), obj +0x100 = special-ability data pack ("SAP"),
obj +0x1e8 = ability-countdown data, obj +0x2d0 = hardpoint vector, obj +0x298 = type, obj +0x332+t = slot of
behaviour type t (0xFF = absent), obj +0x3d0 + 16·t = unit-ability slot t (data pointer, +8 active byte, +9
autofire byte).

---

## 0. Summary for the implementer

1. **Hardpoints are not game objects.** They are 0xD8-byte `HardPointClass` records owned by the object,
   created in `HardPoints` list order at object init, with **one synced draw each** (initial weapon cooldown).
   They consume no object ids. Their destruction spawns objects (explosion particles, breakoff props) that do.
2. **Unit abilities** (`Unit_Abilities_Data`, enum 1..75) are per-object slots plus a **static per-type info
   table** (§2.1) that decides whether the ability is a toggled mode, which behaviour implements it, whether
   it auto-deactivates on expiry, whether a team container forwards it to its members, and whether "Force"
   gating applies. All gameplay effect of the plain modes is `Mod_Multiplier` (System B); the rest forwards to
   a special ability by name (`GUI_Activated_Ability_Name`).
3. **Every activation is a command event** (43 toggle, 40/57 targeted) executed at the command phase of the
   frame, or the same event executed directly by Lua. Gates are in `Can_Activate_Unit_Ability` (§2.4).
4. **Timers are integer frames** in `AbilityCountdown` (behaviour 0x46): active phase counts up to
   int(FPS × Expiration_Seconds), recharge counts down from int(FPS × Recharge_Seconds × (1 + object-level
   modifier list 9)). Truncation, not rounding (§2.6).
5. **Targeted abilities** (tractor beam, ion cannon shot, lucky shot, energy weapon, …) become an attack order
   with a *pending ability* on the unit; the targeting behaviour fires the special ability when it is in
   range, every targeting service, until it succeeds (§2.9).
6. **No synced RNG is drawn anywhere in the unit-ability or special-ability framework paths read** (activation,
   countdown, tractor, ion, lucky shot, stealth, jamming, laser defense, missile shield). The draws S9 adds are
   at hardpoint creation, at death (death projectiles) and in converted-unit decay (§6).
7. **Port choices to make up front:** iterate "all objects" scans (stealth detection, sensor jamming, laser
   defense, missile-shield emitters) in **ascending object id**; vanilla uses the GOM list order, and two
   of these are first-match-wins (§6.3).

---

## 1. Hardpoints (delta to TACTICAL_COMBAT §5.1)

Weapon hardpoints do not replace the unit's own `Projectile_Types` weapon: both fire independently, each behind its
own deploy / ROCKET_ATTACK / mode gate (TACTICAL_COMBAT §3.1.1, 2026-11-02).

### 1.1 Creation at spawn [H]
Hardpoints are built by the object (re)initialiser **0x3989a0** (called on spawn, type change and load):
1. The function first fills the 75 unit-ability slots from the type's `Unit_Abilities_Data` (type +0x1ab0), in
   XML order; a later entry of the same `Type` replaces an earlier one (§2.3).
2. Only if the object **has a model** (obj +0x2a0) and the type has hardpoints (count type +0xe40, pointer array
   +0xe38), it allocates the vector at obj +0x2d0 and, **for each hardpoint type in `HardPoints` list order**:
   - constructs the instance (`HardPoint::ctor` 0x381a90) with owner, list index, type, and health =
     `Health` × the mode multiplier (0x540070; Land 1.0 / Space 1.5). The ModifierSet list-2 bonus is **not**
     applied here; it only enters through `HP_Get_Max_Health` (0x385ae0) later.
   - **Synced draw:** initial weapon cooldown = `Get_Int(0, int(FPS × Fire_Max_Recharge_Seconds))` (type
     +0x22c), stored at instance +0x58. Exactly one `Get_Int` per hardpoint, in list order, *including*
     non-weapon hardpoints (the draw is unconditional). `Get_Int` can consume several RNG steps (§0.2 of
     TACTICAL_COMBAT). When the bound is 0 there is no draw.
   - stores the current frame at +0x60 and +0x68, copies `Fire_Pulse_Count` (+0x230) to +0x5c;
   - a hardpoint of type `ENABLE_SPECIAL_ABILITY` (11) is put online at once (0x384bb0, see TACTICAL §5.1);
   - resolves bones on the owner model: `Attachment_Bone` → +0x2c, `Fire_Bone_A/B` (in the attached model) →
     +0x38/+0x3c, `Damage_Particles` → +0x30 (its subtree hidden now), `Engine_Particles` → +0x34
     (SPACE_AND_OPEN §B.10).
3. After all are built, each is post-initialised (0x384740) in list order.
4. Then the SAP is created if the type has `Abilities` (0x374da0 → 0x37c050).

**Port note:** an object without a model gets **no hardpoints at all** in vanilla. A headless port must still
create them (the sim needs them); bone indices can be resolved lazily. Keep the draw order: one `Get_Int`
per hardpoint in list order at the moment the object is initialised, interleaved with the other spawn draws.

### 1.2 Instance fields used by S9 [M unless tagged]
| Offset | Meaning |
|---|---|
| +0x10 | owner object [H] |
| +0x18 | index in the owner's `HardPoints` list (this is the "hardpoint id" carried by commands 40/45 and by ion/lucky-shot targets) [H] |
| +0x20 | hardpoint type [H] |
| +0x28 | health (float) [H] |
| +0x2c / +0x30 / +0x34 / +0x38 / +0x3c | bone indices (attachment, damage particles, engine particles, fire A, fire B) [H] |
| +0x50 | opportunity target |
| +0x58 | weapon cooldown counter (frames), randomised at creation [H] |
| +0x5c | pulses remaining |
| +0x6e | **disabled** flag, separate from destroyed (destruction does not set it). Set only by the station-upgrade hardpoint copy, cleared only by repair to full health, not saved. See **HARDPOINT_DISABLED.md** [H] |
| +0x6f | being repaired (repairer list +0x78 non-empty) [H] |
| +0x78 | list of repairing players (TACTICAL §5.1) |

### 1.3 Health, destruction and effects on the parent
All per-hit rules, the coupling, repair and the per-type offline effects are in TACTICAL §5.1 and are not
repeated. Additions:
- **Owner death kills its hardpoints [H]** (`Die` 0x39bdb0): every destroyable hardpoint (`Is_Destroyable`)
  with health > 0 receives `HP_Take_Damage(kind 6, its scaled base health)`, in list order, which runs the
  full destruction path (visuals, `Death_Explosion_Particles` and `Death_Breakoff_Prop` objects spawned).
  Already-dead ones get 0x384a30 (cleanup). So a capital-ship death spawns its hardpoint debris **after** the
  death signal and **before** the death clone (§4.9).
- **Hull fraction seen by scripts/AI** (`Get_Min_Health_Fraction` 0x396470, Lua `Get_Hull`) [H]: the hull
  fraction, but if the type has `Should_Be_Destroyed_When_All_Hardpoints_Destroyed` (type +0xa8) and at least
  one destroyable hardpoint, the minimum of the hull fraction and the hardpoint fraction (Σcur/Σmax, 0x396070),
  clamped to [0, 1].
- **"Damaged model swap" is a smoke attachment, not a model swap [H]** (0x3ab890, correcting TACTICAL §3.6
  step 18): when hull health crosses **75 %** or **50 %** of maximum downward in one hit, and the type has
  `Damaged_Smoke_Asset_Name` (type +0xc58), `Data\Art\Models\<name>` is attached to the model. Visual only;
  no gameplay field changes. Hardpoints have no partial-damage stage (SPACE_AND_OPEN §B.10 item 5).
- **No hardpoint death event on its own** beyond what TACTICAL lists: destruction raises the owner-type
  "last weapon of its class" event [L] and the offline signals; the generic object-death signal 0x25 is only
  raised by `Die`.
- **"All hardpoints dead → hull" rule:** only with type +0xa8 (TACTICAL §3.9). Without it, a ship can lose every
  hardpoint and keep flying with hull damage routed normally.

### 1.4 Ids consumed by hardpoint and ability code [H unless tagged]
| Spawns an object (consumes an id) | Does not |
|---|---|
| hardpoint `Death_Explosion_Particles`, `Death_Breakoff_Prop` (one each per destruction) | hardpoints themselves |
| projectiles (every shot, incl. ion/lucky-shot override projectiles) | unit-ability activation, countdown |
| `Dummy_Barrage_Target` for every position-targeted special ability (Lua or command 57) | modifier entries |
| death clone, one `Death_Projectiles` entry (§4.9) | tractor/stealth/jamming visuals (render-side) |
| ability `Spawned_Object_Type` objects (self-destruct blasts, buzz droids, bombs, mines) [M] | |

---

## 2. Unit abilities (`Unit_Abilities_Data`)

### 2.1 The type enum and the static info table [H]
`UnitAbilityType` (0x5dea20). The info table at 0x140b2f2e0 has 0x4d entries of 0x30 bytes (index = type) and is
filled by 0x43fab0 at boot; `Data\XML\UnitAbilityTypes.xml` is the override file and is **empty in vanilla**.
Columns: **M** = +0x20 "mode" (the per-object active byte is written on activate/deactivate; read by 0x43fa70),
**B** = +0x24 behaviour type that implements it (−1 none; when M = 0 and B ≠ −1, `Is_Active` = that behaviour is
active), **X** = +0x29 auto-deactivate on expiry (0x43fa10), **U** = +0x2a "per-unit": a team container does
**not** aggregate it over its members (0x43fa90), **F** = +0x2c "Force power": blocked while the Force-disable
timer runs (0x43fa30). +0x28 and +0x2b are 1 everywhere (meaning not read). Rows without explicit values in
0x43fab0 keep the entry constructor's defaults, assumed to be the same as the explicit pattern
M1 B−1 X1 U0 F0 [M]. Vanilla FoC usage counts in the last column (`Unit_Ability` entries).

| Id | Name | M | B | X | U | F | FoC |
|---|---|---|---|---|---|---|---|
| 1 | DEFEND | 1 | − | 1 | 0 | 0 | 9 |
| 2 | DEPLOY | 1 | − | 1 | 0 | 0 | 11 |
| 3 | INTERDICT | 1 | − | 1 | 0 | 0 | 2 |
| 4 | HARASS | d | d | d | d | d | 0 |
| 5 | BARRAGE | 1 | − | 1 | 0 | 0 | 4 |
| 6 | HUNT | d | 0x21 HUNT | d | d | d | 25 |
| 7 | SPREAD_OUT | 1 | − | 1 | 0 | 0 | 29 |
| 8 | AFTERBURNER | d | d | d | d | d | 0 |
| 9 | TURBO | 1 | − | 1 | 0 | 0 | 4 |
| 10 | LURE | d | 0x23 LURE | d | d | d | 6 |
| 11 | ROCKET_ATTACK | 1 | − | 1 | 0 | 0 | 5 |
| 12 | AVOID_DANGER | d | d | d | d | d | 0 |
| 13 | FORCE_TELEKINESIS | 0 | − | 1 | 0 | **1** | 4 |
| 14 | FORCE_WHIRLWIND | 0 | − | 1 | 0 | **1** | 4 |
| 15 | FORCE_LIGHTNING | 0 | − | 1 | 0 | **1** | 2 |
| 16 | FLAME_THROWER | 0 | − | 1 | 0 | 0 | 3 |
| 17 | JET_PACK | 0 | − | 1 | 0 | 0 | 6 |
| 18 | CAPTURE_VEHICLE | 1 | − | 1 | 0 | 0 | 2 |
| 19 | TARGETED_INVULNERABILITY | 0 | − | 1 | 0 | 0 | 2 |
| 20 | TARGETED_HACK | 0 | − | 1 | **1** | 0 | 3 |
| 21 | TARGETED_REPAIR | 0 | − | 1 | **1** | 0 | 3 |
| 22 | STICKY_BOMB | 0 | − | 1 | 0 | 0 | 4 |
| 23 | AREA_EFFECT_STUN | 0 | − | 1 | 0 | 0 | 2 |
| 24 | AREA_EFFECT_HEAL | 0 | − | 1 | 0 | 0 | 0 |
| 25 | AREA_EFFECT_CONVERT | 0 | − | 1 | 0 | **1** | 3 |
| 26 | DISTRACT | 0 | − | 1 | 0 | 0 | 0 |
| 27 | CABLE_ATTACK | 0 | − | 1 | 0 | 0 | 2 |
| 28 | FOW_REVEAL_PING | 0 | − | 1 | 0 | 0 | 4 |
| 29 | POWER_TO_WEAPONS | 1 | − | 1 | 0 | 0 | 21 |
| 30 | TRACTOR_BEAM | 1 | − | 1 | 0 | 0 | 10 |
| 31 | ENERGY_WEAPON | 1 | − | 1 | 0 | 0 | 2 |
| 32 | MISSILE_SHIELD | 1 | − | 1 | 0 | 0 | 5 |
| 33 | INVULNERABILITY | 1 | − | 1 | 0 | 0 | 1 |
| 34 | LUCKY_SHOT | d | d | d | **1** | d | 7 |
| 35 | HARMONIC_BOMB | 0 | − | 1 | 0 | 0 | 1 |
| 36 | UNTARGETED_STICKY_BOMB | 0 | − | 1 | 0 | 0 | 2 |
| 37 | CONCENTRATE_FIRE | 1 | − | 1 | 0 | 0 | 2 |
| 38 | DEPLOY_TROOPERS | 0 | 0x1C DEPLOY_TROOPERS | d | d | d | 4 |
| 39 | SELF_DESTRUCT | 1 | 0x43 SELF_DESTRUCT | **0** | d | d | 10 |
| 40 | SPRINT | d | d | d | d | d | 6 |
| 41 | ION_CANNON_SHOT | d | d | d | **1** | d | 10 |
| 42 | WEAKEN_ENEMY | 0 | − | 1 | 0 | 0 | 1 |
| 43 | SPOILER_LOCK | d | d | d | d | d | 14 |
| 44 | REPLENISH_WINGMEN | 0 | − | 1 | 0 | 0 | 3 |
| 45 | MAXIMUM_FIREPOWER | d | d | d | d | d | 2 |
| 46 | EJECT_VEHICLE_THIEF | d | d | d | d | d | 18 |
| 47 | FIRE_LOBBING_SUPERWEAPON | 0 | − | 1 | 0 | 0 | 2 |
| 48 | STEALTH | 1 | − | 1 | 0 | 0 | 14 |
| 49 | STUN | 0 | − | 1 | 0 | 0 | 2 |
| 50 | RADIOACTIVE_CONTAMINATE | 0 | − | 1 | 0 | 0 | 1 |
| 51 | BERSERKER | 0 | − | 1 | 0 | **1** | 1 |
| 52 | FORCE_SIGHT | 0 | − | 1 | 0 | **1** | 1 |
| 53 | SABER_THROW | 0 | − | 1 | 0 | **1** | 1 |
| 54 | FORCE_CLOAK | 1 | − | 1 | 0 | **1** | 1 |
| 55 | SUPER_LASER | 1 | − | 1 | 0 | 0 | 2 |
| 56 | LASER_DEFENSE | 0 | − | 1 | 0 | 0 | 2 |
| 57 | FORCE_CONFUSE | 0 | − | 1 | 0 | **1** | 2 |
| 58 | LEECH_SHIELDS | 0 | − | 1 | 0 | 0 | 1 |
| 59 | TACTICAL_BRIBE | 0 | − | 1 | 0 | 0 | 2 |
| 60 | SWAP_WEAPONS | 1 | − | 1 | 0 | 0 | 4 |
| 61 | CLUSTER_BOMB | 0 | − | 1 | 0 | 0 | 1 |
| 62 | FULL_SALVO | 1 | − | 1 | 0 | 0 | 1 |
| 63 | SENSOR_JAMMING | 1 | − | 1 | 0 | 0 | 3 |
| 64 | PLACE_REMOTE_BOMB | 0 | − | 1 | 0 | 0 | 4 |
| 65 | DETONATE_REMOTE_BOMB | 0 | − | 1 | 0 | 0 | 4 |
| 66 | INFECTION | 0 | − | 1 | 0 | 0 | 1 |
| 67 | PROXIMITY_MINES | 0 | − | 1 | 0 | 0 | 4 |
| 68 | STIM_PACK | d | d | d | d | d | 7 |
| 69 | DRAIN_LIFE | 0 | − | 1 | 0 | **1** | 4 |
| 70 | BLAST | 0 | − | 1 | 0 | 0 | 1 |
| 71 | BUZZ_DROIDS | 0 | − | 1 | **1** | 0 | 3 |
| 72 | SHIELD_FLARE | 0 | − | 1 | 0 | 0 | 1 |
| 73 | SUMMON | 0 | − | 1 | 0 | 0 | 1 |
| 74 | CORRUPT_SYSTEMS | 0 | − | 1 | 0 | 0 | 1 |
| 75 | DEPLOY_SQUAD | 0 | − | 1 | 0 | 0 | 2 |

`d` = constructor default (see above). Corrections to TACTICAL §5.2: id 10 is **LURE** and id 49 is **STUN**
(icon strings `i_sa_lure.tga` at entry 10 and `i_sa_stun.tga` at entry 49) [H]. The table also gives each
type's GUI icon name (entry +0x00), which the renderer/UI can use. One vanilla entry spells the type in lower case
(`power_to_weapons`, Acclamator); enum lookup is case-insensitive [M].

### 2.2 `Unit_Ability` XML fields [H for offsets (xml_field_tables.md), M for meanings]
| Tag | Offset | Vanilla uses | Meaning |
|---|---|---|---|
| `Type` | +0x18 | 332 | enum §2.1 |
| `Active_By_Default` | +0x1c | 2 (INTERDICT, REPLENISH_WINGMEN) | on at spawn (where it is applied was not found, §8) |
| `Effective_Radius` | +0x20 | 48 | radius for MISSILE_SHIELD, SENSOR_JAMMING, CONCENTRATE_FIRE, LURE, LASER_DEFENSE… |
| `Recharge_Seconds` | +0x24 | 222 | recharge (§2.6) |
| `Expiration_Seconds` | +0x28 | 100 | active duration; ≤ 0 = no active phase |
| `Is_Pulsing`, `Pulse_Frequency_Secs`, `Max_Number_Of_Pulses` | +0x2c/+0x30/+0x34 | 4 (LURE) | pulsed effect |
| `Target_Types` | +0x38 | 4 | LURE target type list |
| `Particle_Effect` | +0x58 | 10 | effect on activation |
| `GUI_Activated_Ability_Name` | +0x68 (CRC at +0x88) | 132 | name of the special ability this forwards to (§2.5) |
| `Spawned_Object_Type`, `Max_Num_Spawned_Objects` | +0x90/+0xa0 | 35/4 | SELF_DESTRUCT blasts etc. |
| `Bomb_Countdown_Seconds` | +0xa4 | 3 | |
| `Area_Effect_Decal_Distance` | +0xa8 | 19 | UI |
| `Mod_Multiplier` / `Mod_Flag` | +0xac / +0xe0 | 320 / 2 | System B (TACTICAL §4.4) |
| `Projectile_Types_Override` | +0xe8 (vector: data +0xf0, count +0xf8) | 15 | ION_CANNON_SHOT, LUCKY_SHOT, … projectile swap (§3.3) |
| `Targeting_Max_Attack_Distance` | +0x108 | 4 | |
| `Disable_Movement` | +0x10c | 5 | immobilise while active (MISSILE_SHIELD, INTERDICT) |
| `Owner_Attachment_Bone` | +0x110 | 7 | |
| `Armor_Type` | +0x128 | 0 | |
| `SFXEvent_*` | +0x170…+0x1a0 | | audio only |
| `Friendly_Ability`, `Stop_When_Activated` | +0x1b0/+0x1b1 | 5/5 | |
| `Targeting_Fire_Inaccuracy_Fixed_Radius_Override` | +0x1b4 | 4 | |
| `Target_Position_Z_Offset` | +0x1b8 | 4 | |
| `Alternate_Name_Text/Description_Text/Icon_Name` | +0x1c0/+0x1e0/+0x200 | | UI |
| `Must_Be_Bought_On_Black_Market` | +0x220 | 25 | campaign gate (§2.4) |
| `Damage_Percent_When_Activated` | +0x224 | 7 | STIM_PACK self-damage (§2.5) |
| `Supports_Autofire` | +0x24c | 69 | autofire allowed / default-on (§2.10) |

Mod_Multiplier names in vanilla XML carry a `_MULTIPLIER` suffix (`WEAPON_DELAY_MULTIPLIER, 0.5f`); kinds as
in TACTICAL §4.4. Values are written with a trailing `f` in vanilla; the parser must accept it.

### 2.3 Per-object state [H]
- **Slots:** obj +0x3d0 + 16·t: +0 pointer to the `Unit_Ability` record (refcounted), +8 active byte, +9
  autofire byte. Filled at init from the type's list (§1.1 step 1). `Has_Ability(t)` (0x3986b0) = slot pointer
  non-null; for a team container whose type entry has U = 0, it is "any member has it".
- **Which record is used** (0x397d90): if U = 1 or the object is not a team container (no TEAM behaviour), its own
  slot; otherwise the container walks to its member (0x4ec820 on obj +0x110) and uses the member's record.
- **Countdown data** (obj +0x1e8, behaviour 0x46): int counter[0x4d] at +8, int maximum[0x4d] at +0x13c, byte
  phase[0x4d] at +0x270 (1 = active phase counting up, 0 = recharge counting down), last-update frame +0x2c0.
- **Pending targeted ability** (CD +0x148 target, +0x150 special-ability name CRC, +0x154 unit-ability type,
  §2.9).
- **Autofire default at spawn:** for the **local** player's objects only, when the game option
  `AbilitiesDefaultToAutofire` is on, 0x3989a0 queues a `SetAbilityAutofire` command (id 58) for every slot with
  `Supports_Autofire`. It goes through the command stream, so it is synced; the port should treat it as a
  normal client command.

### 2.4 `Can_Activate_Unit_Ability(obj, t, checkRecharge, checkNebula)` (0x39b480) [H]
All gates, in order; any failure returns false:
1. t outside 1..75.
2. `checkNebula` and the object is inside the nebula lockout: (frame − CD +0x104) / FPS <
   `Nebula_Ability_Disable_Time` (GameConstants +0x1181c, vanilla 5.0).
3. Entry F = 1 and the Force-disable timer runs: elapsed = (frame − CD +0x110) / FPS (elapsed < 0.1 s counts as
   0) ≤ `Force_Ability_Disable_Time` (+0x11824, vanilla 0.0, so this is "the same frame" in vanilla).
4. obj +0x3a7 set (garrison-style object) and its garrison/container count is 0 [M meaning].
5. SAP disabled byte (SAP +0x28) set.
6. The object has behaviour 0x58 **CORRUPT_SYSTEMS** (IG-2000's ability on the victim): **no ability at all**.
7. `checkRecharge` and the object has ABILITY_COUNTDOWN: false when counter ≥ 1 **and** (phase 0, or the
   active phase has already reached its maximum) — i.e. while recharging, or in the frame between reaching the
   active maximum and the expiry update.
8. Team container: every member must pass (recursive, `checkRecharge` = 1).
9. The locomotor reports "busy" (locomotor vfunc +0x2b0, e.g. hyperspace jump) [M meaning].
10. Per-type checks:

| Type | Extra condition |
|---|---|
| 1 DEFEND | not ion-stunned (behaviour 0x45 active blocks); has SHIELDED; not in the ion-storm lockout (CD +0x100 within `Ion_Storm_Shield_Disable_Time` 5.0) nor the depleted-shield lockout (0x380ad0); shield behaviour allows (0x549460) |
| 3 INTERDICT | gravity well not destroyed (obj +0x3ca = 0) |
| 6 HUNT | not already hunting (obj +0x3a8 counter = 0, locomotor / squad checks 0x39ad40) |
| 18, 19, 23, 24, 25, 74 | false when obj +0x3a7 = 1 |
| 30 TRACTOR_BEAM | ≥ 1 live TRACTOR_BEAM (4) hardpoint, then as 31 |
| 31, 34, 55 (ENERGY_WEAPON, LUCKY_SHOT, SUPER_LASER) | weapons not disabled (obj +0x3a0 bit 0x10 clear) |
| 38 DEPLOY_TROOPERS | DEPLOY_TROOPERS behaviour present and idle |
| 47 FIRE_LOBBING_SUPERWEAPON | player's super-weapon state allows (needs base power when the type requires it) |
| 52 FORCE_SIGHT | no ability of the object is busy (0x37c520, §4.14; this row read "SABER_THROW" before 2026-11-01) |
| 58 LEECH_SHIELDS, 70 BLAST | ≥ 1 live WEAPON_SPECIAL (10) hardpoint |
| 64 PLACE_REMOTE_BOMB | player's remote-bomb count < `Max_Remote_Bombs_Per_Player` (20) |
| 72 SHIELD_FLARE | current shield fraction > 0 |
| 73 SUMMON | SAP summon slot free |
| 75 DEPLOY_SQUAD | the SPAWN_SQUADRON state is 4 (ready) and ≥ 1 live FIGHTER_BAY (3) hardpoint |

**Black market gate** (0x39bbd0, campaign only): outside skirmish, an ability whose record has
`Must_Be_Bought_On_Black_Market` is unusable until the owner bought it. This is checked by the GUI, Lua
`Is_Ability_Ready` and `Activate_Ability`, **not** by 0x39b480; skirmish always passes [H].

### 2.5 Activation path and per-type dispatch [H]
**Command 43 `SetUnitAbilityMode`** (execute 0x430270; payload in COMMANDS §2.2: object ids, type, on/off, a
position q1–q3, target id): for each listed object, in list order:
1. `Can_Activate(obj, t, on, on)`;
2. skip when `Is_Active(obj, t, 1)` already equals `on`;
3. when turning on and the object has a countdown, require the active counter < 1 (0x42f8e0);
4. `Set_Unit_Ability_Active(obj, t, on, pos, player, target)` (0x38a350).
The response sound afterwards is local-player audio only.

**`Set_Unit_Ability_Active`** (0x38a350):
- Fails unless `Has_Ability`; when turning on, `Can_Activate(obj, t, 0, 1)` must pass and the ability must not
  already be active (BARRAGE has a re-target exception).
- **Team containers** with U = 0: the call is forwarded to every member that has the ability (target id
  forced to none); the container itself only proceeds for SPREAD_OUT (7).
- Then a per-type handler. At the end, if entry M = 1, the slot's active byte := `on`.

| Types | Handler | What it does |
|---|---|---|
| 1 DEFEND, 9 TURBO, 29 POWER_TO_WEAPONS, 43 SPOILER_LOCK, 58 LEECH_SHIELDS | 0x3a4250 | DEFEND: toggles the shield-bubble mesh, forces the shield and energy behaviours to service next frame (their +0x30 = frame + 1). TURBO/SPOILER_LOCK: animation states (SPOILER_LOCK plays the S-foil animation). POWER_TO_WEAPONS/LEECH_SHIELDS: animation state 5. Then: **on** → start active phase (0x42fde0); **off** → early-deactivate (0x42f320, §2.6) unless already recharging |
| 2 DEPLOY | 0x3a1ce0 | deploy/undeploy |
| 3 INTERDICT | 0x3a2240 | §3.9 |
| 5 BARRAGE | 0x3a1930 | area attack at the position |
| 6 HUNT, 10 LURE, 12 AVOID_DANGER, 38 DEPLOY_TROOPERS, 39 SELF_DESTRUCT | 0x3a4070 | turns the implementing behaviour (column B) on/off |
| 7 SPREAD_OUT | 0x3a3500 | formation spread |
| 11 ROCKET_ATTACK | 0x3a3260 | |
| 14 FORCE_WHIRLWIND | 0x3a1fb0 | |
| 17 JET_PACK | 0x3a38d0 | jump to position |
| 18 CAPTURE_VEHICLE | | turning off restarts recharge and ejects (EJECT 46 logic) |
| 19, 23–26, 28, 35, 36, 42 | 0x3a3e20 | targeted/area "instant" effects at position or target |
| 22, 30, 31, 34, 37, 41, 45, 55 | none | only the active byte; the work is done by the forwarded special ability (§3) |
| 32 MISSILE_SHIELD | inline | **on**: (correction, TACTICAL_GAPS.md §1.7: the unit is already an emitter from spawn; activation only makes it "active"); with `Disable_Movement`, increments the immobilise counter obj +0x3a8 (0x3a8e50; the first increment splits the unit into a formation of its own, A1_READINGS §28.8) and pauses deploy; starts the active phase. **off**: decrements the counter, restarts recharge. Both toggle animation state 7 |
| 33 INVULNERABILITY | inline | turns the countdown on/off only; effect = `TAKE_DAMAGE_MULTIPLIER 0` |
| 40 SPRINT, 68 STIM_PACK | inline | needs a squad/locomotor path; STIM_PACK **on** first applies self-damage `Take_Damage(kind 1, Damage_Percent_When_Activated × max hull)`; then countdown on/off; animation state 4 |
| 44 REPLENISH_WINGMEN | 0x3a2970 | |
| 46 EJECT_VEHICLE_THIEF | inline | |
| 48 STEALTH, 54 FORCE_CLOAK | inline | **on**: start countdown, then forward to the special ability (gates as §2.4 re-checked). **off**: restart recharge and break stealth (0x592d00) |
| 49, 52, 56, 57, 61, 63, 69, 70, 72 | inline | re-check §2.4 gates, then **forward** to the special ability whose name CRC equals the record's `GUI_Activated_Ability_Name` CRC (+0x88) via 0x37b8a0 (§3.1). SENSOR_JAMMING **off** restarts recharge and removes the ability effect |
| 60 SWAP_WEAPONS | inline | flips obj +0x324 when the type has more than one weapon set |
| 62 FULL_SALVO | inline | countdown on / restart recharge off; animation state 5 |
| 65 DETONATE_REMOTE_BOMB | 0x280ec0 | detonates the player's remote bombs |
| 75 DEPLOY_SQUAD | inline | resets SPAWN_SQUADRON, sets its next-ready frame = frame + int((Recharge_Seconds − 1) × FPS) and starts recharge |

### 2.6 Countdown (AbilityCountdownBehavior 0x46) — exact [H]
- **Update** (0x42f910, each service; delta = frame − last update frame): for every t = 0..0x4c in order:
  - phase 0 (recharge): if counter > 0: counter = max(0, counter − delta). When it reaches 0 and
    `Can_Activate(obj, t, 1)` passes, signal 0x2c ("ability ready", consumed by AI/UI) is raised.
  - phase 1 (active): counter = min(counter + delta, maximum). When counter = maximum → expiry (0x42f460):
    if entry X = 1, `Set_Unit_Ability_Active(teamLeaderOrSelf, t, off)`, phase := 0, start recharge. Signal 0x29
    either way.
- **Set duration** (0x42fae0): counter = maximum = **int(FPS × seconds)** (truncation).
- **Start active** (0x42fde0): only if `Expiration_Seconds` > 0: set duration(`Expiration_Seconds`),
  counter := 0, phase := 1.
- **Start recharge** (0x42fb70): R = `Recharge_Seconds` × (1 + r9), where r9 = the max-then-sum reduction of
  **list 9 of the object's own ModifierSet only** (CD +0x138 → list at +0x120, with that list's clamps; the
  owner player's set is *not* merged here, unlike `Gather_Modifiers`). If R > 0: phase := 0, set duration(R).
  If R ≤ 0 nothing changes (the ability is ready at once).
- **Deactivate early** (0x42f320): phase 0 → start recharge. Phase 1 → f = 1 − counter/maximum (1 when either is
  0), start recharge, then counter := max(0, int(maximum × (1 − f) + 0.5)) — i.e. the recharge is scaled by
  the fraction of the active time used; if the recharge maximum is 0, signal 0x2c.
- `Expiration_Seconds` without `Recharge_Seconds` (e.g. Interdictor MISSILE_SHIELD with both 0) gives a
  permanent toggle.

### 2.7 `Is_Unit_Ability_Active(obj, t, ·)` (0x39b950) [H]
1. Team container with U = 0: true iff at least one member has the ability and **every** member that has it is
   active.
2. Must `Has_Ability` and have a record.
3. Entry B ≠ −1 and M = 0: active = "that behaviour is active" (HUNT, LURE, DEPLOY_TROOPERS).

> **Status 2026-10-30:** HUNT and LURE have M = 1 (active = slot byte), as corrected in TACTICAL_GAPS §1.1; only DEPLOY_TROOPERS keeps M = 0. The port table has HUNT and LURE at mode 1 (`engine/tactical/abilities/src/ability_types.cpp:29, 33`; `ability_types.cpp:61` for DEPLOY_TROOPERS at 0).

4. Special cases: DEPLOY (2) = the deployed state (0x39a390, or the type `Deploys` and the locomotor's deployed
   query); BARRAGE (5) = needs the BARRAGE behaviour and its data, and is also true while a barrage is in
   progress (barrage data +0xc) or the behaviour is active; JET_PACK (17) = derived from the locomotor's jump
   query (vfunc +0x300). INTERDICT (3) reads the normal slot byte (obj +0x400 is simply slot 3).
5. Otherwise: the slot's active byte.

### 2.8 Mode effects in vanilla FoC (`Mod_Multiplier`, System B) [H for values, M for consumers]
The product over the type's first two `Unit_Abilities_Data` entries that are active (TACTICAL §4.4). Values in
vanilla XML:

| Mode | Multipliers (most common first) | Expiration / Recharge s |
|---|---|---|
| DEFEND | SHIELD_REGEN 1.0, SHIELD_REGEN_INTERVAL **0.1**, ENERGY_REGEN_INTERVAL 0.1, ENERGY_REGEN 3 (or 5), WEAPON_DELAY 1 (or 3), SPEED 0.8 | 15–25 / 20–60 |
| POWER_TO_WEAPONS | WEAPON_DELAY 0.5 (or 0.2), SPEED 0.5, ENERGY_REGEN 1, SHIELD_REGEN **−25** or 0 | 6–20 / 50–60 |
| TURBO | SPEED 2, WEAPON_DELAY 3, ENERGY_REGEN 1, SHIELD_REGEN 0 or 1 | 20–23 / 50–60 |
| SPOILER_LOCK | WEAPON_DELAY 3, SPEED 2 (1.75, 1.3), SHIELD/ENERGY_REGEN 1 (or 3) | none (permanent toggle) |
| SPREAD_OUT | SPEED 0.5, TAKE_DAMAGE 0.5 | toggle |
| SPRINT | SPEED 2 (2.5, 1.5), TAKE_DAMAGE 0.75 (0.5) | 7–12 / 30–45 |
| INVULNERABILITY | TAKE_DAMAGE 0 | 6 / 60 |
| LURE | TAKE_DAMAGE 0.25 (0.7) | 6–16 / 45–50 |
| TRACTOR_BEAM (on the user) | SPEED 0.8 or 0.6 | − / 5–25 |
| ENERGY_WEAPON, SUPER_LASER (user) | SPEED 0.8 | 5–10 / 10–120 |
| LUCKY_SHOT | CAUSE_DAMAGE 600 — **skipped by the product**; applied once on the hit (§4.5) | − / 60 |

Notes: a negative SHIELD_REGEN (POWER_TO_WEAPONS −25) makes each regen tick *drain* the shield; the regen clamp
(TACTICAL §3.7) floors it at 0. SHIELD_REGEN_INTERVAL 0.1 makes regen tick 10× as often. WEAPON_DELAY > 1
slows fire. SPEED (kind 6) is read by the speed reader 0x376990 [M]. **No consumer of kinds 11 TARGET_SPEED and
12 TARGET_TAKE_DAMAGE was found** (no call of 0x398010 with 11 or 12) [M]: treat them as unused in vanilla.
Kind 10 SCATTER_RADIUS is read by area damage 0x4c7f70.

### 2.9 Targeted abilities and the pending-ability mechanism [H unless tagged]
**Command 40 `TacticalSpecialAbility`** (execute 0x429960; command 57 is the same with a dummy target object
created at a ground point): target must exist and not be dying; for each user in list order:
`Can_Activate(user, t, 1)` and the special ability is enabled, ready and accepts the target (0x37c380 →
vfunc +0x28) and the user is not a squad member → `Issue_Auto_Target(user, target, hardpointIndex, 1,
abilityNameCRC, t, …)` (0x38d730). This is an attack order that **also stores the pending ability** in the
user's CD (+0x148 target, +0x150 name CRC, +0x154 type). Successful users are added to a new AI/selection
group and moved toward the target (attack order).
**Firing:** every targeting service (`Targeting::Service_Land/_Space` 0x634810/0x633ae0 and team targeting
0x62eeb0) calls 0x565de0 first: if a pending ability exists, it calls the activate-by-name function
(0x37b8a0 → enabled 0x37c270, ready 0x37be10, apply vfunc +0xf0). The class's apply checks range (0x48d820 /
0x48e100: the targeting behaviour's in-range/needs-to-close tests evaluated with the ability temporarily set as
pending) and returns false while out of range. On success the pending fields are cleared (+0x154 := −1). So a
targeted ability is retried at the targeting cadence until in range, and `Is_Ability_Ready` also returns
true while it is pending (§5).

**Lua `Activate_Ability(name, arg)`** (0x570a50) builds the matching command and executes it immediately (same
on every machine). The argument it expects tells which family a type belongs to [H for the case lists, M for
which list goes with which error message]:

| Argument | Types |
|---|---|
| none or bool (mode toggle via command 43) | 1, 3, 7, 9, 11, 29, 32, 33, 39, 40, 43, 48, 52, 54, 56, 60, 62, 63, 65, 68, 69, 70; DEPLOY (2) toggles deploy |
| target **object** (command 40) | 13, 15, 16, 18, 20, 21, 22, 27, 30, 31, 34, 37, 41, 45, 51, 53, 55, 58, 59, 64, 66 |
| target **position** (command 57 style, spawns `Dummy_Barrage_Target`) | 50, 67, 71, 73, 74 |
| command 43 with the position zeroed (no position) | 14, 23, 24, 25, 35, 36, 38, 44, 46, 49, 57, 61, 72 |
| position via command 43 | 5, 26, 28, 42 |
| special | 6/10/12 (HUNT, LURE, AVOID_DANGER: optional target), 17 JET_PACK (position), 19 (object) |

Correction (S7 t26 RE, 2026-11-02) [H]: the command-57 position row is ids 50, 67, 71, 73, 74; ids 14, 23, … 72
were listed there before but take command 43 with no position. Which families return an `AbilityBlockStatus`
and which signals finish it: AI_TACTICAL §7.3.3.

Argument handling per family (0x570a50, second switch; 2026-11-02) [H]. The Lua call always needs exactly 2
arguments (name + one argument).
- **Toggle row:** the argument must be a bool, and it gives the mode (on / off) of command 43.
- **Command 43 with no position** (14 FORCE_WHIRLWIND, 23, 24, 25, 35, 36, 38, 44, 46, 49, 57, 61, 72): the argument
  is **not read** (any value). Command 43 has mode **on**, position (0,0,0) and target 0x3FFFFF.
- **Position via 43** (5, 26, 28, 42): the argument must convert to a position (0x4990b0, else "expects a target
  position"). Command 43 has mode on, that position and target 0x3FFFFF.
- **17 JET_PACK:** same as position via 43, after 0x397d90(unit, 17) [M: gate]. No dummy.
- **19 TARGETED_INVULNERABILITY:** the argument must be an object or an AI target with a live object ("expects a
  target object" / "target object is already dead"). It is **command 43**, not 40: mode on, position = the target's
  position (obj +0x78), target id = the object's id (obj +0x50).
- **Command-57 row** (50 RADIOACTIVE_CONTAMINATE, 67 PROXIMITY_MINES, 71 BUZZ_DROIDS, 73 SUMMON, 74 CORRUPT_SYSTEMS):
  1. the argument must convert to a position;
  2. a `Dummy_Barrage_Target` is created there for the unit's owner (0x29f810);
  3. the type's `GUI_Activated_Ability_Name` CRC is checked (else "not hooked up");
  4. a `TacticalSpecialAbility` command (0x4292e0 / 0x429da0) is built with the dummy as target object and executed
     (0x429960), the same path as command 40 (pending ability stored, fired when in range).

  Who removes the dummy for these five ids was not traced [L] (Remote_Bomb's toss kills its own, see "Ground
  marker" below).

It errors with "not hooked up to special ability logic" when a targeted type's record has no matching
`GUI_Activated_Ability_Name` in the type's `Abilities`.

**Thrown abilities: facing, lock, release and the ground marker (S8 t10, 2026-11-02)** [H unless tagged]. Full
text: LOB_SOLVER §3.5, "MARKER removal" and "Facing at apply".
- **Facing.** `Grenade_Attack` (0x700860) and `Remote_Bomb` (0x719c90) apply both snap the unit's yaw to the
  target's raw position (no z adjust; roll and pitch 0) with `0x3a8710`, instantly, with no turn rate. The snap
  is skipped while obj +0x3a0 & 0x10 (stunned / weapons disabled).
- **Then:** stop (`0x3a9880`), movement lock `0x3a8e50(obj, 1)` (counter obj +0x3a8), disable TARGETING.
- **The throw** waits for the locomotor to stop, not for the facing, and does not turn again.
- **Release** (end of the animation or interruption): enable TARGETING and the locomotor, unlock (decrement the
  counter), signal 0x29. The facing is not restored.
- **Ground marker.** A `Dummy_Barrage_Target` (command 57) target is killed by the toss 0x71a200 after the spawn
  attempt, even when nothing spawned. It has no lifetime of its own, so it **stays** when the toss never happens.
  [M] Whether the GUI issues command 57 for Remote_Bomb (type 64 is in the Lua object-target list above).
- **Garrisoned thrower (s78f t03, 2026-11-03)** [H]. The Remote_Bomb toss goes through `Spawn_Projectile`, so a
  thrower inside a host with garrison guns launches from the host bone chosen by the same rule as a garrisoned
  WeaponBehavior shot (LOB_SOLVER §5.1 "Garrison pick inputs"):
  - candidate = the bone's **x-axis yaw** (column 0 of its world matrix, animated pose) within 60° of the yaw from
    the host position to the target's raw position;
  - one synced `Get_Int(0, n − 1)` when n ≥ 2, none when n = 1;
  - the host's current damage ALT selects the bone row;
  - the pick happens before the lob solve, so a toss with no arc has already spent the draw (LOB_SOLVER §5.2 "Order
    and draws"); the toss returns false and is not retried (§3.5).

  The port's abilities hook `garrison_fire_bones` returns bone positions only, and `remote_bomb_launch` tests the
  angle of the bone's **position** around the host. It should get each bone's world position **and** x-axis yaw
  (or matrix) and test the yaw, as combat's `garrison_muzzle` does. Vanilla bunkers have no animation, so rest pose
  × host transform is exact.
- **DEPLOY (2) "active" (§2.7 item 4).** It is `0x39a390`: the locomotor pack's pending flag +0x2b3, or Walk
  state 0x19 (Deployed) only, not 0x18 Deploying (TACTICAL_COMBAT §3.1.1 "Gate details").
- **Locomotor swaps and hardpoints (S8 t12).** The JET_PACK / locomotor swap (`0x3a9910`, MOVEMENT §3.1.1)
  suspends only TARGETING, WEAPON and SPECIAL_WEAPON. **Weapon hardpoints are not suspended**:
  - `Update_Weapon` keeps running;
  - the hardpoints keep the target pushed before the flight, and no new target is pushed while TARGETING is
    suspended;
  - they are held only by their own gates.

  Because a swap undeploys the unit (MOVEMENT §3.2, "A locomotor swap undeploys"), `Dark_Trooper_PhaseIII`'s
  deployed-only missile hardpoints stop firing when it jets and stay off until it deploys again. A hardpoint with
  `Fire_When_Undeployed` = Yes would keep firing in the air [H from the code paths; M: no vanilla unit has one,
  not observed in game].

### 2.10 Autofire [H for state, open for the driver]

> **Status 2026-10-30:** The driver is found: it runs in the sim as `Issue_Auto_Target` (0x38d730) → 0x37d280, with one synced pick when several abilities qualify, and AI players autofire every User_Input ability (TACTICAL_GAPS §1.3). The port implements it as `AbilitySystem::autofire_driver` (`engine/tactical/abilities/src/ability_behaviors.cpp:188-239`).

`SetAbilityAutofire` (command 58, execute 0x4b3860) sets slot byte +9 on each listed non-member object that has
the ability. `Is_Ability_Autofire` (0x39b310) = that byte, or the forwarded special ability's own
"automatic" answer (0x48d5e0), after the black-market gate. The code that **acts** on autofire (decides to
activate) was not found in the sim; all readers of 0x39b310 are command-bar GUI code. It is most likely the AI
player layer (open item §8).

---

## 3. Special abilities (the `Abilities` list)

### 3.1 Framework (delta to TACTICAL §5.4) [H unless tagged]
- **Fields** (SpecialAbilityClass): name +0x18, name CRC +0x38 (matched against unit-ability +0x88),
  `Activation_Style` +0x3c, `Initially_Enabled` +0x40, `Applicable_Unit_Categories` +0x60,
  `Applicable_Unit_Types` +0x68, `Excluded_Unit_Types` +0x90 (target filter 0x48e290), class fields from +0x178.
- **`Activation_Style` enum** (0x5e42e0) [H]: 1 Global_Automatic, 2 Combat_Automatic, 3 Galactic_Automatic,
  4 Space_Automatic, 5 Ground_Automatic, 6 Ground_Activated, 7 Hero_Detected, 8 Combat_Imminent,
  9 Special_Attack, 10 Take_Damage, 11 User_Input, 12 Skirmish_Automatic. The XML spelling `COMBAT_AUTOMATIC`
  (two vanilla jammers) must parse case-insensitively.
- **Allowed in the current mode** (0x37c270): never when the object has obj +0x3a0 bit 0x80 or the global
  abilities-off flag; style 1 always; 2, 9, 10, 11 in tactical modes; 3, 6, 7, 8 in galactic; 4 in space; 5 in
  land; 12 only in skirmish.
- **Ready** (0x37be10): the player's override for this type/ability if one exists (enabled unless the
  override says disabled), else `Initially_Enabled`.
- **Apply / remove**: vfunc +0xf0 (slot 30) apply(owner, target), +0xf8 (31) remove(owner, target). Every
  successful apply registers the effect in the owner's SAP (0x55e100: ability name → list of target ids;
  remove 0x55f120). `apply` usually first removes the previous target's effect (one target per ability).
- **Per-frame update**: inside `GameObject::Update` (§6.2) the SAP update 0x37da80 walks the type's
  `Abilities` in list order; an ability with a service interval (vfunc +0x18 > 0) that is ready is updated
  (vfunc +0x20) when its due frame ≤ now, then rescheduled to now + interval. Due frames are kept per
  ability name.
- **Event triggers**: 0x37b580(style, …) tries every ability of the list in order whose style matches, which is
  allowed/ready, accepts the context (vfunc +0x28) and passes 0x39b0b0; the first that applies returns 1
  (unless "apply all"). Used with style **10 Take_Damage** by `Take_Damage` (§4.6) and with the automatic
  styles on unit add/remove (auras, TACTICAL §5.4).

### 3.2 Tractor beam (`Tractor_Beam_Attack_Ability`, 0x710080…, TRACTOR_BEAM 30) [H unless tagged]
Vanilla: 9 users (Star Destroyer, Executor, Piett, satellite…), `Activation_Max_Range` 800–2000,
`Target_Speed_Decrease_Percent` 0.75–0.98, `Stacking_Category` 0 or 2, applicable to transports/corvettes/
frigates (+capital for some) and named heroes (Millennium Falcon…), `Excluded_Unit_Types` Buzz_Droids.
- **Accepts target** (vfunc +0x28, 0x710540): enemy (0x39a450), passes the applicable filter, target
  locomotor not busy, layer check when either side has obj +0x3b4 set, owner passes `Can_Activate(30)` (needs
  a live tractor hardpoint), target not in the nebula lockout, target has a model.
- **Apply** (0x710130): allowed (§3.1) and in range → remove the effect from any previous target →
  register → `Set_Unit_Ability_Active(owner, 30, on)` → **target speed modifier** (0x710280) → beam visual.
  - Modifier: one entry in the **target's** object ModifierSet **list 5 (speed)**, value
    **−Target_Speed_Decrease_Percent**, the ability's stacking category, source = owner, name = owner type name,
    **permanent** (duration −1 → expiry 0xFFFFFFFF), inserted with the replace-by-source insert 0x3fe810 (a
    second application from the same source overwrites, it does not stack).
  - **Corvettes are slowed twice as much**: if the target's `CategoryMask` contains `Corvette` and the target
    type is neither `Is_Named_Hero` nor `Is_Generic_Hero`, the value is −2 × percent. The list-5 minimum clamp
    (−0.99, TACTICAL §4.3) limits the result.
- **Update** (vfunc +0x20, 0x710710): while TRACTOR_BEAM is active, `Can_Activate(owner, 30, 0, 1)` still passes
  (so destroying the last tractor hardpoint ends it), the target is not in a nebula and the target is still in
  range → mark the target "tractored" (target CD +0x132 = 1) and refresh the beam; otherwise remove.
- **Remove** (0x710880): `Set_Unit_Ability_Active(owner, 30, off)`, erase every list-5 entry of the target whose
  source is the owner, clear target CD +0x132, destroy visuals, early-deactivate the countdown (§2.6), signal
  0x29.
- The `Mod_Multiplier SPEED 0.6/0.8` on the **user** is the unit-ability product while active.
- What reads CD +0x132 was not traced [L: blocks the target's hyperspace/retreat].

### 3.3 Ion cannon shot (`Ion_Cannon_Shot_Attack_Ability`, ION_CANNON_SHOT 41) [H unless tagged]
Vanilla: Y-wing, V-wing, TIE Defender squadrons; `Projectile_Types_Override` = `Proj_Ion_Cannon_Medium_Laser_Blue`;
recharge 20–25 s; target filter `All`.
- **Apply** (0x704980 → 0x704b50): fails if the owner or any squad member already has 41 active. Registers,
  sets 41 active on the owner **and every squad member**, and for each of them resets the non-hardpoint weapon
  burst: CD +0x24 := 0 and CD +0x28 := int(`Fire_Pulse_Count` (type +0x4f0) × FIRE_RATE product (kind 9) ×
  (1 + gathered list 6)) [M meaning: pulses fired / pulse budget]. Stores the ion target (SAP +0x270) and the
  chosen target hardpoint index (SAP +0x278, −1 = any) from the command.
- **Projectile swap**: the hardpoint/weapon fire path (`HP_Fire_At` 0x3825b0 via 0x397e00, weapon list
  0x394eb0) uses the override projectile **only while 41 is active and the shot's target is the ion target (or
  its container) and the hardpoint matches when one was chosen**. A unit with 41 (or 34) active **does not fire
  its normal weapons at other targets**.
- **One volley**: after each shot (0x3a06a0, called by `HP_Fire_At` and `WeaponBehavior`), if 41 is active, the
  shot was at the ion target and its projectile type equals the override, 41 is turned off for that shooter.
- **Update** (0x704f50): ends when the target enters a nebula, when 41 is no longer active or allowed (for
  squads: when no member is still active and allowed). If the unit's current attack target differs from the
  ion target (or hardpoint), it re-issues `Issue_Auto_Target(owner, ionTarget, hpIndex)`; a chosen hardpoint
  that died or is untargetable is reset to −1.
- **Remove** (0x705250): deactivate owner and members, clear the target (+0x278 := −1); signal 0x29. It calls
  `Start_Recharge` (0x3a9840 → 0x42fb70) only for a squad in which some member was still active. Where the
  recharge of a **single** unit (or of a squad whose members all fired) starts was not located [M]; candidates
  are the generic special-ability end path 0x48eb10 or the GUI. See §8 item 14.

> **Status 2026-10-30:** Resolved (TACTICAL_GAPS §1.11): 0x48eb10 is the galactic hero-respawn scheduler, not a recharge. Only a team-container owner with a member that fired recharges, in the Remove step 0x705250; the port does this in the IonCannonShot / LuckyShot case of the special-ability remove path (`engine/tactical/abilities/src/ability_system.cpp:972-1003`).

- **The ion effect is a projectile property** (§4.4).

### 3.4 Lucky shot (`Lucky_Shot_Attack_Ability`, LUCKY_SHOT 34) [H]
Vanilla: Red Squadron, Luke's X-wing; override projectile `Proj_Ship_Lucky_Shot_Medium_Laser_Cannon_Red`;
unit-ability `CAUSE_DAMAGE_MULTIPLIER 600`; recharge 60 s.
- Apply (0x706320 → 0x7064e0) mirrors the ion shot (weapon burst reset for owner and members, 34 on, target
  at SAP +0x280, hardpoint index +0x288, "fired" flag +0x28c := 0). Update 0x706850 mirrors 0x704f50.
- **Consumption in `Take_Damage`** (TACTICAL §3.6 step 11): when the source's (or its container's) 34 is active
  and the victim is the lucky target (or its container) and the hit hardpoint matches (or −1): set
  "fired", turn 34 off, and apply the record's CAUSE_DAMAGE multiplier (×600) to **this** hit. A lucky shot that
  hits the hull is re-routed to the hardpoint closest to the impact (0x394a80).
- Remove (0x7069e0): turn off, signal 0x29; the recharge starts only if the shot actually fired.

### 3.5 Concentrate fire (`Concentrate_Fire_Attack_Ability`, CONCENTRATE_FIRE 37; Home One) [H unless tagged]
- Requires the unit ability 37 with `Expiration_Seconds` > 0 and an ABILITY_COUNTDOWN behaviour (errors logged
  otherwise).
- Apply (0x6f5550): query the **owner player's** objects within `Effective_Radius` (6000) (0x2a9ff0); every
  eligible unit not already hunting is given an **Attack command (id 6) built and executed directly** on the
  target; their ids are remembered (+0x180 vector). Then 37 on, countdown on, target speed modifier (list 5,
  −`Target_Speed_Decrease_Percent`, vanilla 0.0) and the `Particle_Effect` on the target.
- `Target_Damage_Increase_Percent` (0.5 in vanilla) is parsed but **no reader was found** [M].
- Update 0x6f5cc0: removes when 37 is no longer active (expiry). Remove 0x6f5d50: 37 off, erase the target's
  list-5 entries from this source, signal 0x29.

### 3.6 Energy weapon (`Energy_Weapon_Attack_Ability`, ENERGY_WEAPON 31; Piett/Accuser) [M]
Apply (0x6f9ad0): in range → 31 on, countdown on, beam visual. Update (0x6f9e60): while 31 active, allowed,
target not in a nebula and in range: `Take_Damage(target, kind 1, Damage_Per_Frame (50), source owner,
hardpoint = target's chosen hardpoint)` once per update — the update interval of this class was not read
(per frame by name).

### 3.7 Stealth (`Stealth_Ability`, STEALTH 48; Tyber Zann, Phantom, Vengeance, Twi'lek…) [H unless tagged]
- **Apply** (0x5926e0): stealth duration frames (SAP +0x2b8) = int(FPS × (1 + s) × `Expiration_Seconds` of the
  unit ability 48), where s = the owner **player's** stealth-duration list (player tactical data +0x268,
  max-then-sum with clamps; written by `Battlefield_Modifier_Ability` `Stealth_Duration_Multiplier`), and
  (1 + s) ≤ 0 is replaced by 1. Then: stealthed flag SAP +0x2a4 := 1, last-detected frame SAP +0x2bc := 0,
  fade parameters from `Stealth_Transition_Time`, the targeting behaviour drops its target, every hardpoint
  clears its target (0x38f840), the unit's AI group is told to move on, model meshes: `BASEMESH` hidden-flag set,
  `STEALTH` mesh shown.
- **`Is_Stealthed`** (0x39b140) [H]: stealthed flag set **and** last-detected frame + int(FPS ×
  `Stealth_Detection_Time` (GameConstants +0x12094)) < now. Garrison-style objects use obj +0x220's frame
  instead. Targeting filters use this (TACTICAL §2.3).
- **Update** (0x592e80, every frame): visual fade spring (render-only floats); duration counter −1 per update,
  at 0 → remove (reason 3). While running, **detection** (0x592ae0): walk all objects; the first live enemy
  (not dying/converted, not inside a container, with a model) within 3D distance < its detection range sets
  last-detected := now. Detection range (0x3979d0) = 0 unless the type has `Target_Stealth_Units` (+0xa4);
  land: `Land_FOW_Reveal_Range` × terrain factor (0x3722a0); space: `Space_FOW_Reveal_Range` (+0x1d6c). Becoming
  visible plays `SFX_Stealth_Reveal` (audio).
- **Breaking stealth** (0x592d00, fade time t): duration := 0, flag := 0, signal 0x29, meshes restored. Called on
  STEALTH/FORCE_CLOAK off, by **firing** (0x3a06a0 breaks with t = 0.3 s whenever the shooter is stealthed),
  by tactical bribe and earthquake attacks.
- Must be bought on the black market in campaign for 5 vanilla entries (§2.4).

### 3.8 Sensor jamming (`Sensor_Jamming_Ability`, SENSOR_JAMMING 63) [H unless tagged]
Vanilla: Skipray, Houndstooth (user), two Underworld defence structures (`COMBAT_AUTOMATIC`, plus a passive
missile shield).
- Apply (0x719000): jamming frames (SAP +0x378) = int(FPS × unit-ability 63 `Expiration_Seconds`) if the unit
  has 63, else int(FPS × `Duration_In_Secs`).
- Update (0x719620): style Combat_Automatic → the counter is pinned at 10 (permanent). In a nebula → remove.
  Radius = unit-ability 63 `Effective_Radius`, else the type's `Passive_Missile_Shield_Radius`. Every object
  that is **not hostile** to the jammer's owner (corrected, TACTICAL_GAPS.md §1.5: friendly units are protected), not in a container, **has a locomotor** (this includes projectiles), alive, within 3D
  distance ≤ radius gets **CD +0x194 := now** ("jammed"). Counter −1; at 0 → remove.
- **`Is_Jammed`** (0x39b2f0) = (now − CD +0x194) / FPS < `Sensor_Jamming_Time` (0.5 s). Read by the projectile
  service: a MISSILE-category projectile that is jammed (or has no target) flies the straight mode instead of
  homing [M on whose CD is tested]; and by a game-mode function 0x35b4d0 [L: radar/visibility].
- An active jammer also counts as a **missile-shield emitter** with its `Effective_Radius` (§4.3).

### 3.9 Interdict (INTERDICT 3, gravity well; Interdictor, gravity well station) [M]
Handler 0x3a2240: needs the gravity well intact (obj +0x3ca = 0). On: start countdown; with
`Disable_Movement` immobilise (0x3a8e50(obj, 1); off: 0x3a8e50(obj, 0) while ability 3 is active; counter shared with
script locks, A1_READINGS §28.8); raise story event `Interdict_Activated`; in space, insert a
**permanent entry keyed by (owner, "INTERDICT")** into a per-player list (player tactical data +0x180) of every
player returned by 0x294910 for the owner [L: the enemy players]; off: remove those entries. The Interdictor
has `Active_By_Default` = Yes. The consumer of the per-player list is the retreat/hyperspace logic [L, not
traced].

### 3.10 Laser defense (`Laser_Defense_Ability`, LASER_DEFENSE 56; HAV Juggernaut, Crusader) [H]
- Apply (0x7161b0): start recharge bookkeeping, active frames (SAP +0x318) = int(FPS × `Defense_Duration_In_Secs`),
  zap cooldown (SAP +0x31c) := 0.
- Update (0x716730, per frame): when the zap cooldown ≤ 0 → search (0x716230), then cooldown := int(FPS ×
  `Recharge_Time_In_Secs`) (0.25/0.5 s); else cooldown −1. Active frames −1; at 0 → remove.
- **Search** (0x716230): walk all objects; the **first** projectile (has the PROJECTILE behaviour) whose owner is
  not the user's owner and not allied, whose `Projectile_Category` is in `Projectile_Types_Targeted`
  (vanilla `MISSILE, ROCKET, MPTL_ROCKET`), that has flown more than `Projectile_Dist_Travelled` and is within
  `Protection_Radius` (3D) is **destroyed** (detonated via its projectile behaviour 0x4caea0 and removed); the
  zap beam visual starts from the nearest of `Bone_Names`. One projectile per zap.

### 3.11 Other classes (fields only; not analysed in this pass)
`Buzz_Droids_Ability` (spawns `Object_Type` Buzz_Droids within `Activation_Max_Range` 400; damage radius 175,
23 dps enemy / 3 dps own), `Cluster_Bomb_Ability` (12 × `Proj_Cluster_Bomb`, detonation 1.5 s), `Blast_Ability`
(charge 5 s, damage ×5), `Leech_Shields_Ability` (25 s, 100 shield/s, ×3), `Super_Laser_Ability`,
`Shield_Flare_Ability`, `Corrupt_Systems_Ability` (attaches behaviour 0x58 to the victim, which blocks all its
abilities, §2.4 gate 6), `Combat_Bonus_Ability` / `Battlefield_Modifier_Ability` / `Enhance_Defense_Ability`
(TACTICAL §4.3; vanilla 130 / 29 / 7 entries, the most common), `Redirect_Blaster_Ability` /
`Absorb_Blaster_Ability` (Take_Damage triggers, §4.6), the hero Force powers (land). Their XML values are in
`xml_field_tables.md`; their class internals are §8 open items.

---

## 4. Combat features deferred by S8

### 4.1 Fog of war in targeting
Already specified in TACTICAL §2.3 (`FOW_Object_Hidden` filter) and the stealth filter; S9 adds `Is_Stealthed`
(§3.7). No further rule was found.

### 4.2 Opportunity fire — correction [H]
TACTICAL §2.6 says the idle-opportunity rule depends on "an AI player". In 0x385620 the test is
player +0x360 ≠ null, and TACTICAL §4.3 identifies player +0x360 as the **faction** pointer, which every
player has. The effective rule is therefore: idle opportunity fire is allowed when `Fire_Max_Recharge_Seconds`
≤ `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` (3.0) **for every player**; above the cutoff the
hardpoint's `Allow_Opportunity_Fire_When_Idle` flag decides. The same cutoff is used in 0x387400.

### 4.3 Missile shields [H unless tagged]
- **Emitters**: a global list (0x140a2bb98, count 0x140a2bba0). Objects are added when MISSILE_SHIELD (32) is
  activated (§2.5) and removed in `Die` when the type has MISSILE_SHIELD in one of its first two ability
  entries, a `Passive_Missile_Shield_Radius` > 0, or active jamming (0x4d07e0). Where passive emitters are
  added at spawn was not traced [M: probably at spawn when the radius > 0].

> **Status 2026-10-30:** Resolved (TACTICAL_GAPS §1.7): passive emitters are registered at creation / load (MISSILE_SHIELD users too) and again on each jammer apply. The port registers them at unit creation (`engine/tactical/combat/src/combat_system.cpp:217`) and on every jammer apply (`engine/tactical/abilities/src/ability_system.cpp:924`).

- **Test** (0x4c6e30, per projectile per step): walk the emitters in list order; an emitter qualifies when it is
  **not allied** with the projectile's owner and (MISSILE_SHIELD active, or passive radius > 0, or jamming
  active). Radius = passive radius, overridden by the unit-ability 32 `Effective_Radius`, overridden by the
  unit-ability 63 `Effective_Radius` when jamming. The first emitter with |projectile − emitter|² < r² (3D)
  wins.
- **Effect**: the projectile is **re-oriented to point straight away from the emitter centre** (0x3abe40) and
  continues; it is not destroyed. Applied in the straight flight mode 0x4cec80 and the homing mode 0x4cf870.
- **Which categories** [H]: `Projectile_Category` enum (0x469280): 0 DEFAULT, 1 MISSILE, 2 BOMB, 3 GRENADE,
  4 ROCKET, 5 STATIC_BOMB, 6 MPTL_ROCKET, 7 LASER, 8 LIGHT_SABER, 9 CLUSTER_BOMB. The flight dispatcher
  (0x4ce6a0) sends 1 MISSILE to 0x4cec80/0x4cf870, 4 ROCKET and 6 MPTL_ROCKET to 0x4cfc40 (which tests the
  passive radius itself), 3 GRENADE to 0x4ceea0, 8 LIGHT_SABER to 0x4cf460, 2/5/9 to the plain step, and **every
  other category including 0 DEFAULT and 7 LASER to 0x4cec80**, which calls the missile-shield test. So in
  vanilla an active missile shield also turns away lasers [M: read from the dispatch, not observed in game].
  Lua `Is_Affected_By_Missile_Shield` (0x605c90) answers true only for categories 4 and 6.
- Vanilla emitters: Interdictor (32, radius 750, permanent toggle, `Disable_Movement`), AT-AA and its company
  (32, 100–150, 10 s / 20 s), two jammer structures (passive 300 / 1000), one ground turret (passive 150).

### 4.4 Ion cannon damage: the ion stun (`IonStunEffectBehavior`, behaviour 0x45) [H]
Projectile fields (type): `Projectile_Ion_Stun_On_Detonation` +0xab, `_Duration` +0x4cc,
`_Speed_Reduction_Percent` +0x4d0, `_Shot_Rate_Reduction_Percent` +0x4d4, `_Stack_Duration` +0xac,
`_Radius` +0x4d8. Applied on a direct hit (from `Process_Hit` 0x4cb280) and, when the radius > 0, to victims in
radius at detonation (0x4cbbb0). Per victim (0x62b920):
1. frames = int(FPS × duration); nothing if ≤ 0 or the victim's locomotor is busy.
2. End frame (behaviour +0x30): without stacking, or when the current end is already past, end := now + frames;
   with `Stack_Duration`, end += frames and the modifier duration becomes (end − now + 1)/FPS.
3. **DEFEND is switched off** if active.
4. Victim's object ModifierSet: list 5 (speed) −`Speed_Reduction_Percent` and list 6 (fire rate)
   −`Shot_Rate_Reduction_Percent`, **stacking category 1**, source = the projectile type, names
   "Ion Stun - Speed Reduced" / "Ion Stun - Fire Rate Reduced", duration = the stun duration (replace-by-source
   insert, so repeated hits from the same projectile type refresh rather than add).
5. The behaviour keeps the "stunned" animation state while active; while active it blocks DEFEND (§2.4).
Vanilla ion projectile: 50 damage, `Does_Shield_Damage` yes, `Does_Hitpoint_Damage` **no** — it only strips
shields — plus 1 s stun, −50 % speed and −50 % fire rate, stacking. It also carries land `Projectile_Stun_*`
fields (300 frames, radius 300) used by the separate land stun path (0x4d01c0, not analysed).

### 4.5 Lucky shot
§3.4.

### 4.6 Damage listeners [H]
In `Take_Damage`, after `Get_Damage_Multiplier` (TACTICAL §3.6 step 12–13), if the victim has a SAP, its
special abilities with style **10 Take_Damage** are tried in list order (0x37d160 → 0x37b580, context = source
and projectile); if any applies, the damage becomes **0** and the out-invulnerable flag is set. Vanilla users:
`Redirect_Blaster_Ability` (52 entries: walkers, heroes, structures, some space units) and
`Absorb_Blaster_Ability` (5, Force users); their apply performs the redirect/block rolls of TACTICAL §3.8 (1–2
synced draws).

### 4.7 Energy pool (`EnergyPoolBehavior`) [H]
- Current energy at CD +0xf8; maximum (0x372320) = `Energy_Capacity` × (1 + list 4).
- **Service interval** (vfunc +0x28 = 0x56bfd0): int(FPS × ENERGY_REGEN_INTERVAL product (kind 5) ×
  `EnergyRechargeIntervalInSecs` (GameConstants +0x358, 5.0) + 0.5) frames.
- **Regen** (0x56c030): a type with `Requires_Base_Power` (+0x8b) whose player has no base power gets energy 0.
  Otherwise, unless regen is blocked (CD +0x130, the same flag shields use), energy := clamp(energy +
  `Energy_Refresh_Rate` × ENERGY_REGEN product (kind 4), 0, max).
- **Per shot** (0x56bee0): a projectile with `Projectile_Energy_Per_Shot` (+0x4b8) > 0 can be fired only when
  energy ≥ it; firing deducts it (clamped).
- **Shield exchange** (0x56be20): shield regeneration of x points costs x × `EnergyToShieldExchangeRate`
  (+0x35c, 5.0) energy, all-or-nothing (TACTICAL §3.7).
- **Energy damage** (`Projectile_Does_Energy_Damage`) is absorbed by `EnergyPool::Absorb` 0x56c230 (TACTICAL
  §3.6 step 15); `Projectile_Disables_Engines_When_Power_Drained` +0x48 is the engines flag (not traced).

### 4.8 Fire ignition and burning [H unless tagged]
- In `Take_Damage`: a victim whose type `Is_Combustible` (+0x81), hit with damage kind 7 (fire), and not yet
  burning, **ignites instead of taking that hit's damage**: obj +0x3a1 |= 2 and a `BURNING` behaviour (0x2d) is
  attached. Later fire hits on a burning object deal normal damage.
- `BurningBehavior` service (0x6241b0): `Take_Damage(kind 7, Burning_Damage_Per_Second (+0x2044))`. Its service
  interval was not read [L: once per second, given the per-second value].
- Burning indigenous units (0x621d80) take `Burning_Damage_Per_Second / FPS` per frame and wander with synced
  `Get_Float` draws (land only).
- `Targeting_Allowed_When_Burning` (+0x83) exists; reader not traced.

### 4.9 Death sequence, death clones, death projectiles (`Die` 0x39bdb0) [H unless tagged]
In order:
1. obj +0x3a0 |= 0x40 (dying), health := 0, 0x392160; record the killer's player byte (obj +0x330, also on the
   team container).
2. Missile-shield / jammer emitter removal (§4.3).
3. Signal **0x25** (object died). Transport/fleet/garrison notifications; the object linked through obj +0x150
   (the container link of TACTICAL §3.6 step 1) dies recursively with the same arguments [M on which side of the
   link].
4. Battle statistics entry (type, owner) appended to the game mode's list.
5. Animation states reset; **hardpoints destroyed** (§1.3).
6. Hero death → special-ability manager event (0x48eb10).
7. Removal from trees / selection (0x391310).
8. **Death clone** (tactical modes, damage kind ≠ 15): the death-clone key is the damage kind, remapped to 11 or
   12 for deploying/deployed units (12 when kind ≠ 8) and to 0x14 when the INFECTION behaviour is active [M on
   the exact deploy condition]; 0x3718f0 picks
   the `Death_Clone` (type +0xd58) entry for the key, falling back to the original kind; the clone object is
   spawned (0x29f270, **consumes an id**) and stored at obj +0x2c0.
9. **Death projectile**: if `Death_Projectiles` (type +0xd38, count +0xd48) is non-empty, **one synced
   `Get_Int(0, count − 1)`** picks one; it is spawned as a projectile object (id consumed) at the object's position
   + `Ranged_Target_Z_Adjust` in z, owned by the dying object's player.
10. Local-player `SFXEvent_Unit_Lost` (audio only).
`DeathBehavior` (0x58d430) then runs the corpse: fade after `Death_Persistence_Duration` with
`Death_Fade_Time`, or the spin-away path (`Spin_Away_On_Death_Time`); it removes the object when done. The
spin-away decision draw and `Destruction_Survivors` were not found in `Die` [open].

### 4.10 Converted-unit decay (TACTICAL §8 item 7 — resolved) [H]
In `GameObject::Update` (0x3a6b80), for an object with obj +0x3a2 bit 4 (converted to the enemy), not dying,
and `Converted_To_Enemy_Die_Time_Seconds` (+0x234c) > 0: d = max hull / that time / FPS; **one synced
`Get_Float` per frame** with bounds ±d/10 [M on the exact bounds: the decompiler shows only −(d/10)]; if
d + draw > 0, `Apply_Hull_Damage(kind 0x11, d + draw)`.

### 4.11 Lobbed and circular lead [M]
The lob solver itself (0x54a6f0) is closed form and does not use this root finder: LOB_SOLVER.md.
Already in TACTICAL §3.1 step 9. Implementation detail: the circular solver (0x399450) calls a generic 1-D root
finder (0x7d68e0) on 0x3aba90 with bracket [0.1, 1000], tolerance 0.001, 15 iterations, passing its context
through **globals** 0x140b2c380–0x140b2c394. The port must pass that context explicitly (thread safety). The
root-finder method is not identified; any deterministic method with the same bracket and stop rule is
acceptable for our own clients.

### 4.12 `Destroy_When_Stunned_Over_Water` (type +0xe6) — port guess, no RE pass, confirmed by probe 2026-11-04
Not traced (Gabriel 2026-10-30, `docs/PHASE4.md` "Roadmap trim": a documented guess instead of an RE pass). The
only vanilla user is `MAL_Rocket_Vehicle` (`UNITS_LAND_UNDERWORLD_MAL.XML`): a `Hover` movement-class vehicle with
`STUNNABLE` in its land behaviours, so it is the one STUNNABLE unit that can stand on water cells (`Hover`
passability includes `Water`). The port's reading of the name: a stunned hover vehicle loses its lift and sinks.
**Rule (engine/tactical/abilities, `AbilitySystem::unit_effect`, Stunned):** when a STUNNABLE stun is applied
(`CombatSystem::stun_unit`: the land stun of `Projectile_Stun_*` detonations or a `Stun_Ability` wave, §4.4 and
TACTICAL_GAPS §2.3.7) to a unit whose type sets the tag, and the unit is over water, the unit dies at once:
`Die` (§4.9) with kind 1 and no killer, so its normal death clone / death projectile apply. "Over water" = the
map has a water plane (`water_type` ≠ None) and the terrain height under the unit's XY is below the water height
(battle host `AbilityHost::over_water`). The test is made only when the stun starts: a stunned unit cannot move,
so its position does not change during the stun. Not included: the ion stun (§4.4, a separate behaviour), Confuse,
Earthquake and other immobilisations. Space battles and maps without water never trigger it.
Why this guess: the tag is a per-type bool that only a hover vehicle sets, and the only state a STUNNABLE unit
changes is "stunned"; destroying at the stun start keeps the sim simple (no per-frame water scan, no save state).
**Probe result (2026-11-04, OI follow-up t02, kit `tools/probe_mod/oi_abilities/`, retail FoC, Dagobah, Han Solo's
`AREA_EFFECT_STUN`: blast centred on Han, `Projectile_Stun_Radius` 150, 600 frames): rule confirmed.** A MAL stunned
on dry land (terrain 14.9 above the water plane) gets the stun flag for 600 frames and survives; a MAL placed on the
water at a point whose terrain is 3.8 below the water plane is removed 2 frames after the stun (the blast's own
lifetime), with no stun-end delay. MALs stunned on low shore ground (terrain 2.9 and 5.0 above the plane) survive, so
"over water" is the terrain test above, not "near water". Not measured: the 46-deep point (the run stopped after the
shallow kill); the port's rule has no depth threshold, and a kill at 3.8 already rules one out above that depth.
Probe notes: `Spawn_Unit` moves a land unit off water cells (the MALs landed 80–90 units away on the shore);
`Create_Generic_Object` places it exactly.

### 4.13 Animation-driven ability timings on sim frames [port]
Several special-ability classes time their effects from the owner's animation (TACTICAL_GAPS §2.3: "at animation
frame N", "at the animation end", Berserker's 20 %–80 % window). The port never reads a playing animation clock in
sim code (determinism rule 3); instead a clip position is a pure function of fixed data and the sim frame count:
- **Position** after n sim frames since the clip started = n × rate × speed. rate = clip fps / 30, the sim
  animation's clock (`engine/anim` advances at t = frame / 30); speed = `Attack_Animation_Speedup` for the melee
  classes, 1 for the others. With no clip data rate = 1 (one clip frame per sim frame, as for a clip at the format's default 30 fps).
- **"At animation frame N"** = the first update whose position ≥ N: Generic `Damage_Frame_Number`, Arc
  `Begin_Applying_Damage_Frame`, Eat `Attachment_Frame` / `Target_Destruction_Frame` (all on `Attack_Animation`),
  Grenade `Spawn_Frame` (on `Toss_Anim`), Super_Laser `Fire_Frame` (on the attack clip, type 5).
- **"At the animation end"** = position ≥ max(count − 2, 0), the frame where a one-shot clip finishes
  (anim_state.hpp `advance_one_shot`). Generic / Eat / Arc_Sweep hold the unit (the special stays applied) until
  then, after their last frame-numbered event; Grenade until the end of `Toss_Anim` after the throw; Hack / Repair
  change the owner / repair at the end of animation 0x5a: ceil(max(count − 2, 0) / rate) sim frames after the apply.
- **Berserker window**: animation 0x68 is taken as looping [port]: position = (n × rate) mod (count − 1), n = the
  frames since the apply (int(FPS × duration) − counter); a damage pass needs counter % 5 = 0 and 0.2 × count <
  position < 0.8 × count.
- **Clip data**: frame count and fps of variant 0 of the clip, from the type's sim model (`AbilityHost::anim_clip`,
  battle host `TacticalBattle::anim_clip_info`; XML names such as `Attack_Animation` / `Toss_Anim` values resolve to
  ModelAnimType ids with `anim::find_anim_type`). This is static model data loaded from the ALA files, the same for
  every run, worker count and compiler.
- **Fallbacks** (no host query, no sim animation, the type has no model or the model lacks the clip): rate 1; the
  melee and grenade classes end with their last frame-numbered event (the pre-t03 behaviour); Hack / Repair end after
  60 frames (`kHackFrames`); the Berserker window is always open. Headless runs without models therefore keep the
  old timings and CRCs.
- The "not in animation X unless ending" apply gates and Super_Laser's `Attack_Anim_Sub_Index`: §4.14 (implemented
  2026-11-01, OI follow-up t01).
**Probe result (2026-11-04, OI follow-up t02, 3 retail runs with clip positions read from memory, ±1 frame):**
- Rancor `Rancor_Pound_Vehicles`: clip 63 advances 0.75 clip frames per sim frame (rate × `Attack_Animation_Speedup`
  0.75); the 240 damage lands 27 frames after each clip start (5 clips over 2 runs), = the port's ceil(20 / 0.75). Match.
- Rancor `Rancor_Eat_Infantry` (one Eat in 3 runs; the Rancor mostly used its plain attack clip 5): clip 64 at 1.0 clip
  frame per sim frame; the victim is destroyed at clip start + 112 (position 111); port: position ≥ 110 → +111. Match
  within the reader's ±1 frame. The attach (re-own) frame was not visible in memory.
- Infiltrator `STICKY_BOMB`: `PROJ_STICKY_BOMB` appears at `Bombtoss` (47) start + 12 / + 13 (port: position ≥ 12);
  the clip ends at + 29 and the thrower's next clip starts at + 30 (port: ceil(max(count − 2, 0) / rate)). Match.
- Yoda `BERSERKER`: damage passes come every 5 frames (counter % 5 = 0 confirmed) and only in the middle of the clip
  (passes at clip positions 8–29, none at 3–5 or 30+, i.e. about 20 %–80 %). Two differences, kept as the [port]
  approximation: retail played **variant 1** of `RI_YODA_SPINMOVE` (38 frames; the port uses variant 0, 36 frames)
  in both runs, and it restarts the clip before its end at irregular points (loops of 36, 35, 34, 34, 34 and 34, 33,
  36, 34, 33 frames), so no fixed period fits. Measured 23 passes per 6 s activation against 20 in the port model.

### 4.14 Ability animation gates and sequences (RE pass 2026-11-01) [H unless tagged]
Settles the gates §4.13 lists as "not modelled". Corrections to TACTICAL_GAPS §2.3: Telekinesis apply has **no**
animation gate; Generic's gate uses its own `Attack_Animation`, not 5; Earthquake refuses on 0x27 only; §2.4's
row "52 SABER_THROW" is case 0x34 = **52 FORCE_SIGHT**.

**What is read.** Every gate reads the **primary slot** of the owner's own model (obj +0x2a0, ANIM_PARTICLES
§A.3): type 0x3956a0 (slot +0x0c), mode 0x395660 (slot +0x10), frame 0x395550 (slot +0x18). No model or no
clip → type = mode = −1, so every gate passes. The owner is the object whose type lists the ability; there is no
redirection to a container or team member. The overlay slot and the near-end bit (obj +0x74 bit 3) are never
read. **"Unless ending" = mode 2 (finished).** The one-shot advance (0x2650f0) sets it on the first advance with
frame ≥ count − 2 (inclusive; frame clamped to max(count − 2, 0)), and also when Δ = 0, so a clip of ≤ 2 frames
finishes at its first advance. Loops never finish. Object Service (0x3a6b80) advances the model (step 6) **before**
the SAP update (0x37da80); behaviours run before both. So an apply from the command phase or a behaviour sees the
previous tick's advance, and ability updates see this tick's. A play sets model +0x148 = now, so a clip does not
move in the tick it starts.

**Apply gates (owner's primary slot)**
| apply | gate | notes |
|---|---|---|
| Earthquake 0x6f7240 | runs the remove-all first (vf15 0x48f1e0 → remove 0x6f7660, which plays 0x27 when an effect was live). Then type ∈ {0x25, 0x26} and the SAP still lists this target (0x48df60) → return 1 without effect [M: unreachable after the remove-all]; **type == 0x27 → refused, in any mode** | no busy check. A re-cast during a quake ends it (FB_Release) and fails |
| Telekinesis 0x6fe1b0 | **none** (the remove-all first plays 0x23 when an effect was live) | no busy check; the target must not already be lifted (0x63efa0) |
| Force_Lightning 0x6fd010 | refused iff type == 0x28 **and** mode ≠ 2 | the SAP target lists are cleared **before** the gate. A refused re-apply during FL_Attack therefore drops the beam and the end-of-clip clean-up [M; recharge 15 normally prevents it] |
| Force_Sight 0x713a90 | busy → refused; type ∈ {0x69, 0x6a} → return 1 (already running); **type == 0x6b → refused, in any mode**; else start | |
| Generic 0x700100 | type ≠ `Attack_Animation` (+0x190) or mode == 2, **then busy**. Busy includes Generic's own predicate, so in land the apply is refused whenever type == Attack_Animation, finished or not | not moving |
| Arc_Sweep 0x6eec40 | SAP +0x212 clear; type ≠ **5** (literal, not the XML field) or mode == 2 | no busy check, so it can cut a Generic/Eat clip [M consequences] |
| Eat 0x6f7c70 | SAP +0x229 clear; busy clear; type ≠ **5** (literal) or mode == 2 | |
| Super_Laser 0x714fa0 | none | |

**Busy** (`0x39a350` = 0x37c520: OR of vfunc +0x30 over the owner type's `Abilities`). The predicates check the
**type only; the mode is ignored**. Earthquake: type ∈ [0x25, 0x27]. Telekinesis: [0x21, 0x23]. Force_Lightning:
== 0x28. Generic: == `Attack_Animation`. Eat: == `Attack_Animation` or == `Target_Grabbed_Animation` (+0x1d0)
[M intent]. These five count only in land mode (mode type 1). Arc_Sweep: SAP +0x212 set, in any mode.
Force_Sight, Super_Laser and the rest: never. Readers: the Generic, Eat and Force_Sight applies, `Can_Activate`
(52), the activation of unit ability 52 (0x38a350) and the formation wait 0x40edd0 [M]. The SAP state is per
owner and per class (Lightning +0x1a8…, Generic +0x208…, Arc +0x212…, Eat +0x21c…, Force_Sight +0x2d0,
Super_Laser +0x300…). Rancor Swat Left and Right therefore share one Arc state.

**Sequences.** Plays go through Play_Animation 0x3a92f0 with start frame 0, blend mode 0 and **lock 0**: while
another caller's lock bit is set they are ignored. Index 0xFFFF = **one synced draw** in [0, count − 1] when the
model has the clip (§A.4). Hold = 0x63f060 / 0x6331e0(obj, 1, 1): it stops the locomotor and suspends
behaviours 9 TARGETING, 10 WEAPON, 11 TURRET, 7 IDLE and 0x20 [M: suspend semantics]. (obj, 0, 1) releases. The
updates act only in land mode. Earthquake and Telekinesis update every FPS / 10 = 3 frames (0x6f74e0); the
others update every frame.
- **Earthquake:** apply → hold, **0x25 FB_Attack** one-shot, 0xFFFF, blend 0.2, speed 1. Update 0x6f77b0: 0x25
  finished → **0x26 FB_Hold loop**, 0xFFFF, blend 0.1. Remove 0x6f7660 → **0x27 FB_Release** one-shot, 0xFFFF,
  blend 0.2. Update: 0x27 finished → release, **type 0 Idle loop**, 0xFFFF, blend 0.2.
- **Telekinesis:** same shape with **0x21 / 0x22 / 0x23** (apply 0x6fe1b0, update 0x6fe6c0, remove 0x6fe570),
  same blends and the same end (Idle loop, 0xFFFF).
- **Force_Lightning:** apply → hold, **0x28 FL_Attack one-shot**, 0xFFFF, blend 0.1, speed 1. Damage is dealt at
  once and recharge 15 starts (§2.3.6). Update 0x6fd880: while 0x28 is not finished, beam visuals only. When it
  finishes (target list not empty): release, default idle (0x395920) index 0, loop = `Loop_Idle_Anim_00`, blend
  0, speed 1; lists cleared; signal 0x29. **The cast lasts exactly the FL_Attack clip.** If something replaced
  0x28, the update does nothing (the hold stays) [M consequence].
- **Force_Sight** (no hold; apply stops the locomotor, 0x3a9880): **0x69 one-shot**, 0xFFFF, blend 0. Update
  0x713ba0: 0x69 finished → Reveal_All on, recharge 52, **0x6a loop**, 0xFFFF, blend 0.1, counter =
  int(FPS × +0x178 `Force_Sight_Duration_In_Secs` [M name]). Each update counts down by 1 unless 0x380b10 or the
  locomotor query 0x39ade0 [M: moving]; either of those ends it at once. At 0 the remove-all calls 0x713d20:
  **0x6b one-shot**, 0xFFFF, blend 0.5, reveal off, signal 0x29. Nothing follows 0x6b; the IDLE behaviour
  replaces it once it has finished.
- **Generic:** apply → hold, **`Attack_Animation` one-shot, 0xFFFF, blend 0.2, speed
  `Attack_Animation_Speedup`**; SAP +0x211 set. Update 0x7003d0: type ≠ Attack_Animation, or finished → default
  idle (index 0, `Loop_Idle_Anim_00`, blend 0) and release. An interrupted attack deals no damage. Otherwise one
  hit at frame ≥ `Damage_Frame_Number`.
- **Arc_Sweep:** apply → hold, **`Attack_Animation` with index `Attack_Animation_Subindex`** (+0x198; explicit, so
  no draw; vanilla 0 = right-to-left, 1 = left-to-right), one-shot, blend 0.2, speed Speedup. Update 0x6eeff0:
  type ≠ Attack_Animation → release only (no idle play). Finished → default idle and release. Otherwise sweep
  from frame ≥ `Begin_Applying_Damage_Frame`.
- **Eat:** apply → hold and arm the state (phase 1). After the optional turret turn, phase 2 plays
  **`Attack_Animation` one-shot, 0xFFFF, blend 0.2, speed Speedup**. Phase 3 runs the frame events (§2.3.8). Type
  changed, clip finished or 0x39a180 → phase 4: default idle, release, remove. The victim plays
  `Target_Grabbed_Animation` loop, 0xFFFF, blend 0 (fallback: Idle index 0, loop).
- **What else ends them:** removal (the remove-all at the next apply; the named removal 0x37ddc0, e.g. Earthquake
  ↔ Force_Cloak; the cancel 0x48cdd0; vfunc +0xb0 0x48e3c0 when ability +0x42 is set [M: triggers]). Writers
  the hold does not suspend can also replace the clip: Lua / cinematic Play_Animation, death, the Berserker
  redirect (§A.4) and Super_Laser's direct play. Generic, Arc and Eat abort when that happens; Lightning,
  Earthquake and Telekinesis ignore it.

**Super_Laser `Attack_Anim_Sub_Index`** (+0x188, default 0). Two paths call **ModelClass play 0x265560
directly**: the apply when there is no `Face_Target` or no TACTICAL_SUPER_WEAPON behaviour, and the Face_Target
branch of update 0x715450 (after `Fire_Delay`, once inside `Firing_Cone`). The call is type 5, index =
sub-index, one-shot, blend 0, speed 1, and it bypasses the lock bit, the Berserker redirect and the random draw.
**If the model lacks that variant (index ≥ count, including no Attack clip at all), the play returns 0 and nothing
changes**: the old primary clip keeps playing. Fire gate (every update after the delay): wait while **type == 5
and frame < `Fire_Frame`** (+0x184, float compare); otherwise start 55's countdown. There is no variant check.
The frame is that of whatever type-5 clip is in the primary slot, normally the sub-index clip just started. With
a missing variant the primary is normally not type 5, so the laser fires at the first update after the delay. A
one-shot clip parks at count − 2, so a `Fire_Frame` beyond that waits until something else replaces the clip [M].
In vanilla both users have their variant: Death Star (`w_planet_deathstar02`, index 1, `Fire_Frame` 60,
`_ATTACK_01`) and Eclipse (`UV_Eclipse_UC`, index 0, `Fire_Frame` 106, `_ATTACK_00`).

**Writers of animation 5 (Attack)**
- WeaponBehavior 0x54d090 after each shot: primary slot, 0xFFFF, blend 0.4 / 0.1. **With
  `Attack_Animation_Is_Overlay` the clip goes to the overlay slot** (0x3a8ed0 → 0x265be0, timed 3 s), which no
  gate reads.
- Spawn_Ability apply 0x71db50: 0xFFFF, blend 0.1.
- TacticalSuperWeaponBehavior 0x42a7b0, state 7: index 0, blend 0.
- Super_Laser (above).
- The melee classes whose XML `Attack_Animation` is ATTACK: the six vanilla Rancor swats and `Sarlacc_Eat_Infantry`.
- The hero-clash `Attack_Action` 0x71ed40, as its fallback.
- Lua and cinematic calls.

An Arc swat never leaves a finished type-5 clip behind: its update replaces the clip with idle in the same SAP pass.
The type-5 test of Arc and Eat therefore matters mainly for a non-overlay weapon shot or a swat that is still
playing. It was not checked whether vanilla Rancors fire such a weapon [M].

**Implementer summary** (port keeps, per object, the animation the ability system started: type, start frame,
clip length from frame count / fps; `n` = sim frames since the start, position = n × rate × speed as in §4.13)
- Read only the owner's own record. No record or no clip data → type "none", not finished: every gate passes and
  nothing is busy. Weapon overlay clips never count.
- **Finished** (one-shot) = position ≥ count − 2, true from the first frame where that holds (count ≤ 2: finished
  at once). A loop never finishes. A new play by the ability system replaces the record.
- Use the state as of the owner's last advance: for an apply that runs before the owner's update in frame N, use
  N − 1; for ability updates, use N.
- Gates: Earthquake refuses iff type == FB_Release (any phase), after running its remove-all, which itself starts
  FB_Release. Telekinesis has no gate. Lightning refuses iff type == FL_Attack and not finished. Force_Sight
  refuses iff busy or type == Force_Revel_End, and succeeds without effect in Begin / Loop. Generic refuses iff
  type == its Attack_Animation (any phase) or busy. Arc refuses iff its per-owner state is active, or type == 5
  (Attack) and not finished. Eat refuses iff its state is active, busy, or type == 5 and not finished.
- Busy = OR over the unit's abilities of type-only tests (FB 0x25–0x27, FTK 0x21–0x23, FL 0x28, Generic /
  Eat clip types; land only), plus Arc's state flag.
- Sequences: Earthquake / Telekinesis Attack one-shot → Hold loop at the first 3-frame update after it finishes
  → Release one-shot on removal → release the hold and play an Idle loop at the first update after it finishes.
  Lightning: FL_Attack one-shot, then default idle when finished (the whole cast). Force_Sight: Begin one-shot →
  Loop for the duration → End one-shot, then the unit's idle. Melee: Attack_Animation one-shot at Speedup, then
  default idle when finished.
- Synced draws: each 0xFFFF play draws once when the model has the clip. That covers every begin / hold / release
  play, the Idle at the end of Earthquake / Telekinesis, FL_Attack, 0x69–0x6b, the Generic / Eat attack and the
  Eat victim. Arc, Super_Laser and the default-idle returns do not draw. Without clip data, make no draw (same
  fallback as §4.13).
- Super_Laser: when the variant `Attack_Anim_Sub_Index` is missing, fire at `Fire_Delay`. Otherwise fire when
  position ≥ `Fire_Frame` on that variant's clip, at that variant's rate (not variant 0's).

**Port (OI follow-up t01, 2026-11-01)** — `AbilitySystem` keeps the record above per object (`anim_type`, start
frame, rate, end, loop, what follows; saved from ability save v2, hashed only when set) and the host gives clip data
(`anim_clip` with the variant, `anim_type` for names, `attack_overlay`). [port] choices: no synced draws (variant 0
unless an explicit sub-index); Eat's `Target_Grabbed_Animation` is not parsed; a Super_Laser whose variant is missing
keeps the §4.13 rate-1 fallback (the host cannot tell "no model" from "no variant"); writers outside the ability
system (Lua / cinematic Play_Animation, death, the Berserker redirect) are not seen by the record.

---

## 5. Lua API semantics [H unless tagged]
| Function (RVA) | Semantics |
|---|---|
| `Is_Ability_Active(name)` 0x57b2a0 | `Is_Unit_Ability_Active(obj, t, 1)` (§2.7); unknown name → error |
| `Is_Ability_Ready(name)` 0x57b580 | false when not `Has_Ability` (warning) or black-market blocked; true when `Can_Activate(obj, t, 1)` **or** the ability is pending as a targeted ability (CD +0x154 = t) |
| `Has_Ability(name)` 0x579db0 | `Has_Ability` |
| `Activate_Ability(name, arg)` 0x570580 | §2.9; executes the command directly (no queue) |
| `Cancel_Ability(name)` 0x5745c0 | `SetUnitAbilityMode(t, off)` executed directly |
| `Force_Ability_Recharge(name[, s])` 0x576b70 | without s: start recharge (§2.6); with s: counter = maximum = int(FPS × s) (phase unchanged) |
| `Reset_Ability_Counter(name)` 0x57f430 | sets counter = maximum = 1 frame when 0x42f8e0 allows [M] |
| `Is_Ability_Autofire` / `Set_Single_Ability_Autofire` / `Set_All_Abilities_Autofire` | slot byte +9 (§2.10); the setters write it directly (0x3a78b0) |
| `Is_Under_Effects_Of_Ability(name)` 0x57c600 | tests a behaviour's active state [L which behaviour] |
| `Get_Hull()` 0x578c30 | §1.3 (tactical only, error elsewhere) |
| `Get_Shield()` 0x579980 | current shield (CD +0xfc) / max shield clamped to [0, 1]; 0 without SHIELDED |
| `Is_Affected_By_Missile_Shield()` (type) 0x605c90 | `Projectile_Category` ∈ {4 ROCKET, 6 MPTL_ROCKET} |

---

## 6. RNG and tick placement

### 6.1 Synced draws added by S9 [H unless tagged]
| When | Draws | Where |
|---|---|---|
| hardpoint creation | 1 `Get_Int(0, int(FPS × Fire_Max_Recharge_Seconds))` per hardpoint, list order (none when the bound is 0) | 0x381a90 |
| death | 1 `Get_Int(0, n − 1)` when `Death_Projectiles` has n ≥ 1 entries (none for n = 1) | 0x39bdb0 |
| converted unit | 1 `Get_Float` per frame | 0x3a6b80 |
| Take_Damage trigger abilities | the redirect/block rolls (1–2) | TACTICAL §3.8 |
| burning indigenous units | several `Get_Float` per update (land) | 0x621d80 |
None in: unit-ability activation, countdown, modifier insert/purge, tractor beam, ion/lucky shot, concentrate
fire, stealth (including detection), sensor jamming, laser defense, missile-shield test, ion stun, energy pool.

### 6.2 Where each piece runs in the tactical frame [H for order inside the object update, M for the rest]
1. **Command phase** (OBJECT_RUNTIME §5.1 step 2): commands 40, 43, 45, 57, 58 and Lua-executed equivalents run
   here: activation, deactivation, pending-ability set-up, repair start, autofire.
2. **Object updates** (stores walked newest-first, OBJECT_RUNTIME §5), per object (0x3a6b80):
   1. ModifierSet expiry purge (object set);
   2. delayed-damage queue (frame counters, `Take_Damage` when due);
   3. **behaviours in attach order** (OBJECT_RUNTIME §4.4): includes ABILITY_COUNTDOWN (expiry/recharge,
      §2.6), SHIELDED, energy pool, ION_STUN_EFFECT, BURNING, TARGETING/TEAM_TARGETING (which first tries the
      pending targeted ability, §2.9, then targeting and hardpoint orders);
   4. **SAP update** (0x37da80): every special ability that is due (§3.1): tractor, ion/lucky shot, stealth,
      jamming, laser defense, energy weapon, …;
   5. script service rate;
   6. hull cap from hardpoints (TACTICAL §5.1), then **hardpoint updates in list order** (0x3a76b0: coupling
      cap, per-hardpoint update, opportunity fire, weapon fire — which may turn off ion/lucky shot and break
      stealth via 0x3a06a0);
   7. converted-unit decay (§4.10).
3. Projectiles are objects in vanilla and are serviced as such (missile-shield test, jamming test in their
   flight step).

**Port mapping onto `CombatSystem::tick` (S8 phases A–G):** run the countdown and the SAP updates per unit in
the same serial newest-first pass as phase D (they read and write other units' modifier sets, so they are not
per-index parallel), **after** phase A's purge and **before** targeting/hardpoint fire, keeping the per-unit
order "countdown → (targeting incl. pending ability) → SAP update → hardpoints". Missile-shield and jamming
tests belong in phase E's projectile step (read-only on emitters; parallel-safe if the emitter list is
snapshot in id order). The one-volley ion/lucky deactivation and the stealth break happen in phase F/D when the
shot is emitted.

### 6.3 Determinism hazards and recommendations
- **First-match scans over "all objects"** — laser-defense target choice (§3.10) and the missile-shield emitter
  pick (§4.3, only the direction differs) — depend on GOM list order in vanilla. Use ascending object id.
  Stealth detection and jamming write the same value for any order, so order does not matter there.
- **Replace-by-source modifier inserts** (0x3fe810) vs append inserts (0x38c850): tractor beam, ion stun and
  concentrate fire use replace-by-source; CombatBonus uses append (TACTICAL §4.3). Keep both behaviours.
- **Float accumulations** in the stealth/death fade springs are render-only; do not CRC them.
- Integer frame timers everywhere: keep int(FPS × seconds) truncation exactly where the table says so (§2.6),
  and int(x + 0.5) only in the modifier expiry and energy interval.

---

## 7. Implementation order for S9 (vanilla FoC skirmish first)

> **Status 2026-10-30:** This build order is finished. The abilities module `engine/tactical/abilities/` implements the unit-ability core, tractor beam, ion shot, lucky shot, concentrate fire, stealth, sensor jamming, laser defense and the additional classes of step 6 (`ability_types.hpp` `SpecialClass`, `ability_classes.cpp`); special-ability types without a class body stay `SpecialClass::Unsupported`. Hardpoints, damage listeners, burning and the S8 deferred items live in `engine/tactical/combat/`.

1. **Hardpoint creation** (§1.1) with its draw, ids, `Get_Hull`, owner-death hardpoint kill, smoke-at-75/50 as a
   render event. Hook into S8's existing routing and coupling.
2. **Unit-ability core**: info table (§2.1), slots and team forwarding (§2.3), `Can_Activate` (§2.4), command
   43 + `Set_Unit_Ability_Active` dispatch for the multiplier modes (DEFEND, POWER_TO_WEAPONS, TURBO,
   SPOILER_LOCK, SPREAD_OUT, SPRINT, INVULNERABILITY, LURE, FULL_SALVO), countdown (§2.6), `Is_Active` (§2.7).
   This covers most space and land skirmish buttons.
3. **Targeted framework**: command 40/57, pending ability, targeting retry (§2.9); special-ability framework
   (§3.1); then tractor beam, ion cannon shot (+ ion stun §4.4), lucky shot, concentrate fire.
4. **Defensive/stealth**: missile shield (§4.3) incl. category dispatch, sensor jamming, stealth, laser
   defense, interdict.
5. **Deferred S8 items**: energy pool, damage listeners + redirect/absorb blaster, fire ignition/burning, death
   clone + death projectile, converted decay, opportunity-fire correction.
6. Remaining ~60 special-ability classes (land heroes, Force powers, bombs, buzz droids) after a follow-up RE
   pass (§8).

---

## 8. Open items
Items 1–12 and 14 are answered in **TACTICAL_GAPS.md** (2026-10-28); several correct this doc (§2.1 HUNT/LURE
M = 1, §2.5 missile-shield registration, §3.8 jammer side, §3.3 0x48eb10).
1. ~~Info-table constructor defaults~~ — resolved [H]: static init 0x18750 sets M1 B−1 X1 U0 F0; **HUNT and LURE
   have M = 1** (active = slot byte), AVOID_DANGER B = −1. TACTICAL_GAPS §1.1.
2. ~~`Active_By_Default`~~ — resolved [H]: 0x3a82a0 at creation, team spawn, AI free-store and task-force
   release. TACTICAL_GAPS §1.2.
3. ~~Autofire driver~~ — resolved [H]: in the sim, `Issue_Auto_Target` → 0x37d280 (synced pick when several);
   AI players autofire every User_Input ability. TACTICAL_GAPS §1.3.
4. ~~Service intervals~~ — resolved [H]: 0 / 1 / 3 frames per class. TACTICAL_GAPS §1.4.
5. ~~Readers of tractored / interdict / jammed / `Target_Damage_Increase_Percent`~~ — resolved [H]: tractored
   Lua-only; interdict = retreat-permission tests; 0x35b4d0 = local radar; the increase is unused tactically.
   TACTICAL_GAPS §1.5.
6. ~~`Is_Jammed` argument~~ — resolved [H]: the projectile's target; the jammer stamps **non-hostile** objects.
   TACTICAL_GAPS §1.5.
7. ~~Converted-decay bounds~~ — resolved [H]: `Get_Float(−d/10, +d/10)`. TACTICAL_GAPS §1.6.
8. ~~Passive emitters~~ — resolved [H]: registered at creation / load (MISSILE_SHIELD users too) and on each
   jammer apply. TACTICAL_GAPS §1.7.
9. ~~BurningBehavior interval~~ — resolved [H]: 30 frames, per-second damage once per service. TACTICAL_GAPS §1.8.
10. ~~`Destruction_Survivors` / spin-away~~ — resolved [H]: DEATH Initialise 0x58cdb0 and world removal
    0x391310, with synced draws. TACTICAL_GAPS §1.9.
11. ~~Class internals~~ — resolved for every tactical class the FoC / TR data uses [H/M]. TACTICAL_GAPS §2.3;
    handler bodies (DEPLOY, JET_PACK, BARRAGE, SPREAD_OUT, …) §2.1, HUNT / BARRAGE / SELF_DESTRUCT behaviours §2.2.
12. ~~Land stun path and detonation effects~~ — resolved [H dispatch, M effects]. TACTICAL_GAPS §1.10.
13. Missile shields turning away LASER/DEFAULT projectiles is read from the dispatch only — worth an in-game probe
    (Interdictor missile shield vs a laser frigate), prepared under `tools/probe_mod/` if needed. **Still open.**

> **Status 2026-10-30:** Answered by S9_DEATH_CLONE_SHIELDS.md §2 (status row 2): lasers are tested but not turned in vanilla, because the deflection is a turn limited by the projectile's own `Max_Rate_Of_Turn`, which vanilla lasers have as 0. Only an optional confirmation probe remains (S9_DEATH_CLONE_SHIELDS §2.6).

14. ~~ION_CANNON_SHOT recharge~~ — resolved [H]: only a team owner with a member that fired recharges (0x705250);
    0x48eb10 is hero respawn. TACTICAL_GAPS §1.11. Hardpoint repair (command 45): TACTICAL_GAPS §1.12.

**Runtime probes worth preparing** (Gabriel launches): (a) Interdictor missile shield vs lasers (item 13);
(b) a Star Destroyer spawn with an RNG-state log before/after to confirm one draw per hardpoint; (c) Lua
`Is_Ability_Ready` on a unit with a pending tractor beam out of range.

---

## 9. Named entry points (this pass)
Names are proposals; the Ghidra project was not updated.

| RVA | Proposed name |
|---|---|
| 0x3989a0 | `GameObject::Init_From_Type` (ability slots, hardpoints, SAP) |
| 0x381a90 | `HardPoint::ctor` (initial cooldown draw) |
| 0x384740 | `HardPoint::Post_Init` |
| 0x43fab0 | `UnitAbilityTypeTable::Init_Defaults` (table 0x140b2f2e0) |
| 0x43fa70 / 0x43ead0 / 0x43fa10 / 0x43fa90 / 0x43fa30 / 0x43eab0 / 0x43fa50 | table accessors +0x20 / +0x24 / +0x29 / +0x2a / +0x2c / +0x2b / +0x28 |
| 0x43eaf0 | `UnitAbilityTypeTable::Get_Icon_Name` |
| 0x3986b0 | `Has_Unit_Ability` |
| 0x397d90 | `Get_Unit_Ability_Record` |
| 0x39b480 | `Can_Activate_Unit_Ability` |
| 0x39b950 | `Is_Unit_Ability_Active` |
| 0x38a350 | `Set_Unit_Ability_Active` |
| 0x3a4250 | `Toggle_Multiplier_Mode` (DEFEND/TURBO/P2W/SPOILER_LOCK/LEECH) |
| 0x3a2240 | `Toggle_Interdict` |
| 0x3a4070 | `Toggle_Behavior_Ability` |
| 0x3a97e0 | `Countdown_Start_Or_Cancel` |
| 0x42f910 | `AbilityCountdown::Update` |
| 0x42fae0 | `AbilityCountdown::Set_Duration` |
| 0x42fde0 | `AbilityCountdown::Start_Active` |
| 0x42fb70 | `AbilityCountdown::Start_Recharge` |
| 0x42f320 | `AbilityCountdown::Deactivate_Early` |
| 0x42f460 | `AbilityCountdown::Expire` |
| 0x42f8e0 / 0x42f7a0 | `Is_Counter_Zero` / `Is_Recharging` |
| 0x42fa70 | `AbilityCountdown::Set_Frames` |
| 0x430270 | `SetUnitAbilityModeEvent::Execute` |
| 0x429960 | `TacticalSpecialAbilityEvent::Execute` |
| 0x4b3860 | `SetAbilityAutofireEvent::Execute` |
| 0x3a78b0 | `Set_Ability_Autofire` |
| 0x39b310 | `Is_Ability_Autofire` |
| 0x39bbd0 | `Black_Market_Allows_Ability` |
| 0x565de0 | `Targeting_Try_Pending_Ability` |
| 0x37b8a0 | `SAP_Activate_By_Name` |
| 0x37bd50 | `SAP_Find_By_Name` |
| 0x37c270 | `SAP_Style_Allowed_In_Mode` |
| 0x37be10 | `SAP_Is_Enabled` |
| 0x37c380 | `SAP_Accepts_Target` |
| 0x37b580 | `SAP_Trigger_By_Style` |
| 0x37d160 | `SAP_On_Take_Damage` |
| 0x37da80 | `SAP_Update` |
| 0x48cca0 / 0x48e920 | `SpecialAbility::Begin_Effect` / `End_Effect` |
| 0x48d820 / 0x48e100 | `SpecialAbility::Target_In_Range` / `Must_Close_Range` |
| 0x48e290 | `SpecialAbility::Target_Type_Allowed` |
| 0x55e100 / 0x55f120 / 0x55e330 | SAP effect register / unregister / lookup |
| 0x3fe810 | `ModifierList_Set_By_Source` (replace-or-append, duration −1 = permanent) |
| 0x710130 / 0x710280 / 0x710710 / 0x710880 | tractor apply / slow / update / remove |
| 0x704980 / 0x704b50 / 0x704f50 / 0x705250 | ion shot apply / start / update / remove |
| 0x397e00 / 0x394eb0 | `Get_Ability_Projectile_Override` / `Collect_Weapon_Projectiles` |
| 0x3a06a0 | `On_Shot_Fired` (ion/lucky one-volley, stealth break) |
| 0x706320 / 0x7064e0 / 0x706850 / 0x7069e0 | lucky shot apply / start / update / remove |
| 0x6f5370 / 0x6f5550 / 0x6f5d50 | concentrate fire apply / start / remove |
| 0x6f9ad0 / 0x6f9e60 | energy weapon apply / update |
| 0x5926e0 / 0x592e80 / 0x592ae0 / 0x592d00 | stealth apply / update / detect / break |
| 0x39b140 / 0x3979d0 | `Is_Stealthed` / `Get_Stealth_Detection_Range` |
| 0x719000 / 0x719620 | sensor jamming apply / update |
| 0x39b2f0 | `Is_Jammed` |
| 0x7161b0 / 0x716730 / 0x716230 | laser defense apply / update / zap |
| 0x4c6e30 | `Missile_Shield_Deflect_Test` |
| 0x4d07e0 | `Unregister_Missile_Shield_Emitter` |
| 0x469280 | `Parse_Projectile_Category` |
| 0x605c90 | `Lua_Is_Affected_By_Missile_Shield` |
| 0x62b920 | `IonStunEffect::Apply` |
| 0x4cb070 / 0x4cbbb0 | `Projectile::On_Detonate` / `Ion_Stun_Radius` |
| 0x56bfd0 / 0x56c030 / 0x56bee0 / 0x56be20 | energy pool interval / regen / pay shot / pay shield |
| 0x6241b0 | `Burning::Service` |
| 0x39bdb0 | `Die` (full sequence §4.9) |
| 0x3718f0 | `Pick_Death_Clone` |
| 0x58d430 | `DeathBehavior::Service` (fade / spin-away) |
| 0x396470 | `Get_Min_Health_Fraction` (Lua `Get_Hull`) |
| 0x3976f0 | `Get_Shield_Fraction` |
| 0x385620 | `HP_Allows_Opportunity_When_Idle` (corrected rule §4.2) |
| 0x570a50 / 0x57b580 / 0x57b2a0 / 0x576b70 / 0x57f430 | Lua ability functions (§5) |
