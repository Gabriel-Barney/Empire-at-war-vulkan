// S9 - read-only ability data: <Unit_Ability> records (Unit_Abilities_Data), special abilities
// (the type's <Abilities> list) and the per-object-type ability descriptor, plus their XML
// parsers (docs/re/ABILITIES_HARDPOINTS.md 2.2, 3.1-3.10, xml_field_tables.md).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/tactical/abilities/ability_types.hpp"
#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo::data {
class XmlNode;
}

namespace alamo::tactical::abilities {

// OI abilities: the CubicInterpolator (vtable 0x85c120, TACTICAL_GAPS §3.3) for ability curves (the Whirlwind
// Activation_Chance): points sorted by x, natural cubic spline (y'' = 0 at both ends), float; before the first point
// the first y, at or past the last point the last y.
struct CubicCurve {
    std::vector<float> x, y, y2;
    bool empty() const { return x.empty(); }
    void set(std::vector<std::pair<float, float>> points);  // sorts (stable) and builds the spline
    float eval(float v) const;
};

// One <Unit_Ability> (Unit_Abilities_Data entry).
struct UnitAbilityRecord {
    uint8_t type = 0;                    // Type (enum 2.1)
    combat::UnitAbilityDesc combat;      // Mod_Multiplier / Mod_Flag / Armor_Type / range + inaccuracy overrides
    bool active_by_default = false;      // Active_By_Default (applied at spawn [port], open item 2)
    float effective_radius = 0.0f;       // Effective_Radius
    float recharge_seconds = 0.0f;       // Recharge_Seconds
    float expiration_seconds = 0.0f;     // Expiration_Seconds (<= 0: no active phase)
    bool is_pulsing = false;
    float pulse_frequency_secs = 0.0f;
    int32_t max_pulses = 0;
    std::string alternate_icon_name;         // Alternate_Icon_Name: button art instead of the type's (GUI only, t42)
    std::string gui_activated_ability_name;  // special ability this forwards to (2.5)
    uint32_t gui_name_crc = 0;               // name_crc of it (+0x88)
    std::string spawned_object_type;
    int32_t max_spawned_objects = 0;
    float bomb_countdown_seconds = 0.0f;
    float area_effect_decal_distance = 0.0f;
    std::vector<std::string> projectile_override_names;  // Projectile_Types_Override
    std::vector<int32_t> projectile_override;            // resolved ProjectileDesc indices (-1 unknown)
    bool disable_movement = false;       // Disable_Movement
    bool friendly = false, stop_when_activated = false;
    int32_t target_position_z_offset = 0;
    bool must_be_bought_on_black_market = false;
    float damage_percent_when_activated = 0.0f;  // STIM_PACK self damage
    bool supports_autofire = false;
    std::string particle_effect;
    std::string armor_type_name;
    // ---- OI abilities ----
    int32_t spawned_type = -1;        // Spawned_Object_Type as a host TypeIndex (resolver.object_type), -1 unknown
    int32_t spawned_projectile = -1;  // ... as a ProjectileDesc index when it is a projectile type (resolver.projectile)
    std::vector<int32_t> target_types;  // Target_Types (LURE)
    // REMAINING_GAPS §5.2: Owner_Attachment_Bone (+0x110), DEPLOY_TROOPERS spawn bones in list order (comma lists and
    // repeated tags both append).
    std::vector<std::string> owner_attachment_bones;
};

// One special ability (<Tractor_Beam_Attack_Ability Name="...">, ...). Only the fields of the
// classes this module implements are interpreted; the rest are ignored.
struct SpecialAbilityDesc {
    std::string name;
    uint32_t name_crc = 0;  // name_crc(name), matched against UnitAbilityRecord::gui_name_crc
    std::string class_tag;  // XML element name
    SpecialClass cls = SpecialClass::Unsupported;
    ActivationStyle style = ActivationStyle::None;
    bool initially_enabled = true;  // Initially_Enabled [M default]
    // Target filter (0x48e290): category mask OR exact type, minus excluded types.
    uint64_t applicable_categories = 0;
    std::vector<int32_t> applicable_types, excluded_types;
    float min_range = 0.0f, max_range = 0.0f;    // Activation_Min_Range / Activation_Max_Range
    float target_speed_decrease = 0.0f;          // Target_Speed_Decrease_Percent
    float target_damage_increase = 0.0f;         // Target_Damage_Increase_Percent (no reader, 3.5)
    int32_t stacking_category = 0;
    float damage_per_frame = 0.0f;               // energy weapon
    float duration_secs = 0.0f;                  // Duration_In_Secs (jamming fallback)
    float stealth_transition_time = 0.0f;
    bool must_be_bought_on_black_market = false;
    // laser defense
    float defense_duration = 0.0f, protection_radius = 0.0f, zap_recharge = 0.0f, dist_travelled = 0.0f;
    uint32_t targeted_categories = 0;  // bit (1 << ProjectileCategory)
    // redirect / absorb blaster
    float redirect_chance = 0.0f, block_chance = 0.0f, reaction_arc_deg = 360.0f, absorb_chance = 0.0f;
    // force sight (fog follow-up)
    float force_sight_duration = 0.0f;  // Force_Sight_Duration_In_Secs
    // Service interval of the class's per-frame update (vfunc +0x18). OI abilities: -1 (the default, and what the parser
    // leaves) = per class (special_service_interval with the battle's FPS, TACTICAL_GAPS §1.4: 0 / 1 / FPS / 10); >= 0
    // overrides it.
    int32_t service_interval = -1;

    // ---- OI abilities: class fields (TACTICAL_GAPS.md §2.3; XML names in the comments) ----
    CubicCurve chance_curve;              // Activation_Chance with >= 2 values: (x, y) pairs (Whirlwind / Grenade)
    float activation_chance = 1.0f;       // Activation_Chance (one value)
    float damage_amount = 0.0f;           // Damage_Amount
    float damage_percentage = 0.0f;       // Damage_Percentage
    float damage_interval = 0.0f;         // Damage_Interval_In_Secs
    uint16_t damage_type = combat::kNoTypeId;  // Damage_Type (DamageArmorTable row)
    float damage_range = 0.0f;            // Damage_Range (Whirlwind) / Damage_Radius (Shield_Flare, Buzz)
    float travel_time = 0.0f;             // Travel_Time_In_Secs / Stun_ / Confuse_Travel_Time_In_Secs
    float wave_range = 0.0f;              // Stun_Range / Confuse_Range
    float wave_time = 0.0f;               // Stun_Time_In_Secs / Confuse_Time_In_Secs
    // Combat_Bonus (TACTICAL_COMBAT §4.3) / Enhance_Defense: per list (ModList index) value, 0 = no entry
    std::array<float, combat::kModListCount> bonus{};
    bool apply_to_all_allies = false;     // Apply_To_All_Allies / Affects_All_Allies
    bool reverse_logic = false;           // Reverse_Application_Logic
    // Battlefield_Modifier (§2.3.11)
    float fow_reveal_multiplier = 1.0f, reinforcement_time_multiplier = 1.0f, stealth_duration_multiplier = 1.0f,
          capture_time_multiplier = 1.0f, reinforcement_deploy_multiplier = 1.0f;
    bool enable_bombing_runs = false, enable_bombardment = false, enable_special_weapons = false;
    // Force_Healing (§2.3.1)
    float heal_range = 0.0f, heal_percent = 0.0f, heal_amount = 0.0f, heal_interval = 0.0f;
    bool single_target_heal = false;
    // Berserker (§2.3.2)
    float berserker_speed = 0.0f, berserker_damage = 0.0f, berserker_damage_range = 0.0f, berserker_duration = 0.0f;
    // Proximity_Mines (§2.3.3) / Buzz_Droids / Summon / Spawn / Radioactive (Object_Type, Mine_Type, ...)
    float activation_time = 0.0f;         // Activation_Time_In_Seconds
    float trigger_radius = 0.0f;          // Trigger_Radius
    int32_t spawn_type = -1;              // Mine_Type / Object_Type / Contamination_Object_Name (host TypeIndex)
    int32_t spawn_count = 0;              // Number_Of_Mines / Number_Of_Bombs
    float creation_delay = 0.0f;          // Creation_Delay_In_Seconds / Spray_Time_In_Secs / Charge_Up_Seconds
    // Grenade / Remote_Bomb / Cluster_Bomb / Infection (§2.3.5, §2.3.12)
    int32_t projectile = -1;              // Grenade_Type / Bomb_Type / Projectile_Type / Explosion_Type (ProjectileDesc)
    float spawn_frame = 0.0f;             // Grenade_Spawn_Frame / Spawn_Frame
    std::string toss_anim;                // Toss_Anim (animation type name; t03 clip timing, empty = none)
    std::string spawn_bone;               // Spawn_Bone (Remote_Bomb +0x198, LOB_SOLVER §3.5; empty = none)
    float fuse_time = 0.0f;               // Grenade_Explode_Timer_In_Secs / Detonation_Time_In_Secs
    bool requires_direct_command = false; // Requires_Direct_Player_Command
    // Force_Lightning / Hack / Repair (§2.3.6)
    int32_t max_targets = 0;              // Lightning_Max_Targets / Max_Drain_Victims / Max_Targets
    float targets_radius = 0.0f;          // Lightning_Targets_Radius / Drain_Radius
    // melee (§2.3.8)
    float arc_width = 0.0f, arc_min = 0.0f, arc_max = 0.0f, sweep_time = 0.0f, anim_speedup = 1.0f;
    bool right_to_left = false;
    std::string attack_anim;              // Attack_Animation (animation type name; t03 clip timing, empty = none)
    // Arc_Sweep Attack_Animation_Subindex (+0x198) / Super_Laser Attack_Anim_Sub_Index (+0x188): the explicit variant
    // of the attack clip (OI follow-up t01, §4.14); 0 elsewhere.
    int32_t anim_sub_index = 0;
    int32_t damage_frame = 0, begin_damage_frame = 0, attachment_frame = 0, destruction_frame = 0;
    float health_per_victim = 0.0f;
    // Super_Laser (§2.3.9)
    int32_t fire_frame = 0;
    bool face_target = false;
    float turn_rate = 0.0f, firing_cone = 0.0f, fire_delay = 0.0f, fire_continuation = 0.0f;
    // Drain_Life / Leech_Shields / Blast / Corrupt_Systems / Flame thrower (§2.3.10, §2.3.12)
    float damage_per_second = 0.0f;       // Damage_Per_Second / Shield_Damage_Per_Second / Shield_Drain_Per_Second
    float charging_time = 0.0f;           // Charging_Time_In_Seconds / Charge_Up_Seconds
    bool should_heal = false;
    float damage_multiplier = 1.0f;       // Damage_Multiplier (Leech_Shields / Blast) [L: application not traced]
    float max_damage_per_victim = 0.0f;   // Max_Damage_Per_Victim (Shield_Flare)
    float damage_arc = 0.0f;              // Damage_Arc_In_Degrees (flame thrower)
    // Infection (refix t02; Damage_Per_Second above, Duration_Of_Infection -> duration_secs)
    float distance_to_infect = 0.0f;      // Distance_To_Infect
    float seconds_to_infect = 0.0f;       // Seconds_To_Infect
    uint64_t infection_categories = 0;    // Infection_CategoryMask
    // Enable_Ability / Cancel_Special
    uint32_t ability_name_crc = 0;        // Ability_Name / Special_Ability_Name
    // Permanent_Weapon_Swap
    int32_t weapon_index = 0;             // Weapon_Index
    // R5 jamming visual (ABILITY_VISUALS §3, presentation only: never hashed / saved, no sim reader). Sensor_Jamming_Ability:
    // Blob_Material_Name (ShadowBlobMaterials.xml), Blob_Color (R, G, B, A 0..255 in XML -> 0..1 here [M]), Show_Blob_To_Enemies,
    // Owner_Particle_Effect (a particle / GameObjectType name) and the first Owner_Particle_Bone_Name.
    std::string blob_material;
    float blob_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool show_blob_to_enemies = false;
    std::string owner_particle_effect, owner_particle_bone;
};

// Ability description of one object type. Index = the host's ability-type index (for combat units
// usually the same index as the UnitDesc).
struct AbilityObjectDesc {
    std::string name;
    uint32_t name_crc = 0;                       // tractor-beam modifier source name (owner type name)
    std::vector<UnitAbilityRecord> unit_abilities;  // Unit_Abilities_Data, XML order
    std::vector<uint32_t> specials;              // AbilityData::specials indices, Abilities list order
    bool countdown = true;                       // has ABILITY_COUNTDOWN (behaviour 0x46)
    bool team_container = false;                 // has TEAM (squadron container)
    uint64_t category_mask = 0;                  // CategoryMask (tractor corvette rule, target filters)
    int32_t object_type = -1;                    // host TypeIndex (target filters)
    bool hero = false;                           // Is_Named_Hero || Is_Generic_Hero
    float stealth_detection_range = 0.0f;        // 0x3979d0: Target_Stealth_Units ? FOW reveal range : 0
    float passive_missile_shield_radius = 0.0f;  // Passive_Missile_Shield_Radius
    bool garrison_style = false;                 // obj +0x3a7 [M meaning]
    // Destroy_When_Stunned_Over_Water (type +0xe6; only MAL_Rocket_Vehicle sets it): [port] a STUNNABLE stun that starts
    // while the host reports the unit over water destroys it (ABILITIES_HARDPOINTS.md §4.12, no RE pass).
    bool destroy_when_stunned_over_water = false;
};

struct AbilityData {
    std::vector<AbilityObjectDesc> objects;
    std::vector<SpecialAbilityDesc> specials;
};

// Name resolution hooks for the parsers (all optional).
struct AbilityResolvers {
    std::function<uint64_t(std::string_view)> category;  // one GameObjectCategoryType name -> mask bit(s), 0 unknown
    std::function<int32_t(std::string_view)> object_type;  // -1 unknown
    std::function<int32_t(std::string_view)> projectile;   // ProjectileDesc index, -1 unknown
    std::function<uint16_t(std::string_view)> armor;       // DamageArmorTable armor index, kNoTypeId unknown
    // OI abilities: DamageArmorTable damage index (Damage_Type of melee classes), kNoTypeId unknown; and whether a
    // type name is a projectile type (Spawned_Object_Type resolution; unset: names starting "Proj" [port]).
    std::function<uint16_t(std::string_view)> damage;
    std::function<bool(std::string_view)> is_projectile;
};

// "WEAPON_DELAY_MULTIPLIER" / "Weapon_Delay" -> 1..12, 0 unknown (case-insensitive, suffix optional).
uint8_t mod_kind_from_name(std::string_view name);
// "ALLOW_TARGET_DIMINISHING_FIREPOWER" -> 3 (1 WEAPONS_ENABLED[_FLAG], 2 CAN_BE_SQUASHED [M]), 0 unknown.
uint8_t mod_flag_from_name(std::string_view name);
// "Transport | Corvette" via the resolver; "All" = every bit.
uint64_t parse_category_mask(std::string_view text, const AbilityResolvers& r, std::vector<std::string>* diag = nullptr);
// MISSILE, ROCKET, ... -> bit mask of ProjectileCategory.
uint32_t parse_projectile_categories(std::string_view text);

// One <Unit_Ability> element. Unknown Type -> false (diagnostic). Values accept a trailing 'f'.
bool parse_unit_ability(const data::XmlNode& e, UnitAbilityRecord& out, const AbilityResolvers& r,
                        std::vector<std::string>* diag = nullptr);
// Every <Unit_Ability> under the type element's <Unit_Abilities_Data> children, in order.
std::vector<UnitAbilityRecord> parse_unit_abilities(const data::XmlNode& type_element, const AbilityResolvers& r,
                                                    std::vector<std::string>* diag = nullptr);
// One special-ability element (child of <Abilities>).
SpecialAbilityDesc parse_special_ability(const data::XmlNode& e, const AbilityResolvers& r,
                                         std::vector<std::string>* diag = nullptr);
// Every child of the type element's <Abilities> blocks, appended to data.specials; returns their indices.
std::vector<uint32_t> parse_special_abilities(const data::XmlNode& type_element, AbilityData& data, const AbilityResolvers& r,
                                              std::vector<std::string>* diag = nullptr);

// The record of `type` in a type's list: a later entry of the same Type replaces an earlier one (2.3).
const UnitAbilityRecord* find_record(const AbilityObjectDesc& d, uint8_t type);
// Fills UnitDesc::abilities (the first two Unit_Abilities_Data entries, Mod_Multiplier system B)
// by appending their combat descriptors to `cd.abilities`.
void link_combat_abilities(const AbilityObjectDesc& d, combat::CombatData& cd, combat::UnitDesc& ud);
// refix t02: UnitDesc::infection from the type's first Infection_Ability special (`specials` = AbilityData::specials, the
// pool d.specials indexes); no such special -> has = false.
void link_infection(const AbilityObjectDesc& d, std::span<const SpecialAbilityDesc> specials, combat::UnitDesc& ud);

}  // namespace alamo::tactical::abilities
