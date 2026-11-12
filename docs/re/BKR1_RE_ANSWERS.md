# BKR1 RE answers: tactical RE pass 1 (movement collisions, paths, attack / guard blocks, land scan range)

Roadmap group bkr1 (2026-11-12, track C). Two RE passes merged; each part keeps its own source notes below. Every
section ends with its port consequence; the coordinator filed those on the backlog (ids at the end).

Part A (Q1, Q3-Q6): Behaviour read from StarWarsG.exe (FoC Steam x64, Ghidra export plus capstone disassembly of the Steam exe). Prose
only. Confidence: **[H]** read in code, **[M]** inferred with partial code, **[L]** guess. RVAs are image-relative.
Roadmap: b0034 (Q1), b0060 (Q3), b0061 (Q4), b0062 (Q5), b0057 (Q6). Port line numbers are from
`engine/tactical/movement/src/` at bkc1 tip 5ce7cd93.

Part B (Q2, Q7, Q8): Behaviour read from StarWarsG.exe (FoC Steam x64, Ghidra export; vtable slots read from the exe's RTTI with
`tools/re/rtti.py`). Prose only. Confidence: **[H]** read in code, **[M]** partly inferred, **[L]** guess. RVAs are
image-relative. Sources: `Move_Internal` 0x57da90 (Lua `Attack_Move` 0x5730e0 = flags 1, `Guard_Target` 0x579da0 =
flags 2), `Service_Land` 0x634810, `Validate_Current_Target` 0x567c50, `Get_Scan_Range` 0x566f50, 0x4160a0,
LocomotorCommon `Auto_Engage` 0x42d0f0, the formation step 0x422b70, 0x410a60, 0x40edd0, staging 0x40e260, fire trim
0x410e90, plan 0x417520, attack slots 0x438c10, composition 0x43c1b0, Push 0x41fd30, line test 0x5a4c10 → 0x5a50d0 →
0x5a5810 / 0x5a51f0, `0x397780`. Notation as in TACTICAL_COMBAT §2.8 and FIGHTER_FLYING §4.11: TD = the TARGETING data
pack (+8 current target, +0xf4 lock), F = the formation, blocks[0] = bottom (the order's block), top = blocks[n−1].
Roadmap: b0038 (Q2), b0069 (Q7), b0077 (Q8).

## Q1 (b0034). Does retail break a Starved / Pass cycle?

**State 15 service `0x681ef0`** [H]. Countdown +0xb0 = 0 → 30. Validity `0x680650` first (record ends only by 8 / 9
there). Reset `0x67f080`, M := 0 (flag 0), S := max. Then `0x505630`(collision system, M, list) and, **only when the
list has exactly one entry**, the path-around `0x5c80e0`(record, M, S, flag, kind 4, 0); success → state 16. Failure,
or a list size ≠ 1 → state 15 stays and the same thing is tried next frame. No timer, no retry count, no stamp (vf
+0x78 is true only in states 1 / 4), no quota gate (non-zero states are serviced every frame, so the search runs every
frame and adds to the complexity counter +0x13c).

**What the list counts** (the spec misreads it) [H]. `0x505630` walks the record ids listed for M (the id hash,
system +0x90) and keeps a record only when its kind (vf +0x48) is 4 (MovingVsMoving) **and** `0x680e80`(record, M) is
true: **state = 15 and +0x7c (the master) = M**. It appends the partner (`0x5c7100`). So "exactly one" means **M is the
starved master of exactly one state-15 record**. M's other records (a Pass in state 4, a MovingVsStopped, a Terrain,
records where M is slave, or MvM records in any other state) do not count. M stands for good only when it is the
starved master of two or more records at once, or when its detour keeps failing.

**Flag argument** [H]. vf +0x1f8 gives each unit's wanted orientation as Euler degrees (component 2 = yaw, as
`0x41c000` reads it). `0x20b100` subtracts M − S and wraps the yaw difference into [0, 360); `fabs` is a no-op on it.
Flag = **(yaw_M − yaw_S) mod 360 < 90**, which is **one-sided**: true only when M's wanted heading is 0 to 90° on the
positive (counter-clockwise) side of S's [M: the yaw sign convention]. Not the symmetric "within 90°".

**The same list in the obstacle set** [H]. `0x67d580` (every Path Around obstacle test) marks as **always an
obstacle**: the forced-id list `0x140b3a1a0` (filled per window by `0x677340`: the forced id plus `0x505e10`, the
slaves of u's state-4 records where u is master) and then the `0x505630` partners, i.e. the S of each state-15 record
where u is the starved master. So during the state-15 detour S is always an obstacle, whatever the flag.

**Answer.** Retail has no timeout and no contact or end test in state 15 [H]. Its breaker is the narrower count: a
starved M with other collision records still detours. The 5.4.6 row 15 ("while M has exactly one collision record")
is wrong and should read "while M is master of exactly one state-15 record".

**Port consequence** (tier 1). `movement_system.cpp` 4059-4067 (`case S::Starved`) counts **every** CollisionObject
in which M takes part (any kind, any role), so a starved M that also has a Pass record stands. That is the
test_land_group jam. Backlog text: *"State 15 (Starved), movement_system.cpp 4062-4066: count only MovingVsMoving
records in state 15 whose master is M (retail `0x505630` → `0x680e80`), not every record of M. Path-around flag =
(wanted yaw M − wanted yaw S) wrapped into [0, 360) < 90 (one-sided, `0x681ef0` → `0x20b100`), not dot > 0. In
`around_obstacles` (around 2983) add the partners of u's state-15-as-master records as always-obstacles (`0x67d580`);
in `goal_obstacle` (3065-3067) replace 'any MvM partner' by those partners plus the forced list."* Tier 1, S1: it
causes a permanent jam that retail does not have.

## Q2 (b0038). Attack_Move on an object while the unit holds an older target

**What 0x40edd0 and 0x410a60 test: the block object, never TD +8** [H]. 0x40edd0(F) takes the top block's object
(blocks[n−2]'s when n > 1 and the top has flag 0x10); true when F has no block, no member or the block has no object.
Every member that is not busy (0x39a350) and has TARGETING is asked TARGETING slot 46 (inside min range from where it
stands) and slot 43 (in max range) **against that object**, plus the land line of fire (0x3b5ad0). 0x410a60 combines it
with 0x4165f0 (FIGHTER_FLYING §4.11 A). The unit's current or locked target is not read by either.

**`unit.Attack_Move(obj)` neither drops nor replaces the target** [H]. 0x5730e0 loads flags 1 and jumps to
`Move_Internal`. Per unit it: switches off unit abilities 5, 0x22, 0x29 and 6 when active (0x38f5d0), leaves every
coordinator (0x426d50), raises signal 0xb, joins one new coordinator; then `Move_To` 0x4f8200 with {kind 3, object,
flags 0x01}. No `Order_Attack`, no `Set_Target`, no write of the lock. The static call graph from 0x57da90 reaches
`Order_Attack` / the team target only through the ability switch-off (0x38a350) [M: virtual calls are not in the graph].
So TD +8 and the lock keep whatever they held; the block object only reaches TARGETING through path B (TACTICAL_COMBAT
§2.8 B), which runs **only while the lock is clear**.

**Task-force `Attack_Move` (issue-attack bit) differs** [H, from §2.8 C]: `Order_Attack(ordered 1)` on a land unit that
is not a team container, team member or LUCKY_SHOT unit takes the weapon path: lock cleared, weapon hard points aimed,
**TD +8 kept but now unlocked**. Team members / containers take the set path: `Set_Target(obj, lock 1)` replaces the
old target at once.

**What an unlocked older target X does** [H]. With the lock clear, `Service_Land` runs path B first: the formation goal
Y replaces X (locked) as soon as Y is inside weapon range. Otherwise X goes through `Validate_Current_Target` on the
due scan, whose range rule accepts X only when it is the formation goal (0x41a2e0(F, −1)), in weapon range, or
0x4160a0(F, u, X) ≠ 0 (§2.3 step 9). So an unlocked X never holds a unit next to Y.

**What a locked older target X does** [H unless tagged]. While locked, `Service_Land` skips path B and the
validate / rescan block. Then, for every service with TD +8 ≠ null (the null branch returns before this step):
1. A unit with a LOCOMOTOR (+0x333) calls its locomotor vf +0x260 (`Auto_Engage`) with (TD +8, hard point 0), locked or
   not. For Walk, Bike, LandTeamContainer and LandTeamInfantry this is LocomotorCommon **0x42d0f0** (vtables 0x8ad798,
   0x8b23c0, 0x8a3630, 0x8af8d0; JetPack has its own 0x6418f0, not read). 0x42d0f0: F = the unit's pack +0x1f0
   (none → nothing); when F's goal 0x41a2e0(F, −1) ≠ X and rr = 0x4160a0(F, u, X) ≠ 0: rr = 1 splits the unit into a
   formation of its own (0x424850), then the auto block {kind 3, X, flags 0, +0x5d = 1} is pushed (0x41cf70 → Push).
2. Range: X in weapon range (slots 44 / 45) → fire; out of range → TARGETING state 2 and X is **kept**, except that a
   deployed or deploying unit (type +0x23b0 ≤ 0) drops it (slot 27).
- Consequence while the Y formation is under way (state ≠ 3; blocks[0] has flag 0x01): 0x4160a0 answers 2 when the unit
  is within `Attack_Move_Response_Range` of F's path point, or always when F has no path-progress object (+0x230) [M:
  +0x230 is created by Push]. **The formation is turned toward X** (auto block on top of Y).
- Consequence after arrival (state 3): 0x4160a0's non-attack-move branch returns 0 for a non-guard top whose
  blocks[0] names an object, so nothing is pushed; 0x410a60 sees a = true (n = 1, kind 3), c = true (every member can
  hit Y) → nothing. **The unit stands next to Y, does not fire at Y, does not approach X**, until X dies, is hidden by
  fog for the unit's player, stealthed (no `Target_Stealth_Units`), garrisoned or +0x3b4 (lock drop list, §2.8 B).
- The item-6 stop reaches state 3 without a targeting service in between when the members already stand: the plan at
  composition returns success with no path, the formation step 0x422b70 loops states 0 → 1 → (arrival, every member
  done) → 3 in the same coordinator update [M: "done" for a standing member]. A member that was still braking leaves
  F in state 1 for a few services, and X is pushed then.
- The UNIT_AI attacked-by response 0x4f5e30 acts only with the lock clear, so it does not break the hold either.

So retail shows the b0038 hold only for a **locked** X plus a unit-level `Attack_Move` (no issue bit) whose formation
arrives at once [M overall: the chain is [H] per step]. Probe to settle it: unit A `Attack_Move(X)` until it fires at X
(lock), move X out of A's range (Teleport), then `A.Attack_Move(Y)` with Y in A's range from where A stands; retail
holds iff A neither fires at Y nor turns to X within 60 frames.

**Port consequence.** Two differences that change sim results (tier 1). (1) The port keeps an **unlocked "ordered
target"** that suppresses path B and the scan while it lives (`CombatSystem::gather` combat_system.cpp 1341-1345,
`decide_target` 1477-1488; written by `CombatSystem::order_attack` 332 from Lua `Attack_Target` battle_ai.cpp 1070 and
the player attack order tactical_battle.cpp 1748 / 1898). Retail has one target slot with a lock bit; an unlocked target
is validated each due scan and replaced by path B. Backlog text (tier 1, S2): *"Land Order_Attack(ordered 1) on a unit
that is not a team container / team member / LUCKY_SHOT (Lua Attack_Target battle_ai.cpp 1070, player attack
tactical_battle.cpp 1748 / 1898) must not store an unlocked ordered target: retail (TACTICAL_COMBAT §2.8 C, BKR1 Q2)
clears the lock, aims the weapon hard points and leaves TD +8 alone; the lock comes from path B in weapon range. Only a
locked target may skip path B and the rescan (combat_system.cpp gather 1341-1345, decide_target 1477-1488); an
unlocked one is validated each due scan (range rule incl. the formation goal and 0x4160a0) and replaced by path B.
Use order_attack_issue semantics for these orders."* (2) The port has **no land `Auto_Engage`** (the push exists for
space fighters only, tactical_battle.cpp ~4238-4377). Backlog text (tier 1, S1): *"Land Service_Land 0x634810 calls
the unit's LOCOMOTOR vf +0x260 every service with a non-null TD +8 (locked or not, hard point 0): LocomotorCommon
0x42d0f0 for Walk / Bike / LandTeamContainer / LandTeamInfantry. When the target is not the formation goal
(0x41a2e0(F, −1)) and 0x4160a0(F, u, t) != 0, it pushes the auto attack block {kind 3, t, flags 0, +0x5d = 1} (rr = 1:
split the unit into its own formation first). The port never does this on land, so idle land units do not chase
targets within Idle_Chase_Range of their slot goal, guards within Guard_Chase_Range, attack-movers within
Attack_Move_Response_Range of the path point, and a unit locked on an out-of-range target keeps walking its order
(BKR1 Q2 / Q8; FIGHTER_FLYING §4.9 step 2 for the space twin). Relates to the b0084 stand-in."* The port's handling
of a **locked** X plus `unit.Attack_Move(Y)` (keep X, item-6 stop on Y) is retail; the b0038 seeds should be re-run
after (1) and (2) to see which holds remain.

## Q3 (b0060). MovingVsMoving head-on tie-break and the overtake test

**`0x41a5c0`(formation +0x10, the member vector)** [H]. It returns a scalar half extent in cells and has no axis
parameter. With n = member count, k = ⌈√n⌉ (an exact integer root is kept) and cell = max over members with a locomotor
pack of (`FormationSpacing` type +0x16d4 + 2·hx/20), where hx = `0x396a30` (the **X / length half extent**,
`Custom_Hard_XExtent` × `Scale_Factor`, deployed variant when deployed; a squad container: its team box). Result =
**max(k·cell + 0.25, √max(1, 1.8·A))**, A = `0x41b4a0` = Σ over members of (2hx/20 + g)(2hy/20 + g) (a squad
container: (2·`0x404cd0`/20)², [M] on that branch's condition). The layout size `0x41a740` (MOVEMENT §4.3) has the same
shape with two differences: it uses the **Y** half extent `0x396b70`, and it returns the bare cell when n = 1.
`0x41a5c0` has a single caller, the head-on branch of `0x67f7b0`.

**Head-on tie-break** [H], equal class, both in formations (pack +0x1f0): e0 = `0x41a5c0`(F0), e1 = `0x41a5c0`(F1),
both floats. e1 > e0 → M = p1. e0 > e1 → M = p0. Equal → M = p1 when the id (+0x50) of F0's **member-vector index 0**
(`0x41b450`(F, 0)) is lower than F1's, else p0. Index 0 is the first member added at composition (`0x43e5b0` adds them
in descriptor order, seed first) [M: later joins and leaves may reorder it]. It is not the lowest id.

**Overtake `0x683260`(a = rear, b = front)** [H]:
- **+0x3a0 & 2** (mask 0x02) is the object's "killed / being removed" bit (set when it is destroyed and before it is
  unlinked; LUA_API.md, `Detach` 0x3a8380). Either unit with it → false. Giant a → false.
- Same coordinator: `0x426140`(coordinator system, unit) returns the first coordinator in the system's vector for which
  `0x4f72e0` is true (the unit's formation +0x220 is that coordinator, or the unit is in the coordinator's raw unit list
  +0x28). Both non-null and equal = **same coordinator** (one group order).
- Va, Vb = formation speed (F +0x21c; negative → max speed) or max speed. Continue only if **Vb ≤ Va or same
  coordinator**. vs = min(Vb, 0.5·Va). Need vs ≤ Va, Va ≠ 0, Va − vs ≠ 0, a's pack +0x28c = 0, then the time test of
  MOVEMENT 5.4.6.
- If the time test passes, same coordinator and both in formations: return **true only when rank(F_b) > rank(F_a)**,
  compared unsigned. The rank is FormationClass +0x23c, read by the int getter `0x41a2d0` (`mov eax`, `jbe`).
- **The rank** [H]. `0x439360` (the destination check of a land composition) takes every descriptor whose first
  member's `Space_Layer` (`0x397900`(m, 1)) is **Land (0x10)**. This is the layer, not movement class 16 LargeHover.
  After the placement passes it sorts them with `std::sort` (unstable) using `0x43b110`: **descending projection of
  each formation's block goal on the order axis** (globals `0x140b2f278` = D, `0x140b2f288` = axis a, MOVEMENT §4.3.1
  item 3), so the front-most destination gets rank 0. With order flag +0x48 bit 0x80 the sort uses `0x43b420`
  instead: descending `0x41b2a0` (the formation's slowest member's max speed), fastest first. It writes descriptor
  +0x22c = 0, 1, 2, …; `0x43e5b0` copies that into the new FormationClass +0x23c (`0x423ad0`). The early exits (no
  land descriptor; R's test path < 2 points) leave every rank 0 (ctor `0x40d660`). *Correction:* MOVEMENT §4.3.3's
  "ψ = F +0x23c" is offset +0x23c of the inner formation data (`0x411980` gets F + 0x10), i.e. FormationClass +0x24c.
  It is not this rank.
- Effect: inside one group move, the rear unit passes the front one (state 4) only when the front one's formation is
  heading to a destination behind its own. Otherwise it is slaved (state 6), even when it is faster. With equal ranks
  (one formation per order, or early exits) a same-coordinator overtake between two formations is always false.

**Port consequence** (tier 1). (a) `mvm_choose` 3670-3690 compares member counts and the lowest member id.
Backlog text: *"Head-on tie-break (movement_system.cpp 3673-3687): compare the `0x41a5c0` half extent max(⌈√n⌉·max_m
(FormationSpacing_m + hx_m/10) + 0.25, √max(1, 1.8·Σ(2hx/20 + g)(2hy/20 + g))) with hx = the X (length) half extent,
not the member count; on a float tie use F's first member (composition order, seed first), not the lowest id."* (b)
`mvm_overtake` 3738-3751. Backlog text: *"Overtake (0x683260): give each land formation the rank of `0x439360`
(descending goal projection on the order axis; fastest first with order flag 0x80), assigned for **every** Land-layer
formation of a composed order (the port's `large_hover_index`, 1099-1105, gates on movement class LargeHover and
sorts by centroid: both wrong). Keep a per-unit coordinator id (the order). Allow Vb > Va when a and b share it; after
the time test, with both in formations, return true only when rank_b > rank_a. Add the killed-bit test (a unit being
removed)."* Tier 1, S2. The killed bit alone has no visible effect: S3.

## Q4 (b0061). Terrain corners and the staircase masks (`0x675240` → `0x677e40`)

Terrain vertices are made in the feasibility step `0x677e40`, called per window by `0x675240` before the obstacle
corners [H]:
1. **Passability P** over the window (x0, y0, w, h = `0x140b3a190`…`19c`): `0x67cd80` = in the grid and (cell bits
   `0x50e610` & class mask of type +0x1688) = cell bits.
2. **Flood fill R**: 4-neighbour BFS over P from the start cell. The goal cell must be in R, else the window fails.
3. **Blocked-neighbour mask M** for **every** window cell (`0x67a2d0`): bit k is set when neighbour k is outside the
   window or not in P. Bits: 0 (−1,−1), 1 (0,−1), 2 (+1,−1), 3 (−1,0), 4 (+1,0), 5 (−1,+1), 6 (0,+1), 7 (+1,+1).
   **Outside the window counts as blocked.**
4. **Candidates C** = `0x67ce80`(M) on every window cell, with no test of the cell's own passability:
   - convex: (M & 0xD0) = 0x80, or (M & 0x68) = 0x20, or (M & 0x0B) = 0x01, or (M & 0x16) = 0x04. That is, one
     diagonal blocked with both of its adjacent orthogonals free; the other bits do not matter;
   - **concave**: any two perpendicular orthogonals blocked: (M & 0x0A) = 0x0A, (M & 0x12) = 0x12, (M & 0x50) = 0x50,
     or (M & 0x48) = 0x48.
5. **Pass 1, row-major (y outer, x inner), clearing C in place.** A cell not in R is cleared. A cell in R is cleared
   when it matches a staircase step, where C(·) is read live, so a cell already visited shows its pruned value:
   - (M & 0xDA) = 0x0A, and C(x+1, y) and C(x, y+1);
   - (M & 0x7A) = 0x12, and C(x, y+1) and C(x−1, y);
   - (M & 0x5B) = 0x50, and C(x−1, y) and C(x, y−1);
   - (M & 0x5E) = 0x48, and C(x, y−1) and C(x+1, y).

   Each is an inner L (two orthogonals blocked, the opposite three cells free) whose two open-side neighbours are
   candidates too, i.e. one step of a diagonal staircase. C(x+1, y) and C(x, y+1) have not been visited yet, so blocked
   or unreached candidates there still count.
6. **Pass 2 and vertex creation, row-major** over the cells left in C. If M is **exactly** 0x20 and (M(x−1, y+1) & 0x81)
   = 0x81, or M = 0x01 and (M(x−1, y−1) & 0x24) = 0x24, or M = 0x04 and (M(x+1, y−1) & 0x81) = 0x81, or M = 0x80 and
   (M(x+1, y+1) & 0x24) = 0x24, the cell is dropped. These are lone convex corners against a one-cell-thick diagonal
   wall. Every other cell gets a vertex: type **4** (+0x18), no obstacle, both positions = **(20x, 20y)** (cell index ×
   cell size, which is the cell centre, MOVEMENT §1.1), cell coordinates as floats at +0x28 / +0x2c.

Consequences [H for the rule, M for frequency]: a reachable cell in the inner corner of a U or L of impassable cells
is a vertex, unless it is part of a diagonal staircase. Every reachable window-corner cell is a concave candidate
(two sides outside the window) and becomes a vertex. Edges from terrain vertices use the rules of §1.5.1 item 4a (no
cone test: no obstacle).

**Port consequence** (tier 1). `path_around.cpp` 377-389 makes only convex corners, judges neighbours by the whole
grid (not "outside window = blocked"), and has no pruning passes. Backlog text: *"Terrain-corner vertices of the
Path Around search (path_around.cpp 377-389): build the blocked-neighbour mask M per window cell with
outside-window = blocked, take candidates by `0x67ce80` (four convex patterns plus the four concave 'two
perpendicular orthogonals blocked' patterns) over all window cells, then pass 1 (drop unreached cells and the four
staircase patterns 0xDA/0x0A, 0x7A/0x12, 0x5B/0x50, 0x5E/0x48 against live C of the four orthogonal neighbours,
row-major, in place), then pass 2 (drop single-diagonal masks 0x20/0x01/0x04/0x80 whose blocked diagonal cell has
both cross diagonals blocked); vertices at the cell centre, row-major. Changes vertex counts (quota) and detours inside
U-shaped impassable areas."* Tier 1, S2.

## Q5 (b0062). The approximation flag pack +0x28c (`0x505fe0`)

**Writers** [H]. Only `0x505fe0` (and the pack ctor `0x553f00`, 0). Callers: detection `0x508170` (countdown pack
+0x275 ≤ 0 → rebuild into pack +0x200, countdown := `FramesPerPositionApproximationRebuild` 8); `0x50aad0` (the
in-between trim) when only 2 points are left and the unit is past the second; and the push free-spot search `0x441750`,
which rebuilds into a private list but still writes the flag of each listed mover (horizon min(2T, T + 4)). A path push
zeroes +0x275 (MOVEMENT §5.4.1), so the next detection pass rebuilds.

**`0x505fe0`(unit, out, T)** [H]. The out list starts with the position. Nothing more, and **+0x28c is not written**,
when the locomotor is not moving (vf +0x1a8) or the speed (pack +0xec) is 0. When the top record is missing or not
started (record +0x6d = 0) it adds a braking point (v² / 2·decel ahead) and also leaves +0x28c unchanged. Otherwise
it **simulates the walk**:
- Look-ahead L = max(`MinLandPredictionDistance` (GC +0x1498 = `0x140b16c98`, 60), 30 × T × max speed). With T = 0
  (every call but `0x441750`), T = max(type `MovementPredictionInterval` +0x3c8, `LandPredictionTimeInterval` (GC
  +0x1490, 2.0)) (`0x3972a0`).
- Start at the unit's position and facing, at the top record's current waypoint (+0x60), with its waypoint lines
  (`0x5c5060`). Straight steps of clamp(distance to the current waypoint line, 0.1, `ApproximationForwardDistance`
  2.0) add to the travelled distance. Turning steps (the turn rate, at least half of acos(0.9962) ≈ 2.5° per step;
  speed limited by acceleration and the waypoint speed factors) do **not** add to it. Every waypoint line crossed
  (n·(P − W) ≥ −0.001) advances the index. Past the record's last segment it continues on the record **directly below
  the top** (`0x556960`, from index 0, skipping lines already crossed). Past that one, or past the top record when it
  is the only one, the walk ends.
- **+0x28c := true when the walk ran out of path before the travelled distance reached L; false when L was reached
  first** [H]. Only the top record and the one below it are walked.
- Output: the sampled points thinned so that consecutive kept directions differ by more than cos 0.9962, plus the
  last point.

So +0x28c means "the end of (top + next) path lies within about L of path length (turn arcs not counted), at the last
rebuild". It is sticky. It is refreshed at most every 8 frames per unit (phase per unit), at once after a path push,
and it is stale while the unit stands, brakes or has an unstarted top record.

**Port consequence** (tier 1). `near_path_end` (movement_system.cpp 2713-2717) is straight-line distance ≤ 30 ×
max(1.5, MPI) × V (+1.5 s for a crusher) + r_a + r_b, evaluated live. It uses the wrong time constant (1.5 instead of
2.0), has no 60 floor, adds radii, measures a straight line instead of path length and is never stale. Backlog text:
*"Model pack +0x28c (0x505fe0) as a per-unit byte: recompute it on an 8-frame countdown per unit (reset to 0 by every
path push, and when the approximation is used up), as 'remaining path length from the unit along the top record from
its current waypoint, then the record under it, ≤ L = max(MinLandPredictionDistance 60, 30 × max(MovementPrediction
Interval, LandPredictionTimeInterval 2.0) × max speed)'. Leave it unchanged while the unit is not moving, has speed 0
or an unstarted top record. Use it in near_path_end's callers (destination_blocked, around_obstacles, goal_obstacle,
mvm_overtake) instead of the straight-line test."* Tier 1, S2. [M: path length is the stand-in for the simulated walk
minus turn arcs.]

## Q6 (b0057). When the tracker flags are taken, against the formation plan

**Frame order of the land mode update `0x3639d0`** [H, read in code]: FOW → **tracker update `0x539b20`** (vf +0x238;
`0x53a400` stores each registered mover's position, `0x5d8620` re-fits every record: rectangle from the stored
position, flag 1 Moving / 2 Static from the **live** locomotor test vf +0x1a8, `0x6b3b00`) → player stats → **GOM
service** `0x2be640` ×2 (object walks) → `0x490580` → **collision driver `0x50a780`** (detection `0x508170` at its end)
→ **movement coordinators `0x426eb0`** (composition `0x43e5b0` / `0x43c1b0` / `0x439360` and formation steps
`0x4f80c0` → `0x422b70`: all calls of the plan `0x41c1a0`) → the rest. `0x539b20` has this one caller. So **the plan
runs last, and the corridor test it calls (`0x5093c0` → `0x538200` → `0x5d7ae0` → `0x6b2fb0`) reads flags and
rectangles from the start of the same frame**: before this frame's walks, its collision driver (stops, pushes,
detours) and every plan of this coordinator update. A unit planned at frame F is Moving from F + 1's tracker update.
A unit that arrived (or was stopped by the driver) during F is still Moving, at its start-of-F position, for F's plan.
The occupancy query does not rebuild a dirty layer. Box queries `0x5d7a00` / `0x5d7c00` do (`0x5d8140`, flags live,
positions as stored) [M: whether such a rebuild happens before the plan in practice]. Attack-block pushes
(`0x41fd30` → `0x438c10` → `0x41c1a0`) can plan at other times in the frame, and they read the same snapshot.

**Port consequence** (tier 1, small). `plan_land` runs at the start of the port tick (plan → detect → drive → walk)
and `corner_static_` (movement_system.cpp 1843-1845) takes `unit_moving` just before the plan, with live footprints
in `corridor_ok` (2066-2083). These include the walk and collision driver that ran since the previous plan. Retail
excludes them. Backlog text: *"Corner-pass corridor test (0x5093c0 via the tracker, MOVEMENT §5.3.2): take the
Moving / Static flag and the footprint rectangle of every land layer object from a snapshot made right after the
previous tick's plan_land (retail: the tracker update at the start of the frame whose coordinator update plans),
not from the state just before this plan. So a unit that arrived or was stopped since then still counts as Moving at
its old place, and one planned last tick counts as Moving."* [M for the mapping, because the port's driver sits
before its walk while retail's sits after.] Tier 1, S2 (one-frame timing; it changes corner fractions next to units
that just stopped or started).

## Q7 (b0069). Guard blocks: staging, slots, trim start, slot-leg reach

**Staging 0x40e260 never stages a guard block** [H]. It reads the **top** block and acts only when its object is set
and its flag 0x02 is clear. (For land formations 0x417520 calls it at every plan unless the block has flag 0x20; for
space formations only with more than one block.) So G = C for a guard: the port's choice is retail; MOVEMENT §4.3.2
item 6 should say "a non-guard top", and the `[port]` at tactical_battle.cpp 2023 becomes [H]. Detail for b0068: M is
the member with the largest `Targeting_Min_Attack_Distance` (or a pending ability's range > 0), strict >, starting
from −1, so the first member wins a tie at 0; slot 45 at 0 min range is false, so nothing moves then.

**(a) The fire trim's start value g is a guard-of-an-obstacle rule** [H]. In 0x410e90 the guard range is
`Land_Guard_Range` (GC +0x11ec4, 150) and g starts at the path length L. Only when the top is a guard **and its
object has LAND_OBSTACLE** (obj +0x346 ≠ 0xFF): the range becomes 150 + `0x397780`(T) and g starts at
max(0, L − that range). The argument of 0x397780 is the block object T (disassembly 0x4110cb); 0x397780 = √(hx² + hy²)
of T's extents (0x396850) when T has TEAM and a locomotor answering vf +0x230, else the type radius 0x373b30, else 0.
A non-guard block always starts at g = L. Other trim facts confirmed: it runs only for a kind-3 top whose stored point
is (0, 0, 0) (object blocks), with an object, F type ≠ 2, T not fogged / stealthed for members[0]; a guard top in space
gives no trim; the guard member test is |p − T.position| (2-D, T +0x78 / +0x7c) > the guard range, strict; the land
line of fire applies to guard and non-guard alike.

**(b) Who gets guard slots** [H unless tagged]. Lua `unit.Guard_Target(obj)` 0x579da0 = `Move_Internal` with flags 2:
a kind-3 block with flag 0x02 through `Move_To` 0x4f8200 into a new coordinator. The land composition 0x43c1b0 calls
0x438c10 for any kind-3 block without flag 0x20, guard included. 0x438c10's guard rule: a one-block, Land-layer
formation joins L when **members[0]** (0x41b480(F, 0)) is farther, **2-D, strict**, from C (0x583150 of the block)
than min(0x3749d0 attack range of members[0], 150) (the test sits in the member loop but always reads members[0]).
Push 0x41fd30 calls 0x438c10 only for a block with an object and flag 0x02 clear, so a pushed guard block never gets
slots. The auto path (0x42d0f0 → 0x41cf70) pushes flags 0, never a guard. The other Push callers read (0x4f6470,
0x575710 `Divert`, 0x58d930, 0x58dd10, 0x58e370, 0x58e8e0, 0x640880, 0x646990, 0x63e2b0, 0x685490, 0x5d58f0) build
blocks with flags 0 or 0x40 [M: their block init was scanned, not read line by line]. `LandTaskForce.Guard_Target`
was not read [M: same kind-3, flag 0x02 block expected]. So guard slots come only from a group order's composition.

**(c) q, the slot-leg reach, including "blocked at once"** [H]. 0x417520 sets q := 0 and calls 0x5a4c10(G, Q =
G + ρ·rot(−v, θ), the formation's class +0x1c4, &q); there is **no q > 0 test afterwards**. The line test:
1. ρ = 0 → q = 0; G's cell off the grid → q = 0 (failure).
2. **Region shortcut:** G's cell enterable for the class (its block bits inside the class mask) **and** the same
   region (0x50e780) as Q's cell → q = ρ, with no cell walk: blocked cells on the straight line inside one region
   do not shorten it.
3. Otherwise a piece shorter than 40 gets the cell walk 0x5a51f0: q = the 2-D distance from G to the entry of the first
   refused cell, ρ when every cell (and the end test 0x5a3b60) passes; longer pieces are halved at the midpoint (whose
   region is taken) and the first half decides: failure → q = its partial; success → q = half + the second half's.
4. **Blocked at once** (G's own cell refused, or G off the grid): q = 0, so Q' = G. The second path start → Q' (= G)
   is still planned, and when its first segment points the same way as the first path's, G is appended: the formation
   path becomes start → … → G → G (a zero-length last segment) [M: that the second path equals the first; the
   zero-length last segment's effect on the end shift of §4.3.3 was not traced]. With G re-validated as reachable
   (§4.3.1 item 7 (b)) this needs a G on a cell the class may reach but not enter (rare).

**Port consequence.** Staging and the guard L rule: none (port retail; retag the `[port]` 2-D distance in
attack_approach.cpp 401-403 and tactical_battle.cpp 2023 as [H]). Two differences (tier 1). Backlog text (tier 1, S2):
*"Guard fire trim (attack_approach.cpp fire_trim 190-194, attack_plan ~622-627): retail 0x410e90 starts g at L for every
guard whose object lacks LAND_OBSTACLE and uses 150 + 0x397780(T) (T's radius) as the guard range and
max(0, L − that) as g only for a LAND_OBSTACLE object; the port starts g at max(0, L − 150) for every guard and never
adds T's radius (BKR1 Q7 (a))."* Backlog text (tier 1, S2): *"Slot-leg reach line_reach (attack_approach.cpp 231-252):
retail 0x5a4c10 → 0x5a5810 gives q = rho at once when G's cell is enterable and in the same region as Q's (no walk),
else halves pieces >= 40 and walks < 40 from G; the port walks every cell from G, so a Q behind an obstacle inside
one region is cut short. Also slot_leg_point (213-217) refuses q = 0, where retail plans start -> G and appends G
(BKR1 Q7 (c); the q = 0 part is S3)."*

## Q8 (b0077). Land scan range 0x566f50 / 0x4160a0

**`Get_Scan_Range` 0x566f50 on land** [H]. Let o = the TARGETING owner (on land **not** replaced by its container; that
swap is space-only), R = `Get_Max_Attack_Range` (vf +0x140).
1. o null → 0. o deployed (0x39a390) → R.
2. o without a locomotor pack, or a pack without a formation (+0x1f0) → **R**.
3. 0x4160a0(F, o, none, &Rr) ≠ 0 → **R + Rr**.
4. Otherwise land: **R when o has a TURRET slot (+0x33d), else 0** (space R, other modes 0).

**0x4160a0 on land** [H]: identical to FIGHTER_FLYING §4.10 A, minus the space-only `Space_Layer` gate. Corrections to
TACTICAL_COMBAT §2.8 "Targets of opportunity": its step 3 is broader. A candidate adds `Autonomous_Move_Extension_Vs_
Attacker` (+0x211c) when it is in the attacker list (0x39b1c0) of a guarded object (top is a guard whose object has
+0x370 and +0x333) **or of the unit itself**; the result in steps 4 / 5 uses blocks[0] (bottom) for the flags and the
attack-move branch, the top for the guard and 0x40 tests. Resulting land scan cube for a non-turret, non-deployed unit in
a formation:

| Formation state | Rr (from blocks[0]) | 0x4160a0(F, o, none) | scan half-extent |
|---|---|---|---|
| Attack_Move (position or object), not arrived (state ≠ 3) | `Attack_Move_Response_Range` | 2 when o is within Rr (2-D) of F's path point (always without a path-progress object) | R + Rr, else 0 |
| Attack_Move(position), arrived (state 3, one block, +0x240 = 1) | `Attack_Move_Response_Range` | 1 | R + Rr |
| Attack_Move(object) arrived; Attack_Target; any non-guard object block outside the attack-move branch | — | 0 | **0** |
| Guard (top guard), position or object | `Guard_Chase_Range` | 1 when o is within Rr of its slot goal (state 3: always) | R + Rr, else 0 |
| Move_To (flags 0) under way (+0x240 = 0 [M: new formation]) | — | 0 | **0** |
| Move_To arrived, one block; idle after the adopt's degenerate move | `Idle_Chase_Range` | 1 | R + Rr |
| order-locked (0x39ad40); top flag 0x40; deployed with +0x23b0 = 0 | — | 0 | 0 (R if deployed: step 1) |

A turret unit gets R in every "0" row. Candidates beyond R are accepted, both in the scan (§2.3 step 9) and in
`Validate_Current_Target` 0x567c50, when 0x4160a0(F, o, c) ≠ 0 (the attacker extension and, in the guard / idle
branch, the 0x5672b0 "attackable from the point Rr toward c" test apply). FoC land data: `Idle_Chase_Range` mostly
50-500 (100 / 200 common), `Attack_Move_Response_Range` 150-500 (300 / 150 common), `Guard_Chase_Range` 200 common
(up to 1000). So TACTICAL_COMBAT §2.8's "[M for land]" becomes [H], with the additions above.

**Port consequence.** Differs (tier 1): `CombatSystem::gather` (combat_system.cpp 1360-1366, stale comment: land
formations exist now) scans R on land and `responds()` (1305-1311) never fires there, since the scan-range query is set
for space only (tactical_battle.cpp 1029-1034). Backlog text (tier 1, S2): *"Land scan range: set the scan-range query on
land too (tactical_battle.cpp 1029-1034) with 0x566f50 / 0x4160a0 per BKR1 Q8: no formation or deployed -> R;
0x4160a0 != 0 -> R + Rr (Rr by blocks[0] flags; the attack-move branch vs F's path point while not arrived; the
guard / idle branch vs the slot goal; +0x240 and blocks[0] object gates); else R with a TURRET slot, else 0 (a
non-turret unit moving on a plain Move_To or standing on an arrived Attack_Move(object) / Attack_Target block does not
scan). Apply 0x4160a0(F, u, c) to candidate acceptance and to the current-target validation. Fix the stale comment
combat_system.cpp 1360-1366."* Needs the land formation state (arrived, path point, slot goal) from MovementSystem;
it pairs with the land `Auto_Engage` entry of Q2, which uses the same 0x4160a0.

## Filed port consequences (backlog, tier 1, parity-sim, from bkr1)

| Question | Backlog | Sev | What |
|---|---|---|---|
| Q1 | b0176 | S1 | Starved state 15 counts only state-15 MvM records with M as master; yaw detour flag; S as an obstacle |
| Q2 / Q8 | b0177 | S1 | land `Auto_Engage` (LocomotorCommon 0x42d0f0) on every Service_Land with a target |
| Q2 | b0178 | S2 | land `Order_Attack(ordered 1)` must not store an unlocked ordered target |
| Q3 | b0179 | S2 | head-on tie-break by the 0x41a5c0 size, tie to member index 0 |
| Q3 | b0186 | S2 | overtake: 0x439360 rank for every Land-layer formation, same-coordinator rule |
| Q4 | b0180 | S2 | Path Around concave corners, staircase and diagonal prunes, cell-centre vertices |
| Q5 | b0181 | S2 | +0x28c approximation flag instead of the straight-line near_path_end |
| Q6 | b0187 | S2 | corner-pass corridor test reads start-of-frame tracker flags |
| Q1 / Q6 | b0182 | S2 | tick order (retail: tracker, walk, collision driver, plans) vs the port's plan, detect, drive, walk (RE / design) |
| Q7 | b0183 | S2 | guard fire-trim start (LAND_OBSTACLE case only), [port] -> [H] retags |
| Q7 | b0184 | S2 | slot-leg line reach: same-region shortcut, halving above 40 |
| Q8 | b0185 | S2 | land scan range 0x566f50 / 0x4160a0, stale combat_system comment |

All touch track A's movement files or track D's tactical combat files, so none was ported in bkr1.
