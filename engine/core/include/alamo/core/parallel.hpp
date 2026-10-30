// Fan-out hook so modules (pathing, AI scoring) can run in parallel without depending on the
// job system. engine/jobs provides an implementation backed by its thread pool; the default
// runs serially on the calling thread.
//
// Contract: fn(begin, end) is called for disjoint ranges covering [0, count). Ranges may run
// concurrently and in any order, so fn must write only to per-index outputs. The call returns
// after every range has finished. Results must not depend on how ranges were split.
#pragma once
#include <cstdint>
#include <functional>

namespace alamo {

using RangeFn = std::function<void(uint32_t begin, uint32_t end)>;
using ParallelFor = std::function<void(uint32_t count, uint32_t grain, const RangeFn& fn)>;

inline void serial_for(uint32_t count, uint32_t /*grain*/, const RangeFn& fn) {
    if (count) fn(0, count);
}

}  // namespace alamo
