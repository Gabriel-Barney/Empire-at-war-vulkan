# S9 leftovers: death-clone key remap, missile shields vs lasers (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe: the Ghidra 12.2 export in `re_reference/`, plus llvm-objdump on
`corruption\StarWarsG.exe` where the decompiler lost arguments. This file is prose and tables only. It closes the two
S9 items left open by ABILITIES_HARDPOINTS.md §4.3 / §4.9 (PHASE4 group S9: "no death-clone remap (deploy /
infection)", "missile shields vs lasers (probe)").

Conventions:
- Addresses are RVAs (image base 0x140000000).
- Confidence: [H] read directly, [M] inferred from strong evidence, [L] guess. **probe** = needs an in-game run, which Gabriel launches; in-game probes are paused.
- "Synced draw" = the lockstep LCG 0x140a13e24 through int(lo, hi) 0x1ffb40 (MAIN_LOOP §4.1).
- GC = GameConstants (base 0x140b15800). FPS = 30. "Kind" = the `tDamageType` damage kind passed to `Take_Damage` /
  `Die` (OBJECT_TYPES §9.1: 1 Normal, 8 Cable_Attack, 11 Cable_Attack_Deployed, 12 Normal_Deployed, 15 Eat,
  20 Infection, …).

Data checked: vanilla FoC `Data\XML` (`extracted/foc_data_text.zip`).

## 0. Status

| § | Item | Status | Probe |
|---|---|---|---|
| 1 | Death-clone key: remap (deploy / infection), Damage_Type sub-key, fallbacks, synced draws | [H]; when the "deploy pending" flag is set during the deploy animation [M]; infantry alternate object list [L] | not needed |
| 2 | Missile shields vs lasers | **settled [H]: lasers are tested but not turned in vanilla**, because the deflection is a turn limited by the projectile's own `Max_Rate_Of_Turn` and vanilla lasers have 0 | optional confirmation only (§2.6) |

---

## 1. Death-clone key and clone choice

### 1.1 Where it runs [H]
`Die` (0x39bdb0) step 8 (ABILITIES_HARDPOINTS §4.9), only when the current game mode reports tactical (mode vfunc
+0xf0) and the death kind ≠ 15 (Eat). It runs after hardpoint destruction and world removal, and **before** the
death-projectile draw of step 9. Every such death spawns a clone object (0x29f270), so one object id is always
consumed. The clone is stored at obj +0x2c0.

`Die` receives, besides the kind, a **Damage_Type sub-key**: `Take_Damage` (0x3a9e30) looks up the source object's
projectile data pack (source +0xe8). When there is one, the sub-key is the index of that projectile's `Damage_Type`
id (pack +0x68, copied from projectile type +0x440) in the GameConstants `Damage_Types` list (0x2c7990: linear
search in GC +0x16b0, count GC +0x16a8; −1 when absent). A source that is not a projectile gives −1. [H]

### 1.2 The key remap in `Die` [H]
Start with key K = kind. Then:

1. **Deploy remap.** All of these hold:
   - the type `Deploys` (type +0xd1) is Yes;
   - the object has a locomotor (behaviour slot obj +0x333 ≠ −1);
   - the locomotor data pack (obj +0xa8) has its **deploy-pending flag** (+0x2b3) set, **or** the locomotor's vfunc
     +0x120 returns true. Only `WalkLocomotorBehaviorClass` implements +0x120 (0x61df40): locomotor state
     (pack +0x48) == 0x19 **Deployed** (MOVEMENT §3.2). Every other locomotor class returns false (0x13b7d0).

   Then K = **11** (`Damage_Cable_Attack_Deployed`) when kind == 8 (`Damage_Cable_Attack`), else **12**
   (`Damage_Normal_Deployed`). This is the same test as `Is_Deployed_Or_Deploying` (0x39a390), inlined.
2. **Infection remap**, only when the deploy remap did not apply: the object has an INFECTION behaviour (0x54) whose
   suspend counter (behaviour +0x3c) is 0 (0x4c3700, "active"). Then K = **20** (`Damage_Infection`).
   So a deployed unit that is also infected uses 11/12, not 20.
3. Nothing else remaps. In particular, `Damage_Force_Corruption` (5) and every other kind are used as they are.

About the deploy-pending flag (pack +0x2b3) [M]: it is set by 0x3ac100 when a `Deploys` unit with a positive
`Max_Dist_Walk_While_Deployed` (type +0x23b0) is asked to deploy. It is also set by one branch of the walk
locomotor's state machine (0x61e930) that enters state 0x12 when that distance is positive. It is cleared on the
branch that switches to state 0x19 (Deployed) and on the undeploy branch (state 0x1A). Read together, the remap covers **deploying and deployed** walkers. Undeploying (0x1A) walkers with the
flag already cleared fall back to the plain kind.

### 1.3 Existence check, then spawn lookup [H]
The per-type `Death_Clone` table (type +0xd58; hash map keyed by int, each key holds a list of clone types) is
read by `Pick_Death_Clone` (0x3718f0) with (key, sub-key, index):
- if key == 1 and sub-key ≠ −1, the key becomes **0x18 + sub-key**;
- with index −1: a list of one returns that entry; **a list of two or more takes one synced `Get_Int(0, n − 1)`**;
  an empty or missing key returns nothing (no draw).

`Die` then:
1. **Existence check**: `Pick_Death_Clone(K, −1, −1)`. If it finds nothing, K reverts to the original kind. The
   entry it returns is **discarded**, but its draw (when the list has ≥ 2 entries) is still consumed.
2. **Spawn** (0x29f270): `Pick_Death_Clone(K, sub-key, −1)`; if nothing, `Pick_Death_Clone(1, −1, −1)` (the plain
   `Damage_Normal` list); if still nothing, **the dying object's own type** is cloned. Each of these calls draws
   once when the list it hits has ≥ 2 entries.

So one death can take **up to two** synced draws before the death-projectile draw (§4.9 step 9).

| Case | Lists read, in order |
|---|---|
| not deployed, not infected, kind 1, projectile source | check: key 1. Spawn: key 0x18 + sub-key, else key 1, else own type |
| not deployed, not infected, kind k ≠ 1 | check: key k. Spawn: key k, else key 1, else own type |
| deploying / deployed, kind 8 | check: key 11; missing → K = 8. Spawn: key 11 or 8, else key 1, else own type |
| deploying / deployed, other kind | check: key 12; missing → K = kind. Spawn: key 12 (no sub-key) or the kind's path above |
| infected (not deployed) | check: key 20; missing → K = kind. Spawn: key 20 or the kind's path above |

### 1.4 How the XML keys map [H]
`Death_Clone` = `<key>, <TypeName>` (OBJECT_TYPES §3.5). The key is first converted through `tDamageType`. Otherwise
it is a name from the GameConstants `Damage_Types` list and maps to **0x18 + its index**. The FoC list ends with
copies of the `tDamageType` names (index 79 `Damage_Normal` …). The enum is tried first, so those copies never become
0x18 + index keys. Repeating a key appends to that key's list.

Vanilla keys (149 types with `Death_Clone`):

| XML key | Key | Types | Notes |
|---|---|---|---|
| `Damage_Normal` | 1 | 93 | 2 types with 2 entries and 2 with 3 (e.g. `AT_AT_Walker` _00/_01/_02) → draws |
| `Damage_Disruptor_Hand` | 0x18 + 62 = 86 | 42 | Damage_Types name; **2 entries each** (Infantry_Disruptor_Death_Clone_00/_01) → one draw when hit |
| `Damage_Infection` | 20 | 42 | `Infantry_Melt_Death_Clone_00`, reached through the infection remap |
| `Damage_Force_Whirlwind` / `_Lightning` / `_Telekinesis` | 2 / 4 / 3 | 40 / 40 / 2 | |
| `Damage_Crush` | 14 | 21 | |
| `Damage_Remote_Bomb` | 21 | 8 | |
| `Damage_Fire` | 7 | 5 | |
| `Damage_Cable_Attack` | 8 | 1 | `AT_AT_Walker` |
| `Damage_Normal_Deployed` | 12 | 1 | `Veers_AT_AT_Walker` only |
| `Damage_Self_Destruct` | 0x18 + 55 = 79 | 1 | Ewok bomber; Damage_Types name, **not** the enum's `Damage_Hard_Point_Self_Destruct` |
| (none) | — | — | No vanilla type uses key 11 |

Vanilla `Deploys` = Yes types with clones: `AT_AT_Walker`, `Veers_AT_AT_Walker`, `SPMAT_Walker`, `MPTL`.
- Deployed Veers killed by fire or normal damage → key 12 → `Veers_AT_AT_Walker_Deployed_Death_Clone`.
- Deployed Veers killed by a cable attack → key 11 missing → kind 8 missing → key 1 → `Veers_AT_AT_Walker_Death_Clone`.
- Deployed AT-AT killed by a cable attack → key 11 missing → kind 8 → `AT_AT_Walker_Cable_Attack_Death_Clone`.
- Deployed AT-AT killed by normal damage → key 12 missing → kind 1 → 3-entry list: the check draws once and the spawn
  lookup draws once more.
- An infected stormtrooper shot by a blaster → key 20 → `Infantry_Melt_Death_Clone_00` (no draw).
- An uninfected one hit by a disruptor bolt (kind 1, Damage_Disruptor_Hand) → check key 1 (missing) → spawn key 86 →
  one draw between _00 and _01.

### 1.5 INFECTION behaviour [H unless tagged]
The INFECTION behaviour (0x54, `InfectionBehaviorClass`) is attached to a victim hit by a projectile with
`Causes_Infection` (projectile +0xa7) fired by an object with the INFECTION ability (66) (`Process_Hit` 0x4cb280). A
victim that already has the behaviour is resumed instead (vfunc +0xb8: suspend counter −1, floored at 0). The behaviour's
update (0x636ae0) spreads it to nearby objects that stay in range for consecutive frames; it also resumes suspended ones.
Suspend is vfunc +0xc0 (+1). What suspends it, if anything, was not traced [L: cure / expiry]. The remap only reads
"counter == 0".

### 1.6 Clone object details [H unless tagged]
- 0x29f270 creates the clone at the dying object's transform, owned by its player. The clone gets obj +0x3a0 |= 0xc0
  and copies the dying object's current animation state (0x395590 → 0x3a92f0). When the dying object has none and
  the clone type's byte +0x93 is set, it gets animation 0 instead [M on +0x93's tag]. The `DEATH` behaviour
  (DeathBehavior) is then attached to the clone through the behaviour factory (0x4c3520).
- Infantry special case [L]: when the dying type is in category `Infantry`, the clone type lacks behaviours 0x19
  HIDE_WHEN_FOGGED, 0x40 IMPOSING_PRESENCE, 0x14 LAND_OBSTACLE and 0x55 PROXIMITY_MINE (and a 0x5051d0 test), and
  K ≠ 4 (Force lightning), the clone is created through a different object manager (mode +0x20 → +0x20). It is
  probably a lighter corpse list. Whether that path also consumes a sim id was not checked.

---

## 2. Missile shields vs lasers

### 2.1 The question
ABILITIES_HARDPOINTS §4.3 noted that the flight dispatcher sends LASER (7), DEFAULT (0) and uncategorised (−1)
projectiles to the straight mode, which runs the missile-shield test, and concluded [M] that an active shield turns
lasers away too. That reading is right about the test, but it missed **how** the projectile is turned.

### 2.2 Dispatch, re-read [H]
`ProjectileBehaviorClass` vf6 (0x4ce6a0) switches on `Projectile_Category` (type +0x1fe8; upper-cased, unknown or
absent → −1):

| Category | Mode | Shield handling |
|---|---|---|
| 1 MISSILE, target alive and not jammed (`Is_Jammed` 0x39b2f0 on the target) | homing 0x4cf870 | 0x4c6e30 test; on a hit the steering goal becomes "away from the emitter" for this step |
| 1 MISSILE, no target or jammed target | straight 0x4cec80 | 0x4c6e30 test |
| 2 BOMB, 5 STATIC_BOMB, 9 CLUSTER_BOMB | inline ballistic step | none |
| 3 GRENADE | 0x4ceea0 | none |
| 4 ROCKET, 6 MPTL_ROCKET | spline 0x4cfc40 | own loop, §2.4 |
| 8 LIGHT_SABER | 0x4cf460 | none |
| 0 DEFAULT, 7 LASER, −1 and anything else | straight 0x4cec80 | 0x4c6e30 test |

### 2.3 The emitter test and the turn [H]
- `Missile_Shield_Deflect_Test` (0x4c6e30) walks the global emitter list in order. An emitter qualifies when it is
  **not allied** with the projectile's owner (0x39a230 → 0x2823e0; a controlled object uses its controller's
  player). It must also have MISSILE_SHIELD (32) active (`Is_Ability_Active` 0x39b950), or a
  `Passive_Missile_Shield_Radius` > 0, or active jamming (0x38ecf0: SAP +0x378 > 0).
  - Radius: the passive radius, overridden by ability 32's `Effective_Radius` when the unit has that ability, then
    by ability 63's `Effective_Radius` when it is jamming.
  - The first emitter with |p − e|² < r² returns the yaw/pitch of the outward direction (p − e; +x if p = e).
- Both callers then call **`Turn_Toward` (0x3abe40) with rate −1**. A rate ≤ 0 means "use the type's turn rate"
  (0x372560): `Max_Rate_Of_Turn` (type +0x38c, default 2.3) × the mode multiplier
  `Object_Max_Speed_Multiplier_{Space,Land}` (GC +0x15c0 / +0x15c4, FoC 1.2 / 1.56) × 1.0. The multipliers are
  skipped when type flag (+0x790)→byte1 bit0 is set.
  - The pitch delta and the yaw delta (each wrapped to (−180, 180]) are **clamped separately** to ±rate degrees.
  - The new orientation is then set (0x3a8710). The straight / homing step moves the projectile along that
    orientation (`Max_Speed` per step).
- So the shield **steers the projectile outward at most `Max_Rate_Of_Turn` × multiplier degrees per step** while it
  is inside the sphere. The heading is not snapped. **A projectile whose turn rate is 0 keeps its heading and is
  not deflected at all.**

### 2.4 ROCKET / MPTL_ROCKET (0x4cfc40) [H unless tagged]
The rocket's spline position advances. Then every emitter is checked with the same qualification and radius rules
as §2.3, but with a **crossing** test: the next spline point is inside the sphere (≤ r) and the current position
is outside (≥ r). On a crossing the spline is re-planned through game-mode-specific builders: mode 1 → 0x4cceb0,
mode 2 → 0x4cdb00 [M: land / space; new path shape not read]. The loop does not stop at the first emitter. The
rocket's turn rate is not used here. Lua `Is_Affected_By_Missile_Shield` (0x605c90, categories 4 and 6 only)
describes this path.

### 2.5 What this means with vanilla data [H data, H code]
Effective `Max_Rate_Of_Turn` per category (174 projectile types; `Variant_Of_Existing_Type` inheritance applied;
default 2.3):

| Category | turn = 0 | turn > 0 |
|---|---|---|
| LASER | 71 | 6: Krayt megaweapon / bombardment (×5) and `Proj_Underworld_Station_Main_Cannon`, all 0.3 |
| DEFAULT | 29 | 1: `Proj_Plasma_Space_Turret_Blast` 0.2 |
| none (−1) | 1 | 23, all detonation / self-destruct / blast-effect objects |
| MISSILE | 5 (`Proj_Ewok_Rock`, `Proj_Gungan_Energy_Grenade`, both mass drivers, `Infection_Projectile`) | 10: proton torpedoes 3.0, concussion missiles 3.0 (Bossk 5.0), `Proj_Flak_Pod` 1.0, `Proj_MDU_Rocket_Pod_Rockets` 8 |
| ROCKET / MPTL_ROCKET | — | spline path; turn rate not used |

Result:
- **An active missile shield does not turn vanilla lasers.** Turbolasers, laser cannons, ion cannons and blasters all
  have `Max_Rate_Of_Turn` 0. The test runs every step but the turn is clamped to 0°.
- Proton torpedoes and concussion missiles turn outward at 3.6° per step in space (3.0 × 1.2). Inside the
  Interdictor's 750 radius that is enough to make them miss. Bossk's missiles turn at 6°.
- The few 0.2–0.3 straight shots bend by 0.24–0.36° per step: a small miss at most.
- Jamming: a jammed missile target sends MISSILE projectiles to the straight mode. The jammer is an emitter with
  ability 63's radius, so the same turn applies there.

### 2.6 Optional confirmation probe (not required)
Static reading settles the vanilla outcome. If Gabriel wants to see it, prepare a space skirmish mod later. Do not
create it now.
1. Empire Interdictor with Missile Shield switched on, stationary. Opposite it: one Rebel Nebulon-B (lasers only)
   and one Y-wing squadron (proton torpedoes), both ordered to attack the Interdictor.
2. Observe: Nebulon-B laser bolts hit the hull and shields with no visible bending. Torpedoes curve away inside about
   750 units.
3. Variant: in the mod, set `Max_Rate_Of_Turn` 3.0 on the Nebulon's laser projectile. The bolts should now bend
   away, which confirms the turn-rate gate.
4. Cleanup: remove the mod folder.

---

## Port-affecting findings
1. **Death-clone remap is missing** (`combat_system.cpp` step 8). It needs:
   - deploy → 11 (kind 8) / 12;
   - infection → 20;
   - the existence-check fallback to the kind;
   - spawn fallbacks: key → key 1 → own type (the port's "no clone" case should spawn an own-type clone and consume
     an id).
2. **Multi-entry lists draw.** `Pick_Death_Clone` takes one synced `Get_Int(0, n − 1)` per lookup that hits a
   list of ≥ 2. The existence check and the spawn lookup can **both** draw, before the death-projectile draw. Today
   the port takes the first matching entry and never draws, so the RNG stream drifts (e.g. any AT-AT death, any
   disruptor kill on infantry).
3. **Damage_Type sub-key is missing.** `battle_catalog.cpp` drops `Death_Clone` keys that are not `tDamageType`
   names (42 `Damage_Disruptor_Hand` types, the Ewok bomber's `Damage_Self_Destruct`). They should map to 0x18 +
   the index in GameConstants `Damage_Types`. `DeathEvent` / `kill_unit` need the source projectile's Damage_Type
   index next to the kind.
4. The port needs an **infected** state (INFECTION behaviour with a suspend counter, attached by `Causes_Infection`
   hits) and a **deploy-pending / deployed** query per unit for the remap.
5. **Missile-shield deflection must be a rate-limited turn, not a snap.** `CombatSystem::deflect` sets
   `p.dir = normalize(d)`. The original turns yaw and pitch each by at most `Max_Rate_Of_Turn` × mode multiplier per
   step, so vanilla lasers (rate 0) fly through. `ProjectileDesc` needs `Max_Rate_Of_Turn`. The ability test
   "lasers too" in `test_abilities.cpp` (`test_missile_shield`) expects the wrong outcome for a 0-rate laser. The
   port's homing step (re-aim with no turn limit) should use the same turn helper.
6. **ROCKET / MPTL** shields are not "passive radius only": the qualification and radius are the same as §2.3. The
   effect is a spline re-plan on crossing into the sphere, not a heading change, and every emitter is checked. The
   port's jammed-emitter skip for rockets comes from the MISSILE re-acquire scan in 0x4ce6a0 (0x1404ce8e0 tests
   `Is_Jammed` on re-acquire candidates); it does not belong to the rocket path.
