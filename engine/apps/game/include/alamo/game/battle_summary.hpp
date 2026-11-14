// Playtest 2 t13: the figures of the auto-resolve results screen (IDD_BATTLE_END_DIALOG after the local player's galactic
// auto-resolve): battle time, bounty ("Salvage collected") and the combat efficiency. Display-only: pure functions of the
// forces recorded when the auto-resolve started and the ones gone afterwards; nothing here touches the sim state, the
// RNG or the credits, so the per-tick CRC and the goldens are unchanged. Render-free (alamo_game_core).
//
// What is recovered (docs/re/GALACTIC_DIALOGS.md 3.1, BATTLE_TRANSITIONS 4.4): the screen shows a battle time
// (TEXT_BATTLE_TIME), TEXT_SUMMARY_BOUNTY_COLLECTED "Salvage collected: $ %d", the credit value of each side's losses
// (types costing 0 skipped) and a combat efficiency per side. The data side of bounty is recovered: GameConstants
// Default_Bounty_By_Category_SP (category -> credits) and the per-type Override_Bounty_SP. NOT recovered (all [M]):
//   * battle time   the original resolves in one tick, so no tactical timer exists; the port shows a simulated duration
//                   from the result: kBaseSeconds + kSecondsPerCombatant x units that fought + kSecondsPerLoss x units
//                   destroyed (both sides). Deterministic, integer, no wall clock.
//   * bounty        the sum of the bounty of every destroyed unit that is not the local player's: Override_Bounty_SP when
//                   the type sets it (0 allowed), else the amount of the FIRST Default_Bounty_By_Category_SP entry
//                   (document order) whose category intersects the type's CategoryMask, else 0. Shown only here; since
//                   a2m r2 the galactic sim pays the bounty of a player's Bounty_On_Faction abilities for auto-resolve
//                   kills (conflicts.cpp, docs/re/A2M_R2_UW_BOUNTY_TRACE.md §1).
//   * efficiency    per side: the other side's lost credit value / both sides' lost credit value, rounded to a percent
//                   (nothing lost on either side: 100 for the winner, 0 for the loser).
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "alamo/data/game_database.hpp"
#include "alamo/galactic/bounty.hpp"
#include "alamo/objects/game_object_types.hpp"

namespace alamo::game {

// a2m r2: the bounty rule lives in the galactic sim (galactic/bounty.hpp), which now also pays it for galactic
// auto-resolve kills; the results screen shows the same values.
using galactic::BountyTable;
using galactic::bounty_of;
using galactic::load_bounty_table;
using galactic::type_bounty;

// One unit of the auto-resolve's forces (recorded when it started; `destroyed` evaluated after it ended).
struct ArUnitRecord {
    int32_t owner = -1;
    bool destroyed = false;
    int64_t cost = 0;    // build cost in credits (0: not listed / valued, as the original skips such types)
    int64_t bounty = 0;  // type_bounty()
};

struct ArSummary {
    uint32_t units_total = 0, units_lost[2] = {0, 0};  // [0] = local player, [1] = the other side
    int64_t battle_seconds = 0;
    int64_t bounty = 0;
    int64_t loss_value[2] = {0, 0};   // credit value of the destroyed units with cost > 0: [0] yours, [1] the enemy's
    int32_t efficiency[2] = {0, 0};   // percent: [0] yours, [1] the enemy's
};

inline constexpr int64_t kArBaseSeconds = 20;
inline constexpr int64_t kArSecondsPerCombatant = 4;
inline constexpr int64_t kArSecondsPerLoss = 12;

int64_t auto_resolve_battle_seconds(uint32_t combatants, uint32_t losses);
ArSummary summarize_auto_resolve(std::span<const ArUnitRecord> units, int32_t local_player, bool victory);

// Playtest QA D1 / V1: the tactical battle-end loss lists, owner -> type -> count, from one battle's dead units
// (sim::TacticalBattle::unit_losses; owner / type pairs here so this stays sim-free). Built per battle: nothing carries over from
// the attract demo or an earlier battle, and the final tick's deaths count.
struct BattleLossRecord {
    int32_t owner = -1;
    uint32_t type = 0;
};
using BattleLosses = std::map<int32_t, std::map<uint32_t, uint32_t>>;
BattleLosses tally_battle_losses(std::span<const BattleLossRecord> dead);
// t41: the campaign score's kills / losses of one battle, from the same records, so the final tick's deaths count (the battle
// is gone by the time the host could read that tick's death events). Kills = every other player's dead units, as
// CombatSystem::is_enemy treats any two different players in a battle.
struct BattleKillCount {
    uint32_t kills = 0, losses = 0;
};
BattleKillCount count_battle_kills(std::span<const BattleLossRecord> dead, int32_t local_player);

}  // namespace alamo::game
