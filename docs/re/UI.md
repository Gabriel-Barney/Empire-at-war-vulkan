# UI, HUD, menus and input routing (StarWarsG.exe FoC x64 v1.121) — behaviour spec

Written 2026-10-27 by the RE agent, ahead of roadmap steps P2 "Tactical HUD" and P4 "GUI & menus".
Clean-room: behaviour, orders, constants and data layouts in prose and tables only; no code and no
pseudo-code from the decompilation. Addresses are RVAs in StarWarsG.exe; names come from
`docs/re/ghidra/functions.csv` where one exists, otherwise `FUN_<rva>`. Data facts come from the
vanilla FoC text data (`extracted/foc_data_text.zip`: `Data\XML\CommandBarComponents.xml`,
`GUIDialogs.xml`, `GameConstants.xml`, `RadarMap.xml`, `ShadowBlobMaterials.xml`, `SFXEventsGUI.xml`,
`Data\Resources\GUIDialog\GUIDialogs.rc` + `Resource.h`).

Confidence: **[H]** read in the code or the data and cross-checked, **[M]** read but the intent is
inferred, **[L]** inferred from surroundings, needs a capture or an in-game check.

Companion documents: RENDER.md §1.1 (frame steps), MAIN_LOOP.md §2 and §5 (no menu mode, load
thread, boot order), COMMANDS.md §2.4 (Select / SelectAll / ControlGroup events),
xml_field_tables.md (CommandBarComponent offsets), docs/formats/SPEC.md §5 (MTD atlas) and the DAT
section (MasterTextFile).
**HUD_GAPS.md** (2026-10-28) covers hardpoint reticle behaviour (§1.5 here), tooltip and encyclopedia
timing and anchors (§2.3 here), the hero roster, the galactic day pips, and the order-button enable rules.
**GALACTIC_DIALOGS.md** (2026-10-28) covers the battle-pending banner (`pending_battle_shell` /
`story_pending_battle_shell`), the mission holocron and `IDD_STORY_SUMMARY_DIALOG`, the auto-resolve result
screens, the fleet organiser (`i_main_organize`) visibility (its pane fill: §7 here; IME gadgets: §8), build-pad / queue / resource-panel details of the
galactic bar, and the font `Stretch_Factor` rule (§3.4 here).

---

## 0. Summary for the P2/P4 engineers

1. **Two UI systems.** (a) The **HUD / command bar** object (one global, 0x140b27f60) owns all
   in-game UI: the command bar shells, buttons, bars, text, tooltips, selection brackets and health
   bars. It is data-driven by `CommandBarComponents.xml`. (b) The **GUI dialog manager**
   (0x140a78510) owns every Win32-resource-style dialog: main menu, options, skirmish/campaign setup,
   load screens, battle-end screens, in-game menus. It is data-driven by `GUIDialogs.rc` (layout),
   `Resource.h` (ids) and `GUIDialogs.xml` (textures, fonts, tooltips). [H]
2. **World-space UI is mostly screen-space.** Brackets, health/shield bars, control-group numbers,
   ability icons, fly-text and the hard-point reticles are 2D quads placed by projecting a world
   point through the gameplay camera, then drawn in the HUD layers **after** the 3D world (after the
   whole post chain incl. bloom, and after a depth clear). Only the **selection circle** is a true
   3D-scene primitive (a terrain shadow blob on land, a flat blob in space) drawn in the Transparent
   bucket. [H for the projection path, M for the per-item list]
3. **HUD canvas = 1024×768 virtual units, origin at screen centre, Hor+ for widescreen**: wider than
   4:3 widens the canvas (height stays 768), narrower than 4:3 heightens it. Three orthographic
   cameras are rebuilt on every resolution change. [H] (0x304770)
4. **HUD draw = 13 layers (0..12) rendered in fixed pairs between other passes**; each layer has six
   sub-lists drawn in a fixed order (plain quads, radial-wipe quads ×2, extra quads, model-text,
   text). Component `Base_Layer` selects the layer. [H for structure, M for the Base_Layer mapping]
5. **Fonts are Windows TrueType fonts rasterised through GDI at runtime** (`CreateFontA`,
   antialiased, weight 400/700) into glyph textures; pixel height scales with screen height
   (point size × screenH × 96 / 600 / 72). Fallback font "EmpireAtWar-Medium". [H] (0x2706c0)
6. **Input order per mouse/key message**: IME → cinematic/movie overlay → modal targeting controller
   → GUI dialogs (topmost first) → [chat] → command bar/HUD → world. First consumer wins. [H] (0x60c60)
7. **Default hotkeys: a fixed 153-slot table** (keycode + modifier, modifier 0 none / 1 Shift /
   2 Ctrl / 3 Alt) built in code (0x211940), saved to the profile as `KeySettingsCount`,
   `KeySettingKeycode<n>`, `KeySettingModifier<n>`. Full table in §4.3. [H]

---

## 1. World-space UI (P2 "Tactical HUD")

### 1.1 Where it draws in the frame

Frame order from RENDER.md §1.1, with the UI roles now identified. All calls take the HUD object
0x140b27f60 unless stated. Every step except the 2D scenes and the HUD layer pairs 7–8 / 9–12 is
skipped when the "present this frame" test fails or the interface is hidden (byte 0x140b3069b). [H]

| # | call | role | conf |
|---|---|---|---|
| 3 | mode vf21 | 3D world incl. selection circles (Transparent bucket), post chain, bloom | H |
| 4 | 0x13d030 | depth clear | H |
| 5a | 0x3dda70 | galactic only: per-planet label groups (name, value, icons) with zoom-driven alpha (§1.6) | H |
| 5b | 0x302620 | per-object HUD groups kept in a map (+0x2018): queue up to 10 components each (brackets, bars, icons); tactical HUD modes only (HUD mode +0x211c: 1 galactic, 2 land, 3 space), not while hidden | M |
| 5c | 0x300c00 | mode-specific HUD build: galactic 0x311380, land 0x318720, space 0x312b90; then drag-object list and the hover target | M |
| 5d | 0x302520 | pause/"objective" shells (two component lists) | L |
| 5e | 0x2fce50 | tactical only: marker at a projected world point (component 0x210) | L |
| 5f | 0x301190 | tactical: selected-unit panel layout (grid of selection buttons with full/left/centre/right border pieces, column count from global 0x140b16a4c) | M |
| 5g | 0x301120 | galactic radar (+0x20d0) or space radar (+0x20d8) | M |
| 5h | 0x2ff5a0 | main HUD queue pass: all shells/buttons, tooltips (0x301d00), drag rectangle, galactic planet pop-ups, cursor-attached ability ring (within 150 px of the press point), world-anchored fly-text list (+0x2af8) | H for structure |
| 5i | 0x2ffcb0(0,1) | **draw HUD layers 0–1** | H |
| 6 | 0x3023b0(1) | ortho 3D **shell models** (camera +0xfd0) — first half; plus up to 10 attached 3D icon models | H |
| 6 | 0x301100, 0x3010d0 | HUD 3D overlay model (+0xfb8); tactical radar map render (0x140b2bcb0) | M |
| 6 | 0x2717c0 | registered 3D overlay scene list A | M |
| 6 | 0x2ffcb0(2,4), 0x300dc0, 0x2ffcb0(5,6) | layers 2–4, a HUD-owned Scene2d (+0xff0), layers 5–6 | H |
| 6 | 0x300980, 0x2ffcb0(7,8) | second ortho model set (+0xfd8, camera +0xfe0), layers 7–8 | H |
| 6 | 0x3023b0(0), [0x44dea0], 0x2ffcb0(9,10) | shell models second half, tutorial/teletype text (0x140b306dd), layers 9–10 | M |
| 7 | 0x25c8c0(0x140a619f0) | **2D scene #1** (GUI dialogs, splash) | M |
| 8 | 0x2ffcb0(11,12), 0x271740 | layers 11–12 (topmost HUD: tooltips are on layer 10) and 3D overlay scene list B | H |
| 9 | 0x25c8c0(0x140a619f8) | **2D scene #2** (mouse cursor, topmost) | L |

Port rules [H]: nothing after step 4 reads world depth; every HUD layer is drawn with an identity
world and the HUD ortho camera; the HUD is drawn even while the sim is paused.

### 1.2 Projection and scaling

- World → screen: the gameplay camera of the current mode (mode +0x90) projects the anchor point to
  normalised screen coordinates, which are multiplied by the pixel width/height (HUD +0x1008/+0x100c)
  and then converted to HUD canvas units through the HUD ortho camera (+0xfc0). An "off screen" flag
  from the projection hides the item. [H] (0x2ff5a0 tail, 0x261e40, 0x261a70)
- **Brackets and bars therefore keep a constant pixel size**; they do not shrink with distance. The
  only distance-dependent part is the selection circle (a world-space blob, §1.3). The GameConstants
  `Health_Bar_Scale` 1500, `Land_Health_Bar_Scale` 500, `Min_Health_Bar_Scale` 1.0 and
  `Health_Bar_Spacing` 2.0 (code defaults 20, 20, ?, 1.0 at 0x2c91c0) are the only scale knobs; their
  use site was not found in this pass. [H for values, L for use]
- Text is snapped to whole pixels: the left/top edge is rounded to the pixel grid (half-pixel bias)
  before drawing. [H] (0x2ffcb0, 0x301d00)

### 1.3 Selection circle (3D) [H unless marked]

Owned by `SelectBehaviorClass` (setup 0x3c1f90, per-tick update 0x3c2710, called from the object
service 0x3a6b80):
- Created when the object type has `Select_Box_Scale` > 0. Land: an `alTerrainShadowBlob`
  (0x12f3e0, terrain-projected); space: an `alBlob` (0x13a2d0, flat quad). Both are 3D-scene
  primitives in **PHASE_TRANSPARENT** (RENDER.md §1.3 lists blob shadows there).
- Radius = `Select_Box_Scale` × object scale (type +0x318 × +0x658); centre = object position, or
  the parent's position for hard-point children; a 3D model variant is raised by
  `Select_Box_Z_Adjust` (+0x31c).
- Shown only when the object is selected by the local player and either owned by or allied with
  them (type byte +0x27 allows allies), and hidden while the modal targeting controller
  (0x140b27a10 +0x34a) or the cinematic flag 0x140a28c91 is active.
- Material from `Selection_Blob_Material_Name`, else the faction default:
  `ShadowBlobMaterials.xml` entries `Selection_<Rebel|Empire>_<Land|Space>`, `Selection_Garrison_*`,
  `Selection_<faction>_Hero`, `Highlight_<faction>_Hero`. They use `BlobStencilMasked.fx` with
  `Texture_Name` = `i_selection_cutout(_space).tga` and `Texture_Name_2` = `i_selection_<faction>.tga`.
  Other overlays on the same path: `Invalid_Move_Land`, `Yellow_Move_Land`, ability range overlays,
  `Reinforcement_Point_Radius_Overlay`.

### 1.4 Brackets, bars and per-unit icons (2D) [H for components, M for the build logic]

Per selected/hovered object the HUD builds a 0x58-byte record (0x2ddac0) of components:

| element | component (CommandBarComponents.xml) | texture(s) |
|---|---|---|
| bracket (two corner pieces) | `st_bracket_small` / `_medium` / `_large` (`Color` 0,255,0) | `i_icon_select_{small,med,big}_{l,r}.tga` |
| health bar | `st_health` / `_medium` / `_large` (`Type=Bar`, 11 levels, `Smooth_Bar`) | frame `i_bar_control_wide[_medium/_large].tga`, fill `i_bar_health_{small,medium,large}0..10.tga` |
| shield bar | `st_shields` / `_medium` / `_large` | fill `i_bar_shield_*10.tga` |
| power bar | `st_power` | as health |
| control-group number | `st_control_group` (EmpireAtWar-Medium 9, outline, offset 0,−32) | text |
| active ability icon | `st_ability_icon` (alternate textures indexed by ability) | `i_button_sa_*.tga` |
| garrison / weather | `st_garrison_icon`, `lt_weather_icon`, `garrison_slot_icon` | |
| hero icon + bar | `st_hero_icon`, `st_hero_health` | |
| fleet grab bar | `st_grab_bar` | |

- **Size class** = type `GUI_Bracket_Size` (0 small, 1 medium, 2 large; vanilla uses all three,
  2 most often). When −1: in land mode the class comes from the unit's category (two lowest
  categories → small); otherwise from category bits of type +0x1648 (medium/large). The class picks
  the matching bracket, health and shield components. [H for the switch, M for the category meaning]
- **P2 data check (2026-10-29)** — what the shipped files settle (FoC and EaW `CommandBarComponents.xml`, `MT_COMMANDBAR.MTD`
  1107 entries, all `Data\XML` object files with `Variant_Of_Existing_Type` resolved) [H]:
  - *Bracket art does not exist.* `i_icon_select_{small,med,big}_{l,r}.tga` is in no MEG, MTD or loose folder of either game
    (searched every `.meg` listing and the atlas), so vanilla's `st_bracket_*` TextButtons have no texture: nothing is drawn
    and the corner length/thickness/offset cannot come from data. The port draws no bracket on an atlas miss; a mod that adds
    the textures gets one piece per side at the left/right bound [port]. The same components also carry a text label
    (`Font_Point_Size` 5/7/9, `Text_Offset` 6,−8 / 10,−32 / 14,−64, `Text_Color2` 255,255,255,180; the control-group number is
    separate: `st_control_group` 0,−32); whether the object HUD fills it is **needs RE**.
  - *Bar size = atlas size.* Frame `i_bar_control_wide[_medium|_large]` and all 11 fills `i_bar_health_*0..10` /
    `i_bar_shield_*10` are 34 / 68 / 102 × 2 canvas units, `Scale` 1 (`st_health_bar` 0.8, `st_hero_health` 1.0: galactic /
    hero bars), `Smooth_Bar` true, `Outlined_Bar` true on health, shield and hero bars. `Health_Bar_Scale` & co. are unused
    (STRATEGIC_GAPS §7.2), so the port's fixed-size bars are the original behaviour. Every bar `Offset` is 0 0 (the two
    others −16 / −24), so the bars' vertical placement relative to the object's bounds is not in data (**needs RE**), and
    what `Outlined_Bar` draws (a 1-px outline?) is **needs RE**; the port ignores it.
  - *Layer identity.* `Base_Layer` takes exactly 0..10 in the data; the `tooltip_*` components carry 10 and §1.1 puts tooltips on
    HUD layer 10, so Base_Layer = HUD layer (identity) is consistent; all `st_*` world components are 0, `generic_flytext` 1.
    Layers 11–12 have no data users (topmost, dialogs / cursor). Which components go into the plain (PrimOpaque) list and
    which into the fill-dial (PrimAlpha) list is not in data (**needs RE**: the add-quad call sites); the port sends every
    translucent item to the PrimAlpha list, which is safe because PrimOpaque cannot blend.
  - *Size classes, explicit `GUI_Bracket_Size` by `CategoryMask`* (0/1/2/default −1): Transport 36/0/0/3, Wall 16/0/0/5,
    Infantry 174/0/5/104, Vehicle 3/14/0/24, Corvette 0/2/0/1, Capital (+AntiFrigate) 0/0/16/65, Structure 56/70/241/95,
    Fighter+SpaceHero 10/8/0/1, plain Fighter 0/0/0/5, Super 0/0/0/2, Air 0/3/0/15, LandHero 3/0/0/49. So most ships and
    infantry rely on the −1 rule; the data agrees with Fighter/Bomber small, Corvette/Frigate/Vehicle/Air medium, Capital /
    Super / Structure large, Infantry small, but cannot fix the rule itself (the RE note above says only category bits, "two
    lowest → small"; Fighter 0x1 and Bomber 0x2 are the two lowest bits of §OBJECT_TYPES 9.1.1). The old port rule looked for
    `SuperWeapon` / `SpaceStructure`, which are not category names: fixed to `Super` / `Structure`. Exact bit → class table
    is **needs RE**.
- Bar fill uses the 11-step overlay textures (`Max_Bar_Level` 10); `Smooth_Bar` interpolates.
  [M]
- Type `GUI_Hide_Health_Bar` suppresses the bars; `GUI_Bounds_Scale` scales the screen-space
  bounds used for bracket placement (read in 0x2e7fd0). [H for the read, L for the formula]
- Colour/alpha follow the owner colour and the HUD fade; a whole group is faded through each
  component's set-alpha virtual (vtable +0x20). [M]

### 1.5 Reticles, markers, rally and waypoints

- **Hard-point reticles** (space): `TargetingHUDReticleClass`/`HUDReticleClass`, textures per
  hard-point type from GameConstants `HardPoint_Target_Reticle_{Enemy,Friendly}[_Tracked|_Repairing|
  _Disabled[_Tracked]]_Texture` (pairs "HARD_POINT_<TYPE>, I_Hard_Point_Reticle_<name>"), constant
  screen size `HardPoint_Target_Reticle_*_Screen_Size` = 0.03 (fraction of screen). [H for data, M for
  constant-size behaviour]
- **Move acknowledgement**: particle effects `GUI_Move_Command_Particle`,
  `GUI_Double_Click_Move_Command_Particle`, `GUI_Attack_Move_Command_Particle`,
  `GUI_Guard_Move_Command_Particle` (GameConstants), scaled `GUI_Move_Acknowledge_Scale_Land` 1.0 /
  `_Space` 5.0 — 3D particles in the world (Transparent). The radar also plays a radar event
  (`Default_Click`, `Default_Attack_Click`, `Default_Double_Click`). [H]
- **Waypoint lines** use `WaypointLineSubClass` (3D, world). Beacons (`PlaceBeacon` event,
  `BeaconBehaviorClass`) are world objects plus a radar event `Default_Beacon_Placement`. Rally
  points: not identified in this pass. [L]
- **Fly-text** (`generic_flytext`: EmpireAtWar-Medium 7, yellow, embossed, layer 1): a list at HUD
  +0x2af8 of world-anchored entries with a pixel offset, re-projected every frame and removed when
  their component is gone. [H] (0x2ff5a0)

### 1.6 Galactic planet labels [H] (0x3dda70)

Per planet (records of 0x188 bytes at HUD +0xde0, count +0xde8) the HUD queues name, value,
political control, ground/space level pips, conflict/hero/smuggler icons. Their alpha follows the
galactic camera zoom (camera object 0x140b2eea0, zoom state +0x44, zoom fraction z = 0x3c2bf0):
state 1 → 255·max(0, 1 − 10z); state 2 → 255·max(0, 10(z − 0.9)); state 3 → 0. Selected planets
show extra groups; some groups appear only when HUD flags +0xe02/+0xe03 are set.
Fleet/ground stack buttons, hero medals, fleet models in transit and the selected-planet ellipse:
GALAXY_MAP_PRESENTATION.md §1–§3.

### 1.7 Radar / minimap

- Components `radar` (tactical, 171×175) and `radar_galactic` (175×175), blips `radar_blip`
  (`i_radar_focus.tga`, layer 5) and `g_radar_blip`/`g_radar_view` (galactic). [H]
- `RadarMap.xml`: logical screen space (−1,−1) top-left … (1,1) bottom-right; event models
  (`I_minimap_*.alo`, scale 0.001–0.003, duration 0.8–10 s); backdrop `i_radar_map_grid.tga`; FOW
  colours (space 25,66,120,100; land 10,10,10,100); asteroid colours; terrain passability colours
  per biome (`Passability_Color_Settings`). [H]
- Per-type: `Radar_Icon_Name`, `Radar_Icon_Size`, `Radar_Icon_Scale_Land/Space`, `Radar_Rotate_Icon`,
  `Radar_Show_Facing`, `Is_Visible_On_Radar`, `Is_Visible_On_Enemy_Radar`,
  `Visible_On_Radar_When_Fogged`, `Radar_Clip_To_Visible_Region`. GameConstants
  `Radar_Colorize_Selected_Units` Yes with 209,255,209,255; MP enemy colouring off. [H for tags]
- The radar map is rendered by its own object (0x140b2bcb0) between HUD layer pairs (§1.1); event
  models are drawn as 3D models through an ortho camera. Lua `Add_Radar_Blip` / `Remove_Radar_Blip`
  exist. [M]

---

## 2. Command bar (P2/P4)

### 2.1 Data model: `CommandBarComponents.xml` [H]

`CommandBarComponentFiles.xml` lists one file, `CommandBarComponents.xml` (850 components). Each
child element's **tag name is the component name**, which is also the **bone name** in the parent
shell model and the key the code uses (a fixed name table at 0x85f488… resolved by name, 0x347b70 /
0x347da0; the code then refers to components by enum index via 0x347b50). Types and counts:
TextButton 621, Button 114, Bar 85, Shell 18, Icon 8. Runtime classes: `ShellComponentClass`,
`ButtonComponentClass`, `TextButtonComponentClass`, `BarComponentClass`, `IconComponentClass`,
`ModelComponentClass`, `TextComponentClass`, `SelectComponentClass`, all deriving from
`BaseComponentClass` (vtables in classes.csv).

Field meanings (offsets in xml_field_tables.md "CommandBarComponent"):

| group | tags |
|---|---|
| identity | `Type` (Shell/Button/TextButton/Bar/Icon), `Group` (logical set, e.g. `Tactical_Selection`, `Build_Queue`, `Fleet 0`, `Reinforce`) |
| shell | `Model_Name` (.alo), `Model_Offset_X/Y` (anchor the shell to the screen edge on that axis), `Manual_Offset`, `Hidden`, `Disabled`, `Dialog_Scene`, `Tutorial_Scene`, `Mega_Texture_Name` (`MT_CommandBar`) |
| textures (state) | `Blank_Texture_Name` (background), `Icon_Texture_Name`, `Icon_Alternate_Texture_Name` (list, selected by faction or index), `Mouse_Over_`, `Selected_`, `Disabled_`, `Flash_`, `Build_` (radial build dial), `Bar_Texture_Name`/`Bar_Overlay_Name` (lists of 11), `Lower_/Upper_Effect_`, `Overlay_`, `Overlay2_`, `Cursor_Texture_Name` |
| placement | `Base_Layer` (0..10), `Size`, `Scale`, `Offset`, `Default_Offset`, `Default_Offset_Widescreen`, `Icon_Offset`, `Text_Offset`, `Text_Offset2`, `Build_Dial_Offset`, `Build_Dial2_Offset`, `Overlay_Offset`, `Overlay2_Offset`, `Lower/Upper_Effect_Offset`, `Mouse_Over_Offset`, `Disabled_Offset`, `Pixel_Align`, `No_Shell` (free-floating, not bone-attached), `Snap_Location`, `Offset_Render` |
| text | `Font_Name`, `Alternate_Font_Name`, `Font_Point_Size`, `Text_Color`, `Text_Color2`, `Text_Outline`, `Text_Emboss`, `Left_Justified`, `Right_Justified`, `Max_Text_Width`, `Tooltip_Text` (MasterTextFile keys, a list: one per state) |
| behaviour | `Click_SFX`, `Mouse_Over_SFX`, `Right_Click_SFX`, `Click_Shift`, `Toggle`, `Tab`, `Drag_And_Drop`, `Drag_Select`, `Snap_Drag`, `Drag_Back`, `Can_Drag_Stack`, `Stackable`, `Should_Ghost`, `Ghost_Base_Only`, `Should_Render_At_Drag_Pos`, `No_Hidden_Collision`, `Selected_Alpha`, `Disable_Darken`/`Disabled_Darken`, `Swap_Texture`, `Cross_Fade` |
| animation | `Can_Animate`, `Anim_FPS`, `Loop_Anim`, `Animate_Back`, `Animate_Upper_Effect`, `Scale_Duration`, `Blink_Duration`, `Blink_Rate`, `Blink_Fade`, `Smooth_Bar`, `Max_Bar_Level`, `Outlined_Bar` |
| colour | `Color` (RGBA tint), `Lower_Effect_Additive` |

All textures come from the **`MT_CommandBar` MTD atlas** (`Data\Art\Textures\MT_CommandBar.mtd` +
tga, 3,577 entries, SPEC.md §5); GUIDialogs.xml names the same atlas (and a compressed variant
`MT_CommandBarCompressed`). [H]

### 2.2 Shells and layouts [H for data, M for placement rule]

Shells are **3D .alo models drawn with an orthographic camera** (HUD +0xfd0, near 0.01, far 500) on
top of the world; buttons attach to bones of the same name. Vanilla shells:

| shell component | model | used in |
|---|---|---|
| `i_main_commandbar` | `i_galactic_controls.alo` | galactic main bar (MegaTexture MT_CommandBar) |
| `i_main_organize` | `i_galactic_organize.alo` | galactic fleet organiser |
| `i_main_skirmish` | `i_tactical_controls.alo` | tactical (land and space) bar |
| `i_build_buttons` | `i_build_buttons.alo` | tactical build pads |
| `i_main_reinforce` | `i_main_reinforce.alo` | land reinforcement pad |
| `good_hero_frame` / `evil_hero_frame` | `i_main_hero_frame_{rebel,empire}.alo` | hero roster |
| `pending_battle_shell`, `story_pending_battle_shell`, `autoresolve_shell`, `pause_shell` | `i_attack_mode_choice.alo`, `i_attack_mode_single.alo`, `i_resolve_mode.alo`, `i_attack_mode.alo` | galactic battle prompts (top-centre; GALACTIC_DIALOGS.md §1; `autoresolve_shell` unused in retail) |
| `map_shell`, `map_overlay_shell` | `planetary_mode.alo`, `planetary_mode_overlay.alo` | advanced planet map |
| `market_frame`, `bm_frame`, `corruption_shell`, `hack_shell`, `target_type_back` | `i_black_market.alo`, `i_sabotage.alo`, `i_corrupt_choice.alo`, `i_hack.alo`, `i_neutralize_hero.alo` | FoC dialogs |

`Model_Offset_X/Y` = the shell sticks to the left/right or top/bottom edge on that axis when the
canvas widens (Hor+), otherwise it stays centred; `Default_Offset_Widescreen` replaces
`Default_Offset` when the aspect is wider than 4:3 (only `objective_back` uses it). [M]

### 2.3 Bindings to the selection and production [M unless marked]

| HUD element | components | bound to |
|---|---|---|
| galactic build buttons | `b_Create_00..47` (Group `Structure Buttons`), `build_pad00..79`, `unit_pad00..09` | buildable types at the selected planet; `Build_Texture_Name` radial dial = progress; tooltips list = queue full / pop cap / no credits |
| production queues | `q1..q3_*`, `queue_*`, `tqueue_*` (Groups `Build_Queue`, `Tactical_Build_Queue[_Land]`) | queue entries; `CommandBarClass::Cancel_Production` 0x3cb2f0 |
| fleets | `q1_0000..0406`, `land_fleet_00..19` (Stackable, Drag_And_Drop) | fleet/unit stacks; drag starts post "start_fleet_drag" / "start_land_drag" (0x2f6c30) |
| tactical selection panel | `s_select_00..47` (portrait buttons), `s_health_00..47`, `s_shield_00..47` (bars), `special_border_00..11`, `special_button_00..47` (ability buttons with recharge dial `i_button_sa_recharge.tga`) | the local selection set (COMMANDS.md §2.4), grouped by type; abilities via the special-ability events |
| reinforcements | `r_0000..0403`, `reinforcement_counter`, `r_pop_icon/text` | reinforcement pool (land) |
| top-bar text | `Text_Credits[_tactical]`, `Text_Pop_Cap`, `Text_Day_Counter`, `Text_Planet[_tactical]`, `Text_Tech_Level`, `Text_Tactical_Tech` | player credits, pop cap, galactic day, location, tech |
| buttons | `b_option_g/t` (menu), `b_camera_g/t` (movie mode), `b_beacon_t`, `b_special_weapon[2]`, `b_reinforcement`, `b_retreat`, `b_play_pause[_t]`, `b_fast_forward[_t]`, `b_droid_help[_tactical]`, `b_filters0..3`, `b_dial` | game commands; play/pause and fast forward = game speed |
| tooltips / encyclopedia | `tooltip_*` (layer 10), `encyclopedia_*`, `zoomed_*` | hovered component or object; delays from GameConstants `Tooltip_Delay` 330 ms, `Encyclopedia_Delay` 750, `Long_Encyclopedia_Delay` 4000 [H] |

Lua can drive components by name (`GUIComponentVisibilityClass`, `...Enable`, `...Text`,
`...PlayAnim`, `...StopAnim`, `...Color`, `...Blink`, `...Flash`, `...StopFlash`, `GUITextColorClass`,
`GUIButtonIconClass`). Selection itself is sim state (COMMANDS.md): the panel only reads it. [H]

### 2.4 HUD canvas and cameras [H] (0x304770, called on WM_SIZE-type messages 0x214/0x232)

- Canvas base 1024 × 768. Aspect > 4:3: width = screenW × 768 / screenH, X scale (+0x1028) =
  width / 1024. Aspect < 4:3: height = screenH × 1024 / screenW, Y scale (+0x102c) = height / 768.
- Ortho cameras, all spanning ±width/2 × ±height/2: +0xfc0 (near 0.1, far 40: components),
  +0xfd0 (near 0.01, far 500: shell models), +0xfe0 (near 0.01, far 40: second model set).
- Units per pixel (+0x1000, +0x1004) = canvas size / screen pixels; the four screen corners in
  canvas units are cached (+0x10dc…).
- At exactly 800 pixels wide, wrapped text width is reduced to 95 %. (0x2df0c0)

### 2.5 HUD layer drawing [H] (0x2ffcb0)

For each layer in the requested range, six lists are drawn in this order, each as one dynamic quad
batch through the 2D render path, then cleared:
1. plain quads (4 vertices / 6 indices each);
2. quads with a 0–100 fill value: 100 → plain quad, otherwise a radial "clock wipe" fan whose vertex
   count shrinks by one per 12.5 % (8 segments) — the build/recharge dial;
3. the same with an alternate state flag (second dial);
4. extra quads;
5. text glyph runs attached to models (colour from the item);
6. text strings: font scaled by units-per-pixel × item scale, optional pixel snapping, optional
   second (shadow/outline) pass in white.
The four quad lists use different 2D blend-state selectors (0, 2, 1, 3) [L: alpha, additive,
modulate variants].

---

## 3. 2D scenes, dialogs and menus (P4)

Main menu overlay logos, Petroglyph button and version text: GALAXY_MAP_PRESENTATION.md §5;
attract demo: MAIN_MENU_BACKGROUND.md (map owners: GALAXY_MAP_PRESENTATION.md §6); hero bar order: §4.

### 3.1 Files [H]

| file | role |
|---|---|
| `Data\Resources\GUIDialog\GUIDialogs.rc` | Visual C++ resource script: one `DIALOGEX` per screen (117 dialogs), controls with position/size in dialog units |
| `Data\Resources\GUIDialog\Resource.h` | numeric ids for `IDD_*` / `IDC_*` |
| `Data\XML\GUIDialogs.xml` | `<Textures File="MT_CommandBar">`: `<Default>` gadget skins + per-dialog / per-control overrides (`none` = no texture); `<Fonts>`: per-gadget-type defaults + per-control fonts; `<Tooltips>` |
| `Data\Text\MasterTextFile_<LANGUAGE>.DAT` | localised strings (CRC32-sorted DAT, SPEC.md) |
| `Data\XML\SFXEventsGUI.xml` | UI sounds |
| `Data\XML\MousePointers.xml` | cursors (`Base_Texture`, `Hot_X/Y`, `Anim_Frame_Delay`) |
| `Data\XML\Movies.xml` | Logo1/Logo2/intro Bink movies |

All three GUI files are read at boot by `TheGUIDialogManager` (MAIN_LOOP §5.1 step 11.7).
The original game does not use .rc compilation: the engine parses the script text itself. [M]

### 3.2 Dialog format [H for syntax, M for interpretation]

- Header: `IDD_NAME DIALOGEX 0, 0, W, H`, `STYLE`, `CAPTION`, `FONT size, "face"` (Win32 header
  fields; the face is ignored in favour of GUIDialogs.xml fonts [M]).
- Controls: `PUSHBUTTON`/`DEFPUSHBUTTON` (288/51), `LTEXT`/`RTEXT`/`CTEXT` (630/124/80),
  `EDITTEXT` 150, `COMBOBOX` 120, `LISTBOX` 93, `GROUPBOX` 65, and `CONTROL` with class
  `Button` (checkbox/radio, `BS_AUTORADIOBUTTON | BS_PUSHLIKE` = tab), `msctls_trackbar32` (slider),
  `msctls_progress32` (progress bar), `PETROGLYPH_DIALOG_IMAGE` (custom image, 718 uses),
  `IMEEditBox`, `IMELocaleIndicator`.
- Gadget classes: `GUIGadgetPushbuttonClass`, `...TextClass`, `...EditboxClass`,
  `...IMEEditboxClass`, `...ComboboxClass`, `...ListboxClass`, `...CheckboxClass`,
  `...RadiobuttonClass`, `...TrackbarSliderClass`, `...ProgressbarClass`, `...VScrollbarClass`,
  `...CustomImageClass`, `...FrameClass`, `...IMELocaleIndicatorClass` (enum `GUIGadgetType`).
- **Captions are MasterTextFile keys** (`TEXT_BUTTON_BEGIN`); other captions ("Name 0", "swamp")
  are placeholders overwritten at runtime. [M]
- Skins (GUIDialogs.xml `<Default>`): 16-piece dialog frame (`Frame_Top_Left`, `_Top_Transition_
  Left`, …, `Frame_Background`), small gadget frame (9 pieces), push button left/middle/right ×
  normal/mouse-over/pressed/disabled, check on/off, radio on/off/mouse-over, slider (`Dial_*`),
  scroll bar and trackbar pieces, combo box pieces, progress bar left/middle on/off/right,
  `Scanlines` (`i_scan_lines.tga`). Overrides keyed by `IDD_*` or `IDC_*`. [H]
- Main menu `IDD_MAIN_MENU_DIALOG` is 512 × 1024 units, frame pieces `none`, background
  `MenuBack_overlay_panel.tga`; buttons: Tutorial, Single Player Games, Multiplayer, Load Game,
  Options, Exit, Credits (image button), Petroglyph logo. [H]

Screens by dialog: main menu, `IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG`, `IDD_MULTIPLAYER_DIALOG`,
`IDD_MAIN_OPTIONS_DIALOG` → `IDD_AUDIO/VIDEO/ADVANCED_VIDEO/GAMEPLAY/NETWORK_OPTIONS_DIALOG`,
`IDD_KEYBOARD_OPTIONS_DIALOG[_2.._5]`; skirmish `IDD_SKIRMISH_STAGING_DIALOG`,
`IDD_SKIRMISH_BUY_UNITS_DIALOG` (list, buttons and openers: SKIRMISH_SETUP §2.7), `IDD_SELECT_MAP_DIALOG`; galactic conquest
`IDD_CAMPAIGN_SELECTION_DIALOG`, `IDD_CUSTOM_CAMPAIGN_STAGING_DIALOG`,
`IDD_CAMPAIGN_ADVANCED_OPTIONS_DIALOG`, `IDD_MAIN_CAMPAIGN_DIALOG`; LAN/Internet lobbies; save/load;
battle end `IDD_BATTLE_END_DIALOG`, `IDD_CAMPAIGN_SCORE_DIALOG`; in-game `IDD_IN_GAME_OPTIONS_DIALOG`,
`IDD_INGAME_CHAT_DIALOG`, `IDD_RETREAT_DIALOG`, message boxes `IDD_DIALOG_BOX*`. [H]

### 3.3 Layout resolution handling [M]

- Dialog units map to the same 1024 × 768 canvas as the HUD; full-screen dialogs are 1025 × 768 or
  975 × 718 units; the menu column dialogs are 512 × 1024 and are placed against the right side of
  the screen (the Petroglyph logo control has x = −374, i.e. outside the dialog to the left).
  `DS_CENTER` centres a dialog. Widescreen keeps 768 height and extends the width (same Hor+ rule as
  §2.4). [M for the placement, L for the exact unit → pixel factor]

### 3.4 Fonts [H]

- `GUIDialogs.xml <Fonts>`: `<Default>` has `Global_Default` (EmpireAtWar-Medium 7,
  195,171,2,255) and per gadget type `Push_Button` (EmpireAtWar-Bold 8, emboss), `List_Box`,
  `Combo_Box`, `Edit_Box`, `IME_Edit_Box` (Arial Unicode MS 8–9), `L_Text`/`R_Text`
  (EmpireAtWar-Medium 7, stretch 1.3), `Overlay_Caption_Text` (Arial Bold 8), …; then ~290 per-control
  entries keyed by `IDC_*` (some nested under an `IDD_*`). Fields: `Name`, `Size` (points),
  `Character_Padding`, `Stretch_Factor` (glyph height scale at the unstretched width, §3.4 below), `Top_Color`/`Bottom_Color` (vertical
  gradient), `Emboss`, `Outline` (Yes/No).
- Face names used: `EmpireAtWar-Medium`, `EmpireAtWar-Bold`, `Arial`, `Arial Bold`, `Arial Black`,
  `Arial Unicode MS`. They are **system-installed TrueType faces**; the boot registers
  "Arial Unicode MS" and "EmpireAtWar" as allowed families (0x270020 from 0x5d990). [H]
- `WindowsFontClass::Load_Font` 0x2706c0: GDI `CreateFontA`, negative em height =
  point size × (screenH × 96 / 600) / 72 unless an absolute size is requested, weight 700 for bold
  else 400, italic flag, charset per language, `ANTIALIASED_QUALITY`; `Stretch_Factor` ≠ 1 re-creates
  the font with height = floor(height × stretch + 0.5) and width = the unstretched font's
  `tmAveCharWidth`, i.e. stretch scales glyph **height** only (corrected 2026-10-28,
  GALACTIC_DIALOGS.md §6); failure falls back to EmpireAtWar-Medium. Glyphs are drawn
  white into a DIB section and uploaded as texture pages (`tFontCharacterStruct` per glyph);
  colours are applied at draw time. A warning is logged when Windows substitutes a face. [H]
- Port: the EmpireAtWar TTFs are not in the MEGs and the Steam install has no font files; four whole
  TrueType fonts (EmpireAtWar-Light / -Medium / -Bold / -Stencil, families "Empire At War <Style>",
  OS/2 weights 400 / 500 / 600 / 700) are stored inside StarWarsG.exe (GameData and corruption, identical
  bytes; found by scanning the file for sfnt headers, 2026-10-28). `ui::register_game_fonts` loads them
  process-private (AddFontMemResourceEx) from the game roots; loose .ttf files next to the game win. The
  credits sign before HUD credit amounts is the '$' glyph of these fonts. Clean-room note: the
  rule above, not GDI, is the contract.

### 3.5 Text and localisation [H]

- Language is detected at boot (0x200090); `Data\Text\MasterTextFile_<LANGUAGE>.DAT` is tried and, if
  missing, the engine falls back to English and logs "No %s MasterTextFile exists. Falling back to
  %s". Lookup is by key CRC32 in the sorted DAT (`TheGameText`, 0x1fa460 is the key → wide string
  lookup used by every dialog). Text is UTF-16. `CreditsText_<LANGUAGE>.DAT` is separate.
- XML text fields (`Tooltip_Text`, `Encyclopedia_Text`, `Text_ID` …) store keys, not text.

### 3.6 UI sounds [H]

SFX events from `SFXEventsGUI.xml`: generic dialog sounds `GUI_Common_Dialog_Button_Hilight`,
`GUI_Common_Dialog_Button_Press`, `GUI_Common_Dialog_Listbox_Entry_Select`, chat send/receive;
command bar per component `Mouse_Over_SFX` (mostly `GUI_Highlight_Mouse_Over`) and `Click_SFX`
(mostly `GUI_Button_Press_2_SFX`); `GUI_Text_Hint_SFX` (load screen ready, hints), `GUI_Cancel_SFX`,
`GUI_Bad_Sound`, `GUI_Drag_Fleet`/`GUI_Drop_Fleet`, faction select `GUI_Select_{Empire,Rebel,
Underworld}_SFX`, toggles `GUI_Toggle_*_On/Off`. Menu music: `Main_Menu_Music_Event`; load screen
music GameConstants `Music_Event_Battle_Load_Screen`. UI sounds use the free (non-sync) RNG. [H]

### 3.7 Colour combos and tech-tree arrows (2026-11-02, p4ui t03)

Sources: the staging colour fill 0xfa1c0, the slot refresh 0xfe7c0, the LAN lobby fill 0xb1240, the colour-change
handlers 0x288c0 / 0xb6410 / 0x2ba70, `GUIGadgetComboboxClass` (0x1e7920 vtable; set-current-item 0x1e7a60, field layout 0x1e8520),
the custom-image texture setter 0x1eb000, the tech-tree page fill 0x11ebc0, the battle-end fill 0x88fe0 and the
fleet summary row fill 0x116070; data: `GUIDialogs.rc`, `GUIDialogs.xml`, FoC `MT_CommandBar.mtd`.

#### 3.7.1 Player-colour combo

- **Controls**: `IDC_PLAYER_COLOR_0..8` on `IDD_SKIRMISH_STAGING_DIALOG`, `IDD_LAN_STAGING_DIALOG`,
  `IDD_INTERNET_STAGING_DIALOG` (75 × 216 units, `CBS_DROPDOWN`; `_0` also has `CBS_SORT`, which the gadget does
  not act on [M]), and `IDC_LAN_COLOR_SELECT` on `IDD_LAN_LOBBY_DIALOG` (73 × 169). They are ordinary combo
  gadgets with the default combo skin (`Combo_Box_Left_Cap` `i_dialogue_combo_left.tga` 2 × 25,
  `Combo_Box_Text_Box` `i_dialogue_combo_middle.tga` 2 × 25 stretched, `Combo_Box_Popdown_Button` /
  `_Mouse_Over` / `_Pressed` `i_dialogue_scroll_small_down*.tga` 22 × 25): **no colour-specific textures or
  frame**. [H]
- **Items are text, not swatches.** Every colour combo is filled with one item per palette entry, index
  i = 0..8 (9 entries), item data = i. The item text is a single character and the item carries a top and a
  bottom text colour, both set to palette colour i (so no gradient). The palette is the 9 GameConstants
  `MP_Color_Blue, _Red, _Green, _Orange, _Cyan, _Purple, _Yellow, _Gray, _Eight` (SPACE_AND_OPEN.md §B.3; the
  same table the player colours use). [H]
  - Staging combos: the character is byte 0xFE converted with the OEM code page = **U+25A0 BLACK SQUARE ■**.
  - LAN lobby `IDC_LAN_COLOR_SELECT`: byte 0xDB through the OEM code page = **U+2588 FULL BLOCK █**.
  - [port] use these two code points directly (0xFE / 0xDB map to them in the US and Western-European OEM pages
    437 / 850; the port does not depend on the system code page).
- **Font**: per-control `GUIDialogs.xml` fonts: `IDC_PLAYER_COLOR_n` = Arial Unicode MS, `Size` 35,
  `Stretch_Factor` 0.25; `IDC_LAN_COLOR_SELECT` = Arial Unicode MS 30, `Stretch_Factor` 0.20; both
  `Top_Color` = `Bottom_Color` = 33,37,47,255, no emboss / outline (that colour is only the font default; the
  item colours replace it). With the stretch rule (§3.4, GALACTIC_DIALOGS.md §6: height × s, width kept at the
  unstretched average character width) the glyph comes out as a **flat wide bar**: about a quarter of the
  35-pt glyph's height at its full width. That bar is the whole "swatch". [H for data, M for the look]
- **Field (closed combo)**: the current item's text is drawn with the item's two colours in the control font.
  Placement for **every** combo gadget (not only colour combos): horizontally **centred** in the field part
  left of the pop-down button (x = field left + (field width − button width) / 2 − text width / 2), vertically
  centred on the control. Only a combo flagged left-justified starts at the field's left edge plus a small inset
  (0.005 of the screen width); the only caller of that flag is `IDC_INTERNET_HOST_CLAN_COMBO`. If the text is
  wider than (field width − 2 × padding − button width) the gadget drops **trailing characters** until it fits
  (no ellipsis). [H for the formulas, M for the padding field]
- **Drop-down list**: the rows are the combo's list items, each drawn in its own item colours with the control
  font; hover / selected highlight and scrolling are the generic combo list's (no colour-specific art). [M]
- **States**: no per-state colour art: hover only swaps the pop-down button texture
  (`Combo_Box_Popdown_Button_Mouse_Over`), open = `_Pressed`; a disabled combo (not your slot / not the host)
  keeps its item colours. [M]
- **Selection**: the chosen item's data (0..8) is the player's colour index; a slot's combo is set from that
  index (clamped to the palette). [H]

#### 3.7.2 Tech-tree arrows

- **Controls**: plain `PETROGLYPH_DIALOG_IMAGE` controls placed in `GUIDialogs.rc` between the unit icons:
  `IDC_RIGHT_ARROWn`, `IDC_DRIGHT_ARROWn`, `IDC_RRIGHT_ARROWn`, `IDC_DOWN_ARROWn` (n ≤ 30 in the shipped data;
  `IDC_DRIGHT_ARROW` has no instance). Counts: `IDD_TECH_TREE_EMPIRE` 24, `_REBEL` 22, `_BLACKMARKET` 21,
  `_EMPIRE_SPACE` 7, `_REBEL_SPACE` and `_BLACKMARKET_SPACE` 1 each (the long `IDC_RIGHT_ARROW21`, 845 × 14
  units: the tech-level axis under the `IDC_STATIC_TECHn` captions). **Placement is entirely the .rc rectangle**;
  no code positions arrows relative to tech levels or units. `GUIDialogs.xml` has no entries for them. [H]
- **Fill** (each time a tech-tree page is filled): for n = 0..99 and each of the four prefixes, if the dialog
  has that control and it is visible, set its texture and tint, then clear the image's overlay layers to
  transparent: [H]

  | prefix | texture | MTD rect (FoC `MT_CommandBar`) |
  |---|---|---|
  | `IDC_RIGHT_ARROWn` | `i_tree_arrow_right.tga` | 30 × 14 |
  | `IDC_DRIGHT_ARROWn` | `i_tree_arrow_2right.tga` | 30 × 14 |
  | `IDC_RRIGHT_ARROWn` | `i_tree_arrow_3right.tga` | 30 × 14 |
  | `IDC_DOWN_ARROWn` | `i_tree_arrow_down.tga` | 14 × 30 |

  Tint 255,255,255,255, alpha-blended draw mode (2D selector 2, PrimAlpha, §6). The texture fills the control
  rectangle (so the 845-unit axis is the 30 × 14 arrow stretched). [H for texture / tint, M for stretch]
- **No locked / researched states on arrows**: every arrow is white whatever the player has researched. [H]
- **Unit icons** (`IDC_TECHTREE_<type>`, same fill, after the arrows): for every object type that belongs to the
  viewing player's faction, the name with `-` replaced by `_` gives the control; if it exists its texture =
  the type's icon, tint **white** 255,255,255,255, **or grey 90,90,90,255** when the player's tech level is below
  the type's `Tech_Level`, or the type is build-locked for that player (flag + the player's unlocked test). No
  "researched" colour. Icons use draw mode 0; the tooltip is the type's text. [H for colours, M for the lock test]
- **Same arrow art elsewhere**: `IDD_BATTLE_END_DIALOG` `IDC_RIGHT_ARROW1` / `_2` (255 × 14, between the two
  sides' unit columns) get `i_tree_arrow_right.tga`, alpha mode, white [M for the tint argument], every time the
  dialog is filled. `IDD_SUMMARY_SCREEN_FLEETS` `IDC_HEROES_DIVIDERSk` (824 × 14 row separators): when row r is
  filled, `IDC_HEROES_DIVIDERS(r − 1)` gets `i_tree_arrow_right.tga`, white, alpha mode, and is shown; when row r
  is empty that divider and the row's icon are hidden. [H]

---

## 4. Input routing (P1 input layer)

### 4.1 Mouse [H] (window procedure 0x60c60)

Mouse input is ignored until the game is initialised (0x140a110d0). For each message the handlers
below are tried in order; the first that consumes ends the chain:

| message | order |
|---|---|
| move (0x200) | attract controller input time (0x1409cf080, MAIN_MENU_BACKGROUND.md); cinematic/movie overlay (0x140a28c10, only when no GUI dialog is active); modal targeting controller (0x140b27a10, the in-game cinematic camera controller); then the HUD hover (0x2f6c30 code 7), or, while a drag is active, the drag tracker |
| left down (0x201) | SetCapture; overlay; targeting controller; **GUI dialogs** (code 0); a chat box (0x1409cf390); **HUD** (code 0); **world** (0x30fc0 code 0) |
| left up (0x202) | ReleaseCapture; GUI dialogs (1); if not consumed and no drag-select/box flags: HUD (1); HUD drag end; **world always gets the up** (unless the 0x140a28c91 flag) |
| left double (0x203) | overlay; targeting; GUI (6); HUD (4); world (6) |
| right down/up/double (0x204/5/6) | as left with GUI 2/3/7, HUD 2/3/5, world 2/3/7; right-up always reaches the world |
| middle down/up/double (0x207/8/9) | overlay; targeting; GUI 4/5/8; world 4/5/8 (HUD only for double, code 6) |
| wheel (0x20a) | GUI dialogs first only when a dialog is active (codes 9 up / 10 down); overlay; targeting; world with notches = delta / 120 |

GUI dialog dispatch (0x1c3660): the manager iterates a snapshot of its open-dialog list and offers
the event to every dialog flagged "always receives input" (+0x108) and to the **last (topmost)**
dialog; consumed if any accepts. `0x1c3c40` = "a dialog has focus". [H]

### 4.2 Keyboard [H] (0x60c60, 0x61fe0)

- `WM_CHAR` goes only to the GUI dialogs (code 0x0c). `WM_KEYUP`: ignored while a dialog has focus;
  otherwise overlay → targeting controller → HUD (0x2f6bc0) → world (0x33580).
- `WM_KEYDOWN`/`WM_SYSKEYDOWN`: auto-repeat downs are dropped unless a dialog has focus. Dispatch
  (0x61fe0): splash dismissal → while a movie plays only Esc/Space (skip) → debug console → two
  overlays → if the top dialog is not the main menu: GUI dialogs (code 0x0d) → overlay → targeting
  controller → Esc closes the tactical in-game panel → world key handler (0x33460, camera keys) →
  **hotkey action dispatch (0x63f00)** → Enter opens chat.
- Key state array of 256 bytes at 0x140a7c560 (cleared by `KeyboardClass::Init` 0x20e010).

### 4.3 Default bindings [H for table, M for the slot → label mapping]

Profile entries `KeySettingsCount` (max 153), `KeySettingKeycode<n>` (Win32 VK, −1 = unbound),
`KeySettingModifier<n>` (0 none, 1 Shift, 2 Ctrl, 3 Alt) (load 0x212e60, save 0x212d80, defaults
0x211940). Binding a key removes it from every other slot; slots 0–9 drive three derived slots
each. Labels come from the keyboard dialog's table (0x807330, stride 0x40), whose order matches
the slots.

| slot | action | default |
|---|---|---|
| 0–9 | select control group 1..9, 0 | 1..9, 0 |
| 123–132 | assign group 1..0 | Ctrl+1..0 |
| 133–142 | add to group 1..0 | Shift+1..0 |
| 143–152 | third group action 1..0 [M: Alt, camera jump] | Alt+1..0 [M] |
| 10 / 11 | select all / select like | Ctrl+A / Ctrl+Q |
| 12 / 13 | next / previous unit | F / D |
| 14–17 | guard, attack, stop, move | G, A, S, M |
| 18–23 | waypoint, retreat, reinforce, place beacon, super weapon, attack move | W, E, R, B, Ctrl+W, T |
| 24–45 | abilities: deploy Ctrl+D, spread out Ctrl+Z, jet pack Ctrl+J, sticky bomb K, untargeted sticky bomb Ctrl+K, rocket attack Ctrl+G, cable attack Ctrl+F, FOW reveal ping Ctrl+O, power to weapons Ctrl+B, self destruct Ctrl+X, deploy troopers Ctrl+H, maximum firepower Ctrl+M, sprint Ctrl+N, system control Ctrl+C, place remote bomb Ctrl+E, detonate remote bomb Ctrl+R, fire lobbing superweapon Ctrl+U, laser defense Ctrl+L, stim pack Ctrl+S, swap weapons Ctrl+I, minor drain life Ctrl+T, proximity mines Ctrl+P | |
| 46–65 | space abilities: defend Shift+O, tractor beam Shift+T, interdict Shift+G, barrage Shift+B, hunt Shift+H, turbo Shift+E, lure Shift+L, missile shield Shift+M, spoiler lock Shift+W, ion cannon shot Shift+I, destroy planet Shift+D, sensor jamming Shift+J, stealth Shift+C, leech shields Shift+N, buzz droids Shift+Z, super laser Shift+A, cluster bomb Shift+K, space laser defense Shift+P, space self destruct Shift+S, full salvo Shift+F | |
| 66 | deploy squad | Q |
| 67 / 68 | droid advisor / initiate chat | Numpad − / Enter |
| 69–72 | scroll up/down/left/right | arrow keys |
| 73–77 | camera reset / centre / home / tactical overview / tether | End / PgUp / Home / Insert / ScrollLock |
| 78 / 79 | movie mode / cinematic view | Space / C |
| 80–83 | screenshot / quick save / quick load / mission holocron | F12 / F6 / F5 / Tab |
| 84–86 | taunts 1–3 | Numpad 1–3 |
| 87–89 | give credits / toggle interface / toggle player list | Shift+Y / I / P |
| 90–122 | FoC abilities (all Alt): lucky shot K, replenish wingmen W, telekinesis H, whirlwind P, lightning L, area convert O, targeted invulnerability Z, capture vehicle V, eject vehicle thief Y, energy weapon X, harmonic bomb M, weaken enemy [, invulnerability ], area stun /, targeted hack ",", targeted repair ".", flame thrower J, Home One ;, saber throw T, force cloak C, force confuse U, force sight S, berserker E, radioactive contaminate −, infection I, drain life D, summon R, blast G, shield flare F, stun N, tactical bribe B, full salvo A, sensor jamming Q | |

### 4.4 Control groups [H] (COMMANDS.md §2.4)

Recall, assign and add are `ControlGroup` events (group 0–9, assign bit, add bit) scheduled through
the command queue; the selection is sim state. A second recall of the same group within 1 s also
jumps the camera to the group (local player only). The group number is drawn over each member with
`st_control_group`.

---

## 5. Load screens (P4)

### 5.1 Flow [H unless marked] (`BattleLoadDialogClass::Activate` 0x8d930, 0x8cb50, 0x8df00, 0x8eff0)

1. Activate stores the target map, mode (1 land, 2 space), planet and the original map name; picks
   the planet's land or space load texture name from the mode record if present; then 0x8cb50 opens
   one of `IDD_LAND_BATTLE_LOAD_DIALOG`, `IDD_SPACE_BATTLE_LOAD_DIALOG`,
   `IDD_SKIRMISH_BATTLE_LOAD_DIALOG`, `IDD_LAND_MISSION_LOAD_DIALOG`, `IDD_SPACE_MISSION_LOAD_DIALOG`
   and fills planet name, faction, credit production, weather/terrain/native texts and icons, the
   historical-reference list and player names; **`IDC_BEGIN_BUTTON` is hidden**; the sim stops
   (FS +0x34 = 1, MAIN_LOOP §2.4).
2. 0x8df00 starts the "LoadThread" worker; the main thread runs a small loop: progress update, network
   pump, present.
3. Progress: single player uses `IDC_PROGRESS_SOLO`; multiplayer uses `IDC_PROGRESS0..8` (one per
   player, from load-progress packets) next to `IDC_PLAYER_NAME_0..8`; text `IDC_STATIC_LOADING`
   (`TEXT_SAVELOAD_LOADING_MSG`).
4. Done (0x8eff0): progress set to full; in single player (and game type 3) **the Begin button is
   shown and focused and `GUI_Text_Hint_SFX` plays**; the player clicks `TEXT_BUTTON_BEGIN` to start.
   In MP the dialog closes when all peers report done. [M for the game-type split]
5. Galactic campaign / save loads use the same pattern with a different dialog; command-line quick
   loads draw `Splash.TGA` instead (MAIN_LOOP §2.5).

### 5.2 Minimum data for a port load screen [M]

- `GUIDialogs.rc` block for the chosen `IDD_*_LOAD_DIALOG` (975 × 718 units) and its GUIDialogs.xml
  overrides: small frame pieces, `Frame_Background` = `loadmenuback.tga`, font override for `L_Text`
  (EmpireAtWar-Medium 8, 195,171,2).
- MT_CommandBar atlas textures for frame, progress bar and push button; `EmpireAtWar-*` fonts.
- MasterTextFile keys: `TEXT_BUTTON_BEGIN`, `TEXT_SAVELOAD_LOADING_MSG`, `TEXT_HISTORICAL_REFERENCE`,
  `TEXT_TERRAIN_TYPE`, `TEXT_AVG_WEATHER`, `TEXT_NATIVE_POPULATION`, `TEXT_PREPARE_FOR_LAND_BATTLE` /
  `_SPACE_BATTLE`.
- Planet data: name, planet icon, terrain/weather/native texts and icons (from the planet type).
- Music event `Music_Event_Battle_Load_Screen`; ready sound `GUI_Text_Hint_SFX`.

---

## 6. Open items

- Base_Layer → HUD layer: identity is consistent with the data (§1.4 P2 data check); a capture would confirm. The four 2D
  selectors are answered: 0 PrimOpaque, 2 PrimAlpha, 1 PrimAdditive, 3 PrimModulate (STRATEGIC_GAPS §7.2). Open (needs RE):
  which item kinds use the plain list vs the PrimAlpha dial list; bracket / bar placement relative to the bounds,
  `Outlined_Bar`, the bracket text label, and the category-bit → size-class table (§1.4).
- ~~Use site of `Health_Bar_Scale`/`Land_Health_Bar_Scale`/`Min_Health_Bar_Scale`~~: none, only the constructor
  touches them (STRATEGIC_GAPS §7.2). Scroll acceleration / deceleration and the unused `Camera_Stop_*`: §7.1 there.
- Roles of the overlay object 0x140a28c10 and the targeting controller 0x140b27a10 (likely cinematic
  letterbox and ability/beacon targeting mode).
- Slots 143–152: confirm Alt modifier and action (camera jump to group).
- Dialog-unit → pixel factor and where the menu column dialogs are anchored at widescreen.
- Rally point visuals; whether `Pixel_Align` and `Snap_Location` change placement.
- In-game probe (Gabriel): screenshots at 1024×768, 1920×1080 and 1280×1024 of the tactical HUD and
  a load screen would settle §2.4/§3.3.
- ~~§7.4: the organiser's "pathfinder" header pad and `Pathfinder_Slot_Index`~~ (answered, p4uio t01); §7.2 the slot-0 →
  pane 2 reading for a foreign orbit [M].

---

## 7. Fleet organiser panes for one, two or three fleets (p4ui t04, 2026-11-02)

Answers the P4 open item "which `i_galactic_organize` panels show for 1..3 fleets". Read in the organiser
update 0x3e83a0 and its two fill routines 0x3e2cf0 / 0x3e1b70; component ids from the command-bar table
(0xa174e0, GALACTIC_DIALOGS.md header); pane pad bases from the 3-entry int table at 0x140a28880
(0x79, 0x9c, 0xbf). Companion: GALACTIC_DIALOGS.md §4 (when the organiser shows), GALAXY_MAP_PRESENTATION.md
§1.3 (the map's slot rule, which the organiser repeats).

### 7.1 Panes are not chosen by the fleet count [H]
- The three frames `q1_frame` / `q2_frame` / `q3_frame` are **meshes of the shell model**, not components (no
  `q*_frame` entry in the component table, no string for them in the executable), so they draw whenever the
  shell `i_main_organize` (0x5a) draws. Nothing in the organiser code hides a frame.
- The tab labels `text_fleet1..3` (0x271..0x273) are set to `TEXT_FLEET_PANE_0..2` and shown for **all three**
  panes on every update, whatever the number of fleets (loop of 3 in 0x3e83a0).
- So with one, two or three fleets (or none) the organiser always shows **three panes**, "Fleet 1/2/3" from left
  to right. Only the pane contents change: which fleet fills which pane (§7.2) and which unit pads show (§7.3).

### 7.2 Which fleet fills which pane [H unless marked]
Pane k = frame `q{k+1}_frame`; its unit pads are `q{k+1}_0001 .. q{k+1}_0406` (34 pads, row-major `q{n}_RRCC`,
R 0..4, C 0..6, `_0000` excluded); its header pad is `q{k+1}_0000` (§7.4). A fleet's **slot** is its own orbit
slot 0..2 (fleet +0xc0 → +0x48), the field the map buttons use (GALAXY_MAP_PRESENTATION §1.3). "Orbit owner" =
0x3f9270: the forced owner of a contested planet if one is set, else the owner of the first non-stealthed
slotted fleet in orbit order, else the first stealthed one's, else nobody.

| orbit owner | pane 0 (`q1`) | pane 1 (`q2`) | pane 2 (`q3`) |
|---|---|---|---|
| the local player (0x3e2cf0) | the units of **every** local-player fleet in orbit with slot 0, merged into one pane (0x3f9370 lists the orbit's fleets with a given slot) | the fleet in slot 1 | the fleet in slot 2 |
| anyone else, or nobody (0x3e1b70) | the units of every **non-local** fleet in orbit (slots 0..2), a fleet included only if allied with the local player (0x39a230) or the planet is visible to the local player (0x50b3f0 / 0x50b5a0) and the fleet is not stealthed (0x3a9760); drawn with the pad draw's "foreign" flag (0x309a80 argument 6) | the local player's slot-1 fleet(s) | the local player's slot-0 **and** slot-2 fleets [M: the decompiled loop maps slot 0 → pane 2, 1 → 1, 2 → 2, so 0 and 2 share pane 2] |

- This is the map rule of GALAXY_MAP_PRESENTATION §1.3 ("enemy fleets go to slot 0, an own slot-0 fleet moves to
  slot 2") applied to the panes: pane k shows what map button k shows.
- Inside a pane the units are grouped per type into stacks (one pad per stack, the count as text), filled from the
  pane's first pad upward in the order the fleets list them (0x3cc040 / 0x3d7470); at most 34 stacks per pane,
  further stacks are not drawn [M]. Port (p4uio t03): stacks are made per fleet and unit type (the host feeds the object
  type index), so a pane holding several fleets (own slot 0, foreign) shows one stack per fleet and type, and a drag
  names one fleet; the stack's count is the pad text, a click selects all its members, a drag takes its first unit only.
- A pane with no fleet keeps its frame and its tab; its unit pads are empty (§7.3).
- Drops (0x3d02e0, unchanged): a drop on pane k addresses the fleet in slot k; a drop on an empty pane makes a new
  fleet in slot k (FleetManagement with the slot), on an occupied one merges ("merge_fleets").

### 7.3 Unit pads that show [M]
- Pads holding a stack are shown and enabled.
- Every pad of a pane is reset first; the reset hides it unless the HUD's drag / hover component (+0x2160) is set,
  in which case it is shown empty and enabled. Reading: **empty pads show only while something is dragged** (as
  drop targets); otherwise a pane shows only its filled pads. Port (p4uio t02): while a drag is active every unit pad
  without a stack draws empty; a drop on an empty pad of an empty pane k makes a new fleet in slot k (the map's
  slot-oval rule: unit → detach with slot, fleet header → op 8, land unit → launch as a new fleet), on an empty pad of
  an occupied pane it merges into that pane's first fleet, and a foreign pane is no target.

### 7.4 The `q*_0000` header pad ("Fleet PF", the fleet's pathfinder) [H unless marked] (p4uio t01, 2026-11-03)
- `q{n}_0000` is in group `Fleet PF`, blank texture `i_button_pathfinder.tga`, tooltip `TEXT_TOOLTIP_PATHFINDER`.
  Both fill routines show and enable it whenever the organiser updates (pane base − 1), **filled or not**: an empty
  header pad draws its blank pathfinder texture. It is not one of the 34 unit pads (units start at `_0001`).
- **The index.** `Pathfinder_Slot_Index` = GameConstants +0x120b0 (0x140b278b0, int, parser type 0x6). The GameConstants
  defaults routine 0x2c91c0 sets it to **999**; the tag is in neither FoC nor EaW `GAMECONSTANTS.XML` (only
  `Preferred_Pathfinder_Types`, an unrelated invasion list), so vanilla runs with 999. The code also handles −1 (then
  every comparison below matches "no index"), but nothing sets −1.
- **The unit field.** Unit +0x1b0 is a lazily allocated per-unit UI record (0x38 bytes, ctor 0x564ed0); +8 is the unit's
  **GUI index**, −1 when there is no record. Setter 0x3a87c0(unit, idx) (−1 clears). The network-safe copy is the
  event `SetGUIIndexEventClass` (ctor 0x5b1170, args 0x5b06b0(obj id, idx), sent via 0x3b01f0 on 0x140b2ed18); its
  execute 0x5b12a0 sets record +0x32 := (idx == `Pathfinder_Slot_Index`), the sim-visible **"is pathfinder"** flag.
  The only writer of 999 is the header-pad drop below, so **no unit starts as pathfinder**: in vanilla SP the pad is
  empty until the player drops a unit on it [H for the code; M that no Lua/story path sets it, none found].
- **Drop on the header pad** (0x3d63f0, a component drop callback, no direct callers): refused with a HUD message /
  sound (0x2d5290) when the dragged unit's type has table-2 property 0x2c (0x370320, the "always deployed" property of
  BATTLE_TRANSITIONS §split rule 1 [M]). Otherwise: if the pad's pane is the dragged unit's own pane (pad id + 1 has the
  same parent) the fleet's current pathfinder is cleared (0x3ccdd0: every unit with GUI index 999 → −1, with the
  event); from another pane the unit is first moved into this pane's fleet (0x3d0790). Then the unit gets GUI index
  999 (event + 0x3a87c0). So the pad is a drop target that **appoints the fleet's single pathfinder**.
- **Fill** (stack builder 0x3cac60): the first unit of the pane with GUI index 999 goes to the header pad (not into a
  stack); any further one is reset to −1 (event, then 0x3a87c0). Drop on a pane (0x3d02e0): if the target fleet already
  has a pathfinder, the dropped unit/fleet's pathfinder is cleared (0x3ccdd0) before the merge, so a merged fleet keeps
  the target's.
- **What it does in the sim.** Only the battle-start split 0x2bd500 reads +0x32: with the `flagship` argument (P14 of a
  story `LINK_TACTICAL` space battle, sandbox only; BATTLE_TRANSITIONS step 5) the first pathfinder unit starts the
  battle and the rest go to reserve ("Setting pathfinder for space battle"). Ordinary galactic space battles ignore it;
  skirmish has no organiser. [M: flagship path seen used only by story scripts; tutorial / campaign XML comments name
  `Reward_Param6` "Hero pathfinder name", which may be the same mechanism, not traced] **[L]**.
- **Port:** units start at `q{n}_0001`; keep `_0000` apart, always shown (empty = blank texture) and enabled as a drop
  target; a drop sets the unit's pathfinder flag (one per fleet, rejecting types with the always-deployed property),
  the pad shows that unit. Until the port implements `LINK_TACTICAL` flagship battles the flag has no sim effect, so a
  minimal port may draw the empty pad and ignore drops on it [M recommendation]. Port (p4uio t01): units start at
  `q{n}_0001`; `q{n}_0000` is drawn in all three panes, empty and inert (no drag, click or drop) until flagship battles
  exist.

### 7.5 Port mapping
- ui: draw all three panes and tabs whenever the organiser shows; fill the panes by slot per §7.2 (the host feeds
  each fleet's slot and whether it is the local player's), not in provider order; foreign units in pane 0 are drawn
  but not draggable; empty pads only during a drag (§7.3).
- apps: the organiser provider feeds every fleet in orbit at the planet that the local player may see (own, allied,
  or visible and not stealthed), with its slot and owner flag; the planet's `GalacticPlanetInfo::orbit_local`
  (already fed for the map) picks the table row.

---

## 8. IME edit boxes (p4ui t05, 2026-11-02)

Question: does any single-player screen open the IME dialog (text input for Asian-language keyboards)?
**Answer: no.** [H]

- The GUI has one IME-aware gadget, `GUIGadgetIMEEditboxClass` (gadget type 0x1d, made from a dialog control of class
  `"IMEEditBox"`, factory 0x1dcf90 → 0x1f3100), plus a locale indicator `GUIGadgetIMELocaleIndicatorClass`
  (`"IMELocaleIndicator"`). Only these talk to the IME: they register with the static IME message hook (list
  0x140a13740, added by 0x1d4060 from the gadget's focus paths 0x1f6700 / 0x1f84d0), and only they associate or
  dissociate the window's input context (imm32 `ImmAssociateContext`, loaded at boot by 0x1d3c20; the boot only reads
  the default context, 0x1d4700, and does not disable it).
- The IME hook is the first step of the window procedure (0x1d4170 from 0x60c60, §0 item 6). With no IME gadget
  registered it consumes nothing.
- In the shipped `GUIDialogs.rc` the only `IMEEditBox` / `IMELocaleIndicator` controls are in `IDD_IME_TEST_DIALOG`
  (`IDC_IME_TEST_EDITBOX`, `IDC_CUSTOM2`). The routine that opens it (0x12ae70; close 0x12aff0, callback 0x12b030)
  has **no caller** in the executable: a leftover developer test.
- Every other text field is a plain `EDITTEXT` (`GUIGadgetEditboxClass`): save names, profile / player names,
  passwords, lobby and in-game chat. `IDC_IME_EDITBOX` in `IDD_INGAME_CHAT_DIALOG` is, despite its name, a plain
  `EDITTEXT` (and that chat is multiplayer only). `IDC_IME_EDIT_BOX` (1208) and `IDC_IME_EDITBOX2` (1235) exist in
  `Resource.h` only.
- Plain edit boxes take characters from `WM_CHAR` (§4.2). Because the game never dissociates the window's input
  context, a system IME still composes in its own window and the committed text reaches the game as `WM_CHAR`
  [M: Windows' default handling of `WM_IME_CHAR` / `WM_IME_COMPOSITION`, not traced inside the game].

Port: nothing to build; the IME dialog is not needed for single player (t05 closed). The port's edit boxes already
take `WM_CHAR` text (P4 integrator ask 1); a host window that leaves the IME messages to `DefWindowProc` gets
committed IME text the same way.

---

## 9. Skirmish map list: official, custom and mod-folder maps (P4 GUI t06, 2026-11-02)

Answers roadmap p4ui t06. **Single-player feature, not multiplayer.** `IDD_SKIRMISH_STAGING_DIALOG` (handler 0xf2690,
the only dialog with `IDC_SKIRMISH_START_GAME_BUTTON`) has the two buttons `IDC_SELECT_OFFICIAL_MAPS` /
`IDC_SELECT_CUSTOM_MAPS` (`TEXT_BUTTON_SELECT_OFFICIAL_MAPS` / `_CUSTOM_MAPS`, GUIDialogs.rc), the game-type list
`IDC_GAME_TYPE_LIST`, the map list `IDC_STAGING_MAP_LIST` (LBS_SORT), the preview `IDC_IMAGE` and
`IDC_SCENARIO_DESCRIPTION_TEXT_BOX`. The LAN / internet staging dialogs use the same two buttons and the same catalog,
but nothing here needs them. The other "custom map" machinery is multiplayer-only and out of scope: the host-option
keys `HO_CUSTOMMAPNAME` / `HO_CUSTOMMAPCRC` (0x74d50, 0x750e0) and the map transfer that writes a received map to
`.\Data\CustomMaps\temp.ted` after `CreateDirectoryA(".\Data\CustomMaps\")` (0xcb330). [H]

### 9.1 The map catalog (object 0x140b311a8, built once at boot by 0x47cb40) [H]

It is the same table the galactic random-map picker reads (TACTICAL_GAPS §4.10, BATTLE_TRANSITIONS §2.1). Three
directory scans, each through the MEG-aware find (§9.2), each into one of two path lists:

| Scan | Pattern | List |
|---|---|---|
| 1 | `.\Data\Art\Maps\*.TED` | official |
| 2 | `.\Data\CustomMaps\*.TED` | custom |
| 3 | for each entry of the mod search list (MODPATH= / STEAMMOD= dirs in command-line order, object 0x140a14cc0, the list `FileClass::Open` searches, SUBSYSTEMS.md "File lookup priority"): `<dir>\Data\Art\Maps\*.TED` | custom |

- Scan 3 puts mod-folder maps into the **custom** list, not the official one (the list argument of the third call is
  the custom list; checked in the disassembly at 0x14047cd58). A mod's `Data\CustomMaps` folder and the
  `FALLBACKPATH=` / EaW `GameData` loose folders are never scanned. EaW and mod **MEGs** are scanned (scan 1 sees
  every mounted archive), so a mod whose `Data\MegaFiles.xml` mounts its own `Data\Maps.meg` (Thrawn's Revenge:
  108 map entries, next to ~600 loose maps that are mostly campaign maps) gets its MEG maps in the official list.
- Each scan appends `<directory of the pattern> + <found file name>` (the pattern up to and including its last `\`,
  so `.\Data\Art\Maps\_MP_LAND_ENDOR.TED` or `<dir>\Data\Art\Maps\X.ted`), skipping a path string already in that
  list (exact, case-sensitive compare). The same file name under two different directories is two entries.
- Every path is then opened with the normal `FileClass` open (mod search list > FoC loose > MEGs > fallbacks,
  PHASE2.md), so an official `.\Data\Art\Maps\X.TED` whose name a mod overrides reads the mod's copy. Each file's
  header (§9.3) fills one 0xf0-byte entry (0x47b030, logs `   Parsing found map '%s'`). Official entries are all
  kept, even unreadable ones; custom entries (scans 2 and 3) are kept **only when the header's custom flag (mini `0B`)
  is 1**. Table order: official in scan order, then the kept custom entries.
- Correction to TACTICAL_GAPS §4.10: +0x00 is the path and +0x80 the upper-cased mini 0A tag (the `CONTROL` filter
  reads it); +0xb4 is the custom flag, so the picker's "+0xb4 = 0" means "official maps only".

### 9.2 The MEG-aware find (0x217e80 first, 0x218e40 next, 0x2199d0 close) [H]

- First the loose directory, by `FindFirstFileA` on the pattern as given (relative to the working directory, which
  is the FoC install; scan 3 uses the mod dir); `.` and `..` are skipped. Every name returned is remembered in a
  per-search set, keyed by the full path trimmed, `/` → `\` and upper-cased (0x2005e0, 0x200ba0).
- When the loose search has no (more) files, every mounted MEG is walked in **mount order** (first mounted first:
  EaW before FoC), entries in archive index order. An entry is returned when its directory equals the pattern's
  directory after the same normalisation (case-insensitive; a leading `.\` matches the stored `DATA\...` names, since
  the retail skirmish maps exist only in `maps.meg` / `Patch*.meg` and are listed [M for the `.\` step]) and its file
  name matches the pattern's wildcard, and its path is not in the set yet. So a name present loose **and** in a MEG,
  or in several MEGs, is returned **once**, loose first.
- The returned name is the loose name as on disk, or the MEG entry's file name (upper case).

### 9.3 Header read: top chunk 0 of the `.ted` (0x47b030) [H fields, M meanings]

Chunk layout: TERRAIN_WATER_POST.md §1 (the port's `terrain::MapInfo` already decodes it). Minis of top chunk 0:

| Mini | Type | Entry | Meaning (vanilla values) |
|---|---|---|---|
| 01 | u32 | +0xa0 | map kind: 1 land, 2 space (default −1) |
| 02 | u32 | +0xac | player count, default 1 (vanilla MP maps 2–9, campaign maps 1) |
| 03 | u32 | +0xb0 | max base level (5) [M] |
| 05 | u32 | +0xa8 | faction index for the random picker (3 in vanilla) [M] |
| 06 | u32 | +0xa4 | terrain index (TEMPERATE 0 … URBAN 6, SPACE 7) |
| 08 | UTF-16 | +0x20 | map-name text key (`TEXT_MAP_NAME_MP_LAND_10`); without it +0x20 holds the path |
| 09 | UTF-16 | +0x60 | planet / display name as written (`Endor`); not used by the list [M] |
| 0A | UTF-16 | +0x80 | game-mode tag, stored upper-cased (`CONTROL` on most land MP maps); only game type 4 filters on it |
| 0B | u8 | +0xb4 | **custom-map flag**: 0 official, 1 custom |
| 10, 11 | f32 | +0xbc, +0xc0 | world width / height |
| 12 | u8 | +0xb5 | the map has start positions for the preview overlay (1 on 24 of the 48 patched vanilla MP maps) |
| 00, 04, 07 | u32 | — | read, not stored (00 = format 513) |

Top chunk 3 (Vector2 list, the start positions) → +0xc8 (count +0xd8); top chunk 0x13 (raw bytes, the embedded
preview image: 262 175 bytes in the three `Patch2.meg` maps that carry one) → +0xe0 / +0xe8.

**List label** (+0x40): only when mini 08 is present: `"(%d) %s"` with the player count and the localised text of the
key; if the text database answers `[MISSING]`, the raw key is used instead. Without mini 08 the list falls back to the
file name (§9.4).

### 9.4 Filling the skirmish list (0xfb950 → filter 0x47bf40) [H unless tagged]

- `IDC_GAME_TYPE_LIST` sets the game type: 2 = land, 3 = space (0xf2690). The game type maps to the map kind 1 / 2
  (0x28ab20; type 1 → kind 0, type 4 → kind 1). The Official / Custom buttons toggle one flag (0x140a62282, official
  when the dialog opens) and refill the list.
- Filter 0x47bf40, called with (game type, terrain −1 = any, faction −1 = any, base level 0, minimum players 2,
  custom flag, no exclusion list): an entry is listed when its kind is the game type's kind, players (+0xac) ≥ 2,
  max base level (+0xb0) ≥ 0, and its custom flag (+0xb4) **equals** the Official / Custom choice. Game type 4 would
  also need `CONTROL` in mini 0A and honour `Quickmatch_Map_Exclusion_List`; skirmish (2 / 3) does neither.
- Rows in table order; each row's text is the entry's label (+0x40) when set, else the file name without directory
  and `.TED` (found case-insensitively), cut to 22 characters + `...` when longer than 25 (0xfb7e0). The list is then
  sorted (comparator 0x104510: a per-row integer first [M: the same for every row here], then the row text,
  `_stricmp`), so the player sees the rows ordered by label, `(2) …` before `(4) …`.
- Selection: the map chosen last time for that game type (strings at 0x140a7c2c8 land / 0x140a7c2e8 space) if it is
  still listed, else the first row.
- Vanilla result after the patches: 24 land maps (`_MP_LAND_*`) and 24 space maps (`_MP_SPACE_*`), all official; no
  custom maps. `_MP_LAND_DATHOMIR` / `_MP_LAND_UTAPAU` have 1 player in `maps.meg` and are listed only because
  `Patch2.meg`'s copies (4 / 6 players) win the file open. No campaign map passes (all have 1 player).
- Preview `IDC_IMAGE` (0xfacb0) [H, not needed for t06]: no map → `Galactic_Back.tga`; else the texture
  `<file name without directory and extension>.tga`; if that does not exist, the embedded chunk-0x13 image (texture
  `temp_map_preview_texture`); else `Generic_Space.tga` / `Generic_Land.tga`. When mini 12 is set the start
  positions are drawn over it (0x1032f0).

### 9.5 Port rules for the implementation

- Catalog built once (first use), immutable, in a deterministic order (never the VFS's hash or pointer order).
  Official = `Data\Art\Maps\*.TED` from the FoC-loose and MEG layers; custom = `Data\CustomMaps\*.TED` from the
  FoC-loose and MEG layers plus `Data\Art\Maps\*.TED` from the mod layer. Each map is read through the normal VFS
  lookup.
- [port] Entries are keyed by the logical path, so a mod-folder map whose name two mod dirs share is one entry (the
  VFS reads the earlier dir's file; the original lists two rows that load different files). The same logical path can
  sit in both lists; the custom flag decides which tab shows it, as in the original.
- `IDC_GAME_TYPE_LIST` (p4uim t01): two rows, Land then Space (`TEXT_GAME_TYPE_TEAM_LAND` / `_TEAM_SPACE`), and the map
  list shows only the rows of that type; a map's kind comes from mini 01 (never its name), each list is sorted by row text,
  case-insensitively, ties by path. [port] The original's third row, Capture Control Points (game type 4, 9.7), is not
  offered. The row selected when the dialog opens: 9.7 (p4uis t03).
- The map chosen last is remembered **per game type** (9.7): the selected row of the map list is stored for its type
  whenever it changes, and any refill (opening, tab switch, type switch) selects that map when the active tab lists it,
  else row 0, as the original (9.7; modc t19 dropped the port's Kuat-on-Space default).

### 9.6 Preview, start overlay, description and colours (p4uim t03 / t04, p4uia t01 / t02; p4uis t01 / t02, 2026-11-03)

- **Colour combos** (`IDC_PLAYER_COLOR_n`, UI.md 3.7.1): the used slots (local player + one AI [port]) are filled with
  the 9 palette entries from the host (`MP_Color_*`, byte RGB), one U+25A0 item per entry; the default colour is the slot
  index. What happens when two players pick the same colour: 9.7. Start carries the colour index of each player in player
  order; the host applies it to the battle's team colours (presentation only: the sim never reads a colour). Without a
  palette the combos stay hidden.
- **Preview** `IDC_IMAGE` (9.4, 0xfacb0): the host picks `<file stem>.tga`, else the map's embedded top chunk 0x13, else
  `Generic_Space.tga` / `Generic_Land.tga`. The shipped `_MP_*` previews are `Data\Art\Textures\<stem>.DDS` found through
  the `.tga` name; chunk 0x13 is a complete **TGA file** (256 x 256, 32 bit, `Saved by ...` id field; 262 175 bytes in
  the three `Patch2.meg` maps that have one: `_MP_LAND_HONOGHR`, `_MP_LAND_UTAPAU`, `_MP_SPACE_DATHOMIR`) decoded in memory
  and registered under `temp_map_preview_texture:<stem>`.

#### 9.6.1 Start overlay (0x1032f0, called by 0xfacb0 when the entry's mini 12 byte +0xb5 is set) [H unless tagged]

- 0xfacb0 first removes the overlay of the previous map (0xfc9a0, also called at the top of 0x1032f0), so a map without
  mini 12, or no map, shows no overlay.
- **Marker = a number, not art**: start i (0-based, top chunk 3 order) is the text `"%d"` of **i + 1** (wide format at
  0x1407ffa20), drawn in **opaque white** (255, 255, 255, 255; 0x1f93e0) in the font of the `IDC_IMAGE` control (its font
  handle and two style bytes, control +0x20 / +0x4c / +0x4d, read through 0x1c3140). No per-player colour, no square,
  no rim. Each text is centred on its point: its left = x - text width / 2, its top = y - text height / 2 (0x25b900 /
  0x25b880 measure it).
- **Drawing area**: the `IDC_IMAGE` rectangle (position 0x1c2bf0, size 0x1c2c90) inset by 5 % of its size on each side:
  left = x + 0.05 w, top = y + 0.05 h, width 0.9 w, height 0.9 h.
- **World to fraction** (world width +0xbc, height +0xc0 from minis 10 / 11; nothing is drawn when either is 0): on the
  Space game type (3) the point is first shifted by + width / 2, + height / 2 (space maps are centred on the origin); land
  maps are not shifted (origin at the lower-left corner). Then fx = x / width, fy = y / height, each clamped to [0, 1].
  Screen point = (left + fx * area width, top + (1 - fy) * area height): world +Y is up, the image's y runs down. This
  confirms the earlier [M] reading of the patched `_MP_*` maps.
- Side effects in 0xfacb0, not needed by the port's two-player staging: with the overlay drawn it records the number of
  start positions (0x140a62290, which caps the `IDC_PLAYER_TEAM_n` choice) and the map's player count (0x140a62294,
  9 otherwise).

#### 9.6.2 Description box `IDC_SCENARIO_DESCRIPTION_TEXT_BOX` (p4uis t01) [H]

- In the skirmish staging dialog the box is **switched off and never filled**: the dialog's open path (0xf1230, after the
  first map-list fill) and every `IDC_STAGING_MAP_LIST` selection change (0xf2690) call the control's show (0x1c30d0, control
  vtable +0x50; the same call GALACTIC_DIALOGS.md reads as switching `IDC_MAP_ICON` off) with 0, and nothing in the skirmish
  path sets its text or switches it on. In `GUIDialogs.rc` it is an empty `LTEXT` (22,85,179,167) inside the `IDC_IMAGE`
  rectangle (17,73,190,192), so with it off nothing covers the preview. Per-map descriptions do not exist: the text
  database has only `TEXT_MAP_NAME_*` for maps.
- The only code that fills it is an **uncalled** historical-scenario mode of the same dialog (no callers in the call
  graph): 0xf4880 lists seven `TEXT_SCENARIO_*_NAME` rows (Hoth Assault, ...) in `IDC_STAGING_MAP_LIST`, and 0xf4ad0 switches
  the box on and sets `TEXT_SCENARIO_*_DESCRIPTION` for the picked one. The keys are in the shipped text database, but the
  mode is dead in the retail game; the port does not offer it.
- Port: the box is hidden when the dialog opens and stays hidden; the earlier [port] text (`TEXT_GAME_DESCRIPTION_TEAM_*`)
  is dropped.

### 9.7 Game type on opening, remembered map and colour clashes (p4uis t03, 2026-11-03) [H unless tagged]

- **Rows**: the open path (0xf1230) adds game types 2, 3, 4 to `IDC_GAME_TYPE_LIST` (LBS_SORT), each row's text from the
  key table at 0x140a11458 (`TEXT_GAME_TYPE_INVALID`, `_CAMPAIGN`, `_TEAM_LAND`, `_TEAM_SPACE`, `_CAPTURE_CONTROL_POINTS`)
  and its item data the type, then selects the row whose data is the current type (0x1c4840). Type 4 lists only land maps
  with the `CONTROL` tag (9.4). The port offers types 2 and 3 only [port], see 9.5.
- **Type on opening** = the player profile's `LastInternetGameType` + 1 when the stored value is 1..3 (types 2..4), else
  Land (2). The profile (`PlayerProfileClass`, constructor 0x202510) defaults `LastInternetGameType` to **1**, so a first
  opening with a fresh profile is on **Land**. Changing the game type stores type - 1 in the profile at once and saves it
  (0x203330; the same key the LAN / internet dialogs use), so later openings, also after a restart, are on the type
  selected last, not on the type of the map started last.
- **Remembered map**: every `IDC_STAGING_MAP_LIST` selection change stores the row's map path in the profile key
  `LastInternetMapLand` (any type but 3) or `LastInternetMapSpace` (type 3) and saves the profile; a type switch posts a
  selection change for the row the refill selected, so that row is stored too. The list fill (0xfb950) selects the stored
  map of the type when it is listed, else row 0 (9.4).
- **Colour change** (`IDC_PLAYER_COLOR_n` in 0xf2690; 0x288c0 is the LAN / internet dialog's handler, out of scope): the
  chosen item is stored as slot n's colour as it is, with no check against the other players; for slot 0 it is also saved
  as the profile's `Color`. Then the player rows are rebuilt (0xfe7c0, 0x1023d0 per slot) and the clash rule runs in
  **slot order**: slot 0 keeps its colour (clamped to 0..9); each later slot whose colour (< 9) an earlier slot already
  uses is moved to the **lowest colour index no earlier slot uses**, and its combo shows that. So nothing is refused and
  nobody shares a colour, but it is not a swap: the earlier slot wins and the later one takes the lowest free index.
  Two-player examples: slot 0 = 0, slot 1 = 1, slot 0 picks 1: slot 1 becomes 0. Slot 1 picks 0: slot 1 becomes 1 again
  (the lowest index slot 0 does not use). Slot 0 = 3, slot 1 = 1, slot 0 picks 1: slot 1 becomes 0, not 3.
- Port: first opening on Land; the type selected last is kept for the session [port: the port's only profile file holds
  the video options, so the type and the remembered maps do not survive a restart]; the remembered map follows the list selection (not Start); colours follow the
  slot-order clash rule above.
