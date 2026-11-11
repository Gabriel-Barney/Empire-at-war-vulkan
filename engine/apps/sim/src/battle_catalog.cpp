#include "alamo/sim/battle_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef ALAMO_SIM_HAVE_ASSETS
#include "alamo/assets/model.hpp"  // battle host: model bounds / bind-pose bones
#endif

#include "alamo/core/dmath.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/data/xml_list.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"
#include "alamo/tactical/abilities/ability_types.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"
#include "alamo/tactical/movement/unit_params.hpp"

namespace alamo::sim {

namespace tc = alamo::tactical::combat;
namespace ta = alamo::tactical::abilities;

namespace {

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
        if (ascii_iequals(type, n)) return k;
    return tc::HardpointKind::DummyArt;
}

// cu5 t02: every <File> entry is opened (an empty one fails to); a missing or unparsable file is noted as not found.
data::ListedXmlOptions battle_list_options() {
    data::ListedXmlOptions o;
    o.keep_empty_entries = true;
    o.visit_missing = true;
    o.visit_unparsable = true;
    return o;
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
        land_health_mult_ = constants->get_float("Object_Max_Health_Multiplier_Land", land_health_mult_);
        land_capture_countdown_ = constants->get_float("Land_Capture_Allowed_Countdown_Seconds", land_capture_countdown_);
        space_capture_countdown_ = constants->get_float("Space_Capture_Allowed_Countdown_Seconds", space_capture_countdown_);
        control_point_victory_seconds_ =
            constants->get_float("Control_Point_Domination_Victory_Time_In_Secs", control_point_victory_seconds_);  // ccpgt t02
        garrisoned_range_mult_ = constants->get_float("Garrisoned_Max_Attack_Distance_Multiplier", garrisoned_range_mult_);
        use_reinforcement_points_ = constants->get_bool("Use_Reinforcement_Points", use_reinforcement_points_);  // a1h t06
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
        if (const auto v = constants->get("Raid_Force_Required_Faction")) k.raid_required_faction = std::string(ascii_trim(*v));
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
        f.setup_countdown_seconds = constants->get_float("SetupPhaseCountdownSeconds", f.setup_countdown_seconds);  // FOWF t05
        // --- end fog ---
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
    const data::ListStatus st = data::for_each_listed_xml(files, "Data\\XML\\HardPointDataFiles.xml", [&](data::ListedXmlFile& file) {
        if (!file.doc || !file.doc->ok()) {
            note(diag, "battle: " + file.entry.path + " not found");
            return;
        }
        for (data::XmlNode hp : file.doc->root().children("HardPoint")) {
            Tags tags;
            const std::string_view variant = hp.child_text("Variant_Of_Existing_Type");
            if (!variant.empty()) {
                const auto it = hardpoints_.find(ascii_upper(variant));
                if (it != hardpoints_.end()) tags = it->second;
            }
            for (data::XmlNode c = hp.first_child(); c; c = c.next_sibling()) {
                const std::string tag = ascii_upper(c.name());
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
            hardpoints_[ascii_upper(hp.attribute("Name"))] = std::move(tags);  // last definition wins
        }
    }, battle_list_options());
    if (st != data::ListStatus::Ok) note(diag, "battle: HardPointDataFiles.xml not found");
}

void BattleCatalog::load_priority_sets(const FileProvider& files, std::vector<std::string>* diag) {
    const objects::GameObjectTypes& types = *types_;
    const tc::TypeResolver resolve = [&types](std::string_view name) -> int32_t {
        const objects::TypeIndex t = types.resolve_name(name);
        return t == objects::kNone ? -1 : static_cast<int32_t>(t);
    };
    const data::ListStatus st = data::for_each_listed_xml(files, "Data\\XML\\TargetingPrioritySetFiles.xml", [&](data::ListedXmlFile& file) {
        if (!file.doc || !file.doc->ok()) {
            note(diag, "battle: " + file.entry.path + " not found");
            return;
        }
        std::vector<tc::PrioritySet> sets = tc::load_priority_sets(file.doc->root(), types.enums(), resolve, nullptr);
        for (tc::PrioritySet& s : sets) priority_sets_.push_back(std::move(s));
    }, battle_list_options());
    if (st != data::ListStatus::Ok) note(diag, "battle: TargetingPrioritySetFiles.xml not found");
}

int32_t BattleCatalog::type_or_none(std::string_view name) const {
    name = ascii_trim(name);
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
    p.damage_type = damage_.damage_index(ascii_trim(ty.get_string(t, "Damage_Type")));
    const int32_t cat = ty.get_int(t, "Projectile_Category");
    p.homing = cat == 1;
    p.max_rate_of_turn = ty.get_float(t, "Max_Rate_Of_Turn");  // S9 t05: missile-shield / homing turn limit
    p.causes_infection = ty.get_bool(t, "Causes_Infection");    // S9 t04: INFECTION behaviour on hit
    p.lobbed = cat == 3;
    // --- S8 t04 (LOB_SOLVER.md §1.1): grenade gravity / lob mod - an XML value (0 included) replaces the constructor
    // defaults 1.05 / 5.02 (ProjectileDesc); Can_Lob_Slower (0.8 speed steps), Sticks_On_Collision ---
    if (!ty.get_string(t, "Projectile_Grenade_Gravity").empty()) p.grenade_gravity = ty.get_float(t, "Projectile_Grenade_Gravity");
    if (!ty.get_string(t, "Projectile_Grenade_Gravity_Lob_Mod").empty())
        p.lob_mod = ty.get_float(t, "Projectile_Grenade_Gravity_Lob_Mod");
    p.can_lob_slower = ty.get_bool(t, "Projectile_Grenade_Can_Lob_Slower");
    p.sticks_on_collision = ty.get_bool(t, "Projectile_Grenade_Sticks_On_Collision");
    p.friendly_damage_amount = ty.get_float(t, "Friendly_Damage_Amount");  // S8 t06 (+0x23ec): own-player sweep, remote bombs
    // --- end S8 t04 ---
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
        // cu8k t01 (CU8F_T01_T02_T07_RE §2.7 item 3): the type ctor 0x367250 defaults CD = 50 and CO = 30; the thrown
        // saber (LIGHT_SABER, no curve fields in vanilla) plans its path with them. [port] applied to category 8 only and
        // when the read value is 0 (an explicit 0 is not told apart): every vanilla ROCKET / MPTL_ROCKET sets both fields.
        if (p.category == tc::ProjectileCategory::LightSaber) {
            if (p.rocket_curve_distance == 0.0f) p.rocket_curve_distance = 50.0f;
            if (p.rocket_curve_offset == 0.0f) p.rocket_curve_offset = 30.0f;
        }
        p.target_point_on_terrain = ty.get_bool(t, "Projectile_Target_Point_On_Terrain");
        p.explode_when_reached_target_radius = ty.get_bool(t, "Explode_When_Reached_Target_Radius");
        p.damage_delay_secs = ty.get_float(t, "Projectile_Damage_Delay_Secs");
        p.ion_stun = ty.get_bool(t, "Projectile_Ion_Stun_On_Detonation");
        p.ion_stun_duration = ty.get_float(t, "Projectile_Ion_Stun_Duration");
        p.ion_stun_speed_reduction = ty.get_float(t, "Projectile_Ion_Stun_Speed_Reduction_Percent");
        p.ion_stun_shot_rate_reduction = ty.get_float(t, "Projectile_Ion_Stun_Shot_Rate_Reduction_Percent");
        p.energy_per_shot = ty.get_float(t, "Projectile_Energy_Per_Shot");
        p.does_energy_damage = ty.get_bool(t, "Projectile_Does_Energy_Damage");
        p.disables_engines_when_drained = ty.get_bool(t, "Projectile_Disables_Engines_When_Power_Drained");  // cu8f t02
        p.disable_engines_duration = ty.get_float(t, "Projectile_Disable_Engines_Duration");
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
            const std::string_view m = ascii_trim(ty.get_string(t, "Projectile_Combat_Mod"));
            for (size_t k = 0; k < std::size(kMods) && !m.empty(); ++k)
                if (ascii_iequals(m, kMods[k])) p.combat_mod = static_cast<uint8_t>(k + 1);
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
    const std::string key = ascii_upper(name);
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
    h.collision_mesh = ascii_upper(ascii_trim(tag("COLLISION_MESH")));
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
        const auto m = cat.lookup(ascii_trim(std::string_view(v).substr(0, comma)));
        if (!m) continue;
        h.inaccuracy.push_back({static_cast<uint64_t>(*m), static_cast<float>(objects::c_atof(ascii_trim(std::string_view(v).substr(comma + 1))))});
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
    if (const std::string_view an = ascii_trim(tag("SPECIAL_ABILITY_NAME")); !an.empty())
        h.special_ability_name_crc = name_crc(an);
    // S8 t08 (TACTICAL_COMBAT §3.1.1, 0x384850 steps 4-6): the deploy / attack-mode bytes (+0x3d1..+0x3d4, default true)
    // and the per-mode bytes Fire_When_In_<Mode>_Mode (+0x384 + m; listed when No).
    h.fire_when_deployed = flag("FIRE_WHEN_DEPLOYED", true);
    h.fire_when_undeployed = flag("FIRE_WHEN_UNDEPLOYED", true);
    h.fire_when_rocket_attack = flag("FIRE_WHEN_IN_ROCKET_ATTACK_MODE", true);
    h.fire_when_normal_attack = flag("FIRE_WHEN_IN_NORMAL_ATTACK_MODE", true);
    for (const auto& [n, v] : tags) {
        constexpr std::string_view kPre = "FIRE_WHEN_IN_", kSuf = "_MODE";
        const std::string_view nv = n;
        if (nv.size() <= kPre.size() + kSuf.size() || nv.substr(0, kPre.size()) != kPre || nv.substr(nv.size() - kSuf.size()) != kSuf)
            continue;
        const std::string_view mode = nv.substr(kPre.size(), nv.size() - kPre.size() - kSuf.size());
        if (mode == "ROCKET_ATTACK" || mode == "NORMAL_ATTACK") continue;  // the dedicated bytes above
        const uint8_t m = ta::ability_type_from_name(mode);
        if (m >= 1 && m <= 0x4c && !objects::parse_type_bool(v) &&
            std::find(h.no_fire_modes.begin(), h.no_fire_modes.end(), m) == h.no_fire_modes.end())
            h.no_fire_modes.push_back(m);
    }
    if (s.full) {
        // S10-int: S9 hardpoint fields. Fire bones: B present only when named (Percent(50) pick without
        // randomisation, TACTICAL_COMBAT 2.5 step 8); positions come from the animated pose (host hook).
        const std::string_view bone_a = ascii_trim(tag("FIRE_BONE_A")), bone_b = ascii_trim(tag("FIRE_BONE_B"));
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
    u.armor_type = damage_.armor_index(ascii_trim(ty.get_string(t, "Armor_Type")));
    u.shield_armor_type = damage_.armor_index(ascii_trim(ty.get_string(t, "Shield_Armor_Type")));
    u.radius = tactical::movement::unit_desc_from_type(ty, t).radius;
    u.max_attack_distance = ty.columns().targeting_max_attack_distance[t];
    // --- OI tactical: targeting slots 44 / 45 (TACTICAL_GAPS 3.1) ---
    u.min_attack_distance = ty.get_float(t, "Targeting_Min_Attack_Distance");
    {
        const float sc = ty.get_float(t, "Scale_Factor") > 0.0f ? ty.get_float(t, "Scale_Factor") : 1.0f;
        u.hard_x_extent = ty.get_float(t, "Custom_Hard_XExtent") * sc;
        u.hard_y_extent = ty.get_float(t, "Custom_Hard_YExtent") * sc;
        // s78n t11 (TACTICAL_COMBAT §5.4.6 item 3): the deployed range extents (MPTL / MAL_Rocket_Vehicle set only Y 24).
        u.hard_x_extent_deployed = ty.get_float(t, "Custom_Hard_XExtent_Deployed") * sc;
        u.hard_y_extent_deployed = ty.get_float(t, "Custom_Hard_YExtent_Deployed") * sc;
        // s78n t08f (TACTICAL_COMBAT §5.4.5, 0x371e70 / 0x372040; replaces A1j t04's |model bbox max.x / max.y|): the model
        // fallback of the slot 43 / 46 extents is the half size h of the collision bound of the mode's model (ModelClass vf
        // +0x58 = avoid_model's bound_h: collision-enabled meshes' 0x402 boxes through the bind pose, (1, 1, 1) without one;
        // the centre offset is ignored). Models face -Y, so the X extent (along the facing) is |h.y| and the Y extent
        // (across) |h.x|, x Scale_Factor. No model for the mode -> 0. [port] a named model whose file does not load (no art
        // data) leaves -1: the radius stands in (CombatSystem::footprint_extents).
        if (s.model_mode >= 0) {
            const bool space = s.model_mode == 1;
            if (ascii_trim(ty.get_string(t, space ? "Space_Model_Name" : "Land_Model_Name")).empty()) {
                u.model_x_extent = u.model_y_extent = 0.0f;
            } else if (const tactical::movement::AvoidModel* am = avoid_model(t, space)) {
                u.model_x_extent = std::fabs(am->bound_h.y) * sc;
                u.model_y_extent = std::fabs(am->bound_h.x) * sc;
            }
        }
    }
    // --- end OI tactical ---
    u.fire_inaccuracy_fixed_radius = ty.get_float(t, "Targeting_Fire_Inaccuracy_Fixed_Radius");
    {  // A1j t03 (TACTICAL_COMBAT §3.1.2): Targeting_Fire_Inaccuracy `<category>, <distance>` pairs (code 0x3d, XML order)
        const objects::EnumConverter& cat = ty.enums().get(objects::EnumId::GameObjectCategory);
        const std::span<const std::string_view> fi = ty.get_list(t, "Targeting_Fire_Inaccuracy");
        for (size_t i = 0; i + 1 < fi.size(); i += 2)
            if (const auto m = cat.lookup(ascii_trim(fi[i])))
                u.fire_inaccuracy.push_back({static_cast<uint64_t>(*m), static_cast<float>(objects::c_atof(ascii_trim(fi[i + 1])))});
    }
    const std::string_view set = ascii_trim(ty.get_string(t, "Targeting_Priority_Set"));
    for (size_t i = 0; i < priority_sets_.size() && !set.empty(); ++i)
        if (ascii_iequals(priority_sets_[i].name, set)) {
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
    for (uint32_t hid : ty.hardpoints(t)) {
        if (hid >= ty.hardpoint_names().size()) continue;
        int32_t h = hardpoint(ty.hardpoint_names()[hid], s);
        if (h < 0) continue;
        if (s.model_mode >= 0) h = hardpoint_on_model(h, t, s);  // battle host: bones of this unit's model
        u.hardpoints.push_back(static_cast<uint32_t>(h));
    }
    // Non-hardpoint weapon (WeaponBehavior). S8 t08 (TACTICAL_COMBAT §3.1.1): it exists when the object has the WEAPON
    // behaviour (0x0A: Behavior plus the battle mode's LandBehavior / SpaceBehavior list; every list without a mode) and a
    // non-empty Projectile_Types, independent of weapon hardpoints (the two sources fire side by side, gated by the
    // Fire_Weapon_When_* / Fire_When_* flags). Projectile_Types without WEAPON is never fired (abilities / Lua read it).
    // a1h t01: M1_Repulsor_Tank (WEAPON + HP_2M_Tank_00, which excludes Air / Vehicle / Structure) fires its
    // Proj_M1_Laser_Cannon at vehicles through this weapon.
    const auto projs = ty.get_list(t, "Projectile_Types");
    const bool weapon_behavior =
        s.model_mode < 0 ? ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Weapon)
                         : ty.has_behavior(t, objects::BehaviorSet::Generic, objects::beh::Weapon) ||
                               ty.has_behavior(t, s.model_mode == 1 ? objects::BehaviorSet::Space : objects::BehaviorSet::Land,
                                               objects::beh::Weapon);
    if (!projs.empty()) {  // the list's first entry for the host's special weapons (UnitDesc::listed_projectile)
        const objects::TypeIndex pt = ty.resolve_name(ascii_trim(projs.front()));
        if (pt != objects::kNone) u.listed_projectile = projectile(static_cast<uint32_t>(pt), s);
    }
    if (weapon_behavior && !projs.empty()) {
        const objects::TypeIndex pt = ty.resolve_name(ascii_trim(projs.front()));
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
                if (const objects::TypeIndex q = ty.resolve_name(ascii_trim(n)); q != objects::kNone)
                    u.weapon_projectiles.push_back(projectile(static_cast<uint32_t>(q), s));
        // S8 t08: the own-weapon gate flags (type +0xae..+0xb1, default true).
        auto yes = [&](const char* tag) { return ty.get_string(t, tag).empty() || ty.get_bool(t, tag); };
        u.fire_weapon_when_deployed = yes("Fire_Weapon_When_Deployed");
        u.fire_weapon_when_undeployed = yes("Fire_Weapon_When_Undeployed");
        u.fire_weapon_when_rocket_attack = yes("Fire_Weapon_When_In_Rocket_Attack_Mode");
        u.fire_weapon_when_normal_attack = yes("Fire_Weapon_When_In_Normal_Attack_Mode");
        // S8 t06 B4 (LOB_SOLVER §5.1): the fire bones of the unit's model (battle host with model data only).
        if (u.weapon_projectile >= 0 && s.model_mode >= 0) u.muzzle_model = muzzle_bones(t, s.model_mode == 1, u.muzzle_bones);
    }
    // OI host (TACTICAL_GAPS 3.1 slot 43): obj +0x346 != 0xFF read as the type's LAND_OBSTACLE behaviour [inferred]. S8 t01:
    // the battle host's obstacle-cell query walks the target's passability stamp (map placements' stamps move to their
    // objects); targets without a stamp (objects spawned during the battle) keep the half-extent rule.
    u.land_obstacle = ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::LandObstacle);
    // S8 t10 / t11 (TACTICAL_COMBAT §3.1.1 "Gate details" steps 6-7, §3.4): Can_Fighter_Fire_When_Idle (type +0x8a, default
    // false), LOBBING_SUPERWEAPON (own weapon only by a fire-at-position order), TRANSPORT_LANDING (no own-player sweep hit).
    u.can_fighter_fire_when_idle = ty.get_bool(t, "Can_Fighter_Fire_When_Idle");
    static const objects::BehaviorTypeId kLobbing = objects::behavior_type_id("LOBBING_SUPERWEAPON");
    u.lobbing_superweapon = kLobbing != objects::kNoBehavior && ty.has_behavior(t, objects::BehaviorSet::Union, kLobbing);
    // S8 t26 / t27 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order", "Bone list"): TURRET (the lobbing charge-up aims it),
    // Lobbing_Superweapon_Chargeup_Frames (vanilla 90 [M: the default without the tag]) and the Target_Bones found on the
    // type's model for the battle mode (battle host with model data only).
    static const objects::BehaviorTypeId kTurret = objects::behavior_type_id("TURRET");
    u.turret = kTurret != objects::kNoBehavior && ty.has_behavior(t, objects::BehaviorSet::Union, kTurret);
    if (u.lobbing_superweapon && !ty.get_string(t, "Lobbing_Superweapon_Chargeup_Frames").empty())
        u.lobbing_chargeup_frames = static_cast<uint32_t>(std::max(0, ty.get_int(t, "Lobbing_Superweapon_Chargeup_Frames")));
    // s78f t04 (LOB_SOLVER §3.6 "Charge-up aim"): the lobbing TURRET's slew speed, undeployed extents, XY-only flag and
    // rest angle (absent extents keep the UnitDesc defaults [M: the type ctor's]).
    if (u.lobbing_superweapon && u.turret) {
        const auto tagged = [&](const char* tag) { return !ty.get_string(t, tag).empty(); };
        u.turret_rotate_speed = ty.get_float(t, "Turret_Rotate_Speed");
        if (tagged("Turret_Rotate_Extent_Degrees")) u.turret_extent = ty.get_float(t, "Turret_Rotate_Extent_Degrees");
        if (tagged("Turret_Elevate_Extent_Degrees")) u.turret_elevate_extent = ty.get_float(t, "Turret_Elevate_Extent_Degrees");
        u.turret_xy_only = ty.get_bool(t, "Turret_XY_Only");
        const std::array<float, 4> r = ty.get_vec(t, "Turret_Rest_Angle");
        u.turret_rest_pitch = r[1];
        u.turret_rest_yaw = r[2];
    }
    if (s.model_mode >= 0) (void)target_bones(t, s.model_mode == 1, u.target_bones);
    u.transport_landing = ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::TransportLanding);
    // S8 t10 (LOB_SOLVER §5.1): Num_Garrison_Guns and the gun-bone table (battle host with model data only); the model ALT
    // comes from Land_Damage_Thresholds / _Alternates (TACTICAL_GAPS §6.3).
    u.num_garrison_guns = ty.get_int(t, "Num_Garrison_Guns");
    if (u.num_garrison_guns != 0 && s.model_mode >= 0) (void)garrison_bones(t, s.model_mode == 1, u.num_garrison_guns, u.garrison_bones);
    {
        const auto th = ty.get_list(t, "Land_Damage_Thresholds");
        const auto al = ty.get_list(t, "Land_Damage_Alternates");
        for (size_t k = 0; k < th.size() && k < al.size(); ++k)
            u.damage_alternates.emplace_back(static_cast<float>(objects::c_atof(ascii_trim(th[k]))),
                                             static_cast<int32_t>(objects::c_atof(ascii_trim(al[k]))));
    }
    // q6 t19 - the type half of 0x5680b0 (A1_READINGS §33.2.1 tests 1 and 6) for the path-B goal promotion: both
    // Collidable_By_Projectile_Living and Is_Valid_Target [port: or a company member type - the combat goal is the member
    // unit where the original's is its TEAM container, which passes on its own]; a SPECIAL_WEAPON type only when it is
    // also a DUMMY_STAR_BASE. The object half (garrisoned, dead, hero clash) is tested by the combat system.
    {
        static const objects::BehaviorTypeId kSpecialWeapon = objects::behavior_type_id("SPECIAL_WEAPON");
        static const objects::BehaviorTypeId kDummyStarBase = objects::behavior_type_id("DUMMY_STAR_BASE");
        const auto beh = [&](objects::BehaviorTypeId b) {
            return b != objects::kNoBehavior && ty.has_behavior(t, objects::BehaviorSet::Union, b);
        };
        u.target_test_type = ((ty.get_bool(t, "Collidable_By_Projectile_Living") && ty.get_bool(t, "Is_Valid_Target")) ||
                              ty.company_member(t)) &&
                             (!beh(kSpecialWeapon) || beh(kDummyStarBase));
    }
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
    vis.smoke = std::string(ascii_trim(ty.get_string(t, "Damaged_Smoke_Asset_Name")));
    u.hit_particle_count = clamp_count(vis.hit.size());
    u.shield_hit_particle_count = clamp_count(vis.shield_hit.size());
    u.damaged_smoke = !vis.smoke.empty();
    u.target_stealth_units = ty.get_bool(t, "Target_Stealth_Units");
    // s78g t06 (TACTICAL_COMBAT §3.6 step 2): the Take_Damage team rules (vanilla: Droid_R2D2 / Droid_C3P0 share, Luke_XWing
    // / TIE_Prototype redirect). Never read before, so the rules had not run in any battle.
    u.redirect_damage_to_teammates = ty.get_bool(t, "Redirect_Damage_To_Teammates");
    u.share_damage_with_teammates = ty.get_bool(t, "Share_Damage_With_Teammates");
    u.combustible = ty.get_bool(t, "Is_Combustible");
    u.burning_damage_per_second = ty.get_float(t, "Burning_Damage_Per_Second");
    u.energy_capacity = ty.get_float(t, "Energy_Capacity");
    u.energy_refresh_rate = ty.get_float(t, "Energy_Refresh_Rate");
    u.requires_base_power = ty.get_bool(t, "Requires_Base_Power");
    u.passive_missile_shield_radius = ty.get_float(t, "Passive_Missile_Shield_Radius");
    // S8 t04 (LOB_SOLVER §1.1): type +0x4dc, constructor default 6.0 when the tag is absent.
    u.ranged_target_z_adjust =
        ty.get_string(t, "Ranged_Target_Z_Adjust").empty() ? 6.0f : ty.get_float(t, "Ranged_Target_Z_Adjust");
    u.converted_die_time = ty.get_float(t, "Converted_To_Enemy_Die_Time_Seconds");
    // s78n t05 (TACTICAL_COMBAT §3.5, 0x395c10 steps 3-5): the convert victim test's type half.
    u.proximity_mine = ty.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("PROXIMITY_MINE"));
    u.friendly_damage_amount = ty.get_float(t, "Friendly_Damage_Amount");
    u.named_hero = ty.get_bool(t, "Is_Named_Hero");
    u.has_locomotor = ty.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Locomotor);
    // cu8g t05 (obj +0x2a0, the infection spread candidate test): a model is named for the battle's mode.
    if (s.model_mode >= 0)
        u.has_model = !ascii_trim(ty.get_string(t, s.model_mode == 1 ? "Space_Model_Name" : "Land_Model_Name")).empty();
    // Death_Clone: (key name, type) pairs (S9_DEATH_CLONE_SHIELDS.md §1.4): the static tDamageType enum first, else
    // 0x18 + the name's index in GameConstants Damage_Types (e.g. Damage_Disruptor_Hand). S9 t04.
    const auto clones = ty.get_list(t, "Death_Clone");
    const objects::EnumConverter& kinds = ty.enums().get(objects::EnumId::DamageType);
    for (size_t i = 0; i + 1 < clones.size(); i += 2) {
        const std::string_view kn = ascii_trim(clones[i]);
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
    // s78f t03 (TACTICAL_COMBAT §3.1.1 "Targeting states, slots 47 / 48"): the facing test's turn rate and the land
    // service's deploy / move flags.
    u.turn_rate = ty.get_float(t, "Max_Rate_Of_Turn");
    u.auto_deploys = ty.get_bool(t, "Auto_Deploys");
    u.must_face_target_when_deployed = ty.get_bool(t, "Must_Face_Target_To_Attack_When_Deployed");
    u.stationary_when_attacking = ty.get_bool(t, "Is_Stationary_When_Attacking");
    u.max_dist_walk_while_deployed = ty.get_float(t, "Max_Dist_Walk_While_Deployed");
    // s78f t08: the target-type flags of the hold-fire rule and the TURRET cannot-aim branch.
    u.hold_fire_for_special_ability = ty.get_bool(t, "Should_Attacker_Hold_Fire_For_Special_Ability");
    u.limited_turrets_turn = ty.get_bool(t, "Should_Cause_Limited_Turrets_To_Turn");
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
            cum = cum + static_cast<float>(objects::c_atof(ascii_trim(list[i + 1])));
            if (st >= 0) u.destruction_survivors.emplace_back(st, cum);
        }
        if (!ascii_trim(ty.get_string(t, "Minimum_Destruction_Survivor_Count")).empty())
            u.min_survivors = ty.get_int(t, "Minimum_Destruction_Survivor_Count");
        if (!ascii_trim(ty.get_string(t, "Maximum_Destruction_Survivor_Count")).empty())
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
            if (n.line() != fc.line[f] || !ascii_iequals(n.name(), db.tag_name(f))) return false;
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
    // s78l t05 (TACTICAL_COMBAT §5.4.1): Tactical_Bribe acceptance / cost. [port] every desc (a TEAM container too) reads
    // its own type: the FoC containers carry their own Tactical_Bribe_Cost (Stormtrooper_Team 25, its member 10), and
    // the container type does not name a member type here.
    d.named_hero = ty.get_bool(t, "Is_Named_Hero");
    d.tactical_bribe_cost = ty.get_int(t, "Tactical_Bribe_Cost");  // 0 when the tag is missing
    const auto float_or = [&ty, t](std::string_view tag, float def) { return ty.get_string(t, tag).empty() ? def : ty.get_float(t, tag); };
    d.bribe_modifier_corrupted = float_or("Bribe_Modifier_On_Corrupted_Planets", 1.0f);
    d.bribe_modifier_uncorrupted = float_or("Bribe_Modifier_On_Uncorrupted_Planets", 1.0f);
    d.friendly_damage_amount = ty.get_float(t, "Friendly_Damage_Amount");
    d.proximity_mine = ty.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("PROXIMITY_MINE"));
    // cu8f t06 (CU8F_T04_T06_RE §3.4): the Face_Target Super_Laser turns through TACTICAL_SUPER_WEAPON
    d.tactical_super_weapon = ty.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("TACTICAL_SUPER_WEAPON"));
    // s78m t05 (TACTICAL_COMBAT §5.4.3 item 4): the Combat_Bonus accept refuses the source type's unique units
    d.unique_ground_type = type_or_none(ty.get_string(t, "Unique_Ground_Unit"));
    d.unique_space_type = type_or_none(ty.get_string(t, "Unique_Space_Unit"));
    // s78n t04 (TACTICAL_COMBAT §5.4.4 item 3b): a Combat_Bonus source buffs itself only with this type flag
    d.apply_self_on_creation = ty.get_bool(t, "Apply_Special_Ability_To_Self_On_Creation");
    // 0x3979d0 [port: the mode's FOW reveal range column; land for land battles is not distinguished here]
    if (ty.get_bool(t, "Target_Stealth_Units")) d.stealth_detection_range = ty.columns().space_fow_reveal_range[t];
    ta::AbilityResolvers r;
    const objects::GameObjectTypes* types = types_;
    r.category = [types](std::string_view name) -> uint64_t {
        const auto m = types->enums().get(objects::EnumId::GameObjectCategory).lookup(name);
        return m ? static_cast<uint64_t>(*m) : 0;
    };
    r.object_type = [types](std::string_view name) -> int32_t {
        const objects::TypeIndex x = types->resolve_name(ascii_trim(name));
        return x == objects::kNone ? -1 : static_cast<int32_t>(x);
    };
    r.projectile = [this, &s](std::string_view name) -> int32_t {
        const int32_t pt = type_or_none(name);
        return pt < 0 ? -1 : projectile(static_cast<uint32_t>(pt), s);
    };
    r.type_max_speed = [types](std::string_view name) -> float {  // cu8i t01: Saber_Throw's Saber_Name speed
        const objects::TypeIndex x = types->resolve_name(ascii_trim(name));
        return x == objects::kNone ? -1.0f : types->columns().max_speed[x];
    };
    r.type_max_flight_distance = [types](std::string_view name) -> float {  // cu8j t03: Saber_Name's max travel base
        const objects::TypeIndex x = types->resolve_name(ascii_trim(name));
        return x == objects::kNone ? -1.0f : types->get_float(x, "Projectile_Max_Flight_Distance");
    };
    r.armor = [this](std::string_view name) -> uint16_t { return damage_.armor_index(ascii_trim(name)); };
    // OI abilities: melee Damage_Type and the projectile test for Spawned_Object_Type (instant effects).
    r.damage = [this](std::string_view name) -> uint16_t { return damage_.damage_index(ascii_trim(name)); };
    r.is_projectile = [types](std::string_view name) {
        const objects::TypeIndex x = types->resolve_name(ascii_trim(name));
        return x != objects::kNone && types->has_behavior(x, objects::BehaviorSet::Union, objects::behavior_type_id("PROJECTILE"));
    };
    std::vector<std::string>* diag = s.full ? &s.full->diagnostics : nullptr;
    // Sub-object lists through the type loader (variants inherit / replace them, OBJECT_TYPES 3.3).
    for (objects::FieldIndex f : ty.get_sub_objects(t, "Unit_Abilities_Data")) {
        data::XmlNode e;
        if (!field_node(f, e) || !ascii_iequals(e.name(), "Unit_Ability")) continue;
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

void BattleCatalog::build_units(const std::vector<uint32_t>& unit_types, size_t first, std::vector<uint32_t>& out_unit_index,
                                BuildState& s) const {
    std::map<uint32_t, uint32_t>& unit_index = s.unit_index;
    for (size_t i = first; i < unit_types.size(); ++i) {
        const uint32_t t = unit_types[i];
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
    out_unit_index.clear();
    build_units(unit_types, 0, out_unit_index, s);
    return d;
}

BattleTables BattleCatalog::build_battle(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index) const {
    BattleTables b;
    b.combat.damage = damage_;
    b.combat.priority_sets = priority_sets_;
    BuildState s;
    s.d = &b.combat;
    s.full = &b;
    out_unit_index.clear();
    build_units(unit_types, 0, out_unit_index, s);
    return b;
}

// ---- battle host: model data (ALO bind pose through engine/assets) ---------------------------------------------------

struct BattleCatalog::ModelCache {
    std::mutex mutex;
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<ModelInfo>> entries;  // (type, mode code) -> info / null
    std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<tactical::movement::AvoidModel>> avoid;  // (type, space)
};

#ifdef ALAMO_SIM_HAVE_ASSETS
namespace {

// SPACE_AVOIDANCE §3.2 / §5.3 / §5.4 from the ALO: collision bound = union of the collision-enabled meshes' 0x402 boxes
// through their bones' bind-pose transforms (centre 0, half 1 without one); COLLISIONBOX = the mesh of that name
// (case-insensitive), its box the same way; collision meshes = their submesh triangles in model space (static vertices
// through bind_world[attach bone], skinned vertices are model space already), ALO order, `SHIELD` flagged.
std::unique_ptr<tactical::movement::AvoidModel> avoid_from_asset(const assets::ModelAsset& m) {
    namespace mv = tactical::movement;
    auto out = std::make_unique<mv::AvoidModel>();
    const auto& bw = m.skeleton.bind_world;
    auto bone = [&bw](uint32_t b) { return b < bw.size() ? bw[b] : assets::Mat3x4::identity(); };
    auto f3 = [](const assets::Float3& v) { return mv::F3{v.x, v.y, v.z}; };
    bool any = false;
    assets::Aabb bound;
    for (const assets::MeshInfo& mi : m.meshes) {
        const assets::Aabb box = assets::transform_aabb(bone(mi.attach_bone), mi.file_bounds);
        if (ascii_upper(mi.name) == "COLLISIONBOX" && box.valid()) {
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
        mesh.name = ascii_upper(mi.name);
        mesh.shield = mesh.name == "SHIELD";
        mesh.bone_visible = mi.attach_bone >= m.skeleton.visible.size() || m.skeleton.visible[mi.attach_bone] != 0;  // S8 t13 ray set
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
    return out;
}

// model_info's entry: bind-pose bounds, bones, _ALT index.
std::unique_ptr<BattleCatalog::ModelInfo> info_from_asset(const assets::ModelAsset& m) {
    auto info = std::make_unique<BattleCatalog::ModelInfo>();
    if (m.bounds.min.x <= m.bounds.max.x && m.bounds.min.y <= m.bounds.max.y && m.bounds.min.z <= m.bounds.max.z) {
        info->has_bounds = true;
        info->min[0] = m.bounds.min.x, info->min[1] = m.bounds.min.y, info->min[2] = m.bounds.min.z;
        info->max[0] = m.bounds.max.x, info->max[1] = m.bounds.max.y, info->max[2] = m.bounds.max.z;
    }
    for (size_t b = 0; b < m.skeleton.size(); ++b) {
        info->bones.push_back(ascii_upper(m.skeleton.names[b]));
        const assets::Mat3x4& w = m.skeleton.bind_world[b];
        info->bone_pos.push_back({w.m[0][3], w.m[1][3], w.m[2][3]});
        info->bone_rot.push_back({w.m[0][0], w.m[0][1], w.m[0][2], w.m[1][0], w.m[1][1], w.m[1][2], w.m[2][0], w.m[2][1], w.m[2][2]});
    }
    // S8 t10 (0x12dca0 / 0x13f8c0): the highest digit after "_ALT" in the mesh names.
    for (const assets::MeshInfo& mi : m.meshes) {
        const std::string n = ascii_upper(mi.name);
        for (size_t i = 0; i + 5 <= n.size(); ++i)
            if (n.compare(i, 4, "_ALT") == 0 && n[i + 4] >= '0' && n[i + 4] <= '9') {
                info->max_alt = std::max<int32_t>(info->max_alt, n[i + 4] - '0');
                break;
            }
    }
    return info;
}

}  // namespace

// OPT3 t09: the model file an entry reads ("" none: the entry is null). avoid_model reads the mode's field; model_info
// the first named field of its order (a missing file: no model data).
std::string BattleCatalog::avoid_path(uint32_t type, bool space) const {
    const std::string_view name = ascii_trim(types_->get_string(type, space ? "Space_Model_Name" : "Land_Model_Name"));
    return name.empty() ? std::string() : assets::model_path(name);
}

std::string BattleCatalog::info_path(uint32_t type, bool space, bool fallback) const {
    std::vector<const char*> order = {space ? "Space_Model_Name" : "Land_Model_Name"};
    if (fallback) {
        order.push_back(space ? "Land_Model_Name" : "Space_Model_Name");
        order.push_back("Galactic_Model_Name");
    }
    for (const char* tag : order) {
        const std::string_view name = ascii_trim(types_->get_string(type, tag));
        if (!name.empty()) return assets::model_path(name);
    }
    return {};
}
#endif

const tactical::movement::AvoidModel* BattleCatalog::avoid_model(uint32_t type, bool space) const {
#ifdef ALAMO_SIM_HAVE_ASSETS
    if (!files_ || type >= types_->size()) return nullptr;
    const std::pair<uint32_t, uint32_t> key{type, space ? 1u : 0u};
    {
        std::lock_guard<std::mutex> lock(model_cache_->mutex);
        auto it = model_cache_->avoid.find(key);
        if (it != model_cache_->avoid.end()) return it->second.get();
    }
    // Built outside the lock (OPT3 t05: preload loads files in parallel); two threads building the same key make the
    // same entry and the first one stored is kept.
    std::unique_ptr<tactical::movement::AvoidModel> out;
    const std::string path = avoid_path(type, space);
    assets::ModelAsset m;
    if (!path.empty() && assets::load_model(*files_, path, m)) out = avoid_from_asset(m);
    std::lock_guard<std::mutex> lock(model_cache_->mutex);
    return model_cache_->avoid.emplace(key, std::move(out)).first->second.get();
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
    const std::string key = ascii_upper(ascii_trim(bone));
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i] == key) return static_cast<int32_t>(i);
    return -1;
}

const BattleCatalog::ModelInfo* BattleCatalog::model_info(uint32_t type, bool space, bool fallback) const {
#ifdef ALAMO_SIM_HAVE_ASSETS
    if (!files_ || type >= types_->size()) return nullptr;
    const uint32_t code = (space ? 1u : 0u) | (fallback ? 2u : 0u);
    {
        std::lock_guard<std::mutex> lock(model_cache_->mutex);
        auto it = model_cache_->entries.find({type, code});
        if (it != model_cache_->entries.end()) return it->second.get();
    }  // built outside the lock, as avoid_model
    std::unique_ptr<ModelInfo> info;
    const std::string path = info_path(type, space, fallback);
    assets::ModelAsset m;
    if (!path.empty() && assets::load_model(*files_, path, m)) info = info_from_asset(m);
    std::lock_guard<std::mutex> lock(model_cache_->mutex);
    return model_cache_->entries.emplace(std::make_pair(type, code), std::move(info)).first->second.get();
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

void BattleCatalog::preload_models(std::span<const uint32_t> types, bool space, const ParallelFor& pf) const {
    preload(types, space, kPreloadAvoid | (space ? 0u : kPreloadExtents), pf);
}

void BattleCatalog::preload(std::span<const uint32_t> types, bool space, uint32_t what, const ParallelFor& pf) const {
#ifdef ALAMO_SIM_HAVE_ASSETS
    if (!files_) return;
    // The entries of `types` not cached yet, grouped by the file they read (path order): one job per file parses it once
    // and builds each of its entries (OPT3 t09; t05 parsed a file once per entry).
    struct Want {
        uint32_t type;
        uint32_t kind;  // kPreloadAvoid / kPreloadBones / kPreloadExtents
    };
    const uint32_t sc = space ? 1u : 0u;
    std::map<std::string, std::vector<Want>> by_path;
    {
        std::lock_guard<std::mutex> lock(model_cache_->mutex);
        auto want = [&](uint32_t t, uint32_t kind, bool cached, std::string path) {
            if (cached) return;
            std::vector<Want>& w = by_path[std::move(path)];
            for (const Want& x : w)
                if (x.type == t && x.kind == kind) return;  // a type listed twice
            w.push_back({t, kind});
        };
        for (const uint32_t t : types) {
            if (t >= types_->size()) continue;
            if (what & kPreloadAvoid) want(t, kPreloadAvoid, model_cache_->avoid.count({t, sc}) != 0, avoid_path(t, space));
            if (what & kPreloadBones) want(t, kPreloadBones, model_cache_->entries.count({t, sc}) != 0, info_path(t, space, false));
            if (what & kPreloadExtents)
                want(t, kPreloadExtents, model_cache_->entries.count({t, sc | 2u}) != 0, info_path(t, space, true));
        }
    }
    std::vector<const std::pair<const std::string, std::vector<Want>>*> jobs;
    jobs.reserve(by_path.size());
    for (const auto& e : by_path) jobs.push_back(&e);
    if (jobs.empty()) return;
    const RangeFn body = [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            const auto& [path, wants] = *jobs[i];
            assets::ModelAsset m;
            const bool ok = !path.empty() && assets::load_model(*files_, path, m);
            for (const Want& w : wants) {
                if (w.kind == kPreloadAvoid) {
                    std::unique_ptr<tactical::movement::AvoidModel> out = ok ? avoid_from_asset(m) : nullptr;
                    std::lock_guard<std::mutex> lock(model_cache_->mutex);
                    model_cache_->avoid.emplace(std::make_pair(w.type, sc), std::move(out));
                } else {
                    std::unique_ptr<ModelInfo> out = ok ? info_from_asset(m) : nullptr;
                    const uint32_t code = w.kind == kPreloadExtents ? (sc | 2u) : sc;
                    std::lock_guard<std::mutex> lock(model_cache_->mutex);
                    model_cache_->entries.emplace(std::make_pair(w.type, code), std::move(out));
                }
            }
        }
    };
    if (pf) pf(static_cast<uint32_t>(jobs.size()), 1, body);
    else serial_for(static_cast<uint32_t>(jobs.size()), 1, body);
#else
    (void)types, (void)space, (void)what, (void)pf;
#endif
}

bool BattleCatalog::muzzle_bones(uint32_t type, bool space, std::vector<tactical::Vec3>& out) const {
    out.clear();
    const ModelInfo* m = model_info(type, space, false);
    if (!m) return false;
    std::string base = ascii_upper(ascii_trim(types_->get_string(type, "Muzzle_Bone_Name")));
    if (base.empty()) base = "MUZZLEA";  // type +0x580 default
    float scale = types_->columns().scale_factor[type];
    if (!(scale > 0.0f)) scale = 1.0f;
    for (uint32_t i = 0; i < 100; ++i) {
        const char nn[3] = {static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), 0};
        const int32_t b = m->find(base + "_" + nn);
        if (b < 0) break;  // the list ends at the first missing name
        // ALO model space (nose -Y, left +X, up +Z) -> the combat unit-local frame (x forward, y left, z up), as the
        // hard-point bones (hardpoint_on_model).
        const auto& p = m->bone_pos[static_cast<size_t>(b)];
        out.push_back(tactical::Vec3{-p[1] * scale, p[0] * scale, p[2] * scale});
    }
    return true;
}

bool BattleCatalog::target_bones(uint32_t type, bool space, std::vector<tactical::Vec3>& out) const {
    out.clear();
    const auto names = types_->get_list(type, "Target_Bones");
    if (names.empty()) return true;
    const ModelInfo* m = model_info(type, space, false);
    if (!m) return false;
    float scale = types_->columns().scale_factor[type];
    if (!(scale > 0.0f)) scale = 1.0f;
    for (const std::string_view n : names) {
        const std::string name = ascii_upper(ascii_trim(n));
        if (name.empty()) continue;
        const int32_t b = m->find(name);
        if (b < 0) continue;  // missing on the model: dropped
        // ALO model space (nose -Y, left +X, up +Z) -> the combat unit-local frame, as muzzle_bones.
        const auto& p = m->bone_pos[static_cast<size_t>(b)];
        out.push_back(tactical::Vec3{-p[1] * scale, p[0] * scale, p[2] * scale});
    }
    return true;
}

bool BattleCatalog::entry_bones(uint32_t type, bool space, std::vector<tactical::Vec3>& out) const {
    out.clear();
    const auto names = types_->get_list(type, "Garrison_Bone_Names");
    if (names.empty()) return true;
    const ModelInfo* m = model_info(type, space, false);
    if (!m) return false;
    float scale = types_->columns().scale_factor[type];
    if (!(scale > 0.0f)) scale = 1.0f;
    for (const std::string_view n : names) {
        const std::string name = ascii_upper(ascii_trim(n));
        if (name.empty()) continue;
        const int32_t b = m->find(name);
        if (b < 0) continue;  // unresolvable on the model: dropped (0x375380)
        // ALO model space (nose -Y, left +X, up +Z) -> the unit-local frame (x forward, y left, z up), as target_bones.
        const auto& p = m->bone_pos[static_cast<size_t>(b)];
        out.push_back(tactical::Vec3{-p[1] * scale, p[0] * scale, p[2] * scale});
    }
    return true;
}

bool BattleCatalog::garrison_bones(uint32_t type, bool space, int32_t guns,
                                   std::vector<std::vector<tactical::combat::GarrisonBone>>& out) const {
    out.clear();
    const ModelInfo* m = model_info(type, space, false);
    if (!m) return false;
    if (guns <= 0) return true;
    float scale = types_->columns().scale_factor[type];
    if (!(scale > 0.0f)) scale = 1.0f;
    constexpr float kRadToDeg = 57.295779513082320876f;
    for (int32_t a = 0; a < m->max_alt; ++a) {  // M = the highest _ALT index: ALT M itself has no entry
        std::vector<tactical::combat::GarrisonBone> entry(static_cast<size_t>(guns));
        for (int32_t i = 0; i < guns; ++i) {
            const std::string name = (i <= 9 ? "FP_0" : "FP_") + std::to_string(i) + "_ALT" + std::to_string(a);
            const int32_t b = m->find(name);
            if (b < 0) continue;  // missing: -1
            tactical::combat::GarrisonBone& g = entry[static_cast<size_t>(i)];
            g.valid = true;
            // ALO model space (nose -Y, left +X, up +Z) -> the combat unit-local frame, as muzzle_bones; the x axis (matrix
            // column 0, row-major bone_rot[0] / [3] / [6]) turned the same way.
            const auto& p = m->bone_pos[static_cast<size_t>(b)];
            g.pos = tactical::Vec3{-p[1] * scale, p[0] * scale, p[2] * scale};
            if (static_cast<size_t>(b) < m->bone_rot.size()) {
                const auto& r = m->bone_rot[static_cast<size_t>(b)];
                const float ax = -r[3], ay = r[0];
                if (ax != 0.0f || ay != 0.0f) g.yaw_deg = dmath::atan2(ay, ax) * kRadToDeg;
            }
        }
        out.push_back(std::move(entry));
    }
    return true;
}

int32_t BattleCatalog::hardpoint_on_model(int32_t base, uint32_t unit_type, BuildState& s) const {
    if (!s.full || base < 0 || static_cast<size_t>(base) >= s.d->hardpoints.size()) return base;
    const ModelInfo* m = model_info(unit_type, s.model_mode == 1, false);
    if (!m) return base;
    const std::string key = ascii_upper(s.d->hardpoints[static_cast<size_t>(base)].name);
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
    out_unit_index.clear();
    const std::unique_ptr<BattleBuild> build = begin_battle(b, space);
    extend_battle(*build, unit_types, out_unit_index);
    return b;
}

std::unique_ptr<BattleCatalog::BattleBuild> BattleCatalog::begin_battle(BattleTables& out, bool space) const {
    out = BattleTables{};
    out.combat.damage = damage_;
    out.combat.priority_sets = priority_sets_;
    auto b = std::make_unique<BattleBuild>();
    b->state.d = &out.combat;
    b->state.full = &out;
    b->state.model_mode = space ? 1 : 0;
    return b;
}

void BattleCatalog::extend_battle(BattleBuild& b, const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index) const {
    if (unit_types.size() <= b.built) return;
    if (b.state.model_mode >= 0) {
        // The new types' bone data, loaded in parallel before the serial build below finds it cached; their avoid models
        // and (land) stamp extents come from the same parse, for the setup steps that follow.
        const bool space = b.state.model_mode == 1;
        preload(std::span<const uint32_t>(unit_types).subspan(b.built), space,
                kPreloadBones | kPreloadAvoid | (space ? 0u : kPreloadExtents), b.pf);
    }
    build_units(unit_types, b.built, out_unit_index, b.state);
    b.built = unit_types.size();
}

}  // namespace alamo::sim
