#include "alamo/render/galactic/galactic_snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>

#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/sim/sim_host.hpp"

namespace alamo::render {

namespace {

using objects::GameObjectTypes;
using objects::ObjectHandle;
using objects::ObjectId;
using objects::TypeIndex;

bool yes(std::string_view v) { return data::parse_type_bool(v); }

}  // namespace

// ------------------------------------------------------------------------------------------- types
void GalacticTypeTable::build(const GameObjectTypes& types) {
    types_ = &types;
    info_.assign(types.size(), {});
    const objects::ObjectTypeDatabase& raw = types.raw();
    for (TypeIndex t = 0; t < info_.size(); ++t) {
        GalacticTypeInfo& ti = info_[t];
        ti.name = std::string(raw.types().name[t]);
        ti.name_crc = raw.types().name_crc[t];
        ti.model = data::trim(type_text(types, t, "Galactic_Model_Name"));
        ti.destroyed_model = data::trim(type_text(types, t, "Destroyed_Galactic_Model_Name"));
        ti.fleet_override = data::trim(type_text(types, t, "Galactic_Fleet_Override_Model_Name"));
        const float sf = types.get_float(t, "Scale_Factor");
        ti.scale = std::isfinite(sf) && sf > 0.0f ? sf : 1.0f;
        const float lb = types.get_float(t, "LOD_Bias");
        ti.lod_bias = std::isfinite(lb) && lb >= 0.0f ? lb : kDefaultLodBias;
        ti.in_background = yes(data::trim(type_text(types, t, "In_Background")));
        ti.loop_idle = yes(data::trim(type_text(types, t, "Loop_Idle_Anim_00")));
        const std::string rate(data::trim(type_text(types, t, "Idle_Anim_00_Rate_Mod")));
        if (!rate.empty()) {
            const float r = std::strtof(rate.c_str(), nullptr);
            ti.idle_rate = std::isfinite(r) && r >= 0.0f ? r : 1.0f;
        }
    }
    fleet_type_ = find("Galactic_Fleet");
}

TypeIndex GalacticTypeTable::find(std::string_view name) const {
    if (!types_) return objects::kNone;
    const TypeIndex t = types_->resolve_name(name);
    return t < info_.size() ? t : objects::kNone;
}

// ----------------------------------------------------------------------------------------- colours
bool parse_color_bytes(std::string_view text, materials::float4& out) {
    float v[4] = {0, 0, 0, 255};
    int n = 0;
    size_t i = 0;
    while (i < text.size() && n < 4) {
        while (i < text.size() && (text[i] == ',' || std::isspace(static_cast<unsigned char>(text[i])))) ++i;
        if (i >= text.size()) break;
        size_t j = i;
        while (j < text.size() && text[j] != ',' && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        const std::string tok(text.substr(i, j - i));
        char* end = nullptr;
        const float f = std::strtof(tok.c_str(), &end);
        if (end == tok.c_str() || !std::isfinite(f)) return false;
        v[n++] = std::clamp(f, 0.0f, 255.0f);
        i = j;
    }
    if (n < 3) return false;
    out = {v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f};
    return true;
}

std::vector<FactionColor> load_faction_colors(const FileProvider& files) {
    std::vector<FactionColor> out;
    std::vector<uint8_t> bytes;
    if (!files.read("Data\\XML\\FactionFiles.xml", bytes)) return out;
    const data::XmlDocument list = data::XmlDocument::parse(bytes, "FactionFiles.xml");
    if (!list.ok()) return out;
    for (data::XmlNode f : list.root().children("File")) {
        std::vector<uint8_t> fb;
        const std::string name(data::trim(f.text_trimmed()));
        if (name.empty() || !files.read("Data\\XML\\" + name, fb)) continue;
        const data::XmlDocument doc = data::XmlDocument::parse(fb, name);
        if (!doc.ok()) continue;
        for (data::XmlNode n : doc.root().children("Faction")) {
            FactionColor c;
            c.name = std::string(n.attribute("Name"));
            c.neutral = yes(n.child_text("Is_Neutral"));
            c.has_color = parse_color_bytes(n.child_text("Color"), c.color);
            if (!parse_color_bytes(n.child_text("No_Colorization_Color"), c.no_colorization)) c.no_colorization = {1, 1, 1, 1};
            c.has_display_font = parse_color_bytes(n.child_text("Display_Font_Color"), c.display_font);
            c.has_big_fleet = parse_color_bytes(n.child_text("Big_Fleet_Color"), c.big_fleet);
            c.color.w = 1.0f;
            c.no_colorization.w = 1.0f;
            c.display_font.w = 1.0f;
            out.push_back(std::move(c));
        }
    }
    return out;
}

bool faction_map_color(std::span<const FactionColor> table, std::string_view name, materials::float4& out) {
    for (const FactionColor& f : table)
        if (ascii_iequals(f.name, name)) {
            if (!f.has_color) return false;
            out = f.color;
            return true;
        }
    return false;
}

bool faction_text_color(std::span<const FactionColor> table, std::string_view name, materials::float4& out) {
    for (const FactionColor& f : table)
        if (ascii_iequals(f.name, name)) {
            if (f.has_display_font) out = f.display_font;
            else if (f.has_color) out = f.color;
            else return false;
            return true;
        }
    return false;
}

materials::float4 GalacticPalette::color(int32_t owner, uint8_t& index) const {
    if (owner < 0 || static_cast<size_t>(owner) >= player.size()) {
        index = kNeutralTeam;
        return none;
    }
    const auto p = static_cast<size_t>(owner);
    index = neutral[p] ? kNeutralTeam : static_cast<uint8_t>(static_cast<uint32_t>(owner) % kMpPaletteSize);
    return player[p];
}

GalacticPalette make_galactic_palette(const galactic::GalacticSim& sim, std::span<const FactionColor> factions) {
    GalacticPalette pal;
    const MpPalette mp = default_mp_palette();
    const galactic::GalacticRules& rules = sim.rules();
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const uint32_t fi = sim.players()[p].faction;
        const std::string fname = fi < rules.factions.size() ? rules.factions[fi].name : std::string();
        const bool neutral = fi < rules.factions.size() && rules.factions[fi].is_neutral;
        materials::float4 c = mp.color[p % kMpPaletteSize];
        for (const FactionColor& f : factions)
            if (ascii_iequals(f.name, fname)) {
                if (neutral) c = f.no_colorization;
                else if (f.has_color) c = f.color;
                break;
            }
        pal.player.push_back(c);
        pal.neutral.push_back(neutral ? 1 : 0);
    }
    return pal;
}

// ---------------------------------------------------------------------------------------- snapshot
const GalacticFleetSnap* GalacticSnapshot::find_fleet(ObjectId id) const {
    const auto it = std::lower_bound(fleets.begin(), fleets.end(), id, [](const GalacticFleetSnap& f, ObjectId v) { return f.id < v; });
    return it != fleets.end() && it->id == id ? &*it : nullptr;
}

uint64_t GalacticSnapshot::digest() const {
    StateHasher h;
    h.u64(tick);
    h.i32(mode_frame);
    h.i32(galactic_day);
    h.u32(static_cast<uint32_t>(planets.size()));
    for (const GalacticPlanetSnap& p : planets) {
        h.u32(p.row);
        h.i32(p.id);
        h.u32(p.type);
        for (float f : {p.position.x, p.position.y, p.position.z, p.team_color.x, p.team_color.y, p.team_color.z, p.team_color.w,
                        p.control, p.starbase_color.x, p.starbase_color.y, p.starbase_color.z})
            h.f32(f);
        h.i32(p.owner);
        h.u8(p.team_index);
        h.i32(p.starbase_level);
        h.i32(p.starbase);
        h.u32(p.starbase_type);
        h.i32(p.starbase_owner);
        h.i32(p.ground_level);
        h.u32(p.surface_units);
        h.u32(p.orbit_fleets);
        h.u16(p.flags);
        h.i32(p.corruption_type);
        h.i32(p.corruption_effective);
        h.f32(p.corruption_fraction);
        for (float f : p.corruption_pairs) h.f32(f);
    }
    h.u32(static_cast<uint32_t>(routes.size()));
    for (const GalacticRouteSnap& r : routes) {
        h.u32(r.a);
        h.u32(r.b);
        h.u8(r.flags);
    }
    h.u32(static_cast<uint32_t>(fleets.size()));
    for (const GalacticFleetSnap& f : fleets) {
        h.i32(f.id);
        h.u32(f.index);
        h.i32(f.owner);
        h.u8(f.team_index);
        for (float v : {f.team_color.x, f.team_color.y, f.team_color.z, f.team_color.w, f.position.x, f.position.y, f.position.z,
                        f.facing.x, f.facing.y, f.facing.z})
            h.f32(v);
        h.u32(f.planet);
        h.u32(f.from);
        h.u32(f.to);
        h.u32(f.orbit_slot);
        h.i32(f.slot);
        h.u32(f.members);
        h.u32(f.model_type);
        h.u8(f.flags);
    }
    return h.digest();
}

void build_galactic_snapshot(const galactic::GalacticSim& sim, uint64_t tick, const GalacticTypeTable& types,
                             const GalacticPalette& palette, GalacticSnapshot& out, const ParallelFor& pf) {
    const objects::ObjectStore& store = sim.store();
    const GalaxyMap& map = sim.map();
    const std::span<const galactic::PlanetState> planets = sim.planets();
    const std::span<const galactic::FleetState> fleets = sim.fleets();
    out.tick = tick;
    out.mode_frame = sim.frame();
    out.galactic_day = sim.galactic_day();
    // Planets: one record per row, written per index (parallel).
    const auto n = static_cast<uint32_t>(map.planets.size());
    out.planets.assign(n, GalacticPlanetSnap{});
    pf(n, 64u, [&](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) {
            GalacticPlanetSnap& o = out.planets[i];
            o.row = i;
            o.position = map.planets.position[i];
            const uint32_t row_flags = map.planets.flags[i];
            const galactic::PlanetState* ps = i < planets.size() ? &planets[i] : nullptr;
            const bool is_planet = ps && ps->is_planet;
            // Scenery rows have no planet object: the row name is the type name (Planets.xml <Planet Name=..>).
            o.type = is_planet ? static_cast<TypeIndex>(ps->type) : types.find(map.planets.name[i]);
            if (!types.valid(o.type)) o.type = objects::kNone;
            uint16_t fl = 0;
            if (is_planet) {
                fl |= kGalPlanet;
                o.id = store.exists(ps->object) ? store.id(ps->object) : objects::kNoObjectId;
                o.owner = ps->owner;
                o.control = ps->control;
                o.starbase_level = ps->starbase_level;
                o.starbase_owner = ps->starbase_owner;
                if (store.alive(ps->starbase)) {
                    o.starbase = store.id(ps->starbase);
                    const TypeIndex st = store.type(ps->starbase);
                    o.starbase_type = types.valid(st) ? st : objects::kNone;
                    uint8_t idx = kNeutralTeam;
                    o.starbase_color = palette.color(ps->starbase_owner, idx);
                }
                uint32_t structures = 0;
                for (ObjectHandle h : ps->structures) structures += store.alive(h) ? 1u : 0u;
                o.ground_level = static_cast<int32_t>(structures);
                for (ObjectHandle h : ps->surface) o.surface_units += store.alive(h) ? 1u : 0u;
                for (uint32_t f : ps->orbit) o.orbit_fleets += (f < fleets.size() && fleets[f].alive) ? 1u : 0u;
                if (ps->hidden) fl |= kGalHidden;
                if (ps->destroyed) fl |= kGalDestroyed;
                if (ps->corruption != galactic::kCorruptionNone) fl |= kGalCorrupted;
                if (ps->in_battle) fl |= kGalInBattle;
                // GRC 4 corruption presentation: fraction at the mode frame (now = 0), pairs as the sim built them.
                const auto row = static_cast<PlanetIndex>(i);
                o.corruption_type = ps->corruption;
                o.corruption_effective = sim.effective_corruption_type(row);
                o.corruption_fraction = sim.corruption_transition_fraction(row);
                if (sim.corruption_transition_active(row)) fl |= kGalCorruptionTransition;
                const galactic::GalacticSim::CorruptionParticlePairs pp = sim.corruption_particle_pairs(row);
                if (pp.present) {
                    fl |= kGalCorruptionParticles;
                    const float pv[6] = {pp.brightness_start, pp.brightness_end, pp.scale_start, pp.scale_end, pp.energy_start, pp.energy_end};
                    std::copy(std::begin(pv), std::end(pv), std::begin(o.corruption_pairs));
                }
                // FIXPASS_GALACTIC 3.2 fading source [port inference]: gains fade with the particle object, removals with
                // ramping pairs (both are what the wrappers' rebuild leaves; the ownership change rebuilds nothing at begin).
                if (fl & kGalCorruptionTransition) {
                    const bool gaining = ps->corruption == galactic::kCorruptionNone;
                    bool ramp = false;
                    for (int k = 0; k < 6; k += 2) ramp = ramp || o.corruption_pairs[k] != o.corruption_pairs[k + 1];
                    if (gaining ? pp.present : (pp.present && ramp)) fl |= kGalCorruptionFading;
                }
            }
            o.team_color = palette.color(o.owner, o.team_index);
            const bool bg = (row_flags & kPlanetInBackground) != 0 || (types.valid(o.type) && types[o.type].in_background);
            if (bg) fl |= kGalBackground;
            if (types.valid(o.type) && !types[o.type].model.empty()) fl |= kGalDrawable;
            o.flags = fl;
        }
    });
    // Routes, route order.
    out.routes.clear();
    for (size_t r = 0; r < map.routes.size(); ++r) out.routes.push_back({map.routes.point_a[r], map.routes.point_b[r], 0});
    // Orbit slots: index in the planet's orbit list (arrival order).
    std::vector<uint32_t> slot(fleets.size(), 0);
    for (const galactic::PlanetState& p : planets)
        for (uint32_t k = 0; k < p.orbit.size(); ++k)
            if (p.orbit[k] < slot.size()) slot[p.orbit[k]] = k;
    // Fleets, fleet order, then ascending id.
    out.fleets.clear();
    for (uint32_t fi = 0; fi < fleets.size(); ++fi) {
        const galactic::FleetState& f = fleets[fi];
        if (!f.alive || !store.exists(f.object)) continue;
        GalacticFleetSnap o;
        o.id = store.id(f.object);
        o.index = fi;
        o.owner = f.owner;
        o.team_color = palette.color(f.owner, o.team_index);
        o.position = f.position;
        const float fl = std::sqrt(f.facing.x * f.facing.x + f.facing.y * f.facing.y + f.facing.z * f.facing.z);
        o.facing = (std::isfinite(fl) && fl > 1e-6f) ? f.facing : Vec3{1, 0, 0};
        o.planet = f.moving ? kInvalidPlanet : f.planet;
        o.from = f.from;
        o.to = f.to;
        o.orbit_slot = slot[fi];
        o.slot = f.moving ? static_cast<int8_t>(-1) : f.slot;
        o.model_type = types.fleet_type();
        bool overridden = false;
        uint32_t ships = 0, stealthed = 0;  // non-transport members, of them with the galactic stealth byte
        for (ObjectHandle h : f.contents) {
            if (!store.alive(h)) continue;
            ++o.members;
            if (!sim.is_company_transport(h)) {
                ++ships;
                stealthed += sim.galactic_stealth(h) ? 1u : 0u;
            }
            const TypeIndex t = store.type(h);
            if (!overridden && types.valid(t) && !types[t].fleet_override.empty()) {  // [port] first member with an override
                o.model_type = t;
                overridden = true;
            }
        }
        if (f.moving) o.flags |= kGalFleetMoving;
        if (types.valid(o.model_type)) o.flags |= kGalFleetDrawable;
        if (ships > 0 && stealthed == ships) o.flags |= kGalFleetStealthed;
        out.fleets.push_back(o);
    }
    std::sort(out.fleets.begin(), out.fleets.end(), [](const GalacticFleetSnap& a, const GalacticFleetSnap& b) { return a.id < b.id; });
}

void apply_galactic_corruption_fading(GalacticSnapshot& snap, std::span<const uint8_t> fading) {
    for (size_t i = 0; i < snap.planets.size() && i < fading.size(); ++i) {
        uint16_t& fl = snap.planets[i].flags;
        const bool on = fading[i] != 0 && (fl & kGalCorruptionTransition) != 0;
        fl = static_cast<uint16_t>(on ? (fl | kGalCorruptionFading) : (fl & ~kGalCorruptionFading));
    }
}

void apply_galactic_fog(GalacticSnapshot& snap, std::span<const uint8_t> visible, std::span<const uint8_t> route_revealed) {
    for (size_t i = 0; i < snap.planets.size(); ++i) {
        const bool fogged = i < visible.size() && visible[i] == 0;
        uint16_t& fl = snap.planets[i].flags;
        fl = static_cast<uint16_t>(fogged ? (fl | kGalFogged) : (fl & ~kGalFogged));
    }
    const auto revealed = [&](PlanetIndex p) { return p >= route_revealed.size() || route_revealed[p] != 0; };
    for (GalacticRouteSnap& r : snap.routes) {
        const bool hidden = !revealed(r.a) || !revealed(r.b);
        r.flags = static_cast<uint8_t>(hidden ? (r.flags | kGalRouteHidden) : (r.flags & ~kGalRouteHidden));
    }
}

void apply_galactic_fog(const galactic::GalacticSim& sim, int32_t player, GalacticSnapshot& snap, GalacticFogRule rule) {
    std::vector<uint8_t> visible, raw;  // empty: no fog, every route shown
    if (player >= 0 && sim.local_player() != galactic::kNoPlayer) {
        visible.assign(snap.planets.size(), 1);
        raw.assign(snap.planets.size(), 1);
        for (size_t i = 0; i < snap.planets.size(); ++i) {
            if (!(snap.planets[i].flags & kGalPlanet)) continue;  // scenery: no data pack (a route end without one counts as set)
            const auto p = static_cast<PlanetIndex>(i);
            raw[i] = sim.planet_revealed_raw(p) ? 1 : 0;
            visible[i] = rule == GalacticFogRule::RawByte ? raw[i] : (sim.is_planet_revealed(p) ? 1 : 0);
        }
    }
    apply_galactic_fog(snap, visible, raw);
}

const galactic::GalacticSim* find_galactic_sim(const sim::SimHost& host) {
    const sim::GameModeManager& m = host.modes();
    for (size_t i = 0; i < m.depth(); ++i) {
        const sim::GameMode& g = m.at(i);
        if (g.kind() != objects::ModeKind::Galactic) continue;
        return static_cast<const sim::GalacticMode&>(g).sim();
    }
    return nullptr;
}

bool capture_galactic_snapshot(const sim::SimHost& host, const GalacticTypeTable& types, const GalacticPalette& palette,
                               GalacticSnapshot& out, const ParallelFor& pf) {
    const galactic::GalacticSim* gs = find_galactic_sim(host);
    if (!gs) {
        out = GalacticSnapshot{};
        out.tick = host.frame();
        return false;
    }
    build_galactic_snapshot(*gs, host.frame(), types, palette, out, pf);
    return true;
}

// ------------------------------------------------------------------------------------------ buffer
void GalacticSnapshotBuffer::publish(std::shared_ptr<const GalacticSnapshot> s) {
    std::lock_guard<std::mutex> lock(mutex_);
    pair_.prev = std::move(pair_.curr);
    pair_.curr = std::move(s);
    ++published_;
}

GalacticSnapshotBuffer::Pair GalacticSnapshotBuffer::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pair_;
}

void GalacticSnapshotBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pair_ = {};
    published_ = 0;
}

uint64_t GalacticSnapshotBuffer::published() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
}

// ----------------------------------------------------------------------------------- interpolation
void interpolate_galactic(const GalacticSnapshot* prev, const GalacticSnapshot& curr, float alpha, InterpMode mode,
                          GalacticFrame& out) {
    if (!std::isfinite(alpha)) alpha = 1.0f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    out.tick = curr.tick;
    out.alpha = alpha;
    out.mode = mode;
    out.anim_frames = static_cast<float>(curr.mode_frame);
    out.planets.assign(curr.planets.begin(), curr.planets.end());
    out.routes.assign(curr.routes.begin(), curr.routes.end());
    out.fleets.assign(curr.fleets.begin(), curr.fleets.end());
    if (mode == InterpMode::Original || !prev || alpha >= 1.0f) return;
    out.anim_frames = interp_lerp(static_cast<float>(prev->mode_frame), static_cast<float>(curr.mode_frame), alpha);
    // Both lists ascend by id: one merge walk. Fleets new this tick keep their current state.
    size_t j = 0;
    const std::vector<GalacticFleetSnap>& p = prev->fleets;
    for (GalacticFleetSnap& o : out.fleets) {
        while (j < p.size() && p[j].id < o.id) ++j;
        if (j >= p.size()) break;
        const GalacticFleetSnap& a = p[j];
        if (a.id != o.id) continue;
        // Blend only while the fleet is in flight in both snapshots: an orbiting fleet's drawn position is its
        // orbit slot (render side), not the sim position.
        if ((a.flags & kGalFleetMoving) == 0 || (o.flags & kGalFleetMoving) == 0) continue;
        o.position = interp_lerp(a.position, o.position, alpha);
        o.facing = interp_facing(a.facing, o.facing, alpha);
    }
}

}  // namespace alamo::render
