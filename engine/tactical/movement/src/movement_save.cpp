// N1 stage 2: save / load of the movement side of a tactical battle (contract: alamo/core/save_file.hpp). Layouts are
// documented in the headers ("N1 stage 2"): MovementSystem (movement_system.hpp), PassabilityGrid (passability.hpp),
// SpaceOccupancy (space_path.hpp), CollisionLayer (collision.hpp), FighterCellGrid(s) (fighter.hpp),
// HyperspaceDepartures / EngineControl (hyperspace.hpp). The SpatialTree of the collision candidates saves itself
// (engine/tactical src/tree_save.cpp).
//
// Byte rules: little-endian, field by field, counts before data, floats as bit patterns, containers in their
// canonical (ascending id / insertion) order, cross-references as object ids. Every read parses into temporaries and
// commits only when everything parsed and validated, so a false return leaves the object unchanged.
// Units are serialised by one template (io_unit) used for both directions, so the write and read orders cannot drift.
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/movement/hyperspace.hpp"
#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::tactical::movement {

namespace {

constexpr uint32_t kGridSaveVersion = 1;
constexpr uint32_t kOccupancySaveVersion = 1;
constexpr uint32_t kCollisionSaveVersion = 1;
constexpr uint32_t kFighterGridSaveVersion = 1;
constexpr uint32_t kFighterGridsSaveVersion = 1;
constexpr uint32_t kDeparturesSaveVersion = 1;
constexpr uint32_t kEnginesSaveVersion = 1;
constexpr uint32_t kMovementSaveVersion = 4;  // 2: S7 t06 multi-locomotor fields (UnitDesc, Unit); 3: S7 t14 alternate speed / turn read raw (no 0 = unset); 4: S7 t15 jetpack turn band rate, S8 t09 jetpack_still_frames

// u32 byte length + the bytes (like apps/sim write_section / read_section, which this module cannot include).
template <class F>
void put_section(ByteWriter& w, F&& fn) {
    ByteWriter sub;
    fn(sub);
    w.u32(static_cast<uint32_t>(sub.size()));
    w.raw(sub.bytes());
}
template <class F>
bool get_section(ByteReader& r, F&& fn) {
    const uint32_t n = r.u32();
    const std::span<const uint8_t> d = r.raw(n);
    if (!r.ok()) return false;
    ByteReader sub(d);
    return fn(sub) && sub.ok() && sub.done();
}

// A count followed by items of at least `min_bytes` each: false when it cannot fit in the remaining data.
bool get_count(ByteReader& r, size_t min_bytes, uint32_t& n) {
    n = r.u32();
    return r.ok() && (min_bytes == 0 || n <= r.remaining() / min_bytes);
}

// ---- enum validation (switches without default: -Wswitch flags a new enumerator here) ---------------------------
bool valid_loco_state(uint8_t v) {
    switch (static_cast<LocoState>(v)) {
        case LocoState::None:
        case LocoState::WalkStartTurn:
        case LocoState::WalkTurning:
        case LocoState::WalkEndTurn:
        case LocoState::WalkStartMove:
        case LocoState::WalkMoving:
        case LocoState::WalkEndMove:
        case LocoState::WalkStopped:
        case LocoState::FighterMoving:
        case LocoState::FighterIdle:
        case LocoState::FighterIdleCombat:
        case LocoState::FighterDirectedCombat:
        case LocoState::FighterDeadStop:
        case LocoState::BikeTurningInPlace:
        case LocoState::BikeStartMove:
        case LocoState::BikeMoving:
        case LocoState::BikeEndMove:
        case LocoState::BikePreStopped:
        case LocoState::BikeStopped:
        case LocoState::FlyCircling:
        case LocoState::FlyNoPath: return true;
    }
    return false;
}
bool valid_locomotor(uint8_t v) {
    switch (static_cast<LocomotorKind>(v)) {
        case LocomotorKind::Default:
        case LocomotorKind::Bike:
        case LocomotorKind::Jetpack:
        case LocomotorKind::Flying:
        case LocomotorKind::LandBomber:
        case LocomotorKind::Fighter: return true;
    }
    return false;
}
bool valid_phase(uint8_t v) {
    switch (static_cast<FighterPhase>(v)) {
        case FighterPhase::None:
        case FighterPhase::Approach:
        case FighterPhase::Pass:
        case FighterPhase::BreakOff:
        case FighterPhase::Dogfight:
        case FighterPhase::Chase: return true;
    }
    return false;
}
bool valid_collision_kind(uint8_t v) {
    switch (static_cast<CollisionKind>(v)) {
        case CollisionKind::MovingVsMoving:
        case CollisionKind::MovingVsStopped:
        case CollisionKind::StoppedVsStopped: return true;
    }
    return false;
}
bool valid_collision_state(uint8_t v) {
    switch (static_cast<CollisionState>(v)) {
        case CollisionState::Active:
        case CollisionState::TryAgainLater:
        case CollisionState::Resolved: return true;
    }
    return false;
}
bool valid_fighter_event(uint8_t v) {
    switch (static_cast<FighterEvent::Kind>(v)) {
        case FighterEvent::Provoke:
        case FighterEvent::Retarget:
        case FighterEvent::EnterIdle: return true;
    }
    return false;
}
bool valid_hyperspace_event(uint8_t v) {
    switch (static_cast<HyperspaceEvent::Kind>(v)) {
        case HyperspaceEvent::Kind::Jumped:
        case HyperspaceEvent::Kind::Killed: return true;
    }
    return false;
}
bool valid_engine_event(uint8_t v) {
    switch (static_cast<EngineEvent::Kind>(v)) {
        case EngineEvent::Kind::Off:
        case EngineEvent::Kind::On: return true;
    }
    return false;
}
bool valid_object_id(ObjectId id) { return id >= 0 && static_cast<uint32_t>(id) != kNoEntity; }

// ---- symmetric field I/O ----------------------------------------------------------------------------------------
// Out writes, In reads (validating enums and bounding counts; a failure clears the reader's ok()). The same io_*
// template drives both, with U = const T for Out and T for In.
class Out {
public:
    explicit Out(ByteWriter& w) : w_(w) {}
    void f32(const float& v) { w_.f32(v); }
    void u32(const uint32_t& v) { w_.u32(v); }
    void i32(const int32_t& v) { w_.i32(v); }
    void u8(const uint8_t& v) { w_.u8(v); }
    void b(const bool& v) { w_.boolean(v); }
    void str(const std::string& v) { w_.str(v); }
    template <class E>
    void e8(const E& v, bool (*)(uint8_t)) {
        w_.u8(static_cast<uint8_t>(v));
    }
    void v2(const V2& v) { w_.f32(v.x).f32(v.y); }
    void f3(const F3& v) { w_.f32(v.x).f32(v.y).f32(v.z); }
    void att(const FAttitude& o) { w_.f32(o.roll).f32(o.pitch).f32(o.yaw); }
    void cell(const FighterCell& c) { w_.i32(c.i).i32(c.j); }
    void v2s(const std::vector<V2>& v) {
        w_.u32(static_cast<uint32_t>(v.size()));
        for (const V2& p : v) v2(p);
    }
    void f3s(const std::vector<F3>& v) {
        w_.u32(static_cast<uint32_t>(v.size()));
        for (const F3& p : v) f3(p);
    }
    void ids(const std::vector<ObjectId>& v) {
        w_.u32(static_cast<uint32_t>(v.size()));
        for (const ObjectId id : v) w_.i32(id);
    }
    void wps(const std::vector<SpaceWaypoint>& v) {
        w_.u32(static_cast<uint32_t>(v.size()));
        for (const SpaceWaypoint& p : v) w_.f32(p.pos.x).f32(p.pos.y).f32(p.heading).f32(p.speed).f32(p.time);
    }

private:
    ByteWriter& w_;
};

class In {
public:
    explicit In(ByteReader& r) : r_(r) {}
    void f32(float& v) { v = r_.f32(); }
    void u32(uint32_t& v) { v = r_.u32(); }
    void i32(int32_t& v) { v = r_.i32(); }
    void u8(uint8_t& v) { v = r_.u8(); }
    void b(bool& v) { v = r_.boolean(); }
    void str(std::string& v) { v = r_.str(); }
    template <class E>
    void e8(E& v, bool (*valid)(uint8_t)) {
        const uint8_t x = r_.u8();
        if (!valid(x)) r_.fail();
        v = static_cast<E>(x);
    }
    void v2(V2& v) {
        v.x = r_.f32();
        v.y = r_.f32();
    }
    void f3(F3& v) {
        v.x = r_.f32();
        v.y = r_.f32();
        v.z = r_.f32();
    }
    void att(FAttitude& o) {
        o.roll = r_.f32();
        o.pitch = r_.f32();
        o.yaw = r_.f32();
    }
    void cell(FighterCell& c) {
        c.i = r_.i32();
        c.j = r_.i32();
    }
    void v2s(std::vector<V2>& v) {
        uint32_t n = 0;
        if (!get_count(r_, 8, n)) return r_.fail();
        v.assign(n, V2{});
        for (V2& p : v) v2(p);
    }
    void f3s(std::vector<F3>& v) {
        uint32_t n = 0;
        if (!get_count(r_, 12, n)) return r_.fail();
        v.assign(n, F3{});
        for (F3& p : v) f3(p);
    }
    void ids(std::vector<ObjectId>& v) {
        uint32_t n = 0;
        if (!get_count(r_, 4, n)) return r_.fail();
        v.assign(n, 0);
        for (ObjectId& id : v) id = r_.i32();
    }
    void wps(std::vector<SpaceWaypoint>& v) {
        uint32_t n = 0;
        if (!get_count(r_, 20, n)) return r_.fail();
        v.assign(n, SpaceWaypoint{});
        for (SpaceWaypoint& p : v) {
            p.pos.x = r_.f32();
            p.pos.y = r_.f32();
            p.heading = r_.f32();
            p.speed = r_.f32();
            p.time = r_.f32();
        }
    }

private:
    ByteReader& r_;
};

template <class IO, class D>
void io_desc(IO& io, D& d) {  // UnitDesc
    io.i32(d.id);
    io.v2(d.pos);
    io.f32(d.z);
    io.f32(d.yaw_deg);
    io.f32(d.max_speed);
    io.f32(d.turn_rate);
    io.f32(d.accel);
    io.f32(d.decel);
    io.f32(d.close_enough_angle);
    io.i32(d.movement_class);
    io.i32(d.collision_class);
    io.f32(d.radius);
    io.f32(d.formation_spacing);
    io.b(d.hero);
    io.str(d.type_name);
    io.i32(d.grouping);
    io.i32(d.formation_order);
    io.b(d.team_member);
    io.u32(d.space_layer);
    io.f32(d.layer_z);
    io.i32(d.path_frame_delay);
    io.f32(d.formation_raggedness);
    io.f32(d.max_jiggle_distance);
    io.i32(d.owner);
    io.b(d.crusher);
    io.b(d.supercrusher);
    io.b(d.squashable);
    io.b(d.squashable_by_supercrusher);
    io.u32(d.space_layer_type);
    io.e8(d.locomotor, valid_locomotor);
    io.f32(d.hover_offset);
    io.f32(d.max_lift);
    io.f32(d.max_rate_of_roll);
    io.f32(d.bank_turn_angle);
    io.f32(d.min_speed);
    io.f32(d.strafe_distance);
    io.f32(d.prepare_strafe_height);
    io.b(d.has_pre_turn_anim);
    io.b(d.uses_multiple_locomotors);  // S7 t06 (version 2)
    io.b(d.has_secondary_locomotor);
    io.e8(d.secondary_locomotor, valid_locomotor);
    io.f32(d.alt_max_speed);
    io.f32(d.alt_turn_rate);
}

template <class IO, class T>
void io_target(IO& io, T& t) {  // FighterTarget
    io.i32(t.id);
    io.b(t.point);
    io.i32(t.hard_point);
    io.f3(t.pos);
    io.f32(t.radius);
    io.b(t.dying);
    io.b(t.hidden);
}

template <class IO, class S>
void io_special(IO& io, S& s) {  // SpecialMotion
    io.e8(s.state, valid_loco_state);
    io.i32(s.anim.id);
    io.b(s.anim.loop);
    io.u32(s.anim.frames);
    io.u32(s.anim.length);
    io.f32(s.anim.rate);
    io.b(s.airborne);
    io.b(s.turn_left);
    io.f32(s.turn_band_rate);  // S7 t15 (version 4)
    io.f32(s.lean);
    io.u32(s.idle_frames);
    io.b(s.turn_back);
    io.v2(s.dest);
    io.b(s.circling);
    io.v2(s.circle_c);
    io.f32(s.circle_z);
    io.f32(s.circle_rmin);
    io.f32(s.circle_rmax);
    io.i32(s.circle_target);
    io.b(s.has_target);
    io.v2(s.target);
    io.f32(s.target_z);
    io.f32(s.target_radius);
    io.f32(s.pitch);
    io.v2(s.vel);
    io.f32(s.vel_z);
    io.u32(s.path_idx);
    io.f32(s.clock);
    io.f32(s.pass_z);
}

template <class IO, class P>
void io_special_params(IO& io, P& p) {  // SpecialParams
    io.f32(p.hover_offset);
    io.f32(p.max_lift);
    io.f32(p.roll_rate);
    io.f32(p.bank_turn_angle);
    io.f32(p.min_speed);
    io.f32(p.strafe_distance);
    io.f32(p.prepare_strafe_height);
    io.b(p.has_pre_turn_anim);
}

template <class IO, class F>
void io_fighter(IO& io, F& f) {  // FighterState
    io.f3(f.b.p);
    io.f3(f.b.v);
    io.att(f.b.o);
    io.b(f.b.loop);
    io.f3(f.dest);
    io.att(f.stored);
    io.u32(f.idx);
    io.cell(f.dog);
    io.cell(f.park);
    io.i32(f.lock);
    io.u32(f.lock_start);
    io.u32(f.lock_frames);
    io.i32(f.hold);
    io_target(io, f.target);
    io.i32(f.squad);
    io.e8(f.phase, valid_phase);
}

template <class IO, class U>
void io_unit(IO& io, U& u) {  // MovementSystem::Unit (deduced: the type is private to the system)
    io_desc(io, u.d);
    io.f32(u.lp.max_speed);
    io.f32(u.lp.accel);
    io.f32(u.lp.decel);
    io.f32(u.lp.turn_rate);
    io.u32(u.lp.mask);
    io.f32(u.lp.close_enough_angle);
    io.i32(u.slot);
    io.v2(u.m.pos);
    io.f32(u.m.yaw);
    io.f32(u.m.speed);
    io.e8(u.m.state, valid_loco_state);
    io.v2s(u.m.path);
    io.u32(u.m.next);
    io.u32(u.m.escape_until);
    io.f32(u.z);
    io.b(u.has_goal);
    io.b(u.need_path);
    io.b(u.arrived);
    io.b(u.failed);
    io.v2(u.goal);
    io.f32(u.facing);
    io.u32(u.start_frame);
    io.f32(u.formation_speed);
    io.i32(u.large_hover_index);
    io.u32(u.blocked_frames);
    io.u32(u.replans);
    io.u32(u.waits);
    io.i32(u.check_countdown);
    io.f32(u.speed_cap);
    io.u32(u.hold_until);
    io.v2(u.nudge);
    io.u32(u.nudge_frames);
    io.wps(u.wps);
    io.u32(u.path_start);
    io.b(u.following);
    io.b(u.turning);
    io.b(u.fallback);
    io.f32(u.turn_from);
    io.f32(u.turn_to);
    io.u32(u.turn_start);
    io.u32(u.turn_frames);
    io.f32(u.speed_factor);
    io.f32(u.speed_override);
    io.f32(u.path_lag);
    io.v2(u.jiggle);
    io.b(u.in_formation);
    io.f32(u.threat);
    io_special(io, u.sm);
    io_special_params(io, u.sp);
    io.e8(u.primary_locomotor, valid_locomotor);  // S7 t06 (version 2): d.locomotor is the active one
    io.b(u.on_secondary);
    io_fighter(io, u.fs);
    io.f32(u.fp.max_thrust);
    io.f32(u.fp.min_follow_distance);
    io.f32(u.fp.attack_range);
}

template <class IO, class S>
void io_squad(IO& io, S& s) {  // FighterSquad
    io.i32(s.id);
    io.ids(s.members);
    io.f3s(s.slots);
    io.f32(s.tolerance);
    io.f32(s.error);
    io.f3(s.spread);
    io.cell(s.dog);
    io.cell(s.park);
    io.f3(s.dest);
    io_target(io, s.target);
}

template <class IO, class S>
void io_stats(IO& io, S& s) {  // MovementStats
    io.u32(s.paths_planned);
    io.u32(s.path_failures);
    io.u32(s.replans);
    io.u32(s.collision_checks);
    io.u32(s.dynamic_repaths);
    io.u32(s.quota_deferrals);
    io.u32(s.early_arrivals);
    io.u32(s.blocked_steps);
    io.u32(s.space_fallbacks);
    io.u32(s.collisions_created);
    io.u32(s.collisions_cancelled);
    io.u32(s.collisions_resolved);
    io.u32(s.jetpack_still_frames);  // S8 t09 (version 4)
}

uint64_t collision_pair_key(ObjectId a, ObjectId b) {  // as collision.cpp pair_key
    const uint32_t x = static_cast<uint32_t>(std::min(a, b)), y = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(x) << 32) | y;
}

void put_vec3(ByteWriter& w, Vec3 v) { w.f32(v.x).f32(v.y).f32(v.z); }
Vec3 get_vec3(ByteReader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

}  // namespace

// ================================================================ PassabilityGrid
uint64_t PassabilityGrid::static_digest() const {
    StateHasher h(0x5041535347524944ull);  // "PASSGRID"
    h.u32(w_);
    h.u32(h_);
    h.span(base_);
    h.span(forced_);
    h.boolean(has_water_);
    h.f32(has_water_ ? water_height_ : 0.0f);
    return h.digest();
}

void PassabilityGrid::write(ByteWriter& w) const {
    w.u32(kGridSaveVersion);
    w.u32(w_).u32(h_).u64(static_digest());
    w.u64(version_);
    w.boolean(!counts_.empty());
    const auto put = [&w](const std::map<ObjectId, Stamp>& m) {
        w.u32(static_cast<uint32_t>(m.size()));
        for (const auto& [id, s] : m) {
            w.i32(id).u32(s.bits).u32(static_cast<uint32_t>(s.cells.size()));
            for (const uint32_t c : s.cells) w.u32(c);
        }
    };
    put(stamps_);
    put(secondary_);
}

bool PassabilityGrid::read(ByteReader& r) {
    if (r.u32() != kGridSaveVersion || !r.ok()) return false;
    const uint32_t sw = r.u32(), sh = r.u32();
    const uint64_t digest = r.u64(), version = r.u64();
    const bool counted = r.boolean();
    if (!r.ok() || sw != w_ || sh != h_ || digest != static_digest()) return false;  // another map
    const size_t cells = base_.size();
    std::vector<std::array<uint16_t, 9>> counts;
    if (counted) counts.assign(cells, std::array<uint16_t, 9>{});
    const auto get = [&](std::map<ObjectId, Stamp>& m) {
        uint32_t n = 0;
        if (!get_count(r, 12, n)) return false;
        bool first = true;
        ObjectId prev = 0;
        for (uint32_t k = 0; k < n; ++k) {
            const ObjectId id = r.i32();
            Stamp s;
            s.bits = r.u32();
            uint32_t nc = 0;
            if (!get_count(r, 4, nc) || (!first && id <= prev) || (s.bits & ~0x1FFu) != 0) return false;
            if (nc > 0 && counts.empty()) return false;  // stamps always allocate the counts
            s.cells.resize(nc);
            for (uint32_t i = 0; i < nc; ++i) {
                const uint32_t c = r.u32();
                if (!r.ok() || c >= cells || (i > 0 && c <= s.cells[i - 1])) return false;
                s.cells[i] = c;
                for (uint32_t b = 0; b < 9; ++b) {
                    if (!(s.bits & (1u << b))) continue;
                    if (counts[c][b] == 0xFFFFu) return false;
                    ++counts[c][b];
                }
            }
            m.emplace_hint(m.end(), id, std::move(s));
            first = false;
            prev = id;
        }
        return r.ok();
    };
    std::map<ObjectId, Stamp> stamps, secondary;
    if (!get(stamps) || !get(secondary)) return false;
    stamps_ = std::move(stamps);
    secondary_ = std::move(secondary);
    counts_ = std::move(counts);
    recompute_all();  // effective = base | box | counted bits (the map-derived part matched the digest)
    version_ = version;
    return true;
}

// ================================================================ SpaceOccupancy
void SpaceOccupancy::write(ByteWriter& w) const {
    w.u32(kOccupancySaveVersion);
    w.u32(static_cast<uint32_t>(statics_.size()));
    for (const auto& [id, s] : statics_) w.i32(id).f32(s.c.x).f32(s.c.y).f32(s.r).u32(s.bits);
    w.u32(static_cast<uint32_t>(tracks_.size()));
    for (const auto& [id, t] : tracks_) {
        w.i32(id).u32(t.layer).f32(t.r).u32(t.start);
        Out(w).wps(t.wps);
    }
    w.f32(box_min_.x).f32(box_min_.y).f32(box_max_.x).f32(box_max_.y).boolean(has_box_).u32(window_);
}

bool SpaceOccupancy::read(ByteReader& r) {
    if (r.u32() != kOccupancySaveVersion || !r.ok()) return false;
    IdVec<Static> statics;
    IdVec<Track> tracks;
    uint32_t n = 0;
    if (!get_count(r, 20, n)) return false;
    for (uint32_t k = 0; k < n; ++k) {
        const ObjectId id = r.i32();
        Static s{};
        s.c.x = r.f32();
        s.c.y = r.f32();
        s.r = r.f32();
        s.bits = r.u32();
        if (!r.ok() || (!statics.empty() && id <= statics.rbegin()->first)) return false;
        statics.emplace_back(id, s);
    }
    if (!get_count(r, 20, n)) return false;
    In in(r);
    for (uint32_t k = 0; k < n; ++k) {
        const ObjectId id = r.i32();
        Track t{};
        t.layer = r.u32();
        t.r = r.f32();
        t.start = r.u32();
        in.wps(t.wps);
        if (!r.ok() || t.wps.empty() || (!tracks.empty() && id <= tracks.rbegin()->first)) return false;
        make_pieces(t);
        tracks.emplace_back(id, std::move(t));
    }
    V2 mn, mx;
    in.v2(mn);
    in.v2(mx);
    const bool has_box = r.boolean();
    const uint32_t window = r.u32();
    if (!r.ok()) return false;
    statics_ = std::move(statics);
    tracks_ = std::move(tracks);
    box_min_ = mn;
    box_max_ = mx;
    has_box_ = has_box;
    window_ = window;
    return true;
}

// ================================================================ CollisionLayer
void CollisionLayer::write(ByteWriter& w) const {
    w.u32(kCollisionSaveVersion);
    w.u32(next_seq_);
    w.u32(static_cast<uint32_t>(list_.size()));
    for (const CollisionObject& c : list_) {
        w.u32(c.seq).u8(static_cast<uint8_t>(c.kind)).u8(static_cast<uint8_t>(c.state)).u8(c.participants);
        w.i32(c.a).i32(c.b).u32(c.created).u32(c.crowd_frame).f32(c.crowd).f32(c.priority).u32(c.retry);
    }
    w.u32(static_cast<uint32_t>(order_.size()));
    for (const Step& s : order_) w.u32(s.seq).boolean(s.full);
    w.u32(cancelled_).u32(resolved_);
}

bool CollisionLayer::read(ByteReader& r) {
    if (r.u32() != kCollisionSaveVersion || !r.ok()) return false;
    const uint32_t next_seq = r.u32();
    uint32_t n = 0;
    if (!get_count(r, 35, n)) return false;  // u32 seq, 3 x u8, i32 a / b, u32 x 2, f32 x 2, u32 retry
    std::vector<CollisionObject> list(n);
    std::vector<uint64_t> pairs;
    pairs.reserve(n);
    for (uint32_t k = 0; k < n; ++k) {
        CollisionObject& c = list[k];
        c.seq = r.u32();
        const uint8_t kind = r.u8(), state = r.u8();
        c.participants = r.u8();
        c.a = r.i32();
        c.b = r.i32();
        c.created = r.u32();
        c.crowd_frame = r.u32();
        c.crowd = r.f32();
        c.priority = r.f32();
        c.retry = r.u32();
        if (!r.ok() || !valid_collision_kind(kind) || !valid_collision_state(state) || c.seq >= next_seq ||
            (k > 0 && c.seq <= list[k - 1].seq))
            return false;
        c.kind = static_cast<CollisionKind>(kind);
        c.state = static_cast<CollisionState>(state);
        pairs.push_back(collision_pair_key(c.a, c.b));
    }
    std::sort(pairs.begin(), pairs.end());
    if (std::adjacent_find(pairs.begin(), pairs.end()) != pairs.end()) return false;  // one collision per pair
    if (!get_count(r, 5, n)) return false;
    std::vector<Step> order(n);
    for (Step& s : order) {
        s.seq = r.u32();
        s.full = r.boolean();
    }
    const uint32_t cancelled = r.u32(), resolved = r.u32();
    if (!r.ok()) return false;
    list_ = std::move(list);
    pairs_ = std::move(pairs);
    order_ = std::move(order);
    sort_.clear();
    next_seq_ = next_seq;
    cancelled_ = cancelled;
    resolved_ = resolved;
    return true;
}

// ================================================================ fighter cell grids
void FighterCellGrid::write(ByteWriter& w) const {
    w.u32(kFighterGridSaveVersion);
    w.f32(cx_).f32(cy_).f32(cz_).f32(hx_).f32(hy_).f32(hz_).f32(size_);
    w.u32(capacity_).u32(extra_).boolean(flag_).i32(nx_).i32(ny_);
    uint32_t used = 0;
    for (const auto& c : cells_) used += c.empty() ? 0u : 1u;
    w.u32(used);
    for (size_t k = 0; k < cells_.size(); ++k) {
        if (cells_[k].empty()) continue;
        w.u32(static_cast<uint32_t>(k)).u32(static_cast<uint32_t>(cells_[k].size()));
        for (const Entry& e : cells_[k]) w.i32(e.id).i32(e.owner);
    }
}

bool FighterCellGrid::read(ByteReader& r) {
    if (r.u32() != kFighterGridSaveVersion || !r.ok()) return false;
    F3 c, h;
    c.x = r.f32();
    c.y = r.f32();
    c.z = r.f32();
    h.x = r.f32();
    h.y = r.f32();
    h.z = r.f32();
    const float size = r.f32();
    const uint32_t capacity = r.u32(), extra = r.u32();
    const bool flag = r.boolean();
    const int32_t nx = r.i32(), ny = r.i32();
    if (!r.ok()) return false;
    FighterCellGrid g;
    g.build(c, h, size, capacity, extra, flag);  // the same parameters give the same geometry
    if (g.nx_ != nx || g.ny_ != ny) return false;
    uint32_t used = 0;
    if (!get_count(r, 16, used)) return false;
    int64_t prev = -1;
    for (uint32_t u = 0; u < used; ++u) {
        const uint32_t k = r.u32();
        uint32_t n = 0;
        if (!get_count(r, 8, n) || n == 0 || static_cast<int64_t>(k) <= prev || k >= g.cells_.size()) return false;
        std::vector<Entry>& cell = g.cells_[k];
        cell.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            cell[i].id = r.i32();
            cell[i].owner = r.i32();
            for (uint32_t j = 0; j < i; ++j)
                if (cell[j].id == cell[i].id) return false;  // join refuses duplicates
        }
        prev = static_cast<int64_t>(k);
    }
    if (!r.ok()) return false;
    *this = std::move(g);
    return true;
}

void FighterCellGrids::write(ByteWriter& w) const {
    w.u32(kFighterGridsSaveVersion);
    w.boolean(built);
    put_section(w, [this](ByteWriter& s) { dogfight.write(s); });
    put_section(w, [this](ByteWriter& s) { parking.write(s); });
}

bool FighterCellGrids::read(ByteReader& r) {
    if (r.u32() != kFighterGridsSaveVersion || !r.ok()) return false;
    const bool b = r.boolean();
    FighterCellGrid dog, park;
    if (!r.ok() || !get_section(r, [&](ByteReader& s) { return dog.read(s); }) ||
        !get_section(r, [&](ByteReader& s) { return park.read(s); }))
        return false;
    built = b;
    dogfight = std::move(dog);
    parking = std::move(park);
    return true;
}

// ================================================================ hyperspace departures / engines
void HyperspaceDepartures::write(ByteWriter& w) const {
    w.u32(kDeparturesSaveVersion);
    w.u32(static_cast<uint32_t>(jumps_.size()));
    for (const Jump& j : jumps_) {
        w.i32(j.id).i32(j.timer).u8(j.keep).f32(j.increment);
        put_vec3(w, j.pos);
        put_vec3(w, j.dir);
    }
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const HyperspaceEvent& e : events_) w.u8(static_cast<uint8_t>(e.kind)).i32(e.id).boolean(e.keep_parent_link).u32(e.frame);
}

bool HyperspaceDepartures::read(ByteReader& r) {
    if (r.u32() != kDeparturesSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!get_count(r, 37, n)) return false;
    std::vector<Jump> jumps(n);
    for (uint32_t k = 0; k < n; ++k) {
        Jump& j = jumps[k];
        j.id = r.i32();
        j.timer = r.i32();
        j.keep = r.u8();
        j.increment = r.f32();
        j.pos = get_vec3(r);
        j.dir = get_vec3(r);
        if (!r.ok() || j.keep > 1 || (k > 0 && j.id <= jumps[k - 1].id)) return false;  // ascending id
    }
    if (!get_count(r, 10, n)) return false;
    std::vector<HyperspaceEvent> events(n);
    for (HyperspaceEvent& e : events) {
        const uint8_t kind = r.u8();
        e.id = r.i32();
        e.keep_parent_link = r.boolean();
        e.frame = r.u32();
        if (!valid_hyperspace_event(kind)) return false;
        e.kind = static_cast<HyperspaceEvent::Kind>(kind);
    }
    if (!r.ok()) return false;
    jumps_ = std::move(jumps);
    events_ = std::move(events);
    return true;
}

void EngineControl::write(ByteWriter& w) const {
    w.u32(kEnginesSaveVersion);
    w.u32(static_cast<uint32_t>(list_.size()));
    for (const Entry& e : list_) w.i32(e.id).u8(e.online).u8(e.destroyed).u8(e.indefinite).u32(e.reenable);
    w.u32(static_cast<uint32_t>(events_.size()));
    for (const EngineEvent& e : events_) w.u8(static_cast<uint8_t>(e.kind)).i32(e.id).u32(e.frame);
}

bool EngineControl::read(ByteReader& r) {
    if (r.u32() != kEnginesSaveVersion || !r.ok()) return false;
    uint32_t n = 0;
    if (!get_count(r, 11, n)) return false;
    std::vector<Entry> list(n);
    for (uint32_t k = 0; k < n; ++k) {
        Entry& e = list[k];
        e.id = r.i32();
        e.online = r.u8();
        e.destroyed = r.u8();
        e.indefinite = r.u8();
        e.reenable = r.u32();
        if (!r.ok() || e.online > 1 || e.destroyed > 1 || e.indefinite > 1 || (k > 0 && e.id <= list[k - 1].id)) return false;
    }
    if (!get_count(r, 9, n)) return false;
    std::vector<EngineEvent> events(n);
    for (EngineEvent& e : events) {
        const uint8_t kind = r.u8();
        e.id = r.i32();
        e.frame = r.u32();
        if (!valid_engine_event(kind)) return false;
        e.kind = static_cast<EngineEvent::Kind>(kind);
    }
    if (!r.ok()) return false;
    list_ = std::move(list);
    events_ = std::move(events);
    return true;
}

// ================================================================ MovementSystem
void MovementSystem::write_unit(ByteWriter& w, const Unit& u) {
    Out out(w);
    io_unit(out, u);
}

bool MovementSystem::read_unit(ByteReader& r, Unit& u) {
    In in(r);
    io_unit(in, u);
    return r.ok() && valid_object_id(u.d.id);
}

void MovementSystem::write(ByteWriter& w) const {
    w.u32(kMovementSaveVersion);
    w.u8(static_cast<uint8_t>(cfg_.mode));
    put_section(w, [this](ByteWriter& s) { grid_.write(s); });
    w.boolean(regions_built_);
    put_section(w, [this](ByteWriter& s) { space_.write(s); });
    put_section(w, [this](ByteWriter& s) { air_.write(s); });
    put_section(w, [this](ByteWriter& s) { tree_.write(s); });
    w.u32(quota_used_);
    w.f32(map_floor_);
    Out out(w);
    io_stats(out, stats_);
    w.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) write_unit(w, u);
    put_section(w, [this](ByteWriter& s) { coll_.write(s); });
    w.u32(static_cast<uint32_t>(crush_.size()));
    for (const CrushEvent& e : crush_) w.i32(e.crusher).i32(e.victim);
    out.ids(despawned_);
    w.u32(static_cast<uint32_t>(squads_.size()));
    for (const FighterSquad& s : squads_) io_squad(out, s);
    put_section(w, [this](ByteWriter& s) { fgrids_.write(s); });
    w.u32(static_cast<uint32_t>(fevents_.size()));
    for (const FighterEvent& e : fevents_) w.u8(static_cast<uint8_t>(e.kind)).i32(e.who).i32(e.target);
}

bool MovementSystem::read(ByteReader& r) {
    if (r.u32() != kMovementSaveVersion || !r.ok()) return false;
    if (r.u8() != static_cast<uint8_t>(cfg_.mode) || !r.ok()) return false;
    PassabilityGrid grid = grid_;  // the rebuilt map; read() checks its map-derived part and replaces the stamps
    if (!get_section(r, [&](ByteReader& s) { return grid.read(s); })) return false;
    const bool regions_built = r.boolean();
    SpaceOccupancy space, air;
    SpatialTree tree(tree_.config());
    if (!r.ok() || !get_section(r, [&](ByteReader& s) { return space.read(s); }) ||
        !get_section(r, [&](ByteReader& s) { return air.read(s); }) ||
        !get_section(r, [&](ByteReader& s) { return tree.read(s); }))
        return false;
    const uint32_t quota = r.u32();
    const float map_floor = r.f32();
    In in(r);
    MovementStats stats;
    io_stats(in, stats);
    uint32_t n = 0;
    if (!get_count(r, 200, n)) return false;  // a unit is well over 200 bytes
    std::vector<Unit> units(n);
    for (uint32_t k = 0; k < n; ++k)
        if (!read_unit(r, units[k]) || (k > 0 && units[k].d.id <= units[k - 1].d.id)) return false;
    CollisionLayer coll;
    if (!get_section(r, [&](ByteReader& s) { return coll.read(s); })) return false;
    if (!get_count(r, 8, n)) return false;
    std::vector<CrushEvent> crush(n);
    for (CrushEvent& e : crush) {
        e.crusher = r.i32();
        e.victim = r.i32();
    }
    std::vector<ObjectId> despawned;
    in.ids(despawned);
    if (!get_count(r, 60, n)) return false;
    std::vector<FighterSquad> squads(n);
    for (uint32_t k = 0; k < n; ++k) {
        io_squad(in, squads[k]);
        if (!r.ok() || squads[k].id < 0 || squads[k].members.empty() || (k > 0 && squads[k].id <= squads[k - 1].id)) return false;
    }
    FighterCellGrids fgrids;
    if (!get_section(r, [&](ByteReader& s) { return fgrids.read(s); })) return false;
    if (!get_count(r, 9, n)) return false;
    std::vector<FighterEvent> fevents(n);
    for (FighterEvent& e : fevents) {
        const uint8_t kind = r.u8();
        e.who = r.i32();
        e.target = r.i32();
        if (!valid_fighter_event(kind)) return false;
        e.kind = static_cast<FighterEvent::Kind>(kind);
    }
    if (!r.ok()) return false;
    // Squadron membership both ways (members are fighter units naming their squadron; a unit naming a squadron is in
    // it): the fighter code relies on it.
    const auto unit_of = [&units](ObjectId id) -> Unit* {
        auto it = std::lower_bound(units.begin(), units.end(), id, [](const Unit& u, ObjectId v) { return u.d.id < v; });
        return it != units.end() && it->d.id == id ? &*it : nullptr;
    };
    for (const FighterSquad& s : squads)
        for (const ObjectId m : s.members) {
            const Unit* u = unit_of(m);
            if (!u || u->d.locomotor != LocomotorKind::Fighter || u->fs.squad != s.id) return false;
        }
    for (const Unit& u : units) {
        if (u.fs.squad < 0) continue;
        auto it = std::lower_bound(squads.begin(), squads.end(), u.fs.squad,
                                   [](const FighterSquad& s, ObjectId v) { return s.id < v; });
        if (it == squads.end() || it->id != u.fs.squad ||
            std::find(it->members.begin(), it->members.end(), u.d.id) == it->members.end())
            return false;
    }
    // Commit.
    grid_ = std::move(grid);
    space_ = std::move(space);
    air_ = std::move(air);
    tree_ = std::move(tree);
    quota_used_ = quota;
    map_floor_ = map_floor;
    stats_ = stats;
    units_ = std::move(units);
    coll_ = std::move(coll);
    crush_ = std::move(crush);
    despawned_ = std::move(despawned);
    squads_ = std::move(squads);
    fgrids_ = std::move(fgrids);
    fevents_ = std::move(fevents);
    // The region map is a cache of the grid's effective values (rebuilt, see the header); the rebuild also re-derives
    // every unit's mask slot.
    regions_ = RegionMap{};
    regions_built_ = false;
    if (regions_built) rebuild_regions_if_dirty();
    return true;
}

}  // namespace alamo::tactical::movement
