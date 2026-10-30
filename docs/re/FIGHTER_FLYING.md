# Fighter attack runs, flying attitude and the timed follower (StarWarsG.exe FoC x64 v1.121)

This spec closes the S7 movement items left open in REMAINING_GAPS §4.7–§4.8 and MOVEMENT §2.5 / §8 item 10:
- the fighter locomotor's attack run (Directed_Combat 0x1E) and its other open states;
- the flying locomotor's attitude steps, circling and its caller;
- the timed-path follower (locomotor vf +0x1d8);
- the infantry-team slot geometry 0x62d620 and a summary of LocomotorCommon 0x5070b0.

Sources: the Ghidra 12.2 export in `re_reference/`, plus llvm-objdump on `corruption\StarWarsG.exe`. Disassembly
was used to check argument lists the decompiler lost, the read-only constants, and the grouping of float sums.

**Conventions** (same as MOVEMENT.md and REMAINING_GAPS.md):
- Addresses are RVAs; `0x140…` values are absolute VAs of globals.
- Confidence: **[H]** read directly; **[M]** inferred from structure or partly read; **[L]** guess.
- **(dis)** marks arithmetic whose operand order was checked in the disassembly. Elsewhere the order is as
  decompiled. Ghidra sometimes regroups sums (§4.2 has one case), so treat the order of an unmarked sum of three
  or more terms as [M].
- Per-frame units, no dt: speeds are units/frame, rates are degrees/frame, timers are frames (MOVEMENT §0).
- GC = GameConstants (base 0x140b15800). FPS = 30 (global 0x140b0a340, int).
- "Synced draw" = the lockstep LCG 0x140a13e24 (MAIN_LOOP §4.1).
- P = object position (obj +0x78/+0x7c/+0x80). O = orientation (§1.1). V = velocity (data pack +0x14..+0x1c).
  T = the object's type. "Pack" = the locomotor data pack (obj +0xa8).

**RNG summary:** none of the fighter states, the fighter cell grids, the flying follower, the attitude or altitude
steps, or circling draw from the synced RNG. The only draw in this document is the flying loiter re-issue (§6.6,
one synced float(−90, +90)). The fighter hyperspace arrival draw is documented in L1_S4_REMAINING §3.4.

---

## 0. Summary for the implementer

1. **Fighters do not replay the Hermite path.**
   - Every fighter state steers reactively. Each frame it turns yaw (with bank), pitch and speed toward one aim
     point under three per-frame budgets (turn rate, Max_Lift, Max_Thrust), then moves by P += V.
   - Paths from the space planner are only used as waypoint positions with a "passed the waypoint" test (§4.1).
   - The port's "Moving = the Hermite playback" for fighters is wrong. [H]
2. **The attack run vs a ship or structure** (§4.4) is a three-phase loop, decided each frame by the squadron
   leader:
   - **Approach / re-attack:** outside the strafe circle (`Strafe_Distance` + target radius, 2-D, measured from
     the leader), steer at the target.
   - **Firing pass:** inside the circle with the target inside a ±45° yaw / ±45° pitch cone, keep steering at it.
   - **Break-off:** inside the circle with the target outside the cone, fly straight on (aim one turn radius
     ahead along the current velocity) until outside the circle again.
   - A target more than 170° behind triggers a vertical loop (§3.1).
   - Members fly the squadron formation behind the leader.
3. **Dogfights vs squadrons** (§4.5) use a 400-unit "dogfight cell" grid:
   - the attacker squadron flies to the target's cell;
   - it provokes the target squadron to attack it back;
   - each fighter locks one enemy fighter for 10 s (300 frames) and chases it, matching speed outside
     `Minimum_Follow_Distance`.
4. **Idle squadrons park** in a 120-unit cell grid, one squadron per cell (§5). The leader orbits its parking point
   (radius 60, speed 0.5). Members hold their slot at 0.75 × the slot error, plus 40 frames of the leader's
   velocity (§4.2).
5. **Flying (land):**
   - The follower vf +0x1d8 (0x63c340) is the same cubic Hermite as SimpleSpace, with the same basis. The basis is
     now resolved: standard Hermite, exact integers (§6.1).
   - It differs from SimpleSpace in two ways: segments are bracketed with **strict** `t1 < T`, and it computes bank
     from the yaw change.
   - Z and pitch come from the separate altitude step 0x63b920. The pitch target comes from a 60-frame terrain
     look-ahead and is limited to ±30°; z moves by `speed × tan(−pitch)` (§6.3).
6. **The circling caller is the snowspeeder tow cable** (`CableAttackAbilityClass`, §6.5). It circles the target
   object between radii taken from the ability record (FoC data: 10 / 100 [M]).
7. **Two earlier statements are corrected** (§9):
   - the flying loiter re-issue moves to *destination* + dir × L, not unit + dir × L;
   - the strafe offset only applies when the unit is already close to its target.

---

## 1. Shared conventions and primitives

### 1.1 Orientation layout and angle helpers [H]
- **Orientation** O = obj +0x84 **roll**, +0x88 **pitch**, +0x8c **yaw**, in degrees.
  - The setter 0x3a8710 writes the same triple to a second copy at +0x90/+0x94/+0x98 and sets the dirty bit
    (obj +0x3a1 |= 1).
  - It does nothing while obj +0x3a0 bit 0x10 is set.
  - Locomotors read either copy. They are equal unless something writes one copy alone; none was found on these
    paths. [M]
- **Axes:**
  - yaw is counter-clockwise from +x;
  - **positive pitch is nose down**;
  - forward = (cos Y · cos Pi, sin Y · cos Pi, −sin Pi).
- **wrap180(a)** (0x20b6d0): while a > 180 subtract 360; while a ≤ −180 add 360. Result in (−180, 180].
- **wrap360(a)** (0x20b710): while a ≥ 360 subtract 360; while a < 0 add 360. Result in [0, 360).
- **Degrees to radians:** k = 0.017453292f (bits 0x3C8EFA35).
  - Several functions compute it at run time as (3.1415927f × 2) / 360. That rounds to the same bits (checked).
  - The argument is formed as (±angle) × k.
- **Tiny-angle shortcut** (every sin/cos below): if (bits(x) & 0x7F800000) < 0x1D000001, i.e. |x| < 2⁻⁶⁹,
  then sin x = x and cos x = 1. Otherwise call CRT `sinf` (0x776650) / `cosf` (0x776150).
- **sqrt** = CRT `sqrtf` (0x776d48). **atan2** = CRT `atan2f` (0x78437c).

### 1.2 Angles of a vector (0x20acd0) [H]
Input: from-point A, to-point B. Output: (roll 0, pitch, yaw).
- yaw:
  - 0 if A.x == B.x and A.y == B.y;
  - otherwise atan2f(B.y − A.y, B.x − A.x) × 57.295776f (bits 0x42652EE0), plus 360 if the result is negative.
- pitch:
  - computed only if A.z ≠ B.z **or A.x ≠ B.x** (y is not tested; this is a quirk: a vector along y only
    gets pitch 0);
  - pitch = −(atan2f(B.z − A.z, sqrtf((B.y−A.y)² + (B.x−A.x)²)) × 57.295776f).

### 1.3 Fast normalise (0x16f570) (dis) [H]
- s = (x·x + y·y) + z·z. If s > 0:
  - h = s × 0.5;
  - y₀ = bits(0x5F375A86 − (bits(s) >> 1)) (arithmetic shift);
  - y₁ = (1.5 − (h·y₀)·y₀)·y₀;
  - y₂ = (1.5 − (y₁·h)·y₁)·y₁;
  - return (x·y₂, y·y₂, z·y₂).
- The same two-step pattern appears inline in 0x5c5060 and 0x5070b0. It is the MOVEMENT §7 normalisation.
- 0x139800 is a different normaliser: len = sqrtf(…), inv = 1/len, multiply (when len > 0). It is used by the
  timed follower and the loiter.

### 1.4 Type getters [H]
Let Mm = the mode multiplier `Object_Max_Speed_Multiplier_{Galactic,Space,Land}` (GC +0x15bc / +0x15c0 /
+0x15c4; FoC 5.0 / 1.2 / 1.56). Let G = global 0x140a28568, which is **1.0** in the image (resolves the
"global speed factor" in MOVEMENT §8 item 5). Mm and G are skipped (factor 1) when type flag (+0x790)→byte1 bit0
is set. Every getter below computes (Mm × G) × field.

| name here | getter | field |
|---|---|---|
| turn(T) | 0x372560 | `Max_Rate_Of_Turn` +0x38c |
| roll(T) | 0x3724d0 | `Max_Rate_Of_Roll` +0x394 |
| lift(T) | 0x372440 | `Max_Lift` +0x3a0. **Fighters and flyers use this as the pitch rate** (deg/frame). |
| minspeed(T, obj) | 0x3728f0 | ((Mm×G) × `Min_Speed` +0x3b4) × e. e = GC +0x1530 `Engines_Disabled_Speed_Modifier` (0.4) when obj pack +0xd0 == 0, else 1 [M: +0xd0 = engines enabled] |
| maxspeed(T, obj) | 0x370f00 | effective max speed, MOVEMENT §3.4 (team members capped by the team minimum) |
| thrust(T) | raw | `Max_Thrust` +0x384, **no multiplier**: the per-frame speed-change budget |
| bank(T) | raw | `Bank_Turn_Angle` +0x39c (float; default 70) |

### 1.5 Pack and object fields used here

| field | meaning | conf. |
|---|---|---|
| pack +0x08..+0x10 | D, destination / station point (Idle, parking) | H |
| pack +0x14..+0x1c | V, velocity per frame | H |
| pack +0x38..+0x40 | stored orientation (roll, pitch, yaw) used by Dead_Stop | H |
| pack +0x48 | locomotor state | H |
| pack +0x138/+0x140 | path stack (0x70-byte records). The current path is the last one (0x5569b0). | H |
| path +0x18 | waypoints (0x34 bytes: pos +0x00, facing vector +0x0c, speed +0x18, time +0x30) | H |
| path +0x48 | path kind (1 → use the override speed, vf +0x1c8 = 0x42cc20) | H |
| path +0x60 / +0x64 / +0x6c / +0x6d | current index / cleared on advance / cleared on advance / "started" flag | H |
| pack +0xec | scalar speed (flying) | H |
| pack +0x158 | override / formation speed | H |
| pack +0x15c | lateral path offset used by the look-ahead (§3.6) | L |
| pack +0x1c4 / +0x1c8 | dogfight cell (i, j); −1 = none | H |
| pack +0x1cc / +0x1d0 | parking cell (i, j); −1 = none | H |
| pack +0x1d4 | loop flag (byte, §3.1) | H |
| pack +0x1d8 / +0x1dc | lock timer: start frame (int), duration (int) | H |
| pack +0x1e0 | locked enemy fighter (object pointer) | H |
| pack +0x1f0 | owning formation | H |
| pack +0x248 | path id | M |
| pack +0x278 / +0x280 / +0x284 | circling target object / r_min / r_max (flying) | H |
| pack +0x288 | frame of the last path-stack reset | H |
| pack +0x2b4 | avoidance hold counter (int) | H |
| obj +0x2b0 | C, the team container (squadron) | H |
| C +0x348 | ≠ −1 ⇔ C is a team container | H |
| C +0x110 | team member list: +0x10 members, +0x18 count, +0x78 slot offsets (12 bytes each), +0x88 formation error E, +0x8c..+0x94 spread vector S | H layout, M meaning of E / S |

The **leader** L of a squadron is the first entry of C +0x110 (0x4ec820). Without a squadron, L = self.

---

## 2. Fighter per-frame driver (vf6 0x5cb830) [H]

1. If the pack is missing, return an error.
2. If the locomotor is busy (vf +0x210, the hyperspace states), run the state handler and return:
   4 → 0x5cc970, 5 → 0x5cd010, 0x28 → 0x5cd680, 0x2c → 0x5cbc10 (§4.8).
3. Run the base service 0x4c2f70 [M: LocomotorCommon bookkeeping].
4. **Pitch fold.**
   - p = wrap180(O.pitch).
   - If |p| ≥ 90, set the orientation to:
     - roll = wrap360(O.roll + 180);
     - pitch = wrap360(((180 − |p|) − lift(T)) × (p ≥ 0 ? 1 : −1));
     - yaw = wrap360(O.yaw + 180).
   - The condition is ≥ 90, not > 90.
5. **Squadron bookkeeping** (only when C exists):
   - set C.list +0xbc = 1 [M];
   - if state ∉ {0x1C, 0x1D}: leave the parking grid (§5);
   - if state ∉ {0x1D, 0x1E}: leave the dogfight grid;
   - if the team list exists and its leader's state ≠ 0x1E: set C.list +0xbd = 0 [M].
6. **Dispatch on the state.** "Target" means the formation of (C's pack, else the own pack) has an attack
   target (0x41a2e0).
   - **0:** with a target → state 0x1E and run §4.3 now. Otherwise leave the coordinator (0x5c9ca0), set state
     0x1B and run §4.1.
   - **0x1B Moving:** with a target → state 0x1E and run §4.3 now; otherwise run §4.1.
   - **0x1C Idle:** §4.2.
   - **0x1D Idle_Combat:** nothing.
   - **0x1E Directed_Combat:** §4.3.
   - **0x1F Dead_Stop:** §4.6.

---

## 3. Fighter steering primitives

### 3.1 Steer to a point: `steer_to(A, v, &tB, &thB, &lB)` (0x5caaf0) [H]
The budgets are passed by reference and consumed. A 7th argument (0/1) is passed by every caller and **never
read** (dis).

1. If thB ≤ 0 or tB ≤ 0, return. Nothing changes; the caller still moves by V.
2. **Target in the body frame** (dis). Let a = −(k·Pi), b = −(k·Y) (§1.1 shortcut applies), sa = sin a,
   ca = cos a, sb = sin b, cb = cos b. The rows are:
   - r0 = (ca·cb, −(ca·sb), sa);
   - r1 = (sb, cb, 0);
   - r2 = (−(sa·cb), sa·sb, ca).

   With d = A − P, the body vector is ℓ = (((r·d.x + r·d.y) + r·d.z) + 0) for each row, summed in that order.
   (This is the identity matrix rotated by 0x2cf9e0(a), then by 0x480f0(b). The closed forms above are exact.)
3. (·, rp, ry) = angles(origin → ℓ) (§1.2). RelYaw = wrap180(ry), RelPitch = wrap180(rp). Positive RelPitch means
   the target is below.
4. **Loop (Immelmann) flag**, pack +0x1d4:
   - If |RelYaw| < 90, clear the flag and go to the test below.
   - If |RelYaw| ≥ 90 and the flag is set, go straight to "loop".
   - Test: if state ≠ 0x1C and |RelYaw| > 170 and lift(T) > 0, go to "loop". Otherwise keep the angles.
   - **Loop:** RelPitch = (RelPitch > 0 ? 180 : −180), RelYaw = 0, set the flag.
   - So a target well behind is reached by a half-loop in pitch, not a flat turn, and the flag keeps the loop
     going until the target is back in front (|RelYaw| < 90).
5. **Roll anticipation:**
   - R = wrap180(O.roll), rr = roll(T), tr = turn(T).
   - If 180 < |tr × (R / rr) + RelYaw|, then RelYaw −= (RelYaw ≥ 0 ? 1 : −1) × 360.
6. Run, in order: `yaw_bank_step(RelYaw, &tB)` (§3.2), `pitch_step(RelPitch, &lB)` (§3.3),
   `speed_step(v, &thB)` (§3.4).
7. Debug-draw branch (flag 0x140b395e4): render only, ignore.

### 3.2 Yaw with bank (0x5c95a0) [H]
Input dy (degrees) and the turn budget B (by reference).
1. dy = wrap180(dy); bank = clamp(dy, −bank(T), +bank(T)).
2. Read O = (R0, Pi, Y). tr = turn(T). R = wrap180(R0).
3. e = wrap180(−bank − R). The **target roll is −bank**: a left turn (positive dy) rolls negative.
4. rb = roll(T) × (B / tr) (dis), clamped to [0, 180] (negative → 0).
5. If rb < |e|: R += rb (e > 0) or R −= rb (e < 0). Otherwise R += e.
6. **Yaw gate:**
   - Let s(x) = (x ≥ 0 ? +1 : −1).
   - If R ≠ 0 and s(−bank) ≠ s(R): set B = 0 and leave yaw unchanged (still banked the wrong way).
   - Otherwise:
     - if B < |dy|: Y −= B when dy ≤ 0, else Y += B; then B = 0;
     - else Y += dy and B −= |dy|.
7. Set the orientation to (wrap360(R), Pi unchanged, wrap360(Y)).

### 3.3 Pitch step (0x5c8fb0) [H]
- p = wrap180(O.pitch).
- If B < |dp|: p += B (dp > 0) or p −= B (dp < 0), then B = 0.
- Otherwise p += dp and B −= |dp|.
- Set the orientation to (roll unchanged, wrap360(p), yaw unchanged).

### 3.4 Speed and velocity (0x5c9360) (dis) [H]
1. s = sqrtf((Vx² + Vy²) + Vz²). e = v − s.
2. If B ≤ |e| (note ≤): s += B (e ≥ 0) or s −= B, then B = 0. Otherwise s += e and B −= |e|.
3. Read the **current** O (after §3.2–§3.3).
   - cp = cos((−Pi)·k), sp = sin((−Pi)·k), cy = cos(Y·k), sy = sin(Y·k), with the shortcut.
   - V = ((cy·cp)·s, (sy·cp)·s, sp·s).

The caller then commits P' = P + V through the non-forced position setter (0x3a8f90). Moving and Idle pass the
point through the land clamp §3.7 first.

### 3.5 Obstacle avoidance: `avoid(&A, tB, thB, lB)` → bool (0x5c8b70) [H; M for the hit flag]
Space only: the tree is GOM +0x558 (TACTICAL_COMBAT §1.5). With no tree, return false.

1. tr = turn(T). Tl = 2 / (tr × k), i.e. frames to turn 2 radians. If self is its squadron's leader, Tl ×= 2.
2. s = |V|. Query_AABB on the tree with a box centred on P with half-extent s on every axis:
   - centre = ((P−s)+(P+s))·0.5;
   - half = |(P+s)−(P−s)|·0.5.

   Result list order is TACTICAL_COMBAT §1.6: reverse collection order.
3. Segment: from P to E = (Tl·Vx + Px, Tl·Vy + Py, Tl·Vz + Pz).
4. For each candidate c in list order:
   - Run the precise model test (0x38f900) of c against the segment.
   - If it hits and hit-record byte 1 == 0 [corrected in SPACE_AVOIDANCE.md: byte 1 = the segment starts inside a COLLISIONBOX; the record is shared across candidates and never reset; fighters are never tree members]:
     - away = P − c.P; if away == 0 use (1, 0, 0);
     - steer_to(away + P, avoid_speed(), tB, thB, lB);
     - set pack +0x2b4 = 15;
     - return true.
5. No hit: if pack +0x2b4 > 0, decrement it and **overwrite the caller's aim point** with A = (Vx·10 + Px,
   Vy·10 + Py, Vz·10 + Pz). So for 15 frames after an evasion the fighter keeps flying straight. Return false.
6. Headless note: the precise test needs model collision meshes (render object obj +0x2a0). Without them, avoidance
   finds nothing.

### 3.6 Formation / path follow: `follow(v_req, z_goal)` (0x5c9e90) [H]
1. **Setup.**
   - C = obj +0x2b0. If C exists but has no team behaviour or no members: commit P += V and return.
   - L = the leader (or self).
   - v = station_speed(v_req, 0.5·v_req, 1.5·v_req) (below).
   - tB = turn(T), thB = thrust(T), lB = lift(T).
   - If maxspeed(T, obj) < v: f = v / maxspeed, and tB, thB, lB are each multiplied by f.
2. **Aim inputs.**
   - dir = L.V; if L.V == 0, dir = (cos L.Y, sin L.Y, 0) (0x41c000).
   - off = slot(self) − slot(L) in the team (0x404c60 returns the slot offset; zero vector outside a team).
   - Debug flag 0x140b395e5 (0 in shipping) would set z_goal = P.z.
3. la = max(v, |V|).
4. **Look-ahead** A = lookahead(C or self, L.P, dir, self == L, la) (0x5ca860):
   - **No usable path** (not the leader, or the container has no current path):
     - d = normalise(dir) (§1.3);
     - solo fighter: d ×= max(la × 1.1, 5);
     - in a squadron: d ×= ((360 / turn(T)) × la) / 6.2831855, i.e. one **turn radius**.
   - **Leader with a path:**
     - d = wp[idx+1] − wp[idx] (0x5c5850; unchanged if idx+1 ≥ count), normalised.
     - A solo fighter on the second-to-last waypoint aims exactly at wp[idx+1] (0x5c54c0) and returns.
     - Otherwise, with f = the turn radius as above and o = container pack +0x15c [L]:
       d = (d.x·f + (−d.y)·o, d.y·f + d.x·o, d.z·f + o·0).
   - A = base + d.
5. **Glide slope:** if lB > 0:
   - q = (|wrap180(O.pitch)| / lB) × la (the distance needed to level out);
   - if q > 0 and h = sqrtf((A.y−P.y)² + (A.x−P.x)²) < q: z_goal = (h × (z_goal − P.z)) / q + P.z.
6. Aim = (A.x + off.x, A.y + off.y, off.z + z_goal).
7. If !avoid(&Aim, tB, thB, lB): steer_to(Aim, v, tB, thB, lB).

**`station_speed(v, lo, hi)`** (0x5ca4c0) [H]: holds a member on its slot along the track.
1. Applies only inside a squadron whose leader L exists. Otherwise return v.
2. lo = min(lo, hi), hi = max(lo, hi).
3. û = L.V / |L.V| (when |L.V| > 0). sL = |L.V|.
4. Along-track slot error:
   e = ((P.y − L.y) + slotL.y − slotS.y)·û.y + û.x·((slotL.x + (P.x − L.x)) − slotS.x)
       + ((P.z − L.z) + slotL.z − slotS.z)·û.z.
   Positive e means ahead of the slot.
5. s = |V|, a = min(lo, s), b = max(hi, s).
   - accelDist = (b² − v²) / (thrust(T) × 2);
   - decelDist = (v² − a²) / (thrust(T) × 2).
6. Tol = the squadron type's `Squadron_Formation_Error_Tolerance` (+0x9b0; FoC 25 X-wing, 35 TIE).
   E = the formation error (C list +0x88; 1e18 without a list).
7. If Tol < |E|:
   - **e > Tol (ahead):** if decelDist > 0, t = clamp(e / decelDist, 0, 1) and return (1−t)·sL + t·lo; else
     return lo.
   - **e < −Tol (behind):** if accelDist > 0, t = max(−e / accelDist, 0) (no upper clamp) and return
     (1−t)·sL + t·hi; else return hi.
8. Return v.

For a leader e = 0, so the leader always gets v.

### 3.7 Land clamp (0x5c9090) [H]
Identity outside land mode. On land (birds use FIGHTER_LOCOMOTOR) it keeps the next position above terrain:
- for i = 0..59, sample p = P' + i·V;
- inside the map box, take h = max(terrain(p) + `Hover_Offset` (+0x1b68), map floor (0x136660) + 5);
- let H = the maximum h;
- z steps toward H by at most max(lift(T), 0.5).

### 3.8 Speed helpers [H]
- **avoid_speed()** (0x5ca430) = minspeed(T, obj).
  - If (C's or the own) pack has a formation whose flag bit 1 (+0x48) is set and +0x28 is non-null, use
    min(minspeed, 0.5 × formation speed (+0x21c)) instead.
- **formation_speed_or_max()** (0x5ca390) = formation +0x21c under the same condition (and only when the pack has
  a current path); otherwise maxspeed(T, obj).

### 3.9 Cone test `cone(A, from, O, yawLim, pitchLim, R²)` (0x5cb1f0) [H]
1. False if |A − from|² > R², with the 3-D sum ((dx² + dy²) + dz²).
2. Otherwise compute RelYaw / RelPitch exactly as §3.1 steps 2–3, using O's pitch (+4) and yaw (+8).
3. True iff |RelYaw| ≤ yawLim and |RelPitch| ≤ pitchLim.
4. The relative angles are returned. The chase in §4.5 uses them directly.

---

## 4. Fighter states

### 4.1 Moving 0x1B (0x5ce010) [H]
1. Modifier event: the out-of-combat defence modifier (`Out_Of_Combat_Defense_Adjustment`, type +0xde0) is
   pushed to the object's modifier sources (obj +0xf0). This is combat, not movement [M].
2. path = the **own** pack's current path. With no path: commit P' = clamp(P + V) and return (coast).
3. Set path +0x6d = 1.
4. **Waypoint pass test** (0x5c5060, [M] for the corner shift):
   - (W, w) = waypoint idx+1 and the unit bisector of the incoming and outgoing segment directions (2-D, §1.3
     normalisation).
   - For path kind 1, W is shifted along the incoming segment by (len + path +0x68) when |path +0x68| > len
     (len from 0x54f700 [L]).
   - Passed ⇔ w.x·(P.x − W.x) + w.y·(P.y − W.y) ≥ −0.001.
5. **Leader of a squadron:**
   - If passed, advance the waypoint index of **every member** that has a current path, in team index order
     (0x5c4920: idx += 1, +0x64 = 0, +0x6c = 0).
   - If the path is complete (idx + 1 ≥ count) and state ≠ 0x1E:
     - for every member m: m.D = (W_last.x, W_last.y, `Layer_Z_Adjust` (+0xdfc)) and m's stored orientation =
       the leader's pack +0x38..+0x40;
     - if m has a path, clear its timing (0x5c5dc0) and call m's enter_idle (§4.7).
     - Then coast this frame.
6. **Solo fighter:** same pass test on its own path. If complete: D = (W_last.xy, Layer_Z_Adjust), enter_idle, and
   coast.
7. **Otherwise** (members, or the path is not complete):
   - v = formation_speed_or_max(); if the current path kind is 1, v = (C's or own) pack +0x158;
   - follow(v, Layer_Z_Adjust);
   - commit P' = clamp(P + V).

### 4.2 Idle 0x1C (0x5cd8e0) [H]
This replaces the port's [L] `fighter_station_step`.

> **Status 2026-10-30:** The replacement is in: the Idle 0x1C steps of this section are implemented in `engine/tactical/movement/include/alamo/tactical/movement/fighter.hpp` (see the §4.2 helpers there) and `fighter_station_step` is gone from `engine/`.
1. tB = turn(T), thB = thrust(T), lB = lift(T). Modifier event as §4.1.
2. **Solo fighter** (no team):
   - v = GC +0x1554 `SpaceIdleMovementSpeed` (FoC 0.0); on land v = avoid_speed();
   - if P is outside the playable box (0x59e5b0), v = 0;
   - yaw_bank_step(0, &tB) (unwinds bank, no yaw);
   - pitch_step(−wrap180(O.pitch copy +0x94), &lB) (levels);
   - speed_step(v, &thB).
3. **In a squadron:**
   1. Publish the own D into the team list +0xa4..+0xac and clear list +0xbc [M].
   2. D = L.pack.D (a local copy).
   3. u = (P − D).xy; if u = 0, u = (1, 0). r = sqrtf((u.x² + u.y²) + 0). If r > 0, u ×= 1/r (u.z = (1/r)·0).
   4. rl = max(1, sqrtf((L.x−D.x)² + (L.y−D.y)²)).
   5. v = avoid_speed(). If rl < 90:
      - v = (r / rl) × 0.5, for every squadron fighter;
      - **leader only:** orbit. a = 10 / r (radians), s = sin a, c = cos a (shortcut), and
        D.x += ((c·0 − s)·u.y + (s·0 + c)·u.x + u.z·0) × 60,
        D.y += ((c − s·0)·u.y + (c·0 + s)·u.x + u.z·0) × 60,
        D.z unchanged in effect.
        That aims 60 units out at an angle 10/r ahead around D, so the leader circles D slowly.
   6. Aim:
      - member: A = (L.V × 40 + L.P) + (slot(self) − slot(L)) × 0.75 (dis; the same grouping on all three axes);
      - leader: A = D.
   7. If !avoid(&A, tB, thB, lB): steer_to(A, v, tB, thB, lB).
4. Commit P' = clamp(P + V).
5. If (C's or own) formation has an attack target that is not dying (+0x3a0 bit 2 clear): state = 0x1E (it runs
   next frame).

### 4.3 Directed_Combat 0x1E (0x5cc220) [H]
1. **Setup.** C, team, L as usual. Modifier event with value 0 [M]. hp = −1. F = C's pack (or own).
2. **Target resolution.**
   - No formation on F → **NoTarget**.
   - tgt = formation attack target, with hard-point index hp (0x41a2e0).
   - **Retarget** if tgt is null or dying (+0x3a0 bit 2):
     - if C exists and C's dogfight cell (+0x1c4) ≥ 0, walk that dogfight cell's object list in **join order**
       (oldest first, §5);
     - for each object o that is not dying and is hostile to C (0x39a450): order C to attack o (0x38d730), then
       tgt = C's current attack target (0x397a40);
     - stop at the first non-null tgt.
     - Still none → **NoTarget**.
   - tgt dying, or hidden by fog for the own player (0x35f470, force = 1) → **NoTarget**.
3. **Approach test.** Run it unless F and tgt are already in the **same** dogfight cell (equal +0x1c4 and +0x1c8,
   with F's ≠ −1):
   - T = tgt.P, or the hard-point position (0x396ce0) when hp ≠ −1;
   - Rs = `Strafe_Distance` (own type +0x2118; FoC fighters 500) + radius(tgt) (0x397780);
   - X = L (in a team) or self.
   - If Rs² < (T.x − X.x)² + (T.y − X.y)², **approach**:
     - leave the dogfight grid (C or self);
     - if F has a current path that is not complete, run Moving (§4.1) and return;
     - otherwise, if !avoid(&T, turn, thrust, lift): steer_to(T, formation_speed_or_max(), …);
     - commit P += V and return.
4. **Attack.**
   - The leader resets the path stack of every member, then C's own pack (0x554480 with the pack's +0x248: the
     whole stack is cleared, the object re-registered and +0x288 = now). A solo fighter resets its own.
   - tgt is not a team (tgt +0x348 == −1): leave the dogfight grid, then attack_run(tgt, hp) (§4.4).
   - Otherwise: dogfight(tgt) (§4.5).
   - Commit P += V.
5. **NoTarget.**
   - If F has a current path that is not complete: Moving (§4.1).
   - Solo fighter: D = P + V, stored orientation = O, enter_idle (§4.7).
   - Squadron:
     - leave the dogfight grid (C);
     - if self == L: clear the team's target (0x4061d0) [M], then for each member m (index order):
       m.D = (L.x + L.V.x, L.y + L.V.y, Layer_Z_Adjust(self type)), m's stored orientation = L's, then m's
       enter_idle.
   - Commit P += V.

### 4.4 Attack run vs a ship, structure or hard point (0x5cedf0) [H]
Inputs: tgt, hp.
1. T = tgt.P or the hard-point position. X = L or self.
2. **Members** (X ≠ self): follow(maxspeed(T, self), T.z), i.e. fly the formation slot at the target's height.
   Return.
3. **Leader or solo fighter:**
   1. vmax = maxspeed, vmin = minspeed. v = station_speed(vmax, vmin, vmax × 1.5) (equals vmax for a leader).
   2. tB = turn(T), thB = thrust(T), lB = lift(T); if maxspeed < v, scale the three budgets by v / maxspeed.
   3. R² = (`Strafe_Distance` + radius(tgt))².
   4. If (T.x − X.x)² + (T.y − X.y)² ≤ R² (inside the strafe circle, 2-D):
      - **Firing pass:** if cone(T, X.P, X.O, 45, 45, R²) (3-D distance and a ±45° body cone): if
        avoid(&T, …) return; else steer_to(T, v, …).
      - **Break-off:** otherwise follow(maxspeed, T.z). The path stack was cleared in §4.3, so the look-ahead is
        one turn radius ahead along the own velocity: the fighter flies straight on, adjusting its height toward
        T.z.
   5. Otherwise (outside the circle) — **approach / re-attack:** if avoid(&T, …) return; else steer_to(T, v, …).
      With the target behind, the loop rule (§3.1) turns it around.
4. **The cycle:**
   - outside the circle the fighter turns back to the target;
   - entering the circle facing it is the firing pass;
   - when it passes the target (|RelYaw| or |RelPitch| > 45) it breaks off straight ahead until it leaves the
     circle, then re-attacks.
5. **Timers and randomness:** none in the run itself. Fire control is the combat system's (TACTICAL_COMBAT); it
   is not driven from here.

### 4.5 Dogfight vs a squadron (0x5ce4f0) [H; M for the provocation flags]
Inputs: tgt (a team container). Let G_D = the dogfight grid (§5), "range" = max attack range (0x3749d0; plain
fighters: `Targeting_Max_Attack_Distance` +0x500, FoC 450–500) [M]. tB = turn, thB = thrust, lB = lift. A = tgt.P.
c = tgt.pack.dogfight cell.

1. **Target not in a cell** (c.i < 0 **and** c.j < 0):
   - c = lookup(G_D, tgt, register = false) (0x4f9c60);
   - members (self ≠ L): follow(maxspeed, tgt.P.z) and return;
   - leader: leave G_D, then write the own (C's) cell = c **without joining** the cell list. Go to the tail.
2. **Target in a cell:** A = centre(G_D, self, c) (z = own `Layer_Z_Adjust`).
   - If 80000 < (P.x−A.x)² + (P.y−A.y)² (farther than ≈282.8): leave G_D, join(G_D, C-or-self, c). Members:
     follow(maxspeed, Layer_Z_Adjust) and return. Leader: go to the tail (fly to the cell centre).
   - Otherwise (at the fight):
     1. Leave G_D and write the own cell = c.
     2. **Provocation:** E = tgt's leader (or tgt), tt = tgt's current attack target. If E exists, E.state ≠ 0x1B,
        (tt is null or not a team), and tgt has neither unit ability 0x29 nor 0x22 active (0x39b950): order tgt to
        attack (C, or L when solo) (0x38d730).
     3. If tgt's cell i ≠ −1: join(G_D, C-or-self, tgt's cell).
     4. **Lock.** O = pack +0x1e0, timer = pack +0x1d8.
        - If O ≠ null and the timer has **not** expired:
          - if cone(O.P, P, O_self, 90, 90, range²): chase(O) and return;
          - else A = O.P; if avoid(&A, …) return; steer_to(A, formation_speed_or_max(), …); return.
        - If the timer **has** expired (whether or not O exists), scan G_D's cell c in join order:
          - for each entry o hostile to the own owner (0x39a4e0):
            - if o is a single object, the candidate is o;
            - otherwise each member of o's team in index order is a candidate.
          - A candidate qualifies if its own lock timer has expired and cone(cand.P, P, O_self, 90, 90, range²)
            holds.
          - The first qualifying candidate x is locked: lock(x), then chase(x), return.
        - If O == null and the timer is still running, nothing is scanned.
3. **Tail** (nothing chased): members follow(maxspeed, Layer_Z_Adjust); the leader runs
   steer_to(A, maxspeed, tB, thB, lB) with A = the cell centre (or tgt.P after step 1).

**lock(x)** [H]:
- self +0x1e0 = x; self timer = (start = mode frame, duration = int(float(FPS) × 10.0) = 300);
- x +0x1e0 = null; x timer = (now, 300). So the victim cannot lock back for 10 s.
- Death signals are re-subscribed (0x220e90 / 0x220eb0, not sim-relevant).
- Expired ⇔ start + duration ≤ mode frame (0x4ad350; GALACTIC_ECONOMY §timer).

**chase(x)** [H]:
1. If thB == 0 or tB == 0, return.
2. yaw_bank_step(cone.RelYaw, &tB); pitch_step(cone.RelPitch, &lB). This uses the cone angles directly: no loop
   rule and no avoidance.
3. If x has no pack, return.
4. d² = (dx² + dy²) + dz². If `Minimum_Follow_Distance`² (+0x534; FoC 25 X-wing, 50 TIE) ≤ d², v = |x.V| (match
   its speed); otherwise v = avoid_speed().
5. speed_step(v, &thB).

### 4.6 Dead_Stop 0x1F (0x5cbea0) [H flow; M for E / S / extent]
1. tB, thB, lB as usual. Modifier event.
2. **Settled** if not in a team, or if |E| ≤ Tol and |S| ≤ 1.33 × Rf, where:
   - E = C list +0x88;
   - Tol = `Squadron_Formation_Error_Tolerance`;
   - S = C list +0x8c..+0x94;
   - Rf = 0x404cd0 = the longest slot offset (first maximum) + the radius of the first member's type (0x373b30).

   Settled: yaw_bank_step(wrap180(stored.yaw − O.yaw), &tB), pitch_step(wrap180(stored.pitch − O.pitch), &lB),
   speed_step(0, &thB). So it turns to the stored orientation and brakes by Max_Thrust per frame.
3. **Otherwise (regroup):**
   - vmin = minspeed;
   - leader: A = (2·vmin·cos sY + P.x, 2·vmin·sin sY + P.y, 2·vmin·0 + P.z), with sY = stored yaw;
     if !avoid(&A, …), steer_to(A, vmin, …);
   - members: follow(vmin, Layer_Z_Adjust).
4. Commit P += V.
5. Who sets 0x1F is **untraced** (probably the Stop order).

### 4.7 enter_idle (0x5cb750) [H]
- **Solo fighter:** state 0x1C, leave the coordinator (0x5c9ca0), reset the path stack.
- **In a squadron:**
  1. leave the parking grid;
  2. clear the team's two target slots (0x4061f0 / 0x4061d0) [M];
  3. state 0x1C, leave the coordinator, reset the path stack;
  4. **register a parking cell** (0x4fafd0 = lookup(G_P, self, register = true), §5). This overwrites D (own and
     C's) with the cell centre, so every idle squadron gets its own 120-unit slot.

### 4.8 Hyperspace states [H]
These match L1_S4_REMAINING §3.4 (states 4, 5, 0x28, 0x2c), which this document does not repeat. Fighter specifics:
- **State 4 (0x5cc970):** one synced int(1, 4) on the first frame, as documented there. Each frame
  position += V × profile(timer), with V **not normalised**.
- **State 0x2c (0x5cbc10):** V is **normalised** first (sqrtf, 1/len), then position += V̂ × profile(timer). No
  draw; SFX at 45; REVEAL hook at 120; finish at ≥ 150 when pack +0x260 == 1.
- **State 0x28 (0x5cd680):** step along obj +0x98 (yaw): 8.125/frame while the timer > 90, then the faction's
  `Space_Retreat_Flight_Move_Increment`. Killed at 0.
- The profile table 0x140b27a10 is still unread (MOVEMENT §8 item 5).

---

## 5. Fighter cell grids (singleton 0x404950) [H; M for the ring order]

- **Structure.** The singleton holds two grids of equal layout: G_D at +0x00 and G_P at +0x38. Each grid has:
  - +0x00 cells (0x48 bytes each; an intrusive object list plus a count at +0x30);
  - +0x08 nx, +0x0c ny;
  - +0x10 centre (cx, cy, cz), +0x1c half-extents (hx, hy, hz);
  - +0x28 cell size, +0x2c capacity, +0x30 extra, +0x34 flag.
- **Build** (0x4faf40, at space-mode start; callers 0x4d7820 / 0x4d80b0 / 0x4d8750 [M]). The input is the playable
  box (mode vf +0x1f8). nx = int(((hx + cx) − (cx − hx)) / size), ny the same in y.
  - **G_D, dogfight grid:** size 400, capacity 512, extra 512, flag 1. The reference point is the object's
    position; the cell is stored in pack +0x1c4.
  - **G_P, parking grid:** size 120, capacity 1, extra 0, flag 0. The reference point is pack D; the cell is
    stored in pack +0x1cc.
- **Save/load:** chunked (0x4fad30, 0x4f9e10, 0x4fa170). Cell membership is saved; the port must save it too.
- **centre(grid, obj, i, j)** (0x4fa6f0):
  - x = ((cx − hx) + i·size) + size/2, plus size/2 when j is odd (rows are staggered);
  - y = ((cy − hy) + j·size) + size/2;
  - z = obj type `Layer_Z_Adjust`.
- **join(grid, obj, i, j)** (0x4fa540):
  - obj → its container when it has one. Requires i, j ≥ 0 and j·nx + i < nx·ny.
  - Flag 0 only:
    - fails if extra + capacity ≤ the cell's object count;
    - fails if the number of objects in the cell **allied** to obj's owner (0x39a230) ≥ capacity.

    For G_P this means the cell must be empty.
  - Inserts at the list tail (duplicates refused). Traversal is from the head, so lists are in **join order**.
  - Writes the cell (i, j) into the container's pack.
- **leave(grid, obj)** (0x4f9d50): obj → container; if its stored cell is valid, unlink it and store (−1, −1).
- **lookup(grid, obj, register)** (0x4fb160):
  1. ref = obj.P (flag 1) or pack D (flag 0).
  2. j = int((ref.y − (cy − hy)) / size).
  3. x = ref.x, or ref.x − size/2 when j is odd; x = max(x, cx − hx).
  4. i = int((x − (cx − hx)) / size).
  5. Clamp i to [0, nx−1] and j to [0, ny−1]. The float → int conversion truncates toward zero.
  6. **Already assigned** (stored cell ≥ 0): return it. Flag 0 also rewrites D (own and container) to its
     centre.
  7. **Flag 0 and register:** try join(i, j). On success, return it and set D = centre.
  8. **Otherwise ring search** [M for the enumeration]:
     - first pass: the 3×3 block around (i, j), including it; then the border of each larger square, up to 64
       evaluated cells;
     - each cell is a candidate if flag 1, or (allies < capacity and no non-allied objects);
     - its key is the 3-D squared distance ((cy−ry)² + (cx−rx)² + (cz−rz)²) from its centre to ref; on G_D an
       **occupied** cell gets key 0 (snap to an existing fight);
     - after each completed ring, take the smallest key (first minimum, strict <);
     - if register and join fails: return nothing;
     - otherwise return it, and on flag 0 set D = centre.

     After 64 evaluations with nothing found, nothing is returned.
  - The candidate vector is a static scratch: not re-entrant.

---

## 6. Flying locomotor (land) [H unless tagged]

### 6.1 The timed follower vf +0x1d8 (0x63c340) — answer to REMAINING_GAPS §4.8 item 2 [H]
**It is the SimpleSpace Hermite** (same basis table 0x140b2f1f0, same tangents), with two differences:
- strict bracketing;
- an added bank computation.

The jetpack's vf +0x1d8 (0x641010) uses the same basis and strict bracketing [M: not compared line by line].

**Basis.**
- LocomotorCommon's constructor (0x42b9f0 → static init in 0x42b9f0's guard) fills the 4×4 matrix
  [[1,0,0,0],[1,1,1,1],[0,1,0,0],[0,1,2,3]] and inverts it in place with a Gauss–Jordan routine (0x2324a0: partial
  pivoting on the largest |a|, strict <; multiply by 1/pivot).
- Emulated in float32, the result is exact:

| row | p0 | p1 | m0 | m1 |
|---|---|---|---|---|
| a0 | 1 | 0 | 0 | 0 |
| a1 | 0 | 0 | 1 | 0 |
| a2 | −3 | 3 | −2 | −1 |
| a3 | 2 | −2 | 1 | 1 |

  So MOVEMENT §2.5's "assume the standard Hermite basis [M]" becomes [H].
- Coefficients are a_r = ((p0·B[r][0] + p1·B[r][1]) + m0·B[r][2]) + m1·B[r][3], per axis.

**Inputs:** path, T = the global frame counter 0x140b0a320 (uint → float), current P, O, speed s₀ (pack +0xec).
**Outputs:** P₁, O₁, s₁.

1. Precompute cp = cos((−Pi)·k), sp = sin((−Pi)·k), sy = sin(Y·k), cy = cos(Y·k) (shortcut).
2. **Extrapolate** (no path, or the path is complete) = P₁ = ((cy·cp)·s₀ + P.x, (sy·cp)·s₀ + P.y, sp·s₀ + P.z),
   O₁ = O, s₁ = s₀.
3. **Bracket:** while wp[idx+1].time < T (**strict**: `t ≤ T && T ≠ t`): advance idx (0x5c4920); if complete,
   extrapolate. (SimpleSpace 0x625990 advances while t ≤ T.)
4. Δ = wp[idx+1].time − wp[idx].time. If Δ ≤ 0: advance idx and extrapolate.
5. u = clamp((T − t0) / Δ, 0, 1).
6. **Zero-length segment** (identical positions):
   - P₁ = P, s₁ = 0;
   - yaw = yaw of wp0.facing + wrap180(yaw(wp1.facing) − yaw(wp0.facing)) × u, using §1.2 on the facing vectors
     (+0x0c) and the angle add/subtract helpers 0x20b100 / 0x20b070 (yaw wrapped to [0, 360)).
7. **Otherwise:**
   - p0 = wp0.pos, p1 = wp1.pos;
   - m0 = ((s_wp0·f̂0.x)·Δ, (s_wp0·f̂0.y)·Δ, (s_wp0·f̂0.z)·Δ), with f̂ = the facing vector normalised by 0x139800
     and s_wp = waypoint speed +0x18; m1 the same from wp1;
   - u2 = u·u, u3 = u·u2;
   - P₁.x = ((a1·u + a0) + a2·u2) + a3·u3, and the same for y and z (z is overridden below);
   - derivative D = ((a2·2)·u + a1 + (a3·3)·u2) / Δ, per axis.
   - |D| = sqrtf((D.y² + D.x²) + D.z²). If |D| ≤ 0.0001, O₁ = O; else (·, pitch, yaw) = angles(0 → D).
     s₁ = |D|.
   - **Bank:**
     - dy = wrap180(yaw₁ − yaw₀) (via 0x20b100); g = clamp(|dy / turn(T)|, 0, 1); R = wrap180(O.roll),
       rr = roll(T);
     - if |dy| ≤ 0.01: R decays toward 0 by rr (R > 0: max(R − rr, 0); R < 0: min(R + rr, 0));
     - else R = R − g·rr when dy > 0, or R + g·rr when dy ≤ 0;
     - roll₁ = clamp(R, −bank(T), +bank(T)).
   - **O₁ = (roll₁, O.pitch (unchanged), yaw₁). P₁.z = P.z (unchanged).** Height and pitch belong to §6.3.
8. **Waypoint times:** the times (+0x30), speeds and facings are written by the path producer: the timed lattice
   planner (MOVEMENT §2.1) for land jetpack and flying paths [M, as REMAINING_GAPS §4.3]. The follower adds no
   time scaling: one path time unit = one logic frame.

### 6.2 Attitude step (0x63d530) [H]
Inputs: current O_c, desired O_d, yaw budget B_y, pitch budget B_p (by reference), and gate.
1. d = O_d − O_c (0x20b100). dy = wrap180(d.yaw). Y = wrap180(O_c.yaw), R = wrap180(O_c.roll), rr = roll(T).
2. **Yaw.** Done unless gate is set and s(R) ≠ −s(dy) (s as §3.2). That is, with the gate the yaw only turns once
   the roll is on the turning side (a left turn needs R < 0; R = 0 with dy ≥ 0 blocks).
   - If |dy| ≤ B_y: Y += dy and B_y −= |dy|; otherwise Y ±= B_y and B_y = 0.
3. **Roll** (always): e = (−dy × 5) − R (dis order); R ± rr if |e| ≥ rr, else R += e. Clamp to ±bank(T). The bank
   target is −5 × the remaining yaw error.
4. **Pitch:** dp = wrap180(d.pitch), p = wrap180(O_c.pitch); step by at most B_p as in §3.3.
5. Out = (R, wrap180(p), wrap180(Y)). Note the (−180, 180] wrap, unlike the fighter's [0, 360).

### 6.3 Altitude and pitch step (0x63b920) [H; M for the formation destination]
Inputs: frame, &P (already moved in XY), &O, speed s, useTrack.
1. **Limits.**
   - maxPitch = 30; M = 0.
   - If the pack has a formation:
     - Q = its move destination clamped to the playable box, with z clamped to ±5000 (0x583150 → 0x583680) [M];
     - (·, q, ·) = angles(P → Q); M = Q.z;
     - if wrap180(q) > 0 (Q below): maxPitch = min(wrap180(q), 30).
2. With no object-tracking system (mode vf +0x238), return without changes.
3. **Terrain look-ahead.** floor = map floor (0x136650). f = (cos Y, sin Y) (0x41c000). x, y = P.x, P.y. For
   i = 0..59:
   - **Sample:**
     - if useTrack and the tracker has a position for (obj id, frame + i) (0x538830): p = that position.
       - If p equals the current (x, y, z) exactly, clear useTrack and use the current point (no advance).
       - Otherwise also take the tracker's facing for that frame as f (0x538510).
     - Else p = (x + f.x·s, y + f.y·s, z).
     - Then (x, y) = p.
   - If p is inside the map box (|p.x−c.x| ≤ hx and |p.y−c.y| ≤ hy):
     - h = max(terrain(p) + `Hover_Offset` (+0x1b68), floor);
     - if h > M: h = (h − M) × (1 − float(i) / 60) + M;
     - M = max(M, h).
4. **Clearance:** M = clearance(P, last sample, M) (0x63b640, §6.3.1), once, after the 60 samples. It raises M to
   the box top of every Corvette-layer (4) object that the flyer's 2-D swept footprint P → last sample touches at
   the current frame.
5. **Pitch target.**
   - dz = M − P.z;
   - c = clamp(dz, −100, 100);
   - L = maxPitch when c ≤ 0 or dz < −100, and 30 when c > 0 (climbs are always limited to 30);
   - t = clamp(c / 200 + 0.5, 0, 1);
   - target pitch Pt = (−L − L)·t + L, which runs from +L (dive, at dz ≤ −100) to −L (climb, at dz ≥ +100).
6. **Pitch step:** p = wrap180(O.pitch); move toward Pt by at most lift(T) (Max_Lift × multiplier). O.pitch = p.
7. **Height:** P.z = ((−s)·sin(p·k)) / cos(p·k) + P.z (shortcut: sin = arg, cos = 1). The horizontal speed is
   kept; climb = s·tan(−p).
8. Clamp P.z ≥ terrain(P.x, P.y) and P.z ≥ floor. This clamps to the bare terrain, not terrain + Hover_Offset.

#### 6.3.1 Clearance query (0x63b640) [H unless tagged]
Inputs: flyer F, from = P (already moved in XY), to = the last sample of step 3, M. Output: M' ≥ M. Only the x and y
of from / to are read.
1. **Layer.** T = the mode's object-tracking system (mode vf +0x238); L = T's layer whose id is 4 (0x5387f0, linear
   search). No T or no L: return M.
   - Layer ids are SpaceLayerType values (enum 0x5e0c00): None 0, Capital 1, Frigate 2, **Corvette 4**,
     StaticObject 8, Land 0x10, SuperCapital 0x20. The system always makes the six layers 1, 2, 4, 8, 0x10, 0x20
     (ctor 0x537b30): 8 is one static tree, 0x10 one current tree, the others are rings of time slices. (MOVEMENT
     §2.2's "Fighter 8 [M]" and "0x10 unnamed" should read StaticObject 8 and Land 0x10.)
2. **Query box** (init 0x5c8a30, search 0x5d7c00):
   - F's half-extents (qx, qy) = 0x396850: `Custom_Hard_XExtent` / `Custom_Hard_YExtent` × `Scale_Factor`, each
     falling back to |model bbox max x / y| × `Scale_Factor` (0x371be0); deployed objects use the deployed variant
     0x371d20 [M].
   - qdir = (to − from).xy / sqrtf(dx² + dy²), divided in double; with length 0 it stays (0, 0).
   - Time window t0 = t1 = float(global frame 0x140b0a320).
   - Ignore ids +0x28 / +0x2c = 0x3fffff (none) and no ignore list: **F itself is not excluded**.
3. **Slice.** With t0 = t1 exactly one tree is searched: the ring slice holding the current frame (index
   min(⌊(t − ring base) / interval⌋, count − 1) from the ring head; nothing when t < base). Land ring:
   `LandObjectTrackingTreeCount` 25 × `LandObjectTrackingInterval` 20 frames. A dirty layer is rebuilt first
   (0x5d8140).
4. **Candidates:** entries whose AABB overlaps (strict <) the AABB of {from, to} grown by max(qx, qy) on every side.
   The entry AABB is built by 0x6b3b00 [M: not traced].
5. **Hit test** per candidate (0x6b24f0), a 2-D test in double (heights are not compared):
   - entry: the object's tracked position S at the slice start and E at the slice end (0x538830), facing f at the
     slice end (0x538510), half-extents (ex, ey) (0x6b3b00 via 0x3969b0 [M]);
   - A = S + (E − S)·a, a = clamp((t − slice start) / (slice end − slice start), 0, 1);
   - u0 = normalise(S − from), u1 = normalise(E − to) (sqrtf; a zero vector stays zero);
   - R(u) = lerp(ey, ex, |u·f|²) + lerp(qy, qx, |u·qdir|²), lerp(a, b, s) = (1 − s)·a + s·b. R0 = R(u0),
     ΔR = R(u1) − R0;
   - d = from − A, D = to − from; qa = |D|² − ΔR², qb = 2(d·D) − 2·R0·ΔR, qc = |d|² − R0²;
   - qa = 0 and qb = 0: hit iff qc ≤ 0. qa = 0: r = −qc / qb; miss iff (qb < 0 and r > 1) or (qb > 0 and r < 0).
     Otherwise disc = qb² − 4·qa·qc. disc ≤ 0: hit iff qa < 0. disc > 0: r1 = (√disc − qb) / (2qa),
     r2 = (−qb − √disc) / (2qa); qa > 0: miss iff r1 < 0 or r2 > 1; qa < 0: miss iff r1 ≤ 0 and r2 ≥ 1.
   - A hit appends the entry's object id to the result list (cleared when the search starts).
6. **Per hit**, in list order:
   - O = the object by id; skip it if gone or typeless;
   - m = the type's model for the current mode (0x3729f0; land model type +0x2a0); skip if none;
   - (lo, hi) = m's bounding box (vf +0x58: min xyz, max xyz, model space);
   - v = (lo.z + hi.z) × `Scale_Factor` (type +0x658) + O.z (obj +0x80);
   - if v ≥ M: M = v.

   The term is lo.z + **hi.z** (a sum, not hi − lo): for a box starting at z = 0 it is the scaled top; a box centred
   on 0 gives O.z. There is no `Hover_Offset`, no fade and no margin. The reduction is a max, so the hit order (tree
   order) does not matter.
7. Return M. There is no terrain, water or floor term here: those come only from steps 3 and 8.

**Layer 4 membership on land** [H for the rules; M for the unit list]:
- **Records.** The tracking system keeps one record per object. A record is made when the object's locomotor path
  changes (0x559260, from the path helpers 0x554310 / 0x554480 / 0x557b10) or from the flying vf10 0x63d470. It is
  made only when the object's layer has a bit other than Land (bits & ~0x10 ≠ 0) and its object manager is the
  live one (manager +0x63e).
- **Record contents:** one of
  - the planned position per frame (the path played through locomotor vf +0x1d8, ≤ 9000 frames; 0x53a900);
  - a locomotor track (vf +0x1e0; 0x53ae20);
  - the current position only (0x53acf0).

  Its layer bits come from 0x397900. The object is in layer 4 when (bits & 4) == 4 (0x537e60).
- **Layer 0x397900(obj, moving):**
  - 4 when `Uses_Multiple_Locomotors` (+0x74) is set and the active locomotor answers vf +0x240 with kind
    vf +0x238 = 2;
  - else `Space_Layer` (+0x16dc) when moving (vf +0x1a8 0x42cc60: not vf +0x210, and pack speed +0xec > 0 or a
    path), when the caller passes moving = 1 (the path record 0x53a900 does), or when `Stationary_Space_Layer`
    (+0x16e0) is 0;
  - else `Stationary_Space_Layer`.

  Kind 2: Flying and LandBomber always. JetPack: kind 2 with no pack or with pack state +0x48 ∈ {0, 0x0f…0x13},
  else kind 1. Walk: kind 1.
- **FoC land members:**
  - the airborne jetpack units with `Uses_Multiple_Locomotors` (Dark_Trooper_PhaseII/III, Boba_Fett, Bossk, IG-88,
    Silri, Tyber_Zann, Urai_Fen) [M];
  - objects whose `Space_Layer` contains Corvette (the TransportUnit landing craft and shuttles), if they move with
    a locomotor on a land map [L].

  The flyers are not members: Snowspeeder and Y-Wing_Bombing are Frigate, Gallofree_Ground_Fly is Capital. Land
  props with Corvette (rock forms, wrecks) have no locomotor and get no record [M]. In vanilla, the clearance in
  effect lifts flyers over airborne jetpack troops [M].
- **Self-hit:** F is not excluded. That is harmless only because no vanilla flyer is in layer 4. A modded flyer
  whose own layer includes Corvette would raise M to its own top every frame and climb, as the original does [M].

**Frequency and determinism.**
- It runs once per altitude step: every frame in Moving 0x13 and loiter 0x27, never while circling. The locomotor
  keeps no cache.
- The layer is rebuilt lazily: when any record with that bit changes (dirty flag via 0x539040), and at the next
  query.
- The inputs are the tracked positions, the model box and `Scale_Factor`. The result is a max, so it does not depend
  on the tree layout. The hit test runs in double; evaluate it in double to match borderline hits [M].

### 6.4 Flying per-frame driver (vf6 0x63d7f0) [H]
1. Land only. The state must be in {0, 0x13, 0x16, 0x26, 0x27}; otherwise return an error.
2. **0 → 0x13.**
3. **0x13 Moving:**
   - no current path → 0x27;
   - else set path +0x6d = 1;
   - (P₁, O₁, s₁) = follower(path, frame, P, O, pack +0xec) (§6.1);
   - altitude(frame, &P₁, &O₁, s₁, useTrack = 1) (§6.3);
   - set the position P₁ and the orientation O₁; pack +0xec = s₁; if P₁ ≠ P, V = P₁ − P;
   - if the path is complete, pop it (0x557b10); if no path is left → 0x27.
4. **0x16:** nothing.
5. **0x26:** circling (§6.5).
6. **0x27:** loiter (§6.6).

### 6.5 Circling 0x26 (0x63db30) and its caller [H; M for the radius fields]
**Entry** (vf +0x278 = 0x63bf20, Start_Circling(target object, r_min, r_max)):
- requires a target and both radii > 0;
- pack +0x278 = the target **object**; state 0x26; +0x280 = r_min; +0x284 = r_max;
- subscribes to the target's signals.
- **The only caller** (full-text scan of `callq *0x278`) is `CableAttackAbilityClass` 0x6f34d0, reached from its
  vf4 0x6f2d50, i.e. the snowspeeder tow-cable start:
  - it starts recharge 0x1b and creates the cable render object (width +0x198, mode +0x1c0);
  - then it calls Start_Circling(cable target, record +0x178, record +0x17c).
  - Those two floats sit where the shared activation-range base keeps `Activation_Min_Range` /
    `Activation_Max_Range` (the Tractor_Beam field table puts them at +0x178 / +0x17c). FoC `Cable` ability:
    10 / 100 [M].
- How 0x26 is left was not traced. vf +0x270 (0x63bfc0) is a separate clearance test over a static grid around a
  point (obstacle height × 2 vs `Hover_Offset`) [M].

**Per frame**:
1. C = the target object. If null, P += V and stop.
2. u = (C − P).xy. r = sqrtf(u.y² + u.x²). If r > 0, û = u × (1/r).
3. **Height target** (dis for the probes):
   - H = terrain(P + V); H = max(H, C.z);
   - for k = 2..11, H = max(H, terrain(P + k·V)) (x and y both advance);
   - H += Hover_Offset.
4. **Vertical step:**
   - vmax = maxspeed;
   - dz = clamp(H − P.z, −vmax, vmax);
   - if dz < 0: span = max(`Prepare_Strafe_Height` − Hover_Offset, 1) and dz = (−(H − P.z) / span) × dz. There is
     **no second clamp**: the port's "(then clamped again)" is wrong.
5. v̂ = V.xy normalised (when > 0).
6. **Heading:**
   - r < r_min: dir = (−û.x, −û.y, dz) (away);
   - r > r_max: dir = (û.x, û.y, dz) (toward);
   - otherwise tangent: if v̂.x·û.y − v̂.y·û.x ≤ 0, dir = (−û.y, û.x, dz); else dir = (û.y, −û.x, dz).
   - Desired orientation = angles(0 → dir).
7. **Attitude:** attitude(O, desired, &turn(T), &lift(T), gate = 0) (§6.2).
8. V = ((cy·cp)·vmax, (sy·cp)·vmax, sp·vmax) from the new pitch and yaw, as §3.4. Q = P + V.
9. **Up to 3 terrain-clear retries:**
   - g = terrain(Q) if Q is inside the 3-D map box, else 0; g += Hover_Offset;
   - if g ≤ Q.z, stop;
   - else V = normalise(Q.x − P.x, Q.y − P.y, g − P.z) (sqrtf, 1/len) × vmax, and Q = P + V.
10. The orientation is set to (attitude roll, angles(P → Q).pitch, angles(P → Q).yaw). The position is set to Q;
    pack V = V.

### 6.6 No-path loiter 0x27 (0x63e2b0) [H]
1. **Move and level.**
   - P' = (s·cos Y + P.x, s·sin Y + P.y, P.z + 0), with s = pack +0xec and the yaw from before this frame's
     attitude change.
   - attitude(O copy +0x90, desired (0, O.pitch, O.yaw), &turn, &lift, gate = 1): unwinds the bank, keeps the
     heading.
   - altitude(frame, &P', &O, s, useTrack = 0).
   - Set the orientation, then the position P'. If P' ≠ P, V = P' − P.
2. **Formation.** If the pack has none, find or create one through the movement coordinator (mode vf +0x228) and
   order it to move to P.
3. **Re-issue** (formation not busy: 0x41cf40 / 0x41cf30 false):
   - if formation +0x240 is set, raise signal 10 on the unit (0x220ed0) [M];
   - the condition is formation +0x1dc + 2·FPS + 1 < now (strict);
   - **No attack target:**
     - L = 1.1 (0x140a4432c) × `XYExpansionDistanceLand` (GC +0x147c);
     - D = the formation destination (0x583150);
     - if sqrtf(dx² + dy²) of (P − D) ≥ 0.3·L:
       - n = normalise(D − P with z = 0) (0x139800);
       - a = one synced float(−90, +90) (0x1ffbb0; **both bounds read, (dis)**) × k;
       - dir = (n.x·cos a − n.y·sin a, n.x·sin a + n.y·cos a, n.z (= 0)).
     - Otherwise dir = −(cos Y, sin Y) (from obj +0x8c), z −0.0, and set formation +0x240 = 1.
     - The new destination is **D + dir·L** (z: D.z + dir.z·L). Issue it (0x41fd30, kind 4) and set the formation
       speed to minspeed (0x4205a0).
   - **With an attack target** tgt:
     - aim = tgt position (0x398440);
     - S = the unit's representative type `Strafe_Distance`;
     - only if the 2-D distance (unit, aim) < radius(tgt) + 0.3·S: aim.xy += (cos Y, sin Y)·S,
       aim.z += S·0 + `Prepare_Strafe_Height`, and set formation +0x240 = 1;
     - issue the move (kind 4).
4. **RNG:** exactly one synced draw per no-target re-issue that is ≥ 0.3·L away. Re-issues are more than 61 frames
   apart.

---

## 7. Infantry-team slot geometry (LandTeamInfantry 0x62d620) [H; M for 0x509810]
Called from the member service 0x62ca10 (MOVEMENT §3.7). Let K be the container. The slot comes from the
container's locomotor.

1. θ = K's yaw (container locomotor vf +0x1f8 → the third float). (c, s) = normalise(cos θ·k, sin θ·k) via sqrtf
   and 1/len. The function evaluates this twice with slightly different code; the values are equal.
2. d = member P − K.P.
3. **Errors in the team frame:**
   - along = (slot.x (pack +0x114) + jig.x (+0x12c)) − ((c·d.x + s·d.y) + 0·d.z);
   - side = (slot.y (+0x118) + jig.y (+0x130)) − ((−s·d.x + c·d.y) + d.z·0).

   0x42c270 refreshes the jiggle (MOVEMENT §4) before it is read.
4. **Speed.** vK = K pack +0xec, vmax = the member's maxspeed.
   - |along| ≤ 1.0 (0x140a4430c): v = vK.
   - along > 1: t = clamp(along / 10, 0, 1) (0x140a44310) and v = (vmax − vK)·t + vK.
   - along < −1: t = clamp(−along / 10, 0, 1) and v = (0 − vK)·t·t + vK (quadratic slow-down).
   - If the member state is 0x10: v = 0.
5. Speed step (0x62d520): accelerate by vf +0x2d8 or decelerate by vf +0x2e0, clamped to [0, vmax], into pack
   +0xec.
6. **Heading.**
   - w = clamp(|side| / 10, 0, 0.5) (0x140a44314 / 0x140a44318);
   - perp = (−s, c) when side > 0, else (s, −c);
   - dir = (c + w·(perp.x − c), (perp.y − s)·w + s, 0).
   - Add the collision steering vector from the mode's +0x230 system (0x509810) [M].
   - Yaw target = angles(0 → dir).yaw.
7. **Turn:** 0x5ad5a0 turns yaw by at most the turn rate (× GC +0x150c `Fleeing_Infantry_Speed_Bonus` when fleeing,
   0x62c3d0) and sets the orientation to (0, 0, wrap360(yaw)).
8. **Move:** P' = (cos(yaw)·v + P.x, sin(yaw)·v + P.y, P.z + 0) through vf +0x2f8 (conform).
9. Constants (read from the image): 1.0, 10.0, 10.0, 0.5 at 0x140a4430c / 10 / 14 / 18. The recovery threshold
   0x140a4431c = **4.0** (MOVEMENT §8 item 5).

---

## 8. LocomotorCommon 0x5070b0 [M] (not needed by the fighter or flying paths)
- It is called once, from 0x42c680, and is land only. It is the **land walk path corner-speed pass**, not a turn
  core used by these locomotors.
- It walks the land path and computes, for each corner, the turn angle between the unit directions (acos of the
  clamped dot product, §1.3 normalisation). It merges short segments while the turn keeps its side and stays under
  π.
- The corner speed is derived from maxspeed, turn rate (rad/frame) and a radius term (100, min 20).
- It tests each candidate speed against the static map with collision probes (0x5a4c10), dividing by 1.5 until
  the probe passes or the speed falls below minspeed / 0.1 × maxspeed.
- Each segment's **speed fraction** (v / maxspeed) is written into the path. **A fraction below
  `Min_Speed_Fraction_For_Turn` (+0x3ac) is set to 0**: the walker stops and turns in place (the first-waypoint
  fraction test of MOVEMENT §3.5).
- A backward pass caps each fraction so the unit can brake: v_i = sqrtf(decel·2·|seg| + v_{i+1}²) / maxspeed,
  with decel = vf +0x2e0.
- The exact arithmetic and the use of `Stopped_Rate_Of_Turn` (+0x3b0) are **untraced**. Nothing in §2–§6 reads
  +0x3ac or +0x3b0.

---

## 9. Corrections to earlier documents and to the port
1. **Fighters steer reactively in every state** (§0.1). The port's Hermite playback for FighterMoving and its [L]
   `fighter_station_step` should be replaced by §3–§4.

> **Status 2026-10-30:** Done. The fighter locomotor in `engine/tactical/movement/include/alamo/tactical/movement/fighter.hpp` steers reactively in every state with no Hermite playback (header lines 5-7), and `fighter_station_step` no longer exists anywhere in `engine/`.
2. **REMAINING_GAPS §4.4 loiter:**
   - the re-issue target is the formation **destination** + dir × L, not unit + dir × L;
   - the random bound is confirmed: float(−90, +90);
   - the turn-back dir = −facing is confirmed;
   - the strafe offset (+facing × Strafe_Distance, z + Prepare_Strafe_Height) only applies within
     radius + 0.3 × Strafe_Distance of the target. Farther out it aims at the target itself.
3. **REMAINING_GAPS §4.4 circling:**
   - pack +0x278 is a target **object**, not a point;
   - the descent scale has no second clamp;
   - the terrain probes are at P + k·V for k = 1..11 (the port's spacing note "k × 6 frames" is wrong);
   - H is max'd with the target z before Hover_Offset is added.
4. **REMAINING_GAPS §4.7 / MOVEMENT §2.5:**
   - Idle 0x1C is the squadron orbit / slot hold of §4.2 (the "×60 look-ahead terms" are the leader's 60-unit
     orbit);
   - "Dead_Stop repositions above 1.33× the threshold, with a doubled step" means §4.6: regroup when the spread
     exceeds 1.33 × the formation extent, with a leader aim of 2 × Min_Speed.
5. **MOVEMENT §8 item 5:** global speed factor 0x140a28568 = 1.0; team slot threshold 0x140a4431c = 4.0; Hermite
   basis = §6.1. Still unread: 0x140b27a10 (hyperspace profile) and the land k-means / rank caps.

---

## 10. XML fields read here

| field | offset | used in | FoC sample |
|---|---|---|---|
| `Max_Speed` | +0x37c | maxspeed | X-wing 4.0, TIE 4.5, snowspeeder 5.0 |
| `Min_Speed` | +0x3b4 | minspeed, avoid_speed, Dead_Stop | 2.5 / 2.5 / 2.0 |
| `Max_Thrust` | +0x384 | speed budget (raw) | 1.0 |
| `Max_Rate_Of_Turn` | +0x38c | yaw budget, turn radius, avoidance horizon | 3.0 / 3.0 / 5.0 |
| `Max_Rate_Of_Roll` | +0x394 | roll rate | 5.0 / 5.0 / 4.5 |
| `Bank_Turn_Angle` | +0x39c | bank clamp (raw) | 40 / 40 / 20 |
| `Max_Lift` | +0x3a0 | pitch rate | 4 / 4 / 2.5 |
| `Strafe_Distance` | +0x2118 | strafe circle, loiter strafe | 500 |
| `Minimum_Follow_Distance` | +0x534 | chase speed match | 25 / 50 |
| `Targeting_Max_Attack_Distance` | +0x500 | dogfight lock range (0x3749d0) [M] | 450 / 500 / 300 |
| `Layer_Z_Adjust` | +0xdfc | station / cell z | unset in FoC fighters |
| `Squadron_Formation_Error_Tolerance` | +0x9b0 (squadron type) | station speed, Dead_Stop | 25 / 35 |
| `Out_Of_Combat_Defense_Adjustment` | +0xde0 | modifier event (combat) | — |
| `Hover_Offset` | +0x1b68 | flying altitude, circling, land clamp | snowspeeder 50 |
| `Prepare_Strafe_Height` | +0x1e24 | circling descent span, loiter strafe | 150 |
| `Scale_Factor` | +0x658 | clearance box top, query half-extents (§6.3.1) | snowspeeder 1.2, Dark_Trooper_PhaseII 1.7 |
| `Custom_Hard_XExtent` / `Custom_Hard_YExtent` | +0x16bc / +0x16c0 | clearance query half-extents (else model bbox) | unset on the snowspeeder |
| `Space_Layer` / `Stationary_Space_Layer` | +0x16dc / +0x16e0 | tracking layer (layer 4 = Corvette) | snowspeeder Frigate / Land |
| `Uses_Multiple_Locomotors` | +0x74 | airborne jetpack → layer 4 | Dark troopers, bounty-hunter heroes |
| GC `LandObjectTrackingTreeCount` / `LandObjectTrackingInterval` | +0x1470 / +0x146c | tracking ring on land | 25 / 20 |
| GC `SpaceIdleMovementSpeed` | +0x1554 | solo idle speed | 0.0 |
| GC `Engines_Disabled_Speed_Modifier` | +0x1530 | minspeed | 0.4 |
| GC `XYExpansionDistanceLand` | +0x147c | loiter L | — |
| GC `Fleeing_Infantry_Speed_Bonus` | +0x150c | §7 turn rate | 2 |

---

## 11. Still open
1. **Fighter / squadron:**
   - the writers of team list +0x88 (E) and +0x8c (S);
   - pack +0x15c (lateral offset) [L];
   - the 0x5c5060 corner shift length (0x54f700) [L];
   - who enters Dead_Stop 0x1F;
   - unit abilities 0x29 / 0x22 in the provocation test (by number only);
   - the exact ring enumeration of 0x4fb160 [M].
2. **Flying:**
   - how state 0x26 is left;
   - the clearance query 0x63b640: resolved (§6.3.1). Still open there: the entry builder 0x6b3b00 (entry AABB and
     half-extents) [M], and whether land transports ever hold a Corvette-layer record [L];
   - the formation destination kinds of 0x583680 [M];
   - the jetpack follower 0x641010 was not compared line by line.
3. **0x5070b0:** exact arithmetic; `Stopped_Rate_Of_Turn` readers.
4. **Hyperspace:** the profile table 0x140b27a10 (unchanged from MOVEMENT §8).
5. **Probe suggestions** (Gabriel launches; tools/probe_mod/):
   - log pack +0x1c4 / +0x1cc and +0x1e0 for two squadrons in a dogfight;
   - log one X-wing's RelYaw / RelPitch and the phase (pass / break-off) during an attack run on a frigate, to
     check §4.4.
