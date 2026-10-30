// synchronization2 helpers: state-based image transitions, raw barrier builders and a timeline
// semaphore wrapper.
#pragma once
#include <cstdint>
#include <span>

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

// Common image usages. Each maps to (layout, stages, accesses) for barrier building.
// transition(from, to): from = Undefined discards contents but still waits for the previous use of
// the same kind (to's stages/writes), so a depth buffer or swapchain image reused across frames in
// flight is safe. If the previous use was a different state, pass that state as rom.
enum class ImageState : uint8_t {
    Undefined,
    TransferDst,
    TransferSrc,
    ShaderRead,          // sampled in vertex/fragment/compute
    ColorAttachment,
    DepthAttachment,     // depth/stencil read+write
    DepthRead,           // depth test only / sampled depth
    Present,
    General,             // storage image
};

struct StateInfo {
    VkImageLayout layout;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
};
StateInfo state_info(ImageState s);

inline VkImageSubresourceRange color_range(uint32_t mips = VK_REMAINING_MIP_LEVELS,
                                           uint32_t layers = VK_REMAINING_ARRAY_LAYERS) {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                    VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access,
                                    const VkImageSubresourceRange& range = color_range());
VkImageMemoryBarrier2 transition(VkImage image, ImageState from, ImageState to,
                                 const VkImageSubresourceRange& range = color_range());
VkBufferMemoryBarrier2 buffer_barrier(VkBuffer buffer, VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                      VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access,
                                      VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
VkMemoryBarrier2 memory_barrier(VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access);

// One vkCmdPipelineBarrier2 with any mix of barriers (empty spans allowed; no-op when all empty).
void cmd_barrier(const DeviceFns& fn, VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> images,
                 std::span<const VkBufferMemoryBarrier2> buffers = {}, std::span<const VkMemoryBarrier2> memory = {});
inline void cmd_barrier(const DeviceFns& fn, VkCommandBuffer cmd, const VkImageMemoryBarrier2& image) {
    cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(&image, 1));
}

// Timeline semaphore (Vulkan 1.2 core). Values only increase.
class TimelineSemaphore {
public:
    TimelineSemaphore() = default;
    ~TimelineSemaphore();
    TimelineSemaphore(const TimelineSemaphore&) = delete;
    TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;

    bool init(Context& ctx, uint64_t initial = 0, const char* name = nullptr);
    void shutdown();
    VkSemaphore handle() const { return sem_; }
    uint64_t value() const;  // current GPU-side counter
    bool wait(uint64_t value, uint64_t timeout_ns = UINT64_MAX) const;  // false on timeout/error
    bool signal(uint64_t value);  // host signal

private:
    Context* ctx_ = nullptr;
    VkSemaphore sem_ = VK_NULL_HANDLE;
};

// VkSemaphoreSubmitInfo shorthand.
inline VkSemaphoreSubmitInfo semaphore_info(VkSemaphore s, uint64_t value, VkPipelineStageFlags2 stages) {
    auto i = vk_struct<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    i.semaphore = s;
    i.value = value;
    i.stageMask = stages;
    return i;
}

}  // namespace alamo::rhi
