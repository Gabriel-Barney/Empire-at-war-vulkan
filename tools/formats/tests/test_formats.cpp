// Self-contained unit tests on synthetic data (no game assets needed).
#include "alamo/formats/ala.hpp"
#include "alamo/formats/alo.hpp"
#include "alamo/formats/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace alamo;

static int g_fail = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) { std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #x); ++g_fail; } \
    } while (0)

namespace {
struct W {
    std::vector<std::uint8_t> b;
    template <class T> W& put(T v) { auto p = reinterpret_cast<const std::uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v); return *this; }
    W& str(const char* s, bool nul = true) { b.insert(b.end(), s, s + std::strlen(s)); if (nul) b.push_back(0); return *this; }
    W& raw(const std::vector<std::uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); return *this; }
};
std::vector<std::uint8_t> chunk(std::uint32_t id, const std::vector<std::uint8_t>& payload, bool children) {
    W w;
    w.put(id).put(std::uint32_t(payload.size()) | (children ? 0x80000000u : 0u)).raw(payload);
    return w.b;
}
std::vector<std::uint8_t> mini(std::uint8_t id, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> v{id, std::uint8_t(payload.size())};
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}
template <class T> std::vector<std::uint8_t> bytes_of(T v) { W w; w.put(v); return w.b; }
std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> l) {
    std::vector<std::uint8_t> o;
    for (auto& v : l) o.insert(o.end(), v.begin(), v.end());
    return o;
}

void test_chunks() {
    auto leaf = chunk(0x10, {1, 2, 3}, false);
    auto parent = chunk(0x20, cat({leaf, leaf}), true);
    ChunkCursor c(parent);
    auto p = c.next();
    CHECK(p && p->id == 0x20 && p->has_children && p->data.size() == 22);
    auto kids = p->children().all();
    CHECK(kids.size() == 2 && kids[1].id == 0x10 && kids[1].offset == 8 + 11);
    // overrun detection
    auto bad = parent;
    bad[4] = 0xFF;
    bool threw = false;
    try { ChunkCursor(bad).next(); } catch (const FormatError&) { threw = true; }
    CHECK(threw);
    CHECK(crc32(std::string_view("123456789")) == 0xCBF43926u);
}

void test_model() {
    // skeleton: Root + child bone
    auto mat = [](float tx) {
        W w;
        float m[12] = {1, 0, 0, tx, 0, 1, 0, 0, 0, 0, 1, 0};
        for (float f : m) w.put(f);
        return w.b;
    };
    W hdr; hdr.put(std::uint32_t(2)); hdr.b.resize(128, 0);
    auto bone = [&](const char* n, std::int32_t parent, float tx) {
        W info; info.put(parent).put(std::uint32_t(1)).put(std::uint32_t(0)).raw(mat(tx));
        return chunk(0x202, cat({chunk(0x203, W().str(n).b, false), chunk(0x206, info.b, false)}), true);
    };
    auto skel = chunk(0x200, cat({chunk(0x201, hdr.b, false), bone("Root", -1, 0), bone("B", 0, 5)}), true);
    // mesh with one triangle
    W mi; mi.put(std::uint32_t(1)); for (int i = 0; i < 6; ++i) mi.put(float(i)); mi.put(0u).put(0u).put(0u); mi.b.resize(128, 0);
    auto param = chunk(0x10105, cat({mini(1, W().str("BaseTexture").b), mini(2, W().str("a.dds").b)}), false);
    auto material = chunk(0x10100, cat({chunk(0x10101, W().str("MeshGloss.fx").b, false), param}), true);
    W sinfo; sinfo.put(3u).put(1u); sinfo.b.resize(128, 0);
    W verts;
    for (int v = 0; v < 3; ++v) {
        float pos[3] = {float(v), 0, 0}, n[3] = {0, 0, 1};
        for (float f : pos) verts.put(f);
        for (float f : n) verts.put(f);
        for (int k = 0; k < 8 + 3 + 3 + 4 + 4; ++k) verts.put(0.f);
        for (int k = 0; k < 4; ++k) verts.put(0u);
        for (int k = 0; k < 4; ++k) verts.put(k == 0 ? 1.f : 0.f);
    }
    W idx; idx.put(std::uint16_t(0)).put(std::uint16_t(1)).put(std::uint16_t(2));
    auto sub = chunk(0x10000, cat({chunk(0x10001, sinfo.b, false), chunk(0x10002, W().str("alD3dVertNU2").b, false),
                                   chunk(0x10007, verts.b, false), chunk(0x10004, idx.b, false)}), true);
    auto mesh = chunk(0x400, cat({chunk(0x401, W().str("M").b, false), chunk(0x402, mi.b, false), material, sub}), true);
    auto conn = chunk(0x600, cat({chunk(0x601, cat({mini(1, bytes_of(1u)), mini(4, bytes_of(1u))}), false),
                                  chunk(0x602, cat({mini(2, bytes_of(0u)), mini(3, bytes_of(1u))}), false),
                                  chunk(0x603, cat({mini(5, W().str("P").b), mini(6, bytes_of(1u))}), false)}), true);
    auto file = cat({skel, mesh, conn});
    auto f = alo::parse(file);
    CHECK(f.kind == alo::AloFile::Kind::Model);
    const auto& m = f.model;
    CHECK(m.skeleton.size() == 2 && m.skeleton.names[1] == "B" && m.skeleton.parents[1] == 0);
    CHECK(std::fabs(m.skeleton.world(1).m[0][3] - 5) < 1e-6);
    CHECK(m.meshes.size() == 1 && m.meshes[0].submeshes.size() == 1);
    const auto& sm = m.meshes[0].submeshes[0];
    CHECK(sm.vertices.size() == 3 && sm.indices.size() == 3 && sm.format.uv_sets == 1);
    CHECK(sm.material.shader == "MeshGloss.fx");
    auto* p = sm.material.find("basetexture");
    CHECK(p && std::get<std::string>(p->value) == "a.dds");
    CHECK(m.mesh_bone(0) == 1 && m.proxies.size() == 1 && m.proxies[0].name == "P");
    CHECK(m.unknown.empty());
    // corrupt: index out of range
    auto bad = file;
    auto pos = std::search(bad.begin(), bad.end(), idx.b.begin(), idx.b.end());
    pos[4] = 9;
    bool threw = false;
    try { alo::parse(bad); } catch (const FormatError&) { threw = true; }
    CHECK(threw);
}

// One-bone model with a single 3-vertex submesh: vertex chunk `vid`, `stride` bytes per vertex
// (128 or 144), format `fmt`, optional bone map. Vertex v: pos (v,0,0), normal +Z, uv (v,1),
// tangent +X, binormal +Y, colour (0.5,0.25,v,0), [unknown 1,1,1,1 when 144], bone indices
// (0, v, 0, 0), weights (0.25, 0.75, 0, 0) for B4I4-style records or (1,0,0,0).
std::vector<std::uint8_t> vertex_model(std::uint32_t vid, int stride, const char* fmt, bool four_bones,
                                       const std::vector<std::uint32_t>& bone_map) {
    W hdr; hdr.put(std::uint32_t(1)); hdr.b.resize(128, 0);
    W info; info.put(std::int32_t(-1)).put(std::uint32_t(1)).put(std::uint32_t(0));
    for (float f : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f}) info.put(f);
    auto skel = chunk(0x200, cat({chunk(0x201, hdr.b, false),
                                  chunk(0x202, cat({chunk(0x203, W().str("Root").b, false), chunk(0x206, info.b, false)}), true)}), true);
    W mi; mi.put(std::uint32_t(1)); for (int i = 0; i < 6; ++i) mi.put(float(i)); mi.put(0u).put(0u).put(0u); mi.b.resize(128, 0);
    auto material = chunk(0x10100, chunk(0x10101, W().str("MeshGloss.fx").b, false), true);
    W sinfo; sinfo.put(3u).put(1u); sinfo.b.resize(128, 0);
    W verts;
    for (std::uint32_t v = 0; v < 3; ++v) {
        const float fv = float(v);
        for (float f : {fv, 0.f, 0.f, 0.f, 0.f, 1.f, fv, 1.f}) verts.put(f);
        for (int k = 0; k < 6; ++k) verts.put(0.f);
        for (float f : {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.5f, 0.25f, fv, 0.f}) verts.put(f);
        if (stride == 144) for (int k = 0; k < 4; ++k) verts.put(1.f);
        verts.put(0u).put(v).put(0u).put(0u);
        const float w4[4] = {four_bones ? 0.25f : 1.f, four_bones ? 0.75f : 0.f, 0.f, 0.f};
        for (float f : w4) verts.put(f);
    }
    W idx; idx.put(std::uint16_t(0)).put(std::uint16_t(1)).put(std::uint16_t(2));
    auto subp = cat({chunk(0x10001, sinfo.b, false), chunk(0x10002, W().str(fmt).b, false), chunk(vid, verts.b, false),
                     chunk(0x10004, idx.b, false)});
    if (!bone_map.empty()) {
        W bm;
        for (auto b : bone_map) bm.put(b);
        subp = cat({subp, chunk(0x10006, bm.b, false)});
    }
    auto mesh = chunk(0x400, cat({chunk(0x401, W().str("M").b, false), chunk(0x402, mi.b, false), material,
                                  chunk(0x10000, subp, true)}), true);
    auto conn = chunk(0x600, cat({chunk(0x601, cat({mini(1, bytes_of(1u)), mini(4, bytes_of(0u))}), false),
                                  chunk(0x602, cat({mini(2, bytes_of(0u)), mini(3, bytes_of(0u))}), false)}), true);
    return cat({skel, mesh, conn});
}

bool parses(const std::vector<std::uint8_t>& f) {
    try { alo::parse(f); } catch (const FormatError&) { return false; }
    return true;
}

void test_vertex_layouts() {
    // Legacy 128-byte records in 0x10005 decode to the same fields as the 144-byte layout.
    {
        const auto f = alo::parse(vertex_model(0x10005, 128, "alD3dVertNU2C", false, {}));
        const auto& sm = f.model.meshes.at(0).submeshes.at(0);
        const auto& vb = sm.vertices;
        CHECK(sm.vertex_chunk_id == 0x10005 && sm.vertex_stride == 128 && vb.size() == 3);
        CHECK(vb.position[2].x == 2.f && vb.normal[1].z == 1.f && vb.uv[0][2].x == 2.f && vb.uv[0][2].y == 1.f);
        CHECK(vb.tangent[1].x == 1.f && vb.binormal[1].y == 1.f);
        CHECK(vb.color[2].x == 0.5f && vb.color[2].y == 0.25f && vb.color[2].z == 2.f && vb.color[2].w == 0.f);
        CHECK(vb.extra[0].x == 1.f && vb.extra[0].w == 1.f);  // not stored: default
        CHECK(vb.bone_index[2][1] == 2 && vb.bone_weight[2].x == 1.f);
        CHECK(sm.format.known && sm.format.color && !sm.format.skinned);
        // 144-byte records in 0x10005 are still accepted; 128-byte records only in 0x10005.
        CHECK(alo::parse(vertex_model(0x10005, 144, "alD3dVertNU2", false, {})).model.meshes[0].submeshes[0].vertex_stride == 144);
        CHECK(!parses(vertex_model(0x10007, 128, "alD3dVertNU2", false, {})));
        CHECK(f.model.unknown.empty());
    }
    // B4I4: four influences; every index with a non-zero weight must be inside the bone map.
    {
        const auto f = alo::parse(vertex_model(0x10007, 144, "alD3dVertB4I4NU2U3U3", true, {0, 0, 0}));
        const auto& sm = f.model.meshes.at(0).submeshes.at(0);
        CHECK(sm.format.known && sm.format.skinned && sm.format.bone_influences == 4 && sm.format.tangent_frame);
        CHECK(sm.vertices.bone_index[2][1] == 2 && sm.vertices.bone_weight[2].y == 0.75f);
        CHECK(parses(vertex_model(0x10005, 128, "alD3dVertB4I4NU2", true, {0, 0, 0})));
        CHECK(!parses(vertex_model(0x10007, 144, "alD3dVertB4I4NU2", true, {0, 0})));  // index 2, weight 0.75
        // RSkin only checks bone_index[0] (index 1 carries weight 0 there).
        CHECK(parses(vertex_model(0x10007, 144, "alD3dVertRSkinNU2", false, {0})));
        CHECK(alo::vertex_format_info("alD3dVertRSkinNU2").bone_influences == 1);
    }
    // Vertex-format table and token inference.
    {
        const auto c = alo::vertex_format_info("alD3dVertNU2U3U3C");
        CHECK(c.known && c.color && c.tangent_frame && c.uv_sets == 1 && !c.skinned);
        const auto u = alo::vertex_format_info("alD3dVertFooU2U3U3C");
        CHECK(!u.known && u.color && u.tangent_frame && u.uv_sets == 1 && !u.skinned);
        const auto b = alo::vertex_format_info("alD3dVertB4I4Foo");
        CHECK(!b.known && b.skinned && b.bone_influences == 4 && !b.color);
        CHECK(alo::vertex_format_info("ALD3DVERTNU2").known);
    }
}

void test_anim() {
    auto info = chunk(0x1001, cat({mini(1, bytes_of(2u)), mini(2, bytes_of(30.f)), mini(3, bytes_of(1u)),
                                   mini(0xB, bytes_of(4u)), mini(0xC, bytes_of(3u)), mini(0xD, bytes_of(0u))}), false);
    W off; off.put(1.f).put(2.f).put(3.f);
    W sc; sc.put(0.5f).put(0.f).put(0.f);
    W one; one.put(1.f).put(1.f).put(1.f);
    W zero; zero.put(0.f).put(0.f).put(0.f);
    auto binfo = chunk(0x1003, cat({mini(4, W().str("Bone").b), mini(5, bytes_of(1u)), mini(0xA, bytes_of(0u)),
                                    mini(6, off.b), mini(7, sc.b), mini(8, one.b), mini(9, zero.b),
                                    mini(0xE, bytes_of(std::uint16_t(0))), mini(0xF, bytes_of(std::uint16_t(0xFFFF))),
                                    mini(0x10, bytes_of(std::uint16_t(0))), mini(0x11, std::vector<std::uint8_t>(8, 0))}), false);
    auto bone = chunk(0x1002, binfo, true);
    W rot; for (int f = 0; f < 2; ++f) rot.put(std::int16_t(0)).put(std::int16_t(0)).put(std::int16_t(0)).put(std::int16_t(32767));
    W tr; tr.put(std::uint16_t(0)).put(std::uint16_t(0)).put(std::uint16_t(0)).put(std::uint16_t(4)).put(std::uint16_t(0)).put(std::uint16_t(0));
    auto file = chunk(0x1000, cat({info, bone, chunk(0x1009, rot.b, false), chunk(0x100A, tr.b, false)}), true);
    auto a = ala::parse(file);
    CHECK(a.layout == ala::Layout::Block && a.num_frames == 2 && a.bones.size() == 1);
    CHECK(std::fabs(a.translation(0, 1).x - 3.f) < 1e-6);
    CHECK(std::fabs(a.rotation(0, 0).w - 1.f) < 1e-6);
    CHECK(a.visible(0, 1));
}

void test_text() {
    W m; m.put(1u); auto name = W().str("A.TGA").b; name.resize(64, 0); m.raw(name).put(1u).put(2u).put(3u).put(4u).put(std::uint8_t(1));
    auto at = mtd::parse(m.b);
    CHECK(at.size() == 1 && at.names[0] == "A.TGA" && at.height[0] == 4 && *at.find("a.tga") == 0);
    W d; d.put(1u).put(crc32(std::string_view("KEY"))).put(2u).put(3u).put(std::uint16_t('h')).put(std::uint16_t('i')).str("KEY", false);
    auto t = dat::parse(d.b);
    CHECK(t.size() == 1 && t.find("KEY") && dat::to_utf8(*t.find("KEY")) == "hi" && t.crc_mismatches() == 0);
}
}  // namespace

void test_emitter_desc() {
    auto block = cat({mini(0x04, bytes_of(2u)), mini(0x07, {1}), mini(0x0F, bytes_of(2.5f)),
                      mini(0x2A, bytes_of(30u)), mini(0x0A, cat({bytes_of(1.f), bytes_of(2.f), bytes_of(3.f)})),
                      mini(0x77, {9, 9})});
    auto d = particle::parse_emitter_desc(block);
    CHECK(d.blend_mode == 2 && d.use_bursts && d.lifetime == 2.5f && d.particles_per_second == 30);
    CHECK(d.acceleration.z == 3.f && d.present.test(0x0F) && !d.present.test(0x10));
    CHECK(d.unknown.size() == 1 && d.unknown[0].id == 0x77);
    bool threw = false;
    try { particle::parse_emitter_desc(mini(0x0F, {1, 2})); } catch (const FormatError&) { threw = true; }
    CHECK(threw);
    CHECK(particle::emitter_field(0x3D) && std::string(particle::emitter_field(0x3D)->name) == "is_weather_particle");
}

int main() {
    test_emitter_desc();
    test_chunks();
    test_model();
    test_vertex_layouts();
    test_anim();
    test_text();
    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("all tests passed\n");
    return 0;
}
