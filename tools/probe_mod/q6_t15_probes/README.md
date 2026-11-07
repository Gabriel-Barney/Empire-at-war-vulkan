# Q6 t15 probes: retail checks of the Q6 [port] rules

Roadmap Q6, task t15. Six questions that the Thrawn's Revenge (TR) work answered with a `[port]` rule or an
`[M]` reading. Each needs the retail game. The kit has two sessions: **vanilla** (FoC, `Mods\Q6T15Probe`) and **TR**
(`Mods\Q6T15ProbeTR` loaded in front of TR). Nothing from the game or from TR is stored in the repository. The mods
are built from your install at run time.

| # | Question | Spec | Port today | Session |
|---|---|---|---|---|
| 1 | **EliminationLatch**: does the original end a galactic campaign for a player who owns nothing? | `engine/galactic/include/alamo/galactic/victory.hpp`, STRATEGIC_GAPS §1.4, BATTLE_TRANSITIONS §3.1a | A player is eliminated only after a check has seen it own something (planet or live unit). A player that never owned anything goes on; one that owned something and then nothing loses at the next check | vanilla (3 campaigns) + TR GCMenu by eye |
| 2 | **Object-script first pump**: is the first-pump draw bound `ServiceRate`? Do vanilla objects with `Lua_Script` run their scripts at all? | OBJECT_SCRIPTS §3.2 step 7, §8 item 1, §9 | First pump at the first service after the attach, then `LastService = -(synced range_float(0, ServiceRate))`. Object scripts are on only when a mod is loaded (`[port interim rule]`) | vanilla (land + space skirmish) |
| 3 | **Perpetual STORY_ELAPSED period** | STORY §4.7, §8 item 11 | T seconds + one story service. With T = 2 it fires at frames 61 and 122 (period 61) | vanilla (campaign) |
| 4 | **Load-time story resume**: are a plot's threads resumed once at load, before the frame-1 STORY_ELAPSED dispatch? | STORY §8 item 9 | Yes (`StorySession::start_threads_at_load`): `main` runs before the frame-1 events, so a `STORY_ELAPSED 0` start event enters its state | vanilla (same campaign as 3) |
| 5 | **TR land end time**: how long does the last Rebel command centre survive once the rest of the base is gone (Blood Gulch)? | AI_TACTICAL §7.7.6, §12 item 12 | `alamo_headless`, TR Blood Gulch, Empire AI vs Rebel AI, object scripts on: the Rebels win at frame 18,731 (10 min 24 s; 59,083 without object scripts) since the 2026-11-04 land rules and company-target fix (was: Empire at 45,143) | TR |
| 6 | **Select All look**: what does Ctrl+A select with turrets, base structures, heroes, a starbase and docked fighters present? | COMMANDS §2.4.1 | Every owned SELECTABLE object without a parent: heroes, build-pad turrets and the Eclipse (`Not_Really_Selectable`) are selected. Base structures, star bases, defense satellites / orbital structures, proximity mines, TEAM members and docked / garrisoned units are not. TEAM containers expand to their members | vanilla (same skirmishes as 2) |

## What Gabriel does

Both sessions work the same way. Start Steam, then double-click the `.cmd`. It builds and installs the mod and
launches `StarWarsG.exe` with it. While the game runs, it scans the game's memory for the probe records. When you quit
the game it writes `out\<yyyymmdd-hhmmss>-<session>\report.txt` and removes the mod. The steps are printed in the
console too.

Rules for both sessions:
- **Never pause** and **never change the game speed** (probes 3 and 4 read the game clock).
- To leave a campaign, use Esc > Quit to main menu. **Never save** a probe campaign.
- Give no orders unless a step asks for one.

### Session 1: vanilla (about 20 minutes): `run_q6.cmd`

Launch: `StarWarsG.exe MODPATH=Mods\Q6T15Probe`.

**A. Probes 3 and 4 (about 3 minutes).**
1. Go to Single Player > Galactic Conquest > **"Q6 PROBE: STORY TIMING"** > **Empire** > Start.
2. Click nothing.
3. You will see:
   - "Q6 PROBE: event Q6S_Begin0 (STORY_ELAPSED 0) fired", at once (this text is the event's own reward);
   - "story timing running", after 3 s;
   - "Q6 PROBE: story timing done", after about 2 min 10 s.
4. Quit to the main menu.
5. Note whether the first message (Begin0) appeared.

**B. Probe 1 (about 7 minutes).** Three campaigns. In each one, pick **Empire**, start it and click nothing. You own
nothing at the start.
- **"Q6 PROBE: ELIM A (OWN NOTHING)"**: watch for 3 minutes, until "170 s passed and the campaign is still running"
  or until the campaign ends by itself.
- **"Q6 PROBE: ELIM B (OWN, THEN LOSE)"**:
  - at 20 s, "the script gave you Abregado Rae" appears (the planet turns Empire);
  - at 50 s, "...took Abregado Rae back. You own nothing again" appears;
  - then watch until the 170 s message, or until the campaign ends.
- **"Q6 PROBE: ELIM C (CONTROL)"**: expected to end at once (the Rebel wins). This is the control.

For each campaign, write down whether it ended, when (seconds after the start, or seconds after the "took it back"
line for B), and what the screen showed. Take a screenshot of any defeat or end screen. Then quit to the main menu.

**C. Probes 2 and 6, land (about 3 minutes).**
1. Set up the skirmish:
   - Skirmish > **LAND**, map **Geonosis**;
   - slot 1: you, **Empire**, team 1;
   - slot 2: **AI**, **Rebel**, team 2;
   - every other slot **closed**; defaults for everything else.
2. Start, and give no orders.
3. About 5–10 s in, "Q6 PROBE: selection test set (land) is next to your units" appears. It means:
   - a turret pair, a barracks, a turbolaser tower, Darth Vader, a stormtrooper squad, an AT-ST and a proximity mine
     have been spawned for you;
   - the probe's object-script units have been spawned near the AI.
4. Press **Ctrl+A**. Take a **screenshot of the whole screen** that shows the selection brackets and the
   selected-units panel. If the test set is off screen, take a second screenshot with the camera on it. Do not click
   on the map.
5. After "tactical part done" (about 2 min), press Ctrl+A again, take another screenshot, and quit the battle.

**D. Probes 2 and 6, space (about 3 minutes).**
1. Set up the skirmish: Skirmish > **SPACE**, map **Kuat**, same slots as C.
2. Start, and give no orders.
3. About 5–10 s in, "selection test set (space) ..." appears. A Star Destroyer, the Accuser (a hero), the Eclipse, a
   TIE fighter squadron and a defense satellite have been spawned next to your fleet. Your own space station is there
   from the start.
4. Press Ctrl+A and take a screenshot. Take another one once the Star Destroyer's fighters have launched, and a last
   one after "tactical part done".
5. Quit the battle, then quit the game. The run ends by itself (`Q6_RUN_DONE exit=<n>`).

### Session 2: Thrawn's Revenge (35 to 45 minutes): `run_q6_tr.cmd`

Launch: `StarWarsG.exe MODPATH=Mods\Q6T15ProbeTR MODPATH=..\..\..\workshop\content\32470\1125571106`. The probe's
overlay comes first. MODPATH / STEAMMOD items are searched in command-line order and the earlier one wins
(docs/PHASE2.md). If TR does not load that way, run `run_q6_tr.cmd --tr-steammod`, which loads TR with
`STEAMMOD=1125571106` instead.

**A. Probe 1, TR GCMenu (3 minutes).**
1. Go to Galactic Conquest > TR's **"Galactic Conquest Selection Menu"** (campaign `GCMenu_Rebel`) > Start.
2. **Do not pick anything** in its menus, and click nothing for 3 minutes.
3. Write down whether the campaign ends. Take a screenshot if it does. Then quit without saving.

**B. Probe 5, Blood Gulch (25 to 40 minutes).**
1. Set up the skirmish:
   - Skirmish > **LAND**, map **Blood Gulch**;
   - slot 1: you, the **Rebel** faction (TR's New Republic), team 1;
   - slot 2: **AI**, **Empire**, team 2;
   - other slots closed; default credits and settings.
2. Start. Write down the wall-clock time.
3. Give **no orders at all**, until the battle ends. Your units still defend themselves.
4. Write down:
   - when your last structure other than the command centre dies;
   - when the command centre dies;
   - the end screen, with a screenshot.
5. Quit the game.

The overlay's free-store recorder is read-only. It gives no orders, spawns nothing and keeps TR's service rates, so
the AI plays exactly as in plain TR.

### What to send back

- The folders `tools\probe_mod\q6_t15_probes\out\<run>-vanilla\` and `...\<run>-tr\`. `report.txt` comes first, then
  `records.log`. `out\latest.txt` names the newest one.
- Screenshots:
  - the Ctrl+A screenshots: land ×2, space ×3;
  - any ELIM / GCMenu end screen;
  - the Blood Gulch end screen.
- Your notes:
  - for ELIM A, B and C and for GCMenu: did it end, and at what time;
  - whether the "Q6S_Begin0 fired" message appeared;
  - the Blood Gulch times.

**Cleanup** runs by itself at the end of every run, also after Ctrl+C or an error. After a crash, double-click
`cleanup_q6.cmd`. It is safe to repeat. It deletes `corruption\Mods\Q6T15Probe` and `corruption\Mods\Q6T15ProbeTR`,
and only folders that hold `Q6T15_PROBE_MARKER.txt`. No retail file is changed. TR's workshop folder is only read.
Other mods are left alone.

**Driven run** (Claude through computer use; see the memory note on driving the game):
1. Run `run_q6.cmd --non-interactive --no-launch`, or `run_q6_tr.cmd ...`, in a terminal tab.
2. Launch the game windowed yourself, from the `corruption` folder, through a one-line `.cmd` started with
   `Start-Process explorer.exe`:
   `StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 <the printed MODPATH arguments>`.
3. `out\<run>\run_status.txt` shows the progress.

Exit codes:

| code | meaning |
|---|---|
| 0 | records found, report written |
| 10 | no records |
| 20 | preflight failed: the game or TR is missing, or the game is already running |
| 30 | interrupted |
| 31 | error |

## How each probe works and what the log shows

All records are Lua strings `Q6P|<KIND>|p=<probe>|sid=<Lua state>|n=<seq>|f=<frame>|...|END`, kept in a global table
of the probe Lua. `f` = round(`GetCurrentTime()` × 30), the mode frame. Retail `_CustomScriptMessage` and
`_MessagePopup` show nothing (S10 / Q2 / A1 runs). So `q6_run.py` finds the strings with a read-only scan of the
game's private memory every 4 s (`../a1_tactical/a1mem.py`). The `Game_Message` lines on screen are only for you.
Every probe uses only standard FoC Lua that the port implements too:
- the game commands `GetCurrentTime`, `Find_Player`, `FindPlanet`, `Change_Owner`, `Find_All_Objects_Of_Type`,
  `Spawn_Unit`, `Create_Generic_Object`, `Create_Position`, `Game_Message`, `Create_Thread` and `Sleep`;
- the object methods `Get_Hull`, `Get_Distance`, `Is_Category`, `Prevent_AI_Usage`, `Prevent_All_Fire` and
  `Make_Invulnerable`;
- the engine callback `Story_Event_Trigger` and the library hook `Pump_Service`, which PGBase's `PumpEvents` calls once
  per pump.

### 1. EliminationLatch (`q6_story_elim.lua`, campaigns ELIM A / B / C)

**Setup.** The galaxy has four planets: Tatooine, Byss, Abregado Rae and Jabiim.
- Rebel owns Jabiim (a barracks). The Rebel, Underworld and Pirates have AI_Player_Control None.
- The human Empire has `Home_Location` Tatooine but no `Starting_Forces`, so it owns nothing, as in TR's GCMenu.
- `Human_Victory_Conditions` is Galactic_All_Planets_Controlled, as in every TR campaign. The AI side has the same,
  except in C: `Galactic_Opponent_Controls_No_Planets`.

**What the plot Lua records.** It starts at `STORY_ELAPSED 1`.
- `START`: who owns each planet and how many planets / objects the human has.
- `HB`, every 5 s, until t+170 s.
- `GIVE` (t+20 s) and `TAKE` (t+50 s), in B only.
- `DONE`, at t+170 s. When the campaign ends, the galactic mode goes away and the heartbeats stop.

**The report:**
- `RAN_TO_END`: `DONE` was seen, so the campaign never ended.
- `STOPPED`: the last heartbeat came before 170 s. Your note tells whether it ended or was quit.
- `PREMISE FAILED`: the human owned a planet at START, for example because `Home_Location` gave it one.

**Expected results:**

| campaign | port (latch) | literal original reading (STRATEGIC_GAPS §1.4: a player loses only when another player's condition passes) |
|---|---|---|
| A, own nothing | goes on (`RAN_TO_END`) | goes on |
| B, owned once, then nothing | **ends** a few s after TAKE (human eliminated) | goes on |
| C, control | ends at once (the Rebel's condition passes) | ends at once |
| TR GCMenu (by eye) | goes on | goes on |

**How to read it:**
- If B goes on in retail, the original has no elimination test at all. Then the port's latch, and the elimination
  rule behind it, end campaigns that the original keeps running. Fix: drop the elimination test.
- If B ends, the latch matches.
- If A ends, the original eliminates even a player that never owned anything. TR's GCMenu would then need its story
  to give planets before the first check: compare the GCMenu note.

### 2. Object-script first pump (`q6_objscript_hook.lua` + `q6_tactical_hook.lua`, both skirmishes)

**The object scripts.** The kit copies six shipped object scripts into the mod with a recorder appended:
- `ATTACH` (inside `Definitions`), `MAIN` (first line of `main`);
- `PUMP` for the first 6 pumps, through `Pump_Service`. Each one carries `LastService` as that pump sees it, the
  object id and the frame.

During pump 2, `LastService` is still the value the engine wrote after pump 1, so `r = -LastService` is the drawn
value. The rates:

| script | type spawned | ServiceRate |
|---|---|---|
| `ObjectScript_PowerToShields` | Nebulon_B_Frigate (space) | shipped (1) |
| `ObjectScript_MissileShield` | AT_AA_Walker (land) | shipped (1) |
| `ObjectScript_MC30` | MC30_Frigate (space) | 6 (probe) |
| `ObjectScript_SelfDestructLand` | TIE_Crawler (land) | 6 (probe) |
| `ObjectScript_PointDefense` | Crusader_Gunship (space), HAV_Juggernaut (land) | 20 (probe) |
| `Interdictor` | Interdictor_Cruiser (space) | 0.3 (probe) |

**The spawner.** The shipped `BusyTacticalFreeStore.lua` gets `q6_tactical_hook.lua` appended. It runs inside the AI
player's free store, rides on the shipped calls and changes no rate. It spawns 5 of each type for the AI player at
frame ≥ 150 (wave 1) and 2 more at frame ≥ 2700 (wave 2, objects created late). They get `Prevent_AI_Usage`,
`Prevent_All_Fire` and `Make_Invulnerable`.

**The report** has a table per instance (spawn frame, pump 1 / pump 2 frames, r, r / rate) and these answers:
- `ANSWER bound`:
  - `hi = ServiceRate`: every r is in [0, rate], and r goes above 1 at rate 6 / 20. This is the port's rule;
  - `hi = 1`: no r above 1 even at rate 20. The port's `first_pump_draw_hi` must become 1;
  - `OTHER`.
- `ANSWER vanilla objects run their Lua_Script`: YES when the shipped-rate scripts pumped at least twice. If YES, the
  `[port interim rule]` (object scripts only with a mod) can go.
- Frames from the spawn to pump 1. Spec: the first service after the attach.
- Wave 2, frames from pump 1 to pump 2. Spec: the very next service.

### 3. Perpetual STORY_ELAPSED period (`q6_story_timing.lua`, campaign STORY TIMING)

The plot has these events, none with prereqs:

| event | STORY_ELAPSED | Perpetual |
|---|---|---|
| `Q6S_Begin0` | 0 | no |
| `Q6S_Begin3` | 3 (starts the thread) | no |
| `Q6S_Once2` | 2 (reference) | no |
| `Q6S_Cycle2` | 2 | yes |
| `Q6S_Cycle1` | 1 | yes |
| `Q6S_Cycle07` | 0.7 | yes |
| `Q6S_Cycle40` | 40 (TR's value) | yes |

The engine calls the plot script's `Story_Event_Trigger(name)` synchronously inside Fire (STORY §1.5 step 3). The
wrapper records a `TRIG` there, so each fire frame is exact. It keeps the first 40 fires of each event.

The report lists the first frames and a histogram of periods per event.

**Expected (port):** Cycle2 period **61** (fires 61, 122, ...), Cycle1 31, Cycle07 22, Cycle40 1201.
- A period of T × 30 + 1 means accumulation restarts at the service after the fire. This is the port's rule.
- A period of T × 30 means the timer does not lose a service.

### 4. Load-time story resume (same campaign)

Same script. It records `TOP` (file top level), `DEFS` (`Definitions`), `MAIN` (the first resume of the main thread),
the first `PUMP`s, the `TRIG` of each event (with `CurrentState` / `NextState`) and `ENTER` (state OnEnter). Record
order is execution order inside the plot's Lua state.

**The report:**
- `MAIN ... ran before the Begin0 fire`: the threads were resumed before the frame-1 ELAPSED dispatch, as in the port.
  `ENTER Q6S_Begin0` should follow.
- `Begin0 fired ... BEFORE main's first resume`: no load-time resume. If `ENTER Q6S_Begin0` is missing, PGStoryMode
  dropped the event (CurrentState ≠ NextState). Then the port's resume is the port's own fix for a start that works
  differently in retail.

The on-screen "Q6S_Begin0 fired" line is the event's own reward. It shows that the event fired, not that the state
was entered.

**Expected (port):** MAIN before TRIG Q6S_Begin0, ENTER Q6S_Begin0 at frame 1.

### 5. TR land end time (`q6_tr_land_hook.lua`, TR session)

TR's own `LandTacticalFreeStore.lua` is copied with a recorder appended. It runs inside the Empire AI's free store,
from TR's own calls only (`On_Unit_Service` every 2 s per idle unit, `FreeStoreService` every 20 s). It records:
- `S`, every ≥ 150 frames: the enemy (your) structure count; each command centre's hull; the number of AI units within
  600 and within 2884 of it; the nearest one;
- `GONE` for each enemy structure that disappears;
- `BASEGONE` when only the command centre is left;
- `CCGONE` when it is gone.

**The report:** `base gone at f=..., last CC gone at f=...: the CC survived N frames (m:ss)`, plus the CC's hull and
the number of nearby AI units every 30 s after the base is gone.

**Port:** since the 2026-11-04 land rules and company-target fix the Rebels win at frame 18,731 (10:24) with object scripts
(59,083 without; was: the Empire at 45,143, with the last CC repairing itself, AI_TACTICAL §7.7.6).

### 6. Select All (`q6_tactical_hook.lua`, both skirmishes)

**What the hook spawns for you:**

| mode | object | spec (COMMANDS §2.4.1) |
|---|---|---|
| land | `Empire_Anti_Infantry_Turret`, `Empire_Anti_Vehicle_Turret` | IN (build-pad turrets) |
| land | `E_Ground_Barracks`, `E_Ground_Turbolaser_Tower` | OUT (DUMMY_GROUND_STRUCTURE) |
| land | `Darth_Vader` | IN (hero) |
| land | `Imperial_Stormtrooper_Squad` | IN (TEAM container, expanded to its members) |
| land | `AT_ST_Walker` | IN |
| land | `Proximity_Mine` | OUT |
| space | `Star_Destroyer` | IN (its docked fighters: not spawned yet) |
| space | `Accuser_Star_Destroyer` | IN (hero) |
| space | `Eclipse_Super_Star_Destroyer` | IN (`Not_Really_Selectable` is not tested) |
| space | `TIE_Fighter_Squadron` | IN |
| space | `Empire_Defense_Satellite_Laser` | OUT (orbital) |
| space | your own starting space station | OUT (DUMMY_STAR_BASE) |

All of them are invulnerable. The hook then lists every object you own, with its parent (`OWN`, about 10 s after the
spawn and once more at 2 min). The report prints the spec's IN / OUT for each listed object. No Lua query reads the
selection, so **the answer is in your Ctrl+A screenshots**: compare the brackets and the selected-units panel with the
list.

## Files

| file | what |
|---|---|
| `run_q6.cmd`, `run_q6_tr.cmd` → `q6_run.py` | one session each: build + install, launch (`--no-launch`, `--windowed`, `--tr-steammod`), scan, report, cleanup; `--report-only <records.log>` re-reads an earlier run |
| `cleanup_q6.cmd` → `q6_cleanup.py` | idempotent cleanup of both mod folders (`--dry-run`, `--no-pause`) |
| `build_q6_mod.py` | `vanilla` builds `Mods\Q6T15Probe` (campaigns, plots, story Lua, the hooked free store, 6 hooked object scripts, text); `tr` builds `Mods\Q6T15ProbeTR` (TR's land free store + recorder, TR's faction list). Reuses `common/ted_owners.py` (install reader) and `build_probe.py` (`.dat` writer) |
| `q6_story_timing.lua`, `q6_story_elim.lua` | plot scripts (probes 3 + 4, probe 1) |
| `q6_tactical_hook.lua`, `q6_objscript_hook.lua` | free-store spawner (probes 2 + 6), object-script recorder (probe 2) |
| `q6_tr_land_hook.lua` | TR land recorder (probe 5) |
| `q6_parse.py` | report: one section per probe with ANSWER lines and the port's behaviour |
| `selftest_q6.py` | offline test (below) |

All Python runs with the project venv: `toolchain\venv\Scripts\python.exe -I <script>`.

## Offline validation

`toolchain\venv\Scripts\python.exe -I tools\probe_mod\q6_t15_probes\selftest_q6.py` (needs
`tools\probe_mod\out\tools\lua50.exe` from `common\build_lua50.cmd`) printed
**Q6 SELFTEST OK** on 2026-11-04. It:
- builds the vanilla mod from `extracted\foc_data_text.zip` and the TR overlay from the workshop folder, and compiles
  every built Lua file;
- runs the plot scripts, the hooked shipped free store (land and space) and all 49 hooked shipped object-script
  instances under stock Lua 5.0.2 with the game's own PG library and stub engines;
- runs the TR recorder on a stub free store;
- checks that the report gives each model's answers:
  - "port" model: load-time resume, bound = ServiceRate, B ends after the take-back, Perpetual re-init at the next
    service, which gives period 61;
  - "alt" model: no resume, so Begin0 is dropped; bound = 1; B runs to the end.

Not run in game yet (Gabriel's runs).

## Limits (what the probes cannot answer)

- **1:**
  - The end of a campaign is seen only as the heartbeats stopping. Your note tells "ended" from "quit".
  - The vanilla campaigns test the rule, and TR GCMenu is checked by eye only (no TR story hook).
  - The probe cannot tell which check ended a campaign: an elimination test, or a condition of another player.
    In B no other player's condition can pass, so an end there is an elimination.
- **2:**
  - ServiceRates 6 / 20 / 0.3 are probe overrides; every vanilla object script uses 1. The bound is read from those
    overrides, assuming it does not depend on the script.
  - The position of the draw on the synced stream (its effect on later AI / combat draws) is not visible from Lua.
  - Pool reuse (§1.4) is not exercised: all instances live at once.
  - The units are spawned for the AI with `Spawn_Unit`, including other factions' types.
- **3 / 4:**
  - Galactic time stops while the game is paused and runs faster at a higher game speed, so do not touch either.
  - The order of T9 (dispatch) against the Lua service is inferred from record order and frames inside the plot's own
    Lua state.
  - The tactical plot path (a mission's `_Begin`) is not covered.
- **5:**
  - The original's skirmish needs a human slot, so this is **human idle vs AI**, not AI vs AI as in the port run. The
    human side does not rebuild or repair by orders, but its structures' own self-repair still works.
  - Sample resolution is 2–20 s (TR's own free-store calls).
  - If the second MODPATH is not honoured, there are no `trland` records. Your stopwatch notes then are the result.
- **6:** screenshot only. Neither the selection list nor the Ctrl+A acknowledgement sound is logged.
