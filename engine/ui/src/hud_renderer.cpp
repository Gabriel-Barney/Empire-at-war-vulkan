#include "alamo/ui/hud_renderer.hpp"

#include <algorithm>
#include <cstring>

#include "alamo/assets/texture.hpp"
#include "alamo/core/log.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/rhi/context.hpp"
#include "alamo/rhi/upload.hpp"
#include "alamo/ui/hud_font.hpp"
#include "hud_ps.h"
#include "hud_vs.h"

namespace alamo::ui {

namespace {
struct HudPush {
    float xform[4];
    uint32_t page;
    uint32_t pad[3];
};
static_assert(sizeof(HudPush) == 32);
}  // namespace

render::ViewMatrices hud_ortho_view(const HudCanvas& canvas, HudCameraId camera) {
    const HudOrthoCamera& c = canvas.cameras[camera < kHudCameraCount ? camera : 0];
    render::ViewMatrices v;
    v.view = render::Mat4::identity();
    v.view.m[2][3] = -c.eye_z();  // camera at (0, 0, eye_z) looking down -Z
    const float range = c.zfar - c.znear;
    render::Mat4 p;
    p.m[0][0] = 1.0f / c.half_width;
    p.m[1][1] = 1.0f / c.half_height;
    p.m[2][2] = -1.0f / range;
    p.m[2][3] = -c.znear / range;
    v.proj = p;
    v.view_proj = p * v.view;
    v.view_inv = v.view.inverse_affine();
    v.eye = {0.0f, 0.0f, c.eye_z()};
    v.width = canvas.screen_width;
    v.height = canvas.screen_height;
    return v;
}

HudRenderer::~HudRenderer() { shutdown(); }

bool HudRenderer::ensure_buffer(rhi::Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage, rhi::MemoryUsage mem) {
    if (b && b.size >= bytes) return true;
    if (b) alloc_->destroy(b);
    VkDeviceSize size = 64 * 1024;
    while (size < bytes) size *= 2;
    return alloc_->create_buffer(size, usage, mem, b, "hud");
}

bool HudRenderer::create_pipelines() {
    const rhi::DeviceFns& fn = ctx_->vkd();
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binds[0].descriptorCount = kHudMaxPages;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    auto li = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    li.bindingCount = 2;
    li.pBindings = binds;
    if (fn.vkCreateDescriptorSetLayout(ctx_->device(), &li, nullptr, &set_layout_) != VK_SUCCESS) return false;
    layout_ = rhi::create_pipeline_layout(*ctx_, std::span<const VkDescriptorSetLayout>(&set_layout_, 1), sizeof(HudPush),
                                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!layout_) return false;

    const std::vector<uint32_t> vs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_hud_vs, sizeof g_hud_vs));
    const std::vector<uint32_t> ps = rhi::spirv_from_bytes(std::span<const uint8_t>(g_hud_ps, sizeof g_hud_ps));
    const rhi::VertexBinding vb{0, sizeof(HudVertex), false};
    const rhi::VertexAttribute attrs[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HudVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(HudVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(HudVertex, color)},
    };
    const VkFormat colors[1] = {desc_.color_format};
    // One pipeline per HUD-list effect (STRATEGIC_GAPS §7.2): 0 PrimOpaque, 1 PrimAdditive, 2 PrimAlpha, 3 PrimModulate.
    for (uint32_t b = 0; b < pipelines_.size(); ++b) {
        rhi::GraphicsPipelineDesc d;
        d.vs_spirv = vs;
        d.fs_spirv = ps;
        d.vs_entry = "VSMain";
        d.fs_entry = "PSMain";
        d.bindings = std::span<const rhi::VertexBinding>(&vb, 1);
        d.attributes = attrs;
        d.raster.cull = VK_CULL_MODE_NONE;
        d.depth.depth_test = false;
        d.depth.depth_write = false;
        d.blend.enable = true;
        switch (static_cast<HudEffect>(b)) {
            case HudEffect::PrimAlpha:  // SRCALPHA / INVSRCALPHA, alpha = tex x diffuse (straight alpha)
                d.blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
                d.blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                d.blend.src_alpha = VK_BLEND_FACTOR_ONE;
                d.blend.dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                break;
            case HudEffect::PrimAdditive:  // ONE / ONE, tex x diffuse
                d.blend.src_color = VK_BLEND_FACTOR_ONE;
                d.blend.dst_color = VK_BLEND_FACTOR_ONE;
                d.blend.src_alpha = VK_BLEND_FACTOR_ZERO;
                d.blend.dst_alpha = VK_BLEND_FACTOR_ONE;
                break;
            case HudEffect::PrimModulate:  // ZERO / SRCCOLOR (dst x src), colour = tex + diffuse (shader mode 1)
                d.blend.src_color = VK_BLEND_FACTOR_ZERO;
                d.blend.dst_color = VK_BLEND_FACTOR_SRC_COLOR;
                d.blend.src_alpha = VK_BLEND_FACTOR_ZERO;
                d.blend.dst_alpha = VK_BLEND_FACTOR_ONE;
                break;
            default:  // PrimOpaque: no blending
                d.blend.enable = false;
                break;
        }
        d.color_formats = colors;
        d.depth_format = desc_.depth_format;
        d.layout = layout_;
        d.name = "hud";
        pipelines_[b] = cache_.create_graphics(d);
        if (!pipelines_[b]) return false;
    }
    return true;
}

bool HudRenderer::init(rhi::Context& ctx, rhi::Allocator& alloc, rhi::Uploader& up, const HudRendererDesc& desc) {
    ctx_ = &ctx;
    alloc_ = &alloc;
    up_ = &up;
    desc_ = desc;
    if (!cache_.init(ctx)) return false;
    if (!create_pipelines()) {
        log::errorf("ui", "HUD pipelines could not be created");
        return false;
    }
    auto si = rhi::vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (ctx.vkd().vkCreateSampler(ctx.device(), &si, nullptr, &sampler_) != VK_SUCCESS) return false;
    const uint8_t white[4] = {255, 255, 255, 255};
    rhi::ImageDesc id;
    id.format = rhi::Format::RGBA8;
    id.width = id.height = 1;
    id.name = "hud_white";
    if (!alloc.create_image(id, white_)) return false;
    const rhi::SubresourceData sd{0, 0, std::span<const uint8_t>(white, 4)};
    if (!up.finish(up.upload_image(white_, std::span<const rhi::SubresourceData>(&sd, 1)))) return false;
    rhi::DescriptorPoolRatios r;
    r.sets = 16;
    r.uniform_buffers = 1;
    r.combined_image_samplers = 1;
    r.storage_buffers = 1;
    r.sampled_images = 16 * kHudMaxPages;
    r.samplers = 16;
    r.max_pools = 4;
    for (Slot& s : slots_) {
        if (!s.desc.init(ctx, r)) return false;
        s.desc_ready = true;
    }
    return true;
}

void HudRenderer::shutdown() {
    if (!ctx_) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    for (Slot& s : slots_) {
        if (s.vb) alloc_->destroy(s.vb);
        if (s.ib) alloc_->destroy(s.ib);
        if (s.staging) alloc_->destroy(s.staging);
        if (s.staging_dynamic) alloc_->destroy(s.staging_dynamic);
        if (s.desc_ready) s.desc.shutdown();
        s.desc_ready = false;
    }
    for (Page& p : font_pages_)
        if (p.image) alloc_->destroy(p.image);
    font_pages_.clear();
    for (DynamicPage& d : dynamic_) {
        if (d.image) alloc_->destroy(d.image);
        d = DynamicPage{};
    }
    for (rhi::Image& im : images_)
        if (im) {
            up_->forget(im.handle);
            alloc_->destroy(im);
        }
    for (Retired& r : retired_) alloc_->destroy(r.image);
    retired_.clear();
    if (atlas_) {
        up_->forget(atlas_.handle);
        alloc_->destroy(atlas_);
    }
    if (white_) {
        up_->forget(white_.handle);
        alloc_->destroy(white_);
    }
    for (VkPipeline& p : pipelines_)
        if (p) {
            fn.vkDestroyPipeline(ctx_->device(), p, nullptr);
            p = VK_NULL_HANDLE;
        }
    if (layout_) fn.vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
    if (set_layout_) fn.vkDestroyDescriptorSetLayout(ctx_->device(), set_layout_, nullptr);
    if (sampler_) fn.vkDestroySampler(ctx_->device(), sampler_, nullptr);
    layout_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    cache_.shutdown(false);
    ctx_ = nullptr;
}

bool HudRenderer::set_atlas_rgba(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) {
    if (!ctx_ || rgba.size() < static_cast<size_t>(width) * height * 4) return false;
    if (atlas_) {
        up_->forget(atlas_.handle);
        alloc_->destroy(atlas_);
    }
    rhi::ImageDesc id;
    id.format = rhi::Format::RGBA8;
    id.width = width;
    id.height = height;
    id.name = "hud_atlas";
    if (!alloc_->create_image(id, atlas_)) return false;
    const rhi::SubresourceData sd{0, 0, rgba.first(static_cast<size_t>(width) * height * 4)};
    return up_->finish(up_->upload_image(atlas_, std::span<const rhi::SubresourceData>(&sd, 1)));
}

bool HudRenderer::set_atlas_texture(const assets::TextureAsset& tex) {
    if (!ctx_ || tex.levels.empty()) return false;
    const rhi::Format f = render::to_rhi_format(tex.format);
    rhi::Swizzle sw = rhi::Swizzle::Identity;
    if (f == rhi::Format::Undefined || !render::to_rhi_swizzle(tex.swizzle, sw)) return false;
    if (atlas_) {
        up_->forget(atlas_.handle);
        alloc_->destroy(atlas_);
    }
    rhi::ImageDesc id;
    id.format = f;
    id.width = tex.width;
    id.height = tex.height;
    id.swizzle = sw;
    uint32_t mips = 0;
    for (const assets::TextureLevel& l : tex.levels)
        if (l.layer == 0) mips = std::max(mips, l.mip + 1);
    id.mips = mips;
    id.name = "hud_atlas";
    if (!alloc_->create_image(id, atlas_)) return false;
    std::vector<rhi::SubresourceData> subs;
    for (const assets::TextureLevel& l : tex.levels)
        if (l.layer == 0) subs.push_back({l.mip, 0, tex.level_bytes(l)});
    return up_->finish(up_->upload_image(atlas_, subs));
}

bool HudRenderer::replace_image(uint16_t page, rhi::Image img) {
    rhi::Image& slot = images_[page - kHudPageImageBase];
    if (slot) {
        up_->forget(slot.handle);
        retired_.push_back({std::move(slot), records_});
    }
    slot = std::move(img);
    return true;
}

bool HudRenderer::set_image_rgba(uint16_t page, uint32_t width, uint32_t height, std::span<const uint8_t> rgba) {
    if (!ctx_ || page < kHudPageImageBase || page >= kHudPageCount || width == 0 || height == 0 ||
        rgba.size() < static_cast<size_t>(width) * height * 4)
        return false;
    rhi::Image img;
    rhi::ImageDesc id;
    id.format = rhi::Format::RGBA8;
    id.width = width;
    id.height = height;
    id.name = "hud_image";
    if (!alloc_->create_image(id, img)) return false;
    const rhi::SubresourceData sd{0, 0, rgba.first(static_cast<size_t>(width) * height * 4)};
    if (!up_->finish(up_->upload_image(img, std::span<const rhi::SubresourceData>(&sd, 1)))) {
        alloc_->destroy(img);
        return false;
    }
    return replace_image(page, std::move(img));
}

bool HudRenderer::set_image_page(uint16_t page, const assets::TextureAsset& tex) {
    if (!ctx_ || page < kHudPageImageBase || page >= kHudPageCount || tex.levels.empty()) return false;
    const rhi::Format f = render::to_rhi_format(tex.format);
    rhi::Swizzle sw = rhi::Swizzle::Identity;
    if (f == rhi::Format::Undefined || !render::to_rhi_swizzle(tex.swizzle, sw)) return false;
    rhi::Image img;
    rhi::ImageDesc id;
    id.format = f;
    id.width = tex.width;
    id.height = tex.height;
    id.swizzle = sw;
    uint32_t mips = 0;
    for (const assets::TextureLevel& l : tex.levels)
        if (l.layer == 0) mips = std::max(mips, l.mip + 1);
    id.mips = mips;
    id.name = "hud_image";
    if (!alloc_->create_image(id, img)) return false;
    std::vector<rhi::SubresourceData> subs;
    for (const assets::TextureLevel& l : tex.levels)
        if (l.layer == 0) subs.push_back({l.mip, 0, tex.level_bytes(l)});
    if (!up_->finish(up_->upload_image(img, subs))) {
        alloc_->destroy(img);
        return false;
    }
    return replace_image(page, std::move(img));
}

void HudRenderer::clear_image_page(uint16_t page) {
    if (!ctx_ || page < kHudPageImageBase || page >= kHudPageCount) return;
    replace_image(page, rhi::Image{});
}

bool HudRenderer::set_dynamic_page(uint16_t page, uint32_t width, uint32_t height, std::span<const uint8_t> rgba,
                                   uint64_t generation) {
    if (!ctx_ || page < kHudPageRadar || page >= kHudPageFontBase || width == 0 || height == 0 ||
        rgba.size() < static_cast<size_t>(width) * height * 4)
        return false;
    DynamicPage& d = dynamic_[page - kHudPageRadar];
    if (d.generation == generation && d.width == width && d.height == height && d.uploaded == generation) return true;
    d.rgba.assign(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(width) * height * 4));
    if (d.image && (d.width != width || d.height != height)) {
        alloc_->destroy(d.image);  // record() of an earlier frame-in-flight slot may still read it: callers change
                                   // the radar size only on map load (GPU idle) [port]
        d.state = rhi::ImageState::Undefined;
    }
    d.width = width;
    d.height = height;
    d.generation = generation;
    d.uploaded = ~uint64_t{0};  // new pixels: upload in the next record()
    return true;
}

bool HudRenderer::upload_dynamic_pages(VkCommandBuffer cmd, Slot& slot, uint32_t& uploads) {
    const rhi::DeviceFns& fn = ctx_->vkd();
    VkDeviceSize total = 0;
    for (DynamicPage& d : dynamic_)
        if (!d.rgba.empty() && d.uploaded != d.generation) total += static_cast<VkDeviceSize>(d.rgba.size());
    if (total == 0) return true;
    if (!ensure_buffer(slot.staging_dynamic, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::MemoryUsage::Upload)) return false;
    auto* dst = static_cast<uint8_t*>(slot.staging_dynamic.mapped());
    VkDeviceSize off = 0;
    for (DynamicPage& d : dynamic_) {
        if (d.rgba.empty() || d.uploaded == d.generation) continue;
        if (!d.image) {
            rhi::ImageDesc id;
            id.format = rhi::Format::RGBA8;
            id.width = d.width;
            id.height = d.height;
            id.name = "hud_dynamic";
            if (!alloc_->create_image(id, d.image)) return false;
            d.state = rhi::ImageState::Undefined;
        }
        std::memcpy(dst + off, d.rgba.data(), d.rgba.size());
        rhi::cmd_barrier(fn, cmd, rhi::transition(d.image.handle, d.state, rhi::ImageState::TransferDst));
        VkBufferImageCopy c{};
        c.bufferOffset = off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {d.width, d.height, 1};
        fn.vkCmdCopyBufferToImage(cmd, slot.staging_dynamic.handle, d.image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        rhi::cmd_barrier(fn, cmd, rhi::transition(d.image.handle, rhi::ImageState::TransferDst, rhi::ImageState::ShaderRead));
        d.state = rhi::ImageState::ShaderRead;
        d.uploaded = d.generation;
        off += static_cast<VkDeviceSize>(d.rgba.size());
        ++uploads;
    }
    alloc_->flush(slot.staging_dynamic.alloc);
    return true;
}

bool HudRenderer::upload_font_pages(rhi::Frame& frame, VkCommandBuffer cmd, Slot& slot, FontCache& fonts, uint32_t& uploads) {
    (void)frame;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const size_t n = std::min<size_t>(fonts.page_count(), kHudFontPages);
    const uint32_t ps = fonts.page_size();
    const VkDeviceSize page_bytes = static_cast<VkDeviceSize>(ps) * ps * 4;
    std::vector<size_t> dirty;
    for (size_t i = 0; i < n; ++i) {
        if (i >= font_pages_.size()) font_pages_.resize(i + 1);
        Page& p = font_pages_[i];
        if (p.image && (p.image.width != ps)) alloc_->destroy(p.image);
        if (!p.image) {
            rhi::ImageDesc id;
            id.format = rhi::Format::RGBA8;
            id.width = id.height = ps;
            id.name = "hud_glyphs";
            if (!alloc_->create_image(id, p.image)) return false;
            p.state = rhi::ImageState::Undefined;
            p.generation = ~uint64_t{0};
        }
        if (p.generation != fonts.page_generation(i)) dirty.push_back(i);
    }
    if (dirty.empty()) return true;
    if (!ensure_buffer(slot.staging, page_bytes * dirty.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::MemoryUsage::Upload))
        return false;
    auto* dst = static_cast<uint8_t*>(slot.staging.mapped());
    std::vector<VkImageMemoryBarrier2> pre, post;
    for (size_t k = 0; k < dirty.size(); ++k) {
        std::memcpy(dst + page_bytes * k, fonts.page_rgba(dirty[k]).data(), static_cast<size_t>(page_bytes));
        Page& p = font_pages_[dirty[k]];
        pre.push_back(rhi::transition(p.image.handle, p.state, rhi::ImageState::TransferDst));
        post.push_back(rhi::transition(p.image.handle, rhi::ImageState::TransferDst, rhi::ImageState::ShaderRead));
    }
    alloc_->flush(slot.staging.alloc);
    rhi::cmd_barrier(fn, cmd, pre);
    for (size_t k = 0; k < dirty.size(); ++k) {
        VkBufferImageCopy c{};
        c.bufferOffset = page_bytes * k;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {ps, ps, 1};
        fn.vkCmdCopyBufferToImage(cmd, slot.staging.handle, font_pages_[dirty[k]].image.handle,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    rhi::cmd_barrier(fn, cmd, post);
    for (size_t i : dirty) {
        font_pages_[i].generation = fonts.page_generation(i);
        font_pages_[i].state = rhi::ImageState::ShaderRead;
    }
    uploads += static_cast<uint32_t>(dirty.size());
    return true;
}

bool HudRenderer::record(rhi::Frame& frame, VkCommandBuffer cmd, const HudTarget& target, const HudCanvas& canvas,
                         const HudDrawList& dl, FontCache* fonts, const StepCallback& steps, HudDrawStats* stats,
                         const HudDrawList* dialogs, const HudDrawList* cursor) {
    if (!ctx_ || !target.color || !target.color_view) return false;
    if (desc_.depth_format != VK_FORMAT_UNDEFINED && !target.depth_view) return false;
    const rhi::DeviceFns& fn = ctx_->vkd();
    HudDrawStats st;
    Slot& slot = slots_[frame.slot() % slots_.size()];
    // Images replaced at least one full slot cycle ago are no longer read by any frame in flight.
    ++records_;
    for (size_t i = 0; i < retired_.size();) {
        if (records_ > retired_[i].serial + slots_.size()) {
            alloc_->destroy(retired_[i].image);
            retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    slot.desc.reset();
    if (fonts && !upload_font_pages(frame, cmd, slot, *fonts, st.page_uploads)) return false;
    if (!upload_dynamic_pages(cmd, slot, st.page_uploads)) return false;

    // Descriptor set: pages.
    const VkDescriptorSet set = slot.desc.allocate(set_layout_);
    if (!set) return false;
    VkDescriptorImageInfo imgs[kHudMaxPages];
    for (uint32_t i = 0; i < kHudMaxPages; ++i) imgs[i] = {VK_NULL_HANDLE, white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (atlas_) imgs[kHudPageAtlas].imageView = atlas_.view;
    for (size_t i = 0; i < dynamic_.size(); ++i)
        if (dynamic_[i].image && dynamic_[i].state == rhi::ImageState::ShaderRead) imgs[kHudPageRadar + i].imageView = dynamic_[i].image.view;
    for (size_t i = 0; i < font_pages_.size() && i < kHudFontPages; ++i)
        if (font_pages_[i].image && font_pages_[i].state == rhi::ImageState::ShaderRead)
            imgs[i + kHudPageFontBase].imageView = font_pages_[i].image.view;
    for (size_t i = 0; i < images_.size(); ++i)
        if (images_[i]) imgs[kHudPageImageBase + i].imageView = images_[i].view;
    const VkDescriptorImageInfo smp{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w[2];
    w[0] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w[0].dstSet = set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = kHudMaxPages;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = imgs;
    w[1] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w[1].dstSet = set;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &smp;
    fn.vkUpdateDescriptorSets(ctx_->device(), 2, w, 0, nullptr);

    // Geometry of every layer, in layer / sub-list order.
    verts_.clear();
    idx_.clear();
    batches_.clear();
    HudDrawList::LayerBatchRanges layer_batches{};
    dl.build_layers(verts_, idx_, batches_, &layer_batches);
    const size_t dialog_b0 = batches_.size();
    if (dialogs) dialogs->build_layers(verts_, idx_, batches_);
    const size_t dialog_b1 = batches_.size();
    if (cursor) cursor->build_layers(verts_, idx_, batches_);
    const size_t cursor_b1 = batches_.size();
    st.vertices = static_cast<uint32_t>(verts_.size());
    st.quads = static_cast<uint32_t>(dl.total() + (dialogs ? dialogs->total() : 0) + (cursor ? cursor->total() : 0));
    if (!verts_.empty()) {
        if (!ensure_buffer(slot.vb, verts_.size() * sizeof(HudVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::MemoryUsage::Dynamic) ||
            !ensure_buffer(slot.ib, idx_.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::MemoryUsage::Dynamic))
            return false;
        std::memcpy(slot.vb.mapped(), verts_.data(), verts_.size() * sizeof(HudVertex));
        std::memcpy(slot.ib.mapped(), idx_.data(), idx_.size() * sizeof(uint32_t));
    }

    // Pass: colour LOAD (the finished, post-processed world), depth CLEAR (step 4).
    VkImageMemoryBarrier2 b[2];
    uint32_t nb = 0;
    b[nb++] = rhi::transition(target.color, target.before, rhi::ImageState::ColorAttachment);
    if (target.depth)
        b[nb++] = rhi::transition(target.depth, rhi::ImageState::Undefined, rhi::ImageState::DepthAttachment,
                                  {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1});
    rhi::cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(b, nb));
    auto ca = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = target.color_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    auto da = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    da.imageView = target.depth_view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = {1.0f, 0};
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, target.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = target.depth ? &da : nullptr;
    fn.vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{};
    vp.y = static_cast<float>(target.extent.height);
    vp.width = static_cast<float>(target.extent.width);
    vp.height = -static_cast<float>(target.extent.height);
    vp.maxDepth = 1.0f;
    const VkRect2D sc{{0, 0}, target.extent};

    HudPush push{};
    push.xform[0] = 1.0f / canvas.cameras[kHudCameraComponents].half_width;
    push.xform[1] = 1.0f / canvas.cameras[kHudCameraComponents].half_height;
    bool bound = false;
    int32_t current = -1;
    auto draw_batches = [&](size_t b0, size_t b1) {
        for (size_t i = b0; i < b1; ++i) {
            const HudDrawList::Batch& bt = batches_[i];
            if (!bound) {
                fn.vkCmdSetViewport(cmd, 0, 1, &vp);
                fn.vkCmdSetScissor(cmd, 0, 1, &sc);
                fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
                const VkDeviceSize off = 0;
                fn.vkCmdBindVertexBuffers(cmd, 0, 1, &slot.vb.handle, &off);
                fn.vkCmdBindIndexBuffer(cmd, slot.ib.handle, 0, VK_INDEX_TYPE_UINT32);
                bound = true;
                current = -1;
            }
            const auto ei = static_cast<uint32_t>(bt.effect);
            const int32_t pi = ei < pipelines_.size() ? static_cast<int32_t>(ei) : static_cast<int32_t>(HudEffect::PrimAlpha);
            if (pi != current) {
                fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[static_cast<size_t>(pi)]);
                current = pi;
            }
            push.page = bt.page < kHudMaxPages ? bt.page : 0u;
            push.pad[0] = pi == static_cast<int32_t>(HudEffect::PrimModulate) ? 1u : 0u;  // colour op: 1 = tex + diffuse
            fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push,
                                  &push);
            fn.vkCmdDrawIndexed(cmd, bt.index_count, 1, bt.first_index, 0, 0);
            ++st.batches;
        }
    };
    for (const HudFrameStep& step : hud_frame_steps()) {
        if (step.kind != HudStepKind::Layers) {
            if (step.kind == HudStepKind::Dialogs && dialog_b1 > dialog_b0) draw_batches(dialog_b0, dialog_b1);
            if (step.kind == HudStepKind::Cursor && cursor_b1 > dialog_b1) draw_batches(dialog_b1, cursor_b1);
            if (steps) {
                steps(cmd, step);
                ++st.callbacks;
                bound = false;  // the callback may have changed any state
            }
            continue;
        }
        for (uint32_t l = step.first; l <= step.last && l < kHudLayers; ++l) {
            ++st.layers;
            const auto [b0, b1] = layer_batches[l];
            draw_batches(b0, b1);
        }
    }
    fn.vkCmdEndRendering(cmd);
    rhi::cmd_barrier(fn, cmd, rhi::transition(target.color, rhi::ImageState::ColorAttachment, target.after));
    if (stats) *stats = st;
    return true;
}

}  // namespace alamo::ui
