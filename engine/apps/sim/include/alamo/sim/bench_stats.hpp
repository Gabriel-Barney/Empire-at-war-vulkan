// BM1 benchmark statistics and result comparison, shared by alamo_headless --bench (sim-only) and the game-side
// benchmark (BM2): tick-time summaries (median / p99 / worst) and the rule for when two results may be compared.
//
// Wall-clock data only: nothing here may feed the sim (determinism rule 3).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alamo::sim::bench {

// Summary of a series of tick (or frame) times in milliseconds.
//   median: the middle value (mean of the two middle values for an even count);
//   p99:    nearest rank, the value at rank ceil(0.99 * n) of the sorted series;
//   worst:  the largest value and its index in the input.
struct TickStats {
    uint64_t count = 0;
    double total_ms = 0, mean_ms = 0, median_ms = 0, p99_ms = 0, worst_ms = 0;
    uint64_t worst_index = 0;
};
TickStats summarize(std::span<const double> ms);
// Nearest-rank percentile (0 < p <= 100) of an unsorted series; 0 for an empty one.
double percentile(std::span<const double> ms, double p);

// What decides whether two benchmark results measured the same work.
struct ResultKey {
    std::string scenario;            // scenario name
    uint32_t version = 0;            // scenario version
    std::string fingerprint_digest;  // the fingerprint file's digest (16 hex)
    uint64_t ticks = 0;              // ticks per run
    bool fingerprint_ok = false;     // every run matched the fingerprint
};
struct Comparison {
    bool comparable = false;
    std::string reason;              // why not (empty when comparable)
    double median_ratio = 0;         // baseline median / this median: > 1 = faster
    double p99_ratio = 0, worst_ratio = 0;
    std::string text;                // one-line summary for the console
};
// Speedup / slowdown of `now` against `base`, or "scenario changed, not comparable" (the reason says what differs).
Comparison compare(const ResultKey& base, const TickStats& base_stats, const ResultKey& now, const TickStats& now_stats);

}  // namespace alamo::sim::bench
