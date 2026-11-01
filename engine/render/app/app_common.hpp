// Helpers shared by the alamo_render app modes (sim_main.cpp, galactic_main.cpp, particles_main.cpp).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "alamo/render/png.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render_app {

// Value of an environment variable, "" when unset.
inline std::string env_or_empty(const char* name) {
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
}

// Headless colour target (RGBA8 or BGRA8) plus a host-readable readback buffer, written out as a PNG.
struct Offscreen {
    rhi::Image color;
    rhi::Buffer readback;
    uint32_t w = 0, h = 0;
    bool bgra = false;
    bool create(rhi::Allocator& a, uint32_t width, uint32_t height, VkFormat fmt, const char* name) {
        w = width;
        h = height;
        rhi::ImageDesc cd;
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM) cd.format = rhi::Format::BGRA8;
        else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) cd.format = rhi::Format::RGBA8;
        else return false;
        bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM;
        cd.width = w;
        cd.height = h;
        cd.mips = 1;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        cd.name = name;
        return a.create_image(cd, color) &&
               a.create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::Readback, readback);
    }
    void destroy(rhi::Allocator& a) {
        if (color) a.destroy(color);
        if (readback) a.destroy(readback);
    }
    bool write_png(const std::string& path) const {
        const auto* px = static_cast<const uint8_t*>(readback.mapped());
        std::vector<uint8_t> rgba(px, px + static_cast<size_t>(w) * h * 4);
        if (bgra)
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        return render::write_png_rgba(path, w, h, rgba);
    }
};

}  // namespace alamo::render_app
