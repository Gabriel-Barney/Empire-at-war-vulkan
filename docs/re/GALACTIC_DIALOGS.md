# Galactic dialogs and command-bar details (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-28 by the RE agent. Clean-room: behaviour, data and layout only, no code. Addresses are
RVAs in StarWarsG.exe. Component indices are the command-bar enum (name table of 0x48-byte records at
RVA 0xa21338 − 0x233·0x48; e.g. 0x233 = `pending_battle_shell`). Data: FoC `CommandBarComponents.xml`,
`GUIDialogs.rc`, `GameConstants.xml`, `Factions.xml`, `MasterTextFile_English.dat`, shell models from the
MEGs (`i_attack_mode*.alo`, `i_resolve_mode.alo` exist only in the EaW `GameData` models.meg; FoC overrides
`i_galactic_controls.alo` and `i_galactic_organize.alo`).

Confidence: **[H]** read in code or data, **[M]** read but intent inferred, **[L]** needs an in-game check.
Reference screenshots: base FoC 20–28.jpg, port 29–35.webp (2000 px wide).

Companions: UI.md §2 (command bar model), §3 (dialogs), §3.4 (fonts); BATTLE_TRANSITIONS.md §1.5–1.6 and
§4 (conflict state machine, auto-resolve maths); P4_SIM_SYSTEMS.md §9 (Withdraw, Abandon, retreat dialog);
STORY.md (story events); HUD_GAPS.md.

| item | original | port today | module |
|---|---|---|---|
| 1 battle prompt | top-centre banner shell, Begin / Auto-Resolve, no Retreat | centred dialog Fight / Auto-resolve / Retreat | ui (+ apps/game for the state hook) |
| 2 objectives | holocron button flashes; `IDD_STORY_SUMMARY_DIALOG` on click | `IDD_STORY_DIALOG` auto-opened | ui, apps/game |
| 3 auto-resolve | instant; red result line + `IDD_BATTLE_END_DIALOG` | "Combat Auto Resolve" progress dialog | ui, apps/game |
| 4 fleet organiser | top of screen, planet close-up camera only | bottom, over the galaxy view | ui (+ render for the shell anchor) |
| 5 build bar / resource panel | see §5 | see §5 | ui |
| 6 font Stretch_Factor | scales height, keeps unstretched width | width = height × stretch | ui (font rasteriser) |
| 2.6 story `PAUSE n` | boolean mode pause / unpause; no effect in the shipped game | counted, no effect | none (optional parser fidelity: apps/game) |
| 9 planet info | `planet_info` → `IDD_PLANET_INFO` (static type text, no gating, no pause) | `IDD_PLANET_DIALOG` screen exists, never opened | ui, apps/game |

> **Status 2026-10-30:** The "port today" column is out of date for rows 1, 2, 3, 6 and 9. Row 1: the top-centre `pending_battle_shell` / `story_pending_battle_shell` banner is built in `engine/ui/src/galactic_hud.cpp:205` and fed from `engine/apps/game/app/game_main.cpp:398`. Row 2: `IDD_STORY_SUMMARY_DIALOG` opens from the holocron (`game_main.cpp:2911`) and `IDD_STORY_DIALOG` serves random-plot offers. Row 3: the local player's auto-resolve opens `IDD_BATTLE_END_DIALOG` (`game_main.cpp:2489`). Row 6: `Stretch_Factor` scales glyph height only (`engine/ui/src/hud_font.cpp:153`). Row 9: `IDD_PLANET_INFO` opens from the organiser's planet_info button (`game_main.cpp:1722`).

---

## 1. Battle-pending prompt (28.jpg)

### 1.1 When it shows [H]
- Galactic conflict state machine (BATTLE_TRANSITIONS §1.5): when a human is attacker or defender and the
  battle is not auto-resolved outright (AI vs AI, overrun, MP option "always"), the conflict goes to state
  **0**, the galactic **pause level is raised** (0x28bb80), a story "battle pending" notification is posted
  (0x28d7f0) and the vote `PENDING_TACTICAL_BATTLE_VOTE` is opened. The same state is used in single
  player; the "vote" is only the local choice there.
- The banner itself is shown by 0x291330 (from 0x28d7f0 / 0x28d400). If other queued notifications are still
  pending (a GUI queue, 0x140b2b360..368), showing is deferred until that queue is empty [M].
- The game stays **paused** while the banner is up (pause counter GMM+0x2c > 0; the galactic clock and all
  movement stop; play/pause cannot release it). Every exit releases the level (0x28c720). [H]
- **No timeout in single player** [H]. In MP the banner text gets a countdown of
  `Battle_Pending_Timeout_Seconds` (GameConstants, vanilla 15, wall clock); when it expires the vote is
  resolved with the default (vote manager +0x180, option 2) [H for the timer, M for the default].

### 1.2 Shells and layout [H]
Two alternative shells, both `Type Shell`, `Hidden True`, `Model_Offset_X False` (horizontally centred),
`Model_Offset_Y True` (stuck to the **top** screen edge), `Manual_Offset True`, `Tutorial_Scene True`:

| shell (index) | model | bones (canvas units, relative to the frame bone) |
|---|---|---|
| `pending_battle_shell` (0x233) | `i_attack_mode_choice.alo` (texture `i_attack_mode_choice.dds`) | `choice_frame` (y +106 from root); `text_attack_choice` (0, −22); `choice_button_left` (−133, −59); `choice_button_right` (+132, −59); `graph_left` (−20, −57); `graph_right` (+20, −57) |
| `story_pending_battle_shell` (0x2ae) | `i_attack_mode_single.alo` | `attack_frame_single` (y +85); `text_attack_single` (0, −18); `attack_button_single` (+7, −58) |

The frame is the gold/olive plate hanging from the top edge; the title strip is on top and the two red
buttons sit left and right of the central "graph" pair (28.jpg). The pause banner (27.jpg, "GAME PAUSED" /
"RESUME GAME") is the sibling `pause_shell` (0x2ab, `i_attack_mode.alo`, `text_attack` + `attack_button`),
same anchor. A fourth shell `autoresolve_shell` (0x239, `i_resolve_mode.alo`, `resolve_button`,
`graph_left_resolve`, `graph_right_resolve`) is only ever hidden in retail (0x306290, 0x30c3b0) — vestigial,
do not show it [M].

### 1.3 Contents [H]
- **Title** (`text_attack_choice` 0x234 / `text_attack_single` 0x2af, set every frame by GameModeClass vf78
  0x364f30 through 0x3deac0): `<planet display name>` + `TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING`
  (": SPACE BATTLE IMMINENT!") or `TEXT_TACTICAL_LAND_BATTLE_IS_PENDING` (": PLANETARY BATTLE IMMINENT!").
  MP: `TEXT_MP_TACTICAL_{SPACE,LAND}_BATTLE_IS_PENDING` ("SPACE BATTLE IMMINENT:") + seconds left, or
  `TEXT_WAITING_FOR_OPPONENT` after the local vote. Colour = GameConstants `Battle_Pending_Message_Color`
  (255, 32, 32, 240, red); font from the component (EmpireAtWar-Medium 7; the small-caps look is the font).
- **Buttons**: `choice_button_left` (0x235) caption `TEXT_BUTTON_BEGIN` "Begin", tooltip
  `Hint_IDC_NEW_FIGHT_BUTTON` "Initiates the battle."; `choice_button_right` (0x236) caption
  `TEXT_BUTTON_RESOLVE_BATTLE` "Auto-Resolve", tooltip `TEXT_GUI_DIALOG_TOOLTIP_IDC_NEW_AUTORESOLVE_BUTTON`.
  Text colour 255, 64, 64 embossed; rollover texture `i_attack_mode_rollover.tga`; hover SFX
  `GUI_Highlight_Mouse_Over`. Single shell: `attack_button_single` (0x2b0) = "Begin" only.
- **Commander icons** (`graph_left` 0x237 / `graph_right` 0x238, filled by 0x3df8b0): one per side, the
  side's representative icon (its hero/commander portrait if it has one, else the lead unit icon) drawn on a
  "pancake" stack `fleet_empty.tga`, `fleet_stacks_01..05.tga`; the stack depth = ceil(unit count / a
  GameConstants divisor), capped at 5, offset 3 px per layer, icon scale 0.75. Left = the local player's
  force, right = the enemy [M for the side rule, H for the textures].
- **Sound** on show: faction `SFX_Event_Tactical_{Space,Land}_Battle_Pending` (`EHD_/RHD_/UHD_Battle_Pending`,
  "Tactical Battle Imminent") and the pending music (`Tactical_Battle_Loading_Loop_Event`) [H].

### 1.4 Which shell [H for the tests, M for their names]
The single-button shell (no auto-resolve) is used when any of: a tutorial/scripted lock global is set; story
rules forbid auto-resolve at this planet (story manager 0x140b30350: `DISABLE/RESTRICT_AUTORESOLVE`, the
planet list, a land/space-specific test); a player flag (+0x3c0) is set. Otherwise the two-button shell.

### 1.5 Buttons [H]
- **Begin** (handler 0x347580): SP → 0x28c7e0: conflict state 3 (start the tactical battle; load screen
  follows, UI.md §5), banner hidden (both shells, 0x3d26e0), pause level released. If the battle cannot be
  started (0x35f6b0 false) the state becomes 1 instead [L]. MP → vote option 0.
- **Auto-Resolve** (0x347850, only for 0x236): SP → state 6 with the *force* flag (GOM+0x634) set, banner
  hidden; MP → vote option 1.
- **Retreat: none.** The original has no retreat button on the prompt and no pre-battle withdrawal.
  Retreat routes are: inside the tactical battle (`b_retreat`, after `*_Retreat_Allowed_Countdown_Seconds`;
  BATTLE_TRANSITIONS §3.4), or Withdraw in the MP auto-resolve dialog (§3.2). The tactical in-battle dialog
  `IDD_RETREAT_DIALOG` ("Battle Resolution?": Cancel / Auto-Resolve / Retreat) is a different screen.
  **Correction to BATTLE_TRANSITIONS §1.6** ("Fight, Auto-resolve, Retreat"): the pending prompt has two
  buttons.

### 1.6 Port mapping
Replace the centred Fight/Auto-resolve/Retreat box (31.webp) with the two shells above: render the `.alo`
shell top-centre, fill text and graphs, wire Begin → state 3, Auto-Resolve → state 6 + force. Keep the
pause. **ui** (shell, text, buttons) + **apps/game** (choice → conflict state command). For replays, emit
the choice as a command (SP has no command today, the GUI writes the state directly) [M].

> **Status 2026-10-30:** The two shells are rendered top-centre, with Begin and Auto-Resolve as `HudAction::BattleBegin` / `BattleAutoResolve` clicks (`engine/ui/include/alamo/ui/galactic_hud.hpp:161-174`, `engine/apps/game/app/game_main.cpp:398-399`).

---

## 2. Campaign-start story / objectives

### 2.1 What the original does at campaign start [H]
Shipped GC story files open with an event like
`Event_Type STORY_ELAPSED, Event_Param1 0, Story_Dialog Dialog_Expansion_GC_Solo, Story_Chapter 3` (e.g.
`Story_Sandbox_12U_Underworld.xml`). When a story event with a `Story_Dialog` fires (0x455db0):
1. the chapter is **added to the mission holocron** (story log; 0x47dc70);
2. if `Story_Dialog_Popup` is true → the summary dialog opens directly at that chapter (0x47e2c0);
3. otherwise (every shipped event: STORY.md) → **no dialog opens**. Instead the holocron button
   `b_story_arc_g` (index 0x43, `i_button_command_bar_holocron*.tga`, tooltip "Mission Holocron", top-left
   of the resource panel) **flashes**, the faction `Default_Transmission_Message`
   (`TEXT_ENCYCLOPEDIA_HOLOCRON_UPDATED`, "The mission holocron has been updated.") is shown as a white
   screen-text line, and, if the event has `Story_Dialog_SFX`, the faction `SFXEvent_Mission_Added`
   (`EHD_/RHD_Mission_Updated`) plays (with the advisor's "incoming transmission" talking-head movie in
   `Movie_galactic`).
4. In MP a different hook is used (0x47edf0) [M].

So **nothing is forced open at campaign start**; 23.jpg is the player having clicked the holocron.
Clicking `b_story_arc_g` (0x347600) opens the summary dialog [M: in one player-flag state it only toggles a
UI flag].

### 2.2 `IDD_STORY_SUMMARY_DIALOG` (23.jpg) [H for controls, M for fill]
Built by 0x107280, callback 0x107bb0, close 0x107900. In SP opening it **raises the galactic pause**, closing
releases it. Controls (GUIDialogs.rc, 545 × 572 units, centred):

| control | text / role |
|---|---|
| `IDC_PLANET_REFERENCE_TITLE` | `TEXT_STORY_GOALS` "Active Story Objectives" (dialog title) |
| `IDC_RADIO_ACTIVE` / `IDC_RADIO_COMPLETED` | push-like radio tabs `TEXT_ACTIVE_STORIES_BUTTON` "Active", `TEXT_COMPLETED_STORIES_BUTTON` "Completed". Active is checked on open; Completed is disabled while no story is completed |
| `IDC_GOAL_LIST` | list of chapter titles of the selected tab (e.g. "Heart of the Maw"); the entry for the chapter that triggered the open is preselected |
| `IDC_STATIC` above the recap (placeholder `TEXT_STORY_GOALS`) | replaced by the chapter's `TITLE` text, e.g. `TEXT_STORY_TRANSMISSION_13` "Incoming Transmission from Darth Vader" |
| `IDC_STORY_RECAP` | the chapter body (below) |
| `IDC_HINTS` | `TEXT_STORY_HINT` "Hint: This menu recalls mission goals." |
| `IDC_SUMMARY_DIALOG_CLOSE_BUTTON` | "Close" |
| `IDC_SUMMARY_DIALOG_ABANDON` | always disabled/hidden, no handler (P4_SIM_SYSTEMS §9.1); 23.jpg shows no Abandon |

No Accept button and no image pane.

### 2.3 Chapter text [H for the format, M for rendering details]
`Data\Scripts\Story\<Story_Dialog>.txt`, sections `[CHAPTER n]`. Line keywords: `TITLE <key>` (the
transmission header), `TEXT <key>` (append a localised paragraph), `TEXTCOLOR r g b a` (colour of following
text; shipped: white 255 255 255 176, red 255 024 039 255 for the victory conditions), `NEWLINE n`,
`WAIT ms`, `SFX <event>`, `MOVIE` / `MOVIE_ONCE <movie>`, `IMAGE`, `DIALOG`, `PAUSE` (no effect, §2.6),
`EXTRA_TEXT` (append the event's extra text, e.g. the campaign's victory text). Full keyword table: §2.6. In 23.jpg the recap is: chapter title line, body
(`TEXT_CAMPAIGN_SINGLE_12` / `..._INTRO_E`), blank line, red "Empire Will Achieve Victory If:" + conditions +
"TIME LIMIT: 60 Galactic Days".

### 2.4 `IDD_STORY_DIALOG` (what the port shows, 30.webp) [M]
382 × 519 units: title `TXT_STORY_DIALOG_TITLE`, `IDC_IMAGE` (244 × 138, the chapter's MOVIE plays here — the
empty pane in 30.webp), `IDC_STORY_TEXT` listbox, buttons Accept / Close / Cancel. Created by 0x104b00 via
0x47e030, whose only caller builds the **random story plots** (`Random_Plot_%d`, 0x4bfdc0) — the offer of a
random mission the player may accept or decline. It is not the campaign-start screen.

### 2.5 Port mapping
apps/game: on a Story_Dialog event, add the chapter to the holocron log, flash `b_story_arc_g`, show the
transmission line and SFX; open nothing unless `Story_Dialog_Popup`. ui: implement the summary dialog per
§2.2 with the pause; keep `IDD_STORY_DIALOG` for random-plot offers only.

> **Status 2026-10-30:** The summary dialog exists and holds the ticks while it is open (`engine/apps/game/app/game_main.cpp:2911-2913`, `engine/ui/include/alamo/ui/gui_info_screens.hpp:163`); `IDD_STORY_DIALOG` is used for random-plot offers (`game_main.cpp:2834`).

### 2.6 Story script format, keyword table and `PAUSE` (added 2026-10-28)

**Parser** (0x47eac0, loader 0x47d9c0 "Loading story dialog script %s", path `data/scripts/story/<name>.txt`,
cached by name CRC) [H]:
- The whole file is one token stream; delimiters are space, tab, CR, LF and `]`. Line breaks have no meaning,
  so an argument may sit on the next line.
- A token that **starts with `#`** skips the rest of its line. `;` and `//` are not comments.
- The keyword is upper-cased before lookup (case-insensitive). Arguments keep their case.
- Keyword table at 0xa2a1d0: 21 records of {char name[32], int arg kind}. Arg kinds: 0 none, 1 one integer
  (atoi), 3 one string token, 4 four integers (atoi; the 4th defaults to 255 when the file ends).
  Kind 2 (one float) exists in the parser, but no keyword uses it.
- `[CHAPTER n]` (index 20) starts a new chapter numbered atoi(n); commands before the first chapter header
  are dropped.
- **An unknown keyword aborts the whole file**: "Story script error!  Unrecognized keyword : %s". The loader
  returns false and caches nothing, so the dialog has no chapters at all.

| idx | keyword | arg | summary executor 0x1089e0 (holocron, the path every story event uses) | `IDD_STORY_DIALOG` executor 0x1053c0 (random plots) | uses FoC / EaW |
|---|---|---|---|---|---|
| 0 | SFX | string | play SFX event | same | 538 / 414 |
| 1 | MUSIC | string | play music event [M] | same | 0 / 0 |
| 2 | DIALOG | string | play speech event | same | 113 / 113 |
| 3 | MOVIE | string | looped movie | looped movie in `IDC_IMAGE` | 152 / 148 |
| 4 | MOVIE_ONCE | string | one-shot movie | same | 52 / 52 |
| 5 | ANIM | int | ignored | play anim n on the dialog's 3D scene object [M] | 0 / 0 |
| 6 | IMAGE | string | show image | same | 2 / 2 |
| 7 | TEXT | string | append the localised paragraph | same | 963 / 712 |
| 8 | EXTRA_TEXT | none | append the event's extra text | same | 153 / 117 |
| 9 | TITLE | string | header text | `IDC_STATIC` header | 251 / 184 |
| 10 | CLEARTEXT | none | ignored | clear `IDC_STORY_TEXT` | 0 / 0 |
| 11 | TEXTCOLOR | 4 int | colour of the following text (r g b a) | same | 824 / 634 |
| 12 | NEWLINE | int | n blank lines | same | 607 / 481 |
| 13 | WAIT | int | hold the next command for n **ms of wall-clock time** | same | 703 / 527 |
| 14 | **PAUSE** | int | **ignored** | game-mode pause / unpause (below) | 23 / 23 |
| 15 | LOADCINEMATIC | string | ignored | load a cinematic into the dialog scene [M] | 0 / 0 |
| 16 | CLEARIMAGE | none | clear the image | same | 0 / 0 |
| 17 | HIDECINEMATIC | int | ignored | set a hide flag on that scene object [M] | 0 / 0 |
| 18 | STOPMOVIE | none | stop the movie | same | 0 / 0 |
| 19 | WAIT_SPEECH | none | hold until the current speech ends [M] | ignored | 0 / 0 |

Counts are over the shipped `Data\Scripts\Story\*.txt`: FoC 92 files, EaW 85. EXTRA_TEXT includes 5 files
that end on EXTRA_TEXT without a newline. **Unspecified commands in the shipped files:** only `PAUSE` and
`IMAGE`. IMAGE appears twice, both in DIALOG_TUTORIAL_07. MUSIC, ANIM, CLEARTEXT, LOADCINEMATIC, CLEARIMAGE,
HIDECINEMATIC, STOPMOVIE and WAIT_SPEECH are never used. Malformed: `DIALOG_TUTORIAL_07.TXT` line 49
`TEXTCOLOR255 255 255 176` (missing space; present in both games). It is an unknown keyword, so the original
rejects the whole file. No shipped XML or Lua references that file, so the rejection has no effect [H].

**Timing** [H]: both executors run from the dialog's timer event (and once on open) with now = timeGetTime() −
the app start time, in ms. WAIT n sets "next command due" to now + n. The executor then runs commands until
one is not yet due. This is wall-clock time: it is independent of sim ticks, game speed and the pause. The
holocron summary dialog itself raises the pause while it is open (§2.2), so the reveal plays over a paused game.

**PAUSE n** [H code, M routing]:
- It is not a delay, a typewriter speed or a reveal timing. n is a boolean. The executor tests the low byte
  of the stored integer: n ≠ 0 calls current game mode vf+0x38, and n = 0 calls vf+0x40.
- In the holocron summary dialog (0x1089e0), PAUSE has no case and is skipped.
- In `IDD_STORY_DIALOG` (0x1053c0), the effect depends on the mode:
  - Galactic mode (GalacticModeClass vtable 0x8762c0): vf+0x38 and vf+0x40 are both an empty `ret` (0x1fa90). PAUSE does nothing.
  - Land / space modes: vf+0x38 = 0x3ba3b0 sets the tactical pause (mode +0x99, then FS pause byte 0x140b153d0 unless MP).
    vf+0x40 = 0x3baed0 clears both and the fast-mode byte. These are the toggles of MAIN_LOOP §"tactical pause".
  - `IDD_STORY_DIALOG` only opens for random plots in galactic mode, so this branch is unreachable with shipped data.
- **Net effect in the shipped game: none.** It touches neither sim time, nor the galactic pause counter
  (GMM +0x2c), nor presentation. The intent shows in the file comments, e.g. "# Wait 10 seconds and then
  unpause the game": PAUSE 1 = pause, PAUSE 0 = unpause, written for a dialog that paused the game itself.
- Shipped uses: 23 per game (8 × `PAUSE 1`, 15 × `PAUSE 0`) in 13 files. Only 2 of those files are
  referenced: `Dialog_E_Intervention_Hunt_Capital_Ships` and `Dialog_R_Sandbox_Event_Fleet_Rampage`, both
  via EaW intervention Lua `event.Set_Dialog`, which shows them through the story-event path (§2.1).

**Port deltas** (`engine/apps/game/src/story_script.cpp`):
1. Keep PAUSE parsed and **without effect** (it is counted today: correct). Do not pause the sim.
2. Optional fidelity fixes, none visible with shipped data:
   - case-insensitive keywords;
   - `;` and `//` are not comments;
   - an unknown keyword invalidates the whole file instead of being counted;
   - token-stream parsing across lines;
   - the TEXTCOLOR alpha defaults to 255;
   - recognise the 8 unused keywords instead of counting them as unknown (MUSIC, ANIM, CLEARTEXT, LOADCINEMATIC,
     CLEARIMAGE, HIDECINEMATIC, STOPMOVIE, WAIT_SPEECH).
3. WAIT is ms of wall-clock time (port: seconds of presentation time, the same thing).

---

## 3. Auto-resolve flow and screens

### 3.1 Single player [H]
- Choosing Auto-Resolve sets state 6 with **force**; the next GOM service runs the whole resolution in a loop
  within one tick (BATTLE_TRANSITIONS §4.1), so **no in-progress screen** is shown. `AutoResolveDisplayTime`
  (0.01 s) and the `*_Auto_Resolve_Delay_Seconds` (0.01) make the unforced path immediate too.
- **Result, step 1**: a red (255, 24, 39) screen-text line when the local player took part or the planet is
  visible to them: `TEXT_AR_{SPACE,LAND}_{WIN,LOSS}_{MAJOR,REGULAR,MINOR}`, e.g. "##0 minor space victory at
  ##1. Ships lost: ##2" (##0 = the local player's faction name, ##1 = planet, ##2 = own losses), plus
  `TEXT_AR_HERO_LOSS` "##0 defeated at ##1" per dead named hero. Major/regular/minor thresholds:
  BATTLE_TRANSITIONS §4.1 step 4.
- **Result, step 2** (local player involved, SP, not an overrun): `IDD_BATTLE_END_DIALOG` opens (0x88050 via
  hook 0x140b15658), the same results screen as after a tactical battle, and **pauses** the galaxy:
  - `IDC_TITLE_STATIC` `TEXT_WIN_BATTLE` "You have won the battle!" / `TEXT_LOST_BATTLE`;
  - `IDC_BATTLE_END_PLANET_STATIC` planet; `IDC_TIME_STATIC` `TEXT_BATTLE_TIME` " %02d:%02d:%02d";
  - `IDC_STATIC_BOUNTY` `TEXT_SUMMARY_BOUNTY_COLLECTED` "Salvage collected: $ %d" (credits);
  - `TEXT_YOUR_LOSSES` / `TEXT_ENEMY_LOSSES` with `IDC_YOUR_LOSS_VAL_STATIC` / `IDC_ENEMY_LOSS_VAL_STATIC`
    (credit value of losses), hero portraits `IDC_YOUR_HERO0..4` / `IDC_ENEMY_HERO0..4`, unit icon + name
    grids `IDC_YOUR_UNIT0..11` / `IDC_ENEMY_UNIT0..11` with scroll bars, efficiency `%d%%`
    (`IDC_EFFICIENCY_STATIC[2]`); types costing 0 are skipped;
  - buttons: Continue (`TEXT_BUTTON_CONTINUE` / `TEXT_BUTTON_RETURN_TO_GALACTIC`), `IDC_LAND_INVADE_BUTTON`
    "Invade System" (space win with an invasion possible), `IDC_SAVE_REPLAY_BUTTON` and the close button
    disabled here.
  [H for controls and strings, M for which button set appears after auto-resolve.]
- Losses in both steps come from the auto-resolve battle record filled by 0x60da50: a destroyed transport
  contributes one entry per carried ground unit and none for its hull, so "Ships lost: ##2" and the loss
  grids include ground units that died in a transport (BATTLE_TRANSITIONS §4.4, "Loss records for carried
  units"; [H] for the text line, [M] for the dialog).
- Overrun auto-resolves (and AI vs AI) show only the text line.

### 3.2 The "Combat Auto Resolve" dialog (port 33.webp) [H]
`IDD_AUTO_RESOLVE_DIALOG` (537 × 406: `TEXT_AUTO_RESOLVE_COMBAT`, Invading/Defending force lists, two
progress bars with side icons, "Advanced…" checkbox toggling the `_LITE` variant, Withdraw) is opened only by
GUI hook 0x140b15608 (space) / 0x140b15628 (land), i.e. **MP with the auto-resolve option "Always"**, timed
by `*_Auto_Resolve_Delay_Seconds`. Withdraw → `WithdrawlEvent` (49) → Set_Retreating (P4_SIM_SYSTEMS §9.2).
MP results: `IDD_MP_AUTO_RESOLVE_RESULT_DIALOG` with the same text line and Close. `IDD_AUTO_RESOLVE_RESULT_DIALOG`
(your/enemy loss lists, Continue, Go To Planet) has no caller in retail [M].

### 3.3 Port mapping
apps/game: SP auto-resolve resolves immediately, then posts the text line and opens the battle-end screen.
ui: battle-end dialog (shared with the tactical end). Keep the progress dialog behind the MP "Always" path.

> **Status 2026-10-30:** The local player's galactic auto-resolve now ends in `IDD_BATTLE_END_DIALOG`, and the ticks wait for Continue (`engine/apps/game/app/game_main.cpp:2489-2494`, `show_auto_resolve_end`; figures from `engine/apps/game/include/alamo/game/battle_summary.hpp`).

---

## 4. Fleet organiser (26.jpg)

[H for data and gating, M for the meaning of the camera state]
- Shell `i_main_organize` (0x5a), `i_galactic_organize.alo`, `Model_Offset_X False` (centred),
  `Model_Offset_Y True` — anchored to the **top** edge. Bones: three frames `q1_frame` (−347, 199),
  `q2_frame` (1, 199), `q3_frame` (345, 199) with slots `q{1,2,3}_0000..0406` (`q*_0000` = pathfinder slot,
  `TEXT_TOOLTIP_PATHFINDER`), labels `Text_Fleet1..3` / `text_fleet1..3` (0x271–0x273) at y 90 =
  `TEXT_FLEET_PANE_0..2` "Fleet 1/2/3" (black EmpireAtWar-Medium 10 on the gold tab), `icon_fleet01..03`;
  plus garrison pads `Pad_00..09` ("Open Base Garrison Slot"), `Planet_structure_00..08`, `Slot_00/01`,
  `space_Pad_*`, `advanced_planetary` ("Base Layout" button, enters the FoC planet map), `planet_info`
  ("Planet Info"), `Text_Land_Fleet`, big fleet icons `q1..q4_fleet`, `collision_background`.
- **Shown only** (0x3e83a0, every HUD update) when a planet is selected (+0x2150), the galactic camera is in
  its planet close-up state (camera +0x44 = 3, the zoom-in of 26.jpg), the organiser flag (+0x26a4) is set,
  and the advanced planet map is not open. Otherwise hidden. Entering the planet map hides it and the main
  bar; leaving restores both (0x3c5fc0 / 0x3ce970).
- Port (33/34.webp) shows the frames over the galaxy view and anchored low (over the command bar). Fix: top
  anchor per `Model_Offset_Y`, and show only in the planet close-up. **ui** (+ the shell anchor in the HUD
  layout).

---

## 5. Galactic command bar details

Shell `i_galactic_controls.alo` is anchored bottom-left; canvas = 1024 × 768 units scaled by
screen height / 768 (UI.md §2.4). Positions below are bone positions in canvas units from the bottom-left.

### 5.1 Build buttons [H data/bones, M colour rules]
- 26 pads `b_Create_00..25`, two rows: row 1 (even) y 67..117, row 2 (odd) y 12..62, x from 307 to 1005,
  54-unit pitch (13 columns). Group `Structure Buttons` 00..15, `Hero Buttons` 16..25.
- Pad background = `Blank_Texture_Name` list `i_button_pad_rebel.tga i_button_pad_empire.tga` (disabled:
  `..._dark`). **Despite the names, `_rebel` is the amber/orange pad and `_empire` the blue pad** (atlas
  `MT_CommandBar`). 25/26.jpg (Empire, land tab) show amber pads, so the index is not "Empire = 1". It follows
  the build tab: land/ground tab → index 0 (amber), space tab → index 1 (blue), matching the queue holograms
  (`i_button_hologram.tga` blue = space, `_land` orange = ground) [M; confirm with a space-tab screenshot].
  The port shows blue pads on the land tab (33.webp).
- Cost text: `Text_Offset 0 0`, EmpireAtWar-Medium 6, outlined. In the original it sits **centred along the
  bottom edge inside the pad** and is **green** when affordable (25.jpg) [H screenshot]; red/grey when not
  [L]. Port draws it white at the top. `Text_Color2` (255, 189, 26, 180) with `Text_Offset2 0 −20` is the
  second line (queued count / bonus) [L]. Build progress = `i_button_build.tga` radial dial.

### 5.2 Build queue strip [H]
`queue00..09` bones at y 174 (meshes y 147..201), x 343..960: the **row of dome lights along the top edge of
the bar**, above the build rows (25.jpg: five blue domes = space queue `queue00..04,06` group `Build_Queue`;
five orange = ground `queue05,07..09` group `Build_Queue_Land`). A queued item is shown as a hologram cone
(`i_button_hologram[_land].tga`) rising from its dome with the unit icon, progress dial `i_button_build[_land].tga`,
small count text at (0, −20) in 192,192,255 / 255,192,192. The port draws queued items as boxes over the
second build row (35.webp); they belong on the dome row.

### 5.3 Resource panel (25.jpg) [H]
| bone (x, y) | component | content |
|---|---|---|
| (48, 255) | `b_story_arc_g` | holocron button |
| (73, 256) | `Text_Allegiance_A` (Icon) | faction emblem of the selected planet's owner: `Icon_Alternate_Texture_Name` rebel / empire / underworld / pirate / jabba `i_button_command_bar_*_symbol.tga`, cross-fade |
| (175, 257) | `Text_Planet` | selected planet name, EmpireAtWar-Bold 9 outlined, max width 110; **shown in the owner's colour** (blue for Empire in 25.jpg) [M for the colour source] |
| (290, 256) | `Text_Planetary_Income` | planet income, white EmpireAtWar-Medium 7, right-justified |
| (289, 235) | `Text_Day_Credits` | total income per day, gold 255,212,33, right-justified |
| (289, 215) | `Text_Credits` | credit reserve, gold, right-justified, `TEXT_TOOLTIP_GALACTIC_RESERVE` |
| (66, 235) / (66, 215) | `Text_Day_Counter` / `Text_Tech_Level` | "Day: n" / "Tech: n" (Arial bold 8, gold) |
| (184, 215) | `Text_Pop_Cap` | "38 / 71" + pop icon |
| (143, 236) | `fight_bar_g` | day progress pips |

- The **credits symbol** before each of the three money values is the atlas texture `i_icon_credit.tga`,
  drawn left of the right-justified number [M: texture name from the atlas, placement from 25.jpg]. Numbers
  use EmpireAtWar-Medium (stylised digits), not Arial.
- Planet row is empty when no planet is selected (28.jpg). Port shows the name in white mixed case
  (33.webp) without the emblem; the original shows the name in the font's caps style at the bone above.

### 5.4 Yellow rectangle above the bar (35.webp, x ≈ 990–1205, y ≈ 1010–1045 px) [L]
It is about canvas x 608–740, y 141–162, i.e. straddling the `queue04`/`queue05` domes. No visible shell mesh
sits there: every bone mesh of `i_galactic_controls.alo` except the radar, faceplates and help droid is
`hidden` (placement markers only, never drawn). Most likely causes, in order: (1) a hidden marker mesh
(`queue0x`, `b_Create_xx` …) being drawn — the port must skip meshes flagged hidden; (2) a No_Shell
component drawn with an unresolved texture as a flat tint (e.g. a queue hologram quad with no item); (3) a
pending/organiser shell piece. Settle by dumping the component/mesh under that rectangle. It should not be
visible in the original.

---

## 6. Font `Stretch_Factor` [H] (WindowsFontClass::Load_Font 0x2706c0)

1. The font is created with height h = −(point size × screen-height-scaled DPI / 72) (UI.md §3.4) and
   **width 0** (the face's natural width).
2. If stretch s is not within 0.9999..1.0001: the engine reads the text metrics of that font, keeps its
   **`tmAveCharWidth`** (the unstretched average character width), deletes it, and creates it again with
   **height = floor(h × s + 0.5)** and **width = the unstretched average width**.
3. So stretch scales the **glyph height only**; horizontal size stays that of the unstretched font. s > 1 =
   taller, narrower-looking text (`MAIN MENU` at 1.3 is 30 % taller, same width as at 1.0); s < 1 = squat
   text (0.25 / 0.20 are used for the `IDC_PLAYER_COLOR_n` swatches, Arial Unicode MS, i.e. flat wide blocks).
   Nothing at draw time scales the glyph quads by s [M].
4. Port fix: rasterise at pixel height |h|·s with the advance/width scaled so the average advance equals the
   unstretched font's average advance (a horizontal scale of 1/s relative to a plain size-|h|·s font). Do not
   pass height × s as the width. UI.md §3.4 previously said "width = height × stretch": corrected there.

---

## 7. Open items
- Space-tab screenshot to confirm the pad index rule (§5.1) and the unaffordable cost colour.
- Which side is `graph_left` in a battle the local player attacks (§1.3).
- Source of the planet-name colour (owner colour vs. fixed blue).
- 35.webp rectangle: port-side dump of the item drawn there.
- Exact button set of `IDD_BATTLE_END_DIALOG` after an auto-resolve (Invade shown?).

## 8. Entry points
| RVA | role |
|---|---|
| 0x291330 / 0x28c6c0 / 0x28c7e0 | show pending banner + SFX / hide / Begin (SP) |
| 0x3ca480 | build banner: shell choice, captions |
| 0x364f30 (GameModeClass vf78), 0x3deac0 | per-frame banner text and colour |
| 0x3df8b0 | fill graph_left/right |
| 0x347580 / 0x347850 | Begin / Auto-Resolve click |
| 0x3d26e0 | hide both pending shells |
| 0x455db0 | story event trigger: holocron add, flash, transmission text |
| 0x47e2c0 / 0x107280 / 0x107bb0 / 0x107900 | open / build / callback / close story summary |
| 0x47e030 / 0x104b00 / 0x4bfdc0 | IDD_STORY_DIALOG for random plots |
| 0x2b92e0 / 0x2b76a0 | space / land conflict driver incl. AR result text |
| 0x88050 | IDD_BATTLE_END_DIALOG open (also after auto-resolve) |
| 0x82e60 / 0x82fe0 | IDD_AUTO_RESOLVE_DIALOG open (MP "Always") |
| 0x3e83a0 / 0x3c5fc0 / 0x3ce970 | organiser visibility / enter / leave planet map |
| 0x2706c0 | font load incl. stretch |
| 0x47d9c0 / 0x47eac0 | story script load (cache by name) / parse; keyword table 0xa2a1d0 (§2.6) |
| 0x1089e0 / 0x1053c0 | story script executors: holocron summary / IDD_STORY_DIALOG (§2.6) |
| 0x347ae0 → 0x3d6c30 | `planet_info` click → command-bar callback +0x2898 with HUD +0x2150 (§9) |
| 0xd3c20 / 0xd3eb0 / 0xd3e00 / 0xd3dc0 | IDD_PLANET_INFO open / fill / callback / close (§9) |
| 0xd5d00 / 0xd6120 | planet-info list fill (wrap + colour) / planet model render to IDC_PLANET_ICON |

---

## 9. Planet Info dialog (`IDD_PLANET_INFO`) (added 2026-10-28)

### 9.1 What opens it [H]
- **Only the organiser button `planet_info`** ("Planet Info", `TEXT_BUTTON_PLANET_INFO`; the button is
  visible only in the planet close-up, §4). The click handler 0x347ae0 is in the component table at
  0xa17780, next to the name `planet_info`. It calls 0x3d6c30, which calls the command-bar callback stored at
  CB +0x2898 with the selected planet (HUD +0x2150), and does nothing when no planet is selected.
- The callback is 0xd3c20. The command-bar init (0x30ae50 called from 0x4b250, argument 30) installs it.
  0x3d6c30 is its only caller, and 0x347ae0 is the only caller of 0x3d6c30.
- No other path opens it. A right-click on a planet gives move / fleet orders. The planet hover encyclopedia
  (`encyclopedia_*` components, HUD_GAPS / UI.md §2) is a separate HUD panel. Nothing in the story or Lua
  code opens the dialog.
- **`IDD_PLANET_DIALOG` (435 × 384: political / starbase / ground-base icon rows, credits, company / fleet
  counts, production progress, Organization / Production / Cancel buttons) is dead.** No code references it
  or any of its control names (IDC_PLANET_MENU_PLANET_NAME, IDC_POLITICAL_ICON_n, IDC_STARBASE_ICON_n,
  IDC_TEXT_PLANET_CREDITS, …). It is a leftover in GUIDialogs.rc.
- `IDD_PLANET_QUICK_REFERENCE` is a different multi-planet table and is not opened from `planet_info`.

### 9.2 Layout and contents (GUIDialogs.rc 975 × 718, fill 0xd3eb0) [H unless marked]
Everything comes from the planet **type** (static XML). No live sim state appears.

| control | content |
|---|---|
| `IDC_STATIC_PLANET_NAME` | "`TEXT_BUTTON_PLANET_INFO` `<planet display name>`" ("Planet Info Kuat"), in the owner faction's colour (faction +0xb8..+0xc4), white when unowned |
| `IDC_PLANET_ICON` (435 × 435) | the planet's own model rendered into a 512 × 512 target (0xd6120), same camera / light set-up as the battle load screen [M for the exact camera] |
| `IDC_PLANET_OVERLAY` | `load_overlay.tga` drawn over the planet |
| static headers | `TEXT_TACTICAL_INFORMATION` "Tactical Information:", `TEXT_PLANETARY_ADVANTAGES` "Planetary Advantages:", `TEXT_HISTORICAL_REFERENCE` "Historical Reference:", `TEXT_AVG_WEATHER` "Weather:", `TEXT_TERRAIN_TYPE` "Terrain:", `TEXT_NATIVE_POPULATION` "Population:" |
| `IDC_LIST_TACTICAL` | `Describe_Tactical` text, word-wrapped into list rows (0xd5d00), colour (50, 200, 255), scrolled to the top |
| `IDC_LIST_ADVANTAGE` | `Describe_Advantage`, wrapped, default text colour [M: palette entry 1] |
| `IDC_LIST_REFERENCE` | `Describe_History`, wrapped, default text colour |
| `IDC_STATIC_NATIVE` | `Describe_Population` |
| `IDC_STATIC_TERRAIN` | `Describe_Terrain` |
| `IDC_STATIC_WEATHER` | `Describe_Weather` |
| `IDC_WEATHER_ICON` | `Encyclopedia_Weather_Icon` with its first 4 characters replaced by `load` (`i_pa_weather_rain.tga` → `load_weather_rain.tga`) |
| `IDC_NATIVE_ICON` | `Native_Icon_Name` as is (e.g. `load_swamp.tga`) |
| `IDC_MAP_ICON` | `Land_Tactical_Map` with `.ted` → `.dds`, under `Data\Art\Textures\` [M: exact prefix]. If the file is missing, the control is switched off (0x1c30d0(…, 0)); otherwise its image is set and it is switched on |
| `IDC_CLOSE_BUTTON` | "Close" |

The fill also writes `IDC_STATIC_FACTION` (owner faction name), `IDC_STATIC_CREDIT` ("%4.0f" planet credit
value from the planet behaviour, 0x3f8a60) and `IDC_STATIC_WILDLIFE` (`Describe_Wildlife`). **None of these
controls exists in the shipped IDD_PLANET_INFO**, so nothing is shown for them. Do not add them [H].
Data: FoC PLANETS.XML has 64 planets with all seven `Describe_*` fields; 50 have `Native_Icon_Name`, 61
`Land_Tactical_Map`.

### 9.3 Visibility gating [H]
**None.** 0xd3eb0 calls none of the REMAINING_GAPS §6.3 queries (0x50b2a0 … 0x50c420, 0x50bf10, 0x50c550).
Their callers are all elsewhere (galactic HUD: planet labels 0x3ea1b0, organiser 0x3e83a0, fleet tooltip
0x2e2910, credit tooltip 0x2f2ad0, corruption / hero lines 0x3ec380, 0x30f4e0, 0x30fd00, and others). The
dialog shows only static type text, so an enemy, fogged or stealth-flagged planet shows the same full
dialog as one's own. Only the name colour reveals the owner. No `???` or hidden fields. The income, fleets,
ground forces, starbase / ground-base level, political control and special structures that the task asks
about are **not in this dialog**. In the original they appear only on the HUD, where §6.3 gates them.

### 9.4 Pause and closing [H]
- **No pause.** Neither open nor close touches the galactic pause counter or the mode pause, so the galactic
  sim keeps running underneath. It is a normal dialog (created by 0x1c1570 with the same layer and flags as the load
  screens). Whether it blocks map clicks is the GUI manager's modal rule (UI.md §3) [M].
- **Single instance.** While it is open (flag 0x140a620c9), another `planet_info` click does nothing. The
  dialog keeps the first planet; it is not refreshed.
- On open it also switches off `IDC_DIALOG_CLOSE_BUTTON`, which is absent from this dialog (no effect).
- **Closes on:** a click on `IDC_CLOSE_BUTTON`; callback event 0 (the dialog's cancel / close event, [M]: Esc);
  and a battle load-screen activation (0x8cb50 / 0x8d930 call the close 0xd3dc0), so it never survives into
  a battle. Selecting another planet or leaving the close-up does not close it [M: no other caller of 0xd3dc0].

### 9.5 Port mapping
- **ui**: add an `IDD_PLANET_INFO` screen (e.g. `PlanetInfoInfo{name, name_rgba, planet_model/type,
  tactical, advantage, history, population, terrain, weather, weather_icon, native_icon, map_icon (optional)}`)
  with the three wrapped lists (tactical in 50/200/255), overlay, icon-name rewrite rules and Close / Esc.
  Retire `InfoScreens::show_planet` / `PlanetDialogInfo` (IDD_PLANET_DIALOG: never shown by the original),
  or keep it unused. Do not apply §6.3 gates in this dialog.
- **apps/game**: on `HudAction::PlanetInfo` with a selected planet, open it once (ignore while open), fill
  it from the planet type, and do **not** pause. Close it when a battle load screen starts.

> **Status 2026-10-30:** `IDD_PLANET_INFO` is `GameScreens::show_planet_info` (`engine/ui/include/alamo/ui/gui_screens.hpp:173-190`), opened once from `HudAction::PlanetInfo` without a pause (`engine/apps/game/app/game_main.cpp:1722-1724`) and closed when a battle load starts (`game_main.cpp:3492`); `IDD_PLANET_DIALOG` is marked dead (`gui_screens.hpp:18`).
