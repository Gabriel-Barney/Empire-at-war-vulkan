// Render targets for the post chain (docs/re/RENDER.md 1.5, TERRAIN_WATER_POST.md 3.5 / 4):
// SceneTexture / DistortionTexture / transition (1.0 x back buffer), bloom A/B (0.25 x), water
// reflection/refraction (absolute N x N + shared D24S8). A RenderTarget owns a colour image and/or a
// depth-stencil image, tracks their ImageState, and is recreated when the reference size changes.
//
// cmd_stretch_rect() is the D3D9 StretchRect equivalent: vkCmdCopyImage2 when extent and format
// match, otherwise vkCmdBlitImage2 (scaling + format conversion, e.g. BGRA swapchain -> RGBA target)
// with the given filter (the original copies the back buffer with point filtering; use LINEAR for
// downscales). It records the barriers into/out of the transfer layouts.
#pragma once
#include <cstdint>

#include "alamo/rhi/format.hpp"
#include "alamo/rhi/memory.hpp"
#include "alamo/rhi/sync.hpp"

namespace alamo::rhi {

class Context;

// One side of a copy: the image, its size/format, and the state it is in / should be left in.
struct CopyImage {
    VkImage image = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    ImageState before = ImageState::Undefined;  // state on entry (Undefined for dst = discard)
    ImageState after = ImageState::ShaderRead;  // state on exit
};
void cmd_stretch_rect(const DeviceFns& fn, VkCommandBuffer cmd, const CopyImage& src, const CopyImage& dst,
                      VkFilter filter = VK_FILTER_NEAREST);

struct RenderTargetDesc {
    Format color = Format::RGBA8;      // Undefined: no colour image
    Format depth = Format::Undefined;  // e.g. pick_depth_stencil_format(ctx); Undefined: none
    uint32_t width = 0, height = 0;    // absolute size when both non-zero ...
    float scale = 1.0f;                // ... else (int)(reference * scale), at least 1
    bool sample_depth = false;         // add SAMPLED usage to the depth image
    VkImageUsageFlags extra_color_usage = 0;
    const char* name = nullptr;
};

struct ClearValues {
    float color[4] = {0, 0, 0, 1};
    float depth = 1.0f;
    uint32_t stencil = 0;
};

class RenderTarget {
public:
    RenderTarget() = default;
    ~RenderTarget();
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    bool init(Allocator& alloc, const RenderTargetDesc& desc, VkExtent2D reference);
    // Recreates the images when the derived size changes (returns true if it did). Contents and
    // states are reset; the caller must make sure the GPU no longer uses the old images.
    bool resize(VkExtent2D reference);
    void shutdown();

    VkExtent2D extent() const { return extent_; }
    const Image& color() const { return color_; }
    const Image& depth() const { return depth_; }
    bool has_color() const { return color_.handle != VK_NULL_HANDLE; }
    bool has_depth() const { return depth_.handle != VK_NULL_HANDLE; }
    ImageState color_state() const { return color_state_; }
    ImageState depth_state() const { return depth_state_; }
    // For external changes (e.g. after a cmd_stretch_rect into the colour image).
    void set_color_state(ImageState s) { color_state_ = s; }
    void set_depth_state(ImageState s) { depth_state_ = s; }

    // Barrier from the tracked state (no-op when already there).
    void transition_color(const DeviceFns& fn, VkCommandBuffer cmd, ImageState to);
    void transition_depth(const DeviceFns& fn, VkCommandBuffer cmd, ImageState to);

    // Transitions to attachment states and begins dynamic rendering over the whole target, setting
    // viewport and scissor. clear == nullptr loads the previous contents.
    void begin_rendering(const DeviceFns& fn, VkCommandBuffer cmd, const ClearValues* clear = nullptr);
    void end_rendering(const DeviceFns& fn, VkCommandBuffer cmd) { fn.vkCmdEndRendering(cmd); }

    // StretchRect into this target's colour image, which is left in `after`.
    void copy_from(const DeviceFns& fn, VkCommandBuffer cmd, const CopyImage& src, VkFilter filter = VK_FILTER_NEAREST,
                   ImageState after = ImageState::ShaderRead);

private:
    bool create();
    void destroy_images();

    Allocator* alloc_ = nullptr;
    RenderTargetDesc desc_{};
    VkExtent2D extent_{};
    Image color_, depth_;
    ImageState color_state_ = ImageState::Undefined, depth_state_ = ImageState::Undefined;
};

}  // namespace alamo::rhi
