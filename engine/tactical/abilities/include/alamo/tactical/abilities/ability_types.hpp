// S9 - unit-ability type enum, its static per-type info table and the special-ability enums
// (docs/re/ABILITIES_HARDPOINTS.md 2.1, 3.1). Pure data, no state.
#pragma once
#include <cstdint>
#include <string_view>

#include "alamo/tactical/combat/formulas.hpp"

namespace alamo::tactical::abilities {

using combat::GameMode;

// Per-object arrays are indexed by type 0..0x4c (the info table has 0x4d entries; 0 and 76 unused).
inline constexpr uint32_t kAbilitySlots = 0x4d;
inline constexpr uint8_t kMaxAbilityType = 75;

// UnitAbilityType (0x5dea20) [H]; LURE = 10 and STUN = 49 per the icon strings (2.1).
enum AbilityType : uint8_t {
    kAbilityNone = 0,
    kDefend = 1,
    kDeploy = 2,
    kInterdict = 3,
    kHarass = 4,
    kBarrage = 5,
    kHunt = 6,
    kSpreadOut = 7,
    kAfterburner = 8,
    kTurbo = 9,
    kLure = 10,
    kRocketAttack = 11,
    kAvoidDanger = 12,
    kForceTelekinesis = 13,
    kForceWhirlwind = 14,
    kForceLightning = 15,
    kFlameThrower = 16,
    kJetPack = 17,
    kCaptureVehicle = 18,
    kTargetedInvulnerability = 19,
    kTargetedHack = 20,
    kTargetedRepair = 21,
    kStickyBomb = 22,
    kAreaEffectStun = 23,
    kAreaEffectHeal = 24,
    kAreaEffectConvert = 25,
    kDistract = 26,
    kCableAttack = 27,
    kFowRevealPing = 28,
    kPowerToWeapons = 29,
    kTractorBeam = 30,
    kEnergyWeapon = 31,
    kMissileShield = 32,
    kInvulnerability = 33,
    kLuckyShot = 34,
    kHarmonicBomb = 35,
    kUntargetedStickyBomb = 36,
    kConcentrateFire = 37,
    kDeployTroopers = 38,
    kSelfDestruct = 39,
    kSprint = 40,
    kIonCannonShot = 41,
    kWeakenEnemy = 42,
    kSpoilerLock = 43,
    kReplenishWingmen = 44,
    kMaximumFirepower = 45,
    kEjectVehicleThief = 46,
    kFireLobbingSuperweapon = 47,
    kStealth = 48,
    kStun = 49,
    kRadioactiveContaminate = 50,
    kBerserker = 51,
    kForceSight = 52,
    kSaberThrow = 53,
    kForceCloak = 54,
    kSuperLaser = 55,
    kLaserDefense = 56,
    kForceConfuse = 57,
    kLeechShields = 58,
    kTacticalBribe = 59,
    kSwapWeapons = 60,
    kClusterBomb = 61,
    kFullSalvo = 62,
    kSensorJamming = 63,
    kPlaceRemoteBomb = 64,
    kDetonateRemoteBomb = 65,
    kInfection = 66,
    kProximityMines = 67,
    kStimPack = 68,
    kDrainLife = 69,
    kBlast = 70,
    kBuzzDroids = 71,
    kShieldFlare = 72,
    kSummon = 73,
    kCorruptSystems = 74,
    kDeploySquad = 75,
};

// Behaviour type ids of column B (OBJECT_RUNTIME 4.3).
inline constexpr int16_t kBehaviorNone = -1;
inline constexpr int16_t kBehaviorDeployTroopers = 0x1c;
inline constexpr int16_t kBehaviorHunt = 0x21;
inline constexpr int16_t kBehaviorLure = 0x23;
inline constexpr int16_t kBehaviorSelfDestruct = 0x43;

// One row of the info table at 0x140b2f2e0 (filled by 0x43fab0) [H]; `defaulted` marks the rows
// that keep the entry constructor's defaults, assumed M1 B-1 X1 U0 F0 apart from the explicit
// fields [M] (open item 1).
struct AbilityTypeInfo {
    const char* name = "";
    uint8_t mode = 1;             // M (+0x20): the per-object active byte is written on (de)activation
    int16_t behavior = kBehaviorNone;  // B (+0x24): implementing behaviour
    uint8_t auto_deactivate = 1;  // X (+0x29): Set_Unit_Ability_Active(off) on expiry
    uint8_t per_unit = 0;         // U (+0x2a): a team container does not aggregate / forward it
    uint8_t force = 0;            // F (+0x2c): blocked while the Force-disable timer runs
    uint8_t defaulted = 0;
};
const AbilityTypeInfo& ability_type_info(uint32_t type);  // out of range -> the row of 0
// Case-insensitive name lookup ("power_to_weapons" is accepted); 0 if unknown.
uint8_t ability_type_from_name(std::string_view name);

// Activation_Style (0x5e42e0) [H].
enum class ActivationStyle : uint8_t {
    None = 0,
    GlobalAutomatic = 1,
    CombatAutomatic = 2,
    GalacticAutomatic = 3,
    SpaceAutomatic = 4,
    GroundAutomatic = 5,
    GroundActivated = 6,
    HeroDetected = 7,
    CombatImminent = 8,
    SpecialAttack = 9,
    TakeDamage = 10,
    UserInput = 11,
    SkirmishAutomatic = 12,
};
ActivationStyle activation_style_from_name(std::string_view name);  // case-insensitive; None if unknown
// SAP_Style_Allowed_In_Mode (0x37c270) [H]: 1 always; 2, 9, 10, 11 tactical; 3, 6, 7, 8 galactic;
// 4 space; 5 land; 12 skirmish only.
bool style_allowed(ActivationStyle style, GameMode mode, bool skirmish);
// Styles applied when the unit is added (auras / passive abilities, 3.1 "event triggers") [M].
bool style_automatic(ActivationStyle style);

// Special-ability classes this module implements (the XML element name selects the class).
enum class SpecialClass : uint8_t {
    Unsupported = 0,  // parsed (fields kept) but no behaviour: apply fails, reported in events
    TractorBeam,      // Tractor_Beam_Attack_Ability (3.2)
    IonCannonShot,    // Ion_Cannon_Shot_Attack_Ability (3.3)
    LuckyShot,        // Lucky_Shot_Attack_Ability (3.4)
    ConcentrateFire,  // Concentrate_Fire_Attack_Ability (3.5)
    EnergyWeapon,     // Energy_Weapon_Attack_Ability (3.6)
    Stealth,          // Stealth_Ability (3.7)
    SensorJamming,    // Sensor_Jamming_Ability (3.8)
    LaserDefense,     // Laser_Defense_Ability (3.10)
    RedirectBlaster,  // Redirect_Blaster_Ability (4.6, TACTICAL 3.8)
    AbsorbBlaster,    // Absorb_Blaster_Ability (4.6) [L]
    ForceSight,       // Force_Sight_Ability (FOG_OF_WAR.md §3.3: Reveal_All for the owner while channelling)
    // ---- OI abilities (TACTICAL_GAPS.md §2.3, TACTICAL_COMBAT §4.3) ----
    CombatBonus,          // Combat_Bonus_Ability (aura: object ModifierSet entries, event-driven)
    EnhanceDefense,       // Enhance_Defense_Ability: permanent per-player list-0 entries (s78n t04, TACTICAL_COMBAT §5.4.4 3c)
    BattlefieldModifier,  // Battlefield_Modifier_Ability (player-level lists, §2.3.11)
    ForceHealing,         // Force_Healing_Ability (§2.3.1)
    Berserker,            // Berserker_Ability (§2.3.2)
    ProximityMines,       // Proximity_Mines_Ability (§2.3.3)
    Earthquake,           // Earthquake_Attack_Ability (§2.3.4)
    Telekinesis,          // Force_Telekinesis_Ability (§2.3.4)
    GrenadeAttack,        // Grenade_Attack_Ability (§2.3.5)
    ForceLightning,       // Force_Lightning_Ability (§2.3.6)
    ForceWhirlwind,       // Force_Whirlwind_Ability (§2.3.7)
    StunWave,             // Stun_Ability (§2.3.7)
    ForceConfuse,         // Force_Confuse_Ability (§2.3.7)
    ArcSweep,             // Arc_Sweep_Attack_Ability (§2.3.8)
    GenericAttack,        // Generic_Attack_Ability (§2.3.8)
    EatAttack,            // Eat_Attack_Ability (§2.3.8)
    SuperLaser,           // Super_Laser_Ability (§2.3.9)
    DrainLife,            // Drain_Life_Ability (§2.3.10)
    LeechShields,         // Leech_Shields_Ability
    Blast,                // Blast_Ability
    ForceCloak,           // Force_Cloak_Ability
    Hack,                 // Hack_Ability
    Repair,               // Repair_Ability
    VehicleThief,         // Vehicle_Thief_Ability
    MaximumFirepower,     // Maximum_Firepower_Attack_Ability
    EnableAbility,        // Enable_Ability
    BasePower,            // Base_Power_Ability (§7.3)
    RetreatPrevention,    // Retreat_Prevention_Ability (§1.5, the interdict list) [M]
    TacticalBribe,        // Tactical_Bribe_Ability (§2.3.12)
    CorruptSystems,       // Corrupt_Systems_Ability
    BuzzDroids,           // Buzz_Droids_Ability
    Summon,               // Summon_Ability
    Spawn,                // Spawn_Ability
    ClusterBomb,          // Cluster_Bomb_Ability
    RemoteBomb,           // Remote_Bomb_Ability
    FlameThrower,         // Personal_Flame_Thrower_Ability
    ShieldFlare,          // Shield_Flare_Ability
    Infection,            // Infection_Ability
    Radioactive,          // Radioactive_Contaminate_Ability
    SaberThrow,           // Saber_Throw_Ability
    WeaponSwap,           // Permanent_Weapon_Swap_Ability
    Weatherproof,         // Weatherproof_Ability (§7.4)
    CancelSpecial,        // Cancel_Special_Ability
    Cable,                // Cable_Ability
    // Starbase_Upgrade_Ability (STATION_HANGARS_SOUND.md §1.1, update 0x70ec20): the skirmish station upgrade. Service
    // interval 2 (first due = synced Get_Int(now, now + 2) at the SAP init); on the first service it asks the host for
    // the station's Next_Level_Base (SpawnPurpose::StationUpgrade) and then for the upgrade object's silent removal
    // (AbilityEventKind::RemoveSilently), whether or not a station was found. One-shot.
    StarbaseUpgrade,
};
SpecialClass special_class_from_tag(std::string_view element_name);
const char* special_class_name(SpecialClass c);
// OI abilities: the class's SAP service interval (vfunc +0x18, TACTICAL_GAPS §1.4) [H]: 0 never updated, 1 every
// frame, FPS / 10 (integer divide) for Earthquake_Attack / Force_Telekinesis. Galactic / unknown classes: 0.
int32_t special_service_interval(SpecialClass c, uint32_t fps);

}  // namespace alamo::tactical::abilities
