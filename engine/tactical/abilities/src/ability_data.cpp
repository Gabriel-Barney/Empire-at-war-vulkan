#include "alamo/tactical/abilities/ability_data.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml.hpp"

namespace alamo::tactical::abilities {

namespace {

using combat::kModKindCount;
using combat::ModKind;
using combat::ProjectileCategory;

struct KindName {
    const char* name;
    uint8_t kind;
};
constexpr KindName kKinds[] = {
    {"WEAPON_DELAY", 1},  {"SHIELD_REGEN", 2},  {"SHIELD_REGEN_INTERVAL", 3}, {"ENERGY_REGEN", 4},
    {"ENERGY_REGEN_INTERVAL", 5}, {"SPEED", 6}, {"TAKE_DAMAGE", 7}, {"CAUSE_DAMAGE", 8},
    {"FIRE_RATE", 9}, {"SCATTER_RADIUS", 10}, {"TARGET_SPEED", 11}, {"TARGET_TAKE_DAMAGE", 12},
};
constexpr KindName kFlags[] = {
    {"WEAPONS_ENABLED", 1}, {"WEAPONS_ENABLED_FLAG", 1}, {"CAN_BE_SQUASHED", 2}, {"ALLOW_TARGET_DIMINISHING_FIREPOWER", 3},
};
constexpr KindName kProjCats[] = {
    {"DEFAULT", 0}, {"MISSILE", 1}, {"BOMB", 2}, {"GRENADE", 3}, {"ROCKET", 4},
    {"STATIC_BOMB", 5}, {"MPTL_ROCKET", 6}, {"LASER", 7}, {"LIGHT_SABER", 8}, {"CLUSTER_BOMB", 9},
};

bool ends_with_i(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

void note(std::vector<std::string>* diag, const data::XmlNode& e, std::string msg) {
    if (diag) diag->push_back(std::to_string(e.line()) + ": " + std::move(msg));
}

float f_or(const data::XmlNode& e, std::string_view tag, float fallback) {
    const data::XmlNode c = e.child(tag);
    return c ? data::parse_float_or(c.text_trimmed(), fallback) : fallback;
}
int32_t i_or(const data::XmlNode& e, std::string_view tag, int32_t fallback) {
    const data::XmlNode c = e.child(tag);
    return c ? data::parse_int_or(c.text_trimmed(), fallback) : fallback;
}
bool b_or(const data::XmlNode& e, std::string_view tag, bool fallback) {
    const data::XmlNode c = e.child(tag);
    return c ? data::parse_bool_or(c.text_trimmed(), fallback) : fallback;
}
std::string s_or(const data::XmlNode& e, std::string_view tag) {
    const data::XmlNode c = e.child(tag);
    return c ? std::string(c.text_trimmed()) : std::string();
}

std::vector<int32_t> type_list(std::string_view text, const AbilityResolvers& r) {
    std::vector<int32_t> out;
    for (std::string_view n : data::split_list(text)) out.push_back(r.object_type ? r.object_type(n) : -1);
    return out;
}

// OI abilities
bool is_projectile_name(std::string_view n, const AbilityResolvers& r) {
    n = data::trim(n);
    if (n.empty()) return false;
    if (r.is_projectile) return r.is_projectile(n);
    return n.size() >= 4 && iequals(n.substr(0, 4), "Proj");  // [port] naming convention fallback
}
int32_t projectile_of(const data::XmlNode& e, std::string_view tag, const AbilityResolvers& r) {
    const std::string_view n = data::trim(e.child_text(tag));
    if (n.empty() || !r.projectile) return -1;
    return r.projectile(n);
}
int32_t object_of(const data::XmlNode& e, std::string_view tag, const AbilityResolvers& r) {
    const std::string_view n = data::trim(e.child_text(tag));
    if (n.empty() || !r.object_type) return -1;
    return r.object_type(n);
}
// First present tag of a list (classes spell the same field differently).
float f_any(const data::XmlNode& e, std::initializer_list<std::string_view> tags, float fallback) {
    for (std::string_view t : tags)
        if (const data::XmlNode c = e.child(t)) return data::parse_float_or(c.text_trimmed(), fallback);
    return fallback;
}

void parse_class_fields(const data::XmlNode& e, SpecialAbilityDesc& s, const AbilityResolvers& r) {
    // Activation_Chance: one value, or a flat (x, y) list read as a CubicInterpolator (§2.3.7, §3.3).
    {
        std::vector<float> v;
        for (std::string_view t : data::split_list(e.child_text("Activation_Chance"))) {
            const auto f = data::parse_float(t);
            if (f) v.push_back(*f);
        }
        if (v.size() == 1) {
            s.activation_chance = v[0];
        } else if (v.size() >= 2) {
            std::vector<std::pair<float, float>> pts;
            for (size_t i = 0; i + 1 < v.size(); i += 2) pts.emplace_back(v[i], v[i + 1]);
            s.chance_curve.set(std::move(pts));
        }
    }
    s.damage_amount = f_or(e, "Damage_Amount", 0.0f);
    s.damage_percentage = f_or(e, "Damage_Percentage", 0.0f);
    s.damage_interval = f_or(e, "Damage_Interval_In_Secs", 0.0f);
    if (r.damage) {
        const std::string_view dt = data::trim(e.child_text("Damage_Type"));
        if (!dt.empty()) s.damage_type = r.damage(dt);
    }
    s.damage_range = f_any(e, {"Damage_Range", "Damage_Radius"}, 0.0f);
    s.travel_time = f_any(e, {"Travel_Time_In_Secs", "Stun_Travel_Time_In_Secs", "Confuse_Travel_Time_In_Secs"}, 0.0f);
    s.wave_range = f_any(e, {"Stun_Range", "Confuse_Range"}, 0.0f);
    s.wave_time = f_any(e, {"Stun_Time_In_Secs", "Confuse_Time_In_Secs"}, 0.0f);
    // Combat_Bonus (TACTICAL_COMBAT §4.3 lists) / Enhance_Defense (list 0) [inferred].
    using L = combat::ModList;
    auto set_bonus = [&s](L l, float v) { s.bonus[static_cast<size_t>(l)] = v; };
    set_bonus(L::MaxHealth, f_or(e, "Health_Bonus_Percentage", 0.0f));
    set_bonus(L::Damage, f_or(e, "Damage_Bonus_Percentage", 0.0f));
    set_bonus(L::MaxEnergy, f_or(e, "Energy_Pool_Bonus_Percentage", 0.0f));
    set_bonus(L::MaxShield, f_or(e, "Shield_Bonus_Percentage", 0.0f));
    set_bonus(L::MaxSpeed, f_or(e, "Movement_Speed_Bonus_Percentage", 0.0f));
    set_bonus(L::Defense, f_any(e, {"Defense_Bonus_Percentage", "Enhancement_Percentage"}, 0.0f));
    set_bonus(L::AbilityRecharge, f_or(e, "Ability_Recharge_Bonus_Percentage", 0.0f));
    set_bonus(L::FireRange, f_or(e, "Fire_Range_Bonus_Percentage", 0.0f));
    set_bonus(L::BuildTime, f_or(e, "Tactical_Build_Time_Bonus_Percentage", 0.0f));
    set_bonus(L::FireRate, f_or(e, "Fire_Rate_Bonus_Percentage", 0.0f));  // [M: the fire-rate field name]
    s.apply_to_all_allies = b_or(e, "Apply_To_All_Allies", false) || b_or(e, "Affects_All_Allies", false);
    s.reverse_logic = b_or(e, "Reverse_Application_Logic", false);
    s.fow_reveal_multiplier = f_or(e, "FOW_Reveal_Range_Multiplier", 1.0f);
    s.reinforcement_time_multiplier = f_or(e, "Reinforcement_Time_Multiplier", 1.0f);
    s.stealth_duration_multiplier = f_or(e, "Stealth_Duration_Multiplier", 1.0f);
    s.capture_time_multiplier = f_or(e, "Capture_Point_Time_Multiplier", 1.0f);
    s.reinforcement_deploy_multiplier = f_or(e, "Reinforcement_Deploy_Time_Multiplier", 1.0f);
    s.enable_bombing_runs = b_or(e, "Enable_Bombing_Runs", false);
    s.enable_bombardment = b_or(e, "Enable_Planetary_Bombardment", false);
    s.enable_special_weapons = b_or(e, "Enable_Special_Weapons", false);
    s.heal_range = f_or(e, "Heal_Range", 0.0f);
    s.heal_percent = f_or(e, "Heal_Percent", 0.0f);
    s.heal_amount = f_or(e, "Heal_Amount", 0.0f);
    s.heal_interval = f_or(e, "Heal_Interval_In_Secs", 0.0f);
    s.single_target_heal = b_or(e, "Single_Target_Heal", false);
    s.berserker_speed = f_or(e, "Berserker_Speed", 0.0f);
    s.berserker_damage = f_or(e, "Berserker_Damage", 0.0f);
    s.berserker_damage_range = f_or(e, "Berserker_Damage_Range", 0.0f);
    s.berserker_duration = f_or(e, "Berserker_Duration_In_Secs", 0.0f);
    s.catch_dist = f_or(e, "Catch_Dist", 0.0f);  // cu8i t01: Saber_Throw
    if (r.type_max_speed) {
        const std::string_view saber = data::trim(e.child_text("Saber_Name"));
        if (!saber.empty()) s.saber_speed = std::max(0.0f, r.type_max_speed(saber));
        if (!saber.empty() && r.type_max_flight_distance) s.saber_max_flight = std::max(0.0f, r.type_max_flight_distance(saber));
        if (!saber.empty() && r.projectile) s.saber_projectile = r.projectile(saber);  // cu8k t01: the thrown saber's desc
    }
    s.activation_time = f_or(e, "Activation_Time_In_Seconds", 0.0f);
    s.trigger_radius = f_or(e, "Trigger_Radius", 0.0f);
    for (std::string_view tag : {"Mine_Type", "Object_Type", "Contamination_Object_Name"})
        if (s.spawn_type < 0) s.spawn_type = object_of(e, tag, r);
    s.spawn_count = i_or(e, "Number_Of_Mines", i_or(e, "Number_Of_Bombs", 0));
    s.creation_delay = f_any(e, {"Creation_Delay_In_Seconds", "Spray_Time_In_Secs"}, 0.0f);
    for (std::string_view tag : {"Grenade_Type", "Bomb_Type", "Projectile_Type", "Explosion_Type"})
        if (s.projectile < 0) s.projectile = projectile_of(e, tag, r);
    s.spawn_frame = f_any(e, {"Grenade_Spawn_Frame", "Spawn_Frame"}, 0.0f);
    // Grenade_Attack +0x1c0 is Grenade_Toss_Anim; Toss_Anim (+0x190) belongs to Remote_Bomb (xml_field_tables.md).
    s.toss_anim = s_or(e, "Grenade_Toss_Anim");
    if (s.toss_anim.empty()) s.toss_anim = s_or(e, "Toss_Anim");
    s.spawn_bone = s_or(e, "Spawn_Bone");  // S8 t06: Remote_Bomb toss bone (LOB_SOLVER §3.5)
    s.fuse_time = f_any(e, {"Grenade_Explode_Timer_In_Secs", "Detonation_Time_In_Secs"}, 0.0f);
    s.requires_direct_command = b_or(e, "Requires_Direct_Player_Command", false);
    s.max_targets = i_or(e, "Lightning_Max_Targets", i_or(e, "Max_Drain_Victims", i_or(e, "Max_Targets", 0)));
    s.targets_radius = f_any(e, {"Lightning_Targets_Radius", "Drain_Radius"}, 0.0f);
    s.arc_width = f_or(e, "Arc_Width_In_Degrees", 0.0f);
    s.arc_min = f_or(e, "Arc_Min_Range", 0.0f);
    s.arc_max = f_or(e, "Arc_Max_Range", 0.0f);
    s.sweep_time = f_or(e, "Damage_Sweep_Time_In_Secs", 0.0f);
    s.anim_speedup = f_or(e, "Attack_Animation_Speedup", 1.0f);
    s.right_to_left = b_or(e, "Apply_Damage_Right_To_Left", false);
    s.attack_anim = s_or(e, "Attack_Animation");
    s.anim_sub_index = i_or(e, "Attack_Animation_Subindex", i_or(e, "Attack_Anim_Sub_Index", 0));
    s.damage_frame = i_or(e, "Damage_Frame_Number", 0);
    s.begin_damage_frame = i_or(e, "Begin_Applying_Damage_Frame", 0);
    s.attachment_frame = i_or(e, "Attachment_Frame", 0);
    s.destruction_frame = i_or(e, "Target_Destruction_Frame", 0);
    s.health_per_victim = f_or(e, "Health_Per_Victim", 0.0f);
    s.fire_frame = i_or(e, "Fire_Frame", 0);
    s.face_target = b_or(e, "Face_Target", false);
    s.turn_rate = f_or(e, "Turn_Rate", 10.0f);      // ctor 0x714e30 defaults (cu8f t06)
    s.firing_cone = f_or(e, "Firing_Cone", 45.0f);
    s.fire_delay = f_or(e, "Fire_Delay", 0.0f);
    s.fire_continuation = f_or(e, "Fire_Continuation", 0.0f);
    s.damage_per_second = f_any(e, {"Damage_Per_Second", "Shield_Damage_Per_Second", "Shield_Drain_Per_Second"}, 0.0f);
    s.charging_time = f_any(e, {"Charging_Time_In_Seconds", "Charge_Up_Seconds"}, 0.0f);
    s.should_heal = b_or(e, "Should_Heal", false);
    s.damage_multiplier = f_or(e, "Damage_Multiplier", 1.0f);
    s.max_damage_per_victim = f_or(e, "Max_Damage_Per_Victim", 0.0f);
    s.damage_arc = f_or(e, "Damage_Arc_In_Degrees", 0.0f);
    s.distance_to_infect = f_or(e, "Distance_To_Infect", 0.0f);
    s.seconds_to_infect = f_or(e, "Seconds_To_Infect", 0.0f);
    s.infection_categories = parse_category_mask(e.child_text("Infection_CategoryMask"), r);
    for (std::string_view tag : {"Ability_Name", "Special_Ability_Name"}) {
        const std::string_view n = data::trim(e.child_text(tag));
        if (!n.empty() && s.ability_name_crc == 0) s.ability_name_crc = name_crc(n);
    }
    s.weapon_index = i_or(e, "Weapon_Index", 0);
    // Durations: the classes spell them Duration_In_Secs / Duration_In_Seconds / Fire_Time_In_Secs.
    if (s.duration_secs == 0.0f) s.duration_secs = f_any(e, {"Duration_In_Seconds", "Fire_Time_In_Secs", "Duration_Of_Infection"}, 0.0f);
}

}  // namespace

SaberFlight saber_flight(float out_dist, float back_dist, float max_travel, float speed, float catch_dist) {
    SaberFlight f;
    if (!(speed > 0.0f)) return f;
    const float ltot = std::max(0.0f, out_dist) + std::max(0.0f, back_dist);
    const float b = max_travel > 0.0f ? max_travel : 3.0e38f;
    const float kc = std::floor(std::max(0.0f, ltot - catch_dist) / speed) + 1.0f;
    if (speed * kc < std::min(ltot, b)) {
        f.catch_step = static_cast<uint32_t>(kc);
        f.end = f.catch_step + 11u;  // SWORD_CATCH finishes at the 10th advance after its play tick, seen by the next update
        return f;
    }
    const float kd = std::min(std::ceil(ltot / speed), std::ceil(b / speed));
    f.end = std::max(1u, static_cast<uint32_t>(kd));  // epsilon 0 [M]
    return f;
}

// ---- OI abilities: CubicInterpolator ----
void CubicCurve::set(std::vector<std::pair<float, float>> pts) {
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t n = pts.size();
    x.resize(n);
    y.resize(n);
    y2.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        x[i] = pts[i].first;
        y[i] = pts[i].second;
    }
    if (n < 3) return;
    // Natural spline second derivatives (tridiagonal solve, float).
    std::vector<float> u(n, 0.0f);
    for (size_t i = 1; i + 1 < n; ++i) {
        const float hl = x[i] - x[i - 1], hr = x[i + 1] - x[i], hs = x[i + 1] - x[i - 1];
        if (!(hl > 0.0f) || !(hr > 0.0f) || !(hs > 0.0f)) continue;
        const float sig = hl / hs;
        const float p = sig * y2[i - 1] + 2.0f;
        y2[i] = (sig - 1.0f) / p;
        const float d = (y[i + 1] - y[i]) / hr - (y[i] - y[i - 1]) / hl;
        u[i] = (6.0f * d / hs - sig * u[i - 1]) / p;
    }
    y2[n - 1] = 0.0f;
    for (size_t k = n - 1; k-- > 0;) y2[k] = y2[k] * y2[k + 1] + u[k];
}

float CubicCurve::eval(float v) const {
    const size_t m = x.size();
    if (m == 0) return 0.0f;
    const size_t k = static_cast<size_t>(std::upper_bound(x.begin(), x.end(), v) - x.begin());
    if (k >= m) return y[m - 1];
    if (k == 0) return y[0];
    const float h = x[k] - x[k - 1];
    if (!(h > 0.0f)) return y[k];
    const float a = (x[k] - v) / h, b = (v - x[k - 1]) / h;
    return a * y[k - 1] + b * y[k] + ((a * a * a - a) * y2[k - 1] + (b * b * b - b) * y2[k]) * (h * h) / 6.0f;
}

uint8_t mod_kind_from_name(std::string_view name) {
    name = data::trim(name);
    if (ends_with_i(name, "_MULTIPLIER")) name = name.substr(0, name.size() - 11);
    for (const KindName& k : kKinds)
        if (iequals(k.name, name)) return k.kind;
    return 0;
}

uint8_t mod_flag_from_name(std::string_view name) {
    name = data::trim(name);
    for (const KindName& k : kFlags)
        if (iequals(k.name, name)) return k.kind;
    return 0;
}

uint64_t parse_category_mask(std::string_view text, const AbilityResolvers& r, std::vector<std::string>* diag) {
    uint64_t mask = 0;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('|', i);
        if (j == std::string_view::npos) j = text.size();
        const std::string_view tok = data::trim(text.substr(i, j - i));
        if (!tok.empty()) {
            if (iequals(tok, "All")) {
                mask = ~uint64_t{0};
            } else {
                const uint64_t m = r.category ? r.category(tok) : 0;
                if (m == 0 && diag) diag->push_back("unknown category " + std::string(tok));
                mask |= m;
            }
        }
        i = j + 1;
    }
    return mask;
}

uint32_t parse_projectile_categories(std::string_view text) {
    uint32_t m = 0;
    for (std::string_view n : data::split_list(text))
        for (const KindName& k : kProjCats)
            if (iequals(k.name, n)) m |= 1u << k.kind;
    return m;
}

bool parse_unit_ability(const data::XmlNode& e, UnitAbilityRecord& out, const AbilityResolvers& r,
                        std::vector<std::string>* diag) {
    out = UnitAbilityRecord{};
    const std::string_view type_name = e.child_text("Type");
    out.type = ability_type_from_name(type_name);
    if (out.type == 0) {
        note(diag, e, "unknown Unit_Ability Type '" + std::string(type_name) + "'");
        return false;
    }
    out.combat.ability_type = out.type;
    out.active_by_default = b_or(e, "Active_By_Default", false);
    out.effective_radius = f_or(e, "Effective_Radius", 0.0f);
    out.recharge_seconds = f_or(e, "Recharge_Seconds", 0.0f);
    out.expiration_seconds = f_or(e, "Expiration_Seconds", 0.0f);
    out.is_pulsing = b_or(e, "Is_Pulsing", false);  // no reader in the original [M] (cu8f t06): kept for the record only
    out.pulse_frequency_secs = f_or(e, "Pulse_Frequency_Secs", 0.0f);
    out.max_pulses = i_or(e, "Max_Number_Of_Pulses", 0);
    out.alternate_icon_name = s_or(e, "Alternate_Icon_Name");
    out.gui_activated_ability_name = s_or(e, "GUI_Activated_Ability_Name");
    out.gui_name_crc = out.gui_activated_ability_name.empty() ? 0u : name_crc(out.gui_activated_ability_name);
    out.spawned_object_type = s_or(e, "Spawned_Object_Type");
    out.max_spawned_objects = i_or(e, "Max_Num_Spawned_Objects", -1);  // [port] absent: no cap (cu8f t06)
    out.bomb_countdown_seconds = f_or(e, "Bomb_Countdown_Seconds", 0.0f);
    out.area_effect_decal_distance = f_or(e, "Area_Effect_Decal_Distance", 0.0f);
    for (data::XmlNode c : e.children("Projectile_Types_Override"))
        for (std::string_view n : data::split_list(c.text_trimmed())) {
            out.projectile_override_names.emplace_back(n);
            out.projectile_override.push_back(r.projectile ? r.projectile(n) : -1);
        }
    out.disable_movement = b_or(e, "Disable_Movement", false);
    out.friendly = b_or(e, "Friendly_Ability", false);
    out.stop_when_activated = b_or(e, "Stop_When_Activated", false);
    out.target_position_z_offset = i_or(e, "Target_Position_Z_Offset", 0);
    out.must_be_bought_on_black_market = b_or(e, "Must_Be_Bought_On_Black_Market", false);
    out.damage_percent_when_activated = f_or(e, "Damage_Percent_When_Activated", 0.0f);
    out.supports_autofire = b_or(e, "Supports_Autofire", false);
    out.particle_effect = s_or(e, "Particle_Effect");
    // OI abilities: Spawned_Object_Type (instant effects / SELF_DESTRUCT / DEPLOY_TROOPERS), Target_Types (LURE).
    if (!out.spawned_object_type.empty()) {
        out.spawned_type = r.object_type ? r.object_type(out.spawned_object_type) : -1;
        if (is_projectile_name(out.spawned_object_type, r) && r.projectile) out.spawned_projectile = r.projectile(out.spawned_object_type);
    }
    out.target_types = type_list(e.child_text("Target_Types"), r);
    for (data::XmlNode c : e.children("Owner_Attachment_Bone"))  // REMAINING_GAPS §5.2
        for (std::string_view n : data::split_list(c.text_trimmed())) out.owner_attachment_bones.emplace_back(n);
    out.combat.range_override = f_or(e, "Targeting_Max_Attack_Distance", 0.0f);
    out.combat.inaccuracy_override = f_or(e, "Targeting_Fire_Inaccuracy_Fixed_Radius_Override", 0.0f);
    out.armor_type_name = s_or(e, "Armor_Type");
    if (!out.armor_type_name.empty() && r.armor) out.combat.armor_override = r.armor(out.armor_type_name);
    // <Mod_Multiplier>KIND_MULTIPLIER, value</Mod_Multiplier>, value with an optional trailing 'f'.
    for (data::XmlNode c : e.children("Mod_Multiplier")) {
        const auto pr = data::parse_pair(c.text_trimmed());
        const uint8_t k = pr ? mod_kind_from_name(pr->first) : 0;
        const auto v = pr ? data::parse_float(pr->second) : std::nullopt;
        if (k == 0 || !v) {
            note(diag, c, "bad Mod_Multiplier '" + std::string(c.text_trimmed()) + "'");
            continue;
        }
        out.combat.mult[static_cast<size_t>(k) - 1] = *v;
    }
    for (data::XmlNode c : e.children("Mod_Flag")) {
        const auto pr = data::parse_pair(c.text_trimmed());
        const uint8_t k = pr ? mod_flag_from_name(pr->first) : 0;
        const auto v = pr ? data::parse_bool(pr->second) : std::nullopt;
        if (k == 0 || k >= combat::kModFlagCount || !v) {
            note(diag, c, "bad Mod_Flag '" + std::string(c.text_trimmed()) + "'");
            continue;
        }
        out.combat.flag[k] = *v ? 1 : 0;
    }
    return true;
}

std::vector<UnitAbilityRecord> parse_unit_abilities(const data::XmlNode& type_element, const AbilityResolvers& r,
                                                    std::vector<std::string>* diag) {
    std::vector<UnitAbilityRecord> out;
    for (data::XmlNode block : type_element.children("Unit_Abilities_Data"))
        for (data::XmlNode e : block.children("Unit_Ability")) {
            UnitAbilityRecord rec;
            if (parse_unit_ability(e, rec, r, diag)) out.push_back(std::move(rec));
        }
    return out;
}

SpecialAbilityDesc parse_special_ability(const data::XmlNode& e, const AbilityResolvers& r, std::vector<std::string>* diag) {
    SpecialAbilityDesc s;
    s.class_tag = std::string(e.name());
    s.cls = special_class_from_tag(e.name());
    s.name = std::string(e.attribute("Name"));
    s.name_crc = s.name.empty() ? 0u : name_crc(s.name);
    const std::string_view style = e.child_text("Activation_Style");
    s.style = activation_style_from_name(style);
    if (!style.empty() && s.style == ActivationStyle::None) note(diag, e, "unknown Activation_Style '" + std::string(style) + "'");
    s.initially_enabled = b_or(e, "Initially_Enabled", true);
    s.applicable_categories = parse_category_mask(e.child_text("Applicable_Unit_Categories"), r, diag);
    s.applicable_types = type_list(e.child_text("Applicable_Unit_Types"), r);
    s.excluded_types = type_list(e.child_text("Excluded_Unit_Types"), r);
    s.min_range = f_or(e, "Activation_Min_Range", 0.0f);
    s.max_range = f_or(e, "Activation_Max_Range", 0.0f);
    s.target_speed_decrease = f_or(e, "Target_Speed_Decrease_Percent", 0.0f);
    s.target_damage_increase = f_or(e, "Target_Damage_Increase_Percent", 0.0f);
    s.stacking_category = i_or(e, "Stacking_Category", 0);
    s.damage_per_frame = f_or(e, "Damage_Per_Frame", 0.0f);
    s.duration_secs = f_or(e, "Duration_In_Secs", 0.0f);
    s.force_sight_duration = f_or(e, "Force_Sight_Duration_In_Secs", 0.0f);  // fog follow-up
    s.stealth_transition_time = f_or(e, "Stealth_Transition_Time", 0.0f);
    s.must_be_bought_on_black_market = b_or(e, "Must_Be_Bought_On_Black_Market", false);
    s.defense_duration = f_or(e, "Defense_Duration_In_Secs", 0.0f);
    s.protection_radius = f_or(e, "Protection_Radius", 0.0f);
    s.zap_recharge = f_or(e, "Recharge_Time_In_Secs", 0.0f);
    s.dist_travelled = f_or(e, "Projectile_Dist_Travelled", 0.0f);
    s.targeted_categories = parse_projectile_categories(e.child_text("Projectile_Types_Targeted"));
    s.redirect_chance = f_or(e, "Redirect_Chance", 0.0f);
    s.block_chance = f_or(e, "Block_Chance", 0.0f);
    s.reaction_arc_deg = f_or(e, "Reaction_Arc_In_Degrees", 360.0f);
    s.absorb_chance = f_or(e, "Absorb_Chance", 0.0f);
    parse_class_fields(e, s, r);  // OI abilities (service_interval stays -1: per class)
    // R5 jamming visual (presentation only): Blob_* / Owner_Particle_* of Sensor_Jamming_Ability.
    if (s.cls == SpecialClass::SensorJamming) {
        s.blob_material = s_or(e, "Blob_Material_Name");
        const std::vector<std::string_view> bc = data::split_list(e.child_text("Blob_Color"));
        for (size_t k = 0; k < 4 && k < bc.size(); ++k)
            s.blob_color[k] = std::clamp(data::parse_float_or(bc[k], 255.0f) / 255.0f, 0.0f, 1.0f);
        s.show_blob_to_enemies = b_or(e, "Show_Blob_To_Enemies", false);
        s.owner_particle_effect = s_or(e, "Owner_Particle_Effect");
        const std::vector<std::string_view> bones = data::split_list(e.child_text("Owner_Particle_Bone_Name"));
        if (!bones.empty()) s.owner_particle_bone = std::string(bones.front());
    }
    return s;
}

std::vector<uint32_t> parse_special_abilities(const data::XmlNode& type_element, AbilityData& data, const AbilityResolvers& r,
                                              std::vector<std::string>* diag) {
    std::vector<uint32_t> out;
    for (data::XmlNode block : type_element.children("Abilities"))
        for (data::XmlNode e = block.first_child(); e; e = e.next_sibling()) {
            out.push_back(static_cast<uint32_t>(data.specials.size()));
            data.specials.push_back(parse_special_ability(e, r, diag));
        }
    return out;
}

void link_combat_abilities(const AbilityObjectDesc& d, combat::CombatData& cd, combat::UnitDesc& ud) {
    ud.abilities = {-1, -1};
    for (size_t s = 0; s < 2 && s < d.unit_abilities.size(); ++s) {
        ud.abilities[s] = static_cast<int32_t>(cd.abilities.size());
        cd.abilities.push_back(d.unit_abilities[s].combat);
    }
}

void link_infection(const AbilityObjectDesc& d, std::span<const SpecialAbilityDesc> specials, combat::UnitDesc& ud) {
    ud.infection = combat::InfectionDesc{};
    for (const uint32_t i : d.specials) {
        if (i >= specials.size() || specials[i].cls != SpecialClass::Infection) continue;
        const SpecialAbilityDesc& s = specials[i];
        ud.infection.has = true;
        ud.infection.distance_to_infect = s.distance_to_infect;
        ud.infection.seconds_to_infect = s.seconds_to_infect;
        ud.infection.duration_secs = s.duration_secs;
        ud.infection.damage_per_second = s.damage_per_second;
        ud.infection.category_mask = s.infection_categories;
        return;
    }
}

}  // namespace alamo::tactical::abilities
