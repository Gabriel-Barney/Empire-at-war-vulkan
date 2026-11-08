# cu8g probes: stickiness default, missing fallback maps, LURE pulsing, cancel restore

In-game checks of three cu8f readings, for roadmap group cu8g (t07, t08, t09). Gabriel launches the game (CLAUDE.md
rule 7); everything else is scripted. Each session builds `Mods\CU8GProbe` from your install, installs it, launches
FoC with `MODPATH=Mods\CU8GProbe`, reads the probe records from the game's memory while it runs (read-only scan),
writes the report when the game exits and removes the mod again.

| Session | Command | Answers | Time |
|---|---|---|---|
| `stick` | `run_cu8g_stick.cmd` | t07: is a freshly spawned unit sticky? (`docs/re/CU8F_T04_T06_RE.md` §1.5) | about 5 min |
| `maps-miss` | `run_cu8g_maps_miss.cmd` | t08 part 1: what happens when no map matches and the fallback map is absent (`docs/re/CU8F_T03_T05_RE.md` §1.7) | 5 to 10 min |
| `maps-hit` | `run_cu8g_maps_hit.cmd` | t08 part 2: is an MP map with faction mini 05 patched to 1 picked instead? | about 5 min |
| `abil` (optional) | `run_cu8g_abil.cmd` | t09a: does LURE still pulse with `Is_Pulsing` No? t09b: when do abilities blocked by `Cancel_Special_Ability` work again after the user is removed? (`docs/re/CU8F_T04_T06_RE.md` §3.1, `docs/re/CU8F_T01_T02_T07_RE.md` §1) | about 12 min |

Run them one at a time (never two games at once), in any order. Steam must be running. All four sessions use the
same mod folder name, so each run replaces the previous one; the cleanup at the end of every run removes it.

## What you do

Double-click the `.cmd` (or run it from a terminal). It prints the steps, waits for a key, starts the game and then
waits until the game exits. The same steps are printed in the window; the in-game messages starting with
"CU8G PROBE:" tell you when a part is done. General rules for every session: give no orders unless a step asks for
one, never pause, never change the game speed, never save.

### `run_cu8g_stick.cmd` (t07)

1. Single Player > Skirmish > SPACE, map **KUAT**. Slot 1 (you): **REBEL**, team 1. Slot 2: **AI EMPIRE** (any
   difficulty), team 2. All other slots closed. Default credits and settings. Start.
2. Give no orders. "CU8G PROBE: stickiness trials running" appears at about 10 s. Six trials run in an empty corner of
   the map (frigates, a corvette, a TIE bomber appear and disappear). Wait for "CU8G PROBE: stickiness trials done"
   (3 to 4 minutes).
3. Esc > Quit to the main menu, quit the game.

### `run_cu8g_maps_miss.cmd` (t08, part 1)

The mod adds the campaign "CU8G PROBE: MISSING MAPS" (Abregado Rae, Jabiim, Thyferra). In it, Jabiim (Empire) has no
land map and Thyferra (Empire) no space map; both have Terrain Temperate, for which the game's fallback maps are not
shipped.

1. Single Player > Galactic Conquest > "CU8G PROBE: MISSING MAPS" > REBEL > Start.
2. LAND: send the ground units on Abregado Rae to **JABIIM** and choose to fight (never auto-resolve). Write down what
   happens: a map loads (take a screenshot once it has loaded), an error box, a return to the galaxy, an auto-resolve,
   a crash. If a battle loads: no orders for 30 s, screenshot, then Esc > Quit to the main menu.
3. SPACE: start the same campaign again, send the fleet at Abregado Rae to **THYFERRA**, choose to fight, note the
   same as in 2.
4. Quit the game. After a crash the run still finishes on its own; repeat the session only for the part not done.

### `run_cu8g_maps_hit.cmd` (t08, part 2)

Same campaign and the same step 2 (land, Jabiim) only. The mod also carries `_MP_LAND_CU8G_ALDERAAN.TED`, a copy of
`_MP_LAND_ALDERAAN.TED` with one byte changed: faction mini 05 from 3 to 1. If the game picks it, the battle is on
the Alderaan copy (green Alderaan terrain, the report shows the Alderaan box and props).

If the copy is **not** picked (same outcome as maps-miss), run it once more with the map loose in the game folder
instead of the mod folder, to tell "the picker ignores mod-folder maps" from "the picker ignores the patched value":

    run_cu8g_maps_hit.cmd --map-loose

This writes `corruption\Data\Art\Maps\_MP_LAND_CU8G_ALDERAAN.TED` (a new file name, no shipped file is replaced)
plus a marker; the cleanup deletes both, only if the file is still byte-identical.

### `run_cu8g_abil.cmd` (t09, optional)

A. Skirmish > LAND, map **DAGOBAH**. Slot 1 (you): **EMPIRE**, team 1. Slot 2: **AI REBEL**, team 2. Others closed.
   Start, no orders. A Canderous tank, a T2-B and three AT-STs appear in an open spot (three trials, all units
   invulnerable). Wait for "CU8G PROBE: lure trials done" (about 2 minutes), Esc > Quit to the main menu.
B. Skirmish > SPACE, map **KUAT**, same slots. Start, no orders. The Accuser, a Corellian corvette and Home One
   appear (two trials). Wait for "CU8G PROBE: cancel trials done" (about 3 minutes). Quit the battle and the game.

## What the probes do and record

Records are strings `C8G|<KIND>|p=<probe>|sid=..|n=..|f=<frame>|...|END`. The probe Lua is appended to the shipped
`Data\Scripts\FreeStore\BusyTacticalFreeStore.lua` (the free store of every AI player in a tactical battle) and arms
only in the AI player named per session; the galactic part of t08 is a story script of the probe campaign. The
runner scans the game's memory every 4 s and appends new records to `records.log`.

Since cu8g t10, a probe with a human side (stick, lure, cancel) arms only when that side's player `Is_Human()`. So the
main-menu background battle (two AI players) records `SKIP why=no_human_<faction>` and runs nothing.
- Every armed script writes a heartbeat record `HB` every 30 frames.
- In its first trial the lure probe also writes a step trace `ST`: one record before each engine call.
- So the report shows where a script stopped when a failed engine call stopped it ("no DONE: last heartbeat ...").
- The heap scan can miss a few records, so trust the heartbeat over "the last record".

- **stick** (`cu8g_stick.lua`): per trial a human Nebulon-B frigate F (stickiness 5.0 s in XML), an AI Tartan corvette
  V 800 units away (no fire, no AI) and two human helper frigates ordered to attack V (they raise V's damage rate so
  its time-to-kill drops under 4 s while its hull is still above the 0.33 low-health threshold). When F targets V and
  `V.Get_Time_Till_Dead()` <= 4 s, an AI TIE bomber N (Bomber: a better priority class for a frigate than a
  corvette; invulnerable, no fire, frozen) appears 350 units from F, closer than V. Arms: A no Lua call (spawn
  default), B `Set_Targeting_Stickiness_Time_Threshold(-1)` (= the XML 5 s), C the setter with 1000 (control: always
  sticky). Order A B C A B C. Recorded every 3 frames (0.1 s): F's attack target (V / N / helper / none), V's hull,
  shield, time till dead and damage rate, N's hull.
- **maps** (`cu8g_maps_story.lua`, `cu8g_maps_tact.lua`): on the galaxy every 2 s the owners of the three planets
  and the object counts; in any tactical battle, at 1 s and 20 s, per player the object count, the bounding box and up
  to 24 object types. A battle on the Alderaan copy shows Alderaan props and a box of about x 300..3540,
  y -165..3430.
- **abil / lure** (`cu8g_lure.lua`): AI Canderous tank L, AI T2-B decoy D, three human AT-STs (a LURE Target_Type).
  The AT-STs are ordered onto D, then L's LURE is activated (`Activate_Ability("LURE", true)`; a call with no second
  argument stops a retail script silently, so that form is tried last), and every 40 frames the AT-STs are ordered onto
  D again.
  A pulsing lure pulls them back to L within one pulse (0.5 s) while the ability lasts (6 s). Trials: patched tank
  (`Is_Pulsing` No in the mod's copy of `Units_Land_Underworld_Canderous_Tank.xml`), the captured tank (shipped
  `Is_Pulsing` Yes, control), patched tank again. Recorded every 3 frames: each AT-ST's attack target.
- **abil / cancel** (`cu8g_cancel.lua`): human Accuser T, AI Corellian corvette W 700 units away. Phase base: T's
  TRACTOR_BEAM on W. Phase cancel: AI Home One appears (the mod un-comments its `Akbar_Cancel_Special_Ability`
  block) and is ordered to attack T; at the phase start W is replaced by a fresh corvette, so the beam is off; T tries
  the beam every 30 frames. Phase kill: Home One is made vulnerable and given `Take_Damage` until it dies (no
  `Despawn`). Phase restore (from the first poll with Home One dead): T tries the beam every 3 frames. Removing Home
  One while T's beam is still running crashes retail (c0000005 at +0x7107c8, twice), which is why W is swapped.
  Recorded every 3 frames: distance T-W, beam ready / active, W under the beam, T's ENERGY_WEAPON state.

## Results

Every run writes `tools\probe_mod\cu8g_probes\out\<yyyymmdd-hhmmss>-<session>\` (`out\latest.txt` names the newest):
`report.txt` (read this), `records.log` (all records), `config.json` (the build: sites, trial values, patched
files), `run.json`, `run_status.txt`, `cleanup.log`. `out\` is not in git. Rebuild a report from records:
`..\..\..\toolchain\venv\Scripts\python.exe -I cu8g_run.py --session <s> --report-only out\<run>\records.log`.

### What each result decides

**t07** (report line `VERDICT t07`; per trial SWITCHED / KEPT / KEPT_LOW_HEALTH / OTHER / INCOMPLETE)

| Result | Meaning | Consequence |
|---|---|---|
| A switches to N, B and C keep V (CONFIRMED) | spawn default is 0.0, -1 means the XML value (§1.3) | the port rule of cu8f t04a stays; the foc-station cap seeds of cu8g t06 are not a stickiness artefact |
| A keeps V like B (DIFFERS) | the spawn default is the XML value | revert the spawn 0.0 of cu8f t04a (TD +0x158 starts at the type value), regenerate the goldens it moved |
| C keeps, B switches (PARTLY) | the setter works but -1 does not mean the XML value | check V's time till dead at N in the trial lines; reopen §1.2 |
| anything else (INCONCLUSIVE) | low health, no acquisition, N never targeted | read the trial lines; adjust `STICK` in `build_cu8g_mod.py` and repeat |

**t08** (your notes and screenshots plus the `t08` section: galactic owner changes, pauses, per battle the object box
and types)

| maps-miss outcome | Consequence for the port's map picker (cu8f t03) |
|---|---|
| a map loads | note which (screenshot, props in the report); the port loads that map in this case |
| error box, crash, or the game returns to the galaxy / auto-resolves | the port does the same (or the closest safe equivalent: skip the battle); record the observed behaviour in `docs/re/CU8F_T03_T05_RE.md` §1.7 |

| maps-hit outcome | Consequence |
|---|---|
| the Alderaan copy loads | the picker reads mini 05 and accepts mod-folder maps; port rule unchanged |
| not picked, but picked with `--map-loose` | the picker's map list only scans `Data\Art\Maps` of the game, not MODPATH; the port's list must do the same |
| not picked either way | the patched faction value alone does not make a map eligible; reopen §1.5 (another filter) |

**t09a** (`VERDICT t09a`): CONFIRMED = `Is_Pulsing` No still pulses, so the port keeps the field parsed and unread;
DIFFERS = No stops the repeat pulses (one pulse only), so the port must read `Is_Pulsing`; INCONCLUSIVE = the
control did not re-lure or LURE never activated (see the LACT records and APIERR).

**t09b** (read by hand, `t09b` section): the expected pattern is a pull in base, no pull and beam not ready (or the
call refused) in cancel, and the beam back within one poll (+0..+3 frames) in restore (`CRESTORED`). The frame of
`CRESTORED` relative to the despawn is the restore timing the port needs. No block at all in the cancel phase means
the cancel aura did not apply to the Accuser (its Combat_Automatic trigger is only [L] in the spec): the timing
question then stays open.

## Cleanup

Every run cleans up by itself at the end, also after an error, Ctrl+C or a crash of the game. To clean up by hand
(for example after the PC was switched off during a run):

    cleanup_cu8g.cmd

It removes `corruption\Mods\CU8GProbe` only if it carries `CU8G_PROBE_MARKER.txt`, and the loose
`corruption\Data\Art\Maps\_MP_LAND_CU8G_ALDERAAN.TED` only with its marker and unchanged content. `--dry-run` shows
what it would remove. Nothing outside these two names is touched.

## Options

`--game DIR` (default: `DEFAULT_GAME` in `../common/ted_owners.py`), `--windowed` (1920x1080 window, for a computer-use driven
run), `--no-launch` (install, print the command line, start the game yourself), `--non-interactive` (no key
prompts), `--dry-run` (build into `out\<run>\dryrun_mod`, print the steps, install nothing).

## Offline checks (done before handing the kit over)

    ..\..\..\toolchain\venv\Scripts\python.exe -I selftest_cu8g.py
    run_cu8g_stick.cmd --dry-run   (and the other three)

`selftest_cu8g.py` builds every session from the install (or from `extracted\foc_data_text.zip` without the install;
maps-hit needs the install), parses every built or patched XML and compares it with the shipped file, checks that the
Alderaan copy differs in exactly one byte, compiles every built Lua with stock Lua 5.0.2
(`..\common\build_lua50.cmd`), runs the hooks against a stubbed engine that follows the port's model and checks that
the parser gives the expected verdicts. It ends with `CU8G SELFTEST OK`.

Not checked offline (only the game can): what `Get_Attack_Target`, `Get_Time_Till_Dead`, `Activate_Ability` (the
argument kinds tried in turn), `Is_Under_Effects_Of_Ability` and spawning a single TIE bomber really do in retail;
whether the retail free store reaches the hooks in a skirmish; whether the game accepts the probe campaign XML; and
the in-game messages (they need the patched text file).
