// S8 - CombatSystem: targeting, weapons, projectiles, damage and both modifier systems for one
// tactical battle (docs/re/TACTICAL_COMBAT.md sections 2-4; section 5 only as far as damage routing
// needs it - S9 owns hardpoint systems and abilities).
//
// DATA MODEL
//   * CombatData: read-only type tables the host fills once (unit, hardpoint, projectile, ability
//     descriptors, damage/armor matrix, priority sets). Indices are dense; no strings at run time.
//   * Units: one flat record per combatant, kept sorted by gameplay object id (lookup = binary
//     search, no hash map). The host adds units in id order (ids are never reused) and pushes
//     positions/facings before each tick. Hardpoints are flat sub-records of their unit (HardPoints
//     list order), not separate objects.
//   * Projectiles: a FLAT TABLE, not ObjectStore objects (decision for S8). A projectile is never a
//     target, never in a spatial tree and never collides with other projectiles, so a table with a
//     monotonic serial (the deterministic order key) is enough and avoids the object-store and
//     id-counter cost per shot. Deviation from the original: retail projectiles are GameObjects and
//     consume object ids, which shifts the ids of later-created objects ([M] matters only for
//     bit-parity with retail saves/ids, not for our own determinism).
//   * Modifiers: system A (ModifierSet per unit and per player, modifiers.hpp) with a per-unit cache
//     of the 11 reduced values; system B (Mod_Multiplier) cached per unit per tick.
//
// TICK (tick(frame, trees, rng, pf)) - phase order of this port. Parallel phases write per-index
// outputs only and never draw from the RNG; every synced draw happens in a serial phase, in a fixed
// order, so results are identical for any worker count:
//   A  modifiers   [parallel per unit] purge expired object entries (player sets serially first),
//                  refresh the reduced-value cache and the Mod_Multiplier products; [serial] squad min.
//   B  upkeep      [serial] delayed-damage countdowns (queue order); [parallel per unit] shield
//                  regeneration, hull <-> hardpoint 0.2 coupling.
//   C  gather      [parallel per unit due a target scan] tree queries per enemy player (cube of
//                  half-extent R), candidate filters 2.3, priority score, health fraction, XY distance,
//                  aim test -> per-unit candidate list (player, then object id order).
//   D  decide+fire [serial, newest unit first (descending id = the store's service order)]: scan
//                  reschedule draw, start-player draw, per-candidate aim-point draws, selection rule
//                  2.4; then each weapon hardpoint in HardPoints order (Update_Weapon / Fire_At:
//                  muzzle, lead, inaccuracy, spawn, recharge draws), then the non-hardpoint weapon.
//   E  sweep       [parallel per projectile] segment ray queries against every hostile player tree;
//                  first hit by (t, object id) among units alive at the start of the phase.
//   F  commit      [serial, projectile serial order] re-sweep if the victim died this phase, hit
//                  processing (random-hardpoint, delay and particle draws, Take_Damage, area damage),
//                  otherwise move and expire (area damage on expiry).
//   G  flush       deaths of this tick reported in deaths(); dead units and projectiles compacted.
// Projectiles spawned in D are first stepped on the next tick (objects created during the service
// loop are serviced next frame, OBJECT_RUNTIME 5.3).
//
// S9 additions (docs/re/ABILITIES_HARDPOINTS.md): CombatHooks (installed by the ability system) are
// called from the serial parts only: begin_tick after A; in D per unit countdown -> burning ->
// targeting -> pending ability / special abilities -> weapons (opportunity fire, ion / lucky
// projectile override, shot hooks) -> converted-unit decay; in F missile-shield deflection (emitters
// in ascending id), jammed missiles fly straight, ion stun on detonation; Take_Damage steps 11
// (lucky shot) and 13 (listeners), energy absorption, fire ignition, smoke events; the full Die
// sequence (hardpoints in list order, death clone, death projectile draw). Energy pools regenerate
// in B2. Outputs per tick: deaths(), hardpoint_events(), smoke_events(), damage_events().
//
// OI tactical (PHASE4 open items, S8): circular lead over a three-position history (formulas.hpp circular_lead,
// replaces linear lead in Fire_At / WeaponBehavior / host launches; straight tracks keep linear lead bit-for-bit),
// team targeting (members score with their squad head's priority set, filter 7 accepts the head's weapon, filter 9
// accepts the head's current target out of range).
// S8 t04 (docs/re/LOB_SOLVER.md): category-3 (grenade) shots - slot 44 = the lob solver finds an arc; the launch solves
// from the launch point to the un-led target point (lead / inaccuracy turn the yaw only), no solution = no shot;
// hardpoint grenades leave straight at the aim point; the flight is the closed-form parabola, detonating on the land
// ground test (On_Detonate at the hit) or an object hit; sticky grenades attach / rest; lifetime-only expiry.
//
// DETERMINISM: no pointer-keyed or hashed containers are iterated; candidate lists, ray hits and
// radius queries come sorted by object id from the S6 tree; float math is IEEE single, no FMA, sqrt
// and atan2 from dmath. Steady state allocates nothing per tick once reserve() covers the peak.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/combat/formulas.hpp"
#include "alamo/tactical/combat/modifiers.hpp"
#include "alamo/tactical/combat/rocket.hpp"
#include "alamo/tactical/combat/tables.hpp"
#include "alamo/tactical/geometry.hpp"
#include "alamo/tactical/tree_set.hpp"

namespace alamo {
class StateHasher;
class CrcRegistry;
class ByteWriter;
class ByteReader;
}  // namespace alamo
namespace alamo::data {
struct GameConstants;
}

namespace alamo::tactical::combat {

using ObjectId = int32_t;
inline constexpr ObjectId kNoObject = 0x3FFFFF;  // = objects::kNoObjectId

// Hardpoint type enum (SPACE_AND_OPEN B.10 / TACTICAL_COMBAT 5.1).
enum class HardpointKind : uint8_t {
    Engine = 0,
    ShieldGenerator = 1,
    GravityWell = 2,
    FighterBay = 3,
    TractorBeam = 4,
    WeaponLaser = 5,
    WeaponMissile = 6,
    WeaponTorpedo = 7,
    WeaponIonCannon = 8,
    WeaponMassDriver = 9,
    WeaponSpecial = 10,
    EnableSpecialAbility = 11,
    DummyArt = 12,
};
inline bool is_weapon(HardpointKind k) {
    return static_cast<uint8_t>(k) >= 5 && static_cast<uint8_t>(k) <= 10;
}

// tDamageType kinds known so far (section 3.6, [M]). 6 = owner death killing its hardpoints, 15 =
// no death clone, 0x11 = converted-unit decay (ABILITIES_HARDPOINTS.md 1.3, 4.9, 4.10).
enum DamageKind : uint8_t {
    kKindDebug = 0,
    kKindNormal = 1,
    kKindHardpointDeath = 6,
    kKindFire = 7,
    kKindDeployed = 11,
    kKindDeployedOther = 12,
    kKindVehicleThief = 13,
    kKindNoDeathClone = 15,
    kKindConvertedDecay = 0x11,
    kKindInfection = 0x14,
    kKindBypassShield = 22,
};

// Projectile_Category (0x469280) [H] (ABILITIES_HARDPOINTS.md 4.3).
enum class ProjectileCategory : uint8_t {
    Default = 0,
    Missile = 1,
    Bomb = 2,
    Grenade = 3,
    Rocket = 4,
    StaticBomb = 5,
    MptlRocket = 6,
    Laser = 7,
    LightSaber = 8,
    ClusterBomb = 9,
};

struct ProjectileDesc {
    std::string name;
    float max_speed = 0.0f;                // Max_Speed, per frame
    float damage = 0.0f;                   // Projectile_Damage
    uint16_t damage_type = kNoTypeId;      // Damage_Type -> DamageArmorTable row
    uint8_t damage_kind = kKindNormal;     // tDamageType passed to Take_Damage (pack +0x70)
    bool does_shield_damage = true;        // Projectile_Does_Shield_Damage
    bool does_hitpoint_damage = true;      // Projectile_Does_Hitpoint_Damage
    bool homing = false;                   // Projectile_Category 1 [L: re-aims at the target each step]
    bool lobbed = false;                   // Projectile_Category 3 (range = Max_Speed^2 / (gravity x lob mod))
    // S8 t04 (docs/re/LOB_SOLVER.md §1.1): G = Projectile_Grenade_Gravity (+0x1e80) x Projectile_Grenade_Gravity_Lob_Mod
    // (+0x1e84); constructor defaults 1.05 / 5.02, an XML 0 is kept (G = 0). Category-3 shots (`lobbed`) launch on the
    // lob solver's arc (formulas.hpp lob_solve; hardpoint shots straight at the aim point) and fly lob_position.
    float lob_mod = 5.02f;
    float grenade_gravity = 1.05f;
    bool can_lob_slower = false;           // Projectile_Grenade_Can_Lob_Slower (+0x4a): the 0.8 speed steps
    bool sticks_on_collision = false;      // Projectile_Grenade_Sticks_On_Collision (+0x4b): attach / rest, no damage
    // S8 t06: Friendly_Damage_Amount (+0x23ec). > 0: the sweep also tests the own player (TACTICAL_COMBAT §3.4 step 3),
    // and DETONATE_REMOTE_BOMB detonates such bombs, damaging an own parent by it (LOB_SOLVER §3.5).
    float friendly_damage_amount = 0.0f;
    float max_flight_distance = 0.0f;      // Projectile_Max_Flight_Distance
    float max_lifetime = 0.0f;             // Projectile_Max_Lifetime (seconds)
    float blast_area_damage = 0.0f;        // Projectile_Blast_Area_Damage
    float blast_area_range = 0.0f;         // Projectile_Blast_Area_Range
    int32_t blast_max_victims = 0;         // Projectile_Blast_Area_Max_Victims (0 = no cap)
    bool blast_dropoff = false;            // Dropoff
    int32_t blast_dropoff_tiers = 0;
    float damage_delay_secs = 0.0f;        // Projectile_Damage_Delay_Secs
    bool damages_random_hardpoints = false;  // Projectile_Damages_Random_Hard_Points
    // ---- S9 ----
    ProjectileCategory category = ProjectileCategory::Default;  // flight dispatch + missile-shield test
    // Ion stun on detonation (behaviour 0x45, ABILITIES_HARDPOINTS.md 4.4) [H].
    bool ion_stun = false;                  // Projectile_Ion_Stun_On_Detonation
    float ion_stun_duration = 0.0f;         // seconds
    float ion_stun_speed_reduction = 0.0f;  // list 5 entry = -value
    float ion_stun_shot_rate_reduction = 0.0f;  // list 6 entry = -value
    bool ion_stun_stack_duration = false;
    float ion_stun_radius = 0.0f;           // 0 = direct victim only
    // Energy pool (4.7) [H]
    float energy_per_shot = 0.0f;           // Projectile_Energy_Per_Shot
    bool does_energy_damage = false;        // Projectile_Does_Energy_Damage
    // Saber/redirect listeners (TACTICAL_COMBAT 3.8) [M]
    float redirect_chance_modifier = 1.0f;  // Projectile_Redirect_Chance_Modifier
    float block_chance_modifier = 1.0f;     // Projectile_Block_Chance_Modifier
    // S9 t05 (S9_DEATH_CLONE_SHIELDS.md §2.3): Max_Rate_Of_Turn (type +0x38c, type default 2.3) in degrees per flight
    // step, x the mode's Object_Max_Speed_Multiplier (CombatConfig). Homing steering and the missile-shield turn are
    // limited by it; 0 (vanilla lasers) = never turned.
    float max_rate_of_turn = 2.3f;
    // S9 t04 (§1.5): Causes_Infection (+0xa7): a hit attaches / resumes the victim's INFECTION behaviour (kUnitInfected).
    bool causes_infection = false;
    // ---- OI abilities: detonation effects 0x4cb070 (TACTICAL_GAPS §1.10). Category masks: 0 = every category [M]. ----
    bool stun = false;                       // Projectile_Stun_On_Detonation (land only)
    int32_t stun_frames = 0;                 // Projectile_Stun_Duration_Frames
    float stun_radius = 0.0f;                // Projectile_Stun_Radius
    uint64_t stun_mask = 0;                  // Projectile_Stun_Victims_Category_Mask
    std::vector<int32_t> stun_types;         // Projectile_Stun_Victims_Unit_Types (either passes)
    bool distract = false;                   // Projectile_Distract_On_Detonation
    float distract_radius = 0.0f;
    uint64_t distract_mask = 0;
    bool heal = false;                       // Projectile_Instant_Heal_On_Detonation
    float heal_radius = 0.0f;
    uint64_t heal_mask = 0;
    int32_t heal_amount = 0;                 // Projectile_Instant_Heal_Health_Increase (int)
    bool convert = false;                    // Projectile_Convert_Enemy_On_Detonation
    float convert_radius = 0.0f;
    uint64_t convert_mask = 0;
    bool invulnerability = false;            // Projectile_Cause_Invulnerability_On_Detonation
    int32_t invulnerability_frames = 0;
    float invulnerability_radius = 0.0f;
    int32_t invulnerability_max_targets = 0;
    uint64_t invulnerability_mask = 0;
    bool weaken = false;                     // Projectile_Weaken_Enemy_On_Detonation
    float weaken_seconds = 0.0f, weaken_radius = 0.0f, weaken_take_increase = 0.0f, weaken_cause_reduction = 0.0f;
    uint64_t weaken_mask = 0;
    uint8_t combat_mod = 0;                  // Projectile_Combat_Mod: ModList index + 1 (code 0x52 enum), 0 none
    float combat_mod_seconds = 0.0f, combat_mod_value = 0.0f;
    uint64_t combat_mod_mask = 0;
    // ---- refixf t01: ROCKET / MPTL_ROCKET spline flight (S9_DEATH_CLONE_SHIELDS.md §2.4, rocket.hpp) ----
    // The three distances: type +0x1fec / +0x1ff0 / +0x1ff4 (schema "int", read by the float converter 0x8); floats here.
    float rocket_straight_distance = 0.0f;  // Projectile_Rocket_Straight_Distance (space initial plan)
    // Projectile_Rocket_Curve_Distance. Every ROCKET / MPTL_ROCKET flies the spline; 0 (the XML default) builds no legs.
    float rocket_curve_distance = 0.0f;
    float rocket_curve_offset = 0.0f;              // Projectile_Rocket_Curve_Offset
    bool target_point_on_terrain = false;          // Projectile_Target_Point_On_Terrain (type +0x7f)
    bool explode_when_reached_target_radius = false;  // Explode_When_Reached_Target_Radius (type +0x7e)
};

struct InaccuracyEntry {
    uint64_t category_mask = 0;  // Fire_Inaccuracy_Distance category
    float distance = 0.0f;
};
struct WeaponDelayEntry {
    uint8_t ability_type = 0;  // per-mode delay table (+0x250 + ability_type x 4)
    float factor = 1.0f;
};

struct HardpointDesc {
    std::string name;
    uint32_t name_crc = 0;  // name_crc(name)
    std::string collision_mesh;  // refixf t07: Collision_Mesh (type +0xa0), upper case; "" = none (§2.9 Q1 step 4)
    HardpointKind kind = HardpointKind::DummyArt;
    float health = 0.0f;
    bool destroyable = false, targetable = false, turret = false;  // ctor defaults false [M]
    Vec3 attach_offset;  // attachment bone, unit-local (x forward, z up)
    Vec3 bone_a, bone_b;  // Fire_Bone_A / Fire_Bone_B, unit-local
    bool has_bone_b = false;
    bool randomize_between_fire_bones = true;  // default true [M]
    float fire_yaw_deg = 0.0f;                  // fire direction relative to the unit's forward [L]
    // Struct default unrestricted [L] (synthetic tables); the XML catalog uses the type ctor default 0 (TACTICAL_COMBAT
    // §2.9 Q2 step 4).
    float fire_cone_width = 360.0f, fire_cone_height = 180.0f;
    // refixf t06 (TACTICAL_COMBAT §2.9 Q2 step 5): bind-pose orientation of Fire_Bone_A (fire_*) and of the attachment
    // bone (attach_*) from the unit model, as unit-local unit axes (x forward, y left, z up). The cone test reads the
    // point in that frame; without a model frame it uses the unit facing + fire_yaw_deg stand-in [port].
    bool has_fire_frame = false, has_attach_frame = false;
    Vec3 fire_x, fire_y, fire_z;
    Vec3 attach_x, attach_y, attach_z;
    float turret_rotate_extent = 180.0f;
    int32_t projectile = -1;           // Fire_Projectile_Type
    // OI abilities (REMAINING_GAPS §5.5): Blast_Ability_Fire_Projectile_Type (+0x220), fired instead of `projectile` while
    // the owner's Blast is charged (CombatHooks::fire_gate); -1 none.
    int32_t blast_projectile = -1;
    float projectile_damage = 0.0f;    // > 0 overrides the projectile's
    uint16_t damage_type = kNoTypeId;  // Damage_Type override
    float min_recharge = 0.0f, max_recharge = 0.0f;  // Fire_Min/Max_Recharge_Seconds
    int32_t pulse_count = 1;                          // Fire_Pulse_Count
    float pulse_delay = 0.0f;                         // Fire_Pulse_Delay_Seconds
    float fire_range = 0.0f, fire_min_range = 0.0f;   // Fire_Range_Distance / Fire_Min_Range_Distance
    std::vector<InaccuracyEntry> inaccuracy;          // first entry whose mask hits the target wins
    uint64_t fire_category_restrictions = 0;
    std::vector<WeaponDelayEntry> weapon_delay;
    // S9: opportunity fire flags (type bytes 0x3d5 / 0x3d6, constructor default true) [H].
    bool allow_opportunity_fire_when_idle = true;
    bool allow_opportunity_fire_when_targeting = true;
    // Death_Explosion_Particles / Death_Breakoff_Prop object types (host TypeIndex, -1 none):
    // reported in HardpointEvent so the host spawns them (they consume object ids).
    int32_t death_explosion_type = -1;
    int32_t death_breakoff_type = -1;
    // OI abilities: hardpoint repair (command 45, TACTICAL_GAPS §1.12): per repairer per frame.
    float repair_cost_per_frame = 0.0f;    // Repair_Cost_Per_Frame (hp type +0x470)
    float repair_amount_per_frame = 3.0f;  // Repair_Amount_Per_Frame (+0x474; ctor default 3.0, TACTICAL_COMBAT 5.1)
    // Allows_Special_Weapon_Use (type +0x3d8, HARDPOINT_DISABLED.md §4): every Set_System_Online of this hardpoint
    // writes the allied players' special-weapon flag (CombatSystem::special_weapon_hp_enabled).
    bool allows_special_weapon_use = false;
    // ENABLE_SPECIAL_ABILITY (11) hardpoints: Special_Ability_Name (type +0x450) as name_crc, 0 = empty (no effect).
    uint32_t special_ability_name_crc = 0;
    // FIGHTER_BAY (3) hardpoints: Fighter_Bay_Flyout_Distance (the hangar module's initial move order). Not read by combat.
    float fighter_bay_flyout_distance = 0.0f;
    // S8 t08 (TACTICAL_COMBAT §3.1.1, Does_Weapon_State_Allow_Firing_At 0x384850 steps 4-6): Fire_When_Deployed /
    // _Undeployed / _In_Rocket_Attack_Mode / _In_Normal_Attack_Mode (type bytes +0x3d1..+0x3d4, ctor default true), and
    // the per-mode bytes +0x384 + m (Fire_When_In_<Mode>_Mode, m = UnitAbilityType 1..0x4c) as the list of the modes whose
    // byte is No [M: ctor default true].
    bool fire_when_deployed = true, fire_when_undeployed = true;
    bool fire_when_rocket_attack = true, fire_when_normal_attack = true;
    std::vector<uint8_t> no_fire_modes;
};

// Death_Clone entry (S9_DEATH_CLONE_SHIELDS.md §1.4): key -> clone object type (host TypeIndex). The key is the
// tDamageType value of the XML name, else 0x18 + the name's index in GameConstants Damage_Types. A repeated key
// appends to that key's list (list order = entry order here).
struct DeathCloneEntry {
    int32_t key = 0;
    int32_t type = -1;
};
inline constexpr int32_t kDeathCloneSubKeyBase = 0x18;

// refix t02: the Infection_Ability parameters (xml_field_tables "fields of Infection_Ability objects"; vanilla IG-88:
// Distance_To_Infect 70, Seconds_To_Infect 2, Duration_Of_Infection 30, Damage_Per_Second 2, Infection_CategoryMask
// Infantry). An INFECTION behaviour started by a Causes_Infection hit of a unit whose type has them (and every behaviour
// spread from it) runs with them; `has` false = none (the behaviour then only marks the unit infected).
struct InfectionDesc {
    bool has = false;
    float distance_to_infect = 0.0f;   // Distance_To_Infect
    float seconds_to_infect = 0.0f;    // Seconds_To_Infect: consecutive time in range before a neighbour is infected
    float duration_secs = 0.0f;        // Duration_Of_Infection (<= 0: no expiry)
    float damage_per_second = 0.0f;    // Damage_Per_Second
    uint64_t category_mask = 0;        // Infection_CategoryMask (0 = every category [M])
};

struct UnitDesc {
    std::string name;
    int32_t object_type = -1;  // host TypeIndex (priority-set Type entries)
    float tactical_health = 0.0f;
    float shield_points = 0.0f, shield_refresh_rate = 0.0f;
    uint16_t armor_type = kNoTypeId, shield_armor_type = kNoTypeId;
    float radius = 1.0f;  // target radius used by the range gates and max travel
    float max_attack_distance = 0.0f;           // Targeting_Max_Attack_Distance
    // OI tactical (TACTICAL_GAPS 3.1): Targeting_Min_Attack_Distance (slot 45) and Custom_Hard_X/YExtent x Scale_Factor
    // (slot 43 footprint E; 0 = the radius).
    float min_attack_distance = 0.0f;
    float hard_x_extent = 0.0f, hard_y_extent = 0.0f;
    // OI tactical (TACTICAL_GAPS 3.1 slot 43): the object is a LAND_OBSTACLE (obj +0x346 != 0xFF). Range tests then use
    // the obstacle-cell aim walk (set_obstacle_cell_query) instead of the footprint half-extent.
    bool land_obstacle = false;
    float fire_inaccuracy_fixed_radius = 0.0f;  // Targeting_Fire_Inaccuracy_Fixed_Radius
    int32_t priority_set = -1;                  // Targeting_Priority_Set (index into CombatData)
    float stickiness_threshold = 0.0f;          // Targeting_Stickiness_Time_Threshold (seconds)
    uint64_t category_mask = 0, property_flags = 0, attack_category_restrictions = 0;
    uint32_t space_layer = 0;  // Space_Layer (short-range attackers only)
    bool target_stealth_units = false;
    bool is_valid_target = true;
    bool destroy_when_all_hardpoints_destroyed = false;  // type +0xa8
    bool immune_to_damage = false;
    bool redirect_damage_to_teammates = false, share_damage_with_teammates = false;
    std::vector<uint32_t> hardpoints;  // HardpointDesc indices, HardPoints list order
    // Non-hardpoint weapon (WeaponBehavior): Projectile_Types / Projectile_Fire_* fields.
    int32_t weapon_projectile = -1;
    // OI abilities (SWAP_WEAPONS / Permanent_Weapon_Swap): the type's Projectile_Types list (ProjectileDesc indices,
    // list order; type +0xc48). The object's weapon index obj +0x324 picks the entry the weapon fires (L1_S4_REMAINING
    // §2.1 Get_Current_Projectile_Type, TACTICAL_GAPS §3.1 slot 44). Empty: `weapon_projectile` only (index ignored).
    std::vector<int32_t> weapon_projectiles;
    // S8 t08: Projectile_Types[0] (ProjectileDesc index, -1 none) whether or not the type has the WEAPON behaviour - not
    // fired by combat; read by the host's special weapons (SPECIAL_WEAPON / lobbing superweapons / TSW take their shot
    // from the list without a WEAPON behaviour).
    int32_t listed_projectile = -1;
    float weapon_recharge = 0.0f;
    int32_t weapon_pulse_count = 1;
    float weapon_pulse_delay = 0.0f;
    // S8 t08 (TACTICAL_COMBAT §3.1.1, own-weapon gate 0x54c910): Fire_Weapon_When_Deployed / _Undeployed /
    // _In_Rocket_Attack_Mode / _In_Normal_Attack_Mode (type +0xae..+0xb1, default true).
    bool fire_weapon_when_deployed = true, fire_weapon_when_undeployed = true;
    bool fire_weapon_when_rocket_attack = true, fire_weapon_when_normal_attack = true;
    // S8 t06 B4 (LOB_SOLVER §5.1): the WeaponBehavior fire bones <Muzzle_Bone_Name>_00, _01, ... (default MUZZLEA_NN) as
    // rest-pose offsets in unit-local space (x forward, y left, z up; turned by the unit's facing), list order. The host
    // fills them from the unit's model (battle host: BattleCatalog::muzzle_bones). `muzzle_model` false = no model data:
    // [port] the shot leaves from the unit's position (grenades: + its own Ranged_Target_Z_Adjust) as before. Model known
    // and no bone: the weapon never fires.
    bool muzzle_model = false;
    std::vector<Vec3> muzzle_bones;
    std::array<int32_t, 2> abilities{-1, -1};  // first two Unit_Abilities_Data slots
    uint8_t hit_particle_count = 0, shield_hit_particle_count = 0;  // list sizes (synced pick draws)
    // ---- S9 (ABILITIES_HARDPOINTS.md 1.3, 4.3, 4.7-4.10) ----
    bool combustible = false;               // Is_Combustible: kind-7 hit ignites instead of damaging
    float burning_damage_per_second = 0.0f; // Burning_Damage_Per_Second
    float energy_capacity = 0.0f;           // Energy_Capacity (> 0 = EnergyPoolBehavior)
    float energy_refresh_rate = 0.0f;       // Energy_Refresh_Rate
    bool requires_base_power = false;       // Requires_Base_Power (energy 0 without base power)
    float passive_missile_shield_radius = 0.0f;  // Passive_Missile_Shield_Radius
    bool damaged_smoke = false;             // Damaged_Smoke_Asset_Name set: smoke events at 75 % / 50 %
    std::vector<DeathCloneEntry> death_clones;    // Death_Clone
    std::vector<int32_t> death_projectiles;       // Death_Projectiles (ProjectileDesc indices)
    float ranged_target_z_adjust = 0.0f;          // Ranged_Target_Z_Adjust
    float converted_die_time = 0.0f;              // Converted_To_Enemy_Die_Time_Seconds
    bool has_locomotor = true;                    // jamming victims need a locomotor
    bool deploys = false;                         // Deploys (type +0xd1): the death-clone deploy remap (S9 t04)
    InfectionDesc infection;                      // refix t02: this type's Infection_Ability (the infector's side)
    // HUD repair cursor (HUD_GAPS §1.4): the object has behaviour 0x5C DUMMY_STAR_BASE (behaviour slot obj +0x38e =
    // +0x332 + 0x5C, OBJECT_RUNTIME §4.3). Static type data; read by can_repair_hardpoints, not hashed.
    bool repairs_hardpoints = false;
    // ---- OI abilities (TACTICAL_GAPS §1.9, §1.10, §7.3) ----
    bool spin_away_on_death = false;              // Spin_Away_On_Death (+0x9a)
    float spin_away_chance = 0.0f;                // Spin_Away_On_Death_Chance (+0x2208)
    std::vector<std::pair<int32_t, float>> destruction_survivors;  // (host TypeIndex, cumulative weight), list order
    // Minimum / Maximum_Destruction_Survivor_Count; ctor defaults 1 / 1 [inferred: shipped one-survivor lists such as
    // Scout_Trooper_No_Bike give no count]
    int32_t min_survivors = 1, max_survivors = 1;
    float tactical_respawn_secs = 0.0f;           // Tactical_Respawn_Time_In_Secs (+0x21d0)
    bool stunnable = false;                       // has STUNNABLE (0x38)
    bool shield_requires_base_power = false;      // Shield_Requires_Base_Power (+0x8c)
};

struct CombatData {
    DamageArmorTable damage;
    std::vector<PrioritySet> priority_sets;
    std::vector<ProjectileDesc> projectiles;
    std::vector<HardpointDesc> hardpoints;
    std::vector<UnitAbilityDesc> abilities;
    std::vector<UnitDesc> units;
};

struct PlayerInfo {
    bool active = true;
    bool ai = false;
    float ai_damage_multiplier = 1.0f;  // DifficultyAdjustments Damage_Multiplier (AI attackers)
    float ai_accuracy = 1.0f;           // land inaccuracy factor input
    float faction_health = 1.0f;        // faction data +0x50
    float faction_shield = 1.0f;        // faction data +0x54
    // OI abilities: base power is no longer a player flag - see CombatSystem::add_base_power (TACTICAL_GAPS §7.3: a
    // per-player list of Base_Power_Ability entries; an empty list = no base power).
};

// OI abilities: effects of a detonation (0x4cb070) and land stuns the ability system / host react to
// (CombatHooks::unit_effect).
enum class UnitEffect : uint8_t {
    Stunned,      // STUNNABLE victim stunned (land stun path / Stun_Ability): stealth breaks (0.3 s)
    StunEnded,    // the stun end frame passed (release [inferred])
    Distracted,   // Projectile_Distract victim: order toward `point` [M]
    Converted,    // Projectile_Convert victim (or a team member of it): owner := `player`, converted flag set
    Healed,       // Projectile_Instant_Heal
    Invulnerable, // Projectile_Cause_Invulnerability (INVULNERABLE 0x4b for `frames`)
};

struct CombatConfig {
    GameMode mode = GameMode::Space;
    uint32_t fps = kDefaultFps;
    float object_max_health_multiplier = 1.5f;     // Object_Max_Health_Multiplier_Space (Land 1.0)
    float hardpoint_health_multiplier = 1.5f;      // 0x540070: Space 1.5, Land 1.0
    float low_health_threshold = 0.0f;             // 0x140b27338, value unknown [L]: 0 disables rule 1
    float short_range_cutoff = 0.0f;               // 0x140b27340, value unknown [L]
    float hull_vs_hardpoints_constraint = 0.2f;    // Hull_Vs_Hard_Points_Health_Constraint
    float depleted_shield_regen_cap = 0.25f;       // GameConstants +0x1183c (absolute points)
    float depleted_shield_disable_time = 5.0f;     // Depleted_Shield_Disable_Time
    float shield_recharge_interval_secs = 3.0f;    // ShieldRechargeIntervalInSecs
    float under_construction_damage_multiplier = 2.0f;
    float retreat_damage_mod_factor = 1.0f;        // Space/Land_Retreat_Units_Damaged_Mod_Factor
    float base_shield_vulnerability = 1.0f;        // Base_Shield_Vulnerability_Modifier
    float max_secs_for_ae_delayed_damage = 0.0f;   // Max_Secs_For_AE_Delayed_Damage
    // refixf t06 (TACTICAL_COMBAT §2.9 Q1 step 1, port difference 1) [port]: a space hit on a non-target keeps the render
    // mesh it struck; the port has no collision meshes, so the hit goes to the closest live hardpoint whose bone lies
    // within this distance of the impact point (a stand-in for the hardpoint's Collision_Mesh extent), else the hull.
    float hardpoint_mesh_radius = 40.0f;
    // Diminishing_Firepower curve as (seconds since last hit, factor) points (OI tactical, TACTICAL_GAPS 3.3: a
    // natural cubic spline through the points sorted by x, float; before the first / past the last point the end
    // value); empty = 1.0.
    std::vector<std::pair<float, float>> diminishing_firepower;
    bool weapons_enabled = true;
    bool damage_enabled = true;      // global flag: only kinds 0, 8, 13 pass when false
    bool inaccuracy_enabled = true;  // debug flag 0x140b3934d
    uint32_t grain = 64;             // parallel grain (results never depend on it)
    // ---- S9 ----
    // Opportunity fire (TACTICAL_COMBAT 2.6 with the ABILITIES_HARDPOINTS.md 4.2 correction: the
    // idle rule applies to every player). The S8 regression battle turns it off to keep its golden.
    bool opportunity_fire = true;
    float opportunity_recharge_cutoff = 3.0f;    // Hardpoint_Recharge_Cutoff_For_Opportunity_Fire
    float energy_recharge_interval_secs = 5.0f;  // EnergyRechargeIntervalInSecs
    float energy_to_shield_exchange_rate = 5.0f; // EnergyToShieldExchangeRate
    float sensor_jamming_time = 0.5f;            // Sensor_Jamming_Time
    bool death_clones = true;                    // tactical mode: Die spawns death clones (kind != 15)
    // S9 t05: Object_Max_Speed_Multiplier_Space / _Land (GC +0x15c0 / +0x15c4; FoC 1.2 / 1.56), the factor on a
    // projectile's Max_Rate_Of_Turn (0x372560). apply_game_constants reads both; the mode picks one.
    float turn_multiplier_space = 1.0f;
    float turn_multiplier_land = 1.0f;
};

// OI tactical (TACTICAL_GAPS 3.2 / 3.3): the GameConstants fields combat reads - Health_Low_Percent_Threshold
// (low_health_threshold), Melee_Cutoff_Range (short_range_cutoff), Hardpoint_Recharge_Cutoff_For_Opportunity_Fire,
// Diminishing_Firepower (flat number list read as (x, y) pairs). Absent tags keep the current values.
void apply_game_constants(CombatConfig& c, const data::GameConstants& gc);

// Hooks the ability system (S9, engine/tactical/abilities) installs into the combat tick. Every call
// is made from a serial phase, in the order given, so implementations may draw from the synced RNG
// and call back into CombatSystem (no unit is added or removed while a hook runs).
// refixf t07 (TACTICAL_COMBAT §3.4, HOTSPOTS 0x2abcf0, SPACE_AVOIDANCE §5): the per-entry test of the projectile
// segment query, supplied by the host that owns the models. Called from the sweep phase (worker threads: read-only).
struct PreciseQuery {
    ObjectId victim = kNoObject;
    Vec3 a, b;               // the swept segment a -> b (world)
    uint32_t mask = 5;       // collision mask: 7 with Projectile_Does_Shield_Damage, else 5
    bool shield_up = false;  // shield points > 0: the victim's SHIELD mesh carries mask 2 (else 0) [port: no disable state]
    bool collide = true;     // model collide switch (§5.5): off while garrisoned
};
struct PreciseResult {
    float t = 1.0f;                     // segment fraction of the hit
    const std::string* mesh = nullptr;  // the struck sub-object's name (upper case); null: none recorded
};
enum class PreciseOutcome : uint8_t {
    NotModelled,  // no model data for this victim: the tree-box hit stands (the pre-t07 rule) [port]
    Miss,
    Hit,
};
using PreciseHitFn = std::function<PreciseOutcome(const PreciseQuery&, PreciseResult&)>;

// S8 t01 (TACTICAL_GAPS 3.1 slot 43): answer of the host's obstacle-cell query for a LAND_OBSTACLE target.
enum class ObstacleCell : uint8_t {
    NoFootprint,  // the target occupies no passability cells (not stamped): the half-extent rule applies [port]
    Free,         // the cell containing the point is not one of the target's
    Occupied,     // the target is in that cell's hard-occupant list (0x59efa0)
};
// `query(target, p, centre)`: the cell containing p against `target`'s stamp; always writes that cell's centre (XY; z
// is free). Read-only: the targeting scan calls it from job workers.
using ObstacleCellFn = std::function<ObstacleCell(ObjectId target, Vec3 p, Vec3& centre)>;

class CombatHooks {
public:
    virtual ~CombatHooks() = default;
    // After phase A (modifier purge / caches), before upkeep and targeting.
    virtual void begin_tick(uint32_t /*frame*/) {}
    // Phase D start, before the first unit (non-combat objects such as team containers).
    virtual void before_units(uint32_t /*frame*/, SimRng& /*rng*/) {}
    // Phase D per unit (newest first): before targeting (ability countdown) ...
    virtual void unit_begin(ObjectId /*id*/, uint32_t /*frame*/, SimRng& /*rng*/) {}
    // ... after targeting, before the hardpoints fire (pending targeted ability, special abilities).
    virtual void unit_after_targeting(ObjectId /*id*/, uint32_t /*frame*/, SimRng& /*rng*/) {}
    // Fire gate (0x397e00 / 0x394eb0): may swap the projectile (ion/lucky override) or forbid the
    // shot (a unit with ION_CANNON_SHOT / LUCKY_SHOT active fires only at its ability target).
    virtual bool shot_allowed(ObjectId /*shooter*/, ObjectId /*target*/, int32_t /*target_hp*/, int32_t& /*projectile*/) {
        return true;
    }
    // OI abilities (REMAINING_GAPS §5.5): the special-weapon state gate of Fire_At (0x3825b0 steps 1 and 4: the Blast
    // counter, SAP pack +0x394). `hardpoint` = the firing hard point (nullptr: the unit's own weapon). False refuses the
    // shot; may replace `projectile` (Blast_Ability_Fire_Projectile_Type) and set `damage_scale` (the shot's damage =
    // (hard point damage > 0 ? it : projectile damage) x damage_scale). Called before shot_allowed; no draws.
    virtual bool fire_gate(ObjectId /*shooter*/, const HardpointDesc* /*hardpoint*/, int32_t& /*projectile*/,
                           float& /*damage_scale*/) {
        return true;
    }
    // ... after the shot spawned (before shot_fired), same `hardpoint`: the Blast counts its charged shots.
    virtual void fire_done(ObjectId /*shooter*/, const HardpointDesc* /*hardpoint*/, uint32_t /*frame*/) {}
    // On_Shot_Fired (0x3a06a0): ion one-volley, stealth break.
    virtual void shot_fired(ObjectId /*shooter*/, ObjectId /*target*/, int32_t /*target_hp*/, int32_t /*projectile*/,
                            uint32_t /*frame*/) {}
    // S10-int (R3 hookup, host): world positions of hardpoint `hardpoint`'s fire bones A / B from the animated
    // pose (ANIM_PARTICLES 0.1: projectile spawn points read animated bones). Return true after writing both
    // (b = a when the hardpoint has no bone B); false keeps the HardpointDesc offsets (no draw either way).
    virtual bool fire_bones(ObjectId /*shooter*/, uint32_t /*hardpoint*/, Vec3& /*a*/, Vec3& /*b*/) { return false; }
    // Take_Damage step 11 (lucky shot): returns the extra CAUSE_DAMAGE factor (1 = none) and may move
    // a hull hit onto a hardpoint.
    virtual float lucky_shot(ObjectId /*source*/, ObjectId /*victim*/, int32_t& /*hardpoint*/, Vec3 /*impact*/,
                             bool /*has_impact*/) {
        return 1.0f;
    }
    // Take_Damage step 13 (style Take_Damage special abilities): true cancels the damage. `rng` is
    // null outside a tick unless bind_rng() was called; listeners that need draws then do nothing.
    virtual bool damage_listener(ObjectId /*victim*/, ObjectId /*source*/, int32_t /*projectile_desc*/, uint32_t /*frame*/,
                                 SimRng* /*rng*/) {
        return false;
    }
    // Set_System_Online(hp, offline) (0x384d00), after the combat-side offline effects. `destroyed` = the hardpoint
    // was shot down (else disabled by the station-upgrade copy, HARDPOINT_DISABLED.md §3). `last_of_kind` = no
    // functioning hardpoint of the kind remains (Count_Functioning_Hardpoints 0x397c10). The default forwards to
    // hardpoint_destroyed (hosts that predate the distinction treat both the same).
    virtual void hardpoint_offline(ObjectId owner, uint32_t index, HardpointKind kind, bool last_of_kind, bool destroyed,
                                   uint32_t frame) {
        (void)destroyed;
        hardpoint_destroyed(owner, index, kind, last_of_kind, frame);
    }
    // Set_System_Online(hp, online) (0x384d00, HARDPOINT_DISABLED.md §9): a hardpoint was repaired to max health
    // (the only online caller), after the combat-side online effects (shield regen block cleared). No live-count
    // check: any one hardpoint of the kind runs it.
    virtual void hardpoint_online(ObjectId /*owner*/, uint32_t /*index*/, HardpointKind /*kind*/, uint32_t /*frame*/) {}
    // Legacy offline hook (see hardpoint_offline).
    virtual void hardpoint_destroyed(ObjectId /*owner*/, uint32_t /*index*/, HardpointKind /*kind*/, bool /*last_of_kind*/,
                                     uint32_t /*frame*/) {}
    // Die step 2 (before the death signal): emitter removal, ability effects.
    virtual void unit_dying(ObjectId /*id*/, uint32_t /*frame*/) {}
    // Ion stun applied (DEFEND is switched off).
    virtual void ion_stunned(ObjectId /*id*/, uint32_t /*frame*/) {}
    // After dead units were compacted away (end of tick, remove_unit).
    virtual void units_removed() {}
    // Fog of war (FOG_OF_WAR.md §5.2 FOW_Object_Hidden for `player`, with `force`): true = `target` is hidden. The
    // host fills it from its FogOfWar (the battle: TacticalBattle::fog_object_hidden). Unlike the hooks above this
    // query is ALSO called from the PARALLEL gather phase C (targeting filter step 4, TACTICAL_COMBAT 2.3, force 1
    // since OI tactical - TACTICAL_GAPS 5.8: AI shooters too) and from const queries (special-weapon can-fire,
    // force 1): it must be read-only and thread-safe. Default: nothing is hidden (no fog).
    virtual bool target_hidden(int32_t /*player*/, ObjectId /*target*/, bool /*force*/) const { return false; }
    // L1 systems (L1_S4_REMAINING.md §3.3): the victim's ion-storm lockout - its shields absorb nothing (0x549ac0).
    // Called from Take_Damage (serial phases / host calls). Default: never.
    virtual bool ion_storm_shields_down(ObjectId /*victim*/, uint32_t /*frame*/) const { return false; }
    // OI abilities: a detonation effect / stun on `id` (serial phases, after the combat-side state changed).
    virtual void unit_effect(ObjectId /*id*/, UnitEffect /*effect*/, int32_t /*player*/, Vec3 /*point*/, uint32_t /*frame*/) {}
};

// Unit flags (subset of the original's +0x3a0..+0x3b4 bytes).
enum UnitFlag : uint32_t {
    kUnitDead = 1u << 0,
    kUnitInvulnerable = 1u << 1,       // +0x37d: only kinds 0 and 13 pass
    kUnitCannotDie = 1u << 2,          // +0x3a1 bit 0x80
    kUnitStealthed = 1u << 3,          // 0x39b140
    kUnitUnderConstruction = 1u << 4,
    kUnitRetreating = 1u << 5,
    kUnitInBaseShield = 1u << 6,
    kUnitSkipTargeting = 1u << 7,      // +0x3a1 bit 0x10 (candidate skip; CAUSE multiplier for non-projectile sources)
    kUnitGarrisoned = 1u << 8,
    kUnitConverted = 1u << 9,          // obj +0x3a2 bit 4: converted-unit decay (4.10)
    kUnitLocomotorBusy = 1u << 10,     // locomotor vfunc +0x2b0 (hyperspace jump ...): no ion stun
    // S9 t04 (S9_DEATH_CLONE_SHIELDS.md §1.2): deploy pending (locomotor pack +0x2b3) or Deployed (walk state 0x19).
    // Set by the ability system's DEPLOY [port: on = deploying / deployed]. Read by the death-clone remap only.
    kUnitDeploying = 1u << 11,
    // S9 t04 (§1.5): an active INFECTION behaviour (0x54, suspend counter 0). refix t02: mirrors the behaviour state
    // (attached by a Causes_Infection hit or a spread, cleared while suspended); set_flag(kUnitInfected) attaches /
    // suspends it like the behaviour calls. Read by the death-clone remap.
    kUnitInfected = 1u << 12,
    // S7 t14 (MOVEMENT §3.1.1 step 6): TARGETING / WEAPON / SPECIAL_WEAPON suspended (Disable, suspend count +1) while
    // the unit is on its jetpack (the host mirrors MovementSystem's UnitView::secondary_locomotor). The unit neither
    // acquires targets nor fires; orders and targets are kept. S7 t15 A2: the WeaponBehavior recharge keeps elapsing
    // (the first service after the suspension subtracts every frame since the last one). [port] a flag stands for the
    // counter: the swap disables once (never twice) and the swap back enables (count - 1, floor 0).
    kUnitWeaponsSuspended = 1u << 13,
    // S8 t08 (TACTICAL_COMBAT §3.1.1): the walk locomotor is in a deploy transition (state 0x18 Deploying or 0x1A
    // Undeploying, 0x39a400 / 0x39b2a0) - a Deploys unit with a locomotor neither fires its own weapon nor counts its
    // weapon hardpoints down. Set by the host [port: the port's DEPLOY is instant, so nothing sets it yet].
    kUnitDeployTransition = 1u << 14,
};

struct UnitSpawn {
    ObjectId id = kNoObject;
    uint32_t desc = 0;
    int32_t owner = -1;
    Vec3 position;
    Vec3 facing{1.0f, 0.0f, 0.0f};
    uint32_t team = 0;              // squad / team container (0 = none)
    bool team_leader = false;       // Mod_Multiplier squad-minimum rule
    ObjectId damage_parent = kNoObject;  // parent container that takes this unit's damage [M]
    float health_fraction = 1.0f;
    uint32_t flags = 0;
};

struct DamageRequest {
    ObjectId victim = kNoObject;
    float amount = 0.0f;
    uint16_t damage_type = kNoTypeId;
    uint8_t kind = kKindNormal;
    ObjectId source = kNoObject;
    bool source_is_projectile = false;
    int32_t projectile_desc = -1;
    int32_t hardpoint = -1;  // index within the victim's hardpoints (the "hardpoint name")
    bool no_team = false;
    bool has_impact = false;  // impact point known (projectile hits): lucky-shot hull re-route
    Vec3 impact;
};

struct DamageResult {
    bool applied = false;     // passed the gates
    float after_multipliers = 0.0f;  // amount after steps 4-12
    float absorbed = 0.0f;    // shield points removed (shield-armor space)
    float to_hardpoint = 0.0f;
    float to_hull = 0.0f;     // hull points removed (hull-armor space)
    float hull_armor_factor = 1.0f;
    bool killed = false;
    bool cancelled = false;  // a Take_Damage listener (redirect / absorb) cancelled the hit
    float energy_absorbed = 0.0f;
};

struct DeathEvent {
    ObjectId id = kNoObject;
    ObjectId killer = kNoObject;
    uint8_t kind = kKindNormal;  // damage kind of the killing hit
    // Death_Clone type to spawn (host; consumes an id), -1 none (kind 15, death clones off). S9 t04: when no list
    // matched this is the dying unit's own UnitDesc::object_type (0x29f270 clones the dying type).
    int32_t death_clone = -1;
    // refix t03: the final death-clone key K of Die step 8 (after the remap and the existence check), -1 when no clone.
    // The host's infantry corpse path (S9 §1.6) reads it (K != 4).
    int32_t clone_key = -1;
    int32_t death_projectile = -1;  // Death_Projectiles entry fired (ProjectileDesc index), -1 none
    bool spin_away = false;      // OI abilities: DeathBehavior Initialise spin-away decision (TACTICAL_GAPS §1.9)
};

// OI abilities: Die step 7 world removal (0x391310, TACTICAL_GAPS §1.9): objects the host creates for a death, in
// order (each consumes an id). respawn: Tactical_Respawn_Time_In_Secs (same type, created at `due_frame`); otherwise
// a Destruction_Survivors pick (owner rule 0x3751a0: the dying owner if its faction may own the type, else the first
// player in list order whose faction can - the host decides; `owner` is the dying object's owner).
struct SurvivorEvent {
    ObjectId dead = kNoObject;
    int32_t type = -1;  // host TypeIndex
    int32_t owner = -1;
    Vec3 position, facing;
    bool respawn = false;
    uint32_t due_frame = 0;
};

// A hardpoint was destroyed (in event order; the host spawns Death_Explosion_Particles, then
// Death_Breakoff_Prop for live destruction, each consuming an object id; render applies B.10).
struct HardpointEvent {
    ObjectId owner = kNoObject;
    uint32_t index = 0;  // HardPoints list index
    HardpointKind kind = HardpointKind::DummyArt;
    bool owner_dying = false;    // killed by the owner's Die (still "live destruction" in vanilla)
    bool last_of_kind = false;   // last live hardpoint of this kind on the owner
    int32_t explosion_type = -1, breakoff_type = -1;
};

// One applied Take_Damage call (for the tactical AI's damage tracking). Filled in call order inside
// the serial phases (delayed damage, unit updates, projectile commit) and by host calls; cleared at
// the start of each tick. Hits stopped by the gates or cancelled by listeners are not reported.
struct DamageEvent {
    ObjectId victim = kNoObject;  // after parent / team redirection (one event per damaged unit)
    ObjectId source = kNoObject;
    int32_t source_owner = -1;    // -1 when the source is unknown or already removed
    bool source_is_projectile = false;
    int32_t projectile_desc = -1;
    uint8_t kind = kKindNormal;
    int32_t hardpoint = -1;       // hardpoint that took the damage (-1 = hull)
    float after_multipliers = 0.0f;
    float absorbed = 0.0f, energy_absorbed = 0.0f, to_hardpoint = 0.0f, to_hull = 0.0f;
    bool killed = false;
    uint32_t frame = 0;
};

// S10-int (R4 hookup): one projectile detonation, for the host's PARTICLE objects (TACTICAL_COMBAT 3.3 expiry,
// 3.5 step 4 visuals). Filled in the serial commit phase (projectile serial order); cleared each tick.
struct HitEvent {
    ObjectId victim = kNoObject;   // kNoObject: lifetime / distance expiry (no victim)
    ObjectId shooter = kNoObject;
    int32_t owner = -1;
    uint32_t projectile_desc = 0;
    Vec3 position;                 // impact point (expiry: the projectile's position)
    bool absorbed = false;         // absorbed by shields (shield-hit list, Absorbed_By_Shields particle)
    bool armor_reduced = false;    // hull armor factor <= 0.75 (armor-reduced detonation particle)
    bool delayed = false;          // Projectile_Damage_Delay_Secs: damage queued, absorption unknown [port]
    int32_t pick = -1;             // the synced pick from the victim's (shield-)hit-particle list, -1 none
};

// OI fog: one shot that reached the post-shot hook (On_Shot_Fired 0x3a06a0, from HP_Fire_At and WeaponBehavior
// vf6), in fire order inside the serial unit phase; cleared each tick. The fog flashes the shooter's footprint on
// the target owner's grid (REVEAL vf26, TACTICAL_GAPS 5.1). Not part of any hash.
struct ShotEvent {
    ObjectId shooter = kNoObject;
    ObjectId target = kNoObject;
    int32_t target_owner = -1;  // at fire time
};

// Damaged_Smoke_Asset_Name attachment (0x3ab890): hull crossed 75 % or 50 % downward in one hit.
struct SmokeEvent {
    ObjectId id = kNoObject;
    uint8_t percent = 75;
};

struct ProjectileView {
    uint32_t serial = 0;
    ObjectId shooter = kNoObject, target = kNoObject;
    int32_t target_hp = -1;
    Vec3 pos, dir;
    float damage = 0.0f, max_travel = 0.0f;
    // S9
    uint32_t desc = 0;
    int32_t owner = -1;
    float travelled = 0.0f;
    ProjectileCategory category = ProjectileCategory::Default;
    bool alive = true;
    // Presentation (render snapshot, read-only): current speed per frame (pos advances by dir x speed each flight step;
    // 0 for an armed bomb) and the spawn frame.
    float speed = 0.0f;
    uint32_t spawn_frame = 0;
    // S8 t04: a grenade (category 3) flying the closed-form parabola (pos / dir are the last step's point and chord;
    // speed = the launch speed s), and a sticky grenade that stopped: attached to `stuck_to` (kNoObject: resting).
    bool lob = false;
    bool stuck = false;
    ObjectId stuck_to = kNoObject;
};

// refixf t01: a spline rocket's flight state (projectile data pack fields of S9_DEATH_CLONE_SHIELDS.md §2.4.1).
struct RocketView {
    bool planned = false;      // +0x30 set (the initial plan ran)
    float s = 0.0f;            // +0x38 RocketDistanceAlongSpline
    float budget = 0.0f;       // +0x54 RocketMaxFlightDistance
    Vec3 fire_at;              // +0x3c FireAtPosition
    Vec3 launch;               // +0x80 launch point
    Vec3 last_velocity;        // +0x98 LastKnownVelocity
    const RocketSpline* path = nullptr;
};

// L1 systems: a projectile launched by a host system that is not a combat weapon (special weapons, lobbing
// superweapons, bombardment / bombing-run shells; engine/tactical/abilities special_weapons.hpp). [port] No
// draws: no muzzle / inaccuracy / recharge (those belong to the launching system); flies straight from `from`.
struct ProjectileLaunch {
    ObjectId shooter = kNoObject;  // the launching object (need not be a combat unit; damage source / killer)
    int32_t owner = -1;            // projectile owner (hostility for the sweep, area damage)
    uint32_t projectile = 0;       // ProjectileDesc index
    Vec3 from;
    ObjectId target = kNoObject;   // a combat unit: aimed at its position with linear lead; kNoObject: `point`
    int32_t target_hp = -1;        // hardpoint of the target (-1 = none / the projectile's own routing)
    Vec3 point;                    // ground / space point when target == kNoObject (detonates there on expiry)
    float damage = -1.0f;          // < 0: the ProjectileDesc's
    // OI abilities: a BOMB / grenade timer (Bomb_Countdown_Seconds, Grenade_Explode_Timer): >= 0 makes the shot sit
    // at `from` and detonate when the fuse ran out (lifetime expiry, area damage + detonation effects) [port: no
    // flight for armed bombs]. < 0: normal flight.
    float fuse_seconds = -1.0f;
    // S8 t04 (LOB_SOLVER §5, abilities Grenade_Attack / Remote_Bomb): the BOMB timer runs while the shot flies instead of
    // placing it at `from` - a category-3 projectile is tossed on the lob solver's arc (launch_projectile's grenade
    // branch) and `fuse_seconds` >= 0 sets it off wherever it is (in flight, stuck or resting). Without `toss` a
    // category-3 launch with a fuse is still placed; one without a fuse always takes the grenade branch.
    bool toss = false;
};

struct CombatStats {  // diagnostics (not hashed)
    uint64_t scans = 0, shots = 0, hits = 0, expired = 0, deaths = 0, resweeps = 0;
};

class CombatSystem {
public:
    CombatSystem(const CombatData& data, CombatConfig config, std::vector<PlayerInfo> players);
    CombatSystem(const CombatSystem&) = delete;
    CombatSystem& operator=(const CombatSystem&) = delete;

    const CombatConfig& config() const { return cfg_; }
    uint32_t player_count() const { return static_cast<uint32_t>(players_.size()); }
    // Relation matrix (attacker player +0x370[id]); default: different players are enemies.
    void set_enemy(int32_t a, int32_t b, bool enemy);
    bool is_enemy(int32_t a, int32_t b) const;

    // Pre-sizes per-tick scratch so steady-state ticks never allocate.
    void reserve(uint32_t units, uint32_t projectiles, uint32_t candidates_per_unit, uint32_t delayed = 256);

    // ---- units (host) ----
    // Ids must be strictly increasing. Draws: one synced Get_Int per weapon hardpoint (first
    // cooldown, HardPoints order) [H], then one for a non-hardpoint weapon reset [L].
    bool add_unit(const UnitSpawn& s, uint32_t frame, SimRng& rng);
    bool remove_unit(ObjectId id);  // host removal (e.g. retreat); deaths are removed by tick()
    void set_position(ObjectId id, Vec3 p);
    void set_facing(ObjectId id, Vec3 f);
    void set_flag(ObjectId id, uint32_t flag, bool on);
    bool set_owner(ObjectId id, int32_t owner);  // S10-int: Change_Owner (host keeps trees / store in step)
    void set_ability_active(ObjectId id, int slot, bool on);
    bool ability_active(ObjectId id, int slot) const;
    // Attack order (engine/commands Attack): the ordered target slot; drops the auto target.
    // `hardpoint` >= 0 targets one hardpoint of the target (command 40/45 hardpoint index): the
    // weapons aim at and route damage to it while it is alive (S9).
    void order_attack(ObjectId attacker, ObjectId target, int32_t hardpoint = -1);
    void clear_order(ObjectId attacker);
    ObjectId ordered_target(ObjectId id) const;
    int32_t ordered_hardpoint(ObjectId id) const;
    // ---- refixf t06: Attack_Move on an object (TACTICAL_COMBAT §2.8) ----
    // The unit formation's attack goal (Formation_Attack_Target(F, &hp, 0) 0x41a2e0: the current movement block's object
    // when the block is not a guard). The host sets it with every move order of the unit (kNoObject: a position, guard or
    // stop). Path B (decide, before the scan): while the unit holds no ordered target, a goal that is an enemy, passes
    // 0x5680b0 (valid target, not dead / garrisoned / skip-targeting) and lies inside weapon range (XY distance <= max
    // attack range + footprint extent, outside Targeting_Min_Attack_Distance) becomes the LOCKED ordered target - no
    // priority score, exclusions, category restrictions or fog test. Attack_Move_Response_Range is not used. A dead goal
    // is cleared (the formation goal became a position).
    void set_attack_goal(ObjectId unit, ObjectId target, int32_t hardpoint = -1);
    ObjectId attack_goal(ObjectId unit) const;
    // Order_Attack (0x38d730, §2.8 C) with ordered = 1, as a task-force movement block issues it at order time (path A).
    // False = refused (C step 1: target not an enemy of the unit's owner; stealthed and the unit lacks
    // Target_Stealth_Units; garrisoned or dead) - the caller drops the unit from the move. A unit without any weapon has
    // no TARGETING behaviour: nothing is issued, true. Otherwise the weapon path (space unless BARRAGE (5) is active; land
    // always, ordered = 1): the lock is released (a locked ordered target becomes the auto target) and the weapon
    // hardpoints aim at the target (weapon_aim) until the next scan pick, path B or another order replaces it; the unit's
    // own target is untouched. Team members, LUCKY_SHOT (34) units and space BARRAGE units take the set path: the target
    // becomes their locked ordered target at once. [port: the melee-vs-airborne refusal and the autofire fallback are not
    // modelled.]
    bool order_attack_issue(ObjectId attacker, ObjectId target, int32_t hardpoint = -1);
    ObjectId weapon_aim(ObjectId id) const;  // path-A hardpoint aim (kNoObject: none)
    bool order_locked(ObjectId id) const;    // the ordered target carries the lock (TD +0xf4: path B / set path)
    // Get_Base_Attack_Range and Targeting_Min_Attack_Distance of a unit (staging, §2.8; 0 for an unknown id).
    float base_attack_range_of(ObjectId id) const;
    float min_attack_distance(ObjectId id) const;
    // Slot 45 "inside min range" (TACTICAL_GAPS §3.1): types with a min distance only; false for unknown ids.
    bool inside_min_range(ObjectId attacker, ObjectId target) const;

    // ---- S9: hooks and ability-facing API ----
    void set_hooks(CombatHooks* hooks) { hooks_ = hooks; }
    // RNG used by Take_Damage listeners called outside tick() (tick() binds its own for the tick).
    void bind_rng(SimRng* rng) { bound_rng_ = rng; }
    const CombatData& data() const { return *data_; }
    uint32_t desc_index(ObjectId id) const;  // UnitDesc index (0 if unknown)
    int32_t owner(ObjectId id) const;
    Vec3 position(ObjectId id) const;
    Vec3 facing(ObjectId id) const;
    uint32_t flags(ObjectId id) const;
    uint32_t team(ObjectId id) const;
    ObjectId unit_id_at(size_t index) const;  // ascending id order (index < unit_count())
    uint32_t hardpoint_count(ObjectId id) const;
    HardpointKind hardpoint_kind(ObjectId id, uint32_t hp) const;
    float hardpoint_max(ObjectId id, uint32_t hp) const;  // HP_Get_Max_Health
    bool hardpoint_targetable(ObjectId id, uint32_t hp) const;  // live and Is_Targetable
    uint32_t live_hardpoints(ObjectId id, HardpointKind kind) const;  // functioning: not destroyed, not disabled
    ObjectId hardpoint_opportunity_target(ObjectId id, uint32_t hp) const;
    int32_t choose_target_hardpoint(ObjectId attacker, ObjectId target) const;  // 0x394610
    int32_t closest_hardpoint(ObjectId target, Vec3 from, bool targetable_only) const;  // 0x394a80
    bool can_attack(ObjectId id) const;  // has a live weapon
    // ---- fog follow-up: space-station special weapon (SCRIPT_GAPS.md §1.1 step 5; Lua Fire_Special_Weapon on a
    // DUMMY_STAR_BASE through lua::TacticalScriptHooks::station_readiness / station_fire) ----
    // The special-weapon hard point: the first WEAPON_SPECIAL hardpoint in HardPoints order (0x394e60), -1 none.
    int32_t special_weapon_hardpoint(ObjectId station) const;
    // Readiness (0x385c00 [port]: the recharge formula is not reproduced): < 0 no such hard point, 0 destroyed /
    // disabled, 1 when its cooldown has run out, else 1 - cooldown / (Fire_Max_Recharge_Seconds x fps), >= 0.
    float special_weapon_readiness(ObjectId station) const;
    // Can-fire test (0x386220): live station and target, target an enemy of the station's owner and a valid
    // target, NOT FOW-hidden for the owner with force 1 (CombatHooks::target_hidden), inside the hard point's fire
    // cone, 2D distance from its fire bone (A, or the A-B midpoint) <= fire range + target radius and >= the
    // minimum range. Protection (0x540140) is not modelled [port].
    bool special_weapon_can_fire(ObjectId station, ObjectId target) const;
    // Target assignment (0x3822e0 [port]): an attack order on `target` (CombatSystem::order_attack). False when
    // special_weapon_can_fire fails.
    bool special_weapon_fire(ObjectId station, ObjectId target);
    // ---- L1 systems ----
    // A host-launched projectile (ProjectileLaunch). Max travel = the 3-D distance to the aim point (+ the target's
    // radius for a unit target), so a point shot detonates at the point (expiry: area damage / ion stun). It is
    // first stepped on the next tick, as weapon shots. False for an unknown projectile or a dead / missing target.
    // S8 t04: a category-3 projectile (LOB_SOLVER §5.1 / §5.2) leaves from `from` toward the un-led target point (the
    // target's position + its Ranged_Target_Z_Adjust, or `point`) on the solver's arc and speed, the yaw from the led aim;
    // no solution -> nothing is spawned, false.
    bool launch_projectile(const ProjectileLaunch& l, uint32_t frame);
    // S8 t04 (LOB_SOLVER §2, §3): the lob solver for ProjectileDesc `projectile` (category 3 only; G = gravity x lob
    // mod, Can_Lob_Slower) from `from` to `to`; false = no arc (or not a grenade). `out` may be null.
    bool lob_solution(uint32_t projectile, Vec3 from, Vec3 to, LobSolution* out = nullptr) const;
    // 0x398440: the unit's position + its type's Ranged_Target_Z_Adjust (z); the origin for an unknown id.
    Vec3 target_point(ObjectId id) const;
    // refixf t01: land rocket terrain (heightmap h(x, y) and its extent box, S9_DEATH_CLONE_SHIELDS.md §2.4.4 mode 1,
    // §2.4.6 ground points). Read in the serial commit phase only; S8 t04: also the land grenade ground test (LOB_SOLVER
    // §6.2). Unset [port]: flat ground at z 0, no box.
    void set_rocket_terrain(RocketTerrain t) { rocket_terrain_ = std::move(t); }
    // S8 t06: the host's tree set for detonations it triggers between ticks (DETONATE_REMOTE_BOMB). Set at setup so a
    // detonation before the first tick after a load queries the same trees as an uninterrupted run; tick() also sets it.
    void set_trees(const TreeSet* trees) { last_trees_ = trees; }
    const RocketTerrain& rocket_terrain() const { return rocket_terrain_; }
    // Spline-rocket state of projectile i (serial order); false when it is not a spline rocket.
    bool rocket(size_t i, RocketView& out) const;
    // Tactical weather (weather.hpp, L1_S4_REMAINING §3.2): accuracy modifier m per Projectile_Category; land shots'
    // inaccuracy radius / spread x (2 - m) (the ability inaccuracy override stays unscaled [M]). All 1 = no effect
    // (the default; bit-identical). [M: every owner is weather-affected.]
    void set_weather_accuracy(const std::array<float, 10>& m) { weather_acc_ = m; }
    // WEAPON_SPECIAL hard points never fire at a target FOW-hidden for their owner with force 1 (FOG_OF_WAR §5.3,
    // the special-weapon consumers): auto-targeted and opportunity shots of such a hard point are skipped while the
    // target is hidden (CombatHooks::target_hidden, force 1; no hooks = nothing hidden, unchanged behaviour).
    // Fog query used by the targeting filter (CombatHooks::target_hidden; false without hooks).
    bool target_hidden(int32_t player, ObjectId target, bool force) const {
        return hooks_ && hooks_->target_hidden(player, target, force);
    }
    // Get_Min_Health_Fraction (0x396470, Lua Get_Hull): hull fraction, or min(hull, hardpoint
    // fraction) for Should_Be_Destroyed_When_All_Hardpoints_Destroyed types, clamped to [0, 1].
    float script_hull(ObjectId id) const;
    float shield_fraction(ObjectId id) const;  // Lua Get_Shield: current / max, [0, 1]; 0 without shields
    bool shield_lockout(ObjectId id, uint32_t frame) const;  // depleted-shield lockout (0x380ad0)
    // Shield regen block (CD +0x131): set when the last functioning generator went offline, cleared when a generator
    // is repaired to max (HARDPOINT_DISABLED.md §9.2). DEFEND's shield gate (0x549460) reads it.
    bool shield_regen_blocked(ObjectId id) const;
    void force_regen_service(ObjectId id);  // DEFEND: shield and energy behaviours service next frame
    // Stealth (0x592ae0 target drop): clears the auto/ordered target and every hardpoint target.
    void clear_targets(ObjectId id);
    // WeaponBehavior burst reset (ion / lucky shot apply): cooldown 0, pulses = int(Fire_Pulse_Count
    // x FIRE_RATE x (1 + list 6)) [M].
    void reset_weapon_burst(ObjectId id);
    // Missile-shield emitter radius (0 = not an emitter); set by the ability system (4.3).
    void set_emitter_radius(ObjectId id, float radius);
    float emitter_radius(ObjectId id) const;
    // Sensor jamming (0x719620): every live non-allied unit with a locomotor and every non-allied
    // projectile within 3D distance <= radius gets its jammed frame := frame.
    void apply_jamming(int32_t jammer_owner, Vec3 center, float radius, uint32_t frame);
    bool is_jammed(ObjectId id, uint32_t frame) const;  // Is_Jammed (0x39b2f0)
    // Ion stun (0x62b920) with the projectile type's parameters; false if nothing applied.
    bool apply_ion_stun(ObjectId victim, uint32_t projectile_desc, uint32_t frame);
    bool ion_stunned(ObjectId id, uint32_t frame) const;
    uint32_t ion_stun_end(ObjectId id) const;
    float energy(ObjectId id) const;
    float max_energy(ObjectId id);
    void set_energy(ObjectId id, float e);
    bool burning(ObjectId id) const;
    // refix t02: INFECTION behaviour (0x54, S9_DEATH_CLONE_SHIELDS.md §1.5). infection_attached: the unit has the
    // behaviour (active or suspended); infection_suspend_count: its suspend counter (+0x3c; 0 = active = kUnitInfected).
    // suspend_infection = vfunc +0xc0 (counter + 1), resume_infection = vfunc +0xb8 (counter - 1, floored at 0); both
    // are no-ops without the behaviour (the Lua behaviour switch 0x393570 ignores a missing behaviour).
    bool infection_attached(ObjectId id) const;
    uint32_t infection_suspend_count(ObjectId id) const;
    void suspend_infection(ObjectId id);
    void resume_infection(ObjectId id, uint32_t frame);
    // Laser defense (0x716230): detonate projectile i (serial order index) at its position (area
    // damage, ion stun) and remove it. False if it was not alive.
    bool detonate_projectile(size_t index, uint32_t frame);
    // Die (0x39bdb0) for scripts / self-destruct: full death sequence (no-op on dead units). `sub_key` = the killing
    // projectile's Damage_Type index (DamageArmorTable damage index, GameConstants Damage_Types order), -1 none.
    void kill_unit(ObjectId id, ObjectId killer, uint8_t kind, uint32_t frame, int32_t sub_key = -1);
    std::span<const HardpointEvent> hardpoint_events() const { return hp_events_; }  // this tick
    std::span<const SmokeEvent> smoke_events() const { return smoke_events_; }      // this tick
    std::span<const DamageEvent> damage_events() const { return damage_events_; }   // this tick (A1 AI)
    std::span<const HitEvent> hit_events() const { return hit_events_; }            // this tick (S10-int, R4)
    std::span<const ShotEvent> shot_events() const { return shot_events_; }         // this tick (OI fog)
    std::span<const SurvivorEvent> survivor_events() const { return survivor_events_; }  // this tick (OI abilities)

    // ---- OI abilities ----
    // Base power (TACTICAL_GAPS §7.3 [H]): per player a list of entries keyed by the source object; "has base
    // power" = the list is non-empty (every entry is true) - an EMPTY list is no power. Readers here: energy pools
    // (energy 0), units with Requires_Base_Power do not target or fire (turrets 0x632270 "do not act"), shields of
    // Shield_Requires_Base_Power types do not regenerate [M]. Writer: Base_Power_Ability (abilities).
    void add_base_power(int32_t player, ObjectId source);
    void remove_base_power(int32_t player, ObjectId source);  // every entry of `source` for the player
    bool has_base_power(int32_t player) const;
    // Hardpoint repair (command 45, RepairHardpointEvent 0x4308b0 -> 0x387d40; TACTICAL_GAPS §1.12): adds `player`
    // to the hardpoint's repairer list (no duplicates, no cancel, no ownership / credit check here). Serviced per
    // frame in the unit's phase-D update before its weapons: each repairer in list order pays Repair_Cost_Per_Frame
    // (credits hook) and adds Repair_Amount_Per_Frame; one who cannot pay is dropped; a destroyed destroyable
    // hardpoint clears the list; at full health the list clears and the system comes back online. False for an
    // unknown unit / index.
    bool add_hardpoint_repairer(ObjectId id, uint32_t hp, int32_t player);
    uint32_t hardpoint_repairers(ObjectId id, uint32_t hp) const;
    // `player` is in the hardpoint's repairer list (hp +0x78; HUD "being repaired by the local player" blink).
    bool hardpoint_repaired_by(ObjectId id, uint32_t hp, int32_t player) const;
    // Hardpoint disabled flag (hp +0x6e, HARDPOINT_DISABLED.md), independent of destroyed. A hardpoint is "out"
    // (non-functional) when (Is_Destroyable && health <= 0) || disabled: its weapon does not count down or fire
    // (Update_Weapon 0x387400), it fails targeting filter 7 (Does_Weapon_State_Allow_Firing_At 0x384850), the aim
    // dry-run and unit_can_attack, it is not counted by live_hardpoints (Count_Functioning_Hardpoints 0x397c10) nor
    // by the offline "last of kind" test, and a special-weapon hard point reads readiness 0. It keeps its health,
    // stays targetable / damageable / repairable. Set only by copy_upgrade_hardpoints; cleared only by a repair
    // reaching max health (TACTICAL_GAPS §1.12 step 4). No timer. Hashed in "<p>.units". HUD (HUD_GAPS §1.2/§1.3):
    // grey reticle, Friendly_Disabled texture, no reticle for a disabled hardpoint of an enemy ship.
    // False for an unknown unit / index.
    bool hardpoint_disabled(ObjectId id, uint32_t hp) const;
    // HP_Set_Disabled (0x387d10): +0x6e := on, then Set_System_Online(hp, online = !on, destroyed = 0) (per-type
    // offline effects, CombatHooks::hardpoint_destroyed, special-weapon flag). Internal setter; the game's only
    // caller is the station-upgrade copy (always on = true). No-op for an unknown unit / index.
    void set_hardpoint_disabled(ObjectId id, uint32_t hp, bool on);
    // Station upgrade hardpoint copy (Starbase_Upgrade_Copy_Hardpoints 0x70ec70, HARDPOINT_DISABLED.md §3), by index
    // i < min(old count, new count), in list order:
    //   old has repairers -> new disabled; new health := old health (Is_Destroyable types; clamped); the old
    //                        repairers re-added to the new hardpoint in list order;
    //   else old destroyed (Is_Destroyable && health <= 0) or disabled -> new disabled, health := 0.1 (destroyable);
    //   else nothing (fresh, enabled);
    //   then, for an Allows_Special_Weapon_Use new hardpoint: every player allied to the new station's owner gets
    //   special_weapon_hp_enabled := !new.disabled.
    // The host (Starbase_Upgrade_Ability, not in the port yet) spawns `new_station` (Next_Level_Base) first and removes
    // the old station after. False when either unit is unknown.
    bool copy_upgrade_hardpoints(ObjectId old_station, ObjectId new_station);
    // Player tactical data +0x2f0 (HARDPOINT_DISABLED.md §4): starts 1; written by every Set_System_Online of an
    // Allows_Special_Weapon_Use hardpoint (:= online, for every player allied to the owner) and by the upgrade copy.
    // Skirmish SpecialWeaponsAllowed (0x527620) ANDs it (host). [port] allied = the player itself or not an enemy
    // (is_enemy). True for an unknown player. Hashed in "<p>.ext" only once some player's flag is 0.
    bool special_weapon_hp_enabled(int32_t player) const;
    // The hardpoint type's Special_Ability_Name crc (ENABLE_SPECIAL_ABILITY), 0 for none / unknown.
    uint32_t hardpoint_special_ability(ObjectId id, uint32_t hp) const;
    // HUD repair-capable test (HUD_GAPS §1.4, obj slot +0x38e): the unit's type has DUMMY_STAR_BASE
    // (UnitDesc::repairs_hardpoints). The HUD ORs this over the local player's selection before showing the repair
    // cursor; command 45 itself does no such check (TACTICAL_GAPS §1.12). False for an unknown unit.
    bool can_repair_hardpoints(ObjectId id) const;
    // OI tactical / S8 t01 (TACTICAL_GAPS 3.1 slot 43, LAND_OBSTACLE targets): the per-object passability cell query
    // (ObstacleCellFn; the battle host answers from PassabilityGrid::occupies). Used by every slot 43 / 46 range test
    // (targeting scan, inside_min_range, formation goal promotion). Unset, or NoFootprint for the target: the
    // half-extent rule. Not saved (the host installs it at setup).
    void set_obstacle_cell_query(ObstacleCellFn query) { obstacle_cell_ = std::move(query); }
    // refixf t07: the projectile segment query's per-entry test (precise model test, then the Collision_Box_Modifier
    // sphere). Unset: every tree-box hit is a hit (synthetic data without models).
    void set_precise_hit(PreciseHitFn fn) { precise_hit_ = std::move(fn); }
    // Credit hooks for the repair (CampaignBattles: the galactic account). Unset: repairs are free [port].
    void set_credit_hooks(std::function<float(int32_t)> get, std::function<void(int32_t, float)> add) {
        credits_get_ = std::move(get);
        credits_add_ = std::move(add);
    }
    // Lua Set_Targeting_Priorities (TACTICAL_COMBAT 2.4: data pack +0x160 override of the type's
    // Targeting_Priority_Set): set index into data().priority_sets, -1 clears. Team members score with their head's
    // set (so overriding the head drives the squad). False for an unknown unit or index.
    bool set_priority_override(ObjectId id, int32_t set);
    int32_t priority_override(ObjectId id) const;
    // OI abilities: the weapon index obj +0x324 (SWAP_WEAPONS flip, Permanent_Weapon_Swap := Weapon_Index). The
    // non-hardpoint weapon fires UnitDesc::weapon_projectiles[index]; [port] an index outside the list (or an empty
    // list) fires `weapon_projectile`. False for an unknown unit or a negative index.
    bool set_weapon_index(ObjectId id, int32_t index);
    int32_t weapon_index(ObjectId id) const;
    size_t weapon_set_count(ObjectId id) const;            // UnitDesc::weapon_projectiles size
    int32_t current_weapon_projectile(ObjectId id) const;  // Lua Get_Current_Projectile_Type: list entry, -1 outside
    int32_t priority_set_index(std::string_view name) const;  // case-insensitive, -1 unknown
    // Land stun (0x647290, TACTICAL_GAPS §1.10): weapons disabled, targeting / locomotor suspended, STUNNABLE end
    // frame := now + frames (assignment, not extended). Only STUNNABLE units; false otherwise. The release at the end
    // frame is [inferred] (STUNNABLE service not traced).
    bool stun_unit(ObjectId id, int32_t frames, uint32_t frame);
    bool stunned(ObjectId id, uint32_t frame) const;  // movement host: speed 0 while stunned
    // INVULNERABLE (0x4b) behaviour for `frames` (attached when missing, 0x630d60): Take_Damage passes only kinds 0 / 13.
    void make_invulnerable(ObjectId id, int32_t frames, uint32_t frame);
    bool timed_invulnerable(ObjectId id, uint32_t frame) const;
    // Missile-shield emitter registration (TACTICAL_GAPS §1.7): Add_Emitter count (multiset; Die removes one). The
    // per-projectile test needs a registration AND a radius (set_emitter_radius, the ability system's "qualifies").
    void add_emitter_entry(ObjectId id);
    uint32_t emitter_entries(ObjectId id) const;
    // Targeting acquisition this tick (Issue_Auto_Target for a targeting-acquired target; the autofire driver).
    bool acquired_target(ObjectId id) const;
    // Position changed since the previous tick (the host pushed a new position) - the ability system's "moving" test
    // when the host gives no locomotor state [port].
    bool moved(ObjectId id) const;
    bool ai_player(int32_t p) const { return player(p).ai; }
    // Mark a unit as converted to the enemy (obj +0x3a2 bit 4, converted decay) and give it to `owner`.
    void convert_unit(ObjectId id, int32_t owner);
    void set_hardpoint_health(ObjectId id, uint32_t hp, float h);  // clamp(h, 0, max); destroyed hardpoints stay

    ModifierSet* unit_modifiers(ObjectId id);  // nullptr if unknown
    ModifierSet& player_modifiers(int32_t player);

    // ---- simulation ----
    void tick(uint32_t frame, const TreeSet& trees, SimRng& rng, const ParallelFor& pf = serial_for);
    // Take_Damage (0x3a9e30) - section 3.6 order. Public for abilities/host and the formula tests.
    DamageResult take_damage(const DamageRequest& r, uint32_t frame);

    // ---- outputs / inspection ----
    std::span<const DeathEvent> deaths() const { return deaths_; }  // this tick
    const CombatStats& stats() const { return stats_; }
    size_t unit_count() const { return units_.size(); }
    size_t projectile_count() const { return projectiles_.size(); }
    ProjectileView projectile(size_t i) const;  // serial order
    bool has_unit(ObjectId id) const { return find(id) >= 0; }
    bool alive(ObjectId id) const;
    float health(ObjectId id) const;
    float shield(ObjectId id) const;
    float max_health(ObjectId id) const;
    float max_shield_of(ObjectId id) const;
    float hardpoint_health(ObjectId id, uint32_t hp) const;
    bool hardpoint_destroyed(ObjectId id, uint32_t hp) const;
    int32_t hardpoint_cooldown(ObjectId id, uint32_t hp) const;
    ObjectId auto_target(ObjectId id) const;
    ObjectId current_target(ObjectId id) const;  // ordered if set, else auto
    uint32_t next_scan(ObjectId id) const;
    // Cached modifier value r of `list` for a unit (owner player set + own set), refreshed at the
    // start of each tick and on demand when stale.
    float modifier(ObjectId id, ModList list);
    float mod_multiplier(ObjectId id, ModKind kind);
    float attack_range(ObjectId id);  // Get_Max_Attack_Range
    void set_health(ObjectId id, float h);
    void set_shield(ObjectId id, float s);

    // Q1: ordered digests (units by id, projectiles by serial). register_crc adds "<p>.units",
    // "<p>.projectiles", "<p>.modifiers", "<p>.delayed".
    void hash_units(StateHasher& h) const;
    void hash_projectiles(StateHasher& h) const;
    void hash_modifiers(StateHasher& h) const;
    void hash_delayed(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& prefix = "combat") const;
    // S9 state (energy, burning, ion stun, jamming, ordered hardpoint, emitters, opportunity fire,
    // projectile categories). Kept out of hash_units so the S8 golden stays valid; register with
    // register_extended_crc ("<p>.ext") - AbilitySystem::register_crc does it.
    void hash_extended(StateHasher& h) const;
    void register_extended_crc(CrcRegistry& registry, const std::string& prefix = "combat") const;

    // ---- N1 stage 2: save / load during a battle (contract: alamo/core/save_file.hpp; layout: combat_save.cpp) ----
    // Between ticks only. Writes every mutable field: units (incl. hardpoints, modifier sets and the modifier /
    // Mod_Multiplier caches, which are restored verbatim), projectiles, delayed damage, relations, special-weapon flags,
    // base power, repair lists, weather accuracy, the serial / frame counters and this tick's output events. read()
    // checks the definition counts (players, unit / hardpoint / projectile / ability descriptors, priority sets)
    // against the CombatData the system was built with, validates every index, requires r.done(), and leaves the
    // system unchanged on failure. Not saved (the host re-attaches them): hooks, bound RNG, obstacle / credit
    // callbacks, config, players, data. No host call is needed after read.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Hardpoint {
        uint32_t desc = 0;
        float health = 0.0f;
        int32_t cooldown = 0;
        int32_t pulses = 0;
        uint8_t destroyed = 0, disabled = 0;
        ObjectId target = kNoObject;
        int32_t target_hp = -1;
        // S9 opportunity fire (+0x50), hashed in hash_extended
        ObjectId opp_target = kNoObject;
        int32_t opp_hp = -1;
        uint32_t opp_next_scan = 0;
    };
    struct Unit {
        ObjectId id = kNoObject;
        uint32_t desc = 0;
        int32_t owner = -1;
        uint32_t team = 0;
        uint8_t team_leader = 0;
        ObjectId damage_parent = kNoObject;
        uint32_t flags = 0;
        Vec3 pos, last_pos, facing;
        Vec3 prev_pos;  // OI tactical: position two ticks ago (circular lead's three-point fit; derived, not hashed)
        float health = 0.0f, shield = 0.0f;
        uint8_t shield_depleted = 0;
        uint32_t shield_depleted_frame = 0;
        int32_t shield_regen_countdown = 0;
        uint32_t next_scan = 0;
        ObjectId auto_target = kNoObject, ordered_target = kNoObject;
        float target_score = 0.0f;
        int32_t weapon_cooldown = 0, weapon_pulses = 0;
        // S7 t15 A2 (MOVEMENT §3.1.1, pack +0x2c): the frame of the last WeaponBehavior service (0xFFFFFFFF: none yet - the
        // first service subtracts one frame [port]). Saved; not hashed (its effect shows in weapon_cooldown).
        uint32_t weapon_last = 0xFFFFFFFFu;
        // S8 t06 B4 (LOB_SOLVER §5.1, pack +0xf0): the fire-bone cycle index (round robin per shot). Hashed in
        // hash_extended when non-zero.
        uint32_t weapon_bone = 0;
        std::array<uint8_t, 2> ability_active{0, 0};
        uint32_t hp_begin = 0, hp_count = 0;
        uint8_t ever_hit = 0;
        uint32_t last_hit_frame = 0;
        std::array<float, 5> dmg_bucket{};     // DamageTracking [L]: damage per second, 5 s window
        std::array<uint32_t, 5> dmg_second{};
        ObjectId killer = kNoObject;
        // ---- S9 (hash_extended) ----
        int32_t ordered_hp = -1;
        float energy = 0.0f;
        int32_t energy_countdown = 0;
        uint8_t burning = 0;
        int32_t burn_countdown = 0;
        uint32_t ion_stun_end = 0;      // IonStunEffect end frame (behaviour +0x30); active while > frame
        uint32_t jammed_frame = 0;      // CD +0x194
        uint8_t ever_jammed = 0;
        uint8_t regen_blocked = 0;      // CD +0x130/+0x131: last shield generator destroyed
        float emitter_radius = 0.0f;    // missile-shield emitter (0 = none)
        // ---- refix t02: INFECTION behaviour (hash_extended only while attached) ----
        uint8_t infect_attached = 0;
        uint32_t infect_suspend = 0;           // suspend counter (+0x3c)
        int32_t infect_desc = -1;              // UnitDesc whose InfectionDesc drives it (-1 none: flag only)
        int32_t infect_player = -1;            // the infecting player (spread targets its enemies [L])
        ObjectId infect_source = kNoObject;    // the infecting unit (damage source)
        uint32_t infect_end = 0;               // expiry frame (Duration_Of_Infection; 0 = none)
        uint32_t infect_next_damage = 0;       // next Damage_Per_Second frame
        std::vector<std::pair<ObjectId, uint32_t>> infect_exposure;  // neighbours in range: (id, first frame), id order
        // ---- OI abilities (hash_extended) ----
        int32_t priority_override = -1;  // Lua Set_Targeting_Priorities (data pack +0x160)
        uint32_t stun_end = 0;           // STUNNABLE end frame (stunned while > frame)
        uint32_t invuln_end = 0;         // INVULNERABLE behaviour end frame
        uint32_t emitter_entries = 0;    // Add_Emitter registrations (multiset count)
        int32_t weapon_index = 0;        // obj +0x324 (hashed only when != 0)
        uint8_t acquired = 0;            // scan picked a target this tick (not hashed: transient)
        // ---- refixf t06 (TACTICAL_COMBAT §2.8; hash_extended only when set) ----
        ObjectId goal_target = kNoObject;  // formation attack goal (path B)
        int32_t goal_hp = -1;
        ObjectId aim_target = kNoObject;   // path-A weapon-hardpoint aim (Order_Attack weapon path)
        int32_t aim_hp = -1;
        uint8_t locked = 0;                // the ordered target carries the lock (dropped on garrison / stealth / fog)
        // refixf t06 (TACTICAL_COMBAT §2.9 Q3): TD +0x10 for the scan target, fixed when the scan sets a new target
        // (Choose_Target_Hardpoint) and re-chosen only by the space formation step; -1 none.
        int32_t auto_hp = -1;
        ModifierSet mods;
        // ---- caches (never hashed) ----
        std::array<float, kModListCount> mod_r{};
        uint32_t mod_ver = 0, player_ver = 0;
        uint8_t mod_valid = 0;
        std::array<float, kModKindCount> own_mult{};
        std::array<float, kModKindCount> mult{};
        uint16_t squad_seen = 0;
        uint8_t needs_scan = 0;  // phase C output
        uint8_t scan_due = 0;
    };
    struct Candidate {
        ObjectId id;
        int32_t player;
        float score;
        float fraction;
        float dist2;
        uint16_t aim_points;  // live targetable hardpoints (HP_Choose_Aim_Point draw span)
        uint8_t aimable;
    };
    struct Projectile {
        uint32_t serial = 0;
        uint32_t desc = 0;
        ObjectId shooter = kNoObject;
        int32_t owner = -1;
        ObjectId target = kNoObject;
        int32_t target_hp = -1;
        Vec3 pos, dir;
        float speed = 0.0f, travelled = 0.0f, max_travel = 0.0f;
        uint32_t spawn_frame = 0;
        float damage = 0.0f;
        uint16_t damage_type = kNoTypeId;
        uint8_t kind = kKindNormal;
        uint8_t alive = 1;
        uint32_t jammed_frame = 0;  // S9 (hash_extended)
        uint8_t ever_jammed = 0;
        // S8 t04 (LOB_SOLVER §5.3 / §6): grenade flight (category 3). `lob` set: N(t) = launch + speed t lob_dir -
        // 1/2 gravity t^2 z with t = frame - spawn_frame (the pack's launch point, InitialFacing forward and G). A sticky
        // grenade that hit something stops: `stuck`, attached to `stuck_to` at `stuck_offset` (S8 t06: in the parent's
        // frame, turned with its facing) or resting (kNoObject). Hashed in hash_extended for grenades only.
        uint8_t lob = 0;
        uint8_t stuck = 0;
        float gravity = 0.0f;
        Vec3 launch, lob_dir, stuck_offset;
        ObjectId stuck_to = kNoObject;
        // OI abilities: BOMB timer (seconds; < 0 none), hashed when set. Without `lob` the shot sits where it was placed
        // (armed bomb); a tossed grenade (ProjectileLaunch::toss) flies with it running.
        float fuse = -1.0f;
        uint8_t rocket = 0;  // refixf t01: spline rocket (state in rockets_, same serial); hashed in hash_extended
    };
    // refixf t01: spline-rocket flight state, one per Projectile with rocket != 0, serial order (S9 §2.4.1 pack fields).
    struct Rocket {
        uint32_t serial = 0;
        uint8_t planned = 0;  // +0x30 non-null
        float s = 0.0f;       // +0x38
        float budget = 0.0f;  // +0x54
        Vec3 fire_at;         // +0x3c
        Vec3 launch;          // +0x80
        Vec3 last_vel;        // +0x98
        RocketSpline path;
    };
    struct RocketNext {  // phase E output per projectile: the next spline point N (sample at s + speed)
        Vec3 n;
        uint8_t ok = 0;
    };
    struct Repairer {  // OI abilities: hardpoint repair lists (insertion order per hardpoint)
        ObjectId unit;
        uint32_t hp;
        int32_t player;
    };
    struct Delayed {
        ObjectId victim;
        int32_t frames;
        float amount;
        uint16_t damage_type;
        uint8_t kind;
        uint8_t source_is_projectile;
        ObjectId source;
        int32_t projectile_desc;
        int32_t hardpoint;
    };
    // mesh_hp (refixf t07): the hardpoint the struck mesh names (§2.9 Q1 step 4), -1 none (hull), kMeshUnknown when
    // the hit came without a precise test (the mesh_hardpoint stand-in decides).
    static constexpr int32_t kMeshUnknown = -2;
    struct SweepHit {
        ObjectId id;
        float t;
        int32_t mesh_hp = kMeshUnknown;
    };

    int32_t find(ObjectId id) const;  // unit index or -1
    const PlayerInfo& player(int32_t p) const;
    Unit* unit(ObjectId id);
    const Unit* unit(ObjectId id) const;
    const UnitDesc& desc_of(const Unit& u) const { return data_->units[u.desc]; }
    bool live(const Unit* u) const { return u && !(u->flags & kUnitDead); }

    // modifiers
    void refresh_unit_cache(Unit& u);
    void ensure_cache(Unit& u);
    float mod_r(const Unit& u, ModList l) const { return u.mod_r[static_cast<uint32_t>(l)]; }
    float mult(const Unit& u, ModKind k) const { return u.mult[static_cast<uint32_t>(k) - 1u]; }
    const UnitAbilityDesc* ability(const Unit& u, int slot) const;

    // derived stats
    float unit_max_hull(const Unit& u) const;
    float unit_max_shield(const Unit& u) const;
    float hp_max(const Unit& u, const Hardpoint& h) const;
    float hp_fraction(const Unit& u) const;  // destroyable hardpoints: sum cur / sum max (0 if all dead)
    bool hp_out(const Hardpoint& h) const;  // (Is_Destroyable && health <= 0) || destroyed || disabled
    bool hp_dead(const Hardpoint& h) const;  // refixf t06: Is_Destroyable && (destroyed || health <= 0)
    // refixf t06 (TACTICAL_COMBAT §2.9 Q1 port difference 1) [port]: the hardpoint standing in for the struck mesh.
    int32_t mesh_hardpoint(const Unit& v, Vec3 impact) const;
    // refixf t06 (§2.9 Q2 step 3, 0x383f70): the aim point without a target hardpoint; false = no shot.
    bool pick_aim_point(const Unit& u, const Hardpoint& h, const Unit& t, Vec3 ref, float range, SimRng& rng, Vec3& out) const;
    // refixf t06 (§2.9 Q3): Service_Space re-chooses a null / dead target hardpoint for formation units.
    void service_space_hardpoint(Unit& u);
    void set_system_online(Unit& u, uint32_t index, bool online, bool destroyed, uint32_t frame);  // 0x384d00
    float min_health_fraction(const Unit& u) const;
    float base_attack_range(const Unit& u) const;
    float unit_fire_range(const Unit& u) const;
    Vec3 local_to_world(const Unit& u, Vec3 local) const;
    Vec3 hp_world(const Unit& u, const HardpointDesc& hd) const;
    Vec3 velocity(const Unit& u) const;
    // OI tactical: Compute_Lead_Point (0x399450): circular lead over the three-position history (formulas.hpp).
    Vec3 lead_point(const Unit& t, Vec3 muzzle, Vec3 aim0, float projectile_speed) const;
    // S8 t04: category 3 (Projectile_Category 3 / `lobbed`) and its G = gravity x lob mod (may be <= 0 from XML).
    static bool grenade(const ProjectileDesc& pd) { return pd.lobbed || pd.category == ProjectileCategory::Grenade; }
    static float lob_g(const ProjectileDesc& pd) { return pd.grenade_gravity * pd.lob_mod; }
    // Slot 44 for a grenade weapon (LOB_SOLVER §3.1): an arc from the attacker to the target point exists.
    bool lob_in_range(const Unit& a, const ProjectileDesc& pd, const Unit& t) const;
    // The current weapon's projectile when it is a grenade (slot 42 -> category 3), else nullptr.
    const ProjectileDesc* grenade_weapon(const Unit& u) const;
    // §5.1 target point Q: a team container (a unit other live units name as their team) -> its live member nearest
    // `from` (0x404fc0; XY distance, ties by id [port]); then position + Ranged_Target_Z_Adjust.
    const Unit& lob_member(const Unit& t, Vec3 from) const;
    Vec3 lob_target_point(const Unit& t) const;
    // §6 grenade flight: the step's end point N (from the closed form) and the land ground test of P -> N.
    Vec3 lob_next(const Projectile& p, uint32_t frame) const;
    bool ground_hit(Vec3 a, Vec3 b, GroundHit& out) const;  // S8 t06: 0x135840 (rocket.hpp ground_segment)
    Vec3 stuck_local(const Unit& v, Vec3 world) const;     // S8 t06: world point -> v's frame (inverse local_to_world)
    uint32_t bomb_frame(float seconds, uint32_t armed_frame) const;  // S8 t06: BOMB arm 0x58f030 detonation frame
    void lob_step(uint32_t i, uint32_t frame, SimRng& rng);
    // OI tactical (team targeting, TACTICAL_COMBAT 2.3 / 2.4): the unit's squad "container" - the unit whose id is
    // its team id when that is a combat unit, else the team's team_leader member; nullptr when none / itself.
    const Unit* team_head(const Unit& u) const;
    float target_extent(const Unit& t, Vec3 from) const;  // OI tactical: TACTICAL_GAPS 3.1 footprint half-extent E
    // TACTICAL_GAPS 3.1 slot 43 range geometry: XY distance from `from` to the aim point and the slack E.
    void range_geometry(const Unit& t, Vec3 from, float& d, float& E) const;
    int32_t fired_weapon_projectile(const Unit& u) const;  // OI abilities: Projectile_Types[obj +0x324]
    bool sticky(const Unit& u, const Unit& target, uint32_t frame) const;
    float damage_rate(const Unit& u, uint32_t frame) const;

    // targeting
    bool unit_can_attack(const Unit& u) const;
    bool candidate_ok(const Unit& a, const Unit& t, float range) const;  // filters 2.3 (without score)
    float score(const Unit& a, const Unit& t) const;
    bool aimable(const Unit& a, const Unit& t) const;
    bool in_cone(const Unit& a, const Hardpoint& h, Vec3 aim) const;
    void gather(uint32_t unit_index, QueryContext& ctx);
    void decide(Unit& u, std::vector<Candidate>& cands, uint32_t frame, SimRng& rng);  // decide_target + space hp step
    void decide_target(Unit& u, std::vector<Candidate>& cands, uint32_t frame, SimRng& rng);
    bool lock_holds(const Unit& u, const Unit& t) const;  // refixf t06: §2.8 B "while locked" drop rules
    bool promote_goal(Unit& u);                           // refixf t06: §2.8 B promotion
    bool ability_type_active(const Unit& u, int32_t type) const;
    int32_t choose_target_hardpoint(const Unit& a, const Unit& t) const;
    int32_t closest_hardpoint(const Unit& t, Vec3 from, bool targetable_only) const;
    uint16_t aim_point_count(const Unit& t) const;

    // weapons
    void update_weapons(Unit& u, uint32_t frame, SimRng& rng);
    bool fire_hardpoint(Unit& u, Hardpoint& h, uint32_t frame, SimRng& rng);
    void fire_unit_weapon(Unit& u, uint32_t frame, SimRng& rng);
    // S8 t08 (TACTICAL_COMBAT §3.1.1): Is_Deployed_Or_Deploying (0x39a390) and the deploy transition (0x39a400 / 0x39b2a0),
    // both for a Deploys type with a locomotor only; the own-weapon gate 0x54c910 and the hardpoint gate 0x384850 (`t`:
    // the primary target, nullptr = none - steps 4-6 only).
    bool deployed_or_deploying(const Unit& u) const;
    bool deploy_transition(const Unit& u) const;
    bool unit_weapon_allowed(const Unit& u) const;
    bool hp_state_allows(const Unit& u, const Hardpoint& h, const Unit* t) const;
    bool opportunity_allowed(const Unit& u, const HardpointDesc& hd, bool idle) const;
    bool opportunity_fire(Unit& u, Hardpoint& h, uint32_t frame, SimRng& rng);
    bool opportunity_valid(const Unit& u, const Hardpoint& h, const Unit& t) const;
    float weapon_delay(const Unit& u, const HardpointDesc* hd) const;
    float fire_rate(const Unit& u) const;
    // `lob` (S8 t04): the solver's result for a WeaponBehavior grenade (launch from `muzzle`, speed / pitch from it, yaw
    // from `aim`; `q_point` = the un-led Q for the travel budget); nullptr: straight at `aim` (hardpoints - a
    // category-3 hardpoint shot still flies the parabola at Max_Speed).
    void spawn(const Unit& shooter, const ProjectileDesc& pd, uint32_t pdesc, float damage, uint16_t damage_type,
               Vec3 muzzle, Vec3 aim, const Unit& target, int32_t target_hp, float range, uint32_t frame,
               const LobSolution* lob = nullptr, Vec3 q_point = {});

    // projectiles / damage
    void sweep(uint32_t pi, QueryContext& ctx);
    SweepHit sweep_segment(const Projectile& p, Vec3 delta, QueryContext& ctx) const;  // ray p.pos -> p.pos + delta
    // `segment` (rockets): the step's P -> N vector; the impact is p.pos + segment x t (else p.pos + dir x (speed x t)).
    void process_hit(Projectile& p, Unit& victim, float t, uint32_t frame, SimRng& rng, const Vec3* segment = nullptr,
                     int32_t mesh_hp = kMeshUnknown);
    int32_t hardpoint_by_mesh(const Unit& v, const std::string& mesh) const;  // §2.9 Q1 step 4 (0x394d40)
    void expire_projectile(uint32_t i, uint32_t frame);  // 3.3 expiry: detonation at the projectile's position
    void area_damage(const Projectile& p, Vec3 center, ObjectId direct, uint32_t frame);
    void detonation_effects(const Projectile& p, Vec3 center, ObjectId direct, uint32_t frame);
    bool ion_stun_unit(Unit& v, uint32_t pdesc, uint32_t frame);
    void apply_or_queue(const DamageRequest& r, float delay_secs, uint32_t frame);
    DamageResult take_damage_inner(const DamageRequest& r, uint32_t frame, int depth);
    void hull_damage(Unit& v, float x, ObjectId source, DamageResult& out, uint32_t frame, uint8_t kind,
                     int32_t sub_key = -1);
    void hardpoint_damage(Unit& v, Hardpoint& h, float x, ObjectId source, DamageResult& out, uint32_t frame,
                          bool owner_dying = false, int32_t sub_key = -1);
    void kill(Unit& v, ObjectId killer, uint8_t kind, uint32_t frame, int32_t sub_key = -1);
    // S9 t04: Pick_Death_Clone (0x3718f0) with index -1; one synced draw for a list of >= 2. -1 when no entry.
    int32_t pick_death_clone(const UnitDesc& vd, int32_t key, int32_t sub_key);
    // S9 t05: Missile_Shield_Deflect_Test (0x4c6e30): true + the outward direction for the first qualifying emitter
    // (ascending id) whose sphere holds the projectile.
    bool shield_away(const Projectile& p, Vec3& away) const;
    // refixf t01: one flight step of a spline rocket (0x4cfc40, §2.4.2), serial commit phase.
    void rocket_step(uint32_t i, uint32_t frame, SimRng& rng);
    bool spline_rocket(const ProjectileDesc& pd) const;
    void add_rocket(const Projectile& p, Vec3 fire_at, Vec3 launch);
    Rocket* find_rocket(uint32_t serial);
    const Rocket* find_rocket(uint32_t serial) const;
    float projectile_turn_rate(const ProjectileDesc& pd) const;  // 0x372560
    bool has_energy_pool(const Unit& u) const { return desc_of(u).energy_capacity > 0.0f; }
    float unit_max_energy(const Unit& u) const;
    void burning_service(Unit& u, uint32_t frame);
    // refix t02: INFECTION update (0x636ae0) of one unit, and attaching / resuming the behaviour on a victim.
    void infection_service(Unit& u, uint32_t frame);
    void infect(Unit& v, int32_t desc, int32_t player, ObjectId source, uint32_t frame);
    void converted_decay(Unit& u, uint32_t frame, SimRng& rng);
    SimRng* listener_rng() const { return tick_rng_ ? tick_rng_ : bound_rng_; }
    void track_damage(Unit& v, float amount, uint32_t frame);
    float diminishing(uint32_t frame, const Unit& v) const;
    void upkeep_unit(Unit& u, uint32_t frame);
    void flush();

    QueryContext* acquire_ctx();
    void release_ctx(QueryContext* c);

    const CombatData* data_;
    CombatConfig cfg_;
    std::vector<PlayerInfo> players_;
    std::vector<uint8_t> enemy_;  // [a * n + b]
    std::vector<uint8_t> sw_hp_enabled_;  // per player, player tactical data +0x2f0 (starts 1)
    std::vector<ModifierSet> player_mods_;
    std::vector<Unit> units_;  // sorted by id
    std::vector<Hardpoint> hps_;
    std::vector<std::vector<Candidate>> cands_;  // per unit index (phase C)
    std::vector<Projectile> projectiles_;        // serial order
    std::vector<SweepHit> sweep_;                 // per projectile (phase E)
    std::vector<Delayed> delayed_;
    std::vector<DeathEvent> deaths_;
    std::vector<HardpointEvent> hp_events_;
    std::vector<SmokeEvent> smoke_events_;
    std::vector<DamageEvent> damage_events_;
    std::vector<HitEvent> hit_events_;  // S10-int
    std::vector<ShotEvent> shot_events_;  // OI fog
    // ---- OI abilities ----
    std::vector<SurvivorEvent> survivor_events_;
    std::vector<std::pair<int32_t, ObjectId>> base_power_;  // (player, source), insertion order
    std::vector<Repairer> repairs_;
    ObstacleCellFn obstacle_cell_;                              // OI tactical / S8 t01: LAND_OBSTACLE aim walk
    PreciseHitFn precise_hit_;                                  // refixf t07: projectile segment entry test
    std::function<float(int32_t)> credits_get_;
    std::function<void(int32_t, float)> credits_add_;
    void service_repairs(Unit& u, uint32_t frame);
    void death_spawns(Unit& v, uint32_t frame);  // Die step 7 (respawn, Destruction_Survivors)
    bool effect_match(const Unit& v, uint64_t mask, const std::vector<int32_t>* types) const;
    void effect_victims(int32_t owner, bool enemies, Vec3 center, float radius, std::vector<Unit*>& out);
    std::vector<Unit*> effect_scratch_;
    std::vector<std::pair<ObjectId, uint32_t>> infect_scratch_;  // refix t02: infection_service exposure scratch
    void record_damage(const DamageRequest& r, const Unit& v, const Unit* src, int32_t hp, const DamageResult& out,
                       uint32_t frame);
    std::vector<uint32_t> emitters_;  // unit indices with emitter_radius > 0 (ascending id), phase F scratch
    std::vector<Rocket> rockets_;          // refixf t01: spline-rocket state, serial order
    std::vector<RocketNext> rocket_next_;  // refixf t01: phase E per-projectile scratch
    RocketTerrain rocket_terrain_;         // refixf t01: host terrain (not saved: the host re-attaches it)
    CombatHooks* hooks_ = nullptr;
    SimRng* bound_rng_ = nullptr;
    SimRng* tick_rng_ = nullptr;
    std::vector<Hardpoint> flush_hps_;
    std::vector<std::pair<uint32_t, uint32_t>> leaders_;  // (team, unit index) scratch
    std::vector<std::pair<uint32_t, ObjectId>> team_heads_;  // OI tactical: (team, team_leader id), built in phase A
    std::vector<float> dim_x_, dim_y_, dim_y2_;              // OI tactical: diminishing-firepower spline (sorted)
    uint32_t next_serial_ = 1;
    uint32_t frame_ = 0;
    uint32_t cand_reserve_ = 16;
    CombatStats stats_;
    // per-tick parameters for the parallel phases (lambdas capture `this` only)
    const TreeSet* trees_ = nullptr;
    // S8 t06: the host's tree set (set_trees, and each tick), for detonations the host triggers between ticks
    const TreeSet* last_trees_ = nullptr;
    std::mutex ctx_mutex_;
    std::vector<std::unique_ptr<QueryContext>> ctx_all_;
    std::vector<QueryContext*> ctx_free_;
    QueryContext serial_ctx_;
    std::array<float, 10> weather_acc_{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // L1 systems: weather accuracy per category
    float weather_scale(const ProjectileDesc& pd) const {
        const float m = weather_acc_[static_cast<uint32_t>(pd.category) < 10u ? static_cast<uint32_t>(pd.category) : 0u];
        return 2.0f - m;
    }
};

}  // namespace alamo::tactical::combat
