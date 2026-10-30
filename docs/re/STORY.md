# Story / campaign event system (Story_Plots, story events, rewards)

Behaviour recovered from StarWarsG.exe (FoC Steam x64, Ghidra 12.2 export). Prose and tables only.
Confidence: **[H]** read directly in the decompile, **[M]** inferred from structure or sizes, **[L]** guess.
RVAs are image-relative (`0x455db0` = `FUN_140455db0`). Field offsets are on `StoryEventClass` (event)
or `StorySubPlotClass` (plot) and are only there so later RE passes can find them again.

## 0. Objects and ownership

| Object | What it is | Key state |
|---|---|---|
| Story manager | One per **game mode** (galactic, land, space) at mode `+0xa0` [H] | plot table (hash map keyed by CRC of the file name as written), delayed-event queue `+0xa0..0xb0` [H] |
| Plot (`StorySubPlotClass`, ctor `0x52d400`, 0x668 bytes) | One plot XML file | `+0x660` active flag; `+0x638` owning player; `+0x630` story manager; `+0x610` Lua script; `+0x20` event table (hash map keyed by CRC of the UPPER-CASED event name); `+0x58 + type*0x18` per-event-type buckets [H] |
| Event (`StoryEventClass` + 27 subclasses, factory `0x453310`) | One `<Event>` | `+0x38` event type; `+0x3c` reward type; `+0x4c` **triggered**; `+0x4d` **active** (armed); `+0x4e` Multiplayer; `+0x4f` **disabled**; `+0x228` owning plot; `+0x2d8` Inactive_Delay; `+0x2dc` inactivity timer start; `+0x2e0` Branch; `+0x300` Perpetual; `+0x304` activation time; `+0x308` Timeout; `+0x318/+0x320` Prereq groups (names); `+0x330/+0x338` Prereq groups (resolved pointers); `+0x348/+0x350` dependants; `+0x358` XML index [H] |

Story flags (SET_FLAG / STORY_FLAG) live in one global table, keyed by the upper-cased flag name; an
undefined flag reads as `-99999999` (`0x327d00`) [H]. Flags are therefore shared by all plots and players.

### 0.1 Event type numbers [H]

The enum table (`0x451cd0`, strings at `0x86bc10`) is sequential from `STORY_NONE = 0`. Confirmed by the factory
(`0x453310`) and by the hard-coded dispatch numbers below: ELAPSED = 0x10, SPEECH_DONE = 0x1e, TRIGGER = 0x22,
AI_NOTIFICATION = 0x23, FLAG = 0x2c, OBJECTIVE_TIMEOUT = 0x34.

| Value | Type | Class (factory `0x453310`) |
|---|---|---|
| 1, 2, 0x17, 0x31, 0x33 | ENTER, LAND_ON, PLANET_DESTROYED, FLEET_BOUNCED, INVASION_BOUNCED | StoryEventEnterClass |
| 3, 4, 0x11, 0x12, 0x16, 0x24, 0x26, 0x27, 0x28, 0x37 | CONQUER, CONSTRUCT, CAPTURE_HERO, DEFEAT_HERO, REINFORCE, SELECT_UNIT, UNIT_ARRIVED, FULL_STOP, ATTACK_HARDPOINT, GARRISON_UNIT | StoryEventSingleObjectNameClass |
| 5 | CONSTRUCT_LEVEL | ConstructLevel |
| 6, 8 | DESTROY, TACTICAL_DESTROY | Destroy |
| 7 | DESTROY_BASE | DestroyBase |
| 9, 10 | BEGIN_ERA, TECH_LEVEL | BeginEra |
| 0xc, 0xd | DEPLOY, MOVE | HeroMove |
| 0xe | ACCUMULATE | Accumulate |
| 0xf | CONQUER_COUNT | ConquerCount |
| 0x10 | ELAPSED | Elapsed |
| 0x13, 0x14 | WIN_BATTLES, LOSE_BATTLES | WinBattles |
| 0x15 | RETREAT | Retreat |
| 0x18, 0x19 | SPACE_TACTICAL, LAND_TACTICAL | StartTactical |
| 0x1a–0x1c | SELECT_PLANET, ZOOM_INTO_PLANET, ZOOM_OUT_PLANET | SelectPlanet |
| 0x1d, 0x1e, 0x21, 0x32, 0x38, 0x39, 0x3c | CLICK_GUI, SPEECH_DONE, GENERIC, MISSION_LOST, BUY_BLACK_MARKET, GALACTIC_SABOTAGE, OPEN_CORRUPTION | String |
| 0x1f, 0x20 | FOG_OBJECT_REVEAL, FOG_POSITION_REVEAL | FogReveal |
| **0x22** | **TRIGGER** | **base StoryEventClass — no evaluator** |
| 0x23 | AI_NOTIFICATION | AINotification |
| 0x25 | COMMAND_UNIT | CommandUnit |
| 0x29, 0x2a, 0x2b | GUARD_UNIT, UNIT_PROXIMITY, DIFFICULTY_LEVEL | GuardUnit, Proximity, Difficulty |
| 0x2c | FLAG | Flag |
| 0x2d, 0x2e, 0x2f, 0x30 | LOAD_TACTICAL_MAP, CHECK_DESTROYED, VICTORY, MOVIE_DONE | LoadTactical, CheckDestroyed, Victory, MovieDone |
| 0x34, 0x35 | OBJECTIVE_TIMEOUT, CAPTURE_STRUCTURE | ObjectiveTimeout, Capture |
| 0x36, 0x3a, 0x3b | CORRUPTION_CHANGED, CORRUPTION_TACTICAL_COMPLETE/FAILED | CorruptionLevel |

STORY_CORRUPTION_INCREASED (0x3d) has no factory case, so an event of that type is dropped with
"STORY MODE ERROR! Unable to process event of type %s" (`0x4562a0`) [H].

Other enums share the same name→int table (`0x45c3f0`, unknown name → 0) [H]:
`FILTER_NONE…FILTER_FRIENDLY_AND_ENEMY` = 0..6; `GREATER_THAN, LESS_THAN, EQUAL_TO, NOT_EQUAL_TO,
GREATER_THAN_EQUAL_TO, LESS_THAN_EQUAL_TO` = 1..6 (the comparison switches use exactly these cases).

### 0.2 Event virtual slots [H]

| Slot | Offset | Role |
|---|---|---|
| vf2 | +0x10 | Set parameter `i` (0-based Event_Param index) from a list of strings; called once per non-empty Event_ParamN at creation (`0x4562a0`) and by Lua `Set_Event_Parameter` |
| vf3 | +0x18 | **Evaluate** a dispatched game notification (`data1`, `data2`); calls Fire when it matches |
| vf4 | +0x20 | Reset (base `0x45e480`: if the event has ≥ 1 Prereq group, set active = 0 and cancel its timeout; subclasses also restore counters) |
| vf7 | +0x38 | On-activate hook (only ELAPSED `0x4511f0` and OBJECTIVE_TIMEOUT `0x451290` override it: start the timer) |
| vf8 / vf9 | +0x40 / +0x48 | Load / save |

## 1. Arming and self-evaluation (question 1)

### 1.1 Creation state [H]

`0x4562a0` builds an event from its parsed XML: **active = (no Prereq tags)**. Nothing is fired and vf7 is not
called at creation. After all events of a plot are created, `0x530180` puts each event into its type bucket
and calls Compute_Dependants (`0x452d70`), which resolves every Prereq name **inside the same plot only**
(an unknown name logs "Unable to find event %s in event %s, plot %s" and is dropped from its group) and adds
the event to each prereq's dependants list.

### 1.2 What arms an event [H]

An inactive event becomes active only in two places:

1. **Fire of one of its prereqs** (`0x455db0`, dependants loop). After the firing event's rewards run, every
   dependant that is not active and not triggered is checked: if any Prereq group has all its events
   triggered, the dependant becomes active, its activation time = mode frame × seconds-per-frame
   (`+0x304`), its Timeout is registered, and then
   - **if its type is STORY_TRIGGER (0x22) it is fired at once** (recursive call, same stack),
   - otherwise its vf7 runs (ELAPSED / OBJECTIVE_TIMEOUT start their timers).
2. **RESET_BRANCH** re-check (`0x45c470`): same test and same consequences for every event of the branch.

Plot activation, Lua `Activate`, loading and Reset never arm or fire anything by themselves.

### 1.3 Who calls vf3 (self-evaluating vs notified types) [H]

Dispatch (`0x32e5f0` → `0x5306b0`) walks the plots of the mode's story manager in table order and, for active
plots only (`+0x660`), calls vf3 on every event of the requested type bucket that is **active, not triggered,
not disabled** and passes the Multiplayer gate (§4.3). An optional player argument restricts it to plots
owned by that player.

| Type | Driven by |
|---|---|
| STORY_ELAPSED | polled every story service (T9) with story time and a "paused" flag |
| STORY_FLAG | polled every T9 per active plot (`0x52e730`), no data |
| UNIT_PROXIMITY, CHECK_DESTROYED, FOG_OBJECT_REVEAL, FOG_POSITION_REVEAL, OBJECTIVE_TIMEOUT | polled every T9 (`0x3271d0`) |
| STORY_AI_NOTIFICATION | Lua `Story_Event(name [, object])` (`0x69e040`) |
| every other type | the game system that raises it (conquest `0x326c00`, construction `0x326cd0`, fleet arrival `0x327560`, battles `0x32f570`/`0x32a3b0`/…, speech `0x32e290`, …) |
| **STORY_TRIGGER** | **nobody**: no dispatch call uses type 0x22 and its class has no vf3 |

### 1.4 Answer: STORY_TRIGGER without Prereq [H]

A STORY_TRIGGER event with no Prereq is **active from load but never fires on its own**. It fires only when
something calls Fire on it explicitly:

- the **TRIGGER_EVENT** reward (`0x32e960`): looks the name up in **every plot of the story manager, active or
  suspended**, and fires each match;
- **RESET_BRANCH** with Reward_Param2 (fires that event in the rewarding plot after the reset);
- debug/cheat paths that fire by name (the same `0x530770` helper).

Shipped data confirms this: every prereq-less STORY_TRIGGER in FoC is a debug/cheat jump (`JumpA2`, `JumpA3`
in Underworld; `Mission_Five`, `Mission_Six`, … in the Empire/Rebel acts; `Mission_1…13` in the Cheats plots).
A STORY_TRIGGER **with** Prereq fires synchronously at the moment its prereq group completes (§1.2).

`Story_Event` from Lua does **not** fire STORY_TRIGGER events; it dispatches STORY_AI_NOTIFICATION.

### 1.5 Fire (`0x455db0`), in order [H]

1. If disabled (`+0x4f`): do nothing.
2. If `Inactive_Delay > 0` and this is not the forced inactivity call: restart the inactivity timer and **return
   without firing** (§4.4).
3. Call the plot script's `Story_Event_Trigger(<event name>)` **synchronously** (`0x52f920`) and cancel the
   event's Timeout.
4. triggered = 1.
5. Apply the reward (`0x456970`).
6. Story dialog / chapter handling (`+0x230` Story_Dialog and friends).
7. Dependants loop (§1.2), in dependants-list order.
8. Perpetual: triggered = 0, vf4 Reset; if the type is not STORY_TRIGGER, active = 1 again with a new
   activation time (the event stays armed without needing its prereqs again).

Fire does **not** check active or triggered. Anything that calls Fire directly (TRIGGER_EVENT, RESET_BRANCH
Param2) therefore fires an event that is unarmed, or already fired, again: the reward is re-applied and the
dependants are re-checked.

## 2. Plots: activation, suspension, reset (question 2)

### 2.1 Story_Plots file (`0x329aa0`) [H]

Children of `<Story_Mode_Plots>` are read in order:

- `<Lua_Script>X</Lua_Script>` stores X as the pending script.
- Any other tag with text creates a plot from `Data\XML\<text>`. **active = (tag name is exactly `Active_Plot`)**,
  so `Suspended_Plot` (or any other tag) loads the plot inactive. If a script is pending, it is loaded for that
  plot. The pending script is cleared after each plot entry, so a script pairs with the next plot only
  (Empire Act II has no script).
- The plot is stored under the CRC of the text **as written** (case kept). Lookups by name (`0x328370`, used by
  Get_Story_Plot and STORY_ELEMENT) hash without upper-casing, so the case must match [M: the CRC
  routine `0x215a30` is assumed to be case-sensitive].

`<Suspended_Plot />` with no text creates nothing.

### 2.2 Activation [H]

- **STORY_ELEMENT reward** (`0x4624e0`): `Reward_Param1 + ".xml"` → find the plot → active = 1. This is how
  Empire Act II/III/IV are opened (`Empire_ActI_Open_Act_I_01`: STORY_TRIGGER, Prereq
  `Empire_ActI_Holocron_Geonosis_Entry_Completed`, reward `STORY_ELEMENT Story_Campaign_Empire_Act_II`).
- **Lua `StoryPlotWrapper.Activate`** (`0x7243a0`, 19 bytes, not decompiled): plot `+0x660 = 1` [M from size].
- **Lua `Suspend`** (`0x7247a0`, 19 bytes): plot `+0x660 = 0` [M from size].

Activation only sets the flag. No event is re-evaluated, armed or reset at that moment [H]. Consequences:

- Prereq-less events of the newly active plot are already active. From the next dispatch they receive game
  notifications. A prereq-less STORY_ELAPSED starts its clock on the **first T9 after activation** (the
  first evaluation initialises it, `0x454980`). A prereq-less STORY_TRIGGER still does nothing.
- An event of a suspended plot can still fire through TRIGGER_EVENT (all plots are searched) and through a
  cascade inside that plot. Its `Story_Event_Trigger` call still reaches the plot's script.
- While a plot is suspended, nothing is dispatched to it: no polls, no ELAPSED accumulation, no inactivity
  timers, and FLAG conditions are not checked.

### 2.3 Lua Reset (`0x7246e0` → `0x52fc10`) [H]

For every event of the plot: triggered = 0, then vf4 twice. Events **with** prereqs become inactive (and wait
for a prereq to fire again). Prereq-less events stay active. Disabled flags are not touched. The ELAPSED
accumulator and its "initialised" flag are not cleared by vf4 (no ELAPSED override) [M]. A reset ELAPSED event
whose accumulated time already reached its threshold therefore fires on its next evaluation.

## 3. Prereq semantics (question 3) [H]

- The parser (`0x45c5d0`) reads each `<Prereq>` tag as a string list (the generic list parser) and appends it
  as **one group** (`+0x318`).
- Arming test (`0x455db0`, `0x45c470`): **any group** whose events are **all triggered** arms the event.
  Multiple `<Prereq>` tags = OR. Names within one tag = AND. The port's assumption is correct.
- An empty group (count < 1) counts as satisfied.
- Prereqs resolve within the same plot only. Cross-plot names never resolve.
- The test runs only when a prereq fires (or on RESET_BRANCH). It is edge-triggered, not re-checked every frame.
  If an event is reset by RESET_EVENT while all of its prereqs remain triggered, it stays unarmed until one of
  those prereqs fires again.
- List separators: whitespace and commas are both accepted by the generic list parser [M].

## 4. Evaluation order and cadence (question 4)

### 4.1 Where in the frame [H]

- **Game-side notifications are synchronous.** The game system calls dispatch at the moment the event happens
  (conquest, construction complete, fleet arrival, battle won, Lua `Story_Event`, …). Any resulting Fire,
  reward and cascade runs inside that call.
- If the notification targets a story manager whose mode is not the running mode (for example a galactic
  notification raised while a tactical battle runs), it is **queued** ("STORY DELAYED EVENT", `0x32e5f0`).
  The queue is replayed in order at the start of that mode's next T9. Types 8 (TACTICAL_DESTROY), 0x18 and
  0x19 (SPACE/LAND_TACTICAL) are dropped instead of queued.
- **T9 story service** (`0x3271d0`, MAIN_LOOP T9: after the mode tick and the player update), in order:
  1. replay the delayed queue (only when this is the running mode);
  2. dispatch STORY_ELAPSED (time, paused flag). paused = galactic mode and galactic pause counter
     `0x140b1540c` > 0;
  3. for each plot in table order: Lua script service (`0x52f720`, gated by the script's `ServiceRate` /
     `LastService`, which runs even for suspended plots); then, for active plots only: inactivity timers
     (`0x52e7b0`), the STORY_FLAG poll (`0x52e730`), and the plot's timeout processing (indirect call [M]);
  4. UNIT_PROXIMITY for each local-player object;
  5. CHECK_DESTROYED, FOG_OBJECT_REVEAL, FOG_POSITION_REVEAL, OBJECTIVE_TIMEOUT (time, paused);
  6. GENERIC "battle_end_closed" when that flag is set.

### 4.2 Order across plots and events [H for the structure, M for the exact order]

- Plots are visited in the order of the story manager's hash table list. Events of one type are visited in the
  order their plot's event table was walked when the type buckets were built (`0x530180`). Dependants are
  visited in the same order.
- Both tables are MSVC `std::unordered_map`s keyed by a CRC32 (`0x215a30`). The hash is Park–Miller of
  `(crc ^ 0xDEADBEEF) & 0x7FFFFFFF`, ANDed with the bucket mask. Start: 8 buckets, max load 1.0. On growth the
  new count is `max(8, ceil(n/1.0))`, raised to ×8 while the table is under 512 buckets, then rounded up to a
  power of two (`0x325750`, `0x32f610`).
- Insertion: into an empty bucket, append at the end of the list. Into a non-empty bucket, insert keeping the
  bucket's nodes in ascending key order. Rehash walks the list and regroups nodes by bucket (MSVC
  `_Forced_rehash`).
- **The walk order is therefore not XML order.** For parity, emulate this container: plot key = CRC of the
  plot file name as written in Story_Plots; event key = CRC of the upper-cased event name. The XML index is
  kept in `+0x358` but is not used for ordering.

### 4.3 Gates on every dispatch [H]

Event active `+0x4d`, not triggered `+0x4c`, not disabled `+0x4f`, plot active `+0x660`, and
(`0x140b153f8` == 0 or 3, or the event has `<Multiplayer>` set). `0x140b153f8` is a game-type global; 0 and 3
are taken to be the single-player kinds [M].

### 4.4 Inactive_Delay (`0x52e7b0`) [H]

For an active, untriggered, enabled event with `Inactive_Delay > 0`, each T9 starts its timer if needed, and
after Inactive_Delay seconds forces Fire. Every normal match before that only restarts the timer. The event
therefore fires once its condition has **not** happened for Inactive_Delay seconds (tutorial nags).

### 4.5 Cascades within one frame [H]

Yes. Rewards are applied synchronously. STORY_TRIGGER dependants fire recursively, depth-first, inside the
prereq's Fire. TRIGGER_EVENT fires synchronously. RESET_BRANCH is recursion-limited to depth 3
(`0x140b3073c`, "Event resets itself over and over forever"). A non-TRIGGER event armed by a cascade can fire
in the same frame only if its type is dispatched later in the same frame. For example, a FLAG poll later in T9
sees a SET_FLAG made earlier in T9; an event armed during a dispatch loop for its own type is evaluated if it
comes later in that bucket.

### 4.6 Perpetual [H]

Perpetual is applied at the end of Fire (§1.5 step 8). There is no once-per-frame limit: a perpetual event
fires every time a matching notification arrives. A perpetual STORY_TRIGGER with prereqs becomes inactive
after firing, and fires again whenever a prereq fires again with a satisfied group. Without prereqs it fires
only on explicit triggers.

### 4.7 STORY_ELAPSED timer (`0x4511f0`, `0x454980`, `0x465340`) [H]

- Event_Param1 = seconds (float).
- **Armed by prereqs:** vf7 records LastTime = activation time.
- **Prereq-less:** the first evaluation records LastTime = current story time; this happens at the first T9
  while the plot is active.
- Each T9: if not paused, accumulated += now − LastTime; LastTime = now. Fire when accumulated ≥ Param1. If
  time goes backwards, LastTime is reset and nothing is added.
- The clock is the float time passed into T9. Activation time uses mode frame × `0x140b0a344`
  (seconds per frame), so both are mode time [M].
- Time while the plot is suspended does not accumulate, except that a prereq-armed timer in a suspended
  plot adds the gap at its first evaluation after activation [M].
- The accumulator is never cleared by Reset or Perpetual, so a Perpetual ELAPSED fires every T9 after its
  first fire [M]. No shipped FoC event is Perpetual + ELAPSED.
- OBJECTIVE_TIMEOUT uses the same timer code.

## 5. Parameter matching for game-side triggers (question 5)

Names in Event_Param lists are upper-cased at load. A list param holds several names (any one matches unless
stated otherwise).

| Type(s) | Event_Param1 | Param2 | Param3 | Param4–7 | Match rule (vf3) | Conf |
|---|---|---|---|---|---|---|
| SingleObjectName: CONQUER, CONSTRUCT, CAPTURE/DEFEAT_HERO, REINFORCE, SELECT_UNIT, UNIT_ARRIVED, FULL_STOP, ATTACK_HARDPOINT, GARRISON_UNIT | object/planet type names | counts per name (default 1) | Event_Filter (FILTER_*), **default FILTER_FRIENDLY_ONLY** | – | name equal to the dispatched type name. With no object, fires at once. Otherwise filter on the object's owner vs the plot's player, then decrement that name's counter; fires when it reaches ≤ 0 ("Event didn't trigger because filter did not match") | H (`0x4555f0`, `0x4667e0`, `0x455ce0`) |
| CONQUER dispatch | – | – | – | – | name = conquered planet's type name; object = planet (filter = new owner); also dispatches CONQUER_COUNT | H (`0x326c00`) |
| CONSTRUCT dispatch | – | – | – | – | name = built type name; object = planet. If the type is a starbase/ground-base category it also dispatches CONSTRUCT_LEVEL | H (`0x326cd0`) |
| CONQUER_COUNT | count | – | – | – | +1 per dispatched conquest, **no filter**; fires at ≥ Param1 | H (`0x454390`); who counts as conqueror: M |
| CONSTRUCT_LEVEL | planet names | level | 0 = starbase, 1 = ground base, other/absent = any | – | planet in list (and category matches) and planet level ≥ Param2 | H (`0x4543b0`, `0x464760`) |
| Enter family: ENTER, LAND_ON, PLANET_DESTROYED, FLEET_BOUNCED, INVASION_BOUNCED | planet names (empty = any planet) | Event_Filter, **default FILTER_NONE** | unit type names that must **all** be in the fleet | P4 hero/unit names (any one in the fleet); P5 bool; P6 bool "exact composition" (fleet size = P3 count); P7 bool (alternate fleet test) | planet in list, filter, P3, P4 | H structure (`0x454a90`, `0x465380`, `0x451e20`, `0x452170`); P5/P7 meaning L |
| ACCUMULATE | amount | comparison, **default 0 → behaves as GREATER_THAN** | – | – | dispatched credits `op` amount; player-scoped dispatch | H (`0x453f80`, `0x464150`) |
| STORY_FLAG | flag names | value | comparison, **default EQUAL_TO** (ctor sets 3) | – | for each name: flag must be defined (undefined ⇒ skip), then `flag op value`. NOT_EQUAL_TO (4) has no case and never fires | H (`0x454c00`, `0x4659a0`, ctor) |
| WIN/LOSE_BATTLES | count | location enum (2 = EITHER) | first entry FRIENDLY/ENEMY(_ONLY)_CONTAINS + CLASS_* list | P4 planet names | counter +1 per matching battle; fires at ≥ Param1 and (no planets, or battle planet in list). The counter increments even when the planet does not match | H (`0x455a50`, `0x466f40`) |
| BEGIN_ERA / TECH_LEVEL | level (−1 = "next") | – | – | – | level ≥ Param1; with −1, fires on the first change after the first observation | M (`0x454030`) |
| STORY_AI_NOTIFICATION | planet names (object filter) | notification names | – | – | dispatched name in **Param2** (case-insensitive). If Story_Event passed an object, its type name must be in Param1. If no object, Param1 must be empty. Dispatched to the running mode's story, all players' active plots | H (`0x453e20`, `0x463ec0`, `0x69e040`) |
| SPEECH_DONE | – | – | – | – | default Timeout 60 s when none is given | H (`0x4562a0`) |

FILTER semantics (`0x455ce0`) [H]: 0 any; 1 owner is the plot's player; 2 owner is neutral; 3 owner is neither;
4 friendly or neutral; 5 enemy or neutral; 6 not neutral.

## 6. Branches and rewards (question 6)

### 6.1 Reward application [H]

One reward per event (`Reward_Type`, `0x456970` switch on the reward enum), applied at Fire step 5. This is
**after** the script's `Story_Event_Trigger` call and the triggered flag, and **before** dialog, dependants and
the Perpetual re-arm. Reward_ParamN are single strings. Reward_Param_List is a list.

### 6.2 Story-control rewards

| Reward | Params | Behaviour | Conf |
|---|---|---|---|
| TRIGGER_EVENT | P1 event | Fire every event named P1 in **all plots** of the story manager (active or not), in plot order. No armed, triggered or prereq check | H (`0x32e960`, `0x530770`) |
| RESET_EVENT | P1 event, plus each name in Reward_Param_List | In the **rewarding plot**: triggered = 0, vf4 (counters reset; unarmed if it has prereqs). No re-check | H (`0x52fdf0`) |
| RESET_BRANCH | P1 branch, P2 optional event | In the rewarding plot, every event whose Branch equals P1 (**exact, case-sensitive byte compare**): triggered = 0, vf4. Then re-check prereqs of every branch event (§1.2: arms; TRIGGER fires). Then, if P2 is given, Fire event P2 of that plot. Depth limit 3. Disabled flags untouched | H (`0x52fc70`) |
| DISABLE_BRANCH | P1 branch, **P2 required** | rewarding plot only: disabled = (atoi(P2) ≠ 0) for all events of the branch; missing P1/P2 → logged and ignored | H (`0x52ee40`) |
| DISABLE_STORY_EVENT | P1 event, **P2 required**, P3 optional | disabled = (atoi(P2) ≠ 0); rewarding plot only unless atoi(P3) ≠ 0 (then every plot with that name) | H |
| STORY_ELEMENT | P1 plot name without ".xml" | activate plot `P1.xml` | H (`0x4624e0`) |
| SET_FLAG | P1 name, **P2 required** | flag = atoi(P2) (global table) | H (`0x32b960`) |
| INCREMENT_FLAG | P1 name, **P2 required** | flag += atoi(P2) **only if the flag already exists**; otherwise "Attempting to increment undefined flag" and nothing happens | H (`0x3285a0`) |
| RANDOM_STORY | – | builds a random plot (`0x4bfdc0`); this is where the "Event %s, Type STORY_TRIGGER" / "Prereq %s" strings come from (`0x4be210`, `0x4be420`, `0x4bdfa0`) | H |

All other rewards act on game systems (credits `+0x638` player, units, camera, objectives, …). See the
`STORY REWARD -` strings at `0x86e180..0x8711e8`.

## 7. Open items

Data check (2026-10-27, L1/S4 wave 1; shipped FoC and EaW `Data\XML`, no binary reading): no shipped event
uses `<Timeout>`, `<Inactive_Delay>`, `<Multiplayer>`, `<Reward_Param_List>` or STORY_CONQUER_COUNT, so items 1
(beyond SPEECH_DONE's 60 s default), 4 and 5 cannot change shipped campaign behaviour; every literal
`Get_Story_Plot("…")` name and every STORY_ELEMENT `Reward_Param1` + ".xml" matches its Story_Plots entry with
the same case (FoC 19, EaW 55 references, 0 case mismatches), so item 6 is not observable with shipped data; the
only dialog / chapter tags shipped events use are `Story_Dialog`, `Story_Chapter` (0..n), `Story_Tag` and
`Story_Dialog_Popup` (always False) - item 7's fields to trace.

1. Timeout (`<Timeout>`, `+0x308`): registration on arm and cancellation on fire are seen. The expiry handler
   (indirect plot call in T9) was not resolved: does it fire the event, or fail it? [L: fires, given the 60 s
   default for SPEECH_DONE].
2. ~~Lua `Activate`/`Suspend` bodies~~ confirmed from the bytes: plot (wrapper +0x38) +0x660 := 1 / 0 when non-null,
   no return values (STRATEGIC_GAPS §4) [H].
3. ~~Exact T9 time argument~~ = (float)(mode frame) × seconds-per-frame 0x140b0a344; paused = galactic and counter
   0x140b1540c > 0 (STRATEGIC_GAPS §4) [H].
4. Meaning of `0x140b153f8` values (Multiplayer gate), and of Enter-family Param5/Param7.
5. CONQUER_COUNT: whether the conquest dispatch is raised for every faction or only the human player.
6. Whether `0x215a30` (CRC) is case-sensitive. This affects plot-name lookups from Get_Story_Plot and
   STORY_ELEMENT.
7. Story dialog / chapter fields (`+0x230..+0x2b9`) and the Lua dialog setters were not traced.

> **Status 2026-10-30:** The XML fields `Story_Dialog`, `Story_Tag`, `Story_Chapter` and `Story_Dialog_Popup` are now parsed (`engine/lua/src/story_model.cpp:367-374`) and a fired event with a dialog or tag is recorded for the UI (`story_model.cpp:628-630`). Not covered by this note: the +0x230..+0x2b9 layout and the Lua dialog setters.

8. Save/load: ELAPSED saves LastTime, accumulated and initialised (`0x463580`). Plot-level save traced (0x52fe20:
   active flag, owner, every event's own save, plot script state): STRATEGIC_GAPS §4 [H].
