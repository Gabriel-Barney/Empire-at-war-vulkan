// alo2gltf: export an ALO model (static meshes + skeleton, rigid skins) and optionally one ALA
// animation to glTF 2.0.
//
//   alo2gltf [options] <model.alo> <out.gltf|out.glb>
//     --anim <file.ala>   add the animation (bones matched by name)
//     --all               also export hidden meshes, collision meshes and shadow volumes
//     --all-lods          export every <name>_LOD<n> mesh (default: only the most detailed one)
//     --no-skeleton       do not emit bone nodes (meshes are baked to model space)
//
// Coordinate system: Alamo is Z-up; a root node rotates -90 deg about X so the result is Y-up.
// Triangle winding is kept as stored (it is CCW w.r.t. the stored normals in a right-handed frame).
#include "alamo/formats/ala.hpp"
#include "alamo/formats/alo.hpp"
#include "json_writer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>

using namespace alamo;
using tools::JsonWriter;

namespace {

struct Options {
    std::filesystem::path in, out, anim;
    bool all = false;
    bool all_lods = false;
    bool skeleton = true;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool skip_submesh(const alo::Mesh& m, const alo::SubMesh& sm, const Options& o) {
    if (o.all) return false;
    if (m.hidden) return true;
    const auto sh = lower(sm.material.shader);
    return sh.find("shadow") != std::string::npos || sh.find("collision") != std::string::npos;
}

// Meshes named <base>_LOD<n> are alternative levels of detail. Unless --all-lods is given, keep
// only the variant with the most triangles for each base name.
std::vector<bool> select_lods(const alo::Model& m, const Options& o) {
    std::vector<bool> keep(m.meshes.size(), true);
    if (o.all_lods) return keep;
    std::map<std::string, std::pair<std::size_t, std::size_t>> best;  // base -> (tris, mesh)
    auto base_of = [](const std::string& name) -> std::optional<std::string> {
        const auto l = lower(name);
        const auto p = l.rfind("_lod");
        if (p == std::string::npos || p + 4 >= l.size()) return std::nullopt;
        for (std::size_t i = p + 4; i < l.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(l[i]))) return std::nullopt;
        return l.substr(0, p);
    };
    for (std::size_t i = 0; i < m.meshes.size(); ++i) {
        auto b = base_of(m.meshes[i].name);
        if (!b) continue;
        std::size_t tris = 0;
        for (const auto& sm : m.meshes[i].submeshes) tris += sm.indices.size() / 3;
        auto it = best.find(*b);
        if (it == best.end()) {
            best[*b] = {tris, i};
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

// ---- binary buffer builder ----
struct Buffer {
    std::vector<std::uint8_t> data;
    struct View { std::size_t offset, length; int target; };
    struct Accessor {
        std::size_t view; int component; std::size_t count; const char* type;
        std::vector<float> min, max; bool normalized = false;
    };
    std::vector<View> views;
    std::vector<Accessor> accessors;

    template <class T>
    std::size_t add_view(const T* p, std::size_t n, int target) {
        while (data.size() % 4) data.push_back(0);
        const std::size_t off = data.size();
        const auto* b = reinterpret_cast<const std::uint8_t*>(p);
        data.insert(data.end(), b, b + n * sizeof(T));
        views.push_back({off, n * sizeof(T), target});
        return views.size() - 1;
    }
    std::size_t add_accessor(std::size_t view, int comp, std::size_t count, const char* type,
                             std::vector<float> mn = {}, std::vector<float> mx = {}) {
        accessors.push_back({view, comp, count, type, std::move(mn), std::move(mx)});
        return accessors.size() - 1;
    }
};
constexpr int kFloat = 5126, kU16 = 5123;
[[maybe_unused]] constexpr int kU32 = 5125;
constexpr int kArrayBuffer = 34962, kElementBuffer = 34963;

std::size_t add_vec3(Buffer& b, const std::vector<Vec3>& v, bool minmax) {
    std::vector<float> mn, mx;
    if (minmax && !v.empty()) {
        mn = {v[0].x, v[0].y, v[0].z};
        mx = mn;
        for (const auto& p : v) {
            const float c[3] = {p.x, p.y, p.z};
            for (std::size_t k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], c[k]); mx[k] = std::max(mx[k], c[k]); }
        }
    }
    auto view = b.add_view(v.data(), v.size(), kArrayBuffer);
    return b.add_accessor(view, kFloat, v.size(), "VEC3", mn, mx);
}

Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-12f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0, 0, 1};
}

// Decompose an affine 3x4 into translation, rotation (quat xyzw), scale.
void decompose(const Mat34& m, Vec3& t, Quat& q, Vec3& s) {
    t = {m.m[0][3], m.m[1][3], m.m[2][3]};
    float c[3][3];
    for (int col = 0; col < 3; ++col) {
        const float len = std::sqrt(m.m[0][col] * m.m[0][col] + m.m[1][col] * m.m[1][col] + m.m[2][col] * m.m[2][col]);
        (col == 0 ? s.x : col == 1 ? s.y : s.z) = len;
        for (int r = 0; r < 3; ++r) c[r][col] = len > 1e-12f ? m.m[r][col] / len : 0.f;
    }
    // Handle reflection: flip one axis so that the rotation is proper.
    const float det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
                      c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
    if (det < 0) {
        s.x = -s.x;
        for (int r = 0; r < 3; ++r) c[r][0] = -c[r][0];
    }
    const float tr = c[0][0] + c[1][1] + c[2][2];
    if (tr > 0) {
        const float S = std::sqrt(tr + 1.f) * 2;
        q = {(c[2][1] - c[1][2]) / S, (c[0][2] - c[2][0]) / S, (c[1][0] - c[0][1]) / S, 0.25f * S};
    } else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) {
        const float S = std::sqrt(1.f + c[0][0] - c[1][1] - c[2][2]) * 2;
        q = {0.25f * S, (c[0][1] + c[1][0]) / S, (c[0][2] + c[2][0]) / S, (c[2][1] - c[1][2]) / S};
    } else if (c[1][1] > c[2][2]) {
        const float S = std::sqrt(1.f + c[1][1] - c[0][0] - c[2][2]) * 2;
        q = {(c[0][1] + c[1][0]) / S, 0.25f * S, (c[1][2] + c[2][1]) / S, (c[0][2] - c[2][0]) / S};
    } else {
        const float S = std::sqrt(1.f + c[2][2] - c[0][0] - c[1][1]) * 2;
        q = {(c[0][2] + c[2][0]) / S, (c[1][2] + c[2][1]) / S, 0.25f * S, (c[1][0] - c[0][1]) / S};
    }
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 0) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
}

void mat4_colmajor(const Mat34& m, float out[16]) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            out[col * 4 + row] = row < 3 ? m.m[row][col] : (col == 3 ? 1.f : 0.f);
}

struct Prim {
    std::size_t pos, nrm, uv = SIZE_MAX, joints = SIZE_MAX, weights = SIZE_MAX, idx;
    std::size_t material;
};
struct GMesh { std::string name; std::vector<Prim> prims; bool skinned; std::uint32_t bone; };

int run(const Options& o) {
    auto file = alo::load(o.in);
    if (file.kind != alo::AloFile::Kind::Model) throw std::runtime_error("not a model (particle system)");
    const auto& model = file.model;
    const auto& skel = model.skeleton;
    const auto world = skel.world_all();
    std::vector<Mat34> inv_world(world.size());
    for (std::size_t i = 0; i < world.size(); ++i) inv_world[i] = world[i].inverse();

    Buffer buf;
    std::vector<GMesh> gmeshes;
    std::vector<const alo::Material*> materials;
    std::size_t exported_tris = 0;

    const auto lod_keep = select_lods(model, o);
    for (std::uint32_t mi = 0; mi < model.meshes.size(); ++mi) {
        const auto& mesh = model.meshes[mi];
        if (!lod_keep[mi]) continue;
        GMesh st{mesh.name, {}, false, model.mesh_bone(mi)};
        GMesh sk{mesh.name + "_skin", {}, true, 0};
        for (const auto& sm : mesh.submeshes) {
            if (skip_submesh(mesh, sm, o) || sm.indices.empty()) continue;
            const auto& vb = sm.vertices;
            const std::size_t n = vb.size();
            const bool skinned = sm.format.skinned && !sm.bone_map.empty();
            std::vector<Vec3> pos(vb.position), nrm(n);
            for (std::size_t i = 0; i < n; ++i) nrm[i] = normalize(vb.normal[i]);
            // Skinned (RSkin) vertices are stored in model/bind space; static mesh vertices are
            // stored relative to the bone the mesh is connected to.
            if (!skinned && !o.skeleton) {
                for (std::size_t i = 0; i < n; ++i) {
                    pos[i] = world[st.bone].transform_point(vb.position[i]);
                    nrm[i] = normalize(world[st.bone].transform_dir(vb.normal[i]));
                }
            }
            Prim p;
            p.pos = add_vec3(buf, pos, true);
            p.nrm = add_vec3(buf, nrm, false);
            if (sm.format.uv_sets > 0 || sm.format.known == false) {
                std::vector<Vec2> uv(vb.uv[0]);
                auto view = buf.add_view(uv.data(), uv.size(), kArrayBuffer);
                p.uv = buf.add_accessor(view, kFloat, uv.size(), "VEC2");
            }
            if (skinned && o.skeleton) {
                std::vector<std::uint16_t> joints(n * 4, 0);
                std::vector<float> weights(n * 4, 0.f);
                for (std::size_t i = 0; i < n; ++i) {
                    joints[i * 4] = static_cast<std::uint16_t>(sm.bone_map[vb.bone_index[i][0]]);
                    weights[i * 4] = 1.f;  // rigid skin
                }
                auto jv = buf.add_view(joints.data(), joints.size(), kArrayBuffer);
                p.joints = buf.add_accessor(jv, kU16, n, "VEC4");
                auto wv = buf.add_view(weights.data(), weights.size(), kArrayBuffer);
                p.weights = buf.add_accessor(wv, kFloat, n, "VEC4");
            }
            auto iv = buf.add_view(sm.indices.data(), sm.indices.size(), kElementBuffer);
            p.idx = buf.add_accessor(iv, kU16, sm.indices.size(), "SCALAR");
            p.material = materials.size();
            materials.push_back(&sm.material);
            exported_tris += sm.indices.size() / 3;
            ((skinned && o.skeleton) ? sk : st).prims.push_back(p);
        }
        if (!st.prims.empty()) gmeshes.push_back(std::move(st));
        if (!sk.prims.empty()) gmeshes.push_back(std::move(sk));
    }
    const bool any_skin = std::any_of(gmeshes.begin(), gmeshes.end(), [](const GMesh& g) { return g.skinned; });

    // Inverse bind matrices for the skin (all bones are joints).
    std::size_t ibm_accessor = SIZE_MAX;
    if (any_skin) {
        std::vector<float> ibm(skel.size() * 16);
        for (std::size_t i = 0; i < skel.size(); ++i) mat4_colmajor(inv_world[i], &ibm[i * 16]);
        auto v = buf.add_view(ibm.data(), ibm.size(), 0);
        ibm_accessor = buf.add_accessor(v, kFloat, skel.size(), "MAT4");
    }

    // Animation.
    struct Channel { std::size_t node; const char* path; std::size_t sampler_out; };
    std::vector<Channel> channels;
    std::size_t time_accessor = SIZE_MAX;
    std::string anim_name;
    if (!o.anim.empty()) {
        if (!o.skeleton) throw std::runtime_error("--anim requires the skeleton");
        auto anim = ala::load(o.anim);
        anim_name = o.anim.stem().string();
        std::vector<float> times(anim.num_frames);
        for (std::uint32_t f = 0; f < anim.num_frames; ++f) times[f] = anim.fps > 0 ? static_cast<float>(f) / anim.fps : static_cast<float>(f);
        auto tv = buf.add_view(times.data(), times.size(), 0);
        time_accessor = buf.add_accessor(tv, kFloat, times.size(), "SCALAR", {times.front()}, {times.back()});
        for (std::size_t bi = 0; bi < anim.bones.size(); ++bi) {
            auto target = skel.find(anim.bones[bi].name);
            if (!target) continue;
            const std::size_t node = 1 + *target;  // node 0 is the scene root
            std::vector<Vec3> tr(anim.num_frames), sc(anim.num_frames);
            std::vector<Quat> rot(anim.num_frames);
            for (std::uint32_t f = 0; f < anim.num_frames; ++f) {
                tr[f] = anim.translation(bi, f);
                rot[f] = anim.rotation(bi, f);
                sc[f] = anim.scale(bi, f);
            }
            auto v1 = buf.add_view(tr.data(), tr.size(), 0);
            channels.push_back({node, "translation", buf.add_accessor(v1, kFloat, tr.size(), "VEC3")});
            auto v2 = buf.add_view(rot.data(), rot.size(), 0);
            channels.push_back({node, "rotation", buf.add_accessor(v2, kFloat, rot.size(), "VEC4")});
            auto v3 = buf.add_view(sc.data(), sc.size(), 0);
            channels.push_back({node, "scale", buf.add_accessor(v3, kFloat, sc.size(), "VEC3")});
        }
    }

    // ---- JSON ----
    const bool glb = lower(o.out.extension().string()) == ".glb";
    const auto bin_name = o.out.stem().string() + ".bin";
    std::ostringstream js;
    JsonWriter j(js, !glb);
    j.begin_object();
    j.key("asset").begin_object().kv("version", "2.0").kv("generator", "alamo_formats alo2gltf").end_object();
    j.kv("scene", 0);
    j.key("scenes").begin_array().begin_object().key("nodes").begin_array().value(0).end_array().end_object().end_array();

    // Nodes: 0 = root, 1..B = bones, then mesh nodes.
    std::vector<std::vector<std::size_t>> children(1 + (o.skeleton ? skel.size() : 0));
    std::size_t first_mesh_node = children.size();
    if (o.skeleton)
        for (std::size_t i = 0; i < skel.size(); ++i)
            children[skel.parents[i] < 0 ? 0 : 1 + std::size_t(skel.parents[i])].push_back(1 + i);
    for (std::size_t g = 0; g < gmeshes.size(); ++g) {
        const auto& gm = gmeshes[g];
        const std::size_t parent = (gm.skinned || !o.skeleton) ? 0 : 1 + gm.bone;
        children[parent].push_back(first_mesh_node + g);
    }
    auto write_children = [&](std::size_t n) {
        if (n < children.size() && !children[n].empty()) {
            j.key("children").begin_array();
            for (auto c : children[n]) j.value(c);
            j.end_array();
        }
    };
    j.key("nodes").begin_array();
    j.begin_object().kv("name", o.in.stem().string()).key("rotation").floats({-0.70710678f, 0, 0, 0.70710678f});
    write_children(0);
    j.end_object();
    if (o.skeleton) {
        for (std::size_t i = 0; i < skel.size(); ++i) {
            j.begin_object().kv("name", skel.names[i]);
            Vec3 t, s;
            Quat q;
            decompose(skel.local[i], t, q, s);
            j.key("translation").floats({t.x, t.y, t.z});
            j.key("rotation").floats({q.x, q.y, q.z, q.w});
            j.key("scale").floats({s.x, s.y, s.z});
            write_children(1 + i);
            // Proxies attached to this bone
            bool any = false;
            for (const auto& p : model.proxies) {
                if (p.bone != i) continue;
                if (!any) { j.key("extras").begin_object().key("proxies").begin_array(); any = true; }
                j.value(p.name);
            }
            if (any) j.end_array().end_object();
            j.end_object();
        }
    }
    for (std::size_t g = 0; g < gmeshes.size(); ++g) {
        j.begin_object().kv("name", gmeshes[g].name).kv("mesh", g);
        if (gmeshes[g].skinned) j.kv("skin", 0);
        j.end_object();
    }
    j.end_array();

    if (any_skin) {
        j.key("skins").begin_array().begin_object().kv("inverseBindMatrices", ibm_accessor).kv("skeleton", 1);
        j.key("joints").begin_array();
        for (std::size_t i = 0; i < skel.size(); ++i) j.value(1 + i);
        j.end_array().end_object().end_array();
    }

    j.key("meshes").begin_array();
    for (const auto& gm : gmeshes) {
        j.begin_object().kv("name", gm.name).key("primitives").begin_array();
        for (const auto& p : gm.prims) {
            j.begin_object().key("attributes").begin_object().kv("POSITION", p.pos).kv("NORMAL", p.nrm);
            if (p.uv != SIZE_MAX) j.kv("TEXCOORD_0", p.uv);
            if (p.joints != SIZE_MAX) j.kv("JOINTS_0", p.joints).kv("WEIGHTS_0", p.weights);
            j.end_object().kv("indices", p.idx).kv("material", p.material).kv("mode", 4).end_object();
        }
        j.end_array().end_object();
    }
    j.end_array();

    j.key("materials").begin_array();
    for (const auto* m : materials) {
        std::string tex;
        if (auto* p = m->find("BaseTexture"); p && std::holds_alternative<std::string>(p->value))
            tex = std::get<std::string>(p->value);
        j.begin_object().kv("name", m->shader + (tex.empty() ? "" : ":" + tex));
        j.key("pbrMetallicRoughness").begin_object();
        if (auto* d = m->find("Diffuse"); d && std::holds_alternative<Vec4>(d->value)) {
            const auto& v = std::get<Vec4>(d->value);
            j.key("baseColorFactor").floats({std::clamp(v.x, 0.f, 1.f), std::clamp(v.y, 0.f, 1.f), std::clamp(v.z, 0.f, 1.f), 1.f});
        }
        j.kv("metallicFactor", 0.0).kv("roughnessFactor", 0.8).end_object();
        if (lower(m->shader).find("alpha") != std::string::npos || lower(m->shader).find("additive") != std::string::npos)
            j.kv("alphaMode", "BLEND");
        j.kv("doubleSided", false);
        j.key("extras").begin_object().kv("shader", m->shader);
        for (const auto& p : m->params) {
            j.key(p.name);
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::int32_t>) j.value(static_cast<long long>(v));
                else if constexpr (std::is_same_v<T, float>) j.value(v);
                else if constexpr (std::is_same_v<T, Vec3>) j.floats({v.x, v.y, v.z});
                else if constexpr (std::is_same_v<T, Vec4>) j.floats({v.x, v.y, v.z, v.w});
                else j.value(v);
            }, p.value);
        }
        j.end_object().end_object();
    }
    j.end_array();

    if (!channels.empty()) {
        j.key("animations").begin_array().begin_object().kv("name", anim_name);
        j.key("samplers").begin_array();
        for (const auto& c : channels)
            j.begin_object().kv("input", time_accessor).kv("output", c.sampler_out).kv("interpolation", "LINEAR").end_object();
        j.end_array();
        j.key("channels").begin_array();
        for (std::size_t i = 0; i < channels.size(); ++i)
            j.begin_object().kv("sampler", i).key("target").begin_object().kv("node", channels[i].node)
                .kv("path", channels[i].path).end_object().end_object();
        j.end_array();
        j.end_object().end_array();
    }

    j.key("buffers").begin_array().begin_object().kv("byteLength", buf.data.size());
    if (!glb) j.kv("uri", bin_name);
    j.end_object().end_array();
    j.key("bufferViews").begin_array();
    for (const auto& v : buf.views) {
        j.begin_object().kv("buffer", 0).kv("byteOffset", v.offset).kv("byteLength", v.length);
        if (v.target) j.kv("target", v.target);
        j.end_object();
    }
    j.end_array();
    j.key("accessors").begin_array();
    for (const auto& a : buf.accessors) {
        j.begin_object().kv("bufferView", a.view).kv("componentType", a.component).kv("count", a.count).kv("type", a.type);
        if (!a.min.empty()) {
            j.key("min").begin_array();
            for (float f : a.min) j.value(f);
            j.end_array().key("max").begin_array();
            for (float f : a.max) j.value(f);
            j.end_array();
        }
        j.end_object();
    }
    j.end_array();
    j.end_object();

    std::string json = js.str();
    if (glb) {
        while (json.size() % 4) json += ' ';
        while (buf.data.size() % 4) buf.data.push_back(0);
        std::ofstream f(o.out, std::ios::binary);
        auto w32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        w32(0x46546C67);
        w32(2);
        w32(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + buf.data.size()));
        w32(static_cast<std::uint32_t>(json.size()));
        w32(0x4E4F534A);
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
        w32(static_cast<std::uint32_t>(buf.data.size()));
        w32(0x004E4942);
        f.write(reinterpret_cast<const char*>(buf.data.data()), static_cast<std::streamsize>(buf.data.size()));
    } else {
        std::ofstream(o.out, std::ios::binary) << json << "\n";
        std::ofstream b(o.out.parent_path() / bin_name, std::ios::binary);
        b.write(reinterpret_cast<const char*>(buf.data.data()), static_cast<std::streamsize>(buf.data.size()));
    }
    std::cerr << "alo2gltf: " << o.in.filename().string() << " -> " << o.out.string() << ": " << gmeshes.size()
              << " meshes, " << exported_tris << " triangles, " << skel.size() << " bones"
              << (channels.empty() ? "" : ", " + std::to_string(channels.size()) + " anim channels") << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--anim" && i + 1 < argc) o.anim = argv[++i];
        else if (a == "--all") o.all = true;
        else if (a == "--all-lods") o.all_lods = true;
        else if (a == "--no-skeleton") o.skeleton = false;
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: alo2gltf [--anim file.ala] [--all] [--all-lods] [--no-skeleton] <in.alo> <out.gltf|out.glb>\n";
            return 0;
        } else pos.push_back(a);
    }
    if (pos.size() != 2) {
        std::cerr << "usage: alo2gltf [--anim file.ala] [--all] [--all-lods] [--no-skeleton] <in.alo> <out.gltf|out.glb>\n";
        return 2;
    }
    o.in = pos[0];
    o.out = pos[1];
    try {
        return run(o);
    } catch (const std::exception& e) {
        std::cerr << "alo2gltf: " << e.what() << "\n";
        return 1;
    }
}
