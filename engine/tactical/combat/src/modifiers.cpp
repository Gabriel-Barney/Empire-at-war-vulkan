#include "alamo/tactical/combat/modifiers.hpp"

#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::combat {

namespace {

constexpr ListClamp kNone{};
constexpr ListClamp min_clamp(float v) { return ListClamp{true, false, v, 0.0f}; }
constexpr ListClamp max_clamp(float v) { return ListClamp{false, true, 0.0f, v}; }

const std::array<ListClamp, kModListCount> kDefaultClamps = {
    max_clamp(0.75f),   // 0 defense
    min_clamp(-1.0f),   // 1 damage
    min_clamp(-0.99f),  // 2 max health
    min_clamp(-1.0f),   // 3 max shield
    min_clamp(-1.0f),   // 4 max energy
    min_clamp(-0.99f),  // 5 max speed
    min_clamp(-0.99f),  // 6 fire rate
    kNone,              // 7 reveal range
    kNone,              // 8 fire range
    min_clamp(-0.99f),  // 9 ability recharge
    min_clamp(-0.99f),  // 10 build time
};

float apply_clamp(float r, const ListClamp& c) {
    if (c.has_min && r < c.min) r = c.min;
    if (c.has_max && r > c.max) r = c.max;
    return r;
}

// Small stack buffer of (category, value) in merge order.
constexpr uint32_t kStackEntries = 64;
struct Item {
    int32_t category;
    float value;
};

// Stable insertion sort by category (keeps merge order within a category).
void sort_items(Item* items, uint32_t n) {
    for (uint32_t i = 1; i < n; ++i) {
        const Item it = items[i];
        uint32_t j = i;
        while (j > 0 && items[j - 1].category > it.category) {
            items[j] = items[j - 1];
            --j;
        }
        items[j] = it;
    }
}

// Max-then-sum over category-sorted items.
float max_then_sum(const Item* items, uint32_t n) {
    float total = 0.0f;
    bool any = false;
    uint32_t i = 0;
    while (i < n) {
        const int32_t c = items[i].category;
        float best = items[i].value;
        uint32_t j = i + 1;
        for (; j < n && items[j].category == c; ++j)
            if (items[j].value > best) best = items[j].value;  // GreaterThan<float>: first of equals kept
        if (!any) {
            total = best;  // seeded with the first category's maximum
            any = true;
        } else {
            total = total + best;  // Plus<float>, ascending category order
        }
        i = j;
    }
    return total;
}

// Fallback for more than kStackEntries entries of one list: O(n * categories), allocation-free.
float max_then_sum_slow(const ModifierSet* a, const ModifierSet* b, uint8_t list) {
    const ModifierSet* sets[2] = {a, b};
    float total = 0.0f;
    bool any = false;
    bool have_last = false;
    int32_t last = 0;
    for (;;) {
        bool found = false;
        int32_t next = 0;
        for (const ModifierSet* s : sets) {
            if (!s) continue;
            for (const ModifierEntry& e : s->entries()) {
                if (e.list != list || (have_last && e.category <= last)) continue;
                if (!found || e.category < next) {
                    next = e.category;
                    found = true;
                }
            }
        }
        if (!found) break;
        bool first = true;
        float best = 0.0f;
        for (const ModifierSet* s : sets) {
            if (!s) continue;
            for (const ModifierEntry& e : s->entries()) {
                if (e.list != list || e.category != next) continue;
                if (first || e.value > best) best = e.value;
                first = false;
            }
        }
        if (!any) {
            total = best;
            any = true;
        } else {
            total = total + best;
        }
        last = next;
        have_last = true;
    }
    return total;
}

}  // namespace

const std::array<ListClamp, kModListCount>& default_list_clamps() { return kDefaultClamps; }

uint32_t expiry_frame(uint32_t now, float seconds, uint32_t fps) {
    if (!(seconds > 0.0f)) return 0;
    return now + static_cast<uint32_t>(static_cast<int32_t>(static_cast<float>(fps) * seconds + 0.5f));
}

void ModifierSet::insert(ModList list, int32_t category, float value, uint32_t expiry, uint32_t source_crc,
                         int32_t source_id) {
    ModifierEntry e;
    e.value = value;
    e.expiry = expiry;
    e.category = category;
    e.source_crc = source_crc;
    e.source_id = source_id;
    e.list = static_cast<uint8_t>(list);
    entries_.push_back(e);
    ++version_;
}

uint32_t ModifierSet::remove_source(int32_t source_id, uint32_t source_crc) {
    size_t w = 0;
    for (size_t r = 0; r < entries_.size(); ++r) {
        const ModifierEntry& e = entries_[r];
        const bool match = e.source_id == source_id && (source_crc == 0 || e.source_crc == source_crc);
        if (!match) entries_[w++] = e;
    }
    const auto removed = static_cast<uint32_t>(entries_.size() - w);
    if (removed) {
        entries_.resize(w);
        ++version_;
    }
    return removed;
}

void ModifierSet::set_by_source(ModList list, int32_t category, float value, uint32_t expiry, uint32_t source_crc,
                                int32_t source_id) {
    const auto l = static_cast<uint8_t>(list);
    for (ModifierEntry& e : entries_) {
        if (e.list != l || e.source_id != source_id || e.source_crc != source_crc) continue;
        e.value = value;  // replaced in place: keeps its position (and so the first-of-equals rule)
        e.expiry = expiry;
        e.category = category;
        ++version_;
        return;
    }
    insert(list, category, value, expiry, source_crc, source_id);
}

uint32_t ModifierSet::remove_source_in_list(ModList list, int32_t source_id, uint32_t source_crc) {
    const auto l = static_cast<uint8_t>(list);
    size_t w = 0;
    for (size_t r = 0; r < entries_.size(); ++r) {
        const ModifierEntry& e = entries_[r];
        const bool match = e.list == l && e.source_id == source_id && (source_crc == 0 || e.source_crc == source_crc);
        if (!match) entries_[w++] = e;
    }
    const auto removed = static_cast<uint32_t>(entries_.size() - w);
    if (removed) {
        entries_.resize(w);
        ++version_;
    }
    return removed;
}

uint32_t ModifierSet::purge_expired(uint32_t frame) {
    size_t w = 0;
    for (size_t r = 0; r < entries_.size(); ++r)
        if (!(entries_[r].expiry < frame)) entries_[w++] = entries_[r];  // unsigned: kPermanent never
    const auto removed = static_cast<uint32_t>(entries_.size() - w);
    if (removed) {
        entries_.resize(w);
        ++version_;
    }
    return removed;
}

void ModifierSet::clear() {
    if (!entries_.empty()) ++version_;
    entries_.clear();
}

void ModifierSet::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(entries_.size()));
    for (const ModifierEntry& e : entries_) {
        h.u8(e.list);
        h.i32(e.category);
        h.f32(e.value);
        h.u32(e.expiry);
        h.u32(e.source_crc);
        h.i32(e.source_id);
    }
}

float reduce_list(const ModifierSet* player, const ModifierSet* object, ModList list, const ListClamp& clamp) {
    const auto l = static_cast<uint8_t>(list);
    Item items[kStackEntries];
    uint32_t n = 0;
    bool overflow = false;
    const ModifierSet* sets[2] = {player, object};
    for (const ModifierSet* s : sets) {
        if (!s) continue;
        for (const ModifierEntry& e : s->entries()) {
            if (e.list != l) continue;
            if (n == kStackEntries) {
                overflow = true;
                break;
            }
            items[n++] = Item{e.category, e.value};
        }
    }
    float r;
    if (overflow) {
        r = max_then_sum_slow(player, object, l);
    } else {
        sort_items(items, n);
        r = max_then_sum(items, n);
    }
    return apply_clamp(r, clamp);
}

void reduce_all(const ModifierSet* player, const ModifierSet* object, const std::array<ListClamp, kModListCount>& clamps,
                std::array<float, kModListCount>& out) {
    const size_t total = (player ? player->entries().size() : 0) + (object ? object->entries().size() : 0);
    if (total == 0) {
        for (uint32_t i = 0; i < kModListCount; ++i) out[i] = apply_clamp(0.0f, clamps[i]);
        return;
    }
    if (total > kStackEntries) {
        for (uint32_t i = 0; i < kModListCount; ++i)
            out[i] = reduce_list(player, object, static_cast<ModList>(i), clamps[i]);
        return;
    }
    // One pass: bucket by list (merge order kept), then sort + reduce each bucket.
    Item items[kStackEntries];
    uint8_t lists[kStackEntries];
    uint32_t n = 0;
    uint32_t present = 0;  // bit per list
    const ModifierSet* sets[2] = {player, object};
    for (const ModifierSet* s : sets) {
        if (!s) continue;
        for (const ModifierEntry& e : s->entries()) {
            if (e.list >= kModListCount) continue;
            items[n] = Item{e.category, e.value};
            lists[n] = e.list;
            present |= 1u << e.list;
            ++n;
        }
    }
    for (uint32_t li = 0; li < kModListCount; ++li) {
        if (!(present & (1u << li))) {
            out[li] = apply_clamp(0.0f, clamps[li]);
            continue;
        }
        Item bucket[kStackEntries];
        uint32_t m = 0;
        for (uint32_t k = 0; k < n; ++k)
            if (lists[k] == li) bucket[m++] = items[k];
        sort_items(bucket, m);
        out[li] = apply_clamp(max_then_sum(bucket, m), clamps[li]);
    }
}

float reduce_most_extreme(std::span<const ModifierEntry> entries, ModList list) {
    const auto l = static_cast<uint8_t>(list);
    float total = 0.0f;
    bool any = false;
    bool have_last = false;
    int32_t last = 0;
    for (;;) {
        bool found = false;
        int32_t next = 0;
        for (const ModifierEntry& e : entries) {
            if (e.list != l || (have_last && e.category <= last)) continue;
            if (!found || e.category < next) {
                next = e.category;
                found = true;
            }
        }
        if (!found) break;
        bool first = true;
        float best = 0.0f;
        for (const ModifierEntry& e : entries) {
            if (e.list != l || e.category != next) continue;
            const float a = e.value < 0.0f ? -e.value : e.value;
            const float b = best < 0.0f ? -best : best;
            if (first || a > b) best = e.value;  // largest |value|, first kept on ties
            first = false;
        }
        if (!any) {
            total = best;
            any = true;
        } else {
            total = total + best;
        }
        last = next;
        have_last = true;
    }
    return total;
}

float ability_product(const UnitAbilityDesc* const abilities[2], const bool active[2], ModKind kind) {
    const auto k = static_cast<uint32_t>(kind) - 1u;
    float p = 1.0f;
    for (int s = 0; s < 2; ++s) {
        const UnitAbilityDesc* a = abilities[s];
        if (!a || !active[s] || a->ability_type == kAbilityLuckyShot || k >= kModKindCount) continue;
        p = p * a->mult[k];
    }
    return p;
}

float squad_multiplier(float own, std::span<const float> members) {
    if (own != 1.0f || members.empty()) return own;
    float best = members[0];
    for (size_t i = 1; i < members.size(); ++i)
        if (members[i] < best) best = members[i];
    return best;
}

bool ability_flag(const UnitAbilityDesc* const abilities[2], const bool active[2], uint8_t flag) {
    if (flag >= kModFlagCount) return true;
    bool r = true;
    for (int s = 0; s < 2; ++s) {
        const UnitAbilityDesc* a = abilities[s];
        if (!a || !active[s] || a->flag[flag] < 0) continue;
        r = r && a->flag[flag] != 0;
    }
    return r;
}

}  // namespace alamo::tactical::combat
