// The stacking-category reduction of the modifier lists (0x33fb70, TACTICAL_COMBAT §4.2): categories in ascending
// order, one value picked per category, the picks summed in that order. Shared by combat (modifiers.cpp), abilities
// (player lists) and fog (player reveal range). Header-only; allocation-free, O(entries x categories).
#pragma once
#include <cstdint>

namespace alamo::tactical {

// `for_each(f)` calls f(int32_t category, float value) for every entry of the list, in the caller's entry order (it is
// called twice per category, so it must visit the same entries each time). Per category the pick starts with the
// category's first entry and `better(v, best)` (strict: first kept on ties) replaces it. The sum is seeded with the
// first category's pick when `seed_with_first`, else it is 0.0f + pick (the two differ only for a first pick of
// -0.0f). An empty list gives 0.0f.
template <class ForEach, class Better>
float reduce_by_category(ForEach&& for_each, Better&& better, bool seed_with_first = true) {
    float total = 0.0f;
    bool any = false;
    bool have_last = false;
    int32_t last = 0;
    for (;;) {
        bool found = false;
        int32_t next = 0;
        for_each([&](int32_t c, float) {
            if (have_last && c <= last) return;
            if (!found || c < next) {
                next = c;
                found = true;
            }
        });
        if (!found) break;
        bool first = true;
        float best = 0.0f;
        for_each([&](int32_t c, float v) {
            if (c != next) return;
            if (first || better(v, best)) best = v;
            first = false;
        });
        total = any || !seed_with_first ? total + best : best;
        any = true;
        last = next;
        have_last = true;
    }
    return total;
}

}  // namespace alamo::tactical
