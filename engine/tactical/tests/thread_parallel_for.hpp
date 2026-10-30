// Minimal std::thread-backed alamo::ParallelFor for tests (the real backend lives in
// engine/jobs). Spawns threads per call; chunks of `grain` indices are claimed atomically, so
// which thread runs which chunk varies run to run - results must not.
#pragma once
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "alamo/core/parallel.hpp"

namespace alamo::tactical::testing {

inline ParallelFor make_thread_parallel_for(unsigned threads) {
    return [threads](uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        std::atomic<uint32_t> next{0};
        auto worker = [&] {
            for (;;) {
                const uint32_t c = next.fetch_add(1);
                if (c >= chunks) break;
                const uint32_t b = c * gr;
                fn(b, std::min(count, b + gr));
            }
        };
        const unsigned extra = std::min<unsigned>(std::max(1u, threads), chunks) - 1;
        std::vector<std::thread> ts;
        ts.reserve(extra);
        for (unsigned t = 0; t < extra; ++t) ts.emplace_back(worker);
        worker();
        for (auto& t : ts) t.join();
    };
}

}  // namespace alamo::tactical::testing
