// alamo_tactical_movement_bkm_tests (no framework; non-zero exit on failure).
// bkm1 (2026-11-12, backlog b0095 b0096 b0035 b0036 b0037 b0051 b0055 b0050): retail rules of the land formation and
// path code (docs/re/MOVEMENT.md), one scene per rule.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/land_path.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/test/support.hpp"
#include "alamo/test/thread_parallel_for.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;

namespace {

void land_system(MovementSystem& sys, uint32_t cells) {
    PassabilityGrid g;
    g.init(cells, cells);
    const float hi = static_cast<float>(g.width() - 1) * kCellSize;
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float, float) { return 0.0f; });
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
}

UnitDesc vehicle(ObjectId id, V2 pos, float speed = 1.0f) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = speed;
    d.turn_rate = 6.0f;
    d.alt_max_speed = speed;
    d.alt_turn_rate = 6.0f;
    d.movement_class = static_cast<int32_t>(MovementClass::Wheeled);  // Clear | Shield
    d.collision_class = kUccVehicle;
    d.radius = 12.0f;
    d.hard_x = 10.0f;
    d.hard_y = 6.0f;
    d.owner = 1;
    d.type_name = "BKM_VEHICLE";
    return d;
}

// Cells x0..x1, y0..y1 become base-shield cells (Clear | Shield): Wheeled may enter them, WheeledNoShield may not.
void shield(MovementSystem& sys, int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x) sys.grid().set_base(x, y, kPassClear | kPassShield);
}

// Any shield cell on the unit's planned path polyline (sampled every 2 units)?
bool path_crosses_shield(const MovementSystem& sys, ObjectId id) {
    const std::span<const V2> p = sys.land_path(id);
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        const float len = distance(p[i], p[i + 1]);
        const int n = std::max(1, static_cast<int>(len / 2.0f));
        for (int k = 0; k <= n; ++k) {
            const V2 q = p[i] + (p[i + 1] - p[i]) * (static_cast<float>(k) / static_cast<float>(n));
            const uint32_t c = sys.grid().effective(world_to_cell(q.x), world_to_cell(q.y));
            if (c & kPassShield) return true;
        }
    }
    return false;
}

// Plan the order and return once every unit has its path (need_path served).
void plan(MovementSystem& sys, const std::vector<ObjectId>& ids, V2 dest, SimRng& rng) {
    MoveOrder o;
    o.units = ids;
    o.destination = dest;
    sys.move(o, rng, 1);
    for (uint32_t f = 1; f < 200; ++f) {
        sys.tick(f, rng);
        bool all = true;
        for (const ObjectId id : ids) all = all && sys.land_path(id).size() >= 2;
        if (all) return;
    }
}

// ---------------------------------------------------------------- b0035: the class without the Shield bit
// MOVEMENT §4.3.1 item 7 (c) [H]: 0x414720 plans for the best class without the Shield bit 0x40 (0x477b00) when the goal
// is zone-connected to the lead for it. A Wheeled vehicle at cell (10, 20) ordered to (40, 20) across a shield band x = 24
// .. 27, y = 10 .. 30: (a) the band leaves a way round, so the path keeps out of it (WheeledNoShield); (b) the band spans
// the whole map, so the goal is cut off for WheeledNoShield and the path crosses it (Wheeled); (c) the vehicle starts on a
// shield cell: its own cell is zone 0 for WheeledNoShield, so it plans for Wheeled and drives straight through. (d) a
// formation of two (the formation path 0x417520 with the formation's class): both members keep out of the band.
void test_shield_class() {
    ::alamo::test::section("shield_class");
    {  // (a)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 60);
        shield(sys, 24, 27, 10, 30);
        CHECK(sys.add_unit(vehicle(1, cell_center(10, 20))));
        SimRng rng(7);
        plan(sys, {1}, cell_center(40, 20), rng);
        CHECK(sys.land_path(1).size() >= 3);
        CHECK(!path_crosses_shield(sys, 1));
    }
    {  // (b)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 60);
        shield(sys, 24, 27, 0, 59);
        CHECK(sys.add_unit(vehicle(1, cell_center(10, 20))));
        SimRng rng(7);
        plan(sys, {1}, cell_center(40, 20), rng);
        CHECK(sys.land_path(1).size() >= 2);
        CHECK(path_crosses_shield(sys, 1));
    }
    {  // (c)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 60);
        shield(sys, 8, 27, 10, 30);
        CHECK(sys.add_unit(vehicle(1, cell_center(10, 20))));
        SimRng rng(7);
        plan(sys, {1}, cell_center(40, 20), rng);
        CHECK(path_crosses_shield(sys, 1));
    }
    {  // (d)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 60);
        shield(sys, 24, 27, 10, 30);
        CHECK(sys.add_unit(vehicle(1, cell_center(10, 19))));
        CHECK(sys.add_unit(vehicle(2, cell_center(10, 21))));
        SimRng rng(7);
        plan(sys, {1, 2}, cell_center(40, 20), rng);
        CHECK(sys.stats().formation_paths == 1);
        CHECK(!path_crosses_shield(sys, 1) && !path_crosses_shield(sys, 2));
    }
}

// ---------------------------------------------------------------- b0055: an unplanned order survives the old path's stop
// MOVEMENT §3.5.2 G, §4.6.2: between a land order and its plan the old order path stays on the stack (the stack reset
// 0x554480 comes with the path set), so a moving unit walks on along it. When that walk meets a terrain stamp, the
// TerrainCollision record (§5.4.7) runs Path Around on the old path (it is the kind-1 record) or, failing, Stops the
// stack's records; the new order is held by its formation's block and is planned when its start comes. A vehicle with
// PathFrameDelay 400 walks east along y = 20; a second order to cell (30, 40) waits for its start (start_frame); a wall
// is stamped 3 cells ahead on the old path before that start. The unit must keep the second order and arrive at it.
void test_unplanned_order_kept() {
    ::alamo::test::section("unplanned_order_kept");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = vehicle(10, cell_center(10, 20));
    d.path_frame_delay = 400;
    CHECK(sys.add_unit(d));
    SimRng rng(401);
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(60, 20);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 1000 && !(sys.view(10).speed > 0.0f && sys.view(10).pos.x > cell_center(14, 20).x); ++f) sys.tick(f, rng);
    CHECK(sys.view(10).speed > 0.0f);
    MoveOrder o2;
    o2.units = {10};
    o2.destination = cell_center(30, 40);
    sys.move(o2, rng, f);
    sys.tick(f++, rng);  // the composition: the new order's start
    const uint32_t start = sys.view(10).start_frame;
    const int32_t cx = world_to_cell(sys.view(10).pos.x) + 3;
    for (int32_t y = 17; y <= 23; ++y) sys.grid().set_base(cx, y, kPassImpassable);
    uint32_t hit = 0;
    for (; f < 4000; ++f) {
        const uint32_t rec0 = sys.stats().terrain_records;
        sys.tick(f, rng);
        if (hit == 0 && sys.stats().terrain_records > rec0) hit = f;
        if (sys.view(10).arrived) break;
    }
    const UnitView v = sys.view(10);
    std::printf("unplanned order: start %u, terrain record at %u, arrived %d at frame %u, pos (%.1f, %.1f)\n", start, hit,
                v.arrived ? 1 : 0, f, static_cast<double>(v.pos.x), static_cast<double>(v.pos.y));
    CHECK(hit != 0 && hit < start);  // the record came while the order waited
    CHECK(v.arrived && distance(v.pos, o2.destination) < 25.0f);
}

// ---------------------------------------------------------------- b0095: lane segments pass the member's class
// MOVEMENT §3.5.2 D (0x413340 tests each lane point) with port choice (1) of PHASE4 s78p: retail keeps the formation path
// and steers toward the lane, so a member never walks a lane segment that crosses ground its class cannot enter; the port
// walks the lane, so every segment of a member's lane path must pass the static line test 0x5a4c10 for its class. Two
// vehicles side by side (cells (10, y0) and (10, y0 + 2)) are ordered round the end of a wall (`wall_x`, y = 0 .. tip): the
// formation path hugs the wall's tip, and the inner lane, shifted toward the wall, would cut through it. Checks: every
// segment of both planned paths is passable, no TerrainCollision record over the walk, and both arrive.
struct LaneScene {
    int32_t y0, wall_x, tip;
    V2 goal;
};

void lane_scene(const LaneScene& sc, uint32_t& bad_segments, uint32_t& records, bool& arrived) {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 60);
    for (int32_t y = 0; y <= sc.tip; ++y) sys.grid().set_base(sc.wall_x, y, kPassImpassable);
    CHECK(sys.add_unit(vehicle(1, cell_center(10, sc.y0))));
    CHECK(sys.add_unit(vehicle(2, cell_center(10, sc.y0 + 2))));
    SimRng rng(9);
    plan(sys, {1, 2}, sc.goal, rng);
    bad_segments = 0;
    const uint32_t mask = sys.classes().mask(static_cast<int32_t>(MovementClass::Wheeled));
    for (const ObjectId id : {ObjectId{1}, ObjectId{2}}) {
        const std::span<const V2> p = sys.land_path(id);
        for (size_t i = 0; i + 1 < p.size(); ++i)
            if (!line_passable(sys.grid(), p[i], p[i + 1], mask)) ++bad_segments;
    }
    for (uint32_t f = 200; f < 3000; ++f) {
        sys.tick(f, rng);
        if (sys.view(1).arrived && sys.view(2).arrived) break;
    }
    records = sys.stats().terrain_records;
    arrived = sys.view(1).arrived && sys.view(2).arrived;
}

void test_lane_segments() {
    ::alamo::test::section("lane_segments");
    const LaneScene scenes[] = {
        {10, 25, 14, cell_center(40, 10)},  // round the tip at (25, 14): the lower (inner) lane cuts it
        {30, 25, 33, cell_center(40, 30)},
    };
    for (const LaneScene& sc : scenes) {
        uint32_t bad = 0, rec = 0;
        bool arr = false;
        lane_scene(sc, bad, rec, arr);
        std::printf("lane scene y0 %d tip %d: bad segments %u, terrain records %u, arrived %d\n", sc.y0, sc.tip, bad, rec,
                    arr ? 1 : 0);
        CHECK(bad == 0);
        CHECK(rec == 0);
        CHECK(arr);
    }
}

// ---------------------------------------------------------------- b0036: engagement compatibility in the recruitment
// MOVEMENT §4.2 step 2 item 7 test 9 [H]: C joins S only when K(C) <= E(S) and K(S) <= E(C), E = the engagement range (a
// move order: the attack range), K = min(E, Targeting_Min_Attack_Distance). Three vehicles in a row 30 apart; unit 1 is
// SPMAT-like (E 500, min 220), unit 2 E 200, unit 3 E 500 with no minimum. The seed is the one nearest the centroid, unit
// 2: unit 1 fails (K 220 > E(2) 200), unit 3 joins; unit 1 seeds its own formation. Without the min-distance query every
// K is 0 and all three share one formation.
void test_engagement_recruit() {
    ::alamo::test::section("engagement_recruit");
    for (const bool query : {true, false}) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 60);
        for (const ObjectId id : {ObjectId{1}, ObjectId{2}, ObjectId{3}})
            CHECK(sys.add_unit(vehicle(id, {200.0f + 30.0f * static_cast<float>(id - 1), 400.0f})));
        sys.set_attack_range_query([](ObjectId id) { return id == 2 ? 200.0f : 500.0f; });
        if (query) sys.set_min_attack_distance_query([](ObjectId id) { return id == 1 ? 220.0f : 0.0f; });
        SimRng rng(3);
        MoveOrder o;
        o.units = {1, 2, 3};
        o.destination = cell_center(40, 20);
        sys.move(o, rng, 1);
        const uint32_t f1 = sys.view(1).form_id, f2 = sys.view(2).form_id, f3 = sys.view(3).form_id;
        std::printf("engagement recruit (query %d): formations %u %u %u\n", query ? 1 : 0, f1, f2, f3);
        if (query) {
            CHECK(f2 == f3 && f1 != f2);
        } else {
            CHECK(f1 == f2 && f2 == f3);
        }
    }
}

// ---------------------------------------------------------------- b0096: where a formation member stops
// MOVEMENT §4.3.3 items 2 - 5 [H]: a member stops on the formation path's last waypoint line moved along the last segment
// d1 by its depth: G - d1 x depth + d1_perp x lateral, the front row (depth 0) on G's line, the rear rows behind it; not
// snapped, not reserved. Four vehicles in a 2 x 2 block ordered east on open ground, two of attack range 100 (the front
// rank, §4.3 item 1) and two of 300: the front pair's goals lie on the line through G (along d1: 0), the rear pair's
// behind it by the row step, and no goal is on a cell centre.
void test_member_stop() {
    ::alamo::test::section("member_stop");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 60);
    const V2 start[4] = {cell_center(10, 19), cell_center(10, 21), cell_center(8, 19), cell_center(8, 21)};
    for (ObjectId id = 1; id <= 4; ++id) CHECK(sys.add_unit(vehicle(id, start[id - 1])));
    sys.set_attack_range_query([](ObjectId id) { return id <= 2 ? 100.0f : 300.0f; });  // two ranks: two rows
    SimRng rng(5);
    MoveOrder o;
    o.units = {1, 2, 3, 4};
    o.destination = {801.0f, 403.0f};  // off a cell centre
    sys.move(o, rng, 1);
    CHECK(sys.stats().formation_paths == 1 && sys.stats().lanes_set == 4);
    V2 c{};
    for (const V2& p : start) c = c + p * 0.25f;
    const V2 g = o.destination;
    const V2 dd = g - c;
    const V2 d1 = dd * (1.0f / length(dd));
    float front = -1e9f, rear = 1e9f;
    uint32_t on_line = 0, off_centre = 0;
    for (ObjectId id = 1; id <= 4; ++id) {
        const V2 goal = sys.view(id).goal;
        const float a = dot(goal - g, d1);
        std::printf("member %d goal (%.3f, %.3f) along %.3f\n", static_cast<int>(id), static_cast<double>(goal.x),
                    static_cast<double>(goal.y), static_cast<double>(a));
        front = std::max(front, a);
        rear = std::min(rear, a);
        if (std::fabs(a) < 1e-2f) ++on_line;
        const V2 cc = cell_center(world_to_cell(goal.x), world_to_cell(goal.y));
        if (!(distance(goal, cc) < 1e-3f)) ++off_centre;
    }
    CHECK(std::fabs(front) < 1e-2f);  // the front row stops on G's line, not ahead of it
    CHECK(on_line == 2);
    CHECK(rear < -10.0f);  // the rear row behind it
    CHECK(off_centre == 4);
}


// bkm1 t06 (b0051, MOVEMENT §3.5.2 "waypoint z" items 4-5): C.11's rest is the 3-D sum of the stored waypoints, and the
// pathfinder stores z = 0, so an ordinary path is exactly 2-D (the walk step reads no terrain); only a slot-leg path's last
// waypoint (G with the target's height, LandMotion::end_z keyed to its point) adds sqrt(l^2 + z^2) on that segment.
// v^2 / (2a) = 17.5 between the 2-D rest 17 and the 3-D rest 7 + sqrt(10^2 + 6^2) = 18.66.
void test_stored_z_rest() {
    ::alamo::test::section("bkm1_stored_z_rest");
    LocomotorParams lp;
    lp.max_speed = 2.0f;
    lp.accel = 1.0f;
    lp.decel = 4.0f / 35.0f;
    lp.turn_rate = 10.0f;
    const auto run = [&](float end_z, V2 end_at) {
        LandMotion m;
        m.pos = {1.0f, 0.0f};
        m.yaw = 0.0f;
        m.speed = 2.0f;
        m.state = LocoState::WalkMoving;
        m.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}};
        m.next = 1;
        m.end_z = end_z;
        m.end_at = end_at;
        walk_step(m, lp, 3.0e38f, nullptr);
        return m.state;
    };
    const LocoState flat = run(0.0f, {20.0f, 0.0f});
    const LocoState raised = run(6.0f, {20.0f, 0.0f});
    const LocoState other = run(6.0f, {20.0f, 1.0f});  // a height stored for another path's end (a push record on top)
    std::printf("bkm1 stored z: flat %d, slot-leg G at z 6 %d, keyed to another point %d (End Move %d)\n", static_cast<int>(flat),
                static_cast<int>(raised), static_cast<int>(other), static_cast<int>(LocoState::WalkEndMove));
    CHECK(flat == LocoState::WalkEndMove);  // 2-D rest 17 < 17.5
    CHECK(raised != LocoState::WalkEndMove);  // 3-D rest 18.66 > 17.5
    CHECK(other == LocoState::WalkEndMove);
}


// bkm1 t08 (b0050, MOVEMENT §3.5.3): UseSecondaryFacing. (a) The move step's B2 = clamp(delta2 / SecondaryTurnAngle x f,
// Min, Max) x R from the error between Y2 and the primary target, the body yaw untouched; (b) within the lookahead of
// the segment end Y2 aims at the next segment (B2 = Min x R: delta2 is measured to the primary target); (c) the arrival
// turn ends when Y2 lands at r x SecondaryTurnInPlaceROTCoefficient - with 0.5 a 90-degree turn at 30 / frame lasts
// 3 frames longer than the body's (Y2 lands on frame 6 with nothing left, reached on frame 7), the body yaws the same.
void test_secondary_facing() {
    ::alamo::test::section("bkm1_secondary_facing");
    LocomotorParams lp;
    lp.max_speed = 2.0f;
    lp.accel = lp.decel = 1.0f;
    lp.turn_rate = 10.0f;
    lp.secondary = true;
    {
        LandMotion m;
        m.pos = {1.0f, 0.0f};
        m.speed = 2.0f;
        m.state = LocoState::WalkMoving;
        m.path = {{0.0f, 0.0f}, {20.0f, 0.0f}};
        m.next = 1;
        m.yaw2 = 45.0f;
        walk_step(m, lp, 3.0e38f, nullptr);
        std::printf("bkm1 secondary B2: yaw %.3f, Y2 45 -> %.3f (want 40)\n", static_cast<double>(m.yaw), static_cast<double>(m.yaw2));
        CHECK(m.yaw == 0.0f && m.yaw2 == 40.0f);
        LocomotorParams off = lp;
        off.secondary = false;
        LandMotion k;
        k.pos = {1.0f, 0.0f};
        k.speed = 2.0f;
        k.state = LocoState::WalkMoving;
        k.path = {{0.0f, 0.0f}, {20.0f, 0.0f}};
        k.next = 1;
        k.yaw2 = 45.0f;
        walk_step(k, off, 3.0e38f, nullptr);
        CHECK(k.yaw2 == 45.0f);  // without the flag nothing steps Y2
    }
    {
        LocomotorParams la = lp;
        la.sec_min = 0.4f;
        la.sec_max = 0.9f;
        const auto run = [&](float lookahead) {
            la.sec_lookahead = lookahead;
            LandMotion m;
            m.pos = {8.0f, 0.0f};
            m.speed = 2.0f;
            m.state = LocoState::WalkMoving;
            m.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};
            m.next = 1;
            walk_step(m, la, 3.0e38f, nullptr);
            return m.yaw2;
        };
        const float near_end = run(5.0f), none = run(0.0f);
        std::printf("bkm1 secondary lookahead: 5 -> Y2 %.3f (want 4), 0 -> %.3f (want 0)\n", static_cast<double>(near_end),
                    static_cast<double>(none));
        CHECK(near_end == 4.0f);
        CHECK(none == 0.0f);
    }
    const auto arrival = [](bool secondary, std::vector<float>& yaws) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc d = vehicle(1, cell_center(10, 10), 2.0f);
        d.stopped_turn_rate = 30.0f;
        d.use_secondary_facing = secondary;
        d.secondary_in_place = 0.5f;
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        o.final_facing_deg = 90.0f;
        sys.move(o, rng, 0);
        uint32_t f = 1;
        for (; f < 600 && !sys.view(1).arrived; ++f) sys.tick(f, rng);
        int turning = 0;
        for (int k = 0; k < 40; ++k, ++f) {
            sys.tick(f, rng);
            if (sys.view(1).arrival_turn) ++turning;
            yaws.push_back(sys.view(1).yaw);
        }
        return turning;
    };
    std::vector<float> ya, yb;
    const int plain = arrival(false, ya), sec = arrival(true, yb);
    std::printf("bkm1 secondary arrival turn: %d frames plain, %d with UseSecondaryFacing\n", plain, sec);
    CHECK(plain > 0 && sec == plain + 3);
    CHECK(ya == yb && !ya.empty() && ya.back() == 90.0f);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash keeps the log
    test_shield_class();  // bkm1 t03 (b0035)
    test_unplanned_order_kept();  // bkm1 t07 (b0055)
    test_lane_segments();         // bkm1 t01 (b0095)
    test_engagement_recruit();    // bkm1 t04 (b0036)
    test_member_stop();           // bkm1 t02 (b0096)
    test_stored_z_rest();         // bkm1 t06 (b0051)
    test_secondary_facing();      // bkm1 t08 (b0050)
    return ::alamo::test::report("alamo_tactical_movement_bkm_tests");
}
