// a2m r2: bounty values (bounty.hpp). Moved from apps/game battle_summary.cpp (Playtest 2 t13) so the galactic sim pays
// the auto-resolve bounty with the same rule the results screen shows.
#include "alamo/galactic/bounty.hpp"

#include <algorithm>
#include <cmath>
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
        e.amount = static_cast<float>(objects::c_atof(ascii_trim(s.substr(comma + 1))));
        t.entries.push_back(e);
    }
    return t;
}

int64_t bounty_of(const BountyTable& table, bool has_override, float override_amount, uint64_t categories) {
    float b = 0.0f;
    if (has_override && override_amount >= 0.0f) {
        b = override_amount;  // 0 stays 0: no category fallback (A2M_R2_RE_B §2.2)
    } else {
        // The largest default over the mask's category bits, floored at 0.
        for (const BountyTable::Entry& e : table.entries)
            if ((e.categories & categories) != 0) b = std::max(b, e.amount);
    }
    // 0x22f9c0: rounded half away from zero (std::round).
    return static_cast<int64_t>(std::round(b));
}

int64_t type_bounty(const BountyTable& table, const objects::GameObjectTypes& types, objects::TypeIndex t) {
    if (t >= types.size()) return 0;
    // Override_Bounty_SP is a float field (parser kind 0x8, A2M_R2_RE_B §2.2). It is read from the field's text: the
    // typed view's get_int gives 0 for it (seen on Gallofree_Transport, text "50"). Absent = negative = category default.
    const std::string_view text = ascii_trim(types.get_string(t, "Override_Bounty_SP"));
    const bool has_override = !text.empty();
    return bounty_of(table, has_override, has_override ? static_cast<float>(objects::c_atof(text)) : -1.0f,
                     types.get_mask(t, "CategoryMask"));
}

}  // namespace alamo::galactic
