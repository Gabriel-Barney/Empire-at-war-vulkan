// P4 mouse cursor (docs/re/UI.md §3.1 MousePointers.xml, §1.1 step 9 "2D scene #2: mouse cursor, topmost"): the cursor
// set from Data\XML\MousePointers.xml (53 <MousePointer Name="POINTER_*"> entries: Base_Texture, Hot_X / Hot_Y,
// Anim_Frame_Delay; the names map to an engine enumeration in data order), the context -> cursor choice and the draw
// data. Host API: fill a CursorContext each frame, `MouseCursor::update(choose_pointer(ctx), now)`, then
// `MouseCursor::build(cursor_dl, canvas, cursor_px, now)` and pass the list to HudRenderer::record(..., cursor = &dl),
// which draws it at the Cursor step (after the dialogs and layers 11-12). Presentation only.
//
// [port] choices (the docs give the data, not the rules):
//   * animation: Base_Texture "<name>_00.tga" is frame 0; frames _01, _02, ... follow while the texture exists
//     (CursorTable::resolve_frames with the host's existence test); Anim_Frame_Delay = 1/30 s ticks per frame (missing:
//     5); a texture without the _00 suffix is a single frame;
//   * size: the texture's pixel size, constant on screen (converted to canvas units with units-per-pixel); hot spot =
//     Hot_X / Hot_Y pixels from the texture's top-left corner;
//   * choose_pointer precedence: hidden / movie -> POINTER_MOVIE_MODE; busy -> WAIT; camera rotate / scroll drags; HUD
//     or dialog under the cursor -> NORMAL (CAN_GRAB over a draggable pad); fleet drag (GRABBING; COMBINE / CANT_COMBINE
//     over a fleet pad, DROP / CANT_DROP over a planet); drag-select box -> DRAG_SELECT_MODE; targeting modes (ability
//     by target kind and validity, special weapon, beacon, reinforcement point, bombing run, tactical build / sell,
//     waypoint, guard, barrage, repair, garrison, attack-move, move-only, attack-only); then the hovered object: enemy
//     with a selection -> ATTACK (ATTACK_OUT_OF_RANGE / CANT_ATTACK), a garrisonable own / friendly / neutral object with
//     a selection -> GARRISON_IN (ui1 t01), own selectable -> SELECT; ground with a selection -> MOVE / CANT_MOVE; else NORMAL.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/gui_dialog.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"

namespace alamo::data {
class XmlNode;
}

namespace alamo::ui {

// The shipped pointer names in MousePointers.xml order.
enum class Pointer : uint8_t {
    Normal, Wait, Select, Move, CanGrab, Grabbing, Drag, Drop, Combine, CantSelect, CantMove, CantDrop, CantGrab,
    CantCombine, CantAttack, Attack, AttackMove, AttackOutOfRange, AttackWithSpecialWeapon0, AttackWithSpecialWeapon1,
    AttackWithSpecialWeapon2, AttackWithSpecialWeaponInvalid, DragSelectMode, MapScrollingMode, MapRotatingMode, MovieMode,
    WaypointFollow, WaypointPlacement, TacticalBuild, TacticalSell, Guard, Barrage, PlaceBeacon, BombingRunTarget,
    ReinforcementsLandingPoint, AttackOnlyUnitTargeted, AttackOnlyNoUnitTargeted, MoveOnlyPassableTargeted,
    MoveOnlyNoPassableTargeted, AbilityEnemy, AbilityEnemyInvalid, AbilityFriendly, AbilityFriendlyInvalid, AbilityTerrain,
    AbilityTerrainInvalid, AbilitySpace, AbilitySpaceInvalid, GarrisonIn, GarrisonOut, AbilityBribe, AbilityCantBribe,
    AbilityBlast, RepairHardpoint,
    Count
};
inline constexpr size_t kPointerCount = static_cast<size_t>(Pointer::Count);
// "POINTER_NORMAL" ... (data names).
const char* pointer_name(Pointer p);
bool pointer_from_name(std::string_view name, Pointer& out);

struct CursorDef {
    std::string name;
    std::string base_texture;
    int32_t hot_x = 0, hot_y = 0;
    int32_t frame_delay = 5;           // 1/30 s ticks per frame [port]
    std::vector<std::string> frames;   // resolve_frames(); empty = base_texture only
};

class CursorTable {
public:
    // <MousePointers> root. Returns the number of entries read; unknown names are kept (lookup by name).
    size_t parse(const data::XmlNode& root);
    void add(CursorDef d);  // tests
    // Expands "<x>_00.ext" into _00, _01, ... while `exists(name)`; returns the total frame count.
    size_t resolve_frames(const std::function<bool(std::string_view)>& exists, size_t max_frames = 64);
    const CursorDef* find(Pointer p) const;
    const CursorDef* find(std::string_view name) const;
    size_t size() const { return defs_.size(); }
    std::span<const CursorDef> defs() const { return defs_; }

private:
    std::vector<CursorDef> defs_;
};

// What the cursor is over / doing this frame (filled by the host from the HUD, router, targeting and picking state).
struct CursorContext {
    enum class Hover : uint8_t { None, Ground, Own, Friendly, Enemy, Neutral };
    enum class Targeting : uint8_t {
        None, AbilityEnemy, AbilityFriendly, AbilityTerrain, AbilitySpace, AbilityBribe, AbilityBlast, SpecialWeapon,
        Beacon, Reinforcement, BombingRun, TacticalBuild, TacticalSell, Waypoint, WaypointFollow, Guard, Barrage, Repair,
        GarrisonOut, AttackMove, MoveOnly, AttackOnly,
    };
    bool hidden = false, movie = false, busy = false;
    bool over_ui = false;              // a dialog / HUD component under the cursor
    bool over_draggable = false;       // HUD pad that can be dragged (Drag_And_Drop)
    bool rotating = false, scrolling = false, drag_selecting = false;
    // galactic fleet drag (GalacticHud::drag())
    bool fleet_drag = false;
    bool drop_on_fleet = false, drop_on_planet = false, drop_valid = true;
    Targeting targeting = Targeting::None;
    bool target_valid = true;
    int32_t special_weapon = 0;        // 0..2 (POINTER_ATTACK_WITH_SPECIAL_WEAPON_n)
    Hover hover = Hover::None;
    bool selection = false;            // the local player has units selected
    bool in_range = true;              // hovered enemy in range of the selection
    bool can_attack = true;            // the selection can attack the hovered enemy
    bool passable = true;              // ground under the cursor reachable
    bool garrisonable = false;         // hovered friendly object takes garrisons
    bool selectable = true;            // hovered own object selectable
};
Pointer choose_pointer(const CursorContext& ctx);

class MouseCursor {
public:
    void init(const CursorTable& table, const TextureAtlas* atlas, GuiImageResolver images);
    // Sets the pointer; the animation restarts when it changes.
    void update(Pointer p, double now);
    Pointer pointer() const { return current_; }
    // Frame index at `now`.
    size_t frame(double now) const;
    // Texture of the current frame ("" when the table has no entry).
    std::string texture(double now) const;
    // Adds the cursor quad (layer 12, Quads) at the cursor pixel. False when nothing could be drawn (no entry / image).
    bool build(HudDrawList& dl, const HudCanvas& canvas, Vec2f cursor_px, double now) const;

private:
    const CursorTable* table_ = nullptr;
    const TextureAtlas* atlas_ = nullptr;
    GuiImageResolver images_;
    Pointer current_ = Pointer::Normal;
    double since_ = 0.0;
};

}  // namespace alamo::ui
