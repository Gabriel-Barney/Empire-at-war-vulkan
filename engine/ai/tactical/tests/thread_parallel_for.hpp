// Minimal std::thread-backed alamo::ParallelFor for the ai_tactical tests (the real backend lives in engine/jobs).
// A persistent pool of `threads - 1` workers plus the calling thread; chunks of `grain` indices are claimed
// atomically, so which thread runs which chunk varies run to run - results must not. A call made from inside a
// running body (nested) runs serially on that thread.
//
// The caller waits only for chunks that a thread has claimed, never for a worker to wake up. Floyd-Warshall makes one
// call per k (about 9300 over the shipped maps), and the old per-call std::thread spawn + join waited for every new
// thread to get a time slice: at 100% CPU (other sessions' builds) the 2 s test ran past ctest's 600 s timeout
// (2026-11-03). A worker that wakes after the last chunk was claimed finds nothing left and goes back to sleep.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "alamo/core/parallel.hpp"

namespace alamo::ai::tactical::testing {

class ThreadPoolFor {
public:
    explicit ThreadPoolFor(unsigned threads) {
        const unsigned extra = std::max(1u, threads) - 1;
        workers_.reserve(extra);
        for (unsigned t = 0; t < extra; ++t) workers_.emplace_back([this] { loop(); });
    }
    ThreadPoolFor(const ThreadPoolFor&) = delete;
    ThreadPoolFor& operator=(const ThreadPoolFor&) = delete;
    ~ThreadPoolFor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : workers_) t.join();
    }

    void run(uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        if (workers_.empty() || chunks == 1 || in_body()) {
            for (uint32_t c = 0; c < chunks; ++c) fn(c * gr, std::min(count, c * gr + gr));
            return;
        }
        std::lock_guard<std::mutex> one_call(call_);  // calls from different outside threads take turns
        auto job = std::make_shared<Job>();
        job->fn = &fn;
        job->count = count;
        job->grain = gr;
        job->chunks = chunks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = job;
            ++generation_;
        }
        wake_.notify_all();
        work(*job);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return job->done.load() == job->chunks; });
        job_.reset();
    }

private:
    // One call's chunks. fn is only called for a chunk claimed from `next`, and run() does not return before every
    // claimed chunk is done, so a worker still holding an old Job never touches a dead fn.
    struct Job {
        const RangeFn* fn = nullptr;
        uint32_t count = 0, grain = 1, chunks = 0;
        std::atomic<uint32_t> next{0}, done{0};
    };

    static bool& in_body() {
        thread_local bool flag = false;
        return flag;
    }
    void work(Job& j) {
        in_body() = true;
        for (;;) {
            const uint32_t c = j.next.fetch_add(1);
            if (c >= j.chunks) break;
            const uint32_t b = c * j.grain;
            (*j.fn)(b, std::min(j.count, b + j.grain));
            if (j.done.fetch_add(1) + 1 == j.chunks) {
                std::lock_guard<std::mutex> lock(mutex_);  // the waiter checks `done` under mutex_: no lost wakeup
                done_.notify_one();
            }
        }
        in_body() = false;
    }
    void loop() {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            std::shared_ptr<Job> j = job_;  // null when the call already finished without this worker
            lock.unlock();
            if (j) work(*j);
            j.reset();
            lock.lock();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex call_, mutex_;
    std::condition_variable wake_, done_;
    std::shared_ptr<Job> job_;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

inline ParallelFor make_thread_parallel_for(unsigned threads) {
    auto pool = std::make_shared<ThreadPoolFor>(threads);
    return [pool](uint32_t count, uint32_t grain, const RangeFn& fn) { pool->run(count, grain, fn); };
}

}  // namespace alamo::ai::tactical::testing
