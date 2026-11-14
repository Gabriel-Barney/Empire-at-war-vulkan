// N1 save / load of the tactical AI components (contract alamo/core/save_file.hpp; the layouts are in each class's
// header). Every read() is all-or-nothing: the record is parsed into staged values, validated, then committed; on bad
// data it returns false (r.fail()) and leaves the object unchanged. Each reads its own fields only (the owner checks
// done()). TacticalAiWorld (tactical_ai.cpp) drives these.
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "alamo/ai/tactical/damage_tracking.hpp"
#include "alamo/ai/tactical/free_store.hpp"
#include "alamo/ai/tactical/perception_cache.hpp"
#include "alamo/ai/tactical/perception_world.hpp"
#include "alamo/ai/tactical/tactical_perception.hpp"
#include "alamo/ai/tactical/target_list.hpp"
#include "alamo/ai/tactical/threat_grid.hpp"
#include "alamo/ai/tactical/zone_reachability.hpp"
#include "alamo/core/sim_crc.hpp"
#include "tactical_save_io.hpp"

namespace alamo::ai::tactical {

namespace io = save_io;

namespace {

// 2: skirmish_or_mp, PerceptionObject::parent_object (A1_READINGS §30.3); 3 (cu12 t10): PerceptionObject::valid_bombing_target
// removed (never read: IsValidBombingTarget is computed, A1_READINGS §21).
constexpr uint32_t kPerceptionWorldVersion = 3;
constexpr uint32_t kGridVersion = 1;
constexpr uint32_t kCacheVersion = 1;
constexpr uint32_t kPerceptionVersion = 1;
constexpr uint32_t kTargetListVersion = 1;
constexpr uint32_t kFreeStoreVersion = 2;  // 2: entry +0x30 (owning PP) replaces the order lock
constexpr uint32_t kScheduleVersion = 1;
constexpr uint32_t kDamageTrackingVersion = 1;
constexpr uint32_t kZoneGraphVersion = 1;

// Lower bounds of one record (count checks only).
constexpr size_t kPlayerBytes = 40;
constexpr size_t kHardPointBytes = 32;
constexpr size_t kObjectBytes = 160;
constexpr size_t kMarkerBytes = 17;
constexpr size_t kEntryBytes = 59;
constexpr size_t kEntrySnapBytes = 21;
constexpr size_t kGridObjectBytes = 69;
constexpr size_t kLeafBytes = 47;
constexpr size_t kPerceptionTargetBytes = 50;

// ---------------------------------------------------------------- PerceptionWorld records

void put_player(ByteWriter& w, const PerceptionPlayerInfo& p) {
    w.i32(p.id);
    w.str(p.faction);
    w.boolean(p.neutral_faction);
    w.boolean(p.is_human);
    w.boolean(p.attacker);
    w.str(p.ai_difficulty);
    w.f32(p.credits);
    w.i32(p.pop_cap);
    w.i32(p.pop_used);
    w.boolean(p.can_retreat);
    w.boolean(p.is_retreating);
    w.boolean(p.bombing_run_available);
    w.boolean(p.orbital_bombardment_available);
    w.f32(p.tech_level);
    io::put_vec(w, p.reinforcements);
    io::put_vec(w, p.production_queue);
}

bool get_player(ByteReader& r, PerceptionPlayerInfo& p) {
    p.id = r.i32();
    p.faction = r.str();
    p.neutral_faction = r.boolean();
    p.is_human = r.boolean();
    p.attacker = r.boolean();
    p.ai_difficulty = r.str();
    p.credits = r.f32();
    p.pop_cap = r.i32();
    p.pop_used = r.i32();
    p.can_retreat = r.boolean();
    p.is_retreating = r.boolean();
    p.bombing_run_available = r.boolean();
    p.orbital_bombardment_available = r.boolean();
    p.tech_level = r.f32();
    return r.ok() && io::get_vec(r, p.reinforcements) && io::get_vec(r, p.production_queue);
}

void put_hard_point(ByteWriter& w, const PerceptionHardPoint& h) {
    w.i32(h.type);
    w.boolean(h.grid_flag);
    w.f32(h.weapon_range);
    w.f32(h.power);
    w.f32(h.health);
    w.boolean(h.destroyable);
    w.boolean(h.destroyed);
    w.boolean(h.turret);
    io::put_vec3(w, h.world_position);
}

void get_hard_point(ByteReader& r, PerceptionHardPoint& h) {
    h.type = r.i32();
    h.grid_flag = r.boolean();
    h.weapon_range = r.f32();
    h.power = r.f32();
    h.health = r.f32();
    h.destroyable = r.boolean();
    h.destroyed = r.boolean();
    h.turret = r.boolean();
    h.world_position = io::get_vec3(r);
}

void put_object(ByteWriter& w, const PerceptionObject& o) {
    w.i32(o.id);
    w.i32(o.owner);
    w.i32(o.type);
    w.i32(o.rep_type);
    w.boolean(o.alive);
    w.boolean(o.has_unit_ai);
    w.boolean(o.team_or_hide_when_fogged);
    w.boolean(o.mobile);
    w.f32(o.speed);
    io::put_vec3(w, o.position);
    for (const float f : o.transform) w.f32(f);
    w.f32(o.radius);
    w.f32(o.health);
    w.f32(o.shield);
    w.f32(o.energy);
    w.f32(o.combat_power);
    w.f32(o.max_attack_range);
    io::put_count(w, o.hard_points.size());
    for (const PerceptionHardPoint& h : o.hard_points) put_hard_point(w, h);
    w.i32(o.team_leader);
    w.boolean(o.has_damage_tracking);
    w.i32(o.last_damage_frame);
    w.boolean(o.is_build_pad);
    w.i32(o.built_type);
    w.boolean(o.is_cash_point);
    w.f32(o.cash_point_value);
    w.boolean(o.contestable);
    w.i32(o.garrison_capacity);
    w.i32(o.garrison_occupants);
    w.f32(o.additional_population_capacity);
    io::put_vec(w, o.contained_types);
    w.boolean(o.engines_online);
    w.boolean(o.dummy_ground_structure);
    w.boolean(o.dummy_star_base);
    w.i32(o.starbase_level);
    w.boolean(o.built_structure);
    w.boolean(o.transport_landing);
    w.u64(o.visible_to);
    w.i32(o.zone);
    w.i32(o.parent_object);
}

bool get_object(ByteReader& r, PerceptionObject& o) {
    o.id = r.i32();
    o.owner = r.i32();
    o.type = r.i32();
    o.rep_type = r.i32();
    o.alive = r.boolean();
    o.has_unit_ai = r.boolean();
    o.team_or_hide_when_fogged = r.boolean();
    o.mobile = r.boolean();
    o.speed = r.f32();
    o.position = io::get_vec3(r);
    for (float& f : o.transform) f = r.f32();
    o.radius = r.f32();
    o.health = r.f32();
    o.shield = r.f32();
    o.energy = r.f32();
    o.combat_power = r.f32();
    o.max_attack_range = r.f32();
    o.hard_points.resize(io::count(r, kHardPointBytes));
    for (PerceptionHardPoint& h : o.hard_points) get_hard_point(r, h);
    o.team_leader = r.i32();
    o.has_damage_tracking = r.boolean();
    o.last_damage_frame = r.i32();
    o.is_build_pad = r.boolean();
    o.built_type = r.i32();
    o.is_cash_point = r.boolean();
    o.cash_point_value = r.f32();
    o.contestable = r.boolean();
    o.garrison_capacity = r.i32();
    o.garrison_occupants = r.i32();
    o.additional_population_capacity = r.f32();
    if (!r.ok() || !io::get_vec(r, o.contained_types)) return false;
    o.engines_online = r.boolean();
    o.dummy_ground_structure = r.boolean();
    o.dummy_star_base = r.boolean();
    o.starbase_level = r.i32();
    o.built_structure = r.boolean();
    o.transport_landing = r.boolean();
    o.visible_to = r.u64();
    o.zone = r.i32();
    o.parent_object = r.i32();
    return r.ok();
}

// ---------------------------------------------------------------- threat grid helpers

// Per-cell entry lists, sparse: u32 count of non-empty cells, then (u32 cell ascending, the list).
void put_cell_lists(ByteWriter& w, const std::vector<std::vector<uint32_t>>& cells) {
    uint32_t n = 0;
    for (const auto& c : cells) n += c.empty() ? 0u : 1u;
    w.u32(n);
    for (size_t c = 0; c < cells.size(); ++c) {
        if (cells[c].empty()) continue;
        w.u32(static_cast<uint32_t>(c));
        io::put_vec(w, cells[c]);
    }
}

// Into `out` (sized to the cell count, all empty): cells ascending, lists non-empty, entries < `entries`.
bool get_cell_lists(ByteReader& r, size_t entries, std::vector<std::vector<uint32_t>>& out) {
    const uint32_t n = io::count(r, 8);
    int64_t last = -1;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        const uint32_t c = r.u32();
        if (!r.ok() || c >= out.size() || static_cast<int64_t>(c) <= last) return io::bad(r);
        last = c;
        if (!io::get_vec(r, out[c])) return false;
        if (out[c].empty()) return io::bad(r);
        for (const uint32_t e : out[c])
            if (e >= entries) return io::bad(r);
    }
    return r.ok();
}

}  // namespace

// ================================================================ PerceptionWorld

uint64_t PerceptionWorld::types_digest() const {
    StateHasher h;
    h.u32(static_cast<uint32_t>(types.size()));
    for (const PerceptionType& t : types) {
        h.str(t.name);
        h.u64(t.category);
        h.f32(t.combat_power);
        h.boolean(t.is_hero);
        h.f32(t.bombing_run_prevention_radius);
        h.boolean(t.community_property);
        h.i32(t.builds_into);
        h.i32(t.tech_level);
        h.f32(t.population);
        h.f32(t.build_cost);
        h.i32(t.build_time_seconds);  // A2g t06
        h.f32(t.tactical_cost);
        h.u32(static_cast<uint32_t>(t.affiliation.size()));
        for (const std::string& a : t.affiliation) h.str(a);
        h.boolean(t.pp_excluded);
        h.boolean(t.under_construction);
        h.boolean(t.generic_hero);
        h.boolean(t.special_weapon);
        h.boolean(t.has_ai_script);
    }
    return h.digest();
}

void PerceptionWorld::write(ByteWriter& w) const {
    w.u32(kPerceptionWorldVersion);
    w.u8(static_cast<uint8_t>(mode));
    w.u32(frame);
    w.boolean(campaign);
    w.boolean(land_control);
    w.boolean(setup_phase);
    w.boolean(story_campaign);
    w.boolean(skirmish_or_mp);
    w.boolean(fow_active);
    io::put_rect(w, map);
    w.i32(conflict_ground_structures);
    io::put_count(w, players.size());
    for (const PerceptionPlayerInfo& p : players) put_player(w, p);
    io::put_vec(w, relation);
    w.u32(static_cast<uint32_t>(types.size()));
    w.u64(types_digest());
    io::put_count(w, objects.size());
    for (const PerceptionObject& o : objects) put_object(w, o);
    io::put_count(w, markers.size());
    for (const StartMarker& m : markers) {
        w.u8(static_cast<uint8_t>(m.kind));
        w.i32(m.player_id);
        io::put_vec3(w, m.position);
    }
}

bool PerceptionWorld::read(ByteReader& r, uint32_t& saved_type_count, uint64_t& saved_types_digest) {
    if (!io::version(r, kPerceptionWorldVersion)) return false;
    PerceptionWorld st;
    const uint8_t m = r.u8();
    if (m != static_cast<uint8_t>(TacticalMode::Land) && m != static_cast<uint8_t>(TacticalMode::Space)) r.fail();
    st.mode = static_cast<TacticalMode>(m);
    st.frame = r.u32();
    st.campaign = r.boolean();
    st.land_control = r.boolean();
    st.setup_phase = r.boolean();
    st.story_campaign = r.boolean();
    st.skirmish_or_mp = r.boolean();
    st.fow_active = r.boolean();
    st.map = io::get_rect(r);
    st.conflict_ground_structures = r.i32();
    st.players.resize(io::count(r, kPlayerBytes));
    for (PerceptionPlayerInfo& p : st.players)
        if (!get_player(r, p)) return false;
    if (!r.ok() || !io::get_vec(r, st.relation)) return false;
    const size_t np = st.players.size();
    if (!st.relation.empty() && st.relation.size() != np * np) return io::bad(r);
    const uint32_t type_count = r.u32();
    const uint64_t digest = r.u64();
    if (!r.ok()) return false;
    // This world's types (when it has some) must be the writer's; definitions are not restored.
    if (!types.empty() && (type_count != types.size() || digest != types_digest())) return io::bad(r);
    st.objects.resize(io::count(r, kObjectBytes));
    for (PerceptionObject& o : st.objects)
        if (!get_object(r, o)) return false;
    st.markers.resize(io::count(r, kMarkerBytes));
    for (StartMarker& mk : st.markers) {
        const uint8_t kind = r.u8();
        if (kind > static_cast<uint8_t>(StartMarker::Kind::AttackerEntry)) r.fail();
        mk.kind = static_cast<StartMarker::Kind>(kind);
        mk.player_id = r.i32();
        mk.position = io::get_vec3(r);
    }
    if (!r.ok()) return false;
    st.index();
    for (size_t i = 1; i < st.id_index_.size(); ++i)
        if (st.id_index_[i - 1].first == st.id_index_[i].first) return io::bad(r);  // an object id twice
    // Commit: this world's types and callbacks stay.
    st.types = std::move(types);
    st.object_fogged = std::move(object_fogged);
    st.point_fogged = std::move(point_fogged);
    st.point_blocked = std::move(point_blocked);
    st.inside_shield = std::move(inside_shield);
    st.in_active_base_shield = std::move(in_active_base_shield);
    st.parent_rep_type = std::move(parent_rep_type);
    st.zone_hint = std::move(zone_hint);
    st.nearby_open_pads = std::move(nearby_open_pads);
    st.pad_accepts = std::move(pad_accepts);
    st.hard_points = std::move(hard_points);
    *this = std::move(st);
    saved_type_count = type_count;
    saved_types_digest = digest;
    return true;
}

// ================================================================ TacticalPerceptionGrid

void TacticalPerceptionGrid::write(ByteWriter& w) const {
    w.u32(kGridVersion);
    w.u8(static_cast<uint8_t>(mode_));
    w.u32(players_);
    w.i32(fow_x_);
    w.i32(fow_y_);
    w.i32(nx_);
    w.i32(ny_);
    for (const float f : {left_, top_, right_, bottom_, cell_w_, cell_h_, half_diag_}) w.f32(f);
    w.u64(category_union_);
    io::put_count(w, entries_.size());
    for (const Entry& e : entries_) {
        w.i32(e.object);
        w.i32(e.hard_point);
        w.f32(e.radius);
        w.f32(e.gx);
        w.f32(e.gy);
        w.f32(e.area);
        w.i32(e.center_cell);
        w.boolean(e.projected);
        w.i32(e.c0);
        w.i32(e.c1);
        w.i32(e.r0);
        w.i32(e.r1);
        w.f32(e.v);
        w.boolean(e.affects_snapshot);
        io::put_vec(w, e.hostile);
        io::put_count(w, e.snap.size());
        for (const EntrySnap& s : e.snap) {
            w.f32(s.radius);
            w.f32(s.gx);
            w.f32(s.gy);
            w.f32(s.area);
            w.i32(s.center_cell);
            w.boolean(s.valid);
        }
        w.boolean(e.in_use);
    }
    io::put_vec(w, free_entries_);
    put_cell_lists(w, live_);
    // Per-player layers (players_ of them on a valid grid, none on an empty one).
    io::put_count(w, threat_.size());
    const uint32_t cells = cell_count();
    for (size_t p = 0; p < threat_.size(); ++p) {
        auto used = [&](uint32_t c) {
            return std::bit_cast<uint32_t>(threat_[p][c]) != 0 || std::bit_cast<uint32_t>(snapshot_[p][c]) != 0 || last_seen_[p][c] != 0;
        };
        uint32_t n = 0;
        for (uint32_t c = 0; c < cells; ++c) n += used(c) ? 1u : 0u;
        w.u32(n);
        for (uint32_t c = 0; c < cells; ++c) {
            if (!used(c)) continue;
            w.u32(c);
            w.f32(threat_[p][c]);
            w.f32(snapshot_[p][c]);
            w.i32(last_seen_[p][c]);
        }
        put_cell_lists(w, memory_[p]);
        io::put_vec(w, player_objects_[p]);
    }
    io::put_count(w, objects_.size());
    for (const auto& [id, g] : objects_) {
        w.i32(id);
        w.i32(g.owner);
        io::put_vec(w, g.entries);
        w.u32(g.next_check);
        for (const float f : g.transform) w.f32(f);
        io::put_vec(w, g.fogged);
        w.boolean(g.team_or_hide);
    }
    io::put_vec(w, registration_order_);
}

bool TacticalPerceptionGrid::read(ByteReader& r) {
    if (!io::version(r, kGridVersion)) return false;
    // Geometry: must be this grid's own (init() from the same map / constants / players).
    const uint8_t m = r.u8();
    const uint32_t np = r.u32();
    const int32_t fx = r.i32(), fy = r.i32(), gx = r.i32(), gy = r.i32();
    std::array<float, 7> geo{};
    for (float& f : geo) f = r.f32();
    if (!r.ok()) return false;
    const std::array<float, 7> own{left_, top_, right_, bottom_, cell_w_, cell_h_, half_diag_};
    bool same = m == static_cast<uint8_t>(mode_) && np == players_ && fx == fow_x_ && fy == fow_y_ && gx == nx_ && gy == ny_;
    for (size_t i = 0; i < geo.size(); ++i) same = same && io::same_bits(geo[i], own[i]);
    if (!same) return io::bad(r);
    const uint64_t cu = r.u64();
    const uint32_t cells = cell_count();
    std::vector<Entry> es(io::count(r, kEntryBytes));
    for (Entry& e : es) {
        e.object = r.i32();
        e.hard_point = r.i32();
        e.radius = r.f32();
        e.gx = r.f32();
        e.gy = r.f32();
        e.area = r.f32();
        e.center_cell = r.i32();
        e.projected = r.boolean();
        e.c0 = r.i32();
        e.c1 = r.i32();
        e.r0 = r.i32();
        e.r1 = r.i32();
        e.v = r.f32();
        e.affects_snapshot = r.boolean();
        if (!r.ok() || !io::get_vec(r, e.hostile)) return false;
        e.snap.resize(io::count(r, kEntrySnapBytes));
        for (EntrySnap& s : e.snap) {
            s.radius = r.f32();
            s.gx = r.f32();
            s.gy = r.f32();
            s.area = r.f32();
            s.center_cell = r.i32();
            s.valid = r.boolean();
        }
        e.in_use = r.boolean();
        if (!r.ok()) return false;
        if ((!e.hostile.empty() && e.hostile.size() != players_) || (!e.snap.empty() && e.snap.size() != players_) ||
            (e.projected && (e.c0 < 0 || e.r0 < 0 || e.c1 >= nx_ || e.r1 >= ny_)))
            return io::bad(r);
    }
    std::vector<uint32_t> fr;
    if (!io::get_vec(r, fr)) return false;
    for (const uint32_t i : fr)
        if (i >= es.size() || es[i].in_use) return io::bad(r);
    std::vector<std::vector<uint32_t>> live(cells);
    if (!get_cell_lists(r, es.size(), live)) return false;
    const uint32_t layers = r.u32();
    if (!r.ok() || layers != (valid() ? players_ : 0u)) return io::bad(r);
    std::vector<std::vector<float>> thr(layers, std::vector<float>(cells, 0.0f)), snp(layers, std::vector<float>(cells, 0.0f));
    std::vector<std::vector<int32_t>> seen(layers, std::vector<int32_t>(cells, 0));
    std::vector<std::vector<std::vector<uint32_t>>> mem(layers, std::vector<std::vector<uint32_t>>(cells));
    std::vector<std::vector<int32_t>> pobj(layers);
    for (uint32_t p = 0; p < layers; ++p) {
        const uint32_t n = io::count(r, 16);
        int64_t last = -1;
        for (uint32_t i = 0; i < n && r.ok(); ++i) {
            const uint32_t c = r.u32();
            const float t = r.f32(), s = r.f32();
            const int32_t ls = r.i32();
            // Ascending cells, each with a non-zero value (the writer's sparse form).
            if (!r.ok() || c >= cells || static_cast<int64_t>(c) <= last ||
                (std::bit_cast<uint32_t>(t) == 0 && std::bit_cast<uint32_t>(s) == 0 && ls == 0))
                return io::bad(r);
            last = c;
            thr[p][c] = t;
            snp[p][c] = s;
            seen[p][c] = ls;
        }
        if (!r.ok() || !get_cell_lists(r, es.size(), mem[p]) || !io::get_vec(r, pobj[p])) return false;
    }
    std::map<int32_t, GridObject> objs;
    const uint32_t no = io::count(r, kGridObjectBytes);
    for (uint32_t i = 0; i < no && r.ok(); ++i) {
        const int32_t id = r.i32();
        GridObject g;
        g.owner = r.i32();
        if (!io::get_vec(r, g.entries)) return false;
        g.next_check = r.u32();
        for (float& f : g.transform) f = r.f32();
        if (!r.ok() || !io::get_vec(r, g.fogged)) return false;
        g.team_or_hide = r.boolean();
        if (!r.ok() || (!objs.empty() && objs.rbegin()->first >= id) || g.fogged.size() != players_ ||
            (g.owner != kNoPlayerIndex && (g.owner < 0 || static_cast<uint32_t>(g.owner) >= layers)))
            return io::bad(r);
        for (const uint32_t ei : g.entries)
            if (ei >= es.size() || !es[ei].in_use || es[ei].object != id || es[ei].snap.size() != players_) return io::bad(r);
        objs.emplace_hint(objs.end(), id, std::move(g));
    }
    std::vector<int32_t> order;
    if (!r.ok() || !io::get_vec(r, order)) return false;
    category_union_ = cu;
    entries_ = std::move(es);
    free_entries_ = std::move(fr);
    live_ = std::move(live);
    threat_ = std::move(thr);
    snapshot_ = std::move(snp);
    last_seen_ = std::move(seen);
    memory_ = std::move(mem);
    player_objects_ = std::move(pobj);
    objects_ = std::move(objs);
    registration_order_ = std::move(order);
    return true;
}

// ================================================================ LeafCache

void LeafCache::write(ByteWriter& w) const {
    std::lock_guard<std::mutex> lock(mu_);
    w.u32(kCacheVersion);
    io::put_count(w, map_.size());
    for (const auto& [k, e] : map_) {
        w.u8(k.eval_kind);
        w.i32(k.eval_a);
        w.i32(k.eval_b);
        w.u16(k.leaf);
        w.i32(k.self);
        w.i32(k.target);
        w.i32(k.enemy);
        w.i32(k.human);
        w.i32(k.location);
        w.str(k.params);
        w.f64(e.value);
        w.u32(e.stamp);
    }
    w.u64(hits_);
}

bool LeafCache::read(ByteReader& r) {
    if (!io::version(r, kCacheVersion)) return false;
    std::map<LeafCacheKey, Entry> m;
    const uint32_t n = io::count(r, kLeafBytes);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        LeafCacheKey k;
        k.eval_kind = r.u8();
        k.eval_a = r.i32();
        k.eval_b = r.i32();
        k.leaf = r.u16();
        k.self = r.i32();
        k.target = r.i32();
        k.enemy = r.i32();
        k.human = r.i32();
        k.location = r.i32();
        k.params = r.str();
        Entry e;
        e.value = r.f64();
        e.stamp = r.u32();
        if (!r.ok() || (!m.empty() && !(m.rbegin()->first < k))) return io::bad(r);  // key order
        m.emplace_hint(m.end(), std::move(k), e);
    }
    const uint64_t h = r.u64();
    if (!r.ok()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    map_ = std::move(m);
    hits_ = h;
    return true;
}

// ================================================================ TacticalPerception

void TacticalPerception::write(ByteWriter& w) const {
    w.u32(kPerceptionVersion);
    w.u8(static_cast<uint8_t>(mode_));
    io::put_count(w, zones_.size());
    grid_.write(w);
    io::put_count(w, targets_.size());
    for (const PerceptionTarget& t : targets_) {
        w.u8(static_cast<uint8_t>(t.kind));
        w.i32(t.object_id);
        w.i32(t.owner_at_registration);
        w.boolean(t.released);
        w.i32(t.zone);
        w.i32(t.cell_x);
        w.i32(t.cell_y);
        io::put_rect(w, t.rect);
        io::put_vec3(w, t.center);
    }
    cache_.write(w);
}

bool TacticalPerception::read(ByteReader& r) {
    if (!io::version(r, kPerceptionVersion)) return false;
    const uint8_t m = r.u8();
    const uint32_t nz = r.u32();
    if (!r.ok() || m != static_cast<uint8_t>(mode_) || nz != zones_.size()) return io::bad(r);
    TacticalPerceptionGrid g = grid_;
    if (!g.read(r)) return false;
    std::vector<PerceptionTarget> ts(io::count(r, kPerceptionTargetBytes));
    for (PerceptionTarget& t : ts) {
        const uint8_t kind = r.u8();
        if (kind > static_cast<uint8_t>(PerceptionTarget::Kind::SpaceCell)) r.fail();
        t.kind = static_cast<PerceptionTarget::Kind>(kind);
        t.object_id = r.i32();
        t.owner_at_registration = r.i32();
        t.released = r.boolean();
        t.zone = r.i32();
        t.cell_x = r.i32();
        t.cell_y = r.i32();
        t.rect = io::get_rect(r);
        t.center = io::get_vec3(r);
    }
    if (!r.ok()) return false;
    // The handle tables are rebuilt: registration appends one target per object / zone / cell, so the target list in
    // handle order gives them back exactly (units sorted by object id, zones and cells in registration order).
    std::vector<std::pair<int32_t, int32_t>> uh, zh;
    std::vector<std::pair<int64_t, int32_t>> ch;
    for (size_t i = 0; i < ts.size(); ++i) {
        const auto h = static_cast<int32_t>(i);
        const PerceptionTarget& t = ts[i];
        if (t.kind == PerceptionTarget::Kind::Unit) uh.emplace_back(t.object_id, h);
        else if (t.kind == PerceptionTarget::Kind::LandZone) zh.emplace_back(t.zone, h);
        else ch.emplace_back((static_cast<int64_t>(t.cell_x) << 32) | static_cast<int64_t>(static_cast<uint32_t>(t.cell_y)), h);
    }
    std::sort(uh.begin(), uh.end());
    auto unique_keys = [](auto v) {
        std::sort(v.begin(), v.end());
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i - 1].first == v[i].first) return false;
        return true;
    };
    if (!unique_keys(uh) || !unique_keys(zh) || !unique_keys(ch)) return io::bad(r);  // registered twice
    if (!cache_.read(r)) return false;  // the last fallible step (all-or-nothing itself)
    grid_ = std::move(g);
    targets_ = std::move(ts);
    unit_handles_ = std::move(uh);
    zone_handles_ = std::move(zh);
    cell_handles_ = std::move(ch);
    rebuild_zone_name_handles();  // TACTICAL_GAPS 6.8 (derived from the zones and the targets in handle order)
    return true;
}

// ================================================================ TacticalTargetList

void TacticalTargetList::write(ByteWriter& w) const {
    w.u32(kTargetListVersion);
    w.u8(static_cast<uint8_t>(mode_));
    io::put_count(w, targets_.size());
    for (const TacticalTarget& t : targets_) io::put_target(w, t);
    io::put_vec(w, registered_);
}

bool TacticalTargetList::read(ByteReader& r) {
    if (!io::version(r, kTargetListVersion)) return false;
    const uint8_t m = r.u8();
    if (!r.ok() || m != static_cast<uint8_t>(mode_)) return io::bad(r);
    std::vector<TacticalTarget> ts(io::count(r, io::kTargetBytes));
    for (TacticalTarget& t : ts)
        if (!io::get_target(r, t)) return false;
    std::vector<int32_t> reg;
    if (!r.ok() || !io::get_vec(r, reg)) return false;
    for (size_t i = 1; i < reg.size(); ++i)
        if (reg[i - 1] >= reg[i]) return io::bad(r);  // sorted lookup table
    targets_ = std::move(ts);
    registered_ = std::move(reg);
    return true;
}

// ================================================================ free store

void TacticalFreeStore::write(ByteWriter& w) const {
    w.u32(kFreeStoreVersion);
    io::put_count(w, entries_.size());
    for (const auto& [id, e] : entries_) {
        w.i32(id);
        w.u32(e.type_id);
        w.i32(e.owner_pp);
    }
}

bool TacticalFreeStore::read(ByteReader& r) {
    if (!io::version(r, kFreeStoreVersion)) return false;
    std::map<int32_t, Entry> es;
    std::map<uint32_t, int32_t> per;
    const uint32_t n = io::count(r, 9);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        const int32_t id = r.i32();
        Entry e;
        e.type_id = r.u32();
        e.owner_pp = r.i32();
        if (!r.ok() || e.owner_pp < -1 || (!es.empty() && es.rbegin()->first >= id)) return io::bad(r);
        es.emplace_hint(es.end(), id, e);
        ++per[e.type_id];  // the type counters are the entries' (admit / remove keep them so)
    }
    if (!r.ok()) return false;
    entries_ = std::move(es);
    per_type_ = std::move(per);
    return true;
}

void FreeStoreSchedule::write(ByteWriter& w) const {
    w.u32(kScheduleVersion);
    w.boolean(service_rate.has_value());
    w.f64(service_rate.value_or(0.0));
    w.boolean(unit_service_rate.has_value());
    w.f64(unit_service_rate.value_or(0.0));
    w.f64(last_service);
    w.f64(last_unit_service);
}

bool FreeStoreSchedule::read(ByteReader& r) {
    if (!io::version(r, kScheduleVersion)) return false;
    const bool has_rate = r.boolean();
    const double rate = r.f64();
    const bool has_unit_rate = r.boolean();
    const double unit_rate = r.f64();
    const double last = r.f64(), last_unit = r.f64();
    if (!r.ok()) return false;
    service_rate.reset();
    if (has_rate) service_rate = rate;
    unit_service_rate.reset();
    if (has_unit_rate) unit_service_rate = unit_rate;
    last_service = last;
    last_unit_service = last_unit;
    return true;
}

// ================================================================ DamageTrackingStore

void DamageTrackingStore::write(ByteWriter& w) const {
    w.u32(kDamageTrackingVersion);
    io::put_count(w, objects_.size());
    for (const auto& [id, t] : objects_) {
        w.i32(id);
        w.f32(t.max_health);
        w.f32(t.max_shield);
        io::put_count(w, t.by_attacker.size());
        for (const auto& [a, d] : t.by_attacker) {
            w.i32(a);
            w.f32(d);
        }
    }
}

bool DamageTrackingStore::read(ByteReader& r) {
    if (!io::version(r, kDamageTrackingVersion)) return false;
    std::map<int32_t, Tracked> objs;
    const uint32_t n = io::count(r, 16);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        const int32_t id = r.i32();
        Tracked t;
        t.max_health = r.f32();
        t.max_shield = r.f32();
        const uint32_t na = io::count(r, 8);
        for (uint32_t k = 0; k < na && r.ok(); ++k) {
            const int32_t a = r.i32();
            const float d = r.f32();
            if (!r.ok() || (!t.by_attacker.empty() && t.by_attacker.rbegin()->first >= a)) return io::bad(r);
            t.by_attacker.emplace_hint(t.by_attacker.end(), a, d);
        }
        if (!r.ok() || (!objs.empty() && objs.rbegin()->first >= id)) return io::bad(r);
        objs.emplace_hint(objs.end(), id, std::move(t));
    }
    if (!r.ok()) return false;
    objects_ = std::move(objs);
    return true;
}

// ================================================================ ZoneGraph

void ZoneGraph::write(ByteWriter& w) const {
    w.u32(kZoneGraphVersion);
    w.u32(size());
    for (const uint32_t c : node_cost_) w.u32(c);
    StateHasher h;
    hash(h);
    w.u64(h.digest());
}

bool ZoneGraph::read(ByteReader& r) {
    if (!io::version(r, kZoneGraphVersion)) return false;
    const uint32_t n = r.u32();
    if (!r.ok() || n != size()) return io::bad(r);
    std::vector<uint32_t> costs(n);
    for (uint32_t& c : costs) c = r.u32();
    const uint64_t digest = r.u64();
    if (!r.ok()) return false;
    // The all-pairs matrices are rebuilt from the node costs (the §6.1 weights, as recompute_with_threat runs them).
    ZoneGraph g = *this;
    g.node_cost_ = std::move(costs);
    g.recompute();
    StateHasher h;
    g.hash(h);
    if (h.digest() != digest) return io::bad(r);
    *this = std::move(g);
    return true;
}

}  // namespace alamo::ai::tactical
