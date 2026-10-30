#include "alamo/render/galactic/galactic_view.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/assets/model.hpp"
#include "alamo/assets/vertex_layout.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/render/sim_snapshot.hpp"

namespace alamo::render {

namespace {

constexpr float kPi = 3.14159265358979323846f;
static_assert(sizeof(assets::StaticVertex) == 64, "StaticVertex is the 64-byte alD3dVertNU2C record");

std::string upper(std::string_view s) {
    std::string u(s);
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

std::string trim(std::string s) {
    const auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0, b = s.size();
    while (a < b && ws(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && ws(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Up to `n` floats separated by commas / whitespace; returns how many parsed.
size_t parse_float_list(std::string_view text, float* out, size_t n) {
    const std::string t(text);
    const char* p = t.c_str();
    size_t k = 0;
    while (*p && k < n) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) break;
        out[k++] = v;
        p = end;
    }
    return k;
}

float len3(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 norm(const Vec3& v) {
    const float l = len3(v);
    return (l > 1e-12f && std::isfinite(l)) ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0, 0, 1};
}

uint8_t channel(uint32_t rgba, int k) { return static_cast<uint8_t>((rgba >> (8 * k)) & 0xFFu); }
uint8_t shade(uint8_t c, float s) { return static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(c) * s), 0L, 255L)); }

// Effect-less static mesh (unlit stand-in: vertex colour), one submesh on a ROOT bone.
struct MeshBuilder {
    std::vector<assets::StaticVertex> v;
    std::vector<uint16_t> idx;
    uint16_t vertex(const Vec3& p, const Vec3& n, uint32_t rgba, float s = 1.0f, float u = 0.0f, float tv = 0.0f) {
        assets::StaticVertex st{};
        st.uv0[0] = u;
        st.uv0[1] = tv;
        const float pp[3] = {p.x, p.y, p.z}, nn[3] = {n.x, n.y, n.z};
        // Any tangent frame orthogonal to n (no normal maps on these meshes).
        const Vec3 t = norm(std::fabs(n.z) < 0.9f ? cross({0, 0, 1}, n) : cross({1, 0, 0}, n));
        const Vec3 b = cross(n, t);
        const float tt[3] = {t.x, t.y, t.z}, bb[3] = {b.x, b.y, b.z};
        for (int a = 0; a < 3; ++a) {
            st.position[a] = pp[a];
            st.normal[a] = nn[a];
            st.tangent[a] = tt[a];
            st.binormal[a] = bb[a];
        }
        for (int k = 0; k < 3; ++k) st.color[k] = shade(channel(rgba, k), s);
        st.color[3] = channel(rgba, 3);
        v.push_back(st);
        return static_cast<uint16_t>(v.size() - 1);
    }
    void tri(uint16_t a, uint16_t b, uint16_t c) {
        idx.push_back(a);
        idx.push_back(b);
        idx.push_back(c);
    }
    // Flat face (a, b, c) wound counter-clockwise seen from outside (`inside` is any interior point).
    void face_outward(Vec3 a, Vec3 b, Vec3 c, const Vec3& inside, uint32_t rgba) {
        Vec3 n = norm(cross(sub(b, a), sub(c, a)));
        const Vec3 centre = mul(add(add(a, b), c), 1.0f / 3.0f);
        if (dot(n, sub(centre, inside)) < 0.0f) {
            std::swap(b, c);
            n = mul(n, -1.0f);
        }
        const float s = face_shade(n);
        const uint16_t i0 = vertex(a, n, rgba, s), i1 = vertex(b, n, rgba, s), i2 = vertex(c, n, rgba, s);
        tri(i0, i1, i2);
    }
    // Fixed fake light so the shapes read without lighting.
    static float face_shade(const Vec3& n) {
        const Vec3 l = norm({-0.35f, -0.45f, 0.82f});
        return 0.35f + 0.65f * std::max(0.0f, dot(n, l));
    }
};

std::shared_ptr<assets::ModelAsset> make_model(const std::string& name, const MeshBuilder& mb) {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = name;
    assets::Skeleton& sk = m->skeleton;
    sk.names = {"ROOT"};
    sk.parents = {-1};
    sk.visible = {1};
    sk.billboard_mode = {0};
    sk.bind_local = {assets::Mat3x4{}};
    sk.finalize();
    assets::MeshInfo mi;
    mi.name = "GALACTIC_PRIMITIVE";
    mi.attach_bone = 0;
    mi.first_submesh = 0;
    mi.submesh_count = 1;
    m->meshes.push_back(mi);
    assets::Submesh sm;
    sm.mesh = 0;
    sm.vertex_format = "alD3dVertNU2C";
    sm.layout = assets::VertexLayout::Static;
    sm.attach_bone = 0;
    sm.vertex_count = static_cast<uint32_t>(mb.v.size());
    sm.vertices.resize(mb.v.size() * 64);
    if (!mb.v.empty()) std::memcpy(sm.vertices.data(), mb.v.data(), mb.v.size() * 64);
    sm.index_type = assets::IndexType::U16;
    sm.index_count = static_cast<uint32_t>(mb.idx.size());
    sm.indices.resize(mb.idx.size() * 2);
    if (!mb.idx.empty()) std::memcpy(sm.indices.data(), mb.idx.data(), mb.idx.size() * 2);
    assets::Float3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const assets::StaticVertex& s : mb.v) {
        lo = {std::min(lo.x, s.position[0]), std::min(lo.y, s.position[1]), std::min(lo.z, s.position[2])};
        hi = {std::max(hi.x, s.position[0]), std::max(hi.y, s.position[1]), std::max(hi.z, s.position[2])};
    }
    if (mb.v.empty()) lo = hi = {0, 0, 0};
    // Flat shapes get a little thickness so bounds are never degenerate.
    if (hi.z - lo.z < 0.02f) {
        lo.z -= 0.01f;
        hi.z += 0.01f;
    }
    sm.bounds = {lo, hi};
    m->submeshes.push_back(std::move(sm));
    m->bounds = {lo, hi};
    const assets::Float3 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    const float dx = hi.x - c.x, dy = hi.y - c.y, dz = hi.z - c.z;
    m->sphere = {c, std::sqrt(dx * dx + dy * dy + dz * dz)};
    return m;
}

// Flat two-sided polygon fan helpers.
void flat_quad(MeshBuilder& mb, const Vec3 (&q)[4], uint32_t top, uint32_t bottom) {
    const uint16_t a = mb.vertex(q[0], {0, 0, 1}, top), b = mb.vertex(q[1], {0, 0, 1}, top), c = mb.vertex(q[2], {0, 0, 1}, top),
                   d = mb.vertex(q[3], {0, 0, 1}, top);
    mb.tri(a, b, c);
    mb.tri(a, c, d);
    const uint16_t e = mb.vertex(q[0], {0, 0, -1}, bottom), f = mb.vertex(q[1], {0, 0, -1}, bottom),
                   g = mb.vertex(q[2], {0, 0, -1}, bottom), h = mb.vertex(q[3], {0, 0, -1}, bottom);
    mb.tri(e, g, f);
    mb.tri(e, h, g);
}

const char* kind_name(GalacticPrimitive k) {
    switch (k) {
    case GalacticPrimitive::Ribbon: return "RIBBON";
    case GalacticPrimitive::Ring: return "RING";
    case GalacticPrimitive::Sphere: return "SPHERE";
    case GalacticPrimitive::Fleet: return "FLEET";
    case GalacticPrimitive::Station: return "STATION";
    case GalacticPrimitive::Disc: return "DISC";
    case GalacticPrimitive::Globe: return "GLOBE";
    case GalacticPrimitive::Line: return "LINE";
    default: return "NONE";
    }
}

// A model by name; procedural stand-ins (synthetic world) are drawn in the owner colour instead (the unlit stand-in
// has no Colorization parameter).
const GpuModel* owner_model(GalacticModelSource& models, const std::string& name, const materials::float4& color);

}  // namespace

uint32_t rgba8_of(const materials::float4& c) {
    auto b = [](float f) {
        const float v = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f;
        return static_cast<uint32_t>(std::lround(v * 255.0f));
    };
    return rgba8(b(c.x), b(c.y), b(c.z), b(c.w));
}

std::string primitive_model_name(GalacticPrimitive kind, uint32_t rgba) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "%02X%02X%02X%02X", channel(rgba, 0), channel(rgba, 1), channel(rgba, 2), channel(rgba, 3));
    return std::string("ALAMO_GAL_") + kind_name(kind) + "_" + hex + ".ALO";
}

bool parse_primitive_model_name(std::string_view name, GalacticPrimitive& kind, uint32_t& rgba) {
    const std::string u = upper(name);
    const std::string_view prefix = "ALAMO_GAL_";
    if (u.size() < prefix.size() || u.compare(0, prefix.size(), prefix) != 0) return false;
    for (uint8_t k = 0; k < static_cast<uint8_t>(GalacticPrimitive::Count); ++k) {
        const auto kk = static_cast<GalacticPrimitive>(k);
        const std::string head = std::string(prefix) + kind_name(kk) + "_";
        if (u.size() != head.size() + 8 + 4 || u.compare(0, head.size(), head) != 0 || u.compare(head.size() + 8, 4, ".ALO") != 0)
            continue;
        uint32_t c[4] = {};
        for (int i = 0; i < 4; ++i) {
            const std::string byte = u.substr(head.size() + static_cast<size_t>(i) * 2, 2);
            char* end = nullptr;
            const unsigned long v = std::strtoul(byte.c_str(), &end, 16);
            if (end != byte.c_str() + 2) return false;
            c[i] = static_cast<uint32_t>(v);
        }
        kind = kk;
        rgba = rgba8(c[0], c[1], c[2], c[3]);
        return true;
    }
    return false;
}

std::shared_ptr<assets::ModelAsset> galactic_primitive_model(GalacticPrimitive kind, uint32_t rgba) {
    MeshBuilder mb;
    const uint32_t dim = rgba8(shade(channel(rgba, 0), 0.55f), shade(channel(rgba, 1), 0.55f), shade(channel(rgba, 2), 0.55f),
                               channel(rgba, 3));
    switch (kind) {
    case GalacticPrimitive::Ribbon: {
        const Vec3 q[4] = {{0, -0.5f, 0}, {1, -0.5f, 0}, {1, 0.5f, 0}, {0, 0.5f, 0}};
        flat_quad(mb, q, rgba, dim);
        break;
    }
    case GalacticPrimitive::Line: {
        // Ribbon geometry, u across the line, v along it (w_galaxy_line*.tga vary across u, not v); both faces in the full
        // colour (additive).
        const Vec3 q[4] = {{0, -0.5f, 0}, {1, -0.5f, 0}, {1, 0.5f, 0}, {0, 0.5f, 0}};
        const float uv[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
        uint16_t t[4], b[4];
        for (int i = 0; i < 4; ++i) {
            t[i] = mb.vertex(q[i], {0, 0, 1}, rgba, 1.0f, uv[i][0], uv[i][1]);
            b[i] = mb.vertex(q[i], {0, 0, -1}, rgba, 1.0f, uv[i][0], uv[i][1]);
        }
        mb.tri(t[0], t[1], t[2]);
        mb.tri(t[0], t[2], t[3]);
        mb.tri(b[0], b[2], b[1]);
        mb.tri(b[0], b[3], b[2]);
        break;
    }
    case GalacticPrimitive::Ring: {
        const int n = 64;
        const float r0 = 0.95f, r1 = 1.0f;  // [port] thin band (the original ring is a thin faint ellipse)
        for (int i = 0; i < n; ++i) {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / n, a1 = 2.0f * kPi * static_cast<float>(i + 1) / n;
            const Vec3 q[4] = {{r0 * std::cos(a0), r0 * std::sin(a0), 0}, {r1 * std::cos(a0), r1 * std::sin(a0), 0},
                               {r1 * std::cos(a1), r1 * std::sin(a1), 0}, {r0 * std::cos(a1), r0 * std::sin(a1), 0}};
            flat_quad(mb, q, rgba, dim);
        }
        break;
    }
    case GalacticPrimitive::Sphere: {
        const int stacks = 16, slices = 32;
        const float r = kPlanetSphereRadius;
        std::vector<uint16_t> ring_start;
        for (int s = 0; s <= stacks; ++s) {
            const float phi = kPi * static_cast<float>(s) / stacks;  // 0 = +Z pole
            ring_start.push_back(static_cast<uint16_t>(mb.v.size()));
            for (int k = 0; k <= slices; ++k) {
                const float th = 2.0f * kPi * static_cast<float>(k) / slices;
                const Vec3 n{std::sin(phi) * std::cos(th), std::sin(phi) * std::sin(th), std::cos(phi)};
                // Latitude bands so the spin reads.
                const float band = ((s / 2 + k / 4) % 2 == 0) ? 1.0f : 0.85f;
                mb.vertex(mul(n, r), n, rgba, MeshBuilder::face_shade(n) * band);
            }
        }
        for (int s = 0; s < stacks; ++s)
            for (int k = 0; k < slices; ++k) {
                const auto a = static_cast<uint16_t>(ring_start[static_cast<size_t>(s)] + k);
                const auto b = static_cast<uint16_t>(ring_start[static_cast<size_t>(s) + 1] + k);
                // Outward CCW: (a, b, b+1), (a, b+1, a+1) with phi growing downwards.
                mb.tri(a, b, static_cast<uint16_t>(b + 1));
                mb.tri(a, static_cast<uint16_t>(b + 1), static_cast<uint16_t>(a + 1));
            }
        break;
    }
    case GalacticPrimitive::Fleet: {
        // Tip at +Y, like the Fleet_ALT0 darts of w_planet_fleet.alo.
        const Vec3 N{0, 2.0f, 0}, L{1.1f, -1.0f, 0}, R{-1.1f, -1.0f, 0}, T{0, -0.5f, 0.7f}, B{0, -0.5f, -0.45f};
        const Vec3 in{0, -0.2f, 0.05f};
        for (const auto& f : {std::array<Vec3, 3>{N, L, T}, std::array<Vec3, 3>{N, T, R}, std::array<Vec3, 3>{N, R, B},
                              std::array<Vec3, 3>{N, B, L}, std::array<Vec3, 3>{L, T, R}, std::array<Vec3, 3>{L, R, B}})
            mb.face_outward(f[0], f[1], f[2], in, rgba);
        break;
    }
    case GalacticPrimitive::Station: {
        const float e = 1.2f, h = 0.5f;
        const Vec3 c[8] = {{-e, -e, -h}, {e, -e, -h}, {e, e, -h}, {-e, e, -h}, {-e, -e, h}, {e, -e, h}, {e, e, h}, {-e, e, h}};
        const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        for (const auto& f : faces) {
            mb.face_outward(c[f[0]], c[f[1]], c[f[2]], {0, 0, 0}, rgba);
            mb.face_outward(c[f[0]], c[f[2]], c[f[3]], {0, 0, 0}, rgba);
        }
        break;
    }
    case GalacticPrimitive::Disc: {
        const int n = 96;
        const uint16_t ct = mb.vertex({0, 0, 0}, {0, 0, 1}, rgba, 0.45f), cb = mb.vertex({0, 0, 0}, {0, 0, -1}, rgba, 0.45f);
        std::vector<uint16_t> top, bot;
        for (int i = 0; i <= n; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / n;
            // Two faint spiral arms in the rim brightness.
            const float arm = 0.02f + 0.03f * std::cos(2.0f * a);
            const Vec3 p{std::cos(a), std::sin(a), 0};
            top.push_back(mb.vertex(p, {0, 0, 1}, rgba, arm));
            bot.push_back(mb.vertex(p, {0, 0, -1}, rgba, arm));
        }
        for (int i = 0; i < n; ++i) {
            mb.tri(ct, top[static_cast<size_t>(i)], top[static_cast<size_t>(i) + 1]);
            mb.tri(cb, bot[static_cast<size_t>(i) + 1], bot[static_cast<size_t>(i)]);
        }
        break;
    }
    case GalacticPrimitive::Globe: {
        // Thin two-sided bands on the sphere: parallels offset along the latitude tangent, meridians along the east one.
        const float r = kPlanetSphereRadius, hw = 0.035f * kPlanetSphereRadius;
        const int segs = 48;
        auto pt = [&](float phi, float th) {
            return Vec3{r * std::sin(phi) * std::cos(th), r * std::sin(phi) * std::sin(th), r * std::cos(phi)};
        };
        auto band = [&](const Vec3& a, const Vec3& b, const Vec3& oa, const Vec3& ob) {
            const Vec3 q[4] = {sub(a, oa), sub(b, ob), add(b, ob), add(a, oa)};
            const Vec3 n = mul(add(a, b), 1.0f / (2.0f * r));
            const Vec3 nb = mul(n, -1.0f);
            uint16_t v[4], w[4];
            for (int i = 0; i < 4; ++i) {
                v[i] = mb.vertex(q[i], n, rgba);
                w[i] = mb.vertex(q[i], nb, rgba);
            }
            mb.tri(v[0], v[1], v[2]);
            mb.tri(v[0], v[2], v[3]);
            mb.tri(w[0], w[2], w[1]);
            mb.tri(w[0], w[3], w[2]);
        };
        for (int k = 1; k < 8; ++k) {  // parallels
            const float phi = kPi * static_cast<float>(k) / 8.0f;
            auto north = [&](float th) {
                return mul(Vec3{std::cos(phi) * std::cos(th), std::cos(phi) * std::sin(th), -std::sin(phi)}, hw);
            };
            for (int i = 0; i < segs; ++i) {
                const float t0 = 2.0f * kPi * static_cast<float>(i) / segs, t1 = 2.0f * kPi * static_cast<float>(i + 1) / segs;
                band(pt(phi, t0), pt(phi, t1), north(t0), north(t1));
            }
        }
        for (int k = 0; k < 12; ++k) {  // meridians
            const float th = 2.0f * kPi * static_cast<float>(k) / 12.0f;
            const Vec3 east = mul(Vec3{-std::sin(th), std::cos(th), 0}, hw);
            for (int i = 0; i < segs / 2; ++i) {
                const float p0 = kPi * static_cast<float>(i) / (segs / 2), p1 = kPi * static_cast<float>(i + 1) / (segs / 2);
                band(pt(p0, th), pt(p1, th), east, east);
            }
        }
        break;
    }
    default: break;
    }
    return make_model(primitive_model_name(kind, rgba), mb);
}

std::shared_ptr<assets::ModelAsset> galactic_line_model(const std::string& texture) {
    const uint32_t rgba = 0xFFFFFFFFu;
    std::shared_ptr<assets::ModelAsset> m = galactic_primitive_model(GalacticPrimitive::Line, rgba);
    assets::Submesh& sm = m->submeshes.front();
    sm.vertex_format = sm.effective_vertex_format = "alD3dVertNU2";  // MeshAdditive's _ALAMO_VERTEX_TYPE
    sm.format.known = true;
    sm.format.normal = true;
    sm.format.uv_sets = 1;
    sm.material.effect = "MeshAdditive.fx";
    assets::MaterialParam tex;
    tex.name = "BaseTexture";
    tex.type = assets::ParamType::Texture;
    tex.texture = texture;
    assets::MaterialParam col;
    col.name = "Color";
    col.type = assets::ParamType::Float3;
    col.value = {static_cast<float>(channel(rgba, 0)) / 255.0f, static_cast<float>(channel(rgba, 1)) / 255.0f,
                 static_cast<float>(channel(rgba, 2)) / 255.0f, 0.0f};
    sm.material.params = {tex, col};
    return m;
}

// ------------------------------------------------------------------------------------------ models
const GpuModel* GalacticModels::model(const std::string& name) {
    if (name.empty()) return nullptr;
    GalacticPrimitive kind{};
    uint32_t rgba = 0;
    if (parse_primitive_model_name(name, kind, rgba)) return primitive(kind, rgba);
    if (!streamer_) return nullptr;
    auto [it, inserted] = by_name_.try_emplace(upper(name), 0u);
    if (inserted) {
        it->second = streamer_->request(name);
        handles_.push_back(it->second);
    }
    return streamer_->state(it->second) == ModelStreamer::State::Ready ? streamer_->model(it->second) : nullptr;
}

bool GalacticModels::model_failed(const std::string& name) {
    if (!streamer_) return false;
    const auto it = by_name_.find(upper(name));
    return it != by_name_.end() && streamer_->state(it->second) == ModelStreamer::State::Failed;
}

const GpuModel* GalacticModels::primitive(GalacticPrimitive kind, uint32_t rgba) {
    const uint64_t key = (uint64_t{static_cast<uint8_t>(kind)} << 32) | rgba;
    auto [it, inserted] = prims_.try_emplace(key);
    if (inserted) {
        it->second = std::make_unique<Prim>();
        std::string err;
        it->second->ok = renderer_->upload_model(
            galactic_primitive_model(kind, rgba),
            [](std::string_view, assets::TextureRole) { return std::shared_ptr<const assets::TextureAsset>(); }, it->second->model,
            nullptr, &err);
    }
    Prim& p = *it->second;
    if (!p.ok) return nullptr;
    renderer_->make_resident(p.model);
    return p.model.resident() ? &p.model : nullptr;
}

const GpuModel* GalacticModels::resident(Prim& p) {
    if (!p.ok) return nullptr;
    renderer_->make_resident(p.model);
    return p.model.resident() ? &p.model : nullptr;
}

const GpuModel* GalacticModels::line(const std::string& texture, uint32_t rgba) {
    (void)rgba;  // tinted per instance (Instance::light_scale)
    auto [it, inserted] = lines_.try_emplace(upper(texture));
    if (inserted) {
        it->second = std::make_unique<Prim>();
        std::string err;
        assets::AssetCache* cache = cache_;
        it->second->ok = renderer_->upload_model(
            galactic_line_model(texture),
            [cache](std::string_view n, assets::TextureRole role) {
                return cache ? cache->texture(n, role) : std::shared_ptr<const assets::TextureAsset>();
            },
            it->second->model, nullptr, &err);
    }
    return resident(*it->second);
}

std::shared_ptr<const assets::AnimationClip> GalacticModels::idle_clip(const std::string& model) {
    if (!cache_ || !files_ || model.empty()) return nullptr;
    const std::string key = upper(model);
    auto [it, inserted] = clips_.try_emplace(key);
    if (!inserted) return it->second;
    // "<model stem>_IDLE_00" (w_planet_urban01.alo -> W_PLANET_URBAN01_IDLE_00.ALA), existence checked first so
    // a model without a clip does not count as a failed load (MapScene's rule).
    std::string_view stem = model;
    const size_t b = stem.find_last_of("\\/");
    if (b != std::string_view::npos) stem = stem.substr(b + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string_view::npos) stem = stem.substr(0, dot);
    if (stem.empty()) return nullptr;
    const std::string clip = std::string(stem) + "_IDLE_00";
    if (!files_->exists(assets::animation_path(clip))) return nullptr;
    std::shared_ptr<const assets::AnimationClip> c = cache_->animation(clip);
    if (c && c->frame_count == 0) c.reset();
    it->second = c;
    return c;
}

void GalacticModels::update() {
    for (auto& [key, p] : prims_)
        if (p->ok) renderer_->make_resident(p->model);
    for (auto& [key, p] : lines_)
        if (p->ok) renderer_->make_resident(p->model);
    if (streamer_) streamer_->update();
}

void GalacticModels::destroy() {
    for (auto& [key, p] : prims_)
        if (p->ok) renderer_->destroy_model(p->model);
    prims_.clear();
    for (auto& [key, p] : lines_)
        if (p->ok) renderer_->destroy_model(p->model);
    lines_.clear();
}

uint32_t GalacticModels::ready() const {
    uint32_t n = 0;
    for (uint32_t h : handles_) n += streamer_->state(h) == ModelStreamer::State::Ready ? 1u : 0u;
    for (const auto& [key, p] : prims_) n += (p->ok && p->model.resident()) ? 1u : 0u;
    for (const auto& [key, p] : lines_) n += (p->ok && p->model.resident()) ? 1u : 0u;
    return n;
}

uint32_t GalacticModels::failed() const {
    uint32_t n = 0;
    for (uint32_t h : handles_) n += streamer_->state(h) == ModelStreamer::State::Failed ? 1u : 0u;
    for (const auto& [key, p] : prims_) n += p->ok ? 0u : 1u;
    for (const auto& [key, p] : lines_) n += p->ok ? 0u : 1u;
    return n;
}

// -------------------------------------------------------------------------------------- route lines
RouteLineStyle load_route_line_style(const FileProvider& files) {
    RouteLineStyle s;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\TradeRouteLines.xml", bytes)) return s;
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "TradeRouteLines.xml");
    if (!doc.ok()) return s;
    data::XmlNode line;
    for (data::XmlNode n : doc.root().children("TradeRouteLine")) {
        if (!line) line = n;  // first entry, unless one is called Default
        std::string nm(n.attribute("Name"));
        if (upper(nm) == "DEFAULT") {
            line = n;
            break;
        }
    }
    if (!line) return s;
    // R2 (STRATEGIC_GAPS §6.1): each block starts from the parser's field defaults, then its tags.
    for (data::XmlNode f : line.children("Settings_For_Faction")) {
        RouteLineSettings r;
        materials::float4 c;
        if (parse_color_bytes(f.child_text("Color_Zoomed_Out"), c)) r.zoomed_out = c;
        if (parse_color_bytes(f.child_text("Color_Zoomed_In"), c)) r.zoomed_in = c;
        {
            const std::string w(f.child_text("Width"));
            const float v = std::strtof(w.c_str(), nullptr);
            if (std::isfinite(v) && v >= 0.0f) r.width = v;
        }
        const std::string mode = trim(std::string(f.child_text("Render_Mode")));
        if (!mode.empty()) r.render_mode = upper(mode);
        const std::string tex = trim(std::string(f.child_text("Texture")));
        if (!tex.empty()) r.texture = tex;
        const std::string spark = trim(std::string(f.child_text("Spark_Texture")));
        if (!spark.empty()) r.spark = spark;
        s.settings[route_settings_index(f.attribute("Name"))] = r;
    }
    return s;
}

RouteLineStyle::RouteLineStyle() {
    auto set = [&](uint32_t k, materials::float4 out, materials::float4 in) {
        settings[k].zoomed_out = out;
        settings[k].zoomed_in = in;
        settings[k].width = 3.0f;
    };
    set(kRouteEnemy, {225 / 255.0f, 0, 0, 1}, {128 / 255.0f, 0, 0, 1});
    set(kRouteAlly, {0, 1, 0, 1}, {0, 128 / 255.0f, 0, 1});
    set(kRouteNeutral, {200 / 255.0f, 200 / 255.0f, 200 / 255.0f, 1}, {150 / 255.0f, 150 / 255.0f, 150 / 255.0f, 1});
    set(kRouteGood, {225 / 255.0f, 0, 0, 1}, {128 / 255.0f, 0, 0, 1});
    set(kRouteEvil, {0, 0, 1, 1}, {0, 0, 1, 1});
    set(kRouteCorrupt, {1, 1, 0, 1}, {128 / 255.0f, 128 / 255.0f, 0, 1});
}

materials::float4 RouteLineStyle::color(RouteClass c, float f) const {
    const float t = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f;
    const materials::float4 a = of(c).zoomed_out, b = of(c).zoomed_in;
    return {a.x * (1.0f - t) + b.x * t, a.y * (1.0f - t) + b.y * t, a.z * (1.0f - t) + b.z * t, a.w * (1.0f - t) + b.w * t};
}

uint32_t RouteLineStyle::index(RouteClass c) {
    switch (c) {
    case RouteClass::Ally: return kRouteAlly;
    case RouteClass::Enemy: return kRouteEnemy;
    case RouteClass::Corrupt: return kRouteCorrupt;
    case RouteClass::Good: return kRouteGood;
    case RouteClass::Evil: return kRouteEvil;
    case RouteClass::Neutral: break;
    }
    return kRouteNeutral;
}

uint32_t route_settings_index(std::string_view name) {
    const std::string n = upper(trim(std::string(name)));
    if (n == "ENEMY") return kRouteEnemy;
    if (n == "ALLY") return kRouteAlly;
    if (n == "GOOD") return kRouteGood;
    if (n == "EVIL") return kRouteEvil;
    if (n == "CORRUPT") return kRouteCorrupt;
    return kRouteNeutral;
}

RouteClass classify_route(std::span<const GalacticPlanetSnap> planets, const GalacticRouteSnap& r, int32_t viewpoint,
                          std::span<const uint8_t> neutral_players, std::span<const uint8_t> sides, bool multiplayer) {
    if (r.a >= planets.size() || r.b >= planets.size()) return RouteClass::Neutral;
    const GalacticPlanetSnap& a = planets[r.a];
    const GalacticPlanetSnap& b = planets[r.b];
    if ((a.flags | b.flags) & kGalFogged) return RouteClass::Neutral;
    if (a.owner != b.owner || a.owner < 0) return RouteClass::Neutral;
    const auto o = static_cast<size_t>(a.owner);
    if (o < neutral_players.size() && neutral_players[o]) return RouteClass::Neutral;
    if (!multiplayer && o < sides.size()) {
        if (sides[o] == kRouteSideGood) return RouteClass::Good;
        if (sides[o] == kRouteSideEvil) return RouteClass::Evil;
        if (sides[o] == kRouteSideOther) return RouteClass::Corrupt;
    }
    return a.owner == viewpoint ? RouteClass::Ally : RouteClass::Enemy;
}

namespace {
// Corruption-state helpers (FIXPASS_GALACTIC 3.2).
bool fading_transition(const GalacticPlanetSnap& p) {
    return (p.flags & kGalCorruptionTransition) && (p.flags & kGalCorruptionFading);
}
bool path_member(const GalacticPlanetSnap& p) {
    if (!(p.flags & kGalPlanet) || (p.flags & kGalFogged)) return false;
    if (p.corruption_type >= 0) return true;  // corrupted (also while any removal runs: lines vanish at completion)
    return fading_transition(p);               // fading gain
}
float path_factor(const GalacticPlanetSnap& p) {
    if (!fading_transition(p)) return 1.0f;
    const float fr = std::isfinite(p.corruption_fraction) ? std::clamp(p.corruption_fraction, 0.0f, 1.0f) : 1.0f;
    return p.corruption_type < 0 ? fr : 1.0f - fr;
}
}  // namespace

CorruptionPathSettings load_corruption_path_settings(const FileProvider& files) {
    CorruptionPathSettings s;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\GameConstants.xml", bytes)) return s;
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "GameConstants.xml");
    if (!doc.ok()) return s;
    const data::XmlNode root = doc.root();
    const std::string tex = trim(std::string(root.child_text("Fleet_Hyperspace_Band_Texture_Name")));
    if (!tex.empty()) s.texture = tex;
    float v[4] = {0, 0, 0, 0};
    if (parse_float_list(root.child_text("Corruption_Path_Width"), v, 1) == 1 && std::isfinite(v[0]) && v[0] >= 0.0f) s.width = v[0];
    if (const size_t n = parse_float_list(root.child_text("Corruption_Path_Color"), v, 4); n >= 3) {
        for (size_t k = 0; k < 4; ++k) {
            const float c = k < n && std::isfinite(v[k]) ? std::clamp(v[k], 0.0f, 255.0f) : 255.0f;
            s.color[k] = static_cast<uint8_t>(std::lround(c));
        }
    }
    if (parse_float_list(root.child_text("Corruption_Path_Offset"), v, 3) == 3 && std::isfinite(v[0]) && std::isfinite(v[1]) &&
        std::isfinite(v[2]))
        s.offset = {v[0], v[1], v[2]};
    if (parse_float_list(root.child_text("MaximumFleetMovementDistance"), v, 1) == 1 && std::isfinite(v[0]) && v[0] >= 0.0f)
        s.neighbour_distance = v[0];
    return s;
}

void galactic_corruption_paths(const GalacticFrame& frame, const CorruptionPathSettings& s, std::vector<CorruptionPathLine>& out) {
    out.clear();
    const std::span<const GalacticPlanetSnap> pl(frame.planets);
    const size_t n = pl.size();
    std::vector<uint8_t> member(n, 0);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        member[i] = path_member(pl[i]) ? 1 : 0;
        any = any || member[i];
    }
    if (!any) return;
    // A* neighbours (0x3ef2d0): a trade route, or distance <= MaximumFleetMovementDistance.
    std::vector<uint8_t> routed(n * n, 0);
    for (const GalacticRouteSnap& r : frame.routes)
        if (r.a < n && r.b < n) routed[size_t{r.a} * n + r.b] = routed[size_t{r.b} * n + r.a] = 1;
    const float d2max = s.neighbour_distance * s.neighbour_distance;
    auto add = [&](size_t owner, size_t other) {
        CorruptionPathLine l;
        l.owner = static_cast<uint32_t>(owner);
        l.other = static_cast<uint32_t>(other);
        l.factor = path_factor(pl[owner]);
        // 0x3fe1b0: (int)((float)((int)(c x 255) & 0xFF) x f) per channel, alpha 255.
        for (int k = 0; k < 3; ++k) l.rgba[k] = static_cast<uint8_t>(static_cast<int>(static_cast<float>(s.color[k]) * l.factor));
        l.rgba[3] = 255;
        if (l.rgba[0] == 0 && l.rgba[1] == 0 && l.rgba[2] == 0) return;
        out.push_back(l);
    };
    for (size_t a = 0; a < n; ++a) {
        if (!member[a]) continue;
        for (size_t b = a + 1; b < n; ++b) {
            if (!member[b]) continue;
            const Vec3 d = sub(pl[b].position, pl[a].position);
            if (!routed[a * n + b] && !(dot(d, d) <= d2max)) continue;
            const bool ta = fading_transition(pl[a]), tb = fading_transition(pl[b]);
            if (!ta && !tb) {
                add(a, b);
                continue;
            }
            if (ta) add(a, b);
            if (tb) add(b, a);
        }
    }
}

// --------------------------------------------------------------------------------------- builder
float GalacticSceneBuilder::planet_radius(objects::TypeIndex t) const {
    return (types_->valid(t) ? (*types_)[t].scale : 1.8f) * kPlanetSphereRadius;
}

Vec3 GalacticSceneBuilder::orbit_slot_offset(uint32_t slot, float planet_radius) {
    const float a = -0.25f * kPi + 2.0f * kPi * static_cast<float>(slot % 8u) / 8.0f;
    const float r = planet_radius * (1.9f + 0.35f * static_cast<float>(slot / 8u));
    return {r * std::cos(a), r * std::sin(a), 0.5f};
}

Vec3 GalacticSceneBuilder::orbit_slot_facing(uint32_t slot) {
    const float a = -0.25f * kPi + 2.0f * kPi * static_cast<float>(slot % 8u) / 8.0f;
    return {-std::sin(a), std::cos(a), 0.0f};  // counter-clockwise tangent
}

Vec3 GalacticSceneBuilder::starbase_offset(float planet_radius) { return {-1.06f * planet_radius, 1.06f * planet_radius, 0.0f}; }

Vec3 GalacticSceneBuilder::fleet_draw_position(const GalacticFleetSnap& f, std::span<const GalacticPlanetSnap> planets) const {
    if ((f.flags & kGalFleetMoving) || f.planet >= planets.size()) return f.position;
    const GalacticPlanetSnap& p = planets[f.planet];
    return add(p.position, orbit_slot_offset(f.orbit_slot, planet_radius(p.type)));
}

std::string GalacticSceneBuilder::planet_model(const GalacticPlanetSnap& p) const {
    if (!types_->valid(p.type)) return {};
    const GalacticTypeInfo& ti = (*types_)[p.type];
    if ((p.flags & kGalDestroyed) && !ti.destroyed_model.empty()) return ti.destroyed_model;
    return ti.model;
}

std::string GalacticSceneBuilder::fleet_model(const GalacticFleetSnap& f) const {
    if (!types_->valid(f.model_type)) return {};
    const GalacticTypeInfo& ti = (*types_)[f.model_type];
    return ti.fleet_override.empty() ? ti.model : ti.fleet_override;
}

assets::Mat3x4 GalacticSceneBuilder::ribbon_transform(const Vec3& a, const Vec3& b, float width) {
    const Vec3 d = sub(b, a);
    Vec3 side = cross({0, 0, 1}, d);
    const float sl = len3(side);
    side = sl > 1e-6f ? mul(side, width / sl) : Vec3{0, width, 0};
    const Vec3 up = mul(norm(cross(d, side)), width);
    assets::Mat3x4 m;
    const Vec3 cols[3] = {d, side, up};
    for (int c = 0; c < 3; ++c) {
        m.m[0][c] = cols[c].x;
        m.m[1][c] = cols[c].y;
        m.m[2][c] = cols[c].z;
    }
    m.m[0][3] = a.x;
    m.m[1][3] = a.y;
    m.m[2][3] = a.z;
    return m;
}

assets::Mat3x4 GalacticSceneBuilder::ring_transform(const Vec3& centre, float radius) {
    assets::Mat3x4 m;
    m.m[0][0] = radius;
    m.m[1][1] = radius;
    m.m[2][2] = radius;
    m.m[0][1] = m.m[0][2] = m.m[1][0] = m.m[1][2] = m.m[2][0] = m.m[2][1] = 0.0f;
    m.m[0][3] = centre.x;
    m.m[1][3] = centre.y;
    m.m[2][3] = centre.z;
    return m;
}

std::span<const assets::Mat3x4> GalacticSceneBuilder::pose(RowAnim& a, const GpuModel* m, const std::string& model,
                                                           const GalacticTypeInfo& ti, float frames, GalacticModelSource& models) {
    if (a.model != model) {
        a = RowAnim{};
        a.model = model;
        a.clip = models.idle_clip(model);
    }
    if (!a.clip || !m || !m->asset || m->asset->skeleton.size() == 0) return {};
    a.asset = m->asset.get();
    if (a.world.size() != a.asset->skeleton.size()) a.world.resize(a.asset->skeleton.size());
    // Clip time: galactic mode frames / 30 x Idle_Anim_00_Rate_Mod (MapScene::idle_anim_seconds on the mode frame).
    const float seconds = static_cast<float>(static_cast<double>(frames) / 30.0 * static_cast<double>(ti.idle_rate));
    assets::Pose p;
    assets::sample(a.asset->skeleton, *a.clip, seconds, ti.loop_idle, p);
    assets::compute_world(a.asset->skeleton, p.local, a.world);
    return a.world;
}

void GalacticSceneBuilder::build(const GalacticFrame& frame, GalacticModelSource& models, std::vector<Instance>& main,
                                 std::vector<Instance>& background, GalacticViewStats* stats) {
    GalacticViewStats st;
    main.clear();
    background.clear();
    if (anims_.size() != frame.planets.size()) anims_.resize(frame.planets.size());
    const std::span<const GalacticPlanetSnap> planets = frame.planets;
    const float overlay = overlay_fade(route_zoom_);  // [port] rings, selection, fleet lines: not in the planet close-up
    // Planets and scenery (row order).
    for (const GalacticPlanetSnap& p : planets) {
        if (!(p.flags & kGalDrawable)) continue;
        const bool fog = (p.flags & kGalFogged) && (p.flags & kGalPlanet) && !(p.flags & kGalBackground);
        const std::string name = planet_model(p);
        const GpuModel* m = models.model(name);
        // A fogged planet is its own model's grid_ALT2 mesh (MeshAlpha w_planet_grid.tga; C2_galactic_silent.png). Without that
        // mesh, or until the model is ready: the procedural globe [port].
        if (fog && !(m && m->max_alt >= kFogPlanetAlt)) {
            const GpuModel* g = models.primitive(GalacticPrimitive::Globe, rgba8_of(opt_.fog_globe_color));
            if (!g) continue;
            Instance in;
            in.model = g;
            in.transform = object_transform(p.position, kIdentityFacing, types_->valid(p.type) ? (*types_)[p.type].scale : 1.8f);
            main.push_back(in);
            ++st.planets;
            ++st.fogged;
            continue;
        }
        if (!m) {
            ++st.no_model;
            continue;
        }
        const GalacticTypeInfo& ti = (*types_)[p.type];
        Instance in;
        in.model = m;
        in.transform = object_transform(p.position, kIdentityFacing, ti.scale);
        in.team_color_override = true;
        in.team_color = p.team_color;
        in.lod_bias = ti.lod_bias;
        in.alt = fog ? kFogPlanetAlt : 0;
        in.bone_world = pose(anims_[p.row], m, name, ti, frame.anim_frames, models);
        st.animated += in.bone_world.empty() ? 0u : 1u;
        st.fogged += fog ? 1u : 0u;
        const bool bg = (p.flags & kGalBackground) != 0;
        (bg ? background : main).push_back(in);
        st.planets += bg ? 0u : 1u;
        st.background += bg ? 1u : 0u;
    }
    // Starbases (row order).
    for (const GalacticPlanetSnap& p : planets) {
        if (!opt_.starbases || p.starbase == objects::kNoObjectId || !types_->valid(p.starbase_type) || (p.flags & kGalFogged)) continue;
        const GalacticTypeInfo& ti = (*types_)[p.starbase_type];
        const GpuModel* m = owner_model(models, ti.model, p.starbase_color);
        if (!m) {
            st.no_model += ti.model.empty() ? 0u : 1u;
            continue;
        }
        Instance in;
        in.model = m;
        in.transform = object_transform(add(p.position, starbase_offset(planet_radius(p.type))), kIdentityFacing, ti.scale);
        in.team_color_override = true;
        in.team_color = p.starbase_color;
        in.lod_bias = ti.lod_bias;
        main.push_back(in);
        ++st.starbases;
    }
    // Ownership rings (row order): owned, non-neutral planets.
    for (const GalacticPlanetSnap& p : planets) {
        if (!opt_.rings || !(overlay > 0.0f) || !(p.flags & kGalPlanet) || p.owner < 0 || (p.flags & kGalFogged)) continue;
        const auto o = static_cast<size_t>(p.owner);
        if (o < opt_.neutral.size() && opt_.neutral[o]) continue;
        const float rb = opt_.ring_brightness;
        const materials::float4 rc{p.team_color.x * rb, p.team_color.y * rb, p.team_color.z * rb, p.team_color.w};
        const GpuModel* m = models.primitive(GalacticPrimitive::Ring, rgba8_of(rc));
        if (!m) continue;
        Instance in;
        in.model = m;
        in.transform = ring_transform(p.position, planet_radius(p.type) * 1.45f);
        main.push_back(in);
        ++st.rings;
    }
    // Trade routes (route order), shortened to the planets' surfaces.
    for (const GalacticRouteSnap& r : frame.routes) {
        if (!opt_.routes || r.a >= planets.size() || r.b >= planets.size()) continue;
        if (r.flags & kGalRouteHidden) {  // GRC 2.4: an endpoint's raw revealed byte is clear
            ++st.hidden_routes;
            continue;
        }
        const RouteClass rc = classify_route(planets, r, opt_.viewpoint, opt_.neutral, opt_.side, opt_.multiplayer);
        // §3.1 zoom blend: Color_Zoomed_In at f = 1. The line stays in the close-up (§3.1 has no hide rule, the sparkles alone
        // stop at f == 1). ADDITIVE with the block's texture; tinted per instance (LIGHT_SCALE), as the corruption paths are.
        const float zf = std::isfinite(route_zoom_) ? std::clamp(route_zoom_, 0.0f, 1.0f) : 0.0f;
        const materials::float4 lc = style_.color(rc, zf);
        const GpuModel* m = models.line(style_.of(rc).texture, rgba8_of(lc));
        if (!m) continue;
        const Vec3 pa = planets[r.a].position, pb = planets[r.b].position;
        const Vec3 d = sub(pb, pa);
        const float len = len3(d);
        if (!(len > 0.0f)) continue;
        const Vec3 u = mul(d, 1.0f / len);
        // Centre to centre (ui_005_gc_running.png [H screenshot]): a route shows on the planet's camera side from where it leaves
        // the sphere and stops at the far silhouette; the depth test hides the part inside.
        Instance in;
        in.model = m;
        in.light_scale = {lc.x, lc.y, lc.z, lc.w};
        in.transform = ribbon_transform(pa, pb, style_.width(rc) * opt_.route_width_scale);
        main.push_back(in);
        ++st.routes;
        // Spark: a bright dash looping along the part outside the planets, phase-shifted per route so they don't march in step.
        const float ra = planet_radius(planets[r.a].type) * 1.15f, rb = planet_radius(planets[r.b].type) * 1.15f;
        const float vis = len - ra - rb, sl = std::min(opt_.spark_length, vis);
        // §3.1: the sparkles are off on the Neutral slot and when fully zoomed in (f == 1).
        // [port] the opaque dash also narrows away with the overlays.
        if (!opt_.route_sparks || rc == RouteClass::Neutral || zf >= 1.0f || !(overlay > 0.0f) || !(sl > 0.0f) ||
            !(opt_.spark_speed > 0.0f) || !std::isfinite(frame.anim_frames))
            continue;
        const float room = vis - sl;
        const float phase = 0.618034f * static_cast<float>(&r - frame.routes.data());
        const float travel = frame.anim_frames * opt_.spark_speed + phase * vis;
        const float s0 = room > 0.0f ? std::fmod(travel, room) : 0.0f;
        const materials::float4 sc{0.5f + 0.5f * lc.x, 0.5f + 0.5f * lc.y, 0.5f + 0.5f * lc.z, lc.w};
        const GpuModel* sm = models.primitive(GalacticPrimitive::Ribbon, rgba8_of(sc));
        if (!sm) continue;
        const Vec3 a0 = add(add(pa, mul(u, ra + s0)), Vec3{0, 0, 0.05f});  // [port] lifted off the line (no z-fight)
        Instance sp;
        sp.model = sm;
        sp.transform = ribbon_transform(a0, add(a0, mul(u, sl)), opt_.spark_width_scale * style_.width(rc) * overlay);
        main.push_back(sp);
        ++st.route_sparks;
    }
    // Fleets in transit (ascending id). §2.1: no fleet model in orbit; one _ALT (stealth -> ALT1, else ALT0).
    for (const GalacticFleetSnap& f : frame.fleets) {
        if (!(f.flags & kGalFleetDrawable)) continue;
        if (!fleet_in_transit(f, planets.size())) {
            if (planets[f.planet].flags & kGalFogged) ++st.fogged_fleets;  // orbiting a planet the local player does not see
            else ++st.orbit_fleets;
            continue;
        }
        const std::string name = fleet_model(f);
        const GpuModel* m = owner_model(models, name, f.team_color);
        if (!m) {
            ++st.no_model;
            continue;
        }
        const GalacticTypeInfo& ti = (*types_)[f.model_type];
        Instance in;
        in.model = m;
        // [inferred] the Fleet_ALT0 darts taper to +Y, the other way round from a ship's -Y nose, and the marker points along the hop.
        in.transform = object_transform(f.position, mul(f.facing, -1.0f), ti.scale);
        in.team_color_override = true;
        in.team_color = f.team_color;
        in.lod_bias = ti.lod_bias;
        in.alt = fleet_alt(f);
        main.push_back(in);
        ++st.fleets;
        st.stealth_fleets += in.alt == 1 ? 1u : 0u;
    }
    // Movement lines of fleets in flight (ascending id).
    for (const GalacticFleetSnap& f : frame.fleets) {
        if (!opt_.fleet_lines || !(overlay > 0.0f) || !(f.flags & kGalFleetMoving) || f.to >= planets.size()) continue;
        const Vec3 a = f.position, b = planets[f.to].position;
        const Vec3 d = sub(b, a);
        const float len = len3(d), rb = planet_radius(planets[f.to].type) * 1.15f;
        if (!(len > rb + 0.5f)) continue;
        const GpuModel* m = models.primitive(GalacticPrimitive::Ribbon, rgba8_of(f.team_color));
        if (!m) continue;
        Instance in;
        in.model = m;
        in.transform = ribbon_transform(a, sub(b, mul(d, rb / len)), opt_.fleet_line_width * overlay);
        main.push_back(in);
        ++st.fleet_lines;
    }
    // Corruption path lines (FIXPASS_GALACTIC §3.2): additive textured ribbons, centre to centre (+ offset), width
    // Corruption_Path_Width x corruption_width_scale [port: the original's width unit is not documented]. Like the routes
    // (§3.1) they have no zoom hide rule: §3.2's only tint is the corruption factor.
    if (opt_.corruption_paths) {
        galactic_corruption_paths(frame, opt_.corruption_path, paths_);
        const CorruptionPathSettings& cp = opt_.corruption_path;
        for (const CorruptionPathLine& l : paths_) {
            const GpuModel* m = models.line(cp.texture, rgba8(l.rgba[0], l.rgba[1], l.rgba[2], l.rgba[3]));
            if (!m) continue;
            Instance in;
            in.model = m;
            // Tint per instance: MeshAdditive Color (white) x LIGHT_SCALE.rgb x LIGHT_SCALE.w.
            in.light_scale = {static_cast<float>(l.rgba[0]) / 255.0f, static_cast<float>(l.rgba[1]) / 255.0f,
                              static_cast<float>(l.rgba[2]) / 255.0f, static_cast<float>(l.rgba[3]) / 255.0f};
            in.transform = ribbon_transform(add(planets[l.owner].position, cp.offset), add(planets[l.other].position, cp.offset),
                                            cp.width * opt_.corruption_width_scale);
            main.push_back(in);
            ++st.corruption_paths;
        }
    }
    // Selection model round the selected planet (§3).
    if (opt_.selection && overlay > 0.0f && sel_.planet != objects::kNoObjectId && !opt_.selection_model.empty()) {
        const GalacticPlanetSnap* sp = nullptr;
        for (const GalacticPlanetSnap& p : planets)
            if (p.id == sel_.planet && (p.flags & kGalPlanet)) {
                sp = &p;
                break;
            }
        const GpuModel* m = sp ? models.model(opt_.selection_model) : nullptr;
        if (sp && !m) ++st.no_model;
        if (m) {
            const materials::float4 c = (sp->flags & kGalFogged) ? opt_.selection_unrevealed_color : sp->team_color;
            GalacticTypeInfo ti;  // IDLE_00 loops at its own rate
            ti.loop_idle = true;
            Instance in;
            in.model = m;
            in.transform = selection_transform(sp->position, sel_);
            // Instance colour (+0x13c..+0x148) -> LIGHT_SCALE.rgb; [port] the additive meshes fade with both, w is the alpha.
            in.light_scale = {c.x * overlay, c.y * overlay, c.z * overlay, overlay};
            in.bone_world = pose(sel_anim_, m, opt_.selection_model, ti, frame.anim_frames, models);
            st.animated += in.bone_world.empty() ? 0u : 1u;
            main.push_back(in);
            ++st.selection;
        }
    }
    if (stats) *stats = st;
}

float GalacticSceneBuilder::overlay_fade(float zoom) {
    const float f = std::isfinite(zoom) ? std::clamp(zoom, 0.0f, 1.0f) : 0.0f;
    return std::max(0.0f, 1.0f - f / 0.25f);
}

bool GalacticSceneBuilder::fleet_in_transit(const GalacticFleetSnap& f, size_t planet_rows) {
    return (f.flags & kGalFleetMoving) != 0 || f.planet >= planet_rows;
}

assets::Mat3x4 GalacticSceneBuilder::selection_transform(const Vec3& centre, const GalacticSelection& s) {
    Vec3 z = sub(s.eye, s.target);  // toward the camera
    if (!(len3(z) > 1e-6f)) z = {0, 0, 1};
    z = norm(z);
    Vec3 x = cross(mul(z, -1.0f), s.up);  // forward x up = screen right
    if (!(len3(x) > 1e-6f)) x = cross(mul(z, -1.0f), std::fabs(z.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    x = norm(x);
    const Vec3 y = cross(z, x);
    assets::Mat3x4 m;
    const Vec3 cols[3] = {x, y, z};
    for (int c = 0; c < 3; ++c) {
        m.m[0][c] = cols[c].x;
        m.m[1][c] = cols[c].y;
        m.m[2][c] = cols[c].z;
    }
    m.m[0][3] = centre.x;
    m.m[1][3] = centre.y;
    m.m[2][3] = centre.z;
    return m;
}

namespace {
const GpuModel* owner_model(GalacticModelSource& models, const std::string& name, const materials::float4& color) {
    GalacticPrimitive kind{};
    uint32_t rgba = 0;
    if (parse_primitive_model_name(name, kind, rgba)) return models.primitive(kind, rgba8_of(color));
    return models.model(name);
}
}  // namespace

GalacticLightSettings load_galactic_light_settings(const FileProvider& files) {
    GalacticLightSettings s;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\GameConstants.xml", bytes)) return s;
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "GameConstants.xml");
    if (!doc.ok()) return s;
    const data::XmlNode root = doc.root();
    auto angles = [&](std::string_view tag, float& yaw, float& elev) {
        float v[3] = {0, 0, 0};
        if (parse_float_list(root.child_text(tag), v, 3) == 3 && std::isfinite(v[1]) && std::isfinite(v[2])) {
            yaw = v[1];  // element [0] unused (STRATEGIC_GAPS §6.1)
            elev = v[2];
        }
    };
    angles("Galactic_Zoom_Out_Light_Angle", s.out_yaw_deg, s.out_elevation_deg);
    angles("Galactic_Zoom_In_Light_Angle", s.in_yaw_deg, s.in_elevation_deg);
    float lv = 0.0f;
    if (parse_float_list(root.child_text("Galactic_Zoom_Light_Level"), &lv, 1) == 1 && std::isfinite(lv)) s.zoom_light_level = lv;
    return s;
}

GalacticCorruptionSettings load_galactic_corruption_settings(const FileProvider& files) {
    GalacticCorruptionSettings s;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\GameConstants.xml", bytes)) return s;
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "GameConstants.xml");
    if (!doc.ok()) return s;
    const data::XmlNode root = doc.root();
    const std::string pn = trim(std::string(root.child_text("Corruption_Particle_Name")));
    const std::string ln = trim(std::string(root.child_text("Corruption_Particle_Line_Name")));
    if (!pn.empty()) s.planet_particle = pn;
    if (!ln.empty()) s.line_particle = ln;
    float b[2] = {0, 0};
    if (parse_float_list(root.child_text("Particle_Brightness_Per_Corruption_Level"), b, 2) == 2 && std::isfinite(b[0]) &&
        std::isfinite(b[1])) {
        s.brightness[0] = b[0];
        s.brightness[1] = b[1];
    }
    return s;
}

void galactic_corruption_effects(const GalacticFrame& frame, const GalacticCorruptionSettings& s,
                                 std::vector<GalacticCorruptionEffect>& out) {
    out.clear();
    const std::span<const GalacticPlanetSnap> planets(frame.planets);
    for (size_t i = 0; i < planets.size(); ++i) {
        const GalacticPlanetSnap& p = planets[i];
        const bool particles = (p.flags & kGalCorruptionParticles) != 0;
        if (!(p.flags & kGalPlanet) || (p.flags & kGalFogged) || !(particles || (p.flags & kGalCorrupted))) continue;
        GalacticCorruptionEffect e;
        e.key = galactic_corruption_planet_key(static_cast<uint32_t>(i));
        e.type = &s.planet_particle;
        e.transform = object_transform(p.position, kIdentityFacing, 1.0f);
        e.corruption_type = p.corruption_effective;
        if (particles) {
            const float f = p.corruption_fraction;
            e.scalars = true;
            e.brightness = galactic_corruption_lerp(p.corruption_pairs[0], p.corruption_pairs[1], f);
            e.scale = galactic_corruption_lerp(p.corruption_pairs[2], p.corruption_pairs[3], f);
            e.energy = galactic_corruption_lerp(p.corruption_pairs[4], p.corruption_pairs[5], f);
        }
        out.push_back(e);
    }
    for (size_t i = 0; i < frame.routes.size(); ++i) {
        const GalacticRouteSnap& r = frame.routes[i];
        // Midpoint line particles between two corrupted planets (§3.2; untinted). [port] trade routes only, both ends
        // visible, corrupted (kGalCorrupted).
        if ((r.flags & kGalRouteHidden) || r.a >= planets.size() || r.b >= planets.size()) continue;
        const GalacticPlanetSnap& ea = planets[r.a];
        const GalacticPlanetSnap& eb = planets[r.b];
        if (((ea.flags | eb.flags) & kGalFogged) || !(ea.flags & kGalCorrupted) || !(eb.flags & kGalCorrupted)) continue;
        const Vec3 pa = planets[r.a].position, pb = planets[r.b].position;
        const Vec3 d = sub(pb, pa);
        const float len = len3(d);
        GalacticCorruptionEffect e;
        e.key = galactic_corruption_route_key(static_cast<uint32_t>(i));
        e.type = &s.line_particle;
        e.line = true;
        // Local +X along the route: the facing is the route turned -90 degrees about up (object_transform: X = up x facing).
        e.transform = object_transform(mul(add(pa, pb), 0.5f), len > 0.0f ? Vec3{d.y / len, -d.x / len, 0.0f} : kIdentityFacing, 1.0f);
        out.push_back(e);
    }
}

Lighting galactic_lighting(const GalacticLightSettings& s, float zoom) {
    Lighting l = Lighting::space_default();
    const float t = std::isfinite(zoom) ? std::clamp(zoom, 0.0f, 1.0f) : 0.0f;
    const float deg = kPi / 180.0f;
    const float yaw = s.out_yaw_deg + (s.in_yaw_deg - s.out_yaw_deg) * t;
    const float elev = s.out_elevation_deg + (s.in_elevation_deg - s.out_elevation_deg) * t;
    l.set_sun_angles(yaw * deg, elev * deg);
    l.sun.color = {1, 1, 1};
    l.sun.specular = {1, 1, 1};  // [M] the record's specular is not listed; white like its colour
    l.sun.intensity = s.intensity + (s.zoom_light_level - s.intensity) * t;
    l.fill[0].intensity = l.fill[1].intensity = 0.0f;
    l.ambient = {0.05f, 0.05f, 0.05f};
    l.clear_color = {0, 0, 0};
    l.fog_enabled = false;
    l.environment = "galactic light record";
    l.skydome[0].clear();
    l.skydome[1].clear();
    return l;
}

Lighting galactic_lighting() { return galactic_lighting(GalacticLightSettings{}); }

}  // namespace alamo::render
