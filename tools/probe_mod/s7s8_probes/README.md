# S7/S8 probe (lob solver, deploy windows, ability blocks, squadrons, escorts)

In-game probe for the open **probe** items left by roadmap S7 t23–t27 and S8 t10–t13. It needs one game session with
two skirmish battles: a land battle on Geonosis and a space battle on Kuat. Questions, numbered as in the request:

| # | Question | docs/re source |
|---|---|---|
| 1 | SPMAT grenade arc at ~275 (about 64°, lands in about 0.9 s). Range edge: a target 544–550 away is scanned but not fired at | LOB_SOLVER §9 (last bullet), §2.3, §4, §8 item 22 |
| 2 | Remote_Bomb at bare ground, interrupted before BOMBTOSS frame 12: does a `w_smallbomb` marker stay, or is ground targeting not offered at all? | LOB_SOLVER §3.5 ("MARKER removal"), §9 |
| 3 | Deploy window length for SPMAT (rate 2.0) and AT-AT, and after an undeploy reversal | TACTICAL_COMBAT "Gate details (S8 t11 / t12)", MOVEMENT §3.2 |
| 4 | Muzzle bones: do Moldy_Crow, Underworld_Infantry_Droid_Shuttle and the other S8 t12 types fire their WeaponBehavior in retail? | LOB_SOLVER §5.1, TACTICAL_COMBAT "Gate details" |
| 5 | `Activate_Ability` blocks: DEPLOY on an MDU, and an object-target ability whose target dies (return value and finish frame) | AI_TACTICAL §7.3.3, §12 |
| 6 | Squadron team target: do the members share it; does the member that scans with an empty target retarget them all? | FIGHTER_FLYING §4.10 B |
| 7 | Escort squadrons after their carrier dies: do they guard its last position? | FIGHTER_FLYING §4.10 E / F |
| 8 | (optional) First shot after a late spawn (no random initial delay); Dark_Trooper_PhaseIII deploy, jet pack, missiles silent until redeploy | TACTICAL_COMBAT "Gate details", ABILITIES_HARDPOINTS (jet-pack swap) |

The probe does not play the game for you. The shipped `BusyTacticalFreeStore.lua` gets `s7s8_hook.lua` appended.
In it, only the **Rebel AI** player's free store spawns the units and runs the fixed scenarios, one after the other.
On land it also gives **you** an Underworld Saboteur for item 2, which needs the GUI.

A read-only memory reader (`s7s8_reader.py`, built on `../a1_tactical/a1mem.py`) records every new object near the
test sites. For each object it records owner, flags, health, position and primary animation slot. Projectiles are
included, because they are in the object ID map (OBJECT_RUNTIME §1.1). For units it also records four inferred
fields:

- the locomotor state (obj +0xa8 → +0x48);
- the current target and targeting state (obj +0xf0 → +0x08 / +0x18);
- the container (obj +0x2b0);
- the squadron's team target (container +0x110 → +0x30).

The Lua side writes `S78P1|...|END` records: spawns, orders, what each `Activate_Ability` returned, and when each
block finished. The reader finds them with a heap scan. `s7s8_parse.py` turns both into `report.txt`, which ends with
an **ANSWERS** list.

## What Gabriel does (about 15 minutes in all)

1. Start Steam. Double-click `run_s7s8.cmd` in this folder. It builds `Mods\S7S8Probe` from your install, starts the
   reader, and launches FoC **windowed** with `MODPATH=Mods\S7S8Probe`. Keep the game windowed.
2. **Battle 1, LAND (about 6.5 minutes).** Go to **Single Player > Skirmish > LAND**, map **GEONOSIS**, and set the
   slots:
   - Slot 1 (you): **EMPIRE**, team 1.
   - Slot 2: **AI** (any difficulty), **REBEL**, team 2.
   - Every other slot: **closed**.
   - Credits, tech and settings: defaults.

   Start the battle.
3. Give your own army **no orders**. About 15 s in, the message *"S7S8 PROBE: your Saboteur is here"* appears and the
   camera jumps to an **Underworld Saboteur** next to your units. The probe's own units appear elsewhere on the map
   (around x 3480, y 4640, 1900 from the AI start). Leave them alone; you can watch them if you like.
4. Select **only the Saboteur** and do these three steps. His Remote Bomb throw reaches 5–130 units, about 3 tank
   lengths. The throw releases at clip frame 12, about 0.4 s after he starts the throw animation.
   - **H1 (far target, interrupted while walking):** click the Remote Bomb button, then click **bare ground** about a
     third of the screen away (well beyond 130, so he walks first). Right after that, right-click the ground on the
     **other** side so he walks away. Watch the target spot for 15 s. Note whether a small bomb marker stays there.
   - **H2 (near target, interrupted in the throw):** click Remote Bomb, then click bare ground just in front of him
     (2–3 body lengths). Right-click away **at once**: the throw must not finish. Watch the spot for 15 s and note
     what you see.
   - **H3 (near target, let him throw):** click Remote Bomb, then bare ground just in front of him, and let him throw.
     Wait 20 s. The ability recharges 18 s only after a bomb actually spawned, so H3 may need a short wait after H2.
   - If the button never accepts a click on the ground (only on units), note **"ground not offered"** and skip the
     rest.
5. Wait until about **6 minutes** after the battle started (message *"land scenarios done"*). Then press
   **Esc > Quit** to go back to the **main menu**. Do not close the game.
6. **Battle 2, SPACE (about 4.5 minutes).** Go to **Skirmish > SPACE**, map **KUAT**. Use the same slots: you
   EMPIRE team 1, AI REBEL team 2, others closed. Start. Give **no orders at all** and leave your fleet where it
   starts. Wait about **4.5 minutes** (message *"space scenarios done"*).
7. Press **Esc > Quit**, then quit the game. The `run_s7s8.cmd` window asks for your H1–H3 notes (one line), writes
   the report and removes the mod folder by itself.
8. Send back `out\<newest run>\`, `report.txt` first. The folder name is also written in `out\latest.txt`.

If the text file could not be built, no "S7S8 PROBE" messages appear: go by the clock (the scenarios start 15 s into
each battle). If anything went wrong, `cleanup_s7s8.cmd` removes the mod folder. That folder is the only thing the
probe installs; no retail file is touched.

`run_s7s8.cmd --no-launch` installs the mod and starts the reader without launching the game. Start
`StarWarsG.exe WINDOWED MODPATH=Mods\S7S8Probe` yourself; Claude can do this with computer use, see the memory note
"Driving the game with computer use". `--non-interactive` never waits for a key.

## Run plan (frames are mode frames, 30 per second; each scenario despawns its units at its end)

**Land, Geonosis, test site (3480, 4640), facing −x.** All scenarios run at the same flat strip:

| Scenario | Frames | What happens | Items |
|---|---|---|---|
| SPMAT | 450–2850 | SPMAT at the site, facing down the strip. Held tank at 275. DEPLOY at +45. At +585 the tank is replaced by one at 547 (edge). At +1185 a tank at 530 is added (control). Undeploy at +1635. Deploy +1875, undeploy +1887, deploy +1899 (two reversals 12 frames apart) | 1, 3, 5 |
| ATAT | 2853–4353 | AT-AT attacks a held tank at 250. DEPLOY true / false / true / false / true at +330 / +780 / +1080 / +1092 / +1104. A nil DEPLOY(true) falls back to DEPLOY_TROOPERS | 3 |
| MDU | 4356–5556 | Rebel MDU: DEPLOY at +30. DEPLOY again at +300 (deployed). Undeploy at +330. Move 450 at +600. DEPLOY while moving at +630. New move at +930 | 5 |
| STICKY | 5559–7059 | Two Infiltrators (autofire off). STICKY_BOMB at a live tank 120 away (+30, control). STICKY_BOMB at a tank 450 away (+480), killed 60 frames later. Move order at +1200 | 5 |
| DT3 | 7062–9162 | Dark_Trooper_PhaseIII attacks a held tank at 200. DEPLOY at +300. JET_PACK to (−120, 60) at +750. Attack again at +1200. DEPLOY at +1650 | 8 |
| REINF | 9165–9945 | Four MZ8 pulse tanks spawned 150 frames apart (+30, +180, +330, +480), each ordered at once to attack a held tank at 200 | 8 |
| MUZZLE_LAND | 9948–10608 | The 8 S8 t12 types in two groups of four, 220 apart, each with a held tank 150 to the side, each ordered to attack it | 4 |
| HUMAN | from 450 to the end | Your Saboteur (H1–H3). The reader watches every `DUMMY_BARRAGE_TARGET`, `PROJ_REMOTE_BOMB` and the Saboteur's BOMBTOSS clip anywhere on the map | 2 |

**Space, Kuat.** Three points: `sq` (−1106, 1351), `esc` (1449, −1186) and `muz` (172, 82).

| Scenario | Frames | What happens | Items |
|---|---|---|---|
| MUZZLE_SPACE | 450–1110 | The same 8 types, 600 apart, each with a held Tartan 350 to the side | 4 |
| SQUAD | 1113–3213 | Rebel X-Wing squadron. Short move. Three held Tartans at +300. At +1200 and +1800 the Tartan the squadron is on is despawned | 6 |
| ESCORT | 3216–about 7700 | Star_Destroyer. When its squadrons are out (or after 900 frames) it moves 900, and 600 frames later it is killed. 1200 frames after that a held Tartan appears 700 from the escorts' centroid; it is removed 900 frames later | 7 |

Victims are held in place, never fire and are invulnerable, except the STICKY ones and the squadron's targets, which
are despawned. They are owned by the map's Hostile player when there is one.

## Expected answers (what `report.txt` should say if docs/re is right)

The **ANSWERS** list at the end of the report uses these keys.

- **`1_arc` = `MATCHES_LOB_SLOWER`.** Expected about 64° (63.9° with the +6 target z adjust, 64.3° with none), about
  26 frames (0.87 s) of flight, apex about 142 and launch speed 24 (30 × 0.8, one `Can_Lob_Slower` step).
  `MATCHES_V30_NO_SLOWER` (about 75°, 35 frames) would mean the slow-down loop is not used. The parser fits each
  grenade's measured path, z quadratic and horizontal linear. It compares that with the solver run on the SPMAT and
  tank positions in memory, with a tolerance of 4° and 3 frames.
- **`1_edge` = `SCANNED_NOT_FIRED`.** The tank at 547 is beyond the 544 reach (slot 44, v = 30, target z + 6) but
  inside the 550 scan cube. So the SPMAT's current target becomes that tank, the targeting state never reaches 4 and
  no grenade is launched. The control at 530 must fire, otherwise the edge result says little. `NOT_SCANNED` means
  the scan cube is smaller than thought. `FIRED` means the reach is larger.
- **`2_marker` = `MARKER_USED_AND_PERSISTS_AFTER_INTERRUPT`** if the GUI issues command 57. That puts a
  `Dummy_Barrage_Target` marker (`w_smallbomb.alo`) at the spot. The marker has no lifetime: only the toss removes it,
  so after H1 / H2 it stays, and after H3 it goes when the bomb is thrown (`REMOVED_AT_THE_THROW`). `NO_MARKER` with
  your note "ground not offered" means ground targeting is not offered, the other branch of the LOB_SOLVER §9 item.
- **`3_spmat_window`.** The measured number of frames from the DEPLOY order to locomotor state 0x19. The report also
  shows the order → 0x18 delay (turret recentre wait) and the 0x18 length, to set against
  ceil(max(count − 2, 0) / (clip rate × 2.0)). No grenade is expected while the state is 0x18 or 0x1a.
- **`3_spmat_reversal` = `MIRRORS`.** Deploy(false) from 0x18 starts the undeploy clip at len − cur − 1, and the
  reverse. The parser checks the measured lengths against undeploy ≈ e + 1, or deploy3 ≈ L_undeploy − e + e2
  (e, e2 = frames spent in the interrupted states).
- **`3_atat` = `NO_DEPLOY_STATE (DEPLOY ret=nil)`.** AT_AT_Walker has `Deploys = Yes` but only a DEPLOY_TROOPERS
  ability, so Lua cannot start a deploy. The laser counts per phase show whether the AT-AT ever stops firing.
- **`4_muzzle_land`: every type `SILENT` or `NOT_SPAWNED`.** All 8 types have WEAPON only in SpaceBehavior; the land
  half is the control. **`4_muzzle_space`: `FIRES`** for every type that spawns in space (their space models have
  MuzzleA_00 / 01). `SILENT` there for a spawned type with a target would be the surprise to report.
- **`5_deploy_block` = `FINISHES_AT_DEPLOYED`.** The block finishes on signal 0x29, at the 0x19 frame (±4).
  **`5_deploy_again` = `NIL`**: a second DEPLOY true while deployed returns nil.
  **`5_deploy_moving`** is open ([M], the "moving" gate vf +0x2b0). Possible values:
  - `NO_BLOCK`: refused while moving;
  - `FINISHED_AT_DEPLOYED`: deploys anyway;
  - `FINISHED_BY_MOVE_ORDER`: pending until the move order (signal 0xb).
- **`5_target_dies`** is open ([M], is the pending entry cleared on target death?). Possible values:
  - `FINISHES_ON_TARGET_DEATH`: cleared;
  - `FINISHES_ON_NEW_ORDER`: no timeout, ends at the +1200 move order (signal 0xb);
  - `NEVER_FINISHED`;
  - `NO_BLOCK`: the pending ability was not stored.

  The `sb_live` control should finish shortly after its bomb.
- **`6_squad` = `ALL_AT_ONCE`.** While a target lives, every member is on it, and the team target (container +0x110 →
  +0x30) equals it. After a drop, the member that scans with an empty target retargets the whole squadron: every member
  gets the same new target in the same frame. `INDIVIDUAL` / `SAME_TARGET_STAGGERED` would contradict 0x406370.
- **`7_escort` = `GUARDS_LAST_POSITION`.** The escort block becomes a position block (the last waypoint) with the guard
  flags kept, so the escorts stay near the death point (centroid within 600). They engage the intruder 700 away
  (inside `Guard_Chase_Range` 1000) and come back. `_IGNORES_INTRUDER` / `_BUT_DID_NOT_RETURN` refine it.
  `RETURNED_TO_SPAWN_POINT` / `LEFT_THE_POSITION` contradict it.
- **`8_first_shot` = `NO_RANDOM_DELAY`.** A late spawn's first service clears the `Get_Int(0, FPS × recharge)`
  initial delay, so the four tanks fire after the same short aim time. A spread above 45 frames (up to 159 for
  recharge 5.3 s) means the delay survives.
- **`8_dt3` = `MISSILES_SILENT_UNTIL_REDEPLOY`.** Phases: lasers only while undeployed, missiles only while deployed,
  no missiles after the jet pack (the swap undeploys), missiles again after the second DEPLOY.

## s78g run (2026-11-05: roadmap group s78g, tasks t01, t02, t03, t05 and a t04 control)

One land and one space battle, same set-up as above (you EMPIRE team 1, AI REBEL team 2, Geonosis / Kuat). Pick the
scenarios with these values (set them before `run_s7s8.cmd`, which builds the mod):

```
set ALAMO_S78_LAND=SPMAT_EDGE2,SPMAT_CTRL,REINF,REINF2,IDLE47W
set ALAMO_S78_SPACE=GUARD_HAND,ESCORT
run_s7s8.cmd --no-launch --non-interactive
```

**Why t05 failed in s78f t12.** The hook was fine: ESCORT launched 1 squadron + 7 fighters, killed the carrier at
F 6486 and placed the intruder, and REINF's four MZ8s fired. The parser was wrong. The heap scan returns the Lua
records in address order, and `scenarios_of` gave each start record the next end record in that order. ESCORT got
SQUAD's end and REINF got IDLE47V's end (windows F 5856..5853 and 8406..8403), so both windows were empty. Now the
records are sorted by frame and each start is paired with the end of the same scenario. The parser selftest also runs
a shuffled log. Re-parsing `out/20261105-062322` with the fix gives:

- REINF: delays 17 / 30 / 41 / 52, with the first shots 162 / 162 / 161 frames apart (spawns 150 apart). That gives
  `FIRST_SHOT_ON_SHARED_CLOCK (P=161)`: no per-unit random delay, but a shared phase.
- ESCORT: 18 fighters, `LEFT_THE_POSITION`. The escorts were the AI's, and they flew 4000 away toward the AI's own
  start.

**Land, about 7.5 minutes** (frames from the battle start; the Lua clock starts at 450):

| Scenario | Frames | What happens | Task |
|---|---|---|---|
| SPMAT_EDGE2 | 450–4500 | 9 windows of 450 frames. Each window spawns a fresh SPMAT, faces it down the strip, attacks a T4B at 275 (control) and deploys it (+21). At +150 the 275 tank is replaced by a T4B at d = 500, 530, 540, 544, 546, 548, 550, 553, 560 in front of the walker, with an explicit `Attack_Target`. No `Suspend_Locomotor` (in SPMAT_EDGE it sent the walker to 0x1a and froze it there) | t01 |
| SPMAT_CTRL | 4503–5703 | Undeployed SPMAT, no DEPLOY call, attacks a T4B at 275: window 1 with a Hostile-owned tank, window 2 (+600) with an Empire-owned one, at the same offsets | t04 control |
| REINF | 5706–6486 | As before (4 MZ8s, 150 apart) | t05 |
| REINF2 | 6489–7869 | 6 MZ8s at the irregular gaps 200 / 170 / 250 / 140 / 290, each attacking the held victim at once | t05 |
| IDLE47W | 7872–13272 | Toggle as IDLE47V with three phases of 1800 frames: P = 6 Pod_Walkers (no ATTACKIDLE), D = 6 `Dark_Trooper_PhaseII`, V = victims only. D replaces the Rebel_Infiltrator, which stayed in state 0 for 3480 of 3600 attacker-frames in t12. Dark_Trooper_PhaseII is a single GroundVehicle with its own TARGETING + WEAPON, no TURRET, no Deploys and range 180 (the victims are 160 away). Its model EI_DARKTROOPER_TWO has ATTACKIDLE_00 / _01 and ATTACK clips. JET_PACK autofire is off | t03 |

The land battle also spawns the usual Saboteur for you. Ignore it: no H1–H3 this time. Give no orders at all.
*"land scenarios done"* appears at about 7 min 25 s. Then press **Esc > Quit** to go to the main menu.

**Space, about 5.3 minutes.** You give 4 orders by hand in the first 2.5 minutes:

| Battle time | Message (TEXT_S78_*) | What you do |
|---|---|---|
| 0:15 (F 450) | GUARD_FIND | **Left-click the centre of the minimap.** The camera starts at your fleet in a corner, so the probe units are at the map centre (`muz`, (172, 82)), far from both fleets and the map edge. There is a 7-fighter `TIE_Fighter_Squadron` at the centre and a `Broadside_Class_Cruiser` (G) about 400 away, starting to move. **Box-select only the TIE squadron.** G shuttles along the line through the centre at right angles to the line between the two start corners, ±1500 from the centre, at about 2 units / frame. If you lose it, click the minimap centre again |
| 0:30 (F 900) | GUARD_GO | Squadron selected: **right-click G** (it is moving) |
| 1:00 (F 1800) | SHIFT_GO | Squadron still selected: **hold Shift and right-click empty space** about a third of a screen away from G |
| 1:30 (F 2700) | GUARD_GO2 | Select the squadron again and **right-click G** |
| 1:55 (F 3450) | SHIFT_GO2 | **Shift + right-click** empty space away from G |
| 2:25 (F 4350) | G_STOP | G stops. Give no more orders |

Then ESCORT runs on its own (F 4953–9453; the Star_Destroyer is at `esc` (1449, −1186)). Changes since t12: the
escorts get `Prevent_AI_Usage` as they launch (the t12 escorts were taken by the AI). The intruder is placed 700 from
the death point, not from the escort centroid. Escort centroid COUNT records come every 90 frames.
*"space scenarios done"* appears at about 5 min 15 s. Then **Esc > Quit** and quit the game.

Timing from computer use: each order only has to land inside its window. The parser finds the reaction from the
squadron's motion, not from the message frame. A late guard still counts if it comes before the matching SHIFT
message.

**What report.txt says.** The scenario blocks come first, then these keys in the ANSWERS list:

- `1_edge2` (t01): `EDGE fired d=X (measured), not fired d=Y (measured, <why>)`, or `ALL_FIRED (reach >= 560)`, or
  `NONE_FIRED (aim@275 control fired in k of 9 windows)`. The *why* is one of:
  - `TARGETED_NOT_FIRED[_THEN_DROPPED]`: the item 1 prediction "scanned, not fired";
  - `NOT_TARGETED`;
  - `UNDEPLOYED_OR_MOVED`;
  - `NOT_DEPLOYED_AT_ORDER`.

  The `-- SPMAT_EDGE2` block has one line per window: requested d, measured distance, control grenades at 275, test
  grenades and the delay to the first one, frame-weighted target / targeting-state / locomotor histograms, and how far
  the walker moved. Expected from LOB_SOLVER §4: fired up to 544, then `TARGETED_NOT_FIRED` from 546 to 550.
- `t04_victim_owner`: `Hostile:FIRED Empire:FIRED` if the owner of the victim does not matter (TACTICAL_COMBAT
  §2.10, s78g t04). Each window line has the grenades, the frame of state 4 and the target histogram.
- `8_first_shot` (REINF) and `8_first_shot2` (REINF2), one of:
  - `FIRST_SHOT_ON_SHARED_CLOCK (P=…, delays a..b)`: every gap between first shots is a multiple of one period
    155..165 ±4, and the spawn gaps are not;
  - `NO_RANDOM_DELAY`;
  - `RANDOM_DELAY_LIKELY`;
  - `UNCLEAR`.

  The block lists each tank's spawn, its state-4 frame and its first shot, then the first-shot frames, the gaps
  between them and the spawn gaps.
- `t03_slot47_clip`: `WITH_CLIP_NO_PER_SERVICE_DRAW (on step P +x, D +y, V z; D clip 6 in n%)` confirms the docs:
  with the clip, slot 47 draws only when it enters the pose. The other values are:
  - `WITH_CLIP_ALSO_DRAWS`;
  - `D_DID_NOT_TAKE_TARGETS`;
  - `UNCLEAR`;
  - `NOT_MEASURED`.

  The `-- IDLE47W` block has one line per phase (P / D / V) with the on and off steps of the synced draws per frame
  and their medians, the mean draws per frame in the on and off windows, and frame-weighted histograms of the
  attackers' targeting state and clip while the victims exist. 6 = Attack Idle, 5 = Attack.
- `t02_guard_shift_1` and `t02_guard_shift_2` (one per guard + shift pair):
  - `WAYPOINT_TAKEN_WHILE_G_MOVES (+n after the shift message)`;
  - `WAYPOINT_WAITED_UNTIL_G_STOPPED (left +n after the stop)`: what B 3 predicts for pair 2;
  - `NO_DEPARTURE`;
  - `NO_GUARD_SEEN`.

  Any of them can carry a `NOT_FOLLOWING_AT_SHIFT (dist d)` suffix: the squadron was not with G when the waypoint was
  given (the t12 "catch up, then stop following" pattern). The `-- GUARD_HAND` block has a line every 30 frames: G
  position and speed, squadron centroid and speed, distance, a class (WITH_G / CHASE / AWAY / HOLD / DRIFT) and the
  members' targets. Then one summary line per pair: when the squadron reacted, when it came within 500, the follow
  share while G moved, and the departure frame. When memory misses the units, positions fall back to Lua (COUNT
  `gh`).
- `7_escort` (t05): `GUARDS_LAST_POSITION[...]`, `RETURNED_TO_SPAWN_POINT` or `LEFT_THE_POSITION`. The block also
  says how many escorts Lua held back from the AI, the Lua centroid distances, and the escorts' targets after the death
  (type / owner).

Parser fixes in this pass:
- The targeting-state and clip histograms (`s78f_extra_parse.py` `ts_hist` / `anim_hist`, and the new analysers) are
  frame-weighted. The reader writes ST only on change, so counting samples undercounted steady states. That is part
  of the wrong s78f "AI never targets Hostile-owned units" reading: re-parsed, the t11 Pod_Walkers show state 4 for
  3060 of 3060 attacker-frames.
- `IDLE47V` logs now also go through the IDLE47W analyser.

### s78g rerun (after `out/20261105-073814`)

```
set ALAMO_S78_LAND=SPMAT_EDGE3,IDLE47X
set ALAMO_S78_SPACE=GUARD_HAND
run_s7s8.cmd --no-launch --non-interactive
```

What the 073814 run showed, and what changed:
- **GUARD_HAND hit an ERR**: "attempt to index global `math`". The retail free-store Lua has **no math library**, so
  G never moved and the shift waypoint was never taken. The hook now uses arithmetic only: the math calls in
  MAGNEPULSE's ability retry, GUARD_HAND's leg side and its arrival-distance check are gone. The selftest driver sets `math = nil` before loading the
  script, so any new math call fails the selftest. The timing is unchanged.
- **SPMAT_EDGE2 is invalid.** The explicit `Attack_Target` on the edge tank made the deployed walker undeploy and
  drive about 209 units in. The analyser now calls such windows `FIRED_AFTER_MOVING` / `UNDEPLOYED_OR_MOVED` and keeps
  them out of the edge, so 073814 parses as `INVALID`. **SPMAT_EDGE3** deploys the same way (attack the 275 tank, DEPLOY at +21). At +150 it
  swaps that tank for one at d **with no order**, so the walker has to acquire it by itself. Ten windows of 450,
  d = 450 (in-range control), 500, 530, 540, 544, 546, 548, 550, 553, 560. Answer `1_edge3`: same verdicts as
  `1_edge2`, only from windows where the walker held 0x19 and moved ≤ 15. The block lists which d fired and which held
  deploy without firing.
- **IDLE47W was noise-bound.** The land background is about 440 synced draws per frame and drifts by ±100 over 150
  frames, which is larger than the expected effect. **IDLE47X** uses 12 attackers (75 apart from along −200, victims
  160 to the side) and 15 cycles of 200 frames per phase: 100 with victims, 100 without. The W attackers reached state
  4 within 20 frames. The parser's statistic is per cycle: the mean draws per frame with victims, minus the mean of the
  victim-free windows on either side, which cancels a linear drift. Each phase gets mean ± stderr. A verdict needs
  3-sigma separation of P−V, D−V and P−D; otherwise it is `UNCLEAR (P-V a +- s, D-V …, P-D …)`. Answer
  `t03_slot47_clip_x`. Re-parsed with this statistic, 073814 IDLE47W gives P +1.9 ± 31.8, D +13.5 ± 25.6, V −48.6 ±
  39.9, so `UNCLEAR`. Clips are printed by name (docs/re/ANIM_PARTICLES.md): 5 = ATTACK, 6 = ATTACKIDLE. In 073814
  the Dark Troopers spent 2730 of 3750 attacker-frames in ATTACK (firing) and 1020 in ATTACKIDLE.
  The Pod_Walkers were in 0 IDLE throughout.
- **REINF verdict.** New `PER_UNIT_STAGGER (slope S frames per earlier unit, delays a..b, max residual r)` when the
  first-shot delays do not decrease with spawn index, the least-squares slope is above 5 and no delay is more than
  12 from the line. 073814 REINF2 gives slope 11.3, delays 9..61, residual 5.9. REINF (re-parsed) gives slope 11.4,
  residual 0.3; its first shots also fit the 161 clock, which the verdict notes.
- **ESCORT target.** All 18 escorts targeted obj 67593 after the carrier died. 67593 has no OBJ line in the space
  battle: it existed before the probe started (a starting-fleet unit or map object), and the reader only described new
  objects. The reader now writes one `TGT n= frame= obj= type= owner= x= y= z= watched= initial=` line the first time
  a watched unit takes a target id. The escort target summary uses it, e.g. `obj67593=<type>/owner<n> (pre-existing)`.

**Land, about 8 minutes**, no orders:

| Scenario | Frames | Task |
|---|---|---|
| SPMAT_EDGE3 | 450–4950 | t01 |
| IDLE47X | 4953–14553 (P 4953, D 8153, V 11353) | t03 |

*"land scenarios done"* appears at about 8 min 5 s (F 14556).

**Space, about 2 min 45 s.** GUARD_HAND only, with the same hand orders and messages as above: F 450 / 900 / 1800 /
2700 / 3450 / 4350 (0:15 / 0:30 / 1:00 / 1:30 / 1:55 / 2:25). It ends at F 4950.

## s78h run (2026-11-05: roadmap group s78h, t01 and t02 prep, t07)

What the s78g rerun `out/20261105-092438` showed, and what changed:
- **t07, kit departure verdict.** Both GUARD_HAND pairs read `NO_DEPARTURE`, yet the trace shows the squadron leaving
  for the waypoint at about 5.5 / frame each time. The s78g rule needed two `AWAY` rows (moving away from G and more
  than 600 from it); here the squadron flew *past* G on its way out, so those rows were `WITH_G` / `CHASE` / `DRIFT`.
  The waypoint is in no record (the click is on empty space), so the parser now takes it as the **settle point**: the
  first place after the shift message where the squadron, after a leg faster than 3 / frame, stays below 1.5 / frame
  for three rows (90 frames), away from G (more than 300 from it, or G pulling away). The departure is the first row
  of that leg faster than **3 units / frame whose motion has a positive component along the direction to the
  waypoint**. When no settle point exists, the old `AWAY` rule is the fallback. The pair line now prints the
  waypoint, its frame and which rule found the departure. Re-parsing 092438 with this gives:
  - `t02_guard_shift_1 = WAYPOINT_TAKEN_WHILE_G_MOVES (+150 after the shift message)`. Waypoint (−399, 148) at
    F 2040, departure F 1950, G moving at 2.2 / frame.
  - `t02_guard_shift_2 = WAYPOINT_TAKEN_WHILE_G_MOVES (+120 after the shift message) NOT_FOLLOWING_AT_SHIFT (dist
    836)`. Waypoint (−853, −161) at F 3840, departure F 3570, G stopped only at F 4350.

  So in both pairs the waypoint was taken while G still moved: no wait for G to stop. Pair 2 is weaker, because the
  squadron was not with G when the shift order came. The selftest has a synthetic trace of the 092438 shape (section 3).
- **t01, the SPMAT edge by auto-acquire.** In SPMAT_EDGE3 the deployed walker acquired nothing, not even the in-range
  control at 450: every window was `NOT_TARGETED`, ts 0. The likely reason is vision: a deployed AI unit without an
  order may not acquire a target outside the AI's vision. **SPMAT_EDGE4** is EDGE3 with vision at the edge tank:
  - an AI-owned spotter, a `Rebel_Infiltrator` (invulnerable, `Prevent_All_Fire`, locomotor suspended). It stands
    100 to the side of the tank spot and is spawned with the 275 aim tank, so it already sees the spot when the edge
    tank appears;
  - `FogOfWar.Reveal(PlayerObject, tank, 250, 250)` 2 frames after the tank appears, undone (`Undo_Reveal`) at the
    next window. The shipped AI script `BurnUnitsLand.lua` uses the same FogOfWar API.

  The rest is unchanged: there is **no order at the edge tank**, the victims are the human's (never Hostile), and
  windows are 450 frames. Distances: 12 windows in two passes, 450 (control), 540, 544, 547, 550, 556, then 450, 556,
  550, 547, 544, 540. Only one edge tank exists at a time. Answer **`1_edge4`**:
  - `CONTROL_NOT_FIRED (...)`: the 450 control never fired, so vision or acquisition is still the limit;
  - otherwise the EDGE3 verdict plus `per d 540=F/F 544=T/T ...`. Per pass, F = fired, T = targeted not fired,
    N = not targeted, x = invalid window (moved or undeployed).

  The block adds one line per distance (measured distance, verdict and grenades per pass) and how many spotters and
  reveals worked. Expected from LOB_SOLVER §4: F up to 540 and T from 544 on. If the reach is above 544, the edge lies
  between the last F and the first T.
- **t02, slot 47 on a quiet background.** The IDLE47X background was about 440 synced draws per frame, ±100, mostly
  from the two armies. **IDLE47Q** runs in four stages:
  1. It measures the background for 300 frames (`bg_pre`).
  2. At +300 it sweeps both armies. It despawns every AI-owned object that answers `Is_Category` Infantry, Vehicle,
     Air or LandHero. Structures and everything else stay, and so does the AI unit farthest from the site, so the AI
     is never left without units. The human's mobile units go too, all but the Saboteur
     (`set ALAMO_S78_QUIET_HUMAN=0` keeps them). The probe's own objects are never touched. The sweep repeats every
     300 frames, to catch reinforcements and new builds (COUNT `quiet` records).
  3. It measures the background again (`bg_post`, +450 to +900).
  4. It runs the IDLE47X phases unchanged: P (Pod_Walker), D (Dark_Trooper_PhaseII), V (victims only), 12 attackers
     each, 15 cycles of 200 frames.

  Answer **`t03_slot47_clip_q`**: the IDLE47X verdict (same 3-sigma rule) plus `[background a -> b draws/frame]`. The
  block also has the sweep counts. Read the background numbers first: if b is not well below a, the sweep missed the
  main draw source.

  **Per-object reading of the slot-47 draw was considered and set aside.** The reader is read-only and samples the
  synced LCG state once per poll, so a draw can be counted but not tied to the object that made it. Tying draws to
  objects would need one of two things:
  - a hook on the slot-47 service, which is not read-only;
  - a per-object field that the slot-47 draw writes, such as the ATTACKIDLE_00 / _01 variant chosen on pose entry or
    a randomised idle countdown. The reader would then log it for each attacker.

  No such field is mapped in docs/re yet, and the primary anim slot the reader already logs (clip type : mode :
  position : speed) does not show the variant index. The quiet-background route needs no new offsets.

**Kit command.** One land battle, no space battle. Run it from PowerShell in the main checkout (or a worktree with
`toolchain` and game data junctioned). Run it in the background: it waits for the game to exit.

```
$env:ALAMO_S78_LAND = 'SPMAT_EDGE4,IDLE47Q'; $env:ALAMO_S78_SPACE = 'GUARD_HAND'
& tools\probe_mod\s7s8_probes\run_s7s8.cmd --no-launch --non-interactive
```

From cmd use `set ALAMO_S78_LAND=SPMAT_EDGE4,IDLE47Q` and `set ALAMO_S78_SPACE=GUARD_HAND`, then
`run_s7s8.cmd --no-launch --non-interactive`. The space list only matters if a space battle is started; none is
needed this time.

**In-game steps (computer use).**
1. Wait until `out\<run>\reader.log` exists. Start `StarWarsG.exe WINDOWED MODPATH=Mods\S7S8Probe` in
   `F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption` (Steam running). Check that reader.log shows
   `GAME ... identity=ok`.
2. Double-click the title bar to maximise the window. The menu coordinates below are for the 1456x819 frame.
3. **Single Player > Skirmish > LAND**, map **GEONOSIS**. Slot 1 (you): **EMPIRE**, team 1. Slot 2: **AI** (any
   difficulty), **REBEL**, team 2. Every other slot: **closed**. Credits, tech and settings: defaults.
4. Click **Start Game** (about (1193, 587)), then **Begin** (about (970, 726)).
5. Give **no orders at all**. The Saboteur spawned for you is not used, and neither are H1–H3. At about 3 min 25 s
   your own starting army disappears: that is the IDLE47Q sweep, not a defeat. Your base structures and the
   Saboteur stay.
6. Timeline (battle time = frame / 30):
   - SPMAT_EDGE4: F 450–5850 (0:15–3:15);
   - IDLE47Q: F 5853–16353 (3:15–9:05). The sweep is at F 6153 (3:25) and phase P starts at F 6753 (3:45).
7. *"land scenarios done"* appears at about **9 min 05 s** (F 16356). Then press **Esc > Quit** (about (728, 485)),
   **Yes** (about (610, 440)) and **Exit** (about (1047, 694)).
8. The kit writes `out\<run>\report.txt` and removes the mod. The run folder is in `out\latest.txt`. Read the ANSWERS
   keys `1_edge4` and `t03_slot47_clip_q`, and the `-- SPMAT_EDGE4` / `-- IDLE47Q` blocks.

The sweep keeps one AI unit and your Saboteur so that neither side is left without units. If the battle still ends
early (a victory or defeat screen after the sweep), the IDLE47Q block shows how far it got. Rerun with
`set ALAMO_S78_QUIET_HUMAN=0`, which leaves your army alone.

## Feeding results back

Edit only `docs/re` (RE/doc task). Quote the run folder and the report line. Then:

- items 1 / 2: LOB_SOLVER §9. Remove or answer the two **probe** bullets and adjust §3.5 / §8 item 22 if the result
  differs.
- items 3 / 4 / 8: TACTICAL_COMBAT "Gate details (S8 t11 / t12)" (deploy window, muzzle types, first-shot delay) and
  MOVEMENT §3.2 (deploy states, reversal).
- DT3: ABILITIES_HARDPOINTS (jet-pack swap undeploys).
- item 5: AI_TACTICAL §7.3.3 (the two [M] tags) and §12.
- items 6 / 7: FIGHTER_FLYING §4.10 B and E (and F, which is derived from them).

Then tick the matching roadmap tasks (S7 / S8 follow-ups) with `roadmap_store.py write`.

## Files

| File | What |
|---|---|
| `run_s7s8.cmd` / `s7s8_run.py` | One session: build + install, reader, launch, wait, report, cleanup (always). Exit 0 report, 10 no armed run, 20 preflight, 30 interrupted, 31 error |
| `cleanup_s7s8.cmd` / `s7s8_cleanup.py` | Deletes `Mods\S7S8Probe` only if it holds `S7S8_PROBE_MARKER.txt` (`--dry-run`, `--no-pause`) |
| `build_s7s8_mod.py` | Builds the mod from the install: hooked free-store script + `S78_CFG` (sites from the .ted maps), the text file with the `TEXT_S78_*` messages, the marker. `--map-info` prints the sites; `--rank-land-maps` scores every MP land map |
| `s7s8_hook.lua` | The scenarios (Lua 5.0, everything pcall'd) |
| `s7s8_reader.py` | Read-only memory recorder (`--selftest`) |
| `s7s8_parse.py` | Report and answers (`parse <log>`, `--selftest` on a synthetic log built from the expected model) |
| `selftest/s7s8_selftest.py` | Runs the hooked script under stock Lua 5.0.2 against a stub engine (land, space, other faction; the s78g and s78h lists), parses those records, parses synthetic memory traces for the s78h GUARD_HAND departure and SPMAT_EDGE4 per-distance verdicts, runs both `--selftest`s. `selftest/build_lua50.cmd` builds `out\tools\lua50.exe` |

Run the selftest: `toolchain\venv\Scripts\python.exe -I tools\probe_mod\s7s8_probes\selftest\s7s8_selftest.py`.
It ends with `S7S8 SELFTEST OK`.

## Limits

- **Item 2 is manual.** The Remote_Bomb ground order (command 57) is a GUI path; Lua's `Activate_Ability` cannot issue
  it. The answer depends on you doing H1–H3. Without markers the parser cannot tell "not offered" from "not done", so
  your note matters.
- **AT-AT has no DEPLOY ability.** Its deploy window cannot be driven from Lua; the ATAT scenario records the nil
  return and the firing per phase only.
- **The extra memory fields are inferred** from docs/re offsets: locomotor state, current target / targeting state,
  container and team target. Any wrong one shows as `-` and makes the matching verdict `UNREADABLE` / `NOT_MEASURED`.
  The Lua answers (block returns and finish frames) and the projectile counts do not depend on them. The `TEAMDATA`
  line says whether the team data resolved as a pointer or inline.
- **Kuat has no parsed playable box.** The space points come from the start positions only, 1800 to each side of the
  start line.
- **The land muzzle half is a control:** those types have no WEAPON in land.
- **Lua frames are quantized to 3 frames** (free-store service). ACT / BLK / MARK frames are ±2 frames; memory frames
  are exact. The parser applies a Lua → memory frame offset only when the spawn ids show 3 or more frames.
- **Projectile attribution** is by type and by the first-seen position next to the shooter (120 land, 450 space).
- **The squadron's scanning member is not visible.** The report shows the first member to switch, and whether all
  members switch in one frame and the team target changes with them.
- **"Reinforcements" are `Spawn_Unit` spawns,** not a reinforcement drop. The doc rule (first service frame beyond
  the drawn delay) covers both.
- **The escort answer needs the Star_Destroyer to launch its starting squadrons.** If none launch within 900 frames,
  ESCORT reports `NOT_MEASURED`.
