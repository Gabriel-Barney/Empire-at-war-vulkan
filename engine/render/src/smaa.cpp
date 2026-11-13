// [port] SMAA 1x: pipelines, targets, lookup textures and the pass recording (smaa.hpp, shaders/smaa_pass.hlsl).
// Three fullscreen-triangle graphics passes, no vertex input; one descriptor set layout of three sampled images and
// two samplers (linear clamp, point clamp) and a 16-byte push block (render-target metrics).
#include "smaa.hpp"

#include <cstring>
#include <initializer_list>
#include <span>

#include "AreaTex.h"    // third_party/smaa: areaTexBytes (RG8, AREATEX_WIDTH x AREATEX_HEIGHT)
#include "SearchTex.h"  // third_party/smaa: searchTexBytes (R8, SEARCHTEX_WIDTH x SEARCHTEX_HEIGHT)
#include "alamo/core/log.hpp"
#include "render_internal.hpp"
#include "smaa_ps_blend.h"
#include "smaa_ps_edge.h"
#include "smaa_ps_weights.h"
#include "smaa_vs.h"

namespace alamo::render {

struct SmaaPass::Push {
    float metrics[4];  // 1 / width, 1 / height, width, height
};

bool SmaaPass::init(rhi::Context& ctx, rhi::Allocator& alloc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    samplers_.init(ctx);
    const rhi::DeviceFns& fn = ctx.vkd();
    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    auto ci = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci.bindingCount = 5;
    ci.pBindings = b;
    rhi::DescriptorPoolRatios ratios;
    ratios.sets = 3;
    ratios.uniform_buffers = 0;
    ratios.combined_image_samplers = 0;
    ratios.storage_buffers = 0;
    ratios.sampled_images = 9;
    ratios.samplers = 6;
    ratios.max_pools = 1;
    if (!cache_.init(ctx) || !pool_.init(ctx, ratios) ||
        fn.vkCreateDescriptorSetLayout(ctx.device(), &ci, nullptr, &set_layout_) != VK_SUCCESS) {
        log::errorf("render", "smaa: layout creation failed");
        shutdown();
        return false;
    }
    for (VkDescriptorSet& s : sets_) s = pool_.allocate(set_layout_);
    const VkDescriptorSetLayout sets[1] = {set_layout_};
    layout_ = rhi::create_pipeline_layout(ctx, sets, sizeof(Push), VK_SHADER_STAGE_FRAGMENT_BIT);
    edge_pipe_ = layout_ ? pipeline(0, rhi::to_vk(rhi::Format::R8G8)) : VK_NULL_HANDLE;
    weights_pipe_ = layout_ ? pipeline(1, rhi::to_vk(rhi::Format::RGBA8)) : VK_NULL_HANDLE;
    if (!layout_ || !edge_pipe_ || !weights_pipe_ || !sets_[0] || !sets_[1] || !sets_[2]) {
        log::errorf("render", "smaa: pipeline / descriptor set creation failed");
        shutdown();
        return false;
    }
    return true;
}

void SmaaPass::shutdown() {
    if (!ctx_) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkDevice dev = ctx_->device();
    for (VkPipeline p : {edge_pipe_, weights_pipe_})
        if (p) fn.vkDestroyPipeline(dev, p, nullptr);
    for (const Blend& bl : blend_)
        if (bl.pipe) fn.vkDestroyPipeline(dev, bl.pipe, nullptr);
    blend_.clear();
    edge_pipe_ = weights_pipe_ = VK_NULL_HANDLE;
    if (layout_) fn.vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set_layout_) fn.vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    for (int i = 0; i < 3; ++i) {
        sets_[i] = VK_NULL_HANDLE;
        set_views_[i] = VK_NULL_HANDLE;
    }
    pool_.shutdown();
    edges_.shutdown();
    weights_.shutdown();
    if (alloc_) {
        if (area_) alloc_->destroy(area_);
        if (search_) alloc_->destroy(search_);
        if (staging_) alloc_->destroy(staging_);
    }
    lookup_ready_ = lookup_failed_ = false;
    cache_.shutdown(false);
    samplers_.shutdown();
    ctx_ = nullptr;
    alloc_ = nullptr;
}

VkPipeline SmaaPass::pipeline(int which, VkFormat color_format) {
    const std::vector<uint32_t> vs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_smaa_vs, sizeof g_smaa_vs));
    std::vector<uint32_t> fs;
    if (which == 0) fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_smaa_ps_edge, sizeof g_smaa_ps_edge));
    else if (which == 1) fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_smaa_ps_weights, sizeof g_smaa_ps_weights));
    else fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_smaa_ps_blend, sizeof g_smaa_ps_blend));
    static const char* const kEntry[3] = {"PSEdge", "PSWeights", "PSBlend"};
    static const char* const kName[3] = {"smaa.edge", "smaa.weights", "smaa.blend"};
    rhi::GraphicsPipelineDesc d;
    d.vs_spirv = vs;
    d.fs_spirv = fs;
    d.vs_entry = "VSFull";
    d.fs_entry = kEntry[which];
    d.raster.cull = VK_CULL_MODE_NONE;
    d.color_formats = std::span<const VkFormat>(&color_format, 1);
    d.layout = layout_;
    d.name = kName[which];
    return cache_.create_graphics(d);
}

bool SmaaPass::ensure_targets(VkExtent2D ext) {
    if (edges_.has_color() && edges_.extent().width == ext.width && edges_.extent().height == ext.height) return true;
    if (edges_.has_color()) ctx_->wait_idle();  // rare (resize): earlier frames may still use the old images
    edges_.shutdown();
    weights_.shutdown();
    rhi::RenderTargetDesc d;
    d.color = rhi::Format::R8G8;
    d.name = "render.smaa_edges";
    rhi::RenderTargetDesc w;
    w.color = rhi::Format::RGBA8;
    w.name = "render.smaa_weights";
    if (!edges_.init(*alloc_, d, ext) || !weights_.init(*alloc_, w, ext)) {
        edges_.shutdown();
        weights_.shutdown();
        log::errorf("render", "smaa: targets (%ux%u) could not be created", ext.width, ext.height);
        return false;
    }
    return true;
}

bool SmaaPass::ensure_lookup(const rhi::DeviceFns& fn, VkCommandBuffer cmd) {
    if (lookup_ready_) return true;
    if (lookup_failed_) return false;
    lookup_failed_ = true;  // until everything below succeeded (logged once)
    rhi::ImageDesc ad;
    ad.format = rhi::Format::R8G8;
    ad.width = AREATEX_WIDTH;
    ad.height = AREATEX_HEIGHT;
    ad.name = "render.smaa_area";
    rhi::ImageDesc sd;
    sd.format = rhi::Format::R8;
    sd.width = SEARCHTEX_WIDTH;
    sd.height = SEARCHTEX_HEIGHT;
    sd.name = "render.smaa_search";
    constexpr VkDeviceSize kArea = AREATEX_SIZE, kSearch = SEARCHTEX_SIZE;
    static_assert(sizeof areaTexBytes == kArea && sizeof searchTexBytes == kSearch, "SMAA lookup texture sizes");
    if (!alloc_->create_image(ad, area_) || !alloc_->create_image(sd, search_) ||
        !alloc_->create_buffer(kArea + kSearch, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::MemoryUsage::Upload, staging_,
                               "render.smaa_staging") ||
        !staging_.mapped()) {
        log::errorf("render", "smaa: lookup textures could not be created");
        return false;
    }
    auto* dst = static_cast<uint8_t*>(staging_.mapped());
    std::memcpy(dst, areaTexBytes, static_cast<size_t>(kArea));
    std::memcpy(dst + kArea, searchTexBytes, static_cast<size_t>(kSearch));
    alloc_->flush(staging_.alloc);
    const VkImageMemoryBarrier2 to_dst[2] = {rhi::transition(area_.handle, rhi::ImageState::Undefined, rhi::ImageState::TransferDst),
                                             rhi::transition(search_.handle, rhi::ImageState::Undefined, rhi::ImageState::TransferDst)};
    rhi::cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(to_dst, 2));
    VkBufferImageCopy r[2]{};
    r[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r[0].imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1};
    r[1] = r[0];
    r[1].bufferOffset = kArea;
    r[1].imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};
    fn.vkCmdCopyBufferToImage(cmd, staging_.handle, area_.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r[0]);
    fn.vkCmdCopyBufferToImage(cmd, staging_.handle, search_.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r[1]);
    const VkImageMemoryBarrier2 to_read[2] = {rhi::transition(area_.handle, rhi::ImageState::TransferDst, rhi::ImageState::ShaderRead),
                                              rhi::transition(search_.handle, rhi::ImageState::TransferDst, rhi::ImageState::ShaderRead)};
    rhi::cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(to_read, 2));
    lookup_ready_ = true;
    lookup_failed_ = false;
    return true;
}

void SmaaPass::write_sets(VkImageView scratch_view) {
    const VkSampler linear = samplers_.get(rhi::SamplerKey::linear_clamp());
    const VkSampler point = samplers_.get(rhi::SamplerKey::point_clamp());
    // Bindings 0..2 per pass; every binding is written (the lookup texture stands in where a pass reads nothing).
    const VkImageView views[3][3] = {{scratch_view, area_.view, area_.view},
                                     {edges_.color().view, area_.view, search_.view},
                                     {scratch_view, weights_.color().view, area_.view}};
    for (int s = 0; s < 3; ++s) {
        VkDescriptorImageInfo ii[5]{};
        for (int i = 0; i < 3; ++i) {
            ii[i].imageView = views[s][i];
            ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        ii[3].sampler = linear;
        ii[4].sampler = point;
        VkWriteDescriptorSet w[5];
        for (uint32_t i = 0; i < 5; ++i) {
            w[i] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            w[i].dstSet = sets_[s];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            w[i].pImageInfo = &ii[i];
        }
        ctx_->vkd().vkUpdateDescriptorSets(ctx_->device(), 5, w, 0, nullptr);
    }
    set_views_[0] = scratch_view;
    set_views_[1] = edges_.color().view;
    set_views_[2] = weights_.color().view;
}

bool SmaaPass::record(VkCommandBuffer cmd, rhi::RenderTarget& scene, rhi::RenderTarget& scratch) {
    if (!ctx_ || !layout_ || !scene.has_color() || !scratch.has_color()) return false;
    const VkExtent2D ext = scene.extent();
    if (scratch.extent().width != ext.width || scratch.extent().height != ext.height ||
        scratch.color().format != scene.color().format)
        return false;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkFormat format = scene.color().vk_format();
    VkPipeline blend = VK_NULL_HANDLE;
    for (const Blend& bl : blend_)
        if (bl.format == format) blend = bl.pipe;
    if (!blend) {
        blend = pipeline(2, format);
        if (!blend) {
            log::errorf("render", "smaa: blend pipeline (format %d) failed", static_cast<int>(format));
            return false;
        }
        blend_.push_back({format, blend});
    }
    if (!ensure_targets(ext) || !ensure_lookup(fn, cmd)) return false;
    if (set_views_[0] != scratch.color().view || set_views_[1] != edges_.color().view ||
        set_views_[2] != weights_.color().view) {
        if (set_views_[0]) ctx_->wait_idle();  // the sets may be bound by frames in flight (rare: resize)
        write_sets(scratch.color().view);
    }

    scratch.copy_from(fn, cmd, detail::copy_of(scene, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                      rhi::ImageState::ShaderRead);
    scene.set_color_state(rhi::ImageState::ColorAttachment);
    Push pc{};
    pc.metrics[0] = 1.0f / static_cast<float>(ext.width);
    pc.metrics[1] = 1.0f / static_cast<float>(ext.height);
    pc.metrics[2] = static_cast<float>(ext.width);
    pc.metrics[3] = static_cast<float>(ext.height);
    auto full = [&](VkPipeline pipe, VkDescriptorSet set) {
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
        fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        fn.vkCmdDraw(cmd, 3, 1, 0, 0);
    };
    rhi::ClearValues zero;
    zero.color[3] = 0.0f;
    edges_.begin_rendering(fn, cmd, &zero);  // pixels without an edge are discarded: cleared to 0
    full(edge_pipe_, sets_[0]);
    edges_.end_rendering(fn, cmd);
    edges_.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    weights_.begin_rendering(fn, cmd, &zero);
    full(weights_pipe_, sets_[1]);
    weights_.end_rendering(fn, cmd);
    weights_.transition_color(fn, cmd, rhi::ImageState::ShaderRead);
    detail::begin_target(fn, cmd, scene, false, nullptr, false, false);
    full(blend, sets_[2]);
    fn.vkCmdEndRendering(cmd);
    return true;
}

}  // namespace alamo::render
