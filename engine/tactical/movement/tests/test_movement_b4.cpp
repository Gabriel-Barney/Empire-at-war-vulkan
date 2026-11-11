// alamo_tactical_movement_b4_tests (no framework; non-zero exit on failure).
// s78n t24 (t20 batch 4): TerrainCollision (MOVEMENT §5.4.7) - a walk step that would end on ground the unit may not enter
// is not taken (speed 0) and makes a one-participant record; its decision runs Path Around with no obstacle (kind 3, D =
// r_u + 40) and the unit turns in place onto the detour's first leg before it moves, or the unit is stopped and loses its
// paths. No blocked-frame count, no re-plan of the ordered path.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/test/support.hpp"
#include "alamo/test/thread_parallel_for.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;

namespace {

ParallelFor make_thread_pf(unsigned threads) { return ::alamo::test::make_thread_parallel_for(threads); }

PassabilityGrid open_grid(uint32_t w, uint32_t h) {
    PassabilityGrid g;
    g.init(w, h);
    return g;
}

void land_system(MovementSystem& sys, uint32_t cells) {
    PassabilityGrid g = open_grid(cells, cells);
    const float hi = static_cast<float>(g.width() - 1) * kCellSize;
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float, float) { return 0.0f; });
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
}

UnitDesc vehicle(ObjectId id, V2 pos, int32_t owner, float speed = 1.0f) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = speed;
    d.turn_rate = 6.0f;
    d.alt_max_speed = speed;
    d.alt_turn_rate = 6.0f;
    d.movement_class = 2;
    d.collision_class = kUccVehicle;
    d.radius = 12.0f;
    d.hard_x = 10.0f;
    d.hard_y = 6.0f;
    d.owner = owner;
    d.type_name = "B4_VEHICLE";
    return d;
}

uint32_t terrain_records(const MovementSystem& sys, ObjectId id) {
    uint32_t n = 0;
    for (const CollisionObject& c : sys.collisions().list())
        if (c.kind == CollisionKind::Terrain && c.a == id) ++n;
    return n;
}

const CollisionObject* terrain_record(const MovementSystem& sys, ObjectId id) {
    for (const CollisionObject& c : sys.collisions().list())
        if (c.kind == CollisionKind::Terrain && c.a == id) return &c;
    return nullptr;
}

void wall(MovementSystem& sys, int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x) sys.grid().set_base(x, y, kPassImpassable);
}

// ---------------------------------------------------------------- the detour
// A vehicle (10 x 6, speed 1, turn rate 6) ordered along y = 400 from x = 200 to x = 800; once it moves, a wall of cells
// x = 20, y = 17 .. 23 is stamped across its path. The step into the wall fails: speed 0, the position unchanged, one
// Terrain record with one participant (made in the commit, decided on the next tick). The decision pushes a kind-2 detour
// owned by the record whose rejoin point lies at least D + 2 = r_u + 42 ahead along the ordered path; the vehicle turns
// in place at speed 0 (6 degrees a tick) until it faces the detour's first leg exactly, then walks it; the record ends
// when the detour is popped; the order arrives with no re-plan. Serial and 4 workers give the same digest every tick.
struct DetourOut {
    bool failed_step_ok = false, record_ok = false, detour_ok = false, turned_ok = false, ended_ok = false, arrived = false;
    uint32_t fail_frame = 0, turn_frames = 0, pop_frame = 0, records = 0, detours = 0, replans = 0;
    float rejoin_ahead = 0.0f, first_leg = 0.0f, yaw_at_move = 0.0f;
    std::vector<uint64_t> crc;
};

DetourOut run_detour(const ParallelFor& pf) {
    DetourOut r;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(vehicle(10, cell_center(10, 20), 1)));
    SimRng rng(401);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(40, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 40; ++f) sys.tick(f, rng, pf);
    CHECK(sys.view(10).has_goal && sys.view(10).speed > 0.0f && sys.view(10).pos.x < 360.0f);
    wall(sys, 20, 20, 17, 23);
    const float ru = std::sqrt(10.0f * 10.0f + 6.0f * 6.0f);
    bool turning = false, moved_after = false;
    float heading = 0.0f;
    for (; f < 2000; ++f) {
        const UnitView b = sys.view(10);
        const uint32_t rec0 = sys.stats().terrain_records;
        sys.tick(f, rng, pf);
        StateHasher h;
        sys.hash_units(h);
        r.crc.push_back(h.digest());
        const UnitView v = sys.view(10);
        if (r.fail_frame == 0 && sys.stats().terrain_records == rec0 + 1) {
            r.fail_frame = f;
            const CollisionObject* c = terrain_record(sys, 10);
            r.failed_step_ok = v.speed == 0.0f && v.pos == b.pos && !v.pushed;
            r.record_ok = c && c->participants == 1 && c->state == CollisionState::Active && terrain_records(sys, 10) == 1;
            continue;
        }
        if (r.fail_frame != 0 && f == r.fail_frame + 1) {
            const std::span<const V2> top = sys.top_path(10);
            const CollisionObject* c = terrain_record(sys, 10);
            r.detour_ok = v.pushed && v.push_kind == 2 && top.size() >= 2 && c && c->state == CollisionState::Resolving;
            if (top.size() >= 2) {
                r.rejoin_ahead = top.back().x - b.pos.x;  // the ordered path runs along +x
                heading = heading_deg(top[1] - top[0]);
                r.first_leg = heading;
            }
            turning = true;
        }
        if (turning && !moved_after) {
            if (v.pos == b.pos) {
                // turning in place: speed 0, the yaw steps by the turn rate toward the first leg
                if (v.speed == 0.0f && std::fabs(wrap_deg(heading - v.yaw)) < std::fabs(wrap_deg(heading - b.yaw))) ++r.turn_frames;
            } else {
                moved_after = true;
                r.yaw_at_move = v.yaw;
                // it starts to move only when the start-of-tick facing was within one tick's turn of the leg, and the
                // move lands the facing on the leg exactly
                r.turned_ok = std::fabs(wrap_deg(heading - b.yaw)) <= 6.0f + 1e-4f && std::fabs(wrap_deg(heading - v.yaw)) < 1e-3f;
            }
        }
        if (moved_after && r.pop_frame == 0 && !v.pushed) {
            r.pop_frame = f;
            r.ended_ok = terrain_records(sys, 10) == 0;
        }
        if (r.pop_frame != 0 && !v.has_goal) break;
    }
    r.arrived = sys.view(10).arrived && distance(sys.view(10).pos, cell_center(40, 20)) < 1.0f;
    r.records = sys.stats().terrain_records;
    r.detours = sys.stats().terrain_detours;
    r.replans = sys.stats().replans;
    (void)ru;
    return r;
}

void test_terrain_detour() {
    ::alamo::test::section("terrain_detour");
    const DetourOut s = run_detour(serial_for), p = run_detour(make_thread_pf(4));
    const float ru = std::sqrt(10.0f * 10.0f + 6.0f * 6.0f);
    CHECK(s.fail_frame > 0 && s.failed_step_ok && s.record_ok);
    CHECK(s.detour_ok && s.rejoin_ahead >= ru + 42.0f - 1e-3f);
    CHECK(s.turn_frames >= 3 && s.turned_ok);
    CHECK(s.pop_frame > 0 && s.ended_ok && s.arrived);
    CHECK(s.records == 1 && s.detours == 1 && s.replans == 0);
    CHECK(s.crc == p.crc);
    std::printf("terrain detour: failed step at %u, rejoin %.2f ahead (>= r_u + 42 = %.2f), first leg %.2f deg, %u turning "
                "ticks, yaw at the first move %.4f, popped at %u, arrived %d, records %u, detours %u, re-plans %u, serial = 4 "
                "workers %d (%zu ticks)\n",
                s.fail_frame, static_cast<double>(s.rejoin_ahead), static_cast<double>(ru + 42.0f), static_cast<double>(s.first_leg),
                s.turn_frames, static_cast<double>(s.yaw_at_move), s.pop_frame, s.arrived ? 1 : 0, s.records, s.detours, s.replans,
                s.crc == p.crc ? 1 : 0, s.crc.size());
}

// ---------------------------------------------------------------- no detour: stopped
// The vehicle is on the last leg (goal x = 600) when every cell x >= 29 is stamped: no rejoin candidate along the path
// (s0 lies past its end), none past the end or around it in front of E. The record stops the unit: no goal, arrived,
// speed 0, its paths gone; no further record and no motion for 200 frames.
void test_terrain_stop_last_leg() {
    ::alamo::test::section("terrain_stop_last_leg");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(vehicle(10, cell_center(10, 20), 1)));
    SimRng rng(402);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(30, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 900 && sys.view(10).pos.x < 520.0f; ++f) sys.tick(f, rng, serial_for);
    wall(sys, 29, 99, 0, 99);
    for (; f < 900 && sys.stats().terrain_records == 0; ++f) sys.tick(f, rng, serial_for);
    const uint32_t fail = f;
    sys.tick(f++, rng, serial_for);  // the decision
    const UnitView v = sys.view(10);
    CHECK(fail < 900 && !v.has_goal && v.arrived && v.speed == 0.0f && !v.pushed && sys.land_path(10).empty());
    CHECK(sys.stats().terrain_detours == 0 && sys.stats().terrain_stops_failed == 1 && terrain_records(sys, 10) == 0);
    const V2 at = v.pos;
    for (uint32_t n = 0; n < 200; ++n) sys.tick(f++, rng, serial_for);
    CHECK(sys.stats().terrain_records == 1 && sys.view(10).pos == at && sys.view(10).speed == 0.0f);
    std::printf("terrain stop (last leg): failed step at %u, stopped at x = %.2f (goal 600), arrived %d, 200 frames on: %u record(s)\n",
                fail, static_cast<double>(at.x), v.arrived ? 1 : 0, sys.stats().terrain_records);
}

// ---------------------------------------------------------------- an order-less pushed unit
// Vehicle 10 on a forced path (flag 0x20) pushes the standing vehicle 11 aside (MovingVsStopped step 2). Walled in two
// cells out, 11's push walk fails: its record finds no kind-1 record (it has no order), so the unit is stopped and its
// push record is cleared - the push owner is another record, so it brakes (here from speed 0: at once).
void test_terrain_pushed_orderless() {
    ::alamo::test::section("terrain_pushed_orderless");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc a = vehicle(10, cell_center(10, 20), 1);
    a.accel = a.decel = 0.05f;
    UnitDesc b = vehicle(11, cell_center(18, 20), 1, 0.2f);
    b.accel = b.decel = 0.05f;
    CHECK(sys.add_unit(a) && sys.add_unit(b));
    SimRng rng(403);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(40, 20);
    o.flags = kMoveFlagSkipValidation;
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 600 && !sys.view(11).pushed; ++f) sys.tick(f, rng, serial_for);
    CHECK(sys.view(11).pushed && sys.view(11).push_kind == 1 && !sys.view(11).has_goal);
    const V2 kp = sys.view(11).pos;
    const int32_t cx = world_to_cell(kp.x), cy = world_to_cell(kp.y);
    for (int32_t y = cy - 2; y <= cy + 2; ++y)
        for (int32_t x = cx - 2; x <= cx + 2; ++x)
            if (std::abs(x - cx) == 2 || std::abs(y - cy) == 2) sys.grid().set_base(x, y, kPassImpassable);
    for (uint32_t n = 0; n < 200 && sys.view(11).pushed; ++n, ++f) sys.tick(f, rng, serial_for);
    CHECK(!sys.view(11).pushed && sys.view(11).speed == 0.0f && !sys.view(11).has_goal);
    CHECK(sys.stats().terrain_records >= 1 && sys.stats().terrain_stops_nobase >= 1 && sys.stats().terrain_detours == 0);
    std::printf("terrain stop (order-less pushed unit): push record cleared at frame %u, records %u\n", f,
                sys.stats().terrain_records);
}

// ---------------------------------------------------------------- a forced path makes no record
void test_terrain_forced_path() {
    ::alamo::test::section("terrain_forced_path");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(vehicle(10, cell_center(10, 20), 1)));
    SimRng rng(404);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(40, 20);
    o.flags = kMoveFlagSkipValidation;
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 20; ++f) sys.tick(f, rng, serial_for);
    wall(sys, 20, 20, 17, 23);
    for (; f < 1500 && sys.view(10).has_goal; ++f) sys.tick(f, rng, serial_for);
    CHECK(!sys.view(10).has_goal && sys.view(10).arrived && sys.view(10).pos.x > 790.0f);
    CHECK(sys.stats().terrain_records == 0 && sys.stats().blocked_steps == 0);
    std::printf("terrain forced path: walked through the stamped wall, arrived at x = %.2f, %u records\n",
                static_cast<double>(sys.view(10).pos.x), sys.stats().terrain_records);
}

// ---------------------------------------------------------------- a record waiting for quota
// With no dynamic quota no record is decided: every further failed step makes another record (no duplicate rule). When
// the unit's own cell is stamped the waiting service stops it and every record ends.
void test_terrain_waiting() {
    ::alamo::test::section("terrain_waiting");
    MovementConfig cfg;
    cfg.constants.dynamic_land_complexity_quota = 0;
    MovementSystem sys{cfg};
    land_system(sys, 100);
    CHECK(sys.add_unit(vehicle(10, cell_center(10, 20), 1)));
    SimRng rng(405);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(40, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 40; ++f) sys.tick(f, rng, serial_for);
    wall(sys, 20, 20, 17, 23);
    for (; f < 900 && terrain_records(sys, 10) < 3; ++f) sys.tick(f, rng, serial_for);
    const uint32_t waiting = terrain_records(sys, 10);
    CHECK(waiting == 3 && sys.view(10).has_goal && !sys.view(10).pushed && sys.view(10).speed == 0.0f);
    const V2 p = sys.view(10).pos;
    sys.grid().set_base(world_to_cell(p.x), world_to_cell(p.y), kPassImpassable);
    sys.tick(f++, rng, serial_for);
    CHECK(!sys.view(10).has_goal && sys.view(10).speed == 0.0f && terrain_records(sys, 10) == 0);
    CHECK(sys.stats().terrain_stops_cell >= 1 && sys.stats().terrain_detours == 0);
    std::printf("terrain waiting for quota: %u records waited, the stamped own cell stopped the unit (stops %u)\n", waiting,
                sys.stats().terrain_stops_cell);
}

}  // namespace

int main() {
    test_terrain_detour();
    test_terrain_stop_last_leg();
    test_terrain_pushed_orderless();
    test_terrain_forced_path();
    test_terrain_waiting();
    return ::alamo::test::report("alamo_tactical_movement_b4_tests");
}
