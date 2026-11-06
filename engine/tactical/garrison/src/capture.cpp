#include "alamo/tactical/garrison/capture.hpp"

#include <algorithm>

namespace alamo::tactical::garrison {

int32_t CapturePointSystem::index(ObjectId id) const {
    const auto it = std::lower_bound(points_.begin(), points_.end(), id, [](const Point& p, ObjectId x) { return p.id < x; });
    return (it == points_.end() || it->id != id) ? -1 : static_cast<int32_t>(it - points_.begin());
}

bool CapturePointSystem::add_point(ObjectId id, int32_t owner, Vec3 position, const CapturePointDesc& d, uint32_t first_frame) {
    const auto it = std::lower_bound(points_.begin(), points_.end(), id, [](const Point& p, ObjectId x) { return p.id < x; });
    if (it != points_.end() && it->id == id) return false;
    Point p;
    p.id = id;
    p.owner = owner;
    p.pos = position;
    p.d = d;
    const float t = d.transition_seconds;
    p.step = t > 0.0f ? (1.0f / (t * 30.0f)) * static_cast<float>(interval_) : 1.0f;
    p.next = first_frame;
    points_.insert(it, p);
    return true;
}

void CapturePointSystem::remove(ObjectId id) {
    if (const int32_t i = index(id); i >= 0) points_.erase(points_.begin() + i);
}

void CapturePointSystem::set_owner(ObjectId id, int32_t owner) {
    if (const int32_t i = index(id); i >= 0) {
        Point& p = points_[static_cast<size_t>(i)];
        p.owner = owner;
        p.pending = -1;
        p.progress = 0.0f;
    }
}

void CapturePointSystem::reset(ObjectId id, int32_t owner) {
    if (const int32_t i = index(id); i >= 0) {
        Point& p = points_[static_cast<size_t>(i)];
        p.owner = owner;
        p.pending = owner;
        p.progress = 0.0f;
    }
}

void CapturePointSystem::set_position(ObjectId id, Vec3 pos) {
    if (const int32_t i = index(id); i >= 0) points_[static_cast<size_t>(i)].pos = pos;
}

int32_t CapturePointSystem::owner(ObjectId id) const {
    const int32_t i = index(id);
    return i < 0 ? -1 : points_[static_cast<size_t>(i)].owner;
}
int32_t CapturePointSystem::pending(ObjectId id) const {
    const int32_t i = index(id);
    return i < 0 ? -1 : points_[static_cast<size_t>(i)].pending;
}
float CapturePointSystem::progress(ObjectId id) const {
    const int32_t i = index(id);
    return i < 0 ? 0.0f : points_[static_cast<size_t>(i)].progress;
}

int32_t CapturePointSystem::controller(const Point& p, std::span<const Influencer> inf) const {
    if (pad_occupied_ && pad_occupied_(p.id)) return p.owner;  // (a) an occupied build pad keeps its owner
    if (occupant_owner_) {  // (b) a garrison host with occupants
        const int32_t o = occupant_owner_(p.id);
        if (o >= 0) return o;
    }
    const float r2 = p.d.radius * p.d.radius;
    const bool neutral_owned = p.owner < 0 || p.owner == neutral_;
    int32_t ctl = -1;
    bool any = false;
    for (const Influencer& f : inf) {  // (c)
        if (f.id == p.id || f.owner < 0) continue;
        const float dx = f.position.x - p.pos.x, dy = f.position.y - p.pos.y, dz = f.position.z - p.pos.z;
        if (dx * dx + dy * dy + dz * dz > r2) continue;
        any = true;
        if (neutral_owned) {
            if (ctl < 0) ctl = f.owner;
            else if (!allied(ctl, f.owner)) return neutral_;  // contested
        } else if (!allied(p.owner, f.owner)) {
            return neutral_;  // contested
        }
    }
    if (!any) return p.d.sticky ? p.owner : neutral_;  // (d)
    return neutral_owned ? ctl : p.owner;
}

void CapturePointSystem::service(uint32_t frame, std::span<const Influencer> influencers) {
    for (Point& p : points_) {
        if (frame < p.next) continue;
        p.next = frame + interval_;
        if (frame < allowed_frame_ && p.owner >= 0 && p.owner != neutral_ && !(neutral_owner_ && neutral_owner_(p.owner)))
            continue;  // start gate
        const int32_t ctl = controller(p, influencers);
        if (ctl == p.owner) {
            p.progress = p.progress - p.step;
            if (p.progress <= 0.0f) {
                p.progress = 0.0f;
                p.pending = p.owner;
            }
            continue;
        }
        p.pending = ctl;
        p.progress = p.progress + p.step;
        if (p.progress < 1.0f) continue;
        events_.push_back({p.id, p.owner, ctl, frame});
        p.owner = ctl;
        p.progress = 0.0f;
    }
}

void CapturePointSystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(points_.size()));
    for (const Point& p : points_) {
        h.i32(p.id);
        h.i32(p.owner);
        h.i32(p.pending);
        h.f32(p.progress);
        h.u32(p.next);
    }
}

}  // namespace alamo::tactical::garrison
