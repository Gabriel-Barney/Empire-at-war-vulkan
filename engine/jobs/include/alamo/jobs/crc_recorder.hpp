// Per-tick CRC recording for a TickScheduler (Q1). Plugs core's CrcRegistry (sim_crc.hpp) into
// the fixed-step tick: step() runs one tick, then captures every registered system's state hash
// for that frame into a CrcLog.
//
//   alamo::CrcRegistry reg;
//   reg.add("economy", [&](alamo::StateHasher& h) { h.u32(n); h.span(credits); });
//   alamo::jobs::CrcRecorder rec(sched, reg);
//   for (...) rec.step();
//   rec.log().save("run.crc");
//
// Determinism checks run the same sim for every count in determinism_worker_counts() and compare
// the logs with alamo::first_divergence().
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/jobs/tick.hpp"

namespace alamo::jobs {

class CrcRecorder {
public:
    // parallel_capture: run the hashers on the job system (one job per system; result identical).
    CrcRecorder(TickScheduler& sched, const CrcRegistry& reg, bool parallel_capture = false)
        : sched_(&sched), reg_(&reg), parallel_(parallel_capture) {
        log_.systems = reg.names();
        log_.parity_parts = reg.parity_names();
    }

    // Runs tick frame() and records the state after it under that frame number.
    const CrcRecord& step() {
        const uint64_t frame = sched_->frame();
        sched_->step();
        return capture(frame);
    }
    void run(uint64_t ticks) {
        for (uint64_t i = 0; i < ticks; ++i) step();
    }
    // Records the current state under `tick` without stepping (e.g. the initial state). Ticks must
    // increase across records. Systems registered after construction are picked up here.
    const CrcRecord& capture(uint64_t tick) {
        if (log_.systems.size() != reg_->size()) log_.systems = reg_->names();
        log_.parity_parts = reg_->parity_names();
        if (parallel_) log_.records.push_back(reg_->capture(tick, make_parallel_for(sched_->jobs())));
        else log_.records.push_back(reg_->capture(tick));
        return log_.records.back();
    }

    CrcLog& log() noexcept { return log_; }
    const CrcLog& log() const noexcept { return log_; }

private:
    TickScheduler* sched_;
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

}  // namespace alamo::jobs
