// FIFO ring sub-allocator (pure CPU bookkeeping, no Vulkan). Used for the staging upload ring and
// per-frame transient memory. Positions are monotonic 64-bit "virtual" offsets; the physical
// offset is virtual % capacity. An allocation never straddles the end: it wraps to 0 and the tail
// gap counts as used until retired. Not thread-safe (callers lock).
#pragma once
#include <cstdint>
#include <optional>

namespace alamo::rhi {

class RingAllocator {
public:
    RingAllocator() = default;
    explicit RingAllocator(uint64_t capacity) : capacity_(capacity) {}
    void reset(uint64_t capacity) {
        capacity_ = capacity;
        head_ = tail_ = 0;
    }

    // Physical offset, or nullopt when there is not enough free space (or size > capacity).
    // align must be a power of two.
    std::optional<uint64_t> allocate(uint64_t size, uint64_t align = 1) {
        if (!capacity_ || size > capacity_) return std::nullopt;
        if (head_ == tail_ && head_ % capacity_) {
            // Empty: restart at physical 0 (keeps positions monotonic) so a large request never
            // fails just because the head sits mid-ring.
            head_ = tail_ = (head_ / capacity_ + 1) * capacity_;
        }
        const uint64_t phys = head_ % capacity_;
        uint64_t pad = ((phys + align - 1) & ~(align - 1)) - phys;
        uint64_t start = phys + pad;
        if (start + size > capacity_) {  // wrap
            pad = capacity_ - phys;
            start = 0;
        }
        if (used() + pad + size > capacity_) return std::nullopt;
        head_ += pad + size;
        return start;
    }

    // Marker for everything allocated so far; pass it to retire() once the GPU is done with it.
    uint64_t marker() const { return head_; }
    // Frees everything allocated before `marker` (markers must be retired in order).
    void retire(uint64_t marker) {
        if (marker > tail_ && marker <= head_) tail_ = marker;
    }

    uint64_t used() const { return head_ - tail_; }
    uint64_t capacity() const { return capacity_; }
    bool empty() const { return head_ == tail_; }

private:
    uint64_t capacity_ = 0;
    uint64_t head_ = 0, tail_ = 0;
};

}  // namespace alamo::rhi
