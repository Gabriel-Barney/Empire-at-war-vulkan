// --- L1 systems (owner: tactical-systems agent): the battle side of special weapons, the tactical superweapon,
// hyperspace departures, timed engine disables, teleport, ground height and clip lengths (tactical_battle.hpp
// "L1 systems" block; docs/re/SCRIPT_GAPS.md §1-§3). Serial code only; the synced RNG is drawn by the departure
// timers (HyperspaceDepartures) and nothing else here.
#include <algorithm>
#include <array>

#include "alamo/core/dmath.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/sim/tactical_battle.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;
namespace mv = alamo::tactical::movement;

namespace {

tactical::Vec3 to_t(Vec3 v) { return tactical::Vec3{v.x, v.y, v.z}; }
Vec3 to_s(tactical::Vec3 v) { return Vec3{v.x, v.y, v.z}; }
float yaw_deg_of(Vec3 f) { return dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF); }

struct L1Behaviors {
    objects::BehaviorTypeId special = objects::behavior_type_id("SPECIAL_WEAPON");                  // 0x2f
    objects::BehaviorTypeId lobbing = objects::behavior_type_id("LOBBING_SUPERWEAPON");             // 0x37
    objects::BehaviorTypeId tsw = objects::beh::TacticalSuperWeapon;                                // 0x2c
    objects::BehaviorTypeId tsw_target = objects::behavior_type_id("DUMMY_TACTICAL_SUPER_WEAPON_TARGET");
};
const L1Behaviors& l1_beh() {
    static const L1Behaviors b;
    return b;
}

}  // namespace

// ---- constructor / setup ----

void TacticalBattle::l1_add_types(std::vector<uint32_t>& use) const {
    // The mode's special-weapon types join the battle tables (their weapon projectiles), appended after every other
    // type so the existing indices do not move: space = SPECIAL_WEAPON types with Is_Special_Weapon_In_Space and
    // Special_Weapon_Index >= 0 (the Spawn_Special_Weapon set), land = LOBBING_SUPERWEAPON types. Type index order.
    const objects::GameObjectTypes& types = catalog_->types();
    const L1Behaviors& b = l1_beh();
    for (uint32_t t = 0; t < types.size(); ++t) {
        bool want = false;
        if (space_) {
            want = types.has_behavior(t, objects::BehaviorSet::Space, b.special) && types.get_bool(t, "Is_Special_Weapon_In_Space") &&
                   types.get_int(t, "Special_Weapon_Index") >= 0;
        } else {
            want = types.has_behavior(t, objects::BehaviorSet::Land, b.lobbing);
            // Bombing runs: every Land_Bomber_Type (its bomb projectile in the tables).
            if (const objects::TypeIndex bt = types.get_type_ref(t, "Land_Bomber_Type"); bt != objects::kNone &&
                std::find(use.begin(), use.end(), static_cast<uint32_t>(bt)) == use.end())
                use.push_back(static_cast<uint32_t>(bt));
        }
        if (want && std::find(use.begin(), use.end(), t) == use.end()) use.push_back(t);
    }
}

void TacticalBattle::l1_setup(TacticalMode& mode) {
    ta::SpecialWeaponConfig cfg;
    cfg.fps = 30;
    cfg.space = space_;
    l1_sw_ = ta::SpecialWeaponSystem({}, cfg);
    l1_desc_cache_.clear();
    l1_mode_ = &mode;
    l1_add_strike_projectiles();  // land strikes: Bombardment_Projectile types (before any shot)
    l1_strikes_setup(mode);
    // Space fields: the mode's objects (insertion order) of a field type with SPACE_OBSTACLE (FOG_OF_WAR §1.3 rule).
    l1_fields_.clear();
    const objects::ObjectStore& st = mode.primary();
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::BehaviorTypeId obstacle = objects::behavior_type_id("SPACE_OBSTACLE");
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.alive(h) || obstacle == objects::kNoBehavior || !st.has_behavior(h, obstacle)) continue;
        const objects::TypeIndex t = st.type(h);
        if (t >= types.size()) continue;
        uint32_t kinds = 0;
        if (types.get_bool(t, "Is_Asteroid_Field")) kinds |= ta::kFieldAsteroid;
        if (types.get_bool(t, "Is_Impassable_Asteroid")) kinds |= ta::kFieldImpassable;
        if (types.get_bool(t, "Is_Ion_Storm")) kinds |= ta::kFieldIonStorm;
        if (types.get_bool(t, "Is_Nebula")) kinds |= ta::kFieldNebula;
        if (!kinds) continue;
        const Vec3 p = st.position(h);
        const std::array<float, 4> off = types.get_vec(t, "Space_Obstacle_Offset");
        l1_fields_.add({st.id(h), kinds, p.x + off[0], p.y + off[1], types.get_float(t, "Space_Obstacle_Radius")});
    }
}

// Field behaviours (L1_S4_REMAINING §3.3), from BattleHooks::before_units (combat phase D start, serial, ascending
// id). [port] membership by the object's 2-D position (the original grid query uses the unit's box); NOW = the battle
// frame (the original nebula clock is the global logic frame: only differences are compared).
void TacticalBattle::l1_fields(uint32_t frame, SimRng& rng) {
    if (l1_fields_.empty() || !l1_mode_ || !space_) return;
    objects::ObjectStore& st = l1_mode_->primary();
    static const objects::BehaviorTypeId kNebula = objects::behavior_type_id("NEBULA");
    static const objects::BehaviorTypeId kShielded = objects::behavior_type_id("SHIELDED");
    static const objects::BehaviorTypeId kAsteroid = objects::behavior_type_id("ASTEROID_FIELD_DAMAGE");
    const objects::GameObjectTypes& types = catalog_->types();
    const uint16_t asteroid_damage_type = catalog_->damage().damage_index("Damage_Asteroid");  // [M] type lost in the decompile
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !combat_->has_unit(u.id) || !combat_->alive(u.id)) continue;
        const objects::ObjectHandle h = u.h;
        const tactical::Vec3 p = combat_->position(u.id);
        const bool busy = l1_jumps_.departing(u.id);  // locomotor vf +0x2b0
        // NEBULA (0x437b60): outside the lockout and not busy, query; entry cancels every active ability.
        if (kNebula != objects::kNoBehavior && st.has_behavior(h, kNebula) && abilities_->has_object(u.id) &&
            !abilities_->nebula_lockout(u.id, frame) && !busy) {
            if (l1_fields_.contains(p.x, p.y, ta::kFieldNebula)) {
                if (!abilities_->nebula_stamped(u.id)) abilities_->cancel_active_abilities(u.id, frame);
                abilities_->set_nebula(u.id, frame);
            } else if (abilities_->nebula_stamped(u.id)) {
                abilities_->clear_nebula(u.id);
            }
        }
        // SHIELDED (0x5495e0): ion-storm stamp refreshed every service while inside.
        if (kShielded != objects::kNoBehavior && st.has_behavior(h, kShielded) && abilities_->has_object(u.id)) {
            if (l1_fields_.contains(p.x, p.y, ta::kFieldIonStorm)) abilities_->set_ion_storm(u.id, frame);
            else abilities_->clear_ion_storm(u.id);
        }
        // ASTEROID_FIELD_DAMAGE (0x437310): Capital 1 / Frigate 2 / SuperCapital 0x20 layers, moving, both constants set.
        if (kAsteroid != objects::kNoBehavior && st.has_behavior(h, kAsteroid) && l1_k_.asteroid_field_damage != 0.0f &&
            l1_k_.asteroid_field_damage_rate != 0.0f && !busy) {
            const uint32_t layer = combat_->data().units[combat_->desc_index(u.id)].space_layer;
            const bool moving = u.movable && movement_->view(u.id).speed > 0.0f;
            if (!(layer & 0x23u) || !moving) continue;
            const bool overlap = (l1_fields_.kinds_at(p.x, p.y) & ta::kFieldAsteroid) != 0;  // stamp CD +0x10c (§2.7)
            const auto it = std::lower_bound(l1_in_asteroid_.begin(), l1_in_asteroid_.end(), u.id);
            const bool had = it != l1_in_asteroid_.end() && *it == u.id;
            if (overlap && !had) l1_in_asteroid_.insert(it, u.id);
            if (!overlap && had) l1_in_asteroid_.erase(it);
            for (const ta::SpaceField& f : l1_fields_.fields()) {
                if (!(f.kinds & ta::kFieldAsteroid)) continue;
                const float dx = p.x - f.x, dy = p.y - f.y;
                if (!(dx * dx + dy * dy <= f.radius * f.radius)) continue;
                if (rng.range_float(0.0f, 1.0f) > l1_k_.asteroid_field_damage_rate) continue;  // synced float(0, 1)
                tc::DamageRequest r;
                r.victim = u.id;
                r.amount = l1_k_.asteroid_field_damage;
                r.damage_type = asteroid_damage_type;
                combat_->take_damage(r, frame);
                // One Asteroid_Damage_Hit_Particles entry (synced int); [port] no skeleton: the model centre + 90 z,
                // no bone draw.
                const auto list = types.get_list(st.type(h), "Asteroid_Damage_Hit_Particles");
                if (!list.empty()) {
                    const int32_t k = rng.range_int(0, static_cast<int32_t>(list.size()) - 1);
                    std::string name;
                    for (char ch : list[static_cast<size_t>(k)])
                        if (ch != ' ' && ch != '\t') name.push_back(ch);
                    const objects::TypeIndex pt = types.resolve_name(name);
                    if (pt != objects::kNone) spawn_particle(*l1_mode_, static_cast<int32_t>(pt), Vec3{p.x, p.y, p.z + 90.0f}, st.owner(h));
                }
                if (!combat_->alive(u.id)) break;
            }
        }
    }
}

// Is_In_Nebula / Is_In_Ion_Storm / Is_In_Asteroid_Field of a battle object (L1_S4_REMAINING §2.7, the non-team cases):
// with the victim behaviour the behaviour's stamp (nebula = lockout, ion storm = stamp within the disable time,
// asteroid = stamp set), else the geometric test at its 2-D position.
int32_t TacticalBattle::object_in_field(objects::ObjectId id, uint32_t kind) const {
    if (!l1_mode_) return -1;
    const objects::ObjectStore& st = l1_mode_->primary();
    const objects::ObjectHandle h = st.find(id);
    if (!st.alive(h)) return -1;
    static const objects::BehaviorTypeId kNebula = objects::behavior_type_id("NEBULA");
    static const objects::BehaviorTypeId kShielded = objects::behavior_type_id("SHIELDED");
    static const objects::BehaviorTypeId kAsteroid = objects::behavior_type_id("ASTEROID_FIELD_DAMAGE");
    const bool pack = combat_ && combat_->has_unit(id) && abilities_->has_object(id);
    if (kind == ta::kFieldNebula && pack && st.has_behavior(h, kNebula)) return abilities_->nebula_lockout(id, frame_) ? 1 : 0;
    if (kind == ta::kFieldIonStorm && pack && st.has_behavior(h, kShielded)) return abilities_->ion_storm_lockout(id, frame_) ? 1 : 0;
    if (kind == ta::kFieldAsteroid && pack && st.has_behavior(h, kAsteroid))
        return std::binary_search(l1_in_asteroid_.begin(), l1_in_asteroid_.end(), id) ? 1 : 0;
    const Vec3 p = st.position(h);
    return (l1_fields_.kinds_at(p.x, p.y) & kind) ? 1 : 0;
}

objects::ObjectId TacticalBattle::nearest_field(float x, float y, uint32_t mask) const {
    const int32_t i = l1_fields_.nearest(x, y, mask);
    return i < 0 ? objects::kNoObjectId : l1_fields_.fields()[static_cast<size_t>(i)].id;
}

uint32_t TacticalBattle::l1_desc_of(int32_t type) {
    for (const auto& [t, d] : l1_desc_cache_)
        if (t == type) return d;
    const objects::GameObjectTypes& types = catalog_->types();
    const auto ti = static_cast<objects::TypeIndex>(type);
    const objects::BehaviorSet set = space_ ? objects::BehaviorSet::Space : objects::BehaviorSet::Land;
    const L1Behaviors& b = l1_beh();
    ta::SpecialWeaponDesc d;
    if (types.has_behavior(ti, set, b.special)) d.kinds |= ta::kSwSpecialWeapon;
    if (types.has_behavior(ti, set, b.lobbing)) d.kinds |= ta::kSwLobbing;
    if (types.has_behavior(ti, set, b.tsw)) d.kinds |= ta::kSwTsw;
    d.recharge_seconds = types.get_float(ti, "Projectile_Fire_Recharge_Seconds");
    d.pulse_count = std::max(1, types.get_int(ti, "Projectile_Fire_Pulse_Count"));
    d.pulse_delay_seconds = types.get_float(ti, "Projectile_Fire_Pulse_Delay_Seconds");
    for (size_t i = 0; i < types_used_.size(); ++i)  // the weapon projectile from the battle tables
        if (static_cast<int32_t>(types_used_[i]) == type && descs_[i] < tables_->combat.units.size())
            d.projectile = tables_->combat.units[descs_[i]].listed_projectile;  // S8 t08: with or without WEAPON
    d.valid_targets = types.get_mask(ti, "Special_Weapon_Valid_Targets");
    d.requires_base_power = types.get_bool(ti, "Requires_Base_Power");
    d.target_action_index = types.get_int(ti, "Special_Weapon_Target_Action_Index");
    d.max_speed = ti < types.columns().max_speed.size() ? types.columns().max_speed[ti] : 0.0f;
    d.tsw_start_distance = types.get_float(ti, "TSW_Start_Distance_From_Target");
    d.tsw_attack_distance = types.get_float(ti, "TSW_Attack_Distance_From_Target");
    d.tsw_match_target_z = types.get_bool(ti, "TSW_Start_Pos_Match_Targets_Z");
    d.tsw_z_adjust = types.get_float(ti, "TSW_Z_Adjust_Relative_To_Target_Pos");
    d.tsw_power_up_seconds = types.get_float(ti, "TSW_Power_Up_Countdown_Seconds");
    d.tsw_attack_anim_seconds = types.get_float(ti, "TSW_Attack_Anim_Duration_Seconds");
    d.tsw_debris_delay_frames = types.get_int(ti, "TSW_Explosion_Debris_Creation_Frame_Delay");
    d.tsw_post_destruction_frames = types.get_int(ti, "TSW_Post_Destruction_Wait_Frames");
    const uint32_t idx = l1_sw_.add_desc(d);
    l1_desc_cache_.emplace_back(type, idx);
    return idx;
}

void TacticalBattle::l1_register(TacticalMode& mode, objects::ObjectHandle h, int32_t type) {
    const objects::ObjectStore& st = mode.primary();
    if (!st.alive(h)) return;
    // DAMAGE_TRACKING (0x3e): the damage-rate tracker of Get_Rate_Of_Damage_Taken / Get_Time_Till_Dead.
    if (st.has_behavior(h, objects::BehaviorTypeId{0x3E})) {
        L1Tracker t;
        t.id = st.id(h);
        t.next = frame_;
        const auto it = std::lower_bound(l1_trackers_.begin(), l1_trackers_.end(), t.id,
                                         [](const L1Tracker& a, objects::ObjectId v) { return a.id < v; });
        if (it == l1_trackers_.end() || it->id != t.id) l1_trackers_.insert(it, t);
    }
    const L1Behaviors& b = l1_beh();
    if (!st.has_behavior(h, b.special) && !st.has_behavior(h, b.lobbing) && !st.has_behavior(h, b.tsw)) return;
    if (l1_sw_.add(st.id(h), l1_desc_of(type), st.owner(h), frame_)) l1_used_ = true;
}

void TacticalBattle::adopt_object(TacticalMode& mode, objects::ObjectHandle h) {
    const objects::ObjectStore& st = mode.primary();
    if (st.alive(h)) l1_register(mode, h, static_cast<int32_t>(st.type(h)));
}

ta::SpecialWeaponWorld TacticalBattle::l1_world(const TacticalMode& mode) {
    const objects::ObjectStore* st = &mode.primary();
    ta::SpecialWeaponWorld w;
    // S8 t26 / t30 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order"): the order (point + pending) goes to the weapon's own
    // WeaponBehavior, which charges and fires it.
    w.fire_at_position = [this](tc::ObjectId id, tactical::Vec3 p) { return combat_ != nullptr && combat_->order_fire_at_position(id, p); };
    w.owner_of = [st](tc::ObjectId t) {
        const objects::ObjectHandle h = st->find(t);
        return st->alive(h) ? st->owner(h) : -1;
    };
    // "not allied" [M]: the relation matrix of the battle (enemies may be fired at; same / allied players not).
    w.allied = [this, st](int32_t owner, tc::ObjectId t) {
        const objects::ObjectHandle h = st->find(t);
        return !st->alive(h) || !combat_->is_enemy(owner, st->owner(h));
    };
    w.category_mask = [this, st](tc::ObjectId t) -> uint64_t {
        if (combat_->has_unit(t)) return combat_->data().units[combat_->desc_index(t)].category_mask;
        const objects::ObjectHandle h = st->find(t);
        return st->alive(h) ? catalog_->types().get_mask(st->type(h), "CategoryMask") : 0u;
    };
    w.target_flagged = [st](tc::ObjectId t) { return !st->alive(st->find(t)); };  // obj +0x3a0 & 0x40 [M: dying]
    // OI abilities: the Base_Power_Ability list (TACTICAL_GAPS §7.3; empty = no power) replaces the "passes" stand-in.
    w.base_power = [this](int32_t p) { return combat_ && combat_->has_base_power(p); };
    const objects::BehaviorTypeId dummy = l1_beh().tsw_target;
    w.find_tsw_target = [st, dummy](tc::ObjectId, tc::ObjectId& target, tactical::Vec3& pos) {
        for (const objects::ObjectHandle h : st->all_objects())  // the first one (insertion order) [M]
            if (st->alive(h) && st->has_behavior(h, dummy)) {
                target = st->id(h);
                pos = to_t(st->position(h));
                return true;
            }
        return false;
    };
    w.position = [st](tc::ObjectId id) { return to_t(st->position(st->find(id))); };
    TacticalMode* m = l1_mode_;
    w.place = [this, m](tc::ObjectId id, tactical::Vec3 p, tactical::Vec3 f) {
        if (m) teleport(*m, id, to_s(p), to_s(f));
    };
    w.is_battle_attacker = [this](int32_t owner) { return owner == req_.info.attacker; };  // 0x28aec0 [M]
    // TSW_PLANET_DESTROY §4 step 4: the target dummy type's Death_Explosions count (0 when the dummy is gone: no draw).
    const objects::GameObjectTypes* types = &catalog_->types();
    w.debris_choices = [st, types](tc::ObjectId target) {
        const objects::ObjectHandle h = st->find(target);
        return st->alive(h) ? static_cast<uint32_t>(types->get_type_refs(st->type(h), "Death_Explosions").size()) : 0u;
    };
    return w;
}

// ---- special weapons / TSW API ----

float TacticalBattle::sw_readiness(objects::ObjectId weapon) const { return l1_sw_.readiness(weapon, frame_); }

bool TacticalBattle::sw_begin_targeting(objects::ObjectId weapon) { return l1_sw_.begin_targeting(weapon, frame_); }

bool TacticalBattle::sw_fire_at_object(const TacticalMode& mode, objects::ObjectId weapon, objects::ObjectId target) {
    if (finished_) return false;
    return l1_sw_.fire_at_object(weapon, target, frame_, l1_world(mode));
}

bool TacticalBattle::sw_fire_lobbing_at(const TacticalMode& mode, objects::ObjectId weapon, Vec3 point) {
    if (finished_) return false;
    return l1_sw_.fire_lobbing_at(weapon, to_t(point), frame_, l1_world(mode));
}

bool TacticalBattle::special_weapons_allowed(int32_t player) const {
    // campaign: no per-player rule (0x527620); skirmish: the player's special-weapon hard-point flag (HARDPOINT_DISABLED §4).
    if (cfg_.skirmish && combat_ && !combat_->special_weapon_hp_enabled(player)) return false;
    if (l1_planet_destroyed_) return false;
    const PlanetIndex p = req_.info.planet;
    return !(p < galaxy_->planets().size() && galaxy_->planets()[p].destroyed);
}

int32_t TacticalBattle::tsw_state(objects::ObjectId tsw) const { return l1_sw_.tsw_state(tsw); }

bool TacticalBattle::tsw_fire(objects::ObjectId tsw) {
    // 0x42a720: the battle planet exists in the parent (galactic) store, is not destroyed and its type has
    // Planet_Is_Destroyable_By_TSW.
    bool planet_ok = false;
    const PlanetIndex p = req_.info.planet;
    if (!finished_ && !l1_planet_destroyed_ && p < galaxy_->planets().size() && !galaxy_->planets()[p].destroyed) {
        const uint32_t gt = galaxy_->planets()[p].type;
        if (gt < galaxy_->rules().types.size()) {
            const objects::TypeIndex t = catalog_->types().resolve_name(galaxy_->rules().types[gt].name);
            planet_ok = t != objects::kNone && catalog_->types().get_bool(t, "Planet_Is_Destroyable_By_TSW");
        }
    }
    return l1_sw_.tsw_fire(tsw, planet_ok, frame_);
}

// ---- motion: teleport, hyperspace, engines ----

bool TacticalBattle::teleport(TacticalMode& mode, objects::ObjectId id, Vec3 position, Vec3 facing) {
    objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle h = st.find(id);
    if (!st.alive(h)) return false;
    st.set_position(h, position);
    st.set_facing(h, facing);
    if (const Unit* u = unit_by_id(id); u && u->movable && movement_)
        movement_->teleport(id, {position.x, position.y}, position.z, yaw_deg_of(facing), frame_);
    if (combat_ && combat_->has_unit(id)) {
        combat_->set_position(id, to_t(position));
        combat_->set_facing(id, to_t(facing));
    }
    mode.trees().update(static_cast<tactical::EntityHandle>(id), mode.bounds_of(h));  // no-op when not in the trees
    return true;
}

bool TacticalBattle::hyperspace_out(objects::ObjectId id, int32_t delay_frames, bool keep_parent_link) {
    Unit* u = unit_by_id(id);
    if (finished_ || !u || !u->alive || u->retreated || !l1_mode_ || !space_) return false;
    objects::ObjectStore& st = l1_mode_->primary();
    if (!st.has_behavior(u->h, objects::beh::Locomotor)) return false;
    // vf +0x2a8 is implemented by the FIGHTER and SIMPLE_SPACE locomotors only (STARSHIP_LOCOMOTOR: empty stub).
    const objects::GameObjectTypes& types = catalog_->types();
    bool implemented = false;
    for (const char* tag : {"Behavior", "SpaceBehavior"})
        for (std::string_view b : types.get_list(st.type(u->h), tag)) {
            std::string up;
            for (char ch : b)
                if (ch != ' ' && ch != '\t' && ch != ',') up.push_back(ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch);
            implemented = implemented || up == "FIGHTER_LOCOMOTOR" || up == "SIMPLE_SPACE_LOCOMOTOR";
        }
    if (!implemented) return false;
    const int32_t owner = st.owner(u->h);
    if (!l1_jumps_.schedule(id, static_cast<uint32_t>(std::max(0, delay_frames)), keep_parent_link, to_t(st.position(u->h)),
                            to_t(st.facing(u->h)), retreat_flight_increment(owner)))
        return false;
    // Stop at once: out of path following (the departure owns the position), locomotor busy (ABILITIES §2.4 gate 9).
    if (u->movable) {
        movement_->remove_unit(id);
        u->movable = false;
    }
    if (abilities_->has_object(id)) abilities_->set_locomotor_busy(id, true);
    l1_used_ = true;
    return true;
}

float TacticalBattle::retreat_flight_increment(int32_t player) const {
    if (player >= 0 && static_cast<size_t>(player) < galaxy_->players().size()) {
        const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
        if (f < l1_flight_increments_.size() && l1_flight_increments_[f] > 0.0f) return l1_flight_increments_[f];
    }
    return 500.0f;  // [port] the shipped value when the faction does not set it
}
int32_t TacticalBattle::engines_online(objects::ObjectId id) const {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, objects::ObjectId v) { return u.id < v; });
    if (it == units_.end() || it->id != id || !l1_mode_ || !l1_mode_->primary().has_behavior(it->h, objects::beh::Locomotor)) return -1;
    return (it->engines_offline || !l1_engines_.online(id)) ? 0 : 1;
}

bool TacticalBattle::disable_engines(objects::ObjectId id, float seconds) {
    if (engines_online(id) < 0) return false;
    l1_engines_.disable(id, seconds, frame_);
    l1_used_ = true;
    // cu8 t04 (STATION_HANGARS_SOUND §3.3): a timed engine disable turns both engine loops off (audio requests only).
    if (const Unit* u = unit_by_id(id); u && hg_engine_unit(*u)) engine_loops_.timed_disable(id, frame_);
    return true;
}

void TacticalBattle::energy_drain_engines(objects::ObjectId id, float seconds) {
    // cu8f t02 (CU8F_T01_T02_T07_RE.md §2.4), after EnergyPool::Absorb took energy > 0: 1. with a locomotor,
    // Locomotor_Set_Engines(destroyed 0, t) (EngineControl::disable keeps the 0x5545a0 gate and the max expiry), then the
    // current move stops (0x422040); 2. signal 0x16 [port: not modelled]; 3. moving loop off, then idle loop off.
    const Unit* u = unit_by_id(id);
    if (!u || !u->alive || u->retreated) return;
    if (engines_online(id) >= 0) {
        l1_engines_.disable(id, seconds, frame_);
        l1_used_ = true;
        if (u->movable && movement_ && movement_->has_unit(id)) {
            const std::vector<objects::ObjectId> ids{id};
            movement_->stop(ids);
        }
    }
    if (hg_engine_unit(*u)) engine_loops_.timed_disable(id, frame_);
}

bool TacticalBattle::enable_engines(objects::ObjectId id) {
    if (engines_online(id) < 0) return false;
    l1_engines_.enable(id, frame_);
    return true;
}

void TacticalBattle::l1_engines_destroyed(objects::ObjectId id) {
    l1_engines_.destroy(id, frame_);
    l1_used_ = true;
}

float TacticalBattle::ground_height(float x, float y) const {
    if (space_ || !map_.terrain.present) return -3.0e38f;
    float z = map_.terrain.sample_height(x, y);
    if (map_.terrain.header.water_type != terrain::WaterType::None) z = std::max(z, map_.terrain.header.water_height);
    return z;
}

uint32_t TacticalBattle::anim_clip_frames(const TacticalMode& mode, objects::ObjectId id, uint8_t anim_type) const {
    uint32_t frames = 0;
    float fps = 0.0f;
    return anim_clip_info(mode, id, anim_type, frames, fps) ? frames : 0u;
}

bool TacticalBattle::anim_clip_info(const TacticalMode& mode, objects::ObjectId id, int32_t anim_type, uint32_t& frames,
                                    float& fps, uint32_t variant) const {
#ifdef ALAMO_SIM_HAVE_ANIM
    if (!anim_ || anim_type < 0) return false;
    const objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle h = st.find(id);
    if (!st.exists(h)) return false;
    const anim::AnimModel* m = anim_->model_for(st.type(h), mode.kind());
    const anim::ClipRef* c = m ? m->clip(anim_type, variant) : nullptr;
    if (!c || !c->clip || c->clip->frame_count == 0) return false;
    frames = c->clip->frame_count;
    fps = c->clip->fps;
    return true;
#else
    (void)mode, (void)id, (void)anim_type, (void)frames, (void)fps, (void)variant;
    return false;
#endif
}

bool TacticalBattle::play_clip(const TacticalMode& mode, objects::ObjectId id, uint8_t anim_type, float start_fraction, float speed,
                               uint64_t global_frame) {
#ifdef ALAMO_SIM_HAVE_ANIM
    const uint32_t frames = anim_clip_frames(mode, id, anim_type);
    if (!anim_ || frames == 0) return false;
    anim::AnimWorld& w = anim_->world(mode);
    if (!w.contains(id)) return false;
    anim::PlayRequest r;
    r.type = anim_type;
    r.index = 0;
    r.loop = false;
    r.speed = speed;
    // Start frame int((frames - 1) x fraction) (TransportLandingBehavior 0x642c70).
    r.start_frame = static_cast<float>(static_cast<int32_t>(static_cast<float>(frames - 1) * std::clamp(start_fraction, 0.0f, 1.0f)));
    w.play(id, r, nullptr, global_frame);
    ++stats_.anim_plays;
    return true;
#else
    (void)mode, (void)id, (void)anim_type, (void)start_fraction, (void)speed, (void)global_frame;
    return false;
#endif
}

int32_t TacticalBattle::retreat_increment_frames(int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return 0;
    const uint32_t f = galaxy_->players()[static_cast<size_t>(player)].faction;
    return f < l1_retreat_increments_.size() ? l1_retreat_increments_[f] : 0;
}

void TacticalBattle::l1_force_disable(const TacticalMode& mode) {
    if (!abilities_) return;
    const objects::GameObjectTypes& types = catalog_->types();
    if (force_disable_range_.size() != types.size()) {
        force_disable_range_.assign(types.size(), 0.0f);
        for (objects::TypeIndex t = 0; t < types.size(); ++t)
            force_disable_range_[t] = types.get_float(t, "Disable_Force_Abilities_Range");  // read as a float [M]
    }
    // cu8f t07 (CU8F_T01_T02_T07_RE.md §3.1): DisableForceAbilitiesBehaviorClass (0x52) Service 0x636260, interval 1, so
    // every frame. Only objects that carry DISABLE_FORCE_ABILITIES in their current mode's list emit (FoC: Ysalamiri_Tree,
    // Underworld_Ysalamiri_Cage, land only; Rebel_Mobile_Shield_Generator's range tag is dead data). Query 0x2a9ff0:
    // SELECTABLE objects of every player but the emitter's owner (allies and neutral included), 3-D distance² < r².
    // Victim stamp CD +0x110 := now when it has a Force (F column) unit ability; nothing ever clears it (gate 3 / 0x380b10
    // ends the lockout passively).
    const objects::ObjectStore& st = mode.primary();
    for (const objects::ObjectHandle e : st.all_objects()) {  // emitters, store order
        const objects::TypeIndex et = st.type(e);
        if (!st.alive(e) || et >= force_disable_range_.size() || !(force_disable_range_[et] > 0.0f)) continue;
        if (!st.has_behavior(e, objects::beh::DisableForceAbilities)) continue;
        const float r = force_disable_range_[et];
        const Vec3 ep = st.position(e);
        const int32_t owner = st.owner(e);
        for (const Unit& u : units_) {  // ascending id; the stamp is order-independent
            if (!u.alive || u.retreated || !st.alive(u.h) || st.owner(u.h) == owner) continue;
            if (!st.has_behavior(u.h, objects::beh::Selectable)) continue;
            const Vec3 p = st.position(u.h);
            const float dx = p.x - ep.x, dy = p.y - ep.y, dz = p.z - ep.z;
            if (!(dx * dx + dy * dy + dz * dz < r * r)) continue;
            if (abilities_->has_force_ability(u.id)) abilities_->set_force_disabled(u.id, frame_);
        }
    }
}

void TacticalBattle::l1_motion(TacticalMode& mode, SimRng& rng) {
    if (!l1_used_) return;
    objects::ObjectStore& st = mode.primary();
    l1_engines_.clear_events();
    l1_engines_.tick(frame_);
    if (l1_jumps_.empty()) return;
    (void)rng;  // no draws (L1_S4_REMAINING §3.4: the departure uses no RNG)
    for (const Unit& u : units_)  // dead units lose their departure
        if ((!u.alive || u.retreated) && l1_jumps_.departing(u.id)) l1_jumps_.remove(u.id);
    l1_jumps_.tick(frame_);
    // Positions of the departing units (store, combat, trees).
    for (const Unit& u : units_) {
        if (!u.alive || u.retreated || !l1_jumps_.departing(u.id)) continue;
        const Vec3 p = to_s(l1_jumps_.position(u.id));
        st.set_position(u.h, p);
        st.set_facing(u.h, to_s(l1_jumps_.facing(u.id)));
        if (combat_->has_unit(u.id)) combat_->set_position(u.id, to_t(p));
        mode.trees().update(static_cast<tactical::EntityHandle>(u.id), mode.bounds_of(u.h));
    }
    for (const mv::HyperspaceEvent& e : l1_jumps_.events()) {
        if (e.kind != mv::HyperspaceEvent::Kind::Killed) continue;  // Jumped: presentation (SFX, model stretch)
        Unit* u = unit_by_id(e.id);
        if (!u || !u->alive || u->retreated) continue;
        // Killed (0x391310, flag 1). Parent link cleared: the galactic unit survives (a retreat for the battle result);
        // kept: a death (the galactic counterpart is death-marked).
        if (e.keep_parent_link) {
            u->alive = false;
            --parts_[u->participant].alive;
            ++stats_.deaths;
        } else {
            u->retreated = true;
        }
        abilities_->remove(u->id);
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        if (u->tracked) damage_tracking_.disable(u->id);
#endif
        unit_gone(mode, *u);
        if (st.alive(u->h)) mode.destroy(u->h);
    }
}

void TacticalBattle::l1_weapons(TacticalMode& mode, SimRng& rng) {
    l1_sw_events_.clear();    l1_damage_stats();
    l1_tsw_kill_owner_ = -1;  // this tick's TSW planet kill (read by the battle host after the tick)
    l1_strikes(mode, rng);
    // S8 t30: the lobbing charge-up events of this tick's combat step (presentation, ascending id per combat order).
    if (combat_)
        for (const tc::LobEvent& e : combat_->lob_events()) {
            ta::SpecialWeaponEvent ev;
            ev.kind = e.charged ? ta::SwEventKind::LobCharged : ta::SwEventKind::LobChargeup;
            ev.id = e.id;
            ev.frame = e.frame;
            ev.point = e.point;
            l1_sw_events_.push_back(ev);
        }

    if (l1_sw_.empty()) return;
    objects::ObjectStore& st = mode.primary();
    l1_sw_.prune([&st](tc::ObjectId id) { return st.alive(st.find(id)); });
    l1_sw_.tick(frame_, rng, l1_world(mode));
    for (const ta::SpecialWeaponShot& s : l1_sw_.shots()) {
        const objects::ObjectHandle wh = st.find(s.weapon);
        if (!st.alive(wh) || s.projectile < 0) continue;
        tc::ProjectileLaunch l;
        l.shooter = s.weapon;
        l.owner = s.owner;
        l.projectile = static_cast<uint32_t>(s.projectile);
        l.from = to_t(st.position(wh));
        l.target = s.at_point ? tc::kNoObject : s.target;
        l.point = s.point;
        combat_->launch_projectile(l, frame_);  // a dead / non-combat target: the pulse is lost [port]
    }
    for (const ta::SpecialWeaponEvent& e : l1_sw_.events()) {
        l1_sw_events_.push_back(e);
        if (e.kind == ta::SwEventKind::TswPlanetDestroyed) l1_tsw_destruction(mode, e);
        if (e.kind == ta::SwEventKind::TswDebris) l1_tsw_debris(mode, e);
    }
    l1_sw_.clear_outputs();
}

// TSW_PLANET_DESTROY §4 steps 3-6 (the galactic steps 7-9 are the battle host's, CampaignBattles::hook): no live target
// dummy -> nothing; else the Death_Explosions pick at the dummy for the neutral player (0x294aa0), the dummy killed
// (0x391310), its transform saved for the state-10 debris. The presentation steps (camera shake 0x2cdbb0 at the dummy,
// its Death_SFXEvent_Start_Die) are the game host's: it reads the TswPlanetDestroyed event from special_weapon_events()
// after the tick, while the dummy is dead but not flushed (engine/apps/game battle_fx.hpp, GameAudioHost::tsw_audio).
void TacticalBattle::l1_tsw_destruction(TacticalMode& mode, const ta::SpecialWeaponEvent& e) {
    objects::ObjectStore& st = mode.primary();
    const objects::ObjectHandle dh = st.find(e.target);
    if (!st.alive(dh)) return;
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::TypeIndex dt = st.type(dh);
    const Vec3 pos = st.position(dh), facing = st.facing(dh);
    const int32_t neutral = galaxy_->neutral_player();
    const auto explosions = types.get_type_refs(dt, "Death_Explosions");
    if (e.value >= 0 && static_cast<size_t>(e.value) < explosions.size() && explosions[static_cast<size_t>(e.value)] != objects::kNone)
        spawn_particle(mode, static_cast<int32_t>(explosions[static_cast<size_t>(e.value)]), pos, neutral);
    bt_kill_silently(mode, dh);
    L1TswDebris d;
    d.tsw = e.id;
    d.type = static_cast<int32_t>(dt);
    d.pos = pos;
    d.facing = facing;
    std::erase_if(l1_tsw_debris_, [&d](const L1TswDebris& x) { return x.tsw == d.tsw; });
    l1_tsw_debris_.push_back(d);
    l1_planet_destroyed_ = true;  // D +0x2c8 of the battle planet (SpecialWeaponsAllowed, TSW Fire)
    const objects::ObjectHandle th = st.find(e.id);
    l1_tsw_kill_owner_ = st.alive(th) ? st.owner(th) : -1;  // the galactic part (host, same tick)
}

// State-10 end (§3 / §4): every Death_By_TSW_Replacements entry of the dead dummy's type at its saved transform, for
// the neutral player, in the main store. No draw.
void TacticalBattle::l1_tsw_debris(TacticalMode& mode, const ta::SpecialWeaponEvent& e) {
    const auto it = std::find_if(l1_tsw_debris_.begin(), l1_tsw_debris_.end(), [&e](const L1TswDebris& x) { return x.tsw == e.id; });
    if (it == l1_tsw_debris_.end()) return;  // no dummy at state 9
    const L1TswDebris d = *it;
    l1_tsw_debris_.erase(it);
    const int32_t neutral = galaxy_->neutral_player();
    for (const objects::TypeIndex t : catalog_->types().get_type_refs(static_cast<objects::TypeIndex>(d.type), "Death_By_TSW_Replacements"))
        if (t != objects::kNone && spawn_prop(mode, static_cast<int32_t>(t), d.pos, d.facing, neutral)) ++stats_.props;
}

// ---- tactical weather (land; L1_S4_REMAINING §3.2) ----
void TacticalBattle::l1_weather_tick(SimRng& rng) {
    if (space_ || !l1_weather_data_) return;
    if (!l1_weather_started_) {  // the first service's re-init starts the map environment's scenario
        l1_weather_started_ = true;
        const terrain::EnvironmentSet& env = map_.environments;
        std::string name;
        if (env.start_index < env.environments.size()) name = env.environments[env.start_index].weather_effect;
        const tactical::combat::WeatherScenario* sc = l1_weather_data_->find(name);
        if (!sc) return;
        l1_weather_.start(sc, l1_weather_data_.get(), frame_, rng);
        l1_used_ = true;
    } else {
        l1_weather_note_fow();  // Int-final: events raised between ticks (Force_Weather) reach the fog first
        l1_weather_.clear_events();
    }
    if (!l1_weather_.active()) return;
    l1_weather_.service(frame_, rng);
    l1_weather_note_fow();  // Int-final
    combat_->set_weather_accuracy(l1_weather_.accuracy_row());
}

// Int-final: the fog's weather hookup (FOG_OF_WAR §3.1.1 / L1_S4_REMAINING §3.2): FowDirty -> every REVEAL source dirty.
void TacticalBattle::l1_weather_note_fow() {
    for (const tactical::combat::WeatherEvent& e : l1_weather_.events())
        if (e.kind == tactical::combat::WeatherEvent::Kind::FowDirty) fog_weather_dirty_ = true;
}

bool TacticalBattle::force_weather(SimRng& rng) {
    if (space_ || finished_ || !l1_weather_.active()) return false;
    const bool ok = l1_weather_.force_weather(frame_, rng);
    combat_->set_weather_accuracy(l1_weather_.accuracy_row());
    return ok;
}

float TacticalBattle::weather_sight_modifier(objects::ObjectId id) const {
    if (!l1_weather_.active() || !l1_mode_) return 1.0f;
    const objects::ObjectStore& st = l1_mode_->primary();
    const objects::ObjectHandle h = st.find(id);
    if (!st.exists(h)) return 1.0f;
    if (abilities_ && !abilities_->weather_affected(st.owner(h))) return 1.0f;  // cu8 t01: Weatherproof owner
    return l1_weather_.sight_modifier(catalog_->types().get_string(st.type(h), "Weather_Category"));
}

// ---- damage statistics (L1_S4_REMAINING §2.4) ----
// [port] trackers / final-blow ids derive from the hashed combat state and are not hashed themselves.
void TacticalBattle::l1_set_final_blow(objects::ObjectId id, int32_t player) {
    const auto it = std::lower_bound(l1_final_blow_.begin(), l1_final_blow_.end(), id,
                                     [](const std::pair<objects::ObjectId, int32_t>& a, objects::ObjectId v) { return a.first < v; });
    if (it != l1_final_blow_.end() && it->first == id) it->second = player;
    else l1_final_blow_.insert(it, {id, player});
}

void TacticalBattle::l1_damage_stats() {
    for (const tc::DamageEvent& e : combat_->damage_events()) {
        if (e.source_owner >= 0) {  // 0x39bdb0: the attacker's player, copied to the team container
            l1_set_final_blow(e.victim, e.source_owner);
            if (const Unit* u = unit_by_id(e.victim); u && u->container != 0xFFFFFFFFu)
                l1_set_final_blow(containers_[u->container].id, e.source_owner);
        }
        const auto it = std::lower_bound(l1_trackers_.begin(), l1_trackers_.end(), e.victim,
                                         [](const L1Tracker& a, objects::ObjectId v) { return a.id < v; });
        if (it != l1_trackers_.end() && it->id == e.victim) it->acc += e.after_multipliers;
    }
    for (const tc::DeathEvent& d : combat_->deaths())  // 0x3a01f0: the killer's owner at death
        if (d.killer != tc::kNoObject && l1_mode_) {
            const objects::ObjectStore& st = l1_mode_->primary();
            const objects::ObjectHandle kh = st.find(d.killer);
            if (st.exists(kh)) l1_set_final_blow(d.id, st.owner(kh));
        }
    for (L1Tracker& t : l1_trackers_)
        if (frame_ >= t.next) {  // service: rate = FPS x accumulated / interval (30 frames)
            t.rate = 30.0f * t.acc / 30.0f;
            t.acc = 0.0f;
            t.next = frame_ + 30;
        }
}

int32_t TacticalBattle::final_blow_player(objects::ObjectId id) const {
    const auto it = std::lower_bound(l1_final_blow_.begin(), l1_final_blow_.end(), id,
                                     [](const std::pair<objects::ObjectId, int32_t>& a, objects::ObjectId v) { return a.first < v; });
    return (it != l1_final_blow_.end() && it->first == id) ? it->second : -1;
}

double TacticalBattle::rate_of_damage(objects::ObjectId id) const {
    auto rate_of = [this](objects::ObjectId x) -> double {
        const auto it = std::lower_bound(l1_trackers_.begin(), l1_trackers_.end(), x,
                                         [](const L1Tracker& a, objects::ObjectId v) { return a.id < v; });
        return (it != l1_trackers_.end() && it->id == x) ? static_cast<double>(it->rate) : 0.0;
    };
    for (size_t c = 0; c < containers_.size(); ++c)
        if (containers_[c].id == id) {  // TEAM: the sum over the members
            double s = 0.0;
            for (const Unit& u : units_)
                if (u.container == c && u.alive && !u.retreated) s += rate_of(u.id);
            return s;
        }
    if (!combat_->has_unit(id) || !combat_->alive(id)) return -1.0;
    return rate_of(id);
}

double TacticalBattle::time_till_dead(objects::ObjectId id) const {
    auto hull = [this](objects::ObjectId x) -> double {  // destroyable hard points' health when any, else the hull
        const tc::UnitDesc& ud = combat_->data().units[combat_->desc_index(x)];
        double s = 0.0;
        bool any = false;
        for (uint32_t k = 0; k < combat_->hardpoint_count(x) && k < ud.hardpoints.size(); ++k)
            if (combat_->data().hardpoints[ud.hardpoints[k]].destroyable) {
                any = true;
                s += std::max(0.0, static_cast<double>(combat_->hardpoint_health(x, k)));
            }
        return any ? s : static_cast<double>(combat_->health(x));
    };
    auto tracker = [this](objects::ObjectId x) -> const L1Tracker* {
        const auto it = std::lower_bound(l1_trackers_.begin(), l1_trackers_.end(), x,
                                         [](const L1Tracker& a, objects::ObjectId v) { return a.id < v; });
        return (it != l1_trackers_.end() && it->id == x) ? &*it : nullptr;
    };
    for (size_t c = 0; c < containers_.size(); ++c)
        if (containers_[c].id == id) {  // TEAM: member health / member rates (members with a tracker)
            double h = 0.0, r = 0.0;
            for (const Unit& u : units_) {
                if (u.container != c || !u.alive || u.retreated || !combat_->has_unit(u.id)) continue;
                const L1Tracker* t = tracker(u.id);
                if (!t) continue;
                h += static_cast<double>(combat_->health(u.id));
                r += static_cast<double>(t->rate);
            }
            return r > 0.0 ? h / r : 1.0e18;
        }
    if (!combat_->has_unit(id) || !combat_->alive(id)) return -1.0;
    const L1Tracker* t = tracker(id);
    if (!t || !(t->rate > 0.0f)) return 1.0e18;
    return (hull(id) + static_cast<double>(combat_->shield(id))) / static_cast<double>(t->rate);
}

std::vector<objects::TypeIndex> TacticalBattle::projectile_types(objects::ObjectId id) const {
    std::vector<objects::TypeIndex> out;
    objects::ObjectId x = id;
    for (size_t c = 0; c < containers_.size(); ++c)
        if (containers_[c].id == id) {  // TEAM: the representative member
            x = objects::kNoObjectId;
            for (const Unit& u : units_)
                if (u.container == c && u.alive && !u.retreated) {
                    x = u.id;
                    break;
                }
        }
    if (x == objects::kNoObjectId || !l1_mode_) return out;
    const objects::GameObjectTypes& types = catalog_->types();
    if (combat_->has_unit(x) && combat_->alive(x)) {
        const tc::UnitDesc& ud = combat_->data().units[combat_->desc_index(x)];
        for (uint32_t k = 0; k < combat_->hardpoint_count(x) && k < ud.hardpoints.size(); ++k) {
            const tc::HardpointDesc& hd = combat_->data().hardpoints[ud.hardpoints[k]];
            if (hd.projectile < 0 || static_cast<size_t>(hd.projectile) >= combat_->data().projectiles.size()) continue;
            if (hd.destroyable && !(combat_->hardpoint_health(x, k) > 0.0f)) continue;
            const objects::TypeIndex t = types.resolve_name(combat_->data().projectiles[static_cast<size_t>(hd.projectile)].name);
            if (t != objects::kNone) out.push_back(t);
        }
    }
    const objects::ObjectStore& st = l1_mode_->primary();
    const objects::ObjectHandle h = st.find(x);
    if (!st.alive(h)) return out;
    for (const objects::TypeIndex t : types.get_type_refs(st.type(h), "Projectile_Types"))
        if (t != objects::kNone) out.push_back(t);
    return out;
}

objects::TypeIndex TacticalBattle::current_projectile(objects::ObjectId id) const {
    if (!l1_mode_) return objects::kNone;
    const objects::ObjectStore& st = l1_mode_->primary();
    objects::ObjectHandle h = st.find(id);
    for (size_t c = 0; c < containers_.size(); ++c)
        if (containers_[c].id == id) {
            bool member = false;  // S8 t12: a team container answers for its first member; an empty team: nothing
            for (const Unit& u : units_)
                if (u.container == c && u.alive && !u.retreated) {
                    h = u.h;
                    member = true;
                    break;
                }
            if (!member) return objects::kNone;
        }
    if (!st.alive(h)) return objects::kNone;
    const auto refs = catalog_->types().get_type_refs(st.type(h), "Projectile_Types");
    if (refs.empty()) return objects::kNone;
    // S8 t12 (TACTICAL_COMBAT §3.1.1 "Lua Get_Current_Projectile_Type", 0x578470): Projectile_Types[obj +0x324] (the weapon
    // index: 0 at creation, SWAP_WEAPONS / Permanent_Weapon_Swap), with no WEAPON behaviour test; an index outside the list
    // returns nothing.
    const int32_t wi = combat_ && combat_->has_unit(st.id(h)) ? combat_->weapon_index(st.id(h)) : 0;
    return wi >= 0 && static_cast<size_t>(wi) < refs.size() ? refs[static_cast<size_t>(wi)] : objects::kNone;
}

bool TacticalBattle::opposite_sides_of_shield(Vec3 a, Vec3 b, int32_t player, bool friendly, bool shield_damage) const {
    if (space_ || !l1_mode_ || !shield_damage) return false;
    static const objects::BehaviorTypeId kBaseShield = objects::behavior_type_id("BASE_SHIELD");
    const objects::ObjectStore& st = l1_mode_->primary();
    const objects::GameObjectTypes& types = catalog_->types();
    for (const objects::ObjectHandle h : st.all_objects()) {  // the land store's object list order
        if (!st.alive(h) || kBaseShield == objects::kNoBehavior || !st.has_behavior(h, kBaseShield)) continue;
        const float r = types.get_float(st.type(h), "Base_Shield_Radius");
        if (!(r > 0.0f)) continue;
        if (player >= 0 && (!combat_->is_enemy(player, st.owner(h))) != friendly) continue;  // 0x39a2c0 [M]
        const Vec3 c = st.position(h);
        auto inside = [&](Vec3 p) {
            const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
            return dx * dx + dy * dy + dz * dz < r * r;
        };
        if (inside(a) != inside(b)) return true;
    }
    return false;
}

void TacticalBattle::l1_hash(StateHasher& h) const {
    l1_sw_.hash(h);
    l1_jumps_.hash(h);
    l1_engines_.hash(h);
    h.boolean(l1_planet_destroyed_);
    if (l1_planet_destroyed_ || l1_tsw_story_winner_ >= 0 || !l1_tsw_debris_.empty()) {  // TSW destruction (others keep bytes)
        h.i32(l1_tsw_story_winner_);
        h.u32(static_cast<uint32_t>(l1_tsw_debris_.size()));
        for (const L1TswDebris& d : l1_tsw_debris_) {
            h.i32(d.tsw);
            h.i32(d.type);
            h.f32(d.pos.x);
            h.f32(d.pos.y);
            h.f32(d.pos.z);
            h.f32(d.facing.x);
            h.f32(d.facing.y);
            h.f32(d.facing.z);
        }
    }
    l1_weather_.hash(h);
    h.u32(static_cast<uint32_t>(l1_bombard_.size()));
    for (const L1Bombard& r : l1_bombard_) {
        h.i32(r.player);
        h.u8(static_cast<uint8_t>((r.enabled ? 1 : 0) | (r.available ? 2 : 0) | (r.in_progress ? 4 : 0) | (r.disabled ? 8 : 0)));
        h.u32(r.ready);
        h.u32(r.next_shot);
        h.u32(r.cursor);
        h.i32(r.shot);
        h.i32(r.salvo);
    }
    h.u32(static_cast<uint32_t>(l1_runs_.size()));
    for (const L1BombingRun& r : l1_runs_) {
        h.i32(r.player);
        h.u8(static_cast<uint8_t>((r.enabled ? 1 : 0) | (r.available ? 2 : 0) | (r.in_progress ? 4 : 0) | (r.disabled ? 8 : 0)));
        h.u32(r.ready);
        h.u32(static_cast<uint32_t>(r.bombers.size()));
        for (const L1Bomber& b : r.bombers) {
            h.i32(b.id);
            h.f32(b.pos.x);
            h.u32(b.next_fire);
        }
    }
    h.u32(static_cast<uint32_t>(l1_in_asteroid_.size()));
    for (const objects::ObjectId id : l1_in_asteroid_) h.i32(id);
}

// ---- CampaignBattles helper: Space_Retreat_Unit_Increment_Wait_Frames per faction (FactionFiles.xml) ----
std::vector<int32_t> load_retreat_increments(const galactic::GalacticRules& rules) {
    std::vector<int32_t> out(rules.factions.size(), 0);  // [port] unset = 0 (the hook's documented default)
    for (const galactic::FactionRules& f : rules.factions) {  // every definition, list order
        const uint32_t fi = rules.find_faction(f.name);
        const std::string_view v = f.xml_text("Space_Retreat_Unit_Increment_Wait_Frames");
        if (fi < out.size() && !v.empty()) out[fi] = objects::c_atoi(v);
    }
    return out;
}

std::vector<float> load_retreat_flight_increments(const galactic::GalacticRules& rules) {
    std::vector<float> out(rules.factions.size(), 500.0f);  // [port] unset = 500 (the shipped value)
    for (const galactic::FactionRules& f : rules.factions) {  // every definition, list order
        const uint32_t fi = rules.find_faction(f.name);
        const std::string_view v = f.xml_text("Space_Retreat_Flight_Move_Increment");
        if (fi < out.size() && !v.empty()) out[fi] = static_cast<float>(objects::c_atof(v));
    }
    return out;
}

}  // namespace alamo::sim