# Main-menu background (attract demo) — what FoC renders behind IDD_MAIN_MENU_DIALOG

Source: `re_reference/ghidra` (StarWarsG.exe, FoC Steam x64), FoC/EaW XML, the FoC/EaW MEGs and
`GameData\Recordings`. Tags: [H] read directly in the code/data, [M] inferred from code with some gaps,
[L] guess. Addresses are RVAs (image base 0x140000000).

**Answer in one line:** the moving picture behind the FoC main menu is a **live, AI-vs-AI tactical space
battle** (`Data\Art\Maps\Demo_Attract_Kamino.ted`) loaded the moment the main menu opens, filmed by the
**in-game cinematic camera**, with the localized `Menuback_Overlay` texture stretched over the whole screen
as a 2D quad and the menu dialog on top. It is not a Bink movie, not a model scene, and it does not wait for
an idle timeout. [H]

## 1. Objects and globals

| what | where | notes |
|---|---|---|
| attract controller (static object) | 0x1409cf080 | +0x0 map index, +0x4 current map start time (ms), +0x8 last input time (ms), +0xc **running** (byte), +0xd **armed** (byte), +0x10 saved debug flags. UI.md §4.1 called it "camera helper" — it is this controller (mouse moves refresh its input time). [H] |
| GameConstants base | 0x140b15800 | `Demo_Attract_Maps` vector at +0x117b0 (data 0x140b26fb8, 32-byte strings; count 0x140b26fc0), `Demo_Attract_Start_Timeout_Seconds` 0x140b26fc8, `Demo_Attract_Map_Cycle_Delay_Seconds` vector at +0x117d0 (data 0x140b26fd8, count 0x140b26fe0), `Main_Menu_Demo_Attract_Mode` 0x140b2762c, `Localized_Menu_Overlay` +0x11cc8, `Localized_Splash_Screen` +0x11b48, `In_Game_Cinematics` 0x140b16dd2 (offsets from xml_field_tables.md) [H] |
| "attract demo is running" | GMM +0x1e4 = 0x140b155c4 | set by 0x22f10, cleared by 0x22bf0; read in about 40 places (§5) [H] |
| overlay quad | 0x140a61fd0 | `TextureRectClass`, in 2D scene #1 (0x140a619f0) [H] |
| fade-in quad | 0x140a62000 (+ start time 0x140a62008) | black rect in 2D scene #2 (0x140a619f8) [H] |
| version text | 0x140a61ff8 | "V %s %s", 2D scene #2 [H] |
| main menu open | 0x140a61fc8; dialog id 0x140a118e8; first-open latch 0x140a61fc9 | [H] |
| dialog manager | 0x140a78510; 0x140a785a8 ≠ 0 means a GUI dialog is open | [M for the exact meaning of +0x98] |
| in-game cinematic camera controller | 0x140b27a10 | "Start_Interesting_Object_Cinematic"; +0x34a active, +0x37c state (UI.md "targeting controller" is this object) [H for the string, M for the role] |

## 2. Flow: entering the main menu

Boot (MAIN_LOOP §5.1 step 13, corrected): `Logo1` (LucasArtsLogo.bik) → 0x65780 `Logo2` (PetroglyphLogo.bik) →
0x66140 (black screen + `TEXT_SAVELOAD_LOADING_MSG`, one present) → `Logo3` (empty Movie_File, ends at once) →
0x65850 → **main menu 0xc58e0**. (0x662f0 is the variant with the `Intro` movie, or, when the skip flag
0x140a61aa9 is set, starting the attract demo straight away.) [H]

Main menu open, `0xc58e0` (also the return point from credits, sub-dialogs, battle end, quit-to-menu): [H]
1. If the menu is already open, return.
2. Close any open dialog (0x1c1e00).
3. **If `Main_Menu_Demo_Attract_Mode`:** if the controller is not running → arm it (0x22bc0: armed = 1,
   start = last-input = now) and **start the first demo map now** (0x22f10). If it is already running →
   0x235f0 (the map-cycle check, §3).
4. If the overlay quad does not exist: name = `Localized_Menu_Overlay` entry for the current language
   (0x2c8160 with language 0x140a7c090), fallback `Menuback_Overlay.TGA`; texture = `PGRender::Get_Texture`
   0x25e990(name); quad = `TextureRectClass` 0x25ff10(x 0, y 0, w 1.0, h 1.0, texture); add to 2D scene #1
   (0x25c080).
5. Create `IDD_MAIN_MENU_DIALOG` (0x1c1570, callback 0xc6590); request **music `Main_Menu_Music_Event`,
   mode 13** (0x2d2070); build the version text "V %s %s" (colour 40,160,255,128) in 2D scene #2, placed
   bottom-left by 0xc6b80; then widget state calls (0x1c30d0, 0x1c29e0, 0x1c2a40, 0x1c2aa0) on
   `IDC_DIALOG_CLOSE_BUTTON`, `IDC_MAIN_MENU_RESUME_GAME` (depends on a game in progress, 0x56360),
   `IDC_MAIN_MENU_PLAY_DEMO`, `IDC_MAIN_MENU_SINGLE_PLAYER_GAMES`, `IDC_MENU_PETRO_LOGO` [M: which call is
   show/hide/enable].
6. **First open only** (latch 0x140a61fc9): create a full-screen **black rect (0,0,0,255)** in 2D scene #2
   (topmost) and record its start time.
7. Menu-open flag 0x140a61fc8 = 1.

Fade-in (dialog callback 0xc6590, message 0xc = per-frame update): while the demo runs and the
FrameSynchronizer logic frame (0x140b0a320) is < 4, keep resetting the fade start; afterwards
alpha = 1 − (now − start)/2000 ms; at ≥ 1 remove and delete the rect. So the first menu appears black and
fades in over **2 s** once the demo battle has simulated 4 ticks (hides the map load and first frames). [H]

## 3. The attract controller

### 3.1 Start next map — 0x22f10 [H unless tagged]
Does nothing when `Demo_Attract_Maps` is empty. Otherwise:
1. running = 1; save the debug flags 0x140a13e10 into +0x10 and clear them; **music lock** (music manager
   +0x80, 0x140b27e40) = 1; SFX manager +0x94 (0x140b27ef4) = 1 (AUDIO.md's untraced "GUI-only" flag [M]).
2. index = (index + 1) mod count (the first run therefore plays entry 1 mod count — entry 0 for a one-map
   list; with several maps the first one shown is the second entry [H, odd but as coded]).
3. If no GUI dialog is open, build a full-screen `Localized_Splash_Screen` quad (fallback `Splash.TGA`) for the
   load; it is deleted at the end [L whether it is ever drawn: no scene add was seen].
4. Tear down the current game: end all modes (0x28d940), reset players (0x297630), event queues, Lua, story,
   music manager (0x2d2790: queue cleared, current mode −1), SFX stop all (0x2d6f20), etc.
5. **`SaveLoadManagerClass::Load_Map`** 0x56480 on `".\Data\Art\Maps\" + name` (synchronously, on the main
   thread, no load dialog). Load_Map itself deletes the overlay quad (0xc6e30). Clear the scheduled event
   queue (0x2c1e30) and queue the map's start events (0x2c0ab0).
6. Players: the AI of player 0 = `BasicRebel`, player 6 = `AI_Player_Underworld` (0x2886d0) [M: which
   faction each index is]; every object of the query 0x2a9ff0(category 0x1a) gets byte +0x3cb = 1 [L: role].
7. **attract-running (GMM +0x1e4) = 1**.
8. **Start the in-game cinematic camera**: 0x2cca80(0x140b27a10, 1) → state 3, active; it first tries a
   combat shot (0x2cc780 → 0x4a30b0), then the mode's object (0x4a4ac0), then 0x49f370, then
   `Start_Interesting_Object_Cinematic` on a random object (0x2cd930 → 0x4a53f0). Its per-frame update
   0x2cd870 → 0x4aaf00 chains to a new shot when one ends (0x2cda20 / 0x2cd930). Gated by
   `In_Game_Cinematics` (True in FoC and EaW). During attract the 0.5 overlay call 0x44b2c0(0x140a28c10)
   is skipped [M: that call is the cinematic letterbox/overlay fade]. Starting a cinematic hides the HUD
   (0x49b1a0) but **does not hide the mouse cursor during attract** (explicit check). The same call **clears the FOW render
   option**, so the demo draws no fog (§4a). [M for the shot logic
   details; H for the calls]
9. 0xc6c30: `Main_Menu_Music_Event` mode 13 again, re-arm if needed, recreate the overlay quad exactly as in
   §2 step 4 (2D scene #1).
10. Map start time (+0x4) = now.

### 3.2 Per frame — 0x22c50(now) (main loop step 13, from 0x5d990) [H]
Only while armed:
- **Tactical pause flag** (FS +0xb0b0, 0x140b153d0) = (now − last input) > `Demo_Attract_Start_Timeout_Seconds`
  × 1000. So the 3600 s value is an **idle freeze**: after one hour without any key/mouse input the demo
  battle stops ticking (rendering continues); the next input unpauses it. It never *starts* the demo. Fast
  mode (+0xb0b5) is cleared every frame.
- Map cycling (only while running); delay = `Demo_Attract_Map_Cycle_Delay_Seconds[index]`, **120 s when the
  list is shorter than the map list**:
  - no dialog open: if not idle-paused and (now − map start) > delay → next map (0x22f10);
  - a dialog open (the normal case: the main menu): if not paused, the window is active (0x1409cf314), the
    user has been idle > **8 s** (main menu open) or **20 s** (another dialog), and (now − map start) > delay →
    next map. 0x235f0 is the same test, run when the menu is re-entered.
- With FoC data (one map, 1200 s): Kamino is reloaded from scratch every 20 minutes, but only after 8 s of
  idle. [H]

### 3.3 Input [H]
- Mouse move/buttons (WndProc 0x60c60 → 0x22ea0/0x22ee0) and keys (0x61fe0 → 0x22d80) refresh the last-input time.
- Keys (Win32 VK) reach 0x22d80 only while running **and no dialog is open** (debug/kiosk keys):
  Backspace = restart the current map, Enter = next map, Esc = stop the demo (as 0x22bf0 with 1),
  Insert = pull the map start time back by one cycle delay. With the menu open, input never stops the demo.
- Keyboard handler 0x61fe0, key 0x0d with no game running, the menu open and the demo not running → arm + start.

### 3.4 Stop — 0x22bf0(flag) [H]
If armed: disarm; if running: running = 0, restore the saved debug flags, music lock = 0, SFX +0x94 = 0,
end the demo game through the player-left/end handler 0x5f6b0(flag), attract-running = 0.
Callers: every front-end action that starts a real game or leaves the menu — campaign start 0xc5250 (it also
deletes the overlay 0xc6e30), skirmish/MP/tutorial/load-game launch paths (0xb6410, 0xbc270, 0xbd960, 0xc0ac0,
0xcd550, 0xd35e0, 0xe4aa0, 0xe4e60, 0xe9920, 0xef700, 0xf2690, 0x957a0, 0x2b800 …), the menu's close message,
`IDC_MENU_CREDITS` (credits_loop.bik plays instead, then 0xc58e0 restarts everything) and `IDC_MAIN_MENU_EXIT2`.
[H for the list, M for each caller's exact button]

### 3.5 Sub-dialogs [H]
`IDC_MAIN_MENU_SINGLE_PLAYER_GAMES` / `MULTIPLAYER` / `TUTORIAL` / `IDC_OPTIONS` / `IDC_LOAD_GAME…` only close
the main-menu dialog (0xc6440(1): deletes the version text and the fade rect, menu-open = 0) and open their
dialog. **The demo battle and the overlay stay**: the sub-menus show the same moving battle behind them.
`IDC_MAIN_MENU_RESUME_GAME` deletes the overlay and resumes the paused game.

## 4. What the demo is (data)

| | FoC (this exe) | EaW data |
|---|---|---|
| `Main_Menu_Demo_Attract_Mode` | Yes | Yes |
| `Demo_Attract_Maps` | `Demo_Attract_Kamino.ted` (commented out: + `Demo_Attract_Korriban.ted`) | `Front_end_demo_attract.rec` (commented out: three more `.rec`) |
| `Demo_Attract_Start_Timeout_Seconds` | 3600 | 1000000 (never) |
| `Demo_Attract_Map_Cycle_Delay_Seconds` | 1200 | 1200 |
| `Localized_Menu_Overlay` | ENGLISH `Menuback_Overlay.TGA`, GERMAN/FRENCH/… `Menuback_Overlay_german_french.TGA`, SPANISH, ITALIAN | same |
[H]

- **Demo_Attract_Kamino.ted** (maps.meg, 70 984 bytes) is a **space** map: environment `STARS_LOW`,
  backdrop `STAR_BACKDROP_GREEN_ORANGE`, `KAMINO_BACKDROP_LARGE` planet, `MOON_SMALL`, asteroid fields.
  Units (type-name CRCs found in the file): Rebel Home One, Calamari cruiser, Nebulon-B, Corellian corvette,
  Alliance assault frigate, Marauder, Millennium Falcon, X/Y/B/A-wing squadrons vs Underworld Kedalbe,
  Krayt destroyer, Vengeance frigates, Crusader gunships, Interceptor IV, StarViper and Skipray squadrons,
  Houndstooth, Peacebringer, Underworld star base 4, plus a Tartan cruiser. [H for the strings, M for the
  owner split]. Owner split read from the .ted (GALAXY_MAP_PRESENTATION.md §6) [H]: owners are faction-order
  player indices — 0 Rebel (15 units), 1 Empire (Tartan), 3 Neutral (planet, moon, asteroids),
  6 Underworld (16 incl. the station). The session must keep those indices; the port's skirmish-galaxy
  staging drops owners 3 and 6. Both sides are AI; there is no recorded input. Other FoC maps exist but are unused:
  `DEMO_ATTRACT_KORRIBAN/GEONOSIS/FOCDEMO.TED`, `FRONT_END_DEMO_ATTRACT.TED`.
- `Demo_Attract_Controller.lua` (marker `DEMO_CONTROLLER`) reveals the map for Empire/Rebel/Underworld and
  Attack_Moves every non-fighter unit to a random `STORY_TRIGGER_ZONE_00/01/02`. **No DEMO_CONTROLLER CRC was
  found in the Kamino map**, so on Kamino the battle runs on the two AI players alone [M].
  > **Data check 2026-10-31** [H, data]: the script is the `Lua_Script` of the `DEMO_CONTROLLER` marker (Markers.xml), so it
  > runs only on a map that places that marker. Type-name CRCs counted in every demo `.ted` (FoC and EaW maps.meg):
  > `Demo_Attract_Kamino`, `Demo_Attract_Korriban`, `Demo_Attract_FocDemo`, `Front_end_demo_attract` (both games),
  > `Space_Demo_Myrkyr` and the `Skirmish_Demo_*` maps place no `DEMO_CONTROLLER` and no `STORY_TRIGGER_ZONE_00/01/02`;
  > only `Demo_Attract_Geonosis` (not in either game's `Demo_Attract_Maps`) places 1 controller and 3 of each zone. So in
  > the shipped demos the controller never runs: their fog is not revealed by it. Script details: Empire → zone 02,
  > Rebel → 00, Underworld → 01; `GameRandom(1, 3)` indexes the zone list, so a map with fewer than three zones of a
  > type stops the script with a Lua error. The port reproduces the script in the session (`GameSession::attract_demo_script`).
- During the demo (attract-running = 1): 3D SFX voices are silent (AUDIO §1.6), mode-based music is suppressed
  (0x28e800), so only the menu theme plays; the music lock stops re-requests of mode 13 from restarting the
  theme while it is already current (0x2d21f0: refused only when current mode = 13, lock set, not forced) [H].
  Game-speed changes (0x27bf00/0x27bf80), world mouse/key input (0x30fc0, 0x33460), the battle-end callback
  (0x3639d0) and many HUD/dialog paths are skipped [H for the checks, M for their visible effect].
- **EaW** uses a recording: `GameData\Recordings\front_end_demo_attract.rec` (2 320 bytes) whose header holds
  the map path `.\Data\Art\Maps\Front_end_demo_attract.ted` (length-prefixed, at +0x10) followed by recorded
  commands. This FoC exe always calls Load_Map on `.\Data\Art\Maps\<entry>`; it has no `.rec` branch in
  0x22f10, so the EaW recording path lives in the EaW exe (not in re_reference) [L for how EaW plays it;
  probably the recording playback used by the quick-load-recording path 0x69040].
- EaW's own `MENUBACK_OVERLAY.DDS` in textures.meg is 512×384; both games' Patch2 replace it with 1024×768. [H]

## 4a. Why the attract view shows no fog (traced 2026-11-01)

**Rule:** the attract demo is filmed by the in-game cinematic camera, and **every cinematic shot start clears the FOW
render option (byte 0x140a12501)**, which is the switch every fog draw path tests. So the demo draws **no fog overlay on any
map**, with or without a `DEMO_CONTROLLER` reveal. The demo's *sim* fog is untouched (grids, targeting, AI use it as in any
space battle). [H for the code chain below; M that the G1 / EaW attract captures show exactly this, since a capture cannot
tell "not drawn" from "all revealed"]

Chain (all RVAs, image base 0x140000000):
1. `0x22f10` step 8 starts the camera: `0x2cca80(0x140b27a10, 1)`. It tries a combat shot (`0x2cc780` → `0x4a30b0`), then the
   mode's object (`0x4a4ac0`), then `0x49f370`, then `Start_Interesting_Object_Cinematic` (`0x2cd930` → `0x4a53f0`). [H]
2. **Every shot starter calls `0x49b1a0(camera, 0)` once it has set its shot up** (`0x4a30b0`, `0x4a4ac0`, `0x49f370`, and
   everything `0x4a53f0` dispatches to: `0x4a3b40`, `0x4a47d0`, `0x4a0140`, `0x4a4e30`, `0x4a17c0`, `0x4a26f0`, `0x4a6dd0`,
   `0x4a23d0`, `0x4a0830`, `0x4a1c90`, `0x4a5ef0`, `0x4a6790`). The per-frame update `0x2cd870` → `0x4aaf00` chains to the
   next shot through `0x2cda20` / `0x2cd930`, i.e. through the same starters, so the effect is continuous while the demo
   lasts. [H]
3. `0x49b1a0(camera, 0)` (the "cinematic presentation state" switch; the HUD hide of §3.1 is part of it) saves and changes
   several presentation switches and remembers them in camera bytes: **if the FOW render option (`0x13ecd0` reads it) is set →
   camera +0x311 := 1 and `0x13ecc0(0)` clears it** (code `0x49b1a0` lines `if (0x13ecd0() != 0) { +0x311 = 1; 0x13ecc0(0) }`,
   no attract or map condition); a second render switch (`0x48a680` / `0x48a3a0`, +0x312), a scene-flag pair (+0x313,
   `0x140b2a5f9`), +0x314 / +0x316 (counter `0x140b2c198`, flag `0x140b28d63`) and the world camera save (+0x310) are handled
   alike [M for what those are]. It also holds the mouse-cursor decision that §3.1 step 8 notes. [H for the FOW option]
4. `0x49b1a0(camera, 1)` (reached from `CinematicClass::End_Cinematic_Mode` `0x49b5c0` and from `0x2cbc90`) undoes it:
   if +0x311 is set, `0x13ecc0(0x140b15490 == 0)` → the option comes back on (0x140b15490 is cleared at start-up and read in
   about 50 places as a "skip graphics" test, never seen written otherwise: 0 in the shipped game [M]). [H]
5. The consumers of the render option (each tests `0x13ecd0`): the tactical mode's per-frame FOW renderer update (the
   `0x7db070` call, made only when the option is set), the space mode's FOW renderer update `0x4d8de0` (option clear → the
   renderer object's flag |= 1, which hides it), the scene draw paths `0x192d40` and case 4 of `0x1946f0` (the FOW
   renderer's passes through vtable +0x40..+0x58 run only when it is set), the FOW material choice `0x7dab30` (picks the
   FOW-off object when it is clear) and the fog value query `0x35e450` (returns 1.0 = fully visible when it is clear). [H for
   the gates, M for what each pass is]
6. Static defaults, read from the exe image: render option 0x140a12501 = 1, FOW-active 0x140a284c4 = 1. The space mode start
   (`0x4d7d10` → `0x35eb40`) does not set the option (it only clears it when FOW-active is 0 or when 0x140b15490 is set); only
   the land mode start (`0x3b86f0`) sets both to 1. The attract map is a space map, so nothing between the boot and the first
   shot re-enables it. [H]

What this rules out: the demo's *viewpoint player* is not the cause (`0x22f10` neither changes the local player nor the FOW
flags) and the FOW draw is not skipped by an `attract-running` (GMM +0x1e4) test: the gate is the cinematic camera.
Consequences:
- A demo map **with** a `DEMO_CONTROLLER` (Geonosis, not in any shipped list) shows no fog either; the controller's
  `Reveal_All` only changes the sim.
- If a shot cannot start at all (no combat unit and no interesting object), `0x2cca80` returns 0 with the option untouched and
  the fog would show with the normal RTS camera. With the shipped Kamino fleets this does not happen. Between shots the end
  call (`0x49b5c0`, only when chaining fails) restores the option for at most the frame in which the next starter clears it
  again. [M]
- A gap in the trace: the writes of the option are all via `0x13ecc0`; the starter of a normal skirmish (`0xbd380`) also ends
  with `0x13ecc0(0)`, and where a space skirmish turns the option on again was not found (§7 open items). It does not affect
  the attract trace: the option is only ever *cleared* on this path.

## 5. Composition [H unless tagged]

Frame order (RENDER §1, UI §1): the attract space mode renders its 3D scenes (backdrop, planet, ships, effects)
through the cinematic camera → mode 2D scene → HUD (hidden while the cinematic camera runs [M]) → **2D scene #1**:
the overlay quad (added before the dialog) and the GUI dialogs → **2D scene #2**: version text, the first-open
black fade rect, cursor.

- **Overlay texture:** `MENUBACK_OVERLAY.DDS` (the `.TGA` name resolves to `.DDS`), 1024×768, uncompressed
  A8R8G8B8, no mips. 86 % of the pixels have alpha 0: the opaque parts are a band at the top (rows ~32–230,
  the logo) and one at the bottom (rows ~640–768), both over roughly the left three quarters; the centre and the
  right side are fully transparent, so the battle shows through. The dialog's own `Frame_Background`
  (`MenuBack_overlay_panel`, 1024×512) is the button panel, part of the dialog, not this quad.
- **Placement / aspect:** `TextureRectClass` 0x25ff10 converts the normalised rect (0,0,1,1) to pixels with
  the back-buffer width/height (0x25e960/0x25e930), rounding `v·size + 0.5`; UVs (0,0)–(1,1). The 4:3 image is
  **stretched to the full window** at any aspect ratio — no letterboxing, no aspect correction.
- **Blend:** straight alpha (SRCALPHA/INVSRCALPHA) [M: TextureRect's draw path was not read; the texture is
  only meaningful alpha-blended].
- The quad lives until Load_Map, the resume button, credits/exit/close, or a game launch (0xc6e30) and is
  recreated by 0xc58e0 / 0xc6c30.

## 6. Port implementation notes

1. **Replace the idle-start model.** The port's `AttractController` (engine/apps/game attract_mode.hpp:
   "start after Start_Timeout idle, any input stops") is not the original: start the demo **immediately when the
   main menu opens** (if `Main_Menu_Demo_Attract_Mode`), keep it running under the main menu *and* its
   sub-dialogs, stop it only when a real game starts / credits / exit. Use `Start_Timeout` only as the idle
   **pause** threshold, and cycle maps after the per-map delay (default 120 s past the list end) when the user
   has been idle ≥ 8 s (menu) / 20 s (other dialog). Map index advances before the first load.
   > **Status 2026-10-30:** Done. `AttractController` now starts the demo as soon as the main menu opens, keeps it running under sub-dialogs, treats `Start_Timeout` as the idle pause and cycles maps with the 8 s / 20 s idle rule (`engine/apps/game/include/alamo/game/attract_mode.hpp:1-16`); the quoted "start after Start_Timeout idle" behaviour no longer exists.
2. **Demo session:** a normal tactical **space** session on `Data\Art\Maps\<entry>` with two AI players
   (Rebel `BasicRebel`, Underworld `AI_Player_Underworld`), no human input, FOW as the map/Lua sets it, battle
   end ignored (no end dialog; the 20-minute reload replaces it). It is deterministic sim like any session but
   its pause/cycle timing is wall clock (outside the CRC).
3. **Camera:** drive the in-game cinematic ("interesting object") camera continuously instead of the tactical
   RTS camera; hide the HUD; keep the mouse cursor visible; no letterbox bars.
   > **Status 2026-10-30:** Partly done. A [port] stand-in camera exists: 8 s shots on the next live combat unit, side view, pitch 35, wall time only (`App::demo_camera`, `engine/apps/game/app/game_main.cpp:891-915`; tuning in `attract_mode.hpp`). It is not the original's "interesting object" camera, whose shot logic is untraced, so this item stays open.
4. **Render (engine/render + apps/game):** draw the demo world first, then a full-screen 2D quad with the
   localized `Menuback_Overlay` texture (language → name from `Localized_Menu_Overlay`), UV 0–1 stretched to
   the window, straight-alpha blend, under the menu dialog; version text bottom-left; on the first menu open a
   black full-screen quad above everything that fades out over 2000 ms starting once the demo has ticked 4
   frames.
   > **Status 2026-10-30:** Partly done. The overlay quad (stretched to the window, straight alpha, under the dialogs), the version text and the 2 s black fade after 4 demo frames are drawn by the host (`engine/apps/game/app/game_main.cpp:813, 4260-4290`). What remains is that this is a host workaround (it removes the GUI backdrop's black fill while the demo is shown) until `engine/ui` can draw the overlay alone over a world; the host's own comment at `game_main.cpp:4261-4262` says so.
5. **Audio:** play `Main_Menu_Music_Event` (music mode 13) and do not let the demo's battle/ambient music or 3D
   SFX play (AUDIO §1.6 flag); don't restart the theme on returning to the menu while it is current.
6. **EaW data:** the `.rec` entry has to be handled by the port itself: read the map path from the recording
   header (or strip `.rec` → `Front_end_demo_attract.ted`) and either replay the recorded commands or run it AI vs
   AI. Choose and record the decision in docs/PHASE4.md.
   > **Status 2026-10-30:** Done, decision taken (Gabriel 2026-10-28): a `.rec` entry is not replayed; its header only names the map and the same live AI-vs-AI demo runs there (`attract_rec_map`, `engine/apps/game/src/attract_mode.cpp:66-84`; `App::start_attract_map`, `engine/apps/game/app/game_main.cpp:843-881`). An entry with no map path is skipped with a warning.
7. While the demo map loads (synchronous in the original), keep showing the previous frame / black; a job-thread
   load is fine as long as the menu stays responsive.

## 7. Open items
- TextureRect draw path (blend state, filtering) — confirm straight alpha in a capture.
- Exact owner split of the Kamino units. (Whether any object carries a Lua script: settled 2026-10-31, §4 data check:
  no `DEMO_CONTROLLER` on any shipped demo map.)
- ~~**Fog in the attract view.**~~ Settled 2026-11-01, §4a [H code chain, M capture match]: the cinematic camera clears the FOW
  render option at every shot start, so the attract view never draws fog. The port: `GameSession::view_draws_fog()` is false for
  the demo (`BattleView::capture`), whichever map or controller; the sim fog is unchanged.
- FOW render option: where a normal space skirmish sets it back to 1 after `0xbd380` ends with `0x13ecc0(0)` (not needed for the
  attract rule; the gate itself is FOG_OF_WAR.md §8 item 4).
- Role of byte +0x3cb set on the 0x1a-category objects at demo start.
- How the EaW exe plays `.rec` attract entries.
