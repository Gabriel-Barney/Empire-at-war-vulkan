# Fix pass, galactic: free-store routes, hero bar input, corruption route tint, capture checks (StarWarsG.exe FoC x64 v1.121)

This is a clean-room behaviour spec. Sources are the Ghidra 12.2 decompilation, capstone disassembly of the FoC and EaW
`StarWarsG.exe`, and the shipped FoC data (`GalacticFreeStore.lua`, `GameConstants.xml`, `TradeRouteLines.xml`,
`CommandBarComponents.xml`, `HeroCompanies.xml`, `UniqueUnits.xml`). Confidence tags:
- [H]: read in code or data;
- [M]: inferred from code that has a gap;
- [L]: a guess.

Addresses are RVAs from 0x140000000. No decompiled code is reproduced.

Related documents: AI_GALACTIC.md §2.5 / §2.8, GALACTIC.md §3–4, PHASE2_ANSWERS.md §5–11,
GALACTIC_REVEAL_CORRUPTION.md §4, HUD_GAPS.md §3, GALAXY_MAP_PRESENTATION.md §4, PROJECTILE_NEBULA_DRAW.md §2, UI.md §4.1.

Implementing modules:
- **ai**: free-store movement;
- **ui**: HUD components and input;
- **apps**: the game host that wires input to the camera and selection;
- **render**: lines, particles and the nebula.

---

## 1. AI free-store moves on "Friendly_Only" routes (module: ai)

### 1.1 Who issues the moves [H]
- **When the Lua script runs.** `GalacticFreeStore.lua` has `ServiceRate = 8`. The per-unit service rate is
  misspelled (`UnitServciceRate`), so `On_Unit_Service` never runs in vanilla galactic (AI_GALACTIC §2.8). In practice
  `FreeStoreService` only recomputes counters. A unit is moved only from **`On_Unit_Added`**, through `MoveUnit` →
  `FreeStore.Move_Object(unit, target)`.
- **How the target is chosen:**
  - heroes: `Find_Custom_Target` (GalacticHeroFreeStore.lua);
  - transports: `Find_Ground_Unit_Target`;
  - all other units: `Find_Space_Unit_Target`.

  Each one tries the faction leader's planet first (MON_MOTHMA / EMPEROR_PALPATINE). It then calls
  `FindTarget.Reachable_Target(PlayerObject, <perception>, "Friendly", "Friendly_Only", p, unit)` with these
  perceptions, in order:
  1. `*_Priority_Defense_Score`, p = 0.1;
  2. `Low_*_Defense_Score`, p = 1.0;
  3. ground only, and only while the unit is not on a planet: `"One"`, p = 0.1.

  The candidates are also gated by Lua checks: `Get_Is_Planet_AI_Usable`, `Can_Land_On_Planet`, force totals against
  `force_target`, and `Enemy_Present == 0` for space.
- **Reachability filter in `Reachable_Target`:** Friendly_Only is level 0. A candidate planet is kept only if the
  reachability flood (0x665160 → 0x3f12e0) from the AI player reaches it under the level-0 request flags (§1.3). The
  flood result is cached per level for at most 10 × FPS frames (AI_GALACTIC_3 §8).

### 1.2 `Move_Object` (0x68f5b0) [H]
1. **Refusals.** The call returns false and does nothing when any of these hold:
   - the game is paused or a linked battle is pending (REMAINING_GAPS §6.4);
   - there are fewer than 2 arguments;
   - the unit is not a free-store object;
   - the destination is invalid;
   - the unit's planet fails 0x3fa420 ("restricted planet");
   - `Is_Unit_In_Transit` is true;
   - the unit is a landed transport and its planet is blockaded (0x3fa370).

   A unit that is already at the destination, or in a fleet orbiting it, returns true and nothing happens.
2. **Landed transports** are launched (FleetManagement op 4, then op 0) and executed directly.
3. **Splitting.** If the unit's fleet holds more than one object, the unit is split into its own fleet (op 2, then
   op 0).
4. **The move object.** A `FreeStoreMovementBlockStatus` is created (id 0x814) with these fields:
   - fleet, destination, player;
   - **mode = 0** (field +0x60);
   - signals 9, 10, 11, 0x10–0x14 and 0x1B on the fleet.

   It is appended to the free store's move list (+0x138). Its first `Get_Next_Hop` runs immediately.

### 1.3 Path mode and passability [H unless tagged]
`FreeStoreMovementBlockStatus::Get_Next_Hop` (0x71f710) does **not** use the A* finder of GALACTIC.md §3. It uses the
**AI execution path finder** (the all-pairs matrix, PHASE2_ANSWERS §7–11; engine/ai `AiPathFinder`).

1. **Pause.** If either galactic pause counter (mode +0x478 / +0x490) is > 0, the move waits: +0x41 is set and the
   call returns 0. It retries on the next signal.
2. **Location.** The fleet's parent must be a planet. Otherwise the move finishes (+0x40) and logs "doesn't appear to
   be on a planet". This happens for a fleet in hyperspace when a signal fires. If the planet is already the
   destination, the move finishes.
3. **Path.** `Get_Path(current planet, destination)` (0x690f10) is read from the player's AI execution system. It
   uses the matrix from the last 90-frame recompute.
4. **Friendly-only test** (0x6913a0 with threshold = field +0x60 read as a **float**; mode 0 gives 0.0f):
   - S = the int32 sum (wrapping) of the per-hop weights `weights[0 .. min(1000, n−1))` (0x691250). An empty path gives
     S = −1.
   - The path passes when `float(((uint32)S >> 16) × 100) <= 0.0f`, i.e. when the **high 16 bits of S are 0**.
   - Each hop weight is `node_cost(next planet) × 65536 + hop_distance_term` (PHASE2_ANSWERS §8). The test therefore
     means that **every planet after the start, the destination included, has node cost 0**, and that the summed
     distance terms do not carry past 0xFFFF. The carry needs a total of `(d / HS) × 10 ≥ 65536`, roughly 65 hops of
     100 units, so it does not happen on shipped maps.
   - node_cost = (u32)trunc(perception `Target.EnemyForce.SpaceTotalUnnormalized` + starbase `AI_Combat_Power` when the
     planet is not held by the AI or an ally) / 100 (GALACTIC_ECONOMY §6.2). It is 0 when the perceived enemy space
     force plus the hostile starbase power is below 100.
5. **On failure:** the move finishes and logs "Unable to find a path from %s to %s". **There is no fallback**: no A*,
   no wider mode, no retry. The unit stays at its current planet in its own split fleet. The Lua script does not
   re-issue the move, because `On_Unit_Added` fires only once and the per-unit service is dead. The finished move is
   pruned by the unit hook (0x68fde0), so `Is_Unit_In_Transit` becomes false again.
6. **Orbit check on the next hop.** path[1] must have an **orbit owner** (0x3f9270) that is either null or exactly
   this player. **Allies count as "other"**. The orbit owner is computed as follows:
   - with a starbase (+0x148 > 0): the planet's owner;
   - otherwise: the owner of the first orbiting fleet with a fleet state below 3 whose flag test 0x3a9760 is false. If
     every such fleet passes 0x3a9760, it is the owner of the first one. 0x3a9760 reads object flag +0x3A0 bit 1 or a
     fleet/team sub-test, probably "transient / in transit" [L].
   - If the check fails, the move finishes silently.
7. **Issuing the hop.** A fleet-movement transaction (fleet id → path[1] id) is built and executed directly (not
   queued), and the call returns 1. Arrival signals run `Get_Next_Hop` again, so **the path is re-queried at every
   hop** and can change when the matrix recomputes in between.

**Passability under Friendly_Only** (combined effect):

| planet (intermediate or destination) | passes? |
|---|---|
| own / allied, no perceived enemy space force ≥ 100 | yes [H] |
| neutral or unowned, no perceived force ≥ 100 | yes: node cost 0 because there is no hostile starbase. The `Reachable_Target` target filter still requires the **destination** to be held by the player or an ally (flood flag 4 clear, §1.4) [H] |
| enemy-held with a starbase | no: node cost ≥ starbase power / 100 [H] |
| enemy-held, no starbase, no perceived space force | matrix passes [H]. The flood (§1.4) rejects it as a target and as a transit node for `Reachable_Target`, but a matrix path through it is still accepted by `Get_Next_Hop` [M: only relevant when the matrix picks it] |
| own planet "blockaded" (enemy fleet in orbit) | no: the node cost carries the enemy force; on the next hop the orbit owner is not this player [H] |
| unexplored (not revealed) | edge = −1 in the matrix for non-human AI players whenever either endpoint fails `Is_Planet_Revealed`. Note that this tests the **local human's** reveal state, and auto-reveal makes it true (GALACTIC_REVEAL_CORRUPTION §1.1). Unreachable → no move [H] |
| planet in an active battle | not tested by the matrix. A battle implies a hostile presence, so the node cost and the orbit check normally stop the move [M] |

### 1.4 Reachability flood flags (corrects GALACTIC.md §3, the AI-side line) [H]
The request builder 0x665160(level m) sets the six `Path_Request` flags like this:

| flag | meaning | set when |
|---|---|---|
| 0 | +0x18 neutral / unowned / hidden transit | m == 5 |
| 1 | +0x19 enemy transit | m == 5 |
| 2 | +0x1A skip the danger check | m == 1 or m == 5 |
| 3 | +0x1B impassable transit when safe | m > 2 |
| 4 | +0x1C accept the destination unconditionally | m > 1 |
| 5 | +0x1D fog | left at its default of 1 |

GALACTIC.md wrote "flag2 = flag0 = (m == 5), flag4 = (m == 1 or 5)". That is wrong: flag 2 is `Friendly_Ignore_Threat` or
`Any`, and flag 4 is `m > 1`. The flood refuses outright when flag 4 is clear and the start planet is not held by the
player or an ally (0x4ad9f0). For Friendly_Only, every flag except fog is 0.

### 1.5 Determinism and ordering [H]
- **Lua side:**
  - `On_Unit_Added` order = free-store admission order (the initial fill is in GOM planet order, then
    production/arrival order);
  - `Reachable_Target` makes at most one synced RNG draw per call, and only when there are 2 or more positive
    candidates and p < 1 (AI_TACTICAL §6.2);
  - the candidate order is the target list order.
- **Path:** the Floyd–Warshall tie rules decide (k, i, j loop order, strict unsigned `<`, PHASE2_ANSWERS §9). The
  path is read at hop time from the matrix as of the last recompute: the initial run, then every 90 frames with the
  per-player stagger. A port that double-buffers the matrix must serve the **same matrix version** at the same frame.
- The move list +0x138 is serviced only through signals. There is no per-frame iteration, so its order matters only
  for save and load.
- No pointer-keyed containers are needed. Key moves by fleet id.

### 1.6 Port (ai)

> **Status 2026-10-30:** `FreeStoreMove` exists and is stepped per player on each AI service (`engine/ai/src/galactic_ai_impl.hpp:176`, `step_free_store_move` at `engine/ai/src/galactic_ai.cpp:1450-1492`); the Lua-facing entry is `GalacticAiWorld::free_store_move` (`engine/ai/include/alamo/ai/galactic_ai.hpp:183`). The `Reachable_Target` level-0 flood in the last bullet was not re-checked.

- Add `FreeStoreMove { fleet_id, dest_planet, player, mode=0 }` driven by the arrival and pause events. On each event,
  call `AiPathFinder::get_path(cur, dest)` and apply the high-16-bit test on the wrapping int32 sum of the hop weights.
  Fail with no fallback, apply the orbit-owner check on path[1], then issue the hop directly.
- Use the existing 90-frame cadence unchanged. Do not route free-store moves through A*.
- Galactic `Reachable_Target` needs the level-0 flood (§1.4) as its target filter.

---

## 2. Hero bar input (modules: ui, apps)

### 2.1 Callback table [H]
The command-bar component table at 0xa174e0 has 0x3C5 records of 0x48 bytes:
`{name, component*, +0x10 vf64 cb, +0x18 click, +0x20, +0x28 right-click, …, +0x38 toggle, +0x40 …}`.
- 0x347da0 binds each record to its component (component +0x98 = record, +0xA8 = id).
- **Left click** is ButtonComponent vf65 (0x34cf90). It plays `Click_SFX` and calls record +0x18.
- **Right click** is vf66 (0x350cc0). It plays `Right_Click_SFX` (component type +0x280) and calls record +0x28.

`hero_slot_good00..10` all use **+0x18 = 0x347810** and **+0x28 = 0x347830**. The `hero_slot_evil00..10` slots have
**no callbacks**: clicking an enemy portrait does nothing in either mode. The enemy branch of 0x2f64e0 (§2.3) is
reached only when a good slot holds a hero that fails 0x39a230. Good slots are filled with self or allied heroes
(0x39a2c0), so the branch is effectively dead [M: whether 0x39a230 and 0x39a2c0 differ for allies was not traced]. No slot has a `Right_Click_SFX`, so a right-click is silent.

HUD mouse codes (UI.md §4.1):
- 0 = left down: clears the HUD "double-click" flag (HUD +0x2038);
- 4 = left double, 5 = right double: set the flag to 1 before the button is hit-tested again.

A double-click therefore arrives as a click followed by a second click with the flag set [H for the flag, M for the
second click reaching the button].

### 2.2 Galactic (HUD mode +0x211C == 1)
- **Left click**, 0x3cd1b0 [H]:
  1. **Does nothing** when any of these hold:
     - HUD +0x2A33 is set;
     - the slot has no attached type;
     - attached slot 1 ≠ 0.
  2. Take the **first** object of that type in the mode's per-type list (0x2a87d0). The owner is not filtered [M].
  3. Walk up the parent chain (+0x2B0) to the first object that has a planet behaviour. If the chain ends without a
     planet (hero in hyperspace), do nothing. A hero that is still being built or respawning has no object, so the
     click does nothing [M].
  4. **Centre the galactic camera** on that planet: 0x448c20 sets the galactic camera target (camera +0xC..+0x14) and
     re-aims. **Select / focus that planet** (0x3de570). The hero's fleet is **not** selected, and no planet panel is
     opened by the single click.
  5. **Double-click** (flag set) **and** a planet selected (HUD +0x2150):
     - camera zoom state 0 or 2 → **zoom into the planet** (0x3e4660, the same path as double-clicking a planet);
     - state 1 or 3 → **zoom out** (0x3e48a0; with the advanced planet map open it closes it instead, 0x3ce970).

     Both are gated by the story-manager control lock (0x446220, action 4) [M for the action meaning].
- **Right click:** nothing [H].
- **Hover:** the hover handler 0x2e0c20 accepts slots 0x27E..0x288 when an attached type exists and shows the generic
  type tooltip / encyclopedia pop-up (HUD_GAPS §2.1–2.2: `Tooltip_Delay` 330 ms, `Encyclopedia_Delay` 750 ms). The
  rollover texture is `hero_ring_rollover.tga`, and `GUI_Highlight_Mouse_Over` plays [H].

### 2.3 Tactical (HUD mode 2 = space, 3 = land): 0x2f64e0(hud, slot, isLeft) [H unless tagged]
Attached 0 = the hero object; attached 1 = its parent / container.

**No parent.** The handler uses the hero's behaviour 0x16 sub-object (0x4052d0), probably the selectable leader [L].
The rest is as below, except that there is no camera centring.

**Own or allied hero** (0x39a230 against the local player):
- **Right click:** nothing.
- **Left single click:** select the hero, i.e. replace the selection. HUD +0x27F8 is the callback 0x37210, which calls
  the world input handler 0x3a0e0. The add-variant is +0x2800 (0x37230 → 0x3c100) while HUD +0x2060 is set
  [M: the add / modifier state].
- **Left double click:**
  - hero inside a container: select the container;
  - otherwise: call +0x2820 (0x3a690) with the hero's **type**, or +0x2828 (0x3a8a0) when 0x20e2b0 is true
    [L: select all of this type, with or without a modifier].

  Then **centre the tactical camera** on the hero (0x448c20 → the mode camera 0x2d03d0). The +0x2828 variant returns
  without centring.

**Enemy hero:**
- right click while HUD +0x21AB is clear, or left click while it is set, calls +0x2808 (0x37250) with the hero
  [M: attack-target order; +0x21AB = the swapped / left-click-command mouse option].

The click handler does not depend on the slot number.

### 2.4 Sort order: settles GALAXY_MAP_PRESENTATION §4's [L] tie key [H for FoC]
The comparator 0x3202e0 was read from the disassembly. The entries are **types**, and the sort is MSVC `std::sort`
(insertion sort below 33 elements).
- a goes before b when both `Ranking_In_Category` (type +0x1660) are > 0 and a's is lower.
- On equal ranking: type **+0x11C ascending, unsigned**. +0x11C is written only by the type fix-up 0x3782a0 as
  **CRC32 (standard, table 0xa14d20) of the upper-cased type name** (`_strupr` copy of +0xF8).
- If either ranking is ≤ 0, the pair compares equal.

**The FoC key is therefore CRC32(upper(name)), not the as-written name.** Expected FoC order for the base 24.jpg set:

| slot | type | ranking | key (CRC32 of upper-case name) |
|---|---|---|---|
| 1 | Emperor_Palpatine_Team | 1 | |
| 2 | General_Veers_Team | 3 | 0x0CD15FBF |
| 3 | Boba_Fett_Team | 3 | 0x8D12C16C |
| 4 | Accuser_Star_Destroyer | 3 | 0x9E58A04B |
| 5 | Grand_Moff_Tarkin_Team | 3 | 0xA76122B4 |

The ranking-3 keys sort as 215,048,127 < 2,366,816,620 < 2,656,608,331 < 2,808,160,948.

**Conflict.** GALAXY_MAP_PRESENTATION §4 reads 24.jpg as Emperor, Boba, Piett, Tarkin, Veers. That order matches CRC32
of the **as-written** names, which FoC never computes. The **EaW** `GameData\StarWarsG.exe` has the same comparator at
0x2e8100, but with ranking at +0x14C0 and the tie key at **type +0x3C** [H code, L meaning: no CRC is written there].

Either 24.jpg was taken in EaW, or its reading is wrong. **Port rule (FoC): ties by CRC32(upper-case name)
ascending.** Confirm with capture C3.

**Capture C3 (FoC 1.121, Clusters GC, 1920x1080; portraits identified by their tooltips): neither order above.** The
comparator is right, but the keys belong to the hero's **bar type**, not the `*_Team` type. Right to left:

| bar | seen | bar type: ranking, key |
|---|---|---|
| Empire | Emperor, Boba Fett, Veers, Tarkin, Accuser (Piett) | Emperor_Palpatine 1; Boba_Fett 3, 0x31FC4F9A; General_Veers 3, 0x4B2003E7; Grand_Moff_Tarkin 3, 0x8B291F16; Accuser_Star_Destroyer 3, 0x9E58A04B |
| Rebel | Mon Mothma, Obi-Wan, Millennium Falcon, Sundered Heart, R2-D2, Rogue Leader | Mon_Mothma 1; Obi_Wan_Kenobi 2, 0x1EAE6581; Millennium_Falcon 2, 0xA8F81A7D; Sundered_Heart 3; Droid_R2D2 5; Rogue_Squadron_Space 0 (compares equal, stays last) |
| Zann | Tyber Zann, Urai Fen, Bossk, Silri, IG-88 | The_Peacebringer 1; Urai_Fen 2; Houndstooth 3, 0x85C350D1; Silri 3, 0xA180E7F7; IG-2000 3, 0xE025ED50 |

**Bar type [H, capture; the Zann order was predicted from it before that capture]:** a company = its
`Company_Transport_Unit` when that type is `Is_Named_Hero`, else its first `Company_Units` member when that is; any other
type = itself. The tooltip shows the bar type too ("Millennium Falcon", "R2-D2"). `*_Team` ranks differ and are not used
(Han_Solo_Team 3, Droids_Team 3). Port: `game::hero_bar_type` feeds `HeroEntry::ranking` / `type_name`; the as-written CRC
option is removed. Tests: `alamo_ui_screens_tests`, `alamo_game_session_tests` (real data). 24.jpg's Veers-last reading is
not what FoC 1.121 shows. Files: `tools/probe_mod/fixpass_capture/C3_*.png`.

---

## 3. Route tint during a corruption fade (module: render, fed by apps)

### 3.1 Trade-route ribbons are not tinted by the fade [H]
- **Colour slot.** Each route line instance (TradeRouteLineClass, cloned per route by 0x467640, listed in both
  planets' PD +0x40) picks its colour slot with 0x5c28e0(ownerA, ownerB). The owners are the visible owner (0x50bf10),
  else the last-seen owner (PD +0x9C), else none.

  | condition | slot |
  |---|---|
  | either owner none or `Is_Neutral`, or the two owners' factions differ | 2 Neutral (200,200,200 out / 150,150,150 in) |
  | same faction, multiplayer flag 0x140a7c475 set | 1 Ally if allied to the local player, else 0 Enemy |
  | same faction, faction = `Good_Side_Name` (REBEL) | 3 Good |
  | same faction, faction = `Evil_Side_Name` (EMPIRE) | 4 Evil |
  | same faction, any other faction (UNDERWORLD) | 5 **Corrupt** (255,255,0 out / 128,128,0 in) |

  The "Corrupt" slot is the **Underworld faction**, not the corruption state.
- **When the slot updates.** It is re-evaluated **every planet service** (0x3ffc20, called from 0x3fe1b0; it recurses
  once into neighbours). A slot change rebuilds the line at once (0x5c32a0 → 0x5c2590). There is **no blend between
  slots**, so an ownership change recolours the ribbon on that frame. The ownership change is also the frame the
  10 s corruption gain starts (GALACTIC_REVEAL_CORRUPTION §4.3).
- **The only blend is the camera zoom.** Every planet service calls 0x5c31a0(line, f), where f is:
  - 0 when zoomed out (camera state 0);
  - z while zooming in (state 1);
  - 1 − z while zooming out (state 2);
  - 1 when zoomed in (state 3).

  The colour is `Color_Zoomed_Out × (1 − f) + Color_Zoomed_In × f`. Nothing is recomputed while f is unchanged.
- **Sparkle quads.** 0x5c3360 adds eight sparkle quads at ±0.15…0.89 along the line, size 50, colour × 0.7. They are
  on unless the slot is 2 or f == 1.
- **Quirk [H code, M visible].** The rebuild 0x5c2590 mixes with the weights swapped
  (`Out × f + In × (1 − f)`), and 0x5c31a0 skips when f is unchanged. After an ownership change at a steady zoom, the
  ribbon shows the **other** zoom colour until the zoom next changes. When zoomed out (f = 0) that is
  `Color_Zoomed_In`, e.g. Underworld 128,128,0 instead of 255,255,0. Capture C4.
- **Data quirk.** `Color_Zoomed_In` for Enemy and Good is written "128, 0 0, 255" (comma missing). The parser result
  is not traced [L].

### 3.2 What does fade: the corruption path lines and line particles [H]
**Rebuild 0x3ff230** (planet A): it runs from the gain / removal wrappers, at completion (0x3fc5b0) and after load. It
does not run at the start of an ownership-change transition.

- **Building.** The pass runs only when:
  - A is corrupted or gaining;
  - A's particle-line list PD +0x320 is empty;
  - a render scene exists.

  It walks all planets in category-3 order. For each B ≠ A that is an **A\* graph neighbour** of A (0x3ef2d0: a
  trade route, or distance ≤ `MaximumFleetMovementDistance`):
  - **B corrupted and A idle:** spawn `Corruption_Particle_Line_Name` (`Galactic_Corruption_Line_Particles`) at the
    midpoint of A and B. Record {obj, B, −1.0f} in A's +0x320 and {null, A, −1.0f} in B's +0x320. Log
    "Corruption line between %s and %s".
  - **B corrupted or gaining:**
    - if A is idle, add a **path line** A→B unless A's +0x338 already lists B or B's +0x338 lists A;
    - if A is itself in transition, add it without the duplicate check.
- **The path line** (0x3f4b50, rebuilt by 0x3fce00 when its object is null):
  - endpoints = planet positions + `Corruption_Path_Offset` (0,0,0);
  - texture `Fleet_Hyperspace_Band_Texture_Name` (`w_galaxy_line.tga`);
  - width `Corruption_Path_Width` (16);
  - colour `Corruption_Path_Color` (36,36,0,255);
  - mode argument 1 [M: the additive line mode];
  - stored in A's PD +0x338 as {line, B id}.
- **Removal.** When A is uncorrupted and idle, the rebuild destroys A's line particles and the matching entries in the
  neighbours' +0x320, then A's path lines and the neighbours' path lines that point to A.

**Tint (every planet service of A, 0x3fe1b0)** [H]:
- f is:
  - 1 when idle;
  - `fraction` when gaining and the current type is −1;
  - 1 − `fraction` when removing and the current type is not −1.

  The fraction comes from 0x4b7e20 at the store frame (GALACTIC_REVEAL_CORRUPTION §4.2).
- Each channel is `(int)((float)((int)(c × 255) & 0xFF) × f)` and alpha is 255. The result is set on **every path line
  in A's +0x338**, i.e. the lines **owned by A**. Lines owned by B toward A keep B's factor.
- Because the line is additive, a factor of 0 makes it invisible.

**Which transitions fade:**

| transition | fades? |
|---|---|
| Gain through a wrapper (ability, corruption action 5 s, mission success 5 s) | yes: A's lines to corrupted or gaining neighbours fade in 0 → 36,36,0 over the duration |
| Removal through a wrapper (`Remove_Corruption` ability, Death Star hack 5 s, story `REMOVE_CORRUPTION`) | yes: A's lines fade out to black, then are deleted at completion |
| Ownership change (10 s, Underworld takes or loses a planet) | **no**: no rebuild at begin, so the lines appear (gain) or vanish (removal) at completion at full colour |

**Midpoint line particles:** they get no tint. They are created only between two corrupted planets while A is idle,
and they use `Corruption_Line_Radius` 12.5, `Start_End_Offset` 18 and `Grow_Seconds` 7 [M: the consumer of these
three constants was not traced].

### 3.3 Port (render + apps)
- **Ribbons.** Add a per-route slot from the owners (table §3.1), a zoom blend with the zoom factor f, and sparkles.
  Keep or deliberately drop the swapped-weights quirk once C4 confirms it.
- **Corruption path lines.** Add per-planet path-line lists and the 0x3ff230 build rules. Tint per owning planet with
  f, recomputed per frame from `corruption_transition_fraction`, which is presentation only.

> **Status 2026-10-30:** Route ribbons now take a per-route slot with the zoom blend and sparkles (`engine/render/galactic/src/galactic_view.cpp:875-897`), and corruption path lines are built and tinted per planet (`galactic_view.cpp:958-963`) from `corruption_fraction`, which `engine/render/galactic/src/galactic_snapshot.cpp:307` reads from `corruption_transition_fraction`.

---

## 4. Evidence check: settled vs capture needed

### 4.1 Endor nebula (PROJECTILE_NEBULA_DRAW §2) — module: render
**Settled by the data and the binary [H]:**
- the `Nebula Large Ion` SpaceProp (SpaceProps.xml) has no `In_Background`, `Is_Nebula` = Yes, `Layer_Z_Adjust` −100
  and `Sort_Order_Adjust` −1;
- `_MP_SPACE_ENDOR` places three of them in the **main** scene at z = −100, and the planets are background objects;
- the space frame draws the background scene, then a depth-only clear, then the main scene (SPACE_AND_OPEN §A.5);
- `Nebula.fx` is additive ONE/ONE with no Z write.

The original therefore **composites the nebula additively over the planets**: it brightens them and never hides them.
**The port's "behind the planets" is a deviation.** Keep it only as a deliberate choice.

**Needs a screenshot:** only the visual strength (the n_z⁸ rim, distortion, `LIGHT_SCALE`). Capture C1.

### 4.2 Tactical HUD planet / advisor frame (bottom left) — module: ui
**Settled [H, data]:**
- `Text_Planet_tactical`: EmpireAtWar-Bold 10, outlined, gold 255,212,33, max width 110;
- `Movie_tactical`: an Icon of 200 × 200 that shows the advisor `*_LOOP` Bink with straight alpha (STRATEGIC_GAPS §7.3);
- `b_droid_help_tactical`: help button textures, `GUI_Dial_Switch_SFX`, no tooltip;
- the advisor is a talking-head movie, not a static C-3PO image (HUD_GAPS §6).

**Settled by capture C2 (FoC 1.121, 1920x1080, 1 model unit = 1.406 px; `tools/probe_mod/fixpass_capture/C2_*.png`) [H]:**

| item | capture, pixels (top-left origin) | bone, model units from bottom-left | port |
|---|---|---|---|
| Planet name | centred at (250, 745); "KASHYYYK" spans x 182..318 | `Text_Planet_tactical` (179, 239) = (252, 744) | same |
| Help button "?" | face x 4..50, y 706..738, centre (27, 722) | `b_droid_help_tactical` (19, 255) = (27, 721) | same with the shell; no-shell fallback moved from (23.5, 277.5) to the bone |
| Droid + help hit area | clicks on the dome (45, 662) and on "?" (27, 722) both open the holocron dialog | hit mesh -22..31 x -12..57 = x -4..70, y 641..738 | same (hit box = bone mesh) |
| Talking head | beam tip (74, 670); figure x 180..285, top 406..414, feet 670 | `Movie_tactical` (50, 288) = (70, 675) = **bottom-left corner**, 200 x 200 = x 70..351, y 394..675 | was centred on the bone (x -70..211, y 534..816); fixed |

The 200 x 200 size matches the Bink files (`rebel_trooper_loop4.bik`, `moff_loop1.bik`, `bossk_loop.bik`, `ig-88_loop.bik`).
The rectangle is inferred from the visible hologram (the movie's alpha hides its edges); the `Movie_tactical` bone has no placeholder mesh, so the corner rule comes from the capture.

- **No frame is drawn behind the movie.** It is a blue hologram with a beam out of the droid dome, over the battlefield.
  The dome is the always-visible faction shell model (Rebel blue R2, Empire grey; x 8..80, y 640..686).
- **Visible** only while a story `MULTIMEDIA` reward with `Reward_Param9` = a `*_Loop` movie plays, until
  `STOP_COMMANDBAR_MOVIE` (STORY_TUTORIAL_M01_LAND, M03_SPACE). Silent otherwise; none appeared in a sandbox GC space battle.
- **Numpad − is not a talking-head trigger.** It opens the Droid Log dialog, as the dome and "?" do. The port does the same on
  the galactic map and in a battle with a galaxy (a skirmish has no holocron data, so the key does nothing there).
- Same placement in land (Tutorial 1), space (Tutorial 3) and on the galactic map (Tutorial 3, `Movie_galactic`, also 200 x 200:
  beam tip (75, 670)). The `i_galactic_controls.alo` bone is `Movie_galactic` (51, 288), bottom-left corner as on the tactical bar.
- Port trigger: `Reward_Param9` ending in `_Loop` (any case; the data also holds "" and "-1") starts the loop in whichever HUD is
  up, `STOP_COMMANDBAR_MOVIE` ends it. `Reward_Param10` (mostly 1, sometimes 0 or 3) has no known effect and is ignored [open].
  A head left by a tactical plot ends with its battle [inferred].

> **Status 2026-10-30:** The trigger is implemented: `MULTIMEDIA` with a `Reward_Param9` ending in `_loop` (any case) and `STOP_COMMANDBAR_MOVIE` become advisor cues (`engine/apps/game/src/story_ui.cpp:47-54`) that drive `Movie_galactic` / `Movie_tactical` (`engine/apps/game/app/game_main.cpp:1084-1115`).

- Not captured: a talking head in a GC land battle (tutorial land only; the sandbox battle shows none). Frames after an
  accidental window resize are 0.5% squeezed in x (back buffer 1930 shown at 1920), under 2 px in this area.

---

## 5. Capture list (tools/probe_mod/fixpass_capture/README.md)

| id | item | status |
|---|---|---|
| C1 | Endor nebula over the planet | binary settled; visual strength only |
| C2 | Tactical bottom-left planet name / advisor movie / help button, space and land | done (§4.2): movie anchored bottom-left, no frame |
| C3 | Hero bar order with tied rankings (FoC) | done (§2.4): keys come from the bar type |
| C4 | Ribbon colour after an ownership change at a steady zoom (swapped-weights quirk) | needed |
| C5 | Corruption path lines fading during Remove_Corruption, and their absence during an ownership-change gain | confirm |
| C6 | Hero portrait left / double / right click on the galactic map | confirm |

## 6. Named entry points (this pass)
- 0x71f710 `FreeStoreMovementBlockStatus::Get_Next_Hop`;
- 0x71f9a0 its init;
- 0x6913a0 path-threshold test;
- 0x691250 hop-weight sum;
- 0x3f9270 planet orbit owner;
- 0x665160 reachability request flags;
- 0x3cd1b0 galactic hero click;
- 0x2f64e0 tactical hero click;
- 0x347810 / 0x347830 hero-slot click / right-click thunks;
- 0x3202e0 hero comparator;
- 0x5c28e0 route colour slot;
- 0x5c2590 route line rebuild;
- 0x5c31a0 route zoom blend;
- 0x5c3360 route sparkles;
- 0x3ffc20 route slot refresh;
- 0x3ff230 corruption visual rebuild;
- 0x3f4b50 / 0x3fce00 corruption path line;
- 0x448c20 camera centre.
