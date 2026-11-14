# s78n t14 probe kit: land-squad formation (member lag, start delays, Rows / Dartboard slots)

Retail check of the land-squad port guesses in `docs/re/FIGHTER_FLYING.md` sec. 7 to 7.4:

- **Q1 member lag.** How far members trail their slot or the container on a long straight march, and whether they
  close up after the stop. In the port, members move at most at the team speed, so they never make up a start or turn
  lag. The port's fixture ends about 30 units behind.
- **Q2 start delays.** Each member waits a delay of 0..`Max_Move_Frame_Delay` (6) frames before it starts. In the
  game this shows as start frames staggered by up to 6.
- **Q3 layout.** `Stormtrooper_Team` uses **Rows**: a 3 x 3 grid of pitch 12 with the member `FormationSpacing` 0.3,
  or pitch 26 with the container's 1.0. `Rebel_Trooper_Team` uses **Dartboard**: a Halton (bases 2 and 3) scatter
  seeded with container id x NOW (sec. 7.2). The kit reads where the slots sit relative to the container after a
  move and fits the exact sec. 7.2 scatter to them. It also checks the re-lay rule: the layout is laid again only when
  dot(new heading, last laid heading) <= 0.99 (8.11 deg), so a 30 deg turn re-lays it and a 0 or 5 deg turn does not.
- **7.7 container speed.** The container accelerates and brakes at the member `OverrideDeceleration` x 1.56, and its
  speed is capped while a member straggles behind the layout box (the straggler cap 0x5ab640).

## Changes for the second run (roadmap s78n t17)

The first run (`out\20261110-043421`, `docs/re/S78N_T14_RUN.md`) could not test the Dartboard seed: the records
carried only the mode clock, and the jiggle (3.5) hid the slots. This version:

- **NOW clock.** Every poll and every order also records g = `GetCurrentTime.Frame()` x 30 (the logic frame NOW, the
  Halton seed's frame), next to f = `GetCurrentTime()` x 30. The call runs under pcall: if it fails, one APIERR
  `api=GetCurrentTime.Frame` is recorded, g is left out from then on and the script keeps running.
- **No jiggle for the Rebel troopers.** The probe mod's copy of `GROUNDINFANTRY.XML` sets `Squad_Rebel_Trooper`
  `FormationRaggedness` and `MaxJiggleDistance` to 0 (shipped 1.6 and 3.5), so the settled slots are the Halton
  points themselves. Only the mod's copy changes; `--keep-jiggle` builds the mod without the patch.
- **Dartboard fit as sec. 7.2.** For each leg the analyser lays the scatter in float32 exactly as sec. 7.2: k = ceil
  sqrt n, m = ceil(n / k), L / Wd from the spacing and the member half-extents, minimum distance 5 + r, up to 1000
  tries, the radical inverse in float32 with bases 2 and 3 from the same index, seed = (int32) container id x NOW
  (wrapping). NOW is searched only in [g, g + 10] of the spawn and of each order up to that leg. The points are
  assigned to the members in the retail order (sec. 7.2 step 6: points by x descending, members by projection
  descending, groups by x[i] - x[j] <= Wd / m sorted by side, the next group from i = j + 1). The fit is compared
  with 200 NOW values far from the leg (their 5 % quantile, since Halton scatters repeat at some index offsets).
- **Re-lay rule from the actual path.** The turn from the last laid layout uses the container's measured heading at
  the start of each leg; turns within 1.5 deg of 8.11 deg are not judged.
- **7.7 checks.** The container's start and braking ramps (a parabola fitted to the distance walked) against the
  member `OverrideDeceleration` x 1.56, and the straggler cap solved for HX at each steady poll against the layout.
- **Q1 CLOSES / STAYS OPEN** skips a leg without settled slots (the first run's STAYS OPEN came from such a leg).
- Records from before this version (no g) still analyse; the NOW checks then print n/a.

### Expected values (FIGHTER_FLYING 7.7, printed by `--dry-run`)

| Kind | Team speed / frame | Container acceleration = braking / frame | Straggler box HX |
|---|---|---|---|
| Rows (Stormtroopers) | 1.248 | 0.078 (0.05 x 1.56) | 15.0 (3 x 3 grid pitch 12, half-extent 3; first run 14.9) |
| Dartboard (Rebel troopers) | 1.56 | 0.1092 (0.07 x 1.56) | per layout: (max - min) / 2 of the laid points' x +- 3, jiggle 0; the report prints predicted vs measured per leg |

The first run already measured 0.078 / 0.109 and HX 14.9; the re-run must repeat them with the patched XML.

## Run (one land battle, about 4.3 minutes in battle, hands-off)

1. From the project root, in a normal PowerShell or cmd window, start the runner. Use a normal window, not a Claude
   terminal tab, so the game is not a child of Claude's shell. Close the game first and make sure Steam is running.

   ```
   tools\probe_mod\s78n_t14_squads\run_s78n_t14.cmd --no-launch --non-interactive
   ```

2. Then launch the game, in a second window or from a Claude shell:

   ```
   toolchain\venv\Scripts\python.exe -I tools\probe_mod\common\launch_foc.py MODPATH=Mods\S78NT14Probe
   ```

   Launch the game WINDOWED. Without `--no-launch` the runner launches it itself (add `--windowed`).

### Menu steps

1. Main menu > Single Player > Skirmish > **Land**, map **Geonosis**.
2. Set up the slots:
   - Slot 1 (you): **Empire**, team 1.
   - Slot 2: **AI Easy**, **Rebel**, team 2.
   - Every other slot: **Closed**.
   Keep the default credits and settings, then Start.
3. Leave the battle alone:
   - Give no orders and click nothing on the map. Moving the camera is fine; the probe points it at the squads.
   - Never pause, never change the game speed, never save.
   - Keep the game window in front (FoC pauses on focus loss).
4. "S78N T14 PROBE: squad formation trials armed" appears in the first seconds. From 15 s on, four squads appear
   one after another on a flat corridor far from both bases: Stormtroopers, Rebel troopers, Stormtroopers, Rebel
   troopers. All of them are yours and invulnerable. Each squad marches four legs, one leg at a time. The squad is
   removed after its last leg.
5. Wait for "S78N T14 PROBE: squad formation trials recorded" (about 4.3 minutes in battle). Then press Esc > Quit
   and quit the game.

The runner then:
- writes `out\<run>\report.txt` and `report.json`,
- removes `Mods\S78NT14Probe`,
- restores the profile,
- ends with `S78N_T14_RUN_DONE exit=0`.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | records found |
| 10 | no records |
| 20 | preflight: the game is missing or already running |
| 30 | interrupted |
| 31 | error |

Options:

| Option | Effect |
|---|---|
| `--map NAME` | Use another land map (default `_MP_LAND_GEONOSIS`). The builder searches that map for a corridor; Dagobah has none far enough from the starts. |
| `--reps N` | Trials per squad kind (default 2). |
| `--report-only out\<run>\records.log` | Rebuild a report from an earlier run. |

## How it works

### The mod
`build_s78n_t14_mod.py` builds `Mods\S78NT14Probe` from the install, adding only three files and the marker:
- `Data\Scripts\FreeStore\BusyTacticalFreeStore.lua`: the shipped free store with the probe appended.
- `Data\Text\MasterTextFile_English.dat`: the shipped text plus the two probe messages.
- `Data\XML\GROUNDINFANTRY.XML`: the shipped file with `Squad_Rebel_Trooper` `FormationRaggedness` and
  `MaxJiggleDistance` set to 0 (read back after patching; not with `--keep-jiggle`).

### The corridor
The builder parses the map placements (`ted_owners`) and the passability grid (`build_oi_mod`). It picks a straight,
flat corridor at least 1400 units long, with map objects at least 40 units away and far from every start position.

On Geonosis the corridor starts at (2000, 800) with heading 165 deg, 2374 units from the nearest start.

The legs:

| Leg | Name | Turn | Length | Heading |
|---|---|---|---|---|
| 1 | `first_600` | 0 deg | 600 | 165 deg |
| 2 | `straight_200` | 0 deg | 200 | 165 deg |
| 3 | `turn30_400` | 30 deg | 400 | 195 deg |
| 4 | `turn5_200` | 5 deg | 200 | 200 deg |

### Squads and XML values
The builder reads these values from the game XML (see `--dry-run`). The fallback companies are
`Imperial_Stormtrooper_Squad` and `Rebel_Infantry_Squad`.

| Kind | Company | Team | Members |
|---|---|---|---|
| Rows | `Imperial_Medium_Stormtrooper_Squad` | `Stormtrooper_Team` | 9 x `Squad_Stormtrooper` |
| Dartboard | `Rebel_Mini_Infantry_Squad` | `Rebel_Trooper_Team` | 5 x `Squad_Rebel_Trooper` |

### The probe Lua (`s78n_t14_probe.lua`)
- **Arming.** The probe runs only in the Rebel AI's free store, only in a Land battle, and only when the Empire is
  human. Otherwise it records SKIP.
- **Polling.** The service rate is 0.05, so the probe polls every 3 frames. f = `GetCurrentTime()` x 30 (mode
  frame), g = `GetCurrentTime.Frame()` x 30 (NOW) in every record; the HB record also carries both raw times.
- **Spawning.** Each trial spawns the company for the human player at the corridor start. It finds the team
  container through `Get_Parent_Object` of the members and despawns any extra teams. It then makes the squad
  invulnerable, points the camera at it and lets it settle for 180 frames.
- **Ordering.** For each leg it gives `Move_To` to the container. It re-orders through a member if the container
  does not move within 90 frames.
- **Recording.**
  - Every poll records the container and every member position.
  - Events are recorded as CSTART, CSTOP (3 still polls) and LEGTO (timeout, 1200 frames).
  - Each squad holds still for 120 frames after the stop, then the next leg is ordered.
- **Records.** Records are strings `S8Q|KIND|...|END`. The probe keeps them in a global table. The runner finds them
  with a read-only scan of the game's memory every 4 s (`../a1_tactical/a1mem.py`); it never writes the game's
  memory. When the run is finished the probe records DONE.

## What the report prints

The report is `out\<run>\report.txt`; `report.json` holds the same numbers.

- **Header.**
  - Script state (ARMED / DONE / SKIP, ERR, APIERR).
  - **Clock.** The range of g - f (NOW minus the mode frame), or a note that no record carries g.
  - **Dartboard layout fit** per Dartboard leg: the best NOW in [g, g + 10] of the spawn or an order, its score
    (along-weighted RMS, a common along shift up to 2 allowed), the baseline's 5 % quantile and the plain RMS. A leg
    matches when the score is at most 0.75 and at most 0.5 x the baseline.
- **Per trial:** the company, container id, member count and poll count. It also counts how often the container
  position was readable and how often it equalled a member position.
- **Per leg:**
  - **Container.** Start (fitted from the order), cruise speed per frame, heading off the plan, and the stop frame.
  - **Q2 member start.** Frames after the earliest member, from an interpolated 1.5-unit crossing, plus the spread.
  - **Q1 lag table.** At container start +0/15/30/60/120/240/360/480, pre-stop and stop +0/15/30/60/90 it prints
    lag mean / max (positive = behind), side |err| mean and max distance. Each line has two columns: against the
    settled slot (the squad's own layout after the stop) and against the predicted layout (the Rows grid, or the
    fitted Halton scatter).
  - **Steady march.** Lag and p90 distance over the polls at cruise speed.
  - **Member speed.** Peak member speed / container cruise: above 1 means members run faster than the team to catch
    up.
  - **Close-up times.** The frames until all members are within 4 + `MaxJiggleDistance` of their slot, while
    marching and after the stop.
  - **Q3 settled slots** (along, side). For Rows: the RMS to the member-spacing grid and to the container-spacing
    grid. For Dartboard: the bounds check and the Halton score with the event it was laid at (spawn or a leg order)
    and the NOW offset. Also the change from the previous leg's slots: the set RMS (layout change) and the per-member
    RMS (members swapped).
  - **Path heading** at the start and the stop of the leg.
  - **7.7 container speed step.** The start and braking ramps per frame and the polls each was fitted through.
  - **7.7 straggler cap.** HX solved at each steady poll (median and range) against the predicted HX (Rows grid,
    fitted Halton points, or the settled slots).
- **Summary per squad kind, with the verdicts:**

  | Question | Verdicts and evidence |
  |---|---|
  | Q1 | **TIGHT / LOOSE / TRAILING** from the steady march against the predicted slots (or the settled slots if no prediction); settled centroid along the heading; **catch-up YES/NO** (peak ratio > 1.05); **CLOSES / STAYS OPEN** after the stop |
  | Q2 | **STAGGERED / SIMULTANEOUS** from the start spread on the straight leg 2 (>= 2.5 or <= 1.0 frames); a histogram of rounded start frames; the min / max over all legs |
  | Q3 Rows | **MATCH** when the member-spacing RMS is at most 1.5 + jiggle and below the container-spacing RMS (the port uses the member spacing) |
  | Q1 close | **CLOSES / STAYS OPEN** over the legs with settled slots; legs without them are counted as skipped |
  | Q3 Dartboard | **MATCH** (every leg's NOW fit matches), **PARTIAL** (some legs), **NO MATCH**, or **n/a** for records without g |
  | Q3 slot order | **MATCH** when, on every matched leg laid at an order, the retail order (sec. 7.2 step 6) scores within 0.5 of the best assignment |
  | Re-lay rule | **MATCH** when each matched leg was laid at the event the 8.11 deg rule picks from the measured headings (`FIGHTER_FLYING` 7.2) |
  | 7.7 acceleration | **MATCH** when every start and braking ramp fitted through 4 or more polls is within 10 % of the expected value; ramps from 3 polls are printed in brackets and not judged |
  | 7.7 straggler | **MATCH** when each leg's median HX is within 1.0 of the predicted HX |

  The set change per leg is printed too: legs 2 and 4 should show jiggle only; leg 3 should show a new scatter for
  Dartboard, and the same grid (members may swap) for Rows.

How to read the result against the port:
- **Q1.** TRAILING with STAYS OPEN and catch-up NO matches the port as it is. TIGHT with CLOSES and catch-up YES
  means retail members do catch up, and the port's member vmax = team speed is wrong.
- **Q2.** STAGGERED with a spread of up to about 6 frames confirms the start delay.

## Offline checks (no game)

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\s78n_t14_squads\selftest_s78n_t14.py
tools\probe_mod\s78n_t14_squads\run_s78n_t14.cmd --dry-run --non-interactive
toolchain\venv\Scripts\python.exe -I tools\probe_mod\s78n_t14_squads\build_s78n_t14_mod.py --map-info
```

The selftest:
- compiles the probe with Lua 5.0 (`../out/tools/lua50.exe`);
- checks that the probe uses no `math.`, `#` or `%`;
- checks the XML values and that the run fits in 9000 frames.

It also checks the probe mod's XML patch and the analyser's float32 maths (radical inverse, `dart_dims`, the
Dartboard points, the retail slot order with the i = j + 1 skip, the Rows grid HX of 15.0). It then runs the probe
against a stubbed engine with a squad model in seven worlds. The stub's NOW runs 1234 frames ahead of the mode clock,
lays a Dartboard scatter at the container's next service after an order (NOW + 1) with the sec. 7.2 seed and order,
and moves the container with the 7.7 acceleration and straggler cap.

| World | What it models | Expected result |
|---|---|---|
| retail | members at their own Max_Speed, recover past 4 units, delays 0..6, 7.7 acceleration and cap | Q1 TIGHT / CLOSES, Q2 STAGGERED, Q3 MATCH for both kinds, every Dartboard leg fitted at NOW + 1 of the spawn or the leg 3 order, slot order and re-lay rule MATCH, 7.7 acceleration and straggler MATCH, g - f = 1234 |
| port | members limited to the team speed, no recovery, no cap, the container type's own acceleration | Rows TRAILING / STAYS OPEN / catch-up NO, 7.7 acceleration DIFFERS |
| nodelay | no start delays | Q2 SIMULTANEOUS |
| badseed | Dartboard seeded with id x (NOW + 777) | Dartboard Q3 NO MATCH, no leg matched |
| noframe | `GetCurrentTime.Frame` missing | one APIERR, Dartboard Q3 n/a, Rows 7.7 acceleration still MATCH |
| space | a Space battle | SKIP, nothing analysed |
| menu | Empire not human | SKIP, nothing analysed |

It ends with `S78N T14 SELFTEST OK`. Add `--keep` to keep the work folder with every world's report.

`--dry-run` builds the mod into `out\<run>\dryrun_mod` and prints the plan, the XML values, the steps and the command
line. Nothing is installed.

## Cleanup

```
tools\probe_mod\s78n_t14_squads\cleanup_s78n_t14.cmd
```

The runner already does this at the end of every run. The script removes `Mods\S78NT14Probe`, but only if it holds
`S78NT14_PROBE_MARKER.txt`. It also restores the profile snapshot of the newest run (`out\latest.txt`), unless that
snapshot was already restored or the game is running. It is safe to repeat. Use `--dry-run` to only show what it
would do.

## If something is missing

- **No records, or only SKIP.** The battle was not Land, or the Empire was not the human player, or the Rebel AI
  slot was missing.
- **TRIAL without TEAM.** Neither the company nor its fallback spawned, or the members had no parent container.
- **LEGTO.** The squad did not stop within 1200 frames. That leg's numbers use the last poll as the stop.
- **ERR / APIERR records.** A failed engine read; retail stops the script silently. The records up to that point
  are still analysed.

## Known limits

- **Poll granularity.** Polls come every 3 frames. Start frames are interpolated from a 1.5-unit crossing, so the
  Q2 starts are good to about 1 frame. A spread below 1 frame reads as SIMULTANEOUS.
- **Jiggle noise.** Stormtrooper slots carry the jiggle (`MaxJiggleDistance` 2) and up to the 4-unit recovery dead
  band, so the Rows grid match allows 1.5 + jiggle. The Rebel troopers run with jiggle 0 in the probe mod.
- **Halton seed frame.** The fit searches NOW in [g, g + 10] of each event; with g missing (old records or a failed
  `GetCurrentTime.Frame`) it cannot run and prints n/a. A Dartboard NO MATCH with g present means the seed, the
  float32 maths or the event that lays the scatter differs from sec. 7.2.
- **Ramps from few polls.** At 3-frame polls a short ramp may hold only 3 polls; such a ramp is printed but not
  judged.
- **Container on a member.** At spawn the container position may equal a member's position. The header counts
  this, and the analysis then uses the container position as given.
- **Starting state.** Members may still be moving at the leg 1 order, after the 180-frame settle from the spawn. The
  leg line counts them ("members moving at the order"). Leg 1 is reported, but Q2 is judged on the straight leg 2.
