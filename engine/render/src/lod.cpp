#include "alamo/render/lod.hpp"

#include <algorithm>
#include <cctype>

namespace alamo::render {

int max_lod(std::span<const assets::MeshInfo> meshes) {
    int m = -1;
    for (const auto& mi : meshes) m = std::max(m, mi.lod);
    return m;
}

int choose_lod(int max_lod_index, float detail) {
    if (max_lod_index < 0) return -1;
    const float d = std::clamp(detail, 0.0f, 1.0f);
    const int c = static_cast<int>(static_cast<float>(max_lod_index + 1) * d);
    return std::clamp(c, 0, max_lod_index);
}

int mesh_alt_index(std::string_view n) {
    for (size_t i = 0; i + 5 <= n.size(); ++i) {
        if (n[i] == '_' && std::toupper(static_cast<unsigned char>(n[i + 1])) == 'A' &&
            std::toupper(static_cast<unsigned char>(n[i + 2])) == 'L' &&
            std::toupper(static_cast<unsigned char>(n[i + 3])) == 'T' && std::isdigit(static_cast<unsigned char>(n[i + 4])))
            return n[i + 4] - '0';
    }
    return -1;
}

namespace {
// Case-insensitive substring; `w` upper-case.
bool contains_upper(std::string_view n, std::string_view w) {
    for (size_t i = 0; i + w.size() <= n.size(); ++i) {
        bool eq = true;
        for (size_t j = 0; j < w.size() && eq; ++j) eq = std::toupper(static_cast<unsigned char>(n[i + j])) == w[j];
        if (eq) return true;
    }
    return false;
}
bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
}  // namespace

bool is_state_mesh(std::string_view n) {
    return contains_upper(n, "STEALTH") || contains_upper(n, "BUILDUP") || contains_upper(n, "CHARGED");
}

bool fallback_damage_decal_rule(std::string_view n) { return contains_upper(n, "_BLAST"); }

bool default_hidden_mesh_rule(std::string_view n) { return fallback_damage_decal_rule(n) || is_state_mesh(n); }

std::vector<uint8_t> hidden_mesh_mask(const assets::ModelAsset& model, std::span<const std::string> decals) {
    std::vector<uint8_t> mask(model.meshes.size(), 0);
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const std::string& n = model.meshes[i].name;
        bool h = model.meshes[i].hidden || is_state_mesh(n);
        for (const std::string& d : decals) h = h || iequal(n, d);
        mask[i] = h ? 1 : 0;
    }
    return mask;
}

std::vector<uint8_t> hidden_mesh_mask(const assets::ModelAsset& model, DamageDecalRule rule) {
    std::vector<uint8_t> mask(model.meshes.size(), 0);
    for (size_t i = 0; i < model.meshes.size(); ++i)
        mask[i] = (model.meshes[i].hidden || (rule && rule(model.meshes[i].name))) ? 1 : 0;
    return mask;
}

uint32_t set_hardpoint_destroyed(std::vector<uint8_t>& mask, const assets::ModelAsset& model, std::string_view decal, bool destroyed) {
    uint32_t changed = 0;
    if (mask.size() < model.meshes.size()) mask.resize(model.meshes.size(), 0);
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        if (decal.empty() || !iequal(model.meshes[i].name, decal)) continue;
        const uint8_t v = destroyed ? 0 : 1;
        changed += mask[i] != v ? 1u : 0u;
        mask[i] = v;
    }
    return changed;
}

MeshSelection select_meshes(const assets::ModelAsset& model, float detail, int current_alt, bool show_damage,
                            DamageDecalRule rule) {
    MeshSelection s;
    s.lod = choose_lod(max_lod(model.meshes), detail);
    s.alt = current_alt;
    s.mesh_visible.resize(model.meshes.size(), 0);
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const assets::MeshInfo& m = model.meshes[i];
        const int alt = mesh_alt_index(m.name);
        const bool lod_ok = m.lod < 0 || m.lod == s.lod;
        const bool alt_ok = alt < 0 || alt == current_alt;
        const bool damage = !show_damage && rule && rule(m.name);
        if (damage && lod_ok && alt_ok && !m.hidden) ++s.damage_hidden;
        s.mesh_visible[i] = lod_ok && alt_ok && !m.hidden && !damage ? 1 : 0;
    }
    return s;
}

MeshSelection select_meshes_all_lods(const assets::ModelAsset& model, int current_alt, bool show_damage, DamageDecalRule rule) {
    const bool any_alt = current_alt == kAnyAlt;
    MeshSelection s = select_meshes(model, 1.0f, any_alt ? 0 : current_alt, show_damage, rule);
    s.damage_hidden = 0;
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const assets::MeshInfo& m = model.meshes[i];
        const int alt = mesh_alt_index(m.name);
        const bool damage = !show_damage && rule && rule(m.name);
        const bool ok = (any_alt || alt < 0 || alt == current_alt) && !m.hidden;
        if (damage && ok) ++s.damage_hidden;
        s.mesh_visible[i] = ok && !damage ? 1 : 0;
    }
    return s;
}

float screen_coverage(const float (&vp)[4][4], const assets::Aabb& b) {
    auto project = [&](float x, float y, float z, float& nx, float& ny) {
        const float cx = vp[0][0] * x + vp[0][1] * y + vp[0][2] * z + vp[0][3];
        const float cy = vp[1][0] * x + vp[1][1] * y + vp[1][2] * z + vp[1][3];
        float cw = vp[3][0] * x + vp[3][1] * y + vp[3][2] * z + vp[3][3];
        if (cw < 1e-4f) cw = 1e-4f;
        nx = cx / cw;
        ny = cy / cw;
    };
    const float c[3] = {0.5f * (b.min.x + b.max.x), 0.5f * (b.min.y + b.max.y), 0.5f * (b.min.z + b.max.z)};
    const float h[3] = {0.5f * (b.max.x - b.min.x), 0.5f * (b.max.y - b.min.y), 0.5f * (b.max.z - b.min.z)};
    float x0 = 0, y0 = 0;
    project(c[0], c[1], c[2], x0, y0);
    float best = 0;
    for (int axis = 0; axis < 3; ++axis) {
        float p[3] = {c[0], c[1], c[2]};
        p[axis] += h[axis];
        float x = 0, y = 0;
        project(p[0], p[1], p[2], x, y);
        best = std::max(best, (x - x0) * (x - x0) + (y - y0) * (y - y0));
    }
    return std::clamp(best * 10.995574f, 0.0f, 1.0f);
}

std::vector<uint32_t> drawable_submeshes(const assets::ModelAsset& model, const MeshSelection& sel, bool shadow_volumes,
                                         bool include_hidden) {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < model.submeshes.size(); ++i) {
        const assets::Submesh& sm = model.submeshes[i];
        const bool volume = shadow_volumes && (sm.flags & assets::kSubmeshShadowVolume) &&
                            !(sm.flags & (assets::kSubmeshCollisionShader | assets::kSubmeshProxyShader));
        if (!sm.is_render_geometry() && !volume) continue;
        if ((sm.flags & assets::kSubmeshHidden) && !(include_hidden && (sm.is_render_geometry() || volume))) continue;
        if (sm.mesh >= sel.mesh_visible.size() || !sel.mesh_visible[sm.mesh]) continue;
        if (sm.index_count == 0 || sm.vertex_count == 0) continue;
        out.push_back(i);
    }
    return out;
}

}  // namespace alamo::render
