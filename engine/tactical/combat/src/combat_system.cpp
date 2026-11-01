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
    leaders_.reserve(units);
    team_heads_.reserve(units);  // OI tactical
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
    u.damage_parent = s.damage_parent;
    u.flags = s.flags & ~(kUnitDead | kUnitRemoved);
    if (u.flags & kUnitInfected) u.infect_attached = 1;  // refix t02: spawned infected = the behaviour, no parameters
    u.pos = s.position;
    u.last_pos = s.position;
    u.prev_pos = s.position;  // OI tactical
    u.facing = s.facing;
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
        // WeaponBehavior reset at creation [L]: one synced Get_Int(0, 10).
        const Recharge r = weapon_reset(ud.weapon_recharge, ud.weapon_pulse_count, weapon_delay(nu, nullptr),
                                        fire_rate(nu), cfg_.fps, rng);
        nu.weapon_cooldown = r.frames;
        nu.weapon_pulses = r.pulses;
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
    u->ordered_target = target;
    u->ordered_hp = hardpoint;
    u->locked = 0;
    u->aim_target = kNoObject;  // refixf t06: the order re-aims the hardpoints
    u->aim_hp = -1;
    u->auto_target = kNoObject;  // an order drops the auto target (0x565e50)
    u->target_score = 0.0f;
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
    const float mn = desc_of(*a).min_attack_distance;
    if (!(mn > 0.0f)) return false;
    float d = 0.0f, E = 0.0f;
    range_geometry(*t, a->pos, d, E);
    return d <= mn + E;
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
    if (ud.weapon_projectile >= 0) {
        const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(fired_weapon_projectile(u))];
        if (pd.lobbed) return lob_range(pd.max_speed, pd.grenade_gravity > 0.0f ? pd.grenade_gravity : cfg_.grenade_gravity, pd.lob_mod);
    }
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

float CombatSystem::lob_gravity(const ProjectileDesc& pd) const {
    if (!pd.lobbed && pd.category != ProjectileCategory::Grenade) return 0.0f;
    const float g = (pd.grenade_gravity > 0.0f ? pd.grenade_gravity : cfg_.grenade_gravity) * pd.lob_mod;
    return g > 0.0f ? g : 0.0f;
}

float CombatSystem::target_extent(const Unit& t, Vec3 from) const {
    // TACTICAL_GAPS 3.1 slot 43 E (no LAND_OBSTACLE): |dot(target facing, line of sight)| > 0.7071 -> the X extent, else
    // the Y extent (Custom_Hard_X/YExtent x Scale_Factor). [port] Without a hard extent: the target radius (the sim has
    // no model bounding boxes). The +10 for a moving team and the melee redirect apply to team-container targets, which
    // the port never has (candidates are units / members); the LAND_OBSTACLE walk is range_geometry.
    const UnitDesc& td = desc_of(t);
    if (!(td.hard_x_extent > 0.0f) && !(td.hard_y_extent > 0.0f)) return td.radius;
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
    const float x = td.hard_x_extent > 0.0f ? td.hard_x_extent : td.radius;
    const float y = td.hard_y_extent > 0.0f ? td.hard_y_extent : td.radius;
    return abs_f(fx * dx + fy * dy) > 0.7071f ? x : y;
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

bool CombatSystem::sticky(const Unit& u, const Unit& t, uint32_t frame) const {
    // Is_Target_Sticky (0x569000) [M]: time-to-kill <= threshold. Health term: hull, or the sum of
    // destroyable hardpoint health for all-hardpoints types; + 0x396420 (unknown, shield here [L]).
    const float threshold = desc_of(u).stickiness_threshold;
    if (!(threshold > 0.0f)) return false;
    const float rate = damage_rate(t, frame);
    if (!(rate > 0.0f)) return false;
    float hp = t.health;
    if (desc_of(t).destroy_when_all_hardpoints_destroyed && t.hp_count) {
        hp = 0.0f;
        for (uint32_t i = 0; i < t.hp_count; ++i) {
            const Hardpoint& h = hps_[t.hp_begin + i];
            if (!h.destroyed && data_->hardpoints[h.desc].destroyable) hp = hp + h.health;
        }
    }
    return (hp + t.shield) / rate <= threshold;
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
    // HP_In_Fire_Cone (0x383ba0) [M]: aim point in the hardpoint's frame; |yaw| <= width/2 and
    // |pitch| <= height/2; turrets: |yaw| <= Turret_Rotate_Extent_Degrees. The fire-bone frame is
    // approximated by the unit's facing (yaw only) plus fire_yaw_deg [L].
    const HardpointDesc& hd = data_->hardpoints[h.desc];
    if (!hd.turret && hd.fire_cone_width >= 360.0f && hd.fire_cone_height >= 180.0f) return true;
    if (hd.turret && hd.turret_rotate_extent >= 180.0f) return true;
    const Vec3 o = hp_world(a, hd);
    const Vec3 d = aim - o;
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
    // the cell grid (the cell centre), E = sqrt(200). [port] the walk stops once it has passed the attacker.
    if (desc_of(t).land_obstacle && obstacle_cell_) {
        float dx = from.x - t.pos.x, dy = from.y - t.pos.y;
        const float l2 = dx * dx + dy * dy;
        if (!(l2 > 0.0f)) {  // zero offset counts as in range
            d = 0.0f;
            E = 0.0f;
            return;
        }
        const float len = dmath::sqrt(l2);
        dx = dx / len;
        dy = dy / len;
        Vec3 aim = t.pos;
        Vec3 centre = aim;
        for (float walked = 0.0f; obstacle_cell_(t.id, aim, centre) && walked <= len; walked = walked + 20.0f) {
            aim.x = aim.x + dx * 20.0f;
            aim.y = aim.y + dy * 20.0f;
        }
        obstacle_cell_(t.id, aim, centre);  // the final cell's centre
        d = distance_xy(from, centre);
        E = 14.142136f;  // sqrt(200)
        return;
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
    // hardpoint allowed to fire at the target.
    const Unit* head = team_head(a);
    auto has_weapon = [&](const Unit& s) {
        if (desc_of(s).weapon_projectile >= 0) return true;
        for (uint32_t i = 0; i < s.hp_count; ++i) {
            const Hardpoint& h = hps_[s.hp_begin + i];
            const HardpointDesc& hd = data_->hardpoints[h.desc];
            if (!hp_out(h) && is_weapon(hd.kind) && !(hd.fire_category_restrictions & td.category_mask)) return true;
        }
        return false;
    };
    if (!has_weapon(a) && !(head && has_weapon(*head))) return false;
    // 9: range acceptance (OI tactical, TACTICAL_GAPS 3.1): slot 44 "in max range" = XY distance <= R + E and NOT slot
    // 45 "inside min range" (d <= Targeting_Min_Attack_Distance + E, types with a min distance only); E = the target's
    // footprint half-extent along the line of sight. OR the target is the container's (squad head's) current target
    // (team targeting, filter 9 [H]).
    float d = 0.0f, E = 0.0f;
    range_geometry(t, a.pos, d, E);  // OI tactical: + the LAND_OBSTACLE aim walk
    const bool head_target = head && current_target(head->id) == t.id;
    if (!(d <= range + E) && !head_target) return false;
    if (ad.min_attack_distance > 0.0f && d <= ad.min_attack_distance + E && !head_target) return false;
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
    const Aabb box{{u.pos.x - range, u.pos.y - range, u.pos.z - range}, {u.pos.x + range, u.pos.y + range, u.pos.z + range}};
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
    if (t->flags & (kUnitSkipTargeting | kUnitDead | kUnitGarrisoned)) return false;  // 0x5680b0 [M: +0x361 / +0x38e]
    if (!desc_of(*t).is_valid_target) return false;
    float d = 0.0f, E = 0.0f;
    range_geometry(*t, u.pos, d, E);
    const float range = fire_range(base_attack_range(u), mod_r(u, ModList::FireRange));
    if (!(d <= range + E)) return false;  // slot 44
    const float mn = desc_of(u).min_attack_distance;
    if (mn > 0.0f && d <= mn + E) return false;  // slot 45
    int32_t hp = -1;
    if (u.goal_hp >= 0 && static_cast<uint32_t>(u.goal_hp) < t->hp_count && !hps_[t->hp_begin + static_cast<uint32_t>(u.goal_hp)].destroyed)
        hp = u.goal_hp;
    u.ordered_target = t->id;
    u.ordered_hp = hp;
    u.locked = 1;
    u.auto_target = kNoObject;  // replaces any auto target
    u.target_score = 0.0f;
    u.aim_target = kNoObject;
    u.aim_hp = -1;
    return true;
}

void CombatSystem::decide(Unit& u, std::vector<Candidate>& cands, uint32_t frame, SimRng& rng) {
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
    const Candidate* best = nullptr;
    for (int32_t k = 0; k < n; ++k) {
        const int32_t p = (start + k) % n;
        for (const Candidate& c : cands) {
            if (c.player != p) continue;
            if (c.aim_points > 0) rng.range_int(0, static_cast<int32_t>(c.aim_points) - 1);  // HP_Choose_Aim_Point
            if (c.id == u.auto_target && c.score == 1.0f) {  // early exit
                best = &c;
                goto done;
            }
            if (!best) {
                best = &c;
                continue;
            }
            // Replacement rule [H]. Step 2 has NO equality guard (disassembly 0x5668f5): an equal or
            // WORSE score falls through to the aim and XY-distance tie-breaks.
            const bool cl = c.fraction <= thr, bl = best->fraction <= thr;
            bool replace;
            if (cl != bl) {
                replace = cl;
            } else if (c.score < best->score) {
                replace = true;
            } else if (has_hp && c.aimable != best->aimable) {
                replace = c.aimable != 0;
            } else {
                replace = c.dist2 < best->dist2;
            }
            if (replace) best = &c;
        }
    }
done:
    if (best) {
        u.auto_target = best->id;
        u.target_score = best->score;
        u.aim_target = kNoObject;  // refixf t06: Order_Attack(unit, pick, ordered 0) re-aims the hardpoints (§2.8 C)
        u.aim_hp = -1;
        u.acquired = 1;  // OI abilities: Issue_Auto_Target for the acquired target (the autofire driver, abilities)
    } else {
        u.auto_target = kNoObject;
        u.target_score = 0.0f;
    }
}

int32_t CombatSystem::closest_hardpoint(const Unit& t, Vec3 from, bool targetable_only) const {
    int32_t best = -1;
    float best_d2 = 0.0f;
    for (uint32_t i = 0; i < t.hp_count; ++i) {
        const Hardpoint& h = hps_[t.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (h.destroyed || (targetable_only && !hd.targetable)) continue;
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
    int32_t best = -1;
    float best_rank = 0.0f, best_d2 = 0.0f;
    for (uint32_t i = 0; i < t.hp_count; ++i) {
        const Hardpoint& h = hps_[t.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (h.destroyed || !hd.destroyable) continue;
        const float rank = hardpoint_rank(set, static_cast<int32_t>(hd.kind));  // refixf t06: by HardPointType
        if (rank < 0.0f) continue;
        const Vec3 d = hp_world(t, hd) - a.pos;
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
    return u->auto_target;
}

void CombatSystem::update_weapons(Unit& u, uint32_t frame, SimRng& rng) {
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
        // S9: an ordered hardpoint (command 40/45 index) is kept while it is alive.
        if (tgt == u.ordered_target && u.ordered_hp >= 0 && static_cast<uint32_t>(u.ordered_hp) < t->hp_count &&
            !hps_[t->hp_begin + static_cast<uint32_t>(u.ordered_hp)].destroyed) {
            sub = u.ordered_hp;
        } else if (tgt == u.aim_target && u.aim_hp >= 0 && static_cast<uint32_t>(u.aim_hp) < t->hp_count &&
                   !hps_[t->hp_begin + static_cast<uint32_t>(u.aim_hp)].destroyed) {
            sub = u.aim_hp;
        } else {
            sub = choose_target_hardpoint(u, *t);
        }
    }
    const bool idle = !live(t);
    for (uint32_t i = 0; i < u.hp_count; ++i) {
        Hardpoint& h = hps_[u.hp_begin + i];
        const HardpointDesc& hd = data_->hardpoints[h.desc];
        if (!is_weapon(hd.kind)) continue;
        h.target = tgt;  // HP_Set_Target: owner's target + chosen sub-hardpoint
        h.target_hp = sub;
        // Update_Weapon (0x387400): gates, then the cooldown countdown, then Fire_At.
        if (!cfg_.weapons_enabled || hp_out(h)) continue;
        h.cooldown -= 1;
        if (h.cooldown > 0) continue;
        if (fire_hardpoint(u, h, frame, rng)) continue;
        // Opportunity fire (TACTICAL_COMBAT 2.6, S9): no shot at the owner's target.
        if (cfg_.opportunity_fire && opportunity_allowed(u, hd, idle)) opportunity_fire(u, h, frame, rng);
    }
    if (desc_of(u).weapon_projectile >= 0 && cfg_.weapons_enabled) fire_unit_weapon(u, frame, rng);
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
    // 2. target hardpoint: closest to the shooter when none was chosen (0x394a80)
    int32_t thp = h.target_hp;
    if (thp < 0 && t.hp_count > 0) thp = closest_hardpoint(t, u.pos, false);
    // S9: ability projectile override / suppression (ion cannon shot, lucky shot).
    int32_t proj = hd.projectile;
    float dscale = 1.0f;  // OI abilities: Blast state gate / projectile / damage multiplier (REMAINING_GAPS §5.5)
    if (hooks_ && !hooks_->fire_gate(u.id, &hd, proj, dscale)) return false;
    if (hooks_ && !hooks_->shot_allowed(u.id, t.id, thp, proj)) return false;
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return false;
    // 5. aim point
    const Vec3 aim0 = thp >= 0 ? hp_world(t, data_->hardpoints[hps_[t.hp_begin + static_cast<uint32_t>(thp)].desc]) : t.pos;
    // 3. cone (turret test / fire cone)
    if (!in_cone(u, h, aim0)) return false;
    // 6. fire bones: A, or the A-B midpoint (C and D never read)
    Vec3 a = local_to_world(u, hd.bone_a);
    Vec3 b = hd.has_bone_b ? local_to_world(u, hd.bone_b) : a;
    if (hooks_) hooks_->fire_bones(u.id, static_cast<uint32_t>(&h - &hps_[u.hp_begin]), a, b);  // S10-int: animated bones
    const Vec3 ref = hd.has_bone_b ? (a + b) * 0.5f : a;
    // 7. range, XY only
    const float range = fire_range(hd.fire_range, mod_r(u, ModList::FireRange));
    const float dist = distance_xy(ref, aim0);
    if (!in_fire_range(dist, hd.fire_min_range, range, td.radius)) return false;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    // S9 energy pool (0x56bee0): a shot with Projectile_Energy_Per_Shot needs that much energy [H];
    // tested before the shot's draws [M].
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u) && u.energy < pd.energy_per_shot) return false;
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
    // 9. lead
    Vec3 aim = lead_point(t, muzzle, aim0, pd.max_speed * static_cast<float>(cfg_.fps));  // OI tactical: circular lead
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
        const float ws = cfg_.mode == GameMode::Land ? weather_scale(pd) : 1.0f;
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
            float spread = inaccuracy_spread(scale_inaccuracy(dd, f), dist, range);
            if (ws != 1.0f) spread = spread * ws;
            aim = aim + inaccuracy_offset(spread, rng);
        }
    }
    // 11. spawn (an override projectile keeps the hardpoint's damage / type overrides [M])
    const float damage = (hd.projectile_damage > 0.0f ? hd.projectile_damage : pd.damage) * dscale;
    const uint16_t dtype = hd.damage_type != kNoTypeId ? hd.damage_type : pd.damage_type;
    spawn(u, pd, static_cast<uint32_t>(proj), damage, dtype, muzzle, aim, t, thp, range, frame);
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
    u.weapon_cooldown -= 1;
    if (u.weapon_cooldown > 0) return;
    const UnitDesc& ud = desc_of(u);
    const Unit* tp = unit(current_target(u.id));
    if (!live(tp)) return;
    const Unit& t = *tp;
    if ((t.flags & kUnitStealthed) && !ud.target_stealth_units && t.owner != u.owner) return;
    if (!is_enemy(u.owner, t.owner)) return;
    // S9: the ordered hardpoint (command 40 index) of the ordered target, while it is alive.
    int32_t thp = -1;
    if (t.id == u.ordered_target && u.ordered_hp >= 0 && static_cast<uint32_t>(u.ordered_hp) < t.hp_count &&
        !hps_[t.hp_begin + static_cast<uint32_t>(u.ordered_hp)].destroyed)
        thp = u.ordered_hp;
    int32_t proj = fired_weapon_projectile(u);  // OI abilities: weapon index obj +0x324
    float dscale = 1.0f;  // OI abilities: Blast state gate (REMAINING_GAPS §5.5; a weapon never fires while it runs)
    if (hooks_ && !hooks_->fire_gate(u.id, nullptr, proj, dscale)) return;
    if (hooks_ && !hooks_->shot_allowed(u.id, t.id, thp, proj)) return;
    if (proj < 0 || static_cast<size_t>(proj) >= data_->projectiles.size()) return;
    const ProjectileDesc& pd = data_->projectiles[static_cast<size_t>(proj)];
    const float range = fire_range(ud.max_attack_distance, mod_r(u, ModList::FireRange));  // 0x54d090: list 8
    const float dist = distance_xy(u.pos, t.pos);
    if (!in_fire_range(dist, 0.0f, range, desc_of(t).radius)) return;
    if (pd.energy_per_shot > 0.0f && has_energy_pool(u) && u.energy < pd.energy_per_shot) return;
    const Vec3 muzzle = u.pos;
    Vec3 aim = lead_point(t, muzzle, t.pos, pd.max_speed * static_cast<float>(cfg_.fps));  // OI tactical: circular lead
    if (cfg_.inaccuracy_enabled) {  // [L] fixed-radius branch only (no per-hardpoint table)
        float r = scale_inaccuracy(ud.fire_inaccuracy_fixed_radius, inaccuracy_f(cfg_.mode, player(u.owner).ai_accuracy));
        if (cfg_.mode == GameMode::Land && weather_scale(pd) != 1.0f) r = r * weather_scale(pd);  // L1 systems: weather
        for (int s = 0; s < 2; ++s) {
            const UnitAbilityDesc* ab = ability(u, s);
            if (ab && u.ability_active[static_cast<size_t>(s)] && ab->inaccuracy_override > 0.0f) {
                r = ab->inaccuracy_override;
                break;
            }
        }
        if (r > 0.0f) aim = aim + inaccuracy_offset(r, rng);
    }
    spawn(u, pd, static_cast<uint32_t>(proj), pd.damage * dscale, pd.damage_type, muzzle, aim, t, thp, range, frame);
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
    shot_events_.push_back({u.id, t.id, t.owner});  // OI fog: REVEAL vf26 flash
    if (hooks_) hooks_->fire_done(u.id, nullptr, frame);
    if (hooks_) hooks_->shot_fired(u.id, t.id, thp, proj, frame);
}

void CombatSystem::spawn(const Unit& shooter, const ProjectileDesc& pd, uint32_t pdesc, float damage, uint16_t damage_type,
                         Vec3 muzzle, Vec3 aim, const Unit& target, int32_t target_hp, float range, uint32_t frame) {
    Projectile p;
    p.serial = next_serial_++;
    p.desc = pdesc;
    p.shooter = shooter.id;
    p.owner = shooter.owner;
    p.target = target.id;
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
    p.max_travel = base > 0.0f ? base + desc_of(target).radius + abs_f(aim.z - muzzle.z) : 0.0f;
    // OI tactical: lobbed shots (category 3) leave on the ballistic arc to the aim point (formulas.hpp lob_direction).
    const float g = lob_gravity(pd);
    if (g > 0.0f) {
        p.gravity = g;
        p.floor_z = aim.z;
        p.dir = lob_direction(muzzle, aim, pd.max_speed, g);
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
    // Hostile players' trees; the port takes the minimum (t, object id) over all of them (the
    // original returns the first hit in tree order, section 3.4).
    for (int32_t q = 0; q < np; ++q) {
        if (!is_enemy(p.owner, q)) continue;
        for (const RayHit& hit : trees_->player(static_cast<uint32_t>(q)).query_ray(ray, kSkipDead, ctx)) {
            const Unit* v = unit(static_cast<ObjectId>(hit.handle));
            if (!live(v)) continue;
            if (best.id == kNoObject || hit.t < best.t || (hit.t == best.t && v->id < best.id)) best = SweepHit{v->id, hit.t};
            break;  // hits are sorted by (t, id): the first live one is this tree's minimum
        }
    }
    return best;
}

void CombatSystem::sweep(uint32_t pi, QueryContext& ctx) {
    const Projectile& p = projectiles_[pi];
    SweepHit best{kNoObject, 0.0f};
    if (p.alive && p.spawn_frame < frame_ && p.fuse < 0.0f) {  // OI abilities: armed bombs only detonate on the fuse
        if (p.rocket) {
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
            best = sweep_segment(p, p.dir * p.speed, ctx);
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

void CombatSystem::process_hit(Projectile& p, Unit& v, float t, uint32_t frame, SimRng& rng, const Vec3* segment) {
    // Process_Hit (0x4cb280) [H unless tagged].
    const ProjectileDesc& pd = data_->projectiles[p.desc];
    const UnitDesc& vd = desc_of(v);
    ++stats_.hits;
    float dmg = p.damage;
    if (dmg == 0.0f) dmg = pd.blast_area_damage;
    int32_t hp = v.id == p.target ? p.target_hp : -1;  // the projectile's targeted hardpoint [M]
    if (pd.damages_random_hardpoints && v.hp_count > 0) {
        // synced Get_Int(0, count - 1), then probe forward (wrapping) to a live destroyable one
        const auto start = static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(v.hp_count) - 1));
        hp = -1;
        for (uint32_t k = 0; k < v.hp_count; ++k) {
            const uint32_t j = (start + k) % v.hp_count;
            const Hardpoint& h = hps_[v.hp_begin + j];
            if (!h.destroyed && data_->hardpoints[h.desc].destroyable) {
                hp = static_cast<int32_t>(j);
                break;
            }
        }
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
    const Vec3 impact = segment ? p.pos + *segment * t : p.pos + p.dir * (p.speed * t);
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
        // [port] Destroy_When_Stunned_Over_Water (scene water height) is not modelled.
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
        for (Unit* v : vs) {
            if (!effect_match(*v, pd.convert_mask, nullptr) || !live(v)) continue;
            const uint32_t team = v->team;
            const ObjectId vid = v->id;
            convert_unit(vid, p.owner);
            if (team != 0)
                for (Unit& m : units_)
                    if (m.team == team && m.id != vid && live(&m)) convert_unit(m.id, p.owner);
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
        if (vd.deploys && vd.has_locomotor && (v.flags & kUnitDeploying))
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
    if (v.health <= 0.0f) {
        if (v.flags & kUnitCannotDie) {
            v.health = old > 1.0f ? old : 1.0f;  // max(1, old)
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
    // 2. teams [H]
    if (!r.no_team && v.team != 0 && (vd.redirect_damage_to_teammates || vd.share_damage_with_teammates) && depth < 4) {
        uint32_t count = 0;
        for (const Unit& m : units_)
            if (m.team == v.team && !(m.flags & kUnitDead)) ++count;
        if (vd.redirect_damage_to_teammates && count > 1) {
            const float each = r.amount / static_cast<float>(count - 1);
            for (size_t i = 0; i < units_.size(); ++i) {
                const Unit& m = units_[i];
                if (m.team != v.team || (m.flags & kUnitDead) || m.id == v.id) continue;
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
                    if (m.team != v.team || (m.flags & kUnitDead)) continue;
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
    if (v.flags & kUnitRetreating) x = x * cfg_.retreat_damage_mod_factor;                    // 9
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
    float hx;
    if (sf != 0.0f) {
        hx = (sx - absorbed - energy_taken) / sf * hf;  // back to raw, then hull-armor space
    } else {
        hx = x * hf;  // [L] shield factor 0: nothing to convert back from
    }
    if (pd && !pd->does_hitpoint_damage) hx = 0.0f;
    // S9 t04 (§1.1): the death-clone sub-key = the source projectile's Damage_Type index (-1: not a projectile).
    const int32_t sub_key = pd && pd->damage_type != kNoTypeId ? static_cast<int32_t>(pd->damage_type) : -1;
    // 17 hardpoint routing: a named live destroyable hardpoint takes everything
    if (hp_index >= 0 && static_cast<uint32_t>(hp_index) < v.hp_count) {
        Hardpoint& h = hps_[v.hp_begin + static_cast<uint32_t>(hp_index)];
        if (!h.destroyed && data_->hardpoints[h.desc].destroyable) {
            hardpoint_damage(v, h, hx, r.source, out, frame, false, sub_key);
            track_damage(v, absorbed + hx, frame);
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
    // refix t02 (S9_DEATH_CLONE_SHIELDS.md §1.5 [H]): attach INFECTION (behaviour factory 0x4c3520) to a victim without
    // it; a victim that has it is resumed instead (vfunc +0xb8: counter - 1, floored at 0). [L] A behaviour that becomes
    // active (attached, or resumed to 0) restarts its Duration_Of_Infection and its once-per-second damage; it keeps the
    // parameters, infecting player and source of its first attach. Resuming an active one changes nothing.
    if (v.flags & kUnitDead) return;
    if (v.infect_attached) {
        if (v.infect_suspend == 0) return;
        if (--v.infect_suspend != 0) return;
    } else {
        v.infect_attached = 1;
        v.infect_suspend = 0;
        v.infect_desc = desc >= 0 && static_cast<size_t>(desc) < data_->units.size() ? desc : -1;
        v.infect_player = player;
        v.infect_source = source;
    }
    v.flags |= kUnitInfected;
    v.infect_exposure.clear();
    v.infect_end = 0;
    v.infect_next_damage = frame + cfg_.fps;
    if (v.infect_desc >= 0) {
        const InfectionDesc& in = data_->units[static_cast<size_t>(v.infect_desc)].infection;
        if (in.duration_secs > 0.0f)
            v.infect_end = frame + static_cast<uint32_t>(std::max(1.0f, in.duration_secs * static_cast<float>(cfg_.fps)));
    }
}

void CombatSystem::infection_service(Unit& u, uint32_t frame) {
    // INFECTION update (0x636ae0), S9_DEATH_CLONE_SHIELDS.md §1.5 [H for the shape: it spreads the behaviour to nearby
    // objects that stay in range for consecutive frames, and resumes suspended ones]. The rules below are [L] (the update
    // was not read), built on the Infection_Ability fields:
    //   * serviced every frame while active (counter 0) and parameterised; a suspended one does nothing;
    //   * expiry: at Duration_Of_Infection after it became active it suspends itself (counter + 1, flag cleared);
    //   * spread: every live unit (index order) other than this one, not actively infected, enemy of the infecting
    //     player, whose CategoryMask meets Infection_CategoryMask, within Distance_To_Infect (XY). The first frame a
    //     neighbour is seen in range is kept while it stays in range on every service; after Seconds_To_Infect x FPS
    //     consecutive frames it is infected (attached with these parameters, or resumed) and dropped from the list;
    //   * damage: Damage_Per_Second once per second, kind 20 (Damage_Infection), source = the infecting unit.
    if (!u.infect_attached || u.infect_suspend != 0 || u.infect_desc < 0 || (u.flags & kUnitDead)) return;
    const InfectionDesc& in = data_->units[static_cast<size_t>(u.infect_desc)].infection;
    if (u.infect_end != 0 && frame >= u.infect_end) {
        suspend_infection(u.id);
        return;
    }
    if (in.distance_to_infect > 0.0f) {
        const float r2 = in.distance_to_infect * in.distance_to_infect;
        const auto need = static_cast<uint32_t>(std::max(1.0f, in.seconds_to_infect * static_cast<float>(cfg_.fps)));
        infect_scratch_.clear();
        for (Unit& c : units_) {
            if (&c == &u || (c.flags & kUnitDead) || (c.infect_attached && c.infect_suspend == 0)) continue;
            if (!is_enemy(u.infect_player, c.owner)) continue;
            if (in.category_mask != 0 && (desc_of(c).category_mask & in.category_mask) == 0) continue;
            if (distance2_xy(u.pos, c.pos) > r2) continue;
            uint32_t first = frame;
            for (const auto& [id, f] : u.infect_exposure)
                if (id == c.id) first = f;
            if (frame + 1 - first >= need) {
                infect(c, u.infect_desc, u.infect_player, u.infect_source, frame);
            } else {
                infect_scratch_.emplace_back(c.id, first);
            }
        }
        u.infect_exposure.assign(infect_scratch_.begin(), infect_scratch_.end());
    }
    if (in.damage_per_second > 0.0f && frame >= u.infect_next_damage) {
        u.infect_next_damage = frame + cfg_.fps;
        DamageRequest r;
        r.victim = u.id;
        r.amount = in.damage_per_second;
        r.kind = kKindInfection;
        r.source = u.infect_source;
        take_damage(r, frame);
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
        process_hit(pc, *v, hit.t, frame, rng, &seg);
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
    tick_rng_ = &rng;
    deaths_.clear();
    hp_events_.clear();
    smoke_events_.clear();
    damage_events_.clear();
    hit_events_.clear();  // S10-int
    shot_events_.clear();  // OI fog
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
                u.mult = u.own_mult;
                u.squad_seen = 0;
            }
        };
        pf(n, cfg_.grain, fn);
    }
    // Squad rule (serial): a leader whose own product is 1 takes the minimum over its members.
    leaders_.clear();
    for (uint32_t i = 0; i < n; ++i)
        if (units_[i].team != 0 && units_[i].team_leader && !(units_[i].flags & kUnitDead)) leaders_.emplace_back(units_[i].team, i);
    // OI tactical team targeting: (team, team_leader id) for team_head() (read-only in the parallel gather phase).
    team_heads_.clear();
    for (const auto& [team, index] : leaders_) team_heads_.emplace_back(team, units_[index].id);
    std::sort(team_heads_.begin(), team_heads_.end());
    if (!leaders_.empty()) {
        std::sort(leaders_.begin(), leaders_.end());
        for (uint32_t i = 0; i < n; ++i) {
            const Unit& m = units_[i];
            if (m.team == 0 || m.team_leader || (m.flags & kUnitDead)) continue;
            const auto it = std::lower_bound(leaders_.begin(), leaders_.end(), std::make_pair(m.team, 0u));
            if (it == leaders_.end() || it->first != m.team) continue;
            Unit& L = units_[it->second];
            for (uint32_t k = 0; k < kModKindCount; ++k) {
                if (L.own_mult[k] != 1.0f) continue;
                const auto bit = static_cast<uint16_t>(1u << k);
                if (!(L.squad_seen & bit) || m.own_mult[k] < L.mult[k]) L.mult[k] = m.own_mult[k];
                L.squad_seen = static_cast<uint16_t>(L.squad_seen | bit);
            }
        }
    }
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
    for (uint32_t k = n; k-- > 0;) {
        Unit& u = units_[k];
        if (u.flags & kUnitDead) continue;
        if (hooks_) {
            hooks_->unit_begin(u.id, frame, rng);
            if (u.flags & kUnitDead) continue;
        }
        burning_service(u, frame);
        if (u.flags & kUnitDead) continue;
        infection_service(u, frame);  // refix t02 [M: slot next to the other damage-over-time behaviour]
        if (u.flags & kUnitDead) continue;
        // OI abilities: a stunned unit's targeting / turret behaviours are suspended (0x647290); its STUNNABLE service
        // at the end frame releases it [inferred]. Requires_Base_Power units do not act without base power (0x632270).
        if (u.stun_end != 0 && frame >= u.stun_end) {
            u.stun_end = 0;
            if (hooks_) hooks_->unit_effect(u.id, UnitEffect::StunEnded, u.owner, u.pos, frame);
        }
        const bool idle = u.stun_end > frame || (desc_of(u).requires_base_power && !has_base_power(u.owner));
        u.acquired = 0;
        if (!idle) decide(u, cands_[k], frame, rng);
        if (hooks_) {
            hooks_->unit_after_targeting(u.id, frame, rng);
            if (u.flags & kUnitDead) continue;
        }
        service_repairs(u, frame);  // OI abilities: hardpoint update 0x387010 -> repair 0x387170, before the weapons
        if (!idle) update_weapons(u, frame, rng);
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
        if (projectiles_[i].rocket) {  // refixf t01: spline rocket (0x4cfc40)
            rocket_step(i, frame, rng);
            continue;
        }
        SweepHit hit = sweep_[i];
        if (hit.id != kNoObject && !live(unit(hit.id))) {  // victim died earlier in this phase
            ++stats_.resweeps;
            sweep(i, serial_ctx_);
            hit = sweep_[i];
        }
        if (hit.id != kNoObject) {
            Unit* v = unit(hit.id);
            projectiles_[i].alive = 0;
            Projectile pc = projectiles_[i];
            process_hit(pc, *v, hit.t, frame, rng);
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
        if (p.gravity > 0.0f) {
            // OI tactical: lobbed flight (0x4ceea0) [M/port] - gravity bends the velocity after the step; the shot
            // detonates when it comes down to its aim height (the ground test 0x4d0630 stand-in), or by its lifetime
            // when it has one (3.3 expiry: category 3 with a lifetime). The travel limit does not apply to the arc.
            const Vec3 v{p.dir.x * p.speed, p.dir.y * p.speed, p.dir.z * p.speed - p.gravity};
            const float sp = dmath::sqrt(dot(v, v));
            if (sp > 0.0f) {
                p.dir = v * (1.0f / sp);
                p.speed = sp;
            }
            const bool landed = p.dir.z < 0.0f && p.pos.z <= p.floor_z;
            if (landed) p.pos.z = p.floor_z;
            expire = landed || (pd.max_lifetime > 0.0f &&
                                static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps) > pd.max_lifetime);
        } else if (p.fuse >= 0.0f) {  // OI abilities: armed bomb timer
            expire = static_cast<float>(frame - p.spawn_frame) / static_cast<float>(cfg_.fps) >= p.fuse;
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
    Vec3 aim = l.point;
    float radius = 0.0f;
    if (l.target != kNoObject) {
        const Unit* t = unit(l.target);
        if (!live(t)) return false;
        aim = t->pos;
        if (l.target_hp >= 0 && static_cast<uint32_t>(l.target_hp) < t->hp_count)
            aim = hp_world(*t, data_->hardpoints[hps_[t->hp_begin + static_cast<uint32_t>(l.target_hp)].desc]);
        aim = lead_point(*t, l.from, aim, pd.max_speed * static_cast<float>(cfg_.fps));  // OI tactical: circular lead
        radius = desc_of(*t).radius;
    }
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
    if (l.fuse_seconds >= 0.0f) {  // OI abilities: an armed bomb sits where it was placed until the fuse runs out
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
float CombatSystem::max_energy(ObjectId id) {
    Unit* u = unit(id);
    if (!u) return 0.0f;
    ensure_cache(*u);
    return unit_max_energy(*u);
}
void CombatSystem::set_energy(ObjectId id, float e) {
    if (Unit* u = unit(id)) u->energy = e;
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
void CombatSystem::suspend_infection(ObjectId id) {
    Unit* u = unit(id);
    if (!u || !u->infect_attached) return;
    ++u->infect_suspend;
    u->flags &= ~kUnitInfected;
    u->infect_exposure.clear();
}
void CombatSystem::resume_infection(ObjectId id, uint32_t frame) {
    Unit* u = unit(id);
    if (!u || !u->infect_attached || (u->flags & kUnitDead)) return;
    infect(*u, u->infect_desc, u->infect_player, u->infect_source, frame);
}
bool CombatSystem::detonate_projectile(size_t index, uint32_t frame) {
    if (index >= projectiles_.size() || !projectiles_[index].alive) return false;
    projectiles_[index].alive = 0;
    const Projectile pc = projectiles_[index];  // copy: detonation may spawn (death projectiles)
    const ProjectileDesc& pd = data_->projectiles[pc.desc];
    if (pd.blast_area_damage > 0.0f && pd.blast_area_range > 0.0f) area_damage(pc, pc.pos, kNoObject, frame);
    detonation_effects(pc, pc.pos, kNoObject, frame);
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
        h.u32(u.flags);
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
        h.u32(u.stun_end);
        h.u32(u.invuln_end);
        h.u32(u.emitter_entries);
        if (u.weapon_index != 0) {  // OI abilities: swapped weapons only (format unchanged otherwise)
            h.u8(0x24);
            h.i32(u.weapon_index);
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
            h.u32(u.infect_end);
            h.u32(u.infect_next_damage);
            h.u32(static_cast<uint32_t>(u.infect_exposure.size()));
            for (const auto& [id, f] : u.infect_exposure) {
                h.i32(id);
                h.u32(f);
            }
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
        if (p.gravity > 0.0f) {  // OI tactical: lobbed flight state (absent for straight shots: format unchanged)
            h.f32(p.gravity);
            h.f32(p.floor_z);
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
