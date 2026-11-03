#include "alamo/formats/alo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace alamo::alo {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

Mat34 read_mat34(ByteReader& r) {
    Mat34 m;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) m.m[row][col] = r.f32();
    return m;
}

struct Ctx {
    Model& model;
    void unknown(const std::string& parent, const Chunk& c) {
        model.unknown.push_back({parent.empty() ? hex_id(c.id) : parent + "/" + hex_id(c.id), c.offset, c.data.size()});
    }
};

// ---- skeleton ----
void parse_skeleton(const Chunk& c, Ctx& ctx) {
    Skeleton& s = ctx.model.skeleton;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        if (ch->id == 0x201) {
            auto r = ch->reader();
            s.declared_count = r.u32();
        } else if (ch->id == 0x202) {
            std::string name;
            bool got = false;
            auto bc = ch->children();
            while (auto b = bc.next()) {
                if (b->id == 0x203) {
                    name = b->as_string();
                } else if (b->id == 0x205 || b->id == 0x206) {
                    auto r = b->reader();
                    s.parents.push_back(r.i32());
                    s.visible.push_back(static_cast<std::uint8_t>(r.u32() != 0));
                    s.billboard_mode.push_back(b->id == 0x206 ? r.u32() : 0);
                    s.local.push_back(read_mat34(r));
                    got = true;
                } else {
                    ctx.unknown("200/202", *b);
                }
            }
            if (!got) throw FormatError("bone without transform chunk", ch->offset);
            s.names.push_back(std::move(name));
        } else {
            ctx.unknown("200", *ch);
        }
    }
    if (s.declared_count != s.names.size())
        throw FormatError("skeleton bone count mismatch: header " + std::to_string(s.declared_count) +
                              ", found " + std::to_string(s.names.size()),
                          c.offset);
    for (std::size_t i = 0; i < s.parents.size(); ++i) {
        const auto p = s.parents[i];
        if (p < -1 || (p >= 0 && static_cast<std::size_t>(p) >= i))
            throw FormatError("bone " + std::to_string(i) + " has invalid parent " + std::to_string(p), c.offset);
    }
}

// ---- material ----
Material parse_material(const Chunk& c, Ctx& ctx) {
    Material m;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        if (ch->id == 0x10101) {
            m.shader = ch->as_string();
            continue;
        }
        if (ch->id < 0x10102 || ch->id > 0x10106) {
            ctx.unknown("400/10100", *ch);
            continue;
        }
        MaterialParam p;
        p.type = static_cast<ParamType>(ch->id - 0x10102);
        bool has_value = false;
        auto mc = ch->minis();
        while (auto mini = mc.next()) {
            if (mini->id == 1) {
                p.name = mini->as_string();
            } else if (mini->id == 2) {
                has_value = true;
                switch (p.type) {
                    case ParamType::Int: p.value = static_cast<std::int32_t>(mini->as_u32()); break;
                    case ParamType::Float: p.value = mini->as_float(); break;
                    case ParamType::Float3: p.value = mini->as_vec3(); break;
                    case ParamType::Texture: p.value = mini->as_string(); break;
                    case ParamType::Float4: p.value = mini->as_vec4(); break;
                }
            }
        }
        if (!has_value) throw FormatError("material parameter without value", ch->offset);
        m.params.push_back(std::move(p));
    }
    return m;
}

// ---- vertices ----
// 0x10007: 144-byte records. 0x10005 (EaW-era files): 128-byte records, the 144-byte layout without
// the unknown float4 at +96 (SPEC.md 2.5); a 144-byte 0x10005 is accepted too. Returns the stride.
std::uint32_t parse_vertices(const Chunk& c, std::uint32_t count, VertexBuffer& vb) {
    if (count == 0) return 0;
    if (c.data.size() % count != 0)
        throw FormatError("vertex buffer size " + std::to_string(c.data.size()) +
                              " not a multiple of vertex count " + std::to_string(count),
                          c.offset);
    const std::size_t stride = c.data.size() / count;
    const bool legacy = stride == 128 && c.id == 0x10005;
    if (stride != 144 && !legacy)
        throw FormatError("unsupported vertex stride " + std::to_string(stride) + " in chunk " + hex_id(c.id), c.offset);
    vb.position.resize(count);
    vb.normal.resize(count);
    for (auto& u : vb.uv) u.resize(count);
    vb.tangent.resize(count);
    vb.binormal.resize(count);
    vb.color.resize(count);
    vb.extra.resize(count);
    vb.bone_index.resize(count);
    vb.bone_weight.resize(count);
    // The record is little-endian floats / u32s in field order (the size check above covers every record), copied field
    // by field straight into the arrays (OPT3 t11: the per-value reader dominated battle model loads).
    static_assert(sizeof(Vec2) == 8 && sizeof(Vec3) == 12 && sizeof(Vec4) == 16 && sizeof(vb.bone_index[0]) == 16);
    const std::uint8_t* rec = c.data.data();
    for (std::uint32_t i = 0; i < count; ++i, rec += stride) {
        const std::uint8_t* f = rec;
        auto take = [&f](auto& out) {
            std::memcpy(&out, f, sizeof out);
            f += sizeof out;
        };
        take(vb.position[i]);
        take(vb.normal[i]);
        for (auto& u : vb.uv) take(u[i]);
        take(vb.tangent[i]);
        take(vb.binormal[i]);
        take(vb.color[i]);
        if (legacy) vb.extra[i] = Vec4{1, 1, 1, 1};  // absent in 128-byte records
        else take(vb.extra[i]);
        take(vb.bone_index[i]);
        take(vb.bone_weight[i]);
    }
    return static_cast<std::uint32_t>(stride);
}

CollisionTree parse_collision(const Chunk& c, std::uint32_t num_primitives) {
    CollisionTree t;
    bool hdr = false, nodes = false, tris = false;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        if (ch->id == 0x1201) {
            hdr = true;
            auto mc = ch->minis();
            while (auto m = mc.next()) {
                switch (m->id) {
                    case 0: t.bounds.min = m->as_vec3(); break;
                    case 1: t.bounds.max = m->as_vec3(); break;
                    case 2: t.declared_nodes = m->as_u32(); break;
                    case 3: t.declared_triangles = m->as_u32(); break;
                    default: break;
                }
            }
        } else if (ch->id == 0x1202) {
            nodes = true;
            if (ch->data.size() % 10) throw FormatError("collision node chunk not multiple of 10", ch->offset);
            auto r = ch->reader();
            t.nodes.resize(ch->data.size() / 10);
            for (auto& n : t.nodes) {
                for (auto& q : n.qmin) q = r.u8();
                for (auto& q : n.qmax) q = r.u8();
                n.count = r.u16();
                n.index = r.u16();
            }
        } else if (ch->id == 0x1203) {
            tris = true;
            auto r = ch->reader();
            t.triangles.resize(ch->data.size() / 2);
            for (auto& v : t.triangles) v = r.u16();
        }
    }
    if (!hdr || !nodes || !tris) throw FormatError("incomplete collision tree", c.offset);
    if (t.declared_nodes != t.nodes.size() || t.declared_triangles != t.triangles.size())
        throw FormatError("collision tree count mismatch", c.offset);
    for (std::size_t i = 0; i < t.nodes.size(); ++i) {
        const auto& n = t.nodes[i];
        if (n.is_leaf()) {
            if (std::size_t(n.index) + n.count > t.triangles.size())
                throw FormatError("collision leaf triangle range out of bounds", c.offset);
        } else if (std::size_t(n.index) + 1 >= t.nodes.size() || n.index <= i) {
            throw FormatError("collision node child index out of bounds", c.offset);
        }
    }
    for (auto tri : t.triangles)
        if (tri >= num_primitives) throw FormatError("collision triangle index out of range", c.offset);
    return t;
}

SubMesh parse_submesh_data(const Chunk& c, Material&& mat, Ctx& ctx) {
    SubMesh sm;
    sm.material = std::move(mat);
    std::optional<Chunk> vchunk;
    std::optional<Chunk> collision;
    bool info = false;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        switch (ch->id) {
            case 0x10001: {
                auto r = ch->reader();
                sm.declared_vertices = r.u32();
                sm.declared_primitives = r.u32();
                info = true;
                break;
            }
            case 0x10002:
                sm.vertex_format = ch->as_string();
                sm.format = vertex_format_info(sm.vertex_format);
                break;
            case 0x10005:
            case 0x10007:
                vchunk = *ch;
                break;
            case 0x10004:
                sm.indices.resize(ch->data.size() / 2);
                if (!sm.indices.empty()) std::memcpy(sm.indices.data(), ch->data.data(), sm.indices.size() * 2);  // LE u16
                break;
            case 0x10006: {
                auto r = ch->reader();
                sm.bone_map.resize(ch->data.size() / 4);
                for (auto& b : sm.bone_map) b = r.u32();
                break;
            }
            case 0x1200:
                collision = *ch;
                break;
            default:
                ctx.unknown("400/10000", *ch);
        }
    }
    if (!info) throw FormatError("submesh without info chunk 0x10001", c.offset);
    if (vchunk) {
        sm.vertex_chunk_id = vchunk->id;
        sm.vertex_stride = parse_vertices(*vchunk, sm.declared_vertices, sm.vertices);
    } else if (sm.declared_vertices) {
        throw FormatError("submesh without vertex buffer", c.offset);
    }
    if (sm.indices.size() != std::size_t(sm.declared_primitives) * 3)
        throw FormatError("index count " + std::to_string(sm.indices.size()) + " != 3 * primitives " +
                              std::to_string(sm.declared_primitives),
                          c.offset);
    for (auto i : sm.indices)
        if (i >= sm.declared_vertices) throw FormatError("index out of range", c.offset);
    if (sm.format.skinned && !sm.bone_map.empty()) {
        // RSkin: bone_index[0] only. B4I4: every influence with a non-zero weight.
        const auto& vb = sm.vertices;
        for (std::size_t v = 0; v < vb.bone_index.size(); ++v) {
            const auto& bi = vb.bone_index[v];
            const auto& w = vb.bone_weight[v];
            const float ws[4] = {w.x, w.y, w.z, w.w};
            for (int k = 0; k < std::max(1, sm.format.bone_influences); ++k) {
                if (k > 0 && ws[k] == 0.f) continue;
                if (bi[static_cast<std::size_t>(k)] >= sm.bone_map.size())
                    throw FormatError("skin bone index out of range of bone map", c.offset);
            }
        }
    }
    if (collision) sm.collision = parse_collision(*collision, sm.declared_primitives);
    return sm;
}

void parse_mesh(const Chunk& c, Ctx& ctx) {
    Mesh m;
    std::optional<Material> pending;
    bool info = false;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        switch (ch->id) {
            case 0x401: m.name = ch->as_string(); break;
            case 0x402: {
                auto r = ch->reader();
                m.declared_submeshes = r.u32();
                m.bounds.min = r.vec3();
                m.bounds.max = r.vec3();
                m.unknown = r.u32();
                m.hidden = r.u32() != 0;
                m.collision_enabled = r.u32() != 0;
                info = true;
                break;
            }
            case 0x10100:
                if (pending) throw FormatError("two materials without submesh data", ch->offset);
                pending = parse_material(*ch, ctx);
                break;
            case 0x10000:
                if (!pending) throw FormatError("submesh data without preceding material", ch->offset);
                m.submeshes.push_back(parse_submesh_data(*ch, std::move(*pending), ctx));
                pending.reset();
                break;
            default:
                ctx.unknown("400", *ch);
        }
    }
    if (!info) throw FormatError("mesh without info chunk 0x402", c.offset);
    if (pending) throw FormatError("trailing material without submesh data", c.offset);
    if (m.declared_submeshes != m.submeshes.size())
        throw FormatError("mesh '" + m.name + "' declares " + std::to_string(m.declared_submeshes) +
                              " submeshes, found " + std::to_string(m.submeshes.size()),
                          c.offset);
    ctx.model.objects.push_back({ObjectRef::Kind::Mesh, static_cast<std::uint32_t>(ctx.model.meshes.size())});
    ctx.model.meshes.push_back(std::move(m));
}

void parse_light(const Chunk& c, Ctx& ctx) {
    Light l;
    bool info = false;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        if (ch->id == 0x1301) {
            l.name = ch->as_string();
        } else if (ch->id == 0x1302) {
            auto r = ch->reader();
            l.type = static_cast<LightType>(r.u32());
            l.color = r.vec3();
            l.intensity = r.f32();
            l.far_attenuation_end = r.f32();
            l.far_attenuation_start = r.f32();
            l.hotspot_size = r.f32();
            l.falloff_size = r.f32();
            info = true;
        } else {
            ctx.unknown("1300", *ch);
        }
    }
    if (!info) throw FormatError("light without info chunk", c.offset);
    ctx.model.objects.push_back({ObjectRef::Kind::Light, static_cast<std::uint32_t>(ctx.model.lights.size())});
    ctx.model.lights.push_back(std::move(l));
}

void parse_connections(const Chunk& c, Ctx& ctx) {
    Model& m = ctx.model;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        if (ch->id == 0x601) {
            auto mc = ch->minis();
            while (auto mini = mc.next()) {
                if (mini->id == 1) m.declared_connections = mini->as_u32();
                else if (mini->id == 4) m.declared_proxies = mini->as_u32();
                else if (mini->id == 9) m.declared_dazzles = mini->as_u32();
            }
        } else if (ch->id == 0x602) {
            Connection con;
            auto mc = ch->minis();
            while (auto mini = mc.next()) {
                if (mini->id == 2) con.object = mini->as_u32();
                else if (mini->id == 3) con.bone = mini->as_u32();
            }
            m.connections.push_back(con);
        } else if (ch->id == 0x603) {
            Proxy p;
            auto mc = ch->minis();
            while (auto mini = mc.next()) {
                switch (mini->id) {
                    case 5: p.name = mini->as_string(); break;
                    case 6: p.bone = mini->as_u32(); break;
                    case 7: p.hidden = mini->as_u32() != 0; break;
                    case 8: p.alt_decrease_stay_hidden = mini->as_u32() != 0; break;
                    default: break;
                }
            }
            m.proxies.push_back(std::move(p));
        } else {
            ctx.unknown("600", *ch);
        }
    }
}

void validate_model(const Model& m) {
    const auto nb = m.skeleton.size();
    if (m.connections.size() != m.declared_connections)
        throw FormatError("connection count mismatch: header " + std::to_string(m.declared_connections) +
                              ", found " + std::to_string(m.connections.size()), 0);
    if (m.proxies.size() != m.declared_proxies)
        throw FormatError("proxy count mismatch", 0);
    for (const auto& c : m.connections) {
        if (c.object >= m.objects.size()) throw FormatError("connection object index out of range", 0);
        if (c.bone >= nb) throw FormatError("connection bone index out of range", 0);
    }
    for (const auto& p : m.proxies)
        if (p.bone >= nb) throw FormatError("proxy bone index out of range", 0);
    for (const auto& mesh : m.meshes)
        for (const auto& sm : mesh.submeshes)
            for (auto b : sm.bone_map)
                if (b >= nb) throw FormatError("skin bone map entry out of range", 0);
}

}  // namespace

// ---- public helpers ----
const char* billboard_mode_name(std::uint32_t m) {
    static const char* n[] = {"none", "parallel", "face", "z_axis_view", "z_axis_wind", "z_axis_light", "sun_glow", "sun"};
    return m < 8 ? n[m] : "unknown";
}

VertexFormatInfo vertex_format_info(std::string_view n) {
    VertexFormatInfo f;
    // Known names (all observed in the shipped FoC + EaW data unless noted; SPEC.md 2.6).
    // bones: influences per vertex (0 static, 1 RSkin rigid, 4 B4I4).
    struct E { std::string_view name; bool normal; int uv; bool tbn; bool color; int bones; bool grass; };
    static constexpr E table[] = {
        {"alD3dVertN", true, 0, false, false, 0, false},
        {"alD3dVertNU2", true, 1, false, false, 0, false},
        {"alD3dVertNU2C", true, 1, false, true, 0, false},
        {"alD3dVertNU2U3U3", true, 1, true, false, 0, false},
        {"alD3dVertNU2U3U3C", true, 1, true, true, 0, false},
        {"alD3dVertNC", true, 0, false, true, 0, false},            // not observed
        {"alD3dVertRSkinNU2", true, 1, false, false, 1, false},
        {"alD3dVertRSkinNU2U3U3", true, 1, true, false, 1, false},
        {"alD3dVertB4I4NU2", true, 1, false, false, 4, false},
        {"alD3dVertB4I4NU2U3U3", true, 1, true, false, 4, false},
        {"alD3dVertGrass", true, 1, false, false, 0, true},
    };
    auto contains = [&](std::string_view tok) {
        for (std::size_t i = 0; i + tok.size() <= n.size(); ++i)
            if (iequals(n.substr(i, tok.size()), tok)) return true;
        return false;
    };
    for (const auto& e : table) {
        if (iequals(e.name, n)) {
            f.known = true;
            f.normal = e.normal;
            f.uv_sets = e.uv;
            f.tangent_frame = e.tbn;
            f.color = e.color;
            f.bone_influences = e.bones;
            f.skinned = e.bones > 0;
            f.grass = e.grass;
            return f;
        }
    }
    // Unknown name: infer conservatively from the tokens (U2, U3U3, RSkin/B4I4, trailing C).
    f.normal = true;
    f.uv_sets = contains("U2") ? 1 : 0;
    f.tangent_frame = contains("U3U3");
    f.color = !n.empty() && (n.back() == 'C' || n.back() == 'c');
    f.bone_influences = contains("B4I4") ? 4 : contains("Skin") ? 1 : 0;
    f.skinned = f.bone_influences > 0;
    return f;
}

Mat34 Skeleton::world(std::size_t bone) const {
    Mat34 m = local.at(bone);
    for (std::int32_t p = parents.at(bone); p >= 0; p = parents[static_cast<std::size_t>(p)])
        m = local[static_cast<std::size_t>(p)] * m;
    return m;
}

std::vector<Mat34> Skeleton::world_all() const {
    std::vector<Mat34> w(size());
    for (std::size_t i = 0; i < size(); ++i)
        w[i] = parents[i] < 0 ? local[i] : w[static_cast<std::size_t>(parents[i])] * local[i];
    return w;
}

std::optional<std::size_t> Skeleton::find(std::string_view name) const {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (iequals(names[i], name)) return i;
    return std::nullopt;
}

const MaterialParam* Material::find(std::string_view name) const {
    for (const auto& p : params)
        if (iequals(p.name, name)) return &p;
    return nullptr;
}

std::vector<AABB> CollisionTree::node_bounds() const {
    std::vector<AABB> out(nodes.size());
    if (nodes.empty()) return out;
    auto deq = [](const AABB& parent, const Node& n) {
        AABB b;
        const float pmin[3] = {parent.min.x, parent.min.y, parent.min.z};
        const float pmax[3] = {parent.max.x, parent.max.y, parent.max.z};
        float mn[3], mx[3];
        for (int k = 0; k < 3; ++k) {
            mn[k] = pmin[k] + (pmax[k] - pmin[k]) * n.qmin[k] / 255.f;
            mx[k] = pmin[k] + (pmax[k] - pmin[k]) * n.qmax[k] / 255.f;
        }
        b.min = {mn[0], mn[1], mn[2]};
        b.max = {mx[0], mx[1], mx[2]};
        return b;
    };
    // Iterative DFS: (node, parent box)
    std::vector<std::pair<std::size_t, AABB>> stack{{0, bounds}};
    while (!stack.empty()) {
        auto [i, pb] = stack.back();
        stack.pop_back();
        if (i >= nodes.size()) continue;
        out[i] = deq(pb, nodes[i]);
        if (!nodes[i].is_leaf()) {
            stack.push_back({nodes[i].index, out[i]});
            stack.push_back({std::size_t(nodes[i].index) + 1, out[i]});
        }
    }
    return out;
}

std::uint32_t Model::object_bone(std::uint32_t object) const {
    for (const auto& c : connections)
        if (c.object == object) return c.bone;
    return 0;
}
std::uint32_t Model::mesh_bone(std::uint32_t mesh_index) const {
    for (std::uint32_t o = 0; o < objects.size(); ++o)
        if (objects[o].kind == ObjectRef::Kind::Mesh && objects[o].index == mesh_index) return object_bone(o);
    return 0;
}

AloFile parse(Bytes data) {
    AloFile f;
    auto top = top_level_chunks(data);
    auto first = top.next();
    if (!first) throw FormatError("empty file", 0);
    if (first->id == 0x900) {
        f.kind = AloFile::Kind::ParticleSystem;
        f.particles = particle::parse_system(*first);
        if (auto extra = top.next())
            throw FormatError("unexpected chunk after particle system", extra->offset);
        return f;
    }
    f.kind = AloFile::Kind::Model;
    Ctx ctx{f.model};
    bool skeleton = false, connections = false;
    for (std::optional<Chunk> c = first; c; c = top.next()) {
        switch (c->id) {
            case 0x200:
                if (skeleton) throw FormatError("duplicate skeleton", c->offset);
                parse_skeleton(*c, ctx);
                skeleton = true;
                break;
            case 0x400: parse_mesh(*c, ctx); break;
            case 0x1300: parse_light(*c, ctx); break;
            case 0x600:
                parse_connections(*c, ctx);
                connections = true;
                break;
            default: ctx.unknown("", *c);
        }
    }
    if (!skeleton) throw FormatError("model has no skeleton chunk 0x200", 0);
    if (!connections) throw FormatError("model has no connections chunk 0x600", 0);
    validate_model(f.model);
    return f;
}

AloFile load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    return parse(buf);
}

}  // namespace alamo::alo
