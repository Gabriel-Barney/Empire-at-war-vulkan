// Minimal std::thread-backed alamo::ParallelFor for tests and benches (the real backend lives in
// engine/jobs). A persistent pool of `threads - 1` workers plus the calling thread; chunks of `grain` indices are
// claimed atomically. (It used to spawn threads per call, which dominated the campaign CRC tests' run time.)
// A call made from inside a running body (nested) runs serially on that thread.
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

namespace alamo::galactic::testing {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads) {
        const unsigned extra = std::max(1u, threads) - 1;
        workers_.reserve(extra);
        for (unsigned t = 0; t < extra; ++t) workers_.emplace_back([this] { loop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (std::thread& t : workers_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void run(uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        if (workers_.empty() || chunks == 1 || in_body()) {
            for (uint32_t c = 0; c < chunks; ++c) fn(c * gr, std::min(count, c * gr + gr));
            return;
        }
        std::lock_guard<std::mutex> one_call(call_);  // calls from different outside threads take turns
        std::unique_lock<std::mutex> lock(mutex_);
        fn_ = &fn;
        count_ = count;
        grain_ = gr;
        chunks_ = chunks;
        next_.store(0, std::memory_order_relaxed);
        active_ = workers_.size();
        ++generation_;
        lock.unlock();
        wake_.notify_all();
        work();
        lock.lock();
        done_.wait(lock, [this] { return active_ == 0; });
        fn_ = nullptr;
    }

private:
    static bool& in_body() {
        thread_local bool flag = false;
        return flag;
    }
    void work() {
        in_body() = true;
        for (;;) {
            const uint32_t c = next_.fetch_add(1, std::memory_order_relaxed);
            if (c >= chunks_) break;
            const uint32_t b = c * grain_;
            (*fn_)(b, std::min(count_, b + grain_));
        }
        in_body() = false;
    }
    void loop() {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return generation_ != seen; });
            seen = generation_;
            if (stop_) return;
            lock.unlock();
            work();
            lock.lock();
            if (--active_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex call_, mutex_;
    std::condition_variable wake_, done_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    size_t active_ = 0;
    const RangeFn* fn_ = nullptr;
    uint32_t count_ = 0, grain_ = 1, chunks_ = 0;
    std::atomic<uint32_t> next_{0};
};

inline ParallelFor make_thread_parallel_for(unsigned threads) {
    auto pool = std::make_shared<ThreadPool>(threads);
    return [pool](uint32_t count, uint32_t grain, const RangeFn& fn) { pool->run(count, grain, fn); };
}

}  // namespace alamo::galactic::testing
