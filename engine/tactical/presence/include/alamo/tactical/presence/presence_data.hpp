// Builds the presence definitions from the typed GameObjectType view and GameConstants (library
// alamo_tactical_presence_data; configured when the objects and data modules are).
//
// The type fields come from engine/objects' typed accessors (R3f t05):
//   Presence_Induced_Animations (code 0x35, +0x1610): presence_induced_animations() tokens, see
//     parse_presence_induced_animations();
//   Presence_Range (+0x1628, float, default 0), Allow_Idle_When_Moving (+0x85), Deploys (+0xd1): presence_range /
//     allow_idle_when_moving / deploys;
//   Induced_Flinch_Range / _Intensity (+0x20e0 / +0x20e4): induced_flinch_range / _intensity, constructor defaults
//     100 / 1.0 (objects' type_field_defaults());
//   GameConstants Unit_Presence_Relaxation_Time (+0x117a8): get_float(tag, 5.0).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "alamo/tactical/presence/presence_system.hpp"

namespace alamo::objects {
class GameObjectTypes;
}
namespace alamo::data {
struct GameConstants;
}

namespace alamo::tactical::presence {

// Code 0x35 parse: a token that converts as an anim type name (`anim_type` >= 0) starts a new entry; any other
// token is a type name appended to the last entry's list. [port] Type names that do not resolve (`type` returns
// kNoType) and type names before the first anim name are dropped.
std::vector<InducedAnimation> parse_presence_induced_animations(std::span<const std::string_view> tokens,
                                                                const std::function<int32_t(std::string_view)>& anim_type,
                                                                const std::function<TypeId(std::string_view)>& type);

// One PresenceTypeInfo per TypeIndex. `anim_type`: ModelAnimType name -> id (pass anim::find_anim_type).
PresenceTypeTable build_presence_types(const objects::GameObjectTypes& types, int32_t (*anim_type)(std::string_view));

// fps / interval as given; relaxation_seconds from GameConstants (default 5.0 when null or unset).
PresenceConfig presence_config(const data::GameConstants* constants, uint32_t fps = kDefaultFps);

struct ExplosionFlinchParams {
    float range = 100.0f;      // Induced_Flinch_Range
    float intensity = 1.0f;    // Induced_Flinch_Intensity
};
ExplosionFlinchParams explosion_flinch_params(const objects::GameObjectTypes& types, TypeId t);

}  // namespace alamo::tactical::presence
