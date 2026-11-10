// alamo_tactical_movement_gaps_tests (no framework; non-zero exit on failure).
// docs/re/REMAINING_GAPS.md §4.1-4.7: corridor squeeze, threat mode (space ops 25-32, land region cost), and the
// jetpack, flying, bike, land-bomber and fighter locomotors; determinism of a mixed scenario across 1 / 4 / N threads
// with a golden CRC.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/corridor.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

using ::alamo::test::g_fail;

static ParallelFor make_thread_pf(unsigned threads) { return ::alamo::test::make_thread_parallel_for(threads); }

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

static PassabilityGrid open_grid(uint32_t w, uint32_t h) {
    PassabilityGrid g;
    g.init(w, h);
    return g;
}

static UnitDesc unit(ObjectId id, V2 pos, int32_t cls, float speed, float radius, LocomotorKind kind) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = speed;
    d.turn_rate = 6.0f;
    d.alt_max_speed = speed;  // S7 t14: a jetpack flies with the alternates; the same values keep these scenarios
    d.alt_turn_rate = 6.0f;
    d.movement_class = cls;
    d.collision_class = kUccInfantry;
    d.radius = radius;
    d.type_name = "GAPS_UNIT";
    d.locomotor = kind;
    return d;
}

// ---------------------------------------------------------------- §4.1 corridor squeeze
static void test_corridor() {
    // 60 x 30 cells; a channel along x between wall rows y = 10 and y = 16 for x in [20, 40): free world y (210, 310).
    PassabilityGrid g = open_grid(60, 30);
    for (int32_t x = 20; x < 40; ++x) {
        g.set_base(x, 10, kPassImpassable);
        g.set_base(x, 16, kPassImpassable);
    }
    // Rays: exact hit distance to the wall face, full length when clear, 0 from inside a blocked cell.
    CHECK(near(passability_ray(g, {500.0f, 260.0f}, {0.0f, 1.0f}, 100.0f, kPassClear), 50.0f, 1e-3f));
    CHECK(near(passability_ray(g, {500.0f, 260.0f}, {0.0f, -1.0f}, 100.0f, kPassClear), 50.0f, 1e-3f));
    CHECK(passability_ray(g, {100.0f, 260.0f}, {0.0f, 1.0f}, 100.0f, kPassClear) == 100.0f);
    CHECK(passability_ray(g, {500.0f, 200.0f}, {0.0f, 1.0f}, 100.0f, kPassClear) == 0.0f);

    CorridorParams p;
    p.width = 200.0f;
    p.depth_cells = 4.0f;
    p.type = kCorridorProbe;
    p.mask = kPassClear;
    const std::vector<V2> centred = {{0.0f, 260.0f}, {1100.0f, 260.0f}};
    const CorridorSqueeze c = squeeze_corridor(g, centred, p);
    CHECK(c.samples.size() == static_cast<size_t>(1100.0f / kCorridorSampleStep) + 1);
    CHECK(c.table.size() >= 2 && c.table[0] == 10.0f);
    float wmin = 1e9f;
    size_t first_narrow = c.width.size();
    for (size_t i = 0; i < c.width.size(); ++i) {
        wmin = std::min(wmin, c.width[i]);
        if (c.width[i] < p.width && first_narrow == c.width.size()) first_narrow = i;
        CHECK((c.width[i] >= p.width) == (c.entry[i] == 0));
        CHECK(c.entry[i] < c.table.size());
    }
    CHECK(near(wmin, 100.0f, 1e-3f));
    // The channel starts at world x = 390 (sample 13); the window (K = 4) and the back-fill narrow earlier.
    CHECK(first_narrow < 13);
    // Centred: no shift inside the channel.
    CHECK(near(c.shift[20], 0.0f, 1e-3f));
    // Off-centre path (y = 240): L = 70, R = 30 inside; h = 50 -> shift = h - R = +20 (toward the roomier left).
    const std::vector<V2> low = {{0.0f, 240.0f}, {1100.0f, 240.0f}};
    const CorridorSqueeze c2 = squeeze_corridor(g, low, p);
    CHECK(near(c2.left[20], 70.0f, 1e-3f) && near(c2.right[20], 30.0f, 1e-3f) && near(c2.shift[20], 20.0f, 1e-3f));
    // Fixed width (type 2) and no-squeeze types.
    p.type = kCorridorFixed;
    const CorridorSqueeze c3 = squeeze_corridor(g, centred, p);
    CHECK(c3.table.size() == 1 && std::all_of(c3.width.begin(), c3.width.end(), [](float w) { return w == 200.0f; }));
    p.type = 0;
    CHECK(squeeze_corridor(g, centred, p).samples.empty());
    // Flag +0x9c: every sample (h, h).
    p.type = kCorridorProbe;
    p.fixed_flag = true;
    CHECK(squeeze_corridor(g, centred, p).table.size() == 1);
    p.fixed_flag = false;
    // A path turning 90 degrees next to the channel: the vertex fan sees the wall; deterministic.
    const std::vector<V2> bend = {{300.0f, 100.0f}, {300.0f, 260.0f}, {1000.0f, 260.0f}};
    const CorridorSqueeze b1 = squeeze_corridor(g, bend, p), b2 = squeeze_corridor(g, bend, p);
    CHECK(b1.width == b2.width && b1.shift == b2.shift && b1.table == b2.table);
    bool bounded = true;
    for (size_t i = 0; i < b1.left.size(); ++i) bounded = bounded && b1.left[i] <= 100.0f && b1.right[i] <= 100.0f;
    CHECK(bounded);
    std::printf("corridor: %zu samples, min width %.1f, %zu width entries\n", c.samples.size(), static_cast<double>(wmin),
                c.table.size());
}

// ---------------------------------------------------------------- §4.2 threat mode
static bool compass_aligned(V2 d) {
    const float ax = std::fabs(d.x), ay = std::fabs(d.y);
    return ax < 1e-2f || ay < 1e-2f || std::fabs(ax - ay) < 1e-2f * std::max(ax, ay);
}

static void test_threat() {
    // Cost rule.
    const ThreatFn tf = [](V2 p) { return length(p) < 600.0f ? 100.0f : 0.0f; };
    const ThreatCost tc = ThreatCost::make(&tf, 10.0f);
    CHECK(near(tc.weight, 0.1f, 1e-7f) && tc.factor({0.0f, 0.0f}) == 11.0f && tc.factor({1000.0f, 0.0f}) == 1.0f);
    const ThreatFn t20 = [](V2) { return 20.0f; };
    CHECK(ThreatCost::make(&t20, 10.0f).factor({}) == 3.0f && ThreatCost::make(&t20, 20.0f).factor({}) == 2.0f &&
          ThreatCost::make(&t20, 30.0f).factor({}) == 1.0f && !ThreatCost{}.active());
    CHECK(ThreatCost::make(&t20, 0.5f).weight == 1.0f);  // weight = 1 / max(T, 1)

    SpaceOccupancy occ;
    occ.set_box({-3000.0f, -3000.0f}, {3000.0f, 3000.0f});
    MovementConstants k;
    SpaceRequest q;
    q.self = 1;
    q.start = {-1500.0f, 0.0f};
    q.goal = {1500.0f, 0.0f};
    q.speed = 5.0f;
    q.turn_rate = 1.5f;
    q.radius = 20.0f;
    SpaceScratch sc;
    SpacePath plain;
    CHECK(plan_space_path(occ, k, q, sc, plain) && plain.ok);
    // Threat mode with a threat blob on the straight line: compass steps only, around the blob.
    q.threat_threshold = 10.0f;
    q.threat = &tf;
    SpacePath tp;
    CHECK(plan_space_path(occ, k, q, sc, tp) && tp.ok && !tp.fallback && tp.wps.back().pos == q.goal);
    bool aligned = true, clear = true;
    for (size_t i = 1; i + 1 < tp.wps.size(); ++i) {
        aligned = aligned && compass_aligned(tp.wps[i].pos - tp.wps[i - 1].pos);
        clear = clear && length(tp.wps[i].pos) >= 600.0f;
    }
    CHECK(aligned && clear);
    std::printf("threat: %zu waypoints, tries %u, pushes %u\n", tp.wps.size(), tp.tries, tp.pushes);
    SpacePath tp2;
    CHECK(plan_space_path(occ, k, q, sc, tp2) && tp2.wps.size() == tp.wps.size());
    bool same = true;
    for (size_t i = 0; i < tp.wps.size() && i < tp2.wps.size(); ++i)
        same = same && tp.wps[i].pos == tp2.wps[i].pos && tp.wps[i].time == tp2.wps[i].time;
    CHECK(same);
    // No threat grid: still compass moves (cost x 1), straight along x.
    q.threat = nullptr;
    SpacePath tn;
    CHECK(plan_space_path(occ, k, q, sc, tn) && tn.ok);
    bool on_axis = true;
    for (const SpaceWaypoint& w : tn.wps) on_axis = on_axis && near(w.pos.y, 0.0f, 1e-3f);
    CHECK(on_axis);
    // Space_Layer set (corvette and up): threat mode is not honoured -> the plain plan.
    q.threat = &tf;
    q.space_layer_type = 4;
    SpacePath tl;
    CHECK(plan_space_path(occ, k, q, sc, tl) && tl.wps.size() == plain.wps.size());
    bool equal = true;
    for (size_t i = 0; i < tl.wps.size() && i < plain.wps.size(); ++i) equal = equal && tl.wps[i].pos == plain.wps[i].pos;
    CHECK(equal);

    // Land region search: a block splits the map into a north and a south corridor; threat on one side moves the path
    // to the other.
    PassabilityGrid g = open_grid(60, 60);
    for (int32_t y = 15; y < 45; ++y)
        for (int32_t x = 20; x < 40; ++x) g.set_base(x, y, kPassImpassable);
    RegionMap rm;
    const uint32_t masks[] = {kPassClear};
    rm.build(g, masks);
    LandPathRequest lq;
    lq.start = cell_center(5, 30);
    lq.goal = cell_center(55, 30);
    lq.mask = kPassClear;
    LandPathScratch ls;
    LandPath base;
    CHECK(find_land_path(g, rm, lq, ls, base) && base.ok);
    float ysum = 0.0f;
    for (const V2& pt : base.points) ysum += pt.y - 600.0f;
    const bool north = ysum > 0.0f;  // side taken without threat
    const ThreatFn side = [north](V2 p) { return (north ? p.y > 600.0f : p.y < 600.0f) && p.x > 300.0f && p.x < 900.0f ? 50.0f : 0.0f; };
    lq.threat = ThreatCost::make(&side, 10.0f);
    LandPath avoid;
    CHECK(find_land_path(g, rm, lq, ls, avoid) && avoid.ok);
    bool other_side = true;
    for (const V2& pt : avoid.points)
        if (pt.x > 300.0f && pt.x < 900.0f) other_side = other_side && (north ? pt.y < 600.0f : pt.y > 600.0f);
    CHECK(other_side);
    bool valid = true;
    for (size_t i = 1; i < avoid.points.size(); ++i) valid = valid && segment_passable(g, avoid.points[i - 1], avoid.points[i], kPassClear);
    CHECK(valid);
}

// ---------------------------------------------------------------- §4.3-4.7 pure rules
static void test_rules() {
    CHECK(locomotor_kind_from_token("jetpack_locomotor") == LocomotorKind::Jetpack &&
          locomotor_kind_from_token("FLYING_LOCOMOTOR") == LocomotorKind::Flying &&
          locomotor_kind_from_token("Bike_Locomotor") == LocomotorKind::Bike &&
          locomotor_kind_from_token("LANDBOMBER_LOCOMOTOR") == LocomotorKind::LandBomber &&
          locomotor_kind_from_token("FIGHTER_LOCOMOTOR") == LocomotorKind::Fighter &&
          locomotor_kind_from_token("WALK_LOCOMOTOR") == LocomotorKind::Default);
    // Jetpack turn anims (§4.3 table).
    CHECK(jetpack_turn_anim(2.0f, 5.0f, true).anim == kAnimNone);
    TurnAnim a = jetpack_turn_anim(11.25f, 4.0f, true);
    CHECK(a.anim == kAnimTurnLeftQuarter && near(a.rate, 1.0f, 1e-6f));
    a = jetpack_turn_anim(-30.0f, 4.0f, true);
    CHECK(a.anim == kAnimTurnRightHalf && a.rate == 2.0f);
    a = jetpack_turn_anim(90.0f, 4.0f, true);
    CHECK(a.anim == kAnimTurnLeft && a.rate == 4.0f);
    a = jetpack_turn_anim(-90.0f, 4.0f, false);
    CHECK(a.anim == kAnimTurnRight && near(a.rate, 22.5f * 0.03333f, 1e-5f));
    // Altitude: flat terrain 0, hover 30 -> target 30; a ridge 40 frames ahead raises it with the fade.
    const TerrainFn flat = [](V2) { return 0.0f; };
    const TrackFn still = [](uint32_t) { return V2{}; };
    CHECK(jetpack_altitude_target(still, flat, {}, 30.0f, -1e30f, 0.0f) == 30.0f);
    const TerrainFn ridge = [](V2 p) { return p.x >= 400.0f ? 100.0f : 0.0f; };
    const TrackFn ahead = [](uint32_t i) { return V2{10.0f * static_cast<float>(i), 0.0f}; };
    const float t = jetpack_altitude_target(ahead, ridge, {}, 30.0f, -1e30f, 0.0f);
    CHECK(near(t, 100.0f * (1.0f - 40.0f / 60.0f) + 30.0f, 1e-3f));
    CHECK(step_altitude(0.0f, 30.0f, 0.8f, 0.0f, -1e30f) == 0.8f && step_altitude(5.0f, -10.0f, 0.8f, 4.5f, -1e30f) == 4.5f);
    // Bike lean.
    CHECK(bike_lean(0.0f, kAnimTurnLeft, 2.0f, 70.0f) == -2.0f && bike_lean(69.0f, kAnimTurnRight, 2.0f, 70.0f) == 70.0f &&
          bike_lean(1.0f, kAnimNone, 2.0f, 70.0f) == 0.0f && bike_lean(-5.0f, kAnimNone, 2.0f, 70.0f) == -3.0f);
    // Fighter fold (>= 90, FIGHTER_FLYING §2 step 4): fighter.hpp, tested in test_movement.cpp.
    const FAttitude f = fighter_fold_pitch360({0.0f, 100.0f, 10.0f}, 0.8f);
    CHECK(near(f.yaw, 190.0f, 1e-4f) && near(f.pitch, 79.2f, 1e-4f) && near(f.roll, 180.0f, 1e-4f));
    CHECK(fighter_fold_pitch360({0.0f, 45.0f, 10.0f}, 0.8f).pitch == 45.0f);
}

// ---------------------------------------------------------------- FIGHTER_FLYING §6 pure rules (flying)
static void test_flying_rules() {
    FlyRates r;
    r.turn = 5.0f;
    r.roll = 2.0f;
    r.lift = 100.0f;
    r.bank = 20.0f;
    r.max_speed = 5.0f;
    r.hover_offset = 50.0f;
    r.prepare_strafe_height = 150.0f;

    // §6.1 strict bracketing vs the space playback (advance while t <= T): at T = a waypoint time the follower stays
    // on the earlier segment at u = 1; between waypoints both trace the same Hermite curve.
    const std::vector<SpaceWaypoint> wps = {{{0.0f, 0.0f}, 0.0f, 3.0f, 0.0f},
                                            {{30.0f, 10.0f}, 30.0f, 3.0f, 10.0f},
                                            {{50.0f, 40.0f}, 70.0f, 3.0f, 20.0f}};
    FlyBody b;
    b.pos = {0.0f, 0.0f, 77.0f};
    b.o = {0.0f, 4.0f, 0.0f};
    b.speed = 3.0f;
    uint32_t idx = 0;
    FlyFollow fo = flying_follow(wps, idx, 10.0f, b, r);
    const SpacePose sp10 = eval_space_path(wps, 10.0f);
    CHECK(idx == 0 && !fo.complete && sp10.pos == wps[1].pos);  // space: segment 1 at s = 0
    CHECK(near(fo.pos.x, 30.0f, 1e-3f) && near(fo.pos.y, 10.0f, 1e-3f) && fo.pos.z == 77.0f && fo.o.pitch == 4.0f);
    fo = flying_follow(wps, idx, 10.5f, b, r);
    CHECK(idx == 1);
    uint32_t idx5 = 0;
    const FlyFollow f5 = flying_follow(wps, idx5, 5.0f, b, r);
    const SpacePose sp5 = eval_space_path(wps, 5.0f);
    CHECK(near(f5.pos.x, sp5.pos.x, 1e-3f) && near(f5.pos.y, sp5.pos.y, 1e-3f) && near(f5.speed, sp5.speed, 1e-4f));
    CHECK(near(fly_wrap180(f5.o.yaw - sp5.heading), 0.0f, 1e-3f) && f5.o.yaw >= 0.0f);  // yaw from angles: [0, 360)
    fo = flying_follow(wps, idx, 20.0f, b, r);  // the last time: still on the path (space: done)
    CHECK(idx == 1 && !fo.complete && eval_space_path(wps, 20.0f).done);
    fo = flying_follow(wps, idx, 20.5f, b, r);  // past it: extrapolate along O at s0, z moves with the pitch
    CHECK(fo.complete && near(fo.pos.x, 3.0f * std::cos(4.0f * kDegToRad), 1e-4f) && fo.pos.z < 77.0f && fo.speed == 3.0f);

    // §6.1 bank from the yaw change: dy = +10 (g = 1) rolls by -roll(T); a held heading decays the roll; +-bank clamp.
    const std::vector<SpaceWaypoint> line = {{{0.0f, 0.0f}, 0.0f, 2.0f, 0.0f}, {{20.0f, 0.0f}, 0.0f, 2.0f, 10.0f}};
    b.o = {0.0f, 0.0f, 350.0f};
    idx = 0;
    fo = flying_follow(line, idx, 5.0f, b, r);
    CHECK(fo.o.yaw == 0.0f && fo.o.roll == -2.0f);
    b.o = {3.0f, 0.0f, 0.0f};
    CHECK(flying_follow(line, idx, 5.0f, b, r).o.roll == 1.0f);
    b.o = {-19.0f, 0.0f, 350.0f};
    CHECK(flying_follow(line, idx, 5.0f, b, r).o.roll == -20.0f);
    b.o = {0.0f, 0.0f, 357.5f};  // dy = 2.5: g = 0.5
    CHECK(flying_follow(line, idx, 5.0f, b, r).o.roll == -1.0f);

    // §6.2 attitude: roll target -5 x dy, yaw budget, the gate.
    float by = 3.0f, bp = 1.0f;
    FlyOrient o = flying_attitude_step({0.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 10.0f}, by, bp, false, 2.0f, 20.0f);
    CHECK(o.yaw == 3.0f && by == 0.0f && o.roll == -2.0f && o.pitch == 1.0f && bp == 0.0f);
    by = 3.0f;
    bp = 1.0f;
    o = flying_attitude_step({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f}, by, bp, true, 2.0f, 20.0f);
    CHECK(o.yaw == 0.0f && by == 3.0f && o.roll == -2.0f);  // gate: R = 0 is not on the left-turn side
    o = flying_attitude_step({-5.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f}, by, bp, true, 2.0f, 20.0f);
    CHECK(o.yaw == 3.0f && o.roll == -7.0f);  // banked left: the yaw turns
    by = 3.0f;
    o = flying_attitude_step({5.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -10.0f}, by, bp, true, 2.0f, 20.0f);
    CHECK(o.yaw == -3.0f && o.roll == 7.0f);  // a right turn needs R >= 0
    by = 3.0f;
    o = flying_attitude_step({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.25f}, by, bp, false, 2.0f, 20.0f);
    CHECK(o.roll == -1.25f && o.yaw == 0.25f && by == 2.75f);  // |e| < roll rate: exactly -5 x dy
    by = 3.0f;
    o = flying_attitude_step({-19.5f, 0.0f, 179.0f}, {0.0f, 0.0f, 270.0f}, by, bp, false, 2.0f, 20.0f);
    CHECK(o.yaw == -178.0f && o.roll == -20.0f);  // (-180, 180] wrap and the bank clamp

    // §6.3 altitude: climbs are limited to 30 deg; a dive to the destination's depression angle; lift-limited steps.
    FlyWorld w;
    w.terrain = [](V2) { return 0.0f; };
    FlyRates hr = r;
    hr.hover_offset = 200.0f;
    FlyVec3 P{0.0f, 0.0f, 0.0f};
    FlyOrient O{0.0f, 0.0f, 0.0f};
    flying_altitude_step(P, O, 4.0f, false, nullptr, hr, w);
    CHECK(O.pitch == -30.0f && near(P.z, 4.0f * std::tan(30.0f * kDegToRad), 1e-4f));
    P = {0.0f, 0.0f, 1000.0f};
    O = {};
    flying_altitude_step(P, O, 4.0f, false, nullptr, hr, w);
    CHECK(O.pitch == 30.0f && P.z < 1000.0f);
    P = {0.0f, 0.0f, 1000.0f};
    O = {};
    const FlyVec3 far_dest{10000.0f, 0.0f, 0.0f};
    flying_altitude_step(P, O, 4.0f, false, &far_dest, hr, w);
    CHECK(near(O.pitch, std::atan(0.1f) / kDegToRad, 1e-3f));  // min(depression angle, 30)
    FlyRates slow = hr;
    slow.lift = 2.0f;
    P = {0.0f, 0.0f, 0.0f};
    O = {};
    flying_altitude_step(P, O, 4.0f, false, nullptr, slow, w);
    CHECK(O.pitch == -2.0f);
    // Look-ahead with the fade: samples at P + (i + 1) s; from the ridge (x >= 400, i >= 39) on, each sample raises M
    // to (150 - M) (1 - i/60) + M (the fade is relative to the running M, starting from 50 = flat ground + hover).
    w.terrain = [](V2 p) { return p.x >= 400.0f ? 100.0f : 0.0f; };
    FlyRates ar = r;
    ar.lift = 1000.0f;
    float minh = 0.0f;
    w.clearance = [&minh](FlyVec3, FlyVec3 to, float m) {
        minh = m;
        CHECK(near(to.x, 600.0f, 1e-3f));  // the last sample
        return m;
    };
    P = {0.0f, 0.0f, 50.0f};
    O = {};
    flying_altitude_step(P, O, 10.0f, false, nullptr, ar, w);
    float want_m = 50.0f;
    for (uint32_t i = 39; i < 60; ++i)
        want_m = std::max(want_m, (150.0f - want_m) * (1.0f - static_cast<float>(i) / 60.0f) + want_m);
    CHECK(near(minh, want_m, 1e-3f) && minh > 100.0f);
    // The tracker replaces the extrapolation; a repeated point switches back to it.
    w.clearance = {};
    uint32_t tracked = 0;
    w.track = [&tracked](uint32_t i, FlyVec3& p, V2& f) {
        ++tracked;
        p = {i < 3 ? 1000.0f + static_cast<float>(i) : 1002.0f, 0.0f, 50.0f};
        f = {-1.0f, 0.0f};
        return true;
    };
    P = {0.0f, 0.0f, 50.0f};
    O = {};
    flying_altitude_step(P, O, 10.0f, true, nullptr, ar, w);
    CHECK(tracked == 4 && O.pitch < 0.0f);  // samples 0..2 on the ridge, sample 3 repeats -> extrapolate (no query)
    w.track = {};

    // §6.6 loiter re-issue (§9.2): D + dir x L; one draw only when >= 0.3 L from D; else turn back without a draw.
    SimRng rng(99);
    bool back = false;
    const V2 dst{100.0f, 0.0f};
    V2 q = flying_loiter_reissue({0.0f, 0.0f}, dst, 0.0f, 66.0f, rng, back);
    CHECK(rng.draws() == 1 && !back && near(distance(q, dst), 66.0f, 1e-3f) && q.x >= 100.0f - 1e-3f);
    q = flying_loiter_reissue({90.0f, 0.0f}, dst, 90.0f, 66.0f, rng, back);
    CHECK(rng.draws() == 1 && back && near(q.x, 100.0f, 1e-4f) && near(q.y, -66.0f, 1e-4f));
    // Strafe offset only within radius + 0.3 x Strafe_Distance of the target.
    back = false;
    q = flying_strafe_aim({0.0f, 0.0f}, {1000.0f, 0.0f}, 0.0f, 10.0f, 500.0f, back);
    CHECK(!back && q.x == 1000.0f && q.y == 0.0f);
    q = flying_strafe_aim({900.0f, 0.0f}, {1000.0f, 0.0f}, 0.0f, 10.0f, 500.0f, back);
    CHECK(back && q.x == 1500.0f && q.y == 0.0f);

    // §6.5 circling probes at P + k V, k = 1..11; H max'd with the target z; no second clamp on the descent.
    FlyWorld cw;
    float spike_x = 110.0f;
    cw.terrain = [&spike_x](V2 p) { return p.x == spike_x ? 500.0f : 0.0f; };
    const FlyVec3 cp{0.0f, 0.0f, 0.0f}, cv{10.0f, 0.0f, 0.0f};
    CHECK(flying_circle_height(cw, cp, cv, 20.0f, 50.0f) == 550.0f);  // k = 11
    spike_x = 10.0f;
    CHECK(flying_circle_height(cw, cp, cv, 20.0f, 50.0f) == 550.0f);  // k = 1
    spike_x = 120.0f;
    CHECK(flying_circle_height(cw, cp, cv, 20.0f, 50.0f) == 70.0f);   // k = 12 is not probed: target z 20 + 50
    spike_x = 0.0f;
    CHECK(flying_circle_height(cw, cp, cv, -5.0f, 50.0f) == 50.0f);   // P itself is not probed
    CHECK(flying_circle_dz(300.0f, 200.0f, 5.0f, 150.0f, 50.0f) == 5.0f);
    CHECK(flying_circle_dz(100.0f, 300.0f, 5.0f, 150.0f, 50.0f) == -10.0f);  // -5 x (200 / 100), not clamped to -5
    CHECK(flying_circle_dz(100.0f, 101.0f, 5.0f, 40.0f, 50.0f) == -1.0f);    // span max(-10, 1) = 1
    // One circling frame: in the band the flyer turns onto the tangent and moves at vmax; null target: P += V.
    FlyBody cb;
    cb.pos = {-150.0f, 0.0f, 50.0f};
    cb.o = {0.0f, 0.0f, 80.0f};  // the tangent (yaw 90) is 10 deg to the left: yaw +5, roll -2
    cb.vel = {0.0f, 5.0f, 0.0f};
    cw.terrain = [](V2) { return 0.0f; };
    const FlyVec3 tgt{0.0f, 0.0f, 0.0f};
    flying_circle_step(cb, &tgt, 100.0f, 200.0f, r, cw);
    CHECK(near(std::sqrt(cb.vel.x * cb.vel.x + cb.vel.y * cb.vel.y + cb.vel.z * cb.vel.z), 5.0f, 1e-4f));
    CHECK(near(cb.pos.x, -150.0f + cb.vel.x, 1e-4f) && cb.o.roll == -2.0f && near(cb.o.yaw, 85.0f, 1e-3f));
    const FlyVec3 before = cb.pos, v = cb.vel;
    flying_circle_step(cb, nullptr, 100.0f, 200.0f, r, cw);
    CHECK(cb.pos.x == before.x + v.x && cb.pos.y == before.y + v.y && cb.pos.z == before.z + v.z);
}

// ---------------------------------------------------------------- land system
static uint32_t anim_len(ObjectId, int32_t a) {
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

static void land_system(MovementSystem& sys, uint32_t cells) {
    PassabilityGrid g = open_grid(cells, cells);
    sys.set_grid(std::move(g));
    sys.set_height_fn([](float x, float) { return x > 1000.0f ? 40.0f : 0.0f; });
    sys.set_anim_length_fn(anim_len);
    const float hi = static_cast<float>(cells - 1) * kCellSize;
    sys.air().set_box({0.0f, 0.0f}, {hi, hi});
}

static void test_jetpack() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = unit(1, cell_center(10, 10), 0, 2.0f, 6.0f, LocomotorKind::Jetpack);
    d.yaw_deg = 180.0f;  // facing away: turn first
    d.hover_offset = 30.0f;
    d.has_pre_turn_anim = true;
    CHECK(sys.add_unit(d));
    SimRng rng(5);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(70, 40);
    sys.move(o, rng, 0);
    sys.coordinator_update(0, 0, rng);  // cu8j t01: the composition draws at the next coordinator update
    const SimRng before = rng;
    std::vector<uint8_t> states;
    std::vector<int32_t> anims;
    float max_z = 0.0f;
    bool airborne_moving = true;
    uint32_t f = 0;
    for (; f < 3000; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        if (states.empty() || states.back() != static_cast<uint8_t>(v.loco_state)) states.push_back(static_cast<uint8_t>(v.loco_state));
        if (anims.empty() || anims.back() != v.anim) anims.push_back(v.anim);
        max_z = std::max(max_z, v.z);
        if (v.loco_state == LocoState::WalkMoving && !v.airborne && v.anim != kAnimFlyLand) airborne_moving = false;
        if (v.loco_state == LocoState::WalkStopped && v.arrived) break;
    }
    const UnitView v = sys.view(1);
    const std::vector<uint8_t> want = {0x00, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16};
    CHECK(std::search(states.begin(), states.end(), want.begin() + 1, want.end()) != states.end() || states == want);
    auto seen = [&](int32_t a) { return std::find(anims.begin(), anims.end(), a) != anims.end(); };
    CHECK(seen(kAnimJump) && seen(kAnimFlyIdle) && seen(kAnimFlyLand) && seen(kAnimFlyLandIdle) && seen(kAnimFlyLandDrop) &&
          seen(kAnimTurnLeftEnd) == !seen(kAnimTurnRightEnd));
    CHECK(airborne_moving && v.arrived && !v.airborne && v.z == 40.0f && distance(v.pos, o.destination) < 1.0f);
    CHECK(max_z >= 70.0f - 1e-3f);  // terrain 40 + Hover_Offset 30 over the plateau
    CHECK(rng == before);           // no synced draws
    std::printf("jetpack: %u frames, %zu states, max z %.2f\n", f, states.size(), static_cast<double>(max_z));

    // Landing blocked by a stopped walker on the destination: the move is re-issued to a free spot within ~50.
    MovementSystem s2{MovementConfig{}};
    land_system(s2, 100);
    CHECK(s2.add_unit(d));
    CHECK(s2.add_unit(unit(2, cell_center(70, 40), 0, 1.0f, 10.0f, LocomotorKind::Default)));
    s2.move(o, rng, 0);
    for (f = 0; f < 3000; ++f) {
        s2.tick(f, rng);
        const UnitView w = s2.view(1);
        if (w.loco_state == LocoState::WalkStopped && w.arrived) break;
    }
    const UnitView w = s2.view(1), blocker = s2.view(2);
    CHECK(w.arrived && distance(w.pos, blocker.pos) >= 16.0f && distance(w.pos, o.destination) <= 60.0f);
}

static void test_flying() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 120);
    UnitDesc d = unit(1, cell_center(20, 20), 6, 3.0f, 10.0f, LocomotorKind::Flying);
    d.hover_offset = 50.0f;
    CHECK(sys.add_unit(d));
    SimRng rng(77);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(60, 60);
    sys.move(o, rng, 0);
    sys.coordinator_update(0, 0, rng);  // cu8j t01: the composition draws at the next coordinator update
    uint32_t arrive = 0, reissues = 0, last_reissue = 0, min_gap = 100000;
    LocoState prev = LocoState::None;
    const uint64_t draws0 = rng.draws();
    float max_roll = 0.0f, max_dist = 0.0f, max_pitch = 0.0f;
    bool att_ok = true;
    for (uint32_t f = 0; f < 1500; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        max_roll = std::max(max_roll, std::fabs(v.roll));
        float ar = 0.0f, ap = 0.0f;  // S7 t05: the render attitude = the flyer's roll / pitch
        att_ok = att_ok && sys.attitude(1, ar, ap) && ar == v.roll && ap == v.pitch && std::fabs(ap) <= 30.0f;
        max_pitch = std::max(max_pitch, std::fabs(ap));
        if (arrive) max_dist = std::max(max_dist, distance(v.pos, o.destination));
        if (v.arrived && !arrive) arrive = f;
        if (arrive && prev == LocoState::FlyNoPath && v.loco_state == LocoState::WalkMoving) {
            if (reissues) min_gap = std::min(min_gap, f - last_reissue);
            ++reissues;
            last_reissue = f;
        }
        prev = v.loco_state;
        CHECK(v.airborne && v.z >= 0.0f && std::fabs(v.roll) <= 70.0f);  // bank clamp (Bank_Turn_Angle default 70)
    }
    // At most one synced draw per loiter re-issue (none for a turn-back); re-issues more than 61 frames apart.
    const uint64_t draws = rng.draws() - draws0;
    CHECK(arrive > 0 && reissues >= 3 && draws >= 1 && draws <= reissues && min_gap > kLoiterIdleFrames);
    // The loiter stays around the destination: every re-issue targets D + dir x L (L = 66); between re-issues it coasts
    // straight for up to 62 frames (~290 units at 4.68 / frame).
    CHECK(max_dist < 600.0f && max_roll > 0.0f);
    CHECK(att_ok && max_pitch > 0.0f);  // the climb to Hover_Offset pitches the nose (S7 t05 render attitude)
    std::printf("flying: arrived at %u, %u loiter re-issues, %llu draws (min gap %u), max distance %.1f\n", arrive,
                reissues, static_cast<unsigned long long>(draws), min_gap, static_cast<double>(max_dist));
    // Circling.
    CHECK(sys.circle(1, cell_center(60, 60), 20.0f, 100.0f, 200.0f));
    float dmin = 1e9f, dmax = 0.0f;
    const uint64_t draws1 = rng.draws();
    for (uint32_t f = 1500; f < 2500; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        CHECK(v.loco_state == LocoState::FlyCircling);
        if (f > 1800) {
            const float dd = distance(v.pos, cell_center(60, 60));
            dmin = std::min(dmin, dd);
            dmax = std::max(dmax, dd);
        }
    }
    CHECK(rng.draws() == draws1 && dmin > 60.0f && dmax < 260.0f);
    std::printf("flying circle: distance %.1f .. %.1f\n", static_cast<double>(dmin), static_cast<double>(dmax));

    // Start_Circling (cable attack): around a target OBJECT. A walker of the system is followed as it moves; a
    // non-mover comes from the host hook; an unknown target coasts on V. A target and both radii > 0 are required.
    MovementSystem s2{MovementConfig{}};
    land_system(s2, 120);
    CHECK(s2.add_unit(d));
    CHECK(s2.add_unit(unit(2, cell_center(60, 60), 0, 1.0f, 8.0f, LocomotorKind::Default)));
    CHECK(!s2.start_circling(1, 2, 0.0f, 100.0f) && !s2.start_circling(2, 1, 10.0f, 100.0f) &&
          !s2.start_circling(1, -1, 10.0f, 100.0f));
    CHECK(s2.start_circling(1, 2, 40.0f, 120.0f) && s2.view(1).loco_state == LocoState::FlyCircling);
    MoveOrder walk;
    walk.units = {2};
    walk.destination = cell_center(80, 60);
    s2.move(walk, rng, 0);
    s2.coordinator_update(0, 0, rng);  // cu8j t01: the composition draws at the next coordinator update
    const uint64_t draws2 = rng.draws();
    float near_end = 1e9f;
    for (uint32_t f = 0; f < 1500; ++f) {
        s2.tick(f, rng);
        CHECK(s2.view(1).loco_state == LocoState::FlyCircling);
        if (f > 1300) near_end = std::min(near_end, distance(s2.view(1).pos, s2.view(2).pos));
    }
    CHECK(rng.draws() == draws2 && s2.view(2).arrived && near_end < 160.0f);
    FlyingHooks hooks;  // a fixed structure (id 77) at (400, 1800, 30)
    hooks.object_position = [](ObjectId oid, FlyVec3& p) {
        if (oid != 77) return false;
        p = {400.0f, 1800.0f, 30.0f};
        return true;
    };
    s2.set_flying_hooks(hooks);
    CHECK(s2.start_circling(1, 77, 40.0f, 120.0f));
    float far_end = 1e9f;
    for (uint32_t f = 1500; f < 3000; ++f) {
        s2.tick(f, rng);
        if (f > 2800) far_end = std::min(far_end, distance(s2.view(1).pos, {400.0f, 1800.0f}));
    }
    CHECK(far_end < 160.0f);
    CHECK(s2.start_circling(1, 999, 40.0f, 120.0f));  // unknown target: P += V
    const V2 p0 = s2.view(1).pos;
    s2.tick(3000, rng);
    const V2 p1 = s2.view(1).pos;
    s2.tick(3001, rng);
    const V2 p2 = s2.view(1).pos;
    CHECK(near(p2.x - p1.x, p1.x - p0.x, 1e-3f) && near(p2.y - p1.y, p1.y - p0.y, 1e-3f) && !(p1 == p0));
    std::printf("flying start_circling: closest to the walker %.1f, to the structure %.1f\n",
                static_cast<double>(near_end), static_cast<double>(far_end));
}

// FIGHTER_FLYING §6.3.1: the clearance query (0x63b640): layer rules, hit test, MovementSystem::clearance_query, and
// a flyer climbing over an airborne jetpack unit through FlyingHooks::clearance.
static void test_flying_clearance() {
    // Kind 2 (airborne) and the tracking layer 0x397900.
    CHECK(locomotor_airborne_kind(LocomotorKind::Flying, LocoState::WalkStopped) &&
          locomotor_airborne_kind(LocomotorKind::LandBomber, LocoState::None));
    CHECK(locomotor_airborne_kind(LocomotorKind::Jetpack, LocoState::None) &&
          locomotor_airborne_kind(LocomotorKind::Jetpack, LocoState::WalkStartTurn) &&
          locomotor_airborne_kind(LocomotorKind::Jetpack, LocoState::WalkStartMove) &&
          locomotor_airborne_kind(LocomotorKind::Jetpack, LocoState::WalkMoving));
    CHECK(!locomotor_airborne_kind(LocomotorKind::Jetpack, LocoState::WalkEndMove) &&
          !locomotor_airborne_kind(LocomotorKind::Jetpack, LocoState::WalkStopped) &&
          !locomotor_airborne_kind(LocomotorKind::Default, LocoState::None) &&
          !locomotor_airborne_kind(LocomotorKind::Bike, LocoState::BikeMoving));
    CHECK(object_tracking_layer(true, true, false, 0x10, 0) == kLayerCorvette);
    CHECK(object_tracking_layer(false, true, true, 0x10, 0) == 0x10);                      // needs the flag
    CHECK(object_tracking_layer(true, false, true, kLayerCorvette, 0x10) == kLayerCorvette);  // moving: Space_Layer
    CHECK(object_tracking_layer(true, false, false, kLayerCorvette, 0x10) == 0x10);        // stationary layer
    CHECK(object_tracking_layer(false, false, false, kLayerCorvette, 0) == kLayerCorvette);  // none set: Space_Layer
    // Half-extents 0x396850 and the per-hit value.
    V2 h = clearance_half_extents(2.0f, 0.0f, 1.5f, true, -5.0f, 3.0f);
    CHECK(h.x == 3.0f && h.y == 4.5f);
    h = clearance_half_extents(0.0f, 0.0f, 2.0f, true, -4.0f, -6.0f);
    CHECK(h.x == 8.0f && h.y == 12.0f);
    h = clearance_half_extents(0.0f, 0.0f, 1.2f, false, 9.0f, 9.0f);
    CHECK(h.x == 0.0f && h.y == 0.0f);
    // s78n t10 (TACTICAL_COMBAT §5.4.6 item 1, 0x371be0): one custom and no model -> (0, 0); both customs need no model.
    h = clearance_half_extents(2.0f, 0.0f, 1.5f, false, 9.0f, 9.0f);
    CHECK(h.x == 0.0f && h.y == 0.0f);
    h = clearance_half_extents(2.0f, 3.0f, 1.5f, false, 9.0f, 9.0f);
    CHECK(h.x == 3.0f && h.y == 4.5f);
    CHECK(clearance_top(0.0f, 40.0f, 2.0f, 25.0f) == 105.0f && clearance_top(-10.0f, 10.0f, 3.0f, 7.0f) == 7.0f);
    CHECK(clearance_top(4.0f, 36.0f, 2.0f, 25.0f) == 105.0f);  // s78n t10: (centre z 4 + half z 36) x 2 + 25
    // Hit test, isotropic radii (R = 3 + 5 = 8): the segment (0, 0) -> (100, 0) touches entries within 8 of it.
    ClearanceEntry e;
    e.ex = e.ey = 3.0f;
    e.facing = {1.0f, 0.0f};
    auto hit_at = [&e](V2 p) {
        e.s = e.e = p;
        return clearance_hit({0.0f, 0.0f}, {100.0f, 0.0f}, 5.0f, 5.0f, e, 0.0f);
    };
    CHECK(hit_at({50.0f, 7.9f}) && !hit_at({50.0f, 8.1f}) && hit_at({50.0f, -7.9f}));
    CHECK(hit_at({-7.9f, 0.0f}) && !hit_at({-8.1f, 0.0f}) && hit_at({107.9f, 0.0f}) && !hit_at({108.1f, 0.0f}));
    // Candidate AABBs overlap strictly: entry box [y - 3, y + 3] vs query [-5, 5].
    e.s = e.e = {50.0f, 7.9f};
    CHECK(clearance_candidate({0.0f, 0.0f}, {100.0f, 0.0f}, 5.0f, 5.0f, e));
    e.s = e.e = {50.0f, 8.0f};
    CHECK(!clearance_candidate({0.0f, 0.0f}, {100.0f, 0.0f}, 5.0f, 5.0f, e));
    // Oriented entry (ex 20 along its facing +x, ey 2 across), zero-length query: R = ex toward +x, ey toward +y.
    e.ex = 20.0f;
    e.ey = 2.0f;
    e.s = e.e = {15.0f, 0.0f};
    CHECK(clearance_hit({0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f, 0.0f, e, 0.0f));
    e.s = e.e = {0.0f, 15.0f};
    CHECK(!clearance_hit({0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f, 0.0f, e, 0.0f));
    // A moving entry: A = s + (e - s) a at t within [t0, t1].
    e.ex = e.ey = 3.0f;
    e.s = {50.0f, 100.0f};
    e.e = {50.0f, -100.0f};
    e.t0 = 0.0f;
    e.t1 = 20.0f;
    CHECK(clearance_hit({0.0f, 0.0f}, {100.0f, 0.0f}, 5.0f, 5.0f, e, 10.0f) &&
          !clearance_hit({0.0f, 0.0f}, {100.0f, 0.0f}, 5.0f, 5.0f, e, 0.0f));

    // MovementSystem::clearance_query: a flyer (1), an airborne jetpack (2, Uses_Multiple_Locomotors), a walker with
    // Space_Layer Corvette (3, moving / stationary rules), a walker in Land (4).
    struct Info {
        bool multi = false;
        uint32_t stationary = 0;
        bool box = true;
    };
    std::map<ObjectId, Info> infos;
    auto info_fn = [&infos](ObjectId id, ClearanceInfo& ci) {
        const auto it = infos.find(id);
        if (it == infos.end()) return false;
        ci.uses_multiple_locomotors = it->second.multi;
        ci.stationary_space_layer = it->second.stationary;
        ci.half = {10.0f, 10.0f};
        ci.has_box = it->second.box;
        ci.box_lo_z = 0.0f;
        ci.box_hi_z = 40.0f;
        ci.scale_factor = 2.0f;
        return true;
    };
    {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 120);
        UnitDesc fl = unit(1, {100.0f, 600.0f}, 6, 3.0f, 10.0f, LocomotorKind::Flying);
        fl.space_layer_type = kLayerFrigate;
        UnitDesc jp = unit(2, {500.0f, 600.0f}, 0, 0.2f, 6.0f, LocomotorKind::Jetpack);
        jp.z = 20.0f;
        jp.space_layer_type = 0x10;
        UnitDesc cv = unit(3, {500.0f, 900.0f}, 0, 1.0f, 6.0f, LocomotorKind::Default);
        cv.space_layer_type = kLayerCorvette;
        UnitDesc ld = unit(4, {500.0f, 1200.0f}, 0, 1.0f, 6.0f, LocomotorKind::Default);
        ld.space_layer_type = 0x10;
        CHECK(sys.add_unit(fl) && sys.add_unit(jp) && sys.add_unit(cv) && sys.add_unit(ld));
        infos = {{1, {}}, {2, {true, 0, true}}, {3, {false, 0x10, true}}, {4, {}}};
        auto q = [&](V2 a, V2 b, float m) { return sys.clearance_query(1, {a.x, a.y, 0.0f}, {b.x, b.y, 0.0f}, m, info_fn); };
        CHECK(q({100.0f, 600.0f}, {900.0f, 600.0f}, 10.0f) == 10.0f);  // the parked jetpack (0x16) is kind 1
        SimRng rng(4);
        MoveOrder oj;
        oj.units = {2};
        oj.destination = {900.0f, 600.0f};
        sys.move(oj, rng, 0);
        for (uint32_t f = 0; f < 91; ++f) sys.tick(f, rng);  // cu8j t01 [M]: an order at frame 0 plans at frame 61
        const UnitView jv = sys.view(2);
        CHECK(jv.loco_state == LocoState::WalkMoving && jv.airborne);
        CHECK(q({100.0f, 600.0f}, {900.0f, 600.0f}, 10.0f) == 80.0f + jv.z);  // jetpack: (0 + 40) x 2 + z
        CHECK(q({100.0f, 600.0f}, {900.0f, 600.0f}, 150.0f) == 150.0f);  // a max with M
        CHECK(q({100.0f, 650.0f}, {900.0f, 650.0f}, 10.0f) == 10.0f);    // 50 beside it: miss (R = 20)
        CHECK(q({100.0f, 1200.0f}, {900.0f, 1200.0f}, 10.0f) == 10.0f);  // the Land walker is not tracked
        // Corvette walker, stationary with a Stationary_Space_Layer: not in layer 4; with none it is (top 80 + z).
        CHECK(q({100.0f, 900.0f}, {900.0f, 900.0f}, 10.0f) == 10.0f);
        infos[3].stationary = 0;
        CHECK(q({100.0f, 900.0f}, {900.0f, 900.0f}, 10.0f) == 80.0f + sys.view(3).z);
        infos[2].box = false;  // no model: no value
        CHECK(q({100.0f, 600.0f}, {900.0f, 600.0f}, 10.0f) == 10.0f);
        infos[2] = {false, 0, true};  // no Uses_Multiple_Locomotors: the jetpack keeps its Space_Layer (Land)
        CHECK(q({100.0f, 600.0f}, {900.0f, 600.0f}, 10.0f) == 10.0f);
        infos.erase(1);  // unknown flyer: M unchanged
        CHECK(q({100.0f, 600.0f}, {900.0f, 600.0f}, 10.0f) == 10.0f);
        infos[1] = {true, 0, true};  // the flyer is not excluded: a layer-4 flyer hits itself (top 80 + z 0)
        const UnitView fv = sys.view(1);
        CHECK(q(fv.pos, fv.pos + V2{30.0f, 0.0f}, 10.0f) == std::max(10.0f, 80.0f + fv.z));
    }

    // A flyer passing over an airborne jetpack (moving slowly the same way) climbs toward its top; without the hook
    // it cruises at Hover_Offset.
    auto run = [&](bool hook, float& max_z, float& max_m, float& want_m) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 120);
        UnitDesc fl = unit(1, {100.0f, 600.0f}, 6, 3.0f, 10.0f, LocomotorKind::Flying);
        fl.hover_offset = 50.0f;
        fl.z = 50.0f;  // cruising
        fl.space_layer_type = kLayerFrigate;
        UnitDesc jp = unit(2, {500.0f, 600.0f}, 0, 0.2f, 6.0f, LocomotorKind::Jetpack);
        jp.hover_offset = 25.0f;
        jp.space_layer_type = 0x10;
        CHECK(sys.add_unit(fl) && sys.add_unit(jp));
        infos = {{1, {}}, {2, {true, 0, true}}};
        max_m = 0.0f;
        want_m = 0.0f;
        if (hook) {
            FlyingHooks hk;
            hk.clearance = [&](ObjectId id, FlyVec3 a, FlyVec3 b, float m) {
                const float r = sys.clearance_query(id, a, b, m, info_fn);
                if (r > max_m) {
                    max_m = r;
                    want_m = clearance_top(0.0f, 40.0f, 2.0f, sys.view(2).z);
                }
                return r;
            };
            sys.set_flying_hooks(hk);
        }
        SimRng rng(9);
        MoveOrder oj;
        oj.units = {2};
        oj.destination = {900.0f, 600.0f};
        sys.move(oj, rng, 0);
        MoveOrder of;
        of.units = {1};
        of.destination = {900.0f, 600.0f};
        sys.move(of, rng, 0);
        max_z = 0.0f;
        for (uint32_t f = 0; f < 200; ++f) {  // to x ~ 700 (the look-ahead stays short of the x > 1000 ridge)
            sys.tick(f, rng);
            if (sys.view(1).pos.x <= 620.0f) max_z = std::max(max_z, sys.view(1).z);
        }
        CHECK(sys.view(2).airborne && sys.view(1).pos.x > 620.0f);
    };
    float z0 = 0.0f, m0 = 0.0f, w0 = 0.0f, z1 = 0.0f, m1 = 0.0f, w1 = 0.0f;
    run(false, z0, m0, w0);
    run(true, z1, m1, w1);
    std::printf("flying clearance: max z %.2f without the hook, %.2f with it (max M %.2f, jetpack top %.2f)\n",
                static_cast<double>(z0), static_cast<double>(z1), static_cast<double>(m1), static_cast<double>(w1));
    CHECK(z0 < 60.0f && m1 == w1 && m1 > 95.0f && z1 > 90.0f && z1 <= m1 + 1.0f);
}

// S7 t06: Uses_Multiple_Locomotors WALK <-> JETPACK swap (MOVEMENT §3.1 / §6.2, 0x3a9910) and the clearance query
// acting on the jetpacking unit (FIGHTER_FLYING §6.3.1: the airborne multi-locomotor unit is in the Corvette layer).
static UnitDesc trooper(ObjectId id, V2 pos) {  // Dark_Trooper_PhaseII-like: WALK primary, JETPACK secondary
    UnitDesc d = unit(id, pos, 0, 1.0f, 6.0f, LocomotorKind::Default);
    d.uses_multiple_locomotors = true;
    d.has_secondary_locomotor = true;
    d.secondary_locomotor = LocomotorKind::Jetpack;
    d.alt_max_speed = 2.5f;
    d.alt_turn_rate = 12.0f;
    d.hover_offset = 30.0f;
    d.space_layer_type = 0x10;  // Land
    return d;
}

static void test_multi_locomotor() {
    // Type data per object for the clearance query: the trooper has Uses_Multiple_Locomotors, a 40-high box x 2.
    auto info_fn = [](ObjectId id, ClearanceInfo& ci) {
        if (id != 1 && id != 10) return false;
        ci.uses_multiple_locomotors = id == 1;
        ci.half = {10.0f, 10.0f};
        ci.has_box = true;
        ci.box_lo_z = 0.0f;
        ci.box_hi_z = 40.0f;
        ci.scale_factor = 2.0f;
        return true;
    };
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(trooper(1, cell_center(10, 10))));
    UnitDesc plain = trooper(2, cell_center(10, 20));  // no Uses_Multiple_Locomotors
    plain.uses_multiple_locomotors = false;
    UnitDesc unnamed = trooper(3, cell_center(10, 30));  // Bossk / IG-88: the flag without a Secondary_Locomotor_Name
    unnamed.has_secondary_locomotor = false;
    CHECK(sys.add_unit(plain) && sys.add_unit(unnamed));
    const float walk_speed = sys.view(1).max_speed;
    const float mult = walk_speed / 1.0f;
    CHECK(!sys.use_secondary_locomotor(2, true) && !sys.use_secondary_locomotor(3, true) && !sys.use_secondary_locomotor(99, true));
    CHECK(sys.use_secondary_locomotor(1, false) && !sys.view(1).secondary_locomotor);  // already on the primary
    auto self_clear = [&](ObjectId id) {  // the unit's own footprint (the query does not exclude the flyer)
        const UnitView v = sys.view(id);
        return sys.clearance_query(id, {v.pos.x - 30.0f, v.pos.y, 0.0f}, {v.pos.x + 30.0f, v.pos.y, 0.0f}, 0.0f, info_fn);
    };

    // A plain move walks: WALK stays active, never airborne, not in layer 4.
    SimRng rng(12);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(30, 10);
    sys.move(o, rng, 0);
    uint32_t f = 0;
    bool walked = true;
    for (; f < 2000; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        walked = walked && v.locomotor == LocomotorKind::Default && !v.secondary_locomotor && !v.airborne && v.z == 0.0f &&
                 self_clear(1) == 0.0f;
        if (v.arrived) break;
    }
    CHECK(walked && sys.view(1).arrived);

    // The switch bit: swap first (Alternate speeds), then turn / jump / fly / land, then back to WALK on the ground.
    // Units without the swap ignore the bit and walk.
    MoveOrder j;
    j.units = {1};
    j.destination = cell_center(70, 40);
    j.switch_locomotor = true;
    sys.move(j, rng, f);
    MoveOrder j2;
    j2.units = {2, 3};
    j2.destination = cell_center(20, 60);
    j2.switch_locomotor = true;
    sys.move(j2, rng, f);
    sys.coordinator_update(f, f, rng);  // cu8j: the orders' and the swap's compositions draw here, not in flight
    UnitView v = sys.view(1);
    CHECK(v.locomotor == LocomotorKind::Jetpack && v.secondary_locomotor && v.has_goal && v.loco_state == LocoState::None &&
          v.max_speed == 2.5f * mult);
    CHECK(sys.view(2).locomotor == LocomotorKind::Default && !sys.view(2).secondary_locomotor &&
          sys.view(3).locomotor == LocomotorKind::Default && !sys.view(3).secondary_locomotor);
    const uint64_t draws0 = rng.draws();
    std::vector<uint8_t> states;
    float max_z = 0.0f, max_speed = 0.0f;
    bool layer_ok = true, others_walk = true;
    uint32_t back = 0;
    for (++f; f < 5000; ++f) {
        sys.tick(f, rng);
        v = sys.view(1);
        if (v.locomotor == LocomotorKind::Jetpack &&
            (states.empty() || states.back() != static_cast<uint8_t>(v.loco_state)))
            states.push_back(static_cast<uint8_t>(v.loco_state));
        max_z = std::max(max_z, v.z);
        max_speed = std::max(max_speed, v.speed);
        // Layer 4 exactly while the jetpack is active in states 0 / 0x0F..0x13 (kind 2): the query hits the unit's box.
        const bool kind2 = v.locomotor == LocomotorKind::Jetpack && locomotor_airborne_kind(v.locomotor, v.loco_state);
        const float c = self_clear(1);
        layer_ok = layer_ok && (kind2 ? c == 80.0f + v.z : c == 0.0f);
        for (ObjectId w : {2, 3}) others_walk = others_walk && sys.view(w).locomotor == LocomotorKind::Default && !sys.view(w).airborne;
        if (v.locomotor == LocomotorKind::Default) {
            back = f;
            break;
        }
    }
    // 0x16 is not seen with the jetpack active: the landing's last frame swaps back (0 may be skipped when the plan is
    // ready at once).
    const std::vector<uint8_t> want = {0x00, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14};
    CHECK(back > 0 && (states == want || std::equal(states.begin(), states.end(), want.begin() + 1, want.end())));
    CHECK(layer_ok && others_walk && rng.draws() == draws0);  // no synced draws
    CHECK(max_z >= 70.0f - 1e-3f && max_speed > 1.5f * walk_speed);  // Hover_Offset over the plateau; Alternate_Max_Speed
    v = sys.view(1);
    CHECK(!v.secondary_locomotor && v.max_speed == walk_speed && v.arrived && !v.airborne && v.z == 40.0f &&
          distance(v.pos, j.destination) < 1.0f && self_clear(1) == 0.0f);
    std::printf("multi-locomotor: jetpack %zu states, back on WALK at frame %u, max z %.2f, max speed %.2f (walk %.2f)\n",
                states.size(), back, static_cast<double>(max_z), static_cast<double>(max_speed), static_cast<double>(walk_speed));
    // Walking again after the landing.
    o.destination = cell_center(80, 40);
    sys.move(o, rng, f);
    bool walked2 = true;
    for (++f; f < back + 400; ++f) {
        sys.tick(f, rng);
        walked2 = walked2 && sys.view(1).locomotor == LocomotorKind::Default && !sys.view(1).airborne;
    }
    CHECK(walked2 && sys.view(1).pos.x > v.pos.x + 50.0f);

    // A stop in flight lands the unit (0x14) and the swap back follows on the ground.
    j.destination = cell_center(20, 80);
    sys.move(j, rng, f);
    for (++f; sys.view(1).loco_state != LocoState::WalkMoving && f < back + 1000; ++f) sys.tick(f, rng);
    CHECK(sys.view(1).airborne && sys.view(1).secondary_locomotor);
    const ObjectId stop_ids[] = {1};
    sys.stop(stop_ids);
    CHECK(sys.view(1).loco_state == LocoState::WalkEndMove);
    const uint32_t stop_at = f;
    for (; sys.view(1).secondary_locomotor && f < stop_at + 1000; ++f) sys.tick(f, rng);
    v = sys.view(1);
    CHECK(!v.secondary_locomotor && v.locomotor == LocomotorKind::Default && !v.airborne && !v.has_goal &&
          v.z == (v.pos.x > 1000.0f ? 40.0f : 0.0f));

    // Space: no swap.
    MovementConfig sc;
    sc.mode = MoveMode::Space;
    MovementSystem space{sc};
    CHECK(space.add_unit(trooper(1, {0.0f, 0.0f})) && !space.use_secondary_locomotor(1, true));

    // The clearance hook acts on the jetpacking trooper: a flyer overtaking it climbs over its box only while it flies
    // (FIGHTER_FLYING §6.3.1). With a plain (walking) move the flyer cruises at Hover_Offset.
    auto run = [&](bool jet, float& out_z) {
        MovementSystem s{MovementConfig{}};
        land_system(s, 120);
        UnitDesc fl = unit(10, {100.0f, 600.0f}, 6, 3.0f, 10.0f, LocomotorKind::Flying);
        fl.hover_offset = 50.0f;
        fl.z = 50.0f;
        fl.space_layer_type = kLayerFrigate;
        UnitDesc tr = trooper(1, {500.0f, 600.0f});
        tr.yaw_deg = 0.0f;  // facing the destination
        tr.max_speed = 0.2f;
        tr.alt_max_speed = 0.2f;  // slow: the flyer overtakes it
        tr.hover_offset = 25.0f;
        CHECK(s.add_unit(fl) && s.add_unit(tr));
        FlyingHooks hk;
        hk.clearance = [&s, &info_fn](ObjectId id, FlyVec3 a, FlyVec3 b, float m) { return s.clearance_query(id, a, b, m, info_fn); };
        s.set_flying_hooks(hk);
        SimRng r(9);
        MoveOrder ot;
        ot.units = {1};
        ot.destination = {900.0f, 600.0f};
        ot.switch_locomotor = jet;
        s.move(ot, r, 0);
        MoveOrder of;
        of.units = {10};
        of.destination = {900.0f, 600.0f};
        s.move(of, r, 0);
        out_z = 0.0f;
        for (uint32_t k = 0; k < 200; ++k) {
            s.tick(k, r);
            if (s.view(10).pos.x <= 620.0f) out_z = std::max(out_z, s.view(10).z);
        }
        CHECK(s.view(10).pos.x > 620.0f && s.view(1).airborne == jet && s.view(1).secondary_locomotor == jet);
    };
    float walk_z = 0.0f, jet_z = 0.0f;
    run(false, walk_z);
    run(true, jet_z);
    std::printf("multi-locomotor clearance: flyer max z %.2f over the walking trooper, %.2f over the jetpacking one\n",
                static_cast<double>(walk_z), static_cast<double>(jet_z));
    CHECK(walk_z < 60.0f && jet_z > 90.0f);
}

// S7 t14 (MOVEMENT §3.1.1): Alternate_Max_Speed / _Rate_Of_Turn apply only to the jetpack in its airborne kind (pack state 0
// / 0xF..0x13): the formation speed and the planned flight; the take-off turn (0x10) turns at Max_Rate_Of_Turn and the
// landing (0x14) reports Max_Speed.
static void test_jetpack_alternates() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = trooper(1, cell_center(10, 10));
    d.turn_rate = 3.0f;  // the alternate is 12
    CHECK(sys.add_unit(d));
    CHECK(sys.add_unit(unit(2, cell_center(10, 12), 0, 1.5f, 6.0f, LocomotorKind::Default)));  // between 1.0 and 2.5
    const float mult = sys.view(1).max_speed;  // Max_Speed 1.0 x the land multiplier
    CHECK(sys.use_secondary_locomotor(1, true));
    UnitView v = sys.view(1);
    CHECK(v.locomotor == LocomotorKind::Jetpack && v.loco_state == LocoState::None && v.max_speed == 2.5f * mult);
    SimRng rng(4);
    MoveOrder o;
    o.units = {1, 2};
    o.destination = cell_center(60, 11);
    sys.move(o, rng, 0);
    // One formation: the slowest member is the walker (1.5), not the trooper's Max_Speed (1.0).
    CHECK(sys.view(2).formation_speed == 1.5f * mult && sys.view(1).formation_speed == 1.5f * mult);

    // Alone, facing away from the destination: the take-off turn (0x10) is limited by Max_Rate_Of_Turn, the flight by
    // Alternate_Max_Speed.
    MovementSystem one{MovementConfig{}};
    land_system(one, 100);
    d.yaw_deg = 180.0f;
    CHECK(one.add_unit(d));
    MoveOrder j;
    j.units = {1};
    j.destination = cell_center(60, 10);
    j.switch_locomotor = true;
    one.move(j, rng, 0);
    v = one.view(1);
    bool speeds_ok = true, turn_ok = true, seen_landing = false, seen_kind2 = false;
    uint32_t turn_frames = 0;
    float max_step = 0.0f, max_speed = 0.0f, prev_yaw = v.yaw;
    LocoState prev = v.loco_state;
    uint32_t f = 0;
    for (; f < 3000; ++f) {
        one.tick(f, rng);
        v = one.view(1);
        if (v.locomotor != LocomotorKind::Jetpack) break;
        const bool kind2 = locomotor_airborne_kind(v.locomotor, v.loco_state);
        speeds_ok = speeds_ok && v.max_speed == (kind2 ? 2.5f : 1.0f) * mult;
        seen_kind2 = seen_kind2 || kind2;
        seen_landing = seen_landing || v.loco_state == LocoState::WalkEndMove;
        if (prev == LocoState::WalkTurning || v.loco_state == LocoState::WalkTurning) {
            const float step = std::fabs(wrap_deg(v.yaw - prev_yaw));
            turn_ok = turn_ok && step <= 3.0f * mult + 1e-3f;
            max_step = std::max(max_step, step);
            ++turn_frames;
        }
        max_speed = std::max(max_speed, v.speed);
        prev = v.loco_state;
        prev_yaw = v.yaw;
    }
    std::printf("jetpack alternates: take-off turn %u frames (max step %.2f, Max_Rate_Of_Turn %.2f), max flight speed %.2f "
                "(alternate %.2f), back on WALK at %u\n",
                turn_frames, static_cast<double>(max_step), static_cast<double>(3.0f * mult), static_cast<double>(max_speed),
                static_cast<double>(2.5f * mult), f);
    // The turn reached the normal rate and never the alternate one (12 x the multiplier).
    CHECK(speeds_ok && seen_kind2 && seen_landing && turn_ok && turn_frames > 0 && max_step > 2.0f * mult);
    CHECK(max_speed > 2.0f * mult && max_speed <= 2.5f * mult + 1e-3f);  // flown at the alternate, not at Max_Speed 1.0
    v = one.view(1);
    CHECK(v.locomotor == LocomotorKind::Default && v.max_speed == mult && v.arrived);
}

// S7 t15 (MOVEMENT §3.1.1 "Acceleration / deceleration"): the planner request of a jetpack in its airborne kind takes
// Alternate_Max_Speed / _Rate_Of_Turn, but its acceleration / deceleration are the common getters: OverrideAcceleration x
// (effective max speed / Max_Speed), else the NORMAL effective max speed, never the alternate.
static void test_jetpack_accel_fallback() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = trooper(1, cell_center(10, 10));  // Max_Speed 1.0, Alternate_Max_Speed 2.5, no override
    CHECK(sys.add_unit(d));
    UnitDesc o = trooper(2, cell_center(10, 20));
    o.accel = 0.1f;
    o.decel = 0.05f;
    CHECK(sys.add_unit(o));
    const float mult = sys.view(1).max_speed;  // the normal effective max speed (1.0 x the land multiplier)
    SpaceRequest q;
    CHECK(!sys.air_request(1, 0, q));  // walking: not a land air unit
    CHECK(sys.use_secondary_locomotor(1, true) && sys.use_secondary_locomotor(2, true));
    CHECK(sys.air_request(1, 7, q) && q.now == 7u);
    CHECK(q.speed == 2.5f * mult && q.turn_rate == 12.0f * (mult / 1.0f));  // airborne kind: the alternates
    CHECK(q.accel == mult && q.decel == mult);                              // the normal max speed, not 2.5 x mult
    CHECK(sys.air_request(2, 7, q) && q.speed == 2.5f * mult && q.accel == 0.1f * mult && q.decel == 0.05f * mult);
    CHECK(!sys.air_request(99, 7, q));
    std::printf("jetpack accel fallback: accel %.3f (normal max %.3f, alternate %.3f)\n", static_cast<double>(mult),
                static_cast<double>(mult), static_cast<double>(2.5f * mult));
}

// PR #19 review (S8 t09): a jetpack in flight must never repeat a pose (MOVEMENT §3.1.1: a 0x13 frame whose follower step
// leaves position and orientation unchanged drops to 0x16 and swaps back in the air). The planner made zero-length wait
// steps (ops 7 / 18 clamped to the goal distance) at the goal in facing mode; it no longer does. Checks the planner output
// directly (facing-mode requests: no two consecutive waypoints with the same pose and later time) and whole flights with a
// final facing (no repeated pose in 0x13, never 0x16 while airborne).
static void test_jetpack_no_repeated_pose() {
    const MovementConstants k{};
    SpaceOccupancy occ;
    occ.set_box({0.0f, 0.0f}, {4000.0f, 4000.0f});
    SpaceScratch sc;
    uint32_t paths = 0, repeats = 0;
    for (int32_t i = 0; i < 96; ++i) {
        SpaceRequest q;
        q.self = 1;
        q.start = {1000.0f + 37.0f * static_cast<float>(i % 24), 1200.0f};
        q.goal = {1400.0f + 11.0f * static_cast<float>(i % 5), 1500.0f + 23.0f * static_cast<float>(i % 7)};
        q.start_heading = 30.0f * static_cast<float>(i);
        q.speed = 3.9f;
        q.turn_rate = 9.36f;
        q.radius = 6.0f;
        q.accel = 1.56f;
        q.decel = 1.56f;
        q.goal_facing = 45.0f * static_cast<float>(i % 8);
        // A ship of the same layer crossing the goal around the arrival time (from i >= 24): waiting can pay.
        if (i >= 24) {
            const float sp = 1.0f + 0.25f * static_cast<float>(i % 9);
            std::vector<SpaceWaypoint> tw(2);
            tw[0].pos = q.goal + V2{-800.0f, static_cast<float>(i % 3) * 10.0f - 10.0f};
            tw[0].heading = 0.0f;
            tw[0].speed = sp;
            tw[1].pos = q.goal + V2{800.0f, static_cast<float>(i % 3) * 10.0f - 10.0f};
            tw[1].heading = 0.0f;
            tw[1].speed = sp;
            tw[1].time = 1600.0f / sp;
            occ.set_track(2, kLayerFrigate, 30.0f + static_cast<float>(i % 4) * 20.0f, 0, tw);
        }
        SpacePath out;
        plan_space_path(occ, k, q, sc, out);
        ++paths;
        for (size_t w = 1; w < out.wps.size(); ++w) {
            const SpaceWaypoint& a = out.wps[w - 1];
            const SpaceWaypoint& b = out.wps[w];
            if (b.time > a.time && a.pos == b.pos && a.heading == b.heading) ++repeats;
        }
    }
    CHECK(repeats == 0);

    uint32_t flights = 0, bad_frames = 0, air_drops = 0;
    for (int32_t i = 0; i < 12; ++i) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(trooper(1, cell_center(10, 10 + i))));
        CHECK(sys.add_unit(trooper(2, cell_center(40 + 3 * i, 10))));  // a second jetpack crossing the first one's route
        SimRng rng(static_cast<uint32_t>(30 + i));
        MoveOrder j;
        j.units = {1};
        j.destination = cell_center(40 + 3 * (i % 6), 30 + 2 * i);
        j.facing_deg = 60.0f * static_cast<float>(i);
        j.switch_locomotor = true;
        sys.move(j, rng, 0);
        MoveOrder j2;
        j2.units = {2};
        j2.destination = cell_center(40 + 3 * (i % 6) - 2, 40 + 2 * i);
        j2.facing_deg = 30.0f * static_cast<float>(i);
        j2.switch_locomotor = true;
        sys.move(j2, rng, 0);
        UnitView prev = sys.view(1);
        for (uint32_t f = 0; f < 3000; ++f) {
            sys.tick(f, rng);
            const UnitView v = sys.view(1);
            if (prev.loco_state == LocoState::WalkMoving && v.loco_state == LocoState::WalkMoving && v.pos == prev.pos &&
                v.yaw == prev.yaw)
                ++bad_frames;
            if (prev.loco_state == LocoState::WalkMoving && prev.airborne && v.loco_state == LocoState::WalkStopped) ++air_drops;
            prev = v;
            if (v.locomotor == LocomotorKind::Default && f > 0) break;
        }
        CHECK(prev.locomotor == LocomotorKind::Default && prev.arrived);
        bad_frames += sys.stats().jetpack_still_frames;
        ++flights;
    }
    CHECK(bad_frames == 0 && air_drops == 0);
    std::printf("jetpack poses: %u facing plans (%u repeated poses), %u flights (%u repeated 0x13 poses, %u mid-air drops)\n", paths,
                repeats, flights, bad_frames, air_drops);
}

// S7 t15 (MOVEMENT §3.1.1 "Take-off turn: pack +0xe8"): with Has_Pre_Turn_Anim the take-off turn (0x10) turns at the band
// rate picked after each clip (|e| >= 45: R; 22.5..45: R x 0.5; below: R x 0.25 x |e| / 11.25) and stays in 0x10 until the
// turn clip reaches its last 2 frames; the first frame turns at R.
static uint32_t band_anim_len(ObjectId id, int32_t a) {
    switch (a) {
        case kAnimTurnLeft:
        case kAnimTurnRight: return 4;
        case kAnimTurnLeftHalf:
        case kAnimTurnRightHalf: return 6;
        case kAnimTurnLeftQuarter:
        case kAnimTurnRightQuarter: return 30;
        default: return anim_len(id, a);
    }
}

static void test_jetpack_turn_bands() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    sys.set_anim_length_fn(band_anim_len);
    UnitDesc d = unit(1, cell_center(10, 10), 0, 2.0f, 6.0f, LocomotorKind::Jetpack);
    d.turn_rate = 2.0f;
    d.yaw_deg = 180.0f;
    d.hover_offset = 30.0f;
    d.has_pre_turn_anim = true;
    CHECK(sys.add_unit(d));
    // A static obstacle overlapping the start, ahead of the unit: the plan opens with a turn in place to an escape heading
    // far from 180, so the take-off turn is long (the aim is the first waypoint away from the start).
    sys.air().add_static(900, cell_center(10, 10) + V2{-30.0f, 0.0f}, 40.0f);
    const float mult = sys.view(1).max_speed / 2.0f;
    const float R = 2.0f * mult;
    SimRng rng(5);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(70, 10);
    sys.move(o, rng, 0);
    UnitView prev = sys.view(1);
    auto band = [](int32_t a) {  // 0 none, 1 full, 2 half, 3 quarter
        if (a == kAnimTurnLeft || a == kAnimTurnRight) return 1;
        if (a == kAnimTurnLeftHalf || a == kAnimTurnRightHalf) return 2;
        if (a == kAnimTurnLeftQuarter || a == kAnimTurnRightQuarter) return 3;
        return 0;
    };
    uint32_t turn_frames = 0, half_at_rate = 0, aligned_wait = 0;
    bool first_ok = false, steps_ok = true, seen[4] = {false, false, false, false};
    for (uint32_t f = 0; f < 600; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        if (prev.loco_state == LocoState::WalkTurning) {  // a 0x10 service ran this frame
            const float step = std::fabs(wrap_deg(v.yaw - prev.yaw));
            if (turn_frames++ == 0) first_ok = near(step, R, 1e-3f);  // +0xe8 cleared by 0xF: the first frame turns at R
            const int32_t b = band(prev.anim);
            if (b == 1) steps_ok = steps_ok && step <= R + 1e-3f;
            if (b == 2) {
                steps_ok = steps_ok && step <= 0.5f * R + 1e-3f;
                if (near(step, 0.5f * R, 1e-3f)) ++half_at_rate;
            }
            if (b == 3) steps_ok = steps_ok && step <= 0.5f * R + 1e-3f;  // R x 0.25 x |e| / 11.25 with |e| < 22.5
            if (b != 0 && step == 0.0f && v.loco_state == LocoState::WalkTurning) ++aligned_wait;
        }
        if (v.loco_state == LocoState::WalkTurning) seen[band(v.anim)] = true;
        prev = v;
        if (v.loco_state == LocoState::WalkMoving) break;
    }
    CHECK(prev.loco_state == LocoState::WalkMoving && first_ok && steps_ok && seen[1] && seen[2] && seen[3] && half_at_rate > 0 &&
          aligned_wait > 0);
    std::printf("jetpack turn bands: %u turn frames, full / half / quarter %d / %d / %d, %u at R/2, %u aligned frames waiting "
                "for the clip end\n",
                turn_frames, seen[1] ? 1 : 0, seen[2] ? 1 : 0, seen[3] ? 1 : 0, half_at_rate, aligned_wait);
}

// S7 t14 (MOVEMENT §3.1.1 steps 4 and 7): the swap to the jetpack re-issues the unit's current order to it alone (or holds
// it in place: the jetpack reverts one service after the next); the swap back keeps the order and the walk finishes it.
static void test_jetpack_orders() {
    {  // hold: no order -> pack state 0 -> 0x16 at the next service -> WALK at the one after
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(trooper(1, cell_center(10, 10))));
        SimRng rng(1);
        CHECK(sys.use_secondary_locomotor(1, true));
        CHECK(sys.view(1).loco_state == LocoState::None && !sys.view(1).has_goal);
        sys.tick(0, rng);
        CHECK(sys.view(1).secondary_locomotor && sys.view(1).loco_state == LocoState::WalkStopped);
        sys.tick(1, rng);
        CHECK(!sys.view(1).secondary_locomotor && sys.view(1).locomotor == LocomotorKind::Default &&
              sys.view(1).pos == cell_center(10, 10) && rng.draws() == 1);  // cu8j t02: the swap's composition
    }
    {  // re-issue: a walking formation member keeps its goal alone and flies there
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(trooper(1, cell_center(10, 10))) &&
              sys.add_unit(unit(2, cell_center(10, 12), 0, 1.0f, 6.0f, LocomotorKind::Default)));
        SimRng rng(2);
        MoveOrder o;
        o.units = {1, 2};
        o.destination = cell_center(40, 11);
        sys.move(o, rng, 0);
        uint32_t f = 0;
        for (; f < 40; ++f) sys.tick(f, rng);
        const UnitView w = sys.view(1);
        CHECK(w.has_goal && w.formation_speed < 1.0e30f);
        const uint64_t draws = rng.draws();
        CHECK(sys.use_secondary_locomotor(1, true));
        UnitView v = sys.view(1);
        CHECK(v.locomotor == LocomotorKind::Jetpack && v.has_goal && v.goal == w.goal && v.formation_speed > 1.0e30f &&
              v.loco_state == LocoState::None && rng.draws() == draws);  // no stagger draw
        bool flew = false;
        for (; f < 3000 && sys.view(1).secondary_locomotor; ++f) {
            sys.tick(f, rng);
            flew = flew || sys.view(1).airborne;
        }
        v = sys.view(1);
        CHECK(flew && v.locomotor == LocomotorKind::Default && v.arrived && distance(v.pos, w.goal) < 1.0f);
    }
    {  // swap back in flight: the order is kept and walked
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(trooper(1, cell_center(10, 10))));
        SimRng rng(3);
        MoveOrder j;
        j.units = {1};
        j.destination = cell_center(40, 10);
        j.switch_locomotor = true;
        sys.move(j, rng, 0);
        uint32_t f = 0;
        for (; f < 2000 && sys.view(1).loco_state != LocoState::WalkMoving; ++f) sys.tick(f, rng);
        for (const uint32_t end = f + 10; f < end; ++f) sys.tick(f, rng);
        UnitView v = sys.view(1);
        CHECK(v.airborne && v.secondary_locomotor && v.has_goal);
        const V2 goal = v.goal;
        CHECK(sys.use_secondary_locomotor(1, false));
        v = sys.view(1);
        CHECK(v.locomotor == LocomotorKind::Default && !v.secondary_locomotor && !v.airborne && v.has_goal && v.goal == goal &&
              v.z == (v.pos.x > 1000.0f ? 40.0f : 0.0f));  // Walk Initialise conforms to the terrain
        bool walked = true;
        for (const uint32_t end = f + 3000; f < end && !sys.view(1).arrived; ++f) {
            sys.tick(f, rng);
            walked = walked && sys.view(1).locomotor == LocomotorKind::Default && !sys.view(1).airborne;
        }
        v = sys.view(1);
        CHECK(walked && v.arrived && distance(v.pos, goal) < 1.0f);
    }
}

// S7 t14 / PR #14 review: the swap needs a JETPACK_LOCOMOTOR Secondary_Locomotor_Name and a non-JETPACK primary (retail
// would rebuild a non-JetPack secondary on every call and never swap back, MOVEMENT §3.1.1 step 2); unit_desc_from_type
// reads Alternate_Max_Speed / _Rate_Of_Turn unconditionally (type defaults 1.0 / 2.0).
static void test_multi_locomotor_types() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "alamo_tactical_movement_gaps_multi_types";
    std::error_code ec;
    remove_all_long(root, ec);
    auto put = [](const std::filesystem::path& p, const std::string& s) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << s;
    };
    const std::string multi = "<Uses_Multiple_Locomotors>Yes</Uses_Multiple_Locomotors><LandBehavior>WALK_LOCOMOTOR</LandBehavior>";
    put(root / "Data/XML/GameObjectFiles.xml", "<L><File>ML.xml</File></L>");
    put(root / "Data/XML/ML.xml",
        "<R>\n"
        "<GroundInfantry Name=\"Jet\">" + multi + "<Primary_Locomotor_Name>WALK_LOCOMOTOR</Primary_Locomotor_Name>"
        "<Secondary_Locomotor_Name>JETPACK_LOCOMOTOR</Secondary_Locomotor_Name></GroundInfantry>\n"
        "<GroundInfantry Name=\"Jet_Alt\">" + multi + "<Secondary_Locomotor_Name>jetpack_locomotor</Secondary_Locomotor_Name>"
        "<Alternate_Max_Speed>2.5</Alternate_Max_Speed><Alternate_Max_Rate_Of_Turn>6</Alternate_Max_Rate_Of_Turn></GroundInfantry>\n"
        "<GroundInfantry Name=\"Bike_Secondary\">" + multi + "<Primary_Locomotor_Name>WALK_LOCOMOTOR</Primary_Locomotor_Name>"
        "<Secondary_Locomotor_Name>BIKE_LOCOMOTOR</Secondary_Locomotor_Name></GroundInfantry>\n"
        "<GroundInfantry Name=\"Walk_Secondary\">" + multi + "<Secondary_Locomotor_Name>WALK_LOCOMOTOR</Secondary_Locomotor_Name>"
        "</GroundInfantry>\n"
        "<GroundInfantry Name=\"Jet_Primary\">" + multi + "<Primary_Locomotor_Name>JETPACK_LOCOMOTOR</Primary_Locomotor_Name>"
        "<Secondary_Locomotor_Name>JETPACK_LOCOMOTOR</Secondary_Locomotor_Name></GroundInfantry>\n"
        "<GroundInfantry Name=\"Unknown_Secondary\">" + multi + "<Secondary_Locomotor_Name>NOT_A_LOCOMOTOR</Secondary_Locomotor_Name>"
        "</GroundInfantry>\n"
        "<GroundInfantry Name=\"No_Secondary\">" + multi + "</GroundInfantry>\n"
        "</R>");
    DirectoryProvider dp(path_to_utf8(root));
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    CHECK(raw.load(dp) && types.build(raw, &dp));
    const auto desc = [&types](const char* name) {
        const uint32_t t = types.resolve_name(name);
        CHECK(t < types.size());
        return t < types.size() ? unit_desc_from_type(types, t) : UnitDesc{};
    };
    const UnitDesc jet = desc("Jet"), alt = desc("Jet_Alt"), bike = desc("Bike_Secondary"), walk = desc("Walk_Secondary");
    const UnitDesc jprim = desc("Jet_Primary"), unknown = desc("Unknown_Secondary"), none = desc("No_Secondary");
    CHECK(jet.uses_multiple_locomotors && jet.has_secondary_locomotor && jet.secondary_locomotor == LocomotorKind::Jetpack &&
          jet.alt_max_speed == 1.0f && jet.alt_turn_rate == 2.0f);  // the type defaults
    CHECK(alt.has_secondary_locomotor && alt.alt_max_speed == 2.5f && alt.alt_turn_rate == 6.0f);
    CHECK(bike.uses_multiple_locomotors && !bike.has_secondary_locomotor && bike.secondary_locomotor == LocomotorKind::Bike);
    CHECK(!walk.has_secondary_locomotor && !jprim.has_secondary_locomotor && !unknown.has_secondary_locomotor &&
          !none.has_secondary_locomotor);
    // A refused secondary never swaps: the switch bit walks, the order is not re-issued, nothing to swap back from.
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc b = bike;
    b.id = 1;
    b.pos = cell_center(10, 10);
    UnitDesc j = jet;
    j.id = 2;
    j.pos = cell_center(10, 30);
    CHECK(sys.add_unit(b) && sys.add_unit(j));
    CHECK(!sys.use_secondary_locomotor(1, true) && !sys.use_secondary_locomotor(1, false));
    CHECK(sys.use_secondary_locomotor(2, true) && sys.view(2).locomotor == LocomotorKind::Jetpack);
    SimRng rng(6);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(30, 10);
    o.switch_locomotor = true;
    sys.move(o, rng, 0);
    bool walked = true;
    for (uint32_t f = 0; f < 2000 && !sys.view(1).arrived; ++f) {
        sys.tick(f, rng);
        walked = walked && sys.view(1).locomotor == LocomotorKind::Default && !sys.view(1).secondary_locomotor;
    }
    CHECK(walked && sys.view(1).arrived && !sys.view(2).secondary_locomotor);  // the idle jetpacker reverted meanwhile
    remove_all_long(root, ec);
}

static void test_bike() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Bike);
    d.yaw_deg = 0.0f;
    d.max_rate_of_roll = 1.5f;
    d.bank_turn_angle = 20.0f;
    d.close_enough_angle = 30.0f;
    d.turn_rate = 4.0f;
    CHECK(sys.add_unit(d));
    SimRng rng(3);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(10, 60);  // 90 deg to the left: turn in place, then lean while correcting
    sys.move(o, rng, 0);
    std::vector<uint8_t> states;
    float max_lean = 0.0f;
    bool lean_ok = true;
    uint32_t f = 0;
    for (; f < 2000; ++f) {
        sys.tick(f, rng);
        const UnitView v = sys.view(1);
        if (states.empty() || states.back() != static_cast<uint8_t>(v.loco_state)) states.push_back(static_cast<uint8_t>(v.loco_state));
        max_lean = std::max(max_lean, std::fabs(v.roll));
        lean_ok = lean_ok && std::fabs(v.roll) <= 20.0f;
        if (v.loco_state == LocoState::BikeStopped && v.arrived) break;
    }
    auto seen = [&](uint8_t s) { return std::find(states.begin(), states.end(), s) != states.end(); };
    CHECK(seen(0x20) && seen(0x21) && seen(0x22) && seen(0x24) && states.back() == 0x25 && lean_ok && max_lean > 0.0f);
    for (uint32_t g = 0; g < 40; ++g) sys.tick(f + 1 + g, rng);
    CHECK(sys.view(1).roll == 0.0f && distance(sys.view(1).pos, o.destination) < 1.0f);
    std::printf("bike: %zu states, max lean %.2f\n", states.size(), static_cast<double>(max_lean));
}

static void test_bomber() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc d = unit(9, cell_center(5, 50), 17, 4.0f, 10.0f, LocomotorKind::LandBomber);
    d.prepare_strafe_height = 120.0f;
    CHECK(sys.add_unit(d));
    SimRng rng(1);
    MoveOrder o;
    o.units = {9};
    o.destination = cell_center(60, 50);  // terrain 40 there
    sys.move(o, rng, 0);
    bool gone = false, z_ok = true;
    float max_x = 0.0f;
    for (uint32_t f = 0; f < 2000 && !gone; ++f) {
        sys.tick(f, rng);
        for (ObjectId id : sys.despawned()) gone = gone || id == 9;
        if (!gone && sys.view(9).loco_state == LocoState::WalkMoving) {
            z_ok = z_ok && sys.view(9).z == 160.0f;
            max_x = std::max(max_x, sys.view(9).pos.x);
        }
    }
    // The pass runs to the destination + 2 x XYExpansionDistanceLand (120) and the bomber is removed there.
    std::printf("bomber: gone %d, z ok %d, last x %.2f\n", gone ? 1 : 0, z_ok ? 1 : 0, static_cast<double>(max_x));
    CHECK(gone && !sys.has_unit(9) && z_ok && max_x > 1200.0f && max_x <= 1320.0f + 1e-3f);
}

static void test_fighter_space() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    MovementSystem sys(cfg);
    sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
    const ThreatFn blob = [](V2 p) { return length(p) < 700.0f ? 100.0f : 0.0f; };
    sys.set_threat_fn([&blob](int32_t, V2 p) { return blob(p); });
    UnitDesc f = unit(1, {-2000.0f, 0.0f}, -1, 4.0f, 10.0f, LocomotorKind::Fighter);
    f.space_layer = kLayerFighter;
    f.space_layer_type = 0;
    CHECK(sys.add_unit(f));
    UnitDesc c = unit(2, {-2000.0f, 600.0f}, -1, 2.0f, 60.0f, LocomotorKind::Default);
    c.space_layer_type = 2;  // frigate: threat mode not honoured
    CHECK(sys.add_unit(c));
    SimRng rng(11);
    MoveOrder o;
    o.units = {1};
    o.destination = {2000.0f, 0.0f};
    o.threat_threshold = 10.0f;
    sys.move(o, rng, 0);
    CHECK(sys.view(1).loco_state == LocoState::FighterMoving);
    MoveOrder oc = o;
    oc.units = {2};
    oc.destination = {2000.0f, 600.0f};
    sys.move(oc, rng, 0);
    sys.tick(0, rng);
    bool aligned = true, clear = true;
    const auto wf = sys.space_path(1);
    for (size_t i = 1; i + 1 < wf.size(); ++i) {
        aligned = aligned && compass_aligned(wf[i].pos - wf[i - 1].pos);
        clear = clear && length(wf[i].pos) >= 700.0f;
    }
    CHECK(wf.size() >= 3 && aligned && clear);
    uint32_t fr = 1;
    for (; fr < 4000; ++fr) {
        sys.tick(fr, rng);
        if (sys.view(1).loco_state == LocoState::FighterIdle && !sys.view(2).has_goal) break;
    }
    const UnitView v = sys.view(1);
    // FIGHTER_FLYING §4.1 / §4.2: the reactive fighter passes its last waypoint, enters Idle with D = that waypoint and
    // (solo) brakes by Max_Thrust to SpaceIdleMovementSpeed 0: it comes to rest a braking distance past it.
    CHECK(v.arrived && v.loco_state == LocoState::FighterIdle && distance(v.pos, o.destination) < 60.0f);
    for (uint32_t g = 1; g <= 20; ++g) sys.tick(fr + g, rng);
    CHECK(sys.view(1).speed == 0.0f);
    fr += 20;
    // A teleported fighter rests where it lands (V = 0).
    CHECK(sys.teleport(1, {2030.0f, 0.0f}, 0.0f, 0.0f, fr));
    for (uint32_t g = 1; g <= 20; ++g) sys.tick(fr + g, rng);
    CHECK(distance(sys.view(1).pos, {2030.0f, 0.0f}) < 1e-3f);
    // Attack target: the next move is Moving (0x1B) at order time [port, S7 t12: the target fed then may be the old order's]
    // and turns into Directed_Combat (0x1E) on its first fighter frame (§2 step 6).
    FighterTarget pt;
    pt.point = true;  // a point target at the origin
    CHECK(sys.set_fighter_target(1, pt));
    sys.move(o, rng, fr + 30);
    CHECK(sys.view(1).loco_state == LocoState::FighterMoving);
    sys.tick(fr + 30, rng);
    CHECK(sys.view(1).loco_state == LocoState::FighterDirectedCombat);
    std::printf("fighter: threat path %zu waypoints, idle at frame %u\n", wf.size(), fr);
}

// S7 t37 / s78f t02 (A1_READINGS §33.4.1, FIGHTER_FLYING §4.10 E / §4.11): a fighter's move with an object goal (guard
// flag) builds a formation whose only block is the object block. While the formation moves (state 1) and the object
// moves, the fighter, its centre within R = min(attack range / 2, Space_Guard_Range) of it, flies a copy of the object's
// path at min(own speed, 2 x the object's max speed) (a solo fighter is capped through its own formation, §4.11 F); the
// re-path when the recorded goal is more than R away; after arrival the 30-frame re-check 0x410a60 re-plans when the
// fighter is out of its attack range of the object. The object's removal turns the block into a position block at the last
// waypoint (flags kept); a plain move replaces the formation with a position block.
static void test_fighter_guard() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    MovementSystem sys(cfg);
    sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
    UnitDesc f = unit(1, {-2000.0f, 1500.0f}, -1, 10.0f, 10.0f, LocomotorKind::Fighter);
    f.space_layer = kLayerFighter;
    f.space_layer_type = 0;
    CHECK(sys.add_unit(f));
    UnitDesc c = unit(2, {-2000.0f, 0.0f}, -1, 2.0f, 60.0f, LocomotorKind::Default);
    c.space_layer_type = 2;
    CHECK(sys.add_unit(c));
    SimRng rng(5);
    MoveOrder g;
    g.units = {1};
    g.destination = {-2000.0f, 0.0f};
    g.flags = kMoveFlagGuardMove;
    g.goal_object = 2;
    sys.move(g, rng, 0);
    const FighterFormation* fg = sys.fighter_formation(1);
    CHECK(fg && fg->blocks.size() == 1 && fg->blocks[0].kind == kFighterBlockObject && fg->blocks[0].object == 2 &&
          fg->blocks[0].flags == kMoveFlagGuardMove && fg->follow && fg->next == kStartPending &&
          fg->state == kFormationMoving && fg->speed == -1.0f);
    {  // cu8j t01: the composition (next coordinator update) draws int(1, FPS); the first check is F + 30 + r
        SimRng ref = rng;
        sys.coordinator_update(0, 0, rng);
        const int32_t r = ref.range_int(1, kFormationFps);
        fg = sys.fighter_formation(1);
        CHECK(fg && fg->next == static_cast<uint32_t>(kFormationFps + r) && rng.state() == ref.state());
    }
    MoveOrder mc;
    mc.units = {2};
    mc.destination = {1500.0f, 0.0f};
    sys.move(mc, rng, 0);
    CHECK(sys.fighter_formation(1) != nullptr);  // another unit's order leaves the fighter's formation alone
    bool copied = false, repathed = false;
    uint32_t fr = 0;
    for (; fr < 3000 && !copied; ++fr) {
        sys.tick(fr, rng);
        const UnitView v = sys.view(1), cv = sys.view(2);
        copied = cv.has_goal && !cv.arrived && v.has_goal && v.goal.x == cv.goal.x && v.goal.y == cv.goal.y;
    }
    // The cap (F +0x21c) = min(the fighter's speed, 2 x the carrier's max speed); the solo fighter flies at most that fast.
    const float cap = 2.0f * sys.view(2).max_speed;
    fg = sys.fighter_formation(1);
    const bool capped = fg && fg->speed == std::min(sys.view(1).max_speed, cap) && fg->speed < sys.view(1).max_speed;
    // The cap holds only while the formation speed is capped: when |L - G| reaches R the update resets it to the uncapped
    // speed and re-paths (§4.11 D). Sample the fighter's speed after it has flown 15 frames under the cap.
    float fastest = 0.0f;
    uint32_t under = 0;
    for (uint32_t k = 0; k < 150; ++k, ++fr) {
        sys.tick(fr, rng);
        const FighterFormation* fk = sys.fighter_formation(1);
        under = (fk && fk->speed == cap && sys.view(2).has_goal) ? under + 1 : 0;
        if (under >= 15 && sys.view(1).loco_state == LocoState::FighterMoving) fastest = std::max(fastest, sys.view(1).speed);
    }
    const bool slow = fastest > 0.0f && fastest <= cap + 1e-3f;
    // Save round trip with a live formation (movement save v10): same formation, identical continuation.
    {
        ByteWriter w;
        sys.write(w);
        MovementSystem copy(cfg);
        copy.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done());
        const FighterFormation* a = sys.fighter_formation(1);
        const FighterFormation* b = copy.fighter_formation(1);
        CHECK(a && b && a->key == b->key && a->blocks.size() == b->blocks.size() && a->blocks[0].object == b->blocks[0].object &&
              a->blocks[0].flags == b->blocks[0].flags && a->goal.x == b->goal.x && a->goal.y == b->goal.y && a->next == b->next &&
              a->follow == b->follow && a->state == b->state && a->speed == b->speed && a->single_arrived == b->single_arrived);
        SimRng ra = rng, rb = rng;
        bool same = true;
        for (uint32_t k = fr; k < fr + 90; ++k) {
            sys.tick(k, ra);
            copy.tick(k, rb);
            StateHasher ha, hb;
            sys.hash_units(ha);
            copy.hash_units(hb);
            same = same && ha.digest() == hb.digest();
        }
        CHECK(same && copy.fighter_formation(1) && copy.fighter_formation(1)->next == sys.fighter_formation(1)->next);
        fr += 90;
        rng = ra;
    }
    // Teleport the fighter away (out of its attack range): the formation is done (state 3), and 0x410a60 re-plans it
    // toward the object (the recorded goal moves to the object).
    CHECK(sys.teleport(1, {-2000.0f, 3000.0f}, 0.0f, 0.0f, fr));
    const V2 before = sys.fighter_formation(1)->goal;
    for (uint32_t k = 0; k < 400 && !repathed; ++k, ++fr) {
        sys.tick(fr, rng);
        repathed = distance(sys.fighter_formation(1)->goal, before) > 1.0f &&
                   distance(sys.fighter_formation(1)->goal, sys.view(2).pos) < 10.0f;
    }
    // The object leaves: kind 1 at the last waypoint of the fighter's path, flags kept.
    const auto wp = sys.space_path(1);
    const V2 last = wp.empty() ? sys.fighter_formation(1)->goal : wp.back().pos;
    CHECK(sys.remove_unit(2));
    fg = sys.fighter_formation(1);
    const bool lost = fg && fg->blocks.size() == 1 && fg->blocks[0].kind == kFighterBlockPosition && fg->blocks[0].object == -1 &&
                      fg->blocks[0].flags == kMoveFlagGuardMove && distance(fg->blocks[0].point, last) < 1e-3f;
    MoveOrder p;
    p.units = {1};
    p.destination = {0.0f, 0.0f};
    sys.move(p, rng, fr);
    fg = sys.fighter_formation(1);
    const bool replaced = fg && fg->blocks.size() == 1 && fg->blocks[0].kind == kFighterBlockPosition && fg->blocks[0].flags == 0;
    std::printf("fighter guard: path copy %d (frame %u), cap %d (fastest %.3f <= %.3f: %d), re-path %d, object lost -> position "
                "guard %d, plain move replaces %d\n",
                copied ? 1 : 0, fr, capped ? 1 : 0, static_cast<double>(fastest), static_cast<double>(cap), slow ? 1 : 0,
                repathed ? 1 : 0, lost ? 1 : 0, replaced ? 1 : 0);
    CHECK(copied && capped && slow && repathed && lost && replaced);
}

// s78f t02 (FIGHTER_FLYING §4.11 B, C, E; checklist I 10): the block stack of a fighter formation.
//  1. a shift-queued waypoint after a guard of a stationary object goes to the bottom (flag 0x04) and the guard is
//     popped within one 30-frame check; the formation then flies to the waypoint; save round trip of a two-block stack;
//  2. the waypoint append's patrol loop (a click within 80 of an earlier waypoint) and the MaxWaypointsPerPath limit;
//  3. an auto block pushed over a guard: the pre-pop replaces an earlier auto block but never the guard; the auto block
//     is popped when the fighter leaves Guard_Chase_Range of the guarded object (0x410a60 / 0x4165f0);
//  4. a hidden auto target over a guard is dropped (fog: step 5 / 0x410a60);
//  5. an arrived single object block whose object is hidden becomes a position guard at L (C 3);
//  6. / 7. s78f review: an arrived guard with a shift waypoint pops within one check; fighter_new_order; object_removed.
static void test_fighter_formation_stack() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    const auto build = [&](MovementSystem& sys) {
        sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        UnitDesc f = unit(1, {-2000.0f, 1500.0f}, -1, 10.0f, 10.0f, LocomotorKind::Fighter);
        f.space_layer = kLayerFighter;
        CHECK(sys.add_unit(f));
        CHECK(sys.add_unit(unit(2, {0.0f, 0.0f}, -1, 2.0f, 60.0f, LocomotorKind::Default)));    // guarded (stationary)
        CHECK(sys.add_unit(unit(3, {600.0f, 0.0f}, -1, 2.0f, 30.0f, LocomotorKind::Default)));  // enemy ship
        CHECK(sys.add_unit(unit(4, {0.0f, 700.0f}, -1, 2.0f, 30.0f, LocomotorKind::Default)));  // enemy ship
        FighterParams fp;
        fp.chase.guard = 1000.0f;
        fp.chase.idle = 200.0f;
        fp.chase.attack_move = 300.0f;
        CHECK(sys.set_fighter_params(1, fp));
    };
    const auto guard = [](MovementSystem& sys, SimRng& rng, uint32_t frame) {
        MoveOrder g;
        g.units = {1};
        g.destination = {0.0f, 0.0f};
        g.flags = kMoveFlagGuardMove;
        g.goal_object = 2;
        sys.move(g, rng, frame);
    };
    const auto waypoint = [](MovementSystem& sys, SimRng& rng, uint32_t frame, V2 at) {
        MoveOrder w;
        w.units = {1};
        w.destination = at;
        w.flags = static_cast<uint8_t>(kMoveFlagQueue | kMoveFlagPlayerOrder);
        w.waypoint = true;
        sys.move(w, rng, frame);
    };
    // 1. Shift waypoint after a guard of a stationary object.
    bool queued = false, not_moved = false, popped = false, flew = false, saved = false;
    uint32_t pop_frame = 0;
    {
        MovementSystem sys(cfg);
        build(sys);
        SimRng rng(9);
        guard(sys, rng, 0);
        for (uint32_t fr = 0; fr < 5; ++fr) sys.tick(fr, rng);
        const V2 goal0 = sys.view(1).goal;
        waypoint(sys, rng, 5, {1000.0f, 1000.0f});
        const FighterFormation* f = sys.fighter_formation(1);
        queued = f && f->blocks.size() == 2 && f->blocks[0].kind == kFighterBlockPosition &&
                 f->blocks[0].flags == (kMoveFlagQueue | kMoveFlagPlayerOrder) && f->blocks[1].object == 2 && f->state == kFormationPlan;
        not_moved = sys.view(1).goal.x == goal0.x && sys.view(1).goal.y == goal0.y;
        {  // save round trip of the two-block stack
            ByteWriter w;
            sys.write(w);
            MovementSystem copy(cfg);
            copy.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
            ByteReader r(w.bytes());
            const bool read = copy.read(r) && r.done();
            const FighterFormation* a = sys.fighter_formation(1);
            const FighterFormation* b = copy.fighter_formation(1);
            saved = read && a && b && a->blocks.size() == b->blocks.size() && a->state == b->state;
            for (size_t i = 0; saved && i < a->blocks.size(); ++i) {
                const FighterBlock& x = a->blocks[i];
                const FighterBlock& y = b->blocks[i];
                saved = x.kind == y.kind && x.object == y.object && x.point.x == y.point.x && x.point.y == y.point.y &&
                        x.flags == y.flags && x.pushed == y.pushed && x.loop == y.loop && x.own_speed == y.own_speed;
            }
            SimRng ra = rng, rb = rng;
            for (uint32_t k = 5; k < 65; ++k) {
                sys.tick(k, ra);
                copy.tick(k, rb);
                StateHasher ha, hb;
                sys.hash_units(ha);
                copy.hash_units(hb);
                saved = saved && ha.digest() == hb.digest();
            }
        }
        MovementSystem sys2(cfg);  // the same scenario again, run on
        build(sys2);
        SimRng rng2(9);
        guard(sys2, rng2, 0);
        for (uint32_t fr = 0; fr < 5; ++fr) sys2.tick(fr, rng2);
        waypoint(sys2, rng2, 5, {1000.0f, 1000.0f});
        for (uint32_t fr = 5; fr < 400; ++fr) {
            sys2.tick(fr, rng2);
            const FighterFormation* h = sys2.fighter_formation(1);
            if (!popped && h && h->blocks.size() == 1 && h->blocks[0].kind == kFighterBlockPosition) {
                popped = true;
                pop_frame = fr;
            }
            if (popped && distance(sys2.view(1).goal, {1000.0f, 1000.0f}) < 100.0f && sys2.view(1).has_goal) flew = true;
        }
    }
    // 2. Patrol loop and the waypoint limit.
    bool loop = false, limit = false;
    {
        MovementSystem sys(cfg);
        build(sys);
        SimRng rng(9);
        guard(sys, rng, 0);
        waypoint(sys, rng, 1, {1000.0f, 1000.0f});
        waypoint(sys, rng, 1, {1000.0f, -1000.0f});
        waypoint(sys, rng, 1, {1050.0f, 1000.0f});  // within 80 of the first waypoint: closes the loop
        const FighterFormation* f = sys.fighter_formation(1);
        loop = f && f->blocks.size() == 3 && f->blocks[0].loop && f->blocks[1].loop && !f->blocks[2].loop;
        for (int32_t k = 0; k < 8; ++k) waypoint(sys, rng, 1, {-1000.0f - 200.0f * static_cast<float>(k), 2000.0f});
        f = sys.fighter_formation(1);
        uint32_t q = 0;
        for (const FighterBlock& b : f->blocks) q += (b.flags & kMoveFlagQueue) != 0 ? 1u : 0u;
        limit = q == cfg.constants.max_waypoints_per_path;
    }
    // 3. Auto block over a guard: pre-pop and the leash pop.
    bool prepop = false, kept_near = true, leash_pop = false;
    {
        MovementSystem sys(cfg);
        build(sys);
        SimRng rng(9);
        guard(sys, rng, 0);
        uint32_t fr = 0;
        for (; fr < 400; ++fr) sys.tick(fr, rng);  // arrived next to the guarded object
        CHECK(sys.fighter_push(1, 3));
        CHECK(sys.fighter_push(1, 4));  // replaces the auto block on 3, keeps the guard
        const FighterFormation* f = sys.fighter_formation(1);
        prepop = f && f->blocks.size() == 2 && f->blocks[0].object == 2 && f->blocks[1].object == 4 && f->blocks[1].pushed &&
                 f->blocks[1].own_speed && f->state == kFormationPlan;
        for (uint32_t k = 0; k < 90; ++k, ++fr) {
            sys.tick(fr, rng);
            const FighterFormation* h = sys.fighter_formation(1);
            kept_near = kept_near && h && h->blocks.size() == 2;
        }
        CHECK(sys.teleport(1, {0.0f, 2500.0f}, 0.0f, 0.0f, fr));  // beyond Guard_Chase_Range (1000) of the guarded object
        for (uint32_t k = 0; k < 40 && !leash_pop; ++k, ++fr) {
            sys.tick(fr, rng);
            const FighterFormation* h = sys.fighter_formation(1);
            leash_pop = h && h->blocks.size() == 1 && h->blocks[0].object == 2;
        }
        CHECK(!sys.fighter_pop(1));  // one block: nothing to pop
    }
    // 4. A hidden auto target over a guard is dropped.
    bool hidden_pop = false;
    {
        MovementSystem sys(cfg);
        build(sys);
        bool hide = false;
        FighterFormationHooks hk;
        hk.hidden = [&hide](int32_t, ObjectId o) { return hide && o == 3; };
        sys.set_fighter_formation_hooks(hk);
        SimRng rng(9);
        guard(sys, rng, 0);
        uint32_t fr = 0;
        for (; fr < 200; ++fr) sys.tick(fr, rng);
        CHECK(sys.fighter_push(1, 3));
        for (uint32_t k = 0; k < 20; ++k, ++fr) sys.tick(fr, rng);
        hide = true;
        for (uint32_t k = 0; k < 40 && !hidden_pop; ++k, ++fr) {
            sys.tick(fr, rng);
            const FighterFormation* h = sys.fighter_formation(1);
            hidden_pop = h && h->blocks.size() == 1 && h->blocks[0].object == 2;
        }
    }
    // 5. C 3: an arrived single guard block on a hidden object (here a non-mover, positioned by the hook) becomes a
    // position guard at L.
    bool c3 = false;
    {
        MovementSystem sys(cfg);
        build(sys);
        FighterFormationHooks hk;
        bool hide = false;
        hk.hidden = [&hide](int32_t, ObjectId o) { return hide && o == 50; };
        hk.position = [](ObjectId o, F3& p) {
            if (o != 50) return false;
            p = {-1000.0f, 1500.0f, 0.0f};
            return true;
        };
        sys.set_fighter_formation_hooks(hk);
        SimRng rng(9);
        MoveOrder g;
        g.units = {1};
        g.destination = {-1000.0f, 1500.0f};
        g.flags = kMoveFlagGuardMove;
        g.goal_object = 50;
        sys.move(g, rng, 0);
        uint32_t fr = 0;
        for (; fr < 400; ++fr) sys.tick(fr, rng);
        CHECK(sys.teleport(1, {-1000.0f, 1500.0f}, 0.0f, 0.0f, fr));  // at the object: d < 40
        hide = true;
        for (uint32_t k = 0; k < 400 && !c3; ++k, ++fr) {
            sys.tick(fr, rng);
            const FighterFormation* h = sys.fighter_formation(1);
            c3 = h && h->blocks.size() == 1 && h->blocks[0].kind == kFighterBlockPosition && h->blocks[0].flags == kMoveFlagGuardMove;
        }
    }
    // 6. s78f review (B 3): the guard has ARRIVED (state 3) next to a still object, then a shift waypoint is queued: the
    // re-plan to the guard ends at once (state 3 again with the guard on top), yet the guard pops within one 30-frame check.
    bool arrived_first = false, arrived_pop = false, rearrived = false;
    uint32_t wp_frame = 0, arrived_pop_frame = 0;
    {
        MovementSystem sys(cfg);
        build(sys);
        SimRng rng(9);
        guard(sys, rng, 0);
        uint32_t fr = 0;
        for (; fr < 600; ++fr) sys.tick(fr, rng);
        // Queue the waypoint right after a 0x410a60 check, so the timer is a full interval away.
        while (fr < 700 && sys.fighter_formation(1) && sys.fighter_formation(1)->next != fr + kFighterGuardInterval - 1u) sys.tick(fr++, rng);
        arrived_first = sys.fighter_formation(1) && sys.fighter_formation(1)->state == kFormationArrived;
        wp_frame = fr;
        waypoint(sys, rng, fr, {1000.0f, 1000.0f});
        for (uint32_t k = 0; k < 200 && !arrived_pop; ++k, ++fr) {
            sys.tick(fr, rng);
            const FighterFormation* h = sys.fighter_formation(1);
            rearrived = rearrived || (h && h->blocks.size() == 2 && h->state == kFormationArrived);
            if (h && h->blocks.size() == 1 && h->blocks[0].kind == kFighterBlockPosition) {
                arrived_pop = true;
                arrived_pop_frame = fr;
            }
        }
    }
    // 6b. The same after a follow: the guarded object moves a short way and stops; the escort's copied path ends with it,
    // so the formation can arrive (state 3, guard on top) before its next check. The guard still pops within one check of
    // the object's stop. Several trip lengths (different timer phases).
    uint32_t follow_runs = 0, follow_ok = 0, follow_rearrived = 0;
    for (float trip = 200.0f; trip <= 1400.0f; trip += 100.0f) {
        MovementSystem sys(cfg);
        build(sys);
        SimRng rng(9);
        guard(sys, rng, 0);
        uint32_t fr = 0;
        for (; fr < 400; ++fr) sys.tick(fr, rng);
        MoveOrder mc;
        mc.units = {2};
        mc.destination = {0.0f, -trip};
        sys.move(mc, rng, fr);
        waypoint(sys, rng, fr, {1000.0f, 1000.0f});
        uint32_t stop = 0;
        bool popped6 = false, re = false;
        for (uint32_t k = 0; k < 3000 && !popped6; ++k, ++fr) {
            sys.tick(fr, rng);
            const UnitView cv = sys.view(2);
            if (stop == 0 && (!cv.has_goal || cv.arrived)) stop = fr;
            const FighterFormation* h = sys.fighter_formation(1);
            re = re || (h && h->blocks.size() == 2 && h->state == kFormationArrived);
            if (h && h->blocks.size() == 1 && h->blocks[0].kind == kFighterBlockPosition) {
                popped6 = true;
                ++follow_runs;
                if (stop > 0 && fr <= stop + kFighterGuardInterval + 1u) ++follow_ok;
                if (re) ++follow_rearrived;
            }
        }
        if (!popped6) ++follow_runs;
    }
    // 7. s78f review: a new group move that moves nobody (fighter_new_order) replaces the stack; a non-mover named by an
    // auto block (a station positioned by the hook) that is gone (object_removed) loses the block at once.
    bool new_order = false, station_pop = false;
    {
        MovementSystem sys(cfg);
        build(sys);
        FighterFormationHooks hk;
        hk.position = [](ObjectId o, F3& p) {
            if (o != 60) return false;
            p = {200.0f, 100.0f, 0.0f};
            return true;
        };
        sys.set_fighter_formation_hooks(hk);
        SimRng rng(9);
        guard(sys, rng, 0);
        waypoint(sys, rng, 1, {1000.0f, 1000.0f});
        uint32_t fr = 1;
        for (; fr < 40; ++fr) sys.tick(fr, rng);
        CHECK(sys.fighter_push(1, 60));
        const size_t before = sys.fighter_formation(1) ? sys.fighter_formation(1)->blocks.size() : 0;
        sys.object_removed(60);
        const FighterFormation* h = sys.fighter_formation(1);
        station_pop = before >= 2 && h && h->blocks.size() == before - 1 && h->blocks.back().object != 60;
        MoveOrder a;
        a.units = {1};
        a.destination = {600.0f, 0.0f};
        a.goal_object = 3;
        const V2 goal0 = sys.view(1).goal;
        sys.fighter_new_order(a, fr);
        h = sys.fighter_formation(1);
        new_order = h && h->blocks.size() == 1 && h->blocks[0].kind == kFighterBlockObject && h->blocks[0].object == 3 &&
                    h->blocks[0].flags == 0 && h->state == kFormationMoving && sys.view(1).goal.x == goal0.x && sys.view(1).goal.y == goal0.y;
    }
    std::printf("fighter formation stack: waypoint queued %d (not moved %d, saved %d), guard popped %d (frame %u), flew to "
                "it %d; patrol loop %d, waypoint limit %d; auto pre-pop %d, kept near %d, leash pop %d; hidden pop %d; hidden "
                "goal -> position guard %d; arrived guard %d + waypoint (frame %u, re-arrived %d) popped %d (frame %u); after a follow %u of %u "
                "popped within a check of the stop (%u re-arrived first); station gone -> block dropped %d; in-range group move "
                "replaces the stack %d\n",
                queued ? 1 : 0, not_moved ? 1 : 0, saved ? 1 : 0, popped ? 1 : 0, pop_frame, flew ? 1 : 0, loop ? 1 : 0,
                limit ? 1 : 0, prepop ? 1 : 0, kept_near ? 1 : 0, leash_pop ? 1 : 0, hidden_pop ? 1 : 0, c3 ? 1 : 0,
                arrived_first ? 1 : 0, wp_frame, rearrived ? 1 : 0, arrived_pop ? 1 : 0, arrived_pop_frame, follow_ok, follow_runs, follow_rearrived, station_pop ? 1 : 0,
                new_order ? 1 : 0);
    // cu8j t01: the guard's formation is composed at frame 0, its first check at 0 + 30 + int(1, 30) <= 60.
    CHECK(queued && not_moved && saved && popped && pop_frame <= 2u * kFighterGuardInterval + 1u && flew);
    CHECK(arrived_first && arrived_pop && arrived_pop_frame <= wp_frame + kFighterGuardInterval + 1u);
    CHECK(follow_runs > 0 && follow_ok == follow_runs);
    CHECK(station_pop && new_order);
    CHECK(loop && limit);
    CHECK(prepop && kept_near && leash_pop);
    CHECK(hidden_pop);
    CHECK(c3);
}

// ---------------------------------------------------------------- determinism
static CrcLog mixed_run(const ParallelFor& pf) {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 96);
    for (int32_t i = 0; i < 24; ++i)
        CHECK(sys.add_unit(unit(100 + i, cell_center(5 + (i % 6) * 2, 5 + (i / 6) * 2), 0, 1.0f, 6.0f, LocomotorKind::Default)));
    UnitDesc jp = unit(10, cell_center(20, 20), 0, 2.0f, 6.0f, LocomotorKind::Jetpack);
    jp.hover_offset = 25.0f;
    jp.has_pre_turn_anim = true;
    CHECK(sys.add_unit(jp));
    UnitDesc fl = unit(11, cell_center(22, 20), 6, 3.0f, 10.0f, LocomotorKind::Flying);
    fl.hover_offset = 50.0f;
    CHECK(sys.add_unit(fl));
    UnitDesc bk = unit(12, cell_center(24, 20), 2, 2.0f, 8.0f, LocomotorKind::Bike);
    CHECK(sys.add_unit(bk));
    UnitDesc bm = unit(13, cell_center(5, 40), 17, 4.0f, 10.0f, LocomotorKind::LandBomber);
    bm.prepare_strafe_height = 100.0f;
    CHECK(sys.add_unit(bm));
    sys.set_threat_fn([](int32_t, V2 p) { return distance(p, cell_center(45, 45)) < 200.0f ? 30.0f : 0.0f; });
    CrcRegistry reg;
    sys.register_crc(reg, "movement");
    CrcLog log;
    log.systems = reg.names();
    SimRng rng(2024);
    MoveOrder o;
    o.units = sys.unit_ids();
    o.destination = cell_center(70, 70);
    o.threat_threshold = 10.0f;
    sys.move(o, rng, 0);
    for (uint32_t f = 0; f < 900; ++f) {
        sys.tick(f, rng, pf);
        log.records.push_back(reg.capture(f));
    }
    return log;
}

// Golden digest of the final record (MSVC and clang must both produce it). S7 collision layer (2026-10-29): moved (the
// land hash appends the collision objects; priority-ordered resolution). FIGHTER_FLYING §6 flying (2026-10-29): moved
// (Hermite follower, attitude / altitude steps, D + dir x L loiter; flying units hash pitch, V, path index, target).
// S7 t15 (2026-11-02): moved (the Has_Pre_Turn_Anim jetpack's take-off turn: band rates, one-shot clips, clip-end exit).
// cu8i t02 (2026-11-08): moved (land formations draw the FormationClass ctor int(1, 30); start now + acc + r - 30).
// cu8j t01 / t02 (2026-11-08): compositions at the next coordinator update (draws, start frames, the jetpack swap's draw)
// and the hash's pending composition / adopt state.
static constexpr uint64_t kGoldenMixed = 0x15ed89d5cf5f37eeull;  // s78n t18: land collision records

// ---------------------------------------------------------------- SPACE_AVOIDANCE (fighter avoidance hook rules)
// A wall model: one collision mesh, the square x, z in [-50, 50] of the model plane y = 0 (two triangles).
static AvoidModel wall_model() {
    AvoidModel m;
    m.bound_c = {0.0f, 0.0f, 0.0f};
    m.bound_h = {50.0f, 1.0f, 50.0f};
    AvoidModel::Mesh mesh;
    mesh.lo = {-50.0f, 0.0f, -50.0f};
    mesh.hi = {50.0f, 0.0f, 50.0f};
    mesh.tris = {{-50.0f, 0.0f, -50.0f}, {50.0f, 0.0f, -50.0f}, {50.0f, 0.0f, 50.0f},
                 {-50.0f, 0.0f, -50.0f}, {50.0f, 0.0f, 50.0f}, {-50.0f, 0.0f, 50.0f}};
    m.meshes.push_back(mesh);
    return m;
}

static void test_space_avoid() {
    // §3.1 world box: no model -> [P, P + 0.1]; else the collision bound through the transform (facing +y, scale 2:
    // model x -> world -x, model y -> world -y).
    AvoidObstacle o;
    o.pos = {10.0f, 20.0f, 30.0f};
    avoid_place(o);
    CHECK(near(o.box_c.x, 10.05f, 1e-5f) && near(o.box_h.z, 0.05f, 1e-5f));
    AvoidModel bm;
    bm.bound_c = {1.0f, 2.0f, 3.0f};
    bm.bound_h = {10.0f, 20.0f, 5.0f};
    o.model = &bm;
    o.facing = {0.0f, 3.0f};
    o.scale = 2.0f;
    avoid_place(o);
    CHECK((o.facing == V2{0.0f, 1.0f}));
    CHECK(o.box_c.x == 8.0f && o.box_c.y == 16.0f && o.box_c.z == 36.0f && o.box_h.x == 20.0f && o.box_h.y == 40.0f &&
          o.box_h.z == 10.0f);
    const F3 w = avoid_to_world(o, {1.0f, 2.0f, 3.0f}), back = avoid_to_model(o, w);
    CHECK(w.x == 8.0f && w.y == 16.0f && w.z == 36.0f && back.x == 1.0f && back.y == 2.0f && back.z == 3.0f);

    // 0x20c850 slab test: a start inside latches byte 1; t in [0, t_best) from outside; the other axes inclusive.
    bool inside = false;
    const F3 lo{-5.0f, -5.0f, -5.0f}, hi{5.0f, 5.0f, 5.0f};
    CHECK(avoid_segment_box({0.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, lo, hi, 1.0f, inside) && inside);
    inside = false;
    CHECK(avoid_segment_box({-10.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, lo, hi, 1.0f, inside) && !inside);
    CHECK(avoid_segment_box({-10.0f, 5.0f, 0.0f}, {20.0f, 0.0f, 0.0f}, lo, hi, 1.0f, inside));      // edge: inclusive
    CHECK(!avoid_segment_box({-10.0f, 5.1f, 0.0f}, {20.0f, 0.0f, 0.0f}, lo, hi, 1.0f, inside));     // beside
    CHECK(!avoid_segment_box({-10.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, lo, hi, 1.0f, inside));      // t = 1: short
    CHECK(!avoid_segment_box({-10.0f, 0.0f, 0.0f}, {-20.0f, 0.0f, 0.0f}, lo, hi, 1.0f, inside));    // behind
    CHECK(!inside);
    // 0x20c410 two-sided triangle test: 0 < t < 1, |det| >= 1e-4.
    const F3 ta{0.0f, -10.0f, -10.0f}, tb{0.0f, 10.0f, -10.0f}, tc{0.0f, 0.0f, 10.0f};
    CHECK(avoid_segment_triangle({-5.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, ta, tb, tc, 1.0f));
    CHECK(avoid_segment_triangle({5.0f, 0.0f, 0.0f}, {-10.0f, 0.0f, 0.0f}, ta, tb, tc, 1.0f));     // back face
    CHECK(!avoid_segment_triangle({0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, ta, tb, tc, 1.0f));     // t = 0
    CHECK(!avoid_segment_triangle({-5.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, ta, tb, tc, 1.0f));     // short of it
    CHECK(!avoid_segment_triangle({-5.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}, ta, tb, tc, 1.0f));    // parallel
    CHECK(!avoid_segment_triangle({-5.0f, 15.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, ta, tb, tc, 1.0f));   // outside
    CHECK(!avoid_segment_triangle({-5.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, ta, tb, tc, 1.0f));     // zero length

    // Precise test: a wall at x = 100 (facing +x: model y = 0 is the world plane x = 100).
    const AvoidModel wall = wall_model();
    AvoidObstacle wo;
    wo.id = 2;
    wo.pos = {100.0f, 0.0f, 0.0f};
    wo.model = &wall;
    avoid_place(wo);
    CHECK(wo.box_h.x == 1.0f && wo.box_h.y == 50.0f && wo.box_h.z == 50.0f);
    inside = false;
    CHECK(avoid_precise(wo, {80.0f, 0.0f, 0.0f}, {120.0f, 0.0f, 0.0f}, inside) && !inside);
    CHECK(!avoid_precise(wo, {80.0f, 60.0f, 0.0f}, {120.0f, 60.0f, 0.0f}, inside));  // past the wall's edge
    CHECK(!avoid_precise(wo, {80.0f, 0.0f, 0.0f}, {90.0f, 0.0f, 0.0f}, inside));     // short of it
    CHECK(!avoid_precise(wo, {80.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}, inside));     // stopped: never a hit
    AvoidObstacle off = wo;
    off.collide = false;  // contained: the mesh path is off
    CHECK(!avoid_precise(off, {80.0f, 0.0f, 0.0f}, {120.0f, 0.0f, 0.0f}, inside));
    AvoidModel shield = wall;
    shield.meshes[0].shield = true;
    AvoidObstacle so = wo;
    so.model = &shield;
    CHECK(avoid_precise(so, {80.0f, 0.0f, 0.0f}, {120.0f, 0.0f, 0.0f}, inside));  // SHIELD mesh without the behaviour
    so.skip_shield = true;
    CHECK(!avoid_precise(so, {80.0f, 0.0f, 0.0f}, {120.0f, 0.0f, 0.0f}, inside));
    AvoidObstacle none = wo;
    none.model = nullptr;
    CHECK(!avoid_precise(none, {80.0f, 0.0f, 0.0f}, {120.0f, 0.0f, 0.0f}, inside));
    CHECK(!inside);

    // refixf t07: the projectile form (nearest hit, masks). A SHIELD wall in front of the hull wall: model y = 10 is
    // world x = 90 (t 0.25 on 80 -> 120), the hull at t 0.5.
    {
        AvoidModel two_walls = wall_model();
        AvoidModel::Mesh sh = two_walls.meshes[0];
        for (F3& p : sh.tris) p.y = 10.0f;
        sh.lo.y = sh.hi.y = 10.0f;
        sh.shield = true;
        sh.name = "SHIELD";
        two_walls.meshes.push_back(sh);
        AvoidObstacle po = wo;
        po.model = &two_walls;
        po.skip_shield = true;  // the shield behaviour: the SHIELD mesh carries the shield mask
        const F3 a{80.0f, 0.0f, 0.0f}, b{120.0f, 0.0f, 0.0f};
        AvoidSegmentHit h;
        CHECK(avoid_precise_segment(po, a, b, 7u, 2u, h) && h.mesh == 1 && near(h.t, 0.25f, 1e-6f));  // shield up
        h = {};
        CHECK(avoid_precise_segment(po, a, b, 5u, 2u, h) && h.mesh == 0 && near(h.t, 0.5f, 1e-6f));   // no shield damage
        h = {};
        CHECK(avoid_precise_segment(po, a, b, 7u, 0u, h) && h.mesh == 0);                               // shield down
        po.skip_shield = false;  // no shield behaviour: the SHIELD mesh keeps mask 1
        h = {};
        CHECK(avoid_precise_segment(po, a, b, 5u, 0u, h) && h.mesh == 1);
        h = {};
        CHECK(!avoid_precise_segment(po, a, {85.0f, 0.0f, 0.0f}, 7u, 2u, h) && h.mesh == -1 && h.t == 1.0f);  // short
        po.collide = false;
        CHECK(!avoid_precise_segment(po, a, b, 7u, 2u, h));
        po.model = nullptr;
        CHECK(!avoid_precise_segment(po, a, b, 7u, 2u, h));
        AvoidModel cbox;
        cbox.has_collision_box = true;
        cbox.cbox_h = {30.0f, 30.0f, 30.0f};
        po.model = &cbox;  // COLLISIONBOX: the box path even with the collide switch off; a start inside is t 0
        CHECK(avoid_precise_segment(po, a, b, 5u, 0u, h) && h.t == 0.0f && h.mesh == -1);
    }

    // S8 t13 (MOVEMENT §1.3 "Ray mesh set"): the Fine_Tune stamping ray (vf +0x80 mode 0, mask 1) - any collision mesh
    // whose bone is visible; COLLISIONBOX and the collide switch play no part.
    {
        AvoidModel floor;
        AvoidModel::Mesh m;
        m.lo = {-10.0f, -10.0f, 0.0f};
        m.hi = {10.0f, 10.0f, 0.0f};
        m.tris = {{-10.0f, -10.0f, 0.0f}, {10.0f, -10.0f, 0.0f}, {10.0f, 10.0f, 0.0f},
                  {-10.0f, -10.0f, 0.0f}, {10.0f, 10.0f, 0.0f}, {-10.0f, 10.0f, 0.0f}};
        m.name = "COLLISION_ALT0";  // hidden in the file (the builder keeps it: collision flag), bone visible
        floor.meshes.push_back(m);
        AvoidObstacle fo;
        fo.pos = {200.0f, 200.0f, 5.0f};
        fo.model = &floor;
        avoid_place(fo);
        auto ray = [&](float x, float y) { return avoid_ray_any(fo, {x, y, -1000.0f}, {x, y, 1000.0f}); };
        CHECK(ray(200.0f, 200.0f) && ray(209.0f, 191.0f) && !ray(215.0f, 200.0f));
        fo.collide = false;  // contained / collide switch off: the ray still hits
        CHECK(ray(200.0f, 200.0f));
        floor.has_collision_box = true;  // a COLLISIONBOX off the segment changes nothing (the precise test would miss)
        floor.cbox_c = {500.0f, 500.0f, 0.0f};
        floor.cbox_h = {1.0f, 1.0f, 1.0f};
        AvoidSegmentHit h;
        CHECK(ray(200.0f, 200.0f) && !avoid_precise_segment(fo, {200.0f, 200.0f, -1000.0f}, {200.0f, 200.0f, 1000.0f}, 1u, 0u, h));
        floor.meshes[0].bone_visible = false;  // a mesh whose bone is hidden at run time is skipped
        CHECK(!ray(200.0f, 200.0f));
        AvoidModel::Mesh m2 = m;  // a second, visible mesh (model x + 30 = world y + 30) decides alone
        for (F3& p : m2.tris) p.x += 30.0f;
        m2.lo.x += 30.0f;
        m2.hi.x += 30.0f;
        floor.meshes.push_back(m2);
        CHECK(!ray(200.0f, 200.0f) && ray(200.0f, 230.0f));
        floor.meshes[1].shield = true;  // SHIELD mesh: skipped only for a type with the shield behaviour
        CHECK(ray(200.0f, 230.0f));
        fo.skip_shield = true;
        CHECK(!ray(200.0f, 230.0f));
        fo.model = nullptr;
        CHECK(!avoid_ray_any(fo, {200.0f, 200.0f, -1000.0f}, {200.0f, 200.0f, 1000.0f}));
    }

    // The query: overlap (inclusive) with the box around the fighter, then the first hit decides.
    F3 hit;
    const std::vector<AvoidObstacle> one{wo};
    CHECK(space_avoid(one, {99.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, {99.0f, 0.0f, 0.0f}, {140.0f, 0.0f, 0.0f}, hit) &&
          hit.x == 100.0f && hit.y == 0.0f);
    CHECK(space_avoid(one, {95.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, {95.0f, 0.0f, 0.0f}, {140.0f, 0.0f, 0.0f}, hit));  // touching
    CHECK(!space_avoid(one, {94.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, {94.0f, 0.0f, 0.0f}, {140.0f, 0.0f, 0.0f}, hit));  // out of reach
    // A COLLISIONBOX member whose box holds the segment start latches byte 1: it and every later hit are skipped.
    AvoidModel cb;
    cb.has_collision_box = true;
    cb.cbox_c = {0.0f, 0.0f, 0.0f};
    cb.cbox_h = {10.0f, 10.0f, 10.0f};
    cb.bound_h = {10.0f, 10.0f, 10.0f};
    AvoidObstacle bo;
    bo.id = 1;
    bo.pos = {98.0f, 0.0f, 0.0f};
    bo.model = &cb;
    avoid_place(bo);
    std::vector<AvoidObstacle> two{bo, wo};
    CHECK(!space_avoid(two, {99.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, {99.0f, 0.0f, 0.0f}, {140.0f, 0.0f, 0.0f}, hit));
    bo.id = 3;
    two = {wo, bo};  // the wall first: it decides
    hit = {};
    CHECK(space_avoid(two, {99.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, {99.0f, 0.0f, 0.0f}, {140.0f, 0.0f, 0.0f}, hit) &&
          hit.x == 100.0f);
    // From outside the COLLISIONBOX the box path is an ordinary hit (t in [0, 1)).
    bo.pos = {130.0f, 0.0f, 0.0f};
    avoid_place(bo);
    const std::vector<AvoidObstacle> box_only{bo};
    CHECK(space_avoid(box_only, {119.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, {119.0f, 0.0f, 0.0f}, {160.0f, 0.0f, 0.0f}, hit) &&
          hit.x == 130.0f);

    // A fighter flying at the wall through MovementSystem's avoid (FIGHTER_FLYING §3.5): the hook steers it away and
    // sets the 15-frame hold; without the hook it never does.
    auto fly = [&](bool hook) {
        MovementConfig cfg;
        cfg.mode = MoveMode::Space;
        MovementSystem sys(cfg);
        sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        UnitDesc f = unit(1, {-600.0f, 0.0f}, -1, 4.0f, 10.0f, LocomotorKind::Fighter);
        f.space_layer = kLayerFighter;
        f.space_layer_type = 0;
        CHECK(sys.add_unit(f));
        AvoidObstacle wall0;
        wall0.id = 50;
        wall0.pos = {0.0f, 0.0f, 0.0f};
        wall0.model = &wall;
        avoid_place(wall0);
        const std::vector<AvoidObstacle> members{wall0};
        if (hook)
            sys.set_fighter_avoid_fn([&members](ObjectId, F3 c, F3 h, F3 a, F3 b, F3& out) { return space_avoid(members, c, h, a, b, out); });
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = {600.0f, 0.0f};
        sys.move(o, rng, 0);
        uint32_t holds = 0;
        for (uint32_t fr = 0; fr < 400; ++fr) {
            sys.tick(fr, rng);
            if (const FighterState* fs = sys.fighter_state(1); fs && fs->hold == kFighterAvoidHold) ++holds;
        }
        return holds;
    };
    const uint32_t h0 = fly(false), h1 = fly(true);
    std::printf("space avoid: avoidance frames %u without the hook, %u with it\n", h0, h1);
    CHECK(h0 == 0 && h1 > 0);
}

// ---------------------------------------------------------------- OI abilities: Berserker speed override (+0x2a0)
static void test_speed_override() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default)));
    const float base = sys.view(1).max_speed;
    CHECK(base > 0.0f && sys.speed_override(1) == -1.0f);
    StateHasher h0;
    sys.hash_speed(h0);
    CHECK(sys.set_speed_override(1, 6.0f) && sys.speed_override(1) == 6.0f);
    CHECK(near(sys.view(1).max_speed, base * 3.0f, 1e-4f));  // replaces Max_Speed (2 -> 6), mode multiplier kept
    CHECK(sys.set_speed_factor(1, 0.5f) && near(sys.view(1).max_speed, base * 1.5f, 1e-4f));  // the factor still applies
    StateHasher h1;
    sys.hash_speed(h1);
    CHECK(sys.set_speed_factor(1, 1.0f) && sys.set_speed_override(1, -1.0f) && sys.speed_override(1) == -1.0f);
    CHECK(sys.view(1).max_speed == base);
    StateHasher h2;
    sys.hash_speed(h2);
    CHECK(h0.digest() == h2.digest() && h1.digest() != h0.digest());  // hashed only while set
    CHECK(!sys.set_speed_override(99, 1.0f));
}

// s78k t01 (MOVEMENT §3.5.1, §6.1): the Walk locomotor's arrival turn to a move's final facing, and the flag-0x20
// straight path.
static void test_arrival_turn() {
    // One unit walking east (yaw 0) to cell (20, 10) with a final facing; returns the yaws after the arrival frame.
    struct Run {
        bool arrived = false;
        float arrival_yaw = 0.0f;
        std::vector<float> after;  // yaw per frame after the arrival frame
    };
    auto run = [](float final_facing, float stopped_rate, int32_t directions, int stop_after = -1, bool reorder = false) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc d = unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default);
        d.stopped_turn_rate = stopped_rate;
        d.movement_directions = directions;
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        o.final_facing_deg = final_facing;
        sys.move(o, rng, 0);
        Run r;
        uint32_t f = 1;
        for (; f < 400 && !r.arrived; ++f) {
            sys.tick(f, rng);
            if (sys.view(1).arrived) {
                r.arrived = true;
                r.arrival_yaw = sys.view(1).yaw;
            }
        }
        for (int k = 0; k < 40; ++k, ++f) {
            if (k == stop_after) {
                const ObjectId ids[] = {1};
                sys.stop(ids);
            }
            if (reorder && k == stop_after) {
                MoveOrder b;  // a new order without a final facing replaces the turn
                b.units = {1};
                b.destination = cell_center(10, 10);
                sys.move(b, rng, f);
            }
            sys.tick(f, rng);
            r.after.push_back(sys.view(1).yaw);
        }
        return r;
    };
    const float rate = 6.0f * MovementConstants{}.land_speed_multiplier;  // Max_Rate_Of_Turn x the land multiplier
    {  // 90 degrees off: rate-limited turn landing exactly on 90
        const Run r = run(90.0f, 0.0f, 1);
        CHECK(r.arrived && std::fabs(r.arrival_yaw) < 1e-3f);
        float prev = r.arrival_yaw;
        size_t steps = 0;
        for (const float y : r.after) {
            CHECK(std::fabs(wrap_deg(y - prev)) <= rate + 1e-4f);
            if (y != prev) ++steps;
            prev = y;
        }
        CHECK(r.after.back() == 90.0f);
        CHECK(steps == static_cast<size_t>(std::ceil(90.0f / rate)));
        std::printf("arrival turn: 90 deg in %zu frames at %.2f deg/frame\n", steps, static_cast<double>(rate));
    }
    {  // Stopped_Rate_Of_Turn 30 replaces the Max_Rate_Of_Turn rate
        const Run r = run(90.0f, 30.0f, 1);
        CHECK(r.after.size() >= 3 && r.after[0] == 30.0f && r.after[1] == 60.0f && r.after[2] == 90.0f && r.after.back() == 90.0f);
    }
    {  // within 10 degrees: no turn
        const Run r = run(8.0f, 0.0f, 1);
        CHECK(r.arrived && r.after.back() == r.arrival_yaw);
    }
    {  // no final facing: the arrival heading is kept
        const Run r = run(kNoFacing, 0.0f, 1);
        CHECK(r.arrived && r.after.back() == r.arrival_yaw);
    }
    {  // Movement_Directions_Count 2: of {180, 0} the one closest to yaw 0 -> no turn; <= 0: the current yaw
        CHECK(run(180.0f, 30.0f, 2).after.back() == 0.0f);
        CHECK(run(150.0f, 30.0f, 2).after.back() == -30.0f);  // {150, -30}: -30 is 30 off -> turn to -30
        CHECK(run(90.0f, 30.0f, 0).after.back() == 0.0f);
    }
    {  // a stop during the turn keeps the yaw reached
        const Run r = run(90.0f, 30.0f, 1, 1);
        CHECK(r.after[0] == 30.0f && r.after.back() == 30.0f);
    }
    {  // s78l t02: a stop drops the running turn -> arrival_turn_ended on the next tick only
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc d = unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default);
        d.stopped_turn_rate = 5.0f;
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        o.final_facing_deg = 90.0f;
        sys.move(o, rng, 0);
        uint32_t f = 1;
        for (; f < 400 && !sys.view(1).arrived; ++f) sys.tick(f, rng);
        sys.tick(f++, rng);
        CHECK(sys.view(1).arrival_turn && !sys.view(1).arrival_turn_ended);
        const ObjectId ids[] = {1};
        sys.stop(ids);
        sys.tick(f++, rng);
        CHECK(!sys.view(1).arrival_turn && sys.view(1).arrival_turn_ended);
        sys.tick(f++, rng);
        CHECK(!sys.view(1).arrival_turn_ended);
    }
    {  // a new order replaces the turn: walking back west ends on the travel heading
        const Run r = run(90.0f, 30.0f, 1, 1, true);
        CHECK(r.after[0] == 30.0f && std::fabs(std::fabs(r.after.back()) - 180.0f) < 1e-3f);
    }
}

static void test_forced_path() {
    // A wall across x = 15 (the whole height): a normal move stops at the nearest reachable spot on the near side; a
    // flag-0x20 move walks straight through to the destination.
    for (const bool forced : {false, true}) {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(40, 40);
        for (int32_t y = 0; y < 40; ++y) g.set_base(15, y, kPassImpassable);
        sys.set_grid(std::move(g));
        CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default)));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 12);
        if (forced) o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 0);
        bool arrived = false;
        for (uint32_t f = 1; f < 600 && !arrived; ++f) {
            sys.tick(f, rng);
            arrived = sys.view(1).arrived;
        }
        const UnitView v = sys.view(1);
        std::printf("forced path %d: arrived %d at (%.1f, %.1f), failed %d\n", forced ? 1 : 0, arrived ? 1 : 0,
                    static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), v.failed ? 1 : 0);
        if (forced) {
            CHECK(arrived && v.pos.x == cell_center(20, 12).x && v.pos.y == cell_center(20, 12).y);
        } else {
            CHECK(v.pos.x < 15.0f * kCellSize);  // the wall closes the map: the route ends on the near side
        }
    }
}

// s78k t03 (MOVEMENT §3.4 / §3.7): the team locomotor cap - two team units of different Max_Speed, moved one at a time
// (no formation): each walks at most at min(Max_Speed) x mult x the container factor; leaving the team removes it.
static void test_team_cap() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 1.0f, 8.0f, LocomotorKind::Default)));
    CHECK(sys.add_unit(unit(2, cell_center(10, 30), 2, 3.0f, 8.0f, LocomotorKind::Default)));
    const float mult = MovementConstants{}.land_speed_multiplier;
    CHECK(sys.set_team(1, 50, 1.5f) && sys.set_team(2, 50, 1.5f) && !sys.set_team(9, 50, 1.0f));
    SimRng rng(3);
    MoveOrder o;
    o.units = {2};
    o.destination = cell_center(60, 30);
    sys.move(o, rng, 100);  // cu8j t01 [M]: after frame 60, so the plan is not gated to frame 61
    float top = 0.0f;
    for (uint32_t f = 101; f < 140; ++f) {
        sys.tick(f, rng);
        top = std::max(top, sys.view(2).speed);
    }
    const float cap = (1.0f * mult) * 1.5f;
    CHECK(sys.view(1).team_speed == cap && sys.view(2).team_speed == cap && top == cap);
    CHECK(sys.set_team(2, -1, 1.0f));
    for (uint32_t f = 140; f < 160; ++f) {
        sys.tick(f, rng);
        top = std::max(top, sys.view(2).speed);
    }
    CHECK(sys.view(2).team_speed > 1.0e30f && top == 3.0f * mult);
    std::printf("team cap: %.3f in the team, %.3f alone\n", static_cast<double>(cap), static_cast<double>(top));
}

// s78l t04 (FIGHTER_FLYING §4.11 item 4, MOVEMENT §3.4, TACTICAL_COMBAT §4.4 "No mode gate"): a space squadron's
// formation speed (F +0x21c, 0x41b2a0) is the container's effective max speed - the minimum raw Max_Speed of its fighters
// x the space multiplier x the container factor set_team gives it (the squad Mod_Multiplier SPEED) - and not the fighters'
// own speed factors. A lone fighter's formation keeps its own effective max speed (own factor included).
static void test_squad_team_speed() {
    MovementConfig cfg;
    cfg.mode = MoveMode::Space;
    const float mult = MovementConstants{}.space_speed_multiplier;
    // Guarding a fast carrier (2 x its speed > any fighter speed below, so the follow cap never bites): the formation's
    // speed after `frames` frames with the given container factor / fighter-1 own factor; `squad` false = fighter 1 alone.
    const auto run = [&](bool squad, float team_factor, float own_factor, uint32_t frames) {
        MovementSystem sys(cfg);
        sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        const float speeds[3] = {10.0f, 8.0f, 12.0f};  // raw Max_Speed; fighter 2 is the slowest
        FighterSquadDesc sq;
        sq.id = 500;
        for (ObjectId i = 1; i <= 3; ++i) {
            const float side = static_cast<float>(i - 2) * 30.0f;
            UnitDesc f = unit(i, {-2000.0f, 1500.0f + side}, -1, speeds[i - 1], 10.0f, LocomotorKind::Fighter);
            f.space_layer = kLayerFighter;
            f.space_layer_type = 0;
            CHECK(sys.add_unit(f));
            sq.members.push_back(i);
            sq.slots.push_back({0.0f, side, 0.0f});
        }
        if (squad) {
            CHECK(sys.set_fighter_squad(sq));
            for (ObjectId i = 1; i <= 3; ++i) CHECK(sys.set_team(i, 500, team_factor));
        }
        CHECK(sys.set_speed_factor(1, own_factor));
        UnitDesc c = unit(50, {-2000.0f, 0.0f}, -1, 20.0f, 60.0f, LocomotorKind::Default);
        c.space_layer_type = 2;
        CHECK(sys.add_unit(c));
        SimRng rng(5);
        MoveOrder g;
        g.units = squad ? std::vector<ObjectId>{1, 2, 3} : std::vector<ObjectId>{1};
        g.destination = {-2000.0f, 0.0f};
        g.flags = kMoveFlagGuardMove;
        g.goal_object = 50;
        sys.move(g, rng, 0);
        MoveOrder mc;
        mc.units = {50};
        mc.destination = {3000.0f, 0.0f};
        sys.move(mc, rng, 0);
        float speed = -1.0f;
        for (uint32_t fr = 0; fr < frames; ++fr) {
            sys.tick(fr, rng);
            if (const FighterFormation* f = sys.fighter_formation(squad ? 500 : 1)) speed = f->speed;
        }
        CHECK(2.0f * sys.view(50).max_speed > 12.0f * mult * 2.0f);  // the follow cap stays above every value below
        return std::pair<float, float>{speed, sys.view(1).max_speed};
    };
    const float slow = 8.0f * mult;
    CHECK(run(true, 1.0f, 1.0f, 40).first == slow);         // squadron, factor 1: the slowest fighter's raw speed x mult
    CHECK(run(true, 1.5f, 1.0f, 40).first == slow * 1.5f);  // container factor 1.5 (squad SPRINT)
    CHECK(run(true, 0.5f, 1.0f, 40).first == slow * 0.5f);  // container factor 0.5
    {  // a fighter's own factor (0.25 on fighter 1, which would make it the slowest) is not read for a squadron
        const auto [s, own] = run(true, 1.0f, 0.25f, 40);
        CHECK(s == slow && own == 10.0f * mult * 0.25f);
    }
    {  // a lone fighter: its own effective max speed, own factor included; set_team is not involved
        const auto [s, own] = run(false, 1.0f, 0.5f, 40);
        CHECK(s == own && own == 10.0f * mult * 0.5f);
    }
    std::printf("squad team speed: slowest %.3f, x1.5 %.3f, x0.5 %.3f\n", static_cast<double>(slow), static_cast<double>(slow * 1.5f),
                static_cast<double>(slow * 0.5f));
}

// s78l t01 (MOVEMENT §3.4 "Team cap, exact" / "Full team formula", TACTICAL_COMBAT §4.4 "Team move speed"): the team
// speed is min(container Max_Speed, min member raw Max_Speed) x mult x the container factor, and a member walks AT it -
// its own speed factor is not multiplied in (no double count); a stunned member (factor 0) stays put.
static void test_team_speed_exact() {
    const float mult = MovementConstants{}.land_speed_multiplier;
    // Two members (raw 1.0 and 1.2) of a Stormtrooper_Team-like container (Max_Speed 0.8); member 2 walks alone.
    const auto run = [&](float container_max, float factor, float own2) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        CHECK(sys.add_unit(unit(1, cell_center(10, 10), 2, 1.0f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.add_unit(unit(2, cell_center(10, 30), 2, 1.2f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.set_team(1, 50, factor, container_max) && sys.set_team(2, 50, factor, container_max));
        CHECK(sys.set_speed_factor(2, own2));
        SimRng rng(3);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(60, 30);
        sys.move(o, rng, 100);  // cu8j t01 [M]: after frame 60, so the plan is not gated to frame 61
        float top = 0.0f;
        for (uint32_t f = 101; f < 140; ++f) {
            sys.tick(f, rng);
            top = std::max(top, sys.view(2).speed);
        }
        return std::pair<float, float>{sys.view(2).team_speed, top};
    };
    {  // the container's 0.8 is below both members: it sets the team speed
        const auto [ts, top] = run(0.8f, 1.0f, 1.0f);
        CHECK(ts == 0.8f * mult && top == ts);
    }
    {  // a container faster than its members (Sandpeople_Team 1.0 over 0.5 members): the slowest member
        const auto [ts, top] = run(1.5f, 1.0f, 1.0f);
        CHECK(ts == 1.0f * mult && top == ts);
    }
    {  // container unknown (-1): the members' minimum alone (the s78k behaviour)
        const auto [ts, top] = run(-1.0f, 1.0f, 1.0f);
        CHECK(ts == 1.0f * mult && top == ts);
    }
    {  // the container factor applies once; the member's own factor 0.5 does not enter, nor caps it
        const auto [ts, top] = run(0.8f, 1.25f, 0.5f);
        CHECK(ts == 0.8f * mult * 1.25f && top == ts);
    }
    {  // a factor above the member's own top speed: the team speed still rules (the container moves the team)
        const auto [ts, top] = run(-1.0f, 1.5f, 1.0f);
        CHECK(ts == 1.0f * mult * 1.5f && top == ts && top > 1.2f * mult);
    }
    {  // a stunned member (own factor 0: its locomotor is suspended) does not move
        const auto [ts, top] = run(0.8f, 1.0f, 0.0f);
        CHECK(ts == 0.8f * mult && top == 0.0f);
    }
    {  // space squadron: the container's Max_Speed is in the formation speed minimum too (0x41b2a0 -> 0x370f00)
        MovementConfig cfg;
        cfg.mode = MoveMode::Space;
        const float sm = MovementConstants{}.space_speed_multiplier;
        MovementSystem sys(cfg);
        sys.space().set_box({-4000.0f, -4000.0f}, {4000.0f, 4000.0f});
        FighterSquadDesc sq;
        sq.id = 500;
        for (ObjectId i = 1; i <= 2; ++i) {
            const float side = static_cast<float>(i) * 30.0f;
            UnitDesc f = unit(i, {-2000.0f, 1500.0f + side}, -1, 10.0f, 10.0f, LocomotorKind::Fighter);
            f.space_layer = kLayerFighter;
            f.space_layer_type = 0;
            CHECK(sys.add_unit(f));
            sq.members.push_back(i);
            sq.slots.push_back({0.0f, side, 0.0f});
        }
        CHECK(sys.set_fighter_squad(sq));
        for (ObjectId i = 1; i <= 2; ++i) CHECK(sys.set_team(i, 500, 1.0f, 6.0f));
        UnitDesc c = unit(50, {-2000.0f, 0.0f}, -1, 20.0f, 60.0f, LocomotorKind::Default);
        c.space_layer_type = 2;
        CHECK(sys.add_unit(c));
        SimRng rng(5);
        MoveOrder g;
        g.units = {1, 2};
        g.destination = {-2000.0f, 0.0f};
        g.flags = kMoveFlagGuardMove;
        g.goal_object = 50;
        sys.move(g, rng, 0);
        MoveOrder mc;
        mc.units = {50};
        mc.destination = {3000.0f, 0.0f};
        sys.move(mc, rng, 0);
        float speed = -1.0f;
        for (uint32_t fr = 0; fr < 40; ++fr) {
            sys.tick(fr, rng);
            if (const FighterFormation* f = sys.fighter_formation(500)) speed = f->speed;
        }
        CHECK(speed == 6.0f * sm);
    }
    std::printf("team speed exact: container 0.8 -> %.3f\n", static_cast<double>(0.8f * mult));
}

// s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details"): the pure rules.
static void test_arrival_turn_rules() {
    CHECK(arrival_turn_threshold(6.0f, false) == 10.0f && arrival_turn_threshold(6.0f, true) == 6.0f &&
          arrival_turn_threshold(12.0f, true) == 10.0f);
    CHECK(anim_idle_class(0) && anim_idle_class(1) && anim_idle_class(71) && anim_idle_class(77) && anim_idle_class(82) &&
          !anim_idle_class(2) && !anim_idle_class(83) && !anim_idle_class(kAnimNone));
    CHECK(anim_turn_clip(3) && anim_turn_clip(4) && anim_turn_clip(21) && anim_turn_clip(24) && !anim_turn_clip(12) &&
          !anim_turn_clip(13) && !anim_turn_clip(25));
    {
        const TurnAnim q = arrival_turn_band(15.0f, 3.0f, 10.0f, 0);
        CHECK(q.anim == kAnimTurnLeftQuarter && q.rate == 3.0f * 0.25f * 15.0f / 11.25f);
        const TurnAnim h = arrival_turn_band(-30.0f, 3.0f, 10.0f, 0);
        CHECK(h.anim == kAnimTurnRightHalf && h.rate == 1.5f);
        const TurnAnim f = arrival_turn_band(45.0f, 3.0f, 10.0f, 0);
        CHECK(f.anim == kAnimTurnLeft && f.rate == 3.0f);
        const TurnAnim n = arrival_turn_band(9.5f, 3.0f, 10.0f, kAnimTurnLeftHalf);  // |e| < T: clip unchanged, rate S
        CHECK(n.anim == kAnimTurnLeftHalf && n.rate == 3.0f);
        const TurnAnim t = arrival_turn_band(10.0f, 3.0f, 10.0f, 0);  // e >= T
        CHECK(t.anim == kAnimTurnLeftQuarter);
    }
    CHECK(arrival_turn_end_clip(3) == kAnimTurnLeftEnd && arrival_turn_end_clip(12) == kAnimTurnLeftEnd &&
          arrival_turn_end_clip(22) == kAnimTurnLeftEnd && arrival_turn_end_clip(4) == kAnimTurnRightEnd &&
          arrival_turn_end_clip(14) == kAnimTurnRightEnd && arrival_turn_end_clip(23) == kAnimTurnRightEnd &&
          arrival_turn_end_clip(0) == kAnimNone && arrival_turn_end_clip(2) == kAnimNone);
}

// s78l t02 (b): the Has_Pre_Turn_Anim arrival turn - begin clip holds the yaw, band rates from the frame after the
// pick, the end clip, then the pop with the default idle; without clip data the band still sets the rate and the turn
// ends early (|e| < 10 with an idle-class clip); a unit without Has_Pre_Turn_Anim plays TURNL once and lands exactly.
static bool arrival_clips(ObjectId, int32_t anim, uint32_t& n, float& fps) {
    switch (anim) {
        case kAnimTurnLeftBegin:
        case kAnimTurnRightBegin: n = 12; break;  // 12 frames at 15 fps: 0.5 x speed clip frames per sim frame
        case kAnimTurnLeftEnd:
        case kAnimTurnRightEnd: n = 8; break;
        case kAnimTurnLeft:
        case kAnimTurnRight:
        case kAnimTurnLeftHalf:
        case kAnimTurnRightHalf:
        case kAnimTurnLeftQuarter:
        case kAnimTurnRightQuarter: n = 10; break;
        default: return false;
    }
    fps = 15.0f;
    return true;
}

static void test_arrival_turn_clips() {
    struct Frame {
        float yaw = 0.0f;
        bool turning = false;
        bool ended = false;
        std::vector<MovementSystem::TurnClipEvent> ev;
    };
    struct Run {
        float arrival_yaw = 0.0f;
        std::vector<Frame> fr;  // from the frame after the arrival
    };
    const auto run = [](bool pre, bool clips, float stopped, float ras) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        if (clips) sys.set_deploy_clip_fn(arrival_clips);
        UnitDesc d = unit(1, cell_center(10, 10), 2, 2.0f, 8.0f, LocomotorKind::Default);
        d.has_pre_turn_anim = pre;
        d.stopped_turn_rate = stopped;
        d.rotation_animation_speed = ras;
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        o.final_facing_deg = 90.0f;
        sys.move(o, rng, 0);
        Run r;
        uint32_t f = 1;
        for (; f < 400 && !sys.view(1).arrived; ++f) sys.tick(f, rng);
        CHECK(sys.take_turn_clip_events().empty());
        r.arrival_yaw = sys.view(1).yaw;
        for (int k = 0; k < 120; ++k, ++f) {
            sys.tick(f, rng);
            Frame x;
            x.yaw = sys.view(1).yaw;
            x.turning = sys.view(1).arrival_turn;
            x.ended = sys.view(1).arrival_turn_ended;
            x.ev = sys.take_turn_clip_events();
            CHECK(x.ev.size() <= 1);
            r.fr.push_back(x);
        }
        return r;
    };
    {  // with clips: begin (yaw held) -> TURNL band clips -> end clip -> pop + idle
        const Run r = run(true, true, 3.0f, 1.5f);
        CHECK(std::fabs(r.arrival_yaw) < 1e-3f);
        CHECK(!r.fr[0].ev.empty() && r.fr[0].ev[0].anim == kAnimTurnLeftBegin && r.fr[0].ev[0].speed == 2.0f &&
              r.fr[0].ev[0].blend < 0.0f && r.fr[0].turning);
        // Begin clip: 12 frames at 15 fps x speed 2 = 1 clip frame per sim frame -> in its last 2 frames at position 10.
        size_t k = 1;
        while (k < r.fr.size() && r.fr[k].yaw == r.arrival_yaw) ++k;
        CHECK(k == 10);  // frames 0..9 hold the yaw; the turn starts on frame 10
        CHECK(r.fr[k].yaw == 3.0f);  // first turn frame at S = Stopped_Rate_Of_Turn
        CHECK(!r.fr[k].ev.empty() && r.fr[k].ev[0].anim == kAnimTurnLeft);  // e = 87 after the step: TURNL at S
        bool end = false, idle = false, half = false, quarter = false;
        size_t pop = 0;
        for (size_t j = k; j < r.fr.size(); ++j) {
            for (const auto& e : r.fr[j].ev) {
                half = half || e.anim == kAnimTurnLeftHalf;
                quarter = quarter || e.anim == kAnimTurnLeftQuarter;
                end = end || e.anim == kAnimTurnLeftEnd;
                if (e.idle) {
                    idle = true;
                    pop = j;
                }
            }
        }
        CHECK(half && quarter && end && idle);
        CHECK(!r.fr[pop].turning && (pop == 0 || r.fr[pop - 1].turning));
        for (size_t j = 0; j < r.fr.size(); ++j) CHECK(r.fr[j].ended == (j == pop));  // only the pop frame
        CHECK(std::fabs(r.fr.back().yaw - 90.0f) <= 10.0f && !r.fr.back().turning);
        std::printf("arrival turn (clips): begin held %zu frames, popped at +%zu, final yaw %.3f\n", k, pop,
                    static_cast<double>(r.fr.back().yaw));
    }
    {  // Has_Pre_Turn_Anim without clip data: band rates, early end with 0 < |e| < 10, no events
        const Run r = run(true, false, 3.0f, 1.5f);
        CHECK(r.fr[0].yaw == 3.0f && r.fr[1].yaw == 6.0f);  // S (first frame), then TURNL band at S
        for (const Frame& x : r.fr) CHECK(x.ev.empty());
        const float left = std::fabs(wrap_deg(90.0f - r.fr.back().yaw));
        CHECK(left > 0.0f && left < 10.0f && !r.fr.back().turning);
        // in the quarter band the step shrinks with the error (S x 0.25 x e / 11.25)
        bool shrinking = false;
        for (size_t j = 2; j < r.fr.size(); ++j) {
            const float a = r.fr[j - 1].yaw - r.fr[j - 2].yaw, b = r.fr[j].yaw - r.fr[j - 1].yaw;
            if (b > 0.0f && a > 0.0f && b < a && b < 1.5f) shrinking = true;
        }
        CHECK(shrinking);
        std::printf("arrival turn (no clip data): stopped %.3f deg short\n", static_cast<double>(left));
    }
    {  // without Has_Pre_Turn_Anim, with clip data: TURNL once (speed 1, blend |e| / r x 0.03333), exact landing
        const Run r = run(false, true, 30.0f, 0.0f);
        CHECK(r.fr[0].yaw == 30.0f && r.fr[1].yaw == 60.0f && r.fr[2].yaw == 90.0f);
        CHECK(r.fr[0].ev.size() == 1 && r.fr[0].ev[0].anim == kAnimTurnLeft && r.fr[0].ev[0].speed == 1.0f &&
              r.fr[0].ev[0].blend == 90.0f / 30.0f * 0.03333f);
        size_t plays = 0;
        for (const Frame& x : r.fr)
            for (const auto& e : x.ev) plays += e.idle ? 0u : 1u;
        CHECK(plays == 1);
        size_t ends = 0;
        for (const Frame& x : r.fr) ends += x.ended ? 1u : 0u;
        CHECK(ends == 1);
    }
    {  // TURNL blend cap: |e0| / r x 0.03333 = 90 / 10 x 0.03333 = 0.3 -> kArrivalTurnBlendCapPort
        const Run r = run(false, true, 10.0f, 0.0f);
        CHECK(r.fr[0].ev.size() == 1 && r.fr[0].ev[0].anim == kAnimTurnLeft && r.fr[0].ev[0].blend == kArrivalTurnBlendCapPort);
        // TURNL (10 frames at 0.5 per sim frame) holds the state until its last 2 frames: the pop waits for the clip.
        CHECK(r.fr.back().yaw == 90.0f && !r.fr.back().turning);
    }
}

// s78l t02 (a, step 1): End Move brought to a stop short of the last point (here: a speed override of 0 holds it) on the
// last segment within 5 units of the last point, along the segment, counts as arrived where it stands; further out the
// move stays open.
static void test_arrival_close() {
    const auto run = [](float stop_within) {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(60, 60);
        sys.set_grid(std::move(g));
        UnitDesc d = unit(1, cell_center(10, 10), 2, 1.0f, 6.0f, LocomotorKind::Default);
        d.decel = 0.02f;  // a long End Move
        CHECK(sys.add_unit(d));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(20, 10);
        sys.move(o, rng, 0);
        bool held = false;
        uint32_t f = 1;
        for (; f < 400 && !sys.view(1).arrived; ++f) {
            sys.tick(f, rng);
            if (!held && sys.view(1).has_goal && sys.view(1).goal.x - sys.view(1).pos.x < stop_within) {
                CHECK(sys.set_speed_override(1, 0.0f));
                held = true;
            }
            if (held && f > 300) break;
        }
        const UnitView v = sys.view(1);
        CHECK(held);
        return std::pair<bool, float>{v.arrived, v.goal.x - v.pos.x};
    };
    {
        const auto [arrived, short_by] = run(4.0f);
        CHECK(arrived && short_by > 0.0f && short_by < 4.0f);
        std::printf("arrival close: arrived %.3f units short of the last point\n", static_cast<double>(short_by));
    }
    {
        const auto [arrived, short_by] = run(9.0f);
        CHECK(!arrived && short_by >= 5.0f);
    }
}

// s78l t02 (c, MOVEMENT §6.1 "Forced-path destination check and fallback"): a flag-0x20 destination in a blocked cell is
// placed by the ring search (the first passable sample from the unit's yaw, counter-clockwise); a destination off the grid
// or with no passable sample in 20 rings gives no path and no move.
static void test_forced_destination() {
    const auto run = [](V2 dest, bool block_ring) {
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(40, 40);
        g.set_base(20, 12, kPassImpassable);  // the destination's cell
        if (block_ring)
            for (int32_t y = 0; y < 40; ++y)
                for (int32_t x = 12; x < 40; ++x) g.set_base(x, y, kPassImpassable);
        sys.set_grid(std::move(g));
        CHECK(sys.add_unit(unit(1, cell_center(5, 12), 2, 2.0f, 8.0f, LocomotorKind::Default)));
        SimRng rng(3);
        MoveOrder o;
        o.units = {1};
        o.destination = dest;
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 0);
        bool arrived = false;
        for (uint32_t f = 1; f < 600 && !arrived && !sys.view(1).failed; ++f) {
            sys.tick(f, rng);
            arrived = sys.view(1).arrived;
        }
        return sys.view(1);
    };
    const V2 d = cell_center(20, 12);
    {  // blocked cell: ring search from yaw 0 (east); the first passable sample leaves the cell [390, 410) x [230, 250)
        const UnitView v = run(d, false);
        CHECK(v.arrived && !v.failed);
        CHECK(world_to_cell(v.pos.x) != 20 || world_to_cell(v.pos.y) != 12);
        CHECK(distance(v.pos, d) > 10.0f && distance(v.pos, d) < 30.0f);
        CHECK(v.goal.x == v.pos.x && v.goal.y == v.pos.y);
        // first ring that leaves the cell: R = 13 (0, 1, 2.8, 5.4, 8.8, 13), sample 0 at yaw 0 -> (d.x + 13, d.y)
        CHECK(std::fabs(v.pos.x - (d.x + 13.0f)) < 1e-3f && std::fabs(v.pos.y - d.y) < 1e-3f);
        std::printf("forced destination: placed at (%.2f, %.2f), %.2f from the blocked point\n", static_cast<double>(v.pos.x),
                    static_cast<double>(v.pos.y), static_cast<double>(distance(v.pos, d)));
    }
    {  // off the grid: no passable sample -> no path, the unit stays
        const UnitView v = run({-2000.0f, -2000.0f}, false);
        CHECK(v.failed && !v.arrived && v.pos.x == cell_center(5, 12).x && v.pos.y == cell_center(5, 12).y);
    }
    {  // everything within the 20 rings blocked -> no path
        const UnitView v = run(cell_center(30, 20), true);
        CHECK(v.failed && !v.arrived && v.pos.x == cell_center(5, 12).x);
    }
}

// s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7, FIGHTER_FLYING §7): a land squad container walks its own path
// at the team speed; its members steer to their slots around it (0x62d620). The members stand on one side of the
// container, so their centroid is off the container's straight path; a member behind its slot catches up faster than the
// container (up to its own max speed), one ahead of it waits; an idle team's members recover to their slots (> 4.0 away).
static void test_land_squad() {
    struct Run {
        uint64_t crc = 0;
        float max_off_path = 0.0f, min_centroid_gap = 1.0e9f;
        float lead_speed = 0.0f, lag_speed = 1.0e9f, cont_speed = 0.0f, team = 0.0f;
        bool path = false, arrived = false, member_view = false;
        float final_err = 1.0e9f;
    };
    const float mult = MovementConstants{}.land_speed_multiplier;
    const V2 start = cell_center(10, 20);
    const auto run = [&](const ParallelFor& pf) {
        Run r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        UnitDesc c = unit(100, start, 2, 5.0f, 12.0f, LocomotorKind::Default);
        c.team_container = true;
        CHECK(sys.add_unit(c));
        // slots (along, side): all on the +side, so the centroid sits 25 off the container
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 25.0f}, {0.0f, 35.0f}};
        // member 1 starts 20 behind its slot, member 3 20 ahead of it, member 2 on it
        const float along0[3] = {-20.0f, 0.0f, 20.0f};
        std::vector<ObjectId> ids;
        for (ObjectId id = 1; id <= 3; ++id) {
            const V2 sl = slots[id - 1];
            // member 1 is faster (3.0) than the team speed (the others' 2.0), so it can catch up
            CHECK(sys.add_unit(unit(id, {start.x + sl.x + along0[id - 1], start.y + sl.y}, 2, id == 1 ? 3.0f : 2.0f, 3.0f,
                                    LocomotorKind::Default)));
            ids.push_back(id);
        }
        CHECK(!sys.set_land_team(1, ids, slots));  // not a team container
        CHECK(sys.set_land_team(100, ids, slots));
        ObjectId tc = -1;
        V2 ts;
        CHECK(sys.team_slot(3, tc, ts) && tc == 100 && ts.y == 35.0f && sys.land_team_slots(100).size() == 3);
        for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}}) CHECK(sys.set_team(id, 100, 1.0f));
        r.team = 2.0f * mult;
        SimRng rng(9);
        MoveOrder o;
        o.units = {2};  // a member's order moves its container
        o.destination = cell_center(35, 20);
        sys.move(o, rng, 100);
        uint32_t first_move = 0;
        for (uint32_t f = 101; f < 400; ++f) {
            sys.tick(f, rng, pf);
            const UnitView k = sys.view(100);
            if (f == 102) r.path = k.has_goal && !sys.view(1).arrived && sys.view(1).has_goal;  // the member reports its team's order
            r.max_off_path = std::max(r.max_off_path, std::fabs(k.pos.y - start.y));
            V2 cen{0.0f, 0.0f};
            for (ObjectId id = 1; id <= 3; ++id) cen = cen + sys.view(id).pos;
            cen = cen * (1.0f / 3.0f);
            if (k.has_goal) r.min_centroid_gap = std::min(r.min_centroid_gap, length(cen - k.pos));
            r.cont_speed = std::max(r.cont_speed, k.speed);
            if (k.speed > 0.0f && first_move == 0) first_move = f;
            if (first_move != 0 && f < first_move + 6) {  // the container is under way; members 1 / 3 still off their slots
                r.lead_speed = std::max(r.lead_speed, sys.view(1).speed);
                r.lag_speed = std::min(r.lag_speed, sys.view(3).speed);
            }
        }
        r.arrived = sys.view(100).arrived && !sys.view(100).has_goal;
        r.member_view = sys.view(1).arrived && sys.view(1).goal == sys.view(100).goal;
        // every member ends near its slot point (recovery leaves it on the point; a member within 4.0 stays)
        const UnitView k = sys.view(100);
        const V2 cs = dir_from_deg(k.yaw);
        r.final_err = 0.0f;
        for (ObjectId id = 1; id <= 3; ++id) {
            const UnitView m = sys.view(id);
            const V2 off = slots[id - 1] + m.jiggle;
            const V2 tgt{k.pos.x + cs.x * off.x - cs.y * off.y, k.pos.y + cs.y * off.x + cs.x * off.y};
            r.final_err = std::max(r.final_err, length(tgt - m.pos));
        }
        // s78n t14 (FIGHTER_FLYING §7.3 item 6): this container has no ContainerArrangement (None), so it is never laid out
        // and a member knocked 30 off its slot while the team is idle does not recover (recovery: test_land_squad_walk_states)
        const V2 m2 = sys.view(2).pos;
        CHECK(sys.teleport(2, {m2.x - 30.0f, m2.y}, 0.0f, 90.0f, 400));
        bool recovering = false;
        for (uint32_t f = 400; f < 520; ++f) {
            sys.tick(f, rng, pf);
            recovering = recovering || sys.view(2).has_goal;
        }
        CHECK((!recovering && sys.view(2).pos == V2{m2.x - 30.0f, m2.y}));
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        // removing the container releases its members: their own orders move them again
        CHECK(sys.remove_unit(100) && !sys.team_slot(1, tc, ts));
        MoveOrder own;
        own.units = {1};
        own.destination = cell_center(70, 40);
        sys.move(own, rng, 520);
        for (uint32_t f = 521; f < 530; ++f) sys.tick(f, rng, pf);
        CHECK(sys.view(1).has_goal && sys.view(1).speed > 0.0f);
        return r;
    };
    const Run a = run(serial_for);
    const Run b = run(make_thread_pf(4));
    CHECK(a.crc == b.crc);
    CHECK(a.path && a.arrived && a.member_view);
    CHECK(a.max_off_path < 0.01f);          // the container walks its own straight path, never re-centred on the members
    CHECK(a.min_centroid_gap > 15.0f);      // ... which stay 25 to the side
    // at the team speed (the slowest member's Max_Speed x mult); s78n t16: the straggler cap 0x5ab640 lets it close in on
    // that speed from below while a member that cannot go faster trails it (the cap ends 2 behind the slot box)
    CHECK(a.cont_speed <= a.team && a.cont_speed > 0.9f * a.team);
    CHECK(a.lead_speed > a.team);           // the member behind its slot catches up above the team speed
    CHECK(a.lag_speed < a.team);            // the member ahead of its slot slows down
    CHECK(a.final_err < 25.0f);  // s78n t14: no recovery for a container that is never laid out (ContainerArrangement None)
    std::printf("land squad: off path %.4f, centroid gap >= %.2f, team %.3f, catch-up %.3f, wait %.3f, final slot error %.3f\n",
                static_cast<double>(a.max_off_path), static_cast<double>(a.min_centroid_gap), static_cast<double>(a.team),
                static_cast<double>(a.lead_speed), static_cast<double>(a.lag_speed), static_cast<double>(a.final_err));
}

// s78n t14 (FIGHTER_FLYING §7.2): the reference Dartboard scatter of the spec (radical inverse, rejection 5.0 + r, <= 1000
// tries, sort-matching with the retail grouping quirk), written out independently of land_team.cpp. Returns false when
// fewer than n points were accepted (the port then falls back to Rows).
static V2 t14v(float x, float y) { return V2{x, y}; }

static float t14_radinv(uint32_t i, uint32_t b) {
    float f = 0.0f, bk = static_cast<float>(b);
    while (i != 0) {
        f += static_cast<float>(i % b) / bk;
        i /= b;
        bk *= static_cast<float>(b);
    }
    return f;
}

static bool t14_dartboard(uint32_t n, float hx, float hy, float g, uint32_t seed, float spread, std::vector<V2>& pts) {
    uint32_t k = static_cast<uint32_t>(std::sqrt(static_cast<float>(n)));
    if (k * k != n) ++k;
    const uint32_t m = n / k + (n % k != 0 ? 1u : 0u);
    float r = std::sqrt(hx * hx + hy * hy);
    float L = static_cast<float>(k - 1) * g * 20.0f + static_cast<float>(k) * hx;
    float W = static_cast<float>(m - 1) * g * 20.0f + static_cast<float>(m) * hy;
    L *= spread;
    W *= spread;
    r *= spread;
    pts.clear();
    for (uint32_t t = 0; t < 1000 && pts.size() < n; ++t) {
        const float x = (t14_radinv(seed + t, 2) - 0.5f) * L, y = (t14_radinv(seed + t, 3) - 0.5f) * W;
        bool ok = true;
        for (const V2& p : pts) ok = ok && !(std::sqrt((y - p.y) * (y - p.y) + (x - p.x) * (x - p.x)) < 5.0f + r);
        if (ok) pts.push_back({x, y});
    }
    return pts.size() == n;
}

// Members 1..n of a container 100 at `at` (yaw 0); member i at at + rel[i - 1], shapes (hx, hy, type hx, type hy) and
// FormationSpacing per member.
struct T14Member {
    V2 rel;
    float hx = 3.0f, hy = 3.0f, spacing = 0.5f;
};

static void t14_squad(MovementSystem& sys, V2 at, int32_t arrangement, const std::vector<T14Member>& mem, float speed = 2.0f) {
    UnitDesc c = unit(100, at, 2, speed, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = arrangement;
    CHECK(sys.add_unit(c));
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (size_t i = 0; i < mem.size(); ++i) {
        UnitDesc d = unit(static_cast<ObjectId>(i + 1), at + mem[i].rel, 2, speed, 3.0f, LocomotorKind::Default);
        d.formation_spacing = mem[i].spacing;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(d.id, mem[i].hx, mem[i].hy, mem[i].hx, mem[i].hy));
        ids.push_back(d.id);
        slots.push_back(mem[i].rel);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    CHECK(sys.set_team(100, 100, 1.0f));
    for (ObjectId id : ids) CHECK(sys.set_team(id, 100, 1.0f));
}

static V2 t14_slot(const MovementSystem& sys, ObjectId id) {
    ObjectId c = -1;
    V2 s{-1.0e9f, -1.0e9f};
    CHECK(sys.team_slot(id, c, s) && c == 100);
    return s;
}

// s78n t14 (FIGHTER_FLYING §7.2): Rows 0x5ac970 and Dartboard 0x5ac2a0 slot geometry, the 0.99 re-lay rule, SPREAD_OUT.
static void test_land_squad_layout() {
    const V2 at = cell_center(20, 20);
    {  // Rows: 5 members (k = 3): row 0 = the three front-most, ordered from the most negative side; mixed shapes
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        std::vector<T14Member> mem(5);
        mem[0].rel = {30.0f, 10.0f};
        mem[1].rel = {30.0f, -10.0f};
        mem[2].rel = {0.0f, 5.0f};
        mem[3].rel = {0.0f, -5.0f};
        mem[4].rel = {-30.0f, 0.0f};
        for (T14Member& m : mem) {
            m.hx = 4.0f;
            m.hy = 3.0f;
        }
        mem[4].hx = 6.0f;
        mem[4].hy = 5.0f;
        mem[4].spacing = 1.0f;
        t14_squad(sys, at, 0, mem);
        bool lo = true;
        V2 ld;
        CHECK(sys.land_team_layout(100, lo, ld) && !lo);
        CHECK(sys.lay_out_land_team(100, {2.0f, 0.0f}, 1));  // dir is normalised
        // hand-computed (spec §7.2 Rows): row 0 W = 11 + 16 + 11, row 1 (HX 6, G 20) along -25, D = 35, shift 13.5
        CHECK(t14_slot(sys, 1) == t14v(13.5f, 16.0f));
        CHECK(t14_slot(sys, 2) == t14v(13.5f, -16.0f));
        CHECK(t14_slot(sys, 3) == t14v(13.5f, 0.0f));
        CHECK(t14_slot(sys, 4) == t14v(-11.5f, -12.5f));
        CHECK(t14_slot(sys, 5) == t14v(-11.5f, 10.5f));
        CHECK(sys.land_team_layout(100, lo, ld) && lo && ld == t14v(1.0f, 0.0f));
        const auto team = sys.land_team(100);  // the index the host's joint box reads is refreshed
        CHECK(team.size() == 5 && team[0].slot == t14v(13.5f, 16.0f) && team[4].slot == t14v(-11.5f, 10.5f));
        // 0x5ac120: a turn of 8 degrees keeps the slots (dot > 0.99), 9 degrees re-lays
        const V2 d8 = dir_from_deg(8.0f), d9 = dir_from_deg(9.0f);
        CHECK(!sys.lay_out_land_team(100, d8, 2) && sys.land_team_layout(100, lo, ld) && ld == t14v(1.0f, 0.0f));
        CHECK(sys.lay_out_land_team(100, d9, 3) && sys.land_team_layout(100, lo, ld) && std::fabs(ld.y - d9.y) < 1e-6f);
        // a re-lay with the same dir needs the flag cleared: SPREAD_OUT (x 1.8 spacing) re-lays at the next tick
        CHECK(!sys.lay_out_land_team(100, d9, 4));
        CHECK(sys.set_team_spread_out(100, true) && sys.land_team_layout(100, lo, ld) && !lo);
        SimRng rng(3);
        sys.tick(5, rng, serial_for);
        CHECK(sys.land_team_layout(100, lo, ld) && lo && ld == t14v(1.0f, 0.0f));  // the container's facing (yaw 0)
        // row 0 with g = 10 x 1.8 = 18: W = 15 + 24 + 15, sides -24, 0, 24 (the members did not move: the tick's recovery
        // decision read the cleared flag)
        CHECK(t14_slot(sys, 1).y == 24.0f && t14_slot(sys, 2).y == -24.0f && t14_slot(sys, 3).y == 0.0f);
        // None (-1): never laid out
        MovementSystem none{MovementConfig{}};
        land_system(none, 100);
        t14_squad(none, at, -1, mem);
        CHECK(!none.lay_out_land_team(100, {1.0f, 0.0f}, 1));
        none.tick(1, rng, serial_for);
        CHECK(none.land_team_layout(100, lo, ld) && !lo && t14_slot(none, 1) == mem[0].rel);
        std::printf("land squad layout: Rows 5 members (13.5, 16) (13.5, -16) (13.5, 0) (-11.5, -12.5) (-11.5, 10.5); spread row 0 "
                    "sides +-24\n");
    }
    {  // Dartboard: the Halton points of container id 100 x frame 7, assigned front to back / side to side
        CHECK(team_radical_inverse(0, 2) == 0.0f && team_radical_inverse(1, 2) == 0.5f && team_radical_inverse(2, 2) == 0.25f &&
              team_radical_inverse(3, 2) == 0.75f && team_radical_inverse(1, 3) == 1.0f / 3.0f &&
              team_radical_inverse(4, 3) == 1.0f / 3.0f + 1.0f / 9.0f);
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        std::vector<T14Member> mem(4);
        mem[0].rel = {20.0f, 0.0f};
        mem[1].rel = {0.0f, 20.0f};
        mem[2].rel = {0.0f, -20.0f};
        mem[3].rel = {-20.0f, 0.0f};
        for (T14Member& m : mem) m.spacing = 1.0f;
        t14_squad(sys, at, 1, mem);
        CHECK(sys.lay_out_land_team(100, {1.0f, 0.0f}, 7));
        std::vector<V2> pts;
        CHECK(t14_dartboard(4, 3.0f, 3.0f, 1.0f, 700u, 1.0f, pts));  // L = W = 26, r = sqrt(18)
        // the spec's matching for this case, by hand: points by x descending; members by projection (1 front, 4 back,
        // 2 / 3 tied at 0 -> index order); groups of x within W / m = 13 sorted by side / y
        std::vector<V2> by_x = pts;
        std::stable_sort(by_x.begin(), by_x.end(), [](V2 a, V2 b) { return a.x > b.x; });
        std::vector<ObjectId> order = {1, 2, 3, 4};
        const auto side_of = [&](ObjectId id) { return mem[static_cast<size_t>(id - 1)].rel.y; };
        uint32_t i = 0;
        do {
            uint32_t j = i;
            while (j < 4 && by_x[i].x - by_x[j].x <= 13.0f) ++j;
            std::stable_sort(order.begin() + i, order.begin() + j, [&](ObjectId a, ObjectId b) { return side_of(a) > side_of(b); });
            std::stable_sort(by_x.begin() + i, by_x.begin() + j, [](V2 a, V2 b) { return a.y > b.y; });
            i = j + 1;
        } while (i < 4);
        bool same = true;
        for (size_t q = 0; q < 4; ++q) same = same && t14_slot(sys, order[q]) == by_x[q];
        CHECK(same);
        std::printf("land squad layout: Dartboard id 100 x frame 7 points (%.3f, %.3f) (%.3f, %.3f) (%.3f, %.3f) (%.3f, %.3f), "
                    "matched %d\n",
                    static_cast<double>(pts[0].x), static_cast<double>(pts[0].y), static_cast<double>(pts[1].x),
                    static_cast<double>(pts[1].y), static_cast<double>(pts[2].x), static_cast<double>(pts[2].y),
                    static_cast<double>(pts[3].x), static_cast<double>(pts[3].y), same ? 1 : 0);
        // another frame is another scatter
        CHECK(sys.set_team_spread_out(100, false));
        std::vector<V2> pts2;
        CHECK(t14_dartboard(4, 3.0f, 3.0f, 1.0f, 800u, 1.0f, pts2) && pts2 != pts);
        // too few points (the rejection radius larger than the area): Rows with the same dir
        MovementSystem tight{MovementConfig{}};
        land_system(tight, 100);
        for (T14Member& m : mem) {
            m.spacing = 0.0f;
            m.hx = m.hy = 2.0f;
        }
        t14_squad(tight, at, 1, mem);
        CHECK(!t14_dartboard(4, 2.0f, 2.0f, 0.0f, 700u, 1.0f, pts));
        CHECK(tight.lay_out_land_team(100, {1.0f, 0.0f}, 7));
        // Rows of 2 x 2, g = 0, hx = hy = 2: front row 1 (side 0 -> -2), 2 (side 20 -> +2); back row 3 (-20 -> -2), 4
        CHECK(t14_slot(tight, 1) == t14v(2.0f, -2.0f) && t14_slot(tight, 2) == t14v(2.0f, 2.0f) &&
              t14_slot(tight, 3) == t14v(-2.0f, -2.0f) && t14_slot(tight, 4) == t14v(-2.0f, 2.0f));
    }
}

// s78n t14 (FIGHTER_FLYING §7.2 callers / §7.3): re-lay at the move start and at waypoints, start delays (drawn in team
// index order, counted down while the container turns or moves), the recovery rules, worker-count invariance.
static void test_land_squad_walk_states() {
    const V2 at = cell_center(20, 20);
    std::vector<T14Member> mem(4);
    mem[0].rel = {10.0f, 10.0f};
    mem[1].rel = {10.0f, -10.0f};
    mem[2].rel = {-10.0f, 10.0f};
    mem[3].rel = {-10.0f, -10.0f};
    {  // start delays: drawn at the order (speed 0), in team index order; members stand while > 0
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14_squad(sys, at, 0, mem);
        SimRng rng(77);
        sys.tick(1, rng, serial_for);  // the first layout (the container's facing)
        SimRng expect = rng;
        uint32_t want[4];
        for (uint32_t& w : want) w = static_cast<uint32_t>(expect.range_int(0, MovementConstants{}.max_move_frame_delay));
        MoveOrder o;
        o.units = {3};
        o.destination = cell_center(20, 60);  // 90 degrees left: the container turns in place first
        sys.move(o, rng, 2);
        bool drawn = true;
        for (ObjectId id = 1; id <= 4; ++id) drawn = drawn && sys.team_start_delay(id) == want[id - 1];
        CHECK(drawn && rng.state() == expect.state());
        V2 p0[4];
        for (ObjectId id = 1; id <= 4; ++id) p0[id - 1] = sys.view(id).pos;
        bool stood = true, counted = true, relaid_at_start = false;
        uint32_t left[4] = {want[0], want[1], want[2], want[3]};
        uint32_t go = 0;  // the container's own order stagger (OI tactical; set when the order is composed at the tick)
        for (uint32_t f = 2; f < 130; ++f) {
            sys.tick(f, rng, serial_for);
            if (go == 0) go = sys.view(100).start_frame;
            for (ObjectId id = 1; id <= 4; ++id) {
                const uint32_t d = sys.team_start_delay(id);
                if (f < go) counted = counted && d == want[id - 1];  // not counted down while the container waits
                counted = counted && (d == left[id - 1] || d + 1 == left[id - 1]);  // at most one per tick, never up
                if (f >= go && d > 0) stood = stood && sys.view(id).pos == p0[id - 1];  // stands while it counts
                p0[id - 1] = sys.view(id).pos;
                left[id - 1] = d;
            }
            bool lo = false;
            V2 ld;
            if (sys.land_team_layout(100, lo, ld) && lo && std::fabs(ld.y - 1.0f) < 1e-5f) relaid_at_start = true;
        }
        counted = counted && left[0] == 0 && left[1] == 0 && left[2] == 0 && left[3] == 0;
        CHECK(go > 2 && go < 100 && stood && counted && relaid_at_start);
        // a second order while the container moves: no draw
        CHECK(sys.view(100).speed > 0.0f);
        const uint32_t st = rng.state();
        o.destination = cell_center(30, 60);
        sys.move(o, rng, 130);
        CHECK(rng.state() == st);
        std::printf("land squad walk: start delays %u %u %u %u (team index order), stood while counting %d\n", want[0], want[1],
                    want[2], want[3], stood ? 1 : 0);
    }
    {  // recovery rules (0x62bbf0)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14_squad(sys, at, 0, mem);
        SimRng rng(5);
        for (uint32_t f = 1; f < 60; ++f) sys.tick(f, rng, serial_for);  // laid out; everyone on its slot
        const V2 s1 = t14_slot(sys, 1);
        const V2 home{at.x + s1.x, at.y + s1.y};  // yaw 0, no jiggle (the team never moved)
        CHECK(length(sys.view(1).pos - home) < 4.0f && !sys.view(1).has_goal);
        // 30 behind, facing away: turn first (speed 0), then walk
        CHECK(sys.teleport(1, {home.x - 30.0f, home.y}, 0.0f, 180.0f, 60));
        sys.tick(60, rng, serial_for);
        const UnitView t1 = sys.view(1);
        CHECK(t1.has_goal && t1.speed == 0.0f && t1.pos == t14v(home.x - 30.0f, home.y) && std::fabs(t1.yaw) < 180.0f &&
              std::fabs(t1.yaw) > 160.0f);
        // facing it within 0.5 x rate: walk at once
        CHECK(sys.teleport(1, {home.x - 30.0f, home.y}, 0.0f, 1.0f, 61));
        sys.tick(61, rng, serial_for);
        CHECK(sys.view(1).speed > 0.0f && sys.view(1).pos.x > home.x - 30.0f);
        uint32_t f = 62;
        for (; f < 200 && sys.view(1).has_goal; ++f) sys.tick(f, rng, serial_for);
        CHECK(!sys.view(1).has_goal && length(sys.view(1).pos - home) < 4.0f && sys.view(1).speed == 0.0f);
        // a container with a target: no recovery
        CHECK(sys.set_team_target(100, true));
        CHECK(sys.teleport(1, {home.x - 30.0f, home.y}, 0.0f, 0.0f, f));
        for (uint32_t g = f; g < f + 20; ++g) sys.tick(g, rng, serial_for);
        CHECK(!sys.view(1).has_goal && sys.view(1).pos == t14v(home.x - 30.0f, home.y));
        CHECK(sys.set_team_target(100, false));
        sys.tick(f + 20, rng, serial_for);
        CHECK(sys.view(1).has_goal);
        std::printf("land squad walk: recovery turn first, walk when facing, none with a target; settled at %u\n", f);
    }
    {  // a Dartboard squad walking a path with corners: re-lays at waypoints; worker-count invariant
        const auto run = [&](const ParallelFor& pf, uint32_t& relays) {
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(100, 100);
            for (int32_t y = 10; y <= 40; ++y) g.set_base(30, y, kPassImpassable);  // a wall: the path bends around it
            sys.set_grid(std::move(g));
            sys.set_anim_length_fn(anim_len);
            std::vector<T14Member> m9(9);
            for (size_t i = 0; i < 9; ++i) {
                m9[i].rel = {static_cast<float>(static_cast<int32_t>(i % 3) - 1) * 12.0f, static_cast<float>(static_cast<int32_t>(i / 3) - 1) * 12.0f};
                m9[i].spacing = 0.6f;
            }
            t14_squad(sys, at, 1, m9);
            SimRng rng(11);
            MoveOrder o;
            o.units = {5};
            o.destination = cell_center(45, 25);
            sys.move(o, rng, 1);
            V2 last{};
            relays = 0;
            for (uint32_t f = 1; f < 900; ++f) {
                sys.tick(f, rng, pf);
                bool lo = false;
                V2 ld;
                if (sys.land_team_layout(100, lo, ld) && lo && !(ld == last)) {
                    ++relays;
                    last = ld;
                }
            }
            CHECK(sys.view(100).arrived);
            StateHasher h;
            sys.hash_units(h);
            return h.digest();
        };
        uint32_t ra = 0, rb = 0;
        const uint64_t a = run(serial_for, ra), b = run(make_thread_pf(4), rb);
        CHECK(a == b && ra == rb && ra >= 3);  // the first layout, the move start, >= 1 corner
        std::printf("land squad walk: Dartboard march re-laid %u times, serial = 4 workers %d\n", ra, a == b ? 1 : 0);
    }
}

// s78n review (FIGHTER_FLYING §7, MOVEMENT §3.7):
//  * a unit crossing a marching squad takes no collision object with a member (a member is neither a stopped obstacle
//    nor pathing: before, the moving checker treated it as stopped - MovingVsStopped, path ends, nudges) and arrives;
//  * a member's step has no passability test (FIGHTER_FLYING §7.3 "Recovery in full" item 7): a member whose slot point
//    lies in impassable cells walks onto it and stands there (before: a [port] wall test and a give-up);
//  * park_team_member / rejoin_land_team (the garrison entry and leave) keep the member's container, slot and team index.
static void test_land_squad_review() {
    const auto squad = [](MovementSystem& sys, V2 at, const V2 (&slots)[3], const V2 (&starts)[3], int32_t arrangement = -1) {
        UnitDesc c = unit(100, at, 2, 2.0f, 12.0f, LocomotorKind::Default);
        c.team_container = true;
        c.container_arrangement = arrangement;  // s78n t14
        CHECK(sys.add_unit(c));
        std::vector<ObjectId> ids;
        for (ObjectId id = 1; id <= 3; ++id) {
            CHECK(sys.add_unit(unit(id, starts[id - 1], 2, 2.0f, 6.0f, LocomotorKind::Default)));
            ids.push_back(id);
        }
        CHECK(sys.set_land_team(100, ids, slots));
        for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}}) CHECK(sys.set_team(id, 100, 1.0f));
    };
    {  // a slow unit crossing the squad's line while the squad marches through
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 0.0f}, {0.0f, -15.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts);
        UnitDesc x = unit(50, cell_center(30, 16), 2, 0.4f, 6.0f, LocomotorKind::Default);
        CHECK(sys.add_unit(x));
        SimRng rng(5);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 1);
        MoveOrder xo;
        xo.units = {50};
        xo.destination = cell_center(30, 24);
        sys.move(xo, rng, 1);
        uint32_t member_collisions = 0, crossings = 0;
        for (uint32_t f = 2; f < 700; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& co : sys.collisions().list())
                if ((co.a >= 1 && co.a <= 3) || (co.b >= 1 && co.b <= 3)) ++member_collisions;
            const UnitView k = sys.view(100);
            if (k.has_goal && std::fabs(k.pos.x - sys.view(50).pos.x) < 20.0f) ++crossings;
        }
        const UnitView xv = sys.view(50);
        CHECK(crossings > 0);  // the squad passed the crossing unit while it walked
        CHECK(member_collisions == 0);
        CHECK(xv.arrived && !xv.failed && length(xv.pos - cell_center(30, 24)) < 1.0f);
        CHECK(sys.view(100).arrived);
        std::printf("land squad review: crossing frames %u, member collisions %u, crosser arrived %d\n", crossings,
                    member_collisions, xv.arrived ? 1 : 0);
    }
    {  // a slot in a wall: the recovery walks straight at it, through the impassable cells, and ends on it
        MovementSystem sys{MovementConfig{}};
        // s78n t14: a Rows container (recovery needs the laid-out flag): members 2 / 3 in front (+x), member 1 the back-most,
        // alone in the back row; half-extents 20 -> member 1's slot (-20, 0), its slot point (180, 400) in the wall
        PassabilityGrid g2 = open_grid(100, 100);
        for (int32_t y = 19; y <= 21; ++y)
            for (int32_t x = 7; x <= 9; ++x) g2.set_base(x, y, kPassImpassable);
        sys.set_grid(std::move(g2));
        sys.set_anim_length_fn(anim_len);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{-100.0f, 0.0f}, {20.0f, -20.0f}, {20.0f, 20.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts, 0);
        for (ObjectId id = 1; id <= 3; ++id) CHECK(sys.set_team_member_shape(id, 20.0f, 20.0f, 20.0f, 20.0f));
        const uint32_t mask = MovementConfig{}.classes.mask(2);
        SimRng rng(6);
        uint32_t goal_frames = 0, wall_frames = 0;
        for (uint32_t f = 1; f < 200; ++f) {
            sys.tick(f, rng, serial_for);
            if (sys.view(1).has_goal) ++goal_frames;
            if (!sys.grid().passable_world(sys.view(1).pos, mask)) ++wall_frames;
        }
        ObjectId tc1 = -1;
        V2 ts1;
        CHECK((sys.team_slot(1, tc1, ts1) && ts1 == V2{-20.0f, 0.0f} && sys.team_slot(2, tc1, ts1) && ts1 == V2{20.0f, -20.0f}));
        const V2 sp0{at.x - 20.0f, at.y};
        CHECK(goal_frames > 10 && wall_frames > 0);  // it walked, on impassable cells too
        CHECK((!sys.view(100).has_goal && !sys.view(1).has_goal && length(sys.view(1).pos - sp0) <= 4.0f));  // and is on its slot
        CHECK(!sys.grid().passable_world(sp0, mask));
        // the container is teleported to open ground: s78n t18 (0x3abb80) the member goes with it, at its offset - it
        // stands on its slot there and has nothing to recover
        const V2 there = cell_center(20, 30);
        const V2 off1 = sys.view(1).pos - sys.view(100).pos;
        CHECK(sys.teleport(100, there, 0.0f, 0.0f, 200));
        CHECK(sys.view(1).pos == there + off1);
        bool again = false;
        for (uint32_t f = 200; f < 700; ++f) {
            sys.tick(f, rng, serial_for);
            again = again || sys.view(1).has_goal;
        }
        const V2 sp{there.x - 20.0f, there.y};  // slot (-20, 0) at yaw 0, no jiggle (the team never moved)
        CHECK(!again && !sys.view(1).has_goal && length(sys.view(1).pos - sp) <= 4.0f);
        std::printf("land squad review: slot in a wall - recovering %u frames, on impassable cells %u frames\n", goal_frames,
                    wall_frames);
    }
    {  // park / rejoin (garrison entry and leave)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 0.0f}, {0.0f, -15.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts);
        CHECK(sys.is_land_team_container(100) && !sys.is_land_team_container(2));
        CHECK(!sys.park_team_member(100) && sys.has_unit(100));  // not a steering member: nothing done
        CHECK(sys.park_team_member(2) && !sys.has_unit(2) && sys.team_member_parked(2));
        CHECK(sys.land_team(100).size() == 2 && sys.land_team(100)[0].member == 1 && sys.land_team(100)[1].member == 3);
        CHECK(!sys.rejoin_land_team(2));  // not added again yet: the record is dropped
        CHECK(sys.park_team_member(1) && !sys.team_member_parked(2));
        CHECK(sys.add_unit(unit(1, cell_center(14, 24), 2, 2.0f, 6.0f, LocomotorKind::Default)));
        CHECK(sys.rejoin_land_team(1) && !sys.team_member_parked(1));
        ObjectId tc = -1;
        V2 ts;
        CHECK(sys.team_slot(1, tc, ts) && tc == 100 && ts == slots[0]);
        const auto team = sys.land_team(100);
        CHECK(team.size() == 2 && team[0].member == 1 && team[0].tindex == 0 && team[1].member == 3 && team[1].tindex == 2);
        // an order on the rejoined member moves the squad
        SimRng rng(7);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 20);
        sys.move(o, rng, 1);
        CHECK(sys.view(100).has_goal && sys.view(1).has_goal);
        // a parked member removed for good (it died inside) and a removed container drop their records
        CHECK(sys.park_team_member(3) && sys.team_member_parked(3) && !sys.remove_unit(3) && !sys.team_member_parked(3));
        CHECK(sys.park_team_member(1) && sys.remove_unit(100) && !sys.team_member_parked(1) && sys.land_team(100).empty());
    }
    {  // s78n t06 review: suspend / rejoin (a squad-member hero in a clash): orders act on the member itself meanwhile
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(10, 20);
        const V2 slots[3] = {{0.0f, 15.0f}, {0.0f, 0.0f}, {0.0f, -15.0f}};
        const V2 starts[3] = {at + slots[0], at + slots[1], at + slots[2]};
        squad(sys, at, slots, starts, 0);  // s78n t14: Rows (the recovery below needs a laid-out container)
        ObjectId tc = -1;
        V2 ts;
        CHECK(sys.team_slot(2, tc, ts) && tc == 100);
        CHECK(!sys.suspend_team_member(100));  // not a steering member: nothing done
        CHECK(sys.suspend_team_member(2) && sys.has_unit(2) && sys.team_member_parked(2));
        CHECK(!sys.team_slot(2, tc, ts) && sys.land_team(100).size() == 2);
        SimRng rng(8);
        MoveOrder o;
        o.units = {2};
        o.destination = cell_center(10, 40);
        sys.move(o, rng, 1);
        CHECK(sys.view(2).has_goal && !sys.view(100).has_goal);  // the member walks, its squad stays
        for (uint32_t f = 2; f < 400; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(2).arrived && length(sys.view(2).pos - cell_center(10, 40)) < 2.0f && length(sys.view(100).pos - at) < 0.5f);
        const ObjectId stop_ids[1] = {2};
        sys.stop(stop_ids);
        CHECK(!sys.view(100).has_goal);
        CHECK(sys.rejoin_land_team(2) && !sys.team_member_parked(2));
        CHECK(sys.team_slot(2, tc, ts) && tc == 100 && ts == slots[1] && sys.land_team(100).size() == 3);
        for (uint32_t f = 400; f < 900; ++f) sys.tick(f, rng, serial_for);
        CHECK(length(sys.view(2).pos - (at + slots[1])) <= 4.0f);  // recovered to its slot
        std::printf("land squad review: suspended member walked alone and recovered to its slot\n");
    }
}

static void test_determinism() {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const CrcLog a = mixed_run(serial_for);
    const CrcLog b = mixed_run(make_thread_pf(4));
    const CrcLog c = mixed_run(make_thread_pf(hw));
    CHECK(!a.records.empty() && !first_divergence(a, b).diverged && !first_divergence(a, c).diverged);
    const uint64_t g = a.records.back().combined;
    std::printf("determinism: mixed %zu ticks final %016llx (1/4/%u threads identical)\n", a.records.size(),
                static_cast<unsigned long long>(g), hw);
    if (kGoldenMixed != 0) CHECK(g == kGoldenMixed);
}

// S8 t11 / t12 (MOVEMENT §3.2 "Deploy states", ABILITIES_HARDPOINTS §4.13): the Walk deploy window. Clip 0x19 / 0x1A
// with 30 frames at 30 fps: step = 1 x Deployment_Anim_Rate, end at position >= 30 - 2 - step.
static void test_deploy() {
    using DR = MovementSystem::DeployResult;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    CHECK(sys.add_unit(trooper(1, cell_center(10, 10))));
    UnitDesc fast = trooper(2, cell_center(10, 30));
    fast.deployment_anim_rate = 2.0f;
    CHECK(sys.add_unit(fast));
    CHECK(sys.add_unit(unit(3, cell_center(10, 50), 0, 2.0f, 6.0f, LocomotorKind::Jetpack)));
    sys.set_deploy_clip_fn([](ObjectId, int32_t anim, uint32_t& n, float& fps) {
        if (anim != 0x19 && anim != 0x1A) return false;
        n = 30;
        fps = 30.0f;
        return true;
    });
    bool turret_rest = true;
    sys.set_turret_rest_fn([&turret_rest](ObjectId id) { return id != 1 || turret_rest; });
    SimRng rng(7);
    auto finished_at = [&](ObjectId id, LocoState to, uint32_t from, uint32_t limit) {
        for (uint32_t f = from; f < limit; ++f) {
            sys.tick(f, rng);
            for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
                if (e.id == id && e.to == to && e.finished) return f;
        }
        return 0xFFFFFFFFu;
    };
    // Only Walk deploys; unknown units are NoWalk too.
    CHECK(sys.deploy(3, true, 0) == DR::NoWalk && sys.deploy(99, true, 0) == DR::NoWalk && sys.deploy(1, false, 0) == DR::Ignored);
    // A moving unit stops; Deploy(true) at frame 10 -> 0x18 now, 0x19 when the clip ends (10 + 27 at rate 1).
    MoveOrder o;
    o.units = {1, 2};
    o.destination = cell_center(60, 10);
    sys.move(o, rng, 0);
    for (uint32_t f = 0; f < 10; ++f) sys.tick(f, rng);
    CHECK(sys.deploy(1, true, 10) == DR::Deploying && sys.deploy(2, true, 10) == DR::Deploying);
    CHECK(sys.deploy(1, true, 10) == DR::Ignored);  // nothing in 0x18
    {
        const std::vector<MovementSystem::DeployEvent> ev = sys.take_deploy_events();
        CHECK(ev.size() == 2 && ev[0].id == 1 && ev[0].from == LocoState::None && ev[0].to == LocoState::WalkDeploying &&
              !ev[0].finished);
    }
    CHECK(sys.deploy_state(1) == LocoState::WalkDeploying && sys.view(1).deploy_state == LocoState::WalkDeploying);
    const V2 held = sys.view(1).pos;
    uint32_t f2 = 0xFFFFFFFFu, f1 = 0xFFFFFFFFu;
    for (uint32_t f = 10; f < 80 && f1 == 0xFFFFFFFFu; ++f) {
        sys.tick(f, rng);
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events()) {
            if (e.finished && e.to == LocoState::WalkDeployed && e.id == 1) f1 = f;
            if (e.finished && e.to == LocoState::WalkDeployed && e.id == 2) f2 = f;
        }
        CHECK(sys.view(1).pos == held);  // a deploying unit does not move
    }
    CHECK(f1 == 37);  // pos = f - 10 >= 27
    CHECK(f2 == 23);  // rate 2: 2 (f - 10) >= 26
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed && sys.deploy(1, true, 40) == DR::Ignored);
    // An order while deployed waits: the unit stays put.
    sys.move(o, rng, 40);
    for (uint32_t f = 40; f < 60; ++f) sys.tick(f, rng);
    CHECK(sys.view(1).pos == held);
    // Undeploy: 0x1A, the clip from 0, back to the walk 27 frames later (state None, signal).
    CHECK(sys.deploy(1, false, 60) == DR::Undeploying && sys.deploy_state(1) == LocoState::WalkUndeploying);
    const uint32_t undeployed = finished_at(1, LocoState::None, 60, 200);
    CHECK(undeployed == 87 && sys.deploy_state(1) == LocoState::None);

    // S8 t24 (MOVEMENT §3.2 "Where it gates"): Deploy(true) from the walk starts its clip at once, the turret off rest or
    // not (frame 100 -> ends 127).
    turret_rest = false;
    CHECK(sys.deploy(1, true, 100) == DR::Deploying);
    CHECK(finished_at(1, LocoState::WalkDeployed, 100, 300) == 127);
    // Blocked reverse: 0x1A with the turret off rest (no clip yet); Deploy(true) -> 0x19 at once with the signal.
    turret_rest = false;
    CHECK(sys.deploy(1, false, 150) == DR::Undeploying);
    sys.tick(150, rng);
    (void)sys.take_deploy_events();
    CHECK(sys.deploy(1, true, 151) == DR::ReverseBlocked && sys.deploy_state(1) == LocoState::WalkDeployed);
    {
        const std::vector<MovementSystem::DeployEvent> ev = sys.take_deploy_events();
        CHECK(ev.size() == 1 && ev[0].from == LocoState::WalkUndeploying && ev[0].to == LocoState::WalkDeployed && ev[0].finished);
    }
    turret_rest = true;
    CHECK(finished_at(1, LocoState::None, 160, 200) == 0xFFFFFFFFu);  // nothing pending
    CHECK(sys.deploy(1, false, 200) == DR::Undeploying);
    CHECK(finished_at(1, LocoState::None, 200, 300) == 227);
    // Reversal: Undeploy (frame 250) out of a Deploy clip at position 10 starts the Undeploy clip at 30 - 10 - 1 - 1 = 18:
    // it ends at 27, 9 frames later.
    CHECK(sys.deploy(1, true, 240) == DR::Deploying);
    for (uint32_t f = 240; f < 250; ++f) sys.tick(f, rng);
    CHECK(sys.deploy(1, false, 250) == DR::Undeploying);
    CHECK(finished_at(1, LocoState::None, 250, 300) == 259);

    // Save round trip mid-clip: identical continuation.
    CHECK(sys.deploy(1, true, 300) == DR::Deploying);
    for (uint32_t f = 300; f < 305; ++f) sys.tick(f, rng);
    ByteWriter w;
    sys.write(w);
    MovementSystem copy{MovementConfig{}};
    land_system(copy, 100);
    copy.set_deploy_clip_fn([](ObjectId, int32_t, uint32_t& n, float& fps) {
        n = 30;
        fps = 30.0f;
        return true;
    });
    {
        ByteReader r(w.bytes());
        CHECK(copy.read(r) && r.done());
    }
    StateHasher ha, hb;
    sys.hash_units(ha);
    copy.hash_units(hb);
    CHECK(ha.digest() == hb.digest() && copy.deploy_state(1) == LocoState::WalkDeploying);
    SimRng r2 = rng;
    uint32_t end_a = 0, end_b = 0;
    for (uint32_t f = 305; f < 340; ++f) {
        sys.tick(f, rng);
        copy.tick(f, r2);
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
            if (e.id == 1 && e.finished) end_a = f;
        for (const MovementSystem::DeployEvent& e : copy.take_deploy_events())
            if (e.id == 1 && e.finished) end_b = f;
    }
    CHECK(end_a == 327 && end_b == 327);

    // S8 t12 (e): a locomotor swap undeploys without a signal; JetPack cannot deploy; the Walk rebuilt on landing starts
    // undeployed.
    (void)sys.take_deploy_events();
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed && sys.use_secondary_locomotor(1, true));
    {
        const std::vector<MovementSystem::DeployEvent> ev = sys.take_deploy_events();
        CHECK(ev.size() == 1 && ev[0].from == LocoState::WalkDeployed && ev[0].to == LocoState::None && !ev[0].finished);
    }
    CHECK(sys.deploy_state(1) == LocoState::None && sys.deploy(1, true, 400) == DR::NoWalk);
    CHECK(sys.use_secondary_locomotor(1, false) && sys.deploy_state(1) == LocoState::None);
    CHECK(sys.deploy(1, true, 401) == DR::Deploying);
    // snap_yaw: the facing at once, position kept.
    const V2 p = sys.view(2).pos;
    CHECK(sys.snap_yaw(2, 90.0f) && near(sys.view(2).yaw, 90.0f, 1e-4f) && sys.view(2).pos == p && !sys.snap_yaw(99, 0.0f));
    std::printf("deploy: window timing, turret wait, blocked reverse, reversal, save, locomotor swap ok\n");
}

// S8 t24 (MOVEMENT §3.2 "Turret recentre around deploy"): the TURRET model of Deploys units. Unit 1 AT-AT-like (3 deg
// per frame, +-55), unit 2 SPMAT-like (3 deg per frame, 360, deployed rest elevation -45). Clips 30 frames at 30 fps.
static void test_turret() {
    using DR = MovementSystem::DeployResult;
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    UnitDesc a = trooper(1, cell_center(10, 10));
    a.turret = true;
    a.turret_speed = 3.0f;
    a.turret_extent = a.turret_deployed_extent = 55.0f;
    UnitDesc s = trooper(2, cell_center(10, 40));
    s.turret = true;
    s.turret_speed = 3.0f;
    s.turret_extent = s.turret_deployed_extent = 360.0f;
    s.turret_deployed_rest[1] = -45.0f;
    CHECK(sys.add_unit(a) && sys.add_unit(s));
    sys.set_deploy_clip_fn([](ObjectId, int32_t, uint32_t& n, float& fps) {
        n = 30;
        fps = 30.0f;
        return true;
    });
    bool aim = false;
    const V2 p1 = sys.view(1).pos;
    sys.set_turret_aim_fn([&](ObjectId id, float& x, float& y, float& z) {
        if (id != 1 || !aim) return false;
        x = p1.x;
        y = p1.y + 100.0f;  // 90 degrees left of the body (yaw 0)
        z = 0.0f;
        return true;
    });
    SimRng rng(3);
    uint32_t f = 0;
    auto run_to = [&](uint32_t end) {
        for (; f < end; ++f) sys.tick(f, rng);
    };
    // Aiming: 3 degrees per frame, clamped to the 55-degree extent.
    aim = true;
    run_to(10);
    CHECK(near(sys.view(1).turret_yaw, 30.0f, 1e-4f));
    run_to(20);
    CHECK(sys.view(1).turret_yaw == 55.0f && sys.view(2).turret_yaw == 0.0f);
    // Idle: held for Turret_Rotate_To_Rest_Delay_Sec (2 s = 60 frames), then back to rest at 3 per frame.
    aim = false;
    run_to(79);
    CHECK(sys.view(1).turret_yaw == 55.0f);
    run_to(90);
    CHECK(sys.view(1).turret_yaw < 55.0f && sys.view(1).turret_yaw > 0.0f);
    run_to(110);
    CHECK(sys.view(1).turret_yaw == 0.0f);
    // Undeploy waits for the rest angle: aim while deployed (yaw 55), then Deploy(false) - the forced recentre skips the
    // delay (one held frame, then 19 frames back), the Undeploy clip starts only at rest and ends 27 frames later.
    CHECK(sys.deploy(1, true, f) == DR::Deploying);
    run_to(f + 40);
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed);
    aim = true;
    run_to(f + 20);
    CHECK(sys.view(1).turret_yaw == 55.0f);
    aim = false;
    (void)sys.take_deploy_events();
    const uint32_t u0 = f;
    CHECK(sys.deploy(1, false, u0) == DR::Undeploying);
    uint32_t clip_at = 0, done_at = 0;
    for (; f < u0 + 120 && done_at == 0; ++f) {
        sys.tick(f, rng);
        if (clip_at == 0 && sys.view(1).deploy_clip == 0x1A) {
            clip_at = f;
            CHECK(sys.view(1).turret_yaw == 0.0f);  // at rest when the clip starts
        }
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
            if (e.id == 1 && e.to == LocoState::None && e.finished) done_at = f;
    }
    CHECK(clip_at >= u0 + 19 && clip_at <= u0 + 21 && done_at == clip_at + 27);
    // Reversing out of 0x1A while off rest: straight to 0x19 with the signal.
    aim = true;
    run_to(f + 20);  // walk state: yaw 55 again
    CHECK(sys.deploy(1, true, f) == DR::Deploying);
    run_to(f + 40);
    CHECK(sys.deploy(1, false, f) == DR::Undeploying);
    aim = false;
    sys.tick(f, rng);
    ++f;
    CHECK(!near(sys.view(1).turret_yaw, 0.0f, 1e-4f) && sys.deploy(1, true, f) == DR::ReverseBlocked);
    CHECK(sys.deploy_state(1) == LocoState::WalkDeployed);
    // SPMAT: deployed, the idle return goes to the deployed rest (elevation -45); the undeploy recentres to the
    // undeployed rest (0) before its clip.
    CHECK(sys.deploy(2, true, f) == DR::Deploying);
    run_to(f + 120);
    CHECK(sys.deploy_state(2) == LocoState::WalkDeployed && sys.view(2).turret_elevation == -45.0f);
    CHECK(sys.deploy(2, false, f) == DR::Undeploying);
    (void)sys.take_deploy_events();
    const uint32_t s0 = f;
    uint32_t s_done = 0;
    for (; f < s0 + 120 && s_done == 0; ++f) {
        sys.tick(f, rng);
        for (const MovementSystem::DeployEvent& e : sys.take_deploy_events())
            if (e.id == 2 && e.to == LocoState::None && e.finished) s_done = f;
    }
    CHECK(s_done >= s0 + 15 + 27 && s_done <= s0 + 17 + 27 && sys.view(2).turret_elevation == 0.0f);
    // Save round trip with the turret mid-turn: identical continuation.
    aim = true;
    run_to(f + 5);
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
    CHECK(ha.digest() == hb.digest() && copy.view(1).turret_yaw == sys.view(1).turret_yaw);
    std::printf("turret: aim + extent, idle hold + return, undeploy wait %u frames, blocked reverse, SPMAT deployed rest, save ok\n",
                clip_at - u0);
}

// s78n t14 part B (FIGHTER_FLYING §7.1 / §7.5): a Rows squad - container 100 (Is_Squashable, Infantry, radius 12,
// MovementPredictionInterval 4) and members 1..4 at (+-10, +-10) - of `owner` at `at`, facing +x.
static void t14b_squad(MovementSystem& sys, V2 at, int32_t owner, bool avoidance_disabled = false) {
    UnitDesc c = unit(100, at, 2, 2.0f, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = 0;
    c.squashable = true;
    c.owner = owner;
    c.movement_prediction_interval = 4.0f;
    CHECK(sys.add_unit(c));
    const V2 rel[4] = {{10.0f, 10.0f}, {10.0f, -10.0f}, {-10.0f, 10.0f}, {-10.0f, -10.0f}};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (ObjectId id = 1; id <= 4; ++id) {
        UnitDesc d = unit(id, at + rel[id - 1], 2, 2.0f, 3.0f, LocomotorKind::Default);
        d.owner = owner;
        d.formation_spacing = 0.5f;
        d.avoidance_disabled = avoidance_disabled;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(id);
        slots.push_back(rel[id - 1]);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    for (ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}, ObjectId{4}}) CHECK(sys.set_team(id, 100, 1.0f));
}

// The world point of member `id`'s slot (container position + R(yaw) (slot + jiggle)).
static V2 t14b_slot_point(const MovementSystem& sys, ObjectId id) {
    ObjectId c = -1;
    V2 s;
    if (!sys.team_slot(id, c, s)) return V2{-1.0e9f, -1.0e9f};
    const UnitView k = sys.view(c), m = sys.view(id);
    const V2 cs = dir_from_deg(k.yaw);
    const V2 off = s + m.jiggle;
    return {k.pos.x + cs.x * off.x - cs.y * off.y, k.pos.y + cs.y * off.x + cs.x * off.y};
}

// s78n t14 part B (FIGHTER_FLYING §7.1 / §7.5, MOVEMENT §5.4):
//  * the squad container is the collision checker / partner: a Giant vehicle marching through a stopped squad takes its
//    collision object with the container and pushes the container aside; no member is ever a participant or pushed;
//  * a crusher (SQUASH) approaching a stopped hostile squad pushes its container on a crush-flee path: the squad flees with
//    Fleeing_Infantry_Speed_Bonus (2) on the container's team speed and the members' max speed, then stops fleeing when
//    the push path ends and the members recover to their slots; worker-count invariant;
//  * the member steering vector 0x509810: a stopped unit put right beside a marching squad gives the near member a side
//    step away from it; members stay near their slots on a curved march; worker-count invariant.
static void test_land_squad_part_b() {
    const V2 at = cell_center(30, 20);
    {  // A: a Giant vehicle vs a stopped squad
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14b_squad(sys, at, 1);
        SimRng rng(21);
        for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, serial_for);
        UnitDesc g = unit(50, cell_center(10, 20), 2, 1.0f, 20.0f, LocomotorKind::Default);
        g.collision_class = kUccGiantVehicle;
        g.owner = 2;
        CHECK(sys.add_unit(g));
        MoveOrder o;
        o.units = {50};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 20);
        // (the Giant's MovingVsStopped object with the container is resolved by the push in the pass that creates it, so
        // the live list rarely shows it; the push is the evidence)
        uint32_t member_objects = 0;
        bool cont_pushed = false, member_pushed = false, fled = false;
        for (uint32_t f = 20; f < 900; ++f) {
            sys.tick(f, rng, serial_for);
            for (const CollisionObject& co : sys.collisions().list())
                if ((co.a >= 1 && co.a <= 4) || (co.b >= 1 && co.b <= 4)) ++member_objects;
            cont_pushed = cont_pushed || sys.view(100).pushed;
            for (ObjectId id = 1; id <= 4; ++id) member_pushed = member_pushed || sys.view(id).pushed;
            fled = fled || sys.team_fleeing(100);
        }
        const UnitView gv = sys.view(50);
        CHECK(cont_pushed && !member_pushed && member_objects == 0 && !fled);
        CHECK(gv.arrived && !gv.failed);
        CHECK(sys.stats().pushes >= 1);
        std::printf("land squad part B: Giant vs squad - pushes %u, member objects %u, container pushed %d, giant arrived %d\n",
                    sys.stats().pushes, member_objects, cont_pushed ? 1 : 0, gv.arrived ? 1 : 0);
    }
    {  // B: a crusher vs a stopped hostile squad: crush-flee, 2x, recovery
        struct Res {
            uint64_t crc = 0;
            uint32_t flee_frames = 0;
            float team = 0.0f, cmax = 0.0f, mmax = 0.0f, final_err = 1.0e9f;
            bool ended = false, crusher_arrived = false;
        };
        const auto run = [&](const ParallelFor& pf) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            t14b_squad(sys, at, 1);
            SimRng rng(22);
            for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, pf);
            r.team = sys.view(100).team_speed;
            UnitDesc cr = unit(60, cell_center(8, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
            cr.collision_class = kUccVehicle;
            cr.crusher = true;
            cr.owner = 2;
            CHECK(sys.add_unit(cr));
            MoveOrder o;
            o.units = {60};
            o.destination = cell_center(60, 20);
            sys.move(o, rng, 20);
            uint32_t last_flee = 0;
            for (uint32_t f = 20; f < 1100; ++f) {
                sys.tick(f, rng, pf);
                if (!sys.team_fleeing(100)) continue;
                ++r.flee_frames;
                last_flee = f;
                r.cmax = std::max(r.cmax, sys.view(100).speed);
                for (ObjectId id = 1; id <= 4; ++id) r.mmax = std::max(r.mmax, sys.view(id).speed);
            }
            r.ended = last_flee != 0 && last_flee < 1000 && !sys.team_fleeing(100) && !sys.view(100).pushed;
            r.crusher_arrived = sys.view(60).arrived;
            r.final_err = 0.0f;
            for (ObjectId id = 1; id <= 4; ++id) r.final_err = std::max(r.final_err, length(t14b_slot_point(sys, id) - sys.view(id).pos));
            StateHasher h;
            sys.hash_units(h);
            r.crc = h.digest();
            return r;
        };
        const Res a = run(serial_for);
        const Res b = run(make_thread_pf(4));
        CHECK(a.crc == b.crc && a.flee_frames == b.flee_frames);
        CHECK(a.flee_frames > 0 && a.ended && a.crusher_arrived);
        // the container's team speed and the members' max speed (= the team speed here) x 2 while fleeing
        // (Fleeing_Infantry_Speed_Bonus is exactly 2: this fixture reaches its target speed in one frame)
        CHECK(a.cmax == 2.0f * a.team);
        CHECK(a.mmax == 2.0f * a.team);
        CHECK(a.final_err <= 5.0f);  // recovered (a member stops within the 4.0 recovery distance)
        std::printf("land squad part B: crusher - fled %u frames, team %.3f, container max %.3f, member max %.3f, final slot "
                    "error %.3f\n",
                    a.flee_frames, static_cast<double>(a.team), static_cast<double>(a.cmax), static_cast<double>(a.mmax),
                    static_cast<double>(a.final_err));
    }
    {  // C: the member steering vector on a curved march
        struct Res {
            uint64_t crc = 0;
            float max_err = 0.0f, best_side = 0.0f;
            uint32_t vec_frames = 0;
            bool arrived = false;
        };
        const auto run = [&](const ParallelFor& pf, bool avoidance_disabled) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            PassabilityGrid g = open_grid(100, 100);
            for (int32_t y = 10; y <= 40; ++y) g.set_base(30, y, kPassImpassable);  // a wall: the path bends around it
            sys.set_grid(std::move(g));
            sys.set_anim_length_fn(anim_len);
            const V2 start = cell_center(20, 20);
            t14b_squad(sys, start, 1, avoidance_disabled);
            SimRng rng(23);
            MoveOrder o;
            o.units = {1};
            o.destination = cell_center(45, 25);
            sys.move(o, rng, 1);
            ObjectId next_obstacle = 70;
            for (uint32_t f = 1; f < 1200; ++f) {
                // every 90 frames of the march a stopped unit appears just ahead and right of the container
                const UnitView k = sys.view(100);
                if (k.has_goal && k.speed > 0.0f && f % 90 == 0 && next_obstacle < 74) {
                    const V2 fw = dir_from_deg(k.yaw), right{fw.y, -fw.x};
                    UnitDesc x = unit(next_obstacle++, k.pos + fw * 18.0f + right * 14.0f, 2, 1.0f, 6.0f, LocomotorKind::Default);
                    x.owner = 1;
                    CHECK(sys.add_unit(x));
                }
                sys.tick(f, rng, pf);
                const UnitView kv = sys.view(100);
                const V2 fw = dir_from_deg(kv.yaw), right{fw.y, -fw.x};
                for (ObjectId id = 1; id <= 4; ++id) {
                    if (kv.has_goal && f > 40) r.max_err = std::max(r.max_err, length(t14b_slot_point(sys, id) - sys.view(id).pos));
                    const V2 v = sys.team_member_avoid(id, fw);
                    if (length_sq(v) > 0.0f) {
                        ++r.vec_frames;
                        if (std::fabs(dot(v, right)) > std::fabs(r.best_side)) r.best_side = dot(v, right);
                    }
                }
            }
            r.arrived = sys.view(100).arrived;
            StateHasher h;
            sys.hash_units(h);
            r.crc = h.digest();
            return r;
        };
        const Res a = run(serial_for, false);
        const Res b = run(make_thread_pf(4), false);
        const Res off = run(serial_for, true);  // Avoidance_Disabled members: no vector (0x5c6ca0)
        CHECK(a.crc == b.crc && a.vec_frames == b.vec_frames);
        CHECK(a.arrived && off.arrived);
        CHECK(a.vec_frames > 0 && a.best_side < 0.0f);  // a side step to the left: away from the unit on the right
        CHECK(off.vec_frames == 0);
        // the march's slot error comes from the walk states (start delays, turns around the wall: part A); the vector's
        // side steps stay small next to it
        CHECK(a.max_err < 60.0f && a.max_err <= off.max_err + 5.0f);
        std::printf("land squad part B: steering vector - frames with a vector %u, strongest side component %.3f, max slot "
                    "error on the march %.3f (Avoidance_Disabled %.3f), arrived %d\n",
                    a.vec_frames, static_cast<double>(a.best_side), static_cast<double>(a.max_err),
                    static_cast<double>(off.max_err), a.arrived ? 1 : 0);
    }
    {  // the 0x509810 rules on a hand-placed case: no objects -> 0; not a member -> 0
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t14b_squad(sys, at, 1);
        CHECK(sys.team_member_avoid(1, {1.0f, 0.0f}) == V2{});
        CHECK(sys.team_member_avoid(100, {1.0f, 0.0f}) == V2{});
        CHECK(!sys.team_fleeing(1) && !sys.team_fleeing(100));
    }
}

// s78n t16 (FIGHTER_FLYING §7.7): a squad - container 100 (raw Max_Speed `cmax`, MaxJiggleDistance `cjiggle`) and members
// 1..n at `at` + rel[i] (raw Max_Speed mmax[i], OverrideAcceleration = OverrideDeceleration mdecel[i], half-extents 3).
struct T16Member {
    V2 rel;
    float max_speed = 1.0f, decel = 0.0f, spacing = 0.3f;
    float back = 0.0f;  // the member starts this far behind its slot (-x)
};

static void t16_squad(MovementSystem& sys, V2 at, int32_t arrangement, float cmax, float cjiggle, const std::vector<T16Member>& mem,
                      int32_t owner = -1, bool squashable = false) {
    UnitDesc c = unit(100, at, 2, cmax, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = arrangement;
    c.max_jiggle_distance = cjiggle;
    c.accel = 0.1f;  // the container type's own tags are not read (0x5ab540)
    c.decel = 0.1f;
    c.owner = owner;
    c.squashable = squashable;
    CHECK(sys.add_unit(c));
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (size_t i = 0; i < mem.size(); ++i) {
        UnitDesc d = unit(static_cast<ObjectId>(i + 1), at + mem[i].rel + V2{-mem[i].back, 0.0f}, 2, mem[i].max_speed, 3.0f, LocomotorKind::Default);
        d.accel = mem[i].decel;
        d.decel = mem[i].decel;
        d.owner = owner;
        d.formation_spacing = mem[i].spacing;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(d.id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(d.id);
        slots.push_back(mem[i].rel);
    }
    CHECK(sys.set_land_team(100, ids, slots));
    CHECK(sys.set_team(100, 100, 1.0f, cmax));
    for (ObjectId id : ids) CHECK(sys.set_team(id, 100, 1.0f, cmax));
}

// s78n t16 (FIGHTER_FLYING §7.7, S78N_T14_RUN.md):
//  * 0x5ab540: the container accelerates and brakes at the minimum over its members of OverrideDeceleration x (the
//    members' team-capped speed / member Max_Speed) - FoC Stormtrooper squad 0.05 x 1.56 = 0.078 per frame (team speed
//    0.8 x 1.56), Rebel squad 0.07 x 1.56 = 0.1092 (team speed 1.0 x 1.56); the smallest member value wins; a member type
//    without the tag = full speed in one frame; the container's own acceleration is not read;
//  * 0x5ab640: a member more than HX + 2 behind the container slows its target speed linearly to 0 at HX + 12 (HX = the
//    joint slot box half-length + the container's MaxJiggleDistance); [port] not when a wall is between it and its slot.
static void test_land_squad_speed_step() {
    const float mult = 1.56f;  // Object_Max_Speed_Multiplier_Land (the test config's default)
    struct Ramp {
        std::vector<float> v;  // the container's speed after each tick from the order on
        float team = 0.0f;
        bool arrived = false;
    };
    const auto march = [&](int32_t arrangement, float cmax, const std::vector<T16Member>& mem, uint32_t seed) {
        Ramp r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, cell_center(10, 20), arrangement, cmax, 0.0f, mem);
        SimRng rng(seed);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 20);
        sys.move(o, rng, 1);
        for (uint32_t f = 1; f < 900 && !r.arrived; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView k = sys.view(100);
            r.team = k.team_speed;
            r.arrived = k.arrived;
            r.v.push_back(k.speed);
        }
        return r;
    };
    // `steps` exact start steps of `a`; no frame changes the speed by more than `a` (the stop on the last waypoint aside);
    // the cruise is the team speed; the braking before the stop takes about team / a frames.
    const auto check_ramp = [&](const char* name, const Ramp& r, float a, float team, uint32_t steps) {
        CHECK(r.arrived && near(r.team, team, 1e-5f));
        size_t f0 = 0;
        while (f0 < r.v.size() && !(r.v[f0] > 0.0f)) ++f0;
        bool exact = f0 + steps < r.v.size();
        for (uint32_t i = 0; exact && i < steps; ++i) exact = near(r.v[f0 + i], a * static_cast<float>(i + 1), 1e-4f);
        float top = 0.0f, max_step = 0.0f;
        size_t last_cruise = 0;
        for (size_t i = f0; i + 1 < r.v.size(); ++i) {  // the last entry is the arrival frame (speed 0)
            top = std::max(top, r.v[i]);
            if (near(r.v[i], team, 1e-4f)) last_cruise = i;
            if (i > f0) max_step = std::max(max_step, std::fabs(r.v[i] - r.v[i - 1]));
        }
        const size_t brake = r.v.size() - 1 - last_cruise;
        CHECK(exact);
        CHECK(near(top, team, 1e-4f));
        CHECK(max_step <= a + 1e-4f);
        // (the walk stops on the last waypoint from a few steps of speed, so somewhat fewer than team / a frames)
        CHECK(static_cast<float>(brake) >= 0.75f * team / a && static_cast<float>(brake) <= team / a + 6.0f);
        std::printf("land squad speed step: %s - team %.4f, start steps %.4f %.4f %.4f %.4f, largest change %.4f, braking "
                    "frames %zu\n",
                    name, static_cast<double>(r.team), static_cast<double>(r.v[f0]), static_cast<double>(r.v[f0 + 1]),
                    static_cast<double>(r.v[f0 + 2]), static_cast<double>(r.v[f0 + 3]), static_cast<double>(max_step), brake);
    };
    {  // FoC Stormtrooper squad: Rows, 9 x (Max_Speed 1.0, 0.05), container Max_Speed 0.8
        std::vector<T16Member> mem;
        for (int32_t ix = 1; ix >= -1; --ix)
            for (int32_t iy = 1; iy >= -1; --iy)
                mem.push_back({V2{12.0f * static_cast<float>(ix), 12.0f * static_cast<float>(iy)}, 1.0f, 0.05f, 0.3f, 0.0f});
        check_ramp("Stormtrooper squad", march(0, 0.8f, mem, 31), 0.05f * mult, 0.8f * mult, 8);
    }
    {  // FoC Rebel squad: Dartboard, 5 x (Max_Speed 1.0, 0.07), container Max_Speed 1.1
        std::vector<T16Member> mem;
        const V2 rel[5] = {{10.0f, 6.0f}, {10.0f, -6.0f}, {0.0f, 0.0f}, {-10.0f, 6.0f}, {-10.0f, -6.0f}};
        for (const V2& p : rel) mem.push_back({p, 1.0f, 0.07f, 0.5f, 0.0f});
        check_ramp("Rebel squad", march(1, 1.1f, mem, 32), 0.07f * mult, 1.0f * mult, 8);
    }
    {  // mixed members: the smallest 0x373080 wins - 0.06 x (min(2.0, 1.0) x 1.56) / 2.0 = 0.0468 < 0.05 x 1.56
        std::vector<T16Member> mem = {{V2{8.0f, 0.0f}, 1.0f, 0.05f, 0.3f, 0.0f}, {V2{-8.0f, 0.0f}, 2.0f, 0.06f, 0.3f, 0.0f}};
        const Ramp r = march(-1, 3.0f, mem, 33);
        check_ramp("mixed members", r, 0.06f * (1.0f * mult) / 2.0f, 1.0f * mult, 8);
    }
    {  // a member type without OverrideDeceleration gives the full speed (MOVEMENT §3.4), so the tagged member's value wins
        std::vector<T16Member> mem = {{V2{8.0f, 0.0f}, 1.0f, 0.05f, 0.3f, 0.0f}, {V2{-8.0f, 0.0f}, 1.0f, 0.0f, 0.3f, 0.0f}};
        check_ramp("one untagged member", march(-1, 0.8f, mem, 34), 0.05f * mult, 0.8f * mult, 8);
    }
    {  // no member has the tag: the minimum is the full speed -> the team speed in one frame
        std::vector<T16Member> mem = {{V2{8.0f, 0.0f}, 1.0f, 0.0f, 0.3f, 0.0f}, {V2{-8.0f, 0.0f}, 1.0f, 0.0f, 0.3f, 0.0f}};
        const Ramp r = march(-1, 0.8f, mem, 36);
        size_t f0 = 0;
        while (f0 < r.v.size() && !(r.v[f0] > 0.0f)) ++f0;
        CHECK(r.arrived && f0 < r.v.size() && near(r.v[f0], 0.8f * mult, 1e-5f));
    }
    // ---- the straggler cap 0x5ab640: slots (10, 0) and (-10, 0), half-extent 3 -> box [-13, 13]; MaxJiggleDistance 1.5:
    // HX = 14.5. Member 2 is held (speed factor 0) behind the container; the members have no deceleration tag, so the
    // container's speed is its target speed in one frame.
    const float hx = 13.0f + 1.5f;
    const auto straggler = [&](float behind, bool release) {
        struct Out {
            bool formula = true, arrived = false;
            float first = 0.0f, last = 0.0f, gap = 0.0f, team = 0.0f;
        } out;
        MovementSystem sys{MovementConfig{}};
        // (a wall across member 2's way: nothing about passability enters the cap or the member's step)
        PassabilityGrid g = open_grid(100, 100);
        for (int32_t y = 15; y <= 25; ++y) g.set_base(28, y, kPassImpassable);
        sys.set_grid(std::move(g));
        sys.set_anim_length_fn(anim_len);
        const V2 at = cell_center(30, 20);
        const std::vector<T16Member> mem = {{V2{10.0f, 0.0f}, 2.0f, 0.0f, 0.3f, 0.0f},
                                            {V2{-10.0f, 0.0f}, 2.0f, 0.0f, 0.3f, behind - 10.0f}};
        t16_squad(sys, at, -1, 2.0f, 1.5f, mem);
        SimRng rng(35);
        CHECK(sys.set_speed_factor(2, 0.0f));
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(60, 20);
        sys.move(o, rng, 1);
        for (uint32_t f = 1; f < 180; ++f) {
            // the cap from the positions at the start of the tick
            const UnitView k = sys.view(100);
            const V2 fw = dir_from_deg(k.yaw);
            float fac = 1.0f;
            for (ObjectId id = 1; id <= 2; ++id) {
                const V2 d = sys.view(id).pos - k.pos;
                const float p = fw.x * d.x + fw.y * d.y;
                if (p < -hx - 2.0f) fac = std::min(fac, 1.0f - std::clamp(((-hx - 2.0f) - p) / 10.0f, 0.0f, 1.0f));
            }
            sys.tick(f, rng, serial_for);
            const UnitView kv = sys.view(100);
            out.team = kv.team_speed;
            if (f < kv.start_frame) continue;  // the order's start delay
            if (kv.has_goal) out.formula = out.formula && near(kv.speed, fac * kv.team_speed, 1e-4f);
            if (f == kv.start_frame) out.first = kv.speed;
            out.last = kv.speed;
            out.gap = kv.pos.x - sys.view(2).pos.x;
        }
        if (release) {  // the held member walks again: it closes up and the squad arrives
            CHECK(sys.set_speed_factor(2, 1.0f));
            for (uint32_t f = 180; f < 900 && !out.arrived; ++f) {
                sys.tick(f, rng, serial_for);
                out.arrived = sys.view(100).arrived;
            }
        }
        return out;
    };
    {
        const auto half = straggler(hx + 2.0f + 5.0f, true);  // 5 past HX + 2: t = 0.5 on the first moving frame
        CHECK(half.formula && near(half.first, 0.5f * half.team, 1e-4f));
        CHECK(half.last < 1e-3f && near(half.gap, hx + 12.0f, 1e-2f));  // the container waits HX + 12 ahead of it
        CHECK(half.arrived);
        const auto inside = straggler(hx + 1.5f, false);  // inside HX + 2: the full team speed at first
        CHECK(inside.formula && near(inside.first, inside.team, 1e-5f));
        const auto far = straggler(75.0f, true);  // beyond HX + 12 and behind the wall: the container does not start
        CHECK(far.formula && far.first == 0.0f && far.last == 0.0f);
        CHECK(far.arrived);  // released: the member walks through the wall and the squad arrives
    }
}

// The member steering vector 0x509810 (FIGHTER_FLYING §7.5 items 2-4) recomputed from the public state, for a member of
// a squad whose container's collision partners are plain units (no other squad): per record of the container in creation
// order (live + resolved this tick) the weight (MovingVsMoving 1, MovingVsStopped 2, StoppedVsStopped 3) and the vector
// (MovingVsStopped only: the side step 0x5c71a0 away from the other participant x clamp(speed / max speed)); best = the
// largest weight, a tie the longer vector; + every other vector with dot(best, v) >= -0.5. Open ground: every keep probe
// passes. `added` / `dropped` count the non-zero other vectors the dot rule took / left out.
static V2 t14b_avoid_ref(const MovementSystem& sys, ObjectId member, V2 dir, float vmax, uint32_t& added, uint32_t& dropped) {
    ObjectId k = -1;
    V2 slot;
    if (!sys.team_slot(member, k, slot)) return {};
    std::vector<CollisionObject> recs;
    for (const CollisionObject& c : sys.collisions().list())
        if (c.a == k || c.b == k) recs.push_back(c);
    std::sort(recs.begin(), recs.end(), [](const CollisionObject& x, const CollisionObject& y) { return x.seq < y.seq; });
    const UnitView a = sys.view(member);
    const float ratio = vmax > 0.0f ? std::clamp(a.speed / vmax, 0.0f, 1.0f) : 0.0f;
    const float ra = 3.0f;  // s78n t18: the member's soft footprint radius (0x397780 of a unit that is no container)
    std::vector<V2> vecs;
    std::vector<float> weights;
    for (const CollisionObject& c : recs) {
        const ObjectId other = c.a == k ? c.b : c.a;
        CHECK(!sys.is_land_team_container(other));
        V2 v{};
        // s78n t18: MovingVsStopped - the obstacle side always, the mover side only in Repulsion; MovingVsMoving - Repulse
        const bool gives = c.kind == CollisionKind::MovingVsStopped   ? c.b == k || c.state == CollisionState::Repulsion
                           : c.kind == CollisionKind::MovingVsMoving ? c.state == CollisionState::Repulsion
                                                                     : false;
        if (gives) {
            const UnitView b = sys.view(other);
            const float reach = (ra + 12.0f) * 1.3f;  // the obstacles of this test have radius 12 and no hard extents
            const float t = std::clamp(1.0f - length(a.pos - b.pos) / reach, 0.0f, 1.0f);
            if (t > 0.0f) v = (cross(dir, b.pos - a.pos) < 0.0f ? V2{-dir.y, dir.x} : V2{dir.y, -dir.x}) * t * ratio;
        }
        vecs.push_back(v);
        weights.push_back(c.kind == CollisionKind::MovingVsMoving ? 1.0f : c.kind == CollisionKind::MovingVsStopped ? 2.0f : 3.0f);
    }
    if (vecs.empty()) return {};
    size_t best = 0;
    for (size_t i = 1; i < vecs.size(); ++i)
        if (weights[i] > weights[best] || (weights[i] == weights[best] && length_sq(vecs[i]) > length_sq(vecs[best]))) best = i;
    V2 out = vecs[best];
    for (size_t j = 0; j < vecs.size(); ++j) {
        if (j == best) continue;
        const bool take = dot(vecs[best], vecs[j]) >= -0.5f;
        if (take) out = out + vecs[j];
        if (length_sq(vecs[j]) > 0.0f) ++(take ? added : dropped);
    }
    return out;
}

// s78n review of t14 part B / t16:
//  * fleeing: Fleeing_Infantry_Speed_Bonus (2) on the container's acceleration and turn rate and on the members'
//    acceleration and turn rate (the x 2 on the speeds is pinned in test_land_squad_part_b);
//  * the MovingVsMoving crush pair flees only when the squad's owner scatters (set_scatters_fn), else it is pushed;
//  * set_collision_exempt: no collision object with the exempt unit, and the container walks through it;
//  * the steering vector's weights and dot >= -0.5 rule against a recomputation;
//  * a pushed walk cut short on a container with no order leaves it stopped: its members recover to their slots.
static void test_land_squad_review_fixes() {
    const float mult = 1.56f;
    const auto grid_squad = [](MovementSystem& sys, V2 at, float decel, int32_t owner) {  // 2 x 2, Rows, Max_Speed 2
        std::vector<T16Member> mem;
        for (float x : {8.0f, -8.0f})
            for (float y : {8.0f, -8.0f}) mem.push_back({V2{x, y}, 2.0f, decel, 0.5f, 0.0f});
        t16_squad(sys, at, 0, 2.0f, 0.0f, mem, owner, true);
    };
    {  // fleeing x 2 on acceleration and turn rate
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(30, 20);
        grid_squad(sys, at, 0.1f, 1);
        SimRng rng(41);
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
        const float a = 0.1f * (2.0f * mult) / 2.0f;  // 0x5ab540 and each member's own acceleration: 0.156
        const float rate = 6.0f * mult;               // the fixture's Max_Rate_Of_Turn x the mode multiplier
        float k_dv = 0.0f, k_dyaw = 0.0f, m_dv = 0.0f, m_dyaw = 0.0f;
        uint32_t flee_frames = 0;
        for (uint32_t f = 20; f < 700; ++f) {
            UnitView before[5];
            before[0] = sys.view(100);
            for (ObjectId id = 1; id <= 4; ++id) before[id] = sys.view(id);
            const bool was = sys.team_fleeing(100);
            sys.tick(f, rng, serial_for);
            if (!was || !sys.team_fleeing(100)) continue;  // frames walked wholly on the crush-flee path
            ++flee_frames;
            const UnitView k = sys.view(100);
            k_dv = std::max(k_dv, k.speed - before[0].speed);
            k_dyaw = std::max(k_dyaw, std::fabs(wrap_deg(k.yaw - before[0].yaw)));
            for (ObjectId id = 1; id <= 4; ++id) {
                const UnitView m = sys.view(id);
                m_dv = std::max(m_dv, m.speed - before[id].speed);
                m_dyaw = std::max(m_dyaw, std::fabs(wrap_deg(m.yaw - before[id].yaw)));
            }
        }
        CHECK(flee_frames > 3);
        CHECK(near(k_dv, 2.0f * a, 1e-4f));        // the container's acceleration x 2
        CHECK(near(k_dyaw, 2.0f * rate, 1e-3f));   // its turn rate x 2
        CHECK(near(m_dv, 2.0f * a, 1e-4f));        // a member's acceleration x 2
        CHECK(m_dyaw > rate + 1e-3f && m_dyaw <= 2.0f * rate + 1e-3f);  // and its turn rate
        std::printf("land squad review fixes: flee %u frames - container step %.4f (2 x %.4f), turn %.3f (2 x %.3f); member step "
                    "%.4f, turn %.3f\n",
                    flee_frames, static_cast<double>(k_dv), static_cast<double>(a), static_cast<double>(k_dyaw),
                    static_cast<double>(rate), static_cast<double>(m_dv), static_cast<double>(m_dyaw));
    }
    {  // MovingVsMoving crush pair: crush-flee only when the owner's faction scatters
        const auto run = [&](bool scatters) {
            std::pair<uint32_t, uint32_t> r{0, 0};  // pushed frames, fleeing frames
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            sys.set_scatters_fn([scatters](int32_t) { return scatters; });
            grid_squad(sys, cell_center(15, 20), 0.0f, 1);
            UnitDesc cr = unit(60, cell_center(60, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
            cr.collision_class = kUccVehicle;
            cr.crusher = true;
            cr.owner = 2;
            cr.yaw_deg = 180.0f;
            CHECK(sys.add_unit(cr));
            SimRng rng(42);
            MoveOrder o;
            o.units = {1};  // the squad marches +x, the crusher -x on the same line: both moving when they meet
            o.destination = cell_center(60, 20);
            sys.move(o, rng, 1);
            MoveOrder co;
            co.units = {60};
            co.destination = cell_center(15, 20);
            sys.move(co, rng, 1);
            for (uint32_t f = 1; f < 600; ++f) {
                sys.tick(f, rng, serial_for);
                if (sys.view(100).pushed) ++r.first;
                if (sys.team_fleeing(100)) ++r.second;
            }
            return r;
        };
        const auto on = run(true), off = run(false);
        CHECK(on.first > 0 && on.second > 0);     // Scatters_From_Crushers: pushed on a crush-flee path
        CHECK(off.first > 0 && off.second == 0);  // not scattering: pushed aside, no flee
        std::printf("land squad review fixes: moving crush pair - scatters: pushed %u fleeing %u; not: pushed %u fleeing %u\n",
                    on.first, on.second, off.first, off.second);
    }
    {  // set_collision_exempt
        struct Out {
            uint32_t objects = 0;  // collision objects (live or resolved) of the container with the host
            float off_line = 0.0f;
            bool arrived = false;
        };
        const auto run = [&](ObjectId exempt) {
            Out r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            const V2 at = cell_center(15, 20);
            grid_squad(sys, at, 0.0f, 1);
            UnitDesc host = unit(70, cell_center(30, 20), 2, 1.0f, 15.0f, LocomotorKind::Default);
            host.collision_class = kUccVehicle;
            host.owner = 1;
            CHECK(sys.add_unit(host));
            CHECK(!sys.set_collision_exempt(1, 70) && !sys.set_collision_exempt(70, 100));  // squad containers only
            SimRng rng(43);
            MoveOrder o;
            o.units = {1};
            o.destination = cell_center(45, 20);  // straight through the host
            sys.move(o, rng, 1);
            for (uint32_t f = 1; f < 600; ++f) {
                CHECK(sys.set_collision_exempt(100, exempt));  // host-set every tick
                sys.tick(f, rng, serial_for);
                for (const CollisionObject& c : sys.collisions().list())
                    if ((c.a == 100 && c.b == 70) || (c.a == 70 && c.b == 100)) ++r.objects;
                r.off_line = std::max(r.off_line, std::fabs(sys.view(100).pos.y - at.y));
            }
            r.arrived = sys.view(100).arrived && length(sys.view(100).pos - cell_center(45, 20)) < 1.0f;
            return r;
        };
        const Out none = run(-1), hosted = run(70), all = run(MovementSystem::kCollisionExemptAll);
        CHECK(none.objects > 0);  // without it the container collides with the unit on its path
        CHECK(hosted.objects == 0 && hosted.off_line < 0.01f && hosted.arrived);  // exempt: no object, straight through
        CHECK(all.objects == 0 && all.off_line < 0.01f && all.arrived);     // no land collider at all
        std::printf("land squad review fixes: collision exempt - objects with the host %u / %u / %u (none / host / all), off "
                    "its line %.2f / %.2f / %.2f\n",
                    none.objects, hosted.objects, all.objects, static_cast<double>(none.off_line),
                    static_cast<double>(hosted.off_line), static_cast<double>(all.off_line));
    }
    {  // the steering vector against the recomputation: weights, the longer-vector tie, dot >= -0.5
        struct Out {
            uint32_t added = 0, dropped = 0, frames = 0;
            bool placed = false, same = true;
            float worst = 0.0f;
        };
        // Once the squad cruises, two stopped units (radius 12) appear 5 ahead of member 1, `side` to its left and right
        // (it walks between them): two MovingVsStopped objects of the container with opposed side steps for member 1.
        const auto run = [&](float side) {
            Out r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            grid_squad(sys, cell_center(15, 20), 0.0f, 1);
            SimRng rng(44);
            MoveOrder o;
            o.units = {1};
            o.destination = cell_center(70, 20);
            sys.move(o, rng, 1);
            uint32_t placed_at = 0;
            for (uint32_t f = 1; f < 400; ++f) {
                const UnitView k0 = sys.view(100);
                if (placed_at == 0 && k0.has_goal && near(k0.speed, k0.team_speed, 1e-4f) &&
                    near(sys.view(1).speed, k0.speed, 1e-3f) && k0.pos.x > cell_center(22, 20).x) {
                    const V2 fw0 = dir_from_deg(k0.yaw), right{fw0.y, -fw0.x};
                    const V2 p = sys.view(1).pos;
                    for (int32_t sgn : {1, -1}) {
                        UnitDesc x = unit(sgn > 0 ? 70 : 71, p + fw0 * 5.0f + right * (side * static_cast<float>(sgn)), 2, 1.0f, 12.0f,
                                          LocomotorKind::Default);
                        x.owner = 1;
                        CHECK(sys.add_unit(x));
                    }
                    placed_at = f;
                }
                sys.tick(f, rng, serial_for);
                if (placed_at == 0 || f > placed_at + 40) continue;
                const V2 fw = dir_from_deg(sys.view(100).yaw);
                for (ObjectId id = 1; id <= 4; ++id) {
                    const V2 ref = t14b_avoid_ref(sys, id, fw, 2.0f * mult, r.added, r.dropped);
                    const V2 got = sys.team_member_avoid(id, fw);
                    const float err = length(ref - got);
                    r.worst = std::max(r.worst, err);
                    r.same = r.same && err < 1e-4f;
                    if (length_sq(got) > 0.0f) ++r.frames;
                }
            }
            r.placed = placed_at != 0;
            return r;
        };
        const Out tight = run(3.0f), wide = run(8.0f);
        CHECK(tight.placed && wide.placed && tight.same && wide.same);
        CHECK(tight.frames > 0 && tight.dropped > 0);  // close on both sides: dot(best, other) < -0.5, only the longer one
        CHECK(wide.frames > 0 && wide.added > 0);      // further apart: the opposed step is added
        std::printf("land squad review fixes: steering vector = recomputation (worst %.6f / %.6f); units 3 to each side: %u "
                    "frames, opposed added %u dropped %u; 8 to each side: %u frames, added %u dropped %u\n",
                    static_cast<double>(tight.worst), static_cast<double>(wide.worst), tight.frames, tight.added, tight.dropped,
                    wide.frames, wide.added, wide.dropped);
    }
    {  // a pushed walk cut short (10 blocked frames) on a container with no order: stopped, so its members recover
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 at = cell_center(30, 20);
        grid_squad(sys, at, 0.0f, 1);
        SimRng rng(45);
        for (uint32_t f = 1; f < 20; ++f) sys.tick(f, rng, serial_for);
        UnitDesc g = unit(50, cell_center(10, 20), 2, 1.0f, 20.0f, LocomotorKind::Default);
        g.collision_class = kUccGiantVehicle;
        g.owner = 2;
        CHECK(sys.add_unit(g));
        MoveOrder o;
        o.units = {50};
        o.destination = cell_center(50, 20);
        sys.move(o, rng, 20);
        uint32_t f = 20, cut_at = 0;
        for (; f < 900 && !sys.view(100).pushed; ++f) sys.tick(f, rng, serial_for);
        CHECK(sys.view(100).pushed);
        // wall the container in: its pushed walk is blocked from now on
        const V2 kp = sys.view(100).pos;
        const int32_t cx = world_to_cell(kp.x), cy = world_to_cell(kp.y);
        for (int32_t y = cy - 2; y <= cy + 2; ++y)
            for (int32_t x = cx - 2; x <= cx + 2; ++x) sys.grid().set_base(x, y, kPassImpassable);
        const uint32_t blocked0 = sys.stats().blocked_steps;
        for (uint32_t n = 0; n < 40 && sys.view(100).pushed; ++n, ++f) sys.tick(f, rng, serial_for);
        cut_at = f;
        CHECK(!sys.view(100).pushed && sys.stats().blocked_steps >= blocked0 + 10);
        CHECK(sys.remove_unit(50));  // the pusher goes: the squad stands alone, with no order
        CHECK(!sys.view(100).has_goal && sys.view(100).speed == 0.0f);
        // a member knocked off its slot now recovers (the recovery decision runs only while the container is stopped)
        const V2 m1 = sys.view(1).pos;
        CHECK(sys.teleport(1, {m1.x + 30.0f, m1.y + 30.0f}, 0.0f, 0.0f, f));
        bool recovering = false;
        for (uint32_t n = 0; n < 200; ++n, ++f) {
            sys.tick(f, rng, serial_for);
            recovering = recovering || sys.view(1).has_goal;
        }
        CHECK(recovering && !sys.view(1).has_goal && length(t14b_slot_point(sys, 1) - sys.view(1).pos) <= 4.0f);
        std::printf("land squad review fixes: pushed walk cut at frame %u, member recovered %d\n", cut_at, recovering ? 1 : 0);
    }
}

// s78n t18 item 8 (FIGHTER_FLYING §7.4, 0x3abb80): a teleport of the container puts every member at the new place + its
// old offset from the container, with its locomotor idle; member facings are not written; a parked member's stored
// offset is not touched.
static void test_land_squad_teleport() {
    MovementSystem sys{MovementConfig{}};
    land_system(sys, 100);
    const V2 at = cell_center(20, 20);
    t14b_squad(sys, at, 1);
    SimRng rng(51);
    MoveOrder o;
    o.units = {1};
    o.destination = cell_center(40, 24);
    sys.move(o, rng, 1);
    for (uint32_t f = 1; f < 100; ++f) sys.tick(f, rng, serial_for);  // marching (an early order starts at frame 61)
    CHECK(sys.view(100).speed > 0.0f && sys.view(1).speed > 0.0f);
    V2 off4;
    CHECK(sys.park_team_member(4) && sys.parked_offset(4, off4));
    const UnitView k0 = sys.view(100);
    UnitView m0[4];
    for (ObjectId id = 1; id <= 3; ++id) m0[id] = sys.view(id);
    const V2 to = cell_center(60, 50);
    CHECK(sys.teleport(100, to, 0.0f, 90.0f, 100));
    const UnitView k1 = sys.view(100);
    CHECK(k1.pos == to && k1.yaw == 90.0f && k1.speed == 0.0f && !k1.has_goal);
    for (ObjectId id = 1; id <= 3; ++id) {
        const UnitView m = sys.view(id);
        const V2 want = to + (m0[id].pos - k0.pos);  // the same expression the system evaluates
        CHECK(m.pos == want && m.speed == 0.0f && m.yaw == m0[id].yaw);
    }
    V2 off4b;
    CHECK(sys.parked_offset(4, off4b) && off4b == off4);
    // a member in recovery (0x2A) has it cleared by the teleport
    for (uint32_t f = 100; f < 110; ++f) sys.tick(f, rng, serial_for);
    const V2 m1 = sys.view(1).pos;
    CHECK(sys.teleport(1, {m1.x + 40.0f, m1.y}, 0.0f, 0.0f, 110));
    bool recovering = false;
    for (uint32_t f = 110; f < 210 && !recovering; ++f) {
        sys.tick(f, rng, serial_for);
        recovering = sys.view(1).has_goal && sys.view(1).speed > 0.0f;
    }
    CHECK(recovering);
    const V2 kp = sys.view(100).pos, mp = sys.view(1).pos;
    const V2 to2 = cell_center(30, 60);
    CHECK(sys.teleport(100, to2, 0.0f, 90.0f, 210));
    CHECK(sys.view(1).pos == to2 + (mp - kp) && sys.view(1).speed == 0.0f && !sys.view(1).has_goal);
    // a suspended member (a hero clash) is a TEAM entry: it moves with the container too
    CHECK(sys.suspend_team_member(2));
    const V2 kp2 = sys.view(100).pos, sp2 = sys.view(2).pos;
    CHECK(sys.teleport(100, at, 0.0f, 0.0f, 211));
    CHECK(sys.view(2).pos == at + (sp2 - kp2));
    std::printf("land squad teleport: members keep their offsets, speeds 0, facings kept, parked offset kept\n");
}

// ---- s78n t18 ------------------------------------------------------------------------------------------------------
// FoC squads of the t16 fixtures: Stormtrooper (Rows 3 x 3, slots at pitch 12, half-extents 3, Max_Speed 1.0 capped by
// the container's 0.8, OverrideDeceleration 0.05: team speed 1.248, rate 0.078) and Rebel (Dartboard, 5 members, rate
// 0.1092, team speed 1.56).
static std::vector<T16Member> t18_storm() {
    std::vector<T16Member> mem;
    for (int32_t ix = 1; ix >= -1; --ix)
        for (int32_t iy = 1; iy >= -1; --iy)
            mem.push_back({V2{12.0f * static_cast<float>(ix), 12.0f * static_cast<float>(iy)}, 1.0f, 0.05f, 0.3f, 0.0f});
    return mem;
}

// s78n t18 items 2 / 6 (MOVEMENT §5.3.1 / §5.3.2, FIGHTER_FLYING §7.8): the squad's footprint is its live joint slot box
// (0x404450) - a rectangle centred on the container and turned by its yaw; members are in no collision or occupancy query,
// the container is in all of them with that rectangle.
static void test_land_squad_footprint() {
    const V2 at = cell_center(10, 20);
    {  // the joint box: slots at pitch 12 with half-extents 3 -> (15, 15); SPREAD_OUT x 1.8 on the spacing; a lost row
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm());
        float hx = 0.0f, hy = 0.0f;
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 15.0f, 1e-4f) && near(hy, 15.0f, 1e-4f));
        CHECK(!sys.team_joint_box(1, hx, hy));  // not a container
        SimRng rng(61);
        sys.tick(1, rng, serial_for);  // the first layout (Rows, the container's facing): the same grid
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 15.0f, 1e-3f) && near(hy, 15.0f, 1e-3f));
        CHECK(sys.set_team_spread_out(100, true));
        sys.tick(2, rng, serial_for);  // re-laid: pitch 6 + 0.3 x 20 x 1.8 = 16.8 -> 16.8 + 3
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 19.8f, 1e-3f) && near(hy, 19.8f, 1e-3f));
        CHECK(sys.set_team_spread_out(100, false));
        sys.tick(3, rng, serial_for);
        CHECK(sys.team_joint_box(100, hx, hy) && near(hx, 15.0f, 1e-3f));
        // the three members of the front row (slot x = +12) go: x from -15 to max(0, 0 + 3) -> HX 9; y unchanged
        uint32_t gone = 0;
        for (const auto& [id, slot] : sys.land_team_slots(100))
            if (slot.x > 6.0f) {
                CHECK(sys.remove_unit(id));
                ++gone;
            }
        CHECK(gone == 3 && sys.team_joint_box(100, hx, hy) && near(hx, 9.0f, 1e-3f) && near(hy, 15.0f, 1e-3f));
    }
    // A container marching +x past a stopped Vehicle-class unit of radius 20 (no hard extents: a 20 x 20 footprint): a
    // collision object only when the unit is nearer the path than 15 + 20 (the circle of the box's radius 21.2 would
    // reach 41.2); with SPREAD_OUT the box is 19.8: 39.8.
    const auto pass = [&](float lateral, bool spread) {
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm());
        UnitDesc v = unit(70, {at.x + 200.0f, at.y + lateral}, 2, 1.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        CHECK(sys.add_unit(v));
        SimRng rng(62);
        CHECK(sys.set_team_spread_out(100, spread));
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 400.0f, at.y};
        sys.move(o, rng, 2);
        for (uint32_t f = 2; f < 500; ++f) {
            CHECK(sys.set_team_spread_out(100, spread));
            sys.tick(f, rng, serial_for);
        }
        return sys.stats().collisions_created;  // (the only possible pair: the container and the unit)
    };
    const uint32_t far = pass(38.0f, false), near_by = pass(33.0f, false), spread = pass(38.0f, true);
    CHECK(far == 0 && near_by > 0 && spread > 0);
    std::printf("land squad footprint: objects with a unit 38 / 33 beside the path %u / %u, 38 with SPREAD_OUT %u\n", far, near_by,
                spread);
    {  // occupancy (0x538200, the push spot's box query): a squad member is no occupant, a container's box is
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm());
        UnitDesc j = unit(80, cell_center(30, 30), 0, 2.0f, 6.0f, LocomotorKind::Jetpack);
        CHECK(sys.add_unit(j));
        SimRng rng(63);
        sys.tick(1, rng, serial_for);
        // members of a squad whose container stands far away: a second squad is not needed - move the container off
        CHECK(sys.teleport(100, cell_center(60, 60), 0.0f, 0.0f, 1));  // members go with it (item 8)
        // put member 1 alone at a spot and ask whether the jetpack may land there
        const V2 spot = cell_center(20, 30);
        CHECK(sys.teleport(1, spot, 0.0f, 0.0f, 1));
        CHECK(sys.land_spot_clear(80, spot));                       // only a squad member stands there: free
        CHECK(!sys.land_spot_clear(80, cell_center(60, 60)));       // the container's point: inside its joint box
        const V2 edge{cell_center(60, 60).x + 15.0f + 6.0f - 1.0f, cell_center(60, 60).y};  // box 15 + lander 6, overlapping by 1
        CHECK(!sys.land_spot_clear(80, edge));
        const V2 off{cell_center(60, 60).x + 15.0f + 6.0f + 1.0f, cell_center(60, 60).y};
        CHECK(sys.land_spot_clear(80, off));
    }
}

// A second FoC-sized squad for the s78n t18 tests: container `cid` (Rows, Max_Speed 0.8, Infantry, squashable) facing
// `yaw` with 2 x 2 members cid + 1 .. cid + 4 (Max_Speed 1.0, OverrideDeceleration 0.05, half-extents 3, slots +-6).
static void t18_squad(MovementSystem& sys, ObjectId cid, V2 at, float yaw, int32_t owner, float member_speed = 1.0f) {
    const float cspeed = member_speed > 0.0f ? 0.8f : 0.0f;  // (Max_Speed 0: a squad that cannot be pushed)
    UnitDesc c = unit(cid, at, 2, cspeed, 12.0f, LocomotorKind::Default);
    c.team_container = true;
    c.container_arrangement = 0;
    c.yaw_deg = yaw;
    c.owner = owner;
    c.squashable = true;
    CHECK(sys.add_unit(c));
    const V2 f = dir_from_deg(yaw), l{-f.y, f.x};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (int32_t i = 0; i < 4; ++i) {
        const V2 s{i < 2 ? 6.0f : -6.0f, i % 2 == 0 ? 6.0f : -6.0f};
        UnitDesc d = unit(cid + 1 + i, at + f * s.x + l * s.y, 2, member_speed, 3.0f, LocomotorKind::Default);
        d.accel = d.decel = member_speed > 0.0f ? 0.05f : 0.0f;
        d.yaw_deg = yaw;
        d.owner = owner;
        d.formation_spacing = 0.3f;
        CHECK(sys.add_unit(d));
        CHECK(sys.set_team_member_shape(d.id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(d.id);
        slots.push_back(s);
    }
    CHECK(sys.set_land_team(cid, ids, slots));
    CHECK(sys.set_team(cid, cid, 1.0f, cspeed));
    for (ObjectId id : ids) CHECK(sys.set_team(id, cid, 1.0f, cspeed));
}

// s78n t18 items 1 / 7 (MOVEMENT §5.4.1 / §5.4.2, FIGHTER_FLYING §7.8 item 5): what a squad container does in its
// collision records.
//  * moving against a standing Infantry-class unit with a free destination: Repulsion - it walks on along its path at
//    full speed, its members side-step, the obstacle stays; the record ends when the obstacle is behind by the two radii;
//  * with a blocked destination: the early-arrival cut (its box (HX + 5, HY) clear of the standing unit);
//  * two squads on opposed headings: Repulse (both at full speed, no hold, members' vectors); on the same heading: Speed
//    Slave Only (the rear one follows);
//  * against a standing vehicle: Path Around, never a shove; a crusher's one push attempt, then it drives on;
//  * a failed resolution: Try Again Later - speed 0 at the braking rate, re-decided every 30 frames;
//  * a single infantry unit against single infantry: Repulsion at once, the vector bends its own heading.
static void test_land_squad_repulsion() {
    const float a = 0.05f * 1.56f, vmax = 0.8f * 1.56f;
    const V2 at = cell_center(10, 20);
    struct Out {
        uint64_t crc = 0;
        float off_line = 0.0f, min_cruise = 3.0e38f, obstacle_moved = 0.0f;
        uint32_t repulsion_frames = 0, vec_frames = 0, records_after = 0, other_states = 0;
        bool arrived = false;
        MovementStats st;
    };
    // obstacle: 0 a standing infantry unit dead ahead, 1 a standing second squad (container 200, members 201..204)
    const auto through = [&](int obstacle, const ParallelFor& pf) {
        Out r;
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const V2 op{at.x + 200.0f, at.y};
        if (obstacle == 0) {
            UnitDesc x = unit(70, op, 2, 1.0f, 6.0f, LocomotorKind::Default);
            x.owner = 1;
            CHECK(sys.add_unit(x));
        } else {
            t18_squad(sys, 200, op, 90.0f, 1);
        }
        const ObjectId oid = obstacle == 0 ? 70 : 200;
        SimRng rng(71);
        sys.tick(1, rng, pf);
        const V2 o0 = sys.view(oid).pos;
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 500.0f, at.y};
        sys.move(o, rng, 2);
        bool cruising = false;
        for (uint32_t f = 2; f < 900 && !r.arrived; ++f) {
            sys.tick(f, rng, pf);
            const UnitView k = sys.view(100);
            r.arrived = k.arrived;
            r.off_line = std::max(r.off_line, std::fabs(k.pos.y - at.y));
            if (near(k.speed, vmax, 1e-5f)) cruising = true;
            if (cruising && k.pos.x < at.x + 440.0f) r.min_cruise = std::min(r.min_cruise, k.speed);
            bool rep = false;
            for (const CollisionObject& c : sys.collisions().list())
                if (c.a == 100 && c.b == oid) {
                    if (c.kind == CollisionKind::MovingVsStopped && c.state == CollisionState::Repulsion) rep = true;
                    else ++r.other_states;
                    if (k.pos.x > op.x + 80.0f) ++r.records_after;
                }
            if (rep) ++r.repulsion_frames;
            const V2 fw = dir_from_deg(k.yaw);
            for (ObjectId id = 1; id <= 9; ++id)
                if (length_sq(sys.team_member_avoid(id, fw)) > 0.0f) ++r.vec_frames;
        }
        r.obstacle_moved = length(sys.view(oid).pos - o0);
        r.st = sys.stats();
        StateHasher h;
        sys.hash_units(h);
        r.crc = h.digest();
        return r;
    };
    for (int obstacle = 0; obstacle < 2; ++obstacle) {
        const Out s = through(obstacle, serial_for), p = through(obstacle, make_thread_pf(4));
        CHECK(s.crc == p.crc);
        CHECK(s.arrived && s.off_line < 1e-4f);                 // on its path, straight through the footprint
        CHECK(near(s.min_cruise, vmax, 1e-5f));                 // never slowed
        CHECK(s.repulsion_frames > 0 && s.other_states == 0 && s.records_after == 0);  // Repulsion while passing, then gone
        CHECK(s.vec_frames > 0);                                // the members side-step
        CHECK(s.obstacle_moved == 0.0f);                        // the standing unit / squad stays where it is
        CHECK(s.st.dynamic_repaths == 0 && s.st.pushes == 0 && s.st.early_arrivals == 0);
        std::printf("land squad repulsion: obstacle %d - Repulsion %u frames, member vectors %u, off its line %.5f\n", obstacle,
                    s.repulsion_frames, s.vec_frames, static_cast<double>(s.off_line));
    }
    {  // blocked destination: a standing infantry unit on the spot the squad is ordered to -> the early-arrival cut
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const V2 goal = cell_center(25, 20);
        UnitDesc x = unit(70, goal, 2, 1.0f, 6.0f, LocomotorKind::Default);
        x.owner = 1;
        CHECK(sys.add_unit(x));
        SimRng rng(72);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = goal;
        sys.move(o, rng, 2);
        bool arrived = false;
        for (uint32_t f = 2; f < 900 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
        }
        const UnitView k = sys.view(100);
        // the box (15 + 5, 15) at the stop point clears the unit's circle of 6: the centre at least 26 before it; the
        // search steps back by 5 and the cut takes 3 more, the walk stops within 5 of the cut path's end
        const float gap = goal.x - k.pos.x;
        CHECK(arrived && sys.stats().early_arrivals == 1 && sys.stats().dynamic_repaths == 0 && sys.stats().pushes == 0);
        CHECK(gap >= 26.0f && gap < 26.0f + 5.0f + 3.0f + 5.0f + 1.0f && std::fabs(k.pos.y - goal.y) < 1e-3f);
        CHECK(sys.view(70).pos == goal);
        std::printf("land squad repulsion: blocked destination - stopped %.2f before the standing unit\n", static_cast<double>(gap));
    }
    {  // two squads head-on (MovingVsMoving, opposed headings): Repulse - both on their lines at full speed, both arrive
        struct Res {
            uint64_t crc = 0;
            float off = 0.0f, min_a = 3.0e38f, min_b = 3.0e38f;
            uint32_t repulse = 0, other = 0, vec = 0;
            bool arrived = false;
        };
        const auto run = [&](const ParallelFor& pf) {
            Res r;
            MovementSystem sys{MovementConfig{}};
            land_system(sys, 100);
            const V2 pa = cell_center(10, 20), pb = cell_center(35, 20);
            t18_squad(sys, 100, pa, 0.0f, 1);
            t18_squad(sys, 200, pb, 180.0f, 1);
            SimRng rng(73);
            sys.tick(1, rng, pf);
            MoveOrder oa, ob;
            oa.units = {101};
            oa.destination = pb;
            ob.units = {201};
            ob.destination = pa;
            sys.move(oa, rng, 2);
            sys.move(ob, rng, 2);
            bool ca = false, cb = false;
            for (uint32_t f = 2; f < 900 && !r.arrived; ++f) {
                sys.tick(f, rng, pf);
                const UnitView ka = sys.view(100), kb = sys.view(200);
                r.arrived = ka.arrived && kb.arrived;
                r.off = std::max(r.off, std::max(std::fabs(ka.pos.y - pa.y), std::fabs(kb.pos.y - pa.y)));
                ca = ca || near(ka.speed, vmax, 1e-5f);
                cb = cb || near(kb.speed, vmax, 1e-5f);
                if (ca && ka.pos.x < pb.x - 60.0f) r.min_a = std::min(r.min_a, ka.speed);
                if (cb && kb.pos.x > pa.x + 60.0f) r.min_b = std::min(r.min_b, kb.speed);
                for (const CollisionObject& c : sys.collisions().list()) {
                    if (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::Repulsion) ++r.repulse;
                    else ++r.other;
                }
                const V2 fw = dir_from_deg(ka.yaw);
                for (ObjectId id = 101; id <= 104; ++id)
                    if (length_sq(sys.team_member_avoid(id, fw)) > 0.0f) ++r.vec;
            }
            StateHasher h;
            sys.hash_units(h);
            r.crc = h.digest();
            return r;
        };
        const Res s = run(serial_for), p = run(make_thread_pf(4));
        CHECK(s.crc == p.crc);
        CHECK(s.arrived && s.off < 1e-4f && s.repulse > 0 && s.other == 0 && s.vec > 0);
        CHECK(near(s.min_a, vmax, 1e-5f) && near(s.min_b, vmax, 1e-5f));  // neither is slowed or held
        std::printf("land squad repulsion: head-on squads - Repulse %u frames, member vectors %u\n", s.repulse, s.vec);
    }
    {  // two squads on the same heading, the rear one faster: Speed Slave Only - it follows and never overlaps
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        const V2 pa = cell_center(10, 20), pb = cell_center(14, 20);
        t18_squad(sys, 100, pa, 0.0f, 1);            // rear: team speed 0.8 x 1.56
        t18_squad(sys, 200, pb, 0.0f, 1, 0.4f);      // front: its members cap the team at 0.4 x 1.56
        SimRng rng(74);
        sys.tick(1, rng, serial_for);
        MoveOrder oa, ob;
        oa.units = {101};
        oa.destination = cell_center(45, 20);
        ob.units = {201};
        ob.destination = cell_center(48, 20);
        sys.move(oa, rng, 2);
        sys.move(ob, rng, 2);
        uint32_t slave = 0, slowed = 0;
        float min_gap = 3.0e38f;
        bool wrong_master = false;
        for (uint32_t f = 2; f < 500; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView ka = sys.view(100), kb = sys.view(200);
            min_gap = std::min(min_gap, kb.pos.x - ka.pos.x);
            for (const CollisionObject& c : sys.collisions().list())
                if (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::SpeedSlave) {
                    ++slave;
                    wrong_master = wrong_master || (c.master == 0 ? c.a : c.b) != 200;  // the one farther ahead
                    if (ka.speed < vmax - 1e-3f && kb.speed > 0.0f) ++slowed;
                }
        }
        CHECK(slave > 0 && !wrong_master && slowed > 0);
        CHECK(min_gap > 0.0f);  // the rear container never passes the front one
        std::printf("land squad repulsion: following squads - Speed Slave Only %u frames, rear slowed %u, closest %.2f\n", slave,
                    slowed, static_cast<double>(min_gap));
    }
    {  // against a standing friendly vehicle on the path (destination free): Path Around - the vehicle is never moved, the
       // mover's members get no vector from that record (the mover side gives one only in Repulsion)
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        const V2 vp{at.x + 200.0f, at.y};
        UnitDesc v = unit(70, vp, 2, 1.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        CHECK(sys.add_unit(v));
        SimRng rng(75);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 500.0f, at.y};
        sys.move(o, rng, 2);
        bool arrived = false, vectors = false, around = false;
        for (uint32_t f = 2; f < 1200 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            arrived = sys.view(100).arrived;
            for (const CollisionObject& c : sys.collisions().list()) {
                CHECK(c.state != CollisionState::Repulsion);  // non-infantry pairs never get Repulsion
                around = around || c.state == CollisionState::PathAround;
            }
            const V2 fw = dir_from_deg(sys.view(100).yaw);
            for (ObjectId id = 1; id <= 9; ++id) vectors = vectors || length_sq(sys.team_member_avoid(id, fw)) > 0.0f;
        }
        CHECK(arrived && around && !vectors && sys.stats().dynamic_repaths >= 1 && sys.stats().pushes == 0);
        CHECK(sys.view(70).pos == vp);
    }
    {  // a vehicle (no crusher) against a standing squad: Path Around; the container is never displaced in one step
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, cell_center(20, 20), 0, 0.8f, 0.0f, t18_storm(), 1);
        UnitDesc v = unit(70, cell_center(8, 20), 2, 1.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        CHECK(sys.add_unit(v));
        SimRng rng(76);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {70};
        o.destination = cell_center(35, 20);
        sys.move(o, rng, 2);
        float max_step = 0.0f;
        bool arrived = false;
        for (uint32_t f = 2; f < 1200 && !arrived; ++f) {
            const V2 k0 = sys.view(100).pos;
            sys.tick(f, rng, serial_for);
            max_step = std::max(max_step, length(sys.view(100).pos - k0));
            arrived = sys.view(70).arrived;
        }
        CHECK(arrived && max_step <= vmax + 1e-4f);
    }
    {  // a crusher against a hostile squad that cannot be pushed (Max_Speed 0): one push attempt, then it drives on at
       // full speed - no hold, no arrival cut, no repath, no retry
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t18_squad(sys, 200, cell_center(25, 20), 0.0f, 1, 0.0f);
        UnitDesc cr = unit(60, cell_center(8, 20), 2, 1.0f, 10.0f, LocomotorKind::Default);
        cr.collision_class = kUccVehicle;
        cr.crusher = true;
        cr.owner = 2;
        CHECK(sys.add_unit(cr));
        SimRng rng(77);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {60};
        o.destination = cell_center(45, 20);
        sys.move(o, rng, 2);
        bool arrived = false, cruising = false, crusher_state = false;
        float min_speed = 3.0e38f;
        for (uint32_t f = 2; f < 1200 && !arrived; ++f) {
            sys.tick(f, rng, serial_for);
            const UnitView c = sys.view(60);
            arrived = c.arrived;
            cruising = cruising || near(c.speed, 1.56f, 1e-5f);
            if (cruising && c.pos.x < cell_center(44, 20).x) min_speed = std::min(min_speed, c.speed);
            for (const CollisionObject& co : sys.collisions().list()) crusher_state = crusher_state || co.state == CollisionState::Crusher;
        }
        const MovementStats& st = sys.stats();
        std::printf("land squad repulsion: crusher - arrived %d, state 4 seen %d, min cruise %.3f, push failures %u, pushes %u\n",
                    arrived ? 1 : 0, crusher_state ? 1 : 0, static_cast<double>(min_speed), st.push_failures, st.pushes);
        CHECK(arrived && crusher_state && near(min_speed, 1.56f, 1e-5f));
        CHECK(st.push_failures == 1 && st.pushes == 0 && st.early_arrivals == 0 && st.dynamic_repaths == 0);
        CHECK(length(sys.view(60).pos - cell_center(45, 20)) < 1.0f);
    }
    {  // Try Again Later: a squad on a forced path (flag 0x20) against a vehicle that cannot be pushed (Max_Speed 0) - the
       // speed answer 0 is taken at the braking rate, not at once, and the record is re-decided every 30 frames
        MovementSystem sys{MovementConfig{}};
        land_system(sys, 100);
        t16_squad(sys, at, 0, 0.8f, 0.0f, t18_storm(), 1);
        UnitDesc v = unit(70, {at.x + 250.0f, at.y}, 2, 0.0f, 20.0f, LocomotorKind::Default);
        v.collision_class = kUccVehicle;
        v.owner = 1;
        CHECK(sys.add_unit(v));
        SimRng rng(78);
        sys.tick(1, rng, serial_for);
        MoveOrder o;
        o.units = {1};
        o.destination = {at.x + 500.0f, at.y};
        o.flags = kMoveFlagSkipValidation;
        sys.move(o, rng, 2);
        std::vector<float> v_at;
        bool tal = false;
        for (uint32_t f = 2; f < 600; ++f) {
            sys.tick(f, rng, serial_for);
            v_at.push_back(sys.view(100).speed);
            for (const CollisionObject& c : sys.collisions().list()) tal = tal || c.state == CollisionState::TryAgainLater;
        }
        size_t top = 0;
        while (top < v_at.size() && !near(v_at[top], vmax, 1e-5f)) ++top;
        size_t d0 = top;
        while (d0 + 1 < v_at.size() && near(v_at[d0 + 1], vmax, 1e-5f)) ++d0;  // the last cruise frame
        bool steps = d0 + 17 < v_at.size();
        for (size_t i = 1; steps && i <= 16; ++i) steps = near(v_at[d0 + i], vmax - a * static_cast<float>(i), 1e-4f);
        CHECK(tal && steps && v_at.back() == 0.0f && sys.view(100).has_goal);  // 16 steps of the rate down to 0, then held
        CHECK(sys.stats().push_failures >= 3 && sys.stats().push_failures <= 2 + 600 / 30);  // re-decided every 30 frames
        CHECK(sys.view(100).pos.x < at.x + 250.0f - 35.0f);  // it stopped before the vehicle
        CHECK(sys.view(70).pos == (V2{at.x + 250.0f, at.y}));
    }
    {  // single infantry against single infantry: Repulsion at once - no repath, no push; on a leg that is not its last
       // the vector bends the mover's own heading (MOVEMENT 5.4.1 "How Walk combines the vector")
        MovementSystem sys{MovementConfig{}};
        PassabilityGrid g = open_grid(100, 100);
        for (int32_t y = 10; y <= 40; ++y) g.set_base(30, y, kPassImpassable);  // a wall: the path has two legs or more
        sys.set_grid(std::move(g));
        sys.set_anim_length_fn(anim_len);
        CHECK(sys.add_unit(unit(1, cell_center(20, 20), 2, 1.0f, 6.0f, LocomotorKind::Default)));
        SimRng rng(79);
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(45, 25);
        sys.move(o, rng, 1);
        uint32_t f = 1;
        for (; f < 200 && !(sys.view(1).speed > 0.0f); ++f) sys.tick(f, rng, serial_for);
        const std::vector<V2> path(sys.land_path(1).begin(), sys.land_path(1).end());
        CHECK(path.size() >= 3);
        if (path.size() >= 3) {
            const V2 t = normalize_retail(path[1] - path[0]), left{-t.y, t.x};
            const V2 spot = path[0] + (path[1] - path[0]) * 0.5f + left * -2.0f;  // on the first leg, 2 to its right
            CHECK(sys.add_unit(unit(2, spot, 2, 1.0f, 6.0f, LocomotorKind::Default)));
            float bend = 0.0f;
            uint32_t repulsion = 0, other = 0;
            bool arrived = false;
            for (; f < 1500 && !arrived; ++f) {
                sys.tick(f, rng, serial_for);
                const UnitView v = sys.view(1);
                arrived = v.arrived;
                if (dot(v.pos - path[0], t) < length(path[1] - path[0]) * 0.75f) bend = std::max(bend, dot(v.pos - path[0], left));
                for (const CollisionObject& c : sys.collisions().list()) {
                    if (c.a == 1 && c.b == 2 && c.state == CollisionState::Repulsion) ++repulsion;
                    else ++other;
                }
            }
            CHECK(arrived && length(sys.view(1).pos - cell_center(45, 25)) < 1.0f);
            CHECK(repulsion > 0 && other == 0 && bend > 0.5f);  // it side-stepped to the left, away from the unit on its right
            CHECK(sys.view(2).pos == spot && sys.stats().dynamic_repaths == 0 && sys.stats().pushes == 0);
            std::printf("land squad repulsion: single infantry - Repulsion %u frames, side step up to %.2f\n", repulsion,
                        static_cast<double>(bend));
        }
    }
}

int main() {
    test_corridor();
    test_threat();
    test_rules();
    test_flying_rules();
    test_jetpack();
    test_flying();
    test_flying_clearance();
    test_multi_locomotor();
    test_jetpack_alternates();     // S7 t14
    test_jetpack_orders();         // S7 t14
    test_multi_locomotor_types();  // S7 t14
    test_jetpack_accel_fallback();   // S7 t15
    test_jetpack_turn_bands();       // S7 t15
    test_jetpack_no_repeated_pose(); // S8 t09 (PR #19 review)
    test_deploy();                   // S8 t11 / t12
    test_turret();                   // S8 t24
    test_bike();
    test_bomber();
    test_fighter_space();
    test_fighter_guard();
    test_fighter_formation_stack();  // s78f t02
    test_space_avoid();
    test_speed_override();
    test_arrival_turn();  // s78k t01
    test_forced_path();   // s78k t01
    test_team_cap();      // s78k t03
    test_squad_team_speed();  // s78l t04
    test_team_speed_exact();     // s78l t01
    test_arrival_turn_rules();   // s78l t02
    test_arrival_turn_clips();   // s78l t02
    test_arrival_close();        // s78l t02
    test_forced_destination();   // s78l t02
    test_land_squad();           // s78n t03
    test_land_squad_review();    // s78n review
    test_land_squad_layout();    // s78n t14
    test_land_squad_walk_states();  // s78n t14
    test_land_squad_part_b();       // s78n t14 part B
    test_land_squad_speed_step();   // s78n t16
    test_land_squad_review_fixes(); // s78n review
    test_land_squad_teleport();     // s78n t18 item 8
    test_land_squad_footprint();    // s78n t18 items 2 / 6
    test_land_squad_repulsion();    // s78n t18 items 1 / 7
    test_determinism();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all movement gap tests passed\n");
    return 0;
}
