// Staging uploads (buffers and mip chains) through a host-visible ring, executed on the transfer
// queue and completed through a timeline semaphore. Every public call is thread-safe, so asset
// streaming jobs can upload from worker threads while the frame loop runs.
//
// Data flow per call: ring space is reserved under the lock, the bytes are memcpy'd without the
// lock, then the copies are recorded and submitted under the lock with a new timeline value. The
// returned ticket is that value. Large subresources are split into block-row chunks; when the ring
// is full the caller submits what it has and blocks until the GPU frees space.
//
// Queue ownership: with a dedicated transfer family the upload ends in a queue-family *release*;
// the matching *acquire* must run on the graphics queue. Once per frame, in the first command
// buffer of the frame, call record_acquires(cmd): it records the acquires of every upload that has
// already completed (it never stalls) and returns the timeline value the frame's submit must wait
// on. A resource may be used by the GPU once ready(ticket) is true. With a single queue family the
// upload ends in the final layout transition and ready() is true as soon as the ticket exists.
//
// Contracts:
//  - The destination must not be in use by the GPU while it is uploaded to (initial uploads, or
//    resources the caller has fenced). upload_image always discards the listed subresources.
//  - With the dedicated transfer queue, a buffer that was already uploaded through this Uploader
//    (its release to graphics was submitted, acquired yet or not) can only be overwritten whole;
//    partial re-uploads are rejected until forget(buffer) (use an Uploader with
//    use_transfer_queue = false for dynamic data).
//  - Failure is all-or-nothing: if any submit of a multi-chunk upload fails, or waiting for ring
//    space fails (device lost), the call returns ticket 0 and records nothing more; chunks already
//    submitted may have landed. Callers never spin on a lost device.
//  - Before destroying a resource that was uploaded through this Uploader, call forget(handle): it
//    waits for any in-flight copy into it and drops its pending acquire, so record_acquires never
//    records a barrier on a freed handle.
//  - A failed submit returns ticket 0; nothing is host-signalled.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/ring.hpp"
#include "alamo/rhi/sync.hpp"

namespace alamo::rhi {

class Context;

struct SubresourceData {
    uint32_t mip = 0, layer = 0;
    std::span<const uint8_t> bytes;  // tightly packed block rows, exactly subresource_size() bytes;
                                     // 3D images: all slices of the mip, slice 0 first (volume_mip_size)
};

struct UploadTicket {
    uint64_t value = 0;  // 0 = nothing submitted (error or empty upload)
    explicit operator bool() const { return value != 0; }
};

struct UploaderDesc {
    uint64_t ring_bytes = 64ull << 20;
    bool use_transfer_queue = true;  // false: upload on the graphics queue even if a transfer family exists
};

struct AcquireResult {
    uint64_t wait_value = 0;  // wait on timeline() >= wait_value at the frame submit (0 = no wait)
    uint32_t barriers = 0;    // acquire barriers recorded
};

class Uploader {
public:
    Uploader() = default;
    ~Uploader();
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    bool init(Context& ctx, Allocator& alloc, const UploaderDesc& desc = {});
    void shutdown();  // waits for outstanding uploads

    // dst_stages/access: how the graphics side will read the buffer.
    UploadTicket upload_buffer(const Buffer& dst, VkDeviceSize dst_offset, std::span<const uint8_t> data,
                               VkPipelineStageFlags2 dst_stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VkAccessFlags2 dst_access = VK_ACCESS_2_MEMORY_READ_BIT);
    // Initial upload: every listed subresource is transitioned from UNDEFINED (previous contents are
    // discarded) and ends in final_state. Unlisted subresources are left untouched.
    UploadTicket upload_image(const Image& dst, std::span<const SubresourceData> subresources,
                              ImageState final_state = ImageState::ShaderRead);
    // All mips of all layers in one span, layer-major then mip 0 first (the mip_chain_size layout);
    // 3D images: mip 0 first, each mip's slices packed (volume_chain_size layout).
    UploadTicket upload_image_chain(const Image& dst, std::span<const uint8_t> data,
                                    ImageState final_state = ImageState::ShaderRead);

    bool is_complete(UploadTicket t) const;
    bool wait(UploadTicket t, uint64_t timeout_ns = UINT64_MAX) const;
    bool ready(UploadTicket t) const;  // usable by graphics work recorded after the last record_acquires

    AcquireResult record_acquires(VkCommandBuffer graphics_cmd);
    // Call before destroying a resource uploaded through this Uploader (see contracts above).
    void forget(VkBuffer buffer);
    void forget(VkImage image);

    // Blocking helper for tools/tests: wait for t, then run the acquires on the graphics queue.
    bool finish(UploadTicket t);

    VkSemaphore timeline() const { return timeline_.handle(); }
    bool dedicated_queue() const { return dedicated_; }

    // Test hook (device-loss simulation). The next `fail_submits` queue submits fail without
    // reaching the queue (as vkQueueSubmit2 returning an error). fail_waits: the upload timeline
    // looks stuck (no ring space retires) and every wait for ring space fails, as on
    // VK_ERROR_DEVICE_LOST. Call with (0, false) to clear.
    void inject_faults_for_testing(uint32_t fail_submits, bool fail_waits = false);

private:
    struct Record {
        uint64_t end = 0;     // ring marker after this allocation
        uint64_t value = 0;   // timeline value of the submit that reads it (0 = not submitted yet)
    };
    enum class OpKind : uint8_t { ImagePre, ImageCopy, ImagePost, BufferCopy, BufferPost };
    struct Op {
        OpKind kind = OpKind::BufferCopy;
        VkImage image = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImageSubresourceRange range{};
        ImageState final_state = ImageState::ShaderRead;
        VkBufferImageCopy image_copy{};
        VkBufferCopy buffer_copy{};
        VkPipelineStageFlags2 dst_stages = 0;
        VkAccessFlags2 dst_access = 0;
    };
    struct Job {
        std::vector<Op> ops;
        std::vector<Record*> records;
    };
    struct CmdSlot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t value = 0;
    };
    struct PendingAcquire {
        uint64_t value;
        bool is_image;
        VkImageMemoryBarrier2 image;
        VkBufferMemoryBarrier2 buffer;
    };

    // Reserves ring space for `size`; may submit `job` first and block. Returns the offset.
    bool reserve(Job& job, uint64_t size, uint64_t& offset);
    uint64_t submit(Job& job);  // records + submits job (lock held), returns the value (0 if empty)
    uint64_t submit_locked(Job& job);
    void abandon(Job& job);  // releases job's ring reservations without submitting
    void retire_locked();
    void forget_handle(uint64_t handle, bool is_image);

    Context* ctx_ = nullptr;
    Allocator* alloc_ = nullptr;
    Buffer ring_buffer_;
    uint8_t* ring_ptr_ = nullptr;
    RingAllocator ring_;
    TimelineSemaphore timeline_;
    bool dedicated_ = false;
    uint32_t src_family_ = 0, dst_family_ = 0;
    uint64_t chunk_limit_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable submitted_cv_;
    std::deque<Record> records_;
    uint64_t next_value_ = 0;
    uint64_t acquired_through_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<CmdSlot> cmds_;
    std::vector<PendingAcquire> pending_;
    std::unordered_map<uint64_t, uint64_t> last_use_;  // handle -> last timeline value that wrote it
    std::unordered_set<uint64_t> graphics_owned_;      // buffers released to graphics (dedicated mode)
    uint32_t fault_submits_ = 0;                       // inject_faults_for_testing
    bool fault_waits_ = false;
};

}  // namespace alamo::rhi
