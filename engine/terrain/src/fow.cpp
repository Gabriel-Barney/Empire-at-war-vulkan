// Fog-of-war texture (DECORATIONS.md 2). See fow.hpp.
#include "alamo/terrain/fow.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace alamo::terrain {

std::array<uint32_t, 256> fow_palette(uint32_t a, uint32_t b) {
    std::array<uint32_t, 256> p{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = 0;
        for (uint32_t sh = 0; sh < 32; sh += 8) {
            const uint32_t ca = (a >> sh) & 0xFFu, cb = (b >> sh) & 0xFFu;
            c |= (((ca * i + cb * (255u - i)) >> 8) & 0xFFu) << sh;
        }
        p[i] = c;
    }
    return p;
}

const std::array<uint8_t, 256>& fow_intensity_table() {
    static const std::array<uint8_t, 256> table = [] {
        std::array<uint8_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = 255;
            if (i < 16) v = (i * 255u * 2u + 16u) / 32u;  // i * 255 / 16, rounded [M rounding]
            else if (i >= 239) v = std::min<uint32_t>(255u, static_cast<uint32_t>(static_cast<float>(i - 237) * 14.17f));
            t[i] = static_cast<uint8_t>(v);
        }
        return t;
    }();
    return table;
}

uint8_t fow_state_to_intensity(uint8_t v, uint8_t p, uint32_t frames, uint32_t regrow) {
    const int64_t f = std::min<uint32_t>(frames, 15u);
    int64_t vv = v;
    if (v < 239) vv = std::max<int64_t>(0, vv - f * static_cast<int64_t>(regrow));
    else vv = std::min<int64_t>(255, vv - static_cast<int64_t>(p) + f);
    vv = std::clamp<int64_t>(vv, 0, 255);
    return fow_intensity_table()[static_cast<size_t>(vv)];
}

std::array<float, 4> fow_tex_u(float x0, float, float x1, float) {
    const float e = x1 - x0;
    if (!(std::fabs(e) > 0.0f) || !std::isfinite(e)) return {0, 0, 0, 0};
    return {1.0f / e, 0.0f, 0.0f, -x0 / e};
}

std::array<float, 4> fow_tex_v(float, float y0, float, float y1) {
    const float e = y1 - y0;
    if (!(std::fabs(e) > 0.0f) || !std::isfinite(e)) return {0, 0, 0, 0};
    return {0.0f, -1.0f / e, 0.0f, y1 / e};
}

bool FowGridDesc::cell_of(float x, float y, uint32_t& cx, uint32_t& cy) const {
    if (width == 0 || height == 0 || !(cell_x > 0.0f) || !(cell_y > 0.0f)) return false;
    const float fx = std::floor((x - x0) / cell_x), fy = std::floor((y1 - y) / cell_y);
    if (!(fx >= 0.0f) || !(fy >= 0.0f) || fx >= static_cast<float>(width) || fy >= static_cast<float>(height)) return false;
    cx = static_cast<uint32_t>(fx);
    cy = static_cast<uint32_t>(fy);
    return true;
}

std::array<float, 2> FowGridDesc::cell_center(uint32_t cx, uint32_t cy) const {
    return {x0 + (static_cast<float>(cx) + 0.5f) * cell_x, y1 - (static_cast<float>(cy) + 0.5f) * cell_y};
}

FowGridDesc make_fow_grid(float x0, float y0, float x1, float y1, float cell) {
    FowGridDesc d;
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(cell) ||
        !(cell > 0.0f) || !(x1 > x0) || !(y1 > y0))
        return d;
    d.x0 = x0;
    d.y0 = y0;
    d.x1 = x1;
    d.y1 = y1;
    auto count = [&](float extent) {
        const float n = std::ceil(extent / cell);  // extent may overflow to +inf
        // Range- and NaN-safe float -> integer: NaN / <= 0 -> 0, +inf / huge -> kMaxFowCells.
        if (n >= static_cast<float>(kMaxFowCells)) return kMaxFowCells;
        return n > 0.0f ? static_cast<uint32_t>(n) : 0u;
    };
    d.width = count(x1 - x0);
    d.height = count(y1 - y0);
    if (d.width == 0 || d.height == 0) {
        d.width = d.height = 0;
        return d;
    }
    d.cell_x = (x1 - x0) / static_cast<float>(d.width);
    d.cell_y = (y1 - y0) / static_cast<float>(d.height);
    return d;
}

FowGridDesc make_fow_grid(const MapData& map) {
    float mn[2], mx[2];
    if (!map.playable_box(mn, mx)) return {};
    return make_fow_grid(mn[0], mn[1], mx[0], mx[1], map.is_space() ? kSpaceFowCellSize : kLandFowCellSize);
}

FowGrid::FowGrid(const FowGridDesc& desc) : desc_(desc) {
    const size_t n = static_cast<size_t>(desc_.width) * desc_.height;
    intensity_.assign(n, 0);
    texels_.assign(n * 4, 0);
}

bool FowGrid::set_intensity(std::span<const uint8_t> cells) {
    if (cells.size() != intensity_.size()) return false;
    std::copy(cells.begin(), cells.end(), intensity_.begin());
    return true;
}

void FowGrid::fade_texel_edges(uint32_t cells) {
    const uint32_t W = desc_.width, H = desc_.height;
    if (cells == 0 || texels_.size() != static_cast<size_t>(W) * H * 4) return;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t d = std::min(std::min(x, W - 1 - x), std::min(y, H - 1 - y));
            if (d >= cells) continue;
            uint8_t& a = texels_[(static_cast<size_t>(y) * W + x) * 4 + 3];
            a = static_cast<uint8_t>(static_cast<uint32_t>(a) * d / cells);
        }
}

void FowGrid::build_texels(const std::array<uint32_t, 256>& palette, bool blur, std::vector<uint8_t>* processed) {
    const uint32_t W = desc_.width, H = desc_.height;
    const size_t n = static_cast<size_t>(W) * H;
    if (intensity_.size() != n) intensity_.assign(n, 0);
    if (n == 0) {
        texels_.clear();
        if (processed) processed->clear();
        return;
    }
    work_ = intensity_;
    // 1. Border fogged.
    for (uint32_t x = 0; x < W; ++x) {
        work_[x] = 0;
        work_[static_cast<size_t>(H - 1) * W + x] = 0;
    }
    for (uint32_t y = 0; y < H; ++y) {
        work_[static_cast<size_t>(y) * W] = 0;
        work_[static_cast<size_t>(y) * W + (W - 1)] = 0;
    }
    // 2. Separable 1-2-1 x 1-2-1, (sum + 8) >> 4, interior only (needs a 3x3 neighbourhood).
    if (blur && W >= 3 && H >= 3) {
        row_sum_.assign(n, 0);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 1; x + 1 < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                row_sum_[i] = static_cast<uint16_t>(work_[i - 1] + 2u * work_[i] + work_[i + 1]);
            }
        std::vector<uint8_t> out = work_;
        for (uint32_t y = 1; y + 1 < H; ++y)
            for (uint32_t x = 1; x + 1 < W; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                const uint32_t s = static_cast<uint32_t>(row_sum_[i - W]) + 2u * row_sum_[i] + row_sum_[i + W];
                out[i] = static_cast<uint8_t>((s + 8u) >> 4);
            }
        work_.swap(out);
    }
    // 3. Palette -> RGBA8 (bytes R, G, B, A from the ARGB entry).
    texels_.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t c = palette[work_[i]];
        texels_[4 * i + 0] = static_cast<uint8_t>((c >> 16) & 0xFFu);
        texels_[4 * i + 1] = static_cast<uint8_t>((c >> 8) & 0xFFu);
        texels_[4 * i + 2] = static_cast<uint8_t>(c & 0xFFu);
        texels_[4 * i + 3] = static_cast<uint8_t>((c >> 24) & 0xFFu);
    }
    if (processed) *processed = work_;
}

}  // namespace alamo::terrain
