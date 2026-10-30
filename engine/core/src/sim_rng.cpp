#include "alamo/core/sim_rng.hpp"

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo {

float SimRng::range_float(float lo, float hi) noexcept {
    if (lo > hi) {
        const float t = lo;
        lo = hi;
        hi = t;
    }
    const float u = static_cast<float>(next15()) / 32767.0f;
    return lo + (hi - lo) * u;
}

int32_t SimRng::range_int(int32_t lo, int32_t hi) noexcept {
    if (lo > hi) {
        const int32_t t = lo;
        lo = hi;
        hi = t;
    }
    if (lo == hi) return lo;
    int64_t range = static_cast<int64_t>(hi) - lo;
    if (range > 0x7FFE) range = 0x7FFE;
    uint32_t mask = 1;
    while (static_cast<int64_t>(mask) <= range) mask <<= 1;  // smallest power of two > range
    mask -= 1;
    uint32_t v = 0;
    do {
        v = next15() & mask;
    } while (static_cast<int64_t>(v) > range);
    return static_cast<int32_t>(static_cast<int64_t>(lo) + v);
}

bool SimRng::percent(int32_t p) noexcept {
    if (p == 0) return false;
    if (p == 100) return true;
    uint32_t v = 0;
    do {
        s_ = s_ * kMul + kAdd;
        ++draws_;
        v = (s_ >> 10) & 0x7Fu;
    } while (v > 100);
    return static_cast<int32_t>(v) < p;
}

void SimRng::hash(StateHasher& h) const {
    h.u32(s_);
    h.u64(draws_);
}

void SimRng::write(ByteWriter& w) const { w.u32(kRngFormat).u32(s_).u64(draws_); }

bool SimRng::read(ByteReader& r) {
    const uint32_t fmt = r.u32();
    const uint32_t s = r.u32();
    const uint64_t d = r.u64();
    if (!r.ok() || fmt != kRngFormat) return false;
    s_ = s;
    draws_ = d;
    return true;
}

void GameRandom::write(ByteWriter& w) const {
    synced_.write(w);
    free_.write(w);
}

bool GameRandom::read(ByteReader& r) {
    SimRng s, f;
    if (!s.read(r) || !f.read(r)) return false;
    synced_ = s;
    free_ = f;
    return true;
}

}  // namespace alamo
