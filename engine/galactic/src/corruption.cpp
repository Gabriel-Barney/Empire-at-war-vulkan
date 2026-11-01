// S4 (owner: S4 agent): corruption options (corruption.hpp).
#include "alamo/galactic/corruption.hpp"

#include <string_view>

#include "alamo/core/text.hpp"
#include "alamo/objects/game_object_types.hpp"

namespace alamo::galactic {

std::vector<CorruptionOption> corruption_options(const objects::GameObjectTypes& types, objects::TypeIndex planet_type) {
    std::vector<CorruptionOption> out;
    if (planet_type == objects::kNone || planet_type >= types.size()) return out;
    for (uint32_t slot = 1; slot <= 3; ++slot) {
        const std::string p = "Corruption_" + std::to_string(slot) + "_";
        if (ascii_trim(types.get_string(planet_type, p + "Type")).empty()) continue;
        CorruptionOption o;
        o.slot = slot;
        o.type = static_cast<int8_t>(types.get_enum(planet_type, p + "Type"));
        o.credit_cost = types.get_int(planet_type, p + "Credit_Cost");
        if (o.credit_cost == 0) {  // stored as a float field: rounded half up
            const float c = types.get_float(planet_type, p + "Credit_Cost");
            if (c > 0.0f) o.credit_cost = static_cast<int32_t>(c + 0.5f);
        }
        o.required_unit = ascii_trim(types.get_string(planet_type, p + "Required_Unit_Type"));
        o.required_unit_count = types.get_int(planet_type, p + "Required_Unit_Count");
        if (!ascii_trim(types.get_string(planet_type, p + "Required_Mission_Type")).empty())
            o.required_mission = static_cast<int8_t>(types.get_enum(planet_type, p + "Required_Mission_Type"));
        o.required_mission_count = types.get_int(planet_type, p + "Required_Mission_Count");
        o.map = ascii_trim(types.get_string(planet_type, p + "Tactical_Map_Name"));
        o.plots = ascii_trim(types.get_string(planet_type, p + "Tactical_Script_Name"));
        const bool has_mode = !ascii_trim(types.get_string(planet_type, p + "Tactical_Game_Mode")).empty();
        const int32_t mode = types.get_enum(planet_type, p + "Tactical_Game_Mode");  // Galactic 0, Land 1, Space 2
        o.kind = mode == 2 ? ConflictKind::Space : ConflictKind::Land;
        o.tactical = has_mode && (mode == 1 || mode == 2) && !o.map.empty();
        out.push_back(std::move(o));
    }
    return out;
}

const CorruptionOption* find_corruption_option(const std::vector<CorruptionOption>& options, int8_t type) {
    for (const CorruptionOption& o : options)
        if (o.type == type) return &o;
    return nullptr;
}

}  // namespace alamo::galactic
