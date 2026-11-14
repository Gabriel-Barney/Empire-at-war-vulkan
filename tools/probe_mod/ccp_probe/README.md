# ccpgu t02: Capture Control Points probe (SKIRMISH_SETUP §4.2)

Retail check of the two `[M]` points the port took from `docs/re/SKIRMISH_SETUP.md` §4.2 (roadmap group `ccpgu`, task
t02; the rule itself was ported in `ccpgt`):

1. **Frames from the last capture to the battle end** in a game type 4 (Capture Control Points) skirmish. Port: the
   completed capture arms a domination timer of round(30 × `Control_Point_Domination_Victory_Time_In_Secs` = 30.0) =
   900 frames, the 0x15 test runs when it reaches 0, and the declaration's 210-frame countdown ends the battle:
   "900 + 210", `[M] ±1` for the order of the capture service and the mode tick.
2. **Which `_MP_LAND_*` maps place `Reinforcement_Point` objects, and with which start owner.** Answered offline from
   the map files (`findings_maps.md`); the run confirms the start owner in memory on the map it plays.

## What the port does (the values compared)

| value | port | source |
|---|---|---|
| timer at the capture frame | 899 (armed with 900 in the capture service, decremented by `check_victory` in the same tick) | `engine/apps/sim/src/tactical_battle_garrison.cpp:276` (`vm_cp_capture` from `s4_tick`, `tactical_battle.cpp:4494`, before `check_victory` at `:4513`); `tactical_battle_victory.cpp:113-116`, `:331` |
| capture → 0x15 test | 899 frames | `tactical_battle_victory.cpp:331-335` |
| test → declaration | 0 (same tick) | `tactical_battle_victory.cpp:224-227` |
| countdown at the declaration | 210 | `tactical_battle_victory.cpp:16` (`kRecordCountdown`), `:226` |
| declaration → countdown 0 / battle end | 210 frames (from the next tick) | `tactical_battle_victory.cpp:328`, `:358` |
| **capture → battle end** | **1109 frames** (nominal 900 + 210 = 1110) | the rows above |
| start owner of the points | the Neutral player (raw map owner 3 = Neutral faction) | `tactical_battle_bt.cpp:696` (`bt_map_owner`), `:781` |
| relevant types | `Reinforcement_Point` and its six `_PlusN_Cap` variants | `tactical_battle_victory.cpp:61-67` (`vm_cp_relevant`) |

## Part 2 offline (no game run needed)

`ccp_maps.py table` reads every `_MP_LAND_*` map from the install (FoC file order) and MARKERS.XML; the result is in
**`findings_maps.md`**. In short: all 24 land maps place 1 to 5 relevant points (69 in all, every one
`Reinforcement_Point` except Honoghr's two `_Plus2_Cap`), every one with raw owner 3 (Neutral). No map file places a
faction-fixed `Reinforcement_Point_<Faction>*` directly, but every map carries `Team_NN_Reinforcement_Point_Plus2_Cap`
team markers (51 on 24 maps) that the skirmish setup turns into `Reinforcement_Point_<slot NN faction>` owned by slot
NN (seen in the retail run on Kamino; port `tactical_battle_bt.cpp` `team_pad`). Those have no CAPTURE_POINT and never
count for 0x15. With the §2.1 owner remap every relevant point starts with the Neutral player, as the port has it. The 18 `CONTROL`-tagged maps are the ones the Capture Control Points list offers. What only the
game can show (the owner the engine really gives at frame 0) is read by the run on the map it plays.

## How the probe works

- **Lua (`ccp_hook.lua`)**, appended to the shipped `BusyTacticalFreeStore.lua`: runs in the **Rebel AI's** free store,
  arms only in a land mode while `Find_Player("Empire").Is_Human()` is true (so never in the main-menu background
  battle), lowers `ServiceRate` to 0.05 s and skips the shipped free-store bodies (the AI's free units stay idle).
  1 s in it lists every `Reinforcement_Point*` object with its owner (`PT`); 5 s in it spawns, for the human Empire, two
  `Imperial_Stormtrooper_Squad` (they capture) and one `AT_AT_Walker` (cannot capture, `Influences_Capture_Point False`;
  shoots contesting units) on every relevant point, all invulnerable. The troopers capture through the capture service
  (10 s), which is what arms the timer (`Change_Owner` would not, §4.2). It then logs owner changes (`OWN`, `ALL`) and a
  heartbeat at 3-frame resolution. Records are Lua strings `CCP1|...|END` found by a heap scan.
- **Memory reader (`ccp_reader.py`, read-only, `../a1_tactical/a1mem.py`)**: per poll (every 2 ms, several per frame,
  each poll framed by two reads of the mode frame) the owner (`+0x58`) of every point object, the mode's domination
  state (`+0x2b0` armed, `+0x2b4` timer, `+0x2b8` player) and the victory monitor (`mode +0x28`; validated by its
  back-pointer `+0x00 == mode`; `+0x20` declared, `+0x24` countdown, `+0x28` declared type / player). One `ST` line per
  change, so there is a line for every frame while the timer or the countdown runs. `STALL` marks the frame where the
  game stopped advancing (end screen). `reader_status.json` gives `phase` / `hint` for the driver: `menu`, `battle`,
  `capturing`, `captured`, `armed`, `declared`, `ended`. The player list is re-read every 30 frames: at mode frame 0
  retail still holds the pre-remap list without a human (run 20261108-214851 stayed in `menu_battle` because of this);
  the Lua `ARM` record's `human_id` is the fallback. `+0x2b0` is a byte (`+0x2b1` is logged as `b1`).
- **Frame stamps**: retail writes the mode frame counter after the tick's sim state, so a poll can see a tick's change
  still stamped with the previous frame (two `ST` lines with one stamp, the next stamp skipping one). The parser moves
  such a line to the next tick and reports the logged stamps beside the ticks.
- **Parser (`ccp_parse.py`)**: per battle with a human player: C (last capture), A (arm, timer at A), T (test), D
  (declaration, countdown, type 0x15 = 21, player), E (countdown 0), the visible end, and a retail / port table.
  Logs from before the reader fix (no human in the reader `PL` lines) take the human and the factions from Lua.

## Run it

Start Steam. Then either let the kit launch the game:

```
tools\probe_mod\ccp_probe\run_ccp.cmd --non-interactive
```

or, when another session launches and drives the game (the overnight computer-use run):

```
tools\probe_mod\ccp_probe\run_ccp.cmd --no-launch --non-interactive
rem then, from F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption:
StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\CcpProbe
```

The runner refuses to start while `StarWarsG.exe` runs (exit 20). It snapshots the profile (registry
`HKCU\Software\Petroglyph\StarWars FOC` and the Saved Games Save / Recordings files, `../q2_traces/q2_profile.py`),
then **presets** `Profiles\Profile0\Player\LastInternetGameType = 3` (the skirmish setup opens on game type 4) and
`LastInternetMapLand = .\Data\Art\Maps\_MP_LAND_KAMINO.TED` (`--no-preset` to skip), builds `Mods\CcpProbe`, starts
the reader and waits for the game to start (up to 15 min) and then to exit. On exit it writes the report, deletes
`Mods\CcpProbe` (only with `CCP_PROBE_MARKER.txt` in it) and restores and verifies the profile. `cleanup_ccp.cmd`
does the same by hand. Other maps: `--map _MP_LAND_BOTHAWUI` (any CONTROL map works; the hook finds the points itself).

### In the game

1. Main menu > **Single Player** > **Skirmish**. With the preset the setup screen opens on the game type shown as
   **Land Control** with the map **(4) El Kamino** selected (retail run 20261108-214851; the labels are not "Capture
   Control Points" / "Kamino"). Otherwise: pick **Land Control** in the game-type list at the top left
   (`IDC_GAME_TYPE_LIST`), then the **(4) El Kamino** row in the map list.
2. Slot 1 (you): **Empire**, team 1. Slot 2: **AI** (Easy is fine), **Rebel**, team 2. Other slots closed. Leave the
   options at their defaults (the win condition is fixed to Control Point Domination on this game type). Start.
3. Give **no orders**. About 5 s in, troopers and an AT-AT appear on the control point; ~10 s later the point is the
   Empire's (`phase` `captured` → `armed`), 30 s later the victory text shows (`declared`), ~7 s later the battle
   ends (`ended`). About 60 s of battle time.
4. When `phase` is `ended` (or the end screen shows), wait ~10 s, then leave to the main menu and quit the game. Optional,
   same launch: a second Land Control battle on Bothawui (2 points) before quitting; the report covers every battle.

If `phase` stays `capturing` for more than 2 minutes, or flips `disarmed` and never re-arms, quit anyway: part 2 and
the Lua records are still in the report.

## Results

`out\<run>\report.txt` (`out\latest.txt` names the run). Read the `RESULT capture -> end` line and the table
(`MATCH` / `DIFF ±n` per value), the `PART 2` block (start owner of each point) and the `NOTE` lines. Re-parse by hand:
`toolchain\venv\Scripts\python.exe -I tools\probe_mod\ccp_probe\ccp_parse.py parse out\<run>\reader.log --config
out\<run>\ccp_config.json`.

Reading it:
- `timer_at_capture` 899 = retail runs the capture service before the mode tick, as the port; 900 = after (then the
  port's test and end come one frame early: move the decrement or the arm).
- `decl_countdown` / `decl_to_zero` other than 210 = the countdown starts in the declaring tick or later than the port.
- `stall after D` = the last frame counter value; retail stops one below the countdown-zero tick (the battle ends
  inside that tick).

### Retail result (run 20261108-214851, Kamino)

Every value MATCH (`out/20261108-214851/report_v2.txt`; the kit's own report.txt of that run is empty because of the
reader bug fixed since): capture tick 454 with timer 899, test and declaration {0x15, Empire} at tick 1353 = C + 899
with countdown 210, countdown 0 and battle end at tick 1563 = **C + 1109 = 900 + 210 - 1**, as the port. Raw stamp
deltas are 898 / 1108 (frame counter written after the sim state). Full record: `docs/re/CCPGU_T02_RETAIL_RUN.md`.

## Files

| File | What it does |
|---|---|
| `run_ccp.cmd` / `ccp_run.py` | One session (see "Run it"). Exit codes: 0 declaration recorded; 10 no battle data / no declaration; 20 preflight (game missing or running); 30 interrupted; 31 error |
| `build_ccp_mod.py` | Builds the mod from the install (or `--source-zip`): shipped free-store script + generated `CCP_CFG` + `ccp_hook.lua` |
| `ccp_hook.lua` | The Lua side (Lua 5.0) |
| `ccp_reader.py` | Read-only memory recorder; `--selftest` on a fake memory image |
| `ccp_parse.py` | `parse reader.log [--config] [--out] [--json]`; `--selftest` with logs from the port model |
| `ccp_maps.py` | Offline part 2: `table` (writes `findings_maps.md`), `points <MAP>`, `choose` |
| `findings_maps.md` | Offline answer to part 2 (with the team markers) |
| `ccp_cleanup.py` / `cleanup_ccp.cmd` | Removes `Mods\CcpProbe` (marker rule) and restores the profile of the newest run |
| `selftest_ccp.py` | Offline check, below |

## Offline check

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\ccp_probe\selftest_ccp.py
```

Runs the hook under stock Lua 5.0.2 with stub engine calls (armed run, main-menu case without a human Empire, wrong AI
faction), compiles the built free-store script, checks every type name against the FoC XML, runs the reader and parser
selftests, the cleanup marker rule and the profile preset (fake writer). Ends with `CCP SELFTEST OK`.

## Known limits

- The mode offsets `+0x2b0..+0x2b8` and the monitor fields come from the static RE (§4.2, BATTLE_TRANSITIONS §3.1a) and
  were confirmed by the retail run (`CPV` raw `00010000 ffffffff ffffffff`: armed byte 0, `+0x2b1` = 1, timer and
  player -1; the timer armed at the capture tick and the monitor declared type 21).
- A one-point map arms with no other point to check (the "every other point is allied" test is vacuous); Bothawui
  (two points) exercises the full test.
- `Spawn_Unit` on a marker and `Make_Invulnerable(true)` on its results follow shipped story scripts; a failing engine
  call would stop the retail script silently (the `HB` records then stop).

## ccpgu t04: multi-point run (Bothawui, 2 points)

Same kit, `--map _MP_LAND_BOTHAWUI` (CONTROL map, two `Reinforcement_Point`s, both raw owner 3 = Neutral, `findings_maps.md`)
with a `--scenario`:

| scenario | what the hook does | what it shows |
|---|---|---|
| `all` (default) | the human's capturers on every point at once | t02 behaviour |
| `sequential` (`--stagger-sec 20`) | point 1 at 5 s, point 2 `stagger` later | first capture does not arm (another point is Neutral), the last one arms |
| `steal` (`--steal-sec 30`) | the AI's infantry takes point 1, the human takes the other points; `steal` s later the AI units are despawned and the human takes point 1 | a capture with a not-allied point disarms; Rebel -> Neutral -> Empire; the last capture arms and the win follows 899 + 210 - 1 frames later |

`ccp_parse.py` adds a `MULTI-POINT` block per battle: per-point owner changes with the frame (and the offset to the
first change), retail armed / timer / player after each capture beside the port rule replayed on the observed captures
(`port_cp_capture`: a Neutral new owner disarms; arm only when every other relevant point is the new owner's), the count
of enemy takeovers via Neutral vs direct, and the last-capture-to-declaration distance. The usual C / A / T / D / E table
and the `RESULT capture -> end` line refer to the last arm.
