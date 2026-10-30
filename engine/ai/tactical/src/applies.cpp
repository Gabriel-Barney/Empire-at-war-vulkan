#include "alamo/ai/tactical/applies.hpp"


namespace alamo::ai::tactical {

bool parse_goal_application(std::string_view text, uint32_t& bits) {
    bool ok = true;
    bits = parse_application_flags(text, &ok);
    return ok && !ai_split(text).empty();
}

bool is_friendly(const UnitAppliesInfo& u) { return u.has_owner && u.owner_allied; }

UnitClassBits classify_unit(const UnitAppliesInfo& u) {
    if (u.has_locomotor) return {kAppFriendlyUnit, kAppEnemyUnit};
    if (u.has_build_objects) return {kAppFriendlyBuildPad, kAppEnemyBuildPad};
    if (u.has_cash_point) return {kAppFriendlyCashPoint, kAppEnemyCashPoint};
    if (u.reinforcement_enabling_radius > 0.0f) return {kAppFriendlyReinforcementPoint, kAppEnemyReinforcementPoint};
    return {kAppFriendlyStructure, kAppEnemyStructure};
}

bool region_applies(uint32_t flags) { return (flags & kAppTacticalLocation) != 0; }

bool unit_applies(uint32_t flags, const UnitAppliesInfo& u, bool fully_fogged) {
    if (fully_fogged && !u.last_state_visible_under_fow && !u.initial_state_visible_under_fow) return false;
    const UnitClassBits c = classify_unit(u);
    return is_friendly(u) ? (flags & c.friendly) != 0 : (flags & c.enemy) != 0;
}

bool unit_applies(uint32_t flags, const UnitAppliesInfo& u, const FogQuery& fully_fogged) {
    const bool visible_type = u.last_state_visible_under_fow || u.initial_state_visible_under_fow;
    const bool fogged = !visible_type && fully_fogged && fully_fogged(u.object_id);
    return unit_applies(flags, u, fogged);
}

}  // namespace alamo::ai::tactical
