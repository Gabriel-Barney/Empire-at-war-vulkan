# cu8h probes: synced draw order of a map load (debugger), lightsaber hum / Saber_Throw (memory reader)

These are the retail checks left over from cu8g, for roadmap group cu8h:
- **t01:** `docs/re/CU8G_T02_ENV_PICK_RE.md` §7 P1.
- **t02:** `docs/re/CU8F_T01_T02_T07_RE.md` §2.5.

Both were run on 2026-11-08, with Claude driving the game. The findings are in those specs (§7 P1 results and §2.6),
not here. Rules for every run: one game at a time, give no orders unless a step asks for one, never pause, never
change the game speed, never save. Steam must be running.

| Command | Answers | Time |
|---|---|---|
| `run_cu8h_trace.cmd` | t01: the synced draws of one battle load, traced by a debugger (`cu8h_dbg.py`) | about 2 min per load |
| `run_cu8h_galendor.cmd` | t01, galactic half: installs the probe campaign whose Jabiim land map is `_MP_LAND_ENDOR` (trace it with `run_cu8h_trace.cmd`) | about 5 min |
| `run_cu8h_hum.cmd` | t02: Vader's hum / holster timing, then Luke's Saber_Throw, read from the game's memory | about 3 min in battle |
| `run_cu8h_luke.cmd` | t02 rerun: Luke throws once while holstered and once while drawn; also reads the thrown-saber id | about 1 min 30 s in battle |
| `cleanup_cu8h.cmd` | removes `Mods\CU8HProbe` (only with its marker file) | — |
| `run_storyb_kessel.cmd` | storyb t07 / t08 (2026-11-09): the Underworld story start traced from the seed through the Kessel UM01 map load (919 + 111 draws) and 10 min of battle (2.6 draws / frame), call site of every draw; vanilla game | about 15 min |
| `run_cu8k_kashyyyk.cmd` | cu8k t05, MOVEMENT §4.7.6 items 1 and 3: probe campaign, the Kashyyyk space battle load, then one group move of a capital ship, a frigate and two squadrons (two traces) | about 10 min |
| `run_cu8k_jetpack.cmd` | cu8k t05, §4.7.6 item 4: three Dark Trooper JET_PACK trials in a land skirmish; the probe arms the debugger itself | about 3 min |
| `run_cu8k_saber.cmd` | cu8k t05, CU8F §2.7 item 6: the `luke` session with the thrown saber's flight read every frame | about 1 min 30 s in battle |
| `launch_foc_vanilla.cmd`, `launch_foc_cu8hprobe.cmd` | start FoC windowed (no mod / `MODPATH=Mods\CU8HProbe`); for computer use: `Start-Process explorer.exe <file>` | — |

`selftest_cu8h.py` checks the kit offline and needs no game. It builds every session, compiles the Lua, runs the
debugger against a fake process and parses synthetic logs (draw report, call-site report, thrown-saber report). It
ends with `CU8H SELFTEST OK`.

Every new `run_*.cmd` passes its arguments on; with `--non-interactive` it does not pause and writes its console to
`out\run_<name>.log` (for a run started by Claude with `Start-Process`). The debugger runs are armed with the file
`out\arm` and stopped early with `out\stop` (PowerShell: `New-Item -ItemType File "<kit>\out\arm"`); the scripts
delete old ones at start. `cu8h_run.py` sessions with traces write `out\trace_status.txt` (which trace waits for
what).

## Tools

- **`cu8h_dbg.py`** is a small debugger. It uses the Windows debug API through Python ctypes, so nothing is installed
  (toolchain rule 5).
  - It attaches to `StarWarsG.exe` and sets a hardware **write watchpoint** on the synced LCG state (RVA 0xa13e24) in
    every thread. Each write is one LCG step, so it sees every draw, whichever function makes it. It logs the thread,
    the state, the mode / frame and a stack walk.
  - The game's memory is never written; only the threads' debug registers are set. They are cleared before it
    detaches, and the game keeps running.
  - Arming: the main-menu background battle draws all the time, so the watchpoint stays off until armed (`--arm-file`
    or `--arm-now`). It stops at mode frame N of the new battle (`--stop-mode-frame`), or N mode frames after arming
    inside a running battle (`--stop-after-frames`), or when the stop file appears.
  - `cu8h_dbg.py --selftest` attaches to a fake process.
- **`cu8h_dbg_parse.py`** writes the report.
  - It gives each hit a load phase from the functions on its stack: the exe's `.pdata`, chained entries resolved.
  - It checks A: the pick comes after every chunk-`0x102` draw.
  - It checks B: the pick is one `int(0, n − 1)`, and it replays the index (`--n-env`, 4 for Endor).
  - It checks C: no post-load draw comes before the pick.
  - It checks D: the first-tick weather draws.
- **`cu8h_trace.py`** (behind `run_cu8h_trace.cmd` and `run_storyb_kessel.cmd`) runs one trace. It starts the tracer,
  arms it when you press Enter (or, with `--arm-file PATH`, when that file appears: no prompt), then parses the trace
  into `out\<run>\report.txt`; with `--sites` also `sites.txt`. `out\latest_trace.txt` names the run folder.
- **`cu8h_sites.py`** (2026-11-09) writes the call-site report of a trace (`sites.txt`):
  1. the game modes seen and the battle mode;
  2. the load split: from the last seed write before the battle, one row per run of hits with the same thread /
     mode / mode frame, with cumulative steps (to match the q2 reader's per-frame draw counts), then the load hits by
     load phase and call site;
  3. steps per battle-frame window (`--windows`);
  4. per call site (the return address into the caller of the draw primitive, or the PC of an inlined draw) over
     `--table` ranges: steps, steps per frame, frames with a draw, the commonest frame gaps, the caller chains;
  5. the formation-ctor draws of MOVEMENT §4.7.6 (stacks `…,40d11f,4f6fd4,<creator>`): creator, adopt caller,
     team-helper caller, counted as draws (a rejected `int(1, 30)` try is merged into its draw).
- **`cu8h_run.py`** runs a mod session: it builds and installs `Mods\CU8HProbe`, optionally launches the game, and
  always cleans up.
  - For `hum` / `luke`, a read-only watcher polls every 2 ms each `DARTH_VADER` / `LUKE_SKYWALKER_JEDI` object of the
    land mode. It reads obj +0x3b4..+0x3b9 (+0x3b9 = engine loops suppressed, +0x3b5 / +0x3b6 = idle / moving loop
    on).
  - It also reads the HOLSTER_WEAPON behaviour (array obj +0x278, index byte obj +0x332 + 0x4E, vftable RVA 0x8b0f78):
    +0x40 bone, +0x44 / +0x48 transition start / end, +0x4c last engaged, +0x50 drawn.
  - It also reads SAP (obj +0x100) +0x2e8, the thrown-saber id; 0x3fffff = none.
  - It writes one line per change and finds the probe Lua records ("C8G|…|END") by a heap scan.
  - cu8k t05 (2026-11-09): when SAP +0x2e8 names a saber, the watcher finds that object in the store and logs it on
    every change (every frame in flight): position obj +0x78, flags +0x3a0, the projectile pack (obj +0xe8) target
    +0x08 / travelled +0x5c / damage +0x64 / max travel +0x6c, the thrower's position and the 3D distance. The hero
    lines carry SAP +0x2e4 (target id) and +0x2d8 (aim point); the target's position is logged once. The report's
    "thrown sabers" section gives the flight frame by frame, the death / gone / ability-end frames, the catch row (back
    within `Catch_Dist` 15) and the target radius R = max travel − 250 − |aim z − spawn z|.
  - `kashyyyk` / `jetpack`: runs `cu8h_dbg.py` traces one after the other against the game (`trace<k>-<label>\`
    folders with `trace.log`, `report.txt`, `sites.txt`), armed by `out\arm` (by hand, or by the probe Lua's
    `C8G|DBGARM|` record for `jetpack`) and stopped by `out\stop` (or `C8G|DBGSTOP|`); `report.txt` summarises each.
- **`build_cu8h_mod.py`** builds the mod from the install.
  - `galendor`: the campaign "CU8H PROBE: ENDOR GALACTIC" and a Planets.xml patch (Jabiim's
    `Land_Tactical_Map` := `_MP_LAND_ENDOR.ted`).
  - `hum` / `luke`: the shipped `BusyTacticalFreeStore.lua` plus `../cu8g_probes/cu8g_common.lua` and `cu8h_hum.lua`
    (the probe state machine).
  - `kashyyyk`: the campaign "CU8K PROBE: KASHYYYK SPACE": human Rebel at Corellia with Calamari_Cruiser (capital),
    Nebulon_B_Frigate (frigate) and two Rebel_X-Wing_Squadron, none with a hangar; Empire_Star_Base_2 at Kashyyyk
    (no AI); route Corellia_Kashyyyk; the space map is `_Space_Planet_Kashyyyk_01.ted`.
  - `jetpack`: the free store plus `cu8k_jetpack.lua` (three `Dark_Trooper_PhaseII` for the human Empire on Dagobah;
    trial A JET_PACK while moving, B standing, C after a move; each JET_PACK targets a `Story_Trigger_Zone` marker
    spawned with the units, as the shipped scripts do; `C8G|DBGARM|` 8 s before A, `C8G|DBGSTOP|` 8 s after C).

Output goes to `out\<yyyymmdd-hhmmss>-<session>\` (git-ignored).

## What you do

### `run_cu8h_trace.cmd` (t01, skirmish)

1. Start the game: `StarWarsG.exe` (windowed is easier). Run `run_cu8h_trace.cmd --label skirmish`. It waits for the
   game.
2. Single Player > Skirmish > LAND, map **(6) Endor Xpress** (`_MP_LAND_ENDOR`), you EMPIRE, an Easy AI REBEL.
3. Press Enter in the trace window, then click **Start Game** at once.
4. Click Begin on the load screen. The tracer stops by itself 4 s into the battle and prints the verdict
   (`VERDICT t01: A B C`). Quit the battle. Repeat for more loads; a rain environment shows the weather draws (D).

### `run_cu8h_galendor.cmd` + `run_cu8h_trace.cmd` (t01, galactic)

1. Run `run_cu8h_galendor.cmd`. It installs the mod and launches the game with `MODPATH=Mods\CU8HProbe`. Then run
   `run_cu8h_trace.cmd --label galactic` in a second window.
2. Single Player > Galactic Conquest > "CU8H PROBE: ENDOR GALACTIC" > REBEL > Start.
3. At Abregado Rae, drag the infantry into a fleet and move it to **Jabiim**. Open Jabiim and drop the infantry onto the
   surface. The battle bar appears.
4. Press Enter in the trace window, then click **Begin**. Click Begin again on the load screen.
5. After the tracer stops, Esc > Quit Game > Yes, then Exit Game. Never save. The galendor window cleans up.

### `run_cu8h_hum.cmd` (t02)

1. Single Player > Skirmish > LAND, map **(6) Dagobah Rush**. Slot 1 (you) EMPIRE, slot 2 AI REBEL, the rest closed.
   Start Game, then Begin.
2. Give no orders. The timeline:
   - 10 s: Vader appears in an open square, away from both bases.
   - 30 s: a Rebel trooper appears, and Vader attacks it.
   - 40 s: the trooper vanishes.
   - About 65 s: Luke appears with a second trooper.
3. Wait for "CU8H PROBE: lightsaber probe done" (about 2 min). Then Esc > Quit Game > Yes, Exit on the results screen,
   and quit the game. `out\<run>\report.txt` is written.

### `run_cu8h_luke.cmd` (t02 rerun)

The same steps as `hum`. Luke appears at 10 s and a trooper at 15 s, and Luke throws at once. A second trooper appears
at 50 s, with a second throw while drawn. Wait for "CU8H PROBE: lightsaber probe done" (about 1 min 30 s).

### `run_storyb_kessel.cmd` (storyb t07 / t08, vanilla game)

1. Close the game. Run `run_storyb_kessel.cmd` (it waits up to 30 min for the game), then start the game windowed
   with no mod (`launch_foc_vanilla.cmd`).
2. Esc through the intro movies. Single Player > Campaigns (Forces of Corruption) > Underworld; difficulty Medium
   (preselected). Create `out\arm` right before you click **Start Game**.
3. Esc through the opening movie if it plays. When the Kessel battle has loaded, press nothing, not even Esc (as
   `q2_traces` README step 4). Keep the game in front: it pauses when it loses focus.
4. The trace stops by itself at battle mode frame 18010 (10 min of battle; longer in wall time while the debugger
   runs). The window ends with `CU8H_TRACE_DONE`. Quit the game (Esc > Quit Game > Yes, Exit Game). Never save.
5. `out\<run>-trace-storyb-kessel\sites.txt`: section 2 splits the 412 galactic + 919 + 111 load draws, sections 3
   and 4 give the steady-state rate and its call sites (tables 256-18000 and 1801-18000).

### `run_cu8k_kashyyyk.cmd` (cu8k t05, MOVEMENT §4.7.6 items 1 and 3)

1. Close the game. Run `run_cu8k_kashyyyk.cmd --no-launch`, then start the game with `launch_foc_cu8hprobe.cmd`.
2. Single Player > Galactic Conquest > "CU8K PROBE: KASHYYYK SPACE" > REBEL > Start. Drag the fleet at Corellia to
   Kashyyyk. When the battle bar appears: create `out\arm`, click Begin, then Begin on the load screen. Trace 1
   stops at mode frame 240.
3. When `out\trace_status.txt` says `trace=2/2 … state=waiting_for_arm` and the fleet has arrived: box-select the
   Calamari Cruiser, the Nebulon-B and both X-wing squadrons, create `out\arm`, right-click one move. Trace 2 stops by
   itself 600 mode frames (20 s of game time) later; wait until `out\trace_status.txt` says `all_traces_done`.
4. Esc > Quit Game > Yes, Exit Game. The kit writes `out\<run>-kashyyyk\report.txt` and cleans up.

### `run_cu8k_jetpack.cmd` (cu8k t05, §4.7.6 item 4)

1. Close the game. Run `run_cu8k_jetpack.cmd --no-launch`, start the game with `launch_foc_cu8hprobe.cmd`.
2. Single Player > Skirmish > LAND, map (6) Dagobah Rush; slot 1 (you) EMPIRE, slot 2 AI REBEL, others closed.
   Start Game, Begin. Give no orders. Wait for "CU8K PROBE: jetpack probe done" (about 35 s of battle; several minutes of wall time, because the debugger slows the game while armed).
3. Esc > Quit Game > Yes, Exit on the results screen, Exit Game. Report: `out\<run>-jetpack\report.txt`
   (`trace1-jetpack\sites.txt` section 5: the ctor draws by mode frame; the JORDER records give the trial frames).

### `run_cu8k_saber.cmd` (cu8k t05, CU8F §2.7 item 6)

As `run_cu8h_luke.cmd` (`--no-launch` + `launch_foc_cu8hprobe.cmd` when Claude drives). The report's "thrown sabers"
section has the two flights.

## The probe Lua (`cu8h_hum.lua`)

`C8H_Tick` runs on the AI Rebel's free store (fast service rate). Its phases are wait → idle → engage → after →
luke_idle → luke → luke_after → end.
- **Spawns:** the heroes are spawned for the human player and made invulnerable. The targets are AI troopers with
  `Prevent_AI_Usage`.
- **Orders:** each hero gets `Attack_Target` every 30 frames. Luke has `SABER_THROW` autofire on, plus one
  `Activate_Ability("SABER_THROW", target)`.
- **Records:** `HSPAWN`, `HORDER`, `HPHASE`, `HP` (each hero's attack target on every service) and `HEND`.
- **Session `luke`:** skips Vader (`luke_only`) and adds the second target and throw (`luke_second`).
- **Timings:** set in `build_cu8h_mod.py` (`HUM`, `LUKE`).
