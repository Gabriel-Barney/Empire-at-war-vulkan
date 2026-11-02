#include "alamo/sim/battle_catalog.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef ALAMO_SIM_HAVE_ASSETS
#include "alamo/assets/model.hpp"  // battle host: model bounds / bind-pose bones
#endif

#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"
#include "alamo/tactical/movement/unit_params.hpp"

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string upper_str(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = upper(c);
    return out;
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

void note(std::vector<std::string>* d, std::string m) {
    if (d) d->push_back(std::move(m));
}

tc::HardpointKind hardpoint_kind(std::string_view type) {
    static const std::pair<const char*, tc::HardpointKind> kTable[] = {
        {"HARD_POINT_ENGINE", tc::HardpointKind::Engine},
        {"HARD_POINT_SHIELD_GENERATOR", tc::HardpointKind::ShieldGenerator},
        {"HARD_POINT_GRAVITY_WELL", tc::HardpointKind::GravityWell},
        {"HARD_POINT_FIGHTER_BAY", tc::HardpointKind::FighterBay},
        {"HARD_POINT_TRACTOR_BEAM", tc::HardpointKind::TractorBeam},
        {"HARD_POINT_WEAPON_LASER", tc::HardpointKind::WeaponLaser},
        {"HARD_POINT_WEAPON_MISSILE", tc::HardpointKind::WeaponMissile},
        {"HARD_POINT_WEAPON_TORPEDO", tc::HardpointKind::WeaponTorpedo},
        {"HARD_POINT_WEAPON_ION_CANNON", tc::HardpointKind::WeaponIonCannon},
        {"HARD_POINT_WEAPON_MASS_DRIVER", tc::HardpointKind::WeaponMassDriver},
        {"HARD_POINT_WEAPON_SPECIAL", tc::HardpointKind::WeaponSpecial},
        {"HARD_POINT_ENABLE_SPECIAL_ABILITY", tc::HardpointKind::EnableSpecialAbility},
        {"HARD_POINT_DUMMY_ART", tc::HardpointKind::DummyArt},
    };
    for (const auto& [n, k] : kTable)
        if (iequal(type, n)) return k;
    return tc::HardpointKind::DummyArt;
}

std::unique_ptr<data::XmlDocument> read_doc(const FileProvider& files, const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return nullptr;
    auto doc = std::make_unique<data::XmlDocument>(data::XmlDocument::parse(bytes, path));
    if (!doc->ok()) return nullptr;
    return doc;
}

uint8_t clamp_count(size_t n) { return static_cast<uint8_t>(std::min<size_t>(n, 255)); }

}  // namespace

BattleCatalog::BattleCatalog(const objects::GameObjectTypes& types, const FileProvider* files,
                             const data::GameConstants* constants, std::vector<std::string>* diag)
    : types_(&types), files_(files), model_cache_(std::make_shared<ModelCache>()) {
    if (constants) {
        damage_ = tc::DamageArmorTable::from_constants(*constants, diag);
        space_health_mult_ = constants->get_float("Object_Max_Health_Multiplier_Space", space_health_mult_);
        space_idle_speed_ = constants->get_float("SpaceIdleMovementSpeed", space_idle_speed_);
        presence_relaxation_ = constants->get_float("Unit_Presence_Relaxation_Time", presence_relaxation_);
        land_health_mult_ = constants->get_float("Object_Max_Health_Multiplier_Land", land_health_mult_);
        land_capture_countdown_ = constants->get_float("Land_Capture_Allowed_Countdown_Seconds", land_capture_countdown_);
        space_capture_countdown_ = constants->get_float("Space_Capture_Allowed_Countdown_Seconds", space_capture_countdown_);
        garrisoned_range_mult_ = constants->get_float("Garrisoned_Max_Attack_Distance_Multiplier", garrisoned_range_mult_);
        threat_decay_step_ = constants->get_float("AI_SpaceThreatDecayStep", threat_decay_step_);
        engines_disabled_speed_ = constants->get_float("Engines_Disabled_Speed_Modifier", engines_disabled_speed_);
        nebula_ability_disable_ = constants->get_float("Nebula_Ability_Disable_Time", nebula_ability_disable_);
        force_ability_disable_ = constants->get_float("Force_Ability_Disable_Time", force_ability_disable_);
        ion_storm_shield_disable_ = constants->get_float("Ion_Storm_Shield_Disable_Time", ion_storm_shield_disable_);
        tc::apply_game_constants(combat_config_, *constants);  // OI tactical (TACTICAL_GAPS 3.2 / 3.3)
        // --- battle host: transition constants (TACTICAL_GAPS §4.2 raid / lead, §4.9 retreat attrition) ---
        TransitionConstants& k = transition_;
        k.space_retreat_attrition = constants->get_float("Space_Retreat_Attrition_Factor", k.space_retreat_attrition);
        k.land_retreat_attrition = constants->get_float("Land_Retreat_Attrition_Factor", k.land_retreat_attrition);
        k.blockade_run_attrition = constants->get_float("Blockade_Run_Attrition_Factor", k.blockade_run_attrition);
        auto words = [](std::string_view s, bool bars) {
            std::vector<std::string> out;
            std::string cur;
            for (const char c : s) {
                const bool sep = c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || (bars && c == '|');
                if (!sep) cur.push_back(c);
                else if (!cur.empty()) out.push_back(std::exchange(cur, {}));
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        };
        if (const auto v = constants->get("Preferred_Pathfinder_Types")) k.preferred_pathfinder_types = words(*v, false);
        if (const auto v = constants->get("Raid_Force_Required_Faction")) k.raid_required_faction = std::string(trim(*v));
        const objects::EnumConverter& cat = types.enums().get(objects::EnumId::GameObjectCategory);
        auto mask = [&](std::string_view tag) {
            uint64_t m = 0;
            if (const auto v = constants->get(tag))
                for (const std::string& w : words(*v, true))
                    if (const auto b = cat.lookup(w)) m |= static_cast<uint64_t>(*b);
            return m;
        };
        k.raid_free_mask = mask("Raid_Force_Free_Object_Category_Mask");
        k.raid_limited_mask = mask("Raid_Force_Limited_Object_Category_Mask");
        k.raid_max_limited = constants->get_int("Raid_Force_Max_Limited_Objects", 0);
        k.raid_max_heroes = constants->get_int("Raid_Force_Max_Heros", 0);
        // --- end battle host ---
#ifdef ALAMO_SIM_HAVE_FOG
        // --- fog of war constants (owner: fog agent) ---
        tactical::fog::FogConstants& f = fog_constants_;
        f.desired_land_cell_size = constants->get_float("DesiredLandFOWCellSize", f.desired_land_cell_size);
        f.desired_space_cell_size = constants->get_float("DesiredSpaceFOWCellSize", f.desired_space_cell_size);
        f.land_regrow_time = constants->get_float("LandFOWRegrowTime", f.land_regrow_time);
        f.space_regrow_time = constants->get_float("SpaceFOWRegrowTime", f.space_regrow_time);
        f.ai_uses_fog_land = constants->get_bool("AIUsesFogOfWarLand", f.ai_uses_fog_land);
        f.ai_uses_fog_space = constants->get_bool("AIUsesFogOfWarSpace", f.ai_uses_fog_space);
        f.tactical_overrun_multiple = constants->get_float("Tactical_Overrun_Multiple", f.tactical_overrun_multiple);
        f.min_overrun_seconds = constants->get_float("Minimum_Tactical_Overrun_Time_In_Secs", f.min_overrun_seconds);
        f.setup_phase_enabled = constants->get_bool("SetupPhaseEnabled", f.setup_phase_enabled);  // OI fog
        // --- end fog ---
#endif
    }
    if (files) {
        load_hardpoints(*files, diag);
        load_priority_sets(*files, diag);
    }
}

ta::AbilityConfig BattleCatalog::ability_config(bool space, bool skirmish) const {
    ta::AbilityConfig c;
    c.mode = space ? tc::GameMode::Space : tc::GameMode::Land;
    c.skirmish = skirmish;
    c.nebula_ability_disable_time = nebula_ability_disable_;
    c.force_ability_disable_time = force_ability_disable_;
    c.ion_storm_shield_disable_time = ion_storm_shield_disable_;
    const auto corvette = types_->enums().get(objects::EnumId::GameObjectCategory).lookup("Corvette");
    c.corvette_category = corvette ? static_cast<uint64_t>(*corvette) : 0;
    return c;
}

void BattleCatalog::load_hardpoints(const FileProvider& files, std::vector<std::string>* diag) {
    auto list = read_doc(files, "Data\\XML\\HardPointDataFiles.xml");
    if (!list) {
        note(diag, "battle: HardPointDataFiles.xml not found");
        return;
    }
    for (data::XmlNode f : list->root().children("File")) {
        const std::string path = "Data\\XML\\" + std::string(f.text_trimmed());
        auto doc = read_doc(files, path);
        if (!doc) {
            note(diag, "battle: " + path + " not found");
            continue;
        }
        for (data::XmlNode hp : doc->root().children("HardPoint")) {
            Tags tags;
            const std::string_view variant = hp.child_text("Variant_Of_Existing_Type");
            if (!variant.empty()) {
                const auto it = hardpoints_.find(upper_str(variant));
                if (it != hardpoints_.end()) tags = it->second;
            }
            for (data::XmlNode c = hp.first_child(); c; c = c.next_sibling()) {
                const std::string tag = upper_str(c.name());
                const std::string_view text = c.text_trimmed();
                // Repeated list tags (Fire_Inaccuracy_Distance) append; other tags replace.
                bool replaced = false;
                if (tag != "FIRE_INACCURACY_DISTANCE")
                    for (auto& [t, v] : tags)
                        if (t == tag) {
                            v = std::string(text);
                            replaced = true;
                        }
                if (!replaced) tags.emplace_back(tag, std::string(text));
            }
            hardpoints_[upper_str(hp.attribute("Name"))] = std::move(tags);  // last definition wins
        }
    }
}

void BattleCatalog::load_priority_sets(const FileProvider& files, std::vector<std::string>* diag) {
    auto list = read_doc(files, "Data\\XML\\TargetingPrioritySetFiles.xml");
    if (!list) {
        note(diag, "battle: TargetingPrioritySetFiles.xml not found");
        return;
    }
    const objects::GameObjectTypes& types = *types_;
    const tc::TypeResolver resolve = [&types](std::string_view name) -> int32_t {
        const objects::TypeIndex t = types.resolve_name(name);
        return t == objects::kNone ? -1 : static_cast<int32_t>(t);
    };
    for (data::XmlNode f : list->root().children("File")) {
        const std::string path = "Data\\XML\\" + std::string(f.text_trimmed());
        auto doc = read_doc(files, path);
        if (!doc) {
            note(diag, "battle: " + path + " not found");
            continue;
        }
        std::vector<tc::PrioritySet> sets = tc::load_priority_sets(doc->root(), types.enums(), resolve, nullptr);
        for (tc::PrioritySet& s : sets) priority_sets_.push_back(std::move(s));
    }
}

int32_t BattleCatalog::type_or_none(std::string_view name) const {
    name = trim(name);
    if (name.empty()) return -1;
    const objects::TypeIndex t = types_->resolve_name(name);
    return t == objects::kNone ? -1 : static_cast<int32_t>(t);
}

int32_t BattleCatalog::projectile(uint32_t t, BuildState& s) const {
    if (t >= types_->size()) return -1;
    const auto it = s.proj_index.find(t);
    if (it != s.proj_index.end()) return it->second;
    const objects::GameObjectTypes& ty = *types_;
    tc::ProjectileDesc p;
    p.name = std::string(ty.raw().types().name[t]);
    p.max_speed = ty.columns().max_speed[t];
    p.damage = ty.get_float(t, "Projectile_Damage");
    p.damage_type = damage_.damage_index(trim(ty.get_string(t, "Damage_Type")));
    const int32_t cat = ty.get_int(t, "Projectile_Category");
    p.homing = cat == 1;
    p.max_rate_of_turn = ty.get_float(t, "Max_Rate_Of_Turn");  // S9 t05: missile-shield / homing turn limit
    p.causes_infection = ty.get_bool(t, "Causes_Infection");    // S9 t04: INFECTION behaviour on hit
    p.lobbed = cat == 3;
    // --- OI tactical: per-type grenade gravity / lob mod (TACTICAL_COMBAT 2.2, 3.3; lobbed flight in combat) ---
    if (ty.get_float(t, "Projectile_Grenade_Gravity") > 0.0f) p.grenade_gravity = ty.get_float(t, "Projectile_Grenade_Gravity");
    if (ty.get_float(t, "Projectile_Grenade_Gravity_Lob_Mod") > 0.0f) p.lob_mod = ty.get_float(t, "Projectile_Grenade_Gravity_Lob_Mod");
    // --- end OI tactical ---
    if (!ty.get_string(t, "Projectile_Does_Shield_Damage").empty()) p.does_shield_damage = ty.get_bool(t, "Projectile_Does_Shield_Damage");
    if (!ty.get_string(t, "Projectile_Does_Hitpoint_Damage").empty())
        p.does_hitpoint_damage = ty.get_bool(t, "Projectile_Does_Hitpoint_Damage");
    p.max_flight_distance = ty.get_float(t, "Projectile_Max_Flight_Distance");
    p.max_lifetime = ty.get_float(t, "Projectile_Max_Lifetime");
    p.blast_area_damage = ty.get_float(t, "Projectile_Blast_Area_Damage");
    p.blast_area_range = ty.get_float(t, "Projectile_Blast_Area_Range");
    p.damages_random_hardpoints = ty.get_bool(t, "Projectile_Damages_Random_Hard_Points");
    if (!(p.max_speed > 0.0f)) p.max_speed = 20.0f;  // [port] a projectile type without Max_Speed still flies
    if (s.full) {
        // S10-int: S9 projectile fields (ABILITIES_HARDPOINTS 4.3, 4.4, 4.7; TACTICAL_COMBAT 3.5).
        if (cat >= 0 && cat <= 9) p.category = static_cast<tc::ProjectileCategory>(cat);
        // refixf t01: ROCKET / MPTL_ROCKET spline flight (S9_DEATH_CLONE_SHIELDS.md §2.4). The distances use converter 0x8
        // (float; type +0x1fec / +0x1ff0 / +0x1ff4), 0 when absent; the two flags are type +0x7f / +0x7e.
        p.rocket_straight_distance = ty.get_float(t, "Projectile_Rocket_Straight_Distance");
        p.rocket_curve_distance = ty.get_float(t, "Projectile_Rocket_Curve_Distance");
        p.rocket_curve_offset = ty.get_float(t, "Projectile_Rocket_Curve_Offset");
        p.target_point_on_terrain = ty.get_bool(t, "Projectile_Target_Point_On_Terrain");
        p.explode_when_reached_target_radius = ty.get_bool(t, "Explode_When_Reached_Target_Radius");
        p.damage_delay_secs = ty.get_float(t, "Projectile_Damage_Delay_Secs");
        p.ion_stun = ty.get_bool(t, "Projectile_Ion_Stun_On_Detonation");
        p.ion_stun_duration = ty.get_float(t, "Projectile_Ion_Stun_Duration");
        p.ion_stun_speed_reduction = ty.get_float(t, "Projectile_Ion_Stun_Speed_Reduction_Percent");
        p.ion_stun_shot_rate_reduction = ty.get_float(t, "Projectile_Ion_Stun_Shot_Rate_Reduction_Percent");
        p.energy_per_shot = ty.get_float(t, "Projectile_Energy_Per_Shot");
        p.does_energy_damage = ty.get_bool(t, "Projectile_Does_Energy_Damage");
        // --- OI abilities: detonation effects 0x4cb070 (TACTICAL_GAPS §1.10) ---
        p.ion_stun_stack_duration = ty.get_bool(t, "Projectile_Ion_Stun_Stack_Duration");
        p.ion_stun_radius = ty.get_float(t, "Projectile_Ion_Stun_Radius");
        p.stun = ty.get_bool(t, "Projectile_Stun_On_Detonation");
        p.stun_frames = ty.get_int(t, "Projectile_Stun_Duration_Frames");
        p.stun_radius = ty.get_float(t, "Projectile_Stun_Radius");
        p.stun_mask = ty.get_mask(t, "Projectile_Stun_Victims_Category_Mask");
        for (std::string_view n : ty.get_list(t, "Projectile_Stun_Victims_Unit_Types")) p.stun_types.push_back(type_or_none(n));
        p.distract = ty.get_bool(t, "Projectile_Distract_On_Detonation");
        p.distract_radius = ty.get_float(t, "Projectile_Distract_Radius");
        p.distract_mask = ty.get_mask(t, "Projectile_Distract_Victims_Category_Mask");
        p.heal = ty.get_bool(t, "Projectile_Instant_Heal_On_Detonation");
        p.heal_radius = ty.get_float(t, "Projectile_Instant_Heal_Radius");
        p.heal_mask = ty.get_mask(t, "Projectile_Instant_Heal_Targets_Category_Mask");
        p.heal_amount = ty.get_int(t, "Projectile_Instant_Heal_Health_Increase");
        p.convert = ty.get_bool(t, "Projectile_Convert_Enemy_On_Detonation");
        p.convert_radius = ty.get_float(t, "Projectile_Convert_Enemy_Radius");
        p.convert_mask = ty.get_mask(t, "Projectile_Convert_Enemy_Targets_Category_Mask");
        p.invulnerability = ty.get_bool(t, "Projectile_Cause_Invulnerability_On_Detonation");
        p.invulnerability_frames = ty.get_int(t, "Projectile_Cause_Invulnerability_Duration_Frames");
        p.invulnerability_radius = ty.get_float(t, "Projectile_Cause_Invulnerability_Radius");
        p.invulnerability_max_targets = ty.get_int(t, "Projectile_Cause_Invulnerability_Max_Targets");
        p.invulnerability_mask = ty.get_mask(t, "Projectile_Cause_Invulnerability_Targets_Category_Mask");
        p.weaken = ty.get_bool(t, "Projectile_Weaken_Enemy_On_Detonation");
        p.weaken_seconds = ty.get_float(t, "Projectile_Weaken_Enemy_Duration_Seconds");
        p.weaken_radius = ty.get_float(t, "Projectile_Weaken_Enemy_Radius");
        p.weaken_take_increase = ty.get_float(t, "Projectile_Weaken_Enemy_Take_Damage_Increase_Percent");
        p.weaken_cause_reduction = ty.get_float(t, "Projectile_Weaken_Enemy_Cause_Damage_Reduction_Percent");
        p.weaken_mask = ty.get_mask(t, "Projectile_Weaken_Enemy_Targets_Category_Mask");
        {  // Projectile_Combat_Mod: field code 0x52 (TACTICAL_GAPS §6.5) - ModList index + 1, unknown token = none
            static constexpr const char* kMods[] = {"DEFENSE", "DAMAGE", "HEALTH", "SHIELD", "ENERGY", "SPEED",
                                                   "FIRE_RATE", "FOW_REVEAL_RANGE", "FIRE_RANGE", "ABILITY_RECHARGE_TIME",
                                                   "TACTICAL_BUILD_TIME"};
            const std::string_view m = trim(ty.get_string(t, "Projectile_Combat_Mod"));
            for (size_t k = 0; k < std::size(kMods) && !m.empty(); ++k)
                if (iequal(m, kMods[k])) p.combat_mod = static_cast<uint8_t>(k + 1);
            p.combat_mod_seconds = ty.get_float(t, "Projectile_Combat_Mod_Duration");
            p.combat_mod_value = ty.get_float(t, "Projectile_Combat_Mod_Value");
            p.combat_mod_mask = ty.get_mask(t, "Projectile_Combat_Mod_Category_Mask");
        }
        // --- end OI abilities ---
        BattleTables::ProjectileVisuals v;
        v.detonation = type_or_none(ty.get_string(t, "Projectile_Object_Detonation_Particle"));
        v.armor_reduced = type_or_none(ty.get_string(t, "Projectile_Object_Armor_Reduced_Detonation_Particle"));
        v.absorbed = type_or_none(ty.get_string(t, "Projectile_Absorbed_By_Shields_Particle"));
        v.lifetime = type_or_none(ty.get_string(t, "Projectile_Lifetime_Detonation_Particle"));
        s.full->projectiles.push_back(v);
    }
    const int32_t idx = static_cast<int32_t>(s.d->projectiles.size());
    s.d->projectiles.push_back(std::move(p));
    s.proj_index[t] = idx;
    return idx;
}

int32_t BattleCatalog::hardpoint(std::string_view name, BuildState& s) const {
    const std::string key = upper_str(name);
    const auto it = s.hp_index.find(key);
    if (it != s.hp_index.end()) return it->second;
    const auto rec = hardpoints_.find(key);
    if (rec == hardpoints_.end()) return -1;
    const Tags& tags = rec->second;
    auto tag = [&tags](const char* t) -> std::string_view {
        for (const auto& [n, v] : tags)
            if (n == t) return v;
        return {};
    };
    auto num = [&tag](const char* t, float fallback) {
        const std::string_view v = tag(t);
        return v.empty() ? fallback : static_cast<float>(objects::c_atof(v));
    };
    auto flag = [&tag](const char* t, bool fallback) {
        const std::string_view v = tag(t);
        return v.empty() ? fallback : objects::parse_type_bool(v);
    };
    const objects::EnumConverter& cat = types_->enums().get(objects::EnumId::GameObjectCategory);
    tc::HardpointDesc h;
    h.name = std::string(name);
    h.name_crc = name_crc(h.name);
    h.kind = hardpoint_kind(tag("TYPE"));
    h.health = num("HEALTH", 0.0f);
    h.destroyable = flag("IS_DESTROYABLE", false);
    h.targetable = flag("IS_TARGETABLE", false);
    h.turret = flag("IS_TURRET", false);
    // refixf t07 (TACTICAL_COMBAT §2.9 Q1 step 4): Collision_Mesh (type +0xa0), upper case for the _stricmp lookup.
    h.collision_mesh = upper_str(trim(tag("COLLISION_MESH")));
    // refixf t06 (TACTICAL_COMBAT §2.9 Q2 step 4): unset Fire_Cone_Width / Height are 0 (hardpoint type ctor 0x53e3b0).
    h.fire_cone_width = num("FIRE_CONE_WIDTH", 0.0f);
    h.fire_cone_height = num("FIRE_CONE_HEIGHT", 0.0f);
    h.turret_rotate_extent = num("TURRET_ROTATE_EXTENT_DEGREES", h.turret_rotate_extent);
    const std::string_view proj = tag("FIRE_PROJECTILE_TYPE");
    if (!proj.empty()) {
        const objects::TypeIndex pt = types_->resolve_name(proj);
        if (pt != objects::kNone) h.projectile = projectile(static_cast<uint32_t>(pt), s);
    }
    // REMAINING_GAPS §5.5: the projectile this hard point fires while the owner's Blast is charged (default -1).
    const std::string_view blast = tag("BLAST_ABILITY_FIRE_PROJECTILE_TYPE");
    if (!blast.empty()) {
        const objects::TypeIndex bt = types_->resolve_name(blast);
        if (bt != objects::kNone) h.blast_projectile = projectile(static_cast<uint32_t>(bt), s);
    }
    const std::string_view dt = tag("DAMAGE_TYPE");
    if (!dt.empty()) h.damage_type = damage_.damage_index(dt);
    h.min_recharge = num("FIRE_MIN_RECHARGE_SECONDS", 0.0f);
    h.max_recharge = num("FIRE_MAX_RECHARGE_SECONDS", h.min_recharge);
    h.pulse_count = static_cast<int32_t>(num("FIRE_PULSE_COUNT", 1.0f));
    h.pulse_delay = num("FIRE_PULSE_DELAY_SECONDS", 0.0f);
    h.fire_range = num("FIRE_RANGE_DISTANCE", 0.0f);
    h.fire_min_range = num("FIRE_MIN_RANGE_DISTANCE", 0.0f);
    h.fighter_bay_flyout_distance = num("FIGHTER_BAY_FLYOUT_DISTANCE", 0.0f);  // STATION_HANGARS_SOUND §2.5 (hangar module)
    for (const auto& [n, v] : tags) {
        if (n != "FIRE_INACCURACY_DISTANCE") continue;
        const size_t comma = v.find(',');
        if (comma == std::string::npos) continue;
        const auto m = cat.lookup(trim(std::string_view(v).substr(0, comma)));
        if (!m) continue;
        h.inaccuracy.push_back({static_cast<uint64_t>(*m), static_cast<float>(objects::c_atof(trim(std::string_view(v).substr(comma + 1))))});
    }
    const std::string_view restr = tag("FIRE_CATEGORY_RESTRICTIONS");
    if (!restr.empty()) {
        const auto m = cat.convert(restr);
        if (m) h.fire_category_restrictions = static_cast<uint64_t>(*m);
    }
    // HARDPOINT_DISABLED.md §4: Allows_Special_Weapon_Use (type +0x3d8) - its Set_System_Online writes the allied players'
    // special-weapon flag.
    h.allows_special_weapon_use = flag("ALLOWS_SPECIAL_WEAPON_USE", h.allows_special_weapon_use);
    // ENABLE_SPECIAL_ABILITY hardpoints: Special_Ability_Name (type +0x450) as name_crc (0 = empty).
    if (const std::string_view an = trim(tag("SPECIAL_ABILITY_NAME")); !an.empty())
        h.special_ability_name_crc = name_crc(an);
    if (s.full) {
        // S10-int: S9 hardpoint fields. Fire bones: B present only when named (Percent(50) pick without
        // randomisation, TACTICAL_COMBAT 2.5 step 8); positions come from the animated pose (host hook).
        const std::string_view bone_a = trim(tag("FIRE_BONE_A")), bone_b = trim(tag("FIRE_BONE_B"));
        h.has_bone_b = !bone_b.empty();
        h.randomize_between_fire_bones = flag("RANDOMIZE_BETWEEN_FIRE_BONES", h.randomize_between_fire_bones);
        h.death_explosion_type = type_or_none(tag("DEATH_EXPLOSION_PARTICLES"));
        h.death_breakoff_type = type_or_none(tag("DEATH_BREAKOFF_PROP"));
        h.repair_cost_per_frame = num("REPAIR_COST_PER_FRAME", h.repair_cost_per_frame);      // OI abilities (§1.12)
        h.repair_amount_per_frame = num("REPAIR_AMOUNT_PER_FRAME", h.repair_amount_per_frame);
        s.full->fire_bones.push_back({std::string(bone_a), std::string(bone_b)});
    }
    const int32_t idx = static_cast<int32_t>(s.d->hardpoints.size());
    s.d->hardpoints.push_back(std::move(h));
    s.hp_index[key] = idx;
    return idx;
}

tc::UnitDesc BattleCatalog::unit_desc(uint32_t t, BuildState& s) const {
    const objects::GameObjectTypes& ty = *types_;
    tc::UnitDesc u;
    u.name = std::string(ty.raw().types().name[t]);
    u.object_type = static_cast<int32_t>(t);
    u.tactical_health = ty.columns().tactical_health[t];
    u.shield_points = ty.columns().shield_points[t];
    u.shield_refresh_rate = ty.get_float(t, "Shield_Refresh_Rate");
    u.armor_type = damage_.armor_index(trim(ty.get_string(t, "Armor_Type")));
    u.shield_armor_type = damage_.armor_index(trim(ty.get_string(t, "Shield_Armor_Type")));
    u.radius = tactical::movement::unit_desc_from_type(ty, t).radius;
    u.max_attack_distance = ty.columns().targeting_max_attack_distance[t];
    // --- OI tactical: targeting slots 44 / 45 (TACTICAL_GAPS 3.1) ---
    u.min_attack_distance = ty.get_float(t, "Targeting_Min_Attack_Distance");
    {
        const float sc = ty.get_float(t, "Scale_Factor") > 0.0f ? ty.get_float(t, "Scale_Factor") : 1.0f;
        u.hard_x_extent = ty.get_float(t, "Custom_Hard_XExtent") * sc;
        u.hard_y_extent = ty.get_float(t, "Custom_Hard_YExtent") * sc;
    }
    // --- end OI tactical ---
    u.fire_inaccuracy_fixed_radius = ty.get_float(t, "Targeting_Fire_Inaccuracy_Fixed_Radius");
    const std::string_view set = trim(ty.get_string(t, "Targeting_Priority_Set"));
    for (size_t i = 0; i < priority_sets_.size() && !set.empty(); ++i)
        if (iequal(priority_sets_[i].name, set)) {
            u.priority_set = static_cast<int32_t>(i);
            break;
        }
    u.stickiness_threshold = ty.get_float(t, "Targeting_Stickiness_Time_Threshold");
    // refixf t06 (TACTICAL_COMBAT §2.9): type +0xa8, default TRUE (flag-block initialiser 0x367100): the hull <->
    // hardpoint 0.2 coupling and death with the last destroyable hardpoint.
    u.destroy_when_all_hardpoints_destroyed = ty.get_bool(t, "Should_Be_Destroyed_When_All_Hardpoints_Destroyed");
    u.category_mask = ty.get_mask(t, "CategoryMask");
    u.property_flags = ty.get_mask(t, "Property_Flags");
    u.attack_category_restrictions = ty.get_mask(t, "Attack_Category_Restrictions");
    u.space_layer = static_cast<uint32_t>(ty.get_mask(t, "Space_Layer"));
    bool weapon_hp = false;
    for (uint32_t hid : ty.hardpoints(t)) {
        if (hid >= ty.hardpoint_names().size()) continue;
        int32_t h = hardpoint(ty.hardpoint_names()[hid], s);
        if (h < 0) continue;
        if (s.model_mode >= 0) h = hardpoint_on_model(h, t, s);  // battle host: bones of this unit's model
        u.hardpoints.push_back(static_cast<uint32_t>(h));
        weapon_hp = weapon_hp || tc::is_weapon(s.d->hardpoints[static_cast<size_t>(h)].kind);
    }
    // Non-hardpoint weapon (WeaponBehavior): Projectile_Types, when no weapon hardpoint fires [M].
    const auto projs = ty.get_list(t, "Projectile_Types");
    if (!weapon_hp && !projs.empty()) {
        const objects::TypeIndex pt = ty.resolve_name(trim(projs.front()));
        if (pt != objects::kNone) {
            u.weapon_projectile = projectile(static_cast<uint32_t>(pt), s);
            u.weapon_recharge = ty.get_float(t, "Projectile_Fire_Recharge_Seconds");
            u.weapon_pulse_count = std::max(1, ty.get_int(t, "Projectile_Fire_Pulse_Count"));
            u.weapon_pulse_delay = ty.get_float(t, "Projectile_Fire_Pulse_Delay_Seconds");
        }
        // OI host: the whole Projectile_Types list (list order, unresolved names skipped) for the weapon index of
        // SWAP_WEAPONS / Permanent_Weapon_Swap (type +0xc48; combat UnitDesc::weapon_projectiles).
        if (u.weapon_projectile >= 0 && projs.size() > 1)
            for (const std::string_view n : projs)
                if (const objects::TypeIndex q = ty.resolve_name(trim(n)); q != objects::kNone)
                    u.weapon_projectiles.push_back(projectile(static_cast<uint32_t>(q), s));
    }
    // OI host (TACTICAL_GAPS 3.1 slot 43): obj +0x346 != 0xFF read as the type's LAND_OBSTACLE behaviour [inferred]. S8 t01:
    // the battle host's obstacle-cell query walks the target's passability stamp (map placements' stamps move to their
    // objects); targets without a stamp (objects spawned during the battle) keep the half-extent rule.
    u.land_obstacle = ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::LandObstacle);
    if (s.full) s9_unit_fields(t, u, s);
    return u;
}

void BattleCatalog::s9_unit_fields(uint32_t t, tc::UnitDesc& u, BuildState& s) const {
    // S10-int: S9 unit fields (ABILITIES_HARDPOINTS 1.3, 4.3, 4.7-4.10) + the visual object lists (R4).
    const objects::GameObjectTypes& ty = *types_;
    BattleTables::UnitVisuals vis;
    for (std::string_view n : ty.get_list(t, "Damage_Hit_Particles")) vis.hit.push_back(type_or_none(n));
    for (std::string_view n : ty.get_list(t, "Shield_Hit_Particles")) vis.shield_hit.push_back(type_or_none(n));
    for (std::string_view n : ty.get_list(t, "Death_Explosions")) vis.death_explosions.push_back(type_or_none(n));
    vis.smoke = std::string(trim(ty.get_string(t, "Damaged_Smoke_Asset_Name")));
    u.hit_particle_count = clamp_count(vis.hit.size());
    u.shield_hit_particle_count = clamp_count(vis.shield_hit.size());
    u.damaged_smoke = !vis.smoke.empty();
    u.target_stealth_units = ty.get_bool(t, "Target_Stealth_Units");
    u.combustible = ty.get_bool(t, "Is_Combustible");
    u.burning_damage_per_second = ty.get_float(t, "Burning_Damage_Per_Second");
    u.energy_capacity = ty.get_float(t, "Energy_Capacity");
    u.energy_refresh_rate = ty.get_float(t, "Energy_Refresh_Rate");
    u.requires_base_power = ty.get_bool(t, "Requires_Base_Power");
    u.passive_missile_shield_radius = ty.get_float(t, "Passive_Missile_Shield_Radius");
    u.ranged_target_z_adjust = ty.get_float(t, "Ranged_Target_Z_Adjust");
    u.converted_die_time = ty.get_float(t, "Converted_To_Enemy_Die_Time_Seconds");
    u.has_locomotor = ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Locomotor);
    // Death_Clone: (key name, type) pairs (S9_DEATH_CLONE_SHIELDS.md §1.4): the static tDamageType enum first, else
    // 0x18 + the name's index in GameConstants Damage_Types (e.g. Damage_Disruptor_Hand). S9 t04.
    const auto clones = ty.get_list(t, "Death_Clone");
    const objects::EnumConverter& kinds = ty.enums().get(objects::EnumId::DamageType);
    for (size_t i = 0; i + 1 < clones.size(); i += 2) {
        const std::string_view kn = trim(clones[i]);
        const int32_t ct = type_or_none(clones[i + 1]);
        if (ct < 0) continue;
        int32_t key = -1;
        if (const auto k = kinds.lookup(kn)) {
            key = static_cast<int32_t>(*k);
        } else if (const uint16_t di = damage_.damage_index(kn); di != tc::kNoTypeId) {
            key = tc::kDeathCloneSubKeyBase + static_cast<int32_t>(di);
        }
        if (key < 0) continue;
        u.death_clones.push_back({key, ct});
    }
    u.deploys = ty.get_bool(t, "Deploys");
    for (std::string_view n : ty.get_list(t, "Death_Projectiles")) {
        const int32_t pt = type_or_none(n);
        if (pt >= 0) u.death_projectiles.push_back(projectile(static_cast<uint32_t>(pt), s));
    }
    // --- OI abilities (TACTICAL_GAPS §1.9, §1.10, §7.3) ---
    u.spin_away_on_death = ty.get_bool(t, "Spin_Away_On_Death");
    u.spin_away_chance = ty.get_float(t, "Spin_Away_On_Death_Chance");
    {  // Destruction_Survivors: (type, weight) pairs, stored with cumulative weights (binary search 0x378b70)
        const auto list = ty.get_list(t, "Destruction_Survivors");
        float cum = 0.0f;
        for (size_t i = 0; i + 1 < list.size(); i += 2) {
            const int32_t st = type_or_none(list[i]);
            cum = cum + static_cast<float>(objects::c_atof(trim(list[i + 1])));
            if (st >= 0) u.destruction_survivors.emplace_back(st, cum);
        }
        if (!trim(ty.get_string(t, "Minimum_Destruction_Survivor_Count")).empty())
            u.min_survivors = ty.get_int(t, "Minimum_Destruction_Survivor_Count");
        if (!trim(ty.get_string(t, "Maximum_Destruction_Survivor_Count")).empty())
            u.max_survivors = ty.get_int(t, "Maximum_Destruction_Survivor_Count");
    }
    u.tactical_respawn_secs = ty.get_float(t, "Tactical_Respawn_Time_In_Secs");
    u.stunnable = ty.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("STUNNABLE"));
    u.shield_requires_base_power = ty.get_bool(t, "Shield_Requires_Base_Power");
    // HUD_GAPS §1.4: behaviour DUMMY_STAR_BASE = can repair hard points (the HUD repair cursor; can_repair_hardpoints).
    u.repairs_hardpoints = ty.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("DUMMY_STAR_BASE"));
    // --- end OI abilities ---
    s.full->units.push_back(std::move(vis));
}

bool BattleCatalog::field_node(uint32_t f, data::XmlNode& out) const {
    // The raw store keeps every element in document preorder (ObjectTypeDatabase): the type element is the
    // k-th element child of its file's root (k = position among the types of the same file walk), the field
    // is the (f - field_begin)-th element of the type's subtree in preorder.
    const objects::ObjectTypeDatabase& db = types_->raw();
    const objects::FieldColumns& fc = db.fields();
    const objects::TypeColumns& tc_ = db.types();
    if (f >= fc.size()) return false;
    const objects::TypeIndex tt = fc.type[f];
    const uint32_t file = tc_.file[tt], walk = tc_.walk[tt];
    uint32_t k = 0;
    for (objects::TypeIndex x = tt; x > 0 && tc_.file[x - 1] == file && tc_.walk[x - 1] == walk; --x) ++k;
    if (file >= db.files().size() || !db.files()[file].doc) return false;
    data::XmlNode type_el = db.files()[file].doc->root().first_child();
    for (uint32_t i = 0; i < k && type_el; ++i) type_el = type_el.next_sibling();
    if (!type_el || type_el.line() != tc_.line[tt]) return false;
    // Preorder walk of the subtree (explicit stack, element children only).
    uint32_t want = f - tc_.field_begin[tt];
    std::vector<data::XmlNode> stack;
    for (data::XmlNode c = type_el.first_child(); c; c = c.next_sibling()) stack.push_back(c);
    std::reverse(stack.begin(), stack.end());
    while (!stack.empty()) {
        const data::XmlNode n = stack.back();
        stack.pop_back();
        if (want == 0) {
            if (n.line() != fc.line[f] || !iequal(n.name(), db.tag_name(f))) return false;
            out = n;
            return true;
        }
        --want;
        const size_t at = stack.size();
        for (data::XmlNode c = n.first_child(); c; c = c.next_sibling()) stack.push_back(c);
        std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(at), stack.end());
    }
    return false;
}

ta::AbilityObjectDesc BattleCatalog::ability_desc(uint32_t t, BuildState& s) const {
    const objects::GameObjectTypes& ty = *types_;
    ta::AbilityObjectDesc d;
    d.name = std::string(ty.raw().types().name[t]);
    d.name_crc = name_crc(d.name);
    d.object_type = static_cast<int32_t>(t);
    d.category_mask = ty.get_mask(t, "CategoryMask");
    d.countdown = ty.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("ABILITY_COUNTDOWN"));
    d.team_container = ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Team);
    d.hero = ty.get_bool(t, "Is_Named_Hero") || ty.get_bool(t, "Is_Generic_Hero");
    d.passive_missile_shield_radius = ty.get_float(t, "Passive_Missile_Shield_Radius");
    d.destroy_when_stunned_over_water = ty.get_bool(t, "Destroy_When_Stunned_Over_Water");  // OI t02 [port]
    // 0x3979d0 [port: the mode's FOW reveal range column; land for land battles is not distinguished here]
    if (ty.get_bool(t, "Target_Stealth_Units")) d.stealth_detection_range = ty.columns().space_fow_reveal_range[t];
    ta::AbilityResolvers r;
    const objects::GameObjectTypes* types = types_;
    r.category = [types](std::string_view name) -> uint64_t {
        const auto m = types->enums().get(objects::EnumId::GameObjectCategory).lookup(name);
        return m ? static_cast<uint64_t>(*m) : 0;
    };
    r.object_type = [types](std::string_view name) -> int32_t {
        const objects::TypeIndex x = types->resolve_name(trim(name));
        return x == objects::kNone ? -1 : static_cast<int32_t>(x);
    };
    r.projectile = [this, &s](std::string_view name) -> int32_t {
        const int32_t pt = type_or_none(name);
        return pt < 0 ? -1 : projectile(static_cast<uint32_t>(pt), s);
    };
    r.armor = [this](std::string_view name) -> uint16_t { return damage_.armor_index(trim(name)); };
    // OI abilities: melee Damage_Type and the projectile test for Spawned_Object_Type (instant effects).
    r.damage = [this](std::string_view name) -> uint16_t { return damage_.damage_index(trim(name)); };
    r.is_projectile = [types](std::string_view name) {
        const objects::TypeIndex x = types->resolve_name(trim(name));
        return x != objects::kNone && types->has_behavior(x, objects::BehaviorSet::Union, objects::behavior_type_id("PROJECTILE"));
    };
    std::vector<std::string>* diag = s.full ? &s.full->diagnostics : nullptr;
    // Sub-object lists through the type loader (variants inherit / replace them, OBJECT_TYPES 3.3).
    for (objects::FieldIndex f : ty.get_sub_objects(t, "Unit_Abilities_Data")) {
        data::XmlNode e;
        if (!field_node(f, e) || !iequal(e.name(), "Unit_Ability")) continue;
        ta::UnitAbilityRecord rec;
        if (ta::parse_unit_ability(e, rec, r, diag)) d.unit_abilities.push_back(std::move(rec));
    }
    for (objects::FieldIndex f : ty.get_sub_objects(t, "Abilities")) {
        data::XmlNode e;
        if (!field_node(f, e)) continue;
        d.specials.push_back(static_cast<uint32_t>(s.full->abilities.specials.size()));
        s.full->abilities.specials.push_back(ta::parse_special_ability(e, r, diag));
    }
    return d;
}

void BattleCatalog::build_units(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index,
                                BuildState& s) const {
    std::map<uint32_t, uint32_t> unit_index;
    out_unit_index.clear();
    for (uint32_t t : unit_types) {
        const auto it = unit_index.find(t);
        if (it != unit_index.end()) {
            out_unit_index.push_back(it->second);
            continue;
        }
        const uint32_t idx = static_cast<uint32_t>(s.d->units.size());
        if (t < types_->size()) {
            s.d->units.push_back(unit_desc(t, s));
        } else {
            s.d->units.push_back(tc::UnitDesc{});
            if (s.full) s.full->units.emplace_back();
        }
        if (s.full) {
            ta::AbilityObjectDesc ad = t < types_->size() ? ability_desc(t, s) : ta::AbilityObjectDesc{};
            ta::link_combat_abilities(ad, *s.d, s.d->units[idx]);
            ta::link_infection(ad, s.full->abilities.specials, s.d->units[idx]);  // refix t02
            s.full->abilities.objects.push_back(std::move(ad));
        }
        unit_index[t] = idx;
        out_unit_index.push_back(idx);
    }
}

tc::CombatData BattleCatalog::build(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index) const {
    tc::CombatData d;
    d.damage = damage_;
    d.priority_sets = priority_sets_;
    BuildState s;
    s.d = &d;
    build_units(unit_types, out_unit_index, s);
    return d;
}

BattleTables BattleCatalog::build_battle(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index) const {
    BattleTables b;
    b.combat.damage = damage_;
    b.combat.priority_sets = priority_sets_;
    BuildState s;
    s.d = &b.combat;
    s.full = &b;
    build_units(unit_types, out_unit_index, s);
    return b;
}

// ---- battle host: model data (ALO bind pose through engine/assets) ---------------------------------------------------

struct BattleCatalog::ModelCache {
    std::mutex mutex;
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<ModelInfo>> entries;  // (type, mode code) -> info / null
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<tactical::movement::AvoidModel>> avoid;  // (type, space)
};

// SPACE_AVOIDANCE §3.2 / §5.3 / §5.4 from the ALO: collision bound = union of the collision-enabled meshes' 0x402 boxes
// through their bones' bind-pose transforms (centre 0, half 1 without one); COLLISIONBOX = the mesh of that name
// (case-insensitive), its box the same way; collision meshes = their submesh triangles in model space (static vertices
// through bind_world[attach bone], skinned vertices are model space already), ALO order, `SHIELD` flagged.
const tactical::movement::AvoidModel* BattleCatalog::avoid_model(uint32_t type, bool space) const {
#ifdef ALAMO_SIM_HAVE_ASSETS
    namespace mv = tactical::movement;
    if (!files_ || type >= types_->size()) return nullptr;
    std::lock_guard<std::mutex> lock(model_cache_->mutex);
    const std::pair<uint32_t, uint32_t> key{type, space ? 1u : 0u};
    auto it = model_cache_->avoid.find(key);
    if (it != model_cache_->avoid.end()) return it->second.get();
    std::unique_ptr<mv::AvoidModel> out;
    const std::string_view name = trim(types_->get_string(type, space ? "Space_Model_Name" : "Land_Model_Name"));
    assets::ModelAsset m;
    if (!name.empty() && assets::load_model(*files_, assets::model_path(name), m)) {
        out = std::make_unique<mv::AvoidModel>();
        const auto& bw = m.skeleton.bind_world;
        auto bone = [&bw](uint32_t b) { return b < bw.size() ? bw[b] : assets::Mat3x4::identity(); };
        auto f3 = [](const assets::Float3& v) { return mv::F3{v.x, v.y, v.z}; };
        bool any = false;
        assets::Aabb bound;
        for (const assets::MeshInfo& mi : m.meshes) {
            const assets::Aabb box = assets::transform_aabb(bone(mi.attach_bone), mi.file_bounds);
            if (upper_str(mi.name) == "COLLISIONBOX" && box.valid()) {
                out->has_collision_box = true;
                out->cbox_c = {(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, (box.min.z + box.max.z) * 0.5f};
                out->cbox_h = {(box.max.x - box.min.x) * 0.5f, (box.max.y - box.min.y) * 0.5f, (box.max.z - box.min.z) * 0.5f};
            }
            if (!mi.collision_enabled) continue;
            if (box.valid()) {
                bound = any ? assets::merge(bound, box) : box;
                any = true;
            }
            mv::AvoidModel::Mesh mesh;
            mesh.name = upper_str(mi.name);
            mesh.shield = mesh.name == "SHIELD";
            bool first = true;
            for (uint32_t si = mi.first_submesh; si < mi.first_submesh + mi.submesh_count && si < m.submeshes.size(); ++si) {
                const assets::Submesh& sm = m.submeshes[si];
                const size_t stride = assets::vertex_stride(sm.layout);
                const assets::Mat3x4 xf = sm.skinned() ? assets::Mat3x4::identity() : bone(sm.attach_bone);
                auto vert = [&](uint32_t i) {
                    float p[3] = {0.0f, 0.0f, 0.0f};
                    if (static_cast<size_t>(i) * stride + sizeof(p) <= sm.vertices.size())
                        std::memcpy(p, sm.vertices.data() + static_cast<size_t>(i) * stride, sizeof(p));
                    return xf.transform_point({p[0], p[1], p[2]});
                };
                const size_t isz = assets::index_size(sm.index_type);
                for (uint32_t k = 0; k + 2 < sm.index_count && (static_cast<size_t>(k) + 3) * isz <= sm.indices.size(); k += 3) {
                    for (uint32_t c = 0; c < 3; ++c) {
                        uint32_t ix = 0;
                        if (isz == 2) {
                            uint16_t v16 = 0;
                            std::memcpy(&v16, sm.indices.data() + static_cast<size_t>(k + c) * 2, 2);
                            ix = v16;
                        } else {
                            std::memcpy(&ix, sm.indices.data() + static_cast<size_t>(k + c) * 4, 4);
                        }
                        const assets::Float3 q = vert(ix);
                        mesh.tris.push_back(f3(q));
                        if (first) {
                            mesh.lo = mesh.hi = f3(q);
                            first = false;
                        } else {
                            mesh.lo = {std::min(mesh.lo.x, q.x), std::min(mesh.lo.y, q.y), std::min(mesh.lo.z, q.z)};
                            mesh.hi = {std::max(mesh.hi.x, q.x), std::max(mesh.hi.y, q.y), std::max(mesh.hi.z, q.z)};
                        }
                    }
                }
            }
            if (!mesh.tris.empty()) out->meshes.push_back(std::move(mesh));
        }
        if (any) {
            out->bound_c = {(bound.min.x + bound.max.x) * 0.5f, (bound.min.y + bound.max.y) * 0.5f, (bound.min.z + bound.max.z) * 0.5f};
            out->bound_h = {(bound.max.x - bound.min.x) * 0.5f, (bound.max.y - bound.min.y) * 0.5f, (bound.max.z - bound.min.z) * 0.5f};
        }
    }
    const mv::AvoidModel* r = out.get();
    model_cache_->avoid.emplace(key, std::move(out));
    return r;
#else
    (void)type, (void)space;
    return nullptr;
#endif
}

bool BattleCatalog::space_model_half_extents(uint32_t type, float& half_x, float& half_y) const {
    const tactical::movement::AvoidModel* m = avoid_model(type, true);
    if (!m) return false;
    half_x = m->bound_h.x;
    half_y = m->bound_h.y;
    return true;
}

int32_t BattleCatalog::ModelInfo::find(std::string_view bone) const {
    const std::string key = upper_str(trim(bone));
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i] == key) return static_cast<int32_t>(i);
    return -1;
}

const BattleCatalog::ModelInfo* BattleCatalog::model_info(uint32_t type, bool space, bool fallback) const {
#ifdef ALAMO_SIM_HAVE_ASSETS
    if (!files_ || type >= types_->size()) return nullptr;
    const uint32_t code = (space ? 1u : 0u) | (fallback ? 2u : 0u);
    std::lock_guard<std::mutex> lock(model_cache_->mutex);
    auto it = model_cache_->entries.find({type, code});
    if (it != model_cache_->entries.end()) return it->second.get();
    std::vector<const char*> order = {space ? "Space_Model_Name" : "Land_Model_Name"};
    if (fallback) {
        order.push_back(space ? "Land_Model_Name" : "Space_Model_Name");
        order.push_back("Galactic_Model_Name");
    }
    std::unique_ptr<ModelInfo> info;
    for (const char* tag : order) {
        const std::string_view name = trim(types_->get_string(type, tag));
        if (name.empty()) continue;
        assets::ModelAsset m;
        if (assets::load_model(*files_, assets::model_path(name), m)) {
            info = std::make_unique<ModelInfo>();
            if (m.bounds.min.x <= m.bounds.max.x && m.bounds.min.y <= m.bounds.max.y && m.bounds.min.z <= m.bounds.max.z) {
                info->has_bounds = true;
                info->min[0] = m.bounds.min.x, info->min[1] = m.bounds.min.y, info->min[2] = m.bounds.min.z;
                info->max[0] = m.bounds.max.x, info->max[1] = m.bounds.max.y, info->max[2] = m.bounds.max.z;
            }
            for (size_t b = 0; b < m.skeleton.size(); ++b) {
                info->bones.push_back(upper_str(m.skeleton.names[b]));
                const assets::Mat3x4& w = m.skeleton.bind_world[b];
                info->bone_pos.push_back({w.m[0][3], w.m[1][3], w.m[2][3]});
                info->bone_rot.push_back({w.m[0][0], w.m[0][1], w.m[0][2], w.m[1][0], w.m[1][1], w.m[1][2], w.m[2][0], w.m[2][1], w.m[2][2]});
            }
        }
        break;  // the first named model field decides (a missing file: no model data)
    }
    const ModelInfo* out = info.get();
    model_cache_->entries.emplace(std::make_pair(type, code), std::move(info));
    return out;
#else
    (void)type, (void)space, (void)fallback;
    return nullptr;
#endif
}

bool BattleCatalog::model_half_extents(uint32_t type, float& half_x, float& half_y, float& height_z) const {
    const ModelInfo* m = model_info(type, false, true);
    if (!m || !m->has_bounds) return false;
    half_x = 0.5f * (m->max[0] - m->min[0]);  // [M] half of the box size (the stamp centres it on the object)
    half_y = 0.5f * (m->max[1] - m->min[1]);
    height_z = m->max[2] - m->min[2];
    return true;
}

int32_t BattleCatalog::hardpoint_on_model(int32_t base, uint32_t unit_type, BuildState& s) const {
    if (!s.full || base < 0 || static_cast<size_t>(base) >= s.d->hardpoints.size()) return base;
    const ModelInfo* m = model_info(unit_type, s.model_mode == 1, false);
    if (!m) return base;
    const std::string key = upper_str(s.d->hardpoints[static_cast<size_t>(base)].name);
    const std::string cache_key = key + "@" + std::to_string(unit_type);
    if (const auto it = s.hp_index.find(cache_key); it != s.hp_index.end()) return it->second;
    const auto rec = hardpoints_.find(key);
    if (rec == hardpoints_.end()) return base;
    auto tag = [&rec](const char* t) -> std::string_view {
        for (const auto& [n, v] : rec->second)
            if (n == t) return v;
        return {};
    };
    const int32_t at = m->find(tag("ATTACHMENT_BONE"));
    int32_t ba = m->find(tag("FIRE_BONE_A")), bb = m->find(tag("FIRE_BONE_B"));
    if (at < 0 && ba < 0 && bb < 0) {
        s.hp_index[cache_key] = base;
        return base;
    }
    if (ba < 0) ba = at;  // [port] fire bone not in the unit model: the attachment bone
    if (bb < 0) bb = ba;
    float scale = types_->columns().scale_factor[unit_type];
    if (!(scale > 0.0f)) scale = 1.0f;
    tc::HardpointDesc h = s.d->hardpoints[static_cast<size_t>(base)];
    auto pos = [&](int32_t b, decltype(h.attach_offset) fallback) {
        if (b < 0) return fallback;
        // ALO model space (nose -Y, left +X, up +Z) -> the combat unit-local frame (x forward, y left, z up).
        const auto& p = m->bone_pos[static_cast<size_t>(b)];
        return decltype(h.attach_offset){-p[1] * scale, p[0] * scale, p[2] * scale};
    };
    h.attach_offset = pos(at >= 0 ? at : ba, h.attach_offset);
    h.bone_a = pos(ba, h.attach_offset);
    h.bone_b = pos(bb, h.bone_a);
    // refixf t06 (TACTICAL_COMBAT §2.9 Q2 step 5): the bones' bind-pose axes in the unit-local frame (model column k ->
    // (-y, x, z), normalised; a degenerate axis leaves the frame unset).
    auto frame = [&](int32_t b, decltype(h.attach_offset)& x, decltype(h.attach_offset)& y, decltype(h.attach_offset)& z) {
        if (b < 0 || static_cast<size_t>(b) >= m->bone_rot.size()) return false;
        const auto& r = m->bone_rot[static_cast<size_t>(b)];
        decltype(h.attach_offset)* out[3] = {&x, &y, &z};
        for (int k = 0; k < 3; ++k) {
            const float ax = -r[static_cast<size_t>(3 + k)], ay = r[static_cast<size_t>(k)], az = r[static_cast<size_t>(6 + k)];
            const float l2 = ax * ax + ay * ay + az * az;
            if (!(l2 > 1e-12f)) return false;
            const float inv = 1.0f / dmath::sqrt(l2);
            *out[k] = decltype(h.attach_offset){ax * inv, ay * inv, az * inv};
        }
        return true;
    };
    h.has_fire_frame = frame(ba, h.fire_x, h.fire_y, h.fire_z);
    h.has_attach_frame = frame(at >= 0 ? at : ba, h.attach_x, h.attach_y, h.attach_z);
    const int32_t idx = static_cast<int32_t>(s.d->hardpoints.size());
    s.d->hardpoints.push_back(std::move(h));
    s.full->fire_bones.push_back(s.full->fire_bones[static_cast<size_t>(base)]);
    s.hp_index[cache_key] = idx;
    return idx;
}

BattleTables BattleCatalog::build_battle(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index, bool space) const {
    BattleTables b;
    b.combat.damage = damage_;
    b.combat.priority_sets = priority_sets_;
    BuildState s;
    s.d = &b.combat;
    s.full = &b;
    s.model_mode = space ? 1 : 0;
    build_units(unit_types, out_unit_index, s);
    return b;
}

}  // namespace alamo::sim
