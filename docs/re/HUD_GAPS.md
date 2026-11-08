# HUD playtest gaps (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-28 by the RE agent for six gaps seen in port playtests. Clean-room: behaviour, data,
constants and timings only. Addresses are RVAs in StarWarsG.exe. This file adds to **UI.md** (HUD object
0x140b27f60, command bar, layers, tooltip delays) and **ABILITIES_HARDPOINTS.md** / **TACTICAL_COMBAT.md
§2.6** (hardpoint instance fields, per-hardpoint targeting); it does not repeat them.

> **Status 2026-10-30:** All six gaps are implemented in the port: reticles (§1) `engine/ui/include/alamo/ui/hud_markers.hpp:94`; tooltips and encyclopedia (§2) `engine/ui/include/alamo/ui/hud_panels.hpp`; hero bar (§3) `engine/ui/include/alamo/ui/tactical_hud.hpp:135-141` and `engine/apps/game/src/hud_feeds.cpp`; day pips (§4) `engine/ui/src/galactic_hud.cpp:1225`; order buttons (§5) `engine/ui/include/alamo/ui/tactical_hud.hpp:181` and `hud_feeds.cpp:23`; the "35" box (§6) see the note there. Still open per docs/PHASE4_HISTORY.md:93 (now in docs/archive/PHASE4_HISTORY_full_2026-10-30.zip): the in-game probes for the hero click and sort key, the hard-point "disabled" state, and day pips on the advanced map.

Confidence: **[H]** read in code or data and cross-checked, **[M]** read but the intent is inferred,
**[L]** a guess that needs an in-game check.

**Component ids.** The HUD names components by a fixed enum (lookup 0x347b50, table of 72-byte entries
at 0x140a174e0). Neither the string order in .rdata nor the XML order matches the enum exactly. These
ids were pinned from code use and are safe to quote: `i_main_commandbar` 0x000, `i_main_skirmish` 0x0e1,
`c_button00..05` 0x0eb..0x0f0, `good_hero_frame` 0x27d, `hero_slot_good00..10` 0x27e..0x288,
`hero_hb_good00..10` 0x289..0x293, `evil_hero_frame` 0x294, `hero_slot_evil00..10` 0x295..0x29f,
`hero_hb_evil00..10` 0x2a0..0x2aa, `pause_shell` 0x2ab, `story_pending_battle_shell` 0x2ae, `map_shell` 0x2b2.
`balance_pip` is 0x04f [M]. The port should resolve components by **name** and never copy these numbers.

---

## 1. Hardpoint reticles on capital ships (tactical space)

Owner: the tactical world-input object (0x140a60df0; reticle list at +0x868, 32-byte entries
{hardpoint, quad, flash timer, flash phase/size flag}). Builder 0x3ed90, per-frame update 0x3f330,
clear 0x3f220, texture choice 0x3f8d0, hit test 0x3fe80, texture load 0x30010.

### 1.1 When they appear [H]
- **Hover only, not selection.** On every mouse move that is not over the HUD (0x2ee40), the world
  pick (0x36600, §2.2) finds the object under the cursor. The hovered object is then replaced by the
  owner of the reticle under the cursor, if there is one, so the set does not flicker while the cursor
  moves across the reticles. If the result is an object with hardpoints, its reticles are built. If
  nothing is hovered, they are cleared. Selecting a ship does not create reticles. They follow only
  the cursor, for own, allied and enemy ships alike.
- The set is rebuilt only when the hovered object id changes. Hovering the same ship keeps it.
- **Not built** when: the modal targeting controller (0x140b27a10) is active; the object is fully
  fogged for the local player (FOW_Object_Hidden 0x35f470, which is applied only when the object has the
  FOW slot +0x34b); the object is stealthed (0x39b140) and hostile to the local player (0x39a450); or
  the object has no hardpoints.
- The whole set is dropped later when the object becomes FOW-hidden, or when its last reticle is
  removed.

### 1.2 Which hardpoints get one [H]
Every hardpoint in the owner's list order whose type has `Is_Targetable` (type +0x4c), unless the type
is `Is_Destroyable` (+0x4d) and its health is ≤ 0. In other words, **destroyed hardpoints get no
reticle.** One extra rule: for an enemy ship, a hardpoint with the disabled flag (+0x6e) gets no reticle
when the set is built.

During the per-frame update, a destroyable hardpoint whose health reaches ≤ 0 has its reticle removed:
at most one per frame, the last such entry found. When none are left, the set is cleared.

### 1.3 Placement and size [H]
- Centre = the hardpoint's attachment point projected to the screen (0x383580) every frame. The quad is
  a 2D screen-space sprite added to the mode's 2D scene, not a HUD component.
- Size, as a fraction of the screen: width = `HardPoint_Target_Reticle_Enemy_Screen_Size` (0.03) for an
  enemy-owned ship, `..._Friendly_Screen_Size` (0.03) otherwise; height = width × 4/3 (4:3 aspect
  correction); centred on the point. The per-frame update reuses the enemy size for every entry. That
  makes no difference in vanilla, where both sizes are 0.03.
- **Flash:** when an attack order or a targeted ability is issued on a specific hardpoint (§1.6), its
  entry gets a 60-frame timer, with the phase toggling every 3 frames. On "on" frames the quad is drawn
  at **half size**. That gives a 2 s pulse at 10 Hz (frame-based, 30 fps HUD ticks).

### 1.4 Colour by hardpoint health [H]
Tint (RGBA) is recomputed every frame from health fraction f = health / max (clamped 0..1):

| condition | colour |
|---|---|
| disabled flag +0x6e set | 128,128,128,255 (grey) |
| f ≥ 0.66 | 32,255,32,255 (green) |
| 0.33 ≤ f < 0.66 | 255,255,32,255 (yellow) |
| f < 0.33 | 255,32,32,255 (red) |
| no hardpoint (defensive) | 255,255,255,255 |

### 1.5 Texture choice [H for the tables, M for the meaning of "can hit hardpoints"]
GameConstants holds seven lists of pairs "HARD_POINT_<TYPE>, texture" (UI.md §1.5). At HUD init
(0x30010), each list is loaded into a per-hardpoint-type table (13 types, 0..12) of texture lists:
Enemy, Enemy_Tracked, Friendly, Friendly_Tracked, Friendly_Repairing. The two Disabled lists keep only
one texture per type. Vanilla files are `Data\Art\Textures\I_Hard_Point_Reticle_{Engines,Shield_Gen,
Generic,Docking_Bay,Weapons}[_Tracked|_Repair].dds`. Shield generators get Shield_Gen, engines get
Engines, fighter bays get Docking_Bay, weapons get Weapons, and everything else gets Generic.

When a list has several textures for a type, the one picked is index ⌊n·f⌋ clamped to n−1. Vanilla
lists have one entry each.

"Hovered" below means the hardpoint whose reticle is under the cursor (hit test §1.6).

- **Enemy ship, and the local selection can hit hardpoints** (0x41ca0: the selection is not empty and
  either the target's shields are ≤ 0 / suppressed by an ion storm or nebula, or some selected unit
  carries a weapon whose projectile is not blocked by shields): hovered or flashing → **Enemy_Tracked**,
  else **Enemy**.
- **Enemy ship, selection cannot hit hardpoints:** hovered → Friendly_Tracked, else Friendly. Vanilla
  uses the same image for the Enemy and Friendly lists, so only the tint shows.
- **Own/allied ship, not hovered:** being repaired by the local player (hardpoint repair list +0x78) →
  blink between **Friendly_Repairing** and Friendly (Repairing when `global frame % 30 < 15`, so 1 Hz).
  Otherwise, if disabled → **Friendly_Disabled**; else **Friendly**.
- **Own/allied ship, hovered:** f < 1 and not being repaired → **Friendly_Disabled_Tracked**, else
  **Friendly_Tracked**.

### 1.6 Click on a reticle [H]
- Hit test (0x3fe80): the cursor (pixels ÷ screen size) inside a reticle quad's screen rectangle. The
  first live entry in list order wins.
- Any world click resolves the target object as **the reticle's owner** when the cursor is over a
  reticle, and passes the hardpoint along with it.
- **Right-click attack** (the attack branch of the world command, 0x3c220): the attack command carries
  the hardpoint's type id (hardpoint type +0x20). That reticle starts flashing (§1.3). The sim then uses
  this hardpoint as the chosen target hardpoint (TACTICAL_COMBAT §2.6).
- **Targeted special abilities** (ion cannon shot, lucky shot, etc.; ABILITIES_HARDPOINTS §2.9): the
  ability command carries the same hardpoint type id, and the reticle flashes.
- **Own damaged hardpoint** (f < 1, or < 0.99 on the move/attack-cursor path) that is not being repaired
  by the local player, while the selection has a repair-capable unit (object slot +0x38e): the cursor
  becomes the repair cursor (id 0x34, 0x38b60). Clicking issues `RepairHardpoint` (COMMANDS #45) on that
  hardpoint and plays the unit's response.

### 1.7 Port mapping
- **engine/ui** (or the tactical HUD layer) owns the reticle list, hover and hit tests, texture, tint
  and flash.
- **engine/render** draws the 2D sprites after the 3D world, in the 2D-scene pass (UI.md §1.1).
- The attack and repair commands go through the command queue into **engine/objects**, which must accept
  a hardpoint (type id) in the attack event.
- Reticles are presentation only. They never feed the sim CRC.

---

## 2. Tooltips and encyclopedia pop-ups on hover

Two separate systems sit on the HUD object. Their delays are in UI.md §2.3; the rest is below.

### 2.1 Component tooltip (command-bar buttons, small text box near the cursor) [H unless tagged]
- **Trigger:** the hovered HUD component changes (HUD hover handler 0x2e32f0). A new component resets
  the countdown (HUD +0x2144) to `Tooltip_Delay` = **330 ms** and clears the old lines (0x2f4970).
- The HUD per-frame update (0x30c3b0) subtracts the real elapsed milliseconds. When it reaches 0, the
  tooltip lines are built. Hovering a different component restarts the 330 ms; there is no carry-over.
- **Suppressed** while a HUD modal is up (HUD +0x26a0 > 0, e.g. pending battle or pause shells) or the
  tactical-interface-hidden flag is set.
- **Text source:** the component's `Tooltip_Text` list (MasterTextFile keys, looked up through
  TheGameText 0x1fa460). Rules:
  - Entry 0 normally.
  - Entry 1 in tactical land mode (HUD mode 3; corrected cu9g t01, §9.0: HUD mode 3 is land, not space) when
    the component has a second entry.
  - Some groups, such as build or selection buttons and filter tabs, pick entry 0–2 by button state.
  - Unit buttons fall back to `TEXT_TOOLTIP_GUI_UNAVAILABLE` (type not buildable) or
    `TEXT_TOOLTIP_GUI_UNIDENTIFIED_UNIT`.
  - Example: `c_button00` = `TEXT_TOOLTIP_ATTACK` / `TEXT_TOOLTIP_ATTACK_01`.
- **Drawn with** the `tooltip_*` components (`tooltip_back` `i_tooltip_back.tga` with faction frame
  `i_tooltip_rebel_frame` / `_empire_frame`, `tooltip_name` Arial 7, `tooltip_price`, `tooltip_icon`,
  `tooltip_left_text`; layer 10). Lines are wrapped (0x2df0c0).
- **Anchor (0x301d00):**
  - The box centre is placed at the cursor + (half box width + 8 px) in x, and the cursor −
    (half box height + 24 px) in y, i.e. **below and to the right of the cursor**. Pixel offsets are
    computed as 8192/screenW and 3·6144/screenH canvas units.
  - If the right edge would leave the canvas, the box flips to the left of the cursor (by box width +
    16 px).
  - If the bottom would leave it, it flips above (by box height + 48 px).
  - The box is repositioned every frame from the live cursor, so it follows the mouse.
- **Hides** when the hovered component changes or becomes null (cursor leaves the HUD), when a modal
  opens, or when the interface is hidden. There is no fade.

### 2.2 World-object and planet pop-up (encyclopedia) [H for timing, M for layout]
- **Picking (tactical, 0x36600):**
  - A ray from the camera through the cursor is tested against every object in the camera's visible
    set (render-scene list; objects with flag +0x3a7 skipped).
  - Per object: first the precise model collision test (0x38f900). If that misses and the type has
    `Mouse_Collide_Override_Sphere_Radius` > 0, a ray–sphere test is done at the object centre with
    that radius.
  - The nearest hit wins. A garrisoned or container child resolves to its container.
  - So the pick is the **model hull, with an optional sphere override**. It is not a screen rectangle.
- **Picking (galactic):** the galactic scene's own pick (planet and fleet models) first, then a
  ray–sphere fallback over the planets using the same radius field.
- **Delay:**
  - Countdown HUD +0x598 = `Encyclopedia_Delay` **750 ms**.
  - It is `Long_Encyclopedia_Delay` **4000 ms** when the player ticked the gameplay option
    `IDC_DELAY_TOOLTIPS` ("delay tooltips", profile flag 0x140a7c472).
  - The countdown restarts whenever the hovered thing changes or is lost.
  - While it is > 0, hovering does nothing. When it expires, the pop-up is built for the hovered object.
- **Not shown** for objects that are FOW-hidden or stealthed-and-hostile to the local player.
- **Content:**
  - Built from the object's **type**: name from the type's text id (type +0x140), icon `Icon_Name`
    (type +0x1820), cost, class, and the stat lines laid out with GameConstants
    `Encyclopedia_Population_Offset` 11, `_Name_Offset` 68, `_Cost_Offset` 258, `_Icon_X/Y_Offset`
    39/−12, `_Class_Y_Offset` 5.
  - Components `encyclopedia_back` (`e_background.tga`, 262 wide, layer 9), `encyclopedia_header_text`,
    `encyclopedia_text`, `encyclopedia_icon`, `encyclopedia_right_text`, `encyclopedia_center_text`,
    `encyclopedia_cost_text`.
  - Look in the base-game captures (`tools/probe_mod/fixpass_capture/C3_*tooltip*`): the inside is solid
    `e_background` (22 33 73, nothing shows through), text solid 192 192 192, and an 8 px light-blue rim outside it
    at 1080p, for both factions. The layout there is name + class + ability icons, description, then
    "Strong Against" / "Weak Against" icon rows, with no stat rows.
  - Fleets (galactic) use a fleet summary instead and fire the story trigger `fleet_tooltip`.
  - Land forces use the trigger `land_tooltip`. These are tutorial string triggers, not GUI Lua.
  - Hero slots and planet labels open the same pop-up for their type or planet (0x2e0c20).
- **Anchor [M]:** a fixed bone on the active command-bar shell. `encyclopedia_back` `Default_Offset` is
  documented in the XML as "offset from hint bone". It does **not** follow the cursor. The `zoomed_*`
  set is the larger variant.
- **Fade:**
  - Alpha rises at `Encyclopedia_Fade_Rate` **4.0 /s** (0.25 s fade-in).
  - Once shown it stays at least `Encyclopedia_Min_Display_Time` **1.0 s**.
  - After the target is lost it fades out at 4.0 /s, then is destroyed (0x30c3b0).

### 2.3 Port mapping
- **engine/ui** owns both timers, the component text lookup, the anchors and the fades.
- The ray pick belongs with the tactical input layer (**engine/ui** or **apps**). It reads object
  collision data read-only.
- Text comes from the MasterTextFile service (**engine/data**).

---

## 3. Hero bar / hero roster

Builders: galactic 0x311380 (part of the galactic HUD build), tactical 0x31af30 (called from the space
0x318720 and land 0x312b90 HUD builds; corrected cu9g t01, §9.0). Both run **every HUD frame**, so the roster refreshes
continuously. There is no event-driven update.

### 3.1 Components and placement [H]
- **Good set:**
  - Frame `good_hero_frame` (`i_main_hero_frame_rebel.alo`), slots `hero_slot_good00..10`
    (TextButton, `rebel_hero_ring.tga`, rollover `hero_ring_rollover.tga`, build dial `i_button_build.tga`,
    scale 0.8, layer 3).
  - Health bars `hero_hb_good00..10` (11-step `i_bar_health_small*`, Smooth_Bar).
- **Evil set:** the same with `evil_hero_frame` (`i_main_hero_frame_empire.alo`) and
  `empire_hero_ring.tga`.
- At HUD init (0x2fcc10) the **good frame is pinned to the top-right screen corner**, which is cached at
  HUD +0x10e8 = pixel (screenW, 0). The **evil frame is pinned to the top-left** (+0x10dc = (0,0)).
  [H for the corners, M that +0x10e8 is the right edge]
- The frame model's variant is set to min(count, 6) − 1, so the frame art grows with up to 6 heroes.
  Slots beyond the count are hidden. With 0 heroes the frame is hidden.
- Each slot also owns a `p_hero_empire_fx.alo` effect model (HUD +0x2770 good, +0x2788 evil). It is used
  as the "ability ready" glow in tactical (§3.3) and is always hidden in galactic.
- "Good" and "evil" here mean **friendly** and **enemy to the local player**, not Rebel and Empire.

### 3.2 Galactic list (good set only; evil slots, all health bars and fx models hidden) [H unless tagged]
1. Every game object owned by the local player that has a galactic presence pointer (+0x2b0), whose type
   has `Is_Named_Hero` (type +0x13) and a non-empty `Icon_Name`. There is **one entry per hero type**
   (duplicates skipped). Objects for which 0x39a600 is true are skipped [M: probably "in transit /
   not on a planet"]. These entries show a **full** dial.
2. Then, for every planet, each entry in the planet's pending list (+0xb8 → +0x240) whose type is a named
   hero owned by the local player. These are hero types being **built or awaiting respawn** [M]. Entry
   = {type, start frame, end frame}. The radial build dial shows (now − start)/(end − start), clamped
   0..1; end = 0 means full.
3. The list is sorted with a fixed comparator (0x3202e0: Ranking_In_Category ascending, ties by type
   +0x11c; details and the screenshot check in GALAXY_MAP_PRESENTATION.md §4) and filled into
   `hero_slot_good00..10` in order. Extra entries beyond 11 are dropped.
4. Per slot: unhidden; attached type = the hero type; icon texture = type `Icon_Name`; tooltip and
   encyclopedia from the type (§2.2).
5. The frame and all slots are hidden while the advanced planet map is open (HUD +0x2a34) or the list is
   empty.

### 3.3 Tactical list [H for the per-slot fill, M for list contents]
- The input is two parallel lists built by the mode HUD: hero objects present in the battle, plus each
  one's parent or container.
- A hero friendly to the local player (self or ally, 0x39a2c0) goes to the next free good slot; others
  go to evil slots (max 11 each).
- Per slot:
  - Icon = type `Icon_Name`.
  - Attached object 0 = the hero, 1 = its parent.
  - Health bar = hull health fraction (0x396470), or the team/container health (0x396df0) for heroes
    that are part of a team (+0x2b0).
  - Glow fx model shown when the hero has an ability state that is flagged for the current mode: any
    tactical mode flag 2, land flag 4, space flag 5 [M: "special ability ready"].
- Everything is hidden while a HUD modal is up.

### 3.4 Click [L]
No hero-slot-specific click branch was found. The slots use the generic "attached object" button path
(attached object 0 = hero in tactical, hero type in galactic). Expected behaviour to confirm in game:
- Tactical: left-click selects the hero; a second click within the double-click window centres the
  camera, like `st_hero_icon` and control groups (UI.md §4.4).
- Galactic: click selects the hero's fleet or planet and centres the galactic camera.

Hovering opens the type's encyclopedia pop-up (§2.2) [H].

### 3.5 Port mapping
**engine/ui** (galactic and tactical HUD build). It reads ownership, hero flag, icon and health from
**engine/objects** / **engine/galactic** read-only. The production/respawn queue progress comes from
**engine/galactic**.

---

## 4. Galactic "day progress bar" (resource panel)

### 4.1 What drives it [H]
- It is the **fiscal-cycle (income) timer**. There is no separate day clock (GALACTIC_ECONOMY §5).
- Cycle length = round(30 × `Fiscal_Cycle_Time_In_Secs`) = **1350 frames (45 s)** of galactic mode time.
  It freezes during tactical battles and pauses with the mode.
- Each HUD frame in galactic (0x30c3b0), the HUD computes k = ⌊10 × (modeFrame − cycleStart) / 1350⌋,
  clamped 0..9:
  - modeFrame = mode +0x10;
  - cycleStart = mode +0x368, the frame the current cycle started. At each income payout it is set to
    the previous "next cycle" frame, and next += 1350 (0x4b21f0 / GALACTIC_ECONOMY §1.2).
- The bar therefore **fills once per 45 s and resets when income is paid**. At the same moment the day
  counter `Text_Day_Counter` (mode +0x370 cycle count) increments and `Text_Day_Credits` shows the
  income.

### 4.2 GUI component [H for the build and layout, M for the component name]
- **Ten pips**, not a bar. At HUD setup (0x306290) one component (0x04f, almost certainly
  `balance_pip`: TextButton, alternate textures `i_bar_fight_red.tga` / `i_bar_fight_blue.tga`, selected
  `i_bar_fight_flash.tga`, No_Shell, Pixel_Align) is cloned 10 times. Pointers are kept at HUD +0x21c8.
- **Layout:**
  - The first pip sits at the bone **`fight_bar_g`** of `i_main_commandbar` (`i_galactic_controls.alo`).
  - When the advanced planet map is open it sits at bone **`day_clock`** of `map_shell`
    (`planetary_mode.alo`) instead.
  - The bone position is snapped to the pixel centre.
  - Each following pip is placed one pip width further right.
  - Re-laid out on resolution change (0x2fca50).
- **Fill direction: left to right.**
  - Pips 0..k use alternate texture 1 (`i_bar_fight_blue`).
  - Pips k+1..9 use alternate texture 0 (`i_bar_fight_red`) and have their flash state cleared.
  - The current pip k flashes (component flash, `i_bar_fight_flash`) while HUD +0x2968 ≠ 0.
- Shown only in galactic mode (HUD mode 1). Hidden in land and space tactical.

### 4.3 Port mapping
- **engine/ui** galactic HUD.
- Inputs: the galactic mode frame and cycle-start frame from **engine/galactic**, read-only.

---

## 5. Tactical order buttons (attack, attack-move, move, waypoint, stop, guard)

### 5.1 Who controls them [H]
- **The engine, not Lua.**
  - Vanilla FoC ships **no GUI Lua scripts**; `Data\Scripts` has only AI, Evaluators, FreeStore,
    GameObject, Library, Miscellaneous and Story.
  - No vanilla script calls `GUI_Component_Visibility` or `GUI_Component_Enable`. Those functions exist
    in the Lua API but only mods use them (LUA_API.md: used only by the tr33 mod).
  - Story XML can only `LOCK_CONTROLS`, `FLASH_GUI`, `FORCE_CLICK_GUI` and similar.
- **Buttons are never hidden by the selection.**
  - The six `c_button00..05` are bones on the tactical shell `i_main_skirmish` (`i_tactical_controls.alo`).
    They are visible whenever the tactical command bar is.
  - Every HUD frame the land (0x312570) and space (0x317d00) HUD updates (corrected cu9g t01, §9.0) only **enable or disable**
    them. Disabled shows `i_button_disabled.tga`; the button stays in place.

| button | tooltip key | disabled when |
|---|---|---|
| `c_button00` attack | `TEXT_TOOLTIP_ATTACK` | no selected unit owned by the local player has an attack behaviour (obj slot +0x33b) |
| `c_button01` attack-move | `TEXT_TOOLTIP_ATTACK_MOVE` | none can attack **and** none can move |
| `c_button02` move | `TEXT_TOOLTIP_MOVE` | no selected own unit has a locomotor (obj slot +0x333) |
| `c_button03` waypoint | `TEXT_TOOLTIP_WAYPOINT` | as move |
| `c_button04` full stop | `TEXT_TOOLTIP_FULL_STOP` | as move |
| `c_button05` guard | `TEXT_TOOLTIP_GUARD` | none can attack and none can move |

- When controls are locked (mode +0x1f0 → +0x30, set by story `LOCK_CONTROLS` / Lua `Lock_Controls`)
  the scan is skipped, so all six are disabled [M for the flag's meaning]. Enemy and allied units in the
  selection are ignored; only units owned by the local player count.
- In the same pass, `b_special_weapon`, `b_special_weapon2`, `b_reinforcement` and `b_retreat` get their
  own enable rules (superweapon ready, reinforcement availability, retreat allowed). The play-pause and
  fast-forward pair is disabled in multiplayer (player flag +0x3c0) [M for the pairing].
- **Ability buttons** (`special_button_00..47`, `cs_ability_button`) belong to the selection panel
  (UI.md §2.3). Each is shown per ability present in the selection, with the recharge dial; that is the
  only place buttons appear and disappear with the selection [M].
- There is **no formation button** in vanilla FoC. "Spread out" is an ability (UI.md §4.3, slot 25).

### 5.2 Port mapping
**engine/ui** tactical HUD. Drop the Lua-driven visibility for vanilla. Keep Lua `GUI_Component_*` only
as a mod API that overrides the engine state.

---

## 6. The small numbered box ("35") beside the galactic story/advisor dialog [L]

The original has no numbered box attached to the story dialog. `IDD_STORY_DIALOG` (382×519, centred)
holds only a title, an image, a scrolling text listbox and Accept/Close/Cancel buttons. Advisor speech in
FoC galactic is the talking-head Bink in `Movie_galactic` (200×200; STRATEGIC_GAPS §7.3) plus teletype
`SCREEN_TEXT` lines (`tutorial_text_back` / `tutorial_text`). So the box is almost certainly a
**command-bar component that the port shows when the original keeps it hidden or elsewhere**.
Candidates, most likely first:

1. **`Text_Day_Counter`** (Arial bold 8, gold 255,212,33, left-justified, galactic shell): the galactic
   **day = fiscal-cycle count**. "35" is plausible after about 26 minutes (35 × 45 s). In the original it
   sits in the resource panel next to credits, not beside the dialog. **If the number goes up by one
   every 45 s, this is it**, and the fault is placement (bone lookup on `i_galactic_controls.alo`).
2. **`Text_Pop_Cap` / `Text_Planetary_Pop`** (icon `i_icon_planet_pop.tga` plus a right-justified
   number): the population cap. It changes only when planets or structures change.
3. **`objective_icon`** (16×16 `checked_box.tga` with a 6-pt outlined text at +12,−12) of the objectives
   panel (`objective_back`, Default_Offset 383,−180, i.e. right of centre). Shown only when story
   `ENABLE_OBJECTIVE_DISPLAY` / `ADD_OBJECTIVE` have run; it should hold a check or fail mark, not a
   number.
4. A component that should be invisible until used, such as `reinforcement_counter` or
   `garrison_respawn_counter` (`i_button_unit_frame_dark.tga` box plus a number). These are tactical
   only and should never appear in galactic.

**How to settle it (port side):** dump the component name under the box (the port resolves components
by name) and check it against the list above. Watch whether the number ticks every 45 s.

> **Status 2026-10-30:** Cause found and fixed (docs/PHASE4_HISTORY.md:93 (now in docs/archive/PHASE4_HISTORY_full_2026-10-30.zip)): a galactic planet element was showing through the story dialog. The port now blocks galactic hover while a GUI dialog is open (`engine/apps/game/app/game_main.cpp:2952, 3651`, comments "HUD_GAPS §6").

---

## 7. Open items
- Hero slot click behaviour (§3.4) and the galactic roster sort key: in-game probe (click a hero icon
  in galactic and tactical, note the selection and camera).
- Confirm which screen corner the good/evil hero frames use at widescreen (screenshot).
- Item 6: needs the port-side component name.
> **Status 2026-10-30:** The item 6 entry above is closed: the cause was a galactic planet element showing through the story dialog, not a command-bar component (see the note under §6).
- Encyclopedia anchor bone name in the shell models (read `i_galactic_controls.alo` / `i_tactical_controls.alo` bones).

## 8. Named entry points (this pass)
| RVA | role |
|---|---|
| 0x2ee40 | world mouse-move: pick, hover target, build/clear reticles |
| 0x30010 | GamePlayUI init: load reticle texture tables from GameConstants |
| 0x3ed90 / 0x3f330 / 0x3f220 | reticles: build for hovered ship / per-frame update / clear |
| 0x3f8d0 / 0x3f860 | reticle texture choice / health colour |
| 0x3fe80 | reticle under cursor (screen-rect hit test) |
| 0x3c220 / 0x41f70 | attack order with hardpoint (+ flash) / world command dispatch incl. targeted abilities |
| 0x36600 | world pick (precise model test, then `Mouse_Collide_Override_Sphere_Radius` sphere) |
| 0x2e32f0 / 0x2f4970 / 0x301d00 | component tooltip: hover change / reset / draw and anchor |
| 0x2e2910 / 0x305fd0 / 0x305fb0 | encyclopedia: object hover / target set / delay select |
| 0x30c3b0 | HUD per-frame update (timers, fades, day pips, mode HUD updates) |
| 0x311380 / 0x31af30 / 0x2fcc10 | hero roster: galactic / tactical / frame corner anchors |
| 0x306290 / 0x2fca50 | HUD mode switch incl. day-pip clone and layout / pip re-layout |
| 0x312570 / 0x317d00 | land / space HUD update incl. order-button enable rules (corrected cu9g t01, §9.0) |

---

## 9. cu9g: bombing-run button (land tactical)

Written 2026-11-08 (cu9g t01, RE only) for the code tasks cu9g t02 (sim: execute command 28) and t03 (HUD button).
Addresses are RVAs in StarWarsG.exe (FoC x64 v1.121). It builds on L1_S4_REMAINING §3.7 (bombing-run manager,
record layout, service, target test, start, end), which it does not repeat.

### 9.0 Correction: HUD mode numbers [H]
The HUD mode (+0x211c) is set by 0x28b080 from the game mode type (mode vf +0xe0): galactic 0 → **1**, land 1 →
**3**, space 2 → **2**. Checks: the HUD mode 3 branch of the per-frame update 0x30c3b0 calls 0x312570 and then
handles the bombing-run / bombard targeting blobs only for mode type 1 with the land managers (mode +0x38 / +0x40);
the HUD build 0x300c00 calls 0x318720 for mode 2 and 0x312b90 for mode 3; the `b_special_weapon` click 0x346a80
fires the space superweapon for HUD mode 2; the tooltip code 0x2e32f0 gives `b_special_weapon` the bombing-run
tooltip for HUD mode 3. So **0x312570 is the land HUD update and 0x317d00 the space one**. §2.1, §3, §5.1 and §8 of
this file and UI.md §1 (steps 5b, 5c) had land and space swapped; they are corrected in place
(FIXPASS_GALACTIC §2.3 already had it right).

### 9.1 The component [H]
`b_special_weapon`: HUD component enum **0x0e5** (table 0x140a174e0, 72-byte entries; entry +0x18 = left-click
callback **0x346a80**), a bone button of the tactical shell `i_main_skirmish`. Neighbours: `b_special_weapon2` 0x0e6
(place-beacon button), `b_reinforcement` 0x0e7, `b_retreat` 0x0e8, `c_button00` 0x0eb. CommandBarComponents.xml:

| field | value |
|---|---|
| Type | Button, `Disabled` True by default, Scale 1.0, Selected_Alpha, Click_Shift |
| Blank_Texture_Name | `i_button_back_new.tga` |
| Icon_Alternate_Texture_Name | 0 `i_button_command_bar_ion_cannon`, 1 `..._hyper_gun`, 2 `..._station_turret`, **3 `i_button_command_bar_bombing_run.tga`** |
| Mouse_Over / Selected | `i_button_heros_over.tga` |
| Disabled_Texture_Name | `i_button_disabled.tga` |
| Build_Texture_Name | `i_button_build.tga` (the radial dial = the cooldown) |
| Tooltip_Text | 0 IONC, 1 HYPERV, 2 PLASMAC, **3 `TEXT_TOOLTIP_BOMBRUN_BUTTON`**, 4 `TEXT_TOOLTIP_REQUIRES_COMM_ARRAY` |
| SFX | Mouse_Over_SFX `GUI_Highlight_Mouse_Over`, Click_SFX `GUI_Button_Press_2_SFX` |

In space the same component is the station superweapon button (0x317d00: icon 0..2, tooltip 0..2 or 4). **In land it
is always the bombing-run button** (icon 3 and tooltip 3, set every frame); land has no other use for it.

### 9.2 Shown / enabled (land HUD update 0x312570, every HUD frame) [H]
- **Never hidden** in land (0x312570 only disables it): it is on screen whenever the tactical command bar is, like
  the order buttons of §5.1.
- `ready` = controls not locked (0x35f790: mode +0x1f0 → +0x30, story `LOCK_CONTROLS` / Lua `Lock_Controls`) **and**
  the mode has a bombing-run manager (+0x38) **and** 0x494410(manager, local player).
- 0x494410 (available): manager enabled; global 0x140b15490 clear (always clear in the shipped game); not a skirmish
  (0x28afb0) **or** the skirmish option bit 0x140b154bc & 4 (the lobby "special weapons" bit, AI_GALACTIC_3 §1.7);
  record enabled (+0); record available (+1); not in progress (+0x28). The record's available flag is kept by the
  per-frame service 0x495dd0 (L1_S4_REMAINING §3.7: the player's bool list +0x1c8 of `Enable_Bombing_Runs` /
  `Disable_Bombing_Run` entries, empty = allowed in a campaign battle and refused in a skirmish; then ready frame ≤
  now).
- Per frame: disabled := !ready (component vf +0x80); icon := alternate texture 3 (vf +0xa0); dial (vf +0xf0, §9.3).
  No selected / flash / blink state is set; the button does not show "targeting on".
- When not ready and targeting is on (HUD +0x20b2): targeting ends **silently** (blob removed, flag cleared, no SFX).
- So: no record (campaign defender, no Ship_Class 1 bomber in orbit, skirmish faction without
  `Skirmish_Land_Bomber`), a refused gate, a run in progress, the cooldown and locked controls all show the
  disabled texture; they differ only in the dial (§9.3).

### 9.3 Cooldown display: the build dial only [H unless tagged]
No text, number or tint: only the radial `Build_Texture_Name` dial and the disabled texture.
- dial := 1.0 when ready; else f = 0x495250(manager, local player), and f == 0 is shown as 1.0.
- **0x495250** (returns a float in xmm0; the decompile shows `void`, read from the disassembly):
  1. player index out of range or record disabled → 0;
  2. when the player has tactical data (mode vf +0x220): gate 0x527550 false → 0; multiplayer game types
     (0x28af80) or game type 3 (0x28b060) → 1.0;
  3. R = 0x4950f0(record, now); now ≥ R → 1.0;
  4. S = record +0xc (last start frame); R ≤ S, S == −1 or now ≤ S → 0;
  5. else (now − S) / (R − S) (integers converted to float).
- **0x4950f0**(record, now) returns an **absolute frame**, not a count (its only caller is 0x495250):
  record disabled → −1; available and not in progress → now; not in progress → ready frame (+4); in progress →
  now + interval (+8) + int(F), F = the largest remaining flight time over the run's bombers: for each bomber with a
  current locomotor path (0x5569b0), remaining path length from its position (0x5c5530) / its effective max speed
  (0x370f00), bombers with speed ≤ 0 skipped [M: the meaning of 0x5c5530 as remaining length]. "Frames until
  available" for a caller is therefore R − now.
- Record defaults (0x492340): ready −1, interval 3600, last start −1. Campaign init sets last start := now, so the
  dial sweeps from the battle start to the first ready frame. Skirmish init does not set it, so before the first run a
  skirmish button is disabled with a **full** dial (rule 4). After a run (start sets last start := now; the end sets
  ready := now + interval) the dial runs continuously from the run's start, through the flight, to ready.
- On the start frame itself (now == S) the dial shows full for one frame (rule 4 → 0 → 1.0).
- [L] Whether the dial at 1.0 draws a complete `i_button_build` overlay or nothing (draw path of vf +0xf0 not read);
  see §9.9.

### 9.4 Entering and leaving targeting [H unless tagged]
Triggers:
- Left click on `b_special_weapon` → 0x346a80: HUD mode 2 (space) → space superweapon; otherwise → **0x2e7950(HUD, 0)**
  (toggle). There is no availability check in the callback; [M] a disabled button does not run its callback.
- Hotkey slot 22 "super weapon" (default Ctrl+W, UI.md §4.3) → 0x63f00 case 0x16 (when mode vf +0xf0 is true [M:
  tactical mode]): first the local player's special-weapon objects (0x35e4d0): one with behaviour 0x2f, a weapon
  record and readiness 0x4914e0 ≥ 1 fires (0x491b40) and ends the key; one with behaviour 0x37 ends the search;
  then, in land, if 0x494410(manager, local) → 0x2e7950(HUD, 0).

Toggle **0x2e7950(HUD, cancel)**, HUD +0x20b2 = targeting flag, +0x20c0 = targeting blob:
- cancel = 0, flag off → on: flag := 1. A blob already present → stop. No manager (+0x38) → stop. Play the record's
  bomber type `SFXEvent_Bomb_Run_Select_Target` (type +0x1360; Y-Wing `Unit_Select_Y_Wing`, TIE Bomber
  `Unit_Select_Tie_Bomber`, Skipray `Unit_Select_Skipray`) when the player is the local one (0x4953d0). Then, only in
  land and when the mode has a terrain blob list (mode +0x338): material = index of the local faction's
  `Bombing_Run_Shadow_Blob_Material_Name` (faction +0x26d8: Rebel `Bombing_Run_Rebel`, Empire `Bombing_Run_Empire`,
  Underworld `Bombing_Run_Rebel`; ShadowBlobMaterials.xml, ALPHA) in the blob material table (0x140b2f170); index < 1
  → no blob (targeting stays on without one). Else create an `alTerrainShadowBlob` (0x12f3e0, DECORATIONS.md §3),
  material, colour white, intensity 1.0, +0x94 = 1, hidden, add it to the mode's blob list (0x136b00).
- cancel = 0, flag on → off (second click or second Ctrl+W), or cancel = 1 with the flag on: flag := 0; remove the
  blob from the list (0x136ba0) and destroy it; play the local faction's `SFXEvent_Bombing_Run_Cancelled`
  (faction +0x2000, `RHD_Bombing_Run_Canceled`). cancel = 1 with the flag off does nothing.
- cancel = 1 caller: the start of a drag of a HUD component (0x346de0).
- Silent ends (no SFX): not ready any more (§9.2), the HUD mode switch 0x306290, HUD reset 0x3035c0.

### 9.5 While targeting [H unless tagged]
- **Cursor** (0x2f5c80): after the higher-priority cases (HUD drags and dragged objects, hovered build / unit
  buttons), when the flag is on and the cursor is not over a HUD component (0x2fbc70): pointer id **0x21 =
  `POINTER_BOMBING_RUN_TARGET`** (MousePointers.xml: `BOMBPOINTER_00.TGA`, Hot 16,16, Anim_Frame_Delay 4). The same id
  serves orbital-bombardment targeting (HUD +0x2ae6). There is no "invalid" cursor; validity is shown by the blob.
  Over a HUD component the normal HUD cursor applies.
- **Blob** (0x30c3b0, HUD mode 3 branch, every HUD frame, mode type 1 with a manager): colour = green (0, 1, 0, 1)
  when 0x494540(manager, local player, point) passes, else red (1, 0, 0, 1) [M: RGBA order of 0x1f9340]; position =
  the HUD's ground point under the cursor (HUD +0x26dc..+0x26e4) [M: writer not traced]; size = the local faction's
  `Bombing_Run_Blob_Size` (faction +0x26f8; 100.0 for Rebel, Empire, Underworld) as both width and height (full side,
  not radius; DECORATIONS §3.2), rotation 0, intensity 1.0; then shown. It is a terrain-draped decal.
- **Target test** 0x494540 (L1_S4_REMAINING §3.7): the point not fogged for the player (force 1); no active
  BASE_SHIELD of any owner covering it; no object not owned by the player with `Bombing_Run_Prevention_Radius`
  covering it.
- **World input**: the drag-select box does not start while the flag is on (0x30fc0, 0x39380, 0x39420 test it).

### 9.6 The click (HUD mouse handler 0x2f6c30) [H unless tagged]
- **Left button up** (handler code 1), cursor not over a HUD component (over one, that component's click runs and
  targeting stays on): P = the ground point under the cursor pixel (0x3bb2d0(mode, x, y)) [M: screen → terrain
  pick]. If the mode has a manager and 0x494540(manager, local player, P) passes:
  1. build a BombingRun event (0x524560, type 28), point := P (0x5247a0);
  2. OutgoingEventQueue::Add (0x3b01f0; COMMANDS §4: executed at a later frame, also in single player);
  3. play the bomber type's `SFXEvent_Bomb_Run_Incoming` (type +0x1370; `Unit_Bombing_Run_Y_Wing`,
     `Unit_Bombing_Run_Tie_Bomber`, `Unit_Attack_Skipray`) for the local player (0x495360);
  4. remove and destroy the blob, flag := 0 (no cancel SFX).
  There is **no availability check** at the click. An invalid point plays the GUI negative feedback SFX (0x140b301c8)
  and **targeting stays on**. The click is consumed either way.
- **Right button up** (code 3), not over a HUD component: blob removed, local faction `SFXEvent_Bombing_Run_Cancelled`,
  flag := 0. The handler returns 0, so the right-up still reaches the world as a normal right-click order [M].
- Orbital bombardment uses the same flow with HUD +0x2ae6 / +0x2ae8, test 0x528a70, event 54 (ctor 0x52afc0) and
  `Bombardment_Blob_Size` × 2 for the blob.

### 9.7 Command 28 BombingRun [H]
- `BombingRunEventClass`, 0x58 bytes, type 0x1c, scheduled. Issuer = the event's player (+0x24, wire header P).
  Payload: point x, y, z at +0x48 / +0x4c / +0x50; pack 0x524650 and unpack 0x524590 use quantizer entries 1, 2, 3
  (COMMANDS §3.2).
- **Execute 0x5246d0**: round-trip x, y, z through q1..q3 (0x23bc10); |z| < 0.0001 → 0; manager = mode +0x38, none →
  nothing; if 0x494540(manager, issuer, P) passes → start 0x492d60(manager, issuer, P). The start checks again:
  manager enabled and target valid (silent), then 0x494410 available, else the log line
  "Warning: Player %S(%d) cannot intiate a bombing run yet" and the GUI negative feedback SFX when the issuer is the
  local player. Otherwise the start of L1_S4_REMAINING §3.7 (available := 0, in progress := 1, last start := now,
  three bombers on the edge-to-edge path, markers, crosstalk SFX). No return value, **no RNG**.
- Command 28 has one sender, the HUD click of §9.6. Lua `LandTaskForceClass.Bombing_Run` (L1_S4_REMAINING §2.10)
  calls the start 0x492d60 directly and the AI perception reads 0x494410; neither goes through the command.

### 9.8 What the port needs (t02 / t03)
Port state at dev 7c6ea635: `TacticalBattle` has `start_bombing_run`, `bombing_run_available`,
`bombing_run_target_valid`, `bombing_run_frames_until_available` (`engine/apps/sim/src/tactical_battle_l1_strikes.cpp`;
start checks validity and availability, no RNG). The command codec has `BombingRun = 28` with a q1..q3 point
(`engine/commands/src/event_types.cpp` `kPoint`); `EventData::f` returns the dequantized value, which is the
original's round trip, and dequantize snaps |v| below one step to 0, which covers the |z| rule. `ui::Pointer::
BombingRunTarget` and the cursor's `Targeting::BombingRun` exist (`engine/ui/include/alamo/ui/mouse_cursor.hpp`);
`engine/render/include/alamo/render/jam_fx.hpp` already draws a ShadowBlobMaterials blob. `TacticalBattle::
handle_command` has no case for 28, so the command is Unhandled.

**t02 (sim)**
- [ ] `TacticalBattle::handle_command`: `T::BombingRun` → land only (space has no manager: nothing happens); P =
  (`f("x")`, `f("y")`, `f("z")`); if `bombing_run_target_valid(player, P)` → `start_bombing_run(mode, player, P)`.
  Outcomes: invalid point or no record → silent (a reject reason is fine, nothing else happens); valid but not
  available → a distinct reason, so the app can play the GUI negative SFX for a local issuer (presentation only).
- [ ] Human path: the campaign battle's tactical command hook (`campaign_battles.cpp`, `set_tactical_command_hook`)
  already forwards to `handle_command`. `BattleAi::execute` (AI direct commands) needs no case: AI and Lua start runs
  directly, as in the original.
- [ ] Test: command 28 through the command pipeline starts the run exactly as a direct `start_bombing_run` at the
  execute frame would; a fogged / prevention-radius point and a not-ready run change nothing.
- [ ] `bombing_run_frames_until_available` deviates while a run is in progress: the port returns 0 (its ready frame is
  the old one); the original gives interval + remaining flight (§9.3). For the HUD, expose the 0x495250 fraction (or
  0x4950f0's frame together with the last start; the port keeps it in `L1BombingRun::last_start`), port-side as a
  const query: the HUD reads it, the sim never reads it back. The port's bombers fly straight at a fixed per-frame
  speed, so the remaining flight is the distance to the exit along the path / speed, max over the live bombers.
- [ ] Goldens: only if a recorded run issues command 28 (none expected).
- [ ] Gap, not in t02 unless the coordinator adds it: the skirmish records (0x493d50 skirmish branch, L1_S4_REMAINING
  §3.7) are not modelled; `l1_strikes_setup` builds only the campaign attacker's record, so in a port skirmish the
  button would always be disabled. Original skirmish record: every player whose faction has `Is_Playable` and a
  skirmish land bomber: enabled, available, interval = int(FPS × `Max_Bombing_Run_Interval_Seconds`), ready = now +
  interval, last start −1; usable only once the gate list allows it (`Enable_Bombing_Runs` from the
  `RL_Bombing_Run_Use_Upgrade`-type upgrades) and the lobby bit of §9.2 is set.

**t03 (HUD)**
- [ ] `b_special_weapon` in land: never hidden; disabled = controls locked or not `bombing_run_available(local)`;
  alternate icon 3, tooltip index 3 (`TEXT_TOOLTIP_BOMBRUN_BUTTON`).
- [ ] Dial: `build_fill` = 100 × (ready ? 1 : (f == 0 ? 1 : f)), f per §9.3. The port's ability buttons hide a complete
  dial (`tactical_hud.cpp`, recharge ≥ 1 → no dial); until §9.9 item 1 is settled pick one rule and mark it [port].
- [ ] Targeting state lives in the HUD / app, not the sim: toggle on click and Ctrl+W (Select_Target SFX on entry);
  `POINTER_BOMBING_RUN_TARGET` while not over the HUD; green / red blob (`Bombing_Run_Shadow_Blob_Material_Name`,
  side `Bombing_Run_Blob_Size`) at the ground point under the cursor, validity from the sim's const
  `bombing_run_target_valid`; left-up on a valid point → command 28 + Incoming SFX + end; invalid → GUI negative SFX,
  stay; right-up → cancel SFX + end, and the right-click still reaches the world; not ready → silent end; second
  click / Ctrl+W → cancel SFX + end.
- [ ] Feed test (hud_feeds) for enable, icon, tooltip and dial; update the visual baselines only if the button's look
  changes.

### 9.9 Open items (game probes; not run)
1. [L] Dial at 1.0: complete `i_button_build` overlay or nothing? Probe: campaign land battle with bombers in orbit;
   screenshot the button during the cooldown and when ready.
2. [M] Blob colours (green valid / red invalid): same battle, hover a fogged point and an open point while targeting.
3. [M] A disabled button does not run its click callback (generic button path not read): click the button during
   the cooldown and check that no Select_Target sound plays.
4. [M] The writer of the HUD ground point +0x26dc (blob position); the click itself uses the 0x3bb2d0 pick.

### 9.10 Named entry points (this pass)
| RVA | role |
|---|---|
| 0x312570 | land HUD update: `b_special_weapon` enable, icon 3, dial; order buttons (§5.1) |
| 0x346a80 | `b_special_weapon` click: space superweapon / land bombing-run toggle |
| 0x2e7950 | bombing-run targeting toggle / cancel (blob, Select_Target and Cancelled SFX) |
| 0x63f00 case 0x16 | hotkey "super weapon" (Ctrl+W) |
| 0x2f6c30 code 1 / 3 | HUD mouse handler: left-up issues command 28, right-up cancels |
| 0x2f5c80 | cursor choice: 0x21 `POINTER_BOMBING_RUN_TARGET` while targeting |
| 0x30c3b0 | per-frame blob colour / position / size |
| 0x494410 / 0x494540 | bombing run available / target test |
| 0x495250 / 0x4950f0 | dial fraction / frame the run is available again |
| 0x4953d0 / 0x495360 | bomber type SFX Select_Target / Incoming for the local player |
| 0x524560 / 0x5247a0 / 0x524590 / 0x524650 / 0x5246d0 | BombingRunEventClass ctor / set point / unpack / pack / execute |
| 0x492340 | PlayerBombingRunClass default record (ready −1, interval 3600, last start −1) |
| 0x28b080 | HUD mode from the game mode type (§9.0) |
