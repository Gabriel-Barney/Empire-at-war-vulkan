// Terrain mesh build (TERRAIN_WATER_POST.md 2.1-2.3). See mesh.hpp.
#include "alamo/terrain/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace alamo::terrain {

namespace {

float radical_inverse(uint32_t i, uint32_t base) {
    double f = 1.0, r = 0.0;
    while (i > 0) {
        f /= base;
        r += f * static_cast<double>(i % base);
        i /= base;
    }
    return static_cast<float>(r);
}

// Render-only: std::cos/sin (CRT-dependent, not correctly rounded) build the vertex normal table.
std::array<std::array<float, 3>, 256> make_normal_table() {
    std::array<std::array<float, 3>, 256> t{};
    constexpr double kTwoPi = 6.283185307179586;
    for (uint32_t i = 0; i < 255; ++i) {
        const double u = radical_inverse(i, 2), v = radical_inverse(i, 3);
        const double s = std::sqrt(1.0 - v);
        t[i] = {static_cast<float>(s * std::cos(kTwoPi * u)), static_cast<float>(s * std::sin(kTwoPi * u)),
                static_cast<float>(std::sqrt(v))};
    }
    t[255] = {0.0f, 0.0f, 1.0f};
    return t;
}

inline uint8_t clamp_mat(uint8_t m) { return m < kMaterialSlots ? m : static_cast<uint8_t>(kMaterialSlots - 1); }

void push_quad(std::vector<uint16_t>& idx, uint32_t c, uint32_t r) {
    const uint32_t v = r * kPatchVerts + c;
    const uint16_t a = static_cast<uint16_t>(v), b = static_cast<uint16_t>(v + 1);
    const uint16_t d = static_cast<uint16_t>(v + kPatchVerts + 1), e = static_cast<uint16_t>(v + kPatchVerts);
    idx.insert(idx.end(), {a, b, d, a, d, e});
}

}  // namespace

const std::array<std::array<float, 3>, 256>& normal_table() {
    static const auto table = make_normal_table();
    return table;
}

uint8_t quantize_normal(const std::array<float, 3>& n) {
    const auto& t = normal_table();
    uint32_t best = 0;
    float best_dot = -2.0f;
    for (uint32_t i = 0; i < 256; ++i) {
        const float d = t[i][0] * n[0] + t[i][1] * n[1] + t[i][2] * n[2];
        if (d > best_dot) {
            best_dot = d;
            best = i;
        }
    }
    return static_cast<uint8_t>(best);
}

std::array<float, 3> terrain_normal(const Terrain& t, int64_t x, int64_t y) {
    auto h = [&](int64_t dx, int64_t dy) { return t.height_at(x + dx, y + dy); };
    // Sobel 1-2-1; each difference spans 2 cells (2 * 20 world units), weights sum to 4.
    const float gx = ((h(1, -1) + 2.0f * h(1, 0) + h(1, 1)) - (h(-1, -1) + 2.0f * h(-1, 0) + h(-1, 1))) /
                     (4.0f * 2.0f * kVertexSpacing);
    const float gy = ((h(-1, 1) + 2.0f * h(0, 1) + h(1, 1)) - (h(-1, -1) + 2.0f * h(0, -1) + h(1, -1))) /
                     (4.0f * 2.0f * kVertexSpacing);
    const float nx = -gx, ny = -gy, nz = 1.0f;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    return {nx / len, ny / len, nz / len};
}

std::array<float, 2> material_uv(const TerrainMaterial& m, const float (&p)[3]) {
    const auto u = m.tex_u(), v = m.tex_v();
    return {u[0] * p[0] + u[1] * p[1] + u[2] * p[2] + u[3], v[0] * p[0] + v[1] * p[1] + v[2] * p[2] + v[3]};
}

uint16_t TerrainPass::texel(uint32_t c, uint32_t r) const {
    if (c >= kBlendTextureSize || r >= kBlendTextureSize || blend_texture.size() < kBlendTextureBytes) return 0;
    const size_t o = 2 * (static_cast<size_t>(r) * kBlendTextureSize + c);
    return static_cast<uint16_t>(blend_texture[o] | (blend_texture[o + 1] << 8));
}

std::vector<uint8_t> blend_texture_r8(const TerrainPass& pass) {
    std::vector<uint8_t> r(kBlendTextureSize * kBlendTextureSize);
    for (uint32_t y = 0; y < kBlendTextureSize; ++y)
        for (uint32_t x = 0; x < kBlendTextureSize; ++x)
            r[y * kBlendTextureSize + x] = (pass.texel(x, y) >> 12) ? uint8_t{255} : uint8_t{0};
    return r;
}

void build_patch(const Terrain& t, uint32_t px, uint32_t py, const MeshOptions& opt, TerrainPatch& out) {
    out = TerrainPatch{};
    out.px = px;
    out.py = py;
    const uint32_t x0 = px * kPatchQuads, y0 = py * kPatchQuads;

    // Vertices.
    std::vector<TerrainVertex> verts(kPatchVerts * kPatchVerts);
    uint8_t mat[kPatchVerts * kPatchVerts];
    float zmin = 0, zmax = 0;
    const auto& table = normal_table();
    for (uint32_t r = 0; r < kPatchVerts; ++r) {
        for (uint32_t c = 0; c < kPatchVerts; ++c) {
            const int64_t gx = x0 + c, gy = y0 + r;
            TerrainVertex& v = verts[r * kPatchVerts + c];
            const float wx = static_cast<float>(gx) * kVertexSpacing, wy = static_cast<float>(gy) * kVertexSpacing;
            const float wz = t.height_at(gx, gy);
            v.position[0] = wx;
            v.position[1] = wy;
            v.position[2] = wz;
            std::array<float, 3> n = terrain_normal(t, gx, gy);
            if (opt.quantize_normals) n = table[quantize_normal(n)];
            std::copy(n.begin(), n.end(), v.normal);
            if (t.header.use_vertex_brightness) {
                const size_t i = static_cast<size_t>(gy) * t.width + static_cast<size_t>(gx);
                const uint8_t b = i < t.brightness.size() ? t.brightness[i] : uint8_t{0xFF};
                v.color[0] = v.color[1] = v.color[2] = b;
                v.color[3] = 0xFF;
            } else {
                v.color[0] = v.color[1] = v.color[2] = v.color[3] = 0xFF;
            }
            v.texcoord[0] = wx;
            v.texcoord[1] = wy;
            mat[r * kPatchVerts + c] = clamp_mat(t.material_at(gx, gy));
            if (r == 0 && c == 0) zmin = zmax = wz;
            zmin = std::min(zmin, wz);
            zmax = std::max(zmax, wz);
        }
    }
    out.vertices.resize(verts.size() * sizeof(TerrainVertex));
    std::memcpy(out.vertices.data(), verts.data(), out.vertices.size());
    const float ext = static_cast<float>(kPatchQuads) * kVertexSpacing;
    const float minx = static_cast<float>(x0) * kVertexSpacing, miny = static_cast<float>(y0) * kVertexSpacing;
    out.bounds.min = {minx, miny, zmin};
    out.bounds.max = {minx + ext, miny + ext, zmax};
    out.min_height = zmin;
    out.max_height = zmax;
    const float scale = (62.0f / 64.0f) / ext;
    out.blend_tex_scale[0] = out.blend_tex_scale[1] = scale;
    out.blend_tex_offset[0] = 1.0f / 64.0f - minx * scale;
    out.blend_tex_offset[1] = 1.0f / 64.0f - miny * scale;

    // Materials used (ascending), skipping "not rendered" slots.
    bool present[kMaterialSlots] = {};
    for (uint8_t m : mat) present[m] = true;
    for (uint32_t m = 0; m < kMaterialSlots; ++m)
        if (present[m] && t.materials[m].not_rendered) out.has_holes = true;

    for (uint32_t m = 0; m < kMaterialSlots; ++m) {
        if (!present[m] || t.materials[m].not_rendered) continue;
        TerrainPass pass;
        pass.material = static_cast<uint8_t>(m);
        std::vector<uint16_t> tex(kBlendTextureSize * kBlendTextureSize, 0);
        for (uint32_t i = 0; i < kPatchVerts * kPatchVerts; ++i)
            if (mat[i] == m) tex[i] = kBlendTexelOn;
        const bool full = t.materials[m].full_coverage;
        for (uint32_t r = 0; r < kPatchQuads; ++r) {
            for (uint32_t c = 0; c < kPatchQuads; ++c) {
                const uint32_t i00 = r * kPatchVerts + c, i10 = i00 + 1, i01 = i00 + kPatchVerts, i11 = i01 + 1;
                const uint8_t a = mat[i00], b = mat[i10], d = mat[i01], e = mat[i11];
                if (a != m && b != m && d != m && e != m) continue;
                push_quad(pass.indices, c, r);
                const uint8_t lowest = std::min(std::min(a, b), std::min(d, e));
                if (lowest == m || full) tex[i00] = tex[i10] = tex[i01] = tex[i11] = kBlendTexelOn;
            }
        }
        pass.blend_texture.resize(kBlendTextureBytes);
        for (size_t i = 0; i < tex.size(); ++i) {
            pass.blend_texture[2 * i] = static_cast<uint8_t>(tex[i] & 0xFF);
            pass.blend_texture[2 * i + 1] = static_cast<uint8_t>(tex[i] >> 8);
        }
        out.passes.push_back(std::move(pass));
    }

    if (out.has_holes) {
        auto rendered = [&](uint8_t m) { return !t.materials[m].not_rendered; };
        for (uint32_t r = 0; r < kPatchQuads; ++r)
            for (uint32_t c = 0; c < kPatchQuads; ++c) {
                const uint32_t i00 = r * kPatchVerts + c;
                if (rendered(mat[i00]) || rendered(mat[i00 + 1]) || rendered(mat[i00 + kPatchVerts]) ||
                    rendered(mat[i00 + kPatchVerts + 1]))
                    push_quad(out.rendered_indices, c, r);
            }
    }
}

bool build_terrain_mesh(const Terrain& t, TerrainMesh& out, const MeshOptions& opt, const ParallelFor& parallel,
                        std::string* error) {
    out = TerrainMesh{};
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (!t.present) return fail("map has no terrain");
    if (t.width < kPatchVerts || t.height < kPatchVerts || (t.width - 1) % kPatchQuads || (t.height - 1) % kPatchQuads)
        return fail("terrain grid is not (31n+1)^2");
    const size_t n = static_cast<size_t>(t.width) * t.height;
    if (t.heights.size() != n || t.material.size() != n || t.brightness.size() != n)
        return fail("terrain vertex arrays do not match the grid");
    out.patches_x = t.patches_x();
    out.patches_y = t.patches_y();
    const uint32_t count = out.patches_x * out.patches_y;
    out.patches.resize(count);
    (void)normal_table();  // build the table before going parallel
    const RangeFn body = [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) build_patch(t, i % out.patches_x, i / out.patches_x, opt, out.patches[i]);
    };
    if (parallel) parallel(count, 1, body);
    else serial_for(count, 1, body);

    TerrainMeshStats& s = out.stats;
    s.patches = count;
    for (const TerrainPatch& p : out.patches) {
        s.passes += static_cast<uint32_t>(p.passes.size());
        s.max_passes = std::max(s.max_passes, static_cast<uint32_t>(p.passes.size()));
        s.vertex_bytes += p.vertices.size();
        s.index_bytes += p.rendered_indices.size() * 2;
        for (const TerrainPass& q : p.passes) {
            s.triangles += q.triangle_count();
            s.index_bytes += q.indices.size() * 2;
            s.blend_bytes += q.blend_texture.size();
            out.materials[q.material].used = true;
        }
    }
    for (uint32_t m = 0; m < kMaterialSlots; ++m) {
        const TerrainMaterial& src = t.materials[m];
        TerrainMaterialGpu& g = out.materials[m];
        g.diffuse_texture = src.diffuse_texture;
        g.normal_texture = src.effective_normal_texture();
        g.tex_u = src.tex_u();
        g.tex_v = src.tex_v();
        g.material_diffuse = {src.diffuse_color[0], src.diffuse_color[1], src.diffuse_color[2], 1.0f};
        g.material_specular = {src.specular_color[0], src.specular_color[1], src.specular_color[2], 1.0f};
        g.shininess = src.specular_power;
    }
    return true;
}

}  // namespace alamo::terrain
