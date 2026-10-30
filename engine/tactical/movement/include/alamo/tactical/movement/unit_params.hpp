// Per-unit movement parameters taken from GameObjectType fields (docs/re/MOVEMENT.md §3.4, §4,
// §5.3; OBJECT_TYPES.md). Raw XML values: the MovementSystem applies the mode multiplier
// (Object_Max_Speed_Multiplier_Land / _Space) to speed, acceleration and turn rate.
#pragma once
#include <cstdint>
#include <string>

#include "alamo/tactical/movement/common.hpp"
#include "alamo/tactical/movement/space_path.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"

namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::tactical::movement {

struct UnitDesc {
    ObjectId id = 0;
    V2 pos;
    float z = 0.0f;
    float yaw_deg = 0.0f;
    // Raw type values (per frame; mode multiplier applied by the system).
    float max_speed = 0.25f;      // Max_Speed (OBJECT_TYPES default)
    float turn_rate = 2.3f;       // Max_Rate_Of_Turn
    float accel = 0.0f;           // OverrideAcceleration (<= 0: full speed in one frame)
    float decel = 0.0f;           // OverrideDeceleration
    float close_enough_angle = 0.0f;  // Close_Enough_Angle_For_Move_Start (0: GameConstants value)
    int32_t movement_class = -1;  // MovementClass
    int32_t collision_class = -1; // UnitCollisionClass
    float radius = 10.0f;         // soft footprint radius
    float formation_spacing = 0.0f;  // FormationSpacing (cells)
    bool hero = false;            // CategoryMask has Hero
    std::string type_name;
    int32_t grouping = 2;         // FormationGrouping (unset: Standard [L])
    int32_t formation_order = 0;  // FormationOrder
    bool team_member = false;
    uint32_t space_layer = kLayerFrigate;  // Space_Layer
    float layer_z = 0.0f;         // Layer_Z_Adjust
    // ---- OI tactical (PHASE4 S7 open items) ----
    int32_t path_frame_delay = 0;       // PathFrameDelay (+0x3D0): formation start stagger (§1.6)
    float formation_raggedness = 0.0f;  // FormationRaggedness (+0x16A8): jiggle step scale (§3.5 step 6)
    float max_jiggle_distance = 0.0f;   // MaxJiggleDistance (+0x16AC): jiggle clamp
    int32_t owner = -1;                 // owning player (crush hostility, §5.5)
    bool crusher = false;               // SQUASH behaviour (AT-AT, AT-ST, MAL)
    bool supercrusher = false;          // Is_Supercrusher (+0x19)
    bool squashable = false;            // Is_Squashable (+0x18)
    bool squashable_by_supercrusher = false;  // Is_Squashable_By_Supercrusher (+0x1a)
    // ---- REMAINING_GAPS §4.2-4.7 ----
    uint32_t space_layer_type = 0;      // raw Space_Layer (+0x16dc; 0 = None / unset): threat mode only then (§4.2)
    LocomotorKind locomotor = LocomotorKind::Default;  // special locomotors (§4.3-4.7); host: locomotor_kind_from_type
    float hover_offset = 0.0f;          // Hover_Offset (+0x1b68)
    float max_lift = 0.8f;              // Max_Lift (+0x3a0), raw
    float max_rate_of_roll = 2.0f;      // Max_Rate_Of_Roll (+0x394), raw
    float bank_turn_angle = 70.0f;      // Bank_Turn_Angle (+0x39c)
    float min_speed = 0.0f;             // Min_Speed (+0x3b4), raw
    float strafe_distance = 0.0f;       // Strafe_Distance (+0x2118)
    float prepare_strafe_height = 0.0f; // Prepare_Strafe_Height (+0x1e24)
    bool has_pre_turn_anim = false;     // Has_Pre_Turn_Anim (+0x36)
};

// Fills the type-derived members (id / position / yaw are left to the caller). `model_radius`
// (> 0) is used when the type has no Custom_Soft_Footprint_Radius; otherwise a default by
// UnitCollisionClass applies (infantry 6, bike 10, vehicle 20, large 40, giant 80, other 15) [L].
// REMAINING_GAPS §4.2-4.7 fields are filled too, except UnitDesc::locomotor, which stays Default: the host opts in
// with locomotor_kind_from_type(types, type_index, mode) (special_locomotors.hpp), so existing hosts keep their
// behaviour (and synced draws) until they route the kind.
UnitDesc unit_desc_from_type(const objects::GameObjectTypes& types, uint32_t type_index, float model_radius = 0.0f);

}  // namespace alamo::tactical::movement
