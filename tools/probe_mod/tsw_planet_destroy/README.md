# TSW planet-destroy probe (L1 systems t04)

Spec: `docs/re/TSW_PLANET_DESTROY.md`. Three open **probe** items:

| # | Question | Spec section | Port today |
|---|---|---|---|
| P1 | Does a **galactic** story plot waiting for `STORY_PLANET_DESTROYED` ever fire after a tactical TSW kill? | §7 | no (expected: no) |
| P2 | Exact frame offsets after a successful Fire: planet destroyed at F+348, hold released at F+732 (vanilla Death Star II), ±1? | §3 | F+348 / F+732, with Fire inside the Lua service between ticks |
| P3 | Owner and starbase of the planet after a **won** and after a **lost** TSW battle | §8 | the TSW does not touch owner / starbase / fleets; the normal battle write-back and the derived owner decide |

## What the mod does

`build_mod.py` builds `corruption\Mods\TswPlanetProbe` from the installed game's own files (nothing from the game is
stored in the repository; it reuses the `s10_map_owners` install reader):

- **Campaign "TSWP PROBE: KUAT"** (play the **Empire**): Byss (Empire: level-1 station, `Death_Star_II`, 3
  `Star_Destroyer`) - route `Byss_Kuat` - Kuat (Rebel: `Rebel_Star_Base_1`, 2 `Corellian_Corvette`,
  `R_Ground_Barracks`, `Rebel_Infantry_Squad`). Rebel AI off (`None`).
- **Galactic plot** (`TSWP_Story.xml` + `TSWP_Story.lua`): `TSWP_Begin` (3 s, on-screen "TSWP PROBE: galactic plot
  loaded") starts a status thread that shows a message box `TSWP galaxy t=... destroyed=... owner=... starbase=...
  death_star=... ds_at=... Rebel_objects_here=... Empire_objects_here=...` every time the status of Kuat changes.
  `TSWP_Heard` (`STORY_PLANET_DESTROYED`, Param1 Kuat) shows the on-screen line **"TSWP PROBE: THE GALACTIC PLOT
  HEARD PLANET_DESTROYED (spec: never)"** and a message box if it ever fires (P1).
- **GameScoring hook** (`tswp_hook.lua`, appended to the shipped `GameScoring.lua`), in every space battle:
  fires the Death Star by itself (`Fire_Tactical_Superweapon`) at the first service where
  `Is_Tactical_Superweapon_Ready()` is true and stamps the Lua frame F = round(`GetCurrentTime()` x 30) (mode time,
  frame exact); then stamps the first service where the target dummy `Kuat_Backdrop_Large` is gone (the state-9
  kill) and where `FindPlanet("Kuat").Is_Planet_Destroyed()` turns true (D +0x2c8), and the frame of
  `Game_Mode_Ending_Event` (battle end). Message boxes: `TSWP fired at frame F=...`, `TSWP dummy ... gone at frame
  ... = F+...`, `TSWP Is_Planet_Destroyed true at frame ... = F+...`, `TSWP battle 1 END frame=... = F+...; ...
  services=... min step=...`. `ServiceRate` is 0 (every service the game gives the script); `min step` says how fine
  the stamps are (1 = frame exact). Re-run with `--rate 0.0333` if 0 is refused (min step stays large).

## Run it (Gabriel)

Start Steam, double-click **`run_tswp.cmd`**. It installs the mod, launches `StarWarsG.exe MODPATH=Mods\TswPlanetProbe`,
waits for the game to exit and cleans up. `run_tswp.cmd --no-launch` installs only (start the game yourself, quit it,
press Enter). Do the two runs in one session or in two sessions (the cleanup runs at the end of each).

### Click path

**Screenshot every message box whose text starts with `TSWP` before clicking OK**, and the galaxy map (Kuat's
owner colour, station icon, fleets) after each battle.

1. Main menu > **Single Player** > **Galactic Conquest** > **TSWP PROBE: KUAT** > **Empire** > Start.
2. Wait for "TSWP PROBE: galactic plot loaded" and the first `TSWP galaxy ...` box (the start status).
3. Select the Byss fleet (Death Star + 3 Star Destroyers), move it to **Kuat**. Battle prompt: **fight** (not
   auto-resolve).
4. In the space battle expect "TSWP PROBE: GameScoring hook running ..." and `TSWP hook alive`. Give no orders. The
   Death Star moves in; the hook fires it as soon as it is ready (`TSWP fired ...`, on-screen "the hook fired the
   Death Star"). About 11.6 s later the planet blows up (`dummy ... gone`, `Is_Planet_Destroyed true`).
   - **Run W (won):** let the battle run; if the station still lives 30 s after the explosion, order the Star
     Destroyers to attack it. Note the moment the battle ends (`TSWP battle 1 END ...`).
   - **Run L (lost):** right after the `TSWP fired` box, press **Retreat** (the whole fleet leaves). Note whether the
     retreat completes before the explosion and when the battle ends.
5. Back on the galaxy map: screenshot the map and every `TSWP galaxy ...` box; watch for the on-screen "GALACTIC PLOT
   HEARD" line for 30 s.
6. Esc > Quit to the main menu; for the second run start again at step 1. Then quit the game.

## Expected results (spec) and how to read them

| Item | Expected | Deviation means |
|---|---|---|
| P1 | no "GALACTIC PLOT HEARD" line or box in W or L | the galactic manager does hear tactical kills: the port must route PLANET_DESTROYED to the galactic plots too (`StorySession::planet_destroyed_in_battle`, the value-1 stream filter) |
| P2 dummy gone | F+348 (vanilla 270 + 1 + 1 + 75 + 1), `Is_Planet_Destroyed` the same frame | F+347 / F+349: the Lua Fire is serviced in frame F (or the step is not 1); the port's `special_weapons.cpp` state 6/7 services need the shift |
| P2 battle end (W) | not before F+732 (hold release); with a dead station the end is at the release frame or the next | earlier end: the hold (`TacticalBattle::check_victory`) is wrong; a later end with everything dead: a post-release countdown we do not model |
| P2 retreat (L) | the retreat may finish early, but the battle ends only at ~F+732 | an end at the retreat: no hold for retreats |
| P3 after W | `destroyed=true`; starbase = what the battle left (the station was killed: none), owner by the derived-owner rules (no surface units; Empire fleet in orbit) [M]; the Death Star still at Kuat | starbase / owner forced (e.g. Neutral): the TSW path does the hack's extras |
| P3 after L | `destroyed=true`, owner Rebel, `Rebel_Star_Base_1` kept, Death Star fleet back at Byss (retreated) | Rebel loses the station / owner: same |
| `Rebel_objects_here` | the infantry squad and barracks are gone after the explosion (surface list killed) | the surface survives: the core routine's list naming (§5 [M]) is wrong |

**Send back:** the screenshots and `out\<run>\run.txt`.

## Cleanup

Automatic at the end of every run (also after an error or Ctrl+C). After a crash, double-click **`cleanup_tswp.cmd`**
(safe to repeat): it deletes `corruption\Mods\TswPlanetProbe` only if it carries `TSWP_PROBE_MARKER.txt`. No retail
file is modified.

## Files

| file | what |
|---|---|
| `run_tswp.cmd` -> `run_tswp.py` | build + install, launch, wait, cleanup (`--no-launch`, `--non-interactive`, `--rate S`, `--game DIR`) |
| `cleanup_tswp.cmd` -> `cleanup_tswp.py` | idempotent cleanup (`--dry-run`) |
| `build_mod.py` | builds the mod from the install; `--check` = offline self-check (XML parses, placeholders filled) |
| `tswp_hook.lua` | the Lua appended to GameScoring.lua (fires the TSW, stamps frames) |
| `tswp_story.lua` | the galactic story script (status popups) |

Offline validation (2026-10-31): `build_mod.py --check` OK. Not validated (needs the retail game): the Lua under the
real engine (`Fire_Tactical_Superweapon` / `Is_Tactical_Superweapon_Ready` on a `Find_All_Objects_Of_Type` result,
`FindPlanet(...).Is_Planet_Destroyed()` inside a space battle, `Find_First_Object` of the backdrop prop), whether
`ServiceRate = 0` gives one service per frame, and whether `Death_Star_II` is accepted in `Starting_Forces`.

## Run log 2026-10-31 (Claude via computer use; W done, L incomplete)

Kit fixes and findings from the first real run. Launching `StarWarsG.exe` from a Claude shell exits at once with code 0
(also without the mod); starting it through `explorer.exe <launcher.cmd>` works.

- **Target moved from Alderaan to Kuat.** In FoC `PLANETS.XML` Alderaan is the asteroid field
  (`W_planet_Asteroids.alo`) with `Planet_Is_Destroyable_By_TSW No` (also The_Maw, VergessoAsteroids). The kit now
  uses Kuat, route `Byss_Kuat`, dummy `Kuat_Backdrop_Large`.
- **`_MessagePopup` shows nothing in the retail game**, so none of the `TSWP ...` boxes (frame stamps, galaxy
  status) appeared. `Game_Message` works: "GameScoring hook running in this space battle" showed in run L.
- **The hook never fired the Death Star.** Ready (green button, timer at 0) for 20+ s in both runs with no Fire, so
  `TSWP_FindDS` / `Fire_Tactical_Superweapon` fail silently (every call is inside `pcall`). The Death Star was fired by
  hand in both runs, so there are no frame stamps: **P2 is still open.**
- **Galactic plot:** the `TSWP_Begin` screen text ("galactic plot loaded") never showed and no "GALACTIC PLOT HEARD"
  line came either. Since the proof line is missing too, **P1 is inconclusive** (plot wiring not proven).
- **Run W (won):** Kuat destroyed by the manual Fire about 12 s later, station killed by the Star Destroyers, battle
  time 6:21; enemy losses 2 Corellian Corvette + Space Station Level 1. The galactic conquest then ended at once
  (credits, "Victory": "removed the Rebellion from the galaxy"): after a won TSW battle the Rebels no longer hold Kuat
  (their barracks and infantry did not keep it). **P3 after W is only half answered** (owner left the Rebel; Empire or
  neutral not seen), and the campaign needs a second Rebel planet so the galaxy map survives.
- **Run L:** fired by hand, then pressed Retreat twice (button "Retreat from battle", bottom of the column next to the
  minimap) right after the Fire; the planet blew up without the cinematic and the fleet did not leave in 30 s. The run
  was stopped there. **P3 after L is open.**

Open kit work before the next run: an output channel that works in retail (`Game_Message` with fixed text IDs, or a
process-memory reader like `s10_map_owners` / `a1_tactical`), find out why the hook does not fire, a second Rebel planet,
and the retreat rule (when the button is allowed).
