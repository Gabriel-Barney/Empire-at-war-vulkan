// Offscreen-render fixtures shared by the GPU test executables (render, galactic, particles, ...).
// Header-only; nothing in a shipped library includes it.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/rhi/rhi.hpp"

namespace alamo::testing {

// The device objects every offscreen GPU test needs. A test that needs more derives from it
// (`struct Gpu : testing::Gpu { render::Renderer renderer; };`).
struct Gpu {
    rhi::Context ctx;
    rhi::Allocator alloc;
    rhi::Uploader up;
    rhi::FrameRing ring;
};

// RGBA8 colour target (optionally with a D32F depth target) plus a host-readable readback buffer of the colour.
// `G` is any struct with an `alloc` (rhi::Allocator) member.
struct Offscreen {
    rhi::Image color, depth;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    template <class G>
    bool create(G& g, uint32_t width, uint32_t height, bool with_depth = true) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        cd.format = rhi::Format::RGBA8;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!g.alloc.create_image(cd, color)) return false;
        if (with_depth) {
            rhi::ImageDesc dd = cd;
            dd.format = rhi::Format::D32F;
            dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            if (!g.alloc.create_image(dd, depth)) return false;
        }
        return g.alloc.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     rhi::MemoryUsage::Readback, readback);
    }
    template <class G>
    void destroy(G& g) {
        if (color) g.alloc.destroy(color);
        if (depth) g.alloc.destroy(depth);
        if (readback) g.alloc.destroy(readback);
    }
    const uint8_t* data() const { return static_cast<const uint8_t*>(readback.mapped()); }
    const uint8_t* px(uint32_t x, uint32_t y) const { return data() + (static_cast<size_t>(y) * w + x) * 4; }
    std::vector<uint8_t> pixels() const { return std::vector<uint8_t>(data(), data() + static_cast<size_t>(w) * h * 4); }
};

}  // namespace alamo::testing
