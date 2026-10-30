#include "alamo/tactical/abilities/ability_types.hpp"

#include <array>

#include "alamo/core/hash.hpp"

namespace alamo::tactical::abilities {

namespace {

// Builders for the rows of ABILITIES_HARDPOINTS.md 2.1.
constexpr AbilityTypeInfo row(const char* n, uint8_t m, int16_t b = kBehaviorNone, uint8_t x = 1, uint8_t u = 0, uint8_t f = 0) {
    return AbilityTypeInfo{n, m, b, x, u, f, 0};
}
// Constructor defaults (open item 1): M1 B-1 X1 U0 F0 apart from the explicit fields.
constexpr AbilityTypeInfo dflt(const char* n, int16_t b = kBehaviorNone, uint8_t u = 0) {
    return AbilityTypeInfo{n, 1, b, 1, u, 0, 1};
}

constexpr std::array<AbilityTypeInfo, kAbilitySlots> kTable = {{
    AbilityTypeInfo{"NONE", 0, kBehaviorNone, 0, 0, 0, 1},
    row("DEFEND", 1),
    row("DEPLOY", 1),
    row("INTERDICT", 1),
    dflt("HARASS"),
    row("BARRAGE", 1),
    // HUNT / LURE: B explicit, the rest the static-init defaults: M = 1, so "active" is the slot byte written
    // after 0x3a4070 toggled the behaviour (OI abilities, TACTICAL_GAPS §1.1 [H]).
    AbilityTypeInfo{"HUNT", 1, kBehaviorHunt, 1, 0, 0, 1},
    row("SPREAD_OUT", 1),
    dflt("AFTERBURNER"),
    row("TURBO", 1),
    AbilityTypeInfo{"LURE", 1, kBehaviorLure, 1, 0, 0, 1},
    row("ROCKET_ATTACK", 1),
    dflt("AVOID_DANGER"),
    row("FORCE_TELEKINESIS", 0, kBehaviorNone, 1, 0, 1),
    row("FORCE_WHIRLWIND", 0, kBehaviorNone, 1, 0, 1),
    row("FORCE_LIGHTNING", 0, kBehaviorNone, 1, 0, 1),
    row("FLAME_THROWER", 0),
    row("JET_PACK", 0),
    row("CAPTURE_VEHICLE", 1),
    row("TARGETED_INVULNERABILITY", 0),
    row("TARGETED_HACK", 0, kBehaviorNone, 1, 1, 0),
    row("TARGETED_REPAIR", 0, kBehaviorNone, 1, 1, 0),
    row("STICKY_BOMB", 0),
    row("AREA_EFFECT_STUN", 0),
    row("AREA_EFFECT_HEAL", 0),
    row("AREA_EFFECT_CONVERT", 0, kBehaviorNone, 1, 0, 1),
    row("DISTRACT", 0),
    row("CABLE_ATTACK", 0),
    row("FOW_REVEAL_PING", 0),
    row("POWER_TO_WEAPONS", 1),
    row("TRACTOR_BEAM", 1),
    row("ENERGY_WEAPON", 1),
    row("MISSILE_SHIELD", 1),
    row("INVULNERABILITY", 1),
    dflt("LUCKY_SHOT", kBehaviorNone, 1),
    row("HARMONIC_BOMB", 0),
    row("UNTARGETED_STICKY_BOMB", 0),
    row("CONCENTRATE_FIRE", 1),
    AbilityTypeInfo{"DEPLOY_TROOPERS", 0, kBehaviorDeployTroopers, 1, 0, 0, 1},
    AbilityTypeInfo{"SELF_DESTRUCT", 1, kBehaviorSelfDestruct, 0, 0, 0, 1},
    dflt("SPRINT"),
    dflt("ION_CANNON_SHOT", kBehaviorNone, 1),
    row("WEAKEN_ENEMY", 0),
    dflt("SPOILER_LOCK"),
    row("REPLENISH_WINGMEN", 0),
    dflt("MAXIMUM_FIREPOWER"),
    dflt("EJECT_VEHICLE_THIEF"),
    row("FIRE_LOBBING_SUPERWEAPON", 0),
    row("STEALTH", 1),
    row("STUN", 0),
    row("RADIOACTIVE_CONTAMINATE", 0),
    row("BERSERKER", 0, kBehaviorNone, 1, 0, 1),
    row("FORCE_SIGHT", 0, kBehaviorNone, 1, 0, 1),
    row("SABER_THROW", 0, kBehaviorNone, 1, 0, 1),
    row("FORCE_CLOAK", 1, kBehaviorNone, 1, 0, 1),
    row("SUPER_LASER", 1),
    row("LASER_DEFENSE", 0),
    row("FORCE_CONFUSE", 0, kBehaviorNone, 1, 0, 1),
    row("LEECH_SHIELDS", 0),
    row("TACTICAL_BRIBE", 0),
    row("SWAP_WEAPONS", 1),
    row("CLUSTER_BOMB", 0),
    row("FULL_SALVO", 1),
    row("SENSOR_JAMMING", 1),
    row("PLACE_REMOTE_BOMB", 0),
    row("DETONATE_REMOTE_BOMB", 0),
    row("INFECTION", 0),
    row("PROXIMITY_MINES", 0),
    dflt("STIM_PACK"),
    row("DRAIN_LIFE", 0, kBehaviorNone, 1, 0, 1),
    row("BLAST", 0),
    row("BUZZ_DROIDS", 0, kBehaviorNone, 1, 1, 0),
    row("SHIELD_FLARE", 0),
    row("SUMMON", 0),
    row("CORRUPT_SYSTEMS", 0),
    row("DEPLOY_SQUAD", 0),
    AbilityTypeInfo{"", 1, kBehaviorNone, 1, 0, 0, 1},  // 76: unused (command 40 accepts 1..76)
}};

struct StyleName {
    const char* name;
    ActivationStyle style;
};
constexpr StyleName kStyles[] = {
    {"Global_Automatic", ActivationStyle::GlobalAutomatic}, {"Combat_Automatic", ActivationStyle::CombatAutomatic},
    {"Galactic_Automatic", ActivationStyle::GalacticAutomatic}, {"Space_Automatic", ActivationStyle::SpaceAutomatic},
    {"Ground_Automatic", ActivationStyle::GroundAutomatic}, {"Ground_Activated", ActivationStyle::GroundActivated},
    {"Hero_Detected", ActivationStyle::HeroDetected}, {"Combat_Imminent", ActivationStyle::CombatImminent},
    {"Special_Attack", ActivationStyle::SpecialAttack}, {"Take_Damage", ActivationStyle::TakeDamage},
    {"User_Input", ActivationStyle::UserInput}, {"Skirmish_Automatic", ActivationStyle::SkirmishAutomatic},
};

struct ClassName {
    const char* tag;
    SpecialClass cls;
};
constexpr ClassName kClasses[] = {
    {"Tractor_Beam_Attack_Ability", SpecialClass::TractorBeam},
    {"Ion_Cannon_Shot_Attack_Ability", SpecialClass::IonCannonShot},
    {"Lucky_Shot_Attack_Ability", SpecialClass::LuckyShot},
    {"Concentrate_Fire_Attack_Ability", SpecialClass::ConcentrateFire},
    {"Energy_Weapon_Attack_Ability", SpecialClass::EnergyWeapon},
    {"Stealth_Ability", SpecialClass::Stealth},
    {"Sensor_Jamming_Ability", SpecialClass::SensorJamming},
    {"Laser_Defense_Ability", SpecialClass::LaserDefense},
    {"Redirect_Blaster_Ability", SpecialClass::RedirectBlaster},
    {"Absorb_Blaster_Ability", SpecialClass::AbsorbBlaster},
    {"Force_Sight_Ability", SpecialClass::ForceSight},  // fog follow-up (FOG_OF_WAR.md §3.3)
    // OI abilities
    {"Combat_Bonus_Ability", SpecialClass::CombatBonus},
    {"Enhance_Defense_Ability", SpecialClass::EnhanceDefense},
    {"Battlefield_Modifier_Ability", SpecialClass::BattlefieldModifier},
    {"Force_Healing_Ability", SpecialClass::ForceHealing},
    {"Berserker_Ability", SpecialClass::Berserker},
    {"Proximity_Mines_Ability", SpecialClass::ProximityMines},
    {"Earthquake_Attack_Ability", SpecialClass::Earthquake},
    {"Force_Telekinesis_Ability", SpecialClass::Telekinesis},
    {"Grenade_Attack_Ability", SpecialClass::GrenadeAttack},
    {"Force_Lightning_Ability", SpecialClass::ForceLightning},
    {"Force_Whirlwind_Ability", SpecialClass::ForceWhirlwind},
    {"Stun_Ability", SpecialClass::StunWave},
    {"Force_Confuse_Ability", SpecialClass::ForceConfuse},
    {"Arc_Sweep_Attack_Ability", SpecialClass::ArcSweep},
    {"Generic_Attack_Ability", SpecialClass::GenericAttack},
    {"Eat_Attack_Ability", SpecialClass::EatAttack},
    {"Super_Laser_Ability", SpecialClass::SuperLaser},
    {"Drain_Life_Ability", SpecialClass::DrainLife},
    {"Leech_Shields_Ability", SpecialClass::LeechShields},
    {"Blast_Ability", SpecialClass::Blast},
    {"Force_Cloak_Ability", SpecialClass::ForceCloak},
    {"Hack_Ability", SpecialClass::Hack},
    {"Repair_Ability", SpecialClass::Repair},
    {"Vehicle_Thief_Ability", SpecialClass::VehicleThief},
    {"Maximum_Firepower_Attack_Ability", SpecialClass::MaximumFirepower},
    {"Enable_Ability", SpecialClass::EnableAbility},
    {"Base_Power_Ability", SpecialClass::BasePower},
    {"Retreat_Prevention_Ability", SpecialClass::RetreatPrevention},
    {"Tactical_Bribe_Ability", SpecialClass::TacticalBribe},
    {"Corrupt_Systems_Ability", SpecialClass::CorruptSystems},
    {"Buzz_Droids_Ability", SpecialClass::BuzzDroids},
    {"Summon_Ability", SpecialClass::Summon},
    {"Spawn_Ability", SpecialClass::Spawn},
    {"Cluster_Bomb_Ability", SpecialClass::ClusterBomb},
    {"Remote_Bomb_Ability", SpecialClass::RemoteBomb},
    {"Personal_Flame_Thrower_Ability", SpecialClass::FlameThrower},
    {"Shield_Flare_Ability", SpecialClass::ShieldFlare},
    {"Infection_Ability", SpecialClass::Infection},
    {"Radioactive_Contaminate_Ability", SpecialClass::Radioactive},
    {"Saber_Throw_Ability", SpecialClass::SaberThrow},
    {"Permanent_Weapon_Swap_Ability", SpecialClass::WeaponSwap},
    {"Weatherproof_Ability", SpecialClass::Weatherproof},
    {"Cancel_Special_Ability", SpecialClass::CancelSpecial},
    {"Cable_Ability", SpecialClass::Cable},
    {"Starbase_Upgrade_Ability", SpecialClass::StarbaseUpgrade},
};

}  // namespace

const AbilityTypeInfo& ability_type_info(uint32_t type) { return kTable[type < kAbilitySlots ? type : 0u]; }

uint8_t ability_type_from_name(std::string_view name) {
    for (uint32_t i = 1; i <= kMaxAbilityType; ++i)
        if (iequals(kTable[i].name, name)) return static_cast<uint8_t>(i);
    return 0;
}

ActivationStyle activation_style_from_name(std::string_view name) {
    for (const StyleName& s : kStyles)
        if (iequals(s.name, name)) return s.style;
    return ActivationStyle::None;
}

bool style_allowed(ActivationStyle style, GameMode mode, bool skirmish) {
    const bool tactical = mode == GameMode::Land || mode == GameMode::Space;
    switch (style) {
        case ActivationStyle::GlobalAutomatic: return true;
        case ActivationStyle::CombatAutomatic:
        case ActivationStyle::SpecialAttack:
        case ActivationStyle::TakeDamage:
        case ActivationStyle::UserInput: return tactical;
        case ActivationStyle::GalacticAutomatic:
        case ActivationStyle::GroundActivated:
        case ActivationStyle::HeroDetected:
        case ActivationStyle::CombatImminent: return mode == GameMode::Galactic;
        case ActivationStyle::SpaceAutomatic: return mode == GameMode::Space;
        case ActivationStyle::GroundAutomatic: return mode == GameMode::Land;
        case ActivationStyle::SkirmishAutomatic: return skirmish;
        case ActivationStyle::None: break;
    }
    return false;
}

bool style_automatic(ActivationStyle style) {
    return style == ActivationStyle::GlobalAutomatic || style == ActivationStyle::CombatAutomatic ||
           style == ActivationStyle::SpaceAutomatic || style == ActivationStyle::GroundAutomatic ||
           style == ActivationStyle::SkirmishAutomatic;
}

SpecialClass special_class_from_tag(std::string_view element_name) {
    for (const ClassName& c : kClasses)
        if (iequals(c.tag, element_name)) return c.cls;
    return SpecialClass::Unsupported;
}

const char* special_class_name(SpecialClass c) {
    for (const ClassName& n : kClasses)
        if (n.cls == c) return n.tag;
    return "Unsupported";
}

int32_t special_service_interval(SpecialClass c, uint32_t fps) {
    // OI abilities: vfunc +0x18 per class (TACTICAL_GAPS §1.4) [H].
    using S = SpecialClass;
    switch (c) {
        case S::Unsupported:  // galactic economy / corruption / political classes and unknown tags
        case S::CombatBonus:
        case S::EnhanceDefense:
        case S::BattlefieldModifier:
        case S::RedirectBlaster:
        case S::AbsorbBlaster:
        case S::EnableAbility:
        case S::ClusterBomb:
        case S::VehicleThief:
        case S::WeaponSwap:
        case S::BasePower:
        case S::CancelSpecial:
        case S::Weatherproof:        // [M: an aura class like Base_Power; not in the §1.4 list]
        case S::RetreatPrevention:   // [M: same]
            return 0;
        case S::StarbaseUpgrade:     // STATION_HANGARS_SOUND.md §1.1: interval 2, Skirmish_Automatic, one-shot service
            return 2;
        case S::Earthquake:
        case S::Telekinesis: return static_cast<int32_t>(fps / 10u);
        default: return 1;
    }
}

}  // namespace alamo::tactical::abilities
