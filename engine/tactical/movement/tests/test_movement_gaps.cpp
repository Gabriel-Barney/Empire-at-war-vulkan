// alamo_tactical_movement_gaps_tests (no framework; non-zero exit on failure).
// docs/re/REMAINING_GAPS.md §4.1-4.7: corridor squeeze, threat mode (space ops 25-32, land region cost), and the
// jetpack, flying, bike, land-bomber and fighter locomotors; determinism of a mixed scenario across 1 / 4 / N threads
// with a golden CRC.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/corridor.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"

using namespace alamo;
using namespace alamo::tactical::movement;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static ParallelFor make_thread_pf(unsigned threads) {
    return [threads](uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        std::atomic<uint32_t> next{0};
        auto worker = [&] {
            for (;;) {
                const uint32_t c = next.fetch_add(1);
                if (c >= chunks) break;
                const uint32_t b = c * gr;
                fn(b, std::min(count, b + gr));
            }
        };
        const unsigned extra = std::min<unsigned>(std::max(1u, threads), chunks) - 1;
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < extra; ++t) ts.emplace_back(worker);
        worker();
        for (auto& t : ts) t.join();
    };
}

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
    CHECK(clearance_top(0.0f, 40.0f, 2.0f, 25.0f) == 105.0f && clearance_top(-10.0f, 10.0f, 3.0f, 7.0f) == 7.0f);
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
        for (uint32_t f = 0; f < 30; ++f) sys.tick(f, rng);
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
    // Attack target: the next move starts in Directed_Combat (0x1E).
    CHECK(sys.set_attack_target(1, true, {0.0f, 0.0f}, 0.0f));
    sys.move(o, rng, fr + 30);
    CHECK(sys.view(1).loco_state == LocoState::FighterDirectedCombat);
    std::printf("fighter: threat path %zu waypoints, idle at frame %u\n", wf.size(), fr);
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
static constexpr uint64_t kGoldenMixed = 0x81d85cc90e42fdfbull;

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

int main() {
    test_corridor();
    test_threat();
    test_rules();
    test_flying_rules();
    test_jetpack();
    test_flying();
    test_flying_clearance();
    test_multi_locomotor();
    test_bike();
    test_bomber();
    test_fighter_space();
    test_space_avoid();
    test_speed_override();
    test_determinism();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all movement gap tests passed\n");
    return 0;
}
