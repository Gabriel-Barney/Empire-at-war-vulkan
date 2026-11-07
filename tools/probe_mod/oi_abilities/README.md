# OI ability probe (ABILITIES_HARDPOINTS §4.12 / §4.13)

In-game probe for two `[port]` guesses in `docs/re/ABILITIES_HARDPOINTS.md`:

- **§4.12 `Destroy_When_Stunned_Over_Water`**: does a stunned `MAL_Rocket_Vehicle` die over water, and when? The
  options are at once, at the stun end, only on deep water, or never.
- **§4.13 clip timings**: the frame of each event, counted in sim frames from the start of its clip:
  - Rancor `Rancor_Pound_Vehicles`: the damage frame.
  - Rancor `Rancor_Eat_Infantry`: the attach and destruction frames.
  - Infiltrator `STICKY_BOMB`: the frame the projectile appears, and the frame the thrower resumes.
  - Yoda `BERSERKER`: the frame of each damage pass. The port assumes clip 0x68 loops, needs counter % 5 = 0, and
    only hits between 20 % and 80 % of the clip.

The probe does not play a game for you. One AI player's free-store script, the shipped
`BusyTacticalFreeStore.lua` with this hook appended, spawns the units and runs seven fixed scenarios.
A read-only memory reader (`oi_reader.py`, built on `../a1_tactical/a1mem.py`) records each spawned object's
health, flags, owner, position and primary animation slot on every frame it can see.
`oi_parse.py` turns the recording into `report.txt`.

## What Gabriel does

1. Start Steam. Double-click `run_oi.cmd` in this folder. It builds `Mods\OIAbilityProbe` from your install,
   starts the reader, and launches FoC **windowed** with `MODPATH=Mods\OIAbilityProbe`.
2. In the game, go to **Single Player > Skirmish > LAND**, map **DAGOBAH**, and set the slots:
   - Slot 1 (you): **EMPIRE**, team 1.
   - Slot 2: **AI** (any difficulty), **REBEL**, team 2. The AI must be Rebel, because only the Rebel AI runs the
     probe.
   - Every other slot: **closed**.
   - Credits and tech: defaults.

   Start the battle.
3. Give **no orders** and click nothing on the map. Moving the camera is fine. The probe spawns its units away from the
   start positions: Han Solo and MAL rocket vehicles on land, in shallow water and in deep
   water, then a Rancor, a Rebel Infiltrator and Yoda. **Wait 5 minutes** from the moment the battlefield shows.
   The scenarios start 15 s in and finish after about 3.9 minutes.
4. Press **Esc > Quit** to go back to the main menu, then quit the game. The window running `run_oi.cmd` writes the
   report and removes the mod folder by itself.
5. Send back `out\<newest run>\`. The folder name is also written in `out\latest.txt`.

No manual step is needed: the water spawn points come from the map file. If anything went wrong, `cleanup_oi.cmd`
removes `Mods\OIAbilityProbe`. It deletes that folder only when it holds `OI_PROBE_MARKER.txt`, and nothing else.

## What the report says, and what each result means for the port

`report.txt` has one block per scenario, followed by a summary for §4.12. Frames are sim frames. A clip's start is
worked out from samples of the clip position: start = frame − position / slope.

### §4.12 `Destroy_When_Stunned_Over_Water`

| Scenario | Spawn point on Dagobah | Port prediction |
|---|---|---|
| `MAL_LAND` | (4120, 840), dry land | `SURVIVES`, and moves again about 600 frames after the stun |
| `MAL_SHALLOW` | (3480, 840), 3.8 under the water plane | `DIES_AT_ONCE`, within 6 frames of the stun |
| `MAL_DEEP` | (3480, 1680), 46 under the water plane | `DIES_AT_ONCE` |

The summary line maps the three results to one action for the port:

| Result | What it means for the port |
|---|---|
| `CONFIRMED` | Keep the rule as it is. |
| `ONLY DEEP WATER` | Add a depth threshold, somewhere between 3.8 and 46. |
| `STUN END` | Defer the death to when the stun expires. |
| `NEVER` | The tag has no visible effect with this stun source. |
| `NO_STUN` | Nothing to decide yet: read the `ACT` / `APIERR` lines. |

### §4.13 clip timings

| Scenario | Measured (memory reader) | Port prediction (slope = clip frames per sim frame) |
|---|---|---|
| `RANCOR_POUND` | Pound_Attack (63) start; tank health drop | first frame with position ≥ 20, i.e. ceil(20 / slope); slope = rate × 0.75 |
| `RANCOR_EAT` | Eat_Attack (64) start; victim re-owned (attach); victim dying | position ≥ 14 (attach), position ≥ 110 (destroy); slope = rate × 1.0 |
| `GRENADE` | Bombtoss (47) start; `PROJ_STICKY_BOMB` appears; clip end; thrower's next clip | position ≥ 12 (spawn); end at ceil(max(count − 2, 0) / slope) |
| `BERSERKER` | SpinMove (104) start and loop length; every health drop of the two tanks | passes at n with n % 5 = 0 and 0.2·count < (n·slope) mod (count − 1) < 0.8·count; the report prints `MATCHES` or `DIFFERS FROM` |

If a number differs from its prediction, change the timing for that class in `engine/tactical/abilities`, as
documented in §4.13. If the Berserker pass gaps are not multiples of 5, or the passes fall outside 20–80 %, the
model in §4.13 is wrong.

## Files

| File | What it does |
|---|---|
| `run_oi.cmd` / `oi_run.py` | One session: build, start the reader, launch the game, wait, parse, clean up. Exit codes: 0 report written; 10 no armed run; 20 preflight failed; 30 interrupted; 31 error. Options: `--no-launch`, `--non-interactive`, `--game DIR` |
| `build_oi_mod.py` | Builds the mod from the install (or `--source-zip`). The output is the shipped free-store script, plus a generated `OIP_CFG` table of spawn points read from the map's `.ted` file, plus `oi_hook.lua`. `--map-info` prints the chosen spawn points |
| `oi_hook.lua` | The probe itself, in Lua 5.0. See "How the hook works" below |
| `oi_reader.py` | Read-only memory recorder. Finds the `OIP1|` records with a heap scan, walks the object id map, and polls the watched objects. Finds the object-to-model pointer offset by a search (`MODELOFF`). `--selftest` |
| `oi_parse.py` | `parse reader.log --out report.txt --json report.json`. `--selftest` runs a synthetic log built from the port model |
| `oi_cleanup.py` / `cleanup_oi.cmd` | Deletes `Mods\OIAbilityProbe`, but only when the marker file is there. `--dry-run` |
| `selftest/oi_selftest.py` (Lua: `../common/build_lua50.cmd`) | Offline check, see below |

How the hook works:

- **Which AI runs it.** Only the free store of the AI player whose faction is `REBEL`, in a land battle, arms the
  probe. Every other AI player records `SKIP` and keeps the shipped behaviour. Retail runs `Base_Definitions` before
  it sets `PlayerObject`, so the decision is retried from the first service calls until the faction is readable
  (the first retail run, 2026-11-04, skipped itself with `pid=? fac=?`).
- **MAL placement.** The MAL is created with `Create_Generic_Object` at its point (`SPAWN how=cfg+generic`);
  `Spawn_Unit` moves a land unit off water cells. If it is not there, it is driven there (up to 400 frames), and
  Han is spawned within 120 of it once it arrives (`MALPOS`): the stun blast is centred on Han, radius 150.
- **How often it runs.** It lowers `ServiceRate` to 0.05 s, so the script runs on every free-store service (every
  3 frames). It is a state machine driven from `FreeStoreService`, with `On_Unit_Service` as a fallback. It uses no
  thread and no yield inside `pcall`.
- **Failures.** Every engine call is `pcall`-guarded. A failing API is recorded once per scenario as `APIERR`, and
  the next scenario runs anyway.
- **When it finishes.** When it is done it restores `ServiceRate` to 20.

## Offline check (no game)

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\oi_abilities\selftest\oi_selftest.py
toolchain\venv\Scripts\python.exe -I tools\probe_mod\oi_abilities\build_oi_mod.py --source-zip extracted\foc_data_text.zip --out <temp dir>
```

The selftest does three things:

1. It runs the hooked script under a stock Lua 5.0.2 (`tools\probe_mod\out\tools\lua50.exe`), which is also the syntax check. Stub
   engine calls and two planted failures check that:
   - the failing API is recorded and the next attempt still runs;
   - a missing type falls back to its alternative;
   - the seven scenarios run in order and the Rancor is carried over from Pound to Eat;
   - `ServiceRate` is restored at the end;
   - a non-Rebel AI only records `SKIP`.
2. It passes those records through the parser.
3. It runs the reader and parser selftests.

## Results (2026-11-04)

Four retail runs played by Claude (`out61104-*`, not in git). Run 1: the probe skipped itself (the
`PlayerObject` timing above). Runs 2 and 3: every clip-timing scenario, MALs only on the shore. Run 4: MALs on their
points, land and shallow only. The findings are in `docs/re/ABILITIES_HARDPOINTS.md` §4.12 / §4.13 "Probe result":
§4.12 confirmed; Pound, Eat and Grenade match the port; the Berserker cadence and window match, while retail plays
SpinMove variant 1 and restarts its loop irregularly.

## Known limits (not checkable offline)

- **Reading the clip position needs the model pointer.** The offset from the object to its `ModelClass` is not
  documented, so the reader searches for it: one or two pointer levels deep, a valid animation slot, and model
  time = frame / 30. If `MODELOFF_FAIL` appears, the clip starts are missing. The event frames are still measured,
  relative to the Lua `FIRED` / `ACT` records, at 3-frame resolution.
- **Argument kinds for `Activate_Ability`.** The hook tries them in order: position, object, none, bool for
  `AREA_EFFECT_STUN`; object first for the others. The `ACT` lines show which one fired.
- **Projectiles.** If they are not in the object id map, the stun frame comes from Lua `FIRED`, and the grenade
  spawn comes from the Lua `proj` count, both at 3-frame resolution.
- **The Rancor might Swat instead of Eat.** It may use `Swat` (chance 0.5) instead of Eat. Three troopers are
  offered, 300 frames apart.
- **The hook stops after a placed MAL dies.** In the 2026-11-04 run the AI script produced no record after the
  shallow-water MAL (made by `Create_Generic_Object`) was destroyed, so the deep scenario and the rest never ran.
  The parser copes (a missing scenario end is taken from the next start, a MAL without a `SPAWN` record is bound by
  type and frame). The cause is not known; a likely one is a method call on the destroyed object's handle.
- **Records are found in address order.** The parser sorts each run's records by Lua frame before grouping them
  into scenarios.
- **Read races.** Reads can be off by about ±1 frame. The reader polls every 4 ms, while the game runs a frame
  every 33 ms.
