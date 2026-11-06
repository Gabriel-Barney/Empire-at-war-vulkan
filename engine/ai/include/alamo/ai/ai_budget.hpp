// AI framework: AIBudgetClass (docs/re/AI_GALACTIC_2.md §3; galactic only, goal system +0x190).
//
// Per category (list order of AIGoalCategoryType): +0x38 applied weight (one-tick lag), +0x3C normalised
// weight, +0x44 available A, +0x48 available rate R, +0x4C next pool P, +0x50 income rate I, +0x54 running
// mean of goal horizons, +0x68 the BudgetedGoalStructs (vector order = add order).
// Per goal (0x50 B): +0x28 req, +0x2C alloc, +0x30 horizon h, +0x34 required rate q, +0x38 accrued a,
// +0x3C covered c, +0x40 committed spend s, +0x44 needs-re-add.
// Budget-wide: +0x68 income per second, +0x70 last tick period T, +0x74 reset flag, +0x80 refund accumulator.
//
// Every formula is §3 verbatim, in float, in the documented order [H]:
//   init 0x6137b0, reweight 0x614590 (each commit), tick 0x614340 (each galactic income tick of an AI
//   player), add goal 0x610d30 (first add and re-add), can fund 0x6129c0, time to afford 0x613720, release
//   0x614880, spend 0x6110e0 (Allocate_Resources / build-task charge).
// Flush_Category 0x612cb0 = "available A = 0" and nothing else (AI_GALACTIC_3 §8) [H]; the PAYG reclaim of
// release (0x5f7d40) is computed by the caller (galactic_build.cpp payg_reclaim) and passed in.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace alamo {
class ByteReader;
class ByteWriter;
class StateHasher;
}

namespace alamo::ai {

class AiBudget {
public:
    struct GoalEntry {
        uint32_t goal = 0;  // owner key (goal serial)
        float req = 0.0f, alloc = 0.0f, h = 0.0f, q = 0.0f, a = 0.0f, c = 0.0f, s = 0.0f;
        bool readd = false;
    };
    struct Category {
        float applied = 0.0f;  // +0x38
        float weight = 0.0f;   // +0x3C normalised
        float A = 0.0f;        // +0x44 available
        float R = 0.0f;        // +0x48 available rate
        float P = 0.0f;        // +0x4C next pool
        float I = 0.0f;        // +0x50 income rate
        float mean_h = 0.0f;   // +0x54
        uint32_t mean_n = 0;
        std::vector<GoalEntry> goals;
    };

    void reset(size_t categories);
    size_t size() const { return cat_.size(); }
    bool initialised() const { return init_; }
    // Init 0x6137b0: normalised weights from `raw` (negatives 0), applied = w, A = P = credits x w, R = I = 0.
    void init(std::span<const float> raw, float credits);
    // Reweight 0x614590: `raw` = each category's equation value (the caller pre-loads the previous normalised
    // weight, so a failing equation keeps it). Then A = P, R = I, every goal A -= a, c = 0, needs re-add.
    void reweight(std::span<const float> raw);
    // Per-tick accrual 0x614340 (income = the player's gain this tick, period = T seconds, credits = player +0x70).
    void tick(float income, float period, float credits);
    // Add goal 0x610d30: the first call creates the struct (req = cost, h = min(eta, time_limit unless < 0));
    // later calls only re-add a struct flagged by reweight. Returns false for an invalid category.
    bool add_goal(int32_t c, uint32_t goal, float cost, float eta, float time_limit);
    bool has_goal(int32_t c, uint32_t goal) const { return find(c, goal) != nullptr; }
    // Can fund 0x6129c0. Without a struct: max(max(min(eta, tl), T) x R + A, 0) >= cost (tl < 0 = none).
    bool can_fund(int32_t c, float cost, float eta, float time_limit) const;
    // With the goal's struct: max(max(h, T) x R + A, 0) >= max(req, 0) + alloc - a.
    bool can_fund_goal(int32_t c, uint32_t goal) const;
    // Time to afford 0x613720 (seconds; 0 when affordable now or without a struct).
    float time_to_afford(int32_t c, uint32_t goal, float n) const;
    // Spend: req -= n, alloc += n (Allocate_Resources, build-task charge 0x6110e0).
    void spend(int32_t c, uint32_t goal, float n);
    // Committed spend s += n (production paid, 0x5f9f90) [M].
    void paid(int32_t c, uint32_t goal, float n);
    // Pay-as-you-go installment n (0x53b900, AI_GALACTIC_2 §3.4; q2f t01) [H]. With the goal's struct (an item its build
    // task ordered, 0x64adb0): have = min(alloc, c + a); (int)(have - n) < 0 -> s += n - alloc when alloc < n, returns
    // -1 (the build pauses); else p = min(n, have) is charged (0x615700: P -= p, s and alloc -= p floored at 0, a -= p
    // with a shortfall taken from c) and returned. Without a struct: +0x80 -= n (floor 0), returns n (always paid).
    float installment(int32_t c, uint32_t goal, float n);
    void set_request(int32_t c, uint32_t goal, float req);
    void set_alloc(int32_t c, uint32_t goal, float alloc);
    // Release 0x614880: A += c + a; struct deleted. `reclaimed` = the PAYG reclaim r (A -= r, P -= r, +0x80 += r).
    void release(int32_t c, uint32_t goal, float reclaimed);
    void flush(int32_t c);  // Flush_Category [H]: A = 0
    // Reset flag +0x74 (a byte): the next tick() sets P = Σ x w before the drift redistribution, then clears it
    // (AI_GALACTIC_2 §3.1). Set on a template switch (docs/re/A2G_T01_RESET_FLAG_PROBE.md [H correlation, setter unread]);
    // init() clears it.
    void request_reset() { reset_ = true; }
    bool reset_requested() const { return reset_; }

    float weight(int32_t c) const { return valid(c) ? cat_[static_cast<size_t>(c)].weight : 0.0f; }
    float available(int32_t c) const { return valid(c) ? cat_[static_cast<size_t>(c)].A : 0.0f; }
    float rate(int32_t c) const { return valid(c) ? cat_[static_cast<size_t>(c)].R : 0.0f; }
    float pool(int32_t c) const { return valid(c) ? cat_[static_cast<size_t>(c)].P : 0.0f; }
    float income_rate(int32_t c) const { return valid(c) ? cat_[static_cast<size_t>(c)].I : 0.0f; }  // +0x50
    float income_per_second() const { return rate68_; }
    float period() const { return period_; }
    float pools_total() const;  // Σ P (AI CreditsUnnormalized, 0x6136b0 [M])
    const Category& category(int32_t c) const { return cat_[static_cast<size_t>(c)]; }
    const GoalEntry* goal(int32_t c, uint32_t goal) const { return find(c, goal); }
    void hash(StateHasher& h) const;
    // N1 save / load: every field. read() needs reset() with the same category count.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    bool valid(int32_t c) const { return c >= 0 && static_cast<size_t>(c) < cat_.size(); }
    const GoalEntry* find(int32_t c, uint32_t goal) const;
    GoalEntry* find(int32_t c, uint32_t goal);
    void normalise(std::span<const float> raw);
    void readd(Category& k, GoalEntry& e) const;
    std::vector<Category> cat_;
    float rate68_ = 0.0f, period_ = 0.0f, refund_ = 0.0f;
    bool reset_ = false, init_ = false;
};

}  // namespace alamo::ai
