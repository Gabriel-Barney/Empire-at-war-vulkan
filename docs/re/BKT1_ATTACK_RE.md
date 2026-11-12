# BKT1: land attack staging, formation movement class, attack moves on non-hostile objects (StarWarsG.exe FoC x64 v1.121)

RE pass for backlog entries b0068, b0074 and b0075 (2026-11-12). Read from the decompiled output and, where
the decompiler lost call arguments, from the retail exe's disassembly (`llvm-objdump`) and vtable bytes.
Functions read: `0x4f6e40`, `0x40e260`, `0x440be0`, `0x441750`, `0x440f90`, `0x20acd0`, `0x37bd50`,
`0x3749d0`, `0x397780`, `0x568340`, `0x567350`, `0x567b40`, `0x5673b0`, `0x5688c0`, `0x5c8900`, `0x414720`,
`0x4779e0`, `0x477b00`, `0x477b70`, `0x50ee10`, `0x417520` (call sites), `0x43c1b0` (validation part),
`0x40edd0`, `0x57da90`, the callee lists of `0x410e90`, `0x438c10`, `0x4192b0`, `0x4165f0`, `0x5683a0`.
Ability vtables read from the exe: `SpecialAbilityClass` 0x874e28, `TacticalBribeAbilityClass` 0x8a3c08,
`ForceLightningAbilityClass` 0x8c7618, `EarthquakeAttackAbilityClass` 0x8c6908, `BuzzDroidsAbilityClass`
0x8cd470, `DemolitionAbilityClass` 0x8c64f8, `AbsorbBlasterAbilityClass` 0x8c4c68,
`ConcentrateFireAttackAbilityClass` 0x8c6120.

Context: TACTICAL_COMBAT §2.8 "Staging and movement", MOVEMENT §4.3.1 item 7 and §4.3.2, FIGHTER_FLYING §4.11,
A1_READINGS §40. Notation as there: F = formation, block = 0x68-byte move block (+0x24 kind, +0x28 object,
+0x30 hard point, +0x48 flags), T = the block's object, TD = targeting data pack (object +0xf0).
Tags: [H] read directly, [M] inferred from code with a gap, [L] best reading.

## 1. b0068: engagement range `0x4f6e40` and attack staging `0x40e260`

### 1.1 Engagement range `0x4f6e40`(unit U, block B), complete rule [H]

1. U null → 0.
2. r = `Get_Base_Attack_Range`(U's type, U) (`0x3749d0`). This is the **base** range (unit-ability mode range
   +0x108 of the first two `Unit_Abilities_Data` entries when active and > 0; grenade-weapon lob range;
   else `Targeting_Max_Attack_Distance` +0x500, times the deployed multiplier +0x23f8 when deployed, or a
   GameConstants factor 0x140b278ac when garrisoned), **not** the modified slot-40 `Get_Max_Attack_Range`.
3. **Pending-ability clause.** It applies when all of these hold:
   - TD exists, TD +0x148 (pending ability target) is non-null and TD +0x150 (pending ability name CRC) ≠ 0;
     i.e. a targeted ability is waiting for range (set only by TARGETING slot 38: an `Order_Attack` with an
     ability CRC, or the autofire driver's pick; TACTICAL_COMBAT §2.5 / §3.1 item 7);
   - U has a special-ability pack (U +0x100) and the CRC names an ability A in U's type ability list
     (`0x37bd50`: type +0x1a98..+0x1aa0, entry +0x38 = name CRC; the CRC is passed in edx, confirmed in the
     disassembly);
   - A vf +0xd8 > 0. vf +0xd8 is `0x5b0ea0` = A +0x17c = **`Activation_Max_Range`** for every targeted ability
     class read (Bribe, ForceLightning, Earthquake, BuzzDroids, Demolition); the base `SpecialAbilityClass` and
     non-targeted classes (AbsorbBlaster, ConcentrateFire) return 0 (`0x35deb0`), so the clause never applies
     to them.
   Then the function **returns min(r, `Activation_Max_Range`) at once**: no target extent is added and the
   guard cap of step 5 is **not** applied.
   - Unlike slot 43, there is **no check that the pending target is B's object**: any pending targeted ability
     of U changes U's engagement range for every block [H].
4. Otherwise, when B has an object T:
   - B names no hard point (+0x30 null): r += min(hx, hy) of `0x396850`(T) (T's hard half extents with
     deployed variants; a land squad container: its joint box; TACTICAL_COMBAT §5.4.6);
   - B names a hard point: r += `0x397780`(**T**), the object itself, not the hard point (rcx = B +0x28 in the
     disassembly). See 1.3.
5. Guard block (flag 0x02, `0x583670`(B, 2)): r := min(r, `Space_Guard_Range` 750) in space (mode 2),
   min(r, `Land_Guard_Range` 150) on land (mode 1); other modes unchanged.

### 1.2 Staging `0x40e260`(F, G) [H unless tagged]

G is the in/out goal point (the block goal point C of MOVEMENT §4.3.2). Callers: the path set-up `0x417520`,
always for a formation of type F +0x218 = 1, and for type 2 only with more than one block (TACTICAL_COMBAT §2.8).
Order of the checks:

1. The **last** block (top) must have an object T and the guard bit 0x02 clear; else return.
2. **M = the member with the largest minimum range.** Walk the members in vector order with best = −1.0. For
   each non-null member: m = its type's raw `Targeting_Min_Attack_Distance` (+0x504); m > best (strict) → M,
   best := m. Then, if that member has a pending targeted ability (same test as 1.1 step 3, CRC in TD +0x150)
   whose vf +0xd0 = `0x5b0eb0` = A +0x178 = **`Activation_Min_Range`** is > 0 and > best → M, best := it.
   So the first member wins ties, and min_s = best is the largest of all members' weapon min ranges and pending
   ability min ranges.
3. **Gate:** M must be **inside its min range** of T: `0x568340`(M, T, block hard point) = TARGETING vf +0x168
   = slot 45 → slot 46 at M's position (TACTICAL_COMBAT §5.4.2: d ≤ min + E, min = `Targeting_Min_Attack_Distance`
   or `Activation_Min_Range` when the pending ability is on T; false when min ≤ 0). False → return, G unchanged.
   A formation whose members all have min range 0 is therefore never staged.
4. **Crush case** (checked only after step 3 passed): F type +0x218 = 1, F has **exactly one member**, M has the
   **SQUASH** behaviour (behaviour id 0x26, object slot byte +0x358 ≠ −1; +0x332 + id is the slot table, so
   TARGETING 9 → +0x33b, TEAM 0x16 → +0x348), and either T's type **`Is_Squashable`** (+0x18) or M's type
   **`Is_Supercrusher`** (+0x19) and T's type **`Is_Squashable_By_Supercrusher`** (+0x1a) → G := T's position
   (+0x78..+0x80, not the hard-point point), return. No hostility test and no `UnitCollisionClass` test here
   (unlike `0x5c8900`).
5. d = min_s + 0.2 × (`Get_Base_Attack_Range`(M) − min_s) (`0x3749d0`, base range).
6. Type 1 formation (land): free-position search, 1.2.1. Other types: `0x59caa0`(…, M, G, 0, d, 0x3f, 0, 0)
   (the space ring snap), result into G [M: 0x59caa0 not re-read here].

#### 1.2.1 The stand-off point: `0x440be0` → `0x441750` → `0x440f90` [H]

Search record (A1_READINGS §40.1 offsets), as set by the staging:

| field | value |
|---|---|
| +0x18 object placed | M |
| +0x20 mode | **3** (minimum distance to +0x4c) |
| +0x24..+0x2c centre | **M's position** |
| +0x30..+0x38 orientation | `0x20acd0`(G, M position): yaw +0x38 = atan2(M.y − G.y, M.x − G.x) in degrees [0, 360), i.e. the direction **from T's point to M** (2-D); pitch is computed but unused |
| +0x3c / +0x40 cone / outside radius | ctor 2π / 1.0, unused in mode 3 |
| +0x44 first ring radius | R0 = min_s − \|M − G\| (3-D) when ≥ 0, else 0: the back-off still needed to reach min_s |
| +0x48 occupancy margin | M's `FormationSpacing` × 5 (unused, occupancy skipped) |
| +0x4c..+0x54 reference point | G |
| +0x58 minimum distance | d |
| +0x5c skip occupancy | **1** |
| +0x5e / +0x5f | 0 / 1 (reachability test on) |
| +0x00 mover list | empty, so the path test has nothing to test |
| +0xa8 movement class | M's type `MovementClass` (+0x1688) |

`0x441750` resets ring radius := R0, step := 1.0, ring := 0, sample := 0. `0x440f90`(…, record, out = G):
- Fails at once (G untouched) unless the mode is land (vf +0xe0 = 1) and the land object grid (vf +0x238) exists.
- r = M's footprint radius `0x3969f0` (√(hx² + hy²) of its hard half extents); maxStep = 1.5 r (5 when r = 0).
- Rings while ring index < **20**: n = trunc(R · 2π / r) + 1 samples, n := max(4, n) on every ring but ring 0.
  Sample i: θ = 2π i / n, point = centre + R · (cos(θ + yaw), sin(θ + yaw)), z = M's z. θ = 0 is on the T→M
  ray beyond M; later samples turn **counter-clockwise** around M.
- Tests in order, first failure rejects: (1) mode 3: 3-D distance sample → G ≥ d (reject when d > distance);
  (2) inside the map box (vf +0x1f8 centre ± half size, per axis); (3) reachability `0x50eeb0`: the sample's
  cell and M's cell both pass M's class mask and lie in the same connected region (A1_READINGS §40.2);
  occupancy skipped; path test empty. First passing sample → G := sample, success.
- After each ring: R += step; step := min(step + 0.8, min(maxStep, 10)). Ring radii from M: R0, R0 + 1,
  R0 + 2.8, R0 + 5.4, R0 + 8.8, … until the step reaches min(1.5 r, 10).
- **No sample after 20 rings → failure, G is left at T's goal point** (the staging writes nothing). The path
  then goes to T's point (re-validated by `0x50f480` when unreachable, MOVEMENT §4.3.1 item 7 (b)); the fire
  trim cannot cut it because M fails at the path end (inside min range), so the formation walks up to T [M for
  the consequence]. With r ≈ 3 (infantry) the 20 rings reach about R0 + 76; with r ≥ 6.7 about R0 + 135.
- No random draws.

**How this differs from "goal at d on the T→M line":**
1. The rings are centred on **M**, not on T, and the candidate is accepted on its distance from G, so the
   on-line candidate lands at |M − G| + R_k from G with R_k the first ring radius for which that is ≥ d: it
   **overshoots d by up to one ring step** (1, 1.8, 2.6, … ≤ min(1.5 r, 10)); it is not exactly d.
2. On a ring, the θ = 0 candidate is the farthest from G; when it is off the map or not reachable for M's
   class (impassable cell, or another region), the next candidates turn counter-clockwise around M, still
   needing ≥ d from G; then the next ring. So near a cliff or the map edge the stand-off point slides sideways
   around M rather than stopping short on the line.
3. Units and structures standing at the spot do **not** reject it (occupancy and path tests off).
4. The class used is M's own `MovementClass` (record +0xa8), not the formation class F +0x1c4.
5. The distance test is 3-D (sample z = M's z, G's own z), the direction 2-D.
6. Search failure keeps G at T (no fallback to the line point).

### 1.3 Hard-point block: what `0x397780`(T) returns [H]

- T has TEAM (behaviour 0x16) **and** a locomotor whose vf +0x230 is true (the land team container locomotor
  `LandTeamContainerLocomotorBehaviorClass`; TACTICAL_COMBAT §5.4.2 step 5) → √(HX² + HY²) of `0x396850`(T) =
  the diagonal of the squad's joint slot box.
- Else T has a type (+0x298) → `0x373b30` = `Scale_Factor` × the **soft** footprint radius
  (`Custom_Soft_Footprint_Radius`, else `Space_Obstacle_Radius`, else the larger model x / y half extent;
  MOVEMENT §5.3.1 item 6). Not the hard extents and not the hard point's own size.
- Else 0.
- This is added **instead of** min(hx, hy) when the block names a hard point (1.1 step 4). On land a block
  names a hard point for `TF.Attack_Target(obj, hpType)` and for the auto-attack Push when the object has
  hard points (FIGHTER_FLYING §4.11 B, `0x394610`).

### 1.4 Port rule (b0068)

Engagement range E_r(U, B): if U has a pending targeted ability (TD +0x148 set, CRC ≠ 0) found in its type's
ability list with `Activation_Max_Range` > 0, return min(base range(U), `Activation_Max_Range`) and stop (no
extent, no guard cap; do not require the pending target to be B's object). Else E_r = base range(U)
(`Get_Base_Attack_Range`, not the modified max range) + (B names a hard point ? radius(T) : min(hx, hy) of T's
hard half extents), radius(T) = joint-box diagonal for a land squad container, else `Scale_Factor` × soft
footprint radius; then a guard block caps it at 150 (land) / 750 (space).
Staging (last block an object, not guard): M = first member with the largest of (type min range, pending
ability `Activation_Min_Range` > 0); only when M is inside its min range of T: (a) one-member land formation,
M has SQUASH, and T `Is_Squashable` or (M `Is_Supercrusher` and T `Is_Squashable_By_Supercrusher`) → G = T's
position; (b) else d = min + 0.2 × (base range(M) − min) and G = `free_position_near(M)`: rings around M's
position starting at radius max(0, min − |M − G|), steps 1.0 growing by 0.8 to min(1.5 r, 10), 20 rings,
n = trunc(2πR / r) + 1 samples (≥ 4 after ring 0) counter-clockwise from the T→M direction, accept the first
sample that is ≥ d from G (3-D), inside the map and in M's reachable region for M's own class (no occupancy
test); none → leave G at T. Do not place on the T→M line at exactly d.

## 2. b0074: the movement class of a mixed land formation

### 2.1 `0x4779e0`(class manager, member vector): the intersection class [H]

1. mask∩ = AND over every member of `0x477b70`(its type's `MovementClass` +0x1688) (start 0xFFFFFFFF; an
   unknown class id gives mask 0).
2. Walk the manager's class list (the element list of its hash map, +0x10; node +0x10 = class id, +0x14 =
   mask). Keep a class whose mask is non-zero and **contained in mask∩** ((mask & mask∩) = mask); among those
   the one with the **most mask bits** (strict >, so the first in list order wins a tie). None → −1.

### 2.2 `0x414720`(F, lead): F +0x1c4, run at every plan (`0x41c1a0`, lead = `0x41a9b0`) [H]

1. G0 = the top block's clamped goal point for the lead (`0x583150`; for an object goal the object's position,
   MOVEMENT §4.2 / FIGHTER_FLYING §4.11 D 7). Staging has not run yet.
2. c = `0x4779e0`(F's members). c = −1 → F +0x1c4 := −1 (no further step).
3. V = G0 when G0 is zone-connected to the lead's position for c (`0x50ee10`, style 0); else V = the result of
   `0x50f480`(G0, lead position, style 0, c, 30). Neither → F +0x1c4 := c (the validated point is not stored
   anywhere).
4. Shield refinement: c2 = `0x477b00`(mask(c) with the Shield bit 0x40 cleared) = the class with the most
   bits whose mask is contained in mask(c) & ~0x40 (first in list on a tie; −1 when none). If c2 ≠ −1 and V is
   zone-connected to the lead for c2 → c := c2.
5. F +0x1c4 := c. (Then the formation speed F +0x20c is recomputed when negative.)

There is **no fallback to the lead's own class**. c = −1 cannot happen with vanilla data (every class allows
Clear and `WheeledNoShield` / `WalkerNoShield` / `TrackedNoShield` are Clear only); it happens only when a
member has an unregistered `MovementClass` (mask 0) [M]. With c = −1 every zone lookup is "none", so
`0x50ee10` is false and `0x50f480` keeps P (zone none = zone none): the goal is effectively not validated [M].

**Worked mix (vanilla `MovementClassTypeDefs.XML`, MOVEMENT §1.2):** infantry (Infantry: Clear | Shield |
InfantryOnly = 0xC1) with tanks (Tracked: Clear | Shield = 0x41) → mask∩ = 0x41 → c = a Clear | Shield class
(Wheeled / Walker / Tracked: equal masks, so equal zones; which id is stored does not change behaviour) → when
the goal is reachable for it, c2 = a Clear-only class (0x01) and, when the goal is also zone-connected without
crossing shield cells, F +0x1c4 = that Clear-only class. The same holds for infantry + hover (0x61) and
infantry + large walker (0x49). An all-infantry formation gets Infantry, then InfantryNoShield when possible.
Ties between **different** masks with the same bit count (e.g. Spotter alone: 0xA1 → InfantryNoShield 0x81 vs
HoverNoShield 0x21; Lancet / Flying no-shield subsets) depend on the hash map's list order, which was not
recovered [L].

### 2.3 `0x50f480` and which class each validation uses [H]

- `0x50f480`(P, Q, style, class, 30, out) returns a **bool** (true: out := P when P's zone equals Q's or Q is
  outside the grid, else out := the centre of the cell nearest to Q among the cells of Q's zone on the first
  square ring 1 … 29 around P that has one; false: out untouched). Full rule: MOVEMENT §4.3.1 item 7.
- **Composition** (`0x43c1b0`, §4.2 step 2, read again here): the seed's **own** type `MovementClass` +0x1688
  and `OccupationStyle` +0x1698, not a formation class (the formation does not exist yet). Only for a position
  goal is the validated point used. (A seed whose locomotor answers vf +0x240 uses `0x440f90` instead.)
- **Path set-up** (`0x417520`, after staging): F +0x1c4 from 2.2 with style 0; failure → the plan fails.
- **Slot test** (`0x43a070`, MOVEMENT §4.3.2 item 5): `0x4779e0` directly, i.e. c of 2.1 **without** the
  shield refinement.
- **Stand-off search** (1.2.1): M's own class.

### 2.4 Port rule (b0074)

Formation class for zone validation and paths = from the registered classes, the one with the most mask bits
whose mask ⊆ AND of all members' class masks; then, if the goal (or its validated point for that class) is
reachable from the lead, prefer the best class whose mask ⊆ that class's mask minus Shield (0x40) when the
goal point is zone-connected to the lead for it. Recompute at every plan from the top block's goal for the
current lead. Classes with equal masks are interchangeable; break other ties by the lowest class id [port].
No class (only possible with bad data): skip validation. Composition-time validation keeps using the seed's own
class and style; the arc-slot test uses the intersection class without the shield step.

## 3. b0075: `unit.Attack_Move` on a non-hostile or unattackable object

### 3.1 The order `Move_Internal` 0x57da90 (flags 1) [H]

- The destination is resolved without any relation or attackability test: a GameObjectWrapper (or an AI target
  with an object) that is not the caller or its container (+0x2b0) becomes the **object goal**. The only
  object test is "dead → error". So an allied, own, neutral or hostile object, and a story marker, all give the
  same order.
- Units are added to a new coordinator (`0x38f5d0`, `0x425e30`; squad containers expand to their members).
  **`Order_Attack` 0x38d730 is not called** (no call in the function), so nothing is refused and no member is
  dropped (unlike `TF.Attack_Move`, TACTICAL_COMBAT §2.8 table).
- The coordinator order `0x4f8200` gets **kind 3**, object = that object, flags = 0x01, plus the threat number
  and the avoidance mask (0x3f). Kind 1 is used only when no object resolved (positions, task forces).
- So for both cases (a) and (b) retail issues the **attack-move kind 3 block**, not a plain kind-1 move.

### 3.2 TARGETING slots 43 / 44 / 45 against such a target [H]

- Slot 44 (`0x567b40`), slot 43 (`0x5673b0`), slot 45 (`0x5688c0`) → slot 46 (`0x5683a0`) are pure geometry:
  XY distance from the attacker (or a slot position) to the target point against max range + extent / min range
  + extent (TACTICAL_COMBAT §5.4.2). None of them calls a relation test, `Is_Valid_Target`, `0x5680b0` or a
  hostility check; the only object properties read are TEAM / locomotor (+10 rule), LAND_OBSTACLE (+0x346) and
  the extents. They answer for an allied object or a marker exactly as for an enemy at the same place.
- A marker without hard extents and without a model has extent 0 [M: marker types not checked one by one].

### 3.3 What the kind-3 block does when the object is not a valid attack target [H unless tagged]

None of the attack-approach functions tests hostility (callee lists of `0x43c1b0`, `0x438c10`, `0x41c1a0`,
`0x417520`, `0x40e260`, `0x40edd0`, `0x410e90`, `0x4192b0`, `0x410a60`, `0x4165f0`: no relation call):
1. **Composition:** the attack-order part of §4.2 step 6 runs (solo / short-range from slot 44 and `0x4f6e40`);
   the zone-validated point is thrown away (object goal); **no destination check** (§4.3.1, position goals only).
2. **Arc slots** `0x438c10` run as for an enemy (formations not yet in max range get (θ, ρ)); a marker on an
   enterable cell can get slots; one stamped on a non-enterable cell cannot (line test, §4.3.2 item 5.3).
3. **Plan:** `0x40edd0` "every member can fire from here" (members with TARGETING; fog / stealth of the object
   counted) true → the plan stops the formation with no path. Else path to the object's goal point: its exact
   position (+0x78, or for a land squad container the member picked by `0x566aa0` [M: nearest]; clamped to the
   playable box), staged by `0x40e260` when a member is inside its min range of it, re-validated by `0x50f480`
   only when not zone-connected (a marker on impassable ground → the reachable cell next to it). **No free-cell
   snap of the goal on land** (`0x59caa0` is the space path; `0x440f90` only in staging).
4. **Fire trim** `0x410e90`: the path is cut back to the first point from which every TARGETING member is in max
   range, outside min range and has line of fire to the object. So an armed unit **stops at weapon range of the
   allied object or marker**, it does not walk onto it.
5. **No firing:** path B needs `Formation_Attack_Target` (`0x41a2e0`), which only returns a **hostile** object
   (relation 1); an allied / neutral object never becomes the locked target, and the scan's own filters reject
   it. A hostile-owned marker passes `0x41a2e0` but fails `0x5680b0` (+0x41 / +0x6b), so no lock either. While
   moving, the scan takes targets of opportunity with `Attack_Move_Response_Range` (first block flag 0x01;
   `0x4160a0` excludes the goal object itself).
6. **Unarmed units** (no TARGETING member): `0x40edd0` skips them and answers true, so on land the formation
   **does not move at all** [H for the code, M that nothing else moves them].

### 3.4 Following a moving non-hostile object on land [H]

- While moving (state 1), `0x4192b0` every 30 frames: R = `Get_Base_Attack_Range`(lead type) / 2 (no guard cap,
  the block is not a guard); D = 2-D distance from the recorded goal to the object's current goal point;
  re-path when D > R, or when D ≤ R and the formation is stopping while some member cannot fire (bVar7 = kind 3
  without 0x02 is true for this block). With one block, `0x4165f0` allows it (blocks[0] is kind 3). When D ≤ R,
  not stopping and every member can fire: the land stop (path cut to its first node).
- After arrival (state 3), `0x410a60` every 30 frames: any TARGETING member out of max range / inside min range /
  without line of fire → F state 0 → re-plan.
- **Both are a re-plan of the same formation** (`0x417520` on the same block stack, same coordinator; staging and
  trim re-applied; the arc slot (θ, ρ) stored in the block is kept, `0x438c10` is not re-run). No new move order,
  no new coordinator, no re-composition. A fogged object blocks the re-path (FIGHTER_FLYING §4.11 C).
- Not read: when the `UnitMovementBlockStatus` (0x6ad960) of such an order reports finished (what
  `BlockOnCommand` waits on) [L: probably on arrival / object death as for position moves].

### 3.5 Port rule (b0075)

`unit.Attack_Move(obj)` (Lua) on any live object other than the caller or its container issues the land attack
move (kind 3, flags 0x01) whatever the object's owner or attackability: no `Order_Attack`, no member dropped,
no destination check, goal = the object's exact point (zone re-validated only when unreachable), arc slots,
min-range staging and the fire trim exactly as for an enemy, with TARGETING ranges computed by geometry alone.
Armed units therefore stop at weapon range and never lock or fire at a non-hostile or unattackable target (path
B needs a hostile object that passes the attackable test); they still take targets of opportunity within
`Attack_Move_Response_Range`. A formation with no TARGETING member does not move. When the object moves, keep
the same formation and re-plan it (30-frame checks: D > base range / 2, or stopping short; after arrival, any
member unable to fire), never issue a new move.
