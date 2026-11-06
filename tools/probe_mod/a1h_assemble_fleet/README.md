# a1h t09 probe: does `Assemble_Fleet` launch ground units from a planet's surface?

Roadmap group A1h, task t09 (follow-up of a1g t06). The port's rule, marked [inferred]: `Assemble_Fleet(units)` of
ground units that sit on a planet's surface launches them into the new fleet in orbit (`GalacticSim::assemble_fleet`,
`engine/lua/src/bind_players.cpp` `g_assemble_fleet`). The S10 run of 2026-10-31 showed indirectly that the original
flies freshly spawned Underworld squads with `Assemble_Fleet` + `Move_To`. This probe checks the rule directly, including
for units that were never spawned by Lua.

Nothing from the game is stored in the repository: the mod is built from the install at run time.

## What Gabriel does

1. Start Steam. Double-click **`tools\probe_mod\a1h_assemble_fleet\run_a1h.cmd`**. It builds and installs
   `corruption\Mods\A1hAssembleProbe` and launches `StarWarsG.exe MODPATH=Mods\A1hAssembleProbe`. Then it prints the
   click path below and scans the game's memory for the probe records until the game exits.
2. Main menu > Single Player > **Galactic Conquest**. Pick **"A1H PROBE: ASSEMBLE FLEET"**, faction **EMPIRE**,
   Start (accept defaults).
3. On the galaxy map **click nothing**. About 3 s in, "A1H PROBE: story plot loaded ..." appears. Then one line per
   case shows, about 15 s apart:
   - "A1H PROBE case C (...): every unit is in the new fleet" (or "... stayed out of the fleet" / "gave no fleet");
   - the same for cases A, B and D.

   About a minute in, "A1H PROBE: done" appears.
4. **Take a screenshot** of the galaxy map: the fleet icons over Tatooine, Abregado Rae and Byss, and the planets'
   ground-unit counts. Then Esc > Quit to the main menu > Quit. Do not save the campaign. The run finishes by itself
   (`A1H_RUN_DONE exit=<n>`).
5. Send back `tools\probe_mod\a1h_assemble_fleet\out\<yyyymmdd-hhmmss>\` (`report.txt` first, then `records.log`)
   and the screenshot. `out\latest.txt` names the newest run folder.

**Cleanup** runs by itself at the end of every run, also after Ctrl+C or an error. After a crash, double-click
`cleanup_a1h.cmd`; it is safe to repeat. It deletes `corruption\Mods\A1hAssembleProbe`, and only if that folder
carries `A1H_PROBE_MARKER.txt`. No retail file is changed and other mods are left alone.

**Driven run** (Claude via computer use, see the memory note on driving the game):
`run_a1h.cmd --non-interactive --no-launch` in a terminal tab. Then launch the game windowed yourself
(`StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\A1hAssembleProbe` from the `corruption`
folder, through a one-line `.cmd` started with `Start-Process explorer.exe`). Progress is in
`out\<run>\run_status.txt`.

Exit codes:

| code | meaning |
|---|---|
| 0 | every case has a verdict |
| 10 | no records, or a case without a verdict |
| 20 | preflight failed: game missing or already running |
| 30 | interrupted |
| 31 | error |

## The cases

The campaign has Tatooine and Abregado Rae (Empire, the human), Byss (Underworld, `AI_Player_Control None`) and Jabiim
(Rebel, None). There are no space units or stations, and no AI moves anything. The story plot's Lua
(`a1h_story.lua`) runs the cases one after another:

| case | units | owner | what it isolates |
|---|---|---|---|
| C | the two `Imperial_Stormtrooper_Squad` from `Starting_Forces` on Tatooine's surface (`Find_All_Objects_Of_Type`) | Empire (human) | units never touched by Lua |
| A | `Imperial_Heavy_Scout_Squad` + `Imperial_Stormtrooper_Squad` spawned on Abregado Rae (`SpawnList`, as the shipped story scripts do) | Empire (human) | freshly spawned surface units (a1g t06's case) |
| B | 2 `Underworld_Merc_Squad` spawned on Byss | Underworld (AI None) | the shipped AI-story pattern (S10 probe) |
| D | `Tartan_Patrol_Cruiser` + `Imperial_Stormtrooper_Squad` spawned at Tatooine, after C | Empire (human) | a mixed list: a ship (orbit) and infantry (surface); C's fleet is already in orbit there |

Each case:
1. Waits 3 s.
2. Records every unit's parent object (planet or fleet), its parent's content count and its planet (`BEFORE`).
3. Calls `Assemble_Fleet(units)` and records the fleet (type, count, parent, planet) (`ASSEMBLE`).
4. Records every unit again (`AFTER`).
5. Waits 10 s and records again (`LATER`, `FLEET_LATER`).
6. Gives the verdict (`VERDICT`, shown on screen):
   - `LAUNCHED`: every unit's parent is the fleet, right after the call and 10 s later;
   - `PARTIAL`: only some units are in the fleet;
   - `NOT_LAUNCHED`: no unit is in the fleet;
   - `NOFLEET`: no fleet came back;
   - `NOUNITS`: there were no units (case C when `Starting_Forces` did not spawn them).

**Output channel.** Retail `_CustomScriptMessage` / `_MessagePopup` show nothing (S10, Q2 and A1 runs). So the Lua
keeps every record as a string `A1HR|<kind>|n=<k>|...|END` in its global table `A1H_REC`. `a1h_run.py` finds those
strings with a read-only scan of the game's private memory every 5 s (`a1_tactical/a1mem.py`; it never writes the
game's memory). The `Game_Message` lines on screen show the same verdicts.

## Reading the result

| result | meaning | port |
|---|---|---|
| C, A, B `LAUNCHED` | the original launches surface units into the new fleet | rule confirmed; drop [inferred] (a1g t06) |
| C, A, B `NOFLEET` / `NOT_LAUNCHED` | `Assemble_Fleet` takes only units already in orbit | `GalacticSim::assemble_fleet` must leave surface units out |
| A/B `LAUNCHED`, C not | only units spawned by Lua behave that way | look at `BEFORE`: where the spawn put them |
| D `PARTIAL` | the ship joins, the infantry does not | as the previous row, for mixed lists |
| D `LAUNCHED` and the fleet count includes C's squads | `Assemble_Fleet` merged into the orbit fleet already there | port: check the merge rule |

`BEFORE parent=` shows what holds a unit on the surface (planet name expected). `ASSEMBLE ... planet=` and
`FLEET_LATER valid=` show whether the fleet stays at the planet.

## Files

| file | what |
|---|---|
| `run_a1h.cmd` → `a1h_run.py` | build + install, launch (`--no-launch`, `--windowed`), scan, report, cleanup; `--report-only <records.log>` re-reads an earlier run |
| `cleanup_a1h.cmd` → `a1h_cleanup.py` | idempotent cleanup (`--dry-run`, `--no-pause`) |
| `build_a1h_mod.py` | builds `Mods\A1hAssembleProbe` (`--game`, `--out`), reusing `s10_map_owners/ted_owners.py` (install reader) and `build_probe.py` (`.dat` writer) |
| `a1h_story.lua` | the story script (cases, records, verdicts) |
| `selftest_a1h.py` | offline test: the story under stock Lua 5.0.2 with the game's PG library and two engine models, then the report |

All Python runs with the project venv: `toolchain\venv\Scripts\python.exe -I <script>`.

## Offline validation (2026-11-02)

- `selftest_a1h.py --lua tools\probe_mod\a1_tactical\out\tools\lua50.exe` (built by
  `a1_tactical\selftest\build_lua50.cmd`) printed **A1H SELFTEST OK**:
  - "launch" model (the port's rule): LAUNCHED for C, A, B and D;
  - "keep" model (only orbit units join): NOFLEET for C, A and B, PARTIAL for D;
  - the state machine reaches `A1H_Begin`, there are 6 spawns, B spawns for Underworld at Byss, and every verdict
    text exists.
- `build_a1h_mod.py --out <scratch>` against the install builds the mod.

## In-game result (2026-11-05, a1k t02, run 20261105-181610, driven by Claude via computer use)

Kit exit 0, cleanup OK. All four cases are **LAUNCHED**: every unit's parent is the new `GALACTIC_FLEET` right after
`Assemble_Fleet` and 10 s later, and the fleet stays at its planet.
- C: the 2 starting squads on Tatooine (`BEFORE parent=TATOOINE`) form a fleet of 2.
- A: the 2 squads spawned on Abregado Rae form a fleet of 2.
- B: the 2 Underworld mercs on Byss (AI None) form a fleet of 2.
- D: the Tartan was spawned into its own fleet (`BEFORE parent=GALACTIC_FLEET parent_count=1`) and the squad was on the
  surface. The result is a new fleet of exactly those 2 units, **not** merged into C's fleet orbiting Tatooine.

The port's rule matches every case (a fleet holding exactly the units is reused, otherwise a new one is made), so the
surface-launch rule is no longer [inferred] (`campaign.hpp` `assemble_fleet`). Screenshot: fleet icons over Tatooine
(a ship) and Abregado Rae (infantry). Byss is enemy-owned, so its fleet is hidden on the map.
