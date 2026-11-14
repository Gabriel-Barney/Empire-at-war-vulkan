// alamo_render FSR / XeSS upscaling, pure rules [port: Gabriel 2026-11-13] (alamo/render/upscale.hpp).
#include "alamo/render/upscale.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::render {

float upscale_ratio(UpscaleQuality q, Upscaler u) {
    if (u == Upscaler::Xess) {  // [port] XeSS 1.3+ fixed scaling (developer guide, "Fixed Input Resolution")
        switch (q) {
            case UpscaleQuality::NativeAA: return 1.0f;
            case UpscaleQuality::Quality: return 1.7f;
            case UpscaleQuality::Balanced: return 2.0f;
            case UpscaleQuality::Performance: return 2.3f;
            case UpscaleQuality::UltraPerformance: return 3.0f;
        }
        return 1.0f;
    }
    switch (q) {
        case UpscaleQuality::NativeAA: return 1.0f;
        case UpscaleQuality::Quality: return 1.5f;
        case UpscaleQuality::Balanced: return 1.7f;
        case UpscaleQuality::Performance: return 2.0f;
        case UpscaleQuality::UltraPerformance: return 3.0f;
    }
    return 1.0f;
}

void upscale_render_size(uint32_t display_w, uint32_t display_h, UpscaleQuality q, uint32_t& render_w, uint32_t& render_h,
                         Upscaler u) {
    const float r = upscale_ratio(q, u);
    render_w = std::max(1u, static_cast<uint32_t>(static_cast<float>(display_w) / r));
    render_h = std::max(1u, static_cast<uint32_t>(static_cast<float>(display_h) / r));
}

uint32_t upscale_jitter_phases(uint32_t render_w, uint32_t display_w) {
    if (render_w == 0) return 8u;
    const float s = static_cast<float>(display_w) / static_cast<float>(render_w);
    return std::max(1u, static_cast<uint32_t>(8.0f * s * s));
}

void upscale_jitter_pixels(uint64_t index, uint32_t phases, float& jx, float& jy) {
    const uint32_t i = static_cast<uint32_t>(index % std::max(phases, 1u)) + 1u;
    jx = halton(i, 2) - 0.5f;
    jy = halton(i, 3) - 0.5f;
}

UpscaleCamera upscale_camera(const TaaCamera& c) {
    UpscaleCamera u;
    if (!c.valid || c.depth_a == 0.0f) return u;
    if (c.perspective) {  // view distance b / (ndc + a): ndc 0 -> near, 1 -> far
        const float n = std::fabs(c.depth_b / c.depth_a);
        const float den = 1.0f + c.depth_a;
        const float f = den != 0.0f ? std::fabs(c.depth_b / den) : 0.0f;
        if (n > 0.0f && f > n) {
            u.near_plane = n;
            u.far_plane = f;
        }
        if (c.proj_y != 0.0f) u.fov_y = 2.0f * std::atan(1.0f / std::fabs(c.proj_y));
    } else {  // (b - ndc) / a
        const float n = std::fabs(c.depth_b / c.depth_a), f = std::fabs((c.depth_b - 1.0f) / c.depth_a);
        u.near_plane = std::max(std::min(n, f), 1e-3f);
        u.far_plane = std::max(std::max(n, f), u.near_plane * 2.0f);
    }
    return u;
}

}  // namespace alamo::render
