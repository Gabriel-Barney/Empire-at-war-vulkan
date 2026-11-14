#include "alamo/ai/tactical/perception_world.hpp"

#include <algorithm>

namespace alamo::ai::tactical {

void PerceptionWorld::index() {
    id_index_.clear();
    id_index_.reserve(objects.size());
    for (uint32_t i = 0; i < objects.size(); ++i) id_index_.emplace_back(objects[i].id, i);
    std::sort(id_index_.begin(), id_index_.end());
}

const PerceptionObject* PerceptionWorld::object(int32_t id) const {
    const auto it = std::lower_bound(id_index_.begin(), id_index_.end(), std::pair<int32_t, uint32_t>(id, 0u));
    if (it == id_index_.end() || it->first != id) return nullptr;
    return &objects[it->second];
}

uint8_t PerceptionWorld::rel(int32_t a, int32_t b) const {
    if (a == b) return 0;
    if (!is_player(a) || !is_player(b)) return 1;
    const size_t n = players.size();
    if (relation.size() != n * n) return 1;
    return relation[static_cast<size_t>(a) * n + static_cast<size_t>(b)];
}

const PerceptionType* PerceptionWorld::type_of(const PerceptionObject& o) const {
    if (o.type < 0 || static_cast<size_t>(o.type) >= types.size()) return nullptr;
    return &types[static_cast<size_t>(o.type)];
}

const PerceptionType* PerceptionWorld::rep_type_of(const PerceptionObject& o) const {
    const int32_t t = o.rep_type >= 0 ? o.rep_type : o.type;
    if (t < 0 || static_cast<size_t>(t) >= types.size()) return nullptr;
    return &types[static_cast<size_t>(t)];
}

void PerceptionWorld::object_hard_points(const PerceptionObject& o, std::vector<PerceptionHardPoint>& out) const {
    out.clear();
    if (hard_points) {
        hard_points(o.id, out);
        return;
    }
    out = o.hard_points;
}

}  // namespace alamo::ai::tactical
