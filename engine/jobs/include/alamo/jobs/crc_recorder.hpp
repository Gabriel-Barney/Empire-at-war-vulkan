// Per-tick CRC recording (Q1). Plugs core's CrcRegistry (sim_crc.hpp) into the fixed-step tick:
// capture(tick) hashes every registered system's state for that frame into a CrcLog.
//
//   alamo::CrcRegistry reg;
//   reg.add("economy", [&](alamo::StateHasher& h) { h.u32(n); h.span(credits); });
//   alamo::jobs::CrcRecorder rec(js, reg);
//   for (...) { const uint64_t f = sched.frame(); sched.step(); rec.capture(f); }
//   rec.log().save("run.crc");
//
// Determinism checks run the same sim for every count in determinism_worker_counts() and compare
// the logs with alamo::first_divergence().
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/jobs/job_system.hpp"

namespace alamo::jobs {

class CrcRecorder {
public:
    // parallel_capture: run the hashers on the job system (one job per system; result identical).
    CrcRecorder(JobSystem& js, const CrcRegistry& reg, bool parallel_capture = false)
        : js_(&js), reg_(&reg), parallel_(parallel_capture) {
        log_.systems = reg.names();
        log_.parity_parts = reg.parity_names();
    }

    // Records the current state under `tick` (after the tick ran, or the initial state). Ticks must
    // increase across records. Systems registered after construction are picked up here.
    const CrcRecord& capture(uint64_t tick) {
        if (log_.systems.size() != reg_->size()) log_.systems = reg_->names();
        log_.parity_parts = reg_->parity_names();
        if (parallel_) log_.records.push_back(reg_->capture(tick, make_parallel_for(*js_)));
        else log_.records.push_back(reg_->capture(tick));
        return log_.records.back();
    }

    CrcLog& log() noexcept { return log_; }
    const CrcLog& log() const noexcept { return log_; }

private:
    JobSystem* js_;
    const CrcRegistry* reg_;
    bool parallel_;
    CrcLog log_;
};

// Worker counts every determinism check covers: 0 (serial), 1, 4 and the machine's default
// (hardware threads - 1), de-duplicated, in that order.
inline std::vector<int32_t> determinism_worker_counts() {
    std::vector<int32_t> w = {0, 1, 4};
    const auto n = static_cast<int32_t>(JobSystem::default_worker_count());
    if (std::find(w.begin(), w.end(), n) == w.end()) w.push_back(n);
    return w;
}

// Worker-count check (OPT3 t07), in-process form for unit tests and short runs: runs `run(workers)`
// (returns the run's CrcLog) for every count in order and compares each log with the first count's.
// Long runs use the per-process form instead, one ctest entry per count: alamo_add_worker_count_check()
// in engine/CMakeLists.txt (docs/BUILD.md "Worker-count check").
//
//   const auto rep = jobs::check_worker_counts([&](int32_t w) { return run_my_sim(w); });
//   for (const auto& r : rep.runs) std::printf("  workers %d: %s\n", r.workers, r.vs_first.message.c_str());
//   CHECK(rep.ok());
struct WorkerCountRun {
    int32_t workers = 0;
    CrcDivergence vs_first;  // against the first count's log (not diverged for the first count itself)
};
struct WorkerCountReport {
    CrcLog first;  // the first count's log (for --crc-out / golden compares)
    std::vector<WorkerCountRun> runs;
    bool ok() const noexcept {
        return std::none_of(runs.begin(), runs.end(), [](const WorkerCountRun& r) { return r.vs_first.diverged; });
    }
};
template <class Run>
WorkerCountReport check_worker_counts(Run&& run, const std::vector<int32_t>& counts = determinism_worker_counts()) {
    WorkerCountReport rep;
    for (const int32_t w : counts) {
        WorkerCountRun r;
        r.workers = w;
        if (rep.runs.empty()) {
            rep.first = run(w);
            r.vs_first.message = "reference";
        } else {
            r.vs_first = first_divergence(rep.first, run(w));
        }
        rep.runs.push_back(std::move(r));
    }
    return rep;
}

}  // namespace alamo::jobs
