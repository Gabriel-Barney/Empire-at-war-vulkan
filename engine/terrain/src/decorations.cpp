// Terrain decorations, CPU side (DECORATIONS.md 1, 3, 4, 5). See decorations.hpp for the contract.
#include "alamo/terrain/decorations.hpp"

#include "alamo/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace alamo::terrain {

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Deterministic 2D length: std::hypot is not correctly rounded and differs between CRTs, sqrt is
// (IEEE). Products are named so no FMA contraction can merge them (also /fp:precise,
// -ffp-contract=off). Used for state that game logic may read (dynamic track points / distances).
float det_len2(float dx, float dy) {
    const float xx = dx * dx;
    const float yy = dy * dy;
    const float s = xx + yy;
    return std::sqrt(s);
}

// [0,1] -> byte, round to nearest (non-finite -> 0).
uint8_t unorm8(float v) {
    if (!std::isfinite(v)) return 0;
    return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f);
}

void grow(assets::Aabb& b, bool& first, const float (&p)[3]) {
    if (first) {
        b.min = {p[0], p[1], p[2]};
        b.max = b.min;
        first = false;
        return;
    }
    b.min.x = std::min(b.min.x, p[0]);
    b.min.y = std::min(b.min.y, p[1]);
    b.min.z = std::min(b.min.z, p[2]);
    b.max.x = std::max(b.max.x, p[0]);
    b.max.y = std::max(b.max.y, p[1]);
    b.max.z = std::max(b.max.z, p[2]);
}

void compute_bounds(DecorationMesh& m) {
    bool first = true;
    m.bounds = {};
    for (const LitVertex& v : m.vertices) grow(m.bounds, first, v.position);
}

void set_color(uint8_t (&dst)[4], const std::array<uint8_t, 4>& bgra) {
    dst[0] = bgra[0];
    dst[1] = bgra[1];
    dst[2] = bgra[2];
    dst[3] = bgra[3];
}

std::array<float, 3> normalized(float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 1e-20f)) return {0.0f, 0.0f, 1.0f};
    return {x / len, y / len, z / len};
}

// Grid vertex range [lo, hi] (inclusive) covering world interval [a, b] on an axis of n vertices.
bool grid_range(float a, float b, uint32_t n, uint32_t& lo, uint32_t& hi) {
    if (n < 2 || !std::isfinite(a) || !std::isfinite(b)) return false;
    const float maxv = static_cast<float>(n - 1);
    const float fa = std::floor(a / kVertexSpacing), fb = std::ceil(b / kVertexSpacing);
    if (fb < 0.0f || fa > maxv) return false;
    lo = static_cast<uint32_t>(std::clamp(fa, 0.0f, maxv));
    hi = static_cast<uint32_t>(std::clamp(fb, 0.0f, maxv));
    return hi > lo;
}

bool terrain_ok(const Terrain& t) {
    return t.present && t.width >= 2 && t.height >= 2 && t.heights.size() >= static_cast<size_t>(t.width) * t.height;
}

}  // namespace

// ---------------------------------------------------------------- render modes
RenderMode render_mode_from_string(std::string_view s) {
    s = trim_ws(s);
    for (uint32_t i = 0; i < kRenderModeCount; ++i) {
        const std::string_view n = render_mode_name(static_cast<RenderMode>(i));
        if (!n.empty() && iequals(n, s)) return static_cast<RenderMode>(i);
    }
    return RenderMode::Opaque;
}

RenderMode render_mode_from_u32(uint32_t v) {
    return v < kRenderModeCount ? static_cast<RenderMode>(v) : RenderMode::Opaque;
}

std::string_view render_mode_name(RenderMode m) {
    switch (m) {
    case RenderMode::Opaque: return "OPAQUE";
    case RenderMode::Additive: return "ADDITIVE";
    case RenderMode::Alpha: return "ALPHA";
    case RenderMode::Modulate: return "MODULATE";
    case RenderMode::DepthSpriteAdditive: return "DEPTHSPRITE_ADDITIVE";
    case RenderMode::DepthSpriteAlpha: return "DEPTHSPRITE_ALPHA";
    case RenderMode::DepthSpriteModulate: return "DEPTHSPRITE_MODULATE";
    case RenderMode::DiffuseAlpha: return "DIFFUSE_ALPHA";
    case RenderMode::StencilDarken: return "STENCIL_DARKEN";
    case RenderMode::DecalBumpAlpha: return "DECAL_BUMP_ALPHA";
    default: return "";
    }
}

uint8_t pack_unit(float t) {
    if (!std::isfinite(t)) return 0x7F;
    const float v = (std::clamp(t, -1.0f, 1.0f) + 1.0f) * 127.5f;
    return static_cast<uint8_t>(std::min(v, 255.0f));  // truncation, as a float -> byte store [M]
}

// ---------------------------------------------------------------- shared helpers
std::array<float, 3> terrain_normal_at(const Terrain& t, float wx, float wy) {
    if (!terrain_ok(t) || !std::isfinite(wx) || !std::isfinite(wy)) return {0.0f, 0.0f, 1.0f};
    const float maxx = static_cast<float>(t.width - 1), maxy = static_cast<float>(t.height - 1);
    const float gx = std::clamp(wx / kVertexSpacing, 0.0f, maxx);
    const float gy = std::clamp(wy / kVertexSpacing, 0.0f, maxy);
    int64_t ix = static_cast<int64_t>(gx), iy = static_cast<int64_t>(gy);
    if (ix >= static_cast<int64_t>(t.width) - 1) ix = t.width - 2;
    if (iy >= static_cast<int64_t>(t.height) - 1) iy = t.height - 2;
    const float fx = gx - static_cast<float>(ix), fy = gy - static_cast<float>(iy);
    const auto n00 = terrain_normal(t, ix, iy), n11 = terrain_normal(t, ix + 1, iy + 1);
    std::array<float, 3> r{};
    if (fx >= fy) {  // triangle (0,0)(1,0)(1,1): weights 1-fx, fx-fy, fy
        const auto n10 = terrain_normal(t, ix + 1, iy);
        for (size_t i = 0; i < 3; ++i) r[i] = (1.0f - fx) * n00[i] + (fx - fy) * n10[i] + fy * n11[i];
    } else {  // (0,0)(1,1)(0,1): weights 1-fy, fx, fy-fx
        const auto n01 = terrain_normal(t, ix, iy + 1);
        for (size_t i = 0; i < 3; ++i) r[i] = (1.0f - fy) * n00[i] + fx * n11[i] + (fy - fx) * n01[i];
    }
    return normalized(r[0], r[1], r[2]);
}

DecorationFrame DecorationFrame::make(float cx, float cy, float width, float height, float rotation) {
    DecorationFrame f;
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(rotation))
        return f;
    // Render-only (decal / blob projection frames): std::cos/sin are not correctly rounded and may
    // differ between CRTs; no game-logic state reads the result.
    const float c = std::cos(rotation), s = std::sin(rotation);
    f.x_axis[0] = c * width;
    f.x_axis[1] = s * width;
    f.y_axis[0] = s * height;   // R(rot) (0, -height)
    f.y_axis[1] = -c * height;
    f.origin[0] = cx - 0.5f * (f.x_axis[0] + f.y_axis[0]);
    f.origin[1] = cy - 0.5f * (f.x_axis[1] + f.y_axis[1]);
    const float det = f.x_axis[0] * f.y_axis[1] - f.y_axis[0] * f.x_axis[1];
    if (!(std::fabs(det) > 1e-12f) || !std::isfinite(det)) return f;
    const float ox = f.origin[0], oy = f.origin[1];
    f.inv_x[0] = f.y_axis[1] / det;
    f.inv_x[1] = -f.y_axis[0] / det;
    f.inv_x[2] = -(f.inv_x[0] * ox + f.inv_x[1] * oy);
    f.inv_y[0] = -f.x_axis[1] / det;
    f.inv_y[1] = f.x_axis[0] / det;
    f.inv_y[2] = -(f.inv_y[0] * ox + f.inv_y[1] * oy);
    const float k[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        f.corners[i][0] = f.origin[0] + k[i][0] * f.x_axis[0] + k[i][1] * f.y_axis[0];
        f.corners[i][1] = f.origin[1] + k[i][0] * f.x_axis[1] + k[i][1] * f.y_axis[1];
    }
    f.min_xy[0] = f.max_xy[0] = f.corners[0][0];
    f.min_xy[1] = f.max_xy[1] = f.corners[0][1];
    for (int i = 1; i < 4; ++i)
        for (int a = 0; a < 2; ++a) {
            f.min_xy[a] = std::min(f.min_xy[a], f.corners[i][a]);
            f.max_xy[a] = std::max(f.max_xy[a], f.corners[i][a]);
        }
    f.valid = true;
    return f;
}

std::array<float, 2> DecorationFrame::local(float x, float y) const {
    return {inv_x[0] * x + inv_x[1] * y + inv_x[2], inv_y[0] * x + inv_y[1] * y + inv_y[2]};
}

// ---------------------------------------------------------------- decals
namespace {

struct ClipVert {
    float p[3];
    float n[3];
    float l[2];  // decal-local (lx, ly)
};

ClipVert lerp(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert r;
    for (int i = 0; i < 3; ++i) {
        r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
        r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * t;
    }
    for (int i = 0; i < 2; ++i) r.l[i] = a.l[i] + (b.l[i] - a.l[i]) * t;
    return r;
}

// Sutherland-Hodgman against one half-plane: keep d(v) >= 0 with d = sign * (l[axis] - bound).
void clip_against(std::vector<ClipVert>& poly, std::vector<ClipVert>& tmp, int axis, float bound, float sign) {
    tmp.clear();
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const ClipVert& a = poly[i];
        const ClipVert& b = poly[(i + 1) % n];
        const float da = sign * (a.l[axis] - bound), db = sign * (b.l[axis] - bound);
        if (da >= 0.0f) tmp.push_back(a);
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            ClipVert v = lerp(a, b, t);
            v.l[axis] = bound;  // exact on the edge
            tmp.push_back(v);
        }
    }
    poly.swap(tmp);
}

}  // namespace

std::array<uint8_t, 4> decal_vertex_color(RenderMode mode, float alpha, const uint8_t (&tangent)[3]) {
    const float a = std::isfinite(alpha) ? clamp01(alpha) : 0.0f;  // [M] clamped: intensity is 1 in data
    uint8_t r = 255, g = 255, b = 255;
    switch (mode) {
    case RenderMode::Additive:
    case RenderMode::DepthSpriteAdditive:
        r = g = b = unorm8(a);  // c + a - 1 with c = 1
        break;
    case RenderMode::Modulate:
    case RenderMode::DepthSpriteModulate:
        r = g = b = unorm8(1.0f - a);  // (1 - c) + (1 - a) with c = 1
        break;
    case RenderMode::DecalBumpAlpha:
        r = tangent[0];
        g = tangent[1];
        b = tangent[2];
        break;
    default: break;
    }
    return {b, g, r, unorm8(a)};
}

DecalFade decal_fade(const TerrainDecal& d, float age) {
    DecalFade f;
    f.alpha = d.intensity;
    if (d.fade_time > 0.0f && std::isfinite(age)) {
        f.alpha = d.intensity * (d.fade_time - age) / d.fade_time;
        f.dead = age > d.fade_time;
        if (f.alpha < 0.0f) f.alpha = 0.0f;
    }
    return f;
}

void set_decal_alpha(DecalGeometry& g, float alpha) {
    const auto c = decal_vertex_color(g.mode, alpha, g.tangent_rgb);
    for (LitVertex& v : g.mesh.vertices) set_color(v.color, c);
}

namespace {

enum class BuildStatus { Built, Empty, TooLarge, OverBudget };

// Grid cells spanned by world interval [a, b] on one axis before clamping to the grid; computed in
// double so a huge (finite) extent cannot overflow. Negative for non-finite input.
double axis_cells(float a, float b) {
    if (!std::isfinite(a) || !std::isfinite(b)) return -1.0;
    const double fa = std::floor(static_cast<double>(a) / static_cast<double>(kVertexSpacing));
    const double fb = std::ceil(static_cast<double>(b) / static_cast<double>(kVertexSpacing));
    return std::max(fb - fa, 1.0);
}

// Builds one decal. `vert_room` / `idx_room`: what is left of the caller's budget; the decal is
// skipped (OverBudget) when its worst case (14 vertices / 30 indices per clamped cell) does not
// fit, before anything is allocated. `cells` receives the unclamped AABB cell count.
BuildStatus decal_impl(const Terrain& t, const TerrainDecal& d, DecalGeometry& out, size_t vert_room, size_t idx_room,
                       double& cells) {
    out = DecalGeometry{};
    out.mode = render_mode_from_u32(d.render_mode);
    cells = 0.0;
    if (!terrain_ok(t)) return BuildStatus::Empty;
    const DecorationFrame f = DecorationFrame::make(d.position[0], d.position[1], d.width, d.height, d.rotation);
    if (!f.valid) return BuildStatus::Empty;
    // Size cap before any clipping or allocation (hostile .ted input).
    const double cx = axis_cells(f.min_xy[0], f.max_xy[0]), cy = axis_cells(f.min_xy[1], f.max_xy[1]);
    if (!(cx > 0.0) || !(cy > 0.0)) {
        cells = -1.0;
        return BuildStatus::TooLarge;
    }
    cells = cx * cy;
    if (cells > static_cast<double>(kMaxDecalCells)) return BuildStatus::TooLarge;

    // Atlas matrix (4.3 step 2): u = (column + scale lx) / s, v = (row + scale ly) / s, s = sqrt(N),
    // lx, ly = frame-local fractions (0..1 from the frame origin = the decal's (-X, +Y) corner; the
    // frame's Y axis is -height). Slot origin settled by capture (g1f t05, DECORATIONS 4.3 [H]):
    // with X^ = (cos t, sin t), Y^ = X^ turned +90 deg about +z and lx', ly' = (P - C).X^, (P - C).Y^,
    // u = (col + 0.5 + lx'/w)/4 - 1, v = (row + 0.5 - ly'/h)/4 - 1 for N = 16, scale 1. So (u min,
    // v min) of the slot sits at the (-X, +Y) corner, u runs along +X, v along -Y, no inset. This
    // matrix is that rule without the -1 offset (integer, ignored under WRAP); test_decal_atlas_uv.
    const float s = std::max(1.0f, std::sqrt(static_cast<float>(std::max<uint32_t>(d.slot_count, 1))));
    const float sc = static_cast<float>(std::max<uint32_t>(d.slot_scale, 1)) / s;
    out.atlas_u = {f.inv_x[0] * sc, f.inv_x[1] * sc, 0.0f, static_cast<float>(d.slot_column) / s + f.inv_x[2] * sc};
    out.atlas_v = {f.inv_y[0] * sc, f.inv_y[1] * sc, 0.0f, static_cast<float>(d.slot_row) / s + f.inv_y[2] * sc};

    // Bump tangent (4.3 step 4): decal X axis projected on the terrain plane at the centre.
    {
        const auto n = terrain_normal_at(t, d.position[0], d.position[1]);
        const float c = std::cos(d.rotation), sn = std::sin(d.rotation);
        const float dot = c * n[0] + sn * n[1];
        const auto tg = normalized(c - dot * n[0], sn - dot * n[1], -dot * n[2]);
        for (int i = 0; i < 3; ++i) out.tangent_rgb[i] = pack_unit(tg[static_cast<size_t>(i)]);
    }

    uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    if (!grid_range(f.min_xy[0], f.max_xy[0], t.width, x0, x1) || !grid_range(f.min_xy[1], f.max_xy[1], t.height, y0, y1))
        return BuildStatus::Empty;
    {
        const size_t clamped = static_cast<size_t>(x1 - x0) * (y1 - y0);  // <= cells <= kMaxDecalCells
        if (14u * clamped > vert_room || 30u * clamped > idx_room) {
            const RenderMode mode = out.mode;
            out = DecalGeometry{};
            out.mode = mode;
            return BuildStatus::OverBudget;
        }
    }

    const auto color = decal_vertex_color(out.mode, d.intensity, out.tangent_rgb);
    std::map<std::array<uint32_t, 3>, uint32_t> dedup;  // keyed by position bits: deterministic order
    std::vector<ClipVert> poly, tmp;
    auto corner = [&](uint32_t gx, uint32_t gy) {
        ClipVert v;
        v.p[0] = static_cast<float>(gx) * kVertexSpacing;
        v.p[1] = static_cast<float>(gy) * kVertexSpacing;
        v.p[2] = t.height_at(gx, gy);
        const auto n = terrain_normal(t, gx, gy);
        v.n[0] = n[0];
        v.n[1] = n[1];
        v.n[2] = n[2];
        const auto l = f.local(v.p[0], v.p[1]);
        v.l[0] = l[0];
        v.l[1] = l[1];
        return v;
    };
    auto emit = [&](const ClipVert& c) -> uint32_t {
        LitVertex v{};
        v.position[0] = c.p[0];
        v.position[1] = c.p[1];
        v.position[2] = c.p[2] + kDecalLift;
        std::array<uint32_t, 3> key{};
        std::memcpy(key.data(), v.position, 12);
        const auto it = dedup.find(key);
        if (it != dedup.end()) return it->second;
        const auto n = normalized(c.n[0], c.n[1], c.n[2]);
        v.normal[0] = n[0];
        v.normal[1] = n[1];
        v.normal[2] = n[2];
        set_color(v.color, color);
        const float p4[4] = {v.position[0], v.position[1], v.position[2], 1.0f};
        v.texcoord[0] = out.atlas_u[0] * p4[0] + out.atlas_u[1] * p4[1] + out.atlas_u[2] * p4[2] + out.atlas_u[3];
        v.texcoord[1] = out.atlas_v[0] * p4[0] + out.atlas_v[1] * p4[1] + out.atlas_v[2] * p4[2] + out.atlas_v[3];
        const uint32_t idx = static_cast<uint32_t>(out.mesh.vertices.size());
        out.mesh.vertices.push_back(v);
        dedup.emplace(key, idx);
        return idx;
    };
    for (uint32_t gy = y0; gy < y1; ++gy)
        for (uint32_t gx = x0; gx < x1; ++gx) {
            const ClipVert v00 = corner(gx, gy), v10 = corner(gx + 1, gy);
            const ClipVert v11 = corner(gx + 1, gy + 1), v01 = corner(gx, gy + 1);
            const ClipVert tris[2][3] = {{v00, v10, v11}, {v00, v11, v01}};  // terrain winding
            for (const auto& tri : tris) {
                poly.assign(tri, tri + 3);
                clip_against(poly, tmp, 0, 0.0f, 1.0f);
                if (poly.size() >= 3) clip_against(poly, tmp, 0, 1.0f, -1.0f);
                if (poly.size() >= 3) clip_against(poly, tmp, 1, 0.0f, 1.0f);
                if (poly.size() >= 3) clip_against(poly, tmp, 1, 1.0f, -1.0f);
                if (poly.size() < 3) continue;
                const uint32_t first = emit(poly[0]);
                uint32_t prev = emit(poly[1]);
                for (size_t i = 2; i < poly.size(); ++i) {
                    const uint32_t cur = emit(poly[i]);
                    if (first != prev && prev != cur && first != cur) out.mesh.indices.insert(out.mesh.indices.end(), {first, prev, cur});
                    prev = cur;
                }
            }
        }
    compute_bounds(out.mesh);
    return out.mesh.empty() ? BuildStatus::Empty : BuildStatus::Built;
}

constexpr size_t kMaxBudgetLogs = 8;  // per map builder call; the rest is summarised

}  // namespace

bool build_decal_geometry(const Terrain& t, const TerrainDecal& d, DecalGeometry& out) {
    double cells = 0.0;
    const BuildStatus st = decal_impl(t, d, out, SIZE_MAX, SIZE_MAX, cells);
    if (st == BuildStatus::TooLarge)
        log::warnf("terrain", "decal %gx%g at (%g, %g): %.0f cells exceed the %u-cell decal cap; rejected",
                   static_cast<double>(d.width), static_cast<double>(d.height), static_cast<double>(d.position[0]),
                   static_cast<double>(d.position[1]), cells, kMaxDecalCells);
    return st == BuildStatus::Built;
}

size_t build_map_decal_geometry(const Terrain& t, std::vector<DecalGeometry>& out) {
    out.clear();
    out.resize(t.decals.size());  // proportional to the already-parsed decal list
    size_t built = 0, verts = 0, idx = 0, too_large = 0, over = 0;
    for (size_t i = 0; i < t.decals.size(); ++i) {
        const TerrainDecal& d = t.decals[i];
        DecalGeometry& g = out[i];
        double cells = 0.0;
        const BuildStatus st = decal_impl(t, d, g, kMaxMapDecalVertices - verts, kMaxMapDecalIndices - idx, cells);
        if (st == BuildStatus::Built) {
            verts += g.mesh.vertices.size();
            idx += g.mesh.indices.size();
            ++built;
        } else if (st == BuildStatus::TooLarge) {
            if (too_large++ < kMaxBudgetLogs)
                log::warnf("terrain", "decal %zu (%gx%g): %.0f cells exceed the %u-cell decal cap; rejected", i,
                           static_cast<double>(d.width), static_cast<double>(d.height), cells, kMaxDecalCells);
        } else if (st == BuildStatus::OverBudget) {
            if (over++ < kMaxBudgetLogs)
                log::warnf("terrain", "decal %zu: would exceed the map decal budget (%u vertices / %u indices); skipped", i,
                           kMaxMapDecalVertices, kMaxMapDecalIndices);
        }
    }
    if (too_large > kMaxBudgetLogs || over > kMaxBudgetLogs)
        log::warnf("terrain", "%zu decals: %zu oversized, %zu over the map budget; %zu built", t.decals.size(), too_large, over,
                   built);
    return built;
}

// ---------------------------------------------------------------- shadow blobs
std::vector<ShadowBlobMaterial> builtin_blob_materials() {
    std::vector<ShadowBlobMaterial> m(2);
    m[0].name = "Default_Shadow";
    m[0].texture = "w_shadow_blob.tga";
    m[0].mode = RenderMode::Modulate;
    m[1].name = "Default_Light";
    m[1].texture = "w_light_blob.tga";
    m[1].mode = RenderMode::Additive;
    return m;
}

uint32_t find_blob_material(const std::vector<ShadowBlobMaterial>& materials, std::string_view name) {
    for (size_t i = 0; i < materials.size(); ++i)
        if (iequals(materials[i].name, name)) return static_cast<uint32_t>(i);
    return 0;
}

std::array<uint8_t, 4> blob_vertex_color(RenderMode mode, const float (&c)[4], float alpha) {
    const float a = std::isfinite(alpha) ? clamp01(alpha) : 0.0f;
    float rgb[3] = {c[0], c[1], c[2]};
    float out_a = a;
    switch (mode) {
    case RenderMode::Additive:
        for (float& x : rgb) x *= a;
        out_a = a * a;
        break;
    case RenderMode::Alpha:
        out_a = c[3];  // [M] rgb = colour
        break;
    case RenderMode::Modulate:
        for (float& x : rgb) x = x == 1.0f ? 0.0f : (x == 0.0f ? 1.0f : x + (1.0f - a));
        break;
    case RenderMode::DecalBumpAlpha:
        rgb[0] = rgb[1] = rgb[2] = 127.0f / 255.0f;  // [L] blobs carry no tangent: flat
        break;
    default: break;
    }
    return {unorm8(rgb[2]), unorm8(rgb[1]), unorm8(rgb[0]), unorm8(out_a)};
}

bool build_blob_geometry(const Terrain& t, const ShadowBlob& b, RenderMode mode, BlobGeometry& out) {
    out = BlobGeometry{};
    if (!terrain_ok(t)) return false;
    const DecorationFrame f = DecorationFrame::make(b.position[0], b.position[1], b.width, b.height, b.rotation);
    if (!f.valid) return false;
    uint32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    if (!grid_range(f.min_xy[0], f.max_xy[0], t.width, x0, x1) || !grid_range(f.min_xy[1], f.max_xy[1], t.height, y0, y1))
        return false;
    const auto color = blob_vertex_color(mode, b.color, b.intensity);
    const uint32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
    out.vertices.reserve(static_cast<size_t>(w) * h);
    bool first = true;
    out.max_height = -1e30f;
    for (uint32_t gy = y0; gy <= y1; ++gy)
        for (uint32_t gx = x0; gx <= x1; ++gx) {
            PrelitVertex v{};
            v.position[0] = static_cast<float>(gx) * kVertexSpacing;
            v.position[1] = static_cast<float>(gy) * kVertexSpacing;
            const float ht = t.height_at(gx, gy);
            out.max_height = std::max(out.max_height, ht);
            v.position[2] = ht + kBlobLift;
            set_color(v.color, color);
            const auto l = f.local(v.position[0], v.position[1]);
            v.texcoord[0] = l[0];
            v.texcoord[1] = 1.0f - l[1];
            grow(out.bounds, first, v.position);
            out.vertices.push_back(v);
        }
    out.indices.reserve(static_cast<size_t>(w - 1) * (h - 1) * 6);
    for (uint32_t r = 0; r + 1 < h; ++r)
        for (uint32_t c = 0; c + 1 < w; ++c) {
            const uint32_t a = r * w + c, bb = a + w;
            out.indices.insert(out.indices.end(), {a, a + 1, bb + 1, a, bb + 1, bb});
        }
    return true;
}

// ---------------------------------------------------------------- map tracks
namespace {

struct Pt2 {
    float x = 0, y = 0;
};

Pt2 cr_pos(const Pt2& p0, const Pt2& p1, const Pt2& p2, const Pt2& p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    auto f = [&](float a, float b, float c, float d) {
        return 0.5f * ((2.0f * b) + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 + (-a + 3.0f * b - 3.0f * c + d) * t3);
    };
    return {f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y)};
}

Pt2 cr_deriv(const Pt2& p0, const Pt2& p1, const Pt2& p2, const Pt2& p3, float t) {
    const float t2 = t * t;
    auto f = [&](float a, float b, float c, float d) {
        return 0.5f * ((-a + c) + 2.0f * (2.0f * a - 5.0f * b + 4.0f * c - d) * t + 3.0f * (-a + 3.0f * b - 3.0f * c + d) * t2);
    };
    return {f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y)};
}

}  // namespace

// Determinism note: std::hypot below (not correctly rounded, CRT-dependent) only shapes render
// geometry (map-track ribbons); no game-logic state reads it.
std::vector<SplineSample> sample_track_spline(const TerrainTrack& track, uint32_t max_samples) {
    std::vector<SplineSample> out;
    if (max_samples < 2) return out;
    // Point cap before anything is allocated: the arc table below holds 32 floats per segment.
    size_t finite_points = 0;
    for (const auto& q : track.points) finite_points += std::isfinite(q[0]) && std::isfinite(q[1]) ? 1u : 0u;
    if (finite_points < 2) return out;
    if (finite_points > kMaxTrackPoints) {
        log::warnf("terrain", "track '%s': %zu control points exceed the %u-point cap; rejected", track.texture.c_str(),
                   finite_points, kMaxTrackPoints);
        return out;
    }
    std::vector<Pt2> p;
    p.reserve(finite_points);
    for (const auto& q : track.points)
        if (std::isfinite(q[0]) && std::isfinite(q[1])) p.push_back({q[0], q[1]});
    const size_t segs = p.size() - 1;
    auto P = [&](int64_t i) { return p[static_cast<size_t>(std::clamp<int64_t>(i, 0, static_cast<int64_t>(p.size()) - 1))]; };
    auto width_at = [&](size_t seg, float t) {
        if (track.widths.empty()) return track.field_00;  // [L] no widths: the 00 default (10)
        const size_t last = track.widths.size() - 1;
        const float a = track.widths[std::min(seg, last)], b = track.widths[std::min(seg + 1, last)];
        return a + (b - a) * t;
    };
    // Arc-length table: 32 chords per segment.
    constexpr uint32_t kSteps = 32;
    std::vector<float> arc(segs * kSteps + 1, 0.0f);
    {
        Pt2 prev = p[0];
        size_t k = 1;
        for (size_t s = 0; s < segs; ++s) {
            const int64_t i = static_cast<int64_t>(s);
            for (uint32_t j = 1; j <= kSteps; ++j, ++k) {
                const Pt2 q = cr_pos(P(i - 1), P(i), P(i + 1), P(i + 2), static_cast<float>(j) / static_cast<float>(kSteps));
                arc[k] = arc[k - 1] + std::hypot(q.x - prev.x, q.y - prev.y);
                prev = q;
            }
        }
    }
    const float total = arc.back();
    // Finite points far apart (e.g. alternating +-1e37) overflow the arc length to +inf (or NaN
    // via inf - inf); every value derived from it would be garbage, and NaN -> integer is UB.
    if (!std::isfinite(total)) {
        log::warnf("terrain", "track '%s': non-finite arc length (%zu points); rejected", track.texture.c_str(), p.size());
        return out;
    }
    float spacing = track.sample_spacing > 0 ? static_cast<float>(track.sample_spacing) : 50.0f;  // [L] 0 -> default
    if (total / spacing + 2.0f > static_cast<float>(max_samples))
        spacing = max_samples > 2 ? total / static_cast<float>(max_samples - 2) : 0.0f;  // 0: end points only
    Pt2 last_tan{1.0f, 0.0f};
    bool bad = false;
    auto eval = [&](size_t table_index, float frac, float s_arc) {
        // table_index in [0, segs*kSteps); global parameter -> segment + local t
        const float g = (static_cast<float>(table_index) + frac) / static_cast<float>(kSteps);
        // Range- and NaN-safe float -> index (g >= 0 is false for NaN).
        size_t seg = 0;
        if (g >= static_cast<float>(segs)) seg = segs - 1;
        else if (g >= 0.0f) seg = static_cast<size_t>(g);
        const float lt = std::clamp(g - static_cast<float>(seg), 0.0f, 1.0f);
        const int64_t i = static_cast<int64_t>(seg);
        const Pt2 q = cr_pos(P(i - 1), P(i), P(i + 1), P(i + 2), lt);
        const Pt2 dq = cr_deriv(P(i - 1), P(i), P(i + 1), P(i + 2), lt);
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(lt)) bad = true;
        const float len = std::hypot(dq.x, dq.y);
        if (len > 1e-6f && std::isfinite(len)) last_tan = {dq.x / len, dq.y / len};
        SplineSample sm;
        sm.position[0] = q.x;
        sm.position[1] = q.y;
        sm.tangent[0] = last_tan.x;
        sm.tangent[1] = last_tan.y;
        sm.arc = s_arc;
        sm.width = width_at(seg, lt);
        out.push_back(sm);
    };
    size_t k = 0;
    const size_t n_table = arc.size() - 1;
    auto finish = [&]() {
        if (bad) {
            log::warnf("terrain", "track '%s': non-finite spline samples (%zu points); rejected", track.texture.c_str(), p.size());
            out.clear();
        }
    };
    if (!(total > 0.0f)) {
        eval(0, 0.0f, 0.0f);
        eval(n_table - 1, 1.0f, 0.0f);
        finish();
        return out;
    }
    if (!(spacing > 0.0f) || !std::isfinite(spacing)) {
        eval(0, 0.0f, 0.0f);
        eval(n_table - 1, 1.0f, total);
        finish();
        return out;
    }
    for (uint32_t i = 0;; ++i) {
        const float s = static_cast<float>(i) * spacing;  // finite: spacing finite, s < total + spacing
        if (!(s < total) || out.size() + 1 >= max_samples) break;
        while (k + 1 < n_table && arc[k + 1] < s) ++k;
        const float seg_len = arc[k + 1] - arc[k];
        float frac = 0.0f;
        if (seg_len > 0.0f) {
            const float r = (s - arc[k]) / seg_len;
            frac = r >= 1.0f ? 1.0f : (r > 0.0f ? r : 0.0f);  // NaN -> 0, +inf -> 1
        }
        eval(k, frac, s);
    }
    eval(n_table - 1, 1.0f, total);
    finish();
    return out;
}

std::array<float, 2> track_uv_offset(const TerrainTrack& track, float seconds) {
    auto frac = [](float v) {
        if (!std::isfinite(v)) return 0.0f;
        return v - std::floor(v);
    };
    return {frac(seconds * track.u_speed), frac(seconds * track.v_speed)};
}

namespace {

// One ribbon. The budget check (`vert_room` / `idx_room`, what is left of the caller's budget) runs
// after sampling the spline (bounded by kMaxTrackPoints / kMaxTrackVertices) and before the ribbon
// is allocated; `need_v` / `need_i` receive the ribbon size.
BuildStatus track_impl(const Terrain& t, const TerrainTrack& track, TrackGeometry& out, size_t vert_room, size_t idx_room,
                       size_t& need_v, size_t& need_i) {
    need_v = need_i = 0;
    out = TrackGeometry{};
    out.type = track.on_water() ? DecorationType::WaterTrack : DecorationType::LandTrack;
    out.texture = track.texture;
    out.clamp_u = track.clamp_u;
    out.clamp_v = track.clamp_v;
    out.u_speed = track.u_speed;
    out.v_speed = track.v_speed;
    if ((track.flags & 4u) == 0 || track.profile.size() < 2) return BuildStatus::Empty;
    // Budget: profile columns, then rows so that rows * columns <= kMaxTrackVertices (this also
    // bounds the indices: 6 (rows - 1)(n - 1) < 6 rows n <= kMaxTrackIndices).
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(track.profile.size(), kMaxTrackProfile));
    if (n < track.profile.size())
        log::warnf("terrain", "track '%s': profile of %zu entries clipped to %u", track.texture.c_str(), track.profile.size(), n);
    const uint32_t max_rows = std::min<uint32_t>(16384u, kMaxTrackVertices / n);
    const auto samples = sample_track_spline(track, max_rows);
    if (samples.size() < 2) return BuildStatus::Empty;
    if (samples.size() + 2 >= max_rows)  // a widened spacing yields max_rows - 1 .. max_rows samples
        log::warnf("terrain", "track '%s': %zu samples x %u columns hits the %u-vertex budget; sampling widened",
                   track.texture.c_str(), samples.size(), n, kMaxTrackVertices);
    const uint32_t rows = static_cast<uint32_t>(samples.size());
    need_v = static_cast<size_t>(rows) * n;
    need_i = static_cast<size_t>(rows - 1) * (n - 1) * 6;
    if (need_v > vert_room || need_i > idx_room) return BuildStatus::OverBudget;
    out.rows = rows;
    out.columns = n;
    out.length = samples.back().arc;
    const float repeat = track.repeat_length > 0.0f ? track.repeat_length : out.length;
    const float water_z = t.header.water_height + kWaterTrackLift;
    out.mesh.vertices.reserve(static_cast<size_t>(rows) * n);
    for (uint32_t r = 0; r < rows; ++r) {
        const SplineSample& sm = samples[r];
        const float px = -sm.tangent[1], py = sm.tangent[0];  // left perpendicular [L side]
        float row_mul = 1.0f;
        if (r == 0) row_mul = track.first_row_alpha;
        else if (r + 1 == rows) row_mul = track.last_row_alpha;
        for (uint32_t k = 0; k < n; ++k) {
            const float off = (0.5f - static_cast<float>(k) / static_cast<float>(n - 1)) * sm.width;
            LitVertex v{};
            v.position[0] = sm.position[0] + off * px;
            v.position[1] = sm.position[1] + off * py;
            std::array<float, 3> nrm{0.0f, 0.0f, 1.0f};
            if (track.height_mode == 0) {
                v.position[2] = water_z;
            } else {
                v.position[2] = t.sample_height(v.position[0], v.position[1]);
                if (track.height_mode == 2) v.position[2] += track.height_offset;
                nrm = terrain_normal_at(t, v.position[0], v.position[1]);
            }
            v.normal[0] = nrm[0];
            v.normal[1] = nrm[1];
            v.normal[2] = nrm[2];
            const float a = track.profile[k] * track.opacity * row_mul;
            set_color(v.color, {255, 255, 255, unorm8(a)});
            v.texcoord[0] = repeat > 0.0f ? sm.arc / repeat : 0.0f;
            v.texcoord[1] = static_cast<float>(k) / static_cast<float>(n - 1);
            out.mesh.vertices.push_back(v);
        }
    }
    out.mesh.indices.reserve(static_cast<size_t>(rows - 1) * (n - 1) * 6);
    for (uint32_t r = 0; r + 1 < rows; ++r)
        for (uint32_t k = 0; k + 1 < n; ++k) {
            // a = (r, k), b = (r, k+1) to the right, c = (r+1, k) ahead, d = ahead-right.
            const uint32_t a = r * n + k, b = a + 1, c = a + n, d = c + 1;
            if ((k & 1u) == 0) out.mesh.indices.insert(out.mesh.indices.end(), {a, b, d, a, d, c});
            else out.mesh.indices.insert(out.mesh.indices.end(), {a, b, c, b, d, c});
        }
    compute_bounds(out.mesh);
    return BuildStatus::Built;
}

}  // namespace

bool build_track_geometry(const Terrain& t, const TerrainTrack& track, TrackGeometry& out) {
    size_t nv = 0, ni = 0;
    return track_impl(t, track, out, SIZE_MAX, SIZE_MAX, nv, ni) == BuildStatus::Built;
}

size_t build_map_track_geometry(const Terrain& t, std::vector<TrackGeometry>& out) {
    out.clear();
    out.resize(t.tracks.size());
    size_t built = 0, verts = 0, idx = 0, over = 0;
    for (size_t i = 0; i < t.tracks.size(); ++i) {
        size_t nv = 0, ni = 0;
        const BuildStatus st = track_impl(t, t.tracks[i], out[i], kMaxMapTrackVertices - verts, kMaxMapTrackIndices - idx, nv, ni);
        if (st == BuildStatus::OverBudget) {
            if (over++ < kMaxBudgetLogs)
                log::warnf("terrain", "track %zu ('%s'): %zu vertices / %zu indices exceed the map budget (%u / %u); skipped", i,
                           t.tracks[i].texture.c_str(), nv, ni, kMaxMapTrackVertices, kMaxMapTrackIndices);
            continue;
        }
        if (st != BuildStatus::Built) continue;
        verts += out[i].mesh.vertices.size();
        idx += out[i].mesh.indices.size();
        ++built;
    }
    if (over > kMaxBudgetLogs)
        log::warnf("terrain", "%zu tracks: %zu over the map budget; %zu built", t.tracks.size(), over, built);
    return built;
}

// ---------------------------------------------------------------- dynamic tracks
float smoothstep01(float t) {
    if (!std::isfinite(t)) return 0.0f;
    t = clamp01(t);
    return (3.0f - 2.0f * t) * t * t;
}

const DynamicTrackParams* find_dynamic_track(const std::vector<DynamicTrackParams>& all, std::string_view name) {
    for (const DynamicTrackParams& p : all)
        if (iequals(p.name, name)) return &p;
    return nullptr;
}

DynamicTrackGenerator::DynamicTrackGenerator(const DynamicTrackParams& params) : params_(params) {
    if (!(params_.segment_length > 0.01f) || !std::isfinite(params_.segment_length)) params_.segment_length = 0.01f;
    if (!std::isfinite(params_.fade_distance_per_second)) params_.fade_distance_per_second = 0.0f;
}

void DynamicTrackGenerator::clear() {
    points_.clear();
    head_distance_ = 0.0f;
}

void DynamicTrackGenerator::trim_tail() {
    size_t drop = 0;
    while (drop + 1 < points_.size() && head_distance_ - points_[drop].distance > params_.fade_end_distance) ++drop;
    if (drop) points_.erase(points_.begin(), points_.begin() + static_cast<std::ptrdiff_t>(drop));
}

void DynamicTrackGenerator::advance(float dt) {
    if (std::isfinite(dt) && dt > 0.0f) head_distance_ += dt * params_.fade_distance_per_second;
    if (!points_.empty() && points_.size() >= 2) points_.back().distance = head_distance_;
    trim_tail();
}

void DynamicTrackGenerator::update(float x, float y, float dt, bool gap) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (std::isfinite(dt) && dt > 0.0f) head_distance_ += dt * params_.fade_distance_per_second;
    if (points_.empty()) {
        points_.push_back({{x, y}, head_distance_, gap});
        return;
    }
    const Point& h = points_.back();
    const float move = det_len2(x - h.position[0], y - h.position[1]);
    if (!std::isfinite(move)) {  // overflowing jump (|d| > ~1.8e19): restart the trail like a teleport
        points_.clear();
        points_.push_back({{x, y}, head_distance_, gap});
        return;
    }
    head_distance_ += move;
    if (points_.size() == 1) {
        if (move > 0.0f) points_.push_back({{x, y}, head_distance_, gap});
        else points_.back().distance = head_distance_;
    } else {
        points_.back() = {{x, y}, head_distance_, gap};
    }
    // Drop points every segment_length from the last fixed point towards the head.
    if (points_.size() >= 2) {
        const float seg = params_.segment_length;
        Point anchor = points_[points_.size() - 2];
        const Point head = points_.back();
        float dx = head.position[0] - anchor.position[0], dy = head.position[1] - anchor.position[1];
        float dist = det_len2(dx, dy);
        if (dist / seg > static_cast<float>(kMaxPoints)) {  // teleport: restart the trail [L]
            points_.clear();
            points_.push_back({{x, y}, head_distance_, gap});
            return;
        }
        while (dist >= seg) {
            const float f = seg / dist;
            Point p;
            p.position[0] = anchor.position[0] + dx * f;
            p.position[1] = anchor.position[1] + dy * f;
            p.distance = anchor.distance + (head.distance - anchor.distance) * f;  // [M] proportional
            p.gap = gap;
            points_.insert(points_.end() - 1, p);
            anchor = p;
            dx = head.position[0] - anchor.position[0];
            dy = head.position[1] - anchor.position[1];
            dist = det_len2(dx, dy);
        }
        if (points_.size() > kMaxPoints) points_.erase(points_.begin(), points_.end() - static_cast<std::ptrdiff_t>(kMaxPoints));
    }
    trim_tail();
}

float DynamicTrackGenerator::point_alpha(size_t i) const {
    if (i >= points_.size() || points_[i].gap) return 0.0f;
    const float range = params_.fade_end_distance - params_.fade_begin_distance;
    const float behind = head_distance_ - points_[i].distance;
    float t = 0.0f;
    if (range > 0.0f) t = (behind - params_.fade_begin_distance) / range;
    else t = behind >= params_.fade_end_distance ? 1.0f : 0.0f;
    return params_.opacity * (1.0f - smoothstep01(t));
}

// Render-only: the std::hypot calls here only shape the ribbon mesh; the generator state that game
// logic may read (points_, head_distance_) is updated with det_len2 in update().
bool DynamicTrackGenerator::build(const Terrain& t, DecorationMesh& out, const DynamicTrackBuildOptions& opt) const {
    out = DecorationMesh{};
    // Drop coincident consecutive points (zero-length segments have no direction).
    std::vector<size_t> idx;
    for (size_t i = 0; i < points_.size(); ++i) {
        if (!idx.empty()) {
            const Point& a = points_[idx.back()];
            if (a.position[0] == points_[i].position[0] && a.position[1] == points_[i].position[1]) {
                if (i + 1 == points_.size()) idx.back() = i;  // keep the head's alpha
                continue;
            }
        }
        idx.push_back(i);
    }
    const size_t n = idx.size();
    if (n < 2) return false;
    const float w = params_.width;
    const float seg_len = params_.segment_length;
    std::vector<Pt2> dir(n - 1), left(n), right(n);
    for (size_t i = 0; i + 1 < n; ++i) {
        const Point &a = points_[idx[i]], &b = points_[idx[i + 1]];
        const float dx = b.position[0] - a.position[0], dy = b.position[1] - a.position[1];
        const float len = std::hypot(dx, dy);
        dir[i] = {dx / len, dy / len};
    }
    for (size_t i = 0; i < n; ++i) {
        Pt2 off;
        const Pt2 nin = i > 0 ? Pt2{-dir[i - 1].y, dir[i - 1].x} : Pt2{-dir[0].y, dir[0].x};
        const Pt2 nout = i + 1 < n ? Pt2{-dir[i].y, dir[i].x} : nin;
        const Pt2 m{nin.x + nout.x, nin.y + nout.y};
        const float l2 = m.x * m.x + m.y * m.y;
        if (l2 < 1e-6f) {
            off = {nout.x * w, nout.y * w};
        } else {  // miter: (n1 + n2) * 2 / |n1 + n2|^2, length capped at 4 w [L]
            off = {m.x * (2.0f / l2) * w, m.y * (2.0f / l2) * w};
            const float ol = std::hypot(off.x, off.y);
            if (ol > 4.0f * std::fabs(w) && ol > 0.0f) {
                off.x *= 4.0f * std::fabs(w) / ol;
                off.y *= 4.0f * std::fabs(w) / ol;
            }
        }
        const Point& p = points_[idx[i]];
        left[i] = {p.position[0] + off.x, p.position[1] + off.y};
        right[i] = {p.position[0] - off.x, p.position[1] - off.y};
    }
    const bool bump = params_.mode == RenderMode::DecalBumpAlpha;
    const float max_q = opt.max_quad_length > 1.0f ? opt.max_quad_length : 1.0f;
    for (size_t i = 0; i + 1 < n; ++i) {
        const float a0 = point_alpha(idx[i]), a1 = point_alpha(idx[i + 1]);
        uint8_t rgb[3] = {255, 255, 255};
        if (bump || opt.direction_in_color) {
            rgb[0] = pack_unit(dir[i].x);
            rgb[1] = pack_unit(dir[i].y);
            rgb[2] = 0x7F;
        }
        const Point &pa = points_[idx[i]], &pb = points_[idx[i + 1]];
        const float centre_len = std::hypot(pb.position[0] - pa.position[0], pb.position[1] - pa.position[1]);
        const float quads = std::ceil(centre_len / max_q);
        // Range- and NaN-safe float -> integer: NaN -> 1, +inf / huge -> 256.
        const uint32_t m = quads >= 256.0f ? 256u : (quads > 1.0f ? static_cast<uint32_t>(quads) : 1u);
        const float el = std::hypot(left[i + 1].x - left[i].x, left[i + 1].y - left[i].y) / seg_len;
        const float er = std::hypot(right[i + 1].x - right[i].x, right[i + 1].y - right[i].y) / seg_len;
        const uint32_t base = static_cast<uint32_t>(out.vertices.size());
        for (uint32_t j = 0; j <= m; ++j) {
            const float f = static_cast<float>(j) / static_cast<float>(m);
            const float alpha = a0 + (a1 - a0) * f;
            for (int side = 0; side < 2; ++side) {
                const Pt2& s0 = side == 0 ? left[i] : right[i];
                const Pt2& s1 = side == 0 ? left[i + 1] : right[i + 1];
                const float e = side == 0 ? el : er;
                LitVertex v{};
                v.position[0] = s0.x + (s1.x - s0.x) * f;
                v.position[1] = s0.y + (s1.y - s0.y) * f;
                v.position[2] = t.sample_height(v.position[0], v.position[1]) + kDynamicTrackLift;
                const auto nrm = terrain_normal_at(t, v.position[0], v.position[1]);
                v.normal[0] = nrm[0];
                v.normal[1] = nrm[1];
                v.normal[2] = nrm[2];
                set_color(v.color, {rgb[2], rgb[1], rgb[0], unorm8(alpha)});
                v.texcoord[0] = side == 0 ? 0.0f : 1.0f;
                v.texcoord[1] = 0.5f - 0.5f * e + e * f;
                out.vertices.push_back(v);
            }
        }
        for (uint32_t j = 0; j < m; ++j) {
            const uint32_t l0 = base + 2 * j, r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
            out.indices.insert(out.indices.end(), {l0, r0, r1, l0, r1, l1});
        }
    }
    compute_bounds(out);
    return true;
}

}  // namespace alamo::terrain
