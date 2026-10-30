// alamo_render - temporal anti-aliasing [port]: the original has none (D3D9 back buffer, MSAA only). Pure helpers of
// the TAA in render() (RecordOptions::taa, src/taa.cpp):
//   * jitter: every scene draw of a TAA frame uses the projection offset by a sub-pixel Halton(2, 3) point
//     (kTaaSamples per cycle); the screen-space post passes (heat, bloom, the resolve) are not jittered;
//   * history: the resolve reprojects the previous frame's output with the scene depth and the previous un-jittered
//     view-projection (camera motion only - the translated D3D9 materials have no motion vectors), clips it to the
//     current 3x3 neighbourhood (YCoCg variance box) and blends kTaaCurrentWeight of the current frame in;
//   * reset: the history restarts (current frame only) on the first TAA frame, after a frame without TAA, on a
//     resize, on a camera cut (taa_history_usable below) and on Renderer::reset_taa_history().
#pragma once
#include <cstdint>

#include "alamo/render/camera.hpp"

namespace alamo::render {

inline constexpr uint32_t kTaaSamples = 8;          // jitter cycle (Halton points 1..8)
inline constexpr float kTaaCurrentWeight = 0.1f;   // share of the current frame in the resolve
inline constexpr float kTaaDefaultSharpen = 0.25f; // RecordOptions::taa_sharpen default

// Radical inverse of `index` in `base` (Halton sequence; index 0 -> 0).
float halton(uint32_t index, uint32_t base);
// Jitter of TAA frame `frame` in pixels, each in (-0.5, 0.5): Halton(2, 3) point (frame % kTaaSamples) + 1, minus 0.5.
void taa_jitter_pixels(uint64_t frame, float& jx, float& jy);
// `v` with its projection offset by (jx, jy) pixels of v.width x v.height (NDC offset 2 j / size added to rows 0 / 1 as
// a multiple of row 3, so perspective and orthographic projections both move by exactly that); view_proj follows.
// (0, 0) returns `v` unchanged bit for bit.
ViewMatrices jitter_view(const ViewMatrices& v, float jx, float jy);

// What the history test compares between two frames (the un-jittered camera).
struct TaaCamera {
    bool valid = false;
    Mat4 view_proj;
    Float3 eye{0, 0, 0}, forward{0, 0, -1};
    float proj_x = 0, proj_y = 0, far_plane = 0;  // projection x / y scale, far plane (0: not a perspective projection)
    uint32_t width = 0, height = 0;
};
TaaCamera taa_camera(const ViewMatrices& unjittered);
// False when the previous frame's history cannot be reprojected into `cur`: no previous frame, a different size,
// a field-of-view change over 2 %, a turn over ~11 degrees (forward dot < 0.98) or a move over 5 % of the far plane
// (perspective; orthographic: any eye move over 5 % of the larger x / y extent) in one frame - a teleport / camera cut.
bool taa_history_usable(const TaaCamera& prev, const TaaCamera& cur);

}  // namespace alamo::render
