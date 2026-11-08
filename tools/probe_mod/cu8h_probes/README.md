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

`selftest_cu8h.py` checks the kit offline and needs no game. It builds every session, compiles the Lua, runs the
debugger against a fake process and parses synthetic logs. It ends with `CU8H SELFTEST OK`.

## Tools

- **`cu8h_dbg.py`** is a small debugger. It uses the Windows debug API through Python ctypes, so nothing is installed
  (toolchain rule 5).
  - It attaches to `StarWarsG.exe` and sets a hardware **write watchpoint** on the synced LCG state (RVA 0xa13e24) in
    every thread. Each write is one LCG step, so it sees every draw, whichever function makes it. It logs the thread,
    the state, the mode / frame and a stack walk.
  - The game's memory is never written; only the threads' debug registers are set. They are cleared before it
    detaches, and the game keeps running.
  - Arming: the main-menu background battle draws all the time, so the watchpoint stays off until armed (`--arm-file`
    or `--arm-now`). It stops at mode frame N of the new battle (`--stop-mode-frame`).
  - `cu8h_dbg.py --selftest` attaches to a fake process.
- **`cu8h_dbg_parse.py`** writes the report.
  - It gives each hit a load phase from the functions on its stack: the exe's `.pdata`, chained entries resolved.
  - It checks A: the pick comes after every chunk-`0x102` draw.
  - It checks B: the pick is one `int(0, n − 1)`, and it replays the index (`--n-env`, 4 for Endor).
  - It checks C: no post-load draw comes before the pick.
  - It checks D: the first-tick weather draws.
- **`cu8h_trace.py`** (behind `run_cu8h_trace.cmd`) runs one trace. It starts the tracer, arms it when you press
  Enter, then parses the trace into `out\<run>\report.txt`.
- **`cu8h_run.py`** runs a mod session: it builds and installs `Mods\CU8HProbe`, optionally launches the game, and
  always cleans up.
  - For `hum` / `luke`, a read-only watcher polls every 2 ms each `DARTH_VADER` / `LUKE_SKYWALKER_JEDI` object of the
    land mode. It reads obj +0x3b4..+0x3b9 (+0x3b9 = engine loops suppressed, +0x3b5 / +0x3b6 = idle / moving loop
    on).
  - It also reads the HOLSTER_WEAPON behaviour (array obj +0x278, index byte obj +0x332 + 0x4E, vftable RVA 0x8b0f78):
    +0x40 bone, +0x44 / +0x48 transition start / end, +0x4c last engaged, +0x50 drawn.
  - It also reads SAP (obj +0x100) +0x2e8, the thrown-saber id; 0x3fffff = none.
  - It writes one line per change and finds the probe Lua records ("C8G|…|END") by a heap scan.
- **`build_cu8h_mod.py`** builds the mod from the install.
  - `galendor`: the campaign "CU8H PROBE: ENDOR GALACTIC" and a Planets.xml patch (Jabiim's
    `Land_Tactical_Map` := `_MP_LAND_ENDOR.ted`).
  - `hum` / `luke`: the shipped `BusyTacticalFreeStore.lua` plus `../cu8g_probes/cu8g_common.lua` and `cu8h_hum.lua`
    (the probe state machine).

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
