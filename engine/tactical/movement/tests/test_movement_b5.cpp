// alamo_tactical_movement_b5_tests (no framework; non-zero exit on failure).
// s78n t25.1 (t20 batch 5): the attack approach on land, pure geometry (docs/re/MOVEMENT.md §4.3.2 items 2-4, 6, 7):
// the coefficient tries, the chord step, one ring (0x43b520), the ring layout (0x43d380), the trim point, the fire trim
// (0x410e90) and the slot leg point. Hand numbers are derived in the comments from the spec's rules; each test checks the
// rule's output against them.
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "alamo/tactical/movement/attack_approach.hpp"
#include "alamo/tactical/movement/path_around.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;

namespace {

bool close_to(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
bool close2(V2 a, V2 b, float tol) { return close_to(a.x, b.x, tol) && close_to(a.y, b.y, tol); }

constexpr float kTol = 2e-6f;  // float acos near 1 is good to about 1e-6 rad at these ring sizes

// §4.3.2 item 2: f = 0.2, f -= 0.2 while f > -1.0 in float32 -> six tries; item 3: A = (1 - f) x 90 deg.
void test_arc_tries() {
    ::alamo::test::section("arc_tries");
    const std::vector<float> t = attack_arc_tries();
    CHECK(t.size() == 6);
    const float want[6] = {0.2f, 0.0f, -0.2f, -0.4f, -0.6f, -0.8f};
    const float deg[6] = {72.0f, 90.0f, 108.0f, 126.0f, 144.0f, 162.0f};
    for (size_t i = 0; i < 6 && i < t.size(); ++i) {
        CHECK(close_to(t[i], want[i], 1e-6f));
        CHECK(close_to(attack_half_arc(t[i]) * kRadToDeg, deg[i], 1e-4f));
    }
    CHECK(t.size() == 6 && t[1] == 0.0f);
    std::printf("arc tries: %zu, last %.9g\n", t.size(), t.empty() ? 0.0 : static_cast<double>(t.back()));
}

// §4.3.2 item 4: step(a, b) = acos(sqrt(rho^2 - (a + b)^2 / 4) / rho) = asin((a + b) / (2 rho)).
// (15, 15, 240): asin(0.0625) = 0.0625408; (15, 0, 240): asin(0.03125) = 0.0312551 (hand).
void test_arc_step() {
    ::alamo::test::section("arc_step");
    const float a = arc_step(15.0f, 15.0f, 240.0f);
    const float b = arc_step(15.0f, 0.0f, 240.0f);
    CHECK(close_to(a, 0.0625408f, kTol));
    CHECK(close_to(b, 0.0312551f, kTol));
    CHECK(arc_step(0.0f, 0.0f, 240.0f) == 0.0f);
    CHECK(std::isnan(arc_step(300.0f, 300.0f, 240.0f)));  // a + b > 2 rho: NaN, every |theta| <= A test then fails
    std::printf("arc_step(15,15,240) %.7f, arc_step(15,0,240) %.7f\n", static_cast<double>(a), static_cast<double>(b));
}

// The approach in these scenes runs along +x onto C = (1000, 0): d0 = d1 = (1, 0), u = -d1 = (-1, 0). The right-hand
// normal of d0 is (0, -1), so the left-most formation is the one with the largest y.
constexpr V2 kC{1000.0f, 0.0f};
constexpr V2 kU{-1.0f, 0.0f};
constexpr V2 kD0{1.0f, 0.0f};

std::vector<RingForm> forms_left_to_right(uint32_t n, float hx, float hy) {
    // Given right-most first, so the sort has work to do: form i sits at y = 30 i (index n - 1 is the left-most).
    std::vector<RingForm> f(n);
    for (uint32_t i = 0; i < n; ++i) {
        f[i].index = i;
        f[i].id = static_cast<ObjectId>(100 + i);
        f[i].centroid = {0.0f, 30.0f * static_cast<float>(i)};
        f[i].hx = hx;
        f[i].hy = hy;
    }
    return f;
}

// §4.3.2 item 4, every test passing, HY 15, rho_k 240, A 72 deg (hand, s = step(15, 15) = 0.0625408, s0 = step(15, 0)):
// n = 1: 0. n = 2: middle (right) 2 s0 = 0.0625102, left 2 s0 - 2 s = -0.0625713. n = 3: -2 s, 0, +2 s = +-0.1250815.
// n = 4: middle (index 2) 0.0625102, index 1 at -0.0625713, index 0 at -0.0625713 - s = -0.1251121 (factor 1), index 3 at
// 0.0625102 + 2 s = 0.1875917: the factor 2 is only on the first neighbour, so the ring is not symmetric.
void test_ring_place() {
    ::alamo::test::section("ring_place");
    const float a72 = attack_half_arc(0.2f);
    const SlotTestFn all = [](const RingForm&, V2) { return true; };
    const float want[5][4] = {
        {0, 0, 0, 0},
        {0.0f, 0, 0, 0},
        {0.0625102f, -0.0625713f, 0, 0},
        {0.1250815f, 0.0f, -0.1250815f, 0},
        {0.1875917f, 0.0625102f, -0.0625713f, -0.1251121f},
    };
    for (uint32_t n = 1; n <= 4; ++n) {
        std::vector<RingForm> f = forms_left_to_right(n, 15.0f, 15.0f);
        CHECK(ring_place(f, kC, kU, kD0, 240.0f, a72, all, false));
        bool untouched = true;
        for (const RingForm& r : f) untouched = untouched && r.theta == 0.0f && r.rho == 0.0f;
        CHECK(untouched);  // no commit: nothing written
        CHECK(ring_place(f, kC, kU, kD0, 240.0f, a72, all, true));
        // f[i] is at y = 30 i: f[0] is the right-most (largest angle), f[n - 1] the left-most (most negative angle).
        for (uint32_t i = 0; i < n; ++i) {
            CHECK(close_to(f[i].theta, want[n][i], kTol));
            CHECK(f[i].rho == 240.0f);
        }
        std::printf("ring n=%u:", n);
        for (const RingForm& r : f) std::printf(" %.7f", static_cast<double>(r.theta));
        std::printf("\n");
    }
    // A negative angle lies on the approach's left (+y), where the left-most formation stands.
    {
        std::vector<RingForm> f = forms_left_to_right(2, 15.0f, 15.0f);
        CHECK(ring_place(f, kC, kU, kD0, 240.0f, a72, all, true));
        CHECK(arc_point(kC, kU, f[1].rho, f[1].theta).y > 0.0f && arc_point(kC, kU, f[0].rho, f[0].theta).y < 0.0f);
    }
    // The comparator is the right-normal projection, not the input order: the same forms given left-most first.
    {
        std::vector<RingForm> f = forms_left_to_right(3, 15.0f, 15.0f);
        std::swap(f[0], f[2]);
        CHECK(ring_place(f, kC, kU, kD0, 240.0f, a72, all, true));
        CHECK(close_to(f[0].theta, -0.1250815f, kTol) && f[1].theta == 0.0f && close_to(f[2].theta, 0.1250815f, kTol));
    }
    // Equal projections: the id breaks the tie (lower id first = left).
    {
        std::vector<RingForm> f = forms_left_to_right(2, 15.0f, 15.0f);
        f[0].centroid = f[1].centroid;
        f[0].id = 7;
        f[1].id = 5;
        CHECK(ring_place(f, kC, kU, kD0, 240.0f, a72, all, true));
        CHECK(close_to(f[1].theta, -0.0625713f, kTol) && close_to(f[0].theta, 0.0625102f, kTol));
    }
}

// §4.3.2 item 4, the 5 deg search: a lone formation blocked at 0, +5 and -5 deg takes +10 deg (0.1745329, hand: the
// order is 0, +5, -5, +10, ...). Blocked everywhere within +-72 deg: the ring fails and nothing is written.
void test_ring_place_search() {
    ::alamo::test::section("ring_place_search");
    const float a72 = attack_half_arc(0.2f);
    std::vector<V2> tried;
    // |P.y| at 240: 0, 20.9 (5 deg), 41.7 (10 deg): blocked below 30.
    const SlotTestFn mid = [&](const RingForm&, V2 p) {
        tried.push_back(p);
        return std::fabs(p.y) >= 30.0f;
    };
    std::vector<RingForm> f = forms_left_to_right(1, 15.0f, 15.0f);
    CHECK(ring_place(f, kC, kU, kD0, 240.0f, a72, mid, true));
    CHECK(close_to(f[0].theta, 0.1745329f, kTol) && f[0].rho == 240.0f);
    CHECK(tried.size() == 4);
    if (tried.size() == 4) CHECK(tried[0].y == 0.0f && tried[1].y < -20.0f && tried[2].y > 20.0f && tried[3].y < -40.0f);
    std::printf("lone blocked at 0 / +-5 deg: theta %.7f after %zu tests\n", static_cast<double>(f[0].theta), tried.size());

    std::vector<RingForm> g = forms_left_to_right(1, 15.0f, 15.0f);
    const SlotTestFn none = [](const RingForm&, V2) { return false; };
    CHECK(!ring_place(g, kC, kU, kD0, 240.0f, a72, none, true));
    CHECK(g[0].theta == 0.0f && g[0].rho == 0.0f);

    // Even n: the middle is tried at theta_0, then -theta_0. The middle (the right one of two) blocked on the right side
    // takes -theta_0 = -0.0625102; its left neighbour then starts at -0.0625102 - 2 step(15, 15) = -0.1875918 (hand).
    std::vector<RingForm> h = forms_left_to_right(2, 15.0f, 15.0f);
    const SlotTestFn right_blocked = [](const RingForm& r, V2 p) { return !(r.index == 0 && p.y < 0.0f); };
    CHECK(ring_place(h, kC, kU, kD0, 240.0f, a72, right_blocked, true));
    CHECK(close_to(h[0].theta, -0.0625102f, kTol) && close_to(h[1].theta, -0.1875918f, kTol));
    // Even n whose middle starts beyond A fails at once: HY 150 on a ring of 100 (2 asin(0.75) = 1.696 > 72 deg).
    std::vector<RingForm> k = forms_left_to_right(2, 15.0f, 150.0f);
    CHECK(!ring_place(k, kC, kU, kD0, 100.0f, a72, {}, true));
    // A neighbour pushed below -A fails: the left one of two blocked everywhere on the left.
    std::vector<RingForm> e = forms_left_to_right(2, 15.0f, 15.0f);
    const SlotTestFn left_blocked = [](const RingForm& r, V2) { return r.index != 1; };
    CHECK(!ring_place(e, kC, kU, kD0, 240.0f, a72, left_blocked, true));
}

// §4.3.2 item 3, test always true, R 255, r_T = sqrt(50) = 7.0711, f 0.2 (A 72 deg, R_min 51):
// three forms HX 15, HY 150 (rear-most first): k = 3 fails (left neighbour at -2 asin(150 / 240) = -77.36 deg), so forms
// 1-2 go on ring 255 - 15 = 240 (middle 2 asin(75 / 240) = +0.6356 on the right, left one -0.7146) and form 3 on ring
// 210 (rho := 255 - 2 x 15 = 225, rho_k 210, theta 0) (hand). Three forms HX 15, HY 15: all on ring 240.
void test_ring_layout() {
    ::alamo::test::section("ring_layout");
    const float rt = std::sqrt(50.0f);
    const SlotTestFn all = [](const RingForm&, V2) { return true; };
    {
        std::vector<RingForm> l = forms_left_to_right(3, 15.0f, 150.0f);
        l[0].centroid = {-100.0f, -50.0f};  // rear-most, right
        l[1].centroid = {-90.0f, 50.0f};    // left
        l[2].centroid = {-10.0f, 0.0f};     // front
        CHECK(ring_layout(l, kC, kU, kD0, 255.0f, 0.2f, rt, all, false));
        CHECK(l[0].rho == 0.0f && l[1].rho == 0.0f && l[2].rho == 0.0f);  // no commit: nothing written
        CHECK(ring_layout(l, kC, kU, kD0, 255.0f, 0.2f, rt, all, true));
        CHECK(l[0].rho == 240.0f && l[1].rho == 240.0f && l[2].rho == 210.0f);
        CHECK(close_to(l[0].theta, 0.6356f, 1e-4f) && close_to(l[1].theta, -0.7146f, 1e-4f) && l[2].theta == 0.0f);
        std::printf("ring layout HY 150: (%.4f, %.0f) (%.4f, %.0f) (%.4f, %.0f)\n", static_cast<double>(l[0].theta),
                    static_cast<double>(l[0].rho), static_cast<double>(l[1].theta), static_cast<double>(l[1].rho),
                    static_cast<double>(l[2].theta), static_cast<double>(l[2].rho));
    }
    {
        std::vector<RingForm> l = forms_left_to_right(3, 15.0f, 15.0f);
        CHECK(ring_layout(l, kC, kU, kD0, 255.0f, 0.2f, rt, all, true));
        CHECK(l[0].rho == 240.0f && l[1].rho == 240.0f && l[2].rho == 240.0f);
    }
    // rho := rho - 2h with h of the LAST prefix examined: HX 15, 15, 40 (HY 150): k = 3 (h 40, rho_k 215) fails (left
    // neighbour -2 asin(300 / 430) = -88.4 deg), forms 1-2 go on ring 240, rho := 255 - 80 = 175 (not 255 - 30 = 225), and
    // form 3 lands on ring 175 - 40 = 135 (hand).
    {
        std::vector<RingForm> l = forms_left_to_right(3, 15.0f, 150.0f);
        l[2].hx = 40.0f;
        CHECK(ring_layout(l, kC, kU, kD0, 255.0f, 0.2f, rt, all, true));
        CHECK(l[0].rho == 240.0f && l[1].rho == 240.0f && l[2].rho == 135.0f);
    }
    // rho < R_min stops: f 0.6 (A 36 deg, R_min 153), HX 40, HY 150: two forms never share a ring (2 asin(75 / 215) =
    // 40.8 deg > 36), form 1 on 215, rho := 175, form 2 on 135, rho := 95 < 153: a third form is left -> fail (hand).
    {
        std::vector<RingForm> two = forms_left_to_right(2, 40.0f, 150.0f);
        CHECK(ring_layout(two, kC, kU, kD0, 255.0f, 0.6f, rt, all, true));
        CHECK(two[0].rho == 215.0f && two[1].rho == 135.0f);
        std::vector<RingForm> three = forms_left_to_right(3, 40.0f, 150.0f);
        CHECK(!ring_layout(three, kC, kU, kD0, 255.0f, 0.6f, rt, all, false));
    }
    // rho - 2h < r_T: one formation too deep for the ring stops at k = 1 -> the try fails.
    {
        std::vector<RingForm> l = forms_left_to_right(1, 130.0f, 15.0f);
        CHECK(!ring_layout(l, kC, kU, kD0, 255.0f, 0.2f, rt, all, false));
    }
    // A slot test that fails everywhere: no ring fits -> fail; nothing written by the commit run.
    {
        std::vector<RingForm> l = forms_left_to_right(2, 15.0f, 15.0f);
        CHECK(!ring_layout(l, kC, kU, kD0, 255.0f, 0.2f, rt, [](const RingForm&, V2) { return false; }, true));
        CHECK(l[0].rho == 0.0f && l[1].rho == 0.0f);
    }
}

// The member slot position on a path (§4.3.2 item 7) on (0,0), (10,0), (10,10): the right of (1,0) is (0,-1), the right
// of (0,1) is (1,0) (formation_right). s 12, depth -4, lateral 1 -> q 8 -> (8, -1); s 14, depth 0, lateral 1 -> (11, 4);
// s 20, depth +5 -> beyond L, not tested, with extend (10, 15); s 2, depth -6 -> q -4 -> (-4, 0) (hand).
void test_trim_point() {
    ::alamo::test::section("trim_point");
    const std::vector<V2> path{{0, 0}, {10, 0}, {10, 0}, {10, 10}};  // with a zero-length segment, which is skipped
    V2 p;
    CHECK(trim_point(path, 12.0f, -4.0f, 1.0f, false, p) && close2(p, {8, -1}, 1e-5f));
    CHECK(trim_point(path, 14.0f, 0.0f, 1.0f, false, p) && close2(p, {11, 4}, 1e-5f));
    CHECK(!trim_point(path, 20.0f, 5.0f, 0.0f, false, p));
    CHECK(trim_point(path, 20.0f, 5.0f, 0.0f, true, p) && close2(p, {10, 15}, 1e-5f));
    CHECK(trim_point(path, 2.0f, -6.0f, 0.0f, false, p) && close2(p, {-4, 0}, 1e-5f));
    CHECK(trim_point(path, 20.0f, 0.0f, 0.0f, false, p) && close2(p, {10, 10}, 1e-5f));  // q = L exactly: tested
    const std::vector<V2> empty;
    CHECK(!trim_point(empty, 0.0f, 0.0f, 0.0f, true, p));
    const std::vector<V2> dot1{{3, 4}};
    CHECK(trim_point(dot1, 0.0f, 0.0f, 5.0f, false, p) && close2(p, {3.0f, 4.0f}, 0.0f));
    CHECK(!trim_point(dot1, 0.0f, 1.0f, 0.0f, false, p));
}

// 0x410e90 (§4.3.2 item 7, worked case item 9), path (0,0) -> (1000,0), the member can fire when |p - (1000,0)| <= 255:
// one member at depth 0 passes from s = 1000 down to 750 and fails at 740 -> keep 750; with a rear row at depth -15 the
// rear row fails at s = 750 (p 735) -> keep 760. The structure case: path (0,0) -> (940,0), |p - (1000,0)| <= 290, the
// rear row passes down to s = 730 (p 715, 285) and fails at 720 -> keep 730. A test failing at s = L cuts nothing
// (keep L); always true -> keep 0 (the first node) (hand).
void test_fire_trim() {
    ::alamo::test::section("fire_trim");
    const V2 t{1000.0f, 0.0f};
    const auto within = [&](float r) { return [t, r](const TrimMember&, V2 p) { return distance(p, t) <= r; }; };
    const std::vector<V2> path{{0, 0}, {1000, 0}};
    const std::vector<TrimMember> one{{0, 0.0f, 0.0f, true}};
    const std::vector<TrimMember> squad{{0, 0.0f, 0.0f, true}, {1, -15.0f, 0.0f, true}};
    const float k1 = fire_trim(path, one, false, 0.0f, within(255.0f));
    const float k2 = fire_trim(path, squad, false, 0.0f, within(255.0f));
    CHECK(k1 == 750.0f);
    CHECK(k2 == 760.0f);
    const std::vector<V2> spath{{0, 0}, {940, 0}};
    const float k3 = fire_trim(spath, squad, false, 0.0f, within(290.0f));
    CHECK(k3 == 730.0f);
    CHECK(fire_trim(path, squad, false, 0.0f, [](const TrimMember&, V2) { return false; }) == 1000.0f);
    CHECK(fire_trim(path, squad, false, 0.0f, [](const TrimMember&, V2) { return true; }) == 0.0f);
    std::printf("fire trim: one member keeps %.1f, with the rear row %.1f, structure case %.1f\n", static_cast<double>(k1),
                static_cast<double>(k2), static_cast<double>(k3));
    // The cut with the existing record cut 0x54e780: keep 760 -> (0,0), (760,0); keep 0 -> the first node only.
    {
        std::vector<V2> pts = path;
        CHECK(cut_points(pts, k2) && pts.size() == 2 && close2(pts[1], {760.0f, 0.0f}, 1e-3f));
        std::vector<V2> first = path;
        cut_points(first, 0.0f);
        CHECK(first.size() == 1);
    }
    // A member beyond L (depth +20) is not tested while s + 20 > L: the test never sees a point past the end; it passes
    // down to s = 730 (p 750) and fails at 720 -> keep 730 (hand).
    {
        float max_x = 0.0f;
        const std::vector<TrimMember> ahead{{0, 20.0f, 0.0f, true}};
        const float k = fire_trim(path, ahead, false, 0.0f, [&](const TrimMember&, V2 p) {
            max_x = std::fmax(max_x, p.x);
            return distance(p, t) <= 255.0f;
        });
        CHECK(k == 730.0f && max_x == 1000.0f);
    }
    // Untested members are skipped; the lateral offset is applied (right of +x is -y).
    {
        const std::vector<TrimMember> side{{0, 0.0f, 10.0f, true}, {1, 0.0f, 0.0f, false}};
        bool saw_untested = false;
        float y = 0.0f;
        fire_trim(path, side, false, 0.0f, [&](const TrimMember& m, V2 p) {
            saw_untested = saw_untested || m.member == 1;
            y = p.y;
            return true;
        });
        CHECK(!saw_untested && y == -10.0f);
    }
    // Guard (the [L] reading of item 7): path (0,0) -> (400,0), guard range 150, |p - (400,0)| <= 150 -> keep 250; a member
    // failing at the path end keeps max(0, L - 150) = 250 instead of L (hand).
    {
        const std::vector<V2> gpath{{0, 0}, {400, 0}};
        const V2 gt{400.0f, 0.0f};
        CHECK(fire_trim(gpath, one, true, 150.0f, [gt](const TrimMember&, V2 p) { return distance(p, gt) <= 150.0f; }) == 250.0f);
        CHECK(fire_trim(gpath, one, true, 150.0f, [](const TrimMember&, V2) { return false; }) == 250.0f);
        CHECK(fire_trim(gpath, one, true, 500.0f, [](const TrimMember&, V2) { return false; }) == 0.0f);
    }
}

// §4.3.2 item 6: Q' = G + q rot(-v, theta); G (1000, 0), v (1, 0), theta 0.3, q 240 -> (1000 - 240 cos 0.3, -240 sin 0.3)
// = (770.7192, -70.9248) (hand). q <= 0: no leg.
void test_slot_leg() {
    ::alamo::test::section("slot_leg");
    V2 q;
    CHECK(slot_leg_point({1000.0f, 0.0f}, {1.0f, 0.0f}, 0.3f, 240.0f, q));
    CHECK(close2(q, {770.7192f, -70.9248f}, 1e-3f));
    CHECK(!slot_leg_point({1000.0f, 0.0f}, {1.0f, 0.0f}, 0.3f, 0.0f, q));
    // rot / arc_point: the ring point at theta 0 is C + rho u; rot by pi/2 turns (1, 0) into (0, 1).
    CHECK(close2(arc_point(kC, kU, 240.0f, 0.0f), {760.0f, 0.0f}, 1e-4f));
    CHECK(close2(rot({1.0f, 0.0f}, dmath::kHalfPiF), {0.0f, 1.0f}, 1e-6f));
    std::printf("slot leg Q' (%.4f, %.4f)\n", static_cast<double>(q.x), static_cast<double>(q.y));
}

}  // namespace

int main() {
    test_arc_tries();
    test_arc_step();
    test_ring_place();
    test_ring_place_search();
    test_ring_layout();
    test_trim_point();
    test_fire_trim();
    test_slot_leg();
    return ::alamo::test::report("alamo_tactical_movement_b5_tests");
}
