// P4 galactic HUD (docs/re/UI.md §1.6, §2.2-2.3): the galactic command bar shell (i_main_commandbar =
// i_galactic_controls.alo), the fleet organiser (i_main_organize = i_galactic_organize.alo), per-planet labels, the
// selected planet's info, build buttons and queue, fleet / land-force pads and the story objectives panel.
//
// Data (CommandBarComponents.xml):
//   planet labels   g_planet_name (EmpireAtWar-Bold 7, Offset 30 0), g_planet_value (credits, Offset 30 -11),
//                   g_political_control (6-level bar), g_ground_level / g_space_level (5-level bars, Default_Offset
//                   72 -26 / 72 -13), g_conflict / g_hero / g_smuggler / g_bounty_hunter icons (Offset 30 -16, row),
//                   g_planet_ring (every planet); alpha by galactic zoom (§1.6): state 1 -> 255 max(0, 1 - 10 z),
//                   state 2 -> 255 max(0, 10 (z - 0.9)), state 3 -> 0, [port] state 0 -> 255
//   top bar text    Text_Credits, Text_Pop_Cap, Text_Day_Counter, Text_Planet, Text_Planetary_Income, Text_Tech_Level
//   build           b_Create_NN (one grid: structures, then units) with Build_Texture_Name dials; queueNN
//                   (Build_Queue) -> commands::ev::production (cancel on click)
//   fleets          q1/q2/q3_RRCC (rows 00..04 x columns 00..06; RRCC 0000 = the fleet's first unit), q1..q4_fleet,
//                   land_fleet_00..19 -> HudAction::GalacticSelect
//   objectives      objective_back (i_tooltip_back + faction frame, Size 266 x 14, Default_Offset 383 -180 /
//                   _Widescreen 530 -350), objective_header_text (Text_Color2 = failed), objective_text (Size x = line
//                   length, y = line spacing; Text_Color2 = completed), objective_icon (checked_box / failed_box)
//   abilities       g_special_ability: one row per GalacticProviders::special_abilities entry beside the selected planet
//                   -> HudAction::Button "g_special_ability" (unit = hero). [port] row i centre = planet centre +
//                   (Offset.x, Offset.y x (i + 1)) (Offset -48 -20: rows left of the planet, 20 units apart going down,
//                   +Y up like the labels); Blank_Texture_Name frame, icon = the row's icon or
//                   Icon_Alternate_Texture_Name[faction], Mouse_Over_Texture_Name[faction] on the row under the cursor
//                   only, text at Text_Offset (+Y down, Right_Justified); label alpha; at most 8 rows.
// [port] choices: label Offset / Default_Offset read with +Y UP (the data comment on g_conflict: "y = distance to shift the
// status icons down" with -16); planet name coloured by the owner; status icons in a row 2 units apart; political
// control level = round(control x (levels - 1)); fleet pad order row-major; objectives panel anchored at
// Default_Offset (canvas centre + offset, +Y down) - widescreen variant above 4:3; objective lines wrapped at
// objective_back Size.x - 24 units; without shell bones components sit at apply_galactic_fallback_layout positions.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/ui/hud_buttons.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/tactical_hud.hpp"

namespace alamo::ui {

class FontCache;

float planet_label_alpha(int32_t zoom_state, float zoom_fraction);

// GALAXY_MAP_PRESENTATION §1 (supersedes the row layout): one entry per orbiting fleet (up to three buttons around the
// planet: slot 0 upper-left, 1 above, 2 upper-right) plus one land-forces entry (button just above the planet centre).
// Host feed per fleet: owner (enemy), unit count, representative icon (§1.4: hero with the lowest Ranking_In_Category,
// else the unit with the highest max tactical health; Icon_Name), its slot, stealth / raid flags.
struct GalacticPlanetStack {
    int32_t faction = 0;        // owner's Icon_Alternate_Texture_Name index: fleet tooltip only (never drawn on the map)
    std::string icon;           // representative unit's Icon_Name ("" = stack only)
    uint32_t count = 1;         // pancake layers 0..5 when `units` is 0 (older feeds)
    uint32_t units = 0;         // unit count: layers = min(ceil(units / Ships_Per_Stack 5), 5)
    int32_t slot = -1;          // the fleet's own orbit slot 0..2 (fleet +0xc0 -> +0x48); -1 = first free
    bool enemy = false;         // not the local player's: takes slot 0 when the orbit is not the local player's
    bool stealth = false;       // every unit stealthed -> i_icon_stealth.tga badge
    bool raid = false;          // raid fleet -> i_icon_raid.tga badge (stealth wins)
    bool land = false;          // g_planet_land_forces instead of g_planet_fleet (first land entry only)
    bool selected = false;
    // The fleet object this button stands for (host feed; 0 / kNoObject = not clickable). Left click on the button ->
    // HudAction GalacticSelect {ids = {fleet}, unit = planet, value = slot 0..2 (-1 land)}; right click: nothing [port].
    // A press on an own fleet's button (enemy = false, g_planet_fleet Drag_And_Drop) that moves > 4 px drags the fleet
    // (GalacticDrag fleet_index = kMapFleetIndex, planet = its planet): drop on another planet -> MoveObjectToObject
    // {fleet, planet}; on an organiser fleet pad -> FleetManagement merge; on the land pad -> land (only at the selected
    // planet). Land-forces entries are click-only.
    uint32_t fleet = 0;
    // GALAXY_MAP_PRESENTATION §1.4 (0x3df060): the button's colour is white and its secondary colour the owner faction's.
    // [M] drawn as a tint of the pancake stack texture (the rim is grey / white art; the unit icon and the badges stay
    // white). Host: the owner's Factions.xml <Color> (which faction field the original reads is not traced).
    Rgba8 owner_color{255, 255, 255, 255};
};
// Credit amounts can be negative: this one means "no value, blank text".
inline constexpr int64_t kNoGalacticAmount = INT64_MIN;
struct GalacticPlanetInfo {
    uint32_t planet = 0;     // planet object id
    Vec3 position{};
    float radius = 50.0f;    // world radius (ring size)
    std::u32string name;
    int32_t credits = -1;    // g_planet_value: the planet's credit value, shown while the b_filters0 toggle is on; < 0: no value text
    int64_t income = kNoGalacticAmount;  // Text_Planetary_Income of the selected planet, may be negative
    Rgba8 owner_color{255, 255, 255, 255};
    float political_control = -1.0f;  // 0..1, < 0 none
    int32_t ground_level = -1, space_level = -1;  // starbase / ground base levels (bar index), < 0 none
    // REMAINING_GAPS §6.3 display gates (host: GalacticSim::can_see_starbase_info / can_see_groundbase_info for the local
    // player): false hides g_space_level / g_ground_level whatever the level.
    bool starbase_info = true, groundbase_info = true;
    bool conflict = false, smuggler = false, bounty_hunter = false;
    // GALAXY_MAP_PRESENTATION §1.5: one g_hero medal per faction with a hero on the surface (orbiting heroes show only in
    // their fleet slot, Gabriel 2026-10-29) [inferred: surface only]. The you / enemy alternates (mode flag not traced) are unused.
    bool hero_rebel = false, hero_empire = false, hero_underworld = false;
    bool selected = false, visible = true;
    // GALACTIC_DIALOGS §5.3: Text_Allegiance_A emblem of the owner (Icon_Alternate_Texture_Name index: 0 rebel, 1 empire,
    // 2 underworld, 3 pirate, 4 jabba / hutt), < 0 = no emblem (neutral). Host: the owner faction's index.
    int32_t allegiance = -1;
    // GALAXY_MAP_PRESENTATION §1.3: the orbit owner is the local player (fleets keep their slots); false = enemy fleets
    // go to slot 0, an own slot-0 fleet to slot 2.
    bool orbit_local = true;
    // OI 2026-10-28 [port: Gabriel, original screenshot]: fogged (render kGalFogged): the name stays, the garrison /
    // stack icons are hidden. garrison_icon: the strongest garrison unit's Icon_Name, drawn inside the planet;
    // stacks: fleet / land-force stacks under the name (orbiting heroes show only in their fleet slot, Gabriel 2026-10-29). Planet name drawn upper-case in owner_color (host: the owner faction's colour,
    // neutral grey).
    bool fogged = false;
    std::string garrison_icon;
    std::vector<GalacticPlanetStack> stacks;
    // ui4 t03 (GALAXY_MAP_PRESENTATION §1.1 / §1.2; [port] choices in docs/PHASE4.md "ui4 t03"). Filter-gated: the planet
    // type's Encyclopedia_Weather_Icon on g_weather and Planet_Ability_Icon on g_planet_ability (b_filters2, "" = none);
    // credit_level 0..10 on g_credit_bar (b_filters0 with the value text, < 0 none). Status row after g_conflict / g_hero:
    // g_build (the local player builds here), g_smuggled (an income siphon is active here), g_corruption_icon (corrupted),
    // g_enemy_hero (an enemy hero is seen here: Icon_Alternate 0 i_icon_hero, 1 i_icon_empire_hero; < 0 none).
    // g_corruption_text: the corruption transition line (TEXT_CORRUPTION_PERCENT / _REMOVE_PERCENT), "" none.
    std::string weather_icon, ability_icon;
    int32_t credit_level = -1;
    bool building = false, smuggled = false, corrupted = false;
    int32_t enemy_hero = -1;
    std::u32string corruption_text;
};
// ui4 t03: the galactic map filters (GALAXY_MAP_PRESENTATION §1.1, HUD +0x21c4 bits 0x3cd120): b_filters0 credit value,
// b_filters1 base level (ground / space level pips), b_filters2 ability / weather, b_filters3 hero (the fleet button's
// representative icon prefers the hero). Toggles, all off at start.
inline constexpr uint32_t kFilterCredit = 1, kFilterBaseLevel = 2, kFilterAbilityWeather = 4, kFilterHero = 8;
// GalacticFleetSlot::type of a unit the host did not classify: it is never merged into a stack.
inline constexpr uint32_t kNoSlotType = 0xFFFFFFFFu;
struct GalacticFleetSlot {
    uint32_t object = 0;
    std::string icon;
    uint32_t count = 1;
    bool selected = false;
    // UI.md §7.2: units of one fleet with the same type share one organiser pad (a stack with a count). Host: the unit's
    // object type index; kNoSlotType = its own stack. Land-force slots ignore it.
    uint32_t type = kNoSlotType;
};
// One fleet in orbit at the selected planet, as the organiser sees it (UI.md §7.2). The HUD, not the host, decides which
// of the three panes q1 / q2 / q3 it fills, from `slot`, `enemy` and the planet's GalacticPlanetInfo::orbit_local; the
// provider order only breaks ties. Several fleets can share a pane (own slot-0 fleets, every foreign fleet): their stacks
// follow each other from the pane's first pad.
struct GalacticFleet {
    uint32_t fleet = 0;                 // fleet object id
    std::vector<GalacticFleetSlot> units;
    int32_t slot = -1;                  // the fleet's own orbit slot 0..2 (galactic FleetState::slot); -1 = unknown, first free pane [port]
    bool enemy = false;                 // not the local player's (foreign or allied): drawn in pane 0, not draggable, clicks do nothing [port]
};
struct GalacticBuildOption {
    uint16_t type_index = 0;
    std::string icon;
    int32_t cost = -1;
    bool enabled = true;
    bool structure = false;             // placed before the units in the b_Create_NN grid
    std::string tooltip_key;
    // The player's production-disabled list (story ENABLE / DISABLE_BUILDABLE, REMAINING_GAPS §1.5: read by the GUI build
    // bar 0x3145c0, not by buildability). Host: GalacticSim::production_disabled(player, type). [port] such options take
    // no pad (hidden) unless GalacticHudFrame::show_production_disabled, then drawn greyed with clicks ignored.
    bool production_disabled = false;
    // GALACTIC_DIALOGS §5.1: the build tab the option belongs to (space = true -> blue pad i_button_pad_empire, Blank
    // index 1; ground -> amber i_button_pad_rebel, index 0). Host: true for space units / starbases.
    bool space = false;
    // ui4 t02: why the pad is greyed (GalacticSim::can_enqueue at the planet). != None -> disabled; the b_Create_NN
    // Tooltip_Text entry follows the reason (0 TEXT_PRODUCTION_QUEUE_FULL, 1 _POP_CAP_REACHED, 2 _INSUFFICIENT_FUNDS; other
    // reasons: no tooltip) and a left click plays the GUI negative feedback (HudAction::BuildRefused) instead of sending
    // a Production command the sim would refuse. Host: also set enabled = (block == None).
    HudBuildOption::Block block = HudBuildOption::Block::None;
    uint32_t queued = 0;  // the local player's entries of this type in the planet's queues (Text_Offset2 line, > 0 shown)
};
// HudObjective: tactical_hud.hpp.
// One g_special_ability row (CommandBarComponents: TextButton beside the planet, "x = offset to right of planet, y =
// offset to next ability"): a hero / unit at the planet with a usable galactic special ability. Clicking a row emits
// HudAction{Button, name "g_special_ability", unit = hero, ids = {planet}, value = row} - the SpecialAbility activate
// request the host turns into the command (activate_special_here).
struct GalacticSpecialAbility {
    uint32_t hero = 0;          // object id of the hero / unit that activates it
    std::u32string text;        // row text (e.g. the ability or hero name); "" = text_key
    std::string text_key;       // MasterTextFile key
    std::string icon;           // icon over the frame ("" = Icon_Alternate_Texture_Name[faction]: the faction symbol)
    bool enabled = true;        // false: drawn darkened, clicks ignored
};
struct GalacticProviders {
    std::function<void(uint32_t planet, std::vector<GalacticBuildOption>& out)> build_options;
    std::function<void(std::vector<HudQueueEntry>& out)> build_queue;
    // Organiser (UI.md §7): every fleet in orbit at the planet that the local player may see (own, allied, or visible and
    // not stealthed), each with its slot and enemy flag; any number, the HUD places them in the three panes.
    std::function<void(uint32_t planet, std::vector<GalacticFleet>& out)> fleets;
    std::function<void(uint32_t planet, std::vector<GalacticFleetSlot>& out)> land_forces;  // max 20
    std::function<void(std::vector<HudObjective>& out)> objectives;                        // StorySession objectives
    // g_special_ability rows for the selected planet, in display order (host: the local player's units there with a
    // Ground_Activated special ability, P4_SIM_SYSTEMS).
    std::function<void(uint32_t planet, std::vector<GalacticSpecialAbility>& out)> special_abilities;
};

// GALACTIC_DIALOGS §1: the battle-pending prompt. Shells pending_battle_shell (i_attack_mode_choice.alo: Begin +
// Auto-Resolve) and story_pending_battle_shell (i_attack_mode_single.alo: Begin only), top-centre; host loads both into
// the galactic ShellSet and applies GalacticHud::shell_visibility(). Title = planet + TEXT_TACTICAL_{SPACE,LAND}_BATTLE_
// IS_PENDING in Battle_Pending_Message_Color. graph_left / graph_right: the side's icon on a fleet_empty /
// fleet_stacks_01..05 pancake stack (offset 3 per layer, icon scale 0.75). No Retreat. Clicks: HudAction BattleBegin /
// BattleAutoResolve. The galaxy stays paused while it is up (host).
struct GalacticBattleSide {
    std::string icon;        // representative icon (hero / commander portrait, else the lead unit's Icon_Name)
    uint32_t units = 0;      // force size
    int32_t stack = -1;      // pancake layers 0..5 (0 = fleet_empty); < 0 = min(5, ceil(units / 5)) [port divisor]
};
struct GalacticBattlePrompt {
    bool active = false;
    bool single = false;     // story_pending_battle_shell (auto-resolve forbidden: tutorial / story / player flag, §1.4)
    bool space = true;       // TEXT_TACTICAL_SPACE_ vs _LAND_BATTLE_IS_PENDING
    std::u32string planet;   // planet display name
    std::u32string title;    // overrides planet + key when not empty (MP countdown text)
    Rgba8 color{255, 32, 32, 240};  // GameConstants Battle_Pending_Message_Color
    GalacticBattleSide left, right;  // left = the local player's force, right = the enemy
};
// A screen-text line (GALACTIC_DIALOGS §2.1 / §3.1): "The mission holocron has been updated." (white), the auto-resolve
// result line TEXT_AR_* (red 255, 24, 39). The host owns the timing (alpha = fade). [port] drawn centred, stacked down
// from 140 canvas units below the top edge, EmpireAtWar-Medium 9, outlined, on kScreenTextLayer: the last HUD layer before
// the GUI dialogs (hud_frame_steps), so an open dialog covers them.
inline constexpr uint32_t kScreenTextLayer = kHudLayers - 3;
struct GalacticScreenText {
    std::u32string text;
    Rgba8 color{255, 255, 255, 255};
    uint8_t alpha = 255;
};
inline constexpr Rgba8 kAutoResolveTextColor{255, 24, 39, 255};
// GALACTIC_DIALOGS §3.1: the auto-resolve result line. space / win / magnitude (0 major, 1 regular, 2 minor) pick
// TEXT_AR_{SPACE,LAND}_{WIN,LOSS}_{MAJOR,REGULAR,MINOR}; ##0 = faction name, ##1 = planet, ##2 = own losses.
std::u32string auto_resolve_text(const TextDatabase* text, bool space, bool win, int32_t magnitude,
                                 std::u32string_view faction, std::u32string_view planet, int64_t losses);
// TEXT_AR_HERO_LOSS "##0 defeated at ##1" per dead named hero.
std::u32string auto_resolve_hero_loss_text(const TextDatabase* text, std::u32string_view hero, std::u32string_view planet);
// One host shell visibility wish of the last build (GalacticHud::shell_visibility): ShellSet::set_visible(component,
// visible), then ShellSet::apply(layout) when anything changed.
struct GalacticShellVisibility {
    uint32_t component = 0;
    bool visible = false;
};

struct GalacticHudFrame {
    View view;                    // galactic camera (make_view of the GalacticCameraController's params)
    int32_t zoom_state = 0;       // galactic camera zoom state (§1.6)
    float zoom = 0.0f;            // zoom fraction z
    std::span<const GalacticPlanetInfo> planets;
    int64_t credits = kNoGalacticAmount;
    int32_t pop = -1, pop_cap = -1, day = -1, tech = -1;
    int64_t income = kNoGalacticAmount;  // total income per day (Text_Day_Credits), may be negative
    uint8_t faction_alt = 1;      // per-faction component lists [port] (Blank_Texture_Name 0 rebel, 1 empire)
    bool shells_loaded = false;   // shells drawn by alamo_ui_draw ShellDrawer
    bool organize_open = true;    // fleet organiser shown for the selected planet
    // Playtest 3 (base 40.jpg): the fleet-slot ovals (g_planet_fleet's fleet_empty on every free slot 0..2 of every planet
    // on screen) show while a fleet / fleet unit is selected (host: the galactic selection is not empty), while an own
    // stack is selected, and while a fleet or unit is dragged.
    bool show_fleet_slots = false;
    // Tint of the empty ovals (R5, base 40.jpg: Empire blue on every planet, unowned Felucia too). [inferred] the local
    // player's Factions.xml <Color> (host); no base shot of another faction's ovals.
    Rgba8 empty_slot_color{255, 255, 255, 255};
    bool objectives_open = true;
    std::u32string objectives_header;  // "" = TEXT_OBJECTIVES [port]
    double time = 0.0;
    Vec2f cursor_px{};
    GalacticProviders providers;
    bool show_production_disabled = false;  // [port] grey production-disabled build options instead of hiding them
    bool paused = false;          // [port: Gabriel] b_play_pause look; a click emits pause_event(!paused) (orders.hpp)
    // Day pips (docs/re/HUD_GAPS.md §4): the fiscal-cycle (income) timer as 10 balance_pip clones from the bone fight_bar_g
    // of i_main_commandbar, left to right, pip width apart (Pixel_Align: snapped to the pixel centre). k = floor(10 x
    // cycle_fraction) clamped 0..9: pips 0..k alternate texture 1 (i_bar_fight_blue), k+1..9 texture 0 (i_bar_fight_red);
    // pip k shows the Selected (i_bar_fight_flash) art while day_pip_flash. Host: cycle_fraction = (galactic mode frame -
    // cycle start frame) / cycle length (round(30 x Fiscal_Cycle_Time_In_Secs) = 1350 frames); < 0 hides the pips.
    float cycle_fraction = -1.0f;
    bool day_pip_flash = true;
    // The advanced (full-screen) planet map is open (HUD +0x2a34): the first day pip sits on the bone day_clock of map_shell
    // (planetary_mode.alo) instead of fight_bar_g (HUD_GAPS §4.2). Host: map_shell's bones must be in layout() (ShellSet
    // load of "map_shell" + apply); [port] without that bone the pips stay on fight_bar_g.
    bool advanced_map_open = false;
    // GALACTIC_DIALOGS §4: the organiser (i_main_organize) and planet_info show only while organize_open (host: selected
    // planet && planet close-up) and the advanced planet map is closed. NOTE host: advanced_map_open must mean the FoC
    // planet map (advanced_planetary / map_shell), not the zoomed planet view, or the organiser never shows.
    // §1 battle prompt, §2.1 holocron flash (b_story_arc_g Flash_Texture_Name while a chapter is unread), screen text.
    GalacticBattlePrompt battle;
    bool holocron_flash = false;
    std::span<const GalacticScreenText> screen_text;
    // FIXPASS_GALACTIC §4.2: the advisor *_LOOP Bink in Movie_galactic, placed as Movie_tactical (TacticalHudFrame). Host feed:
    // the image page holding the loop's current frame, only while a loop plays; -1 = nothing drawn.
    int32_t advisor_page = -1;
    bool advisor_alpha = true;
    // §5.1 build tab shown (the pads' colour follows each option's `space`); informational.
};

// Fleet drag and drop (P4 completion; CommandBarComponents Drag_And_Drop on q{n}_fleet, q{n}_RRCC, land_fleet_NN).
// FleetManagement ops per AI_GALACTIC.md / AI_GALACTIC_2.md §5.4 (op names [M]): 0 form fleet, 2 detach, 3 merge,
// 4 launch, 5 land. [port] payload use: target = destination fleet / planet, fleet = source fleet (kNoObject = none /
// new), unit = the dragged unit (kNoObject = the whole fleet).
// The op codes and the event come from engine/commands (commands::FleetOp, commands::fleet_management_event), shared
// with the galactic sim.

// Drop rules [port]: a press on a draggable pad that moves more than 4 pixels starts a drag (the pad's icon follows the
// cursor on layer 12); the release decides:
//   fleet header / unit -> another fleet's pad      FleetManagement merge {target = that fleet, fleet = source, unit}
//   land unit -> a fleet pad                        FleetManagement launch {target = fleet, unit}
//   fleet header / unit -> a land_fleet pad         FleetManagement land {target = selected planet, fleet, unit}
//   fleet header -> a planet in the world           MoveObjectToObject {fleet, planet} (not its own planet)
//   fleet unit -> a planet                          MoveObjectToObject {unit, planet} (the sim splits it off)
//   fleet unit -> empty space                       FleetManagement detach {target = selected planet, unit} (new fleet)
// Same-fleet drops and land units dropped in the world do nothing. A drop swallows the click of that release.
// Fleet-slot drops (playtest 3, base 40.jpg; the slot ovals of GalacticHudFrame::show_fleet_slots, one g_planet_fleet
// instance per slot; [inferred] drop semantics - the specs give the slots, not the drop rules):
//   same planet, slot holds another own fleet   fleet / unit: FleetManagement merge {target = that fleet, fleet, unit};
//                                                land unit: launch {target = that fleet, unit}
//   same planet, empty slot                     whole fleet: FleetManagement slot (op 8) {target = planet, fleet,
//                                                slot}; fleet unit: detach {target = planet, fleet, unit, b = slot}
//                                                (the new fleet takes that slot); land unit: op 6 {target = planet,
//                                                unit, a = planet, b = slot} (launched as a new fleet in that slot)
//   another planet's slot (empty or own fleet)  whole fleet: op 8 {planet, fleet, slot, a = destination} then
//                                                MoveObjectToObject {fleet, planet} (it takes the slot on arrival, or
//                                                merges into the fleet in it); unit (fleet or land): op 6 {target =
//                                                its planet, unit, a = destination, b = slot}
//   another planet's enemy slot                 as above without the slot choice
//   own slot, enemy slot at the same planet     nothing
// The slots live in the sim (playtest 3 t07, galactic FleetState::slot): a drop only issues the commands below and the
// host feeds the resulting slot back through GalacticPlanetStack::slot. Slot choices ride in the payload: op 8 {slot}
// re-slots / merges in place; op 8 {slot, a = planet} is the slot to take on arrival there (ahead of the move order);
// `b` = the slot a new fleet takes (detach, launch, op 6 with its destination).
inline constexpr int32_t kMapFleetIndex = 3;  // GalacticDrag::fleet_index of a fleet dragged from its map button
struct GalacticDrag {
    bool active = false;
    int32_t component = -1;      // dragged pad
    int32_t fleet_index = -1;    // 0..2 organiser fleet, -1 land forces, kMapFleetIndex = a g_planet_fleet map button
    uint32_t fleet = 0;          // source fleet object (0: land unit)
    uint32_t planet = 0;         // map button: the fleet's planet (drops on it do nothing); 0 = organiser / land pads
    uint32_t unit = 0;           // dragged unit (0: the whole fleet)
    std::string icon;
    Vec2f cursor_px{};
    int32_t over_component = -1; // HUD pad under the cursor
    int32_t over_planet = -1;    // index into the frame's planets under the cursor
    bool over_fleet = false;     // over another fleet's pad
    bool valid = false;          // a drop here does something
    int32_t over_slot = -1;      // fleet slot 0..2 under the cursor (g_planet_fleet instance), -1 none
    uint32_t over_slot_planet = 0, over_slot_fleet = 0;  // that slot's planet / fleet (0 = empty slot)
    int32_t layers = 0;          // map fleet: pancake layers drawn under the dragged icon (Drag_Back)
    Rgba8 tint{};                // map fleet: its pancake's owner tint (GalacticPlanetStack::owner_color)
};

struct GalacticHudStats {
    uint32_t labels = 0, label_icons = 0, buttons = 0, build_buttons = 0, queue_entries = 0, fleet_slots = 0,
             land_slots = 0, objectives = 0, hit_boxes = 0, ability_rows = 0, radar_blips = 0;
    uint32_t garrison_icons = 0, stack_icons = 0;  // planet icons (OI 2026-10-28)
    uint32_t planet_rings = 0, planet_values = 0;  // g_planet_ring, g_planet_value
    uint32_t level_pips = 0, weather_icons = 0, ability_icons = 0, credit_bars = 0, corruption_texts = 0;  // ui4 t03
    bool tooltip = false;
    bool radar_view = false;  // camera footprint drawn on the minimap
    bool radar_backdrop = false;  // opaque radar rectangle drawn under the shells (build_radar)
    uint32_t day_pips = 0, day_pips_lit = 0;  // balance_pip clones drawn / of them blue
    enum class DayPipAnchor : uint8_t { None, FightBar, DayClock, Fallback };
    DayPipAnchor day_pip_anchor = DayPipAnchor::None;  // where the first pip was placed
    Vec2f day_pip_origin{};                            // first pip centre (canvas, pixel-snapped)
    bool battle_prompt = false, battle_prompt_shell = false;  // §1 banner drawn / on its shell bones
    uint32_t battle_graphs = 0, screen_text_lines = 0;
    bool advisor_movie = false;  // Movie_galactic drew the advisor loop this frame
    bool organize_shown = false;                       // §4 organiser shell active this build
    bool fleet_slots_shown = false;                    // slot ovals shown this build
    uint32_t empty_slots = 0, slot_highlights = 0;     // fleet_empty ovals drawn / mouse-over frames on slots
};

// [port] positions for the galactic components when i_galactic_controls.alo / i_galactic_organize.alo are not loaded.
void apply_galactic_fallback_layout(const ComponentTable& table, const HudCanvas& canvas, HudLayout& layout);

class GalacticHud {
public:
    void init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text = nullptr);
    void resize(uint32_t width, uint32_t height);
    const HudCanvas& canvas() const { return canvas_; }
    HudLayout& layout() { return layout_; }
    const HudLayout& layout() const { return layout_; }
    HudButtons& buttons() { return buttons_; }
    void set_sfx_hook(HudSfxHook hook) { buttons_.set_sfx_hook(std::move(hook)); }
    void set_tooltip_config(const TooltipConfig& c) { tooltip_cfg_ = c; }

    void build(const GalacticHudFrame& frame);
    const HudDrawList& draw_list() const { return dl_; }
    const GalacticHudStats& stats() const { return stats_; }
    // ui4 t03: the filter bits (kFilter*) of the b_filters0..3 toggles now on (the host feeds the hero bit back into the
    // fleet buttons' representative icon).
    uint32_t filter_bits() const;
    int32_t hit_test(int32_t px, int32_t py) const;
    const std::vector<HudHitBox>& hit_boxes() const { return hits_; }
    // Shell visibility the last build wants (i_main_organize, pending_battle_shell, story_pending_battle_shell); the host
    // applies it to its galactic ShellSet (only shells it loaded matter).
    std::span<const GalacticShellVisibility> shell_visibility() const { return shell_vis_; }
    // Selected planet (from the frame's planets) of the last build, -1 if none.
    int32_t selected_planet() const { return selected_; }

    // Clicks: build pads -> Production (type, planet), queue -> Production cancel, fleet / land pads ->
    // HudAction::GalacticSelect, planet_info -> HudAction::PlanetInfo, other buttons -> HudAction::Button.
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<commands::EventData>& out,
                      std::vector<HudAction>* actions = nullptr);
    // Fleet drag state (cursor choice: CursorContext::fleet_drag / drop_on_fleet / drop_on_planet / drop_valid).
    const GalacticDrag& drag() const { return drag_; }
    // A draggable pad of the last build (cursor CAN_GRAB).
    bool draggable(int32_t component) const;
    // RouteHooks::hud_takes: false for a right press on g_planet_fleet / g_planet_land_forces (they ignore the right
    // button, so a right-click move onto the planet under them reaches the world); true otherwise.
    bool takes_button(int32_t component, Button b) const;
    static constexpr int32_t kDragThresholdPx = 4;

private:
    void drag_update(const RoutedInput& routed, std::vector<commands::EventData>& out, bool& dropped);
    // What releasing the dragged item over drag_.over_component at drag_.cursor_px does: appends the commands to `out` and
    // returns true when there are any. out = nullptr is a dry run (no commands), which is what drag_.valid (the drop
    // highlight and cursor) is: the highlight can never disagree with the drop.
    bool drop(std::vector<commands::EventData>* out);
    int32_t planet_at(float px, float py) const;
    void build_labels(const GalacticHudFrame& f);
    void build_planet_stacks(const GalacticPlanetInfo& p, Vec2f at, uint8_t alpha, Vec2f cursor);
    void build_bar(const GalacticHudFrame& f);
    void build_radar(const GalacticHudFrame& f);
    void build_fleets(const GalacticHudFrame& f);
    void build_objectives(const GalacticHudFrame& f);
    void build_special_abilities(const GalacticHudFrame& f);
    void build_day_pips(const GalacticHudFrame& f);
    void build_battle_prompt(const GalacticHudFrame& f);
    void build_screen_text(const GalacticHudFrame& f);
    std::u32string text_or(const char* key, std::u32string_view fallback) const;
    void hit(int32_t component, uint32_t layer, Vec2f center, Vec2f size);
    Vec2f component_size(uint32_t component, const ComponentPlacement& p) const;
    void text_at(int32_t component, std::u32string_view text, Vec2f anchor, uint32_t layer, uint8_t alpha = 255,
                 const Rgba8* color = nullptr, bool second_color = false);
    int32_t font_of(int32_t component) const;

    const ComponentTable* table_ = nullptr;
    const TextureAtlas* atlas_ = nullptr;
    FontCache* fonts_ = nullptr;
    const TextDatabase* text_ = nullptr;
    HudCanvas canvas_ = HudCanvas::make(1024, 768);
    HudLayout layout_;
    HudButtons buttons_;
    TooltipState tooltip_;
    TooltipConfig tooltip_cfg_;
    HudDrawList dl_;
    std::vector<HudHitBox> hits_;
    std::vector<uint8_t> dynamic_;
    GalacticHudStats stats_;
    int32_t selected_ = -1;
    uint32_t selected_id_ = 0;
    // Component ids.
    int32_t name_ = -1, value_ = -1, control_ = -1, ground_ = -1, space_ = -1, conflict_ = -1, hero_ = -1, smuggler_ = -1,
            bounty_ = -1, ring_ = -1, credit_filter_ = -1;
    // ui4 t03: b_filters0..3 and the extra planet-label components.
    int32_t filters_[4] = {-1, -1, -1, -1};
    int32_t build_icon_ = -1, smuggled_ = -1, corruption_icon_ = -1, corruption_text_ = -1, enemy_hero_ = -1, weather_ = -1,
            ability_ = -1, credit_bar_ = -1;
    int32_t credits_ = -1, pop_ = -1, day_ = -1, planet_text_ = -1, income_ = -1, tech_ = -1, planet_info_ = -1;
    int32_t day_credits_ = -1, radar_ = -1, radar_blip_ = -1, radar_view_ = -1, movie_ = -1;
    int32_t planet_fleet_ = -1, planet_land_ = -1;
    int32_t obj_back_ = -1, obj_header_ = -1, obj_text_ = -1, obj_icon_ = -1, commandbar_ = -1, organize_ = -1;
    int32_t pause_ = -1;
    int32_t balance_pip_ = -1, map_shell_ = -1;
    // §1 battle prompt / §2 holocron / §5.3 emblem.
    int32_t pending_shell_ = -1, story_pending_shell_ = -1, choice_text_ = -1, choice_left_ = -1, choice_right_ = -1,
            graph_left_ = -1, graph_right_ = -1, single_text_ = -1, single_button_ = -1;
    int32_t holocron_ = -1, allegiance_ = -1;
    std::vector<GalacticShellVisibility> shell_vis_;
    bool paused_ = false;
    // g_special_ability rows of the last build (canvas rectangles).
    int32_t special_ = -1;
    struct AbilityRow {
        Vec2f min{}, max{};
        uint32_t hero = 0;
        bool enabled = true;
    };
    std::vector<AbilityRow> ability_rows_;
    std::vector<int32_t> build_pad_, create_, queue_, land_;
    std::vector<std::vector<int32_t>> fleet_;  // [fleet][slot] unit pads q{n}_0001 .. q{n}_0406 (UI.md §7.2)
    std::vector<int32_t> pathfinder_;          // [fleet] the header pad q{n}_0000 (UI.md §7.4), -1 if absent
    struct PadSlot {
        int32_t component = -1;
        GalacticBuildOption option;
        HudQueueEntry queue;
        bool is_queue = false;
        std::vector<uint32_t> select;  // fleet / land pads
        // drag and drop
        bool draggable = false;
        int32_t fleet_index = -1;      // 0..2 organiser pane, -1 land forces
        bool foreign = false;          // organiser pad of a non-local fleet: no drag, no click, no drop target (UI.md §7.2)
        uint32_t fleet = 0, unit = 0;  // unit 0 = fleet header; a stack: its first unit (a drag takes that one unit)
        bool empty = false;            // an empty organiser pad shown while dragging (UI.md §7.3): fleet 0 = a pane without a fleet
        std::string icon;
    };
    std::vector<PadSlot> pads_;
    const PadSlot* pad_of(int32_t component) const;
    // g_planet_fleet / g_planet_land_forces buttons of the last build (one component, many instances).
    struct StackHit {
        Vec2f min{}, max{};  // canvas
        int32_t component = -1, slot = -1;
        uint32_t fleet = 0, planet = 0;  // fleet 0: an empty slot oval
        bool draggable = false;
        bool enemy = false;
        int32_t layers = 0;
        Rgba8 tint{};  // GalacticPlanetStack::owner_color
        std::string icon;
    };
    std::vector<StackHit> stack_hits_;
    const StackHit* stack_at(int32_t component, float px, float py) const;  // pixel position
    StackHit pane_hit(const PadSlot& pad) const;
    StackHit press_stack_;
    // drag and drop
    GalacticDrag drag_;
    int32_t press_component_ = -1;
    Vec2f press_px_{};
    struct PlanetScreen {
        uint32_t planet = 0;
        float x = 0, y = 0, r = 0;  // pixels
    };
    std::vector<PlanetScreen> planet_px_;
    // Fleet slots: the sim's state (GalacticPlanetStack::slot = galactic::FleetState::slot); the HUD keeps none of its own.
    bool slots_shown_ = false;
    Rgba8 empty_slot_color_{255, 255, 255, 255};  // GalacticHudFrame::empty_slot_color
    bool slot_drop(const StackHit& sh, std::vector<commands::EventData>* out);  // false: nothing to do
    std::vector<int32_t> fleet_header_;  // q{n}_fleet
};

}  // namespace alamo::ui
