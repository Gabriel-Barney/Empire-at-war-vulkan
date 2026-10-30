// Corridor squeeze: formation path narrowing (docs/re/REMAINING_GAPS.md §4.1, 0x412210; MOVEMENT.md §4.3).
//
// Runs once per land formation after its shape and path are known. Inputs: the formation path (world XY), the full
// width W (world units, = 2 x half-width +0x254; 0 -> 20 x the cell width of the shape rule, the caller's job), the
// depth D in cells (2 x +0x250 / 20), the formation type (+0x218) and flag +0x9c.
//   - sample step s = 30 world units (0x140b2f0f4 = 1.5 x 20);
//   - type 1 = clearance probe (0x4138b0), type 2 = fixed width (every sample (W/2, W/2)), other types: no squeeze;
//   - clearance probe, h = W/2: flag +0x9c set -> every sample (h, h); else per path sample a length-h passability
//     ray (0x5a4c10, the formation's movement-class mask) along the left normal (-dy, dx) and one along the right
//     normal: clear -> h, hit -> the hit distance. A shortened side is refined: the cursor steps back by s and 4
//     probes s/2 apart are taken, keeping the minimum. At a vertex (the waypoint index changes between samples):
//     alpha = acos(dot(in, out)), n = int(alpha * h / (s/4) + 1) rays at angles k * alpha / n (k = 0..n) rotating
//     with the turn, from the vertex, on both sides, length h; each side keeps its minimum, and the previous sample
//     is min-merged into the current one ([port] reading of "on the turning side, both sides of the vertex": the
//     fan sweeps the incoming normals toward the outgoing ones);
//   - look-ahead window K = int(W / (2s) + 2) - 1: L_i = min L over [i, min(i + K, n)), same for R, in place;
//     w_i = L_i + R_i;
//   - smoothing front to back for each w_i < W: back-fill j = i-1, i-2, ... (w_j := w_i; stop at the start or once
//     i - j > 2 and w_j >= W), forward fill k > i while w_k >= w_i (a sample with w_k < W fills the gap since the
//     last one with w_i and resets the gap counter; a full-width sample grows the gap; stop when the gap reaches
//     int(2.5 x D) samples);
//   - outputs: the width-change table (+0xb8, widths in cells = w / 20; entry 0 = full width, an entry is added only
//     when the width differs from the last entry), the per-sample entry index (+0xd0: 0 when w_i >= W) and the
//     lateral shift (+0xa0): h_i = min(w_i, W) / 2; shift = 0 unless L_i < h_i or R_i < h_i, then h_i - R_i when
//     R_i <= L_i, else L_i - h_i (positive = toward the left normal, centring the block on the roomier side).
//   Vectors use the fast inverse sqrt (MOVEMENT §3.5 item 5). No RNG; everything runs in path order.
// Not modelled: the third field of the 12-byte width record (a flag, meaning unread) and the re-sampling passes
// 0x4241b0 / 0x415720 [M].
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/tactical/movement/passability.hpp"

namespace alamo::tactical::movement {

inline constexpr float kCorridorSampleStep = 30.0f;  // 0x140b2f0f4

enum CorridorType : int32_t { kCorridorNone = 0, kCorridorProbe = 1, kCorridorFixed = 2 };

struct CorridorParams {
    float width = 0.0f;          // W, world units (full width)
    float depth_cells = 0.0f;    // D, cells
    int32_t type = kCorridorProbe;
    bool fixed_flag = false;     // formation +0x9c: every sample (h, h)
    uint32_t mask = kPassClear;  // the formation's movement-class mask
};

struct CorridorSqueeze {
    std::vector<V2> samples;          // sample points (arc length i * s)
    std::vector<float> left, right;   // windowed L_i, R_i
    std::vector<float> width;         // smoothed w_i
    std::vector<float> table;         // width-change table, cells (entry 0 = W / 20)
    std::vector<uint32_t> entry;      // per sample: 0 when w_i >= W, else the current table entry
    std::vector<float> shift;         // per sample lateral shift (world units, + = left)
};

// Length of the clear part of the ray from `from` along the unit direction `dir`, at most `len`: the distance to the
// first cell (grid walk) that does not pass `mask`; `len` when the whole ray is clear (0x5a4c10).
float passability_ray(const PassabilityGrid& grid, V2 from, V2 dir, float len, uint32_t mask);

// Empty result for types other than 1 / 2, W <= 0 or a path of fewer than 2 points.
CorridorSqueeze squeeze_corridor(const PassabilityGrid& grid, std::span<const V2> path, const CorridorParams& p);

}  // namespace alamo::tactical::movement
