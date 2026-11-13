// alamo_render - FSR upscaling [port: Gabriel 2026-11-13] (the original has none; docs/DECISIONS.md "FSR upscaling").
// RecordOptions::upscaler = Upscaler::Fsr draws the scene and its post chain at a lower render resolution with a
// sub-pixel jittered camera, then AMD's FSR (FidelityFX SDK 2.4.0 Vulkan runtime: FSR Upscaling 4.1 where the GPU
// supports it, else FSR 3.1) reconstructs the display-resolution image from colour, depth and motion vectors. It
// replaces TAA for that frame: the TAA jitter / camera-cut / object-motion bookkeeping (taa.hpp) is reused, the TAA
// resolve is not run. When the runtime is missing or fails, the frame is drawn at full resolution with TAA instead.
// The pure rules here need no GPU (alamo_render_fsr_tests check them without one).
#pragma once
#include <cstdint>

#include "alamo/render/taa.hpp"

namespace alamo::render {

enum class Upscaler : uint8_t {
    None,  // full-resolution rendering (TAA per RecordOptions::taa)
    Fsr,   // AMD FSR (4.1 or 3.1, chosen by the runtime)
};

// FSR's preset quality modes (same order and values as the SDK's FfxApiUpscaleQualityMode).
enum class UpscaleQuality : uint8_t {
    NativeAA = 0,          // 1.0x: render at display resolution, FSR as anti-aliasing
    Quality = 1,           // 1.5x per dimension
    Balanced = 2,          // 1.7x
    Performance = 3,       // 2.0x
    UltraPerformance = 4,  // 3.0x
};
inline constexpr uint32_t kUpscaleQualityCount = 5;
inline constexpr float kFsrDefaultSharpness = 0.2f;  // RecordOptions::fsr_sharpness default (RCAS, 0 = off)

// Per-dimension ratio display / render of a mode (1.0, 1.5, 1.7, 2.0, 3.0; out-of-range modes: 1.0).
float upscale_ratio(UpscaleQuality q);
// Render size for a display size: each dimension display / ratio, truncated, at least 1 (the SDK's
// GETRENDERRESOLUTIONFROMQUALITYMODE rule).
void upscale_render_size(uint32_t display_w, uint32_t display_h, UpscaleQuality q, uint32_t& render_w, uint32_t& render_h);
// Jitter cycle length for a render / display width: 8 x (display / render)^2, truncated (the SDK's GETJITTERPHASECOUNT
// rule: 8 at native, 18 at 1.5x, 32 at 2x, 72 at 3x). At least 1.
uint32_t upscale_jitter_phases(uint32_t render_w, uint32_t display_w);
// FSR jitter of frame `index` in render pixels, each in (-0.5, 0.5): Halton(2, 3) point (index % phases) + 1, minus 0.5
// (the SDK's GETJITTEROFFSET sequence; never (0, 0)). Pixel space: +x right, +y DOWN - what ffxDispatchDescUpscale::
// jitterOffset takes. The projection offset is jitter_view(view, jx, -jy): render()'s NDC y points up.
void upscale_jitter_pixels(uint64_t index, uint32_t phases, float& jx, float& jy);

// The camera numbers FSR asks for, from an un-jittered camera (TaaCamera): near / far plane distances and the vertical
// field of view in radians. Perspective: from the depth rows (ndc 0 -> near, 1 -> far) and proj_y; orthographic (no
// field of view): near / far of the depth range and a nominal 1 radian, which FSR only uses for its own heuristics.
struct UpscaleCamera {
    float near_plane = 0.1f, far_plane = 1000.0f, fov_y = 1.0f;
};
UpscaleCamera upscale_camera(const TaaCamera& c);

}  // namespace alamo::render
