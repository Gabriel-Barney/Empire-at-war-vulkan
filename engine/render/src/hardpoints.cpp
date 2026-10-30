#include "alamo/render/hardpoints.hpp"

#include <algorithm>
#include <cctype>

#include "alamo/data/xml.hpp"
#include "alamo/render/lod.hpp"

namespace alamo::render {

namespace {
bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
void sort_unique(std::vector<uint32_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
// XML bool: yes / true / 1 (case-insensitive first letter), no / false / 0; `fallback` when empty / other.
bool parse_bool(std::string_view s, bool fallback) {
    if (s.empty()) return fallback;
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (c == 'Y' || c == 'T' || c == '1') return true;
    if (c == 'N' || c == 'F' || c == '0') return false;
    return fallback;
}
// Case-insensitive bone lookup (-1 when empty / absent).
int32_t find_bone(const assets::Skeleton& sk, std::string_view name) {
    if (name.empty()) return -1;
    for (size_t b = 0; b < sk.names.size(); ++b)
        if (iequal(sk.names[b], name)) return static_cast<int32_t>(b);
    return -1;
}
}  // namespace

std::string_view HardpointDef::tag(std::string_view tag_name) const {
    for (const auto& [k, v] : tags)
        if (iequal(k, tag_name)) return v;
    return {};
}

bool HardpointDef::has_tag(std::string_view tag_name) const {
    for (const auto& kv : tags)
        if (iequal(kv.first, tag_name)) return true;
    return false;
}

bool HardpointDef::dummy_art() const { return iequal(type, "HARD_POINT_DUMMY_ART"); }

HardpointDef parse_hardpoint(const data::XmlNode& hp) {
    HardpointDef d;
    d.name = std::string(hp.attribute("Name"));
    d.known = true;
    for (data::XmlNode c : hp.children()) d.tags.emplace_back(std::string(c.name()), std::string(c.text_trimmed()));
    d.type = std::string(d.tag("Type"));
    d.attachment_bone = std::string(d.tag("Attachment_Bone"));
    d.damage_decal = std::string(d.tag("Damage_Decal"));
    d.damage_particles = std::string(d.tag("Damage_Particles"));
    d.engine_particles = std::string(d.tag("Engine_Particles"));
    d.model_to_attach = std::string(d.tag("Model_To_Attach"));
    d.hide_engine_particles = parse_bool(d.tag("Engine_Death_Hide_Engine_Particles"), true);
    d.destroyable = parse_bool(d.tag("Is_Destroyable"), true);
    return d;
}

MeshBoneMap::MeshBoneMap(const assets::ModelAsset& model) {
    const size_t nb = model.skeleton.size();
    parent_.resize(nb, -1);
    for (size_t b = 0; b < nb && b < model.skeleton.parents.size(); ++b) {
        const int32_t p = model.skeleton.parents[b];
        parent_[b] = (p >= 0 && static_cast<size_t>(p) < b) ? p : -1;  // parent < own index (loader-verified)
    }
    mesh_bone_.reserve(model.meshes.size());
    for (const assets::MeshInfo& m : model.meshes)
        mesh_bone_.push_back(m.attach_bone < nb ? static_cast<int32_t>(m.attach_bone) : -1);
}

bool MeshBoneMap::in_subtree(int32_t bone, int32_t root) const {
    if (root <= 0 || bone <= 0 || static_cast<size_t>(root) >= parent_.size() || static_cast<size_t>(bone) >= parent_.size()) return false;
    for (int32_t b = bone; b > 0; b = parent_[static_cast<size_t>(b)])  // parents strictly decrease: terminates
        if (b == root) return true;
    return false;
}

std::vector<uint32_t> MeshBoneMap::meshes_on_bone(int32_t bone) const {
    std::vector<uint32_t> out;
    if (bone < 0) return out;
    for (uint32_t i = 0; i < mesh_bone_.size(); ++i)
        if (mesh_bone_[i] == bone) out.push_back(i);
    return out;
}

std::vector<uint32_t> MeshBoneMap::meshes_in_subtree(int32_t root) const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < mesh_bone_.size(); ++i)
        if (in_subtree(mesh_bone_[i], root)) out.push_back(i);
    return out;
}

std::vector<uint32_t> meshes_named(const assets::ModelAsset& model, std::string_view name) {
    std::vector<uint32_t> out;
    if (name.empty()) return out;
    for (uint32_t i = 0; i < model.meshes.size(); ++i)
        if (iequal(model.meshes[i].name, name)) out.push_back(i);
    return out;
}

namespace {
// Sub-object bones of the layout (meshes, proxies, attachments).
std::vector<int32_t> sub_object_bones(const assets::ModelAsset& model, const HardpointLayout& l) {
    const size_t nb = model.skeleton.size();
    std::vector<int32_t> bones;
    bones.reserve(l.sub_objects());
    for (const assets::MeshInfo& m : model.meshes) bones.push_back(m.attach_bone < nb ? static_cast<int32_t>(m.attach_bone) : -1);
    for (const assets::Proxy& p : model.proxies) bones.push_back(p.bone < nb ? static_cast<int32_t>(p.bone) : -1);
    for (uint32_t h = 0; h < l.hardpoints; ++h) bones.push_back(l.attach_model[h].empty() ? -1 : l.attach_bone[h]);
    return bones;
}
// First sub-object named `name` (meshes, then proxies; B.10 answer 6), -1 if none.
int64_t first_named(const assets::ModelAsset& model, std::string_view name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < model.meshes.size(); ++i)
        if (iequal(model.meshes[i].name, name)) return static_cast<int64_t>(i);
    for (size_t p = 0; p < model.proxies.size(); ++p)
        if (iequal(model.proxies[p].name, name)) return static_cast<int64_t>(model.meshes.size() + p);
    return -1;
}
std::vector<uint32_t> subtree(const MeshBoneMap& bm, const std::vector<int32_t>& bones, int32_t root) {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < bones.size(); ++i)
        if (bm.in_subtree(bones[i], root)) out.push_back(i);
    return out;
}
}  // namespace

HardpointLayout build_hardpoint_layout(const assets::ModelAsset& model, std::span<const HardpointDef> hardpoints, bool show_damage) {
    HardpointLayout l;
    l.meshes = static_cast<uint32_t>(model.meshes.size());
    l.proxies = static_cast<uint32_t>(model.proxies.size());
    l.hardpoints = static_cast<uint32_t>(hardpoints.size());
    for (const HardpointDef& d : hardpoints) {
        l.attach_model.push_back(d.model_to_attach);
        l.attach_bone.push_back(find_bone(model.skeleton, d.attachment_bone));
    }
    const MeshBoneMap bm(model);
    const std::vector<int32_t> bones = sub_object_bones(model, l);
    // Type setup: ALO flags, then the Damage_Decals and the state meshes.
    l.base.assign(l.sub_objects(), 0);
    for (uint32_t i = 0; i < l.meshes; ++i)
        l.base[i] = (model.meshes[i].hidden || (!show_damage && is_state_mesh(model.meshes[i].name))) ? 1 : 0;
    for (uint32_t p = 0; p < l.proxies; ++p) l.base[l.proxy_index(p)] = model.proxies[p].hidden ? 1 : 0;
    l.effects.resize(hardpoints.size());
    for (size_t h = 0; h < hardpoints.size(); ++h) {
        const HardpointDef& d = hardpoints[h];
        const int64_t decal = first_named(model, d.damage_decal);
        const std::vector<uint32_t> dmg = subtree(bm, bones, find_bone(model.skeleton, d.damage_particles));
        if (!show_damage) {
            if (decal >= 0) l.base[static_cast<size_t>(decal)] = 1;       // type setup
            for (uint32_t s : dmg) l.base[s] = 1;                          // instance creation (HardPoints order)
        }
        if (d.dummy_art() || !d.destroyable) continue;                     // never changes visuals
        HardpointEffect& e = l.effects[h];
        if (decal >= 0) e.show.push_back(static_cast<uint32_t>(decal));
        e.show.insert(e.show.end(), dmg.begin(), dmg.end());
        if (d.hide_engine_particles) e.hide = subtree(bm, bones, find_bone(model.skeleton, d.engine_particles));
        sort_unique(e.show);
        sort_unique(e.hide);
    }
    return l;
}

HardpointLayout fallback_hardpoint_layout(const assets::ModelAsset& model, std::span<const uint8_t> mesh_mask, size_t hardpoints) {
    HardpointLayout l;
    l.meshes = static_cast<uint32_t>(model.meshes.size());
    l.proxies = static_cast<uint32_t>(model.proxies.size());
    l.hardpoints = static_cast<uint32_t>(hardpoints);
    l.base.assign(l.sub_objects(), 0);
    for (uint32_t i = 0; i < l.meshes && i < mesh_mask.size(); ++i) l.base[i] = mesh_mask[i];
    for (uint32_t p = 0; p < l.proxies; ++p) l.base[l.proxy_index(p)] = model.proxies[p].hidden ? 1 : 0;
    l.effects.resize(hardpoints);
    l.attach_bone.assign(hardpoints, -1);
    l.attach_model.assign(hardpoints, std::string());
    return l;
}

std::vector<uint8_t> base_hidden_mask(const assets::ModelAsset& model, std::span<const HardpointDef> hardpoints, bool show_damage) {
    const HardpointLayout l = build_hardpoint_layout(model, hardpoints, show_damage);
    return std::vector<uint8_t>(l.base.begin(), l.base.begin() + l.meshes);
}

assets::Mat3x4 attachment_transform(const assets::ModelAsset& owner, const assets::Mat3x4& owner_transform,
                                    std::span<const assets::Mat3x4> bone_world, int32_t bone) {
    if (bone < 0) return owner_transform;
    const size_t b = static_cast<size_t>(bone);
    if (b < bone_world.size()) return owner_transform * bone_world[b];
    if (bone_world.empty() && b < owner.skeleton.bind_world.size()) return owner_transform * owner.skeleton.bind_world[b];
    return owner_transform;
}

void HardpointVisibility::reset(size_t hardpoint_count) {
    hardpoint_count_ = hardpoint_count;
    order_.clear();
    overrides_.clear();
    mask_.clear();
    layout_.reset();
    ready_ = false;
}

void HardpointVisibility::init(HardpointLayoutPtr layout) {
    layout_ = std::move(layout);
    ready_ = layout_ != nullptr;
    const size_t n = layout_ ? layout_->base.size() : 0;
    overrides_.assign(n, 0);
    mask_.assign(n, 0);
    recompute();
}

bool HardpointVisibility::set_destroyed(size_t hp, bool d) {
    if (hp >= hardpoint_count_) return false;
    const auto it = std::find(order_.begin(), order_.end(), static_cast<uint32_t>(hp));
    if (d == (it != order_.end())) return true;  // no change (destroying twice is a no-op)
    if (d) order_.push_back(static_cast<uint32_t>(hp));
    else order_.erase(it);  // debug reset
    if (ready_) recompute();
    return true;
}

bool HardpointVisibility::destroyed(size_t hp) const {
    return std::find(order_.begin(), order_.end(), static_cast<uint32_t>(hp)) != order_.end();
}

bool HardpointVisibility::set_override(uint32_t s, MeshOverride o) {
    if (!ready_ || s >= overrides_.size()) return false;
    if (overrides_[s] != static_cast<uint8_t>(o)) {
        overrides_[s] = static_cast<uint8_t>(o);
        recompute();
    }
    return true;
}

bool HardpointVisibility::attachment_visible(uint32_t h) const {
    if (!layout_ || h >= layout_->hardpoints || layout_->attach_model[h].empty()) return false;
    return !hidden(layout_->attachment_index(h));
}

void HardpointVisibility::recompute() {
    if (!layout_) return;
    // In place: mask_ keeps its size (and storage) from init().
    std::copy(layout_->base.begin(), layout_->base.end(), mask_.begin());
    for (uint32_t h : order_) {
        if (h >= layout_->effects.size()) continue;
        const HardpointEffect& e = layout_->effects[h];
        for (uint32_t s : e.show)
            if (s < mask_.size()) mask_[s] = 0;
        for (uint32_t s : e.hide)
            if (s < mask_.size()) mask_[s] = 1;
    }
    for (size_t i = 0; i < mask_.size(); ++i) {
        if (overrides_[i] == static_cast<uint8_t>(MeshOverride::Show)) mask_[i] = 0;
        else if (overrides_[i] == static_cast<uint8_t>(MeshOverride::Hide)) mask_[i] = 1;
    }
}

}  // namespace alamo::render
