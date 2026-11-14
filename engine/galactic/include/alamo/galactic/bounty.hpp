// a2m r2: the bounty value of an object type (shared by the galactic auto-resolve bounty and the game's auto-resolve
// results screen, apps/game battle_summary.hpp).
//
// Rule (docs/re/A2M_R2_RE_B.md §2.2 [H]; values traced in A2M_R2_UW_BOUNTY_TRACE.md §1): the type's float
// Override_Bounty_SP when it is >= 0 (0 stays 0), else (absent or negative) the LARGEST GameConstants
// Default_Bounty_By_Category_SP amount over the category bits of the type's CategoryMask, floored at 0. The result is
// rounded half away from zero (0x22f9c0), as the bounty payment rounds it per object.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/objects/game_object_types_fwd.hpp"

namespace alamo::data {
struct GameConstants;
}

namespace alamo::galactic {

// Default_Bounty_By_Category_SP, document order.
struct BountyTable {
    struct Entry {
        uint64_t categories = 0;  // GameObjectCategory mask of the entry's category name (0 = unknown name: never matches)
        float amount = 0.0f;
    };
    std::vector<Entry> entries;
};

BountyTable load_bounty_table(const data::GameConstants& constants, const objects::GameObjectTypes& types);
// Bounty of one object of type `t` (rule above), rounded; never negative.
int64_t type_bounty(const BountyTable& table, const objects::GameObjectTypes& types, objects::TypeIndex t);
// Same rule from already-extracted values (tests, hosts without a type database).
int64_t bounty_of(const BountyTable& table, bool has_override, float override_amount, uint64_t categories);

}  // namespace alamo::galactic
