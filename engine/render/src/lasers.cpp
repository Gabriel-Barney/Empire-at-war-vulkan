// alamo_render - procedural laser projectiles (docs/re/PROJECTILE_NEBULA_DRAW.md 1). See lasers.hpp.
#include "alamo/render/lasers.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <cmath>
#include <string_view>

#include "alamo/core/log.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/render/renderer.hpp"
#include "render_internal.hpp"

namespace alamo::render {

namespace {

struct D3 {
    double x = 0, y = 0, z = 0;
};
D3 d3(const Float3& f) { return {f.x, f.y, f.z}; }
D3 operator+(const D3& a, const D3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(const D3& a, const D3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(const D3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const D3& a, const D3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
bool normalise(D3& a) {
    const double l = std::sqrt(dot(a, a));
    if (!(l > 1e-12)) return false;
    a = a * (1.0 / l);
    return true;
}

// General 4x4 inverse (Gauss-Jordan, partial pivoting) of the camera's view_proj.
bool invert(const Mat4& m, double (&inv)[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? static_cast<double>(m.m[r][c]) : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (!(std::fabs(a[p][c]) > 1e-30)) return false;
        if (p != c)
            for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
        const double s = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= s;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f == 0.0) continue;
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inv[r][c] = a[r][c + 4];
    return true;
}

// The original's screen-space construction (1.2 step 2): A and B projected to the viewport (D3D pixels, y down), the
// screen perpendicular P = A + (-(B-A).y, (B-A).x), all three unprojected at A's depth [port: the depth the original
// unprojects at is not stated; A's keeps the directions in the plane through A parallel to the screen].
// a = normalise(B' - A'), p = normalise(P' - A'). A degenerate screen axis (head-on) uses the camera's up / right.
struct Frame {
    D3 a, p;
    double depth = 0;  // view depth d of the midpoint (positive in front)
};
bool screen_frame(const ViewMatrices& v, const D3& A, const D3& B, Frame& out) {
    const double W = v.width ? v.width : 1, H = v.height ? v.height : 1;
    auto clip = [&](const D3& p, double (&c)[4]) {
        for (int r = 0; r < 4; ++r)
            c[r] = double(v.view_proj.m[r][0]) * p.x + double(v.view_proj.m[r][1]) * p.y + double(v.view_proj.m[r][2]) * p.z +
                   double(v.view_proj.m[r][3]);
    };
    double ca[4], cb[4];
    clip(A, ca);
    clip(B, cb);
    const D3 mid = (A + B) * 0.5;
    out.depth = -(double(v.view.m[2][0]) * mid.x + double(v.view.m[2][1]) * mid.y + double(v.view.m[2][2]) * mid.z + double(v.view.m[2][3]));
    if (!(out.depth > 0.0)) return false;
    const D3 cam_right{v.view.m[0][0], v.view.m[0][1], v.view.m[0][2]}, cam_up{v.view.m[1][0], v.view.m[1][1], v.view.m[1][2]};
    double inv[4][4];
    if (!(ca[3] > 1e-9) || !(cb[3] > 1e-9) || !invert(v.view_proj, inv)) {  // an end behind the camera: camera axes
        out.a = B - A;
        if (!normalise(out.a)) out.a = cam_up;
        out.p = cam_right;
        return true;
    }
    const double ax = (ca[0] / ca[3] + 1.0) * 0.5 * W, ay = (1.0 - ca[1] / ca[3]) * 0.5 * H;
    const double bx = (cb[0] / cb[3] + 1.0) * 0.5 * W, by = (1.0 - cb[1] / cb[3]) * 0.5 * H;
    const double px = ax - (by - ay), py = ay + (bx - ax);
    const double zn = ca[2] / ca[3], w = ca[3];
    auto unproject = [&](double sx, double sy) {
        const double c[4] = {(sx / W * 2.0 - 1.0) * w, (1.0 - sy / H * 2.0) * w, zn * w, w};
        double o[4];
        for (int r = 0; r < 4; ++r) o[r] = inv[r][0] * c[0] + inv[r][1] * c[1] + inv[r][2] * c[2] + inv[r][3] * c[3];
        const double s = std::fabs(o[3]) > 1e-30 ? 1.0 / o[3] : 1.0;
        return D3{o[0] * s, o[1] * s, o[2] * s};
    };
    const D3 a0 = unproject(ax, ay), b0 = unproject(bx, by), p0 = unproject(px, py);
    out.a = b0 - a0;
    out.p = p0 - a0;
    if (!normalise(out.a) || !normalise(out.p)) {  // head-on: no screen axis
        out.a = cam_up;
        out.p = cam_right;
    }
    return true;
}

uint32_t d3dcolor(const uint8_t (&rgba)[4]) {
    return uint32_t{rgba[2]} | (uint32_t{rgba[1]} << 8) | (uint32_t{rgba[0]} << 16) | (uint32_t{rgba[3]} << 24);
}

PrimVertex vertex(const D3& p, uint32_t color, float u, float v) {
    PrimVertex o;
    o.pos[0] = static_cast<float>(p.x);
    o.pos[1] = static_cast<float>(p.y);
    o.pos[2] = static_cast<float>(p.z);
    o.color = color;
    o.uv[0] = u;
    o.uv[1] = v;
    return o;
}

}  // namespace

bool parse_laser_constants(std::span<const uint8_t> xml, LaserConstants& out) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, "GameConstants.xml");
    return parse_laser_constants(doc.root(), out);
}

bool parse_laser_constants(const data::XmlNode& root, LaserConstants& out) {
    if (!root) return false;
    auto value = [&](std::string_view tag, float& v) {
        const data::XmlNode n = root.child(tag);
        float f = 0.0f;
        if (n && data::parse_floats(n.text_trimmed(), &f, 1) == 1 && std::isfinite(f)) v = f;
    };
    value("Laser_Beam_Z_Scale_Factor", out.beam_z_scale);
    value("Laser_Kite_Z_Scale_Factor", out.kite_z_scale);
    return true;
}

void projection_near_far(const Mat4& proj, float& znear, float& zfar) {
    // perspective_rh: m22 = f / (n - f), m23 = n f / (n - f) -> n = m23 / m22, f = m23 / (m22 + 1).
    const float m22 = proj.m[2][2], m23 = proj.m[2][3];
    znear = m22 != 0.0f ? m23 / m22 : 10.0f;
    zfar = m22 != -1.0f ? m23 / (m22 + 1.0f) : 7000.0f;
}

float laser_depth_factor(float d, float znear, float zfar, float z_scale) {
    const float range = zfar - znear;
    return range > 0.0f ? 1.0f + z_scale * (d - znear) / range : 1.0f;
}

bool laser_beam_vertices(const Laser& l, const ViewMatrices& view, float z_scale, PrimVertex (&out)[6]) {
    D3 dir = d3(l.direction);
    if (!normalise(dir) || !(l.length >= 0.0f) || !(l.width > 0.0f)) return false;
    const D3 O = d3(l.position);
    const D3 A = O - dir * (0.5 * l.length), B = O + dir * (0.5 * l.length);  // local -Y / +Y ends (1.2 step 1)
    Frame fr;
    if (!screen_frame(view, A, B, fr)) return false;
    float zn = 10.0f, zf = 7000.0f;
    projection_near_far(view.proj, zn, zf);
    const double k = double(l.width) * laser_depth_factor(static_cast<float>(fr.depth), zn, zf, z_scale);
    const float du = 0.25f * static_cast<float>(l.slot_col), dv = 0.25f * static_cast<float>(l.slot_row);  // 4 x 4 atlas
    const uint32_t white = 0xFFFFFFFFu;
    out[0] = vertex(A - fr.a * k, white, 0.25f + du, 0.0f + dv);  // tail cap tip
    out[1] = vertex(A + fr.p * k, white, 0.125f + du, 0.0f + dv);
    out[2] = vertex(A - fr.p * k, white, 0.25f + du, 0.125f + dv);
    out[3] = vertex(B + fr.p * k, white, 0.0f + du, 0.125f + dv);
    out[4] = vertex(B - fr.p * k, white, 0.125f + du, 0.25f + dv);
    out[5] = vertex(B + fr.a * k, white, 0.0f + du, 0.25f + dv);  // head cap tip
    return true;
}

bool laser_kite_vertices(const Laser& l, const ViewMatrices& view, float z_scale, PrimVertex (&out)[4]) {
    D3 dir = d3(l.direction);
    if (!normalise(dir) || !(l.length >= 0.0f) || !(l.width > 0.0f)) return false;
    const D3 O = d3(l.position);
    const D3 A = O - dir * (0.5 * l.length), B = O + dir * (0.5 * l.length);
    Frame fr;
    if (!screen_frame(view, A, B, fr)) return false;
    float zn = 10.0f, zf = 7000.0f;
    projection_near_far(view.proj, zn, zf);
    const double k = double(l.width) * laser_depth_factor(static_cast<float>(fr.depth), zn, zf, z_scale);
    const D3 ab = A - B, vx{view.view.m[0][0], view.view.m[0][1], view.view.m[0][2]}, vy{view.view.m[1][0], view.view.m[1][1], view.view.m[1][2]};
    const double lp = std::max(1.0, std::sqrt(dot(ab, vx) * dot(ab, vx) + dot(ab, vy) * dot(ab, vy)));  // L_perp, >= 1
    const float du = 0.125f * static_cast<float>(l.slot_col), dv = 0.125f * static_cast<float>(l.slot_row);  // 8 x 8 atlas
    const uint32_t c = d3dcolor(l.color);
    out[0] = vertex(O + fr.p * k, c, 0.0f + du, 0.125f + dv);
    out[1] = vertex(O + fr.a * (k * lp), c, 0.0f + du, 0.0f + dv);  // nose
    out[2] = vertex(O - fr.p * k, c, 0.125f + du, 0.0f + dv);
    out[3] = vertex(O - fr.a * k, c, 0.125f + du, 0.125f + dv);  // tail
    return true;
}

void LaserGeometry::build(std::span<const Laser> lasers, const ViewMatrices& view, const LaserConstants& k) {
    beam_vertices.clear();
    kite_vertices.clear();
    beam_indices.clear();
    kite_indices.clear();
    beams = kites = skipped = 0;
    for (const Laser& l : lasers) {
        if (l.kind == LaserKind::Beam) {
            PrimVertex v[6];
            if (!laser_beam_vertices(l, view, k.beam_z_scale, v)) {
                ++skipped;
                continue;
            }
            const uint32_t base = static_cast<uint32_t>(beam_vertices.size());
            beam_vertices.insert(beam_vertices.end(), v, v + 6);
            for (uint32_t i : kLaserBeamIndices) beam_indices.push_back(base + i);
            ++beams;
        } else {
            PrimVertex v[4];
            if (!laser_kite_vertices(l, view, k.kite_z_scale, v)) {
                ++skipped;
                continue;
            }
            const uint32_t base = static_cast<uint32_t>(kite_vertices.size());
            kite_vertices.insert(kite_vertices.end(), v, v + 4);
            for (uint32_t i : kLaserKiteIndices) kite_indices.push_back(base + i);
            ++kites;
        }
    }
}

}  // namespace alamo::render

// ------------------------------------------------------------------------------------------ renderer side
namespace alamo::render {

bool Renderer::init_lasers(const TextureSource& textures) {
    if (!ctx_ || !desc_.effects) return false;
    shutdown_lasers();
    laser_fx_ = desc_.effects->get("PrimAdditive");
    laser_pass_ = nullptr;
    if (laser_fx_)
        for (const materials::ResolvedPass& p : laser_fx_->passes) {
            if (p.restore_only) continue;
            if (p.fixed_function && p.ff) laser_pass_ = &p;
            break;
        }
    if (!laser_pass_ || !laser_fx_->effect) {
        log::warnf("render", "lasers: Engine\\PrimAdditive has no fixed-function pass; lasers not drawn");
        return false;
    }
    auto tpl = std::make_shared<materials::FfDrawTemplate>();
    const materials::ParamStore store;
    std::string err;
    if (!tpl->build(*laser_pass_->ff, *laser_fx_->effect, store.lookup(), true, &err)) {
        log::warnf("render", "lasers: PrimAdditive template: %s", err.c_str());
        return false;
    }
    laser_tpl_ = std::move(tpl);
    const char* names[2] = {kLaserBeamTexture, kLaserKiteTexture};
    for (int i = 0; i < 2; ++i) {
        GpuTexture& t = laser_tex_[i];
        t.name = names[i];
        t.role = assets::TextureRole::Color;
        std::shared_ptr<const assets::TextureAsset> a = textures ? textures(t.name, t.role) : nullptr;
        assets::TextureAsset fb;
        if (!a) {
            fb = assets::make_fallback_texture(t.role);
            t.fallback = true;
            log::warnf("render", "lasers: texture %s not found, using a fallback", t.name.c_str());
        }
        if (!upload_texture(t, a ? *a : fb)) return false;
    }
    lasers_init_ = true;
    return true;
}

bool Renderer::lasers_ready() const {
    return lasers_init_ && laser_tex_[0].resident && laser_tex_[1].resident;
}

void Renderer::shutdown_lasers() {
    for (GpuTexture& t : laser_tex_) {
        if (t.image) release_texture(t.resident ? t.slot : 0u, &t.image);
        t = GpuTexture{};
    }
    for (rhi::Buffer& b : laser_vb_) {
        release_buffer(b);
        b = rhi::Buffer{};
    }
    for (VkDeviceSize& s : laser_vb_size_) s = 0;
    laser_tpl_.reset();
    laser_pass_ = nullptr;
    laser_fx_.reset();
    lasers_init_ = false;
}

void Renderer::draw_lasers(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, DrawStats& st) {
    laser_geo_.build(lasers_, view, laser_k_);
    if (laser_geo_.beams + laser_geo_.kites == 0) return;
    for (GpuTexture& t : laser_tex_)  // resident once the upload landed: bindless slot for the FF stage
        if (t.image && !t.resident && up_->ready(t.ticket)) {
            t.slot = allocate_texture_slot(t.image.view, "laser");
            t.resident = true;
        }
    if (!lasers_ready()) {
        st.laser_pending += (laser_geo_.beams ? 1u : 0u) + (laser_geo_.kites ? 1u : 0u);
        return;
    }
    // One per-slot dynamic buffer: beam vertices, kite vertices, then the 32-bit indices of both.
    const VkDeviceSize vb0 = laser_geo_.beam_vertices.size() * sizeof(PrimVertex);
    const VkDeviceSize vb1 = laser_geo_.kite_vertices.size() * sizeof(PrimVertex);
    const VkDeviceSize ib0 = (laser_geo_.beam_indices.size() + laser_geo_.kite_indices.size()) * sizeof(uint32_t);
    const VkDeviceSize kite_off = (vb0 + 63) / 64 * 64, ib_off = (kite_off + vb1 + 63) / 64 * 64, need = ib_off + ib0;
    const uint32_t slot = frame.slot() % 4;
    rhi::Buffer& buf = laser_vb_[slot];
    if (laser_vb_size_[slot] < need) {
        if (buf) alloc_->destroy(buf);  // this slot's previous frame has completed
        const VkDeviceSize size = std::max<VkDeviceSize>(need + need / 2, 16 * 1024);
        if (!alloc_->create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rhi::MemoryUsage::Dynamic,
                                   buf, "render.lasers") ||
            !buf.mapped()) {
            if (buf) alloc_->destroy(buf);
            laser_vb_size_[slot] = 0;
            ++st.laser_pending;
            return;
        }
        laser_vb_size_[slot] = size;
    }
    auto* base = static_cast<uint8_t*>(buf.mapped());
    if (vb0) std::memcpy(base, laser_geo_.beam_vertices.data(), static_cast<size_t>(vb0));
    if (vb1) std::memcpy(base + kite_off, laser_geo_.kite_vertices.data(), static_cast<size_t>(vb1));
    if (!laser_geo_.beam_indices.empty())
        std::memcpy(base + ib_off, laser_geo_.beam_indices.data(), laser_geo_.beam_indices.size() * sizeof(uint32_t));
    if (!laser_geo_.kite_indices.empty())
        std::memcpy(base + ib_off + laser_geo_.beam_indices.size() * sizeof(uint32_t), laser_geo_.kite_indices.data(),
                    laser_geo_.kite_indices.size() * sizeof(uint32_t));
    alloc_->flush(buf.alloc);
    FfGeoDraw d[2];
    uint32_t n = 0;
    auto add = [&](VkDeviceSize vb_offset, uint32_t first_index, size_t index_count, const GpuTexture& t) {
        FfGeoDraw& g = d[n++];
        g.fx = laser_fx_;
        g.pass = laser_pass_;
        g.tpl = laser_tpl_.get();
        g.layout = materials::VertexLayout::PrimVert;
        g.vb = buf.handle;
        g.vb_offset = vb_offset;
        g.ib = buf.handle;
        g.first_index = first_index;
        g.index_count = static_cast<uint32_t>(index_count);
        g.tex[0] = t.slot;
        g.sampler[0] = materials::kSamplerWrapLinear;
        g.cull_none = true;
        g.no_depth_write = true;
        g.fog = true;
    };
    const uint32_t ib_first = static_cast<uint32_t>(ib_off / sizeof(uint32_t));
    if (laser_geo_.beams) add(0, ib_first, laser_geo_.beam_indices.size(), laser_tex_[0]);
    if (laser_geo_.kites)
        add(kite_off, ib_first + static_cast<uint32_t>(laser_geo_.beam_indices.size()), laser_geo_.kite_indices.size(), laser_tex_[1]);
    const uint32_t drawn = ff_geometry_draws(frame, cmd, view, std::span<const FfGeoDraw>(d, n));
    if (drawn == n) {
        st.laser_beams += laser_geo_.beams;
        st.laser_kites += laser_geo_.kites;
        st.draws += drawn;
        st.ff_draws += drawn;
    } else {
        st.laser_pending += n - drawn;
    }
}

}  // namespace alamo::render
