// Water plane / mesh / constants (TERRAIN_WATER_POST.md 3). See water.hpp.
#include "alamo/terrain/water.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace alamo::terrain {

float shore_alpha(float water_height, float terrain_height, float d, float m) {
    float depth01;
    if (d > 0.0f) depth01 = std::clamp((water_height - terrain_height) / d, 0.0f, 1.0f);
    else depth01 = terrain_height < water_height ? 1.0f : 0.0f;  // degenerate fade depth: step
    return std::clamp(m + (1.0f - m) * depth01, 0.0f, 1.0f);
}

// Render-only: the std::cos/sin in this file (UV scroll, bump env matrix) are not correctly rounded
// and may differ between CRTs; they only produce shader constants.
void scroll_uv(const WaterScroll& s, float t, float off, std::array<float, 4>& u, std::array<float, 4>& v) {
    const float inv = s.tile_size != 0.0f ? 1.0f / s.tile_size : 0.0f;
    const float ca = std::cos(s.angle) * inv, sa = std::sin(s.angle) * inv;
    const float d = s.direction + off;
    u = {ca, sa, 0.0f, s.speed * std::cos(d) * t};
    v = {-0.0f, ca, 0.0f, s.speed * std::sin(d) * t};  // the V row's X term is -0 in this build (3.4)
}

WaterConstants water_constants(const WaterSurface& w, float t) {
    WaterConstants c;
    const TerrainHeader& h = w.header;
    constexpr float kQuarterPi = 0.7853981633974483f;
    scroll_uv(w.scroll[0], t, 0.0f, c.base_u, c.base_v);
    scroll_uv(w.scroll[0], t, kQuarterPi, c.bump0_u, c.bump0_v);
    scroll_uv(w.scroll[0], t, -kQuarterPi, c.bump1_u, c.bump1_v);
    scroll_uv(w.scroll[1], t, 0.0f, c.crust_u, c.crust_v);
    c.ice_bump_u = c.crust_u;
    c.ice_bump_v = c.crust_v;
    const float rd = h.reflection_distortion * 0.02f, fd = h.refraction_distortion * 0.02f;
    c.reflection_distortion = {rd, rd};
    c.refraction_distortion = {fd, fd};
    const float r = h.reflection_fraction;
    for (size_t i = 0; i < 3; ++i) {
        c.reflect_c[i] = h.water_color[i] * r;
        c.refract_c[i] = h.water_color[i] * (1.0f - r);
        c.water_color[i] = c.crust_specular[i] = c.ice_diffuse[i] = h.water_color[i];
    }
    c.reflect_c[3] = c.refract_c[3] = c.crust_specular[3] = c.ice_diffuse[3] = 1.0f;
    c.water_color[3] = h.water_alpha;
    const float a = t * 3.14f;
    c.bump_env = {std::cos(a) * rd, -std::sin(a) * rd, std::sin(a) * rd, std::cos(a) * rd};
    c.ice_reflection_distortion = h.reflection_distortion;
    return c;
}

namespace {

void build_water_patch(const Terrain& t, uint32_t px, uint32_t py, float wh, WaterPatch& out) {
    out = WaterPatch{};
    out.px = px;
    out.py = py;
    const TerrainHeader& h = t.header;
    const uint32_t x0 = px * kPatchQuads, y0 = py * kPatchQuads;
    std::vector<WaterVertex> v(kPatchVerts * kPatchVerts);
    bool below[kPatchVerts * kPatchVerts];
    for (uint32_t r = 0; r < kPatchVerts; ++r)
        for (uint32_t c = 0; c < kPatchVerts; ++c) {
            const int64_t gx = x0 + c, gy = y0 + r;
            const float th = t.height_at(gx, gy);
            const uint8_t m = t.material_at(gx, gy);
            WaterVertex& w = v[r * kPatchVerts + c];
            w.position[0] = static_cast<float>(gx) * kVertexSpacing;
            w.position[1] = static_cast<float>(gy) * kVertexSpacing;
            w.position[2] = wh;
            w.normal[0] = w.normal[1] = 0.0f;
            w.normal[2] = 1.0f;
            const float a = shore_alpha(wh, th, h.shore_fade_depth, h.shore_min_alpha);
            w.color0[0] = w.color0[1] = w.color0[2] = 0xFF;
            w.color0[3] = static_cast<uint8_t>(std::lround(a * 255.0f));
            w.color1[0] = w.color1[1] = w.color1[2] = w.color1[3] = 0xFF;
            below[r * kPatchVerts + c] = th < wh || t.materials[m < kMaterialSlots ? m : kMaterialSlots - 1].not_rendered;
        }
    for (uint32_t r = 0; r < kPatchQuads; ++r)
        for (uint32_t c = 0; c < kPatchQuads; ++c) {
            const uint32_t i = r * kPatchVerts + c;
            if (!(below[i] || below[i + 1] || below[i + kPatchVerts] || below[i + kPatchVerts + 1])) continue;
            const uint16_t a = static_cast<uint16_t>(i), b = static_cast<uint16_t>(i + 1);
            const uint16_t d = static_cast<uint16_t>(i + kPatchVerts + 1), e = static_cast<uint16_t>(i + kPatchVerts);
            out.indices.insert(out.indices.end(), {a, b, d, a, d, e});
        }
    out.vertices.resize(v.size() * sizeof(WaterVertex));
    std::memcpy(out.vertices.data(), v.data(), out.vertices.size());
    const float ext = static_cast<float>(kPatchQuads) * kVertexSpacing;
    out.bounds.min = {static_cast<float>(x0) * kVertexSpacing, static_cast<float>(y0) * kVertexSpacing, wh};
    out.bounds.max = {out.bounds.min.x + ext, out.bounds.min.y + ext, wh};
}

std::string or_white(const std::string& s) { return s.empty() ? std::string("w_generic_white.tga") : s; }

}  // namespace

bool build_water(const Terrain& t, WaterSurface& out, const WaterOptions& opt, const ParallelFor& parallel,
                 std::string* error) {
    out = WaterSurface{};
    if (!t.present) {
        if (error) *error = "map has no terrain";
        return false;
    }
    const size_t n = static_cast<size_t>(t.width) * t.height;
    if (t.width < kPatchVerts || (t.width - 1) % kPatchQuads || (t.height - 1) % kPatchQuads || t.heights.size() != n ||
        t.material.size() != n) {
        if (error) *error = "terrain grid is inconsistent";
        return false;
    }
    const TerrainHeader& h = t.header;
    out.header = h;
    out.type = h.water_type;
    out.height = h.water_height;
    out.extent_max[0] = static_cast<float>(t.width - 1) * kVertexSpacing;
    out.extent_max[1] = static_cast<float>(t.height - 1) * kVertexSpacing;
    for (int i = 0; i < 3; ++i) out.textures[i] = or_white(h.water_texture[i]);
    out.scroll[0] = t.water_scroll[0];
    out.scroll[1] = t.water_scroll[1];
    WaterTargets& rt = out.targets;
    rt.size = static_cast<uint32_t>(std::clamp<int32_t>(opt.render_target_resolution, 256, 1024));
    rt.clip_plane_offset = opt.clip_plane_offset;
    switch (out.type) {
    case WaterType::Water:
        out.effect = "TerrainWater";
        out.alpha_blend = true;
        rt.reflection = rt.refraction = rt.sky_cube = true;
        out.bindings = {{"Bump0Texture", out.textures[0]}, {"Bump1Texture", out.textures[0]}, {"BaseTexture", out.textures[1]}};
        break;
    case WaterType::Lava:
        out.effect = "TerrainLava";
        out.bindings = {{"BaseTexture", out.textures[0]}, {"CrustTexture", out.textures[1]}, {"CrustBumpTexture", out.textures[2]}};
        break;
    case WaterType::Ice:
        out.effect = "TerrainIce";
        rt.reflection = true;
        out.bindings = {{"IceTexture", out.textures[0]}, {"BumpTexture", out.textures[1]}};
        break;
    case WaterType::None: return true;
    }
    // Patches whose minimum height is below the plane.
    std::vector<uint32_t> list;
    const uint32_t pxn = t.patches_x(), pyn = t.patches_y();
    for (uint32_t py = 0; py < pyn; ++py)
        for (uint32_t px = 0; px < pxn; ++px) {
            float mn = t.height_at(px * kPatchQuads, py * kPatchQuads);
            for (uint32_t r = 0; r < kPatchVerts; ++r)
                for (uint32_t c = 0; c < kPatchVerts; ++c) mn = std::min(mn, t.height_at(px * kPatchQuads + c, py * kPatchQuads + r));
            if (mn < h.water_height) list.push_back(py * pxn + px);
        }
    out.patches.resize(list.size());
    const RangeFn body = [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) build_water_patch(t, list[i] % pxn, list[i] / pxn, h.water_height, out.patches[i]);
    };
    const uint32_t count = static_cast<uint32_t>(list.size());
    if (parallel) parallel(count, 1, body);
    else serial_for(count, 1, body);
    for (const WaterPatch& p : out.patches) {
        out.triangles += p.triangle_count();
        out.bytes += p.vertices.size() + p.indices.size() * 2;
    }
    out.enabled = !out.patches.empty();
    return true;
}

}  // namespace alamo::terrain
