// OPT2 t02 - fixed-latency background jobs for AI work: start on tick N, apply the result on tick N + k.
//
// The pattern (docs/PERFORMANCE.md "AI scheduling"): on the kick tick the sim thread copies everything the computation reads
// (a plain snapshot) and starts the job; the result is applied on the tick the caller fixed at the kick (ready frame),
// never earlier even when it finished early, and the sim thread blocks on that tick when it is late. So what the sim
// sees depends on tick numbers only, never on the worker count or wall-clock time: rule 3 holds for any host.
//
// BackgroundStart is the host's hook (like ParallelFor, alamo/core/parallel.hpp, so the AI modules need no job system):
// it starts `work` off the sim thread and returns a function that blocks until `work` has finished (running other work
// meanwhile). An empty hook, or one that returns an empty function, runs `work` inline at the kick (the serial host and
// the 0-worker job system), which gives the same results: only the thread changes. engine/jobs backs it with
// make_background_start (job_system.hpp).
//
// Save rule (OPT3 t04): a caller keeps the inputs of its in-flight job and writes them with the ready frame; a load
// kicks the job again from those inputs (inline) with the saved ready frame. The computation is a pure function of the
// inputs, so the loaded run applies the same result on the same tick as the uninterrupted one.
#pragma once
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace alamo::ai {

using BackgroundStart = std::function<std::function<void()>(std::function<void()> work)>;

// One background job at a time with a persistent result buffer (the "back" buffer: it keeps the previous result between
// kicks, so a computation may reuse it, e.g. a cache with change detection). Sim thread only. Moving a LatencyJob keeps
// the buffer's address (it is on the heap); assigning to one first waits for its own job.
template <class T>
class LatencyJob {
public:
    LatencyJob() = default;
    ~LatencyJob() { wait_quietly(); }
    LatencyJob(const LatencyJob&) = delete;
    LatencyJob& operator=(const LatencyJob&) = delete;
    LatencyJob(LatencyJob&& o) noexcept
        : s_(std::move(o.s_)), ready_(o.ready_), kick_(o.kick_), in_flight_(std::exchange(o.in_flight_, false)) {}
    LatencyJob& operator=(LatencyJob&& o) noexcept {
        if (this != &o) {
            wait_quietly();
            s_ = std::move(o.s_);
            ready_ = o.ready_;
            kick_ = o.kick_;
            in_flight_ = std::exchange(o.in_flight_, false);
        }
        return *this;
    }

    // Starts compute(T& out) at tick `frame`; its result is applied at `ready_frame` (>= frame). `compute` must only use
    // what it captured by value (and data that stays constant while it runs). Throws std::logic_error when a job is
    // still in flight (the caller applies or cancels it first).
    template <class F>
    void kick(int64_t frame, int64_t ready_frame, const BackgroundStart& bg, F&& compute) {
        if (in_flight_) throw std::logic_error("LatencyJob::kick: a job is in flight");
        if (!s_) s_ = std::make_unique<State>();
        State* s = s_.get();
        s->error = nullptr;
        std::function<void()> work = [s, fn = std::forward<F>(compute)]() mutable {
            try {
                fn(s->out);
            } catch (...) {
                s->error = std::current_exception();
            }
        };
        if (bg) s->wait = bg(std::move(work));
        else work();
        kick_ = frame;
        ready_ = ready_frame;
        in_flight_ = true;
    }

    bool in_flight() const { return in_flight_; }
    bool due(int64_t frame) const { return in_flight_ && frame >= ready_; }
    int64_t kick_frame() const { return kick_; }
    int64_t ready_frame() const { return ready_; }

    // Waits for the job and returns its result buffer (no longer in flight). Rethrows the job's exception (the buffer
    // is then unspecified). The caller applies the result: swap / move it out or read it in place until the next kick.
    T& finish() {
        wait_job();
        in_flight_ = false;
        if (s_->error) std::rethrow_exception(std::exchange(s_->error, nullptr));
        return s_->out;
    }
    // Waits for the job and drops its result.
    void cancel() {
        wait_quietly();
        in_flight_ = false;
    }
    // The result buffer (valid after finish(); the previous result while nothing is in flight).
    T* buffer() { return s_ ? &s_->out : nullptr; }

private:
    struct State {
        T out{};
        std::function<void()> wait;
        std::exception_ptr error;
    };
    void wait_job() {
        if (!in_flight_ || !s_) return;
        if (s_->wait) std::exchange(s_->wait, nullptr)();
    }
    void wait_quietly() noexcept {
        try {
            wait_job();
        } catch (...) {
        }
    }

    std::unique_ptr<State> s_;
    int64_t ready_ = 0;
    int64_t kick_ = 0;
    bool in_flight_ = false;
};

}  // namespace alamo::ai
