// Per-thread scratch (bump) arena. Fixes the allocator contention noted in docs/re/HOTSPOTS.md
// (perception contexts, goal candidates and cull-query vectors heap-allocate per evaluation and
// serialize on the process heap lock).
//
// - thread_scratch() returns the calling thread's arena (thread_local, created on first use,
//   no locking ever).
// - Allocation is a pointer bump; deallocation is a no-op. Memory comes back by rewinding to a
//   Marker, normally through ScratchScope (RAII). Scopes on one thread nest strictly (stack
//   discipline), which also holds when a waiting thread helps run other jobs inside its wait.
// - The job system wraps every job and every parallel_for chunk in a ScratchScope, so a job's
//   scratch is "reset per job batch" automatically. Never return scratch pointers out of a job.
// - No destructors run on rewind: only put trivially destructible data in it directly, or use it
//   as a std::pmr::memory_resource for containers whose lifetime ends inside the scope, e.g.
//     ScratchScope scope;
//     std::pmr::unordered_map<int, float> tokens(&thread_scratch());   // perception context
// - When the current block is full a larger block is chained in; reset() (or a scope that
//   returns the arena to empty) coalesces blocks so steady state is a single block.
// - Addresses handed out must never influence simulation results (determinism contract).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <vector>

namespace alamo::jobs {

class ScratchArena final : public std::pmr::memory_resource {
public:
    static constexpr size_t kDefaultBlockSize = 256 * 1024;

    explicit ScratchArena(size_t initial_block_size = kDefaultBlockSize);
    ~ScratchArena() override;
    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;

    struct Marker {
        size_t block = 0;
        size_t offset = 0;
    };

    // `align` must be a power of two.
    void* alloc(size_t bytes, size_t align = alignof(std::max_align_t));

    // Uninitialized storage for n objects of a trivially destructible T.
    template <class T>
    T* alloc_array(size_t n) {
        static_assert(std::is_trivially_destructible_v<T>, "scratch never runs destructors");
        return static_cast<T*>(alloc(sizeof(T) * n, alignof(T)));
    }

    Marker mark() const noexcept { return {cur_, off_}; }
    void rewind(Marker m) noexcept;
    void reset() noexcept;  // rewind to empty and coalesce blocks

    size_t bytes_in_use() const noexcept;
    size_t capacity() const noexcept;
    size_t block_count() const noexcept { return blocks_.size(); }
    size_t high_water() const noexcept { return high_water_; }

private:
    struct Block {
        std::byte* data;
        size_t size;
    };
    static Block make_block(size_t size);
    static void free_block(Block b) noexcept;
    void coalesce() noexcept;

    void* do_allocate(size_t bytes, size_t align) override { return alloc(bytes, align); }
    void do_deallocate(void*, size_t, size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::vector<Block> blocks_;
    size_t cur_ = 0;
    size_t off_ = 0;
    size_t high_water_ = 0;
};

// The calling thread's arena.
ScratchArena& thread_scratch();

class ScratchScope {
public:
    explicit ScratchScope(ScratchArena& arena = thread_scratch()) noexcept
        : arena_(arena), mark_(arena.mark()) {}
    ~ScratchScope() { arena_.rewind(mark_); }
    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    ScratchArena& arena() const noexcept { return arena_; }

private:
    ScratchArena& arena_;
    ScratchArena::Marker mark_;
};

}  // namespace alamo::jobs
