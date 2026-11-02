# FOW regrow probe (fowf t06): regrow window and top-edge circle shift

In-game probe for roadmap task fowf t06 / `docs/re/FOG_OF_WAR.md` §9 item 8. One land skirmish, no orders, about
3 minutes. Nothing from the game is stored in the repository: the mod is built from the install at run time.

## Questions and predictions

**Q1 regrow window** (§4.2, §4.4, §5.2). After the last reveal source of a cell stops counting it, the cell's state
goes 239+ → 238 at the player's next decay (decays run when player id ≡ F mod 16, at common-tick step 4, before Lua),
then 217, 196, 175, 154, 133, 112, 91, 70, 49, 28, 7, **0** at the next 12 decays (step (int)(238 / 11.25) = 21). An
object is hidden for game logic iff every sample point lies in a cell with state 0. So an object revealed only by
the probe stays visible after `Undo_Reveal` in frame F for exactly `192 + d1` frames, d1 = ((id − F − 1) mod 16) + 1:

| undo frame F mod 16 (human id 0) | 0 | 1 | 2 | 4 | 8 | 12 | 15 |
|---|---|---|---|---|---|---|---|
| frames to the first decay (d1) | 16 | 15 | 14 | 12 | 8 | 4 | 1 |
| hidden after (frames / s) | 208 / 6.93 | 207 / 6.90 | 206 / 6.87 | 204 / 6.80 | 200 / 6.67 | 196 / 6.53 | 193 / 6.43 |

The probe makes 4 trials whose undo is timed for d1 = 16, 1, 8, 12: predicted **208, 193, 200, 204 frames**
(6.93 / 6.43 / 6.67 / 6.80 s); the cells hit 0 at a frame ≡ human id (mod 16), and the object turns hidden in that
frame (visible in every sample before it). `python -I fow_model.py predict` prints the full table.

**Q2 top-edge shift** (§2.2). Row 0 is the max-y edge of the land FOW box. On `_MP_LAND_POLUS` (terrain 187 × 187
vertices) the box is [0, 3740]² if the land box is 20·W (RE 2026-11-01), 250 × 250 cells of 14.96, or [0, 3720]² /
248 cells of 15.0 if it is 20·(W − 1); the reader logs the real box and the parser predicts from it. A reveal of
R = 90 has r = (int)(90 / 14.96 + 0.5) = 6 cells (row widths 5 7 9 11 13 13 13 13 13 11 9 7 5). A reveal whose
centre row k < 6 is drawn as the circle centred on **row 6** (y ≈ 3643) cut below row k + 6, instead of the plain
circle around row k cut at row 0:

| centre (y = 3740 − d) | rows drawn | widths, quirk (predicted) | widths, plain clip (the alternative) |
|---|---|---|---|
| d = −300 or 7.5 (row 0) | 0..6 (y 3635–3740) | 5 7 9 11 13 13 13 | 13 13 13 11 9 7 5 |
| d = 22.5 (row 1) | 0..7 | 5 7 9 11 13 13 13 13 | 13 13 13 13 11 9 7 5 |
| d = 52.5 (row 3) | 0..9 | 5 7 9 11 13 13 13 13 13 11 | 11 13 13 13 13 13 11 9 7 5 |
| d = 82.5 (row 5) | 0..11 | 5 7 9 11 13 ... 9 7 | 7 9 11 13 ... 7 5 |
| d = 97.5 .. 127.5 (row 6..8) | full circle | control: quirk = plain | |

So for a source at the top edge the revealed world area is narrow (5 cells ≈ 75 units) at the edge and widest
(13 cells ≈ 195 units) 60–105 units below it, i.e. the circle is pushed 6 rows (≈ 90 units) toward −y; plain
clipping would be widest at the edge. Map / position limitation: no unit can stand near row 0: Polus' playable box
ends at y = 3440, 300 units = 20 rows below the edge (every shipped MP land map has ≥ 280 units), so in real play
only reveal radii ≥ ~300 units at the playable top edge hit the quirk. The probe therefore uses **position
reveals** (`FogOfWar.Reveal(human, Create_Position(x, 3740 − d, 0), 90)`), which reach any row whatever the playable
box; d = −300 tests the clamp of a point outside the box (row 0).

## How it is measured

- **Lua** (`fow_hook.lua` appended to the shipped `BusyTacticalFreeStore.lua`, the AI free-store script; the path the
  A1 probe proved in the retail game on 2026-10-31). In the AI player's land free store it lowers `ServiceRate` /
  `UnitServiceRate` to 0.05 s so it runs every 3rd frame (the shipped `FreeStoreService` / `On_Unit_Service` still run
  on their 20 s / 2 s schedule). At frame 180 it finds the human, spawns two AI-owned objects (Rebel: `MPTL`) at the
  AI object farthest from every human object (`Prevent_AI_Usage`, `Prevent_All_Fire`, `Make_Invulnerable`; skipped by
  the free store), then: 4 trials of `FogOfWar.Reveal(human, src, 90)` → hold ≥ 45 frames → `Undo_Reveal` at the
  wanted decay phase → 270 frames of samples; then 10 top-edge reveals (hold 9 frames, undo, 9-frame gap). Every
  step samples `Find_Nearest(src, "MPTL", human, false)` (FOW_Object_Hidden with force 1): returns the target while
  the human can see it. Records `FWR1|...|END` stay in a Lua table (retail script output functions are stubs).
- **Memory** (`fow_reader.py`, read-only `ReadProcessMemory`, a1mem.py layout): the human's FOW grid (mode +0x198 by
  player id, header §1.1), read once per frame: count 0↔>0 transitions (CUP / CDN), state → 0 transitions with the
  grid's last-decay frame (+0x44), and per released area the state at every decay (TRK). The Lua records are found by
  a heap scan every 5 s. The parser joins both: cell sets, state sequence, zero frame, visibility flip.

## What Gabriel does

1. Start Steam. Double-click **`tools\probe_mod\fow_regrow\run_fow.cmd`**. It builds and installs the mod, starts the
   reader, launches `StarWarsG.exe WINDOWED ... MODPATH=Mods\FowRegrowProbe` and prints the steps below.
2. Main menu > **Single Player** > **Skirmish** > **Land**. Map **(2) Polus Winds**. Slot 1 (you): **Empire**, team 1.
   Slot 2: **AI** (Easy) **Rebel**, team 2. Other slots closed. Default credits / tech. **Start**.
3. **Give no orders, click nothing on the map.** Wait **3 minutes** from the moment the battlefield is shown. (Two
   Rebel MPTL artillery units appear at the Rebel base after ~6 s: that is the probe. Do not attack them.)
4. **Esc > Quit** (to the main menu), then main menu > **Quit**. The run finishes by itself (`FOW_RUN_DONE exit=<n>`).
5. **Send back** the run folder `tools\probe_mod\fow_regrow\out\<yyyymmdd-hhmmss>\` (`report.txt` first; also
   `reader.log`, `run.json`, `geometry.txt`). `out\latest.txt` names the newest one.

Background / driven run: `run_fow.cmd --non-interactive` (progress in `out\<run>\run_status.txt`, `state=` installing /
waiting_for_game_start / game_running / collecting / done). `--no-launch`: start the game yourself with
`StarWarsG.exe MODPATH=Mods\FowRegrowProbe`. `--map NAME`: another land map (then pick that map in step 2). Exit codes:
0 report with probe data, 10 no land battle / no probe records, 20 preflight (game missing / already running),
30 interrupted, 31 error.

**Cleanup** is automatic at the end of every run (also after Ctrl+C or an error). After a crash double-click
**`cleanup_fow.cmd`** (safe to repeat): it deletes `corruption\Mods\FowRegrowProbe` only if it holds
`FOW_PROBE_MARKER.txt`. No retail file is changed; other mods are left alone.

## Reading the report

| result | meaning |
|---|---|
| Q1 **CONFIRMED**: every trial's states 238, 217, ..., 7, 0 at decays 16 frames apart, zero 193..208 frames after the undo (and `phase_exact`: at undo + 192 + d1), the object visible in every sample before that frame and hidden from it on | §4 / §5.2 as written; the port's decay and hidden test stand |
| Q1 window in range but not `phase_exact` (note "phase rule predicts ...") | the Lua call runs on the other side of the decay in the tick (or the Lua frame is one off the decay frame): fix §4.4's order, not the window |
| Q1 **REFUTED**: window outside 193..208, other state sequence or decay spacing | §4.2's step / table / cadence is wrong: the report shows the observed states |
| Q1 REFUTED with "hidden ... before the cells reached 0" / "still visible ... after" | the game-logic test is not "state == 0" (§5.2): note the state at the flip from the TRK lines |
| Q2 **CONFIRMED**: every reveal with centre row < r matches the shifted circle, the row ≥ r controls match the full circle | §2.2 top-edge quirk confirmed (keep it in the port) |
| Q2 **REFUTED**, label `plain` | the original clips normally; drop the quirk |
| Q2 REFUTED, label `other` | another shape: observed / quirk / plain spans are printed for that reveal |
| INCONCLUSIVE | no data: `GRIDS ... via=none` (grid not found at mode +0x198: the reader searched the mode object too), no `LUA` lines (hook not loaded or records not found: check `SRC` / `SCAN`), `ERR` records, `SPAWN ok=0` (Q1 then rests on the grid only), "counted again" (another source saw the cells), "baseline: object visible" |

The circle radius is read from the interior reveal (`R 90 -> r 6 (model) / N (observed)`); a different r means a
battle-scale reveal multiplier (§3.1) and the edge predictions use the observed r.

## Files

| file | what |
|---|---|
| `run_fow.cmd` → `fow_run.py` | build + install, reader, launch, wait, report, cleanup (`--non-interactive`, `--no-launch`, `--game`, `--map`, `--scan-sec`) |
| `cleanup_fow.cmd` → `fow_cleanup.py` | idempotent cleanup (`--dry-run`, `--no-pause`) |
| `build_fow_mod.py` | builds `Mods\FowRegrowProbe` (shipped free-store script + hook, map extent from the .ted); `--geometry-only` prints the map's FOW geometry |
| `fow_hook.lua` | the Lua appended to the shipped `BusyTacticalFreeStore.lua` |
| `fow_reader.py` | the recorder (log format in its docstring; imports `..\a1_tactical\a1mem.py`) |
| `fow_parse.py` | `parse <reader.log> [--out] [--json]` → report |
| `fow_model.py` | the spec model (geometry, midpoint circle with / without the quirk, decay, reference grid); `predict` |
| `selftest_model.py`, `selftest_parse.py`, `selftest_hook.py` | offline tests (below) |

All Python runs with the project venv: `toolchain\venv\Scripts\python.exe -I <script>`.

## Offline validation (2026-11-01)

- `selftest_model.py`: **SELFTEST_MODEL OK** — step 21, sequence 238..0, windows 193..208 over the 16 phases,
  interior circles symmetric (r 0..12), top-edge shift for centre rows < r, plain bottom-edge cut, Polus geometry.
- `selftest_parse.py`: **SELFTEST_PARSE OK** — grid discovery at mode +0x198 and by fallback search in a fake memory
  image; a reference grid runs the hook's schedule frame by frame (human base reveal, a patrol that also moves in an
  undo frame, frames skipped by the reader) through `FrameProcessor` and the parser: spec rules → Q1 CONFIRMED
  (208 / 193 / 200 / 204 frames, phase exact) and Q2 CONFIRMED; step 17 → Q1 REFUTED; hidden below state 16 → Q1
  REFUTED; no top-edge shift → Q2 REFUTED; no Lua records → INCONCLUSIVE.
- `selftest_hook.py`: **SELFTEST_HOOK OK** — the built script (shipped `BusyTacticalFreeStore.lua` from
  `extracted\foc_data_text.zip` + hook) under stock Lua 5.0.2 (built into `out\tools` at /Od) with stubbed engine calls
  for 120 s: rates lowered, a step every 3 frames, 4 trials with each undo at the wanted phase, 10 edge reveals, 14
  Reveal / Undo_Reveal pairs, DONE at frame 1764 (~59 s), no ERR, shipped FreeStoreService 5× (20 s) and
  On_Unit_Service on its 2 s schedule, never for the probe objects.
- `build_fow_mod.py` against the install builds the mod (free-store script from `64Patch.meg`); `run_fow.cmd
  --non-interactive --no-launch` on a scratch game folder installs, times out waiting for the game (exit 31) and
  deletes only `Mods\FowRegrowProbe`.
- Not validated (needs the retail game): the FOW grid location (mode +0x190 / +0x198 from FOG_OF_WAR §1.3; the reader
  falls back to searching the mode object), the land box extent (20·W vs 20·(W − 1)), whether the engine reads
  `ServiceRate` / `UnitServiceRate` after `Base_Definitions` each service (if not, the cadence falls to ~60 frames,
  shown as `Lua step cadence`; the grid timing does not depend on it), `Create_Generic_Object` / `Spawn_Unit` from
  a free-store script, `Find_Nearest`'s type filter on a spawned `MPTL`, and whether Lua's frame equals the decay frame
  (`phase_exact`).
