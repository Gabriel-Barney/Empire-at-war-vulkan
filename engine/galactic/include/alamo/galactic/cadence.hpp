// AI all-pairs recompute schedule (docs/re/PHASE2_ANSWERS.md Q11). Pure functions; the frame
// is the current game mode's frame counter (mode +0x10; assumed to advance once per 30 Hz logic
// tick like the global FrameSynchronizer counter, not verified).
//
//   creation (0x526120): Recompute immediately (outside the cadence), then
//                        next = first_recompute_due(frame, slot, K).
//   lazy creation (0x524c10): Recompute immediately, next stays 0 (constructor value), so the
//                        first service call recomputes again.
//   service (0x526590):  if (recompute_due(frame, next)) { next = frame + 90; Recompute; }
// Results must be served on exactly those frames even when the work runs on a worker thread.
#pragma once
#include <cstdint>

namespace alamo::galactic {

inline constexpr int32_t kAllPairsRecomputePeriod = 90;  // 0x5a frames
inline constexpr int32_t kLazyCreatedNextRecompute = 0;

// K = number of player slots - number of active human players (+0x62 and +0x180 set).
constexpr int32_t stagger_divisor(int32_t player_slots, int32_t active_humans) {
    return player_slots - active_humans;
}

// `slot` is PlayerClass +0x48, the first saved PlayerClass field. OPEN ITEM (RE): its meaning is
// unresolved (probably a slot ordinal). Float math as the original:
// frame + int(float(slot) * (90.0f / float(K)) + 0.5f), truncating (round half up for >= 0).
inline int32_t first_recompute_due(int32_t frame, int32_t slot, int32_t k) {
    if (k < 2) return frame + kAllPairsRecomputePeriod;
    const float step = static_cast<float>(kAllPairsRecomputePeriod) / static_cast<float>(k);
    const float off = static_cast<float>(slot) * step + 0.5f;
    return frame + static_cast<int32_t>(off);
}

constexpr bool recompute_due(int32_t frame, int32_t next) { return next <= frame; }
constexpr int32_t next_after_service(int32_t frame) { return frame + kAllPairsRecomputePeriod; }

}  // namespace alamo::galactic
