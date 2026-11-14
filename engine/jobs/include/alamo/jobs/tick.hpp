// Deterministic fixed-step simulation tick.
//
// TickScheduler- frame counter + systems with (period, phase) cadence, run in registration order
//                on the calling (sim) thread. A system may fan out internally (parallel_for).
//                Tracked Deferred results are published at the start of a tick, before any
//                system runs.
// Deferred<T>  - kick a background computation at frame T whose result becomes visible exactly
//                at frame T+k: never earlier (even if it finished early), blocking at T+k if it is
//                late. Double-buffered: readers before T+k keep seeing the previous result.
//                This is how the per-AI 90-frame all-pairs recompute (GALACTIC.md §4) runs off the
//                sim thread while staying lockstep-safe.
//
// Sim rate: kDefaultTicksPerSecond = 30 logic frames/s for galactic and tactical, confirmed by RE
// (docs/re/PHASE2_ANSWERS.md Q12; single-player fast mode runs 120). All cadences here are in
// frames, so only the tick rate changes between modes.
#pragma once
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "alamo/core/profile.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/jobs/scratch.hpp"

namespace alamo::jobs {

inline constexpr uint32_t kDefaultTicksPerSecond = 30;  // confirmed: docs/re/PHASE2_ANSWERS.md Q12

class DeferredBase {
public:
    virtual ~DeferredBase() = default;
    // Publish the pending result if `frame` reached its ready frame (blocking if needed).
    virtual void sync(uint64_t frame) = 0;
};

// Single-owner object: kick/sync/read are called from the sim thread only. The compute function
// runs on a worker and must only use inputs captured (by value / snapshot) at kick time. It is
// either `T f()` or `void f(T& out)`; the latter reuses the back buffer (e.g. an N*N matrix),
// which holds the result from two kicks ago and must be fully overwritten.
template <class T>
class Deferred final : public DeferredBase {
public:
    explicit Deferred(JobSystem& js, T initial = T{})
        : js_(&js), front_(std::move(initial)), back_() {}
    ~Deferred() override {
        if (in_flight_) {
            try {
                js_->wait(counter_);
            } catch (...) {
            }
        }
    }
    Deferred(const Deferred&) = delete;
    Deferred& operator=(const Deferred&) = delete;

    // Start computing at `frame`; the result is published at frame + latency. If a previous
    // computation is pending and frame >= its ready frame it is published first; kicking while
    // the previous result is not yet due throws std::logic_error (results are never dropped or
    // published early).
    template <class F>
    void kick(uint64_t frame, uint32_t latency, F&& compute) {
        if (in_flight_) {
            if (frame < ready_frame_)
                throw std::logic_error("Deferred::kick: previous result not yet published");
            sync(frame);
        }
        // Strong guarantee: if allocation or submit throws, nothing is in flight.
        task_ = std::make_unique<Task<std::decay_t<F>>>(std::forward<F>(compute));
        if (js_->serial()) {
            run_job(this, 0, 0);  // serial mode: as if a worker finished instantly
        } else {
            try {
                js_->submit(counter_, &run_job, this, 0, 0, Priority::Background);
            } catch (...) {
                task_.reset();
                throw;
            }
        }
        kick_frame_ = frame;
        ready_frame_ = frame + latency;
        in_flight_ = true;
    }

    void sync(uint64_t frame) override {
        if (!in_flight_ || frame < ready_frame_) return;
        js_->wait(counter_);
        in_flight_ = false;
        task_.reset();
        if (error_) std::rethrow_exception(std::exchange(error_, nullptr));  // front unchanged
        using std::swap;
        swap(front_, back_);
        ++version_;
    }

    // Result visible at `frame` (publishes first if due).
    const T& read(uint64_t frame) {
        sync(frame);
        return front_;
    }
    // Currently published result, without publishing.
    const T& current() const noexcept { return front_; }

    bool in_flight() const noexcept { return in_flight_; }
    bool computed() const noexcept { return !in_flight_ || counter_.done(); }  // diagnostics only
    uint64_t kick_frame() const noexcept { return kick_frame_; }
    uint64_t ready_frame() const noexcept { return ready_frame_; }
    uint64_t version() const noexcept { return version_; }  // number of published results

private:
    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual void run(T& out) = 0;
    };
    template <class F>
    struct Task final : TaskBase {
        explicit Task(F f) : fn(std::move(f)) {}
        void run(T& out) override {
            if constexpr (std::is_invocable_v<F&, T&>) fn(out);
            else out = fn();
        }
        F fn;
    };

    static void run_job(void* ctx, uint32_t, uint32_t) {
        auto* self = static_cast<Deferred*>(ctx);
        try {
            ScratchScope scope;
            self->task_->run(self->back_);
        } catch (...) {
            self->error_ = std::current_exception();
        }
    }

    JobSystem* js_;
    T front_;
    T back_;
    std::unique_ptr<TaskBase> task_;
    std::exception_ptr error_;
    JobCounter counter_;
    uint64_t kick_frame_ = 0;
    uint64_t ready_frame_ = 0;
    uint64_t version_ = 0;
    bool in_flight_ = false;
};

struct TickContext {
    uint64_t frame;
    double dt;  // fixed step in seconds (informational; sim logic should count frames)
    JobSystem& jobs;
};

class TickScheduler {
public:
    using SystemFn = std::function<void(const TickContext&)>;
    using SystemId = uint32_t;

    // Tick rate (clamped to >= 1) only sets TickContext::dt; the sequence of frames never depends on it.
    explicit TickScheduler(JobSystem& js, uint32_t ticks_per_second = kDefaultTicksPerSecond);

    // Runs on frames f with f % period == phase % period. period >= 1 (throws otherwise).
    // Systems due on the same frame run in registration order.
    SystemId add_system(std::string name, uint32_t period, uint32_t phase, SystemFn fn);
    void set_enabled(SystemId id, bool enabled);

    // Deferred results synced (published if due) at the start of every tick, in track order.
    void track(DeferredBase& d);
    void untrack(DeferredBase& d);

    // Run exactly one tick for frame(), then increment frame(). If a system throws, the
    // exception propagates and frame() is not incremented.
    void step();

    uint64_t frame() const noexcept { return frame_; }  // next frame to run == ticks completed
    uint32_t ticks_per_second() const noexcept { return tps_; }
    double fixed_dt() const noexcept { return 1.0 / static_cast<double>(tps_); }
    JobSystem& jobs() noexcept { return *js_; }
    size_t system_count() const noexcept { return systems_.size(); }
    const std::string& system_name(SystemId id) const { return systems_.at(id).name; }

private:
    struct System {
        std::string name;
        uint32_t period;
        uint32_t phase;
        bool enabled;
        SystemFn fn;
        profile::ZoneId zone;  // OPT1: "sys.<name>"
    };
    JobSystem* js_;
    uint32_t tps_;
    uint64_t frame_ = 0;
    std::vector<System> systems_;
    std::vector<DeferredBase*> tracked_;
};

}  // namespace alamo::jobs
