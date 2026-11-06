# TheGameScoringManagerClass: the GameScoring Lua script (q6 t06)

Question: how does retail FoC load and drive `Data\Scripts\Miscellaneous\GameScoring.lua`, which functions does it
call, and does a `GlobalValue.Set` done in its `Base_Definitions()` reach the story / plot scripts of a new campaign?
Immediate need: Thrawn's Revenge sets `GlobalValue.Set("MOD_ID", ModContentLoader.get_mod_id())` there, and its
galactic scripts (`AbstractResourceManager`, `GalacticStatDisplay`, `ModContentLoader.get_mod_specific_path`) read
`GlobalValue.Get("MOD_ID")`.

Answer in one line: the script is created at app start and **re-created (fresh Lua state, `Base_Definitions()` re-run)
every time a new game session starts**, before the campaign's story plots are loaded; `GlobalValue` is one
process-wide store shared with every other script, so the value is there when the plot scripts first run.

Source: Ghidra decompile of Steam FoC x64 1.121 (`re_reference/ghidra/StarWarsG_decompiled.zip`, `Global.c`), RVAs
relative to the image base (SUBSYSTEMS.md lists the same methods at mid-function addresses; the function starts are
the ones in `ghidra/renames.csv` and are used here). Script text: vanilla `GameScoring.lua` (FoC data zip) and Thrawn's
Revenge (workshop 1125571106). Confidence [H] unless marked.

## 1. The object and its script

- Singleton (0x4fd50, created on first use; vtable `TheGameScoringManagerClass`, base `SignalListenerClass`), 0x20
  bytes: +0x10 the current `LuaScriptClass*` (null = no script), +0x18 "initialized" byte, +0x19 "suppress player-list
  reset" byte.
- **Script name:** `GameConstants` tag `Game_Scoring_Script_Name` (GC +0x300, string; vanilla and TR both
  `GameScoring`). Empty name → no script ever; the manager only marks itself initialized.
- **Lookup:** by name through the normal script loader (pool get 0x244170 → load 0x245e30), the same as any script:
  `LUA_PATH` and the script roots of LUA_API.md §"Script search path"; `GameScoring` resolves to
  `Data\Scripts\Miscellaneous\GameScoring.lua` (mod folders first, as for every data file) [M for the root order].
- **VM:** every `LuaScriptClass` owns its **own `lua_State`** (ctor 0x2457f0: new state, standard libraries, `_ALERT`,
  global `Script` = the script object, `LUA_PATH`; then `Debug_Should_Issue_Event_Alert` and the `GlobalValue`
  global, 0x2567b0). So GameScoring shares no Lua globals or `require` cache with any other script; it shares only the
  engine-side stores (`GlobalValue`, the game objects).
- **Pooling:** the script declares `ScriptPoolCount = 0`. The pool get then creates a fresh instance, marks it "needs
  Base_Definitions" (+0x122 = 1), not poolable (+0x124 = 0), in use (+0x123 = 1), not exited (+0x121 = 0), and does
  not keep it in the pool. Releasing it (0x2488e0) therefore closes its `lua_State` (no `Flush_G` / pool return).
  With a missing `ScriptPoolCount` the default would be a pool of 1; irrelevant for the shipped files.

### 1.1 Attach_Lua_Script (0x4854e0; args: name, "from save" flag, "flush pool" flag)

Normal path (flag 2 = 0):
1. Get the script by name (0x244170, passing the flush flag). On failure the manager keeps no script and returns false.
2. Replace +0x10 with it (reference counted).
3. Register `GlobalValue` again (0x2567b0) and the game-command globals (0x546c70: `_ProduceObject`, `FindPlanet`,
   `FindTarget`, `BlockForever`, `_FindStageArea`, ... the PGCommands set).
4. If +0x122 (fresh): call the Lua function **`Base_Definitions()`** (by name, no args; missing → nothing).
5. Start the **`main`** thread (0x244740 with `"main"`): if the global `main` is a function, a coroutine for it is
   appended to the script's thread list; it does not run yet (first resume in §2). Missing `main` → no thread.

Note the loading of the file itself (step 1) executes the file's top-level chunk first, so the order inside the new
state is: top level (`require`s, `ScriptPoolCount`, function definitions) → `Base_Definitions()` → (later) `main()`.
Vanilla `Base_Definitions` runs `Common_Base_Definitions()` (sets `LastService = nil`, clears the PGBase tables),
`ServiceRate = 10`, `Reset_Stats()` (which calls `Reset_Tactical_Stats()` → `GlobalValue.Set(
"Allow_AI_Controlled_Fog_Reveal", 1)`), `Definitions()` if defined, `Define_Title_Faction_Table()`. TR's sets
`ServiceRate = 0.1` and `MOD_ID` (via `Find_Object_Type` of `rev` / `fotr` / `icw`, so it needs the object types,
which are loaded before GameScoring in the data-init order).

"From save" path (flag 2 ≠ 0; no caller passes it in 1.121 as far as traced [M]): plain new script by name (0x2440f0)
with GlobalValue + commands registered, **no** `Base_Definitions`, no `main` thread.

### 1.2 When the script is (re)created

| moment | function | what happens |
|---|---|---|
| app start, data init (after AI data, before WeatherModifiers; MAIN_LOOP §1) | System_Initialize 0x4889d0 | Attach(name, 0, 0); register the script file with a file-watcher singleton (0x475140, debug reload [L]); listen to the GameModeManager's signals **0x23 (mode starting)** and **0x24 (mode ending)**; +0x18 = 1 |
| **every new game session**: `Start_Game` with an empty mode stack (new campaign, skirmish, attract map, MP) | Start_Game 0x28e890 step 2 → default players 0x293390 → PlayerList reset 0x297630(list, 0) → **Player_List_Reset 0x4868e0** | unless +0x19 is set: release the current script (threads killed, state closed), then **Attach(name, 0, flush = 1)**: a fresh state, `Base_Definitions()` re-run, a new `main` thread |
| other PlayerList resets with flag 0 (teardown paths, e.g. demo attract 0x22f10, the front-end callers of 0x297630) | same | same re-creation |
| a mode-side player-list rebuild (0x35f970) | sets +0x19 = 1 around its own 0x293390 / 0x297630 calls, then 0 | the script survives that rebuild |
| changed script files (debug file watcher, 0x475260) | Reload_Script 0x4876e0(list of names) | if any name equals the script's name (case-insensitive): release, Attach(name, 0, 1), re-register the watcher [L for the trigger] |
| load of a saved game | Load 0x486690 (§6) | release, Attach by the saved name, then restore the saved Lua state |

The engine never calls the script's own Lua function `Player_List_Reset()` (no string reference in the exe): the
re-creation replaces it. A re-creation always re-runs the whole file and `Base_Definitions()`, so anything a mod does
there (TR: `MOD_ID`, its plugin runner reset) happens again for each session.

### 1.3 Order inside a new galactic campaign (0x28dbe0)

1. (When asked to) shut the Lua system registries down / reset (0x248c60, 0x248be0) [L: what they clear; not
   GlobalValue].
2. `Start_Game(galactic, campaign)`: empty stack → … → default players → **GameScoring re-created,
   `Base_Definitions()` runs, `MOD_ID` set** → galactic mode allocated, init (+0x08), made current; campaign name copied
   to mode +0x348.
3. Players set up from the campaign; the per-faction story files: **`TheStoryMode::Load_Plots(<file>)`** for each
   (0x329aa0: plot scripts loaded, their `Base_Definitions` run and `main` threads created, STORY.md §2.1).
4. Campaign flags; then the mode's vf +0x20 (0x35cc10): tactical-only inits, per-player story hook (0x4d9cd0), then the
   **mode-starting signal 0x23** → `Game_Mode_Starting_Event("Galactic", <campaign name>)` (§3).
5. Ticks start; plot `main()` threads first resume at T9, GameScoring's `main` at its first Service (§2).

So for a new campaign `MOD_ID` is written before any plot script is even loaded [H for the call order]. Skirmish
(Start_Game(1/2) from Load_Map) gets the same re-creation before its map scripts.

## 2. Service (0x488660), once per frame

Called by `GameModeManager::Update` (0x28d400) before the mode tick, only while a mode is current and no out-of-sync
condition is flagged (MAIN_LOOP §3.1). Nothing when +0x10 is null.

1. Read the script globals `ServiceRate` and `LastService` (numbers; anything else counts as absent).
2. `ServiceRate` absent → nothing at all (no threads ever run).
3. now = (float)global frame ÷ (float)frame-rate field of the FrameSynchronizer (game seconds; 30 fps) [M for the
   field].
4. Pump when `LastService` is absent, or when (now − LastService) **>** ServiceRate (strict, compared in double).
   Pump = resume every live thread of the script once, in creation order (0x247a90, the common Lua thread service;
   a thread that yields `true` stays, a return / `false` / error kills it).
5. After the pump: if the script's exited flag (+0x121) is set, **the manager releases the script** (+0x10 = null): no
   more events or services until the next re-creation (§1.2). Otherwise `LastService` := now (a new number global).

The pump is skipped and step 5 releases the script when +0x121 was already set before the pump. +0x121 is set by
`ScriptExit()`, by a release, and by the Lua error path (LUA_SCRIPT_OUTPUT §1, [M]), so **one Lua error in any
GameScoring function (event or thread) ends the script at the next Service** [M].

What `main` does: vanilla `main` loops `GameService(); PumpEvents()` (PumpEvents yields `true` once), so with
`ServiceRate = 10` `GameService` runs at the first Service after the (re)creation (LastService is nil) and then on the
first Service more than 10 s later, and so on. Vanilla `GameService` only prints stats (output goes nowhere in retail).
TR's uses 0.1 s, i.e. about every 3 to 4 frames depending on the float rounding of `now` [M], and also calls `GameScoringPluginRunner:
update()`.

**Conflict with the TSW probe (2026-10-31, LUA_SCRIPT_OUTPUT §2):** the probe saw `Base_Definitions` and
`Game_Mode_Starting_Event` but never its wrapped `GameService`. The decompile shows no gate that would stop `main`
(it is the same thread machinery story plots use), and TR ships `always`-target plugins (`bootana-hutta`) that only run
from that `main` loop, which suggests it runs in retail. The likely cause is the error rule above: an error in the
probe hook or in an event function before the first Service releases the script, so `main` is never resumed [M]. A
retail check is still open (§8).

## 3. Calls into the script

All calls are by global name with the argument list shown; a missing function is skipped silently. They run
synchronously on the script's main state (not in a coroutine, so they cannot yield / `Sleep`), at the moment the engine
raises the signal, i.e. inside the frame's logic. Wrappers: GameObjectWrapper 0x5751d0, PlayerWrapper 0x6019f0,
GameObjectTypeWrapper 0x6051b0.

| Lua function (args) | trigger |
|---|---|
| `Base_Definitions()` | fresh script at attach (§1.1) |
| `Game_Mode_Starting_Event(mode_name, map_name)` | GMM signal 0x23, raised by the mode's vf +0x20 (0x35cc10) once a mode is set up: new campaign (§1.3), tactical battle after its map is loaded, skirmish / MP / attract starts (other vf +0x20 callers 0xbd380, 0xc9e10, 0xeed80 [L roles]). `mode_name` = mode type as text (`"Galactic"`, `"Land"`, `"Space"` [M strings]); `map_name` = galactic: the campaign name (mode +0x348); tactical: the map name (mode +0x70). Not raised again when galactic resumes after a battle [M] |
| `Game_Mode_Ending_Event(mode_name)` | GMM signal 0x24 in End_Game_Mode (pop, 0x28f320) before the mode's shutdown, for every mode popped (also by End_All) |
| `Tactical_Unit_Destroyed_Event(object, killer_player)` | death signal 0x25 of a listened object while the object's mode is tactical; `killer_player` = player at object +0x330, or the neutral player when it is −1 (0x294aa0: first player whose faction has the +0x108 flag [M]). Objects whose type has category / behaviour index 0x2e in either of the first two slots (0x374b50) are skipped [L meaning] |
| `Galactic_Unit_Destroyed_Event(object, killer_player)` | the same signal while the mode is galactic |
| `Tactical_Production_End_Event(object_type, player, location)` | signal 0x22 (tactical build finished) on a player or a build-capable object; `location` = the object with the signal's location ID (current mode's store, 0x2aca60), nil when the ID is 0x3fffff; `player` from the signal's player index |
| `Galactic_Production_Begin_Event(planet, object_type)` / `..._Canceled_Event(planet, object_type)` | planet signals 2 / 3 |
| `Galactic_Production_End_Event(planet, object)` | planet signal 4; `object` = the created object (its team container when it is a team member), or the object type when no object was created. The manager starts listening to the new object |
| `Galactic_Planet_Faction_Change(planet, new_player, old_player)` | planet signal 0xE (owner change data), only when new ≠ old; a missing player index → the neutral player |
| `Galactic_Starbase_Level_Change(planet, old_type, new_type)` | planet signal 0x26; each type nil when absent [M which side is which] |
| `Galactic_Neutralized_Event(hero_type, killer_object)` | object signal 0x27 (hero neutralized), and directly from the auto-resolve path 0x616740 (0x4879b0) |
| `Player_Quit_Event(player)` | 0x488190, from `QuitGameEventClass` vf4 (0x499420, the quit-game event) and its neighbour 0x4994c0 [M roles]; nothing when the player is null |
| `Reset_Tactical_Stats()` | AutoResolve vf4 Begin (0x60c9e0), BATTLE_TRANSITIONS §"auto-resolve" |
| `Get_Game_Stat_For_Control_ID(player, control_id, for_tactical)` → value | battle-end dialog fill 0x88fe0 and the campaign-end path 0x92bb0 (UI.md, MAIN_LOOP T11) |
| `Get_Neutralized_Count_For_Type(object_type, player)` → number | `NeutralizeHeroAbilityClass` (0x6171e0) |

Listening (what makes the signals arrive): on 0x23 the manager subscribes to the new mode's object store "created"
signal 0x1C and to signal 0x22 on every player, then walks the mode's objects: galactic → every planet (signals 0xE, 2,
3, 4, 0x26, plus the units at it); tactical → every object except those with behaviour 8 or 0xC or type flag +0x3b.
Each listened object gets 0x1A, 0x25, 0x27 (and 0x22 with behaviour 0x29). New objects (0x1C) are added the same way
(log "GameScoring -- Listening to new Object"); in galactic a non-member object is added only when its type passes the
test 0x370320 for one of the ids 0x5e–0x61 (0x486490) [L meaning]. On 0x24 it drops the store listener; when the ending mode is galactic it drops
**all** its listeners and re-subscribes to GMM 0x23 / 0x24 only.

## 4. GlobalValue: one process-wide store

- `GlobalValue` is an engine global object registered into every script state (L1_S4_REMAINING §1.5): `Get(name)`,
  `Set(name, value)`; userdata values refused. Its data is **one map in the exe** (0x140b0a060, string → value),
  shared by GameScoring, story plots, AI plans, object scripts and every mode. A `Set` in GameScoring is visible to a
  plot script's next `Get` at once.
- **Cleared** (0x2566d0) when the mode stack becomes empty (pop of the last mode, 0x28f320) and in End_All (0x28d940:
  quit to menu, load game, player left). **Not** cleared between a galactic mode and its tactical battles.
- **Saved / loaded** with the game: GMM save chunk 0x12 (0x2566e0); load (0x2563a0) clears the map and then reads the
  saved entries.
- Consequence for `MOD_ID`: quit-to-menu clears it, the next `Start_Game` re-creates GameScoring and its
  `Base_Definitions` writes it again before the plots load. After a save load the value comes from the save (chunk 0x12
  is read after the scoring chunk, whose re-attach also writes it).

## 5. Effects on the simulation (single player)

- Vanilla: none. The script only keeps Lua tables, prints, and writes `GlobalValue` `Allow_AI_Controlled_Fog_Reveal`
  = 1 on every stats reset. No reader in the exe, but the vanilla AI scripts read it:
  `Data\Scripts\AI\SpaceMode\BurnUnits.lua` and `LandMode\BurnUnitsLand.lua` (line 69) test
  `GlobalValue.Get("Allow_AI_Controlled_Fog_Reveal") == 1`, so running the script changes tactical AI behaviour
  (checked 2026-11-03 against the FoC data text). No `GameRandom` use.
- Mods can act on the game from it: TR's event functions and `main` loop despawn / spawn objects, change GUI
  components, publish `crossplot` messages and call `GameRandom.Free_Random` (the unsynced stream). These run at the
  signal moments in §3 and at the Service of §2, before the mode tick.
- `Get_CRC` (0x485700): when a script exists, a hash of the script's `lua_State` (0x243630 → 0x7d22d0) seeded with the
  frame CRC; used only by Calculate_Frame_CRC when frame & 0x31 = 0 or sync-debug is on, and that CRC is computed only
  when the game is synced (MP, or a mode record flag; COMMANDS §7.1). Single player: not part of any check; the port
  needs nothing (MP is out of scope).

## 6. Save and Load (0x487870 / 0x486690), GMM chunk 3

Inside the GameModeManager save (after the story chunk 2, before chunk 4):
- chunk 2: micro 3 = byte +0x18 (initialized); micro 4 = the manager's own pointer (for pointer remapping on load).
- if a script exists: chunk 0 = the script's file name string; chunk 1 = the script's full Lua state (LuaScriptClass
  save 0x247e50: its globals and threads).

Load: chunk 0 → remember the name; chunk 1 → release the current script, Attach(name) (normal path: fresh state,
`Base_Definitions()`, a new `main` thread [M for the flag values the decompile drops]), then restore the saved Lua state
into it (0x246310); chunk 2 → +0x18 and the pointer remap entry.

What the saved state holds in vanilla: the stats tables (kill / build / conquest / neutralized / player tables,
`CampaignGame`, `GameStartTime`, `LastService`), i.e. only what the end-of-game statistics screens show.

## 7. UI readers (low priority)

- `Get_Game_Stat_For_Control_ID` (0x485760): calls the Lua function with (PlayerWrapper, control-id string, bool) and
  returns its first result; used to fill the statistics dialog fields.
- `Get_Tactical_Killed_List` (0x485b50): reads the Lua global table `TacticalKillStatsTable` in tactical modes or
  `TacticalTeamKillStatsTable` in galactic, and returns a static list of (object type, count) entries from it (battle
  end dialog fill 0x88fe0) [M for the exact entries].
- `Get_Neutralized_Count_For_Type` (0x4859a0): calls `Get_Neutralized_Count_For_Type(type, player)`; needs a valid
  player index and a script.

## 8. Open

1. Retail check of §2: does GameScoring's `main` / `GameService` run in an unmodified install, and does an error in an
   event function stop all later events? A probe can wrap `GameService` and `Tactical_Unit_Destroyed_Event` with
   `pcall`-safe counters only (no other change), then read them after a skirmish.
2. The meaning of the 0x2e type check in the death handler and the exact role of the `Player_Quit_Event` callers.

## Port notes

Minimum for the `MOD_ID` case:
1. Read `Game_Scoring_Script_Name` from GameConstants; resolve it with the normal script lookup (mod layers first).
2. At app start and **at every new-session `Start_Game`** (before the galactic mode is created and before
   `Load_Plots`), drop the old GameScoring instance and create a fresh, unpooled script state with the usual globals
   (`Script`, `GlobalValue`, the PGCommands set), run the file, then `Base_Definitions()`.
3. Use the existing process-wide `GlobalValue` store (cleared at quit-to-menu / when the stack empties, saved and
   loaded with the game), so the plot scripts see `MOD_ID`.

Next, for stats screens and mods like TR:
4. Event calls of §3 at their signal moments (mode starting / ending first, then unit destroyed, production, faction
   change, starbase level, neutralized, player quit, `Reset_Tactical_Stats` on auto-resolve).
5. The `main` thread and the `ServiceRate` / `LastService` Service of §2, run before the mode tick; release the script
   when it has exited or errored. Mods may act on the sim from here, so keep it at that fixed point in the frame and
   keep `GameRandom.Free_Random` on its own stream.
6. Save: the port's own format only needs the GlobalValue store (already saved) and, if the stats screens are wanted
   across a load, the script's tables; otherwise re-create the script on load and accept empty stats.

Can wait / skip: Get_CRC (MP only), the debug file-watcher reload, the `from save` attach path.

## Port status (2026-11-03, q6 t06)

- Done (port notes 1-3): `SimHost::set_scoring_script(files, name)` (`engine/apps/sim/include/alamo/sim/sim_host.hpp`);
  `start_game` on an empty stack re-creates the VM after the GlobalValue clear and before `start_galactic` sets the
  players up, so before `start_story`; top level, `Base_Definitions()` (skipped when missing), `main` thread created.
  The VM is dropped when the stack empties [port]. `GameSession` always enables it; `alamo_headless` with `--scoring`.
  The session GlobalValue table is the store (saved with the game; a load restores it after the re-creation, as §4).
- Not yet: the Service pump of §2 (so `main` never resumes), the event calls of §3, the UI readers of §7, saving the
  script's own tables (§6: stats are empty after a load).
