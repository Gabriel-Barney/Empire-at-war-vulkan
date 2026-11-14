// alamo_tactical_movement_b4_tests (no framework; non-zero exit on failure).
// s78n t24 (t20 batch 4): TerrainCollision (MOVEMENT §5.4.7) - a walk step that would end on ground the unit may not enter
// is not taken (speed 0) and makes a one-participant record; its decision runs Path Around with no obstacle (kind 3, D =
// r_u + 40) and the unit turns in place onto the detour's first leg before it moves, or the unit is stopped and loses its
// paths. No blocked-frame count, no re-plan of the ordered path.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
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
// A vehicle (10 x 6, speed 1, turn rate 6) ordered along y = 400 from x = 200 to x = 800; once it moves (its order path
// planned straight after the start delay), a wall of cells
// x = 20, y = 17 .. 23 is stamped across its path. The step into the wall fails: speed 0, the position unchanged, one
// Terrain record with one participant (made in the commit, decided on the next tick). The decision pushes a kind-2 detour
// owned by the record whose rejoin point lies at least D + 2 = r_u + 42 ahead along the ordered path. s78n t26.2
// (MOVEMENT §5.4.7 item 6, §3.5.2 A / B.4 / B.7, §3.5.1 item 1): the detour's waypoint 0 has speed fraction 0, so on
// that tick the corner cap (turn-done clear, the facing off the leg) sends the unit to End Move, where T = a: one creep
// step of min(acc, a) = 1 x 1.56 (accel / decel unset: the max speed, §3.4) [port: toward the waypoint until t26.4, retail along
// the facing]; on the next tick End Move at v <= a stops (0x15) and restarts: the start rule fails (fraction 0) and the
// unit turns in place at speed 0 (state 0x10 at R = 6 x 1.56, no stopped rate) until a frame leaves rate over, landing on
// the first leg exactly, then walks it in the same tick; the record ends at its first service after the detour is
// popped; the order arrives with no re-plan. Serial and 4 workers give the same digest every tick.
struct DetourOut {
    bool failed_step_ok = false, record_ok = false, detour_ok = false, creep_ok = false, turned_ok = false, ended_ok = false,
         arrived = false;
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
    for (; f < 300 && !(sys.view(10).speed > 0.0f); ++f) sys.tick(f, rng, pf);
    CHECK(sys.view(10).has_goal && sys.view(10).speed > 0.0f && sys.view(10).pos.x < 360.0f && sys.view(10).yaw == 0.0f);
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
            // End Move from the corner cap at fraction 0: one creep step at T = a. s78n t26.4 (MOVEMENT 3.5.2 C.6 / C.7 / C.10,
            // 5.4.7 item 1): it turns by R first and steps along the new facing, still into the wall's clearance - that step
            // fails too (no commit, speed 0, a second record: no duplicate rule); the turn that follows aims at the detour's
            // waypoint 1 from where it stands
            if (top.size() >= 2) heading = heading_deg(top[1] - v.pos);
            r.creep_ok = v.speed == 0.0f && v.pos == b.pos && v.pushed && sys.stats().terrain_records == 2;
            turning = true;
            continue;
        }
        if (turning && !moved_after) {
            if (v.pos == b.pos) {
                // turning in place: speed 0, the yaw steps by the turn rate toward the first leg
                if (v.speed == 0.0f && std::fabs(wrap_deg(heading - v.yaw)) < std::fabs(wrap_deg(heading - b.yaw))) ++r.turn_frames;
            } else {
                moved_after = true;
                r.yaw_at_move = v.yaw;
                // it starts to move only when the start-of-tick facing was within one tick's turn (R = 6 x 1.56) of the
                // leg (the frame that leaves rate over), and the turn lands the facing on the leg exactly
                r.turned_ok = std::fabs(wrap_deg(heading - b.yaw)) < 6.0f * 1.56f + 1e-4f && std::fabs(wrap_deg(heading - v.yaw)) < 1e-3f;
            }
        }
        // the detour is popped in the commit pass; the record sees that at its next service (the next tick's driver pass)
        if (moved_after && r.pop_frame == 0 && !v.pushed) {
            r.pop_frame = f;
            r.ended_ok = terrain_records(sys, 10) == 1;
        } else if (r.pop_frame != 0 && f == r.pop_frame + 1) {
            r.ended_ok = r.ended_ok && terrain_records(sys, 10) == 0;
        }
        if (r.pop_frame != 0 && f > r.pop_frame && !v.has_goal) break;
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
    CHECK(s.creep_ok && s.turn_frames >= 3 && s.turned_ok);
    CHECK(s.pop_frame > 0 && s.ended_ok && s.arrived);
    // s78n t26.4: the creep's record and the later lane-term steps into the wall's corner on the order path (each failed
    // step makes a record, MOVEMENT 5.4.7 item 1); every one detours
    CHECK(s.records >= 2 && s.detours == s.records && s.replans == 0);
    CHECK(s.crc == p.crc);
    std::printf("terrain detour: failed step at %u, rejoin %.2f ahead (>= r_u + 42 = %.2f), first leg %.2f deg, creep %d, %u turning "
                "ticks, yaw at the first move %.4f, popped at %u, arrived %d, records %u, detours %u, re-plans %u, serial = 4 "
                "workers %d (%zu ticks)\n",
                s.fail_frame, static_cast<double>(s.rejoin_ahead), static_cast<double>(ru + 42.0f), static_cast<double>(s.first_leg),
                s.creep_ok ? 1 : 0, s.turn_frames, static_cast<double>(s.yaw_at_move), s.pop_frame, s.arrived ? 1 : 0, s.records, s.detours, s.replans,
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
// Vehicle 10 on a forced path (flag 0x20) pushes the standing vehicle 11 aside (MovingVsStopped step 2). Walled in by
// the ring of cells around its own, 11's push walk fails: its record finds no kind-1 record (it has no order), so the unit is stopped and its
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
    for (int32_t y = cy - 1; y <= cy + 1; ++y)
        for (int32_t x = cx - 1; x <= cx + 1; ++x)
            if (x != cx || y != cy) sys.grid().set_base(x, y, kPassImpassable);
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
    for (; f < 300 && !(sys.view(10).speed > 0.0f); ++f) sys.tick(f, rng, serial_for);
    CHECK(sys.view(10).yaw == 0.0f && sys.view(10).pos.x < 360.0f);
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

// ================================================================ bkm2 t07 (b0043): the missing b4 rule scenes

// ---------------------------------------------------------------- (d) a corner-clipping Walk step commits
// s78n t26.4 (MOVEMENT §3.5.2 C.10, §3.5 step 4): the walk tests only the point it would end on (0x50e280), never the
// segment to it. Pure step: cell (6, 6) (world [110, 130) x [110, 130)) is impassable; a unit at (108, 113) - cell (5, 6) -
// faces -45 degrees at speed 5 x sqrt(2) on a path along (1, -1): its step ends at (113, 108) - cell (6, 5) - and the
// segment crosses the blocked cell's corner (through (110.5, 110.5)). The step is taken: not blocked, moved, no speed lost.
void test_walk_corner_clip_step() {
    ::alamo::test::section("walk_corner_clip_step");
    PassabilityGrid g = open_grid(20, 20);
    g.set_base(6, 6, kPassImpassable);
    LandMotion m;
    m.pos = {108.0f, 113.0f};
    m.yaw = -45.0f;
    const float v = 7.0710678f;
    m.speed = v;
    m.state = LocoState::WalkMoving;
    m.path = {{108.0f, 113.0f}, {208.0f, 13.0f}};
    m.next = 1;
    LocomotorParams p;
    p.max_speed = v;
    p.accel = p.decel = 1.0f;
    p.turn_rate = 6.0f;
    p.mask = kPassClear;
    const V2 from = m.pos;
    const WalkResult r = walk_step(m, p, 3.0e38f, &g, nullptr);
    const bool clips = !line_passable(g, from, m.pos, kPassClear);  // the segment meets the blocked cell
    std::printf("walk corner clip (pure step): (%.6f, %.6f) -> (%.6f, %.6f), cells (%d, %d) -> (%d, %d), segment crosses cell (6, 6) %d, "
                "blocked %d, moved %d, speed %.6f\n",
                static_cast<double>(from.x), static_cast<double>(from.y), static_cast<double>(m.pos.x), static_cast<double>(m.pos.y),
                world_to_cell(from.x), world_to_cell(from.y), world_to_cell(m.pos.x), world_to_cell(m.pos.y), clips ? 1 : 0,
                r.blocked ? 1 : 0, r.moved ? 1 : 0, static_cast<double>(m.speed));
    CHECK(clips && world_to_cell(m.pos.x) == 6 && world_to_cell(m.pos.y) == 5);
    CHECK(!r.blocked && r.moved && m.speed == v && m.state == LocoState::WalkMoving);
    CHECK(std::fabs(m.pos.x - 113.0f) < 1e-3f && std::fabs(m.pos.y - 108.0f) < 1e-3f);
}

// The same rule in the system: a vehicle (speed 7, accel = decel = max speed) ordered from (200, 603) to (600, 203) along
// x + y = 803 walks without a wall first; every step whose segment meets a cell that holds neither of its end points nor
// any position of the run is a corner clip (the line cuts each cell with i + j = 41 in a corner triangle of legs 3, a
// 4.24 chord, shorter than one step). The run is repeated with the first such cell stamped impassable once the unit
// moves: the walk is the same, step for step (positions, speeds), no TerrainCollision record is made and no step fails.
struct ClipRun {
    std::vector<V2> pos;
    std::vector<float> speed;
    uint32_t move_frame = 0, records = 0, blocked = 0;
    bool arrived = false;
};

ClipRun run_clip(int32_t cx, int32_t cy) {
    ClipRun r;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = vehicle(10, {200.0f, 603.0f}, 1, 7.0f);
    d.yaw_deg = -45.0f;
    CHECK(sys.add_unit(d));
    SimRng rng(406);
    MoveOrder o;
    o.units = {10};
    o.destination = {600.0f, 203.0f};
    sys.move(o, rng, 1);
    for (uint32_t f = 1; f < 400; ++f) {
        sys.tick(f, rng, serial_for);
        const UnitView v = sys.view(10);
        r.pos.push_back(v.pos);
        r.speed.push_back(v.speed);
        if (r.move_frame == 0 && v.speed > 0.0f) {
            r.move_frame = f;
            if (cx >= 0) sys.grid().set_base(cx, cy, kPassImpassable);
        }
        if (r.move_frame != 0 && !v.has_goal) {
            r.arrived = v.arrived;
            break;
        }
    }
    r.records = sys.stats().terrain_records;
    r.blocked = sys.stats().blocked_steps;
    return r;
}

void test_walk_corner_clip_system() {
    ::alamo::test::section("walk_corner_clip_system");
    const ClipRun a = run_clip(-1, -1);
    // the candidate cells: crossed by a step's segment (sampled finely), holding none of the run's positions
    std::vector<std::pair<int32_t, int32_t>> held;
    for (const V2& p : a.pos) held.emplace_back(world_to_cell(p.x), world_to_cell(p.y));
    int32_t cx = -1, cy = -1;
    uint32_t step = 0, candidates = 0;
    for (size_t k = 1; k < a.pos.size(); ++k) {
        if (k <= a.move_frame) continue;  // the stamp lands after the move frame's tick
        for (int32_t s = 1; s < 256; ++s) {
            const float t = static_cast<float>(s) / 256.0f;
            const V2 q = a.pos[k - 1] + (a.pos[k] - a.pos[k - 1]) * t;
            const std::pair<int32_t, int32_t> c{world_to_cell(q.x), world_to_cell(q.y)};
            if (std::find(held.begin(), held.end(), c) != held.end()) continue;
            ++candidates;
            if (cx < 0) {
                cx = c.first;
                cy = c.second;
                step = static_cast<uint32_t>(k);
            }
            break;
        }
    }
    CHECK(a.arrived && a.records == 0 && cx >= 0);
    if (cx < 0) return;
    const ClipRun b = run_clip(cx, cy);
    PassabilityGrid g = open_grid(100, 100);
    g.set_base(cx, cy, kPassImpassable);
    const bool clips = !line_passable(g, a.pos[step - 1], a.pos[step], kPassClear);
    std::printf("walk corner clip (system): %u clipping step(s); cell (%d, %d) stamped at frame %u, clipped by the step of frame %u "
                "((%.4f, %.4f) -> (%.4f, %.4f)); same walk %d (%zu frames), records %u, failed steps %u, arrived %d\n",
                candidates, cx, cy, b.move_frame, step + 1, static_cast<double>(a.pos[step - 1].x), static_cast<double>(a.pos[step - 1].y),
                static_cast<double>(a.pos[step].x), static_cast<double>(a.pos[step].y), (a.pos == b.pos && a.speed == b.speed) ? 1 : 0,
                b.pos.size(), b.records, b.blocked, b.arrived ? 1 : 0);
    CHECK(clips && b.move_frame == a.move_frame);
    CHECK(a.pos == b.pos && a.speed == b.speed);
    CHECK(b.records == 0 && b.blocked == 0 && b.arrived);
}

// ---------------------------------------------------------------- (c) a squad container on TerrainCollision
// A land team container (FIGHTER_FLYING §7.7) 100 with four members (shape 3 x 3, slots +-8), every speed 1 or 3, member
// accel = decel = acc (the team rate 0x5ab540 = acc), turn rate 6, Land speed multiplier 1 (R = 6).
void add_squad(MovementSystem& sys, V2 at, float speed, float acc) {
    UnitDesc cd = vehicle(100, at, 1, speed);
    cd.type_name = "BKM2_TEAM";
    cd.team_container = true;
    CHECK(sys.add_unit(cd));
    const V2 rel[4] = {{8.0f, 8.0f}, {8.0f, -8.0f}, {-8.0f, 8.0f}, {-8.0f, -8.0f}};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (ObjectId id = 1; id <= 4; ++id) {
        UnitDesc d = vehicle(id, at + rel[id - 1], 1, speed);
        d.type_name = "BKM2_TROOPER";
        d.accel = d.decel = acc;
        d.radius = 3.0f;
        d.hard_x = d.hard_y = 3.0f;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(id);
        slots.push_back(rel[id - 1]);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    CHECK(sys.set_team(100, 100, 1.0f, speed));
    for (const ObjectId id : ids) CHECK(sys.set_team(id, 100, 1.0f, speed));
}

MovementConfig unit_multiplier() {
    MovementConfig cfg;
    cfg.constants.land_speed_multiplier = 1.0f;
    return cfg;
}

// (c1) MOVEMENT §5.4.7 item 6 / FIGHTER_FLYING §7.7 item 7: the squad marches +x (speed 1); a wall (cells x = 20, y = 15
// .. 25) is stamped across its path. The container's own step into it fails: speed 0, no commit, one Terrain record whose
// participant is the container. Its decision (the next tick) pushes a kind-2 detour (waypoint 0 fraction 0) and puts the
// container in 0x16, so its walk of the same tick is the start turn 0x10 in place: speed 0, the position kept, the facing
// stepped by exactly R = 6 toward the detour's first leg (more than 6 off). It then turns until the leg is within R, walks
// the detour and arrives.
void test_terrain_container_start_turn() {
    ::alamo::test::section("terrain_container_start_turn");
    MovementSystem sys{unit_multiplier()};
    land_system(sys, 100);
    add_squad(sys, cell_center(10, 20), 1.0f, 0.05f);
    SimRng rng(407);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(40, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 300 && !(sys.view(100).speed > 0.0f); ++f) sys.tick(f, rng, serial_for);
    CHECK(sys.view(100).has_goal && sys.view(100).speed > 0.0f && sys.view(100).pos.x < 360.0f);
    wall(sys, 20, 20, 15, 25);
    uint32_t fail = 0, member_records = 0;
    bool fail_ok = false;
    for (; f < 900 && fail == 0; ++f) {
        const UnitView b = sys.view(100);
        sys.tick(f, rng, serial_for);
        if (terrain_records(sys, 100) == 1) {
            fail = f;
            const UnitView v = sys.view(100);
            const CollisionObject* c = terrain_record(sys, 100);
            fail_ok = v.speed == 0.0f && v.pos == b.pos && !v.pushed && c && c->participants == 1 && c->state == CollisionState::Active;
        }
    }
    for (ObjectId id = 1; id <= 4; ++id) member_records += terrain_records(sys, id);
    const UnitView b = sys.view(100);
    sys.tick(f++, rng, serial_for);  // the decision, then the container's walk
    const UnitView v = sys.view(100);
    const std::span<const V2> top = sys.top_path(100);
    const CollisionObject* c = terrain_record(sys, 100);
    const float leg = top.size() >= 2 ? heading_deg(top[1] - top[0]) : 0.0f;
    const float turned = std::fabs(wrap_deg(v.yaw - b.yaw));
    std::printf("terrain container start turn: failed step at %u (x = %.3f), member records %u; decision: pushed %d kind %u, record "
                "state %u, walk state 0x%02x, speed %.4f, moved %d, first leg %.3f deg, turned %.6f deg (R 6)\n",
                fail, static_cast<double>(b.pos.x), member_records, v.pushed ? 1 : 0, v.push_kind,
                c ? static_cast<unsigned>(c->state) : 99u, static_cast<unsigned>(v.walk_state), static_cast<double>(v.speed),
                v.pos == b.pos ? 0 : 1, static_cast<double>(leg), static_cast<double>(turned));
    CHECK(fail > 0 && fail_ok && member_records == 0 && sys.stats().terrain_detours == 1);
    CHECK(v.pushed && v.push_kind == 2 && c && c->state == CollisionState::Resolving);
    CHECK(std::fabs(wrap_deg(leg - b.yaw)) > 6.0f);
    CHECK(v.walk_state == LocoState::WalkStartTurn && v.speed == 0.0f && v.pos == b.pos && std::fabs(turned - 6.0f) < 1e-4f);
    // the turn goes on in place until the leg is within R; then the container walks the detour and the order arrives
    uint32_t turn_frames = 1;
    for (; f < 3000 && sys.view(100).pos == b.pos; ++f) {
        sys.tick(f, rng, serial_for);
        if (sys.view(100).pos == b.pos) ++turn_frames;
    }
    for (; f < 4000 && sys.view(100).has_goal; ++f) sys.tick(f, rng, serial_for);
    std::printf("terrain container start turn: %u frame(s) turning in place, arrived %d at (%.2f, %.2f), records %u, detours %u\n",
                turn_frames, sys.view(100).arrived ? 1 : 0, static_cast<double>(sys.view(100).pos.x),
                static_cast<double>(sys.view(100).pos.y), sys.stats().terrain_records, sys.stats().terrain_detours);
    CHECK(turn_frames >= 2 && !sys.view(100).has_goal && sys.view(100).arrived);
}

// (c2) MOVEMENT §3.5.2 C.4 / E, §5.4.7 item 1 (s78n t24, s78o b0054): a squad marching +x at speed 3 is halted (the path
// stack cleared, the speed kept): the container coasts in End Move along its facing, braking by a = 0.05 a frame. A wall
// stamped two cells ahead is met by a coast step: the point test refuses it (not forced: no order path), so the step is
// not committed, the speed is zeroed and a Terrain record names the container - in the tick whose walk was a coast step.
// The record's decision finds no order path to go around (no base): the container is stopped there (terrain_stops_nobase)
// and the record ends.
void test_terrain_container_coast() {
    ::alamo::test::section("terrain_container_coast");
    MovementSystem sys{unit_multiplier()};
    land_system(sys, 100);
    add_squad(sys, cell_center(10, 20), 3.0f, 0.05f);
    SimRng rng(408);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(90, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 900 && !(sys.view(100).speed >= 3.0f && sys.view(100).pos.x > 400.0f); ++f) sys.tick(f, rng, serial_for);
    CHECK(sys.view(100).speed == 3.0f && sys.view(100).has_goal);
    const ObjectId ids[] = {100};
    sys.halt(ids, f);
    const int32_t wx = world_to_cell(sys.view(100).pos.x) + 2;
    wall(sys, wx, wx, 15, 25);
    uint32_t fail = 0, coast_frames = 0;
    bool fail_ok = false;
    for (; f < 1200 && fail == 0; ++f) {
        const UnitView b = sys.view(100);
        const uint32_t coasts = sys.stats().walk_coasts;
        sys.tick(f, rng, serial_for);
        const bool coast = sys.stats().walk_coasts > coasts;
        if (coast) ++coast_frames;
        if (terrain_records(sys, 100) == 1) {
            fail = f;
            const UnitView v = sys.view(100);
            fail_ok = coast && v.speed == 0.0f && v.pos == b.pos && !v.has_goal && b.speed > 0.0f;
        }
    }
    const V2 at = sys.view(100).pos;
    const uint32_t nobase0 = sys.stats().terrain_stops_nobase;
    sys.tick(f++, rng, serial_for);  // the decision
    std::printf("terrain container coast: wall at cell x %d; %u coast frame(s), the coast step refused at frame %u at x = %.3f "
                "(cell edge %.1f); decision: stops (no base) +%u, record left %u, speed %.4f\n",
                wx, coast_frames, fail, static_cast<double>(at.x), static_cast<double>(wx) * kCellSize - kHalfCell,
                sys.stats().terrain_stops_nobase - nobase0, terrain_records(sys, 100), static_cast<double>(sys.view(100).speed));
    CHECK(fail > 0 && fail_ok && coast_frames >= 2);
    CHECK(sys.stats().terrain_stops_nobase == nobase0 + 1 && terrain_records(sys, 100) == 0);
    CHECK(sys.view(100).speed == 0.0f && sys.view(100).pos == at && at.x < static_cast<float>(wx) * kCellSize - kHalfCell);
}

// ---------------------------------------------------------------- (b) the goal-spot filter's relation rules
// MOVEMENT §5.4.7 "Rejoin candidates of a kind-3 Path Around" item 2 (0x67d580, s78n t24 goal_obstacle): a candidate spot
// is refused when the unit's rectangle there overlaps an object that counts as an obstacle - a standing object does; an
// object moving the same way, not slower, does not (no shared record, not on its final approach). The detour scene of
// run_detour (vehicle 10, speed 1, here a shorter wall x = 20, y = 19 .. 21): run once alone, its detour rejoins at G0
// (the first candidate, along the path). Then vehicle 11 (the same type and owner) stands over G0 at the decision tick:
// standing (added just before it) - G0 is refused, the rejoin moves on along the path by the 10-step of the generator
// (rule (a)) to the first spot clear of it; moving at the same speed on a heading 55 degrees off (the same way, not
// slower; no record with the unit) - G0 is kept; moving on that line at half the speed (same way, slower) - refused as
// the standing one (the spec's relation 0x20). Neither placement makes a record with the unit before the decision.
struct GoalRun {
    uint32_t fail = 0, other_start = 0, pair_records = 0;
    V2 rejoin, other_at;
    bool decided = false, other_moving = false;
    float other_speed0 = 0.0f, other_speed1 = 0.0f;  // the other's speed before / after the decision tick
};

// mode 0 alone, 1 standing, 2 moving toward `to` at max speed `speed` (x 1.56)
GoalRun run_goal(int mode, V2 g0, uint32_t at_frame, V2 to = {}, float speed = 1.0f) {
    GoalRun r;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(vehicle(10, cell_center(10, 20), 1)));
    SimRng rng(401);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(40, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 300 && !(sys.view(10).speed > 0.0f); ++f) sys.tick(f, rng, serial_for);
    wall(sys, 20, 20, 19, 21);
    for (; f < 2000; ++f) {
        if (mode != 0 && f == at_frame) {
            UnitDesc d = vehicle(11, g0, 1, speed);
            if (mode == 2) d.yaw_deg = heading_deg(to - g0);
            CHECK(sys.add_unit(d));
            if (mode == 2) {
                MoveOrder m;
                m.units = {11};
                m.destination = to;
                sys.move(m, rng, f);
            }
        }
        const uint32_t rec0 = sys.stats().terrain_records;
        if (mode != 0 && r.fail != 0) r.other_speed0 = sys.view(11).speed;
        sys.tick(f, rng, serial_for);
        if (mode != 0 && f >= at_frame) {
            if (r.other_start == 0 && sys.view(11).speed > 0.0f) r.other_start = f;
            for (const CollisionObject& c : sys.collisions().list())
                if (c.kind != CollisionKind::Terrain && (c.a == 10 || c.b == 10)) ++r.pair_records;
        }
        if (r.fail == 0 && sys.stats().terrain_records == rec0 + 1) {
            r.fail = f;
            continue;
        }
        if (r.fail != 0) {
            const std::span<const V2> top = sys.top_path(10);
            r.decided = sys.view(10).pushed && sys.view(10).push_kind == 2 && top.size() >= 2;
            if (top.size() >= 2) r.rejoin = top.back();
            if (mode != 0) {
                r.other_speed1 = sys.view(11).speed;
                r.other_moving = sys.view(11).has_goal && r.other_speed0 > 0.0f && r.other_speed1 > 0.0f;
                r.other_at = sys.view(11).pos;
            }
            break;
        }
    }
    return r;
}

void test_terrain_goal_relation() {
    ::alamo::test::section("terrain_goal_relation");
    const GoalRun a = run_goal(0, {}, 0);
    CHECK(a.fail > 0 && a.decided);
    const GoalRun s = run_goal(1, a.rejoin, a.fail + 1);
    // the mover (max speed 1 x 1.56, moving from the tick it is added) comes up from below-left at 55 degrees (the same way:
    // within 60 degrees of the unit's +x, and not slower), 60 ticks of travel short of G0, so it stands over G0 at the
    // decision; its line passes below the wall (y < 370 at x = 410)
    const V2 dir55 = dir_from_deg(55.0f);
    const uint32_t lead = 60;
    const GoalRun m = run_goal(2, a.rejoin - dir55 * (1.56f * static_cast<float>(lead)), a.fail + 1 - lead + 1, a.rejoin + dir55 * 400.0f);
    std::printf("  other: standing at (%.3f, %.3f), pair records %u; mover started %u at (%.3f, %.3f), pair records %u\n",
                static_cast<double>(s.other_at.x), static_cast<double>(s.other_at.y), s.pair_records, m.other_start,
                static_cast<double>(m.other_at.x), static_cast<double>(m.other_at.y), m.pair_records);
    std::printf("terrain goal relation: alone fail %u rejoin (%.3f, %.3f); standing obstacle fail %u rejoin (%.3f, %.3f); same-way "
                "mover (moving %d) fail %u rejoin (%.3f, %.3f)\n",
                a.fail, static_cast<double>(a.rejoin.x), static_cast<double>(a.rejoin.y), s.fail, static_cast<double>(s.rejoin.x),
                static_cast<double>(s.rejoin.y), m.other_moving ? 1 : 0, m.fail, static_cast<double>(m.rejoin.x),
                static_cast<double>(m.rejoin.y));
    // a slower mover (max speed 0.5 x 1.56 = 0.78 < the unit's 1.56) on the same 55-degree line, 60 ticks short of G0:
    // relation 0x20 (same way, slower) - an obstacle (MOVEMENT §5.4.7 "Rejoin candidates" item 3), refused like the standing one
    const GoalRun w = run_goal(2, a.rejoin - dir55 * (0.78f * static_cast<float>(lead)), a.fail + 1 - lead + 1, a.rejoin + dir55 * 400.0f, 0.5f);
    std::printf("  movers at the decision: same way speed %.4f -> %.4f at (%.3f, %.3f); slower speed %.4f -> %.4f at (%.3f, %.3f), pair "
                "records %u, fail %u, rejoin (%.3f, %.3f)\n",
                static_cast<double>(m.other_speed0), static_cast<double>(m.other_speed1), static_cast<double>(m.other_at.x),
                static_cast<double>(m.other_at.y), static_cast<double>(w.other_speed0), static_cast<double>(w.other_speed1),
                static_cast<double>(w.other_at.x), static_cast<double>(w.other_at.y), w.pair_records, w.fail,
                static_cast<double>(w.rejoin.x), static_cast<double>(w.rejoin.y));
    CHECK(s.fail == a.fail && s.decided && m.fail == a.fail && m.decided && m.other_moving);
    CHECK(s.other_at == a.rejoin && distance(m.other_at, a.rejoin) < 0.01f && s.pair_records == 0 && m.pair_records == 0);
    CHECK(m.other_speed0 == 1.56f && m.other_speed1 == 1.56f);  // moving at full speed through the decision
    CHECK(w.fail == a.fail && w.decided && w.other_moving && w.other_speed0 == 0.78f && w.other_speed1 == 0.78f);
    CHECK(distance(w.other_at, a.rejoin) < 0.01f && w.pair_records == 0);
    // standing: refused, the rejoin moves on along the path (+x) by whole generator steps of 10 - measured: 3 steps (the
    // unit's rectangle, half-length 10, clears the obstacle's at G0 + 30)
    const float shift = s.rejoin.x - a.rejoin.x;
    CHECK(shift >= 10.0f - 1e-3f && std::fabs(shift / 10.0f - std::round(shift / 10.0f)) < 1e-3f && std::fabs(s.rejoin.y - a.rejoin.y) < 1e-3f);
    CHECK(std::fabs(a.rejoin.x - 442.422f) < 1e-3f && a.rejoin.y == 400.0f && std::fabs(shift - 30.0f) < 1e-3f);
    // same way, not slower: not an obstacle - the first candidate is kept
    CHECK(m.rejoin == a.rejoin);
    // same way, slower: an obstacle - refused, the rejoin moves on along the path as for the standing one
    CHECK(std::fabs(w.rejoin.x - s.rejoin.x) < 1e-3f && std::fabs(w.rejoin.y - s.rejoin.y) < 1e-3f);
}

}  // namespace

int main() {
    test_terrain_detour();
    test_terrain_stop_last_leg();
    test_terrain_pushed_orderless();
    test_terrain_forced_path();
    test_terrain_waiting();
    test_walk_corner_clip_step();    // bkm2 t07 (b0043)
    test_walk_corner_clip_system();
    test_terrain_container_start_turn();
    test_terrain_container_coast();
    test_terrain_goal_relation();
    return ::alamo::test::report("alamo_tactical_movement_b4_tests");
}
