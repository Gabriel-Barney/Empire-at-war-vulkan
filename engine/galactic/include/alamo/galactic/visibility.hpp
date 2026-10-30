// GRC 2 (docs/re/GALACTIC_REVEAL_CORRUPTION.md §2): planet visibility levels (VisibilityLevelType bits), the
// per-(planet, player) modifier list entries (SimpleModifierListClass<tVisibilityLevelType>, PD +0x1B8) and the
// VisibilityLevelType text converter (0x510120). The runtime (lists, fold, sources, expiry) is on GalacticSim
// (campaign.hpp, src/visibility.cpp).
#pragma once
#include <cstdint>
#include <string_view>

namespace alamo::galactic {

// VisibilityLevelType bits (GRC 2.2) [H].
enum VisibilityBit : uint32_t {
    kVisPlanetOwnership = 0x1,
    kVisStarbaseLevel = 0x2,
    kVisGroundBaseLevel = 0x4,
    kVisCreditIncome = 0x8,
    kVisFleetPresence = 0x10,
    kVisNumFleets = 0x20,
    kVisFleetContents = 0x40,
    kVisNumGroundCompanies = 0x80,
    kVisGroundCompanyContents = 0x100,
    kVisSpecialStructures = 0x200,
    kVisPoliticalControl = 0x400,
    kVisPoliticalControlBreakdown = 0x800,
    kVisObjectUnderProduction = 0x1000,
    kVisEnemyMinorStealthHeroes = 0x2000,
    kVisEnemyMajorStealthHeroes = 0x4000,
    kVisCreditIncomeBreakdown = 0x8000,
    kVisMostPowerfulShip = 0x10000,
    kVisForceSensitiveHeroes = 0x20000,
    kVisSuperWeapons = 0x40000,
};
// Named composites (GRC 2.2).
inline constexpr uint32_t kVisEvilDefault = 0x0;
inline constexpr uint32_t kVisGoodDefault = 0x11;
inline constexpr uint32_t kVisPlanetOwner = 0x9FFF;
inline constexpr uint32_t kVisMaximum = 0xFFFFFFFFu;
// Presence entry values (GRC 2.3, 0x3fd080): an own object in orbit / on the ground.
inline constexpr uint32_t kVisPresenceOrbit = 0x71;
inline constexpr uint32_t kVisPresenceGround = 0x191;

// Expiry frame meaning "never" (entry +0x14).
inline constexpr uint32_t kVisNever = 0xFFFFFFFFu;

// Entry key (entry +0x20, 0x4ad4c0; matched field-wise as 0x4ad580 / 0x4ad550). Stable ids only, never pointers:
// object = ObjectId of the source object (planet for baseline / presence / story, the spy unit for spies), serial = the
// object's serial (+0x298; the port's ObjectIds are never reused, so 0 is fine), sub = sub-key, extra = spare.
struct VisibilityKey {
    int32_t object = -1;
    uint32_t serial = 0;
    uint32_t sub = 0;
    uint32_t extra = 0;
    friend bool operator==(const VisibilityKey& a, const VisibilityKey& b) {
        return a.object == b.object && a.serial == b.serial && a.sub == b.sub && a.extra == b.extra;
    }
};
// Sub-keys of the derived entries (GRC 2.3): baseline (planet, 0), presence (planet, 1).
inline constexpr uint32_t kVisSubBaseline = 0;
inline constexpr uint32_t kVisSubPresence = 1;

// One list entry (0x40 bytes in the original): value +0x10, expiry +0x14, name hash +0x18, key +0x20.
struct VisibilityModifier {
    uint32_t value = 0;
    uint32_t expiry = kVisNever;  // removed by the planet service when expiry < the mode frame (0x285e40)
    uint32_t name_hash = 0;       // special_name_crc of the ability name / story key (0x215a30)
    VisibilityKey key;
};

// VisibilityLevelType converter (0x510120) [port syntax]: one or more names (case-insensitive) separated by '|', ',',
// '+' or whitespace, OR-ed: the 19 bit names of GRC 2.2 (with or without a leading "Enemy_" where the original has one)
// and the composites Evil_Default, Good_Default, Planet_Owner, Maximum; a plain integer (decimal or 0x hex) is taken as
// a mask. False (and *out untouched) for an empty text or an unknown name. "none" is NOT a level (the story reward
// treats it as "remove").
bool parse_visibility_level(std::string_view text, uint32_t* out);
// The faction default by name (GRC 2.3 baseline / presence start): "Rebel" and "Underworld" Good_Default (0x11),
// "Empire" and every other faction Evil_Default (0). Exact-case names as the original compares them [M: case-insensitive
// here, the shipped names match either way].
uint32_t faction_default_visibility(std::string_view faction_name);

}  // namespace alamo::galactic
