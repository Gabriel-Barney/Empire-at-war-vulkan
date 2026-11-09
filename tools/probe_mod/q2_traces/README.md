# Q2 reference traces (`tools\probe_mod\q2_traces\`)

This kit records what the original game does, tick by tick, so the new simulation can be compared with it.
It records two runs of the Steam FoC 1.121 64-bit game:
- one Galactic Conquest opening of N fiscal cycles ("galactic days");
- one space skirmish of M ticks.

The same kit works with Thrawn's Revenge (`-Mod tr`).

## How it works

`q2reader.exe` is a small read-only recorder.
- It opens the running `StarWarsG.exe` with read access and reads the statics documented in `docs/re/` once per
  logic tick:
  - the frame counters;
  - the synchronised RNG state;
  - the session seed;
  - every object in the active mode's object store;
  - planet state;
  - player credits.
- It writes nothing into the game, injects nothing and suspends nothing.
- It checks the exe build (PE timestamp and size) and refuses any other build.
- It ends the trace by itself and then closes the game.

**Seed: captured, not forced.** Forcing a seed would mean writing into the game's memory during the load, so
the reader records the seed slot (GameModeManager +0xE0) and the RNG state at every tick instead. The new
engine replays from the recorded state.

The first real run (2026-10-27, Galactic Conquest from the menu, first session) found seed slot 0 and RNG
state 0 at frame 0, and every later state is reachable from 0. This start path never writes a seed, so the run
is deterministic from seed 0. The QPC seeding of MAIN_LOOP.md §4.1 is on other paths. **Skirmish is seeded** (2026-10-30 run
`20261030-062751-skirmish-vanilla-lua`): seed slot 0x41ccdfe4, RNG 0x2ec48706 at mode frame 0, 370 draws between
seeding and the first traced tick (`summary.json` → `draws_seed_to_first_sample`). In that skirmish the per-object
CRC cache (+0x2C8) *is* filled and equals the reader's `crc_calc` for all 424,430 object samples; galactic leaves it 0.

**Main-menu backdrop.** The main menu runs its own space battle (game type 3, mode type 2, no human player), which
looks like a skirmish session. Reader 2 starts a skirmish trace only once PlayerList holds a player vector with a
human, so the backdrop is skipped (the first 2026-10-30 attempt traced the backdrop before this gate).

**Single player computes no frame CRC.** The object CRC cache (+0x2C8) was always 0, and the FrameSynchronizer
CRC ring was never written. The reader's own `crc_calc` / `sum_calc` are the parity numbers.

**Hands-off runs.** Nobody issues orders once the battle or galaxy is running:
- the AI plays;
- the human side only reacts automatically.

The command stream is therefore empty, and seed + setup fully determine the run (COMMANDS.md §7.3).

**Why not Lua for the main trace?** The original Lua cannot:
- see the RNG or the seed;
- run every tick;
- read positions in bulk.

Nor can it write a file: in retail 1.121 `_CustomScriptMessage`, `_ScriptMessage` and `_OuputDebug` write nothing
(docs/re/LUA_SCRIPT_OUTPUT.md). An optional Lua probe (`-LuaProbe`, vanilla only) adds what only Lua can give:
- galactic: player credits through the game's own API, planet owners and AI perception values;
- skirmish: the game's own `GameScoring.lua` (copied unchanged from `extracted/foc_data_text.zip`) with a
  read-only probe appended. It logs `TMODE` / `TKILL` / `TBUILT` from the scoring events and, at most once per game
  second, `TSAMPLE` plus a `TPLAYER faction= id= credits= tech= objects=` line per player (taken on those events and
  from a `Create_Thread` loop: retail never calls GameScoring's `GameService`). The original `GameService` would
  still run every 10th service.
- AI plans (q2f t09 2026-11-02, rewritten in q2f t08 2026-11-08): unchanged copies of the shipped
  `BuildSpaceForcesPlan.lua`, `BuildGroundForcesPlan.lua` and `ConquerPiratePlan.lua` (from
  `extracted/foc_data_text.zip`) with a read-only block appended. Records (src `P`, one sid per pooled script
  instance): `PSTART` when the task-force thread starts (`n` = run number in the instance, `prev` = how the previous
  run ended; `none` = its thread was killed when the plan was released), `PBLOCK` after each `BlockOnCommand` returns
  (after `Produce_Force`: the units just built), `PDONE` when the thread function returns, `PEXIT` on `ScriptExit`,
  and for Conquer_Pirate `PFAIL` per production failure (the names of the handler's arguments). Each carries
  galactic time, frame, plan, faction, target, the goal's budget (`spend=` / `unalloc=` from `Budget`), the player's
  credits and the task force's unit types. It shows which units retail's Build_Space_Forces / Build_Ground_Forces
  pick, when, and why Conquer_Pirate plans fail.
  **Extended 2026-11-08** (which plan builds the Rebel's post-Shola infantry): also `ConquerOpponentPlan.lua` (two
  threads, `SpaceForce` and `GroundForce`) and `CrushPlan.lua` (`MainForce`; its category is Conquer_Opponent too, so
  its records say `plan=Crush`). Every record now carries `target=` (`Target.Get_Type().Get_Name()`, the goal's planet;
  `none` = no target; the old accessor read a method game objects do not have, hence the old `target=?`), `tf=` (the
  task force; with two threads told apart by `GetThreadID()`, `tf=?` when unknown, and then `count=` lists every task
  force) and `count=` (`<tf>.Get_Force_Count()`: `units=0` was all retail showed at PBLOCK). `PSTART` adds `tname=`
  (`Target.Get_Name()`). New record `PSTAGE` when `AssembleForce` / `SynchronizedAssemble` is called, before it
  produces: `via=`, `strongest=` (its second argument), `stage=` (planet of `<tf>.Get_Stage()`; `nil` = no stage, the
  plan then exits) and `stagestr=` (`tostring` of it). `PFAIL` names the event's first argument `tf` (the task force,
  which has no `Get_Name`). The seven files are written by `make_plan_hooks.py` (shipped script + generated block);
  edit the generator, not the files. `lua_summary.json` → `plans` sums them per `player plan tf`: starts, stagings,
  blocks, first frame, targets, stages and the unit types the task force held.
  **Also 2026-11-08:** `ContrastToReconnect.lua` (Conquer_To_Reconnect, `SpaceForce` / `GroundForce`, both staged by
  `SynchronizedAssemble`: the likely plan of retail's Shola attack at ticks 6941-7498) and `LiftBlockadePlan.lua`
  (Lift_Blockade, `MainForce`), hooked the same way. The galactic script adds `GOALPERC` rows each cycle, after the
  other rows: the Rebel's `Should_Conquer_Pirate_Planet`, `Is_Good_Invasion_Target`, `Need_To_Reconnect_Islands` and
  `First_Attack_Allowed` against Shola and Naboo (control), and `Initial_Grace_Period` without a target
  (`planet=none`), each named once in an `EVAL` record in the first sample. A value `nil` is a failed evaluation; as
  a binding error it also ends the galactic script after that service pass. Written to `lua_goal_perception.tsv`.
  **The 2026-11-03 run's plan hooks broke the plans.** Releasing a pooled plan runs PGBase `Flush_G`, which deletes
  every global table, the old probe's records table included; the next record then raised a Lua error that killed
  the plan's thread before the shipped code ran (Rebel fleet departures 19 instead of retail's 71). The records now
  live in a closure, and every probe step is in `pcall` (docs/re/LUA_SCRIPT_OUTPUT.md §4). The galactic and
  GameScoring scripts no longer use the `math` library, which retail lacks (`math.floor` ended the galactic script at
  its first `OnUpdate`). `selftest_lua.py` hides `math` and runs each plan twice across an emulated `Flush_G`.

**Lua output channel (q2f t06, 2026-11-02).** The probe scripts keep every line as a Lua string
`Q2L1|<G, T or P>|<sid>|<seq>|<KIND>|<text>|END` (G = galactic plot, T = GameScoring, P = AI plan hooks) in a table. `q2lua_reader.py`
(started by the run, below-normal priority) scans the game's memory for them every 10 s, read-only, like the A1 / TSW
probes. At the end it writes `lua_records.txt`, `lua_perception.tsv` (cycle, AI, planet, one column per equation) and
`lua_summary.json` (verdict per script). Separately, each script calls `_CustomScriptMessage` once on a canary file
shipped in the mod (`Mods\Q2Trace\Q2Trace_Canary_Galactic.txt`, `..._Scoring.txt`). Retail deletes a file on the
first `_CustomScriptMessage` call for that name, so a canary that is gone after the run proves that script ran
(`run_status.json` → `lua_canary`), even if the memory scan found nothing.

**Game folder as it is now (2026-10-30).** The prior patch chain (d3dx9_43 proxy, `eaw_*` files) and DXVK are
no longer in `corruption\`, and runs do not restore them. Every run records the folder state at start and end:
trace meta `game_folder`, and `run_status.json` → `game_folder_before` / `game_folder_after` (`patch_chain=absent`
plus size and hash of any d3d9/dxgi/d3dx9_43/dinput8/version dll). A change during the run is reported as an error.

**Player profile.** The game rewrites its registry settings (`HKCU\Software\Petroglyph\StarWars FOC`, e.g.
`LastScenarioMap`) and may add files under `Saved Games\Petroglyph\Empire At War - Forces of Corruption`.
`q2_profile.py` snapshots both before the game starts and restores them after cleanup. New files are moved into
`out\<run>\profile_new\`. The result is in `profile_restore.json` and `run_status.json` → `profile_restore`.
A run whose restore does not verify exits 40.

## Steps for Gabriel (manual run)

Each run takes about 15 minutes. Start Steam first, and do not touch the game window while the battle or the
galaxy is running.

1. **One time only:** double-click `build_reader.cmd`, then `selftest.cmd`. The self-test should end with
   `SELFTEST OK`. It does not need the game.
2. **Galactic run, about 12 min:** double-click `run_galactic.cmd`. The game starts in a window. Follow
   **UI steps → Galactic** below, then leave the game alone. The game closes by itself after 10 galactic days
   (7.5 min of game time).
3. **Galactic run with the Lua probe, about 12 min:** double-click `run_lua_probe.cmd` (the same as
   `run_galactic.cmd -LuaProbe`). Use the same clicks as step 2. Details and what to check: **Lua probe run** below.
4. **Skirmish run, about 6 min:** double-click `run_skirmish.cmd`. Follow **UI steps → Skirmish**. It stops
   after 5400 ticks (3 min of game time).
5. **Optional, Thrawn's Revenge:** run `run_galactic.cmd -Mod tr` and `run_skirmish.cmd -Mod tr`. Pick the
   smallest GC map and type its name with `-Label "<map>"`.
6. **Optional, land skirmish (skai t04), about 8 min:** `run_skirmish_land.cmd`. The menu driver knows only Geonosis, so
   pick the map by hand as the script header says (Land, Utapau, you = Empire, slot 2 Rebel Medium AI, Advanced Default),
   then leave the game alone. It settles the land items of `docs/re/SKIRMISH_SETUP.md` §10 (pre-built pads, AI credit
   charges, start units, pad capture).
   **Long land run (skai t02), about 22 min:** `run_skirmish_land_attack.cmd`, the same clicks, 36000 ticks (20 min of game
   time) with the Lua probe. It shows whether and when the Rebel AI attacks the idle Empire base: its army size over time
   (is 9 the real 2-player cap), the first tick its units reach the base and which base objects die. Read it with
   `analysis\land_attack.py out\<run>`.
   **Story mission (story t04), about 15 min:** `run_story.cmd`, see **Story mission run** below.
7. **Send back:** zip `out\` and send it. Leave out `out\selftest`. Each run has its own folder:
   - `trace.q2t`;
   - `summary.json`;
   - `reference.crc`;
   - `run_status.json`;
   - `cleanup.log`;
   - with the Lua probe: `lua_summary.json`, `lua_records.txt`, `lua_perception.tsv`, `lua_goal_perception.tsv`, `lua_reader.log`.

### Story mission run (story t04): steps for Gabriel

Goal: a reference trace of one story mission, for roadmap `story` t04 (none of the earlier traces has one: they are the
Heart of the Maw sandbox galaxy and skirmishes). The reader's `story` scenario (2026-11-03) traces the tactical battle a story
campaign starts as a child of its galaxy (mode stack 2): it starts when the battle has loaded and stops when the battle ends
(`reason battle_ended`, a complete trace) or after 18000 ticks (10 min of game time). Nothing is installed in the game folder.

1. Start Steam. Close the game if it is running.
2. Double-click **`run_story.cmd`** in `tools\probe_mod\q2_traces\`. It rebuilds `q2reader.exe` (needs `toolchain\`), then
   launches the game **windowed** at 1920x1080. Leave the black command window open.
3. In the game: Esc through the intro movies, then **Single Player** → **Campaigns** (Forces of Corruption) →
   **Underworld** → difficulty **Medium** (the only choice offered) → **Start**. Esc through the opening movie. The
   campaign starts its first mission at once: the Kessel land battle (Tyber Zann's escape, `STORY_UNDERWORLD_M01_LAND.XML`).
4. When the battle has loaded (the intro cinematic plays), click nothing and press no keys, **not even Esc**: the
   battle's own intro cinematic must play out. An Esc there runs the mission's `Story_Handle_Esc` and changes the sim on
   that frame (2026-11-08 runs: cleanup at battle frame 129 and 248, identical before), so the run is no longer
   hands-off. Without orders Tyber stays in his cell, so the mission does not finish: the trace stops after 10 minutes
   of game time and the game closes by itself. If a dialog asks something, do not answer it. Keep the game window in
   front: the game pauses when it loses focus (the 2026-11-08 22:41 run stopped at frame 1286).
5. The command window ends with `done: exit <n>` (0 = complete). Send back
   `tools\probe_mod\q2_traces\out\<yyyymmdd-hhmmss>-story-vanilla\` (zipped): `trace.q2t`, `summary.json`,
   `run_status.json`, `cleanup.log`.

If the window was closed early or the PC crashed, double-click `cleanup.cmd`. What the trace is compared with: the port's
Full_Story_Campaign_Underworld linked Kessel battle over the same ticks (RNG draw counts, object counts by type and owner,
the intro spawns of the M01 Lua, the victory monitor staying held: `story` t04 on the roadmap). Read a run with
`analysis\story_battle.py out\<run> [--port-trace port.q2t]`; the first run and its comparison with the port:
`docs/re/STORY_T04_KESSEL_RUN.md`.

### Lua probe run (q2f t06): steps for Gabriel

Goal: the AI's galactic perception values (`EnemyForce.*`, `TargetPlayerID`) read through the game's own
`EvaluatePerception`, per AI player, planet and fiscal cycle. One galactic run, about 12 minutes.

1. Start Steam. Close the game if it is running. One time only: double-click `selftest.cmd` and wait for
   `SELFTEST OK` (it also runs the probe scripts under Lua 5.0.2; no game needed).
2. Double-click **`run_lua_probe.cmd`** in `tools\probe_mod\q2_traces\`. It installs `corruption\Mods\Q2Trace`,
   starts the two readers and launches the game **windowed** at 1920x1080 (`MODPATH=Mods\Q2Trace WINDOWED
   SCREENWIDTH=1920 SCREENHEIGHT=1080`). Leave the black command window open.
3. In the game: Esc through the intro movies, then **Single Player** → **Galactic Conquest** → **Heart of the
   Maw** → **Empire** → leave every option at its default → **Start**. If the "Active Story Objectives" dialog
   appears, click **Close**.
4. Then click nothing and press no keys. Let the galaxy run: after 10 galactic days (about 7.5 minutes) the game
   closes by itself. If a battle dialog pops up ("Begin" / "Auto-resolve"), do not answer it; the trace stops after
   60 s on its own (the Lua data up to then is kept).
5. The command window prints `lua canary: ...` and `lua: galactic ...; scoring ...` and ends with
   `done: exit <n>`. Send back the run folder `tools\probe_mod\q2_traces\out\<yyyymmdd-hhmmss>-galactic-vanilla-lua\`
   (zipped). The files that matter for t06: `lua_summary.json`, `lua_perception.tsv`, `lua_records.txt`,
   `lua_reader.log`, `run_status.json`.

If the window was closed early or the PC crashed, double-click `cleanup.cmd`.

How to read the result (`run_status.json` → `lua_canary` and `lua`):

| canary galactic | `lua.galactic` | meaning |
|---|---|---|
| deleted | `ran: N cycles, M perception rows, G goal rows` | works; the values are in `lua_perception.tsv` and `lua_goal_perception.tsv` |
| deleted | `ran, but no perception rows` / `ERR` records | script runs; `EvaluatePerception` or `Get_All_Planets` failed (see `errors`). If only the `Q2_*` columns read `ERR` and `Roaming_Empire_Fleet_Target` has values, the mod's equation file was not picked up |
| deleted | `no records` | script ran, the memory scan missed it: check the `SCAN` lines in `lua_reader.log` |
| present | `no records` | the plot script never loaded: the override of `Story_Plots_Sandbox_12U_Empire.xml` is not used |

The `scoring` canary and verdict are about the GameScoring hook; in a galactic run it loads too, but only a skirmish
(`run_skirmish.cmd -LuaProbe`, same clicks as step 4 above) gives it battle events. Optional.

`lua.plans` (q2f t09) reads `loaded: N plan scripts; S thread starts, D ends, F production failures; G stagings,
B blocks done, E script exits, R probe errors`. `lua_summary.json` `gaps` (and the `GAPS` line of `lua_reader.log`) must be empty:
a gap means records were lost. What it must show for q2f t09 (docs/re/Q2F_T09_REBEL_PRODUCTION.md): the Rebel's
first `Build_Space_Forces` `PBLOCK` (unit types; the port picks a Marauder where retail's queue starts a Corellian
gunboat at frame 109), the first `Build_Ground_Forces` `PSTART` / `PBLOCK` (retail's queue shows light tank, tank
buster and infantry by frame 559), and every `Conquer_Pirate` start and failure with its target. Check also that the
Rebel's fleet departures and queue starts in `parity\parity_report.txt` are close to the run without plan hooks
(71 / 50): if not, the probe still disturbs the AI.

**Cleanup is automatic.** It runs at the end of every run, even after an error. If a window was closed
early or the PC crashed, double-click `cleanup.cmd`. It is safe to run any number of times.
- With `-DryRun` it only shows what it would do.
- Without `-LuaProbe`, nothing is ever copied into the game folder.

## Unattended runs (Claude driving the menus)

**`-NonInteractive` runs need no clicks.** The run starts `q2_ui.py drive <scenario>` after the game launches
(`-NoDrive` turns it off, `-Drive` turns it on for interactive runs). The driver:
- imports `g1_capture\g1_ui.py` (read-only) and sends window messages only (PostMessage, PrintWindow screenshots);
- follows the steps in `q2_scenes.json` and checks every screen against a reference crop (`refs\*.png`, plus the
  g1 crops) before it clicks. If a check fails it exits 3, and the run stops the reader and cleans up;
- writes `ui_status.json` and screenshots into `out\<run>\shots\`;
- exits as soon as the galaxy / battle runs, so the traced session gets no player input.

Galactic: Heart of the Maw, Medium, Empire, Advanced -> Default (10000 credits, tech 3, max tech 5), Start, then
Close on the "Active Story Objectives" dialog. Skirmish: Space, (2) Battle Over Geonosis, slot 2 Medium AI
Rebellion, Advanced -> Default (6000 credits, heroes, free units and superweapons on, win = destroy the enemy
space station), Start, then Begin on the loading screen.

The driver needs the game at 1920x1080 windowed (the run passes `WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080`),
the English FoC 1.121 menus, and the player name `Gabriel` in skirmish slot 1 (crop `sk_slots`). It knows the
vanilla menus only, so `-Mod tr` needs `-NoDrive`. `python -I q2_ui.py shot|click|key|like|info|crop` helps to
recalibrate.

The manual instructions below are for runs without the driver.

Every script accepts these options:
- `-NonInteractive`: no prompts;
- `-DryRun`: prints every action and changes nothing.

Results:
- the status of a run is in `out\<run>\run_status.json`;
- while the trace runs, the live state is in `out\<run>\reader_status.json`. It is refreshed every 2 s with
  `state`, `frames_sampled`, `mode_frame`, `cycles_done` and `galactic_pause`.

Exit codes:

| code | meaning |
|---|---|
| 0 | complete |
| 10 | partial trace; `reason` in the status file |
| 20 | preflight failed; nothing changed |
| 30 | no trace |
| 40 | cleanup left something |

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe_mod\q2_traces\run_trace.ps1 -Scenario galactic -NonInteractive
powershell ... run_trace.ps1 -Scenario galactic -LuaProbe -NonInteractive
powershell ... run_trace.ps1 -Scenario skirmish -NonInteractive -Label "<map name>"
```

Run the command in the background. Then drive the UI with computer use until `reader_status.json` shows
`"state": "tracing"`, and stop touching the game.

### What is automatic

| automatic | needs UI driving |
|---|---|
| preflight: game found through the Steam registry and library list; exe SHA-256 = PE_OVERVIEW.md; Steam running; game not already running | main menu → scenario setup → start (no command-line or Lua path is known that starts a GC or a skirmish; see "Open items") |
| mod install + manifest (Lua probe only) | skipping the intro movies (Esc) |
| game launch (`WINDOWED`, mod arguments) | |
| trace start when the session starts, trace stop after N cycles / M ticks | |
| timeout (`-WaitSessionMin 20` for menus, `-TimeoutMin 30` for the trace), game closed by the reader (WM_CLOSE, then terminate after 20 s) | |
| cleanup always, then validate and Q1 conversion | |

**AI vs AI is not possible in a FoC skirmish.** The local player always holds a slot and there is no spectator
slot. The skirmish is therefore human (idle) vs AI. The human sends no commands, but the human's units still
defend themselves through their own behaviours. That is simulation, and the new engine must match it.

### UI steps

The labels below are from FoC 1.121 as remembered. Take a screenshot at each step and match on the shown
text. If a label differs, pick the obvious equivalent and write it in `-Label`.

Galactic (Heart of the Maw, Empire):

| # | screen | click | expect next |
|---|---|---|---|
| 1 | Logo / intro movies | press **Esc** (repeat until the menu appears) | Main menu |
| 2 | Main menu | **Single Player** | Single-player menu |
| 3 | Single-player menu | **Galactic Conquest** | GC map list |
| 4 | GC map list | **Heart of the Maw** (sort order 1, normally the first entry) | Faction choice for that map |
| 5 | Faction choice | **Empire** | Settings (difficulty and similar) or straight to start |
| 6 | Settings, if shown | leave every option at its default (AI difficulty **Normal**); click **Start** (or Play / Begin) | Loading screen, then the galaxy |
| 7 | Galaxy | **click nothing** and do not press keys. If a story/advisor pop-up pauses the galaxy, close it with its **OK/Close** button only; `reader_status.json` shows `galactic_pause` > 0 while paused | trace runs; the game closes by itself |

If the AI attacks an Empire planet, a battle dialog pauses the galaxy ("Begin" / "Auto-resolve"). **Do not
answer it.** After 60 s of pause the reader ends the trace with reason `paused_too_long` (exit 10, a partial
trace is kept). A tactical battle that starts ends the trace with reason `mode_changed`.

Skirmish (space, 2 players):

| # | screen | click | expect next |
|---|---|---|---|
| 1 | Logo / intro movies | **Esc** | Main menu |
| 2 | Main menu | **Single Player** | Single-player menu |
| 3 | Single-player menu | **Skirmish** | Skirmish setup |
| 4 | Skirmish setup | map type **Space**. Map: the first 2-player space map in the list (pass its name as `-Label`). Slot 1 (you): **Empire**. Slot 2: **AI, Normal**, **Rebel**. All other settings default; extra slots **Closed** | — |
| 5 | Skirmish setup | **Start** (or Launch) | Loading screen, then the battle |
| 6 | Battle | **click nothing** (a click in the battle is a Select command); do not press keys | trace runs 5400 ticks; the game closes by itself |

## Files

The per-metric `q2trace.py compare <run dir>` report is superseded for Q4 by the behavioural parity harness
`tools/parity/` (q4h, 2026-11-07: tolerances per metric, retail run-to-run envelope, skirmish scenario, baseline ctest).
It imports `analysis/q2_parity.py`, so keep that module's functions stable.

| file | what |
|---|---|
| `run_galactic.cmd`, `run_skirmish.cmd` → `run_trace.ps1` | one recorded run: preflight, optional mod install, reader + game start, wait, cleanup, validate, Q1 conversion |
| `cleanup.cmd` → `cleanup.ps1` | undo per manifest: stop the reader/game it started (pid + start time), move Lua output into the run folder, delete only files it created and whose SHA-256 still matches, remove created folders if empty, verify |
| `selftest.cmd` → `selftest.ps1` | no game needed: hash vectors vs engine/core, reader vs `fake_game.exe`, install/cleanup on scratch fake game folders (dry run, twice, modified file, no-Mods case, canary files), preflight refusal, `q2lua_reader.py selftest`, `selftest_lua.py` |
| `build_reader.cmd` | builds `bin\q2reader.exe` and `bin\fake_game.exe` with the project-local MSVC (`/W4 /WX`). Log in `logs\q2reader_build.txt` |
| `reader\q2_layout.hpp` | every address and offset used, each with its `docs/re` source |
| `reader\q2reader.cpp`, `reader\fake_game.cpp` | recorder; test target that lays out the same structures, including a real MSVC `unordered_map` |
| `q2trace.py` | `validate` (rebuild state, recompute every digest, RNG draws per tick, fiscal cycles, credits, ownership), `to-q1`, `compare`, `selftest`, `selftest-fake`. Standard library only (project venv) |
| `mod\Q2Trace\` | optional Lua probe (vanilla): plot Lua script, `Story_Plots_Sandbox_12U_Empire.xml` with its empty `Lua_Script` set, the galactic equation file plus 4 `Q2_*` equations, `Scripts\Miscellaneous\GameScoring.lua` (original + tactical probe), `Scripts\AI\` copies of seven plan scripts (original + plan probe, q2f t09; Conquer_Opponent, Crush, Conquer_To_Reconnect and Lift_Blockade added 2026-11-08), and the two canary files |
| `make_plan_hooks.py` | writes the seven `Scripts\AI\` plan hooks from `extracted\foc_data_text.zip` (shipped script, CRLF → LF, + the generated probe block); `--check` compares only (`selftest_lua.py` runs it) |
| `run_lua_probe.cmd` | `run_trace.ps1 -Scenario galactic -LuaProbe` |
| `q2lua_reader.py` | `run` (started by the run: read-only memory scan for the probe's `Q2L1` records), `summarize` (rebuild the outputs from `lua_reader.log`), `selftest` |
| `selftest_lua.py` | runs the probe scripts under stock Lua 5.0.2 (`tools\probe_mod\out\tools\lua50.exe`, built by `common\build_lua50.cmd`) with stubbed engine calls, and checks that a memory scan of that process finds every record. Conquer_Opponent, Crush, Conquer_To_Reconnect and Lift_Blockade run as real coroutines with PGBase's yielding `BlockOnCommand` / `Sleep` / `ScriptExit` (a probe `pcall` around a yield would fail; the two-thread plans interleave) |
| `q2_ui.py`, `q2_scenes.json`, `refs\` | unattended menu driver (see "Unattended runs") |
| `q2_profile.py` | `snapshot` / `restore` / `diff <run_dir>`: the player's registry settings and saved-games files |
| `q2common.ps1` | shared paths, game detection, manifest, install |

## Trace format (`trace.q2t`, "Q2TRACE 1")

The file is ASCII text with LF line endings and one record per line. Fields are separated by spaces.
- Floats are written as 8 hex digits of their IEEE bits.
- CRCs and RNG states are hex.
- Lists are sorted by object id.
- Wall-clock data never appears in the trace (it goes to the status JSON only), so the same game state always
  produces the same bytes.

Header, up to `end_header`:

```
Q2TRACE 1
meta <key> <value...>          reader, scenario, exe_timestamp, exe_size_of_image, game, mod, mod_args, exe_sha256,
                               lua_probe, campaign | skirmish, label, game_type, mode_type, target_cycles/day_frames
                               or target_ticks, key_every, player_vector_offset, game_folder, menu_driver
seed <seed> <alt_seed> <use_alt>   GameModeManager +0xE0/+0xE4/+0xE8 (the session seed)
load <global_frame> <rng>      first sight of the mode stack while loading (optional)
end_header
start <global_frame> <mode_type> <mode_frame> <rng>
```

Per sampled tick, in this order:

```
F <global_frame> <mode_type> <mode_frame> <sync_rng> <galactic_pause> <stack_count> <flags> [<last_cycle_frame> <next_cycle_frame> <cycle>]
S <global_frame> <n_live> <sum_cached_crc> <sum_calc_crc> <rng_crc> <ref_rng:16> <ref_objects:16> <ring:24>
K <global_frame>               keyframe: every object/planet/player line follows, not only changed ones
O <id> <owner> <type_index> <px> <py> <pz> <fx> <fy> <fz> <health> <flags> <parent_mode_id> <container_id> <attach_id> <crc_cached> <crc_calc>
G <planet_id> <political_control> <base_value> <black_market_mult> <piracy_pool> <corruption_type> <destroyed> <siphoner>
C <player_index> <credits> <credit_cap> <is_human> <has_ai>
X <id>                          object left the store
P <global_frame> <galactic_pause>   pause counter changed
M <key> <value...>              late meta (e.g. player_vector found after the header)
```

Field notes:
- **flags** bit 0 = torn: the frame advanced while it was being read. Bit 1 = the in-tick byte did not clear
  within 30 ms. Bit 2 = light sample: no object read on this tick, so only `F` and `C` lines and no `S` line.
  The reader takes a light sample on the tick after an object read slower than `--object-budget-ms` (12),
  so a slow machine loses object detail rather than whole ticks.
- **Player vector:** found in the game on 2026-10-30 (read-only): a `std::vector<Player*>` {begin, end, cap} at
  PlayerList +0x00 (RVA 0xA16FD0), in galactic (8 players) and in skirmish (7 players). Each player has its list
  index at +0x48, which the reader checks; credits f32 +0x70, cap +0x74, is_human +0x62, AI pointer +0x360
  (0 for the human). Reader 2 still searches (directly or one pointer hop away, as DynamicVector or
  std::vector) and writes where it found the vector (`M player_vector ...` / status `player_vector`). It writes `playerlist_dump.txt` (PlayerList and two pointer levels) into the
  run folder at session start and end, for offline RE if the search fails.
- **Delta encoding:** `O`, `G` and `C` lines appear only when their content changed, except on keyframes (every
  `key_every` mode frames and on the first sample).
- **Dead objects** (flag 0x02) are listed but left out of the sums and digests.
- **Missed ticks** show as gaps in `global_frame`. The RNG draw count across a gap is still recovered.

Footer:

```
T <type_index> <name_crc32> <engine_name_crc> <NAME>      every type seen (zlib CRC-32 of the upper-case name)
E <reason>            cycles_reached | frames_reached | ticks_reached | mode_changed | paused_too_long | game_exited | timeout | layout_error
end
```

## Production queues (`production.q2q`, "Q2QUEUES 1", galactic, reader 2)

A side file, so `trace.q2t` keeps format 1. Layout from GALACTIC_ECONOMY.md §2.1, confirmed live on 2026-10-30:
the planet object +0xD0 holds the production behaviour. Queue 0 (ground) is at +0x10/+0x18 (data, count) and
queue 1 (space) at +0x28/+0x30. Each entry is a 0x38-byte block.

```
Q2QUEUES 1
Q <global_frame> <mode_frame> <planet_id> <queue> <n> [<type_index>:<player>:<start_frame>:<frames>:<T_f32>:<remain_f32>:<cost_f32>:<tag>:<planet>:<created_id>]...
E <reason>
end
```

A `Q` line is written when a (planet, queue) line changes, and for every planet and queue on keyframes. Type
indices are the trace's `T` lines; floats are hex bits, as in the trace. Example seen: REBEL_GROUND_MINING_FACILITY
T=22.8 frames=684 cost=338.

## Comparison contract (Q1)

`q2trace.py to-q1 trace.q2t reference.crc` writes an `ALAMO_CRC 1` log (engine/core `sim_crc.hpp`) with:
- tick = mode_frame − 1, which is the engine's "record after tick t" convention;
- systems `ref_rng` and `ref_objects`;
- parity parts `objects` and `rng`.

`q2trace.py compare reference.crc engine.crc` checks the engine log on the reference ticks only.

For a match, the engine registers two systems with exactly these names and hashers, each a fresh
`StateHasher`. The reader computes both with a copy of that algorithm, which was checked against engine/core
on 2026-10-27.

| system | hasher |
|---|---|
| `ref_rng` | `u32(synced RNG state)` |
| `ref_objects` | `u32(live object count)`, then for each live object of the active mode's sync store, ascending id: `i32 id, i32 owner, u32 crc32_zlib(UPPER type name), f32 pos.x,y,z, f32 facing.x,y,z, f32 health` (bit patterns) |

Parity is the retail-shaped number without the scoring and player terms (still [L]):
- parts: `objects` = Σ `parity_object_crc`; `rng` = `parity_rng_crc`;
- options: `ParityFrameOptions` defaults (invert only).

**Seed the engine with the `seed` line.** `summary.json` → `draws_seed_to_first_sample` gives how many draws the
original made between seeding and the first traced tick (world setup).

## Coverage of the requested traces

GALACTIC_ECONOMY.md §7 items and the other requested data:

| # / data | covered? | how |
|---|---|---|
| credits per player per fiscal cycle | **yes** | `C` lines every tick (player vector at PlayerList +0x00, found 2026-10-30; `M player_vector` line). `summary.json` → `credits_by_cycle`. The Lua cross-check of 2026-10-30 wrote nothing (see Known limits); the probe now has `PLAYER ... credits=` records |
| 1 initial `next_cycle_frame` | **yes** | `F` carries mode +0x368/+0x36C/+0x370 from the first galactic tick; credits per tick. Save/load part: **no** (needs a manual mid-cycle save/load) |
| 2 black-market LCG = synced RNG? | **partly** | the address *is* the synced RNG (MAIN_LOOP.md). The trace gives draws per tick, so the cycle frames show the extra draws. Heart of the Maw includes Nal Hutta, which has `Planet_Black_Market_Influences` |
| per-planet income terms | **partly** | `G`: political control, runtime base value, black-market multiplier, piracy pool, corruption type, destroyed, siphoner. The income breakdown list (D +0x268) layout is undocumented, so the terms are predicted offline and checked against credit deltas |
| 3 fleet speed M / fleet travel frames per route | **partly** | every position change of every galactic object (fleets included) is in the deltas, which gives exact departure/arrival frames for AI fleet hops. Choosing a known fleet and route needs player input: **no** |
| ownership / political-control ramp | **yes** (when it happens) | owner in `O`, control in `G` every tick. `summary.json` → `planet_owner_changes`, `political_control_samples`. Only captures made by the AI in the first N days |
| build queue timings | **yes** (reader 2) | `production.q2q`: every queue entry with start frame, build frames, T, remaining time (counts down every tick, so active queues write one line per tick), cost and the created object id (0x3FFFFF until the object exists). Refunds: **no** (needs a player cancel) |
| 4 upkeep | **partly** | credits per cycle vs income predicted from `G`. No large-fleet setup |
| 5 refunds, 6 ground-cap stall, 7 carrier fill, 12 tech gate | **no** | each needs deliberate player or Lua actions that would change the game being recorded. They belong in a separate, non-hands-off probe |
| 8 what transfers on capture, 9 orbital-control capture | **opportunistic** | object deltas list everything created and destroyed if the AI captures a planet in the window |
| 10 strategic game speed | **no** in the default runs | needs the speed slider (UI). Repeat the galactic run at slider 0 and 4; wall seconds per cycle are in `run_status.json` |
| 11 perception (`EnemyForce.*`, `TargetPlayerID`) | **with `-LuaProbe`, not yet captured** | `EvaluatePerception` of the 4 `Q2_*` equations (+ the shipped `Roaming_Empire_Fleet_Target` as a control) for Rebel and Underworld × every planet × every cycle, with planet owners → `lua_perception.tsv`. Kept out of the baseline run because evaluating equations may touch AI state |

## Known limits

- **Exe build:** only FoC Steam x64 1.121 (`sha256 07daee…`). Any other build is refused.
- **Game exits at once (2026-11-08):** if no window appears, `ui_status.json` stays `waiting_window` and the Lua reader
  ends with `no_game`, StarWarsG.exe quit right after launch (exit code 0, no crash log). Seen when Steam's
  `HKCU\Software\Valve\Steam\ActiveProcess` `pid` names an old Steam process: steam_api then reports
  `SteamAPI_IsSteamRunning() failed` (OutputDebugString only) and the game ends. Compare that pid with the running
  `steam.exe`; restarting Steam (`steam.exe -shutdown`, then start it) fixes it. `steam -applaunch 32470` is no
  workaround: it starts base EaW (`GameData`), not FoC.
- **Sampling:** the reader polls about every 1 ms, at above-normal priority in its own process. A tick it
  misses leaves a gap in `global_frame`; the RNG draw count across the gap is still recovered. Torn samples are
  flagged and left out of `reference.crc`. The first real run missed 4% of ticks under heavy machine load, in
  bursts of 100–330 ms stalls. The light samples and the priority are aimed at that.
  `reader_status.json` reports `object_read_ms_avg` / `_max` and `light_samples`.
- **Not captured:**
  - the player-list CRC term and the scoring CRC (both [L] in COMMANDS.md);
  - the secondary (cosmetic) object store;
  - behaviour or data-pack internals other than the planet fields and the production queues;
  - AI internals.
- **Frame CRC:** single player does not compute it (confirmed by the first run; `summary.json` → `notes`).
- **[L] / [M] assumptions:**
  - the offsets used are [H]/[M] in the docs;
  - two are [L]: the player vector (found heuristically) and the FS ring entry size;
  - a wrong ID-map layout is detected on the first sample (exit 4, `layout_error`).
- **Lua output path:** the 2026-10-30 `-LuaProbe` runs (galactic `20261030-061228-galactic-vanilla-lua`, skirmish
  `20261030-062751-skirmish-vanilla-lua`) wrote no `Q2Trace_Lua.txt` anywhere. Cause (2026-11-02, q2f t06,
  docs/re/LUA_SCRIPT_OUTPUT.md): in 1.121 the file writer behind `_CustomScriptMessage` is an empty function, so no
  Lua probe can write a file. Whether those scripts loaded is not recorded. Since then the probe keeps its lines in
  memory for `q2lua_reader.py` and proves its load with the canary files. Not yet run against the game.
  The `C` lines and `production.q2q` cover credits and queues without Lua.
- **Thrawn's Revenge:**
  - the default launch is `STEAMMOD=1125571106` (the workshop folder is checked);
  - its GC map and UI labels differ, so name them with `-Label`;
  - the Lua probe is vanilla-only.

### Open items

1. A non-UI way to start a scenario. MAIN_LOOP.md §5.1 lists quick-load map/recording paths (0x68b20, 0x69040)
   and a command-line save path, but not their triggers. With those, the runs would need no clicks at all.
2. ~~The PlayerList vector offset~~ found (PlayerList +0x00 std::vector, 2026-10-30).
3. ~~The production-queue layout~~ recorded in `production.q2q`. The income-breakdown list layout is still open.
