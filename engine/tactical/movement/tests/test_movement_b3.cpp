// alamo_tactical_movement_b3_tests (no framework; non-zero exit on failure).
// s78n t23 (t20 batch 3): the t21 review follow-ups (step 0) - halt's brake stub, crush and locomotor-less objects, a
// squad's final facing after an early arrival; the deployed half width (MOVEMENT §5.3 "When the deployed pair is used");
// MovingVsMoving for every pair (§5.4.2, §5.4.6).
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

[[maybe_unused]] ParallelFor make_thread_pf(unsigned threads) { return ::alamo::test::make_thread_parallel_for(threads); }

[[maybe_unused]] bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

PassabilityGrid open_grid(uint32_t w, uint32_t h) {
    PassabilityGrid g;
    g.init(w, h);
    return g;
}

UnitDesc unit(ObjectId id, V2 pos, int32_t cls, float speed, float radius) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = speed;
    d.turn_rate = 6.0f;
    d.alt_max_speed = speed;
    d.alt_turn_rate = 6.0f;
    d.movement_class = cls;
    d.collision_class = kUccInfantry;
    d.radius = radius;
    d.type_name = "B3_UNIT";
    return d;
}

void land_system(MovementSystem& sys, uint32_t cells) {
    PassabilityGrid g = open_grid(cells, cells);
    const float hi = static_cast<float>(g.width() - 1) * kCellSize;
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float, float) { return 0.0f; });
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
}

UnitDesc vehicle(ObjectId id, V2 pos, int32_t owner, float hx = 10.0f, float hy = 6.0f, float speed = 1.0f) {
    UnitDesc d = unit(id, pos, 2, speed, 12.0f);
    d.collision_class = kUccVehicle;
    d.hard_x = hx;
    d.hard_y = hy;
    d.owner = owner;
    return d;
}

// A 9-member squad on a 3 x 3 grid of pitch 12 (member half extents 3): container `cid`, members cid + 1 .. cid + 9.
void squad(MovementSystem& sys, ObjectId cid, V2 at, int32_t owner, float cmax = 0.8f) {
    UnitDesc c = unit(cid, at, 2, cmax, 12.0f);
    c.team_container = true;
    c.container_arrangement = 0;
    c.owner = owner;
    c.type_hx = c.type_hy = 0.0f;
    CHECK(sys.add_unit(c));
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    ObjectId id = cid + 1;
    for (int32_t ix = 1; ix >= -1; --ix)
        for (int32_t iy = 1; iy >= -1; --iy) {
            const V2 rel{12.0f * static_cast<float>(ix), 12.0f * static_cast<float>(iy)};
            UnitDesc d = unit(id, at + rel, 2, 1.0f, 3.0f);
            d.accel = d.decel = 0.05f;
            d.owner = owner;
            d.formation_spacing = 0.3f;
            CHECK(sys.add_unit(d));
            CHECK(sys.set_team_member_shape(d.id, 3.0f, 3.0f, 3.0f, 3.0f));
            ids.push_back(id++);
            slots.push_back(rel);
        }
    CHECK(sys.set_land_team(cid, ids, slots));
    CHECK(sys.set_team(cid, cid, 1.0f, cmax));
    for (ObjectId m : ids) CHECK(sys.set_team(m, cid, 1.0f, cmax));
}

// ---------------------------------------------------------------- step 0: t21 review follow-ups
// halt() keeps the speed: the unit brakes along its facing over v^2 / (2 decel) (the End Move brake stub), no order left.
void test_step0_halt_brake() {
    ::alamo::test::section("step0_halt_brake");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 80);
    UnitDesc d = vehicle(1, cell_center(10, 40), 0);
    d.accel = d.decel = 0.05f;
    CHECK(sys.add_unit(d));
    SimRng rng(301);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(70, 40);
    sys.move(o, rng, 1);
    uint32_t f = 1;
    for (; f < 200 && sys.view(1).speed < sys.view(1).max_speed; ++f) sys.tick(f, rng, serial_for);
    const UnitView v0 = sys.view(1);
    const float v = v0.speed, mult = v0.max_speed / d.max_speed;  // the land multiplier scales the speed and the rates
    const float stub = v * v / (2.0f * d.decel * mult);
    CHECK(v == v0.max_speed && v0.has_goal);
    const ObjectId one[] = {1};
    sys.halt(one, f);
    const UnitView h = sys.view(1);
    CHECK(!h.has_goal && h.speed == v && h.pushed && h.push_kind == 1);
    float travelled = 0.0f;
    V2 p = h.pos;
    for (uint32_t n = 0; n < 200 && sys.view(1).speed > 0.0f; ++n) {
        sys.tick(++f, rng, serial_for);
        travelled += distance(p, sys.view(1).pos);
        p = sys.view(1).pos;
    }
    // the walk brakes along the stub (v^2 / (2 decel)) and stops at its end (one step of slack)
    CHECK(sys.view(1).speed == 0.0f && !sys.view(1).pushed && travelled > stub - v && travelled <= stub + 1e-3f);
    CHECK(std::fabs(p.y - v0.pos.y) < 1e-3f);
    std::printf("step0 halt: braked %.3f along the facing (stub v^2 / 2a = %.3f)\n", static_cast<double>(travelled),
                static_cast<double>(stub));
}

// A crusher never crushes an object without a locomotor (a death-clone obstacle), even a hostile squashable one.
void test_step0_crush_no_locomotor() {
    ::alamo::test::section("step0_crush_no_locomotor");
    for (int clone = 0; clone < 2; ++clone) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 80);
        UnitDesc c = vehicle(1, cell_center(10, 40), 0, 20.0f, 12.0f);
        c.crusher = true;
        CHECK(sys.add_unit(c));
        UnitDesc v = unit(2, cell_center(13, 40), 2, 1.0f, 4.0f);
        v.owner = 1;
        v.max_speed = 0.0f;  // it cannot flee (the crush-flee push fails): the crusher drives on through it
        v.squashable = true;
        v.no_locomotor = clone != 0;
        v.death_clone_obstacle = clone != 0;
        CHECK(sys.add_unit(v));
        SimRng rng(302);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 40);
        sys.move(o, rng, 1);
        bool crushed = false;
        for (uint32_t f = 1; f < 120; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CrushEvent& e : sys.crush_events()) crushed = crushed || e.victim == 2;
        }
        CHECK(crushed == (clone == 0));
        std::printf("step0 crush: %s victim -> %s\n", clone ? "locomotor-less" : "walking", crushed ? "crushed" : "not crushed");
    }
}

// A squad ordered onto a standing unit arrives early and turns in place to its ordered facing (the final-facing test
// also runs when the arrival is not the container's own 0x15).
void test_step0_squad_final_facing() {
    ::alamo::test::section("step0_squad_final_facing");
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    const V2 at = cell_center(10, 20);
    squad(sys, 100, at, 0);
    const V2 goal = cell_center(25, 20);
    UnitDesc x = unit(70, goal, 2, 1.0f, 6.0f);
    x.owner = 1;
    CHECK(sys.add_unit(x));
    SimRng rng(303);
    sys.tick(1, rng, serial_for);
    MoveOrder o;
    o.units = {100};
    o.destination = goal;
    o.final_facing_deg = 90.0f;
    sys.move(o, rng, 2);
    bool arrived = false, turned = false;
    for (uint32_t f = 2; f < 900 && !arrived; ++f) {
        sys.tick(f, rng, serial_for);
        turned = turned || sys.view(100).team_final_turn;
        arrived = sys.view(100).arrived && !sys.view(100).team_final_turn;
    }
    const UnitView k = sys.view(100);
    CHECK(arrived && turned && sys.stats().early_arrivals >= 1 && std::fabs(wrap_deg(k.yaw - 90.0f)) < 1e-3f);
    std::printf("step0 squad: early arrival %.2f before the unit, final yaw %.2f\n", static_cast<double>(goal.x - k.pos.x),
                static_cast<double>(k.yaw));
}

// ---------------------------------------------------------------- 3.1 deployed half width (MOVEMENT §5.3)
// MPTL-like (Y 15, deployed Y 24, scale 1.1): the half width 16.5 -> 26.4 while deploying and deployed, 16.5 while
// undeploying; the rectangle never changes (no deployed X). MAL-like (scale 0.7): 10.5 -> 16.8. A synthetic deployed X
// 40 with no deployed Y: the rectangle (40, 0) while deployed, the half width keeps the normal Y.
void test_t20_deployed_width() {
    ::alamo::test::section("t20_deployed_width");
    using DR = MovementSystem::DeployResult;
    struct Case {
        float hx, hy, xd, yd;
    };
    const Case cases[3] = {{30.0f * 1.1f, 15.0f * 1.1f, 0.0f, 24.0f * 1.1f},
                           {20.0f * 0.7f, 15.0f * 0.7f, 0.0f, 24.0f * 0.7f},
                           {30.0f, 16.5f, 40.0f, 0.0f}};
    for (int k = 0; k < 3; ++k) {
        const Case& q = cases[k];
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 60);
        UnitDesc d = vehicle(1, cell_center(20, 20), 0, q.hx, q.hy);
        d.hard_x_deployed = q.xd;
        d.hard_y_deployed = q.yd;
        CHECK(sys.add_unit(d));
        UnitDesc n = vehicle(2, cell_center(20, 20) + V2{q.hx + 20.0f, 0.0f}, 0, 20.0f, 10.0f);
        CHECK(sys.add_unit(n));
        SimRng rng(311);
        const bool rect_switch = q.xd > 0.0f;
        const V2 normal{q.hx, q.hy}, deployed = rect_switch ? V2{q.xd, 0.0f} : normal;
        const float hw_dep = q.yd > 0.0f ? q.yd : q.hy;
        CHECK(sys.collision_half(1) == normal && sys.collision_half_width(1) == q.hy);
        CHECK(sys.deploy(1, true, 1) == DR::Deploying && sys.deploy_state(1) == LocoState::WalkDeploying);
        CHECK(sys.collision_half(1) == deployed && sys.collision_half_width(1) == hw_dep);
        sys.tick(1, rng, serial_for);  // no clip data: the Deploy clip ends on the frame it starts
        CHECK(sys.deploy_state(1) == LocoState::WalkDeployed);
        CHECK(sys.collision_half(1) == deployed && sys.collision_half_width(1) == hw_dep);
        CHECK(sys.deploy(1, false, 2) == DR::Undeploying && sys.deploy_state(1) == LocoState::WalkUndeploying);
        CHECK(sys.collision_half(1) == normal && sys.collision_half_width(1) == q.hy);
        sys.tick(2, rng, serial_for);
        CHECK(sys.deploy_state(1) == LocoState::None && sys.collision_half(1) == normal && sys.collision_half_width(1) == q.hy);
        std::printf("t20 deployed width: case %d - half width %.2f -> %.2f, rectangle (%.1f, %.1f) -> (%.1f, %.1f)\n", k,
                    static_cast<double>(q.hy), static_cast<double>(hw_dep), static_cast<double>(normal.x),
                    static_cast<double>(normal.y), static_cast<double>(deployed.x), static_cast<double>(deployed.y));
    }
}

}  // namespace

int main() {
    test_step0_halt_brake();
    test_step0_crush_no_locomotor();
    test_step0_squad_final_facing();
    test_t20_deployed_width();
    return ::alamo::test::report("alamo_tactical_movement_b3_tests");
}
