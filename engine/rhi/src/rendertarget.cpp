#include "alamo/rhi/rendertarget.hpp"

#include <algorithm>

#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

void cmd_stretch_rect(const DeviceFns& fn, VkCommandBuffer cmd, const CopyImage& src, const CopyImage& dst,
                      VkFilter filter) {
    const VkImageSubresourceRange src_range{src.aspect, 0, 1, 0, 1};
    const VkImageSubresourceRange dst_range{dst.aspect, 0, 1, 0, 1};
    VkImageMemoryBarrier2 pre[2];
    uint32_t n = 0;
    if (src.before != ImageState::TransferSrc) pre[n++] = transition(src.image, src.before, ImageState::TransferSrc, src_range);
    // dst contents are fully overwritten: discard (Undefined) unless the caller says otherwise.
    if (dst.before != ImageState::TransferDst) pre[n++] = transition(dst.image, dst.before, ImageState::TransferDst, dst_range);
    cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(pre, n));

    const bool same = src.extent.width == dst.extent.width && src.extent.height == dst.extent.height &&
                      src.format == dst.format;
    if (same) {
        auto region = vk_struct<VkImageCopy2>(VK_STRUCTURE_TYPE_IMAGE_COPY_2);
        region.srcSubresource = {src.aspect, 0, 0, 1};
        region.dstSubresource = {dst.aspect, 0, 0, 1};
        region.extent = {src.extent.width, src.extent.height, 1};
        auto ci = vk_struct<VkCopyImageInfo2>(VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2);
        ci.srcImage = src.image;
        ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ci.dstImage = dst.image;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = 1;
        ci.pRegions = &region;
        fn.vkCmdCopyImage2(cmd, &ci);
    } else {
        auto region = vk_struct<VkImageBlit2>(VK_STRUCTURE_TYPE_IMAGE_BLIT_2);
        region.srcSubresource = {src.aspect, 0, 0, 1};
        region.dstSubresource = {dst.aspect, 0, 0, 1};
        region.srcOffsets[1] = {static_cast<int32_t>(src.extent.width), static_cast<int32_t>(src.extent.height), 1};
        region.dstOffsets[1] = {static_cast<int32_t>(dst.extent.width), static_cast<int32_t>(dst.extent.height), 1};
        auto bi = vk_struct<VkBlitImageInfo2>(VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2);
        bi.srcImage = src.image;
        bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bi.dstImage = dst.image;
        bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bi.regionCount = 1;
        bi.pRegions = &region;
        bi.filter = filter;
        fn.vkCmdBlitImage2(cmd, &bi);
    }

    VkImageMemoryBarrier2 post[2];
    n = 0;
    if (src.after != ImageState::TransferSrc) post[n++] = transition(src.image, ImageState::TransferSrc, src.after, src_range);
    if (dst.after != ImageState::TransferDst) post[n++] = transition(dst.image, ImageState::TransferDst, dst.after, dst_range);
    cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(post, n));
}

// ---------------------------------------------------------------------------------------------

RenderTarget::~RenderTarget() { shutdown(); }

bool RenderTarget::init(Allocator& alloc, const RenderTargetDesc& desc, VkExtent2D reference) {
    shutdown();
    alloc_ = &alloc;
    desc_ = desc;
    extent_ = {0, 0};
    return resize(reference) || has_color() || has_depth();
}

bool RenderTarget::resize(VkExtent2D reference) {
    if (!alloc_) return false;
    VkExtent2D e{desc_.width, desc_.height};
    if (!desc_.width || !desc_.height) {
        e.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(reference.width) * desc_.scale));
        e.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(reference.height) * desc_.scale));
    }
    if (e.width == extent_.width && e.height == extent_.height && (has_color() || has_depth())) return false;
    destroy_images();
    extent_ = e;
    return create();
}

bool RenderTarget::create() {
    if (desc_.color != Format::Undefined) {
        ImageDesc id;
        id.format = desc_.color;
        id.width = extent_.width;
        id.height = extent_.height;
        id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | desc_.extra_color_usage;
        id.name = desc_.name;
        if (!alloc_->create_image(id, color_)) return false;
    }
    if (desc_.depth != Format::Undefined) {
        ImageDesc id;
        id.format = desc_.depth;
        id.width = extent_.width;
        id.height = extent_.height;
        id.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (desc_.sample_depth) id.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        id.name = desc_.name;
        if (!alloc_->create_image(id, depth_)) {
            destroy_images();
            return false;
        }
    }
    color_state_ = depth_state_ = ImageState::Undefined;
    return true;
}

void RenderTarget::destroy_images() {
    if (!alloc_) return;
    if (color_) alloc_->destroy(color_);
    if (depth_) alloc_->destroy(depth_);
    color_state_ = depth_state_ = ImageState::Undefined;
}

void RenderTarget::shutdown() {
    destroy_images();
    alloc_ = nullptr;
    extent_ = {0, 0};
}

void RenderTarget::transition_color(const DeviceFns& fn, VkCommandBuffer cmd, ImageState to) {
    if (!has_color() || color_state_ == to) return;
    cmd_barrier(fn, cmd, transition(color_.handle, color_state_, to, color_.range()));
    color_state_ = to;
}

void RenderTarget::transition_depth(const DeviceFns& fn, VkCommandBuffer cmd, ImageState to) {
    if (!has_depth() || depth_state_ == to) return;
    cmd_barrier(fn, cmd, transition(depth_.handle, depth_state_, to, depth_.range()));
    depth_state_ = to;
}

void RenderTarget::begin_rendering(const DeviceFns& fn, VkCommandBuffer cmd, const ClearValues* clear) {
    // Always from the tracked state (not Undefined) so earlier sampling of this target (bloom
    // ping-pong, SceneTexture reuse) is ordered before the new writes.
    VkImageMemoryBarrier2 b[2];
    uint32_t n = 0;
    if (has_color() && color_state_ != ImageState::ColorAttachment) {
        b[n++] = transition(color_.handle, color_state_, ImageState::ColorAttachment,
                            color_.range());
        color_state_ = ImageState::ColorAttachment;
    }
    if (has_depth() && depth_state_ != ImageState::DepthAttachment) {
        b[n++] = transition(depth_.handle, depth_state_, ImageState::DepthAttachment,
                            depth_.range());
        depth_state_ = ImageState::DepthAttachment;
    }
    cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(b, n));

    const VkAttachmentLoadOp load = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    auto ca = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = color_.attachment_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = load;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear) ca.clearValue.color = {{clear->color[0], clear->color[1], clear->color[2], clear->color[3]}};
    auto da = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    da.imageView = depth_.attachment_view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = load;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear) da.clearValue.depthStencil = {clear->depth, clear->stencil};

    auto ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = has_color() ? 1u : 0u;
    ri.pColorAttachments = has_color() ? &ca : nullptr;
    const FormatInfo& dfi = format_info(desc_.depth);
    if (has_depth() && dfi.depth) ri.pDepthAttachment = &da;
    if (has_depth() && dfi.stencil) ri.pStencilAttachment = &da;
    fn.vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    const VkRect2D sc{{0, 0}, extent_};
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
}

void RenderTarget::copy_from(const DeviceFns& fn, VkCommandBuffer cmd, const CopyImage& src, VkFilter filter,
                             ImageState after) {
    if (!has_color()) return;
    CopyImage dst;
    dst.image = color_.handle;
    dst.extent = extent_;
    dst.format = color_.vk_format();
    dst.before = color_state_;  // tracked state: also orders after earlier reads (WAR)
    dst.after = after;
    cmd_stretch_rect(fn, cmd, src, dst, filter);
    color_state_ = after;
}

}  // namespace alamo::rhi
