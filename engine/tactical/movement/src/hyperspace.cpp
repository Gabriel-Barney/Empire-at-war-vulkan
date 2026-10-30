#include "alamo/tactical/movement/hyperspace.hpp"

#include <algorithm>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::movement {

// ---------------------------------------------------------------- HyperspaceDepartures

int32_t HyperspaceDepartures::find(ObjectId id) const {
    const auto it = std::lower_bound(jumps_.begin(), jumps_.end(), id, [](const Jump& j, ObjectId v) { return j.id < v; });
    return (it == jumps_.end() || it->id != id) ? -1 : static_cast<int32_t>(it - jumps_.begin());
}

bool HyperspaceDepartures::schedule(ObjectId id, uint32_t delay, bool keep_parent_link, Vec3 position, Vec3 facing,
                                    float flight_increment) {
    if (find(id) >= 0) return false;
    Jump j;
    j.id = id;
    j.timer = static_cast<int32_t>(delay) + kHyperspaceJumpTimer;
    j.keep = keep_parent_link ? 1 : 0;
    j.increment = flight_increment;
    j.pos = position;
    float fx = facing.x, fy = facing.y;  // along the yaw (obj +0x98)
    const float len2 = fx * fx + fy * fy;
    if (len2 > 0.0f) {
        const float inv = 1.0f / dmath::sqrt(len2);
        fx = fx * inv;
        fy = fy * inv;
    } else {
        fx = 1.0f;
        fy = 0.0f;
    }
    j.dir = Vec3{fx, fy, 0.0f};
    const auto it = std::lower_bound(jumps_.begin(), jumps_.end(), id, [](const Jump& a, ObjectId v) { return a.id < v; });
    jumps_.insert(it, j);
    return true;
}

void HyperspaceDepartures::remove(ObjectId id) {
    const int32_t i = find(id);
    if (i >= 0) jumps_.erase(jumps_.begin() + i);
}

int32_t HyperspaceDepartures::timer(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? -1 : jumps_[static_cast<size_t>(i)].timer;
}

Vec3 HyperspaceDepartures::position(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? Vec3{} : jumps_[static_cast<size_t>(i)].pos;
}

Vec3 HyperspaceDepartures::facing(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? Vec3{1.0f, 0.0f, 0.0f} : jumps_[static_cast<size_t>(i)].dir;
}

void HyperspaceDepartures::tick(uint32_t frame) {
    events_.clear();
    for (Jump& j : jumps_) {
        const float step = j.timer > kHyperspaceJumpTimer ? kHyperspacePreJumpStep : j.increment;
        j.pos = j.pos + j.dir * step;
        if (j.timer == kHyperspaceJumpTimer) events_.push_back({HyperspaceEvent::Kind::Jumped, j.id, j.keep != 0, frame});
        --j.timer;
        if (j.timer <= 0) events_.push_back({HyperspaceEvent::Kind::Killed, j.id, j.keep != 0, frame});
    }
    std::erase_if(jumps_, [](const Jump& j) { return j.timer <= 0; });
}

void HyperspaceDepartures::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(jumps_.size()));
    for (const Jump& j : jumps_) {
        h.i32(j.id);
        h.i32(j.timer);
        h.u8(j.keep);
        h.f32(j.increment);
        h.f32(j.pos.x);
        h.f32(j.pos.y);
        h.f32(j.pos.z);
        h.f32(j.dir.x);
        h.f32(j.dir.y);
    }
}
// ---------------------------------------------------------------- EngineControl

EngineControl::Entry& EngineControl::entry(ObjectId id) {
    auto it = std::lower_bound(list_.begin(), list_.end(), id, [](const Entry& e, ObjectId v) { return e.id < v; });
    if (it == list_.end() || it->id != id) {
        Entry e;
        e.id = id;
        it = list_.insert(it, e);
    }
    return *it;
}

const EngineControl::Entry* EngineControl::find(ObjectId id) const {
    const auto it = std::lower_bound(list_.begin(), list_.end(), id, [](const Entry& e, ObjectId v) { return e.id < v; });
    return (it == list_.end() || it->id != id) ? nullptr : &*it;
}

// 0x5545a0 entry test: (+0xd0 == 1 or +0xd4 >= 0) and +0xd1 == 0 (HARDPOINT_DISABLED.md §9.1) [H].
static bool engines_settable(uint8_t online, uint8_t destroyed, uint8_t indefinite, uint32_t reenable) {
    const bool timed = !indefinite && reenable != 0;
    return (online != 0 || timed) && destroyed == 0;
}

void EngineControl::destroy(ObjectId id, uint32_t frame) {
    Entry& e = entry(id);
    if (!engines_settable(e.online, e.destroyed, e.indefinite, e.reenable)) return;
    const bool was = e.online != 0;
    e.online = 0;  // word +0xd0 := 0x100: online 0, permanent 1; +0xd4 := -1
    e.destroyed = 1;
    e.indefinite = 0;
    e.reenable = 0;
    if (was) events_.push_back({EngineEvent::Kind::Off, id, frame});
}

void EngineControl::disable(ObjectId id, float seconds, uint32_t frame) {
    Entry& e = entry(id);
    if (!engines_settable(e.online, e.destroyed, e.indefinite, e.reenable)) return;
    const bool was = e.online != 0;
    e.online = 0;
    if (seconds < 0.0f) {
        e.indefinite = 1;  // +0xd4 := -1
        e.reenable = 0;
    } else {
        const float f = 30.0f * seconds;  // int(30 t) [H]
        const uint32_t until = frame + (f > 0.0f ? static_cast<uint32_t>(f) : 0u);
        e.reenable = std::max(e.reenable, until);
    }
    if (was) events_.push_back({EngineEvent::Kind::Off, id, frame});
}

void EngineControl::enable(ObjectId id, uint32_t frame) {
    const Entry* f = find(id);
    if (!f) return;  // unknown = online
    Entry& e = entry(id);
    if (e.destroyed) return;
    const bool was = e.online != 0;
    e.online = 1;
    e.indefinite = 0;
    e.reenable = 0;
    if (!was) events_.push_back({EngineEvent::Kind::On, id, frame});
}

bool EngineControl::repair_online(ObjectId id, uint32_t frame) {
    const Entry* f = find(id);
    if (!f || f->online) return false;  // already on: return at once (no signal, no audio)
    Entry& e = entry(id);
    e.online = 1;  // word +0xd0 := 1: online 1, permanent 0; +0xd4 := -1
    e.destroyed = 0;
    e.indefinite = 0;
    e.reenable = 0;
    events_.push_back({EngineEvent::Kind::On, id, frame});
    return true;
}

bool EngineControl::online(ObjectId id) const {
    const Entry* e = find(id);
    return !e || e->online != 0;
}

bool EngineControl::destroyed(ObjectId id) const {
    const Entry* e = find(id);
    return e && e->destroyed != 0;
}

uint32_t EngineControl::reenable_frame(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->reenable : 0u;
}

void EngineControl::remove(ObjectId id) {
    const auto it = std::lower_bound(list_.begin(), list_.end(), id, [](const Entry& e, ObjectId v) { return e.id < v; });
    if (it != list_.end() && it->id == id) list_.erase(it);
}

void EngineControl::tick(uint32_t frame) {
    for (Entry& e : list_) {
        if (e.online || e.destroyed || e.indefinite || e.reenable == 0 || frame < e.reenable) continue;
        e.online = 1;
        e.reenable = 0;
        events_.push_back({EngineEvent::Kind::On, e.id, frame});
    }
}

void EngineControl::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(list_.size()));
    for (const Entry& e : list_) {
        h.i32(e.id);
        h.u8(e.online);
        h.u8(e.destroyed);
        h.u8(e.indefinite);
        h.u32(e.reenable);
    }
}

}  // namespace alamo::tactical::movement
