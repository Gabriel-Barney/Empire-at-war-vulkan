// A1 - land campaign-battle credit transfer (docs/re/AI_TACTICAL.md 1.3, 1.4; AI_GALACTIC.md 1.7-1.8)
// for the S10 battle host.
//
// Battle start (land goal slot 6, 0x6b87e0 -> 0x612b00) [H code; M field names]: only when the mode
// under the land mode is GALACTIC (campaign battle) and the AI player is not human. On the GALACTIC
// context's budget, for each category in list order: take = min(allowance x available, available)
// when allowance > 0 and available > 0 (float); available -= take; pool -= take; sum += take. The
// player's credits become sum, and (credits - sum) is saved (budget +0x88, flag +0x84).
// Battle end (land slot 7, 0x6b8860 -> 0x614a70) [H]: leftover = current credits; credits = saved;
// then Add_Credits(leftover - released, aiGain = true) - so positive leftovers are multiplied by
// Credit_Multiplier (quirk kept). `released` is the budget's +0x8c (Release_Credits_For_Tactical,
// 0x6147d0, stage 2 with the budget). Skirmish (no galactic parent) and space: no transfer.
#pragma once
#include <span>

#include "alamo/galactic/economy.hpp"

namespace alamo {
class StateHasher;
}

namespace alamo::ai::tactical {

// One galactic budget category as the transfer sees it (A2's AIBudget fields: +0x5c / +0x44 / +0x4c).
struct TacticalBudgetCategory {
    float allowance = 0.0f;  // template Tactical_Budget_Allowance (fraction of the available credits)
    float available = 0.0f;  // available credits
    float pool = 0.0f;       // pool
};

struct TacticalCreditTransfer {
    bool active = false;     // flag +0x84
    float saved = 0.0f;      // +0x88: credits the player had beyond the transferred sum
    float released = 0.0f;   // +0x8c: credits released by Release_Credits_For_Tactical
    void hash(StateHasher& h) const;
};

// Battle start. Returns the transferred sum (0 and no change when the transfer does not apply).
float begin_land_battle_credits(std::span<TacticalBudgetCategory> galactic_budget,
                                ::alamo::galactic::econ::CreditAccount& player, bool has_galactic_parent,
                                bool player_is_human, TacticalCreditTransfer& state);
// Battle end. Returns the amount Add_Credits applied (0 if no transfer was active).
float end_land_battle_credits(::alamo::galactic::econ::CreditAccount& player, TacticalCreditTransfer& state,
                              float credit_multiplier);

}  // namespace alamo::ai::tactical
