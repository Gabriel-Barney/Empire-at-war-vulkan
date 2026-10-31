# A1 tactical-AI probes: free-store timing (A1 t02) and landing-point ownership (A1 t04)

Two in-game probes for roadmap group A1 (tactical AI). They share one mod folder (`Mods\A1TacticalProbe`, one
`MODPATH`), one read-only memory reader (`a1_reader.py`) and one parser (`a1_parse.py`), so both can run in one game
session. Nothing from the game is stored in the repository: the mod is built from the install at run time.

| probe | question | spec | run script | cleanup |
|---|---|---|---|---|
| free-store timing (A1 t02) | at which mode frame do `FreeStoreService` and `On_Unit_Service` first run for an AI player? | AI_TACTICAL §5.3, §12 item 10 | `freestore_timing\run_freestore.cmd` | `freestore_timing\cleanup_freestore.cmd` |
| landing points (A1 t04) | does the attacker own its landing reinforcement point(s) at battle start, and what is its land cap (+0x404)? | PHASE4 A1 "Land reinforcements", AI_GALACTIC_3 §3.3, REMAINING_GAPS §3.3 | `landing_points\run_landing.cmd` | `landing_points\cleanup_landing.cmd` |
| both, one session (recommended) | | | `run_a1.cmd` | `cleanup_a1.cmd` |

## What Gabriel does

1. Start Steam. Double-click **`tools\probe_mod\a1_tactical\run_a1.cmd`** (both probes, about 8 minutes of play).
   It installs the mod, starts the reader, launches `StarWarsG.exe MODPATH=Mods\A1TacticalProbe` from the
   `corruption` folder and prints the click path below. (`run_freestore.cmd` / `run_landing.cmd` do one probe only.)
2. In **every** battle: give **no orders** and click nothing on the map; just wait.
   - **A) Skirmish land.** Main menu > Single Player > Skirmish > **Land**. Map **Utapau**. Slot 1 (you): **Empire**,
     team 1. Slot 2: **AI** (any difficulty) **Rebel**, team 2. Other slots closed. Default credits / tech. Start.
     Wait **90 s** from the moment the battlefield is shown, then Esc > Quit (back to the main menu).
   - **B) Skirmish space.** Skirmish > **Space**, map **Kuat**, same slots (you Empire, AI Rebel). Start, wait
     **90 s**, Esc > Quit.
   - **C) Campaign invasion.** Galactic Conquest > campaign **"S10 PROBE A: TATOOINE"**, faction **Empire**, Start
     (accept defaults). On the galaxy map click nothing: after about 3 s the line "S10 PROBE: story plot loaded ..."
     appears and Underworld ground forces fly from Byss to Tatooine and invade. At the battle prompt choose to
     **fight** (not auto-resolve, not retreat). The Tatooine land battle loads; wait **60 s**, Esc > Quit.
     If no battle starts within 60 s of the galaxy map appearing, note it and quit to the main menu.
   - Then quit the game (main menu > Quit). The run finishes by itself (`A1_RUN_DONE exit=<n>` in the window).
   - `run_landing.cmd` alone asks for C and then **D) Skirmish land Utapau** as in A (60 s);
     `run_freestore.cmd` alone asks for A and B and installs no campaign.
3. **Send back** the run folder `tools\probe_mod\a1_tactical\out\<yyyymmdd-hhmmss>-<probe>\` (`report.txt` first;
   also `reader.log`, `run.json`). `out\latest.txt` names the newest one.

**Cleanup** is automatic at the end of every run (also after Ctrl+C or an error). After a crash, double-click
`cleanup_a1.cmd` (or the probe's own cleanup script; all three do the same and are safe to repeat): it deletes
`corruption\Mods\A1TacticalProbe` only if it carries `A1_PROBE_MARKER.txt`. No retail file is changed; other mods in
`corruption\Mods` are left alone. The game itself may rewrite its own settings (last map etc.) as in any session.

Background / driven run: `run_a1.cmd --non-interactive` (no key presses; progress in `out\<run>\run_status.txt`,
`state=` installing / waiting_for_game_start / game_running / collecting / done). `--no-launch`: start the game
yourself with `StarWarsG.exe MODPATH=Mods\A1TacticalProbe`. Exit codes: 0 report with probe data, 10 no tactical
battle / no probe data, 20 / 21 preflight (game missing / already running), 30 interrupted, 31 error.

## Probe 1: free-store timing (A1 t02)

**Reading under test.** The free store is serviced every 3rd frame (AI_TACTICAL §5.3). `FreeStoreService` (resuming
the script's `main` loop) runs when `now - LastService > 20` and the unit pass (`On_Unit_Service` per free unit)
when `now - LastUnitService > 2`, both stamps starting at 0. With the literal `now = (float)F * (1/30)` in float,
600 * 0.033333335f rounds to 20.0000019 > 20, so `FreeStoreService` fires at **F = 600** and **1200** (and then at
**1803**, not 1800: 60.0000038 - 40.0000038 is exactly 20.0 in float, not > 20), while §5.3's prose says "about every
20.1 s" (603, 1206, 1809). The first unit pass is at **F = 63** in both readings (60 * (1/30f) is
exactly 2.0, not > 2); the second is **123** in float and 126 in exact arithmetic. `python a1_parse.py model`
prints all predictions (phase 0, 1, 2 of the 3-frame service).

**How it is measured.** `Data\Scripts\FreeStore\BusyTacticalFreeStore.lua` = the shipped script, unchanged, with
`freestore_timing\fs_hook.lua` appended. The hook wraps `Base_Definitions`, `main`, `FreeStoreService`,
`On_Unit_Service` and `On_Unit_Added` (each wrapper records, then calls the shipped function; no orders, no
`GameRandom`). Every AI player's tactical free store (land and space; `BusyTacticalFreeStore` is the
`Land/SpaceFreeStoreScript` of every shipped AI player) records `GetCurrentTime()` (mode time, printed `%.9g`, so the
float value 20.0000019 is visible), `GetCurrentTime.Frame()`, player id and faction for: script load, `main()`
entry, the first 12 `FreeStoreService` calls, the first 16 unit passes (with the number of units serviced in the
previous pass), the first 6 `On_Unit_Added` calls, and `Find_Player` ids of every faction.

**Output channel.** Retail `_CustomScriptMessage` / `_OuputDebug` write nothing (S10 and Q2 runs, 2026-10-29 / 30;
their file / debug-print calls are empty stubs in the 1.121 exe). The hook therefore keeps its records as Lua strings
`A1P1|<kind>|sid=...|...|END` in a table, and `a1_reader.py` finds them by scanning the game's committed private
memory every 5 s during a battle (in a second thread; read-only `ReadProcessMemory`). It also looks for the comment
`A1PROBE_SRC_MARK_FREESTORE_V1` (proof that the mod file was read from disk, if the loader's buffer is still around).

**Report** (`report.txt`, "Free-store timing probe"): one block per script instance (player, land / space) with the
frames of LOAD / MAIN / every `FreeStoreService` call / every unit pass / `On_Unit_Added`, the offset of
`GetCurrentTime.Frame()` against the mode frame, and the readings `float/p0..2`, `exact/p0..2` that reproduce them.

| result | meaning |
|---|---|
| `FreeStoreService calls: F=600, 1200, 1803 ...`, `float/p0` → "A1 reading CONFIRMED" | port: `now = (float)F * (1/30f)`, compare in float, F = 600 / 1200 / 1803 |
| `F=603, 1206, 1809`, `exact/p0` → "REFUTED" | the §5.3 prose (about 20.1 s) is right; the clock is exact (e.g. integer frames compared with 600) |
| first unit pass F = 63, then 123 (float) or 126 (exact) | unit cadence; also shows the service phase (frames mod 3) |
| a `FreeStoreService` call at F < 300 (with MAIN at the same F) | `main()` runs once when the script starts; the 20 s timer gates only later resumes. The port must do that first call too |
| phase 1 or 2 | the 3-frame service is not aligned with frame 0; the port's service phase needs changing |
| no records, no `SRC` line | the loose mod script was not loaded (Lua files may not go through MODPATH), or no AI free store ran |
| `SRC` line but no records | the file was read but the hook did not run (or the strings were not found: check the `SCAN` lines) |

## Probe 2: landing-point ownership (A1 t04)

**Question.** The port's attacker never qualifies for land reinforcements: its land cap is 0 (AI_GALACTIC_3 §3.3:
cap = `Base_Population_Capacity` [L 0] + `Additional_Population_Capacity` of the reinforcement points it holds; the
defender always has `Max_Ground_Forces_On_Planet` = 10), and it holds none at start. Does the attacker own its
landing point(s) at battle start (and from which frame), what is its +0x404 then, and is `Base_Population_Capacity`
really 0? Already known (S10 run `20261029-074311`, Tatooine): the Underworld attacker landed 17 units from a
Neutral `Reinforcement_Point_Plus4_Cap` that it owned at the first dump (mode frame 2); every other point stayed
Neutral. This probe adds the frame-exact ownership history and the capacity.

**How it is measured** (memory only; no Lua needed). For every land battle `a1_reader.py` polls the game every
4 ms (several times per frame): every `REINFORCEMENT_POINT*` object of the mode's object store (owner at first
sight, then every owner change with the mode frame read in the same poll, so +-1 frame), every player's +0x404 (land population capacity; PlayerList by
id, `docs/re` AI_GALACTIC_3 §3.3) at the first frame and at every change, and per player the capacity of the alive
objects it holds (`Additional_Population_Capacity` of their types, from the install's MARKERS.XML / PROPS_STORY.XML,
`caps.json` in the run folder). The campaign is the S10 probe's "S10 PROBE A: TATOOINE" (built by
`tools\probe_mod\s10_map_owners\build_mod.py`; its GameScoring hook is left out): the AI Underworld invades the human
Empire's Tatooine by itself.

**Report** ("Landing-point ownership probe"): per land battle the players (faction, human, +0x404 first and every
change, held capacity, top object types), every reinforcement point with its owner at first sight and its changes,
then per attacker (campaign: the non-human side with units; skirmish: both sides) the points it owns at first sight,
the first frame it owns one, and at frames (first, 30, 300, last) +0x404, held capacity, `Base_Population_Capacity
= +0x404 - held` and the effective cap min(10, +0x404) by §3.3.

| result | meaning |
|---|---|
| attacker owns its landing point at first sight / from frame 0-2, +0x404 = that point's capacity (4 on Tatooine) | ownership is given at spawn (or captured at once: Neutral points are serviced without the 30 s countdown, REMAINING_GAPS §3.3). Port: `bt_landing_points` + capture rule must hand the attacker that point in the first frames; cap = 4, not 0 |
| attacker owns nothing for the whole minute, +0x404 = 0 | the port's cap 0 is right; the attacker's reinforcements must come from elsewhere (look at what the AI does) |
| +0x404 - held > 0 (skirmish or campaign) | `Base_Population_Capacity` is not 0: use the value |
| +0x404 lags the ownership change | +0x404 is refreshed only on object add / remove (§3.3), not on capture; the port must refresh the same way |
| skirmish: +0x404 at start = 0 and no points | skirmish units do not use +0x404 the same way (record what the HUD shows) |

## Files

| file | what |
|---|---|
| `run_a1.cmd`, `freestore_timing\run_freestore.cmd`, `landing_points\run_landing.cmd` → `a1_run.py` | install, start the reader, launch, wait, report, cleanup (`--probe both / freestore / landing`, `--non-interactive`, `--no-launch`, `--game`, `--scan-sec`) |
| `cleanup_a1.cmd`, `freestore_timing\cleanup_freestore.cmd`, `landing_points\cleanup_landing.cmd` → `a1_cleanup.py` | idempotent cleanup (`--dry-run`, `--no-pause`) |
| `build_a1_mod.py` | builds `Mods\A1TacticalProbe` from the install (`--out`, `--no-campaign`, `--source-zip`); `--caps-only` writes the capacity table |
| `freestore_timing\fs_hook.lua` | the Lua appended to the shipped `BusyTacticalFreeStore.lua` |
| `a1mem.py` | read-only process access (ctypes): exe check, documented layout (same values as `s10_layout.hpp` / `q2_layout.hpp` + the player fields), heap scan |
| `a1_reader.py` | the recorder (log format in its docstring) |
| `a1_parse.py` | `parse <reader.log>` → report; `model` prints the predicted free-store frames |
| `selftest\selftest.py` (`run_selftest.cmd`), `selftest\fake_game.py`, `selftest\build_lua50.cmd` | offline end-to-end test |

All Python runs with the project venv: `toolchain\venv\Scripts\python.exe -I <script>`.

## Offline validation (2026-10-31)

- `selftest\run_selftest.cmd`: **SELFTEST OK**.
  1. The hooked `BusyTacticalFreeStore.lua` (shipped script from `64Patch.meg` + hook) runs under stock Lua 5.0.2
     (`build_lua50.cmd`, built at /Od: MSVC 14.51 /O2 miscompiles `ltable.c`, see `engine/lua/CMakeLists.txt`) with
     a harness that plays the 3-frame service for a land battle (float clock) and a space battle (exact clock): all
     record kinds appear, the shipped `FreeStoreService` / `On_Unit_Service` bodies still run, `t=20.0000019` at
     F = 600.
  2. `fake_game.py` lays out the documented structures (campaign land battle: Empire human, Underworld attacker
     taking a Neutral `Reinforcement_Point_Plus4_Cap` at frame 2, +0x404 0 → 4; then a space battle) and holds the
     Lua records in its memory from their frame on; `a1_reader.py` finds every record by heap scan and logs the
     ownership change at frame 2.
  3. `a1_parse.py`: land "CONFIRMED" (600, 1200, `float/p0`), space "REFUTED" (603, 1206, `exact/p0`), first unit pass
     63; attacker 6: 0 points at first sight, owns one from F = 2, +0x404 4 = held 4, base 0.
- `build_a1_mod.py` against the install: builds the mod (free-store script from `64Patch.meg`, the S10 campaign,
  text file through `build_probe.py`'s `.dat` writer); `--caps-only`: 15 types (Reinforcement_Point* 1-100,
  UMP_DropPoint_Empire 100, UM12_Star_Base 17).
- `run_a1.cmd --non-interactive --no-launch --game <scratch copy> --source-game <install>`: installs into the
  scratch folder, the reader ignores a non-FoC `StarWarsG.exe` (PE timestamp check), the run sees that process start
  and exit, writes `report.txt` (exit 10, no data), and cleanup deletes only `Mods\A1TacticalProbe` (another mod
  folder is left).
- Not validated (needs the retail game): whether a loose `Data\Scripts\FreeStore\*.lua` in a MODPATH mod replaces
  the shipped one (the S10 / Q2 Lua hooks never produced output, but their channel was the problem: the S10 story
  Lua did start the invasion, so loose story Lua does load); the heap scan against the real game (size, time per
  scan: logged in `SCAN` lines); the player fields +0x4C / +0x404 / +0x68 against the real exe (the faction name is
  found by search and its offset printed implicitly by `faction=` being filled).
