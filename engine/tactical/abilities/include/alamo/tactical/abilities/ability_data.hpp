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
    // LURE pulses (cu8f t06, CU8F_T04_T06_RE §3.1 [H]): every int(FPS x Pulse_Frequency_Secs) frames up to
    // Max_Number_Of_Pulses, then the behaviour suspends (AbilitySystem lure service).
    // Is_Pulsing (+0x2c) is parsed but has no reader in the original ([M]: LURE always pulses; the tag is even
    // registered as a float), so nothing reads it here either.
    bool is_pulsing = false;
    float pulse_frequency_secs = 0.0f;  // Pulse_Frequency_Secs (+0x30)
    int32_t max_pulses = 0;             // Max_Number_Of_Pulses (+0x34)
    std::string alternate_icon_name;         // Alternate_Icon_Name: button art instead of the type's (GUI only, t42)
    std::string gui_activated_ability_name;  // special ability this forwards to (2.5)
    uint32_t gui_name_crc = 0;               // name_crc of it (+0x88)
    std::string spawned_object_type;
    // Max_Num_Spawned_Objects (+0xa0, cu8f t06, CU8F_T04_T06_RE §3.2 [H]): DEPLOY_TROOPERS only - Can_Activate(38) is
    // false while the unit's live deployed squads reach it. [port] -1 = tag absent: no cap (the record ctor's default
    // was not read [L]; every vanilla DEPLOY_TROOPERS user sets 2).
    int32_t max_spawned_objects = -1;
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
    // Target_Types (+0x38, host TypeIndex, XML order; -1 unknown): read only by the LURE pulse (exact type match,
    // cu8f t06, CU8F_T04_T06_RE §3.1).
    std::vector<int32_t> target_types;
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
    // Target filter (0x48e290, TACTICAL_COMBAT §5.4.1): a listed type passes; else the categories must hit the CategoryMask
    // (empty = reject), then the excluded types reject. No tags: nothing passes. Tactical_Bribe never reads them.
    uint64_t applicable_categories = 0;
    std::vector<int32_t> applicable_types, excluded_types;
    float min_range = 0.0f, max_range = 0.0f;    // Activation_Min_Range / Activation_Max_Range
    float target_speed_decrease = 0.0f;          // Target_Speed_Decrease_Percent
    float target_damage_increase = 0.0f;         // Target_Damage_Increase_Percent (no reader, 3.5)
    int32_t stacking_category = 0;
    float damage_per_frame = 0.0f;               // energy weapon
    float duration_secs = 0.0f;                  // Duration_In_Secs (jamming fallback)
    float stealth_transition_time = 0.0f;  // parsed, not read by the sim: render-only fade (ABILITY_VISUALS §1.1)
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
    // Enable_Bombing_Runs (+0x191) / Enable_Special_Weapons (+0x192) / Enable_Planetary_Bombardment (+0x193) (cu8f t06,
    // CU8F_T04_T06_RE §3.3 [H]): the apply pushes a `true` entry (keyed by this source) onto each affected player's bool
    // list (AbilitySystem::player_gate), the remove deletes them; Reverse_Application_Logic swaps the two.
    bool enable_bombing_runs = false, enable_bombardment = false, enable_special_weapons = false;
    // Force_Healing (§2.3.1)
    float heal_range = 0.0f, heal_percent = 0.0f, heal_amount = 0.0f, heal_interval = 0.0f;
    bool single_target_heal = false;
    // Berserker (§2.3.2)
    float berserker_speed = 0.0f, berserker_damage = 0.0f, berserker_damage_range = 0.0f, berserker_duration = 0.0f;
    // Saber_Throw (cu8i t01, CU8F_T01_T02_T07_RE §2.6): Catch_Dist (+0x180) and the Saber_Name projectile type's Max_Speed
    // (per frame; 0 = unknown), for the thrown-saber flight stand-in (saber_flight_frames).
    float catch_dist = 0.0f;
    float saber_speed = 0.0f;
    float saber_max_flight = 0.0f;  // cu8j t03: the Saber_Name type's Projectile_Max_Flight_Distance (0 = unknown)
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
    // cu8f t06 (CU8F_T04_T06_RE §3.4 [H]; ctor defaults Turn_Rate 10, Firing_Cone 45): Face_Target with
    // TACTICAL_SUPER_WEAPON turns the owner's yaw toward the target by at most Turn_Rate degrees per update (<= 0: the
    // type's turn rate) and starts the attack clip once |yaw error| <= Firing_Cone (>= 90 reads as 0.01); Fire_Continuation:
    // after the target is dropped the beam runs until now + int(FPS x value) with no damage.
    bool face_target = false;
    float turn_rate = 10.0f, firing_cone = 45.0f, fire_delay = 0.0f, fire_continuation = 0.0f;
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
    // Infection_CategoryMask (cu8f t06, CU8F_T04_T06_RE §3.5 [H]): the spread's victim filter only (candidate type
    // CategoryMask & mask != 0; 0 = nothing catches it by proximity); a direct Causes_Infection hit is not filtered.
    uint64_t infection_categories = 0;
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
    // ---- s78l t05: Tactical_Bribe acceptance (TACTICAL_COMBAT §5.4.1, 0x5b0f70 / 0x5b0ec0 / 0x395c10) ----
    // Read from the target side (the team container for a member, else the unit) and the user side (the modifiers).
    // A team container's desc should carry its representative type's values (0x3973b0) [inferred].
    bool named_hero = false;              // Is_Named_Hero (type +0x13): never bribed (`hero` above also counts generic heroes)
    // Tactical_Bribe_Cost (type +0x2358, int): <= 1 = cannot be bribed. -1 = [port] the host supplied no value: the cost
    // gate is skipped and the bribe is free (hosts that predate the field).
    int32_t tactical_bribe_cost = -1;
    float bribe_modifier_corrupted = 1.0f;    // Bribe_Modifier_On_Corrupted_Planets (+0x23e4) [inferred default 1]
    float bribe_modifier_uncorrupted = 1.0f;  // Bribe_Modifier_On_Uncorrupted_Planets (+0x23e0) [inferred default 1]
    float friendly_damage_amount = 0.0f;      // Friendly_Damage_Amount (+0x23ec): > 0 cannot be bribed
    bool proximity_mine = false;              // has the PROXIMITY_MINE behaviour (0x55): cannot be bribed
    // cu8f t06 (CU8F_T04_T06_RE §3.4): has TACTICAL_SUPER_WEAPON (0x2c, slot +0x35e) - a Face_Target Super_Laser turns
    // through it; without it the attack clip starts at the apply.
    bool tactical_super_weapon = false;
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
    // cu8i t01: a type's Max_Speed by name (Saber_Throw's Saber_Name), without registering it as a ProjectileDesc; < 0
    // unknown.
    std::function<float(std::string_view)> type_max_speed;
    // cu8j t03: a type's Projectile_Max_Flight_Distance by name (Saber_Throw's Saber_Name); < 0 unknown.
    std::function<float(std::string_view)> type_max_flight_distance;
};

// cu8j t03 (CU8F_T01_T02_T07_RE §2.7 item 4) [port stand-in for the LIGHT_SABER projectile, no path jitter]: the thrown
// saber's frames counted from the throw. Ltot = |T - P0| + |L - T| (3D: T the target point at the apply, P0 the spawn
// point, L the thrower), B = max travel = MFD + R(target) + |T.z - P0.z| (MFD <= 0: no limit), v = speed per frame:
// k_c = floor((Ltot - catch_dist) / v) + 1; v k_c < min(Ltot, B): caught at step k_c, the ability ends 11 frames later
// (SWORD_CATCH, 30 frames at speed 3); else no catch: the saber dies at min(ceil(Ltot / v), ceil(B / v)) and the ability
// ends then (epsilon 0 [M]). Luke (v 8, Catch_Dist 15, MFD 250): 150 units, B 275 -> no catch, end 35 (retail 35); 120
// units -> catch 29, end 40 (retail 43, the jitter detour). v <= 0: no flight (end 0).
struct SaberFlight {
    uint32_t catch_step = 0;  // 0 = no catch
    uint32_t end = 0;         // the ability's end (SAP +0x2e8 cleared), frames after the throw; 0 = no flight
};
SaberFlight saber_flight(float out_dist, float back_dist, float max_travel, float speed, float catch_dist);
// cu8j t03 [M]: the throw (anim 0x6c SWORD_THROW past its half, +3 frames a tick) comes 7 (or 8) frames after the apply.
inline constexpr uint32_t kSaberThrowDelay = 7;

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

// Fills UnitDesc::abilities (the first two Unit_Abilities_Data entries, Mod_Multiplier system B)
// by appending their combat descriptors to `cd.abilities`.
void link_combat_abilities(const AbilityObjectDesc& d, combat::CombatData& cd, combat::UnitDesc& ud);
// refix t02: UnitDesc::infection from the type's first Infection_Ability special (`specials` = AbilityData::specials, the
// pool d.specials indexes); no such special -> has = false.
void link_infection(const AbilityObjectDesc& d, std::span<const SpecialAbilityDesc> specials, combat::UnitDesc& ud);

}  // namespace alamo::tactical::abilities
