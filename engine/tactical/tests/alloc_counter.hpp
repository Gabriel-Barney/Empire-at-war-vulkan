// Allocation counter for the tactical test executables (counts only while g_count is set). Replaces the global
// operator new/delete of the executable, so include it from exactly one translation unit per executable.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
std::atomic<uint64_t> g_allocs{0};
std::atomic<bool> g_count{false};
}  // namespace
// GCC flags free() inside a replaced operator delete (-Wmismatched-new-delete) although the replaced
// operator new allocates with malloc(); the pairing is correct by construction (as in test_render).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
