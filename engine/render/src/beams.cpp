// alamo_render - ability beams (docs/re/ABILITY_VISUALS.md 2, 4, 6). See beams.hpp. CPU side: constants, lightning
// library, per-beam animation and geometry. The renderer side (init_beams / draw_beams) is in beams_draw.cpp.
#include "alamo/render/beams.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "alamo/core/ascii.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::render {

namespace {

// Trimmed text of the first child named `tag` (case-insensitive), "" when absent.
std::string_view child_text_ci(const data::XmlNode& n, std::string_view tag) {
    for (data::XmlNode c = n.first_child(); c; c = c.next_sibling())
        if (ascii_iequals(c.name(), tag)) return c.text_trimmed();
    return {};
}

bool parse_rgba(std::string_view s, uint8_t (&out)[4]) {
    float f[4] = {0, 0, 0, 255};
    const size_t n = data::parse_floats(s, f, 4);
    if (n < 3) return false;
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(std::clamp(std::lround(f[i]), 0L, 255L));
    if (n < 4) out[3] = 255;
    return true;
}

float parse_f(std::string_view s, float fallback) {
    float f = 0.0f;
    if (data::parse_floats(s, &f, 1) == 1 && std::isfinite(f)) return f;
    return fallback;
}

uint32_t d3dcolor(const uint8_t (&c)[4]) {
    return uint32_t{c[2]} | (uint32_t{c[1]} << 8) | (uint32_t{c[0]} << 16) | (uint32_t{c[3]} << 24);
}

PrimVertex pv(const Float3& p, uint32_t color, float u, float v) {
    PrimVertex o;
    o.pos[0] = p.x;
    o.pos[1] = p.y;
    o.pos[2] = p.z;
    o.color = color;
    o.uv[0] = u;
    o.uv[1] = v;
    return o;
}

Float3 add(const Float3& a, const Float3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 sub(const Float3& a, const Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 mul(const Float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross(const Float3& a, const Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
bool normalise(Float3& a) {
    const float l = std::sqrt(dot(a, a));
    if (!(l > 1e-9f)) return false;
    a = mul(a, 1.0f / l);
    return true;
}

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
// Deterministic [-1, 1) from (seed, index, axis).
float hash_signed(uint32_t seed, uint32_t i, uint32_t k) {
    const uint32_t h = mix32(seed * 0x9e3779b1U + i * 0x85ebca6bU + k * 0xc2b2ae35U + 0x165667b1U);
    return static_cast<float>(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

void set_color(uint8_t (&dst)[4], const uint8_t (&src)[4]) { std::memcpy(dst, src, 4); }

LightningStyle parse_lightning(const data::XmlNode& e) {
    LightningStyle s;
    s.name = std::string(e.attribute("name"));
    if (s.name.empty()) s.name = std::string(e.attribute("Name"));
    auto tx = [&](std::string_view tag) { return child_text_ci(e, tag); };
    if (const auto t = tx("Texture_Name"); !t.empty()) s.texture = std::string(t);
    s.width_min = parse_f(tx("Width_Min"), s.width_min);
    s.width_max = parse_f(tx("Width_Max"), s.width_max);
    float d[3] = {s.displace[0], s.displace[1], s.displace[2]};
    if (data::parse_floats(tx("Displace"), d, 3) >= 1) {
        s.displace[0] = d[0];
        s.displace[1] = d[1];
        s.displace[2] = d[2];
    }
    s.detail = parse_f(tx("Detail"), s.detail);
    parse_rgba(tx("Color_Start"), s.color_start);
    parse_rgba(tx("Color_End"), s.color_end);
    s.fade_min = parse_f(tx("Fadeout_Time_Min"), s.fade_min);
    s.fade_max = parse_f(tx("Fadeout_Time_Max"), s.fade_max);
    s.bolts = static_cast<int32_t>(std::lround(parse_f(tx("Number_Bolts"), static_cast<float>(s.bolts))));
    s.interval_min = parse_f(tx("Bolt_Creation_Interval_Min"), s.interval_min);
    s.interval_max = parse_f(tx("Bolt_Creation_Interval_Max"), s.interval_max);
    s.update_all = data::parse_bool_or(tx("Update_All"), false);
    s.cycles_per_distance = parse_f(tx("Cycles_Per_Distance"), 0.0f);
    s.rotations_per_second = parse_f(tx("Rotations_Per_Second"), 0.0f);
    s.radius = parse_f(tx("Radius"), 0.0f);
    return s;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ constants / styles
bool parse_beam_constants(std::span<const uint8_t> xml, BeamConstants& out) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, "GameConstants.xml");
    return parse_beam_constants(doc.root(), out);
}

bool parse_beam_constants(const data::XmlNode& root, BeamConstants& out) {
    if (!root) return false;
    auto read = [&](std::string_view prefix, BeamConstants::Set& s) {
        const std::string p(prefix);
        s.width = parse_f(child_text_ci(root, p + "_Width"), s.width);
        const std::string_view fr = child_text_ci(root, p + "_Frames");
        if (!fr.empty()) s.frames = static_cast<int32_t>(std::lround(parse_f(fr, static_cast<float>(s.frames))));
        if (const auto t = child_text_ci(root, p + "_Texture"); !t.empty()) s.texture = std::string(t);
        uint8_t c[4];
        if (parse_rgba(child_text_ci(root, p + "_Color"), c)) set_color(s.color, c);
    };
    read("Tractor_Beam", out.tractor);
    read("Energy_Beam", out.energy);
    return true;
}

BeamStyle default_beam_style(uint8_t kind, const BeamConstants& gc) {
    BeamStyle s;
    auto from = [&](const BeamConstants::Set& g) {
        s.texture = g.texture;
        s.width = g.width;
        s.frames = g.frames;
        set_color(s.color, g.color);
    };
    switch (kind) {
        case kBeamTractor:
            from(gc.tractor);
            s.mode = 0;
            s.dots = static_cast<uint16_t>(kBeamTractorDots);
            s.dot_dir = -1;  // target -> owner
            s.lightning = "Tractor_Beam";
            s.start_bone = "HP_trac_bone";
            break;
        case kBeamEnergyWeapon:
            from(gc.energy);
            s.mode = 0;
            s.dots = static_cast<uint16_t>(kBeamEnergyDots);
            s.dot_dir = 1;  // owner -> target
            s.lightning = "Piet_Energy_Weapon";
            s.start_bone = "HP_trac_bone";
            break;
        case kBeamSuperLaser:
            s.texture = "tractor_beam01.tga";
            s.width = 35.0f;
            s.color[0] = 0, s.color[1] = 237, s.color[2] = 119, s.color[3] = 255;
            s.frames = 5;
            s.mode = 1;
            s.dots = 0;
            s.start_bone = "MuzzleA_00";
            s.continuation = 1;
            break;
        case kBeamLaserZap:
            s.texture = "tractor_beam00.tga";
            s.width = 4.0f;
            s.color[0] = 34, s.color[1] = 34, s.color[2] = 235, s.color[3] = 255;
            s.frames = 2;
            s.mode = 0;
            s.dots = 0;
            s.lifetime = 2;
            break;
        case kBeamLeech:
            from(gc.energy);
            s.mode = 0;
            s.dots = 0;
            s.lightning = "Leech_Shields";
            break;
        default: from(gc.energy); break;
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ lightning library
LightningLibrary LightningLibrary::builtin() {
    LightningLibrary lib;
    {
        LightningStyle s;  // Default
        s.name = "Default";
        s.width_max = 4.0f;
        s.width_min = 7.0f;
        s.displace[0] = 0.6f, s.displace[1] = 0.6f, s.displace[2] = 0.0f;
        s.detail = 15.0f;
        s.color_start[0] = 100, s.color_start[1] = 100, s.color_start[2] = 255, s.color_start[3] = 255;
        s.color_end[0] = 0, s.color_end[1] = 0, s.color_end[2] = 0, s.color_end[3] = 0;
        s.fade_max = 3.0f;
        s.fade_min = 0.3f;
        s.bolts = 20;
        s.interval_min = 0.001f;
        s.interval_max = 0.2f;
        lib.add(std::move(s));
    }
    {
        LightningStyle s;  // Piet_Energy_Weapon
        s.name = "Piet_Energy_Weapon";
        s.width_max = 3.0f;
        s.width_min = 0.3f;
        s.displace[0] = s.displace[1] = s.displace[2] = 0.05f;
        s.detail = 2.0f;
        s.color_start[0] = 255, s.color_start[1] = 75, s.color_start[2] = 0, s.color_start[3] = 0;
        s.color_end[0] = 255, s.color_end[1] = 255, s.color_end[2] = 0, s.color_end[3] = 0;
        s.fade_max = 0.1f;
        s.fade_min = 0.0f;
        s.bolts = 5;
        s.interval_min = 0.0f;
        s.interval_max = 0.1f;
        s.update_all = true;
        lib.add(std::move(s));
    }
    {
        LightningStyle s;  // Tractor_Beam
        s.name = "Tractor_Beam";
        s.width_max = s.width_min = 1.0f;
        s.displace[0] = s.displace[1] = s.displace[2] = 0.01f;
        s.detail = 0.1f;
        s.color_start[0] = 255, s.color_start[1] = 255, s.color_start[2] = 0, s.color_start[3] = 0;
        s.color_end[0] = 255, s.color_end[1] = 255, s.color_end[2] = 0, s.color_end[3] = 0;
        s.fade_max = 101.0f;
        s.fade_min = 100.0f;
        s.update_all = true;
        s.cycles_per_distance = -0.01f;
        s.rotations_per_second = 3.0f;
        s.radius = 5.0f;
        s.bolts = 3;
        s.interval_min = 0.0f;
        s.interval_max = 0.1f;
        lib.add(std::move(s));
    }
    return lib;
}

void LightningLibrary::add(LightningStyle s) {
    for (LightningStyle& e : styles_)
        if (ascii_iequals(e.name, s.name)) {
            e = std::move(s);
            return;
        }
    styles_.push_back(std::move(s));
}

bool LightningLibrary::load(std::span<const uint8_t> xml) {
    const data::XmlDocument doc =
        data::XmlDocument::parse(xml, "LightningEffectTypes.xml");
    if (!doc.ok()) return false;
    for (data::XmlNode e = doc.root().first_child(); e; e = e.next_sibling()) {
        if (!ascii_iequals(e.name(), "LightningEffect")) continue;
        LightningStyle s = parse_lightning(e);
        if (!s.name.empty()) add(std::move(s));
    }
    return true;
}

const LightningStyle* LightningLibrary::find(std::string_view name) const {
    for (const LightningStyle& s : styles_)
        if (ascii_iequals(s.name, name)) return &s;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ geometry
float van_der_corput(uint32_t i) {
    uint32_t v = i;
    v = (v >> 16) | (v << 16);
    v = ((v & 0x00ff00ffU) << 8) | ((v & 0xff00ff00U) >> 8);
    v = ((v & 0x0f0f0f0fU) << 4) | ((v & 0xf0f0f0f0U) >> 4);
    v = ((v & 0x33333333U) << 2) | ((v & 0xccccccccU) >> 2);
    v = ((v & 0x55555555U) << 1) | ((v & 0xaaaaaaaaU) >> 1);
    return static_cast<float>(static_cast<double>(v) / 4294967296.0);
}

bool beam_frame(const Float3& a, const Float3& b, Float3& dir, float& length, Float3& side) {
    dir = sub(b, a);
    length = std::sqrt(dot(dir, dir));
    if (!(length > 1e-6f) || !std::isfinite(length)) return false;
    dir = mul(dir, 1.0f / length);
    side = {dir.y, -dir.x, 0.0f};  // dir x (0, 0, 1)
    if (!normalise(side)) side = {1.0f, 0.0f, 0.0f};
    return true;
}

bool beam_ribbon(const BeamInstance& b, float u_offset, std::vector<PrimVertex>& v, std::vector<uint32_t>& idx) {
    Float3 dir, side;
    float len = 0.0f;
    if (!beam_frame(b.start, b.end, dir, len, side)) return false;
    const float w = b.style.width > 0.0f ? b.style.width : kBeamDefaultWidth;
    const float h = 0.5f * w;
    const uint32_t col = d3dcolor(b.style.color);
    auto quad = [&](float s0, float s1, float u0, float u1, bool mode1) {
        const auto base = static_cast<uint32_t>(v.size());
        const Float3 p0 = add(b.start, mul(dir, s0)), p1 = add(b.start, mul(dir, s1));
        if (!mode1) {  // U 1 -> 0 across, V 1 (start) -> 0 (end) along
            v.push_back(pv(add(p0, mul(side, h)), col, 1.0f, 1.0f));
            v.push_back(pv(sub(p0, mul(side, h)), col, 0.0f, 1.0f));
            v.push_back(pv(add(p1, mul(side, h)), col, 1.0f, 0.0f));
            v.push_back(pv(sub(p1, mul(side, h)), col, 0.0f, 0.0f));
        } else {  // U along (0 -> 200 per full segment), V 1 -> 0 across
            v.push_back(pv(add(p0, mul(side, h)), col, u0, 1.0f));
            v.push_back(pv(sub(p0, mul(side, h)), col, u0, 0.0f));
            v.push_back(pv(add(p1, mul(side, h)), col, u1, 1.0f));
            v.push_back(pv(sub(p1, mul(side, h)), col, u1, 0.0f));
        }
        for (uint32_t i : {0u, 1u, 2u, 1u, 3u, 2u}) idx.push_back(base + i);
    };
    if (b.style.mode == 0) {
        quad(0.0f, len, 0.0f, 0.0f, false);
        return true;
    }
    const float seg = 200.0f * w;
    uint32_t guard = 0;
    for (float s0 = 0.0f; s0 < len && guard < 4096; s0 += seg, ++guard) {
        const float s1 = std::min(len, s0 + seg);
        quad(s0, s1, -u_offset, (s1 - s0) / w - u_offset, true);
    }
    return true;
}

bool beam_dot(const BeamInstance& b, float fraction, std::vector<PrimVertex>& v, std::vector<uint32_t>& idx) {
    Float3 dir, side;
    float len = 0.0f;
    if (!beam_frame(b.start, b.end, dir, len, side)) return false;
    const Float3 c = add(b.start, mul(dir, len * fraction));
    const float h = 0.5f * kBeamDotSize;
    uint8_t rgba[4];
    for (int i = 0; i < 4; ++i)
        rgba[i] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(b.style.color[i]) * kBeamDotColorScale), 0L, 255L));
    const uint32_t col = d3dcolor(rgba);
    const auto base = static_cast<uint32_t>(v.size());
    const Float3 a = mul(dir, h), s = mul(side, h);
    v.push_back(pv(add(sub(c, a), s), col, 1.0f, 1.0f));
    v.push_back(pv(sub(sub(c, a), s), col, 0.0f, 1.0f));
    v.push_back(pv(add(add(c, a), s), col, 1.0f, 0.0f));
    v.push_back(pv(sub(add(c, a), s), col, 0.0f, 0.0f));
    for (uint32_t i : {0u, 1u, 2u, 1u, 3u, 2u}) idx.push_back(base + i);
    return true;
}

std::vector<Float3> lightning_points(const LightningStyle& st, const Float3& a, const Float3& b, uint32_t seed, float time, float phase) {
    std::vector<Float3> pts;
    Float3 dir, u1;
    float len = 0.0f;
    if (!beam_frame(a, b, dir, len, u1)) return pts;
    Float3 u2 = cross(dir, u1);
    normalise(u2);
    if (st.helix()) {
        const auto n = static_cast<uint32_t>(std::clamp(std::ceil(len / 20.0f), 4.0f, 128.0f));
        const float turn = 6.28318531f;
        for (uint32_t i = 0; i <= n; ++i) {
            const float s = len * static_cast<float>(i) / static_cast<float>(n);
            const float th = turn * (st.rotations_per_second * time + st.cycles_per_distance * s + phase);
            Float3 p = add(add(a, mul(dir, s)), add(mul(u1, st.radius * std::cos(th)), mul(u2, st.radius * std::sin(th))));
            if (i != 0 && i != n) {  // [M] small shimmer: Displace x radius x 10
                p = add(p, mul(u1, hash_signed(seed, i, 0) * st.displace[0] * st.radius * 10.0f));
                p = add(p, mul(u2, hash_signed(seed, i, 1) * st.displace[1] * st.radius * 10.0f));
            }
            pts.push_back(p);
        }
        return pts;
    }
    // Midpoint displacement: Detail + 3 levels [M], amplitude Displace x 0.5 x length, halved per level.
    const int levels = std::clamp(static_cast<int>(st.detail) + 3, 2, 7);
    pts = {a, b};
    float amp = 0.5f * len;
    for (int l = 0; l < levels; ++l) {
        std::vector<Float3> next;
        next.reserve(pts.size() * 2);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            next.push_back(pts[i]);
            Float3 m = mul(add(pts[i], pts[i + 1]), 0.5f);
            const auto k = static_cast<uint32_t>(next.size());
            m = add(m, mul(u1, hash_signed(seed, k, 0) * st.displace[0] * amp));
            m = add(m, mul(u2, hash_signed(seed, k, 1) * st.displace[1] * amp));
            m = add(m, mul(dir, hash_signed(seed, k, 2) * st.displace[2] * amp));
            next.push_back(m);
        }
        next.push_back(pts.back());
        pts = std::move(next);
        amp *= 0.5f;
    }
    return pts;
}

void lightning_strip(const std::vector<Float3>& pts, const Float3& eye, float w, const uint8_t (&rgba)[4], std::vector<PrimVertex>& v,
                     std::vector<uint32_t>& idx) {
    if (pts.size() < 2) return;
    const uint32_t col = d3dcolor(rgba);
    const float h = 0.5f * w;
    const auto base = static_cast<uint32_t>(v.size());
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
        Float3 t = sub(pts[std::min(i + 1, n - 1)], pts[i > 0 ? i - 1 : 0]);
        Float3 perp = cross(t, sub(eye, pts[i]));
        if (!normalise(perp)) perp = {0.0f, 0.0f, 1.0f};
        const float vv = static_cast<float>(i) / static_cast<float>(n - 1);
        v.push_back(pv(add(pts[i], mul(perp, h)), col, 1.0f, vv));
        v.push_back(pv(sub(pts[i], mul(perp, h)), col, 0.0f, vv));
    }
    for (uint32_t i = 0; i + 1 < n; ++i) {
        const uint32_t k = base + 2 * i;
        for (uint32_t j : {0u, 1u, 2u, 1u, 3u, 2u}) idx.push_back(k + j);
    }
}

void BeamGeometry::build(std::span<const BeamDraw> draws, const ViewMatrices& view) {
    vertices.clear();
    indices.clear();
    batches.clear();
    ribbons = dots = bolts = skipped = 0;
    struct Group {
        std::string texture;
        std::vector<uint32_t> idx;
    };
    std::vector<Group> groups;
    auto group = [&](std::string_view tex) -> std::vector<uint32_t>& {
        for (Group& g : groups)
            if (ascii_iequals(g.texture, tex)) return g.idx;
        groups.push_back({std::string(tex), {}});
        return groups.back().idx;
    };
    for (const BeamDraw& d : draws) {
        const BeamInstance& b = d.beam;
        if (!beam_ribbon(b, d.u_offset, vertices, group(b.style.texture))) {
            ++skipped;
            continue;
        }
        ++ribbons;
        for (float f : d.dots)
            if (beam_dot(b, f, vertices, group(kBeamDotTexture))) ++dots;
        if (!d.lightning) continue;
        for (const BoltDraw& bolt : d.bolts) {
            const std::vector<Float3> pts = lightning_points(*d.lightning, b.start, b.end, bolt.seed, d.time, bolt.phase);
            if (pts.size() < 2) continue;
            const float k = bolt.life > 0.0f ? std::clamp(bolt.age / bolt.life, 0.0f, 1.0f) : 1.0f;
            uint8_t c[4];
            for (int i = 0; i < 4; ++i)
                c[i] = static_cast<uint8_t>(std::clamp(
                    std::lround(static_cast<float>(d.lightning->color_start[i]) +
                                (static_cast<float>(d.lightning->color_end[i]) - static_cast<float>(d.lightning->color_start[i])) * k),
                    0L, 255L));
            lightning_strip(pts, view.eye, bolt.width, c, vertices, group(d.lightning->texture));
            ++bolts;
        }
    }
    for (Group& g : groups) {
        if (g.idx.empty()) continue;
        Batch bt;
        bt.texture = std::move(g.texture);
        bt.first_index = static_cast<uint32_t>(indices.size());
        bt.index_count = static_cast<uint32_t>(g.idx.size());
        indices.insert(indices.end(), g.idx.begin(), g.idx.end());
        batches.push_back(std::move(bt));
    }
}

// ------------------------------------------------------------------------------------------------ animation
void BeamAnimator::clear() {
    states_.clear();
    draws_.clear();
    started_ = false;
    last_tick_ = 0;
}

void BeamAnimator::init_state(State& s, const BeamInstance& b) {
    s.draw = BeamDraw{};
    s.draw.beam = b;
    s.frame = 0;
    s.draw.dots.clear();
    for (uint16_t i = 0; i < b.style.dots; ++i)
        s.draw.dots.push_back(b.kind == kBeamTractor && i < 4 ? kBeamTractorDotStart[i] : van_der_corput(static_cast<uint32_t>(i) + 1u));
    s.draw.lightning = nullptr;
    s.draw.bolts.clear();
    s.next_create = 0.0f;
    if (lib_ && !b.style.lightning.empty()) s.draw.lightning = lib_->find(b.style.lightning);
    s.had_beam = false;
}

void BeamAnimator::advance(State& s) {
    constexpr float dt = 1.0f / 30.0f;
    BeamDraw& d = s.draw;
    const BeamStyle& st = d.beam.style;
    d.time += dt;
    if (st.mode == 1 && st.frames > 0) {
        s.frame = (s.frame + 1) % st.frames;
        d.u_offset = static_cast<float>(s.frame) / static_cast<float>(st.frames);
    }
    const float step = static_cast<float>(st.dot_dir) / static_cast<float>(kBeamDotPeriod);
    for (float& f : d.dots) {
        f += step;
        f -= std::floor(f);
    }
    advance_lightning(s);
}

void BeamAnimator::advance_lightning(State& s) {
    constexpr float dt = 1.0f / 30.0f;
    BeamDraw& d = s.draw;
    if (const LightningStyle* ls = d.lightning) {
        for (BoltDraw& bolt : d.bolts) {
            bolt.age += dt;
            if (ls->update_all) bolt.seed = rng_.next();
        }
        std::erase_if(d.bolts, [](const BoltDraw& bolt) { return bolt.age >= bolt.life; });
        s.next_create -= dt;
        while (s.next_create <= 0.0f && static_cast<int32_t>(d.bolts.size()) < ls->bolts) {
            BoltDraw bolt;
            bolt.life = std::max(dt, rng_.range(std::min(ls->fade_min, ls->fade_max), std::max(ls->fade_min, ls->fade_max)));
            bolt.width = rng_.range(std::min(ls->width_min, ls->width_max), std::max(ls->width_min, ls->width_max));
            bolt.seed = rng_.next();
            bolt.phase = ls->bolts > 0 ? static_cast<float>(d.bolts.size()) / static_cast<float>(ls->bolts) : 0.0f;
            d.bolts.push_back(bolt);
            s.next_create += rng_.range(std::min(ls->interval_min, ls->interval_max), std::max(ls->interval_min, ls->interval_max));
        }
        if (s.next_create < 0.0f) s.next_create = 0.0f;
    }
}

void BeamAnimator::step(uint64_t tick, std::span<const BeamInstance> beams) {
    uint64_t adv = 0;
    if (!started_) {
        started_ = true;
        last_tick_ = tick;
    } else if (tick > last_tick_) {
        adv = std::min<uint64_t>(tick - last_tick_, kMaxCatchUp);
        last_tick_ = tick;
    } else if (tick < last_tick_) {  // the sim restarted (new battle): start over
        states_.clear();
        last_tick_ = tick;
    }
    for (uint64_t i = 0; i < adv; ++i)
        for (auto& [key, s] : states_) {
            advance(s);
            if (s.oneshot) --s.remaining;
        }
    std::erase_if(states_, [](const auto& kv) { return kv.second.oneshot && kv.second.remaining <= 0; });
    for (auto& [key, s] : states_) s.seen = false;
    for (const BeamInstance& b : beams) {
        auto it = states_.find(b.key);
        if (!b.continuous) {  // one-shot: only the first sighting counts
            if (it != states_.end()) continue;
            State& s = states_[b.key];
            init_state(s, b);
            s.oneshot = true;
            s.remaining = std::max(1, b.style.lifetime);
            s.seen = true;
            advance_lightning(s);
            continue;
        }
        if (it == states_.end()) {
            State& s = states_[b.key];
            init_state(s, b);
            s.last = b;
            s.had_beam = true;
            s.seen = true;
            advance_lightning(s);
            continue;
        }
        State& s = it->second;
        s.oneshot = false;  // a super laser that came back during its continuation
        s.draw.beam = b;
        s.last = b;
        s.had_beam = true;
        s.seen = true;
    }
    for (auto it = states_.begin(); it != states_.end();) {
        State& s = it->second;
        if (s.seen || s.oneshot) {
            ++it;
            continue;
        }
        if (s.had_beam && s.last.kind == kBeamSuperLaser && s.last.style.continuation > 0) {
            s.oneshot = true;
            s.remaining = s.last.style.continuation;
            s.draw.beam.start = s.last.start;
            s.draw.beam.end = add(s.last.start, mul(sub(s.last.end, s.last.start), 2.0f));  // 4.3: start + 2 x (target - start)
            s.draw.beam.continuous = false;
            ++it;
        } else {
            it = states_.erase(it);
        }
    }
    draws_.clear();
    for (const auto& [key, s] : states_) draws_.push_back(s.draw);
}

}  // namespace alamo::render
