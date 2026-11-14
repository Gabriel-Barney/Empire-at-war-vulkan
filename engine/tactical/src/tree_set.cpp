// S6 tree set - see tree_set.hpp.
#include "alamo/tactical/tree_set.hpp"

namespace alamo::tactical {

TreeSet::TreeSet(uint32_t player_count, SpatialTreeConfig config)
    : players_(player_count, SpatialTree(config)), all_(config), obstacle_(config), space_(config) {}

const SpatialTree& TreeSet::for_player(int32_t p) const {
    if (p < 0 || static_cast<uint32_t>(p) >= players_.size()) return all_;
    return players_[static_cast<size_t>(p)];
}

template <class Fn>
void TreeSet::for_trees(const TreeMembership& m, Fn&& fn) {
    if (m.projectile_collidable) {
        fn(all_);
        if (m.owner >= 0 && static_cast<uint32_t>(m.owner) < players_.size()) fn(players_[static_cast<size_t>(m.owner)]);
    }
    if (m.obstacle) fn(obstacle_);
    if (m.space_avoidance) fn(space_);
}

template <class Fn>
void TreeSet::for_all_trees(Fn&& fn) {  // registry order
    fn(all_);
    for (SpatialTree& t : players_) fn(t);
    fn(obstacle_);
    fn(space_);
}

bool TreeSet::add(EntityHandle id, const Aabb& box, uint32_t mask, const TreeMembership& m) {
    if (id == kNoEntity || !valid(box) || objects_.find(id) != objects_.end()) return false;
    objects_.emplace(id, Object{box, mask, m});
    for_trees(m, [&](SpatialTree& t) { t.insert(id, box, mask); });
    return true;
}

bool TreeSet::remove(EntityHandle id) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    for_trees(it->second.m, [&](SpatialTree& t) { t.remove(id); });
    objects_.erase(it);
    return true;
}

bool TreeSet::update(EntityHandle id, const Aabb& box) {
    if (!valid(box)) return false;
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    it->second.box = box;
    for_trees(it->second.m, [&](SpatialTree& t) { t.update(id, box); });
    return true;
}

SpatialTree& TreeSet::tree_at(uint32_t k) {
    const auto p = static_cast<uint32_t>(players_.size());
    if (k == 0) return all_;
    if (k <= p) return players_[k - 1];
    return k == p + 1 ? obstacle_ : space_;
}

uint32_t TreeSet::update_batch(std::span<const BoxUpdate> updates, const ParallelFor& pf) {
    const auto p = static_cast<uint32_t>(players_.size());
    const uint32_t trees = p + 3;
    std::vector<std::vector<uint32_t>> lists(trees);  // per tree: indices into updates, in entry order
    uint32_t applied = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(updates.size()); ++i) {
        const BoxUpdate& u = updates[i];
        if (!valid(u.box)) continue;
        const auto it = objects_.find(u.id);
        if (it == objects_.end()) continue;
        it->second.box = u.box;
        ++applied;
        const TreeMembership& m = it->second.m;
        if (m.projectile_collidable) {
            lists[0].push_back(i);
            if (m.owner >= 0 && static_cast<uint32_t>(m.owner) < p) lists[static_cast<size_t>(m.owner) + 1].push_back(i);
        }
        if (m.obstacle) lists[p + 1].push_back(i);
        if (m.space_avoidance) lists[p + 2].push_back(i);
    }
    pf(trees, 1, [&](uint32_t b, uint32_t e) {
        for (uint32_t k = b; k < e; ++k) {
            SpatialTree& t = tree_at(k);
            for (const uint32_t i : lists[k]) t.update(updates[i].id, updates[i].box);
        }
    });
    return applied;
}

bool TreeSet::set_membership(EntityHandle id, const TreeMembership& m) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    Object& o = it->second;
    for_trees(o.m, [&](SpatialTree& t) { t.remove(id); });
    o.m = m;
    for_trees(o.m, [&](SpatialTree& t) { t.insert(id, o.box, o.mask); });
    return true;
}

bool TreeSet::set_owner(EntityHandle id, int32_t owner) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    TreeMembership m = it->second.m;
    m.owner = owner;
    return set_membership(id, m);
}

bool TreeSet::set_mask(EntityHandle id, uint32_t mask) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    it->second.mask = mask;
    for_trees(it->second.m, [&](SpatialTree& t) { t.set_mask(id, mask); });
    return true;
}

bool TreeSet::set_dead(EntityHandle id, bool dead) {
    const auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    return set_mask(id, dead ? (it->second.mask | kDeadCategory) : (it->second.mask & ~kDeadCategory));
}

const TreeMembership* TreeSet::membership(EntityHandle id) const {
    const auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second.m;
}

uint32_t TreeSet::maintain(double game_time, const ParallelFor& pf) {
    uint32_t n = 0;
    for_all_trees([&](SpatialTree& t) { n += t.maintain(game_time, pf) ? 1u : 0u; });
    return n;
}

void TreeSet::rebuild_all(const ParallelFor& pf) {
    for_all_trees([&](SpatialTree& t) { t.rebuild(pf); });
}

void TreeSet::rebuild_all_with_threshold(float large_threshold, const ParallelFor& pf) {
    for_all_trees([&](SpatialTree& t) { t.rebuild_with_threshold(large_threshold, pf); });
}

void TreeSet::hash_state(const std::function<void(const void*, size_t)>& sink) const {
    uint32_t index = 0;
    auto one = [&](const SpatialTree& t) {
        const unsigned char tag[4] = {static_cast<unsigned char>(index & 0xFFu),
                                      static_cast<unsigned char>((index >> 8) & 0xFFu),
                                      static_cast<unsigned char>((index >> 16) & 0xFFu),
                                      static_cast<unsigned char>((index >> 24) & 0xFFu)};
        sink(tag, 4);
        t.hash_state(sink);
        ++index;
    };
    one(all_);
    for (const SpatialTree& t : players_) one(t);
    one(obstacle_);
    one(space_);
}

}  // namespace alamo::tactical
