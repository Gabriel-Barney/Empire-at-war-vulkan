#include "alamo/tactical/abilities/countdown.hpp"

#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::abilities {

void Countdown::set_duration(uint32_t t, float seconds, uint32_t fps) {
    const auto frames = static_cast<int32_t>(static_cast<float>(fps) * seconds);
    counter[t] = frames;
    maximum[t] = frames;
}

bool Countdown::start_active(uint32_t t, float expiration_seconds, uint32_t fps) {
    if (!(expiration_seconds > 0.0f)) return false;
    set_duration(t, expiration_seconds, fps);
    counter[t] = 0;
    phase[t] = 1;
    return true;
}

bool Countdown::start_recharge(uint32_t t, float recharge_seconds, float r9, uint32_t fps) {
    const float r = recharge_seconds * (1.0f + r9);
    if (!(r > 0.0f)) return false;
    phase[t] = 0;
    set_duration(t, r, fps);
    return true;
}

bool Countdown::deactivate_early(uint32_t t, float recharge_seconds, float r9, uint32_t fps) {
    if (phase[t] == 0) {
        start_recharge(t, recharge_seconds, r9, fps);
        return false;
    }
    const float f = (counter[t] == 0 || maximum[t] == 0)
                        ? 1.0f
                        : 1.0f - static_cast<float>(counter[t]) / static_cast<float>(maximum[t]);
    start_recharge(t, recharge_seconds, r9, fps);
    const auto c = static_cast<int32_t>(static_cast<float>(maximum[t]) * (1.0f - f) + 0.5f);
    counter[t] = c > 0 ? c : 0;
    return maximum[t] == 0;
}

void Countdown::hash(StateHasher& h) const {
    h.u32(last_update);
    for (uint32_t t = 0; t < kAbilitySlots; ++t) {
        if (counter[t] == 0 && maximum[t] == 0 && phase[t] == 0) continue;
        h.u32(t);
        h.i32(counter[t]);
        h.i32(maximum[t]);
        h.u8(phase[t]);
    }
    h.u32(0xFFFFFFFFu);
}

}  // namespace alamo::tactical::abilities
