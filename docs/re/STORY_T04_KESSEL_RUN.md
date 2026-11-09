# story t04: retail Kessel UM01 story battle trace, compared with the port

Roadmap `story` t04 (S4 reference trace, Q2). Retail run of 2026-11-08 by Gabriel with
`tools/probe_mod/q2_traces/run_story.cmd` (reader scenario `story`: the campaign's linked tactical battle, mode stack 2).
Read with `tools/probe_mod/q2_traces/analysis/story_battle.py` (new) and `q2trace.py validate`; no binary read.
"Frame" = the battle's mode frame (30 per second) unless it says host frame.

| Run (`tools/probe_mod/q2_traces/out/…`) | Samples | End | Notes |
|---|---|---|---|
| `20261108-234224-story-vanilla` | 18002 (mode frames 0-18000), 0 missed, 1 torn (frame 1) | `ticks_reached` | the reference. Underworld campaign at Medium (the only difficulty offered; the kit README said Normal), Esc through the opening crawl and the battle's intro cinematic, then no input. The run script stopped before its summary step (`run_status.json` still says `running`, no `summary.json`); `q2trace.py validate` here: 18002 digests OK, 0 errors |
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
- [probe] Steady-state draws: ~2.6 per frame with nothing moving (frames 256-18000); trace their call sites with the
  same debugger for a few hundred frames.
- [kit] Rerun `run_story.cmd` without pressing Esc in the battle (README step 4 now says so): a fully hands-off
  reference that the port can replay without an Esc injection; the opening crawl before the battle may still be skipped.
- [kit] `run_story.cmd` left `run_status.json` at `running` without `summary.json` (the post-trace validate takes about
  2 minutes on this 7.8 MB trace); find out why it stopped, or write the summary with
  `q2trace.py validate <trace> --json summary.json` after the run.
- [RE] Format: add chunk `0x482` (object hint) to `docs/formats/SPEC.md` §1.6.
