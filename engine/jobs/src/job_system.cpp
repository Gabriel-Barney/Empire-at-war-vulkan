#include "alamo/jobs/job_system.hpp"

#include "alamo/core/log.hpp"
#include "alamo/jobs/scratch.hpp"

namespace alamo::jobs {
namespace {

// Yields before a thread blocks on a condition variable. Short: blocking is cheap compared to
// burning a core that the sim thread may need.
constexpr uint32_t kSpinBeforeSleep = 64;
constexpr uint32_t kMaxWorkers = 256;

struct ThreadState {
    const JobSystem* system = nullptr;
    uint32_t slot = 0;
};
thread_local ThreadState t_state;
// Non-null while this thread executes a background job of that system (priority inheritance).
thread_local const JobSystem* t_background_of = nullptr;

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
    const auto push_all = [&](std::deque<Job>& q) {
        uint32_t added = 0;
        try {
            for (; added < copies; ++added) q.push_back(job);
        } catch (...) {
            for (; added > 0; --added) q.pop_back();
            throw;
        }
    };
    if (job.priority == Priority::Background) {
        std::lock_guard lk(bg_mutex_);
        push_all(bg_queue_);
        bg_queued_.fetch_add(copies);  // under the queue lock: count never lags the queue
        bg_epoch_.fetch_add(1);
    } else {
        Slot& s = *slots_[my_slot()];
        std::lock_guard lk(s.mutex);
        push_all(s.queue);
        fg_queued_.fetch_add(copies);
    }
    wake(copies);
}

void JobSystem::wake(uint32_t copies) {
    // seq_cst RMW above + seq_cst loads here pair with the sleeper's seq_cst increment of
    // idle_workers_/waiters_ before it re-checks the queued counts (seq_cst): no lost wake-ups.
    const bool idle = idle_workers_.load() > 0;
    const bool waiting = waiters_.load() > 0;
    if (!idle && !waiting) return;
    { std::lock_guard lk(sleep_mutex_); }
    if (idle) {
        if (copies > 1) work_cv_.notify_all();
        else work_cv_.notify_one();
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
    bg_queued_.fetch_sub(1);
    return true;
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
    try {
        ScratchScope scope;  // inside the try: first use of the thread arena may allocate
        job.fn(job.ctx, job.a, job.b);
    } catch (...) {
        job.counter->capture(job.a, std::current_exception());
    }
    t_background_of = prev;
    slots_[self]->executed.fetch_add(1, std::memory_order_relaxed);
    // After this decrement the counter (and anything the waiter owns) may be destroyed: touch
    // only `this` from here on. Only a counter reaching zero wakes waiters (other conditions use
    // notify_waiters()).
    if (job.counter->pending_.fetch_sub(1, std::memory_order_seq_cst) == 1) notify_waiters();
}

bool JobSystem::try_run_one() {
    const uint32_t self = my_slot();
    Job job;
    if (!pop_foreground(self, job) && !(t_background_of == this && pop_background(job))) return false;
    execute(job, self);
    return true;
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

void JobSystem::help_while(FunctionRef<bool()> keep_waiting) { help_loop(keep_waiting, nullptr); }

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
        if (pop_foreground(slot, job) || pop_background(job)) {
            execute(job, slot);
            continue;
        }
        bool found = false;
        for (uint32_t s = 0; s < kSpinBeforeSleep; ++s) {
            if (fg_queued_.load(std::memory_order_relaxed) > 0 ||
                bg_queued_.load(std::memory_order_relaxed) > 0) {
                found = true;
                break;
            }
            std::this_thread::yield();
        }
        if (found) continue;
        std::unique_lock lk(sleep_mutex_);
        idle_workers_.fetch_add(1, std::memory_order_seq_cst);
        work_cv_.wait(lk, [&] { return stop_.load() || fg_queued_.load() > 0 || bg_queued_.load() > 0; });
        idle_workers_.fetch_sub(1);
        // Exit only once every queue is empty: jobs pushed later can only come from threads that
        // are themselves running a job, and they drain their own work while waiting.
        if (stop_.load() && fg_queued_.load() <= 0 && bg_queued_.load() <= 0) break;
    }
    t_state = ThreadState{};
}

// ---------------------------------------------------------------------------------------------
// parallel_for

namespace {

struct ForState {
    ForState(RangeRef f, uint32_t n, uint32_t g, uint64_t c) : fn(f), count(n), grain(g), chunks(c) {}
    RangeRef fn;
    uint32_t count;
    uint32_t grain;
    uint64_t chunks;
    std::atomic<uint64_t> next{0};
    std::atomic<uint64_t> first_failed{~uint64_t{0}};  // lowest chunk that threw so far
    std::mutex error_mutex;
    std::exception_ptr error;
    uint64_t error_chunk = 0;
};

void run_chunks(ForState& st) {
    // Once chunk f threw, chunks above f are pointless (the serial loop would have stopped at f)
    // and are skipped - checked before and after claiming. Chunks below f are always run, so the
    // lowest failing chunk is still found deterministically.
    while (st.first_failed.load(std::memory_order_relaxed) == ~uint64_t{0}) {
        const uint64_t c = st.next.fetch_add(1, std::memory_order_relaxed);
        if (c >= st.chunks || c > st.first_failed.load(std::memory_order_relaxed)) break;
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
                st.first_failed.store(c, std::memory_order_relaxed);
            }
        }
    }
}

void run_chunks_job(void* ctx, uint32_t, uint32_t) { run_chunks(*static_cast<ForState*>(ctx)); }

}  // namespace

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

    ForState st(fn, count, grain, chunks);
    // Helpers claim chunks dynamically; one per worker at most. A helper that starts after all
    // chunks are claimed exits immediately (cheap), and the caller pops still-queued helpers
    // itself while waiting. Helpers inherit background priority from a background caller.
    const auto helpers = static_cast<uint32_t>(std::min<uint64_t>(workers_, chunks - 1));
    JobCounter counter;
    const Job job{&run_chunks_job, &st, 0, 0, &counter, effective_priority(Priority::Normal)};
    counter.pending_.fetch_add(helpers);
    try {
        push(job, helpers);
    } catch (...) {
        // Nothing was queued: degrade to running every chunk on this thread.
        counter.pending_.fetch_sub(helpers);
    }
    run_chunks(st);
    wait(counter);
    if (st.error) std::rethrow_exception(st.error);
}

}  // namespace alamo::jobs
