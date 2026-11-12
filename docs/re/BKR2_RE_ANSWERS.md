# BKR2 RE answers: tactical RE pass 2 (corners and start turns, Walk_Transition, detours, StoppedVsStopped, hidden targets, t25.2 choices)

Roadmap group bkr2 (2026-11-12, track C). Two RE passes merged; each part keeps its own source notes below. Every
section ends with its port consequence; the coordinator filed those on the backlog (ids at the end).

Part A (Q1, Q3-Q5, Q8): Part A (Q1, Q3, Q4, Q5, Q8). Behaviour read from StarWarsG.exe (FoC Steam x64, Ghidra export). Prose only.
Confidence: **[H]** read in code, **[M]** partly inferred, **[L]** guess. RVAs are image-relative. Port line numbers
are from `engine/tactical/movement/src/` at f1952720. Sources: corner pass `0x5070b0`, speed controller `0x61ccd0`,
decision `0x684f90`, `0x685490`, `0x684b10`, `0x2824f0`, `0x5c80e0`, detection `0x508170`, `0x5c7900`, SvS cost
`0x686b20`, land coordinator add `0x438b40`, composition `0x43c1b0`, `0x405500`, formation add `0x40e0f0`, Merge
`0x41fa90`, `0x4261d0`, `0x41cf50`, side step `0x5c71a0`, pack ctor `0x553f00`.

Part B (Q2, Q6, Q7): Steam exe for register flow). Prose only. Confidence: **[H]** read in code, **[M]** partly inferred, **[L]** guess. RVAs
are image-relative. Sources: Walk states 0x12 `0x61ff40` / 0x14 `0x61f510`, `0x61d220`, `0x38f9e0`, `0x395550`,
`0x395570`, `0x395590`, `0x395920`, `0x39a180`, `0x3a92f0`, `0x3a7b30` → `0x2654f0`, `0x27b480` / `0x27b4d0`; Bike vf6
`0x639e90`, its move step `0x63abe0` and speed step `0x638f40` (vftable `0x8b23c0`); formation step `0x422b70`, state 3
`0x423070`, state 4 `0x4234d0`, `0x584750`, goal points `0x583200` / `0x5833e0` / `0x5835c0` / `0x583930` / `0x583150` /
`0x583680`, Set_Move `0x423c10`, plan `0x41c1a0` / `0x417520`, fire trim `0x410e90`, attack slots `0x438c10`. Port line
numbers are from `engine/` at bkr1 tip f1952720. Roadmap: b0058 (Q2), b0078 (Q6), b0080 (Q7).

## Q1 (b0056). Where fraction-0 corners come from, and the start turn

**How a corner gets fraction 0** (`0x5070b0`, MOVEMENT §3.5.2 D.5/D.6, re-read) [H]. Per corner j the speed v starts
at the item-4 value and each try runs the arc probe; the try is accepted when every probe segment passes the static
line test `0x5a4c10` (movement class) **and** (v < 0.2·vmax **and j ≠ 0**, or the corridor test `0x5093c0` passes).
On a failure v /= 1.5; once v < vmin or v < 0.1·vmax: v := vmin when the **last line test passed** (so only the
corridor failed), else 0. f = v / vmax; f < f_min → 0. So:
- **Corner 0 (j = 0, the unit's own position, d_in = its facing)** must pass the corridor at every speed. The corridor
  fails on any **Static** layer-0x10 rectangle (a standing registered mover other than the unit; squad members are
  never in the layer, their container's joint box is) crossed by a probe segment. Then f = vmin / vmax, and vmin =
  `Min_Speed` × mode is **0 for most FoC land types** (102 land types in the FoC XML set no `Min_Speed`; 21 heroes /
  Night Sisters 0.1, the pod walkers 0.3) [M: type default 0]. So **a rear member of a standing group whose first arc
  crosses a standing mate gets fraction 0 at corner 0** with flat terrain. The tracker flags at plan time are those of
  the last tracker update, so every mate that stood still before the order is Static (the port's t26.3 choice (9) is
  retail's state either way the tracker / coordinator order falls) [M].
- **Later corners (j ≥ 1)** skip the corridor below 0.2·vmax, and the 1.5 step always lands a try in [0.1, 0.2)·vmax
  (or vmin). So a later corner gets 0 **only** when the static line test fails on every try down to that floor (the
  arc cuts impassable terrain / a stamped obstacle for the movement class), or when f < f_min (f_min > 0 only for the
  walkers listed in §3.5.2 D). Standing units never zero a later corner.
- Prediction for the port's breakdown: on `_MP_LAND_UTAPAU` the fraction-0 corners should be **mostly corner 0** of
  members ordered out of a standing group; a large share at j ≥ 1 on open ground points at the port's line test or
  probe geometry, not at the rule.

**What the unit does with fraction 0** [H unless tagged].
- **Corner 0 / the start** (`0x61e930` state 0, §3.5.2 A): |δ| ≤ R → Start Move; |δ| ≤ CE (FoC 180) needs wp[idx].+4
  > 0, so with fraction 0 the unit **turns in place** (0xf / 0x10 / 0x11, the stopped rate at speed 0, §3.5.1)
  unless it already faces within R. A moving unit given such a path is not "aligned" (`0x410170` needs wp[0].+4 > 0,
  §3.5.2 G): it coasts in End Move, 0x15, 0x16, state 0, and then turns in place. The turn is never an arc: there is
  no turn-while-moving branch for fraction 0.
- **A later corner** (`0x61ccd0`): the brake-ahead (B.5) with v_n = 0 sets T = 0 → **0x14 End Move** inside the
  braking distance of the corner's line, and after the line crossing the corner cap (B.4: turn-done byte +0x6c clear,
  facing not within R, state ≠ 0x12) with wp[idx].+4 = 0 sets T = 0 → 0x14 again. End Move brakes to 0x15, which
  restarts from state 0 (§3.5.1), and the start test at that waypoint fails the CE shortcut → **stop and turn in
  place at the corner** [H for the cap / brake-ahead and the start test; M for the exact creep sequence to the line,
  which mixes 0x12 / 0x13 restarts with brake-ahead re-entries].
- So both kinds of fraction-0 corner produce turn-in-place ticks; in the port both count as "start-turn ticks".

**Answer.** Fraction 0 at corner 0 comes from the corridor (standing neighbours) with vmin = 0, or from terrain; at
later corners only from terrain or f_min. Every fraction-0 corner that is not already aligned within R is a stop and
turn in place in retail, at the start and mid-path alike [H]. The 625 / 376 start-turn ticks are therefore not
suspect by themselves; what remains open is the share by corner index, which is a port measurement (count
`corner_zero` by j = 0 / j ≥ 1 and by cause: corridor-only vs line test), not an RE question. The retail frequency
can be checked with a trace of `0x61e930`'s branch to state 0xf (cu8h_dbg-style breakpoint) on a 10-unit group move.

**Port consequence**: none found by RE. `corner_pass` (`movement_system.cpp` 2038) and `walk_step` follow these
rules as written in §3.5.2. Suggested follow-up (not a parity difference): split `MovementStats::corner_zero` into
corner 0 / later and corridor / line-test causes, then compare with the prediction above (tier 1, S3).

## Q2 (b0058). Walk_Transition spec gaps

**(a) The end clip's start frame is rate + +0xf4, as coded** [H]. In `0x61f510` the rate V / S (`0x372ae0`) is kept in a
register across the play call (`0x3a92f0`, variant index 0, blend 0.0, that rate), then `0x61f716` adds pack +0xf4 to it
and passes the sum to `0x3a7b30` (flag 0). `0x3a7b30` → `0x2654f0` writes it as the controller's current frame
(controller +0xd0; 0 when the clip has fewer than 2 frames) and stamps the time (both the play and the frame set do
nothing while the object's anim lock +0x3a0 bit 3 is set). So the end clip starts **rate frames after the MOVE
phase**, not at it. With the shipped data (AT-ST V / S = 1.872 / 1.29872 ≈ 1.44, 30-fps clips, so the clip also
advances 1.44 frames per update) the end clip reaches its "ended" test (`0x39a180`: frame ≥ count − 2 − step, or the controller in state 2) about
**one frame earlier** than from +0xf4, so 0x15 comes one frame earlier.

**(b) End clips are numbered from 17, gaps not handled** [H]. `0x61d220` counts the types 17..20 (MOVE_ENDONE..ENDFOUR)
that have at least one variant (`0x27b4d0`: variant count of the type), k = that count, and the chosen clip is
17 + (int(cur / seg) mod k). A model with, say, 17 and 19 but not 18 gets k = 2 and can ask for 18 (absent). Two more
facts the spec lacked: the MOVE test is "current clip ∈ {2 MOVE, 0x42 WALKMOVE, 0x43 CROUCHMOVE}" (the code excludes 3
from the {2, 3, 0x42, 0x43} mask), and seg = (MOVE's frame count / k) **in integer division, then float**, with MOVE's
count read as **type 2 variant 0** (`0x38f9e0`(obj, 2)) even while 0x42 / 0x43 plays.

**(c) A model without end clips, or another clip current: no speed test** [H]. The speed test into 0x15 exists only
in the branch for types without `Walk_Transition`. For a transition unit in 0x14 the target clip is:
- current ∈ {2, 0x42, 0x43}, k > 0: the end clip of (b) (switched only when the phase ≤ the clip step);
- current ∈ {2, 0x42, 0x43}, k = 0: the object's idle type `0x395920` (IDLE 0; 0x47 while the object is in the state
  `0x39b950`(obj, 7, 1); a type-pair override for attached objects), played like an end clip (variant 0, rate V / S,
  frame rate + the stale +0xf4, +0xf0 := +0xec);
- current 16 (MOVESTART): 17;
- any other current clip (FIRE_MOVE 61, Deployed_Walk 115, a turn clip, …): **itself**, nothing is played.
When the current clip equals the target, 0x15 follows as soon as it has ended (`0x39a180`); otherwise the move step
runs. So a transition unit always leaves 0x14 on a clip end. FoC data check (`docs/formats/validation_models_meg.csv`):
the transition models listed there (AI_RANCOR, EV_AT-AA, EV_AT-ST, EV_PODWALKER, EV_SPMAT, UI_NIGHT_SISTER) all have
MOVESTART and MOVE_ENDONE..ENDTHREE (k = 3, no gap) [H for those files]; AT-AT, Scavenger droid and the other EaW-only
models are not in that list [L].

**(d) Bike: its own state machine, not Walk F** [H unless tagged]. BIKE_LOCOMOTOR vf6 `0x639e90` runs states 0x20–0x25
and never reads `Walk_Transition` (+0x35); only the acceleration / deceleration getters (vf +0x2d8 / +0x2e0 =
`0x42c400` / `0x42c4c0`, shared with Walk) depend on the flag.
- 0x20 turn in place (budget pack +0xe8, else the turn rate); budget left → 0x21 with a path, else 0x24.
- 0x21: no path or path done → 0x23. Path started (+0x6d): MOVESTART not current → play 16 (random variant, rate 1.0,
  blend GC `0x140b39938` = 0.0) and speed +0xec := 0; MOVESTART current and ended → 0x22. Every frame the move step
  `0x63abe0` runs. Path not started → 0x23 when MOVESTART is current, else 0x24.
- 0x22: current clip not in 2..4 → play MOVE (random variant, looping, rate 1.0); move step.
- 0x23: +0xf0 := +0xec when +0xf0 = 0 (latched on the first 0x23 frame, so the deceleration getter's u stays fixed);
  current ≠ 17 and speed < 0.55 × V (`0x140a44324`) → play 17 (random variant, rate 1.0); move step; then **speed ≤
  deceleration + 0.0001 → 0x24** (a speed test, no clip test). 0x25: +0xf0 := 0, then 0 (path not started) or 0x24.
- Move step `0x63abe0`: in 0x21 and 0x23 the turn budget is R × (v / V); the speed step `0x638f40` targets V (or the
  speed-slave cap, the waypoint fractions and the next-waypoint brake), **T = the deceleration value in 0x23**, and moves
  the speed by vf +0x2d8 up / vf +0x2e0 down. [M: the rest of `0x63abe0` was not compared line by line with Walk C.]

**(e) Which variant** [H]. Counts read through `0x38f9e0`(obj, type) are **variant 0** of the type (`0x27b480`(set, type,
0)): the acceleration's MOVESTART n, the deceleration's MOVE_ENDONE m, seg in (b), the 0x13 phase rescale. Counts read
through the controller (`0x395570`, `0x39a180`, `0x2654f0`) are the **playing** clip: the drawn MOVESTART variant in 0x12
(played with 0xFFFF = random), variant 0 for the 0x14 end clip (played with index 0). The listed FoC models have one
variant per transition clip, so variant 0 and the drawn one coincide there.

**Port consequence.** Tier 1 (sim: 0x14 → 0x15 timing, Bike start / stop). Backlog text (tier 1, S2): *"Walk_Transition
0x14 (locomotor.cpp wt_end_move 145-177): retail 0x61f510 sets the end clip's frame to rate V/S + +0xf4 (0x2654f0; 0
for a clip of < 2 frames), so it ends about one frame earlier than from +0xf4; the MOVE test is clip 2, 0x42 or 0x43
with seg = int(count(MOVE variant 0) / k); a transition unit never uses the speed test in 0x14: with k = 0 it plays the
idle type 0x395920 (variant 0, rate V/S, frame rate + +0xf4, +0xf0 latched) and with any other current clip (not 16)
it waits for that clip to end, then 0x15 (BKR2 Q2 (a)-(c)). Drop the [M]/[port] notes on 'rate + +0xf4' and 'numbered
from 17' (both [H]; gaps are not handled in retail either)."* Backlog text (tier 1, S2): *"Bike (Scout_Trooper):
retail 0x639e90 is a separate state machine 0x20-0x25 (BKR2 Q2 (d)): 0x21 holds MOVESTART (speed 0 at its play, turn
budget x v/V) until the clip has ended, 0x23 latches +0xf0 on its first frame (fixed u for the deceleration), brakes
toward T = deceleration with the turn budget x v/V, plays MOVE_ENDONE below 0.55 V and leaves on speed <= decel +
0.0001. The port runs Bikes on the Walk step with wt.states off (movement_system.cpp walk_transition_fill 3494-3512)
and only mirrors the states in special_system.cpp bike_frame 551-590 (its 0x23 exit tests min_speed)."* Variant 0
(e): none visible with the listed models; retag movement_system.cpp 3494's [M] as [H] (getters read variant 0, ended
tests the playing clip) — S3 if a model with several differing variants turns up.

## Q3 (b0063). Step 10's `0x685490`: what it does

**Call site** (`0x684f90`, end of the decision) [H]: `alt` starts **true** and is set false only when step 6 ran the
spare-point search `0x5c6ba0` and it failed. Then: (a's or b's class > 6 (above Bike)) **and alt** → Path Around
`0x685920`(a, b, 1) → state 1; else `0x685490`(record, a, b) **true → state 8; false → falls through to 7 Try Again
Later**. With alt false the whole step is skipped → 7. (MOVEMENT §5.4.1 step 10's "else `0x685490` → 8" is
incomplete: `0x685490` returns a bool and its failure is state 7.)

**`0x685490`(record, a, b)** [H unless tagged]:
1. D = |X − a.P| + r_a (`0x3969f0`), X = where a's travel ray leaves b's swept rectangle (`0x67c1a0`, as Path
   Around flag 0); when `0x67c1a0` fails **D = 0** (no |a − b| + r_a + r_b fallback, unlike `0x5c80e0`).
2. A dynamic-pathfinder request built like Path Around's (`0x673ac0`, `0x67c9f0`(a)): obstacle mask +0x40 = 0x10
   (0x11 when a's UnitCollisionClass is 5 or 9), goal-spot mask +0x3c = 0x10, +0x38 "skip a smaller class" = 0,
   `MovementBoxExpansionFactor` when > 0, **flags +0x48 = 2**, **no forced id** (+0x2c unset). Flags 2 (§1.5.1
   item 1): a standing unit is an obstacle only when it shares with a an undecided / detouring record; this record
   is in state 0, so a standing b is still an obstacle; other standing units are ignored; moving units count by
   relation as usual. `0x67b310` is called with D into a **local** path. **The path is never pushed onto a**: it is
   freed at the end. "The units-ignoring detour" is only a probe line.
3. No path → false. Path found: a needs an owner player and a locomotor pack (obj +0xa8), else false.
4. Box query (layer 0x10, `0x5d7a00`) of half size **max(80, 1.75·D)** around a; the hits are sorted by 3-D
   distance from a (`0x684b10`, strict <). In that order, each object o ≠ a whose owner is **hostile** to a's owner
   (`0x2824f0`: relation-table entry == 1) and whose footprint the probe path meets (`0x441b60`, the MovingVsStopped
   directional-radius test of §5.3.1 item 2, run against the probe path) is the answer: a's formation F = pack
   +0x1f0; **F null → false**; else a block is pushed onto F (`0x41fd30`) and the call returns **true**. Only the
   nearest such o is used. b itself is not excluded but is normally off the probe path (it was an obstacle) [M].
5. The block: kind 3 (object), object o, position (0, 0, 0), flags +0x48 = 0 (no attack-move, no guard), facings
   1e18, mask 0x3f, +0x50 = 1, +0x54 = 1e18, +0x5c = 0, +0x60 = 0x1000001 (the composition's template). Push with an
   object and no guard flag runs the attack slots `0x438c10` and the plan (BKR1 Q8), so **a's whole formation
   switches to attacking o**; the move block stays below it [M: the resume when o dies is the normal block pop].
6. No hostile on the probe path → false → state 7.

**State 8** answers max speed and ends at the next service (→ 5); a keeps its old path until the attack plan replaces
it. **State 7** answers **speed 0 for the mover** (unless it can crush b) and is re-decided every 30 frames
(§5.4.1 service table).

**Answer** [H]. `0x685490` never moves the mover past b. It looks for a hostile unit on a path that ignores standing
non-involved units, and if one is there gives a's formation an attack order on it (state 8); otherwise the mover is
held (state 7) and retried every 30 frames. Nothing in it lets a walk through b.

**Port consequence** (tier 1). `movement_system.cpp` 3389-3410 step 10 resolves the record and marks
b as planned around, so the mover walks through b; it also does this when alt is false. Backlog text: *"MvS decision
step 10 (movement_system.cpp 3389-3410): retail 0x684f90 runs step 10 only when alt is true (alt false → 7); Path
Around (a, b, 1) → 1; else 0x685490: a Path Around search (flags 2, no forced id, D = |X − a| + r_a or 0) whose path
is NOT pushed, then the layer-0x10 objects in the box of half size max(80, 1.75 D) around a, nearest first (3-D): the
first hostile one (relation 1) whose footprint the probe path meets (0x441b60) gets a kind-3 attack block (flags 0,
+0x50 = 1) pushed onto a's formation (0x41fd30) → state 8; no formation, no path or no hostile → state 7 (mover held
at speed 0, re-decided every 30 frames). Drop the walk-through stand-in and the step-3 stamp it sets."* Tier 1, **S2**
(a vehicle walks through a unit where retail stops or attacks; rare since the t24 detours).

## Q4 (b0064). StoppedVsStopped Try Again Later: can the pair get a second record?

**Detection pair filter** (`0x508170`) [H]. Before classifying a candidate, detection walks **every record id of
the checker** (the id hash) and skips the candidate when any of those records lists it as a participant
(`0x5c7900`: a linear search of record +0x40..+0x48). No kind or state test. So while **any** record holds both units,
no MovingVsStopped, MovingVsMoving or StoppedVsStopped record is made for the pair, whichever unit checks.

**The SvS record's life** (MOVEMENT §5.4.3, re-checked against the filter) [H]. State 4 → 0 every 30 frames, the
resolution runs again; the contact test is never re-run; vf +0x78 is false (a new path does not re-decide it); it
ends only by validity (a participant gone → 2; no locomotor on either / one on an impassable cell → 3) or by a
resolved move (state 1 → 2). It answers max speed and gives no vector outside state 1. Consequences in retail:
- A pair whose SvS resolution failed (no spot, no path, refused claim, **both infantry with targets**, both
  deploy-immobile) keeps the record **for as long as both live**, and while it lives the two units **never get a
  MovingVsStopped / MovingVsMoving record against each other**: when one later walks into the other there is no
  hold, Path Around or Repulsion for that pair (other pairs are unaffected).
- Each retry re-runs the mover pick and the ring search around the mover even if it is now walking an order: when
  the other unit is still near, a spot ≠ its position is found and a kind-4 path to it is claimed and pushed on top
  of the order path (claim: an order path has no owner, so it succeeds); when they are apart, ring 0 is the unit's
  own position → state 4 again [H for the rules, M for how often this happens in play].
- Rule 5 (two Infantry-class units, both with targets → nobody) is the common entry: two squads overlapping in a
  fire fight. Squad containers are Infantry class and register (§5.2).

**`Max speed 0` in the mover pick** [H]. Retail has no speed rule. The cost `0x686b20` with v = 0 has along = side = 0,
so t is "none" → **cost 0**; such a unit is the cheaper candidate (ties go to p1), its claim is accepted, the path is
pushed and it never moves (the Walk step returns at vmax_u = 0), so the record stays in state 1 while the unit holds
the path (and the unit counts as Moving). Effective max speed 0 needs a speed override or modifier of 0 [L: no FoC
land type has Max_Speed 0 with a Walk locomotor; Lua speed overrides can].

**Answer** [H]. No: a pair with a live record of any kind gets no second record. The retail SvS record in Try Again
Later lives until a participant dies or becomes invalid, and so blocks every later collision record of that pair.

**Port consequence** (tier 1). `svs_service` (`movement_system.cpp` 4293-4347) ends a state-4 record at a retry when
the footprints no longer overlap or either unit is Moving, so the pair is re-detected and gets MvS / MvM handling
retail never gives it. Backlog text: *"StoppedVsStopped Try Again Later (movement_system.cpp 4343-4346): retail keeps
the record (4 → 0 → resolution every 30 frames) until validity ends it (participant gone → 2; no locomotor / on an
impassable cell → 3); detection's pair filter (0x508170 → 0x5c7900, any kind, any state) then blocks every
MovingVsStopped / MovingVsMoving record of that pair, and each retry re-runs the mover pick and ring search, pushing
a kind-4 path to the spot even onto a unit walking an order when the other is still near. Remove the [port] end rule
(overlap ≤ coefficient or a unit Moving)."* Tier 1, **S2**. Second text: *"svs_pick_mover (movement_system.cpp
4259): retail has no 'max speed 0 = immobile' rule; 0x686b20 gives such a unit cost 0, so it is picked first
(ties to p1) and holds the record in state 1 without moving. Drop `!(walk_max_speed(x) > 0)` from imm."* Tier 1,
**S3** (needs a 0 speed override).

## Q5 (b0067). What a squad member's pack +0x1f0 holds

**Writers of pack +0x1f0** [H]: the `LocomotorDataPackClass` ctor `0x553f00` (:= 0); formation add `0x40e0f0` (the
added object's pack := the formation); Merge_Formation `0x41fa90` (the merged members); clears `0x410b80` /
`0x421910`. No other function stores a formation pointer there (a scan of every direct store at +0x1f0 / index 0x3e).
`0x40e0f0`'s callers: the land composition `0x43e5b0` / `0x43c1b0`, `Issue_Degenerate_Move` `0x4f7360`,
`Split_From_Formation` `0x4f84e0`, the companion spawn `0x3a2970` (only for an object with no parent, obj +0x2b0 =
0), and the space compositions.

**Squad members never reach a formation** [H]. The land coordinator add `0x438b40` (vf3): obj byte +0x348 is the
index of behaviour 0x16 (TEAM) in the object's behaviour table (obj +0x332 + id; −1 = none). An object **with** TEAM
is added only when its locomotor answers vf +0x230 (team container); an object **without** TEAM whose locomotor
answers vf +0x228 is replaced by its parent obj +0x2b0 (its container; none → not added); any other object is added
itself. So coordinators, and therefore formations, hold squad **containers** and lone units, never members; Split
and Merge act on formation members only. A member's pack +0x1f0 stays **null** (ctor value) for its whole life.

**Use in the side step** (`0x5c71a0`) [H]: 0 when a and b share a container (obj +0x2b0); the overlap coefficient
branch needs **a's formation non-null** (`0x4261d0`) and b's pack +0x1f0 equal to it (`0x41cf50`); otherwise the
distance ramp. A member a has F = null → ramp; a lone unit a in F against a member b of a container in F: b's +0x1f0
is null ≠ F → ramp. (The MvM chooser's same-formation test is guarded the same way: F0 non-null first.)

**Answer** [H]. Null. Member pairs (and member / formation-unit pairs) always use the distance ramp, which is what the
port's "no form_id on a member" gives.

**Port consequence**: none. (`land_team.cpp` 565: the [M] note can become [H].)

## Q6 (b0078). Hidden target after arrival (C.3) and the stored point

**The stored point is a "last observed target position", refreshed every formation step** [H]. `0x584750` (log text
"LastObservedTargetPosition") writes block +0x18..+0x20 := `0x583150`(block, viewer) for a kind-3 block with an object;
`0x422b70` calls it for **every block of the stack at the start of every formation step** (each coordinator update,
MOVEMENT §4.6.2), viewer = the formation's lead returned by `0x41a9b0` (the register is passed through, checked in the
disassembly). `0x583680`'s redirect applies once the point is not the 1e18 default, i.e. from the second step on:
object hidden for the viewer's owner (`0x35f470`, force 1) → the stored point itself, so it **freezes at the last
position seen**; else stealthed (`0x39b140`) and the viewer's type without `Target_Stealth_Units` (type +0xa4) → the
**viewer's own position** (written into the store each step); else the true goal point (hard point; on land a team
container's member from the viewer's TARGETING `0x566aa0`; else the object's position), clamped to the playable box.
This replaces FIGHTER_FLYING §4.11 C.3's "[M: no writer … in practice the true position]".

**State 3 `0x423070` runs C.3 on every formation step, before the 30-frame `0x410a60`** [H]. With one block, members and
an object: L = `0x41a9b0`'s point (the members' centroid; one member: its position), lead = its return. bS = stealthed
and (no lead or the lead's type lacks `Target_Stealth_Units`). P = `0x583200`(top, lead); on land `0x583200` is
`0x583150` (its kind-3 offset branch is space-only, mode 2), so P = the stored point while hidden, the lead's position
while stealthed. With a lead and (bS or hidden): d = 2-D |L − P|:
- **d ≥ 40 → state := 0 and the step continues: the plan `0x41c1a0`(F, 2) runs in the same step.**
- d < 40 → Set_Move (below) and every member gets `0x38d730`(member, null, 0, 1, 0, −1, 0) [M: clears its attack
  order]; the step continues with the new state 0.
State 4 `0x4234d0` has the same test without the member calls; state 4 is entered only from `0x420a40` (the space
idle-drift block) [M: not reached on land].

**The plan in state 0 uses the stored point** [H]. 0x417520's land branch takes G from `0x5833e0`(top, lead) →
`0x5835c0` → `0x583930` → (land) `0x583150`(top, lead), the same redirect. Also on the hidden object: c = `0x40edd0` is
false (no item-6 stop), the fire trim `0x410e90` is skipped (its gate is `0x35f470`(members[0]'s owner, **the object**,
force 1) and the stealth test), staging `0x40e260` acts only when a member is inside its min range of T. So **a
formation whose target becomes hidden at its slot re-plans at once and walks, untrimmed, to the target's last observed
position** (via its slot point Q' when ρ > 0). While it walks (state 1), `0x4192b0`'s step 4 fog test fails (no re-path;
D against the frozen point stays small anyway); when the target is seen again the 30-frame checks resume (land stop when
every member can fire). On arrival at P: d < 40 → the position block. Recorded goal and the 0x4165f0 / 0x4192b0
distances read the same stored point.

**Set_Move on d < 40** [H]. A copy of the top (`0x40d880`) with point := L (x, y, z), kind := 1, object := null; flags
(+0x48, e.g. 0x01 attack-move), θ / ρ (+0x3c / +0x40) and the other fields kept; `0x423c10`(F, b, clear 1): the stack is
cleared, b pushed, recorded goal := `0x583150`(b, members[0]) = L, F +0x21c := −1, **state := 0, so the plan to L runs
in the same step** (members re-form around their centroid; the single-member shortcut makes a lone unit stand), then
`0x3a8710` per Land-layer member. Afterwards the formation holds a position block at L whose flags decide the scan
range and auto-engage rules (BKR1 Q8 table: an Attack_Move(position) arrived row → R + `Attack_Move_Response_Range`).

**Port consequence.** Differs (tier 1). Backlog text (tier 1, S2): *"Hidden / stealthed attack target (tactical_battle.cpp
attack checks ~2179-2236, the drop ~2280-2300): retail keeps a last-observed point per kind-3 block (+0x18, 0x584750 at
the start of every formation step 0x422b70 from 0x583150(block, lead): live while seen, frozen while hidden for the
lead's owner, the lead's position while stealthed against its type) and every plan, re-path and C.3 test reads it
(0x5833e0 / 0x583150 -> 0x583680). C.3 (0x423070) runs on every formation step, not on the 30-frame timer: d = 2-D
|L - P| >= 40 -> state 0 and the plan in the same step to the last observed point (no item-6 stop, no fire trim: the
formation walks to where it last saw the target); d < 40 -> Set_Move(copy of the top as kind 1 at L (xyz), object null,
flags / theta / rho kept, clear) with an immediate plan to L, plus 0x38d730(member, null, ...) on each member (BKR2 Q6).
The port uses the target's live position (P = tp, 2209), checks on the 30-frame block timer, and drops the block with
no move and no position block (needed by b0177 / b0185 for the arrived attack-move scan)."* Backlog text (tier 1, S3):
*"Fire-trim gate (tactical_battle.cpp land_attack_order 2037-2045): retail 0x410e90 tests the object with 0x35f470
(members[0]'s owner, force 1), the port tests the point tp with force 0 (fog_point_fogged); use the object test as
attack_target_unseen does."* The stealth branch (P = a member's position, d < 40 → drop) matches; the port's comment
"[M] the block's stored point is the 1e18 default" (2203) becomes [H] "last observed point". Order of tests when both
hidden and stealthed: retail returns the frozen point (hidden is tested first), the port the lead's position (S3, rare).

## Q7 (b0080). t25.2 [port] choices, re-checked

1. **Plan and slots at the order** [H]. Retail runs `0x438c10` in the land composition `0x43c1b0` (coordinator vf12,
   common tick step 10, after the object services) or in Push `0x41fd30` (at the push, during the pushing unit's
   service); the plan runs at the formation step once the gate `2·FPS < now − F +0x1dc` opens (unstaggered: the same
   update; FoC `PathFrameDelay` only on Snowspeeder and Lancet, both air). So positions, slots 44 / 45, the reference
   path and T's point are taken after this frame's walks, not at the command phase. **Port: differs only in the
   in-frame sampling point**, the same as every port land move ("[port] built here at the order", movement_system.cpp
   1051); it belongs to the tick-order entry b0182, no new entry.
2. **The formation class** [H]. The slot test uses `0x4779e0` (most mask bits inside the members' intersection); the
   path, the slot leg's line test and the plan use F +0x1c4 (the plan class, `0x414720`'s shield switch). **Port: now
   matches** (attack_approach.cpp 346 `f.best`, 554 / 580 `f.mask`, s78n t26 round 2 and bkm1 t03); the PHASE4 t25.2
   "[port] the lead's mask" note is stale.
3. **Reference distance: 3-D** [H]. `0x438c10` takes the mean of `0x41a9b0`'s points with z and the 3-D distance
   (strict <, first wins). **Port: 2-D** (attack_approach.cpp 439): on uneven ground a different reference formation,
   hence different d0 / d1, ring order and slots. S2.
4. **First / last segments** [H]. Retail takes the literal first segment (path[1] − path[0]) and last segment of the
   reference path, normalises both (2-D) and tests only the last for zero length; the slot leg's v is the **3-D** unit
   last segment of the first path (no leg when its 3-D length is 0), Q and Q' carry v's z term (Q' = G + q·rot(−v, θ),
   so its 2-D reach is q × |v_xy|), and the dot test compares the two paths' normalised **3-D** first segments. **Port:**
   first / last non-zero 2-D segments. On retail-shaped paths (no repeated node) the non-zero rule picks the same
   segments, so it matches; the 2-D v shortens nothing on slopes (Q' differs by the factor |v_xy|, < 1 % below a 14 %
   grade). S3.
5. **q of the slot leg** [H]: answered by BKR1 Q7 (c) (same-region shortcut, halving above 40, q = 0 when blocked at
   once and start → G still planned); filed as b0184. No new entry.
6. **A plan failure keeps the block** [H]. On a failed real plan `0x41c1a0` sets F +0x1dc := now and returns 0; the
   block stays, the state stays 0, **the members' path stacks are not touched** (a formation re-planning while it moves
   keeps walking its old path), and the plan is retried at the first step with `2·FPS < now − F +0x1dc`, i.e. after 60
   frames. **Port:** keeps the block but stops every member and marks it failed (attack_approach.cpp 554-577), and
   retries on the 30-frame attack check. S2.
7. **Air-led formations** [H for the slots, L for the plan]. `0x438c10`'s L needs a Land-layer lead (`0x397900`(lead, 1)
   = 0x10), so an air-led formation gets no slot (port matches). Whether its plan takes 0x417520's land branch (item-6
   stop, staging, fire trim) depends on the formation type F +0x228 copied from the composition descriptor (`0x43e5b0` →
   `0x4241a0`), whose writer in `0x43c1b0` was not traced [L]. Probe: in retail, order two Snowspeeders (one formation)
   onto a standing AT-ST 1000 away on open ground; they stop near weapon range iff the trim applies.

**Port consequence.** Backlog text (tier 1, S2): *"Attack slots reference formation (attack_approach.cpp attack_slots
437-445): retail 0x438c10 picks the formation nearest the mean of 0x41a9b0's points by the 3-D distance (z included,
strict <, first wins); the port measures 2-D, so on uneven ground the reference path, d0 / d1 and the ring order can
differ (BKR2 Q7 item 3)."* Backlog text (tier 1, S2): *"Attack plan failure (attack_approach.cpp attack_plan 554-577):
retail 0x41c1a0 keeps the block and the members' current paths (no stop), stamps F +0x1dc := now and retries the plan
once 2·FPS < now - F +0x1dc (60 frames); the port stops and fails every member and retries on the 30-frame attack check
(BKR2 Q7 item 6)."* Backlog text (tier 1, S3): *"Slot leg in 3-D (attack_approach.cpp 580-600): retail 0x417520 uses
the 3-D unit last segment v (Q' = G + q rot(-v, theta) with z, 2-D reach q |v_xy|) and a 3-D dot of the normalised first
segments (BKR2 Q7 item 4)."* Items 1, 2, 5: none (b0182 / b0184 cover 1 and 5; 2 matches, retag the PHASE4 note). Item 7:
[L], probe above.

## Q8 (b0081, with b0036). "Engaged short-range attacker" in MOVEMENT §4.2 item 4

**The flag** (`0x43c1b0`) [H]. Attack order = block kind 3 with an object and guard flag 2 clear (`0x583670`). For
the seed only: solo when TARGETING slot 44 (in max range) or melee (`0x372210`), else d = 3-D seed → target object
position, solo when d < r_seed + E_r + `Solo_Attack_Range` (GC +0x1548, strict), else **short-range := d < E_r ×
`Short_Range_Attack_Formation_Coefficient` (GC +0x1544, 1.6), strict**. No target, lock, TARGETING state or "already
fighting" test is read: "engaged" adds nothing to item 6's d < 1.6·E_r flag.

**Its only use** is recruitment test 3 (§4.2 item 7): C passes when **(S has no parent team, or
`ShouldInfantryTeamsSplitAcrossFormations` (GC +0x149d) is false) and (not an attack, or S not short-range, or C's
type pointer = S's)**. The team clause takes S's parent obj +0x2b0 and its TEAM behaviour. Since coordinators hold
containers and lone units only (Q5), S's +0x2b0 is null for every land seed and the clause always passes [M: a lone
unit with a parent object was not found on land]. FoC sets the GC true; it matters only if members were ever seeds.

**b0036 (test 9, engagement compatibility)** was ported in bkm1 t04: `formation.cpp` 121-122 tests K(C) ≤ E(S) and
K(S) ≤ E(C) with K = min(E, `Targeting_Min_Attack_Distance`), 0 under guard (`movement_system.cpp` 707-728), for every
order kind. That matches §4.2 item 7 test 9 [H]. Test 3 is `formation.cpp` 108 (`!s.short_range_attack ||
o.type_name == s.type_name`), with the flag from `movement_system.cpp` 742-743: the same rule; the type-name
comparison stands for the type pointer.

**Answer** [H]. Yes: "engaged short-range attacker" is exactly the item-6 flag d < 1.6·E_r (attack orders, non-solo
seeds); there is no further engagement condition. The "engagement distance is compatible" bullet is test 9, now in
the port.

**Port consequence**: none (the port omits test 3's team clause, which cannot fire for land seeds [M]).

## Filed port consequences (backlog, tier 1, parity-sim, from bkr2)

| Question | Backlog | Sev | What |
|---|---|---|---|
| Q1 | b0192 | S3 | measurement: split corner_zero (corner 0 vs later, corridor vs line test) |
| Q2 | b0193 | S2 | Walk_Transition end: clip frame rate + +0xf4, MOVE test clips, no speed test for transition units |
| Q2 | b0194 | S2 | Bike state machine 0x20-0x25 (MOVESTART hold, braking, MOVE_ENDONE below 0.55 V) |
| Q3 | b0189 | S2 | MvS step 10: needs alt; `0x685490` attack block on a hostile in the way, else state 7 (no walk-through) |
| Q4 | b0190 | S2 | StoppedVsStopped Try Again Later kept until validity ends it; blocks other records of the pair |
| Q4 | b0191 | S3 | no "max speed 0 = immobile" rule in the mover pick |
| Q5 | - | - | none: a member's pack +0x1f0 is always null, the port's distance ramp matches |
| Q6 | b0195 | S2 | hidden target C.3: last-observed point per block, every formation step, kind-1 Set_Move at L |
| Q6 | b0196 | S3 | fire-trim gate is the object hidden test (force 1) |
| Q7 | b0197 | S2 | reference formation by 3-D distance |
| Q7 | b0198 | S2 | plan failure keeps paths, retry after 60 frames |
| Q7 | b0199 | S3 | slot leg 3-D segments; stale PHASE4 t25.2 note; air-led land branch [L] (probe named) |
| Q8 | - | - | none: b0036 (bkm1 t04) matches retail |

All touch track A's movement files or track D's tactical files, so none was ported in bkr2. The plan-at-the-order
timing of Q7 item 1 is the tick-order question filed from bkr1 as b0182.
