// alamo_ui_draw - the HUD's Vulkan pass (P2, docs/re/UI.md §1.1 steps 4-9, §2.5). Records, AFTER
// render::Renderer::render() has finished the 3D world and its whole post chain (bloom included) into the
// target, one dynamic-rendering pass over the same colour image (LOAD) with:
//   * the depth clear of step 4 (optional depth attachment, cleared; only HUD 3D models use it - nothing reads
//     world depth);
//   * the 13 HUD layers of a HudDrawList in the fixed hud_frame_steps() order, each (layer, sub-list) one
//     dynamic vertex batch, split where the texture page or blend mode changes;
//   * a callback for the other steps (ortho shell models through Renderer::draw with hud_ortho_view(), the
//     HUD Scene2d, dialogs, cursor) at their place in the order.
// No hook in engine/render is needed: the pass runs on the image Renderer::render leaves in
// PassTarget::color_after and returns it to that state.
//
// Textures: page 0 white, page 1 the MT_CommandBar atlas (set_atlas_*), pages 2.. the FontCache glyph pages
// (re-uploaded through the frame's command buffer when FontCache::page_generation changes). Vertex / index /
// staging buffers and descriptor pools are per frame-in-flight slot (FrameRing::begin_frame guarantees the
// slot's previous use finished), so nothing is overwritten while the GPU reads it.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/render/camera.hpp"
#include "alamo/rhi/descriptors.hpp"
#include "alamo/rhi/frame.hpp"
#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/pipeline.hpp"
#include "alamo/rhi/sync.hpp"
#include "alamo/ui/hud_draw.hpp"

namespace alamo::assets {
struct TextureAsset;
}
namespace alamo::rhi {
class Context;
class Uploader;
}  // namespace alamo::rhi

namespace alamo::ui {

class FontCache;

// white + atlas + radar + 13 glyph pages + 16 image pages (P4). 32 sampled images per stage exceeds Vulkan's
// guaranteed minimum (16) but not any desktop GPU's limit [port].
inline constexpr uint32_t kHudMaxPages = kHudPageCount;

struct HudRendererDesc {
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;  // set to the renderer's depth format to draw HUD 3D models
};

struct HudTarget {
    VkImage color = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;
    VkImage depth = VK_NULL_HANDLE;  // required when HudRendererDesc::depth_format is set (cleared: step 4)
    VkImageView depth_view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    rhi::ImageState before = rhi::ImageState::Present;  // state Renderer::render left the colour image in
    rhi::ImageState after = rhi::ImageState::Present;
};

struct HudDrawStats {
    uint32_t layers = 0, batches = 0, quads = 0, vertices = 0, page_uploads = 0, callbacks = 0;
};

// Ortho ViewMatrices of a HUD camera (identity world; canvas units; D3D depth 0..1) for Renderer::draw.
render::ViewMatrices hud_ortho_view(const HudCanvas& canvas, HudCameraId camera);

class HudRenderer {
public:
    HudRenderer() = default;
    ~HudRenderer();
    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    bool init(rhi::Context& ctx, rhi::Allocator& alloc, rhi::Uploader& up, const HudRendererDesc& desc);
    void shutdown();  // GPU idle

    // MT_CommandBar texture (any format assets decodes; mip 0..n uploaded). False on an unsupported format.
    bool set_atlas_texture(const assets::TextureAsset& tex);
    bool set_atlas_rgba(uint32_t width, uint32_t height, std::span<const uint8_t> rgba);  // tests / synthetic
    // Dynamic page (kHudPageRadar: the radar picture, RadarImage). The pixels are copied; the next record() uploads
    // them through its command buffer when `generation` differs from the uploaded one. Page must be in
    // [kHudPageRadar, kHudPageFontBase).
    bool set_dynamic_page(uint16_t page, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, uint64_t generation);
    // P4 image pages [kHudPageImageBase, kHudPageCount): stand-alone GUI textures (dialog backgrounds, planet / weather
    // icons, splash). Uploaded at once (Uploader::finish); a replaced image is destroyed only after every frame slot
    // has recorded again, so frames in flight keep reading the old one. clear = back to white.
    bool set_image_page(uint16_t page, const assets::TextureAsset& tex);
    bool set_image_rgba(uint16_t page, uint32_t width, uint32_t height, std::span<const uint8_t> rgba);
    void clear_image_page(uint16_t page);

    using StepCallback = std::function<void(VkCommandBuffer cmd, const HudFrameStep& step)>;
    // Records the HUD pass into `cmd` (outside any pass). `fonts` may be null (no text pages). `dialogs` (P4, optional):
    // the GUI dialog draw list (GuiManager::build) drawn at the Dialogs step (2D scene #1, UI.md §1.1 step 7) - all of
    // its 13 layers in order, before the step callback. `cursor` (optional): the mouse cursor list (MouseCursor::build)
    // drawn at the Cursor step (2D scene #2, topmost), all layers in order.
    bool record(rhi::Frame& frame, VkCommandBuffer cmd, const HudTarget& target, const HudCanvas& canvas,
                const HudDrawList& dl, FontCache* fonts, const StepCallback& steps = {}, HudDrawStats* stats = nullptr,
                const HudDrawList* dialogs = nullptr, const HudDrawList* cursor = nullptr);

private:
    struct Slot {
        rhi::Buffer vb, ib, staging, staging_dynamic;
        rhi::DescriptorAllocator desc;
        bool desc_ready = false;
    };
    struct Page {
        rhi::Image image;
        uint64_t generation = 0;
        rhi::ImageState state = rhi::ImageState::Undefined;
    };
    bool ensure_buffer(rhi::Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage, rhi::MemoryUsage mem);
    bool upload_font_pages(rhi::Frame& frame, VkCommandBuffer cmd, Slot& slot, FontCache& fonts, uint32_t& uploads);
    bool upload_dynamic_pages(VkCommandBuffer cmd, Slot& slot, uint32_t& uploads);
    bool create_pipelines();
    struct DynamicPage {
        rhi::Image image;
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
        uint64_t generation = 0, uploaded = ~uint64_t{0};
        rhi::ImageState state = rhi::ImageState::Undefined;
    };
    std::array<DynamicPage, kHudPageFontBase - kHudPageRadar> dynamic_{};
    bool replace_image(uint16_t page, rhi::Image img);
    std::array<rhi::Image, kHudImagePages> images_{};
    struct Retired {
        rhi::Image image;
        uint64_t serial = 0;  // record() count when retired
    };
    std::vector<Retired> retired_;
    uint64_t records_ = 0;

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::Uploader* up_ = nullptr;
    HudRendererDesc desc_{};
    rhi::PipelineCache cache_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> pipelines_{};  // HudEffect 0..3 (the HUD-list effects)
    VkSampler sampler_ = VK_NULL_HANDLE;
    rhi::Image white_, atlas_;
    std::vector<Page> font_pages_;
    std::array<Slot, 3> slots_;
    std::vector<HudVertex> verts_;
    std::vector<uint32_t> idx_;
    std::vector<HudDrawList::Batch> batches_;
};

}  // namespace alamo::ui
