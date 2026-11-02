// Minimal std::thread-backed alamo::ParallelFor for tests (the real backend lives in
// engine/jobs). Each ParallelFor owns a pool of threads - 1 workers started once and reused by
// every call (spawning them per call made the 1/4/N-thread battle tests 4-5x slower under a loaded
// ctest -j 8 and time out, 2026-11-02). Chunks of `grain` indices are claimed atomically, so which
// thread runs which chunk varies run to run - results must not. A nested call (from inside `fn`)
// runs serially on the calling thread.
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

namespace alamo::tactical::testing {

class ThreadPoolFor {
public:
    explicit ThreadPoolFor(unsigned threads) {
        const unsigned extra = std::max(1u, threads) - 1;
        workers_.reserve(extra);
        for (unsigned t = 0; t < extra; ++t) workers_.emplace_back([this] { worker_main(); });
    }
    ThreadPoolFor(const ThreadPoolFor&) = delete;
    ThreadPoolFor& operator=(const ThreadPoolFor&) = delete;
    ~ThreadPoolFor() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
    }

    void run(uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        if (workers_.empty() || chunks == 1 || active_.exchange(true)) {
            for (uint32_t c = 0; c < chunks; ++c) fn(c * gr, std::min(count, c * gr + gr));
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            fn_ = &fn;
            count_ = count;
            grain_ = gr;
            chunks_ = chunks;
            next_.store(0);
            busy_ = workers_.size();
            ++generation_;
        }
        wake_.notify_all();
        drain();
        {
            std::unique_lock<std::mutex> lk(mutex_);
            done_.wait(lk, [this] { return busy_ == 0; });
            fn_ = nullptr;
        }
        active_.store(false);
    }

private:
    // fn_ / count_ / grain_ / chunks_ are written under mutex_ before generation_ moves and stay fixed
    // until every worker has reported back, so drain() reads them without the lock.
    void drain() {
        for (;;) {
            const uint32_t c = next_.fetch_add(1);
            if (c >= chunks_) break;
            const uint32_t b = c * grain_;
            (*fn_)(b, std::min(count_, b + grain_));
        }
    }

    void worker_main() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mutex_);
                wake_.wait(lk, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            drain();
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (--busy_ == 0) done_.notify_one();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> next_{0};
    const RangeFn* fn_ = nullptr;
    uint32_t count_ = 0;
    uint32_t grain_ = 1;
    uint32_t chunks_ = 0;
    size_t busy_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

inline ParallelFor make_thread_parallel_for(unsigned threads) {
    auto pool = std::make_shared<ThreadPoolFor>(threads);
    return [pool](uint32_t count, uint32_t grain, const RangeFn& fn) { pool->run(count, grain, fn); };
}

}  // namespace alamo::tactical::testing
