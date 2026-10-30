// Game randomness (G0 part 4 / Q1), matching the original exactly (docs/re/MAIN_LOOP.md §4.1).
//
// The original has two instances of one 32-bit LCG:
//     s <- s * 0x41C64E6D + 0xBDF   (wrapping)          r15 = (s >> 10) & 0x7FFF
//   * the SYNCHRONIZED instance - the only RNG the simulation may use (behaviours, targeting,
//     death, abilities, weather, galactic tick, Lua GameRandom / GameRandom.Get_Float). Seeded once
//     per session (Start_Game); NOT reseeded when a tactical mode is pushed.
//   * the FREE instance - audio, GUI, particles, render effects. Must never influence sim state.
// GameRandom holds both with explicit state: no globals, no clock reads. The host supplies the
// seeds (single player: the original uses a QueryPerformanceCounter value; multiplayer: the
// host's seed; replays/saves: the recorded seed/state).
//
// Rules for sim code:
//   * Draw from synced() only on the sim thread, in the original's draw order (MAIN_LOOP.md §4.2).
//     Parallel phases must not draw.
//   * Never use std::rand, std::random_device, std::mt19937 or std::uniform_*_distribution.
#pragma once
#include <cstdint>

namespace alamo {

class StateHasher;
class ByteWriter;
class ByteReader;

inline constexpr uint32_t kRngFormat = 2;  // 2 = original LCG (0x41C64E6D, 0xBDF), u32 state

class SimRng {
public:
    static constexpr uint32_t kMul = 0x41C64E6Du;
    static constexpr uint32_t kAdd = 0xBDFu;

    explicit SimRng(uint32_t seed = 0) noexcept : s_(seed) {}

    void seed(uint32_t seed_value) noexcept {
        s_ = seed_value;
        draws_ = 0;
    }
    // One LCG step; returns r15 = (s >> 10) & 0x7FFF.
    uint32_t next15() noexcept {
        s_ = s_ * kMul + kAdd;
        ++draws_;
        return (s_ >> 10) & 0x7FFFu;
    }
    // Original float(lo, hi): swap if lo > hi; one step; lo + (hi - lo) * (r15 / 32767.0f).
    float range_float(float lo, float hi) noexcept;
    // Original int(lo, hi), inclusive: swap if needed; lo if equal (no draw); range = hi - lo capped
    // at 0x7FFE; mask = (smallest power of two > range) - 1; step until (r15 & mask) <= range.
    int32_t range_int(int32_t lo, int32_t hi) noexcept;
    // Original percent(p): p == 0 -> false and p == 100 -> true without a draw; otherwise step until
    // ((s >> 10) & 0x7F) <= 100, true if that value < p.
    bool percent(int32_t p) noexcept;
    // Inline copies of the synced generator in the original (docs/re/COMMANDS.md §8). They are draws
    // like the helpers; call these at the matching sites so the draw count stays identical.
    //   0x532480: float in [lo, hi], bounds swapped if needed    0x5324d0: float in [0, 1]
    //   0x623fe0: one step, coin = bit 0 of (s >> 10)
    // PROVISIONAL: the two float forms are assumed to use the helper's r15 / 32767.0f mapping.
    float inline_range_float(float lo, float hi) noexcept { return range_float(lo, hi); }
    float inline_unit_float() noexcept { return static_cast<float>(next15()) / 32767.0f; }
    bool inline_coin() noexcept { return (next15() & 1u) != 0; }

    uint32_t state() const noexcept { return s_; }
    void set_state(uint32_t s) noexcept { s_ = s; }
    uint64_t draws() const noexcept { return draws_; }  // port diagnostic (not in the original)

    void hash(StateHasher& h) const;  // state (u32) + draw count
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);  // false (unchanged) on short/invalid data

    friend bool operator==(const SimRng&, const SimRng&) = default;

private:
    uint32_t s_;
    uint64_t draws_ = 0;
};

// Save/restore guard (retail 0x40aa0 around tactical previews): everything drawn inside the scope is
// undone on exit, so a sim-side "what-if" query consumes nothing.
class SimRngRestore {
public:
    explicit SimRngRestore(SimRng& rng) noexcept : rng_(&rng), saved_(rng) {}
    ~SimRngRestore() { *rng_ = saved_; }
    SimRngRestore(const SimRngRestore&) = delete;
    SimRngRestore& operator=(const SimRngRestore&) = delete;

private:
    SimRng* rng_;
    SimRng saved_;
};

// Both instances of a session.
class GameRandom {
public:
    GameRandom() = default;
    GameRandom(uint32_t synced_seed, uint32_t free_seed) noexcept : synced_(synced_seed), free_(free_seed) {}

    SimRng& synced() noexcept { return synced_; }
    const SimRng& synced() const noexcept { return synced_; }
    SimRng& free() noexcept { return free_; }
    const SimRng& free() const noexcept { return free_; }

    // Start_Game of a new session (empty mode stack): seeds the synchronized instance. Pushing a
    // tactical mode must NOT call this (the stream continues from the galaxy).
    void seed_session(uint32_t seed) noexcept { synced_.seed(seed); }
    // Boot: seeds the free instance.
    void seed_free(uint32_t seed) noexcept { free_.seed(seed); }

    // Lua GameRandom.Free_Random (MAIN_LOOP.md §4.1, [H]): multiplayer -> synchronized RNG for every
    // form; single player: the no-argument form draws int(0, 0xFFFF) from the SYNCHRONIZED state
    // (so it can only return 0..0x7FFE because of the range cap), ranged forms use the free state.
    int32_t lua_free_random([[maybe_unused]] bool multiplayer) noexcept { return synced_.range_int(0, 0xFFFF); }
    int32_t lua_free_random(bool multiplayer, int32_t lo, int32_t hi) noexcept {
        return (multiplayer ? synced_ : free_).range_int(lo, hi);
    }

    // Sync CRC input: the synchronized instance only (the free one is local and may differ).
    void hash(StateHasher& h) const { synced_.hash(h); }
    void write(ByteWriter& w) const;  // both instances (saves)
    bool read(ByteReader& r);

    friend bool operator==(const GameRandom&, const GameRandom&) = default;

private:
    SimRng synced_;
    SimRng free_;
};

}  // namespace alamo
