// alamo_render_sim - R5 ability beams: style table from the type XML and SnapshotBeam -> world BeamInstance. See sim_beams.hpp.
#include "alamo/render/sim_beams.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"

namespace alamo::render {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

uint8_t kind_of_tag(std::string_view tag) {
    if (iequals(tag, "Tractor_Beam_Attack_Ability")) return kBeamTractor;
    if (iequals(tag, "Energy_Weapon_Attack_Ability")) return kBeamEnergyWeapon;
    if (iequals(tag, "Super_Laser_Ability")) return kBeamSuperLaser;
    if (iequals(tag, "Laser_Defense_Ability")) return kBeamLaserZap;
    if (iequals(tag, "Leech_Shields_Ability")) return kBeamLeech;
    return 0;
}

// "34, 34, 235, 255" -> rgba; false when fewer than three numbers.
bool rgba_of(std::string_view s, uint8_t (&out)[4]) {
    float f[4] = {0, 0, 0, 255};
    const size_t n = data::parse_floats(s, f, 4);
    if (n < 3) return false;
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(std::clamp(std::lround(f[i]), 0L, 255L));
    if (n < 4) out[3] = 255;
    return true;
}

float float_of(std::string_view s, float fallback) {
    float f = 0.0f;
    return data::parse_floats(s, &f, 1) == 1 && std::isfinite(f) ? f : fallback;
}

struct Fields {
    std::vector<std::pair<std::string_view, std::string_view>> kv;
    std::string_view get(std::string_view tag) const {
        for (const auto& [k, v] : kv)
            if (iequals(k, tag)) return v;
        return {};
    }
    std::string_view get2(std::string_view a, std::string_view b) const {
        const std::string_view x = get(a);
        return x.empty() ? get(b) : x;
    }
};

// 4.2: per-ability fields override the GC fallbacks (colour if any RGB != 0, width / frames if != 0, texture if non-empty).
void apply_beam_fields(const Fields& f, BeamStyle& s) {
    uint8_t c[4];
    if (rgba_of(f.get("Beam_Color"), c) && (c[0] || c[1] || c[2])) std::copy(c, c + 4, s.color);
    const float w = float_of(f.get("Beam_Width"), 0.0f);
    if (w != 0.0f) s.width = w;
    const int32_t fr = static_cast<int32_t>(std::lround(float_of(f.get("Beam_Frames"), 0.0f)));
    if (fr != 0) s.frames = fr;
    if (const auto t = f.get2("Beam_Texture", "Beam_Texture_Name"); !t.empty()) s.texture = std::string(t);
}

}  // namespace

void BeamStyleTable::set_z_adjust(objects::TypeIndex t, float z) {
    if (z_adjust_.size() <= t) z_adjust_.resize(static_cast<size_t>(t) + 1, 0.0f);
    z_adjust_[t] = z;
}

void BeamStyleTable::set_hardpoint_bones(objects::TypeIndex t, std::vector<std::string> bones) {
    if (hardpoint_bones_.size() <= t) hardpoint_bones_.resize(static_cast<size_t>(t) + 1);
    hardpoint_bones_[t] = std::move(bones);
}

const BeamStyle* BeamStyleTable::find(uint32_t crc) const {
    const auto it = styles_.find(crc);
    return it == styles_.end() ? nullptr : &it->second;
}

BeamStyle BeamStyleTable::style_for(uint8_t kind, uint32_t crc) const {
    if (const BeamStyle* s = find(crc)) return *s;
    return default_beam_style(kind, gc_);
}

std::string_view BeamStyleTable::hardpoint_bone(objects::TypeIndex t, uint32_t index) const {
    if (t >= hardpoint_bones_.size() || index >= hardpoint_bones_[t].size()) return {};
    return hardpoint_bones_[t][index];
}

void BeamStyleTable::build(const objects::GameObjectTypes& types, const BeamConstants& gc) {
    gc_ = gc;
    styles_.clear();
    z_adjust_.assign(types.size(), 0.0f);
    hardpoint_bones_.assign(types.size(), {});
    const objects::ObjectTypeDatabase& db = types.raw();
    const objects::FieldColumns& fc = db.fields();
    const objects::TypeColumns& tcol = db.types();
    const std::vector<std::string_view>& hp_names = types.hardpoint_names();
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        float z = types.get_float(t, "Ranged_Target_Z_Adjust");
        if (z == 0.0f)
            if (const auto raw = db.get_float(t, "Ranged_Target_Z_Adjust")) z = *raw;
        z_adjust_[t] = std::isfinite(z) ? z : 0.0f;
        for (uint32_t id : types.hardpoints(t))
            if (id < hp_names.size()) hardpoint_bones_[t].emplace_back(hp_names[id]);
        for (const objects::FieldIndex f : types.get_sub_objects(t, "Abilities")) {
            if (f >= fc.size()) continue;
            const uint8_t kind = kind_of_tag(db.tag_name(f));
            if (kind == 0) continue;
            std::string_view name;
            for (uint32_t a = 0; a < fc.attr_count[f]; ++a) {
                const objects::XmlAttr& at = db.attributes()[fc.attr_begin[f] + a];
                if (iequals(at.name, "Name")) name = at.value;
            }
            if (name.empty()) continue;
            Fields fields;
            const objects::TypeIndex owner_type = fc.type[f];
            for (objects::FieldIndex g = f + 1; g < tcol.field_end[owner_type]; ++g)
                if (fc.parent[g] == f) fields.kv.emplace_back(db.tag_name(g), fc.text[g]);
            BeamStyle s = default_beam_style(kind, gc);
            switch (kind) {
                case kBeamEnergyWeapon: apply_beam_fields(fields, s); break;
                case kBeamLeech: {
                    apply_beam_fields(fields, s);
                    if (const auto b = fields.get("Beam_Bone_Name"); !b.empty()) s.start_bone = std::string(b);
                    s.lightning = std::string(fields.get("Beam_Effect_Name"));
                    break;
                }
                case kBeamSuperLaser: {
                    if (const auto tx = fields.get2("Beam_Texture_Name", "Beam_Texture"); !tx.empty()) s.texture = std::string(tx);
                    const float w = float_of(fields.get("Line_Width"), 0.0f);
                    if (w != 0.0f) s.width = w;
                    uint8_t c[4];
                    if (rgba_of(fields.get("Line_Color"), c)) std::copy(c, c + 4, s.color);
                    const int32_t fr = static_cast<int32_t>(std::lround(float_of(fields.get("Num_Anim_Frames"), 0.0f)));
                    if (fr != 0) s.frames = fr;
                    if (const auto b = fields.get("Owner_Particle_Bone_Name"); !b.empty()) s.start_bone = std::string(b);
                    const float cont = float_of(fields.get("Fire_Continuation"), 0.01f);  // seconds [M: 30 ticks/s, at least 1]
                    s.continuation = std::max<int32_t>(1, static_cast<int32_t>(std::lround(cont * 30.0f)));
                    break;
                }
                case kBeamLaserZap: {
                    uint8_t c[4];
                    if (rgba_of(fields.get("Laser_Color"), c)) std::copy(c, c + 4, s.color);
                    const float w = float_of(fields.get("Laser_Beam_Width"), 0.0f);
                    if (w != 0.0f) s.width = w;
                    if (const auto tx = fields.get("Laser_Beam_Texture"); !tx.empty()) s.texture = std::string(tx);
                    const int32_t fr = static_cast<int32_t>(std::lround(float_of(fields.get("Laser_Beam_Frames"), 2.0f)));
                    s.frames = std::max(1, fr);
                    s.lifetime = s.frames;  // removed when the active-frame counter is a multiple of Laser_Beam_Frames: <= frames ticks
                    s.bones.clear();
                    for (std::string_view b : data::split_list(fields.get("Bone_Names"))) s.bones.emplace_back(b);
                    s.radius = float_of(fields.get("Protection_Radius"), 0.0f);
                    break;
                }
                default: break;  // tractor: the GC values only (2)
            }
            styles_[name_crc(name)] = std::move(s);
        }
    }
}

uint64_t beam_key(const SnapshotBeam& b) {
    uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(b.owner)) << 8) | b.kind;
    if (b.kind == kBeamLaserZap) k |= static_cast<uint64_t>(b.frame & 0xFFFFFFu) << 40;
    return k;
}

namespace {
Float3 f3(const Vec3& v) { return {v.x, v.y, v.z}; }
const SnapshotObject* find_object(const InterpolatedFrame& frame, objects::ObjectId id) {
    const auto it = std::lower_bound(frame.objects.begin(), frame.objects.end(), id,
                                     [](const SnapshotObject& o, objects::ObjectId v) { return o.id < v; });
    return it != frame.objects.end() && it->id == id ? &*it : nullptr;
}
bool hidden(const SnapshotObject& o) { return (o.flags & (kSnapFogHidden | kSnapContained)) != 0; }
}  // namespace

std::vector<BeamInstance> beams_from_frame(const InterpolatedFrame& frame, const BeamStyleTable* styles, const BeamBoneFn& bone) {
    std::vector<BeamInstance> out;
    for (const SnapshotBeam& r : frame.beams) {
        const SnapshotObject* owner = find_object(frame, r.owner);
        if (!owner || hidden(*owner)) continue;
        const SnapshotObject* target = nullptr;
        if (!r.fixed_end) {
            target = find_object(frame, r.target);
            if (!target || hidden(*target)) continue;
        }
        BeamInstance b;
        b.key = beam_key(r);
        b.kind = r.kind;
        b.style = r.style;
        b.continuous = r.kind != kBeamLaserZap;
        // far end
        Vec3 end = r.end;
        if (target) {
            end = target->position;
            end.z += r.end_z_adjust;
            if (r.kind == kBeamEnergyWeapon && r.hardpoint >= 0 && bone && styles) {
                const std::string_view hb = styles->hardpoint_bone(target->type, static_cast<uint32_t>(r.hardpoint));
                Vec3 p{};
                if (!hb.empty() && bone(*target, hb, p)) end = p;
            }
        }
        b.end = f3(end);
        // near end: the owner's bone, else the owner position
        Vec3 start = owner->position;
        if (bone) {
            Vec3 p{};
            if (r.kind == kBeamLaserZap) {  // the Bone_Names bone nearest the destroyed projectile
                float best = 0.0f;
                bool have = false;
                for (const std::string& name : r.style.bones) {
                    if (!bone(*owner, name, p)) continue;
                    const float d = (p.x - end.x) * (p.x - end.x) + (p.y - end.y) * (p.y - end.y) + (p.z - end.z) * (p.z - end.z);
                    if (!have || d < best) {
                        best = d;
                        start = p;
                        have = true;
                    }
                }
            } else if (!r.style.start_bone.empty() && bone(*owner, r.style.start_bone, p)) {
                start = p;
            }
        }
        b.start = f3(start);
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace alamo::render
