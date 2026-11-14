# s78m t06: Tyber Zann bribes a Stormtrooper_Team (retail probe)

Roadmap group s78m, task t06 (with the t05 corners it can answer). Spec: `docs/re/TACTICAL_COMBAT.md` §5.4.1
(TacticalBribeAbilityClass accept 0x5b0f70, cost 0x5b0ec0, apply 0x5b07c0). The probe measures in retail what the port
assumed:

| # | Question | Port value (where) |
|---|---|---|
| Q1 | credits paid for one bribe of a `Stormtrooper_Team` | `Tactical_Bribe_Cost` 25 × `Bribe_Modifier_On_Uncorrupted_Planets` 1.0 = **25**; skirmish = uncorrupted (`ability_classes.cpp:288`, `battle_catalog.cpp:718-721`, `tactical_battle_oi_abilities.cpp:204-213`) |
| Q2 | whole team or only the clicked trooper | container + every live member; the second team of the company stays (`ability_classes.cpp:1305-1316`, `ability_behaviors.cpp:53`, `tactical_battle_oi_abilities.cpp:494`, `tactical_battle.cpp:4821`) |
| Q3 | timing | payment and every owner change on **one frame**, at the pending order's next targeting service, no animation wait (`ability_classes.cpp:644-673`, `ability_system.cpp:1237`) |
| Q4 | recharge | `Recharge_Seconds` 50 = **1500 frames** from the payment (`ability_system.cpp:351`) |
| Q5 | a team container as the target | **refused** (`bribe_accepts` takes combat units only, `ability_classes.cpp:306`) [port] |
| Q6 | range | `Activation_Max_Range` 100, XY centre distance to the target; `Must_Close_Range` not modelled (`ability_system.cpp:805`) |
| Q7 | too few credits | no payment, no owner change, **no recharge**, the order is **used up** (no bribe when the money comes back) (`ability_classes.cpp:650-655`) [M in §5.4.1] |
| Q8 | (optional) the same by hand through the UI | as Q1–Q4 |

`Get_Bribe_Cost(tyber)` of `Stormtrooper_Team`, `Squad_Stormtrooper` and `Stormtrooper` (the shipped Tyber AI's own
cost call, `AI_PLAN_UNDERWORLD_TYBERMANAGERLAND.LUA`) is also recorded: 18.75 instead of 25 would mean retail treats the
skirmish planet as corrupted (`Bribe_Modifier_On_Corrupted_Planets` 0.75).

## How it works

The Empire AI's free store (`BusyTacticalFreeStore.lua`, shipped + `s78m_bribe.lua`) runs the probe; it arms only in
a land battle where Underworld is human (never in the main-menu background battle). At 15 s it spawns an invulnerable
`Tyber_Zann` for the human player at a dry, open point of Dagobah far from both bases, turns his bribe autofire off
(as the shipped Tyber AI does), then runs three trials. Each trial spawns an `Imperial_Stormtrooper_Squad` (18
`Squad_Stormtrooper` in two `Stormtrooper_Team` of 9) for the AI: team A is the target, team B the control
(invulnerable, `Prevent_All_Fire`). The order is `tyber.Activate_Ability("TACTICAL_BRIBE", <object>)` (ability 59 takes
an object: command 40, `Issue_Auto_Target`), exactly as the shipped AI does it.

| Trial | Setup | Then |
|---|---|---|
| 1 `near_member` | squad 60 from Tyber, target = a member | watch 15 s, read `Get_Bribe_Cost`, wait for the recharge (≤ 60 s) |
| 2 `far_container` | squad 250 from Tyber, target = team A's **container** | no block returned: retry on a member after 2 s; block but no owner change: retry after 20 s; watch 40 s, wait for the recharge |
| 3 `refused_poor` | squad 60 from Tyber, credits set to 5 (`Give_Money`) | order; 10 s later the money is given back; watch 10 s more, wait for the recharge if one started |
| 4 `manual` (optional) | squad 80 from Tyber, camera on Tyber | the driver may bribe by hand; 90 s |

Each trial's troopers are despawned before the next one. Every free-store service (every 3rd frame) the probe polls
the human's credits, Tyber's `Is_Ability_Ready` / `Is_Ability_Active`, Tyber's position, and the owner of every
member and container, and records each change as it happens (`CR`, `RDY`, `ACTV`, `OWN`, `CONV`), plus `P` snapshots
(positions, distances, per-team owner counts). Records are Lua strings `S8M|...|END` read by a read-only heap scan
(`../a1_tactical/a1mem.py`). All frames are mode frames, good to one poll (±3 frames).

Teams are found after a 1.5 s settle: new AI `Squad_Stormtrooper` within 200 of the spawn point (fallback: any new AI
object there with a parent), grouped by `Get_Parent_Object`. Object ids come from `Get_Object_ID` (`Get_ID` is a
player method); without an id the probe uses labels (`M<k>_<n>` members, `T<k>_<n>` teams) and matches a parent
wrapper by equality, then by position. The `IDENT` record lists what was found and any returned container that did
not match. Each trial is independent: a trial that finds no team ends (`ERR what=no_team`, `TEND`), a trial whose
Lua fails 3 times or runs past its time limit ends with `TEND aborted=errors|timeout`, and the next trial starts. A
tick error is recorded once per message; after 100 the probe finishes.

## Run (the driving session)

`run_s78m.cmd --no-launch --non-interactive` (detached). It refuses to start while `StarWarsG.exe` runs (exit 20),
snapshots the profile (registry `HKCU\Software\Petroglyph\StarWars FOC` + Saved Games, `../q2_traces/q2_profile.py`),
installs `Mods\S78MBribeProbe`, prints the command line, waits for the game, scans until the game exits, writes the
report, removes the mod and restores the profile. Launch, from `...\Star Wars Empire at War\corruption`:

```
StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\S78MBribeProbe
```

In the game:
1. **Single Player > Skirmish > LAND**, map **DAGOBAH**. Slot 1 (you): **UNDERWORLD** (Zann Consortium), team 1.
   Slot 2: **AI** (any difficulty) **EMPIRE**, team 2. Every other slot **closed**. Default credits and settings. Start.
2. Give **no orders**, click nothing on the map (moving the camera is fine; never pause, never change the speed).
   "S78M PROBE: bribe probe armed ..." appears in the first seconds.
3. Wait (about 3.5 minutes). The probe does trials 1–3 by itself.
4. "S78M PROBE: scripted bribes recorded. Optional ..." The camera jumps to Tyber; a new stormtrooper squad stands
   next to him. Optional: left-click Tyber, click his **Bribe** ability button (bottom bar), left-click a stormtrooper.
   Otherwise do nothing.
5. "S78M PROBE: bribe run recorded, quit now" (90 s after step 4) → **Esc > Quit** to the main menu, then quit the
   game. Never save. Screenshots of the bribe effect are welcome but not needed.

Expected time: about 5 minutes in the battle (7 with the optional window), plus menus.

## Results

`out\<yyyymmdd-hhmmss>\report.txt` (`out\latest.txt` names it), also `report.json` and the raw `records.log`.
Rebuild a report: `s78m_run.py --report-only out\<run>\records.log`. The report lists each trial (teams, orders and
their return, owner changes with frames relative to the order, payment, credit changes from income, ready edges,
Tyber's distances to the target / container / nearest member at the conversion) and ends with one line per question:
`-> MATCH` or `-> DIFFERS`, with the port file:line to change. Three corrections are built in (2026-11-08, after run
`20261108-215525`):
- Q1 price = net credit drop on the payment poll + the income on that poll. Skirmish income is +1 per poll (every
  3 frames), so a 25-credit bribe shows as −24; the income is taken from the other polls within ±15 frames and only
  added when they all agree.
- Q6 gives the interval the retail threshold on the centre distance must lie in (distance at the conversion poll,
  distance at the poll before) and the first poll at which the target / container / nearest member came within 100.
- Q7 adds the affordable window: polls after the refused order, before the restore, with credits ≥ 25, Tyber ready
  and team A alive. No bribe there means the refused order is used up. Team A may die before the restore (it is not
  invulnerable; in run `20261108-215525` all of it died on one poll 90 frames after the order), so that window, not "after the restore", is the evidence.

Run `20261108-215525` results: `docs/re/S78M_T06_BRIBE_RUN.md`.

## If something is missing

- `script sid ... skipped`: Underworld was not human, the map was not a land map, or the AI was not Empire.
- `DONE=NO` with step records: a retail engine call stopped the script silently; the last `ST` record names it.
- `Q7 ... NOTE: Give_Money did not bring the credits under 25`: negative `Give_Money` refused; Q7 is void.
- No records at all: the mod did not load (check the MODPATH) or the battle never started.
- Run `out\20261108-214221` (first retail run) failed: the probe read object ids with `Get_ID` (a PlayerWrapper
  method, so every id was `?`), found no team in trial 1, then hit a nil `fact` every tick (2056 `ERR`) and never
  bribed. No owner changed; the Stormtrooper losses on the results screen are Tyber shooting the unprotected troopers.
  Fixed (ids, team matching, per-trial error handling); the driving steps are unchanged.

## Files

| File | What it does |
|---|---|
| `run_s78m.cmd` / `s78m_run.py` | One session (exit 0 report, 10 no records, 20 preflight, 30 interrupted, 31 error). `--no-launch`, `--non-interactive`, `--windowed`, `--no-manual`, `--no-profile`, `--dry-run`, `--report-only` |
| `build_s78m_mod.py` | Builds the mod from the install (or `--source-zip`, `--no-text`); `CFG` holds every trial parameter |
| `s78m_bribe.lua` | The probe (Lua 5.0, appended to the shipped free store) |
| `s78m_parse.py` | records → `report.txt` / `report.json` |
| `s78m_cleanup.py` / `cleanup_s78m.cmd` | Deletes `Mods\S78MBribeProbe` (only with `S78M_PROBE_MARKER.txt`) and restores the profile snapshot |
| `selftest_s78m.py` | Offline check (below) |

## Offline check (no game)

```
toolchain\venv\Scripts\python.exe -I tools\probe_mod\s78m_bribe\selftest_s78m.py
```

Builds the mod (install read only, else the text zip), compiles it with stock Lua 5.0.2, runs the probe under a stubbed
engine five times (port model: every verdict MATCH; a deliberately different engine: cost / timing / recharge /
container / refused DIFFERS; Underworld not human: SKIP only; no `Get_Object_ID` and a fresh parent wrapper per call:
every verdict MATCH; a broken engine where trials 1-2 find no team and trials 3-4 raise a Lua error every tick: all
four trials end, DONE is reached, at most 12 `ERR`) and passes each run through the parser. `Spawn_Unit` returns the
two team containers, as in retail.

## Known limits

- Frames come from the free-store service: ±3 frames. A per-frame owner trace would need a memory reader.
- Whether a refused bribe "uses up the order" is read from the outcome (no bribe once the money is back); the
  pending-ability field itself is not read.
- The port's skirmish battles have no credit account (`set_credit_hooks` is only called by `campaign_battles.cpp:787`),
  so a port skirmish bribe is always refused; compare Q1/Q3/Q4 against the port's ability code, not a port skirmish run.
