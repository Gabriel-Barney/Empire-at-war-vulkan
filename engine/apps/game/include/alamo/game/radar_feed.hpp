// P host (alamo_game): radar feeds - what the tactical radar shows besides the unit blips and the fog (UI.md §1.7,
// RadarMap.xml). Presentation only: reads the battle's const state, writes engine/ui's RadarEvents / field list; nothing
// here feeds the sim or its CRC.
//   radar_asteroid_fields  Space_Asteroid_Field_Color / _Border_Color discs: the battle's SpaceFieldIndex fields that are
//                          asteroid fields (Is_Asteroid_Field, or Is_Impassable_Asteroid [port]) at their centre and
//                          Space_Obstacle_Radius. Nebulae and ion storms have no radar colour in RadarMap.xml: not drawn.
//   RadarFeed::deaths      DEFAULT_UNIT_DIE (TACTICAL_GAPS §1.9: "a local-visibility presentation event"): one
//                          Default_Unit_Die ping at the dying unit's position when the local player sees it. [port] only
//                          types that show on the radar (Is_Visible_On_Radar).
//   RadarFeed::capture_points
//                          L1_S4_REMAINING §4.2: a capture point whose progress leaves 0 towards a controller other
//                          than its owner plays the type's Begin_Control_Transition_Radar_Event (vanilla
//                          DEFAULT_CONTROL_POINT_TRANSITION) at the point, local player sees it only. End_Control_
//                          Transition_Radar_Event has no traced call site (not played).
//   RadarFeed::blips       Lua Add_Radar_Blip(object, name) / Remove_Radar_Blip(name): persistent named markers that follow
//                          the object ([port] visual: gold ring; the original's blip art is not traced).
// Not fed (no spec): the base-under-attack radar ping (AUDIO.md names no event), Interdict_Activated (a story event; the
// same-named RadarMap event has no traced trigger), Default_Beacon_Placement (multiplayer beacons, out of scope),
// Galactic_Build_Complete_Planet_Ping (galactic radar).
// Needs engine/ui's alamo_ui_hud (ALAMO_GAME_HAVE_CONTROLS).
#pragma once
#ifdef ALAMO_GAME_HAVE_CONTROLS
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/tactical/abilities/space_fields.hpp"
#include "alamo/ui/hud_radar.hpp"
#include "alamo/ui/hud_world.hpp"

namespace alamo {
namespace lua {
class StoryModel;
}
}  // namespace alamo

namespace alamo::game {

// Asteroid fields of a space battle for ui::RadarFieldPaint (insertion order).
std::vector<ui::RadarField> radar_asteroid_fields(std::span<const tactical::abilities::SpaceField> fields);

struct RadarDeathNote {
    float x = 0, y = 0;
    bool visible = true;           // the local player sees the unit (not fogged / stealthed)
    bool visible_on_radar = true;  // the type's Is_Visible_On_Radar
};
struct RadarCapturePointNote {
    objects::ObjectId id = objects::kNoObjectId;
    int32_t owner = -1, pending = -1;
    float progress = 0.0f;
    float x = 0, y = 0;
    bool visible = true;
    std::string begin_event;  // the point type's Begin_Control_Transition_Radar_Event ("" = none)
};
struct RadarBlipNote {
    std::string name;
    float x = 0, y = 0;
    bool valid = true;  // the object is alive (a dead object's blip is kept at its last place until removed)
};

class RadarFeed {
public:
    void reset();
    // Default_Unit_Die pings for this tick's deaths.
    void deaths(std::span<const RadarDeathNote> notes, const ui::RadarSettings& settings, ui::RadarEvents& events, const std::string& event = "Default_Unit_Die");
    // Begin_Control_Transition pings (edge: progress left 0 towards another controller than the owner).
    void capture_points(std::span<const RadarCapturePointNote> points, const ui::RadarSettings& settings, ui::RadarEvents& events);
    // Story blips: markers of the notes (upsert), markers no longer listed are removed.
    void blips(std::span<const RadarBlipNote> notes, ui::RadarEvents& events);

    // One battle tick from the battle's const state (builds the notes above). `story` = the tactical story model (null:
    // none); `types` = ui::build_hud_type_table (Is_Visible_On_Radar).
    void battle_tick(const sim::TacticalBattle& battle, const objects::ObjectStore& store, const objects::GameObjectTypes& types,
                     std::span<const ui::HudTypeInfo> hud_types, int32_t local_player, const lua::StoryModel* story,
                     const ui::RadarSettings& settings, ui::RadarEvents& events);

    uint64_t death_pings() const { return death_pings_; }
    uint64_t transition_pings() const { return transition_pings_; }

private:
    std::map<objects::ObjectId, float> progress_;  // capture point -> progress at the last tick (ordered: lookups only)
    uint64_t death_pings_ = 0, transition_pings_ = 0;
};

}  // namespace alamo::game
#endif
