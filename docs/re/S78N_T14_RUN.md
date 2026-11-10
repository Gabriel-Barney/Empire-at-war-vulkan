# s78n t14 retail run: land-squad formation (member lag, start delays, Rows / Dartboard slots) (2026-11-10)

Kit `tools/probe_mod/s78n_t14_squads/` (`run_s78n_t14.cmd`, method in its README), run `out/20261110-043421`
(`report.txt`, `records.log`). Retail FoC, windowed skirmish on `_MP_LAND_GEONOSIS`, human Empire vs Easy AI Rebel,
hands-off. Four invulnerable squads of the human player, one after another on a flat corridor from (2000, 800):
`Imperial_Medium_Stormtrooper_Squad` (`Stormtrooper_Team`, **Rows**, 9 x `Squad_Stormtrooper`) in trials 1 and 3,
`Rebel_Mini_Infantry_Squad` (`Rebel_Trooper_Team`, **Dartboard**, 5 x `Squad_Rebel_Trooper`) in trials 2 and 4. Each
squad marches four legs: 600 at 165 deg, 200 at 165 deg, 400 at 195 deg (30 deg turn), 200 at 200 deg (5 deg turn).
Polls every 3 frames; f = `GetCurrentTime()` x 30. The kit's own verdicts were Rows Q1 TIGHT / catch-up YES /
"STAYS OPEN", Q2 STAGGERED (wider than 0..6), Q3 Rows MATCH; Dartboard Q1 LOOSE / catch-up NO / CLOSES, Q2
STAGGERED (within 0..6), Q3 DIFFERS, Halton fit no match. The readings below are by hand from the report, the
records (container speed per 3-frame poll, members in the container frame) and the binary
(FIGHTER_FLYING §7, §7.2, §7.3, §7.7).

Tags: [H] read from the code and matching the run; [M] inferred; [L] guess.

## XML values (FoC)

| | `Squad_Stormtrooper` (Variant of `Stormtrooper`) | `Stormtrooper_Team` | `Squad_Rebel_Trooper` (Variant of `Rebel_Trooper`) | `Rebel_Trooper_Team` |
|---|---|---|---|---|
| `Max_Speed` | 1.0 | 0.8 | 1.0 | 1.1 |
| `OverrideAcceleration` / `OverrideDeceleration` | 0.05 / 0.05 (base type) | none read (the file has `Override_Acceleration` .1, a tag the parser does not know) | 0.07 / 0.07 (base type) | none read (`Override_Acceleration` .08, unknown tag) |
| `FormationSpacing` | 0.3 (base type) | 1 | 0.5 (base type) | 0.5 |
| `Custom_Hard_X/YExtent` x `Scale_Factor` | 2.0 x 1.5 = 3.0 | | 2.0 x 1.5 = 3.0 | |
| `FormationRaggedness` / `MaxJiggleDistance` | 0.2 / 2 | - / - (type default 0) | 1.6 / 3.5 | - / - (default 0) |

GameConstants: `Object_Max_Speed_Multiplier_Land` 1.56, `Max_Move_Frame_Delay` 6.

## Measurements

| | Rows (trials 1, 3) | Dartboard (trials 2, 4) | Binary rule / prediction |
|---|---|---|---|
| container cruise / frame | 1.247-1.249 | 1.542-1.560 per leg; 1.556-1.563 per poll at steady speed | team speed min(container, members) `Max_Speed` x 1.56: **1.248** / **1.560** (MOVEMENT §3.4) |
| member peak speed / container cruise | 1.15-1.25 | 1.00-1.01 | member vmax = its own 1.0 x 1.56 = 1.56: ceiling **1.25** / **1.00** (§7 step 4) |
| container acceleration = deceleration / frame (3-frame polls) | 0.078 (speed 0.155, 0.390, 0.624, 0.856, 1.094; braking the same steps) | 0.109 (0.218, 0.546, 0.875, 1.202) | min over members of `OverrideDeceleration` x 1.56: **0.078** / **0.109** (0x5ab540, §7.7) |
| steady-march lag behind the settled slot, mean (max) | 0.0-0.3 (1.0-2.5); within 6 of the slot from start +0..+60 | 1.8-5.7 (4.7-21.2); one member 15 and one 21 behind for a whole leg | Rows close up, Dartboard members never gain while the container cruises |
| closing time after a start / turn | lag max 8.5-10.8 at start +15, 1.0-2.5 at +60, < 1 at +120 | none while cruising; all close during the container's braking | exponential at (vmax - vK) / 10 = 0.031 / frame for along 1..10 |
| container slowed by a lagging member | legs 1-3 of trial 1 / 3 starts (speed 0.77-1.18 while a member is 17-21 behind the container); implied HX 15.0-15.4 | trial 2 leg 4: 1.18 -> 1.55 over 85 frames, implied HX 15.69-15.86 | straggler cap 0x5ab640: HX = 15.0 for the 3 x 3 Rows grid (§7.7) |
| member start spread, frames | 4.1-6.0 (trial 3 leg 2: 19.9, leg 3: 17.3) | 0.9-6.0 | delay 0..6 (§7.3 item 2); the two long ones are not delays (below) |
| re-lay at leg 2 (0 deg) / 3 (30 deg) / 4 | kept / laid (members swap, same grid) / kept (path 0.4 and 5.0 deg from leg 3) | kept / new scatter / new scatter (path 10.7 and 9.4 deg from leg 3) | re-lay when dot(new, laid) <= 0.99 (8.11 deg) (§7.2) |
| Rows grid RMS, member spacing 0.3 (pitch 12) vs container spacing 1.0 (pitch 26) | 0.97-1.79 vs 15.9-16.3 | - | pitch 2 hx + 20 g = 12 (§7.2) |

## Readings

1. **Team speed and member catch-up [H].** The container walks at the team speed min(`Max_Speed` of the container,
   of every member) x 1.56 (MOVEMENT §3.4): Stormtroopers min(0.8, 1.0) x 1.56 = 1.248, measured 1.249; Rebels
   min(1.1, 1.0) x 1.56 = 1.560, measured 1.556-1.563 at steady speed (the per-leg 1.553 includes the ramps). A member
   steers with **vmax = 0x370f00(member type, member object)** (disassembly: rdx = the member at 0x14062d97a), the
   member's own effective max speed with no team cap: 1.0 x 1.56 = 1.56. While behind its slot (along > 1) its speed
   target is (vmax - vK) x clamp(along / 10, 0, 1) + vK (§7 step 4). Stormtrooper members can reach 1.56 / 1.248 =
   **1.25** x the container (measured peak 1.25), so they close an 8-10 unit start / turn lag in about 60 frames
   (TIGHT). Rebel members have vmax = vK = 1.56: the formula gives vK whatever the lag, so a member that starts late
   or is given a slot ahead of it stays behind for the whole leg (LOOSE, peak 1.00-1.01) and closes up only while the
   container brakes (pre-stop lag 0.1-1.7). This is a data effect (`Rebel_Trooper_Team` `Max_Speed` 1.1 above the
   members' 1.0), not a separate rule. Matches FIGHTER_FLYING §7 step 4 and the port's `steer_member`
   (`vmax = loco_max_speed(member)`); the kit README's "the port's members move at most at the team speed" is stale.
2. **Container acceleration [H], new.** Both the container's acceleration and its deceleration are 0x5ab540 = the
   minimum over its members of the **member type's** `OverrideDeceleration` rule 0x373080(member type, container):
   0.05 x 1.56 = 0.078 and 0.07 x 1.56 = 0.109 per frame, measured 0.078 and 0.109 (start ramps and braking alike).
   The container's own acceleration tags play no part. FIGHTER_FLYING §7.7.
3. **The container waits for a straggler [H], new.** The container's speed step 0x5aaf70 caps its target speed with
   0x5ab640: a member more than HX + 2 behind the container along its facing slows it linearly to 0 at HX + 12 (HX =
   the squad's joint slot box half-length 0x404450). The run shows it at every Rows start with a member 17-21 behind
   the container (trial 1 leg 3, trial 3 legs 2 and 3: the container drops to 0.77-1.18 for 20-40 frames) and in
   Dartboard trial 2 leg 4 (a member 18-20 behind, container 1.18 rising to 1.55 over 85 frames as the member gains
   during the slowdown). Solving the cap for HX from each poll's speed and rearmost member gives a constant **15.0-15.4**
   for Rows (the 3 x 3 grid predicts (12 + 3) + `MaxJiggleDistance` 0 = 15.0) and **15.69-15.86** for that Dartboard
   layout. FIGHTER_FLYING §7.7.
4. **Start delays [H] for the rule, [M] for the two long ones.** On ordinary legs the members' 1.5-unit start
   crossings spread over 0..6 frames (Rows 4.1-6.0, Dartboard 0.9-6.0): the draw 0..`Max_Move_Frame_Delay` at a
   kind-1 path set while the container speed is 0 (§7.3 item 2). The binary has no longer delay. The two long ones are
   members that stand still because their new slot is far behind them: §7 step 4 gives v = vK x (1 - t^2), t =
   clamp(-along / 10, 0, 1), so **a member 10 or more ahead of its slot has v = 0** until the slot comes within 10.
   - Trial 3 leg 2: the whole 3 x 3 grid came out mirrored in the container frame (front row <-> back row, left <->
     right), i.e. laid with a direction about opposite to the travel direction (the projection order of §7.2
     reversed), and not re-laid during the leg. The old front row (members 1, 2, 7; slots now about 24 behind them)
     stood still in the world while the container passed them: their along error went 13.5 -> -11.4 with no world
     movement, and their crossing came 19-20 frames after the others. The other six members started together (no
     spread at all), so no start delays were drawn: the container's path at that order was not a kind-1 path set at
     speed 0 [M]. What gave the reversed layout direction (a first waypoint behind the container, or a non-kind-1 path
     whose first segment pointed back, §7.2 callers 1 / 4) cannot be read from positions [L]. The leg was disturbed
     in other ways too: container start +9 frames (other legs +3), stop 33 short, then a 13-unit sideways move at
     310 deg before the next order.
   - Trial 3 leg 3: the 30 deg re-lay gave member 3 a slot 19.6 behind it (it stood at along +9 of the new frame
     with its slot at -10.6); it stood still about 11 frames and crossed 17 frames late. Four members were still
     moving at that order (the leg 2 disturbance).
   So both are the step-4 stand-still after a re-lay, not a delay rule.
5. **Re-lay rule [H].** All 16 legs agree with 0x5ac120's dot <= 0.99 test against the last laid direction, taking
   the container's actual path heading (from the records) instead of the planned one: leg 2 (0 deg) keeps the slots
   in all four trials (Dartboard set change 1.85-2.55 = jiggle); leg 3 (30 deg) re-lays (Rows: members swap, same
   grid, per-member change 9.5-10; Dartboard: new scatter); leg 4 keeps them for Rows (path 0.4 and 5.0 deg from leg
   3) and re-lays both Dartboard squads (their leg 4 paths ran 10.7 and 9.4 deg from leg 3, both above 8.11 deg). The
   kit's "heading off the plan" for trial 2 leg 4 (0.7 deg) is the start-to-stop direction; the path itself ran at
   205.7 deg.
6. **Rows layout [H].** The settled slots fit the member-spacing grid (pitch 2 x 3 + 20 x 0.3 = 12, RMS 0.97-1.79)
   and not the container-spacing one (RMS about 16): g is the member type's `FormationSpacing` (§7.2). The settled
   grid sits 0.5-1.2 ahead of the container along the heading; within the +-1 along dead band of step 4 plus the
   jiggle (cause not traced) [L].
7. **Dartboard layout: no reading, the kit's fit cannot work [H for the reasons].** The code of 0x5ac2a0 / 0x241830
   was re-read (FIGHTER_FLYING §7.2 Dartboard, unchanged): seed (int32) container id (obj +0x50 = Lua
   `Get_Object_ID`) x NOW, two radical-inverse generators (bases 2 and 3, float accumulation) from that same index,
   x = (h2 - 0.5) x L, y = (h3 - 0.5) x Wd with L = (k - 1) x 20 g + k hx = 29, Wd = (m - 1) x 20 g + m hy = 16
   (n = 5: k = 3, m = 2; g = member 0's type `FormationSpacing` 0.5, hx = hy = 3.0), rejection below 5 + r = 9.24,
   1000 tries. The kit reproduces this, but:
   - **Wrong clock.** NOW is the global frame 0x140b0a320. Lua `GetCurrentTime()` (0x5428b0) returns the **mode**
     frame (current mode +0x10) x 1/30, not NOW. The `GetCurrentTime` object has a member `Frame` (vf +0x98 =
     0x5406b0) that returns (float)NOW / FPS: so NOW = `GetCurrentTime.Frame()` x 30 [H code; M for the Lua call
     form]. The run-wide offset search of +-60 frames assumed NOW - f is small; it is not measured.
   - **Noise floor at the random baseline.** The fit used the members' settled positions. Those are slot + jiggle +
     residual: `Squad_Rebel_Trooper` `FormationRaggedness` 1.6 with `MaxJiggleDistance` 3.5 keeps each member's
     target wandering over a +-3.5 box on each axis (the slot point 0x62c180 includes the jiggle), plus the +-1 along
     dead band and an uncorrected side residual. That alone gives an RMS near 3-4, the same as random scatters in a
     29 x 16 box (baseline 4.09-4.33): no seed can stand out.
   - **Bounds.** Slots are inside +-L/2 x +-Wd/2 by construction ((h - 0.5) in [-0.5, 0.5)). The two "outside" legs
     (|side| 12.9 and 13.9 against Wd/2 = 8) and the gaps below 9.24 are the jiggle (3.5 per axis) plus residual;
     the kit's tolerance (Wd/2 + jiggle + 0.5) is too tight. Not a finding.
   - **Re-randomised when re-laid, not per leg**: every call of 0x5ac2a0 is a new scatter (new NOW); see reading 5.
8. **Kit labelling bug (not a finding).** "After the stop: STAYS OPEN" for Rows comes from trial 3 leg 2, which has
   no settled slots (`-`): the verdict treats a leg without data as "never closed". Every leg with data closes at 0
   frames (all members within the recovery distance at once).

## What the kit should do on a rerun (Dartboard)
- Record NOW: `GetCurrentTime.Frame()` x 30 (rounded) at every poll and order, next to f.
- Remove the jiggle in the probe mod: override `Squad_Rebel_Trooper` (and its base, as `Variant_Of_Existing_Type`
  copies) with `FormationRaggedness` 0 and `MaxJiggleDistance` 0 in the mod's XML (the layout code does not read
  either), so settled positions are the slots within the +-1 along dead band. Without that, fit the side coordinate
  averaged over the steady march (the jiggle averages out; along carries a fixed per-member lag in Rebel squads).
- Seed (int32)(id x NOW) for NOW in [order NOW, order NOW + 10] (the container's first state-0 service after the path
  arrives lays the slots, §7.2 caller 1); the spawn layout at the container's first service after the spawn; a leg
  whose path bends more than 8.11 deg is laid again at that waypoint (caller 3) with that frame's NOW.
- Accumulate the radical inverse in float32, as 0x241830 does; assign by the retail order (points by x descending,
  members by projection, the group sort with the i = j + 1 skip, §7.2 step 6) and report the min-cost RMS as a check.
- A direct check, if the memory reader can find the member objects: pack (obj +0xa8) +0x114 / +0x118 is the slot,
  +0x12c / +0x130 the jiggle.

## Matches and mismatches with the port (working tree of 2026-11-10)
- **Match:** team speed (`team_caps`, MOVEMENT §3.4 exact); member vmax = the member's own effective max speed
  (`steer_member` uses `loco_max_speed(member)`), so the Rows catch-up and the Rebel no-catch-up both follow; the
  step-4 slow-down that makes a member ahead of its slot stand still; start delay 0..6 on a formation path at speed 0;
  the re-lay rule; the Rows grid with the member `FormationSpacing`.
- **Mismatch 1, container acceleration:** the port gives the container its own type's `OverrideAcceleration` /
  `OverrideDeceleration` (`MovementSystem::configure`); FoC containers have none the parser reads, so the port's
  squads reach full speed and stop in one frame. Retail: 0x5ab540 (§7.7 item 1), 0.078 / 0.109 per frame here.
- **Mismatch 2, straggler cap:** the port's container walks at the team speed whatever its members do. Retail caps
  it with 0x5ab640 (§7.7 item 2).
- **Not checked:** the Dartboard scatter itself (reading 7).
