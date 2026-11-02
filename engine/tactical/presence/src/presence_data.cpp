#include "alamo/tactical/presence/presence_data.hpp"

#include "alamo/data/game_database.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/tactical/presence/explosion_flinch.hpp"

namespace alamo::tactical::presence {

std::vector<InducedAnimation> parse_presence_induced_animations(std::span<const std::string_view> tokens,
                                                                const std::function<int32_t(std::string_view)>& anim_type,
                                                                const std::function<TypeId(std::string_view)>& type) {
    std::vector<InducedAnimation> out;
    for (const std::string_view tok : tokens) {
        if (tok.empty()) continue;
        const int32_t a = anim_type ? anim_type(tok) : kNoAnimType;
        if (a >= 0) {
            out.push_back(InducedAnimation{a, {}});
            continue;
        }
        if (out.empty() || !type) continue;
        const TypeId t = type(tok);
        if (t != kNoType) out.back().types.push_back(t);
    }
    return out;
}

PresenceTypeTable build_presence_types(const objects::GameObjectTypes& types, int32_t (*anim_type)(std::string_view)) {
    PresenceTypeTable table;
    table.resize(types.size());
    const std::function<int32_t(std::string_view)> anim = [anim_type](std::string_view s) {
        return anim_type ? anim_type(s) : kNoAnimType;
    };
    const std::function<TypeId(std::string_view)> resolve = [&types](std::string_view s) -> TypeId {
        const objects::TypeIndex t = types.resolve_name(s);
        return t == objects::kNone ? kNoType : static_cast<TypeId>(t);
    };
    for (size_t i = 0; i < types.size(); ++i) {
        const auto t = static_cast<objects::TypeIndex>(i);
        PresenceTypeInfo& info = table.at(static_cast<TypeId>(i));
        const std::span<const std::string_view> list = types.get_list(t, "Presence_Induced_Animations");
        if (!list.empty()) info.induced = parse_presence_induced_animations(list, anim, resolve);
        info.presence_range = types.get_float(t, "Presence_Range");
        info.allow_idle_when_moving = types.get_bool(t, "Allow_Idle_When_Moving");
        info.deploys = types.get_bool(t, "Deploys");
    }
    return table;
}

PresenceConfig presence_config(const data::GameConstants* constants, uint32_t fps) {
    PresenceConfig c;
    c.fps = fps;
    if (constants) c.relaxation_seconds = constants->get_float("Unit_Presence_Relaxation_Time", kDefaultRelaxationSeconds);
    return c;
}

ExplosionFlinchParams explosion_flinch_params(const objects::GameObjectTypes& types, TypeId t) {
    ExplosionFlinchParams p;
    p.range = kDefaultInducedFlinchRange;
    p.intensity = kDefaultInducedFlinchIntensity;
    if (t >= types.size()) return p;
    const auto ti = static_cast<objects::TypeIndex>(t);
    const uint32_t fr = objects::find_type_field("Induced_Flinch_Range");
    const uint32_t fi = objects::find_type_field("Induced_Flinch_Intensity");
    if (fr != 0xFFFFFFFFu)
        if (const objects::FieldValue* v = types.value(ti, fr)) p.range = v->f;
    if (fi != 0xFFFFFFFFu)
        if (const objects::FieldValue* v = types.value(ti, fi)) p.intensity = v->f;
    return p;
}

}  // namespace alamo::tactical::presence
