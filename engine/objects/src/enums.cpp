#include "alamo/objects/enums.hpp"

#include <memory>

#include "alamo/core/ascii.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::objects {

namespace {

struct Pair {
    std::string_view name;
    int64_t value;
};

// §9.1.1 vanilla FoC GameObjectCategoryType.xml (fallback when the file is absent).
const Pair kCategory[] = {
    {"Fighter", 0x1},           {"Bomber", 0x2},          {"Transport", 0x4},         {"Corvette", 0x8},
    {"Frigate", 0x10},          {"Capital", 0x20},        {"Super", 0x40},            {"Infantry", 0x80},
    {"Vehicle", 0x100},         {"Air", 0x200},           {"Structure", 0x400},       {"AntiFighter", 0x800},
    {"AntiBomber", 0x1000},     {"AntiTransport", 0x2000}, {"AntiCorvette", 0x4000},  {"AntiFrigate", 0x8000},
    {"AntiCapital", 0x10000},   {"AntiSuper", 0x20000},   {"AntiInfantry", 0x40000},  {"AntiVehicle", 0x80000},
    {"AntiAir", 0x100000},      {"AntiStructure", 0x200000}, {"LandHero", 0x400000},  {"Wall", 0x800000},
    {"Upgrade", 0x1000000},     {"SpaceHero", 0x2000000}, {"NonCombatHero", 0x4000000}, {"Hero", 0x6400000},
    {"All", -1},                {"None", 0},
};
// §9.1.2.
const Pair kProperties[] = {
    {"HealsInfantry", 0x1},         {"HealsVehicles", 0x2},        {"Turret", 0x4},          {"TacticalStructure", 0x8},
    {"InBase", 0x10},               {"OutBase", 0x20},             {"Fodder", 0x40},         {"SmallShip", 0x80},
    {"NotOpportunityTarget", 0x100}, {"GoodInfantryCrusher", 0x200}, {"IsSupercrusher", 0x400}, {"IsRushTarget", 0x800},
    {"CanContainGarrison", 0x1000}, {"GarrisonCanFire", 0x2000},
};
const Pair kMovement[] = {
    {"Infantry", 0},     {"InfantryNoShield", 1}, {"Wheeled", 2},      {"WheeledNoShield", 3}, {"Walker", 4},
    {"WalkerNoShield", 5}, {"Flying", 6},         {"Tracked", 7},      {"TrackedNoShield", 8}, {"Hover", 9},
    {"HoverNoShield", 10}, {"LargeWalker", 11},   {"LargeWalkerNoShield", 12}, {"Space", 13}, {"Lancet", 14},
    {"Spotter", 15},     {"LargeHover", 16},      {"BombingRun", 17},
};
const Pair kWeather[] = {
    {"Default", 0}, {"Infantry", 1}, {"LargeWalker", 2}, {"Speeder", 3}, {"Walker", 4},
    {"Tracked", 5}, {"Hover", 6},    {"Wheeled", 7},     {"Flying", 8},
};
// §9.1.3 static tables.
const Pair kSpaceLayer[] = {{"None", 0}, {"Capital", 1}, {"Frigate", 2}, {"Corvette", 4},
                            {"StaticObject", 8}, {"Land", 16}, {"SuperCapital", 32}};
const Pair kUnitCollision[] = {{"None", -1},          {"Landing Transport", 0}, {"Build Pad", 1},
                               {"Low Obstacle", 2},   {"Medium Obstacle", 3},   {"High Obstacle", 4},
                               {"Infantry", 5},       {"Bike", 6},              {"Vehicle", 7},
                               {"Large Vehicle", 8},  {"Giant Vehicle", 9}};
const Pair kOccupation[] = {{"None", -1}, {"1x1", 0}};
const Pair kPassability[] = {{"Invalid", -1},    {"None", 0},       {"Clear", 1},   {"Steep", 2},
                             {"Obstacle", 4},    {"RebelWall", 8},  {"EmpireWall", 16}, {"Water", 32},
                             {"Shield", 64},     {"InfantryOnly", 128}, {"Impassable", 256}};
const Pair kContainer[] = {{"None", -1}, {"Rows", 0}, {"Dartboard", 1}};
const Pair kForce[] = {{"Light", 0}, {"Neutral", 1}, {"Dark", 2}};  // [name-ok: Force_Alignment keyword, not a faction]
const Pair kFormup[] = {{"WaitForAll", 0}, {"WaitForFront", 1}, {"WaitNone", 2}};
const Pair kGrouping[] = {{"Solo", 0}, {"SameOrder", 1}, {"Standard", 2}};
const Pair kVisibility[] = {
    {"Planet_Ownership", 0x1},          {"Starbase_Level", 0x2},          {"Ground_Base_Level", 0x4},
    {"Credit_Income", 0x8},             {"Fleet_Presence", 0x10},         {"Num_Fleets", 0x20},
    {"Fleet_Contents", 0x40},           {"Num_Ground_Companies", 0x80},   {"Ground_Company_Contents", 0x100},
    {"Special_Structures", 0x200},      {"Political_Control", 0x400},     {"Political_Control_Breakdown", 0x800},
    {"Object_Under_Production", 0x1000}, {"Enemy_Minor_Stealth_Heroes", 0x2000},
    {"Enemy_Major_Stealth_Heroes", 0x4000}, {"Credit_Income_Breakdown", 0x8000}, {"Most_Powerful_Ship", 0x10000},
    {"Force_Sensitive_Heroes", 0x20000}, {"Super_Weapons", 0x40000},      {"Evil_Default", 0},
    {"Good_Default", 0x11},             {"Planet_Owner", 0x9FFF},         {"Maximum", -1},
};
const Pair kDamage[] = {
    {"Damage_Debug_Chear", 0},        {"Damage_Normal", 1},          {"Damage_Force_Whirlwind", 2},
    {"Damage_Force_Telekinesis", 3},  {"Damage_Force_Lightning", 4}, {"Damage_Force_Corruption", 5},
    {"Damage_Hard_Point_Self_Destruct", 6}, {"Damage_Fire", 7},     {"Damage_Cable_Attack", 8},
    {"Damage_Explosion", 9},          {"Damage_Asteroid", 10},       {"Damage_Cable_Attack_Deployed", 11},
    {"Damage_Normal_Deployed", 12},   {"Damage_Vehicle_Thief", 13},  {"Damage_Crush", 14},
    {"Damage_Eat", 15},               {"Damage_Redirected", 16},     {"Damage_Wampa", 18},
    {"Damage_Infection", 20},         {"Damage_Remote_Bomb", 21},    {"Damage_Drain_Life", 22},
};
const Pair kQueue[] = {{"Land", 0}, {"Space", 1}, {"Tactical_Units", 2}, {"Tactical_Upgrades", 3}};
const Pair kSellable[] = {{"Sellable_Default", 0}, {"Sellable_Always", 1}, {"Sellable_Never", 2}};
const Pair kCorruption[] = {
    {"Corruption_None", -1},       {"Corruption_Intimidation", 0}, {"Corruption_Piracy", 1},
    {"Corruption_Kidnapping", 2},  {"Corruption_Racketeering", 3}, {"Corruption_Corrupt_Militia", 4},
    {"Corruption_Bribery", 5},     {"Corruption_Black_Market", 6}, {"Corruption_Bonded_Citizens", 7},
};
const Pair kSubMode[] = {{"Galactic", 0}, {"Land", 1}, {"Space", 2}};

bool mask_delim(char c) { return c == '|' || c == ' ' || c == ',' || c == '\t' || c == '\n'; }

template <size_t N>
void fill(EnumConverter& c, std::string_view name, bool bitfield, bool dynamic, const Pair (&table)[N]) {
    c.enum_name = name;
    c.bitfield = bitfield;
    c.dynamic = dynamic;
    c.valid = true;
    for (const Pair& p : table) c.add(p.name, p.value);
}

struct DynamicFile {
    EnumId id;
    std::string_view name;
};
const DynamicFile kDynamicFiles[] = {
    {EnumId::GameObjectCategory, "GameObjectCategoryType"},
    {EnumId::GameObjectProperties, "GameObjectPropertiesType"},
    {EnumId::MovementClass, "MovementClassType"},
    {EnumId::ObjectWeatherCategory, "ObjectWeatherCategoryType"},
    {EnumId::SurfaceFXTrigger, "SurfaceFXTriggerType"},
};

}  // namespace

std::string normalize_enum_name(std::string_view s) {
    return ascii_upper(ascii_trim(s));
}

uint64_t scan_hex_u64(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\v' || s[i] == '\f'))
        ++i;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else break;
        v = (v << 4) | d;  // %llx wraps silently on overflow (MSVC); vanilla never overflows
    }
    return v;
}

bool EnumConverter::add(std::string_view name, int64_t value) {
    std::string n = normalize_enum_name(name);
    for (const Entry& e : entries)
        if (e.name == n || (dynamic && e.value == value)) {
            valid = false;
            return false;
        }
    entries.push_back({std::move(n), value});
    return true;
}

std::optional<int64_t> EnumConverter::lookup(std::string_view name) const {
    if (!valid) return std::nullopt;
    const std::string n = normalize_enum_name(name);
    for (const Entry& e : entries)
        if (e.name == n) return e.value;
    return std::nullopt;
}

std::optional<int64_t> EnumConverter::convert(std::string_view text) const {
    if (!valid) return std::nullopt;
    if (!bitfield) return lookup(text);
    uint64_t mask = 0;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && mask_delim(text[i])) ++i;
        const size_t b = i;
        while (i < text.size() && !mask_delim(text[i])) ++i;
        if (i == b) continue;
        const std::string_view tok = ascii_trim(text.substr(b, i - b));
        if (tok.empty()) continue;  // a lone CR between delimiters
        const std::optional<int64_t> v = lookup(tok);
        if (!v) return std::nullopt;  // one unknown token rejects the whole value
        mask |= static_cast<uint64_t>(*v);
    }
    return static_cast<int64_t>(mask);
}

EnumRegistry::EnumRegistry() {
    auto c = [this](EnumId id) -> EnumConverter& { return conv_[static_cast<size_t>(id)]; };
    fill(c(EnumId::GameObjectCategory), "GameObjectCategoryType", true, true, kCategory);
    fill(c(EnumId::GameObjectProperties), "GameObjectPropertiesType", true, true, kProperties);
    fill(c(EnumId::MovementClass), "MovementClassType", false, true, kMovement);
    fill(c(EnumId::ObjectWeatherCategory), "ObjectWeatherCategoryType", false, true, kWeather);
    EnumConverter& sfx = c(EnumId::SurfaceFXTrigger);  // no built-in table: absent until loaded
    sfx.enum_name = "SurfaceFXTriggerType";
    sfx.dynamic = true;
    fill(c(EnumId::SpaceLayer), "SpaceLayerType", true, false, kSpaceLayer);
    fill(c(EnumId::UnitCollisionClass), "UnitCollisionClassType", false, false, kUnitCollision);
    fill(c(EnumId::UnitOccupation), "UnitOccupationType", false, false, kOccupation);
    fill(c(EnumId::CellPassability), "CellPassabilityType", true, false, kPassability);
    fill(c(EnumId::ContainerArrangement), "ContainerArrangementType", false, false, kContainer);
    fill(c(EnumId::ForceAlignment), "ForceAlignmentType", false, false, kForce);
    fill(c(EnumId::FormationFormupWait), "FormationFormupWaitType", false, false, kFormup);
    fill(c(EnumId::FormationGrouping), "FormationGroupingType", false, false, kGrouping);
    fill(c(EnumId::VisibilityLevel), "tVisibilityLevelType", true, false, kVisibility);
    fill(c(EnumId::DamageType), "tDamageType", false, false, kDamage);
    fill(c(EnumId::ProductionQueue), "ProductionQueueType", false, false, kQueue);
    fill(c(EnumId::Sellable), "SellableTypeEnum", false, false, kSellable);
    fill(c(EnumId::CorruptionType), "CorruptionTypeEnum", false, false, kCorruption);
    fill(c(EnumId::SubGameMode), "tSubGameModeType", false, false, kSubMode);
}

void EnumRegistry::load_dynamic(const FileProvider& provider, std::vector<std::string>* diagnostics) {
    auto note = [&](std::string m) {
        if (diagnostics) diagnostics->push_back(std::move(m));
    };
    for (const DynamicFile& df : kDynamicFiles) {
        EnumConverter& c = conv_[static_cast<size_t>(df.id)];
        std::string path = "Data\\XML\\Enum\\";
        path += df.name;
        path += ".xml";
        std::vector<uint8_t> bytes;
        if (!provider.read(path, bytes)) {
            note("enum file '" + path + "' not found: " +
                 (c.valid ? std::string("built-in vanilla table used") : std::string("converter absent")));
            continue;
        }
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, path);
        EnumConverter fresh;
        fresh.enum_name = c.enum_name;
        fresh.bitfield = c.bitfield;
        fresh.dynamic = true;
        fresh.valid = doc.ok();
        fresh.from_file = true;
        uint32_t index = 0;
        for (data::XmlNode e = doc.root().first_child(); e && fresh.valid; e = e.next_sibling(), ++index) {
            const std::string_view text = ascii_trim(e.text());
            int64_t value;
            if (fresh.bitfield) {
                if (index >= 64) {
                    note("Error - too many entries for bitfield " + path);
                    fresh.valid = false;
                    break;
                }
                value = text.empty() ? static_cast<int64_t>(64 - index) : static_cast<int64_t>(scan_hex_u64(text));
            } else {
                value = text.empty() ? static_cast<int64_t>(index) : static_cast<int64_t>(c_atoi(text));
            }
            if (!fresh.add(e.name(), value))
                note("ERROR: couldn't add enum entry '" + std::string(e.name()) + "' to " + std::string(c.enum_name) +
                     " (converter discarded)");
        }
        c = std::move(fresh);
    }
}

std::optional<EnumId> enum_of_type_field(std::string_view tag) {
    struct FieldEnum {
        std::string_view tag;
        EnumId id;
    };
    static constexpr FieldEnum kFields[] = {
        {"CategoryMask", EnumId::GameObjectCategory},
        {"Garrison_Category", EnumId::GameObjectCategory},
        {"Space_Or_Garrison_Category", EnumId::GameObjectCategory},
        {"Attack_Category_Restrictions", EnumId::GameObjectCategory},
        {"Special_Weapon_Valid_Targets", EnumId::GameObjectCategory},
        {"Planet_Restricted_Unit_Categories", EnumId::GameObjectCategory},
        {"Projectile_Combat_Mod_Category_Mask", EnumId::GameObjectCategory},
        {"Property_Flags", EnumId::GameObjectProperties},
        {"MovementClass", EnumId::MovementClass},
        {"Weather_Category", EnumId::ObjectWeatherCategory},
        {"Projectile_Ground_Detonation_SurfaceFX", EnumId::SurfaceFXTrigger},
        {"Explosion_Scorch_Mark", EnumId::SurfaceFXTrigger},
        {"Space_Layer", EnumId::SpaceLayer},
        {"Stationary_Space_Layer", EnumId::SpaceLayer},
        {"UnitCollisionClass", EnumId::UnitCollisionClass},
        {"OccupationStyle", EnumId::UnitOccupation},
        {"OverridePassability", EnumId::CellPassability},
        {"SecondaryOccupationPassability", EnumId::CellPassability},
        {"ContainerArrangement", EnumId::ContainerArrangement},
        {"The_Force", EnumId::ForceAlignment},
        {"Formation_Formup_Wait_Style", EnumId::FormationFormupWait},
        {"FormationGrouping", EnumId::FormationGrouping},
        {"Can_See_Visibility_Level", EnumId::VisibilityLevel},
        {"Internal_Damage_Type", EnumId::DamageType},
        {"Squash_Damage_Type", EnumId::DamageType},
        {"Tactical_Production_Queue", EnumId::ProductionQueue},
        {"Is_Sellable", EnumId::Sellable},
    };
    for (const FieldEnum& f : kFields)
        if (iequals(f.tag, tag)) return f.id;
    // Projectile_*_Category_Mask (category bitfield).
    const std::string n = normalize_name(tag);
    auto ends = [&](std::string_view suf) { return n.size() >= suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0; };
    if (n.rfind("PROJECTILE_", 0) == 0 && ends("_CATEGORY_MASK")) return EnumId::GameObjectCategory;
    // Corruption_N_Type / _Required_Mission_Type / _Tactical_Game_Mode.
    if (n.rfind("CORRUPTION_", 0) == 0 && n.size() > 12 && n[11] >= '0' && n[11] <= '9' && n[12] == '_') {
        const std::string_view rest = std::string_view(n).substr(13);
        if (rest == "TYPE" || rest == "REQUIRED_MISSION_TYPE") return EnumId::CorruptionType;
        if (rest == "TACTICAL_GAME_MODE") return EnumId::SubGameMode;
    }
    return std::nullopt;
}

bool is_anim_type_field(std::string_view tag) {
    if (iequals(tag, "Shield_On_Anim") || iequals(tag, "Shield_Off_Anim")) return true;
    const std::string n = normalize_name(tag);
    constexpr std::string_view kSuffix = "_ANIM_TYPE";
    return n.size() > kSuffix.size() && n.compare(n.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

std::optional<int32_t> ship_class_value(std::string_view text) {
    static constexpr Pair kShip[] = {{"fighter", 0},  {"bomber", 1}, {"transport", 2}, {"corvette", 3},
                                     {"frigate", 4},  {"capital", 5}, {"super", 6},    {"infantry", 0},
                                     {"tank", 4},     {"artillery", 2}, {"walker", 5}};
    for (const Pair& p : kShip)
        if (iequals(p.name, text)) return static_cast<int32_t>(p.value);
    return std::nullopt;
}

int32_t projectile_category_value(std::string_view text) {
    static constexpr std::string_view kCat[] = {"DEFAULT", "MISSILE", "BOMB",  "GRENADE",    "ROCKET",
                                                "STATIC_BOMB", "MPTL_ROCKET", "LASER", "LIGHT_SABER", "CLUSTER_BOMB"};
    std::string u(text);
    for (char& c : u) c = ascii_to_upper(c);
    for (size_t i = 0; i < std::size(kCat); ++i)
        if (u == kCat[i]) return static_cast<int32_t>(i);
    return -1;
}

std::optional<int32_t> combat_mod_value(std::string_view text) {
    static constexpr std::string_view kMods[] = {"DEFENSE", "DAMAGE",     "HEALTH",     "SHIELD",
                                                 "ENERGY",  "SPEED",      "FIRE_RATE",  "FOW_REVEAL_RANGE",
                                                 "FIRE_RANGE", "ABILITY_RECHARGE_TIME", "TACTICAL_BUILD_TIME"};
    const auto delim = [](char c) { return c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0;
    while (b < text.size() && delim(text[b])) ++b;
    size_t e = b;
    while (e < text.size() && !delim(text[e])) ++e;
    if (e == b) return std::nullopt;
    std::string u(text.substr(b, e - b));
    for (char& c : u) c = ascii_to_upper(c);
    // The data writes COMBAT_MOD_<NAME> (FoC PROJECTILES.XML: COMBAT_MOD_SPEED); the spec lists the bare names.
    // [port] both spellings accepted until the table's exact strings are confirmed (PHASE4 S1 open item).
    constexpr std::string_view kPrefix = "COMBAT_MOD_";
    if (u.size() > kPrefix.size() && std::string_view(u).substr(0, kPrefix.size()) == kPrefix) u.erase(0, kPrefix.size());
    for (size_t i = 0; i < std::size(kMods); ++i)
        if (u == kMods[i]) return static_cast<int32_t>(i + 1);
    return std::nullopt;
}

}  // namespace alamo::objects
