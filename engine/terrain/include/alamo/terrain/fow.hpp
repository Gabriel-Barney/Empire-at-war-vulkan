// alamo_terrain - fog-of-war texture, CPU side (docs/re/DECORATIONS.md 2). No Vulkan.
//
// One texel per FOW cell over the map's playable box (MapData::playable_box). The sim (or a test)
// supplies an intensity byte per cell (0 = fogged, 255 = clear; fow_state_to_intensity() converts
// the sim's state bytes); FowGrid::build_texels() forces the border to fog, applies the 1-2-1 blur
// and maps each value through the palette into RGBA8 texels. Everything is integer arithmetic in
// index order, so the texels are bit-identical on every machine and thread count.
//
// What the renderer must do (decoration type 4, DECORATIONS.md 2.4-2.6):
//   * Upload FowGrid::texels() as a width() x height() VK_FORMAT_R8G8B8A8_UNORM texture, 1 mip,
//     sampler linear + CLAMP_TO_EDGE (re-upload when build_texels() ran).
//   * Bind FOW_TEX_U / FOW_TEX_V (fow_tex_u/fow_tex_v of the grid box) as float4 constants; the
//     shaders compute uv = (dot(FOW_TEX_U, (x, y, z, 1)), dot(FOW_TEX_V, (x, y, z, 1))) from the
//     world position. With the FOW option off, bind a white 1x1 texture instead.
//   * Land: after all other bucket-0 decorations, redraw every visible terrain patch (its full
//     index list, or TerrainPatch::rendered_indices when it has holes) with TerrainFogOfWar: blend
//     DST_COLOR / ZERO (frame buffer x texel), no depth write, LESS_OR_EQUAL, identity world, the
//     0.3 near/far camera offset; four bilinear taps at uv +- FilterKernelSize (0.001237624) along
//     +x, +y, -x, -y, averaged. Also multiply TerrainMeshGloss/Bump, Grass, BatchMesh*, water,
//     lava, ice by the sample in their own shaders.
//   * Space: draw a plane over the box at z = SpaceFOWHeight (-80) with SpaceFogOfWar (alpha blend,
//     no cull): colour = FOW texel, alpha x W_Space_FOW_Grid.tga; use fow_palette(kSpaceVisible,
//     SpaceFOWColor).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/terrain/ted.hpp"

namespace alamo::terrain {

constexpr float kLandFowCellSize = 15.0f;    // GameConstants DesiredLandFOWCellSize (vanilla)
constexpr float kSpaceFowCellSize = 100.0f;  // DesiredSpaceFOWCellSize
constexpr uint32_t kMaxFowCells = 512;       // per side
constexpr float kSpaceFowHeight = -80.0f;    // SpaceFOWHeight
constexpr uint32_t kSpaceFowEdgeFadeCells = 4;  // [port] fade_texel_edges ramp of the space plane (400 world units)

// D3DCOLOR ARGB packing of a GameConstants colour "c1,c2,c3,c4": A = c4, R = c1, G = c2, B = c3
// (DECORATIONS.md 2.3 step 4). Components are clamped to 0..255.
constexpr uint32_t pack_fow_color(uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4) {
    auto cl = [](uint32_t v) { return v > 255u ? 255u : v; };
    return (cl(c4) << 24) | (cl(c1) << 16) | (cl(c2) << 8) | cl(c3);
}
constexpr uint32_t kLandFowVisible = 0xFFFFFFFFu;                   // palette 1 A
constexpr uint32_t kLandFowColor = pack_fow_color(130, 130, 130, 0);  // LandFOWColor (vanilla)
constexpr uint32_t kSetupPhaseFowColor = pack_fow_color(0, 130, 130, 130);  // SetupPhaseFOWColor
constexpr uint32_t kSpaceFowVisible = 0x00000000u;                  // palette 0 A
constexpr uint32_t kSpaceFowColor = pack_fow_color(255, 255, 255, 255);     // SpaceFOWColor
constexpr uint32_t kSpaceReinforceFowColor = pack_fow_color(255, 0, 0, 254);

// Palette entry i (0..255) = per channel (A_c * i + B_c * (255 - i)) >> 8, A = visible, B = fog,
// both ARGB. (So i = 255 gives 254 for a 255 channel, as in the original.)
std::array<uint32_t, 256> fow_palette(uint32_t visible_argb, uint32_t fog_argb);

// State -> intensity table of the renderer ctor (2.3 step 1): 0..15 -> i*255/16, 16..238 -> 255,
// 239..255 -> (i - 237) * 14.17 (28 .. 255).
const std::array<uint8_t, 256>& fow_intensity_table();
// One cell: v = state byte, p = second-array value, frames = sim frames since the grid update
// (0..15), regrow = grid +0x4C / 16. [H] arithmetic, [M] meaning (DECORATIONS.md 2.3).
uint8_t fow_state_to_intensity(uint8_t v, uint8_t p, uint32_t frames, uint32_t regrow);

// FOW_TEX_U / FOW_TEX_V for a box (x0, y0)-(x1, y1): world (x, y, z, 1) -> texture (u, v), with v
// flipped (v = 0 at y1). Zero vectors for an empty box.
std::array<float, 4> fow_tex_u(float x0, float y0, float x1, float y1);
std::array<float, 4> fow_tex_v(float x0, float y0, float x1, float y1);

struct FowGridDesc {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // playable box
    uint32_t width = 0, height = 0;           // cells = texels, ceil(extent / cell) clamped to 512
    float cell_x = 0, cell_y = 0;             // actual cell size = extent / count
    // Cell of a world position (row 0 = the max-y edge, matching the V flip [M]); false outside.
    bool cell_of(float x, float y, uint32_t& cx, uint32_t& cy) const;
    // World centre of a cell.
    std::array<float, 2> cell_center(uint32_t cx, uint32_t cy) const;
};
// Grid over a box with the desired cell size. An invalid box / cell gives a 0 x 0 grid.
FowGridDesc make_fow_grid(float x0, float y0, float x1, float y1, float cell_size);
// Grid over a map's playable box: land kLandFowCellSize, space kSpaceFowCellSize.
FowGridDesc make_fow_grid(const MapData& map);

class FowGrid {
public:
    FowGrid() = default;
    explicit FowGrid(const FowGridDesc& desc);

    const FowGridDesc& desc() const { return desc_; }
    uint32_t width() const { return desc_.width; }
    uint32_t height() const { return desc_.height; }
    std::array<float, 4> tex_u() const { return fow_tex_u(desc_.x0, desc_.y0, desc_.x1, desc_.y1); }
    std::array<float, 4> tex_v() const { return fow_tex_v(desc_.x0, desc_.y0, desc_.x1, desc_.y1); }

    // Per-cell intensity (0 fogged .. 255 clear), row-major, row 0 = max-y edge. Size W x H.
    std::vector<uint8_t>& intensity() { return intensity_; }
    const std::vector<uint8_t>& intensity() const { return intensity_; }
    // Copies W x H bytes (false and unchanged on a size mismatch).
    bool set_intensity(std::span<const uint8_t> cells);

    // Border -> 0, then (if blur) the separable 1-2-1 x 1-2-1 filter (sum + 8) >> 4 on interior
    // cells (border cells stay fogged [M]: the original blurs "the updated rectangle"), then the
    // palette. Writes W x H RGBA8 texels (bytes R, G, B, A). `processed` (optional) receives the
    // final per-cell values before the palette.
    void build_texels(const std::array<uint32_t, 256>& palette, bool blur = true,
                      std::vector<uint8_t>* processed = nullptr);
    const std::vector<uint8_t>& texels() const { return texels_; }  // W * H * 4 bytes
    // After build_texels: alpha ramps linearly to 0 on the border over `cells` cells, so the space FOW plane has no hard
    // box outline [port: the original's plane edge is not traced; DECORATIONS.md 2.7]. Colour and game logic are untouched.
    void fade_texel_edges(uint32_t cells);

private:
    FowGridDesc desc_;
    std::vector<uint8_t> intensity_, texels_, work_;
    std::vector<uint16_t> row_sum_;
};

}  // namespace alamo::terrain
