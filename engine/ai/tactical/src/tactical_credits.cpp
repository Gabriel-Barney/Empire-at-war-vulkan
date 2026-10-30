#include "alamo/ai/tactical/tactical_credits.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai::tactical {

void TacticalCreditTransfer::hash(StateHasher& h) const {
    h.boolean(active);
    h.f32(saved);
    h.f32(released);
}

float begin_land_battle_credits(std::span<TacticalBudgetCategory> galactic_budget,
                                ::alamo::galactic::econ::CreditAccount& player, bool has_galactic_parent,
                                bool player_is_human, TacticalCreditTransfer& state) {
    if (!has_galactic_parent || player_is_human) return 0.0f;
    float sum = 0.0f;
    for (TacticalBudgetCategory& c : galactic_budget) {
        if (!(c.allowance > 0.0f) || !(c.available > 0.0f)) continue;
        const float take = std::min(c.allowance * c.available, c.available);
        c.available -= take;
        c.pool -= take;
        sum += take;
    }
    state.saved = player.credits - sum;
    state.released = 0.0f;
    state.active = true;
    player.credits = sum;
    return sum;
}

float end_land_battle_credits(::alamo::galactic::econ::CreditAccount& player, TacticalCreditTransfer& state,
                              float credit_multiplier) {
    if (!state.active) return 0.0f;
    const float leftover = player.credits;
    player.credits = state.saved;
    const float applied =
        ::alamo::galactic::econ::add_credits(player, leftover - state.released, true, credit_multiplier);
    state.active = false;
    return applied;
}

}  // namespace alamo::ai::tactical
