#include "alamo/tactical/movement/unit_params.hpp"

#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::tactical::movement {

UnitDesc unit_desc_from_type(const objects::GameObjectTypes& t, uint32_t ti, float model_radius) {
    UnitDesc d;
    d.max_speed = t.get_float(ti, "Max_Speed");
    d.turn_rate = t.get_float(ti, "Max_Rate_Of_Turn");
    d.accel = t.get_float(ti, "OverrideAcceleration");
    d.decel = t.get_float(ti, "OverrideDeceleration");
    d.close_enough_angle = t.get_float(ti, "Close_Enough_Angle_For_Move_Start");
    d.movement_class = t.get_enum(ti, "MovementClass");
    d.collision_class = t.get_enum(ti, "UnitCollisionClass");
    d.formation_spacing = t.get_float(ti, "FormationSpacing");
    d.hero = (t.get_mask(ti, "CategoryMask") & 0x6400000u) != 0;  // LandHero | SpaceHero | NonCombatHero
    d.type_name = std::string(t.raw().types().name[ti]);
    d.grouping = t.get_string(ti, "FormationGrouping").empty() ? 2 : t.get_enum(ti, "FormationGrouping");
    d.formation_order = t.get_int(ti, "FormationOrder");
    const uint64_t layer = t.get_mask(ti, "Space_Layer");
    d.space_layer = layer != 0 ? static_cast<uint32_t>(layer) : static_cast<uint32_t>(kLayerFrigate);
    d.layer_z = t.get_float(ti, "Layer_Z_Adjust");
    d.space_layer_type = static_cast<uint32_t>(layer);  // §4.2: threat mode needs 0
    // REMAINING_GAPS §4.3-4.7: special-locomotor fields (the kind itself is the host's opt-in, see the header).
    d.hover_offset = t.get_float(ti, "Hover_Offset");
    d.max_lift = t.get_float(ti, "Max_Lift");
    d.max_rate_of_roll = t.get_float(ti, "Max_Rate_Of_Roll");
    d.bank_turn_angle = t.get_float(ti, "Bank_Turn_Angle");
    if (d.bank_turn_angle == 0.0f) d.bank_turn_angle = static_cast<float>(t.get_int(ti, "Bank_Turn_Angle"));  // int-typed row
    if (t.value(ti, objects::find_type_field("Bank_Turn_Angle")) == nullptr) d.bank_turn_angle = 70.0f;  // unset: 70 (FIGHTER_FLYING §1.4)
    d.min_speed = t.get_float(ti, "Min_Speed");
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
    // OI tactical: stagger, jiggle and crush fields (MOVEMENT.md §1.6, §3.5 step 6, §5.5).
    d.path_frame_delay = t.get_int(ti, "PathFrameDelay");
    d.formation_raggedness = t.get_float(ti, "FormationRaggedness");
    d.max_jiggle_distance = t.get_float(ti, "MaxJiggleDistance");
    d.crusher = t.has_behavior(ti, objects::BehaviorSet::Union, 0x26);  // SQUASH
    d.supercrusher = t.get_bool(ti, "Is_Supercrusher");
    d.squashable = t.get_bool(ti, "Is_Squashable");
    d.squashable_by_supercrusher = t.get_bool(ti, "Is_Squashable_By_Supercrusher");
    const float scale = t.get_float(ti, "Scale_Factor") > 0.0f ? t.get_float(ti, "Scale_Factor") : 1.0f;
    const float soft = t.get_float(ti, "Custom_Soft_Footprint_Radius");
    if (soft > 0.0f) {
        d.radius = soft * scale;
    } else if (model_radius > 0.0f) {
        d.radius = model_radius;
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
