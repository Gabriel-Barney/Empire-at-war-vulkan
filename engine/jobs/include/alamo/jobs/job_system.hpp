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
//   - Background cap (frame-time scaling, 2026-11-14): an idle worker starts a new top-level
//     background job only while fewer than background_cap() of them run (half the workers, at
//     least 1), so long background work (shader compiles, AI recomputes) never occupies every
//     worker a foreground parallel_for could use. Exempt from the cap, so it cannot deadlock or
//     serialise background work: the helpers of a background parallel_for (queued at the FRONT of
//     the background queue, any idle worker takes them), a background job's own help_loop, a
//     waiter claiming its own job, and the shutdown drain.
//
// Three priorities (SIMP sp1, design 4.1): Critical (= Normal: the tick's phase jobs) > Ai > Background, strict.
//   - Critical jobs go to the per-slot deques above; Ai and Background jobs each to one shared FIFO (the helpers of a
//     parallel_for at that priority jump to its front). A worker always takes critical work first, then AI, then
//     background (capped). A worker running chunks of an Ai or Background parallel_for runs any queued critical job
//     between two chunks, so AI and background work only fill gaps in the tick.
//   - Priority is inherited like background always was: anything submitted from inside an Ai job (or a thread inside a
//     ScopedPriority) is at least Ai. A thread waits by helping with work of its own priority or higher: a critical
//     thread never runs AI or background jobs, an AI thread runs critical and AI jobs, a background thread any.
//
// Small forks run inline (sp1): every parallel_for call site (its RangeRef target, or for make_parallel_for the
// std::function's target type) keeps an estimate of its work per item, measured on the wall clock (inline: the loop's
// time; forked: the loop's time x the threads that ran chunks). A loop estimated below kInlineWorkNs runs on the caller
// alone, and a larger one asks for at most one thread per kMinThreadWorkNs of work. Results never depend on this
// (core/parallel.hpp: results must not depend on how ranges were split); only where and when chunks run changes.
//
// Waits (sp1, J2): a waiting thread spins with _mm_pause for a few microseconds, then sleeps with std::atomic::wait on
// an epoch that pushes and completions bump (no std::this_thread::yield, no mutex / condition variable). Idle workers
// spin a little longer before they sleep, so back-to-back forks of one tick find them awake.
//
// Pool (sp1, alamo/jobs/topology.hpp): the default worker count is one per physical core of the performance cores of
// one last-level-cache domain, minus the sim thread and the AI thread; workers are kept on those cores (one logical
// processor per core, SMT siblings left to the OS) when the pool fits (JobSystemConfig::cpu_sets).
//
// parallel_for waits for chunks, not helpers (2026-11-14): the caller returns as soon as every
// chunk is claimed and no thread is still running one. Helper jobs still queued are taken back
// out of the caller's own slot; a helper a worker popped but has not started yet (the OS has not
// scheduled that thread) no longer holds the caller. The loop state is reference counted, so
// such a late helper finds no chunk left and exits without touching the caller's frame.
// Only as many idle workers are woken as helpers were queued (notify_all only when that is all of them).
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

// Strict order: Critical (Normal) > Ai > Background. Normal is the old name of Critical.
enum class Priority : uint8_t { Critical = 0, Normal = 0, Ai = 1, Background = 2 };

struct JobSystemConfig {
    int32_t workers = -1;  // < 0: default_worker_count(); 0: fully serial (debug / repro)
    // sp1: keep the workers on the pool's cores (one logical processor per physical core) when workers + the sim and AI
    // threads fit on them. Wall clock only. ALAMO_JOBS_CPUSETS=0 in the environment turns it off as well.
    bool cpu_sets = true;
    // sp1: run small parallel_for loops inline (see the header comment). Off: every loop forks (tests of the fork path).
    bool inline_small = true;
};

// sp1 tuning (wall clock only; nothing the sim reads depends on them).
inline constexpr int64_t kInlineWorkNs = 50'000;     // estimated loop work below this runs on the caller alone
inline constexpr int64_t kMinThreadWorkNs = 25'000;  // at most one thread per this much estimated work

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

    // sp1: physical cores of the pool's cache domain (topology.hpp) minus the sim and AI threads, at least 1; without
    // topology information hardware_concurrency() - 1, at least 1.
    static uint32_t default_worker_count();

    uint32_t worker_count() const noexcept { return workers_; }
    // Most top-level background jobs that run on workers at once: max(1, workers / 2); 0 when serial.
    uint32_t background_cap() const noexcept { return bg_cap_; }
    uint32_t thread_count() const noexcept { return workers_ + 1; }  // workers + calling thread
    bool serial() const noexcept { return workers_ == 0; }
    // 0 for non-worker threads, 1..N on this system's workers.
    uint32_t current_slot() const noexcept;
    // sp1: whether the workers were kept on the pool's cores (JobSystemConfig::cpu_sets and the pool fits).
    bool pinned() const noexcept { return pinned_; }
    // sp1 diagnostics / tests: parallel_for calls that ran inline because their estimated work was small.
    uint64_t inline_forks() const noexcept { return inline_forks_.load(std::memory_order_relaxed); }
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
        const RangeRef ref(fn);
        parallel_for_ref(count, grain, ref, ref.target_id());
    }
    // `site` keys the call site's work estimate (sp1 inline forks); 0 = fn's target.
    void parallel_for_ref(uint32_t count, uint32_t grain, RangeRef fn, uintptr_t site = 0);

private:
    struct Job {
        JobFn fn = nullptr;
        void* ctx = nullptr;
        uint32_t a = 0;
        uint32_t b = 0;
        JobCounter* counter = nullptr;  // nullptr: detached (parallel_for helpers; nobody waits on the job itself)
        Priority priority = Priority::Normal;
    };
    struct Slot;
    struct ForState;
    struct SiteCost;
    // One shared FIFO per non-critical priority. parallel_for helpers at that priority are queued at the front, so the
    // queue front is a helper exactly when helpers > 0.
    struct SharedQueue {
        std::mutex mutex;
        std::deque<Job> queue;
        std::atomic<int64_t> queued{0};
        std::atomic<int64_t> helpers{0};
        std::atomic<uint64_t> epoch{0};  // incremented on every push
    };
    SharedQueue& shared(Priority p) noexcept { return p == Priority::Ai ? ai_ : bg_; }

    uint32_t my_slot() const noexcept;
    Priority effective_priority(Priority requested) const noexcept;
    void push(const Job& job, uint32_t copies);
    void wake(uint32_t copies);
    bool pop_foreground(uint32_t self, Job& out);
    bool pop_shared(SharedQueue& q, Job& out);
    bool pop_background(Job& out) { return pop_shared(bg_, out); }
    // Worker top level: pop the front background job; a non-helper only below the cap, and then
    // `counted` is true and bg_running_ counts it until finish_background_capped().
    bool pop_background_capped(Job& out, bool& counted);
    void finish_background_capped();
    bool background_startable() const noexcept;
    // parallel_for internals (job_system.cpp).
    void run_for_chunks(ForState& st);
    void release_for_chunk(ForState& st);
    uint32_t cancel_queued_helpers(uint32_t self, const ForState& st);
    static void for_helper_job(void* ctx, uint32_t, uint32_t);
    // Loop states are pooled (no heap allocation per parallel_for once the pool is warm).
    ForState* acquire_for_state();
    void release_for_ref(ForState* st, uint32_t refs = 1);
    bool claim_shared(SharedQueue& q, const JobCounter& counter, Job& out);
    void execute(const Job& job, uint32_t self);
    // Execute queued jobs (critical; plus AI / background when the thread's priority allows) while keep_waiting() is
    // true; spins, then sleeps when there is nothing to do. A sleeping helper is woken by job pushes and by any
    // JobCounter reaching zero (notify_waiters()). keep_waiting must be cheap and non-blocking.
    void help_loop(FunctionRef<bool()> keep_waiting, JobCounter* claim);
    // sp1: run queued critical jobs (between two chunks of a non-critical loop).
    void run_critical(uint32_t self);
    // sp1 inline forks: the call site's work estimate.
    SiteCost& site_cost(uintptr_t site) noexcept;
    // Wake threads sleeping in wait() so they re-check their condition.
    void notify_waiters();
    void worker_main(uint32_t slot);

    uint32_t workers_ = 0;
    uint32_t bg_cap_ = 0;
    bool pinned_ = false;
    bool inline_small_ = true;
    std::atomic<uint32_t> bg_running_{0};       // top-level background jobs running on workers
    std::vector<std::unique_ptr<Slot>> slots_;  // size workers_ + 1
    std::mutex for_pool_mutex_;
    std::vector<std::unique_ptr<ForState>> for_pool_;  // every loop state ever made (owner)
    std::vector<ForState*> for_free_;                  // the idle ones; capacity kept >= for_pool_.size()
    SharedQueue ai_;
    SharedQueue bg_;
    std::atomic<int64_t> fg_queued_{0};
    std::unique_ptr<SiteCost[]> sites_;  // sp1: direct-mapped call-site work estimates
    std::atomic<uint64_t> inline_forks_{0};

    // Sleeping (sp1, J2): idle workers wait on work_epoch_, threads blocked in wait() on done_epoch_ (std::atomic::wait).
    std::atomic<uint32_t> work_epoch_{0};
    std::atomic<uint32_t> done_epoch_{0};
    std::atomic<uint32_t> idle_workers_{0};
    std::atomic<uint32_t> waiters_{0};
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};

// sp1: the calling thread's priority for jobs of `js` it submits (and the work it helps with while it waits) is at least
// `p` until the scope ends: the AI lane's thread runs inside ScopedPriority(js, Priority::Ai). Scopes nest.
class ScopedPriority {
public:
    ScopedPriority(const JobSystem& js, Priority p) noexcept;
    ~ScopedPriority();
    ScopedPriority(const ScopedPriority&) = delete;
    ScopedPriority& operator=(const ScopedPriority&) = delete;

private:
    const JobSystem* prev_of_;
    Priority prev_prio_;
};

// Backs core's ParallelFor hook with `js` (which must outlive the returned function). The call site of a loop is the
// std::function's target type (sp1 inline forks: one work estimate per lambda).
inline alamo::ParallelFor make_parallel_for(JobSystem& js) {
    return [&js](uint32_t count, uint32_t grain, const alamo::RangeFn& fn) {
        js.parallel_for_ref(count, grain, RangeRef(fn), reinterpret_cast<uintptr_t>(&fn.target_type()));
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
