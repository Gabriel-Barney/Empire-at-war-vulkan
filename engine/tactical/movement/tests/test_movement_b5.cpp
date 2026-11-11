// alamo_tactical_movement_b5_tests (no framework; non-zero exit on failure).
// s78n t25.1 (t20 batch 5): the attack approach on land, pure geometry (docs/re/MOVEMENT.md §4.3.2 items 2-4, 6, 7):
// the coefficient tries, the chord step, one ring (0x43b520), the ring layout (0x43d380), the trim point, the fire trim
// (0x410e90) and the slot leg point. Hand numbers are derived in the comments from the spec's rules; each test checks the
// rule's output against them.
// s78n t25.2: the attack block in MovementSystem (MoveOrder::attack): the attack slots 0x438c10 with the slot test 0x43a070,
// the per-formation attack plan (the "every member can fire" stop, the slot leg, the fire trim), the re-plan route, the
// solo rule (§4.2 item 6) and the slot state of Unit (kept by halt, cleared by stop / teleport / plain orders).
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/attack_approach.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/path_around.hpp"
#include "alamo/test/support.hpp"
#include "alamo/test/thread_parallel_for.hpp"

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


// ================================================================ s78n t25.2: MovementSystem scenes (MOVEMENT §4.3.2)
// Open grid of 100 x 60 cells; every scene is translated by kO = (100, 600) (the spec's worked case has the attacker at
// the origin). Attackers are vehicles with foot half extents 15 x 15 (HX = HY = 15). The fire hook is geometric:
// |p - C| <= engage (the host's real one is CombatSystem::can_hit_from, t25.3).
constexpr V2 kO{100.0f, 600.0f};

PassabilityGrid scene_grid() {
    PassabilityGrid g;
    g.init(100, 60);
    return g;
}

void scene_system(MovementSystem& sys) {
    sys.set_grid(scene_grid());
    sys.set_height_fn([](float, float) { return 0.0f; });
}

UnitDesc attacker(ObjectId id, V2 pos, float half = 15.0f) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = 1.0f;
    d.turn_rate = 6.0f;
    d.alt_max_speed = 1.0f;
    d.alt_turn_rate = 6.0f;
    d.accel = d.decel = 0.05f;
    d.movement_class = 2;
    d.collision_class = kUccVehicle;
    d.radius = 12.0f;
    d.hard_x = half;
    d.hard_y = half;
    d.owner = 1;
    d.type_name = "B5_ATTACKER";
    return d;
}

LandAttackUnit entry(ObjectId id, float engage, bool in_range = false) {
    LandAttackUnit e;
    e.id = id;
    e.range = engage;
    e.engage = engage;
    e.in_range = in_range;
    return e;
}

MoveOrder attack_order(std::vector<ObjectId> ids, ObjectId target, V2 c, float t_half, float engage, bool in_range = false) {
    MoveOrder o;
    o.units = ids;
    o.destination = c;
    o.attack.target = target;
    o.attack.c = c;
    o.attack.g = c;
    o.attack.t_hx = o.attack.t_hy = t_half;
    for (const ObjectId id : ids) o.attack.units.push_back(entry(id, engage, in_range));
    return o;
}

void geometric_fire(MovementSystem& sys, V2 c, float engage) {
    sys.set_fire_test([c, engage](ObjectId, V2 p, ObjectId, bool) { return distance(p, c) <= engage; });
}

// §4.3.2 item 9, the unit case: one attacker at the origin, a unit with half extents 5 standing at (1000, 0), engage
// 250 + 5 = 255. R = 255, h = HX = 15 -> ring 240; the first try puts it at theta 0. Plan: G = (1000, 0), v = (1, 0),
// Q = G + 240 rot(-v, 0) = (760, 0), line clear -> q = 240, Q' = (760, 0), the second path start -> Q' points the same way:
// taken, path (0,0) -> (760,0) -> (1000,0), L = 1000. Trim (one member at depth 0): s = 1000 .. 750 pass (|p - C| = 250 <=
// 255), 740 fails (260) -> keep 750: goal (750, 0) (hand).
// rho is 240 to float rounding (the union box is taken on unit(C - centroid): HX = 15 x |fwd . axis| = 15 - 1 ulp-ish).
void test_scene_unit_target() {
    ::alamo::test::section("scene_unit_target");
    MovementSystem sys{MovementConfig{}};
    scene_system(sys);
    CHECK(sys.add_unit(attacker(10, kO)));
    UnitDesc t = attacker(900, kO + V2{1000.0f, 0.0f}, 5.0f);
    t.owner = 2;
    CHECK(sys.add_unit(t));
    const V2 c = kO + V2{1000.0f, 0.0f};
    geometric_fire(sys, c, 255.0f);
    SimRng rng(7);
    sys.move(attack_order({10}, 900, c, 5.0f, 255.0f), rng, 1);
    const UnitView v = sys.view(10);
    CHECK(v.atk_theta == 0.0f && close_to(v.atk_rho, 240.0f, 1e-3f) && v.atk_form == v.form_id && v.atk_form != 0 && v.atk_target == 900);
    CHECK(sys.stats().attack_orders == 1 && sys.stats().attack_slotted == 1 && sys.stats().attack_legs == 1 &&
          sys.stats().attack_leg_refused == 0 && sys.stats().attack_trims == 1);
    CHECK(close2(v.goal, kO + V2{750.0f, 0.0f}, 0.01f) && v.has_goal && !v.arrived);
    CHECK(close_to(sys.stats().attack_trim_sum, 250.0f, 0.01f));
    // the target is not moved and has no block
    CHECK(sys.view(900).atk_form == 0 && sys.view(900).atk_rho == 0.0f);
    std::printf("unit target: theta %.7f rho %.4f goal (%.4f, %.4f), legs %u\n", static_cast<double>(v.atk_theta),
                static_cast<double>(v.atk_rho), static_cast<double>(v.goal.x - kO.x), static_cast<double>(v.goal.y - kO.y),
                sys.stats().attack_legs);
}

// §4.3.2 item 9, the structure case: the target's cells x = 53 .. 57, y = 28 .. 32 (world (1000, 0) +- 50 in the scene
// frame) stamped impassable, half extents 40 x 40, engage 250 + 40 = 290. The line P -> C ends on the stamped cell for
// every P: no slot (rho 0). The plan's G is re-validated to the ring cell of the lead's zone nearest the lead: (940, 0)
// (§4.3.1 item 7); without a fire hook the goal is that point. Trim: |p - C| <= 290 down to s = 710 (p.x = 710), 700 fails
// -> goal (710, 0) (hand: a single member at depth 0; the squad's 730 of the spec is its rear row, t25.1).
void test_scene_structure() {
    ::alamo::test::section("scene_structure");
    for (const bool hook : {false, true}) {
        MovementSystem sys{MovementConfig{}};
        scene_system(sys);
        for (int32_t y = 28; y <= 32; ++y)
            for (int32_t x = 53; x <= 57; ++x) sys.grid().set_base(x, y, kPassImpassable);
        CHECK(sys.add_unit(attacker(10, kO)));
        const V2 c = kO + V2{1000.0f, 0.0f};
        if (hook) geometric_fire(sys, c, 290.0f);
        SimRng rng(7);
        sys.move(attack_order({10}, 901, c, 40.0f, 290.0f), rng, 1);
        const UnitView v = sys.view(10);
        CHECK(v.atk_rho == 0.0f && v.atk_theta == 0.0f && v.atk_form != 0 && v.atk_target == 901);
        CHECK(sys.stats().attack_unslotted == 1 && sys.stats().attack_unslotted_cell == 1 && sys.stats().attack_slotted == 0);
        CHECK(sys.stats().attack_legs == 0 && sys.stats().attack_leg_refused == 0);
        const V2 want = kO + (hook ? V2{710.0f, 0.0f} : V2{940.0f, 0.0f});
        CHECK(close2(v.goal, want, 0.01f) && sys.grid().passable_world(v.goal, sys.mask_of(10)));
        // the unit walks there and stops beside the footprint (never inside it)
        for (uint32_t f = 1; f < 1500 && sys.view(10).has_goal; ++f) sys.tick(f, rng);
        CHECK(sys.view(10).arrived && distance(sys.view(10).pos, want) < 1.0f);
        std::printf("structure (%s hook): rho %.1f goal (%.4f, %.4f), arrived %d\n", hook ? "with" : "no",
                    static_cast<double>(v.atk_rho), static_cast<double>(v.goal.x - kO.x), static_cast<double>(v.goal.y - kO.y),
                    sys.view(10).arrived ? 1 : 0);
    }
}

// Two formations 300 apart across the approach (beyond the recruitment distance 100): the order's L holds both, R = 255,
// one ring 240 (k = 2: h = 15, 255 - 30 >= r_T). Left-most first on the right-hand normal of d0: the y = +150 one (left of
// an approach heading +x) is index 0, the y = -150 one the middle m = 1 at 2 step(15, 0) = +0.0625102; its left neighbour
// at 0.0625102 - 2 step(15, 15) = -0.0625713 (hand, t25.1). The slot points are a chord of 30 = HY + HY apart.
void test_scene_two_formations() {
    ::alamo::test::section("scene_two_formations");
    MovementSystem sys{MovementConfig{}};
    scene_system(sys);
    CHECK(sys.add_unit(attacker(10, kO + V2{0.0f, -150.0f})));
    CHECK(sys.add_unit(attacker(11, kO + V2{0.0f, 150.0f})));
    UnitDesc t = attacker(900, kO + V2{1000.0f, 0.0f}, 5.0f);
    t.owner = 2;
    CHECK(sys.add_unit(t));
    const V2 c = kO + V2{1000.0f, 0.0f};
    SimRng rng(7);
    sys.move(attack_order({10, 11}, 900, c, 5.0f, 255.0f), rng, 1);
    const UnitView a = sys.view(10), b = sys.view(11);
    CHECK(a.form_id != b.form_id && a.atk_form == a.form_id && b.atk_form == b.form_id);
    CHECK(close_to(a.atk_rho, 240.0f, 1e-3f) && a.atk_rho == b.atk_rho && sys.stats().attack_slotted == 2);
    CHECK(close_to(a.atk_theta, 0.0625102f, kTol) && close_to(b.atk_theta, -0.0625713f, kTol));
    // the slot points around C on the reference approach's axis are 30 apart (touching slot rectangles, no overlap)
    // (no hook: the goals are G for both; the leg is laid on each formation's own approach)
    std::printf("two formations: theta %.7f / %.7f, rho %.1f / %.1f, legs %u refused %u\n", static_cast<double>(a.atk_theta),
                static_cast<double>(b.atk_theta), static_cast<double>(a.atk_rho), static_cast<double>(b.atk_rho),
                sys.stats().attack_legs, sys.stats().attack_leg_refused);
    CHECK(sys.stats().attack_legs + sys.stats().attack_leg_refused == 2);
}

// The slot test's object rule (§4.3.2 item 5 step 4): a standing vehicle with half extents 2 x 2 on the theta 0 slot point
// (760, 0): (2 + 5)^2 = 49 >= 0.15 x 15 x 15 = 33.75 fails theta 0; at +-5 deg the member rectangle (20 x 20) still
// overlaps its 7 x 7 (lateral 240 sin 5 deg = 20.9 < 27), at +10 deg (41.7) it is clear -> theta 0.1745329 (hand). A
// blocker with a path record (a goal) is not counted -> theta 0; one without a locomotor counts even with a goal; a
// 0.5 x 0.5 blocker: 5.5^2 = 30.25 < 33.75 passes -> theta 0.
void test_scene_blockers() {
    ::alamo::test::section("scene_blockers");
    const V2 c = kO + V2{1000.0f, 0.0f};
    const V2 slot0 = kO + V2{760.0f, 0.0f};
    struct Case {
        const char* name;
        float half;
        bool moving;
        bool no_loco;
        float want;
    };
    const Case cases[] = {{"standing 2x2", 2.0f, false, false, 10.0f * kDegToRad},
                          {"moving 2x2", 2.0f, true, false, 0.0f},
                          {"no locomotor 2x2 with a goal", 2.0f, true, true, 10.0f * kDegToRad},
                          {"standing 0.5x0.5", 0.5f, false, false, 0.0f}};
    for (const Case& k : cases) {
        MovementSystem sys{MovementConfig{}};
        scene_system(sys);
        CHECK(sys.add_unit(attacker(10, kO)));
        UnitDesc t = attacker(900, c, 5.0f);
        t.owner = 2;
        CHECK(sys.add_unit(t));
        UnitDesc bl = attacker(50, slot0, k.half);
        bl.owner = 3;
        bl.no_locomotor = k.no_loco;
        CHECK(sys.add_unit(bl));
        SimRng rng(7);
        if (k.moving) {
            MoveOrder m;
            m.units = {50};
            m.destination = kO + V2{760.0f, 400.0f};
            sys.move(m, rng, 1);
            CHECK(sys.view(50).has_goal);
        }
        sys.move(attack_order({10}, 900, c, 5.0f, 255.0f), rng, 1);
        const UnitView v = sys.view(10);
        CHECK(close_to(v.atk_rho, 240.0f, 1e-3f) && close_to(v.atk_theta, k.want, 1e-6f));
        std::printf("blocker %s: theta %.7f (want %.7f)\n", k.name, static_cast<double>(v.atk_theta), static_cast<double>(k.want));
    }
}

// Item 6 first bullet: every member can fire from where it stands -> no path, has_goal false, arrived, the block kept
// (not in L: no slot test). A unit that was moving keeps its speed and brakes through End Move (halt()).
void test_scene_all_in_range() {
    ::alamo::test::section("scene_all_in_range");
    MovementSystem sys{MovementConfig{}};
    scene_system(sys);
    CHECK(sys.add_unit(attacker(10, kO)));
    UnitDesc t = attacker(900, kO + V2{400.0f, 0.0f}, 5.0f);
    t.owner = 2;
    CHECK(sys.add_unit(t));
    SimRng rng(7);
    MoveOrder m;
    m.units = {10};
    m.destination = kO + V2{300.0f, 0.0f};
    sys.move(m, rng, 1);
    uint32_t f = 1;
    for (; f < 400 && !(sys.view(10).speed > 0.5f); ++f) sys.tick(f, rng);
    const float v0 = sys.view(10).speed;
    CHECK(v0 > 0.5f);
    const V2 c = sys.view(10).pos + V2{200.0f, 0.0f};  // the target point within 255 of the mover
    geometric_fire(sys, c, 255.0f);
    sys.move(attack_order({10}, 900, c, 5.0f, 255.0f, true), rng, f);
    const UnitView v = sys.view(10);
    CHECK(!v.has_goal && v.arrived && v.atk_form != 0 && v.atk_rho == 0.0f && v.speed == v0);
    CHECK(sys.stats().attack_stops == 1 && sys.stats().attack_in_range == 1 && sys.stats().attack_slotted == 0);
    uint32_t braked = 0;
    for (; f < 800 && sys.view(10).speed > 0.0f; ++f, ++braked) sys.tick(f, rng);
    CHECK(sys.view(10).speed == 0.0f && braked > 2 && !sys.view(10).has_goal && sys.view(10).atk_form == v.atk_form);
    std::printf("all in range: stopped at speed %.3f, braked %u frames\n", static_cast<double>(v0), braked);
}

// Item 8: a re-plan keeps the slot (bit-identical theta / rho), the formation (atk_form) and runs no slot test, no
// composition and no RNG draw; the goal follows the new C.
void test_scene_replan() {
    ::alamo::test::section("scene_replan");
    MovementSystem sys{MovementConfig{}};
    scene_system(sys);
    CHECK(sys.add_unit(attacker(10, kO)));
    UnitDesc t = attacker(900, kO + V2{1000.0f, 0.0f}, 5.0f);
    t.owner = 2;
    CHECK(sys.add_unit(t));
    V2 c = kO + V2{1000.0f, 0.0f};
    geometric_fire(sys, c, 255.0f);
    SimRng rng(7);
    sys.move(attack_order({10}, 900, c, 5.0f, 255.0f), rng, 1);
    for (uint32_t f = 1; f < 120; ++f) sys.tick(f, rng);
    const UnitView a = sys.view(10);
    const MovementStats s0 = sys.stats();
    c = c + V2{0.0f, 200.0f};
    CHECK(sys.teleport(900, c, 0.0f, 0.0f, 120));
    geometric_fire(sys, c, 255.0f);
    MoveOrder o = attack_order({10}, 900, c, 5.0f, 255.0f);
    o.attack.replan = true;
    const SimRng before = rng;
    const size_t pend = sys.pending_compositions();
    sys.move(o, rng, 120);
    const UnitView b = sys.view(10);
    CHECK(rng == before && sys.pending_compositions() == pend);
    CHECK(b.atk_theta == a.atk_theta && b.atk_rho == a.atk_rho && b.atk_form == a.atk_form && b.form_id == a.atk_form);
    CHECK(sys.stats().attack_slotted == s0.attack_slotted && sys.stats().attack_replans == 1 && sys.stats().attack_orders == 1);
    CHECK(b.has_goal && distance(b.goal, c) <= 255.0f + 0.01f && !(b.goal == a.goal) && b.start_frame == 120);
    std::printf("re-plan: theta %.7f rho %.1f kept, goal (%.2f, %.2f) -> (%.2f, %.2f)\n", static_cast<double>(b.atk_theta),
                static_cast<double>(b.atk_rho), static_cast<double>(a.goal.x), static_cast<double>(a.goal.y),
                static_cast<double>(b.goal.x), static_cast<double>(b.goal.y));
}

// §4.2 item 6: two Standard vehicles 50 apart go solo (two formations) when the 3-D distance to the target is below
// r_seed + E_r + 60 = 21.2 + 255 + 60; far away they form one formation of two.
void test_scene_solo() {
    ::alamo::test::section("scene_solo");
    for (const float d : {300.0f, 1500.0f}) {
        MovementSystem sys{MovementConfig{}};
        scene_system(sys);
        CHECK(sys.add_unit(attacker(10, kO)));
        CHECK(sys.add_unit(attacker(11, kO + V2{0.0f, 50.0f})));
        SimRng rng(7);
        sys.move(attack_order({10, 11}, 901, kO + V2{d, 0.0f}, 5.0f, 255.0f), rng, 1);
        const bool two = sys.view(10).form_id != sys.view(11).form_id;
        CHECK(two == (d < 336.0f));
        std::printf("solo rule: target at %.0f -> %s\n", static_cast<double>(d), two ? "two formations" : "one formation");
    }
}

// Orders without the block: stop() and a plain move clear the slot, halt() keeps it.
void test_scene_plain_orders() {
    ::alamo::test::section("scene_plain_orders");
    MovementSystem sys{MovementConfig{}};
    scene_system(sys);
    CHECK(sys.add_unit(attacker(10, kO)));
    const V2 c = kO + V2{1000.0f, 0.0f};
    SimRng rng(7);
    sys.move(attack_order({10}, 901, c, 5.0f, 255.0f), rng, 1);
    CHECK(close_to(sys.view(10).atk_rho, 240.0f, 1e-3f));
    const ObjectId one[] = {10};
    sys.halt(one, 1);
    CHECK(close_to(sys.view(10).atk_rho, 240.0f, 1e-3f) && sys.view(10).atk_form != 0 && !sys.view(10).has_goal);
    sys.stop(one);
    CHECK(sys.view(10).atk_rho == 0.0f && sys.view(10).atk_form == 0 && sys.view(10).atk_target == -1);
    sys.move(attack_order({10}, 901, c, 5.0f, 255.0f), rng, 2);
    CHECK(close_to(sys.view(10).atk_rho, 240.0f, 1e-3f));
    MoveOrder m;
    m.units = {10};
    m.destination = kO + V2{0.0f, 300.0f};
    sys.move(m, rng, 3);
    CHECK(sys.view(10).atk_rho == 0.0f && sys.view(10).atk_form == 0 && sys.view(10).has_goal);
    sys.move(attack_order({10}, 901, c, 5.0f, 255.0f), rng, 4);
    CHECK(sys.teleport(10, kO, 0.0f, 0.0f, 4) && sys.view(10).atk_rho == 0.0f && sys.view(10).atk_form == 0);
    CHECK(sys.stats().attack_orders == 3);
}

// Serial and 4 workers give the same digest every tick of the unit-target approach (two attackers, a slot each).
std::vector<uint64_t> run_approach(const ParallelFor& pf) {
    MovementSystem sys{MovementConfig{}};
    scene_system(sys);
    CHECK(sys.add_unit(attacker(10, kO + V2{0.0f, -150.0f})));
    CHECK(sys.add_unit(attacker(11, kO + V2{0.0f, 150.0f})));
    UnitDesc t = attacker(900, kO + V2{1000.0f, 0.0f}, 5.0f);
    t.owner = 2;
    CHECK(sys.add_unit(t));
    const V2 c = kO + V2{1000.0f, 0.0f};
    geometric_fire(sys, c, 255.0f);
    SimRng rng(7);
    sys.move(attack_order({10, 11}, 900, c, 5.0f, 255.0f), rng, 1);
    std::vector<uint64_t> out;
    for (uint32_t f = 1; f < 900; ++f) {
        sys.tick(f, rng, pf);
        StateHasher h;
        sys.hash_units(h);
        out.push_back(h.digest());
    }
    CHECK(sys.view(10).arrived && sys.view(11).arrived && distance(sys.view(10).pos, c) <= 256.0f &&
          distance(sys.view(11).pos, c) <= 256.0f);
    return out;
}

void test_scene_workers() {
    ::alamo::test::section("scene_workers");
    const std::vector<uint64_t> s = run_approach(serial_for);
    const std::vector<uint64_t> p = run_approach(::alamo::test::make_thread_parallel_for(4));
    CHECK(s == p);
    std::printf("approach: serial = 4 workers over %zu ticks: %d\n", s.size(), s == p ? 1 : 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash keeps the log
    test_arc_tries();
    test_arc_step();
    test_ring_place();
    test_ring_place_search();
    test_ring_layout();
    test_trim_point();
    test_fire_trim();
    test_slot_leg();
    test_scene_unit_target();     // s78n t25.2
    test_scene_structure();
    test_scene_two_formations();
    test_scene_blockers();
    test_scene_all_in_range();
    test_scene_replan();
    test_scene_solo();
    test_scene_plain_orders();
    test_scene_workers();
    return ::alamo::test::report("alamo_tactical_movement_b5_tests");
}
