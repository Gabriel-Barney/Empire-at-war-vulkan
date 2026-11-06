# A2i t01: galactic campaign start, story spawns and the AI's first service (StarWarsG.exe FoC x64, RVAs; 2026-11-06)

Question: why the retail Empire's budget Init (HotM, `Sandbox_Heart_Of_The_Maw_Rebel`) already counts the Accuser
(`Spawn_Piett`) in `MaintenanceUnnormalized` (0.62778, Infrastructure weight 10.7351), while the port's Init at its first T7
sees no Accuser (10.7329) because the story spawns it at the first T9 (A2H_T03_INFRA_WEIGHT.md, last section).

Tags as in the other `docs/re` pages: [H] read from code, [M] one inferred link, [L] unverified. No decompiled code is
reproduced. "Port" = `engine/apps/sim` (`sim_host.cpp`, `story_session.cpp`), `engine/apps/headless/ai_run.cpp` and
`engine/apps/game/src/game_session.cpp` at `cd13efb`.

## 1. Answer

1. **The story fires `Spawn_Piett` during the campaign load, before the first tick.** The starting tech level is applied
   by `Player::Set_Tech_Level` 0x288980, which raises STORY_TECH_LEVEL **synchronously** (0x32e8c0 -> dispatch 0x32e5f0).
   `TheStoryMode::Load_Plots` 0x28dbe0 loads the plots first and sets the tech levels afterwards (0x292590). So
   `Empire_Enters_Tech_01..03` fire inside the load, their STORY_TRIGGER dependants fire in the same call (STORY.md §1.2),
   and the UNIQUE_UNIT reward 0x4626a0 creates the Accuser at the Empire's first planet. All of this happens while the
   load screen is up and no tick runs. [H for the call chain, M for the gate, §3.2]
2. **The AI's first service and budget Init are in T7 of the first tick.** The chain is PlayerList update 0x297e30 ->
   0x287e70 -> AIPlayer 0x4b0ca0 -> the current mode's context service 0x5f5ab0 (first service while +0x69 == 0) ->
   Template init 0x6bbed0 -> budget Init 0x6137b0 (goal system +0x190). No other caller of 0x6bbed0 or 0x6137b0 exists.
   [H, M that context vtable +0x18 is 0x5f5ab0]
3. **The per-tick order is the one the port already has**: T5 mode tick (economy inside), T7 players / AI, T8 global Lua,
   T9 story. The plot Lua scripts first run in T9 of tick 1, after the AI's Init. `Universal_Story_Start` (STORY_ELAPSED
   0, prereq-less) only records its start time at that first T9 and fires at T9 of **tick 2** (0x454980 returns after
   the first observation). Only the start-of-game TECH_LEVEL (and CONSTRUCT, §3.3) notifications come earlier, during
   load. [H]
4. **Port change:** keep the T5 -> T7 -> T9 tick order. Add a load-time story pass at the end of story start, before
   tick 1: for each player in player order, dispatch STORY_TECH_LEVEL with that player's starting tech level to the
   plot sets owned by that player, and apply the rewards at once. Do not move T9 before T7 (§4).

## 2. Campaign start (load), in order [H unless tagged]

Started from the campaign dialog (0xc5250 / the handler that calls 0x28dbe0). The work runs on the load thread while
FS +0x34 = 1, so no tick runs (MAIN_LOOP §2.5, §5.2).

| # | Step | Address | Notes |
|---|---|---|---|
| L1 | GMM +0x80 = 1; Lua reset when reloading (0x248c60, 0x248be0) | 0x28dbe0 | |
| L2 | set the game type (GMM +0x18) | 0x28ab50 | the story's Multiplayer gate reads it (STORY.md §4.3) |
| L3 | **Start_Game(0, campaign)**: FrameSynchronizer reset (frame 0), RNG seed, default players (0x293390), galactic mode allocated and made current (GMM +0x38 = 0x140b15418). The mode's init 0x4b21f0 sets the income bookkeeping (+0x368 = +0x36c, +0x36c += one period: the "frame-0 cycle" of GALACTIC_ECONOMY §1.2, before any planet exists) | 0x28e890 | mode frame stays 0 for the whole load |
| L4 | per player (PlayerList order): victory conditions 0x289a90; Set_Difficulty 0x4b0dc0 for players that already have an AIPlayer | 0x28dbe0 loop | |
| L5 | start camera / local-player focus | 0x3c2c00 | |
| L6 | story manager reset (mode +0xa0) | 0x32acc0 | |
| L7 | **plot loading**: Rebel, Empire, Underworld `*_Story_Name` (campaign +0x48 / +0x68 / +0x88), then the campaign's faction / plot pairs; each through 0x329aa0 with the faction's player as owner. `Active_Plot` plots are active at once; events without Prereq are armed at creation (STORY.md §1.1, §2.1) | 0x329aa0 | |
| L8 | **galaxy and starting forces**: planets from `Locations` (0x29f810 per entry), then per planet the `Starting_Forces` (0x3fae10: planet owner +0x74, units through the creation path 0x400b40) | 0x292050 | 0x400b40 raises STORY CONSTRUCT / CONSTRUCT_LEVEL for every object it creates (§3.3) |
| L9 | starting credits (player +0x70) | 0x291f70 | |
| L10 | **starting tech**: per player in PlayerList order, `Set_Tech_Level(Starting_Tech_Level, sweep)` 0x288980, then max tech +0x88 = `Max_Tech_Level` | 0x292590 | the setter clamps to the current +0x88 (player ctor default 5, so 3 stays 3) and raises STORY_TECH_LEVEL (§3) |
| L11 | `AI_Player_Control` -> Set_AI_Type 0x2886d0 | 0x291d80 | |
| L12 | per object: behaviour vf +0x70 with the campaign (planets: trade routes, 0x3f7490 -> 0x3f5e10) | 0x2b09b0 | |
| L13 | single player only: 0x2924d0, then Set_Difficulty for every AI player 0x291ee0 -> 0x4b0dc0 | | rescales health of the objects the AI owns, including load-time story spawns [M] |
| L14 | markup files, galactic flags (campaign +0x378..+0x37a), 0x28d690, mode +0x38 hook | | |

Then the load dialog closes, FS +0x34 = 0, and the first tick runs on the next main-loop iteration.

## 3. Story notifications during load

### 3.1 Starting tech -> STORY_TECH_LEVEL [H]

- 0x288980(player, level, sweep, ...): +0x84 = min(max(level, 0), +0x88), then **0x32e8c0(current mode story +0xa0, level,
  player)** -> 0x32e5f0(type 10 = TECH_LEVEL, player scope = that player). Every call raises it, changed or not. The
  other callers raise it at the same moment as well: tech-upgrade production completing (0x402840, inside the GOM
  service at T5), the AI / Lua / story SET_TECH_LEVEL paths (0x288980 xrefs).
- Evaluator 0x454030 (TECH_LEVEL / BEGIN_ERA): fires when level >= Event_Param1. With Param1 = -1 it records the
  first observation and fires on the next different level. So the starting-tech call is the **first observation** of a
  "-1" event.
- BEGIN_ERA (type 9) is not raised by the setter. It is dispatched once per T9 by 0x3268c0 with the value from the
  mode +0x9c getter 0x490480 (+0x9c = max of the players' +0x84, common tick step 9) and **no player scope** (all
  active plots) [H call, M argument passing].

### 3.2 Dispatch gate 0x32e5f0 [H]

A notification reaches the plots at once when the target story manager's mode (+0x40) is the current mode, the global
0x140b155c4 is 0 (set and cleared by the attract / demo controller 0x22bf0 / 0x22f10 [M]), and one of these is true:
the application is active (0x140b2b5d0 -> 0x1409cf314, written by the WM_ACTIVATEAPP handler), the game type is
multiplayer (0x28af60: GMM +0x18 in {1, 2, 4}), or 0x140b1574e is set [L meaning]. Otherwise the event is queued as
"STORY DELAYED EVENT" and replayed at the start of the next T9 (types 8, 0x18, 0x19 are dropped instead).

At L10 the galactic mode is current (L3) and the plots are loaded and active (L7). So with the game window in front
the dispatch is immediate [M: the window is active during a normal load]. Fire then runs Story_Event_Trigger, the
reward and the dependants loop synchronously (STORY.md §1.5).

Caveat [M]: a queued event keeps only the pointer to its payload, and TECH_LEVEL's payload is a stack variable of
0x32e8c0 (only SPEECH_DONE copies its string). So if the game loses focus during the load, the replay at the first
T9 reads a stale value. The probes always had the game in front.

### 3.3 What fires in the HotM Rebel entry [H data, M order]

Plots: Rebel `Story_Plots_Sandbox_12U_Rebel` -> `Story_Sandbox_12U_Rebel.XML`, Empire `Story_Plots_Empire_AI_Heroes_EXP` ->
`Story_Empire_AI_Heroes_EXP.xml`, Underworld `Story_Plots_Underworld_AI_Heroes` -> `Story_Underworld_AI_Heroes.xml`.
Starting tech 3 for all three. These plots only use STORY_ELAPSED, STORY_TECH_LEVEL and STORY_TRIGGER.

| When | Empire | Rebel (human) | Underworld |
|---|---|---|---|
| load, L10 (tech 3, so the tech 1, 2 and 3 events fire) | Boba_Fett_Team, General_Veers_Team, **Accuser_Star_Destroyer** (UNIQUE_UNIT; Coruscant is not on the map, so the anchor is the first planet of the per-owner list, 0x2a9ff0(GOM, 3, player)) | Han_Solo_Team, Sundered_Heart, Obi_Wan_Team, Rogue_Squadron_Space, **Home_One** (UNIQUE_UNIT), Yoda_Team | nothing (no tech events) |
| tick 1, T9 | `Universal_Story_Start` records its start time (no fire) | same | same |
| tick 2, T9 (`Universal_Story_Start` fires) | Grand_Moff_Tarkin_Team, Emperor_Palpatine_Team, unlock E_Ground_Research_Facility | Mon_Mothma_Team, Droids_Team, unlock Rebel_X-Wing_Squadron | Tyber_Zann_Team, Bossk_Team, Urai_Fen_Team, IG88_Team, Silri_Team |

Trace check [H trace, M mapping]: the Q2 HotM trace (Empire entry; GALACTIC_RNG_DRAWS row 3) has the tech-level heroes
at mode frame 1, and none of the ELAPSED-0 heroes (Tarkin, Emperor, Mon Mothma, the droids, the Underworld heroes). The
port's `--story` run has both sets by then. That fits ELAPSED 0 firing at tick 2. It does not tell load apart from
tick 1's T9 for the tech-level spawns: only the budget fit in §3.4 does that.

Inside L10, the order is PlayerList order, then the TECH_LEVEL bucket order of each player's plots (hash order,
STORY.md §4.2). Each spawn's creation (0x400b40) raises STORY CONSTRUCT for the new unit synchronously as well. During
L8 the same is true for every starting-force object (CONSTRUCT, and CONSTRUCT_LEVEL for starbases / ground bases),
since the plots are already loaded. The HotM plots have no CONSTRUCT events, so it changes nothing here. It would for
a campaign whose plots react to a starting structure, e.g. `Story_Campaign_Underworld.xml` [M].

In 0x4626a0, after the spawn, an AI owner gets a "new object" call on its current-mode execution system (context
+0x50 -> +0x38, vf +0x98). It needs player +0x360 and a current-mode context. At L10 the AI types are not assigned yet
(L11), so whether this call runs at load is [L]. It does not touch the budget.

### 3.4 Consequence for the AI Init [H arithmetic from A2H_T03, M link]

At T7 of tick 1 the Empire already owns the Accuser in a fleet: MaintenanceUnnormalized = 75.333 x 0.25 / 30 = 0.62778,
Infrastructure weight 3.3 x 3.25 x (1 + 0.62778 / 667.5) = 10.73509. That matches retail's 10.7351. The human's
`SpaceTotalUnnormalized` at Init also includes Home_One, Sundered_Heart and Rogue_Squadron_Space (+5360). The port has
these values only from frame 2 today (A2H_T02_SPACE_TOTAL.md).

## 4. Per-tick order of a galactic tick [H, from MAIN_LOOP §1.2 / §3, re-read for this question]

| Step | What | Address |
|---|---|---|
| T1 | Begin_Tick: global frame += 1 | 0x27c360 |
| T2 | scheduled commands due | 0x2c0c70 |
| T4 | TreeCull | 0x20f000 |
| T5 | GameModeManager::Update 0x28d400: GameScoring Lua service, then the galactic tick 0x4b2ef0: 0x4b2600, common tick 0x3639d0 (GOM service: object behaviours, object Lua scripts, production completion; then mode +0x9c = max tech), cinematics, **economy: income when the mode frame reaches the next income frame** (GALACTIC_ECONOMY §1.2), per-planet RNG, victory events | 0x28d400, 0x4b2ef0, 0x3639d0 |
| T6 | scene animation | 0x2718e0 |
| T7 | **PlayerList update** per player (list order): 0x287e70 -> AIPlayer 0x4b0ca0 -> context service 0x5f5ab0 (first time: opponent pick, subsystem slot-6 inits, Template init 0x6bbed0 -> budget Init 0x6137b0, +0x69 = 1; then the subsystem services: perception, goals, planning, execution with its Lua plans and free store) | 0x297e30 |
| T8 | global / console Lua script | 0x1dc60 |
| T9 | story 0x3271d0: replay delayed events, STORY_ELAPSED, per plot (table order) the plot Lua service 0x52f720 then timers / FLAG poll, UNIT_PROXIMITY, CHECK_DESTROYED, FOG reveals, OBJECTIVE_TIMEOUT, "battle_end_closed"; then the mode +0x9c getter 0x490480 and **BEGIN_ERA** 0x3268c0 | 0x3271d0, 0x490480, 0x3268c0 |
| T10 / T11 | tutorial helper; campaign-end check | 0x44a3f0, 0x92bb0 |
| T12 | Post_Tick: mode frame += 1 unless paused | 0x28a4c0 |
| T13 | End_Tick | 0x27cb70 |

## 5. Frame 0 and frame 1

- The galactic **mode frame** is 0 from L3 through the whole load and the whole first tick. T12 of tick 1 makes it 1.
  The **global frame** is 0 during load, and T1 makes it 1. [H]
- So "retail frame 0" in the a2f / a2h probes (story Lua samples, time 0) is **T9 of tick 1**, after that tick's T7
  Init. The watcher's "initialised byte 1 at mode frame 1" is the same tick seen after T12. A2G_T02_T04 §1 left the
  order open ("whether retail's frame-0 story sample runs before or after frame 1's AI service"). Answer: after. [H for
  the frame counters, M for the probe's time base]
- The port's "frame 1" (SimHost `frame_` after T1 of the first `tick_once`, galactic mode frame 0) is the same tick. The
  port's T7 / T9 order matches retail. The only difference is that the port has no load-time story pass.
- The Q2 trace's "no objects at mode frame 0" (GALACTIC_ECONOMY §1.2) is a reading taken during the load, before L8.
  It fits the table in §2: the mode exists with frame 0 before the galaxy does. [M]

## 6. Port change

**Order to use.** Tick order unchanged: T5 (galactic mode tick incl. economy) -> T7 (AI) -> T8 -> T9 (story). Add one
step to the campaign start, after the story plot sets are loaded (their plots active, scripts loaded and
`Base_Definitions` run) and before the first `tick_once`:

> For each player in player-index order (retail: PlayerList order, L10): dispatch STORY_TECH_LEVEL with that player's
> current (starting) tech level to the plot sets owned by that player, with the story clock at mode frame 0 / time 0,
> not paused. Apply the resulting Fire chain (Story_Event_Trigger, reward, dependants) synchronously, with the same
> reward code as T9 (SPAWN_HERO / UNIQUE_UNIT anchor rule, `GalacticSim::spawn_unit`).

In the port this belongs at the end of `StorySession::start` (or a `SimHost::start_story` follow-up call), since
`GalacticSim::start` has already built the galaxy, the starting forces, the credits and the tech levels (L8-L10) by then.
Both hosts call `start_story` before the first tick: `ai_run.cpp` before the AI world exists, `game_session.cpp`
after `GalacticAiWorld::start`.

Expected effect (HotM Rebel entry, `--story`): the Empire's Init (tick 1, T7) has MaintenanceUnnormalized 0.62778 and
Infrastructure weight 10.73509 (retail 10.7351). Human SpaceTotalUnnormalized at Init is as at frame 2 today. [M]

**Keep** the port's per-T9 TECH_LEVEL poll (`StorySession::poll`). Events fired at load are triggered and not
Perpetual, so the poll cannot fire them twice. It still covers later tech changes, which retail raises synchronously
from 0x288980. A "-1" (next level) event then takes its first observation at load, as retail does. No shipped HotM plot
uses -1.

**Do not move T9 before T7.** Retail runs T9 after T7 on every tick, including tick 1 (§4). A swap would:
- with today's port ELAPSED timing (first item of "Related differences" below), give the AI's Init the ELAPSED-0 rewards (Tarkin, the
  Emperor, the Underworld heroes). Retail's Init does not see them, because they spawn at tick 2;
- run plot Lua scripts and the T9 drain of the galactic event stream before the AI on every tick;
- move every later story-versus-AI interaction by one phase, with no retail basis.

**Side effects of the load pass**
- RNG / objects: the load-time spawns (Empire 3, Rebel 6 in HotM) move their behaviour-attach draws and object ids from
  T9 of tick 1 to before tick 1. That is retail's order: load draws, then the first AI proposal pass. GALACTIC_RNG_DRAWS
  row 3 counts all of them in "tick 1" because the trace's tick 1 includes the load. Every `--story` golden and every
  game campaign smoke golden moves (galactic_ai_hotm*, save / galactic-AI CRC tests that run a story, game smoke). Runs
  without a story session do not change.
- Economy: no change. Income runs inside T5, and the first payment is at mode frame 1350. Upkeep is only an AI token
  read (no credits are charged).
- Lua services: the plot scripts' first `main` service stays at T9 of tick 1. Only Story_Event_Trigger callbacks of the
  fired events would run at load (none of the three HotM plots has a script). The AI's Lua plans and free store first
  run at T7 as now.
- Initial CRC record: `SimHost::tick_once` captures the "initial state (after load)" before tick 1, so the spawns are in
  record 0. The story CRC (`cfg.story_crc`) must hash the new fired flags there too.
- Events: the spawn's galactic events (unit created / construction) reach the story's event drain at T9 of tick 1,
  while retail dispatches CONSTRUCT inside the creation call. No HotM plot reacts to them.
- `game_session.cpp` builds the galactic AI before `start_story`. The AI must take a unit spawned between its `start()`
  and its first `update_player` the same way as a starting unit. Check that its fleet and unit lists are read live, not
  cached at `start()` [L].
- Difficulty: retail runs Set_Difficulty (L13) after the tech spawns, so the AI-owned Accuser gets the AI health
  multiplier like the starting forces [M]. That applies wherever the port applies the multiplier.

**Related differences, separate from this task**
- Prereq-less STORY_ELAPSED fires one T9 early in the port [H]. Retail 0x454980: the first evaluation stores
  LastTime (+0x360) and sets the initialised byte (+0x36c), then **returns**. The accumulator test runs only from the
  second evaluation on. The port's `StoryModel::tick_timer` (engine/lua `story_model.cpp`) records, then tests
  `acc >= threshold`, so an ELAPSED 0 fires at its first T9 (tick 1) instead of tick 2. This is the "port spawns more
  heroes by tick 1" of GALACTIC_RNG_DRAWS §3 row 3. Fix in engine/lua: return false on the first evaluation. A
  prereq-armed timer (vf7 0x4511f0 sets LastTime at activation) is not affected. It does not change the AI Init (T7 of
  tick 1 is before both T9s). It does move the ELAPSED-0 spawns' draws and the story goldens.
- Starting-force CONSTRUCT / CONSTRUCT_LEVEL at load (§3.3): the port creates starting forces before the story exists
  and emits no events then, so plots never see them. A load-time CONSTRUCT dispatch for each starting object, after the
  plot load and before the tech pass, would match retail. No HotM effect.
- Mid-game tech-ups: retail fires TECH_LEVEL at the setter (e.g. in T5 when a tech upgrade completes, before T7). The
  port fires at the next T9 poll, after T7.
- BEGIN_ERA: retail dispatches it at T9 with the mode-wide maximum tech level and no player scope. The port's poll
  uses each plot set's own player's tech level (`StorySession::poll`). It differs only when players' tech levels differ
  and a plot uses STORY_BEGIN_ERA (none in HotM).

## 7. Not resolved

- 0x140b1574e (third clause of the dispatch gate) and the exact meaning of 0x140b155c4 [L / M].
- Whether the AI "new object" call in 0x4626a0 runs at load (player +0x360 at L10) [L]. It has no budget effect.
- Context vtable slot +0x18 = 0x5f5ab0 (read from the call shape in 0x4b0ca0, not from the vtable dump) [M].
- An in-game check of the claim: a probe story script that reads the Accuser's existence or the Empire's
  `MaintenanceUnnormalized` at time 0 cannot tell load from T7. A `CONSTRUCT` or `Story_Event_Trigger` hook would have
  to log the frame it ran in. The fit in A2H_T03 (§3.4) is the current evidence.
