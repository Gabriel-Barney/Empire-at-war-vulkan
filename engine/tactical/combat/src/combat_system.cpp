#include "alamo/tactical/combat/combat_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"

namespace alamo::tactical::combat {

namespace {

constexpr uint32_t kUnitRemoved = 1u << 31;  // internal: host removal (no death event)
constexpr float kRadToDeg = 57.295779513082320876f;
constexpr int32_t kBurningInterval = 30;  // OI abilities: BurningBehavior vtable +0x28 (TACTICAL_GAPS §1.8)
constexpr int32_t kAbilityRocketAttack = 11;    // S8 t08: UnitAbilityType ROCKET_ATTACK (0x39b950(obj, 11))
// S8 t08: Mod_Flag WEAPONS_ENABLED_FLAG (0x397f60(obj, 1); S8 t11: enum table 0x140a434a0 index 1 [H]).
constexpr uint8_t kFlagWeaponsEnabled = 1;
constexpr uint32_t kNoService = 0xFFFFFFFFu;    // S7 t15 A2: Unit::weapon_last of an older save, never serviced (= 0)
constexpr float kGarrisonGunHalfAngle = 60.0f;  // S8 t10: Spawn_Projectile garrison pick, |bone yaw - target yaw|
const PlayerInfo kDefaultPlayer{};

Vec3 normalize(Vec3 v) {
    const float l2 = dot(v, v);
    if (!(l2 > 0.0f)) return Vec3{1.0f, 0.0f, 0.0f};
    const float inv = 1.0f / dmath::sqrt(l2);
    return v * inv;
}

float wrap_deg(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

float abs_f(float v) { return v < 0.0f ? -v : v; }

}  // namespace

// ================================================================================================
// construction / host API
// ================================================================================================

CombatSystem::CombatSystem(const CombatData& data, CombatConfig config, std::vector<PlayerInfo> players)
    : data_(&data), cfg_(std::move(config)), players_(std::move(players)) {
    if (cfg_.fps == 0) cfg_.fps = kDefaultFps;
    if (cfg_.grain == 0) cfg_.grain = 1;
    const auto n = static_cast<uint32_t>(players_.size());
    enemy_.assign(static_cast<size_t>(n) * n, 0);
    for (uint32_t a = 0; a < n; ++a)
        for (uint32_t b = 0; b < n; ++b) enemy_[static_cast<size_t>(a) * n + b] = a != b ? 1 : 0;
    player_mods_.resize(n);
    sw_hp_enabled_.assign(n, 1);  // player tactical data +0x2f0 init (0x526890)
    for (ModifierSet& s : player_mods_) s.reserve(16);
    // OI tactical: Diminishing_Firepower natural cubic spline (TACTICAL_GAPS 3.3): points sorted by x (stable), second
    // derivatives with y'' = 0 at both ends (tridiagonal solve, float).
    std::vector<std::pair<float, float>> pts = cfg_.diminishing_firepower;
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t m = pts.size();
    dim_x_.resize(m);
    dim_y_.resize(m);
    dim_y2_.assign(m, 0.0f);
    for (size_t i = 0; i < m; ++i) {
        dim_x_[i] = pts[i].first;
        dim_y_[i] = pts[i].second;
    }
    if (m >= 3) {
        std::vector<float> u(m, 0.0f);
        for (size_t i = 1; i + 1 < m; ++i) {
            const float h0 = dim_x_[i] - dim_x_[i - 1], h1 = dim_x_[i + 1] - dim_x_[i], h2 = dim_x_[i + 1] - dim_x_[i - 1];
            if (!(h0 > 0.0f) || !(h1 > 0.0f)) continue;
            const float sig = h0 / h2;
            const float p = sig * dim_y2_[i - 1] + 2.0f;
            dim_y2_[i] = (sig - 1.0f) / p;
            const float d = (dim_y_[i + 1] - dim_y_[i]) / h1 - (dim_y_[i] - dim_y_[i - 1]) / h0;
            u[i] = (6.0f * d / h2 - sig * u[i - 1]) / p;
        }
        dim_y2_[m - 1] = 0.0f;
        for (size_t k = m - 1; k-- > 0;) dim_y2_[k] = dim_y2_[k] * dim_y2_[k + 1] + u[k];
        dim_y2_[0] = 0.0f;
    }
}

void apply_game_constants(CombatConfig& c, const data::GameConstants& gc) {
    c.low_health_threshold = gc.get_float("Health_Low_Percent_Threshold", c.low_health_threshold);
    c.short_range_cutoff = gc.get_float("Melee_Cutoff_Range", c.short_range_cutoff);
    c.opportunity_recharge_cutoff = gc.get_float("Hardpoint_Recharge_Cutoff_For_Opportunity_Fire", c.opportunity_recharge_cutoff);
    c.turn_multiplier_space = gc.get_float("Object_Max_Speed_Multiplier_Space", c.turn_multiplier_space);  // S9 t05
    c.turn_multiplier_land = gc.get_float("Object_Max_Speed_Multiplier_Land", c.turn_multiplier_land);
    // cu8 t03: the remaining data-backed knobs (FoC: only Base_Shield_Vulnerability_Modifier 2.0f differs from the
    // defaults; AITestGameConstants changes the shield / energy ones).
    c.base_shield_vulnerability = gc.get_float("Base_Shield_Vulnerability_Modifier", c.base_shield_vulnerability);
    c.hull_vs_hardpoints_constraint = gc.get_float("Hull_Vs_Hard_Points_Health_Constraint", c.hull_vs_hardpoints_constraint);
    c.under_construction_damage_multiplier = gc.get_float("Under_Construction_Damage_Multiplier", c.under_construction_damage_multiplier);
    c.depleted_shield_regen_cap = gc.get_float("Depleted_Shield_Regen_Cap", c.depleted_shield_regen_cap);
    c.depleted_shield_disable_time = gc.get_float("Depleted_Shield_Disable_Time", c.depleted_shield_disable_time);
    c.shield_recharge_interval_secs = gc.get_float("ShieldRechargeIntervalInSecs", c.shield_recharge_interval_secs);
    c.energy_recharge_interval_secs = gc.get_float("EnergyRechargeIntervalInSecs", c.energy_recharge_interval_secs);
    c.energy_to_shield_exchange_rate = gc.get_float("EnergyToShieldExchangeRate", c.energy_to_shield_exchange_rate);
    c.sensor_jamming_time = gc.get_float("Sensor_Jamming_Time", c.sensor_jamming_time);
    c.max_secs_for_ae_delayed_damage = gc.get_float("Max_Secs_For_AE_Delayed_Damage", c.max_secs_for_ae_delayed_damage);
    if (const auto v = gc.get("Diminishing_Firepower")) {
        std::vector<float> nums;
        std::string_view s = *v;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
            size_t j = i;
            while (j < s.size() && !(s[j] == ' ' || s[j] == ',' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) ++j;
            if (j > i) nums.push_back(static_cast<float>(std::strtod(std::string(s.substr(i, j - i)).c_str(), nullptr)));
            i = j;
        }
        c.diminishing_firepower.clear();
        for (size_t k = 0; k + 1 < nums.size(); k += 2) c.diminishing_firepower.emplace_back(nums[k], nums[k + 1]);
    }
}

void CombatSystem::set_enemy(int32_t a, int32_t b, bool enemy) {
    const auto n = static_cast<int32_t>(players_.size());
    if (a < 0 || b < 0 || a >= n || b >= n) return;
    enemy_[static_cast<size_t>(a) * static_cast<size_t>(n) + static_cast<size_t>(b)] = enemy ? 1 : 0;
}

bool CombatSystem::is_enemy(int32_t a, int32_t b) const {
    const auto n = static_cast<int32_t>(players_.size());
    if (a < 0 || b < 0 || a >= n || b >= n) return false;
    return enemy_[static_cast<size_t>(a) * static_cast<size_t>(n) + static_cast<size_t>(b)] != 0;
}

void CombatSystem::reserve(uint32_t units, uint32_t projectiles, uint32_t candidates_per_unit, uint32_t delayed) {
    units_.reserve(units);
    hps_.reserve(static_cast<size_t>(units) * 4);
    flush_hps_.reserve(static_cast<size_t>(units) * 4);
    cands_.reserve(units);
    cand_reserve_ = candidates_per_unit;
    for (auto& c : cands_) c.reserve(candidates_per_unit);
    projectiles_.reserve(projectiles);
    sweep_.reserve(projectiles);
    rocket_next_.reserve(projectiles);  // refixf t01
    delayed_.reserve(delayed);
    deaths_.reserve(units);
    hp_events_.reserve(static_cast<size_t>(units) * 4);
    smoke_events_.reserve(units);
    emitters_.reserve(units);
    damage_events_.reserve(std::max<size_t>(projectiles, static_cast<size_t>(units) * 4));
    hit_events_.reserve(projectiles);  // S10-int
    shot_events_.reserve(projectiles);  // OI fog
    effect_scratch_.reserve(units);     // OI abilities
    survivor_events_.reserve(16);
    attack_events_.reserve(units);  // s78f t08: at most one signal 0x1E per unit and tick
    leaders_.reserve(units);
    team_heads_.reserve(units);  // OI tactical
    team_members_.reserve(units);  // cu8f t04a
    land_teams_.reserve(units);  // s78h t06
    serial_ctx_.reserve(std::max<uint32_t>(candidates_per_unit, units));
}

const PlayerInfo& CombatSystem::player(int32_t p) const {
    if (p < 0 || p >= static_cast<int32_t>(players_.size())) return kDefaultPlayer;
    return players_[static_cast<size_t>(p)];
}

int32_t CombatSystem::find(ObjectId id) const {
    const auto it = std::lower_bound(units_.begin(), units_.end(), id,
                                     [](const Unit& u, ObjectId v) { return u.id < v; });
    if (it == units_.end() || it->id != id) return -1;
    return static_cast<int32_t>(it - units_.begin());
}

CombatSystem::Unit* CombatSystem::unit(ObjectId id) {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &units_[static_cast<size_t>(i)];
}

const CombatSystem::Unit* CombatSystem::unit(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &units_[static_cast<size_t>(i)];
}

bool CombatSystem::add_unit(const UnitSpawn& s, uint32_t frame, SimRng& rng) {
    if (s.id == kNoObject || s.desc >= data_->units.size()) return false;
    if (!units_.empty() && s.id <= units_.back().id) return false;
    const UnitDesc& ud = data_->units[s.desc];
    Unit u;
    u.id = s.id;
    u.desc = s.desc;
    u.owner = s.owner;
    u.team = s.team;
    u.team_leader = s.team_leader ? 1 : 0;
    u.damage_team = s.team != 0 ? s.team : s.damage_team;  // s78g t06
    u.damage_parent = s.damage_parent;
    u.flags = s.flags & ~(kUnitDead | kUnitRemoved);
    if (u.flags & kUnitInfected) u.infect_attached = 1;  // refix t02: spawned infected = the behaviour, no parameters
    u.pos = s.position;
    u.last_pos = s.position;
    u.prev_pos = s.position;  // OI tactical
    u.facing = s.facing;
    u.lob_tpitch = ud.turret_rest_pitch;  // s78f t04: the TURRET starts at Turret_Rest_Angle
    u.lob_tyaw = ud.turret_rest_yaw;
    u.next_scan = frame;  // first scan due at once [L]
    u.mods.reserve(4);  // host / ability inserts stay allocation-free for the usual few entries
    u.hp_begin = static_cast<uint32_t>(hps_.size());
    u.hp_count = static_cast<uint32_t>(ud.hardpoints.size());
    for (uint32_t hd_index : ud.hardpoints) {
        Hardpoint h;
        h.desc = hd_index;
        const HardpointDesc& hd = data_->hardpoints[hd_index];
        // HardPoint ctor: first cooldown Get_Int(0, int(FPS x Fire_Max_Recharge_Seconds)), list order [H]
        // (no draw when the span is 0, e.g. non-weapon hardpoints).
        h.cooldown = hp_first_cooldown(hd.max_recharge, cfg_.fps, rng);
        h.pulses = hd.pulse_count;
        hps_.push_back(h);
    }
    units_.push_back(std::move(u));
    cands_.emplace_back();
    cands_.back().reserve(cand_reserve_);
    Unit& nu = units_.back();
    refresh_unit_cache(nu);
    nu.health = unit_max_hull(nu) * s.health_fraction;
    nu.shield = unit_max_shield(nu);
    for (uint32_t i = 0; i < nu.hp_count; ++i) {
        Hardpoint& h = hps_[nu.hp_begin + i];
        h.health = hp_max(nu, h);
    }
    nu.shield_regen_countdown = std::max<int32_t>(
        1, shield_regen_interval_frames(cfg_.fps, mult(nu, ModKind::ShieldRegenInterval), cfg_.shield_recharge_interval_secs));
    if (has_energy_pool(nu)) {  // S9: full pool at spawn [L]
        nu.energy = unit_max_energy(nu);
        nu.energy_countdown = std::max<int32_t>(
            1, shield_regen_interval_frames(cfg_.fps, mult(nu, ModKind::EnergyRegenInterval), cfg_.energy_recharge_interval_secs));
    }
    // Missile-shield emitters (TACTICAL_GAPS §1.7 [H]): creation registers once when either of the first two
    // Unit_Abilities_Data entries is MISSILE_SHIELD (32) and once more with a Passive_Missile_Shield_Radius; the
    // ability system sets the qualifying radius (passive, 32 active, jamming).
    nu.emitter_radius = ud.passive_missile_shield_radius > 0.0f ? ud.passive_missile_shield_radius : 0.0f;
    {
        bool ms = false;
        for (int slot = 0; slot < 2; ++slot)
            if (const UnitAbilityDesc* a = ability(nu, slot)) ms = ms || a->ability_type == 32;
        nu.emitter_entries = (ms ? 1u : 0u) + (ud.passive_missile_shield_radius > 0.0f ? 1u : 0u);
    }
    if (ud.weapon_projectile >= 0) {
        // S8 t12 (TACTICAL_COMBAT §3.1.1 "First service after spawn" / "Cooldowns"): the attach vf9 0x54c210 sets the initial
        // delay TD +0x24 = Get_Int(0, int(FPS x Projectile_Fire_Recharge_Seconds)), one synced draw (was a weapon_reset
        // stand-in [L]). [M] The pulse count before the first shot: Projectile_Fire_Pulse_Count (the step-7 reset value).
        nu.weapon_cooldown = hp_first_cooldown(ud.weapon_recharge, cfg_.fps, rng);
        nu.weapon_pulses = ud.weapon_pulse_count;
    }
    return true;
}

bool CombatSystem::remove_unit(ObjectId id) {
    Unit* u = unit(id);
    if (!u) return false;
    u->flags |= kUnitDead | kUnitRemoved;
    flush();
    return true;
}

void CombatSystem::set_position(ObjectId id, Vec3 p) {
    if (Unit* u = unit(id)) u->pos = p;
}
void CombatSystem::set_facing(ObjectId id, Vec3 f) {
    if (Unit* u = unit(id)) u->facing = f;
}
bool CombatSystem::set_owner(ObjectId id, int32_t owner) {
    // S10-int: Change_Owner (Lua / story) on a live unit; the player-set modifier cache is rebuilt.
    Unit* u = unit(id);
    if (!live(u)) return false;
    u->owner = owner;
    u->mod_valid = 0;
    return true;
}

void CombatSystem::set_flag(ObjectId id, uint32_t flag, bool on) {
    Unit* u = unit(id);
    if (!u) return;
    flag &= ~(kUnitDead | kUnitRemoved);
    if (flag & kUnitInfected) {  // refix t02: the flag mirrors the INFECTION behaviour (attach + active / suspended)
        flag &= ~kUnitInfected;
        if (on) {
            u->infect_attached = 1;
            u->infect_suspend = 0;
            u->flags |= kUnitInfected;
        } else if (u->infect_attached) {
            u->infect_suspend = std::max<uint32_t>(u->infect_suspend, 1);
            u->flags &= ~kUnitInfected;
        }
    }
    if (on) {
        u->flags |= flag;
    } else {
        u->flags &= ~flag;
    }
}
void CombatSystem::set_ability_active(ObjectId id, int slot, bool on) {
    Unit* u = unit(id);
    if (!u || slot < 0 || slot > 1) return;
    u->ability_active[static_cast<size_t>(slot)] = on ? 1 : 0;
    refresh_unit_cache(*u);
}
bool CombatSystem::ability_active(ObjectId id, int slot) const {
    const Unit* u = unit(id);
    if (!u || slot < 0 || slot > 1) return false;
    return u->ability_active[static_cast<size_t>(slot)] != 0;
}
void CombatSystem::order_attack(ObjectId attacker, ObjectId target, int32_t hardpoint) {
    Unit* u = unit(attacker);
    if (!u) return;
    auto order = [&](Unit& w) {
        w.ordered_target = target;
        w.ordered_hp = hardpoint;
        w.locked = 0;
        w.aim_target = kNoObject;  // refixf t06: the order re-aims the hardpoints
        w.aim_hp = -1;
        w.auto_target = kNoObject;  // an order drops the auto target (0x565e50)
        w.auto_hp = -1;
        w.target_score = 0.0f;
    };
    order(*u);
    // S8 t17 (FIGHTER_FLYING §4.10 B writer 2, TACTICAL_COMBAT §2.8 C step 3): the set path writes the squadron's team
    // target, so every member is ordered (index order = id order; the host's per-member orders make this idempotent).
    if (u->share_group != kNoObject)
        for (Unit& w : units_)
            if (&w != u && w.share_group == u->share_group && !(w.flags & kUnitDead)) order(w);
}
void CombatSystem::set_share_group(ObjectId id, ObjectId group) {
    if (Unit* u = unit(id)) u->share_group = group;
}
ObjectId CombatSystem::share_group(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->share_group : kNoObject;
}
void CombatSystem::share_target(const Unit& u, ObjectId t, float score) {
    // 0x406370(team, t, hp): every member (team list, index order [port: id order]) Set_Target(t, hp, order 0, lock 0)
    // [M: the scan's order flag 0x4061d0 read as 0]. [port] A member holding a live ordered target keeps it (the port keeps
    // the ordered slot apart from the scan's); its auto slot is not touched.
    const Unit* tu = unit(t);
    if (!live(tu)) return;
    for (Unit& w : units_) {
        if (&w == &u || w.share_group != u.share_group || (w.flags & kUnitDead)) continue;
        if (w.ordered_target != kNoObject && live(unit(w.ordered_target))) continue;
        if (w.auto_target != t) w.auto_hp = choose_target_hardpoint(w, *tu);
        w.auto_target = t;
        w.target_score = score;
        w.aim_target = kNoObject;
        w.aim_hp = -1;
        w.acquired = 1;
    }
}
void CombatSystem::clear_order(ObjectId attacker) {
    if (Unit* u = unit(attacker)) {
        u->ordered_target = kNoObject;
        u->ordered_hp = -1;
        u->locked = 0;
    }
}
// ---- refixf t06: Attack_Move on an object (TACTICAL_COMBAT §2.8) ----
void CombatSystem::set_attack_goal(ObjectId attacker, ObjectId target, int32_t hardpoint) {
    if (Unit* u = unit(attacker)) {
        u->goal_target = target;
        u->goal_hp = target == kNoObject ? -1 : hardpoint;
    }
}
ObjectId CombatSystem::attack_goal(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->goal_target : kNoObject;
}
ObjectId CombatSystem::weapon_aim(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->aim_target : kNoObject;
}
bool CombatSystem::order_locked(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->ordered_target != kNoObject && u->locked != 0;
}
bool CombatSystem::ability_type_active(const Unit& u, int32_t type) const {
    // S8 t11 (TACTICAL_COMBAT §3.1.1 "ROCKET_ATTACK detection"): 0x39b950 reads the per-type slot filled from every
    // Unit_Abilities_Data entry; the host mirrors entries past slot 1 as kUnitRocketAttack.
    if (type == kAbilityRocketAttack && (u.flags & kUnitRocketAttack)) return true;
    for (int s = 0; s < 2; ++s)
        if (const UnitAbilityDesc* a = ability(u, s); a && u.ability_active[static_cast<size_t>(s)] && a->ability_type == type) return true;
    return false;
}
bool CombatSystem::order_attack_issue(ObjectId attacker, ObjectId target, int32_t hardpoint) {
    Unit* u = unit(attacker);
    const Unit* t = unit(target);
    if (!u || !live(t)) return false;
    // C step 1: refusals.
    if (!is_enemy(u->owner, t->owner)) return false;
    if ((t->flags & kUnitStealthed) && !desc_of(*u).target_stealth_units && u->owner != t->owner) return false;
    if (t->flags & kUnitGarrisoned) return false;
    // Members without TEAM / TARGETING get no Order_Attack (0x739200 step 3): a unit with no weapon at all [port].
    bool armed = desc_of(*u).weapon_projectile >= 0;
    for (uint32_t i = 0; i < u->hp_count && !armed; ++i) armed = is_weapon(data_->hardpoints[hps_[u->hp_begin + i].desc].kind);
    if (!armed && u->team == 0) return true;
    const int32_t hp = hardpoint >= 0 && static_cast<uint32_t>(hardpoint) < t->hp_count ? hardpoint : -1;
    const bool space = cfg_.mode == GameMode::Space;
    const bool weapon_path = !(space && ability_type_active(*u, 5));  // space unless BARRAGE; land: ordered = 1
    if (weapon_path && u->team == 0 && !ability_type_active(*u, 34)) {
        // C step 2: lock released (slot 32(0): a locked target stays the current target, unlocked), the weapon hardpoints
        // aim at the target; the unit's own target is untouched.
        if (u->ordered_target != kNoObject && u->locked) {
            u->auto_target = u->ordered_target;
            u->auto_hp = u->ordered_hp;
            u->target_score = 0.0f;
            u->ordered_target = kNoObject;
            u->ordered_hp = -1;
            u->locked = 0;
        }
        u->aim_target = t->id;
        u->aim_hp = hp;
        return true;
    }
    // C step 3: the set path - Set_Target(target, hp, ordered 1, lock 1).
    order_attack(attacker, target, hp);
    u->locked = 1;
    return true;
}
float CombatSystem::base_attack_range_of(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? base_attack_range(*u) : 0.0f;
}
float CombatSystem::min_attack_distance(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? desc_of(*u).min_attack_distance : 0.0f;
}
bool CombatSystem::inside_min_range(ObjectId attacker, ObjectId target) const {
    const Unit* a = unit(attacker);
    const Unit* t = unit(target);
    if (!a || !t) return false;
    return min_range_hit(*t, a->pos, desc_of(*a).min_attack_distance);  // s78n t02: slot 46, not slot 43's E
}
bool CombatSystem::min_range_geometry_of(ObjectId target, Vec3 from, float& D, float& E) const {
    D = 0.0f;
    E = 0.0f;
    const Unit* t = unit(target);
    if (!t) return false;
    min_range_geometry(*t, from, D, E);
    return true;
}
bool CombatSystem::range_geometry_of(ObjectId target, Vec3 from, float& d, float& E) const {
    d = 0.0f;
    E = 0.0f;
    const Unit* t = unit(target);
    if (!t) return false;
    range_geometry(*t, from, d, E);
    return true;
}
bool CombatSystem::has_grenade_weapon(ObjectId id) const {
    const Unit* u = unit(id);
    return u && grenade_weapon(*u) != nullptr;
}
bool CombatSystem::weapon_lob_reaches(ObjectId id, Vec3 from, Vec3 to) const {
    // s78n t01: as lob_in_range (one pass at Max_Speed decides the yes / no, LOB_SOLVER §2.3).
    const Unit* u = unit(id);
    const ProjectileDesc* gw = u ? grenade_weapon(*u) : nullptr;
    if (!gw) return false;
    LobSolution s;
    return lob_solve(from, to, gw->max_speed, lob_g(*gw), false, s);
}
bool CombatSystem::melee_attacker(ObjectId id) const {
    const Unit* u = unit(id);
    return u && desc_of(*u).max_attack_distance <= cfg_.short_range_cutoff;
}
ObjectId CombatSystem::ordered_target(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->ordered_target : kNoObject;
}
int32_t CombatSystem::ordered_hardpoint(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? u->ordered_hp : -1;
}
ModifierSet* CombatSystem::unit_modifiers(ObjectId id) {
    Unit* u = unit(id);
    return u ? &u->mods : nullptr;
}
ModifierSet& CombatSystem::player_modifiers(int32_t p) { return player_mods_.at(static_cast<size_t>(p)); }

void CombatSystem::set_health(ObjectId id, float h) {
    if (Unit* u = unit(id)) u->health = h;
}
void CombatSystem::set_shield(ObjectId id, float s) {
    if (Unit* u = unit(id)) u->shield = s;
}

// ================================================================================================
// modifiers
// ================================================================================================

const UnitAbilityDesc* CombatSystem::ability(const Unit& u, int slot) const {
    const int32_t a = desc_of(u).abilities[static_cast<size_t>(slot)];
    if (a < 0 || static_cast<size_t>(a) >= data_->abilities.size()) return nullptr;
    return &data_->abilities[static_cast<size_t>(a)];
}

void CombatSystem::refresh_unit_cache(Unit& u) {
    const ModifierSet* ps = (u.owner >= 0 && static_cast<size_t>(u.owner) < player_mods_.size())
                                ? &player_mods_[static_cast<size_t>(u.owner)]
                                : nullptr;
    reduce_all(ps, &u.mods, default_list_clamps(), u.mod_r);
    u.mod_ver = u.mods.version();
    u.player_ver = ps ? ps->version() : 0;
    u.mod_valid = 1;
    const UnitAbilityDesc* ab[2] = {ability(u, 0), ability(u, 1)};
    const bool act[2] = {u.ability_active[0] != 0, u.ability_active[1] != 0};
    for (uint32_t k = 0; k < kModKindCount; ++k) u.own_mult[k] = ability_product(ab, act, static_cast<ModKind>(k + 1));
    u.mult = u.own_mult;
}

void CombatSystem::ensure_cache(Unit& u) {
    const ModifierSet* ps = (u.owner >= 0 && static_cast<size_t>(u.owner) < player_mods_.size())
                                ? &player_mods_[static_cast<size_t>(u.owner)]
                                : nullptr;
    if (u.mod_valid && u.mod_ver == u.mods.version() && u.player_ver == (ps ? ps->version() : 0)) return;
    reduce_all(ps, &u.mods, default_list_clamps(), u.mod_r);
    u.mod_ver = u.mods.version();
    u.player_ver = ps ? ps->version() : 0;
    u.mod_valid = 1;
}

float CombatSystem::modifier(ObjectId id, ModList list) {
    Unit* u = unit(id);
    if (!u) return 0.0f;
    ensure_cache(*u);
    return mod_r(*u, list);
}

float CombatSystem::mod_multiplier(ObjectId id, ModKind kind) {
    const Unit* u = unit(id);
    return u ? mult(*u, kind) : 1.0f;
}

// ================================================================================================
// derived stats
// ================================================================================================

float CombatSystem::unit_max_hull(const Unit& u) const {
    return max_hull(desc_of(u).tactical_health, cfg_.object_max_health_multiplier, player(u.owner).faction_health,
                    mod_r(u, ModList::MaxHealth));
}
float CombatSystem::unit_max_shield(const Unit& u) const {
    return max_shield(desc_of(u).shield_points, player(u.owner).faction_shield, mod_r(u, ModList::MaxShield));
}
float CombatSystem::hp_max(const Unit& u, const Hardpoint& h) const {
    return hardpoint_max_health(data_->hardpoints[h.desc].health, cfg_.hardpoint_health_multiplier,
                                mod_r(u, ModList::MaxHealth));
}

float CombatSystem::hp_fraction(const Unit& u) const {
    float cur = 0.0f, mx = 0.0f;
    bool any = false, any_live = false;
    for (uint32_t i = 0; i < u.hp_count; ++i) {
        const Hardpoint& h = hps_[u.hp_begin + i];
        if (!data_->hardpoints[h.desc].destroyable) continue;
        any = true;
        mx = mx + hp_max(u, h);
        if (!h.destroyed && h.health > 0.0f) {
            cur = cur + h.health;
            any_live = true;
        }
    }
    if (!any) return 1.0f;  // [L] no destroyable hardpoints
    if (!any_live || !(mx > 0.0f)) return 0.0f;
    return cur / mx;
}

bool CombatSystem::hp_out(const Hardpoint& h) const {
    // HARDPOINT_DISABLED.md §5: every gate tests (Is_Destroyable && health <= 0) || disabled.
    return h.destroyed || h.disabled || (data_->hardpoints[h.desc].destroyable && !(h.health > 0.0f));
}

bool CombatSystem::hp_dead(const Hardpoint& h) const {
    return data_->hardpoints[h.desc].destroyable && (h.destroyed || !(h.health > 0.0f));
}

int32_t CombatSystem::mesh_hardpoint(const Unit& v, Vec3 impact) const {
    // TACTICAL_COMBAT §2.9 Q1 steps 1 / 4 (port difference 1) [port]: retail names the render sub-object the precise model
    // test struck and damages the hardpoint whose Collision_Mesh it is (refixf t07: hardpoint_by_mesh, when the host's
    // precise test supplied the hit). Without a precise test (no model data / synthetic runs): the closest
    // (3D, first in list order on ties) hardpoint that is not (destroyable and dead) stands in, when its bone is within
    // CombatConfig::hardpoint_mesh_radius of the impact point; else no name (hull). A non-destroyable match routes to
    // the hull in take_damage, as retail does for a non-destroyable mesh.
    int32_t best = -1;
    float best_d2 = 0.0f;
    for (uint32_t i = 0; i < v.hp_count; ++i) {
        const Hardpoint& h = hps_[v.hp_begin + i];
        if (hp_dead(h)) continue;
        const Vec3 d = hp_world(v, data_->hardpoints[h.desc]) - impact;
        const float d2 = dot(d, d);
        if (best < 0 || d2 < best_d2) {
            best = static_cast<int32_t>(i);
            best_d2 = d2;
        }
    }
    const float r = cfg_.hardpoint_mesh_radius;
    return best >= 0 && best_d2 <= r * r ? best : -1;
}

bool CombatSystem::pick_aim_point(const Unit& u, const Hardpoint& h, const Unit& t, Vec3 ref, float range, SimRng& rng,
                                  Vec3& out) const {
    // TACTICAL_COMBAT §2.9 Q2 step 3, 0x383f70 [H unless tagged]: candidates accepted when their 3D distance from the
    // fire-bone reference <= Get_Fire_Range (+ the target radius when the shooter type has a weapon list +0xc48) and the
    // point passes the cone. S8 t27 (§3.1.1 "Hardpoint aim for comparison"): first the target type's Target_Bones in
    // list order (no draw); then the hardpoints from a synced random start (one Get_Int(0, n - 1); no draw without
    // hardpoints), wrapping, skipping destroyable dead ones; last the position + Ranged_Target_Z_Adjust (0x398440).
    // [port] Every unit has a "model instance" (retail: none -> position + z adjust, no draw).
    const UnitDesc& sd = desc_of(u);
    const float lim = range + ((sd.weapon_projectile >= 0 || !sd.weapon_projectiles.empty()) ? desc_of(t).radius : 0.0f);
    auto ok = [&](Vec3 p) { return distance3(ref, p) <= lim && in_cone(u, h, p); };
    for (const Vec3& b : desc_of(t).target_bones) {
        const Vec3 p = local_to_world(t, b);
        if (ok(p)) {
            out = p;
            return true;
        }
    }
    if (t.hp_count > 0) {
        const auto start = static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(t.hp_count) - 1));
        for (uint32_t k = 0; k < t.hp_count; ++k) {
            const Hardpoint& th = hps_[t.hp_begin + (start + k) % t.hp_count];
            if (hp_dead(th)) continue;
            const Vec3 p = hp_world(t, data_->hardpoints[th.desc]);
            if (ok(p)) {
                out = p;
                return true;
            }
        }
    }
    if (const Vec3 p = lob_target_point(t); ok(p)) {
        out = p;
        return true;
    }
    return false;
}

Vec3 CombatSystem::own_aim_point(const Unit& t, int32_t hp, SimRng& rng) const {
    // S8 t27 (TACTICAL_COMBAT §3.1.1 "Own-weapon aim point", 0x54a270) [H]:
    // 2. the shooter's target hardpoint (index != -1): its bone's world translation (0x396ce0); an index out of range or a
    //    hardpoint whose type flag +0x4c is clear [M: read as Is_Targetable] -> the bare position (no z adjust);
    // 3. else Target_Bones: one synced Get_Int(0, n - 1) (no draw for n = 1), that bone's world translation [port: rest
    //    pose turned by the facing; every unit has a model instance, so the position + z adjust fallback never runs];
    // 4. else the position + Ranged_Target_Z_Adjust (0x398440).
    if (hp >= 0) {
        if (static_cast<uint32_t>(hp) >= t.hp_count) return t.pos;
        const HardpointDesc& hd = data_->hardpoints[hps_[t.hp_begin + static_cast<uint32_t>(hp)].desc];
        return hd.targetable ? hp_world(t, hd) : t.pos;
    }
    const std::vector<Vec3>& bones = desc_of(t).target_bones;
    if (!bones.empty()) {
        const auto n = static_cast<int32_t>(bones.size());
        const int32_t i = n > 1 ? rng.range_int(0, n - 1) : 0;
        return local_to_world(t, bones[static_cast<size_t>(i)]);
    }
    return lob_target_point(t);
}

void CombatSystem::set_system_online(Unit& v, uint32_t index, bool online, bool destroyed, uint32_t frame) {
    // Set_System_Online(hp, online, destroyed) (0x384d00). `destroyed` is passed to the hooks (engines: permanent).
    const Hardpoint& h = hps_[v.hp_begin + index];
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    if (!online) {
        bool last = true;  // Count_Functioning_Hardpoints(owner, kind) == 0 (0x397c10)
        for (uint32_t i = 0; i < v.hp_count; ++i) {
            const Hardpoint& o = hps_[v.hp_begin + i];
            if (!hp_out(o) && data_->hardpoints[o.desc].kind == hd.kind) last = false;
        }
        if (hd.kind == HardpointKind::ShieldGenerator && last) {
            // 0x549550 -> 0x549810(beh, obj, 0): pool 0, depletion frame := now unless the depleted lockout is already
            // running, then the regen block +0x131 := 1 (HARDPOINT_DISABLED.md §9.2) [H].
            v.shield = 0.0f;
            if (!(v.shield_depleted &&
                  shield_lockout_active(frame, v.shield_depleted_frame, cfg_.fps, cfg_.depleted_shield_disable_time))) {
                v.shield_depleted = 1;
                v.shield_depleted_frame = frame;
            }
            v.regen_blocked = 1;
        }
        // Ability side (TURBO / SPOILER_LOCK / DEFEND / INTERDICT forced off, gravity well flag, engines, type 11).
        if (hooks_) hooks_->hardpoint_offline(v.id, index, hd.kind, last, destroyed, frame);
    } else {
        // Online branch (§9): only the shield generator has a combat-side write: +0x131 := 0 (0x549590). Regen then
        // resumes from the current pool at the next regen tick (rate x live / total generators, depleted cap within
        // the lockout). Weapons need nothing: their cooldown was frozen and continues. Fighter bay / tractor: none.
        if (hd.kind == HardpointKind::ShieldGenerator) v.regen_blocked = 0;
        if (hooks_) hooks_->hardpoint_online(v.id, index, hd.kind, frame);
    }
    // Tail, every call (HARDPOINT_DISABLED.md §4): allied players' special-weapon flag := online.
    if (hd.allows_special_weapon_use) {
        const auto n = static_cast<int32_t>(sw_hp_enabled_.size());
        for (int32_t p = 0; p < n; ++p)
            if (p == v.owner || !is_enemy(v.owner, p)) sw_hp_enabled_[static_cast<size_t>(p)] = online ? 1 : 0;
    }
}

float CombatSystem::min_health_fraction(const Unit& u) const {
    const float mh = unit_max_hull(u);
    const float hull = mh > 0.0f ? u.health / mh : 0.0f;
    const float hp = hp_fraction(u);
    return hp < hull ? hp : hull;
}

float CombatSystem::base_attack_range(const Unit& u) const {
    for (int s = 0; s < 2; ++s) {
        const UnitAbilityDesc* a = ability(u, s);
        if (a && u.ability_active[static_cast<size_t>(s)] && a->range_override > 0.0f) return a->range_override;
    }
    const UnitDesc& ud = desc_of(u);
    // 0x3749d0 step 2 (LOB_SOLVER §4): a grenade weapon's range is Max_Speed^2 / (lob mod x gravity) [port: 0 for G <= 0];
    // Targeting_Max_Attack_Distance is not read. Scan box / approach only - acceptance is the solver (slot 44).
    if (const ProjectileDesc* gw = grenade_weapon(u)) return lob_range(gw->max_speed, gw->grenade_gravity, gw->lob_mod);
    // Deployed / garrisoned multipliers and the transport contribution are not modelled [M].
    return ud.max_attack_distance;
}

float CombatSystem::attack_range(ObjectId id) {
    Unit* u = unit(id);
    if (!u) return 0.0f;
    ensure_cache(*u);
    return fire_range(base_attack_range(*u), mod_r(*u, ModList::FireRange));
}

Vec3 CombatSystem::local_to_world(const Unit& u, Vec3 l) const {
    float fx = u.facing.x, fy = u.facing.y;
    const float len2 = fx * fx + fy * fy;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    return Vec3{u.pos.x + (l.x * fx - l.y * fy), u.pos.y + (l.x * fy + l.y * fx), u.pos.z + l.z};
}

Vec3 CombatSystem::hp_world(const Unit& u, const HardpointDesc& hd) const { return local_to_world(u, hd.attach_offset); }

Vec3 CombatSystem::velocity(const Unit& u) const { return (u.pos - u.last_pos) * static_cast<float>(cfg_.fps); }

Vec3 CombatSystem::lead_point(const Unit& t, Vec3 muzzle, Vec3 aim0, float projectile_speed) const {
    // OI tactical: 0x399450 - three-point fit (circular lead, linear for straight tracks), formulas.hpp.
    return circular_lead(muzzle, t.prev_pos, t.last_pos, t.pos, aim0 - t.pos, projectile_speed, cfg_.fps);
}

const ProjectileDesc* CombatSystem::grenade_weapon(const Unit& u) const {
    if (desc_of(u).weapon_projectile < 0) return nullptr;
    const int32_t p = fired_weapon_projectile(u);
    if (p < 0 || static_cast<size_t>(p) >= data_->projectiles.size()) return nullptr;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(p)];
    return grenade(pd) ? &pd : nullptr;
}

bool CombatSystem::lob_in_range(const Unit& a, const ProjectileDesc& pd, const Unit& t) const {
    // Slot 44 0x567b40 (LOB_SOLVER §3.1): Lob_Solve(attacker position, candidate position + Ranged_Target_Z_Adjust). The
    // yes / no answer does not depend on Can_Lob_Slower (§2.3), so one pass at Max_Speed decides it.
    LobSolution s;
    return lob_solve(a.pos, lob_target_point(t), pd.max_speed, lob_g(pd), false, s);
}

Vec3 CombatSystem::lob_target_point(const Unit& t) const {
    return Vec3{t.pos.x, t.pos.y, t.pos.z + desc_of(t).ranged_target_z_adjust};  // 0x398440
}

const CombatSystem::Unit& CombatSystem::lob_member(const Unit& t, Vec3 from) const {
    const Unit* best = nullptr;
    float best_d2 = 0.0f;
    for (const Unit& m : units_) {  // ascending id: the first of equally near members wins
        if (&m == &t || !live(&m) || m.team == 0 || m.team != static_cast<uint32_t>(t.id)) continue;
        const float d2 = distance2_xy(from, m.pos);
        if (!best || d2 < best_d2) {
            best = &m;
            best_d2 = d2;
        }
    }
    return best ? *best : t;
}

Vec3 CombatSystem::target_point(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? lob_target_point(*u) : Vec3{};
}

float CombatSystem::target_radius(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? desc_of(*u).radius : 0.0f;
}

bool CombatSystem::lob_solution(uint32_t projectile, Vec3 from, Vec3 to, LobSolution* out) const {
    if (projectile >= data_->projectiles.size()) return false;
    const ProjectileDesc& pd = data_->projectiles[projectile];
    if (!grenade(pd)) return false;  // 0x54a6f0 returns false at once for any other category
    LobSolution s;
    const bool ok = lob_solve(from, to, pd.max_speed, lob_g(pd), pd.can_lob_slower, s);
    if (ok && out) *out = s;
    return ok;
}

float CombatSystem::target_extent(const Unit& t, Vec3 from) const {
    // TACTICAL_GAPS 3.1 slot 43 E (no LAND_OBSTACLE): |dot(target facing, line of sight)| > 0.7071 -> the X extent, else
    // the Y extent. A1j t04 (0x371e70 / 0x372040): per axis Custom_Hard_X/YExtent x Scale_Factor, else |model bbox
    // max.x / max.y| x Scale_Factor (model_x / y_extent); [port] the radius only when the host gave no model data. The
    // +10 for a moving team and the melee redirect apply to team-container targets, which the port never has
    // (candidates are units / members); the LAND_OBSTACLE walk is range_geometry.
    float ex = 0.0f, ey = 0.0f;
    if (!footprint_extents(t, ex, ey)) return ex;
    float dx = t.pos.x - from.x, dy = t.pos.y - from.y;
    const float l2 = dx * dx + dy * dy;
    if (!(l2 > 0.0f)) return 0.0f;  // zero offset counts as in range
    const float inv = 1.0f / dmath::sqrt(l2);
    dx = dx * inv;
    dy = dy * inv;
    float fx = t.facing.x, fy = t.facing.y;
    const float f2 = fx * fx + fy * fy;
    if (f2 > 0.0f) {
        const float fi = 1.0f / dmath::sqrt(f2);
        fx = fx * fi;
        fy = fy * fi;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    return abs_f(fx * dx + fy * dy) > 0.7071f ? ex : ey;
}

bool CombatSystem::footprint_extents(const Unit& t, float& ex, float& ey) const {
    const UnitDesc& td = desc_of(t);
    if (!(td.hard_x_extent > 0.0f) && !(td.hard_y_extent > 0.0f) && td.model_x_extent < 0.0f) {
        ex = ey = td.radius;
        return false;
    }
    ex = td.hard_x_extent > 0.0f ? td.hard_x_extent : td.model_x_extent >= 0.0f ? td.model_x_extent : td.radius;
    ey = td.hard_y_extent > 0.0f ? td.hard_y_extent : td.model_y_extent >= 0.0f ? td.model_y_extent : td.radius;
    return true;
}

void CombatSystem::min_range_geometry(const Unit& t, Vec3 from, float& D, float& E) const {
    // s78n t02 (TACTICAL_COMBAT §5.4.4 item 1, slot 46 0x5683a0 steps 2 / 6 / 7): D = the XY centre distance. With
    // LAND_OBSTACLE the aim walk's result is never used: E = 0. Else p = |cos th * (-dx) + sin th * (-dy)| with d = Q -
    // P_U NOT normalised (slot 43 normalises; slot 46 does not), so p is U's offset along X's facing in world units:
    // p > 0.7071 -> the X half-extent, else the Y half-extent. [port] the facing vector is normalised to (cos th, sin th);
    // a zero facing reads as +x. The model-less radius fallback is slot 43's (target_extent).
    D = distance_xy(from, t.pos);
    if (desc_of(t).land_obstacle) {
        E = 0.0f;
        return;
    }
    float ex = 0.0f, ey = 0.0f;
    if (!footprint_extents(t, ex, ey)) {
        E = ex;
        return;
    }
    float fx = t.facing.x, fy = t.facing.y;
    const float f2 = fx * fx + fy * fy;
    if (f2 > 0.0f) {
        const float fi = 1.0f / dmath::sqrt(f2);
        fx = fx * fi;
        fy = fy * fi;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    const float px = from.x - t.pos.x, py = from.y - t.pos.y;
    E = abs_f(fx * px + fy * py) > 0.7071f ? ex : ey;
}

const CombatSystem::Unit* CombatSystem::nearest_team_member(uint32_t team, Vec3 p) const {
    // s78n t02 (0x404fc0, §5.4.4 item 1): member array in order, 3-D squared distance, best replaced only by a strictly
    // smaller value (ties keep the earlier member). [port] the member order is team_members_ (ascending id, rebuilt in
    // phase A); members dead since then are skipped (retail does no alive test [M]).
    const Unit* best = nullptr;
    float bd = 0.0f;
    auto it = std::lower_bound(team_members_.begin(), team_members_.end(), std::make_pair(team, ObjectId{-1}));
    for (; it != team_members_.end() && it->first == team; ++it) {
        const Unit* m = unit(it->second);
        if (!live(m)) continue;
        const float dx = m->pos.x - p.x, dy = m->pos.y - p.y, dz = m->pos.z - p.z;
        const float d2 = (dx * dx + dy * dy) + dz * dz;
        if (!best || d2 < bd) {
            best = m;
            bd = d2;
        }
    }
    return best;
}

bool CombatSystem::min_range_hit(const Unit& t, Vec3 from, float r) const {
    // s78n t02 (TACTICAL_COMBAT §5.4.4 item 1): slot 46 for the weapon min range (no pending-ability override): step 4 -
    // a team-member target (its team is not itself) is retested against its team's member nearest P_U (its own position
    // and E); step 5 - r <= 0 -> false (d = 0 on all axes -> true follows from D <= r + E); steps 6 / 7. [port] step 1
    // (the melee redirect of a container target) is not run here: combat targets are units / members, and no melee
    // attacker has a Targeting_Min_Attack_Distance.
    if (!(r > 0.0f)) return false;
    const Unit* x = &t;
    if (t.team != 0 && t.team != static_cast<uint32_t>(t.id))
        if (const Unit* m = nearest_team_member(t.team, from)) x = m;
    float D = 0.0f, E = 0.0f;
    min_range_geometry(*x, from, D, E);
    return D <= r + E;
}

const CombatSystem::Unit* CombatSystem::team_head(const Unit& u) const {
    if (u.team == 0) return nullptr;
    // The container itself when it is a combat unit (team id = container object id), else the team_leader member.
    if (u.team <= static_cast<uint32_t>(kNoObject)) {
        const Unit* c = unit(static_cast<ObjectId>(u.team));
        if (live(c)) return c == &u ? nullptr : c;
    }
    const auto it = std::lower_bound(team_heads_.begin(), team_heads_.end(), std::make_pair(u.team, ObjectId{-1}));
    if (it == team_heads_.end() || it->first != u.team) return nullptr;
    const Unit* l = unit(it->second);
    return (live(l) && l != &u) ? l : nullptr;
}

CombatSystem::Unit* CombatSystem::team_head_of(uint32_t team) {
    // s78h t06: team_head() for the team itself (no member needed): the container when it is a live combat unit, else
    // the live team_leader member (team_heads_, kept current by hand_over_team).
    if (team == 0) return nullptr;
    if (team <= static_cast<uint32_t>(kNoObject)) {
        Unit* c = unit(static_cast<ObjectId>(team));
        if (live(c)) return c;
    }
    const auto it = std::lower_bound(team_heads_.begin(), team_heads_.end(), std::make_pair(team, ObjectId{-1}));
    if (it == team_heads_.end() || it->first != team) return nullptr;
    Unit* l = unit(it->second);
    return live(l) ? l : nullptr;
}

bool CombatSystem::targeting_idle(const Unit& u, uint32_t frame) const {
    // OI abilities: a stunned unit's targeting is suspended (0x647290); Requires_Base_Power units do not act without base
    // power (0x632270). S7 t14: the jetpack suspends TARGETING; S8 t25: targeting suspended.
    const bool idle = u.stun_end > frame || (desc_of(u).requires_base_power && !has_base_power(u.owner));
    return idle || (u.flags & kUnitWeaponsSuspended) || (u.flags & kUnitTargetingSuspended);
}

float CombatSystem::team_mod_multiplier(uint32_t team, ModKind kind) const {
    // s78h t03 (TACTICAL_COMBAT §4.4 "Who the squad leader is"): 0x398010 on the TEAM container. Members = every live unit
    // whose container id (damage_team: team on land, the squadron in space) is `team`, in units_ (ascending id) order.
    const auto k = static_cast<uint32_t>(kind) - 1u;
    if (team == 0 || k >= kModKindCount) return 1.0f;
    float own = 1.0f;
    if (team <= static_cast<uint32_t>(kNoObject)) {
        const Unit* c = unit(static_cast<ObjectId>(team));
        if (live(c)) own = c->own_mult[k];
    }
    std::vector<float> members;
    for (const Unit& m : units_)
        if (m.damage_team == team && static_cast<uint32_t>(m.id) != team && !(m.flags & kUnitDead)) members.push_back(m.own_mult[k]);
    return squad_multiplier(own, members);
}

void CombatSystem::hand_over_team(const Unit& d) {
    // s78f t07 / s78g t06: a dead team_leader hands the role to its team's first live member (ascending id), so the team
    // keeps its head (TeamTargeting copy, Mod_Multiplier squad rule, team scoring) while any member lives. [port] The
    // original's head is the container, which outlives its members (TACTICAL_COMBAT §3.1.1 "Team targeting copies":
    // TeamTargeting runs on the container; A1_READINGS §29.6: a team counts until the container dies). So the hand-over
    // happens at the death itself (no headless rest of the tick), and the heir takes the container's targeting record
    // (target TD +8 / +0x10, ordered target and lock, state TD +0x18, scan timer, attack goal), which a member's death
    // does not touch in the original.
    if (d.team == 0 || !d.team_leader || !(d.flags & kUnitDead)) return;
    Unit* heir = nullptr;
    for (Unit& m : units_) {
        if ((m.flags & kUnitDead) || m.team != d.team) continue;
        if (m.team_leader) return;  // led already
        if (!heir) heir = &m;
    }
    if (!heir) return;
    heir->team_leader = 1;
    heir->auto_target = d.auto_target;
    heir->auto_hp = d.auto_hp;
    heir->target_score = d.target_score;
    heir->ordered_target = d.ordered_target;
    heir->ordered_hp = d.ordered_hp;
    heir->locked = d.locked;
    heir->goal_target = d.goal_target;
    heir->goal_hp = d.goal_hp;
    heir->tstate = d.tstate;
    heir->ttarget = d.ttarget;
    heir->next_scan = d.next_scan;
    // The heir's phase-C flags and candidates were computed for its member record (a member never advances next_scan, so it
    // always read as due with no target to keep). Inside a tick, re-run its gather on the copied record, so a phase-D
    // hand-over services the heir as the container would: the keep test on the team's target, the team's scan timer, and
    // the candidates when a scan is due. [port] Re-gathering (not just clearing the flags) also keeps a due team scan from
    // slipping a tick when the member's own gather returned early. Between ticks phase C recomputes them anyway.
    if (trees_) {
        const int32_t hi = find(heir->id);
        if (hi >= 0) {
            QueryContext* c = acquire_ctx();
            gather(static_cast<uint32_t>(hi), *c);
            release_ctx(c);
        }
    } else {
        heir->needs_scan = heir->scan_due = 0;
    }
    // team_head() sees the heir at once (the rest of this tick and host reads between ticks); phase A rebuilds the lists.
    const auto it = std::lower_bound(team_heads_.begin(), team_heads_.end(), std::make_pair(d.team, ObjectId{-1}));
    if (it != team_heads_.end() && it->first == d.team) it->second = heir->id;
    if (cfg_.mode == GameMode::Land && std::binary_search(land_heads_.begin(), land_heads_.end(), d.id)) {
        const auto at = std::lower_bound(land_heads_.begin(), land_heads_.end(), heir->id);
        if (at == land_heads_.end() || *at != heir->id) land_heads_.insert(at, heir->id);
    }
}

void CombatSystem::track_damage(Unit& v, float amount, uint32_t frame) {
    const uint32_t sec = frame / cfg_.fps;
    const uint32_t b = sec % 5u;
    if (v.dmg_second[b] != sec) {
        v.dmg_second[b] = sec;
        v.dmg_bucket[b] = 0.0f;
    }
    v.dmg_bucket[b] = v.dmg_bucket[b] + amount;
}

float CombatSystem::damage_rate(const Unit& v, uint32_t frame) const {
    const uint32_t sec = frame / cfg_.fps;
    float sum = 0.0f;
    for (uint32_t b = 0; b < 5; ++b)
        if (v.dmg_bucket[b] != 0.0f && v.dmg_second[b] + 5u > sec) sum = sum + v.dmg_bucket[b];
    return sum / 5.0f;
}

void CombatSystem::rebuild_team_members() {
    team_members_.clear();
    for (const Unit& m : units_)
        if (m.team != 0 && !(m.flags & kUnitDead)) team_members_.emplace_back(m.team, m.id);
    std::sort(team_members_.begin(), team_members_.end());
}

const CombatSystem::Unit* CombatSystem::first_team_member(uint32_t team) const {
    auto it = std::lower_bound(team_members_.begin(), team_members_.end(), std::make_pair(team, ObjectId{-1}));
    for (; it != team_members_.end() && it->first == team; ++it)
        if (const Unit* m = unit(it->second); live(m) && m->team == team) return m;
    return nullptr;
}

const CombatSystem::Unit* CombatSystem::stickiness_type_unit(const Unit& u) const {
    // cu8f t04a (CU8F_T04_T06_RE §1.1 / §1.2) [H]: an owner with TEAM reads the type of the first member of its team data,
    // any other owner its own type. [port] A TEAM owner is a container that is itself a combat unit (live units carry its
    // id as their team) or, on land, the team_leader member standing in for a container that is not a combat unit (it
    // runs the team's TeamTargeting, land_team_head); "first member" = the team's first live unit in ascending id.
    if (u.id >= 0) {
        const auto key = static_cast<uint32_t>(u.id);
        bool team = false;
        auto it = std::lower_bound(team_members_.begin(), team_members_.end(), std::make_pair(key, ObjectId{-1}));
        for (; it != team_members_.end() && it->first == key; ++it) {
            const Unit* m = unit(it->second);
            if (m == &u || !m || m->team != key) continue;
            team = true;
            if (live(m)) return m;
        }
        if (team) return nullptr;
    }
    if (cfg_.mode == GameMode::Land && u.team != 0 && u.team_leader && !team_head(u)) return first_team_member(u.team);
    return &u;
}

float CombatSystem::time_to_dead(const Unit& t, uint32_t frame) const {
    // cu8f t04a: 0x58ba40 [H] - health = the sum of max(0, health) of the destroyable hard points when the unit has any
    // (destroyed ones count, at <= 0), else the hull; + shield (0x396420); / the damage rate; rate 0 -> 1e18. [port]
    // Every combat unit has DAMAGE_TRACKING (damage_rate: the 5 s window).
    const float rate = damage_rate(t, frame);
    if (!(rate > 0.0f)) return 1.0e18f;
    float hp = 0.0f;
    bool any = false;
    for (uint32_t i = 0; i < t.hp_count; ++i) {
        const Hardpoint& h = hps_[t.hp_begin + i];
        if (!data_->hardpoints[h.desc].destroyable) continue;
        any = true;
        hp = hp + std::max(0.0f, h.health);
    }
    if (!any) hp = t.health;
    return (hp + t.shield) / rate;
}

bool CombatSystem::sticky(const Unit& u, const Unit& t, uint32_t frame) const {
    // cu8f t04a: Is_Target_Sticky 0x569000 (CU8F_T04_T06_RE §1.2) [H]. Threshold = the owner's TD +0x158 (spawn 0.0); only
    // a negative value is replaced by the type's Targeting_Stickiness_Time_Threshold, read now (a TEAM owner: its first
    // member's type; no member -> not sticky). Time to kill = the target's time-to-dead, or for a TEAM container target
    // the sum of its live members' (team order; none: 0). Sticky <=> time to kill <= threshold. Both halves are pure, so
    // the threshold is taken first [port: same result].
    float threshold = u.stickiness;
    if (threshold < 0.0f) {
        const Unit* src = stickiness_type_unit(u);
        if (!src) return false;
        threshold = desc_of(*src).stickiness_threshold;
    }
    return time_to_kill(t, frame) <= threshold;
}

float CombatSystem::time_to_kill(const Unit& t, uint32_t frame) const {
    float ttk = 0.0f;
    bool container = false;
    if (t.id >= 0) {  // [port] a container target exists only when the container is itself a combat unit
        const auto key = static_cast<uint32_t>(t.id);
        auto it = std::lower_bound(team_members_.begin(), team_members_.end(), std::make_pair(key, ObjectId{-1}));
        for (; it != team_members_.end() && it->first == key; ++it) {
            const Unit* m = unit(it->second);
            if (m == &t || !m || m->team != key) continue;
            container = true;
            if (live(m)) ttk = ttk + time_to_dead(*m, frame);
        }
    }
    return container ? ttk : time_to_dead(t, frame);
}

bool CombatSystem::target_sticky(ObjectId unit_id, ObjectId target) const {
    const Unit* u = unit(unit_id);
    const Unit* t = unit(target);
    return u && t && sticky(*u, *t, frame_);
}

float CombatSystem::time_to_kill(ObjectId target) const {
    const Unit* t = unit(target);
    return t ? time_to_kill(*t, frame_) : 1.0e18f;
}

// ================================================================================================
// targeting (section 2)
// ================================================================================================

bool CombatSystem::unit_can_attack(const Unit& u) const {
    if (desc_of(u).weapon_projectile >= 0) return true;
    for (uint32_t i = 0; i < u.hp_count; ++i) {
        const Hardpoint& h = hps_[u.hp_begin + i];
        if (!hp_out(h) && is_weapon(data_->hardpoints[h.desc].kind)) return true;
    }
    return false;
}

bool CombatSystem::in_cone(const Unit& a, const Hardpoint& h, Vec3 aim) const {
    // HP_In_Fire_Cone (0x383ba0), TACTICAL_COMBAT §2.9 Q2 step 5: not a turret: the point in the fire bone's world frame,
    // |yaw| (wrapped) <= Fire_Cone_Width / 2 and |pitch| <= Fire_Cone_Height / 2; turret: the point in the attachment-bone
    // frame, |yaw + 90| (normalised) <= Turret_Rotate_Extent_Degrees, no elevation limit. refixf t06 [port]: the bone
    // frames are the model's bind-pose bone axes (HardpointDesc::fire_* / attach_*) turned by the unit's yaw (the port's
    // units have no pitch / roll; animated bone orientations are not read); without a model frame the unit's facing (yaw
    // only) + fire_yaw_deg [L] stands in. A cone of >= 360 x >= 180 (or a turret extent >= 180) passes every point.
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    if (!hd.turret && hd.fire_cone_width >= 360.0f && hd.fire_cone_height >= 180.0f) return true;
    if (hd.turret && hd.turret_rotate_extent >= 180.0f) return true;
    float fx = a.facing.x, fy = a.facing.y;
    const float len2 = fx * fx + fy * fy;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    auto turn = [fx, fy](Vec3 l) { return Vec3{l.x * fx - l.y * fy, l.x * fy + l.y * fx, l.z}; };
    if (hd.turret ? hd.has_attach_frame : hd.has_fire_frame) {
        const Vec3 o = local_to_world(a, hd.turret ? hd.attach_offset : hd.bone_a);
        const Vec3 d = aim - o;
        const float lx = dot(d, turn(hd.turret ? hd.attach_x : hd.fire_x));
        const float ly = dot(d, turn(hd.turret ? hd.attach_y : hd.fire_y));
        if (hd.turret) return abs_f(wrap_deg(dmath::atan2(ly, lx) * kRadToDeg + 90.0f)) <= hd.turret_rotate_extent;
        const float lz = dot(d, turn(hd.fire_z));
        const float yaw = dmath::atan2(ly, lx) * kRadToDeg;
        const float pitch = dmath::atan2(lz, dmath::sqrt(lx * lx + ly * ly)) * kRadToDeg;
        return abs_f(yaw) <= hd.fire_cone_width * 0.5f && abs_f(pitch) <= hd.fire_cone_height * 0.5f;
    }
    const Vec3 o = hp_world(a, hd);
    const Vec3 d = aim - o;
    const float lx = d.x * fx + d.y * fy;
    const float ly = d.y * fx - d.x * fy;
    const float yaw = wrap_deg(dmath::atan2(ly, lx) * kRadToDeg - hd.fire_yaw_deg);
    if (hd.turret) return abs_f(yaw) <= hd.turret_rotate_extent;
    const float pitch = dmath::atan2(d.z, dmath::sqrt(lx * lx + ly * ly)) * kRadToDeg;
    return abs_f(yaw) <= hd.fire_cone_width * 0.5f && abs_f(pitch) <= hd.fire_cone_height * 0.5f;
}

void CombatSystem::range_geometry(const Unit& t, Vec3 from, float& d, float& E) const {
    // TACTICAL_GAPS 3.1 slot 43. With LAND_OBSTACLE (obj +0x346 != 0xFF): the aim point starts at the target, steps back
    // toward the attacker in 20-unit steps while its passability cell is occupied by the target (0x59efa0), snaps to
    // the cell grid (the cell centre), E = sqrt(200). [port] the walk stops once it has passed the attacker; a target
    // the host reports without a footprint (ObstacleCell::NoFootprint: not stamped) keeps the half-extent rule.
    if (desc_of(t).land_obstacle && obstacle_cell_) {
        float dx = from.x - t.pos.x, dy = from.y - t.pos.y;
        const float l2 = dx * dx + dy * dy;
        if (!(l2 > 0.0f)) {  // zero offset counts as in range
            d = 0.0f;
            E = 0.0f;
            return;
        }
        Vec3 aim = t.pos;
        Vec3 centre = aim;
        ObstacleCell cell = obstacle_cell_(t.id, aim, centre);
        if (cell != ObstacleCell::NoFootprint) {
            const float len = dmath::sqrt(l2);
            dx = dx / len;
            dy = dy / len;
            for (float walked = 0.0f; cell == ObstacleCell::Occupied && walked <= len; walked = walked + 20.0f) {
                aim.x = aim.x + dx * 20.0f;
                aim.y = aim.y + dy * 20.0f;
                cell = obstacle_cell_(t.id, aim, centre);  // writes the cell centre of the new aim point
            }
            d = distance_xy(from, centre);
            E = 14.142136f;  // sqrt(200)
            return;
        }
    }
    d = distance_xy(from, t.pos);
    E = target_extent(t, from);
}

bool CombatSystem::aimable(const Unit& a, const Unit& t) const {
    // Dry-run aim test 0x54a970 [M]: some live weapon hardpoint can aim at the target.
    for (uint32_t i = 0; i < a.hp_count; ++i) {
        const Hardpoint& h = hps_[a.hp_begin + i];
        if (hp_out(h) || !is_weapon(data_->hardpoints[h.desc].kind)) continue;
        if (in_cone(a, h, t.pos)) return true;
    }
    return false;
}

uint16_t CombatSystem::aim_point_count(const Unit& t) const {
    uint16_t n = 0;
    for (uint32_t i = 0; i < t.hp_count; ++i) {
        const Hardpoint& h = hps_[t.hp_begin + i];
        if (!h.destroyed && data_->hardpoints[h.desc].targetable) ++n;
    }
    return n;
}

bool CombatSystem::candidate_ok(const Unit& a, const Unit& t, float range) const {
    // Filters of section 2.3 (all must pass, so their order does not change the result; fog [4] is tested last
    // because it is the most expensive).
    if (t.flags & (kUnitSkipTargeting | kUnitDead | kUnitGarrisoned)) return false;  // 1, 3
    const UnitDesc& ad = desc_of(a);
    const UnitDesc& td = desc_of(t);
    if (!td.is_valid_target) return false;  // 2
    if (ad.max_attack_distance <= cfg_.short_range_cutoff && ad.space_layer && td.space_layer &&
        !(ad.space_layer & td.space_layer))
        return false;  // 5
    if (ad.attack_category_restrictions & td.category_mask) return false;  // 6
    // 7: the attacker OR ITS SQUAD LEADER (OI tactical team targeting) has a non-hardpoint weapon or a weapon
    // hardpoint allowed to fire at the target (S8 t08: Does_Weapon_State_Allow_Firing_At 0x384850 in full).
    const Unit* head = team_head(a);
    auto has_weapon = [&](const Unit& s) {
        if (desc_of(s).weapon_projectile >= 0) return true;
        for (uint32_t i = 0; i < s.hp_count; ++i)
            if (hp_state_allows(s, hps_[s.hp_begin + i], &t)) return true;
        return false;
    };
    if (!has_weapon(a) && !(head && has_weapon(*head))) return false;
    // 9: range acceptance (OI tactical, TACTICAL_GAPS 3.1): slot 44 "in max range" = XY distance <= R + E and NOT slot
    // 45 "inside min range" (d <= Targeting_Min_Attack_Distance + E, types with a min distance only); E = the target's
    // footprint half-extent along the line of sight. OR the target is the container's (squad head's) current target
    // (team targeting, filter 9 [H]). S8 t04 (LOB_SOLVER §3.1): a grenade weapon's slot 44 is the lob solver alone (no R,
    // E, FireRange modifier or LAND_OBSTACLE walk); slot 45 is unchanged. s78n t02: slot 45 is slot 46 in full
    // (min_range_hit: its own E, no aim walk, the nearest-member retest), not slot 43's d / E.
    const ProjectileDesc* gw = grenade_weapon(a);
    float d = 0.0f, E = 0.0f;
    if (!gw) range_geometry(t, a.pos, d, E);  // OI tactical: + the LAND_OBSTACLE aim walk
    const bool head_target = head && current_target(head->id) == t.id;
    const bool in_max = gw ? lob_in_range(a, *gw, t) : d <= range + E;
    // S8 t17 (FIGHTER_FLYING §4.10 A, last paragraph): beyond R, a formation unit also accepts c when 0x4160a0(F, u, c)
    // != 0 (the formation's response range).
    const auto responds = [&] {
        float rr = 0.0f;
        return (a.flags & kUnitScanQuery) && scan_range_ && scan_range_(a.id, t.id, range, rr) != 0;
    };
    if (!in_max && !head_target && !responds()) return false;
    if (!head_target && min_range_hit(t, a.pos, ad.min_attack_distance)) return false;
    // 10: stealth
    if ((t.flags & kUnitStealthed) && !ad.target_stealth_units && a.owner != t.owner) return false;
    // 4: fully fogged for the attacker's player (FOW_Object_Hidden 0x35f470 with force 1 - TACTICAL_GAPS 5.8: AI
    // players too). The obj +0x343 MARKER exemption (bombing-run markers) is not modelled: no marker objects yet.
    if (target_hidden(a.owner, t.id, true)) return false;
    return true;
}

float CombatSystem::score(const Unit& a, const Unit& t) const {
    const UnitDesc& ad = desc_of(a);
    const UnitDesc& td = desc_of(t);
    if (ad.attack_category_restrictions & td.category_mask) return -1.0f;
    // OI tactical team targeting (2.4 [H]): team members score with their leader's (container's) priority set.
    const Unit* head = team_head(a);
    // OI abilities: the Lua override (data pack +0x160) before the type's set.
    const Unit& src = head ? *head : a;
    const int32_t ps = src.priority_override >= 0 ? src.priority_override : desc_of(src).priority_set;
    if (ps < 0 || static_cast<size_t>(ps) >= data_->priority_sets.size()) return 1.0f;
    return priority_score(data_->priority_sets[static_cast<size_t>(ps)], td.object_type, td.category_mask, td.property_flags);
}

void CombatSystem::gather(uint32_t index, QueryContext& ctx) {
    Unit& u = units_[index];
    std::vector<Candidate>& out = cands_[index];
    out.clear();
    u.needs_scan = 0;
    u.scan_due = 0;
    if (u.flags & kUnitDead) return;
    if (u.ordered_target != kNoObject) {
        const Unit* o = unit(u.ordered_target);
        if (live(o) && is_enemy(u.owner, o->owner)) return;  // attacking an ordered target: no auto scan [M]
    }
    if (frame_ < u.next_scan) return;
    if (!unit_can_attack(u)) return;  // no TargetingBehavior work without a weapon
    u.scan_due = 1;
    const float range = fire_range(base_attack_range(u), mod_r(u, ModList::FireRange));
    // Current target kept when still valid and sticky or scored exactly 1.0 (section 2.1).
    if (const Unit* cur = unit(u.auto_target)) {
        const bool valid = live(cur) && is_enemy(u.owner, cur->owner) && candidate_ok(u, *cur, range) &&
                           score(u, *cur) != -1.0f;
        if (valid && (u.target_score == 1.0f || sticky(u, *cur, frame_))) return;
    }
    u.needs_scan = 1;
    // S8 t17: Get_Scan_Range 0x566f50 (FIGHTER_FLYING §4.10 A) - R + Rr while the unit's formation answers 0x4160a0(F, u,
    // none) != 0, else R ([port] the land "R with a TURRET slot, else 0" branch needs a land formation: none in the port).
    float half = range;
    if ((u.flags & kUnitScanQuery) && scan_range_) {
        float rr = 0.0f;
        if (scan_range_(u.id, kNoObject, range, rr) != 0 && rr > 0.0f) half = range + rr;
    }
    const Aabb box{{u.pos.x - half, u.pos.y - half, u.pos.z - half}, {u.pos.x + half, u.pos.y + half, u.pos.z + half}};
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_->player_count()));
    for (int32_t p = 0; p < np; ++p) {
        if (!is_enemy(u.owner, p) || !players_[static_cast<size_t>(p)].active) continue;
        for (EntityHandle h : trees_->player(static_cast<uint32_t>(p)).query_aabb(box, kSkipDead, ctx)) {
            const Unit* t = unit(static_cast<ObjectId>(h));
            if (!live(t) || t == &u) continue;
            if (!candidate_ok(u, *t, range)) continue;
            const float s = score(u, *t);
            if (s == -1.0f) continue;  // 8
            Candidate c;
            c.id = t->id;
            c.player = p;
            c.score = s;
            c.fraction = min_health_fraction(*t);
            c.dist2 = distance2_xy(u.pos, t->pos);
            c.aim_points = aim_point_count(*t);
            c.aimable = (u.hp_count && aimable(u, *t)) ? 1 : 0;
            out.push_back(c);
        }
    }
}

bool CombatSystem::lock_holds(const Unit& u, const Unit& t) const {
    // §2.8 B "while locked" [H]: dropped when the target is garrisoned, stealthed for an attacker without
    // Target_Stealth_Units, or hidden by fog for the attacker's player (0x35f470, force 1). Dying / dead: live().
    // [port: the +0x3b4 test is not modelled.]
    if (t.flags & kUnitGarrisoned) return false;
    if ((t.flags & kUnitStealthed) && !desc_of(u).target_stealth_units) return false;
    return !target_hidden(u.owner, t.id, true);
}

bool CombatSystem::promote_goal(Unit& u) {
    // §2.8 B steps 2-6 [H]: Formation_Attack_Target(F, &hp, 0) -> 0x5680b0 -> slot 44 && !slot 45 -> Set_Target(t, hp,
    // order 1, lock 1) and every weapon hardpoint HP_Set_Target (update_weapons pushes the current target each frame).
    const Unit* t = unit(u.goal_target);
    if (!live(t)) {  // the dead object's formation goal has become a position
        u.goal_target = kNoObject;
        u.goal_hp = -1;
        return false;
    }
    if (t == &u || !is_enemy(u.owner, t->owner)) return false;  // hostile to the unit's owner (relation table == 1)
    // 0x5680b0 (A1_READINGS §33.2.1, q6 t19): type half (TEAM / Collidable_By_Projectile_Living + Is_Valid_Target,
    // SPECIAL_WEAPON only on a DUMMY_STAR_BASE) from the desc; not garrisoned / hidden (+0x3a7), not dead (+0x3a0 0x40);
    // [port] no transports or hero clashes (+0x3b4) in battles. kUnitSkipTargeting (the port's untargetable marker) kept.
    if (t->flags & (kUnitSkipTargeting | kUnitDead | kUnitGarrisoned)) return false;
    if (!desc_of(*t).target_test_type) return false;
    float d = 0.0f, E = 0.0f;
    range_geometry(*t, u.pos, d, E);
    if (const ProjectileDesc* gw = grenade_weapon(u)) {
        if (!lob_in_range(u, *gw, *t)) return false;  // slot 44, grenade weapon (S8 t04: LOB_SOLVER §3.1)
    } else {
        const float range = fire_range(base_attack_range(u), mod_r(u, ModList::FireRange));
        if (!(d <= range + E)) return false;  // slot 44
    }
    if (min_range_hit(*t, u.pos, desc_of(u).min_attack_distance)) return false;  // slot 45 -> 46 (s78n t02)
    int32_t hp = -1;
    if (u.goal_hp >= 0 && static_cast<uint32_t>(u.goal_hp) < t->hp_count && !hp_dead(hps_[t->hp_begin + static_cast<uint32_t>(u.goal_hp)]))
        hp = u.goal_hp;  // no goal hardpoint: none pushed (§2.9 Q3; the space formation step fills it in)
    u.ordered_target = t->id;
    u.ordered_hp = hp;
    u.locked = 1;
    u.auto_target = kNoObject;  // replaces any auto target
    u.auto_hp = -1;
    u.target_score = 0.0f;
    u.aim_target = kNoObject;
    u.aim_hp = -1;
    return true;
}

void CombatSystem::service_space_hardpoint(Unit& u) {
    // TACTICAL_COMBAT §2.9 Q3 [H]: Service_Space (0x633ae0), after the keep / switch step, when TD +0x10 is null or a
    // dead destroyable hardpoint and the unit has a formation: the formation goal's hardpoint when it is not dead, else
    // Choose_Target_Hardpoint(target, unit). Service_Land has no such step. [port] "has a formation" = the unit has an
    // attack goal (the port's movement keeps no formation objects for space; group object orders set the goal).
    if (cfg_.mode != GameMode::Space || u.goal_target == kNoObject) return;
    const bool ord = u.ordered_target != kNoObject && live(unit(u.ordered_target));
    const Unit* t = unit(ord ? u.ordered_target : u.auto_target);
    if (!live(t) || t->hp_count == 0) return;
    int32_t& hp = ord ? u.ordered_hp : u.auto_hp;
    if (hp >= 0 && static_cast<uint32_t>(hp) < t->hp_count && !hp_dead(hps_[t->hp_begin + static_cast<uint32_t>(hp)])) return;
    if (t->id == u.goal_target && u.goal_hp >= 0 && static_cast<uint32_t>(u.goal_hp) < t->hp_count &&
        !hp_dead(hps_[t->hp_begin + static_cast<uint32_t>(u.goal_hp)])) {
        hp = u.goal_hp;
    } else {
        hp = choose_target_hardpoint(u, *t);
    }
}

void CombatSystem::decide(Unit& u, std::vector<Candidate>& cands, uint32_t frame, SimRng& rng) {
    decide_target(u, cands, frame, rng);
    service_space_hardpoint(u);
}

void CombatSystem::decide_target(Unit& u, std::vector<Candidate>& cands, uint32_t frame, SimRng& rng) {
    bool dropped = false;
    if (u.ordered_target != kNoObject) {
        const Unit* o = unit(u.ordered_target);
        if (live(o) && is_enemy(u.owner, o->owner) && (!u.locked || lock_holds(u, *o))) return;
        u.ordered_target = kNoObject;  // finished order cleared (0x565de0)
        u.ordered_hp = -1;
        u.locked = 0;
        dropped = true;
    }
    // refixf t06 (TACTICAL_COMBAT §2.8 B): the formation's attack goal becomes the locked target inside weapon range,
    // before the scan; a promotion skips the scan of this service. [port: not in the service that dropped a target.]
    if (!dropped && u.goal_target != kNoObject && promote_goal(u)) return;
    if (!u.scan_due) return;
    u.next_scan = next_scan_frame(frame, cfg_.fps, rng);  // synced draw 1
    ++stats_.scans;
    if (!u.needs_scan) return;
    const auto n = static_cast<int32_t>(player_count());
    const int32_t start = rng.range_int(0, n - 1);  // synced draw 2: random start player
    const float thr = cfg_.low_health_threshold;
    const bool has_hp = u.hp_count != 0;
    // s78g t04 (TACTICAL_COMBAT §2.4 correction, §2.10 item 4) [H]: Scan_Player 0x5662b0 starts each visited player with no
    // local best and takes that player's first passing candidate unconditionally; later candidates of the same player use
    // the replacement rule. 0x568a00 replaces its result with every non-null per-player return, so the pick is the local
    // best of the LAST player (cyclic order from the start player) that has a candidate. The early exit (current target
    // scoring exactly 1.0) fires only once that candidate has been accepted as the local best; the scan stops there.
    const Candidate* best = nullptr;
    for (int32_t k = 0; k < n; ++k) {
        const int32_t p = (start + k) % n;
        const Candidate* local = nullptr;
        for (const Candidate& c : cands) {
            if (c.player != p) continue;
            if (c.aim_points > 0) rng.range_int(0, static_cast<int32_t>(c.aim_points) - 1);  // HP_Choose_Aim_Point
            bool replace = true;
            if (local) {
                // Replacement rule [H]. Step 2 has NO equality guard (disassembly 0x5668f5): an equal or
                // WORSE score falls through to the aim and XY-distance tie-breaks.
                const bool cl = c.fraction <= thr, bl = local->fraction <= thr;
                if (cl != bl) {
                    replace = cl;
                } else if (c.score < local->score) {
                    replace = true;
                } else if (has_hp && c.aimable != local->aimable) {
                    replace = c.aimable != 0;
                } else {
                    replace = c.dist2 < local->dist2;
                }
            }
            if (!replace) continue;
            local = &c;
            if (c.id == u.auto_target && c.score == 1.0f) {  // early exit, after the acceptance
                best = local;
                goto done;
            }
        }
        if (local) best = local;
    }
done:
    if (best) {
        // S8 t17 (FIGHTER_FLYING §4.10 B, writer 1): a squadron member whose own target is empty when its due scan picks
        // sets the team target 0x406370 - every member's Set_Target. A member that had a target (keep / switch) sets only
        // its own.
        const bool team_pick = u.share_group != kNoObject && u.auto_target == kNoObject;
        // §2.9 Q3: Set_Target fixes the target hardpoint (Choose_Target_Hardpoint); the same target is not re-set.
        if (best->id != u.auto_target) {
            const Unit* bt = unit(best->id);
            u.auto_hp = bt ? choose_target_hardpoint(u, *bt) : -1;
        }
        u.auto_target = best->id;
        u.target_score = best->score;
        u.aim_target = kNoObject;  // refixf t06: Order_Attack(unit, pick, ordered 0) re-aims the hardpoints (§2.8 C)
        u.aim_hp = -1;
        u.acquired = 1;  // OI abilities: Issue_Auto_Target for the acquired target (the autofire driver, abilities)
        if (team_pick) share_target(u, best->id, best->score);
    } else {
        u.auto_target = kNoObject;
        u.auto_hp = -1;
        u.target_score = 0.0f;
    }
}

int32_t CombatSystem::closest_hardpoint(const Unit& t, Vec3 from, bool targetable_only) const {
    int32_t best = -1;
    float best_d2 = 0.0f;
    for (uint32_t i = 0; i < t.hp_count; ++i) {
        const Hardpoint& h = hps_[t.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (hp_dead(h) || (targetable_only && !hd.targetable)) continue;  // 0x394a80: not (destroyable and dead)
        const Vec3 d = hp_world(t, hd) - from;
        const float d2 = dot(d, d);
        if (best < 0 || d2 < best_d2) {
            best = static_cast<int32_t>(i);
            best_d2 = d2;
        }
    }
    return best;
}

int32_t CombatSystem::choose_target_hardpoint(const Unit& a, const Unit& t) const {
    // Choose_Target_Hardpoint (0x394610) [H]: with a priority set, rank = position of the hardpoint's TYPE in
    // Hard_Point_Priorities (excluded = -1: skipped [M]), else 1e18; lowest rank, ties closest 3D.
    if (t.hp_count == 0) return -1;
    const int32_t ps = a.priority_override >= 0 ? a.priority_override : desc_of(a).priority_set;  // OI abilities
    if (ps < 0 || static_cast<size_t>(ps) >= data_->priority_sets.size()) return closest_hardpoint(t, a.pos, true);
    const PrioritySet& set = data_->priority_sets[static_cast<size_t>(ps)];
    // §2.9 Q3: ties by the 3D distance from (shooter x, y, z + Ranged_Target_Z_Adjust).
    const Vec3 from{a.pos.x, a.pos.y, a.pos.z + desc_of(a).ranged_target_z_adjust};
    int32_t best = -1;
    float best_rank = 0.0f, best_d2 = 0.0f;
    for (uint32_t i = 0; i < t.hp_count; ++i) {
        const Hardpoint& h = hps_[t.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (!hd.destroyable || hp_dead(h)) continue;
        const float rank = hardpoint_rank(set, static_cast<int32_t>(hd.kind));  // refixf t06: by HardPointType
        if (rank < 0.0f) continue;
        const Vec3 d = hp_world(t, hd) - from;
        const float d2 = dot(d, d);
        if (best < 0 || rank < best_rank || (rank == best_rank && d2 < best_d2)) {
            best = static_cast<int32_t>(i);
            best_rank = rank;
            best_d2 = d2;
        }
    }
    return best;
}

// ================================================================================================
// weapons (section 3.1)
// ================================================================================================

float CombatSystem::weapon_delay(const Unit& u, const HardpointDesc* hd) const {
    // W = product over the active unit-ability slots of the hardpoint type's per-mode delay table,
    // x the unit-ability WEAPON_DELAY multiplier (kind 1).
    float w = 1.0f;
    if (hd) {
        for (int s = 0; s < 2; ++s) {
            const UnitAbilityDesc* a = ability(u, s);
            if (!a || !u.ability_active[static_cast<size_t>(s)]) continue;
            for (const WeaponDelayEntry& e : hd->weapon_delay)
                if (e.ability_type == a->ability_type) {
                    w = w * e.factor;
                    break;
                }
        }
    }
    return w * mult(u, ModKind::WeaponDelay);
}

float CombatSystem::fire_rate(const Unit& u) const {
    // F = FIRE_RATE multiplier (kind 9) x (1 + ModifierSet list 6).
    return mult(u, ModKind::FireRate) * (1.0f + mod_r(u, ModList::FireRate));
}

ObjectId CombatSystem::current_target(ObjectId id) const {
    const Unit* u = unit(id);
    if (!u) return kNoObject;
    if (u->ordered_target != kNoObject) {
        const Unit* o = unit(u->ordered_target);
        if (live(o)) return u->ordered_target;
    }
    // s78f t03 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): a ground team member keeps no target of its own (TD +8
    // null); its WEAPON reads the container's (the head's own order / scan target, not a copy).
    if (u->auto_target == kNoObject) {
        if (const Unit* h = land_team_head(*u)) {
            if (h->ordered_target != kNoObject && live(unit(h->ordered_target))) return h->ordered_target;
            return h->auto_target;
        }
    }
    return u->auto_target;
}

void CombatSystem::update_weapons(Unit& u, uint32_t frame, SimRng& rng, bool suspended) {
    ObjectId tgt = current_target(u.id);
    // refixf t06 (TACTICAL_COMBAT §2.8 C step 2): a path-A aim holds the weapon hardpoints until the next scan pick, a lock
    // or another order replaces it [port: kept while the aimed object is a live enemy].
    if (u.aim_target != kNoObject) {
        const Unit* a = unit(u.aim_target);
        if (!live(a) || !is_enemy(u.owner, a->owner)) {
            u.aim_target = kNoObject;
            u.aim_hp = -1;
        } else if (u.ordered_target == kNoObject) {
            tgt = u.aim_target;
        }
    }
    const Unit* t = unit(tgt);
    int32_t sub = -1;
    if (live(t)) {
        // refixf t06 (TACTICAL_COMBAT §2.9 Q2 step 1 / Q3): the hardpoint fixed when the target was set (TD +0x10: the
        // order's, the aim's or the scan's). Update_Weapon drops it when it is destroyable and dead; Fire_At then picks
        // the closest live targetable hardpoint per shot.
        const int32_t fixed = tgt == u.ordered_target ? u.ordered_hp : tgt == u.aim_target ? u.aim_hp : tgt == u.auto_target ? u.auto_hp : -1;
        if (fixed >= 0 && static_cast<uint32_t>(fixed) < t->hp_count && !hp_dead(hps_[t->hp_begin + static_cast<uint32_t>(fixed)]))
            sub = fixed;
    }
    const bool idle = !live(t);
    // S8 t08 (TACTICAL_COMBAT §3.1.1 "Cooldowns"): the WEAPON behaviour (own weapon) runs in the behaviour pass, before the
    // hardpoint updates; the two sources keep separate cooldowns. Its gate 0x54c910 (global switch included) is tested
    // inside, after the cooldown has elapsed.
    // S8 t12 (MOVEMENT §3.2): a locomotor swap suspends TARGETING / WEAPON / SPECIAL_WEAPON only - the own weapon is not
    // serviced and no target is pushed; the hardpoints keep counting down and firing at their last target.
    if (desc_of(u).weapon_projectile >= 0 && !suspended) fire_unit_weapon(u, frame, rng);
    // S8 t26: LOBBING_SUPERWEAPON's service follows WEAPON's (vanilla LandBehavior list order): a charge completed here
    // fires on the next WEAPON service.
    if (desc_of(u).lobbing_superweapon) lobbing_service(u, frame);
    const bool transition = deploy_transition(u);
    for (uint32_t i = 0; i < u.hp_count; ++i) {
        Hardpoint& h = hps_[u.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (!is_weapon(hd.kind)) continue;
        if (!suspended) {
            h.target = tgt;  // HP_Set_Target: owner's target + chosen sub-hardpoint
            h.target_hp = sub;
        }
        // Update_Weapon (0x387400): gates (S8 t08: none in a deploy transition), then the cooldown countdown, then the
        // weapon-state gate 0x384850 against the primary target (a refusal skips Fire_At and opportunity fire), Fire_At.
        if (!cfg_.weapons_enabled || hp_out(h) || transition) continue;
        h.cooldown -= 1;
        if (h.cooldown > 0) continue;
        const Unit* ht = unit(h.target);  // the primary target (+0x40)
        if (!hp_state_allows(u, h, live(ht) ? ht : nullptr)) continue;
        if (fire_hardpoint(u, h, frame, rng)) continue;
        // Opportunity fire (TACTICAL_COMBAT 2.6, S9): no shot at the owner's target.
        if (cfg_.opportunity_fire && opportunity_allowed(u, hd, idle)) opportunity_fire(u, h, frame, rng);
    }
}

bool CombatSystem::deployed_or_deploying(const Unit& u) const {
    // 0x39a390 (S9_DEATH_CLONE_SHIELDS §1.2): Deploys, a locomotor, and deploy pending / Deployed (kUnitDeploying). S8 t11
    // (TACTICAL_COMBAT §3.1.1 "Step 3 correction"): the walk locomotor's vf +0x120 is state == 0x19 only, so Deploying
    // (0x18) and Undeploying (0x1A) - kUnitDeployTransition - do not count (vanilla never sets the pending flag).
    const UnitDesc& ud = desc_of(u);
    return ud.deploys && ud.has_locomotor && (u.flags & kUnitDeploying) != 0 && (u.flags & kUnitDeployTransition) == 0;
}

bool CombatSystem::deploy_transition(const Unit& u) const {
    const UnitDesc& ud = desc_of(u);
    return ud.deploys && ud.has_locomotor && (u.flags & kUnitDeployTransition) != 0;
}

bool CombatSystem::unit_weapon_allowed(const Unit& u) const {
    // Own-weapon gate 0x54c910 (TACTICAL_COMBAT §3.1.1 with "Gate details"), steps 1-6 in order (step 7, the targeting
    // state, is in fire_unit_weapon: its refusal also restarts the burst): 1 the global weapons switch (the behaviour is
    // enabled: a suspended one is not serviced at all); 2 no deploy transition; 3 deployed (pending / state 0x19) ->
    // Fire_Weapon_When_Deployed, else _Undeployed; 4 ROCKET_ATTACK (11) active in any Unit_Abilities_Data entry ->
    // Fire_Weapon_When_In_Rocket_Attack_Mode, else _In_Normal_Attack_Mode; 5 0x397f60(obj, 1): WEAPONS_ENABLED_FLAG ANDed
    // over the active modes of entries 0 / 1 (default 1: vanilla never refuses); 6 locomotor state 0x1C (fighter Idle)
    // without Can_Fighter_Fire_When_Idle, or state 0x16 on a locomotor whose vf +0x240 is true (host flags).
    if (!cfg_.weapons_enabled || deploy_transition(u)) return false;
    const UnitDesc& ud = desc_of(u);
    if (!(deployed_or_deploying(u) ? ud.fire_weapon_when_deployed : ud.fire_weapon_when_undeployed)) return false;
    if (!(ability_type_active(u, kAbilityRocketAttack) ? ud.fire_weapon_when_rocket_attack : ud.fire_weapon_when_normal_attack))
        return false;
    const UnitAbilityDesc* ab[2] = {ability(u, 0), ability(u, 1)};
    const bool act[2] = {u.ability_active[0] != 0, u.ability_active[1] != 0};
    if (!ability_flag(ab, act, kFlagWeaponsEnabled)) return false;
    if ((u.flags & kUnitFighterIdle) && !ud.can_fighter_fire_when_idle) return false;
    return (u.flags & kUnitHoverRest) == 0;
}

bool CombatSystem::in_attack_range(const Unit& u, Vec3 from, const Unit& t) const {
    // Slot 44 && !slot 45 (TACTICAL_COMBAT §3.1.1 "Gate details" step 7, LOB_SOLVER §8 item 22): In_Max_Range (XY distance
    // <= R + E with the LAND_OBSTACLE aim walk; for a grenade weapon the lob solver alone, LOB_SOLVER §3.1) and not inside
    // Targeting_Min_Attack_Distance + E, from the targeting object's position (s78n t02: slot 46's own E, min_range_hit).
    const UnitDesc& ud = desc_of(u);
    const ProjectileDesc* gw = grenade_weapon(u);
    float d = 0.0f, E = 0.0f;
    if (!gw) range_geometry(t, from, d, E);
    if (gw) {
        LobSolution s;  // as lob_in_range (one pass at Max_Speed decides the yes / no)
        if (!lob_solve(from, lob_target_point(t), gw->max_speed, lob_g(*gw), false, s)) return false;
    } else {
        const float range = fire_range(base_attack_range(u), mod_r(u, ModList::FireRange));
        if (!(d <= range + E)) return false;
    }
    return !min_range_hit(t, from, ud.min_attack_distance);
}

bool CombatSystem::attacking(ObjectId id) const {
    const Unit* u = unit(id);
    if (!live(u)) return false;
    const ObjectId tid = current_target(id);
    const Unit* t = unit(tid);
    return live(t) && u->tstate == 4 && u->ttarget == tid;
}

uint8_t CombatSystem::targeting_state(ObjectId id) const {
    const Unit* u = unit(id);
    if (!u) return 0;
    if (u->tstate == 0) return 0;
    return u->ttarget == current_target(id) ? u->tstate : uint8_t{1};  // another target: Set_Target's state 1
}

bool CombatSystem::needs_turn(ObjectId id) const {
    const Unit* u = unit(id);
    if (!live(u)) return false;
    const Unit* t = unit(current_target(id));
    return live(t) && needs_turn_to(*u, *t);
}

float CombatSystem::turn_step(const Unit& u) const {
    // 0x372560: Max_Rate_Of_Turn x Object_Max_Speed_Multiplier_Land (degrees per frame) [port: the type flag (+0x790 byte 1
    // bit 0) that skips the multiplier is not read].
    return desc_of(u).turn_rate * cfg_.turn_multiplier_land;
}

bool CombatSystem::needs_turn_to(const Unit& u, const Unit& t) const {
    // Slot 48 0x568230 (TACTICAL_COMBAT §3.1.1 "Slot 48") [H]: |wrap(yaw(obj -> target) - obj yaw)| > tol, tol = 0x372560 / 2
    // and 0.01 degrees when that half is >= 90 (0x20af60). Yaws from 0x20acd0 in [0, 360); the difference wrapped to
    // (-180, 180]. A team-container target is replaced by its member nearest the shooter (0x404fc0) [port: the
    // pending-ability member first is not modelled]. s78f t08 [H]: the locomotor is never read, so a unit without one
    // (a weapon structure) is tested too: within the cone it is snapped to the target, outside it holds state 3 (nothing
    // turns it). [port] A turn rate <= 0 (an explicit Max_Rate_Of_Turn 0; the type default is 2.3) never needs to turn
    // (retail: tolerance 0 and no turn step, so only an exact facing would fire).
    const float rate = turn_step(u);
    if (!(rate > 0.0f)) return false;
    float tol = rate * 0.5f;
    if (tol >= 90.0f) tol = 0.01f;
    const Unit& m = lob_member(t, u.pos);
    const float dx = m.pos.x - u.pos.x, dy = m.pos.y - u.pos.y;
    const float to = (dx == 0.0f && dy == 0.0f) ? 0.0f : dmath::atan2(dy, dx) * kRadToDeg;
    const float yaw = dmath::atan2(u.facing.y, u.facing.x) * kRadToDeg;
    return abs_f(wrap_deg(to - yaw)) > tol;
}

void CombatSystem::face_toward(Unit& u, Vec3 p, float max_step) {
    // max_step < 0: the facing snap 0x394440 (yaw to the target; [port] no pitch: port units stay level, so
    // Targeting_Land_Model_Stay_Horiz_Flat changes nothing). Else the turn request, locomotor vf +0x2e8 (s78f t08 [H]:
    // Walk / LandTeamInfantry 0x42d7b0 -> vf +0x2f0 0x620710 / 0x62d400): the yaw moves by clamp(wrapped error, +-R) at
    // once, R = `max_step` (the rate slot 48 measures against) [port: the Fleeing_Infantry_Speed_Bonus factor of 0x62d400
    // is not modelled]. An object without a locomotor gets the snap (0x394440 writes the object) but no turn request
    // (there is no locomotor to call).
    if (max_step >= 0.0f && !desc_of(u).has_locomotor) return;
    const float dx = p.x - u.pos.x, dy = p.y - u.pos.y;
    const float to = (dx == 0.0f && dy == 0.0f) ? 0.0f : dmath::atan2(dy, dx) * kRadToDeg;
    float yaw = to;
    if (max_step >= 0.0f) {
        const float cur = dmath::atan2(u.facing.y, u.facing.x) * kRadToDeg;
        const float d = wrap_deg(to - cur);
        yaw = abs_f(d) <= max_step ? to : cur + (d > 0.0f ? max_step : -max_step);
    }
    const float r = yaw / kRadToDeg;
    const Vec3 f{dmath::cos(r), dmath::sin(r), 0.0f};
    u.facing = f;
    if (hooks_) hooks_->facing_set(u.id, f);
}

void CombatSystem::attack_idle(Unit& u, bool arg, uint32_t frame, SimRng& rng, bool ordered) {
    // Slot 47 0x569540 (TACTICAL_COMBAT §3.1.1 "Slot 47"): the pose itself is the host's (animation). s78f t08 [H]: an
    // Auto_Deploys + Deploys unit without Fire_Weapon_When_Undeployed plays no pose; with the ordered flag (TD +0xf4,
    // `ordered`) set and the current clip not Deploy it requests the deploy (0x391260) [port: "not the Deploy clip" read
    // as neither deployed nor in a deploy transition]. [M, not modelled] the behaviour counter +0x3c gate (0x4c36f0).
    const UnitDesc& ud = desc_of(u);
    if (ud.auto_deploys && ud.deploys && !ud.fire_weapon_when_undeployed) {
        if (ordered && !deployed_or_deploying(u) && !deploy_transition(u) && hooks_) hooks_->request_deploy(u.id, frame);
        return;
    }
    if (hooks_) hooks_->attack_idle(u.id, arg, frame, rng);
}

ObjectId CombatSystem::container_or_self(ObjectId id) const {
    const Unit* u = unit(id);
    return u && u->team != 0 && u->team <= static_cast<uint32_t>(kNoObject) ? static_cast<ObjectId>(u->team) : id;
}

bool CombatSystem::auto_deploy_check(const Unit& u, ObjectId target) const {
    // 0x568e60 (s78f t08 [H]): false without Deploys or Auto_Deploys, with Fire_Weapon_When_Undeployed, or when already
    // deployed / deploying; else true when the target is ordered (TD +0xf4), the owner is an AI player that is not human
    // (player +0x360 / +0x62; [port] PlayerInfo::ai of an active player), or the target is the formation's attack target
    // and Max_Dist_Walk_While_Deployed > 0 [port: the "current formation block has the attack-move / guard flag" term is
    // not modelled - land formation blocks carry no flags in the movement layer].
    const UnitDesc& ud = desc_of(u);
    if (!ud.deploys || !ud.auto_deploys || ud.fire_weapon_when_undeployed || deployed_or_deploying(u) || deploy_transition(u))
        return false;
    if (u.ordered_target != kNoObject && u.ordered_target == target) return true;
    const PlayerInfo& pi = player(u.owner);
    if (pi.ai && pi.active) return true;
    return u.goal_target == target && ud.max_dist_walk_while_deployed > 0.0f;
}

void CombatSystem::hold_fire(Unit& u, ObjectId target) {
    // s78f t08 [H] (TACTICAL_COMBAT §3.1.1, last rule of the land, space and team services): state 4 with the target ==
    // the pending-ability target (TD +0x148, slot 38) and the target type's Should_Attacker_Hold_Fire_For_Special_Ability
    // -> state 2.
    if (u.tstate != 4 || !hooks_ || hooks_->pending_ability_target(u.id) != target) return;
    const Unit* t = unit(target);
    if (t && desc_of(*t).hold_fire_for_special_ability) u.tstate = 2;
}

void CombatSystem::set_tstate(Unit& u, uint8_t s, ObjectId target) {
    // Slot 31 0x568c80 [H]: s = 4 is not written (the old state stays) while the type has Is_Stationary_When_Attacking and
    // the locomotor is moving, or (s78f t08 [H]) while a pending ability (TD +0x148 / +0x150, slot 38) targets the
    // current target (each replaced by its container): a unit not attacking yet holds fire until the ability fires.
    // s78f t08: the first entry to 4 raises signal 0x1E (AttackStateEvent) - for a member of a team, only the team's
    // first member, on the container [port: the first live combat member in ascending id; a land team head standing in
    // for its container counts as the member].
    bool hold = s == 4 && desc_of(u).stationary_when_attacking && (u.flags & kUnitMoving) != 0;
    if (s == 4 && !hold && hooks_) {
        const ObjectId pt = hooks_->pending_ability_target(u.id);
        hold = pt != kNoObject && container_or_self(pt) == container_or_self(target);
    }
    if (hold) {
        if (u.ttarget != target) u.tstate = 1;  // the old state of a new target: Set_Target's 1
        u.ttarget = target;
        return;
    }
    const bool enter = s == 4 && !(u.tstate == 4 && u.ttarget == target);
    u.tstate = s;
    u.ttarget = target;
    if (!enter) return;
    ObjectId on = u.id;
    if (u.team != 0) {
        for (const Unit& m : units_)  // ascending id
            if (!(m.flags & kUnitDead) && m.team == u.team && m.id < u.id && m.id != static_cast<ObjectId>(u.team)) return;
        if (u.team <= static_cast<uint32_t>(kNoObject)) on = static_cast<ObjectId>(u.team);
    }
    const Unit* g = unit(u.goal_target);
    attack_events_.push_back({on, live(g) ? u.goal_target : target});
}

void CombatSystem::drop_target(Unit& u) {
    // Slot 27: the current target is cleared (TD +8 / +0x10, state 0): the order (and its lock / goal) and the scan pick.
    u.ordered_target = kNoObject;
    u.ordered_hp = -1;
    u.locked = 0;
    u.auto_target = kNoObject;
    u.auto_hp = -1;
    u.target_score = 0.0f;
    u.tstate = 0;
    u.ttarget = kNoObject;
}

const CombatSystem::Unit* CombatSystem::land_team_head(const Unit& u) const {
    // s78f t03 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): a ground team's members (Squad_* types) have no TARGETING;
    // the container's TeamTargeting scans, keeps the target and writes the state into them. [port] The container is the
    // team head (team_head: the container when it is a combat unit, else the team_leader member standing in for it).
    if (cfg_.mode != GameMode::Land || u.team == 0) return nullptr;
    return team_head(u);
}

uint32_t CombatSystem::convert_container(const Unit& v) const {
    // s78i t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" step 3, 0x39b1a0): a member's container is its damage_team
    // (team on land, the squadron in space). A container that is itself a combat unit carries damage_team 0 (or its own
    // id); it is its own team when any live unit names it as damage_team. Else no team.
    if (v.damage_team != 0) return v.damage_team;
    const auto id = static_cast<uint32_t>(v.id);
    for (const Unit& m : units_)
        if (m.damage_team == id && &m != &v && !(m.flags & kUnitDead)) return id;
    return 0;
}

const CombatSystem::Unit* CombatSystem::land_team_lead(const Unit& u) const {
    // s78i t03 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): TeamTargeting is a behaviour of the container and runs
    // while the container lives, whatever its member count. [port] The container lives while its head does (the container
    // unit, else the team_leader stand-in, handed over by hand_over_team), so a head left with no live member keeps the
    // team service instead of falling back to its own TARGETING.
    if (const Unit* h = land_team_head(u)) return h;
    if (cfg_.mode != GameMode::Land || u.team == 0 || (u.flags & kUnitDead)) return nullptr;
    if (static_cast<uint32_t>(u.id) == u.team) return &u;  // the container unit itself
    const auto it = std::lower_bound(team_heads_.begin(), team_heads_.end(), std::make_pair(u.team, ObjectId{-1}));
    return (it != team_heads_.end() && it->first == u.team && it->second == u.id) ? &u : nullptr;
}

void CombatSystem::targeting_service(Unit& u, uint32_t frame, SimRng& rng) {
    // Service_Land 0x634810 / Service_Space 0x633ae0 after the scan (TACTICAL_COMBAT §3.1.1 "Targeting states, slots 47 /
    // 48 and team copies") [H unless tagged]. The state belongs to the current target (TD +8).
    const ObjectId tid = current_target(u.id);
    const Unit* t = unit(tid);
    if (!live(t)) {
        u.tstate = 0;  // target clear 0x565f90
        u.ttarget = kNoObject;
        return;
    }
    const UnitDesc& ud = desc_of(u);
    const bool in_range = in_attack_range(u, u.pos, *t);
    if (cfg_.mode != GameMode::Land) {
        // Space: no TURRET -> 4; TURRET: aimed 4 / not yet 3 ([port] the host's aim state; cannot aim: not modelled).
        // Slot 48 is not used. Out of range: 2.
        // s78f t08: the host's -2 (cannot aim) drops the target, as in 0x633ae0. Last, the hold-fire rule.
        uint8_t s = 2;
        if (in_range && ud.turret && hooks_) {
            const int8_t a = hooks_->turret_aim_state(u.id);
            if (a == -2) {
                drop_target(u);
                return;
            }
            s = a == 0 ? uint8_t{3} : uint8_t{4};
        } else if (in_range) {
            s = 4;
        }
        set_tstate(u, s, tid);
        hold_fire(u, tid);
        return;
    }
    if (!in_range) {
        // Out of range: the turret aim is released [port: the host's turret follows the current target]; a unit neither
        // deployed nor deploying, or with Max_Dist_Walk_While_Deployed > 0, gets 2; a deployed / deploying one drops it.
        if (!deployed_or_deploying(u) || ud.max_dist_walk_while_deployed > 0.0f) {
            set_tstate(u, 2, tid);
        } else {
            drop_target(u);
        }
        return;
    }
    const bool moving = (u.flags & kUnitMoving) != 0;
    const bool ordered = u.ordered_target != kNoObject && u.ordered_target == tid;  // TD +0xf4
    uint8_t s = 4;
    if (ud.turret) {
        // 1. TURRET: nothing in a Walk deploy transition, nor for an undeployed Auto_Deploys + Deploys unit without
        //    Fire_Weapon_When_Undeployed (the state is left as it was). Else the host's aim (s78f t08 [H]: one
        //    Turret_Rotate_Speed slew step per service, no shortcut for non-deployers): aimed -> 4, still slewing -> 3.
        //    Cannot aim (-2: beyond the rotate extent) -> deployed, or the target is not the formation's attack target, or
        //    its type lacks Should_Cause_Limited_Turrets_To_Turn: the target is dropped (slot 27) and the service ends;
        //    else the body turns (unless moving: vf +0x218, a turn-in-place path [port: one turn step]) and state 3.
        //    [port] The lob-arc and turret-bone terms of 0x631cc0 are not modelled.
        if (deploy_transition(u) || (ud.auto_deploys && ud.deploys && !deployed_or_deploying(u) && !ud.fire_weapon_when_undeployed))
            return;
        const int8_t a = hooks_ ? hooks_->turret_aim_state(u.id) : int8_t{-1};
        if (a == -2) {
            if (deployed_or_deploying(u) || u.goal_target != tid || !desc_of(*t).limited_turrets_turn) {
                drop_target(u);
                return;
            }
            if (!moving) face_toward(u, lob_member(*t, u.pos).pos, turn_step(u));
            s = 3;
        } else {
            s = a == 0 ? uint8_t{3} : uint8_t{4};
        }
    } else if (ud.weapon_projectile >= 0) {
        // 2. WEAPON, no TURRET. Moving: 4 only when the model has no Attack clip (infantry with attack animations do not
        //    fire on the move). Not moving: slot 48, then the auto-deploy check 0x568e60 (true -> the deploy request
        //    0x391260; s78f t08 [H]); facing off and (no Deploys, or Must_Face_Target_To_Attack_When_Deployed, or
        //    Fire_Weapon_When_Undeployed): slot 47(1), state 3, the turn request, slot 48 again (now facing -> 4);
        //    otherwise a non-deployer's facing is snapped to the target, state 4.
        if (moving) {
            s = (u.flags & kUnitAttackClip) ? uint8_t{2} : uint8_t{4};
        } else {
            const bool turn = needs_turn_to(u, *t);
            if (auto_deploy_check(u, tid) && hooks_) hooks_->request_deploy(u.id, frame);
            if (turn && (!ud.deploys || ud.must_face_target_when_deployed || ud.fire_weapon_when_undeployed)) {
                attack_idle(u, true, frame, rng, ordered);
                set_tstate(u, 3, tid);
                face_toward(u, lob_member(*t, u.pos).pos, turn_step(u));
                s = needs_turn_to(u, *t) ? uint8_t{3} : uint8_t{4};
            } else {
                if (!ud.deploys) face_toward(u, lob_member(*t, u.pos).pos, -1.0f);
                s = 4;
            }
        }
    }
    set_tstate(u, s, tid);
    if (u.tstate == 4) attack_idle(u, true, frame, rng, ordered);
    hold_fire(u, tid);
}

void CombatSystem::team_targeting_service(Unit& head, uint32_t frame, SimRng& rng) {
    // TeamTargetingBehaviorClass 0x62ee00 -> 0x62eeb0 (TACTICAL_COMBAT §3.1.1 "Team targeting copies") [H unless tagged],
    // ground teams only. The container's state, written raw into every member (slot 31 0x62f550: no signal, no hold
    // rule); members keep no target of their own and read the container's (current_target). Member order = the team
    // list ([port] ascending id). In range: a moving container -> 4 only while SPREAD_OUT (7) is active, else 2. Not
    // moving: no member needs to turn -> every member's facing is snapped to the target, 4; else slot 47(0) on the
    // members, 3, the turn request ([port] one turn step on each member that needs it), 4 when none needs to turn any
    // more. No TURRET branch. Out of range: 2. State 4 -> slot 47(1) on each member (each draws its own blend).
    std::vector<Unit*> members;
    for (Unit& m : units_)
        if (&m != &head && !(m.flags & kUnitDead) && land_team_head(m) == &head) members.push_back(&m);
    const ObjectId tid = current_target(head.id);
    const bool ordered = head.ordered_target != kNoObject && head.ordered_target == tid;  // TD +0xf4 (slot 32 copies it to the members)
    const Unit* t = unit(tid);
    uint8_t s = 0;
    if (live(t)) {
        // Slot 44 / 45 from the container's position with its projectile (slot 42: [port] the head's weapon, else the
        // first member's).
        const Unit* src = &head;
        if (desc_of(head).weapon_projectile < 0)
            for (const Unit* m : members)
                if (desc_of(*m).weapon_projectile >= 0) {
                    src = m;
                    break;
                }
        s = 2;
        if (in_attack_range(*src, head.pos, *t)) {
            if (head.flags & kUnitMoving) {
                s = ability_type_active(head, 7) ? uint8_t{4} : uint8_t{2};
            } else {
                const auto any_turn = [&] {
                    if (needs_turn_to(head, *t)) return true;
                    for (const Unit* m : members)
                        if (needs_turn_to(*m, *t)) return true;
                    return false;
                };
                if (!any_turn()) {
                    face_toward(head, lob_member(*t, head.pos).pos, -1.0f);
                    for (Unit* m : members) face_toward(*m, lob_member(*t, m->pos).pos, -1.0f);
                    s = 4;
                } else {
                    // s78f t08 [H]: the container's vf +0x2e8 (LandTeamContainer 0x5adec0) calls every member's turn
                    // request in team order, so each member steps by clamp(error, +-R) - one already within the cone
                    // lands exactly on the target. [port] A head that is a member standing in turns only when it needs to.
                    for (Unit* m : members) attack_idle(*m, false, frame, rng, ordered);
                    if (needs_turn_to(head, *t)) face_toward(head, lob_member(*t, head.pos).pos, turn_step(head));
                    for (Unit* m : members) face_toward(*m, lob_member(*t, m->pos).pos, turn_step(*m));
                    s = any_turn() ? uint8_t{3} : uint8_t{4};
                }
            }
        }
    }
    if (s == 0) {
        head.tstate = 0;
        head.ttarget = kNoObject;
    } else {
        set_tstate(head, s, tid);
    }
    for (Unit* m : members) {  // the raw copy
        m->tstate = s;
        m->ttarget = s == 0 ? kNoObject : tid;
    }
    if (s == 4) {
        if (desc_of(head).weapon_projectile >= 0) attack_idle(head, true, frame, rng, ordered);  // [port] a member standing in
        for (Unit* m : members) attack_idle(*m, true, frame, rng, ordered);
    }
    // s78f t08: the hold-fire rule last (slot 31(2) through the override: the members get the raw 2 as well).
    if (s == 4 && live(t)) {
        hold_fire(head, tid);
        if (head.tstate == 2)
            for (Unit* m : members) m->tstate = 2;
    }
}

int32_t CombatSystem::model_alt_of(const Unit& u) const {
    // TACTICAL_GAPS §6.3 (0x3ac290): every Land_Damage_Thresholds pair whose threshold >= the hull fraction sets the ALT
    // (the last match wins), default 0.
    const float mh = unit_max_hull(u);
    const float f = mh > 0.0f ? u.health / mh : 0.0f;
    int32_t alt = 0;
    for (const auto& [threshold, a] : desc_of(u).damage_alternates)
        if (f <= threshold) alt = a;
    return alt;
}

int32_t CombatSystem::model_alt(ObjectId id) const {
    const Unit* u = unit(id);
    return u ? model_alt_of(*u) : 0;
}

bool CombatSystem::host_fire_bones(const Unit& h, std::vector<GarrisonFireBone>& out) const {
    // LOB_SOLVER §5.1 "Garrison pick inputs" (s78f t03) [H unless tagged]: the bones of the host model's current ALT entry
    // (model +0xc4, from Land_Damage_Thresholds / _Alternates: model_alt_of), table order i = 0 .. Num_Garrison_Guns - 1,
    // missing bones skipped; each as its 3x4 world matrix of the current pose (0x12d2c0): translation = the launch point,
    // x axis (column 0) -> yaw = atan2(m4, m0) in [0, 360) (0x20acd0). [port] The bind pose x the host transform (position
    // + yaw; port units have no pitch / roll): exact for vanilla, whose only gun host NB_Bunker has no animation clip;
    // a modded animated host would differ. [port] The ALT is re-derived from the current hull fraction (retail stores it
    // after each hull damage, keeping the old one when the table names an ALT beyond the model's).
    out.clear();
    const UnitDesc& hd = desc_of(h);
    const int32_t alt = model_alt_of(h);
    if (hd.num_garrison_guns == 0 || alt < 0 || static_cast<size_t>(alt) >= hd.garrison_bones.size()) return false;
    const float host_yaw = dmath::atan2(h.facing.y, h.facing.x) * kRadToDeg;
    for (const GarrisonBone& b : hd.garrison_bones[static_cast<size_t>(alt)]) {
        if (!b.valid) continue;
        GarrisonFireBone f;
        f.pos = local_to_world(h, b.pos);
        float y = b.yaw_deg + host_yaw;  // the world x axis' yaw, into [0, 360)
        while (y >= 360.0f) y -= 360.0f;
        while (y < 0.0f) y += 360.0f;
        f.yaw_deg = y;
        out.push_back(f);
    }
    return !out.empty();
}

bool CombatSystem::garrison_fire_bones(ObjectId host, Vec3& host_pos, std::vector<GarrisonFireBone>& bones) const {
    bones.clear();
    const Unit* h = unit(host);
    if (!live(h)) return false;
    host_pos = h->pos;
    return host_fire_bones(*h, bones);
}

bool garrison_bone_within(float bone_yaw, Vec3 host, Vec3 target) {
    // LOB_SOLVER §5.1 [H]: target yaw 0x20acd0(host -> T) in [0, 360) (0 when T is straight above / below the host); the
    // difference bone - target into [0, 360) (0x20b100), > 180 -> -360; a candidate when |d| <= 60.
    const float dx = target.x - host.x, dy = target.y - host.y;
    float ty = (dx == 0.0f && dy == 0.0f) ? 0.0f : dmath::atan2(dy, dx) * kRadToDeg;
    if (ty < 0.0f) ty += 360.0f;
    float d = bone_yaw - ty;
    while (d >= 360.0f) d -= 360.0f;
    while (d < 0.0f) d += 360.0f;
    if (d > 180.0f) d -= 360.0f;
    return abs_f(d) <= kGarrisonGunHalfAngle;
}

bool CombatSystem::garrison_muzzle(const Unit& u, Vec3 target, SimRng& rng, Vec3& out) const {
    // Spawn_Projectile 0x54ade0 garrison branch (LOB_SOLVER §5.1 "Garrison fire bones in detail" / "Garrison pick inputs")
    // [H]: the shooter is garrisoned (obj +0x3a7); host = its parent (the grandparent for a team member: the host query's
    // answer); the host type's Num_Garrison_Guns != 0, the host model's ALT in 0..M-1 (jge) and a non-empty entry.
    // Candidates (host_fire_bones, table order): the bones whose x-axis yaw lies within 60 degrees of the yaw from the
    // host's position to `target` (the target object's raw position, a team container not yet replaced, or the ground
    // point). n >= 2 -> one synced Get_Int(0, n - 1); n = 1 -> no draw; n = 0 or a failed gate -> false (the shooter's
    // own muzzle bone).
    if (!(u.flags & kUnitGarrisoned) || !garrison_host_) return false;
    const Unit* h = unit(garrison_host_(u.id));
    if (!live(h)) return false;
    std::vector<GarrisonFireBone> bones;
    if (!host_fire_bones(*h, bones)) return false;
    int32_t n = 0;
    for (const GarrisonFireBone& b : bones) n += garrison_bone_within(b.yaw_deg, h->pos, target) ? 1 : 0;
    if (n == 0) return false;
    int32_t k = n >= 2 ? rng.range_int(0, n - 1) : 0;  // one synced draw for two or more candidates
    for (const GarrisonFireBone& b : bones) {
        if (!garrison_bone_within(b.yaw_deg, h->pos, target) || k-- != 0) continue;
        out = b.pos;
        return true;
    }
    return false;
}

bool CombatSystem::hp_state_allows(const Unit& u, const Hardpoint& h, const Unit* t) const {
    // HardPointClass::Does_Weapon_State_Allow_Firing_At (0x384850, TACTICAL_COMBAT §3.1.1), in order: 1 a dead target is
    // refused [port: no delete-pending / death-clone state]; 2 weapon types only, not destroyed / disabled; 3
    // Fire_Category_Restrictions against the target; 4 each active unit-ability slot's mode m needs its Fire_When_In_<Mode>
    // byte, and the WEAPONS_ENABLED flag must hold; 5 Fire_When_Deployed / _Undeployed; 6 Fire_When_In_Normal / Rocket_
    // Attack_Mode. Read-only (also called by the parallel targeting filter 7).
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    if (t && !live(t)) return false;
    if (!is_weapon(hd.kind) || hp_out(h)) return false;
    if (t && (hd.fire_category_restrictions & desc_of(*t).category_mask)) return false;
    bool any_mode = false;
    for (int s = 0; s < 2; ++s) {
        const UnitAbilityDesc* a = ability(u, s);
        if (!a || !u.ability_active[static_cast<size_t>(s)] || a->ability_type < 1 || a->ability_type > 0x4c) continue;
        any_mode = true;
        for (const uint8_t m : hd.no_fire_modes)
            if (m == a->ability_type) return false;
    }
    if (any_mode) {
        const UnitAbilityDesc* ab[2] = {ability(u, 0), ability(u, 1)};
        const bool act[2] = {u.ability_active[0] != 0, u.ability_active[1] != 0};
        if (!ability_flag(ab, act, kFlagWeaponsEnabled)) return false;
    }
    if (!(deployed_or_deploying(u) ? hd.fire_when_deployed : hd.fire_when_undeployed)) return false;
    return ability_type_active(u, kAbilityRocketAttack) ? hd.fire_when_rocket_attack : hd.fire_when_normal_attack;
}

bool CombatSystem::opportunity_allowed(const Unit& /*u*/, const HardpointDesc& hd, bool idle) const {
    // 0x385620 corrected (ABILITIES_HARDPOINTS.md 4.2): the idle rule holds for every player.
    if (hd.allow_opportunity_fire_when_targeting) return true;
    if (!idle) return false;
    return hd.max_recharge <= cfg_.opportunity_recharge_cutoff || hd.allow_opportunity_fire_when_idle;
}

bool CombatSystem::opportunity_valid(const Unit& u, const Hardpoint& h, const Unit& t) const {
    // "can still be fired at" [M]: live enemy, candidate flags, stealth, Fire_Category_Restrictions,
    // fire range (XY + target radius from the hardpoint) and the fire cone.
    if (!live(&t) || (t.flags & (kUnitSkipTargeting | kUnitGarrisoned))) return false;
    if (!is_enemy(u.owner, t.owner)) return false;
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    const UnitDesc& td = desc_of(t);
    if (!td.is_valid_target) return false;
    if ((t.flags & kUnitStealthed) && !desc_of(u).target_stealth_units) return false;
    if (hd.fire_category_restrictions & td.category_mask) return false;
    const float range = fire_range(hd.fire_range, mod_r(u, ModList::FireRange));
    if (!(distance_xy(hp_world(u, hd), t.pos) <= range + td.radius)) return false;
    if (hd.kind == HardpointKind::WeaponSpecial && target_hidden(u.owner, t.id, true)) return false;  // L1 systems: force 1
    return in_cone(u, h, t.pos);
}

bool CombatSystem::opportunity_fire(Unit& u, Hardpoint& h, uint32_t frame, SimRng& rng) {
    // HP_Scan_Player (0x385190) [H for the cadence / draw / box, M for the rest].
    const Unit* cur = unit(h.opp_target);
    if (!(live(cur) && opportunity_valid(u, h, *cur))) {
        h.opp_target = kNoObject;
        h.opp_hp = -1;
        if (frame < h.opp_next_scan || !trees_) return false;
        h.opp_next_scan = frame + cfg_.fps / 2u;  // rescan at most every FPS x 0.5 frames
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        const float range = fire_range(hd.fire_range, mod_r(u, ModList::FireRange)) * 1.1f;
        const Vec3 o = hp_world(u, hd);
        const Aabb box{{o.x - range, o.y - range, o.z - range}, {o.x + range, o.y + range, o.z + range}};
        const auto n = static_cast<int32_t>(player_count());
        const auto nt = static_cast<int32_t>(trees_->player_count());
        const int32_t start = rng.range_int(0, n - 1);  // one synced draw (random start player)
        const Unit* best = nullptr;
        int32_t best_hp = -1;
        float best_score = 0.0f;
        for (int32_t k = 0; k < n; ++k) {
            const int32_t p = (start + k) % n;
            if (p >= nt || !is_enemy(u.owner, p) || !players_[static_cast<size_t>(p)].active) continue;
            for (EntityHandle eh : trees_->player(static_cast<uint32_t>(p)).query_aabb(box, kSkipDead, serial_ctx_)) {
                const Unit* t = unit(static_cast<ObjectId>(eh));
                if (!live(t) || !opportunity_valid(u, h, *t)) continue;
                const float s = score(u, *t);  // owner's priority set (slot 35) [M]
                if (s == -1.0f) continue;
                // HP_Choose_Aim_Point: synced pick among the live targetable hardpoints.
                int32_t aim = -1;
                const uint16_t cnt = aim_point_count(*t);
                if (cnt > 0) {
                    int32_t kth = rng.range_int(0, static_cast<int32_t>(cnt) - 1);
                    for (uint32_t i = 0; i < t->hp_count; ++i) {
                        const Hardpoint& th = hps_[t->hp_begin + i];
                        if (th.destroyed || !data_->hardpoints[th.desc].targetable) continue;
                        if (kth-- == 0) {
                            aim = static_cast<int32_t>(i);
                            break;
                        }
                    }
                }
                if (!best || s < best_score || s == 1.0f) {
                    best = t;
                    best_hp = aim;
                    best_score = s;
                }
                if (s == 1.0f) goto chosen;  // the first exactly-1.0 candidate is taken at once
            }
        }
    chosen:
        if (!best) return false;
        h.opp_target = best->id;
        h.opp_hp = best_hp;
    }
    const ObjectId save_t = h.target;
    const int32_t save_hp = h.target_hp;
    h.target = h.opp_target;
    h.target_hp = h.opp_hp;
    const bool fired = fire_hardpoint(u, h, frame, rng);
    h.target = save_t;
    h.target_hp = save_hp;
    return fired;
}

bool CombatSystem::fire_hardpoint(Unit& u, Hardpoint& h, uint32_t frame, SimRng& rng) {
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    const UnitDesc& ud = desc_of(u);
    const Unit* tp = unit(h.target);
    if (!live(tp)) return false;
    const Unit& t = *tp;
    const UnitDesc& td = desc_of(t);
    // 1. gates
    if ((t.flags & kUnitStealthed) && !ud.target_stealth_units && t.owner != u.owner) return false;
    if (hd.fire_category_restrictions & td.category_mask) return false;
    if (!is_enemy(u.owner, t.owner)) return false;
    if (hd.projectile < 0 || static_cast<size_t>(hd.projectile) >= data_->projectiles.size()) return false;
    // L1 systems: a special-weapon hard point never fires at a target hidden for its owner with force 1.
    if (hd.kind == HardpointKind::WeaponSpecial && target_hidden(u.owner, t.id, true)) return false;
    // 2. target hardpoint (§2.9 Q2 step 1): dropped when destroyable and dead; none -> the closest live targetable one to
    //    the shooter, per shot (0x398440 -> 0x394a80(target, pos, 0, 0)).
    int32_t thp = h.target_hp;
    if (thp >= 0 && (static_cast<uint32_t>(thp) >= t.hp_count || hp_dead(hps_[t.hp_begin + static_cast<uint32_t>(thp)]))) thp = -1;
    if (thp < 0 && t.hp_count > 0) thp = closest_hardpoint(t, u.pos, true);
    // S9: ability projectile override / suppression (ion cannon shot, lucky shot).
    int32_t proj = hd.projectile;
    float dscale = 1.0f;  // OI abilities: Blast state gate / projectile / damage multiplier (REMAINING_GAPS §5.5)
    if (hooks_ && !hooks_->fire_gate(u.id, &hd, proj, dscale)) return false;
    if (hooks_ && !hooks_->shot_allowed(u.id, t.id, thp, proj)) return false;
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return false;
    // fire bones: A, or the A-B midpoint (C and D never read)
    Vec3 a = local_to_world(u, hd.bone_a);
    Vec3 b = hd.has_bone_b ? local_to_world(u, hd.bone_b) : a;
    if (hooks_) hooks_->fire_bones(u.id, static_cast<uint32_t>(&h - &hps_[u.hp_begin]), a, b);  // S10-int: animated bones
    const Vec3 ref = hd.has_bone_b ? (a + b) * 0.5f : a;
    const float range = fire_range(hd.fire_range, mod_r(u, ModList::FireRange));
    // 3. aim point (§2.9 Q2 step 3): the target hardpoint's bone, else 0x383f70 (range + cone per candidate; none: no shot).
    //    [port] Turret pre-check (Q2 step 2) not modelled: the port has no turret rotation state (the barrel is taken
    //    as on target).
    Vec3 aim0;
    if (thp >= 0) {
        aim0 = hp_world(t, data_->hardpoints[hps_[t.hp_begin + static_cast<uint32_t>(thp)].desc]);
    } else if (!pick_aim_point(u, h, t, ref, range, rng, aim0)) {
        return false;
    }
    // 4. range, XY only
    const float dist = distance_xy(ref, aim0);
    if (!in_fire_range(dist, hd.fire_min_range, range, td.radius)) return false;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    // S9 energy pool (0x56bee0): a shot with Projectile_Energy_Per_Shot needs that much energy [H];
    // tested before the shot's draws [M].
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u) && u.energy < pd.energy_per_shot) return false;
    // 5. lead (0x399450) on the aim point [M: from the fire-bone reference], then the cone test on the lead point.
    Vec3 aim = lead_point(t, ref, aim0, pd.max_speed * static_cast<float>(cfg_.fps));  // OI tactical: circular lead
    if (!in_cone(u, h, aim)) return false;
    // 8. muzzle: 3 synced Get_Float (arguments lost [M]: per axis between the two bones), else
    //    bone A without B, else Percent(50) picks A or B.
    Vec3 muzzle;
    if (hd.randomize_between_fire_bones) {
        muzzle.x = rng.range_float(a.x, b.x);
        muzzle.y = rng.range_float(a.y, b.y);
        muzzle.z = rng.range_float(a.z, b.z);
    } else if (!hd.has_bone_b) {
        muzzle = a;
    } else {
        muzzle = rng.percent(50) ? a : b;
    }
    // 10. inaccuracy: always 3 synced draws unless disabled (debug flag)
    if (cfg_.inaccuracy_enabled) {
        const float f = inaccuracy_f(cfg_.mode, player(u.owner).ai_accuracy);
        float r = 0.0f;
        bool overridden = false;
        for (int s = 0; s < 2 && !overridden; ++s) {
            const UnitAbilityDesc* ab = ability(u, s);
            if (ab && u.ability_active[static_cast<size_t>(s)] && ab->inaccuracy_override > 0.0f) {
                r = ab->inaccuracy_override;  // returned UNSCALED
                overridden = true;
            }
        }
        // L1 systems: tactical weather (land) scales the inaccuracy by (2 - m); m = 1 leaves it untouched.
        const float ws = cfg_.mode == GameMode::Land ? weather_scale(pd, u.owner) : 1.0f;
        if (!overridden) r = scale_inaccuracy(ud.fire_inaccuracy_fixed_radius, f);
        if (!overridden && ws != 1.0f) r = r * ws;
        if (r > 0.0f) {
            aim = aim + inaccuracy_offset(r, rng);
        } else {
            float dd = 0.0f;
            for (const InaccuracyEntry& e : hd.inaccuracy)
                if (e.category_mask & td.category_mask) {
                    dd = e.distance;
                    break;
                }
            // A1j t03 (0x381dc0, TACTICAL_COMBAT §3.1 step 10): the three draws are made only when the scaled distance,
            // the XY distance and Get_Fire_Range are all > 0.
            float ds = scale_inaccuracy(dd, f);
            if (ws != 1.0f) ds = ds * ws;
            if (ds > 0.0f && dist > 0.0f && range > 0.0f) aim = aim + inaccuracy_offset(inaccuracy_spread(ds, dist, range), rng);
        }
    }
    // 11. spawn (an override projectile keeps the hardpoint's damage / type overrides [M])
    const float damage = (hd.projectile_damage > 0.0f ? hd.projectile_damage : pd.damage) * dscale;
    const uint16_t dtype = hd.damage_type != kNoTypeId ? hd.damage_type : pd.damage_type;
    spawn(u, pd, static_cast<uint32_t>(proj), damage, dtype, muzzle, aim, &t, thp, range, frame);
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u)) {
        u.energy = u.energy - pd.energy_per_shot;
        if (u.energy < 0.0f) u.energy = 0.0f;
    }
    // 12. recharge
    const float W = weapon_delay(u, &hd);
    const float F = fire_rate(u);
    h.pulses -= 1;
    const Recharge r = h.pulses > 0 ? hp_pulse_recharge(hd.pulse_delay, h.pulses, W, F, cfg_.fps)
                                    : hp_full_recharge(hd.min_recharge, hd.max_recharge, hd.pulse_count, W, F, cfg_.fps, rng);
    h.cooldown = r.frames;
    h.pulses = r.pulses;
    ++stats_.shots;
    shot_events_.push_back({u.id, t.id, t.owner});  // OI fog: REVEAL vf26 flash
    if (hooks_) hooks_->fire_done(u.id, &hd, frame);  // OI abilities: Blast charged-shot count
    if (hooks_) hooks_->shot_fired(u.id, t.id, thp, proj, frame);
    return true;
}

void CombatSystem::fire_unit_weapon(Unit& u, uint32_t frame, SimRng& rng) {
    // WeaponBehavior (non-hardpoint weapons) [M for the shot itself; the recharge is H].
    // S7 t15 A2 (MOVEMENT §3.1.1, 0x54d090): the recharge counts the frames since the last service (pack +0x2c), so it
    // keeps elapsing while the behaviour is not serviced (jetpack suspension, stun, no base power): the first service back
    // subtracts the whole gap. [port] Every service subtracts (retail only while the cooldown is non-zero and clamps at
    // 0); ready = <= 0 either way, so the shots are the same and the steady-state counter keeps its S8 CRC format.
    // S8 t12 (TACTICAL_COMBAT §3.1.1 "First service after spawn"): the last-service frame starts at 0, so the first service
    // subtracts the whole current frame number and a unit spawned after its drawn initial delay can fire at once.
    const uint32_t last = u.weapon_last == kNoService ? 0u : u.weapon_last;
    const uint32_t elapsed = frame - last;
    u.weapon_last = frame;
    const int64_t cd = static_cast<int64_t>(u.weapon_cooldown) - static_cast<int64_t>(elapsed);
    u.weapon_cooldown = cd < INT32_MIN ? INT32_MIN : static_cast<int32_t>(cd);
    // S8 t26 (§3.1.1 "Fire-at-position order"): while the order is charged (TD +0x3d) the recharge wait is skipped.
    if (u.weapon_cooldown > 0 && !u.lob_charged) return;
    if (!unit_weapon_allowed(u)) return;  // S8 t08: own-weapon gate 0x54c910 steps 1-6 (TACTICAL_COMBAT §3.1.1)
    const UnitDesc& ud = desc_of(u);
    // Step 7 (S8 t10 / t11, "Gate details"; S8 t26 "Fire-at-position order"): a LOBBING_SUPERWEAPON object fires only while
    // its fire-at-position order is charged (TD +0x3d), and then needs neither targeting state 4 nor a target: the request
    // is the ordered ground point with no target object. Otherwise TD +0x18 must be 4 (attacking: slot 44 && !slot 45,
    // re-tested every service) with a non-null target; else the burst restarts (TD +0x28 := Projectile_Fire_Pulse_Count)
    // and nothing fires. This replaces the port's XY range gate (LOB_SOLVER §8 item 22).
    const bool ground = ud.lobbing_superweapon;
    if (ground && !u.lob_charged) return;
    const Unit* tp = nullptr;
    if (!ground) {
        tp = unit(current_target(u.id));
        // s78f t03: the state of the last targeting service (a team member: the container's raw copy) for this target.
        if (!live(tp) || u.tstate != 4 || u.ttarget != tp->id) {
            u.weapon_pulses = ud.weapon_pulse_count;
            return;
        }
        if ((tp->flags & kUnitStealthed) && !ud.target_stealth_units && tp->owner != u.owner) return;
        if (!is_enemy(u.owner, tp->owner)) return;
    }
    const ObjectId tid = tp ? tp->id : kNoObject;
    // S9: the ordered hardpoint (command 40 index) of the ordered target, while it is alive.
    int32_t thp = -1;
    if (tp && tp->id == u.ordered_target && u.ordered_hp >= 0 && static_cast<uint32_t>(u.ordered_hp) < tp->hp_count &&
        !hps_[tp->hp_begin + static_cast<uint32_t>(u.ordered_hp)].destroyed)
        thp = u.ordered_hp;
    int32_t proj = fired_weapon_projectile(u);  // OI abilities: weapon index obj +0x324
    float dscale = 1.0f;  // OI abilities: Blast state gate (REMAINING_GAPS §5.5; a weapon never fires while it runs)
    if (hooks_ && !hooks_->fire_gate(u.id, nullptr, proj, dscale)) return;
    if (hooks_ && !hooks_->shot_allowed(u.id, tid, thp, proj)) return;
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    const float range = fire_range(ud.max_attack_distance, mod_r(u, ModList::FireRange));  // 0x54d090: list 8
    // No range test at fire time (LOB_SOLVER §8 item 22 [H]): the targeting state above is the gate; for a grenade the
    // launch solve below can still find no arc from the muzzle (no shot).
    const bool lob = grenade(pd);
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u) && u.energy < pd.energy_per_shot) return;
    // S8 t06 B4 (LOB_SOLVER §5.1): the launch point is the next fire bone <Muzzle_Bone_Name>_NN (round robin per shot; with
    // more than one Projectile_Types entry and more than one bone, the bone of the weapon index), rest pose turned by the
    // unit's facing [port: retail reads the animated pose]. No bone on a known model: the weapon never fires. Bone index
    // outside the list: no fire bone - the position + the shooter's own Ranged_Target_Z_Adjust (0x398440). [port] No
    // model data: straight shots leave from the position, grenades from the position + the z adjust (the pre-t06 rule).
    Vec3 muzzle = u.pos;
    bool on_bone = false;
    if (ud.muzzle_model) {
        const auto n = static_cast<uint32_t>(ud.muzzle_bones.size());
        if (n == 0) return;
        const uint32_t i = u.weapon_bone < n ? u.weapon_bone : 0u;
        u.weapon_bone = i + 1u < n ? i + 1u : 0u;  // advanced before the spawn: a null spawn keeps the new index (§5.2)
        const int64_t bi = ud.weapon_projectiles.size() > 1 && n > 1 ? static_cast<int64_t>(u.weapon_index) : static_cast<int64_t>(i);
        if (bi >= 0 && bi < static_cast<int64_t>(n)) {
            muzzle = local_to_world(u, ud.muzzle_bones[static_cast<size_t>(bi)]);
            on_bone = true;
        }
    }
    if (!on_bone && (lob || ud.muzzle_model)) muzzle.z = muzzle.z + ud.ranged_target_z_adjust;
    // S8 t10 (LOB_SOLVER §5.1): a garrisoned shooter whose host lists gun bones launches from a synced pick among the host's
    // bones within 60 degrees of the target, replacing the request's bone (the own bone count above is still required).
    if (Vec3 gm; garrison_muzzle(u, tp ? tp->pos : u.lob_point, rng, gm)) muzzle = gm;
    // S8 t04 (LOB_SOLVER §5.1 / §5.2): Spawn_Projectile replaces a team-container target by its member nearest the launch
    // point (the hardpoint index is reset). A grenade solves from the launch point toward the un-led target point Q (the
    // member + its Ranged_Target_Z_Adjust, or the ground point as given); no arc = no shot, no cooldown / pulse change
    // (retried next service; a fire-at-position order stays charged). s78f t06 (LOB_SOLVER §5.2 "Order and draws"): the
    // solve comes AFTER the garrison pick above and BEFORE the aim draws below, as in retail - a garrisoned shooter with
    // two or more qualifying host bones has already spent its Get_Int when the solve finds no arc (kept on purpose: the
    // retry spends one draw per service); a non-garrisoned shooter spends none.
    // [port] The projectile keeps the container as its target object.
    const Unit* am = tp ? &lob_member(*tp, muzzle) : nullptr;
    LobSolution sol;
    Vec3 q;
    if (lob) {
        q = am ? lob_target_point(*am) : u.lob_point;
        if (!lob_solve(muzzle, q, pd.max_speed, lob_g(pd), pd.can_lob_slower, sol)) return;
    }
    // S8 t27 (§3.1.1 "Own-weapon aim point", 0x54a270): no target object -> the point as given, no lead; else the
    // shooter's target hardpoint bone, a synced Target_Bones pick or the position + Ranged_Target_Z_Adjust
    // (own_aim_point), then the circular lead. The hardpoint index is the current target's (TD +0x10: the order's or the
    // scan's), reset for a team member.
    Vec3 aim = u.lob_point;
    if (am) {
        int32_t ahp = -1;
        if (am == tp) ahp = tp->id == u.ordered_target ? u.ordered_hp : tp->id == u.auto_target ? u.auto_hp : -1;
        aim = lead_point(*am, muzzle, own_aim_point(*am, ahp, rng), pd.max_speed * static_cast<float>(cfg_.fps));  // circular lead
    }
    if (cfg_.inaccuracy_enabled) {
        // A1j t03: own-weapon inaccuracy 0x549f80 (TACTICAL_COMBAT §3.1.2) [H unless tagged]. Fixed radius r > 0: X and Y
        // draws in (-r, r); the Z draw is made in space only. Otherwise, with a target object: d = the first
        // Targeting_Fire_Inaccuracy entry hitting the target's CategoryMask (scaled like r), and when d > 0 and the 3D
        // muzzle -> led aim distance L != 0: s = d x L / Targeting_Max_Attack_Distance, X, Y, Z draws in (-s, s).
        const float f = inaccuracy_f(cfg_.mode, player(u.owner).ai_accuracy);
        const float ws = cfg_.mode == GameMode::Land ? weather_scale(pd, u.owner) : 1.0f;  // L1 systems: weather
        float r = scale_inaccuracy(ud.fire_inaccuracy_fixed_radius, f);
        if (ws != 1.0f) r = r * ws;
        for (int s = 0; s < 2; ++s) {
            const UnitAbilityDesc* ab = ability(u, s);
            if (ab && u.ability_active[static_cast<size_t>(s)] && ab->inaccuracy_override > 0.0f) {
                r = ab->inaccuracy_override;
                break;
            }
        }
        if (r > 0.0f) {
            Vec3 o;
            o.x = rng.range_float(-r, r);
            o.y = rng.range_float(-r, r);
            if (cfg_.mode == GameMode::Space) o.z = rng.range_float(-r, r);
            aim = aim + o;
        } else if (am) {
            float d = 0.0f;
            for (const InaccuracyEntry& e : ud.fire_inaccuracy)
                if (e.category_mask & desc_of(*am).category_mask) {
                    d = e.distance;
                    break;
                }
            d = scale_inaccuracy(d, f);
            if (ws != 1.0f) d = d * ws;
            const float L = distance3(muzzle, aim);
            // [port] Targeting_Max_Attack_Distance <= 0 would divide by zero in retail: the draws are kept, spread 0.
            if (d > 0.0f && L != 0.0f)
                aim = aim + inaccuracy_offset(ud.max_attack_distance > 0.0f ? d * L / ud.max_attack_distance : 0.0f, rng);
        }
    }
    spawn(u, pd, static_cast<uint32_t>(proj), pd.damage * dscale, pd.damage_type, muzzle, aim, tp, thp, range, frame,
          lob ? &sol : nullptr, q);
    if (ground) {  // S8 t26: a successful spawn clears the order (TD +0x3c / +0x3d / +0x30..+0x38 := 0)
        u.lob_pending = 0;
        u.lob_charged = 0;
        u.lob_point = Vec3{};
    }
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u)) {
        u.energy = u.energy - pd.energy_per_shot;
        if (u.energy < 0.0f) u.energy = 0.0f;
    }
    const float W = weapon_delay(u, nullptr);
    const float F = fire_rate(u);
    u.weapon_pulses -= 1;
    if (u.weapon_pulses > 0) {
        u.weapon_cooldown = weapon_pulse_delay(ud.weapon_pulse_delay, F, cfg_.fps);
    } else {
        const Recharge r = weapon_reset(ud.weapon_recharge, ud.weapon_pulse_count, W, F, cfg_.fps, rng);
        u.weapon_cooldown = r.frames;
        u.weapon_pulses = r.pulses;
    }
    ++stats_.shots;
    // OI fog: REVEAL vf26 flash on the target owner's grid [port: a ground-point shot has no target owner, no flash].
    if (tp) shot_events_.push_back({u.id, tp->id, tp->owner});
    if (hooks_) hooks_->fire_done(u.id, nullptr, frame);
    if (hooks_) hooks_->shot_fired(u.id, tid, thp, proj, frame);
}

bool CombatSystem::own_launch_point(const Unit& u, Vec3& out) const {
    // LOB_SOLVER §3.6 "Charge-up aim" step 2: the weapon's NEXT fire bone (the WEAPON pack's cycle index TD +0xf0 into its
    // bone list; rest pose turned by the facing, as fire_unit_weapon); [port] no model data: the position + its own
    // Ranged_Target_Z_Adjust (the own weapon's no-model launch point). A known model without fire bones: none.
    const UnitDesc& ud = desc_of(u);
    if (ud.muzzle_model) {
        const auto n = static_cast<uint32_t>(ud.muzzle_bones.size());
        if (n == 0) return false;
        out = local_to_world(u, ud.muzzle_bones[u.weapon_bone < n ? u.weapon_bone : 0u]);
        return true;
    }
    out = Vec3{u.pos.x, u.pos.y, u.pos.z + ud.ranged_target_z_adjust};
    return true;
}

bool CombatSystem::lob_turret_aim(Unit& u) {
    // 0x631fe0(turret, ordered point) with the aim update 0x631210 (LOB_SOLVER §3.6 "Charge-up aim", s78f t04) [H unless
    // tagged]: target angles toward the point as given, relative to the body (yaw from the turret bone - [port] the object
    // position: no turret bone offsets in the combat data - minus the facing yaw, wrapped; pitch geometric with up
    // negative, or -theta from Lob_Solve from the weapon's next fire bone for a category-3 projectile, no solution -> not
    // aimed); outside Turret_Rotate_Extent_Degrees (or, unless Turret_XY_Only, Turret_Elevate_Extent_Degrees) -> not aimed.
    // A unit without deploy states always uses the undeployed extents. Then one slew step: pitch and yaw each move
    // toward the target by at most Turret_Rotate_Speed degrees (yaw by the wrapped difference, then wrapped and clamped to
    // +-extent). Aimed (1) only when the current angles equal the target after the step (0x386170: within 0.0001 after
    // mapping negatives into [0, 360)). [port] Turret_Rotate_Speed <= 0: the target is reached at once.
    const UnitDesc& ud = desc_of(u);
    const int32_t proj = fired_weapon_projectile(u);
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return false;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    const Vec3 p = u.lob_point;
    const float dx = p.x - u.pos.x, dy = p.y - u.pos.y, dz = p.z - u.pos.z;
    const float to = (dx == 0.0f && dy == 0.0f) ? 0.0f : dmath::atan2(dy, dx) * kRadToDeg;
    const float yaw = wrap_deg(to - dmath::atan2(u.facing.y, u.facing.x) * kRadToDeg);
    float pitch = -dmath::atan2(dz, dmath::sqrt(dx * dx + dy * dy)) * kRadToDeg;
    if (grenade(pd)) {  // can-aim 0x631a00: a category-3 projectile solves from the next fire bone
        Vec3 from;
        LobSolution s;
        if (!own_launch_point(u, from) || !lob_solve(from, p, pd.max_speed, lob_g(pd), pd.can_lob_slower, s)) return false;
        pitch = -s.angle * kRadToDeg;
    }
    if (abs_f(yaw) > ud.turret_extent) return false;
    if (!ud.turret_xy_only && abs_f(pitch) > ud.turret_elevate_extent) return false;
    const float sp = ud.turret_rotate_speed;
    if (!(sp > 0.0f)) {
        u.lob_tpitch = pitch;
        u.lob_tyaw = yaw;
    } else {
        const float de = pitch - u.lob_tpitch;
        u.lob_tpitch = abs_f(de) <= sp ? pitch : u.lob_tpitch + (de > 0.0f ? sp : -sp);
        const float dyaw = wrap_deg(yaw - u.lob_tyaw);
        const float ny = wrap_deg(abs_f(dyaw) <= sp ? yaw : u.lob_tyaw + (dyaw > 0.0f ? sp : -sp));
        u.lob_tyaw = std::clamp(ny, -ud.turret_extent, ud.turret_extent);
    }
    const auto w = [](float v) { return v < 0.0f ? v + 360.0f : v; };
    return abs_f(w(u.lob_tpitch) - w(pitch)) <= 0.0001f && abs_f(w(u.lob_tyaw) - w(yaw)) <= 0.0001f;
}

void CombatSystem::lobbing_service(Unit& u, uint32_t frame) {
    // S8 t26 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order", LobbingSuperweaponBehavior vf6 0x4ea6f0) [H unless tagged]:
    // while the order is pending and the charge-up stamp is 0, aim the TURRET at the point (0x631fe0: s78f t04, the slew
    // of lob_turret_aim; the charge-up starts on the service where the angles are reached); no TURRET / no solution /
    // outside the extents / still slewing -> wait (no timeout). Once aimed the stamp := now; when now >= stamp +
    // Lobbing_Superweapon_Chargeup_Frames: charged (TD +0x3d) := 1, stamp := 0. After the stamp the turret holds its aim
    // (TD +0xf6 stays set; the next order slews from there). The chargeup / fire SFX and particle are presentation.
    if (!u.lob_pending) return;
    const UnitDesc& ud = desc_of(u);
    if (u.lob_stamp == 0) {
        if (!ud.turret) return;
        if (!lob_turret_aim(u)) return;
        u.lob_stamp = frame;
        lob_events_.push_back({u.id, false, frame, u.lob_point});  // S8 t30: chargeup SFX + particle
    }
    if (u.lob_stamp != 0 && frame >= u.lob_stamp + ud.lobbing_chargeup_frames) {
        u.lob_charged = 1;
        u.lob_stamp = 0;
        lob_events_.push_back({u.id, true, frame, u.lob_point});  // S8 t30: Lobbing_Superweapon_SFXEvent_Fire
    }
}

bool CombatSystem::fires_at_position(ObjectId id) const {
    const Unit* u = unit(id);
    return live(u) && desc_of(*u).lobbing_superweapon && desc_of(*u).weapon_projectile >= 0;
}

bool CombatSystem::order_fire_at_position(ObjectId id, Vec3 point) {
    if (!fires_at_position(id)) return false;
    Unit* u = unit(id);
    u->lob_point = point;  // 0x4ea3f0: TD +0x30..+0x38 := point, +0x3c := 1 (charged / stamp unchanged)
    u->lob_pending = 1;
    return true;
}

uint8_t CombatSystem::fire_at_position_state(ObjectId id, Vec3* point) const {
    const Unit* u = unit(id);
    if (!u || !(u->lob_pending || u->lob_charged)) return 0;
    if (point) *point = u->lob_point;
    return u->lob_charged ? 2 : 1;
}

void CombatSystem::spawn(const Unit& shooter, const ProjectileDesc& pd, uint32_t pdesc, float damage, uint16_t damage_type,
                         Vec3 muzzle, Vec3 aim, const Unit* target, int32_t target_hp, float range, uint32_t frame,
                         const LobSolution* lob, Vec3 q_point) {
    Projectile p;
    p.serial = next_serial_++;
    p.desc = pdesc;
    p.shooter = shooter.id;
    p.owner = shooter.owner;
    p.target = target ? target->id : kNoObject;
    p.target_hp = target_hp;
    p.pos = muzzle;
    p.dir = normalize(aim - muzzle);
    p.speed = pd.max_speed;
    p.spawn_frame = frame;
    p.damage = damage;
    p.damage_type = damage_type;
    p.kind = pd.damage_kind;
    // Maximum travel (+0x6c) = Get_Fire_Range (Projectile_Max_Flight_Distance when <= 0) + target
    // radius + |aim.z - muzzle.z|; 0 -> lifetime expiry.
    // refixf t01 (S9 §2.4.10 step 1, MaxTravelDistance +0x6c): the same rule for rockets [H]; [M] the target radius term
    // is the retail radius helper 0x397780 (+ its container's when garrisoned), here UnitDesc::radius.
    const float base = range > 0.0f ? range : pd.max_flight_distance;
    // S8 t04 (LOB_SOLVER §5.3): a solved grenade's height term is |Q.z - L.z| (the un-led target point).
    const float dz = lob ? abs_f(q_point.z - muzzle.z) : abs_f(aim.z - muzzle.z);
    p.max_travel = base > 0.0f ? base + (target ? desc_of(*target).radius : 0.0f) + dz : 0.0f;  // S8 t26: no target, no radius
    // S8 t04 (LOB_SOLVER §5.3 / §5.4): category 3 flies the closed-form parabola from the launch point. WeaponBehavior
    // shots take the solver's pitch theta and speed s with the yaw of the led, inaccurate aim; hardpoint shots (Fire_At
    // 0x3825b0, no solver) leave straight at the aim point at Max_Speed and gravity pulls them below that line.
    if (grenade(pd)) {
        p.lob = 1;
        p.gravity = lob_g(pd);
        p.launch = muzzle;
        if (lob) {
            p.lob_dir = lob_forward(aim - muzzle, lob->angle);
            p.speed = lob->speed;  // pack +0x90 := the solver's speed
        } else {
            p.lob_dir = p.dir;
        }
        p.dir = p.lob_dir;
    }
    // refixf t01 (S9 §2.4.10 step 1): a spline rocket keeps FireAtPosition = `aim` and the launch point. [M] `aim` is the
    // shared fire path's aim point (target / hardpoint position, circular lead, then the fire-time inaccuracy) standing in
    // for 0x54a270 + 0x549f80 (not re-read). [M/port] Facing: retail leaves along the x axis of the fire-bone (or shooter
    // world) transform; the port has no bone orientations, so the shared path's muzzle -> aim direction (p.dir) is used.
    if (spline_rocket(pd)) {
        p.rocket = 1;
        add_rocket(p, aim, muzzle);
    }
    projectiles_.push_back(p);
}

// ================================================================================================
// projectiles (sections 3.3-3.5)
// ================================================================================================

CombatSystem::SweepHit CombatSystem::sweep_segment(const Projectile& p, Vec3 delta, QueryContext& ctx) const {
    SweepHit best{kNoObject, 0.0f};
    const Ray ray{p.pos, delta, 1.0f};
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_->player_count()));
    // Find_First_Object_Hit_By_Segment_All_Enemies (0x2abcf0, §3.4): hostile players' trees, Query_Ray, then per entry
    // the precise model test (0x38f900) or the Collision_Box_Modifier sphere (refixf t07: the host's precise_hit_; the
    // query mask is 7 with Projectile_Does_Shield_Damage, else 5). The original returns the first passing entry in tree
    // order; the port takes the minimum (segment fraction, object id) over every passing entry of every hostile tree.
    PreciseQuery pq;
    pq.a = p.pos;
    pq.b = p.pos + delta;
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    pq.mask = pd.does_shield_damage ? 7u : 5u;
    auto scan = [&](int32_t q, SweepHit& out) {
        for (const RayHit& hit : trees_->player(static_cast<uint32_t>(q)).query_ray(ray, kSkipDead, ctx)) {
            const Unit* v = unit(static_cast<ObjectId>(hit.handle));
            if (!live(v)) continue;
            SweepHit c{v->id, hit.t};
            if (precise_hit_) {
                pq.victim = v->id;
                pq.shield_up = v->shield > 0.0f;
                pq.collide = (v->flags & kUnitGarrisoned) == 0;
                PreciseResult r;
                const PreciseOutcome o = precise_hit_(pq, r);
                if (o == PreciseOutcome::Miss) continue;
                if (o == PreciseOutcome::Hit) {
                    c.t = r.t;
                    c.mesh_hp = r.mesh ? hardpoint_by_mesh(*v, *r.mesh) : -1;
                }
            }
            if (out.id == kNoObject || c.t < out.t || (c.t == out.t && c.id < out.id)) out = c;
            // Without the precise test the entries are sorted by (t, id): the first live one is this tree's minimum.
            if (!precise_hit_) break;
        }
    };
    for (int32_t q = 0; q < np; ++q)
        if (is_enemy(p.owner, q)) scan(q, best);
    // §3.4 step 3 (S8 t06): nothing hit and Friendly_Damage_Amount > 0 or Causes_Infection -> the own-player variant
    // 0x2acb90. S8 t10 ("Own-player variant 0x2acb90" [H]): the query over the projectile owner's tree skips only the
    // projectile itself (never in a port tree), NOT the shooter, and yields its first passing candidate (the port's order:
    // the minimum (t, id), as the hostile scan); Sweep then keeps it only when it is the projectile's own target and has no
    // TRANSPORT_LANDING - otherwise there is no own hit (the query does not go on to the next candidate).
    if (best.id == kNoObject && (pd.friendly_damage_amount > 0.0f || pd.causes_infection) && p.owner >= 0 && p.owner < np) {
        SweepHit own{kNoObject, 0.0f};
        scan(p.owner, own);
        if (own.id != kNoObject && own.id == p.target) {
            const Unit* v = unit(own.id);
            if (v && !desc_of(*v).transport_landing) best = own;
        }
    }
    return best;
}

int32_t CombatSystem::hardpoint_by_mesh(const Unit& v, const std::string& mesh) const {
    // 0x394d40 (§2.9 Q1 step 4): the first hardpoint in HardPoints order whose Collision_Mesh equals the name (_stricmp;
    // both sides are upper case); -1 when none.
    if (mesh.empty()) return -1;
    for (uint32_t i = 0; i < v.hp_count; ++i)
        if (data_->hardpoints[hps_[v.hp_begin + i].desc].collision_mesh == mesh) return static_cast<int32_t>(i);
    return -1;
}

void CombatSystem::sweep(uint32_t pi, QueryContext& ctx) {
    const Projectile& p = projectiles_[pi];
    SweepHit best{kNoObject, 0.0f};
    // OI abilities: placed bombs only detonate on the fuse; S8 t04: a stuck sticky grenade no longer sweeps.
    const bool placed = p.fuse >= 0.0f && !p.lob;
    if (p.alive && p.spawn_frame < frame_ && !placed && !p.stuck) {
        if (p.lob) {
            best = sweep_segment(p, lob_next(p, frame_) - p.pos, ctx);  // S8 t04: P -> N(t) (LOB_SOLVER §6.2 step 3)
        } else if (p.rocket == 2) {
            rocket_next_[pi].ok = 0;  // cu8k t01: a thrown saber never sweeps (category 8: no direct hit)
        } else if (p.rocket) {
            // refixf t01: a planned spline rocket sweeps P -> N, N = the path sampled at s + speed (§2.4.2 steps 2 / 5;
            // read-only here, the commit phase advances s). An unplanned one (first flight step) is planned and swept in
            // the serial commit phase (its plan draws).
            RocketNext& rn = rocket_next_[pi];
            rn.ok = 0;
            const Rocket* rk = find_rocket(p.serial);
            Vec3 n;
            if (rk && rk->planned && rk->path.sample(rk->s + p.speed, n)) {
                rn.n = n;
                rn.ok = 1;
                best = sweep_segment(p, n - p.pos, ctx);
            }
        } else {
            const Vec3 delta = p.dir * p.speed;
            best = sweep_segment(p, delta, ctx);
            // A1j t03: Sweep 0x4c97a0 step 1, the land ground test 0x4d0630 of the same segment (TACTICAL_COMBAT §3.4).
            if (straight_ground_test(p, delta)) best.ground = ground_hit(p.pos, p.pos + delta, best.gh);
        }
    }
    sweep_[pi] = best;
}

void CombatSystem::apply_or_queue(const DamageRequest& r, float delay_secs, uint32_t frame) {
    if (delay_secs > 0.0f) {
        Delayed d;
        d.victim = r.victim;
        d.frames = delay_frames(delay_secs, cfg_.fps);
        d.amount = r.amount;
        d.damage_type = r.damage_type;
        d.kind = r.kind;
        d.source_is_projectile = r.source_is_projectile ? 1 : 0;
        d.source = r.source;
        d.projectile_desc = r.projectile_desc;
        d.hardpoint = r.hardpoint;
        delayed_.push_back(d);
    } else {
        take_damage(r, frame);
    }
}

void CombatSystem::process_hit(Projectile& p, Unit& v, float t, uint32_t frame, SimRng& rng, const Vec3* segment,
                               int32_t mesh_hp) {
    // Process_Hit (0x4cb280) [H unless tagged].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const UnitDesc& vd = desc_of(v);
    ++stats_.hits;
    float dmg = p.damage;
    if (dmg == 0.0f) dmg = pd.blast_area_damage;
    const Vec3 impact = segment ? p.pos + *segment * t : p.pos + p.dir * (p.speed * t);
    // refixf t06 (TACTICAL_COMBAT §2.9 Q1 steps 1-3): the hardpoint the hit names.
    int32_t random_hp = -1;
    if (pd.damages_random_hardpoints && v.hp_count > 0) {
        // synced Get_Int(0, count - 1), then probe forward (wrapping) to a live destroyable one
        const auto start = static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(v.hp_count) - 1));
        for (uint32_t k = 0; k < v.hp_count; ++k) {
            const uint32_t j = (start + k) % v.hp_count;
            const Hardpoint& h = hps_[v.hp_begin + j];
            if (!hp_dead(h) && data_->hardpoints[h.desc].destroyable) {
                random_hp = static_cast<int32_t>(j);
                break;
            }
        }
    }
    int32_t hp = -1;
    if (v.id == p.target && p.target_hp >= 0) {
        hp = p.target_hp;  // Take_Damage name override: the targeted hardpoint wins (the random draw is still made)
    } else if (cfg_.mode == GameMode::Land) {
        hp = closest_hardpoint(v, impact, true);  // land: closest live targetable to the projectile [M: impact point]
    } else if (pd.damages_random_hardpoints) {
        hp = random_hp;  // space: the random pick's Collision_Mesh
    } else {
        // space: the struck mesh's hardpoint (refixf t07: from the precise test; without one the mesh_hardpoint stand-in)
        hp = mesh_hp == kMeshUnknown ? mesh_hardpoint(v, impact) : mesh_hp;
    }
    DamageRequest r;
    r.victim = v.id;
    r.amount = dmg;
    r.damage_type = p.damage_type;
    r.kind = p.kind;
    r.source = p.shooter;
    r.source_is_projectile = true;
    r.projectile_desc = static_cast<int32_t>(p.desc);
    r.hardpoint = hp;
    r.has_impact = true;
    r.impact = impact;
    // S9 t04 (§1.5): a Causes_Infection hit attaches the INFECTION behaviour (or resumes it: counter - 1). [port] Before
    // Take_Damage (the order inside 0x4cb280 was not traced); the shooter's INFECTION-ability test is left to the data
    // (vanilla: only Infection_Projectile, thrown by that ability). refix t02: the behaviour runs with the shooter type's
    // Infection_Ability parameters; a shooter that is gone or has none starts a parameterless one (flag only) [port].
    if (pd.causes_infection) {
        const Unit* sh = unit(p.shooter);
        const int32_t idesc = sh && desc_of(*sh).infection.has ? static_cast<int32_t>(sh->desc) : -1;
        infect(v, idesc, p.owner, p.shooter, frame);
    }
    bool absorbed_all = false;
    HitEvent he;  // S10-int (R4): the detonation for the host's PARTICLE objects
    he.victim = v.id;
    he.shooter = p.shooter;
    he.owner = p.owner;
    he.projectile_desc = p.desc;
    he.position = impact;
    if (pd.damage_delay_secs > 0.0f) {
        const float delay = rng.range_float(0.25f * pd.damage_delay_secs, pd.damage_delay_secs);  // synced
        apply_or_queue(r, delay, frame);
        he.delayed = true;
    } else {
        const DamageResult res = take_damage(r, frame);
        absorbed_all = res.absorbed > 0.0f && res.to_hull == 0.0f && res.to_hardpoint == 0.0f;  // [M]
        he.armor_reduced = res.hull_armor_factor <= 0.75f;
    }
    // Visual picks are synced draws too (section 0.1).
    if (!absorbed_all) {
        if (vd.hit_particle_count > 0) he.pick = rng.range_int(0, static_cast<int32_t>(vd.hit_particle_count) - 1);
    } else if (vd.shield_hit_particle_count > 0) {
        he.pick = rng.range_int(0, static_cast<int32_t>(vd.shield_hit_particle_count) - 1);
    }
    he.absorbed = absorbed_all;
    hit_events_.push_back(he);
    if (pd.blast_area_damage > 0.0f && pd.blast_area_range > 0.0f) area_damage(p, impact, v.id, frame);
    detonation_effects(p, impact, v.id, frame);  // S9: on-detonation effects (ion stun)
}

bool CombatSystem::effect_match(const Unit& v, uint64_t mask, const std::vector<int32_t>* types) const {
    // OI abilities: the per-effect victim filter (0x39f300 / 0x395c10): CategoryMask & mask, or a listed unit type;
    // mask 0 without types = every category [M].
    const UnitDesc& vd = desc_of(v);
    if (types)
        for (int32_t t : *types)
            if (t >= 0 && t == vd.object_type) return true;
    if (mask == 0) return !types || types->empty();
    return (vd.category_mask & mask) != 0;
}

void CombatSystem::effect_victims(int32_t owner, bool enemies, Vec3 center, float radius, std::vector<Unit*>& out) {
    // Per-player GOM queries around the detonation (0x2a8fb0), players in list order, ascending id within a player
    // (port, §1.10); enemies of `owner`, or owner + allies (not enemies) when !enemies. Live units, XY circle [M].
    out.clear();
    if (!(radius > 0.0f) || !trees_) return;
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_->player_count()));
    for (int32_t q = 0; q < np; ++q) {
        if (enemies != is_enemy(owner, q)) continue;
        for (EntityHandle id : trees_->player(static_cast<uint32_t>(q)).query_circle_xy(CircleXY{center, radius}, kSkipDead, serial_ctx_)) {
            Unit* v = unit(static_cast<ObjectId>(id));
            if (live(v)) out.push_back(v);
        }
    }
}

void CombatSystem::detonation_effects(const Projectile& p, Vec3 center, ObjectId direct, uint32_t frame) {
    // 0x4cb070 order (TACTICAL_GAPS §1.10 [H dispatch, M effects]): stun -> distract -> heal -> convert ->
    // invulnerability -> ion stun -> combat mod; weaken from the detonate paths (0x4d0800) last [M].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    std::vector<Unit*>& vs = effect_scratch_;
    // Stun (land only): STUNNABLE victims of every enemy player in radius (0x4d01c0 -> 0x647290).
    if (pd.stun && cfg_.mode == GameMode::Land && pd.stun_radius > 0.0f && pd.stun_frames != 0) {
        effect_victims(p.owner, true, center, pd.stun_radius, vs);
        for (Unit* v : vs)
            if (desc_of(*v).stunnable && effect_match(*v, pd.stun_mask, &pd.stun_types)) stun_unit(v->id, pd.stun_frames, frame);
        // Destroy_When_Stunned_Over_Water: the abilities' Stunned hook (ABILITIES_HARDPOINTS §4.12 [port]).
    }
    // Distract: enemies in radius matching the mask get an order toward the detonation (0x2a7b80) [M] - the host's.
    if (pd.distract && pd.distract_radius > 0.0f && hooks_) {
        effect_victims(p.owner, true, center, pd.distract_radius, vs);
        for (Unit* v : vs)
            if (effect_match(*v, pd.distract_mask, nullptr)) hooks_->unit_effect(v->id, UnitEffect::Distracted, p.owner, center, frame);
    }
    // Instant heal: owner and allies in radius: health := min(max, health + int Health_Increase).
    if (pd.heal && pd.heal_radius > 0.0f) {
        effect_victims(p.owner, false, center, pd.heal_radius, vs);
        for (Unit* v : vs) {
            if (!effect_match(*v, pd.heal_mask, nullptr)) continue;
            const float mh = unit_max_hull(*v);
            const float hnew = v->health + static_cast<float>(pd.heal_amount);
            v->health = hnew < mh ? hnew : mh;
            if (hooks_) hooks_->unit_effect(v->id, UnitEffect::Healed, p.owner, center, frame);
        }
    }
    // Convert: enemies in radius -> the victim and its team members: converted flag (decay) + owner := projectile owner.
    if (pd.convert && pd.convert_radius > 0.0f && p.owner >= 0) {
        effect_victims(p.owner, true, center, pd.convert_radius, vs);
        // s78h t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule"): a victim already owned by the new owner is skipped
        // (0x4ca2f0), so a team listed several times converts once. The team is the 0x39b1a0 container: damage_team
        // (= team on land, the squadron in space), so a space squadron converts as a whole as a land team does.
        for (Unit* v : vs) {
            if (!effect_match(*v, pd.convert_mask, nullptr) || !live(v)) continue;
            if (v->owner == p.owner) continue;
            // s78i t04 (§3.5 steps 3-5): Change_Owner covers the container and every member. Order = the notification
            // order: the victim, then the container when it is a combat unit (a member victim's container; a container
            // victim is the victim), then the other members ([port] list order = ascending id).
            const uint32_t team = convert_container(*v);
            const ObjectId vid = v->id;
            convert_unit(vid, p.owner);
            if (team != 0) {
                if (team <= static_cast<uint32_t>(kNoObject) && static_cast<ObjectId>(team) != vid)
                    convert_unit(static_cast<ObjectId>(team), p.owner);  // no-op unless the container is a live combat unit
                for (Unit& m : units_)
                    if (m.damage_team == team && m.id != vid && static_cast<uint32_t>(m.id) != team && live(&m))
                        convert_unit(m.id, p.owner);
            }
        }
    }
    // Invulnerability: the projectile's target first, then owner + allies in radius, up to Max_Targets (0x4cc010).
    if (pd.invulnerability && pd.invulnerability_frames > 0) {
        int32_t n = 0;
        const int32_t cap = pd.invulnerability_max_targets > 0 ? pd.invulnerability_max_targets : 0x7FFFFFFF;
        auto give = [&](Unit* v) {
            if (n >= cap || !live(v) || v->invuln_end > frame) return;
            make_invulnerable(v->id, pd.invulnerability_frames, frame);
            ++n;
        };
        if (Unit* t = unit(p.target); live(t) && effect_match(*t, pd.invulnerability_mask, nullptr)) give(t);
        effect_victims(p.owner, false, center, pd.invulnerability_radius, vs);
        for (Unit* v : vs)
            if (v->id != p.target && effect_match(*v, pd.invulnerability_mask, nullptr)) give(v);
    }
    // Ion stun (0x4cbbb0): direct victim, or victims in radius (enemy trees, XY circle, id order).
    if (pd.ion_stun) {
        if (!(pd.ion_stun_radius > 0.0f)) {
            Unit* v = unit(direct);
            if (live(v)) ion_stun_unit(*v, p.desc, frame);
        } else {
            effect_victims(p.owner, true, center, pd.ion_stun_radius, vs);
            for (Unit* v : vs) ion_stun_unit(*v, p.desc, frame);
        }
    }
    // Combat mod (0x4c6950): enemies within Projectile_Blast_Area_Range: append-insert into list (code - 1).
    if (pd.combat_mod > 0 && pd.combat_mod <= kModListCount) {
        const auto list = static_cast<ModList>(pd.combat_mod - 1);
        const uint32_t crc = name_crc(pd.name);
        effect_victims(p.owner, true, center, pd.blast_area_range, vs);
        for (Unit* v : vs)
            if (effect_match(*v, pd.combat_mod_mask, nullptr))
                v->mods.insert(list, 0, pd.combat_mod_value, expiry_frame(frame, pd.combat_mod_seconds, cfg_.fps), crc, -1);
    }
    // Weaken (0x4d0800): two replace-by-source inserts of -Take_Damage_Increase and -Cause_Damage_Reduction for the
    // duration [L: which two lists - taken as defense (list 0, used as 1 - r) and damage dealt (list 1)].
    if (pd.weaken && pd.weaken_radius > 0.0f) {
        const uint32_t crc = name_crc(pd.name);
        const uint32_t expiry = expiry_frame(frame, pd.weaken_seconds, cfg_.fps);
        effect_victims(p.owner, true, center, pd.weaken_radius, vs);
        for (Unit* v : vs) {
            if (!effect_match(*v, pd.weaken_mask, nullptr)) continue;
            v->mods.set_by_source(ModList::Defense, 0, -pd.weaken_take_increase, expiry, crc, -1);
            v->mods.set_by_source(ModList::Damage, 0, -pd.weaken_cause_reduction, expiry, crc, -1);
        }
    }
}

bool CombatSystem::ion_stun_unit(Unit& v, uint32_t pdesc, uint32_t frame) {
    // IonStunEffect::Apply (0x62b920) [H].
    const ProjectileDesc& pd = data_->projectiles[pdesc];
    const auto frames = static_cast<int32_t>(static_cast<float>(cfg_.fps) * pd.ion_stun_duration);
    if (frames <= 0 || (v.flags & kUnitLocomotorBusy)) return false;
    float secs = pd.ion_stun_duration;
    if (!pd.ion_stun_stack_duration || v.ion_stun_end <= frame) {
        v.ion_stun_end = frame + static_cast<uint32_t>(frames);
    } else {
        v.ion_stun_end = v.ion_stun_end + static_cast<uint32_t>(frames);
        secs = static_cast<float>(v.ion_stun_end - frame + 1u) / static_cast<float>(cfg_.fps);
    }
    if (hooks_) hooks_->ion_stunned(v.id, frame);  // DEFEND is switched off
    const uint32_t crc = name_crc(pd.name);
    const uint32_t expiry = expiry_frame(frame, secs, cfg_.fps);
    v.mods.set_by_source(ModList::MaxSpeed, 1, -pd.ion_stun_speed_reduction, expiry, crc, -1);
    v.mods.set_by_source(ModList::FireRate, 1, -pd.ion_stun_shot_rate_reduction, expiry, crc, -1);
    return true;
}

void CombatSystem::area_damage(const Projectile& p, Vec3 center, ObjectId direct, uint32_t frame) {
    // Apply_Area_Damage (0x4c7f70) [H unless tagged].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const Unit* shooter = unit(p.shooter);
    const float scatter = shooter ? mult(*shooter, ModKind::ScatterRadius) : 1.0f;
    const float R = pd.blast_area_range * scatter;
    if (!(R > 0.0f)) return;
    int32_t victims = 0;
    const auto np = static_cast<int32_t>(std::min<uint32_t>(player_count(), trees_ ? trees_->player_count() : 0u));
    for (int32_t q = 0; q < np; ++q) {
        if (!is_enemy(p.owner, q)) continue;  // Immune_Faction case not modelled
        const auto ids = trees_->player(static_cast<uint32_t>(q)).query_circle_xy(CircleXY{center, R}, kSkipDead, serial_ctx_);
        for (EntityHandle id : ids) {
            Unit* v = unit(static_cast<ObjectId>(id));
            if (!live(v)) continue;
            if (v->id == direct && v->hp_count == 0) continue;
            float d = distance_xy(center, v->pos);
            if (pd.blast_dropoff) {  // cut to the victim's surface [M: segment collision point]
                d = d - desc_of(*v).radius;
                if (d < 0.0f) d = 0.0f;
            }
            const float amount = area_falloff(d, R, pd.blast_dropoff_tiers) * pd.blast_area_damage;
            DamageRequest r;
            r.victim = v->id;
            r.damage_type = p.damage_type;
            r.kind = p.kind;
            r.source = p.shooter;
            r.source_is_projectile = true;
            r.projectile_desc = static_cast<int32_t>(p.desc);
            if (v->hp_count == 0) {
                r.amount = amount;
                apply_or_queue(r, area_delay_seconds(d, R, cfg_.max_secs_for_ae_delayed_damage), frame);
            } else {
                // split equally over live destroyable hardpoints within R (3D), each with its own delay
                uint32_t n = 0;
                for (uint32_t i = 0; i < v->hp_count; ++i) {
                    const Hardpoint& h = hps_[v->hp_begin + i];
                    const HardpointDesc& hd = data_->hardpoints[h.desc];
                    if (!h.destroyed && hd.destroyable && distance3(center, hp_world(*v, hd)) <= R) ++n;
                }
                if (n == 0) {  // [L] nothing in reach: the hull takes it
                    r.amount = amount;
                    apply_or_queue(r, area_delay_seconds(d, R, cfg_.max_secs_for_ae_delayed_damage), frame);
                } else {
                    const float share = amount / static_cast<float>(n);
                    for (uint32_t i = 0; i < v->hp_count; ++i) {
                        const Hardpoint& h = hps_[v->hp_begin + i];
                        const HardpointDesc& hd = data_->hardpoints[h.desc];
                        if (h.destroyed || !hd.destroyable) continue;
                        const float dh = distance3(center, hp_world(*v, hd));
                        if (!(dh <= R)) continue;
                        DamageRequest rh = r;
                        rh.amount = share;
                        rh.hardpoint = static_cast<int32_t>(i);
                        apply_or_queue(rh, area_delay_seconds(dh, R, cfg_.max_secs_for_ae_delayed_damage), frame);
                    }
                }
            }
            ++victims;
            if (pd.blast_max_victims > 0 && victims >= pd.blast_max_victims) return;
        }
    }
}

// ================================================================================================
// Take_Damage (section 3.6)
// ================================================================================================

float CombatSystem::diminishing(uint32_t frame, const Unit& v) const {
    const auto& c = cfg_.diminishing_firepower;
    if (c.empty()) return 1.0f;
    // First hit: 999999 frames (TACTICAL_GAPS 3.3) -> past the last point.
    const float secs = static_cast<float>(v.ever_hit ? frame - v.last_hit_frame : 999999u) / static_cast<float>(cfg_.fps);
    // OI tactical: natural cubic spline (vf2 0x2c84b0): first x_k > x; none -> last y; k = 0 -> first y.
    const size_t m = dim_x_.size();
    const size_t k = static_cast<size_t>(std::upper_bound(dim_x_.begin(), dim_x_.end(), secs) - dim_x_.begin());
    if (k >= m) return dim_y_[m - 1];
    if (k == 0) return dim_y_[0];
    const float h = dim_x_[k] - dim_x_[k - 1];
    if (!(h > 0.0f)) return dim_y_[k];
    const float a = (dim_x_[k] - secs) / h, b = (secs - dim_x_[k - 1]) / h;
    return a * dim_y_[k - 1] + b * dim_y_[k] + ((a * a * a - a) * dim_y2_[k - 1] + (b * b * b - b) * dim_y2_[k]) * (h * h) / 6.0f;
}

DamageResult CombatSystem::take_damage(const DamageRequest& r, uint32_t frame) { return take_damage_inner(r, frame, 0); }

int32_t CombatSystem::pick_death_clone(const UnitDesc& vd, int32_t key, int32_t sub_key) {
    // Pick_Death_Clone (0x3718f0), index -1 (S9_DEATH_CLONE_SHIELDS.md §1.3 [H]): key 1 with a sub-key reads 0x18 +
    // sub-key; one entry -> it; two or more -> one synced Get_Int(0, n - 1); none -> nothing (no draw).
    if (key == kKindNormal && sub_key != -1) key = kDeathCloneSubKeyBase + sub_key;
    int32_t n = 0;
    for (const DeathCloneEntry& c : vd.death_clones)
        if (c.key == key) ++n;
    if (n == 0) return -1;
    int32_t pick = 0;
    if (n >= 2) {
        SimRng* rng = listener_rng();
        pick = rng ? rng->range_int(0, n - 1) : 0;  // [port] no RNG bound: first entry
    }
    for (const DeathCloneEntry& c : vd.death_clones)
        if (c.key == key && pick-- == 0) return c.type;
    return -1;
}

void CombatSystem::kill(Unit& v, ObjectId killer, uint8_t kind, uint32_t frame, int32_t sub_key) {
    // Die (0x39bdb0), ABILITIES_HARDPOINTS.md 4.9 [H unless tagged].
    if (v.flags & kUnitDead) return;
    // 1. dying flag, health 0, killer
    v.flags |= kUnitDead;
    v.killer = killer;
    v.health = 0.0f;
    hand_over_team(v);  // s78g t06: the team keeps a head from this moment on
    // 2. missile-shield / jammer emitter removal: 0x4d07e0 removes ONE entry (TACTICAL_GAPS §1.7)
    v.emitter_radius = 0.0f;
    if (v.emitter_entries > 0) --v.emitter_entries;
    if (hooks_) hooks_->unit_dying(v.id, frame);
    // 3. death signal 0x25
    DeathEvent ev;
    ev.id = v.id;
    ev.killer = killer;
    ev.kind = kind;
    deaths_.push_back(ev);
    const size_t ei = deaths_.size() - 1;
    ++stats_.deaths;
    // 5. live destroyable hardpoints die in list order: HP_Take_Damage(kind 6, scaled base health)
    for (uint32_t i = 0; i < v.hp_count; ++i) {
        Hardpoint& h = hps_[v.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (!hd.destroyable || h.destroyed || !(h.health > 0.0f)) continue;
        DamageResult tmp;
        hardpoint_damage(v, h, hd.health * cfg_.hardpoint_health_multiplier, killer, tmp, frame, true);
    }
    // 7. world removal (0x391310): Tactical_Respawn, Destruction_Survivors (OI abilities, TACTICAL_GAPS §1.9)
    death_spawns(v, frame);
    // 8. death clone (tactical modes, kind != 15), S9_DEATH_CLONE_SHIELDS.md §1.2 / §1.3 [H]:
    //    key remap (deploying / deployed Deploys walker: 11 for kind 8, else 12; else an active INFECTION: 20), the
    //    existence check (back to the kind when the remapped key has no list; its draw is consumed, its pick dropped),
    //    then the spawn lookup with the Damage_Type sub-key, else key 1, else the dying unit's own type.
    const UnitDesc& vd = desc_of(v);
    if (cfg_.death_clones && kind != kKindNoDeathClone) {
        int32_t key = kind;
        if (deployed_or_deploying(v))  // S8 t11: 0x39a390 (pending or state 0x19; not in a deploy transition)
            key = kind == 8 ? kKindDeployed : kKindDeployedOther;
        else if (v.flags & kUnitInfected)
            key = kKindInfection;
        if (pick_death_clone(vd, key, -1) < 0) key = kind;
        int32_t clone = pick_death_clone(vd, key, sub_key);
        if (clone < 0) clone = pick_death_clone(vd, kKindNormal, -1);
        if (clone < 0) clone = vd.object_type;
        deaths_[ei].death_clone = clone;
        deaths_[ei].clone_key = key;
    }
    // 9. death projectile: one synced Get_Int(0, n - 1) (no draw for n = 1); spawned at the position +
    //    Ranged_Target_Z_Adjust, owned by the dying object's player, along its facing [L].
    if (!vd.death_projectiles.empty()) {
        const auto n = static_cast<int32_t>(vd.death_projectiles.size());
        SimRng* rng = listener_rng();
        const int32_t pick = rng ? rng->range_int(0, n - 1) : 0;  // [port] no RNG bound: first entry
        const int32_t pdi = vd.death_projectiles[static_cast<size_t>(pick)];
        if (pdi >= 0 && static_cast<size_t>(pdi) < data_->projectiles.size()) {
            const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(pdi)];
            Projectile p;
            p.serial = next_serial_++;
            p.desc = static_cast<uint32_t>(pdi);
            p.shooter = v.id;
            p.owner = v.owner;
            p.pos = Vec3{v.pos.x, v.pos.y, v.pos.z + vd.ranged_target_z_adjust};
            p.dir = normalize(v.facing);
            p.speed = pd.max_speed;
            p.spawn_frame = frame;
            p.damage = pd.damage;
            p.damage_type = pd.damage_type;
            p.kind = pd.damage_kind;
            p.max_travel = pd.max_flight_distance > 0.0f ? pd.max_flight_distance : 0.0f;
            projectiles_.push_back(p);
            deaths_[ei].death_projectile = pdi;
        }
    }
    // DeathBehavior Initialise (vf9 0x58cdb0, TACTICAL_GAPS §1.9 [H]; attached after Die [M: the draw comes after the
    // step 7-9 draws]): with Spin_Away_On_Death one synced Get_Float(0, 1); spin away when draw <= the chance.
    if (vd.spin_away_on_death) {
        SimRng* rng = listener_rng();
        const float u = rng ? rng->range_float(0.0f, 1.0f) : 1.0f;  // [port] no RNG bound: no draw, no spin-away
        deaths_[ei].spin_away = rng != nullptr && u <= vd.spin_away_chance;
    }
}

void CombatSystem::death_spawns(Unit& v, uint32_t frame) {
    // World removal 0x391310 (Die step 7) with obj +0x3a0 bit 0x80 clear (TACTICAL_GAPS §1.9 [H]). The host creates
    // the objects (one id each, SurvivorEvent order); the owner rule is the host's.
    const UnitDesc& vd = desc_of(v);
    if (vd.tactical_respawn_secs > 0.0f && vd.object_type >= 0) {  // 1. delayed creation of the same type
        SurvivorEvent e;
        e.dead = v.id;
        e.type = vd.object_type;
        e.owner = v.owner;
        e.position = v.pos;
        e.facing = v.facing;
        e.respawn = true;
        e.due_frame = frame + static_cast<uint32_t>(static_cast<float>(cfg_.fps) * vd.tactical_respawn_secs);
        survivor_events_.push_back(e);
    }
    if (vd.destruction_survivors.empty()) return;
    SimRng* rng = listener_rng();
    if (!rng) return;  // [port] outside a tick without a bound RNG: no survivors
    // 2. count = synced Get_Int(Minimum, Maximum)
    const int32_t count = rng->range_int(vd.min_survivors, vd.max_survivors);
    const float total = vd.destruction_survivors.back().second;
    for (int32_t i = 0; i < count; ++i) {
        // 3. two inline synced draws per survivor (0x378cb0): spawn when u >= 1 - total; v in [0, total] picks the first
        //    entry with cumulative weight >= v (binary search 0x378b70).
        const float u = rng->inline_unit_float();
        const float pick = rng->inline_unit_float() * total;
        if (!(u >= 1.0f - total)) continue;
        const auto it = std::lower_bound(vd.destruction_survivors.begin(), vd.destruction_survivors.end(), pick,
                                         [](const std::pair<int32_t, float>& a, float x) { return a.second < x; });
        if (it == vd.destruction_survivors.end() || it->first < 0) continue;
        SurvivorEvent e;
        e.dead = v.id;
        e.type = it->first;
        e.owner = v.owner;
        e.position = v.pos;
        e.facing = v.facing;
        survivor_events_.push_back(e);
    }
}

void CombatSystem::hull_damage(Unit& v, float x, ObjectId source, DamageResult& out, uint32_t frame, uint8_t kind,
                               int32_t sub_key) {
    // Apply_Hull_Damage (0x3ab890).
    if (v.flags & kUnitDead) return;
    const UnitDesc& vd = desc_of(v);
    // Fire ignition (4.8): a combustible victim hit with kind 7 that is not burning ignites instead.
    if (vd.combustible && kind == kKindFire && !v.burning) {
        // OI abilities: BURNING (0x2d) attach. Interval 30 (TACTICAL_GAPS §1.8 [H]); base Initialise (§6.4): first due
        // frame = now + synced Get_Int(0, 29). burn_countdown holds the due frame.
        v.burning = 1;
        SimRng* rng = listener_rng();
        const int32_t first = rng ? rng->range_int(0, kBurningInterval - 1) : 0;  // [port] no RNG bound: due at once
        v.burn_countdown = static_cast<int32_t>(frame) + first;
        out.to_hull = 0.0f;
        return;
    }
    const float old = v.health;
    v.health = old - x;
    out.to_hull = x;
    if (vd.damaged_smoke) {  // Damaged_Smoke_Asset_Name at 75 % / 50 % crossed downward [H]
        const float mh = unit_max_hull(v);
        for (const uint8_t pct : {uint8_t{75}, uint8_t{50}}) {
            const float t = mh * (static_cast<float>(pct) / 100.0f);
            if (old > t && v.health <= t) {
                SmokeEvent e;
                e.id = v.id;
                e.percent = pct;
                smoke_events_.push_back(e);
            }
        }
    }
    out.hull_lost = old - (v.health > 0.0f ? v.health : 0.0f);  // A1_READINGS §30.2: the clamped hull loss
    if (v.health <= 0.0f) {
        if (v.flags & kUnitCannotDie) {
            v.health = old > 1.0f ? old : 1.0f;  // max(1, old)
            out.hull_lost = 0.0f;
        } else {
            kill(v, source, kind, frame, sub_key);
            out.killed = true;
        }
    }
}

void CombatSystem::hardpoint_damage(Unit& v, Hardpoint& h, float x, ObjectId source, DamageResult& out, uint32_t frame,
                                    bool owner_dying, int32_t sub_key) {
    // HP_Take_Damage (0x387f50): destroyable, health > 0, amount > 0; subtract with NO clamp.
    if (!(h.health > 0.0f) || !(x > 0.0f)) return;
    h.health = h.health - x;
    out.to_hardpoint = x;
    if (h.health > 0.0f) return;
    h.destroyed = 1;  // HP_Check_Destroyed; not reversible, never repaired
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    bool last = true;  // no functioning hardpoint of the kind remains (destroyed / disabled do not count)
    for (uint32_t i = 0; i < v.hp_count; ++i) {
        const Hardpoint& o = hps_[v.hp_begin + i];
        if (!hp_out(o) && data_->hardpoints[o.desc].kind == hd.kind) last = false;
    }
    // Set_System_Online(hp, 0, destroyed = 1) (0x384d00): combat + ability side, special-weapon flag.
    const auto index = static_cast<uint32_t>(&h - &hps_[v.hp_begin]);
    set_system_online(v, index, false, true, frame);
    HardpointEvent ev;
    ev.owner = v.id;
    ev.index = index;
    ev.kind = hd.kind;
    ev.owner_dying = owner_dying;
    ev.last_of_kind = last;
    ev.explosion_type = hd.death_explosion_type;
    ev.breakoff_type = hd.death_breakoff_type;
    hp_events_.push_back(ev);
    if (!owner_dying && desc_of(v).destroy_when_all_hardpoints_destroyed && !(v.flags & kUnitDead)) {
        bool any_live = false;
        for (uint32_t i = 0; i < v.hp_count; ++i) {
            const Hardpoint& o = hps_[v.hp_begin + i];
            if (!o.destroyed && data_->hardpoints[o.desc].destroyable) any_live = true;
        }
        if (!any_live) hull_damage(v, v.health, source, out, frame, kKindNormal, sub_key);  // hull set to 0 -> the hull path kills [H]
    }
}

DamageResult CombatSystem::take_damage_inner(const DamageRequest& r, uint32_t frame, int depth) {
    DamageResult out;
    Unit* vp = unit(r.victim);
    if (!live(vp)) return out;
    Unit& v = *vp;
    const UnitDesc& vd = desc_of(v);
    // 1. parent container takes the damage when alive [M]
    if (v.damage_parent != kNoObject && depth < 4) {
        const Unit* pc = unit(v.damage_parent);
        if (live(pc)) {
            DamageRequest r2 = r;
            r2.victim = pc->id;
            r2.hardpoint = -1;
            return take_damage_inner(r2, frame, depth + 1);
        }
    }
    // 2. teams [H]. s78g t06: no mode gate in §3.6, so space squadrons count too (vanilla Luke_XWing / TIE_Prototype carry
    // Redirect_Damage_To_Teammates): damage_team = team, or a space squadron member's squadron.
    const uint32_t dteam = v.damage_team;
    if (!r.no_team && dteam != 0 && (vd.redirect_damage_to_teammates || vd.share_damage_with_teammates) && depth < 4) {
        uint32_t count = 0;
        for (const Unit& m : units_)
            if (m.damage_team == dteam && !(m.flags & kUnitDead)) ++count;
        if (vd.redirect_damage_to_teammates && count > 1) {
            const float each = r.amount / static_cast<float>(count - 1);
            for (size_t i = 0; i < units_.size(); ++i) {
                const Unit& m = units_[i];
                if (m.damage_team != dteam || (m.flags & kUnitDead) || m.id == v.id) continue;
                DamageRequest r2 = r;
                r2.victim = m.id;
                r2.amount = each;
                r2.hardpoint = -1;
                r2.no_team = true;
                take_damage_inner(r2, frame, depth + 1);
            }
            out.applied = true;
            return out;
        }
        if (vd.share_damage_with_teammates) {
            const float self_max = unit_max_hull(v);
            if (self_max > 0.0f) {
                for (size_t i = 0; i < units_.size(); ++i) {
                    const Unit& m = units_[i];
                    if (m.damage_team != dteam || (m.flags & kUnitDead)) continue;
                    DamageRequest r2 = r;
                    r2.victim = m.id;
                    r2.amount = unit_max_hull(m) * r.amount / self_max;
                    r2.no_team = true;
                    if (m.id != v.id) r2.hardpoint = -1;
                    take_damage_inner(r2, frame, depth + 1);
                }
                out.applied = true;
                return out;
            }
        }
    }
    // 3. gates [M]
    const uint8_t k = r.kind;
    if ((v.flags & kUnitInvulnerable) && k != kKindDebug && k != kKindVehicleThief) return out;
    if (v.invuln_end > frame && k != kKindDebug && k != kKindVehicleThief) return out;  // OI abilities: INVULNERABLE 0x4b
    if (vd.immune_to_damage) return out;
    if (!cfg_.damage_enabled && k != kKindDebug && k != 8 && k != kKindVehicleThief) return out;
    out.applied = true;
    const Unit* src = unit(r.source);  // a dead-this-tick source still counts until the flush [M]
    float x = r.amount;
    x = x * mult(v, ModKind::TakeDamage);  // 4
    if (src && (r.source_is_projectile || (src->flags & kUnitSkipTargeting)))
        x = x * mult(*src, ModKind::CauseDamage);  // 5
    if (v.flags & kUnitInBaseShield) x = x * cfg_.base_shield_vulnerability;  // 6
    if (k == kKindNormal) {  // 7 diminishing firepower (ALLOW_TARGET_DIMINISHING_FIREPOWER, default true)
        bool allow = true;
        if (src) {
            const UnitAbilityDesc* ab[2] = {ability(*src, 0), ability(*src, 1)};
            const bool act[2] = {src->ability_active[0] != 0, src->ability_active[1] != 0};
            allow = ability_flag(ab, act, kFlagAllowDiminishingFirepower);
        }
        if (allow) x = x * diminishing(frame, v);
    }
    if (v.flags & kUnitUnderConstruction) x = x * cfg_.under_construction_damage_multiplier;  // 8
    if (v.flags & kUnitRetreating) x = x * player(v.owner).retreat_damage_mod;               // 9 (cu8 t03: per faction)
    // 10 hardpoint name = r.hardpoint
    int32_t hp_index = r.hardpoint;
    // 11 lucky shot (S9 hook): consumed on the hit, x its CAUSE_DAMAGE multiplier; a hull hit moves
    //    to the hardpoint closest to the impact.
    if (hooks_ && r.source != kNoObject) x = x * hooks_->lucky_shot(r.source, v.id, hp_index, r.impact, r.has_impact);
    {  // 12 Get_Damage_Multiplier
        const float a1 = src ? mod_r(*src, ModList::Damage) : 0.0f;
        const float ai = (src && player(src->owner).ai) ? player(src->owner).ai_damage_multiplier : 1.0f;
        x = x * damage_multiplier(a1, mod_r(v, ModList::Defense), ai, 1.0f);
    }
    // 13 damage listeners (style Take_Damage special abilities, e.g. Redirect_Blaster) may cancel.
    if (hooks_ && hooks_->damage_listener(v.id, r.source, r.source_is_projectile ? r.projectile_desc : -1, frame,
                                          listener_rng())) {
        out.cancelled = true;
        out.after_multipliers = 0.0f;
        return out;
    }
    out.after_multipliers = x;
    v.ever_hit = 1;
    v.last_hit_frame = frame;
    // 14-16 shields, armor, hull [H]
    uint16_t hull_armor = vd.armor_type;
    for (int s = 1; s >= 0; --s) {  // active ability armor override, slot 1 before slot 0 [M]
        const UnitAbilityDesc* ab = ability(v, s);
        if (ab && v.ability_active[static_cast<size_t>(s)] && ab->armor_override != kNoTypeId) {
            hull_armor = ab->armor_override;
            break;
        }
    }
    const float sf = data_->damage.factor(r.damage_type, vd.shield_armor_type);
    const float hf = data_->damage.factor(r.damage_type, hull_armor);
    out.hull_armor_factor = hf;
    const ProjectileDesc* pd = (r.source_is_projectile && r.projectile_desc >= 0 &&
                                static_cast<size_t>(r.projectile_desc) < data_->projectiles.size())
                                   ? &data_->projectiles[static_cast<size_t>(r.projectile_desc)]
                                   : nullptr;
    const float sx = x * sf;
    float absorbed = 0.0f;
    if (k != kKindBypassShield) {
        const bool can = !r.source_is_projectile || (pd && pd->does_shield_damage);
        const bool lockout = (v.shield_depleted &&
                              shield_lockout_active(frame, v.shield_depleted_frame, cfg_.fps, cfg_.depleted_shield_disable_time)) ||
                             (hooks_ && hooks_->ion_storm_shields_down(v.id, frame));  // L1 systems: ion storm
        if (can && !lockout && v.shield > 0.0f && sx > 0.0f) {
            absorbed = v.shield < sx ? v.shield : sx;  // min(shield, x)
            v.shield = v.shield - absorbed;
            if (v.shield <= 0.0f) {  // depletion starts the lockout
                v.shield = 0.0f;
                v.shield_depleted = 1;
                v.shield_depleted_frame = frame;
            }
        }
    }
    out.absorbed = absorbed;
    // S9: energy pool absorption (EnergyPool::Absorb 0x56c230, Does_Energy_Damage only) [L: takes
    // min(energy, the unabsorbed rest) in shield-armor space].
    float energy_taken = 0.0f;
    if (pd && pd->does_energy_damage && has_energy_pool(v) && k != kKindBypassShield) {
        const float rest = sx - absorbed;
        if (rest > 0.0f && v.energy > 0.0f) {
            energy_taken = v.energy < rest ? v.energy : rest;
            v.energy = v.energy - energy_taken;
        }
    }
    out.energy_absorbed = energy_taken;
    // cu8f t02 (CU8F_T01_T02_T07_RE.md §2.4): any positive absorption ("drained" is not "reached 0") with
    // Projectile_Disables_Engines_When_Power_Drained disables the victim's engines for the projectile's duration.
    if (energy_taken > 0.0f && pd->disables_engines_when_drained && hooks_)
        hooks_->engines_drained(v.id, pd->disable_engines_duration, frame);
    float hx;
    if (sf != 0.0f) {
        hx = (sx - absorbed - energy_taken) / sf * hf;  // back to raw, then hull-armor space
    } else {
        hx = x * hf;  // [L] shield factor 0: nothing to convert back from
    }
    if (pd && !pd->does_hitpoint_damage) hx = 0.0f;
    // S9 t04 (§1.1): the death-clone sub-key = the source projectile's Damage_Type index (-1: not a projectile).
    const int32_t sub_key = pd && pd->damage_type != kNoTypeId ? static_cast<int32_t>(pd->damage_type) : -1;
    // 17 hardpoint routing: a named live destroyable hardpoint takes everything; a named dead destroyable one swallows the
    //    hit (refixf t06, TACTICAL_COMBAT §2.9 Q1 step 5: neither hardpoint nor hull; the shield part stays absorbed); a
    //    named non-destroyable one -> hull.
    if (hp_index >= 0 && static_cast<uint32_t>(hp_index) < v.hp_count) {
        Hardpoint& h = hps_[v.hp_begin + static_cast<uint32_t>(hp_index)];
        if (data_->hardpoints[h.desc].destroyable) {
            if (!hp_dead(h)) hardpoint_damage(v, h, hx, r.source, out, frame, false, sub_key);
            track_damage(v, absorbed + out.to_hardpoint, frame);
            record_damage(r, v, src, hp_index, out, frame);
            return out;
        }
    }
    // 18 hull
    hull_damage(v, hx, r.source, out, frame, k, sub_key);
    track_damage(v, absorbed + hx, frame);
    record_damage(r, v, src, -1, out, frame);
    return out;
}

void CombatSystem::record_damage(const DamageRequest& r, const Unit& v, const Unit* src, int32_t hp, const DamageResult& out,
                                 uint32_t frame) {
    DamageEvent e;
    e.victim = v.id;
    e.source = r.source;
    e.source_owner = src ? src->owner : -1;
    e.source_is_projectile = r.source_is_projectile;
    e.projectile_desc = r.projectile_desc;
    e.kind = r.kind;
    e.hardpoint = hp;
    e.after_multipliers = out.after_multipliers;
    e.absorbed = out.absorbed;
    e.energy_absorbed = out.energy_absorbed;
    e.to_hardpoint = out.to_hardpoint;
    e.to_hull = out.to_hull;
    e.hull_lost = out.hull_lost;
    e.killed = out.killed || (v.flags & kUnitDead) != 0;
    e.frame = frame;
    damage_events_.push_back(e);
}

// ================================================================================================
// upkeep (shields, hull <-> hardpoint coupling)
// ================================================================================================

void CombatSystem::upkeep_unit(Unit& u, uint32_t frame) {
    if (u.flags & kUnitDead) return;
    const UnitDesc& ud = desc_of(u);
    const float C = cfg_.hull_vs_hardpoints_constraint;
    if (u.hp_count) {
        const float maxh = unit_max_hull(u);
        // Hull cap (0x3a6b80, types with +0xa8 only): hull fraction <= min(1, hardpoint fraction + C).
        if (ud.destroy_when_all_hardpoints_destroyed && maxh > 0.0f) {
            float cap = hp_fraction(u) + C;
            if (cap > 1.0f) cap = 1.0f;
            if (cap > 0.0f && u.health / maxh > cap) u.health = cap * maxh;
        }
        // Hardpoint cap (0x3a76b0): excess = (hp fraction - min(1, hull fraction + C)) x T, T = sum
        // of difficulty-scaled base health (no modifiers); each live hardpoint loses health_i / T x excess.
        float T = 0.0f;
        for (uint32_t i = 0; i < u.hp_count; ++i) {
            const HardpointDesc& hd = data_->hardpoints[hps_[u.hp_begin + i].desc];
            if (hd.destroyable) T = T + hd.health * cfg_.hardpoint_health_multiplier;
        }
        if (T > 0.0f && maxh > 0.0f) {
            float hull_cap = u.health / maxh + C;
            if (hull_cap > 1.0f) hull_cap = 1.0f;
            const float excess = (hp_fraction(u) - hull_cap) * T;
            if (excess > 0.0f) {
                for (uint32_t i = 0; i < u.hp_count; ++i) {
                    Hardpoint& h = hps_[u.hp_begin + i];
                    if (h.destroyed || !data_->hardpoints[h.desc].destroyable) continue;
                    h.health = h.health - h.health / T * excess;
                }
            }
        }
    }
    // Shield regeneration (0x5494b0 / 0x549250).
    if (ud.shield_points > 0.0f) {
        u.shield_regen_countdown -= 1;
        if (u.shield_regen_countdown <= 0) {
            u.shield_regen_countdown = std::max<int32_t>(
                1, shield_regen_interval_frames(cfg_.fps, mult(u, ModKind::ShieldRegenInterval), cfg_.shield_recharge_interval_secs));
            uint32_t gens = 0, live_gens = 0;
            for (uint32_t i = 0; i < u.hp_count; ++i) {
                const Hardpoint& h = hps_[u.hp_begin + i];
                if (data_->hardpoints[h.desc].kind != HardpointKind::ShieldGenerator) continue;
                ++gens;
                if (!h.destroyed) ++live_gens;
            }
            const float frac = gens ? static_cast<float>(live_gens) / static_cast<float>(gens) : 1.0f;
            const float amount = ud.shield_refresh_rate * mult(u, ModKind::ShieldRegen) * frac;
            const bool lockout = u.shield_depleted &&
                                 shield_lockout_active(frame, u.shield_depleted_frame, cfg_.fps, cfg_.depleted_shield_disable_time);
            // OI abilities: Shield_Requires_Base_Power (0x56bb00, TACTICAL_GAPS §7.3) - no regeneration without power [M].
            const bool unpowered = ud.shield_requires_base_power && !has_base_power(u.owner);
            if (!u.regen_blocked && !unpowered) {
                const float next = shield_regen_step(u.shield, amount, unit_max_shield(u), lockout, cfg_.depleted_shield_regen_cap);
                if (has_energy_pool(u) && next > u.shield) {
                    // S9 shield exchange (0x56be20): x points cost x * EnergyToShieldExchangeRate, all or nothing.
                    const float cost = (next - u.shield) * cfg_.energy_to_shield_exchange_rate;
                    if (u.energy >= cost) {
                        u.energy = u.energy - cost;
                        u.shield = next;
                    }
                } else {
                    u.shield = next;
                }
            }
        }
    }
    // S9 energy pool regeneration (EnergyPoolBehavior 0x56bfd0 / 0x56c030) [H].
    if (has_energy_pool(u)) {
        u.energy_countdown -= 1;
        if (u.energy_countdown <= 0) {
            u.energy_countdown = std::max<int32_t>(
                1, shield_regen_interval_frames(cfg_.fps, mult(u, ModKind::EnergyRegenInterval), cfg_.energy_recharge_interval_secs));
            if (ud.requires_base_power && !has_base_power(u.owner)) {  // OI abilities: the base-power list (§7.3)
                u.energy = 0.0f;
            } else if (!u.regen_blocked) {
                float e = u.energy + ud.energy_refresh_rate * mult(u, ModKind::EnergyRegen);
                const float mx = unit_max_energy(u);
                if (e < 0.0f) e = 0.0f;
                if (e > mx) e = mx;
                u.energy = e;
            }
        }
    }
}

float CombatSystem::unit_max_energy(const Unit& u) const {
    return apply_bonus(desc_of(u).energy_capacity, mod_r(u, ModList::MaxEnergy));  // 0x372320
}

void CombatSystem::burning_service(Unit& u, uint32_t frame) {
    // BurningBehavior (0x6241b0): Take_Damage(kind 7, Burning_Damage_Per_Second) [H] once per service; OI abilities:
    // serviced every 30 frames (vtable +0x28 = 0x1e, TACTICAL_GAPS §1.8 [H]); burns until death [M].
    if (!u.burning || (u.flags & kUnitDead)) return;
    if (static_cast<int32_t>(frame) < u.burn_countdown) return;  // burn_countdown = due frame
    u.burn_countdown = static_cast<int32_t>(frame) + kBurningInterval;
    DamageRequest r;
    r.victim = u.id;
    r.amount = desc_of(u).burning_damage_per_second;
    r.kind = kKindFire;
    take_damage(r, frame);
}

void CombatSystem::infect(Unit& v, int32_t desc, int32_t player, ObjectId source, uint32_t frame) {
    // cu8f t06 (CU8F_T04_T06_RE §3.5 [H]; projectile hit 0x4cb280 / spread 0x636ae0): attach INFECTION (behaviour factory
    // 0x4c3520) to a victim without it, or resume it (vfunc +0xb8: counter - 1, floored at 0); then the victim's pack
    // (obj +0x218) takes the infector's data: frames left = int(FPS x Duration_Of_Infection) (full), its parameters (the
    // infector type's InfectionDesc: mask, distance, DPS, Seconds_To_Infect), the infecting player and source.
    (void)frame;
    if (v.flags & kUnitDead) return;
    if (!v.infect_attached) {
        v.infect_attached = 1;
        v.infect_suspend = 0;
    } else if (v.infect_suspend != 0) {
        --v.infect_suspend;
    }
    v.infect_desc = desc >= 0 && static_cast<size_t>(desc) < data_->units.size() ? desc : -1;
    v.infect_player = player;
    v.infect_source = source;
    v.infect_left = 0;
    if (v.infect_desc >= 0)
        v.infect_left = static_cast<int32_t>(static_cast<float>(cfg_.fps) *
                                             data_->units[static_cast<size_t>(v.infect_desc)].infection.duration_secs);
    if (v.infect_suspend == 0) v.flags |= kUnitInfected;
}

void CombatSystem::infection_service(Unit& u, uint32_t frame, SimRng& rng) {
    // INFECTION service (vf6 0x636e80, interval 1), cu8f t06 (CU8F_T04_T06_RE §3.5 [H]). cu8g t05: a parameterless
    // behaviour (flag only: spawned / set infected, no infector pack) is serviced too, as the pack it has: frames left 0
    // (no Duration_Of_Infection written; the pack ctor's value [L] taken as 0), so its first service makes the frame % 60
    // draw like any other and then suspends it (expiry) before any damage or spread.
    if (!u.infect_attached || u.infect_suspend != 0 || (u.flags & kUnitDead)) return;
    if (u.flags & kUnitGarrisoned) return;  // nothing while garrisoned (+0x3a7)
    // Coughing SFX: on frames with frame % 60 == 0 one synced Get_Int(0, 4), the sound on 0 (presentation).
    if (frame % 60u == 0u) (void)rng.range_int(0, 4);
    // Frames left -= 1; at <= 0 the behaviour suspends itself (the cure: expiry) and its particle goes.
    if (--u.infect_left <= 0 || u.infect_desc < 0) {  // an empty pack: no DPS, no mask (nothing to damage or spread)
        if (u.infect_left <= 0) suspend_infection(u.id);
        return;
    }
    const InfectionDesc& in = data_->units[static_cast<size_t>(u.infect_desc)].infection;
    DamageRequest r;  // Take_Damage(kind 0x14, DPS / FPS) every frame
    r.victim = u.id;
    r.amount = in.damage_per_second / static_cast<float>(cfg_.fps);
    r.kind = kKindInfection;
    r.source = u.infect_source;
    take_damage(r, frame);
    // Spread 0x636ae0 (the mask's only reader): every player in index order (allies and the infector's own included), that
    // player's objects within Distance_To_Infect (XY) of the infected unit ([port] ascending id for the radius query).
    // A candidate counts when its TD +0x114 != NOW and it passes 0x6cc790: not this unit, not dying / removed, not a
    // projectile, has a locomotor, has a model (obj +0x2a0 non-null; cu8g t05: UnitDesc::has_model), not garrisoned, and
    // its type CategoryMask & Infection_CategoryMask != 0. Actively infected: its counter := 0. Else the consecutive-frame
    // counter (+0x118 += 1 when +0x114 = NOW - 1, else 0; +0x114 := NOW); at count >= FPS x Seconds_To_Infect it is infected with a copy
    // of this pack (duration full) and its counter := 0.
    const float r2 = in.distance_to_infect * in.distance_to_infect;
    const float need = static_cast<float>(cfg_.fps) * in.seconds_to_infect;
    const auto np = static_cast<int32_t>(players_.size());
    for (int32_t p = 0; p < np; ++p) {
        for (Unit& c : units_) {
            if (c.owner != p || &c == &u || c.infect_seen == frame) continue;
            if (c.flags & (kUnitDead | kUnitGarrisoned)) continue;
            const UnitDesc& cd = desc_of(c);
            if (!cd.has_locomotor || !cd.has_model || (cd.category_mask & in.category_mask) == 0) continue;
            if (distance2_xy(u.pos, c.pos) > r2) continue;
            if (c.infect_attached && c.infect_suspend == 0) {
                c.infect_count = 0;
                continue;
            }
            c.infect_count = c.infect_seen != 0xFFFFFFFFu && c.infect_seen + 1u == frame ? c.infect_count + 1 : 0;
            c.infect_seen = frame;
            if (static_cast<float>(c.infect_count) < need) continue;
            infect(c, u.infect_desc, u.infect_player, u.infect_source, frame);
            c.infect_count = 0;
        }
    }
}

void CombatSystem::converted_decay(Unit& u, uint32_t frame, SimRng& rng) {
    // GameObject::Update (0x3a6b80) step 7 (4.10) [H]; draw bounds +-d/10 [M].
    if (!(u.flags & kUnitConverted) || (u.flags & kUnitDead)) return;
    const float t = desc_of(u).converted_die_time;
    if (!(t > 0.0f)) return;
    const float d = unit_max_hull(u) / t / static_cast<float>(cfg_.fps);
    const float draw = rng.range_float(-(d / 10.0f), d / 10.0f);  // one synced Get_Float per frame
    if (d + draw > 0.0f) {
        DamageResult tmp;
        hull_damage(u, d + draw, kNoObject, tmp, frame, kKindConvertedDecay);
    }
}

float CombatSystem::projectile_turn_rate(const ProjectileDesc& pd) const {
    // 0x372560 (rate <= 0 path of Turn_Toward): Max_Rate_Of_Turn x Object_Max_Speed_Multiplier_{Space,Land} x 1.0.
    // [port] the type flag (+0x790 byte 1 bit 0) that skips the multiplier is not read.
    return pd.max_rate_of_turn * (cfg_.mode == GameMode::Space ? cfg_.turn_multiplier_space : cfg_.turn_multiplier_land);
}

bool CombatSystem::shield_away(const Projectile& p, Vec3& away) const {
    // Missile_Shield_Deflect_Test (0x4c6e30), S9_DEATH_CLONE_SHIELDS.md §2.3 [H]: emitters not allied with the
    // projectile's owner, radius = the ability system's qualifying radius (passive / ability 32 / jamming 63); the first
    // (ascending id) with |p - e|^2 < r^2 gives the outward direction (+x when p = e).
    for (const uint32_t idx : emitters_) {
        const Unit& e = units_[idx];
        if (e.flags & kUnitDead) continue;
        if (!is_enemy(e.owner, p.owner)) continue;
        const float r = e.emitter_radius;
        if (!(r > 0.0f)) continue;
        const Vec3 d = p.pos - e.pos;
        if (dot(d, d) < r * r) {
            away = dot(d, d) > 0.0f ? d : Vec3{1.0f, 0.0f, 0.0f};
            return true;
        }
    }
    return false;
}

// ---- refixf t01: spline rockets (S9_DEATH_CLONE_SHIELDS.md §2.4) -------------------------------------------------------

bool CombatSystem::spline_rocket(const ProjectileDesc& pd) const {
    return pd.category == ProjectileCategory::Rocket || pd.category == ProjectileCategory::MptlRocket;
}

void CombatSystem::add_rocket(const Projectile& p, Vec3 fire_at, Vec3 launch) {
    // Spawn (§2.4.10 step 1): FireAtPosition, launch point, s = 0, no path yet; the budget is set by the initial plan.
    Rocket rk;
    rk.serial = p.serial;
    rk.fire_at = fire_at;
    rk.launch = launch;
    rk.budget = data_->projectiles[p.desc].max_flight_distance;
    rockets_.push_back(std::move(rk));  // serials only grow: rockets_ stays in serial order
}

CombatSystem::Rocket* CombatSystem::find_rocket(uint32_t serial) {
    const auto it = std::lower_bound(rockets_.begin(), rockets_.end(), serial,
                                     [](const Rocket& r, uint32_t v) { return r.serial < v; });
    return (it == rockets_.end() || it->serial != serial) ? nullptr : &*it;
}

const CombatSystem::Rocket* CombatSystem::find_rocket(uint32_t serial) const {
    const auto it = std::lower_bound(rockets_.begin(), rockets_.end(), serial,
                                     [](const Rocket& r, uint32_t v) { return r.serial < v; });
    return (it == rockets_.end() || it->serial != serial) ? nullptr : &*it;
}

bool CombatSystem::rocket(size_t i, RocketView& out) const {
    if (i >= projectiles_.size() || !projectiles_[i].rocket) return false;
    const Rocket* rk = find_rocket(projectiles_[i].serial);
    if (!rk) return false;
    out.planned = rk->planned != 0;
    out.s = rk->s;
    out.budget = rk->budget;
    out.fire_at = rk->fire_at;
    out.launch = rk->launch;
    out.last_velocity = rk->last_vel;
    out.path = &rk->path;
    return true;
}

void CombatSystem::expire_projectile(uint32_t i, uint32_t frame) {
    Projectile& p = projectiles_[i];
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    ++stats_.expired;
    p.alive = 0;
    const Projectile pc = p;  // copy: the detonation may spawn projectiles
    HitEvent he;  // S10-int (R4): lifetime detonation particle
    he.shooter = pc.shooter;
    he.owner = pc.owner;
    he.projectile_desc = pc.desc;
    he.position = pc.pos;
    hit_events_.push_back(he);
    if (pd.blast_area_damage > 0.0f && pd.blast_area_range > 0.0f) area_damage(pc, pc.pos, kNoObject, frame);
    detonation_effects(pc, pc.pos, kNoObject, frame);
}

void CombatSystem::rocket_step(uint32_t i, uint32_t frame, SimRng& rng) {
    // One ROCKET / MPTL_ROCKET flight step (0x4cfc40, S9_DEATH_CLONE_SHIELDS.md §2.4.2) [H unless tagged]. Serial commit
    // phase in projectile serial order, so the plan / re-plan draws come at this projectile's place in the stream, before
    // its hit draws (the original re-plans before the sweep). No reference into rockets_ / projectiles_ is used after a
    // call that can spawn (hit processing, expiry).
    Rocket* rk = find_rocket(projectiles_[i].serial);
    if (!rk) {  // [port] cannot happen (spawn / read keep rockets_ in step with the projectiles)
        projectiles_[i].alive = 0;
        return;
    }
    const ProjectileDesc& pd = data_->projectiles[projectiles_[i].desc];
    RocketParams rp;
    rp.curve_distance = pd.rocket_curve_distance;
    rp.curve_offset = pd.rocket_curve_offset;
    rp.straight_distance = pd.rocket_straight_distance;
    rp.max_flight_distance = pd.max_flight_distance;
    rp.target_on_terrain = pd.target_point_on_terrain;
    rp.explode_at_target = pd.explode_when_reached_target_radius;
    RocketContext ctx;
    ctx.params = &rp;
    ctx.terrain = &rocket_terrain_;
    ctx.rng = &rng;
    ctx.launch = rk->launch;
    const bool land = cfg_.mode == GameMode::Land;  // mode 1 = land builders, mode 2 = space
    const Vec3 P = projectiles_[i].pos;
    // 1. No path yet: the initial plan (first flight step, before the facing turns). [port] FireAtPosition is always set.
    const bool fresh = rk->planned == 0;
    if (fresh) {
        if (land) rocket_plan_land(rk->path, P, rk->fire_at, projectiles_[i].dir, rk->budget, ctx);
        else rocket_plan_space(rk->path, P, rk->fire_at, projectiles_[i].dir, rk->budget, ctx);
        rk->planned = 1;
        rk->s = 0.0f;
    }
    // 2. s += speed, N = sample(s) (phase E sampled the same s + speed for planned rockets). Past the path's end the
    //    rocket expires where it is (forced expiry: detonation at P).
    rk->s = rk->s + projectiles_[i].speed;
    Vec3 N;
    bool ok = false;
    if (fresh) {
        ok = rk->path.sample(rk->s, N);
    } else {
        ok = rocket_next_[i].ok != 0;
        N = rocket_next_[i].n;
    }
    if (!ok) {
        expire_projectile(i, frame);
        return;
    }
    Projectile& p = projectiles_[i];
    // 3. Turn_Toward P -> N at the type turn rate: only the facing turns (the position comes from the spline) [M].
    p.dir = turn_toward(p.dir, N - P, projectile_turn_rate(pd));
    // 4. Every qualifying emitter (§2.3 rules, no jamming skip), in list order, with the same P and N: a crossing (N inside,
    //    P outside; squares summed y, x, z) re-plans. A second crossing in the same step re-plans again from s = 0.
    for (const uint32_t idx : emitters_) {
        const Unit& e = units_[idx];
        if (e.flags & kUnitDead) continue;
        if (!is_enemy(e.owner, p.owner)) continue;
        const float R = e.emitter_radius;
        if (!(R > 0.0f)) continue;
        const Vec3 dn = N - e.pos, dp = P - e.pos;
        const float n2 = (dn.y * dn.y + dn.x * dn.x) + dn.z * dn.z;
        const float p2 = (dp.y * dp.y + dp.x * dp.x) + dp.z * dp.z;
        const float R2 = R * R;
        if (n2 <= R2 && R2 <= p2) {
            if (land) rocket_replan_land(rk->path, P, e.pos, rk->last_vel, rk->s, rk->budget, ctx);
            else rocket_replan_space(rk->path, P, e.pos, rk->last_vel, rk->s, rk->budget, ctx);
        }
    }
    // 5. Sweep P -> N (on a re-plan step still the OLD path's N), hit processing, else move to N.
    const Vec3 seg = N - P;
    SweepHit hit = fresh ? sweep_segment(p, seg, serial_ctx_) : sweep_[i];
    if (hit.id != kNoObject && !live(unit(hit.id))) {  // victim died earlier in this phase
        ++stats_.resweeps;
        hit = sweep_segment(p, seg, serial_ctx_);
    }
    if (hit.id != kNoObject) {
        Unit* v = unit(hit.id);
        p.alive = 0;
        Projectile pc = p;
        process_hit(pc, *v, hit.t, frame, rng, &seg, hit.mesh_hp);
        return;
    }
    // LastKnownVelocity (+0x98) = the move's displacement x the frame-time float 0x140b0a344 [M: 1 / FPS].
    rk->last_vel = seg * (1.0f / static_cast<float>(cfg_.fps));
    p.pos = N;
    p.travelled = p.travelled + p.speed;  // DistanceTravelled += speed
    // 6. End of flight (0x4c7050): Explode_When_Reached_Target_Radius and s >= budget, or the travel / lifetime rule of
    //    every projectile.
    bool expire = pd.explode_when_reached_target_radius && rk->s >= rk->budget;
    if (!expire)
        expire = p.max_travel > 0.0f
                     ? p.travelled >= p.max_travel
                     : static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps) > pd.max_lifetime;
    if (expire) expire_projectile(i, frame);
}

// ---- cu8k t01: the thrown saber (CU8F_T01_T02_T07_RE.md §2.7 item 3) ----

uint32_t CombatSystem::launch_saber(const SaberLaunch& l, uint32_t frame) {
    if (l.projectile >= data_->projectiles.size()) return 0;
    const ProjectileDesc& pd = data_->projectiles[l.projectile];
    Projectile p;
    p.serial = next_serial_++;
    p.desc = l.projectile;
    p.shooter = l.thrower;
    p.owner = l.owner;
    p.target = l.target;
    p.pos = l.from;
    const Vec3 d = l.fire_at - l.from;
    const float dist = dmath::sqrt(dot(d, d));
    p.dir = dist > 0.0f ? d * (1.0f / dist) : Vec3{1.0f, 0.0f, 0.0f};
    p.speed = pd.max_speed;  // 0x370f00(type, 0): Max_Speed, no mode multiplier for category 8
    p.spawn_frame = frame;
    p.damage = pd.damage;
    p.damage_type = pd.damage_type;
    p.kind = pd.damage_kind;
    // Pack +0x6c: the max-travel override (Projectile_Max_Flight_Distance) + the target's radius (0x397780) + |dz|.
    const Unit* t = unit(l.target);
    const float radius = t ? desc_of(*t).radius : 0.0f;
    p.max_travel = pd.max_flight_distance + radius + abs_f(l.fire_at.z - l.from.z);
    p.rocket = 2;
    Rocket rk;
    rk.serial = p.serial;
    rk.fire_at = l.fire_at;
    rk.launch = l.from;
    rk.budget = pd.max_flight_distance;  // pack +0x54 := 250 (read only with Explode_When_Reached_Target_Radius)
    rk.thrower = l.thrower;
    rockets_.push_back(std::move(rk));  // serials only grow: rockets_ stays in serial order
    projectiles_.push_back(p);
    ++stats_.shots;
    return p.serial;
}

bool CombatSystem::saber_position(uint32_t serial, Vec3& out) const {
    const auto it = std::lower_bound(projectiles_.begin(), projectiles_.end(), serial,
                                     [](const Projectile& p, uint32_t v) { return p.serial < v; });
    if (it == projectiles_.end() || it->serial != serial || !it->alive || it->rocket != 2) return false;
    out = it->pos;
    return true;
}

void CombatSystem::saber_step(uint32_t i, uint32_t frame, SimRng& rng) {
    // LIGHT_SABER step 0x4cf460 (§2.7 item 3) [H unless tagged]. Serial commit phase, projectile serial order: the plan's
    // jitter draws come at this projectile's place in the stream on its first step.
    Rocket* rk = find_rocket(projectiles_[i].serial);
    if (!rk) {  // [port] cannot happen (spawn / read keep rockets_ in step with the projectiles)
        projectiles_[i].alive = 0;
        return;
    }
    const ProjectileDesc& pd = data_->projectiles[projectiles_[i].desc];
    RocketParams rp;
    rp.curve_distance = pd.rocket_curve_distance;
    rp.curve_offset = pd.rocket_curve_offset;
    rp.max_flight_distance = pd.max_flight_distance;
    RocketContext ctx;
    ctx.params = &rp;
    ctx.terrain = &rocket_terrain_;
    ctx.rng = &rng;
    ctx.launch = rk->launch;
    const Vec3 P = projectiles_[i].pos;
    if (rk->planned == 0) {
        // Path plan 0x4c8910 (first step): P0 = the spawn position; leg 1 P0 -> T (amp 1.0, length scale 1.1, land mode,
        // no ground end, plain end), leg 2 T -> L (amp 0.3, 1.1, land mode, ground end, plain end) with L = the thrower's
        // position + Ranged_Target_Z_Adjust at this step [port: the spawn point when the thrower is gone]; Build(10).
        const Unit* th = unit(rk->thrower);
        const Vec3 L = live(th) ? lob_target_point(*th) : rk->launch;
        rk->path.clear();
        rk->path.append(P);
        rocket_leg(rk->path, P, rk->fire_at, 1.0f, 1.1f, 1, false, true, ctx);
        rocket_leg(rk->path, rk->fire_at, L, 0.3f, 1.1f, 1, true, true, ctx);
        rk->path.build(10);
        rk->planned = 1;
        rk->s = 0.0f;
    }
    rk->s = rk->s + projectiles_[i].speed;
    Vec3 N;
    if (!rk->path.sample(rk->s, N)) {  // past the path's end: expire where it is
        expire_projectile(i, frame);
        return;
    }
    Projectile& p = projectiles_[i];
    p.dir = turn_toward(p.dir, N - P, projectile_turn_rate(pd));
    rk->last_vel = (N - P) * (1.0f / static_cast<float>(cfg_.fps));
    p.pos = N;
    p.travelled = p.travelled + p.speed;
    const bool detonate = p.max_travel > 0.0f && p.travelled >= p.max_travel;
    // Area damage every step: every live unit of a player hostile to the saber's owner within Projectile_Stun_Radius (3D)
    // takes Projectile_Damage of the saber's damage type, the saber's thrower as the source; no once-per-victim memory.
    // [port] ascending unit id (the original's 0x2a9ff0 query order is not read); [M] also on the detonation step.
    const float R = pd.stun_radius;
    if (R > 0.0f && p.damage > 0.0f) {
        const Projectile pc = p;  // damage may kill: no reference into projectiles_ / units_ is held across it
        const float R2 = R * R;
        std::vector<ObjectId> victims;
        for (const Unit& v : units_) {
            if (!live(&v) || !is_enemy(pc.owner, v.owner)) continue;
            const Vec3 dv = v.pos - pc.pos;
            if (dot(dv, dv) <= R2) victims.push_back(v.id);
        }
        for (const ObjectId vid : victims) {
            if (!live(unit(vid))) continue;
            DamageRequest r;
            r.victim = vid;
            r.amount = pc.damage;
            r.damage_type = pc.damage_type;
            r.kind = pc.kind;
            r.source = pc.shooter;
            r.source_is_projectile = true;
            r.projectile_desc = static_cast<int32_t>(pc.desc);
            apply_or_queue(r, 0.0f, frame);
        }
    }
    if (detonate) expire_projectile(i, frame);
}

// ---- S8 t04: grenade flight (docs/re/LOB_SOLVER.md §6, ProjectileBehavior category 3 0x4ceea0) ----

Vec3 CombatSystem::lob_next(const Projectile& p, uint32_t frame) const {
    // §6.1: evaluated from scratch each step; t = frames since the creation frame (the first step, the frame after the
    // spawn, has t = 1).
    return lob_position(p.launch, p.lob_dir, p.speed, p.gravity, static_cast<float>(frame - p.spawn_frame));
}

bool CombatSystem::ground_hit(Vec3 a, Vec3 b, GroundHit& out) const {
    // Ground test 0x4d0630 (§6.2 step 3) -> 0x135840 (§6.6, S8 t06): the segment a -> b against the heightmap cells'
    // triangles and the water plane, two-sided, nearest t in (0, 1) (rocket.hpp ground_segment). [port] No grid from
    // the host: a plane at z 0.
    return ground_segment(rocket_terrain_.grid, a, b, out);
}

bool CombatSystem::straight_ground_test(const Projectile& p, Vec3 delta) const {
    // A1j t03 (TACTICAL_COMBAT §3.4 step 1, 0x4d0630) [H]: land mode only. For Projectile_Category DEFAULT (and an unknown
    // category, -1; the port parses those as DEFAULT) the test is skipped while the target point is still ahead: the sign
    // of (target point - position) equals the sign of the step on all three axes (sign(0) = +). Target point = the
    // targeted hardpoint's position (0x396ce0) with a hardpoint index, else position + Ranged_Target_Z_Adjust (0x398440).
    // No target: tested. Every other category (LASER 7 included) is tested on every step.
    if (cfg_.mode != GameMode::Land) return false;
    if (data_->projectiles[p.desc].category != ProjectileCategory::Default) return true;
    const Unit* t = unit(p.target);
    if (!live(t)) return true;  // [port] a dead target stands for the cleared pack reference
    Vec3 tp;
    if (p.target_hp >= 0 && static_cast<uint32_t>(p.target_hp) < t->hp_count)
        tp = hp_world(*t, data_->hardpoints[hps_[t->hp_begin + static_cast<uint32_t>(p.target_hp)].desc]);
    else
        tp = lob_target_point(*t);
    auto sgn = [](float v) { return v >= 0.0f; };
    return !(sgn(tp.x - p.pos.x) == sgn(delta.x) && sgn(tp.y - p.pos.y) == sgn(delta.y) && sgn(tp.z - p.pos.z) == sgn(delta.z));
}

Vec3 CombatSystem::stuck_local(const Unit& v, Vec3 world) const {
    // S8 t06 (§6.4, LOB_SOLVER §8 item 18): the stuck point in the parent's frame (yaw of its facing, as local_to_world),
    // so it turns with the parent. [port] Retail carries it on the hit bone (bone animation and pitch / roll ignored).
    float fx = v.facing.x, fy = v.facing.y;
    const float len2 = fx * fx + fy * fy;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    const Vec3 d = world - v.pos;
    return Vec3{d.x * fx + d.y * fy, d.y * fx - d.x * fy, d.z};
}

uint32_t CombatSystem::bomb_frame(float seconds, uint32_t armed_frame) const {
    // BOMB arm 0x58f030 (§6.4): detonation frame = int((float)FPS x seconds + (float)frame + 0.5); it fires on the first
    // service with frame >= it. [port] Negative / huge results clamp to 0 / UINT32_MAX.
    const float f = static_cast<float>(cfg_.fps) * seconds + static_cast<float>(armed_frame) + 0.5f;
    if (!(f > 0.0f)) return 0;
    if (!(f < 4294967040.0f)) return 0xFFFFFFFFu;
    return static_cast<uint32_t>(static_cast<int64_t>(f));
}

void CombatSystem::lob_step(uint32_t i, uint32_t frame, SimRng& rng) {
    // §6.2 in the serial commit phase (projectile serial order). No reference into projectiles_ is used after a call that
    // can spawn (hit processing, detonation).
    Projectile& p = projectiles_[i];
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const float age = static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps);
    // BOMB timer (Grenade_Explode_Timer, armed at the throw = the spawn frame; S8 t06, §6.4): fires on the first frame >=
    // int(FPS x secs + spawn frame + 0.5), wherever the grenade is (in flight, stuck or resting). Remote bombs carry none.
    const bool fuse_out = p.fuse >= 0.0f && frame >= bomb_frame(p.fuse, p.spawn_frame);
    if (p.stuck) {
        // §6.4: flight stopped (vfunc +0xc0): no lifetime any more, only the BOMB timer or the remote detonation. An
        // attached grenade rides on its object: S8 t06 the BOMB service puts it at the stuck point carried by the
        // parent, here the offset kept in the parent's frame turned with its facing ([port] no bone animation).
        if (p.stuck_to != kNoObject)
            if (const Unit* v = unit(p.stuck_to); live(v)) p.pos = local_to_world(*v, p.stuck_offset);
        if (fuse_out) expire_projectile(i, frame);
        return;
    }
    // 1. N = N(t); 2. facing := the chord P -> N.
    const Vec3 P = p.pos;
    const Vec3 N = lob_next(p, frame);
    const Vec3 seg = N - P;
    const float l2 = dot(seg, seg);
    if (l2 > 0.0f) p.dir = seg * (1.0f / dmath::sqrt(l2));
    // 3. sweep: the object hit of phase E (re-swept when its victim died earlier in this phase) and the land-only ground
    //    test; the object hit wins when its fraction <= the ground fraction.
    SweepHit hit = sweep_[i];
    if (hit.id != kNoObject && !live(unit(hit.id))) {
        ++stats_.resweeps;
        hit = sweep_segment(p, seg, serial_ctx_);
    }
    GroundHit gh;
    const bool ground = cfg_.mode == GameMode::Land && ground_hit(P, N, gh);
    const float tg = gh.t;
    const bool object = hit.id != kNoObject && (!ground || hit.t <= tg);
    if (pd.sticks_on_collision && (object || ground)) {
        // 5. §6.4: no damage; attach to the struck object or rest on the ground, then wait (BOMB timer / remote).
        p.stuck = 1;
        p.speed = 0.0f;
        if (object) {
            p.pos = P + seg * hit.t;
            p.stuck_to = hit.id;
            p.stuck_offset = stuck_local(*unit(hit.id), p.pos);  // S8 t06: in the parent's frame
        } else {
            p.pos = gh.point;
            p.stuck_to = kNoObject;
        }
        if (fuse_out) expire_projectile(i, frame);
        return;
    }
    if (object) {  // 4. Process_Hit (direct + area damage, effects)
        Unit* v = unit(hit.id);
        p.alive = 0;
        Projectile pc = p;
        process_hit(pc, *v, hit.t, frame, rng, &seg, hit.mesh_hp);
        return;
    }
    if (ground) {  // 4. On_Detonate 0x4cb070 at the ground hit point: no direct damage, area damage + effects there
        p.pos = gh.point;
        expire_projectile(i, frame);
        return;
    }
    // Move_And_Expire 0x4c7050 (§6.5): DistanceTravelled += s; the lifetime rule replaces the travel budget when the
    // budget is 0 or the type has a Max_Lifetime > 0 (every grenade is category 3).
    p.pos = N;
    p.travelled = p.travelled + p.speed;
    const bool lifetime_rule = !(p.max_travel > 0.0f) || pd.max_lifetime > 0.0f;
    const bool expire = fuse_out || (lifetime_rule ? age > pd.max_lifetime : p.travelled >= p.max_travel);
    if (expire) expire_projectile(i, frame);
}

// ================================================================================================
// tick
// ================================================================================================

QueryContext* CombatSystem::acquire_ctx() {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    if (ctx_free_.empty()) {
        ctx_all_.push_back(std::make_unique<QueryContext>());
        ctx_all_.back()->reserve(std::max<size_t>(cand_reserve_, 64));
        ctx_free_.reserve(ctx_all_.size());
        return ctx_all_.back().get();
    }
    QueryContext* c = ctx_free_.back();
    ctx_free_.pop_back();
    return c;
}

void CombatSystem::release_ctx(QueryContext* c) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    ctx_free_.push_back(c);
}

void CombatSystem::tick(uint32_t frame, const TreeSet& trees, SimRng& rng, const ParallelFor& pf) {
    frame_ = frame;
    trees_ = &trees;
    last_trees_ = &trees;
    tick_rng_ = &rng;
    deaths_.clear();
    hp_events_.clear();
    smoke_events_.clear();
    damage_events_.clear();
    hit_events_.clear();  // S10-int
    shot_events_.clear();  // OI fog
    lob_events_.clear();   // S8 t30
    attack_events_.clear();  // s78f t08 (signal 0x1E)
    survivor_events_.clear();  // OI abilities
    const auto n = static_cast<uint32_t>(units_.size());

    // A. modifiers
    for (ModifierSet& ps : player_mods_) ps.purge_expired(frame);  // player-level purge [M: cadence]
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                Unit& u = units_[i];
                if (u.flags & kUnitDead) continue;
                u.mods.purge_expired(frame_);  // once per object update
                ensure_cache(u);
                const UnitAbilityDesc* ab[2] = {ability(u, 0), ability(u, 1)};
                const bool act[2] = {u.ability_active[0] != 0, u.ability_active[1] != 0};
                for (uint32_t k = 0; k < kModKindCount; ++k)
                    u.own_mult[k] = ability_product(ab, act, static_cast<ModKind>(k + 1));
                u.mult = u.own_mult;  // s78h t03: the team_leader too (the squad value is team_mod_multiplier)
            }
        };
        pf(n, cfg_.grain, fn);
    }
    // Team heads (serial). s78h t03: no squad rule here any more; a member (the team_leader stand-in included) keeps its own
    // Mod_Multiplier, and the container's squad value is team_mod_multiplier (TACTICAL_COMBAT §4.4).
    leaders_.clear();
    for (uint32_t i = 0; i < n; ++i)
        if (units_[i].team != 0 && units_[i].team_leader && !(units_[i].flags & kUnitDead)) leaders_.emplace_back(units_[i].team, i);
    // OI tactical team targeting: (team, team_leader id) for team_head() (read-only in the parallel gather phase).
    team_heads_.clear();
    for (const auto& [team, index] : leaders_) team_heads_.emplace_back(team, units_[index].id);
    std::sort(team_heads_.begin(), team_heads_.end());
    rebuild_team_members();  // cu8f t04a: team owners / container targets of the stickiness test
    // s78f t03: the heads of ground teams (TeamTargeting services in phase D), ascending id. A member keeps no target of
    // its own (TD +8 null, TACTICAL_COMBAT §3.1.1 "Team targeting copies"): one it picked before it became a member (its
    // head appeared later) is dropped here, since its scan no longer runs to replace it (s78f review).
    land_heads_.clear();
    if (cfg_.mode == GameMode::Land)
        for (uint32_t i = 0; i < n; ++i)
            if (!(units_[i].flags & kUnitDead))
                if (const Unit* h = land_team_lead(units_[i])) {  // s78i t03: a head with no member left too
                    land_heads_.push_back(h->id);
                    Unit& m = units_[i];
                    if (h != &m && m.auto_target != kNoObject) {
                        m.auto_target = kNoObject;
                        m.auto_hp = -1;
                        m.target_score = 0.0f;
                    }
                }
    std::sort(land_heads_.begin(), land_heads_.end());
    land_heads_.erase(std::unique(land_heads_.begin(), land_heads_.end()), land_heads_.end());
    // s78h t06 (TACTICAL_COMBAT §3.1.1 "When the container is serviced"): each land team's container slot = its highest
    // live unit index (members and head) in phase A; phase D's descending walk reaches it before any of the team's units.
    land_teams_.clear();
    if (cfg_.mode == GameMode::Land)
        for (uint32_t i = 0; i < n; ++i) {
            const Unit& m = units_[i];
            if (m.flags & kUnitDead) continue;
            const Unit* h = land_team_lead(m);  // s78i t03
            if (!h) continue;
            const auto hi = static_cast<uint32_t>(h - units_.data());
            const uint32_t top = hi > i ? hi : i;
            auto it = std::find_if(land_teams_.begin(), land_teams_.end(), [&](const LandTeamSlot& t) { return t.team == m.team; });
            if (it == land_teams_.end()) {
                land_teams_.push_back(LandTeamSlot{top, m.team, 0});
            } else if (top > it->slot) {
                it->slot = top;
            }
        }
    std::sort(land_teams_.begin(), land_teams_.end(), [](const LandTeamSlot& a, const LandTeamSlot& b) {
        return a.slot != b.slot ? a.slot > b.slot : a.team < b.team;
    });
    if (hooks_) hooks_->begin_tick(frame);

    // B1. delayed damage (queue order)
    if (!delayed_.empty()) {
        size_t w = 0;
        const size_t count = delayed_.size();
        for (size_t i = 0; i < count; ++i) {
            Delayed d = delayed_[i];
            d.frames -= 1;
            if (d.frames > 0) {
                delayed_[w++] = d;
                continue;
            }
            DamageRequest r;
            r.victim = d.victim;
            r.amount = d.amount;
            r.damage_type = d.damage_type;
            r.kind = d.kind;
            r.source = d.source;
            r.source_is_projectile = d.source_is_projectile != 0;
            r.projectile_desc = d.projectile_desc;
            r.hardpoint = d.hardpoint;
            take_damage(r, frame);
        }
        delayed_.resize(w);
    }
    // B2. shields and hull/hardpoint coupling (per unit)
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) upkeep_unit(units_[i], frame_);
        };
        pf(n, cfg_.grain, fn);
    }
    // C. target candidates (parallel, no draws)
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            QueryContext* c = acquire_ctx();
            for (uint32_t i = b; i < e; ++i) gather(i, *c);
            release_ctx(c);
        };
        pf(n, cfg_.grain, fn);
    }
    // D. decide + fire, newest unit first (serial: every synced draw of the tick happens here or in F).
    //    S9 per-unit order (OBJECT_RUNTIME 4.4 behaviours, ABILITIES_HARDPOINTS.md 6.2): ability
    //    countdown (hook) -> burning -> targeting -> pending ability + special abilities (hook) ->
    //    hardpoints / weapons -> converted-unit decay.
    if (hooks_) hooks_->before_units(frame, rng);
    size_t next_team = 0;  // s78h t06: land_teams_ cursor (slot descending)
    for (uint32_t k = n; k-- > 0;) {
        // s78h t06: a land team's TeamTargeting runs once, at its container slot just before its highest-index unit, even
        // when that unit died earlier in the walk, while the team has a live head (after an in-tick hand-over: the heir).
        for (; next_team < land_teams_.size() && land_teams_[next_team].slot >= k; ++next_team) {
            LandTeamSlot& ts = land_teams_[next_team];
            if (ts.serviced) continue;
            ts.serviced = 1;
            Unit* h = team_head_of(ts.team);
            if (!h) continue;
            const auto hi = static_cast<uint32_t>(h - units_.data());
            if (hi >= n) continue;  // spawned this tick: phase C has no candidates for it [port]
            h->acquired = 0;
            if (!targeting_idle(*h, frame)) {
                decide(*h, cands_[hi], frame, rng);
                team_targeting_service(*h, frame, rng);
            }
        }
        Unit& u = units_[k];
        if (u.flags & kUnitDead) continue;
        if (hooks_) {
            hooks_->unit_begin(u.id, frame, rng);
            if (u.flags & kUnitDead) continue;
        }
        burning_service(u, frame);
        if (u.flags & kUnitDead) continue;
        infection_service(u, frame, rng);  // refix t02 [M: slot next to the other damage-over-time behaviour]
        if (u.flags & kUnitDead) continue;
        // OI abilities: a stunned unit's targeting / turret behaviours are suspended (0x647290); its STUNNABLE service
        // at the end frame releases it [inferred]. Requires_Base_Power units do not act without base power (0x632270).
        if (u.stun_end != 0 && frame >= u.stun_end) {
            u.stun_end = 0;
            if (hooks_) hooks_->unit_effect(u.id, UnitEffect::StunEnded, u.owner, u.pos, frame);
        }
        const bool idle = u.stun_end > frame || (desc_of(u).requires_base_power && !has_base_power(u.owner));
        // S7 t14: on the jetpack (MOVEMENT §3.1.1) TARGETING and WEAPON are suspended; S8 t12 (MOVEMENT §3.2): the
        // hardpoints are not (update_weapons' `suspended`).
        const bool suspended = (u.flags & kUnitWeaponsSuspended) != 0;
        // S7 t15 A2: an idle unit's weapons are not serviced; the WeaponBehavior recharge still elapses (fire_unit_weapon
        // subtracts the frames since its last service when it runs again).
        // s78h t06: a land team head's TARGETING (TeamTargeting) ran at its team's container slot above (its acquired flag
        // was reset there); a member has none of its own (its container's service writes its state).
        const bool land_head = std::binary_search(land_heads_.begin(), land_heads_.end(), u.id);
        if (!land_head) u.acquired = 0;
        if (!idle && !suspended && !(u.flags & kUnitTargetingSuspended)) {  // S8 t25
            // s78f t03: TARGETING = the scan, then the state service (TACTICAL_COMBAT §3.1.1 "Targeting states").
            if (!land_head && !land_team_head(u)) {
                decide(u, cands_[k], frame, rng);
                targeting_service(u, frame, rng);
            }
        }
        if (hooks_) {
            hooks_->unit_after_targeting(u.id, frame, rng);
            if (u.flags & kUnitDead) continue;
        }
        service_repairs(u, frame);  // OI abilities: hardpoint update 0x387010 -> repair 0x387170, before the weapons
        if (!idle) update_weapons(u, frame, rng, suspended);
        converted_decay(u, frame, rng);
    }
    // E. projectile sweeps (parallel)
    const auto np = static_cast<uint32_t>(projectiles_.size());
    sweep_.resize(np);
    rocket_next_.resize(np);  // refixf t01
    {
        const RangeFn fn = [this](uint32_t b, uint32_t e) {
            QueryContext* c = acquire_ctx();
            for (uint32_t i = b; i < e; ++i) sweep(i, *c);
            release_ctx(c);
        };
        pf(np, cfg_.grain, fn);
    }
    // F. commit in serial order. Hit processing can spawn projectiles (death projectiles), so no
    //    reference into projectiles_ is held across it: work on a copy, write back by index.
    emitters_.clear();  // missile-shield emitters, ascending id (S9 port choice, 6.3)
    for (uint32_t i = 0; i < n; ++i)  // OI abilities: registered (Add_Emitter) and qualifying (radius) emitters
        if (!(units_[i].flags & kUnitDead) && units_[i].emitter_radius > 0.0f && units_[i].emitter_entries > 0) emitters_.push_back(i);
    for (uint32_t i = 0; i < np; ++i) {
        if (!projectiles_[i].alive || projectiles_[i].spawn_frame >= frame) continue;
        if (projectiles_[i].rocket == 2) {  // cu8k t01: thrown saber (category 8 step 0x4cf460)
            saber_step(i, frame, rng);
            continue;
        }
        if (projectiles_[i].rocket) {  // refixf t01: spline rocket (0x4cfc40)
            rocket_step(i, frame, rng);
            continue;
        }
        if (projectiles_[i].lob) {  // S8 t04: grenade (0x4ceea0)
            lob_step(i, frame, rng);
            continue;
        }
        SweepHit hit = sweep_[i];
        if (hit.id != kNoObject && !live(unit(hit.id))) {  // victim died earlier in this phase
            ++stats_.resweeps;
            sweep(i, serial_ctx_);
            hit = sweep_[i];
        }
        // A1j t03: the object hit wins when its segment fraction <= the ground fraction; a ground hit detonates there
        // (On_Detonate 0x4cb070: no direct damage, area damage and effects at the ground point), as lob_step.
        if (hit.id != kNoObject && (!hit.ground || hit.t <= hit.gh.t)) {
            Unit* v = unit(hit.id);
            projectiles_[i].alive = 0;
            Projectile pc = projectiles_[i];
            process_hit(pc, *v, hit.t, frame, rng, nullptr, hit.mesh_hp);
            continue;
        }
        if (hit.ground) {
            projectiles_[i].pos = hit.gh.point;
            expire_projectile(i, frame);
            continue;
        }
        Projectile& p = projectiles_[i];
        const ProjectileDesc& pd = data_->projectiles[p.desc];
        // OI abilities (TACTICAL_GAPS §1.5 [H], disassembly 0x1404ce768): the MISSILE branch tests Is_Jammed on the
        // projectile's TARGET - no target or a jammed target -> straight flight.
        // S9 t05 (S9_DEATH_CLONE_SHIELDS.md §2.2 / §2.3): the flight modes steer by Turn_Toward (0x3abe40) limited to
        // Max_Rate_Of_Turn x the mode multiplier per step - homing toward the target, and in the straight / homing modes
        // away from a missile-shield emitter holding the projectile (that goal replaces the target for the step).
        // Rockets fly rocket_step (spline, re-plan on crossing); bombs, grenades and sabers are never shield-tested.
        using PC = ProjectileCategory;
        const PC cat = pd.category;
        const bool jammed = is_jammed(p.target, frame);
        const Unit* homing_target = nullptr;
        if ((pd.homing || cat == PC::Missile) && !jammed) {
            const Unit* t = unit(p.target);
            if (live(t)) homing_target = t;
        }
        bool steer = false;
        Vec3 goal;
        // ROCKET / MPTL_ROCKET never get here (rocket_step above, refixf t01).
        if (cat != PC::Bomb && cat != PC::Grenade && cat != PC::StaticBomb && cat != PC::LightSaber &&
                   cat != PC::ClusterBomb && !emitters_.empty()) {
            steer = shield_away(p, goal);
        }
        if (!steer && homing_target) {
            goal = homing_target->pos - p.pos;
            steer = true;
        }
        if (steer) p.dir = turn_toward(p.dir, goal, projectile_turn_rate(pd));
        p.pos = p.pos + p.dir * p.speed;
        p.travelled = p.travelled + p.speed;
        bool expire;
        if (p.fuse >= 0.0f) {  // OI abilities: armed bomb timer (S8 t06: the BOMB arm's frame rounding, §6.4)
            expire = frame >= bomb_frame(p.fuse, p.spawn_frame);
        } else {
            expire = p.max_travel > 0.0f
                         ? p.travelled >= p.max_travel
                         : static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps) > pd.max_lifetime;
        }
        if (expire) expire_projectile(i, frame);
    }
    // G. velocity history (OI tactical: two steps for the circular-lead fit), flush
    for (Unit& u : units_) {
        u.prev_pos = u.last_pos;
        u.last_pos = u.pos;
    }
    flush();
    trees_ = nullptr;
    tick_rng_ = nullptr;
}

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
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.id);
        h.u32(u.desc);
        h.i32(u.owner);
        h.u32(u.flags & ~kUnitHostMirrorFlags);  // S8 t10 / t11: the host mirrors are derived state
        h.f32(u.health);
        h.f32(u.shield);
        h.u8(u.shield_depleted);
        h.u32(u.shield_depleted_frame);
        h.i32(u.shield_regen_countdown);
        h.u32(u.next_scan);
        h.i32(u.auto_target);
        h.i32(u.ordered_target);
        h.f32(u.target_score);
        h.i32(u.weapon_cooldown);
        h.i32(u.weapon_pulses);
        h.u8(u.ability_active[0]);
        h.u8(u.ability_active[1]);
        h.u8(u.ever_hit);
        h.u32(u.last_hit_frame);
        for (uint32_t b = 0; b < 5; ++b) {
            h.f32(u.dmg_bucket[b]);
            h.u32(u.dmg_second[b]);
        }
        h.u32(u.hp_count);
        for (uint32_t i = 0; i < u.hp_count; ++i) {
            const Hardpoint& hp = hps_[u.hp_begin + i];
            h.u32(hp.desc);
            h.f32(hp.health);
            h.i32(hp.cooldown);
            h.i32(hp.pulses);
            h.u8(hp.destroyed);
            h.u8(hp.disabled);
            h.i32(hp.target);
            h.i32(hp.target_hp);
        }
    }
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
