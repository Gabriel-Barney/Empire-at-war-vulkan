# cu9h t02 probe: the land bombing-run button, blob and ground point

Roadmap group cu9h, task t02. It checks the four open items of `docs/re/HUD_GAPS.md` §9.9 in a campaign land battle
where the human Rebel has two Y-Wing squadrons in orbit:

1. Does the `b_special_weapon` dial at 1.0 draw a full `i_button_build` overlay, or nothing?
2. Is the targeting blob green on a valid point and red on an invalid (fogged) one?
3. Does a click on the **disabled** button run its callback (0x346a80 → 0x2e7950)?
4. Who writes the HUD ground point +0x26dc (the blob position)?

Nobody has to listen for sounds. Item 3 is answered by a hardware write watchpoint on the targeting flag (HUD
+0x20b2): if the callback ran, 0x2e7950 sets that flag to 1. Item 4 is answered by write watchpoints on the ground
point. Items 1 and 2 need screenshots; the kit records the game's own state (dial value, button bit, blob colour) next
to each one.

Rules for every run: one game at a time, never save, never auto-resolve, keep the game window focused (FoC pauses
when it loses focus, and then the cooldown stops too). Steam must be running.

## Commands

| Command | What it does |
|---|---|
| `run_cu9h.cmd --no-launch --non-interactive` | installs `Mods\CU9HProbe`, waits for the game, watches it, writes the report when it exits, cleans up |
| `mark_cu9h.cmd [label]` | prints the bombing-run state now; with a label it also records it in `out\<run>\marks.log` |
| `cleanup_cu9h.cmd` | removes `Mods\CU9HProbe` (only with its marker file); safe to repeat |
| `selftest_cu9h.py` | offline check, no game (`CU9H SELFTEST OK`) |

The kit refuses to start while `StarWarsG.exe` runs (exit 20). Other options: `--target jabiim` (swamp map instead of
Tatooine), `--interval-sec N` (cooldown; 0 = the shipped 120 s; default 60), `--no-debugger`, `--dry-run`.

**Launch** (after the kit prints "Start the game yourself"): `Start-Process explorer.exe <run>\launch_game.cmd`. The
kit writes that file; it runs this in `F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption`:

    StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\CU9HProbe

Run all Python with `toolchain\venv\Scripts\python.exe -I`.

## Steps in the game

Coordinates are for the 1456×819 computer-use frame with the game window maximised on the 4K screen. They are scaled
from a 1920×1080 retail land screenshot (`../fixpass_capture/C2_land_silent_before_movie.png`) and the cu8g menu
coordinates. Check them on your first screenshot.

**Getting into the battle (about 3 min)**

1. Main menu: Single Player (1273,597), Galactic Conquest (1273,598). Pick **"CU9H PROBE: BOMBING RUN"**, faction
   **REBEL**, Start.
2. On the galaxy, **click nothing**. After 3 s you see "CU9H PROBE: story plot loaded ...", then "fleet assembled,
   flying to Tatooine", then "invasion ordered". Check with `mark_cu9h.cmd`: the phase is `GALAXY`.
3. If a Begin / Auto-Resolve bar appears at the top, click **Begin** (572,58). On the loading screen, click **Begin**
   (970,727). Never click Auto-Resolve.
4. `mark_cu9h.cmd` should now print `phase=COOLDOWN button=DISABLED dial=<n> ready_in=<s>`. If it prints
   `BATTLE_NO_RECORD`, the attacker got no bombing-run record. Note it, take one screenshot of the button, quit:
   items 1 to 3 cannot be checked in that battle.

**Fallback if the story does nothing.** If there is no "flying to" message within about 20 s, quit to the main menu
and start **"CU9H PROBE: BOMBING RUN (MANUAL)"** (REBEL). Its units start on Abregado Rae. Double-click Abregado Rae
to open the planet view. Drag the ground units on the surface into the fleet that holds the Y-Wings. Zoom out with
the magnifier (27,767). Drag the fleet to Tatooine: use left_mouse_down, several mouse_moves, then left_mouse_up (a
plain drag does nothing). Double-click Tatooine and drag one ground unit from the fleet onto the surface. Then Begin as
in step 3.

**The bombing-run button** is the top button of the column of four between the minimap and the command bar. It sits
at about (231,645); Call for Reinforcements is the third, at about (233,720). For a button zoom, use about
`[205,618,258,671]`. For the whole column with the reinforcement button as a plain-button reference, use about
`[200,615,265,795]`. Before every zoom of the button, move the mouse away from the HUD, for example to (728,300). Over
the button you would see the mouse-over texture.

**Item 3: click the disabled button (do this first, while `phase=COOLDOWN`).**

5. Left-click the button once. Wait 1 s and take a screenshot. A normal cursor and no blob are expected.
   Run `mark_cu9h.cmd q3_click_disabled_1`.
6. About 5 s later, repeat: `q3_click_disabled_2`.

**Item 1: the dial during the cooldown, then when ready.**

7. Run `mark_cu9h.cmd` a few times to read `dial=`. With the mouse off the HUD, zoom the button at about
   dial 25, 50 and 80. Mark each one: `q1_cool_25`, `q1_cool_50`, `q1_cool_80`. Run `mark_cu9h.cmd` right after
   each zoom; the `dial=` value it prints goes into the label.
8. When `phase=READY` (`button=ENABLED dial=100`; the game also says the bombing run is available), zoom the button
   and the column again. Mark `q1_ready`. Compare with the cooldown zooms. The answer is "full" if the whole button
   carries the dial overlay. The answer is "nothing" if it looks like a plain enabled button.

**Item 2: blob colours (targeting).**

9. Left-click the button. The cursor becomes the bombing crosshair and a blob follows the cursor on the ground. Run
   `mark_cu9h.cmd q2_targeting_on`.
10. Move the mouse (mouse_move) onto open, lit ground next to your own units: those points are in sight. Wait 1 s and
    zoom on the blob around the cursor, about 120×120 frame px. Mark `q2_open_1`. Do it for a second point:
    `q2_open_2`.
11. Move the mouse onto dark, fogged ground far from your units, such as the upper screen corners when zoomed out.
    Another way: click the minimap on a dark area; that moves the camera, and targeting stays on. Then hover the
    middle of the screen. Zoom on the blob and mark `q2_fog_1`, then `q2_fog_2` for a second point.
    `mark_cu9h.cmd` prints `blob=GREEN` or `blob=RED` each time, and the screenshot must show the same colour.
12. Optional: left-click on a fogged point. The game makes an error sound and targeting stays on. Mark
    `q2_click_fog`; it should print `targeting=1`.
13. Hover an open point at least about 150 units from your own units (the bombers hit everything). Left-click there.
    The run starts and the phase becomes `RUN_IN_PROGRESS`. Mark `q1_run`, zoom the button and mark `q1_run_zoom`.
    When the phase becomes `COOLDOWN` again, zoom it once at about dial 50 (`q1_cool2_50`). When it is `READY`
    again, zoom it once more (`q1_ready_2`).

**Item 4** needs nothing extra. The debugger logs the writers of the ground point at the start of the battle and
again each time targeting turns on (steps 9 to 13).

14. Quit: Esc, Quit Game (727,486), Yes (610,441), Exit on the results screen (1048,696), then Exit Game
    (1273,744). The run prints `CU9H_RUN_DONE exit=0`, writes `out\<run>\report.txt` and removes the mod.

**Expected duration:** about 15 min. Menus and galaxy take about 3 min, the load 1 min, the first cooldown 60 s of
game time, targeting 2 to 3 min, the run plus the second cooldown about 2 min, and quitting 1 min.

## Reading the results

`out\<run>\report.txt` (`out\latest.txt` names the newest run). Rebuild it with
`cu9h_run.py --report-only out\<run>`. The report has these sections:

- **battle / record**: the local player's bombing-run record: interval, ready frame, last start. It also lists every
  phase or button change, with the dial value the game wrote and the value the §9.3 formula predicts. The line
  "N differ by more than 1" checks the port's `bombing_run_cooldown_fraction` rules against retail.
- **Q1**: the dial and button bit at the q1 marks. `READY` must show `button=ENABLED dial=100`. The verdict comes from
  the screenshots: a READY zoom with the full overlay means "full"; one that looks like a plain button means "nothing".
- **Q2**: the colours the game wrote into the blob (+0xb8): GREEN (0,1,0,1) and RED (1,0,0,1), each with its ground
  point. The screenshots confirm the RGBA order: a RED mark must look red.
- **Q3**: every write to the targeting flag, with the button bit, the record state and the writer chain (function
  starts from the exe's `.pdata`). The verdict is one of these:
  - `CALLBACK_RAN`: the flag was set while the button was disabled.
  - `CALLBACK_NOT_RUN`: the q3 clicks are marked and the flag was never set while the button was disabled.
  - The control line must show one `flag:=1` write from your enabled click (step 9), with the writer
    `targeting_toggle(0x2e7950) <- b_special_weapon_click(0x346a80) <- ...`. This shows that the watchpoint works.
- **Q4**: the ground-point writes grouped by writer chain, with the number taken while targeting. Static reading of
  the decompile, done for this kit, says that the per-frame world mouse update **0x2ee40** writes x, y and z from the
  0x3bb2d0 pick of the cursor pixel (0x140b0a158 / 15c) in land, and a z = 0 plane hit in space. The HUD reset
  0x3035c0 zeroes them. The probe confirms or corrects this.

Other files in `out\<run>\`:
- `reader.log`: state changes, plus 10 samples a second while targeting.
- `dbg.log`: the watchpoint hits, each with a full state snapshot and the stack walk (`walk=`).
- `marks.log`, `live.txt`: the driver's marks, and the live state the runner rewrites 4 times a second.
- `config.json`, `run.json`, `run_status.txt`, `cleanup.log`.

## What the port does today (adjust after the probe)

| Item | Port | Code |
|---|---|---|
| 1 | Ready: no dial is drawn (`dial = -1`). Disabled: dial = fraction, and 0 is shown as 1.0 (full). | `engine/apps/game/src/hud_feeds.cpp:33-41` (`bombing_run_button`), `engine/ui/include/alamo/ui/tactical_hud.hpp:194-200` (`HudBombingRunButton::dial`), `engine/ui/src/tactical_hud.cpp:934-944` (`build_fill`), drawn by `engine/ui/src/hud_buttons.cpp:220-221` |
| 2 | Green (0,1,0,1) on a valid point, red (1,0,0,1) on an invalid one. Validity is the sim's `bombing_run_target_valid` (fog, base shield, prevention radius). | `engine/apps/game/app/app_input.cpp:689`, `:718-719` (`App::battle_input`); `engine/apps/sim/src/tactical_battle_l1_strikes.cpp:246` |
| 3 | A disabled button never runs its click: `HudButtons` skips the click. The toggle runs only for an enabled `b_special_weapon`. | `engine/ui/src/hud_buttons.cpp:97`; `engine/apps/game/app/app_input.cpp:433-437`, `:819-831` |
| 4 | Port choice: the blob point is the cursor ray hit, re-intersected 4 times at the terrain height, every input frame while targeting. | `engine/apps/game/app/app_input.cpp:676-687` |

## How it works

- **Mod** (`build_cu9h_mod.py`, built from the install; nothing from the game is stored in git):
  - `CampaignFiles.xml` plus `CU9H_Probe_Campaigns.xml` add two campaign sets:
    - the story campaign: Rebel home Abregado Rae with a barracks; Tatooine owned by the Empire, with a barracks and a
      stormtrooper squad; AI `None` for every other faction;
    - the manual fallback.
  - The story plot `CU9H_Story_Plots.xml` points to `CU9H_Story.xml`, which runs `CU9H_Story.lua` (from
    `cu9h_story.lua`). The script spawns these units for the human Rebel on Abregado Rae:
    - 2 `Y-Wing_Squadron` (Ship_Class bomber, `Land_Bomber_Type Y-Wing_Bombing_Run`);
    - 2 `Rebel_Infantry_Squad` and a `Rebel_Light_Tank_Brigade`.

    It then calls `Assemble_Fleet`, `BlockOnCommand(fleet.Move_To(Tatooine))` and `fleet.Invade()`. This is the
    S10 probe's sequence, which started a human invasion at once in retail (A1_READINGS §31). The Y-Wings stay in
    orbit, so the attacker gets a bombing-run record (L1_S4_REMAINING §3.7).
  - `GameConstants.xml` is the shipped file with `Max_` and `Min_Bombing_Run_Interval_Seconds` set to 60.
  - `MasterTextFile_English.dat` is the shipped file plus the probe strings.
  - Before writing anything, the builder checks that the target's land map resolves in the install.
- **Reader** (`cu9h_run.py`, `cu9h_mem.py`, through `../a1_tactical/a1mem.py`): it only reads memory. The layout is
  in `cu9h_mem.py`'s docstring:
  - the HUD object at RVA 0xb27f60;
  - the land mode's +0x38 bombing-run manager and its records;
  - the button component (table 0xa174e0, enum 0xe5, ButtonComponentClass: disabled bit +0xc0 & 4, dial
    +0x43c = int(f × 100)).
- **Debugger** (`cu9h_dbg.py`): it uses `../cu8h_probes/cu8h_dbg.py`'s Win32 bindings. It is attached once the galaxy
  is up and armed only in a campaign land battle (mode stack ≥ 2), so the menu background battle never arms it.
  Watchpoints:
  - Dr0 on HUD+0x26dc and Dr3 on +0x26e4. Their budget is 240 hits per battle, refilled each time targeting turns on.
  - Dr1 on the flag +0x20b2 and Dr2 on the blob pointer +0x20c0. These two are always on.

  It never writes game memory. It is stopped through `dbg.stop`, never killed: killing it would leave armed debug
  registers in the game, and the game would crash.

## Uncertainties

- The S10 auto-invasion was seen with a human **Underworld** attacker. A human Rebel takes the same code path
  (A1_READINGS §31), but this exact case has not run. If the story does nothing, use the manual campaign.
- In the bombing-run record rule, "player [1] of the battle record = the attacker" is [M] (L1_S4_REMAINING §3.7). If
  the report says `BATTLE_NO_RECORD` while `recs_enabled` names another index, the record went to the defender. That
  is a finding.
- The button coordinates are scaled from a 1920×1080 shot; check them on the first screenshot.
- The ButtonComponentClass offsets (+0xc0, +0x5f0, +0x43c) were read from the decompile for this kit. If the report
  shows `comp_vft` other than `863b60`, the component pointer is wrong and the button fields mean nothing (the
  watchpoints are not affected).
