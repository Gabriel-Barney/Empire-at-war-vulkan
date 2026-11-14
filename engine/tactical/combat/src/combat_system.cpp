// combat_system: part 1 (bo3 t03 split; shared code in combat_system_internal.hpp).
#include "combat_system_internal.hpp"

namespace alamo::tactical::combat {

namespace bo3split {

Vec3 normalize(Vec3 v) {
    const float l2 = dot(v, v);
    if (!(l2 > 0.0f)) return Vec3{1.0f, 0.0f, 0.0f};
    const float inv = 1.0f / dmath::sqrt(l2);
    return v * inv;
}
}  // namespace bo3split

namespace bo3split {

float wrap_deg(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
}  // namespace bo3split

namespace bo3split {

float abs_f(float v) { return v < 0.0f ? -v : v; }
}  // namespace bo3split

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
    if (nu.team != 0) {  // s78n review: a team unit added between ticks is in the member lists at once (as after a load)
        const std::pair<uint32_t, ObjectId> e{nu.team, nu.id};
        team_members_.insert(std::lower_bound(team_members_.begin(), team_members_.end(), e), e);
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

void CombatSystem::set_team_position(uint32_t team, Vec3 p) {
    const auto it = std::lower_bound(team_pos_.begin(), team_pos_.end(), team,
                                     [](const std::pair<uint32_t, Vec3>& e, uint32_t t) { return e.first < t; });
    if (it != team_pos_.end() && it->first == team) it->second = p;
    else team_pos_.insert(it, {team, p});
}

Vec3 CombatSystem::scan_pos(const Unit& u) const {
    // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2.1): TeamTargeting scans and range-tests from the container, which walks its
    // own path (it is not its members' centre). [port] The head is the team_leader member standing in for the container.
    if (cfg_.mode != GameMode::Land || u.team == 0 || team_pos_.empty()) return u.pos;
    if (!u.team_leader && static_cast<uint32_t>(u.id) != u.team) return u.pos;
    const auto it = std::lower_bound(team_pos_.begin(), team_pos_.end(), u.team,
                                     [](const std::pair<uint32_t, Vec3>& e, uint32_t t) { return e.first < t; });
    return it != team_pos_.end() && it->first == u.team ? it->second : u.pos;
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
    // s78n t06 (TACTICAL_COMBAT §2.8 B step 5, HERO_CLASH §5.2): Set_Target 0x569160 refuses (no change) a target with
    // +0x3b4, and the pairing rule refuses a clashing unit any other target.
    if (const Unit* t = unit(target); t && (t->flags & kUnitHeroClash)) return;
    if (!clash_allows(attacker, target)) return;
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
    if ((t->flags & kUnitHeroClash) || !clash_allows(u->id, t->id)) return false;  // s78n t06: Set_Target / pairing
    if ((t->flags & kUnitStealthed) && !desc_of(*u).target_stealth_units && u->owner != t->owner) return false;
    if (t->flags & kUnitGarrisoned) return false;
    if (hover_refused(*u, *t)) return false;  // s78n t20 batch 2: the can-attack test 0x38d730 (MOVEMENT §5.3)
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

bool CombatSystem::inside_min_range_of(ObjectId attacker, ObjectId target, float r) const {
    const Unit* a = unit(attacker);
    const Unit* t = unit(target);
    if (!a || !t) return false;
    return min_range_hit(*t, a->pos, r);
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

bool CombatSystem::hover_refused(const Unit& a, const Unit& t) const {
    // s78n t20 batch 2 (MOVEMENT §5.3 "Hover in full", 0x567c50 / 0x5662b0 / 0x38d730): a melee attacker
    // (Targeting_Max_Attack_Distance <= Melee_Cutoff_Range) rejects a target with Hover_Height > 0 in hover state 1.
    return (t.flags & kUnitHoverRaised) != 0 && desc_of(a).max_attack_distance <= cfg_.short_range_cutoff;
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
    // the Y extent. A1j t04 / s78n t08f (TACTICAL_COMBAT §5.4.5, 0x371e70 / 0x372040): per axis Custom_Hard_X/YExtent x
    // Scale_Factor, else the model's collision-bound half y (X, along) / half x (Y, across) x Scale_Factor (model_x /
    // y_extent); [port] the radius only when the host gave no model data. The
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
    // s78n t11 (TACTICAL_COMBAT §5.4.6 items 3 / 4 and port spec item 6; 0x396a30 / 0x396b70 -> 0x371f30 / 0x372100): a
    // Deploys type with a locomotor in Walk state 0x18 Deploying (kUnitDeployingIn) or 0x19 Deployed (kUnitDeploying; not
    // 0x1A Undeploying) takes Custom_Hard_XExtent_Deployed / _YExtent_Deployed x Scale_Factor on each axis that sets one;
    // the axes are independent (unlike 0x371d20's shared test). Not deployed_or_deploying(): that excludes 0x18. s78n t26.1:
    // walking deployed (pack +0x2b3, the Destroyer Droid) keeps the host's state 0x19, so kUnitDeploying covers it.
    const bool dep = td.deploys && td.has_locomotor && (t.flags & (kUnitDeploying | kUnitDeployingIn)) != 0;
    const float dx = dep ? td.hard_x_extent_deployed : 0.0f, dy = dep ? td.hard_y_extent_deployed : 0.0f;
    if (!(dx > 0.0f) && !(dy > 0.0f) && !(td.hard_x_extent > 0.0f) && !(td.hard_y_extent > 0.0f) && td.model_x_extent < 0.0f) {
        ex = ey = td.radius;
        return false;
    }
    ex = dx > 0.0f ? dx : td.hard_x_extent > 0.0f ? td.hard_x_extent : td.model_x_extent >= 0.0f ? td.model_x_extent : td.radius;
    ey = dy > 0.0f ? dy : td.hard_y_extent > 0.0f ? td.hard_y_extent : td.model_y_extent >= 0.0f ? td.model_y_extent : td.radius;
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
    // s78n t06 (HERO_CLASH §3.1): a hero in a clash has TARGETING suspended (slot 9) while +0x3b4 is set.
    return idle || (u.flags & (kUnitWeaponsSuspended | kUnitTargetingSuspended | kUnitHeroClash));
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
    // 1 (s78n t05: kUnitIncognito is +0x3a1 & 0x10 too), 3 (s78n t06: +0x3b4 is kUnitHeroClash)
    if (t.flags & (kUnitSkipTargeting | kUnitIncognito | kUnitDead | kUnitGarrisoned | kUnitHeroClash)) return false;
    if (hover_refused(a, t)) return false;  // s78n t20 batch 2: a raised hover unit and a melee attacker (Scan_Player)
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
    const Vec3 ap = scan_pos(a);  // s78n t03: a land team head tests from its container
    float d = 0.0f, E = 0.0f;
    if (!gw) range_geometry(t, ap, d, E);  // OI tactical: + the LAND_OBSTACLE aim walk
    const bool head_target = head && current_target(head->id) == t.id;
    const bool in_max = gw ? lob_in_range(a, *gw, t) : d <= range + E;
    // S8 t17 (FIGHTER_FLYING §4.10 A, last paragraph): beyond R, a formation unit also accepts c when 0x4160a0(F, u, c)
    // != 0 (the formation's response range).
    const auto responds = [&] {
        float rr = 0.0f;
        return (a.flags & kUnitScanQuery) && scan_range_ && scan_range_(a.id, t.id, range, rr) != 0;
    };
    if (!in_max && !head_target && !responds()) return false;
    if (!head_target && min_range_hit(t, ap, ad.min_attack_distance)) return false;
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
        // attacking an ordered target: no auto scan [M] (s78n t20 batch 2: unless decide_target drops it, hover_refused)
        if (live(o) && is_enemy(u.owner, o->owner) && !hover_refused(u, *o)) return;
    }
    if (frame_ < u.next_scan) return;
    if (!unit_can_attack(u)) return;  // no TargetingBehavior work without a weapon
    // s78n t05 (HERO_CLASH §8.2, targeting services 0x634810 / 0x633ae0): an incognito unit (+0x3a1 & 0x10) does not
    // auto-acquire. [port] no scan is due, so the scan draws are not made either; an ordered target is kept (above).
    if (u.flags & kUnitIncognito) return;
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
    const Vec3 up = scan_pos(u);  // s78n t03: a land team scans around its container (§2.2 box centre)
    const Aabb box{{up.x - half, up.y - half, up.z - half}, {up.x + half, up.y + half, up.z + half}};
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
            c.dist2 = distance2_xy(up, t->pos);
            c.aim_points = aim_point_count(*t);
            c.aimable = (u.hp_count && aimable(u, *t)) ? 1 : 0;
            out.push_back(c);
        }
    }
}

bool CombatSystem::clash_allows(ObjectId u, ObjectId x) const {
    const Unit* a = unit(u);
    const Unit* b = unit(x);
    if (x == kNoObject) return true;
    const bool fa = a && (a->flags & kUnitHeroClash);
    const bool fb = b && (b->flags & kUnitHeroClash);
    if (!fa && !fb) return true;  // the callers only ask 0x39a540 when U or X has +0x3b4
    if (!fa || !fb) return false;
    // 0x39a540: the first running clash involving U must also involve X (s78n t06; start order = the manager list).
    for (const auto& [h1, h2] : clash_pairs_)
        if (h1 == u || h2 == u) return h1 == x || h2 == x;
    return false;  // flagged with no clash (the snap quirk, §3.8): nothing may act on it, it acts on nothing
}

bool CombatSystem::lock_holds(const Unit& u, const Unit& t) const {
    // §2.8 B "while locked" [H]: dropped when the target is garrisoned, +0x3b4 (s78n t06), stealthed for an attacker
    // without Target_Stealth_Units, or hidden by fog for the attacker's player (0x35f470, force 1). Dying / dead: live().
    if (t.flags & (kUnitGarrisoned | kUnitHeroClash)) return false;
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
    // [port] no transports in battles; +0x3b4 is kUnitHeroClash (s78n t06). kUnitSkipTargeting (the port's untargetable
    // marker) kept.
    if (t->flags & (kUnitSkipTargeting | kUnitDead | kUnitGarrisoned | kUnitHeroClash)) return false;
    if (!desc_of(*t).target_test_type) return false;
    float d = 0.0f, E = 0.0f;
    const Vec3 up = scan_pos(u);  // s78n t03: a land team head's pre-check runs from its container
    range_geometry(*t, up, d, E);
    if (const ProjectileDesc* gw = grenade_weapon(u)) {
        if (!lob_in_range(u, *gw, *t)) return false;  // slot 44, grenade weapon (S8 t04: LOB_SOLVER §3.1)
    } else {
        const float range = fire_range(base_attack_range(u), mod_r(u, ModList::FireRange));
        if (!(d <= range + E)) return false;  // slot 44
    }
    if (min_range_hit(*t, up, desc_of(u).min_attack_distance)) return false;  // slot 45 -> 46 (s78n t02)
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
        // s78n t20 batch 2: Validate_Current_Target 0x567c50 drops a raised hover target of a melee attacker
        if (live(o) && is_enemy(u.owner, o->owner) && !hover_refused(u, *o) && (!u.locked || lock_holds(u, *o))) return;
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
    // (0x18) and Undeploying (0x1A) - kUnitDeployTransition - do not count. s78n t26.1 (MOVEMENT §3.2.1): the walking-deployed
    // flag +0x2b3 keeps the movement state 0x19 (no deploy event), so a walking-deployed Destroyer Droid counts here. s78n
    // t26 closing review (MOVEMENT §3.2.1 Helpers, item 4 row "deploying (0x18), d < 150"): D is +0x2b3 set OR state 0x19,
    // so a deploying unit (0x18, kUnitDeployTransition | kUnitDeployingIn) counts once an order set the flag (the host
    // mirrors it as kUnitDeploying, TacticalBattle::oi_deploy_sync); 0x1A clears the flag (set_deploy_state).
    const UnitDesc& ud = desc_of(u);
    if (!ud.deploys || !ud.has_locomotor || (u.flags & kUnitDeploying) == 0) return false;
    return (u.flags & kUnitDeployTransition) == 0 || (u.flags & kUnitDeployingIn) != 0;
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
    return range_test(u, from, t) == CanHit::Yes;
}

CanHit CombatSystem::range_test(const Unit& u, Vec3 from, const Unit& t) const {
    // Slot 44 && !slot 45 (TACTICAL_COMBAT §3.1.1 "Gate details" step 7, LOB_SOLVER §8 item 22): In_Max_Range (XY distance
    // <= R + E with the LAND_OBSTACLE aim walk; for a grenade weapon the lob solver alone, LOB_SOLVER §3.1) and not inside
    // Targeting_Min_Attack_Distance + E, from the targeting object's position (s78n t02: slot 46's own E, min_range_hit).
    const UnitDesc& ud = desc_of(u);
    const ProjectileDesc* gw = grenade_weapon(u);
    float d = 0.0f, E = 0.0f;
    if (!gw) range_geometry(t, from, d, E);
    if (gw) {
        LobSolution s;  // as lob_in_range (one pass at Max_Speed decides the yes / no)
        if (!lob_solve(from, lob_target_point(t), gw->max_speed, lob_g(*gw), false, s)) return CanHit::OutOfMaxRange;
    } else {
        const float range = fire_range(base_attack_range(u), mod_r(u, ModList::FireRange));
        if (!(d <= range + E)) return CanHit::OutOfMaxRange;
    }
    return min_range_hit(t, from, ud.min_attack_distance) ? CanHit::InsideMinRange : CanHit::Yes;
}

const CombatSystem::Unit& CombatSystem::team_weapon_source(const Unit& head) const {
    if (desc_of(head).weapon_projectile >= 0) return head;
    for (const Unit& m : units_)
        if (&m != &head && !(m.flags & kUnitDead) && land_team_head(m) == &head && desc_of(m).weapon_projectile >= 0) return m;
    return head;
}

Vec3 CombatSystem::can_hit_origin(ObjectId attacker) const {
    const Unit* u = unit(attacker);
    if (!u) return {};
    const Unit* head = land_team_lead(*u);
    return head ? scan_pos(*head) : u->pos;
}

CanHit CombatSystem::can_hit(ObjectId attacker, ObjectId target) const {
    return can_hit_from(attacker, can_hit_origin(attacker), target);
}

CanHit CombatSystem::can_hit_from(ObjectId attacker, Vec3 from, ObjectId target, const Vec3* to) const {
    const Unit* u = unit(attacker);
    const Unit* t = unit(target);
    if (!live(u) || !live(t)) return CanHit::NoTarget;
    const Unit* head = land_team_lead(*u);
    const Unit& src = head ? team_weapon_source(*head) : *u;
    // no weapon range and no grenade arc: nothing to test (0x40edd0 skips members without TARGETING)
    if (!grenade_weapon(src) && !(fire_range(base_attack_range(src), mod_r(src, ModList::FireRange)) > 0.0f)) return CanHit::NoTarget;
    const CanHit r = range_test(src, from, *t);
    if (r != CanHit::Yes) return r;
    return fire_line(src, from, *t, to);
}

// s78n t25.4 (MOVEMENT §4.3.2 item 7, guard blocks): the line-of-fire half of can_hit_from alone.
CanHit CombatSystem::line_of_fire_from(ObjectId attacker, Vec3 from, ObjectId target, const Vec3* to) const {
    const Unit* u = unit(attacker);
    const Unit* t = unit(target);
    if (!live(u) || !live(t)) return CanHit::NoTarget;
    const Unit* head = land_team_lead(*u);
    const Unit& src = head ? team_weapon_source(*head) : *u;
    // a member without TARGETING is not tested (as can_hit_from)
    if (!grenade_weapon(src) && !(fire_range(base_attack_range(src), mod_r(src, ModList::FireRange)) > 0.0f)) return CanHit::NoTarget;
    return fire_line(src, from, *t, to);
}

CanHit CombatSystem::fire_line(const Unit& src, Vec3 from, const Unit& tu, const Vec3* to) const {
    const Unit* t = &tu;  // (the body as it stood in can_hit_from)
    if (cfg_.mode == GameMode::Land && shield_sides_) {
        // 0x3b5ad0's projectile gate: every projectile the attacker would fire (its WEAPON projectile and its live weapon
        // hard points') must do shield damage for the shields to count
        bool shield_damage = true;
        const UnitDesc& ud = desc_of(src);
        if (ud.weapon_projectile >= 0 && !data_->projectiles[static_cast<size_t>(ud.weapon_projectile)].does_shield_damage) shield_damage = false;
        for (uint32_t i = 0; i < src.hp_count && shield_damage; ++i) {
            const Hardpoint& h = hps_[src.hp_begin + i];
            const HardpointDesc& hd = data_->hardpoints[h.desc];
            if (hp_out(h) || !is_weapon(hd.kind) || hd.projectile < 0) continue;
            if (!data_->projectiles[static_cast<size_t>(hd.projectile)].does_shield_damage) shield_damage = false;
        }
        Vec3 a = from, b = to ? *to : t->pos;  // bkt1 t06: the trim's G when given
        a.z = a.z + ud.ranged_target_z_adjust;
        b.z = b.z + desc_of(*t).ranged_target_z_adjust;
        if (shield_sides_(a, b, shield_damage)) return CanHit::LineBlocked;
    }
    return CanHit::Yes;
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

void CombatSystem::drop_clash_targets() {
    // s78n t06 (HERO_CLASH §5.2): the targeting services 0x634810 / 0x633ae0 / team 0x62eeb0 drop the current target of a
    // unit in a clash, and Validate_Current_Target 0x567c50 drops a target with +0x3b4 (reason 4). [port] Both run here,
    // once per tick before phase D (serial, index order, no draws), instead of at each unit's next scan / service: a
    // clashing unit's targeting is suspended (so retail drops it at the start), and a target that joins a clash can no
    // longer be fired at anyway (the pairing rule in Fire_At and the hardpoint pick).
    bool any = false;
    for (const Unit& u : units_)
        if ((u.flags & kUnitHeroClash) && !(u.flags & kUnitDead)) {
            any = true;
            break;
        }
    if (!any) return;
    auto flagged = [&](ObjectId id) {
        const Unit* t = unit(id);
        return t && (t->flags & kUnitHeroClash);
    };
    for (Unit& u : units_) {
        if (u.flags & kUnitDead) continue;
        if (u.flags & kUnitHeroClash) {
            if (u.ordered_target != kNoObject || u.auto_target != kNoObject) drop_target(u);
            u.aim_target = kNoObject;
            u.aim_hp = -1;
            continue;
        }
        if (flagged(u.ordered_target) || flagged(u.auto_target)) drop_target(u);
        if (flagged(u.aim_target)) {
            u.aim_target = kNoObject;
            u.aim_hp = -1;
        }
    }
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
        // first member's; s78n t20: team_weapon_source, shared with can_hit).
        const Unit* src = &team_weapon_source(head);
        s = 2;
        if (in_attack_range(*src, scan_pos(head), *t)) {  // s78n t03: P_U = the container
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
    // s78n t05: the hardpoint pick 0x385190 skips an incognito target (+0x3a1 & 0x10, HERO_CLASH §8.2)
    if (!live(&t) || (t.flags & (kUnitSkipTargeting | kUnitIncognito | kUnitGarrisoned))) return false;
    if (!clash_allows(u.id, t.id)) return false;  // s78n t06: the hardpoint pick's pairing rule (HERO_CLASH §5.2)
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
        // s78n t05 (HERO_CLASH §8.2, hardpoint auto-scan 0x387400): an owner with the incognito flag does not scan.
        // [port] the scan time is not advanced, so the first service after the flag clears scans.
        if (u.flags & kUnitIncognito) return false;
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
}  // namespace alamo::tactical::combat
