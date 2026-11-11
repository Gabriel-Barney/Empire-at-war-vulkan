// alamo_tactical_movement_b2_tests (no framework; non-zero exit on failure).
// s78n t20 batch 2: the order-time items of the land movement port - the zone validation of the ordered point and the
// destination check's D, forced pass and try state (docs/re/MOVEMENT.md §4.2 step 2, §4.3.1 items 3, 4, 7); the squad
// container's turn in place (FIGHTER_FLYING §7.7 item 7); hover z (MOVEMENT §5.3 "Hover in full").
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

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

bool at(V2 v, float x, float y) { return v.x == x && v.y == y; }

PassabilityGrid open_grid(uint32_t w, uint32_t h) {
    PassabilityGrid g;
    g.init(w, h);
    return g;
}

void block(PassabilityGrid& g, int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x) g.set_base(x, y, kPassImpassable);
}

RegionMap regions_of(const PassabilityGrid& g) {
    RegionMap r;
    const uint32_t masks[1] = {kPassClear};
    r.build(g, masks);
    return r;
}

UnitDesc unit(ObjectId id, V2 pos, int32_t cls, float speed, float radius, LocomotorKind kind) {
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
    d.type_name = "B2_UNIT";
    d.locomotor = kind;
    return d;
}

uint32_t anim_len(ObjectId, int32_t a) {
    switch (a) {
        case kAnimJump: return 10;
        case kAnimFlyLand: return 8;
        case kAnimFlyLandDrop: return 5;
        case kAnimTurnLeftEnd:
        case kAnimTurnRightEnd: return 4;
        case kAnimMoveStart: return 6;
        default: return 0;
    }
}

void land_system(MovementSystem& sys, PassabilityGrid g) {
    const float hi = static_cast<float>(g.width() - 1) * kCellSize;
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float, float) { return 0.0f; });
    sys.set_anim_length_fn(anim_len);
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
}

// A 9-member squad on a 3 x 3 grid of pitch 12 (member half extents 3): joint slot box (15, 15). Container `cid`, members
// cid + 1 .. cid + 9 (as test_movement_gaps' t20_squad).
void squad(MovementSystem& sys, ObjectId cid, V2 at, int32_t owner, float cmax = 0.8f, float turn = 6.0f) {
    UnitDesc c = unit(cid, at, 2, cmax, 12.0f, LocomotorKind::Default);
    c.turn_rate = turn;
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

UnitDesc vehicle(ObjectId id, V2 pos, int32_t owner, float hx = 10.0f, float hy = 6.0f, float speed = 1.0f) {
    UnitDesc d = unit(id, pos, 2, speed, 12.0f, LocomotorKind::Default);
    d.collision_class = kUccVehicle;
    d.hard_x = hx;
    d.hard_y = hy;
    d.owner = owner;
    return d;
}

// ---------------------------------------------------------------- 2.1 zone validation (MOVEMENT §4.3.1 item 7)
void test_zone_validate() {
    ::alamo::test::section("zone_validate");
    const V2 p = cell_center(20, 20);  // (400, 400)
    {  // cells x, y 18 .. 22 blocked: ring 1 and 2 are all blocked, ring 3 decides
        PassabilityGrid g = open_grid(40, 40);
        block(g, 18, 22, 18, 22);
        const RegionMap r = regions_of(g);
        V2 out{-1.0f, -1.0f};
        CHECK(zone_validate(r, p, cell_center(5, 20), kPassClear, out) && at(out, 340.0f, 400.0f));
        CHECK(zone_validate(r, p, cell_center(5, 5), kPassClear, out) && at(out, 340.0f, 340.0f));
        // P in the seed's zone is kept as it is (not moved to a cell centre)
        const V2 free{123.5f, 456.25f};
        CHECK(zone_validate(r, free, cell_center(5, 5), kPassClear, out) && out == free);
        // Q outside the grid ("none" with an outside cell) keeps P
        CHECK(zone_validate(r, p, V2{-100.0f, 50.0f}, kPassClear, out) && out == p);
        // the quirk (step 5): Q on impassable ground (zone 0), P passable -> an impassable cell near P (ring 2 of cell
        // (16, 20): (18, 20) is the first kept cell nearest to Q = cell (20, 20))
        CHECK(zone_validate(r, cell_center(16, 20), p, kPassClear, out) && out == cell_center(18, 20));
        CHECK(zone_connected(r, kPassClear, cell_center(5, 5), cell_center(30, 30)));
        CHECK(!zone_connected(r, kPassClear, p, p));  // an impassable end point is never zone-connected
    }
    {  // the first ring with a kept cell decides: ring 1 holds (21, 20); ring 4's (16, 20) would be nearer to Q
        PassabilityGrid g = open_grid(40, 40);
        block(g, 17, 20, 19, 21);
        g.set_base(21, 19, kPassImpassable);
        g.set_base(21, 21, kPassImpassable);
        const RegionMap r = regions_of(g);
        V2 out;
        CHECK(zone_validate(r, p, cell_center(5, 20), kPassClear, out) && at(out, 420.0f, 400.0f));
    }
    {  // the seed in a closed pocket (cells 1 .. 3), P 37 cells away: no cell of its zone within 29 rings -> false
        PassabilityGrid g = open_grid(80, 80);
        block(g, 0, 4, 0, 0);
        block(g, 0, 4, 4, 4);
        block(g, 0, 0, 0, 4);
        block(g, 4, 4, 0, 4);
        const RegionMap r = regions_of(g);
        V2 out{-7.0f, -7.0f};
        CHECK(!zone_validate(r, cell_center(40, 40), cell_center(2, 2), kPassClear, out) && at(out, -7.0f, -7.0f));
        // a P whose ring 29 reaches the pocket's column 3 (the mirrors of cells (61, 3 .. 5)): nearest to Q = cell (2, 2)
        CHECK(zone_validate(r, cell_center(32, 3), cell_center(2, 2), kPassClear, out) && out == cell_center(3, 2));
    }
}

// ---------------------------------------------------------------- 2.1 the order: validation, D, forced pass
void test_order_validation() {
    ::alamo::test::section("order zone validation and the destination check's D");
    {  // a squad ordered onto a blocked square (cells 28 .. 32): its goal is moved to the ring cell nearest to it, (27, 30);
       // another squad stands there, so the destination check runs from D = that cell and displaces the goal off it
        struct Out {
            V2 goal;
            bool arrived = false;
            uint32_t early = 0;
            uint64_t crc = 0;
        };
        const auto run = [&](bool stander, const ParallelFor& pf) {
            Out o;
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(100, 100);
            block(g, 28, 32, 28, 32);
            land_system(sys, std::move(g));
            squad(sys, 100, cell_center(10, 30), 1);
            if (stander) squad(sys, 200, cell_center(27, 30), 1);
            SimRng rng(211);
            sys.tick(1, rng, pf);
            MoveOrder m;
            m.units = {101};
            m.destination = cell_center(30, 30);
            sys.move(m, rng, 2);
            o.goal = sys.view(100).goal;
            for (uint32_t f = 2; f < 1500 && !o.arrived; ++f) {
                sys.tick(f, rng, pf);
                o.arrived = sys.view(100).arrived;
            }
            o.early = sys.stats().early_arrivals;
            StateHasher h;
            sys.hash_units(h);
            o.crc = h.digest();
            return o;
        };
        const Out lone = run(false, serial_for);
        // D = the validated cell (540, 600); its rectangle (20, 20) reaches into the block (try 0 "met", try 1 at 533
        // too), try 2 = D - 14 x is passable: "crossed", connected, accepted
        CHECK(near(lone.goal.x, 526.0f, 1e-3f) && lone.goal.y == 600.0f && lone.arrived);  // (F8 float noise)
        const Out s = run(true, serial_for), p = run(true, make_thread_pf(4));
        const V2 st = cell_center(27, 30);
        const float cheb = std::max(std::fabs(s.goal.x - st.x), std::fabs(s.goal.y - st.y));
        CHECK(cheb >= 15.0f + 5.0f + 15.0f);  // HX + S + the stander's hx: the slot rectangles clear the stander
        CHECK(s.arrived && s.early == 0 && s.crc == p.crc);
        std::printf("validated D: lone goal (%.1f, %.1f); beside a stander goal (%.2f, %.2f), %.2f off, early arrivals %u\n",
                    static_cast<double>(lone.goal.x), static_cast<double>(lone.goal.y), static_cast<double>(s.goal.x),
                    static_cast<double>(s.goal.y), static_cast<double>(cheb), s.early);
    }
    {  // validation fails (the squad in a closed pocket, the goal 37 cells away): no move, the order ends as failed
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(80, 80);
        block(g, 0, 8, 0, 0);
        block(g, 0, 8, 8, 8);
        block(g, 0, 0, 0, 8);
        block(g, 8, 8, 0, 8);
        land_system(sys, std::move(g));
        squad(sys, 100, cell_center(4, 4), 1);
        SimRng rng(212);
        sys.tick(1, rng, serial_for);
        MoveOrder m;
        m.units = {101};
        m.destination = cell_center(41, 41);
        sys.move(m, rng, 2);
        CHECK(!sys.view(100).has_goal && sys.view(100).failed && !sys.view(100).arrived);
        const uint32_t planned = sys.stats().paths_planned;
        float moved = 0.0f;
        for (uint32_t f = 2; f < 120; ++f) {
            sys.tick(f, rng, serial_for);
            moved = std::max(moved, distance(sys.view(100).pos, cell_center(4, 4)));
        }
        CHECK(moved == 0.0f && sys.stats().paths_planned == planned);
    }
    {  // a fresh block (V != P) carries no final facing: ordered with final facing 90 onto a blocked cell the vehicle
       // ends facing its travel direction (0); the same order to a free point turns it to 90
        const auto end_yaw = [&](V2 dest) {
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(100, 100);
            block(g, 28, 32, 28, 32);
            land_system(sys, std::move(g));
            CHECK(sys.add_unit(vehicle(10, cell_center(10, 30), 1)));
            SimRng rng(213);
            MoveOrder m;
            m.units = {10};
            m.destination = dest;
            m.final_facing_deg = 90.0f;
            sys.move(m, rng, 1);
            for (uint32_t f = 1; f < 1500; ++f) sys.tick(f, rng, serial_for);
            CHECK(sys.view(10).arrived);
            return sys.view(10).yaw;
        };
        const float fresh = end_yaw(cell_center(30, 30)), kept = end_yaw(cell_center(22, 30));
        CHECK(std::fabs(fresh) < 1.0f && near(kept, 90.0f, 1e-3f));
        std::printf("fresh block final facing: blocked goal yaw %.2f, free goal yaw %.2f\n", static_cast<double>(fresh),
                    static_cast<double>(kept));
    }
}

void test_forced_pass() {
    ::alamo::test::section("destination check forced pass and try state");
    {  // a vehicle 200 x 200 stands on the goal: every try of every pass is rejected by it; the forced pass at D takes the
       // last try that reached the object test, k = 39: D + v x 28 x 4 x (-0.375) + (0, -1) x min(HX + S, HY + S) with
       // v = -d1 = (-1, 0) and the first formation's halved offsets (no impassable ground met): D + (42, -20)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, open_grid(100, 100));
        squad(sys, 100, cell_center(15, 20), 1);
        CHECK(sys.add_unit(vehicle(50, cell_center(30, 20), 2, 200.0f, 200.0f, 0.0f)));
        SimRng rng(214);
        sys.tick(1, rng, serial_for);
        MoveOrder m;
        m.units = {101};
        m.destination = cell_center(30, 20);
        sys.move(m, rng, 2);
        const V2 g = sys.view(100).goal;
        CHECK(near(g.x, 642.0f, 1e-3f) && near(g.y, 380.0f, 1e-3f));
        std::printf("forced pass: goal (%.3f, %.3f)\n", static_cast<double>(g.x), static_cast<double>(g.y));
    }
    {  // the early end. D = (600, 400) with a squad standing on it (rectangle x 585 .. 615, y 385 .. 415); cells x >= 32
       // blocked, and cells x 27 .. 31, y <= 18 blocked. Pass 0 (v = (-1, 0), R half extents 20): tries 0 .. 3 are
       // passable and fail on the stander before any impassable ground; try 4 (D + (20, 0)) is impassable: "met"; try 5
       // (D + (0, 20)) is passable: "crossed", and fails on the stander; try 6 (D - (20, 0)) fails too; try 7
       // (D - (0, 20)) is impassable after a failure while crossed: the tries end (without the early end try 9 or 10,
       // D - 35 / 42 x, would be taken). Pass 1 (Dn = D + 20 (0.707, 0.707)) ends the same way at its try 4. Pass 2
       // (Dn = D + (0, 20)): tries 0 .. 3 fail on the stander, try 4 is impassable, try 5 = Dn + (0, 20) is accepted.
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(100, 100);
        block(g, 32, 40, 0, 99);
        block(g, 27, 31, 0, 18);
        land_system(sys, std::move(g));
        squad(sys, 100, cell_center(15, 20), 1);
        squad(sys, 200, cell_center(30, 20), 1);
        SimRng rng(215);
        sys.tick(1, rng, serial_for);
        MoveOrder m;
        m.units = {101};
        m.destination = cell_center(30, 20);
        sys.move(m, rng, 2);
        const V2 goal = sys.view(100).goal;
        CHECK(near(goal.x, 600.0f, 1e-3f) && near(goal.y, 440.0f, 1e-3f));
        std::printf("early end: goal (%.3f, %.3f)\n", static_cast<double>(goal.x), static_cast<double>(goal.y));
    }
}

// ---------------------------------------------------------------- 2.2 the container's turn in place (FIGHTER_FLYING §7.7 item 7)
void test_container_turn() {
    ::alamo::test::section("container turn in place");
    struct Out {
        uint32_t first_turn = 0, first_step = 0;
        std::vector<float> yaw;  // per frame from the order frame
        float member_moved = 0.0f;  // the farthest a member moved before the container's first step
    };
    // a lone squad at yaw 0 ordered 200 away at `deg` (speed multiplier 1.0: B = the type's turn rate)
    const auto run = [&](float b, float deg) {
        Out o;
        MovementConfig cfg;
        cfg.constants.land_speed_multiplier = 1.0f;
        MovementSystem sys{cfg};
        land_system(sys, open_grid(100, 100));
        const V2 at = cell_center(40, 40);
        squad(sys, 100, at, 1, 0.8f, b);
        SimRng rng(221);
        for (uint32_t f = 1; f < 40; ++f) sys.tick(f, rng, serial_for);  // laid out, members on their slots
        MoveOrder m;
        m.units = {101};
        m.destination = at + dir_from_deg(deg) * 200.0f;
        sys.move(m, rng, 40);
        const V2 p0 = sys.view(100).pos;
        V2 mp[9];
        for (ObjectId i = 0; i < 9; ++i) mp[i] = sys.view(101 + i).pos;
        float prev = sys.view(100).yaw;
        for (uint32_t f = 40; f < 400 && o.first_step == 0; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView v = sys.view(100);
            o.yaw.push_back(v.yaw);
            if (o.first_turn == 0 && v.yaw != prev) o.first_turn = f;
            if (!(v.pos == p0)) o.first_step = f;
            prev = v.yaw;
            if (o.first_step == 0)
                for (ObjectId i = 0; i < 9; ++i) o.member_moved = std::max(o.member_moved, distance(sys.view(101 + i).pos, mp[i]));
        }
        return o;
    };
    const float up = heading_deg(V2{0.0f, 1.0f});  // the segment's heading (90)
    const Out b4 = run(4.0f, 90.0f), b45 = run(4.5f, 90.0f), b0 = run(4.0f, 0.0f);
    const uint32_t t0 = b4.first_turn;
    CHECK(t0 > 40 && b45.first_turn == t0);
    // B 4.0: 22 ticks of turning (4 .. 88), tick 23 yaw := 90 with 2 left: the first step in the same tick
    CHECK(b4.yaw.size() == t0 - 40 + 23);
    CHECK(b4.yaw[t0 - 40 + 21] == 88.0f && b4.yaw[t0 - 40 + 22] == up && b4.first_step == t0 + 22);
    // B 4.5: tick 20 reaches 90 exactly (no budget left: no step), the first step on tick 21
    CHECK(b45.yaw[t0 - 40 + 19] == up && b45.first_step == t0 + 20);
    // delta 0: the first step on the first tick of the start
    CHECK(b0.first_turn == 0 && b0.first_step == t0);
    CHECK(b4.member_moved == 0.0f && b45.member_moved == 0.0f);  // the members stand while the container turns
    std::printf("container turn: start tick %u, B 4 first step %u (yaw %.4f), B 4.5 first step %u, aligned first step %u; "
                "members moved %.3f / %.3f before the first step\n",
                t0, b4.first_step, static_cast<double>(b4.yaw.back()), b45.first_step, b0.first_step,
                static_cast<double>(b4.member_moved), static_cast<double>(b45.member_moved));
    {  // a crush-flee path (kind 7): the budget doubles (Fleeing_Infantry_Speed_Bonus 2): B 1 turns 2 per tick
        MovementConfig cfg;
        cfg.constants.land_speed_multiplier = 1.0f;
        MovementSystem sys{cfg};
        land_system(sys, open_grid(100, 100));
        const V2 at = cell_center(20, 20);
        UnitDesc c = unit(100, at, 2, 2.0f, 12.0f, LocomotorKind::Default);
        c.team_container = true;
        c.container_arrangement = 0;
        c.squashable = true;
        c.owner = 1;
        c.turn_rate = 1.0f;
        c.movement_prediction_interval = 4.0f;
        CHECK(sys.add_unit(c));
        const V2 rel[4] = {{10.0f, 10.0f}, {10.0f, -10.0f}, {-10.0f, 10.0f}, {-10.0f, -10.0f}};
        std::vector<ObjectId> ids;
        std::vector<V2> slots;
        for (ObjectId id = 1; id <= 4; ++id) {
            UnitDesc d = unit(id, at + rel[id - 1], 2, 2.0f, 3.0f, LocomotorKind::Default);
            d.owner = 1;
            d.formation_spacing = 0.5f;
            CHECK(sys.add_unit(d));
            CHECK(sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
            ids.push_back(id);
            slots.push_back(rel[id - 1]);
        }
        CHECK(sys.set_land_team(100, ids, slots));
        for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}, ObjectId{4}}) CHECK(sys.set_team(id, 100, 1.0f));
        SimRng rng(222);
        for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, serial_for);
        UnitDesc cr = unit(60, cell_center(8, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
        cr.collision_class = kUccVehicle;
        cr.crusher = true;
        cr.owner = 2;
        CHECK(sys.add_unit(cr));
        MoveOrder o;
        o.units = {60};
        o.destination = cell_center(60, 20);
        sys.move(o, rng, 20);
        uint32_t turning = 0, full = 0;
        float worst = 0.0f;
        bool fled = false, moved = false;
        for (uint32_t f = 20; f < 600 && !moved; ++f) {
            const UnitView b = sys.view(100);
            sys.tick(f, rng, serial_for);
            const UnitView a = sys.view(100);
            if (!sys.team_fleeing(100)) continue;
            fled = true;
            if (!(a.pos == b.pos)) {
                moved = true;
                continue;
            }
            const float d = std::fabs(wrap_deg(a.yaw - b.yaw));
            if (!(d > 0.0f)) continue;
            ++turning;
            worst = std::max(worst, d);
            if (std::fabs(d - 2.0f) < 1e-4f) ++full;
        }
        CHECK(fled && moved && turning > 0 && full + 1 >= turning && worst <= 2.0f + 1e-4f && full > 0);
        std::printf("container turn: crush-flee start - %u turning ticks, %u of them 2.0 (B 1 x 2)\n", turning, full);
    }
}

// s78n t20 batch 2 item 2.4 (MOVEMENT §5.3 "Hover in full"): a Hover_Height 80 Walk unit at max speed 2.5 on ground
// 10. By hand: Start Move sets rising (tick 0, z 10); ticks 1 .. 32 add 2.5 (z = 10 + 2.5 n, the 32nd lands exactly on
// 90, so it is still rising); tick 33 finds z >= 90: raised, speed 0; tick 34 is the first horizontal step, at z 90.
// A unit 90 degrees off its path turns in place on the ground first (turn rate 6, close-enough angle 1: 14 turning ticks at z 10, the
// 15th tick finds the error 6 <= the turn rate and rises). Lowering
// falls 2.5 per tick with x / y / yaw frozen and grounds on the 32nd tick (clamped at T on the frame it reaches it).
void test_t20_hover_z() {
    ::alamo::test::section("hover z (rise, raised commit, turn on the ground, lowering)");
    constexpr float kGround = 10.0f;
    MovementConfig unit_speed;
    unit_speed.constants.land_speed_multiplier = 1.0f;  // effective max speed = Max_Speed, turn rate = Rate_Of_Turn
    const auto make = [&](MovementSystem& sys, float yaw) {
        PassabilityGrid g = open_grid(100, 100);
        const float hi = static_cast<float>(g.width() - 1) * kCellSize;
        sys.set_grid(std::move(g));
        sys.set_height_fn([](float, float) { return kGround; });
        sys.air().set_box({0.0f, 0.0f}, {hi, hi});
        UnitDesc d = unit(1, cell_center(20, 50), 2, 2.5f, 12.0f, LocomotorKind::Default);
        d.collision_class = kUccVehicle;
        d.hover_height = 80.0f;
        d.yaw_deg = yaw;
        d.close_enough_angle = 1.0f;  // the start rule turns in place while the error exceeds the turn rate
        d.z = kGround;
        CHECK(sys.add_unit(d));
    };
    struct Tick {
        uint8_t hover = 0;
        float z = 0.0f, yaw = 0.0f;
        V2 pos;
    };
    const auto run = [&](float yaw, uint32_t ticks, std::vector<Tick>& out, MovementSystem& sys, SimRng& rng) {
        make(sys, yaw);
        MoveOrder m;
        m.units = {1};
        m.destination = cell_center(70, 50);
        sys.move(m, rng, 1);
        for (uint32_t f = 1; f <= ticks; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView v = sys.view(1);
            out.push_back({sys.hover_state(1), v.z, v.yaw, v.pos});
        }
    };
    {  // straight ahead: the rise by hand
        MovementSystem sys{unit_speed};
        SimRng rng(241);
        std::vector<Tick> t;
        run(0.0f, 120, t, sys, rng);
        size_t t0 = 0;
        while (t0 < t.size() && t[t0].hover != 2) ++t0;
        CHECK(t0 + 40 < t.size());
        if (t0 + 40 < t.size()) {
            const V2 p0 = t[t0].pos;
            CHECK(t[t0].z == kGround);
            bool climb = true;
            for (size_t n = 1; n <= 32; ++n)
                climb = climb && t[t0 + n].hover == 2 && t[t0 + n].z == kGround + 2.5f * static_cast<float>(n) && t[t0 + n].pos == p0;
            CHECK(climb);
            CHECK(t[t0 + 33].hover == 1 && t[t0 + 33].z == kGround + 80.0f && t[t0 + 33].pos == p0);
            CHECK(t[t0 + 34].hover == 1 && t[t0 + 34].z == kGround + 80.0f && t[t0 + 34].pos.x > p0.x);
            bool raised = true;
            for (size_t n = t0 + 34; n < t.size(); ++n) raised = raised && t[n].hover == 1 && t[n].z == kGround + 80.0f;
            CHECK(raised);
            std::printf("hover z: rising from tick %zu, z %.1f .. %.1f, raised at +33, first step at +34 (x %.2f -> %.2f)\n", t0,
                        static_cast<double>(t[t0 + 1].z), static_cast<double>(t[t0 + 32].z), static_cast<double>(p0.x),
                        static_cast<double>(t[t0 + 34].pos.x));
        }
        // lowering mid-move: x / y / yaw frozen, z falls 2.5 per tick, grounded on the 32nd tick
        const UnitView v0 = sys.view(1);
        CHECK(v0.has_goal && sys.hover_lower(1) && sys.hover_state(1) == 3);
        uint32_t f = 121, lowering = 0;
        bool frozen = true, falls = true;
        float z = v0.z;
        for (; f < 200 && sys.hover_state(1) == 3; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView v = sys.view(1);
            frozen = frozen && v.pos == v0.pos && v.yaw == v0.yaw;
            if (sys.hover_state(1) == 3) {
                ++lowering;
                falls = falls && v.z == z - 2.5f;
                z = v.z;
            }
        }
        CHECK(frozen && falls && lowering == 31 && sys.hover_state(1) == 0 && sys.view(1).z == kGround);
        // the move goes on: it rises again
        sys.tick(f, rng, serial_for);
        CHECK(sys.hover_state(1) == 2 && sys.view(1).pos == v0.pos);
        std::printf("hover z: lowering %u ticks at 2.5, then grounded at %.1f, rising again\n", lowering,
                    static_cast<double>(sys.view(1).z));
    }
    {  // 90 degrees off: the turn in place runs on the ground, then the rise
        MovementSystem sys{unit_speed};
        SimRng rng(242);
        std::vector<Tick> t;
        run(90.0f, 80, t, sys, rng);
        uint32_t turning = 0;
        bool ground_turn = true, no_air_turn = true;
        float yaw = 90.0f;
        size_t rise = t.size();
        for (size_t n = 0; n < t.size(); ++n) {
            if (rise == t.size() && t[n].hover == 2) rise = n;
            if (t[n].yaw != yaw) {
                if (t[n].hover == 0 && t[n].z == kGround) ++turning;
                else if (t[n].hover >= 2) no_air_turn = false;
                else ground_turn = false;
            }
            yaw = t[n].yaw;
        }
        CHECK(ground_turn && no_air_turn && turning == 14 && rise < t.size() && rise >= 14);
        if (rise < t.size()) CHECK(t[rise].pos == t[0].pos && std::fabs(wrap_deg(t[rise].yaw)) <= 6.0f);
        std::printf("hover z: 90 degrees off - %u turning ticks on the ground, rising from tick %zu\n", turning, rise);
    }
}

}  // namespace

int main() {
    test_zone_validate();
    test_order_validation();
    test_forced_pass();
    test_container_turn();
    test_t20_hover_z();
    return ::alamo::test::report("alamo_tactical_movement_b2_tests");
}
