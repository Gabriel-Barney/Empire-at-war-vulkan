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
    // a1k t03 (A1_READINGS §40): Custom_Hard_XExtent / Custom_Hard_YExtent x Scale_Factor (0x371be0); 0 = unset. Read
    // only by the push search (footprint radius 0x3969f0, mover extent 0x372040); unset falls back to `radius`.
    float hard_x = 0.0f;
    float hard_y = 0.0f;
    float formation_spacing = 0.0f;  // FormationSpacing (cells)
    bool hero = false;            // CategoryMask has Hero
    std::string type_name;
    int32_t grouping = 2;         // FormationGrouping (unset: Standard [L])
    int32_t formation_order = 0;  // FormationOrder
    bool team_member = false;
    // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7; save version 14): a land squad container (LandTeamContainer
    // locomotor). It walks its own path at the team speed (its own passability test) and is the formation's anchor its
    // members steer around (MovementSystem::set_land_team). It takes no part in land collision, crush or arrival turns.
    bool team_container = false;
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
    // cu8j t01 (MOVEMENT §4.6.4, save version 13): Stationary_Space_Layer (+0x16e0), the space composition's role of an
    // object that is not moving (when set).
    uint32_t stationary_space_layer = 0;
    LocomotorKind locomotor = LocomotorKind::Default;  // special locomotors (§4.3-4.7); host: locomotor_kind_from_type
    float hover_offset = 0.0f;          // Hover_Offset (+0x1b68)
    float max_lift = 0.8f;              // Max_Lift (+0x3a0), raw
    float max_rate_of_roll = 2.0f;      // Max_Rate_Of_Roll (+0x394), raw
    float bank_turn_angle = 70.0f;      // Bank_Turn_Angle (+0x39c)
    float min_speed = 0.0f;             // Min_Speed (+0x3b4), raw
    float strafe_distance = 0.0f;       // Strafe_Distance (+0x2118)
    float prepare_strafe_height = 0.0f; // Prepare_Strafe_Height (+0x1e24)
    bool has_pre_turn_anim = false;     // Has_Pre_Turn_Anim (+0x36)
    // ---- S7 t06 / t14: Uses_Multiple_Locomotors swap (MOVEMENT.md §3.1.1, 0x3a9910; MovementSystem::use_secondary_locomotor) ----
    bool uses_multiple_locomotors = false;  // Uses_Multiple_Locomotors (+0x74)
    // The swap can run: Secondary_Locomotor_Name (+0x2078) is JETPACK_LOCOMOTOR and Primary_Locomotor_Name (+0x2058) is
    // not. [port] retail tells "on the secondary" by the locomotor class (JetPack vf96 = 0, every other class 1), so a
    // non-JetPack secondary would be rebuilt on every to-secondary call and never swapped back (weapons disabled for
    // good), and a JetPack primary would be rebuilt every stopped frame; the port refuses both (vanilla has neither).
    // An empty or unknown name never swaps (the factory returns null).
    bool has_secondary_locomotor = false;
    LocomotorKind secondary_locomotor = LocomotorKind::Default;  // its kind (locomotor_kind_from_token)
    // Alternate_Max_Speed (+0x380) / Alternate_Max_Rate_Of_Turn (+0x390), raw (type ctor defaults 1.0 / 2.0). Read only
    // by the JETPACK locomotor in its airborne kind (pack state 0 / 0xF..0x13, §3.1.1): the planned flight and the
    // formation speed; the take-off turn (0x10), landing (0x14) and stopped (0x16) use Max_Speed / Max_Rate_Of_Turn.
    float alt_max_speed = 1.0f;
    float alt_turn_rate = 2.0f;
    // S8 t11 (MOVEMENT §3.2 "Deploy states"): Deployment_Anim_Rate (+0x1668, type ctor default 1.0), the speed of the
    // Walk locomotor's deploy / undeploy clips (MovementSystem::deploy).
    float deployment_anim_rate = 1.0f;
    // s78k t01 (MOVEMENT §3.5.1, save version 11): the Walk locomotor's arrival turn. Stopped_Rate_Of_Turn (+0x3b0, raw,
    // read by 0x374310 at speed 0; <= 0 -> Max_Rate_Of_Turn x the mode multiplier) and Movement_Directions_Count (+0x23fc,
    // type default 1).
    float stopped_turn_rate = 0.0f;
    int32_t movement_directions = 1;
    // s78l t02 (MOVEMENT §3.5.1, save version 12): Rotation_Animation_Speed (+0x388, raw). The arrival turn's begin / band /
    // end clips play at (Stopped_Rate_Of_Turn, else the turn rate) / this when both are > 0, else at 1 (0x3736a0).
    float rotation_animation_speed = 0.0f;
    // S8 t24 (MOVEMENT §3.2 "Turret recentre around deploy"): the unit TURRET of a `Deploys` type (TurretBehavior, TD
    // +0x48..+0x5c). [port] Modelled only for Deploys types with the TURRET behaviour and Turret_Rotate_Speed > 0 (the
    // deploy gate is its only sim reader); `turret` false = always at rest. Angles: x (never moves), elevation, yaw
    // (degrees, relative to the body).
    bool turret = false;
    float turret_speed = 0.0f;                // Turret_Rotate_Speed (+0x5a8), degrees per frame
    float turret_extent = 180.0f;             // Turret_Rotate_Extent_Degrees (+0x5a0) [port: unset 180 = no clamp]
    float turret_deployed_extent = 180.0f;    // Deployed_Turret_Rotate_Extent_Degrees (+0x5d0) [port: unset = undeployed]
    float turret_rest[3] = {0.0f, 0.0f, 0.0f};           // Turret_Rest_Angle (+0x5b4)
    float turret_deployed_rest[3] = {0.0f, 0.0f, 0.0f};  // Turret_Deployed_Rest_Angle (+0x5c0)
    float turret_rest_delay = 2.0f;           // Turret_Rotate_To_Rest_Delay_Sec (+0x5cc, ctor default 2.0 s)
};

// Fills the type-derived members (id / position / yaw are left to the caller). Without a
// Custom_Soft_Footprint_Radius the radius is a default by UnitCollisionClass (infantry 6, bike 10,
// vehicle 20, large 40, giant 80, other 15) [L].
// REMAINING_GAPS §4.2-4.7 fields are filled too, except UnitDesc::locomotor, which stays Default: the host opts in
// with locomotor_kind_from_type(types, type_index, mode) (special_locomotors.hpp), so existing hosts keep their
// behaviour (and synced draws) until they route the kind. The Uses_Multiple_Locomotors fields are filled too; they act
// only when the host asks for the swap (use_secondary_locomotor / MoveOrder::switch_locomotor).
UnitDesc unit_desc_from_type(const objects::GameObjectTypes& types, uint32_t type_index);

}  // namespace alamo::tactical::movement
