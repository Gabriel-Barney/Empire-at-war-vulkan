# Lob solver and grenade flight: slot 44 / 0x54a6f0 (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe: the Ghidra 12.2 export in `re_reference/`, plus llvm-objdump on
`corruption\StarWarsG.exe` where the decompiler lost an argument (turret elevation, §3.6). Prose and tables only.
Roadmap S8 t03 (reinstated by Gabriel 2026-11-01); TACTICAL_GAPS §2.3.5 and §3.1 point here. S8 t04 implements it.
2026-11-02 (input for S8 t06): §3.5 toss timing, §3.6 turret callers, §5.1 fire bones, §5.2 null spawns, §6.4 sticky
attach and the BOMB timer, §6.6 the terrain segment test, §8 items 14–21.

Conventions:
- Addresses are RVAs (image base 0x140000000).
- Confidence: [H] read directly, [M] inferred from strong evidence, [L] guess. **probe** = needs an in-game run.
- FPS = 30 (0x140b0a340). Speeds and gravities are **per logic frame** (`Max_Speed` is per frame, TACTICAL_COMBAT §3.3).
- "Synced draw" = the lockstep LCG 0x140a13e24. `sqrtf` = 0x776d48, `acosf` = 0x7823c8 (CRT single precision).
- Angles: object orientation is (roll, pitch, yaw) in degrees at obj +0x84 / +0x88 / +0x8c; the forward vector of
  (roll, pitch P, yaw Y) is (cos Y cos P, sin Y cos P, −sin P) (S9_DEATH_CLONE_SHIELDS §2.4.10), so a **negative pitch
  points up**.

Data checked: vanilla FoC `Data\XML` (`extracted/foc_data_text.zip`) and Thrawn's Revenge 3.x loose XML (workshop
1125571106).

## 0. Summary

| § | Item | Status |
|---|---|---|
| 1 | XML fields, defaults, vanilla / TR values | [H] |
| 2 | The solver 0x54a6f0: inputs, outputs, closed-form high arc, `Can_Lob_Slower` 0.8 steps, failure | [H] |
| 3 | Callers: slot 44, slot 43, Grenade_Attack, Remote_Bomb (toss timing, garrison), turret aim, the launch | [H]; who reads the turret "aimed" result [M] |
| 4 | Max attack range of a grenade weapon = `Max_Speed`² / (gravity × lob mod) | [H] |
| 5 | Launch (Spawn_Projectile 0x54ade0): points (fire bones), facing, speed, no shot without a solution, null-spawn bookkeeping | [H] |
| 6 | Flight 0x4ceea0 (closed-form parabola), hits, the exact terrain / water segment test (§6.6), sticking, the BOMB timer, expiry | [H] |
| 7 | Interactions: min / max range, lead, the root finder, terrain and obstacles | [H] |
| 8 | Differences from the port's stand-in (input for S8 t04) | — |
| 9 | Open items | — |

Key facts:
- **The solver is closed form, not iterative.** It solves the ballistic equation for the **high arc** at the
  projectile's `Max_Speed` under gravity G = `Projectile_Grenade_Gravity` × `Projectile_Grenade_Gravity_Lob_Mod`.
  The only loop is `Projectile_Grenade_Can_Lob_Slower`: while it is set the speed is multiplied by 0.8 and the solve
  repeated, and the **slowest speed that still reaches** wins. Every vanilla and TR grenade sets it.
- "In range" for a grenade weapon (slot 44) means **"the arc exists"**: no R + E test at all.
- No solution means **no shot**: the launch returns no projectile; the abilities refuse.
- The flight is an **exact parabola in frames since launch**, detonating on a **ground (terrain) or object hit** found
  by the normal sweep. There is no "aim height" rule.
- Lead, inaccuracy and the random target bone **only turn the heading**; distance and height come from the un-led
  target point.
- The 15-iteration root finder (0x7d68e0) belongs to circular lead (0x399450), not to the lob solver.

---

## 1. Data [H]

### 1.1 Fields (projectile type)
| Tag | Offset | Default (ctor) | Use |
|---|---|---|---|
| `Projectile_Category` | +0x1fe8 | −1 | 3 = GRENADE selects everything in this file |
| `Max_Speed` | +0x37c | 0.25 | launch speed v (per frame); read through 0x370f00(type, null), which returns it unmodified |
| `Projectile_Grenade_Gravity` | +0x1e80 | **1.05** | gravity factor |
| `Projectile_Grenade_Gravity_Lob_Mod` | +0x1e84 | **5.02** | multiplier; G = +0x1e84 × +0x1e80 |
| `Projectile_Grenade_Can_Lob_Slower` | +0x4a (bool) | 0 | enables the 0.8 speed steps (§2.3) |
| `Projectile_Grenade_Sticks_On_Collision` | +0x4b (bool) | 0 | §6.4 |
| `Projectile_Max_Lifetime` | +0x4c0 | −1 | category 3 with a lifetime > 0 expires by lifetime only (§6.5) |
| `Projectile_Max_Flight_Distance` | +0x4bc | 50 | travel budget, only when the lifetime rule does not apply |
| `Ranged_Target_Z_Adjust` (target / shooter type) | +0x4dc | 6.0 | added to the target point's z (§5.1) |

Defaults are from the GameObjectType constructor (gravity pair written as one 8-byte store 0x40a0a3d7 / 0 at +0x1e84,
0x3f866666 at +0x1e80). A value of 0 in XML is stored as 0 (the parser does not skip it).

### 1.2 Users and their effective numbers
G = gravity × lob mod; reach on flat ground R = v² / G (§4). All vanilla grenades: `Can_Lob_Slower` yes,
`Max_Lifetime` 900.

| Projectile | v | G | R = v²/G | Fired by (vanilla FoC) | `Targeting_Max_Attack_Distance` (unused for the grenade weapon) |
|---|---|---|---|---|---|
| `Proj_Ground_SPMAT_Grenade` | 30 | 1.636363636 | 550.0 | `SPMAT_Walker` (Projectile_Types; min distance 220) | 500 |
| `Proj_Empire_MDU_Concussion_Grenade` | 11.5 | 0.56 | 236.2 | `Empire_Grenade_Mortar` (turret) | 500 |
| `Proj_Merc_Concussion_Grenade` | 10 | 0.56 | 178.6 | `Hutt_Grenade_Mortar` (turret), `Underworld_Merc` (min 50) | 400 / 240 |
| `Proj_Bossk_Plasma_Grenade` | 10 | 0.56 | 178.6 | `Bossk` (second entry of Projectile_Types) | 150 |
| `Magnepulse_Blob[_Skirmish]` | 40 | 0.25 | 6400 | `Ground_Magnepulse_Cannon` (+ skirmish variant), LOBBING_SUPERWEAPON | 1500 / 1600 |
| `Proj_Sticky_Bomb`, `_Mara_Jade_`, `_Kyle_Katarn_` (sticky) | 15 | 1.0 | 225 | `Grenade_Type` of STICKY_BOMB abilities | — |
| `Proj_Remote_Bomb` (sticky) | 20 | 1.0 | 400 | `Bomb_Type` of Remote_Bomb abilities | — |

TR (loose XML over vanilla; `Config.MEG` not searched) has 40 GRENADE projectiles, mostly (v, G) = (10, 0.345)
thrown / light infantry grenades, (55, 1.636) artillery and (10, 0.01) proton / concussion launchers, all with
`Can_Lob_Slower` yes and `Max_Lifetime` 900. They are fired **both** through `Projectile_Types` (thrown infantry
grenades, thermal artillery: the solver path, §5) and through hardpoints' `Fire_Projectile_Type` (grenade, proton and
concussion launchers, some light grenades: the no-solver path, §5.4). **`Proj_Rail_Detonator`,
`HeroProj_Rail_Detonator` and `Proj_EMP_Launcher` have gravity 0 and lob mod 0 (G = 0)**; the first is fired only
from hardpoints, the other two are not referenced by the loose XML. See §2.5 for what G = 0 would do inside the
solver.

---

## 2. The solver 0x54a6f0 [H]

### 2.1 Signature
`bool Lob_Solve(const ProjectileType* T, const Vec3* from, const Vec3* to, float* out_angle, float* out_speed)`
- `out_angle` (radians, elevation above the horizontal, in [0, π/2]) is written 0 at entry and on every loop pass.
- `out_speed` is written only when a solution was found.
- Returns false at once (angle 0, speed untouched) when T's category is not 3.

### 2.2 One solve at speed v
Inputs: P = from, Q = to, G = T+0x1e84 × T+0x1e80, v = `Max_Speed` (T+0x37c), all single precision.
1. d = sqrtf((P.x − Q.x)² + (P.y − Q.y)²) (x term first); h = Q.z − P.z. Both are computed once, before the loop.
2. dd = d, or **0.0001** when d == 0.
3. v2 = v × v.
4. disc = (v2 × v2 − ((G × 2) × h) × v2) − ((G × G) × dd) × dd. **disc < 0 → no solution.**
5. den = ((h × h) / (dd × dd) + 1) × 2. den == 0 → no solution (cannot happen for finite inputs).
6. u = ((v2 − G × h) − sqrtf(disc)) / den. **u < 0 → no solution** (target too high: v² < G·h).
7. vx = sqrtf(u) (horizontal speed); θ = acosf(vx / v). Solution: (θ, v).

u is the squared horizontal speed of the arc through Q. It is the smaller root of
(1 + h²/d²)·u² + (G·h − v²)·u + G²d²/4 = 0, i.e. the **high arc** (steeper of the two launch angles); in exact
arithmetic tan θ = (v² + √disc) / (G·d). Flight time to Q is d / vx frames (not computed by the code; §6 uses it).

### 2.3 The loop (`Can_Lob_Slower`, T+0x4a)
```
best_speed = 0; best_angle = 0
repeat:
    out_angle = 0
    solve at v (2.2); on "no solution" leave the loop
    out_angle = θ; best_angle = θ; best_speed = v; result = true
    if not Can_Lob_Slower: leave the loop (result true)
    v = v × 0.8
on "no solution": result = false
if best_speed != 0: out_angle = best_angle; out_speed = best_speed; return true
return result            (false; out_angle = 0, out_speed untouched)
```
- Without the flag: one solve at `Max_Speed`.
- With the flag: speeds v, 0.8v, 0.64v, … until one fails; the **last success** is returned. The returned speed s
  satisfies s_min ≤ s < 1.25 s_min, where s_min is the minimum speed that reaches Q, so the arc is close to the
  minimum-energy arc (θ = 45° + ½·atan(h/d)). On flat ground θ lies in [45°, 70.1°).
- The yes / no answer never depends on the flag (a lower speed cannot reach further), so callers that only test for
  a solution (slots 43 / 44, the abilities) get the same answer either way.
- Examples on flat ground (h = 0); the angle depends only on d / R:

| d / R | Without the flag: θ, flight time | With the flag: speed, θ, flight time |
|---|---|---|
| 0.25 | 82.8°, 1.32 × T₀ | 0.512 v, 53.8°, 0.55 × T₀ |
| 0.5 | 75.0°, 1.29 × T₀ | 0.8 v, 64.3°, 0.96 × T₀ |
| 0.9 | 57.9°, 1.13 × T₀ | v, 57.9°, 1.13 × T₀ |

  with T₀ = R / v frames. SPMAT at 275: 75.0° / 35.4 frames / apex 257 without the flag, 64.3° / 26.4 frames /
  apex 143 with it (what retail does).

### 2.4 Determinism notes
- The float evaluation order above is the code's (C left-to-right as written). The launch path (§5) carries an
  inlined copy that hoists h², G², G·h and G·2·h out of the loop; the products are the same, so the two copies agree
  bit for bit [H].
- The loop count is data-bounded: each pass multiplies v by 0.8 and disc falls below 0 once v⁴ < G²·dd² + 2G·h·v²,
  at most about 40 passes for a target 0.0001 away.

### 2.5 Degenerate data [M: reading of the loop; never reached by vanilla data]
- d = 0 (target straight above / below): dd = 0.0001 gives θ ≈ 90° (straight up), and with the flag the speed steps
  down to about √(G × 0.0001).
- **G = 0 with `Can_Lob_Slower`**: disc = v⁴ ≥ 0 and u = 0 on every pass, so every speed "succeeds" with θ = 90°;
  v shrinks to the smallest denormal, where 0.8 × v rounds back to the same value, and the loop never ends. Without
  the flag G = 0 gives θ = 90° at `Max_Speed`. G < 0 terminates (disc → −G²d² < 0).
- G ≤ 0 also makes §4's range v² / G infinite or negative. No vanilla or TR (loose XML) unit lists such a
  projectile in `Projectile_Types`.

---

## 3. Callers [H]

| Caller | from | to | Uses |
|---|---|---|---|
| Slot 44 `In_Max_Range` 0x567b40 (TargetingBehaviorClass) | attacker position (obj +0x78) | candidate's position + its type's `Ranged_Target_Z_Adjust` (0x398440) | result only |
| Slot 43 0x5673b0 (explicit from / target) | `from` argument | target position, or its hardpoint position (0x396ce0) when a hardpoint is given, or the override point when non-zero; **+ `Ranged_Target_Z_Adjust`** in every case | result only |
| `GrenadeAttackAbilityClass` apply (vf30 0x700860) | unit position (+0x78) | target position (+0x78), **no** z adjust | result only |
| `RemoteBombAbilityClass` apply (vf30 0x719c90) | unit position | target position + `Ranged_Target_Z_Adjust` (0x398440) | result only |
| TurretBehavior can-aim 0x631a00 and aim update 0x631210 | the turret's fire bone (turret data +0xf0 indexes the bone list +0x90; falls back to the object position) | the turret's aim point | θ (§3.6) |
| `Spawn_Projectile` 0x54ade0 (inline copy) | launch point | target point | θ and speed (§5) |

All callers pick the projectile type the same way: the current weapon (type's `Projectile_Types` list +0xc40, count
+0xc48, index obj +0x324; slot 42 for the targeting slots), or the ability's `Grenade_Type` / `Bomb_Type`.

### 3.1 Slot 44 0x567b40
- No candidate → false.
- Current projectile is category 3 → `return Lob_Solve(proj, attacker pos, 0x398440(candidate), …)`.
- Otherwise → slot 43 (TACTICAL_GAPS §3.1).
So for a grenade weapon **none** of slot 43's rules apply: no R (and so no `FireRange` modifier list 8), no
footprint slack E, no ordered-ability range override, no melee redirect to a team member, no LAND_OBSTACLE aim walk,
no "zero offset is in range". The candidate itself (not its nearest member) is the target point.

### 3.2 Slot 43 0x5673b0
Called with explicit points by other code. After computing the target point (and the override point), a category 3
current projectile returns `Lob_Solve(proj, from, target point + (0, 0, Ranged_Target_Z_Adjust), …)` before any of the
R + E logic. The melee redirect (team container → nearest member, recursing into the slot) happens before this test.

### 3.3 Slot 45 (inside min range) is unchanged
Slot 45 → slot 46 0x5683a0 uses `Targeting_Min_Attack_Distance` + E as before, with no grenade branch. `Scan_Player`
accepts slot 44 && !slot 45 (TACTICAL_GAPS §3.1), so SPMAT (`Targeting_Min_Attack_Distance` 220) still refuses
targets closer than 220 + E, and the Merc closer than 50 + E.

### 3.4 Grenade_Attack apply 0x700860 (TACTICAL_GAPS §2.3.5)
Order: enabled; direct-command gate; `Target_In_Range` 0x48d820 true and `Must_Close_Range` 0x48e100 false (the
ability's own Activation_Min/Max ranges); `Grenade_Type` is category 3; **`Lob_Solve(unit pos, target pos)` true**;
then the face / stop / lock / pending-throw block. A failed solve refuses the apply (the ability is not consumed).

### 3.5 Remote_Bomb apply 0x719c90
Same shape: the range tests run against the target's container when the target is garrisoned (0x39b1a0, obj +0x2b0);
`Bomb_Type` category 3; `Lob_Solve(unit pos, 0x398440(target))` must succeed; then face / stop / lock / pending, as
Grenade_Attack. The toss itself goes through 0x71a200 → Spawn_Projectile (§5).

**Apply in full** [H] (2026-11-02): the ability must be enabled (vf5 0x719e50, which does **not** require an enemy
target); the unit's pending-ability pack is obj +0x100. Range tests 0x48d820 (`Target_In_Range`) / 0x48e100
(`Must_Close_Range`) use the target's container (obj +0x2b0) when the target is garrisoned (0x39b1a0); the solve uses
the target itself (+ its z adjust). Then: face the target (yaw only, roll 0), stop (0x3a9880), movement lock
(0x3a8e50(obj, 1)); pending +0x17c := 1 ("waiting to stop"), +0x178 := target id (obj +0x50); Disable TARGETING
(slot 9); copy the ability name to pending +0x158; start the presentation (0x48e4d0). The recharge is **not** started
here.

**Update** (vf4 0x719ee0), each service while the pending name is this ability's:
1. While +0x17c is set: wait as long as the locomotor reports it is still moving (locomotor vfuncs +0x1a8 / +0x1b0 /
   +0x1b8, or +0x1c0(0), true). Then +0x17c := 0, Disable the locomotor (slot 1), and:
   - `Toss_Anim` (+0x190) = −1 → toss at once (target looked up with 0x2aca60, then 0x71a200);
   - else play `Toss_Anim` (0x3a92f0(anim, 0xffff, 0, blend 0.1, speed 1.0)).
2. If the current animation is `Toss_Anim`:
   - when the animation frame (model +0xd0 via 0x395550) ≥ `Spawn_Frame` (+0x194, a float frame number) and the
     stored target ≠ 0x3fffff: **toss once**, then target := 0x3fffff;
   - when the animation has ended (0x39a180, last 2 frames): Enable TARGETING and the locomotor, unlock
     (0x3a8e50(obj, 0)), signal 0x29 (MOVEMENT §3.1.1 "Signal 0x29 listeners"), clear the pending name.
3. Otherwise (the animation was replaced or never started): clear pending, target := 0x3fffff, Enable TARGETING and
   the locomotor, unlock, signal 0x29 — **no toss**.

Vanilla: `Remote_Bomb_Toss_Anim` = BOMBTOSS, `Spawn_Frame` 12.0, `Spawn_Bone` = `MuzzleB_00`. The animation frame is
animation state; a port uses n × rate × speed (ABILITIES_HARDPOINTS §4.13), i.e. the toss happens 12 frames after the
toss animation starts at speed 1.0.

**Toss** 0x71a200: projectile type `Bomb_Type` (+0x188); bone = `Spawn_Bone` (+0x198) looked up on the unit's model
(0x12d2a0); spawn request = (shooter = the unit, target = the target object, bone) → Spawn_Projectile 0x54ade0 (§5).
**Only when a bomb was spawned** the PLACE_REMOTE_BOMB recharge (ability 0x40 = 64; 0x3a9840 → countdown 0x42fb70)
starts. When the target is a ground-point MARKER (obj +0x343 ≠ −1), the marker is removed (0x391310(target, 1))
[M: which removal path]. The bomb gets **no** BOMB timer: only DETONATE_REMOTE_BOMB sets it off (below).

**Garrisoned thrower** [H]: Spawn_Projectile's garrison branch (§5.1) applies first: a shooter inside a container
(obj +0x3a7) whose container type lists garrison fire bones (type +0x23a8) launches from a synced pick among those
bones that lie within 60° of the target, not from `Spawn_Bone`.

**DETONATE_REMOTE_BOMB** (0x280ec0) [H]: walks the object list in list order; for every live object of the player
whose type has `Friendly_Damage_Amount` (+0x23ec) > 0 (the stuck remote bombs):
- if it is attached (obj +0x2e0 = parent) and the parent is garrisoned (+0x3a7) → skip it (not detonated);
- if the parent belongs to the same player: the parent takes Take_Damage kind 1 of that amount, and the other members
  of the parent's team container take kind 0x15 of the same amount;
- then add the DEATH behaviour, run the projectile detonation 0x4c7f70 at the bomb's position, remove the bomb
  (0x391310).

`Proj_Remote_Bomb` has `Friendly_Damage_Amount` 200, so its sweep also sees own units (§6.2) and it can stick to them.

### 3.6 Turret elevation (TurretBehaviorClass; also used by LobbingSuperweaponBehavior 0x631fe0)
- Can-aim 0x631a00: if the turret's current weapon projectile is category 3, solve from the fire bone to the aim
  point; **no solution → cannot aim (0)**; else the elevation to test is −θ × 57.295776 (degrees, up = negative), in
  place of the geometric elevation. Then the usual limits: |rotation| ≤ `Turret_Rotate_Extent_Degrees` (+0x5a0;
  deployed: +0x5d0) and, unless `Turret_XY_Only` (+0xb3), |elevation| ≤ `Turret_Elevate_Extent_Degrees` (+0x5a4;
  deployed: +0x5d4).
- Aim update 0x631210: same solve; no solution → returns 0 (no aim this frame); else the target elevation (turret
  data +0x58) = wrap(−θ × (360 / (π × 2))) (constants 360, π, 2 at 0x1408007f4 / 0x1408007dc / 0x1408007d4, read in
  the disassembly at 0x14063148f), and the barrel then turns toward it at `Turret_Rotate_Speed` as usual.
- Vanilla grenade turrets allow 85–90° of elevation and the arc with `Can_Lob_Slower` stays below ~71°, so in
  vanilla only the "no solution" part matters.
- **Callers** [H] (2026-11-02):
  - 0x631cc0(turret, object): null → 0; else `can-aim(0x398440(object))`, its result unchanged (no solution → the
    turret cannot aim at that object).
  - 0x632050(turret, object) and 0x631fe0(turret, point; the lobbing superweapon's entry): need the turret data
    (obj +0xf0, +0x40 ≠ −1), else 0. 0x632050 first redirects the object through TARGETING 0x566aa0 when the target
    has obj +0x348 ≠ −1 and the shooter obj +0x33b ≠ −1, and aims at the result's position + z adjust. Both store
    the aim point at turret data +0x60..+0x68, set the "aim requested" byte +0xf6 := 1 and run the aim update
    0x631210. **0 (no solution) → +0xf6 := 0, return 0.** Otherwise return 1 only when the current angles have
    reached the target angles (0x386170(+0x54, +0x48)), else 0.
  - 0x632270 (turret service): runs 0x631210 again (result ignored) only when +0xf6 is 0, and after the idle time
    (type +0x5cc) resets the target angles to the rest angles (type +0x5b4.., deployed +0x5c0..).
  So no solution = "not aimed" this frame, retried every frame; the shot is held [M: the readers of the 0x632050 /
  0x631fe0 result were not re-read; they treat 0 as "not yet aimed"].

---

## 4. Max attack range of a grenade weapon (0x3749d0) [H]
`Get_Max_Attack_Range_Base(type, obj)`:
1. The first two entries of the type's ability-range list (+0x1ab0): when the unit has that ability active and its
   range (+0x108) > 0, return it.
2. If the unit's current weapon projectile (Projectile_Types[obj +0x324]) is category 3, or the type itself is a
   category 3 projectile: **return Max_Speed² / (Lob_Mod × Gravity)** of that projectile.
3. Otherwise `Targeting_Max_Attack_Distance` (+0x500), × `Garrisoned_Max_Attack_Distance_Multiplier` when garrisoned,
   × type +0x23f8 when 0x39a390 (deployed) holds.

Slot 40 (0x566ca0) multiplies this by (1 + ModifierSet list 8). So for a grenade weapon:
- **`Targeting_Max_Attack_Distance` is ignored**, and the garrison / deployed multipliers do not apply.
- The scan cube, approach / "close to range" logic, AI perception radii (AI_TACTICAL_PERCEPTION, `max attack range`)
  and the company range (A1_READINGS) all use R = v²/G × (1 + list 8).
- Acceptance (slot 44) uses the solver instead, which reaches exactly v²/G on flat ground only when the target point
  is at the shooter's height. Because the target point carries `Ranged_Target_Z_Adjust` (default 6) and the shooter
  point does not, a target on the same ground is reached to (v/G)·√(v² − 12G): SPMAT 544.0 of 550, Merc 172.5 of
  178.6. Between the two the unit is "in scan range" but slot 44 rejects; a range bonus from list 8 widens the scan
  but never the acceptance.

---

## 5. Launch: Spawn_Projectile 0x54ade0, grenade branch [H unless tagged]
The shared spawner used by WeaponBehavior (vf6 0x54d090), the abilities (Grenade_Attack 0x700a70, Remote_Bomb
0x71a200, Cluster_Bomb, Infection, Maximum_Firepower, Saber_Throw), SpecialWeapon (lobbing superweapons
0x491cd0 / 0x492010), AttackAction 0x71ed40 and 0x52a110 / 0x54a970.

### 5.1 Points
- **Fire bones of a WeaponBehavior shot** [H] (2026-11-02). The weapon data pack init 0x54cb00 builds the bone list
  `<Muzzle_Bone_Name>_00`, `_01`, … (type +0x580, default `MUZZLEA`) until a name is missing on the model: list
  pack +0x90, count +0xa0; the matching `<bone>_FLASH` meshes (pack +0xa8, hidden at init, flash countdowns +0xc8)
  and `<bone>_LIGHT` meshes (+0xd8, count +0xe8); the cycle index +0xf0 = 0 when bones exist, else −1. The
  service 0x54d090:
  - bone count < 1 → **no shot at all** (the weapon never fires);
  - i = the cycle index, which then advances and wraps (**round robin per shot**); with more than one
    `Projectile_Types` entry and more than one bone, i = the current weapon index (obj +0x324) instead;
  - request bone = list[i] when 0 ≤ i < count, else −1.

  So a WeaponBehavior shot (straight or grenade) **always launches from a muzzle bone's world translation**; the
  "no fire bone" rule below is reached only by other callers. The same holds for slot 43 / 44 straight shots and for
  grenades: neither the unit position nor position + `Ranged_Target_Z_Adjust` is used.
- **Launch point L** (shared rules, unchanged for grenades): no shooter → the request's point; no fire bone →
  the shooter's position + its own `Ranged_Target_Z_Adjust` (0x398440); one bone → the bone's world translation;
  two bones → a synced random point between them (3 draws); garrisoned shooter (obj +0x3a7) whose container type
  lists garrison fire bones (+0x23a8) → a synced pick among the bones within 60° of the target, which **replaces**
  the request's bone [H, 2026-11-02].
- **Target point Q**: with a target object → a team container is first replaced by its member nearest the launch point
  (0x404fc0; nearest the shooter's own container when the shooter sits in one with obj +0x33b ≠ −1; the hardpoint
  index is reset to −1, and no member → no shot); then Q = target position +
  (0, 0, target type `Ranged_Target_Z_Adjust`) (0x398440). Without a target object → Q = the request's ground point
  as given.

### 5.2 Solve
`(θ, s) = Lob_Solve(type, L, Q)` (inlined copy, §2.4). **No solution → the spawner returns null: no projectile, no
sound, nothing spawned** [H]. The callers' bookkeeping on a null spawn [H] (2026-11-02):
- WeaponBehavior 0x54d090: jumps to its end. No pulse / recharge update (0x54d8f0), no muzzle flash, no fire
  animation / SFX, the pack's +0x3c reset is skipped. The cooldown stays 0, so it tries again next service. The
  fire-bone cycle index has already advanced.
- Grenade_Attack throw 0x700a70: returns 0; the ability is not set active and no recharge starts. (Also when the
  projectile has no BOMB behaviour: the spawned grenade is removed and the throw fails.)
- Remote_Bomb toss 0x71a200: returns false; no recharge starts, the toss is not retried (the stored target is
  cleared), and the end of the toss animation ends the ability as usual (§3.5).
- The other spawner users (Cluster_Bomb, the lobbing superweapons, AttackAction, …) were not re-read [M].

### 5.3 Heading and the rest of the aim
The normal aim 0x54a270 still runs after the solve: random target bone (synced draw, when the target type has a
target-bone list +0xe70), lead 0x399450 with projectile speed `Max_Speed` (× FPS inside lead, TACTICAL_COMBAT §3.1
step 9; the reduced lob speed is not used), then the fire-time inaccuracy 0x549f80 (its synced draws). It yields the
facing F = angles(muzzle → FireAt point) and FireAtPosition (pack +0x3c). If the aim fails (zero point, or the check
0x54c2f0 refuses), no projectile is spawned. With `Fires_Forward` (shooter type +0x44) F = the shooter's own
orientation.

The projectile object is created at L with facing F (0x29f810), then for category 3:
- orientation := (F.roll, **−θ × 57.295776**, F.yaw) (0x3a8710), transform refreshed (0x3ac530) [M: role];
- pack InitialFacing +0x74 / +0x78 / +0x7c := (F.roll, −θ × 57.295776, F.yaw);
- pack launch point +0x80..+0x88 := L; CreationFrame +0x8c := current mode frame; **speed +0x90 := s** (the solver's
  speed; other categories store `Max_Speed`);
- MaxTravelDistance +0x6c := (request override if > 0, else `Projectile_Max_Flight_Distance`) + the target's radius
  term 0x397780 (+ its container's when garrisoned) + |Q.z − L.z|; damage, damage type, target and target hardpoint
  as for every projectile.

So **the yaw comes from the led, inaccurate FireAt point, but θ and s come from the un-led target point Q**: the
grenade comes down at horizontal distance |LQ| along the led bearing, at Q's height.

### 5.4 Hardpoint fire does not use the solver
`Fire_At` 0x3825b0 (hardpoints) spawns through 0x29f810 directly and fills the pack itself: InitialFacing = the
straight facing toward the led, inaccurate aim point (pitch included), speed = `Max_Speed`. A category 3 projectile
fired from a hardpoint therefore leaves **straight at the aim point** and the flight's gravity term (§6.1) then pulls
it below that line. With G = 0 (TR rail detonators, EMP launcher) it is a straight shot that ends by hit, ground or
lifetime.

---

## 6. Flight: ProjectileBehaviorClass category 3, 0x4ceea0 [H unless tagged]
vf6 0x4ce6a0 runs category 3 through 0x4ceea0 on every frame after the appearance frame (pack +0x60 < mode frame);
at the appearance frame itself (when the owner's +0x39f flag is clear) CreationFrame +0x8c is reset to the frame
argument, so the first flight step has t = 1 [M: assumes the frame argument is the mode frame].

### 6.1 Position: exact parabola
Per step, with t = (float)(frame − CreationFrame) and the forward vector f of InitialFacing (built by 0x20b350: yaw
about z, then pitch, then roll; f = (m0, m4, m8) = (cos ψ cos P, sin ψ cos P, −sin P) with P = −θ, so
f = (cos ψ cos θ, sin ψ cos θ, sin θ)):
- x = (t × 0 × 0.5 + (m1 × 0 + m0 + m2 × 0) × s) × t + L.x
- y = ((m5 × 0 + m4 + m6 × 0) × s + t × 0 × 0.5) × t + L.y
- z = ((m8 + m9 × 0 + m10 × 0) × s + (−(Gravity × Lob_Mod) × t × 0.5)) × t + L.z
(written in the code's operand order; the "× 0" terms add exact zeros.) I.e. **N(t) = L + s·t·f − ½·G·t²·ẑ**, evaluated
from scratch each step, never integrated. In exact arithmetic it passes through Q (or the led-bearing point at Q's
distance and height) at t = d / vx.

### 6.2 Step order
1. N = N(t) (6.1).
2. Facing := angles(current position → N) (0x20acd0, 0x3a8710): the model points along the chord.
3. `Sweep` 0x4c97a0 from the current position to N (TACTICAL_COMBAT §3.4):
   - Ground test 0x4d0630: **land mode only**; for category 3 it always runs (the "not yet past the target" skip is
     for categories −1 / 0 only): segment from the current position (obj +0x78) to N against the terrain triangles
     and the water plane (0x135840, §6.6).
   - Object test: hostile players' segment query (collision mask 7 with `Projectile_Does_Shield_Damage`, else 5),
     then the own-player variant when `Friendly_Damage_Amount` > 0 or `Causes_Infection`.
   - An object hit wins when its segment fraction ≤ the ground fraction.
4. Not sticky (or nothing hit):
   - ground hit → `On_Detonate` 0x4cb070 **at the ground hit point** (surface FX, ground detonation particle, detonate
     SFX, area damage centred on the hit point, then the on-detonation effects, TACTICAL_GAPS §1.10). No direct damage;
   - else object hit → `Process_Hit` 0x4cb280 (direct damage `Projectile_Damage` / pack +0x64 on the victim, then area
     damage etc., TACTICAL_COMBAT §3.5);
   - `Move_And_Expire` 0x4c7050(N, hit = object hit or ground hit): position := N; with a hit the projectile is
     destroyed (no second detonation); without one, §6.5.
5. Sticky and something hit: §6.4.

There is **no detonation at the aim height**: a grenade aimed at a unit (Q = centre + 6) hits the unit's collision on
the way down if it is still there; otherwise it falls on past Q until the terrain stops it. In space (no ground test)
it hits an object or expires.

### 6.3 Shields, turn rate, target
No missile-shield test, no `Turn_Toward`, no homing; the target and FireAtPosition in the pack are not read by the
flight. `Max_Rate_Of_Turn` is irrelevant (vanilla 0, MDU grenade 0.01).

### 6.4 `Projectile_Grenade_Sticks_On_Collision` (sticky and remote bombs) [H]
When the sweep found a hit and the flag (+0x4b) is set, no damage is done:
- object hit (2026-11-02 detail):
  - A = the angles of the hit normal (record +0xc, 0x20acd0), then pitch += 90;
  - bone = the bone of the hit mesh (record +0x28) on the hit object's model (0x12d520 → 0x12d730), bone 0 when
    not found;
  - the bomb model's local transform := inverse(bone world transform, 0x22d410) × [rotation R(A) | hit point
    (record +0x18)] (0x13f910). At the moment of attach its world transform is therefore exactly R(A) at the hit
    point; afterwards it follows the bone;
  - attach 0x38d320(bomb, parent, bone, zero offset), which sets obj +0x2e0 = parent;
  - when the bomb has a PROXIMITY_MINE behaviour (slot obj +0x387 ≠ −1): model flags 0x264c80(1) / 0x264670(0) —
    presentation only;
- ground hit: position := the ground hit point (0x3a8f90);
- either way the behaviour's vfunc +0xc0 is called (flight stops). The projectile then waits for its BOMB behaviour.
Nothing sticks to allies unless the type has `Friendly_Damage_Amount` > 0 or `Causes_Infection` (the object test only
sees enemies otherwise); `Proj_Remote_Bomb` has 200, so remote bombs can stick to own units (§3.5).

**BOMB behaviour** (service 0x58f210; BombDataPack at obj +0x190, ctor 0x551c90) [H]:
- pack: +8 target id (0x3fffff = none), +0xc damage amount (float), +0x10 damage kind (init 1), +0x14 detonation
  frame (0 = not armed);
- each service: when attached (obj +0x2e0) and the model exists, position := the bomb model's world translation
  (the stuck point carried by the bone, so it moves and turns with the parent);
- when detonation frame ≠ 0 and ≤ the current frame: add the DEATH behaviour (or remove via 0x391310); then with no
  PROJECTILE behaviour (slot +0x33a = −1) and a target: Take_Damage(target, kind +0x10, amount +0xc, source = the
  bomb); otherwise the projectile detonation 0x4c7f70(projectile behaviour, bomb, bomb position, 0, 0) followed by
  0x391310(bomb, 1); then reset the pack (0x551d40).
- **Arm** 0x58f030(behaviour, bomb, target, seconds, kind, amount): detonation frame := int((float)FPS × seconds +
  (float)mode frame (mode +0x10) + 0.5), plus a countdown-animation start frame (presentation).
- **There is no XML type-level timer.** The fuse comes from the arming caller:
  - Grenade_Attack throw 0x700a70 arms **right at spawn** with `Grenade_Explode_Timer_In_Secs` (ability +0x1e8;
    vanilla 8.0 / 6.0), target none, kind 1, amount 0. So the countdown runs from the throw frame, in flight too;
    a non-sticky grenade normally detonates on its ground / object hit first (§6.2) and the timer then never fires.
    On success the ability 0x16 (22) is set active and its recharge starts;
  - other armers: 0x3a3e20, Demolition 0x6401b0, Cluster_Bomb 0x718ad0 (not re-read);
  - Remote_Bomb never arms (DETONATE_REMOTE_BOMB, §3.5).

### 6.5 Expiry (Move_And_Expire 0x4c7050)
- DistanceTravelled (+0x5c) += s (the solver speed, not the arc length) every step.
- expire = DistanceTravelled ≥ MaxTravelDistance (+0x6c).
- **Replaced by** (frames since CreationFrame) / FPS > `Projectile_Max_Lifetime` (strictly greater) when +0x6c = 0
  **or** (category 3 and `Max_Lifetime` > 0). Every vanilla / TR grenade has 900 s, so grenades expire by lifetime
  only and the travel budget never applies to them.
- `Explode_When_Reached_Target_Radius` (+0x7e) has no effect on category 3.
- Expiry without a hit: lifetime detonation particle, detonate SFX, area damage centred on the projectile, the
  on-detonation effects, destroy.

### 6.6 Terrain / water segment test 0x135840 [H for the maths]
Called by the ground test 0x4d0630 as 0x135840(terrain (0x267a50(0x397680(projectile))), from = the projectile
position (obj +0x78), to = N, record). The record is the Sweep result's ground part (+0x3c): "want point" byte = 1,
best t +0x40 := 1.0, material +0x44, normal +0x48.. (initially (0, 0, 1)), point +0x54...

1. Ray: O = from, D = to − from (component-wise, **not normalised**; a unit direction, |D|² and |D| are computed too
   but the triangle and plane tests use D). It calls the terrain renderer object (terrain +0xd8, vftable 0x81b610)
   vf3 0x18f490.
2. Patch selection (0x18f490): patches of 31 × 31 cells; inv = 1 / (cell size (terrain +0x9c) × 31). Over the
   segment's XY bounding box, patch x from floor(min x × inv) to ceil(max x × inv), same for y, clamped to the patch
   counts (renderer +0x10 / +0x14). Each patch's AABB (centre +0xa8, half extents +0xb4) is ray-tested, then its
   BVH (0x1b7160): node ray / AABB test 0x20c6d0; an inner node visits the child on the near side first (by the sign
   of D · split normal) and the far child only when the near one found no hit; a leaf (0x1b92a0) is one cell, grid
   (gx, gy) = leaf cell + the patch's cell offset (+0x30 / +0x34).
3. Cell vertices (0x135a70): (gx × cell, gy × cell, (float)(int)h × height scale (terrain +0xa4)), h = the int16 at
   terrain +0x20 + 4 × (y × width + x) (4-byte samples; width +0x10, height +0x14); x / y are clamped to the grid for
   the **height lookup only**.
4. Two triangles per cell, diagonal (x, y)–(x+1, y+1): T1 = (v00, v10, v11), T2 = (v00, v11, v01).
5. Ray / triangle 0x20c410 (Möller–Trumbore, no back-face culling), in this float order, e1 = v1 − v0, e2 = v2 − v0:
   - P = D × e2 = (Dy·e2z − Dz·e2y, Dz·e2x − Dx·e2z, Dx·e2y − Dy·e2x);
   - det = P.y·e1.y + P.x·e1.x + P.z·e1.z; |det| < 0.0001 → miss; inv = 1 / det;
   - T = O − v0; u = (T.y·P.y + T.x·P.x + T.z·P.z) × inv; miss unless 0 ≤ u ≤ 1;
   - Q = T × e1 = (Ty·e1z − Tz·e1y, Tz·e1x − Tx·e1z, Tx·e1y − Ty·e1x);
   - v = (Q.y·D.y + Q.x·D.x + Q.z·D.z) × inv; miss unless v ≥ 0 and v + u ≤ 1;
   - t = (Q.y·e2.y + Q.x·e2.x + Q.z·e2.z) × inv; miss unless 0 < t < best;
   - hit: best := t; normal := normalize(e1 × e2); point := t·D + O (per component).
6. Water: when terrain +0x204 (water type: 0 none, 1 water, 2 lava, 3 ice; TERRAIN_WATER_POST) ≠ 0 and the toggle
   byte 0x140a123a1 (initialised 1, never written) is set: plane z = terrain +0x200 (0x20c180): denom = D.z,
   |denom| ≥ 0.0001, t = −(O.z − h) / denom, hit when 0 < t < best (both directions). The material comes from a
   table by water type (presentation). [M: the stored normal for a water hit is (0, 0, 1).]
7. The hit material comes from the 0x133b10 lookup (presentation: surface FX).

Result: the nearest t in (0, 1) over the grid triangles and the water plane [M: the BVH's near-first early-out equals
the global minimum, since cells are disjoint in XY and the ray is monotone along the split axis; ties aside]. A hit at
exactly t = 0 or t = 1 is not a hit. Hits from below count (no culling). Sweep 0x4c97a0 then prefers the object hit
when its fraction ≤ the ground t. Grenades therefore **detonate on the water surface**, and lava / ice planes too.

---

## 7. Interactions

| Topic | Rule |
|---|---|
| Max range | Scanning / approach: R = v²/G × (1 + list 8) (§4). Acceptance: the arc exists (§3.1). `Targeting_Max_Attack_Distance` unused |
| Min range | Unchanged: slot 45 with `Targeting_Min_Attack_Distance` + E (§3.3); abilities use their own Activation_Min_Range |
| Too high | A target with v² < G·h or a negative discriminant is out of range even when close |
| "15-step root finder" | Not part of the lob solver. 0x7d68e0 (bracket [0.1, 1000], tolerance 0.001, 15 iterations) is circular lead's flight-time solve (ABILITIES_HARDPOINTS §4.11). For a grenade it runs inside the aim (§5.3) with the straight-line speed `Max_Speed` × FPS and only changes the heading |
| Lead on moving targets | Bearing only. Elevation and speed come from the target's current position, so a target moving along the line of fire is over- or under-shot; a crossing target is led (with a straight-line flight time, not the arc's) |
| Inaccuracy, random target bone | Bearing only (they move FireAt, not Q); their synced draws still happen |
| Terrain / obstacles | The solver ignores terrain and obstacles. In flight the sweep's ground test detonates the grenade early if terrain rises into the arc, and any enemy object crossing the arc is hit. No LAND_OBSTACLE aim walk for grenade targets |
| Team targets | Slot 44 aims at the container's own position (+ z adjust); the launch redirects to the member nearest the launch point (§5.1) |
| Hardpoints | No solver: straight launch + gravity (§5.4) |

---

## 8. Differences from the port's stand-in (input for S8 t04)
The port (engine/tactical/combat: `formulas.hpp lob_direction`, `CombatSystem::spawn`, the flight block of the
commit phase, `base_attack_range`, `candidate_ok` / `promote_goal`, apps/sim `battle_catalog.cpp`) differs as follows:

1. **Speed reduction missing.** Retail steps the speed down by 0.8 while a solution exists (`Can_Lob_Slower`, set on
   every vanilla / TR grenade) and flies the high arc at that slowest speed; the port always uses `Max_Speed`, so its
   arcs are steeper, higher and slower (SPMAT at 275: 75.0° / 35 frames vs 64.3° / 26 frames). Store the solver
   speed as the projectile speed.
2. **No solution = no shot.** Retail spawns nothing (and slot 44 / the abilities refuse); the port falls back to 45°
   and fires.
3. **Slot 44 for grenade weapons.** Retail: in range ⇔ the solve from the attacker position to target position +
   `Ranged_Target_Z_Adjust` succeeds; no R, E, FireRange modifier, ability override, melee redirect or LAND_OBSTACLE
   walk. The port tests d ≤ lob_range × modifier + E. Slot 45 stays as is. The scan box keeps R = v²/G × (1 + list 8).
4. **Solve points.** Retail solves against the un-led target point (position + z adjust, container → nearest member)
   from the launch point; lead, inaccuracy and the random target bone only set the yaw. The port solves toward the
   led, inaccurate aim point in 3D. d = 0 → 0.0001 (θ ≈ 90°), not "straight at the aim point".
5. **Flight.** Retail: closed form N(t) = L + s·t·f − ½G·t²·ẑ with t = frames since CreationFrame (first step t = 1).
   The port integrates (position += velocity, then velocity.z −= g, renormalising the speed), which is lower by
   G·t/2 at every step and changes the travel counter.
6. **Detonation.** Retail: the sweep's terrain ground test (land only) → `On_Detonate` at the ground hit point, or an
   object hit → `Process_Hit`; no aim-height rule, the grenade falls past Q to the ground. The port detonates when it
   comes down to the aim point's z (`floor_z`) through the lifetime-expiry path. The port needs a terrain-segment
   test for projectiles: its sweep has no ground test for any category today; the host terrain it already holds for
   rockets (`RocketTerrain`, heightmap h(x, y)) is a starting point, but 0x4d0630 tests the segment against the
   heightmap (0x135840), not a point height.
7. **Sticky grenades** (`Projectile_Grenade_Sticks_On_Collision`: Sticky_Bomb variants, Remote_Bomb, TR Vape_Charge /
   Disruptor bombs) are not modelled: attach to the hit bone or rest on the ground, stop flying, no contact damage,
   wait for the BOMB timer / remote detonation.
8. **Expiry rule.** Lifetime replaces the travel budget for category 3 only when `Max_Lifetime` > 0; otherwise the
   travel budget (+ default MFD 50) applies even to arcs. The port drops the travel limit for every lobbed shot and,
   for G = 0 category-3 shots, uses the straight-flight travel limit instead of the lifetime.
9. **Zero and absent XML values.** Retail keeps 0 (TR `Proj_Rail_Detonator`, `HeroProj_Rail_Detonator`,
   `Proj_EMP_Launcher`: G = 0) and defaults absent tags to gravity 1.05 and lob mod **5.02** (G = 5.271). The port
   ignores values ≤ 0 and falls back to `CombatConfig::grenade_gravity` 1.0 and lob mod 1.0, so the TR rail
   detonators fall under G = 1.
10. **Hardpoint shots** (Fire_At 0x3825b0) never use the solver: straight launch at the aim point at `Max_Speed`, then
    the parabola's gravity term. The port applies `lob_direction` in the shared `spawn`, hardpoints included.
11. **Abilities** (tactical/abilities owner): Grenade_Attack apply also needs a solution (unit position → target
    position, no z adjust); Remote_Bomb apply needs one (unit position → target position + z adjust) and tosses from
    the unit through the spawner, while the port's Remote_Bomb launches from the target point.
12. **Turret elevation** (only if the port models turret pitch limits): grenade turrets aim at −θ and cannot aim
    without a solution.
13. **Guard needed [port]:** G ≤ 0 with `Can_Lob_Slower` never terminates in retail (§2.5); the port must cap or
    special-case it (e.g. treat G ≤ 0 as "no solution" in the solver, and keep hardpoint G = 0 shots straight).

Further differences found 2026-11-02 (input for S8 t06; port state as of 96c474e):

14. **Ground hit.** The port's `CombatSystem::ground_hit` samples h(x, y) in 10-unit steps (64 max) and bisects 12
    times. Retail intersects the segment (current position → N) with the two triangles of every heightmap cell it
    crosses and with the water plane (§6.6): exact Möller–Trumbore, nearest t in (0, 1), two-sided. The host must
    give combat the terrain grid (cell size, width, height, int16 heights, height scale, water height and type), not
    only a height function; the point and t come out of the triangle test, not from bisection.
15. **Water.** A non-zero water type makes the plane z = water height a hit surface for every land projectile's ground
    test (grenades detonate on water / lava / ice). The port has none.
16. **Launch point.** WeaponBehavior shots launch from the world translation of the next fire bone
    `<Muzzle_Bone_Name>_NN` (default `MUZZLEA_00`…), round robin per shot (§5.1); with no such bone the weapon never
    fires. The port launches straight shots from `u.pos` and lobs from `u.pos` + own z adjust. A port without
    animated bones should use each bone's rest-pose offset in unit-local space (rotated by the unit's orientation)
    [port approximation: retail uses the animated pose]; garrisoned shooters use the container's garrison fire bones
    within 60° of the target.
17. **Remote_Bomb** (tactical/abilities `RemoteBomb`): retail spawns the bomb from `Spawn_Bone` at `Spawn_Frame` of
    `Toss_Anim` (12 frames after the toss starts, vanilla), after the unit has stopped; it locks movement, suspends
    TARGETING and the locomotor for the toss, raises signal 0x29 at the end, and starts the PLACE_REMOTE_BOMB recharge
    **only if the bomb spawned**. The port spawns at apply and starts the recharge unconditionally. Range tests use
    the target's container when the target is garrisoned (§3.5).
18. **Sticky attach frame.** Retail carries the stuck point on the parent's hit bone (it moves and turns with the
    parent). The port stores a world offset (`stuck_offset = p.pos − parent pos`) that does not rotate. Store it in
    the parent's local frame and rotate it with the parent's orientation [port approximation: bone animation is
    ignored].
19. **BOMB timer.** Detonation frame = int(30 × seconds + spawn frame + 0.5), fired when frame ≥ it; the countdown
    starts at the throw (spawn) frame, not at the stick. Check the port's `age >= fuse` against this rounding.
20. **DETONATE_REMOTE_BOMB own-parent rule:** a bomb stuck on an own unit damages that unit (kind 1) and its team
    container's other members (kind 0x15) by `Friendly_Damage_Amount`; bombs on garrisoned parents are skipped (§3.5).
21. **Null spawns** (§5.2): a WeaponBehavior shot that spawns nothing changes no cooldown or pulse count and retries
    next service; Grenade_Attack / Remote_Bomb start no recharge.
22. [M, follow-up] WeaponBehavior has **no range gate at fire time**: the request's travel override (+0x48) is the
    max attack range (0x3749d0 × (1 + list 8)); the range test is only the targeting slots'. The port keeps an XY
    range gate in `fire_unit_weapon`; confirm before removing it.

---

## 9. Open items
- Answered 2026-11-02: the turret callers (§3.6), null-spawn bookkeeping for WeaponBehavior / Grenade_Attack /
  Remote_Bomb (§5.2), the sticky attach (§6.4), the BOMB timer (§6.4), the terrain test (§6.6), the launch point
  (§5.1), the Remote_Bomb toss (§3.5).
- The readers of the turret "aimed" result (0x632050 / 0x631fe0 callers) [M: hold fire while 0].
- Null-spawn bookkeeping of the other spawner users (Cluster_Bomb, lobbing superweapons, AttackAction) [M].
- Which removal path the Remote_Bomb toss uses for a ground MARKER target (0x391310(target, 1)) [M].
- The fire-time range gate (§8 item 22) [M].
- Whether the in-flight position can drift from Q in float (the closed form is evaluated in single precision; the
  hit is decided by the sweep, so a miss by a fraction of a unit only matters at the target's collision edge).
- **probe** (optional): SPMAT firing at a target ~275 away on flat ground should show a ~64° arc landing in about
  0.9 s; a target between 544 and 550 away should be scanned but not fired at.

## Address table
| RVA | Name |
|---|---|
| 0x54a6f0 | `Lob_Solve` (this file §2) |
| 0x567b40 | TargetingBehavior slot 44 `In_Max_Range` |
| 0x5673b0 | TargetingBehavior slot 43 |
| 0x5683a0 | slot 46 (inside min range) |
| 0x3749d0 | max attack range base (grenade: v² / G) |
| 0x566ca0 | slot 40 `Get_Max_Attack_Range` |
| 0x370f00 / 0x376990 | projectile speed getter / its modifiers (none with a null object) |
| 0x398440 | object position + `Ranged_Target_Z_Adjust` |
| 0x54ade0 | `Spawn_Projectile` (grenade branch §5) |
| 0x54a270 | fire-time aim (bone, lead, inaccuracy, facing) |
| 0x3825b0 | hardpoint `Fire_At` (no solver) |
| 0x4ce6a0 / 0x4ceea0 | projectile update dispatch / grenade flight |
| 0x4c97a0 / 0x4d0630 | `Sweep` / ground test |
| 0x4cb070 / 0x4cb280 | `On_Detonate` / `Process_Hit` |
| 0x4c7050 | `Move_And_Expire` |
| 0x700860 / 0x719c90 | Grenade_Attack / Remote_Bomb apply (vf30) |
| 0x631210 / 0x631a00 | TurretBehavior aim update / can-aim |
| 0x20b350 / 0x20acd0 | angles → matrix / direction → angles |
| 0x631cc0 / 0x632050 / 0x631fe0 / 0x632270 | turret can-aim at object / aim at object / aim at point / service (§3.6) |
| 0x54cb00 / 0x54d090 | weapon data pack init (fire bones) / WeaponBehavior service (§5.1, §5.2) |
| 0x135840 | terrain segment test (§6.6) |
| 0x18f490 / 0x1b7160 / 0x1b92a0 | renderer ray query: patches / patch BVH / cell leaf |
| 0x135a70 | cell vertex (grid x, y → position) |
| 0x20c410 / 0x20c180 / 0x20c6d0 | ray / triangle, ray / z-plane, ray / AABB |
| 0x58f210 / 0x58f030 / 0x551c90 | BOMB service / arm / pack ctor (§6.4) |
| 0x700a70 | Grenade_Attack throw (arms the BOMB timer) |
| 0x719ee0 / 0x71a200 | Remote_Bomb update / toss (§3.5) |
| 0x280ec0 | DETONATE_REMOTE_BOMB |
| 0x776d48 / 0x7823c8 | `sqrtf` / `acosf` |
