// --- L1 systems (owner: tactical-systems agent): land strikes - orbital bombardment (docs/re/L1_S4_REMAINING.md §3.8)
// and bombing runs (§3.7) of a campaign land battle (tactical_battle.hpp "land strikes"). Serial code; synced draws:
// the bombardment start (distribution cursor) and its scattered shots (angle, radius). [port] choices are marked.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include "alamo/core/dmath.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/sim/tactical_battle.hpp"

namespace alamo::sim {

namespace tc = alamo::tactical::combat;

namespace {

tactical::Vec3 tv3(Vec3 v) { return tactical::Vec3{v.x, v.y, v.z}; }

std::vector<std::string> split_names(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) return false;
    }
    return true;
}

constexpr float kBomberAltitude = 150.0f;  // [port] flight height over the ground (the LandBomber path height is not specified)

}  // namespace

// ---- data ----

TacticalBattle::L1Constants load_l1_constants(const data::GameConstants& gc) {
    TacticalBattle::L1Constants k;
    k.asteroid_field_damage = gc.get_float("Asteroid_Field_Damage", k.asteroid_field_damage);
    k.asteroid_field_damage_rate = gc.get_float("Asteroid_Field_Damage_Rate", k.asteroid_field_damage_rate);
    k.max_bombing_run_interval = gc.get_float("Max_Bombing_Run_Interval_Seconds", k.max_bombing_run_interval);
    k.min_bombing_run_interval = gc.get_float("Min_Bombing_Run_Interval_Seconds", k.min_bombing_run_interval);
    k.bombing_run_reduction_per_squadron =
        static_cast<int32_t>(gc.get_float("Bombing_Run_Reduction_Per_Squadron_Percent", static_cast<float>(k.bombing_run_reduction_per_squadron)));
    k.max_bombard_interval = gc.get_float("Max_Bombard_Interval_Seconds", k.max_bombard_interval);
    k.min_bombard_interval = gc.get_float("Min_Bombard_Interval_Seconds", k.min_bombard_interval);
    // battle host: Bombardment_Offset ("0 0 600": x y z) and Bombardment_Distribution (comma / space list of ints) from the
    // data; a missing tag or a malformed / empty value keeps the shipped default.
    auto numbers = [](std::string_view s) {
        std::vector<float> out;
        std::string cur;
        auto flush = [&] {
            if (cur.empty()) return;
            char* end = nullptr;
            const double v = std::strtod(cur.c_str(), &end);
            if (end && *end == '\0') out.push_back(static_cast<float>(v));
            else out.push_back(std::numeric_limits<float>::quiet_NaN());
            cur.clear();
        };
        for (const char c : s) {
            if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') flush();
            else cur.push_back(c);
        }
        flush();
        return out;
    };
    if (const auto v = gc.get("Bombardment_Offset")) {
        const std::vector<float> n = numbers(*v);
        if (n.size() == 3 && std::isfinite(n[0]) && std::isfinite(n[1]) && std::isfinite(n[2])) k.bombardment_offset = Vec3{n[0], n[1], n[2]};
    }
    if (const auto v = gc.get("Bombardment_Distribution")) {
        const std::vector<float> n = numbers(*v);
        std::vector<int32_t> d;
        bool ok = !n.empty();
        for (const float x : n) {
            ok = ok && std::isfinite(x) && x >= 0.0f && x < 1.0e6f;
            if (ok) d.push_back(static_cast<int32_t>(x));
        }
        if (ok) k.bombardment_distribution = std::move(d);
    }
    return k;
}

std::vector<TacticalBattle::L1Faction> load_l1_factions(const FileProvider& provider, const galactic::GalacticRules& rules) {
    std::vector<TacticalBattle::L1Faction> out(rules.factions.size());
    std::vector<uint8_t> bytes;
    if (!provider.read("Data\\XML\\FactionFiles.xml", bytes)) return out;
    const data::XmlDocument list = data::XmlDocument::parse(bytes, "FactionFiles.xml");
    for (data::XmlNode file : list.root().children("File")) {
        std::vector<uint8_t> fb;
        const std::string path = "Data\\XML\\" + std::string(file.text_trimmed());
        if (!provider.read(path, fb)) continue;
        const data::XmlDocument doc = data::XmlDocument::parse(fb, path);
        for (data::XmlNode n : doc.root().children("Faction")) {
            const uint32_t fi = rules.find_faction(n.attribute("Name"));
            if (fi >= out.size()) continue;
            TacticalBattle::L1Faction& f = out[fi];
            if (auto v = n.child_text("Bombardment_Required_Orbital_Ships"); !v.empty()) f.bombard_ships = split_names(v);
            if (auto v = n.child_text("Bombardment_Projectile"); !v.empty()) f.bombard_projectiles = split_names(v);
            if (auto v = n.child_text("Bombardment_Particle"); !v.empty()) f.bombard_particles = split_names(v);
            if (auto v = n.child_text("Bombardment_Number_Of_Salvos"); !v.empty()) f.salvos = objects::c_atoi(v);
            if (auto v = n.child_text("Bombardment_Shots_Per_Salvo"); !v.empty()) f.shots_per_salvo = objects::c_atoi(v);
            if (auto v = n.child_text("Bombardment_Shot_Delay"); !v.empty()) f.shot_delay = static_cast<float>(objects::c_atof(v));
            if (auto v = n.child_text("Bombardment_Salvo_Delay"); !v.empty()) f.salvo_delay = static_cast<float>(objects::c_atof(v));
            if (auto v = n.child_text("Bombardment_Initial_Delay"); !v.empty()) f.initial_delay = static_cast<float>(objects::c_atof(v));
        }
    }
    return out;
}

// [port] the Bombardment_Projectile types are not unit weapons, so the battle tables do not hold them: a minimal
// ProjectileDesc (speed, damage, damage type, blast area, lifetime / flight distance, shield / hitpoint flags,
// category) is appended per type at setup.
void TacticalBattle::l1_add_strike_projectiles() {
    if (space_) return;
    const objects::GameObjectTypes& types = catalog_->types();
    for (const L1Faction& f : l1_factions_)
        for (const std::string& name : f.bombard_projectiles) {
            const objects::TypeIndex t = types.resolve_name(name);
            if (t == objects::kNone || l1_projectile_of_type(static_cast<int32_t>(t)) >= 0) continue;
            tc::ProjectileDesc pd;
            pd.name = std::string(types.raw().types().name[t]);
            pd.max_speed = types.columns().max_speed[t];
            pd.damage = types.get_float(t, "Projectile_Damage");
            pd.damage_type = catalog_->damage().damage_index(types.get_string(t, "Damage_Type"));
            pd.max_flight_distance = types.get_float(t, "Projectile_Max_Flight_Distance");
            pd.max_lifetime = types.get_float(t, "Projectile_Max_Lifetime");
            pd.blast_area_damage = types.get_float(t, "Projectile_Blast_Area_Damage");
            pd.blast_area_range = types.get_float(t, "Projectile_Blast_Area_Range");
            pd.blast_max_victims = types.get_int(t, "Projectile_Blast_Area_Max_Victims");
            if (!types.get_string(t, "Projectile_Does_Shield_Damage").empty()) pd.does_shield_damage = types.get_bool(t, "Projectile_Does_Shield_Damage");
            if (!types.get_string(t, "Projectile_Does_Hitpoint_Damage").empty()) pd.does_hitpoint_damage = types.get_bool(t, "Projectile_Does_Hitpoint_Damage");
            const int32_t cat = tc::projectile_category_index(types.get_string(t, "Projectile_Category"));
            pd.category = static_cast<tc::ProjectileCategory>(cat < 0 ? 0 : cat);
            tables_->combat.projectiles.push_back(pd);
            l1_extra_projectiles_.emplace_back(static_cast<int32_t>(t), static_cast<int32_t>(tables_->combat.projectiles.size() - 1));
        }
}

int32_t TacticalBattle::l1_projectile_of_type(int32_t type) const {
    for (const auto& [t, p] : l1_extra_projectiles_)
        if (t == type) return p;
    if (type < 0) return -1;
    const std::string_view name = catalog_->types().raw().types().name[static_cast<size_t>(type)];
    for (size_t i = 0; i < tables_->combat.projectiles.size(); ++i)
        if (same_name(tables_->combat.projectiles[i].name, name)) return static_cast<int32_t>(i);
    return -1;
}

// ---- records (campaign init) ----

void TacticalBattle::l1_strikes_setup(TacticalMode& mode) {
    (void)mode;
    l1_bombard_.clear();
    l1_runs_.clear();
    if (space_) return;
    const int32_t player = req_.info.attacker;  // battle record [1] [M]
    if (player < 0 || static_cast<size_t>(player) >= galaxy_->players().size()) return;
    const uint32_t faction = galaxy_->players()[static_cast<size_t>(player)].faction;
    const objects::GameObjectTypes& types = catalog_->types();
    const objects::ObjectStore& gst = galaxy_->store();
    // The attacker's objects in orbit (fleet contents, squadron / company members expanded to their types).
    std::vector<uint32_t> orbit_types;
    for (const uint32_t fi : galaxy_->fleets_at(req_.info.planet, player)) {
        if (fi >= galaxy_->fleets().size()) continue;
        for (const objects::ObjectHandle h : galaxy_->fleets()[fi].contents) {
            if (!gst.alive(h)) continue;
            const uint32_t t = static_cast<uint32_t>(gst.type(h));
            orbit_types.push_back(t);
            for (const auto& [mt, n] : rules_->type(t).members) {
                (void)n;
                orbit_types.push_back(mt);
            }
        }
    }
    const uint32_t fps = 30;
    // Orbital bombardment (0x5284b0): a required-ship type in orbit (ship count irrelevant).
    if (faction < l1_factions_.size()) {
        const L1Faction& f = l1_factions_[faction];
        bool any = false;
        for (const uint32_t t : orbit_types)
            for (const std::string& s : f.bombard_ships) any = any || same_name(types.raw().types().name[t], s);
        if (any && !f.bombard_projectiles.empty()) {
            L1Bombard r;
            r.player = player;
            r.faction = faction;
            r.enabled = true;
            r.timer_only = false;
            r.interval = static_cast<uint32_t>(static_cast<float>(fps) * l1_k_.min_bombard_interval);
            r.ready = frame_ + r.interval;
            r.start = frame_;
            l1_bombard_.push_back(r);
        }
    }
    // Bombing run (0x493d50): Ship_Class 1 bombers in orbit; n = max(count, 3); r = (n/3 - 1) x reduction %;
    // f = max(0, m + 1 - r) with m = the defender's Bombing_Run_Percent_Modifier objects [port: 0, no base layout];
    // interval = max(int(FPS x Min), int(int(FPS x Max) x f)).
    int32_t bombers = 0, bomber_type = -1;
    for (const uint32_t t : orbit_types)
        if (types.get_int(t, "Ship_Class") == 1 || same_name(types.get_string(t, "Ship_Class"), "bomber")) {  // fighter 0, bomber 1
            ++bombers;
            const objects::TypeIndex bt = types.get_type_ref(t, "Land_Bomber_Type");
            if (bt != objects::kNone && bomber_type < 0) bomber_type = static_cast<int32_t>(bt);
        }
    if (bombers > 0 && bomber_type >= 0) {
        const float n = static_cast<float>(std::max(bombers, 3));
        const float red = (n / 3.0f - 1.0f) * static_cast<float>(l1_k_.bombing_run_reduction_per_squadron) / 100.0f;  // [M] percent
        const float fct = std::max(0.0f, 0.0f + 1.0f - red);
        const uint32_t base = static_cast<uint32_t>(static_cast<float>(fps) * l1_k_.max_bombing_run_interval);
        L1BombingRun r;
        r.player = player;
        r.enabled = true;
        r.bomber_type = bomber_type;
        r.interval = std::max(static_cast<uint32_t>(static_cast<float>(fps) * l1_k_.min_bombing_run_interval),
                              static_cast<uint32_t>(static_cast<float>(base) * fct));
        r.ready = frame_ + r.interval;
        r.last_start = frame_;
        l1_runs_.push_back(r);
    }
    if (!l1_bombard_.empty() || !l1_runs_.empty()) l1_used_ = true;
}

// ---- validity ----

bool TacticalBattle::l1_base_shield_covers(Vec3 p) const {
    if (!l1_mode_) return false;
    static const objects::BehaviorTypeId kBaseShield = objects::behavior_type_id("BASE_SHIELD");
    const objects::ObjectStore& st = l1_mode_->primary();
    for (const objects::ObjectHandle h : st.all_objects()) {
        if (!st.active(h) || kBaseShield == objects::kNoBehavior || !st.has_behavior(h, kBaseShield)) continue;
        const float r = catalog_->types().get_float(st.type(h), "Base_Shield_Radius");  // [port] a live shield is active
        const Vec3 c = st.position(h);
        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        if (r > 0.0f && dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

bool TacticalBattle::bombardment_target_valid(int32_t player, Vec3 target) const {
#ifdef ALAMO_SIM_HAVE_FOG
    if (fog_point_fogged(player, target.x, target.y, true)) return false;  // force 1
#else
    (void)player;
#endif
    return !l1_base_shield_covers(target);
}

bool TacticalBattle::bombing_run_target_valid(int32_t player, Vec3 target) const {
    if (!bombardment_target_valid(player, target) || !l1_mode_) return false;
    const objects::ObjectStore& st = l1_mode_->primary();
    for (const objects::ObjectHandle h : st.all_objects()) {  // Bombing_Run_Prevention_Radius of objects not the player's
        if (!st.active(h) || st.owner(h) == player) continue;
        const float r = static_cast<float>(catalog_->types().get_int(st.type(h), "Bombing_Run_Prevention_Radius"));
        if (!(r > 0.0f)) continue;
        const Vec3 c = st.position(h);
        const float dx = target.x - c.x, dy = target.y - c.y, dz = target.z - c.z;
        if (dx * dx + dy * dy + dz * dz <= r * r) return false;
    }
    return true;
}

bool TacticalBattle::bombardment_available(int32_t player) const {
    for (const L1Bombard& r : l1_bombard_)
        if (r.player == player) return r.enabled && r.available && !r.in_progress && !r.disabled;
    return false;
}

bool TacticalBattle::bombing_run_available(int32_t player) const {
    for (const L1BombingRun& r : l1_runs_)
        if (r.player == player) return r.enabled && r.available && !r.in_progress && !r.disabled;
    return false;
}

void TacticalBattle::set_bombardment_disabled(int32_t player, bool disabled) {
    for (L1Bombard& r : l1_bombard_)
        if (r.player == player) r.disabled = disabled;
}
void TacticalBattle::remove_bombardment(int32_t player, bool removed) {
    for (L1Bombard& r : l1_bombard_)
        if (r.player == player) r.enabled = !removed;
}
void TacticalBattle::set_bombing_run_disabled(int32_t player, bool disabled) {
    for (L1BombingRun& r : l1_runs_)
        if (r.player == player) r.disabled = disabled;
}

bool TacticalBattle::bombing_run_finished(int32_t player) const {
    for (const L1BombingRun& r : l1_runs_)
        if (r.player == player) return r.bombers.empty();
    return true;
}
float TacticalBattle::bombing_run_result(int32_t player) const {
    for (const L1BombingRun& r : l1_runs_)
        if (r.player == player) return r.total ? 1.0f : 0.0f;  // [port] bombers cannot be shot down here
    return 0.0f;
}
uint32_t TacticalBattle::bombing_run_frames_until_available(int32_t player) const {
    for (const L1BombingRun& r : l1_runs_)
        if (r.player == player) return r.ready > frame_ ? r.ready - frame_ : 0u;
    return 0u;
}

// ---- start ----

bool TacticalBattle::start_bombardment(TacticalMode& mode, int32_t player, Vec3 target, SimRng& rng) {
    (void)mode;
    for (L1Bombard& r : l1_bombard_) {
        if (r.player != player) continue;
        if (!r.enabled || finished_) return false;               // silent
        if (!bombardment_target_valid(player, target)) return false;  // silent (fog force 1, base shield)
        if (!bombardment_available(player)) return false;            // logged + local error SFX
        const L1Faction& f = l1_factions_[r.faction];
        r.available = false;
        r.in_progress = true;
        r.start = frame_;
        r.shot = r.salvo = 0;
        r.target = target;
        const uint32_t init = static_cast<uint32_t>(30.0f * f.initial_delay);
        const uint32_t d = init;  // the BOMBARD_DELAY story override (mgr +0x50) is not modelled [port]
        r.next_shot = frame_ + static_cast<uint32_t>(30.0f * f.shot_delay) + d;
        const int32_t count = static_cast<int32_t>(l1_k_.bombardment_distribution.size());
        r.cursor = static_cast<uint32_t>(rng.range_int(0, std::max(0, count - 1)));  // drawn even without scatter [H]
        if (!f.bombard_particles.empty() && l1_mode_) {  // extra == 0: particle[0] now
            const objects::TypeIndex pt = catalog_->types().resolve_name(f.bombard_particles[0]);
            if (pt != objects::kNone) spawn_particle(*l1_mode_, static_cast<int32_t>(pt), target, player);
        }
        r.deferred = -1;
        return true;
    }
    return false;
}

bool TacticalBattle::start_bombing_run(TacticalMode& mode, int32_t player, Vec3 target) {
    for (L1BombingRun& r : l1_runs_) {
        if (r.player != player) continue;
        if (!r.enabled || finished_ || !bombing_run_target_valid(player, target) || !bombing_run_available(player)) return false;
        r.available = false;
        r.in_progress = true;
        r.last_start = frame_;
        // Path (0x4935e0): a straight line through the target from map edge to edge [port: along +x of the bounds].
        const tactical::Aabb& b = mode.bounds();
        const Vec3 dir{1.0f, 0.0f, 0.0f};
        const Vec3 entry{b.min.x, target.y, 0.0f}, exit{b.max.x, target.y, 0.0f};
        const objects::GameObjectTypes& types = catalog_->types();
        const objects::TypeIndex bt = static_cast<objects::TypeIndex>(r.bomber_type);
        const objects::TypeIndex mt = types.resolve_name("IN_GAME_BOMBING_RUN_MARKER");
        const Vec3 off[3] = {{0.0f, 0.0f, 0.0f}, {-70.0f, 70.0f, 0.0f}, {-70.0f, -70.0f, 0.0f}};  // wedge (path frame)
        r.bombers.clear();
        for (const Vec3 o : off) {
            L1Bomber bm;
            const float gz = std::max(ground_height(entry.x + o.x, entry.y + o.y), -1.0e6f) + kBomberAltitude;
            objects::SpawnParams sp;
            sp.type = bt;
            sp.owner = player;
            sp.position = {entry.x + o.x, entry.y + o.y, gz};
            sp.facing = dir;
            const objects::ObjectHandle h = mode.spawn(sp, 1.0f);
            if (!h) continue;
            bm.id = mode.primary().id(h);
            bm.pos = sp.position;
            bm.dir = dir;
            bm.exit = Vec3{exit.x + o.x, exit.y + o.y, gz};
            bm.speed = std::max(types.columns().max_speed[bt], 0.0f) * 1.56f;  // land mode multiplier
            bm.range = types.columns().targeting_max_attack_distance[bt];
            bm.recharge = types.get_float(bt, "Projectile_Fire_Recharge_Seconds");
            const auto refs = types.get_type_refs(bt, "Projectile_Types");
            bm.projectile = refs.empty() ? -1 : l1_projectile_of_type(static_cast<int32_t>(refs.front()));
            bm.next_fire = frame_;
            if (mt != objects::kNone) {
                objects::SpawnParams ms;
                ms.type = mt;
                ms.owner = req_.info.defender == player ? req_.info.attacker : player;  // [M] the "system" marker owner
                ms.position = {target.x + o.x, target.y + o.y, ground_height(target.x + o.x, target.y + o.y)};
                ms.facing = dir;
                bm.marker = mode.primary().id(mode.primary().create(ms));
            }
            r.bombers.push_back(bm);
        }
        r.total = static_cast<uint32_t>(r.bombers.size());
        return true;
    }
    return false;
}

// ---- service ----

void TacticalBattle::l1_strikes(TacticalMode& mode, SimRng& rng) {
    if (space_ || (l1_bombard_.empty() && l1_runs_.empty())) return;
    objects::ObjectStore& st = mode.primary();
    // Orbital bombardment (0x52a110).
    for (L1Bombard& r : l1_bombard_) {
        if (!r.in_progress) {
            r.available = r.timer_only ? r.ready <= frame_ : (!r.disabled && r.ready <= frame_);
            continue;
        }
        const L1Faction& f = l1_factions_[r.faction];
        if (r.deferred >= 0 && r.deferred < static_cast<int64_t>(frame_)) r.deferred = -1;
        if (!(r.next_shot < frame_)) continue;
        const size_t pi = std::min<size_t>(static_cast<size_t>(r.salvo), f.bombard_projectiles.size() - 1);
        const objects::TypeIndex ptype = catalog_->types().resolve_name(f.bombard_projectiles[pi]);
        Vec3 aim = r.target;
        if (f.shots_per_salvo > 1 && !(r.salvo == 0 && r.shot == 0) && !l1_k_.bombardment_distribution.empty()) {
            const int32_t q = l1_k_.bombardment_distribution[r.cursor % l1_k_.bombardment_distribution.size()];
            r.cursor = (r.cursor + 1) % static_cast<uint32_t>(l1_k_.bombardment_distribution.size());
            const float a = rng.range_float(static_cast<float>(q) * 90.0f, static_cast<float>(q + 1) * 90.0f - 1.0f);
            const float rad = rng.range_float(0.0f, 100.0f);  // [port] the radius range is lost in the decompile [M]
            float s = 0.0f, c = 1.0f;
            dmath::sincos(a * (dmath::kPiF / 180.0f), s, c);
            aim.x = aim.x + c * rad;
            aim.y = aim.y + s * rad;
        }
        const Vec3 from{aim.x + l1_k_.bombardment_offset.x, aim.y + l1_k_.bombardment_offset.y, aim.z + l1_k_.bombardment_offset.z};
        const int32_t proj = ptype == objects::kNone ? -1 : l1_projectile_of_type(static_cast<int32_t>(ptype));
        if (proj >= 0) {
            tc::ProjectileLaunch l;
            l.shooter = tc::kNoObject;
            l.owner = r.player;
            l.projectile = static_cast<uint32_t>(proj);
            l.from = tv3(from);
            l.point = tv3(aim);
            combat_->launch_projectile(l, frame_);
        }
        // Advance: next shot / next salvo (particle) / finish.
        if (r.shot + 1 < f.shots_per_salvo) {
            ++r.shot;
            r.next_shot += static_cast<uint32_t>(30.0f * f.shot_delay);
        } else if (r.salvo + 1 < f.salvos) {
            r.shot = 0;
            ++r.salvo;
            r.next_shot += static_cast<uint32_t>(30.0f * f.salvo_delay);
            if (!f.bombard_particles.empty()) {
                const size_t k = std::min<size_t>(static_cast<size_t>(r.salvo), f.bombard_particles.size() - 1);
                const objects::TypeIndex pt = catalog_->types().resolve_name(f.bombard_particles[k]);
                if (pt != objects::kNone) spawn_particle(mode, static_cast<int32_t>(pt), r.target, r.player);
            }
        } else {
            r.ready = frame_ + r.interval;
            r.in_progress = false;
            r.available = false;
            static const objects::BehaviorTypeId kSweep = objects::behavior_type_id("DUMMY_DESTROY_AFTER_PLANETARY_BOMBARD");
            const auto owned = st.owned_by(r.player);
            const std::vector<objects::ObjectHandle> list(owned.begin(), owned.end());
            for (const objects::ObjectHandle h : list)
                if (st.alive(h) && kSweep != objects::kNoBehavior && st.has_behavior(h, kSweep)) mode.destroy(h);
        }
    }
    // Bombing runs (0x495dd0): bombers fly the path, bomb while within range of their marker, leave at the exit.
    for (L1BombingRun& r : l1_runs_) {
        if (!r.in_progress) {
            r.available = !r.disabled && r.ready <= frame_;
            continue;
        }
        for (L1Bomber& bm : r.bombers) {
            const objects::ObjectHandle h = st.find(bm.id);
            if (!st.alive(h)) {  // removed bomber: its marker goes too (0x492910)
                if (const objects::ObjectHandle dm = st.find(bm.marker); st.alive(dm)) st.destroy(dm);
                bm.id = objects::kNoObjectId;
                continue;
            }
            bm.pos = Vec3{bm.pos.x + bm.dir.x * bm.speed, bm.pos.y + bm.dir.y * bm.speed, bm.pos.z};
            st.set_position(h, bm.pos);
            mode.trees().update(static_cast<tactical::EntityHandle>(bm.id), mode.bounds_of(h));
            const objects::ObjectHandle mh = st.find(bm.marker);
            if (st.alive(mh) && bm.projectile >= 0 && frame_ >= bm.next_fire) {
                const Vec3 m = st.position(mh);
                const float dx = m.x - bm.pos.x, dy = m.y - bm.pos.y;
                if (dx * dx + dy * dy <= bm.range * bm.range) {  // [port] the bomber's own weapon at its marker
                    tc::ProjectileLaunch l;
                    l.shooter = bm.id;
                    l.owner = r.player;
                    l.projectile = static_cast<uint32_t>(bm.projectile);
                    l.from = tv3(bm.pos);
                    l.point = tv3(m);
                    combat_->launch_projectile(l, frame_);
                    bm.next_fire = frame_ + std::max(1u, static_cast<uint32_t>(30.0f * bm.recharge));
                }
            }
            if ((bm.exit.x - bm.pos.x) * bm.dir.x + (bm.exit.y - bm.pos.y) * bm.dir.y <= 0.0f) {  // past the exit
                mode.destroy(h);
                if (st.alive(mh)) mode.primary().destroy(mh);
                bm.id = objects::kNoObjectId;
            }
        }
        std::erase_if(r.bombers, [](const L1Bomber& b) { return b.id == objects::kNoObjectId; });
        if (r.bombers.empty()) {  // end: sweep, ready := now + interval
            static const objects::BehaviorTypeId kSweep = objects::behavior_type_id("DUMMY_DESTROY_AFTER_BOMBING_RUN");
            const auto owned = st.owned_by(r.player);
            const std::vector<objects::ObjectHandle> list(owned.begin(), owned.end());
            for (const objects::ObjectHandle h : list)
                if (st.alive(h) && kSweep != objects::kNoBehavior && st.has_behavior(h, kSweep)) mode.destroy(h);
            r.ready = frame_ + r.interval;
            r.available = false;
            r.in_progress = false;
        }
    }
}

}  // namespace alamo::sim
