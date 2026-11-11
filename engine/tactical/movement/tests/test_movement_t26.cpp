// alamo_tactical_movement_t26_tests (no framework; non-zero exit on failure).
// s78n t26.1: walking while deployed (docs/re/MOVEMENT.md §3.2.1): the cap test 0x3ac100 at a path start, the 0x19 walk
// branch with the Deployed_Walk_Speed cap, the 0x16 idle rules (the flag cleared at the stop; an auto-deployer with
// M > 0 and no DEPLOY ability deploys), Deploy(false) from the flag, the move order's skip of deployed manual deployers
// (§6.2) and the gate of deploying ones, the push / StoppedVsStopped refusal for M <= 0, Take_Damage's deploy, a
// lowering hover host's deploy and the walk-out arrivals reported to the garrison host.
// s78n t26.2: the path record's speed fractions and turn-done byte, the speed controller B (corner cap, brake-ahead, End
// Move T = a) and the start rule A with the 3.5.1 turn in place (MOVEMENT 3.5.2 A / B, 3.5.1 "Arrival turn, details"
// items 3-5).
#include <algorithm>
#include <cmath>
#include <cstdio>
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
    CHECK(flag_after && deployed_always && sys.view(1).arrived && near(distance(sys.view(1).pos, p0), 100.0f, 1e-3f));
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
StartTurnOut start_turn_case(float yaw0, float rate, float stopped, float ce, bool pre) {
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
    d.collision_class = kUccVehicle;
    CHECK(sys.add_unit(d));
    SimRng rng(2602);
    order(sys, rng, {1}, {800.0f, 400.0f}, 1);
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
    std::printf("t26.2 start turn: delta 90 R 10 -> %u turn ticks, step on %u; AT-AT-like delta 40 -> %u ticks at 1, step on %u; "
                "within CE -> %u; begin clip: %u play, yaw held %u ticks, %u turn ticks\n",
                a.turn_ticks, a.first_step, b.turn_ticks, b.first_step, c.turn_ticks, d.begin_plays, d.clip_hold, d.turn_ticks);
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
    test_t26_start_rule();
    test_t26_start_turn();
    return ::alamo::test::report("alamo_tactical_movement_t26_tests");
}
