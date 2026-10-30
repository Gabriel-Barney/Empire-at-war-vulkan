// P4 sim (owner: galactic agent): data tables of the FoC galactic special systems (docs/re/P4_SIM_SYSTEMS.md = P4):
// BlackMarketItems.xml, the galactic special abilities of the object types' <Abilities> lists (Black_Market_Ability,
// Galactic_Sabotage_Ability, Hack_Super_Weapon_Ability, Corruption_Ability, Planet_Income_Bonus_Ability,
// Planet_Income_Gambling_Ability), the per-type fields those systems read and the GameConstants they use.
// Loaded once by load_special_rules (galactic_rules.hpp load_galactic_rules calls it) into GalacticRules::special;
// the runtime and the public API are alamo/galactic/special_systems.hpp.
//
// Names: every ability / item name keeps its spelling; name CRCs are special_name_crc = CRC-32 over the bytes as
// written (P4 1.1 "0x215a30 over the bytes as written", 1.1 "no upper-casing").
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/galactic/economy.hpp"

namespace alamo::galactic {

// CRC-32 (IEEE) over the bytes as written, no case folding (P4 1.1). Ability / item name CRCs, income source tags.
uint32_t special_name_crc(std::string_view name);

// Activation_Style enum (ABILITIES_HARDPOINTS 3.1) [H]: 0 = unset.
enum class ActivationStyle : uint8_t {
    None = 0, GlobalAutomatic = 1, CombatAutomatic = 2, GalacticAutomatic = 3, SpaceAutomatic = 4, GroundAutomatic = 5,
    GroundActivated = 6, HeroDetected = 7, CombatImminent = 8, SpecialAttack = 9, TakeDamage = 10, UserInput = 11,
    SkirmishAutomatic = 12
};
ActivationStyle parse_activation_style(std::string_view text);  // case-insensitive, None if unknown

enum class SpecialAbilityClass : uint8_t {
    Other = 0,          // any other <Abilities> entry (kept for list order, never applied by the galactic sim)
    BlackMarket = 1,    // Black_Market_Ability (P4 1.2)
    Sabotage = 2,       // Galactic_Sabotage_Ability (P4 2.1)
    Hack = 3,           // Hack_Super_Weapon_Ability (P4 3.2)
    Corruption = 4,     // Corruption_Ability (P4 5.1)
    IncomeBonus = 5,    // Planet_Income_Bonus_Ability (P4 4.1)
    IncomeGambling = 6, // Planet_Income_Gambling_Ability (P4 4.2)
    Slicer = 7,         // Slicer_Ability (REMAINING_GAPS 1.1: R2D2_Slicer)
    SystemSpy = 8,      // System_Spy_Ability (GRC 2.3 / 2.3.1: planet visibility modifiers)
};

struct SpecialAbilityDef {
    SpecialAbilityClass cls = SpecialAbilityClass::Other;
    std::string tag;                    // XML element name (class)
    std::string name;                   // Name attribute
    uint32_t name_crc = 0;              // special_name_crc(name) (special-ability +0x38)
    ActivationStyle style = ActivationStyle::None;
    bool initially_enabled = true;      // Initially_Enabled; default [inferred] Yes (the income abilities never set it)
    bool causes_despawn = false;        // Causes_Despawn (+0x41)
    float min_respawn = 0.0f;           // Min_Respawn_Time (+0x54), default 0 [M] (P4 8 item 4)
    float max_respawn = 0.0f;           // Max_Respawn_Time (+0x58)
    bool must_be_bought = false;        // Must_Be_Bought_On_Black_Market (+0x168)
    // Black_Market_Ability (P4 1.2)
    std::vector<float> bm_min_respawn, bm_max_respawn;  // Min/Max_Respawn_Times, index = tech level - 1 (cleared when min > max)
    float price_modifier = 1.0f;        // Price_Modifier
    int32_t tech_level_adjustment = 0;  // Tech_Level_Adjustment (parsed; no reader found [M])
    // Galactic_Sabotage_Ability (P4 2.1): option flags 0..4 in option order
    std::array<bool, 5> sabotage_can{};  // Can_Reduce_Ground_Base_Level, _Starbase_Level, Can_Cancel_Production,
                                         // Can_Destroy_Special_Structure, Can_Halt_Credit_Production
    float halt_duration = 0.0f;          // Duration_Of_Credit_Halt (10.0 when halting is on and <= 0; -1 = permanent)
    float cost_increase_factor = 2.0f;   // Sabotage_Cost_Increase_Factor
    // Planet_Income_Bonus_Ability (P4 4.1)
    float income_percentage = 0.0f;      // Percentage_Income_Modifier (a fraction, >= -1; smaller -> 0 with a diagnostic)
    float income_absolute = 0.0f;        // Absolute_Income_Modifier
    float mining_multiplier = 1.0f;      // Additional_Multiplier_On_Mining_Colonies
    std::string source_text;             // Specific_Mod_Source_Text
    uint32_t source_tag = 0;             // special_name_crc(source_text), 0 = empty (the owner's Text_ID CRC is used)
    // Planet_Income_Gambling_Ability (P4 4.2)
    float evaluation_interval = 0.0f;    // Evaluation_Interval_In_Secs (< 0 -> 0)
    float min_income_modifier = 0.0f;    // Min_Income_Modifier (-1..0)
    float max_income_modifier = 0.0f;    // Max_Income_Modifier (>= 0)
    float chance_to_win = 0.0f;          // Chance_To_Win (0..1)
    // Slicer_Ability (REMAINING_GAPS 1.1). Respawn lists: index = tech level (Min/Max_Respawn_Time_Per_Tech_Level);
    // curves keep >= 2 points (loader post-check 0x619980).
    std::vector<float> slice_min_respawn, slice_max_respawn;
    PiecewiseCurve chance_by_base_level;       // Chance_Modifier_By_Base_Level (+0x1a8)
    PiecewiseCurve chance_by_difficulty;       // Chance_Modifier_By_Planet_Difficulty (+0x1d0)
    PiecewiseCurve cost_by_tech_availability;  // Cost_Multiplier_By_Planet_Tech_Availability (+0x1f8)
    float chance_affinity = 0.0f;              // Chance_Affinity_Modifier (+0x220)
    float cost_affinity = 0.0f;                // Cost_Affinity_Modifier (+0x224)
    // System_Spy_Ability (GRC 2.3.1, load-time 0x70fd20): the See_* flags as a VisibilityLevelType mask (never bit 0)
    // after the prerequisite fix-ups, and Duration_In_Secs after the style fix-ups (Galactic_Automatic -> -1,
    // Ground_Activated <= 0 -> 30; any other style -> style None).
    uint32_t spy_mask = 0;                     // ability +0x18C
    float spy_duration = 0.0f;                 // Duration_In_Secs
};

// BlackMarketItems.xml entry (P4 1.1), table order = file order.
struct BlackMarketItemDef {
    std::string name;
    uint32_t name_crc = 0;               // +0xB8: identifies the item in commands and saves
    float uncommon_price_multiplier = 0.0f;
    uint32_t faction = 0xFFFFFFFFu;      // GalacticRules::factions index, kNoIndex = none
    int32_t tech_level = 1;
    std::vector<uint32_t> ability_crcs;  // Ability_Names, special_name_crc each
    std::vector<std::string> ability_names;
    int32_t base_cost = 0;               // Base_Cost_Credits
    int32_t tutorial_tech_level = 0;     // Tutorial_Tech_Level (not set by the ctor [L]: 0)
    int32_t tutorial_base_cost = 0;      // Tutorial_Base_Cost_Credits ([L]: 0)
    std::string icon, localized_name, localized_description;  // UI
    std::vector<std::string> used_by_units;                     // UI only [M]
};

// One corruption choice of a planet type (Corruption_N_*, P4 5.1 record 0x52c570), slot order 1, 2, 3.
struct CorruptionChoiceDef {
    uint32_t slot = 0;
    int8_t type = -1;                            // CorruptionTypeEnum 0..7
    float credit_cost = 0.0f;                    // Corruption_N_Credit_Cost (float)
    std::vector<uint32_t> required_unit_types;   // Corruption_N_Required_Unit_Type (type indices, a union)
    int32_t required_unit_count = 0;
    int8_t required_mission = -1;                // Corruption_N_Required_Mission_Type
    int32_t required_mission_count = 0;
    std::vector<uint32_t> required_planets;      // Corruption_N_Required_Planets (planet type indices)
    uint32_t success_unlock = 0xFFFFFFFFu;       // Corruption_N_Success_Unlock_Unit_Type (REMAINING_GAPS 1.5 item 5)
};

// Per object type (index = TypeIndex). Default-constructed for every type without these fields.
struct TypeSpecial {
    std::vector<uint32_t> abilities;     // SpecialRules::abilities indices, <Abilities> list order; a team without its own
                                         // list uses its first Company_Units member's [inferred] (P4 1.5 "first member")
    bool has_income_ability = false;     // an IncomeBonus / IncomeGambling entry in `abilities`
    std::vector<uint32_t> members;       // Company_Units (type indices)
    int32_t sabotage_cost = 0;           // Sabotage_Cost_Credits (T +0x870)
    int32_t tutorial_sabotage_cost = 0;  // Tutorial_Sabotage_Cost_Credits (T +0x874)
    bool planet_vigilance = false;       // Planet_Vigilance (T +0xC0)
    bool mining_colony = false;          // Planet_Is_Mining_Colony (T +0xBE)
    float slice_difficulty = 0.0f;       // Planet_Slice_Difficulty_Rating (T +0x7d4, read as float)
    float tech_availability = 0.0f;      // Planet_Tech_Availability_Rating (T +0x7d8, float)
    std::vector<uint32_t> slice_affinity;  // Planet_Slice_Affinity_Types (T +0x7e8, resolved)
    float slice_cost = 0.0f;             // Slice_Cost_Credits (T +0x868, float)
    float black_market_modifier = 0.0f;  // Planet_Black_Market_Modifier (T +0x800, read as float)
    bool destroy_when_prereq = false;    // Destroy_When_Used_As_Corruption_Prereq (T +0xE4)
    bool generic_hero = false;           // Is_Generic_Hero (T +0x14), own or first member's
    bool named_hero = false;             // Is_Named_Hero (T +0x13), own or first member's
    uint32_t prev_level_base = 0xFFFFFFFFu;  // Prev_Level_Base (T +0x8B0)
    uint32_t text_crc = 0;               // special_name_crc(Text_ID) (T +0x120 [M]): fallback income source tag
    std::vector<CorruptionChoiceDef> corruption;  // planets: non-empty slots, slot order
    // OI galactic (STRATEGIC_GAPS 2.2): an Initially_Enabled Galactic_Stealth_Ability (Galactic_Automatic: active from
    // creation). A container without its own entry: every Company_Units member has one (a transport is hidden only when
    // all its cargo is).
    bool galactic_stealth = false;
    float detection_chance = -1.0f;  // DetectHeroAbility-style Detection_Chance (ability +0x17C), -1 = no detector
    // GRC 2.3: OR of the spy masks of this type's Initially_Enabled Galactic_Automatic System_Spy_Ability entries (the
    // always-on spies: smugglers, bounty hunters, OLRS, palaces, Bothawui). Derived at load from `abilities`.
    uint32_t auto_spy_mask = 0;
};

struct SpecialRules {
    std::vector<SpecialAbilityDef> abilities;  // every galactic-relevant <Abilities> entry, type order then list order
    std::vector<BlackMarketItemDef> items;     // BlackMarketItems.xml, file order
    std::vector<TypeSpecial> types;            // index = TypeIndex (empty in hand-built rules: every lookup defaults)
    // GameConstants
    float default_hero_respawn_time = 360.0f;  // Default_Hero_Respawn_Time (+0x11F0). Thrawn's Revenge: -360 (P4 6.2)
    uint32_t hack_required_type = 0xFFFFFFFFu; // Hack_Super_Weapon_Required_Type (+0x12040), kNoIndex = none (TR)
    float hack_cost = 50000.0f;                // Hack_Super_Weapon_Cost (+0x12140)
    std::string hack_particle;                 // Hack_Super_Weapon_Particle_Effect (presentation)
    std::string sabotage_particle;             // Sabotage_Particle_Effect (presentation)
    std::vector<std::string> activated_black_market, activated_sabotage, activated_corrupt, activated_hack;

    const TypeSpecial& type(uint32_t t) const;  // a static default for unknown / missing types
    uint32_t find_item(uint32_t name_crc) const;  // table index (first match), 0xFFFFFFFF if absent (0x48f550)
    uint32_t find_item_by_name(std::string_view name) const;  // exact name (GIVE_BLACK_MARKET 0x7E: length + memcmp)
};

}  // namespace alamo::galactic
