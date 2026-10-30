# L1 P4 Lua / story gaps: galactic Activate_Ability filter, BUY_BLACK_MARKET timing, SABOTAGE_STRUCTURE planet (StarWarsG.exe FoC x64 v1.121)

Behaviour recovered from the retail exe (Ghidra 12.2 export in `re_reference/`) for the three items left open in
`docs/PHASE4.md` "L1 P4". Prose and tables only. Read with P4_SIM_SYSTEMS (§1.4, §2.4, §6.1, §6.4), STORY.md (§1.3,
§4.1, §5) and ABILITIES_HARDPOINTS §2.4; this file does not repeat them.

Conventions: addresses are RVAs (image base 0x140000000). Confidence: [H] read directly, [M] inferred from strong
evidence, [L] guess. **probe** = needs an in-game run (Gabriel launches; prepare under `tools/probe_mod/`).
Object offsets: +0x2B0 parent container, +0x298 type, behaviour-slot bytes +0x335 (planet), +0x336 (FLEET),
+0x337 (TRANSPORT); −1 = behaviour absent (PHASE2_ANSWERS, STRATEGIC_GAPS).

Data checked: vanilla FoC `Data\XML` and `Data\Scripts` (`extracted/foc_data_text.zip`).

## 0. Status

| § | Item | Confidence | Port change |
|---|---|---|---|
| 1 | Galactic `GameObjectWrapper.Activate_Ability` argument | [H] (fleet-in-transit case [H] via GALAXY_MAP_PRESENTATION) | Yes: accept a **type name string** as well as a type wrapper; filter only in the fleet branch, on the member's representative type; errors → nothing fires; no filter on the unit branch; returns nothing. Same body for `GalacticTaskForce.Activate_Ability` |
| 2 | Story event BUY_BLACK_MARKET timing and params | [H]; "disabled row cannot be clicked" [M] | Yes: fire at click time from the UI host (story-UI command path), unscoped (all plots), data = item `Name`; drop the routing from the executed `BlackMarketPurchase`. Host should also refuse purchased / unaffordable rows |
| 3 | Story reward SABOTAGE_STRUCTURE planet match | [H] | Yes (small): Param1 is a **game object type name** (case-insensitive), first live object of that type; no map-name match |

---

## 1. Galactic `Activate_Ability` (GameObjectWrapper 0x570670; GalacticTaskForce vf39 0x6cd180)

The galactic binding is named `Activate_Ability_Galactic` in its own error texts. The task-force method 0x6cd180 is
the same code inlined, applied to the task force's fleet object (task force +0x120 → wrapper → object [M: field
meaning]); everything below applies to both. [H]

### 1.1 The argument [H]
- **0 or 1 arguments.** More than 1: script error "invalid number of parameters. Expected 0 or 1, got %d", nothing
  happens.
- The argument is an **object type**, given either as
  - a `GameObjectTypeWrapper` (the wrapper's type), or
  - a **string**: looked up as a game-object type name in the type manager's name map (upper-cased CRC lookup
    0x407c00, so case-insensitive; OBJECT_TYPES §3.2). Unknown name: script error "unknown object type %s",
    nothing happens.
  - Anything else (number, object, nil passed explicitly…): error "invalid type for parameter 1. Expected object
    type.", nothing happens.
- It is **not an ability name.** No ability is selected by the argument: each selected unit gets the
  Ground_Activated trigger 0x3a00b0 (P4_SIM_SYSTEMS §6.1), which tries every style-6 ability of the unit's type.
- Shipped data: `STORY_REBEL_ACTIV_M12_GALACTIC.LUA` calls `death_star_fleet.Activate_Ability("Death_Star")` (a type
  name string: the Death Star unit type), `STORY_TUTORIAL_M04_GALACTIC.LUA` calls `smuggler_transport.Activate_Ability()`
  on the smuggler's parent (its transport), and the FoC galactic AI plans (`AI_PLAN_UNDERWORLD_BLACKMARKET`,
  `_DEPLOYSABOTEUR`, `AI_PLAN_EXPANSIONGENERIC_REMOVECORRUPTION`) call `MainForce.Activate_Ability()` with no argument.
  A task-force call with an ability name (as the tactical plans do) would fail galactically with "unknown object
  type" and fire nothing.

### 1.2 Which units fire [H]
Let O be the wrapped object, C = O's parent (+0x2B0).
1. **C is null** (a fleet in hyperspace transit has no parent, GALAXY_MAP_PRESENTATION §2.1: in orbit the
   fleet's +0x2B0 is the planet, in transit it is null; also planets and free objects): error "%s is not at a planet", nothing.
2. **C is a planet** (C +0x335 ≠ −1):
   - O is not a fleet (O +0x336 = −1), i.e. a unit sitting directly on the planet surface: error "%s is landed. Must
     be in orbit to activate abilities.", nothing.
   - O is a fleet: take O's **direct** contents (0x3965f0 with expand = 0: no look-through into squadrons or
     transports), in content-list order. For each member M:
     - **type filter** (only when an argument was given): M's *representative type* (0x3973b0) must equal the
       argument's type. The representative type is the FLEET behaviour's stored type (squadron type), else the
       TRANSPORT behaviour's stored type (the company type the transport carries), else the TEAM container's type,
       else M's own type.
     - M has TRANSPORT (behaviour 5): fire the trigger for **each unit carried** by M (transport content order),
       at planet C. An empty transport fires nothing.
     - otherwise fire the trigger for M itself at C.
3. **C is not a planet:**
   - C is not a fleet (C +0x336 = −1; e.g. O is a unit inside a transport, or inside a team): **silently nothing**
     (no error).
   - C is a fleet: planet X = C's parent. X null (fleet in transit): error "is not at a planet", nothing.
     Otherwise fire for O at X — or, if O has TRANSPORT, for each unit O carries. **The type filter is not applied
     in this branch** (an argument is parsed and validated, then ignored).
4. Return: **no Lua return values** in every case (the function returns 0 results). [H]

The trigger itself (readiness, accept, black-market gate, apply, Causes_Despawn draw / respawn / despawn) is
unchanged from P4_SIM_SYSTEMS §6.1. Units are fired one after another; a despawn caused by one apply does not stop
the walk (the list was copied first for branch 2; branch 3 re-reads the transport count each step) [H as read].

### 1.3 Port comparison (`engine/lua/src/bind_players.cpp` `galactic_activate_ability`, `bind_ai.cpp` task force)
| Point | Original | Port now |
|---|---|---|
| String argument | type name, case-insensitive; unknown → error, nothing | ignored (no filter, fires everything) |
| Bad argument / > 1 args | error, nothing | ignored |
| Filter scope | fleet branch only, on the member's representative type | also on the self-unit branch, on the object's own type |
| Transport cargo | `transport.Activate_Ability()` fires that transport's cargo only | port fleets hold units directly; the smuggler's `Get_Parent_Object()` must yield something whose call fires only the smuggler's company, not the whole fleet [M: check what the port's Get_Parent_Object returns for a transported unit] |
| Landed unit | error, nothing | nothing (same effect) |
| Return | nothing | a finished BlockingStatus (shipped galactic scripts ignore the result; harmless unless a mod tests it) |
| Task force | same body, same type argument | the name is forwarded to the AI host as an ability name |

---

## 2. Story event BUY_BLACK_MARKET (0x38)

### 2.1 Where it is raised [H]
The **only** caller of the dispatcher 0x3268f0 (type 0x38) is the black-market dialog's row-click handler 0x3caf30,
reached from the GUI button callback 0x347720 (row index from the clicked widget, only while the dialog is open).
0x3caf30(dialog, row), when 0 ≤ row < offer count and the dialog has a buyer (+0x2908 non-null), in this order:
1. build a SpecialAbility event (0x4b4210) and fill it as **kind 4** (0x4b5350) from the clicked offer entry
   (item, price), "only one offer" = (offer count = 1), the stored respawn time, planet id and buyer
   (payload: P4_SIM_SYSTEMS §1.4);
2. **queue** it on the command queue (0x3b01f0); it executes later through the normal command path;
3. copy the offer item's `Name` string (item +0x08) and **dispatch STORY_BUY_BLACK_MARKET** synchronously:
   0x32e5f0(current mode's story manager, 0x38, player = none, data1 = Name, data2 = none);
4. close the dialog (0x3d24b0: hide, clear offers, buyer := null).

So the event fires **at click time, before the purchase executes**, and nothing about the purchase's outcome
reaches it. It is not raised for AI purchases (the AI path never opens the dialog) nor on remote machines.

### 2.2 Params and scope [H]
- data1 = the item's `Name` (as in `BlackMarketItems.xml`, e.g. `Disruptor_Tech`, `Cloaking_Tech`); the String
  evaluator (STORY.md §5) compares it with `Event_Param1` (shipped: `STORY_CAMPAIGN_UNDERWORLD*.XML`,
  `Event_Param1` = item name).
- **No player argument**: every active plot of the running mode's story manager is evaluated (0x5306b0 tests the
  player only when one is passed), not only the buyer's plots. Usual direct / delayed-queue rule (STORY.md §4.1).
- The dialog itself also raises "open_black_market_menu" on the mode's event manager (0x32edd0) when it opens
  (0x3c6680); that is a separate notification, not BUY_BLACK_MARKET [H; its consumer not traced].

### 2.3 Can the purchase still fail after the event? [H, with one M]
The dialog rows are refreshed by the galactic GUI update (0x30c3b0 → 0x3e5aa0, every galactic GUI update [M:
per frame]) and each offer row is:
- **already purchased** (offer flag): disabled, text `TEXT_BLACK_MARKET_PURCHASED`;
- **unaffordable**: disabled and greyed when int(price + 0.5) > int(local player credits + 0.5);
- otherwise enabled, label "$ %d" with int(price + 0.5).
A disabled row does not reach 0x347720 [M: the widget flag set is the one the dialog uses to disable the whole
panel on close; click dispatch for disabled widgets not traced]. So the click handler itself checks nothing, but
the UI pre-filters by **rounded** credits. The event therefore fires and the purchase then does nothing when:
- rounding: e.g. credits 999.6 and price 1000.3 both show as 1000 (enabled), but the executor's test is
  price ≤ credits on the quantized price (q8, COMMANDS §3.2) and fails silently;
- credits drop between the last GUI refresh and the command's execution (spending queued in the same window);
- the executor's guard skips it (a galactic battle pending, P4_SIM_SYSTEMS §1.4 execute guard), or the buyer
  died before execution ("buyer no longer exists").
Already-purchased items cannot be clicked [M], so re-buying (which the executor would charge again) does not
happen through the human UI.

### 2.4 What the port's UI host must do
- In the black-market dialog's buy handler (`GameSession::buy_black_market`): after submitting the kind-4 command,
  raise the story UI notification `BuyBlackMarket` with name = the item's `Name` through the existing story-UI
  command path (`notify_story_ui` → port event → `StorySession::notify_ui` at the command phase), so it is
  recorded and replays. Same frame as the purchase command; it must not depend on the purchase succeeding.
- `StorySession::notify_ui` must stop ignoring `BuyBlackMarket` and dispatch STORY_BUY_BLACK_MARKET to **all**
  plot sets (no player scoping), and the `BlackMarketPurchase` → STORY_BUY_BLACK_MARKET route in
  `notifications()` must go (else it fires twice).
- To match the rows: the host should refuse items already purchased by the local player and items with
  int(price + 0.5) > int(credits + 0.5); it currently submits any offered item.
- Ordering note: the original's story firing (and its rewards) happens before the purchase executes; with the
  port's command-phase delivery both land in the same command phase — the story sees the pre-purchase state only if
  the port event is applied before the kind-4 command [port choice; submit the notification first to match].

### 2.5 GALACTIC_SABOTAGE scope (related, same dispatcher family) [H]
0x327c30 (type 0x39, data1 = structure type name) also passes **no player**: all active plots, for the human
command path and the SABOTAGE_STRUCTURE reward alike. The port scopes the command-path event to the saboteur's
plot sets.

---

## 3. Story reward SABOTAGE_STRUCTURE (0x81) — planet match (reward switch 0x456970)

[H], in order:
1. Param1 or Param2 empty: log "SABOTAGE_STRUCTURE takes two parameters, a planet name and building type", no effect.
2. **Planet**: Param1 is looked up as a **game object type name** (0x407e00: upper-cased, CRC, type name map — case-
   insensitive). Not found: log "failed due to invalid planet %s". Found: the current mode's object list for that type
   (0x2a87d0); empty: log "failed due to non-existent planet %s". Otherwise the **first** object of that type is the
   planet. There is no map-name or display-name match and no check that the type is a planet type (planet objects'
   types are the `Planets.xml` type names; shipped Param1 values `Ryloth`, `Endor` are those).
3. **Structure**: Param2 looked up the same way (an unknown name gives no type and therefore never matches). Walk the
   planet's ground special-structure list (planet data +0x158, list order) for the first structure whose own type
   (+0x298, exact, not the representative type) equals it → executor 0x5b21a0(structure, cost 0, R 0, saboteur none,
   planet, story = 1) (P4_SIM_SYSTEMS §2.4), log "success at planet %s". None: log "failed due to non-existent
   structure %s". Orbital structures (+0x170) are not searched.

Port (`story_session.cpp` SABOTAGE_STRUCTURE): matches the map's planet name first, then the planet type name.
Drop the map-name test; match Param1 case-insensitively against the planet's type name only (first planet row whose
type has that name), and require both params non-empty.

---

## Port-affecting findings
1. **Activate_Ability (galactic and GalacticTaskForce)**: the optional argument is an object type — wrapper **or type
   name string** (case-insensitive); unknown / wrong-type / >1 args → nothing fires. Filter only for a fleet in orbit,
   on each direct member's representative type (squadron / carried-company / team type, else own type); a unit in a
   fleet (or a transport) fires itself or its cargo with no filter; a unit inside a transport or team fires nothing;
   a unit on the surface fires nothing. Returns no values. Task-force calls must treat the argument the same way,
   not as an ability name. [H]
2. **BUY_BLACK_MARKET**: raised by the dialog at click time with data1 = item `Name`, to **all** plots, before and
   regardless of the purchase's outcome; the port should raise it from the buy handler via the story-UI command
   path and stop deriving it from the executed purchase. [H]
3. **Black-market dialog rows**: purchased items and items with int(price+0.5) > int(credits+0.5) are not clickable;
   the port's `buy_black_market` should refuse them. [H for the row state, M for "not clickable"]
4. **GALACTIC_SABOTAGE**: unscoped (all plots) on both paths. [H]
5. **SABOTAGE_STRUCTURE**: Param1 = planet type name (case-insensitive), first object of that type; no map-name match;
   Param2 exact type, ground special structures only. [H]
6. Probe (optional, low value): click an affordable-by-rounding-only black-market row (credits just under the price)
   and confirm the story event fires while the purchase does not — confirms §2.3's [M] and the event-before-execute
   order. Not needed to implement the above.
