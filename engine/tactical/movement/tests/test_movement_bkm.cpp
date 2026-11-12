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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash keeps the log
    test_shield_class();  // bkm1 t03 (b0035)
    test_unplanned_order_kept();  // bkm1 t07 (b0055)
    test_lane_segments();         // bkm1 t01 (b0095)
    return ::alamo::test::report("alamo_tactical_movement_bkm_tests");
}
