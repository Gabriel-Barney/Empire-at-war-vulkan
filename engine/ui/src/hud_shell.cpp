#include "alamo/ui/hud_shell.hpp"

#include <algorithm>

#include "alamo/assets/model.hpp"

namespace alamo::ui {

int32_t shell_alt_index(std::string_view n) {
    // Last "_ALT" (any case) followed by digits only.
    for (size_t i = n.size(); i-- > 0;) {
        if (i + 4 > n.size()) continue;
        const std::string_view tag = n.substr(i, 4);
        const bool alt = tag[0] == '_' && (tag[1] == 'A' || tag[1] == 'a') && (tag[2] == 'L' || tag[2] == 'l') &&
                         (tag[3] == 'T' || tag[3] == 't');
        if (!alt) continue;
        const std::string_view digits = n.substr(i + 4);
        if (digits.empty() || digits.size() > 3) return -1;
        int32_t v = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    }
    return -1;
}

ShellModelInfo shell_model_info(const assets::ModelAsset& model) {
    ShellModelInfo info;
    const assets::Skeleton& sk = model.skeleton;
    std::vector<int32_t> first_mesh(sk.size(), -1);
    for (size_t m = 0; m < model.meshes.size(); ++m) {
        const assets::MeshInfo& mi = model.meshes[m];
        ShellMesh sm;
        sm.name = mi.name;
        sm.bone = mi.attach_bone;
        sm.hidden = mi.hidden;
        sm.alt = shell_alt_index(mi.name);
        sm.min = {mi.file_bounds.min.x, mi.file_bounds.min.y};
        sm.max = {mi.file_bounds.max.x, mi.file_bounds.max.y};
        if (mi.attach_bone < first_mesh.size() && first_mesh[mi.attach_bone] < 0)
            first_mesh[mi.attach_bone] = static_cast<int32_t>(m);
        info.meshes.push_back(std::move(sm));
    }
    for (size_t b = 0; b < sk.size(); ++b) {
        if (sk.parents[b] < 0) continue;  // the root
        ShellBone bone;
        bone.name = sk.names[b];
        const assets::Mat3x4& w = b < sk.bind_world.size() ? sk.bind_world[b] : assets::Mat3x4{};
        bone.x = w.m[0][3];
        bone.y = w.m[1][3];
        bone.z = w.m[2][3];
        if (first_mesh[b] >= 0) {
            const ShellMesh& sm = info.meshes[static_cast<size_t>(first_mesh[b])];
            if (sm.max.x >= sm.min.x && sm.max.y >= sm.min.y) {
                bone.half_size = {0.5f * (sm.max.x - sm.min.x), 0.5f * (sm.max.y - sm.min.y)};
                bone.rect_center = {0.5f * (sm.max.x + sm.min.x), 0.5f * (sm.max.y + sm.min.y)};
            }
        }
        info.bones.push_back(std::move(bone));
    }
    return info;
}

std::vector<uint8_t> shell_mesh_hidden(const ShellModelInfo& info, int32_t alt) {
    std::vector<uint8_t> out(info.meshes.size(), 0);
    for (size_t m = 0; m < info.meshes.size(); ++m) {
        const ShellMesh& sm = info.meshes[m];
        out[m] = static_cast<uint8_t>(sm.hidden || (sm.alt >= 0 && sm.alt != alt));
    }
    return out;
}

size_t ShellSet::load(const ComponentTable& table, const ModelLoader& loader, std::span<const std::string> names,
                      std::vector<std::string>* missing) {
    shells_.clear();
    if (!loader) return 0;
    for (uint32_t i = 0; i < table.size(); ++i) {
        const CommandBarComponent& c = table[i];
        if (c.type != ComponentType::Shell || c.model_name.empty()) continue;
        if (!names.empty()) {
            bool wanted = false;
            for (const std::string& n : names) wanted = wanted || table.find(n) == static_cast<int32_t>(i);
            if (!wanted) continue;
        }
        auto model = loader(c.model_name);
        if (!model) {
            if (missing) missing->push_back(c.model_name);
            continue;
        }
        Shell s;
        s.component = i;
        s.model_name = c.model_name;
        s.info = shell_model_info(*model);
        s.model = std::move(model);
        s.mesh_hidden = shell_mesh_hidden(s.info, s.alt);
        shells_.push_back(std::move(s));
    }
    return shells_.size();
}

void ShellSet::apply(HudLayout& layout) const {
    for (const Shell& s : shells_) {
        layout.set_shell_bones(s.component, s.info.bones);
        layout.set_shell_active(s.component, s.visible);
    }
}

void ShellSet::set_alt(uint32_t component, int32_t alt) {
    for (Shell& s : shells_)
        if (s.component == component && s.alt != alt) {
            s.alt = alt;
            s.mesh_hidden = shell_mesh_hidden(s.info, alt);
        }
}

void ShellSet::set_visible(uint32_t component, bool visible) {
    for (Shell& s : shells_)
        if (s.component == component) s.visible = visible;
}

const ShellSet::Shell* ShellSet::find(uint32_t component) const {
    for (const Shell& s : shells_)
        if (s.component == component) return &s;
    return nullptr;
}

}  // namespace alamo::ui
