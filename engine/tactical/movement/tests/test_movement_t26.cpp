// alamo_tactical_movement_t26_tests (no framework; non-zero exit on failure).
// s78n t26.1: walking while deployed (docs/re/MOVEMENT.md §3.2.1): the cap test 0x3ac100 at a path start, the 0x19 walk
// branch with the Deployed_Walk_Speed cap, the 0x16 idle rules (the flag cleared at the stop; an auto-deployer with
// M > 0 and no DEPLOY ability deploys), Deploy(false) from the flag, the move order's skip of deployed manual deployers
// (§6.2) and the gate of deploying ones, the push / StoppedVsStopped refusal for M <= 0, Take_Damage's deploy, a
// lowering hover host's deploy and the walk-out arrivals reported to the garrison host.
// s78n t26.2: the path record's speed fractions and turn-done byte, the speed controller B (corner cap, brake-ahead, End
// Move T = a) and the start rule A with the 3.5.1 turn in place (MOVEMENT 3.5.2 A / B, 3.5.1 "Arrival turn, details"
// items 3-5).
// s78n t26.3: the corner-speed pass D (MOVEMENT 3.5.2 D with the RE 2 corrections): corner speeds, the merge and the aim
// point, the probe step-downs, the brake pass, and Min_Speed_Fraction_For_Turn on an order path.
// s78n t26.4: the Walk move step C (MOVEMENT 3.5.2 C, G, 3.5.1 0x15): the turn first, the travel along the facing to the
// waypoint line, the tau = 0 skip, the End Move coast along the last displacement, the point test, the pop into the path
// below, 0x15 within 5 / restart, no snap, the unaligned new order's coast, and a save inside the coast.
// s78n t26.5: the land team container's move step (MOVEMENT 3.5.2 E): the budget without f, the coast and travel along the
// facing, the re-lays at the waypoints stepped and after a pop, and a new order behind a marching squad.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;

namespace {

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

using DE = MovementSystem::DeployEvent;

// Destroyer_Droid-like (FoC XML): Max_Speed 3.6, OverrideAcceleration / Deceleration 0.5, Deploys + Auto_Deploys,
// Max_Dist_Walk_While_Deployed 150, Deployed_Walk_Speed 2.5, Deployment_Anim_Rate 3; no DEPLOY ability.
UnitDesc droid(ObjectId id, V2 pos) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = 3.6f;
    d.turn_rate = 15.5f;
    d.stopped_turn_rate = 15.5f;
    d.alt_max_speed = 3.6f;
    d.alt_turn_rate = 15.5f;
    d.accel = d.decel = 0.5f;
    d.movement_class = 0;
    d.collision_class = kUccInfantry;
    d.radius = 6.0f;
    d.type_name = "T26_DROID";
    d.locomotor = LocomotorKind::Default;
    d.deploys = true;
    d.auto_deploys = true;
    d.max_walk_deployed = 150.0f;
    d.deployed_walk_speed = 2.5f;
    d.deployment_anim_rate = 3.0f;
    return d;
}

// AT-AT-like manual deployer (Auto_Deploys No, M 0) and an MPTL / MAL-like auto-deployer with M 0.
UnitDesc walker(ObjectId id, V2 pos, bool auto_deploys) {
    UnitDesc d = droid(id, pos);
    d.type_name = auto_deploys ? "T26_MAL" : "T26_ATAT";
    d.auto_deploys = auto_deploys;
    d.max_walk_deployed = 0.0f;
    d.deployed_walk_speed = 0.0f;
    d.deployment_anim_rate = 1.0f;
    d.deploy_ability = true;
    return d;
}

UnitDesc tank(ObjectId id, V2 pos) {
    UnitDesc d = droid(id, pos);
    d.type_name = "T26_TANK";
    d.deploys = d.auto_deploys = false;
    d.max_walk_deployed = d.deployed_walk_speed = 0.0f;
    return d;
}

void land_system(MovementSystem& sys) {
    PassabilityGrid g;
    g.init(100, 100);
    const float hi = static_cast<float>(g.width() - 1) * kCellSize;
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float, float) { return 0.0f; });
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
    sys.set_deploy_clip_fn([](ObjectId, int32_t, uint32_t& n, float& fps) {
        n = 30;
        fps = 30.0f;
        return true;
    });
}

void order(MovementSystem& sys, SimRng& rng, std::vector<ObjectId> ids, V2 to, uint32_t frame) {
    MoveOrder o;
    o.units = std::move(ids);
    o.destination = to;
    sys.move(o, rng, frame);
}

// Runs ticks [from, to) and returns the deploy events (in order).
std::vector<DE> run(MovementSystem& sys, SimRng& rng, uint32_t from, uint32_t to) {
    std::vector<DE> ev;
    for (uint32_t f = from; f < to; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events()) ev.push_back(e);
    }
    return ev;
}

// Ticks until unit `id` has reached 0x19 with no clip timing (deployed and settled); returns the frame after.
uint32_t settle_deployed(MovementSystem& sys, SimRng& rng, ObjectId id, uint32_t f, uint32_t limit) {
    for (; f < limit; ++f) {
        sys.tick(f, rng);
        (void)sys.take_deploy_events();
        if (sys.deploy_state(id) == LocoState::WalkDeployed && sys.view(id).deploy_clip < 0) return f + 1;
    }
    return f;
}

// ---------------------------------------------------------------- the droid: idle deploy, walk deployed, roll undeployed
void test_droid() {
    ::alamo::test::section("Destroyer Droid: deploy at the stop, walk deployed under M, undeploy over M");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    const V2 p0 = cell_center(20, 50);
    CHECK(sys.add_unit(droid(1, p0)));
    SimRng rng(2601);
    // 0x16 idle, a `Deploys` type with M > 0 and no DEPLOY ability: Deploy(true) on the first tick (0x18), 0x19 when the
    // clip ends (30 frames at rate 3: position >= 30 - 2 - 3 after 9 steps).
    std::vector<DE> ev = run(sys, rng, 1, 2);
    CHECK(ev.size() == 1 && ev[0].id == 1 && ev[0].from == LocoState::None && ev[0].to == LocoState::WalkDeploying && !ev[0].finished);
    ev = run(sys, rng, 2, 40);
    CHECK(ev.size() == 1 && ev[0].to == LocoState::WalkDeployed && ev[0].finished);
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed && !sys.walking_deployed(1) && sys.view(1).pos == p0);
    CHECK(!sys.move_skips(1));  // an auto-deployer is not skipped by a move order

    // An order 100 ahead (L = 100 < M = 150): the flag at the path start, the 0x19 branch (speed 0, 0x0B), then the walk
    // at OverrideAcceleration x 1.56 = 0.78 per tick up to the Deployed_Walk_Speed cap 2.5. Still 0x19 for every reader,
    // no deploy event. The plan runs at the order's start frame (the formation delay); the flag is set there and the
    // branch tick (speed 0, 0x0B) is that same tick.
    ev.clear();
    order(sys, rng, {1}, p0 + V2{100.0f, 0.0f}, 40);
    std::vector<float> speed, dist;
    bool flag_after = true, deployed_always = true;
    size_t k0 = 0;  // the first tick with the flag
    uint32_t f = 40;
    for (; f < 300 && !sys.view(1).arrived; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events()) ev.push_back(e);
        const UnitView v = sys.view(1);
        if (speed.empty() && !v.walk_deployed) {
            CHECK(v.pos == p0 && v.speed == 0.0f);
            continue;
        }
        if (speed.empty()) k0 = f;
        speed.push_back(v.speed);
        dist.push_back(distance(v.pos, p0));
        if (!v.arrived) flag_after = flag_after && v.walk_deployed && v.walk_state != LocoState::WalkStopped;
        deployed_always = deployed_always && v.deploy_state == LocoState::WalkDeployed;
    }
    CHECK(ev.empty());
    // s78n t26.4 (MOVEMENT 3.5.2 C.11, 3.5.1 item 1): no snap onto the goal - End Move brakes from v^2 / (2a) > rest and
    // 0x15 at v <= a ends it where it stands, within one step (the 2.5 cap) of the goal
    CHECK(flag_after && deployed_always && sys.view(1).arrived && near(distance(sys.view(1).pos, p0), 100.0f, 2.5f));
    std::printf("droid deployed walk: stopped %.4f from the start (goal 100)\n", static_cast<double>(distance(sys.view(1).pos, p0)));
    CHECK(speed.size() > 6);
    if (speed.size() > 6) {
        CHECK(speed[0] == 0.0f && near(speed[1], 0.78f, 1e-5f) && near(speed[2], 1.56f, 1e-5f) && near(speed[3], 2.34f, 1e-5f) &&
              speed[4] == 2.5f);
        CHECK(dist[0] == 0.0f && near(dist[1], 0.78f, 1e-3f) && near(dist[2], 2.34f, 1e-3f) && near(dist[3], 4.68f, 1e-3f) &&
              near(dist[4], 7.18f, 1e-3f));
        CHECK(*std::max_element(speed.begin(), speed.end()) == 2.5f);
        std::printf("droid deployed walk: flag from frame %zu, speeds %.2f %.2f %.2f %.2f %.2f, %zu ticks to arrive\n", k0,
                    static_cast<double>(speed[0]), static_cast<double>(speed[1]), static_cast<double>(speed[2]),
                    static_cast<double>(speed[3]), static_cast<double>(speed[4]), speed.size());
    }
    // The stop: the flag is cleared at once, 0x19 with the Deploy clip on its last frame, no deploy event; one deployed
    // pose event (the host's Deploy clip) on that tick.
    uint32_t poses = 0;
    for (const uint32_t end = f + 5; f < end; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events()) ev.push_back(e);
        for (const ObjectId id : sys.deploy_pose_events()) poses += id == 1 ? 1u : 100u;
    }
    CHECK(ev.empty() && poses == 1 && !sys.walking_deployed(1) && sys.deploy_state(1) == LocoState::WalkDeployed &&
          sys.view(1).speed == 0.0f && sys.view(1).deploy_clip == 0x19);  // the Deploy clip (ModelAnimType 25)

    // An order 200 ahead (L >= M): Deploy(false) at the path start (0x1A, no walk while it runs), then the walk at full
    // speed (3.6 x 1.56 = 5.616) and the deploy at the stop.
    const V2 p1 = sys.view(1).pos;
    order(sys, rng, {1}, p1 + V2{200.0f, 0.0f}, f);
    sys.tick(f, rng);
    ev = sys.take_deploy_events();
    ++f;
    CHECK(ev.size() == 1 && ev[0].from == LocoState::WalkDeployed && ev[0].to == LocoState::WalkUndeploying);
    CHECK(sys.view(1).pos == p1 && !sys.walking_deployed(1));
    float vmax = 0.0f;
    bool undeployed_seen = false, redeployed = false;
    for (uint32_t end = f + 400; f < end && !redeployed; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events()) {
            if (e.to == LocoState::None && e.finished) undeployed_seen = true;
            if (e.to == LocoState::WalkDeploying) redeployed = true;
        }
        vmax = std::max(vmax, sys.view(1).speed);
    }
    CHECK(undeployed_seen && redeployed && sys.view(1).arrived && near(vmax, 3.6f * 1.56f, 1e-4f));
    std::printf("droid over M: undeploy, roll at %.3f, deploy at the stop (frame %u)\n", static_cast<double>(vmax), f);
}

// ---------------------------------------------------------------- the deployed pose after an arrival turn
void test_pose_after_turn() {
    ::alamo::test::section("a deployed walk that ends with an arrival turn: the pose event after the turn");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    const V2 p0 = cell_center(20, 50);
    CHECK(sys.add_unit(droid(1, p0)));
    SimRng rng(2607);
    uint32_t f = settle_deployed(sys, rng, 1, 1, 60);
    MoveOrder o;
    o.units = {1};
    o.destination = p0 + V2{100.0f, 0.0f};
    o.final_facing_deg = 90.0f;  // the arrival turn runs after the path (0x0F .. 0x11)
    sys.move(o, rng, f);
    uint32_t pose_frame = 0, turn_seen = 0, poses = 0;
    for (const uint32_t end = f + 300; f < end; ++f) {
        sys.tick(f, rng);
        (void)sys.take_deploy_events();
        if (sys.view(1).arrival_turn) turn_seen = f;
        for (const ObjectId id : sys.deploy_pose_events()) {
            CHECK(id == 1);
            ++poses;
            pose_frame = f;
        }
    }
    CHECK(turn_seen > 0 && poses == 1 && pose_frame > turn_seen && !sys.walking_deployed(1) &&
          sys.deploy_state(1) == LocoState::WalkDeployed && std::fabs(wrap_deg(sys.view(1).yaw - 90.0f)) < 1e-3f);
    std::printf("pose after an arrival turn: turn until frame %u, pose at %u\n", turn_seen, pose_frame);
}

// ---------------------------------------------------------------- Deploy(false) while walking deployed; save round trip
void test_freeze_and_save() {
    ::alamo::test::section("0x1A from a deployed walk freezes the unit; save mid deployed walk");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    const V2 p0 = cell_center(20, 50);
    CHECK(sys.add_unit(droid(1, p0)));
    SimRng rng(2602);
    uint32_t f = settle_deployed(sys, rng, 1, 1, 60);
    order(sys, rng, {1}, p0 + V2{120.0f, 0.0f}, f);
    for (uint32_t end = f + 20; f < end; ++f) sys.tick(f, rng);
    CHECK(sys.walking_deployed(1) && sys.view(1).speed == 2.5f);

    // save / load mid walk: identical hash and continuation
    ByteWriter w;
    sys.write(w);
    MovementSystem copy{MovementConfig{}};
    land_system(copy);
    {
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done());
    }
    StateHasher ha, hb;
    sys.hash_units(ha);
    copy.hash_units(hb);
    CHECK(ha.digest() == hb.digest() && copy.walking_deployed(1));
    SimRng r2 = rng;
    for (uint32_t g = f; g < f + 40; ++g) {
        sys.tick(g, rng);
        copy.tick(g, r2);
    }
    StateHasher hc, hd;
    sys.hash_units(hc);
    copy.hash_units(hd);
    CHECK(hc.digest() == hd.digest() && sys.view(1).pos == copy.view(1).pos);
    f += 40;

    // Deploy(false) from the flag: 0x1A, the unit stands where it is (yaw and position frozen) until the clip ends, the
    // flag cleared; then the walk resumes undeployed (L is not re-tested).
    order(sys, rng, {1}, sys.view(1).pos + V2{100.0f, 0.0f}, f);
    for (uint32_t end = f + 10; f < end; ++f) sys.tick(f, rng);
    CHECK(sys.walking_deployed(1));
    CHECK(sys.deploy(1, false, f) == MovementSystem::DeployResult::Undeploying);
    const UnitView v0 = sys.view(1);
    bool frozen = true, undeployed = false;
    for (uint32_t end = f + 30; f < end && !undeployed; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events()) undeployed = undeployed || (e.to == LocoState::None && e.finished);
        if (!undeployed) frozen = frozen && sys.view(1).pos == v0.pos && sys.view(1).yaw == v0.yaw && !sys.walking_deployed(1);
    }
    CHECK(frozen && undeployed);
    for (uint32_t end = f + 10; f < end; ++f) sys.tick(f, rng);
    CHECK(distance(sys.view(1).pos, v0.pos) > 1.0f && !sys.walking_deployed(1));
}

// ---------------------------------------------------------------- the move order's skip / gate (MOVEMENT §6.2)
void test_manual_deployers() {
    ::alamo::test::section("move order: deployed manual deployers skipped, deploying ones gate, M = 0 auto-deployer undeploys");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    const V2 a = cell_center(20, 20), t = cell_center(20, 40), m = cell_center(20, 60), b = cell_center(20, 80);
    CHECK(sys.add_unit(walker(1, a, false)));  // AT-AT
    CHECK(sys.add_unit(tank(2, t)));
    CHECK(sys.add_unit(walker(3, m, true)));   // MAL-like: auto, M 0
    CHECK(sys.add_unit(walker(4, b, false)));  // a second AT-AT
    SimRng rng(2603);
    sys.tick(1, rng);
    CHECK(sys.deploy(1, true, 2) == MovementSystem::DeployResult::Deploying);
    CHECK(sys.deploy(3, true, 2) == MovementSystem::DeployResult::Deploying);
    uint32_t f = settle_deployed(sys, rng, 1, 2, 80);
    f = settle_deployed(sys, rng, 3, f, 120);
    CHECK(sys.move_skips(1) && !sys.move_skips(2) && !sys.move_skips(3));

    // all deployed manual: no move at all (no goal, not failed)
    order(sys, rng, {1}, a + V2{200.0f, 0.0f}, f);
    CHECK(!sys.view(1).has_goal && !sys.view(1).failed && sys.deploy_state(1) == LocoState::WalkDeployed);
    // the AT-AT with the tank: the tank moves, the AT-AT stays deployed with no goal
    order(sys, rng, {1, 2}, t + V2{200.0f, 0.0f}, f);
    CHECK(!sys.view(1).has_goal && !sys.view(1).failed && sys.view(2).has_goal);
    // the M = 0 auto-deployer: L >= 0 = M -> Deploy(false) at its path start
    order(sys, rng, {3}, m + V2{200.0f, 0.0f}, f);
    std::vector<DE> ev = run(sys, rng, f, f + 2);
    f += 2;
    CHECK(sys.deploy_state(3) == LocoState::WalkUndeploying && !sys.walking_deployed(3));
    CHECK(std::any_of(ev.begin(), ev.end(), [](const DE& e) { return e.id == 3 && e.to == LocoState::WalkUndeploying; }));
    for (uint32_t end = f + 60; f < end; ++f) sys.tick(f, rng);
    CHECK(sys.deploy_state(3) == LocoState::None && distance(sys.view(3).pos, m) > 10.0f);
    CHECK(sys.view(1).pos == a && distance(sys.view(2).pos, t) > 10.0f);

    // a formation of only deploying manual deployers fails the gate: no path while 0x18, then undeployed it moves
    CHECK(sys.deploy(4, true, f) == MovementSystem::DeployResult::Deploying);
    order(sys, rng, {4}, b + V2{200.0f, 0.0f}, f);
    CHECK(sys.view(4).has_goal);
    for (uint32_t end = f + 10; f < end; ++f) sys.tick(f, rng);
    CHECK(sys.view(4).pos == b && sys.view(4).has_goal);
    CHECK(sys.deploy(4, false, f) == MovementSystem::DeployResult::Undeploying);
    for (uint32_t end = f + 80; f < end; ++f) sys.tick(f, rng);
    CHECK(sys.deploy_state(4) == LocoState::None && distance(sys.view(4).pos, b) > 10.0f);
}

// ---------------------------------------------------------------- Take_Damage's deploy (0x3a9e30)
void test_damage_deploy() {
    ::alamo::test::section("Take_Damage deploys an idle M > 0 Deploys unit");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    UnitDesc d = droid(1, cell_center(20, 20));
    d.deploy_ability = true;  // no idle deploy with a DEPLOY ability
    CHECK(sys.add_unit(d));
    CHECK(sys.add_unit(tank(2, cell_center(20, 60))));
    SimRng rng(2604);
    std::vector<DE> ev = run(sys, rng, 1, 10);
    CHECK(ev.empty() && sys.deploy_state(1) == LocoState::None);
    sys.damage_deploy(2, 10);  // not Deploys: nothing
    sys.damage_deploy(1, 10);
    CHECK(sys.deploy_state(1) == LocoState::WalkDeploying && sys.deploy_state(2) == LocoState::None);
    // moving: no deploy
    MovementSystem mv{MovementConfig{}};
    land_system(mv);
    CHECK(mv.add_unit(d));
    order(mv, rng, {1}, cell_center(60, 20), 1);
    (void)run(mv, rng, 1, 5);
    mv.damage_deploy(1, 5);
    CHECK(mv.deploy_state(1) == LocoState::None);
}

// ---------------------------------------------------------------- push / StoppedVsStopped refuse M <= 0 deployed units
void test_push_refusal() {
    ::alamo::test::section("a deployed M = 0 unit is never pushed; a deployed droid walks aside deployed");
    // A vehicle on a forced straight path (the decision's step 2: push) drives through the spot of a stopped friendly
    // vehicle. Case 0: an undeployed one (pushed: the set-up pushes); case 1: a deployed M = 0 auto-deployer (never
    // moves); case 2: a deployed droid (M 150; it walks its push path deployed, 0x19 throughout).
    const auto vehicle = [](UnitDesc d) {
        d.collision_class = kUccVehicle;
        d.movement_class = 2;
        d.radius = 12.0f;
        d.hard_x = 10.0f;
        d.hard_y = 6.0f;
        d.owner = 1;
        return d;
    };
    for (int32_t c = 0; c < 3; ++c) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys);
        const V2 s = cell_center(22, 20);
        const UnitDesc b = c == 0 ? tank(1, s) : c == 1 ? walker(1, s, true) : droid(1, s);
        CHECK(sys.add_unit(vehicle(b)));
        CHECK(sys.add_unit(vehicle(tank(2, cell_center(15, 20)))));
        SimRng rng(2605);
        uint32_t f = 1;
        if (c == 1) CHECK(sys.deploy(1, true, 1) == MovementSystem::DeployResult::Deploying);
        if (c > 0) f = settle_deployed(sys, rng, 1, 1, 80);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(40, 20);
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, f);
        bool deployed = true, walked_deployed = false;
        float moved = 0.0f;
        for (uint32_t end = f + 600; f < end; ++f) {
            sys.tick(f, rng);
            (void)sys.take_deploy_events();
            if (c > 0) deployed = deployed && sys.deploy_state(1) == LocoState::WalkDeployed;
            walked_deployed = walked_deployed || sys.walking_deployed(1);
            moved = std::max(moved, distance(sys.view(1).pos, s));
        }
        if (c == 0) CHECK(moved > 10.0f);
        if (c == 1) CHECK(moved == 0.0f && deployed && !walked_deployed && !sys.view(2).arrived);
        if (c == 2) CHECK(deployed && moved > 10.0f && walked_deployed);
        std::printf("push refusal (case %d): moved %.2f, walked deployed %d, pushes %u, failures %u, mover %s\n", c,
                    static_cast<double>(moved), walked_deployed ? 1 : 0, sys.stats().pushes, sys.stats().push_failures,
                    sys.view(2).arrived ? "arrived" : "not arrived");
    }
}

// ---------------------------------------------------------------- hover host: deploy while lowering; walk-out arrivals
void test_hover_and_exit() {
    ::alamo::test::section("a lowering hover host times its Deploy clip once down; walk-out arrivals reported");
    MovementConfig unit_speed;
    unit_speed.constants.land_speed_multiplier = 1.0f;
    MovementSystem sys{unit_speed};
    land_system(sys);
    UnitDesc h = walker(1, cell_center(20, 50), false);  // HTT-like: Deploys, M 0, hover 80
    h.collision_class = kUccVehicle;
    h.hover_height = 80.0f;
    h.max_speed = h.alt_max_speed = 2.5f;
    h.accel = h.decel = 0.0f;
    CHECK(sys.add_unit(h));
    CHECK(sys.add_unit(tank(2, cell_center(20, 20))));
    SimRng rng(2606);
    order(sys, rng, {1}, cell_center(70, 50), 1);
    uint32_t f = 1;
    for (; f < 200 && sys.hover_state(1) != 1; ++f) sys.tick(f, rng);
    for (uint32_t end = f + 5; f < end; ++f) sys.tick(f, rng);
    CHECK(sys.hover_state(1) == 1);
    CHECK(sys.hover_lower(1) && sys.deploy(1, true, f) == MovementSystem::DeployResult::Deploying);
    (void)sys.take_deploy_events();
    uint32_t grounded = 0, deployed = 0;
    bool clip_while_lowering = false;
    for (uint32_t end = f + 120; f < end && deployed == 0; ++f) {
        sys.tick(f, rng);
        if (sys.hover_state(1) >= 2) clip_while_lowering = clip_while_lowering || sys.view(1).deploy_clip >= 0;
        if (grounded == 0 && sys.hover_state(1) == 0) grounded = f;
        for (const DE& e : sys.take_deploy_events())
            if (e.to == LocoState::WalkDeployed && e.finished) deployed = f;
    }
    CHECK(!clip_while_lowering && grounded > 0 && deployed >= grounded + 27);
    std::printf("hover host: grounded at %u, deployed at %u\n", grounded, deployed);

    // walk-out: a unit ordered out of host 1 reports (unit, host) when its move ends; a new order drops it unreported
    order(sys, rng, {2}, cell_center(30, 20), f);
    CHECK(sys.set_garrison_exit(2, 1));
    std::vector<std::pair<ObjectId, ObjectId>> arr;
    for (uint32_t end = f + 300; f < end && arr.empty(); ++f) {
        sys.tick(f, rng);
        for (const auto& p : sys.take_exit_arrivals()) arr.push_back(p);
    }
    CHECK(arr.size() == 1 && arr[0].first == 2 && arr[0].second == 1 && sys.view(2).arrived);
    order(sys, rng, {2}, cell_center(40, 20), f);
    CHECK(sys.set_garrison_exit(2, 1));
    order(sys, rng, {2}, cell_center(45, 20), f);
    for (uint32_t end = f + 300; f < end; ++f) {
        sys.tick(f, rng);
        for (const auto& p : sys.take_exit_arrivals()) arr.push_back(p);
    }
    CHECK(arr.size() == 1 && sys.view(2).arrived);
}

// ---------------------------------------------------------------- the deploy state across a garrison stay
void test_garrison_keep() {
    ::alamo::test::section("garrison_remove / restore_deploy: a droid deployed on entry walks out deployed");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    const V2 p0 = cell_center(20, 50), exit = cell_center(24, 50);
    CHECK(sys.add_unit(droid(1, p0)));
    CHECK(sys.add_unit(droid(2, cell_center(20, 70))));
    SimRng rng(2608);
    uint32_t f = settle_deployed(sys, rng, 1, 1, 60);
    // entry: the unit leaves the system, its deploy state is kept (a unit without one keeps nothing)
    CHECK(sys.garrison_remove(1, f) && !sys.has_unit(1) && sys.deploy_kept(1));
    CHECK(!sys.garrison_remove(99, f));
    // the record is saved and hashed
    ByteWriter w;
    sys.write(w);
    MovementSystem copy{MovementConfig{}};
    land_system(copy);
    {
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done() && copy.deploy_kept(1));
    }
    StateHasher ha, hb;
    sys.hash_units(ha);
    copy.hash_units(hb);
    CHECK(ha.digest() == hb.digest());
    for (const uint32_t end = f + 20; f < end; ++f) sys.tick(f, rng);
    // exit: a fresh unit of the same id, the kept state put back - 0x19, no deploy event, the record gone
    CHECK(sys.add_unit(droid(1, exit)) && sys.restore_deploy(1, f) && !sys.deploy_kept(1));
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed && !sys.restore_deploy(1, f));
    std::vector<DE> ev = run(sys, rng, f, f + 10);
    f += 10;
    CHECK(ev.empty() && sys.deploy_state(1) == LocoState::WalkDeployed && sys.view(1).pos == exit);
    // its next order shorter than M walks deployed
    order(sys, rng, {1}, exit + V2{80.0f, 0.0f}, f);
    bool walked = false, deployed = true;
    for (const uint32_t end = f + 200; f < end && !sys.view(1).arrived; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events()) ev.push_back(e);
        walked = walked || sys.walking_deployed(1);
        deployed = deployed && sys.deploy_state(1) == LocoState::WalkDeployed;
    }
    CHECK(walked && deployed && ev.empty() && sys.view(1).arrived);
    // a unit deploying on entry resumes its clip where it was; one that dies inside drops its record
    CHECK(sys.deploy(2, false, f) == MovementSystem::DeployResult::Undeploying);
    for (const uint32_t end = f + 4; f < end; ++f) sys.tick(f, rng);
    (void)sys.take_deploy_events();
    CHECK(sys.garrison_remove(2, f) && sys.deploy_kept(2));
    f += 50;
    CHECK(sys.add_unit(droid(2, cell_center(24, 70))) && sys.restore_deploy(2, f) && sys.deploy_state(2) == LocoState::WalkUndeploying);
    uint32_t undeployed = 0;
    for (const uint32_t end = f + 40; f < end && undeployed == 0; ++f) {
        sys.tick(f, rng);
        for (const DE& e : sys.take_deploy_events())
            if (e.id == 2 && e.to == LocoState::None && e.finished) undeployed = f;
    }
    CHECK(undeployed > 0);
    CHECK(sys.deploy(2, true, f) == MovementSystem::DeployResult::Deploying);
    CHECK(sys.garrison_remove(2, f) && sys.deploy_kept(2) && !sys.remove_unit(2) && !sys.deploy_kept(2));
    std::printf("garrison keep: deployed out and walked deployed; an undeploy resumed and ended at frame %u\n", undeployed);
}

// ---------------------------------------------------------------- released inside a footprint (FIGHTER_FLYING §7.4)
void test_released_inside() {
    ::alamo::test::section("a unit released at a destroyed host's centre gets out on its next order");
    // A structure footprint: cells 18 .. 22 x 48 .. 52 impassable; the occupant is put at its centre (0x37a060).
    MovementSystem sys{MovementConfig{}};
    {
        PassabilityGrid g;
        g.init(100, 100);
        for (int32_t y = 48; y <= 52; ++y)
            for (int32_t x = 18; x <= 22; ++x) g.set_base(x, y, kPassImpassable);
        const float hi = static_cast<float>(g.width() - 1) * kCellSize;
        sys.set_grid(std::move(g));
        sys.set_height_fn([](float, float) { return 0.0f; });
        sys.air().set_box({0.0f, 0.0f}, {hi, hi});
    }
    const V2 c = cell_center(20, 50);
    CHECK(sys.add_unit(tank(1, c)));
    // a death clone of the host at the same spot (no locomotor, the land obstacle)
    UnitDesc clone = tank(2, c);
    clone.max_speed = clone.alt_max_speed = 0.0f;
    clone.no_locomotor = true;
    clone.death_clone_obstacle = true;
    clone.owner = 2;
    CHECK(sys.add_unit(clone));
    SimRng rng(2609);
    uint32_t f = 1;
    for (; f < 10; ++f) sys.tick(f, rng);
    order(sys, rng, {1}, cell_center(40, 50), f);
    for (const uint32_t end = f + 600; f < end && !sys.view(1).arrived; ++f) sys.tick(f, rng);
    const UnitView v = sys.view(1);
    CHECK(v.arrived && !v.failed && distance(v.pos, cell_center(40, 50)) < 25.0f);
    std::printf("released inside: arrived %d at (%.1f, %.1f) by frame %u\n", v.arrived ? 1 : 0, static_cast<double>(v.pos.x),
                static_cast<double>(v.pos.y), f);
}


// ---------------------------------------------------------------- s78n t26.2: the speed controller B (pure)
void test_t26_controller() {
    ::alamo::test::section("t26_controller");
    // B.4 the corner cap: 0x20afe0 is |wrap(yaw - wanted)| < |R| (strict): an error of exactly R is capped, R - 0.01 not
    WalkSpeedIn c;
    c.v = 2.0f;
    c.vmax = 3.0f;
    c.acc = c.a = 0.05f;
    c.rate = 10.0f;
    c.frac_cur = 0.5f;
    c.yaw_err = 10.0f;
    const WalkSpeedOut capped = walk_speed(c);
    CHECK(near(capped.v, 1.95f, 1e-6f) && !capped.end_move);  // T = 0.5 x 3 = 1.5 < v: down by a
    c.yaw_err = 9.99f;
    const float open_v = walk_speed(c).v;
    CHECK(near(open_v, 2.05f, 1e-6f));  // within R: T = vmax, up by acc
    c.yaw_err = 10.0f;
    c.turn_done = true;
    CHECK(near(walk_speed(c).v, 2.05f, 1e-6f));  // the turn-done byte set: no cap
    c.turn_done = false;
    c.frac_cur = 0.0f;
    const WalkSpeedOut zero = walk_speed(c);
    CHECK(zero.end_move && near(zero.v, 1.95f, 1e-6f));  // a cap of 0 sets End Move, where T = a
    // B.7 End Move: T = a - v 1.0, a 0.25 -> 0.75, 0.5, 0.25, then stays 0.25 (hand)
    WalkSpeedIn e;
    e.v = 1.0f;
    e.vmax = 3.0f;
    e.acc = e.a = 0.25f;
    e.state = LocoState::WalkEndMove;
    std::vector<float> seq;
    for (int k = 0; k < 5; ++k) {
        e.v = walk_speed(e).v;
        seq.push_back(e.v);
    }
    CHECK(seq == (std::vector<float>{0.75f, 0.5f, 0.25f, 0.25f, 0.25f}));
    // B.2 a formation path at formation speed 0 -> End Move
    WalkSpeedIn fo;
    fo.v = 0.0f;
    fo.vmax = 3.0f;
    fo.acc = fo.a = 0.05f;
    fo.formation = 0.0f;
    CHECK(walk_speed(fo).end_move);
    // B.5 the brake-ahead on (0, 0), (100, 0), (100, 400) with the fractions set by hand to (1, 0.698132, 1) (the t26.3
    // corner speed of a 90-degree corner at R 6: v 2.094395, f 0.698132), vmax 3, a 0.05: vn = 2.094396, brake distance
    // (9 - vn^2) / 0.1 = 46.135 (hand). Line(1) is the bisector line through (100, 0): D = 100 - x along +x.
    const V2 n1 = normalize_retail(normalize_retail(V2{100.0f, 0.0f}) + normalize_retail(V2{0.0f, 400.0f}));
    CHECK(near(line_distance_along({54.0f, 0.0f}, {1.0f, 0.0f}, {100.0f, 0.0f}, n1), 46.0f, 1e-3f));
    CHECK(line_distance_along({54.0f, 0.0f}, {1.0f, 0.0f}, {100.0f, 0.0f}, {0.0f, 1.0f}) == 3.0e38f);  // d . n = 0
    LocomotorParams lp;
    lp.max_speed = 3.0f;
    lp.accel = lp.decel = 0.05f;
    lp.turn_rate = 6.0f;
    lp.close_enough_angle = 180.0f;
    const auto brake_at = [&](float x) {
        LandMotion m;
        m.pos = {x, 0.0f};
        m.speed = 3.0f;
        m.state = LocoState::WalkMoving;
        m.path = {{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 400.0f}};
        m.frac = {1.0f, 0.698132f, 1.0f};
        m.next = 1;
        m.turn_done = true;
        (void)walk_step(m, lp, 3.0e38f, nullptr);
        return m.speed;
    };
    const float far_v = brake_at(53.0f), near_v = brake_at(54.0f);
    CHECK(far_v == 3.0f && near(near_v, 2.95f, 1e-6f));  // D 47 > 46.135: T = 3; D 46: T = vn, down by a
    std::printf("t26.2 controller: corner cap at |e| = R %.3f, R - 0.01 %.3f; End Move %.2f %.2f %.2f %.2f; brake-ahead D 47 -> "
                "%.3f, D 46 -> %.3f\n",
                static_cast<double>(capped.v), static_cast<double>(open_v), static_cast<double>(seq[0]),
                static_cast<double>(seq[1]), static_cast<double>(seq[2]), static_cast<double>(seq[3]),
                static_cast<double>(far_v), static_cast<double>(near_v));
}

// ---------------------------------------------------------------- s78n t26.3: the corner-speed pass D (pure)
// MOVEMENT §3.5.2 D items 2-7 with the RE 2 corrections (L_turn and the third speed term take sin|theta|). vmax 3, R 6
// degrees / frame (omega 0.1047198), facing 0, no probe callbacks (open ground) unless stated.
CornerPassIn corner_in(float f_min, float decel) {
    CornerPassIn in;
    in.yaw0 = 0.0f;
    in.vmax = 3.0f;
    in.rate = 6.0f;
    in.f_min = f_min;
    in.decel = decel;
    in.seg = 2.0f;
    return in;
}

bool line_short_of_110(const void*, V2 a, V2 b) { return a.x <= 110.0f && b.x <= 110.0f; }  // a wall at x 110
bool line_below_5(const void*, V2 a, V2 b) { return a.y < 5.0f && b.y < 5.0f; }             // nothing passes beyond y 5
bool corridor_never(const void*, const std::vector<V2>&) { return false; }

void test_t26_corner_pass() {
    ::alamo::test::section("t26_corner_pass");
    const float w = 6.0f * dmath::kPiF / 180.0f;
    // (0, 0), (100, 0), (100, 40), f_min 0, no brake pass: corner 0 straight (theta 0: vmax); corner 1 theta pi / 2, L 40,
    // r = clamp(20, 20, 100) = 20: v = min(3, w 20 / 1 = 2.094395, 40 w / 2 = 2.094395) -> f 0.698132 (hand)
    const std::vector<V2> l40{{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 40.0f}};
    CornerPassStats s0;
    const std::vector<float> a = corner_fractions(l40, corner_in(0.0f, 0.0f), &s0);
    CHECK(a.size() == 3 && a[0] == 1.0f && near(a[1], 0.698132f, 1e-5f) && a[2] == 1.0f);
    CHECK(s0.zero == 0 && s0.slowed == 1 && s0.stepdowns == 0);
    // the brake pass (a 0.05): v 0 at the end; k = 1: sqrt(0.1 x 40) = 2 -> f1 = min(0.698132, 2 / 3) = 0.666667; k = 0:
    // sqrt(0.1 x 100 + 4) / 3 = 1.247 -> f0 stays 1 (hand)
    const std::vector<float> ab = corner_fractions(l40, corner_in(0.0f, 0.05f));
    CHECK(ab[0] == 1.0f && near(ab[1], 2.0f / 3.0f, 1e-6f) && ab[2] == 1.0f);
    // (0, 0), (100, 0), (100, 40), (100, 400): the corner keeps its speed when the path goes on (brake pass k = 2: sqrt(0.1 x
    // 360) = 6 -> 1, v 3; k = 1: sqrt(0.1 x 40 + 9) / 3 = 1.2 -> 0.698132 stays): (1, 0.698132, 1, 1) - the fractions the t26.2
    // brake-ahead case sets by hand. (On (0, 0), (100, 0), (100, 400) the leg is 400 long: r = 100, v = vmax, all 1.0.)
    const std::vector<float> a400 =
        corner_fractions({{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 40.0f}, {100.0f, 400.0f}}, corner_in(0.0f, 0.05f));
    CHECK(a400.size() == 4 && a400[0] == 1.0f && near(a400[1], 0.698132f, 1e-5f) && a400[2] == 1.0f && a400[3] == 1.0f);
    CHECK(corner_fractions({{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 400.0f}}, corner_in(0.0f, 0.05f)) ==
          (std::vector<float>{1.0f, 1.0f, 1.0f}));
    // (0, 0), (100, 0), (100, 10), f_min 0.25: corner 1 v = min(3, 2.094395, 10 w / 2 = 0.523599) -> f 0.174533 < 0.25 ->
    // 0; corner 0: theta 0, L_turn 0, the aim point pulled back onto P_0 keeps theta 0 -> vmax (hand)
    const std::vector<V2> l10{{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 10.0f}};
    CornerPassStats s1;
    const std::vector<float> b = corner_fractions(l10, corner_in(0.25f, 0.0f), &s1);
    CHECK(b[0] == 1.0f && b[1] == 0.0f && b[2] == 1.0f && s1.zero == 1);
    const std::vector<float> b1 = corner_fractions(l10, corner_in(0.1f, 0.0f));  // f_min 0.1: 0.174533 stays
    CHECK(b1[0] == 1.0f && near(b1[1], 0.174533f, 1e-5f));
    // The merge (f_min 0.1): at corner 1 L 10 < L_turn = 2 x 3 x 1 / w = 57.30 and the next point (100, 20) is on the same
    // side -> merged: L 20, theta pi / 2, v = min(3, 2.094395, 20 w / 2 = 1.047198), f 0.349066 on waypoints 1 and 2 (hand)
    const std::vector<V2> l3{{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 10.0f}, {100.0f, 20.0f}};
    const std::vector<float> m = corner_fractions(l3, corner_in(0.1f, 0.0f));
    CHECK(m.size() == 4 && m[0] == 1.0f && near(m[1], 0.349066f, 1e-5f) && m[2] == m[1] && m[3] == 1.0f);
    // with f_min 0 there is no merge: corner 1 gets 0.174533, corner 2 (straight on along +y, theta 0) 1.0
    const std::vector<float> m0 = corner_fractions(l3, corner_in(0.0f, 0.0f));
    CHECK(near(m0[1], 0.174533f, 1e-5f) && m0[2] == 1.0f);
    // the next point on the other side, (110, -10): no merge, corner 1 0.174533; corner 2 from (100, 10): d_in (0, 1), d =
    // unit(10, -20), theta = acos(-0.894427) = 2.677945, L 22.36068, L_turn = 6 sin(theta) / w = 25.62 > L, no next point:
    // v = min(3, w 20 / (1 - cos theta) = 1.105556, L w / (2 sin theta) = 2.617994) -> f 0.368519 (hand)
    const std::vector<float> o = corner_fractions({{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 10.0f}, {110.0f, -10.0f}}, corner_in(0.1f, 0.0f));
    CHECK(near(o[1], 0.174533f, 1e-5f) && near(o[2], 0.368519f, 2e-5f) && o[3] == 1.0f);
    // The probe: a wall at x 110 across corner 1's arc (radius v / w, bulging to x = 100 + v / w): 2.094395 (bulge 20) and
    // 1.396263 (13.3) fail, 0.930842 (8.89) passes the line test and the open corridor -> f 0.310281, 2 step-downs (hand)
    CornerPassIn wall = corner_in(0.0f, 0.0f);
    wall.line_ok = line_short_of_110;
    CornerPassStats s2;
    const std::vector<float> c = corner_fractions(l40, wall, &s2);
    CHECK(near(c[1], 0.310281f, 1e-5f) && s2.stepdowns == 2);
    // a corridor that always fails: corner 0 (j = 0 always takes it) 3 -> 2 -> 1.333 -> 0.889 -> 0.593 -> 0.395 < vmin 0.5
    // with the line test passing -> vmin, f 0.166667; corner 1 2.094395 -> 1.396263 -> 0.930842 -> 0.620562 -> 0.413708 <
    // vmin -> vmin too (the vmin test comes before the next probe), f 0.166667 (hand)
    CornerPassIn cf = corner_in(0.0f, 0.0f);
    cf.corridor_ok = corridor_never;
    cf.vmin = 0.5f;
    const std::vector<float> d = corner_fractions(l40, cf);
    CHECK(near(d[0], 0.5f / 3.0f, 1e-6f) && near(d[1], 0.5f / 3.0f, 1e-6f));
    // vmin 0: corner 0 goes on to 0.263 < 0.1 vmax -> vmin 0; corner 1's 0.413708 is probed again and, < 0.2 vmax at
    // j != 0, accepted without the corridor -> f 0.137903 (hand)
    cf.vmin = 0.0f;
    const std::vector<float> d0 = corner_fractions(l40, cf);
    CHECK(d0[0] == 0.0f && near(d0[1], 0.137903f, 1e-5f));
    // the line test failing at every speed: down to 0 (the last line test failed), vmin or not
    CornerPassIn lf = corner_in(0.0f, 0.0f);
    lf.vmin = 0.5f;
    lf.line_ok = line_below_5;
    CHECK(corner_fractions(l40, lf)[1] == 0.0f);
    // The arc probe itself: corner 1 of l40 at v 2.094395 (radius 20), a left turn (theta_s -pi / 2): it starts at P_j,
    // bulges to x 120 at the middle and ends with the aim point
    std::vector<V2> pr;
    corner_arc_probe({100.0f, 0.0f}, {100.0f, 40.0f}, 2.094395f, w, -dmath::kHalfPiF, 2.0f, pr);
    float max_x = 0.0f;
    for (const V2 q : pr) max_x = std::max(max_x, q.x);
    CHECK(pr.size() > 3 && pr.front().x == 100.0f && pr.front().y == 0.0f && pr.back().x == 100.0f && pr.back().y == 40.0f &&
          near(max_x, 120.0f, 0.05f));
    // no pass when R is not > 0
    CornerPassIn none = corner_in(0.0f, 0.0f);
    none.rate = 0.0f;
    CHECK(corner_fractions(l40, none).empty());
    std::printf("t26.3 corner pass: 90-degree L 40 -> %.6f (brake pass %.6f); L 10 f_min 0.25 -> %.1f, 0.1 -> %.6f; merged %.6f; "
                "wall %.6f (%u step-downs); corridor fail %.6f / %.6f\n",
                static_cast<double>(a[1]), static_cast<double>(ab[1]), static_cast<double>(b[1]), static_cast<double>(b1[1]),
                static_cast<double>(m[1]), static_cast<double>(c[1]), s2.stepdowns, static_cast<double>(d[0]),
                static_cast<double>(d[1]));
}

// ---------------------------------------------------------------- s78n t26.2: the start rule A (pure walk_step)
struct StartOut {
    WalkResult r;
    LandMotion m;
};

StartOut start_once(float yaw, std::vector<float> frac) {
    LocomotorParams lp;
    lp.max_speed = 2.0f;
    lp.accel = lp.decel = 0.5f;
    lp.turn_rate = 10.0f;
    lp.close_enough_angle = 180.0f;
    StartOut o;
    o.m.pos = {0.0f, 0.0f};
    o.m.yaw = yaw;
    o.m.state = LocoState::WalkStopped;
    o.m.path = {{0.0f, 0.0f}, {100.0f, 0.0f}};
    o.m.frac = std::move(frac);
    o.m.next = 1;
    o.r = walk_step(o.m, lp, 3.0e38f, nullptr);
    return o;
}

void test_t26_start_rule() {
    ::alamo::test::section("t26_start_rule");
    // delta 90, CE 180, fraction 1: moves this tick (0x12 -> 0x13 and the move step in the same call)
    const StartOut a = start_once(-90.0f, {});
    CHECK(a.r.moved && !a.r.turn && a.m.speed == 0.5f && a.m.state == LocoState::WalkMoving);
    // fraction 0: the close-enough clause fails -> the turn in place toward the leg (the caller's 0xF / 0x10 / 0x11)
    const StartOut b = start_once(-90.0f, {0.0f});
    CHECK(!b.r.moved && b.r.turn && b.r.want == 0.0f && b.m.speed == 0.0f && b.m.yaw == -90.0f &&
          b.m.state == LocoState::WalkStartTurn);
    // delta <= R with fraction 0: moves
    const StartOut c = start_once(-10.0f, {0.0f});
    CHECK(c.r.moved && !c.r.turn);
    std::printf("t26.2 start rule: fraction 1 moves %d, fraction 0 turns %d (want %.1f), within R moves %d\n", a.r.moved ? 1 : 0,
                b.r.turn ? 1 : 0, static_cast<double>(b.r.want), c.r.moved ? 1 : 0);
}

// ---------------------------------------------------------------- s78n t26.2: the start turn in place (system)
struct StartTurnOut {
    uint32_t turn_ticks = 0, first_step = 0, clip_hold = 0, begin_plays = 0;
    std::vector<float> yaws;
    bool exact = false;
    float end_yaw = 1e9f;
};

// One vehicle at (400, 400) facing yaw0, ordered 400 along +x (path heading exactly 0); ticks until it first moves.
// first_step counts the ticks from the first one with a path (turn ticks + begin-clip hold ticks + the moving tick).
StartTurnOut start_turn_case(float yaw0, float rate, float stopped, float ce, bool pre, float f_min = 0.0f,
                             V2 to = {800.0f, 400.0f}) {
    StartTurnOut o;
    MovementConfig cfg;
    cfg.constants.land_speed_multiplier = 1.0f;  // R = Rate_Of_Turn
    MovementSystem sys{cfg};
    PassabilityGrid g;
    g.init(100, 100);
    const float hi = static_cast<float>(g.width() - 1) * kCellSize;
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float, float) { return 0.0f; });
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
    if (pre)  // only the begin clips exist: 10 frames at 30 fps
        sys.set_deploy_clip_fn([](ObjectId, int32_t anim, uint32_t& n, float& fps) {
            if (anim != kAnimTurnLeftBegin && anim != kAnimTurnRightBegin) return false;
            n = 10;
            fps = 30.0f;
            return true;
        });
    UnitDesc d = tank(1, {400.0f, 400.0f});
    d.yaw_deg = yaw0;
    d.max_speed = d.alt_max_speed = 2.0f;
    d.turn_rate = d.alt_turn_rate = rate;
    d.stopped_turn_rate = stopped;
    d.close_enough_angle = ce;
    d.has_pre_turn_anim = pre;
    d.min_speed_fraction_for_turn = f_min;  // s78n t26.3
    d.collision_class = kUccVehicle;
    CHECK(sys.add_unit(d));
    SimRng rng(2602);
    order(sys, rng, {1}, to, 1);
    const V2 p0 = sys.view(1).pos;
    for (uint32_t f = 1; f < 400; ++f) {
        const float y0 = sys.view(1).yaw;
        sys.tick(f, rng);
        for (const auto& ev : sys.take_turn_clip_events())
            if (ev.anim == kAnimTurnLeftBegin || ev.anim == kAnimTurnRightBegin) ++o.begin_plays;
        const UnitView v = sys.view(1);
        if (v.pos != p0) {
            o.first_step = o.turn_ticks + o.clip_hold + 1;
            o.exact = y0 == 0.0f;  // the facing on the leg exactly when it starts to move
            o.end_yaw = v.yaw;
            break;
        }
        if (v.arrival_turn && v.yaw == y0 && v.speed == 0.0f) ++o.clip_hold;  // the begin clip holds the yaw
        if (v.yaw != y0) {
            ++o.turn_ticks;
            o.yaws.push_back(v.yaw);
        }
    }
    return o;
}

void test_t26_start_turn() {
    ::alamo::test::section("t26_start_turn");
    // delta 90, R 10, CE 30, no stopped rate: +10 a tick (the rate S = R), exactly on the leg after 9 turn ticks with no
    // rate left (not reached); the 10th turn frame is reached and moves in the same tick (hand)
    const StartTurnOut a = start_turn_case(-90.0f, 10.0f, 0.0f, 30.0f, false);
    CHECK(a.turn_ticks == 9 && a.first_step == 10 && a.exact && !a.yaws.empty() && a.yaws.front() == -80.0f &&
          a.yaws.back() == 0.0f);
    // AT-AT-like: Stopped_Rate_Of_Turn 1 (raw, at speed 0) against R 3.12, CE 30, delta 40: 40 turn ticks at 1 degree, the
    // step on tick 41 (hand)
    const StartTurnOut b = start_turn_case(-40.0f, 3.12f, 1.0f, 30.0f, false);
    CHECK(b.turn_ticks == 40 && b.first_step == 41 && b.exact);
    // within CE (delta 20 <= 30, fraction 1): no turn in place
    const StartTurnOut c = start_turn_case(-20.0f, 3.12f, 1.0f, 30.0f, false);
    CHECK(c.turn_ticks == 0 && c.first_step == 1);
    // Has_Pre_Turn_Anim with only the begin clips (10 frames at speed 1: the hold ends at clip frame 8, its last 2): the
    // yaw holds 8 ticks, then 0x10 with the bands of 0x6204f0 (T = 10 on an ordinary path, S = 10): 10 a tick while
    // e >= 45 (5 ticks: 80 .. 40), the half rate 5 (40 -> 20), the quarter rate S x 0.25 x e / 11.25 (20 -> 15.56 ->
    // 12.10 -> 9.41), and at e < T the clip is "unchanged" - the begin clip, played again; the 13th turn frame lands on the
    // leg with rate left over (reached) and moves in the same tick (hand): 12 turn ticks before it, 2 begin plays
    const StartTurnOut d = start_turn_case(-90.0f, 10.0f, 0.0f, 30.0f, true);
    CHECK(d.begin_plays == 2 && d.clip_hold == 8 && d.turn_ticks == 12 && d.first_step == 21 && !d.exact && d.end_yaw == 0.0f);
    // s78n t26.3: the corner pass on the order path. Facing 90 off a 20-long leg (goal (420, 400)), R 6, vmax 2, CE 180:
    // corner 0 v = min(2, w 20 = 2.094395, 20 w / 2 = 1.047198) -> f 0.523599 (hand). Min_Speed_Fraction_For_Turn 0.5 keeps
    // it and start rule A moves at once (within CE, fraction > 0); 0.55 writes 0 and the unit turns in place first: 15
    // turn ticks of 6 degrees (exact landing, no rate left), the step on tick 16
    const StartTurnOut e = start_turn_case(-90.0f, 6.0f, 0.0f, 180.0f, false, 0.5f, {420.0f, 400.0f});
    CHECK(e.turn_ticks == 0 && e.first_step == 1);
    const StartTurnOut g = start_turn_case(-90.0f, 6.0f, 0.0f, 180.0f, false, 0.55f, {420.0f, 400.0f});
    CHECK(g.turn_ticks == 15 && g.first_step == 16 && g.exact);
    std::printf("t26.3 corner pass on the order path: f_min 0.5 -> %u turn ticks, step on %u; 0.55 -> %u turn ticks, step on %u\n",
                e.turn_ticks, e.first_step, g.turn_ticks, g.first_step);
    std::printf("t26.2 start turn: delta 90 R 10 -> %u turn ticks, step on %u; AT-AT-like delta 40 -> %u ticks at 1, step on %u; "
                "within CE -> %u; begin clip: %u play, yaw held %u ticks, %u turn ticks\n",
                a.turn_ticks, a.first_step, b.turn_ticks, b.first_step, c.turn_ticks, d.begin_plays, d.clip_hold, d.turn_ticks);
}

// ---------------------------------------------------------------- s78n t26.4: the Walk move step C (pure walk_step)
// R 10, v held at vmax 2 (acc = a = 1), path (0, 0), (10, 0), (10, 10), start (4, 0), yaw 30, state 0x13, lane term off.
// The unit turns first and travels along its facing up to the waypoint line (x + y = 10, the bisector normal), s' = D /
// tau (MOVEMENT 3.5.2 C.6 - C.11); on tick 3 it reaches yaw 0 exactly, crosses line 1 with s - s' left, and the facing
// at right angles to segment 2 (tau = 0) skips waypoint 2 (C.9 quirk), so nothing is left to walk: End Move and the
// coast of the rest along the last frame's displacement (C.4).
void test_t26_walk_step() {
    ::alamo::test::section("t26_walk_step");
    LocomotorParams lp;
    lp.max_speed = 2.0f;
    lp.accel = lp.decel = 1.0f;
    lp.turn_rate = 10.0f;
    LandMotion m;
    m.pos = {4.0f, 0.0f};
    m.yaw = 30.0f;
    m.speed = 2.0f;
    m.state = LocoState::WalkMoving;
    m.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};
    m.next = 1;
    std::vector<V2> pos{m.pos};
    WalkResult r1 = walk_step(m, lp, 3.0e38f, nullptr);
    pos.push_back(m.pos);
    // tick 1: yaw 20; D = 6, tau = cos 20, s' = 6.385; pos = (4, 0) + 2 (cos 20, sin 20) (hand)
    CHECK(m.yaw == 20.0f && r1.moved && !r1.coast && m.state == LocoState::WalkMoving && m.next == 1);
    CHECK(near(m.pos.x, 5.879385f, 2e-5f) && near(m.pos.y, 0.684040f, 2e-5f));
    m.last_dir = pos[1] - pos[0];
    walk_step(m, lp, 3.0e38f, nullptr);
    pos.push_back(m.pos);
    // tick 2: yaw 10; s' = 3.489589 (hand)
    CHECK(m.yaw == 10.0f && near(m.pos.x, 7.849001f, 3e-5f) && near(m.pos.y, 1.031337f, 3e-5f) && m.next == 1);
    m.last_dir = pos[2] - pos[1];
    const WalkResult r3 = walk_step(m, lp, 3.0e38f, nullptr);
    // tick 3: yaw exactly 0 (the step that covers the rest lands exactly, no leftover: turn-done stays clear); line 1 is
    // crossed at x 8.968663 with 0.880337 left; tau = 0 on segment 2 skips waypoint 2; the coast: 0.880337 along the last
    // displacement unit(pos2 - pos1)
    const V2 cross_pt{10.0f - pos[2].y, pos[2].y};  // the line x + y = 10 at y = pos2.y (t = (1, 0))
    const float left = 2.0f - (cross_pt.x - pos[2].x);
    const V2 cd = normalize_retail(pos[2] - pos[1]);
    const V2 want = cross_pt + cd * left;
    std::printf("t26.4 walk step: tick 3 crosses line 1 at (%.6f, %.6f), %.6f left, coast to (%.6f, %.6f); pos (%.6f, %.6f)\n",
                static_cast<double>(cross_pt.x), static_cast<double>(cross_pt.y), static_cast<double>(left),
                static_cast<double>(want.x), static_cast<double>(want.y), static_cast<double>(m.pos.x), static_cast<double>(m.pos.y));
    CHECK(m.yaw == 0.0f && !m.turn_done && r3.tau_skips == 1 && r3.coast && m.state == LocoState::WalkEndMove && m.next == 3);
    CHECK(near(cross_pt.x, 8.968663f, 3e-5f) && near(left, 0.880337f, 3e-5f));
    CHECK(near(m.pos.x, want.x, 2e-5f) && near(m.pos.y, want.y, 2e-5f));

    // The line rule (C.5): 0.0005 short of a waypoint line along its normal -> passed; 0.002 short -> not (a step of 0.0001,
    // the cap taken at once).
    const auto line_case = [&](float short_by) {
        LandMotion k;
        k.pos = {10.0f - short_by, 0.0f};
        k.speed = 1.0f;
        k.state = LocoState::WalkMoving;
        k.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}};
        k.next = 1;
        WalkExtras x;
        x.cap_now = true;
        walk_step(k, lp, 0.0001f, nullptr, &x);
        return k.next;
    };
    CHECK(line_case(0.0005f) == 2 && line_case(0.002f) == 1);

    // A waypoint is passed by its line, never by its point: a unit beside its segment (y 3) steps past waypoint 1 where it
    // crosses the line x = 10 (straight path, normal (1, 0)) and walks on toward waypoint 2 without turning back.
    {
        LandMotion k;
        k.pos = {9.0f, 3.0f};
        k.speed = 2.0f;
        k.state = LocoState::WalkMoving;
        k.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {20.0f, 0.0f}};
        k.next = 1;
        walk_step(k, lp, 3.0e38f, nullptr);
        CHECK(k.next == 2 && k.yaw == 0.0f && near(k.pos.x, 11.0f, 1e-4f) && k.pos.y == 3.0f);
    }

    // The point test of the new position (C.10, MOVEMENT 5.4.1 "The impassable-cell rule" item 5): a step from cell (0, 0)
    // that ends in the passable cell (1, 1) is accepted although the straight line crosses the impassable cell (0, 1);
    // a step that ends in an impassable cell is refused with no commit.
    {
        PassabilityGrid g;
        g.init(4, 4);
        g.set_base(0, 1, kPassImpassable);
        LandMotion k;
        k.pos = {9.0f, 9.5f};
        k.yaw = 45.0f;
        k.speed = 2.0f;
        k.state = LocoState::WalkMoving;
        k.path = {{9.0f, 9.5f}, {30.0f, 30.5f}};
        k.next = 1;
        const WalkResult ok = walk_step(k, lp, 3.0e38f, &g);
        CHECK(!ok.blocked && ok.moved && world_to_cell(k.pos.x) == 1 && world_to_cell(k.pos.y) == 1);
        g.set_base(1, 1, kPassImpassable);
        LandMotion b = k;
        b.pos = {9.0f, 9.5f};
        const WalkResult no = walk_step(b, lp, 3.0e38f, &g);
        CHECK(no.blocked && !no.moved && b.pos == (V2{9.0f, 9.5f}));
    }

    // The pop (C.4): a finished top record over a started path below (not in End Move) is popped in the step and the unit
    // goes on along the path below with no more turning in that call; in End Move it coasts instead.
    {
        LandMotion k;
        k.pos = {10.0f, 0.0f};
        k.yaw = 0.0f;
        k.speed = 2.0f;
        k.state = LocoState::WalkMoving;
        k.path = {{0.0f, 0.0f}, {10.0f, 0.0f}};
        k.next = 1;
        LandMotion q;
        q.path = {{0.0f, 0.0f}, {100.0f, 0.0f}};
        q.next = 1;
        WalkExtras x;
        x.below = &q;
        LandMotion e = k;
        LandMotion qe = q;
        const WalkResult pr = walk_step(k, lp, 3.0e38f, nullptr, &x);
        CHECK(pr.popped && k.path[1] == (V2{100.0f, 0.0f}) && q.path[1] == (V2{10.0f, 0.0f}) && k.yaw == 0.0f && near(k.pos.x, 12.0f, 1e-4f));
        e.state = LocoState::WalkEndMove;
        e.speed = 1.5f;
        WalkExtras xe;
        xe.below = &qe;
        const WalkResult er = walk_step(e, lp, 3.0e38f, nullptr, &xe);
        CHECK(!er.popped && er.coast && e.path[1] == (V2{10.0f, 0.0f}));
    }
}

// The End Move end (0x15, MOVEMENT 3.5.1 "Arrival turn, details" item 1): at v <= a the unit stops; within 5 before the
// last line (or past it) it has arrived where it stands, more than 5 short the move restarts from state 0.
void test_t26_end_move() {
    ::alamo::test::section("t26_end_move");
    LocomotorParams lp;
    lp.max_speed = 2.0f;
    lp.accel = lp.decel = 1.0f;
    lp.turn_rate = 10.0f;
    const auto end_case = [&](float x0) {
        LandMotion k;
        k.pos = {x0, 0.0f};
        k.speed = 1.0f;
        k.state = LocoState::WalkEndMove;
        k.path = {{0.0f, 0.0f}, {50.0f, 0.0f}};
        k.next = 1;
        const WalkResult r = walk_step(k, lp, 3.0e38f, nullptr);
        return std::pair<WalkResult, LandMotion>(r, k);
    };
    const auto [ra, ka] = end_case(46.0f);  // 4 short: arrived, the index stepped on, no move
    CHECK(ra.arrived && !ra.moved && ka.next == 2 && ka.speed == 0.0f && ka.state == LocoState::WalkStopped && ka.pos.x == 46.0f);
    const auto [rb, kb] = end_case(44.0f);  // 6 short: restart (start rule, Start Move, the move step from speed 0)
    CHECK(!rb.arrived && rb.restart && kb.next == 1 && kb.state == LocoState::WalkMoving && kb.speed == 1.0f);

    // No snap (system): a vehicle ordered 50 ahead along +x ends where 0x15 finds it - past the last line or within 5 before
    // it - never exactly on the goal point
    MovementConfig cfg;
    cfg.constants.land_speed_multiplier = 1.0f;
    MovementSystem sys{cfg};
    land_system(sys);
    UnitDesc d = tank(1, cell_center(20, 50));
    d.accel = d.decel = 0.3f;
    CHECK(sys.add_unit(d));
    SimRng rng(2604);
    const V2 goal = cell_center(20, 50) + V2{50.0f, 0.0f};
    order(sys, rng, {1}, goal, 1);
    uint32_t f = 1;
    float vmax_seen = 0.0f;
    for (; f < 400 && !sys.view(1).arrived; ++f) {
        sys.tick(f, rng);
        vmax_seen = std::max(vmax_seen, sys.view(1).speed);
    }
    const UnitView v = sys.view(1);
    std::printf("t26.4 no snap: arrived on tick %u at x %.4f (goal %.4f), max speed %.3f\n", f, static_cast<double>(v.pos.x),
                static_cast<double>(goal.x), static_cast<double>(vmax_seen));
    CHECK(v.arrived && v.speed == 0.0f && !(v.pos == goal) && v.pos.x > goal.x - 5.0f);

    // A new order behind a moving AT-AT-like unit (Close_Enough_Angle_For_Move_Start 30): not aligned (3.5.2 G), so the
    // path is not started - the unit coasts in End Move at T = a (its speed falls by a each tick, it moves along its last
    // displacement), stops at 0x15 and then starts the new path (the turn in place, start rule A).
    MovementSystem s2{cfg};
    land_system(s2);
    // the host's order of a tick (MAIN_LOOP 1.2, tactical_battle): the movement tick, then the coordinator update (step 10);
    // an AI order given after it composes at the next tick's update and is planned at the tick after that
    s2.set_deferred_composition(true);
    const auto step2 = [&](uint32_t f) {
        s2.tick(f, rng);
        s2.coordinator_update(f, f, rng);
    };
    UnitDesc w = tank(2, cell_center(20, 50));
    w.accel = w.decel = 0.3f;
    w.close_enough_angle = 30.0f;
    CHECK(s2.add_unit(w));
    order(s2, rng, {2}, cell_center(20, 50) + V2{400.0f, 0.0f}, 1);
    uint32_t g = 1;
    for (; g < 300 && s2.view(2).speed < 3.0f; ++g) step2(g);  // (the order's composition delay first)
    const float v0 = s2.view(2).speed;
    CHECK(v0 >= 3.0f);
    const V2 back_goal = cell_center(20, 50) - V2{200.0f, 0.0f};
    order(s2, rng, {2}, back_goal, g);
    // Until the new order is planned (its composition at the next coordinator update, §4.6.2) the old order path stays on
    // top of the stack (the stack reset 0x554480 runs with the path set, §3.5.2 G): the unit walks on along it in 0x13 -
    // no End Move, its speed does not fall, it keeps moving away from the new goal.
    const auto planned = [&] {
        const std::span<const V2> lp2 = s2.land_path(2);
        return !lp2.empty() && distance(lp2.back(), back_goal) < 40.0f;
    };
    uint32_t gap = 0;
    bool gap_ok = true;
    float gv = v0, gx = s2.view(2).pos.x;
    std::vector<float> speeds;  // from the tick of the plan on
    std::vector<float> xs;
    float v_plan = 0.0f;  // the speed before the tick of the plan
    for (uint32_t end = g + 10; g < end; ++g) {
        step2(g);
        const UnitView gw = s2.view(2);
        if (planned()) {
            v_plan = gv;
            speeds.push_back(gw.speed);
            xs.push_back(gw.pos.x);
            ++g;
            break;
        }
        gap_ok = gap_ok && gw.walk_state == LocoState::WalkMoving && gw.speed >= gv && gw.pos.x > gx;
        ++gap;
        gv = gw.speed;
        gx = gw.pos.x;
    }
    std::printf("t26.4 new order behind: %u tick(s) on the old path before the plan (speed %.3f)\n", gap, static_cast<double>(v_plan));
    CHECK(gap >= 1 && gap_ok && !speeds.empty());
    bool started_turn = !speeds.empty() && speeds.back() == 0.0f;
    for (uint32_t end = g + 40; g < end && !started_turn; ++g) {
        step2(g);
        speeds.push_back(s2.view(2).speed);
        xs.push_back(s2.view(2).pos.x);
        if (s2.view(2).speed == 0.0f) started_turn = true;
    }
    bool falls = speeds.size() >= 2;
    for (size_t i = 0; i + 1 < speeds.size(); ++i) {
        if (speeds[i + 1] == 0.0f) break;
        falls = falls && near(speeds[i] - speeds[i + 1], 0.3f, 1e-4f) && xs[i + 1] > xs[i];
    }
    std::printf("t26.4 new order behind: speed %.3f -> coast %zu ticks (first %.3f)\n", static_cast<double>(v_plan), speeds.size(),
                static_cast<double>(speeds.empty() ? 0.0f : speeds[0]));
    CHECK(falls && started_turn && near(speeds[0], v_plan - 0.3f, 1e-4f));
    bool turned = false;
    for (uint32_t end = g + 80; g < end; ++g) {
        step2(g);
        if (s2.view(2).pos.x < xs.back() - 1.0f) turned = true;
    }
    CHECK(turned);

    // A re-plan of a moving vehicle with CloseEnoughAngleForMoveStart 180 (a chase re-issuing its goal, 3.5.2 G): the new
    // order waits for its composition on the old path, then the new path is aligned and keeps 0x13 - the speed never
    // falls and the unit is never in End Move across the order (the brake-and-restart of each re-plan is gone).
    MovementSystem s3{cfg};
    land_system(s3);
    s3.set_deferred_composition(true);
    const auto step3 = [&](uint32_t f) {
        s3.tick(f, rng);
        s3.coordinator_update(f, f, rng);
    };
    UnitDesc c = tank(3, cell_center(20, 50));
    c.accel = c.decel = 0.3f;
    CHECK(s3.add_unit(c));
    order(s3, rng, {3}, cell_center(20, 50) + V2{600.0f, 0.0f}, 1);
    uint32_t h = 1;
    for (; h < 300 && s3.view(3).speed < 3.6f; ++h) step3(h);
    CHECK(near(s3.view(3).speed, 3.6f, 1e-4f));
    bool kept = true;
    for (int k = 0; k < 3; ++k) {  // three re-plans 20 ticks apart, each a little farther along +x
        order(s3, rng, {3}, cell_center(20, 50) + V2{620.0f + 20.0f * static_cast<float>(k), 30.0f}, h);
        for (uint32_t end = h + 20; h < end; ++h) {
            step3(h);
            kept = kept && s3.view(3).walk_state == LocoState::WalkMoving && near(s3.view(3).speed, 3.6f, 1e-4f);
        }
    }
    std::printf("t26.4 re-plans while moving: speed %.3f state 0x%x\n", static_cast<double>(s3.view(3).speed),
                static_cast<unsigned>(s3.view(3).walk_state));
    CHECK(kept);
}

// s78n t26.5: the land team container's move step 0x5adfb0 (MOVEMENT 3.5.2 E, FIGHTER_FLYING 7.7 item 7, 7.2 caller 3): C with
// the budget R and no f scaling, the coast and the travel along the facing, the re-lay at each waypoint stepped; and the
// new order while moving (3.5.2 G, FIGHTER_FLYING 7.7 item 5): the old path until the plan, then the coast at the rate.
void test_t26_container_step() {
    ::alamo::test::section("t26_container_step");
    LocomotorParams lp;
    lp.max_speed = 2.0f;
    lp.accel = lp.decel = 1.0f;
    lp.turn_rate = 10.0f;
    // t26.4's hand case for a container (rate 10, v held at 2, 0x13): the same turn and travel - ticks 1 and 2 as the Walk's
    // (yaw 20 / 10, s' 6.385 / 3.489589); tick 3 crosses line 1 at x 8.968663 with 0.880337 left, tau = 0 skips waypoint 2,
    // and the coast runs along the facing (yaw 0: (1, 0)), not along the last displacement (hand)
    {
        LandMotion m;
        m.pos = {4.0f, 0.0f};
        m.yaw = 30.0f;
        m.speed = 2.0f;
        m.state = LocoState::WalkMoving;
        m.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};
        m.next = 1;
        std::vector<V2> relay;
        WalkExtras x;
        x.container = true;
        x.relay = &relay;
        walk_step(m, lp, 3.0e38f, nullptr, &x);
        CHECK(m.yaw == 20.0f && near(m.pos.x, 5.879385f, 2e-5f) && near(m.pos.y, 0.684040f, 2e-5f) && m.next == 1);
        walk_step(m, lp, 3.0e38f, nullptr, &x);
        CHECK(m.yaw == 10.0f && near(m.pos.x, 7.849001f, 3e-5f) && near(m.pos.y, 1.031337f, 3e-5f) && m.next == 1);
        const float y2 = m.pos.y;
        const WalkResult r3 = walk_step(m, lp, 3.0e38f, nullptr, &x);
        std::printf("t26.5 container step: tick 3 at (%.6f, %.6f), yaw %.3f, next %u, relays %zu\n", static_cast<double>(m.pos.x),
                    static_cast<double>(m.pos.y), static_cast<double>(m.yaw), m.next, relay.size());
        CHECK(m.yaw == 0.0f && r3.tau_skips == 1 && r3.coast && m.state == LocoState::WalkEndMove && m.next == 3);
        CHECK(near(m.pos.x, 8.968663f + 0.880337f, 4e-5f) && m.pos.y == y2);
        // line 1 was reached in C.11 (s > s': idx + 1) - the re-lay along segment 2; waypoint 2 by the tau = 0 skip: none
        CHECK(relay.size() == 1 && relay[0] == (V2{0.0f, 10.0f}));
    }
    // The turn budget in End Move: v 1.5 of vmax 3 (a 0.5, from 2.0) - a Walk turns R x f = 10 x 0.5 = 5 deg, a container the
    // full R = 10 deg; on a crush-flee path its R is x Fleeing_Infantry_Speed_Bonus (2): 20 deg (hand)
    {
        LocomotorParams p3;
        p3.max_speed = 3.0f;
        p3.accel = p3.decel = 0.5f;
        p3.turn_rate = 10.0f;
        const auto turn = [&](bool team, float rate) {
            LandMotion k;
            k.pos = {10.0f, 0.0f};
            k.yaw = 90.0f;
            k.speed = 2.0f;
            k.state = LocoState::WalkEndMove;
            k.path = {{0.0f, 0.0f}, {200.0f, 0.0f}};
            k.next = 1;
            WalkExtras x;
            x.container = team;
            LocomotorParams q = p3;
            q.turn_rate = rate;
            walk_step(k, q, 3.0e38f, nullptr, &x);
            return std::pair<float, float>(k.yaw, k.speed);
        };
        const float flee = MovementConstants{}.fleeing_infantry_speed_bonus;
        const auto w = turn(false, 10.0f), c = turn(true, 10.0f), fl = turn(true, 10.0f * flee);
        std::printf("t26.5 End Move turn: Walk %.4f, container %.4f, crush-flee %.4f (bonus %.2f, speed %.3f)\n",
                    static_cast<double>(w.first), static_cast<double>(c.first), static_cast<double>(fl.first),
                    static_cast<double>(flee), static_cast<double>(c.second));
        CHECK(w.second == 1.5f && c.second == 1.5f && flee == 2.0f);
        CHECK(w.first == 85.0f && c.first == 80.0f && fl.first == 70.0f);
    }
    // The re-lay (FIGHTER_FLYING 7.2 caller 3): a waypoint stepped in C.11 (s > s') re-lays along the new segment; a pop into
    // the started path below re-lays along its segment
    {
        LandMotion k;
        k.pos = {9.0f, 0.0f};
        k.speed = 2.0f;
        k.state = LocoState::WalkMoving;
        k.path = {{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 10.0f}};
        k.next = 1;
        std::vector<V2> relay;
        WalkExtras x;
        x.container = true;
        x.relay = &relay;
        walk_step(k, lp, 3.0e38f, nullptr, &x);
        CHECK(relay.size() == 1 && relay[0] == (V2{0.0f, 10.0f}) && k.next == 2 && k.yaw == 10.0f);
        LandMotion t;
        t.pos = {10.0f, 0.0f};
        t.speed = 2.0f;
        t.state = LocoState::WalkMoving;
        t.path = {{0.0f, 0.0f}, {10.0f, 0.0f}};
        t.next = 1;
        LandMotion q;
        q.path = {{0.0f, 0.0f}, {100.0f, 0.0f}};
        q.next = 1;
        std::vector<V2> r2;
        WalkExtras xp;
        xp.container = true;
        xp.relay = &r2;
        xp.below = &q;
        const WalkResult pr = walk_step(t, lp, 3.0e38f, nullptr, &xp);
        CHECK(pr.popped && r2.size() == 1 && r2[0] == (V2{100.0f, 0.0f}) && near(t.pos.x, 12.0f, 1e-4f));
    }
    // System: a new order behind a marching squad (the host's tick order, the AI's composition one update later). Until the
    // plan the old path stays on top (MOVEMENT 3.5.2 G): the container walks on along it in 0x13 at its team speed. From the
    // path set (not started, FIGHTER_FLYING 7.7 item 5) it brakes at its rate a = 0.1 in End Move, straight along its facing
    // (team_coast), stops at 0x15 with no step, turns in place from speed 0 and walks the new path to its goal.
    MovementConfig cfg;
    cfg.constants.land_speed_multiplier = 1.0f;
    MovementSystem sys{cfg};
    land_system(sys);
    sys.set_deferred_composition(true);
    const V2 at = cell_center(20, 50);
    UnitDesc cd = tank(100, at);
    cd.type_name = "T26_TEAM";
    cd.team_container = true;
    cd.max_speed = 2.0f;
    cd.turn_rate = 10.0f;
    CHECK(sys.add_unit(cd));
    const V2 rel[4] = {{8.0f, 8.0f}, {8.0f, -8.0f}, {-8.0f, 8.0f}, {-8.0f, -8.0f}};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (ObjectId id = 1; id <= 4; ++id) {
        UnitDesc d = tank(id, at + rel[id - 1]);
        d.type_name = "T26_TROOPER";
        d.max_speed = 2.0f;
        d.accel = d.decel = 0.1f;
        d.radius = 3.0f;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(id);
        slots.push_back(rel[id - 1]);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    CHECK(sys.set_team(100, 100, 1.0f, 2.0f));
    for (ObjectId id : ids) CHECK(sys.set_team(id, 100, 1.0f, 2.0f));
    SimRng rng(2605);
    const auto step = [&](uint32_t f) {
        sys.tick(f, rng);
        sys.coordinator_update(f, f, rng);
    };
    order(sys, rng, {1}, at + V2{600.0f, 0.0f}, 1);
    uint32_t g = 1;
    // (the straggler cap 0x5ab640 holds it a hair under the team speed 2: the members walk at 2 too)
    for (; g < 400 && !(sys.view(100).speed > 1.99f && sys.view(100).pos.x > at.x + 150.0f); ++g) step(g);
    const float v0 = sys.view(100).speed;
    CHECK(v0 > 1.99f && v0 <= 2.0f);
    const V2 back = at - V2{100.0f, 0.0f};
    order(sys, rng, {1}, back, g);
    const auto planned = [&] {
        const std::span<const V2> lp2 = sys.land_path(100);
        return !lp2.empty() && distance(lp2.back(), back) < 40.0f;
    };
    uint32_t gap = 0;
    bool gap_ok = true;
    float gx = sys.view(100).pos.x;
    for (uint32_t end = g + 10; g < end; ++g) {
        step(g);
        if (planned()) break;
        const UnitView k = sys.view(100);
        gap_ok = gap_ok && k.walk_state == LocoState::WalkMoving && near(k.speed, v0, 1e-4f) && k.pos.x > gx && !k.team_coast;
        gx = k.pos.x;
        ++gap;
    }
    CHECK(planned() && gap >= 1 && gap_ok);
    // the tick of the plan: the coast's first step (v - a; a braking container does not wait for stragglers)
    const UnitView k0 = sys.view(100);
    CHECK(k0.team_coast && near(k0.speed, v0 - 0.1f, 1e-4f) && k0.walk_state == LocoState::WalkEndMove);
    const float yaw0 = k0.yaw, y0 = k0.pos.y;
    float px = k0.pos.x, pv = k0.speed;
    bool falls = true;
    uint32_t coast = 1;
    ++g;
    for (uint32_t end = g + 40; g < end; ++g) {
        step(g);
        const UnitView k = sys.view(100);
        if (k.speed == 0.0f) {
            ++g;
            break;
        }
        // B.8 with T = a: v -= min(a, v - a) (the last step down to a itself)
        falls = falls && near(pv - k.speed, std::min(0.1f, pv - 0.1f), 1e-4f) && k.yaw == yaw0 && k.pos.y == y0 && k.pos.x > px && k.team_coast;
        px = k.pos.x;
        pv = k.speed;
        ++coast;
    }
    const UnitView ks = sys.view(100);
    std::printf("t26.5 new order behind a squad: %u tick(s) on the old path, coast %u ticks, stopped at x %.3f\n", gap, coast,
                static_cast<double>(ks.pos.x));
    CHECK(falls && ks.speed == 0.0f && ks.pos.x == px && !ks.team_coast && coast >= 18);
    bool turned = false, arrived = false;
    for (uint32_t end = g + 600; g < end && !arrived; ++g) {
        step(g);
        const UnitView k = sys.view(100);
        turned = turned || (k.speed == 0.0f && k.yaw != yaw0);
        arrived = k.arrived;
    }
    CHECK(turned && arrived && distance(sys.view(100).pos, back) < 5.0f);
}

// Save inside End Move with a coast: the loaded copy hashes equal and goes on identically.
void test_t26_coast_save() {
    ::alamo::test::section("t26_coast_save");
    MovementConfig cfg;
    cfg.constants.land_speed_multiplier = 1.0f;
    MovementSystem sys{cfg};
    land_system(sys);
    UnitDesc d = tank(1, cell_center(20, 50));
    d.accel = d.decel = 0.2f;
    CHECK(sys.add_unit(d));
    SimRng rng(2605);
    order(sys, rng, {1}, cell_center(20, 50) + V2{400.0f, 0.0f}, 1);
    uint32_t f = 1;
    for (; f < 300 && sys.view(1).speed < 3.0f; ++f) sys.tick(f, rng);
    const ObjectId one[] = {1};
    sys.halt(one, f);  // stop with the speed kept: the walk coasts in End Move
    sys.tick(f++, rng);
    CHECK(sys.view(1).speed > 0.5f && sys.view(1).walk_state == LocoState::WalkEndMove);
    ByteWriter w;
    sys.write(w);
    MovementSystem copy{cfg};
    land_system(copy);
    {
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done());
    }
    StateHasher ha, hb;
    sys.hash_units(ha);
    copy.hash_units(hb);
    CHECK(ha.digest() == hb.digest());
    SimRng r2 = rng;
    const V2 p0 = sys.view(1).pos;
    for (uint32_t g = f; g < f + 30; ++g) {
        sys.tick(g, rng);
        copy.tick(g, r2);
    }
    StateHasher hc, hd;
    sys.hash_units(hc);
    copy.hash_units(hd);
    CHECK(hc.digest() == hd.digest() && sys.view(1).pos == copy.view(1).pos);
    CHECK(sys.view(1).speed == 0.0f && sys.view(1).pos.x > p0.x && sys.view(1).pos.y == p0.y);
    CHECK(sys.stats().walk_coasts > 0);
}

// s78n t26 closing review (FIGHTER_FLYING §7.5 "Corrections and confirmations (s78n t18 RE)" item 4 [H], MOVEMENT §5.3.1
// overlap coefficient): the side step 0x5c71a0 of a against b in a's formation takes t = clamp(the footprint overlap
// coefficient, 0, 1) instead of the distance ramp 1 - d / (1.3 (r_a + r_b)). Radius-6 round footprints (r_a + r_b = 12,
// reach 15.6), heading north (left = (-1, 0)): b 13 to the right - ramp 1 - 13 / 15.6 = 0.166667, footprints apart
// (13 > 12) -> 0 for a formation mate; b 10 to the left - ramp 0.358974, overlap 1 - 10 / 12 = 0.166667 (the step to
// the right, -t left).
void test_t26_formation_side_step() {
    ::alamo::test::section("t26_formation_side_step");
    MovementSystem sys{MovementConfig{}};
    land_system(sys);
    const V2 p = cell_center(50, 50);
    CHECK(sys.add_unit(tank(1, p)));
    CHECK(sys.add_unit(tank(2, p + V2{13.0f, 0.0f})));
    CHECK(sys.add_unit(tank(3, p + V2{-10.0f, 0.0f})));
    const V2 north{0.0f, 1.0f};
    // no formation yet (form_id 0): the distance ramp
    V2 v12 = sys.pair_side_step(1, 2, north), v13 = sys.pair_side_step(1, 3, north);
    CHECK(near(v12.x, -(1.0f - 13.0f / 15.6f), 1e-4f) && v12.y == 0.0f);
    CHECK(near(v13.x, 1.0f - 10.0f / 15.6f, 1e-4f) && v13.y == 0.0f);
    SimRng rng(2611);
    order(sys, rng, {1, 2, 3}, p + V2{0.0f, 600.0f}, 1);  // one order formation; nobody has moved yet
    CHECK(sys.view(1).form_id != 0 && sys.view(1).form_id == sys.view(2).form_id && sys.view(1).form_id == sys.view(3).form_id);
    v12 = sys.pair_side_step(1, 2, north);
    v13 = sys.pair_side_step(1, 3, north);
    CHECK(v12 == V2{});  // within 1.3 (r_a + r_b), footprints apart: no side step
    CHECK(near(v13.x, 1.0f - 10.0f / 12.0f, 1e-3f) && near(v13.y, 0.0f, 1e-6f));
    std::printf("t26 formation side step: mate 13 right %.6f, mate 10 left %.6f\n", static_cast<double>(v12.x),
                static_cast<double>(v13.x));
}

// s78n t26 closing review (MOVEMENT §3.4 "Units with Walk_Transition" [H], §3.5.2 A / B.7 / C.2 / F [H]): an AT-ST
// (Max_Speed 1.2 x land 1.56 -> V = 1.872; S = Scale_Factor 1.6 x Movement_Animation_Speed 0.8117 = 1.29872, V / S =
// 1.441419) with synthetic clips MOVE 30, MOVESTART 16, MOVE_ENDONE 17 frames at 30 fps (step 1.0 x the rate). Hand
// values: a = (S / 16) (V / S)^2 = V^2 / (16 S) = 0.168646; the braking u = max(0.75 V, v0) = V at the latch -> the same
// value; v0 = 1.0 -> u = 1.404, ((S / 16) (u / S)) (u / S) = 0.094863; no MOVESTART -> V; no MOVE_ENDONE -> u.
WalkTransition atst_clips() {
    WalkTransition wt;
    wt.on = wt.states = true;
    wt.s = 1.6f * 0.8117f;
    wt.count[0] = 30.0f;  // MOVE
    wt.count[1] = 16.0f;  // MOVESTART
    wt.count[2] = 17.0f;  // MOVE_ENDONE
    for (float& st : wt.step) st = 1.0f;
    return wt;
}

void test_t26_walk_transition() {
    ::alamo::test::section("t26_walk_transition");
    const float vmax = 1.2f * 1.56f;
    const WalkTransition wt = atst_clips();
    const float acc = walk_transition_accel(wt, vmax);
    CHECK(near(acc, 0.168646f, 2e-6f));
    CHECK(near(walk_transition_decel(wt, vmax, vmax, 0.0f), 0.168646f, 2e-6f));  // v0 = v = V
    CHECK(near(walk_transition_decel(wt, vmax, 1.0f, 0.0f), 0.094863f, 2e-6f));  // u = 0.75 V
    CHECK(near(walk_transition_decel(wt, vmax, 0.3f, vmax), 0.168646f, 2e-6f));  // the latch +0xf0 wins over +0xec
    WalkTransition none = wt;
    none.count[1] = none.count[2] = 0.0f;
    CHECK(walk_transition_accel(none, vmax) == vmax && walk_transition_decel(none, vmax, 1.0f, 0.0f) == 0.75f * vmax);
    LocomotorParams p;
    p.max_speed = vmax;
    p.turn_rate = 5.0f * 1.56f;
    p.close_enough_angle = 180.0f;
    WalkExtras x;
    x.wt = &wt;
    x.vmax_u = vmax;
    // Acceleration (F 0x12): MOVESTART plays from frame 0 at V / S = 1.441419 per frame, speed 0 on its first frame and
    // the move step in 0x12 (B.8 v += a); the clip has ended once its frame >= 16 - 2 - 1.441419 = 12.558581: frame
    // 9 x 1.441419 = 12.972774 at the 10th call -> 0x13 in that frame. So v = k a for k = 1 .. 10 (0x12 for k <= 9),
    // then 11 a = 1.855107, then V.
    {
        LandMotion m;
        m.path = {V2{0.0f, 0.0f}, V2{400.0f, 0.0f}};
        m.next = 1;
        m.state = LocoState::WalkStopped;
        float x0 = 0.0f;
        for (int k = 1; k <= 13; ++k) {
            (void)walk_step(m, p, 3.0e38f, nullptr, &x);
            const float want = std::min(static_cast<float>(k) * acc, vmax);
            CHECK(near(m.speed, want, 1e-4f));
            CHECK(m.state == (k <= 9 ? LocoState::WalkStartMove : LocoState::WalkMoving));
            CHECK(m.wt_clip == (k <= 9 ? 16 : 2));
            CHECK(m.pos.x > x0);  // the move step runs in 0x12 too
            x0 = m.pos.x;
            if (k == 1) std::printf("t26 walk transition: speed after the first frame %.6f (a full-speed unit: %.6f)\n",
                                    static_cast<double>(m.speed), static_cast<double>(vmax));
        }
        CHECK(near(m.wt_f4, 12.972774f + 1.441419f - 14.0f, 1e-4f));  // +0xf4: the frames past count - 2
    }
    // Braking (F 0x14, B.7): in End Move at V with MOVE at frame 28 (rate V / S): T = v while MOVE plays; the next frame
    // MOVE wraps to 0.882838 <= its step 1.441419 -> MOVE_ENDONE starts at that frame (+0xf4 := the phase), +0xf0 := V,
    // T = a: v = V - (c - 1) a on calls c = 2 .. 10; the end clip has ended at frame 0.882838 + 9 x 1.441419 >= 17 - 2 -
    // 1.441419 on call 11 -> 0x15 (no speed test: v was 0.354186 > a), speed 0, arrived past the last line.
    {
        LandMotion m;
        m.path = {V2{0.0f, 0.0f}, V2{450.0f, 0.0f}};
        m.next = 1;
        m.pos = {440.0f, 0.0f};
        m.speed = vmax;
        m.state = LocoState::WalkEndMove;
        m.wt_clip = 2;
        m.wt_pos = 28.0f;
        m.wt_rate = vmax / wt.s;
        for (int c = 1; c <= 11; ++c) {
            const WalkResult r = walk_step(m, p, 3.0e38f, nullptr, &x);
            if (c == 1) {
                CHECK(near(m.speed, vmax, 1e-6f) && m.wt_clip == 2 && m.state == LocoState::WalkEndMove);
            } else if (c <= 10) {
                CHECK(near(m.speed, vmax - static_cast<float>(c - 1) * acc, 1e-4f) && m.wt_clip == 17 &&
                      m.state == LocoState::WalkEndMove);
                if (c == 2) CHECK(near(m.wt_f4, 0.882838f, 1e-4f) && m.wt_f0 == vmax);
            } else {
                CHECK(m.speed == 0.0f && r.arrived && m.state == LocoState::WalkStopped && m.wt_clip == -1 && m.wt_f0 == 0.0f);
            }
        }
    }
    // 0x12 with a path not started: MOVESTART current -> 0x14; else 0x15 (idle, speed 0)
    {
        LandMotion m;
        m.path = {V2{0.0f, 0.0f}, V2{400.0f, 0.0f}};
        m.next = 1;
        m.started = false;
        m.state = LocoState::WalkStartMove;
        m.speed = 0.5f;
        m.wt_clip = 16;
        m.wt_rate = vmax / wt.s;
        (void)walk_step(m, p, 3.0e38f, nullptr, &x);
        CHECK(m.state == LocoState::WalkEndMove);
        LandMotion q = m;
        q.state = LocoState::WalkStartMove;
        q.wt_clip = -1;
        (void)walk_step(q, p, 3.0e38f, nullptr, &x);
        CHECK(q.state == LocoState::WalkStopped && q.speed == 0.0f);
    }
    // The system: the clips come from the host's clip hook (set_deploy_clip_fn); the first moving frame is a, not V
    {
        MovementSystem sys{MovementConfig{}};
        land_system(sys);
        sys.set_deploy_clip_fn([](ObjectId, int32_t type, uint32_t& n, float& fps) {
            fps = 30.0f;
            n = type == 2 ? 30u : type == 16 ? 16u : type == 17 ? 17u : 0u;
            return n != 0u;
        });
        UnitDesc d = tank(1, cell_center(20, 50));
        d.max_speed = 1.2f;
        d.turn_rate = 5.0f;
        d.accel = d.decel = 0.0f;  // AT_ST_Walker sets no OverrideAcceleration / OverrideDeceleration
        d.walk_transition = true;
        d.anim_scale_speed = 1.6f * 0.8117f;
        CHECK(sys.add_unit(d));
        SimRng rng(2612);
        order(sys, rng, {1}, cell_center(20, 50) + V2{600.0f, 0.0f}, 1);
        uint32_t f = 1, first = 0;
        for (; f < 200 && first == 0; ++f) {
            sys.tick(f, rng);
            if (sys.view(1).speed > 0.0f) first = f;
        }
        CHECK(first != 0 && near(sys.view(1).max_speed, vmax, 1e-6f));
        CHECK(near(sys.view(1).speed, acc, 1e-5f) && sys.view(1).walk_state == LocoState::WalkStartMove);
        sys.tick(f, rng);
        CHECK(near(sys.view(1).speed, 2.0f * acc, 1e-5f));
    }
}

}  // namespace

int main() {
    test_droid();
    test_pose_after_turn();
    test_freeze_and_save();
    test_manual_deployers();
    test_damage_deploy();
    test_push_refusal();
    test_hover_and_exit();
    test_garrison_keep();
    test_released_inside();
    test_t26_controller();
    test_t26_corner_pass();
    test_t26_start_rule();
    test_t26_start_turn();
    test_t26_walk_step();
    test_t26_end_move();
    test_t26_coast_save();
    test_t26_container_step();  // s78n t26.5
    test_t26_formation_side_step();  // s78n t26 closing review
    test_t26_walk_transition();      // s78n t26 closing review
    return ::alamo::test::report("alamo_tactical_movement_t26_tests");
}
