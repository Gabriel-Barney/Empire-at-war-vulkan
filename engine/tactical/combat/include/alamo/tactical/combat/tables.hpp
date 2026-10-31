// S8 - data tables read by combat: the damage-vs-armor matrix (TACTICAL_COMBAT.md 3.2) and the
// targeting priority sets (2.4). Both are loaded once and read-only during the simulation.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::data {
struct GameConstants;
class XmlNode;
}  // namespace alamo::data
namespace alamo::objects {
class EnumRegistry;
}

namespace alamo::tactical::combat {

inline constexpr uint16_t kNoTypeId = 0xFFFF;

// ---- Damage_To_Armor_Mod (section 3.2) [H] ----------------------------------------------------
// <Damage_Types> / <Armor_Types> name lists give dense indices (the original keys them by CRC32 of
// the name); cells come from <Damage_To_Armor_Mod> damage, armor, factor </...> lines. Lookup: an
// unknown type (kNoTypeId) or a negative / unset cell gives 1.0.
class DamageArmorTable {
public:
    uint16_t add_damage_type(std::string_view name);  // returns the index (existing name: its index)
    uint16_t add_armor_type(std::string_view name);
    uint16_t damage_index(std::string_view name) const;  // case-insensitive; kNoTypeId if unknown
    uint16_t armor_index(std::string_view name) const;
    void set(uint16_t damage, uint16_t armor, float factor);
    float factor(uint16_t damage, uint16_t armor) const;

    uint32_t damage_count() const { return static_cast<uint32_t>(damage_names_.size()); }
    uint32_t armor_count() const { return static_cast<uint32_t>(armor_names_.size()); }
    const std::string& damage_name(uint16_t i) const { return damage_names_[i]; }
    const std::string& armor_name(uint16_t i) const { return armor_names_[i]; }

    // GameConstants loader: Damage_Types, Armor_Types, every Damage_To_Armor_Mod occurrence in
    // document order (a later line overwrites a cell). Names in a mod line that are not in the
    // lists are added. Malformed lines are reported in `diagnostics`.
    static DamageArmorTable from_constants(const data::GameConstants& gc, std::vector<std::string>* diagnostics = nullptr);

private:
    void grow();
    std::vector<std::string> damage_names_, armor_names_;  // upper-cased
    std::vector<float> cells_;                             // [damage][armor], -1 = unset
    uint32_t stride_ = 0;
};

// ---- Targeting_Priority_Sets (section 2.4) [H] ------------------------------------------------
struct PriorityEntry {
    enum class Kind : uint8_t { Type, Category, Property };
    Kind kind = Kind::Type;
    int32_t type = -1;   // Kind::Type: object type index (host's TypeIndex)
    uint64_t mask = 0;   // Category / Property mask
    float weight = 0.0f;
};

struct PrioritySet {
    std::string name;
    std::vector<PriorityEntry> attack;       // XML order
    std::vector<int32_t> unit_exclusions;    // type indices
    uint64_t category_exclusions = 0;
    uint64_t property_exclusions = 0;
    // refixf t06: Hard_Point_Priorities / Hard_Point_Exclusions are HardPointType values (field type 0x45, the enum-list
    // parser also used by Victory_Conditions; the shipped sets list "Shield_Generator, Engine, FIGHTER_BAY, ..."), not
    // hardpoint names: list order, values as HardpointKind (combat_system.hpp).
    std::vector<int32_t> hardpoint_priorities;
    std::vector<int32_t> hardpoint_exclusions;
};

// PrioritySet_Score (0x54dc00). LOWER is better; -1 = excluded. Order:
//   1. a Type entry naming the target's exact type returns its weight immediately;
//   2. running value 1e18, min over Category entries hitting the target CategoryMask;
//   3. Property entries hitting Property_Flags: the FIRST hit overwrites the running value, later
//      hits take the minimum (XML order dependent);
//   4. Unit_Exclusions -> -1;  5. Category_Exclusions -> -1 unless a property entry matched;
//   6. Property_Exclusions -> -1.
float priority_score(const PrioritySet& set, int32_t target_type, uint64_t target_categories, uint64_t target_properties);

// PrioritySet_Hardpoint_Rank (0x54dd80): 1-based position of the hardpoint's TYPE in Hard_Point_Priorities, -1 if
// listed in Hard_Point_Exclusions, otherwise 1e18.
float hardpoint_rank(const PrioritySet& set, int32_t hardpoint_type);

// HardPointType name -> value (HardpointKind order: Engine 0 ... Dummy_Art 12), -1 if unknown. Case-insensitive; the
// "HARD_POINT_" prefix is optional [M: the HardPoint XML <Type> uses HARD_POINT_SHIELD_GENERATOR, the priority sets, the
// perception equations (HardPointType[Shield_Generator]) and the space plans' Attack_Target use the short form; the
// converter's name table was not traced, both forms are accepted].
int32_t hardpoint_type_value(std::string_view name);

// Loader for a <Targeting_Priority_Sets> document root. Names in Attack_Priorities are resolved as
// object type first (resolve_type, may be empty), then GameObjectCategoryType, then
// GameObjectPropertiesType through `enums`; unknown names are reported and skipped. Weights follow
// their name ("Transport, 1.0,"). Hard-point lists are HardPointType names (hardpoint_type_value; unknown ones are
// reported and skipped).
using TypeResolver = std::function<int32_t(std::string_view name)>;  // -1 if unknown
std::vector<PrioritySet> load_priority_sets(const data::XmlNode& root, const objects::EnumRegistry& enums,
                                            const TypeResolver& resolve_type,
                                            std::vector<std::string>* diagnostics = nullptr);

}  // namespace alamo::tactical::combat
