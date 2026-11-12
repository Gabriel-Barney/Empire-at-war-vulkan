// a2m r2: the bounty value of an object type (shared by the galactic auto-resolve bounty and the game's auto-resolve
// results screen, apps/game battle_summary.hpp).
//
// Rule (docs/re/A2M_R2_UW_BOUNTY_TRACE.md §1 [H for the traced values]): the type's Override_Bounty_SP when it sets one
// (0 allowed), else the amount of the FIRST GameConstants Default_Bounty_By_Category_SP entry (document order) whose
// category intersects the type's CategoryMask, else 0. Never negative.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/objects/game_object_types.hpp"

namespace alamo::data {
struct GameConstants;
}

namespace alamo::galactic {

// Default_Bounty_By_Category_SP, document order.
struct BountyTable {
    struct Entry {
        uint64_t categories = 0;  // GameObjectCategory mask of the entry's category name (0 = unknown name: never matches)
        int32_t amount = 0;
    };
    std::vector<Entry> entries;
};

BountyTable load_bounty_table(const data::GameConstants& constants, const objects::GameObjectTypes& types);
// Bounty of one object of type `t` (rule above); never negative.
int64_t type_bounty(const BountyTable& table, const objects::GameObjectTypes& types, objects::TypeIndex t);
// Same rule from already-extracted values (tests, hosts without a type database).
int64_t bounty_of(const BountyTable& table, bool has_override, int32_t override_amount, uint64_t categories);

}  // namespace alamo::galactic
