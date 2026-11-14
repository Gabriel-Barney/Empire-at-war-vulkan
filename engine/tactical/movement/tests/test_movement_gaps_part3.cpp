// Part 3 of test_movement_gaps (bo3 t03: split by position; see test_movement_gaps_common.hpp).
#include "test_movement_gaps_common.hpp"

// s78n t18 item 3 (FIGHTER_FLYING §7.7 items 3 - 6): the squad container's speed step order and arrival.
//  * path end: braking starts at v^2 / (2a) before the end and takes exactly the rate a per frame - Stormtrooper squad
//    (a 0.078, team speed 1.248 = 16 a): 1.170, 1.092, ..., 0.078, then 0 (16 frames); Rebel squad (a 0.1092, 1.56): 13
//    steps down to 0.1404, then the rate itself, then 0 (15 frames); no snap onto the goal (it stops within 8 a of it);
//  * a MovingVsMoving hold (the stand-in's yield to a crossing vehicle) is taken at once;
//  * a new order while moving: the container brakes at the rate, coasting straight on, then starts the new path from 0;
//    no start delays are drawn; Stop zeroes the speed at once;
//  * a final facing on the order: turned to in place before the arrival.
void test_land_squad_speed_order() {
    const float mult = 1.56f;
    std::vector<T16Member> rebel;
    {
        const V2 rel[5] = {{10.0f, 6.0f}, {10.0f, -6.0f}, {0.0f, 0.0f}, {-10.0f, 6.0f}, {-10.0f, -6.0f}};
        for (const V2& p : rel) rebel.push_back({p, 1.0f, 0.07f, 0.5f, 0.0f});
    }
    const auto path_end = [&](const char* name, int32_t arrangement, float cmax, const std::vector<T16Member>& mem, float a,
                              float vmax, uint32_t full, bool rate_step) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, cell_center(10, 20), arrangement, cmax, 0.0f, mem);
        SimRng rng(81);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 20);
        sys.move(o, rng, 1);
        std::vector<float> v;
        std::vector<V2> p;
        bool arrived = false;
        for (uint32_t f = 1; f < 900 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            v.push_back(sys.view(100).speed);
            p.push_back(sys.view(100).pos);
            arrived = sys.view(100).arrived;
        }
        size_t c = 0;  // the last cruise frame
        for (size_t i = 0; i < v.size(); ++i)
            if (near(v[i], vmax, 1e-5f)) c = i;
        // the braking steps, then the rate itself when the last step left more than it (always for the Rebel squad; for
        // the Stormtrooper squad because the float sum lands a hair above 1.248 - 15 x 0.078), then the zero.
        // s78n t18 review: the frame count is pinned exactly - the speed step 0x5aaf70 replayed in floats from the team
        // speed with the rate (v -= min(a, v - a) while v > a), then the frame that zeroes it.
        const uint32_t brake = t18_brake_steps(vmax, a);
        CHECK(brake == full + (rate_step ? 1u : 0u));
        CHECK(arrived && c > 0 && v.size() == c + brake + 2);
        const size_t frames = v.size() - 1 - c;
        CHECK(frames == brake + 1u);
        bool steps = c + brake + 1 < v.size();
        for (uint32_t k = 1; steps && k <= full; ++k) steps = near(v[c + k], vmax - a * static_cast<float>(k), 2e-5f);
        if (steps && brake == full + 1) steps = near(v[c + full + 1], a, 1e-5f);
        CHECK(steps && v.back() == 0.0f);
        CHECK(p.back() == p[p.size() - 2]);  // the arrival frame takes no step: no snap
        const float off = length(p.back() - cell_center(30, 20));
        CHECK(off <= 8.0f * a + 1e-3f);
        std::printf("land squad speed order: %s - %zu braking frames, stopped %.3f from the goal\n", name, frames,
                    static_cast<double>(off));
    };
    // Stormtrooper: 15 steps of the rate leave 0.07800007 > 0.078, so the rate itself follows (17 frames with the zero);
    // Rebel: 13 steps leave 0.1404, then the rate, then the zero (15 frames)
    path_end("Stormtrooper squad", 0, 0.8f, t18_storm(), 0.05f * mult, 0.8f * mult, 15, true);
    path_end("Rebel squad", 1, 1.1f, rebel, 0.07f * mult, 1.0f * mult, 13, true);
    const float a = 0.05f * mult, vmax = 0.8f * mult;
    {  // s78n t23 (MOVEMENT §5.4.6): a vehicle crossing the squad's path - Path Around (state 1), the vehicle detours
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        UnitDesc v = unit(50, cell_center(25, 14), 2, 1.0f, 10.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        v.yaw_deg = 90.0f;
        CHECK(sys.add_unit(v));
        SimRng rng(82);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(40, 20);
        sys.move(o, rng, 1);
        bool crossing = false, detoured = false;
        // s78n t23 review fix 1: the self-stamped records re-decide, the squad arrives near frame 630 (was before 600)
        for (uint32_t f = 1; f < 900; ++f) {
            if (!crossing && sys.view(100).pos.x >= at.x + 200.0f) {
                MoveOrder vo;
                vo.units = {50};
                vo.destination = cell_center(25, 30);
                sys.move(vo, rng, f);
                crossing = true;
            }
            sys.tick(f, rng, serial_for);
            detoured = detoured || !sys.top_path(50).empty();
        }
        std::printf("land squad speed order: crossing vehicle - ordered %d, vehicle detoured %d, squad arrived %d, vehicle "
                    "arrived %d\n",
                    crossing ? 1 : 0, detoured ? 1 : 0, sys.view(100).arrived ? 1 : 0, sys.view(50).arrived ? 1 : 0);
        CHECK(crossing && detoured && sys.view(100).arrived && sys.view(50).arrived);
    }
    {  // a new order while moving, and Stop
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        SimRng rng(83);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(45, 20);
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 300 && !(near(sys.view(100).speed, vmax, 1e-5f) && sys.view(100).pos.x > at.x + 100.0f); ++f) sys.tick(f, rng, serial_for);
        CHECK(near(sys.view(100).speed, vmax, 1e-5f));
        const uint64_t draws0 = rng.draws();
        MoveOrder back;
        back.units = {1};
        back.destination = at;
        sys.move(back, rng, f);
        CHECK(near(sys.view(100).speed, vmax, 1e-5f));  // not taken at once
        for (ObjectId id = 1; id <= 9; ++id) CHECK(sys.team_start_delay(id) == 0);  // no start delays (the squad moves)
        CHECK(rng.draws() == draws0);
        const float x0 = sys.view(100).pos.x, yaw0 = sys.view(100).yaw;
        // s78n t26.5 (MOVEMENT §3.5.2 G, FIGHTER_FLYING §7.7 item 5): the coast starts at the path set - the old path stays on
        // top of the stack until the plan (this tick's plan pass, before the walk)
        bool steps = !sys.view(100).team_coast;
        for (uint32_t k = 1; k <= 15; ++k, ++f) {  // 15 frames of the rate while it coasts straight on
            sys.tick(f, rng, serial_for);
            steps = steps && near(sys.view(100).speed, vmax - a * static_cast<float>(k), 2e-5f) && sys.view(100).yaw == yaw0 &&
                    sys.view(100).team_coast;
        }
        // s78n t18 review: exactly as the path-end braking - the float sum left a hair more than the rate, so one more
        // step of the rate itself follows (t18_brake_steps = 16), then 0x15 zeroes the speed with no step
        CHECK(t18_brake_steps(vmax, a) == 16);
        sys.tick(f++, rng, serial_for);
        steps = steps && near(sys.view(100).speed, a, 1e-5f) && sys.view(100).team_coast;
        const float coast = sys.view(100).pos.x - x0;
        sys.tick(f++, rng, serial_for);  // 0x15: the speed (<= the rate) is zeroed, no step
        CHECK(!sys.view(100).team_coast);
        CHECK(steps && coast > 121.0f * a - 0.01f && coast < 121.0f * a + 0.01f);
        CHECK(sys.view(100).speed == 0.0f && sys.view(100).pos.x - x0 == coast);
        bool turned = false, arrived = false;
        for (uint32_t n = 0; n < 900 && !arrived; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView k = sys.view(100);
            turned = turned || (k.speed == 0.0f && k.yaw != yaw0);  // the start turn of the new path, from speed 0
            arrived = k.arrived;
        }
        CHECK(turned && arrived && length(sys.view(100).pos - at) <= 8.0f * a + 1e-3f);
        // Stop at cruise: zero at once
        MoveOrder again;
        again.units = {1};
        again.destination = cell_center(45, 20);
        sys.move(again, rng, f);
        for (uint32_t n = 0; n < 300 && !near(sys.view(100).speed, vmax, 1e-5f); ++n, ++f) sys.tick(f, rng, serial_for);
        CHECK(near(sys.view(100).speed, vmax, 1e-5f));
        const ObjectId one[1] = {1};
        sys.stop(one);
        CHECK(sys.view(100).speed == 0.0f && !sys.view(100).has_goal);
        std::printf("land squad speed order: new order while moving - coasted %.3f, then turned and arrived\n", static_cast<double>(coast));
    }
    {  // a final facing more than 10 degrees off: turned to in place before the arrival
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, cell_center(10, 20), 0, 0.8f, 0.0f, t18_storm(), 1);
        SimRng rng(84);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(25, 20);
        o.final_facing_deg = 90.0f;
        sys.move(o, rng, 1);
        uint32_t turn_frames = 0;
        bool arrived = false;
        V2 stop_at{};
        for (uint32_t f = 1; f < 900 && !arrived; ++f) {
            const UnitView k0 = sys.view(100);
            sys.tick(f, rng, serial_for);
            const UnitView k = sys.view(100);
            if (k.has_goal && k.speed == 0.0f && k0.speed == 0.0f && k.yaw != k0.yaw && k.pos.x > cell_center(24, 20).x) {
                ++turn_frames;
                stop_at = k.pos;
            }
            arrived = k.arrived;
        }
        const UnitView k = sys.view(100);
        std::printf("land squad speed order: final facing - arrived %d, %u turn frames, yaw %.3f" "\n", arrived ? 1 : 0, turn_frames,
                    static_cast<double>(k.yaw));
        // 90 degrees at 9.36 per frame: 10 turn frames (the first comes with the stop, the last ends the turn)
        CHECK(arrived && turn_frames >= 8 && turn_frames <= 10 && near(k.yaw, 90.0f, 1e-3f) && k.pos == stop_at);
    }
}

// s78n t18 items 4 / 5 (MOVEMENT §5.3 "Pair gates", FIGHTER_FLYING §7.7 item 2):
//  * the garrison-host gate: no collision object between a unit and its garrison host, either way round, in the
//    MovingVsStopped, MovingVsMoving and StoppedVsStopped tests; set_garrison_exit keeps it until the unit's next order
//    or the arrival of its move;
//  * hidden: no layer object, does not walk, takes no order;
//  * hover: a unit with Hover_Height rises before its first move and is "raised" from then on - a raised mover takes no
//    grounded obstacle, a grounded mover takes a raised one; hover_lower grounds it again;
//  * TEAM entries: a member suspended for a hero clash (or absent: parked) still counts in the joint box and the rate,
//    and a suspended one holds the squad through the straggler cap.
void test_land_squad_gates() {
    const auto grid_squad = [](MovementSystem& sys, V2 at) {  // 2 x 2, Rows, Max_Speed 2, no deceleration tag
        std::vector<T16Member> mem;
        for (float x : {8.0f, -8.0f})
            for (float y : {8.0f, -8.0f}) mem.push_back({V2{x, y}, 2.0f, 0.0f, 0.5f, 0.0f});
        t16_squad(sys, at, 0, 2.0f, 0.0f, mem, 1, true);
    };
    struct Out {
        uint32_t objects = 0, back_objects = 0;
        float off_line = 0.0f;
        bool arrived = false, moved = false;
    };
    // The squad marches from cell (15, 20) to (45, 20) straight through a standing vehicle 70 at (30, 20).
    // mode: 0 no gate; 1 the squad's garrison host is 70; 2 unit 70's garrison host is the squad; 3 the squad is hidden;
    // 4 the squad has just left 70 (set_garrison_exit after the order), then marches back through it.
    const auto run = [&](int mode) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(15, 20);
        grid_squad(sys, at);
        UnitDesc host = unit(70, cell_center(30, 20), 2, 1.0f, 15.0f, LocomotorKind::Default);
        host.collision_class = kUccVehicle;
        host.owner = 1;
        CHECK(sys.add_unit(host));
        const auto gates = [&] {
            if (mode == 1) CHECK(sys.set_pair_gate(100, 70, false));
            if (mode == 2) CHECK(sys.set_pair_gate(70, 100, false));
            if (mode == 3) CHECK(sys.set_pair_gate(100, -1, true));
        };
        const auto pair = [&] {
            uint32_t n = 0;
            for (const CollisionObject& c : sys.collisions().list())
                if ((c.a == 100 && c.b == 70) || (c.a == 70 && c.b == 100)) ++n;
            return n;
        };
        CHECK(!sys.set_pair_gate(999, 70, false) && !sys.set_garrison_exit(999, 70));
        SimRng rng(43);
        gates();  // host-set before the order and before every tick
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(45, 20);
        sys.move(o, rng, 1);
        if (mode == 4) CHECK(sys.set_garrison_exit(100, 70));
        for (uint32_t f = 1; f < 400; ++f) {
            gates();
            sys.tick(f, rng, serial_for);
            r.objects += pair();
            r.off_line = std::max(r.off_line, std::fabs(sys.view(100).pos.y - at.y));
        }
        r.arrived = sys.view(100).arrived && length(sys.view(100).pos - cell_center(45, 20)) < 2.0f;
        r.moved = !(sys.view(100).pos == at);
        if (mode == 4) {  // the arrival ended the gate: the way back collides with 70 again
            MoveOrder back;
            back.units = {1};
            back.destination = at;
            sys.move(back, rng, 400);
            for (uint32_t f = 400; f < 800; ++f) {
                sys.tick(f, rng, serial_for);
                r.back_objects += pair();
            }
        }
        return r;
    };
    const Out none = run(0), mine = run(1), theirs = run(2), hidden = run(3), left = run(4);
    CHECK(none.objects > 0 && none.off_line > 1.0f);  // without a gate: a record, and a path around the vehicle
    CHECK(mine.objects == 0 && mine.off_line < 0.01f && mine.arrived);      // gated: no object, straight through
    CHECK(theirs.objects == 0 && theirs.off_line < 0.01f && theirs.arrived);  // either side's host id gates the pair
    CHECK(hidden.objects == 0 && !hidden.moved && !hidden.arrived);          // hidden: it takes no order and stands
    CHECK(left.objects == 0 && left.off_line < 0.01f && left.arrived && left.back_objects > 0);
    std::printf("land squad gates: objects with the host %u / %u / %u / %u / %u (none / host / host's / hidden / left), then %u on "
                "the way back\n",
                none.objects, mine.objects, theirs.objects, hidden.objects, left.objects, left.back_objects);
    {  // the gate in the StoppedVsStopped test; a new order and Stop end set_garrison_exit
        const auto svs = [&](bool gated) {
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            UnitDesc a = unit(80, cell_center(20, 20), 2, 1.0f, 15.0f, LocomotorKind::Default);
            a.collision_class = kUccVehicle;
            UnitDesc b = unit(81, {cell_center(20, 20).x + 8.0f, cell_center(20, 20).y}, 2, 1.0f, 15.0f, LocomotorKind::Default);
            b.collision_class = kUccVehicle;
            CHECK(sys.add_unit(a) && sys.add_unit(b));
            if (gated) CHECK(sys.set_garrison_exit(81, 80));
            SimRng rng(44);
            uint32_t n = 0;
            for (uint32_t f = 1; f < 40; ++f) {
                sys.tick(f, rng, serial_for);
                n += static_cast<uint32_t>(sys.collisions().list().size());
            }
            if (gated) {
                CHECK(sys.view(80).pos == cell_center(20, 20));  // nobody was pushed apart
                const ObjectId one[1] = {81};
                sys.stop(one);  // Stop: the unit's next order - the gate ends
                for (uint32_t f = 40; f < 60; ++f) {
                    sys.tick(f, rng, serial_for);
                    n += static_cast<uint32_t>(sys.collisions().list().size());
                }
            }
            return n;
        };
        const uint32_t plain = svs(false);
        CHECK(plain > 0);
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc a = unit(80, cell_center(20, 20), 2, 1.0f, 15.0f, LocomotorKind::Default);
        UnitDesc b = unit(81, {cell_center(20, 20).x + 8.0f, cell_center(20, 20).y}, 2, 1.0f, 15.0f, LocomotorKind::Default);
        a.collision_class = b.collision_class = kUccVehicle;
        CHECK(sys.add_unit(a) && sys.add_unit(b) && sys.set_garrison_exit(81, 80));
        SimRng rng(45);
        uint32_t gated = 0;
        for (uint32_t f = 1; f < 40; ++f) {
            sys.tick(f, rng, serial_for);
            gated += static_cast<uint32_t>(sys.collisions().list().size());
        }
        CHECK(gated == 0 && svs(true) > 0);  // gated: no StoppedVsStopped object; after Stop there is one
    }
    {  // hover (Hover_Height 80, max speed 1.56 per frame: 52 frames to rise)
        const auto hover_run = [&](bool mover_hovers, bool obstacle_raised) {
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            UnitDesc m = unit(1, cell_center(10, 20), 2, 1.0f, 12.0f, LocomotorKind::Default);
            m.collision_class = kUccVehicle;
            m.hover_height = mover_hovers ? 80.0f : 0.0f;
            UnitDesc o = unit(2, cell_center(25, 20), 2, 1.0f, 12.0f, LocomotorKind::Default);
            o.collision_class = kUccVehicle;
            o.hover_height = 80.0f;
            CHECK(sys.add_unit(m) && sys.add_unit(o));
            SimRng rng(46);
            if (obstacle_raised) {  // a first move raises it for good
                MoveOrder oo;
                oo.units = {2};
                oo.destination = cell_center(25, 21);
                sys.move(oo, rng, 1);
            }
            uint32_t f = 1;
            for (; f < 200; ++f) sys.tick(f, rng, serial_for);
            CHECK(sys.hover_state(2) == (obstacle_raised ? 1 : 0) && !sys.view(2).has_goal);
            const V2 o0 = sys.view(2).pos;
            MoveOrder mo;
            mo.units = {1};
            mo.destination = {o0.x + 300.0f, o0.y};
            sys.move(mo, rng, f);
            uint32_t rise = 0;
            for (uint32_t n = 0; n < 700 && !sys.view(1).arrived; ++n, ++f) {
                sys.tick(f, rng, serial_for);
                if (sys.hover_state(1) == 2) ++rise;
            }
            CHECK(sys.view(1).arrived && rise == (mover_hovers ? 52u : 0u) && sys.hover_state(1) == (mover_hovers ? 1 : 0));
            return sys.stats().collisions_created;
        };
        CHECK(hover_run(true, false) == 0);   // a raised mover takes no grounded obstacle
        CHECK(hover_run(true, true) > 0);     // raised against raised: a record
        CHECK(hover_run(false, true) > 0);    // a grounded mover takes a raised standing unit
        CHECK(hover_run(false, false) > 0);
        // hover_lower: lowering takes the same time; the next move rises again
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc h = unit(1, cell_center(10, 20), 2, 1.0f, 12.0f, LocomotorKind::Default);
        h.hover_height = 80.0f;
        CHECK(sys.add_unit(h) && sys.add_unit(unit(2, cell_center(40, 40), 2, 1.0f, 6.0f, LocomotorKind::Default)));
        CHECK(!sys.hover_lower(2) && !sys.hover_lower(99));  // no Hover_Height / unknown
        SimRng rng(47);
        MoveOrder mo;
        mo.units = {1};
        mo.destination = cell_center(14, 20);
        sys.move(mo, rng, 1);
        uint32_t f = 1;
        for (; f < 300 && !sys.view(1).arrived; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(1).arrived && sys.hover_state(1) == 1);
        CHECK(sys.hover_lower(1) && sys.hover_state(1) == 3);
        uint32_t lower = 0;
        for (uint32_t n = 0; n < 100; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            if (sys.hover_state(1) == 3) ++lower;
        }
        CHECK(lower == 51 && sys.hover_state(1) == 0);  // 52 steps: the 52nd grounds it
    }
    {  // TEAM entries: a suspended member counts in the joint box and holds the squad through the straggler cap
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(30, 20);
        const std::vector<T16Member> mem = {{V2{10.0f, 0.0f}, 2.0f, 0.0f, 0.3f, 0.0f}, {V2{-10.0f, 0.0f}, 2.0f, 0.0f, 0.3f, 0.0f}};
        t16_squad(sys, at, -1, 2.0f, 1.5f, mem);
        const float hx = 13.0f + 1.5f;  // slots +-10, half-extent 3, MaxJiggleDistance 1.5
        float bx = 0.0f, by = 0.0f;
        CHECK(sys.team_joint_box(100, bx, by) && near(bx, hx, 1e-4f) && near(by, 4.5f, 1e-4f));
        CHECK(sys.team_entries(100).size() == 2 && sys.team_entries(100)[1].kind == 0);
        CHECK(sys.suspend_team_member(2));
        CHECK(sys.team_entries(100).size() == 2 && sys.team_entries(100)[1].member == 2 && sys.team_entries(100)[1].kind == 1);
        CHECK(sys.land_team(100).size() == 1);
        CHECK(sys.team_joint_box(100, bx, by) && near(bx, hx, 1e-4f) && near(by, 4.5f, 1e-4f));  // the box is unchanged
        SimRng rng(48);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(60, 20);
        sys.move(o, rng, 1);
        for (uint32_t f = 1; f < 300; ++f) sys.tick(f, rng, serial_for);
        const UnitView k = sys.view(100);
        // the container waits HX + 12 ahead of the member that stands where it stood, its order kept
        CHECK(k.has_goal && k.speed < 1e-3f && near(k.pos.x - sys.view(2).pos.x, hx + 12.0f, 1e-2f));
        CHECK(sys.rejoin_land_team(2));
        bool arrived = false;
        for (uint32_t f = 300; f < 900 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
        }
        CHECK(arrived);  // the member closed up and the squad went on
        // an absent (parked) member is still an entry: the box keeps its slot
        CHECK(sys.park_team_member(2) && !sys.has_unit(2));
        CHECK(sys.team_entries(100).size() == 2 && sys.team_entries(100)[1].kind == 2);
        CHECK(sys.team_joint_box(100, bx, by) && bx > 3.0f + 1.5f + 1.0f);  // more than member 1's slot alone would give
    }
}

// s78n t18 review:
//  * the raised-state gate with a synthetic hover type (Hover_Height 6.0; the FoC hover types are the Lancet and the HTT,
//    both 80, see the real-data test of alamo_tactical_movement_tests): raised four frames into its first move and for
//    good - no MovingVsStopped record against a grounded standing unit, no MovingVsMoving record with a grounded mover
//    (MOVEMENT 5.3 [H] as written);
//  * a squad container coasting after a new order with a team speed multiplier of 0 (rate 0) stops at once;
//  * decision step 10 stamps only the obstacle it decides: an older entry whose unit changed paths since is not revived;
//  * MovementPredictionInterval widens the MovingVsStopped look-ahead of every mover type, not only a squad container's;
//  * the push spot's occupancy: a foreign squad's member on the spot is no occupant, a foreign squad's container is.
void test_land_squad_t18_review() {
    const MovementConstants kc{};
    const float mult = kc.land_speed_multiplier;
    {  // the raised-state gate (a synthetic hover type)
        struct Out {
            uint32_t rise = 0, records = 0;
            uint8_t state = 0;
            bool arrived = false;
            float off_line = 0.0f;
        };
        // other: 0 a grounded vehicle standing on the path, 1 a grounded vehicle walking the same line the other way
        const auto run = [&](float hover, int other) {
            Out r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            const V2 at = cell_center(10, 20);
            UnitDesc m = unit(1, at, 2, 1.0f, 12.0f, LocomotorKind::Default);
            m.collision_class = kUccVehicle;
            m.hover_height = hover;
            UnitDesc o = unit(2, other == 0 ? cell_center(30, 20) : cell_center(50, 20), 2, 1.0f, 12.0f, LocomotorKind::Default);
            o.collision_class = kUccVehicle;
            o.yaw_deg = other == 0 ? 0.0f : 180.0f;
            CHECK(sys.add_unit(m) && sys.add_unit(o));
            SimRng rng(91);
            MoveOrder first;  // its first move raises it
            first.units = {1};
            first.destination = cell_center(11, 20);
            sys.move(first, rng, 1);
            uint32_t f = 1;
            for (; f < 200 && !sys.view(1).arrived; ++f) {
                sys.tick(f, rng, serial_for);
                if (sys.hover_state(1) == 2) ++r.rise;
            }
            CHECK(sys.view(1).arrived);
            r.state = sys.hover_state(1);
            const uint32_t before = sys.stats().collisions_created;
            MoveOrder go;
            go.units = {1};
            go.destination = cell_center(50, 20);
            sys.move(go, rng, f);
            if (other == 1) {
                MoveOrder back;
                back.units = {2};
                back.destination = cell_center(10, 20);
                sys.move(back, rng, f);
            }
            for (uint32_t n = 0; n < 900 && !r.arrived; ++n, ++f) {
                sys.tick(f, rng, serial_for);
                r.arrived = sys.view(1).arrived && (other == 0 || sys.view(2).arrived);
                r.off_line = std::max(r.off_line, std::fabs(sys.view(1).pos.y - at.y));
            }
            r.records = sys.stats().collisions_created - before;
            return r;
        };
        for (int other = 0; other < 2; ++other) {
            const Out hov = run(6.0f, other), plain = run(0.0f, other);
            // 6.0 at 1.56 per frame: the start frame and three steps stand in state 2, the fourth step raises it
            CHECK(hov.rise == 4 && hov.state == 1 && plain.rise == 0 && plain.state == 0);
            CHECK(hov.arrived && hov.records == 0 && hov.off_line < 1e-4f);  // raised: straight through, no record
            CHECK(plain.arrived && plain.records > 0);                       // the same walker without the tag: records
            std::printf("t18 review: a raised hover unit against a grounded %s - records %u (without the tag %u), rose in %u frames\n",
                        other == 0 ? "standing unit" : "mover", hov.records, plain.records, hov.rise);
        }
    }
    {  // coast with rate 0
        const float vmax = 0.8f * mult;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const auto team = [&](float factor) {
            CHECK(sys.set_team(100, 100, factor, 0.8f));
            for (ObjectId id = 1; id <= 9; ++id) CHECK(sys.set_team(id, 100, factor, 0.8f));
        };
        SimRng rng(92);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(45, 20);
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 300 && !(near(sys.view(100).speed, vmax, 1e-5f) && sys.view(100).pos.x > at.x + 100.0f); ++f) sys.tick(f, rng, serial_for);
        CHECK(near(sys.view(100).speed, vmax, 1e-5f));
        team(0.0f);  // the team speed multiplier drops to 0: team speed 0, rate 0
        MoveOrder back;
        back.units = {1};
        back.destination = at;
        sys.move(back, rng, f);
        // the order is not taken at once; s78n t26.5 (MOVEMENT §3.5.2 G): the coast starts at the path set (the tick's plan)
        CHECK(!sys.view(100).team_coast && near(sys.view(100).speed, vmax, 1e-5f));
        const V2 p0 = sys.view(100).pos;
        bool still = true;
        for (uint32_t n = 0; n < 40; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView k = sys.view(100);
            still = still && k.speed == 0.0f && k.pos == p0 && !k.team_coast;  // stopped in the first tick, no slide
        }
        CHECK(still && sys.view(100).has_goal);
        team(1.0f);  // the multiplier is back: the new order's path starts from speed 0
        bool arrived = false;
        for (uint32_t n = 0; n < 900 && !arrived; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
        }
        CHECK(arrived && length(sys.view(100).pos - at) <= 8.0f * 0.05f * mult + 1e-3f);
        std::printf("t18 review: coast with rate 0 - stood %d, then arrived %d\n", still ? 1 : 0, arrived ? 1 : 0);
    }
    {  // A walled lane (60 wide) closed by a row of hostile standing vehicles 40 apart (radius 20: no gap between them).
       // s78n t24 (MOVEMENT §5.4.7 "Rejoin candidates of a kind-3 Path Around", the same for kind 4): the path scan of
       // the rejoin generator runs to the path end with no count (only accepted goals count, at most 11), so the first
       // record's Path Around takes the first free point past the row as its goal; the lane leaves the graph no corner,
       // and the search's crossing pass (segments crossing an obstacle at the overlap penalty) gives the detour straight
       // through the row: one search, a detour record (Path Around state) that the mover walks through the row at full
       // speed, and no later search (the path was planned around the row). (Before s78n t24 the scan stopped after 11
       // tested points, every one on a footprint, so Path Around failed without a search and step 10 resolved the
       // records; this scene no longer reaches step 10.)
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(100, 100);
        for (int32_t x = 0; x < 100; ++x) {
            g.set_base(x, 18, kPassImpassable);
            g.set_base(x, 22, kPassImpassable);
        }
        sys.set_grid(std::move(g));
        sys.set_anim_length_fn(anim_len);
        const auto veh = [&](ObjectId id, int32_t cx, int32_t owner, float radius) {
            UnitDesc d = unit(id, cell_center(cx, 20), 2, 1.0f, radius, LocomotorKind::Default);
            d.collision_class = kUccVehicle;
            d.owner = owner;
            CHECK(sys.add_unit(d));
        };
        veh(1, 3, 0, 18.0f);
        for (ObjectId i = 0; i < 5; ++i) veh(2 + i, 15 + 2 * i, 1, 20.0f);  // x = 300, 340, 380, 420, 460
        SimRng rng(94);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(95, 20);
        sys.move(o, rng, 1);
        uint32_t f = 1, other_states = 0, around_states = 0;
        float min_speed = 3.0e38f;
        bool cruising = false;
        const auto run_to = [&](float x) {
            for (uint32_t n = 0; n < 1500 && sys.view(1).pos.x < x && !sys.view(1).arrived; ++n, ++f) {
                sys.tick(f, rng, serial_for);
                cruising = cruising || near(sys.view(1).speed, mult, 1e-5f);
                if (cruising) min_speed = std::min(min_speed, sys.view(1).speed);
                // (a record step 10 resolved is dropped in the same pass: no state is ever left on the list)
                for (const CollisionObject& c : sys.collisions().list()) {
                    if (c.state == CollisionState::PathAround) ++around_states;
                    else if (c.state != CollisionState::Active) ++other_states;
                }
            }
            return sys.stats().dynamic_repaths;
        };
        // the records with the first two units of the row are decided before x = 260 (contact is predicted 70 + 38 ahead)
        const uint32_t n1 = run_to(260.0f);
        const uint32_t a1 = around_states;
        const bool on_path = !sys.view(1).pushed;
        const uint32_t n2 = run_to(cell_center(23, 20).x + 80.0f);  // past the row
        for (uint32_t n = 0; n < 900 && !sys.view(1).arrived; ++n, ++f) sys.tick(f, rng, serial_for);
        const MovementStats& st = sys.stats();
        std::printf("t18 review: step 10 stamp - dynamic re-paths %u / %u before / past the third of the row, pushes %u, push failures %u, "
                    "early arrivals %u\n", n1, n2, st.pushes, st.push_failures, st.early_arrivals);
        CHECK(n1 == 1 && a1 > 0 && !on_path);          // one search, its detour on the stack
        CHECK(n2 == n1 && around_states > a1);          // no further search: the detour is walked through the row
        CHECK(other_states == 0 && st.pushes == 0 && st.push_failures == 0 && st.early_arrivals == 0);
        CHECK(near(min_speed, mult, 1e-5f));           // never slowed or held
        bool stood = true;
        for (ObjectId i = 0; i < 5; ++i) stood = stood && sys.view(2 + i).pos == cell_center(15 + 2 * i, 20);
        CHECK(sys.view(1).arrived && stood);
    }
    {  // MovementPredictionInterval of a vehicle that is no squad container (FoC: AT-AT 5.0, SPMA-T 4.0, Pod Walker 12 ...).
       // A slow one (0.468 per frame), so that the look-ahead stays inside the 160 x 160 candidate box.
        const float vmax = 0.3f * mult;
        const auto first_record = [&](float interval) {
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            UnitDesc m = unit(1, cell_center(20, 20), 2, 0.3f, 12.0f, LocomotorKind::Default);
            m.collision_class = kUccVehicle;
            m.movement_prediction_interval = interval;
            UnitDesc o = unit(2, cell_center(30, 20), 2, 1.0f, 12.0f, LocomotorKind::Default);
            o.collision_class = kUccVehicle;
            CHECK(sys.add_unit(m) && sys.add_unit(o));
            SimRng rng(95);
            MoveOrder go;
            go.units = {1};
            go.destination = cell_center(45, 20);
            sys.move(go, rng, 1);
            for (uint32_t f = 1; f < 900; ++f) {
                sys.tick(f, rng, serial_for);
                if (sys.stats().collisions_created > 0) return distance(sys.view(1).pos, sys.view(2).pos);
            }
            return -1.0f;
        };
        // the record appears on the first check frame (every FramesPerCollisionCheck frames) with the two circles (12
        // each) closer along the path than max speed x 30 x max(MovingVsMovingLookAheadTime, the interval)
        const float step = static_cast<float>(kc.frames_per_collision_check) * vmax;
        const float look0 = vmax * 30.0f * kc.moving_vs_moving_look_ahead_time, look4 = vmax * 30.0f * 4.0f;
        const float d0 = first_record(0.0f), d4 = first_record(4.0f), d1 = first_record(1.0f);
        CHECK(kc.moving_vs_moving_look_ahead_time < 4.0f && kc.moving_vs_moving_look_ahead_time > 1.0f);
        CHECK(look4 + 24.0f < 0.5f * kCollisionCrowdBox + 12.0f);  // inside the candidate box (+ the unit's record box)
        // (the check runs before the walk of its tick: the mover is one step closer when the record is first seen)
        CHECK(d0 + vmax <= look0 + 24.0f + 1e-3f && d0 + vmax > look0 + 24.0f - step - 1e-3f);
        CHECK(d4 + vmax <= look4 + 24.0f + 1e-3f && d4 + vmax > look4 + 24.0f - step - 1e-3f);
        CHECK(d1 == d0);  // an interval below the global look-ahead changes nothing (the larger of the two)
        std::printf("t18 review: MovementPredictionInterval - first record %.2f apart (none), %.2f (4.0 s), %.2f (1.0 s)\n",
                    static_cast<double>(d0), static_cast<double>(d4), static_cast<double>(d1));
    }
    {  // push-spot occupancy: giant 1 pushes infantry 2 off its path
        struct Res {
            V2 spot;
            bool pushed = false, arrived = false;
        };
        // what: 0 nothing else; 1 a foreign squad's member (201; its container 200 far away) alone on `near_spot`; 2 the
        // foreign container 10 beside `near_spot` (away from the giant's path): its joint box covers the spot's query box
        const auto run = [&](int what, V2 near_spot) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            UnitDesc gv = unit(1, cell_center(5, 20), 2, 1.0f, 14.0f, LocomotorKind::Default);
            gv.collision_class = kUccGiantVehicle;
            gv.owner = 0;
            UnitDesc b = unit(2, {cell_center(30, 20).x, cell_center(30, 20).y - 1.0f}, 2, 1.5f, 5.0f, LocomotorKind::Default);
            b.owner = 0;
            CHECK(sys.add_unit(gv) && sys.add_unit(b));
            if (what != 0) {
                t18_squad(sys, 200, cell_center(80, 80), 0.0f, 0);
                CHECK(sys.set_team_target(200, true));  // its members stand where they are put (no recovery to the slots)
                if (what == 1) CHECK(sys.teleport(201, near_spot, 0.0f, 0.0f, 0));
                else CHECK(sys.teleport(200, V2{near_spot.x, near_spot.y - 10.0f}, 0.0f, 0.0f, 0));
            }
            SimRng rng(93);
            MoveOrder o;
            o.units = {1};
            o.destination = cell_center(55, 20);
            sys.move(o, rng, 1);
            for (uint32_t f = 1; f < 1500 && !r.arrived; ++f) {
                if (what != 0) CHECK(sys.set_team_target(200, true));
                sys.tick(f, rng, serial_for);
                r.pushed = r.pushed || sys.view(2).pushed;
                r.arrived = sys.view(1).arrived && !sys.view(2).pushed;
                if (what == 1) CHECK(sys.view(201).pos == near_spot);  // the member stood on the spot all the time
            }
            r.spot = sys.view(2).pos;
            return r;
        };
        const Res free_run = run(0, V2{});
        CHECK(free_run.pushed && free_run.arrived && free_run.spot.y < cell_center(30, 20).y - 15.0f);  // off the path, to its side
        const Res member = run(1, free_run.spot), container = run(2, free_run.spot);
        CHECK(member.pushed && member.arrived && member.spot == free_run.spot);  // a squad member is no occupant
        CHECK(container.pushed && container.arrived && length(container.spot - free_run.spot) > 1.0f);  // a container is
        std::printf("t18 review: push spot (%.2f, %.2f); a foreign member on it (%.2f, %.2f); a foreign container at it (%.2f, %.2f)\n",
                    static_cast<double>(free_run.spot.x), static_cast<double>(free_run.spot.y), static_cast<double>(member.spot.x),
                    static_cast<double>(member.spot.y), static_cast<double>(container.spot.x), static_cast<double>(container.spot.y));
    }
}

// ---------------------------------------------------------------- s78n t20 (batch A)
// A 9-member squad on a 3 x 3 grid of pitch 12 (member half extents 3): joint slot box (15, 15). Container `cid`, members
// cid + 1 .. cid + 9.
void t20_squad(MovementSystem& sys, ObjectId cid, V2 at, int32_t owner, float cmax) {
    UnitDesc c = unit(cid, at, 2, cmax, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = 0;
    c.owner = owner;
    c.type_hx = c.type_hy = 0.0f;  // the container type's extents 0x371be0 (no model)
    CHECK(sys.add_unit(c));
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    ObjectId id = cid + 1;
    for (int32_t ix = 1; ix >= -1; --ix)
        for (int32_t iy = 1; iy >= -1; --iy) {
            const V2 rel{12.0f * static_cast<float>(ix), 12.0f * static_cast<float>(iy)};
            UnitDesc d = unit(id, at + rel, 2, 1.0f, 3.0f, LocomotorKind::Default);
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

// A vehicle with hard extents (hx, hy).
UnitDesc t20_vehicle(ObjectId id, V2 pos, int32_t owner, float hx, float hy, float speed) {
    UnitDesc d = unit(id, pos, 2, speed, 12.0f, LocomotorKind::Default);
    d.collision_class = kUccVehicle;
    d.hard_x = hx;
    d.hard_y = hy;
    d.owner = owner;
    return d;
}

// s78n t20 item 6 (MOVEMENT 4.3.1): the destination check of a land move order.
void test_t20_destination_check() {
    const V2 at = cell_center(15, 20), dest = cell_center(30, 20);  // (300, 400) -> (600, 400): the final direction is +x
    // The spec's procedure for one lone squad (HX = HY = 15, S = 5) against one rectangle (bhx, bhy) at `bp`: try centres
    // in order; a try is rejected while the squad's slot rectangle (20, 20) overlaps the blocker and the blocker's area
    // bhx x bhy > 0.15 x 20 x 20. Returns the first accepted centre.
    const auto first_centre = [&](V2 bp, float bhx, float bhy) {
        float t = 0.0f;
        const V2 v{-1.0f, 0.0f};
        const V2 axes[4] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
        for (uint32_t k = 0; k < 40; ++k) {
            const float half = 0.5f * ((k % 2u) == 0u ? 1.0f : -0.75f);
            V2 c = dest;
            if (k % 8u < 4u) {
                c = dest + v * (t * half);
                t += 7.0f;
            } else {
                c = dest + v * ((t - 28.0f) * half) + axes[k % 8u - 4u] * 20.0f;
            }
            const bool overlap = std::fabs(c.x - bp.x) <= 20.0f + bhx && std::fabs(c.y - bp.y) <= 20.0f + bhy;
            if (!(overlap && bhx * bhy > 0.15f * 20.0f * 20.0f)) return c;
        }
        return dest;
    };
    struct Out {
        V2 goal;
        bool arrived = false;
        uint32_t tal = 0, records = 0;
        uint64_t crc = 0;
        V2 end;
    };
    // what: 0 a standing squad on the goal; 1 the same squad with an order away; 2 nothing there, but a squad standing
    // 60 beside it (inside the query box, not overlapping) was ordered to the goal first; 3 one standing infantry unit (3 x 3) on the goal; 4 flag 0x20; 5 skip_destination_check
    const auto run = [&](int what, const ParallelFor& pf) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t20_squad(sys, 100, at, 1);
        if (what == 0 || what == 1 || what == 4 || what == 5) t20_squad(sys, 200, dest, 1);
        if (what == 2) t20_squad(sys, 200, cell_center(30, 17), 1);  // 60 beside the goal: inside the query box
        if (what == 3) {
            UnitDesc x = unit(70, dest, 2, 1.0f, 3.0f, LocomotorKind::Default);
            x.owner = 1;
            CHECK(sys.add_unit(x));
        }
        SimRng rng(201);
        sys.tick(1, rng, pf);
        if (what == 1 || what == 2) {
            MoveOrder ob;
            ob.units = {201};
            ob.destination = what == 1 ? cell_center(30, 60) : dest;
            sys.move(ob, rng, 2);
        }
        MoveOrder o;
        o.units = {101};
        o.destination = dest;
        if (what == 4) o.flags = kMoveFlagSkipValidation;
        if (what == 5) o.skip_destination_check = true;
        sys.move(o, rng, 2);
        r.goal = sys.view(100).goal;
        for (uint32_t f = 2; f < 1200 && !r.arrived; ++f) {
            sys.tick(f, rng, pf);
            r.arrived = sys.view(100).arrived;
            for (const CollisionObject& c : sys.collisions().list()) {
                if (c.a != 100 && c.b != 100) continue;
                ++r.records;
                if (c.state == CollisionState::TryAgainLater) ++r.tal;
            }
        }
        r.end = sys.view(100).pos;
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        return r;
    };
    {  // (i) onto an identical standing squad: every try within 35 of it on both axes is rejected (225 > 60)
        const Out s = run(0, serial_for), p = run(0, make_thread_pf(4));
        const V2 c = first_centre(dest, 15.0f, 15.0f);
        CHECK(near(c.x, dest.x - 48.0f, 1e-3f) && c.y == dest.y);  // try 22: 28 short + min(HX + S, HY + S) west
        // s78n t20 F5: the accepted centre itself (no cell snap; F8's axis-frame box moves min(HX + S, HY + S) by float noise)
        CHECK(near(s.goal.x, c.x, 1e-3f) && near(s.goal.y, c.y, 1e-3f));
        CHECK(s.arrived && s.tal == 0 && s.crc == p.crc);
        CHECK(std::fabs(s.end.y - dest.y) < 1e-3f && std::fabs(s.end.x - c.x) < 8.0f);  // (a container brakes to a stop)
        std::printf("t20 destination check: onto a standing squad - goal (%.1f, %.1f), stopped at %.2f, records %u, Try Again Later %u\n",
                    static_cast<double>(s.goal.x), static_cast<double>(s.goal.y), static_cast<double>(s.end.x), s.records, s.tal);
    }
    {  // (ii) the standing squad has an order away: it counts where it is going - the goal stays
        const Out s = run(1, serial_for);
        CHECK(s.goal == dest);
    }
    {  // (iii) a third squad was ordered to the empty goal first: displaced as in (i)
        const Out s = run(2, serial_for);
        const V2 c3 = first_centre(dest, 15.0f, 15.0f);
        CHECK(near(s.goal.x, c3.x, 1e-3f) && near(s.goal.y, c3.y, 1e-3f));
    }
    {  // (iv) one standing infantry unit: 3 x 3 = 9 is inside the 15 % allowance (60) - the goal stays
        const Out s = run(3, serial_for);
        CHECK(first_centre(dest, 3.0f, 3.0f) == dest && s.goal == dest);
    }
    {  // (v) a forced-path order (flag 0x20) and an object goal: no check
        CHECK(run(4, serial_for).goal == dest && run(5, serial_for).goal == dest);
    }
    {  // (vi) three single-vehicle formations ordered to one point: three goals whose rectangles (HX + 5, HY + 5) are
       // apart and none on the ordered point (s78n t20 F5: the accepted centres, not quantised to cells)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 starts[3] = {cell_center(10, 20), cell_center(12, 34), cell_center(12, 6)};
        for (ObjectId i = 0; i < 3; ++i) {
            UnitDesc d = t20_vehicle(10 + i, starts[i], 1);
            d.grouping = kGroupingSolo;  // each its own formation
            CHECK(sys.add_unit(d));
        }
        SimRng rng(202);
        MoveOrder o;
        o.units = {10, 11, 12};
        o.destination = dest;
        sys.move(o, rng, 1);
        V2 g[3];
        for (ObjectId i = 0; i < 3; ++i) g[i] = sys.view(10 + i).goal;
        CHECK(sys.view(10).form_id != 0 && sys.view(10).form_id != sys.view(11).form_id && sys.view(11).form_id != sys.view(12).form_id);
        bool apart = true, off_point = true;
        for (int i = 0; i < 3; ++i) {
            off_point = off_point && !(g[i] == dest);
            for (int j = i + 1; j < 3; ++j) apart = apart && distance(g[i], g[j]) >= 2.0f * 11.0f;
        }
        CHECK(apart && off_point);
        bool all = false;
        for (uint32_t f = 1; f < 1500 && !all; ++f) {
            sys.tick(f, rng, serial_for);
            all = sys.view(10).arrived && sys.view(11).arrived && sys.view(12).arrived;
        }
        CHECK(all);
        std::printf("t20 destination check: three formations - goals (%.0f, %.0f) (%.0f, %.0f) (%.0f, %.0f)\n", static_cast<double>(g[0].x),
                    static_cast<double>(g[0].y), static_cast<double>(g[1].x), static_cast<double>(g[1].y), static_cast<double>(g[2].x),
                    static_cast<double>(g[2].y));
    }
    {  // (vii) the goal at the end of a corridor one cell wide: every rectangle of every try and pass reaches into the
       // walls - the order is not refused and the goal stays
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(100, 100);
        for (int32_t y = 10; y <= 30; ++y)
            for (int32_t x = 18; x <= 42; ++x)
                if (!(y == 20 && x <= 30)) g.set_base(x, y, kPassImpassable);  // a corridor along y = 20 up to x = 30
        sys.set_grid(std::move(g));
        sys.set_anim_length_fn(anim_len);
        t20_squad(sys, 100, at, 1);
        t20_squad(sys, 200, cell_center(8, 40), 1);
        SimRng rng(203);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {101};
        o.destination = dest;
        sys.move(o, rng, 2);
        CHECK(sys.view(100).goal == dest && sys.view(100).has_goal);
        bool arrived = false;
        for (uint32_t f = 2; f < 1500 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
        }
        CHECK(arrived && distance(sys.view(100).pos, dest) < 8.0f);
    }
    {  // (viii) the single-member shortcut: a unit ordered onto its own footprint (the goal inside its footprint radius
       // sqrt(10^2 + 6^2) = 11.66) does not move and reports arrived; one ordered just outside it walks
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, cell_center(20, 20), 1)));
        CHECK(sys.add_unit(t20_vehicle(11, cell_center(20, 30), 1)));
        SimRng rng(204);
        MoveOrder o;
        o.units = {10};
        o.destination = cell_center(20, 20) + V2{8.0f, 3.0f};  // quantised to its own cell: distance 0 < 11.66
        sys.move(o, rng, 1);
        MoveOrder o2;
        o2.units = {11};
        o2.destination = cell_center(21, 30);  // 20 away > 11.66
        sys.move(o2, rng, 1);
        float moved = 0.0f, moved2 = 0.0f;
        bool started2 = false;
        for (uint32_t f = 1; f < 200; ++f) {
            sys.tick(f, rng, serial_for);
            moved = std::max(moved, distance(sys.view(10).pos, cell_center(20, 20)));
            moved2 = std::max(moved2, distance(sys.view(11).pos, cell_center(20, 30)));
            started2 = started2 || sys.view(11).speed > 0.0f;
        }
        CHECK(moved == 0.0f && sys.view(10).arrived && !sys.view(10).has_goal && sys.land_path(10).empty());
        // s78n t26.4 (MOVEMENT 3.5.2 C.4 / C.11, 3.5.1 item 1): no snap - it stops past its last line by at most one step
        CHECK(started2 && sys.view(11).arrived && moved2 > 20.0f - 5.0f && moved2 < 20.0f + 1.56f);
        CHECK(sys.stats().paths_planned == 1);  // the unit on its own footprint was never planned
    }
}

// s78n t20 item 3 (MOVEMENT 1.5.1): the Path Around call inside the system - the detour is a record on top of the path
// stack, the ordered path stays underneath untouched, and the unit resumes it at the next waypoint when the detour ends.
// s78n t20 F3: a detour whose start lies inside the obstacle ends. The mover stands 15 behind a standing friendly
// vehicle (their feet overlap: the start is inside its grown rectangle), a wall of cells closes the near side; ordered
// past it, the mover's order ends (arrived, or stopped) - s78n t24: a failed step makes a TerrainCollision record (a
// detour or a stop, MOVEMENT §5.4.7), so no detour loop runs for ever. Serial = 4 workers.
void test_t20_detour_start_inside() {
    struct Out {
        uint64_t crc = 0;
        bool ended = false, arrived = false, failed = false, pushed = false;
        uint32_t end_frame = 0;
        MovementStats st;
    };
    const V2 bp = cell_center(25, 20), start{bp.x - 15.0f, bp.y}, goal = cell_center(45, 20);
    const auto run = [&](const ParallelFor& pf) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        for (int32_t y = 17; y <= 19; ++y) sys.grid().set_base(23, y, kPassImpassable);  // the near -y corner side
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        CHECK(sys.add_unit(t20_vehicle(11, bp, 1)));
        SimRng rng(303);
        MoveOrder o;
        o.units = {10};
        o.destination = goal;
        sys.move(o, rng, 1);
        for (uint32_t f = 1; f < 4000 && !r.ended; ++f) {
            sys.tick(f, rng, pf);
            const UnitView v = sys.view(10);
            r.pushed = r.pushed || v.pushed;
            if (!v.has_goal && !v.pushed) {
                r.ended = true;
                r.arrived = v.arrived;
                r.failed = v.failed;
                r.end_frame = f;
            }
        }
        r.st = sys.stats();
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        return r;
    };
    const Out s = run(serial_for), p = run(make_thread_pf(4));
    std::printf("t20 F3: detour from inside the obstacle - ended %d at frame %u (arrived %d, failed %d), pushed %d, terrain "
                "records %u (detours %u), dynamic re-paths %u\n",
                s.ended ? 1 : 0, s.end_frame, s.arrived ? 1 : 0, s.failed ? 1 : 0, s.pushed ? 1 : 0, s.st.terrain_records,
                s.st.terrain_detours, s.st.dynamic_repaths);
    CHECK(s.crc == p.crc && s.end_frame == p.end_frame);
    CHECK(s.ended && (s.arrived || s.failed));
}

// s78n t20 F5 (MOVEMENT §4.3.1: the block goal := the accepted centre): a checked order takes no cell snap. One
// vehicle (10 x 6) ordered from (303, 607) to (603, 607): (a) nothing there - the goal is (603, 607) and the vehicle ends
// there; (b) a standing vehicle (10 x 6) at (605, 607), over the goal - the goal is the first try the spec's procedure
// accepts (the slot rectangle (15, 11) clear of the stander's (10, 6), or the stander's area 60 within 15 % of 15 x 11),
// unsnapped (the stander sits 2 off the goal so that no try lies on a touching boundary); (c) a queued order (no check)
// keeps the snap. Serial = 4 workers.
void test_t20_unsnapped_goal() {
    const V2 at{303.0f, 607.0f}, dest{603.0f, 607.0f}, stander{605.0f, 607.0f};
    const auto first_try = [&]() {
        float t = 0.0f;
        const V2 v{-1.0f, 0.0f};
        const V2 axes[4] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
        for (uint32_t k = 0; k < 40; ++k) {
            const float half = 0.5f * ((k % 2u) == 0u ? 1.0f : -0.75f);
            V2 c = dest;
            if (k % 8u < 4u) {
                c = dest + v * (t * half);
                t += 7.0f;
            } else {
                c = dest + v * ((t - 28.0f) * half) + axes[k % 8u - 4u] * 11.0f;
            }
            const bool overlap = std::fabs(c.x - stander.x) <= 15.0f + 10.0f && std::fabs(c.y - stander.y) <= 11.0f + 6.0f;
            if (!(overlap && 10.0f * 6.0f > 0.15f * 15.0f * 11.0f)) return c;
        }
        return dest;
    };
    struct Out {
        V2 goal0, goal1, end;
        bool arrived = false;
        uint64_t crc = 0;
    };
    const auto run = [&](int what, const ParallelFor& pf) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, at, 1)));
        if (what == 1) CHECK(sys.add_unit(t20_vehicle(11, stander, 1)));
        SimRng rng(204);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        if (what == 2) o.flags = kMoveFlagQueue;
        sys.move(o, rng, 1);
        r.goal0 = sys.view(10).goal;
        for (uint32_t f = 1; f < 2000 && !r.arrived; ++f) {
            sys.tick(f, rng, pf);
            r.arrived = sys.view(10).arrived;
        }
        r.goal1 = sys.view(10).goal;
        r.end = sys.view(10).pos;
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        return r;
    };
    const Out a = run(0, serial_for), ap = run(0, make_thread_pf(4));
    const Out b = run(1, serial_for), bp = run(1, make_thread_pf(4));
    const Out q = run(2, serial_for);
    const V2 want = first_try();
    std::printf("t20 F5: unsnapped goals - free (%.2f, %.2f) ended (%.2f, %.2f); stander: first accepted try (%.2f, %.2f), "
                "goal (%.2f, %.2f); queued (%.2f, %.2f)\n",
                static_cast<double>(a.goal1.x), static_cast<double>(a.goal1.y), static_cast<double>(a.end.x), static_cast<double>(a.end.y),
                static_cast<double>(want.x), static_cast<double>(want.y), static_cast<double>(b.goal0.x), static_cast<double>(b.goal0.y),
                static_cast<double>(q.goal0.x), static_cast<double>(q.goal0.y));
    CHECK(a.goal0 == dest && a.goal1 == dest && a.arrived && distance(a.end, dest) < 1.0f && a.crc == ap.crc);
    CHECK(!(want == dest) && b.goal0 == want && b.goal1 == want && b.arrived && b.crc == bp.crc);
    CHECK(q.goal0 == cell_center(world_to_cell(dest.x), world_to_cell(dest.y)));  // unchecked: the slot search's cell
}

void test_t20_path_around() {
    struct Out {
        uint64_t crc = 0;
        bool around = false, kind2 = false, order_kept = true, overlap = false, arrived = false, tagged = false;
        float min_y = 3.0e38f, max_y = -3.0e38f;
        std::vector<V2> detour;
        MovementStats st;
        V2 end, blocker;
        uint32_t resume_frame = 0;
        V2 resume_dir, resume_pos;
    };
    const V2 start = cell_center(15, 20), goal = cell_center(40, 20), bp = cell_center(25, 20);  // y = 400 throughout
    const auto run = [&](const ParallelFor& pf) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        CHECK(sys.add_unit(t20_vehicle(11, bp, 1)));  // a standing friendly vehicle on the line
        SimRng rng(301);
        MoveOrder o;
        o.units = {10};
        o.destination = goal;
        sys.move(o, rng, 1);
        std::vector<V2> order_path;
        bool was_pushed = false;
        for (uint32_t f = 1; f < 1500 && !r.arrived; ++f) {
            sys.tick(f, rng, pf);
            const UnitView v = sys.view(10);
            r.arrived = v.arrived;
            if (order_path.empty() && !sys.land_path(10).empty()) order_path.assign(sys.land_path(10).begin(), sys.land_path(10).end());
            if (v.pushed) {
                r.kind2 = r.kind2 || v.push_kind == 2;
                if (r.detour.empty()) r.detour.assign(sys.top_path(10).begin(), sys.top_path(10).end());
                r.tagged = r.tagged || !sys.top_tags(10).empty();
                // the ordered path is untouched while the detour is walked
                const std::span<const V2> now = sys.land_path(10);
                r.order_kept = r.order_kept && now.size() == order_path.size() && std::equal(now.begin(), now.end(), order_path.begin());
            } else if (was_pushed && r.resume_frame == 0) {
                r.resume_frame = f;
            } else if (r.resume_frame != 0 && f == r.resume_frame + 20) {
                r.resume_dir = dir_from_deg(v.yaw);
                r.resume_pos = v.pos;
            }
            was_pushed = v.pushed;
            for (const CollisionObject& c : sys.collisions().list())
                r.around = r.around || (c.a == 10 && c.b == 11 && c.kind == CollisionKind::MovingVsStopped && c.state == CollisionState::PathAround);
            r.min_y = std::min(r.min_y, v.pos.y);
            r.max_y = std::max(r.max_y, v.pos.y);
            r.overlap = r.overlap || foot_overlap(foot_rect(v.pos, v.yaw, 10.0f, 6.0f), foot_rect(sys.view(11).pos, sys.view(11).yaw, 10.0f, 6.0f));
        }
        r.st = sys.stats();
        r.end = sys.view(10).pos;
        r.blocker = sys.view(11).pos;
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        return r;
    };
    const Out s = run(serial_for), p = run(make_thread_pf(4));
    CHECK(s.crc == p.crc);
    CHECK(s.around && s.kind2 && s.order_kept && s.st.dynamic_repaths == 1 && s.st.pushes == 0);
    // the detour: the unit's position, then the two corners of the obstacle's grown rectangle (11 + 6.5, 7 + 6.5) on the
    // -b side (the two sides tie; the heap rules take that one); the rejoin point is dropped, because the last corner sees
    // the next ordered waypoint
    CHECK(s.detour.size() == 3);
    if (s.detour.size() == 3) {
        CHECK(s.detour[0].y == 400.0f && s.detour[0].x < bp.x - 17.5f);
        CHECK(s.detour[1] == (V2{bp.x - 17.5f, 400.0f - 13.5f}) && s.detour[2] == (V2{bp.x + 17.5f, 400.0f - 13.5f}));
    }
    CHECK(s.tagged);  // the front corner carries the obstacle's hold line
    CHECK(s.min_y < 400.0f - 13.0f && s.max_y <= 400.0f + 1e-3f && !s.overlap);  // around the y < line side, never touching
    // after the pop it walks on along the ordered segment (+x, y = 400). s78n t26.4 (MOVEMENT 5.4.1 "How Walk combines"
    // item 2, 3.5.2 C.6): its wanted direction is the segment's plus the lane term k (-d.y, d.x), k from its side offset
    // e = 400 - y, which brings it back to its line; 20 frames after the pop the facing has caught up with it
    const float k = formation_side_correction(400.0f - s.resume_pos.y);
    const V2 want = normalize_retail(V2{1.0f, k});
    CHECK(s.resume_frame != 0 && k > 0.0f && dot(s.resume_dir, want) > 0.999f);
    std::printf("t20 path around: 20 frames after the pop at y %.2f, facing %.2f deg (lane term %.4f: %.2f deg)\n",
                static_cast<double>(s.resume_pos.y), static_cast<double>(heading_deg(s.resume_dir)), static_cast<double>(k),
                static_cast<double>(heading_deg(want)));
    CHECK(s.arrived && distance(s.end, goal) < 1.0f && s.blocker == bp);
    std::printf("t20 path around: detour %zu points, re-paths %u, y range %.2f .. %.2f, resumed at frame %u\n", s.detour.size(),
                s.st.dynamic_repaths, static_cast<double>(s.min_y), static_cast<double>(s.max_y), s.resume_frame);

    {  // A unit without an order path (pushed aside, walking its push path) cannot path around: the call fails at once -
       // no search, its path stack as it was - and the decision goes on to the push. It is the mover of that record: an
       // order-less unit on a pushed path is Moving (MOVEMENT 5.2).
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(20, cell_center(15, 20), 1)));  // the pusher, on a forced path (decision step 2)
        CHECK(sys.add_unit(t20_vehicle(21, cell_center(22, 20), 1)));  // pushed aside
        SimRng rng(302);
        MoveOrder o;
        o.units = {20};
        o.destination = cell_center(40, 20);
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 400 && !sys.view(21).pushed; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(21).pushed && sys.view(21).push_kind == 1 && !sys.view(21).has_goal && sys.stats().pushes == 1);
        const std::vector<V2> top(sys.top_path(21).begin(), sys.top_path(21).end());
        CHECK(top.size() >= 2);
        // the pusher stops (it is no mover any more); a third vehicle now stands where the pushed one is heading. s78n t20
        // batch 1 (MOVEMENT §5.4.1 0x684710, §5.4.4 item 5): the destination of the pushed unit's path stack (its top
        // record) is blocked, so its record cuts that push path short of the vehicle (the early arrival) - no second push
        // (before: the top record never counted as blocked, and the third vehicle was pushed in turn)
        const ObjectId pusher = 20;
        sys.stop(std::span<const ObjectId>(&pusher, 1));
        CHECK(sys.add_unit(t20_vehicle(22, top.back(), 1)));
        const uint32_t created0 = sys.stats().collisions_created;
        bool svs = false;
        for (uint32_t n = 0; n < 300 && (n < 5 || sys.view(21).pushed); ++n, ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& c : sys.collisions().list())
                svs = svs || (c.kind == CollisionKind::StoppedVsStopped && (c.a == 21 || c.b == 21));
        }
        const UnitView x = sys.view(21);
        // (the cut end lies behind the unit here: the record is finished at once and the unit brakes where it is)
        const bool shorter = distance(x.pos, top.back()) > 20.0f;
        const float gap = foot_gap(foot_rect(x.pos, x.yaw, 10.0f, 6.0f), foot_rect(sys.view(22).pos, sys.view(22).yaw, 10.0f, 6.0f));
        std::printf("t20 batch 1 cut top record: the pushed unit stopped %.2f from the vehicle on its push end (gap %.2f)\n",
                    static_cast<double>(distance(x.pos, top.back())), static_cast<double>(gap));
        CHECK(sys.stats().pushes == 1 && sys.stats().early_arrivals == 1 && shorter && !x.pushed && x.speed == 0.0f);
        CHECK(!sys.view(22).pushed && sys.view(22).pos == top.back() && gap >= 5.0f && !svs);
        CHECK(sys.stats().collisions_created > created0 && sys.stats().dynamic_repaths == 0);
    }

    {  // s78n t20 batch 1 item 1.2 (MOVEMENT §5.4.6 0x5c69f0, the MovingVsStopped answer in state 1): a two-vehicle
       // formation, max speeds 2.0 and 1.0; a friendly vehicle stands on the fast one's path. While the fast one walks its
       // detour, the record answers it the formation-follow speed min(max speed, Vf x L / D) of its top path, and its
       // speed never exceeds that (before: no cap - it walked the detour at 2.0).
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, cell_center(15, 20), 1, 10.0f, 6.0f, 2.0f)));
        CHECK(sys.add_unit(t20_vehicle(12, cell_center(15, 23), 1, 10.0f, 6.0f, 1.0f)));
        SimRng rng(303);
        MoveOrder o;
        o.units = {10, 12};
        o.destination = cell_center(45, 21);
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 200 && sys.land_path(10).size() < 2; ++f) sys.tick(f, rng, serial_for);
        const std::vector<V2> path(sys.land_path(10).begin(), sys.land_path(10).end());
        CHECK(path.size() >= 2 && sys.view(10).formation_speed < 1.0e30f);
        // the stander: 120 along the fast unit's path from its position
        V2 at = sys.view(10).pos;
        float left = 120.0f;
        for (size_t i = 1; i < path.size() && left > 0.0f; ++i) {
            const float l = distance(at, path[i]);
            if (l <= 0.0f) continue;
            at = l <= left ? path[i] : at + (path[i] - at) * (left / l);
            left -= std::min(l, left);
        }
        CHECK(sys.add_unit(t20_vehicle(11, at, 1)));
        uint32_t frames = 0;
        bool capped = true, under = true;
        float max_speed = 0.0f, answer = 0.0f;
        for (uint32_t n = 0; n < 600 && !sys.view(10).arrived; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView v = sys.view(10);
            max_speed = v.max_speed;
            if (!v.pushed || v.push_kind != 2) continue;
            const CollisionObject* rec = nullptr;
            for (const CollisionObject& c : sys.collisions().list())
                if (c.a == 10 && c.b == 11 && c.kind == CollisionKind::MovingVsStopped && c.state == CollisionState::PathAround) rec = &c;
            if (!rec) continue;
            const std::span<const V2> top = sys.top_path(10);
            float len = 0.0f;
            for (size_t i = 1; i < top.size(); ++i) len += distance(top[i - 1], top[i]);
            const float expect = std::min(v.max_speed, v.formation_speed * len / distance(top.front(), top.back()));
            capped = capped && std::fabs(rec->speed_a - expect) <= 1e-4f * expect;
            under = under && v.speed <= rec->speed_a + 1e-4f;
            answer = rec->speed_a;
            ++frames;
        }
        std::printf("t20 batch 1 follow speed: %u detour frames, answer %.3f (max %.3f, formation %.3f)\n", frames,
                    static_cast<double>(answer), static_cast<double>(max_speed), static_cast<double>(sys.view(10).formation_speed));
        CHECK(frames > 0 && capped && under && answer < max_speed);
    }
}

// s78n t20 item 4 (MOVEMENT 5.4.4): the early-arrival cut. In every case the blocker appears after the order was composed
// (the destination check of the order would move the goal off a unit that already stands there).
void test_t20_early_arrival() {
    const float mult = 1.56f;
    const V2 start = cell_center(15, 20), dest = cell_center(30, 20);  // (300, 400) -> (600, 400)
    // The spec's scan for a mover box (thx + 5, thy) walking +x to `dest` against one standing rectangle (bhx, bhy) at
    // `bp`: s = 0 at the path end, stepping back 5; the first point whose box does not even touch the rectangle and that
    // lies within `range` of `tgt` (range < 0: no target). Returns the distance of that point from the path end, -1 none.
    const auto first_clear = [&](float thx, float thy, V2 bp, float bhx, float bhy, float from_x, float range, V2 tgt) {
        for (float back = 0.0f;; back += 5.0f) {
            const float x = std::max(dest.x - back, from_x);
            const bool touch = std::fabs(x - bp.x) <= thx + 5.0f + bhx && std::fabs(dest.y - bp.y) <= thy + bhy;
            const bool in_range = range < 0.0f || distance(V2{x, dest.y}, tgt) <= range;
            if (!touch && in_range) return dest.x - x;
            if (x == from_x) return -1.0f;
        }
    };
    // The spare point 0x5c6ba0 of a mover (rect (hx, hy), FormationSpacing `spacing`) whose path ends on a standing
    // rectangle `rec`: no path point is left past the unit and the two spots 10 and 20 past the end still touch it, so it
    // is the ring spot around the end - the first sample not behind the end whose distance to the rectangle's centre
    // clears r + max(6 x spacing, 0.25 r) + the rectangle's directional radius.
    const auto ring_spot = [&](float hx, float hy, float spacing, const FootRect& rec) {
        const float r = std::sqrt(hx * hx + hy * hy);
        RingSetup s;
        s.centre = dest;
        s.dir = {1.0f, 0.0f};
        s.r = r;
        V2 out = dest;
        (void)ring_search(s, [&](V2 p, uint32_t) { return p.x - dest.x >= 0.0f && !ring_blocked(p, r + std::max(6.0f * spacing, 0.25f * r), rec); }, out);
        return out;
    };
    struct Out {
        bool arrived = false, around = false, pushed = false;
        V2 end, path_end;
        MovementStats st;
        float min_gap = 3.0e38f;
        uint32_t cut_frame = 0, record_frames_after = 0;
        float speed_before = 0.0f, speed_after = 0.0f;
    };
    // what: 0 the plain cut (FormationSpacing 2.0: the spare point lies farther from the end than the stop point);
    //       1 a spare point nearer than the stop point (FormationSpacing 0): no cut, Path Around to it;
    //       2 a target 100 past the goal, range 120: no point within range is clear -> no cut, Path Around toward the target;
    //       3 the blocker and a second standing unit on the mover itself when it is 30 short: even its own position fails
    const auto vehicle_case = [&](int what) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc m = t20_vehicle(10, start, 1);
        m.formation_spacing = what == 0 || what == 2 || what == 3 ? 2.0f : 0.0f;
        m.accel = m.decel = 0.05f;
        CHECK(sys.add_unit(m));
        SimRng rng(401);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        sys.move(o, rng, 1);
        bool placed = false;
        for (uint32_t f = 1; f < 1500 && !r.arrived; ++f) {
            const UnitView v0 = sys.view(10);
            // (the standing unit is (12, 6): no test box touches it exactly, so float rounding decides nothing)
            // case 0: late (50 before the goal), so that the cut's nearer braking curve binds on the cut frame
            if (what != 3 && !placed && v0.pos.x > (what == 0 ? dest.x - 50.0f : start.x + 60.0f)) {  // the order is composed and under way
                CHECK(sys.add_unit(t20_vehicle(11, dest, 1, 12.0f, 6.0f)));
                placed = true;
            }
            if (what == 3 && !placed && v0.pos.x >= dest.x - 30.0f) {
                CHECK(sys.add_unit(t20_vehicle(11, dest, 1, 12.0f, 6.0f)));
                CHECK(sys.add_unit(t20_vehicle(12, v0.pos, 1)));
                placed = true;
            }
            if (what == 2) {
                MovementSystem::CollisionInfo ci;
                ci.target = 99;  // (no such movement unit: a structure 100 past the goal)
                ci.target_pos = {dest.x + 100.0f, dest.y};
                ci.attack_range = 120.0f;
                CHECK(sys.set_collision_info(10, ci));
            }
            const uint32_t ea0 = sys.stats().early_arrivals;
            sys.tick(f, rng, serial_for);
            const UnitView v = sys.view(10);
            r.arrived = v.arrived;
            r.pushed = r.pushed || v.pushed;
            if (sys.stats().early_arrivals > ea0) {
                r.cut_frame = f;
                r.speed_before = v0.speed;
                r.speed_after = v.speed;
            }
            for (const CollisionObject& c : sys.collisions().list()) {
                if (c.a != 10 || c.b != 11) continue;
                r.around = r.around || c.state == CollisionState::PathAround;
                if (r.cut_frame != 0) ++r.record_frames_after;
            }
            if (placed) r.min_gap = std::min(r.min_gap, foot_gap(foot_rect(v.pos, v.yaw, 10.0f, 6.0f), foot_rect(dest, 0.0f, 12.0f, 6.0f)));
        }
        r.end = sys.view(10).pos;
        r.path_end = sys.land_path(10).empty() ? V2{} : sys.land_path(10).back();
        r.st = sys.stats();
        return r;
    };
    const FootRect blocker = foot_rect(dest, 0.0f, 12.0f, 6.0f);
    {  // (i) the cut: the box (10 + 5, 6) touches the standing (12, 6) up to 27 before it, so the first clear point (steps
       // of 5) is 30 before the goal; the path loses 30 + 3 and ends 33 before the goal
        const float back = first_clear(10.0f, 6.0f, dest, 12.0f, 6.0f, start.x, -1.0f, {});
        CHECK(back == 30.0f);
        const V2 spare = ring_spot(10.0f, 6.0f, 2.0f, blocker);
        CHECK(distance(spare, dest) >= back);  // the spare point does not veto the stop point
        const Out r = vehicle_case(0);
        CHECK(r.arrived && r.st.early_arrivals == 1 && r.st.dynamic_repaths == 0 && r.st.pushes == 0 && !r.around && !r.pushed);
        // s78n t26.4 (MOVEMENT 3.5.2 C.4 / C.11): no snap - the cut path ends 33 before the goal; the unit, past its rest
        // test when the cut came, crosses the last line and coasts along its last displacement to 0x15 (v^2 / (2a) at most)
        CHECK(near(r.path_end.x, dest.x - back - 3.0f, 1e-3f) && near(r.path_end.y, dest.y, 1e-3f) && near(r.end.y, dest.y, 1e-3f));
        CHECK(r.end.x > r.path_end.x - 5.0f && r.end.x < r.path_end.x + mult * mult / (2.0f * 0.05f));
        CHECK(r.record_frames_after == 0);  // Resolved in the decision: gone the same frame
        // s78n t26.2 (MOVEMENT §3.5.2 B.5 / B.7 / C.11, §5.4.4 item 6): the "at once" flag only takes a lower target in
        // one frame, and the cut frame has none - the new last waypoint's fraction is 1.0 (no brake-ahead); the braking
        // is End Move's T = a from the rest test on (the t20 sqrt(2 a rest) curve was a port stand-in)
        CHECK(r.speed_after == r.speed_before && near(r.speed_before, mult, 1e-4f));  // at the full 1 x 1.56
        CHECK(r.min_gap >= 5.0f + 3.0f - 1e-3f);  // its front stays 5 + 3 short of the standing footprint
        std::printf("t20 early arrival: cut %.1f, stopped at %.3f, speed %.3f -> %.3f on the cut frame\n", static_cast<double>(back + 3.0f),
                    static_cast<double>(r.end.x), static_cast<double>(r.speed_before), static_cast<double>(r.speed_after));
    }
    {  // (iv) the spare point is nearer the goal than the stop point (23.8 against 30): no cut - the path is not touched
       // and the unit paths around to the spare point, where it arrives
        const V2 spare = ring_spot(10.0f, 6.0f, 0.0f, blocker);
        CHECK(distance(spare, dest) < 30.0f && near(distance(spare, dest), 23.8f, 1e-3f));
        const Out r = vehicle_case(1);
        CHECK(r.arrived && r.st.early_arrivals == 0 && r.around && r.pushed && r.st.dynamic_repaths >= 1);
        CHECK(r.path_end == dest);  // the ordered path was never cut
        // s78n t26.4 (MOVEMENT 3.5.2 C.4 / C.11): the record over the started order path ends with no End Move; the
        // walk pops into the order path, already past its last line, and coasts to 0x15 - within v^2 / (2a) of the spare
        CHECK(distance(r.end, spare) < mult * mult / (2.0f * 0.05f) && r.min_gap > 0.0f);
        std::printf("t20 early arrival: Path Around to the spare point, coasted to (%.2f, %.2f)\n", static_cast<double>(r.end.x),
                    static_cast<double>(r.end.y));
        std::printf("t20 early arrival: spare point (%.2f, %.2f) nearer than the stop point - Path Around to it\n", static_cast<double>(spare.x),
                    static_cast<double>(spare.y));
    }
    {  // (iii) the target clause: with a target 100 past the goal and range 120 no clear point is in range (the nearest
       // clear one is 130 from it): no cut. The detour's rejoin point lies past the path end toward the target, 10 apart:
       // 10 and 20 still touch the standing unit, 30 is free
        CHECK(first_clear(10.0f, 6.0f, dest, 12.0f, 6.0f, start.x, 120.0f, V2{dest.x + 100.0f, dest.y}) < 0.0f);
        const Out r = vehicle_case(2);
        CHECK(r.arrived && r.st.early_arrivals == 0 && r.around && r.path_end == dest);
        // s78n t26.4: the same coast past the rejoin point (dest + 30), no snap
        CHECK(distance(r.end, V2{dest.x + 30.0f, dest.y}) < mult * mult / (2.0f * 0.05f));
    }
    {  // (vi) even the mover's own position fails (a second unit stands on it): nothing is cut
        const Out r = vehicle_case(3);
        CHECK(r.st.early_arrivals == 0 && r.path_end == dest);
    }
    {  // (ii) a squad container: the box is its TYPE's extents (0, 0) + 5 along the path, not its joint slot box. A squad
       // (joint box 15 x 15) now stands 1.5 past the goal. The destination counts as blocked while the mover's joint box
       // at the path end overlaps it (centres within 30); each decision then scans back from the path end for the first
       // point whose line box (5, 0) clears the standing box (centres more than 20 apart) and cuts 3 more than that: 600
       // -> 577 (the point 580), -> 574, -> 571, where the boxes are 30.5 apart. (t18's joint-box rule stopped it 43 short.)
        const float bx = dest.x + 1.5f;
        float want_end = dest.x;
        uint32_t want_cuts = 0;
        while (bx - want_end <= 30.0f) {
            float p = want_end;
            while (bx - p <= 20.0f) p -= 5.0f;
            want_end = p - 3.0f;
            ++want_cuts;
        }
        CHECK(want_cuts == 3 && want_end == 571.0f);
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t20_squad(sys, 100, start, 1);
        SimRng rng(402);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {101};
        o.destination = dest;
        sys.move(o, rng, 2);
        bool placed = false, arrived = false;
        for (uint32_t f = 2; f < 1500 && !arrived; ++f) {
            if (!placed && sys.view(100).pos.x > start.x + 60.0f) {
                t20_squad(sys, 200, {bx, dest.y}, 1);
                placed = true;
            }
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
        }
        const float x = sys.view(100).pos.x;
        CHECK(arrived && sys.stats().early_arrivals == want_cuts && sys.stats().dynamic_repaths == 0 && sys.stats().pushes == 0);
        CHECK(near(sys.land_path(100).back().x, want_end, 1e-3f));
        CHECK(x > want_end - 5.0f && x < want_end + 1.0f);  // it brakes to a stop within 5 before the end (or a rate step past it)
        std::printf("t20 early arrival: squad path end %.2f after %u cuts, stopped at %.2f\n", static_cast<double>(sys.land_path(100).back().x),
                    sys.stats().early_arrivals, static_cast<double>(x));
    }
    {  // (v) a formation: the leader's cut also shortens the order paths of the members whose slot is behind its own
       // within the two half widths sideways (the same amount), and leaves the members in other lanes alone. Five
       // vehicles: three ranks by attack range (s78n t26 review round 2: the host's 0x3749d0 query, 50 / 60 / 70 here,
       // MOVEMENT §4.3 correction [H]; the max speeds were the stand-in) -> rows of 1, 1 and 3; the row of three puts one member
       // in the leader's lane and two far to the sides. (The leader starts 80 ahead of the others: its record is decided
       // inside the 160 x 160 candidate box, about 90 before its slot, and the members behind it then still have more
       // than the cut left to walk. The port's members path to their own slots, so one that is nearer its slot than the
       // cut stops where it stands.)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const float speeds[5] = {0.5f, 0.6f, 0.7f, 0.7f, 0.7f};
        sys.set_attack_range_query([&speeds](ObjectId id) { return id >= 10 && id < 15 ? 100.0f * speeds[id - 10] : 0.0f; });
        const V2 starts[5] = {cell_center(16, 20), cell_center(12, 20), cell_center(12, 17), cell_center(12, 22), cell_center(12, 25)};
        for (ObjectId i = 0; i < 5; ++i) {
            UnitDesc d = t20_vehicle(10 + i, starts[i], 1, 10.0f, 6.0f, speeds[i]);
            d.formation_spacing = 2.0f;
            CHECK(sys.add_unit(d));
        }
        SimRng rng(403);
        MoveOrder o;
        o.units = {10, 11, 12, 13, 14};
        o.destination = cell_center(40, 20);
        sys.move(o, rng, 1);
        V2 goal[5];
        uint32_t form = sys.view(10).form_id;
        bool one_form = form != 0;
        for (ObjectId i = 0; i < 5; ++i) {
            goal[i] = sys.view(10 + i).goal;
            one_form = one_form && sys.view(10 + i).form_id == form;
        }
        CHECK(one_form);
        // the leader is the front slot: the shortest range (rank order); its lane = the members at its y
        int lead = 0;
        for (int i = 1; i < 5; ++i)
            if (goal[i].x > goal[lead].x) lead = i;
        CHECK(lead == 0);
        std::vector<int> lane, side;
        for (int i = 0; i < 5; ++i) {
            if (i == lead) continue;
            (std::fabs(goal[i].y - goal[lead].y) <= 12.0f ? lane : side).push_back(i);  // two half widths: 6 + 6
        }
        CHECK(lane.size() == 2 && side.size() == 2);
        bool placed = false;
        uint32_t f = 1;
        for (; f < 2500 && sys.stats().early_arrivals == 0; ++f) {
            if (!placed && sys.view(10).speed > 0.0f && sys.view(10).pos.x > cell_center(20, 20).x) {
                CHECK(sys.add_unit(t20_vehicle(30, goal[lead], 1, 12.0f, 6.0f)));  // a unit now stands on the leader's slot
                placed = true;
            }
            sys.tick(f, rng, serial_for);
        }
        CHECK(placed && sys.stats().early_arrivals == 1);
        // the leader's stop point is 30 before the standing unit: every cut path lost 33 from its end (s78n t20 batch 1:
        // 0x54e780 keeps total - 33 measured from the path's first point, the index untouched) - the member one row back
        // had more than that left and walks on; the member two rows back had less: its record is left on its last
        // waypoint (it is past the new end's line), so it is finished - arrived, braking where it is; the two beside the
        // lane keep their paths. s78p (b0049): the members walk their lanes (one formation path, each lane started at the
        // foot of the member's position), so both lane members still have more than 33 left when the leader stops and
        // both walk on.
        bool cut = near(distance(sys.land_path(10 + lead).back(), goal[lead]), 33.0f, 1e-2f);
        uint32_t exact = 0, walking = 0, finished = 0;
        for (int i : lane) {
            const V2 end = sys.land_path(10 + i).back();
            if (near(distance(end, goal[i]), 33.0f, 1e-2f)) ++exact;
            const UnitView v = sys.view(10 + i);
            if (v.has_goal) ++walking;
            else if (v.arrived && !v.pushed) ++finished;
        }
        bool kept = true;
        for (int i : side) kept = kept && sys.land_path(10 + i).back() == goal[i];
        CHECK(cut && exact == 2 && walking == 2 && finished == 0 && kept);
        std::printf("t20 early arrival: formation - leader cut 33, lane members cut 33: %u (walking on %u, finished %u), side lanes kept %d\n",
                    exact, walking, finished, kept ? 1 : 0);
        bool all = false;
        for (; f < 4000 && !all; ++f) {
            sys.tick(f, rng, serial_for);
            all = true;
            for (ObjectId i = 0; i < 5; ++i) all = all && sys.view(10 + i).arrived;
        }
        CHECK(all);
    }
    {  // (vii) the record's obstacle lies behind the stop point: the path is cut all the same, but the call answers false
       // and the decision goes on - the mover paths around the obstacle and arrives at the cut path's end
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc m = t20_vehicle(10, start, 1);
        m.formation_spacing = 2.0f;
        CHECK(sys.add_unit(m));
        SimRng rng(404);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        sys.move(o, rng, 1);
        bool placed = false, arrived = false, around = false;
        for (uint32_t f = 1; f < 1500 && !arrived; ++f) {
            // (late: the mover must be on its final leg when the record with the nearer unit is decided)
            if (!placed && sys.view(10).pos.x >= dest.x - 90.0f) {
                CHECK(sys.add_unit(t20_vehicle(11, {dest.x - 60.0f, dest.y}, 1)));  // on the path, 60 before the goal
                CHECK(sys.add_unit(t20_vehicle(12, dest, 1, 12.0f, 6.0f)));         // on the goal
                placed = true;
            }
            sys.tick(f, rng, serial_for);
            arrived = sys.view(10).arrived;
            for (const CollisionObject& c : sys.collisions().list()) around = around || (c.a == 10 && c.b == 11 && c.state == CollisionState::PathAround);
        }
        // the stop point (30 before the goal) clears both units; the obstacle (60 before the goal) lies behind it: the cut
        // is made, the answer is false (no early arrival counted) and the record goes to Path Around
        CHECK(arrived && around && sys.stats().dynamic_repaths >= 1 && sys.stats().early_arrivals == 0);
        // s78n t26.4 (MOVEMENT 3.5.1 item 1): arrived where 0x15 stopped it - within 5 before the cut path's last line,
        // still off to the detour's side (the lane term had not brought it back yet; no snap)
        CHECK(near(sys.land_path(10).back().x, dest.x - 33.0f, 1e-3f) && sys.view(10).pos.x > dest.x - 33.0f - 5.0f &&
              distance(sys.view(10).pos, V2{dest.x - 33.0f, dest.y}) < 13.5f + 5.0f);
        CHECK(sys.view(11).pos == (V2{dest.x - 60.0f, dest.y}) && sys.view(12).pos == dest);
    }
}

// s78n t20 item 5 (MOVEMENT 5.4.5): the decision's target clause (step 4), the hero-clash test (step 8) and the crusher's
// record with a single unit it crushes.
void test_t20_decision_leftovers() {
    const float mult = 1.56f;
    const V2 start = cell_center(15, 20), dest = cell_center(30, 20);
    {  // Step 4, a melee squad against its target: a squad ordered onto a standing enemy infantry unit that stands on its
       // goal. Without a target the destination is blocked and the path is cut (step 6); with the unit as its target and
       // a short-range (melee) squad it is Repulsion at once: no cut, it walks on to its goal.
        struct Out {
            bool arrived = false, repulsion = false;
            V2 end;
            MovementStats st;
        };
        const auto run = [&](bool melee_target) {
            Out r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            t20_squad(sys, 100, start, 1);
            SimRng rng(501);
            sys.tick(1, rng, serial_for);
            MoveOrder o;
            o.units = {101};
            o.destination = dest;
            sys.move(o, rng, 2);
            bool placed = false;
            for (uint32_t f = 2; f < 1500 && !r.arrived; ++f) {
                if (!placed && sys.view(100).pos.x > start.x + 60.0f) {
                    UnitDesc x = unit(70, dest, 2, 1.0f, 6.0f, LocomotorKind::Default);
                    x.owner = 2;
                    CHECK(sys.add_unit(x));
                    placed = true;
                }
                if (melee_target && placed) {
                    MovementSystem::CollisionInfo ci;
                    ci.target = 70;
                    ci.target_pos = dest;
                    ci.attack_range = 40.0f;  // (longer than the unit's footprint radius 6: only "short-range" makes the clause)
                    ci.short_range = true;
                    CHECK(sys.set_collision_info(100, ci));
                }
                sys.tick(f, rng, serial_for);
                r.arrived = sys.view(100).arrived;
                for (const CollisionObject& c : sys.collisions().list())
                    r.repulsion = r.repulsion || (c.a == 100 && c.b == 70 && c.state == CollisionState::Repulsion);
            }
            r.end = sys.view(100).pos;
            r.st = sys.stats();
            return r;
        };
        const Out plain = run(false), melee = run(true);
        CHECK(plain.arrived && plain.st.early_arrivals >= 1 && !plain.repulsion && plain.end.x < dest.x - 8.0f);
        CHECK(melee.arrived && melee.st.early_arrivals == 0 && melee.repulsion && std::fabs(melee.end.x - dest.x) < 8.0f);
        CHECK(melee.st.dynamic_repaths == 0 && melee.st.pushes == 0);
        std::printf("t20 decision: melee squad on its target - stopped %.2f / %.2f before it (no target / target)\n",
                    static_cast<double>(dest.x - plain.end.x), static_cast<double>(dest.x - melee.end.x));
    }
    {  // Step 4, a ranged infantry unit whose attack range (5) is shorter than its target's footprint radius (a standing
       // vehicle (10, 6): 11.66): Repulsion. With a longer range, or another target, it paths around the vehicle.
        // what: 0 no target info, 1 target = the vehicle with range 5, 2 the same with range 20
        const auto run = [&](int what, bool& repulsion, bool& around) {
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            CHECK(sys.add_unit(unit(1, start, 2, 1.0f, 3.0f, LocomotorKind::Default)));
            CHECK(sys.add_unit(t20_vehicle(11, cell_center(22, 20), 2)));
            if (what != 0) {
                MovementSystem::CollisionInfo ci;
                ci.target = 11;
                ci.target_pos = cell_center(22, 20);
                ci.attack_range = what == 1 ? 5.0f : 20.0f;
                CHECK(sys.set_collision_info(1, ci));
            }
            SimRng rng(502);
            MoveOrder o;
            o.units = {1};
            o.destination = dest;
            sys.move(o, rng, 1);
            bool arrived = false;
            for (uint32_t f = 1; f < 1500 && !arrived; ++f) {
                sys.tick(f, rng, serial_for);
                arrived = sys.view(1).arrived;
                for (const CollisionObject& c : sys.collisions().list()) {
                    repulsion = repulsion || (c.a == 1 && c.b == 11 && c.state == CollisionState::Repulsion);
                    around = around || (c.a == 1 && c.b == 11 && c.state == CollisionState::PathAround);
                }
            }
            CHECK(arrived);
            return sys.stats().dynamic_repaths;
        };
        bool r0 = false, a0 = false, r1 = false, a1 = false, r2 = false, a2 = false;
        const uint32_t n0 = run(0, r0, a0), n1 = run(1, r1, a1), n2 = run(2, r2, a2);
        CHECK(!r0 && a0 && n0 >= 1);  // no target: Path Around (the classes are not both Infantry)
        CHECK(r1 && !a1 && n1 == 0);  // range 5 < 11.66: Repulsion at once
        CHECK(!r2 && a2 && n2 >= 1);  // range 20: no clause
    }
    {  // Step 8, the hero clash: a bike walking a pushed path (no order: it cannot path around) into a standing friendly
       // bike that is in a hero clash - no push; both classes are at most Bike, so the record is Try Again Later (the mover
       // brakes to 0) and is re-decided every 30 frames. Once the clash is over the next decision pushes the blocker.
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(20, cell_center(15, 20), 1)));  // the first pusher, on a forced path
        UnitDesc bike = t20_vehicle(21, cell_center(22, 20), 1);
        bike.collision_class = kUccBike;
        bike.accel = bike.decel = 0.05f;
        CHECK(sys.add_unit(bike));
        SimRng rng(503);
        MoveOrder o;
        o.units = {20};
        o.destination = cell_center(40, 20);
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 400 && !sys.view(21).pushed; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(21).pushed && sys.stats().pushes == 1);
        const ObjectId pusher = 20;
        sys.stop(std::span<const ObjectId>(&pusher, 1));
        const std::vector<V2> top(sys.top_path(21).begin(), sys.top_path(21).end());
        CHECK(top.size() >= 2);
        // s78n t20 batch 1: the blocker stands across the push path's middle (12 to its side, its long axis across the
        // path), clear of the path's end - a blocker on the end would be a blocked destination (the push record is cut,
        // MOVEMENT 5.4.4 item 5) and the decision would never reach step 8
        const V2 pd = normalize_retail(top.back() - top.front()), side = perp_left(pd);
        UnitDesc blocker = t20_vehicle(22, (top.front() + top.back()) * 0.5f + side * 12.0f, 1);
        blocker.yaw_deg = heading_deg(side);
        blocker.collision_class = kUccBike;
        CHECK(sys.add_unit(blocker));
        MovementSystem::CollisionInfo clash;
        clash.hero_clash = true;
        CHECK(sys.set_collision_info(22, clash));
        uint32_t tal = 0;
        float last_speed = 1.0f;
        for (uint32_t n = 0; n < 70; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& c : sys.collisions().list())
                if (c.a == 21 && c.b == 22 && c.state == CollisionState::TryAgainLater) ++tal;
            last_speed = sys.view(21).speed;
        }
        CHECK(tal >= 60 && sys.stats().pushes == 1 && !sys.view(22).pushed);  // two decisions and more: never pushed
        CHECK(last_speed == 0.0f && sys.view(21).pushed);                     // held (braked at its rate), still on its path
        CHECK(sys.set_collision_info(22, MovementSystem::CollisionInfo{}));   // the clash is over
        uint32_t waited = 0;
        for (; waited < 40 && sys.stats().pushes == 1; ++waited, ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.stats().pushes == 2 && sys.view(22).pushed && waited <= 31);  // at the next retry
        std::printf("t20 decision: hero clash - Try Again Later %u frames, pushed %u frames after the clash ended\n", tal, waited);
    }
    {  // A crusher against a standing enemy single infantry unit it can crush (MOVEMENT 5.4.5 item 4): a record is made.
       // what: 0 the victim scatters -> exactly one push per record, onto a crush-flee path (kind 7), the record resolved;
       //       1 it does not scatter -> no push at all, the record is in state 4 until the crusher has passed;
       //       2 it scatters but cannot walk -> the one push fails, state 4, no second attempt
        struct Out {
            bool arrived = false, crusher_state = false, fled = false;
            float min_speed = 3.0e38f;
            uint32_t records_end = 0;
            MovementStats st;
        };
        const auto run = [&](int what) {
            Out r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            if (what == 1) sys.set_scatters_fn([](int32_t) { return false; });
            UnitDesc cr = t20_vehicle(60, cell_center(8, 20), 2);
            cr.crusher = true;
            CHECK(sys.add_unit(cr));
            UnitDesc v = unit(70, cell_center(25, 20), 2, what == 2 ? 0.0f : 1.0f, 3.0f, LocomotorKind::Default);
            v.owner = 1;
            v.squashable = true;
            CHECK(sys.add_unit(v));
            SimRng rng(504);
            MoveOrder o;
            o.units = {60};
            o.destination = cell_center(45, 20);
            sys.move(o, rng, 1);
            bool cruising = false;
            for (uint32_t f = 1; f < 1500 && !r.arrived; ++f) {
                sys.tick(f, rng, serial_for);
                const UnitView c = sys.view(60);
                r.arrived = c.arrived;
                cruising = cruising || near(c.speed, mult, 1e-5f);
                if (cruising && c.pos.x < cell_center(44, 20).x) r.min_speed = std::min(r.min_speed, c.speed);
                r.fled = r.fled || (sys.view(70).pushed && sys.view(70).push_flee);
                for (const CollisionObject& co : sys.collisions().list())
                    r.crusher_state = r.crusher_state || (co.a == 60 && co.b == 70 && co.state == CollisionState::Crusher);
            }
            r.records_end = static_cast<uint32_t>(sys.collisions().list().size());
            r.st = sys.stats();
            return r;
        };
        const Out scat = run(0), stay = run(1), stuck = run(2);
        CHECK(scat.arrived && scat.st.pushes >= 1 && scat.fled && scat.st.push_failures == 0 && near(scat.min_speed, mult, 1e-5f));
        CHECK(stay.arrived && stay.crusher_state && stay.st.pushes == 0 && stay.st.push_failures == 0 && stay.records_end == 0);
        CHECK(near(stay.min_speed, mult, 1e-5f));
        CHECK(stuck.arrived && stuck.crusher_state && stuck.st.pushes == 0 && stuck.st.push_failures == 1 && near(stuck.min_speed, mult, 1e-5f));
        CHECK(scat.st.dynamic_repaths == 0 && stay.st.dynamic_repaths == 0 && scat.st.early_arrivals == 0 && stay.st.early_arrivals == 0);
        std::printf("t20 decision: crusher - scattering victim pushed %u time(s), non-scattering: state 4, stuck: %u failed push\n",
                    scat.st.pushes, stuck.st.push_failures);
    }
    // s78n t20 batch 1 item 1.3 (MOVEMENT §5.4.1 "The impassable-cell rule in full"): validity, first in every state - a
    // participant with a locomotor whose top path is not forced and that stands on a cell impassable for it is stopped
    // (its path stack emptied; speed 0 and state 0x16 at once when its path owner is none or this record) and the record
    // ends; participant b is tested only when a passed.
    {  // (a) a standing obstacle on a stamped cell beside the mover's line: stopped (a no-op, it has no path) at every
       // record - the mover never gets an answer from it (no detour, no hold) and walks on past it
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        const V2 bp{cell_center(22, 20).x, 412.0f};  // cell (22, 21); its footprint reaches y = 406, the mover's y = 406
        CHECK(sys.add_unit(t20_vehicle(11, bp, 1)));
        sys.grid().set_base(22, 21, kPassImpassable);
        SimRng rng(511);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        sys.move(o, rng, 1);
        bool arrived = false, held = false, pushed = false, listed = false;
        for (uint32_t f = 1; f < 800 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView v = sys.view(10);
            arrived = v.arrived;
            pushed = pushed || v.pushed;
            held = held || (v.has_goal && v.speed < v.max_speed && v.pos.x > start.x + 20.0f && v.pos.x < dest.x - 20.0f);
            for (const CollisionObject& c : sys.collisions().list()) listed = listed || (c.a == 10 && c.b == 11);
        }
        const MovementStats st = sys.stats();
        std::printf("t20 batch 1 impassable cell: stander on a stamped cell - records made %u, listed after a tick %d\n",
                    st.collisions_created, listed ? 1 : 0);
        CHECK(arrived && !pushed && !held && !listed && st.collisions_created >= 1 && st.dynamic_repaths == 0 && st.pushes == 0);
        CHECK(sys.view(11).pos == bp && !sys.view(11).has_goal);
    }
    {  // (b) the mover itself on a stamped cell (an ordinary path) while it has a record: stopped the same tick - no
       // path, no goal, speed 0, signalled arrived (the formation counts it done)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        CHECK(sys.add_unit(t20_vehicle(11, cell_center(24, 20), 1)));
        SimRng rng(512);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        sys.move(o, rng, 1);
        uint32_t f = 1;
        bool rec = false;
        for (; f < 400 && !rec; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& c : sys.collisions().list()) rec = rec || (c.a == 10 && c.b == 11);
        }
        CHECK(rec && sys.view(10).speed > 0.0f && sys.view(10).has_goal);
        const V2 p = sys.view(10).pos;
        sys.grid().set_base(world_to_cell(p.x), world_to_cell(p.y), kPassImpassable);
        sys.tick(f++, rng, serial_for);
        const UnitView v = sys.view(10);
        CHECK(!v.has_goal && !v.pushed && v.speed == 0.0f && v.arrived && v.pos == p && sys.land_path(10).empty());
        bool listed = false;
        for (const CollisionObject& c : sys.collisions().list()) listed = listed || (c.a == 10 && c.b == 11);
        CHECK(!listed);
    }
    {  // (c) a forced (flag 0x20) path across stamped cells: exempt - the record goes on to decision step 2 (the push)
       // and the mover walks on to its goal
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        CHECK(sys.add_unit(t20_vehicle(11, cell_center(24, 20), 1)));
        for (int32_t x = 16; x <= 23; ++x) sys.grid().set_base(x, 20, kPassImpassable);
        SimRng rng(514);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 1);
        bool arrived = false;
        for (uint32_t f = 1; f < 800 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(10).arrived;
        }
        CHECK(arrived && distance(sys.view(10).pos, dest) < 1.0f && sys.stats().pushes >= 1);
    }
    {  // (d) a unit ordered off a stamped cell: its first leg (the planner's escape from a blocked start) is exempt from
       // the rule ([port] "forced" = a flag-0x20 path or this escape leg), so a record on that leg does not delete it
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        CHECK(sys.add_unit(t20_vehicle(11, cell_center(17, 20) + V2{6.0f, 0.0f}, 1)));
        sys.grid().set_base(15, 20, kPassImpassable);
        SimRng rng(513);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        sys.move(o, rng, 1);
        bool arrived = false, rec_on_cell = false, kept = true;
        for (uint32_t f = 1; f < 1500 && !arrived; ++f) {
            const bool on_cell = !sys.grid().passable_world(sys.view(10).pos, sys.mask_of(10));
            const uint32_t made = sys.stats().collisions_created;
            sys.tick(f, rng, serial_for);
            arrived = sys.view(10).arrived;
            // a record made or listed this tick while the mover started it on the stamped cell
            bool rec = sys.stats().collisions_created != made;
            for (const CollisionObject& c : sys.collisions().list()) rec = rec || c.a == 10 || c.b == 10;
            if (on_cell && rec) {
                rec_on_cell = true;
                kept = kept && sys.view(10).has_goal;
            }
        }
        CHECK(arrived && distance(sys.view(10).pos, dest) < 1.0f && kept);
        std::printf("t20 batch 1 impassable cell: escape leg with a record on it %d, order kept %d\n", rec_on_cell ? 1 : 0, kept ? 1 : 0);
    }
}

// s78n t20 item 1 (MOVEMENT 5.4.3): StoppedVsStopped - who moves, where to, and that it walks there.
void test_t20_stopped_vs_stopped() {
    const float mult = 1.56f;
    const V2 p0 = cell_center(20, 20), p1{p0.x + 8.0f, p0.y};
    const float r = std::sqrt(136.0f);  // footprint radius of (10, 6)
    // what: 0 plain; 1 the cheaper unit (11) has a target; 2 the cheaper unit is deployed
    struct Out {
        uint64_t crc = 0;
        uint8_t master = 9;
        CollisionState state0 = CollisionState::Active;
        bool still0 = false, pushed = false, kind3 = true, gone = false;
        V2 spot, end10, end11;
        std::vector<float> speeds;
        MovementStats st;
    };
    const auto run = [&](int what, const ParallelFor& pf) {
        Out o;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        for (ObjectId id = 10; id <= 11; ++id) {
            UnitDesc d = t20_vehicle(id, id == 10 ? p0 : p1, 1);
            d.accel = d.decel = 0.05f;
            CHECK(sys.add_unit(d));
        }
        if (what == 1) {
            MovementSystem::CollisionInfo ci;
            ci.target = 500;
            ci.target_pos = {p1.x, p1.y + 300.0f};
            ci.attack_range = 400.0f;
            CHECK(sys.set_collision_info(11, ci));
        }
        if (what == 2) CHECK(sys.deploy(11, true, 0) == MovementSystem::DeployResult::Deploying);
        SimRng rng(101);
        sys.tick(1, rng, pf);
        if (sys.collisions().list().size() == 1) {
            o.master = sys.collisions().list()[0].master;
            o.state0 = sys.collisions().list()[0].state;
        }
        const ObjectId mover = o.master == 0 ? 10 : 11;
        // nothing is displaced in one step: the other unit stands, the mover has made at most its first walk step
        o.still0 = sys.view(mover == 10 ? 11 : 10).pos == (mover == 10 ? p1 : p0) && distance(sys.view(mover).pos, mover == 10 ? p0 : p1) <= 0.05f * mult + 1e-4f;
        if (!sys.top_path(mover).empty()) o.spot = sys.top_path(mover).back();
        for (uint32_t f = 2; f < 400 && !o.gone; ++f) {
            const UnitView v = sys.view(mover);
            if (v.pushed) {
                o.pushed = true;
                o.kind3 = o.kind3 && v.push_kind == 3;
                o.speeds.push_back(v.speed);
            }
            sys.tick(f, rng, pf);
            o.gone = sys.collisions().list().empty();
        }
        for (uint32_t f = 400; f < 420; ++f) sys.tick(f, rng, pf);  // no new record afterwards
        o.gone = o.gone && sys.collisions().list().empty();
        o.end10 = sys.view(10).pos;
        o.end11 = sys.view(11).pos;
        o.st = sys.stats();
        StateHasher h;
        sys.hash_units(h);
        o.crc = h.digest();
        return o;
    };
    {  // the plain case: both would back straight away from the other; unit 11 faces that way already
        // costs 0x686b20, restated: v x (d . D) < 0 and |D| = 8 <= R = 2 r -> t = (R - 8) / (8 v); + the turn to d
        const float t = (2.0f * r - 8.0f) / (8.0f * mult), turn_rate = 6.0f * mult;
        const float cost11 = 0.0f / turn_rate + t, cost10 = 180.0f / turn_rate + t;
        CHECK(near(cost11, 1.2277f, 1e-3f) && cost11 <= cost10);  // the cheaper one first: unit 11 (p1)
        // the spot: the first free sample of the ring search around unit 11 along +x (cone 3 pi / 4, outside 0.75);
        // occupied = the other rectangle's centre within r + its directional radius of the sample
        const FootRect other = foot_rect(p0, 0.0f, 10.0f, 6.0f);
        RingSetup rs;
        rs.centre = p1;
        rs.dir = {1.0f, 0.0f};
        rs.r = r;
        rs.cone = 2.3561945f;
        rs.outside = 0.75f;
        V2 want = p1;
        CHECK(ring_search(rs, [&](V2 p, uint32_t) { return !ring_blocked(p, r, other); }, want));
        CHECK(near(want.x, p1.x + 18.0f, 1e-3f) && near(want.y, p1.y, 1e-3f));  // rings 1 .. 13 blocked (8 + 13 <= 21.66), 18 free
        const Out s = run(0, serial_for), p = run(0, make_thread_pf(4));
        CHECK(s.crc == p.crc);
        CHECK(s.master == 1 && s.state0 == CollisionState::Resolving && s.still0);
        CHECK(distance(s.spot, want) < 1e-3f);
        CHECK(s.pushed && s.kind3 && s.gone && s.st.collisions_created == 1 && s.st.pushes == 0);
        // it walks: the speed rises by its acceleration per frame (0.05 x 1.56), no jump
        CHECK(s.speeds.size() >= 5 && near(s.speeds[0], 0.05f * mult, 1e-5f) && near(s.speeds[1], 0.10f * mult, 1e-5f) &&
              near(s.speeds[2], 0.15f * mult, 1e-5f));
        // s78n t26.4 (MOVEMENT 5.4.3 "The move", 3.5.2 C.7): X walks along its facing with the side-step vector away
        // from O (weight 3), so it ends off the straight line, past the spot's line, where 0x15 stops it (no snap); the
        // two footprints are at least the spot's 6 apart
        CHECK(s.end10 == p0 && s.end11.x > want.x - 5.0f && distance(s.end11, want) < 6.0f);
        CHECK(foot_gap(foot_rect(s.end10, 0.0f, 10.0f, 6.0f), foot_rect(s.end11, 0.0f, 10.0f, 6.0f)) >= 6.0f - 1e-3f);
        std::printf("t20 stopped vs stopped: unit 11 walked to (%.2f, %.2f), costs %.3f / %.3f\n", static_cast<double>(s.end11.x),
                    static_cast<double>(s.end11.y), static_cast<double>(cost11), static_cast<double>(cost10));
    }
    {  // exactly one has a target: the one without moves, whatever the costs; it backs straight away (-x), turning first
        const Out s = run(1, serial_for);
        CHECK(s.master == 0 && s.state0 == CollisionState::Resolving && s.gone && s.end11 == p1);
        // s78n t26.4 (MOVEMENT 3.5.2 A, C.6 / C.7): CloseEnoughAngleForMoveStart 180 - it turns on the move (a U-turn arc
        // to the side) and stops past the spot's line (-x), off the straight line
        CHECK(s.end10.x < p0.x - 18.0f + 5.0f && std::fabs(s.end10.y - p0.y) < 18.0f);
        std::printf("t20 stopped vs stopped: target case - unit 10 ended at (%.2f, %.2f)\n", static_cast<double>(s.end10.x),
                    static_cast<double>(s.end10.y));
    }
    {  // a deploying / deployed unit never moves: the other one does
        const Out s = run(2, serial_for);
        CHECK(s.master == 0 && s.gone && s.end11 == p1 && s.end10.x < p0.x - 18.0f + 5.0f && std::fabs(s.end10.y - p0.y) < 18.0f);
    }
    {  // two Infantry-class units that both have targets: nobody moves - Try Again Later, re-decided every 30 frames
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(unit(1, p0, 2, 1.0f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.add_unit(unit(2, {p0.x + 4.0f, p0.y}, 2, 1.0f, 6.0f, LocomotorKind::Default)));
        MovementSystem::CollisionInfo ci;
        ci.target = 500;
        ci.target_pos = {p0.x, p0.y + 200.0f};
        CHECK(sys.set_collision_info(1, ci) && sys.set_collision_info(2, ci));
        SimRng rng(102);
        std::vector<uint32_t> retries;
        bool tal = true, nobody = true;
        for (uint32_t f = 1; f <= 100; ++f) {
            sys.tick(f, rng, serial_for);
            CHECK(sys.collisions().list().size() == 1);
            if (sys.collisions().list().size() != 1) break;
            const CollisionObject& c = sys.collisions().list()[0];
            tal = tal && (c.state == CollisionState::TryAgainLater || c.state == CollisionState::Active);
            nobody = nobody && c.master == 2;
            if (retries.empty() || retries.back() != c.retry) retries.push_back(c.retry);
        }
        CHECK(tal && nobody && retries.size() >= 3);
        for (size_t i = 1; i < retries.size(); ++i) CHECK(retries[i] - retries[i - 1] >= 30 && retries[i] - retries[i - 1] <= 31);
        CHECK(sys.view(1).pos == p0 && sys.view(2).pos == (V2{p0.x + 4.0f, p0.y}) && !sys.view(1).pushed && !sys.view(2).pushed);
        CHECK(sys.stats().collisions_created == 1);
    }
    {  // The cone: X stands in the upper right corner of a dead-end lane (walls right of it and above it, the lane open to
       // the left), overlapping an immobile unit 4 to its left, so its escape direction is +x, into the wall. The only
       // free ground lies behind it, 170 degrees and more off that direction: inside the Infantry cone (pi), outside the
       // cone of every other class (3 pi / 4), whose samples there sit 0.75 from the unit. An Infantry-class X walks to
       // the spot behind; a Vehicle-class X of the same shape finds none (Try Again Later). [port] The record of a
       // failed resolution ends at a retry once the two no longer overlap.
        const auto corner = [&](int32_t cls, bool& moved, bool& tal, float& angle, uint32_t& records_after_apart) {
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(60, 60);
            for (int32_t y = 0; y < 60; ++y)
                for (int32_t x = 0; x < 60; ++x)
                    if (!(y == 20 && x >= 10 && x <= 20)) g.set_base(x, y, kPassImpassable);  // the lane: cells (10 .. 20, 20)
            sys.set_grid(std::move(g));
            sys.set_anim_length_fn(anim_len);
            const V2 xp = cell_center(20, 20) + V2{5.0f, 5.0f};
            UnitDesc x = unit(1, xp, 2, 1.0f, 6.0f, LocomotorKind::Default);
            x.collision_class = cls;
            CHECK(sys.add_unit(x));
            CHECK(sys.add_unit(unit(2, {xp.x - 4.0f, xp.y}, 2, 0.0f, 3.0f, LocomotorKind::Default)));  // cannot walk
            SimRng rng(103);
            moved = tal = false;
            angle = 0.0f;
            uint32_t f = 1;
            for (; f < 200; ++f) {
                sys.tick(f, rng, serial_for);
                if (sys.view(1).pushed && !moved) {
                    moved = true;
                    const V2 d = sys.top_path(1).back() - xp;
                    angle = std::fabs(heading_deg(d));
                }
                for (const CollisionObject& c : sys.collisions().list()) tal = tal || c.state == CollisionState::TryAgainLater;
            }
            // the other unit goes away: a record left in Try Again Later ends at its next retry
            CHECK(sys.teleport(2, cell_center(10, 20), 0.0f, 0.0f, f));
            for (uint32_t n = 0; n < 40; ++n, ++f) sys.tick(f, rng, serial_for);
            records_after_apart = static_cast<uint32_t>(sys.collisions().list().size());
        };
        bool moved_i = false, tal_i = false, moved_v = false, tal_v = false;
        float ang_i = 0.0f, ang_v = 0.0f;
        uint32_t left_i = 9, left_v = 9;
        corner(kUccInfantry, moved_i, tal_i, ang_i, left_i);
        corner(kUccVehicle, moved_v, tal_v, ang_v, left_v);
        CHECK(moved_i && ang_i > 135.0f && ang_i <= 180.0f && left_i == 0);
        CHECK(!moved_v && tal_v && left_v == 0);
        std::printf("t20 stopped vs stopped: cone - Infantry spot %.1f degrees off its escape direction, Vehicle: Try Again Later %d\n",
                    static_cast<double>(ang_i), tal_v ? 1 : 0);
    }
}

// s78n t20 batch 1 item 1.5 (MOVEMENT §5.4.5 item 2, §5.2 item 7, §5.4.3): death-clone obstacles - objects without a
// locomotor in the land layer. Never Moving, never a checker, only ever the obstacle b: a forced path or a failed Path
// Around kills one (the push succeeds; the host removes it after the tick), it never blocks a destination, and a stopped
// unit overlapping one is the unit that moves.
UnitDesc t20_clone(ObjectId id, V2 pos, int32_t cls, float hx, float hy) {
    UnitDesc d = unit(id, pos, 2, 0.0f, 12.0f, LocomotorKind::Default);
    d.collision_class = cls;
    d.hard_x = hx;
    d.hard_y = hy;
    d.owner = 2;
    d.no_locomotor = true;
    d.death_clone_obstacle = true;
    return d;
}

void test_t20_death_clone() {
    const V2 start = cell_center(15, 20), dest = cell_center(30, 20);
    // the host's part: the killed clones leave the movement system after the tick
    const auto remove_killed = [](MovementSystem& sys, std::vector<ObjectId>& killed) {
        const std::vector<ObjectId> ids(sys.push_kills().begin(), sys.push_kills().end());
        for (const ObjectId id : ids) {
            killed.push_back(id);
            CHECK(sys.remove_unit(id));
        }
    };
    const auto clone_checked = [](const MovementSystem& sys, ObjectId clone) {
        for (const CollisionObject& c : sys.collisions().list())
            if (c.a == clone) return true;
        return false;
    };
    {  // (a) a forced (flag 0x20) path: decision step 2 takes b with no locomotor when it is a death clone - the push kills
       // it (in push_kills the frame of the decision) and the mover walks on to its goal
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        const V2 cp = cell_center(24, 20);
        CHECK(sys.add_unit(t20_clone(11, cp, kUccVehicle, 10.0f, 6.0f)));
        SimRng rng(521);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 1);
        std::vector<ObjectId> killed;
        bool arrived = false, checker = false, moved = false, same_tick = false;
        uint32_t kill_frame = 0;
        for (uint32_t f = 1; f < 800 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            if (sys.has_unit(11)) {
                checker = checker || clone_checked(sys, 11);
                moved = moved || sys.view(11).pos != cp || sys.view(11).pushed;
            }
            if (!sys.push_kills().empty() && kill_frame == 0) {
                kill_frame = f;
                for (const CollisionObject& c : sys.collisions().list())  // decided (Resolved) the frame it was made
                    same_tick = same_tick || (c.a == 10 && c.b == 11 && c.state == CollisionState::Resolved);
            }
            remove_killed(sys, killed);
            arrived = sys.view(10).arrived;
        }
        CHECK(arrived && distance(sys.view(10).pos, dest) < 1.0f && killed == std::vector<ObjectId>{11} && kill_frame > 0);
        CHECK(!checker && !moved && sys.stats().pushes == 0 && sys.stats().push_failures == 0 && !sys.has_unit(11));
        std::printf("t20 batch 1 death clone: forced path - killed at frame %u (resolved that frame %d), mover arrived %d\n",
                    kill_frame, same_tick ? 1 : 0, arrived ? 1 : 0);
    }
    {  // (b) an Infantry mover against an Infantry-class clone (a mine / bomb class): Repulsion (step 4), never a kill
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc m = unit(10, start, 2, 1.0f, 6.0f, LocomotorKind::Default);
        m.owner = 1;
        CHECK(sys.add_unit(m));
        CHECK(sys.add_unit(t20_clone(11, cell_center(22, 20) + V2{0.0f, 3.0f}, kUccInfantry, 4.0f, 4.0f)));
        SimRng rng(522);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        sys.move(o, rng, 1);
        bool arrived = false, repulsion = false, kill = false;
        for (uint32_t f = 1; f < 800 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& c : sys.collisions().list())
                repulsion = repulsion || (c.a == 10 && c.b == 11 && c.state == CollisionState::Repulsion);
            kill = kill || !sys.push_kills().empty();
            arrived = sys.view(10).arrived;
        }
        CHECK(arrived && repulsion && !kill && sys.has_unit(11) && sys.view(11).pos == (cell_center(22, 20) + V2{0.0f, 3.0f}));
    }
    {  // (c) only objects with a locomotor block the destination (0x684710 / 0x685e10): the early-arrival case (vii) of
       // test_t20_early_arrival - a unit 60 before the goal and one on the goal - with a death clone on the goal. With a
       // standing vehicle there the destination is blocked and the order path is cut 33 short; with the clone it is
       // free: no cut, the record goes to Path Around and the path still ends on the goal
        const auto run = [&](bool clone, bool& around, V2& end) {
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            UnitDesc m = t20_vehicle(10, start, 1);
            m.formation_spacing = 2.0f;
            CHECK(sys.add_unit(m));
            SimRng rng(523);
            MoveOrder o;
            o.units = {10};
            o.destination = dest;
            sys.move(o, rng, 1);
            bool placed = false, decided = false;
            around = false;
            for (uint32_t f = 1; f < 1500 && !decided; ++f) {
                if (!placed && sys.view(10).pos.x >= dest.x - 90.0f) {
                    CHECK(sys.add_unit(t20_vehicle(11, {dest.x - 60.0f, dest.y}, 1)));
                    CHECK(sys.add_unit(clone ? t20_clone(12, dest, kUccVehicle, 12.0f, 6.0f) : t20_vehicle(12, dest, 1, 12.0f, 6.0f)));
                    placed = true;
                }
                sys.tick(f, rng, serial_for);
                for (const CollisionObject& c : sys.collisions().list())
                    if (c.a == 10 && c.b == 11 && c.state != CollisionState::Active) {
                        decided = true;
                        around = c.state == CollisionState::PathAround;
                    }
            }
            end = sys.land_path(10).empty() ? V2{} : sys.land_path(10).back();
        };
        bool around_c = false, around_v = false;
        V2 end_c, end_v;
        run(true, around_c, end_c);
        run(false, around_v, end_v);
        CHECK(around_c && distance(end_c, dest) < 1e-3f);
        CHECK(around_v && near(end_v.x, dest.x - 33.0f, 1e-3f));
        std::printf("t20 batch 1 death clone: on the goal - Path Around %d, path end %.1f from the goal (a standing vehicle: %.1f)\n",
                    around_c ? 1 : 0, static_cast<double>(distance(end_c, dest)), static_cast<double>(distance(end_v, dest)));
    }
    {  // (d) a clone that fills the end of a one-cell lane (the goal lies under it): it does not block the destination,
       // and Path Around finds no rejoin point (every candidate overlaps the clone or lies off the lane), so step 8 kills
       // the clone ("shove_ok or b is a death clone") - for a hostile vehicle and for a hostile single infantry mover
        const auto run = [&](bool infantry, uint32_t& kill_frame, bool& arrived, MovementStats& st) {
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(60, 60);
            for (int32_t y = 0; y < 60; ++y)
                for (int32_t x = 0; x < 60; ++x)
                    if (!(y == 20 && x >= 10 && x <= 32)) g.set_base(x, y, kPassImpassable);
            sys.set_grid(std::move(g));
            sys.set_anim_length_fn(anim_len);
            UnitDesc m = infantry ? unit(10, start, 2, 1.0f, 4.0f, LocomotorKind::Default) : t20_vehicle(10, start, 1, 8.0f, 4.0f);
            m.owner = 1;
            CHECK(sys.add_unit(m));
            CHECK(sys.add_unit(t20_clone(11, {dest.x - 30.0f, dest.y}, kUccVehicle, 90.0f, 14.0f)));  // x 480 .. 660, across the lane
            SimRng rng(524);
            MoveOrder o;
            o.units = {10};
            o.destination = dest;
            o.skip_destination_check = true;  // (the goal under the clone; no forced path)
            sys.move(o, rng, 1);
            std::vector<ObjectId> killed;
            kill_frame = 0;
            arrived = false;
            for (uint32_t f = 1; f < 1500 && !arrived; ++f) {
                sys.tick(f, rng, serial_for);
                if (!sys.push_kills().empty() && kill_frame == 0) kill_frame = f;
                remove_killed(sys, killed);
                arrived = sys.view(10).arrived;
            }
            CHECK(killed == std::vector<ObjectId>{11});
            st = sys.stats();
        };
        uint32_t kv = 0, ki = 0;
        bool av = false, ai = false;
        MovementStats sv, si;
        run(false, kv, av, sv);
        run(true, ki, ai, si);
        CHECK(kv > 0 && ki > 0 && av && ai && sv.pushes == 0 && si.pushes == 0 && sv.push_failures == 0 && si.push_failures == 0);
        std::printf("t20 batch 1 death clone: lane end - killed at frame %u (vehicle) / %u (single infantry), both arrived %d\n",
                    kv, ki, av && ai ? 1 : 0);
    }
    {  // (e) a standing unit overlapping a clone: StoppedVsStopped with the unit as the checker; rule 1 moves it (the
       // clone has no locomotor) - and a save / load in the middle continues identically
        const auto setup = [&](MovementSystem& sys) {
            land_system(sys, 100);
            const V2 p0 = cell_center(20, 20);
            CHECK(sys.add_unit(t20_vehicle(10, p0, 1)));
            CHECK(sys.add_unit(t20_clone(11, {p0.x + 8.0f, p0.y}, kUccVehicle, 10.0f, 6.0f)));
        };
        MovementSystem sys{MovementConfig{}};
        setup(sys);
        SimRng rng(525);
        sys.tick(1, rng, serial_for);
        CHECK(sys.collisions().list().size() == 1);
        if (sys.collisions().list().size() == 1) {
            const CollisionObject& c = sys.collisions().list()[0];
            CHECK(c.kind == CollisionKind::StoppedVsStopped && c.a == 10 && c.b == 11 && c.master == 0 &&
                  c.state == CollisionState::Resolving);
        }
        for (uint32_t f = 2; f < 10; ++f) sys.tick(f, rng, serial_for);
        ByteWriter w;
        sys.write(w);
        MovementSystem copy{MovementConfig{}};
        land_system(copy, 100);
        {
            ByteReader r(w.bytes());
            CHECK(copy.read(r) && r.done());
        }
        SimRng r2 = rng;
        for (uint32_t f = 10; f < 300; ++f) {
            sys.tick(f, rng, serial_for);
            copy.tick(f, r2, serial_for);
        }
        StateHasher ha, hb;
        sys.hash_units(ha);
        copy.hash_units(hb);
        CHECK(ha.digest() == hb.digest());
        CHECK(sys.view(11).pos == (V2{cell_center(20, 20).x + 8.0f, cell_center(20, 20).y}) && sys.collisions().list().empty());
        CHECK(distance(sys.view(10).pos, cell_center(20, 20)) > 5.0f);
    }
    {  // save / load with a kill listed (push_kills is saved and hashed like the crush events)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(t20_vehicle(10, start, 1)));
        CHECK(sys.add_unit(t20_clone(11, cell_center(24, 20), kUccVehicle, 10.0f, 6.0f)));
        SimRng rng(526);
        MoveOrder o;
        o.units = {10};
        o.destination = dest;
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 800 && sys.push_kills().empty(); ++f) sys.tick(f, rng, serial_for);
        CHECK(!sys.push_kills().empty());
        ByteWriter w;
        sys.write(w);
        MovementSystem copy{MovementConfig{}};
        land_system(copy, 100);
        {
            ByteReader r(w.bytes());
            CHECK(copy.read(r) && r.done());
        }
        StateHasher ha, hb;
        sys.hash_units(ha);
        copy.hash_units(hb);
        CHECK(ha.digest() == hb.digest() && copy.push_kills().size() == 1 && copy.push_kills()[0] == 11);
    }
}
