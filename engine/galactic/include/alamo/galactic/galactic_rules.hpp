// S3 rule tables: GameConstants, factions, difficulty adjustments and the per-type fields the galactic
// economy reads, extracted once from engine/data + engine/objects (or built by hand in tests).
//
// TypeInfo is indexed by objects::TypeIndex (the GameObjectTypes index), so object types in the galactic
// ObjectStore and the 16-bit type index of the Production event map onto it directly.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/galactic/special_data.hpp"  // P4 sim

namespace alamo::data {
class GameDatabase;
}
namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::galactic {

inline constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

// How a type takes part in the galaxy. The original classifies by category flags (GE 2.1 / 2.5:
// 0x5c starbase, 0x5e/0x60/4/0x61 starbase upgrade families, 0x5f ground structure) [M]; the port
// derives them from the XML element kind (StarBase, GroundStructure/GroundBase/SpecialStructure,
// Squadron, ground companies/infantry/vehicles) and, since q6 t03, from the DUMMY_* behaviours those
// flags turned out to be (galactic_unit_kind below).
enum class UnitKind : uint8_t {
    Other = 0,
    SpaceUnit = 1,        // ships, space heroes: live in fleets
    Squadron = 2,         // fighters/bombers: carried when holds allow (GE 3.1 step 2)
    GroundUnit = 3,       // companies, infantry, vehicles, ground heroes: planet surface
    GroundStructure = 4,  // ground base buildings: raise the ground-base level
    StarBase = 5,         // starbase (Base_Level 1 = new base, > 1 = upgrade)
    Planet = 6,
};

struct TypeInfo {
    std::string name;
    UnitKind kind = UnitKind::Other;
    int32_t build_cost = 0;             // Build_Cost_Credits
    int32_t build_time_seconds = 0;     // Build_Time_Seconds
    int32_t tech_level = 0;             // Tech_Level
    int32_t required_ground_base = 0;   // Required_Ground_Base_Level
    int32_t required_star_base = 0;     // Required_Star_Base_Level
    int32_t base_level = 0;             // Base_Level (starbases)
    int32_t advances_tech = 0;          // Build_Advances_Tech_Level (tech upgrades)
    int32_t squadron_capacity = 0;      // Squadron_Capacity
    bool hyperspace = false;            // Hyperspace
    float hyperspace_speed = 0.0f;      // Hyperspace_Speed
    float influence_range = 0.0f;       // Galactic_Influence_Range (type +0x1a00; REMAINING_GAPS 6.1 launch reveal)
    float piracy_value = 0.0f;          // Piracy_Value_Credits
    float ai_combat_power = 0.0f;       // AI_Combat_Power
    float maintenance_cost = 0.0f;      // Maintenance_Cost (+0x798, default 0, variants inherit; AI upkeep tokens, A2H_GALACTIC_AI_RE §3)
    bool reduced_by_factories = false;  // Build_Time_Reduced_By_Multiple_Factories
    bool build_initially_locked = false;  // Build_Initially_Locked (type +0xC1)
    bool sliceable = false;               // Build_Can_Be_Unlocked_By_Slicer (type +0xC2; REMAINING_GAPS 1.4 sweep)
    std::vector<uint32_t> required_special_structures;  // type indices
    std::vector<uint32_t> affiliation;  // faction indices (empty = any) [M buildability]
    // A2 galactic (AI_GALACTIC_3 Â§3, Â§6).
    int32_t population = 0;              // Population_Value = pop(T, 0)
    int32_t population_capacity = 0;     // Additional_Population_Capacity
    uint64_t categories = 0;             // CategoryMask
    int32_t movement_class = -1;         // MovementClass enum id (a company: its first Company_Units member's), -1 none
    int32_t terrain = -1;                // planets: Terrain (kTerrainNames index), -1 none
    uint64_t restricted_categories = 0;  // planets: Planet_Restricted_Unit_Categories
    std::vector<uint32_t> restricted_types;  // planets: Planet_Restricted_Unit_Types (type indices)
    // Planet types.
    int32_t credit_value = 0;            // Planet_Credit_Value
    int32_t destroyed_credit_value = 0;  // Planet_Destroyed_Credit_Value
    int32_t capture_bonus = 0;           // Planet_Capture_Bonus_Reward
    bool black_market_influences = false;  // Planet_Black_Market_Influences (type +0xbd)
    bool starbase_stealth = false;       // Planet_Starbase_Stealth (type +0xbb; REMAINING_GAPS 6.3, display only)
    bool groundbase_stealth = false;     // Planet_Groundbase_Stealth (type +0xbc; display only)
    bool surface_accessible = true;      // Planet_Surface_Accessible
    ForceSide the_force = ForceSide::Neutral;  // The_Force
    float force_strength = 0.0f;         // Force_Strength

    // ---- OI galactic (docs/re/STRATEGIC_GAPS.md = SG) -----------------------------------------------------------
    // Defaults are the hand-built-rules values (tests): no dummy behaviour, no limits, unlimited slots. The data path
    // (extract_type_info) fills every field from the typed view.
    uint8_t dummy = 0;                   // DUMMY_* behaviour id (kDummy* below), 0 = none (classify by `kind`)
    bool data_rules = false;             // filled from data: Affiliation is enforced (SG 1.3 step 2); hand-built: empty = any
    uint32_t company_transport = kNoIndex;  // Company_Transport_Unit[0] (type +0x938), SG 1.7
    std::vector<uint32_t> company_units;    // Company_Units (resolved, list order, duplicates kept)
    bool company_member = false;         // listed in some type's Company_Units (+0x2A)
    bool squadron_member = false;        // listed in some type's Squadron_Units (+0x29, SG 1.2)
    bool transport_behavior = false;     // TRANSPORT behaviour (slot 5)
    bool cinematic_only = false;         // Cinematic_Object_Only (+0x21)
    bool always_spawn_in_orbit = false;  // Always_Spawn_In_Orbit (+0x20)
    bool super_weapon_killer = false;    // Is_Super_Weapon_Killer (+0x1F)
    bool planet_destruction = false;     // has a Planet_Destruction_Ability (the galactic super weapon, SG 1.4) [port]
    bool named_hero = false;             // Is_Named_Hero (+0x13)
    bool generic_hero = false;           // Is_Generic_Hero (+0x14)
    // Required_Special_Structures as OR chains (SG 1.3 step 14): `A | B, C` = {{A, B}, {C}}; kNoIndex = an unresolvable
    // name (passes its chain). `required_special_structures` above keeps the flat resolved list (factory count).
    std::vector<std::vector<uint32_t>> required_special_groups;
    std::vector<uint32_t> required_orbiting_units;  // Required_Orbiting_Units (resolved)
    std::vector<uint32_t> required_planets;         // Required_Planets (planet type indices)
    int32_t max_instances_per_planet = -1;  // Build_Max_Instances_Per_Planet (-1 = unlimited)
    int32_t limit_lifetime = -1;            // Build_Limit_Lifetime_Per_Player (-1 unlimited, 0 never)
    int32_t limit_current = -1;             // Build_Limit_Current_Per_Player (-1 unlimited, 0 never)
    bool requires_initial_placement = false;  // Build_Requires_Initial_Placement (+0xC3)
    // Planet types.
    int32_t max_space_base = 5;             // Max_Space_Base (type +0x668)
    int32_t ground_structure_slots = -1;    // Special_Structures_Land (+0x92C), -1 = unlimited
    int32_t orbital_structure_slots = -1;   // Special_Structures_Space (+0x928), -1 = unlimited
};

// DUMMY_* behaviour ids (behaviors.hpp: 0x5B..0x69 in doc order; SG 1.2).
inline constexpr uint8_t kDummyGroundBase = 0x5B, kDummyStarBase = 0x5C, kDummyGroundCompany = 0x5D,
                         kDummyFighterSquadron = 0x5E, kDummyGroundStructure = 0x5F, kDummyOrbitalStructure = 0x60,
                         kDummyStarship = 0x61;
// Classification used by production and buildability: the dummy behaviour when known, else the port's UnitKind.
inline bool is_ground_structure_type(const TypeInfo& t) {
    return t.dummy != 0 ? t.dummy == kDummyGroundStructure : t.kind == UnitKind::GroundStructure;
}
inline bool is_orbital_structure_type(const TypeInfo& t) { return t.dummy == kDummyOrbitalStructure; }
inline bool is_star_base_type(const TypeInfo& t) {
    return t.dummy != 0 ? t.dummy == kDummyStarBase : t.kind == UnitKind::StarBase;
}

// Galactic role (q6 t03). The element tag carries no behaviour in the original (OBJECT_TYPES §2); galactic code tests
// the DUMMY_* behaviours (SG 1.2, AI_GALACTIC_TOKENS). The port's older element-tag table stays for the vanilla tags.
//   unit_kind_from_element: the tag table alone (Other for a tag outside it).
//   unit_kind_from_dummy:   DUMMY_STAR_BASE -> StarBase, DUMMY_GROUND_BASE / _GROUND_STRUCTURE / _ORBITAL_STRUCTURE ->
//                           GroundStructure, DUMMY_GROUND_COMPANY -> GroundUnit, DUMMY_SPACE_FIGHTER_SQUADRON ->
//                           Squadron, DUMMY_STARSHIP -> SpaceUnit; Other for 0.
//   galactic_unit_kind:     a tag in the table takes the behaviour's role when it has one, else the table's (no vanilla
//                           FoC / EaW type has a behaviour that disagrees with its tag); a tag outside the table takes
//                           the behaviour's role, except the vanilla tags whose vanilla types the port always left Other
//                           (is_vanilla_unclassified_element), so vanilla data maps exactly as before.
UnitKind unit_kind_from_element(std::string_view element, bool hyperspace);
UnitKind unit_kind_from_dummy(uint8_t dummy);
bool is_vanilla_unclassified_element(std::string_view element);
UnitKind galactic_unit_kind(std::string_view element, bool hyperspace, uint8_t dummy);

// Terrain enum (AI_GALACTIC_3 Â§6.1, BATTLE_TRANSITIONS map picker order): Temperate 0 .. Urban 6, Space 7.
inline constexpr const char* kTerrainNames[] = {"Temperate", "Arctic", "Desert", "Forest", "Swamp", "Volcanic", "Urban", "Space"};
int32_t terrain_id(std::string_view name);  // case-insensitive, -1 unknown

// Data\XML\AITerrainEffectiveness.xml (AI_GALACTIC_3 Â§6.2): value[terrain][movement class]; missing = 1.0.
struct TerrainEffectiveness {
    std::vector<std::vector<double>> value;   // [terrain id][MovementClass id]; NaN-free, absent entries = 1.0
    double get(int32_t terrain, int32_t movement_class) const;
};

struct GalacticRules {
    EconomyConstants constants;
    TerrainEffectiveness terrain_effectiveness;  // A2 galactic
    std::vector<FactionRules> factions;        // FactionFiles.xml order
    std::vector<DifficultyRules> difficulties; // DifficultyAdjustments.xml order
    std::vector<TypeInfo> types;               // index = objects::TypeIndex
    SpecialRules special;                      // P4 sim: black market, sabotage, hack, income abilities, corruption choices

    uint32_t find_faction(std::string_view name) const;     // case-insensitive, kNoIndex if absent
    uint32_t find_difficulty(std::string_view name) const;
    uint32_t find_type(std::string_view name) const;        // first definition
    uint32_t neutral_faction() const;                       // first Is_Neutral faction, kNoIndex if none
};

// GameConstants -> EconomyConstants (fields absent from the XML keep the vanilla defaults).
EconomyConstants load_economy_constants(const data::GameDatabase& db);
// Factions (Data\XML\FactionFiles.xml, list order) and Data\XML\DifficultyAdjustments.xml.
std::vector<FactionRules> load_factions(const FileProvider& provider, std::vector<std::string>* diagnostics = nullptr);
std::vector<DifficultyRules> load_difficulties(const FileProvider& provider,
                                               std::vector<std::string>* diagnostics = nullptr);
// Per-type fields from the typed view; `factions` resolves Affiliation names.
std::vector<TypeInfo> extract_type_info(const objects::GameObjectTypes& types,
                                        const std::vector<FactionRules>& factions);
// AITerrainEffectiveness.xml (missing file = empty table = 1.0 everywhere). MovementClass names resolve through the
// typed view's MovementClassType enum.
TerrainEffectiveness load_terrain_effectiveness(const FileProvider& provider, const objects::GameObjectTypes& types,
                                                std::vector<std::string>* diagnostics = nullptr);
// P4 sim: BlackMarketItems.xml, the types' galactic special abilities and per-type fields, the special GameConstants
// (special_data.hpp). Needs out.factions and out.types already filled.
void load_special_rules(const FileProvider& provider, const data::GameDatabase& db, const objects::GameObjectTypes& types,
                        GalacticRules& out, std::vector<std::string>* diagnostics = nullptr);
// All of the above.
bool load_galactic_rules(const FileProvider& provider, const data::GameDatabase& db,
                         const objects::GameObjectTypes& types, GalacticRules& out,
                         std::vector<std::string>* diagnostics = nullptr);

}  // namespace alamo::galactic
