// Offscreen colour target with a host-visible readback buffer (CU4 t02 / t03): the one copy the render / ui GPU tests and
// the alamo_render sub-mains (--sim, --galactic, --particles PNG captures) share. Header-only; tool and test code, not
// used by the renderer itself.
//
// Options instead of per-file copies: a D32F depth attachment of the same size or none, RGBA8 or BGRA8 colour (the
// swapchain format of a windowed run), and the colour image's debug name. The colour image is always a colour
// attachment, transfer source (copied to `readback`) and transfer destination (cleared / blitted into).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "alamo/render/png.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render {

struct OffscreenDesc {
    rhi::Format format = rhi::Format::RGBA8;  // RGBA8 or BGRA8 (4 bytes per pixel in `readback`)
    bool depth = false;                       // also create a D32F depth attachment
    const char* name = nullptr;               // debug name of the colour image
};

struct Offscreen {
    rhi::Image color, depth;
    rhi::Buffer readback;  // w * h * 4 bytes, tightly packed rows, in the colour format's byte order
    uint32_t w = 0, h = 0;
    bool bgra = false;

    bool create(rhi::Allocator& a, uint32_t width, uint32_t height, const OffscreenDesc& d = {}) {
        w = width;
        h = height;
        bgra = d.format == rhi::Format::BGRA8;
        rhi::ImageDesc cd;
        cd.format = d.format;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        cd.name = d.name;
        if (!a.create_image(cd, color)) return false;
        if (d.depth) {
            rhi::ImageDesc dd = cd;
            dd.format = rhi::Format::D32F;
            dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            dd.name = nullptr;
            if (!a.create_image(dd, depth)) return false;
        }
        return a.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback,
                               readback);
    }
    // A swapchain-format target (windowed runs capture what they present): B8G8R8A8 / R8G8B8A8 UNORM, else false.
    bool create(rhi::Allocator& a, uint32_t width, uint32_t height, VkFormat fmt, const char* name) {
        OffscreenDesc d;
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM) d.format = rhi::Format::BGRA8;
        else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) d.format = rhi::Format::RGBA8;
        else return false;
        d.name = name;
        return create(a, width, height, d);
    }
    // Releases whatever create() made (safe after a failed or no create()).
    void destroy(rhi::Allocator& a) {
        if (color) a.destroy(color);
        if (depth) a.destroy(depth);
        if (readback) a.destroy(readback);
    }

    // The mapped readback (valid after the frame's copy, an invalidate and a wait).
    const uint8_t* data() const { return static_cast<const uint8_t*>(readback.mapped()); }
    const uint8_t* px(uint32_t x, uint32_t y) const { return data() + (static_cast<size_t>(y) * w + x) * 4; }
    std::vector<uint8_t> pixels() const { return std::vector<uint8_t>(data(), data() + static_cast<size_t>(w) * h * 4); }
    // RGBA with alpha forced to 255 (BGRA swizzled), as written to PNG captures.
    std::vector<uint8_t> rgba_opaque() const {
        std::vector<uint8_t> rgba = pixels();
        if (bgra)
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        return rgba;
    }
    bool write_png(const std::string& path_utf8) const { return write_png_rgba(path_utf8, w, h, rgba_opaque()); }
};

}  // namespace alamo::render
