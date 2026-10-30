// AI framework: AILearningSystemClass (docs/re/AI_GALACTIC.md §1.11).
//
// Lifetime attempt/success counters keyed by (goal type, target), goal type and plan; recent records
// {success, expiry} keyed by (plan, target) (+0x148), (goal type, target) (+0x188) and activations
// (goal type, target) (+0x1c8). Expiry = NOW + (int)(FPS x duration), or -1 (permanent) for a negative
// duration; a zero duration records no recent entry [H]. Queries count recent FAILURES whose expiry is
// -1 or later than NOW [port: the "live" test is not stated; expiry > NOW]. Service (every 10 s) prunes
// each recent list up to its first live record [M].
//
// Determinism: the original keys these maps by pointer; the port keys them by stable ids (goal type
// index, target index, plan index) in std::map, so every walk (pruning, hashing) is id-ordered [M].
#pragma once
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace alamo {
class ByteReader;
class ByteWriter;
class StateHasher;
}

namespace alamo::ai {

class LearningSystem {
public:
    struct Record {
        bool success = false;
        int64_t expiry = -1;
    };
    struct Counter {
        uint32_t attempts = 0, successes = 0;
    };
    using Key = std::pair<uint32_t, int32_t>;  // (goal type or plan, target)

    // Plan end (0x58a720): outcome of a goal served by `plan` (kAiNone-like 0xFFFFFFFF = none).
    void record_goal_outcome(uint32_t goal_type, int32_t target, uint32_t plan, bool success, float tracking_duration,
                             int64_t now);
    // Resolve (0x58a5d0): activation outcome.
    void record_activation(uint32_t goal_type, int32_t target, bool success, float activation_tracking_duration,
                           int64_t now);

    int32_t recent_goal_failures(uint32_t goal_type, int32_t target, int64_t now) const;        // 0x588870
    int32_t recent_activation_failures(uint32_t goal_type, int32_t target, int64_t now) const;  // 0x588760
    int32_t recent_plan_failures(uint32_t plan, int32_t target, int64_t now) const;             // 0x588980
    double goal_type_success_ratio(uint32_t goal_type) const;  // port: per goal type (1.0 without attempts)
    double plan_success_ratio(uint32_t plan) const;            // 0x588b60 (1.0 without attempts)
    // Per object-type survival (STRATEGIC_GAPS §1.1) [H]: writer 0x58a9a0 (attempts += 1, successes += survived), keyed
    // by the type-name CRC; 0x588a90 = successes / attempts, 1.0 when the type was never recorded. Lookups only.
    void record_survival(uint32_t type_name_crc, bool survived);
    double survival_ratio(uint32_t type_name_crc) const;
    size_t survival_entries() const { return survival_.size(); }

    void prune(int64_t now);  // Service
    void hash(StateHasher& h) const;
    void write(ByteWriter& w) const;  // N1 save / load: every table, key order
    bool read(ByteReader& r);

    const std::map<Key, std::vector<Record>>& recent_goals() const { return recent_goal_; }

private:
    static int64_t expiry_of(float duration, int64_t now);
    static int32_t failures(const std::map<Key, std::vector<Record>>& m, Key k, int64_t now);
    std::map<Key, std::vector<Record>> recent_goal_, recent_activation_, recent_plan_;
    std::map<Key, Counter> type_target_;
    std::map<uint32_t, Counter> type_, plan_;
    std::map<uint32_t, Counter> survival_;  // type-name CRC -> {attempts, successes} (hashed only when non-empty)
};

}  // namespace alamo::ai
