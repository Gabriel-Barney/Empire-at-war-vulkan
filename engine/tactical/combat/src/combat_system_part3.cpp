// combat_system: part 3 (bo3 t03 split; shared code in combat_system_internal.hpp).
#include "combat_system_internal.hpp"

namespace alamo::tactical::combat {
void CombatSystem::flush() {
    bool any_dead = false;
    for (const Unit& u : units_)
        if (u.flags & kUnitDead) {
            any_dead = true;
            break;
        }
    if (any_dead) {
        // s78f t07: a dead team_leader hands the role over (kill() already did it for deaths through Die; this covers
        // remove_unit and saves taken between the two).
        for (const Unit& d : units_)
            if (d.flags & kUnitDead) hand_over_team(d);
        flush_hps_.clear();
        size_t w = 0;
        for (size_t i = 0; i < units_.size(); ++i) {
            if (units_[i].flags & kUnitDead) continue;
            Unit& u = units_[i];
            const auto begin = static_cast<uint32_t>(flush_hps_.size());
            for (uint32_t k = 0; k < u.hp_count; ++k) flush_hps_.push_back(hps_[u.hp_begin + k]);
            u.hp_begin = begin;
            if (w != i) {
                units_[w] = std::move(units_[i]);
                std::swap(cands_[w], cands_[i]);
            }
            ++w;
        }
        units_.resize(w);
        cands_.resize(w);
        hps_.swap(flush_hps_);
        // OI abilities: repairer lists of removed units go with them.
        repairs_.erase(std::remove_if(repairs_.begin(), repairs_.end(), [this](const Repairer& r) { return find(r.unit) < 0; }),
                       repairs_.end());
        if (hooks_) hooks_->units_removed();
    }
    size_t w = 0;
    for (size_t i = 0; i < projectiles_.size(); ++i)
        if (projectiles_[i].alive) projectiles_[w++] = projectiles_[i];
    projectiles_.resize(w);
    // refixf t01: rocket states follow their projectiles (both in serial order).
    if (!rockets_.empty()) {
        size_t rw = 0, j = 0;
        for (size_t k = 0; k < rockets_.size(); ++k) {
            while (j < projectiles_.size() && projectiles_[j].serial < rockets_[k].serial) ++j;
            if (j < projectiles_.size() && projectiles_[j].serial == rockets_[k].serial) {
                if (rw != k) rockets_[rw] = std::move(rockets_[k]);
                ++rw;
            }
        }
        rockets_.resize(rw);
    }
}

// ================================================================================================
// inspection
// ================================================================================================

ProjectileView CombatSystem::projectile(size_t i) const {
    ProjectileView v;
    if (i >= projectiles_.size()) return v;
    const Projectile& p = projectiles_[i];
    v.serial = p.serial;
    v.shooter = p.shooter;
    v.target = p.target;
    v.target_hp = p.target_hp;
    v.pos = p.pos;
    v.dir = p.dir;
    v.damage = p.damage;
    v.max_travel = p.max_travel;
    v.desc = p.desc;
    v.owner = p.owner;
    v.travelled = p.travelled;
    v.category = data_->projectiles[p.desc].category;
    v.alive = p.alive != 0;
    v.speed = p.speed;
    v.spawn_frame = p.spawn_frame;
    v.lob = p.lob != 0;
    v.stuck = p.stuck != 0;
    v.stuck_to = p.stuck_to;
    return v;
}

// ================================================================================================
// S9 ability-facing API
// ================================================================================================

uint32_t CombatSystem::desc_index(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->desc : 0u;
}

int32_t CombatSystem::owner(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->owner : -1;
}

Vec3 CombatSystem::position(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->pos : Vec3{};
}

Vec3 CombatSystem::facing(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->facing : Vec3{1.0f, 0.0f, 0.0f};
}

uint32_t CombatSystem::flags(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? (u->flags & ~kUnitRemoved) : 0u;
}

uint32_t CombatSystem::team(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->team : 0u;
}

ObjectId CombatSystem::unit_id_at(size_t index) const { return index < units_.size() ? units_[index].id : kNoObject; }

uint32_t CombatSystem::hardpoint_count(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->hp_count : 0u;
}

HardpointKind CombatSystem::hardpoint_kind(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? data_->hardpoints[hps_[u->hp_begin + hp].desc].kind : HardpointKind::DummyArt;
}

float CombatSystem::hardpoint_max(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? hp_max(*u, hps_[u->hp_begin + hp]) : 0.0f;
}

bool CombatSystem::hardpoint_targetable(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    if (!u || hp >= u->hp_count) return false;
    const Hardpoint& h = hps_[u->hp_begin + hp];
    return !h.destroyed && data_->hardpoints[h.desc].targetable;
}

uint32_t CombatSystem::live_hardpoints(ObjectId id, HardpointKind kind) const {
    const Unit* u = unit(id);
    if (!u) return 0;
    uint32_t c = 0;
    for (uint32_t i = 0; i < u->hp_count; ++i) {
        const Hardpoint& h = hps_[u->hp_begin + i];
        if (!hp_out(h) && data_->hardpoints[h.desc].kind == kind) ++c;  // Count_Functioning_Hardpoints (0x397c10)
    }
    return c;
}

ObjectId CombatSystem::hardpoint_opportunity_target(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? hps_[u->hp_begin + hp].opp_target : kNoObject;
}

int32_t CombatSystem::choose_target_hardpoint(ObjectId attacker, ObjectId target) const {
    const Unit* a = unit(attacker);
    const Unit* t = unit(target);
    return (a && live(t)) ? choose_target_hardpoint(*a, *t) : -1;
}

int32_t CombatSystem::closest_hardpoint(ObjectId target, Vec3 from, bool targetable_only) const {
    const Unit* t = unit(target);
    return live(t) ? closest_hardpoint(*t, from, targetable_only) : -1;
}

bool CombatSystem::can_attack(ObjectId id) const {
    const Unit* u = unit(id);
    return live(u) && unit_can_attack(*u);
}

// ---- fog follow-up: space-station special weapon (SCRIPT_GAPS.md §1.1 step 5) ----
int32_t CombatSystem::special_weapon_hardpoint(ObjectId station) const {
    const Unit* u = unit(station);
    if (!u) return -1;
    for (uint32_t i = 0; i < u->hp_count; ++i)
        if (data_->hardpoints[hps_[u->hp_begin + i].desc].kind == HardpointKind::WeaponSpecial) return static_cast<int32_t>(i);
    return -1;
}

float CombatSystem::special_weapon_readiness(ObjectId station) const {
    const int32_t i = special_weapon_hardpoint(station);
    if (i < 0) return -1.0f;
    const Unit& u = *unit(station);
    const Hardpoint& h = hps_[u.hp_begin + static_cast<uint32_t>(i)];
    if (!live(&u) || h.destroyed || h.disabled) return 0.0f;
    if (h.cooldown <= 0) return 1.0f;
    const float full = data_->hardpoints[h.desc].max_recharge * static_cast<float>(cfg_.fps);
    if (!(full > 0.0f)) return 0.0f;
    return std::max(0.0f, 1.0f - static_cast<float>(h.cooldown) / full);
}

bool CombatSystem::special_weapon_can_fire(ObjectId station, ObjectId target) const {
    const int32_t i = special_weapon_hardpoint(station);
    const Unit* u = unit(station);
    const Unit* t = unit(target);
    if (i < 0 || !live(u) || !live(t) || t == u) return false;
    const Hardpoint& h = hps_[u->hp_begin + static_cast<uint32_t>(i)];
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    if (h.destroyed || !is_enemy(u->owner, t->owner) || !desc_of(*t).is_valid_target) return false;
    if (target_hidden(u->owner, t->id, true)) return false;  // force 1: never at hidden targets (FOG_OF_WAR §5.3)
    if (!in_cone(*u, h, t->pos)) return false;
    Vec3 bone = local_to_world(*u, hd.bone_a);
    if (hd.has_bone_b) bone = (bone + local_to_world(*u, hd.bone_b)) * 0.5f;
    const float d = distance_xy(bone, t->pos);
    const float range = fire_range(hd.fire_range, mod_r(*u, ModList::FireRange));
    return d <= range + desc_of(*t).radius && d >= hd.fire_min_range;
}

bool CombatSystem::special_weapon_fire(ObjectId station, ObjectId target) {
    if (!special_weapon_can_fire(station, target)) return false;
    order_attack(station, target, -1);
    return true;
}

// ---- L1 systems: host-launched projectiles (special weapons, lobbing superweapons, bombardment) ----
bool CombatSystem::launch_projectile(const ProjectileLaunch& l, uint32_t frame) {
    if (l.projectile >= data_->projectiles.size()) return false;
    const ProjectileDesc& pd = data_->projectiles[l.projectile];
    // S8 t04 (LOB_SOLVER §5): the shared spawner's grenade branch - every category-3 launch except a placed bomb (a fuse
    // without `toss`). Q = the un-led target point (team container -> nearest member, + Ranged_Target_Z_Adjust) or the
    // ground point; no arc -> nothing is spawned.
    const bool lob = grenade(pd) && (l.fuse_seconds < 0.0f || l.toss);
    Vec3 aim = l.point, q = l.point;
    float radius = 0.0f;
    if (l.target != kNoObject) {
        const Unit* t = unit(l.target);
        if (!live(t)) return false;
        aim = t->pos;
        if (l.target_hp >= 0 && static_cast<uint32_t>(l.target_hp) < t->hp_count)
            aim = hp_world(*t, data_->hardpoints[hps_[t->hp_begin + static_cast<uint32_t>(l.target_hp)].desc]);
        aim = lead_point(*t, l.from, aim, pd.max_speed * static_cast<float>(cfg_.fps));  // OI tactical: circular lead
        radius = desc_of(*t).radius;
        if (lob) q = lob_target_point(lob_member(*t, l.from));
    }
    LobSolution sol;
    if (lob && !lob_solve(l.from, q, pd.max_speed, lob_g(pd), pd.can_lob_slower, sol)) return false;
    Projectile p;
    p.serial = next_serial_++;
    p.desc = l.projectile;
    p.shooter = l.shooter;
    p.owner = l.owner;
    p.target = l.target;
    p.target_hp = l.target_hp;
    p.pos = l.from;
    const Vec3 d = aim - l.from;
    const float dist = dmath::sqrt(dot(d, d));
    p.dir = dist > 0.0f ? d * (1.0f / dist) : Vec3{1.0f, 0.0f, 0.0f};
    p.speed = pd.max_speed;
    p.spawn_frame = frame;
    p.damage = l.damage >= 0.0f ? l.damage : pd.damage;
    p.damage_type = pd.damage_type;
    p.kind = pd.damage_kind;
    // [port] travel to the aim point (+ the target's radius): a point shot expires (detonates) there. A shot with
    // no distance at all (aim == from) falls back to the lifetime rule.
    p.max_travel = dist > 0.0f ? dist + radius : 0.0f;
    if (lob) {
        // §5.3: pitch theta and speed s from the solve, yaw from the led aim; MaxTravelDistance = Projectile_Max_Flight_
        // Distance (no request override) + the target radius + |Q.z - L.z| [M: 0 without a flight distance -> lifetime].
        p.lob = 1;
        p.gravity = lob_g(pd);
        p.launch = l.from;
        p.lob_dir = lob_forward(aim - l.from, sol.angle);
        p.dir = p.lob_dir;
        p.speed = sol.speed;
        p.max_travel = pd.max_flight_distance > 0.0f ? pd.max_flight_distance + radius + abs_f(q.z - l.from.z) : 0.0f;
        p.fuse = l.fuse_seconds;  // a tossed bomb's timer runs during the flight
    } else if (l.fuse_seconds >= 0.0f) {  // OI abilities: an armed bomb sits where it was placed until the fuse runs out
        p.speed = 0.0f;
        p.max_travel = 0.0f;
        p.fuse = l.fuse_seconds;
    } else if (spline_rocket(pd)) {
        // refixf t01: a host-launched spline rocket flies the §2.4.10 path to `aim`; its travel limit is Spawn_Projectile's
        // (MFD with no override + the target radius + |aim z - launch z|) [M], since the path is longer than the line.
        p.rocket = 1;
        if (pd.max_flight_distance > 0.0f) p.max_travel = pd.max_flight_distance + radius + abs_f(aim.z - l.from.z);
        add_rocket(p, aim, l.from);
    }
    projectiles_.push_back(p);
    ++stats_.shots;
    return true;
}

float CombatSystem::script_hull(ObjectId id) const {
    const Unit* u = unit(id);
    if (!u) return 0.0f;
    const float mh = unit_max_hull(*u);
    float f = mh > 0.0f ? u->health / mh : 0.0f;
    if (desc_of(*u).destroy_when_all_hardpoints_destroyed) {
        bool any = false;
        for (uint32_t i = 0; i < u->hp_count; ++i)
            if (data_->hardpoints[hps_[u->hp_begin + i].desc].destroyable) any = true;
        if (any) {
            const float hf = hp_fraction(*u);
            if (hf < f) f = hf;
        }
    }
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

float CombatSystem::shield_fraction(ObjectId id) const {
    const Unit* u = unit(id);
    if (!u || !(desc_of(*u).shield_points > 0.0f)) return 0.0f;
    const float mx = unit_max_shield(*u);
    if (!(mx > 0.0f)) return 0.0f;
    float f = u->shield / mx;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

bool CombatSystem::shield_lockout(ObjectId id, uint32_t frame) const {
    const Unit* u = unit(id);
    return u && u->shield_depleted &&
           shield_lockout_active(frame, u->shield_depleted_frame, cfg_.fps, cfg_.depleted_shield_disable_time);
}

void CombatSystem::force_regen_service(ObjectId id) {
    Unit* u = unit(id);
    if (!u) return;
    u->shield_regen_countdown = 1;  // +0x30 = frame + 1: serviced on the next upkeep
    if (has_energy_pool(*u)) u->energy_countdown = 1;
}

void CombatSystem::clear_targets(ObjectId id) {
    Unit* u = unit(id);
    if (!u) return;
    u->auto_target = kNoObject;
    u->auto_hp = -1;
    u->ordered_target = kNoObject;
    u->ordered_hp = -1;
    u->locked = 0;
    u->aim_target = kNoObject;  // refixf t06
    u->aim_hp = -1;
    u->target_score = 0.0f;
    for (uint32_t i = 0; i < u->hp_count; ++i) {  // 0x38f840 per hardpoint
        Hardpoint& h = hps_[u->hp_begin + i];
        h.target = kNoObject;
        h.target_hp = -1;
        h.opp_target = kNoObject;
        h.opp_hp = -1;
    }
}

void CombatSystem::reset_weapon_burst(ObjectId id) {
    Unit* u = unit(id);
    if (!u) return;
    ensure_cache(*u);
    u->weapon_cooldown = 0;
    u->weapon_pulses = static_cast<int32_t>(static_cast<float>(desc_of(*u).weapon_pulse_count) * mult(*u, ModKind::FireRate) *
                                            (1.0f + mod_r(*u, ModList::FireRate)));
}

void CombatSystem::set_emitter_radius(ObjectId id, float radius) {
    if (Unit* u = unit(id)) u->emitter_radius = radius > 0.0f ? radius : 0.0f;
}

float CombatSystem::emitter_radius(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->emitter_radius : 0.0f;
}

void CombatSystem::apply_jamming(int32_t jammer_owner, Vec3 center, float radius, uint32_t frame) {
    // OI abilities (TACTICAL_GAPS §1.5 [H]): the jammer stamps objects NOT hostile to its owner (own side and allies):
    // homing missiles aimed at them fly straight, so a jammer protects friendly units in its radius.
    const float r2 = radius * radius;
    for (Unit& u : units_) {  // ascending id; every hit writes the same value (order-free)
        if ((u.flags & kUnitDead) || !desc_of(u).has_locomotor || is_enemy(jammer_owner, u.owner)) continue;
        if (u.damage_parent != kNoObject) continue;  // inside a container
        const Vec3 d = u.pos - center;
        if (dot(d, d) <= r2) {
            u.jammed_frame = frame;
            u.ever_jammed = 1;
        }
    }
    for (Projectile& p : projectiles_) {  // projectiles have a locomotor too (no reader of their stamp [H])
        if (!p.alive || is_enemy(jammer_owner, p.owner)) continue;
        const Vec3 d = p.pos - center;
        if (dot(d, d) <= r2) {
            p.jammed_frame = frame;
            p.ever_jammed = 1;
        }
    }
}

bool CombatSystem::is_jammed(ObjectId id, uint32_t frame) const {
    const Unit* u = unit(id);
    return u && u->ever_jammed &&
           static_cast<float>(frame - u->jammed_frame) / static_cast<float>(cfg_.fps) < cfg_.sensor_jamming_time;
}

bool CombatSystem::apply_ion_stun(ObjectId victim, uint32_t projectile_desc, uint32_t frame) {
    Unit* v = unit(victim);
    if (!live(v) || projectile_desc >= data_->projectiles.size()) return false;
    return ion_stun_unit(*v, projectile_desc, frame);
}

bool CombatSystem::ion_stunned(ObjectId id, uint32_t frame) const {
    const Unit* u = unit(id);
    return u && u->ion_stun_end > frame;
}

uint32_t CombatSystem::ion_stun_end(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->ion_stun_end : 0u;
}

float CombatSystem::energy(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->energy : 0.0f;
}

bool CombatSystem::burning(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->burning != 0;
}

bool CombatSystem::infection_attached(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->infect_attached != 0;
}

uint32_t CombatSystem::infection_suspend_count(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->infect_suspend : 0;
}

int32_t CombatSystem::infection_frames_left(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->infect_attached ? u->infect_left : 0;
}

int32_t CombatSystem::infection_spread_count(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->infect_seen != 0xFFFFFFFFu ? u->infect_count : -1;
}

void CombatSystem::suspend_infection(ObjectId id) {
    Unit* u = unit(id);
    if (!u || !u->infect_attached) return;
    ++u->infect_suspend;
    u->flags &= ~kUnitInfected;
}

void CombatSystem::resume_infection(ObjectId id, uint32_t frame) {
    // vfunc +0xb8 alone (cu8f t06): counter - 1, floored at 0; the pack (frames left, parameters) is not rewritten.
    (void)frame;
    Unit* u = unit(id);
    if (!u || !u->infect_attached || (u->flags & kUnitDead)) return;
    if (u->infect_suspend != 0 && --u->infect_suspend == 0) u->flags |= kUnitInfected;
}

bool CombatSystem::detonate_projectile(size_t index, uint32_t frame) {
    if (index >= projectiles_.size() || !projectiles_[index].alive) return false;
    projectiles_[index].alive = 0;
    const Projectile pc = projectiles_[index];  // copy: detonation may spawn (death projectiles)
    const ProjectileDesc& pd = data_->projectiles[pc.desc];
    // S8 t06: a host call between ticks (DETONATE_REMOTE_BOMB) queries the last tick's trees for the area damage.
    const TreeSet* const saved = trees_;
    if (!trees_) trees_ = last_trees_;
    if (pd.blast_area_damage > 0.0f && pd.blast_area_range > 0.0f) area_damage(pc, pc.pos, kNoObject, frame);
    detonation_effects(pc, pc.pos, kNoObject, frame);
    trees_ = saved;
    return true;
}

void CombatSystem::kill_unit(ObjectId id, ObjectId killer, uint8_t kind, uint32_t frame, int32_t sub_key) {
    Unit* u = unit(id);
    if (live(u)) kill(*u, killer, kind, frame, sub_key);
}

void CombatSystem::remove_from_world(ObjectId id, ObjectId killer, uint32_t frame) {
    Unit* u = unit(id);
    if (live(u)) kill(*u, killer, kKindNoDeathClone, frame, -1, true);
}

// ---- OI abilities ----
void CombatSystem::add_base_power(int32_t player, ObjectId source) {
    for (const auto& [pl, src] : base_power_)
        if (pl == player && src == source) return;  // one entry per (player, source): keyed by the source [H]
    base_power_.emplace_back(player, source);
}

void CombatSystem::remove_base_power(int32_t player, ObjectId source) {
    base_power_.erase(std::remove_if(base_power_.begin(), base_power_.end(),
                                     [&](const std::pair<int32_t, ObjectId>& e) { return e.first == player && e.second == source; }),
                      base_power_.end());
}

bool CombatSystem::has_base_power(int32_t player) const {
    // 0x527520 -> 0x2800d0: AND of the (true) entries; an empty list is false [H].
    for (const auto& [pl, src] : base_power_)
        if (pl == player) return true;
    return false;
}

bool CombatSystem::add_hardpoint_repairer(ObjectId id, uint32_t hp, int32_t player) {
    const Unit* u = unit(id);
    if (!live(u) || hp >= u->hp_count) return false;  // bounded by obj +0x2d0 (0x396cb0)
    for (const Repairer& r : repairs_)
        if (r.unit == id && r.hp == hp && r.player == player) return true;  // no duplicates
    repairs_.push_back(Repairer{id, hp, player});
    return true;
}

uint32_t CombatSystem::hardpoint_repairers(ObjectId id, uint32_t hp) const {
    uint32_t n = 0;
    for (const Repairer& r : repairs_)
        if (r.unit == id && r.hp == hp) ++n;
    return n;
}

bool CombatSystem::hardpoint_repaired_by(ObjectId id, uint32_t hp, int32_t player) const {
    for (const Repairer& r : repairs_)
        if (r.unit == id && r.hp == hp && r.player == player) return true;
    return false;
}

bool CombatSystem::hardpoint_disabled(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? hps_[u->hp_begin + hp].disabled != 0 : false;
}

void CombatSystem::set_hardpoint_disabled(ObjectId id, uint32_t hp, bool on) {
    // HP_Set_Disabled (0x387d10).
    Unit* u = unit(id);
    if (!u || hp >= u->hp_count) return;
    hps_[u->hp_begin + hp].disabled = on ? 1 : 0;
    set_system_online(*u, hp, !on, false, frame_);
}

bool CombatSystem::copy_upgrade_hardpoints(ObjectId old_station, ObjectId new_station) {
    // Starbase_Upgrade_Copy_Hardpoints (0x70ec70), HARDPOINT_DISABLED.md §3 [H].
    const Unit* o = unit(old_station);
    const Unit* n = unit(new_station);
    if (!o || !n) return false;
    const uint32_t count = o->hp_count < n->hp_count ? o->hp_count : n->hp_count;
    std::vector<int32_t> players;
    for (uint32_t i = 0; i < count; ++i) {
        const Hardpoint oh = hps_[unit(old_station)->hp_begin + i];
        players.clear();
        for (const Repairer& r : repairs_)
            if (r.unit == old_station && r.hp == i) players.push_back(r.player);
        const bool new_destroyable = data_->hardpoints[hps_[unit(new_station)->hp_begin + i].desc].destroyable;
        if (!players.empty()) {  // old +0x6f: being repaired
            set_hardpoint_disabled(new_station, i, true);
            if (new_destroyable) set_hardpoint_health(new_station, i, oh.health);  // 0x387ca0, clamped
            for (const int32_t p : players) add_hardpoint_repairer(new_station, i, p);
        } else if (hp_out(oh)) {  // old destroyed or disabled
            set_hardpoint_disabled(new_station, i, true);
            if (new_destroyable) set_hardpoint_health(new_station, i, 0.1f);
        }
        const Unit* nu = unit(new_station);
        const Hardpoint& nh = hps_[nu->hp_begin + i];
        if (data_->hardpoints[nh.desc].allows_special_weapon_use) {
            const auto np = static_cast<int32_t>(sw_hp_enabled_.size());
            for (int32_t p = 0; p < np; ++p)
                if (p == nu->owner || !is_enemy(nu->owner, p)) sw_hp_enabled_[static_cast<size_t>(p)] = nh.disabled ? 0 : 1;
        }
    }
    return true;
}

bool CombatSystem::shield_regen_blocked(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->regen_blocked != 0;
}

uint32_t CombatSystem::hardpoint_special_ability(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? data_->hardpoints[hps_[u->hp_begin + hp].desc].special_ability_name_crc : 0u;
}

bool CombatSystem::special_weapon_hp_enabled(int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= sw_hp_enabled_.size()) return true;
    return sw_hp_enabled_[static_cast<size_t>(player)] != 0;
}

bool CombatSystem::can_repair_hardpoints(ObjectId id) const {
    const Unit* u = unit(id);
    return u && desc_of(*u).repairs_hardpoints;
}

void CombatSystem::service_repairs(Unit& u, uint32_t frame) {
    // 0x387170 per hardpoint in list order (TACTICAL_GAPS §1.12 [H]).
    if (repairs_.empty() || u.hp_count == 0) return;
    for (uint32_t i = 0; i < u.hp_count; ++i) {
        Hardpoint& h = hps_[u.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        bool any = false;
        for (const Repairer& r : repairs_) any = any || (r.unit == u.id && r.hp == i);
        if (!any) continue;  // 1. empty list -> flag cleared (implicit)
        auto drop_all = [&] {
            repairs_.erase(std::remove_if(repairs_.begin(), repairs_.end(), [&](const Repairer& r) { return r.unit == u.id && r.hp == i; }),
                           repairs_.end());
        };
        if (hd.destroyable && (h.destroyed || !(h.health > 0.0f))) {  // 2. a destroyed destroyable hardpoint
            drop_all();
            continue;
        }
        const float mx = hp_max(u, h);
        // 3. each repairer in list order: pays the cost (credits) and adds the amount, else is removed.
        for (size_t k = 0; k < repairs_.size();) {
            const Repairer r = repairs_[k];
            if (r.unit != u.id || r.hp != i) {
                ++k;
                continue;
            }
            const float cost = hd.repair_cost_per_frame;
            const bool can_pay = !credits_get_ || credits_get_(r.player) >= cost;  // [port] no hooks: free
            if (!can_pay) {
                repairs_.erase(repairs_.begin() + static_cast<std::ptrdiff_t>(k));
                continue;
            }
            if (credits_add_ && cost > 0.0f) credits_add_(r.player, -cost);
            float hn = h.health + hd.repair_amount_per_frame;
            if (hn < 0.0f) hn = 0.0f;
            if (hn > mx) hn = mx;
            h.health = hn;
            // parent hull below the hardpoint fraction: hull x= (hp fraction - hull fraction + 1)
            const float mh = unit_max_hull(u);
            if (mh > 0.0f) {
                const float hull_f = u.health / mh, hp_f = hp_fraction(u);
                if (hull_f < hp_f) u.health = u.health * (hp_f - hull_f + 1.0f);
            }
            ++k;
        }
        // 4. at max: list cleared, system back online (Set_System_Online(hp, 1, 0)).
        if (h.health >= mx) {
            drop_all();
            h.disabled = 0;  // word +0x6e := 0 (disabled and being-repaired)
            set_system_online(u, i, true, false, frame);
        }
    }
}

bool CombatSystem::set_priority_override(ObjectId id, int32_t set) {
    Unit* u = unit(id);
    if (!u || set < -1 || (set >= 0 && static_cast<size_t>(set) >= data_->priority_sets.size())) return false;
    u->priority_override = set;
    u->next_scan = frame_;  // [port] rescan with the new set at the next service
    return true;
}

int32_t CombatSystem::priority_override(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->priority_override : -1;
}

bool CombatSystem::set_stickiness_override(ObjectId id, float seconds) {
    // cu8f t04a: 0x568df0 from the Lua setters (CU8F_T04_T06_RE §1.1 writers 1 / 2): TD +0x158 := seconds, as given.
    Unit* u = unit(id);
    if (!u) return false;
    u->stickiness = seconds;
    return true;
}

float CombatSystem::stickiness_override(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->stickiness : 0.0f;
}

bool CombatSystem::reset_stickiness(ObjectId id) {
    // cu8f t04a: the task-force release 0x5ff610 (§1.1 writer 3): TD +0x158 := type +0x528 of the unit's type, or of
    // its first team member's type for a TEAM owner (its own type when it has no member left).
    Unit* u = unit(id);
    if (!u) return false;
    const Unit* src = stickiness_type_unit(*u);
    u->stickiness = desc_of(src ? *src : *u).stickiness_threshold;
    return true;
}

int32_t CombatSystem::fired_weapon_projectile(const Unit& u) const {
    // OI abilities: obj +0x324 indexes the type's Projectile_Types list; [port] outside the list -> weapon_projectile.
    const UnitDesc& ud = desc_of(u);
    if (u.weapon_index >= 0 && static_cast<size_t>(u.weapon_index) < ud.weapon_projectiles.size()) {
        const int32_t p = ud.weapon_projectiles[static_cast<size_t>(u.weapon_index)];
        if (p >= 0 && static_cast<size_t>(p) < data_->projectiles.size()) return p;
    }
    return ud.weapon_projectile;
}

bool CombatSystem::set_weapon_index(ObjectId id, int32_t index) {
    Unit* u = unit(id);
    if (!u || index < 0) return false;
    u->weapon_index = index;
    return true;
}

int32_t CombatSystem::weapon_index(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->weapon_index : 0;
}

size_t CombatSystem::weapon_set_count(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? desc_of(*u).weapon_projectiles.size() : 0;
}

int32_t CombatSystem::current_weapon_projectile(ObjectId id) const {
    const Unit* u = unit(id);
    if (!u) return -1;
    const UnitDesc& ud = desc_of(*u);
    if (u->weapon_index < 0 || static_cast<size_t>(u->weapon_index) >= ud.weapon_projectiles.size()) return -1;
    return ud.weapon_projectiles[static_cast<size_t>(u->weapon_index)];
}

int32_t CombatSystem::priority_set_index(std::string_view name) const {
    for (size_t i = 0; i < data_->priority_sets.size(); ++i)
        if (iequals(data_->priority_sets[i].name, name)) return static_cast<int32_t>(i);
    return -1;
}

bool CombatSystem::stun_unit(ObjectId id, int32_t frames, uint32_t frame) {
    Unit* v = unit(id);
    if (!live(v) || !desc_of(*v).stunnable || frames <= 0) return false;
    v->stun_end = frame + static_cast<uint32_t>(frames);  // plain assignment (not extended) [H]
    // targeting / turret suspended: drop the current targets so the release starts from a fresh scan [M]
    v->auto_target = kNoObject;
    v->auto_hp = -1;
    v->target_score = 0.0f;
    if (hooks_) hooks_->unit_effect(id, UnitEffect::Stunned, v->owner, v->pos, frame);  // stealth break, host freeze
    return true;
}

bool CombatSystem::stunned(ObjectId id, uint32_t frame) const {
    const Unit* u = unit(id);
    return u && u->stun_end > frame;
}

void CombatSystem::make_invulnerable(ObjectId id, int32_t frames, uint32_t frame) {
    Unit* u = unit(id);
    if (!live(u) || frames <= 0) return;
    if (u->invuln_end <= frame) u->invuln_end = frame + static_cast<uint32_t>(frames);  // attached only when missing
    if (hooks_) hooks_->unit_effect(id, UnitEffect::Invulnerable, u->owner, u->pos, frame);
}

void CombatSystem::end_invulnerable(ObjectId id) {
    if (Unit* u = unit(id)) u->invuln_end = 0;
}

bool CombatSystem::timed_invulnerable(ObjectId id, uint32_t frame) const {
    const Unit* u = unit(id);
    return u && u->invuln_end > frame;
}

void CombatSystem::add_emitter_entry(ObjectId id) {
    if (Unit* u = unit(id)) ++u->emitter_entries;
}

uint32_t CombatSystem::emitter_entries(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->emitter_entries : 0u;
}

bool CombatSystem::acquired_target(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->acquired != 0;
}

bool CombatSystem::moved(ObjectId id) const {
    const Unit* u = unit(id);
    return u && (u->pos.x != u->last_pos.x || u->pos.y != u->last_pos.y || u->pos.z != u->last_pos.z);
}

void CombatSystem::convert_unit(ObjectId id, int32_t owner) {
    Unit* u = unit(id);
    if (!live(u)) return;
    u->flags |= kUnitConverted;
    set_owner(id, owner);
    if (hooks_) hooks_->unit_effect(id, UnitEffect::Converted, owner, u->pos, frame_);
}

void CombatSystem::set_hardpoint_health(ObjectId id, uint32_t hp, float h) {
    Unit* u = unit(id);
    if (!u || hp >= u->hp_count) return;
    Hardpoint& x = hps_[u->hp_begin + hp];
    if (x.destroyed) return;
    const float mx = hp_max(*u, x);
    x.health = h < 0.0f ? 0.0f : (h > mx ? mx : h);
}

bool CombatSystem::alive(ObjectId id) const { return live(unit(id)); }

float CombatSystem::health(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->health : 0.0f;
}

float CombatSystem::shield(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->shield : 0.0f;
}

float CombatSystem::max_health(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? unit_max_hull(*u) : 0.0f;
}

float CombatSystem::max_shield_of(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? unit_max_shield(*u) : 0.0f;
}

float CombatSystem::hardpoint_health(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? hps_[u->hp_begin + hp].health : 0.0f;
}

bool CombatSystem::hardpoint_destroyed(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? hps_[u->hp_begin + hp].destroyed != 0 : true;
}

int32_t CombatSystem::hardpoint_cooldown(ObjectId id, uint32_t hp) const {
    const Unit* u = unit(id);
    return (u && hp < u->hp_count) ? hps_[u->hp_begin + hp].cooldown : 0;
}

ObjectId CombatSystem::auto_target(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->auto_target : kNoObject;
}

uint32_t CombatSystem::next_scan(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->next_scan : 0;
}

// ================================================================================================
// CRC
// ================================================================================================

void CombatSystem::hash_units(StateHasher& h) const {
    // SIMP sp6: tree hash, blocks of kCrcBlockItems hashed in parallel (same digest for any worker count)
    hash_blocks(h, static_cast<uint32_t>(units_.size()), [&](StateHasher& bh, uint32_t blk_first, uint32_t blk_last) {
        for (uint32_t blk_i = blk_first; blk_i < blk_last; ++blk_i) {
            const Unit& u = units_[blk_i];
            bh.i32(u.id);
            bh.u32(u.desc);
            bh.i32(u.owner);
            bh.u32(u.flags & ~kUnitHostMirrorFlags);  // S8 t10 / t11: the host mirrors are derived state
            bh.f32(u.health);
            bh.f32(u.shield);
            bh.u8(u.shield_depleted);
            bh.u32(u.shield_depleted_frame);
            bh.i32(u.shield_regen_countdown);
            bh.u32(u.next_scan);
            bh.i32(u.auto_target);
            bh.i32(u.ordered_target);
            bh.f32(u.target_score);
            bh.i32(u.weapon_cooldown);
            bh.i32(u.weapon_pulses);
            bh.u8(u.ability_active[0]);
            bh.u8(u.ability_active[1]);
            bh.u8(u.ever_hit);
            bh.u32(u.last_hit_frame);
            for (uint32_t b = 0; b < 5; ++b) {
                bh.f32(u.dmg_bucket[b]);
                bh.u32(u.dmg_second[b]);
            }
            bh.u32(u.hp_count);
            for (uint32_t i = 0; i < u.hp_count; ++i) {
                const Hardpoint& hp = hps_[u.hp_begin + i];
                bh.u32(hp.desc);
                bh.f32(hp.health);
                bh.i32(hp.cooldown);
                bh.i32(hp.pulses);
                bh.u8(hp.destroyed);
                bh.u8(hp.disabled);
                bh.i32(hp.target);
                bh.i32(hp.target_hp);
            }
        }
    });
}

void CombatSystem::hash_projectiles(StateHasher& h) const {
    h.u32(next_serial_);
    h.u32(static_cast<uint32_t>(projectiles_.size()));
    for (const Projectile& p : projectiles_) {
        h.u32(p.serial);
        h.u32(p.desc);
        h.i32(p.shooter);
        h.i32(p.owner);
        h.i32(p.target);
        h.i32(p.target_hp);
        h.f32(p.pos.x);
        h.f32(p.pos.y);
        h.f32(p.pos.z);
        h.f32(p.dir.x);
        h.f32(p.dir.y);
        h.f32(p.dir.z);
        h.f32(p.speed);
        h.f32(p.travelled);
        h.f32(p.max_travel);
        h.u32(p.spawn_frame);
        h.f32(p.damage);
        h.u16(p.damage_type);
        h.u8(p.kind);
    }
}

void CombatSystem::hash_modifiers(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(player_mods_.size()));
    for (const ModifierSet& s : player_mods_) s.hash(h);
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.id);
        u.mods.hash(h);
    }
}

void CombatSystem::hash_delayed(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(delayed_.size()));
    for (const Delayed& d : delayed_) {
        h.i32(d.victim);
        h.i32(d.frames);
        h.f32(d.amount);
        h.u16(d.damage_type);
        h.u8(d.kind);
        h.u8(d.source_is_projectile);
        h.i32(d.source);
        h.i32(d.projectile_desc);
        h.i32(d.hardpoint);
        h.i32(d.projectile_target);  // s78n t12
    }
}

void CombatSystem::hash_extended(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.id);
        h.i32(u.ordered_hp);
        h.f32(u.energy);
        h.i32(u.energy_countdown);
        h.u8(u.burning);
        h.i32(u.burn_countdown);
        h.u32(u.ion_stun_end);
        h.u32(u.jammed_frame);
        h.u8(u.ever_jammed);
        h.u8(u.regen_blocked);
        h.f32(u.emitter_radius);
        // OI abilities
        h.i32(u.priority_override);
        if (u.stickiness != 0.0f) {  // cu8f t04a: TD +0x158 stickiness threshold (format unchanged at the spawn 0.0)
            h.u8(0x58);
            h.f32(u.stickiness);
        }
        h.u32(u.stun_end);
        h.u32(u.invuln_end);
        h.u32(u.emitter_entries);
        if (u.weapon_index != 0) {  // OI abilities: swapped weapons only (format unchanged otherwise)
            h.u8(0x24);
            h.i32(u.weapon_index);
        }
        if (u.weapon_bone != 0) {  // S8 t06 B4: the fire-bone cycle index (format unchanged at 0)
            h.u8(0x0A);
            h.u32(u.weapon_bone);
        }
        if (u.lob_pending || u.lob_charged || u.lob_stamp != 0) {  // S8 t26: fire-at-position order (format unchanged without)
            h.u8(0x37);
            h.f32(u.lob_point.x);
            h.f32(u.lob_point.y);
            h.f32(u.lob_point.z);
            h.u8(u.lob_pending);
            h.u8(u.lob_charged);
            h.u32(u.lob_stamp);
        }
        if (desc_of(u).lobbing_superweapon && desc_of(u).turret) {  // s78f t04: the lobbing turret's angles
            h.u8(0x3B);
            h.f32(u.lob_tpitch);
            h.f32(u.lob_tyaw);
        }
        if (u.tstate != 0) {  // s78f t03: targeting state TD +0x18 and its target (format unchanged at 0)
            h.u8(0x18);
            h.u8(u.tstate);
            h.i32(u.ttarget);
        }
        if (u.auto_hp >= 0) {  // refixf t06 round 3: the scan target's fixed hardpoint (format unchanged without one)
            h.u8(0x29);
            h.i32(u.auto_hp);
        }
        if (u.goal_target != kNoObject || u.aim_target != kNoObject || u.locked) {  // refixf t06 (format unchanged otherwise)
            h.u8(0x28);
            h.i32(u.goal_target);
            h.i32(u.goal_hp);
            h.i32(u.aim_target);
            h.i32(u.aim_hp);
            h.u8(u.locked);
        }
        if (u.infect_attached) {  // refix t02: INFECTION behaviour state (format unchanged without one)
            h.u8(0x54);
            h.u32(u.infect_suspend);
            h.i32(u.infect_desc);
            h.i32(u.infect_player);
            h.i32(u.infect_source);
            h.i32(u.infect_left);  // cu8f t06
        }
        if (u.infect_seen != 0xFFFFFFFFu) {  // cu8f t06: the spread counter on a candidate, once counted
            h.u8(0x55);
            h.u32(u.infect_seen);
            h.i32(u.infect_count);
        }
        for (uint32_t i = 0; i < u.hp_count; ++i) {
            const Hardpoint& hp = hps_[u.hp_begin + i];
            h.i32(hp.opp_target);
            h.i32(hp.opp_hp);
            h.u32(hp.opp_next_scan);
        }
    }
    h.u32(static_cast<uint32_t>(projectiles_.size()));
    for (const Projectile& p : projectiles_) {
        h.u32(p.serial);
        h.u32(p.jammed_frame);
        h.u8(p.ever_jammed);
        if (p.lob) {  // S8 t04: grenade flight state (absent for other shots: format unchanged)
            h.u8(0x33);
            h.f32(p.gravity);
            for (const Vec3& v : {p.launch, p.lob_dir}) {
                h.f32(v.x);
                h.f32(v.y);
                h.f32(v.z);
            }
            h.u8(p.stuck);
            if (p.stuck) {
                h.i32(p.stuck_to);
                h.f32(p.stuck_offset.x);
                h.f32(p.stuck_offset.y);
                h.f32(p.stuck_offset.z);
            }
        }
        if (p.fuse >= 0.0f) h.f32(p.fuse);  // OI abilities: armed bombs only
        if (p.rocket) {  // refixf t01: spline-rocket state (absent for other projectiles: format unchanged)
            h.u8(0x52);
            const Rocket* rk = find_rocket(p.serial);
            if (!rk) continue;
            h.u8(rk->planned);
            h.f32(rk->s);
            h.f32(rk->budget);
            for (const Vec3& v : {rk->fire_at, rk->launch, rk->last_vel}) {
                h.f32(v.x);
                h.f32(v.y);
                h.f32(v.z);
            }
            h.u32(static_cast<uint32_t>(rk->path.count()));
            for (const Vec3& v : rk->path.points()) {
                h.f32(v.x);
                h.f32(v.y);
                h.f32(v.z);
            }
            h.u32(static_cast<uint32_t>(rk->path.segments().size()));
            for (const RocketSpline::Segment& sg : rk->path.segments()) {
                for (const float c : sg.k) h.f32(c);
                h.f32(sg.length);
            }
            h.f32(rk->path.total());
            if (p.rocket == 2) h.i32(rk->thrower);  // cu8k t01
        }
    }
    // OI abilities: base-power entries, hardpoint repair lists (insertion order).
    h.u32(static_cast<uint32_t>(base_power_.size()));
    for (const auto& [pl, src] : base_power_) {
        h.i32(pl);
        h.i32(src);
    }
    h.u32(static_cast<uint32_t>(repairs_.size()));
    for (const Repairer& r : repairs_) {
        h.i32(r.unit);
        h.u32(r.hp);
        h.i32(r.player);
    }
    // Special-weapon hardpoint flags (player +0x2f0): hashed only once one is 0, so battles that never touch them keep
    // their bytes.
    bool any_off = false;
    for (const uint8_t f : sw_hp_enabled_) any_off = any_off || f == 0;
    if (any_off) {
        h.u32(0x5357u);  // tag
        for (const uint8_t f : sw_hp_enabled_) h.u8(f);
    }
}

void CombatSystem::register_extended_crc(CrcRegistry& registry, const std::string& prefix) const {
    registry.add(prefix + ".ext", [this](StateHasher& h) { hash_extended(h); });
}

void CombatSystem::register_crc(CrcRegistry& registry, const std::string& prefix) const {
    registry.add(prefix + ".units", [this](StateHasher& h) { hash_units(h); });
    registry.add(prefix + ".projectiles", [this](StateHasher& h) { hash_projectiles(h); });
    registry.add(prefix + ".modifiers", [this](StateHasher& h) { hash_modifiers(h); });
    registry.add(prefix + ".delayed", [this](StateHasher& h) { hash_delayed(h); });
}
}  // namespace alamo::tactical::combat
