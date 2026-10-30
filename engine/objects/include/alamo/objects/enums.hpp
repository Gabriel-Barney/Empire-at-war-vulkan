// Enum and bitmask value converters of the GameObjectType loader (docs/re/OBJECT_TYPES.md §9.1).
//
// Three kinds, all with the same string rules (value trimmed of space/tab/CR/LF, then upper-cased;
// inner spaces significant):
//   * dynamic enum      - Data\XML\Enum\<Name>.xml, values C atoi, empty text = running entry index;
//   * dynamic bitfield  - Data\XML\Enum\<Name>.xml, values sscanf("%llx") (always hex, "0x" optional),
//                         empty text = 64 - entry index [M], more than 64 entries discards the converter;
//   * static enum / bitfield - name/value pairs compiled into the exe (§9.1.3).
// Dynamic converters: a duplicate name or a duplicate value discards the whole converter (every
// later conversion fails). Static tables are taken as listed.
// Enum fields convert the whole value as one name. Bitfield fields split on '|', space, comma, tab
// and LF (collapsed); each token is trimmed and looked up; ONE unknown token fails the whole value
// (the field keeps its previous value); a value of delimiters only is mask 0.
//
// Without an Enum file (no provider, or the file is missing) the dynamic converters fall back to
// the vanilla FoC tables of §9.1.1 / §9.1.2 (SurfaceFXTriggerType has no built-in table: absent).
//
// Two codes do not use a converter object: Ship_Class (0x32, _stricmp table, unknown = unchanged,
// no log) and Projectile_Category (0x4a, upper-cased exact compare, unknown = -1).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"

namespace alamo::objects {

enum class EnumId : uint8_t {
    // Dynamic (Data\XML\Enum).
    GameObjectCategory,   // bitfield
    GameObjectProperties, // bitfield
    MovementClass,
    ObjectWeatherCategory,
    SurfaceFXTrigger,
    // Static (§9.1.3).
    SpaceLayer,  // bitfield
    UnitCollisionClass,
    UnitOccupation,
    CellPassability,  // bitfield
    ContainerArrangement,
    ForceAlignment,
    FormationFormupWait,
    FormationGrouping,
    VisibilityLevel,  // bitfield
    DamageType,
    ProductionQueue,
    Sellable,
    CorruptionType,
    SubGameMode,
    Count
};

class EnumConverter {
public:
    struct Entry {
        std::string name;  // normalised (trimmed, upper-cased)
        int64_t value = 0;
    };
    std::string_view enum_name;  // e.g. "GameObjectCategoryType"
    bool bitfield = false;
    bool dynamic = false;
    bool valid = false;            // false: absent or discarded -> every conversion fails
    bool from_file = false;        // dynamic: loaded from Data\XML\Enum (else built-in fallback)
    std::vector<Entry> entries;    // registration order

    // Registers one entry; false (and the converter is discarded) on a duplicate name, or for a
    // dynamic converter a duplicate value.
    bool add(std::string_view name, int64_t value);
    std::optional<int64_t> lookup(std::string_view name) const;  // normalises `name`
    // Converts a field value: enum = the whole value as one name; bitfield = OR of the tokens.
    std::optional<int64_t> convert(std::string_view text) const;
};

// Name normalisation of §9.1 (trim space/tab/CR/LF, C toupper).
std::string normalize_enum_name(std::string_view s);
// sscanf("%llx") on an entry text: leading C whitespace, optional "0x", hex digits; 0 if none.
uint64_t scan_hex_u64(std::string_view s);

class EnumRegistry {
public:
    EnumRegistry();  // static tables + vanilla fallbacks for the dynamic ones
    // Loads Data\XML\Enum\<Name>.xml for the dynamic converters through the provider. A missing
    // file keeps the built-in fallback (diagnostic added); a duplicate entry or (bitfield) more
    // than 64 entries discards the converter (diagnostic added).
    void load_dynamic(const FileProvider& provider, std::vector<std::string>* diagnostics = nullptr);
    const EnumConverter& get(EnumId id) const { return conv_[static_cast<size_t>(id)]; }

private:
    EnumConverter conv_[static_cast<size_t>(EnumId::Count)];
};

// Converter of a GameObjectType field (tag, ASCII case-insensitive); nullopt when the field has no
// EnumRegistry converter (ModelAnimType fields and Targeting_Priority_Set: see game_object_types.hpp;
// code 0x43 and non-enum fields keep the value's name / tokens only; 0x52: combat_mod_value).
std::optional<EnumId> enum_of_type_field(std::string_view tag);
// True for the ModelAnimType fields of §9.1.3: Specific_Death_Anim_Type, Shield_On_Anim,
// Shield_Off_Anim and any other *_Anim_Type tag (ASCII case-insensitive).
bool is_anim_type_field(std::string_view tag);

// Ship_Class (code 0x32): _stricmp table; nullopt = unknown (field unchanged, no log).
std::optional<int32_t> ship_class_value(std::string_view text);
// Projectile_Category (code 0x4a): upper-cased exact compare; unknown -> -1.
int32_t projectile_category_value(std::string_view text);
// Combat-mod enum (code 0x52, table 0x140a434e0, TACTICAL_GAPS §6.5): the first token (strtok on
// space / comma / tab / CR / LF), upper-cased, exact match: DEFENSE 1, DAMAGE 2, HEALTH 3, SHIELD 4,
// ENERGY 5, SPEED 6, FIRE_RATE 7, FOW_REVEAL_RANGE 8, FIRE_RANGE 9, ABILITY_RECHARGE_TIME 10,
// TACTICAL_BUILD_TIME 11 (= modifier list index + 1); nullopt = unknown (field unchanged).
// [port] A "COMBAT_MOD_" prefix (how the XML writes them: COMBAT_MOD_SPEED) is accepted too.
std::optional<int32_t> combat_mod_value(std::string_view text);

}  // namespace alamo::objects
