# s78n t14 probe kit: land-squad formation (member lag, start delays, Rows / Dartboard slots)

Retail check of the land-squad port guesses in `docs/re/FIGHTER_FLYING.md` sec. 7 to 7.4:

- **Q1 member lag.** How far members trail their slot or the container on a long straight march, and whether they
  close up after the stop. In the port, members move at most at the team speed, so they never make up a start or turn
  lag. The port's fixture ends about 30 units behind.
- **Q2 start delays.** Each member waits a delay of 0..`Max_Move_Frame_Delay` (6) frames before it starts. In the
  game this shows as start frames staggered by up to 6.
- **Q3 layout.** `Stormtrooper_Team` uses **Rows**: a 3 x 3 grid of pitch 12 with the member `FormationSpacing` 0.3,
  or pitch 26 with the container's 1.0. `Rebel_Trooper_Team` uses **Dartboard**: a Halton (bases 2 and 3) scatter
  seeded with container id x frame. The kit reads where the slots sit relative to the container after a move. It
  also checks the re-lay rule: the layout is laid again only when dot(new heading, last laid heading) <= 0.99, so a
  30 deg turn re-lays it and a 0 or 5 deg turn does not.

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
`build_s78n_t14_mod.py` builds `Mods\S78NT14Probe` from the install, adding only two files and the marker:
- `Data\Scripts\FreeStore\BusyTacticalFreeStore.lua`: the shipped free store with the probe appended.
- `Data\Text\MasterTextFile_English.dat`: the shipped text plus the two probe messages.

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
- **Polling.** The service rate is 0.05, so the probe polls every 3 frames. f = `GetCurrentTime()` x 30.
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
  - **Dartboard layout fit** over all Dartboard legs of the run. The fit computes one frame offset (logic frame
    minus f) and its mean RMS, then repeats the same search at far offsets as a baseline. It reports **MATCH** when
    the best RMS is at most 0.6 x the baseline's best.
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
    grid. For Dartboard: the bounds check and the Halton RMS with the event it was laid at (spawn or a leg order).
    Also the change from the previous leg's slots: the set RMS (layout change) and the per-member RMS (members
    swapped).
- **Summary per squad kind, with the verdicts:**

  | Question | Verdicts and evidence |
  |---|---|
  | Q1 | **TIGHT / LOOSE / TRAILING** from the steady march against the predicted slots (or the settled slots if no prediction); settled centroid along the heading; **catch-up YES/NO** (peak ratio > 1.05); **CLOSES / STAYS OPEN** after the stop |
  | Q2 | **STAGGERED / SIMULTANEOUS** from the start spread on the straight leg 2 (>= 2.5 or <= 1.0 frames); a histogram of rounded start frames; the min / max over all legs |
  | Q3 Rows | **MATCH** when the member-spacing RMS is at most 1.5 + jiggle and below the container-spacing RMS (the port uses the member spacing) |
  | Q3 Dartboard | **MATCH** (the Halton run fit matches), **INSIDE BOUNDS ONLY** or **DIFFERS** |
  | Re-lay rule | **MATCH** when leg 2 keeps the leg 1 layout and legs 3 and 4 use the layout laid at the leg 3 order, as in the port and the RE (`FIGHTER_FLYING` 7.2) |

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

It then runs the probe against a stubbed engine with a squad model in five worlds:

| World | What it models | Expected result |
|---|---|---|
| retail-like | members catch up at 1.5 x the team speed, recover past 4 units, delays 0..6 | Q1 TIGHT / CLOSES / catch-up YES, Q2 STAGGERED, Q3 MATCH for both kinds, Dartboard fit at offset 0, re-lay rule MATCH |
| port-like | members limited to the team speed | Rows TRAILING / STAYS OPEN / catch-up NO |
| nodelay | no start delays | Q2 SIMULTANEOUS |
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
- **Jiggle noise.** Settled slots carry the jiggle (`MaxJiggleDistance` 2 for Stormtroopers, 3.5 for Rebel
  troopers) and up to the 4-unit recovery dead band. So the Rows grid match allows 1.5 + jiggle, and the Dartboard
  fit uses the slots during the steady march (where members steer to the slot itself) instead.
- **Halton seed frame.** The seed uses the logic frame (NOW), but the records carry f = `GetCurrentTime()` x 30.
  The fit therefore searches one run-wide offset (+-60 frames, then +-3 per leg). A Dartboard "no match" can also
  mean that the seed is not container id x NOW.
- **Container on a member.** At spawn the container position may equal a member's position. The header counts
  this, and the analysis then uses the container position as given.
- **Starting state.** Members may still be moving at the leg 1 order, after the 180-frame settle from the spawn. The
  leg line counts them ("members moving at the order"). Leg 1 is reported, but Q2 is judged on the straight leg 2.
