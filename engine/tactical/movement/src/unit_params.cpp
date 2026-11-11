#include "alamo/tactical/movement/unit_params.hpp"

#include <array>

#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::tactical::movement {

UnitDesc unit_desc_from_type(const objects::GameObjectTypes& t, uint32_t ti) {
    UnitDesc d;
    d.max_speed = t.get_float(ti, "Max_Speed");
    d.turn_rate = t.get_float(ti, "Max_Rate_Of_Turn");
    d.accel = t.get_float(ti, "OverrideAcceleration");
    d.decel = t.get_float(ti, "OverrideDeceleration");
    d.hover_height = t.get_float(ti, "Hover_Height");  // s78n t18
    d.close_enough_angle = t.get_float(ti, "Close_Enough_Angle_For_Move_Start");
    d.movement_class = t.get_enum(ti, "MovementClass");
    d.collision_class = t.get_enum(ti, "UnitCollisionClass");
    d.formation_spacing = t.get_float(ti, "FormationSpacing");
    d.hero = (t.get_mask(ti, "CategoryMask") & 0x6400000u) != 0;  // LandHero | SpaceHero | NonCombatHero
    d.type_name = std::string(t.raw().types().name[ti]);
    d.grouping = t.get_string(ti, "FormationGrouping").empty() ? 2 : t.get_enum(ti, "FormationGrouping");
    d.formation_order = t.get_int(ti, "FormationOrder");
    // s78n t14 (FIGHTER_FLYING §7.2): ContainerArrangement (None -1 when unset)
    d.container_arrangement = t.get_string(ti, "ContainerArrangement").empty() ? -1 : t.get_enum(ti, "ContainerArrangement");
    d.movement_prediction_interval = t.get_float(ti, "MovementPredictionInterval");  // s78n t14 part B
    d.avoidance_disabled = t.get_bool(ti, "Avoidance_Disabled");
    const uint64_t layer = t.get_mask(ti, "Space_Layer");
    d.space_layer = layer != 0 ? static_cast<uint32_t>(layer) : static_cast<uint32_t>(kLayerFrigate);
    d.layer_z = t.get_float(ti, "Layer_Z_Adjust");
    d.space_layer_type = static_cast<uint32_t>(layer);  // §4.2: threat mode needs 0
    d.stationary_space_layer = static_cast<uint32_t>(t.get_int(ti, "Stationary_Space_Layer"));  // cu8j t01 (§4.6.4)
    // REMAINING_GAPS §4.3-4.7: special-locomotor fields (the kind itself is the host's opt-in, see the header).
    d.hover_offset = t.get_float(ti, "Hover_Offset");
    d.max_lift = t.get_float(ti, "Max_Lift");
    d.max_rate_of_roll = t.get_float(ti, "Max_Rate_Of_Roll");
    d.bank_turn_angle = t.get_float(ti, "Bank_Turn_Angle");
    if (d.bank_turn_angle == 0.0f) d.bank_turn_angle = static_cast<float>(t.get_int(ti, "Bank_Turn_Angle"));  // int-typed row
    if (t.value(ti, objects::find_type_field("Bank_Turn_Angle")) == nullptr) d.bank_turn_angle = 70.0f;  // unset: 70 (FIGHTER_FLYING §1.4)
    d.min_speed = t.get_float(ti, "Min_Speed");
    d.min_speed_fraction_for_turn = t.get_float(ti, "Min_Speed_Fraction_For_Turn");  // s78n t26.3
    d.strafe_distance = t.get_float(ti, "Strafe_Distance");
    d.prepare_strafe_height = t.get_float(ti, "Prepare_Strafe_Height");
    d.has_pre_turn_anim = t.get_bool(ti, "Has_Pre_Turn_Anim");
    // S7 t06 / t14: Uses_Multiple_Locomotors (MOVEMENT §3.1.1). The primary is the behaviour-list locomotor the host
    // routes (the swap back builds Primary_Locomotor_Name; every FoC user names the WALK_LOCOMOTOR of its list).
    d.uses_multiple_locomotors = t.get_bool(ti, "Uses_Multiple_Locomotors");
    d.secondary_locomotor = locomotor_kind_from_token(t.get_string(ti, "Secondary_Locomotor_Name"));
    d.has_secondary_locomotor = d.secondary_locomotor == LocomotorKind::Jetpack &&  // [port] JETPACK secondaries only
                                locomotor_kind_from_token(t.get_string(ti, "Primary_Locomotor_Name")) != LocomotorKind::Jetpack;
    d.alt_max_speed = t.get_float(ti, "Alternate_Max_Speed");  // unconditional: the type defaults are 1.0 / 2.0
    d.alt_turn_rate = t.get_float(ti, "Alternate_Max_Rate_Of_Turn");
    // S8 t11: Deployment_Anim_Rate, unset -> the type ctor's 1.0 (MOVEMENT §3.2).
    if (t.value(ti, objects::find_type_field("Deployment_Anim_Rate")) != nullptr) d.deployment_anim_rate = t.get_float(ti, "Deployment_Anim_Rate");
    // s78n t26.1 (MOVEMENT §3.2.1): walking while deployed (deploy_ability is the host's).
    d.deploys = t.get_bool(ti, "Deploys");
    d.auto_deploys = t.get_bool(ti, "Auto_Deploys");
    d.max_walk_deployed = t.get_float(ti, "Max_Dist_Walk_While_Deployed");
    d.deployed_walk_speed = t.get_float(ti, "Deployed_Walk_Speed");
    // s78n t26 closing review (MOVEMENT §3.4 "Units with Walk_Transition"): the flag and S = Scale_Factor x
    // Movement_Animation_Speed (unset: the type defaults 1.0 / 0.25)
    d.walk_transition = t.get_bool(ti, "Walk_Transition");
    {
        const float sc = t.get_float(ti, "Scale_Factor") > 0.0f ? t.get_float(ti, "Scale_Factor") : 1.0f;
        const float mas = t.value(ti, objects::find_type_field("Movement_Animation_Speed")) != nullptr
                              ? t.get_float(ti, "Movement_Animation_Speed")
                              : 0.25f;
        d.anim_scale_speed = sc * mas;
    }
    // s78k t01 (MOVEMENT §3.5.1): the arrival turn's stopped rate and the direction count (unset -> the type default 1).
    d.stopped_turn_rate = t.get_float(ti, "Stopped_Rate_Of_Turn");
    if (t.value(ti, objects::find_type_field("Movement_Directions_Count")) != nullptr)
        d.movement_directions = t.get_int(ti, "Movement_Directions_Count");
    d.rotation_animation_speed = t.get_float(ti, "Rotation_Animation_Speed");  // s78l t02
    // S8 t24: the TURRET of a Deploys type (MOVEMENT §3.2 "Turret recentre around deploy"). s78f t08 (TACTICAL_COMBAT
    // §3.1.1 "s78f t08 answers" 6 [H]): every TURRET type slews the same way (0x632050: one Turret_Rotate_Speed step per
    // service, no shortcut for non-deployers; the deploy state only picks the deployed extents / rest angle), so the model
    // covers every TURRET type; the tick runs it on land only. [port] Turret_Rotate_Speed <= 0: no model (aimed at once).
    const auto is_set = [&](const char* tag) { return t.value(ti, objects::find_type_field(tag)) != nullptr; };
    if (t.has_behavior(ti, objects::BehaviorSet::Union, 0x0B) && t.get_float(ti, "Turret_Rotate_Speed") > 0.0f) {
        d.turret = true;
        d.turret_speed = t.get_float(ti, "Turret_Rotate_Speed");
        if (is_set("Turret_Rotate_Extent_Degrees")) d.turret_extent = t.get_float(ti, "Turret_Rotate_Extent_Degrees");
        d.turret_deployed_extent = is_set("Deployed_Turret_Rotate_Extent_Degrees") ? t.get_float(ti, "Deployed_Turret_Rotate_Extent_Degrees")
                                                                                   : d.turret_extent;
        const std::array<float, 4> r = t.get_vec(ti, "Turret_Rest_Angle"), dr = t.get_vec(ti, "Turret_Deployed_Rest_Angle");
        for (size_t k = 0; k < 3; ++k) {
            d.turret_rest[k] = r[k];
            d.turret_deployed_rest[k] = dr[k];
        }
        if (is_set("Turret_Rotate_To_Rest_Delay_Sec")) d.turret_rest_delay = t.get_float(ti, "Turret_Rotate_To_Rest_Delay_Sec");
    }
    // OI tactical: stagger, jiggle and crush fields (MOVEMENT.md §1.6, §3.5 step 6, §5.5).
    d.path_frame_delay = t.get_int(ti, "PathFrameDelay");
    d.formation_raggedness = t.get_float(ti, "FormationRaggedness");
    d.max_jiggle_distance = t.get_float(ti, "MaxJiggleDistance");
    d.crusher = t.has_behavior(ti, objects::BehaviorSet::Union, 0x26);  // SQUASH
    d.supercrusher = t.get_bool(ti, "Is_Supercrusher");
    d.squashable = t.get_bool(ti, "Is_Squashable");
    d.squashable_by_supercrusher = t.get_bool(ti, "Is_Squashable_By_Supercrusher");
    const float scale = t.get_float(ti, "Scale_Factor") > 0.0f ? t.get_float(ti, "Scale_Factor") : 1.0f;
    d.hard_x = t.get_float(ti, "Custom_Hard_XExtent") * scale;
    d.hard_y = t.get_float(ti, "Custom_Hard_YExtent") * scale;
    d.hard_x_deployed = t.get_float(ti, "Custom_Hard_XExtent_Deployed") * scale;  // s78n t23
    d.hard_y_deployed = t.get_float(ti, "Custom_Hard_YExtent_Deployed") * scale;
    d.box_expansion = t.get_float(ti, "MovementBoxExpansionFactor");  // s78n t20
    const float soft = t.get_float(ti, "Custom_Soft_Footprint_Radius");
    if (soft > 0.0f) {
        d.radius = soft * scale;
    } else {
        switch (d.collision_class) {
            case 5: d.radius = 6.0f; break;
            case 6: d.radius = 10.0f; break;
            case 7: d.radius = 20.0f; break;
            case 8: d.radius = 40.0f; break;
            case 9: d.radius = 80.0f; break;
            default: d.radius = 15.0f; break;
        }
    }
    return d;
}

}  // namespace alamo::tactical::movement
