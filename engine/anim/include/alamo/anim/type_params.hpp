// R3: the GameObjectType fields animation reads (docs/re/ANIM_PARTICLES.md §A.2, §A.7), resolved from the
// typed object view (engine/objects). Built only when engine/objects is configured
// (ALAMO_ANIM_HAVE_OBJECTS).
#pragma once
#include <cstdint>
#include <string>

#include "alamo/anim/anim_behaviors.hpp"
#include "alamo/anim/anim_types.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::anim {

struct TypeAnimParams {
    std::string model;          // Galactic_ / Land_ / Space_Model_Name for the mode ("" = no model)
    std::string anim_override;  // Land_ / Space_Model_Anim_Override_Name ("" = the model's base name)
    bool loop_idle = false;     // Loop_Idle_Anim_00 (type +0x30)
    float idle_rate = 1.0f;     // Idle_Anim_00_Rate_Mod (type +0x1640)
    float movement_anim_speed = 1.0f;     // Movement_Animation_Speed (type +0x370)
    bool attack_overlay = false;          // Attack_Animation_Is_Overlay (type +0x5c)
    int32_t death_type = kAnimDie;        // Specific_Death_Anim_Type (+0xd98), default 7 Die
    uint32_t death_index = 0xFFFF;        // Specific_Death_Anim_Index (+0xd9c), default 0xFFFF = random
    // R3 behaviours (anim_behaviors.hpp): death / corpse and flinch fields with their constructor defaults.
    bool remove_upon_death = false;           // Remove_Upon_Death (+0x9b)
    float death_persistence_seconds = -1.0f;  // Death_Persistence_Duration (+0x2290), -1 = forever
    float death_fade_seconds = 0.25f;         // Death_Fade_Time (+0x2294)
    float max_flinch_blend_seconds = 0.25f;   // Max_Flinch_Blend_Seconds (+0x20e8)
    float max_flinch_hold_seconds = 0.5f;     // Max_Flinch_Hold_Seconds (+0x20ec)
    float idle_flinch_interval_seconds = 1.0f;    // Idle_Flinch_Interval_Seconds (+0x20f4)
    float attack_flinch_interval_seconds = 3.0f;  // Attack_Flinch_Interval_Seconds (+0x20f0)
    bool death_type_explicit = false;             // Specific_Death_Anim_Type given
    bool spin_away_on_death = false;              // Spin_Away_On_Death (+0x9a)
    float spin_away_chance = 0.0f;                // Spin_Away_On_Death_Chance (+0x2208)
    float spin_away_seconds = 0.0f;               // Spin_Away_On_Death_Time (+0x220c)
    std::string spin_away_explosion;              // Spin_Away_On_Death_Explosion (render / particle side)
    float deployed_walk_anim_speed = 1.0f;        // Deployed_Walk_Anim_Speed
    float deployment_anim_rate = 1.0f;            // Deployment_Anim_Rate (+0x1668)
};

TypeAnimParams type_anim_params(const objects::GameObjectTypes& types, objects::TypeIndex t, objects::ModeKind mode);
// The DeathBehavior fields of a type (AnimBehaviors::begin_death / plan_death).
DeathParams death_params(const TypeAnimParams& p);

}  // namespace alamo::anim
