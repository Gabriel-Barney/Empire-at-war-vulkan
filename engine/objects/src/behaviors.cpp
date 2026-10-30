#include "alamo/objects/behaviors.hpp"

#include <string>
#include <unordered_map>

#include "alamo/core/hash.hpp"

namespace alamo::objects {

namespace {

const std::string_view kClasses[] = {
#include "behavior_classes.inc"
};

struct NameId {
    std::string_view name;
    BehaviorTypeId id;
};

// docs/re/OBJECT_RUNTIME.md §4.3.
const NameId kNames[] = {
    {"LAND_TEAM_CONTAINER_LOCOMOTOR", 0x01}, {"LAND_TEAM_INFANTRY_LOCOMOTOR", 0x01}, {"SIMPLE_LOCOMOTOR", 0x01},
    {"STARSHIP_LOCOMOTOR", 0x01}, {"SIMPLE_SPACE_LOCOMOTOR", 0x01}, {"FLEET_LOCOMOTOR", 0x01},
    {"WALK_LOCOMOTOR", 0x01}, {"BIKE_LOCOMOTOR", 0x01}, {"FIGHTER_LOCOMOTOR", 0x01}, {"TEAM_LOCOMOTOR", 0x01},
    {"FLYING_LOCOMOTOR", 0x01}, {"JETPACK_LOCOMOTOR", 0x01}, {"LANDBOMBER_LOCOMOTOR", 0x01},
    {"SELECTABLE", 0x02}, {"PLANET", 0x03}, {"FLEET", 0x04}, {"TRANSPORT", 0x05}, {"PRODUCTION", 0x06},
    {"IDLE", 0x07}, {"PROJECTILE", 0x08}, {"TARGETING", 0x09}, {"TEAM_TARGETING", 0x09}, {"WEAPON", 0x0A},
    {"TURRET", 0x0B}, {"PARTICLE", 0x0C}, {"SURFACE_FX", 0x0D}, {"WIND_DISTURBANCE", 0x0E}, {"POWERED", 0x0F},
    {"SHIELDED", 0x10}, {"MARKER", 0x11}, {"UNIT_AI", 0x12}, {"STARSHIP_COMBATANT", 0x13},
    {"LAND_OBSTACLE", 0x14}, {"SQUAD_LEADER", 0x15}, {"TEAM", 0x16}, {"WALL", 0x17}, {"REVEAL", 0x18},
    {"HIDE_WHEN_FOGGED", 0x19}, {"SPAWN_SQUADRON", 0x1A}, {"SPAWN_INDIGENOUS_UNITS", 0x1B},
    {"DEPLOY_TROOPERS", 0x1C}, {"BARRAGE", 0x1D}, {"LANDING", 0x1E}, {"INDIGENOUS_UNIT", 0x20},
    {"EXPLOSION", 0x20}, {"HUNT", 0x21}, {"GUARD_SPAWN_HOUSE", 0x22}, {"LURE", 0x23}, {"TREAD_SCROLL", 0x25},
    {"SQUASH", 0x26}, {"AMBIENT_SFX", 0x27}, {"DEBRIS", 0x28}, {"TACTICAL_BUILD_OBJECTS", 0x29},
    {"TACTICAL_UNDER_CONSTRUCTION", 0x2A}, {"TACTICAL_SELL", 0x2B}, {"TACTICAL_SUPER_WEAPON", 0x2C},
    {"BURNING", 0x2D}, {"ASTEROID_FIELD_DAMAGE", 0x2E}, {"SPECIAL_WEAPON", 0x2F}, {"TELEKINESIS_TARGET", 0x30},
    {"EARTHQUAKE_TARGET", 0x31}, {"VEHICLE_THIEF", 0x32}, {"DEMOLITION", 0x33}, {"BOMB", 0x34},
    {"TRANSPORT_LANDING", 0x36}, {"LOBBING_SUPERWEAPON", 0x37}, {"STUNNABLE", 0x38}, {"BASE_SHIELD", 0x39},
    {"GRAVITY_CONTROL_FIELD", 0x3A}, {"SKY_DOME", 0x3B}, {"CASH_POINT", 0x3C}, {"CAPTURE_POINT", 0x3D},
    {"DAMAGE_TRACKING", 0x3E}, {"TERRAIN_TEXTURE_MODIFICATION", 0x3F}, {"IMPOSING_PRESENCE", 0x40},
    {"HARASS", 0x41}, {"AVOID_DANGER", 0x42}, {"SELF_DESTRUCT", 0x43}, {"DEATH", 0x44},
    {"ION_STUN_EFFECT", 0x45}, {"ABILITY_COUNTDOWN", 0x46}, {"AFFECTED_BY_SHIELD", 0x47}, {"NEBULA", 0x48},
    {"REINFORCEMENT_POINT", 0x49}, {"HINT", 0x4A}, {"INVULNERABLE", 0x4B}, {"SPACE_OBSTACLE", 0x4C},
    {"MULTIPLAYER_BEACON", 0x4D}, {"HOLSTER_WEAPON", 0x4E}, {"GARRISON_VEHICLE", 0x4F},
    {"GARRISON_STRUCTURE", 0x50}, {"GARRISON_UNIT", 0x51}, {"DISABLE_FORCE_ABILITIES", 0x52}, {"CONFUSE", 0x53},
    {"INFECTION", 0x54}, {"PROXIMITY_MINE", 0x55}, {"GARRISON_HOVER", 0x56}, {"BUZZ_DROIDS", 0x57},
    {"CORRUPT_SYSTEMS", 0x58}, {"BOARDABLE", 0x59}, {"DYNAMIC_TRANSFORM", 0x5A},
    {"DUMMY_GROUND_BASE", 0x5B}, {"DUMMY_STAR_BASE", 0x5C}, {"DUMMY_GROUND_COMPANY", 0x5D},
    {"DUMMY_SPACE_FIGHTER_SQUADRON", 0x5E}, {"DUMMY_GROUND_STRUCTURE", 0x5F}, {"DUMMY_ORBITAL_STRUCTURE", 0x60},
    {"DUMMY_STARSHIP", 0x61}, {"DUMMY_LAND_BASE_LEVEL_COMPONENT", 0x62}, {"DUMMY_SPECIAL_WEAPON_SOURCE", 0x63},
    {"DUMMY_TACTICAL_SUPER_WEAPON_TARGET", 0x64}, {"DUMMY_UPGRADE", 0x65}, {"DUMMY_DESTROY_AFTER_BOMBING_RUN", 0x66},
    {"DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED", 0x67}, {"DUMMY_TOOLTIP", 0x68},
    {"DUMMY_DESTROY_AFTER_PLANETARY_BOMBARD", 0x69},
};

struct NameIndex {
    std::unordered_map<std::string, BehaviorTypeId> by_name;  // lookup only
    std::string_view canonical[kBehaviorTypeCount] = {};
    NameIndex() {
        for (const NameId& n : kNames) {
            by_name.emplace(normalize_name(n.name), n.id);
            if (canonical[n.id].empty()) canonical[n.id] = n.name;
        }
        canonical[0x01] = "LOCOMOTOR";
    }
};

const NameIndex& names() {
    static const NameIndex idx;
    return idx;
}

}  // namespace

BehaviorTypeId behavior_type_id(std::string_view xml_name) {
    const auto& m = names().by_name;
    const auto it = m.find(normalize_name(xml_name));
    return it == m.end() ? kNoBehavior : it->second;
}

std::string_view behavior_type_name(BehaviorTypeId id) {
    return id < kBehaviorTypeCount ? names().canonical[id] : std::string_view{};
}

uint32_t behavior_name_count() { return static_cast<uint32_t>(std::size(kNames)); }

BehaviorService behavior_service(std::string_view xml_name) {
    const BehaviorTypeId id = behavior_type_id(xml_name);
    if (id == kNoBehavior) return {};
    if (id >= 0x5B) return {};  // DUMMY_*
    struct Row {
        std::string_view name;
        bool serviced;
        uint32_t interval;
        bool computed;
    };
    static constexpr Row kRows[] = {
        {"TACTICAL_BUILD_OBJECTS", false, 0, false}, {"MARKER", false, 0, false}, {"HINT", false, 0, false},
        {"TRANSPORT", false, 0, false}, {"TACTICAL_SELL", false, 0, false},
        {"TERRAIN_TEXTURE_MODIFICATION", false, 0, false}, {"WALL", false, 0, false},
        {"SPACE_OBSTACLE", false, 0, false}, {"STARSHIP_COMBATANT", false, 0, false},
        {"EXPLOSION", true, 0, false}, {"CAPTURE_POINT", true, 4, false}, {"CASH_POINT", true, 4, false},
        {"SPAWN_INDIGENOUS_UNITS", true, 30, false}, {"DAMAGE_TRACKING", true, 30, false},
        {"HARASS", true, 30, false}, {"AVOID_DANGER", true, 30, false}, {"BARRAGE", true, 30, false},
        {"SPAWN_SQUADRON", true, 30, false}, {"BURNING", true, 30, false}, {"IMPOSING_PRESENCE", true, 30, false},
        {"AMBIENT_SFX", true, 30, false}, {"INVULNERABLE", true, 30, false},
        {"GRAVITY_CONTROL_FIELD", true, 60, false},
        // Computed at attach (computed_behavior_interval); 1 is the placeholder.
        {"LAND_OBSTACLE", true, 1, true}, {"SHIELDED", true, 1, true}, {"POWERED", true, 1, true},
        {"BASE_SHIELD", true, 1, true}, {"INDIGENOUS_UNIT", true, 1, true}, {"HUNT", true, 1, true},
        {"GUARD_SPAWN_HOUSE", true, 1, true},
    };
    for (const Row& r : kRows)
        if (iequals(r.name, xml_name)) return {r.serviced, r.interval, r.computed};
    return {true, 1, false};
}

BehaviorService behavior_service(BehaviorTypeId id) {
    if (id == 0x01) return {true, 1, false};  // every locomotor
    const std::string_view n = behavior_type_name(id);
    return n.empty() ? BehaviorService{} : behavior_service(n);
}

uint32_t computed_behavior_interval(BehaviorTypeId id, const BehaviorIntervalConfig& cfg, float multiplier, bool burning) {
    const auto rounded = [&](float secs) -> uint32_t {
        const float v = static_cast<float>(cfg.fps) * multiplier * secs + 0.5f;
        if (!(v >= 1.0f)) return 0;  // negative, NaN or < 1 truncate to 0
        if (v >= 4294967040.0f) return 0xFFFFFF00u;
        return static_cast<uint32_t>(v);
    };
    switch (id) {
        case beh::Shielded: return rounded(cfg.shield_recharge_interval_secs);
        case beh::Powered: return rounded(cfg.energy_recharge_interval_secs);
        case beh::BaseShield: return cfg.fps / 2u;
        case beh::IndigenousUnit:
        case beh::Hunt:
        case beh::GuardSpawnHouse: return burning ? 1u : 30u;
        default: return 1u;  // LAND_OBSTACLE and anything else
    }
}

bool behavior_starts_disabled(BehaviorTypeId id) {
    switch (id) {
        case beh::Hunt:
        case beh::Lure:
        case beh::SelfDestruct:
        case beh::DeployTroopers:
        case beh::Barrage:
        case beh::Landing: return true;
        default: return false;
    }
}

std::span<const std::string_view> behavior_classes() { return {kClasses, std::size(kClasses)}; }

bool is_behavior_list_tag(std::string_view tag) {
    static constexpr std::string_view kTags[] = {"Behavior",         "LandBehavior",     "SpaceBehavior",
                                                 "GalacticBehavior", "TacticalBehavior", "DeployedBehavior"};
    for (std::string_view t : kTags)
        if (iequals(t, tag)) return true;
    return false;
}

}  // namespace alamo::objects
