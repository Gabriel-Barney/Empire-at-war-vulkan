#include "alamo/anim/type_params.hpp"

#include <cstdlib>

namespace alamo::anim {

TypeAnimParams type_anim_params(const objects::GameObjectTypes& types, objects::TypeIndex t, objects::ModeKind mode) {
    TypeAnimParams p;
    if (t >= types.size()) return p;
    switch (mode) {
        case objects::ModeKind::Galactic: p.model = std::string(types.get_string(t, "Galactic_Model_Name")); break;
        case objects::ModeKind::Land:
            p.model = std::string(types.get_string(t, "Land_Model_Name"));
            p.anim_override = std::string(types.get_string(t, "Land_Model_Anim_Override_Name"));
            break;
        case objects::ModeKind::Space:
            p.model = std::string(types.get_string(t, "Space_Model_Name"));
            p.anim_override = std::string(types.get_string(t, "Space_Model_Anim_Override_Name"));
            break;
    }
    p.loop_idle = types.get_bool(t, "Loop_Idle_Anim_00");
    // Idle_Anim_00_Rate_Mod defaults to 1.0 (constructor default); an unset field keeps it.
    if (types.get_string(t, "Idle_Anim_00_Rate_Mod").empty()) p.idle_rate = 1.0f;
    else p.idle_rate = types.get_float(t, "Idle_Anim_00_Rate_Mod");
    if (!types.get_string(t, "Movement_Animation_Speed").empty()) p.movement_anim_speed = types.get_float(t, "Movement_Animation_Speed");
    p.attack_overlay = types.get_bool(t, "Attack_Animation_Is_Overlay");
    const int32_t dt = find_anim_type(types.get_string(t, "Specific_Death_Anim_Type"));
    if (dt != kNoAnimType) p.death_type = dt;
    const std::string_view di = types.get_string(t, "Specific_Death_Anim_Index");
    if (!di.empty()) p.death_index = static_cast<uint32_t>(std::strtoul(std::string(di).c_str(), nullptr, 10));
    // R3 behaviours: an unset field keeps the constructor default.
    const auto opt_float = [&types, t](const char* field, float& out) {
        if (!types.get_string(t, field).empty()) out = types.get_float(t, field);
    };
    p.remove_upon_death = types.get_bool(t, "Remove_Upon_Death");
    opt_float("Death_Persistence_Duration", p.death_persistence_seconds);
    opt_float("Death_Fade_Time", p.death_fade_seconds);
    opt_float("Max_Flinch_Blend_Seconds", p.max_flinch_blend_seconds);
    opt_float("Max_Flinch_Hold_Seconds", p.max_flinch_hold_seconds);
    opt_float("Idle_Flinch_Interval_Seconds", p.idle_flinch_interval_seconds);
    opt_float("Attack_Flinch_Interval_Seconds", p.attack_flinch_interval_seconds);
    p.death_type_explicit = dt != kNoAnimType;
    p.spin_away_on_death = types.get_bool(t, "Spin_Away_On_Death");
    opt_float("Spin_Away_On_Death_Chance", p.spin_away_chance);
    opt_float("Spin_Away_On_Death_Time", p.spin_away_seconds);
    p.spin_away_explosion = std::string(types.get_string(t, "Spin_Away_On_Death_Explosion"));
    opt_float("Deployed_Walk_Anim_Speed", p.deployed_walk_anim_speed);
    opt_float("Deployment_Anim_Rate", p.deployment_anim_rate);
    p.deploys = types.get_bool(t, "Deploys");
    return p;
}

FlinchParams flinch_params(const TypeAnimParams& p) {
    FlinchParams f;
    f.deploys = p.deploys;
    f.blend_seconds = p.max_flinch_blend_seconds;
    f.hold_seconds = p.max_flinch_hold_seconds;
    f.idle_interval_seconds = p.idle_flinch_interval_seconds;
    f.attack_interval_seconds = p.attack_flinch_interval_seconds;
    return f;
}

namespace {
float float_or(const objects::GameObjectTypes& types, objects::TypeIndex t, const char* field, float fallback) {
    if (t >= types.size() || types.get_string(t, field).empty()) return fallback;
    return types.get_float(t, field);
}
}  // namespace

float induced_flinch_intensity(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    return float_or(types, t, "Induced_Flinch_Intensity", kDefaultFlinchIntensity);
}

float induced_flinch_range(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    return float_or(types, t, "Induced_Flinch_Range", kDefaultFlinchRange);
}

DeathParams death_params(const TypeAnimParams& p) {
    DeathParams d;
    d.type = p.death_type;
    d.type_explicit = p.death_type_explicit;
    d.index = p.death_index;
    d.remove_upon_death = p.remove_upon_death;
    d.persistence_seconds = p.death_persistence_seconds;
    d.fade_seconds = p.death_fade_seconds;
    // d.spin stays false: the spin-away decision is the caller's (combat's one draw, TACTICAL_GAPS §1.9).
    d.spin_away_seconds = p.spin_away_seconds;
    return d;
}

}  // namespace alamo::anim
