#include "alamo/render/anim/anim_pose_cache.hpp"

#include <algorithm>
#include <cstring>

namespace alamo::render {

void AnimPoseCache::clear() {
    data_.clear();
    visible_.clear();
    any_hidden_.clear();
    offset_.clear();
    count_.clear();
    ids_.clear();
    stats_ = {};
}

void AnimPoseCache::update(const anim::AnimWorld* world, const ParallelFor& pf) {
    clear();
    if (!world) return;
    const std::span<const anim::ObjectId> ids = world->ids();
    const auto n = static_cast<uint32_t>(ids.size());
    ids_.assign(ids.begin(), ids.end());
    offset_.assign(n, 0);
    count_.assign(n, 0);
    any_hidden_.assign(n, 0);
    uint32_t total = 0;
    stats_.rows = n;
    for (uint32_t r = 0; r < n; ++r) {
        const anim::AnimModel* m = world->model(ids[r]);
        const anim::SlotView prim = world->slot(ids[r]);
        const anim::SlotView over = world->slot(ids[r], anim::kOverlay);
        const bool bound = (prim.clip && prim.clip->bound) || (over.clip && over.clip->bound);
        if (!m || !m->has_skeleton || !bound) continue;
        offset_[r] = total;
        count_[r] = static_cast<uint32_t>(m->skeleton.size());
        total += count_[r];
        ++stats_.posed;
    }
    data_.assign(total, assets::Mat3x4{});
    visible_.assign(total, 1);
    static_assert(sizeof(anim::Mat34) == sizeof(assets::Mat3x4), "pose layouts differ");
    pf(n, 16, [&](uint32_t begin, uint32_t end) {
        std::vector<anim::Mat34> tmp;
        std::vector<uint8_t> vis;
        for (uint32_t r = begin; r < end; ++r) {
            if (!count_[r]) continue;
            tmp.assign(count_[r], anim::Mat34{});
            vis.assign(count_[r], 1);
            if (world->world_pose(ids[r], tmp, vis)) {
                std::memcpy(static_cast<void*>(data_.data() + offset_[r]), tmp.data(), tmp.size() * sizeof(assets::Mat3x4));
                std::copy(vis.begin(), vis.end(), visible_.begin() + offset_[r]);
                any_hidden_[r] = std::find(vis.begin(), vis.end(), uint8_t{0}) != vis.end() ? 1 : 0;
            }
        }
    });
}

std::span<const assets::Mat3x4> AnimPoseCache::pose(objects::ObjectId id, size_t bones) const {
    const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (it == ids_.end() || *it != id) return {};
    const auto r = static_cast<size_t>(it - ids_.begin());
    if (count_[r] == 0 || count_[r] != bones) return {};
    return std::span<const assets::Mat3x4>(data_.data() + offset_[r], count_[r]);
}

std::span<const uint8_t> AnimPoseCache::visibility(objects::ObjectId id, size_t bones) const {
    const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (it == ids_.end() || *it != id) return {};
    const auto r = static_cast<size_t>(it - ids_.begin());
    if (count_[r] == 0 || count_[r] != bones || !any_hidden_[r]) return {};
    return std::span<const uint8_t>(visible_.data() + offset_[r], count_[r]);
}

}  // namespace alamo::render
