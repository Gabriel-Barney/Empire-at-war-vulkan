# cu8k t04: coordinator ctor draw traces (MOVEMENT §4.7.6), retail 2026-11-09

Kit: `tools/probe_mod/cu8h_probes/cu8h_dbg.py` (hardware write watchpoint on the synced LCG state), run directly with
`--arm-file` / `--stop-file` so the session could be driven by computer use (the `run_cu8h_trace.cmd` Enter prompt
needs a console). Vanilla game, no mod. Driven by Claude during the 2026-11-08 overnight probe session. Listing method:
the one-liner of MOVEMENT §4.7.6 (hits whose stack continues `40d11f,4f6fd4,`).

## Item 2: skirmish land load, `_MP_LAND_ENDOR` ((6) Endor Xpress)

Run `tools/probe_mod/cu8h_probes/out/20261109-001746-trace-cu8k-endor-land` (`trace.log`, `report.txt`). You EMPIRE,
Easy AI REBEL, defaults; armed just before Start Game, stopped at mode frame 120. 45977 hits.
`cu8h_dbg_parse.py` verdict: `VERDICT t01: A B C` (chunk-0x102 draws n503..n3360, one environment pick `int(0, 3)` = 0,
425 post-load SKIRMISH_FORCES draws, no weather).

Coordinator ctor draws:

| count | first mf | creator chain after the factory | reading |
|---|---|---|---|
| 1 | 0 (gf 0, n 3362, right after the environment pick) | `4f738a` degenerate ← `43b502` ← vf8 return `39bda0` ← **`395421`** ← `2b44a7` | one degenerate draw through an adopt whose caller returns at 0x395421 |
| 18 | 0 (gf 1, from n 4076) | `43e712` land composition ← `4f80a8` ← `426f28` coordinator update ← `363c83` common tick | 18 compositions at the first coordinator update |

**Differs from the §4.7.6 expectation** ("one draw per `Create_Team` chunk under `0x2a4fc0`, adopt return `0x2a61d5`"):
no hit has 0x2a61d5 or 0x2a4fc0 in its stack. The starting forces make **one** degenerate draw (caller 0x395421), and
the teams get their ctor draws from **land compositions in the first coordinator update** (gf 1), not at their creation.

## Item 3: space group move (partial: two fighter squadrons only)

Run `tools/probe_mod/cu8h_probes/out/20261109-002053-trace-cu8k-space-move`. Space skirmish Kuat, you EMPIRE, Easy AI
REBEL. A skirmish starts with only two TIE squadrons (no capital ship or frigate), so the move covered just those:
box-select both, arm, one right-click move, stop file 4 s later. 937 hits over mode frames 861..1191.

| count | mf | creator chain | reading |
|---|---|---|---|
| 1 | 933 | `5d5c19` space composition ← `4f80a8` ← `5d51f7` (space vf5) ← `426f28` ← `363c83` | the two squadrons merge into one role-0 group: **1 draw**, as §4.7.6 expects |

## Items 1 and 3 with ships: Kashyyyk campaign space battle (cu8k t05 run of 2026-11-09)

Run `tools/probe_mod/cu8h_probes/out/20261109-062335-kashyyyk` (`run_cu8k_kashyyyk.cmd`: probe campaign "CU8K
PROBE: KASHYYYK SPACE", human Rebel fleet Calamari_Cruiser + Nebulon_B_Frigate + 2 Rebel_X-Wing_Squadron moved from
Corellia to Kashyyyk against an AI-less Empire star base). Two traces of one battle (`trace<k>-*/trace.log`,
`sites.txt` section 5 lists the ctor draws).

### Item 1: the space battle load: **settled, with one addition**

Trace 1, armed before Begin, stopped at mode frame 240 (815 draws). Mf 0 = 198 draws (196 on the load thread: 151
campaign post-load, 45 chunk `0x102`; 2 in the first tick). The ctor draws of mf 0, all on the load thread, all
creator 0x4f738a (degenerate, vf8 return 0x39bda0), under the campaign spawn 0x2a00b0 ← 0x2a1eb0:

| draws | adopt caller (return) | via | reading |
|---|---|---|---|
| 2 | team spawn helper 0x2a5a60 (0x2a61d5) | campaign spawn 0x2a00b0 (0x2a1b51) | one per team container: the two X-wing squadrons |
| 1 | fleet placement 0x4fca30 (0x4fd0a3) | 0x4fbb10 (0x4fc1d7) | placed object (capital class) |
| 1 | fleet placement 0x4fdfb0 (0x4fe59d) | 0x4fbb10 (0x4fc25c) | placed object (screen class) |
| 2 | fleet placement 0x4fd5b0 (0x4fdb3d) | 0x4fbb10 (0x4fc285) | placed objects (lead class) |
| 2 | movement lock counter 0x3a8e50 (0x3a8e9a) | `TeamLocomotor` vf82 0x62f800 (0x62f856) ← 0x4fbb10 (0x4fc34b) | **not in the §4.7.6 expectation**: one more per team container, at the end of the fleet placement |

- As expected: one draw per qualifying container (0x2a61d5) and one per placed object (the four fleet units: 1 + 1
  + 2). Which unit got which placement class is not in the trace (draws carry no object id) [M: the two ships as
  capital and screen, the two squadrons as lead].
- Added: the fleet placement ends with a TeamLocomotor vf82 call per team container that adopts again through the
  movement lock counter: **8 draws for this fleet = 3 per fighter squadron + 1 per ship**. The two ships are not
  adopted as "members not in a team" (no 0x2a1bcd draw).
- After the load (no order given): space compositions (0x5d5c19 ← 0x5d51f7 ← 0x426eb0) at mf 128, 140 (two), 147
  and 162, and at mf 162 the star base's hangar launch (`SpawnSquadron` 0x594dc0 → 0x595a30 → helper 0x2a61d5) gives
  one degenerate draw plus one space composition. What the compositions at 128-147 belong to is not traced [L].

### Item 3 with a capital ship and a frigate: **count settled, split [M]**

Trace 2: the four units box-selected, armed, one right-click move into empty space; stopped by itself 600 mode
frames after arming (mf 964-1564, 2564 draws).
- **Mf 998: four space-composition ctor draws in a row** (n130-133, one `int(1, 30)` each, stack 0x40d11f ←
  0x4f6fd4 ← 0x5d5c19 ← 0x4f80a8 ← 0x5d51f7 ← 0x426eb0 ← common tick 0x363c83), after that frame's object-service
  draws (`Update_Weapon` 0x387400). No other frame has more than one; this is the order's frame.
- §4.7.6 expects 2 for the ships plus 1 per role-0 group (1 when the squadrons merge, as the two TIE squadrons did
  at Kuat): 4 = 2 + 2, so the two X-wing squadrons stayed **two role-0 groups** here [M: inferred from the count;
  no object ids]. Why they did not merge (distance between them, bucket rules of §4.6) is not read [L].
- The other ctor draws of the window are not the order's: 13 single space compositions spread over mf 1024-1558
  and two star-base hangar launches (mf 1062 and 1362: degenerate draw via 0x595a30 → 0x2a61d5, then one space
  composition in the same frame).

## Item 4: jetpack (cu8k t05 runs of 2026-11-09): **settled** by run 3; runs 1 and 2 killed the probe script

### Run 3, marker targets (`out/20261109-065448-jetpack`, game pid 21200): **settled, as §4.7.4 expects**

`run_cu8k_jetpack.cmd` with the marker-target kit: land skirmish Dagobah, human EMPIRE, AI REBEL; three
Dark_Trooper_PhaseII A, B, C for the human (ids 69512-69514) and three `Story_Trigger_Zone` markers TA (4110,1200),
TB (4060,1320), TC (4060,1440) spawned at Lua frame 300. Every `Activate_Ability("JET_PACK", <marker>)` returned
`res=ok` (before each call: `has=1 ready=1 active=0`); the script ran to `JEND` / `DONE` at Lua frame 1050. Trace:
armed at mf 470, stopped by the DBGSTOP record (Lua 990) at mf 1034, 89473 hits; all 44 ctor draws are land
compositions (creator 0x43e712 ← 0x4f80a8 ← coordinator update 0x426f28 ← common tick 0x363c83). The AI makes two
every 30 frames (mf 487, 517, ... 1027: 19 pairs = 38). The other six are single draws, each the **last LCG draw of
its frame**, after all of that frame's object-service draws:

| mf | order (Lua frame) | walk jiggle of the unit (0x1ffc00 ← 0x42c270 ← 0x620aa9) | reading |
|---|---|---|---|
| 571 | A.Move_To (570) | A walks mf 572..585 | the move's composition |
| **586** | **A JET_PACK while moving (585)** | **A's last jiggle mf 585, none from 586 on** | **the swap's coordinator: one draw** |
| **676** | **B JET_PACK standing (675)** | B never walks | **the hold: one draw** |
| 706 | none | - | not a trial: the AI's (run 2 had one such draw at mf 709 with no trial running) [M] |
| 766 | C.Move_To (765) | C walks mf 767..780 | the move's composition |
| **781** | **C JET_PACK while moving (780)** | **C's last jiggle mf 780** | **the swap's coordinator: one draw** |

- **Item 4 settled:** a JET_PACK activation gives exactly **one land-composition ctor draw** (0x43e712), in the first
  frame in which the unit no longer runs its Walk locomotor, at that frame's coordinator update after the
  object-service draws; the same for a standing unit (the hold). An order given at Lua frame F shows its draw at mf
  F + 1 for Move_To and JET_PACK alike, so the swap adds nothing beyond the coordinator it makes.
- **No other draw from the jetpack:** no stack in the window passes through a `JetPackLocomotorBehaviorClass`
  function (0x640490..0x641df0, 0x370fd0 / 0x371060) or the swap (0x3a9910 / 0x3a38d0), across three take-offs,
  flights and landings (A moves mf ~620..~720, B ~720..~815, C ~825..~915 by the JST polls; each starts about 35-45
  frames after its activation). The flight draws nothing from the synced LCG.
- **The swap back draws nothing**, as §4.7.4 says: `Is_Ability_Active` drops to 0 for A between mf 798 and 813 and
  for B between 903 and 918, and no off-beat ctor draw comes near either.
- Seen on the side [M]: after landing, A (whose saved move goal was TA) stays at 4067,1232, about 54 units short of
  TA, and never walks again (no 0x620aa9 jiggle after mf 585); B and C land within 5 units of their markers.

### Run 1, bool target (`out/20261109-061804-jetpack`): the probe script died

Run `tools/probe_mod/cu8h_probes/out/20261109-061804-jetpack` (`run_cu8k_jetpack.cmd`: land skirmish Dagobah, human
EMPIRE, AI REBEL; the probe Lua spawns three Dark_Trooper_PhaseII A, B, C for the human at 10 s and stages the
JET_PACK trials; `trace1-jetpack/` holds the debugger trace, mode frames 436..1573, 179271 hits).
- **The probe script died at the first JET_PACK call.** `records.log` ends with `JORDER who=A what=Activate_Ability
  step=before` at Lua frame 495: no result record, no further heartbeat, no DBGSTOP. The call was
  `A.Activate_Ability("JET_PACK", true)`; like a call with no second argument, it kills a retail Lua thread silently.
  The ability never started: A's Walk-locomotor jiggle (two draws per frame at 0x1ffc00 ← 0x42c270 ← 0x620aa9) runs
  without a break from mf 482 to 695, the walk to its Move_To goal, and no stack in the trace passes through a
  `JetPackLocomotorBehaviorClass` function (0x640490..0x641df0, 0x370fd0 / 0x371060). Trials B and C never ran.
- What the trace does show: A's `Move_To` (Lua frame 480) gives **one land-composition ctor draw at mf 481** (creator
  0x43e712, as for a group move). The other 91 ctor draws are the AI's: two every 30 frames from mf 457 on.
- The trace is slow: 276 LCG steps per frame, 98 % of them formation jiggle (5 steps per call, two calls per unit per
  frame) of about 40 walking squad members of the AI (0x62ca10 → 0x62d620 → 0x42c270); 1138 frames took about 170 s.
- The kit was changed for run 2: every JET_PACK call passed a position (A: its move goal; B, standing: 300 units east; C as
  before), the debugger is armed 240 frames before trial A (the arm file reached the debugger about 106 frames after
  the DBGARM record), and `trace.log` / `out\trace_status.txt` now show progress while the trace runs.

### Run 2, position targets (`out/20261109-062916-jetpack`, game pid 2728): **no JET_PACK ran**

Debugger armed at mf 576, stopped by `--stop-after-frames 1500` at mf 2076 (234102 hits, about 219 s wall time);
`trace.log` and the status file worked.
- **No trial ran.** No stack in the trace passes through a `JetPackLocomotorBehaviorClass` function (0x640490..0x641df0,
  0x370fd0 / 0x371060) or the secondary-locomotor swap (0x3a9910 / 0x3a38d0). The only non-team Walk-locomotor
  jiggle (0x1ffc00 ← 0x42c270 ← 0x620aa9, 4 LCG hits per frame) is A's: one unbroken run mf 576..785, the same
  ~213-frame walk to its Move_To goal as in run 1, so A's JET_PACK (Lua frame 585, about mf 586) did not interrupt
  it. C never walked, so the script never reached C's Move_To (Lua frame 765).
- **The probe script died at A's (or B's) `Activate_Ability("JET_PACK", Create_Position(...))`.** The last record found
  is the heartbeat at Lua frame 573 (after A's Move_To at 570); no heartbeat, JST poll, JORDER or DBGSTOP record of
  the jetpack script turned up afterwards, although the scanner kept running past the detach (it still found two old
  records of another script). Records n 44..48 (A's Move_To) were also missed while the script was alive, so the
  scanner drops records under the debugger; the missing C walk is what places the death before trial C. If A's
  call had only failed a silent gate (0x39bbd0 / 0x39b480 return 0, no error), the script would have gone on to C.
- Why a position kills it: **not through the binding** (cu8k t10, "Which check ends the script" below: a live
  `PositionWrapper` passes every JET_PACK check). Run 3, identical except for the marker targets, ran through. RE of the binding (0x570580 → 0x570a50, JET_PACK = ability 0x11): the
  target check 0x4990b0 accepts `AITargetLocationWrapper`, `GameObjectWrapper`, a task force and `PositionWrapper`
  (it copies x, y, z from the wrapper); the next step, 0x397d90(object, 0x11), returns the unit-ability record and a
  null gives the script error "object does not seem to have valid ability data"; any script error (0x248190) sets
  the script's error flag and ends the whole script, so `pcall` does not help. Every shipped call passes a game
  object (a `Find_Hint` marker, a `STORY_TRIGGER_ZONE`, an AI target); none passes a position or a bool.
- What the window does show: 136 land-composition ctor draws (creator 0x43e712), all the AI's (two every 30 frames
  from mf 577, one extra at mf 709). A's Move_To draw fell before the debugger armed.
- The kit was changed for run 3: each trial targets a `Story_Trigger_Zone` marker spawned with the units by
  `Create_Generic_Object` (fallback `Generic_Marker_Land`), as `STORY_UNDERWORLD_ACTI_M02_SPACE.LUA` spawns one and
  the story scripts target one; a trial whose marker is missing is skipped; each activation's `before` record carries
  `Has_Ability` / `Is_Ability_Ready` / `Is_Ability_Active`.

### Which check ends the script (cu8k t10, RE of 0x570580 → 0x570a50, 2026-11-09)

Order of the checks in 0x570a50 [H]. "Error" = 0x248190: it sets the script's error flag, the binding returns no
value, and the whole script is ended at the end of the service pass (LUA_API "What an error does to the script"),
so `pcall` does not help. "Warning" = 0x248620: no flag, the script goes on.
1. Wrapper without an object → silent false (nil).
2. Argument count ≠ 2 → **error** "invalid number of parameters. Expected 2". A Lua `nil` is a present argument, so
   `Activate_Ability("X")` errors and `Activate_Ability("X", nil)` does not (it fails later, by kind).
3. Argument 1 not a string → **error**; unknown ability name → **error**.
4. The unit lacks the ability (0x3986b0) → **warning** "Object %s does not have ability %s", nil.
5. Not ready (0x39bbd0) or `Can_Activate` (0x39b480(obj, t, 1)) false → **silent** nil. The argument is not looked at
   yet, so a bad argument costs nothing while the ability recharges.
6. Signal 0xb for the ids of AI_TACTICAL §7.3.3, then the per-ability argument check (second switch) below.

**JET_PACK (0x11)** [H]: 0x4990b0(argument 2, out position, no facing). It accepts, in this order:
`GameObjectWrapper` whose object is alive (0x5791e0 non-null; position obj +0x78..+0x80), `AITargetLocationWrapper`
with a target (its vf+0x68 position), a task force (its vf+0x170 position) and `PositionWrapper` (x, y, z at
+0x28..+0x30). Anything else (bool, nil, number, string, a dead object's wrapper, an AI target without a target)
returns false → **error** "invalid type for parameter 2. Ability JET_PACK expects a target position."
- **Run 1 (`true`) dies here**: the bool is not one of the four classes, so the "expects a target position" error
  ends the script. The ability was ready (the call reached the argument check; run 3 had `ready=1` at the same
  timing), which is why the error fired.
- **A `Create_Position` result passes.** `Create_Position` returns a `PositionWrapper` (0x541c00 → 0x60eba0, class id
  0x140a44290), 0x4990b0 copies its x, y, z and returns true. The rest of case 0x11 never looks at the argument
  again: 0x397d90(unit, 0x11) depends only on the unit (non-null for the Dark Trooper, as run 3 shows); the command
  (43, mode on, target id 0x3FFFFF) carries only the copied point, also for a marker target (0x4990b0 gets no
  facing pointer, so no object id is kept). A marker and a position at the same x, y give the same command; only z
  differs (marker: its ground z; `Create_Position`: the kit's site z 16.55). No later step dereferences the target as
  an object, and the execute (0x430270) and the return-value test (locomotor vf+0x300) do not call Lua.
- **So run 2's death is not the binding's.** Had A's call reached the binding with a live `PositionWrapper`, the
  command would have run and stopped A's walk at ~mf 586 as in run 3; A walked on to mf 785. So either the value
  that reached `Activate_Ability` was not a `PositionWrapper` (a nil or a number gives the same "expects a target
  position" error), or the script died in the kit's own code before the call [M: the run-2 kit was overwritten by
  run 3's and is not in git; its records past Lua frame 573 were lost].

**Per ability: the argument kinds that work** (second switch of 0x570a50, ids decimal; ability names OBJECT_TYPES
§9.1.4) [H]:

| ids | argument 2 must be | otherwise |
|---|---|---|
| 1 3 7 9 11 29 32 33 39 40 43 48 52 54 56 60 62 63 65 68 69 70 | a bool (its value = mode on / off) | error "expects a bool" |
| 2 DEPLOY | a bool; equal to the current deployed state → silent nil | error "expects a bool" |
| 6 HUNT, 10 LURE, 12 AVOID_DANGER | a bool, **value ignored** (mode always on); the command goes to the unit, or to its team container when the unit is a team member (obj +0x348 ≠ −1) | error "expects a bool" |
| 14 23 24 25 35 36 38 44 46 49 57 61 72 | anything (not read) | - |
| 5 26 28 42, 17 JET_PACK, 50 67 71 73 74 | a position source for 0x4990b0: live object, AI target with a target, task force, `PositionWrapper` | error "expects a target position" |
| 13 15 16 18 20 21 22 27 30 31 34 37 41 45 51 53 55 58 59 64 66, 19 | a `GameObjectWrapper` or `AITargetLocationWrapper` (no position, no task force) | wrong class: error "expects a target object"; dead object / AI target without object: **warning** "target object is already dead", nil |
| 4 HARASS, 8 AFTERBURNER, 47 FIRE_LOBBING_SUPERWEAPON, 75 DEPLOY_SQUAD, 76 | - | error "unsupported unit ability" |

After the argument check, the object-target ids and 50 67 71 73 74 also error ("not hooked up to special ability
logic") when the type's `GUI_Activated_Ability_Name` does not match, and JET_PACK errors when 0x397d90 finds no
ability record. Correction: ABILITIES_HARDPOINTS §2.9 and AI_TACTICAL §7.3.3 listed 6 / 10 / 12 as "optional target";
they need a bool (CU8F_T04_T06's `("LURE", true)` runs agree).

**What the port needs** (`engine/lua/src/bind_objects.cpp` `activate_ability`, dev at this writing):
1. Keep the retail order: name checks (error), not the unit's ability (warning, nil), not ready / `Can_Activate`
   (silent nil), and only then the argument kind. Today the port checks the argument first and answers nil.
2. A wrong argument kind for the ability (rows above) must be a **script error that ends the script**
   (`Call::script_error_stop`), not `push_nil`: a bool or a dead object for JET_PACK and the other position ids; a
   non-bool for the toggle ids, DEPLOY and 6 / 10 / 12; a position, task force or bool for the object ids. A dead
   object for the object ids and 19 stays a warning with nil.
3. Argument count ≠ 2 (including the one-argument `Activate_Ability("NAME")` and the no-argument form on a tactical
   object) is an error that ends the script; today the port activates (no name: the type's first ability). The
   galactic form (0x570670) is separate and unchanged.
4. 6 / 10 / 12: the bool's value is ignored (mode on) and the command targets the team container for a team member;
   the port's `OptionalTarget` family uses the bool as on / off and takes an object.
5. Ids 4, 8, 47, 75, 76: error ("unsupported").
6. JET_PACK with a `PositionWrapper` is right as it is (accepted, same as a marker at that point). A task-force
   argument gives the task force's own position (vf+0x170) in retail; the port's `position_arg` uses its first live
   unit [M: what vf+0x170 returns was not read].

## Luke saber run (CU8F §2.7 item 6, cu8k t05 run of 2026-11-09): **settled**

Run `tools/probe_mod/cu8h_probes/out/20261109-061205-luke` (`run_cu8k_saber.cmd` = the cu8h `luke` session with the
memory watcher logging the thrown saber every frame; `report.txt` "thrown sabers"). Watcher rows are polled, not
frame-locked. Below, step k of the saber is the k-th 8-unit advance (travelled = 8k), and F0 its spawn frame.

| | throw 1 (holstered, target 150 units east) | throw 2 (drawn, target about 122 units away) |
|---|---|---|
| saber / target | id 69514 → `REBEL_TROOPER` 69513 | id 69571 → `REBEL_TROOPER` 69567 |
| spawn (F0) | mf 462 (first row: travelled 0) | mf 1511 (first row at 1512 still shows travelled 0; every later row is one step ahead of 1512) |
| aim point (SAP +0x2d8) | 3910, 1600, 30.047 = target position + 7 (`Ranged_Target_Z_Adjust`) | 3910, 1720, 4.607 (target z −2.393 + 7) |
| spawn point, distance to Luke | 3772.99, 1601.75, 48.43; 18.8 | 3888.89, 1608.54, 42.67; 19.4 |
| max travel pack +0x6c | 273.634 = 250 + 5.250 + 18.384 | 293.311 = 250 + 5.250 + 38.061 |
| **target radius R** | **5.250** | **5.250** |
| speed | 8 per frame, one step per frame from F0 + 1 | the same |
| at the aim point | step 19 (mf 481, d to Luke 150.1) | step 19 (mf 1531 row, 1.5 units from it) |
| end of flight | step 35 (mf 497, travelled 280 ≥ 273.6): **detonates on the way back, 27.7 units from Luke**; no catch | step 34 (mf 1545): **within `Catch_Dist` 15 of Luke (7.2)**; the saber is flagged dead at mf 1547 (end of its path: step 36 = 288 < 293.3 [inferred]) |
| ability end (SAP +0x2e8 cleared) | **mf 497 = F0 + 35, the death frame: ε = 0** | **mf 1556 = catch frame + 11** |
| object removed from the store | mf 547 (50 frames after the death flag) | not seen before the watcher stopped |

- The §2.7 rules hold as written: max travel = MFD 250 + R + |aim z − spawn z| (both throws, R = 5.25 for
  `Rebel_Trooper`, container included when it has one); the first step on F0 + 1 and one 8-unit step per frame;
  death at step ceil(+0x6c / 8) = 35 for throw 1; catch end = F0 + k_c + 11 for throw 2 (k_c = 34, throw to end 45
  frames).
- **ε = 0**: Luke's update in the death frame already sees the dying saber (flags 0x42), so the saber is serviced
  before him in that frame (the [M] service order of §2.7 step 3 is confirmed for this case).
- Leg-1 detour: throw 1 reaches the aim point after 152 units against a straight 138.2 (|T − P0| in 3D): 14 units of
  jitter.
- For the port's jitter-free stand-in (§2.7 step 4): R(`Rebel_Trooper`) = 5.25 and ε = 0 replace the estimates.

## Not run

- Nothing left of cu8k t05: items 1, 3 (with ships) and 4 and the saber rerun are done. Item 3's split (which
  groups the four draws belong to) is inferred from the count, not traced.

## Follow-ups

- [RE] Name the adopt caller returning at 0x395421 (skirmish start forces) and correct §4.7.6 item 2 / §4.7.2.
- [port] Check that the port's land skirmish start draws one degenerate ctor draw plus one land-composition draw per
  team at the first coordinator update (gf 1), instead of per-`Create_Team` draws.
- [RE] MOVEMENT §4.7.4 / §4.7.6 item 4: mark the jetpack draw confirmed (one land-composition draw at the
  coordinator update of the first frame without the Walk locomotor, standing or moving; none from the flight or the
  swap back) and update the §4.7.6 run paragraph (items 1, 3 with ships and 4 now run).
- [port] JET_PACK: the swap to the secondary makes one land-composition ctor draw at the next coordinator update
  (§4.7.5 item 2), also for a standing unit; the jetpack flight and the swap back draw nothing.
- [RE] Which check in Activate_Ability (0x570a50, case 0x11) ends the script for a `Create_Position` target (runs 2
  and 3 differ only in the target); matters for the port's Lua binding (LUA_API) if a mod passes positions.
- [RE] MOVEMENT §4.7.3 / §4.7.6 item 1: add the movement-lock-counter adopt per team container at the end of the
  fleet placement (TeamLocomotor vf82 0x62f800 from 0x4fbb10) and the per-class placement draws; name which
  `Ship_Class` goes to which placement function.
- [RE] Item 3: why the two X-wing squadrons formed two role-0 groups (one draw each) when two TIE squadrons merged.
- [port] Space campaign battle start: 1 draw per team container (helper), 1 per placed object, then 1 more per
  team container (lock counter); a group move of N ships and K separate role-0 groups: N + K draws.
- [port] Saber throw: use R = 5.25 for `Rebel_Trooper` and ε = 0 (CU8F §2.7 step 4); the catch constant 11 is confirmed.
