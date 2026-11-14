#include "alamo/jobs/tick.hpp"

#include <algorithm>

namespace alamo::jobs {

TickScheduler::TickScheduler(JobSystem& js, uint32_t ticks_per_second)
    : js_(&js), tps_(std::max(ticks_per_second, 1u)) {}

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

void TickScheduler::step() {
    const TickContext ctx{frame_, fixed_dt(), *js_};
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
