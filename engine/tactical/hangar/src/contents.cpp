#include "alamo/tactical/hangar/contents.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::hangar {

// ---- ContentsSystem ----

int32_t ContentsSystem::find(ObjectId container) const {
    const auto it = std::lower_bound(lists_.begin(), lists_.end(), container,
                                     [](const List& l, ObjectId v) { return l.container < v; });
    return (it != lists_.end() && it->container == container) ? static_cast<int32_t>(it - lists_.begin()) : -1;
}

bool ContentsSystem::add(ObjectId container, ObjectId object) {
    if (container == kNoObject || object == kNoObject || container == object || container_of(object) != kNoObject) return false;
    int32_t i = find(container);
    if (i < 0) {
        const auto it = std::lower_bound(lists_.begin(), lists_.end(), container,
                                         [](const List& l, ObjectId v) { return l.container < v; });
        List l;
        l.container = container;
        i = static_cast<int32_t>(lists_.insert(it, std::move(l)) - lists_.begin());
    }
    lists_[static_cast<size_t>(i)].objects.push_back(object);
    return true;
}

bool ContentsSystem::remove(ObjectId object) {
    for (size_t i = 0; i < lists_.size(); ++i) {
        std::vector<ObjectId>& v = lists_[i].objects;
        const auto it = std::find(v.begin(), v.end(), object);
        if (it == v.end()) continue;
        v.erase(it);
        if (v.empty()) lists_.erase(lists_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

std::vector<ObjectId> ContentsSystem::remove_container(ObjectId container) {
    // Its own membership in another list goes too.
    remove(container);
    const int32_t i = find(container);
    if (i < 0) return {};
    std::vector<ObjectId> out = std::move(lists_[static_cast<size_t>(i)].objects);
    lists_.erase(lists_.begin() + i);
    return out;
}

uint32_t ContentsSystem::move_all(ObjectId from, ObjectId to) {
    if (from == to || to == kNoObject) return 0;
    const std::span<const ObjectId> cur = contents(from);
    const std::vector<ObjectId> snap(cur.begin(), cur.end());
    uint32_t n = 0;
    for (const ObjectId o : snap) {
        remove(o);
        if (add(to, o)) ++n;
    }
    return n;
}

ObjectId ContentsSystem::container_of(ObjectId object) const {
    if (object == kNoObject) return kNoObject;
    for (const List& l : lists_)
        if (std::find(l.objects.begin(), l.objects.end(), object) != l.objects.end()) return l.container;
    return kNoObject;
}

std::span<const ObjectId> ContentsSystem::contents(ObjectId container) const {
    const int32_t i = find(container);
    if (i < 0) return {};
    return lists_[static_cast<size_t>(i)].objects;
}

void ContentsSystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(lists_.size()));
    for (const List& l : lists_) {
        h.i32(l.container);
        h.u32(static_cast<uint32_t>(l.objects.size()));
        for (const ObjectId o : l.objects) h.i32(o);
    }
}

void ContentsSystem::register_crc(CrcRegistry& registry, const std::string& prefix) const {
    registry.add(prefix + ".contents", [this](StateHasher& h) { hash(h); });
}

// ---- TacticalProduction ----

std::vector<BuildOrder>& TacticalProduction::list(int32_t player) {
    const auto p = static_cast<size_t>(player);
    if (p >= players_.size()) players_.resize(p + 1);
    return players_[p];
}

uint32_t TacticalProduction::enqueue(int32_t player, ObjectId builder, int32_t type, float seconds, uint32_t frame) {
    if (player < 0 || builder == kNoObject) return 0;
    BuildOrder o;
    o.id = next_id_++;
    o.player = player;
    o.builder = builder;
    o.type = type;
    const float f = seconds * static_cast<float>(fps_);
    o.total_frames = f >= 1.0f ? static_cast<uint32_t>(f) : 1u;
    o.frames_left = o.total_frames;
    o.issued = frame;
    list(player).push_back(o);
    return o.id;
}

bool TacticalProduction::cancel(uint32_t order) {
    for (std::vector<BuildOrder>& l : players_) {
        const auto it = std::find_if(l.begin(), l.end(), [&](const BuildOrder& o) { return o.id == order; });
        if (it == l.end()) continue;
        l.erase(it);
        return true;
    }
    return false;
}

uint32_t TacticalProduction::retarget(int32_t player, ObjectId old_builder, ObjectId new_builder) {
    if (player < 0 || static_cast<size_t>(player) >= players_.size()) return 0;
    uint32_t n = 0;
    for (BuildOrder& o : players_[static_cast<size_t>(player)])
        if (o.builder == old_builder) {
            o.builder = new_builder;
            ++n;
        }
    return n;
}

std::vector<BuildOrder> TacticalProduction::builder_removed(ObjectId builder) {
    std::vector<BuildOrder> out;
    for (std::vector<BuildOrder>& l : players_) {
        for (const BuildOrder& o : l)
            if (o.builder == builder) out.push_back(o);
        l.erase(std::remove_if(l.begin(), l.end(), [&](const BuildOrder& o) { return o.builder == builder; }), l.end());
    }
    std::sort(out.begin(), out.end(), [](const BuildOrder& a, const BuildOrder& b) { return a.id < b.id; });
    return out;
}

void TacticalProduction::update(uint32_t frame) {
    (void)frame;
    for (std::vector<BuildOrder>& l : players_) {
        std::vector<ObjectId> seen;  // builders whose first order was already advanced this frame
        for (size_t i = 0; i < l.size();) {
            BuildOrder& o = l[i];
            if (std::find(seen.begin(), seen.end(), o.builder) != seen.end()) {
                ++i;
                continue;
            }
            seen.push_back(o.builder);
            if (o.frames_left > 0) o.frames_left -= 1;
            if (o.frames_left == 0) {
                done_.push_back(o);
                l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
}

std::span<const BuildOrder> TacticalProduction::queue(int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= players_.size()) return {};
    return players_[static_cast<size_t>(player)];
}

uint32_t TacticalProduction::count_type(int32_t player, int32_t type) const {
    uint32_t n = 0;
    for (const BuildOrder& o : queue(player)) n += o.type == type ? 1u : 0u;
    return n;
}

void TacticalProduction::hash(StateHasher& h) const {
    h.u32(next_id_);
    h.u32(static_cast<uint32_t>(players_.size()));
    for (const std::vector<BuildOrder>& l : players_) {
        h.u32(static_cast<uint32_t>(l.size()));
        for (const BuildOrder& o : l) {
            h.u32(o.id);
            h.i32(o.builder);
            h.i32(o.type);
            h.u32(o.total_frames);
            h.u32(o.frames_left);
        }
    }
}

void TacticalProduction::register_crc(CrcRegistry& registry, const std::string& prefix) const {
    registry.add(prefix + ".production", [this](StateHasher& h) { hash(h); });
}

}  // namespace alamo::tactical::hangar
