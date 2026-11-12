// a2m r2: bounty values (bounty.hpp). Moved from apps/game battle_summary.cpp (Playtest 2 t13) so the galactic sim pays
// the auto-resolve bounty with the same rule the results screen shows.
#include "alamo/galactic/bounty.hpp"

#include <algorithm>
#include <string_view>

#include "alamo/core/ascii.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::galactic {

BountyTable load_bounty_table(const data::GameConstants& constants, const objects::GameObjectTypes& types) {
    BountyTable t;
    const objects::EnumConverter& cat = types.enums().get(objects::EnumId::GameObjectCategory);
    for (const auto& [tag, text] : constants.fields) {
        if (!ascii_iequals(tag, "Default_Bounty_By_Category_SP")) continue;
        // "<Category>, <credits>"
        const std::string_view s = text;
        const size_t comma = s.find(',');
        if (comma == std::string_view::npos) continue;
        BountyTable::Entry e;
        if (const auto v = cat.lookup(ascii_trim(s.substr(0, comma)))) e.categories = static_cast<uint64_t>(*v);
        e.amount = static_cast<int32_t>(objects::c_atof(ascii_trim(s.substr(comma + 1))));
        t.entries.push_back(e);
    }
    return t;
}

int64_t bounty_of(const BountyTable& table, bool has_override, int32_t override_amount, uint64_t categories) {
    if (has_override) return std::max<int64_t>(0, override_amount);
    for (const BountyTable::Entry& e : table.entries)
        if ((e.categories & categories) != 0) return std::max<int64_t>(0, e.amount);
    return 0;
}

int64_t type_bounty(const BountyTable& table, const objects::GameObjectTypes& types, objects::TypeIndex t) {
    if (t >= types.size()) return 0;
    // The value is read from the field's text: the typed view's get_int gives 0 for this field (seen on
    // Gallofree_Transport, text "50"), which made every Override_Bounty_SP 0 before a2m r2.
    const std::string_view text = ascii_trim(types.get_string(t, "Override_Bounty_SP"));
    const bool has_override = !text.empty();
    return bounty_of(table, has_override, has_override ? static_cast<int32_t>(objects::c_atof(text)) : 0,
                     types.get_mask(t, "CategoryMask"));
}

}  // namespace alamo::galactic
