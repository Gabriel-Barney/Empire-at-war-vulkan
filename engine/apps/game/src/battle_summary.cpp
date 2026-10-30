#include "alamo/game/battle_summary.hpp"

#include <algorithm>

#include "alamo/core/hash.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::game {

namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

}  // namespace

BountyTable load_bounty_table(const data::GameConstants& constants, const objects::GameObjectTypes& types) {
    BountyTable t;
    const objects::EnumConverter& cat = types.enums().get(objects::EnumId::GameObjectCategory);
    for (const auto& [tag, text] : constants.fields) {
        if (!iequals(tag, "Default_Bounty_By_Category_SP")) continue;
        // "<Category>, <credits>"
        const size_t comma = text.find(',');
        if (comma == std::string_view::npos) continue;
        BountyTable::Entry e;
        if (const auto v = cat.lookup(trimmed(text.substr(0, comma)))) e.categories = static_cast<uint64_t>(*v);
        e.amount = static_cast<int32_t>(objects::c_atof(trimmed(text.substr(comma + 1))));
        t.entries.push_back(e);
    }
    return t;
}

int64_t bounty_of(const BountyTable& table, bool has_override, int32_t override_amount, uint64_t categories) {
    if (has_override) return std::max<int64_t>(0, override_amount);
    for (const BountyTable::Entry& e : table.entries)
        if ((e.categories & categories) != 0) return std::max<int64_t>(0, e.amount);
    return 0;
}

int64_t type_bounty(const BountyTable& table, const objects::GameObjectTypes& types, objects::TypeIndex t) {
    if (t >= types.size()) return 0;
    const bool has_override = !types.get_string(t, "Override_Bounty_SP").empty();
    return bounty_of(table, has_override, has_override ? types.get_int(t, "Override_Bounty_SP") : 0,
                     types.get_mask(t, "CategoryMask"));
}

int64_t auto_resolve_battle_seconds(uint32_t combatants, uint32_t losses) {
    return kArBaseSeconds + kArSecondsPerCombatant * static_cast<int64_t>(combatants) +
           kArSecondsPerLoss * static_cast<int64_t>(losses);
}

ArSummary summarize_auto_resolve(std::span<const ArUnitRecord> units, int32_t local_player, bool victory) {
    ArSummary s;
    for (const ArUnitRecord& u : units) {  // list order; integer sums only
        const int side = u.owner == local_player ? 0 : 1;
        ++s.units_total;
        if (!u.destroyed) continue;
        ++s.units_lost[side];
        if (side == 1) s.bounty += u.bounty;
        if (u.cost > 0) s.loss_value[side] += u.cost;  // types costing 0 are not valued (GALACTIC_DIALOGS 3.1)
    }
    s.battle_seconds = auto_resolve_battle_seconds(s.units_total, s.units_lost[0] + s.units_lost[1]);
    const int64_t all = s.loss_value[0] + s.loss_value[1];
    if (all > 0) {
        s.efficiency[0] = static_cast<int32_t>((100 * s.loss_value[1] + all / 2) / all);
        s.efficiency[1] = 100 - s.efficiency[0];
    } else {
        s.efficiency[0] = victory ? 100 : 0;
        s.efficiency[1] = 100 - s.efficiency[0];
    }
    return s;
}

BattleLosses tally_battle_losses(std::span<const BattleLossRecord> dead) {
    BattleLosses out;
    for (const BattleLossRecord& r : dead) ++out[r.owner][r.type];
    return out;
}

BattleKillCount count_battle_kills(std::span<const BattleLossRecord> dead, int32_t local_player) {
    BattleKillCount out;
    for (const BattleLossRecord& r : dead) {
        if (r.owner == local_player) ++out.losses;
        else if (r.owner >= 0) ++out.kills;
    }
    return out;
}

}  // namespace alamo::game
