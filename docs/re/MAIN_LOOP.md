# Main loop, frame pacing and game modes (StarWarsG.exe FoC x64 v1.121) — behaviour spec for roadmap S5

Written 2026-10-27 by the RE agent, ahead of implementation of S5 "Main loop & game modes".
Clean-room: this file describes behaviour, constants and orders in prose and tables. It contains no
code and no pseudo-code from the decompilation. Addresses are RVAs in StarWarsG.exe; names are the
renamed names from `docs/re/ghidra/` where they exist, otherwise `FUN_<rva>`. Many renamed names in this
area are misleading (most main-loop helpers are called `Game_Boot_Init_Filesystem_helper_N`); the
address is authoritative.

Confidence: **[H]** read in the code (decompile, and disassembly where argument passing mattered),
**[M]** read in the code but the role is inferred, **[L]** inferred from surroundings or strings.

Companion documents: `RENDER.md` §1.1–1.2 (what happens inside the render call), `PHASE2_ANSWERS.md`
§11–12 (90-frame AI cadence, tick constants), `SUBSYSTEMS.md` (mount order, threading),
`OBJECT_RUNTIME.md` / `OBJECT_TYPES.md` / `GALACTIC_ECONOMY.md` (written in parallel: per-object
service, behaviours, income — not repeated here).

Global objects used throughout (all are statics in the exe):

| global | what |
|---|---|
| 0x140b0a320 | FrameSynchronizer (FS). +0x00 global logic frame, +0x04 render-frame count |
| 0x140b153e0 | GameModeManager (GMM). +0x18 game type, +0x1c active mode type, +0x2c galactic pause counter, +0x38 current mode, +0x40 mode stack (DynamicVector: data +0x48, count +0x50, capacity +0x54), +0xe0/+0xe4/+0xe8 game seed / alternate seed / use-alternate flag |
| 0x140a16fd0 | PlayerList (persistent across modes) |
| 0x140b2ed18 | OutgoingEventQueue (commands issued locally) |
| 0x140b15690 | ScheduledEventQueue (commands to execute on a frame) |
| 0x140a13e24 | synchronized game RNG state |
| 0x140a13e20 | free (non-synchronized) RNG state |
| 0x140a7bec0 | wall-clock base: `timeGetTime()` value captured at boot; every "now" below is `timeGetTime() − base` in ms |
| 0x1409cf314 | application-active flag (WM_ACTIVATEAPP, WndProc 0x60c60) [H] |

---

## 0. Summary for the S5 implementer

1. **One thread, one loop, at most one logic tick per iteration.** There is no catch-up loop and no
   accumulator: if the loop falls behind, the game simply runs slower in wall time. [H] (0x5d990)
2. **Tick pacing is wall-clock gating with `timeGetTime` (1 ms resolution, `timeBeginPeriod` at the
   device minimum).** A tick is allowed when `now − last_tick_time ≥ floor(1000 / rate)` ms, and then
   `last_tick_time = now` (not `+= period`), so drift is never paid back. [H] (0x27c5f0)
3. **The simulation step is always 1/30 s** (sim time = frame / 30). Game speed changes only the
   wall-clock pacing rate: speed index 0..4 → **10, 20, 30, 45, 60 ticks/s** (table at 0x85aa00), kept
   separately for galactic and tactical, default index 2. Single-player fast mode forces **120**;
   a cinematic forces **30**. [H] (0x27bf80, 0x27c5f0, table read from the exe)
4. **Rendering happens once per loop iteration, independent of ticks**, capped by `FPSCAP=`
   (default **200 FPS**, minimum 15) with a `Sleep` after present. There is **no interpolation**:
   the renderer draws the latest sim state. In fast mode only every 4th frame is presented. [H]
5. **Pause is not a mode-stack operation.** Tactical pause stops ticking entirely (FS +0xb0b0);
   galactic pause is a nesting counter (GMM +0x2c) that keeps ticking the global frame but freezes the
   galactic mode's own frame and object behaviours. [H]
6. **Mode stack: top of stack is element 0.** Only the top mode is ticked and rendered; a galactic
   game stays alive (suspended) under a tactical battle and does **not** tick during the battle. [H]
7. **Two frame counters:** FS +0x00 (global, continuous across a galactic→tactical→galactic round
   trip, reset only when the first mode of a session starts or all modes end) and mode +0x10 (per mode,
   starts at 0, advanced once per tick unless the mode reports "paused"). [H] (resolves the open item
   "Frame counters" in PHASE2_ANSWERS)
8. **RNG: two instances of the same 32-bit LCG** (`s = s·0x41C64E6D + 0xBDF`, 15-bit output
   `(s >> 10) & 0x7FFF`). The synchronized one is seeded once per session from GMM +0xe0; the free one
   (audio, UI, particles) is seeded from `QueryPerformanceCounter` at boot. [H]

---

## 1. The main loop

The loop is the tail of the boot function `Game_Boot_Init_Filesystem` 0x5d990 (boot itself is §5). It
runs until the exit flag 0x140a619ef is set (WM_QUIT seen in the message pump, or a fatal init failure).
On exit: `timeEndPeriod`, release the single-instance mutex, optionally relaunch Media Center, then
`TerminateProcess` (no orderly teardown). [H]

### 1.1 Order of one iteration [H for order; roles M unless stated]

| # | step | address | notes |
|---|---|---|---|
| 1 | one-shot splash/E3 latch | 0x25fb10 | first iteration only |
| 2 | ask the current mode for the **audio listener** (position, forward, up) | mode vtable +0x158 (galactic 0x4b1600, land 0x3b5e20) | defaults if no mode [M] |
| 3 | **pump all window messages** (`PeekMessage` remove, `TranslateAccelerator`, `TranslateMessage`, `DispatchMessage`); WM_QUIT sets the exit flag | | input arrives through the WndProc 0x60c60, i.e. before any tick [H] |
| 4 | minimise/focus housekeeping when active for > 1 s | 0x66980 | [L] |
| 5 | Lua value housekeeping, network lobby/peer update | 0x24bb80, 0x2505c0 | |
| 6 | **F0 = global frame** (captured before the tick) | | used for the render delta (§1.4) |
| 7 | network pump | 0x2089e0 | |
| 8 | **flush the outgoing command queue** | 0x3b08c0 | single-player: each command is stamped with the current global frame and moved straight into the scheduled queue; multiplayer: sent to peers (0x3b03e0) [H] |
| 9 | receive remote commands into the scheduled queue (multiplayer only) | 0x2c2910 | |
| 10 | **single-player only: execute scheduled commands with frame ≤ F0** | 0x2c0c70(F0) | runs every iteration, even when no tick follows and while paused — local orders take effect between ticks [H] |
| 11 | MP vote manager, network service; every 64th frame network diagnostics; profile overlay | 0x4908c0, 0xcaa60, 0x503d0, 0x51d10 | debug/UI |
| 12 | **now** = wall ms; Δwall = now − previous now | | |
| 13 | slow-frame profiler (Δwall/1000), Steam broadcaster, attract/demo controller, lobby | 0x215b90, 0x339bc0, 0x22c50, 0x6a1e0, 0x6d1d0 | the demo controller can raise the pause flag (§1.5) [M] |
| 14 | if the application is active: **audio update** with the listener from step 2 (Miles listener, 2D/3D SFX, `AIL_serve` at most every 125 ms), then SFX events, music, speech, two more audio managers, weather audio | 0x21caf0, 0x2d72c0, 0x2d2ab0, 0x321dc0, 0x325190 | wall-time driven; uses the free RNG [H] |
| 15 | GUI dialog manager update (tooltips etc.) | 0x1c8420 | |
| 16 | if the device is usable: movie queue, HUD/GameUI per-frame update(now), debug overlays | 0x44d840, 0x30c3b0, 0x2ee40, 0x305890, 0x493f0 | |
| 17 | MP out-of-sync dialog when a CRC mismatch was flagged | 0x99d90 | |
| 18 | 2D texture-rect manager, misc | 0x262b80, 0x3b1b50 | |
| 19 | **tick decision** (§1.2) — at most one logic tick | | |
| 20 | if windowed or active: **per-frame update + render** (§1.4) | | |

If the application is inactive in single-player and nothing network-related is pending, step 19 is
replaced by `WaitMessage()` — the loop blocks until a window message arrives (the game freezes when
alt-tabbed in fullscreen). [H] (0x5d990, 0x24cef0)

### 1.2 Tick decision and the logic tick [H]

The pacing test `FrameSynchronizer::Pace` 0x27c5f0(now) returns "tick now" or "not yet":

- If FS +0x34 ("simulation stopped": no mode, or a load in progress) is set → no tick.
- **Single-player** (game type 0 or 3): rate = FS +0x18; if FS +0xb0b5 (fast) → 120; if FS +0x37
  (cinematic) → 30. If `now − FS+0x50 < floor(1000/rate)` → no tick (the remaining ms is returned but
  the main loop ignores it). Then the **pause/step gate**: if FS +0xb0b0 (paused) is set and FS +0xb0b1
  (single-step request) is clear → no tick; otherwise the step request is cleared.
- **Multiplayer** (types 1, 2, 4): wait until every human peer has reported frames and command counts
  up to MaxAhead (FS +0x08), then pace at `floor(1000/(rate+5))` ms, or `floor(1000/(rate+10))` when
  this machine is behind (see PHASE2_ANSWERS §12).
- On "tick now": FS +0x50 = now.

When the test passes (and, in MP, the mode is not waiting on a scripted network condition), one tick
runs in this order:

| # | step | address |
|---|---|---|
| T1 | **Begin_Tick**: global frame += 1; "in tick" byte FS +0x14 = 1; once per wall second store the measured ticks/s in FS +0x1c | 0x27c360 |
| T2 | execute scheduled commands with frame ≤ new frame (all modes; this is the MP execution point) | 0x2c0c70 |
| T3 | every 100 frames: AI debug-diagnostics refresh (debug only) | 0x4767c0 |
| T4 | **TreeCull: rebuild dirty spatial trees** (sim time = frame/30) | 0x20f000 (HOTSPOTS §) |
| T5 | **GameModeManager::Update(frame)** — §3.1 | 0x28d400 |
| T6 | advance registered scene/2D animations with sim time frame/30 (unless the wall-time-animation flag 0x140a61d38 is set; then this happens with wall time at render, step R0) | 0x2718e0 |
| T7 | **PlayerList update**: per player in list order, including the AI player service for the current mode | 0x297e30 → 0x287e70 |
| T8 | global debug/console Lua script service (gated by its own `ServiceRate`/`LastService` in sim seconds) | 0x1dc60 |
| T9 | if a mode exists: **story plot service** on mode +0xa0 (story Lua scripts, delayed events), mode +0x9c getter, story second pass | 0x3271d0, 0x490480, 0x3268c0 |
| T10 | tutorial/story-mode helper | 0x44a3f0 |
| T11 | galactic campaign end check: in a galactic top mode, if the victory monitor reports finished, open the campaign-end dialog | 0x92bb0 |
| T12 | **GameModeManager::Post_Tick**: current mode frame (+0x10) += 1 unless the mode's "paused" query (vtable +0x48) is true | 0x28a4c0 |
| T13 | **End_Tick**: clear FS +0x14; compute the frame CRC when synced (MP, or the mode +0x98 flag), store frame/CRC/command count in a 256-entry ring (index frame & 0xff), send FrameInfo every FS +0x0c frames and perf metrics every 16×, (MP) | 0x27cb70 |
| T14 | a player left during the tick → handle (dialog, hand units over) | 0x5f6b0 |

When the test fails: MP stall dialog after 3000 ms of no progress (0x9ba60), vote manager, `Sleep(0)`,
and in MP re-send FrameInfo every 500 ms (0x27d6a0). Single-player just falls through to rendering. [H]

### 1.3 Time sources and sim vs wall time [H]

- **Wall clock:** `timeGetTime()` everywhere in the loop (`timeBeginPeriod(min)` at boot from
  `timeGetDevCaps`). Values are relative to the boot base 0x140a7bec0 (the base is re-captured by
  0x1fe220/0x1fe270, [L] save/load related).
- **QueryPerformanceCounter** is used only to seed RNGs (0x222fa0), for profiling, and by the FPS cap
  (0x222e90 returns QPC-based ms).
- **Sim time** = global frame / FS +0x20 (30), as float. FS +0x24 = 1/30. The tick never receives a
  wall-clock delta.
- **Wall-time accumulators that are not sim:** Δwall feeds the 2D/GUI clock (0x25ca30, which wraps at
  28800 s, RENDER §1.1); FS +0xb09c accumulates render-call seconds; GMM +0x1e8/+0x1ec accumulate the
  wall duration of a tactical battle while ticks happen (0x28c580). [H]

### 1.4 Per-frame (render-rate) work [H]

After the tick decision, if the window is windowed or the app is active:

| # | step | address |
|---|---|---|
| R0 | (wall-time animation flag only) advance scene animations with wall seconds | 0x2718e0 |
| R1 | **frame delta** `d = max(0, (frame_now − F0) × FS+0x20 / 1000)`: 0 when no tick happened this iteration, **0.03** for one tick (the original multiplies by 30 and divides by 1000; it is not 1/30) | |
| R2 | if this frame is presented (fast mode: frame % 4 == 0; otherwise always): | 0x27d800 |
| R2a | **GMM::Pre_Render(d)**: current mode vtable +0xb8 (per-frame update: camera, cinematics, scene `Update` on the main and three background scenes, mode HUD); for tactical modes with d > 0 add wall time to the battle timer | 0x28c580; land 0x3bad20, space 0x4d9110, galactic 0x4b2c00 (passes 0 while galactic-paused), common 0x362c60 |
| R2b | 2D scene list update(d), HUD update, mouse/camera input helper, 2D clock(Δwall/1000) | 0x271840, 0x301750, 0x2ffb0, 0x25ca30 |
| R2c | **Render(ticked = d ≠ 0)** — order in RENDER.md §1.1; it starts with the mode's pre-render hook (vtable +0xa0: land 0x3ba400, space 0x4d8de0, galactic none) and ends with present and the FPS cap | 0x60330 |
| R3 | count render frame, measured FPS → FS +0x40 once per second; add render ms to FS +0xb09c | 0x27c2f0, 0x27c330 |

Consequences for the port: per-frame consumers that move with `d` (camera scroll, 2D effects) only
advance on iterations where a tick happened. [M] The "ticked" flag passed to Render only gates
screenshot capture. [H]

**FPS cap** (inside 0x60330, after present): limit = `FPSCAP=` (default 200 at 0x140a10d80, clamped
to ≥ 15); target = floor(1000/limit) − 1 ms; if the time since the previous present is shorter,
`Sleep` the difference. [H]

### 1.5 Pause, step, fast mode, game speed [H unless noted]

| state | storage | effect |
|---|---|---|
| tactical pause | FS +0xb0b0 (0x140b153d0), mirrored in land/space mode +0x99 (their "paused" query) | no ticks at all; rendering, audio, UI and SP command execution (step 10) continue. Set by the pause menu (0x31f870 "TEXT_GAME_PAUSED"), by opening full-screen dialogs (0x96420, 0x104b00…), by the land/space pause toggles 0x3ba3b0/0x3baed0 (not in MP), and by the attract/demo controller 0x22c50 (it starts AI-vs-AI demo maps after menu idle time, 0x22f10, and writes this flag from an idle timeout) [M]. Every pause/unpause clears fast mode (+0xb0b5). |
| single step | FS +0xb0b1 | while paused, allows exactly one tick, then clears itself |
| galactic pause | GMM +0x2c, nesting counter (0x28bb80 increments, 0x28c720 decrements; sim-second timestamps at GMM +0x24/+0x28) | ticks continue and the global frame advances, but the galactic "paused" query (0x4b2360) is true → mode frame frozen, GOM services objects in "frozen" mode (behaviours not serviced), galactic per-frame delta forced to 0. Pending space/land conflicts use the same counter. |
| fast mode | FS +0xb0b5 | SP only (0x2e41d0, from a HUD state 1→2 [L]); rate 120 and present every 4th frame |
| cinematic rate lock | FS +0x37 | set by Start/End cinematic camera and cinematic mode (0x49b1a0 callers); forces 30 |
| game speed | FS +0xb094 (tactical index), +0xb098 (galactic index), +0xb090 (active index); rate FS +0x18 = table[active] | set by `GameOptionsEventClass` (a scheduled event, so synced in MP; 0x44d670 → 0x27bf00) and from lobby settings at MP Start_Game; the active index is reselected whenever the top mode changes (0x27bf80). Indices reset to 2 when a session starts (0x27d020). |
| simulation stopped | FS +0x34 | set to 1 by FS reset and when the last mode ends; 0 at Start_Game end and when a load completes |

Corrections to PHASE2_ANSWERS §12: FS +0x1c is the **measured** ticks per wall second (not a
constant); FS +0x18 is the pacing rate taken from the speed table; the "galactic time scale" open item is
answered above (speed changes wall pacing, not dt). [H]

---

## 2. GameModeManager and the mode stack

### 2.1 Modes [H]

| type (vtable +0xe0) | class | size | vtable | tactical? (vtable +0xf0) |
|---|---|---|---|---|
| 0 | GalacticModeClass | 0x378 | 0x8762c0 | no |
| 1 | LandModeClass | 0x430 | 0x8673a0 | yes |
| 2 | SpaceModeClass | 0x3c8 | 0x8792f8 | yes |

There is **no frontend/menu mode**: the main menu, lobby, load screens and dialogs are GUI dialogs
(GUIDialogManager 0x140a78510) living outside the mode stack while the stack is empty (current mode
null, FS +0x34 = 1, nothing ticks). [H]

Mode virtuals used by the loop (slot = byte offset / 8):

| offset | role | galactic / land / space |
|---|---|---|
| +0x08 | initialise(game type, mode type) | 0x4b21f0 / 0x3b81b0 / 0x4d7820 |
| +0x10 | shutdown | 0x4b3350 / 0x3bbd70 / 0x4d97a0 |
| +0x18, +0x30 | post-create hooks (called after becoming current; +0x30 again on resume) | shared 0x3b5e10, 0x3b5a40 |
| +0x28 | pre-shutdown | 0x4b1fb0 / 0x3b7aa0 / 0x4d7540 |
| +0x48 | "paused" query (freezes mode frame) | 0x4b2360 / mode +0x99 / mode +0x99 |
| +0x50 | suspend (another mode pushed on top) | 0x4b25f0 (hide galactic HUD) / no-op / no-op |
| +0x58 | resume (back on top) | 0x4b2c70 (show HUD, refresh planets) / no-op / no-op |
| +0xa0 | render pre-hook | none / 0x3ba400 / 0x4d8de0 |
| +0xa8 | render 3D | RENDER.md §1.2 |
| +0xb0 | **logic tick(global frame)** | 0x4b2ef0 / 0x3bb440 / 0x4d95a0 |
| +0xb8 | per-frame update(d) | 0x4b2c00 / 0x3bad20 / 0x4d9110 |
| +0x158 | audio listener | 0x4b1600 / 0x3b5e20 / 0x4b1600 |

### 2.2 Start_Game (push) — `GameModeManagerClass::Start_Game` 0x28e890 [H]

1. Mark "starting"; clear vote/battle bookkeeping; stop speech, SFX conversation and weather loops.
2. **If the stack is empty (new session):** stop any movie; **reset the FrameSynchronizer** (frame = 0,
   rates 30, speed indices 2, pause flags clear); **seed the synchronized RNG** from GMM +0xe0 (or
   +0xe4 if GMM +0xe8 is set); clear the galactic pause counter; reset an AI/data manager; create the
   default players; reset the tactical-battle vote.
   **Otherwise:** call the current top mode's suspend (+0x50).
3. Allocate the new mode by type (0 galactic, 1 land, 2 space). For tactical modes zero the battle
   wall timer and stamp its start time.
4. Insert the new mode at **index 0** of the stack (existing entries shift up), call init(+0x08), make it
   current (GMM +0x38), call +0x18 and +0x30; GMM +0x1c = mode type; galactic modes copy the campaign
   name/flag from the campaign record.
5. New-session tactical games (skirmish/MP): set up players from the setup record (four PlayerList
   calls) and the GOM.
6. MP: apply lobby game-speed indices. Always: select the active speed index for this mode type.
7. FS +0x34 = 0 (simulation running); clear "starting".

### 2.3 End_Game_Mode (pop) — 0x28f320, and End_All 0x28d940 [H]

Pop (0x28f320): stop speech/audio as above; broadcast signal 0x24 (mode ending) through the
SignalDispatcher; call +0x28 then +0x10 on the top mode; remove it from the stack; destroy it; GMM
+0x38 = null; GUI cleanup. If a mode remains, the new element 0 becomes current, gets **resume
(+0x58)** and +0x30, GMM +0x1c is updated and the active speed index is reselected. If the stack is now
empty: AI/Lua teardown (0x47f690, 0x2566d0), **FS +0x34 = 1** (simulation stopped), optionally flush
the scheduled queue.

End all (0x28d940, used by quit-to-menu, load game, player-left): pops repeatedly until empty, then
resets the FrameSynchronizer (frame = 0), clears both event queues, resets GMM state, releases two
textures and resets the game-object-type and story managers. [H]

### 2.4 Galactic conflict → tactical battle → galactic [M for the overall flow, H for each call]

1. **Detection (galactic tick):** the galactic GOM service (0x2be640) finds conflicts (0x2ad100) and
   runs the space-conflict / land-invasion logic (0x2b92e0, 0x2b76a0: auto-resolve texts,
   "PENDING_TACTICAL_BATTLE_VOTE"). A pending conflict raises the galactic pause counter (§1.5), so the
   galaxy freezes while the player chooses fight or auto-resolve (dialog 0x28c6c0
   "TEXT_BUTTON_BEGIN"/"TEXT_BUTTON_RESOLVE_BATTLE"). In MP both sides vote (0x86550).
2. **Load:** the battle-load dialog (`BattleLoadDialogClass::Activate` 0x8d930 → 0x8cb50) sets FS
   +0x34 = 1, then 0x8df00 starts a worker thread "LoadThread" that runs `SaveLoadManagerClass::Load_Map`
   0x56480. Load_Map reads the map chunk file, sets the game type, and calls **Start_Game(1 or 2)**, which
   pushes the tactical mode on top of the galactic one (the galactic mode is suspended, not destroyed).
   While the worker runs, the main thread runs its own small loop: load-dialog progress, network pump
   and load-progress packets, present. After the worker ends: GOM post-load (0x2a8730, object combining
   0x29dc30), audio listener set, MP waits for all peers, then FS +0x34 = 0 and the dialog closes (SP may
   wait for the Begin button). [M]
3. **Battle:** only the tactical mode ticks; the global frame keeps counting; the galactic mode's own
   frame and its objects are untouched.
4. **Return:** in the tactical common tick (0x3639d0 tail), when the victory monitor (mode +0x28)
   reports the battle finished and a parent mode exists (0x28aa00 = next stack element): surviving units
   are transferred back into the parent's GOM (0x2a6710, 0x2a6840), the result is recorded (0x28f520),
   **the tactical mode is popped** (0x28f320 → galactic resumed), battle results are applied to the galaxy
   (0x28bf10: production re-evaluation, retreats, corruption), story notifications fire on the galactic
   story (+0xa0: won/lost per player), the galactic GOM runs its post-battle step (0x2b59b0), and the next
   pending conflict, if any, is started (0x28f590). [H]

What persists across modes: PlayerList and players (credits, AI player containers per mode), the
synchronized RNG stream (not reseeded on push), the global frame, the galactic mode object and its GOM
(units are copied into the tactical GOM and survivors copied back), story/scoring managers, all XML type
databases. What is per mode: the GOM(s) at mode +0x18/+0x20, victory monitor, retreat coordinator,
scenes and camera, FOW, the mode frame counter. [M]

### 2.5 Load screens [M]

Every long load (new campaign, battle, skirmish, save game) uses a GUI dialog plus `LoadingThreadClass`
/ the "LoadThread" ThreadClass (SUBSYSTEMS threading section). The sim does not tick during the load
because FS +0x34 is set. The port can do the load on a job thread with the same rule. Quick-load
command-line paths (0x68b20 map, 0x69040 recording) bypass the dialog and draw `Splash.TGA`.

---

## 3. Per-tick logic order inside a mode

### 3.1 GameModeManager::Update 0x28d400 [H]

Skipped entirely while an out-of-sync condition is flagged (0x140b0a355).
1. Optional countdown at GMM +0x1f0; on reaching 0 → 0x28a4d0.
2. `TheGameScoringManagerClass::Service` (scoring Lua script service).
3. **Current mode tick** (vtable +0xb0) with the global frame.
4. Vote manager; resolve the MP "PENDING_TACTICAL_BATTLE_VOTE" when all votes are in (posts a
   ResumeGameEvent).
5. MP galactic battle-vote timeout (uses wall time, see §4.4).

### 3.2 Mode-specific tick (first part) [H]

**Galactic (0x4b2ef0):** 0x4b2600; then the **common tick first** (§3.3); cinematic manager step;
**income** when the mode frame reaches the next income frame (interval = round(30 × a galactic
constant); per player, see GALACTIC_ECONOMY.md); a per-planet random value from the synchronized RNG;
HUD message 0x3d; victory-monitor event 10 per player; every 30 frames victory event 8 per player and a
one-shot object action.

**Land (0x3bb440)** and **Space (0x4d95a0)**, same skeleton:
1. Cinematic manager step (if active).
2. **Per AI player** in the mode's AI list (land +0x350, space +0x348): tactical AI data service
   (0x527dd0) [M].
3. Reset of the movement/pathfinding scratch arrays (0x59d270 on a land singleton or on 0x41a240's
   object) [M].
4. Land only: weather system (0x46a0c0, uses the synchronized RNG) and hero-clash manager (0x56ce20).
5. Mode frame counter (land +0x410, space +0x3c0) += 1; with the timed victory condition
   (0x140b27dcc == 7) end the game when it reaches 30 × limit (0x28ee00); countdown at +0x414/+0x3c4.
6. Victory-monitor event 0x11 per player.
7. **Common tick last** (§3.3).

### 3.3 Common tick `GameModeClass` 0x3639d0 (F = mode frame +0x10) [H for order]

1. Refresh when a GMM-level setting changed; recompute a scaled float at mode +0x230.
2. Per-player type-manager update (0x4098d0, land only).
3. Tactical: hologram speech queue (0x364b20) and ambient SFX (0x364920).
4. Tactical with FOW (vtable +0x240) and no active setup phase: **fog of war**: for each player whose
   id ≡ F (mod 16), update that player's FOW grid (0x4c1670; fade table recomputed from the land/space
   fade time); then the fogged-model manager (+0x2a0). So each player's FOW updates every 16 ticks,
   staggered by player id. [H]
5. Object tracking tree (vtable +0x238 → 0x539b20).
6. Clear the main scene's per-tick list.
7. Per player: roll the per-tick stat (+0x78 = +0x7c, +0x7c = 0).
8. **GOM service** for mode +0x18, then for mode +0x20 (0x2be640, F): spatial tree maintenance, **every
   object's service in object-list order** (0x3a6b80: behaviours in reverse slot order when due,
   damage-over-time, Lua object scripts, locomotion — see OBJECT_RUNTIME.md), deferred deletions,
   frame-scheduled spawns, then conflict detection.
9. Max of player field +0x84 into mode +0x9c.
10. Two mode-owned systems via vtable +0x230 / +0x228 (land: 0x50a780, 0x426eb0) [L].
11. Setup-phase manager (+0x1f0, "end_setup").
12. Retreat coordinator (+0x30) update, then **victory monitor** (+0x28) — both skipped while 0x140b15490
    is set.
13. Bombing-run manager (+0x38), planetary-bombard manager (+0x40), income-stream abilities (+0x298).
14. Tactical without parent: 0x3615d0; countdown at +0x2b4 → victory event 0x15.
15. External hologram (Bink speech) bookkeeping.
16. Battle-end handling (§2.4 step 4).

### 3.4 Where AI, Lua, physics, combat, FOW and story run [M]

| system | where it runs in the tick |
|---|---|
| object behaviours, movement/locomotors, combat, abilities | inside the GOM service (§3.3 step 8), per object |
| collision | signal-driven from object movement (CollisionSystemClass is a SignalListener) [L] |
| tactical AI data | mode tick §3.2 step 2 |
| AI planning / execution (Lua) | PlayerList update T7 (per player, AI container +0x360 → current mode's AI) and its execution system (`AIExecutionSystemClass::vf2` 0x526590, 90-frame finder cadence PHASE2_ANSWERS §11) |
| Lua script threads | not centralized: each owner services its scripts with `LuaScriptClass` service 0x247a90 — object scripts (GOM service), AI execution/planning (0x4d5a70, 0x5f8310), GameScoring (§3.1 step 2), story plots (T9, gated by `ServiceRate`/`LastService` in sim seconds), the global script (T8) |
| FOW | common tick step 4 |
| story | T9 after the mode tick and the player update |
| scheduled commands | before the tick (SP, step 10) and at T2 |

---

## 4. Determinism

### 4.1 RNG [H]

- **Algorithm (both instances):** 32-bit LCG `s ← s × 0x41C64E6D + 0xBDF` (wrapping); output
  r15 = `(s >> 10) & 0x7FFF`.
  - **float(lo, hi)** (0x1ffbb0): swap if lo > hi; one step; `lo + (hi − lo) × (r15 / 32767.0f)`.
  - **int(lo, hi)** inclusive (0x1ffb40): swap if needed; return lo if equal; range = hi − lo, capped
    at 0x7FFE; mask = (smallest power of two > range) − 1; step repeatedly until `r15 & mask ≤ range`;
    return lo + value.
  - **percent(p)** (0x1ffdb0): p = 0 → false and p = 100 → true **without** a draw; otherwise step until
    `(s >> 10) & 0x7F ≤ 100`, true if that value < p.
- **Synchronized instance** (0x140a13e24): the only RNG the simulation may use (behaviours, targeting,
  death, abilities, weather, galactic tick, Lua `GameRandom` and `GameRandom.Get_Float`).
  - Seeded **once per session** in Start_Game when the stack is empty: from GMM +0xe0, or +0xe4 when the
    +0xe8 flag is set ([L] replay/recording seed).
  - GMM +0xe0 comes from `QueryPerformanceCounter` (low 32 bits) when a single-player game is set up
    (0xc7680, 0xef700), and from the host's pre-game packet in MP ("Other player is host - applying random
    seed", 0xc8690). MP Start_Game (0xc7ca0) also writes it directly.
  - Pushing a tactical mode does **not** reseed; the stream continues from the galaxy.
- **Free instance** (0x140a13e20): seeded with QPC at boot; used by audio (SFX/music/speech choice),
  GUI, debug overlays, particles and render effects (0x1a5xxx). Must never influence sim state.
- **Lua `GameRandom.Free_Random`**: in MP it logs a warning and falls back to the synchronized RNG; in
  SP the no-argument form draws from the **synchronized** state (int 0..0xFFFF) and the ranged forms
  draw from the free state. [H] (0x540770)
- Debug: when bit 0 of 0x140a13e10 is set, synchronized draws are logged to `_RandomHistory.txt`
  (0x1ffdf0).

### 4.2 Draw order [H for structure]

Randomness is consumed in tick order: scheduled commands (T2) → GMM update: scoring Lua → mode tick
(land: AI data, weather; galactic: income/planet values) → common tick (FOW, GOM object services in
object-list order, behaviours in reverse slot order, victory monitor, bombard managers) → PlayerList
(AI) → story. The port must keep single-threaded semantic order for every synchronized draw (the
determinism rule in CLAUDE.md): parallel phases must not draw, or must draw from per-index streams that
reproduce this order.

### 4.3 What the synchronized state depends on

Global frame, mode frame, sim time = frame / 30 (float), RNG state, command execution frames. MP
command frames are rounded up to the next multiple of FS +0x0c (command-frame interval: 1 for game
types 1 and 4, 2 or 5 for type 2; MaxAhead FS +0x08 = 2 or 2×interval) (0x27cf90, 0x27d020). SP
commands execute at the start of the iteration in which they were issued (step 10).

### 4.4 Wall clock read inside sim-adjacent code [H]

- GMM::Update: MP galactic battle-vote timeout (`timeGetTime` vs a configured number of seconds) →
  forces the vote. Applies in MP only; the result is sent as an event, so it is synced through the event
  path, but **when** it is sent depends on wall time.
- Battle wall duration (GMM +0x1e8/+0x1ec): statistics only.
- Attract/demo controller (0x22c50) toggles the pause flag from a wall-clock idle timeout. Pause
  affects only when ticks happen, not their content.
- Galactic pause timestamps are in sim seconds, not wall time.
- Nothing in the GOM/object path reads `timeGetTime` that was found in this pass. Animation time on the
  sim side is frame / 30 (RENDER §0 item 7).

---

## 5. Startup sequence [H for order]

### 5.1 Exe start to main menu (0x5d990, 0x4b250)

1. Version GUID; Vista access check (0x7f880); Steam/platform init (0x6a050). Abort on failure.
2. Seed the free RNG (and another QPC value at 0x140a2ab18) from `QueryPerformanceCounter`.
3. **Parse the command line** (0x67d70): `LOCALPORT= MONITOR= WINDOWED FULLSCREEN MODPATH= STEAMMOD=
   FALLBACKPATH= LOGFILE= RANDOMLOGFILE= SYNCLOGFILTER= SCREENWIDTH= SCREENHEIGHT= BCAST2..4= PROFILE=
   LOWRAM FPSCAP= MULTI SAFEMODE LANGUAGE= DEEPSYNC` (MODPATH/STEAMMOD mount as they are parsed,
   PHASE2_ANSWERS §14).
4. Locate EaW (FALLBACK_PATH, abort if missing); `SetCurrentDirectory` to the exe folder.
5. **Mount archives** in the order of SUBSYSTEMS.md "Mount order" (EaW MegaFiles.xml + patches, FoC
   MegaFiles.xml + patches, fallback root).
6. Language detection (0x200090) and MasterTextFile language fallback (0x67590); fonts registered
   ("Arial Unicode MS", "EmpireAtWar").
7. Registry keys `Software\Petroglyph\StarWars FOC\Profiles`; single-instance mutex "EAWIsRunning"
   (if already running: focus the other window and exit).
8. `CoInitialize`, hide cursor, `timeGetDevCaps` + `timeBeginPeriod`.
9. Lua wrapper class registration (0x499340), registry profile load (0x21eef0: screen size, volumes,
   options), resolution selection, monitor enumeration.
10. Register window class "TESTWINCLASS", create and show the window.
11. **Main initialisation 0x4b250** (each step logs to `_MemLog.txt`; most failures abort):
    1. Input/device helpers (0x48f20, 0x1d4160, 0x1d25b0, 0x1d3c20, 0x1d4700).
    2. DatabaseConversionSystem, **TheGameConstants** (GameConstants XML), TheAudioConstants,
       MousePointerDataManager, FontManager, DebugWindowManager.
    3. TheGraphicDetailManager (`Data\XML\GraphicDetails.xml`); EmpireAtWar fonts.
    4. **Init_3D_Game** (0x4df10): D3D device, **every shader `.fx`/`.fxo` in the shader search path
       loaded at boot** (RENDER §0 item 9), textures path, mouse cursors, game-play UI textures.
    5. Splash screen (`Splash.TGA`, presented twice), mouse pointer init, keyboard init.
    6. **Lua search paths**: `Data/Scripts/Library/`, `GameObject/`, `FreeStore/`, `Miscellaneous/`,
       `Story/` (scripts are compiled on demand, not at boot).
    7. TheConsole, TheAudio (Miles), TheGameText (MasterTextFile), TheCreditsText,
       TheGUIDialogManager (`GUIDialogs.rc`, `Resource.h`, `GUIDialogs.xml`).
    8. XML databases in this order: SFXEventFiles, TerrainDecalFX, DynamicTrackFX, StarWars3DTextCrawl,
       SurfaceFX, ShadowBlobMaterials, TacticalCameras, RadarMap, UnitAbilityTypes, MusicEvents,
       SpeechEvents, speech conversations, SFX conversations, AnimationSFXMaps, CommandBarComponentFiles,
       Tutorial, CameraFX, **FactionFiles**, TradeRouteLines, TargetingPrioritySetFiles,
       HardPointDataFiles, HeroClash, **GameObjectFiles** (GameObjectTypeManager), **TradeRouteFiles**,
       **CampaignFiles**, SpecialAbility, Movies, LightningEffectTypes, WeatherScenarios,
       MovementClass, DifficultyAdjustments, **AI data** (TheAIDataManager), GameScoring, WeatherModifiers
       (weather system), LightSources, LensFlares, WeatherAudio, BlackMarketItems.
    9. Late managers and GUI dialog classes (0x451cd0 … 0x2a73a0), `Load_Map` hook installed, debug
       fonts, both PlayerLists reset and the default players created in the second list, HUD init
       (0x3d4490).
    Models (`.alo`) and textures are loaded on demand when types or maps reference them, not here. [M]
12. Read an optional text list (path at 0x140a10d40) into the console command list [L].
13. Choose the first screen: quick-load map (0x68b20), quick-load recording (0x69040), command-line
    save/MP save (0x53c10/0x55250), or normally the **"Logo1" movie → "Logo2" → main menu**
    (0x65780 → 0xc58e0, `IDD_MAIN_MENU_DIALOG`, `Main_Menu_Music_Event`); a flag skips the logos.
    The main menu immediately loads the AI-vs-AI attract battle behind it (MAIN_MENU_BACKGROUND.md).
14. Enter the main loop (§1) with an empty mode stack.

### 5.2 Main menu to a running galactic campaign [M]

1. The campaign button (0xc5250, `IDC_MAIN_CAMPAIGN_START`) opens the load dialog and starts
   "LoadThread", which calls **`TheStoryMode::Load_Plots`** 0x28dbe0.
2. Load_Plots: reset story/AI/persistence managers; Lua state reset (0x248c60, 0x248be0); set the game
   type (0x28ab50); **Start_Game(0, campaign)** (new session: FS reset, RNG seeded, default players);
   build the galaxy from the campaign (0x289a90); attach AI players (0x4b0dc0); per-faction story plot
   loading ("TheStoryMode::Load_Plots( %s )"); player setup from the campaign (0x292050, 0x291f70,
   0x292590, 0x291d80, 0x2924d0); GOM setup (0x2b09b0); **`GalacticPathFinderClass::Build_Planet_Graph`**;
   starting forces.
3. The main thread's load loop ends; FS +0x34 = 0; the galactic mode ticks from the next iteration.

---

## 6. Host-loop skeleton checklist for S5 (stub order)

Per iteration: message pump → (SP) flush commands and execute those due ≤ current frame → network →
wall-clock services (audio with listener from the mode, GUI) → pacing test (rate from the speed table /
fast / cinematic; pause and step gate; `last = now`) → at most one tick [Begin_Tick → scheduled commands
→ TreeCull rebuild → GMM update (scoring, mode tick: mode-specific part + common tick) → scene
animation(frame/30) → player list (AI) → global Lua → story → victory/campaign end → mode frame++ unless
paused → End_Tick (CRC)] → if visible: frame delta from ticks done → mode per-frame update → HUD/2D →
render → FPS cap sleep.

Open items (need a capture or more reading):
- The exact UI that sets fast mode (0x2e41d0) and which UI sets the speed indices in single-player.
- Roles of mode +0x20 (second GOM) and the two systems behind vtable +0x228/+0x230.
- ~~The attract/demo controller (0x22c50, 0x22f10): when it is armed and what sets its timeout
  (0x140b26fc8, runtime-initialised).~~ Answered in MAIN_MENU_BACKGROUND.md: armed and started on main-menu
  open; 0x140b26fc8 is `Demo_Attract_Start_Timeout_Seconds`, used as an idle-pause threshold.
- The text list read at 0x140a10d40 after window creation.
