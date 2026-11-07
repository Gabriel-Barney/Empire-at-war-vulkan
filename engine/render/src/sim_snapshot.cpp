#include "alamo/render/sim_snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>

#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/sim_beams.hpp"
#include "alamo/sim/sim_host.hpp"
#ifdef ALAMO_SIM_HAVE_BATTLES
#include "alamo/sim/tactical_battle.hpp"  // Int-B: attached_effects(), combat hardpoints (apply_battle_state)
#include "alamo/tactical/abilities/ability_system.hpp"  // R4 ability effects, R5 stealth (const queries only)
#include "alamo/tactical/abilities/ability_types.hpp"
#include "alamo/tactical/combat/combat_system.hpp"      // R5 hardpoint events
#endif

namespace alamo::render {

static_assert(kMpPaletteSize == 9, "SnapshotTypeTable keeps the 9-colour MP palette");

namespace {

using objects::GameObjectTypes;
using objects::ObjectHandle;
using objects::ObjectId;
using objects::TypeIndex;

}  // namespace

std::string type_text(const GameObjectTypes& types, TypeIndex t, std::string_view tag) {
    if (objects::find_type_field(tag) != 0xFFFFFFFFu) return std::string(types.get_string(t, tag));
    const objects::ObjectTypeDatabase& raw = types.raw();
    for (int depth = 0; t != objects::kNone && t < raw.type_count() && depth < 16; ++depth) {
        if (const auto v = raw.get_text(t, tag); v && !v->empty()) return std::string(*v);
        t = raw.types().variant_parent[t];
    }
    return {};
}

namespace {

// Tokens of a list tag: the typed view's list when the tag is a typed list field, else the text (typed or raw store)
// split at commas / whitespace.
std::vector<std::string> type_tokens(const GameObjectTypes& types, TypeIndex t, std::string_view tag) {
    std::vector<std::string> out;
    if (objects::find_type_field(tag) != 0xFFFFFFFFu)
        for (std::string_view s : types.get_list(t, tag)) out.emplace_back(s);
    if (!out.empty()) return out;
    const std::string text = type_text(types, t, tag);
    std::string cur;
    for (char c : text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// R5 stealth: text of a tag at ANY depth below the type element (the Stealth_Color / Stealth_Transition_Time of the nested
// <Abilities> entries), own subtree first, then the variant chain; the first match with text.
std::string nested_text(const GameObjectTypes& types, TypeIndex t, std::string_view tag) {
    const objects::ObjectTypeDatabase& raw = types.raw();
    const auto id = raw.tags().find(tag);
    if (id == objects::kNone) return {};
    const objects::TypeColumns& tc = raw.types();
    const objects::FieldColumns& fc = raw.fields();
    for (int depth = 0; t != objects::kNone && t < raw.type_count() && depth < 16; ++depth) {
        for (auto f = tc.field_begin[t]; f < tc.field_end[t]; ++f)
            if (fc.tag[f] == id && !fc.text[f].empty()) return std::string(fc.text[f]);
        t = tc.variant_parent[t];
    }
    return {};
}

bool parse_float(std::string_view s, float& v) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && std::isfinite(v);
}

bool yes_text(const std::string& s) {
    return !s.empty() && (s[0] == 'y' || s[0] == 'Y' || s[0] == 't' || s[0] == 'T' || s == "1");
}

bool finite(float f) { return std::isfinite(f); }

float length3(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

}  // namespace

// ------------------------------------------------------------------------------------------- types
void SnapshotTypeTable::build(const GameObjectTypes& types, objects::ModeKind mode, const GameTypeResolver* resolver) {
    mode_ = mode;
    info_.assign(types.size(), {});
    const bool space = mode == objects::ModeKind::Space;
    const MpPalette pal = resolver ? resolver->palette() : default_mp_palette();
    for (uint32_t i = 0; i < kMpPaletteSize; ++i) palette_[i] = pal.color[i];
    neutral_.clear();
    neutral_color_.clear();
    if (resolver && !resolver->factions().empty()) {
        for (const GameTypeResolver::Faction& f : resolver->factions()) {
            neutral_.push_back(f.neutral ? 1 : 0);
            neutral_color_.push_back(f.no_colorization);
        }
    } else {
        // Typed view: every <Faction> definition in file order (duplicates kept) [approximation of the
        // placement owner -> FactionFiles index rule MapScene uses].
        for (uint8_t n : types.faction_is_neutral()) {
            neutral_.push_back(n);
            neutral_color_.push_back({1, 1, 1, 1});
        }
    }
    const objects::ObjectTypeDatabase& raw = types.raw();
    const std::vector<float>& health = types.columns().tactical_health;
    for (TypeIndex t = 0; t < info_.size(); ++t) {
        SnapshotTypeInfo& ti = info_[t];
        ti.name = std::string(raw.types().name[t]);
        ti.name_crc = raw.types().name_crc[t];
        ti.max_health = t < health.size() ? health[t] : 0.0f;
        // Particle gates and land damage states (typed view along the variant chain; also with a resolver).
        ti.allow_particle_model_culling =
            types.get_bool(t, "Allow_Particle_Model_Culling") || yes_text(type_text(types, t, "Allow_Particle_Model_Culling"));
        {
            float lv = types.get_float(t, "No_Particle_Below_Detail_Level");  // typed field (0 when unknown / unset)
            if (!(finite(lv) && lv != 0.0f) && !parse_float(type_text(types, t, "No_Particle_Below_Detail_Level"), lv)) lv = 0.0f;
            ti.no_particle_below_detail = lv;
        }
        {  // R1 (TACTICAL_GAPS §6.3): no mode test - space types with thresholds swap alternates too
            const std::vector<std::string> th = type_tokens(types, t, "Land_Damage_Thresholds");
            const std::vector<std::string> al = type_tokens(types, t, "Land_Damage_Alternates");
            for (size_t k = 0; k < th.size() && k < al.size(); ++k) {
                float f = 0.0f, a = 0.0f;
                if (!parse_float(th[k], f) || !parse_float(al[k], a)) break;
                ti.damage_thresholds.push_back(f);
                ti.damage_alternates.push_back(static_cast<int8_t>(std::clamp(a, -1.0f, 9.0f)));
            }
        }
        // R5 decorations: SurfaceFX_Name list and Explosion_Scorch_Mark (SurfaceFX trigger names).
        ti.surface_fx = type_tokens(types, t, "SurfaceFX_Name");
        ti.scorch_mark = type_text(types, t, "Explosion_Scorch_Mark");
        ti.spin_away_explosion = type_text(types, t, "Spin_Away_On_Death_Explosion");
        {  // projectile laser bolts (Projectile_Custom_Render / _Width / _Length)
            float v = 0.0f;
            if (parse_float(type_text(types, t, "Projectile_Custom_Render"), v)) ti.projectile_custom_render = static_cast<int32_t>(v);
            if (parse_float(type_text(types, t, "Projectile_Width"), v) && v > 0.0f) ti.projectile_width = v;
            if (parse_float(type_text(types, t, "Projectile_Length"), v) && v > 0.0f) ti.projectile_length = v;
            const std::vector<std::string> slot = type_tokens(types, t, "Projectile_Texture_Slot");
            for (size_t k = 0; k < 2 && k < slot.size(); ++k)
                if (parse_float(slot[k], v)) ti.projectile_slot[k] = static_cast<uint8_t>(std::clamp(v, 0.0f, 7.0f));
            const std::vector<std::string> col = type_tokens(types, t, "Projectile_Laser_Color");
            for (size_t k = 0; k < 4 && k < col.size(); ++k)
                if (parse_float(col[k], v)) ti.projectile_color[k] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
        }
        {  // R5 stealth look (ABILITY_VISUALS 1.1): Stealth_Color / Force_Cloak_Color (0..255 -> 0..1) and the transition times
            auto color = [&](std::string_view tag, float* out) {
                const std::string text = nested_text(types, t, tag);
                float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                size_t n = 0;
                const auto sep = [](char ch) { return ch == ',' || std::isspace(static_cast<unsigned char>(ch)) != 0; };
                for (size_t i = 0; i < text.size() && n < 4;) {
                    while (i < text.size() && sep(text[i])) ++i;
                    size_t j = i;
                    while (j < text.size() && !sep(text[j])) ++j;
                    float v = 0.0f;
                    if (j > i && parse_float(std::string_view(text).substr(i, j - i), v)) c[n++] = std::clamp(v / 255.0f, 0.0f, 1.0f);
                    i = j;
                }
                if (n < 3) return false;  // R, G, B at least (alpha stays 1 without a fourth value)
                for (size_t k = 0; k < 4; ++k) out[k] = c[k];
                return true;
            };
            auto seconds = [&](std::string_view tag) {
                float v = 0.0f;
                return parse_float(nested_text(types, t, tag), v) && v > 0.0f ? v : 0.0f;
            };
            ti.has_stealth_color = color("Stealth_Color", ti.stealth_color);
            ti.has_cloak_color = color("Force_Cloak_Color", ti.cloak_color);
            ti.stealth_time = seconds("Stealth_Transition_Time");
            ti.cloak_time = seconds("Force_Cloak_Transition_In_Secs");
        }
        const bool laser = ti.projectile_custom_render == 1 || ti.projectile_custom_render == 2;  // no model (1 summary item 1)
        ti.ghost_last_state = types.get_bool(t, "Last_State_Visible_Under_FOW") || yes_text(type_text(types, t, "Last_State_Visible_Under_FOW"));
        ti.ghost_initial_state =
            types.get_bool(t, "Initial_State_Visible_Under_FOW") || yes_text(type_text(types, t, "Initial_State_Visible_Under_FOW"));
        {
            float ms = types.get_float(t, "Max_Speed");
            if (!(finite(ms) && ms > 0.0f) && !parse_float(type_text(types, t, "Max_Speed"), ms)) ms = 0.0f;
            ti.max_speed = finite(ms) && ms > 0.0f ? ms : 0.0f;
        }
        // [inferred] Is_Marker types (map editor flags: Attacker Entry Position, Defending Forces Position, ...) carry a model
        // but never show in the base game (fixpass_capture C2_space_*: no flag at the start positions; playtest V3). The drawing
        // rule itself is not traced. In_Background markers (Death_Star_Position: a backdrop model) stay drawn.
        const bool marker = types.get_bool(t, "Is_Marker") || yes_text(type_text(types, t, "Is_Marker"));
        terrain::ResolvedType rt;
        if (resolver && resolver->resolve(ti.name_crc, space, rt)) {
            ti.model = laser ? std::string() : rt.model_name;
            ti.scale = rt.scale_factor;
            const TypeRenderInfo ri = resolver->render_info(ti.name_crc);
            ti.lod_bias = ri.lod_bias;
            ti.in_background = ri.in_background;
            if (marker && !ti.in_background) ti.model.clear();
            ti.has_no_colorization = ri.has_no_colorization;
            ti.no_colorization = ri.no_colorization;
            for (const std::string& h : ri.hardpoints) ti.hardpoint_crcs.push_back(alamo::name_crc(h));
            continue;
        }
        // Typed view: the mode's model field, then the other tactical one, then galactic (MapScene's order).
        static const char* const kLand[] = {"Land_Model_Name", "Space_Model_Name", "Galactic_Model_Name"};
        static const char* const kSpace[] = {"Space_Model_Name", "Land_Model_Name", "Galactic_Model_Name"};
        for (const char* tag : (space ? kSpace : kLand)) {
            if (laser) break;
            ti.model = type_text(types, t, tag);
            if (!ti.model.empty()) break;
        }
        const float sf = types.get_float(t, "Scale_Factor");
        ti.scale = finite(sf) && sf > 0.0f ? sf : 1.0f;
        const float lb = types.get_float(t, "LOD_Bias");
        ti.lod_bias = finite(lb) && lb >= 0.0f ? lb : kDefaultLodBias;
        const std::string bg = type_text(types, t, "In_Background");
        ti.in_background = yes_text(bg);
        if (marker && !ti.in_background) ti.model.clear();
        const auto& hp_names = types.hardpoint_names();
        for (uint32_t h : types.hardpoints(t))
            if (h < hp_names.size()) ti.hardpoint_crcs.push_back(alamo::name_crc(hp_names[h]));
    }
    by_name_.clear();
    by_name_.reserve(info_.size());
    for (TypeIndex t = 0; t < info_.size(); ++t) by_name_.emplace_back(ascii_upper(info_[t].name), t);
    std::stable_sort(by_name_.begin(), by_name_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

TypeIndex SnapshotTypeTable::find(std::string_view name) const {
    const std::string key = ascii_upper(name);
    const auto it = std::lower_bound(by_name_.begin(), by_name_.end(), key, [](const auto& e, const std::string& k) { return e.first < k; });
    return it != by_name_.end() && it->first == key ? it->second : objects::kNone;
}

int8_t damage_alt_index(const SnapshotTypeInfo& t, float health_fraction) {
    int8_t alt = 0;
    if (!finite(health_fraction)) return alt;
    const size_t n = std::min(t.damage_thresholds.size(), t.damage_alternates.size());
    for (size_t k = 0; k < n; ++k)
        if (health_fraction <= t.damage_thresholds[k]) alt = t.damage_alternates[k];
    return alt;
}

materials::float4 SnapshotTypeTable::team_color(int32_t owner, const SnapshotTypeInfo& t, uint8_t& index) const {
    const bool neutral = owner < 0 || (static_cast<size_t>(owner) < neutral_.size() && neutral_[static_cast<size_t>(owner)] != 0);
    if (!neutral) {
        const auto o = static_cast<size_t>(owner);
        index = static_cast<uint8_t>(o < owner_colours_.size() ? owner_colours_[o] % kMpPaletteSize : static_cast<uint32_t>(owner) % kMpPaletteSize);
        return palette_[index];
    }
    index = kNeutralTeam;
    if (t.has_no_colorization) return t.no_colorization;
    if (owner >= 0 && static_cast<size_t>(owner) < neutral_color_.size()) return neutral_color_[static_cast<size_t>(owner)];
    return {1, 1, 1, 1};
}

// ---------------------------------------------------------------------------------------- snapshot
const SnapshotObject* SimSnapshot::find(ObjectId id) const {
    const auto it = std::lower_bound(objects.begin(), objects.end(), id, [](const SnapshotObject& o, ObjectId v) { return o.id < v; });
    return it != objects.end() && it->id == id ? &*it : nullptr;
}

uint64_t SimSnapshot::digest() const {
    StateHasher h;
    h.u64(tick);
    h.u64(mode_frame);
    h.u8(static_cast<uint8_t>(mode));
    h.u32(store_objects);
    h.u32(static_cast<uint32_t>(objects.size()));
    for (const SnapshotObject& o : objects) {
        h.i32(o.id);
        h.u32(o.type);
        h.i32(o.owner);
        h.i32(o.attach_parent);
        for (float f : {o.position.x, o.position.y, o.position.z, o.facing.x, o.facing.y, o.facing.z, o.health, o.health_fraction,
                        o.lod_bias, o.fade, o.team_color.x, o.team_color.y, o.team_color.z, o.team_color.w})
            h.f32(f);
        h.u8(o.team_index);
        h.u8(o.flags);
        h.u8(static_cast<uint8_t>(o.alt));
        h.u32(o.hardpoint_first);
        h.u32(o.hardpoint_count);
    }
    h.u32(static_cast<uint32_t>(hardpoint_destroyed.size()));
    if (!hardpoint_destroyed.empty()) h.bytes(hardpoint_destroyed.data(), hardpoint_destroyed.size());
    h.u32(static_cast<uint32_t>(effects.size()));
    for (const SnapshotEffect& e : effects) {
        h.i32(e.id);
        h.u32(static_cast<uint32_t>(e.asset.size()));
        if (!e.asset.empty()) h.bytes(e.asset.data(), e.asset.size());
        h.u8(e.percent);
        h.u32(e.frame);
    }
    h.u32(static_cast<uint32_t>(hardpoint_events.size()));
    for (const SnapshotHardpointEvent& e : hardpoint_events) {
        h.i32(e.id);
        h.u32(e.index);
    }
    for (const SnapshotObject& o : objects) {
        h.f32(o.roll);
        h.f32(o.pitch);  // S7 t05
    }
    h.u32(static_cast<uint32_t>(muzzles.size()));
    for (const SnapshotMuzzle& m : muzzles) {
        h.i32(m.id);
        h.i32(m.hardpoint);
        h.i32(m.bone_index);
        h.u32(static_cast<uint32_t>(m.bone.size()));
        if (!m.bone.empty()) h.bytes(m.bone.data(), m.bone.size());
    }
    h.u32(static_cast<uint32_t>(foot_plants.size()));
    for (const SnapshotFootPlant& f : foot_plants) {
        h.i32(f.id);
        h.u8(f.foot);
        for (float v : {f.position.x, f.position.y, f.position.z}) h.f32(v);
        h.u32(static_cast<uint32_t>(f.trigger.size()));
        if (!f.trigger.empty()) h.bytes(f.trigger.data(), f.trigger.size());
    }
    // R5: shields, ability state / events, Lua effect fields.
    for (const SnapshotObject& o : objects) {
        h.f32(o.shield_fraction);
        h.u64(o.ability_bits[0]);
        h.u64(o.ability_bits[1]);
    }
    for (const SnapshotEffect& e : effects) {
        h.u32(e.lua_seq);
        h.u32(static_cast<uint32_t>(e.bone.size()));
        if (!e.bone.empty()) h.bytes(e.bone.data(), e.bone.size());
    }
    h.u32(static_cast<uint32_t>(ability_events.size()));
    for (const SnapshotAbilityEvent& e : ability_events) {
        h.i32(e.id);
        h.u8(e.type);
        h.u8(static_cast<uint8_t>(e.kind));
    }
    h.u32(static_cast<uint32_t>(projectiles.size()));
    for (const SnapshotProjectile& p : projectiles) {
        h.u32(p.serial);
        h.u32(p.desc);
        h.u32(static_cast<uint32_t>(p.type));
        h.i32(p.owner);
        h.i32(p.shooter);
        for (float v : {p.position.x, p.position.y, p.position.z, p.direction.x, p.direction.y, p.direction.z, p.speed}) h.f32(v);
    }
    if (!jamming.jammers.empty() || !jamming.jammed.empty() || !jamming.stealthed.empty()) {  // R5 jamming (hashed only when present: older digests stay)
        h.u32(static_cast<uint32_t>(jamming.jammers.size()));
        for (const SnapshotJammer& j : jamming.jammers) {
            h.i32(j.id);
            h.i32(j.owner);
            h.f32(j.radius);
            h.u8(static_cast<uint8_t>((j.allied ? 1 : 0) | (j.show_to_enemies ? 2 : 0)));
            h.u32(static_cast<uint32_t>(j.blob_material.size()));
            if (!j.blob_material.empty()) h.bytes(j.blob_material.data(), j.blob_material.size());
            for (float v : j.blob_color) h.f32(v);
        }
        h.u32(static_cast<uint32_t>(jamming.jammed.size()));
        for (const objects::ObjectId id : jamming.jammed) h.i32(id);
        h.u32(static_cast<uint32_t>(jamming.stealthed.size()));
        for (const objects::ObjectId id : jamming.stealthed) h.i32(id);
    }
    h.u32(static_cast<uint32_t>(beams.size()));  // R5 ability beams
    for (const SnapshotBeam& b : beams) {
        h.u8(b.kind);
        h.i32(b.owner);
        h.i32(b.target);
        h.u8(b.fixed_end ? 1 : 0);
        for (float v : {b.end.x, b.end.y, b.end.z, b.end_z_adjust, b.style.width}) h.f32(v);
        h.i32(b.hardpoint);
        h.u32(b.frame);
        h.u32(static_cast<uint32_t>(b.style.texture.size()));
        if (!b.style.texture.empty()) h.bytes(b.style.texture.data(), b.style.texture.size());
        h.bytes(b.style.color, 4);
        h.i32(b.style.frames);
        h.u8(b.style.mode);
    }
    h.u8(fog ? 1 : 0);
    if (fog) {
        const FogFrameInput& f = *fog;
        h.u8(f.valid ? 1 : 0);
        h.i32(f.player);
        h.u32(f.width);
        h.u32(f.height);
        for (float v : {f.x0, f.y0, f.x1, f.y1, f.cell_w, f.cell_h}) h.f32(v);
        h.u32(f.frame);
        h.u32(f.last_decay);
        h.i32(f.regrow_step);
        h.u8(static_cast<uint8_t>((f.fow_active ? 1 : 0) | (f.render_option ? 2 : 0) | (f.setup_phase ? 4 : 0) | (f.reveal_all ? 8 : 0)));
        h.u32(static_cast<uint32_t>(f.state.size()));
        if (!f.state.empty()) h.bytes(f.state.data(), f.state.size());
        h.u32(static_cast<uint32_t>(f.fade.size()));
        if (!f.fade.empty()) h.bytes(f.fade.data(), f.fade.size());
        h.u32(static_cast<uint32_t>(f.reinforce.size()));
        if (!f.reinforce.empty()) h.bytes(f.reinforce.data(), f.reinforce.size());
    }
    return h.digest();
}

void build_snapshot(const objects::ObjectStore& store, uint64_t tick, uint64_t mode_frame, const SnapshotTypeTable& table,
                    SimSnapshot& out, const ParallelFor& pf, const SnapshotOptions& options) {
    out.tick = tick;
    out.mode_frame = mode_frame;
    out.mode = store.mode();
    out.objects.clear();
    out.hardpoint_destroyed.clear();
    out.effects.clear();
    out.hardpoint_events.clear();
    out.fog.reset();
    out.muzzles.clear();
    out.foot_plants.clear();
    out.ability_events.clear();
    out.projectiles.clear();
    out.beams.clear();
    const std::span<const ObjectHandle> all = store.all_objects();
    out.store_objects = static_cast<uint32_t>(all.size());
    const auto n = static_cast<uint32_t>(all.size());
    const bool space = store.mode() == objects::ModeKind::Space;
    // Pass 1 (parallel, per-index outputs): every store object's record and whether it is kept.
    std::vector<SnapshotObject> tmp(n);
    std::vector<uint8_t> keep(n, 0);
    pf(n, std::max(1u, options.grain), [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            const ObjectHandle h = all[i];
            if (!store.exists(h)) continue;
            SnapshotObject& o = tmp[i];
            o.id = store.id(h);
            o.type = store.type(h);
            o.owner = store.owner(h);
            const ObjectHandle ap = store.attach_parent(h);
            o.attach_parent = (ap && store.exists(ap)) ? store.id(ap) : objects::kNoObjectId;
            o.position = store.position(h);
            o.facing = store.facing(h);
            o.health = store.health(h);
            const bool dying = (store.flags(h) & objects::kFlagDead) != 0;
            o.fade = dying ? 0.0f : 1.0f;
            o.flags = dying ? kSnapDying : 0;
            if ((store.flags(h) & objects::kFlagDeathClone) != 0) o.flags = static_cast<uint8_t>(o.flags | kSnapDeathClone);
            if (table.valid(o.type)) {
                const SnapshotTypeInfo& ti = table[o.type];
                o.lod_bias = ti.lod_bias;
                o.health_fraction = ti.max_health > 0.0f ? o.health / ti.max_health : 1.0f;
                o.alt = damage_alt_index(ti, o.health_fraction);  // empty lists on space maps -> 0
                o.team_color = table.team_color(o.owner, ti, o.team_index);
                if (!ti.model.empty()) o.flags = static_cast<uint8_t>(o.flags | kSnapDrawable);
                if (space && ti.in_background) o.flags = static_cast<uint8_t>(o.flags | kSnapBackground);
                o.hardpoint_count = static_cast<uint32_t>(ti.hardpoint_crcs.size());
            }
            keep[i] = (!options.drawable_only || (o.flags & kSnapDrawable) != 0) ? 1 : 0;
        }
    });
    // Pass 2 (ordered commit): compaction in store order, then ascending id (ids are unique).
    for (uint32_t i = 0; i < n; ++i)
        if (keep[i]) out.objects.push_back(tmp[i]);
    std::sort(out.objects.begin(), out.objects.end(), [](const SnapshotObject& a, const SnapshotObject& b) { return a.id < b.id; });
    uint32_t hp = 0;
    for (SnapshotObject& o : out.objects) {
        o.hardpoint_first = hp;
        hp += o.hardpoint_count;
    }
    out.hardpoint_destroyed.assign(hp, 0);
    // Pass 3 (store order): destroyed hardpoint sub-objects (PROVISIONAL rule, see the header).
    for (uint32_t i = 0; i < n; ++i) {
        const SnapshotObject& c = tmp[i];
        if (c.attach_parent == objects::kNoObjectId || !table.valid(c.type)) continue;
        if ((c.flags & kSnapDying) == 0 && c.health > 0.0f) continue;
        const SnapshotObject* p = out.find(c.attach_parent);
        if (!p || !table.valid(p->type)) continue;
        const std::vector<uint32_t>& crcs = table[p->type].hardpoint_crcs;
        const uint32_t child_crc = table[c.type].name_crc;
        for (uint32_t k = 0; k < crcs.size() && k < p->hardpoint_count; ++k)
            if (crcs[k] == child_crc) {
                out.hardpoint_destroyed[p->hardpoint_first + k] = 1;
                break;
            }
    }
}

bool capture_snapshot(const sim::SimHost& host, const SnapshotTypeTable& table, SimSnapshot& out, const ParallelFor& pf,
                      const SnapshotOptions& options) {
    const sim::TacticalMode* tm = host.modes().top_tactical();
    if (!tm) {
        out = SimSnapshot{};
        out.tick = host.frame();
        return false;
    }
    build_snapshot(tm->primary(), host.frame(), tm->frame(), table, out, pf, options);
    return true;
}

void set_snapshot_effects(SimSnapshot& snap, std::span<const SnapshotEffect> effects) {
    snap.effects.clear();
    append_snapshot_effects(snap, effects);
}

void append_snapshot_effects(SimSnapshot& snap, std::span<const SnapshotEffect> effects) {
    for (const SnapshotEffect& e : effects)
        if (!e.asset.empty() && snap.find(e.id)) snap.effects.push_back(e);
    std::stable_sort(snap.effects.begin(), snap.effects.end(), [](const SnapshotEffect& a, const SnapshotEffect& b) { return a.id < b.id; });
}

// R1: the combat source is authoritative for the objects it tracks (same hardpoint count as the type's list): their
// bits are its destroyed states, replacing the PROVISIONAL attach-child rule of build_snapshot; objects it does not
// track keep the provisional bits. (The instance builder keeps destroyed hardpoints sticky: B.10, no repair visuals.)
void apply_hardpoint_source(SimSnapshot& snap, const std::function<uint32_t(ObjectId)>& count,
                            const std::function<bool(ObjectId, uint32_t)>& destroyed) {
    if (!count || !destroyed) return;
    for (const SnapshotObject& o : snap.objects) {
        if (o.hardpoint_count == 0 || count(o.id) != o.hardpoint_count) continue;
        for (uint32_t k = 0; k < o.hardpoint_count; ++k) snap.hardpoint_destroyed[o.hardpoint_first + k] = destroyed(o.id, k) ? 1 : 0;
    }
}

void sort_snapshot_projectiles(SimSnapshot& snap) {
    std::stable_sort(snap.projectiles.begin(), snap.projectiles.end(),
                     [](const SnapshotProjectile& a, const SnapshotProjectile& b) { return a.serial < b.serial; });
}

void set_snapshot_ability_events(SimSnapshot& snap, const SimSnapshot* prev) {
    snap.ability_events.clear();
    size_t j = 0;
    for (const SnapshotObject& o : snap.objects) {
        const SnapshotObject* p = nullptr;
        if (prev) {
            while (j < prev->objects.size() && prev->objects[j].id < o.id) ++j;
            if (j < prev->objects.size() && prev->objects[j].id == o.id && prev->objects[j].type == o.type) p = &prev->objects[j];
        }
        const uint64_t before[2] = {p ? p->ability_bits[0] : 0u, p ? p->ability_bits[1] : 0u};
        for (uint32_t w = 0; w < 2; ++w) {
            const uint64_t changed = before[w] ^ o.ability_bits[w];
            for (uint32_t b = 0; b < 64; ++b) {
                if (!((changed >> b) & 1u)) continue;
                const bool on = ((o.ability_bits[w] >> b) & 1u) != 0;
                snap.ability_events.push_back({o.id, static_cast<uint8_t>(w * 64 + b), on ? SnapshotAbilityKind::Activated : SnapshotAbilityKind::Deactivated});
            }
        }
        const bool st0 = p && (p->flags & kSnapStealthMeshes) != 0, st1 = (o.flags & kSnapStealthMeshes) != 0;
        if (st0 != st1) snap.ability_events.push_back({o.id, 0, st1 ? SnapshotAbilityKind::StealthOn : SnapshotAbilityKind::StealthOff});
    }
}

void set_snapshot_hardpoint_events(SimSnapshot& snap, std::span<const SnapshotHardpointEvent> events) {
    snap.hardpoint_events.clear();
    for (const SnapshotHardpointEvent& e : events) {
        const SnapshotObject* o = snap.find(e.id);
        if (!o || e.index >= o->hardpoint_count) continue;
        snap.hardpoint_events.push_back(e);
        snap.hardpoint_destroyed[o->hardpoint_first + e.index] = 1;
    }
}

namespace {
// Sets or clears flag `bit` on every object by `pred` (a null predicate clears it everywhere).
template <typename Bit>
void mark_flag(SimSnapshot& snap, Bit bit, const std::function<bool(const SnapshotObject&)>& pred) {
    for (SnapshotObject& o : snap.objects) {
        const bool on = pred && pred(o);
        o.flags = static_cast<uint8_t>(on ? (o.flags | bit) : (o.flags & ~bit));
    }
}
}  // namespace

void apply_fog_hidden(SimSnapshot& snap, const std::function<bool(const SnapshotObject&)>& hidden) { mark_flag(snap, kSnapFogHidden, hidden); }

void apply_contained(SimSnapshot& snap, const std::function<bool(const SnapshotObject&)>& contained) {
    mark_flag(snap, kSnapContained, contained);
}

void apply_script_hidden(SimSnapshot& snap, const std::function<bool(const SnapshotObject&)>& hidden) {
    for (SnapshotObject& o : snap.objects) o.script_hidden = hidden && hidden(o);
}

void set_snapshot_muzzles(SimSnapshot& snap, std::span<const SnapshotMuzzle> muzzles) {
    snap.muzzles.clear();
    for (const SnapshotMuzzle& m : muzzles)
        if (snap.find(m.id)) snap.muzzles.push_back(m);
    std::stable_sort(snap.muzzles.begin(), snap.muzzles.end(), [](const SnapshotMuzzle& a, const SnapshotMuzzle& b) { return a.id < b.id; });
}

void set_snapshot_foot_plants(SimSnapshot& snap, std::span<const SnapshotFootPlant> plants) {
    snap.foot_plants.clear();
    for (const SnapshotFootPlant& f : plants)
        if (snap.find(f.id)) snap.foot_plants.push_back(f);
}

void add_snapshot_corpses(SimSnapshot& snap, const SnapshotTypeTable& table, std::span<const SnapshotCorpse> corpses) {
    const auto before = static_cast<std::ptrdiff_t>(snap.objects.size());
    for (const SnapshotCorpse& c : corpses) {
        if (!(c.alpha > 0.0f) || !table.valid(c.type) || table[c.type].model.empty()) continue;
        const auto end = snap.objects.begin() + before;
        const auto it = std::lower_bound(snap.objects.begin(), end, c.id, [](const SnapshotObject& o, ObjectId v) { return o.id < v; });
        if (it != end && it->id == c.id) continue;  // the store object still exists: drawn through it
        bool dup = false;                           // the same id twice in `corpses`
        for (auto k = end; k != snap.objects.end() && !dup; ++k) dup = k->id == c.id;
        if (dup) continue;
        const SnapshotTypeInfo& ti = table[c.type];
        SnapshotObject o;
        o.id = c.id;
        o.type = c.type;
        o.owner = c.owner;
        o.position = c.position;
        o.facing = c.facing;
        o.health = 0.0f;
        o.health_fraction = 0.0f;
        o.lod_bias = ti.lod_bias;
        o.fade = c.alpha < 1.0f ? c.alpha : 1.0f;
        o.team_color = table.team_color(o.owner, ti, o.team_index);
        o.alt = damage_alt_index(ti, 0.0f);
        o.roll = std::isfinite(c.roll) ? c.roll : 0.0f;
        o.flags = static_cast<uint8_t>(kSnapDrawable | kSnapCorpse);
        if (c.spin_away) {  // R3 spin-away motion (render-side [port] rates, sim_snapshot.hpp)
            const float t = std::isfinite(c.spin_seconds) && c.spin_seconds > 0.0f ? c.spin_seconds : 0.0f;
            const float yaw = kSpinAwayYawRate * t, cy = std::cos(yaw), sy = std::sin(yaw);
            o.position = {c.position.x + c.facing.x * kSpinAwayDrift * t, c.position.y + c.facing.y * kSpinAwayDrift * t,
                          c.position.z + c.facing.z * kSpinAwayDrift * t};
            o.facing = {c.facing.x * cy - c.facing.y * sy, c.facing.x * sy + c.facing.y * cy, c.facing.z};
            o.roll += kSpinAwayRollRate * t;
            o.flags = static_cast<uint8_t>(o.flags | kSnapSpinAway);
        }
        if (snap.mode == objects::ModeKind::Space && ti.in_background) o.flags = static_cast<uint8_t>(o.flags | kSnapBackground);
        o.hardpoint_first = static_cast<uint32_t>(snap.hardpoint_destroyed.size());
        o.hardpoint_count = 0;
        snap.objects.push_back(o);
    }
    if (static_cast<std::ptrdiff_t>(snap.objects.size()) != before)
        std::sort(snap.objects.begin(), snap.objects.end(), [](const SnapshotObject& a, const SnapshotObject& b) { return a.id < b.id; });
}

void apply_snapshot_attitude(SimSnapshot& snap, const std::function<bool(ObjectId, float& roll, float& pitch)>& source) {
    if (!source) return;
    for (SnapshotObject& o : snap.objects) {
        if ((o.flags & kSnapCorpse) != 0) continue;  // corpses keep their own (spin-away) roll
        float r = 0.0f, p = 0.0f;
        if (!source(o.id, r, p)) continue;
        o.roll = std::isfinite(r) ? r : 0.0f;
        o.pitch = std::isfinite(p) ? p : 0.0f;
    }
}

#ifdef ALAMO_SIM_HAVE_BATTLES
void apply_battle_projectiles(const sim::TacticalBattle& battle, SimSnapshot& snap, const SnapshotTypeTable& table) {
    const tactical::combat::CombatSystem& combat = battle.combat();
    const std::vector<tactical::combat::ProjectileDesc>& descs = combat.data().projectiles;
    std::vector<objects::TypeIndex> types(descs.size(), objects::kNone);
    for (size_t i = 0; i < descs.size(); ++i) types[i] = table.find(descs[i].name);
    std::vector<tactical::combat::ProjectileView> views;
    views.reserve(combat.projectile_count());
    for (size_t i = 0; i < combat.projectile_count(); ++i) views.push_back(combat.projectile(i));
    set_snapshot_projectiles(snap, views, types);
}

void apply_battle_state(const sim::TacticalBattle& battle, SimSnapshot& snap, const SnapshotTypeTable* table,
                        const BeamStyleTable* beam_styles) {
    std::vector<SnapshotEffect> fx;
    for (const sim::AttachedEffect& e : battle.attached_effects()) fx.push_back({e.id, e.asset, e.percent, e.frame});
    const tactical::abilities::AbilitySystem& ab = battle.abilities();
    const tactical::combat::CombatSystem& combat = battle.combat();
    // R5: per object the active unit-ability bits, the stealth mesh state and the shield fraction (const queries).
    for (SnapshotObject& o : snap.objects) {
        if (combat.has_unit(o.id) && combat.max_shield_of(o.id) > 0.0f) o.shield_fraction = combat.shield_fraction(o.id);
        if (!ab.has_object(o.id)) continue;
        for (uint32_t t = 0; t <= tactical::abilities::kMaxAbilityType && t < 128; ++t) {
            const auto type = static_cast<uint8_t>(t);
            if (ab.has_ability(o.id, type) && ab.is_active(o.id, type)) o.ability_bits[t / 64] |= uint64_t{1} << (t % 64);
        }
        // R5 stealth: the "applied" flag (SAP +0x2a4) = the Stealth / Force_Cloak special is applied (it drops at a break:
        // firing, stun, expiry), plus Is_Stealthed. A unit ability that is still active but no longer applied is a break:
        // the tint then fades with the 0.3 s break time (ABILITY_VISUALS §1.1) [M: expiry is told from a break by the ability
        // having ended too].
        bool stealth_applied = ab.is_stealthed(o.id, battle.frame()), stealth_broken = false;
        for (const uint8_t st : {kAbilityStealth, kAbilityForceCloak}) {
            const tactical::abilities::UnitAbilityRecord* rec = ab.has_ability(o.id, st) ? ab.record(o.id, st) : nullptr;
            const bool applied = rec && ab.special_applied(o.id, rec->gui_name_crc);
            stealth_applied = stealth_applied || applied;
            stealth_broken = stealth_broken || (rec && !applied && o.ability_active(st));
        }
        if (stealth_applied) o.flags = static_cast<uint8_t>(o.flags | kSnapStealthMeshes);
        else if (stealth_broken) o.stealth_time = kStealthBreakFade;
    }
    // R4: Particle_Effect of active unit abilities (resolved through the type table).
    if (table) {
        for (const SnapshotObject& o : snap.objects) {
            if (!ab.has_object(o.id)) continue;
            for (uint32_t t = 0; t <= tactical::abilities::kMaxAbilityType; ++t) {
                const auto type = static_cast<uint8_t>(t);
                if (!o.ability_active(t)) continue;
                const tactical::abilities::UnitAbilityRecord* rec = ab.record(o.id, type);
                if (!rec || rec->particle_effect.empty()) continue;
                std::string asset = rec->particle_effect;
                const objects::TypeIndex ti = table->find(asset);
                if (ti != objects::kNone && table->valid(ti) && !(*table)[ti].model.empty()) asset = (*table)[ti].model;
                fx.push_back({o.id, std::move(asset), static_cast<uint8_t>(kEffectAbilityBase + t), battle.frame()});
            }
        }
    }
    set_snapshot_effects(snap, fx);
    apply_hardpoint_source(
        snap, [&combat](ObjectId id) { return combat.hardpoint_count(id); },
        [&combat](ObjectId id, uint32_t k) { return combat.hardpoint_destroyed(id, k); });
    std::vector<SnapshotHardpointEvent> hp;
    for (const tactical::combat::HardpointEvent& e : combat.hardpoint_events()) hp.push_back({e.owner, e.index});
    // Only objects whose hardpoint list matches the combat one (apply_hardpoint_source's rule).
    std::erase_if(hp, [&](const SnapshotHardpointEvent& e) {
        const SnapshotObject* o = snap.find(e.id);
        return !o || combat.hardpoint_count(e.id) != o->hardpoint_count;
    });
    set_snapshot_hardpoint_events(snap, hp);
    // S7 t05: fighter / flyer roll and pitch (bike lean) from the movement system.
    apply_snapshot_attitude(snap, [&battle](ObjectId id, float& r, float& p) { return battle.unit_attitude(id, r, p); });
    apply_battle_beams(battle, snap, beam_styles);  // R5 ability beams
}

void apply_battle_beams(const sim::TacticalBattle& battle, SimSnapshot& snap, const BeamStyleTable* styles) {
    apply_beams(battle.abilities(), battle.combat(), battle.frame(), snap, styles);
}

void apply_beams(const tactical::abilities::AbilitySystem& ab, const tactical::combat::CombatSystem& combat, uint32_t frame,
                 SimSnapshot& snap, const BeamStyleTable* styles) {
    snap.beams.clear();
    const BeamConstants gc = styles ? styles->constants() : BeamConstants{};
    for (const SnapshotObject& o : snap.objects) {  // ascending id
        if (!ab.has_object(o.id)) continue;
        for (const uint8_t kind : {kBeamTractor, kBeamEnergyWeapon, kBeamSuperLaser, kBeamLaserZap, kBeamLeech}) {
            if (!ab.has_ability(o.id, kind)) continue;
            const tactical::abilities::UnitAbilityRecord* rec = ab.record(o.id, kind);
            if (!rec || rec->gui_name_crc == 0) continue;
            if (kind != kBeamLaserZap && !ab.special_applied(o.id, rec->gui_name_crc)) continue;  // a zap outlives its removal tick
            SnapshotBeam b;
            b.kind = kind;
            b.owner = o.id;
            b.frame = frame;
            b.style = styles ? styles->style_for(kind, rec->gui_name_crc) : default_beam_style(kind, gc);
            if (kind == kBeamLaserZap) {
                // 4.4: the AbilityEvent::Zap of this tick (CombatSystem::detonate_projectile leaves no hit event and the event
                // carries only the shooter). Far end: the event's point when the sim sets one, else [M] the projectile's
                // approximate spot: Protection_Radius from the owner towards the shooter (a zap fires on the first tick the
                // missile is inside the radius).
                for (const tactical::abilities::AbilityEvent& e : ab.events()) {
                    if (e.kind != tactical::abilities::AbilityEventKind::Zap || e.id != o.id) continue;
                    const bool has_point = e.point.x != 0.0f || e.point.y != 0.0f || e.point.z != 0.0f;
                    const SnapshotObject* shooter = e.target == tactical::combat::kNoObject ? nullptr : snap.find(e.target);
                    if (has_point) {
                        b.end = {e.point.x, e.point.y, e.point.z};
                    } else if (shooter) {
                        const float dx = shooter->position.x - o.position.x, dy = shooter->position.y - o.position.y,
                                    dz = shooter->position.z - o.position.z;
                        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (!(d > 0.0f)) continue;
                        const float r = std::min(d, b.style.radius > 0.0f ? b.style.radius : d);
                        b.end = {o.position.x + dx / d * r, o.position.y + dy / d * r, o.position.z + dz / d * r};
                    } else {
                        continue;
                    }
                    b.fixed_end = true;
                    snap.beams.push_back(std::move(b));
                    break;
                }
                continue;
            }
            const ObjectId target = ab.special_target(o.id, rec->gui_name_crc);
            const SnapshotObject* to = target == tactical::combat::kNoObject ? nullptr : snap.find(target);
            if (!to) continue;
            if (kind == kBeamSuperLaser) {  // draws once the active phase (Fire_Frame reached) runs
                const int32_t mx = ab.countdown_maximum(o.id, kind);
                if (mx > 0 && !(ab.countdown_phase(o.id, kind) == 1 && ab.countdown_counter(o.id, kind) < mx)) continue;
            }
            b.target = target;
            b.end_z_adjust = styles ? styles->z_adjust(to->type) : 0.0f;
            if (kind == kBeamEnergyWeapon) {  // the targeted hardpoint: the ordered one, else the best (0x394610)
                int32_t hp = combat.current_target(o.id) == target ? combat.ordered_hardpoint(o.id) : -1;
                if (hp < 0) hp = combat.choose_target_hardpoint(o.id, target);
                if (hp >= 0 && static_cast<uint32_t>(hp) < combat.hardpoint_count(target) &&
                    !combat.hardpoint_destroyed(target, static_cast<uint32_t>(hp)))
                    b.hardpoint = hp;
            }
            snap.beams.push_back(std::move(b));
        }
    }
}

void apply_battle_zaps(const sim::TacticalBattle& battle, SimSnapshot& snap, const SimSnapshot* prev, const BeamStyleTable* styles) {
    apply_beam_zaps(battle.abilities(), battle.combat(), battle.frame(), snap, prev, styles);
}

void apply_beam_zaps(const tactical::abilities::AbilitySystem& ab, const tactical::combat::CombatSystem& combat, uint32_t frame,
                     SimSnapshot& snap, const SimSnapshot* prev, const BeamStyleTable* styles) {
    if (!prev || prev->projectiles.empty()) return;
    const BeamConstants gc = styles ? styles->constants() : BeamConstants{};
    // Projectiles of the previous snapshot that are gone now (ascending serial on both sides).
    std::vector<const SnapshotProjectile*> gone;
    for (const SnapshotProjectile& p : prev->projectiles)
        if (!std::binary_search(snap.projectiles.begin(), snap.projectiles.end(), p,
                                [](const SnapshotProjectile& a, const SnapshotProjectile& b) { return a.serial < b.serial; }))
            gone.push_back(&p);
    if (gone.empty()) return;
    bool added = false;
    for (const SnapshotObject& o : snap.objects) {  // ascending id
        if (!ab.has_object(o.id) || !ab.has_ability(o.id, kBeamLaserZap)) continue;
        const tactical::abilities::UnitAbilityRecord* rec = ab.record(o.id, kBeamLaserZap);
        if (!rec || rec->gui_name_crc == 0 || !ab.special_applied(o.id, rec->gui_name_crc)) continue;
        const BeamStyle style = styles ? styles->style_for(kBeamLaserZap, rec->gui_name_crc) : default_beam_style(kBeamLaserZap, gc);
        if (!(style.radius > 0.0f)) continue;
        if (std::any_of(snap.beams.begin(), snap.beams.end(), [&](const SnapshotBeam& b) { return b.kind == kBeamLaserZap && b.owner == o.id; }))
            continue;  // the sim's own Zap event was still there (a host that reads it before clearing)
        for (const SnapshotProjectile* p : gone) {
            if (!combat.is_enemy(o.owner, p->owner)) continue;
            // Where the zap found it: one flight step on from the previous snapshot [M: the abilities run after the move].
            const Vec3 at{p->position.x + p->direction.x * p->speed, p->position.y + p->direction.y * p->speed,
                          p->position.z + p->direction.z * p->speed};
            const float dx = at.x - o.position.x, dy = at.y - o.position.y, dz = at.z - o.position.z;
            const float tol = style.radius + 0.5f;
            if (dx * dx + dy * dy + dz * dz > tol * tol) continue;
            // A projectile that hit something / expired left a hit event of its shooter near its last step: not a zap.
            const float reach = 2.0f * p->speed + 2.0f;
            bool hit = false;
            for (const tactical::combat::HitEvent& e : combat.hit_events()) {
                if (e.shooter != p->shooter || e.projectile_desc != p->desc) continue;
                const float ex = e.position.x - at.x, ey = e.position.y - at.y, ez = e.position.z - at.z;
                if (ex * ex + ey * ey + ez * ez <= reach * reach) {
                    hit = true;
                    break;
                }
            }
            if (hit) continue;
            SnapshotBeam b;
            b.kind = kBeamLaserZap;
            b.owner = o.id;
            b.frame = frame;
            b.style = style;
            b.fixed_end = true;
            b.end = at;
            snap.beams.push_back(std::move(b));
            added = true;
            break;
        }
    }
    if (added)
        std::stable_sort(snap.beams.begin(), snap.beams.end(), [](const SnapshotBeam& a, const SnapshotBeam& b) {
            return a.owner != b.owner ? a.owner < b.owner : a.kind < b.kind;
        });
}

// R5 sensor jamming (ABILITY_VISUALS §3): jammed objects (Is_Jammed, the radar filter's input) and the jammers' visuals.
void apply_battle_jamming(const sim::TacticalBattle& battle, int32_t local_player, SimSnapshot& snap, const SnapshotTypeTable* table) {
    snap.jamming = {};
    const tactical::abilities::AbilitySystem& ab = battle.abilities();
    const tactical::combat::CombatSystem& combat = battle.combat();
    const uint32_t frame = battle.frame();
    std::vector<SnapshotEffect> fx;
    for (const SnapshotObject& o : snap.objects) {  // ascending id
        if ((o.flags & kSnapCorpse) != 0) continue;
        if (combat.has_unit(o.id) && combat.is_jammed(o.id, frame)) snap.jamming.jammed.push_back(o.id);
        if (!ab.has_object(o.id)) continue;
        if (o.owner != local_player && combat.is_enemy(o.owner, local_player) && ab.is_stealthed(o.id, frame))
            snap.jamming.stealthed.push_back(o.id);  // radar blip filter: hostile stealthed objects (ABILITY_VISUALS 1.4)
        float radius = 0.0f;
        const tactical::abilities::SpecialAbilityDesc* d = ab.jamming_special(o.id, radius);
        if (!d) continue;
        if (!d->owner_particle_effect.empty()) {  // Owner_Particle_Effect: the particle replaces the blob (0x719000)
            SnapshotEffect e;
            e.id = o.id;
            e.asset = d->owner_particle_effect;
            if (table) {
                const objects::TypeIndex ti = table->find(e.asset);
                if (ti != objects::kNone && table->valid(ti) && !(*table)[ti].model.empty()) e.asset = (*table)[ti].model;
            }
            e.percent = kEffectJamming;
            e.frame = frame;
            e.bone = d->owner_particle_bone;
            fx.push_back(std::move(e));
            continue;
        }
        SnapshotJammer j;
        j.id = o.id;
        j.owner = o.owner;
        j.radius = std::isfinite(radius) && radius > 0.0f ? radius : 0.0f;
        j.allied = o.owner == local_player || !combat.is_enemy(o.owner, local_player);
        j.show_to_enemies = d->show_blob_to_enemies;
        j.blob_material = d->blob_material.empty() ? std::string("Sensor_Jamming_Blob") : d->blob_material;  // [M] default
        for (size_t k = 0; k < 4; ++k) j.blob_color[k] = d->blob_color[k];
        snap.jamming.jammers.push_back(std::move(j));
    }
    append_snapshot_effects(snap, fx);
}
#endif

#ifdef ALAMO_SIM_HAVE_BATTLES
void apply_battle_fog(const sim::TacticalBattle& battle, int32_t local_player, SimSnapshot& snap) {
    const tactical::fog::FogOfWar* fog = battle.fog();
    if (!fog) {
        snap.fog.reset();
        apply_fog_hidden(snap, nullptr);
        return;
    }
    snap.fog = fog_frame_input(fog->render_view(local_player));
    const tactical::abilities::AbilitySystem& ab = battle.abilities();
    const uint32_t frame = battle.frame();
    apply_fog_hidden(snap, [&](const SnapshotObject& o) {
        if (battle.fog_object_hidden(local_player, o.id, false)) return true;
        // HideWhenFogged also hides enemy objects that are stealthed (FOG_OF_WAR §5.4, ABILITIES_HARDPOINTS §3.7).
        return o.owner != local_player && !fog->allied(local_player, o.owner) && ab.has_object(o.id) && ab.is_stealthed(o.id, frame);
    });
}

void apply_battle_fog_reinforce(const sim::TacticalBattle& battle, int32_t local_player, SimSnapshot& snap) {
    if (!snap.fog) return;
    std::vector<uint8_t> cells;
    if (!battle.fog_reinforce_overlay(local_player, cells) || cells.size() != snap.fog->cells()) return;
    auto f = std::make_shared<FogFrameInput>(*snap.fog);
    f->reinforce = std::move(cells);
    snap.fog = std::move(f);
}
#endif

// ------------------------------------------------------------------------------------------ buffer
void SnapshotBuffer::publish(std::shared_ptr<const SimSnapshot> s) {
    std::lock_guard<std::mutex> lock(mutex_);
    pair_.prev = std::move(pair_.curr);
    pair_.curr = std::move(s);
    ++published_;
}

SnapshotBuffer::Pair SnapshotBuffer::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pair_;
}

void SnapshotBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pair_ = {};
    published_ = 0;
}

uint64_t SnapshotBuffer::published() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

// ----------------------------------------------------------------------------------- interpolation
const char* interp_mode_name(InterpMode m) { return m == InterpMode::Smooth ? "smooth" : "original"; }

float interp_lerp(float a, float b, float t) { return a + (b - a) * t; }

Vec3 interp_lerp(const Vec3& a, const Vec3& b, float t) {
    return {interp_lerp(a.x, b.x, t), interp_lerp(a.y, b.y, t), interp_lerp(a.z, b.z, t)};
}

Vec3 interp_facing(const Vec3& a, const Vec3& b, float t) {
    const Vec3 v = interp_lerp(a, b, t);
    const float len = length3(v);
    if (!(len > 1e-6f) || !finite(len)) return b;
    return {v.x / len, v.y / len, v.z / len};
}

float interp_angle(float a, float b, float t) {
    constexpr float kPi = 3.14159265f, kTwoPi = 6.28318531f;
    float d = b - a;
    if (!finite(d) || std::fabs(d) > 1.0e4f) return b;
    while (d > kPi) d -= kTwoPi;
    while (d <= -kPi) d += kTwoPi;
    return a + d * t;
}

void interpolate(const SimSnapshot* prev, const SimSnapshot& curr, float alpha, InterpMode mode, InterpolatedFrame& out) {
    if (!finite(alpha)) alpha = 1.0f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    out.tick = curr.tick;
    out.alpha = alpha;
    out.mode = mode;
    out.sim_mode = curr.mode;
    out.objects.assign(curr.objects.begin(), curr.objects.end());
    out.hardpoint_destroyed.assign(curr.hardpoint_destroyed.begin(), curr.hardpoint_destroyed.end());
    out.effects.assign(curr.effects.begin(), curr.effects.end());
    out.hardpoint_events.assign(curr.hardpoint_events.begin(), curr.hardpoint_events.end());
    out.fog = curr.fog;
    out.muzzles.assign(curr.muzzles.begin(), curr.muzzles.end());
    out.foot_plants.assign(curr.foot_plants.begin(), curr.foot_plants.end());
    out.ability_events.assign(curr.ability_events.begin(), curr.ability_events.end());
    out.projectiles.assign(curr.projectiles.begin(), curr.projectiles.end());
    out.beams.assign(curr.beams.begin(), curr.beams.end());
    out.jamming = curr.jamming;
    if (mode == InterpMode::Original || !prev || alpha >= 1.0f) return;
    {  // projectiles: both ascend by serial
        size_t k = 0;
        const std::vector<SnapshotProjectile>& pp = prev->projectiles;
        for (SnapshotProjectile& q : out.projectiles) {
            while (k < pp.size() && pp[k].serial < q.serial) ++k;
            if (k >= pp.size()) break;
            if (pp[k].serial != q.serial) continue;  // new this tick: current position
            q.position = interp_lerp(pp[k].position, q.position, alpha);
            q.direction = interp_facing(pp[k].direction, q.direction, alpha);
        }
    }
    // Both lists ascend by id: one merge walk.
    size_t j = 0;
    const std::vector<SnapshotObject>& p = prev->objects;
    for (SnapshotObject& o : out.objects) {
        while (j < p.size() && p[j].id < o.id) ++j;
        if (j >= p.size()) break;
        const SnapshotObject& a = p[j];
        if (a.id != o.id || a.type != o.type) continue;  // new this tick (or id reuse guard): current state
        o.position = interp_lerp(a.position, o.position, alpha);
        // S7 t05: a fighter's pitch fold (yaw + 180, roll + 180 near the vertical) is not blended (current attitude).
        const bool flip = (a.pitch != 0.0f || o.pitch != 0.0f) &&
                          a.facing.x * o.facing.x + a.facing.y * o.facing.y + a.facing.z * o.facing.z < 0.0f;
        if (!flip) {
            o.facing = interp_facing(a.facing, o.facing, alpha);
            o.roll = interp_angle(a.roll, o.roll, alpha);
            o.pitch = interp_angle(a.pitch, o.pitch, alpha);
        }
        o.health = interp_lerp(a.health, o.health, alpha);
        o.health_fraction = interp_lerp(a.health_fraction, o.health_fraction, alpha);
        o.fade = interp_lerp(a.fade, o.fade, alpha);
        if (a.shield_fraction >= 0.0f && o.shield_fraction >= 0.0f) o.shield_fraction = interp_lerp(a.shield_fraction, o.shield_fraction, alpha);
    }
}

float tick_alpha(uint32_t now_ms, uint32_t last_tick_ms, uint32_t period_ms) {
    if (period_ms == 0) return 1.0f;
    const uint32_t d = now_ms - last_tick_ms;  // wrapping
    if (d >= period_ms) return 1.0f;
    return static_cast<float>(d) / static_cast<float>(period_ms);
}

namespace {
// Object frame of a facing: f = forward (normalised; zero -> kIdentityFacing), u = world up made orthogonal to f
// (facing (anti)parallel to up uses +Y as the reference instead), l = u x f (left); then a pitch of `pitch` radians
// about l (positive = nose down) and a roll of `roll` radians about the pitched f. The model's columns are X = l,
// Y = -f (Alamo models face -Y), Z = u.
assets::Mat3x4 facing_frame(const Vec3& position, const Vec3& facing, float scale, float roll, float pitch) {
    Vec3 f = facing;
    float len = length3(f);
    if (!(len > 1e-6f) || !finite(len)) f = kIdentityFacing;
    else f = {f.x / len, f.y / len, f.z / len};
    Vec3 ref{0, 0, 1};
    float d = f.z;
    if (std::fabs(d) > 0.9999f) {
        ref = {0, 1, 0};
        d = f.y;
    }
    Vec3 u{ref.x - d * f.x, ref.y - d * f.y, ref.z - d * f.z};
    len = length3(u);
    u = {u.x / len, u.y / len, u.z / len};
    Vec3 l{u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x};
    if (pitch != 0.0f && finite(pitch)) {  // f' = f cos - u sin, u' = u cos + f sin (l unchanged; FIGHTER_FLYING §1.1)
        const float c = std::cos(pitch), s = std::sin(pitch);
        const Vec3 f0 = f, u0 = u;
        f = {f0.x * c - u0.x * s, f0.y * c - u0.y * s, f0.z * c - u0.z * s};
        u = {u0.x * c + f0.x * s, u0.y * c + f0.y * s, u0.z * c + f0.z * s};
    }
    if (roll != 0.0f && finite(roll)) {  // l' = l cos + u sin, u' = u cos - l sin (f unchanged)
        const float c = std::cos(roll), s = std::sin(roll);
        const Vec3 l0 = l, u0 = u;
        l = {l0.x * c + u0.x * s, l0.y * c + u0.y * s, l0.z * c + u0.z * s};
        u = {u0.x * c - l0.x * s, u0.y * c - l0.y * s, u0.z * c - l0.z * s};
    }
    const float s = finite(scale) && scale > 0.0f ? scale : 1.0f;
    assets::Mat3x4 m;
    const float cols[3][3] = {{l.x, l.y, l.z}, {-f.x, -f.y, -f.z}, {u.x, u.y, u.z}};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m.m[r][c] = cols[c][r] * s;
    m.m[0][3] = position.x;
    m.m[1][3] = position.y;
    m.m[2][3] = position.z;
    return m;
}
}  // namespace

assets::Mat3x4 object_transform(const Vec3& position, const Vec3& facing, float scale) {
    return facing_frame(position, facing, scale, 0.0f, 0.0f);
}

assets::Mat3x4 object_transform(const Vec3& position, const Vec3& facing, float scale, float roll) {
    return facing_frame(position, facing, scale, roll, 0.0f);
}

assets::Mat3x4 object_transform(const Vec3& position, const Vec3& facing, float scale, float roll, float pitch) {
    return facing_frame(position, facing, scale, roll, pitch);
}

assets::Mat3x4 object_transform(const SnapshotObject& o, float scale) {
    return facing_frame(o.position, o.facing, scale, o.roll, o.pitch);
}

}  // namespace alamo::render
