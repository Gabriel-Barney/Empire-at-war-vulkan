// P2 world-space UI (docs/re/UI.md §1.2-1.5): selection brackets, health / shield / power bars, control-group
// numbers and fly-text are 2D quads placed by projecting a world point through the gameplay camera, drawn in
// the HUD layers after the post chain and a depth clear, at a constant size in canvas units (they never shrink
// with distance). The selection circle is the only true 3D primitive: WorldHud only lists SelectionBlob
// records; alamo_ui_draw turns them into a terrain shadow blob (land) or a flat BlobStencilMasked quad
// instance in the Transparent bucket (space).
//
// Read-only with respect to the sim: inputs are HudUnit records copied from the render snapshot
// (hud_sim_bridge.hpp) and the synced selection; nothing here writes sim state or emits commands.
//
// [port] choices (UI.md leaves them open, §6): bracket pieces centred on the left / right edge of the object's
// projected bounding sphere (radius x GUI_Bounds_Scale, at least 6 px) at the sphere centre's height; bars
// centred above the bounds, health then shield then power, Health_Bar_Spacing (2) pixels apart; Smooth_Bar fills
// clip the full-level overlay to the fraction, otherwise overlay level round(fraction * Max_Bar_Level); bars and
// brackets shown for selected or hovered units; hostile units' brackets tinted red; control-group number at the
// bracket centre + st_control_group Text_Offset (pixels, +Y down); GUI_Bracket_Size -1 from CategoryMask (see
// derive_bracket_classes); fly-text rises 20 px/s for 2 s.
// Bars keep a fixed size: GameConstants Health_Bar_Scale / Land_Health_Bar_Scale / Min_Health_Bar_Scale (defaults 20, 20,
// 0) are only written by the GameConstants constructor and never read (docs/re/STRATEGIC_GAPS.md §7.2 [H]), so the
// port does not parse them.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/picking.hpp"

namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::ui {

class FontCache;

enum class BracketSize : uint8_t { Small = 0, Medium = 1, Large = 2 };

// Per GameObjectType HUD fields (GameObjectTypes -> build_hud_type_table, or filled by tests).
struct HudTypeInfo {
    std::string name;
    int32_t gui_bracket_size = -1;        // GUI_Bracket_Size (0 small, 1 medium, 2 large, -1 derive)
    bool hide_health_bar = false;         // GUI_Hide_Health_Bar
    float bounds_scale = 1.0f;            // GUI_Bounds_Scale
    float select_box_scale = 0.0f;        // Select_Box_Scale (> 0: selection circle)
    float select_box_z_adjust = 0.0f;     // Select_Box_Z_Adjust
    float scale = 1.0f;                   // Scale_Factor
    std::string selection_blob_material;  // Selection_Blob_Material_Name ("" = faction default)
    bool has_shields = false;             // Shield_Points > 0
    bool is_hero = false;
    // [port] class for GUI_Bracket_Size -1 (UI.md §1.4 [M]; derive_bracket_classes: land Infantry -> small,
    // Structure -> large, else medium; space Fighter / Bomber (the two lowest category bits) -> small, Capital / Super / Structure -> large,
    // else medium).
    BracketSize land_class = BracketSize::Medium, space_class = BracketSize::Medium;
    std::string icon_name;                // Icon_Name (selection panel portrait)
    // Radar (UI.md §1.7 per-type tags): Radar_Icon_Name (atlas texture, "" = the radar_blip dot), Radar_Icon_Size (x y in
    // the radar's logical space, -1..1 across the radar: canvas size = size x radar size / 2 [port]), Is_Visible_On_Radar /
    // Is_Visible_On_Enemy_Radar (absent = Yes [port]). Radar_Icon_Scale_Land / _Space (vanilla 1 .. 200) and
    // Radar_Rotate_Icon / Radar_Show_Facing are not applied (no reader documented; HudUnit has no facing) [port].
    std::string radar_icon;
    Vec2f radar_icon_size{0.0f, 0.0f};
    bool visible_on_radar = true, visible_on_enemy_radar = true;
    // q6 t19 (OBJECT_SCRIPTS §10.3, radar filter 0x35b4d0): Visible_On_Radar_When_Fogged - a Lua-hidden unit keeps its blip.
    bool visible_on_radar_when_fogged = false;
};
BracketSize bracket_size(const HudTypeInfo& t, bool land);

// Default class of a type whose GUI_Bracket_Size is -1, from its CategoryMask tokens (GameObjectCategoryType names,
// OBJECT_TYPES.md §9.1.1: Fighter Bomber Transport Corvette Frigate Capital Super Infantry Vehicle Air Structure Wall ...).
// UI.md §1.4 only says the original switches on the category (switch [H], category meaning [M]); this is the rule the
// shipped GUI_Bracket_Size values agree with (docs/re/UI.md §1.4 "P2 data check"): explicit Corvette / Vehicle 1, Capital 2,
// Structure mostly 2, Infantry mostly 0; Transport and Wall are nearly always explicit 0 (36 of 39, 16 of 21), so the
// default rarely matters for them. Tokens match case-insensitively.
struct BracketClasses {
    BracketSize land = BracketSize::Medium, space = BracketSize::Medium;
};
BracketClasses derive_bracket_classes(std::span<const std::string_view> category_tokens);
// One entry per TypeIndex.
std::vector<HudTypeInfo> build_hud_type_table(const objects::GameObjectTypes& types);

// A unit as the HUD sees it this frame (copied from the snapshot; ascending id).
struct HudUnit {
    uint32_t id = 0;
    uint32_t type = 0;  // TypeIndex into the HudTypeInfo table
    int32_t owner = -1;
    Vec3 position{};
    float radius = 0.0f;    // world bounding radius (model sphere x scale); <= 0: 10 [port]
    float health = 1.0f;    // fraction
    float shield = -1.0f;   // fraction, < 0 = no shield bar
    float power = -1.0f;    // fraction, < 0 = no power bar
    int8_t control_group = -1;  // 0..9, -1 none
    bool selected = false, hovered = false;
    // [port: Gabriel, original screenshots] world unit icon: the type's Icon_Name in a square frame (own units gold,
    // hostile blue) above the unit with a thin health bar under it, drawn whether selected or not. The original shows one
    // per fighter / bomber squadron; the host flags one member per squadron (assign_unit_icons), which also gives the
    // marker's anchor and bar: the squadron's centre and its live members' mean health (icon_anchored false: the unit's own).
    bool unit_icon = false;
    bool icon_anchored = false;
    Vec3 icon_anchor{};
    float icon_health = 1.0f;
    Rgba8 team_color{255, 255, 255, 255};  // snapshot team colour (radar blips)
    // R5 sensor jamming (ABILITY_VISUALS 3, radar filter 0x35b4d0): Is_Jammed this tick. The radar drops the blip of a unit that is
    // hostile to the local player and jammed; the owner's and allies' radar still shows it.
    bool jammed = false;
    // R5 stealth (ABILITY_VISUALS 1.4, same filter): the object is Is_Stealthed and hostile to the local player. The radar drops
    // its blip; the world HUD hides its health bars and the hard-point reticles skip it (filter_hardpoints).
    bool stealthed = false;
    // q6 t19 (OBJECT_SCRIPTS §10.3, same filter): Lua Hide(true) (render::SnapshotObject::script_hidden). The radar drops
    // its blip for every viewer unless the type has Visible_On_Radar_When_Fogged.
    bool script_hidden = false;
};

// One live unit and the unit standing for its squadron / company (itself when alone): TacticalBattle::icon_links.
struct UnitIconLink {
    uint32_t unit = 0, squad = 0;
};
// QA U7: one marker per squadron. Clears every unit_icon, then flags the first listed member of each `squad` present in
// `units`, anchored at the centre of its members present in `units` with their mean health. Units without a link get none.
void assign_unit_icons(std::span<HudUnit> units, std::span<const UnitIconLink> links);

struct SelectionBlob {
    uint32_t id = 0;
    Vec3 center{};
    float radius = 0.0f;   // Select_Box_Scale x Scale_Factor (blob half-size)
    bool land = false;     // land: terrain shadow blob; space: flat blob
    std::string material;  // ShadowBlobMaterials.xml name
};

struct WorldHudContext {
    const HudCanvas* canvas = nullptr;
    View view;              // gameplay camera of the current mode (picking.hpp make_view)
    bool land = false;
    int32_t local_player = 0;
    std::function<bool(int32_t owner)> hostile;          // default: owner >= 0 && owner != local_player
    // Faction default selection material (Faction XML Land_ / Space_Mode_Selection_Blob_Material_Name; heroes
    // Selection_<faction>_Hero) for an owner. Null [port]: no material name (modc t20: no built-in faction name; the
    // land blob then takes the material table's first entry, the space blob model does not read it).
    std::function<std::string(int32_t owner, bool land, bool hero)> selection_material;
    bool interface_hidden = false;  // byte 0x140b3069b: nothing world-anchored is drawn
    bool targeting_active = false;  // modal targeting controller: selection circles hidden
    bool cinematic = false;         // cinematic flag: selection circles hidden
    float health_bar_spacing_px = 2.0f;  // GameConstants Health_Bar_Spacing
    // [port] unit icons (HudUnit::unit_icon): frame side in canvas units, frame colours.
    float unit_icon_size = 20.0f;
    Rgba8 unit_icon_own{255, 212, 33, 255}, unit_icon_hostile{60, 140, 255, 255};
};

struct FlyText {
    Vec3 anchor{};
    std::u32string text;
    Rgba8 color{255, 255, 0, 255};  // generic_flytext Text_Color
    Vec2f offset_px{};              // pixel offset from the projected anchor (+Y down)
    float age = 0.0f, lifetime = 2.0f;
    float rise_px_per_s = 20.0f;
};
class FlyTextList {
public:
    void add(FlyText t) { items_.push_back(std::move(t)); }
    void update(float dt);  // ages entries, drops expired ones (order kept)
    std::span<const FlyText> items() const { return items_; }
    void clear() { items_.clear(); }

private:
    std::vector<FlyText> items_;
};

// Projected screen bounds of a unit (pixels, +Y down).
struct UnitScreenBounds {
    bool visible = false;
    float cx = 0, cy = 0, radius_px = 0;
};
UnitScreenBounds unit_screen_bounds(const View& v, const HudUnit& u, const HudTypeInfo& t);

// Hover pick for world tooltips / the encyclopedia pop-up and the hard-point reticles (playtest: "tooltip picking by
// bounding sphere"): the unit whose bounding sphere (HudUnit::position, radius; <= 0: 10 [port] as the brackets) the
// pixel's ray enters first, ties by id (pick_spheres). Units with owner < 0 are skipped when `skip_neutral`. Returns the
// index into `units`.
std::optional<size_t> pick_hud_unit(std::span<const HudUnit> units, const View& view, int32_t px, int32_t py,
                                    bool skip_neutral = false);

struct WorldHudStats {
    uint32_t unit_icons = 0, brackets = 0, bars = 0, group_numbers = 0, flytexts = 0, offscreen = 0, blobs = 0;
};

class WorldHud {
public:
    // Resolves the st_* / generic_flytext components (missing ones draw with [port] fallbacks).
    void bind(const ComponentTable& table);
    // Appends this frame's world-anchored quads / text to `dl` and lists the selection circles.
    void build(const WorldHudContext& ctx, std::span<const HudUnit> units, std::span<const HudTypeInfo> types,
               const TextureAtlas& atlas, FontCache& fonts, HudDrawList& dl, std::vector<SelectionBlob>* blobs,
               const FlyTextList* flytext = nullptr);
    const WorldHudStats& stats() const { return stats_; }

private:
    void bars(const WorldHudContext& ctx, const HudUnit& u, BracketSize sz, const UnitScreenBounds& b,
              const TextureAtlas& atlas, HudDrawList& dl);
    const ComponentTable* table_ = nullptr;
    int32_t bracket_[3] = {-1, -1, -1}, health_[3] = {-1, -1, -1}, shield_[3] = {-1, -1, -1};
    int32_t power_ = -1, group_ = -1, flytext_ = -1;
    WorldHudStats stats_;
};

}  // namespace alamo::ui
