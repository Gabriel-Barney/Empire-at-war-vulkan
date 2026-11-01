// Playtest 2 t13 (alamo_game_battle_summary_tests): the auto-resolve results screen figures (battle_summary.hpp).
//   * data-free: battle time rule, bounty rule (override beats category, first matching category, unknown 0), loss values
//     (cost 0 skipped), efficiency (complementary percentages; nothing lost -> winner 100), determinism of the sums;
//   * FoC data (ALAMO_FOC_DATA, skipped when unset): Default_Bounty_By_Category_SP read from GameConstants (Infantry 5,
//     Capital 500, ...), Override_Bounty_SP honoured on a real type, every type's bounty >= 0 and some type > 0.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "alamo/core/test_support.hpp"
#include "alamo/game/battle_summary.hpp"
#include "alamo/game/game_data.hpp"

using namespace alamo;

namespace {

void test_rules() {
    game::BountyTable t;
    t.entries = {{0x1, 5}, {0x2, 50}, {0x6, 500}, {0x0, 9999}};  // the last never matches (unknown category name)
    CHECK(game::bounty_of(t, false, 0, 0x1) == 5);
    CHECK(game::bounty_of(t, false, 0, 0x2) == 50);
    CHECK(game::bounty_of(t, false, 0, 0x4) == 500);
    CHECK(game::bounty_of(t, false, 0, 0x3) == 5);      // first matching entry in document order
    CHECK(game::bounty_of(t, false, 0, 0x8) == 0);      // no category matches
    CHECK(game::bounty_of(t, false, 0, 0) == 0);
    CHECK(game::bounty_of(t, true, 1800, 0x1) == 1800);  // Override_Bounty_SP beats the category
    CHECK(game::bounty_of(t, true, 0, 0x1) == 0);        // an override of 0 stays 0
    CHECK(game::bounty_of(t, true, -7, 0x1) == 0);       // never negative

    CHECK(game::auto_resolve_battle_seconds(0, 0) == game::kArBaseSeconds);
    CHECK(game::auto_resolve_battle_seconds(10, 4) ==
          game::kArBaseSeconds + 10 * game::kArSecondsPerCombatant + 4 * game::kArSecondsPerLoss);
    CHECK(game::auto_resolve_battle_seconds(10, 4) > 0);

    // Local player 3 (units 0..3) against player 5 (units 4..8); one of ours and three of theirs gone.
    const std::vector<game::ArUnitRecord> u = {
        {3, true, 200, 0},  {3, false, 300, 0}, {3, false, 0, 0},  {3, false, 100, 0},
        {5, true, 400, 50}, {5, true, 100, 15}, {5, true, 0, 15},   {5, false, 250, 500}, {5, false, 80, 5}};
    const game::ArSummary s = game::summarize_auto_resolve(u, 3, true);
    CHECK(s.units_total == 9 && s.units_lost[0] == 1 && s.units_lost[1] == 3);
    CHECK(s.bounty == 50 + 15 + 15);  // only the other side's destroyed units
    CHECK(s.loss_value[0] == 200 && s.loss_value[1] == 500);  // the cost-0 unit has no value
    CHECK(s.efficiency[0] == 71 && s.efficiency[1] == 29);    // 500 / 700
    CHECK(s.battle_seconds == game::auto_resolve_battle_seconds(9, 4) && s.battle_seconds > 0);
    const game::ArSummary again = game::summarize_auto_resolve(u, 3, true);
    CHECK(again.bounty == s.bounty && again.battle_seconds == s.battle_seconds && again.efficiency[0] == s.efficiency[0]);

    // Nothing lost: the winner reads 100, the loser 0; nothing valued but units lost: same.
    const std::vector<game::ArUnitRecord> none = {{3, false, 100, 0}, {5, false, 100, 5}};
    CHECK(game::summarize_auto_resolve(none, 3, true).efficiency[0] == 100);
    CHECK(game::summarize_auto_resolve(none, 3, false).efficiency[0] == 0);
    CHECK(game::summarize_auto_resolve(none, 3, false).efficiency[1] == 100);
    CHECK(game::summarize_auto_resolve(none, 3, true).bounty == 0);
    // Losing everything while killing nothing valued: 0 %.
    const std::vector<game::ArUnitRecord> wiped = {{3, true, 100, 0}, {5, false, 100, 5}};
    const game::ArSummary w = game::summarize_auto_resolve(wiped, 3, false);
    CHECK(w.efficiency[0] == 0 && w.efficiency[1] == 100 && w.bounty == 0);
    // Empty input is safe.
    const game::ArSummary e = game::summarize_auto_resolve({}, 3, true);
    CHECK(e.units_total == 0 && e.bounty == 0 && e.battle_seconds == game::kArBaseSeconds);
}

void test_data(const char* foc) {
    game::GameData data;
    std::string err;
    if (!game::load_game_data(foc, {}, data, &err)) {
        std::printf("FAIL load_game_data: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    const game::BountyTable table = game::load_bounty_table(data.db.constants(), data.types);
    CHECK(table.entries.size() == 11);  // GameConstants: Infantry .. LandHero
    if (table.entries.size() == 11) {
        CHECK(table.entries[0].amount == 5 && table.entries[0].categories != 0);    // Infantry
        CHECK(table.entries[8].amount == 500 && table.entries[8].categories != 0);  // Capital
    }
    int64_t max_bounty = 0;
    uint32_t with_override = 0, positive = 0;
    for (objects::TypeIndex t = 0; t < data.types.size(); ++t) {
        const int64_t b = game::type_bounty(table, data.types, t);
        CHECK(b >= 0);
        max_bounty = std::max(max_bounty, b);
        if (b > 0) ++positive;
        if (!data.types.get_string(t, "Override_Bounty_SP").empty()) {
            ++with_override;
            CHECK(b == std::max<int64_t>(0, data.types.get_int(t, "Override_Bounty_SP")));
        }
    }
    CHECK(positive > 100 && with_override >= 1 && max_bounty >= 500);
    // Infantry units carry the category bounty: a Stormtrooper squad member type or any type whose CategoryMask is Infantry.
    const uint64_t infantry = table.entries.empty() ? 0 : table.entries[0].categories;
    bool found_infantry = false;
    for (objects::TypeIndex t = 0; t < data.types.size() && !found_infantry; ++t)
        if (infantry != 0 && data.types.get_mask(t, "CategoryMask") == infantry &&
            data.types.get_string(t, "Override_Bounty_SP").empty()) {
            CHECK(game::type_bounty(table, data.types, t) == 5);
            found_infantry = true;
        }
    CHECK(found_infantry);
    std::printf("battle_summary data: %zu bounty rows, %u types with a bounty (max %lld), %u overrides\n", table.entries.size(),
                positive, static_cast<long long>(max_bounty), with_override);
}

}  // namespace

// Playtest QA D1 / V1: one battle's records only, per owner and type.
void test_losses() {
    const game::BattleLossRecord dead[] = {{1, 7}, {1, 7}, {0, 3}, {1, 9}};
    const game::BattleLosses l = game::tally_battle_losses(dead);
    CHECK(l.size() == 2 && l.at(1).at(7) == 2 && l.at(1).at(9) == 1 && l.at(0).at(3) == 1);
    CHECK(game::tally_battle_losses({}).empty());
    // t41: the campaign score counts from the same records (owner -1 is nobody's kill).
    const game::BattleLossRecord mixed[] = {{1, 7}, {1, 7}, {0, 3}, {2, 9}, {-1, 4}};
    const game::BattleKillCount k = game::count_battle_kills(mixed, 0);
    CHECK(k.kills == 3 && k.losses == 1);
    const game::BattleKillCount none = game::count_battle_kills({}, 0);
    CHECK(none.kills == 0 && none.losses == 0);
}

int main() {
    test_rules();
    test_losses();
    if (const char* foc = std::getenv("ALAMO_FOC_DATA"); foc && *foc) {
        test_data(foc);
    } else {
        std::printf("alamo_game_battle_summary_tests: ALAMO_FOC_DATA not set: data checks skipped\n");
    }
    if (g_fail) {
        std::printf("alamo_game_battle_summary_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_game_battle_summary_tests: OK\n");
    return 0;
}
