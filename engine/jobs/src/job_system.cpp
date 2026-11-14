#include "alamo/jobs/job_system.hpp"

#include "alamo/core/log.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/jobs/scratch.hpp"

namespace alamo::jobs {
namespace {

// Yields before a thread blocks on a condition variable. Short: blocking is cheap compared to
// burning a core that the sim thread may need.
constexpr uint32_t kSpinBeforeSleep = 64;
constexpr uint32_t kMaxWorkers = 256;
constexpr uint64_t kNoFail = ~uint64_t{0};
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
// Non-null while this thread executes a background job of that system (priority inheritance).
thread_local const JobSystem* t_background_of = nullptr;
// Nesting depth of execute() on this thread: a job that waits runs other jobs inside itself, and only the outermost one
// counts as busy time (OPT1 profiler).
thread_local uint32_t t_execute_depth = 0;

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
// JobSystem

struct JobSystem::Slot {
    std::mutex mutex;
    std::deque<Job> queue;
    std::atomic<uint64_t> executed{0};
};

uint32_t JobSystem::default_worker_count() {
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 1 ? static_cast<uint32_t>(hc - 1) : 1u;
}

JobSystem::JobSystem(JobSystemConfig config) {
    workers_ = config.workers < 0 ? default_worker_count()
                                  : std::min(static_cast<uint32_t>(config.workers), kMaxWorkers);
    bg_cap_ = workers_ == 0 ? 0u : std::max(1u, workers_ / 2u);
    slots_.reserve(workers_ + 1);
    for (uint32_t i = 0; i <= workers_; ++i) slots_.push_back(std::make_unique<Slot>());
    threads_.reserve(workers_);
    for (uint32_t i = 1; i <= workers_; ++i) threads_.emplace_back([this, i] { worker_main(i); });
    log::debugf("jobs", "job system started: %u worker(s)%s", workers_, workers_ ? "" : " (serial mode)");
}

JobSystem::~JobSystem() {
    if (workers_ == 0) {
        // Serial mode: drain on the destroying thread.
        Job job;
        while (pop_foreground(0, job) || pop_background(job)) execute(job, 0);
        return;
    }
    {
        std::lock_guard lk(sleep_mutex_);
        stop_.store(true);
    }
    work_cv_.notify_all();
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

Priority JobSystem::effective_priority(Priority requested) const noexcept {
    return t_background_of == this ? Priority::Background : requested;
}

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
    if (job.priority == Priority::Background) {
        // Helpers of a background parallel_for jump the queue: they are short, somebody waits on
        // them, and they are exempt from the background cap. So every queued helper precedes every
        // other background job, and the queue front is a helper exactly when bg_helpers_ > 0.
        const bool helper = job.fn == &for_helper_job;
        std::lock_guard lk(bg_mutex_);
        push_all(bg_queue_, helper);
        if (helper) bg_helpers_.fetch_add(copies);
        bg_queued_.fetch_add(copies);  // under the queue lock: count never lags the queue
        bg_epoch_.fetch_add(1);
    } else {
        Slot& s = *slots_[my_slot()];
        std::lock_guard lk(s.mutex);
        push_all(s.queue, false);
        fg_queued_.fetch_add(copies);
    }
    wake(copies);
}

void JobSystem::wake(uint32_t copies) {
    // seq_cst RMW above + seq_cst loads here pair with the sleeper's seq_cst increment of
    // idle_workers_/waiters_ before it re-checks the queued counts (seq_cst): no lost wake-ups.
    // Wake at most `copies` idle workers (a 4-chunk loop no longer wakes the whole pool).
    const uint32_t idle = idle_workers_.load();
    const bool waiting = waiters_.load() > 0;
    if (idle == 0 && !waiting) return;
    { std::lock_guard lk(sleep_mutex_); }
    if (idle > 0) {
        if (copies >= idle) work_cv_.notify_all();
        else
            for (uint32_t i = 0; i < copies; ++i) work_cv_.notify_one();
    }
    if (waiting) done_cv_.notify_all();
}

void JobSystem::notify_waiters() {
    if (waiters_.load(std::memory_order_seq_cst) == 0) return;
    { std::lock_guard lk(sleep_mutex_); }
    done_cv_.notify_all();
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

bool JobSystem::pop_background(Job& out) {
    if (bg_queued_.load(std::memory_order_relaxed) <= 0) return false;
    std::lock_guard lk(bg_mutex_);
    if (bg_queue_.empty()) return false;
    out = bg_queue_.front();
    bg_queue_.pop_front();
    if (out.fn == &for_helper_job) bg_helpers_.fetch_sub(1);
    bg_queued_.fetch_sub(1);
    return true;
}

bool JobSystem::background_startable() const noexcept {
    return bg_queued_.load() > 0 && (bg_helpers_.load() > 0 || bg_running_.load() < bg_cap_ || stop_.load());
}

bool JobSystem::pop_background_capped(Job& out, bool& counted) {
    if (bg_queued_.load(std::memory_order_relaxed) <= 0) return false;
    std::lock_guard lk(bg_mutex_);  // every capped pop decides under this lock: the cap is exact
    if (bg_queue_.empty()) return false;
    const bool helper = bg_queue_.front().fn == &for_helper_job;
    if (helper) {
        bg_helpers_.fetch_sub(1);
    } else {
        if (bg_running_.load() >= bg_cap_ && !stop_.load()) return false;  // shutdown drains without the cap
        bg_running_.fetch_add(1);
    }
    out = bg_queue_.front();
    bg_queue_.pop_front();
    bg_queued_.fetch_sub(1);
    counted = !helper;
    return true;
}

void JobSystem::finish_background_capped() {
    // seq_cst RMW + seq_cst loads, paired with the sleeper's idle_workers_ increment before its predicate
    // (background_startable) re-reads bg_running_: a worker that slept because of the cap is woken here.
    bg_running_.fetch_sub(1);
    if (bg_queued_.load() > 0 && idle_workers_.load() > 0) {
        { std::lock_guard lk(sleep_mutex_); }
        work_cv_.notify_one();
    }
}

bool JobSystem::claim_background(const JobCounter& counter, Job& out) {
    if (bg_queued_.load(std::memory_order_relaxed) <= 0) return false;
    std::lock_guard lk(bg_mutex_);
    for (auto it = bg_queue_.begin(); it != bg_queue_.end(); ++it) {
        if (it->counter == &counter) {
            out = *it;
            bg_queue_.erase(it);
            bg_queued_.fetch_sub(1);
            return true;
        }
    }
    return false;
}

void JobSystem::execute(const Job& job, uint32_t self) {
    const JobSystem* const prev = t_background_of;
    t_background_of = job.priority == Priority::Background ? this : (prev == this ? nullptr : prev);
    const bool timed = t_execute_depth == 0 && profile::enabled();  // wall clock, write-only: never read by scheduling
    const int64_t busy_from = timed ? profile::now_ns() : 0;
    ++t_execute_depth;
    try {
        ScratchScope scope;  // inside the try: first use of the thread arena may allocate
        job.fn(job.ctx, job.a, job.b);
    } catch (...) {
        if (job.counter) job.counter->capture(job.a, std::current_exception());
    }
    t_background_of = prev;
    --t_execute_depth;
    if (timed) profile::add_busy_ns(self, profile::now_ns() - busy_from);
    slots_[self]->executed.fetch_add(1, std::memory_order_relaxed);
    // After this decrement the counter (and anything the waiter owns) may be destroyed: touch
    // only `this` from here on. Only a counter reaching zero wakes waiters (other conditions use
    // notify_waiters()).
    if (job.counter && job.counter->pending_.fetch_sub(1, std::memory_order_seq_cst) == 1) notify_waiters();
}

void JobSystem::help_loop(FunctionRef<bool()> keep_waiting, JobCounter* claim) {
    const uint32_t self = my_slot();
    const bool background_ctx = t_background_of == this;
    uint32_t spins = 0;
    uint64_t scanned_epoch = ~uint64_t{0};  // bg epoch at the last unsuccessful claim scan
    while (keep_waiting()) {
        Job job;
        bool got = pop_foreground(self, job);
        if (!got && background_ctx) got = pop_background(job);
        if (!got && claim) {
            // Scan the background queue for our own jobs only when something new was queued.
            const uint64_t epoch = bg_epoch_.load();
            if (epoch != scanned_epoch) {
                got = claim_background(*claim, job);
                if (!got) scanned_epoch = epoch;
            }
        }
        if (got) {
            execute(job, self);
            spins = 0;
            continue;
        }
        if (++spins < kSpinBeforeSleep) {
            std::this_thread::yield();
            continue;
        }
        spins = 0;
        std::unique_lock lk(sleep_mutex_);
        // seq_cst RMW, then seq_cst loads in the predicate (store-buffering pattern with the
        // pusher/completer's seq_cst RMW + seq_cst load of waiters_): one side must see the other.
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        done_cv_.wait(lk, [&] {
            return !keep_waiting() || fg_queued_.load() > 0 ||
                   (background_ctx && bg_queued_.load() > 0) ||
                   (claim && bg_epoch_.load() != scanned_epoch);
        });
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
    (void)thread_scratch();  // create the arena up front
    for (;;) {
        Job job;
        if (pop_foreground(slot, job)) {
            execute(job, slot);
            continue;
        }
        bool counted = false;
        if (pop_background_capped(job, counted)) {
            execute(job, slot);
            if (counted) finish_background_capped();
            continue;
        }
        bool found = false;
        for (uint32_t s = 0; s < kSpinBeforeSleep; ++s) {
            if (fg_queued_.load(std::memory_order_relaxed) > 0 || background_startable()) {
                found = true;
                break;
            }
            std::this_thread::yield();
        }
        if (found) continue;
        std::unique_lock lk(sleep_mutex_);
        idle_workers_.fetch_add(1, std::memory_order_seq_cst);
        work_cv_.wait(lk, [&] { return stop_.load() || fg_queued_.load() > 0 || background_startable(); });
        idle_workers_.fetch_sub(1);
        // Exit only once every queue is empty: jobs pushed later can only come from threads that
        // are themselves running a job, and they drain their own work while waiting.
        if (stop_.load() && fg_queued_.load() <= 0 && bg_queued_.load() <= 0) break;
    }
    t_state = ThreadState{};
}

// ---------------------------------------------------------------------------------------------
// parallel_for

struct JobSystem::ForState {
    explicit ForState(JobSystem& s) : js(&s) {}
    // Set up for a new loop; only called by the owner while no job references this state.
    void reset(RangeRef f, uint32_t n, uint32_t g, uint64_t c, uint32_t r) {
        fn = f;
        count = n;
        grain = g;
        chunks = c;
        next.store(0);
        first_failed.store(kNoFail);
        in_flight.store(0);
        refs.store(r);
        error = nullptr;
        error_chunk = 0;
    }
    JobSystem* js;
    RangeRef fn{kNoopRange};  // the caller's closure: only called for a claimed chunk
    uint32_t count = 0;
    uint32_t grain = 1;
    uint64_t chunks = 0;
    std::atomic<uint64_t> next{0};
    std::atomic<uint64_t> first_failed{kNoFail};  // lowest chunk that threw so far
    std::atomic<uint32_t> in_flight{0};  // threads between their claim attempt and the end of that chunk
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

void JobSystem::run_for_chunks(ForState& st) {
    // Once chunk f threw, chunks above f are pointless (the serial loop would have stopped at f)
    // and are skipped - checked before and after claiming. Chunks below f are always run, so the
    // lowest failing chunk is still found deterministically.
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
    const auto take_out = [&mine](std::deque<Job>& q, std::atomic<int64_t>& queued) {
        const auto it = std::remove_if(q.begin(), q.end(), mine);
        const auto n = static_cast<uint32_t>(q.end() - it);
        q.erase(it, q.end());
        if (n) queued.fetch_sub(n);
        return n;
    };
    {
        Slot& s = *slots_[self];
        std::lock_guard lk(s.mutex);
        if (const uint32_t n = take_out(s.queue, fg_queued_)) return n;
    }
    // Helpers of a background caller went to the front segment of the shared background queue.
    if (bg_helpers_.load(std::memory_order_relaxed) <= 0) return 0;
    std::lock_guard lk(bg_mutex_);
    uint32_t n = 0;
    for (auto it = bg_queue_.begin(); it != bg_queue_.end() && it->fn == &for_helper_job;) {
        if (it->ctx == &st) {
            it = bg_queue_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    if (n) {
        bg_helpers_.fetch_sub(n);
        bg_queued_.fetch_sub(n);
    }
    return n;
}

void JobSystem::parallel_for_ref(uint32_t count, uint32_t grain, RangeRef fn) {
    if (count == 0) return;
    if (grain == 0) grain = std::max<uint32_t>(1u, count / (thread_count() * 8u));
    const uint64_t chunks = (static_cast<uint64_t>(count) + grain - 1) / grain;

    if (workers_ == 0 || chunks == 1) {
        for (uint64_t c = 0; c < chunks; ++c) {
            const uint64_t begin = c * grain;
            const uint64_t end = std::min<uint64_t>(begin + grain, count);
            ScratchScope scope;
            fn(static_cast<uint32_t>(begin), static_cast<uint32_t>(end));
        }
        return;
    }

    // Helpers claim chunks dynamically; one per worker at most. Helpers inherit background
    // priority from a background caller. The state is shared with the helper jobs (refs), so a
    // helper that starts after the caller returned finds every chunk claimed and just exits.
    const auto helpers = static_cast<uint32_t>(std::min<uint64_t>(workers_, chunks - 1));
    ForState* st = acquire_for_state();
    st->reset(fn, count, grain, chunks, helpers + 1);
    const Job job{&for_helper_job, st, 0, 0, nullptr, effective_priority(Priority::Normal)};
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
    release_for_ref(st);
    if (error) std::rethrow_exception(error);
}

}  // namespace alamo::jobs
