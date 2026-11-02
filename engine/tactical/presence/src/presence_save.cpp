// PresenceSystem state hash and N1 save / load (save_file.hpp contract).
//
// Layout (version kPresenceSaveVersion):
//   u32 version
//   u32 presence-unit count, then per unit (ascending id): i32 id, u32 due
//   u32 map count, then per map (ascending owner id): i32 owner, u32 bucket count, u32 entry count,
//       per entry in list order: i32 presence id, i32 remaining
// The hash feeds the same values in the same order (without the version).
#include <algorithm>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/presence/presence_system.hpp"

namespace alamo::tactical::presence {

namespace {

constexpr uint32_t kPresenceSaveVersion = 1;

bool get_count(ByteReader& r, uint32_t& n, size_t min_bytes_each) {
    n = r.u32();
    if (!r.ok()) return false;
    if (min_bytes_each > 0 && static_cast<uint64_t>(n) * min_bytes_each > r.remaining()) {
        r.fail();
        return false;
    }
    return true;
}

bool pow2_at_least_8(uint32_t b) { return b >= 8u && (b & (b - 1u)) == 0u; }

// The list order the hash map would hold: each bucket's nodes contiguous and ascending by key.
bool list_order_valid(const PresenceSystem::Map& m) {
    std::vector<uint32_t> seen;  // buckets whose run has ended
    for (size_t i = 0; i < m.list.size(); ++i) {
        const uint32_t b = PresenceSystem::bucket_of(m.list[i].presence, m.buckets);
        if (i > 0 && PresenceSystem::bucket_of(m.list[i - 1].presence, m.buckets) == b) {
            if (!(m.list[i - 1].presence < m.list[i].presence)) return false;
            continue;
        }
        if (std::find(seen.begin(), seen.end(), b) != seen.end()) return false;
        seen.push_back(b);
    }
    return true;
}

}  // namespace

void PresenceSystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const PresenceUnit& u : units_) {
        h.i32(u.id);
        h.u32(u.due);
    }
    h.u32(static_cast<uint32_t>(maps_.size()));
    for (const Map& m : maps_) {
        h.i32(m.owner);
        h.u32(m.buckets);
        h.u32(static_cast<uint32_t>(m.list.size()));
        for (const PresenceEntry& e : m.list) {
            h.i32(e.presence);
            h.i32(e.remaining);
        }
    }
}

void PresenceSystem::write(ByteWriter& w) const {
    w.u32(kPresenceSaveVersion);
    w.u32(static_cast<uint32_t>(units_.size()));
    for (const PresenceUnit& u : units_) w.i32(u.id).u32(u.due);
    w.u32(static_cast<uint32_t>(maps_.size()));
    for (const Map& m : maps_) {
        w.i32(m.owner).u32(m.buckets).u32(static_cast<uint32_t>(m.list.size()));
        for (const PresenceEntry& e : m.list) w.i32(e.presence).i32(e.remaining);
    }
}

bool PresenceSystem::read(ByteReader& r) {
    if (r.u32() != kPresenceSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!get_count(r, n, 8)) return false;
    std::vector<PresenceUnit> units(n);
    for (PresenceUnit& u : units) {
        u.id = r.i32();
        u.due = r.u32();
    }
    if (!r.ok()) return false;
    for (size_t i = 1; i < units.size(); ++i)
        if (!(units[i - 1].id < units[i].id)) return false;

    if (!get_count(r, n, 12)) return false;
    std::vector<Map> maps(n);
    for (Map& m : maps) {
        m.owner = r.i32();
        m.buckets = r.u32();
        uint32_t k = 0;
        if (!get_count(r, k, 8)) return false;
        if (!pow2_at_least_8(m.buckets) || k > m.buckets) return false;
        m.list.resize(k);
        for (PresenceEntry& e : m.list) {
            e.presence = r.i32();
            e.remaining = r.i32();
            if (e.remaining < 1) return false;
        }
        if (!r.ok()) return false;
        for (size_t i = 0; i < m.list.size(); ++i)
            for (size_t j = i + 1; j < m.list.size(); ++j)
                if (m.list[i].presence == m.list[j].presence) return false;
        if (!list_order_valid(m)) return false;
        if (m.list.empty() && m.buckets == 8u) return false;  // never stored
    }
    for (size_t i = 1; i < maps.size(); ++i)
        if (!(maps[i - 1].owner < maps[i].owner)) return false;
    if (!r.ok() || !r.done()) return false;
    units_ = std::move(units);
    maps_ = std::move(maps);
    return true;
}

}  // namespace alamo::tactical::presence
