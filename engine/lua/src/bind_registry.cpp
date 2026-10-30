// Joins the per-family binding tables (vm_internal.hpp) into one list. The first entry for a
// (kind, class, name) wins, so family order matters only for duplicates, which tests reject.
#include <vector>

#include "vm_internal.hpp"

namespace alamo::lua {

std::span<const detail::ImplEntry> detail::implementations() {
    static const std::vector<ImplEntry> all = [] {
        std::vector<ImplEntry> v;
        for (const auto family : {impl_core(), impl_objects(), impl_players(), impl_story(), impl_presentation(), impl_ai(), impl_ai_tactical(),
                                  impl_script_gaps(),  // L1/S4 wave 2b
                                  impl_fog(),          // fog follow-up
                                  impl_script_remaining()})  // L1 final
            v.insert(v.end(), family.begin(), family.end());
        return v;
    }();
    return all;
}

}  // namespace alamo::lua
