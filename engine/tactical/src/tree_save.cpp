// N1 stage 2: SpatialTree / TreeSet save and load (spatial_tree.hpp, tree_set.hpp "N1 stage 2"). The trees are
// saved as their entry sets and rebuilt on load; the shape is a cache whose contents never reach a query result or
// a hash (spatial_tree.hpp explains why; tests/test_tree_save.cpp proves it).
#include <algorithm>
#include <utility>
#include <vector>

#include "alamo/tactical/spatial_tree.hpp"
#include "alamo/tactical/tree_set.hpp"

namespace alamo::tactical {

namespace {

constexpr uint32_t kTreeSaveVersion = 1;
constexpr uint32_t kTreeSetSaveVersion = 1;
constexpr size_t kEntryBytes = 4 + 4 + 24;          // handle, mask, box
constexpr size_t kObjectBytes = 4 + 24 + 4 + 4 + 1;  // id, box, mask, owner, flags

void put_box(ByteWriter& w, const Aabb& b) {
    w.f32(b.min.x).f32(b.min.y).f32(b.min.z).f32(b.max.x).f32(b.max.y).f32(b.max.z);
}

Aabb get_box(ByteReader& r) {
    Aabb b;
    b.min.x = r.f32();
    b.min.y = r.f32();
    b.min.z = r.f32();
    b.max.x = r.f32();
    b.max.y = r.f32();
    b.max.z = r.f32();
    return b;
}

}  // namespace

// ---------------------------------------------------------------- SpatialTree
void SpatialTree::write(ByteWriter& w) const {
    w.u32(kTreeSaveVersion);
    w.f32(config_.large_threshold);
    std::vector<std::pair<EntityHandle, uint32_t>> order;
    order.reserve(entries_.size());
    for (uint32_t i = 0; i < entries_.size(); ++i) order.emplace_back(entries_[i].handle, i);
    std::sort(order.begin(), order.end());
    w.u32(static_cast<uint32_t>(order.size()));
    for (const auto& [h, i] : order) {
        const Entry& e = entries_[i];
        w.u32(h).u32(e.mask);
        put_box(w, e.tight);
    }
}

bool SpatialTree::read(ByteReader& r) {
    if (r.u32() != kTreeSaveVersion || !r.ok()) return false;
    const float threshold = r.f32();
    const uint32_t n = r.u32();
    if (!r.ok() || threshold != threshold || n > r.remaining() / kEntryBytes) return false;  // NaN threshold
    struct Item {
        EntityHandle h;
        uint32_t mask;
        Aabb box;
    };
    std::vector<Item> items;
    items.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Item it;
        it.h = r.u32();
        it.mask = r.u32();
        it.box = get_box(r);
        if (!r.ok() || it.h == kNoEntity || !valid(it.box) || (!items.empty() && it.h <= items.back().h)) return false;
        items.push_back(it);
    }
    SpatialTreeConfig c = config_;
    c.large_threshold = threshold;
    SpatialTree t(c);
    t.entries_.reserve(items.size());
    for (const Item& it : items)
        if (!t.insert(it.h, it.box, it.mask)) return false;
    t.rebuild();  // the shape is a pure function of the entry set (exact median, handle tie-break)
    *this = std::move(t);
    return true;
}

// ---------------------------------------------------------------- TreeSet
void TreeSet::write(ByteWriter& w) const {
    w.u32(kTreeSetSaveVersion);
    w.u32(player_count());
    w.f32(all_.config().large_threshold);  // registry order: all, players ascending, obstacle, space_avoidance
    for (const SpatialTree& t : players_) w.f32(t.config().large_threshold);
    w.f32(obstacle_.config().large_threshold);
    w.f32(space_.config().large_threshold);
    std::vector<EntityHandle> ids;
    ids.reserve(objects_.size());
    for (const auto& kv : objects_) ids.push_back(kv.first);  // collected, then sorted: never iterated in map order
    std::sort(ids.begin(), ids.end());
    w.u32(static_cast<uint32_t>(ids.size()));
    for (const EntityHandle id : ids) {
        const Object& o = objects_.at(id);
        w.u32(id);
        put_box(w, o.box);
        w.u32(o.mask);
        w.i32(o.m.owner);
        w.u8(static_cast<uint8_t>((o.m.projectile_collidable ? 1u : 0u) | (o.m.obstacle ? 2u : 0u) |
                                  (o.m.space_avoidance ? 4u : 0u)));
    }
}

bool TreeSet::read(ByteReader& r) {
    if (r.u32() != kTreeSetSaveVersion || !r.ok()) return false;
    const uint32_t players = r.u32();
    if (!r.ok() || players != player_count()) return false;
    std::vector<float> threshold(static_cast<size_t>(players) + 3);
    for (float& t : threshold) {
        t = r.f32();
        if (t != t) return false;  // NaN
    }
    const uint32_t n = r.u32();
    if (!r.ok() || n > r.remaining() / kObjectBytes) return false;
    struct Item {
        EntityHandle id;
        Aabb box;
        uint32_t mask;
        TreeMembership m;
    };
    std::vector<Item> items;
    items.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Item it;
        it.id = r.u32();
        it.box = get_box(r);
        it.mask = r.u32();
        it.m.owner = r.i32();
        const uint8_t flags = r.u8();
        it.m.projectile_collidable = (flags & 1u) != 0;
        it.m.obstacle = (flags & 2u) != 0;
        it.m.space_avoidance = (flags & 4u) != 0;
        if (!r.ok() || flags > 7u || it.id == kNoEntity || !valid(it.box) || (!items.empty() && it.id <= items.back().id))
            return false;
        items.push_back(it);
    }
    TreeSet t(players, all_.config());
    for (const Item& it : items)
        if (!t.add(it.id, it.box, it.mask, it.m)) return false;
    size_t k = 0;
    const auto rebuild = [&](SpatialTree& tree) {  // registry order, as written
        const float lt = threshold[k++];
        if (lt >= 0.0f) tree.rebuild_with_threshold(lt);
        else tree.rebuild();
    };
    rebuild(t.all_);
    for (SpatialTree& p : t.players_) rebuild(p);
    rebuild(t.obstacle_);
    rebuild(t.space_);
    *this = std::move(t);
    return true;
}

}  // namespace alamo::tactical
