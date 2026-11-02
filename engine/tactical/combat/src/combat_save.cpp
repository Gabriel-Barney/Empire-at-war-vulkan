// N1 stage 2 - save / load during a tactical battle (contract: alamo/core/save_file.hpp) for CombatSystem, its
// ModifierSets and WeatherSystem. Little-endian, field by field, counts before variable-length data, every container in
// its own (deterministic) order: units ascending id, hardpoints in the flat table order, projectiles in serial order,
// queues / lists / events in insertion order. Floats as bit patterns. No pointers: units, targets, sources and owners
// are object ids / player indices; descriptors are CombatData indices.
//
// CombatSystem layout (version kCombatSaveVersion):
//   u32 version
//   definition counts (checked, not restored): players, unit / hardpoint / projectile / ability descriptors, priority sets
//   u32 frame_, u32 next_serial_
//   relations: P x P u8; special-weapon hardpoint flags: P x u8; player ModifierSets (P, player order)
//   10 x f32 weather accuracy
//   u32 + units (every Unit field, incl. its ModifierSet and the caches: reduced modifier values + the versions they
//         were taken at, the own / squad Mod_Multiplier products, the per-tick scratch bytes - restored verbatim, so
//         reads before the next phase A see the saved values)
//   u32 + flat hardpoint table (each unit's [hp_begin, hp_begin + hp_count) range is validated)
//   u32 + projectiles (dead-not-flushed ones too), u32 + spline-rocket states (refixf t01, serial order: one per
//         projectile with the rocket flag; flags, s, budget, FireAtPosition, launch point, last velocity, the spline's
//         control points, segments (12 coefficients + length) and total), u32 + delayed damage queue
//   u32 + base-power entries, u32 + hardpoint repairers, u32 + team heads (phase A output read by const queries)
//   this tick's outputs: deaths, hardpoint events, smoke, damage, hit, shot, survivor events
//   6 x u64 stats
// Rebuilt on read, not saved: per-unit candidate lists and sweep results (phase C / E scratch, written before read),
// emitter / leader / effect scratch, query contexts, the diminishing-firepower spline (from the config), the rocket
// splines' polyline tables (plan-time scratch) and the rocket terrain (host-attached).
//
// WeatherSystem layout (version kWeatherSaveVersion): bool active; u32 scenario index in the WeatherData (0xFFFFFFFF
// when not found) + str scenario name (active only); every state field; u32 + events.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/tactical/combat/combat_system.hpp"
#include "alamo/tactical/combat/modifiers.hpp"
#include "alamo/tactical/combat/weather.hpp"

namespace alamo::tactical::combat {

namespace {

constexpr uint32_t kCombatSaveVersion = 9;  // 2: refix t02 INFECTION state, refix t03 death clone key; 3: refixf t01 rockets;
                                            // 4: refixf t06 attack goal / weapon aim / order lock; 5: refixf t06 scan
                                            // target hardpoint (auto_hp); 6: a1g t03 damage event hull_lost; 7: S8 t04
                                            // grenade flight / sticky state; 8: S8 t06 stuck_offset in the parent's frame
                                            // (same layout, new meaning), S7 t15 A2 last weapon service frame, S8 t06 B4
                                            // fire-bone cycle index; 9: S8 t26 fire-at-position order
constexpr uint32_t kWeatherSaveVersion = 1;
constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

// Element count, bounded by the bytes left (each element takes at least `min_bytes`), so corrupt counts never allocate.
bool get_count(ByteReader& r, uint32_t& n, size_t min_bytes) {
    n = r.u32();
    return r.ok() && static_cast<uint64_t>(n) * static_cast<uint64_t>(min_bytes) <= static_cast<uint64_t>(r.remaining());
}

void put_vec3(ByteWriter& w, const Vec3& v) { w.f32(v.x).f32(v.y).f32(v.z); }

Vec3 get_vec3(ByteReader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

}  // namespace

// ---- ModifierSet ----------------------------------------------------------------------------------------------------

void ModifierSet::write(ByteWriter& w) const {
    w.u32(version_);
    w.u32(static_cast<uint32_t>(entries_.size()));
    for (const ModifierEntry& e : entries_) {
        w.f32(e.value).u32(e.expiry).i32(e.category).u32(e.source_crc).i32(e.source_id).u8(e.list);
    }
}

bool ModifierSet::read(ByteReader& r) {
    const uint32_t version = r.u32();
    uint32_t n = 0;
    if (!r.ok() || !get_count(r, n, 21)) return false;
    std::vector<ModifierEntry> entries(n);
    for (ModifierEntry& e : entries) {
        e.value = r.f32();
        e.expiry = r.u32();
        e.category = r.i32();
        e.source_crc = r.u32();
        e.source_id = r.i32();
        e.list = r.u8();
        if (!r.ok() || e.list >= kModListCount) return false;
    }
    entries_ = std::move(entries);
    version_ = version;
    return true;
}

// ---- CombatSystem ---------------------------------------------------------------------------------------------------

void CombatSystem::write(ByteWriter& w) const {
    const auto np = static_cast<uint32_t>(players_.size());
    w.u32(kCombatSaveVersion);
    w.u32(np);
    w.u32(static_cast<uint32_t>(data_->units.size()));
    w.u32(static_cast<uint32_t>(data_->hardpoints.size()));
    w.u32(static_cast<uint32_t>(data_->projectiles.size()));
    w.u32(static_cast<uint32_t>(data_->abilities.size()));
    w.u32(static_cast<uint32_t>(data_->priority_sets.size()));
    w.u32(frame_).u32(next_serial_);

    for (const uint8_t e : enemy_) w.u8(e);
    for (const uint8_t f : sw_hp_enabled_) w.u8(f);
    for (const ModifierSet& s : player_mods_) s.write(w);
    for (const float m : weather_acc_) w.f32(m);

    w.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        w.i32(u.id).u32(u.desc).i32(u.owner).u32(u.team).u8(u.team_leader).i32(u.damage_parent).u32(u.flags);
        put_vec3(w, u.pos);
        put_vec3(w, u.last_pos);
        put_vec3(w, u.facing);
        put_vec3(w, u.prev_pos);
        w.f32(u.health).f32(u.shield).u8(u.shield_depleted).u32(u.shield_depleted_frame).i32(u.shield_regen_countdown);
        w.u32(u.next_scan).i32(u.auto_target).i32(u.ordered_target).f32(u.target_score);
        w.i32(u.weapon_cooldown).i32(u.weapon_pulses).u8(u.ability_active[0]).u8(u.ability_active[1]);
        w.u32(u.hp_begin).u32(u.hp_count).u8(u.ever_hit).u32(u.last_hit_frame);
        for (const float b : u.dmg_bucket) w.f32(b);
        for (const uint32_t s : u.dmg_second) w.u32(s);
        w.i32(u.killer);
        // S9
        w.i32(u.ordered_hp).f32(u.energy).i32(u.energy_countdown).u8(u.burning).i32(u.burn_countdown);
        w.u32(u.ion_stun_end).u32(u.jammed_frame).u8(u.ever_jammed).u8(u.regen_blocked).f32(u.emitter_radius);
        // OI abilities
        w.i32(u.priority_override).u32(u.stun_end).u32(u.invuln_end).u32(u.emitter_entries).i32(u.weapon_index);
        w.u8(u.acquired);
        w.i32(u.goal_target).i32(u.goal_hp).i32(u.aim_target).i32(u.aim_hp).u8(u.locked).i32(u.auto_hp);  // refixf t06
        w.u32(u.weapon_last).u32(u.weapon_bone);  // S7 t15 A2 / S8 t06 B4
        put_vec3(w, u.lob_point);  // S8 t26 fire-at-position order
        w.u8(u.lob_pending).u8(u.lob_charged).u32(u.lob_stamp);
        // refix t02: INFECTION behaviour
        w.u8(u.infect_attached).u32(u.infect_suspend).i32(u.infect_desc).i32(u.infect_player).i32(u.infect_source);
        w.u32(u.infect_end).u32(u.infect_next_damage).u32(static_cast<uint32_t>(u.infect_exposure.size()));
        for (const auto& [id, f] : u.infect_exposure) w.i32(id).u32(f);
        u.mods.write(w);
        // caches
        for (const float v : u.mod_r) w.f32(v);
        w.u32(u.mod_ver).u32(u.player_ver).u8(u.mod_valid);
        for (const float v : u.own_mult) w.f32(v);
        for (const float v : u.mult) w.f32(v);
        w.u16(u.squad_seen).u8(u.needs_scan).u8(u.scan_due);
    }

    w.u32(static_cast<uint32_t>(hps_.size()));
    for (const Hardpoint& h : hps_) {
        w.u32(h.desc).f32(h.health).i32(h.cooldown).i32(h.pulses).u8(h.destroyed).u8(h.disabled);
        w.i32(h.target).i32(h.target_hp).i32(h.opp_target).i32(h.opp_hp).u32(h.opp_next_scan);
    }

    w.u32(static_cast<uint32_t>(projectiles_.size()));
    for (const Projectile& p : projectiles_) {
        w.u32(p.serial).u32(p.desc).i32(p.shooter).i32(p.owner).i32(p.target).i32(p.target_hp);
        put_vec3(w, p.pos);
        put_vec3(w, p.dir);
        w.f32(p.speed).f32(p.travelled).f32(p.max_travel).u32(p.spawn_frame).f32(p.damage).u16(p.damage_type).u8(p.kind);
        w.u8(p.alive).u32(p.jammed_frame).u8(p.ever_jammed).f32(p.fuse).u8(p.rocket);
        w.u8(p.lob).u8(p.stuck).f32(p.gravity).i32(p.stuck_to);  // S8 t04: grenade flight
        put_vec3(w, p.launch);
        put_vec3(w, p.lob_dir);
        put_vec3(w, p.stuck_offset);
    }
    w.u32(static_cast<uint32_t>(rockets_.size()));
    for (const Rocket& rk : rockets_) {
        w.u32(rk.serial).u8(rk.planned).f32(rk.s).f32(rk.budget);
        put_vec3(w, rk.fire_at);
        put_vec3(w, rk.launch);
        put_vec3(w, rk.last_vel);
        w.u32(static_cast<uint32_t>(rk.path.count()));
        for (const Vec3& v : rk.path.points()) put_vec3(w, v);
        w.u32(static_cast<uint32_t>(rk.path.segments().size()));
        for (const RocketSpline::Segment& sg : rk.path.segments()) {
            for (const float c : sg.k) w.f32(c);
            w.f32(sg.length);
        }
        w.f32(rk.path.total());
    }

    w.u32(static_cast<uint32_t>(delayed_.size()));
    for (const Delayed& d : delayed_) {
        w.i32(d.victim).i32(d.frames).f32(d.amount).u16(d.damage_type).u8(d.kind).u8(d.source_is_projectile);
        w.i32(d.source).i32(d.projectile_desc).i32(d.hardpoint);
    }

    w.u32(static_cast<uint32_t>(base_power_.size()));
    for (const auto& [pl, src] : base_power_) w.i32(pl).i32(src);
    w.u32(static_cast<uint32_t>(repairs_.size()));
    for (const Repairer& rp : repairs_) w.i32(rp.unit).u32(rp.hp).i32(rp.player);
    w.u32(static_cast<uint32_t>(team_heads_.size()));
    for (const auto& [team, id] : team_heads_) w.u32(team).i32(id);

    // this tick's outputs
    w.u32(static_cast<uint32_t>(deaths_.size()));
    for (const DeathEvent& e : deaths_)
        w.i32(e.id).i32(e.killer).u8(e.kind).i32(e.death_clone).i32(e.clone_key).i32(e.death_projectile).boolean(e.spin_away);
    w.u32(static_cast<uint32_t>(hp_events_.size()));
    for (const HardpointEvent& e : hp_events_) {
        w.i32(e.owner).u32(e.index).u8(static_cast<uint8_t>(e.kind)).boolean(e.owner_dying).boolean(e.last_of_kind);
        w.i32(e.explosion_type).i32(e.breakoff_type);
    }
    w.u32(static_cast<uint32_t>(smoke_events_.size()));
    for (const SmokeEvent& e : smoke_events_) w.i32(e.id).u8(e.percent);
    w.u32(static_cast<uint32_t>(damage_events_.size()));
    for (const DamageEvent& e : damage_events_) {
        w.i32(e.victim).i32(e.source).i32(e.source_owner).boolean(e.source_is_projectile).i32(e.projectile_desc);
        w.u8(e.kind).i32(e.hardpoint).f32(e.after_multipliers).f32(e.absorbed).f32(e.energy_absorbed);
        w.f32(e.to_hardpoint).f32(e.to_hull).f32(e.hull_lost).boolean(e.killed).u32(e.frame);
    }
    w.u32(static_cast<uint32_t>(hit_events_.size()));
    for (const HitEvent& e : hit_events_) {
        w.i32(e.victim).i32(e.shooter).i32(e.owner).u32(e.projectile_desc);
        put_vec3(w, e.position);
        w.boolean(e.absorbed).boolean(e.armor_reduced).boolean(e.delayed).i32(e.pick);
    }
    w.u32(static_cast<uint32_t>(shot_events_.size()));
    for (const ShotEvent& e : shot_events_) w.i32(e.shooter).i32(e.target).i32(e.target_owner);
    w.u32(static_cast<uint32_t>(survivor_events_.size()));
    for (const SurvivorEvent& e : survivor_events_) {
        w.i32(e.dead).i32(e.type).i32(e.owner);
        put_vec3(w, e.position);
        put_vec3(w, e.facing);
        w.boolean(e.respawn).u32(e.due_frame);
    }

    w.u64(stats_.scans).u64(stats_.shots).u64(stats_.hits).u64(stats_.expired).u64(stats_.deaths).u64(stats_.resweeps);
}

bool CombatSystem::read(ByteReader& r) {
    if (r.u32() != kCombatSaveVersion || !r.ok()) return false;
    const auto np = static_cast<uint32_t>(players_.size());
    const uint32_t c_players = r.u32(), c_units = r.u32(), c_hps = r.u32(), c_proj = r.u32(), c_ab = r.u32(),
                   c_prio = r.u32();
    if (!r.ok() || c_players != np || c_units != data_->units.size() || c_hps != data_->hardpoints.size() ||
        c_proj != data_->projectiles.size() || c_ab != data_->abilities.size() || c_prio != data_->priority_sets.size())
        return false;
    const auto unit_descs = static_cast<uint32_t>(data_->units.size());
    const auto hp_descs = static_cast<uint32_t>(data_->hardpoints.size());
    const auto proj_descs = static_cast<uint32_t>(data_->projectiles.size());
    const auto proj_ok = [proj_descs](int32_t p) { return p == -1 || (p >= 0 && static_cast<uint32_t>(p) < proj_descs); };

    const uint32_t frame = r.u32(), next_serial = r.u32();
    std::vector<uint8_t> enemy(static_cast<size_t>(np) * np);
    for (uint8_t& e : enemy) e = r.u8();
    std::vector<uint8_t> sw(np);
    for (uint8_t& f : sw) f = r.u8();
    if (!r.ok()) return false;
    std::vector<ModifierSet> pmods(np);
    for (ModifierSet& s : pmods)
        if (!s.read(r)) return false;
    std::array<float, 10> wacc{};
    for (float& m : wacc) m = r.f32();
    if (!r.ok()) return false;

    uint32_t n = 0;
    if (!get_count(r, n, 64)) return false;
    std::vector<Unit> units(n);
    ObjectId prev = 0;
    bool first = true;
    for (Unit& u : units) {
        u.id = r.i32();
        u.desc = r.u32();
        u.owner = r.i32();
        u.team = r.u32();
        u.team_leader = r.u8();
        u.damage_parent = r.i32();
        u.flags = r.u32();
        u.pos = get_vec3(r);
        u.last_pos = get_vec3(r);
        u.facing = get_vec3(r);
        u.prev_pos = get_vec3(r);
        u.health = r.f32();
        u.shield = r.f32();
        u.shield_depleted = r.u8();
        u.shield_depleted_frame = r.u32();
        u.shield_regen_countdown = r.i32();
        u.next_scan = r.u32();
        u.auto_target = r.i32();
        u.ordered_target = r.i32();
        u.target_score = r.f32();
        u.weapon_cooldown = r.i32();
        u.weapon_pulses = r.i32();
        u.ability_active[0] = r.u8();
        u.ability_active[1] = r.u8();
        u.hp_begin = r.u32();
        u.hp_count = r.u32();
        u.ever_hit = r.u8();
        u.last_hit_frame = r.u32();
        for (float& b : u.dmg_bucket) b = r.f32();
        for (uint32_t& s : u.dmg_second) s = r.u32();
        u.killer = r.i32();
        u.ordered_hp = r.i32();
        u.energy = r.f32();
        u.energy_countdown = r.i32();
        u.burning = r.u8();
        u.burn_countdown = r.i32();
        u.ion_stun_end = r.u32();
        u.jammed_frame = r.u32();
        u.ever_jammed = r.u8();
        u.regen_blocked = r.u8();
        u.emitter_radius = r.f32();
        u.priority_override = r.i32();
        u.stun_end = r.u32();
        u.invuln_end = r.u32();
        u.emitter_entries = r.u32();
        u.weapon_index = r.i32();
        u.acquired = r.u8();
        u.goal_target = r.i32();  // refixf t06
        u.goal_hp = r.i32();
        u.aim_target = r.i32();
        u.aim_hp = r.i32();
        u.locked = r.u8();
        u.auto_hp = r.i32();
        u.weapon_last = r.u32();  // S7 t15 A2
        u.weapon_bone = r.u32();  // S8 t06 B4
        u.lob_point = get_vec3(r);  // S8 t26
        u.lob_pending = r.u8();
        u.lob_charged = r.u8();
        u.lob_stamp = r.u32();
        u.infect_attached = r.u8();
        u.infect_suspend = r.u32();
        u.infect_desc = r.i32();
        u.infect_player = r.i32();
        u.infect_source = r.i32();
        u.infect_end = r.u32();
        u.infect_next_damage = r.u32();
        uint32_t nx = 0;
        if (!get_count(r, nx, 8) || u.infect_attached > 1 || u.infect_desc < -1 ||
            (u.infect_desc >= 0 && static_cast<uint32_t>(u.infect_desc) >= unit_descs))
            return false;
        u.infect_exposure.resize(nx);
        for (auto& [id, f] : u.infect_exposure) {
            id = r.i32();
            f = r.u32();
        }
        if (!r.ok() || !u.mods.read(r)) return false;
        for (float& v : u.mod_r) v = r.f32();
        u.mod_ver = r.u32();
        u.player_ver = r.u32();
        u.mod_valid = r.u8();
        for (float& v : u.own_mult) v = r.f32();
        for (float& v : u.mult) v = r.f32();
        u.squad_seen = r.u16();
        u.needs_scan = r.u8();
        u.scan_due = r.u8();
        if (!r.ok() || u.id == kNoObject || u.desc >= unit_descs || (!first && u.id <= prev)) return false;
        if (u.hp_count != data_->units[u.desc].hardpoints.size()) return false;
        if (u.priority_override < -1 ||
            (u.priority_override >= 0 && static_cast<size_t>(u.priority_override) >= data_->priority_sets.size()))
            return false;
        prev = u.id;
        first = false;
    }

    if (!get_count(r, n, 38)) return false;
    std::vector<Hardpoint> hps(n);
    for (Hardpoint& h : hps) {
        h.desc = r.u32();
        h.health = r.f32();
        h.cooldown = r.i32();
        h.pulses = r.i32();
        h.destroyed = r.u8();
        h.disabled = r.u8();
        h.target = r.i32();
        h.target_hp = r.i32();
        h.opp_target = r.i32();
        h.opp_hp = r.i32();
        h.opp_next_scan = r.u32();
        if (!r.ok() || h.desc >= hp_descs) return false;
    }
    for (const Unit& u : units) {
        if (static_cast<uint64_t>(u.hp_begin) + u.hp_count > hps.size()) return false;
        const std::vector<uint32_t>& list = data_->units[u.desc].hardpoints;
        for (uint32_t i = 0; i < u.hp_count; ++i)
            if (hps[u.hp_begin + i].desc != list[i]) return false;
    }
    const auto find_in = [&units](ObjectId id) -> const Unit* {
        const auto it = std::lower_bound(units.begin(), units.end(), id, [](const Unit& u, ObjectId v) { return u.id < v; });
        return (it == units.end() || it->id != id) ? nullptr : &*it;
    };

    if (!get_count(r, n, 82)) return false;
    std::vector<Projectile> projectiles(n);
    for (Projectile& p : projectiles) {
        p.serial = r.u32();
        p.desc = r.u32();
        p.shooter = r.i32();
        p.owner = r.i32();
        p.target = r.i32();
        p.target_hp = r.i32();
        p.pos = get_vec3(r);
        p.dir = get_vec3(r);
        p.speed = r.f32();
        p.travelled = r.f32();
        p.max_travel = r.f32();
        p.spawn_frame = r.u32();
        p.damage = r.f32();
        p.damage_type = r.u16();
        p.kind = r.u8();
        p.alive = r.u8();
        p.jammed_frame = r.u32();
        p.ever_jammed = r.u8();
        p.fuse = r.f32();
        p.rocket = r.u8();
        p.lob = r.u8();
        p.stuck = r.u8();
        p.gravity = r.f32();
        p.stuck_to = r.i32();
        p.launch = get_vec3(r);
        p.lob_dir = get_vec3(r);
        p.stuck_offset = get_vec3(r);
        if (!r.ok() || p.desc >= proj_descs || p.rocket > 1 || p.lob > 1 || p.stuck > 1) return false;
    }
    // refixf t01: rocket states, exactly one per rocket-flagged projectile, in the same (serial) order.
    if (!get_count(r, n, 61)) return false;
    std::vector<Rocket> rockets(n);
    {
        size_t j = 0;
        for (Rocket& rk : rockets) {
            rk.serial = r.u32();
            rk.planned = r.u8();
            rk.s = r.f32();
            rk.budget = r.f32();
            rk.fire_at = get_vec3(r);
            rk.launch = get_vec3(r);
            rk.last_vel = get_vec3(r);
            uint32_t npts = 0;
            if (!get_count(r, npts, 12)) return false;
            std::vector<Vec3> pts(npts);
            for (Vec3& v : pts) v = get_vec3(r);
            uint32_t ns = 0;
            if (!get_count(r, ns, 52)) return false;
            std::vector<RocketSpline::Segment> segs(ns);
            for (RocketSpline::Segment& sg : segs) {
                for (float& c : sg.k) c = r.f32();
                sg.length = r.f32();
            }
            const float total = r.f32();
            if (!r.ok() || rk.planned > 1) return false;
            rk.path.restore(std::move(pts), std::move(segs), total);
            while (j < projectiles.size() && !projectiles[j].rocket) ++j;
            if (j >= projectiles.size() || projectiles[j].serial != rk.serial) return false;
            ++j;
        }
        while (j < projectiles.size() && !projectiles[j].rocket) ++j;
        if (j != projectiles.size()) return false;  // a rocket-flagged projectile without a state
    }

    if (!get_count(r, n, 28)) return false;
    std::vector<Delayed> delayed(n);
    for (Delayed& d : delayed) {
        d.victim = r.i32();
        d.frames = r.i32();
        d.amount = r.f32();
        d.damage_type = r.u16();
        d.kind = r.u8();
        d.source_is_projectile = r.u8();
        d.source = r.i32();
        d.projectile_desc = r.i32();
        d.hardpoint = r.i32();
        if (!r.ok() || !proj_ok(d.projectile_desc)) return false;
    }

    if (!get_count(r, n, 8)) return false;
    std::vector<std::pair<int32_t, ObjectId>> base_power(n);
    for (auto& [pl, src] : base_power) {
        pl = r.i32();
        src = r.i32();
    }
    if (!get_count(r, n, 12)) return false;
    std::vector<Repairer> repairs(n);
    for (Repairer& rp : repairs) {
        rp.unit = r.i32();
        rp.hp = r.u32();
        rp.player = r.i32();
        if (!r.ok()) return false;
        const Unit* u = find_in(rp.unit);
        if (!u || rp.hp >= u->hp_count) return false;
    }
    if (!get_count(r, n, 8)) return false;
    std::vector<std::pair<uint32_t, ObjectId>> team_heads(n);
    for (auto& [team, id] : team_heads) {
        team = r.u32();
        id = r.i32();
    }
    if (!r.ok()) return false;

    if (!get_count(r, n, 22)) return false;
    std::vector<DeathEvent> deaths(n);
    for (DeathEvent& e : deaths) {
        e.id = r.i32();
        e.killer = r.i32();
        e.kind = r.u8();
        e.death_clone = r.i32();
        e.clone_key = r.i32();
        e.death_projectile = r.i32();
        e.spin_away = r.boolean();
        if (!r.ok() || !proj_ok(e.death_projectile)) return false;
    }
    if (!get_count(r, n, 19)) return false;
    std::vector<HardpointEvent> hp_events(n);
    for (HardpointEvent& e : hp_events) {
        e.owner = r.i32();
        e.index = r.u32();
        const uint8_t kind = r.u8();
        e.owner_dying = r.boolean();
        e.last_of_kind = r.boolean();
        e.explosion_type = r.i32();
        e.breakoff_type = r.i32();
        if (!r.ok() || kind > static_cast<uint8_t>(HardpointKind::DummyArt)) return false;
        e.kind = static_cast<HardpointKind>(kind);
    }
    if (!get_count(r, n, 5)) return false;
    std::vector<SmokeEvent> smoke(n);
    for (SmokeEvent& e : smoke) {
        e.id = r.i32();
        e.percent = r.u8();
    }
    if (!get_count(r, n, 51)) return false;
    std::vector<DamageEvent> damage_events(n);
    for (DamageEvent& e : damage_events) {
        e.victim = r.i32();
        e.source = r.i32();
        e.source_owner = r.i32();
        e.source_is_projectile = r.boolean();
        e.projectile_desc = r.i32();
        e.kind = r.u8();
        e.hardpoint = r.i32();
        e.after_multipliers = r.f32();
        e.absorbed = r.f32();
        e.energy_absorbed = r.f32();
        e.to_hardpoint = r.f32();
        e.to_hull = r.f32();
        e.hull_lost = r.f32();
        e.killed = r.boolean();
        e.frame = r.u32();
        if (!r.ok() || !proj_ok(e.projectile_desc)) return false;
    }
    if (!get_count(r, n, 35)) return false;
    std::vector<HitEvent> hit_events(n);
    for (HitEvent& e : hit_events) {
        e.victim = r.i32();
        e.shooter = r.i32();
        e.owner = r.i32();
        e.projectile_desc = r.u32();
        e.position = get_vec3(r);
        e.absorbed = r.boolean();
        e.armor_reduced = r.boolean();
        e.delayed = r.boolean();
        e.pick = r.i32();
        if (!r.ok() || e.projectile_desc >= proj_descs) return false;
    }
    if (!get_count(r, n, 12)) return false;
    std::vector<ShotEvent> shot_events(n);
    for (ShotEvent& e : shot_events) {
        e.shooter = r.i32();
        e.target = r.i32();
        e.target_owner = r.i32();
    }
    if (!get_count(r, n, 41)) return false;
    std::vector<SurvivorEvent> survivors(n);
    for (SurvivorEvent& e : survivors) {
        e.dead = r.i32();
        e.type = r.i32();
        e.owner = r.i32();
        e.position = get_vec3(r);
        e.facing = get_vec3(r);
        e.respawn = r.boolean();
        e.due_frame = r.u32();
    }
    CombatStats stats;
    stats.scans = r.u64();
    stats.shots = r.u64();
    stats.hits = r.u64();
    stats.expired = r.u64();
    stats.deaths = r.u64();
    stats.resweeps = r.u64();
    if (!r.ok() || !r.done()) return false;

    // commit
    frame_ = frame;
    next_serial_ = next_serial;
    enemy_ = std::move(enemy);
    sw_hp_enabled_ = std::move(sw);
    player_mods_ = std::move(pmods);
    weather_acc_ = wacc;
    units_ = std::move(units);
    hps_ = std::move(hps);
    projectiles_ = std::move(projectiles);
    rockets_ = std::move(rockets);
    delayed_ = std::move(delayed);
    base_power_ = std::move(base_power);
    repairs_ = std::move(repairs);
    team_heads_ = std::move(team_heads);
    deaths_ = std::move(deaths);
    hp_events_ = std::move(hp_events);
    smoke_events_ = std::move(smoke);
    damage_events_ = std::move(damage_events);
    hit_events_ = std::move(hit_events);
    shot_events_ = std::move(shot_events);
    survivor_events_ = std::move(survivors);
    stats_ = stats;
    // per-tick scratch (written before it is read)
    cands_.clear();
    cands_.resize(units_.size());
    for (auto& c : cands_) c.reserve(cand_reserve_);
    sweep_.clear();
    emitters_.clear();
    effect_scratch_.clear();
    leaders_.clear();
    flush_hps_.clear();
    return true;
}

// ---- WeatherSystem --------------------------------------------------------------------------------------------------

void WeatherSystem::write(ByteWriter& w) const {
    w.u32(kWeatherSaveVersion);
    w.boolean(scenario_ != nullptr);
    if (scenario_) {
        uint32_t index = kNoIndex;
        if (data_)
            for (size_t i = 0; i < data_->scenarios.size(); ++i)
                if (&data_->scenarios[i] == scenario_) {
                    index = static_cast<uint32_t>(i);
                    break;
                }
        w.u32(index).str(scenario_->name);
    }
    w.u32(index_).u32(current_).u32(next_).u8(static_cast<uint8_t>(type_));
    w.f32(intensity_).f32(lightning_).f32(fog_alpha_).boolean(transition_).u32(timer_start_).f32(timer_length_);
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const WeatherEvent& e : events_) w.u8(static_cast<uint8_t>(e.kind)).u32(e.phase).u32(e.frame);
}

bool WeatherSystem::read(ByteReader& r) {
    if (r.u32() != kWeatherSaveVersion || !r.ok()) return false;
    const bool active = r.boolean();
    if (!r.ok()) return false;
    const WeatherScenario* scenario = nullptr;
    if (active) {
        const uint32_t index = r.u32();
        const std::string name = r.str();
        if (!r.ok()) return false;
        if (data_ && index < data_->scenarios.size() && data_->scenarios[index].name == name)
            scenario = &data_->scenarios[index];
        else if (scenario_ && scenario_->name == name)
            scenario = scenario_;
        if (!scenario || scenario->phases.empty()) return false;
    }
    const uint32_t index = r.u32(), current = r.u32(), next = r.u32();
    const uint8_t type = r.u8();
    const float intensity = r.f32(), lightning = r.f32(), fog_alpha = r.f32();
    const bool transition = r.boolean();
    const uint32_t timer_start = r.u32();
    const float timer_length = r.f32();
    if (!r.ok() || type >= kWeatherTypeCount) return false;
    if (scenario) {
        const size_t np = scenario->phases.size();
        if (index >= np || current >= np || next >= np) return false;
    }
    uint32_t n = 0;
    if (!get_count(r, n, 9)) return false;
    std::vector<WeatherEvent> events(n);
    for (WeatherEvent& e : events) {
        const uint8_t kind = r.u8();
        e.phase = r.u32();
        e.frame = r.u32();
        if (!r.ok() || kind > static_cast<uint8_t>(WeatherEvent::Kind::Off)) return false;
        e.kind = static_cast<WeatherEvent::Kind>(kind);
    }
    if (!r.ok() || !r.done()) return false;
    scenario_ = scenario;
    index_ = index;
    current_ = current;
    next_ = next;
    type_ = static_cast<WeatherType>(type);
    intensity_ = intensity;
    lightning_ = lightning;
    fog_alpha_ = fog_alpha;
    transition_ = transition;
    timer_start_ = timer_start;
    timer_length_ = timer_length;
    events_ = std::move(events);
    return true;
}

}  // namespace alamo::tactical::combat
