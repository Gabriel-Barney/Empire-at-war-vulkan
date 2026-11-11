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
| 2.4 | ROCKET / MPTL flight from launch to detonation: spline maths, initial path (0x4c8b90 / 0x4c91a0), per-step move, shield re-plan (0x4cceb0 / 0x4cdb00), synced draws | [H] (2026-10-31); fire-time aim point and muzzle transform referenced, not re-read [M] | not needed |

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

*Corrected s78n t26, 2026-11-10 [H] (MOVEMENT §3.2.1):* the flag means **walking while deployed**. It is set by
0x3ac100 when a deployed or deploying unit whose type has `Max_Dist_Walk_While_Deployed` > 0 gets a move order whose
path is shorter than that distance (else the unit undeploys), by Walk vf +0x148 `0x61e570` on the same length test,
and by the 0x19 branch that starts a path. It is cleared when the walk ends (0x16 → 0x19 at once) and during 0x1A.
Only `Destroyer_Droid` (150.0, and its variant `Destroyer_Droid_Attack`) has the field in FoC. So the remap applies to
**deployed** walkers (state 0x19) and to a droid **walking deployed**; a unit that is merely deploying (0x18, flag
clear) is not remapped. For the droid the same test gates its shield (`Shielded_Only_When_Deployed`).

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
- **Behaviour strip** [H] (2026-11-02): between the animation copy and the DEATH attach, every behaviour of the clone
  except HIDE_WHEN_FOGGED (0x19), IMPOSING_PRESENCE (0x40) and LAND_OBSTACLE (0x14, only when the clone type's
  `Death_Clone_Is_Obstacle` +0x92 is set; default true) is detached (vf15) and removed (0x3a54c0). So a clone never
  targets, fires, moves, spawns, shields or holds a space obstacle entry, whatever its type lists. Consequences for
  passability and space occupancy: MOVEMENT §1.3 "Death clones and wrecks", §2.2.
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
Rockets fly a spline (the whole flight from launch to detonation: §2.4.10). The shield does not turn them: on a
**crossing** into an emitter's sphere the whole spline is re-planned (land 0x4cceb0, space 0x4cdb00) into a path
that bends around the emitter and runs on. Lua
`Is_Affected_By_Missile_Shield` (0x605c90, categories 4 and 6 only) describes this path. Re-read 2026-10-31 (refixf
t01) from the Ghidra export plus llvm-objdump of the call sites (the export drops the emitter argument of both
re-plan calls: it is passed in r8).

Notation in this section: P = rocket position (obj +0x78), E = emitter position, `fn(v)` = the fast normalise
0x16f570 (FIGHTER_FLYING §1.3; a zero vector is left unchanged), CD / CO = `Projectile_Rocket_Curve_Distance` (type
+0x1ff0) / `Projectile_Rocket_Curve_Offset` (+0x1ff4), MFD = `Projectile_Max_Flight_Distance` (+0x4bc), h(x, y) =
terrain heightmap height (0x135140 / 0x135350, bilinear; the two differ only in call shape [M]). All arithmetic is
float (no double ops in any routine below except the 1e-6 tests of §2.4.3). Sums of three squares are written in
the order the code adds them where it is not x, y, z.

#### 2.4.1 Projectile data used (pack = obj +0xe8)
Names in quotes are the labels of the retail debug dump of the projectile data (0x4eea10, "Projectile Data :"),
which settles their meaning [H].
| pack + | Meaning |
|---|---|
| 0x18 | "MissileTargetObject" (pointer; +0x20 its hardpoint index): the MISSILE homing target. Spawn sets 0 / −1 for every other category, and every rocket (re-)plan zeroes it again, so it is always null for rockets |
| 0x30 | `SplineClass*`, the path (§2.4.3) |
| 0x38 | "RocketDistanceAlongSpline": s, distance flown along the current path |
| 0x3c..0x44 | "FireAtPosition": the target point (§2.4.10) |
| 0x50 | "PathRedirectedForMissileShield": byte, cleared just before each re-plan call, set to 1 by the re-plan (also when it bails). Its only reader is the debug dump: **no sim effect** |
| 0x54 | "RocketMaxFlightDistance": detonation budget for `Explode_When_Reached_Target_Radius` (type +0x7e): the move step (0x4c7050) detonates a category 4/6/8 projectile with that flag once s ≥ +0x54 |
| 0x5c / 0x6c | "DistanceTravelled" (+= speed every move) / "MaxTravelDistance" (§2.4.10) |
| 0x74..0x7c / 0x8c | "InitialFacing" / "CreationFrame" |
| 0x80..0x88 | launch point (the spawned object's position, Spawn_Projectile 0x54ade0) |
| 0x90 | "Speed" per step |
| 0x98..0xa0 | "LastKnownVelocity": last move's displacement × the frame-time float 0x140b0a344 (written by every move, 0x4c7050). The re-plan's direction D = fn(this) |
| 0x48 | render copy of the spline, rebuilt after a (re-)plan; visual only |

#### 2.4.2 Per-step order (0x4cfc40)
1. No path (+0x30 null) and a target point set (not all three −1e18): initial plan, mode 1 (land) 0x4c8b90, mode 2
   (space) 0x4c91a0 (§2.4.10). This runs on the rocket's first flight step, before step 3 turns its facing.
2. s += speed. Sample the path at s (0x22fe70) → next point N. If the sample fails (s past the end), the rocket
   expires where it is (0x4c7050 with "force expire"): detonation effects at P; the step ends.
3. `Turn_Toward` (0x3abe40) the angles P → N with rate −1 (type turn rate, §2.3). This only turns the facing: the
   position always comes from the spline [M: facing not read by this path].
4. Emitter loop over the **whole** global emitter list in order, with the §2.3 qualification and radius R (no
   jamming skip). Crossing = |N − E|² ≤ R² **and** R² ≤ |P − E|² (squares summed y, x, z). On a crossing: clear
   +0x50, then mode 1 → land re-plan(P, emitter), mode 2 → space re-plan; any other mode returns from the step.
   The loop then continues with the **same** P and N, so two crossings in one step re-plan twice (the second sees
   s = 0 and the first one's new path is discarded).
5. 0x4d0080(N): sweep P → N, hit processing, move to N. **On a re-plan step the rocket still moves to N on the old
   path.** The next step samples the new path at s = speed, measured from P, so that step's distance is flown again
   on the new path.

So a re-plan fires on every step that enters an emitter's sphere, with no count limit; leaving and re-entering
fires again.

#### 2.4.3 `SplineClass` (vtable `SplineClass::vftable`, ctor 0x22fa30, 200 bytes)
- Control points: vector of (x, y, z) at +0x10/+0x18. Append 0x22fc80; erase index i 0x22fcc0 (later points shift
  down); count 0x22fe40.
- **Build** 0x22ffc0(spacing, 0): with fewer than 3 control points it returns at once and **changes nothing**
  (earlier coefficients and lengths stay). Otherwise, per axis, a natural cubic spline over the n control points
  y[0..n−1] (0x230950):
  - γ0 = 0.5; γi = 1 / (4 − γ(i−1)) for i = 1..n−2; γ(n−1) = 1 / (2 − γ(n−2)).
  - δ0 = ((y1 − y0) × 3) × γ0; δi = ((y(i+1) − y(i−1)) × 3 − δ(i−1)) × γi for i = 1..n−2;
    δ(n−1) = ((y(n−1) − y(n−2)) × 3 − δ(n−2)) × γ(n−1).
  - K(n−1) = δ(n−1); Ki = δi − γi × K(i+1) for i = n−2 down to 0.
  - Segment i (0..n−2): a = y[i], b = Ki, c = ((y[i+1] − y[i]) × 3 − Ki × 2) − K(i+1),
    d = ((y[i] − y[i+1]) × 2 + Ki) + K(i+1). Value at u: ((u × d + c) × u + b) × u + a.
  - Segment length Li = len(i, 0, 1) (0x2313b0, adaptive): chord = |p(t0) − p(t1)|; if chord < 1e-6 return chord.
    Else m = (t0 + t1) × 0.5, a = |p(t0) − p(m)|, b = |p(t1) − p(m)|; if |chord / (b + a) − 1| ≤ 1e-6 or
    |chord − (b + a)| ≤ 1e-6 (both tested as double against 1e-6) return b + a, else
    return len(i, t0, m) + len(i, m, t1). Total (+0x88) = Σ Li added in index order.
  - **Polyline table** (+0x90, 7 floats per entry: x, y, z, three derivative slots left unset with flag 0, and the
    length from the start). Entry 0 = segment 0 at u = 0. Then for each segment i:
    c = (int)ceil(Li / (float)spacing), clamped to 1..100, and entries at u = (float)j / (float)c for j = 1..c
    (cubic values as above). Mostly for rendering, but the **space initial planner reads its last two entries**
    (§2.4.10), so `spacing` (10 there) matters in that one place. 0x2308d0(i) reads entry i.
- **Sample** 0x22fe70(s): walk segments in order with cum = 0; at the first i with s < Li + cum:
  u = (s − cum) / Li, point = the three cubics at u, returns true and the segment index i. Else cum += Li. False
  when no segment matches (s ≥ total) or the path was never built. A negative s lands in segment 0 with u < 0.

#### 2.4.4 Jittered straight leg 0x4c9990(from, to, amp, lenScale, obj, mode, groundEnd, plainEnd)
Appends points to the path from `from` (already the last control point) to `to`.
1. amp' = amp × CO; step length = lenScale × CD. d = from − to; m = (from + to) × 0.5 + 1 (each component).
   c1 = m × d, c2 = c1 × d (cross products). n1 = c1 × (1 / sqrtf(|c1|²)) and n2 likewise, each only when the
   length is > 0 (else left raw); both squared lengths are summed y, x, z.
2. n = (int)ceilf(sqrtf(|d|²) / (lenScale × CD)). Nothing happens when n ≤ 0 (also CD = 0).
3. inv = 1 / (float)n; t starts at inv and **accumulates** t += inv after each point. Point k (k = 1..n):
   p = (1 − t) × from + t × to per component.
4. **Mode 0** (free flight): every point except the last (k = n) is jittered (§2.4.5); each point is appended.
5. **Mode 1** (land terrain following): box = heightmap extent (0x1320d0: centre (W·c / 2, H·c / 2), half sizes
   |W·c| / 2, |H·c| / 2; W, H, c = heightmap width, height, cell size), each half size shrunk by (amp' + 1).
   G0 = h(launch.x, launch.y), alt = launch.z − G0 (pack +0x80..+0x88). zPrev starts as the z of the path's last
   control point. For each point, tested on the **un-jittered** p:
   - outside the box (|p.x − cx| > hx' or |p.y − cy| > hy'): not appended, **no draws**;
   - inside, last point and `plainEnd`: not jittered; with `groundEnd` its z = h(p.x, p.y). Appended;
   - inside otherwise: jitter p; then z = max(0.7f × zPrev, max(p.z, h(p.x, p.y) + alt)), where 0.7f is computed
     as 1.0f − 0.3f (the float 0x140a2bba8, read only here) and h is taken at the jittered x, y. Appended.
   - zPrev = this point's z (the lerp z when it was skipped).

#### 2.4.5 Jitter 0x4c6ce0 (two synced draws)
θ = synced `Get_Float(0, 6.2831855f)` (0x1ffbb0, MAIN_LOOP §4.1) **first**, then (sin θ, cos θ) by CRT `sinf` /
`cosf` with the tiny-angle shortcut (FIGHTER_FLYING §1.1), then a = synced `Get_Float(0, amp')` **second**.
p += (cos θ × n1 + sin θ × n2) × a, per component. `Get_Float` always steps the LCG once, so **a jitter with
amp' = 0 still takes both draws**.

#### 2.4.6 Land re-plan 0x4cceb0 (mode 1)
Inputs P, E, D = fn(pack +0x98), s0 = pack +0x38 at entry, k = 20.
1. **Bail-outs** (set +0x50 = 1, path unchanged): D·(E − P) < 0 (emitter behind); or the exit point below is not
   found. Algebra [M]: with D of unit length the exit point exists exactly when D·(E − B) > 0.25, i.e. the emitter
   centre must be more than about 2k + 0.25 units ahead of P along D. D = 0 (no move yet) also bails (NaN t) [M].
2. A = P + k·D; B = A + k·D (two roundings).
3. Exit point (0x20c280, ray-sphere): O = B + 0.5·D, F = B + 1e6·D, R = F − O, len = sqrtf(|R|²) (summed y, x, z),
   Rn = R × (1 / len). Sphere: centre E, **radius r = |B − E|** (summed y, x, z), so B lies on it; it is not the
   shield radius R. w = O − E; b = (w·Rn) × 2; disc = b² − (|w|² − r²) × 4; disc < 0 → bail;
   t = ((sqrtf(disc) − b) × 0.5) / len; bail unless 0 ≤ t < 1; I = O + t·R (the far intersection: where a straight
   flight would leave the sphere).
4. A new spline replaces +0x30. Append P.
5. **Leg 1**: 0x4c9990(P → A, amp 0, lenScale 1, mode 0). ceilf(k / CD) points; 2 draws per point but the last.
6. **Arc** (no draws). U = fn(B − E), V = fn(I − E), then in this order: M = fn((V + U) × 0.5) (½),
   Q = fn((M + U) × 0.5) (¼), W = fn((M + V) × 0.5) (¾), Q8 = fn((Q + U) × 0.5) (⅛ of the arc from U to V).
   Subdivision sub(a, b) (0x4cc780): if acosf(a·b) × r ≤ thr, nothing; else mid = fn((a + b) × 0.5) and the result
   is sub(a, mid), mid, sub(mid, b) in that order (a·b summed x, y, z; CRT `acosf` 0x7823c8). Land uses
   thr = CD × 0.5 and appends, as E + r × dir: **Q8, sub(Q8, W), W** (the code subdivides from W towards Q8 and
   reads the list back to front; both orders give the same bits). Last = the last appended point (E + r·W).
7. H = fn(D.x, D.y, 0) (the original heading made horizontal).
8. **End**, type flag `Projectile_Target_Point_On_Terrain` (+0x7f):
   - **No**: rem = MFD − s0; T = Last + (rem − 20)·H (T.z = Last.z). **Leg 2**: 0x4c9990(Last → T, amp 1,
     lenScale 1, mode 1, groundEnd, plainEnd). Build(10). **Truncation**: if total > rem and sample(rem) gives
     point S in segment j with j < count: erase control points j..end (this also drops point j, the start of S's
     segment), then if |S − E|² > r² (summed y, x, z) set S.z = h(S.x, S.y) − 5; append S.
   - **Yes**: q = (r − |Last − E|) + 20 (summed y, x, z); append (Last.x + H.x·q, Last.y + H.y·q,
     h(that x, y) − 5). No leg 2, no draws.
9. **Common end**: if `Explode_When_Reached_Target_Radius` (+0x7e): +0x54 −= s0 (the remaining budget carries
   over). Build(20). +0x18 = 0, s = 0, +0x50 = 1. Render copy rebuilt.

#### 2.4.7 Space re-plan 0x4cdb00 (mode 2): differences from §2.4.6
- k = **30**. Bail-outs, exit point, leg 1 (amp 0, mode 0) and the U/V/M/Q/W/Q8 directions are the same.
- Arc: thr = **CD** (not × 0.5) and the subdivision runs **from U** (point B) to W. Appended, as E + r × dir: the
  list Q8, sub(U, W), W with its **first two entries dropped when it has more than two** (Q8 and the first point of
  sub(U, W)). With sub(U, W) empty the arc is Q8, W.
- End: always the leg-2 branch, with T = Last + ((MFD − s0) − 30)·H (H horizontal here too), 0x4c9990(Last → T,
  amp 1, lenScale 1, **mode 0**, no groundEnd, plainEnd). Build(10). Truncation as land but at **MFD** (not
  MFD − s0) and **without** the ground drop. Target_Point_On_Terrain is not read.
- Common end as land, but the final build spacing is 10.

Quirks kept as read [H]: the land truncation distance (MFD − s0) and the space one (MFD) ignore earlier re-plans
(s0 restarts at 0); when MFD − s0 < k the end leg points **backwards**, and a truncation at a negative distance
leaves fewer than 3 control points, so the final build keeps the coefficients of the build before the truncation.

#### 2.4.8 Synced draws per re-plan
All on the synced LCG through `Get_Float` (0x1ffbb0), two per jittered point (θ then a), in path order:
| Part | Draws |
|---|---|
| Leg 1 (both modes) | 2 × (ceilf(k / CD) − 1) |
| Arc, truncation, ground point | 0 |
| Leg 2, land, Target_Point_On_Terrain = No | 2 per leg-2 point except the last, **only for points inside the shrunk heightmap box** |
| Leg 2, land, Target_Point_On_Terrain = Yes | none (no leg 2) |
| Leg 2, space | 2 × (n − 1), n = ceilf(|T − Last| / CD), even when CO = 0 |
Draws are taken before the truncation, so points that the truncation later erases still drew.

Vanilla (FoC XML, all 10 rocket types have CD ≥ 50, so leg 1 is one point and draws nothing):
- Land with Target_Point_On_Terrain (`Proj_MAL_Concussion_Missile`, both `Proj_Ground_Proton_Torpedo*`): **no
  draws**; the path bends around the emitter, then dives to 5 below the ground about 20 units past the ¾ point.
- Land without it (`Proj_Dark_Trooper_Missile`, `Proj_Plex_Missile`, `Proj_T4B_Missile`,
  `Proj_MAL_Carbonite_Missile`; CD 50, CO 5–15): leg 2 of length MFD − s0 − 20 in 50-unit steps → about
  2 × (MFD − s0 − 20) / 50 draws.
- Space battles (the mode decides, not the type; e.g. `Proj_Drunk_Missile` CD 400 / CO 180, both
  `Proj_Ship_Diamond_Boron_Missile*` CD 500 / CO 0): about
  2 × (MFD − s0 − 30) / CD draws, taken even with CO 0.

#### 2.4.9 What the port does today, and open items
Port (`engine/tactical/combat/src/combat_system.cpp`, `CombatSystem::rocket_replan` and the projectile step):
- Rockets fly **straight** from launch; there is no spline, no initial plan, no s / +0x54 bookkeeping.
- Crossing test: same inequalities, but N = p.pos + p.dir × speed is recomputed per emitter with the direction
  already changed by earlier emitters (the original keeps one N for the whole loop), and emitters with R ≤ 0 are
  skipped.
- On a crossing the port snaps p.dir to normalize(P − E) (straight away from the centre) and moves along it in the
  same step, every time. The original bails when the emitter is behind or its centre is less than about 2k ahead;
  otherwise it builds the arc around a sphere of radius |B − E| on the side the rocket was passing, then runs on
  along the horizontal heading, truncated at the remaining range, and on land with Target_Point_On_Terrain dives
  to the ground.
- The port takes **no draws**; the original takes the §2.4.8 draws, so any re-planning rocket of the land-no-terrain
  or space kinds shifts the synced RNG stream relative to retail.
- On the re-plan step the original still moves to the old path's N; the next step restarts from P on the new path.

- The port has no initial path either; §2.4.10 gives it.

Open:
- Degenerate arc: a rocket aimed exactly at the emitter centre gives V = −U, so M = fn(≈0) is numerical noise
  (or zero); acosf of a dot just above 1 is NaN, which never stops the subdivision. Not seen in practice.
- The fire-time aim point and muzzle transform (§2.4.10 step 1) were not re-read here; they are the shared fire
  path (Spawn_Projectile 0x54ade0, aim 0x54a270), already used by the port for other categories [M].

#### 2.4.10 The whole rocket flight: spawn, initial path, detonation [H unless tagged]
Read 2026-10-31 (refixf t01 follow-up): Spawn_Projectile 0x54ade0, 0x4c8b90, 0x4c91a0, rotation helpers 0x4800f0 /
0x2cf9e0, polyline reader 0x2308d0, move / expiry 0x4c7050. Argument values checked in the disassembly.

**1. Spawn** (0x54ade0, categories 4 / 6 / 8):
- Facing (obj +0x84 / +0x88 / +0x8c = roll, pitch, yaw in degrees; copied to "InitialFacing") = the angles
  (0x20acd0 from the origin, FIGHTER_FLYING §1.2) of the **x axis of the muzzle transform**, normalised with
  1 / sqrtf when its length is > 0: (m[0], m[4], m[8]) of the fire-bone transform, or of the shooter's world matrix
  (obj +0x248) when no bone is used. So a rocket leaves along its launcher's barrel, not towards the target.
- FireAtPosition +0x3c..0x44 = the aim point from 0x54a270: the target object's (bone) position with lead
  (0x399450), or the ordered ground position; for categories 0 / 3 / 4 / 6 / 7 the point **after** the fire-time
  inaccuracy step 0x549f80 (aim rules: TACTICAL_COMBAT §3.1.1 "Own-weapon aim point", 2026-11-02).
- Speed +0x90 = 0x370f00(type, 0) (TACTICAL_COMBAT §3.3). DistanceTravelled = 0. Launch point +0x80..0x88 = the
  spawn position. +0x18 = 0, +0x20 = −1.
- MaxTravelDistance +0x6c = (spawn override if > 0, else MFD) + the target's radius term (0x397780, plus its
  container's when the target is garrisoned) + |aim z − launch z| [H; M on what 0x397780 measures].

**2. Initial path, land (0x4c8b90, mode 1).** P = launch position, T = FireAtPosition, Y / Pi = facing yaw / pitch
(obj +0x8c / +0x88, degrees × 0.017453292).
1. +0x54 = MFD. dh = sqrtf(((P.y − T.y)² + (P.x − T.x)²) + 0.0); L1 = dh × 0.5. (`Projectile_Rocket_Straight_
   Distance` is **not** read on land.)
2. A new spline replaces +0x30. Append P.
3. If L1 ≠ 0: f = the facing direction (cos Y · cos Pi, sin Y · cos Pi, −sin Pi), built as the identity matrix
   rotated by 0x4800f0(Y) then 0x2cf9e0(Pi) (tiny-angle rule of FIGHTER_FLYING §1.1; the extra ×0 terms in the
   code add exact zeros). K = P + f × L1. **Leg 1**: 0x4c9990(P → K, amp 0.1, lenScale 1.1, mode 1, groundEnd no,
   plainEnd **no**): every point inside the box, the last one included, is jittered and height-clamped.
   If L1 = 0, K = P and there is no leg 1.
4. **Leg 2**: 0x4c9990(K → T, amp 1, lenScale 1.1, mode 1, groundEnd = `Projectile_Target_Point_On_Terrain`,
   plainEnd yes).
5. If `Explode_When_Reached_Target_Radius`: Build(10), then +0x54 = the built total length (launch → target).
6. If **not** Target_Point_On_Terrain: Build(20); H = fn(T.x − K.x, T.y − K.y, 0); e = MFD − total;
   X = T + e × H (X.z = T.z). **Leg 3**: 0x4c9990(T → X, amp 0.3, lenScale 1.1, mode 1, groundEnd yes, plainEnd
   yes); it starts at T exactly, whatever leg 2's last point was. Build(10). Truncation at **MFD**: if
   MFD < total and sample(MFD) gives S in segment j < count, erase control points j..end, set S.z = h(S.x, S.y)
   (no −5) and append S.
7. Build(10). +0x18 = 0, s = 0.

**3. Initial path, space (0x4c91a0, mode 2).** Same as land except:
- L1 = min(dh × 0.5, `Projectile_Rocket_Straight_Distance` (type +0x1fec)) (`minss`: the Straight value wins
  ties).
- All legs use **mode 0** (no terrain, no box; the last point of each leg is never jittered): leg 1 (amp 0.1,
  lenScale 1.1), leg 2 (amp 1, lenScale 1.1).
- The extension is built only **without** `Explode_When_Reached_Target_Radius` (with it the path ends at the
  target and +0x54 stays MFD). Build(10); with c = the polyline entry count (§2.4.3), A = entry c − 2 and B = entry
  c − 1 (x, y, z); e = MFD − total; X = T + e × (B − A) — the last polyline step, **not normalised** (about 10 units
  long, so X lies about 10 × e past the target). Leg 3: 0x4c9990(T → X, amp 0.3, lenScale 1.1, mode 0). Build(10).
  Truncation at MFD as land, **no** height change. Target_Point_On_Terrain is not read.
- Build(10). +0x18 = 0, s = 0.

**4. Synced draws of the initial plan** (§2.4.5: θ then a, two per jittered point, in path order, leg by leg).
With n = ceilf(leg length / (1.1f × CD)) points per leg:
| Leg | Land | Space |
|---|---|---|
| 1 (L1 ≠ 0) | 2 per point inside the shrunk box, **last point included** | 2 × (n − 1) |
| 2 | 2 per point inside the box except the last | 2 × (n − 1) |
| 3 (land: no Target_Point_On_Terrain; space: no Explode flag) | 2 per point inside the box except the last | 2 × (n − 1) |
Draws happen before any truncation. Box and height clamp: §2.4.4 mode 1 (amp' = 0.1 × CO, 1 × CO, 0.3 × CO per
leg). Vanilla example: `Proj_Dark_Trooper_Missile` (CD 50, CO 10, MFD 330, land) at a target 300 units away,
all points on the map: leg 1 is 150 long → n = ceilf(150 / 55) = 3 → 6 draws; leg 2 about 150 → 3 points → 4
draws; leg 3 about 330 − 300 = 30 → 1 point → 0 draws; about 10 draws per launch.

**5. Every flight step**: §2.4.2. The facing is turned towards the next spline point by `Turn_Toward` with the
type turn rate; after launch nothing in the rocket path reads it [M]. LastKnownVelocity is the only direction the
re-plan reads.

**6. End of flight** (0x4c7050 after the move to N, then TACTICAL_COMBAT §3.3 detonation and destroy):
- path end: the sample at s fails → forced expiry at the current position (§2.4.2 step 2);
- `Explode_When_Reached_Target_Radius` and s ≥ +0x54 → detonate at N (land: at the launch → target length;
  space: at MFD or the path end, which is the target);
- DistanceTravelled ≥ MaxTravelDistance → expire at N (the rule for every projectile);
- a hit found by the sweep P → N (TACTICAL_COMBAT §3.4 / §3.5).

### 2.5 What this means with vanilla data [H data, H code]
Effective `Max_Rate_Of_Turn` per category (174 projectile types; `Variant_Of_Existing_Type` inheritance applied;
default 2.3):

| Category | turn = 0 | turn > 0 |
|---|---|---|
| LASER | 71 | 6: Krayt megaweapon / bombardment (×5) and `Proj_Underworld_Station_Main_Cannon`, all 0.3 |
| DEFAULT | 29 | 1: `Proj_Plasma_Space_Turret_Blast` 0.2 |
| none (−1) | 1 | 23, all detonation / self-destruct / blast-effect objects |
| MISSILE | 5 (`Proj_Ewok_Rock`, `Proj_Gungan_Energy_Grenade`, both mass drivers, `Infection_Projectile`) | 10: proton torpedoes 3.0, concussion missiles 3.0 (Bossk 5.0), `Proj_Flak_Pod` 1.0, `Proj_MDU_Rocket_Pod_Rockets` 8 |
| ROCKET / MPTL_ROCKET | — | spline path, re-planned on crossing (§2.4); the turn rate only turns the facing |

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
   `Is_Jammed` on re-acquire candidates); it does not belong to the rocket path. (The port's current rocket code
   already has no jam skip.)
7. **Rocket re-plan shape and draws** (§2.4.6–§2.4.9): `rocket_replan` snaps the heading away from the emitter
   centre with no draws. The original replaces the spline with leg 1 → arc around the sphere through B → leg 2 /
   ground point, taking 2 synced `Get_Float` draws per jittered leg point (land without
   Target_Point_On_Terrain and all space rockets in vanilla). Doing it needs the rocket spline itself (`SplineClass`,
   §2.4.3), which the port does not have.
8. **Rocket flight model is missing** (§2.4.10): the port flies ROCKET / MPTL_ROCKET straight. Retail launches
   along the muzzle's x axis and follows a jittered spline (launch-facing leg, leg to FireAtPosition, extension to
   MFD), with terrain following on land, detonation at the target length for `Explode_When_Reached_Target_Radius`,
   and about 2 synced draws per leg point at the first flight step (e.g. about 10 per Dark Trooper missile). Pack
   +0x50 ("PathRedirectedForMissileShield") has no sim reader and need not be modelled; +0x18 is the MISSILE target
   pointer (null for rockets).
