// cu8l t03: the campaign space fleet placement layout (fleet_placement.hpp; docs/re/CU8K_T03_T08_T09_RE.md §1.2 - §1.8).
// Hand-computed lists, row counts, grid / ring / row offsets and member positions; the placement order is the order of
// the adopt draws (one per placed object).
#include <cmath>
#include <cstdio>
#include <vector>

#include "alamo/tactical/movement/fleet_placement.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;
using namespace alamo::tactical::movement::fleet;

namespace {

using ::alamo::test::g_fail;

bool near(float a, float b, float tol = 1.0e-3f) { return std::fabs(a - b) <= tol; }
bool near(V2 a, V2 b, float tol = 1.0e-3f) { return near(a.x, b.x, tol) && near(a.y, b.y, tol); }

#define CHECK_NEAR(a, b)                                                                                             \
    do {                                                                                                             \
        const V2 va_ = (a), vb_ = (b);                                                                               \
        if (!near(va_, vb_)) {                                                                                       \
            std::printf("FAIL %s:%d: %s ~ %s ((%.6g, %.6g) vs (%.6g, %.6g))\n", __FILE__, __LINE__, #a, #b, va_.x, va_.y, \
                        vb_.x, vb_.y);                                                                               \
            ++g_fail;                                                                                                \
        }                                                                                                            \
    } while (0)

FleetObject ship(int32_t cls, float hx, float hy) {
    FleetObject o;
    o.ship_class = cls;
    o.box = {hx, hy};
    return o;
}

// A squadron container (its members' class `cls`), `n` members with box (hx, hy), the same offsets on both types.
FleetObject squadron(int32_t cls, uint32_t n, float hx, float hy, std::vector<V2> offsets) {
    FleetObject o;
    o.ship_class = cls;
    o.team = true;
    o.members.assign(n, HalfBox{hx, hy});
    o.offsets_team = offsets;
    o.offsets_company = offsets;
    o.units_team = o.units_company = n;
    return o;
}

void test_lists_and_rows() {
    // §1.2 step 2: the class table.
    CHECK(list_of_class(5) == 0 && list_of_class(3) == 1 && list_of_class(4) == 1 && list_of_class(0) == 3);
    CHECK(list_of_class(1) == 3 && list_of_class(2) == 4 && list_of_class(6) == 5);
    CHECK(list_of_class(-1) == -1 && list_of_class(7) == -1);
    // §1.2 step 3: the fighter / bomber interleave (fighters 10.., bombers 20..).
    const std::vector<uint32_t> f3{10, 11, 12}, f1{10}, f2{10, 11}, b1{20}, b2{20, 21}, b4{20, 21, 22, 23};
    CHECK((interleave(f3, b1) == std::vector<uint32_t>{10, 11, 20, 12}));          // B <= F: h = 0
    CHECK((interleave(f1, b4) == std::vector<uint32_t>{20, 21, 10, 22, 23}));      // B > F: h = 1
    CHECK((interleave(f2, b2) == std::vector<uint32_t>{20, 10, 21, 11}));          // B <= F: h = 1
    CHECK((interleave(f3, b2) == std::vector<uint32_t>{20, 10, 11, 21, 12}));      // h = 1, one extra fighter
    // §1.4 steps 1 / 2: rows and the remainder table.
    CHECK(grid_rows(2) == 1 && grid_rows(3) == 2 && grid_rows(6) == 2 && grid_rows(7) == 3 && grid_rows(9) == 3);
    CHECK(grid_rows(10) == 4 && grid_rows(16) == 4 && grid_rows(17) == 5 && grid_rows(25) == 5 && grid_rows(26) == 6);
    CHECK((grid_row_counts(1) == std::vector<uint32_t>{1}));
    CHECK((grid_row_counts(5) == std::vector<uint32_t>{3, 2}));
    CHECK((grid_row_counts(7) == std::vector<uint32_t>{2, 3, 2}));
    CHECK((grid_row_counts(8) == std::vector<uint32_t>{3, 2, 3}));
    CHECK((grid_row_counts(10) == std::vector<uint32_t>{3, 2, 3, 2}));
    CHECK((grid_row_counts(13) == std::vector<uint32_t>{4, 3, 3, 3}));
    CHECK((grid_row_counts(15) == std::vector<uint32_t>{4, 4, 4, 3}));
    CHECK((grid_row_counts(17) == std::vector<uint32_t>{3, 4, 3, 4, 3}));
    CHECK((grid_row_counts(19) == std::vector<uint32_t>{4, 4, 4, 4, 3}));
    CHECK((grid_row_counts(26) == std::vector<uint32_t>{4, 4, 5, 5, 4, 4}));
    CHECK((grid_row_counts(27) == std::vector<uint32_t>{4, 5, 4, 5, 4, 5}));
    CHECK((grid_row_counts(28) == std::vector<uint32_t>{4, 5, 5, 5, 5, 4}));
    CHECK((grid_row_counts(29) == std::vector<uint32_t>{5, 5, 5, 5, 5, 4}));
    // Angles.
    CHECK(grid_theta(0.0f) == 270.0f && grid_theta(100.0f) == 10.0f && grid_theta(90.0f) == 360.0f);
    CHECK(arc_theta(0.0f, 90.0f) == 360.0f && arc_theta(0.0f, 337.5f) == 247.5f && arc_theta(-200.0f, 25.0f) == 95.0f);
}

void test_size() {
    // §1.3: ships -> (2 max hy, 2 max hx).
    const std::vector<FleetObject> objs{ship(5, 100.0f, 30.0f), ship(5, 60.0f, 45.0f)};
    const std::vector<uint32_t> both{0, 1};
    CHECK_NEAR(list_size(objs, both), (V2{90.0f, 200.0f}));
    // A squadron: offsets (30, 0), (0, 15), (0, -15) with member boxes (5, 4): x in [min(0, -4), 34] -> a = 19;
    // y in [-20, 20] -> b = 20. S = (40, 38).
    const std::vector<FleetObject> sq{squadron(0, 3, 5.0f, 4.0f, {{30.0f, 0.0f}, {0.0f, 15.0f}, {0.0f, -15.0f}})};
    const std::vector<uint32_t> one{0};
    CHECK_NEAR(list_size(sq, one), (V2{40.0f, 38.0f}));
    // Members past the offsets list stand at the origin: one member, no offsets -> (2 hx, 2 hy) swapped as the ranges are.
    const std::vector<FleetObject> bare{squadron(0, 1, 6.0f, 2.0f, {})};
    CHECK_NEAR(list_size(bare, one), (V2{12.0f, 4.0f}));
}

void test_capital_grid() {
    // Yaw 0 (facing +x): theta = 270, local +y -> world +x, local +x -> world -y. Sx = 100, Sy = 200.
    const std::vector<FleetObject> objs(4, ship(5, 100.0f, 50.0f));
    const V2 a{1000.0f, 500.0f};
    {
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1};
        const float w = capital_grid(objs, l, V2{100.0f, 200.0f}, a, 0.0f, out);
        CHECK(w == 50.0f && out.size() == 2 && out[0].index == 0 && out[1].index == 1 && out[0].via == Via::Capital);
        CHECK_NEAR(out[0].pos, (V2{1000.0f, 550.0f}));  // local (-50, 0)
        CHECK_NEAR(out[1].pos, (V2{1000.0f, 450.0f}));  // local (50, 0)
    }
    {
        // 3: rows [2, 1], y0 = 100: (-50, 100), (50, 100), then (0, -100).
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1, 2};
        CHECK(capital_grid(objs, l, V2{100.0f, 200.0f}, a, 0.0f, out) == 50.0f);
        CHECK_NEAR(out[0].pos, (V2{1100.0f, 550.0f}));
        CHECK_NEAR(out[1].pos, (V2{1100.0f, 450.0f}));
        CHECK_NEAR(out[2].pos, (V2{900.0f, 500.0f}));
    }
    {
        // 4: rows [2, 2]: the second row of the same count staggers by +Sx: (50, -100), (150, -100).
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1, 2, 3};
        (void)capital_grid(objs, l, V2{100.0f, 200.0f}, a, 0.0f, out);
        CHECK_NEAR(out[2].pos, (V2{900.0f, 450.0f}));
        CHECK_NEAR(out[3].pos, (V2{900.0f, 350.0f}));
    }
    {
        // Yaw 90 (facing +y): theta = 360, local = world. One ship: W = Sx / 2.
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0};
        CHECK(capital_grid(objs, l, V2{100.0f, 200.0f}, a, 90.0f, out) == 50.0f);
        CHECK_NEAR(out[0].pos, a);
    }
    {
        // Size (0, 0) (the supers): all at the anchor.
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1, 2};
        (void)capital_grid(objs, l, V2{}, a, 33.0f, out);
        for (const Placed& p : out) CHECK_NEAR(p.pos, a);
    }
}

void test_arcs_and_rows() {
    // Box (5, 0) -> S = (0, 10), s = 10.
    const std::vector<FleetObject> objs(8, ship(4, 5.0f, 0.0f));
    const V2 a{0.0f, 0.0f};
    {
        // Screen: R0 = 5; rings min(trunc(max(30, 1.25 pi R) / 5), rest): 6 at R 5, then 2 at R 10; r' = 15. Ring radii
        // 5 and 15 (a full s per ring). The 6-ring starts at 337.5 (theta 247.5) and steps 45.
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1, 2, 3, 4, 5, 6, 7};
        CHECK(screen_arc(objs, l, 0.0f, a, 0.0f, out) == 15.0f);
        CHECK(out.size() == 8 && out[0].via == Via::Screen);
        const float t0 = 247.5f * (6.28318530717958647692f / 360.0f);
        CHECK_NEAR(out[0].pos, (V2{5.0f * std::cos(t0), 5.0f * std::sin(t0)}));
        CHECK_NEAR(out[5].pos, (V2{5.0f * std::cos(t0 + 5.0f * 0.78539816f), 5.0f * std::sin(t0 + 5.0f * 0.78539816f)}));
        CHECK(near(std::sqrt(out[6].pos.x * out[6].pos.x + out[6].pos.y * out[6].pos.y), 15.0f));
        // A single ship: straight ahead (a = 90).
        std::vector<Placed> one;
        const std::vector<uint32_t> l1{0};
        CHECK(screen_arc(objs, l1, 20.0f, a, 0.0f, one) == 30.0f);
        CHECK_NEAR(one[0].pos, (V2{25.0f, 0.0f}));
        // Empty: r unchanged, nothing placed.
        std::vector<Placed> none;
        CHECK(screen_arc(objs, {}, 7.0f, a, 0.0f, none) == 7.0f && none.empty());
    }
    {
        // Lead: R0 = 5; first ring trunc(max(20, 10) / 10) + 1 = 3, then trunc(30 / 10) + 1 = 4; 5 ships -> [3, 2]; r' = 25.
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1, 2, 3, 4};
        CHECK(lead_arc(objs, l, 0.0f, a, 0.0f, out) == 25.0f);
        CHECK(out.size() == 5 && out[0].via == Via::Lead);
        CHECK_NEAR(out[1].pos, (V2{5.0f, 0.0f}));  // the middle of 3: a = 25 + 65 = 90
        // Ring 1 (radius 15), two ships at -65 / +65 degrees.
        const float r65 = 65.0f * (6.28318530717958647692f / 360.0f);
        CHECK_NEAR(out[3].pos, (V2{15.0f * std::cos(r65), -15.0f * std::sin(r65)}));
        CHECK_NEAR(out[4].pos, (V2{15.0f * std::cos(r65), 15.0f * std::sin(r65)}));
    }
    {
        // Transport rows: S = (0, 10) -> width trunc(sqrt 5 + 0.99) = 3; y0 = -W - 200; x0 = -(3 * 0 + 2 * 20) / 2 = -20,
        // step 20; next row y -= 30. Yaw 90: local = world.
        std::vector<Placed> out;
        const std::vector<uint32_t> l{0, 1, 2, 3, 4};
        transport_rows(objs, l, 50.0f, a, 90.0f, out);
        CHECK(out.size() == 5 && out[0].via == Via::Transport);
        CHECK_NEAR(out[0].pos, (V2{-20.0f, -250.0f}));
        CHECK_NEAR(out[2].pos, (V2{20.0f, -250.0f}));
        CHECK_NEAR(out[3].pos, (V2{-10.0f, -280.0f}));  // 2 ships: x0 = -(2 * 0 + 20) / 2 = -10
        CHECK_NEAR(out[4].pos, (V2{10.0f, -280.0f}));
    }
}

void test_group() {
    // The Kashyyyk fleet (CU8K_T04 trace): a capital, a frigate, two X-wing squadrons -> capital grid, screen, lead x 2:
    // four placed objects = four adopt draws, in that order.
    const std::vector<V2> xw{{30.0f, 0.0f}, {0.0f, 15.0f}, {0.0f, -15.0f}, {-30.0f, 30.0f}, {-30.0f, -30.0f}};
    std::vector<FleetObject> objs{squadron(0, 5, 6.0f, 6.0f, xw), ship(5, 200.0f, 60.0f), ship(4, 150.0f, 40.0f),
                                  squadron(0, 5, 6.0f, 6.0f, xw)};
    const V2 a{-3000.0f, 200.0f};
    const float yaw = 90.0f;
    const std::vector<Placed> out = place_group(objs, a, yaw);
    CHECK(out.size() == 4);
    CHECK(out[0].index == 1 && out[0].via == Via::Capital);
    CHECK(out[1].index == 2 && out[1].via == Via::Screen);
    CHECK(out[2].index == 0 && out[2].via == Via::Lead && out[3].index == 3 && out[3].via == Via::Lead);
    // Core: one capital, S = (120, 400) -> W = 60, at the anchor; r = sqrt(2) 60. Screen: s = |(80, 300)|, the frigate
    // ahead at r + s / 2 (the screen returns r' = r + s). Leads: both squadrons on one ring at -65 / +65 degrees around
    // the facing, R0 = r' + s_l / 2.
    CHECK_NEAR(out[0].pos, a);
    const float r = std::sqrt(60.0f * 60.0f + 60.0f * 60.0f);
    const float sf = std::sqrt(80.0f * 80.0f + 300.0f * 300.0f);
    CHECK_NEAR(out[1].pos, (V2{a.x, a.y + r + sf / 2.0f}));
    const V2 sl = list_size(objs, std::vector<uint32_t>{0, 3});
    const float rl = r + sf + std::sqrt(sl.x * sl.x + sl.y * sl.y) / 2.0f;
    const V2 d0 = out[2].pos - a, d3 = out[3].pos - a;
    CHECK(near(std::sqrt(d0.x * d0.x + d0.y * d0.y), rl, 2.0e-3f) && near(std::sqrt(d3.x * d3.x + d3.y * d3.y), rl, 2.0e-3f));
    CHECK(d0.x > 0.0f && d3.x < 0.0f && near(d0.y, d3.y));  // a = 25 first: right of the facing (+y), then the left
    // Members: container + Rot(yaw) . offset (the company offsets on the lead ring).
    CHECK(out[2].members.size() == 5);
    CHECK_NEAR(out[2].members[0], (V2{out[2].pos.x, out[2].pos.y + 30.0f}));
    CHECK_NEAR(out[2].members[1], (V2{out[2].pos.x - 15.0f, out[2].pos.y}));
    CHECK(out[0].members.empty() && out[1].members.empty());

    // Supers at the anchor first; transports in rows behind; unknown classes not placed.
    const std::vector<FleetObject> mix{ship(2, 50.0f, 20.0f), ship(-1, 1.0f, 1.0f), ship(6, 500.0f, 200.0f), ship(5, 100.0f, 50.0f)};
    const std::vector<Placed> m = place_group(mix, a, yaw);
    CHECK(m.size() == 3 && m[0].index == 2 && m[1].index == 3 && m[2].index == 0 && m[2].via == Via::Transport);
    CHECK_NEAR(m[0].pos, a);
    CHECK_NEAR(m[2].pos, (V2{a.x, a.y - 50.0f - 200.0f}));  // W = 50, one transport: x0 = 40 / 2 - 40 / 2 = 0
    // Only transports: they form the grid.
    const std::vector<FleetObject> tr{ship(2, 50.0f, 20.0f), ship(2, 50.0f, 20.0f)};
    const std::vector<Placed> t = place_group(tr, a, yaw);
    CHECK(t.size() == 2 && t[0].via == Via::Capital && t[1].via == Via::Capital);
    CHECK_NEAR(t[0].pos, (V2{a.x - 20.0f, a.y}));
    // Only fighters / bombers: interleaved into the grid core (no screen / lead).
    const std::vector<FleetObject> fb{squadron(0, 1, 5.0f, 5.0f, {}), squadron(1, 1, 5.0f, 5.0f, {}), squadron(0, 1, 5.0f, 5.0f, {})};
    const std::vector<Placed> g = place_group(fb, a, yaw);
    CHECK(g.size() == 3 && g[0].index == 0 && g[1].index == 1 && g[2].index == 2 && g[2].via == Via::Capital);
}

void test_member_offset_type() {
    // The capital grid reads the +0x50 type's offsets unless its Squadron_Units count is below the member count and the
    // company's is not.
    FleetObject o = squadron(0, 2, 1.0f, 1.0f, {{10.0f, 0.0f}, {0.0f, 10.0f}});
    o.offsets_team = {{-10.0f, 0.0f}, {0.0f, -10.0f}};
    o.units_team = 0;
    std::vector<FleetObject> objs{o};
    const std::vector<uint32_t> l{0};
    std::vector<Placed> out;
    (void)capital_grid(objs, l, V2{}, V2{}, 90.0f, out);
    CHECK_NEAR(out[0].members[0], (V2{0.0f, 10.0f}));  // the company offsets, rotated by 90
    objs[0].units_team = 2;
    out.clear();
    (void)capital_grid(objs, l, V2{}, V2{}, 90.0f, out);
    CHECK_NEAR(out[0].members[0], (V2{0.0f, -10.0f}));  // the team type's
    out.clear();
    (void)screen_arc(objs, l, 0.0f, V2{}, 0.0f, out);
    CHECK(out[0].members.size() == 2);
    CHECK_NEAR(out[0].members[0] - out[0].pos, (V2{-10.0f, 0.0f}));  // screen: +0x50, yaw 0
    out.clear();
    transport_rows(objs, l, 0.0f, V2{}, 0.0f, out);
    CHECK(out[0].members.empty());  // no member pass
}

}  // namespace

int main() {
    test_lists_and_rows();
    test_size();
    test_capital_grid();
    test_arcs_and_rows();
    test_group();
    test_member_offset_type();
    if (g_fail) {
        std::printf("alamo_tactical_fleet_placement_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_tactical_fleet_placement_tests: OK\n");
    return 0;
}
