// alamo_assets tests (no framework; non-zero exit on failure).
//   * Synthetic in-memory ALO / ALA / DDS / TGA round-trips and AssetCache concurrency: always run.
//   * Real data (ALAMO_FOC_DATA = folder containing FoC's Data\, built with alamo_vfs): every ALO
//     and ALA the VFS lists, every texture they reference, a capital ship, and a comparison against
//     the Phase 1 glTF exports in docs/formats/examples.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/assets/model.hpp"
#include "alamo/assets/skeleton.hpp"
#include "alamo/assets/texture.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/test_support.hpp"
#include "alamo/formats/alo.hpp"
#include "mini_json.hpp"

#if defined(ALAMO_ASSETS_HAVE_VFS)
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo::assets;
using Bytes = std::vector<std::uint8_t>;

namespace {

bool approx(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// ---- byte builders ----------------------------------------------------------------------------
struct W {
    Bytes b;
    template <class T> W& put(T v) {
        // resize + memcpy rather than insert(p, p + n): GCC 14 -Wnonnull false positive on an empty b.
        const std::size_t at = b.size();
        b.resize(at + sizeof v);
        std::memcpy(b.data() + at, &v, sizeof v);
        return *this;
    }
    W& str(const char* s) {
        b.insert(b.end(), s, s + std::strlen(s));
        b.push_back(0);
        return *this;
    }
    W& raw(const Bytes& o) {
        b.insert(b.end(), o.begin(), o.end());
        return *this;
    }
    W& pad_to(std::size_t n) {
        b.resize(std::max(b.size(), n), 0);
        return *this;
    }
};
Bytes chunk(std::uint32_t id, const Bytes& payload, bool children) {
    W w;
    w.put(id).put(static_cast<std::uint32_t>(payload.size()) | (children ? 0x80000000u : 0u)).raw(payload);
    return w.b;
}
Bytes mini(std::uint8_t id, const Bytes& payload) {
    Bytes v{id, static_cast<std::uint8_t>(payload.size())};
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}
template <class T> Bytes bytes_of(T v) { return W().put(v).b; }
Bytes cat(std::initializer_list<Bytes> l) {
    Bytes o;
    for (const auto& v : l) o.insert(o.end(), v.begin(), v.end());
    return o;
}

// ---- synthetic ALO ----------------------------------------------------------------------------
struct SynVert {
    float pos[3];
    float nrm[3];
    float uv[2];
    std::uint32_t bone;
};
struct SynSub {
    std::string shader, format;
    std::vector<SynVert> verts;
    std::vector<std::uint16_t> idx;
    std::vector<std::uint32_t> bone_map;
    std::string texture;  // BaseTexture
};
struct SynMesh {
    std::string name;
    bool hidden = false;
    std::uint32_t bone = 0;
    std::vector<SynSub> subs;
};
struct SynBone {
    std::string name;
    std::int32_t parent;
    float tx, ty, tz;
};

// legacy = EaW-era 0x10005 chunk with 128-byte vertices (colour 1,0.5,0,0, B4I4 style weights 0.2/0.8).
// rskin_junk: non-legacy vertices carry indices (bone, 0xDEAD, 0, 0) and weights (0.2, 0.8, 0, 0);
// RSkin must still use bone_index[0] (the parser range-checks only that one for RSkin).
// zero_frame: tangent and binormal stored as zero (as the exporter writes for formats without a frame).
// chunk5_144: 144-byte records stored in a 0x10005 chunk (accepted by the parser, SPEC.md 2.5).
Bytes make_alo(const std::vector<SynBone>& bones, const std::vector<SynMesh>& meshes, bool legacy = false,
               bool rskin_junk = false, bool zero_frame = false, bool chunk5_144 = false) {
    W hdr;
    hdr.put(static_cast<std::uint32_t>(bones.size())).pad_to(128);
    Bytes skel_payload = chunk(0x201, hdr.b, false);
    for (const auto& b : bones) {
        W info;
        info.put(b.parent).put(std::uint32_t(1)).put(std::uint32_t(0));
        const float m[12] = {1, 0, 0, b.tx, 0, 1, 0, b.ty, 0, 0, 1, b.tz};
        for (float f : m) info.put(f);
        W nm;
        nm.str(b.name.c_str());
        skel_payload = cat({skel_payload, chunk(0x202, cat({chunk(0x203, nm.b, false), chunk(0x206, info.b, false)}), true)});
    }
    Bytes file = chunk(0x200, skel_payload, true);
    for (const auto& m : meshes) {
        W mi;
        mi.put(static_cast<std::uint32_t>(m.subs.size()));
        for (int i = 0; i < 6; ++i) mi.put(float(i < 3 ? -1 : 1));
        mi.put(0u).put(m.hidden ? 1u : 0u).put(0u).pad_to(128);
        W nm;
        nm.str(m.name.c_str());
        Bytes payload = cat({chunk(0x401, nm.b, false), chunk(0x402, mi.b, false)});
        for (const auto& s : m.subs) {
            W sh;
            sh.str(s.shader.c_str());
            Bytes mat = chunk(0x10101, sh.b, false);
            W pe;
            pe.put(1.f).put(0.5f).put(0.25f).put(1.f);
            mat = cat({mat, chunk(0x10106, cat({mini(1, W().str("Diffuse").b), mini(2, pe.b)}), false)});
            mat = cat({mat, chunk(0x10103, cat({mini(1, W().str("Shininess").b), mini(2, bytes_of(32.f))}), false)});
            if (!s.texture.empty())
                mat = cat({mat, chunk(0x10105, cat({mini(1, W().str("BaseTexture").b), mini(2, W().str(s.texture.c_str()).b)}), false)});
            W si;
            si.put(static_cast<std::uint32_t>(s.verts.size())).put(static_cast<std::uint32_t>(s.idx.size() / 3)).pad_to(128);
            W vf;
            vf.str(s.format.c_str());
            W vb;
            for (const auto& v : s.verts) {
                for (float f : v.pos) vb.put(f);
                for (float f : v.nrm) vb.put(f);
                vb.put(v.uv[0]).put(v.uv[1]);
                for (int k = 0; k < 6; ++k) vb.put(0.f);           // uv1..3
                vb.put(zero_frame ? 0.f : 1.f).put(0.f).put(0.f);  // tangent
                vb.put(0.f).put(zero_frame ? 0.f : 1.f).put(0.f);  // binormal
                if (legacy) {
                    vb.put(1.f).put(0.5f).put(0.f).put(0.f);       // color (alpha 0 in legacy files)
                    vb.put(0u).put(v.bone).put(0u).put(0u);        // bone indices
                    vb.put(0.2f).put(0.8f).put(0.f).put(0.f);      // weights
                    continue;
                }
                vb.put(1.f).put(0.5f).put(0.25f).put(0.75f);       // color
                vb.put(1.f).put(1.f).put(1.f).put(1.f);            // unknown
                vb.put(v.bone).put(rskin_junk ? 0xDEADu : 0u).put(0u).put(0u);  // bone indices
                vb.put(rskin_junk ? 0.2f : 1.f).put(rskin_junk ? 0.8f : 0.f).put(0.f).put(0.f);  // weights
            }
            W ib;
            for (auto i : s.idx) ib.put(i);
            Bytes sub = cat({chunk(0x10001, si.b, false), chunk(0x10002, vf.b, false), chunk(legacy || chunk5_144 ? 0x10005u : 0x10007u, vb.b, false),
                             chunk(0x10004, ib.b, false)});
            if (!s.bone_map.empty()) {
                W bm;
                for (auto b : s.bone_map) bm.put(b);
                sub = cat({sub, chunk(0x10006, bm.b, false)});
            }
            payload = cat({payload, chunk(0x10100, mat, true), chunk(0x10000, sub, true)});
        }
        file = cat({file, chunk(0x400, payload, true)});
    }
    Bytes conn = chunk(0x601, cat({mini(1, bytes_of(static_cast<std::uint32_t>(meshes.size()))), mini(4, bytes_of(0u))}), false);
    for (std::uint32_t o = 0; o < meshes.size(); ++o)
        conn = cat({conn, chunk(0x602, cat({mini(2, bytes_of(o)), mini(3, bytes_of(meshes[o].bone))}), false)});
    return cat({file, chunk(0x600, conn, true)});
}

template <class V> V vertex_at(const Submesh& s, std::size_t i) {
    V v;
    std::memcpy(&v, s.vertices.data() + i * sizeof(V), sizeof(V));
    return v;
}

void test_static_model() {
    SynSub sub{"MeshGloss.fx", "alD3dVertNU2", {}, {0, 1, 2}, {}, "tex_a.tga"};
    sub.verts = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0}, {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0}, {{0, 2, 0}, {0, 0, 1}, {0, 1}, 0}};
    SynMesh mesh{"Hull", false, 1, {sub}};
    SynSub shadow{"MeshShadowVolume.fx", "alD3dVertN", {}, {0, 1, 2}, {}, ""};
    shadow.verts = sub.verts;
    SynMesh shmesh{"Shadow_LOD1", true, 0, {shadow}};
    const Bytes file = make_alo({{"Root", -1, 0, 0, 0}, {"B", 0, 5, 0, 0}}, {mesh, shmesh});

    ModelAsset m;
    std::string err;
    CHECK(build_model(file, "syn", m, &err));
    if (!err.empty()) std::printf("  build_model: %s\n", err.c_str());
    CHECK(m.skeleton.size() == 2 && m.skeleton.find("b") == 1 && m.skeleton.find("ROOT") == 0 && m.skeleton.find("x") == -1);
    CHECK(approx(m.skeleton.bind_world[1].m[0][3], 5));
    CHECK(approx(m.skeleton.inverse_bind_world[1].m[0][3], -5));
    CHECK(m.meshes.size() == 2 && m.submeshes.size() == 2);
    const Submesh& s = m.submeshes[0];
    CHECK(s.layout == VertexLayout::Static && s.vertex_count == 3 && s.vertices.size() == 3 * 64);
    CHECK(s.attach_bone == 1 && s.flags == 0 && s.is_render_geometry());
    CHECK(s.index_type == IndexType::U16 && s.index_count == 3 && s.indices.size() == 6);
    const auto v2 = vertex_at<StaticVertex>(s, 2);
    CHECK(approx(v2.position[1], 2) && approx(v2.normal[2], 1) && approx(v2.uv0[1], 1) && v2.color[3] == 255 && v2.pad == 0);
    CHECK(approx(s.bounds.max.x, 1) && approx(s.bounds.max.y, 2) && approx(s.sphere.radius, std::sqrt(0.25f + 1.f)));
    CHECK(s.material.effect == "MeshGloss.fx");
    const auto* d = s.material.find("diffuse");
    CHECK(d && d->type == ParamType::Float4 && approx(d->value.y, 0.5f));
    const auto* sh = s.material.find("Shininess");
    CHECK(sh && sh->type == ParamType::Float && approx(sh->value.x, 32));
    CHECK(m.texture_names() == std::vector<std::string>{"tex_a.tga"});
    // Model bounds: render geometry only, moved by the attach bone (+5 x); shadow volume excluded.
    CHECK(approx(m.bounds.min.x, 5) && approx(m.bounds.max.x, 6));
    const Submesh& sv = m.submeshes[1];
    CHECK((sv.flags & kSubmeshShadowVolume) && (sv.flags & kSubmeshHidden) && !sv.is_render_geometry());
    CHECK(m.meshes[1].lod == 1 && m.meshes[0].lod == -1 && m.meshes[1].hidden);
    // Attribute table matches the struct.
    const auto attrs = vertex_attributes(VertexLayout::Static);
    CHECK(attrs.size() == 6 && attrs[5].semantic == VertexSemantic::Color && attrs[5].offset == 56);
    // D.5: NU2 has no tangent frame -> T = B = 0 (the record's stored (1,0,0)/(0,1,0) are not part
    // of the format), nothing generated.
    CHECK(!s.has_tangents && v2.tangent[0] == 0.f && v2.binormal[1] == 0.f && v2.tangent[2] == 0.f);
    CHECK(s.effective_vertex_format == "alD3dVertNU2" && !s.vertex_type_overridden);
    // Debug option: the old generation (from UVs: dP/du = +x, dP/dv = +y here; alD3dVertN without
    // UVs: an orthonormal basis around the normal).
    {
        ModelLoadOptions gen;
        gen.generate_tangent_frames = true;
        ModelAsset mg;
        CHECK(build_model(file, "syn", mg, gen, &err) && mg.submeshes.size() == 2);
        const auto g2 = vertex_at<StaticVertex>(mg.submeshes[0], 2);
        CHECK(!mg.submeshes[0].has_tangents && approx(g2.tangent[0], 1) && approx(g2.binormal[1], 1) && approx(g2.tangent[2], 0));
        const auto w = vertex_at<StaticVertex>(mg.submeshes[1], 0);
        const float tl = w.tangent[0] * w.tangent[0] + w.tangent[1] * w.tangent[1] + w.tangent[2] * w.tangent[2];
        const float td = w.tangent[0] * w.normal[0] + w.tangent[1] * w.normal[1] + w.tangent[2] * w.normal[2];
        CHECK(approx(tl, 1, 1e-4f) && approx(td, 0, 1e-4f) && !mg.submeshes[1].has_tangents);
    }

    // Particle system / garbage are rejected without throwing.
    const Bytes particle = chunk(0x900, chunk(0x1, {0, 0, 0, 0}, false), true);
    CHECK(is_particle_alo(particle) && !build_model(particle, "p", m, &err));
    Bytes broken = file;
    broken.resize(broken.size() / 2);
    CHECK(!build_model(broken, "b", m, &err) && !err.empty());
    CHECK(model_path("ei_royalgaurd") == "DATA\\ART\\MODELS\\EI_ROYALGAURD.ALO");
    CHECK(model_path("Data/Art/Models/x.alo") == "DATA\\ART\\MODELS\\X.ALO");
}

void test_skinned_model() {
    SynSub sub{"RSkinBumpColorize.fx", "alD3dVertRSkinNU2U3U3", {}, {0, 1, 2}, {2, 1}, ""};
    sub.verts = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0}, {{1, 0, 0}, {0, 0, 1}, {1, 0}, 1}, {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0}};
    SynMesh mesh{"Body", false, 0, {sub}};
    const Bytes file = make_alo({{"Root", -1, 0, 0, 0}, {"Hip", 0, 0, 0, 1}, {"Arm", 1, 2, 0, 0}}, {mesh});
    ModelAsset m;
    std::string err;
    CHECK(build_model(file, "skin", m, &err));
    if (!err.empty()) std::printf("  build_model: %s\n", err.c_str());
    CHECK(m.submeshes.size() == 1);
    if (m.submeshes.empty()) return;
    const Submesh& s = m.submeshes[0];
    CHECK(s.skinned() && (s.flags & kSubmeshSkinned) && s.attach_bone == 0);
    CHECK((s.bone_palette == std::vector<std::uint16_t>{2, 1}));
    const auto v0 = vertex_at<SkinnedVertex>(s, 0), v1 = vertex_at<SkinnedVertex>(s, 1);
    CHECK(approx(v0.normal[3], 0) && approx(v1.normal[3], 1));
    CHECK(s.bone_palette[static_cast<std::size_t>(v0.normal[3])] == 2 && s.bone_palette[static_cast<std::size_t>(v1.normal[3])] == 1);
    CHECK(v0.color[0] == 255 && v0.color[3] == 255);
    CHECK(s.has_tangents && s.format.tangent_frame && s.format.skinned && approx(v0.tangent[0], 1) && approx(v0.binormal[1], 1));
    const auto sattrs = vertex_attributes(VertexLayout::Skinned);
    CHECK(sattrs.size() == 6 && sattrs[5].semantic == VertexSemantic::Color && sattrs[5].offset == 60);
    CHECK(approx(v1.normal[2], 1) && approx(v1.uv0[0], 1) && approx(v1.position[0], 1));
    CHECK(vertex_attributes(VertexLayout::Skinned)[1].format == AttribFormat::Float4);

    // Bind pose palette is identity; moving the Arm bone moves vertex 0 (bound to Arm) only.
    Pose pose = bind_pose(m.skeleton);
    std::vector<Mat3x4> world(m.skeleton.size()), pal(s.bone_palette.size());
    compute_world(m.skeleton, pose.local, world);
    compute_skin_palette(m.skeleton, world, s.bone_palette, pal);
    CHECK(approx(pal[0].m[0][3], 0) && approx(pal[0].m[0][0], 1) && approx(pal[1].m[2][3], 0));
    pose.local[2].m[1][3] = 3;  // Arm local +3 y
    compute_world(m.skeleton, pose.local, world);
    compute_skin_palette(m.skeleton, world, s.bone_palette, pal);
    const Float3 p = pal[static_cast<std::size_t>(v0.normal[3])].transform_point({v0.position[0], v0.position[1], v0.position[2]});
    CHECK(approx(p.x, 0) && approx(p.y, 3) && approx(p.z, 0));
    CHECK(approx(pal[1].m[1][3], 0));

    // Review: RSkin uses bone_index[0] only, even when another slot carries a heavier weight and an
    // index the parser never range-checked (0xDEAD here).
    {
        ModelAsset mj;
        CHECK(build_model(make_alo({{"Root", -1, 0, 0, 0}, {"Hip", 0, 0, 0, 1}, {"Arm", 1, 2, 0, 0}}, {mesh}, false, true),
                          "rskin_junk", mj, &err));
        CHECK(mj.submeshes.size() == 1);
        if (mj.submeshes.size() == 1) {
            const auto j0 = vertex_at<SkinnedVertex>(mj.submeshes[0], 0), j1 = vertex_at<SkinnedVertex>(mj.submeshes[0], 1);
            CHECK(approx(j0.normal[3], 0) && approx(j1.normal[3], 1));
            CHECK(mj.warnings.empty());  // no "blended weights reduced" warning for RSkin
        }
    }
}

void test_legacy_model() {
    SynSub sub{"RSkinBumpColorize.fx", "alD3dVertB4I4NU2U3U3", {}, {0, 1, 2}, {2, 1}, ""};
    sub.verts = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0}, {{1, 0, 0}, {0, 0, 1}, {1, 0}, 1}, {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0}};
    SynSub st{"MeshGloss.fx", "alD3dVertNU2C", {}, {0, 1, 2}, {}, ""};
    st.verts = sub.verts;
    const Bytes file = make_alo({{"Root", -1, 0, 0, 0}, {"Hip", 0, 0, 0, 1}, {"Arm", 1, 2, 0, 0}},
                                {SynMesh{"Body", false, 0, {sub}}, SynMesh{"Prop", false, 2, {st}}}, true);
    ModelAsset m;
    std::string err;
    CHECK(build_model(file, "legacy", m, &err));
    if (!err.empty()) std::printf("  build_model: %s\n", err.c_str());
    CHECK(m.legacy_vertex_chunks == 2 && m.submeshes.size() == 2);
    if (m.submeshes.size() != 2) return;
    const Submesh& s = m.submeshes[0];
    CHECK(s.skinned() && s.vertex_count == 3);
    const auto v1 = vertex_at<SkinnedVertex>(s, 1);
    // heaviest weight (0.8) is on bone_index[1] = 1 -> palette[1] = skeleton bone 1
    CHECK(approx(v1.normal[3], 1) && s.bone_palette[1] == 1 && approx(v1.position[0], 1) && approx(v1.uv0[0], 1));
    CHECK(!m.warnings.empty());
    // D.4: the legacy 128-byte converter overwrites the colour with 1.0 -> white, alpha 255, even
    // though NU2C has colour and the record stores (1, 0.5, 0, 0).
    const auto c0 = vertex_at<StaticVertex>(m.submeshes[1], 0);
    CHECK(c0.color[0] == 255 && c0.color[1] == 255 && c0.color[2] == 255 && c0.color[3] == 255 && m.submeshes[1].attach_bone == 2);
    // Legacy T/B are copied verbatim (NU2C has no frame -> zero; B4I4NU2U3U3 -> stored).
    CHECK(c0.tangent[0] == 0.f && approx(v1.tangent[0], 1) && approx(v1.binormal[1], 1));
    CHECK(m.submeshes[1].format.known && m.submeshes[1].format.color && !(m.submeshes[1].flags & kSubmeshUnknownFormat));
    // Skinned B4I4: colour not part of the format -> white.
    CHECK(v1.color[0] == 255 && v1.color[3] == 255 && s.format.known && s.format.tangent_frame);

    // alD3dVertNU2U3U3C: known to alamo_formats (tangent frame + colour), no assets-side inference.
    SynSub tc{"MeshBumpColorize.fx", "alD3dVertNU2U3U3C", {}, {0, 1, 2}, {}, ""};
    tc.verts = sub.verts;
    ModelAsset m2;
    CHECK(build_model(make_alo({{"Root", -1, 0, 0, 0}}, {SynMesh{"Hull", false, 0, {tc}}}), "nu2u3u3c", m2, &err));
    if (m2.submeshes.size() != 1) return;
    const Submesh& t = m2.submeshes[0];
    CHECK(t.format.known && t.format.color && t.format.tangent_frame && !t.format.skinned && t.has_tangents);
    CHECK(t.flags == 0 && t.layout == VertexLayout::Static && m2.legacy_vertex_chunks == 0);
    // 144-byte 0x10007 record with C: stored colour kept (1, 0.5, 0.25, 0.75).
    const auto tc0 = vertex_at<StaticVertex>(t, 0);
    CHECK(tc0.color[0] == 255 && tc0.color[1] == 128 && tc0.color[2] == 64 && tc0.color[3] == 191);

    // 0x10005 chunk with 144-byte records: not the 128-byte legacy converter (one predicate, stride
    // 128): colour kept and not counted in legacy_vertex_chunks.
    ModelAsset m3;
    CHECK(build_model(make_alo({{"Root", -1, 0, 0, 0}}, {SynMesh{"Hull", false, 0, {tc}}}, false, false, false, true), "c5_144",
                      m3, &err));
    if (m3.submeshes.size() != 1) return;
    CHECK(m3.legacy_vertex_chunks == 0);
    const auto c5 = vertex_at<StaticVertex>(m3.submeshes[0], 0);
    CHECK(c5.color[0] == 255 && c5.color[1] == 128 && c5.color[2] == 64 && c5.color[3] == 191);
}

// D.2: the effect's _ALAMO_VERTEX_TYPE replaces the ALO format (Planet.fx on alD3dVertN submeshes).
void test_vertex_type_override() {
    SynSub sub{"Planet.fx", "alD3dVertN", {}, {0, 1, 2}, {}, ""};
    sub.verts = {{{0, 0, 0}, {0, 0, 1}, {0.25f, 0.5f}, 0}, {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0}, {{0, 1, 0}, {0, 0, 1}, {0, 1}, 0}};
    SynSub other{"MeshGloss.fx", "alD3dVertNU2", {}, {0, 1, 2}, {}, ""};
    other.verts = sub.verts;
    const Bytes file = make_alo({{"Root", -1, 0, 0, 0}}, {SynMesh{"Hull", false, 0, {sub, other}}}, false, false, true);
    ModelLoadOptions opt;
    opt.vertex_type_override = [](const MaterialRef& mat) -> std::string {
        if (alamo::iequals(mat.effect, "Planet.fx")) return "alD3dVertNU2U3U3";
        if (alamo::iequals(mat.effect, "MeshGloss.fx")) return "alD3dVertNU2";  // same as the ALO: no override
        return {};
    };
    ModelAsset m, plain;
    std::string err;
    CHECK(build_model(file, "planet", m, opt, &err) && m.submeshes.size() == 2);
    CHECK(build_model(file, "planet", plain, &err) && plain.submeshes.size() == 2);
    if (m.submeshes.size() != 2 || plain.submeshes.size() != 2) return;
    const Submesh& s = m.submeshes[0];
    CHECK(s.vertex_format == "alD3dVertN" && s.effective_vertex_format == "alD3dVertNU2U3U3" && s.vertex_type_overridden);
    CHECK(s.format.tangent_frame && s.format.uv_sets == 1 && s.has_tangents && s.layout == VertexLayout::Static);
    const auto v0 = vertex_at<StaticVertex>(s, 0);
    // Stored UVs, T = B = 0 as stored, colour white (NU2U3U3 has no C).
    CHECK(approx(v0.uv0[0], 0.25f) && approx(v0.uv0[1], 0.5f) && v0.tangent[0] == 0.f && v0.binormal[1] == 0.f);
    CHECK(v0.color[0] == 255 && v0.color[3] == 255 && m.warnings.empty());
    CHECK(!m.submeshes[1].vertex_type_overridden && m.submeshes[1].effective_vertex_format == "alD3dVertNU2");
    CHECK(!plain.submeshes[0].vertex_type_overridden && !plain.submeshes[0].has_tangents && !plain.submeshes[0].format.tangent_frame);
    // Same bytes either way here (stored T/B are zero, colour white): only the traits differ.
    CHECK(s.vertices == plain.submeshes[0].vertices);
    // A name alamo_formats does not know is ignored with a warning.
    opt.vertex_type_override = [](const MaterialRef&) -> std::string { return "alD3dVertBogus"; };
    ModelAsset bad;
    CHECK(build_model(file, "bad", bad, opt, &err) && !bad.submeshes.empty() && !bad.submeshes[0].vertex_type_overridden &&
          bad.warnings.size() == 2);
}

// ---- synthetic ALA ----------------------------------------------------------------------------
Bytes make_ala(float fps = 30.f, float tx_offset = 1.f) {
    // 3 frames @ 30 fps, block layout, one bone "Arm": translation x = 1 + 2*u16 (0,1,2 -> 1,3,5),
    // rotation animated: identity, 90 deg about Z, identity. Visibility: frame 1 hidden.
    auto info = chunk(0x1001, cat({mini(1, bytes_of(3u)), mini(2, bytes_of(fps)), mini(3, bytes_of(1u)),
                                   mini(0xB, bytes_of(4u)), mini(0xC, bytes_of(3u)), mini(0xD, bytes_of(0u))}), false);
    W off, sc, one, zero;
    off.put(tx_offset).put(0.f).put(0.f);
    sc.put(2.f).put(0.f).put(0.f);
    one.put(1.f).put(1.f).put(1.f);
    zero.put(0.f).put(0.f).put(0.f);
    auto binfo = chunk(0x1003, cat({mini(4, W().str("Arm").b), mini(5, bytes_of(1u)), mini(0xA, bytes_of(0u)),
                                    mini(6, off.b), mini(7, sc.b), mini(8, one.b), mini(9, zero.b),
                                    mini(0xE, bytes_of(std::uint16_t(0))), mini(0xF, bytes_of(std::uint16_t(0xFFFF))),
                                    mini(0x10, bytes_of(std::uint16_t(0))), mini(0x11, Bytes(8, 0))}), false);
    const Bytes vis{0x05};  // frames 0 and 2 visible
    auto bone = chunk(0x1002, cat({binfo, chunk(0x1007, vis, false)}), true);
    W rot;
    const std::int16_t s45 = static_cast<std::int16_t>(std::lround(32767.0 * std::sqrt(0.5)));
    rot.put(std::int16_t(0)).put(std::int16_t(0)).put(std::int16_t(0)).put(std::int16_t(32767));
    rot.put(std::int16_t(0)).put(std::int16_t(0)).put(s45).put(s45);
    rot.put(std::int16_t(0)).put(std::int16_t(0)).put(std::int16_t(0)).put(std::int16_t(32767));
    W tr;
    for (std::uint16_t f = 0; f < 3; ++f) tr.put(f).put(std::uint16_t(0)).put(std::uint16_t(0));
    return chunk(0x1000, cat({info, bone, chunk(0x1009, rot.b, false), chunk(0x100A, tr.b, false)}), true);
}

void test_tangents() {
    // Zero normal, degenerate UVs, out-of-range index: every output must still be a finite unit basis.
    const std::vector<Float3> pos{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {5, 5, 5}};
    const std::vector<Float3> nrm{{0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}};
    const std::vector<Float2> uv{{0, 0}, {0, 0}, {0, 0}, {1, 1}};
    const std::vector<std::uint16_t> idx{0, 1, 2, 0, 1, 9};
    std::vector<Float3> t(4), b(4);
    compute_tangent_frame(pos, nrm, uv, idx, t, b);
    for (std::size_t i = 0; i < 4; ++i) {
        const float tl = t[i].x * t[i].x + t[i].y * t[i].y + t[i].z * t[i].z;
        const float bl = b[i].x * b[i].x + b[i].y * b[i].y + b[i].z * b[i].z;
        const float tb = t[i].x * b[i].x + t[i].y * b[i].y + t[i].z * b[i].z;
        CHECK(approx(tl, 1, 1e-4f) && approx(bl, 1, 1e-4f) && approx(tb, 0, 1e-4f));
    }
    // Mirrored UVs (v decreasing along +y) flip the binormal handedness.
    const std::vector<Float2> mir{{0, 1}, {1, 1}, {0, 0}, {0, 0}};
    compute_tangent_frame(pos, nrm, mir, idx, t, b);
    CHECK(approx(t[1].x, 1) && approx(b[1].y, -1));
    // Repair keeps valid authored vectors and replaces zero ones.
    std::vector<Float3> rt{{1, 0, 0}, {0, 0, 0}}, rb{{0, 1, 0}, {0, 0, 0}};
    const std::vector<Float3> rn{{0, 0, 1}, {0, 0, 1}};
    repair_tangent_frame(rn, rt, rb);
    CHECK(approx(rt[0].x, 1) && approx(rb[0].y, 1) && approx(rt[1].x * rt[1].x + rt[1].y * rt[1].y, 1, 1e-4f));
}

// Review finding 1: block layout, num_frames = 0, an animated translation index and no data must
// fail cleanly (error, no out-of-bounds read).
void test_animation_zero_frames() {
    auto info = chunk(0x1001, cat({mini(1, bytes_of(0u)), mini(2, bytes_of(30.f)), mini(3, bytes_of(1u)),
                                   mini(0xB, bytes_of(0u)), mini(0xC, bytes_of(3u)), mini(0xD, bytes_of(0u))}), false);
    auto binfo = chunk(0x1003, cat({mini(4, W().str("B").b), mini(5, bytes_of(1u)),
                                    mini(0xE, bytes_of(std::uint16_t(0))), mini(0x10, bytes_of(std::uint16_t(0xFFFF)))}), false);
    const Bytes file = chunk(0x1000, cat({info, chunk(0x1002, binfo, true)}), true);
    AnimationClip clip;
    std::string err;
    CHECK(!build_animation(file, "zero", clip, &err) && !err.empty());
    std::printf("  zero-frame ALA (%zu bytes) rejected: %s\n", file.size(), err.c_str());
}

void test_animation() {
    Skeleton sk;
    sk.names = {"Root", "Hip", "ARM"};
    sk.parents = {-1, 0, 1};
    sk.bind_local.assign(3, Mat3x4::identity());
    sk.bind_local[1].m[2][3] = 1;
    sk.finalize();

    AnimationClip clip;
    std::string err;
    CHECK(build_animation(make_ala(), "anim", clip, &err));
    if (!err.empty()) std::printf("  build_animation: %s\n", err.c_str());
    CHECK(clip.frame_count == 3 && approx(clip.fps, 30) && clip.tracks.size() == 1);
    if (clip.tracks.size() != 1) return;
    const auto& t = clip.tracks[0];
    CHECK(t.translation.size() == 3 && t.rotation.size() == 3 && t.scale.size() == 1 && t.visible.size() == 3);
    CHECK(approx(t.translation[2].x, 5) && approx(t.scale[0].x, 1));

    Pose pose;
    sample(sk, clip, 0.f, false, pose);
    CHECK(approx(pose.local[2].m[0][3], 1) && pose.visible[2] == 1 && approx(pose.local[1].m[2][3], 1));
    sample(sk, clip, 0.5f / 30.f, false, pose);  // halfway between frames 0 and 1
    CHECK(approx(pose.local[2].m[0][3], 2, 1e-4f));
    // rotation halfway to 90 deg about z = 45 deg: m[0][0] = cos 45
    CHECK(approx(pose.local[2].m[0][0], std::sqrt(0.5f), 1e-3f) && approx(pose.local[2].m[1][0], std::sqrt(0.5f), 1e-3f));
    sample(sk, clip, 1.001f / 30.f, false, pose);
    CHECK(pose.visible[2] == 0 && approx(pose.local[2].m[0][3], 3, 1e-2f) && approx(pose.local[2].m[1][0], 1, 1e-2f));
    sample(sk, clip, 10.f, false, pose);  // clamps to the last frame
    CHECK(approx(pose.local[2].m[0][3], 5, 1e-4f) && pose.visible[2] == 1);
    sample(sk, clip, 3.f / 30.f, true, pose);  // loop wraps to frame 0
    CHECK(approx(pose.local[2].m[0][3], 1, 1e-4f));
    sample(sk, clip, 2.5f / 30.f, true, pose);  // halfway from last frame back to frame 0
    CHECK(approx(pose.local[2].m[0][3], 3, 1e-4f));
    sample(sk, clip, std::nanf(""), true, pose);  // no UB on garbage time
    CHECK(approx(pose.local[2].m[0][3], 1, 1e-4f));

    std::vector<Mat3x4> world(sk.size());
    sample(sk, clip, 0.f, false, pose);
    compute_world(sk, pose.local, world);
    CHECK(approx(world[2].m[0][3], 1) && approx(world[2].m[2][3], 1));
    CHECK(!build_animation(Bytes{1, 2, 3}, "bad", clip, &err));

    // Review: non-finite header floats are rejected (fps = +inf used to give NaN poses and a
    // NaN -> uint32 conversion); a hand-built clip with a non-finite fps samples as 30 fps.
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(!build_animation(make_ala(inf), "inf_fps", clip, &err) && err.find("fps") != std::string::npos);
    CHECK(!build_animation(make_ala(std::nanf("")), "nan_fps", clip, &err));
    CHECK(!build_animation(make_ala(30.f, inf), "inf_offset", clip, &err) && err.find("non-finite") != std::string::npos);
    CHECK(build_animation(make_ala(-5.f), "neg_fps", clip, &err) && approx(clip.fps, 30));
    {
        AnimationClip odd;
        CHECK(build_animation(make_ala(), "odd", odd, &err));
        for (float f : {inf, -inf, std::nanf("")}) {
            odd.fps = f;
            sample(sk, odd, 1.f / 30.f, false, pose);
            CHECK(approx(pose.local[2].m[0][3], 3, 1e-4f));
            sample(sk, odd, 1.f / 30.f, true, pose);
            CHECK(approx(pose.local[2].m[0][3], 3, 1e-4f));
        }
        odd.fps = 30.f;
        sample(sk, odd, 3.0e38f, false, pose);  // time * fps overflows to +inf: clamps to the last frame
        CHECK(approx(pose.local[2].m[0][3], 5, 1e-4f));
        sample(sk, odd, 3.0e38f, true, pose);  // fmod(inf) = NaN: frame 0
        CHECK(approx(pose.local[2].m[0][3], 1, 1e-4f));
    }

    // Review: block-layout tracks are bones x frames, and any number of bones may share one block
    // element, so a small file can claim gigabytes of decoded keys. 2000 bones x 20000 frames
    // (translation + rotation animated) = ~1.1 GB from a ~0.4 MB file: rejected before allocating.
    {
        constexpr std::uint32_t frames = 20000, bones = 2000;
        Bytes parts = chunk(0x1001, cat({mini(1, bytes_of(frames)), mini(2, bytes_of(30.f)), mini(3, bytes_of(bones)),
                                         mini(0xB, bytes_of(4u)), mini(0xC, bytes_of(3u)), mini(0xD, bytes_of(0u))}),
                            false);
        for (std::uint32_t b = 0; b < bones; ++b) {
            const std::string nm = "B" + std::to_string(b);
            const Bytes binfo = chunk(0x1003, cat({mini(4, W().str(nm.c_str()).b), mini(5, bytes_of(b)),
                                                   mini(0xE, bytes_of(std::uint16_t(0))),
                                                   mini(0xF, bytes_of(std::uint16_t(0xFFFF))),
                                                   mini(0x10, bytes_of(std::uint16_t(0)))}), false);
            const Bytes bc = chunk(0x1002, binfo, true);
            parts.insert(parts.end(), bc.begin(), bc.end());
        }
        Bytes rot(std::size_t(frames) * 4 * 2, 0), tr(std::size_t(frames) * 3 * 2, 0);
        for (std::size_t f = 0; f < frames; ++f) rot[f * 8 + 7] = 0x7F;  // w = 32512: valid quaternion
        const Bytes file = chunk(0x1000, cat({parts, chunk(0x1009, rot, false), chunk(0x100A, tr, false)}), true);
        CHECK(!build_animation(file, "amplified", clip, &err) && err.find("MiB") != std::string::npos);
        std::printf("  amplified ALA (%zu bytes, %u bones x %u frames) rejected: %s\n", file.size(), bones, frames,
                    err.c_str());
    }
}

// ---- synthetic textures -------------------------------------------------------------------------
Bytes dds_header(std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t pf_flags, std::uint32_t fourcc,
                 std::uint32_t bits, std::uint32_t rm, std::uint32_t gm, std::uint32_t bm, std::uint32_t am,
                 std::uint32_t caps2 = 0) {
    W x;
    x.put(0x20534444u).put(124u).put(0x1007u | (mips > 1 ? 0x20000u : 0u)).put(h).put(w).put(0u).put(0u).put(mips);
    for (int i = 0; i < 11; ++i) x.put(0u);
    x.put(32u).put(pf_flags).put(fourcc).put(bits).put(rm).put(gm).put(bm).put(am);
    x.put(0x1000u).put(caps2).put(0u).put(0u).put(0u);
    return x.b;
}
constexpr std::uint32_t FCC(const char* s) {
    return std::uint32_t(std::uint8_t(s[0])) | (std::uint32_t(std::uint8_t(s[1])) << 8) |
           (std::uint32_t(std::uint8_t(s[2])) << 16) | (std::uint32_t(std::uint8_t(s[3])) << 24);
}

void test_dds() {
    std::string err;
    {  // DXT1 8x8, 4 mips: 32 + 8 + 8 + 8 bytes
        Bytes f = dds_header(8, 8, 4, 0x4, FCC("DXT1"), 0, 0, 0, 0, 0);
        for (int i = 0; i < 56; ++i) f.push_back(static_cast<std::uint8_t>(i));
        TextureAsset t;
        CHECK(decode_dds(f, t, &err));
        CHECK(t.format == TextureFormat::BC1_RGBA && t.mip_levels == 4 && t.levels.size() == 4 && t.width == 8);
        CHECK(t.levels[0].offset == 128 && t.levels[0].size == 32 && t.levels[1].offset == 160 && t.levels[3].size == 8);
        CHECK(t.level(0, 3) && t.level(0, 3)->width == 1 && t.level_bytes(*t.level(0, 1))[0] == 32);
        CHECK(std::string(texture_format_vk_name(t.format)) == "VK_FORMAT_BC1_RGBA_UNORM_BLOCK");
        // truncated: last mip missing -> 3 levels + warning
        Bytes tr(f.begin(), f.end() - 4);
        CHECK(decode_dds(tr, t, &err) && t.mip_levels == 3 && !t.warnings.empty());
        Bytes tiny(f.begin(), f.begin() + 140);
        CHECK(!decode_dds(tiny, t, &err));
    }
    {  // DXT5 cubemap 4x4, 1 mip: 6 x 16 bytes
        Bytes f = dds_header(4, 4, 1, 0x4, FCC("DXT5"), 0, 0, 0, 0, 0, 0x200u | 0xFC00u);
        for (int face = 0; face < 6; ++face)
            for (int i = 0; i < 16; ++i) f.push_back(static_cast<std::uint8_t>(face));
        TextureAsset t;
        CHECK(decode_dds(f, t, &err));
        CHECK(t.cubemap && t.layers == 6 && t.format == TextureFormat::BC3 && t.levels.size() == 6);
        CHECK(t.level(5, 0) && t.level_bytes(*t.level(5, 0))[0] == 5 && t.level(4, 0)->offset == 128 + 64);
        Bytes partial = dds_header(4, 4, 1, 0x4, FCC("DXT5"), 0, 0, 0, 0, 0, 0x200u | 0x400u);
        partial.resize(partial.size() + 16);
        CHECK(!decode_dds(partial, t, &err));
    }
    {  // A8R8G8B8 native BGRA8 and X8R8G8B8 with alpha swizzle
        Bytes f = dds_header(1, 1, 1, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u);
        f.insert(f.end(), {10, 20, 30, 40});
        TextureAsset t;
        CHECK(decode_dds(f, t, &err) && t.format == TextureFormat::BGRA8 && t.swizzle.a == Swizzle::A);
        CHECK(t.data.size() == 132 && t.level_bytes(t.levels[0])[2] == 30);
        Bytes x = dds_header(1, 1, 1, 0x40, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u);
        x.insert(x.end(), {10, 20, 30, 40});
        CHECK(decode_dds(x, t, &err) && t.format == TextureFormat::BGRA8 && t.swizzle.a == Swizzle::One);
    }
    {  // R5G6B5 -> RGBA8 conversion; 24-bit RGB
        Bytes f = dds_header(2, 1, 1, 0x40, 0, 16, 0xF800, 0x7E0, 0x1F, 0);
        W px;
        px.put(std::uint16_t(0xF800)).put(std::uint16_t(0x07E0));
        f.insert(f.end(), px.b.begin(), px.b.end());
        TextureAsset t;
        // single-level uncompressed DDS gets a generated mip: 2x1 + 1x1
        CHECK(decode_dds(f, t, &err) && t.format == TextureFormat::RGBA8 && t.data.size() == 12 && t.mip_levels == 2);
        CHECK(t.data[0] == 255 && t.data[1] == 0 && t.data[2] == 0 && t.data[3] == 255 && t.data[5] == 255);
        CHECK(t.levels.size() == 2 && t.levels[1].offset == 8 && t.data[8] == 128 && t.data[9] == 128 && t.data[10] == 0 &&
              t.data[11] == 255);
        Bytes g = dds_header(1, 1, 1, 0x40, 0, 24, 0xFF0000, 0xFF00, 0xFF, 0);
        g.insert(g.end(), {3, 2, 1});  // B G R in memory
        CHECK(decode_dds(g, t, &err) && t.format == TextureFormat::RGBA8 && t.data[0] == 1 && t.data[2] == 3 && t.data[3] == 255);
    }
    {  // L8, A8L8, A8 (flags), and L8 via legacy D3DFMT FourCC 50
        Bytes l = dds_header(2, 2, 1, 0x20000, 0, 8, 0xFF, 0, 0, 0);
        l.insert(l.end(), {1, 2, 3, 4});
        TextureAsset t;
        CHECK(decode_dds(l, t, &err) && t.format == TextureFormat::R8 && t.swizzle.g == Swizzle::R && t.swizzle.a == Swizzle::One);
        Bytes la = dds_header(1, 1, 1, 0x20001, 0, 16, 0xFF, 0, 0, 0xFF00);
        la.insert(la.end(), {7, 9});
        CHECK(decode_dds(la, t, &err) && t.format == TextureFormat::R8G8 && t.swizzle.a == Swizzle::G);
        Bytes a = dds_header(1, 1, 1, 0x2, 0, 8, 0, 0, 0, 0xFF);
        a.push_back(200);
        CHECK(decode_dds(a, t, &err) && t.format == TextureFormat::R8 && t.swizzle.a == Swizzle::R && t.swizzle.r == Swizzle::Zero);
        Bytes l50 = dds_header(1, 1, 1, 0x4, 50, 0, 0, 0, 0, 0);
        l50.push_back(5);
        CHECK(decode_dds(l50, t, &err) && t.format == TextureFormat::R8);
    }
    {
        TextureAsset t;
        CHECK(!decode_dds(Bytes(10, 0), t, &err));
        Bytes bad = dds_header(4, 4, 1, 0x4, FCC("ATI2"), 0, 0, 0, 0, 0);
        bad.resize(bad.size() + 16);
        CHECK(!decode_dds(bad, t, &err) && err.find("FourCC") != std::string::npos);
    }
    {  // Review: decoded-size budget. 8192^2 R3G3B2 (64 MiB file) would expand to 256 MiB RGBA8 +
       // 85 MiB of generated mips: refused before allocating. 64^2 of the same format decodes.
        Bytes big = dds_header(8192, 8192, 1, 0x40, 0, 8, 0xE0, 0x1C, 0x03, 0);
        big.resize(big.size() + std::size_t(8192) * 8192, 0xFF);
        TextureAsset t;
        CHECK(!decode_dds(std::move(big), t, &err) && err.find("budget") != std::string::npos);
        Bytes small = dds_header(64, 64, 1, 0x40, 0, 8, 0xE0, 0x1C, 0x03, 0);
        small.resize(small.size() + 64 * 64, 0xFF);
        CHECK(decode_dds(std::move(small), t, &err) && t.format == TextureFormat::RGBA8 && t.mip_levels == 7 &&
              t.data[0] == 255 && t.data[3] == 255);
    }
}

Bytes tga(std::uint8_t type, std::uint16_t w, std::uint16_t h, std::uint8_t bpp, std::uint8_t desc, const Bytes& px) {
    W x;
    x.put(std::uint8_t(0)).put(std::uint8_t(0)).put(type);
    x.put(std::uint16_t(0)).put(std::uint16_t(0)).put(std::uint8_t(0));
    x.put(std::uint16_t(0)).put(std::uint16_t(0)).put(w).put(h).put(bpp).put(desc);
    return cat({x.b, px});
}

void test_tga() {
    std::string err;
    TextureAsset t;
    // 24-bit, bottom-up: first stored row is the bottom row.
    CHECK(decode_tga(tga(2, 1, 2, 24, 0, {1, 2, 3, 4, 5, 6}), t, &err));
    CHECK(t.format == TextureFormat::BGRA8 && t.width == 1 && t.height == 2 && t.mip_levels == 2);
    CHECK(t.data.size() == 12 && t.data[0] == 4 && t.data[4] == 1 && t.data[3] == 255);
    CHECK(t.data[8] == 3 && t.data[9] == 4 && t.data[10] == 5 && t.data[11] == 255);  // (4+1+1)/2 = 3 ...
    // 32-bit RLE top-down with alpha bits: run of 3 + raw 1.
    CHECK(decode_tga(tga(10, 4, 1, 32, 0x28, {0x82, 1, 2, 3, 4, 0x00, 9, 8, 7, 6}), t, &err));
    CHECK(t.data.size() == 28 && t.data[8] == 1 && t.data[11] == 4 && t.data[12] == 9 && t.swizzle.a == Swizzle::A);
    CHECK(t.mip_levels == 3 && t.levels[1].width == 2 && t.levels[2].width == 1 && t.levels[2].offset == 24);
    // level 1: (1,2,3,4) and avg((1,2,3,4),(9,8,7,6)) = (5,5,5,5); level 2: avg = (3,4,4,5) rounded half up
    CHECK(t.data[16] == 1 && t.data[19] == 4 && t.data[20] == 5 && t.data[23] == 5);
    CHECK(t.data[24] == 3 && t.data[25] == 4 && t.data[26] == 4 && t.data[27] == 5);
    // grey 8
    CHECK(decode_tga(tga(3, 2, 1, 8, 0x20, {10, 20}), t, &err) && t.format == TextureFormat::R8 && t.data[1] == 20);
    // RLE overrun and truncated data are rejected
    CHECK(!decode_tga(tga(10, 2, 1, 32, 0, {0x85, 1, 2, 3, 4}), t, &err));
    CHECK(!decode_tga(tga(2, 2, 2, 24, 0, {1, 2, 3}), t, &err));
    CHECK(!decode_tga(tga(1, 2, 2, 8, 0, {}), t, &err));
    // Review finding 2: huge claims are rejected before any large allocation.
    CHECK(!decode_tga(tga(2, 16384, 16384, 32, 0, {}), t, &err) && err.find("truncated") != std::string::npos);
    CHECK(!decode_tga(tga(10, 16384, 16384, 32, 0, {0xFF, 1, 2, 3, 4}), t, &err) && err.find("budget") != std::string::npos);
    CHECK(!decode_tga(tga(10, 4096, 4096, 32, 0, {0xFF, 1, 2, 3, 4}), t, &err) && err.find("too short") != std::string::npos);
    // Review: decoded-size budget (256 MiB incl. mips). 8000^2 BGRA8 + mips = ~325 MiB is refused
    // even when the RLE data could back it (a 2.6 MB RLE file used to expand to ~850 MB).
    CHECK(!decode_tga(tga(10, 8000, 8000, 32, 0, {0xFF, 1, 2, 3, 4}), t, &err) && err.find("budget") != std::string::npos);
    CHECK(!decode_tga(tga(10, 8000, 8000, 8 * 3, 0, {0xFF, 1, 2, 3}), t, &err) && err.find("budget") != std::string::npos);
    // ... while an 8-bit grey image of the same size fits (61 MiB + mips): only "too short" here.
    CHECK(!decode_tga(tga(11, 8000, 8000, 8, 0, {0xFF, 1}), t, &err) && err.find("too short") != std::string::npos);
    // Review: 32 bpp TGA whose descriptor declares 0 alpha bits keeps its alpha (A8R8G8B8, as D3DX;
    // shipped MT_COMMANDBAR.TGA is like this).
    CHECK(decode_tga(tga(2, 2, 1, 32, 0x20, {1, 2, 3, 77, 5, 6, 7, 200}), t, &err));
    CHECK(t.format == TextureFormat::BGRA8 && t.swizzle.a == Swizzle::A && t.data[3] == 77 && t.data[7] == 200);
    CHECK(t.mip_levels == 2 && t.data[11] == (77 + 200 + 1) / 2);
    // 24 bpp has no alpha: opaque.
    CHECK(decode_tga(tga(2, 1, 1, 24, 0, {1, 2, 3}), t, &err) && t.data[3] == 255 && t.swizzle.a == Swizzle::A);
    {  // a legitimate run-only RLE image (2 packets of 128 pixels) still decodes
        Bytes px;
        for (int k = 0; k < 2; ++k) px.insert(px.end(), {0xFF, 9, 9, 9, 9});
        // 16x16 + 8x8 + 4x4 + 2x2 + 1x1 texels, 4 bytes each; a constant image stays constant
        CHECK(decode_tga(tga(10, 16, 16, 32, 0x28, px), t, &err) && t.data.size() == 1364 && t.data[1023] == 9);
        CHECK(t.mip_levels == 5 && t.data[1363] == 9 && t.levels[4].width == 1 && t.levels[4].offset == 1360);
    }
    // decode_texture sniffs DDS vs TGA
    CHECK(decode_texture(tga(3, 1, 1, 8, 0, {7}), t, &err) && t.data[0] == 7 && t.mip_levels == 1);
    // Lookup order (SPACE_AND_OPEN.md B.2): as named, Textures dir, .dds as named, .dds in Textures dir.
    const auto c = texture_candidates("Foo.tga");
    CHECK(c.size() == 4 && c[0] == "FOO.TGA" && c[1] == "DATA\\ART\\TEXTURES\\FOO.TGA" && c[2] == "FOO.DDS" &&
          c[3] == "DATA\\ART\\TEXTURES\\FOO.DDS");
    const auto lk = texture_lookup("art/sub/Bar.Tex.tga");  // dir kept as named; only the file part in step 2
    CHECK(lk.size() == 4 && lk[0].path == "ART\\SUB\\BAR.TEX.TGA" && lk[0].step == TextureLookupStep::AsNamed &&
          lk[1].path == "DATA\\ART\\TEXTURES\\BAR.TEX.TGA" && lk[1].step == TextureLookupStep::TextureDir &&
          lk[2].path == "ART\\SUB\\BAR.DDS" && lk[2].step == TextureLookupStep::DdsAsNamed &&  // cut at the first '.'
          lk[3].path == "DATA\\ART\\TEXTURES\\BAR.DDS" && lk[3].step == TextureLookupStep::DdsTextureDir);
    const auto ld = texture_lookup("x.dds");  // .dds name: steps 3/4 duplicate 1/2 and are dropped
    CHECK(ld.size() == 2 && ld[0].path == "X.DDS" && ld[1].path == "DATA\\ART\\TEXTURES\\X.DDS");
    const auto ln = texture_lookup("Data\\Art\\Textures\\noext");  // step 2 == step 1; .dds appended
    CHECK(ln.size() == 2 && ln[0].path == "DATA\\ART\\TEXTURES\\NOEXT" && ln[1].path == "DATA\\ART\\TEXTURES\\NOEXT.DDS" &&
          ln[1].step == TextureLookupStep::DdsAsNamed);
    CHECK(texture_lookup("").empty());
    // Fallbacks
    const auto fb = make_fallback_texture(TextureRole::Normal);
    CHECK(fb.is_fallback && fb.data.size() == 64 && fb.data[2] == 255 && fb.mip_levels == 1);
    const auto fc = make_fallback_texture(TextureRole::Color);
    CHECK(fc.is_fallback && fc.width == 256 && fc.height == 256 && fc.mip_levels == 9 && fc.levels.size() == 9 &&
          fc.format == TextureFormat::RGBA8 && fc.name == "<fallback>");
    CHECK(fc.data[0] == 255 && fc.data[1] == 0 && fc.data[2] == 255 && fc.data[3] == 255);    // (0,0) magenta
    CHECK(fc.data[32 * 4] == 0 && fc.data[32 * 4 + 2] == 0 && fc.data[32 * 4 + 3] == 255);    // (32,0) black
    {  // 4x4 level (mip 6): 64-texel squares of a 32-texel checker average to (128,0,128)
        const auto& l6 = *fc.level(0, 6);
        const auto b6 = fc.level_bytes(l6);
        CHECK(l6.width == 4 && b6[0] == 128 && b6[1] == 0 && b6[2] == 128 && b6[3] == 255);
        CHECK(fc.level(0, 5)->width == 8 && fc.level_bytes(*fc.level(0, 5))[0] == 255);  // 1 texel per square
    }
    CHECK(texture_role_for_param("NormalTexture") == TextureRole::Normal && texture_role_for_param("BaseTexture") == TextureRole::Color);
}

TextureAsset raw_texture(TextureFormat f, std::uint32_t w, std::uint32_t h, std::uint32_t layers, std::size_t bpp) {
    TextureAsset t;
    t.format = f;
    t.width = w;
    t.height = h;
    t.layers = layers;
    t.mip_levels = 1;
    t.data.resize(std::size_t(w) * h * bpp * layers);
    for (std::size_t i = 0; i < t.data.size(); ++i) t.data[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    for (std::uint32_t l = 0; l < layers; ++l)
        t.levels.push_back({l, 0, w, h, std::uint64_t(l) * w * h * bpp, std::uint64_t(w) * h * bpp});
    return t;
}

void test_mips() {
    {  // non-square odd R8: 5x3 -> 2x1 -> 1x1; last column/row dropped
        TextureAsset t = raw_texture(TextureFormat::R8, 5, 3, 1, 1);
        const Bytes base = t.data;
        CHECK(generate_mips(t) && t.mip_levels == 3 && t.levels.size() == 3);
        CHECK(t.levels[1].width == 2 && t.levels[1].height == 1 && t.levels[1].offset == 15 && t.levels[1].size == 2);
        CHECK(t.levels[2].width == 1 && t.levels[2].height == 1 && t.levels[2].offset == 17 && t.data.size() == 18);
        CHECK(std::equal(base.begin(), base.end(), t.data.begin()));
        for (std::uint32_t x = 0; x < 2; ++x) {
            const unsigned s = unsigned(base[2 * x]) + base[2 * x + 1] + base[5 + 2 * x] + base[5 + 2 * x + 1];
            CHECK(unsigned(t.data[15 + x]) == (s + 2) / 4);
        }
        CHECK(unsigned(t.data[17]) == (unsigned(t.data[15]) + t.data[16] + 1u) / 2);
        CHECK(!generate_mips(t));  // already has mips
    }
    {  // R8G8 2-layer 4x1 (layer-major levels; channels averaged independently)
        TextureAsset t = raw_texture(TextureFormat::R8G8, 4, 1, 2, 2);
        const Bytes base = t.data;
        CHECK(generate_mips(t) && t.mip_levels == 3 && t.levels.size() == 6);
        CHECK(t.level(1, 0)->offset == 14 && t.level(1, 1)->offset == 22 && t.level(1, 2)->offset == 26 && t.data.size() == 28);
        CHECK(unsigned(t.data[8]) == (unsigned(base[0]) + base[2] + 1u) / 2 &&
              unsigned(t.data[9]) == (unsigned(base[1]) + base[3] + 1u) / 2);
        CHECK(unsigned(t.data[22]) == (unsigned(base[8]) + base[10] + 1u) / 2);
    }
    {  // determinism: two runs give identical bytes; BC formats and 1x1 are left alone
        TextureAsset a = raw_texture(TextureFormat::BGRA8, 37, 19, 1, 4), b = raw_texture(TextureFormat::BGRA8, 37, 19, 1, 4);
        CHECK(generate_mips(a) && generate_mips(b) && a.data == b.data && a.mip_levels == 6);
        CHECK(a.levels.back().width == 1 && a.levels.back().height == 1 && a.levels[5].offset + 4 == a.data.size());
        TextureAsset bc;
        bc.format = TextureFormat::BC1_RGBA;
        bc.width = bc.height = 8;
        bc.mip_levels = 1;
        bc.data.resize(32);
        bc.levels.push_back({0, 0, 8, 8, 0, 32});
        CHECK(!generate_mips(bc) && bc.mip_levels == 1);
        TextureAsset one = raw_texture(TextureFormat::RGBA8, 1, 1, 1, 4);
        CHECK(!generate_mips(one) && one.mip_levels == 1);
    }
}

// ---- AssetCache -----------------------------------------------------------------------------
class MemProvider final : public alamo::FileProvider {
public:
    void add(const std::string& path, Bytes data) { files_[alamo::normalize_path(path)] = std::move(data); }
    bool read(std::string_view path, std::vector<std::uint8_t>& out) const override {
        reads_.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // widen the race window
        auto it = files_.find(alamo::normalize_path(path));
        if (it == files_.end()) {
            out.clear();
            return false;
        }
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(alamo::normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem" : ""; }
    int reads() const { return reads_.load(); }

private:
    std::map<std::string, Bytes> files_;
    mutable std::atomic<int> reads_{0};
};

void test_cache() {
    MemProvider mp;
    SynSub sub{"MeshGloss.fx", "alD3dVertNU2", {}, {0, 1, 2}, {}, "tex_a.tga"};
    sub.verts = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, 0}, {{1, 0, 0}, {0, 0, 1}, {1, 0}, 0}, {{0, 2, 0}, {0, 0, 1}, {0, 1}, 0}};
    mp.add("Data\\Art\\Models\\Ship.alo", make_alo({{"Root", -1, 0, 0, 0}}, {SynMesh{"Hull", false, 0, {sub}}}));
    mp.add("Data\\Art\\Textures\\tex_a.dds", tga(3, 1, 1, 8, 0, {7}));  // .tga name resolves to the .dds file
    mp.add("Data\\Art\\Models\\Ship_Idle.ala", make_ala());

    AssetCache cache(mp);
    std::vector<std::shared_ptr<const ModelAsset>> got(16);
    std::vector<std::shared_ptr<const TextureAsset>> tex(16), miss(16);
    std::vector<std::thread> th;
    for (int i = 0; i < 16; ++i) {
        th.emplace_back([&, i] {
            got[static_cast<std::size_t>(i)] = cache.model(i % 2 ? "SHIP" : "data/art/models/ship.ALO");
            tex[static_cast<std::size_t>(i)] = cache.texture(i % 2 ? "TEX_A.TGA" : "tex_a.tga");
            miss[static_cast<std::size_t>(i)] = cache.texture("nothere.dds", TextureRole::Normal);
        });
    }
    for (auto& t : th) t.join();
    for (int i = 0; i < 16; ++i) {
        CHECK(got[static_cast<std::size_t>(i)] && got[static_cast<std::size_t>(i)] == got[0]);
        CHECK(tex[static_cast<std::size_t>(i)] == tex[0] && !tex[0]->is_fallback);
        CHECK(miss[static_cast<std::size_t>(i)] && miss[static_cast<std::size_t>(i)]->is_fallback);
    }
    CHECK(tex[0]->name == "DATA\\ART\\TEXTURES\\TEX_A.DDS");
    // one model read; textures resolve with exists() and only the resolved file is read: one read
    // for TEX_A, none for the missing name
    CHECK(mp.reads() == 1 + 1 + 0);
    CHECK(tex[0]->mip_levels == 1);  // 1x1 grey TGA: nothing to generate
    CHECK(miss[0] == cache.texture("NOTHERE.DDS", TextureRole::Normal) && miss[0]->width == 4);  // cached miss
    CHECK(cache.texture("nothere.dds")->width == 256 && cache.texture("nothere.dds")->is_fallback);  // Color: 256x256
    CHECK(cache.animation("ship_idle") && cache.animation("SHIP_IDLE") == cache.animation("ship_idle"));
    CHECK(!cache.model("missing") && !cache.animation("missing_anim"));
    const auto f = cache.failures();
    CHECK(f.size() == 3 && f[0].kind == "animation" && f[1].kind == "model" && f[2].kind == "texture" &&
          f[2].key == "NOTHERE.DDS");
    const auto st = cache.stats();
    CHECK(st.models == 2 && st.textures == 2 && st.animations == 2 && st.failed == 3 && st.texture_files == 1);

    // Review: names that resolve to the same file share one decode (W_FUEL_POD.TGA / .DDS /
    // Data\Art\Textures\W_FUEL_POD.tga -> Data\Art\Textures\W_FUEL_POD.DDS), whatever the request order
    // or thread timing. Names resolving to different files stay separate.
    for (int order = 0; order < 2; ++order) {
        MemProvider fp;
        fp.add("Data\\Art\\Textures\\W_FUEL_POD.dds", tga(3, 1, 1, 8, 0, {9}));
        fp.add("Data\\Art\\Textures\\Both.tga", tga(3, 1, 1, 8, 0, {1}));
        fp.add("Data\\Art\\Textures\\Both.dds", tga(3, 1, 1, 8, 0, {2}));
        AssetCache c2(fp);
        const char* names[] = {"W_FUEL_POD.TGA", "w_fuel_pod.dds", "Data\\Art\\Textures\\W_FUEL_POD.tga", "data/art/textures/w_fuel_pod.DDS"};
        std::vector<std::shared_ptr<const TextureAsset>> got2(16);
        std::vector<std::thread> th2;
        for (int i = 0; i < 16; ++i)
            th2.emplace_back([&, i] {
                got2[static_cast<std::size_t>(i)] = c2.texture(names[(i + order) % 4]);
            });
        for (auto& t : th2) t.join();
        for (const auto& g : got2) CHECK(g == got2[0] && !g->is_fallback && g->data[0] == 9);
        CHECK(got2[0]->name == "DATA\\ART\\TEXTURES\\W_FUEL_POD.DDS");
        const auto both_tga = c2.texture("both.tga"), both_dds = c2.texture("both.dds");
        CHECK(both_tga != both_dds && both_tga->data[0] == 1 && both_dds->data[0] == 2);
        const auto s2 = c2.stats();
        CHECK(s2.textures == 6 && s2.texture_files == 3 && s2.failed == 0);
        CHECK(fp.reads() == 3);  // one read per distinct file
    }
}

// Each B.2 lookup step in isolation, precedence between steps, and the fallback.
void test_lookup_steps() {
    const Bytes img = tga(3, 2, 2, 8, 0x20, {10, 20, 30, 40});
    struct File {
        std::string path;
        std::uint8_t grey;  // first texel; tells the files apart
    };
    struct Case {
        const char* name;
        std::vector<File> files;
        bool found;
        TextureLookupStep step;
        const char* path;
    };
    using S = TextureLookupStep;
    const Case cases[] = {
        {"foo.tga", {{"foo.tga", 1}}, true, S::AsNamed, "FOO.TGA"},
        {"foo.tga", {{"data/art/textures/Foo.TGA", 2}}, true, S::TextureDir, "DATA\\ART\\TEXTURES\\FOO.TGA"},
        {"foo.tga", {{"FOO.dds", 3}}, true, S::DdsAsNamed, "FOO.DDS"},
        {"foo.tga", {{"Data\\Art\\Textures\\foo.dds", 4}}, true, S::DdsTextureDir, "DATA\\ART\\TEXTURES\\FOO.DDS"},
        // precedence: each earlier step beats every later one
        {"foo.tga", {{"Data\\Art\\Textures\\foo.dds", 4}, {"foo.dds", 3}, {"Data\\Art\\Textures\\foo.tga", 2}, {"foo.tga", 1}},
         true, S::AsNamed, "FOO.TGA"},
        {"foo.tga", {{"Data\\Art\\Textures\\foo.dds", 4}, {"foo.dds", 3}, {"Data\\Art\\Textures\\foo.tga", 2}},
         true, S::TextureDir, "DATA\\ART\\TEXTURES\\FOO.TGA"},
        {"foo.tga", {{"Data\\Art\\Textures\\foo.dds", 4}, {"foo.dds", 3}}, true, S::DdsAsNamed, "FOO.DDS"},
        // a directory in the name: step 2 uses only the file part
        {"Mod\\Tex\\bar.tga", {{"Data\\Art\\Textures\\bar.tga", 5}}, true, S::TextureDir, "DATA\\ART\\TEXTURES\\BAR.TGA"},
        {"Mod\\Tex\\bar.tga", {{"Mod\\Tex\\bar.dds", 6}}, true, S::DdsAsNamed, "MOD\\TEX\\BAR.DDS"},
        // only the named extension and .dds are tried: a .dds name never finds a .tga
        {"baz.dds", {{"Data\\Art\\Textures\\baz.tga", 7}}, false, S::AsNamed, ""},
        {"foo.tga", {{"Data\\Art\\Textures\\foo.png", 8}}, false, S::AsNamed, ""},
    };
    for (const auto& c : cases) {
        MemProvider mp;
        for (const auto& f : c.files) {
            Bytes b = img;
            b[18] = f.grey;
            mp.add(f.path, b);
        }
        TextureAsset t;
        std::string err, path;
        TextureLookupStep step = S::DdsTextureDir;
        const bool ok = load_texture(mp, c.name, t, &err, &path, &step);
        CHECK(ok == c.found);
        if (ok) {
            CHECK(step == c.step && path == c.path && t.name == c.path);
            CHECK(t.mip_levels == 2 && t.data.size() == 5 && unsigned(t.data[4]) == (unsigned(t.data[0]) + 20u + 30u + 40u + 2u) / 4);
            std::uint8_t expect = 0;
            for (const auto& f : c.files)
                if (alamo::normalize_path(f.path) == c.path) expect = f.grey;
            CHECK(t.data[0] == expect);
        } else {
            CHECK(err.rfind("not found", 0) == 0);
            AssetCache cache(mp);
            const auto fb = cache.texture(c.name);
            CHECK(fb->is_fallback && fb->width == 256 && fb == cache.texture(c.name));
            CHECK(cache.failures().size() == 1);
        }
    }
    {  // the first existing candidate decides: a broken step-1 file fails even if step 2 is fine
        MemProvider mp;
        mp.add("foo.tga", Bytes{1, 2, 3});
        mp.add("Data\\Art\\Textures\\foo.tga", img);
        TextureAsset t;
        std::string err;
        CHECK(!load_texture(mp, "foo.tga", t, &err) && err.find("FOO.TGA") != std::string::npos &&
              err.rfind("not found", 0) != 0);
        AssetCache cache(mp);
        CHECK(cache.texture("foo.tga")->is_fallback);
    }
}

// ---- real data --------------------------------------------------------------------------------
#if defined(ALAMO_ASSETS_HAVE_VFS)

template <class F>
void parallel_for(std::size_t n, F&& fn) {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> th;
    for (unsigned t = 0; t < hw; ++t)
        th.emplace_back([&] {
            for (std::size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) fn(i);
        });
    for (auto& t : th) t.join();
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// alo2gltf's default selection (tools/formats/tools/alo2gltf.cpp): skip hidden meshes and
// shadow/collision effects, keep only the most detailed _LOD<n> mesh per base name.
std::vector<bool> gltf_lod_keep(const ModelAsset& m) {
    std::vector<bool> keep(m.meshes.size(), true);
    std::map<std::string, std::pair<std::size_t, std::size_t>> best;
    for (std::size_t i = 0; i < m.meshes.size(); ++i) {
        if (m.meshes[i].lod < 0) continue;
        std::string base = alamo::normalize_name(m.meshes[i].name);
        base = base.substr(0, base.rfind("_LOD"));
        std::size_t tris = 0;
        for (std::uint32_t s = 0; s < m.meshes[i].submesh_count; ++s)
            tris += m.submeshes[m.meshes[i].first_submesh + s].index_count / 3;
        auto it = best.find(base);
        if (it == best.end()) {
            best[base] = {tris, i};
        } else {
            keep[i] = false;
            if (tris > it->second.first) {
                keep[it->second.second] = false;
                keep[i] = true;
                it->second = {tris, i};
            }
        }
    }
    return keep;
}

void check_gltf(const alamo::FileProvider& fs, const std::string& name) {
    const std::string dir = ALAMO_ASSETS_EXAMPLES_DIR;
    std::string js, bin;
    if (!read_text(dir + "/" + name + ".gltf", js) || !read_text(dir + "/" + name + ".bin", bin)) {
        std::printf("  glTF %s: example files missing, skipped\n", name.c_str());
        return;
    }
    ModelAsset m;
    std::string err;
    if (!load_model(fs, name, m, &err)) {
        std::printf("FAIL glTF %s: model load failed: %s\n", name.c_str(), err.c_str());
        ++g_fail;
        return;
    }
    const auto doc = mini_json::parse(js);
    const auto& acc = doc["accessors"];
    const auto& views = doc["bufferViews"];
    auto acc_ptr = [&](std::size_t a) -> const char* {
        const auto& v = views[acc[a]["bufferView"].as_size()];
        return bin.data() + v["byteOffset"].as_size();
    };
    // Expected order: per kept mesh, its static prims then its skinned prims (one glTF mesh each).
    const auto keep = gltf_lod_keep(m);
    std::vector<const Submesh*> expect;
    for (std::size_t mi = 0; mi < m.meshes.size(); ++mi) {
        if (!keep[mi] || m.meshes[mi].hidden) continue;
        for (int pass = 0; pass < 2; ++pass)
            for (std::uint32_t s = 0; s < m.meshes[mi].submesh_count; ++s) {
                const Submesh& sm = m.submeshes[m.meshes[mi].first_submesh + s];
                if ((sm.flags & (kSubmeshShadowVolume | kSubmeshCollisionShader)) || sm.index_count == 0) continue;
                if (sm.skinned() == (pass == 1)) expect.push_back(&sm);
            }
    }
    std::vector<const mini_json::Value*> prims;
    for (const auto& gm : doc["meshes"].arr)
        for (const auto& p : gm["primitives"].arr) prims.push_back(&p);
    std::size_t verts = 0, mism_pos = 0, mism_bone = 0, mism_idx = 0, skinned_verts = 0;
    CHECK(prims.size() == expect.size());
    for (std::size_t i = 0; i < std::min(prims.size(), expect.size()); ++i) {
        const auto& attrs = (*prims[i])["attributes"];
        const Submesh& sm = *expect[i];
        const std::size_t pa = attrs["POSITION"].as_size();
        const std::size_t n = acc[pa]["count"].as_size();
        if (n != sm.vertex_count) {
            std::printf("FAIL glTF %s prim %zu: %zu verts vs %u\n", name.c_str(), i, n, sm.vertex_count);
            ++g_fail;
            continue;
        }
        verts += n;
        const char* pp = acc_ptr(pa);
        const auto* ja = attrs.get("JOINTS_0");
        CHECK((ja != nullptr) == sm.skinned());
        for (std::size_t v = 0; v < n; ++v) {
            float gp[3];
            std::memcpy(gp, pp + v * 12, 12);
            float mp[3];
            if (sm.skinned()) {
                const auto sv = vertex_at<SkinnedVertex>(sm, v);
                std::memcpy(mp, sv.position, 12);
                if (ja) {
                    std::uint16_t j[4];
                    std::memcpy(j, acc_ptr(ja->as_size()) + v * 8, 8);
                    const auto local = static_cast<std::size_t>(sv.normal[3]);
                    if (local >= sm.bone_palette.size() || sm.bone_palette[local] != j[0]) ++mism_bone;
                }
                ++skinned_verts;
            } else {
                const auto st = vertex_at<StaticVertex>(sm, v);
                std::memcpy(mp, st.position, 12);
            }
            if (std::memcmp(gp, mp, 12) != 0) ++mism_pos;
        }
        const std::size_t ia = (*prims[i])["indices"].as_size();
        const std::size_t ni = acc[ia]["count"].as_size();
        if (ni != sm.index_count || std::memcmp(acc_ptr(ia), sm.indices.data(), ni * 2) != 0) ++mism_idx;
    }
    std::printf("  glTF %s: %zu prims, %zu verts (%zu skinned): position mismatches %zu, bone mismatches %zu, "
                "index-buffer mismatches %zu\n",
                name.c_str(), prims.size(), verts, skinned_verts, mism_pos, mism_bone, mism_idx);
    CHECK(mism_pos == 0 && mism_bone == 0 && mism_idx == 0);
}

void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    alamo::vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto vfs = alamo::vfs::Vfs::create(cfg, &err);
    if (!vfs) {
        std::printf("FAIL real data: VFS: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    alamo::log::set_min_level(alamo::log::Level::Error);  // missing textures are counted below

    const auto t0 = std::chrono::steady_clock::now();
    const auto alos = vfs->list("Data\\Art\\Models\\*.alo");
    AssetCache cache(*vfs);
    std::atomic<std::size_t> particles{0}, models{0}, submeshes{0}, skinned{0}, verts{0}, big_palette{0};
    std::mutex mu;
    std::vector<std::string> fails;
    parallel_for(alos.size(), [&](std::size_t i) {
        Bytes bytes;
        if (!vfs->read(alos[i], bytes)) {
            std::lock_guard<std::mutex> l(mu);
            fails.push_back(alos[i] + ": unreadable");
            return;
        }
        if (is_particle_alo(bytes)) {
            ++particles;
            return;
        }
        auto m = cache.model(alos[i]);
        if (!m) return;  // recorded in cache.failures()
        ++models;
        for (const auto& s : m->submeshes) {
            ++submeshes;
            verts += s.vertex_count;
            if (s.skinned()) ++skinned;
            if (s.bone_palette.size() > 24) ++big_palette;
        }
    });
    for (const auto& f : cache.failures()) fails.push_back(f.key + ": " + f.reason);
    std::sort(fails.begin(), fails.end());
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("real data: %zu ALO listed: %zu models OK, %zu particle systems, %zu failed (%.1f s, %u threads)\n",
                alos.size(), models.load(), particles.load(), fails.size(), secs,
                std::max(2u, std::thread::hardware_concurrency()));
    std::printf("  %zu submeshes (%zu skinned, %zu with >24-bone palettes), %zu vertices\n", submeshes.load(),
                skinned.load(), big_palette.load(), verts.load());
    for (std::size_t i = 0; i < std::min<std::size_t>(fails.size(), 20); ++i) std::printf("  model fail: %s\n", fails[i].c_str());
    CHECK(fails.empty());
    CHECK(models.load() > 1000);

    // Vertex-format names, tangent-frame validity, and generated-vs-authored tangent agreement.
    {
        std::map<std::string, std::size_t> formats;
        std::size_t nonfinite = 0, nonframe_nonzero = 0, bad_frames = 0, authored = 0, agree_t = 0, agree_b = 0, compared = 0, right_handed = 0, left_handed = 0;
        for (const auto& a : alos) {
            auto m = cache.model(a);
            if (!m) continue;
            for (const auto& s : m->submeshes) {
                ++formats[s.vertex_format];
                const std::size_t n = s.vertex_count;
                std::vector<Float3> pos(n), nrm(n), tn(n), bn(n), gt(n), gb(n);
                std::vector<Float2> uv(n);
                for (std::size_t v = 0; v < n; ++v) {
                    float p[3], q[3], t3[3], b3[3], u[2];
                    if (s.skinned()) {
                        const auto x = vertex_at<SkinnedVertex>(s, v);
                        std::memcpy(p, x.position, 12); std::memcpy(q, x.normal, 12); std::memcpy(t3, x.tangent, 12);
                        std::memcpy(b3, x.binormal, 12); std::memcpy(u, x.uv0, 8);
                    } else {
                        const auto x = vertex_at<StaticVertex>(s, v);
                        std::memcpy(p, x.position, 12); std::memcpy(q, x.normal, 12); std::memcpy(t3, x.tangent, 12);
                        std::memcpy(b3, x.binormal, 12); std::memcpy(u, x.uv0, 8);
                    }
                    pos[v] = {p[0], p[1], p[2]};
                    nrm[v] = {q[0], q[1], q[2]};
                    tn[v] = {t3[0], t3[1], t3[2]};
                    bn[v] = {b3[0], b3[1], b3[2]};
                    uv[v] = {u[0], u[1]};
                    const float tl = t3[0] * t3[0] + t3[1] * t3[1] + t3[2] * t3[2];
                    const float bl = b3[0] * b3[0] + b3[1] * b3[1] + b3[2] * b3[2];
                    if (!std::isfinite(tl) || !std::isfinite(bl)) ++nonfinite;
                    else if (!s.has_tangents && (tl != 0.f || bl != 0.f)) ++nonframe_nonzero;
                    else if (s.has_tangents && (!(tl > 1e-6f) || !(bl > 1e-6f))) ++bad_frames;
                }
                if (!s.has_tangents || !s.is_render_geometry() || s.format.uv_sets == 0) continue;
                if (s.index_type != IndexType::U16 || s.index_count == 0) continue;
                ++authored;
                std::vector<std::uint16_t> idx(s.index_count);
                std::memcpy(idx.data(), s.indices.data(), idx.size() * 2);
                compute_tangent_frame(pos, nrm, uv, idx, gt, gb);
                for (std::size_t v = 0; v < n; ++v) {
                    const float tl = std::sqrt(tn[v].x * tn[v].x + tn[v].y * tn[v].y + tn[v].z * tn[v].z);
                    const float bl = std::sqrt(bn[v].x * bn[v].x + bn[v].y * bn[v].y + bn[v].z * bn[v].z);
                    if (!(tl > 1e-6f) || !(bl > 1e-6f)) continue;
                    ++compared;
                    if ((gt[v].x * tn[v].x + gt[v].y * tn[v].y + gt[v].z * tn[v].z) / tl > 0.7f) ++agree_t;
                    if ((gb[v].x * bn[v].x + gb[v].y * bn[v].y + gb[v].z * bn[v].z) / bl > 0.7f) ++agree_b;
                    const Float3 nc{nrm[v].y * tn[v].z - nrm[v].z * tn[v].y, nrm[v].z * tn[v].x - nrm[v].x * tn[v].z,
                                    nrm[v].x * tn[v].y - nrm[v].y * tn[v].x};
                    const float d = (nc.x * bn[v].x + nc.y * bn[v].y + nc.z * bn[v].z) / bl / tl;
                    if (d > 0.7f) ++right_handed;
                    else if (d < -0.7f) ++left_handed;
                }
            }
        }
        std::printf("  vertex formats seen:");
        for (const auto& [name, cnt] : formats) std::printf(" %s=%zu", name.c_str(), cnt);
        std::printf("\n  tangent frames (as stored, D.5): %zu non-finite, %zu non-zero without a frame, %zu frame-format "
                    "vertices with a zero T or B; %zu authored render submeshes: generator agrees "
                    "with authored tangent on %.1f%% and binormal on %.1f%% of %zu vertices (dot > 0.7)\n",
                    nonfinite, nonframe_nonzero, bad_frames, authored,
                    compared ? 100.0 * double(agree_t) / double(compared) : 0.0,
                    compared ? 100.0 * double(agree_b) / double(compared) : 0.0, compared);
        std::printf("  authored binormal vs normal x tangent: %zu same sign, %zu opposite, %zu neither\n", right_handed,
                    left_handed, compared - right_handed - left_handed);
        CHECK(nonfinite == 0 && nonframe_nonzero == 0);
    }

    // alamo_formats directly on every model ALO, and a digest of the GPU-ready output (vertex and
    // index bytes, layout, flags without kSubmeshUnknownFormat, palette, warnings) split into models
    // with and without 128-byte 0x10005 vertex chunks, so loader changes can be checked byte-for-byte.
    {
        std::size_t fmt_ok = 0, fmt_fail = 0, legacy_models = 0;
        std::uint64_t dig_plain = 14695981039346656037ull, dig_legacy = dig_plain;
        auto mix = [](std::uint64_t& h, const void* p, std::size_t n) {
            h = alamo::fnv1a64(p, n, h);
        };
        std::vector<std::string> fmt_fails;
        std::map<std::string, std::array<std::size_t, 4>> legacy_stats;  // submeshes, verts, colour 0000, colour 1111
        std::size_t b4i4_subs = 0, b4i4_verts = 0, b4i4_inexact = 0, b4i4_blended = 0;
        std::size_t d5_nonframe_subs = 0, d5_repaired_subs = 0, d5_repaired_verts = 0, d4_subs = 0, d4_verts = 0;
        std::set<std::string> d5_nonframe_models, d5_repaired_models, d4_models;
        for (const auto& a : alos) {
            Bytes bytes;
            if (!vfs->read(a, bytes) || is_particle_alo(bytes)) continue;
            bool legacy = false;
            try {
                const auto f = alamo::alo::parse(bytes);
                ++fmt_ok;
                for (const auto& mesh : f.model.meshes)
                    for (const auto& sm : mesh.submeshes) {
                        if (sm.format.bone_influences == 4) {
                            ++b4i4_subs;
                            for (const auto& w : sm.vertices.bone_weight) {
                                ++b4i4_verts;
                                if (!(w.x == 1.f && w.y == 0.f && w.z == 0.f && w.w == 0.f)) ++b4i4_inexact;
                                if (std::max({w.x, w.y, w.z, w.w}) < 0.999f) ++b4i4_blended;
                            }
                        }
                        // What the D.4/D.5 rules change against the former loader (repair/generate
                        // frames, legacy colour as stored).
                        if (!sm.format.tangent_frame) {
                            ++d5_nonframe_subs;
                            d5_nonframe_models.insert(a);
                        } else {
                            std::size_t z = 0;
                            for (std::size_t v = 0; v < sm.vertices.size(); ++v) {
                                const auto& t = sm.vertices.tangent[v];
                                const auto& b = sm.vertices.binormal[v];
                                const float tl = t.x * t.x + t.y * t.y + t.z * t.z, bl = b.x * b.x + b.y * b.y + b.z * b.z;
                                if (!(tl > 1e-12f) || !(bl > 1e-12f) || !std::isfinite(tl) || !std::isfinite(bl)) ++z;
                            }
                            if (z) {
                                ++d5_repaired_subs;
                                d5_repaired_verts += z;
                                d5_repaired_models.insert(a);
                            }
                        }
                        if (sm.vertex_stride == 128 && sm.format.color) {
                            std::size_t changed = 0;
                            for (const auto& c : sm.vertices.color)
                                if (!(c.x >= 1.f && c.y >= 1.f && c.z >= 1.f && c.w >= 1.f)) ++changed;
                            ++d4_subs;
                            d4_verts += changed;
                            d4_models.insert(a + "(" + sm.vertex_format + ", " + sm.material.shader + ")");
                        }
                        if (sm.vertex_stride != 128) continue;
                        legacy = true;
                        std::size_t zero = 0, white = 0;
                        for (const auto& c : sm.vertices.color) {
                            if (c.x == 0.f && c.y == 0.f && c.z == 0.f && c.w == 0.f) ++zero;
                            if (c.x == 1.f && c.y == 1.f && c.z == 1.f && c.w == 1.f) ++white;
                        }
                        auto& st = legacy_stats[sm.vertex_format];
                        st[0] += 1;
                        st[1] += sm.vertices.size();
                        st[2] += zero;
                        st[3] += white;
                    }
            } catch (const std::exception& e) {
                ++fmt_fail;
                legacy = true;
                fmt_fails.push_back(a + ": " + e.what());
            }
            auto m = cache.model(a);
            if (!m) continue;
            if (legacy) ++legacy_models;
            std::uint64_t& h = legacy ? dig_legacy : dig_plain;
            for (const auto& s : m->submeshes) {
                mix(h, s.vertices.data(), s.vertices.size());
                mix(h, s.indices.data(), s.indices.size());
                const std::uint32_t meta[] = {static_cast<std::uint32_t>(s.layout), s.flags & ~std::uint32_t(kSubmeshUnknownFormat),
                                              s.vertex_count, s.index_count, s.attach_bone, s.has_tangents ? 1u : 0u,
                                              s.format.skinned ? 1u : 0u, s.format.color ? 1u : 0u,
                                              s.format.tangent_frame ? 1u : 0u, static_cast<std::uint32_t>(s.format.uv_sets)};
                mix(h, meta, sizeof meta);
                mix(h, s.bone_palette.data(), s.bone_palette.size() * 2);
            }
            for (const auto& w : m->warnings) mix(h, w.data(), w.size());
        }
        std::printf("real data: alamo_formats parses %zu of %zu model ALOs directly (%zu rejected); %zu models with "
                    "128-byte vertices; output digest plain %016llx legacy %016llx\n",
                    fmt_ok, fmt_ok + fmt_fail, fmt_fail, legacy_models, static_cast<unsigned long long>(dig_plain),
                    static_cast<unsigned long long>(dig_legacy));
        std::printf("  128-byte vertex submeshes (submeshes/vertices/colour zero/colour one):");
        for (const auto& [name, st] : legacy_stats) std::printf(" %s=%zu/%zu/%zu/%zu", name.c_str(), st[0], st[1], st[2], st[3]);
        std::printf("\n  B4I4 submeshes %zu, %zu vertices: %zu not exactly 1,0,0,0, %zu with heaviest weight < 0.999\n",
                    b4i4_subs, b4i4_verts, b4i4_inexact, b4i4_blended);
        std::printf("  D.5 (no frame generation/repair): %zu submeshes in %zu models without a frame now T = B = 0; "
                    "%zu frame submeshes in %zu models with %zu zero stored T/B vertices no longer repaired\n",
                    d5_nonframe_subs, d5_nonframe_models.size(), d5_repaired_subs, d5_repaired_models.size(), d5_repaired_verts);
        std::printf("  D.4 (legacy 0x10005 colour = white): %zu submeshes with a colour format, %zu vertices changed:",
                    d4_subs, d4_verts);
        for (const auto& s : d4_models) std::printf(" %s", s.c_str());
        std::printf("\n");
        CHECK(fmt_fail == 0);
        for (std::size_t i = 0; i < std::min<std::size_t>(fmt_fails.size(), 5); ++i)
            std::printf("  formats reject: %s\n", fmt_fails[i].c_str());
    }

    // D.2: the effect's _ALAMO_VERTEX_TYPE (from the shader catalog, FoC entries over EaW) replaces
    // the ALO format. Counts what it changes and checks the Death Star II case.
    {
        std::map<std::string, std::string> vtype;  // upper effect stem -> vertex type
        std::ifstream f(ALAMO_ASSETS_SHADER_CATALOG, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string js = ss.str();
        if (js.empty()) {
            std::printf("real data: shader catalog %s not readable, override pass skipped\n", ALAMO_ASSETS_SHADER_CATALOG);
        } else {
            const auto doc = mini_json::parse(js);
            for (const char* game : {"eaw", "foc"}) {
                const auto* g = doc["games"].get(game);
                if (!g || !g->get("effects")) continue;
                for (const auto& e : (*g)["effects"].arr) {
                    const auto* al = e.get("alamo");
                    const auto* vt = al ? al->get("vertex_type") : nullptr;
                    if (vt && vt->kind == mini_json::Value::Kind::String) vtype[e["effect"].str] = vt->str;
                }
            }
            ModelLoadOptions opt;
            opt.vertex_type_override = [&vtype](const MaterialRef& mat) -> std::string {
                std::string stem = alamo::normalize_name(mat.effect);
                const auto dot = stem.rfind('.');
                if (dot != std::string::npos) stem.resize(dot);
                const auto it = vtype.find(stem);
                return it == vtype.end() ? std::string() : it->second;
            };
            AssetCache ocache(*vfs, opt);
            parallel_for(alos.size(), [&](std::size_t i) { (void)ocache.model(alos[i]); });
            std::uint64_t dig = 14695981039346656037ull;
            auto mix = [](std::uint64_t& h, const void* p, std::size_t n) {
                h = alamo::fnv1a64(p, n, h);
            };
            std::size_t ov_subs = 0, ov_render = 0, ov_bytes_changed = 0, ov_skin_change = 0;
            std::set<std::string> ov_models;
            std::map<std::string, std::size_t> ov_pairs;  // "from->to" counts
            for (const auto& a : alos) {
                auto m = ocache.model(a);
                if (!m) continue;
                auto base = cache.model(a);
                for (std::size_t si = 0; si < m->submeshes.size(); ++si) {
                    const auto& s = m->submeshes[si];
                    mix(dig, s.vertices.data(), s.vertices.size());
                    const std::uint32_t meta[] = {static_cast<std::uint32_t>(s.layout), s.flags, s.has_tangents ? 1u : 0u,
                                                  s.format.color ? 1u : 0u, s.vertex_type_overridden ? 1u : 0u};
                    mix(dig, meta, sizeof meta);
                    if (!s.vertex_type_overridden) continue;
                    ++ov_subs;
                    if (s.is_render_geometry()) ++ov_render;
                    if (base && s.format.skinned != base->submeshes[si].format.skinned) ++ov_skin_change;
                    if (base && s.vertices != base->submeshes[si].vertices) ++ov_bytes_changed;
                    ov_models.insert(a);
                    ++ov_pairs[s.vertex_format + "->" + s.effective_vertex_format];
                }
            }
            std::printf("real data: effect vertex-type override (%zu effects with a type): %zu submeshes (%zu render) in "
                        "%zu models overridden, %zu with different vertex bytes, %zu change skinning; digest %016llx\n ",
                        vtype.size(), ov_subs, ov_render, ov_models.size(), ov_bytes_changed, ov_skin_change,
                        static_cast<unsigned long long>(dig));
            for (const auto& [k, c] : ov_pairs) std::printf(" %s=%zu", k.c_str(), c);
            std::printf("\n");
            // W_PLANET_DEATHSTAR_II_DESTROYED: 42 alD3dVertN + Planet.fx submeshes -> NU2U3U3, stored
            // UVs (non-zero), T = B = 0 in every vertex.
            if (auto ds = ocache.model("W_PLANET_DEATHSTAR_II_DESTROYED")) {
                std::size_t planet = 0, ok = 0, uv_nonzero = 0, verts_n = 0;
                for (const auto& s : ds->submeshes) {
                    if (!alamo::iequals(s.material.effect, "Planet.fx") || !alamo::iequals(s.vertex_format, "alD3dVertN")) continue;
                    ++planet;
                    bool all_zero = s.vertex_type_overridden && s.has_tangents && s.format.tangent_frame &&
                                    s.effective_vertex_format == "alD3dVertNU2U3U3" && s.layout == VertexLayout::Static;
                    for (std::uint32_t v = 0; v < s.vertex_count; ++v) {
                        const auto x = vertex_at<StaticVertex>(s, v);
                        ++verts_n;
                        if (x.uv0[0] != 0.f || x.uv0[1] != 0.f) ++uv_nonzero;
                        for (int k = 0; k < 3; ++k)
                            if (x.tangent[k] != 0.f || x.binormal[k] != 0.f) all_zero = false;
                    }
                    if (all_zero) ++ok;
                }
                std::printf("real data: W_PLANET_DEATHSTAR_II_DESTROYED: %zu Planet.fx alD3dVertN submeshes, %zu uploaded as "
                            "NU2U3U3 with T = B = 0; %zu of %zu vertices with non-zero uv0\n",
                            planet, ok, uv_nonzero, verts_n);
                CHECK(planet == 42 && ok == 42 && uv_nonzero > 0);
            } else {
                std::printf("FAIL real data: W_PLANET_DEATHSTAR_II_DESTROYED not loaded\n");
                ++g_fail;
            }
        }
    }

    // Every texture referenced by any loaded model.
    std::set<std::string> tex_names;
    for (const auto& a : alos) {
        if (auto m = cache.model(a))
            for (const auto& t : m->texture_names()) tex_names.insert(alamo::normalize_name(t));
    }
    std::vector<std::string> texv(tex_names.begin(), tex_names.end());
    std::atomic<std::size_t> found{0}, dds{0}, tga{0}, cubes{0}, bytes_total{0};
    std::map<TextureFormat, std::size_t> by_fmt;
    parallel_for(texv.size(), [&](std::size_t i) {
        auto t = cache.texture(texv[i]);
        if (t->is_fallback) return;
        ++found;
        bytes_total += t->data.size();
        if (t->cubemap) ++cubes;
        const auto& n = t->name;
        (n.size() > 4 && n.compare(n.size() - 4, 4, ".TGA") == 0 ? tga : dds)++;
        std::lock_guard<std::mutex> l(mu);
        ++by_fmt[t->format];
    });
    std::size_t missing = 0, broken = 0;
    std::vector<std::string> broken_list, missing_list;
    for (const auto& f : cache.failures()) {
        if (f.kind != "texture") continue;
        if (f.reason.rfind("not found", 0) == 0) {
            ++missing;
            missing_list.push_back(f.key);
        } else {
            ++broken;
            broken_list.push_back(f.key + ": " + f.reason);
        }
    }
    std::printf("real data: %zu distinct textures referenced: %zu loaded (%zu DDS, %zu TGA, %zu cubemaps, %.1f MB), "
                "%zu missing, %zu undecodable\n",
                texv.size(), found.load(), dds.load(), tga.load(), cubes.load(), double(bytes_total.load()) / 1048576.0,
                missing, broken);
    for (const auto& [f, c] : by_fmt) std::printf("  %-8s %zu\n", texture_format_name(f), c);
    for (std::size_t i = 0; i < std::min<std::size_t>(missing_list.size(), 15); ++i)
        std::printf("  missing: %s\n", missing_list[i].c_str());
    for (const auto& b : broken_list) std::printf("  undecodable: %s\n", b.c_str());
    CHECK(broken == 0);
    {
        const auto st = cache.stats();
        std::printf("  texture cache: %zu names -> %zu distinct files decoded\n", st.textures, st.texture_files);
    }

    // Every shipped texture file (not only model references) against the decoded-size budget, and
    // MT_COMMANDBAR.TGA (32 bpp, descriptor says 0 alpha bits, real alpha channel).
    {
        std::vector<std::string> files;
        for (const char* pat : {"Data\\Art\\Textures\\*.dds", "Data\\Art\\Textures\\*.tga"})
            for (auto& f : vfs->list(pat)) files.push_back(std::move(f));
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        std::vector<std::uint64_t> sizes(files.size(), 0);
        std::vector<std::string> errs(files.size());
        parallel_for(files.size(), [&](std::size_t i) {
            Bytes b;
            TextureAsset t;
            if (!vfs->read(files[i], b)) return;
            if (decode_texture(std::move(b), t, &errs[i])) sizes[i] = t.data.size();
        });
        std::size_t best = 0, over = 0, failed = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (sizes[i] > sizes[best]) best = i;
            if (!errs[i].empty()) {
                ++failed;
                if (errs[i].find("budget") != std::string::npos) ++over;
                if (failed <= 5) std::printf("  texture file undecodable: %s: %s\n", files[i].c_str(), errs[i].c_str());
            }
        }
        if (!files.empty())
            std::printf("real data: %zu texture files in Data\\Art\\Textures: largest decoded %s = %.1f MiB (budget %llu MiB), "
                        "%zu over budget, %zu undecodable\n",
                        files.size(), files[best].c_str(), double(sizes[best]) / 1048576.0,
                        static_cast<unsigned long long>(kMaxDecodedTextureBytes >> 20), over, failed);
        CHECK(over == 0);
        TextureAsset bar;
        std::string e;
        if (load_texture(*vfs, "MT_COMMANDBAR.TGA", bar, &e)) {
            std::set<std::uint8_t> alphas;
            const auto l0 = bar.level_bytes(bar.levels[0]);
            for (std::size_t i = 3; i < l0.size(); i += 4) alphas.insert(l0[i]);
            std::printf("  %s: %ux%u %s, alpha swizzle %s, %zu distinct alpha values\n", bar.name.c_str(), bar.width,
                        bar.height, texture_format_name(bar.format), bar.swizzle.a == Swizzle::A ? "A" : "not A",
                        alphas.size());
            CHECK(bar.swizzle.a == Swizzle::A && alphas.size() > 2);
        } else {
            std::printf("  MT_COMMANDBAR.TGA: %s (skipped)\n", e.c_str());
        }
    }

    // Lookup steps (SPACE_AND_OPEN.md B.2) vs the pre-B.2 rule (Textures\ + name, then .DDS, then .TGA).
    {
        auto legacy = [](const std::string& name) {
            std::string s = name;
            if (s.find_first_of("\\/") == std::string::npos) s = "Data\\Art\\Textures\\" + s;
            s = alamo::normalize_path(s);
            std::vector<std::string> out;
            auto add = [&](std::string c) {
                if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(std::move(c));
            };
            const auto slash = s.find_last_of('\\');
            const auto dot = s.rfind('.');
            const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
            const std::string stem = has_ext ? s.substr(0, dot) : s;
            if (has_ext) add(s);
            add(stem + ".DDS");
            add(stem + ".TGA");
            return out;
        };
        std::size_t per_step[4] = {0, 0, 0, 0}, now_missing = 0, before_ok = 0, before_missing = 0, regress = 0,
                    changed = 0, name_mismatch = 0;
        for (const auto& n : texv) {
            std::string now_path, old_path;
            for (const auto& c : texture_lookup(n))
                if (vfs->exists(c.path)) {
                    ++per_step[static_cast<std::size_t>(c.step)];
                    now_path = c.path;
                    break;
                }
            for (const auto& c : legacy(n))
                if (vfs->exists(c)) {
                    old_path = c;
                    break;
                }
            if (now_path.empty()) ++now_missing;
            (old_path.empty() ? before_missing : before_ok)++;
            if (!old_path.empty() && now_path.empty()) {
                ++regress;
                std::printf("  REGRESSION: %s resolved before (%s), misses now\n", n.c_str(), old_path.c_str());
            }
            if (!old_path.empty() && !now_path.empty() && old_path != now_path) {
                if (changed < 10) std::printf("  now resolves elsewhere: %s: %s -> %s\n", n.c_str(), old_path.c_str(), now_path.c_str());
                ++changed;
            }
            const auto t = cache.texture(n);
            if ((t->is_fallback ? std::string() : t->name) != now_path) ++name_mismatch;
        }
        std::printf("real data: texture lookup over %zu distinct references: %s=%zu, %s=%zu, %s=%zu, %s=%zu, missing=%zu "
                    "(before: %zu resolved, %zu missing; %zu resolved before and miss now; %zu resolve to a different file)\n",
                    texv.size(), texture_lookup_step_name(TextureLookupStep::AsNamed), per_step[0],
                    texture_lookup_step_name(TextureLookupStep::TextureDir), per_step[1],
                    texture_lookup_step_name(TextureLookupStep::DdsAsNamed), per_step[2],
                    texture_lookup_step_name(TextureLookupStep::DdsTextureDir), per_step[3], now_missing, before_ok,
                    before_missing, regress, changed);
        CHECK(regress == 0);
        CHECK(name_mismatch == 0);
    }

    // Every ALA, sampled once mid-clip against nothing (decode only) plus one skinned sample.
    const auto alas = vfs->list("Data\\Art\\Models\\*.ala");
    std::atomic<std::size_t> anims_ok{0};
    parallel_for(alas.size(), [&](std::size_t i) {
        if (cache.animation(alas[i])) ++anims_ok;
    });
    std::printf("real data: %zu ALA listed, %zu decoded\n", alas.size(), anims_ok.load());
    CHECK(anims_ok.load() == alas.size());
    {  // largest decoded clip (same accounting as build_animation's budget check)
        std::uint64_t best = 0;
        std::string best_name;
        std::uint32_t best_frames = 0;
        std::size_t best_bones = 0;
        for (const auto& a : alas) {
            const auto c = cache.animation(a);
            if (!c) continue;
            std::uint64_t n = 0;
            for (const auto& t : c->tracks)
                n += sizeof(BoneTrack) + t.bone_name.size() + t.translation.size() * sizeof(Float3) +
                     t.rotation.size() * sizeof(Quat) + t.scale.size() * sizeof(Float3) + t.visible.size();
            if (n > best) {
                best = n;
                best_name = a;
                best_frames = c->frame_count;
                best_bones = c->tracks.size();
            }
        }
        std::printf("  largest decoded ALA: %s = %.2f MiB (%zu bones x %u frames)\n", best_name.c_str(),
                    double(best) / 1048576.0, best_bones, best_frames);
    }

    // Capital ship for the first visible milestone.
    const char* ships[] = {"EV_STARDESTOYER_THRAWN", "EV_EXECUTORSTARDESTROYER", "EV_STARDESTROYER"};
    bool ship_ok = false;
    for (const char* s : ships) {
        auto m = cache.model(s);
        if (!m) continue;
        std::size_t render = 0, tris = 0, tex_ok = 0, tex_all = 0;
        for (const auto& sm : m->submeshes)
            if (sm.is_render_geometry() && !(sm.flags & kSubmeshHidden)) {
                ++render;
                tris += sm.index_count / 3;
            }
        for (const auto& t : m->texture_names()) {
            ++tex_all;
            if (!cache.texture(t)->is_fallback) ++tex_ok;
        }
        std::printf("real data: capital ship %s (%s): %zu bones, %zu meshes, %zu submeshes, %zu visible render "
                    "submeshes, %zu tris, textures %zu/%zu, radius %.1f\n",
                    s, vfs->origin(model_path(s)).c_str(), m->skeleton.size(), m->meshes.size(), m->submeshes.size(),
                    render, tris, tex_ok, tex_all, double(m->sphere.radius));
        ship_ok = ship_ok || (render > 0 && tex_ok == tex_all);
    }
    CHECK(ship_ok);

    // Skinned sample with a real clip.
    if (auto m = cache.model("EI_ROYALGAURD"))
        if (auto a = cache.animation("EI_ROYALGAURD_IDLE_00")) {
            Pose pose;
            sample(m->skeleton, *a, 0.5f, true, pose);
            std::vector<Mat3x4> world(m->skeleton.size());
            compute_world(m->skeleton, pose.local, world);
            bool finite = true;
            for (const auto& sm : m->submeshes) {
                std::vector<Mat3x4> pal(sm.bone_palette.size());
                compute_skin_palette(m->skeleton, world, sm.bone_palette, pal);
                for (const auto& p : pal)
                    for (const auto& row : p.m)
                        for (float f : row) finite = finite && std::isfinite(f);
            }
            CHECK(finite);
        }

    // glTF exports: EI_ROYALGAURD is vanilla; LS_Redeye and LPF_Pellaeon_SD_Turret_04 come from the
    // Legeds_ships_transfer mod (docs/formats/validation.md), mounted as MODPATH when present.
    check_gltf(*vfs, "EI_ROYALGAURD");
    const std::string mod = std::string(root) + "\\Mods\\Legeds_ships_transfer";
    if (std::filesystem::is_directory(alamo::path_from_utf8(mod))) {
        alamo::vfs::VfsConfig mcfg = cfg;
        mcfg.mod_paths = {mod};
        auto mvfs = alamo::vfs::Vfs::create(mcfg, &err);
        CHECK(mvfs != nullptr);
        if (mvfs)
            for (const char* g : {"LS_Redeye", "LPF_Pellaeon_SD_Turret_04"}) check_gltf(*mvfs, g);
    } else {
        std::printf("  glTF LS_Redeye / LPF_Pellaeon_SD_Turret_04: mod %s not installed, skipped\n", mod.c_str());
    }
}
#endif

}  // namespace

int main() {
    test_static_model();
    test_skinned_model();
    test_legacy_model();
    test_vertex_type_override();
    test_tangents();
    test_animation_zero_frames();
    test_animation();
    test_dds();
    test_tga();
    test_mips();
    test_cache();
    test_lookup_steps();
#if defined(ALAMO_ASSETS_HAVE_VFS)
    test_real_data();
#else
    std::printf("real data: built without alamo_vfs, skipped\n");
#endif
    if (g_fail) {
        std::printf("alamo_assets_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_assets_tests: all passed\n");
    return 0;
}
