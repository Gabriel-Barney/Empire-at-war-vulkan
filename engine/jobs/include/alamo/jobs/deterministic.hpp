// Deterministic parallel patterns: results are bit-identical for any worker count (0, 1, 4, 16...)
// and any scheduling, as required by the lockstep contract in docs/PHASE2.md.
//
// Both helpers accept either a JobSystem& or a core alamo::ParallelFor (so modules that only
// depend on core, e.g. galactic, can use them through the hook).
#pragma once
#include <algorithm>
#include <functional>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/jobs/job_system.hpp"

namespace alamo::jobs {

// Default reduction chunk. Part of the result definition for floats: changing it changes the
// summation tree, so callers that persist/compare results should pass an explicit chunk.
inline constexpr uint32_t kDefaultReduceChunk = 1024;

namespace detail {
template <class Exec, class F>
void exec_for(Exec& exec, uint32_t count, uint32_t grain, F&& fn) {
    if constexpr (std::is_same_v<std::remove_cv_t<Exec>, JobSystem>) {
        exec.parallel_for(count, grain, fn);
    } else {
        const alamo::RangeFn range = std::ref(fn);
        exec(count, grain, range);
    }
}
}  // namespace detail

// One per-index result slot. Wrapping R keeps std::vector<bool>'s bit-packing out of the picture:
// adjacent indices written by different threads must be distinct memory locations.
template <class R>
struct ResultCell {
    R value{};
};
// Reusable per-index buffer for parallel_map_then_ordered_commit (any R, including bool).
template <class R>
using CommitBuffer = std::vector<ResultCell<R>>;

// Fixed-chunk reduction.
//   partial[c] = map_chunk(c*chunk, min((c+1)*chunk, count))      (in parallel, one per chunk)
//   result     = combine(...combine(combine(init, partial[0]), partial[1])..., partial[n-1])
// The chunking depends only on (count, chunk) and the fold runs serially in chunk order, so a
// float sum is bit-identical across worker counts. T must be default-constructible and
// move-assignable (bool is fine). map_chunk must be a pure function of its range.
template <class T, class Exec, class MapChunk, class Combine>
T parallel_reduce(Exec& exec, uint32_t count, uint32_t chunk, T init, MapChunk&& map_chunk,
                  Combine&& combine) {
    if (count == 0) return init;
    if (chunk == 0) chunk = kDefaultReduceChunk;
    const auto chunks = static_cast<uint32_t>((static_cast<uint64_t>(count) + chunk - 1) / chunk);
    std::vector<ResultCell<T>> partial(chunks);
    detail::exec_for(exec, chunks, 1, [&](uint32_t cb, uint32_t ce) {
        for (uint32_t c = cb; c < ce; ++c) {
            const uint64_t begin = static_cast<uint64_t>(c) * chunk;
            const uint64_t end = std::min<uint64_t>(begin + chunk, count);
            partial[c].value = map_chunk(static_cast<uint32_t>(begin), static_cast<uint32_t>(end));
        }
    });
    T acc = std::move(init);
    for (uint32_t c = 0; c < chunks; ++c) acc = combine(std::move(acc), std::move(partial[c].value));
    return acc;
}

// "Parallel pure scoring, then serial ordered insert" (AI goal proposal shape, HOTSPOTS.md §1):
//   buffer[i] = map(i)            in parallel (map must be pure / write nothing shared)
//   commit(i, buffer[i])          serially, i = 0..count-1, on the calling thread
// Commit order equals the original serial loop, so insertion order, tie-breaks and any RNG draws
// made in commit are unchanged. `buffer` is reused across calls to avoid reallocation.
// R must be default-constructible and move-assignable; R = bool is fine (see ResultCell).
template <class Exec, class Map, class Commit,
          class R = std::remove_cvref_t<std::invoke_result_t<Map&, uint32_t>>>
void parallel_map_then_ordered_commit(Exec& exec, uint32_t count, uint32_t grain, Map&& map,
                                      Commit&& commit, CommitBuffer<R>& buffer) {
    buffer.clear();
    buffer.resize(count);
    detail::exec_for(exec, count, grain, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) buffer[i].value = map(i);
    });
    for (uint32_t i = 0; i < count; ++i) commit(i, buffer[i].value);
}

template <class Exec, class Map, class Commit>
void parallel_map_then_ordered_commit(Exec& exec, uint32_t count, uint32_t grain, Map&& map,
                                      Commit&& commit) {
    CommitBuffer<std::remove_cvref_t<std::invoke_result_t<Map&, uint32_t>>> buffer;
    parallel_map_then_ordered_commit(exec, count, grain, std::forward<Map>(map),
                                     std::forward<Commit>(commit), buffer);
}

}  // namespace alamo::jobs
