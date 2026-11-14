#include "alamo/jobs/job_system.hpp"

#include <immintrin.h>  // _mm_pause

#include <cstdlib>
#include <cstring>

#include "alamo/core/log.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/jobs/scratch.hpp"
#include "alamo/jobs/topology.hpp"

namespace alamo::jobs {
namespace {

// sp1 (J2): how long a thread spins (with _mm_pause) before it sleeps. A waiter (the sim thread in a fork) spins a few
// microseconds; an idle worker a little longer, so the next fork of the same tick finds it awake. Wall clock only.
constexpr int64_t kWaitSpinNs = 5'000;
constexpr int64_t kWorkerSpinNs = 30'000;
constexpr uint32_t kPausesPerCheck = 16;
constexpr uint32_t kMaxWorkers = 256;
constexpr uint64_t kNoFail = ~uint64_t{0};
constexpr size_t kSites = 256;           // sp1: direct-mapped call-site table
constexpr uint32_t kReservedThreads = 2;  // sp1: the sim thread and the AI thread are not workers
// Placeholder target of an idle pooled loop state's RangeRef (never called).
struct NoopRange {
    void operator()(uint32_t, uint32_t) const noexcept {}
};
constexpr NoopRange kNoopRange{};

struct ThreadState {
    const JobSystem* system = nullptr;
    uint32_t slot = 0;
};
thread_local ThreadState t_state;
// sp1: the thread's priority context: while t_ctx_of is a system, what this thread submits to it is at least
// t_ctx_prio (priority inheritance), and it helps only with work at that priority or higher while it waits.
thread_local const JobSystem* t_ctx_of = nullptr;
thread_local Priority t_ctx_prio = Priority::Critical;
// Nesting depth of execute() on this thread: a job that waits runs other jobs inside itself, and only the outermost one
// counts as busy time (OPT1 profiler).
thread_local uint32_t t_execute_depth = 0;

Priority ctx_priority(const JobSystem* js) noexcept { return t_ctx_of == js ? t_ctx_prio : Priority::Critical; }
Priority lower_of(Priority a, Priority b) noexcept { return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b; }

void pause_burst() noexcept {
    for (uint32_t i = 0; i < kPausesPerCheck; ++i) _mm_pause();
}

bool cpu_sets_allowed() {
    const char* v = std::getenv("ALAMO_JOBS_CPUSETS");
    return !(v && std::strcmp(v, "0") == 0);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// JobCounter

void JobCounter::capture(uint64_t key, std::exception_ptr e) {
    std::lock_guard lk(error_mutex_);
    if (!error_ || key < error_key_) {
        error_ = std::move(e);
        error_key_ = key;
    }
}

void JobCounter::rethrow_if_failed() {
    std::exception_ptr e;
    {
        std::lock_guard lk(error_mutex_);
        e = std::exchange(error_, nullptr);
    }
    if (e) std::rethrow_exception(e);
}

// ---------------------------------------------------------------------------------------------
// ScopedPriority

ScopedPriority::ScopedPriority(const JobSystem& js, Priority p) noexcept : prev_of_(t_ctx_of), prev_prio_(t_ctx_prio) {
    t_ctx_prio = lower_of(ctx_priority(&js), p);
    t_ctx_of = &js;
}

ScopedPriority::~ScopedPriority() {
    t_ctx_of = prev_of_;
    t_ctx_prio = prev_prio_;
}

// ---------------------------------------------------------------------------------------------
// JobSystem

struct JobSystem::Slot {
    std::mutex mutex;
    std::deque<Job> queue;
    std::atomic<uint64_t> executed{0};
};

// sp1: a call site's measured work per item (ns, < 0 unknown). Races only blur an estimate.
struct JobSystem::SiteCost {
    std::atomic<uintptr_t> site{0};
    std::atomic<float> ns_per_item{-1.0f};
};

// parallel_for loop state (pooled; see parallel_for_ref).
struct JobSystem::ForState {
    explicit ForState(JobSystem& s) : js(&s) {}
    // Set up for a new loop; only called by the owner while no job references this state.
    void reset(RangeRef f, uint32_t n, uint32_t g, uint64_t c, uint32_t r, Priority p) {
        fn = f;
        count = n;
        grain = g;
        chunks = c;
        prio = p;
        next.store(0);
        first_failed.store(kNoFail);
        in_flight.store(0);
        participants.store(0);
        refs.store(r);
        error = nullptr;
        error_chunk = 0;
    }
    JobSystem* js;
    RangeRef fn{kNoopRange};  // the caller's closure: only called for a claimed chunk
    uint32_t count = 0;
    uint32_t grain = 1;
    uint64_t chunks = 0;
    Priority prio = Priority::Critical;
    std::atomic<uint64_t> next{0};
    std::atomic<uint64_t> first_failed{kNoFail};  // lowest chunk that threw so far
    std::atomic<uint32_t> in_flight{0};  // threads between their claim attempt and the end of that chunk
    std::atomic<uint32_t> participants{0};  // sp1: threads that ran at least one chunk (work estimate)
    std::atomic<uint32_t> refs{0};       // the caller + every queued or running helper job
    std::mutex error_mutex;
    std::exception_ptr error;
    uint64_t error_chunk = 0;

    // No chunk left to start: all claimed, or the rest skipped after a failure.
    bool exhausted() const noexcept { return next.load() >= chunks || first_failed.load() != kNoFail; }
    // Every chunk ran (or was skipped). seq_cst throughout: a claimer raises in_flight before its claim,
    // so once `next` shows every chunk claimed, an in_flight of 0 means none is still running.
    bool finished() const noexcept { return exhausted() && in_flight.load() == 0; }
};

uint32_t JobSystem::default_worker_count() { return std::max(1u, machine_pool().workers); }

JobSystem::JobSystem(JobSystemConfig config) {
    workers_ = config.workers < 0 ? default_worker_count()
                                  : std::min(static_cast<uint32_t>(config.workers), kMaxWorkers);
    bg_cap_ = workers_ == 0 ? 0u : std::max(1u, workers_ / 2u);
    inline_small_ = config.inline_small;
    sites_ = std::make_unique<SiteCost[]>(kSites);
    const PoolPlan& plan = machine_pool();
    pinned_ = workers_ > 0 && config.cpu_sets && cpu_sets_allowed() && plan.cores > 0 &&
              workers_ + kReservedThreads <= plan.cores;
    if (workers_ > 0) {
        // Pre-warm the loop-state pool: up to 4 nested loops per thread at once never allocate, so a steady-state frame
        // does not either (render's zero-allocation checks); deeper or more concurrent nesting grows it on demand.
        const size_t warm = static_cast<size_t>(workers_ + 1) * 4u;
        for_pool_.reserve(warm);
        for_free_.reserve(warm);
        for (size_t i = 0; i < warm; ++i) {
            for_pool_.push_back(std::make_unique<ForState>(*this));
            for_free_.push_back(for_pool_.back().get());
        }
    }
    slots_.reserve(workers_ + 1);
    for (uint32_t i = 0; i <= workers_; ++i) slots_.push_back(std::make_unique<Slot>());
    threads_.reserve(workers_);
    for (uint32_t i = 1; i <= workers_; ++i) threads_.emplace_back([this, i] { worker_main(i); });
    log::debugf("jobs", "job system started: %u worker(s)%s%s", workers_, workers_ ? "" : " (serial mode)",
                pinned_ ? ", on the pool's cores" : "");
}

JobSystem::~JobSystem() {
    if (workers_ == 0) {
        // Serial mode: drain on the destroying thread.
        Job job;
        while (pop_foreground(0, job) || pop_shared(ai_, job) || pop_shared(bg_, job)) execute(job, 0);
        return;
    }
    stop_.store(true);
    work_epoch_.fetch_add(1);
    work_epoch_.notify_all();
    for (std::thread& t : threads_) t.join();
}

uint32_t JobSystem::my_slot() const noexcept { return t_state.system == this ? t_state.slot : 0u; }
uint32_t JobSystem::current_slot() const noexcept { return my_slot(); }

std::vector<uint64_t> JobSystem::executed_per_slot() const {
    std::vector<uint64_t> out;
    out.reserve(slots_.size());
    for (const auto& s : slots_) out.push_back(s->executed.load(std::memory_order_relaxed));
    return out;
}

Priority JobSystem::effective_priority(Priority requested) const noexcept { return lower_of(requested, ctx_priority(this)); }

void JobSystem::submit(JobCounter& counter, JobFn fn, void* ctx, uint32_t a, uint32_t b,
                       Priority priority) {
    const Job job{fn, ctx, a, b, &counter, effective_priority(priority)};
    counter.pending_.fetch_add(1);
    try {
        push(job, 1);
    } catch (...) {
        if (counter.pending_.fetch_sub(1) == 1) notify_waiters();
        throw;
    }
}

// Strong guarantee: on a throwing push_back nothing stays queued and no count changes.
void JobSystem::push(const Job& job, uint32_t copies) {
    const auto push_all = [&](std::deque<Job>& q, bool front) {
        uint32_t added = 0;
        try {
            for (; added < copies; ++added) front ? q.push_front(job) : q.push_back(job);
        } catch (...) {
            for (; added > 0; --added) front ? q.pop_front() : q.pop_back();
            throw;
        }
    };
    if (job.priority != Priority::Critical) {
        // Helpers of an AI / background parallel_for jump their queue: they are short, somebody waits on them, and
        // (background) they are exempt from the cap. So every queued helper precedes every other job of that queue,
        // and the queue front is a helper exactly when helpers > 0.
        SharedQueue& q = shared(job.priority);
        const bool helper = job.fn == &for_helper_job;
        std::lock_guard lk(q.mutex);
        push_all(q.queue, helper);
        if (helper) q.helpers.fetch_add(copies);
        q.queued.fetch_add(copies);  // under the queue lock: count never lags the queue
        q.epoch.fetch_add(1);
    } else {
        Slot& s = *slots_[my_slot()];
        std::lock_guard lk(s.mutex);
        push_all(s.queue, false);
        fg_queued_.fetch_add(copies);
    }
    wake(copies);
}

void JobSystem::wake(uint32_t copies) {
    // seq_cst RMW above + seq_cst loads here pair with the sleeper's seq_cst increment of idle_workers_ / waiters_
    // before it re-checks the queued counts (seq_cst): one side sees the other, so no wake-up is lost. The sleeper reads
    // the epoch before its check, so a bump between its check and its wait makes the wait return at once.
    // Wake at most `copies` idle workers (a 4-chunk loop no longer wakes the whole pool).
    const uint32_t idle = idle_workers_.load();
    if (idle > 0) {
        work_epoch_.fetch_add(1);
        if (copies >= idle) work_epoch_.notify_all();
        else
            for (uint32_t i = 0; i < copies; ++i) work_epoch_.notify_one();
    }
    if (waiters_.load() > 0) {
        done_epoch_.fetch_add(1);
        done_epoch_.notify_all();
    }
}

void JobSystem::notify_waiters() {
    if (waiters_.load(std::memory_order_seq_cst) == 0) return;
    done_epoch_.fetch_add(1);
    done_epoch_.notify_all();
}

bool JobSystem::pop_foreground(uint32_t self, Job& out) {
    if (fg_queued_.load(std::memory_order_relaxed) <= 0) return false;
    const auto n = static_cast<uint32_t>(slots_.size());
    {
        Slot& s = *slots_[self];
        std::lock_guard lk(s.mutex);
        if (!s.queue.empty()) {
            out = s.queue.back();
            s.queue.pop_back();
            fg_queued_.fetch_sub(1);
            return true;
        }
    }
    for (uint32_t k = 1; k < n; ++k) {
        Slot& s = *slots_[(self + k) % n];
        std::lock_guard lk(s.mutex);
        if (!s.queue.empty()) {
            out = s.queue.front();
            s.queue.pop_front();
            fg_queued_.fetch_sub(1);
            return true;
        }
    }
    return false;
}

bool JobSystem::pop_shared(SharedQueue& q, Job& out) {
    if (q.queued.load(std::memory_order_relaxed) <= 0) return false;
    std::lock_guard lk(q.mutex);
    if (q.queue.empty()) return false;
    out = q.queue.front();
    q.queue.pop_front();
    if (out.fn == &for_helper_job) q.helpers.fetch_sub(1);
    q.queued.fetch_sub(1);
    return true;
}

bool JobSystem::background_startable() const noexcept {
    return bg_.queued.load() > 0 && (bg_.helpers.load() > 0 || bg_running_.load() < bg_cap_ || stop_.load());
}

bool JobSystem::pop_background_capped(Job& out, bool& counted) {
    if (bg_.queued.load(std::memory_order_relaxed) <= 0) return false;
    std::lock_guard lk(bg_.mutex);  // every capped pop decides under this lock: the cap is exact
    if (bg_.queue.empty()) return false;
    const bool helper = bg_.queue.front().fn == &for_helper_job;
    if (helper) {
        bg_.helpers.fetch_sub(1);
    } else {
        if (bg_running_.load() >= bg_cap_ && !stop_.load()) return false;  // shutdown drains without the cap
        bg_running_.fetch_add(1);
    }
    out = bg_.queue.front();
    bg_.queue.pop_front();
    bg_.queued.fetch_sub(1);
    counted = !helper;
    return true;
}

void JobSystem::finish_background_capped() {
    // seq_cst RMW + seq_cst loads, paired with the sleeper's idle_workers_ increment before its predicate
    // (background_startable) re-reads bg_running_: a worker that slept because of the cap is woken here.
    bg_running_.fetch_sub(1);
    if (bg_.queued.load() > 0 && idle_workers_.load() > 0) {
        work_epoch_.fetch_add(1);
        work_epoch_.notify_one();
    }
}

bool JobSystem::claim_shared(SharedQueue& q, const JobCounter& counter, Job& out) {
    if (q.queued.load(std::memory_order_relaxed) <= 0) return false;
    std::lock_guard lk(q.mutex);
    for (auto it = q.queue.begin(); it != q.queue.end(); ++it) {
        if (it->counter == &counter) {
            out = *it;
            q.queue.erase(it);
            q.queued.fetch_sub(1);
            return true;
        }
    }
    return false;
}

void JobSystem::execute(const Job& job, uint32_t self) {
    const JobSystem* const prev_of = t_ctx_of;
    const Priority prev_prio = t_ctx_prio;
    if (job.priority != Priority::Critical) {
        t_ctx_of = this;
        t_ctx_prio = job.priority;
    } else if (prev_of == this) {
        t_ctx_prio = Priority::Critical;  // a critical job run by an AI / background thread: its own work is critical
    }
    const bool timed = t_execute_depth == 0 && profile::enabled();  // wall clock, write-only: never read by scheduling
    const int64_t busy_from = timed ? profile::now_ns() : 0;
    ++t_execute_depth;
    try {
        ScratchScope scope;  // inside the try: first use of the thread arena may allocate
        job.fn(job.ctx, job.a, job.b);
    } catch (...) {
        if (job.counter) job.counter->capture(job.a, std::current_exception());
    }
    t_ctx_of = prev_of;
    t_ctx_prio = prev_prio;
    --t_execute_depth;
    if (timed) profile::add_busy_ns(self, profile::now_ns() - busy_from);
    slots_[self]->executed.fetch_add(1, std::memory_order_relaxed);
    // After this decrement the counter (and anything the waiter owns) may be destroyed: touch
    // only `this` from here on. Only a counter reaching zero wakes waiters (other conditions use
    // notify_waiters()).
    if (job.counter && job.counter->pending_.fetch_sub(1, std::memory_order_seq_cst) == 1) notify_waiters();
}

void JobSystem::run_critical(uint32_t self) {
    Job job;
    while (pop_foreground(self, job)) execute(job, self);
}

void JobSystem::help_loop(FunctionRef<bool()> keep_waiting, JobCounter* claim) {
    const uint32_t self = my_slot();
    const Priority ctx = ctx_priority(this);
    const bool ai_ok = ctx != Priority::Critical;          // an AI or background thread may run AI jobs
    const bool bg_ok = ctx == Priority::Background;        // only a background thread runs background jobs
    uint64_t scanned_ai = ~uint64_t{0}, scanned_bg = ~uint64_t{0};  // queue epochs at the last unsuccessful claim scan
    int64_t spin_from = -1;
    while (keep_waiting()) {
        Job job;
        bool got = pop_foreground(self, job);
        if (!got && ai_ok) got = pop_shared(ai_, job);
        if (!got && bg_ok) got = pop_shared(bg_, job);
        if (!got && claim) {
            // Scan the shared queues for our own jobs only when something new was queued there.
            const uint64_t ea = ai_.epoch.load();
            if (ea != scanned_ai) {
                got = claim_shared(ai_, *claim, job);
                if (!got) scanned_ai = ea;
            }
            const uint64_t eb = bg_.epoch.load();
            if (!got && eb != scanned_bg) {
                got = claim_shared(bg_, *claim, job);
                if (!got) scanned_bg = eb;
            }
        }
        if (got) {
            execute(job, self);
            spin_from = -1;
            continue;
        }
        // J2: spin a few microseconds with _mm_pause (no yield: the sim thread keeps its core), then sleep.
        const int64_t now = profile::now_ns();
        if (spin_from < 0) spin_from = now;
        if (now - spin_from < kWaitSpinNs) {
            pause_burst();
            continue;
        }
        spin_from = -1;
        // seq_cst RMW, then seq_cst loads in the check (store-buffering pattern with the pusher / completer's seq_cst
        // RMW + seq_cst load of waiters_): one side must see the other. The epoch is read before each check.
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            const uint32_t e = done_epoch_.load();
            if (!keep_waiting() || fg_queued_.load() > 0 || (ai_ok && ai_.queued.load() > 0) ||
                (bg_ok && bg_.queued.load() > 0) ||
                (claim && (ai_.epoch.load() != scanned_ai || bg_.epoch.load() != scanned_bg)))
                break;
            done_epoch_.wait(e);
        }
        waiters_.fetch_sub(1);
    }
}

void JobSystem::wait(JobCounter& counter) {
    if (!counter.done()) {
        // seq_cst load: pairs with the completer's seq_cst fetch_sub + waiters_ load.
        help_loop([&counter] { return counter.pending_.load(std::memory_order_seq_cst) != 0; }, &counter);
    }
    counter.rethrow_if_failed();
}

void JobSystem::worker_main(uint32_t slot) {
    t_state = ThreadState{this, slot};
    if (pinned_) (void)pin_current_thread(machine_pool().cpu_set_ids);  // sp1: one logical processor per pool core
    (void)thread_scratch();  // create the arena up front
    const auto has_work = [this] { return fg_queued_.load() > 0 || ai_.queued.load() > 0 || background_startable(); };
    for (;;) {
        Job job;
        if (pop_foreground(slot, job) || pop_shared(ai_, job)) {
            execute(job, slot);
            continue;
        }
        bool counted = false;
        if (pop_background_capped(job, counted)) {
            execute(job, slot);
            if (counted) finish_background_capped();
            continue;
        }
        // J2: spin with _mm_pause before sleeping.
        bool found = false;
        const int64_t spin_from = profile::now_ns();
        do {
            if (has_work()) {
                found = true;
                break;
            }
            pause_burst();
        } while (profile::now_ns() - spin_from < kWorkerSpinNs && !stop_.load(std::memory_order_relaxed));
        if (found) continue;
        idle_workers_.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            const uint32_t e = work_epoch_.load();
            if (stop_.load() || has_work()) break;
            work_epoch_.wait(e);
        }
        idle_workers_.fetch_sub(1);
        // Exit only once every queue is empty: jobs pushed later can only come from threads that
        // are themselves running a job, and they drain their own work while waiting.
        if (stop_.load() && fg_queued_.load() <= 0 && ai_.queued.load() <= 0 && bg_.queued.load() <= 0) break;
    }
    t_state = ThreadState{};
}

// ---------------------------------------------------------------------------------------------
// parallel_for

void JobSystem::run_for_chunks(ForState& st) {
    // Once chunk f threw, chunks above f are pointless (the serial loop would have stopped at f)
    // and are skipped - checked before and after claiming. Chunks below f are always run, so the
    // lowest failing chunk is still found deterministically.
    const bool yield_to_critical = st.prio != Priority::Critical;
    bool counted = false;
    while (st.first_failed.load(std::memory_order_relaxed) == kNoFail) {
        // Fast exit for a late helper: it never shows up in in_flight, so a preemption here cannot
        // hold the caller. (The window between this check and the claim remains, two atomics wide.)
        if (st.next.load() >= st.chunks) break;
        st.in_flight.fetch_add(1);  // before the claim: see ForState::finished
        const uint64_t c = st.next.fetch_add(1);
        if (c >= st.chunks || c > st.first_failed.load()) {
            release_for_chunk(st);
            break;
        }
        if (!counted) {
            counted = true;
            st.participants.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t begin = c * st.grain;
        const uint64_t end = std::min<uint64_t>(begin + st.grain, st.count);
        try {
            ScratchScope scope;
            st.fn(static_cast<uint32_t>(begin), static_cast<uint32_t>(end));
        } catch (...) {
            std::lock_guard lk(st.error_mutex);
            if (!st.error || c < st.error_chunk) {
                st.error = std::current_exception();
                st.error_chunk = c;
                st.first_failed.store(c);
            }
        }
        release_for_chunk(st);
        // sp1 strict priority: between two chunks of an AI / background loop, queued critical work goes first.
        if (yield_to_critical && fg_queued_.load(std::memory_order_relaxed) > 0) run_critical(my_slot());
    }
}

void JobSystem::release_for_chunk(ForState& st) {
    // The thread that leaves the last chunk wakes a caller sleeping in help_loop (seq_cst RMW, then
    // notify_waiters' seq_cst load of waiters_: pairs with the sleeper's increment + predicate).
    if (st.in_flight.fetch_sub(1) == 1 && st.exhausted()) notify_waiters();
}

void JobSystem::for_helper_job(void* ctx, uint32_t, uint32_t) {
    auto* st = static_cast<ForState*>(ctx);
    st->js->run_for_chunks(*st);
    st->js->release_for_ref(st);
}

JobSystem::ForState* JobSystem::acquire_for_state() {
    std::lock_guard lk(for_pool_mutex_);
    if (!for_free_.empty()) {
        ForState* st = for_free_.back();
        for_free_.pop_back();
        return st;
    }
    // Grow (strong guarantee): the free list must hold every state without reallocating later.
    for_free_.reserve(for_pool_.size() + 1);
    for_pool_.reserve(for_pool_.size() + 1);
    for_pool_.push_back(std::make_unique<ForState>(*this));
    return for_pool_.back().get();
}

void JobSystem::release_for_ref(ForState* st, uint32_t refs) {
    if (st->refs.fetch_sub(refs, std::memory_order_acq_rel) != refs) return;
    std::lock_guard lk(for_pool_mutex_);
    for_free_.push_back(st);  // never reallocates: capacity >= for_pool_.size()
}

uint32_t JobSystem::cancel_queued_helpers(uint32_t self, const ForState& st) {
    const auto mine = [&st](const Job& j) { return j.fn == &for_helper_job && j.ctx == &st; };
    if (st.prio == Priority::Critical) {
        Slot& s = *slots_[self];
        std::lock_guard lk(s.mutex);
        const auto it = std::remove_if(s.queue.begin(), s.queue.end(), mine);
        const auto n = static_cast<uint32_t>(s.queue.end() - it);
        s.queue.erase(it, s.queue.end());
        if (n) fg_queued_.fetch_sub(n);
        return n;
    }
    // Helpers of an AI / background caller went to the front segment of that shared queue.
    SharedQueue& q = shared(st.prio);
    if (q.helpers.load(std::memory_order_relaxed) <= 0) return 0;
    std::lock_guard lk(q.mutex);
    uint32_t n = 0;
    for (auto it = q.queue.begin(); it != q.queue.end() && it->fn == &for_helper_job;) {
        if (it->ctx == &st) {
            it = q.queue.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    if (n) {
        q.helpers.fetch_sub(n);
        q.queued.fetch_sub(n);
    }
    return n;
}

JobSystem::SiteCost& JobSystem::site_cost(uintptr_t site) noexcept {
    const auto idx = static_cast<size_t>((static_cast<uint64_t>(site) * 0x9E3779B97F4A7C15ull) >> 56) % kSites;
    SiteCost& e = sites_[idx];
    if (e.site.load(std::memory_order_relaxed) != site) {  // another site held the entry: start over
        e.site.store(site, std::memory_order_relaxed);
        e.ns_per_item.store(-1.0f, std::memory_order_relaxed);
    }
    return e;
}

namespace {
void update_estimate(std::atomic<float>& est, double ns, uint32_t count) {
    const auto per_item = static_cast<float>(ns / static_cast<double>(count));
    const float old = est.load(std::memory_order_relaxed);
    est.store(old < 0.0f ? per_item : old * 0.75f + per_item * 0.25f, std::memory_order_relaxed);
}
}  // namespace

void JobSystem::parallel_for_ref(uint32_t count, uint32_t grain, RangeRef fn, uintptr_t site) {
    if (count == 0) return;
    if (grain == 0) grain = std::max<uint32_t>(1u, count / (thread_count() * 8u));
    const uint64_t chunks = (static_cast<uint64_t>(count) + grain - 1) / grain;

    const auto run_inline = [&] {
        for (uint64_t c = 0; c < chunks; ++c) {
            const uint64_t begin = c * grain;
            const uint64_t end = std::min<uint64_t>(begin + grain, count);
            ScratchScope scope;
            fn(static_cast<uint32_t>(begin), static_cast<uint32_t>(end));
        }
    };
    if (workers_ == 0 || chunks == 1) {
        run_inline();
        return;
    }

    // sp1: small loops run inline; larger ones get at most one thread per kMinThreadWorkNs of estimated work.
    auto helpers = static_cast<uint32_t>(std::min<uint64_t>(workers_, chunks - 1));
    SiteCost* cost = inline_small_ ? &site_cost(site ? site : fn.target_id()) : nullptr;
    if (cost) {
        const float per_item = cost->ns_per_item.load(std::memory_order_relaxed);
        if (per_item >= 0.0f) {
            const double est = static_cast<double>(per_item) * static_cast<double>(count);
            if (est < static_cast<double>(kInlineWorkNs)) {
                const int64_t t0 = profile::now_ns();
                run_inline();  // throws like the serial loop: the lowest failing chunk, nothing after it
                update_estimate(cost->ns_per_item, static_cast<double>(profile::now_ns() - t0), count);
                inline_forks_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto threads = static_cast<uint64_t>(est / static_cast<double>(kMinThreadWorkNs));
            helpers = static_cast<uint32_t>(std::min<uint64_t>(helpers, std::max<uint64_t>(threads, 2) - 1));
        }
    }
    const int64_t t0 = cost ? profile::now_ns() : 0;

    // Helpers claim chunks dynamically; one per worker at most. Helpers inherit the caller's priority (AI /
    // background). The state is shared with the helper jobs (refs), so a helper that starts after the caller
    // returned finds every chunk claimed and just exits.
    const Priority prio = effective_priority(Priority::Critical);
    ForState* st = acquire_for_state();
    st->reset(fn, count, grain, chunks, helpers + 1, prio);
    const Job job{&for_helper_job, st, 0, 0, nullptr, prio};
    try {
        push(job, helpers);
    } catch (...) {
        // Nothing was queued: run every chunk on this thread.
        st->refs.fetch_sub(helpers);  // the caller's own reference stays
    }
    run_for_chunks(*st);
    // Every chunk is claimed now: helpers still queued have nothing to do, take them back.
    if (const uint32_t n = cancel_queued_helpers(my_slot(), *st)) st->refs.fetch_sub(n);  // ours keeps it above 0
    if (!st->finished()) {
        ALAMO_PROFILE_SCOPE("jobs.for_wait");  // wall clock, write-only
        help_loop([st] { return !st->finished(); }, nullptr);
    }
    std::exception_ptr error;
    {
        std::lock_guard lk(st->error_mutex);
        error = std::move(st->error);
    }
    if (cost && !error) {
        // Work = wall time x the threads that ran chunks (an over-estimate when a helper joined late: errs on forking).
        const uint32_t threads = std::max(1u, st->participants.load(std::memory_order_relaxed));
        update_estimate(cost->ns_per_item, static_cast<double>(profile::now_ns() - t0) * threads, count);
    }
    release_for_ref(st);
    if (error) std::rethrow_exception(error);
}

}  // namespace alamo::jobs
