// N1 stage 2 - save / load during a land battle (contract: alamo/core/save_file.hpp) for GarrisonSystem and
// CapturePointSystem. Little-endian, field by field, counts first; holders / units / points ascending id (their
// storage order), occupants in entry order, events in emission order. Floats as bit patterns; references are object
// ids and player indices.
//
// GarrisonSystem (version kGarrisonSaveVersion): i32 neutral player; u32 + holders (id, owner, HolderDesc with u32 + entry
// bones, occupants); u32 + units (id, owner, OccupantDesc, state, host, enter_at, bone, awaiting, retry memo A and count);
// u32 + events (with the state an OrderDropped ended).
// CapturePointSystem (version kCaptureSaveVersion): i32 neutral, u32 interval, u32 allowed frame; u32 + points (id, owner,
// position, CapturePointDesc, step, pending, progress, next); u32 + events.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/tactical/garrison/capture.hpp"
#include "alamo/tactical/garrison/garrison.hpp"
#include "alamo/tactical/save_io.hpp"

namespace alamo::tactical::garrison {

using save_io::get_vec3;
using save_io::put_vec3;

namespace {

// 2: s78n t12 review, HolderKind::VehicleThief. 3: s78n t20 batch 2 (FIGHTER_FLYING §7.4): state 2, the entry bones, the
// bone index / move-in-flight / retry memo per unit, the events' `was`; OccupantDesc::radius gone. Only the current version
// is read.
constexpr uint32_t kGarrisonSaveVersion = 3;
constexpr uint32_t kCaptureSaveVersion = 1;

}  // namespace

// ---- GarrisonSystem -------------------------------------------------------------------------------------------------

void GarrisonSystem::write(ByteWriter& w) const {
    w.u32(kGarrisonSaveVersion);
    w.i32(neutral_);
    w.u32(static_cast<uint32_t>(holders_.size()));
    for (const Holder& h : holders_) {
        w.i32(h.id).i32(h.owner).u8(static_cast<uint8_t>(h.d.kind)).u64(h.d.accepts).i32(h.d.slots);
        w.f32(h.d.enter_dist).f32(h.d.exit_dist).f32(h.d.radius).boolean(h.d.can_fire).f32(h.d.extent_len);
        w.u32(static_cast<uint32_t>(h.d.bones.size()));
        for (const Vec3& b : h.d.bones) put_vec3(w, b);
        w.u32(static_cast<uint32_t>(h.inside.size()));
        for (const ObjectId id : h.inside) w.i32(id);
    }
    w.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        w.i32(u.id).i32(u.owner).u64(u.d.category).i32(u.d.value);
        w.u8(static_cast<uint8_t>(u.state)).i32(u.host).u32(u.enter_at);
        w.i32(u.bone).boolean(u.awaiting);
        put_vec3(w, u.memo);
        w.u32(u.retries);
    }
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const GarrisonEvent& e : events_) {
        w.u8(static_cast<uint8_t>(e.kind)).i32(e.unit).i32(e.holder).i32(e.owner);
        put_vec3(w, e.position);
        w.u32(e.frame).u8(e.was);
    }
}

bool GarrisonSystem::read(ByteReader& r) {
    if (r.u32() != kGarrisonSaveVersion || !r.ok()) return false;
    const int32_t neutral = r.i32();
    uint32_t n = 0;
    if (!r.count(n, 46)) return false;
    std::vector<Holder> holders(n);
    for (size_t i = 0; i < holders.size(); ++i) {
        Holder& h = holders[i];
        h.id = r.i32();
        h.owner = r.i32();
        const uint8_t kind = r.u8();
        h.d.accepts = r.u64();
        h.d.slots = r.i32();
        h.d.enter_dist = r.f32();
        h.d.exit_dist = r.f32();
        h.d.radius = r.f32();
        h.d.can_fire = r.boolean();
        h.d.extent_len = r.f32();
        uint32_t nb = 0;
        if (!r.count(nb, 12)) return false;
        h.d.bones.resize(nb);
        for (Vec3& b : h.d.bones) b = get_vec3<Vec3>(r);
        uint32_t ni = 0;
        if (!r.count(ni, 4) || kind > static_cast<uint8_t>(HolderKind::VehicleThief)) return false;
        if (i > 0 && h.id <= holders[i - 1].id) return false;
        h.d.kind = static_cast<HolderKind>(kind);
        h.inside.resize(ni);
        for (ObjectId& id : h.inside) id = r.i32();
        if (!r.ok()) return false;
    }
    if (!r.count(n, 50)) return false;
    std::vector<Unit> units(n);
    for (size_t i = 0; i < units.size(); ++i) {
        Unit& u = units[i];
        u.id = r.i32();
        u.owner = r.i32();
        u.d.category = r.u64();
        u.d.value = r.i32();
        const uint8_t state = r.u8();
        u.host = r.i32();
        u.enter_at = r.u32();
        u.bone = r.i32();
        u.awaiting = r.boolean();
        u.memo = get_vec3<Vec3>(r);
        u.retries = r.u32();
        if (!r.ok() || (i > 0 && u.id <= units[i - 1].id)) return false;
        if (state != static_cast<uint8_t>(State::None) && state != static_cast<uint8_t>(State::Moving) &&
            state != static_cast<uint8_t>(State::Closing) && state != static_cast<uint8_t>(State::Entering) &&
            state != static_cast<uint8_t>(State::Inside))
            return false;
        u.state = static_cast<State>(state);
    }
    if (!r.count(n, 30)) return false;
    std::vector<GarrisonEvent> events(n);
    for (GarrisonEvent& e : events) {
        const uint8_t kind = r.u8();
        e.unit = r.i32();
        e.holder = r.i32();
        e.owner = r.i32();
        e.position = get_vec3<Vec3>(r);
        e.frame = r.u32();
        e.was = r.u8();
        if (!r.ok() || kind > static_cast<uint8_t>(GarrisonEventKind::Reissue)) return false;
        e.kind = static_cast<GarrisonEventKind>(kind);
    }
    if (!r.ok() || !r.done()) return false;
    neutral_ = neutral;
    holders_ = std::move(holders);
    units_ = std::move(units);
    events_ = std::move(events);
    return true;
}

// ---- CapturePointSystem ---------------------------------------------------------------------------------------------

void CapturePointSystem::write(ByteWriter& w) const {
    w.u32(kCaptureSaveVersion);
    w.i32(neutral_).u32(interval_).u32(allowed_frame_);
    w.u32(static_cast<uint32_t>(points_.size()));
    for (const Point& p : points_) {
        w.i32(p.id).i32(p.owner);
        put_vec3(w, p.pos);
        w.f32(p.d.radius).f32(p.d.transition_seconds).boolean(p.d.sticky);
        w.f32(p.step).i32(p.pending).f32(p.progress).u32(p.next);
    }
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const CaptureEvent& e : events_) w.i32(e.point).i32(e.old_owner).i32(e.new_owner).u32(e.frame);
}

bool CapturePointSystem::read(ByteReader& r) {
    if (r.u32() != kCaptureSaveVersion || !r.ok()) return false;
    const int32_t neutral = r.i32();
    const uint32_t interval = r.u32(), allowed = r.u32();
    if (!r.ok() || interval == 0) return false;
    uint32_t n = 0;
    if (!r.count(n, 41)) return false;
    std::vector<Point> points(n);
    for (size_t i = 0; i < points.size(); ++i) {
        Point& p = points[i];
        p.id = r.i32();
        p.owner = r.i32();
        p.pos = get_vec3<Vec3>(r);
        p.d.radius = r.f32();
        p.d.transition_seconds = r.f32();
        p.d.sticky = r.boolean();
        p.step = r.f32();
        p.pending = r.i32();
        p.progress = r.f32();
        p.next = r.u32();
        if (!r.ok() || (i > 0 && p.id <= points[i - 1].id)) return false;
    }
    if (!r.count(n, 16)) return false;
    std::vector<CaptureEvent> events(n);
    for (CaptureEvent& e : events) {
        e.point = r.i32();
        e.old_owner = r.i32();
        e.new_owner = r.i32();
        e.frame = r.u32();
    }
    if (!r.ok() || !r.done()) return false;
    neutral_ = neutral;
    interval_ = interval;
    allowed_frame_ = allowed;
    points_ = std::move(points);
    events_ = std::move(events);
    return true;
}

}  // namespace alamo::tactical::garrison
