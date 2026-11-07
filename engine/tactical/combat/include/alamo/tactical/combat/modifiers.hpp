// S8 - combat-stat modifiers (docs/re/TACTICAL_COMBAT.md section 4). Two independent systems; do
// not merge them.
//
// System A - ModifierSet (additive per stacking category), section 4.1-4.3 [H]:
//   * 11 lists (ModList). Each entry: value, expiry frame, stacking category, source name CRC,
//     source object id. Stored here as ONE flat record array per set (object or player), in
//     insertion order, instead of the original's 11 std::maps of vectors.
//   * reduce (ModifierSet_Reduce_MaxThenSum, combiner Plus, comparator GreaterThan):
//       1. stacking categories in ASCENDING id order;
//       2. per category the signed MAXIMUM (strictly greater replaces: the first of equals is kept;
//          two debuffs in one category give the less negative one);
//       3. total = first non-empty category's max, + each later category's max in order (empty: 0);
//       4. the list's min clamp (if flagged), then its max clamp.
//     Gather_Modifiers = owner player's set + the object's own set (merge appends per category, so
//     player entries come first within a category; the max makes the order irrelevant except for
//     the first-of-equals rule, which is kept).
//   * stat = base x (1 + r); defense (list 0) is used as (1 - r), r <= 0.75 by the default clamp.
//   * expiry: entries are NOT checked by reduce. purge_expired(frame) erases entries whose expiry
//     frame < frame compared UNSIGNED (so permanent = 0xFFFFFFFF never expires, a stored 0 goes at
//     the next purge), keeping the rest in order. Objects purge once per object update; an expired
//     but not yet purged entry still counts.
//   * Caching (port): the original rebuilt a temporary 11-tree set on every read (53% of the
//     original's main-thread time in large battles). Here reads go through a per-object cache of the
//     11 reduced values, recomputed only when the object's or its owner player's set changed
//     (insert / remove / purge bump a version). The cache is a pure function of the sets, so
//     results are identical with or without it; it is never hashed.
//
// System B - unit-ability Mod_Multiplier, section 4.4 [H]: 12 kinds, product over the type's first
// two ability slots counted only where the ability is active (LUCKY_SHOT skipped); a TEAM container
// whose own product is 1 takes the minimum over all its members (members keep their own). Mod_Flag =
// AND across active slots, default true.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::combat {

enum class ModList : uint8_t {
    Defense = 0,          // damage taken reduction, used as (1 - r); max clamp 0.75
    Damage = 1,           // damage dealt; min -1
    MaxHealth = 2,        // min -0.99
    MaxShield = 3,        // min -1
    MaxEnergy = 4,        // min -1
    MaxSpeed = 5,         // min -0.99
    FireRate = 6,         // min -0.99
    RevealRange = 7,      // none
    FireRange = 8,        // none
    AbilityRecharge = 9,  // min -0.99
    BuildTime = 10,       // min -0.99
};
inline constexpr uint32_t kModListCount = 11;

inline constexpr uint32_t kPermanent = 0xFFFFFFFFu;  // expiry frame -1: never purged

struct ListClamp {
    bool has_min = false, has_max = false;
    float min = 0.0f, max = 0.0f;
};
// The default per-list clamps of section 4.3.
const std::array<ListClamp, kModListCount>& default_list_clamps();

struct ModifierEntry {
    float value = 0.0f;
    uint32_t expiry = kPermanent;  // frame; kPermanent never expires
    int32_t category = 0;          // Stacking_Category
    uint32_t source_crc = 0;       // CRC32 of the source (ability) name
    int32_t source_id = -1;        // source object id (-1 none)
    uint8_t list = 0;              // ModList
};

// Duration_To_Expiry_Frame (0x35cd20): seconds <= 0 -> 0 (erased at the next purge); otherwise
// now + int(fps * seconds + 0.5). Permanent writers (BattlefieldModifier) pass kPermanent directly.
uint32_t expiry_frame(uint32_t now, float seconds, uint32_t fps);

// One set's entries (flat, insertion order). A version counter lets caches detect changes.
class ModifierSet {
public:
    void insert(ModList list, int32_t category, float value, uint32_t expiry, uint32_t source_crc = 0,
                int32_t source_id = -1);
    // ModifierList_Set_By_Source (0x3fe810, S9): replace the entry of `list` with the same source
    // (source_id and source_crc) in place, else append. A second application from the same source
    // overwrites instead of stacking (tractor beam, ion stun, concentrate fire).
    void set_by_source(ModList list, int32_t category, float value, uint32_t expiry, uint32_t source_crc,
                       int32_t source_id);
    // Explicit removal (ability vf31, matched by source). source_crc 0 = any. Returns count removed.
    uint32_t remove_source(int32_t source_id, uint32_t source_crc = 0);
    // Same, limited to one list (tractor / concentrate fire remove: list-5 entries of one source).
    uint32_t remove_source_in_list(ModList list, int32_t source_id, uint32_t source_crc = 0);
    // ModifierSet_Purge_Expired: erase entries with expiry < frame (unsigned compare), keep order.
    uint32_t purge_expired(uint32_t frame);
    void clear();

    std::span<const ModifierEntry> entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }
    uint32_t version() const { return version_; }
    void reserve(size_t n) { entries_.reserve(n); }
    void hash(StateHasher& h) const;  // count, then each entry's fields in order
    // N1 stage 2 (combat_save.cpp): no version of its own (part of the CombatSystem chunk): u32 version counter (caches
    // compare it), u32 count, each entry's fields. read() false (unchanged) on a bad list index / truncated data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::vector<ModifierEntry> entries_;
    uint32_t version_ = 0;
};

// The reduction of section 4.2 for one list over (player set, object set); either may be null.
// Allocation-free for any number of entries.
float reduce_list(const ModifierSet* player, const ModifierSet* object, ModList list,
                  const ListClamp& clamp);
inline float reduce_list(const ModifierSet* player, const ModifierSet* object, ModList list) {
    return reduce_list(player, object, list, default_list_clamps()[static_cast<uint32_t>(list)]);
}
// All 11 lists at once (one pass over the entries; same results as 11 reduce_list calls).
void reduce_all(const ModifierSet* player, const ModifierSet* object, const std::array<ListClamp, kModListCount>& clamps,
                std::array<float, kModListCount>& out);
// The player-level reveal-range list variant (0x35b0c0 MostExtreme<float>): per category the value
// with the largest |value| (first kept on ties), categories summed ascending. No clamp. Battles reduce the player
// reveal-range list in FogOfWar (fog_of_war.cpp) with the same reduce_by_category (alamo/tactical/category_reduce.hpp).
float reduce_most_extreme(std::span<const ModifierEntry> entries, ModList list);

// Stat helpers (section 4.2/4.3).
inline float apply_bonus(float base, float r) { return base * (1.0f + r); }
inline float defense_factor(float r) { return 1.0f - r; }

// ---- System B -------------------------------------------------------------------------------
enum class ModKind : uint8_t {
    WeaponDelay = 1,
    ShieldRegen = 2,
    ShieldRegenInterval = 3,
    EnergyRegen = 4,
    EnergyRegenInterval = 5,
    Speed = 6,
    TakeDamage = 7,
    CauseDamage = 8,
    FireRate = 9,
    ScatterRadius = 10,
    TargetSpeed = 11,
    TargetTakeDamage = 12,
};
inline constexpr uint32_t kModKindCount = 12;
// Mod_Flag ids [M for the numbering]: 1 WEAPONS_ENABLED, 2 CAN_BE_SQUASHED, 3 ALLOW_TARGET_DIMINISHING_FIREPOWER.
inline constexpr uint32_t kModFlagCount = 4;
inline constexpr uint8_t kFlagAllowDiminishingFirepower = 3;

// UnitAbilityType ids used here (section 5.2).
inline constexpr uint8_t kAbilityDefend = 1;
inline constexpr uint8_t kAbilityPowerToWeapons = 29;
inline constexpr uint8_t kAbilityLuckyShot = 0x22;
inline constexpr uint8_t kAbilityFullSalvo = 62;
inline constexpr uint32_t kUnitAbilityTypeCount = 76;  // 1..75

// One Unit_Abilities_Data record (Mod_Multiplier defaults 1.0, Mod_Flag "unset").
struct UnitAbilityDesc {
    uint8_t ability_type = 0;
    std::array<float, kModKindCount> mult{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::array<int8_t, kModFlagCount> flag{-1, -1, -1, -1};  // -1 = not given (does not take part in the AND)
    uint16_t armor_override = 0xFFFF;  // ability data +0x168 (0xFFFF = none)
    float range_override = 0.0f;       // ability data +0x108 (> 0 = override)
    float inaccuracy_override = 0.0f;  // ability data +0x1b4 (> 0 = override, used UNSCALED)
};

// Get_Unit_Ability_Mod_Multiplier (0x398010) for one object: product over the 2 slots where the
// ability is active, LUCKY_SHOT skipped. `abilities` = the type's slot data (nullptr = empty slot).
float ability_product(const UnitAbilityDesc* const abilities[2], const bool active[2], ModKind kind);
// Squad rule (s78h t03): a TEAM container's value. `own` = the container's own product; `members` =
// EVERY live member's own product (a stand-in leader and 1.0-valued members included). own if != 1,
// else the minimum over `members` (member order, <= replaces); no members -> 1. Members themselves
// never take this value (each reads its own product).
float squad_multiplier(float own, std::span<const float> members);
// Get_Unit_Ability_Mod_Flag (0x397f60): AND across active slots whose ability sets the flag; true default.
bool ability_flag(const UnitAbilityDesc* const abilities[2], const bool active[2], uint8_t flag);

}  // namespace alamo::tactical::combat
