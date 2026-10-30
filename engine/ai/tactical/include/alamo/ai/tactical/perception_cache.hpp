// A1 stage 1b - perception leaf cache (docs/re/AI_TACTICAL_PERCEPTION.md 1.2) [H].
//
// Every leaf node (VisiblePerceptualEvaluatorClass) has a lifetime L in SECONDS (+0x98): -1 forever,
// 0 none. Entries are keyed by the full binding set (here: the evaluator the leaf belongs to, the leaf,
// Self / Target / Enemy / Human / Location and the token parameters, compared exactly) and store the
// value with the frame it was computed on.
//   lookup (only when L != 0): hit iff stamp != 0 and (L == -1 or NOW <= stamp + L * 30).
//                              A value computed on frame 0 is therefore never a hit.
//   store: after every SUCCESSFUL computation; never pruned during the battle.
// [port] stores with L == 0 are skipped: they can never be looked up, so results are identical.
// This is game behaviour, not an optimisation: a cached value keeps being returned while the world
// changes, and the port must cache the same way to take the same decisions.
//
// Thread safety: lookups / stores lock a mutex. Concurrent evaluations within one frame (world not
// mutated) are deterministic: a value stored on frame F is what any other thread would compute on F.
#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::ai::tactical {

inline constexpr int32_t kCacheForever = -1;
inline constexpr int32_t kCacheNone = 0;

struct LeafCacheKey {
    uint8_t eval_kind = 0;  // evaluator class (Game / Player / Unit / Location ...)
    int32_t eval_a = -1;    // evaluator instance (player index, target handle, ...)
    int32_t eval_b = -1;
    uint16_t leaf = 0;      // leaf token
    int32_t self = -1, target = -1, enemy = -1, human = -1, location = -1;
    std::string params;     // canonical parameter text (exact)
    friend auto operator<=>(const LeafCacheKey&, const LeafCacheKey&) = default;
    friend bool operator==(const LeafCacheKey&, const LeafCacheKey&) = default;
};

// The hit rule on its own (tests).
bool leaf_cache_hit(int32_t lifetime_s, uint32_t stamp, uint32_t now);

class LeafCache {
public:
    // True and `out` set when the key has a live entry for `now` under `lifetime_s`.
    bool lookup(const LeafCacheKey& key, int32_t lifetime_s, uint32_t now, double& out) const;
    void store(const LeafCacheKey& key, int32_t lifetime_s, uint32_t now, double value);
    void clear();
    size_t size() const;
    uint64_t hits() const;
    // CRC: entries in key order (key fields, stamp, value bits).
    void hash(StateHasher& h) const;
    // N1 save / load: u32 version (1), u32 count + every entry in key order (key fields, f64 value, u32 stamp), u64
    // hits. read() replaces the cache; false and unchanged on bad data (keys not strictly ascending, truncation).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        double value = 0.0;
        uint32_t stamp = 0;
    };
    mutable std::mutex mu_;
    std::map<LeafCacheKey, Entry> map_;
    mutable uint64_t hits_ = 0;
};

}  // namespace alamo::ai::tactical
