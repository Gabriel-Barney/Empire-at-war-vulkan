// alamo_tactical_movement_save_tests (no framework; non-zero exit on failure).
// N1 stage 2 (saves during a tactical battle): MovementSystem and the state it owns (grid stamps, space / air occupancy,
// the land collision layer and candidate tree, special locomotors, fighters with squadrons and cell grids) plus the
// battle host's HyperspaceDepartures / EngineControl.
//   * per class: write -> read -> write gives the same bytes, the hash matches, truncated or invalid data is rejected
//     and leaves the object unchanged;
//   * scenarios (land: maze, formations with jiggle, collisions, crush, stamps, jetpack / flying / bike / bomber; space:
//     capital group with facing, fighter squadrons in a dogfight, a hyperspace departure, engines off / on): the run is
//     saved at several frames, a fresh battle is built the same way (hooks included) and loaded, then both run on and
//     must have identical CRCs (register_crc + register_speed_crc + the departure / engine hashes) and identical save
//     bytes after every tick; the loaded copy runs multi-threaded, the original serially;
//   * a loader that already ran (dirty) is fully replaced by read(); another map or mode is rejected;
//   * random single-bit corruption never crashes and a rejected load leaves the system unchanged.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/hyperspace.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::movement;

namespace {

using ::alamo::test::g_fail;

ParallelFor make_thread_pf(unsigned threads) { return ::alamo::test::make_thread_parallel_for(threads); }

using Bytes = std::vector<uint8_t>;

template <class T>
Bytes bytes_of(const T& t) {
    ByteWriter w;
    t.write(w);
    return w.bytes();
}
// The host's view: the object's read succeeds and consumes the whole chunk.
template <class T>
bool load(T& t, std::span<const uint8_t> b) {
    ByteReader r(b);
    return t.read(r) && r.ok() && r.done();
}
template <class T>
uint64_t hash_of(const T& t) {
    StateHasher h;
    t.hash(h);
    return h.digest();
}

// Every strict prefix of `good` (all near both ends, every `stride`-th in between) is rejected and leaves `target`
// unchanged.
template <class T>
bool rejects_truncation(T& target, const Bytes& good, size_t stride) {
    const Bytes before = bytes_of(target);
    for (size_t n = 0; n < good.size();) {
        ByteReader r(std::span<const uint8_t>(good.data(), n));
        if (target.read(r) || bytes_of(target) != before) {
            std::printf("  truncation at %zu of %zu accepted or changed the object\n", n, good.size());
            return false;
        }
        n += (n < 96 || n + 96 > good.size()) ? 1 : stride;
    }
    return true;
}
// `bad` is rejected and leaves `target` unchanged.
template <class T>
bool rejects(T& target, const Bytes& bad) {
    const Bytes before = bytes_of(target);
    ByteReader r(bad);
    return !target.read(r) && bytes_of(target) == before;
}
void put_u32(Bytes& b, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
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

PassabilityGrid random_maze(uint32_t w, uint32_t h, uint32_t seed, int32_t wall_pct) {
    PassabilityGrid g;
    g.init(w, h);
    SimRng rng(seed);
    std::vector<uint32_t> v(static_cast<size_t>(w) * h, kPassClear);
    const uint32_t blobs = w * h * static_cast<uint32_t>(wall_pct) / 400;
    for (uint32_t b = 0; b < blobs; ++b) {
        const int32_t cx = rng.range_int(0, static_cast<int32_t>(w) - 1), cy = rng.range_int(0, static_cast<int32_t>(h) - 1);
        const int32_t r = rng.range_int(0, 2);
        const int32_t kind = rng.range_int(0, 9);
        const uint32_t bits = kind < 6 ? kPassImpassable : (kind < 8 ? kPassWater : kPassSteep);
        for (int32_t y = cy - r; y <= cy + r; ++y)
            for (int32_t x = cx - r; x <= cx + r; ++x)
                if (x >= 0 && y >= 0 && x < static_cast<int32_t>(w) && y < static_cast<int32_t>(h))
                    v[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = bits;
    }
    g.set_base(v);
    return g;
}

UnitDesc land_unit(ObjectId id, V2 pos, int32_t cls, float speed, float radius, int32_t ucc) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.max_speed = speed;
    d.turn_rate = 10.0f;
    d.movement_class = cls;
    d.collision_class = ucc;
    d.radius = radius;
    d.type_name = "SAVE_TEST_UNIT";
    return d;
}

UnitDesc fighter_desc(ObjectId id, V2 pos, float yaw, int32_t owner) {
    UnitDesc d;
    d.id = id;
    d.pos = pos;
    d.yaw_deg = yaw;
    d.max_speed = 4.0f;
    d.turn_rate = 3.0f;
    d.radius = 8.0f;
    d.space_layer = kLayerFighter;
    d.space_layer_type = 0;
    d.locomotor = LocomotorKind::Fighter;
    d.max_lift = 4.0f;
    d.max_rate_of_roll = 5.0f;
    d.bank_turn_angle = 40.0f;
    d.min_speed = 2.5f;
    d.strafe_distance = 500.0f;
    d.owner = owner;
    d.type_name = "SAVE_TEST_FIGHTER";
    return d;
}

// ================================================================ per-class round trips
void test_grid() {
    const auto build = [](uint32_t seed) {
        PassabilityGrid g = random_maze(48, 40, seed, 15);
        g.set_playable_box({30.0f, 30.0f}, {900.0f, 760.0f});
        g.set_water_height(-4.0f);
        g.stamp(500, make_footprint(cell_center(20, 20), 30.0f, 60.0f, 30.0f, -30.0f, -15.0f, 0, 0, 1.0f, 0));
        return g;
    };
    PassabilityGrid a = build(5);
    // Never stamped: counts unallocated, round trip.
    {
        PassabilityGrid plain = random_maze(16, 16, 1, 10), copy = random_maze(16, 16, 1, 10);
        const Bytes pb = bytes_of(plain);
        CHECK(load(copy, pb) && bytes_of(copy) == pb && hash_of(copy) == hash_of(plain));
    }
    a.stamp(501, make_footprint(cell_center(10, 30), 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0, 0, 1.0f, kPassRebelWall));
    a.stamp(502, make_footprint(cell_center(11, 30), 90.0f, 40.0f, 20.0f, -20.0f, -10.0f, 0, 0, 1.0f, 0));  // overlaps 501
    a.stamp_secondary_circle(600, cell_center(30, 12), 45.0f, kPassShield);
    a.stamp_secondary(601, make_footprint(cell_center(31, 12), 0.0f, 20.0f, 20.0f, -10.0f, -10.0f, 0, 0, 1.0f, kPassShield));
    a.unstamp(500);  // a map obstacle destroyed in battle
    const Bytes good = bytes_of(a);
    PassabilityGrid b = build(5);  // the loader: the same map setup (its stamps are replaced)
    CHECK(load(b, good));
    CHECK(bytes_of(b) == good && hash_of(b) == hash_of(a) && b.version() == a.version());
    CHECK(std::equal(a.effective_values().begin(), a.effective_values().end(), b.effective_values().begin(),
                     b.effective_values().end()));
    // Counts rebuilt exactly: unstamping in the loaded grid restores the same values as in the original.
    a.unstamp(501);
    b.unstamp(501);
    a.unstamp_secondary(600);
    b.unstamp_secondary(600);
    CHECK(hash_of(a) == hash_of(b) && bytes_of(a) == bytes_of(b));
    // S8 t01: a map stamp handed to its object (rekey) round-trips under the object id; the loader's own stamp under the
    // placement id is replaced.
    {
        PassabilityGrid r = build(5);
        r.stamp(-3, make_footprint(cell_center(36, 26), 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0, 0, 1.0f, 0));
        CHECK(r.rekey(-3, 77) && r.stamped(77) && !r.stamped(-3));
        const Bytes rb = bytes_of(r);
        PassabilityGrid l = build(5);
        l.stamp(-3, make_footprint(cell_center(36, 26), 0.0f, 40.0f, 40.0f, -20.0f, -20.0f, 0, 0, 1.0f, 0));
        CHECK(load(l, rb) && bytes_of(l) == rb && hash_of(l) == hash_of(r) && l.stamped(77) && !l.stamped(-3));
        CHECK(l.occupies(77, 36, 26) && !l.occupies(-3, 36, 26));
    }
    // S8 t02: a run-time stamp that lands on no cell (off the grid) is still a stamp; it round-trips with no cells.
    {
        PassabilityGrid z = build(5);
        CHECK(z.stamp(88, make_footprint({-500.0f, -500.0f}, 0.0f, 20.0f, 20.0f, -10.0f, -10.0f, 0, 0, 1.0f, 0)) == 0);
        CHECK(z.stamped(88) && z.stamp_cells(88).empty());
        const Bytes zb = bytes_of(z);
        PassabilityGrid l = build(5);
        CHECK(load(l, zb) && bytes_of(l) == zb && hash_of(l) == hash_of(z) && l.stamped(88) && l.stamp_cells(88).empty());
        CHECK(l.stamped_ids() == z.stamped_ids());
    }
    // Another map, or the same map with another playable box: rejected, unchanged.
    PassabilityGrid other = build(6);
    CHECK(rejects(other, good));
    PassabilityGrid boxed = build(5);
    boxed.set_playable_box({50.0f, 30.0f}, {900.0f, 760.0f});
    CHECK(rejects(boxed, good));
    PassabilityGrid t = build(5);
    CHECK(rejects_truncation(t, good, 7));
    // Layout: u32 version, u32 w, u32 h, u64 digest, u64 version, bool counted, u32 stamps, (i32 id, u32 bits, u32 n,
    // cells)...
    Bytes bad = good;
    put_u32(bad, 0, 99);
    CHECK(rejects(t, bad));
    const size_t first = 4 + 4 + 4 + 8 + 8 + 1 + 4;  // first stamp
    bad = good;
    put_u32(bad, first + 4, 0x200);  // bits outside the 9 passability bits
    CHECK(rejects(t, bad));
    bad = good;
    put_u32(bad, first + 12, 48u * 40u);  // cell index out of range
    CHECK(rejects(t, bad));
    std::printf("grid: %zu save bytes\n", good.size());
}

void test_space_occupancy() {
    SpaceOccupancy a;
    a.set_box({-3000.0f, -2000.0f}, {3000.0f, 2500.0f});
    a.set_window(40, 90);
    a.add_static(-3, {100.0f, 200.0f}, 300.0f, kSpaceStatic | kSpaceAsteroid);
    a.add_static(-1, {-900.0f, 50.0f}, 120.0f);
    a.add_static(7, {0.0f, 0.0f}, 40.0f, kSpaceMoving);
    std::vector<SpaceWaypoint> wps(3);
    for (size_t i = 0; i < wps.size(); ++i) {
        wps[i].pos = {static_cast<float>(i) * 100.0f, 10.0f};
        wps[i].heading = 5.0f * static_cast<float>(i);
        wps[i].speed = 3.0f;
        wps[i].time = 33.0f * static_cast<float>(i);
    }
    a.set_track(12, kLayerFrigate, 60.0f, 400, wps);
    a.set_track(4, kLayerCapital, 160.0f, 380, wps);
    const Bytes good = bytes_of(a);
    SpaceOccupancy b;
    b.add_static(99, {1.0f, 1.0f}, 1.0f);  // replaced
    CHECK(load(b, good) && bytes_of(b) == good && hash_of(b) == hash_of(a));
    CHECK(b.query_segment({-50.0f, 10.0f}, {250.0f, 10.0f}, 400.0f, 470.0f, 20.0f, kLayerFrigate, 1, true, 400) ==
          a.query_segment({-50.0f, 10.0f}, {250.0f, 10.0f}, 400.0f, 470.0f, 20.0f, kLayerFrigate, 1, true, 400));
    // S7 t13: a map placement's entry handed to its object (rekey_static) round-trips under the object id.
    {
        SpaceOccupancy k = a;
        CHECK(k.rekey_static(-3, 21) && !k.has_static(-3) && k.has_static(21));
        const Bytes kb = bytes_of(k);
        SpaceOccupancy l = a;
        V2 c{};
        float r = 0.0f;
        uint32_t bits = 0;
        CHECK(load(l, kb) && bytes_of(l) == kb && hash_of(l) == hash_of(k) && !l.has_static(-3));
        CHECK(l.static_entry(21, c, r, bits) && c.x == 100.0f && c.y == 200.0f && r == 300.0f && bits == (kSpaceStatic | kSpaceAsteroid));
    }
    SpaceOccupancy t;
    CHECK(rejects_truncation(t, good, 3));
    Bytes bad = good;
    put_u32(bad, 4 + 4, static_cast<uint32_t>(-1));  // statics out of order (first id -3 -> -1 equals the next)
    CHECK(rejects(t, bad));
}

void test_collision_layer() {
    CollisionLayer a;
    a.add(CollisionKind::MovingVsMoving, 4, 9, 10);
    a.add(CollisionKind::MovingVsStopped, 12, 3, 11);
    a.add(CollisionKind::StoppedVsStopped, 20, 21, 12);
    a.add(CollisionKind::MovingVsMoving, 30, 2, 12);
    uint32_t calls = 0;
    a.run(
        20, [](CollisionObject& c) { return c.a != 30; }, [](const CollisionObject&) { return true; },
        [](const CollisionObject& c) { return static_cast<uint32_t>(c.a % 3); },
        [&calls] { return ++calls < 2; },
        [](CollisionObject& c, bool full) {
            if (c.kind == CollisionKind::StoppedVsStopped) {
                c.state = CollisionState::Resolved;
            } else if (!full) {
                c.state = CollisionState::TryAgainLater;
                c.retry = 24;
            }
        });
    const Bytes good = bytes_of(a);
    CollisionLayer b;
    b.add(CollisionKind::MovingVsMoving, 1, 2, 0);  // replaced
    CHECK(load(b, good) && bytes_of(b) == good && hash_of(b) == hash_of(a));
    CHECK(b.has_pair(9, 4) && b.has_pair(3, 12) && !b.has_pair(1, 2) && !b.has_pair(20, 21));  // pair index rebuilt
    CHECK(b.add(CollisionKind::MovingVsMoving, 4, 9, 30) == UINT32_MAX && a.add(CollisionKind::MovingVsMoving, 40, 41, 30) ==
                                                                              b.add(CollisionKind::MovingVsMoving, 40, 41, 30));
    CollisionLayer t;
    CHECK(rejects_truncation(t, good, 2));
    // Layout: u32 version, u32 next seq, u32 n, per collision u32 seq, u8 kind, u8 state, u8 participants, i32 a, i32 b...
    Bytes bad = good;
    bad[12 + 4] = 7;  // kind
    CHECK(rejects(t, bad));
    bad = good;
    put_u32(bad, 12 + 81 + 7, 9);   // second collision (81 bytes each since s78n t23): a = 9 ...
    put_u32(bad, 12 + 81 + 11, 4);  // ... b = 4: the pair of the first collision again
    CHECK(rejects(t, bad));
    // s78n t20 (collision save 3): exact version; the StoppedVsStopped state Resolving (7) and "nobody is moved"
    // (master 2) are valid values, the next ones are not. A record is 46 bytes: state at +5, master at +45.
    bad = good;
    put_u32(bad, 0, 2);  // 2: before the s78n t20 StoppedVsStopped states
    CHECK(rejects(t, bad));
    put_u32(bad, 0, 3);  // 3: before the s78n t23 MovingVsMoving states and record fields
    CHECK(rejects(t, bad));
    put_u32(bad, 0, 4);  // 4: before the s78n t24 TerrainCollision records
    CHECK(rejects(t, bad));
    put_u32(bad, 0, 6);
    CHECK(rejects(t, bad));
    bad = good;
    bad[12 + 4] = 2;  // s78n t20 F9: as a StoppedVsStopped record (kind at +4)
    bad[12 + 5] = 7;
    bad[12 + 45] = 2;
    CollisionLayer ok;
    CHECK(load(ok, bad) && ok.list()[0].state == CollisionState::Resolving && ok.list()[0].master == 2);
    bad[12 + 5] = 8;
    CHECK(rejects(t, bad));
    bad[12 + 5] = 7;
    bad[12 + 45] = 3;
    CHECK(rejects(t, bad));
    // s78n t20 F9: Resolving or master 2 on a MovingVsMoving (0) / MovingVsStopped (1) record would never end: rejected
    for (const uint8_t kind : {uint8_t{0}, uint8_t{1}}) {
        bad = good;
        bad[12 + 4] = kind;
        bad[12 + 5] = 7;
        CHECK(rejects(t, bad));
        bad[12 + 5] = 1;  // TryAgainLater
        bad[12 + 45] = 2;
        CHECK(rejects(t, bad));
        bad[12 + 45] = 1;
        CollisionLayer fine;
        CHECK(load(fine, bad));
    }
    // s78n t24 (collision save 5, MOVEMENT §5.4.7): a TerrainCollision record - one participant, b = kNoParticipant, no
    // pair key (two records of one unit load), state None or Resolving only
    CollisionLayer tr;
    CHECK(tr.add(CollisionKind::Terrain, 7, kNoParticipant, 3) != UINT32_MAX && tr.add(CollisionKind::Terrain, 7, 9, 4) != UINT32_MAX);
    CHECK(tr.list().size() == 2 && tr.list()[1].b == kNoParticipant && tr.list()[1].participants == 1 && !tr.has_pair(7, 9));
    const Bytes tg = bytes_of(tr);
    CollisionLayer tl;
    CHECK(load(tl, tg) && bytes_of(tl) == tg && hash_of(tl) == hash_of(tr));
    Bytes tb = tg;
    tb[12 + 5] = static_cast<uint8_t>(CollisionState::Resolving);
    CHECK(load(tl, tb) && tl.list()[0].state == CollisionState::Resolving);
    tb[12 + 5] = static_cast<uint8_t>(CollisionState::TryAgainLater);
    CHECK(rejects(t, tb));
    tb = tg;
    tb[12 + 6] = 2;  // participants
    CHECK(rejects(t, tb));
}

void test_fighter_grids() {
    FighterCellGrids a;
    a.build({-2000.0f, -1500.0f}, {2000.0f, 1500.0f});
    a.built = true;
    const FighterAlliedFn allied = [](int32_t x, int32_t y) { return x == y; };
    FighterCell s1, s2, s3, s4;
    CHECK(a.dogfight.join(600, 0, {2, 3}, s1, allied) && a.dogfight.join(700, 1, {2, 3}, s2, allied));
    CHECK(a.parking.join(30, 0, {5, 6}, s3, allied) && a.parking.join(31, 1, {9, 1}, s4, allied));
    const Bytes good = bytes_of(a);
    FighterCellGrids b;
    CHECK(load(b, good) && bytes_of(b) == good && hash_of(b) == hash_of(a));
    CHECK(b.dogfight.entries({2, 3}).size() == 2 && b.dogfight.entries({2, 3})[1].id == 700 && b.parking.nx() == a.parking.nx());
    FighterCellGrids t;
    CHECK(rejects_truncation(t, good, 1));
    // Layout: u32 version, bool built, u32 section length, grid: u32 version, 7 x f32, u32 cap, u32 extra, bool flag,
    // i32 nx, i32 ny, u32 used, (u32 cell, u32 n, (i32 id, i32 owner)...)...
    const size_t cell0 = 4 + 1 + 4 + 4 + 28 + 4 + 4 + 1 + 4 + 4 + 4;
    Bytes bad = good;
    put_u32(bad, cell0 + 8 + 8, 600);  // duplicate id in a cell
    CHECK(rejects(t, bad));
    bad = good;
    put_u32(bad, cell0 - 8, 3);  // ny does not match the build parameters
    CHECK(rejects(t, bad));
}

void test_departures_engines() {
    HyperspaceDepartures a;
    EngineControl ea;
    CHECK(a.schedule(7, 0, false, {70, 0, 5}, {0, 2, 0}, 500.0f));
    CHECK(a.schedule(3, 10, true, {30, 0, 5}, {0, 0, 0}, 400.0f));
    CHECK(a.schedule(12, 50, true, {-30, 8, 5}, {1, 1, 0}, 300.0f));
    ea.disable(4, 2.0f, 0);
    ea.disable(9, -1.0f, 0);
    ea.destroy(2, 0);
    for (uint32_t f = 1; f <= 40; ++f) {
        a.tick(f);
        ea.tick(f);
    }
    ea.disable(5, 1.0f, 40);  // pending until frame 70
    const Bytes good = bytes_of(a), egood = bytes_of(ea);
    HyperspaceDepartures b;
    EngineControl eb;
    CHECK(b.schedule(1, 4, true, {}, {1, 0, 0}, 1.0f));  // replaced
    CHECK(load(b, good) && load(eb, egood) && bytes_of(b) == good && bytes_of(eb) == egood);
    bool same = true;
    for (uint32_t f = 41; f <= 200; ++f) {
        a.tick(f);
        b.tick(f);
        ea.tick(f);
        eb.tick(f);
        same = same && hash_of(a) == hash_of(b) && hash_of(ea) == hash_of(eb) && bytes_of(a) == bytes_of(b) &&
               bytes_of(ea) == bytes_of(eb) && a.events().size() == b.events().size();
    }
    CHECK(same && a.empty() && b.empty() && eb.online(5) && !eb.online(9) && eb.destroyed(2));
    HyperspaceDepartures t;
    EngineControl et;
    CHECK(rejects_truncation(t, good, 1) && rejects_truncation(et, egood, 1));
    Bytes bad = good;
    bad[4 + 4 + 4 + 4] = 2;  // keep flag of the first departure
    CHECK(rejects(t, bad));
    bad = egood;
    bad[4 + 4 + 4] = 3;  // online flag of the first engine entry
    CHECK(rejects(et, bad));
}

// ================================================================ scenarios
struct World {
    MovementSystem sys;
    HyperspaceDepartures dep;
    EngineControl eng;
    SimRng rng;
    explicit World(const MovementConfig& cfg) : sys(cfg) {}
};

struct Scenario {
    const char* name = "";
    MovementConfig cfg;
    std::function<void(World&)> build;                 // setup: map, hooks, units (the saving and the loading run)
    std::function<void(World&, uint32_t)> before;      // orders issued before tick f
    std::function<void(World&, uint32_t)> after;       // host reactions after tick f
    uint32_t frames = 0;
};

void step(const Scenario& s, World& w, uint32_t f, const ParallelFor& pf) {
    s.before(w, f);
    w.sys.tick(f, w.rng, pf);
    s.after(w, f);
}

uint64_t world_crc(const World& w) {
    CrcRegistry reg;
    w.sys.register_crc(reg, "movement");
    w.sys.register_speed_crc(reg, "movement.speed");
    reg.add("l1", [&w](StateHasher& h) {
        w.dep.hash(h);
        w.eng.hash(h);
    });
    return reg.capture(0).combined;
}
Bytes world_bytes(const World& w) {
    ByteWriter b;
    w.sys.write(b);
    w.dep.write(b);
    w.eng.write(b);
    return b.bytes();
}
// The host loads three chunks (movement, departures, engines) and the synced RNG.
bool load_world(World& dst, const World& src) {
    const Bytes ms = bytes_of(src.sys), ds = bytes_of(src.dep), es = bytes_of(src.eng);
    if (!load(dst.sys, ms) || !load(dst.dep, ds) || !load(dst.eng, es)) return false;
    dst.rng = src.rng;
    return bytes_of(dst.sys) == ms && bytes_of(dst.dep) == ds && bytes_of(dst.eng) == es;  // write -> read -> write
}

std::vector<ObjectId> existing(const World& w, std::initializer_list<ObjectId> ids) {
    std::vector<ObjectId> v;
    for (ObjectId id : ids)
        if (w.sys.has_unit(id)) v.push_back(id);
    return v;
}

// What the resumed segment exercised (so a scenario cannot silently stop covering a system).
struct Coverage {
    size_t crushes = 0, max_collisions = 0, fighter_events = 0, departures = 0, locks = 0;
};

// Saves at `save_at`, loads into a fresh battle (or one that already ran `dirty` frames), runs both to the end.
Coverage check_resume(const Scenario& s, uint32_t save_at, uint32_t dirty) {
    Coverage cov;
    World a(s.cfg);
    s.build(a);
    for (uint32_t f = 0; f < save_at; ++f) step(s, a, f, serial_for);
    World b(s.cfg);
    s.build(b);
    const ParallelFor pf4 = make_thread_pf(4);
    for (uint32_t f = 0; f < dirty; ++f) step(s, b, f, pf4);
    const bool loaded = load_world(b, a);
    CHECK(loaded);
    if (!loaded) return cov;
    CHECK(world_crc(a) == world_crc(b) && world_bytes(a) == world_bytes(b));
    const size_t saved_bytes = bytes_of(a.sys).size(), saved_units = a.sys.unit_ids().size();
    uint32_t bad = UINT32_MAX, ticks = 0;
    for (uint32_t f = save_at; f < s.frames; ++f, ++ticks) {
        step(s, a, f, serial_for);
        step(s, b, f, pf4);
        cov.crushes += a.sys.crush_events().size();
        cov.max_collisions = std::max(cov.max_collisions, a.sys.collisions().list().size());
        cov.fighter_events += a.sys.fighter_events().size();
        cov.departures += a.dep.events().size();
        for (ObjectId id : a.sys.unit_ids())
            if (const FighterState* fs = a.sys.fighter_state(id); fs && fs->lock >= 0 && fs->lock_start == f) ++cov.locks;
        if (world_crc(a) != world_crc(b) || world_bytes(a) != world_bytes(b) || !(a.rng == b.rng)) {
            bad = f;
            break;
        }
    }
    CHECK(bad == UINT32_MAX);
    if (bad != UINT32_MAX) {
        std::printf("  %s: resumed run from %u diverged at frame %u\n", s.name, save_at, bad);
        for (const ObjectId id : a.sys.unit_ids()) {  // which unit (diagnostic)
            const UnitView x = a.sys.view(id), y = b.sys.view(id);
            if (!(x.pos == y.pos) || x.yaw != y.yaw || x.speed != y.speed || x.has_goal != y.has_goal)
                std::printf("    unit %d: pos (%.4f, %.4f) / (%.4f, %.4f), yaw %.4f / %.4f, speed %.4f / %.4f, goal %d / %d\n",
                            static_cast<int>(id), static_cast<double>(x.pos.x), static_cast<double>(x.pos.y),
                            static_cast<double>(y.pos.x), static_cast<double>(y.pos.y), static_cast<double>(x.yaw),
                            static_cast<double>(y.yaw), static_cast<double>(x.speed), static_cast<double>(y.speed),
                            x.has_goal ? 1 : 0, y.has_goal ? 1 : 0);
        }
    }
    std::printf("%s: saved at %u (%zu bytes, %zu units%s), %u ticks identical (crushes %zu, collisions <= %zu, fighter "
                "events %zu, locks %zu, hyperspace events %zu)\n",
                s.name, save_at, saved_bytes, saved_units, dirty ? ", dirty loader" : "", ticks, cov.crushes,
                cov.max_collisions, cov.fighter_events, cov.locks, cov.departures);
    return cov;
}

// ---------------------------------------------------------------- land
PassabilityGrid land_grid(uint32_t seed) {
    PassabilityGrid g = random_maze(96, 96, seed, 20);
    for (int32_t y = 4; y < 22; ++y)
        for (int32_t x = 4; x < 30; ++x) g.set_base(x, y, kPassClear);
    for (int32_t y = 70; y < 90; ++y)
        for (int32_t x = 60; x < 90; ++x) g.set_base(x, y, kPassClear);
    for (int32_t y = 24; y < 31; ++y)  // the crusher's lane
        for (int32_t x = 3; x < 92; ++x) g.set_base(x, y, kPassClear);
    g.set_playable_box({20.0f, 20.0f}, {1880.0f, 1880.0f});
    return g;
}

void build_land(World& w, uint32_t seed) {
    w.sys.set_grid(land_grid(seed));
    w.sys.grid().stamp(900, make_footprint(cell_center(40, 40), 30.0f, 60.0f, 30.0f, -30.0f, -15.0f, 0, 0, 1.0f, 0));
    w.sys.set_height_fn([](float x, float y) { return x > 1000.0f ? 40.0f : (y > 1500.0f ? 12.0f : 0.0f); });
    w.sys.set_anim_length_fn(anim_len);
    // (s78n t20: the crusher lane's victims do not scatter, so the crusher drives over them - the crush events are saved)
    w.sys.set_scatters_fn([](int32_t) { return false; });
    w.sys.set_threat_fn([](int32_t, V2 p) { return distance(p, cell_center(45, 45)) < 200.0f ? 30.0f : 0.0f; });
    const float hi = 95.0f * kCellSize;
    w.sys.air().set_box({0.0f, 0.0f}, {hi, hi});
    w.sys.set_map_floor(-10.0f);
    w.rng = SimRng(2026);
    for (uint32_t i = 0; i < 20; ++i) {
        const int32_t cls = i % 3 == 0 ? 7 : 0;
        UnitDesc d = land_unit(static_cast<ObjectId>(1 + i * 3),
                               cell_center(5 + static_cast<int32_t>(i % 12) * 2, 5 + static_cast<int32_t>(i / 12) * 2), cls,
                               cls == 7 ? 1.4f : 1.0f, cls == 7 ? 18.0f : 6.0f, cls == 7 ? kUccVehicle : kUccInfantry);
        d.hero = i == 5;
        d.owner = 0;
        d.path_frame_delay = static_cast<int32_t>(i % 4);
        if (i % 2 == 0) {
            d.formation_raggedness = 2.5f;
            d.max_jiggle_distance = 3.0f;
        }
        CHECK(w.sys.add_unit(d));
    }
    UnitDesc atat = land_unit(200, cell_center(5, 27), 11, 1.0f, 30.0f, kUccGiantVehicle);
    atat.crusher = true;
    atat.owner = 0;
    CHECK(w.sys.add_unit(atat));
    const int32_t victims[] = {14, 30, 47, 70};
    for (int32_t k = 0; k < 4; ++k) {
        UnitDesc d = land_unit(210 + k, cell_center(victims[k], 27), 0, 1.0f, 5.0f, kUccInfantry);
        d.owner = k == 2 ? 0 : 1;  // one friendly: left alone
        d.squashable = true;
        CHECK(w.sys.add_unit(d));
    }
    UnitDesc jp = land_unit(300, cell_center(20, 18), 0, 2.0f, 6.0f, kUccInfantry);
    jp.locomotor = LocomotorKind::Jetpack;
    jp.hover_offset = 25.0f;
    jp.has_pre_turn_anim = true;
    CHECK(w.sys.add_unit(jp));
    UnitDesc fl = land_unit(301, cell_center(22, 18), 6, 3.0f, 10.0f, kUccInfantry);
    fl.locomotor = LocomotorKind::Flying;
    fl.hover_offset = 50.0f;
    CHECK(w.sys.add_unit(fl));
    UnitDesc bk = land_unit(302, cell_center(24, 18), 2, 2.0f, 8.0f, kUccBike);
    bk.locomotor = LocomotorKind::Bike;
    bk.max_rate_of_roll = 1.5f;
    bk.bank_turn_angle = 20.0f;
    CHECK(w.sys.add_unit(bk));
    UnitDesc bm = land_unit(303, cell_center(5, 45), 17, 4.0f, 10.0f, kUccInfantry);
    bm.locomotor = LocomotorKind::LandBomber;
    bm.prepare_strafe_height = 100.0f;
    CHECK(w.sys.add_unit(bm));
    // S7 t06: a Uses_Multiple_Locomotors trooper (WALK primary, JETPACK secondary, Alternate speeds).
    UnitDesc dt = land_unit(304, cell_center(26, 16), 0, 1.0f, 6.0f, kUccInfantry);
    dt.uses_multiple_locomotors = true;
    dt.has_secondary_locomotor = true;
    dt.secondary_locomotor = LocomotorKind::Jetpack;
    dt.alt_max_speed = 2.5f;
    dt.alt_turn_rate = 6.0f;
    dt.hover_offset = 40.0f;
    CHECK(w.sys.add_unit(dt));
}

std::vector<ObjectId> land_walkers(const World& w) {
    std::vector<ObjectId> v;
    for (ObjectId id : w.sys.unit_ids())
        if (id < 100) v.push_back(id);
    return v;
}

void land_before(World& w, uint32_t f) {
    MovementSystem& s = w.sys;
    MoveOrder o;
    switch (f) {
        case 0:
            o.units = land_walkers(w);
            o.destination = cell_center(75, 80);
            o.threat_threshold = 10.0f;
            s.move(o, w.rng, f);
            o = MoveOrder{};
            o.units = {200};
            o.destination = cell_center(88, 27);
            // (s78n t20: an ordinary order - on a forced path decision step 2 pushes the victims aside instead)
            s.move(o, w.rng, f);
            o.flags = 0;
            o.units = {300};
            o.destination = cell_center(50, 50);
            s.move(o, w.rng, f);
            o.units = {301};
            o.destination = cell_center(60, 60);
            s.move(o, w.rng, f);
            o.units = {302};
            o.destination = cell_center(10, 60);
            s.move(o, w.rng, f);
            o.units = {303};
            o.destination = cell_center(60, 45);
            s.move(o, w.rng, f);
            o.units = {304};  // S7 t06: jetpack jump (switch bit), back on WALK after the landing
            o.destination = cell_center(12, 8);
            o.switch_locomotor = true;
            s.move(o, w.rng, f);
            break;
        case 30: s.grid().stamp_secondary_circle(950, cell_center(45, 75), 45.0f, kPassShield); break;
        case 50:
            s.set_speed_factor(4, 0.5f);
            s.set_speed_override(7, 2.5f);
            break;
        case 120:
            s.grid().stamp(901, make_footprint(cell_center(62, 64), 0.0f, 80.0f, 60.0f, -40.0f, -30.0f, 0, 0, 1.0f, 0));
            break;
        case 200:
            s.grid().unstamp_secondary(950);
            s.set_attack_target(301, true, cell_center(30, 30), 0.0f);
            break;
        case 260: {
            s.circle(301, cell_center(50, 50), 20.0f, 100.0f, 200.0f);
            s.teleport(10, cell_center(8, 8), 0.0f, 90.0f, f);
            const std::vector<ObjectId> stop = existing(w, {13, 16});
            s.stop(stop);
            s.set_speed_factor(4, 1.0f);
            break;
        }
        case 300:
            o.units = existing(w, {1, 4, 7, 22, 25, 300});
            o.destination = cell_center(10, 10);
            o.flags = kMoveFlagQueue;
            s.move(o, w.rng, f);
            o = MoveOrder{};
            o.units = {304};  // S7 t06: a second jump
            o.destination = cell_center(20, 14);
            o.switch_locomotor = true;
            s.move(o, w.rng, f);
            break;
        case 450:
            o.units = {304};  // S7 t06: a plain move (walks when landed)
            o.destination = cell_center(28, 20);
            s.move(o, w.rng, f);
            break;
        case 420:
            s.set_owner(213, 0);
            s.remove_unit(19);
            s.grid().unstamp(901);
            s.set_speed_override(7, -1.0f);
            break;
        case 500:
            o.units = land_walkers(w);
            o.units.push_back(302);
            o.destination = cell_center(20, 14);
            o.flags = static_cast<uint8_t>(kMoveFlagPlayerOrder | kMoveFlagDoubleClick);
            s.move(o, w.rng, f);
            break;
        default: break;
    }
}

void land_after(World& w, uint32_t) {
    const std::vector<CrushEvent> crush(w.sys.crush_events().begin(), w.sys.crush_events().end());
    for (const CrushEvent& e : crush) w.sys.remove_unit(e.victim);  // the host kills it
}

Scenario land_scenario() {
    Scenario s;
    s.name = "land";
    s.build = [](World& w) { build_land(w, 99); };
    s.before = land_before;
    s.after = land_after;
    s.frames = 650;
    return s;
}

// ---------------------------------------------------------------- space
std::vector<ObjectId> add_squad(World& w, ObjectId container, ObjectId first, V2 at, float yaw, int32_t owner, uint32_t n) {
    FighterSquadDesc sq;
    sq.id = container;
    for (uint32_t i = 0; i < n; ++i) {
        const ObjectId id = first + static_cast<ObjectId>(i);
        const float side = (i % 2 ? 1.0f : -1.0f) * static_cast<float>((i + 1) / 2) * 30.0f;
        CHECK(w.sys.add_unit(fighter_desc(id, {at.x - std::fabs(side), at.y + side}, yaw, owner)));
        sq.members.push_back(id);
        sq.slots.push_back({-std::fabs(side), side, 0.0f});
    }
    CHECK(w.sys.set_fighter_squad(sq));
    return sq.members;
}

void build_space(World& w) {
    w.sys.space().set_box({-5000.0f, -5000.0f}, {5000.0f, 5000.0f});
    w.sys.space().add_static(-1, {0.0f, 1500.0f}, 300.0f, kSpaceStatic | kSpaceAsteroid);
    w.sys.space().add_static(-2, {-1200.0f, -2600.0f}, 250.0f, kSpaceStatic | kSpaceAsteroid);
    w.sys.space().add_static(-3, {2400.0f, 900.0f}, 200.0f, kSpaceStatic);
    w.sys.set_threat_fn([](int32_t owner, V2 p) { return owner == 0 && length(p - V2{900.0f, -700.0f}) < 400.0f ? 50.0f : 0.0f; });
    w.rng = SimRng(17);
    add_squad(w, 600, 1, {-400.0f, 0.0f}, 0.0f, 0, 3);
    add_squad(w, 700, 11, {400.0f, 0.0f}, 180.0f, 1, 3);
    UnitDesc ship;
    ship.id = 50;
    ship.pos = {0.0f, -1500.0f};
    ship.radius = 120.0f;
    ship.max_speed = 2.0f;
    ship.turn_rate = 1.0f;
    ship.owner = 1;
    ship.type_name = "SAVE_TEST_SHIP";
    CHECK(w.sys.add_unit(ship));
    CHECK(w.sys.add_unit(fighter_desc(30, {-2500.0f, -2500.0f}, 45.0f, 0)));
    CHECK(w.sys.add_unit(fighter_desc(31, {2500.0f, 2500.0f}, 225.0f, 1)));
    for (uint32_t i = 0; i < 6; ++i) {
        UnitDesc d;
        d.id = static_cast<ObjectId>(100 + i);
        d.pos = {-4200.0f + static_cast<float>(i % 3) * 300.0f, -4200.0f + static_cast<float>(i / 3) * 300.0f};
        d.max_speed = i % 3 == 0 ? 2.5f : 4.0f;
        d.turn_rate = i % 3 == 0 ? 0.5f : 1.5f;
        d.radius = i % 3 == 0 ? 120.0f : 50.0f;
        d.space_layer = i % 3 == 0 ? kLayerCapital : kLayerFrigate;
        d.type_name = i % 3 == 0 ? "Capital" : "Frigate";
        d.owner = 0;
        d.accel = 0.05f;
        d.decel = 0.08f;
        CHECK(w.sys.add_unit(d));
    }
}

void depart(World& w, ObjectId id, uint32_t delay, bool keep, uint32_t) {
    if (!w.sys.has_unit(id)) return;
    const UnitView v = w.sys.view(id);
    const float rad = v.yaw * 0.017453292f;
    w.dep.schedule(id, delay, keep, {v.pos.x, v.pos.y, v.z}, {std::cos(rad), std::sin(rad), 0.0f}, 20.0f);
    const ObjectId ids[] = {id};
    w.sys.stop(ids);
}

void space_before(World& w, uint32_t f) {
    MovementSystem& s = w.sys;
    MoveOrder o;
    FighterTarget t;
    switch (f) {
        case 0:
            t.id = 700;
            s.set_fighter_target(600, t);
            t.id = 600;
            s.set_fighter_target(700, t);
            t.id = 50;
            s.set_fighter_target(30, t);
            o.units = {31};
            o.destination = {-2000.0f, 1500.0f};
            s.move(o, w.rng, f);
            o.units = {50};
            o.destination = {1500.0f, -1500.0f};
            s.move(o, w.rng, f);
            o.units = {100, 101, 102, 103, 104, 105};
            o.destination = {3000.0f, 3500.0f};
            o.facing_deg = 90.0f;
            o.threat_threshold = 20.0f;
            s.move(o, w.rng, f);
            break;
        case 40: depart(w, 102, 30, true, f); break;
        case 60:
            w.eng.disable(103, 3.0f, f);
            s.set_speed_factor(103, 0.25f);  // Engines_Disabled_Speed_Modifier while off
            break;
        case 150:
            w.eng.destroy(104, f);
            s.set_speed_factor(104, 0.25f);
            break;
        case 300: {
            const std::vector<ObjectId> stop = existing(w, {11, 12, 13});
            s.stop(stop);
            s.fighter_dead_stop(31);
            t.point = true;
            t.pos = {0.0f, 3000.0f, 0.0f};
            s.set_fighter_target(700, t);
            break;
        }
        case 340: s.set_fighter_target(600, FighterTarget{}); break;
        case 380:
            s.remove_fighter_squad(700);
            o.units = existing(w, {11, 12, 13});
            o.destination = {-500.0f, 2500.0f};
            s.move(o, w.rng, f);
            break;
        case 420:
            depart(w, 105, 0, false, f);
            s.teleport(101, {2000.0f, -3000.0f}, 0.0f, 45.0f, f);
            if (w.eng.repair_online(104, f)) s.set_speed_factor(104, 1.0f);
            break;
        case 460: {
            FighterSquadDesc sq;
            sq.id = 700;
            sq.members = existing(w, {12, 11});
            sq.slots = {{0.0f, 0.0f, 0.0f}, {-30.0f, 30.0f, 0.0f}};
            s.set_fighter_squad(sq);
            o.units = {100, 103, 104};
            o.destination = {-1000.0f, -3500.0f};
            o.facing_deg = kNoFacing;
            s.move(o, w.rng, f);
            break;
        }
        default: break;
    }
}

void space_after(World& w, uint32_t f) {
    w.dep.tick(f);
    for (ObjectId id : w.sys.unit_ids())
        if (w.dep.departing(id)) {
            const Vec3 p = w.dep.position(id), d = w.dep.facing(id);
            w.sys.teleport(id, {p.x, p.y}, p.z, std::atan2(d.y, d.x) * 57.29578f, f);
        }
    for (const HyperspaceEvent& e : w.dep.events())
        if (e.kind == HyperspaceEvent::Kind::Killed) {
            w.sys.remove_unit(e.id);
            w.eng.remove(e.id);
        }
    w.eng.tick(f);
    for (const EngineEvent& e : w.eng.events())
        if (e.kind == EngineEvent::Kind::On) w.sys.set_speed_factor(e.id, 1.0f);
    w.eng.clear_events();
}

Scenario space_scenario() {
    Scenario s;
    s.name = "space";
    s.cfg.mode = MoveMode::Space;
    s.build = build_space;
    s.before = space_before;
    s.after = space_after;
    s.frames = 600;
    return s;
}

// ---------------------------------------------------------------- s78l t02: arrival turn (version 12)
// Has_Pre_Turn_Anim walkers with begin / band / end clips (clip data hook) and a plain walker turn to their final
// facings; saves land in the begin clip, the band turn and the end clip. The host drains the turn clip events.
bool arrival_clip(ObjectId, int32_t anim, uint32_t& n, float& fps) {
    switch (anim) {
        case kAnimTurnLeftBegin:
        case kAnimTurnRightBegin: n = 12; break;
        case kAnimTurnLeftEnd:
        case kAnimTurnRightEnd: n = 9; break;
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

void build_arrival(World& w) {
    PassabilityGrid g;
    g.init(60, 60);
    w.sys.set_grid(std::move(g));
    w.sys.set_deploy_clip_fn(arrival_clip);
    w.rng = SimRng(77);
    for (ObjectId id = 1; id <= 3; ++id) {
        UnitDesc d = land_unit(id, cell_center(10, 10 + static_cast<int32_t>(id) * 8), 0, 2.0f, 8.0f, kUccVehicle);
        d.has_pre_turn_anim = id != 3;
        d.stopped_turn_rate = id == 1 ? 3.0f : 0.0f;
        d.rotation_animation_speed = 1.5f;
        CHECK(w.sys.add_unit(d));
    }
}

void arrival_before(World& w, uint32_t f) {
    if (f != 0) return;
    for (ObjectId id = 1; id <= 3; ++id) {
        MoveOrder o;
        o.units = {id};
        o.destination = cell_center(20, 10 + static_cast<int32_t>(id) * 8);
        o.final_facing_deg = id == 2 ? -120.0f : 135.0f;
        w.sys.move(o, w.rng, f);
    }
}

void arrival_after(World& w, uint32_t) { (void)w.sys.take_turn_clip_events(); }

Scenario arrival_scenario() {
    Scenario s;
    s.name = "arrival turn";
    s.build = build_arrival;
    s.before = arrival_before;
    s.after = arrival_after;
    s.frames = 260;
    return s;
}

// s78n t03: a land squad container walks its own path, its members steer to their slots; saves land while the team
// moves, while a member recovers to its slot after the team stopped, and after.
void build_squad(World& w) {
    PassabilityGrid g;
    g.init(80, 80);
    w.sys.set_grid(std::move(g));
    w.rng = SimRng(31);
    UnitDesc c = land_unit(100, cell_center(10, 20), 0, 2.0f, 12.0f, kUccInfantry);
    c.team_container = true;
    c.container_arrangement = 1;  // s78n t14: Dartboard (re-lays, Halton seeds, start delays in the saves)
    CHECK(w.sys.add_unit(c));
    const V2 slots[3] = {{0.0f, 0.0f}, {-20.0f, 15.0f}, {-20.0f, -15.0f}};
    std::vector<ObjectId> ids;
    for (ObjectId id = 1; id <= 3; ++id) {
        const V2 s = slots[id - 1];
        UnitDesc d = land_unit(id, cell_center(10, 20) + s, 0, 2.5f, 3.0f, kUccInfantry);
        d.formation_spacing = 0.6f;
        CHECK(w.sys.add_unit(d));
        CHECK(w.sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(id);
    }
    CHECK(w.sys.set_land_team(100, ids, slots));
}

void squad_before(World& w, uint32_t f) {
    // The team links are host state set before every tick (set_team: not saved).
    for (const ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}}) CHECK(w.sys.set_team(id, 100, 1.0f));
    if (f != 0 && f != 150) return;
    MoveOrder o;
    o.units = {2};  // a member's order goes to its container
    o.destination = f == 0 ? cell_center(40, 30) : cell_center(40, 50);
    w.sys.move(o, w.rng, f);
}

void squad_after(World&, uint32_t) {}

Scenario squad_scenario() {
    Scenario s;
    s.name = "land squad";
    s.build = build_squad;
    s.before = squad_before;
    s.after = squad_after;
    s.frames = 420;
    return s;
}

// s78n review: the team speed of a move ordered on the first tick after a load. The host runs its commands before its
// set_team calls (apply_speed_factors, then the movement tick), so a move ordered on the resume frame reads the team link
// and cap of the saving run's last tick: the container's own Max_Speed (2.0) is above its members' (1.0), and in one
// formation with a faster unit (3.0) the formation speed is the team speed (1.0), not the container's own 2.0.
void build_squad_speed(World& w) {
    PassabilityGrid g;
    g.init(80, 80);
    w.sys.set_grid(std::move(g));
    w.rng = SimRng(37);
    UnitDesc c = land_unit(100, cell_center(10, 20), 0, 2.0f, 12.0f, kUccInfantry);
    c.team_container = true;
    CHECK(w.sys.add_unit(c));
    const V2 slots[2] = {{0.0f, 12.0f}, {0.0f, -12.0f}};
    std::vector<ObjectId> ids;
    for (ObjectId id = 1; id <= 2; ++id) {
        CHECK(w.sys.add_unit(land_unit(id, cell_center(10, 20) + slots[id - 1], 0, 1.0f, 3.0f, kUccInfantry)));
        ids.push_back(id);
    }
    CHECK(w.sys.set_land_team(100, ids, slots));
    CHECK(w.sys.add_unit(land_unit(50, cell_center(12, 22), 0, 3.0f, 3.0f, kUccInfantry)));
}

void squad_speed_before(World& w, uint32_t f) {
    if (f == 40 || f == 140) {  // the commands first ...
        MoveOrder o;
        o.units = {1, 50};  // the member's order goes to its container: container + the fast unit in one formation
        o.destination = f == 40 ? cell_center(40, 24) : cell_center(20, 40);
        w.sys.move(o, w.rng, f);
    }
    // member 2 garrisons (parked: its team place is saved) and leaves again
    if (f == 100) CHECK(w.sys.park_team_member(2));
    if (f == 170) {
        CHECK(w.sys.add_unit(land_unit(2, cell_center(14, 30), 0, 1.0f, 3.0f, kUccInfantry)));
        CHECK(w.sys.rejoin_land_team(2));
    }
    for (const ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}})  // ... then set_team
        if (w.sys.has_unit(id)) CHECK(w.sys.set_team(id, 100, 1.0f));
}

Scenario squad_speed_scenario() {
    Scenario s;
    s.name = "land squad team speed";
    s.build = build_squad_speed;
    s.before = squad_speed_before;
    s.after = squad_after;
    s.frames = 260;
    return s;
}

// s78n t18: a squad (container 100, members 1..4 with a deceleration tag) that walks through a standing infantry unit (a
// live Repulsion record), gets a new order while moving (the braking coast), has a member suspended and one parked, a
// garrison-exit host, a hover unit rising, and a vehicle that path-arounds a standing one (the detour bookkeeping).
void build_t18(World& w) {
    PassabilityGrid g;
    g.init(80, 80);
    w.sys.set_grid(std::move(g));
    w.rng = SimRng(41);
    UnitDesc c = land_unit(100, cell_center(10, 20), 0, 0.8f, 12.0f, kUccInfantry);
    c.team_container = true;
    c.container_arrangement = 0;
    CHECK(w.sys.add_unit(c));
    const V2 slots[4] = {{6.0f, 6.0f}, {6.0f, -6.0f}, {-6.0f, 6.0f}, {-6.0f, -6.0f}};
    std::vector<ObjectId> ids;
    for (ObjectId id = 1; id <= 4; ++id) {
        UnitDesc m = land_unit(id, cell_center(10, 20) + slots[id - 1], 0, 1.0f, 3.0f, kUccInfantry);
        m.accel = m.decel = 0.05f;
        m.formation_spacing = 0.3f;
        CHECK(w.sys.add_unit(m));
        CHECK(w.sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(id);
    }
    CHECK(w.sys.set_land_team(100, ids, slots));
    CHECK(w.sys.add_unit(land_unit(70, cell_center(20, 20), 0, 1.0f, 6.0f, kUccInfantry)));   // standing on the squad's path
    CHECK(w.sys.add_unit(land_unit(71, cell_center(30, 26), 7, 1.0f, 18.0f, kUccVehicle)));   // the host the squad "left"
    CHECK(w.sys.add_unit(land_unit(72, cell_center(10, 40), 7, 1.5f, 18.0f, kUccVehicle)));   // a mover ...
    CHECK(w.sys.add_unit(land_unit(73, cell_center(20, 40), 7, 1.5f, 18.0f, kUccVehicle)));   // ... and the unit in its way
    UnitDesc h = land_unit(74, cell_center(10, 60), 7, 1.0f, 12.0f, kUccVehicle);
    h.hover_height = 80.0f;
    CHECK(w.sys.add_unit(h));
}

void t18_before(World& w, uint32_t f) {
    if (f == 0) {
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(45, 20);
        w.sys.move(o, w.rng, f);
        MoveOrder v;
        v.units = {72};
        v.destination = cell_center(40, 40);
        w.sys.move(v, w.rng, f);
        MoveOrder hv;
        hv.units = {74};
        hv.destination = cell_center(30, 60);
        w.sys.move(hv, w.rng, f);
        CHECK(w.sys.set_garrison_exit(100, 71));
    }
    if (f == 230) {  // a new order while the squad moves: it coasts, then turns
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(30, 30);
        w.sys.move(o, w.rng, f);
        CHECK(w.sys.set_garrison_exit(100, 71));
    }
    if (f == 150) CHECK(w.sys.suspend_team_member(2));
    if (f == 200) CHECK(w.sys.rejoin_land_team(2));
    if (f == 300) CHECK(w.sys.park_team_member(4));
    if (f == 320) CHECK(w.sys.hover_lower(74));
    for (const ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}, ObjectId{3}, ObjectId{4}})
        if (w.sys.has_unit(id)) CHECK(w.sys.set_team(id, 100, 1.0f, 0.8f));
    CHECK(w.sys.set_pair_gate(72, -1, false));  // (the host feed of every tick; nothing is gated here)
}

Scenario t18_scenario() {
    Scenario s;
    s.name = "land squad collision records, walk and gates (s78n t18)";
    s.build = build_t18;
    s.before = t18_before;
    s.after = squad_after;
    s.frames = 420;
    return s;
}

// s78n t18 review: a squad for the save scenarios - container `cid` (Rows, Max_Speed `cspeed`, squashable) facing `yaw`,
// members cid + 1 .. cid + 4 (Max_Speed `mspeed`, OverrideDeceleration `decel`, half-extents 3) on slots (+-6, +-6).
void save_squad(World& w, ObjectId cid, V2 at, float yaw, int32_t owner, float cspeed, float mspeed, float decel) {
    UnitDesc c = land_unit(cid, at, 0, cspeed, 12.0f, kUccInfantry);
    c.team_container = true;
    c.container_arrangement = 0;
    c.yaw_deg = yaw;
    c.owner = owner;
    c.squashable = true;
    CHECK(w.sys.add_unit(c));
    const V2 fw = dir_from_deg(yaw), lf{-fw.y, fw.x};
    std::vector<ObjectId> ids;
    std::vector<V2> slots;
    for (int32_t i = 0; i < 4; ++i) {
        const V2 sl{i < 2 ? 6.0f : -6.0f, i % 2 == 0 ? 6.0f : -6.0f};
        UnitDesc m = land_unit(cid + 1 + i, at + fw * sl.x + lf * sl.y, 0, mspeed, 3.0f, kUccInfantry);
        m.accel = m.decel = decel;
        m.yaw_deg = yaw;
        m.owner = owner;
        m.formation_spacing = 0.3f;
        CHECK(w.sys.add_unit(m));
        CHECK(w.sys.set_team_member_shape(m.id, 3.0f, 3.0f, 3.0f, 3.0f));
        ids.push_back(m.id);
        slots.push_back(sl);
    }
    CHECK(w.sys.set_land_team(cid, ids, slots));
}

// s78n t18 review: the record states and walk states the first t18 scenario does not pass through, each in its own lane
// (250 apart: no shared candidates or crowd boxes):
//  * Try Again Later - squad 100 on a forced path (flag 0x20) against vehicle 70 that cannot be pushed (Max_Speed 0);
//  * Speed Slave Only - vehicle 250 behind vehicle 251 of its own formation, which is held up by the crawler 252 ahead
//    (s78n t23 follow-up, MOVEMENT 5.4.6 "State 2 service": only the same-formation state 2 lasts; two squads of two
//    formations - the scenario before - re-decide every frame and never stay in state 2);
//  * Crusher (state 4) - the crusher 60 against the hostile squad 400 that cannot be pushed (its one push attempt fails);
//  * the final-facing turn - squad 500's order carries a final facing 90 degrees off.
void build_t18_states(World& w) {
    PassabilityGrid g;
    g.init(100, 100);
    w.sys.set_grid(std::move(g));
    w.rng = SimRng(43);
    save_squad(w, 100, cell_center(10, 10), 0.0f, 1, 0.8f, 1.0f, 0.05f);
    UnitDesc v = land_unit(70, cell_center(24, 10), 7, 0.0f, 20.0f, kUccVehicle);
    v.owner = 1;
    CHECK(w.sys.add_unit(v));
    const auto vehicle = [&w](ObjectId id, V2 pos, float hx, float hy, float speed, float radius, int32_t owner) {
        UnitDesc d = land_unit(id, pos, 7, speed, radius, kUccVehicle);
        d.hard_x = hx;
        d.hard_y = hy;
        d.owner = owner;
        CHECK(w.sys.add_unit(d));
    };
    const V2 lane = cell_center(14, 35);
    // s78n t26 review round 2 (MOVEMENT §4.3 "Correction, s78n t19" [H]): rows rank by attack range (shortest in front), no longer by max speed: the slower 251 keeps the front
    // row with the shorter range (80 against 100), so 250 walks behind it
    w.sys.set_attack_range_query([](ObjectId id) { return id == 251 ? 80.0f : 100.0f; });
    vehicle(250, lane - V2{40.0f, 0.0f}, 10.0f, 6.0f, 1.0f, 12.0f, 1);
    vehicle(251, lane, 10.0f, 6.0f, 0.8f, 12.0f, 1);
    vehicle(252, lane + V2{80.0f, 0.0f}, 30.0f, 20.0f, 0.02f, 40.0f, 2);
    save_squad(w, 400, cell_center(25, 60), 0.0f, 1, 0.0f, 0.0f, 0.0f);
    UnitDesc cr = land_unit(60, cell_center(8, 60), 7, 1.0f, 10.0f, kUccVehicle);
    cr.crusher = true;
    cr.owner = 2;
    CHECK(w.sys.add_unit(cr));
    save_squad(w, 500, cell_center(10, 85), 0.0f, 1, 0.8f, 1.0f, 0.05f);
}

void t18_states_before(World& w, uint32_t f) {
    const auto team = [&w](ObjectId cid, float cspeed) {  // the host's set_team of every tick
        for (ObjectId id = cid; id <= cid + 4; ++id)
            if (w.sys.has_unit(id)) CHECK(w.sys.set_team(id, cid, 1.0f, cspeed));
    };
    team(100, 0.8f);
    team(400, 0.0f);
    team(500, 0.8f);
    if (f != 1) return;  // (frame 0 laid the squads out)
    const auto order = [&w, f](ObjectId unit, V2 to, uint8_t flags, float final_facing) {
        MoveOrder o;
        o.units = {unit};
        o.destination = to;
        o.flags = flags;
        o.final_facing_deg = final_facing;
        w.sys.move(o, w.rng, f);
    };
    order(101, cell_center(60, 10), kMoveFlagSkipValidation, kNoFacing);
    order(252, cell_center(90, 35), 0, kNoFacing);
    {  // one formation (s78n t23 follow-up)
        MoveOrder o;
        o.units = {250, 251};
        o.destination = cell_center(14, 35) + V2{110.0f, 0.0f};
        w.sys.move(o, w.rng, f);
    }
    order(60, cell_center(60, 60), 0, kNoFacing);
    order(501, cell_center(25, 85), 0, 90.0f);
}

Scenario t18_states_scenario() {
    Scenario s;
    s.name = "land squad record states and the final-facing turn (s78n t18 review)";
    s.build = build_t18_states;
    s.before = t18_states_before;
    s.after = squad_after;
    s.frames = 400;
    return s;
}

void test_t18_states() {
    const Scenario sc = t18_states_scenario();
    World v(sc.cfg);
    sc.build(v);
    // first / last frame (after the tick) each state was seen in
    uint32_t tal[2] = {0, 0}, slave[2] = {0, 0}, crusher[2] = {0, 0}, turn[2] = {0, 0};
    const auto seen = [](uint32_t* r, uint32_t f) {
        if (r[0] == 0) r[0] = f + 1;
        r[1] = f + 1;
    };
    for (uint32_t f = 0; f < sc.frames; ++f) {
        step(sc, v, f, serial_for);
        for (const CollisionObject& c : v.sys.collisions().list()) {
            if (c.kind == CollisionKind::MovingVsStopped && c.a == 100 && c.b == 70 && c.state == CollisionState::TryAgainLater) seen(tal, f);
            if (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::SpeedSlave) seen(slave, f);
            if (c.kind == CollisionKind::MovingVsStopped && c.a == 60 && c.b == 400 && c.state == CollisionState::Crusher) seen(crusher, f);
        }
        if (v.sys.view(500).team_final_turn) seen(turn, f);
    }
    std::printf("land squad t18 state saves: Try Again Later %u..%u, Speed Slave Only %u..%u, Crusher %u..%u, final-facing turn %u..%u\n",
                tal[0], tal[1], slave[0], slave[1], crusher[0], crusher[1], turn[0], turn[1]);
    CHECK(tal[0] > 0 && tal[1] >= tal[0] + 40);        // held: re-decided every 30 frames
    CHECK(slave[0] > 0 && slave[1] >= slave[0] + 10);
    CHECK(crusher[0] > 0 && crusher[1] >= crusher[0] + 3);
    CHECK(turn[0] > 0 && turn[1] >= turn[0] + 3 && v.sys.view(500).arrived);
    if (tal[0] == 0 || slave[0] == 0 || crusher[0] == 0 || turn[0] == 0) return;
    const auto in_state = [&](uint32_t at, int which) {  // the save taken after `at` ticks holds the state
        World c(sc.cfg);
        sc.build(c);
        for (uint32_t f = 0; f < at; ++f) step(sc, c, f, serial_for);
        bool ok = which == 3 && c.sys.view(500).team_final_turn;
        for (const CollisionObject& co : c.sys.collisions().list()) {
            ok = ok || (which == 0 && co.a == 100 && co.b == 70 && co.state == CollisionState::TryAgainLater);
            ok = ok || (which == 1 && co.kind == CollisionKind::MovingVsMoving && co.state == CollisionState::SpeedSlave);
            ok = ok || (which == 2 && co.a == 60 && co.b == 400 && co.state == CollisionState::Crusher);
        }
        CHECK(ok);
        check_resume(sc, at, 0);
    };
    // Try Again Later: just decided, just before its 30-frame re-decide, right after it
    for (const uint32_t at : {tal[0], tal[0] + 29u, tal[0] + 31u}) in_state(at, 0);
    for (const uint32_t at : {slave[0], (slave[0] + slave[1]) / 2}) in_state(at, 1);
    for (const uint32_t at : {crusher[0], crusher[1]}) in_state(at, 2);
    for (const uint32_t at : {turn[0], (turn[0] + turn[1]) / 2, turn[1]}) in_state(at, 3);
    check_resume(sc, (turn[0] + turn[1]) / 2, 41);  // a loader that already ran
}

// s78n t20: saves inside the record states and path records of batch A, each in its own lane (250 apart):
//  * a Path Around detour (a kind-2 record with hold tags and a resume index on top of the order path) - vehicle 10 around
//    the standing vehicle 11;
//  * a StoppedVsStopped record in Resolving (a kind-3 record) - the overlapping parked vehicles 20 / 21;
//  * a StoppedVsStopped record in Try Again Later - the overlapping infantry units 30 / 31, both with targets (the host
//    feed set_collision_info is not saved: the scenario sets it before every tick, as the host does);
//  * a push (a kind-1 record) - vehicle 40 on a forced path against the standing vehicle 41.
// Every order also carries its formation id, slot and single-member flag (saved with version 18).
UnitDesc t20_save_vehicle(ObjectId id, V2 pos, float speed = 1.0f) {
    UnitDesc d = land_unit(id, pos, 7, speed, 12.0f, kUccVehicle);
    d.hard_x = 10.0f;
    d.hard_y = 6.0f;
    d.accel = d.decel = 0.05f;
    d.owner = 1;
    return d;
}

void build_t20_states(World& w) {
    PassabilityGrid g;
    g.init(100, 100);
    w.sys.set_grid(std::move(g));
    w.rng = SimRng(53);
    CHECK(w.sys.add_unit(t20_save_vehicle(10, cell_center(10, 10))));
    CHECK(w.sys.add_unit(t20_save_vehicle(11, cell_center(24, 10))));
    CHECK(w.sys.add_unit(t20_save_vehicle(20, cell_center(10, 35))));
    CHECK(w.sys.add_unit(t20_save_vehicle(21, cell_center(10, 35) + V2{8.0f, 0.0f}, 0.2f)));  // slow: a long Resolving
    CHECK(w.sys.add_unit(land_unit(30, cell_center(10, 60), 0, 1.0f, 6.0f, kUccInfantry)));
    CHECK(w.sys.add_unit(land_unit(31, cell_center(10, 60) + V2{4.0f, 0.0f}, 0, 1.0f, 6.0f, kUccInfantry)));
    CHECK(w.sys.add_unit(t20_save_vehicle(40, cell_center(10, 85))));
    CHECK(w.sys.add_unit(t20_save_vehicle(41, cell_center(24, 85), 0.2f)));  // slow: a long push
}

void t20_states_before(World& w, uint32_t f) {
    MovementSystem::CollisionInfo ci;  // the host's feed of every tick (not saved)
    ci.target = 900;
    ci.target_pos = cell_center(10, 70);
    ci.attack_range = 300.0f;
    CHECK(w.sys.set_collision_info(30, ci) && w.sys.set_collision_info(31, ci));
    if (f != 1) return;
    MoveOrder o;
    o.units = {10};
    o.destination = cell_center(45, 10);
    w.sys.move(o, w.rng, f);
    MoveOrder p;
    p.units = {40};
    p.destination = cell_center(45, 85);
    p.flags = kMoveFlagSkipValidation;
    w.sys.move(p, w.rng, f);
}

void test_t20_states() {
    Scenario sc;
    sc.name = "land record states and path records (s78n t20)";
    sc.build = build_t20_states;
    sc.before = t20_states_before;
    sc.after = squad_after;
    sc.frames = 420;
    World v(sc.cfg);
    sc.build(v);
    // first / last frame (after the tick) each state was seen in: 0 detour, 1 Resolving, 2 Try Again Later, 3 push
    uint32_t seen[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    const auto holds = [](const World& w, int which) {
        if (which == 0) return w.sys.view(10).pushed && w.sys.view(10).push_kind == 2 && !w.sys.top_tags(10).empty();
        if (which == 3) return w.sys.view(41).pushed && w.sys.view(41).push_kind == 1;
        for (const CollisionObject& c : w.sys.collisions().list()) {
            if (which == 1 && c.kind == CollisionKind::StoppedVsStopped && c.a == 20 && c.state == CollisionState::Resolving &&
                w.sys.view(21).pushed && w.sys.view(21).push_kind == 3)
                return true;
            if (which == 2 && c.kind == CollisionKind::StoppedVsStopped && c.a == 30 && c.state == CollisionState::TryAgainLater) return true;
        }
        return false;
    };
    for (uint32_t f = 0; f < sc.frames; ++f) {
        step(sc, v, f, serial_for);
        for (int k = 0; k < 4; ++k)
            if (holds(v, k)) {
                if (seen[k][0] == 0) seen[k][0] = f + 1;
                seen[k][1] = f + 1;
            }
    }
    std::printf("land t20 state saves: detour %u..%u, Resolving %u..%u, Try Again Later %u..%u, push %u..%u\n", seen[0][0], seen[0][1],
                seen[1][0], seen[1][1], seen[2][0], seen[2][1], seen[3][0], seen[3][1]);
    for (int k = 0; k < 4; ++k) CHECK(seen[k][0] > 0 && seen[k][1] >= seen[k][0] + 5);
    CHECK(!v.sys.view(10).pushed && v.sys.view(10).form_id != 0);  // back on its order path, in its formation
    for (int k = 0; k < 4; ++k) {
        if (seen[k][0] == 0) continue;
        for (const uint32_t at : {seen[k][0], (seen[k][0] + seen[k][1]) / 2, seen[k][1]}) {
            World c(sc.cfg);
            sc.build(c);
            for (uint32_t f = 0; f < at; ++f) step(sc, c, f, serial_for);
            CHECK(holds(c, k));  // the save taken after `at` ticks holds the state
            check_resume(sc, at, 0);
        }
    }
    if (seen[0][0] != 0) check_resume(sc, (seen[0][0] + seen[0][1]) / 2, 37);  // a loader that already ran
    // the Try Again Later record around its 30-frame retry
    if (seen[2][0] != 0)
        for (const uint32_t at : {seen[2][0] + 29u, seen[2][0] + 30u, seen[2][0] + 31u}) check_resume(sc, at, 0);
}

// s78n t25.2 (MOVEMENT §4.3.2, save version 28): an attack order on land - two attackers with a slot each (Unit::atk_*),
// a fire hook (host setup, re-set by the loader like every hook) - saved between the order and the arrival, while the
// slot leg is walked, and after the trim's arrival; the resumed run is identical.
void build_t25_attack(World& w) {
    PassabilityGrid g;
    g.init(100, 60);
    w.sys.set_grid(std::move(g));
    w.rng = SimRng(61);
    const V2 c = cell_center(55, 30);
    for (const auto& [id, y] : {std::pair<ObjectId, int32_t>{10, 23}, std::pair<ObjectId, int32_t>{11, 37}}) {
        UnitDesc d = t20_save_vehicle(id, cell_center(5, y));
        d.hard_x = d.hard_y = 15.0f;
        CHECK(w.sys.add_unit(d));
    }
    UnitDesc t = t20_save_vehicle(900, c);
    t.hard_x = t.hard_y = 5.0f;
    t.owner = 2;
    CHECK(w.sys.add_unit(t));
    w.sys.set_fire_test([c](ObjectId, V2 p, ObjectId, bool) { return distance(p, c) <= 255.0f; });
}

void t25_attack_before(World& w, uint32_t f) {
    if (f != 1) return;
    MoveOrder o;
    o.units = {10, 11};
    o.destination = cell_center(55, 30);
    o.attack.target = 900;
    o.attack.c = o.attack.g = cell_center(55, 30);
    o.attack.t_hx = o.attack.t_hy = 5.0f;
    for (const ObjectId id : o.units) {
        LandAttackUnit e;
        e.id = id;
        e.range = 250.0f;
        e.engage = 255.0f;
        o.attack.units.push_back(e);
    }
    w.sys.move(o, w.rng, f);
}

void test_t25_attack() {
    Scenario sc;
    sc.name = "land attack approach (s78n t25.2)";
    sc.build = build_t25_attack;
    sc.before = t25_attack_before;
    sc.after = squad_after;
    sc.frames = 700;
    World v(sc.cfg);
    sc.build(v);
    uint32_t moving_first = 0, moving_last = 0;
    for (uint32_t f = 0; f < sc.frames; ++f) {
        step(sc, v, f, serial_for);
        const UnitView a = v.sys.view(10);
        if (a.atk_rho > 0.0f && a.has_goal && a.speed > 0.0f) {
            if (moving_first == 0) moving_first = f + 1;
            moving_last = f + 1;
        }
    }
    CHECK(moving_first > 0 && moving_last > moving_first + 100);
    CHECK(v.sys.view(10).atk_rho > 0.0f && v.sys.view(11).atk_rho > 0.0f && v.sys.view(10).atk_theta != v.sys.view(11).atk_theta);
    std::printf("land attack saves: approach frames %u..%u\n", moving_first, moving_last);
    for (const uint32_t at : {2u, moving_first, (moving_first + moving_last) / 2, moving_last, moving_last + 20u}) check_resume(sc, at, 0);
    check_resume(sc, (moving_first + moving_last) / 2, 31);
}

// s78n t18 review: a TEAM entry that goes without a steering member leaving - a member suspended for a hero clash that
// is removed (0), a garrisoned (parked) member that dies inside (1), a rejoin the system refuses (2: the unit is not in
// it) - leaves the entry list at once: the joint box, the container's radius and the rate 0x5ab540 are those a loaded
// copy rebuilds, right away and over the next ticks.
void test_stale_team_entries() {
    const auto build = [](World& w) {
        PassabilityGrid g;
        g.init(80, 80);
        w.sys.set_grid(std::move(g));
        w.rng = SimRng(47);
        UnitDesc c = land_unit(100, cell_center(10, 20), 0, 2.0f, 12.0f, kUccInfantry);
        c.team_container = true;
        CHECK(w.sys.add_unit(c));
        const V2 slots[2] = {{10.0f, 0.0f}, {-10.0f, 0.0f}};
        std::vector<ObjectId> ids;
        for (ObjectId id = 1; id <= 2; ++id) {
            UnitDesc m = land_unit(id, cell_center(10, 20) + slots[id - 1], 0, 1.0f, 3.0f, kUccInfantry);
            m.accel = m.decel = id == 1 ? 0.05f : 0.02f;  // member 2 holds the squad's rate down while it is an entry
            CHECK(w.sys.add_unit(m));
            CHECK(w.sys.set_team_member_shape(id, 3.0f, 3.0f, 3.0f, 3.0f));
            ids.push_back(id);
        }
        CHECK(w.sys.set_land_team(100, ids, slots));
    };
    const auto host = [](World& w) {
        for (const ObjectId id : {ObjectId{100}, ObjectId{1}, ObjectId{2}})
            if (w.sys.has_unit(id)) CHECK(w.sys.set_team(id, 100, 1.0f, 2.0f));
    };
    const float mult = MovementConstants{}.land_speed_multiplier;
    for (int mode = 0; mode < 3; ++mode) {
        World a{MovementConfig{}};
        build(a);
        uint32_t f = 0;
        for (; f < 6; ++f) {
            host(a);
            a.sys.tick(f, a.rng, serial_for);
        }
        float hx = 0.0f, hy = 0.0f;
        CHECK(a.sys.team_entries(100).size() == 2 && a.sys.team_joint_box(100, hx, hy) && hx == 13.0f && hy == 3.0f);
        if (mode == 0) {
            CHECK(a.sys.suspend_team_member(2) && a.sys.team_entries(100).size() == 2);
            CHECK(a.sys.remove_unit(2));
        } else if (mode == 1) {
            CHECK(a.sys.park_team_member(2) && a.sys.team_entries(100).size() == 2);
            CHECK(!a.sys.remove_unit(2) && !a.sys.team_member_parked(2));  // (not in the system: the record goes)
        } else {
            CHECK(a.sys.park_team_member(2) && a.sys.team_entries(100).size() == 2);
            CHECK(!a.sys.rejoin_land_team(2) && !a.sys.team_member_parked(2));  // refused: no such unit; the record goes
        }
        // at once: member 1's slot alone (x from 0 to 13, y +-3)
        CHECK(a.sys.team_entries(100).size() == 1 && a.sys.team_entries(100)[0].member == 1);
        CHECK(a.sys.team_joint_box(100, hx, hy) && hx == 6.5f && hy == 3.0f);
        World b{MovementConfig{}};
        build(b);
        const bool loaded = load_world(b, a);  // (write -> read -> write equal: the container's saved radius included)
        CHECK(loaded);
        if (!loaded) continue;
        float bx = 0.0f, by = 0.0f;
        CHECK(b.sys.team_joint_box(100, bx, by) && bx == hx && by == hy && b.sys.team_entries(100).size() == 1);
        CHECK(world_crc(a) == world_crc(b) && world_bytes(a) == world_bytes(b));
        MoveOrder o;
        o.units = {1};
        o.destination = cell_center(40, 20);
        a.sys.move(o, a.rng, f);
        b.sys.move(o, b.rng, f);
        float first = 0.0f;
        bool same = true;
        const ParallelFor pf4 = make_thread_pf(4);
        for (uint32_t n = 0; n < 120; ++n, ++f) {
            host(a);
            host(b);
            a.sys.tick(f, a.rng, serial_for);
            b.sys.tick(f, b.rng, pf4);
            if (first == 0.0f) first = a.sys.view(100).speed;
            same = same && world_crc(a) == world_crc(b) && world_bytes(a) == world_bytes(b);
        }
        CHECK(same);
        CHECK(first == 0.05f * mult);  // the rate is member 1's (0.078), not the gone member's 0.0312
        std::printf("stale team entries (mode %d): box %.1f x %.1f, first speed step %.4f, 120 ticks identical %d\n", mode,
                    static_cast<double>(hx), static_cast<double>(hy), static_cast<double>(first), same ? 1 : 0);
    }
}

// s78n t18 review: "hidden" (a squad container garrisoned with its whole squad) is host-fed and not saved. The host feeds
// it before every tick and right after a load, so an order to the hidden container directly after the load is ignored,
// as it was before the save; read() itself leaves it unset.
void test_hidden_after_load() {
    const auto build = [](World& w) {
        PassabilityGrid g;
        g.init(80, 80);
        w.sys.set_grid(std::move(g));
        w.rng = SimRng(53);
        save_squad(w, 100, cell_center(10, 20), 0.0f, 1, 0.8f, 1.0f, 0.05f);
        CHECK(w.sys.add_unit(land_unit(71, cell_center(14, 20), 7, 1.0f, 18.0f, kUccVehicle)));  // the garrison host
    };
    const auto feed = [](World& w) {  // the host's feed: its whole squad is inside host 71
        CHECK(w.sys.set_team(100, 100, 1.0f, 0.8f));
        CHECK(w.sys.set_pair_gate(100, 71, true));
    };
    World a{MovementConfig{}};
    build(a);
    uint32_t f = 0;
    for (; f < 4; ++f) a.sys.tick(f, a.rng, serial_for);
    for (ObjectId id = 101; id <= 104; ++id) CHECK(a.sys.park_team_member(id));  // every member went in
    for (; f < 8; ++f) {
        feed(a);
        a.sys.tick(f, a.rng, serial_for);
    }
    MoveOrder o;
    o.units = {100};
    o.destination = cell_center(40, 40);
    {  // before the save: the hidden container takes no order
        World pre{MovementConfig{}};
        build(pre);
        CHECK(load_world(pre, a));
        feed(pre);
        pre.sys.move(o, pre.rng, f);
        CHECK(!pre.sys.view(100).has_goal);
    }
    World b{MovementConfig{}}, raw{MovementConfig{}};
    build(b);
    build(raw);
    CHECK(load_world(b, a) && load_world(raw, a));
    ObjectId gh = 0;
    bool hidden = true;
    CHECK(raw.sys.pair_gate(100, gh, hidden) && !hidden && gh == -1);  // read() leaves the host input unset ...
    feed(b);                                                           // ... so the host feeds it right after the load
    CHECK(b.sys.pair_gate(100, gh, hidden) && hidden && gh == 71);
    const uint64_t draws = b.rng.draws();
    a.sys.move(o, a.rng, f);
    b.sys.move(o, b.rng, f);
    CHECK(!a.sys.view(100).has_goal && !b.sys.view(100).has_goal && b.rng.draws() == draws);  // ignored in both
    bool same = world_crc(a) == world_crc(b) && world_bytes(a) == world_bytes(b);
    const V2 p0 = a.sys.view(100).pos;
    for (uint32_t n = 0; n < 40; ++n, ++f) {
        feed(a);
        feed(b);
        a.sys.tick(f, a.rng, serial_for);
        b.sys.tick(f, b.rng, serial_for);
        same = same && world_crc(a) == world_crc(b) && world_bytes(a) == world_bytes(b);
    }
    CHECK(same && a.sys.view(100).pos == p0 && b.sys.view(100).pos == p0);
    // (without the feed the loaded copy would take the order: that is what the host's feed after a load prevents)
    raw.sys.move(o, raw.rng, f);
    CHECK(raw.sys.view(100).has_goal);
}

// s78n t18 review (save 17 hygiene): a hover state above 3 or a garrison-exit host below -1 is rejected; the parked
// record's entry data (half-extents, Max_Speed, OverrideDeceleration) is in the hash.
void test_t18_review_rejections() {
    const Scenario t18 = t18_scenario();
    World a(t18.cfg);
    t18.build(a);
    for (uint32_t f = 0; f < 100 && a.sys.hover_state(74) != 2; ++f) step(t18, a, f, serial_for);
    CHECK(a.sys.hover_state(74) == 2);  // rising
    const Bytes base = bytes_of(a.sys);
    const auto diff = [&base](const Bytes& other) {
        std::vector<size_t> at;
        for (size_t i = 0; i < base.size() && i < other.size(); ++i)
            if (base[i] != other[i]) at.push_back(i);
        return at;
    };
    World t(t18.cfg);
    t18.build(t);
    {  // the hover state byte: found by the one byte hover_lower changes (2 -> 3)
        World h(t18.cfg);
        t18.build(h);
        CHECK(load(h.sys, base) && h.sys.hover_lower(74));
        const Bytes lowered = bytes_of(h.sys);
        const std::vector<size_t> at = diff(lowered);
        CHECK(lowered.size() == base.size() && at.size() == 1 && base[at[0]] == 2 && lowered[at[0]] == 3);
        if (at.size() == 1) {
            Bytes bad = base;
            for (const uint8_t v : {uint8_t{4}, uint8_t{9}, uint8_t{255}}) {
                bad[at[0]] = v;
                CHECK(rejects(t.sys, bad));
            }
            for (const uint8_t v : {uint8_t{0}, uint8_t{1}, uint8_t{3}}) {
                bad[at[0]] = v;
                CHECK(load(t.sys, bad));
            }
        }
    }
    {  // the garrison-exit host of container 100 (71): found by the four bytes set_garrison_exit(-1) changes
        World e(t18.cfg);
        t18.build(e);
        CHECK(load(e.sys, base) && e.sys.set_garrison_exit(100, -1));
        const Bytes cleared = bytes_of(e.sys);
        const std::vector<size_t> at = diff(cleared);
        CHECK(cleared.size() == base.size() && at.size() == 4 && at[3] == at[0] + 3 && base[at[0]] == 71);
        if (at.size() == 4) {
            World fresh(t18.cfg);
            t18.build(fresh);
            Bytes bad = base;
            put_u32(bad, at[0], 0xFFFFFFFEu);  // -2
            CHECK(rejects(fresh.sys, bad));
            put_u32(bad, at[0], 0x80000000u);  // INT32_MIN
            CHECK(rejects(fresh.sys, bad));
            put_u32(bad, at[0], 0xFFFFFFFFu);  // -1: none
            CHECK(load(fresh.sys, bad));
            put_u32(bad, at[0], 73u);  // another object id
            CHECK(load(fresh.sys, bad));
        }
    }
    {  // the hash: two battles that differ only in a parked member's entry data
        const auto digest = [](float decel, float half) {
            World w{MovementConfig{}};
            PassabilityGrid g;
            g.init(40, 40);
            w.sys.set_grid(std::move(g));
            save_squad(w, 100, cell_center(10, 20), 0.0f, 1, 0.8f, 1.0f, 0.05f);
            CHECK(w.sys.remove_unit(104));
            UnitDesc m = land_unit(104, cell_center(10, 20) + V2{-6.0f, -6.0f}, 0, 1.0f, 3.0f, kUccInfantry);
            m.accel = 0.05f;
            m.decel = decel;
            CHECK(w.sys.add_unit(m));
            CHECK(w.sys.set_team_member_shape(104, half, 3.0f, 3.0f, 3.0f));
            const ObjectId ids[4] = {101, 102, 103, 104};
            const V2 slots[4] = {{6.0f, 6.0f}, {6.0f, -6.0f}, {-6.0f, 6.0f}, {-6.0f, -6.0f}};
            CHECK(w.sys.set_land_team(100, ids, slots));
            StateHasher before;
            w.sys.hash_units(before);
            CHECK(w.sys.park_team_member(104));
            StateHasher after;
            w.sys.hash_units(after);
            return std::pair<uint64_t, uint64_t>{before.digest(), after.digest()};
        };
        const auto ref = digest(0.05f, 3.0f), dec = digest(0.07f, 3.0f), box = digest(0.05f, 4.0f);
        CHECK(ref.first == dec.first && ref.first == box.first);  // static member data is not hashed while it steers ...
        CHECK(ref.second != dec.second && ref.second != box.second && dec.second != box.second);  // ... the parked record's is
    }
}

// ---------------------------------------------------------------- rejection and corruption
void test_rejections() {
    const Scenario land = land_scenario();
    World a(land.cfg);
    land.build(a);
    for (uint32_t f = 0; f < 140; ++f) step(land, a, f, serial_for);
    const Bytes good = bytes_of(a.sys);
    // Another map (maze seed) or mode: rejected, unchanged.
    World other(land.cfg);
    build_land(other, 98);
    CHECK(rejects(other.sys, good));
    World space(space_scenario().cfg);
    build_space(space);
    CHECK(rejects(space.sys, good));
    // A trailing byte: read() takes its own fields only, the host's done() check rejects the chunk.
    Bytes longer = good;
    longer.push_back(0);
    World t(land.cfg);
    land.build(t);
    {
        ByteReader r(longer);
        CHECK(t.sys.read(r) && !r.done());
    }
    World u(land.cfg);
    land.build(u);
    CHECK(rejects_truncation(u.sys, good, 41));
    Bytes bad = good;
    put_u32(bad, 0, 1);  // version (1: before the S7 t06 multi-locomotor fields)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 2);  // 2: before S7 t14 read the alternate speed / turn raw
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 3);  // 3: before the S7 t15 turn band rate / S8 t09 jetpack_still_frames
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 4);  // 4: before the S7 t28 fighter speed cap (5) and the S8 t11 Walk deploy state (6)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 5);
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 6);  // 6: before the S8 t24 turret model (7) and the S7 t37 fighter object goal blocks (8)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 7);
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 8);  // 8: before the a1k t03 pushed move (9)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 9);  // 9: before the s78f t02 fighter formation block stacks (10)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 10);  // 10: before the s78k t01 arrival turn / straight move fields (11)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 11);  // 11: before the s78l t02 arrival turn state (12)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 12);  // 12: before the cu8j t01 / t02 pending compositions (13)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 13);  // 13: before the s78n t03 land squad container fields (14)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 14);  // 14: before the s78n t14 squad layout / start delay / recovery fields (15)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 15);  // 15: before the s78n t14 part B crush-flee / prediction interval fields (16)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 16);  // 16: before the s78n t18 collision states / record answers / squad walk and gate fields (17)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 17);  // 17: before the s78n t20 top path record / formation identity / type extents (18)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 18);  // 18: before the s78n t20 F5 exact single-member goal Unit::self_goal (19)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 19);  // 19: before the s78n t20 batch 1 death-clone fields and push kills (20)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 20);  // 20: before the s78n t23 deployed extents (21)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 21);  // 21: before the s78n t23 MovingVsMoving stand-in fields were dropped (22)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 22);  // 22: before the s78n t26.1 walking-deployed fields (23)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 23);  // 23: before the s78n t24 TerrainCollision (push_turn_first, the re-plan counts dropped) (24)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 24);  // 24: before the s78n t26.2 path-record fractions / turn-done / start turn (25)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 25);  // 25: before the s78n t26.3 Min_Speed_Fraction_For_Turn and corner-pass counts (26)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 26);  // 26: before the s78n t26.4 coast vector / started byte / walk counts (27)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 27);  // 27: before the s78n t25.2 land attack block (Unit::atk_*, the attack counters) (28)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 28);  // 28: before the s78n t26 closing review Walk_Transition fields (29)
    CHECK(rejects(u.sys, bad));
    put_u32(bad, 0, 30);
    CHECK(rejects(u.sys, bad));
    bad = good;
    bad[4] = static_cast<uint8_t>(bad[4] ^ 3);  // mode
    CHECK(rejects(u.sys, bad));
    // Random single-bit flips: never a crash; a rejected load leaves the system unchanged.
    const Bytes before = bytes_of(u.sys);
    SimRng pick(4242);
    uint32_t rejected = 0, accepted = 0, broken = 0;
    for (uint32_t k = 0; k < 400; ++k) {
        Bytes x = good;
        const size_t at = static_cast<size_t>(pick.range_int(0, static_cast<int32_t>(x.size()) - 1));
        x[at] = static_cast<uint8_t>(x[at] ^ (1u << static_cast<uint32_t>(pick.range_int(0, 7))));
        ByteReader r(x);
        if (u.sys.read(r)) {
            ++accepted;  // a float or counter bit: valid data
            if (!load(u.sys, before)) ++broken;
        } else {
            ++rejected;
            if (bytes_of(u.sys) != before) ++broken;
        }
    }
    CHECK(broken == 0 && rejected > 0);
    std::printf("corruption: %u flips rejected, %u accepted (valid values), %zu-byte save\n", rejected, accepted, good.size());

    // Space (squadrons, fighter grids, tracks): truncation.
    const Scenario sp = space_scenario();
    World sa(sp.cfg);
    sp.build(sa);
    for (uint32_t f = 0; f < 90; ++f) step(sp, sa, f, serial_for);
    const Bytes sgood = bytes_of(sa.sys);
    World sb(sp.cfg);
    sp.build(sb);
    CHECK(rejects_truncation(sb.sys, sgood, 37));
}

}  // namespace

int main() {
    test_grid();
    test_space_occupancy();
    test_collision_layer();
    test_fighter_grids();
    test_departures_engines();
    const Scenario land = land_scenario();
    const Coverage lc = check_resume(land, 1, 0);
    CHECK(lc.crushes > 0 && lc.max_collisions > 0);
    for (uint32_t at : {45u, 160u, 333u, 480u}) check_resume(land, at, 0);
    {  // S7 t06: the saves above cover the secondary locomotor (45, 333) and the WALK primary after it (480)
        World w(land.cfg);
        land.build(w);
        bool sec45 = false, sec333 = false, walk480 = false, swapped_back = false;
        for (uint32_t f = 0; f < 480; ++f) {
            step(land, w, f, serial_for);
            const UnitView v = w.sys.view(304);
            if (f > 0 && !v.secondary_locomotor && f < 300) swapped_back = true;
            if (f + 1 == 45) sec45 = v.secondary_locomotor && v.locomotor == LocomotorKind::Jetpack;
            if (f + 1 == 333) sec333 = v.secondary_locomotor;
        }
        walk480 = !w.sys.view(304).secondary_locomotor && w.sys.view(304).locomotor == LocomotorKind::Default;
        CHECK(sec45 && swapped_back && sec333 && walk480);
        std::printf("multi-locomotor coverage: secondary at 45 %d, back on WALK before 300 %d, on the jetpack at 333 %d, walking at "
                    "480 %d\n", sec45, swapped_back, sec333, walk480);
    }
    check_resume(land, 250, 37);  // the loader already ran: read() replaces everything
    const Scenario space = space_scenario();
    const Coverage sc = check_resume(space, 1, 0);
    CHECK(sc.locks > 0 && sc.departures > 0);
    for (uint32_t at : {30u, 75u, 210u, 330u, 430u}) check_resume(space, at, 0);
    check_resume(space, 300, 120);
    {  // s78l t02: saves inside the begin clip, the band turn and the end clip of the arrival turns
        const Scenario arr = arrival_scenario();
        World w(arr.cfg);
        arr.build(w);
        uint32_t turning = 0, first = UINT32_MAX, last = 0;
        for (uint32_t f = 0; f < arr.frames; ++f) {
            step(arr, w, f, serial_for);
            if (w.sys.view(1).arrival_turn) {
                ++turning;
                first = std::min(first, f + 1);
                last = f + 1;
            }
        }
        CHECK(turning > 10 && first < last && !w.sys.view(1).arrival_turn && !w.sys.view(2).arrival_turn);
        for (uint32_t at : {first + 1, (first + last) / 2, last - 1, last}) check_resume(arr, at, 0);
        std::printf("arrival turn saves: unit 1 turning %u frames (%u..%u)\n", turning, first, last);
    }
    {  // s78n t03: saves while the squad walks, at the turn of the second order and while members settle
        const Scenario sq = squad_scenario();
        World w(sq.cfg);
        sq.build(w);
        uint32_t moving = 0, recovering = 0, last_recovering = 0;
        for (uint32_t f = 0; f < sq.frames; ++f) {
            step(sq, w, f, serial_for);
            if (w.sys.view(100).has_goal) {
                ++moving;
            } else {
                for (ObjectId id = 1; id <= 3; ++id)
                    if (w.sys.view(id).has_goal) {  // a member's view: recovering while its team is idle
                        ++recovering;
                        last_recovering = f + 1;
                        break;
                    }
            }
        }
        // s78n t16: the container waits for stragglers (0x5ab640), so the members stop near their slots: a short recovery.
        // s78n t18: it also brakes at its rate into the path end, so the members arrive with it (a frame of recovery, or none)
        CHECK(moving > 40 && w.sys.view(100).arrived);
        (void)recovering;
        for (uint32_t at : {5u, 60u, 151u, 200u, 300u, last_recovering > 0 ? last_recovering - 1u : 298u}) check_resume(sq, at, 0);
        check_resume(sq, 120, 33);
        // s78n review: a move ordered exactly on the resume frame (the first tick after the load) reads the saved team
        // speed: the fast unit's formation speed is the team's 1.0 x mult in the saving and in the loaded run.
        const Scenario sp = squad_speed_scenario();
        for (uint32_t at : {40u, 140u, 170u}) check_resume(sp, at, 0);  // 140: member 2 parked
        {  // s78n t18 (save version 17): the states the scenario passes through are really there, and every one resumes
            const Scenario t18 = t18_scenario();
            World v(t18.cfg);
            t18.build(v);
            bool repulsion = false, around = false, rising = false, lowering = false;
            uint32_t rep_at = 0, around_at = 0, coast_first = 0, coast_last = 0;
            for (uint32_t f = 0; f < t18.frames; ++f) {
                step(t18, v, f, serial_for);
                for (const CollisionObject& c : v.sys.collisions().list()) {
                    if (c.state == CollisionState::Repulsion && !repulsion) {
                        repulsion = true;
                        rep_at = f;
                    }
                    if (c.state == CollisionState::PathAround && !around) {
                        around = true;
                        around_at = f;
                    }
                }
                if (v.sys.view(100).team_coast) {  // s78n t18 review: the braking coast after the new order of frame 230
                    if (coast_first == 0) coast_first = f + 1;
                    coast_last = f + 1;
                    CHECK(v.sys.view(100).speed > 0.0f && v.sys.view(100).has_goal);
                }
                rising = rising || v.sys.hover_state(74) == 2;
                lowering = lowering || v.sys.hover_state(74) == 3;
            }
            CHECK(repulsion && around && rising && lowering);
            CHECK(v.sys.team_member_parked(4) && !v.sys.has_unit(4));
            // s78n t18 review: the coast is really there (from the order of frame 230 for several frames), and the saves
            // below land in it - at its first frame, in its middle and at its last frame
            CHECK(coast_first == 231 && coast_last >= coast_first + 4);
            std::printf("land squad t18 saves: coast frames %u..%u\n", coast_first, coast_last);
            for (const uint32_t at : {coast_first, (coast_first + coast_last) / 2, coast_last}) {
                World c(t18.cfg);
                t18.build(c);
                for (uint32_t f = 0; f < at; ++f) step(t18, c, f, serial_for);
                CHECK(c.sys.view(100).team_coast);  // the state the save holds
                check_resume(t18, at, 0);
            }
            for (uint32_t at : {rep_at + 2u, around_at + 2u, 100u, 151u, 201u, coast_last + 1u, 301u, 325u, 400u}) check_resume(t18, at, 0);
            check_resume(t18, (coast_first + coast_last) / 2, 29);
        }
        {
            World v(sp.cfg);
            sp.build(v);
            const float team = 1.0f * MovementConstants{}.land_speed_multiplier;
            float fast = 0.0f;
            for (uint32_t f = 0; f < 120; ++f) {
                step(sp, v, f, serial_for);
                fast = std::max(fast, v.sys.view(50).speed);
            }
            std::printf("land squad team speed: team %.3f (view %.3f), fast unit formation %.3f, top speed %.3f\n",
                        static_cast<double>(team), static_cast<double>(v.sys.view(100).team_speed),
                        static_cast<double>(v.sys.view(50).formation_speed), static_cast<double>(fast));
            CHECK(v.sys.view(100).team_speed == team && v.sys.view(50).formation_speed == team && fast > 0.0f && fast <= team);
        }
        std::printf("land squad saves: container moving %u of %u frames, members recovering %u frames (last %u)\n", moving, sq.frames,
                    recovering, last_recovering);
    }
    test_t18_states();              // s78n t18 review
    test_t20_states();              // s78n t20
    test_t25_attack();              // s78n t25.2
    test_stale_team_entries();      // s78n t18 review
    test_hidden_after_load();       // s78n t18 review
    test_t18_review_rejections();   // s78n t18 review
    test_rejections();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all movement save tests passed\n");
    return 0;
}
