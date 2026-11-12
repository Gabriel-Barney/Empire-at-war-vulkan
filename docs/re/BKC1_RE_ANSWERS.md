# BKC1 RE answers: Story_Handle_Esc, the PlayerList, galactic end timing, galactic bounty

Behaviour read from StarWarsG.exe (FoC Steam x64, Ghidra export). Prose only, no decompiled code. Confidence:
**[H]** read in code, **[M]** inferred with partial code, **[L]** guess. RVAs are image-relative. Sources: keyboard
WndProc 0x60c60 and key-down dispatch 0x61fe0; cinematic / targeting controller 0x140b27a10 (key-down 0x2cbd00, key-up
0x2cc080, Start_Cinematic_Camera 0x544a10 → 0x44cd80 → 0x2ccd80); Story_Handle_Esc caller 0x328440; Lua call-by-name
0x2437c0; options dialog 0xa2fe0; in-game Bink stop 0x44f130; `Create_Default_Players` 0x293390,
player ctor 0x291a50, `Start_Game` 0x28e890; victory setup 0x289a90, derived-owner change 0x3fb040, `Change_Owner`
0x3f7280, evaluator 0x344710, declare 0x343b90, countdown 0x344670, common tick 0x3639d0, galactic tick 0x4b2ef0, main
loop 0x5d990 tick tail, campaign end 0x92bb0; bounty 0x392600. Shipped data: `FACTIONFILES.XML`, `FACTIONS.XML`,
`EXPANSION_FACTIONS.XML`, `STORY_UNDERWORLD_ACTI_M01_LAND.LUA`. Roadmap: b0023 (Q1), b0024 (Q2), b0013 (Q3), b0127 (Q4).

## Q1 (b0023). How the engine calls `Story_Handle_Esc`

**Path.** It is a direct call from the window message pump, not a command [H]. WM_KEYDOWN / WM_SYSKEYDOWN (auto-repeat
downs dropped unless a dialog has focus) → 0x61fe0(vk). Key-up (0x2cc080) never calls it. In 0x61fe0, for vk 0x1B (Esc):
1. A full-screen engine movie playing (0x140a61a98): any key ends it, nothing else [H].
2. An in-game Bink playing (handle 0x140b30690 set, playing byte 0x140b3069b, main menu not open): **every key is
   consumed**. Esc or Space stops the movie (0x44f130) unless the movie's no-skip byte (+0xd5 of 0x140b306d0) is set.
   `Story_Handle_Esc` is **not** called. Lua `Play_Bink_Movie` 0x543170 plays through this same player (0x44d920 on
   0x140b306dd) [H code, M that the handle is the same object]. So in UM01 an Esc during the two intro Binks only skips
   the current movie, and `BlockOnCommand` returns.
3. Debug console, two overlays, then (top dialog is not the main menu) GUI dialogs, then the overlay 0x140a28c10.
4. **Path A, the controller 0x2cbd00** [H]. It returns at once when a GUI dialog has focus (0x1c3c40), and consumes every
   key when its byte +0x348 is set. When the current mode exists and is **tactical** (mode vf +0xF0, land or space), the
   controller state +0x37C is **4**, the key is Esc and the mode's story manager (mode +0xA0) exists:
   it calls 0x328440(mode +0xA0) and **consumes** the key. Nothing else follows: no panel close, no pause menu.
5. If the key was not consumed: Esc in a land / space mode closes the in-game panel (mode vf +0x1E8 → 0x2ce9c0, does not
   consume); world keys 0x33460; hotkeys 0x63f00.
6. **Path B, the Esc tail of 0x61fe0** [H]. Main menu open (0x140a61fc8) → exit flag 0x140a619ef. Otherwise, when a mode
   exists, the controller is active (+0x34A) and its state is 4 (0x140b27d8c), it calls 0x328440(mode +0xA0) **without a
   mode-type test** (in practice galactic, or tactical while a focused dialog let Esc through). Then: a playing in-game
   Bink is stopped (dead in practice, step 2 already took it [M]); no mode → stop; a GUI dialog open (0x140a785a8 ≠ 0) →
   stop; **controller inactive (+0x34A = 0) → open `IDD_GAME_OPTIONS_DIALOG` (0xa2fe0), the pause menu**; controller
   active → stop, no pause menu.

**What decides cinematic vs pause menu** [H]. Controller 0x140b27a10 byte +0x34A (active) and int +0x37C (state). Lua
`Start_Cinematic_Camera` (0x544a10, optional bool, default true) → 0x44cd80 → 0x2ccd80, which on success sets +0x34B,
+0x34A = 1, **+0x37C = 4** (with the bool also 0x28e800(GMM, 1)). `End_Cinematic_Camera` (0x542000 → 0x44b120) and
`End_Cinematic_Mode` (0x542020 → 0x2cbb80) end it; `Start_Cinematic_Mode` (0x544ac0) calls 0x2cca80(controller, 0).
`Letter_Box_*`, `Lock_Controls`, `Suspend_AI` and `Fade_Screen_*` play no part. So:
- cinematic camera on (state 4): Esc → `Story_Handle_Esc`, no pause menu;
- controller active in another state (tactical): state 1 swallows the key; states 2 / 3 (controller camera / targeting
  modes [M names]) end the controller through 0x2cbb80 and consume Esc; no pause menu;
- controller inactive: pause menu (if no dialog is open).

**0x328440(story manager)** [H]. Walks the manager's plot table (+0x68, linked nodes, value at node +0x18; the same
table order STORY.md §0 uses for dispatch). For each plot with the active byte +0x660 set and a Lua script (+0x610):
0x2437c0(script, "Story_Handle_Esc", no arguments) looks the global up in that plot's own `lua_State`
(`getglobal`); **only if it is a function** it is called with **no arguments**; the result table is released unread.
A plot without the function is skipped silently. No player test (+0x638 is not read), no GameObject scripts, no AI or
mission script other than story plots. Every active plot of the **current** mode's manager is offered the call.

**Timing** [H order, M frame stamp]. The message pump (main loop step 3) runs before the iteration's tick (step 19), so
the Lua function runs between tick N and tick N+1, outside any tick. Its direct effects (`Thread.Kill`,
`Stop_Bink_Movie`, flags) happen then; threads it creates (`Create_Thread("IntroCineCleanup")`) first run at the plot's
next service, inside tick N+1. The Kessel trace (STORY_T04 §1: Esc → spawns and removals stamped 129) fits this.

**Port rule**
- Esc down (not a repeat, no focused dialog, no Bink / full-screen movie playing) in a mode with the cinematic camera in
  state 4: for every active plot of the current mode's story manager, in plot-table order, call the plot's global
  `Story_Handle_Esc()` if it is a Lua function; no arguments, results ignored; consume the key.
- Without the cinematic camera: Esc opens the pause menu when the controller is inactive and no dialog is open.
- During a Lua Bink: Esc / Space only stop the movie (unless it is no-skip); no `Story_Handle_Esc`.
- Apply it between ticks: the port should queue it as a local input that executes before tick N+1 (the single-player
  scheduled-command point, main loop step 10, is the same place) and record it in replays [port: retail does not record
  it; the port needs it for replay / headless parity].

**Port consequences**
- Add `Story_Handle_Esc` dispatch to the story session (all active plots, `pcall`-style: a missing function is no error).
- Headless / parity runs: an `--esc-at <frame>` style injection so the UM01 Esc run (20261108-234224) can be replayed.
- Tie it to the port's cinematic-camera state (`Start_Cinematic_Camera` → state 4), not to letterbox or `Lock_Controls`.

## Q2 (b0024). How retail builds the PlayerList; the extra UM01 player

**Builder** `Create_Default_Players` 0x293390 [H]. Called by `Start_Game` 0x28e890 **only when the mode stack is empty**
(GMM +0x50 = 0); with a mode already up, the new mode just gets vf +0x50. A tactical battle linked from a campaign is a
pushed mode, so **it keeps the campaign's PlayerList**: same players, same indices. Steps:
1. Reset the list (0x297630).
2. Only when a required-player list exists (0x140b155e8: skirmish / MP setup) and the flag argument is 0: bubble-sort the
   entries (by +0x94 when game type 0x140b15400 = 1, else by +0xA0), create the local entry first at its slot index,
   then every other entry at its slot index; mark each entry's faction as used.
3. **For every faction of the faction table (0x140a172e0, count 0x140a172e8) not used yet, in table order: one player**,
   index = running count, no faction filter at all (no `Is_Playable`, `Is_Neutral` or AI test), not human (+0x62 = 0)
   [H]. Player fields from the ctor 0x291a50: +0x4C player id = the index, +0x50 = faction index + 10 (−1 when
   0x140b15490 is set) [H], +0x62 human flag.
4. 0x293130, 0x294360 (post steps, not read).

A story campaign has no required list, so **player index = faction table index** [H code, M that the campaign path
leaves 0x140b155e8 null: the trace's indices prove it]. The faction table is `FACTIONFILES.XML` order then document order
[M: matches every index in the trace]:

| index | faction | file | Is_Playable | Is_Neutral | UM01 retail trace |
|---|---|---|---|---|---|
| 0 | Rebel | FACTIONS.XML | true | No | AI |
| 1 | Empire | FACTIONS.XML | true | No | AI, owner 1 |
| 2 | Pirates | FACTIONS.XML | False | No | AI |
| 3 | Neutral | FACTIONS.XML | false | Yes | no AI, owner 3 |
| 4 | Hostile | FACTIONS.XML | false | No | no AI, owner 4 (2 UMP_INTERFACE_NODE) |
| **5** | **Sarlacc** | FACTIONS.XML | false | No | no AI (**the port's missing player**) |
| 6 | Underworld | EXPANSION_FACTIONS.XML | true | No | human |
| 7 | Hutts | EXPANSION_FACTIONS.XML | False | No | AI |

The port (`engine/galactic/src/campaign_setup.cpp`) creates players only for factions the campaign names, plus Neutral
and the native owner (Hostile). It skips Sarlacc, so Underworld lands at 5 and Hutts at 6.

**Port rule**
- New session with no required-player list (every campaign, story or sandbox): one player per faction of the merged
  faction table, in table order, index = faction index; then mark human / AI / credits / tech from the campaign.
- With a required list (skirmish / MP): the sorted required players first at their slot indices, then the remaining
  factions in table order.
- Tactical battles started from a campaign reuse the campaign's list; never rebuild it per battle.

**Port consequences**
- Add Sarlacc (and any other unnamed faction, e.g. mod factions) as a player in campaigns; UM01 then has 8 players with
  the human at 6, matching retail owner ids in traces and goldens (regenerate the affected goldens).
- Everything that walks "every player in index order" now sees 8 players: the galactic AI quota cap (A2M_R2_RE_A §2,
  already assumes 8), victory tests in 0x3fb040, story TECH_LEVEL dispatch, CRC terms. Extra players own nothing, have no
  AI, no credits and no victory records unless the campaign names them.

## Q3 (b0013). Galactic end timing after an owner change

**What the code gives** [H]:
- Every galactic victory record has **countdown 30 frames** (0x289a90 writes 0x1E for every listed type and for the
  implicit type 1 that AI players get). There is no other countdown or delay constant on this path, and no
  "defeat delay" GameConstant (no such exe string).
- 0x3fb040 tests, at the owner change, every player in index order (types 0, 1, 4, 5, 9) and stops at the first pass;
  the pass declares at once (0x343b90: countdown := 30; no win / lose text for galactic types). The planet's derived
  owner (+0x6C) is switched before the tests, also when the new owner is Neutral; Neutral takes no different branch
  (only the "planet lost" speech, faction +0x790, needs a playable new owner).
- The countdown runs in the common tick 0x3639d0, which the galactic tick 0x4b2ef0 calls every tick: countdown −=
  frames since the last call (skipped while 0x140b15490 is set).
- The end screen opens in the main loop right after the tick (0x5d990, "T11"): galactic top mode, monitor ready
  (hold +0x5C ≤ 0, declared, countdown < 1) and, in single player, **no message box (0x140a61e60) and no in-game Bink
  (0x140b30690)** → 0x92bb0, which builds `IDD_CAMPAIGN_SCORE_DIALOG` at once.
- Monitor byte +0x79, when set, keeps the countdown at 1 while callback 0x140b30720 says so (retry-dialog support);
  no writer on the galactic path was found [M].

So D (planet to the Rebel AI while the human owns nothing) should end 30 frames after the change, which fits the trace
(≤ 1.0 s).

**What differs in E** [H code, L effect]. `Change_Owner` 0x3f7280, **when the planet's old owner is the local player**,
first moves the local player's forces off the planet: 0x2ba6c0 gives every local, non-dying object in the planet's orbit
list a move order to a neighbour (0x39f380), and 0x2ba850 retreats surface units into a fleet (log
"Retreating %s from surface."). Only then does it call 0x3fb040. In E the human's Star Destroyer is at Tatooine, so it is
sent away; in D the human has nothing there. No code was found that makes this move delay the declare, the countdown or
the end check, and type 1 does not count fleets. So the ~4 s (≈ 100-130 frames) is **not explained** [L]. Candidates
not excluded: a hold raised during the retreat, or a Bink / message box that holds the main-loop end check.
B ("within 5 s", 5 s heartbeats) does not tell the two cases apart.

**Port rule**
- Both D and E: the end screen opens after the tick in which the 30-frame countdown from the declaring owner change
  reaches < 1, i.e. after tick F + 30 for a declare in tick F, provided no hold, message box or in-game Bink is up
  [H for D; M for E].
- `Change_Owner` away from the local player first sends the local player's orbiting objects to a neighbour and retreats
  its surface units, before the owner change and the victory tests [H].

**Port consequences**
- No timing change needed for D. Keep E on the 30-frame rule and file the extra 4 s as a probe: rerun E (1 s heartbeats)
  (a) without the Star Destroyer and (b) with it while logging its position each second. Also log `Get_Game_Mode` /
  open dialogs per heartbeat. If (a) ends in ≤ 1 s, the local-forces retreat is what delays the end.
- Check that the port's `Change_Owner` does the local-player retreat (0x2ba6c0 / 0x2ba850) before the owner change.

## Q4 (b0127). Bounty modifiers in the galactic branch of 0x392600

- **No modifier is applied in the galactic branch** [H]. With the mode not tactical (vf +0xF0 false) the amount is
  n = round(B) (0x22f9c0, half away from zero, no `max(1, …)` floor), paid to the killer K only, through Add_Credits
  (so an AI killer's `Credit_Multiplier` applies inside Add_Credits [M: arguments not visible]) and added to stat +0x454.
  Not read anywhere in this branch: `NalHutta_Bounty` / the 0.15 planetary advantage, `UL_Bounty_Upgrade` 1.2,
  `BountyAbilityDef`, `Bounty_On_Faction_Ability`, `Bounty_Percentage_Amount`, `Stacking_Category`,
  `Apply_To_All_Allies`. A2M_R2_RE_B §2.3 is confirmed.
- **Case B (NalHutta)** [H]: when the killer's faction lacks `Benefits_From_Corruption` and the victim's faction has it,
  the full n is paid if K owns any instance of type `NalHutta`, else of `NalHutta_Story`, in the current mode's object
  store (planet object owner +0x58 = K). Hard-coded names, full bounty, no planet test.
- **The percentages exist only in the tactical branch** [H]: for each player p in index order allied with K (0x2823c0,
  K included), f = the per-player bounty object's value for the victim faction (max per stacking category, summed,
  optional min / max), n = max(1, round(f × B)). This is where `Apply_To_All_Allies` takes effect.
- **"The battle-planet test at 0x7162"** [M]. 0x392600 has no address 0x7162; the only planet test in it is Case A's
  parent walk at about 0x392760-0x3927BD (0x392600 + ~0x162, so "0x7162" is probably a typo for that offset). From the
  dying object it follows the parent chain (+0x2B0) to the **first object with a planet index (+0x335 ≠ 0xFF)**: ship →
  fleet → planet, company → transport / fleet → planet. That object is the planet that counts: no planet data
  (+0xB8) → nothing; corruption type (data +0x2F4) ≠ 3 (Racketeering) → nothing; no planet in the chain → nothing [H].
  It is the dying object's own location, not a "battle planet" read from the conflict; in an auto-resolve the two are
  the same planet [M].

**Port rule**
- Galactic (auto-resolve) bounty: per dying object, n = round(B); Case A pays K when K's faction has
  `Benefits_From_Corruption` and the object's own planet (parent chain) has corruption type 3; Case B pays K when the
  victim's faction has it and K owns a `NalHutta` / `NalHutta_Story` object; nothing else, no percentages, K only.
- Tactical bounty: the percentage map per allied player, n = max(1, round(f × B)).

**Port consequences**
- b0127 needs no galactic change for the listed modifiers: they are tactical-only (keep them parsed, apply them in the
  tactical bounty only).
- Use round half away from zero without a floor of 1 in galactic (an amount below 0.5 pays 0).
