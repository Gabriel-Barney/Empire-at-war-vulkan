// Thread pool with per-thread work-stealing deques, helping waits, and parallel_for.
//
// Threads and slots
//   - N worker threads (slots 1..N). Slot 0 is shared by every non-worker thread (the sim/main
//     thread, tools). N == 0 is fully serial mode: nothing runs on another thread, jobs are run
//     by whoever waits on them, parallel_for runs inline chunk by chunk in index order.
//   - Each slot owns a mutex-protected deque: the owner pushes/pops at the back (LIFO, cache
//     warm), thieves take from the front (FIFO, oldest/biggest work). Jobs are a small POD
//     (function pointer + context + two u32 + counter + priority), so submitting never
//     heap-allocates except the deque's own block growth. spawn() allocates once (the closure).
//   - Background priority: one shared FIFO that idle workers take from. Priority is inherited:
//     anything submitted (incl. parallel_for helpers, spawn) from inside a
//     background job of this system is background too. A thread that is NOT inside a
//     background job never runs somebody else's background work while it waits, so a short
//     parallel_for / Deferred::sync on the sim thread cannot get stuck running
//     chunks of a long AI recompute. wait(c) does run c's *own* background jobs if they have
//     not started yet (also if they are queued after the waiter went to sleep), which makes
//     background work progress in serial mode. A thread inside a background job may run any
//     background job while it waits (needed so nested background work cannot deadlock).
//     Limitation: once a background job has started on a worker, a foreground thread waiting
//     on it does not help with that job's nested (background) chunks.
//
// Waiting helps: wait() executes queued foreground jobs while the condition holds,
// so nested parallel_for / wait inside a job cannot deadlock (as long as the awaited work is
// in the pool, which it always is for parallel_for).
//
// Exceptions thrown by a job are captured in its JobCounter and rethrown by wait() (the one
// with the lowest key - job argument `a` / chunk index - wins, so the reported error does not
// depend on timing when several jobs fail). parallel_for rethrows the exception of the lowest
// failing chunk, exactly what the serial loop would have thrown first.
//
// Every job and every parallel_for chunk runs inside a ScratchScope of the executing thread's
// scratch arena (see scratch.hpp): scratch allocations made inside are released afterwards.
//
// DEADLOCK RULE: never hold a (non-recursive) mutex across a nested parallel_for, wait(),
// or Deferred::sync. Waits *help*: the waiting thread may run any
// other queued job, and if that job takes the same mutex the thread deadlocks on itself.
// Take locks inside the chunk body, or collect results per index and commit serially.
//
// Exception safety: submit/spawn/parallel_for are strongly exception-safe against allocation
// failure (a failed submit leaves nothing queued and the counter unchanged; parallel_for falls
// back to running every chunk on the caller if its helpers cannot be queued).
//
// Shutdown: the destructor lets workers drain every queued job (foreground and background),
// then joins. Submitting from a non-worker thread while the destructor runs is undefined.
// Counters referenced by still-queued jobs must outlive the JobSystem.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/jobs/function_ref.hpp"

namespace alamo::jobs {

using JobFn = void (*)(void* ctx, uint32_t a, uint32_t b);
using RangeRef = FunctionRef<void(uint32_t, uint32_t)>;

enum class Priority : uint8_t { Normal, Background };

struct JobSystemConfig {
    int32_t workers = -1;  // < 0: default_worker_count(); 0: fully serial (debug / repro)
};

// Completion counter for a group of jobs ("wait group"). Reusable after wait() returns.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    bool done() const noexcept { return pending_.load(std::memory_order_acquire) == 0; }
    uint32_t pending() const noexcept { return pending_.load(std::memory_order_acquire); }

private:
    friend class JobSystem;
    void capture(uint64_t key, std::exception_ptr e);
    void rethrow_if_failed();

    std::atomic<uint32_t> pending_{0};
    std::mutex error_mutex_;
    std::exception_ptr error_;
    uint64_t error_key_ = 0;
};

class JobSystem {
public:
    explicit JobSystem(JobSystemConfig config = {});
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // hardware_concurrency() - 1, at least 1.
    static uint32_t default_worker_count();

    uint32_t worker_count() const noexcept { return workers_; }
    uint32_t thread_count() const noexcept { return workers_ + 1; }  // workers + calling thread
    bool serial() const noexcept { return workers_ == 0; }
    // 0 for non-worker threads, 1..N on this system's workers.
    uint32_t current_slot() const noexcept;
    // Jobs executed per slot since construction (diagnostics / tests).
    std::vector<uint64_t> executed_per_slot() const;

    // Low-level submission: fn(ctx, a, b) runs once on some thread. `a` doubles as the
    // exception-ordering key. Never allocates beyond deque growth.
    void submit(JobCounter& counter, JobFn fn, void* ctx, uint32_t a = 0, uint32_t b = 0,
                Priority priority = Priority::Normal);

    // Run an arbitrary closure as a job (one heap allocation for the closure).
    template <class F>
    void spawn(JobCounter& counter, F&& f, Priority priority = Priority::Normal) {
        using Fn = std::decay_t<F>;
        auto heap = std::make_unique<Fn>(std::forward<F>(f));
        submit(
            counter,
            [](void* ctx, uint32_t, uint32_t) {
                std::unique_ptr<Fn> owned(static_cast<Fn*>(ctx));
                (*owned)();
            },
            heap.get(), 0, 0, priority);
        (void)heap.release();  // owned by the queued job now (submit is strongly exception-safe)
    }

    // Block until every job of `counter` finished, executing other work meanwhile. Rethrows the
    // captured exception (if any) and clears it.
    void wait(JobCounter& counter);

    // fn(begin, end) over [0, count) in chunks of `grain` (0 = automatic). Chunk boundaries are
    // multiples of grain. The calling thread participates. Returns after every chunk finished.
    // Satisfies alamo::RangeFn's contract (see core/parallel.hpp).
    template <class F>
    void parallel_for(uint32_t count, uint32_t grain, F&& fn) {
        parallel_for_ref(count, grain, RangeRef(fn));
    }
    void parallel_for_ref(uint32_t count, uint32_t grain, RangeRef fn);

private:
    struct Job {
        JobFn fn = nullptr;
        void* ctx = nullptr;
        uint32_t a = 0;
        uint32_t b = 0;
        JobCounter* counter = nullptr;
        Priority priority = Priority::Normal;
    };
    struct Slot;

    uint32_t my_slot() const noexcept;
    Priority effective_priority(Priority requested) const noexcept;
    void push(const Job& job, uint32_t copies);
    void wake(uint32_t copies);
    bool pop_foreground(uint32_t self, Job& out);
    bool pop_background(Job& out);
    bool claim_background(const JobCounter& counter, Job& out);
    void execute(const Job& job, uint32_t self);
    // Execute queued jobs (foreground; plus background when called from inside a background
    // job) while keep_waiting() is true; sleeps when there is nothing to do. A sleeping helper
    // is woken by job pushes and by any JobCounter reaching zero (notify_waiters()). keep_waiting
    // may be called under an internal lock: keep it cheap and non-blocking.
    void help_loop(FunctionRef<bool()> keep_waiting, JobCounter* claim);
    // Wake threads sleeping in wait() so they re-check their condition.
    void notify_waiters();
    void worker_main(uint32_t slot);

    uint32_t workers_ = 0;
    std::vector<std::unique_ptr<Slot>> slots_;  // size workers_ + 1
    std::mutex bg_mutex_;
    std::deque<Job> bg_queue_;
    std::atomic<int64_t> fg_queued_{0};
    std::atomic<int64_t> bg_queued_{0};
    std::atomic<uint64_t> bg_epoch_{0};  // incremented on every background push

    std::mutex sleep_mutex_;
    std::condition_variable work_cv_;  // idle workers
    std::condition_variable done_cv_;  // threads blocked in wait()
    std::atomic<uint32_t> idle_workers_{0};
    std::atomic<uint32_t> waiters_{0};
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};

// Backs core's ParallelFor hook with `js` (which must outlive the returned function).
inline alamo::ParallelFor make_parallel_for(JobSystem& js) {
    return [&js](uint32_t count, uint32_t grain, const alamo::RangeFn& fn) {
        js.parallel_for(count, grain, fn);
    };
}

// OPT2 t02: backs the AI's fixed-latency background hook (alamo/ai/latency_job.hpp BackgroundStart) with `js` (which must
// outlive it): `work` runs as one Background-priority job and the returned function blocks until it finished, running
// other work meanwhile. The returned function must be called once before the job's data goes away (LatencyJob always
// does). A serial system runs `work` inline and returns an empty function. The returned function may outlive `js` (an AI
// world that outlives its host): `js`'s destructor runs every queued job, so a finished counter never touches `js`.
inline std::function<std::function<void()>(std::function<void()>)> make_background_start(JobSystem& js) {
    return [&js](std::function<void()> work) -> std::function<void()> {
        if (js.serial()) {
            work();
            return {};
        }
        auto counter = std::make_shared<JobCounter>();
        js.spawn(*counter, std::move(work), Priority::Background);
        return [&js, counter] {
            if (!counter->done()) js.wait(*counter);
        };
    };
}

}  // namespace alamo::jobs
