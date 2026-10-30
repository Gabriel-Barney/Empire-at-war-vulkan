# S10 probe: map owner ids -> battle players (landing / capture-point ownership)

Spec: `docs/re/REMAINING_GAPS.md` §3.1 (map pre-pass, branched-map discard) and §3.3 ("the map owner field is a player
id ... how campaign maps' owner ids line up with the campaign's player order was not traced"). Open item
REMAINING_GAPS §7 item 1 "Map owner ids vs player ids in campaign land battles" and the PHASE4 probe list ("attacker landing /
capture-point ownership vs campaign player ids").

## What the port does today (the guess under test)

The raw .ted owner (placement mini 02) is used directly as an index into the port's galaxy player vector:
- `engine/apps/sim/src/tactical_battle_bt.cpp` `bt_owner_valid` (~l.529): valid only if `owner < players().size()`;
  `bt_map_prepass` (~l.539, check ~l.568) drops a placement with an invalid owner ("Player ID that no longer exists");
  `bt_map_load` (~l.586) spawns with `owner = pl.owner` (~l.605); `bt_landing_points` (~l.461) takes the attacker's landing
  zones as the REINFORCEMENT_POINTs whose raw owner == the attacker's player index.
- The player vector differs per mode:
  - campaign: `engine/galactic/src/campaign_setup.cpp` ~l.37-74 = only the factions the campaign names, in
    FactionFiles order, **compacted**, + Neutral (this probe's campaign: Rebel 0, Empire 1, Pirates 2, Neutral 3,
    Underworld 4);
  - skirmish: `engine/apps/game/src/game_data.cpp` ~l.163 `make_skirmish_galaxy` = {human, ai, neutral};
  - attract demo only: `engine/apps/game/src/game_session.cpp` ~l.113 = one player per faction in faction order.

What the shipped maps say (`ted_owners.py scan`): raw owners are always 0..7 and follow the faction order
**Rebel 0, Empire 1, Pirates 2, Neutral 3, Hostile 4, Sarlacc 5, Underworld 6, Hutts 7** (Factions.xml then
Expansion_Factions.xml): Rebel walls/build pads 0, Empire walls 1, pirate pads 2, props and nearly every
reinforcement / capture point 3, natives 4, Sarlacc 5, Underworld props 6, Hutt-world creatures 7. Skirmish (`_MP_`)
maps encode the start slots by marker *name* (`Team_00_*`, `Team_01_*`), not by owner. If the original's player id is
the faction index, the port is wrong in two places: in skirmish every raw-3 (Neutral) object, i.e. every
reinforcement point, and every raw 4/5/7 native is dropped, and raw 0/1 are given to slot 1/2 instead of the Rebel /
Empire player; in campaign, raw 4..7 shift or are dropped as soon as a faction between them is not in the campaign.

## Hypotheses the probe separates

| rule | raw owner r means | test column |
|---|---|---|
| H1 faction | the player of faction_order[r]; its `Get_ID()` == r | `id==raw`, `fac==raw` |
| H2 slot (port, skirmish) | r = slot / port player index | `port ok` |
| H3 compact (port, campaign) | index into the campaign's own compacted faction list | `port ok` |

## Scenarios and maps

| id | mode | map | sides | why |
|---|---|---|---|---|
| GC_TATOOINE | campaign land invasion | `_Land_Planet_Tatooine_02` | Underworld (attacker, story-driven) vs Empire (defender, human) | raw 0 (Rebel walls, spawn houses, build pad), raw 1 (Empire walls, build pad), raw 2 (pirate pads/walls) and 3 Neutral reinforcement points: a third party (Rebel) owns map objects in a battle it is not in, and §3.1's branched-map discard should drop some of them |
| GC_JABIIM | campaign land invasion | `_Land_Planet_Jabiim_02` | Underworld (attacker, story-driven) vs Rebel (defender, human) | raw 6 (Underworld) and raw 7 (Hutts) objects: H1 vs the port's compacted campaign list (Underworld = 4 there, 6/7 dropped) |
| SK_POLUS | skirmish, 2 players | `_MP_Land_Polus` | slot 1 Empire (human), slot 2 Rebel (AI) | raw 0 Rebel_Build_Pad (H1: Rebel AI; port: slot 1 = Empire human), raw 7 Pyngani_Spawn_House, raw 3 points (port: dropped) |
| SK_MANDALORE (optional) | skirmish, 2 players | `_MP_Land_Mandalore` | same | raw 0 build pad, raw 4 Mandalorian spawn houses (Hostile natives) |

Expected map objects (non-Neutral owners plus all points; full list incl. positions in `expected_objects.json`):

| map | raw | objects |
|---|---|---|
| Tatooine_02 | 0 | Skirmish_Build_Pad, Wall x9, Wall_Corner x4, Desert_Civilian_Spawn_House_Rebel x4, Jawa_Sandcrawler |
| | 1 | Skirmish_Build_Pad, Empire_Wall x9, Empire_Wall_Corner x4, Prop_Tatooine_Bush01 |
| | 2 | Pirate_Build_Pad x5, Pirate_Wall x3, Pirate_Wall_Corner x2, 2 pirate structure markers |
| | 3 | Reinforcement_Point_Plus3_Cap x2, Reinforcement_Point_Plus4_Cap; 16 capture points (Garrison_Bunker_Capturable x4, Secondary_Mercenary_Outfitter, Skirmish_Build_Pad x11) |
| Jabiim_02 | 0 | Swamp_Civilian_Spawn_House_Rebel x5 |
| | 2 | Pirate Command Center / Shield Structure Position markers |
| | 3 | Reinforcement_Point_Plus10_Cap; 20 capture points (Garrison_Bunker_Capturable x3, Skirmish_Build_Pad x17) |
| | 6 | Prop_Swamp_Bamboo x3 |
| | 7 | Bugs Swamp x4 |
| MP Polus | 0 | Rebel_Build_Pad |
| | 3 | Reinforcement_Point x2; 10 capture points |
| | 7 | Pyngani_Spawn_House |
| MP Mandalore | 0 | Skirmish_Build_Pad |
| | 3 | Reinforcement_Point x5; 40 capture points |
| | 4 | Mandalorian_Spawn_House x8 |

## Run 1 (2026-10-29, `out\20261029-064937`) and what changed

- Polus skirmish ran 2:11, but **no `_S10MapOwners.txt` anywhere**: retail `_CustomScriptMessage` writes nothing (or the
  hook did not run; run 1 had no way to tell).
- The GC started as Underworld, but the `Starting_Forces` ground units did not appear (unit cap 0/10), and the
  shipped **Gateways** story ran (objectives window, heroes) because the campaign named
  `Story_Plots_Sandbox_27_Underworld.xml` as `Underworld_Story_Name`.

Changes: output through **process memory** (`bin\s10reader.exe`) instead of a log file; message-box popups as a
second channel that the reader records and closes; on-screen proof that the hook and the story script run; the GC
uses its **own story plot** whose Lua spawns both sides and starts the invasion by itself.

## How it works

**Mod** (`build_mod.py` builds `corruption\Mods\S10MapOwners` from the installed game's own files; nothing from the
game is stored in the repository):
- `Data\Scripts\Miscellaneous\GameScoring.lua` = the shipped global scoring script + `s10_hook.lua`. In each **Land**
  mode: `Game_Message("TEXT_S10_HOOK_ALIVE")` (on-screen line "S10 PROBE: GameScoring hook running in this battle")
  and a popup `S10 hook alive battle=n mode=Land map=<map>`; then dumps at the first service, ~10 s and ~40 s
  (`Find_Player` per faction, `Find_All_Objects_Of_Type(player | type)`, owner `Get_ID` / faction / human) to
  `_CustomScriptMessage` (kept in case it works somewhere) and one popup per dump
  `S10 t0 <map> Rebel=<id>:<count> Empire=<id>:<count> ...` = the player id of every faction. Read-only.
- `Data\XML\S10_Probe_Campaign.xml` (+ `CampaignFiles.xml`): two campaign sets, each with two playable factions:
  - **S10 PROBE A: TATOOINE**: play **Empire** (preferred) or Underworld. Tatooine (Empire) - Byss (Underworld).
  - **S10 PROBE B: JABIIM**: play **Rebel** (preferred) or Underworld. Jabiim (Rebel) - Abregado Rae (Underworld).
  Planets: Tatooine, Byss, Abregado Rae, Jabiim; routes Byss-Tatooine, Abregado Rae-Byss, Abregado Rae-Jabiim. No space
  units or stations, so the Underworld fleet arriving at the target goes straight to the land battle. No Gateways story
  and no AI hero plots; the human faction's `<Faction>_Story_Name` is the probe's own plot.
- `Data\XML\S10_Story_Plots_<A|B>.xml` -> `S10_Story_<A|B>.xml` (one event `S10_Begin`: `STORY_ELAPSED` 3 s, reward
  `SCREEN_TEXT` "S10 PROBE: story plot loaded, the invasion starts now") and `Data\Scripts\Story\S10_Story_<A|B>.lua`
  (from `s10_story.lua`; `PGStoryMode` maps the event to the Lua state `S10_Begin`). The Lua spawns 2 defending
  infantry squads on the target for the defender, spawns Underworld_Merc_Squad x2, Underworld_Disruptor_Merc_Squad
  and MZ8_Pulse_Cannon_Tank_Company on the source planet, `Assemble_Fleet`, `BlockOnCommand(fleet.Move_To(target))`,
  `fleet.Invade()` - the sequence the shipped `Story_Campaign_Underworld.lua` uses for its AI invasions. Proof:
  popups `S10 story <target>: ...` and `Game_Message("TEXT_S10_STORY_LUA")`.
  Preferred variant (human = defender): the invading Underworld player is AI-controlled (`AI_Player_Control
  Underworld, None`), exactly the shipped pattern. Fallback (human = Underworld): `Invade()` is issued on the human's
  own fleet; if that does not start the battle, the driver invades by hand.
- `Data\Text\MasterTextFile_English.dat` = shipped text + the probe strings.

**Reader** (`reader\s10reader.cpp`, built by `build_reader.cmd` into `bin\`, project-local MSVC, `/W4 /WX`). Read-only
(`ReadProcessMemory`), same documented layout as `q2_traces` (`reader\s10_layout.hpp`; values from `docs/re`
MAIN_LOOP / OBJECT_RUNTIME / OBJECT_TYPES / PE_OVERVIEW). It waits for FoC's `StarWarsG.exe` (checks the 1.121
timestamp / image size; any other exe is ignored and reported), watches the GameModeManager, and for every **land**
mode dumps the mode's object store at mode frames 1, 300 and 1200 (~0 / 10 / 40 s): `O id= owner= type= pos=
health= flags=`. Every 0.25 s it records the text of the game's message boxes (`POPUP ... text=...`) and closes them
with OK (`--dismiss-popups`). Log: `out\<run>\reader.log`, status `reader_status.json`.

**Parser** (`s10_parse.py parse <log>`, hook or reader log). For a reader log: map per battle = the hook popup's map,
else the scenario map whose placements match best; faction per player id = the hook popup (`Find_Player`), else a vote
over logged objects whose type has a single-faction `<Affiliation>` (independent of the map's owner field). Then the
same table as for hook logs: per raw owner, placed / found / in-game owner / `id==raw` / `fac==raw` / `port ok`.

## Run it (Gabriel or the coordinator via computer use)

Interactive: start Steam, double-click **`run_s10.cmd`**. Background (no key presses anywhere):

    tools\probe_mod\s10_map_owners\run_s10.cmd --non-interactive

It builds the reader if `bin\s10reader.exe` is missing, installs the mod, starts the reader, launches
`StarWarsG.exe MODPATH=Mods\S10MapOwners`, waits for the game to exit, writes the tables and cleans up. Progress:
`out\latest.txt` = the run folder; `<run>\run_status.txt` `state=` installing / waiting_for_game_start /
game_running / collecting / done (`exit_code=`, `cleanup=`); the console ends with `S10_RUN_DONE exit=<n>`
(0 = tables written, 10 = no land-battle dump, 20/21 = preflight, 30/31 = interrupted / error).
`--no-launch` = start the game yourself (the run waits up to `--wait-start-sec`, default 900, for it);
`--no-popup` = no hook popups.

## Click path

Message boxes titled `S10 ...` are closed by the reader within ~0.3 s; one that stays open: click **OK**. Screens may
look slightly different; the decisions are what matters. In every battle **give no orders**.

1. Main menu > **Single Player** > **Galactic Conquest**. In the campaign list select **S10 PROBE A: TATOOINE**.
   Faction: **Empire**. **Start** (accept defaults on any settings screen).
2. Galaxy map: click nothing. After ~3 s an on-screen line "S10 PROBE: story plot loaded, the invasion starts now"
   appears; the Underworld ground units appear at Byss and fly to Tatooine (a few seconds).
3. When the invasion prompt / battle dialog appears: choose to **fight** the battle (not auto-resolve, not retreat).
   The land battle loads (Tatooine). Expect the line "S10 PROBE: GameScoring hook running in this battle" (and
   maybe a brief message box). **Wait 45 s** of battle time.
4. **Esc** > **Quit** (to the main menu; confirm).
   Fallback, only if no battle started within ~60 s of step 2: repeat 1-4 with faction **Underworld**; if the fleet
   then sits in orbit over Tatooine, select it and use **Ground forces in orbit > Invade** (or right-click the
   planet surface), then step 3.
5. **Galactic Conquest** > **S10 PROBE B: JABIIM**, faction **Rebel**, and steps 2-4 (Abregado Rae -> Jabiim).
6. **Skirmish** > **Land**, map **(2) Polus Winds**. Slot 1 (you) **Empire**, Team 1; slot 2 **AI** (Easy)
   **Rebel**, Team 2; other slots closed. **Start**, wait 45 s, **Esc > Quit**.
7. Main menu > **Quit**. The run finishes by itself; send `out\<run>\` (`table.txt` first).

**Cleanup:** automatic at the end of every run (also after Ctrl+C or an error). After a crash, double-click
**`cleanup_s10.cmd`** (safe to repeat): it deletes `Mods\S10MapOwners` only if it carries `S10_PROBE_MARKER.txt` and
moves any `_S10MapOwners*.txt` into `out\`. No retail file is modified; if the game's own `_warn_message_popup.txt`
changed during the run, a **copy** goes into the run folder.

## Reading the table

`table.txt` (from `reader.log`): per land battle first the reader header (map and how it was identified, player id ->
faction with the evidence, affiliation votes, the popups), then the per-raw-owner table: placements, found in game
(type + nearest x/y within 25 units), their in-game owner, `id==raw` / `fac==raw` / `port ok` counts with the port's
guess (`drop` = the port deletes the placement), every point placement with its owner (raw 3 summarised), the logged
objects without a placement (forces, squad members) with centroid and nearest `Reinforcement_Point*` (where the
attacker landed), and point owner changes at the later dumps. `table_hook.txt` exists only if a hook log appeared.

The result settles: (1) whether the map owner field is the faction-indexed player id in campaign **and** skirmish
(then the port needs one player per faction at its faction index in every battle, not the compacted campaign list or
{human, ai, neutral}); (2) which side owns the reinforcement / capture points at battle start in a campaign invasion
(all raw 3 on these maps, so `bt_landing_points` finds no attacker landing zone today) and where the attacker's units
appear; (3) which third-party map objects survive §3.1's discard.

## Files

| file | what |
|---|---|
| `run_s10.cmd` -> `run_s10.py` | build reader if needed, install, start reader, launch, wait, tables, cleanup (`--non-interactive`, `--no-launch`, `--no-popup`) |
| `cleanup_s10.cmd` -> `cleanup_s10.py` | idempotent cleanup (`--dry-run`) |
| `build_mod.py` | builds the mod from the install; `--expected-only` rewrites `expected_objects.json`; `--out DIR`, `--no-popup` |
| `s10_hook.lua` | the Lua appended to GameScoring.lua |
| `s10_story.lua` | the campaigns' story script template (`@@TARGET@@` etc. filled by build_mod.py) |
| `reader\s10reader.cpp`, `reader\s10_layout.hpp` | the memory / popup reader |
| `reader\s10_fake_game.cpp` | test target: lays out the documented structures with a map's objects, opens a message box |
| `build_reader.cmd` | builds `bin\s10reader.exe` + `bin\s10_fake_game.exe` (log `logs\s10reader_build.txt`, `BUILD_OK`) |
| `ted_owners.py` | .ted placement reader over the install's MEGs: `scan`, `list <map>`, `expected` |
| `maps.json`, `expected_objects.json` | scenarios / the port's player tables; the scenario maps' placements |
| `s10_parse.py` | `parse <log> [--no-popups]`, `fake`, `selftest` |
| `selftest_hook.py`, `selftest_story.py` | the hook / the story scripts under stock Lua 5.0.2 (`--lua`); the story test runs the game's own PG library from `extracted\foc_data_text.zip` |
| `selftest_reader.py` | fake game -> reader -> parser, end to end |

All Python uses the project venv: `toolchain\venv\Scripts\python.exe -I <script>`.

## Offline validation (2026-10-29, after run 1)

- `build_reader.cmd`: both exes build cleanly at `/W4 /WX /permissive-`.
- `selftest_reader.py`: OK for Polus with the hook popup, Polus with no popup (map found by placement match 100 %,
  player ids 0/1/3 from affiliation votes) and Tatooine as a campaign battle with Underworld id 6: 2 dumps per battle,
  the message box recorded and closed, clean `EXIT reason=game_exited`, `id==raw` / `fac==raw` 100 % for the rule
  the fake uses.
- `selftest_story.py`: both story scripts run inside the game's own `PGStoryMode` / `PGStateMachine` / `PGBase` /
  `PGSpawnUnits` under Lua 5.0.2: event `S10_Begin` -> state `S10_Begin` -> one thread; defenders spawned on the
  target for the defender, 4 Underworld units on the source, fleet of 4, `Move_To(target)`, then `Invade()`; popups
  and `Game_Message` present.
- `selftest_hook.py` (Polus, Tatooine, Jabiim `--dist`): 3 dumps, "hook alive" popup + `Game_Message`, tables OK.
- Built mod: GameScoring.lua and both story scripts pass `luac -p`; all XML parses; the text file holds the 7 keys.
- `run_s10.cmd --non-interactive --no-launch` on a scratch game folder: installs, the reader waits and reports a
  non-FoC `StarWarsG.exe` as ignored, the run sees that process start and exit, writes `run_status.txt state=done`,
  `S10_RUN_DONE`, and cleanup deletes only the marked mod folder (another mod folder and `_warn_message_popup.txt`
  untouched). The timeout path (no game within `--wait-start-sec`) also ends in cleanup.
- `s10_parse.py selftest`, `cleanup_s10.py --dry-run`: OK.
- Not validated (needs the retail game): the reader against the real exe (layout validated by the Q2 galactic trace,
  but land-mode object stores have not been read before), whether retail `_MessagePopup` opens a window, whether the
  story script's `Invade()` starts the battle for the human defender (shipped pattern) and for a human attacker.
