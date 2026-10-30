// Frames in flight (2-3) paced by one timeline semaphore: frame N signals value N, and slot reuse
// waits for the value that slot last signalled. Each slot owns, per recording thread, a command
// pool (primaries + secondaries reused frame to frame) and a DescriptorAllocator, so worker threads
// can record in parallel without locks: thread t only touches its own pools. Use either one
// primary per thread (vkCmdBeginRendering with SUSPENDING/RESUMING flags, submitted in order via
// FrameSubmit::extra) or secondaries with VkCommandBufferInheritanceRenderingInfo executed from the
// main primary. A per-frame host-visible uniform ring (lock-free bump) feeds set 0 constants.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "alamo/rhi/descriptors.hpp"
#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/sync.hpp"

namespace alamo::rhi {

class Context;

struct FrameRingDesc {
    uint32_t frames_in_flight = 2;   // clamped to 1..3
    uint32_t recording_threads = 1;  // per-thread pools per frame
    // Q3: command buffers allocated up front per (frame slot, thread), so steady-state frames never grow the lists (a
    // worker's share of a frame's secondaries depends on job stealing). A frame that needs more still gets them
    // (FrameRing::command_buffer_growth() counts those late allocations).
    uint32_t primaries_per_thread = 4;
    uint32_t secondaries_per_thread = 64;
    VkDeviceSize uniform_bytes = 4ull << 20;  // per frame (uniform + storage allocations); 0 disables the ring
    DescriptorPoolRatios descriptors{};
};

struct UniformAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* ptr = nullptr;
    explicit operator bool() const { return ptr != nullptr; }
};

class Frame {
public:
    uint32_t slot() const { return slot_; }
    uint64_t number() const { return number_; }  // == timeline value this frame signals
    VkCommandBuffer cmd() const { return main_; }  // thread 0's first primary, already begun
    VkSemaphore acquire_semaphore() const { return acquire_; }  // binary, for Swapchain::acquire

    // Thread-local (thread < recording_threads). Returned buffers are begun; the caller ends them.
    VkCommandBuffer primary(uint32_t thread);
    VkCommandBuffer secondary(uint32_t thread, const VkCommandBufferInheritanceRenderingInfo& rendering);
    DescriptorAllocator& descriptors(uint32_t thread = 0) { return threads_[thread].descriptors; }
    // Thread-safe. Aligned to minUniformBufferOffsetAlignment. Empty result when the ring is full.
    UniformAlloc alloc_uniform(VkDeviceSize size);
    // Thread-safe. Aligned to max(minUniform, minStorage)BufferOffsetAlignment, so the range can be
    // bound as a storage buffer (DrawData/MaterialTable/bone palettes) or a uniform buffer.
    UniformAlloc alloc_storage(VkDeviceSize size);

private:
    friend class FrameRing;
    struct ThreadPools {
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> primaries, secondaries;
        uint32_t used_primaries = 0, used_secondaries = 0;
        DescriptorAllocator descriptors;
    };
    VkCommandBuffer next(ThreadPools& t, bool secondary);
    bool preallocate(ThreadPools& t, bool secondary, uint32_t count);

    Context* ctx_ = nullptr;
    std::atomic<uint64_t>* growth_ = nullptr;  // FrameRing::growth_
    uint32_t slot_ = 0;
    uint64_t number_ = 0;
    uint64_t submitted_value_ = 0;
    VkCommandBuffer main_ = VK_NULL_HANDLE;
    VkSemaphore acquire_ = VK_NULL_HANDLE;
    std::vector<ThreadPools> threads_;
    Buffer uniforms_;
    std::atomic<VkDeviceSize> uniform_head_{0};
    UniformAlloc alloc_aligned(VkDeviceSize size, VkDeviceSize align);
    VkDeviceSize uniform_align_ = 256;
    VkDeviceSize storage_align_ = 256;
};

struct FrameSubmit {
    std::span<const VkCommandBuffer> extra;          // ended primaries submitted after cmd(), in order
    std::span<const VkSemaphoreSubmitInfo> waits;     // e.g. acquire semaphore, upload timeline
    std::span<const VkSemaphoreSubmitInfo> signals;   // e.g. Swapchain::render_finished(image)
};

class FrameRing {
public:
    FrameRing() = default;
    ~FrameRing();
    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    bool init(Context& ctx, Allocator& alloc, const FrameRingDesc& desc = {});
    void shutdown();

    // Waits until the next slot's previous use finished, resets its pools/descriptors/uniform ring
    // and begins cmd(). Returns nullptr on error.
    Frame* begin_frame();
    // Ends cmd() and submits it (+extra) on the graphics queue, signalling the frame timeline value.
    bool end_frame(Frame& frame, const FrameSubmit& submit = {});

    bool wait_idle() const;  // waits for every submitted frame
    VkSemaphore timeline() const { return timeline_.handle(); }
    uint64_t completed_value() const { return timeline_.value(); }
    // Timeline value of the most recently begun frame (0 before the first): once completed_value()
    // reaches it, the GPU has finished every frame begun so far. Thread-safe to read, but from any
    // thread other than the one calling begin_frame it may be one frame stale (begin_frame can
    // run concurrently and the new frame may record the slot), so it is a valid deferred-release
    // value (BindlessTextureTable::release(slot, ring.current_value())) only on the begin_frame
    // thread or when the caller serialises with begin_frame.
    uint64_t current_value() const { return frame_number_.load(std::memory_order_seq_cst); }
    // Deferred-release value safe from any thread: current_value() + 1, i.e. the frame being
    // recorded or begun concurrently with this call, or later. Contract: once the caller has read
    // this value, no frame that begins afterwards may record the released resource (unpublish it
    // first, then read retire_value(), then release). Never tagged older than a frame that may use
    // it; costs at most one extra frame of latency over current_value().
    uint64_t retire_value() const { return frame_number_.load(std::memory_order_seq_cst) + 1; }
    uint32_t frames_in_flight() const { return static_cast<uint32_t>(frames_.size()); }
    // Q3 counter: command buffers allocated after init() (a frame needed more primaries / secondaries on a thread than
    // FrameRingDesc preallocated). 0 in steady state; any growth also means a std::vector reallocation.
    uint64_t command_buffer_growth() const { return growth_.load(std::memory_order_relaxed); }

private:
    Context* ctx_ = nullptr;
    Allocator* alloc_ = nullptr;
    TimelineSemaphore timeline_;
    std::vector<std::unique_ptr<Frame>> frames_;
    std::atomic<uint64_t> frame_number_{0};  // written by begin_frame only; read from any thread
    uint64_t last_submitted_ = 0;
    std::atomic<uint64_t> growth_{0};
    // end_frame scratch (kept across frames: no per-frame heap allocation once the high water is reached).
    std::vector<VkCommandBufferSubmitInfo> submit_cmds_;
    std::vector<VkSemaphoreSubmitInfo> submit_signals_;
};

}  // namespace alamo::rhi
