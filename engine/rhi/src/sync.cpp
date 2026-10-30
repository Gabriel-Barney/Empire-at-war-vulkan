#include "alamo/rhi/sync.hpp"

#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

StateInfo state_info(ImageState s) {
    switch (s) {
        case ImageState::Undefined: return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        case ImageState::TransferDst:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case ImageState::TransferSrc:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT};
        case ImageState::ShaderRead:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case ImageState::ColorAttachment:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case ImageState::DepthAttachment:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case ImageState::DepthRead:
            return {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case ImageState::Present:
            // Presentation engine reads happen after the semaphore; no stage/access needed.
            return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        case ImageState::General:
            return {VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    }
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                    VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access,
                                    const VkImageSubresourceRange& range) {
    auto b = vk_struct<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    b.srcStageMask = src_stages;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stages;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = range;
    return b;
}

VkImageMemoryBarrier2 transition(VkImage image, ImageState from, ImageState to, const VkImageSubresourceRange& range) {
    const StateInfo a = state_info(from);
    const StateInfo b = state_info(to);
    constexpr VkAccessFlags2 kWrites = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkPipelineStageFlags2 src = a.stages;
    // Read-only source states need no availability operation, only the execution dependency.
    VkAccessFlags2 src_access = a.access & kWrites;
    if (from == ImageState::Present) {
        // The acquire semaphore wait is at COLOR_ATTACHMENT_OUTPUT; chain on that stage.
        src = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = VK_ACCESS_2_NONE;
    } else if (from == ImageState::Undefined) {
        // "Contents don't matter" still has to wait for the previous use of the image, which for
        // per-frame targets (depth buffers, swapchain images, render targets shared across frames
        // in flight) is the same kind of use: wait on the destination state's stages and writes.
        // If the previous use was a different state, transition from that state instead.
        src = b.stages;
        src_access = b.access & kWrites;
        if (to == ImageState::Present) src = VK_PIPELINE_STAGE_2_NONE;
    }
    VkPipelineStageFlags2 dst = b.stages;
    if (to == ImageState::Present) dst = VK_PIPELINE_STAGE_2_NONE;
    return image_barrier(image, a.layout, b.layout, src, src_access, dst, b.access, range);
}

VkBufferMemoryBarrier2 buffer_barrier(VkBuffer buffer, VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                      VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access, VkDeviceSize offset,
                                      VkDeviceSize size) {
    auto b = vk_struct<VkBufferMemoryBarrier2>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
    b.srcStageMask = src_stages;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stages;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.offset = offset;
    b.size = size;
    return b;
}

VkMemoryBarrier2 memory_barrier(VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_access,
                                VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_access) {
    auto b = vk_struct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    b.srcStageMask = src_stages;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stages;
    b.dstAccessMask = dst_access;
    return b;
}

void cmd_barrier(const DeviceFns& fn, VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> images,
                 std::span<const VkBufferMemoryBarrier2> buffers, std::span<const VkMemoryBarrier2> memory) {
    if (images.empty() && buffers.empty() && memory.empty()) return;
    auto di = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    di.memoryBarrierCount = static_cast<uint32_t>(memory.size());
    di.pMemoryBarriers = memory.data();
    di.bufferMemoryBarrierCount = static_cast<uint32_t>(buffers.size());
    di.pBufferMemoryBarriers = buffers.data();
    di.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
    di.pImageMemoryBarriers = images.data();
    fn.vkCmdPipelineBarrier2(cmd, &di);
}

// ---------------------------------------------------------------------------------------------

TimelineSemaphore::~TimelineSemaphore() { shutdown(); }

bool TimelineSemaphore::init(Context& ctx, uint64_t initial, const char* name) {
    shutdown();
    ctx_ = &ctx;
    auto ti = vk_struct<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    ti.initialValue = initial;
    auto ci = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    ci.pNext = &ti;
    if (!vk_check(ctx.vkd().vkCreateSemaphore(ctx.device(), &ci, nullptr, &sem_), "vkCreateSemaphore(timeline)")) {
        sem_ = VK_NULL_HANDLE;
        return false;
    }
    if (name) ctx.set_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(sem_), name);
    return true;
}

void TimelineSemaphore::shutdown() {
    if (sem_) ctx_->vkd().vkDestroySemaphore(ctx_->device(), sem_, nullptr);
    sem_ = VK_NULL_HANDLE;
}

uint64_t TimelineSemaphore::value() const {
    uint64_t v = 0;
    if (sem_) ctx_->vkd().vkGetSemaphoreCounterValue(ctx_->device(), sem_, &v);
    return v;
}

bool TimelineSemaphore::wait(uint64_t value, uint64_t timeout_ns) const {
    if (!sem_) return false;
    auto wi = vk_struct<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem_;
    wi.pValues = &value;
    const VkResult r = ctx_->vkd().vkWaitSemaphores(ctx_->device(), &wi, timeout_ns);
    if (r == VK_TIMEOUT) return false;
    return vk_check(r, "vkWaitSemaphores");
}

bool TimelineSemaphore::signal(uint64_t value) {
    auto si = vk_struct<VkSemaphoreSignalInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
    si.semaphore = sem_;
    si.value = value;
    return vk_check(ctx_->vkd().vkSignalSemaphore(ctx_->device(), &si), "vkSignalSemaphore");
}

}  // namespace alamo::rhi
