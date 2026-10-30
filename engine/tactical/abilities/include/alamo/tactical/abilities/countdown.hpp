// S9 - AbilityCountdownBehavior (behaviour 0x46, obj +0x1e8) arithmetic, exactly as
// docs/re/ABILITIES_HARDPOINTS.md 2.6 [H]. Integer frames; int(x) is truncation.
#pragma once
#include <array>
#include <cstdint>

#include "alamo/tactical/abilities/ability_types.hpp"

namespace alamo {
class StateHasher;
}

namespace alamo::tactical::abilities {

struct Countdown {
    std::array<int32_t, kAbilitySlots> counter{};  // +8
    std::array<int32_t, kAbilitySlots> maximum{};  // +0x13c
    std::array<uint8_t, kAbilitySlots> phase{};    // +0x270: 1 = active (counting up), 0 = recharge (down)
    uint32_t last_update = 0;                      // +0x2c0

    // 0x42fae0: counter = maximum = int(FPS x seconds).
    void set_duration(uint32_t t, float seconds, uint32_t fps);
    // 0x42fde0: only if expiration > 0: set_duration(expiration), counter := 0, phase := 1.
    bool start_active(uint32_t t, float expiration_seconds, uint32_t fps);
    // 0x42fb70: R = recharge x (1 + r9) (r9 = list 9 of the object's own set); R > 0: phase := 0,
    // set_duration(R); otherwise nothing changes. Returns R > 0.
    bool start_recharge(uint32_t t, float recharge_seconds, float r9, uint32_t fps);
    // 0x42f320: phase 0 -> start_recharge. Phase 1 -> f = 1 - counter / maximum (1 when either is 0),
    // start_recharge, counter := max(0, int(maximum x (1 - f) + 0.5)). Returns true when the
    // recharge maximum is 0 afterwards (the caller raises "ability ready", signal 0x2c).
    bool deactivate_early(uint32_t t, float recharge_seconds, float r9, uint32_t fps);
    // 0x42f8e0
    bool counter_zero(uint32_t t) const { return counter[t] < 1; }
    // 0x42f7a0 [M]: recharging = phase 0 with frames left.
    bool recharging(uint32_t t) const { return phase[t] == 0 && counter[t] > 0; }
    // Can_Activate gate 7: counter >= 1 and (phase 0, or the active phase reached its maximum).
    bool blocks_activation(uint32_t t) const {
        return counter[t] >= 1 && (phase[t] == 0 || counter[t] >= maximum[t]);
    }
    void hash(StateHasher& h) const;
};

}  // namespace alamo::tactical::abilities
