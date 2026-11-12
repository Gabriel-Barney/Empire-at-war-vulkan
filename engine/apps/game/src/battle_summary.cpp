#include "alamo/game/battle_summary.hpp"

#include <algorithm>

#include "alamo/core/hash.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::game {

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
