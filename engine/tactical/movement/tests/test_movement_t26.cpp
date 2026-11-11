// alamo_tactical_movement_t26_tests (no framework; non-zero exit on failure).
// s78n t26.1: walking while deployed (docs/re/MOVEMENT.md §3.2.1): the cap test 0x3ac100 at a path start, the 0x19 walk
// branch with the Deployed_Walk_Speed cap, the 0x16 idle rules (the flag cleared at the stop; an auto-deployer with
// M > 0 and no DEPLOY ability deploys), Deploy(false) from the flag, the move order's skip of deployed manual deployers
// (§6.2) and the gate of deploying ones, the push / StoppedVsStopped refusal for M <= 0, Take_Damage's deploy, a
// lowering hover host's deploy and the walk-out arrivals reported to the garrison host.
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

}  // namespace

int main() {
    test_droid();
    test_pose_after_turn();
    test_freeze_and_save();
    test_manual_deployers();
    test_damage_deploy();
    test_push_refusal();
    test_hover_and_exit();
    return ::alamo::test::report("alamo_tactical_movement_t26_tests");
}
