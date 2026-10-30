#include "alamo/jobs/tick.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/log.hpp"

namespace alamo::jobs {

SimClock::SimClock(uint32_t ticks_per_second, uint32_t max_ticks_per_advance)
    : tps_(std::max(ticks_per_second, 1u)), max_per_advance_(std::max(max_ticks_per_advance, 1u)) {}

uint32_t SimClock::advance(double real_seconds) {
    if (!(real_seconds > 0.0)) return 0;  // also rejects NaN
    real_seconds = std::min(real_seconds, 3600.0);
    return advance_ns(static_cast<int64_t>(std::llround(real_seconds * 1e9)));
}

uint32_t SimClock::advance_ns(int64_t real_ns) {
    if (real_ns <= 0) return 0;
    real_ns = std::min<int64_t>(real_ns, int64_t{3600} * kNsPerSecond);
    acc_ += real_ns * static_cast<int64_t>(tps_);
    const int64_t due = acc_ / kNsPerSecond;
    acc_ -= due * kNsPerSecond;
    if (due > static_cast<int64_t>(max_per_advance_)) {
        dropped_ += static_cast<uint64_t>(due - max_per_advance_);
        return max_per_advance_;
    }
    return static_cast<uint32_t>(due);
}

double SimClock::alpha() const noexcept {
    return static_cast<double>(acc_) / static_cast<double>(kNsPerSecond);
}

TickScheduler::TickScheduler(JobSystem& js, SimClock clock) : js_(&js), clock_(clock) {}

TickScheduler::SystemId TickScheduler::add_system(std::string name, uint32_t period, uint32_t phase,
                                                  SystemFn fn) {
    if (period == 0) throw std::invalid_argument("TickScheduler::add_system: period must be >= 1");
    const profile::ZoneId zone = profile::register_zone(("sys." + name).c_str());
    systems_.push_back(System{std::move(name), period, phase % period, true, std::move(fn), zone});
    return static_cast<SystemId>(systems_.size() - 1);
}

void TickScheduler::set_enabled(SystemId id, bool enabled) { systems_.at(id).enabled = enabled; }

void TickScheduler::track(DeferredBase& d) {
    if (std::find(tracked_.begin(), tracked_.end(), &d) == tracked_.end()) tracked_.push_back(&d);
}

void TickScheduler::untrack(DeferredBase& d) {
    tracked_.erase(std::remove(tracked_.begin(), tracked_.end(), &d), tracked_.end());
}

uint32_t TickScheduler::advance(double real_seconds) {
    const uint64_t dropped_before = clock_.dropped_ticks();
    const uint32_t n = clock_.advance(real_seconds);
    if (clock_.dropped_ticks() != dropped_before)
        log::warnf("jobs", "sim falling behind: dropped %llu tick(s) at frame %llu",
                   static_cast<unsigned long long>(clock_.dropped_ticks() - dropped_before),
                   static_cast<unsigned long long>(frame_));
    for (uint32_t i = 0; i < n; ++i) step();
    return n;
}

void TickScheduler::step() {
    const TickContext ctx{frame_, clock_.fixed_dt(), *js_};
    profile::begin_tick(frame_);
    for (DeferredBase* d : tracked_) d->sync(frame_);
    for (const System& s : systems_) {
        if (!s.enabled || frame_ % s.period != s.phase) continue;
        profile::Scope timer(s.zone);
        s.fn(ctx);
    }
    ++frame_;
    profile::end_tick();
}

}  // namespace alamo::jobs
