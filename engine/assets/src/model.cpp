#include "alamo/assets/model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <variant>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/formats/alo.hpp"

namespace alamo::assets {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

int lod_of(std::string_view name) {
    const std::string l = lower(name);
    const auto p = l.rfind("_lod");
    if (p == std::string::npos || p + 4 >= l.size()) return -1;
    int v = 0;
    for (std::size_t i = p + 4; i < l.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(l[i]))) return -1;
        if (v > 100000) return -1;
        v = v * 10 + (l[i] - '0');
    }
    return v;
}

std::uint8_t unorm8(float f) {
    if (!(f > 0.f)) return 0;  // also NaN
    if (f >= 1.f) return 255;
    return static_cast<std::uint8_t>(f * 255.f + 0.5f);
}

Mat3x4 to_mat(const alamo::Mat34& m) {
    Mat3x4 o;
    std::memcpy(o.m, m.m, sizeof o.m);
    return o;
}

Float3 to_f3(const alamo::Vec3& v) { return {v.x, v.y, v.z}; }

MaterialRef convert_material(const alo::Material& src) {
    MaterialRef m;
    m.effect = src.shader;
    m.params.reserve(src.params.size());
    for (const auto& p : src.params) {
        MaterialParam d;
        d.name = p.name;
        d.type = static_cast<ParamType>(static_cast<std::uint8_t>(p.type));
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::int32_t>) {
                    d.int_value = v;
                    d.value = {float(v), 0, 0, 0};
                } else if constexpr (std::is_same_v<T, float>) {
                    d.value = {v, 0, 0, 0};
                } else if constexpr (std::is_same_v<T, alamo::Vec3>) {
                    d.value = {v.x, v.y, v.z, 0};
                } else if constexpr (std::is_same_v<T, alamo::Vec4>) {
                    d.value = {v.x, v.y, v.z, v.w};
                } else {
                    d.texture = v;
                }
            },
            p.value);
        m.params.push_back(std::move(d));
    }
    return m;
}

void bounds_of(const std::vector<alamo::Vec3>& pos, Aabb& box, Sphere& sphere) {
    if (pos.empty()) {
        box = Aabb{{1, 1, 1}, {-1, -1, -1}};  // invalid
        sphere = {};
        return;
    }
    box.min = box.max = to_f3(pos[0]);
    for (const auto& p : pos) {
        box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
        box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
    }
    sphere.center = {(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, (box.min.z + box.max.z) * 0.5f};
    float r2 = 0;
    for (const auto& p : pos) {
        const float dx = p.x - sphere.center.x, dy = p.y - sphere.center.y, dz = p.z - sphere.center.z;
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    sphere.radius = std::sqrt(r2);
}

std::uint32_t submesh_flags(const alo::Mesh& mesh, const alo::SubMesh& sm, const alo::VertexFormatInfo& fmt) {
    std::uint32_t f = 0;
    if (mesh.hidden) f |= kSubmeshHidden;
    if (mesh.collision_enabled) f |= kSubmeshCollisionMesh;
    const std::string sh = lower(sm.material.shader);
    if (sh.find("collision") != std::string::npos) f |= kSubmeshCollisionShader;
    if (sh.find("shadowvolume") != std::string::npos) f |= kSubmeshShadowVolume;
    if (sh == "aldefault.fx" || sh == "default.fx") f |= kSubmeshProxyShader;
    if (sm.collision) f |= kSubmeshHasCollisionTree;
    if (!fmt.known) f |= kSubmeshUnknownFormat;
    return f;
}

// D.4 / SPEC.md 2.5: the legacy converter is the one for 128-byte records (a stride accepted only
// in chunk 0x10005). A 0x10005 chunk holding 144-byte records is read as the 144-byte layout and
// keeps its colour. One predicate for both the colour override and legacy_vertex_chunks.
bool is_legacy_record(const alo::SubMesh& sm) { return sm.vertex_stride == 128; }

void build_submesh(const alo::Model& model, const alo::Mesh& mesh, const alo::SubMesh& sm, std::uint32_t mesh_index,
                   std::uint32_t sub_index, std::uint32_t attach_bone, const ModelLoadOptions& options, Submesh& out,
                   std::vector<std::string>& warnings) {
    const auto& vb = sm.vertices;
    const std::size_t n = vb.size();
    out.mesh = mesh_index;
    out.index_in_mesh = sub_index;
    out.vertex_format = sm.vertex_format;
    out.material = convert_material(sm.material);

    // D.2: the effect's _ALAMO_VERTEX_TYPE replaces the ALO's format string; the stored records are
    // then packed into that type's layout.
    alo::VertexFormatInfo fmt = sm.format;
    out.effective_vertex_format = sm.vertex_format;
    if (options.vertex_type_override) {
        const std::string ov = options.vertex_type_override(out.material);
        if (!ov.empty() && !iequals(ov, sm.vertex_format)) {
            const alo::VertexFormatInfo oi = alo::vertex_format_info(ov);
            if (!oi.known) {
                warnings.push_back(mesh.name + "[" + std::to_string(sub_index) + "]: unknown override vertex type " + ov +
                                   " ignored");
            } else {
                if (oi.skinned != sm.format.skinned)
                    warnings.push_back(mesh.name + "[" + std::to_string(sub_index) + "]: vertex type override " +
                                       sm.vertex_format + " -> " + ov + " changes skinning");
                fmt = oi;
                out.effective_vertex_format = ov;
                out.vertex_type_overridden = true;
            }
        }
    }
    out.format = {fmt.known, fmt.normal, fmt.uv_sets, fmt.tangent_frame, fmt.color, fmt.skinned, fmt.grass};
    out.flags = submesh_flags(mesh, sm, fmt);
    out.vertex_count = static_cast<std::uint32_t>(n);
    const bool skinned = fmt.skinned && !sm.bone_map.empty();
    if (fmt.skinned && sm.bone_map.empty())
        warnings.push_back(mesh.name + "[" + std::to_string(sub_index) + "]: skinned format without bone map, loaded as static");
    out.layout = skinned ? VertexLayout::Skinned : VertexLayout::Static;
    if (skinned) out.flags |= kSubmeshSkinned;
    out.attach_bone = skinned ? 0 : attach_bone;

    const std::size_t bones = model.skeleton.size();
    if (skinned) {
        out.bone_palette.reserve(sm.bone_map.size());
        for (std::uint32_t b : sm.bone_map) {
            if (b >= bones || b > 0xFFFF) {
                warnings.push_back(mesh.name + ": bone map entry " + std::to_string(b) + " out of range, using root");
                b = 0;
            }
            out.bone_palette.push_back(static_cast<std::uint16_t>(b));
        }
    }

    const auto has = [&](const auto& v) { return v.size() == n; };
    const bool has_n = has(vb.normal), has_uv = has(vb.uv[0]), has_t = has(vb.tangent), has_b = has(vb.binormal);
    // D.4: the legacy 128-byte (0x10005) record converter overwrites both colours with 1.0, so the
    // stored colour of those records never reaches the GPU (W_STARS_BACK02 draws at full
    // brightness). 0x10007 (144-byte) records keep their stored colour when the format has C.
    const bool legacy_record = is_legacy_record(sm);
    const bool has_c = has(vb.color) && out.format.color && !legacy_record;
    const bool has_bi = has(vb.bone_index);

    // D.5: the engine never builds a tangent frame. Formats with a frame get the stored vectors
    // verbatim (zero stays zero); formats without one get zero. Generation / repair only on request.
    std::vector<Float3> pos(n), nrm(n), tan(n, Float3{0, 0, 0}), bin(n, Float3{0, 0, 0});
    std::vector<Float2> uvs(n);
    for (std::size_t i = 0; i < n; ++i) {
        pos[i] = to_f3(vb.position[i]);
        nrm[i] = has_n ? to_f3(vb.normal[i]) : Float3{0, 0, 0};
        uvs[i] = has_uv ? Float2{vb.uv[0][i].x, vb.uv[0][i].y} : Float2{0, 0};
    }
    out.has_tangents = fmt.tangent_frame && has_t && has_b;
    if (out.has_tangents) {
        for (std::size_t i = 0; i < n; ++i) {
            tan[i] = to_f3(vb.tangent[i]);
            bin[i] = to_f3(vb.binormal[i]);
        }
        // Only guard: a non-finite stored vector becomes zero (none in shipped data; keeps NaN out
        // of the shaders, which handle zero).
        std::size_t nonfinite = 0;
        for (std::size_t i = 0; i < n; ++i) {
            for (Float3* v : {&tan[i], &bin[i]})
                if (!std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z)) {
                    *v = Float3{0, 0, 0};
                    ++nonfinite;
                }
        }
        if (nonfinite)
            warnings.push_back(mesh.name + "[" + std::to_string(sub_index) + "]: " + std::to_string(nonfinite) +
                               " non-finite tangent/binormal vectors set to zero");
        if (options.generate_tangent_frames) repair_tangent_frame(nrm, tan, bin);
    } else if (options.generate_tangent_frames) {
        const bool uv_ok = has_uv && fmt.uv_sets > 0;
        compute_tangent_frame(pos, nrm, uv_ok ? std::span<const Float2>(uvs) : std::span<const Float2>(), sm.indices, tan, bin);
    }

    std::size_t blended = 0;
    out.vertices.assign(n * vertex_stride(out.layout), 0);
    std::uint8_t* dst = out.vertices.data();
    for (std::size_t i = 0; i < n; ++i, dst += 64) {
        const Float3& p = pos[i];
        const Float3& nr = nrm[i];
        const Float2& uv = uvs[i];
        const Float3& tg = tan[i];
        const Float3& bn = bin[i];
        std::uint8_t col[4] = {255, 255, 255, 255};
        if (has_c) {
            const auto& c = vb.color[i];
            col[0] = unorm8(c.x); col[1] = unorm8(c.y); col[2] = unorm8(c.z); col[3] = unorm8(c.w);
        }
        if (skinned) {
            SkinnedVertex v{};
            v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
            // Rigid: one bone per vertex. RSkin (bone_influences = 1) uses bone_index[0] only; the
            // parser range-checks only that index, the others may hold anything. B4I4 formats
            // (bone_influences = 4, legacy EaW files) carry 4 weights and every non-zero-weight
            // index is range-checked; take the heaviest (shipped data has weights 1,0,0,0).
            std::uint32_t local = has_bi ? vb.bone_index[i][0] : 0;
            if (has_bi && fmt.bone_influences >= 4 && has(vb.bone_weight)) {
                const auto& w = vb.bone_weight[i];
                const float ws[4] = {w.x, w.y, w.z, w.w};
                int best = 0;
                for (int k = 1; k < 4; ++k)
                    if (ws[k] > ws[best]) best = k;
                if (ws[best] < 0.999f) ++blended;
                local = vb.bone_index[i][static_cast<std::size_t>(best)];
            }
            if (local >= out.bone_palette.size()) local = 0;  // parser already range-checks
            v.normal[0] = nr.x; v.normal[1] = nr.y; v.normal[2] = nr.z;
            v.normal[3] = static_cast<float>(local);
            v.uv0[0] = uv.x; v.uv0[1] = uv.y;
            v.tangent[0] = tg.x; v.tangent[1] = tg.y; v.tangent[2] = tg.z;
            v.binormal[0] = bn.x; v.binormal[1] = bn.y; v.binormal[2] = bn.z;
            std::memcpy(v.color, col, 4);
            std::memcpy(dst, &v, sizeof v);
        } else {
            StaticVertex v{};
            v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
            v.normal[0] = nr.x; v.normal[1] = nr.y; v.normal[2] = nr.z;
            v.uv0[0] = uv.x; v.uv0[1] = uv.y;
            v.tangent[0] = tg.x; v.tangent[1] = tg.y; v.tangent[2] = tg.z;
            v.binormal[0] = bn.x; v.binormal[1] = bn.y; v.binormal[2] = bn.z;
            std::memcpy(v.color, col, 4);
            std::memcpy(dst, &v, sizeof v);
        }
    }
    if (blended)
        warnings.push_back(mesh.name + "[" + std::to_string(sub_index) + "]: " + std::to_string(blended) +
                           " vertices with blended weights reduced to their heaviest bone");
    // Indices: the ALO stores u16 triangle lists. Widen to u32 only if the vertex count needs it.
    out.index_count = static_cast<std::uint32_t>(sm.indices.size());
    if (sm.indices.size() % 3 != 0)
        warnings.push_back(mesh.name + "[" + std::to_string(sub_index) + "]: index count not a multiple of 3");
    if (n > 0xFFFF) {
        out.index_type = IndexType::U32;
        out.indices.resize(sm.indices.size() * 4);
        for (std::size_t i = 0; i < sm.indices.size(); ++i) {
            const std::uint32_t v = sm.indices[i];
            std::memcpy(out.indices.data() + i * 4, &v, 4);
        }
    } else {
        out.index_type = IndexType::U16;
        out.indices.resize(sm.indices.size() * 2);
        if (!sm.indices.empty()) std::memcpy(out.indices.data(), sm.indices.data(), out.indices.size());
    }
    bounds_of(vb.position, out.bounds, out.sphere);
}

}  // namespace

const MaterialParam* MaterialRef::find(std::string_view n) const {
    for (const auto& p : params)
        if (iequals(p.name, n)) return &p;
    return nullptr;
}

std::vector<std::string> ModelAsset::texture_names() const {
    std::vector<std::string> out;
    for (const auto& sm : submeshes)
        for (const auto& p : sm.material.params)
            if (p.type == ParamType::Texture && !p.texture.empty()) out.push_back(p.texture);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string na = normalize_name(a), nb = normalize_name(b);
        return na != nb ? na < nb : a < b;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return iequals(a, b); }),
              out.end());
    return out;
}

bool is_particle_alo(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4) return false;
    std::uint32_t id = 0;
    std::memcpy(&id, bytes.data(), 4);
    return id == 0x900;
}

bool build_model(std::span<const std::uint8_t> bytes, std::string_view name, ModelAsset& out, std::string* error) {
    return build_model(bytes, name, out, ModelLoadOptions{}, error);
}

bool build_model(std::span<const std::uint8_t> bytes, std::string_view name, ModelAsset& out,
                 const ModelLoadOptions& options, std::string* error) {
    try {
        if (is_particle_alo(bytes)) {
            if (error) *error = "particle system ALO, not a model";
            return false;
        }
        const alo::AloFile file = alo::parse(bytes);
        if (file.kind != alo::AloFile::Kind::Model) {
            if (error) *error = "particle system ALO, not a model";
            return false;
        }
        const alo::Model& model = file.model;
        ModelAsset m;
        m.name = std::string(name);
        for (const auto& mesh : model.meshes)
            for (const auto& sm : mesh.submeshes)
                if (is_legacy_record(sm)) ++m.legacy_vertex_chunks;

        const auto& sk = model.skeleton;
        m.skeleton.names = sk.names;
        m.skeleton.parents = sk.parents;
        m.skeleton.visible = sk.visible;
        m.skeleton.billboard_mode = sk.billboard_mode;
        m.skeleton.bind_local.reserve(sk.local.size());
        for (const auto& l : sk.local) m.skeleton.bind_local.push_back(to_mat(l));
        if (m.skeleton.names.empty()) {
            // Defensive: every shipped model has Root; keep the invariant "bone 0 exists".
            m.skeleton.names.push_back("Root");
            m.skeleton.parents.push_back(-1);
            m.skeleton.bind_local.push_back(Mat3x4::identity());
            m.warnings.push_back("model has no skeleton; added identity Root");
        }
        m.skeleton.finalize();

        Aabb total{{1, 1, 1}, {-1, -1, -1}};
        m.meshes.reserve(model.meshes.size());
        for (std::uint32_t mi = 0; mi < model.meshes.size(); ++mi) {
            const auto& mesh = model.meshes[mi];
            MeshInfo info;
            info.name = mesh.name;
            info.file_bounds = {to_f3(mesh.bounds.min), to_f3(mesh.bounds.max)};
            info.hidden = mesh.hidden;
            info.collision_enabled = mesh.collision_enabled;
            info.lod = lod_of(mesh.name);
            info.attach_bone = model.mesh_bone(mi);
            if (info.attach_bone >= m.skeleton.size()) {
                m.warnings.push_back(mesh.name + ": attach bone out of range, using root");
                info.attach_bone = 0;
            }
            info.first_submesh = static_cast<std::uint32_t>(m.submeshes.size());
            info.submesh_count = static_cast<std::uint32_t>(mesh.submeshes.size());
            for (std::uint32_t si = 0; si < mesh.submeshes.size(); ++si) {
                Submesh s;
                build_submesh(model, mesh, mesh.submeshes[si], mi, si, info.attach_bone, options, s, m.warnings);
                if (s.is_render_geometry() && s.bounds.valid()) {
                    const Aabb mb = s.skinned() ? s.bounds : transform_aabb(m.skeleton.bind_world[s.attach_bone], s.bounds);
                    total = merge(total, mb);
                }
                m.submeshes.push_back(std::move(s));
            }
            m.meshes.push_back(std::move(info));
        }
        m.bounds = total;
        if (total.valid()) {
            m.sphere.center = {(total.min.x + total.max.x) * 0.5f, (total.min.y + total.max.y) * 0.5f,
                               (total.min.z + total.max.z) * 0.5f};
            const float dx = total.max.x - m.sphere.center.x, dy = total.max.y - m.sphere.center.y,
                        dz = total.max.z - m.sphere.center.z;
            m.sphere.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        m.proxies.reserve(model.proxies.size());
        for (const auto& p : model.proxies) m.proxies.push_back({p.name, p.bone, p.hidden});
        out = std::move(m);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool load_model(const FileProvider& files, std::string_view path, ModelAsset& out, std::string* error) {
    return load_model(files, path, out, ModelLoadOptions{}, error);
}

bool load_model(const FileProvider& files, std::string_view path, ModelAsset& out, const ModelLoadOptions& options,
                std::string* error) {
    const std::string p = model_path(path);
    std::vector<std::uint8_t> bytes;
    if (!files.read(p, bytes)) {
        if (error) *error = "not found: " + p;
        return false;
    }
    return build_model(bytes, p, out, options, error);
}

std::string model_path(std::string_view name) { return with_default_dir_ext(name, "Data\\Art\\Models\\", ".alo"); }
std::string animation_path(std::string_view name) { return with_default_dir_ext(name, "Data\\Art\\Models\\", ".ala"); }

// ---- tangent frames ---------------------------------------------------------------------------

namespace {

float dot3(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 cross3(const Float3& a, const Float3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Float3 sub3(const Float3& a, const Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 scale3(const Float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float len3(const Float3& a) { return std::sqrt(dot3(a, a)); }
bool finite3(const Float3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// Unit normal, or +Z for zero / non-finite normals (shadow-volume fin vertices).
Float3 safe_normal(const Float3& n) {
    const float l = len3(n);
    if (!(l > 1e-12f) || !finite3(n)) return {0, 0, 1};
    return scale3(n, 1.f / l);
}

// Any orthonormal (t, b) with b = n x t.
void basis_from_normal(const Float3& n_in, Float3& t, Float3& b) {
    const Float3 n = safe_normal(n_in);
    const Float3 a = std::fabs(n.x) < 0.9f ? Float3{1, 0, 0} : Float3{0, 1, 0};
    t = sub3(a, scale3(n, dot3(a, n)));
    t = scale3(t, 1.f / len3(t));
    b = cross3(n, t);
}

// Orthonormalise (t, b) against n; b keeps the handedness of the input b.
bool orthonormalise(const Float3& n_in, Float3& t, Float3& b) {
    const Float3 n = safe_normal(n_in);
    Float3 tt = sub3(t, scale3(n, dot3(n, t)));
    const float lt = len3(tt);
    if (!(lt > 1e-12f) || !finite3(tt)) return false;
    tt = scale3(tt, 1.f / lt);
    Float3 bb = cross3(n, tt);
    if (dot3(bb, b) < 0) bb = scale3(bb, -1.f);
    t = tt;
    b = bb;
    return true;
}

}  // namespace

void compute_tangent_frame(std::span<const Float3> pos, std::span<const Float3> nrm, std::span<const Float2> uv,
                           std::span<const std::uint16_t> indices, std::span<Float3> tangent, std::span<Float3> binormal) {
    const std::size_t n = std::min({pos.size(), nrm.size(), tangent.size(), binormal.size()});
    for (std::size_t i = 0; i < n; ++i) tangent[i] = binormal[i] = Float3{0, 0, 0};
    if (uv.size() >= n) {
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            const std::size_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
            if (i0 >= n || i1 >= n || i2 >= n) continue;
            const Float3 e1 = sub3(pos[i1], pos[i0]), e2 = sub3(pos[i2], pos[i0]);
            const float du1 = uv[i1].x - uv[i0].x, dv1 = uv[i1].y - uv[i0].y;
            const float du2 = uv[i2].x - uv[i0].x, dv2 = uv[i2].y - uv[i0].y;
            const float det = du1 * dv2 - du2 * dv1;
            if (!(std::fabs(det) > 1e-20f) || !std::isfinite(det)) continue;
            // Unnormalised dP/du, dP/dv scaled by |det| (UV-area weighting cancels out of the
            // direction; triangle area weighting comes from the edge lengths).
            const float s = det > 0 ? 1.f : -1.f;
            const Float3 T = scale3(sub3(scale3(e1, dv2), scale3(e2, dv1)), s);
            const Float3 B = scale3(sub3(scale3(e2, du1), scale3(e1, du2)), s);
            if (!finite3(T) || !finite3(B)) continue;
            for (std::size_t k : {i0, i1, i2}) {
                tangent[k] = {tangent[k].x + T.x, tangent[k].y + T.y, tangent[k].z + T.z};
                binormal[k] = {binormal[k].x + B.x, binormal[k].y + B.y, binormal[k].z + B.z};
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!orthonormalise(nrm[i], tangent[i], binormal[i])) basis_from_normal(nrm[i], tangent[i], binormal[i]);
    }
}

void repair_tangent_frame(std::span<const Float3> nrm, std::span<Float3> tangent, std::span<Float3> binormal) {
    const std::size_t n = std::min({nrm.size(), tangent.size(), binormal.size()});
    for (std::size_t i = 0; i < n; ++i) {
        const float lt = len3(tangent[i]), lb = len3(binormal[i]);
        if (lt > 1e-6f && lb > 1e-6f && finite3(tangent[i]) && finite3(binormal[i])) continue;
        Float3 t = tangent[i], b = binormal[i];
        if (lt > 1e-6f && finite3(t) && orthonormalise(nrm[i], t, b)) {
            tangent[i] = t;
            binormal[i] = b;
        } else {
            basis_from_normal(nrm[i], tangent[i], binormal[i]);
        }
    }
}

// ---- vertex attribute tables ------------------------------------------------------------------

namespace {
constexpr VertexAttribute kStaticAttrs[] = {
    {VertexSemantic::Position, AttribFormat::Float3, 0},  {VertexSemantic::Normal, AttribFormat::Float3, 12},
    {VertexSemantic::TexCoord0, AttribFormat::Float2, 24}, {VertexSemantic::Tangent, AttribFormat::Float3, 32},
    {VertexSemantic::Binormal, AttribFormat::Float3, 44},  {VertexSemantic::Color, AttribFormat::UNorm8x4, 56},
};
constexpr VertexAttribute kSkinnedAttrs[] = {
    {VertexSemantic::Position, AttribFormat::Float3, 0},  {VertexSemantic::Normal, AttribFormat::Float4, 12},
    {VertexSemantic::TexCoord0, AttribFormat::Float2, 28}, {VertexSemantic::Tangent, AttribFormat::Float3, 36},
    {VertexSemantic::Binormal, AttribFormat::Float3, 48},  {VertexSemantic::Color, AttribFormat::UNorm8x4, 60},
};
}  // namespace

std::span<const VertexAttribute> vertex_attributes(VertexLayout layout) {
    if (layout == VertexLayout::Skinned) return kSkinnedAttrs;
    return kStaticAttrs;
}

}  // namespace alamo::assets
