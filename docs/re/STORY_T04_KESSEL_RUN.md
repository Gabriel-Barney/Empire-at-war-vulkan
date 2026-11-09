# story t04: retail Kessel UM01 story battle trace, compared with the port

Roadmap `story` t04 (S4 reference trace, Q2). Retail run of 2026-11-08 by Gabriel with
`tools/probe_mod/q2_traces/run_story.cmd` (reader scenario `story`: the campaign's linked tactical battle, mode stack 2).
Read with `tools/probe_mod/q2_traces/analysis/story_battle.py` (new) and `q2trace.py validate`; no binary read.
"Frame" = the battle's mode frame (30 per second) unless it says host frame.

| Run (`tools/probe_mod/q2_traces/out/…`) | Samples | End | Notes |
|---|---|---|---|
| `20261108-234224-story-vanilla` | 18002 (mode frames 0-18000), 0 missed, 1 torn (frame 1) | `ticks_reached` | the reference. Underworld campaign at Medium (the only difficulty offered; the kit README said Normal), Esc through the opening crawl and the battle's intro cinematic, then no input. The run folder was read while the run script was still in its post-trace step (`run_status.json` said `running`, no `summary.json` yet); the script finished on its own: reader end 04:53:22Z, cleanup 04:53:27Z, `summary.json` / `reference.crc` / `run_status.json` `done` exit 0 at 04:54:54Z (storyb t09). `q2trace.py validate`: 18002 digests OK, 0 errors |
| `20261108-224154-story-vanilla` | 1288 (frames 0-1286) | `timeout` | earlier attempt: the game lost focus and paused at frame 1286. Used only for the determinism check in §1 |

Seed slot 0, RNG 0 at load; the first battle sample (frame 0, global frame 2, stack 2, paused flag 1) has RNG `b4f2ee6c`,
412 draws from seed 0. Players (C lines): 8; human = index 6 (Underworld); AI flag on 0, 1, 2 and 7.

## 1. What retail does in 10 minutes without orders

| Frame | Event (trace) | Mission Lua (`Story_Underworld_ActI_M01_LAND.lua`) |
|---|---|---|
| 1 | 697 objects: Empire (owner 1) 74 (48 SQUAD_STORMTROOPER, 6 STORMTROOPER_TEAM, 4 DARK_TROOPER_PHASEII, 1 AT_ST_WALKER, doors, 3 consoles), Neutral (owner 3) 619 (props, 64 STORY_TRIGGER_ZONE, 48 UMP_PRISON_BARS, 14 of each of the five UMP_PRISONER_HUMAN* types plus UMP_PRISONER_WOOKIEE, 18 STORY_TRIGGER_ZONE_01), owner 4 two UMP_INTERFACE_NODE, Underworld (owner 6) UMP_TYBER_ZANN + ATTACKER_ENTRY_POSITION. 919 synced draws in this one frame, 111 in frame 2 | map load / first tick |
| 2-33 | stormtrooper squads settle (≤4.8 units, frames 2-14), prisoners wander (frames 2-33, ≤28 units); 36.7 draws / frame (frames 2-29) | |
| 6 | 9 SQUAD_STORMTROOPER + 1 STORMTROOPER_TEAM (Empire) appear | `State_Underworld_A01M01_Begin` OnEnter: `bribeguys = Spawn_Unit(IMPERIAL_MEDIUM_STORMTROOPER_SQUAD, bribed …)` (line 297) |
| 30-128 | nothing moves, 0.16 draws / frame | `CINE_Start_Mission_A`: `Cinematic_Suspend_Units`, `Suspend_AI(1)`, Bink intro |
| 129-130 | created at 129: MILLENNIUM_FALCON_LANDING, UMP_HANSOLO, UMP_CHEWBACCA (Neutral), URAI_FEN (Underworld), 2 UMP_INTERFACE_NODE (one removed at 130: the `broken_node` that takes 9999 damage); removed at 130: the five UMP_CINE_* objects and one UMP_MOVING_DOOR (`cinedoor2`); Tyber teleported 239 units | `IntroCineCleanup` (line 1314), started by Gabriel's Esc (`Story_Handle_Esc`, line 1290) |
| 162, 255 | OBJECT_HIGHLIGHT created (Neutral) | `StartMissionObjective` (line 1587; console hint "Destroy consoles to activate some doors") |
| 256-18000 | no object created, removed or moved; **no health change on any object in the whole run**; 2.6 draws / frame (201-1800: 4189, 1801-18000: 41771) | `Main` loop polls victory / defeat flags |

So with no orders: the Empire AI never moves toward or attacks Tyber, no scripted event fires after the objective
highlight (frame 255), and the battle does not end (the victory / defeat monitor stays held). Total draws over the
battle: 48069.

**Determinism.** The two runs have the same RNG state on every common sample from frame 0 to 128 (frame 26 differs
only because the earlier run sampled it mid-tick); they first differ at frame 129, where the full run's Esc started
`IntroCineCleanup` (the earlier run's Esc came at frame 248: same spawns and removals, 119 frames later). A retail
story battle is deterministic from seed 0 until the first human input; the first Esc (Bink skip) left no RNG trace.

## 2. The port's linked Kessel battle

Port: `build-wt-msvc\bin\alamo_headless.exe` (dev `d3237074`), with `ALAMO_FOC_DATA` / `ALAMO_EAW_DATA` from
`tools\test_env.cmd`:

```
alamo_headless --economy --galactic-ai --story --battles fight --campaign Full_Story_Campaign_Underworld
  --ai-type Underworld=AI_Player_Underworld --human Underworld --seed 0 --ticks 18010 --workers 4
  --trace-out port.q2t --crc-out port.crc --ai-log ai.txt
```

`--ai-type` is a workaround: `--human Underworld` alone fails ("no playable player of that faction") because the story
campaign gives the Underworld the AI type `ScriptableHuman`, which the run treats as unknown and skips before it looks
for the human (`engine/apps/headless/ai_run.cpp:196-213`). Run time 17.8 s.

Result: the linked land battle is on top from host frame 3 to the end (stack 2, galactic frame stays 3), it never ends,
and the mission script dies on its first state:

```
story lua: 9 Story_Underworld_ActI_M01_LAND: ...Story_Underworld_ActI_M01_LAND.lua:249: attempt to index global `falcon_gear1' (a nil value)
```

**Cause: map hints are not loaded.** Line 249 is `falcon_gear1.Hide(true)` after
`falcon_gear1 = Find_Hint("UMP_SMALL_GEAR", "falcongear1")`. In the `.ted` file each placed object's hint name is a
chunk `0x482` directly under the object chunk `0x44C` (sibling of `0x454` / `0x459` / `0x476`): child chunk `0`, mini
`01` = zero-terminated name. `UM01_A_Crimelord_Unleashed.TED` (maps.meg) has 154 of them (the name `falcongear1` at file offset
0x80147, `consolefirst`, `door9`, `bribed`, `tybergoto`, …). The port's object parser skips `0x482`
(`engine/terrain/src/ted_read.cpp:588-639`; `ObjectPlacement` has no hint field, `engine/terrain/include/alamo/terrain/ted.hpp:256`),
so no map object gets a hint: `Find_Hint` reads `ScriptObjectState::hint` (`engine/lua/src/bind_objects.cpp:601-624`),
which only `Create_Cinematic_Transport` sets (`engine/lua/src/bind_script_gaps.cpp:339`). Every `Find_Hint` on a map
object returns nil, in every story mission that uses one (the UM01 script has 187 `Find_Hint` calls).

Consequence in UM01: nothing after line 249 of the Begin state runs: no bribe squad (retail frame 6), no
`Register_Death_Event` on the consoles (the doors cannot open), no `Register_Prox`, no `CINE_Start_Mission_A`
(no `Suspend_AI` / `Lock_Controls`), no `Main` thread (no victory / defeat checks). The port's battle "stays held" only
because the monitor never started.

## 3. Comparison and first divergence

| | retail | port |
|---|---|---|
| battle start | frame 0 at global frame 2, stack 2 | first sample with the battle on top: host frame 3 (galactic frame 3), stack 2 [M: the port may push the linked battle one galactic tick later; the two frame counters are not aligned yet] |
| draws, seed 0 to the battle | 412 | 335 (galactic tick 1; tick 2 draws 0) |
| draws, battle frame 1 / 2 | 919 / 111 | 44 / 19 |
| frames 2-29 | 1028 (36.7 / frame) | 19 (0.7) |
| frames 30-128 | 16 (0.16) | 49 (0.5) |
| frames 129-200 | 146 (2.0) | 28 (0.4) |
| frames 201-1800 | 4189 (2.6) | 651 (0.4) |
| frames 1801-18000 | 41771 (2.6) | 6266 (0.4; draw sizes 2-8, mostly 2) |
| mission Lua | Begin at frame 6, cinematic, cleanup at 129 (Esc), objective at 162 / 255 | Begin dies at story frame 9 (host frame; battle frame 6-7), line 249 |
| players | 8, human index 6 | 7, human index 5 |
| battle end in 18000 frames | no | no |

First divergence: **battle frame 1 (the map load / first battle tick)**: retail 919 + 111 synced draws, the port 44 + 19.
Candidates are the land map load's per-object construction and spawn draws (`CU8G_T02_ENV_PICK_RE.md` §3 steps 6a-10,
`TACTICAL_GAPS.md` §4.3); the split between load and first tick is not in the trace [L]. The second is behavioural: the
mission script (§2). After the intro, retail draws about 2.6 per frame in a battle where nothing moves or takes damage;
the port 0.4. What draws there is not known [L] (idle animation variant picks are one candidate, `CU8G_T02_ENV_PICK_RE.md`
§3 row 0x2aafb0).

Not comparable yet: objects. The port's `--trace-out` in a galactic-AI run writes the galactic store only
(`engine/apps/headless/port_trace.hpp`, sampled at `engine/apps/headless/ai_run.cpp:459-470`); the tactical writer
(`tactical_trace.hpp`) is wired into `--skirmish-ai` only (`engine/apps/headless/skirmish_run.cpp:836-850`). So object
counts by type and owner, spawn frames and movement cannot be read from the port run. `tools/parity` has no story
scenario for the same reason (README "Gaps").

## 4. Follow-ups

- [port] Load map hints: parse chunk `0x44C/0x482/0 mini 01` into `ObjectPlacement`
  (`engine/terrain/src/ted_read.cpp:588`, `ted.hpp:256`; the writer `ted_write.cpp` must keep it) and set the bridge hint
  for every placed object when a battle spawns its map (`CampaignBattles::story_begin`,
  `engine/apps/sim/src/campaign_battles.cpp:1304`, and the object-script bridge). Then rerun §2: the UM01 Begin state
  must spawn the bribe squad on battle frame 6.
- [port] `--human` for a story campaign: accept a faction whose campaign AI type is `ScriptableHuman`
  (`engine/apps/headless/ai_run.cpp:196-213`) so `--ai-type …=AI_Player_Underworld` is not needed.
- [port] `--trace-out` in `--galactic-ai` runs: write the tactical battle's objects while a tactical mode is on top
  (`PortTacticalTrace`, as `skirmish_run.cpp:836`), so object counts, spawns and moves compare with this trace; then add a
  `story_kessel` scenario to `tools/parity` (objects by owner at frame 1, Lua spawn frames, `outcome.battle_ended`).
- [port] Story_Handle_Esc: the engine calls this Lua function (exe string 0x85dde0, xref 0x328440; 42 shipped story
  scripts define it); the port never calls it. Needed to replay a run where the cinematic was skipped.
- [RE] When and how the engine calls `Story_Handle_Esc` (key path, game modes, synced command or direct call), from
  0x328440. Retail shows it changes the sim on the frame it runs (§1).
- [RE] The extra retail player: 8 players (no-AI indices 3, 4, 5) against the port's 7 (3, 4); which faction it is
  (`playerlist_dump.txt` of the run, or `Find_Player("Hostile")` in the M01 Begin state) [M].
- [RE] Draws of the land map load: retail 919 + 111 at battle frames 1-2 of UM01 against the port's 63; split them
  with the cu8h debugger (`tools/probe_mod/cu8h_probes/cu8h_dbg.py`, watchpoint on the LCG state) on this mission.
  Done (storyb t07, 2026-11-09): §5.1.
- [probe] Steady-state draws: ~2.6 per frame with nothing moving (frames 256-18000); trace their call sites with the
  same debugger for a few hundred frames. Done (storyb t08, 2026-11-09): §5.2.
- [kit] Rerun `run_story.cmd` without pressing Esc in the battle (README step 4 now says so): a fully hands-off
  reference that the port can replay without an Esc injection; the opening crawl before the battle may still be skipped.
  Done (storyb t09, 2026-11-09): §6, run `20261109-053933-story-vanilla`.
- [kit] `run_story.cmd` left `run_status.json` at `running` without `summary.json` (the post-trace validate takes about
  2 minutes on this 7.8 MB trace); find out why it stopped, or write the summary with
  `q2trace.py validate <trace> --json summary.json` after the run. Storyb t09 (2026-11-09): it did not stop. The original
  run folder (worktree `game-code-audit-results-207293`) has `summary.json` and `run_status.json` `done` / exit 0
  written 92 s after the reader ended; the folder was read during that window, while `run_status.json` still showed
  the last `running` save. The kit now shows `cleanup` / `summarizing` / `summarized` / `done`, writes `summary.json`
  in its finally block (a partial one from `reader_status.json` if validate cannot), validates a trace cut by a killed
  reader, has no time limit for story runs while the game lives (the 22:41 run ended by the 60-min reader timeout while
  the game sat paused at frame 1286) and logs to `run_trace.log`. `tools/probe_mod/q2_traces/README.md` "Story mission run".
- [RE] Format: add chunk `0x482` (object hint) to `docs/formats/SPEC.md` §1.6. Done (2026-11-09) in
  `docs/re/TERRAIN_WATER_POST.md` §1.6, where SPEC.md keeps the `.ted` layout.
- [kit] Storyb t07 / t08 kit ready (2026-11-09, not run yet): `tools/probe_mod/cu8h_probes/run_storyb_kessel.cmd`
  arms the cu8h debugger before Start Game and traces this mission to battle mode frame 18010;
  `cu8h_sites.py` splits the 412 galactic + 919 + 111 load draws by phase and call site and tables the
  steady-state draws (frames 256-18000) per call site. Steps: that folder's `README.md`.

## 5. Debugger trace: load draws and steady-state draws by call site (storyb t07 / t08)

Retail run of 2026-11-09 with `tools/probe_mod/cu8h_probes/run_storyb_kessel.cmd` (cu8h debugger: hardware write
watchpoint on the synced LCG state, one stack walk per write), run folder
`tools/probe_mod/cu8h_probes/out/20261109-051701-trace-storyb-kessel/` (`trace.log`, `report.txt`, `sites.txt`;
`sites.txt` is `cu8h_sites.py` output). Vanilla game, Underworld campaign at Medium, armed just before Start Game,
Esc once on the opening crawl, **no Esc in the battle**, stopped at battle mode frame 18010. 44076 watchpoint hits =
53927 LCG steps (the jiggle helper 0x1ffc00 below stores the state once per 4 inlined steps). "mf" = the battle
mode's frame counter read at the draw; the q2 trace's sample "frame N" holds the draws of mf N − 1.

**Determinism check against §1.** Galactic start 412 draws (the same 412 from seed 0), battle mf 0 = 919 and mf 1 =
111 draws: identical to the q2 reference. Mf 2-29: 918 draws (q2 frames 2-29: 1028 = mf 1-28, includes the 111),
mf 30-128: 15 (0.15 / frame; q2 0.16). From mf 129 on the two runs differ by design: the reference's Esc skipped the
intro cinematic at frame 129; here it played in full. The mission start that the reference shows at frame 129
(the Falcon landing objects, `IntroCineCleanup`) comes at mf 5514-7526 here (object-creation draws), and walking units
draw formation jiggle from mf 502 to 6924. From mf 7701 on there is no creation, locomotor or weapon draw, and
the rate is **2.579 per frame** (mf 7701-18000: 26564 draws), against the reference's 2.578 (frames 1801-18000:
41771 / 16200). The steady state is therefore the same in both runs.

### 5.1 t07: the 919 + 111 draws of the map load and the first ticks [H counts, M names]

Names: `docs/re/ghidra/functions.csv` class names and the RE docs cited. Every row is one draw per hit (`Get_Int`
0x1ffb40 unless noted).

| mf / thread | Draws | Call site (caller chain) | What it is |
|---|---|---|---|
| 0, LoadThread (global frame 2) | **883** | | `CU8G_T02_ENV_PICK_RE.md` §3 step 6 (chunk `0x102`) and the post-load step |
| | 111 | map object records: GOM loader 0x2b0a20 → parse 0x39c3a0 / 0x39ce40 → `Init_From_Type` 0x3989a0 (86); post-load fixup 0x3a0930 (25) | per record: model start frame 0x3a4820 (16), Idle attach 0x62dd40 (22), UnitAI pack ctor 0x550ed0 (23 + 15, two stamps), Weapon reset 0x54cef0 (8), SAP stagger 0x37c050 (2); `BehaviorClass` vf10 0x4c3000 from 0x3a0930 (25) |
| | 764 | company expansion: 0x2b0a20 (at 0x2b17bc, after the record loop) → 0x2ab060 → company placer 0x29e960 → 0x2b3be0 (666) / 0x2a5a60 (98) → `Create_Object` 0x29f810 → `Init_From_Type` | 0x2ab060 skips when 0x140b15490 is set and takes every loaded object whose type has `Company_Units` (+0x960) or `Squadron_Units` (+0x980) (A1_READINGS §2 line 934); the members' construction draws: Weapon reset 0x54cef0 via vf7 0x54ab00 (183), Weapon attach vf9 0x54c210 (162, TACTICAL_COMBAT: initial delay `Get_Int(0, int(FPS × recharge))`), Idle attach 0x62dd40 (137, first fidget frame `Get_Int(210, 450)`), model start frame 0x3a4820 (134, ANIM_PARTICLES "model created"), base Initialise 0x4c2f90 (50, `Get_Int(0, interval − 1)` per behaviour with interval ≥ 2), UnitAI pack ctor 0x550ed0 (40 + 36), Targeting attach vf9 0x5670b0 (22) |
| | 8 | `FormationClass` ctor 0x40d0d0 ← 0x4f6fb0 ← degenerate creator 0x4f738a ← `LandMovementCoordinator` vf8 0x43b470 ← 0x39bcb0 ← GOM post-load 0x395421 ← 0x2b4440 | eight `int(1, 30)`, MOVEMENT §4.7.6 (same as the skirmish start, `CU8K_T04_COORDINATOR_TRACES.md`) |
| 0, main thread (global frame 3, first tick) | **36** | | |
| | 25 | `FormationClass` ctor ← land composition 0x43e712 ← 0x4f8080 ← 0x426eb0 ← 0x363c83 (common tick) | 25 land-composition formations |
| | 7 | `Acquire_Target` 0x568a00 ← `Targeting::Service_Land` 0x634810 | 6 rescans `Get_Int(0, 15)`, 1 start player (the other 5 scans stopped after the reschedule: scan range ≤ 0 or 0x397f60 false) |
| | 3 | 0x62c420 (`LandTeamInfantryLocomotor`) ← 0x62cf90 ← 0x62d190 | `Get_Int(0, n − 1)` with n = 0x38f9e0(obj, clip), stored as a float: a random clip start frame [M] |
| | 1 | `Get_Float` 0x1ffbb0 at 0x3a7475 in the object service 0x3a6b80 | not read |
| 1, main thread (global frame 4) | **111** | | |
| | 62 | 0x62c420 ← 0x62cf90 (`LandTeamInfantryLocomotor`) | as above: squad members' random clip start frames [M] |
| | 49 | play-clip 0x3a92f0 (variant pick) ← 0x62cf90 at 0x62d0c9 | the squad members' clip variant draws |

Not drawn in this battle: the environment pick (CU8G §3 step 7: one draw only when the map has more than one
environment; none here), weather (step 12), the space-station marker pick and post-load anim variants of
BATTLE_TRANSITIONS §2.1 step 4 (no draw between the LoadThread's last draw and the first tick's 36). The 412
galactic draws before the battle are the galaxy build: base Initialise 0x4c2f90 (309), AIPlanetBuildQueue ctor
0x53b5c0 (66, `A2J_RE_BUILD_MARKET.md` Q4), model start frame 0x3a4820 (33), one other (1), first galactic tick (3), and the seed
write in Start_Game 0x28e890.

### 5.2 t08: the 2.58 draws per frame of the static battle [H counts, H names unless tagged]

Mf 7701-18000 (10299 frames, 26564 draws), five call sites, all on the main thread inside the common tick
0x3639d0 → 0x2be640 → object service 0x3a6b80, except the AI row:

| Draws / frame | Share | Call site | Behaviour |
|---|---|---|---|
| 0.772 + 0.772 | 59.8 % | `Acquire_Target` 0x568a00: 0x568a6e (reschedule `Get_Int(0, FPS / 2)`) and 0x568b11 (start player `Get_Int(0, n_players − 1)`) | the auto-target scan, **two draws per scan** (TACTICAL_COMBAT §2.4 "Synced draws per scan"), 0.772 scans / frame: 0.533 from `TeamTargeting::Service` 0x62ee00 → 0x62eeb0 (team containers, 5492 scans), 0.238 from `Targeting::Service` 0x633a30 → `Service_Land` 0x634810 (2455). With rescans every 30 + 0..15 frames, about 20 team containers and 9 single units scan; none finds a target (no `HP_Choose_Aim_Point` draws, no damage in the whole run) |
| 0.485 | 18.8 % | IdleBehavior service 0x62e570 at 0x62eacb: next fidget frame `Get_Int(210, 450)` (0xd2, 0x1c2) | TACTICAL_COMBAT.md, IDLE notes s78k t02 / s78l t03 (no-target branch): when frame ≥ I +8, play the idle with a fidget, then I +8 := frame + `Get_Int(210, 450)`. 0.485 fidgets / frame × 330 frames mean spacing ≈ 160 idle units |
| 0.393 | 15.2 % | same service at 0x62ea78: fidget variant `Get_Int(1, n − 1)` | drawn only when the idle clip has n ≥ 2 variants (0x397ae0); 81 % of the fidgets |
| 0.158 | 6.1 % | float draw 0x532480 at 0x6baffe in `Find_Plan_For_Goal` 0x6bae50 ← `AIGoalSystem::Propose_Goals_Incremental` 0x6cb120 ← `AIGoalSystem::Service` 0x6cacd0 ← `TacticalAIManager` vf3 0x5f5ab0 | the tactical AI player's goal proposals (AI_TACTICAL.md §4.4, slot 10: one float per accepted candidate), from mf 7527 (the end of the cinematic's `Suspend_AI`). Bursts: gaps of 1, 8 and 26 frames |

Sum 2.579. Not present: the IDLE "has a target" draw (0x62e848, `Get_Int(0, FPS)` + 90), weapon draws, locomotor
draws. The draws are the same kinds during the cinematic, at other rates: mf 1001-3000 = 0.956 / frame (IDLE 0.851, the
same fidget rate; scans 0.053 × 2, as `Cinematic_Suspend_Units` suspends most units' targeting [M]; no AI).

Movement draws seen before mf 7000 (not in the steady state): formation jiggle 0x42c270 → 0x1ffc00 (two calls per
walking unit per frame, x and y; 0x1ffc00(state, n) adds n uniform floats `((s >> 10) & 0x7fff) / 32768` and returns
(sum − n / 2) / (n / 2) [M for the constants], n read from global 0x140a28ba0 = 5 here: 5 LCG steps per call, 10 per
walking unit per frame), from Walk locomotor 0x620820 ← 0x61f840 / 0x61f510 ← vf6 0x61e930; and 0x61f840 at 0x61faa4
(`Get_Int(0, n − 1)` of a clip count 0x38f9e0, walk clip start frame [M]).

**Port** (0.4 / frame in §3): to reach 2.58 the port needs, per frame, (1) both scan draws on every due
`Acquire_Target` of every team container (`TeamTargeting`) and every TARGETING unit, also when nothing is in range;
(2) the IDLE fidget pair (variant when n ≥ 2, then `Get_Int(210, 450)`) for every unit with IDLE, team members
included; (3) the tactical AI's proposal floats once the AI is not suspended. The order inside a tick is the object
service order (AI_TACTICAL.md "Unit-level" row: GOM service order).

**Still open.** Which objects scan (20 containers + 9 units are inferred from the rate, not listed); whether the 5
reschedule-only scans at mf 0 are units with scan range 0 [L]; the float at 0x3a7475 (5 draws in the run).

## 6. storyb t09: the hands-off reference run

Retail run of 2026-11-09 with the storyb t09 kit (`run_story.cmd -NoLaunch`; the game was started by a
create-and-detach launcher, now `tools/probe_mod/common/launch_foc.py`, because `Start-Process explorer.exe <cmd>`
launches now die silently), run
`tools/probe_mod/q2_traces/out/20261109-053933-story-vanilla`. Underworld campaign at Medium, **no key and no click after
Start Game**: the opening crawl and the battle's intro cinematic played in full. End `ticks_reached` at mode frame
18000, exit 0: 18003 samples (frames 0-18000), 0 missed, 0 torn, 18003 digests OK, 0 errors, 412 draws from seed 0 to
the first sample (as in §1). One stall: no frame for 75 s at frame 26 with the game in front (the sim does not advance
while the crawl plays); the RNG state on both sides of it equals the earlier runs', so the stall changed nothing.
`analysis/story_battle.py` on the run:

| Frames | Events | Draws (/ frame) |
|---|---|---|
| 1 | the same census as §1 (Empire 74, Neutral 619, owner 4 two nodes, Underworld Tyber + entry position) | 919 |
| 2-29 | squads settle, prisoners wander | 1028 (36.7) |
| 6 | the Begin state's bribe squad (9 SQUAD_STORMTROOPER + 1 STORMTROOPER_TEAM) | |
| 30-128 | nothing moves | 16 (0.16) |
| 129-920 | cinematic: UMP_CINE_MOUSEDROID moves (20-920), one UMP_MOVING_DOOR removed at 798 | 5299 (6.7) |
| 921-3302 | | 2325 (0.98) |
| 3303-5360 | Tyber walks (3303-7541) | 4686 (2.3) |
| 5361-7542 | MILLENNIUM_FALCON_LANDING + landing dust 5361, URAI_FEN 5528 (removed 6742), UMP_HANSOLO / UMP_CHEWBACCA 5661 (move to 5756), UMP_MANDALORIAN 5961 (one health change, removed 6915), URAI_FEN 6741 (removed 7542); at 7541-7542 the end of the cinematic: two UMP_INTERFACE_NODE (one removed at 7542), URAI_FEN for the Underworld (owner 6), the five UMP_CINE_* objects removed | 12210 (5.6) |
| 7574, 7668 | OBJECT_HIGHLIGHT (the objective hints) | |
| 7543-7700 | | 416 (2.6) |
| 7701-18000 | no object created, removed or moved, no health change | 26545 (**2.58**) |

Total 53444 draws in the battle. The battle does not end (no orders).

**Against `20261108-234224` (§1).** Same start sample (frame 0 at global frame 2, RNG `b4f2ee6c`) and the same RNG
state and object digest on every frame 0-128; the first difference is frame 129, where the earlier run's Esc started
`IntroCineCleanup`. Against `20261108-224154` the first difference is 248 (its Esc). So all three retail runs agree up to
the first human input, and the Esc on the opening crawl (both earlier runs) left no trace. Here the mission start the
Esc produced at 129 comes from the cinematic's own end (the Falcon at 5361, cleanup at 7541-7542, objective at
7574 / 7668), and the steady state from 7701 has the rate of §5.2 (2.58 / frame; the reference after its Esc: 2.58).

**Against the debugger run of §5** (also no Esc in the battle). Up to frame 505 every RNG state of this trace occurs
in the debugger log at mf = frame − 1, as expected. After that every distinct end-of-frame state of this trace still
occurs in the debugger log, except in walking stretches where the jiggle helper stores only every 4th step (447 of
10677). So the draw sequence is the same. But the debugger's `mf` stamp for the same state wanders between −74 and
+31 frames, including the static stretch after 7701. Two explanations: the debugger's frame stamp under its heavy
slowdown, or a per-frame draw split that depends on run speed in retail [L]. Before this run serves for a frame-exact
check past frame 505, a second run with this kit (no debugger) should give the same states on the same frames.

**Use as the parity reference.** Yes, for the hands-off port replay: no human input, so the port needs no
`Story_Handle_Esc` injection (§4); it is frame-identical to the other retail runs up to frame 128; it is a complete
18000-frame trace; and it contains the whole scripted intro (cinematic, mission start, objectives). It is also
frame-exact past frame 505 (next paragraph). This run replaces `20261108-234224` as the reference. `20261108-234224` stays as the Esc run, used for the determinism
check.

**Second hands-off run (2026-11-09, `20261109-064135-story-vanilla`, same kit, no debugger).** Again no key and no
click after Start Game, ended at `ticks_reached`, 0 missed, 0 torn. Converted with `q2trace.py to-q1` and compared with
`q2trace.py compare` against this run: **MATCH on all 18000 ticks** (`ref_rng` and `ref_objects`). So retail repeats a
hands-off Kessel battle frame for frame, and the −74..+31 wander in the debugger comparison above comes from the debugger's `mf`
stamp, not from the game: at normal speed nothing depends on run speed [H]. (Not excluded: a draw split that changes only
under the debugger's heavy slowdown. Neither of the two runs was slowed.) An earlier attempt the same morning,
`20261109-063832-story-vanilla` (marked `NOT_HANDS_OFF.txt`), had an Esc in the battle's intro cinematic. The planet shot
after the crawl is already the battle. That run diverges at tick 227 and was quit early; do not use it.

**Kept in** the main checkout's `tools/probe_mod/q2_traces/out/20261109-053933-story-vanilla/` (`out/` is not in git).
Copy `trace.q2t`, `summary.json`, `reference.crc`, `run_status.json`, `reader_status.json`, `run_trace.log`,
`manifest.json`, `cleanup.log` and `playerlist_dump.txt` (not `profile_backup/`, which is a copy of the player's save
profile).
