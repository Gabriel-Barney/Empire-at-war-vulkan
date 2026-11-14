# cu8k t03 / t08 / t09: space fleet placement, the lock-counter adopt, squadron grouping (RE, 2026-11-09)

StarWarsG.exe FoC x64 v1.121. Read from the decompiled fleet placement `0x4fbb10` and its helpers (`0x4fc3d0`,
`0x4fca30`, `0x4fb9c0`, `0x4fd5b0`, `0x4fdfb0`, `0x4fea10`, `0x4ff100`, `0x4fc7e0`, `0x4fc640`), the campaign spawn
`0x2a00b0` / `0x2a1eb0`, the locomotor vtables (read from the exe: Team `0x8aff88`, Fighter `0x8a6198`, SimpleSpace
`0x8aeaf8`, Starship `0x8ae250`), `0x62f800`, `0x630210`, `0x630390`, `0x6251d0`, `0x5cb020`, `0x624dc0`, `0x3a8e50`,
`0x39ad40`, the space composition `0x5d58f0` and its helpers; float constants and the register flow of `0x4fca30`,
`0x4fd5b0`, `0x4fdfb0`, `0x4fea10` checked in the disassembly. Trace: `tools/probe_mod/cu8h_probes/out/
20261109-062335-kashyyyk/` (CU8K_T04_COORDINATOR_TRACES). Tags: [H] read in code, [M] inferred, [L] guess.

Notation: ψ = the anchor's yaw in degrees (anchor +0x8c); A = the anchor's position (+0x78, +0x7c); Rot(θ)·(x, y) =
(cos θ·x − sin θ·y, sin θ·x + cos θ·y); angles are turned into radians as θ·(2·π / 360) in float. All arithmetic is
float.

## 1. t03: fleet placement `0x4fbb10`

### 1.1 Callers, anchors, registration [H]
- Only caller: the campaign spawn `0x2a00b0`, at its end, **space mode only** (mode vf +0xe0 = 2). `0x2a1eb0` calls
  it once for the defender (anchors = the map's `DEFENDING FORCES POSITION` markers, arrival flag 0) and once for the
  attacker (anchors = the `ATTACKER ENTRY POSITION` markers, arrival flag = 1 in space).
- The placement object is a global (0x140a2bd00): an anchor list (the caller's marker list, copied by `0x4fc640`)
  and a registration list of (object, group) pairs. Groups: the spawn walks the battle's galactic units; each one that
  spawned something uses group index k and then k := k + 1, wrapping to 0 at the marker count. So group k's anchor is
  marker k mod (marker count), in the caller's marker order [M: the order the caller collects the markers].
- Registration `0x4fc7e0(obj, group)`: a team member is replaced by its container (+0x2b0); an object already
  registered is skipped; otherwise (object, group) is appended. The spawn registers, in creation order:
  - each tactical unit it creates whose own `Ship_Class` (type +0x650) is 0..6 (an unsigned compare, so the default −1
    is out) and, for the unit made from the galactic object itself, whose type is not in the `Super` category
    (type +0x1648, `0x375270`) [M: what that category covers];
  - after the team helper `0x2a5a60`: the **first** unit of the group's unit list, i.e. the container of the first
    chunk, when that unit's `Ship_Class` < 7. Containers of later chunks (more than `Max_Squad_Size` units) are not
    registered and not placed [H for the single call, M for the effect].

### 1.2 Per anchor: classes and lists [H]
For each anchor a in list order (all registered objects of group a, in registration order):
1. Clear six lists L0..L5. Skip team members (`0x39b1a0`). Class = the object's type's `Ship_Class`; for a **team
   container** (+0x348 set) the `Ship_Class` of its **first member** (`0x4ec820`, the head of the team's member
   list).
2. Append by class (XML names from OBJECT_TYPES `Ship_Class`):

| `Ship_Class` | XML names | list |
|---|---|---|
| 5 | `capital` (also `walker`) | L0 |
| 3, 4 | `corvette`, `frigate` (also `tank`) | L1 |
| (none) | | L2 is never filled |
| 0, 1 | `fighter`, `bomber` (also `infantry`) | L3 |
| 2 | `transport` (also `artillery`) | L4 |
| 6 | `super` | L5 |
| other (−1, ≥ 7) | | not placed |

3. L3 interleave `0x4ff100`, only when L3 has both F fighters (class 0) and B bombers (class 1); each kind keeps its
   order:
   - B > F: h = (B − F) div 2; bombers 0..h−1; then for i = 0..F−1: bomber h + i, fighter i; then bombers h + F..B−1.
   - B ≤ F: h = B div 2; for i = 0..h−1: bomber i, fighter i; then fighters h..h + F − B − 1; then for i = 0..B−h−1:
     bomber h + i, fighter h + F − B + i.
4. Anchor object null → nothing of this group is placed. Otherwise, in this order:
   1. **L5** with `Set_Capital_Ship_Formation` (`0x4fca30`) and the size S = (0, 0, 0): every super ends exactly at A.
   2. Core: i = the first non-empty list among L0, L1, L2, L3. S = Size(Li) (§1.3); `Set_Capital_Ship_Formation`(Li,
      S) (§1.4), which returns W; r = √(W² + W²).
   3. If i < 3: r := `Set_Screen_Formation`(L(i+1), r) (§1.5); then for k = i+2..3: r := `Set_Lead_Formation`(Lk, r)
      (§1.6). An empty list places nothing and returns r unchanged.
   4. `Set_Transport_Ship_Formation`(L4, W) (§1.7).
   5. If L0..L3 are all empty: S = Size(L4) and L4 goes through `Set_Capital_Ship_Formation` instead (no transport
      function).

So, by class (functions named by their log strings; the RVA ↔ name pairing is [H]):

| Fleet contains | capital | corvette / frigate | fighter / bomber | transport | super |
|---|---|---|---|---|---|
| a capital | Capital grid `0x4fca30` | Screen `0x4fdfb0` | Lead `0x4fd5b0` | Transport `0x4fea10` | grid at A |
| no capital, a corvette / frigate | | Capital grid | Lead (Screen gets empty L2) | Transport | grid at A |
| only fighters / bombers | | | Capital grid | Transport | grid at A |
| only transports | | | | Capital grid | grid at A |

The Kashyyyk trace matches: Calamari_Cruiser (capital) → 0x4fd0a3, Nebulon_B_Frigate → 0x4fe59d, the two
Rebel_X-Wing_Squadron containers → 0x4fdb3d twice (call 0x4fc285, the Lead loop).

### 1.3 Size(L) `0x4fc3d0` [H]
p := 0, q := 0. Per object of L:
- not a team container: from its cull box (object +0x28 vf +8: centre, then half extents hx at +0xc, hy at +0x10;
  the world-axis box [M]): a = hx, b = hy;
- team container with N members: T = the company type (`0x396380`, team +0x58); for member j = 0..N−1: o = (ox, oy)
  = `Squadron_Offsets`[j] of T ((0, 0) when j is out of range), (hx, hy) = the member's cull box half extents;
  xmin = min(0, …, ox − hy), xmax = max(0, …, ox + hy), ymin = min(0, …, oy − hx), ymax = max(0, …, oy + hx) over the
  members; a = (xmax − xmin) / 2, b = (ymax − ymin) / 2 (both ranges start at 0, so they contain the origin);
- q := a if a > q; p := b if b > p.

Result S = (Sx, Sy, Sz) = (2p, 2q, 0): Sx is the lateral spacing, Sy the row depth.

### 1.4 `Set_Capital_Ship_Formation` `0x4fca30`(L, n = |L|, S, A) [H]
1. n = 0 → return. Rows R: n < 3 → 1; < 7 → 2; < 10 → 3; < 17 → 4; ≤ 25 → 5; else 6.
2. Row counts: every row n div R; the remainder e = n − R·(n div R) goes to rows (0-based) by `0x4fb9c0`:

| R | e = 1 | e = 2 | e = 3 | e = 4 | e = 5 |
|---|---|---|---|---|---|
| 2 | 0 | | | | |
| 3 | 1 | 0, 2 | | | |
| 4 | 0 | 0, 2 | 0, 1, 2 | | |
| 5 | 0 | 1, 3 | 0, 2, 4 | 0, 1, 2, 3 | |
| 6 | 0 | 2, 3 | 1, 3, 5 | 1, 2, 3, 4 | 0, 1, 2, 3, 4 |

3. m = the largest row count. y := R·Sy·0.5 − Sy·0.5; prev := 0; stagger := false. Rows in order, ships taken from L
   in order:
   - c = the row's count; x0 = Sx·0.5 − (c·Sx + (c − 1)·0) / 2;
   - if prev ≠ 0: if prev = c: stagger := not stagger, and when it became true x0 += Sx; else if stagger: x0 −= Sx,
     stagger := false. Then prev := c;
   - ship k (0..c−1) of the row: local (x0 + k·Sx, y); θ = ψ + 270 (minus 360 once when > 360); position = A +
     Rot(θ)·local, z = the ship's own z (unchanged);
   - after the row: y −= Sy.
   Local +y is the anchor's facing (θ = ψ − 90° maps local +y onto the yaw direction), local +x its right.
4. Returns W through S: S.x = S.y = W = (m − 1)·0.5·Sx, or 0.5·Sx when m = 1.

### 1.5 `Set_Screen_Formation` `0x4fdfb0`(L, n, r, A) → r' [H; register flow read in the disassembly]
1. n = 0 → return r. s = |Size(L)|. R0 = r + s/2.
2. Ring counts: R := R0; repeat: c = min(trunc(max(3s, 1.25·π·R) / (s/2)), remaining); remaining −= c; R += s/2;
   until remaining = 0. r' = the final R = R0 + J·s/2 (J rings).
3. Placement: ring j uses radius ρj = R0 + j·**s** (a full s per ring here, unlike the count's s/2). Angles: c = 1 →
   a = 90; else a starts at 337.5 and grows by 225 / (c − 1) after each ship (repeated float addition). Ship: θ = ψ
   + a + 270, wrapped into [0, 360] (repeat ±360); position = A + ρj·(cos θ, sin θ), own z. So a ring spans
   −112.5°..+112.5° around the facing (a = 90 is straight ahead), starting on the left [M: handedness].

### 1.6 `Set_Lead_Formation` `0x4fd5b0`(L, n, r, A) → r' [H; disassembly]
1. n = 0 → return r. s = |Size(L)|. R0 = r + s/2.
2. Ring counts: R := R0; first ring c = min(trunc(max(2s, 2R) / s) + 1, remaining); later rings c = min(trunc(2R / s)
   + 1, remaining); R += s after each ring. r' = R0 + J·s.
3. Placement: ring j radius R0 + j·s. Angles: c = 1 → a = 90; else a starts at 25 and grows by 130 / (c − 1)
   (repeated addition). θ = ψ + a + 270 wrapped into [0, 360]; position = A + ρ·(cos θ, sin θ), own z. A ring spans
   −65°..+65° around the facing. Two squadrons on one ring sit at ±65°, 2·R0·sin 65° ≈ 1.81·R0 apart.

### 1.7 `Set_Transport_Ship_Formation` `0x4fea10`(L, n, W, A) [H; disassembly]
1. n = 0 → return. S = Size(L). Row width w = trunc(√n + 0.99), at least 2; rows of min(w, remaining).
2. y := −W − 200 (W = the core grid's S.y). Per row of count c: x0 = Sx/2 − (c·Sx + (c − 1)·20) / 2; ship k: local
   (x0 + k·(Sx + 20), y); θ = ψ + 270 (minus 360 once when > 360); position = A + Rot(θ)·local, own z. After the row
   y −= Sy + 20. So the transports stand in rows behind the core, 200 back. No team-member pass.

### 1.8 Per placed object (all four functions) [H]
In the order the function places them:
1. orientation := the anchor's (anchor +0x84..+0x8c, `0x3a8710`); teleport to (X, Y, own z) (`0x3a8f90`, forced);
   locomotor pack +8..+0x10 := that position, pack +0x38..+0x40 := the object's new orientation;
2. leave its coordinator (`0x426d50`; the old one is deleted empty at the next update) and **adopt `0x39bcb0`: one
   degenerate `int(1, FPS)` ctor draw** (returns 0x4fd0a3 / 0x4fe59d / 0x4fdb3d / 0x4fefc5);
3. team container only: team bookkeeping (the global helper at 0x140a170c0: `0x4fafd0`, `0x4fa6e0`; team +0xa4 and
   +0xb0 := the position; `0x4061a0`, `0x406510`), then every member: orientation := the anchor's; position =
   container + Rot(ψ)·`Squadron_Offsets`[j] (rotation by ψ itself, single −360 wrap; member's own z); member pack +8
   := the **container's** position, pack +0x38 := the container's orientation. Offset type: Capital grid uses team
   +0x50 (`Create_Team_Type`, else the company type), switching to +0x58 (the company type) when +0x50's count at
   type +0x980 (the `Squadron_Units` vector's count [M]) is below the team's member count and +0x58's is not; Lead
   uses +0x58; Screen uses +0x50.

No synced draw other than the adopts: in the trace every hit under 0x4fbb10 is a ctor draw (n 191-196) [H trace].

### 1.9 (c) The lock-counter adopt at the end [H]
Gate: `In_Game_Cinematics` (GC 0x140b16dd2) and the call's arrival flag (attacker in space) and 0x140b27d5e (the story
link's arrival option, BATTLE_TRANSITIONS §2.9 P9). All three held in the Kashyyyk run. Under the same gate the
start of 0x4fbb10 resets the arrival min / max globals (0x140b27bbc := FLT_MAX, 0x140b27bc0 := 0).

After **all** anchor groups are placed: for every registration entry, in registration order (all groups, placed or
not), the object's LOCOMOTOR vf +0x290 (vf82) with argument 0:
- **`TeamLocomotor` `0x62f800` (any `Team` container: fighter / bomber squadrons):** leave the coordinator; lock
  counter +1 (`0x3a8e50(container, 1)`): the count was 0, so it splits the container from any formation and **adopts:
  one degenerate ctor draw** (stack 0x3a8e9a ← 0x62f856 ← 0x4fc34b); count := 1. Then suspend the container's
  locomotor (vf +0x200) and call every member's locomotor vf82(0) (Fighter `0x5cb020`: suspend, state 5 = hyperspace
  arrival; no lock change, no draw).
- **SimpleSpace `0x6251d0`(0) (capitals, frigates, …):** suspend, leave the coordinator, state 5. No draw.
- Starship / Fleet / plain `LocomotorBehaviorClass`: vf82 is empty.

So the extra draws are **one per registered team container, after every placement draw of every group**, in
registration order; ships get none. Kashyyyk: 2 (the X-wing squadrons). Without the gate (defenders, or the cinematic
flags off) nothing calls vf82: no lock draws, no hyperspace-in; the units keep their placement coordinators.

What follows [H code, M timing]: state 5 starts the hyperspace arrival (L1_S4_REMAINING §3.4: state 5 → 4, lock +1,
one `int(1, 4)` on state 4's first frame, finish after about 150 frames). Ship finish `0x624dc0`: release the lock,
resume, signal 0x19, leave the coordinator, **new coordinator + Move_To its own position** (one space-composition draw
at the next coordinator update). Team container: `TeamLocomotor` vf6 `0x630210` → `0x630390` once no member's
locomotor is suspended (pack +0x260): lock −1 (to 0), resume, signal 0x19, leave, **new coordinator + vf3 + Move_To
its own position** (one space-composition draw at the next update). The four single space compositions at mf 128,
140, 140, 147 of the Kashyyyk load trace are these four arrivals [M].

### 1.10 (d) "Locked" `0x39ad40` and multi-locomotor units [H unless tagged]
`0x39ad40(obj)` is true when any of:
1. the movement-lock count obj +0x3a8 > 0 (`0x3a8e50(obj, 1)` adds, `(obj, 0)` subtracts with a floor of 0; the first
   +1 also splits and adopts, §1.9);
2. its LOCOMOTOR vf +0x1d0 (`0x42cbf0`, all locomotors): the current path record (top of pack +0x138 stack,
   `0x5569b0`) has byte +0x5c set (MOVEMENT §3.2 "skip-passability"; set by land push / avoidance paths [M]);
3. its parent is a team container whose locomotor has vf +0x230 true (`LandTeamContainer`) and whose current path
   record has +0x5c set (land only).

Lock +1 callers: the hyperspace arrivals (state 5 `0x627590`, cinematic `0x5c9a80`, `TeamLocomotor` vf82 `0x62f800`,
SimpleSpace / Fighter vf82 with argument 1), and ability code (`0x57cd40`, `0x58f6a0`, `0x5cd010`, `0x5ea910`,
`0x6364e0`, `0x644330`, `0x6abde0`, `0x6cc640`, `0x6f3800`, `0x700860`, `0x630950`) [H list, M which abilities].
Releases: arrival finish (`0x624dc0`, `0x5c9d40`, `0x630390`), `Cancel_Hyperspace` `0x5c9800`, and the matching ability
ends. In a space battle start, a squadron container is locked from the end of the fleet placement until every one of
its fighters has finished arriving; a ship from state 5 until its own finish.

In the composition `0x5d58f0`, a locked **seed** still gets its own formation (one draw) but recruits nobody; a
locked **candidate** can still be recruited by an unlocked seed (no lock test on candidates).

Role 4 (`Get_Space_Layer` `0x397900`): type `Uses_Multiple_Locomotors` (+0x74) and LOCOMOTOR vf +0x240 true and vf
+0x238 = 2. vf +0x240 is true only for Starship (whose kind is −1) and JetPack (kind by state). In vanilla FoC / EaW
XML `Uses_Multiple_Locomotors` = Yes only on land units: IG-88, Boba_Fett, Dark_Trooper_PhaseII / PhaseIII, Silri (and
`_No_Abilities`), Tyber_Zann (and `_Prisoner`), Bossk, Urai_Fen; no space unit. So no vanilla space unit gets role 4
[H for the XML and the slots, M that a jetpack unit never reaches a space composition]. With move type ≠ 4 role 4 is
appended to the corvette list and gets one formation per ship (MOVEMENT §4.6.4).

## 2. t08: MOVEMENT.md updates
Done in MOVEMENT §4.7.3 (campaign spawn bullet), §4.7.4 (jetpack confirmed) and §4.7.6 (run paragraph, items 1 and
4). Not touched: §4.7.2's 0x395421 row, the §4.7.3 skirmish-starting-forces bullet and §4.7.6 item 2 (another agent).

## 3. t09: why two X-wing squadrons made two role-0 groups

The composition (`0x5d58f0`, re-read for this task) recruits a remaining candidate C into a role-0 seed's formation
only when **all** hold [H]:
1. the seed is not locked (`0x39ad40`, §1.10);
2. C has a type (`0x3973b0`) and a LOCOMOTOR whose vf +0x238 equals the seed's;
3. with the seed box (centre cs, half extents es) and C's box (cc, ec) from `0x3982b0` (team container: the team's box,
   team +0x8c [M: the members' bounding box]; other objects: the box of obj +0x2a0 (`0x264ab0`) [M: the model's world
   box]; obj +0x2a0 null: a 0.1-unit box at the position), and D = |seed's goal (`0x583150`) − seed position|: |ccx − csx| ≤ max(2·esx, 0.33·D) + ecx and |ccy −
   csy| ≤ max(2·esy, 0.33·D) + ecy (z: max(2·esz, 1e18), so always true);
4. C's role (`0x397900(C, 0)`) is 0;
5. maxSpeed(C) ≠ 0, minSpeed(C) / maxSpeed(seed) ≤ 0.9 and minSpeed(seed) / maxSpeed(C) ≤ 0.9 (`0x370f00` /
   `0x3728f0` on `0x3973b0`'s type; for a squadron Max_Speed = the members' minimum, Min_Speed = the members' maximum
   scaled Min_Speed, OBJECT_TYPES §6.2).

Against the Kashyyyk move (mf 998):
- **Lock (1): not it.** The X-wing containers were locked by the placement (count 1, §1.9) but `0x630390` released
  them when their fighters finished arriving, about 150 frames into the battle (the compositions at mf 128-147); the
  order came at mf 998. Rule 3 of §1.10 is land only; rule 2 needs a path record with +0x5c, which no space path sets
  as far as read [M].
- **Locomotor (2), role (4), speed (5): identical for both squadrons** (same type `Rebel_X-Wing_Squadron`, `Team`
  locomotor kind 2, role 0; X-Wing Max_Speed 4.0 / Min_Speed 2.5 → 0.625 ≤ 0.9 with the same multipliers on both), so
  they cannot split two equal squadrons. The same holds for two TIE squadrons (4.5 / 2.5).
- **The box (3) is the deciding check [M].** The fleet placement put the two squadrons on the Lead ring at ±65° around
  the entry marker's facing, radius R0 = r + s/2 with r = the frigate screen's outer radius (core W·√2 of the
  Calamari_Cruiser plus the Nebulon-B ring), so they started about 1.81·R0 apart, mostly sideways, and they held
  there after the arrival (Move_To their own positions). A squadron box's half extent is about 30 to 35 units
  (offsets ±30, ±15 plus the fighter boxes), so 2·es ≈ 70; the move merges them only if 0.33·D + 35 reaches their
  per-axis separation, i.e. for D of roughly three times that separation or more. A move "into empty space" a few
  hundred to a couple of thousand units away does not reach that for a separation of several hundred units [L for the
  sizes: no positions or model boxes were recorded]. At Kuat the two TIE squadrons were skirmish starting forces,
  spawned next to each other by the company placer (no Lead ring), so they passed the same test and merged.
- Seen from the trace: 4 draws = 2 ships + 2 role-0 groups, so the seed squadron recruited nobody.

To settle it, log at the order (memory read, no debugger): both containers' positions and team boxes and the move goal;
the rule predicts a merge exactly when both per-axis inequalities of item 3 hold for the seed (the one nearer the goal).

## 4. What the port needs
1. **Space campaign battle start (attacker and defender):** after spawning a side, place its fleet per entry marker
   (attacker: `ATTACKER ENTRY POSITION` markers; defender: `DEFENDING FORCES POSITION`), galactic group k on marker k
   mod (marker count). Register every spawned unit with `Ship_Class` 0..6 and, for a team spawn, only the first
   chunk's container (class = its first member's `Ship_Class`).
2. Per marker: supers at the marker; the first non-empty of {capitals}, {corvettes + frigates}, {}, {fighters +
   bombers (interleaved, §1.2 step 3)} as the grid core (§1.4) with size §1.3; the next list as the screen arc
   (§1.5), the rest as lead arcs (§1.6), transports in rows behind (§1.7); all only transports → transports as the
   grid. Keep each unit's z; give it the marker's orientation; place squadron members at container + offsets rotated
   by the marker yaw.
3. Each placed unit: leave its coordinator and adopt (one `int(1, FPS)`), in placement order (supers, core, screen,
   leads, transports; marker by marker).
4. Then, only for the attacker with the arrival cinematic on (`In_Game_Cinematics` and the link's arrival option):
   for every registered unit in registration order start the hyperspace arrival; **each team container first takes
   the movement lock and adopts again: one more `int(1, FPS)` per container**; ships draw nothing here.
5. Arrival end: a ship, and a squadron container once all its fighters have arrived, releases the lock and gets a new
   coordinator holding its position: one space-composition draw at the next coordinator update each.
6. Space composition: implement the seed lock (lock count > 0 → the seed recruits nobody, still one formation) and
   keep candidates unchecked for locks. Use the box test of §3 item 3 with the team box for squadrons and the model
   box otherwise; z never limits.
7. Role 4 (multi-locomotor) never occurs in vanilla space; it can stay unmodelled.
8. Goldens: the placement moves units (positions) and adds draws (one per placed unit, one per attacking squadron
   container, one composition per arrival); space campaign goldens move.

## 5. RVA index

| RVA | What |
|---|---|
| 0x2a00b0 | campaign spawn (registration; calls 0x4fbb10 at the end, space only) |
| 0x2a1eb0 | battle spawn: defender / attacker marker lists, the two 0x2a00b0 calls |
| 0x4fbb10 | fleet placement (classes, per-anchor order, the vf82 pass; returns 0x4fc1d7, 0x4fc25c, 0x4fc285, 0x4fc34b) |
| 0x4fc640 | copy the marker list into the placement object |
| 0x4fc7e0 | register (object, group); team member → container |
| 0x4ff100 | fighter / bomber interleave of L3 |
| 0x4fc3d0 | list size S |
| 0x4fca30 | `Set_Capital_Ship_Formation` (grid; adopt return 0x4fd0a3) |
| 0x4fb9c0 | grid remainder distribution |
| 0x4fdfb0 | `Set_Screen_Formation` (adopt return 0x4fe59d) |
| 0x4fd5b0 | `Set_Lead_Formation` (adopt return 0x4fdb3d) |
| 0x4fea10 | `Set_Transport_Ship_Formation` (adopt return 0x4fefc5) |
| 0x3742c0 | `Squadron_Offsets`[i] (type +0x998 vector) |
| 0x62f800 | `TeamLocomotor` vf82 (lock +1 + adopt, return 0x62f856; members' vf82) |
| 0x630210 / 0x630390 | `TeamLocomotor` vf6 / arrival done (lock −1, new coordinator) |
| 0x6251d0 / 0x5cb020 | SimpleSpace / Fighter vf82 (state 5; lock only with argument 1) |
| 0x624dc0 | SimpleSpace arrival finish (lock release, new coordinator) |
| 0x3a8e50 | movement lock counter (obj +0x3a8; first +1 adopts, return 0x3a8e9a) |
| 0x39ad40 | movement locked |
| 0x42cbf0 / 0x42d000 / 0x42ce70 / 0x42cd10 | locomotor vf +0x1d0 path flag / suspend / resume / is suspended |
| 0x5d58f0 | space composition (recruit rule) |
| 0x3982b0 | object box (team box / model box) |
| 0x397900 | `Get_Space_Layer` (role) |
| 0x375270 | type in category `Super` |
