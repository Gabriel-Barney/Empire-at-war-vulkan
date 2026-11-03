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
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/movement/hyperspace.hpp"
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/special_locomotors.hpp"

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::movement;

namespace {

int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

ParallelFor make_thread_pf(unsigned threads) {
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
        20, [](CollisionObject& c) { return c.a != 30; }, [](const CollisionObject& c) { return static_cast<uint32_t>(c.a % 3); },
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
    put_u32(bad, 12 + 35 + 7, 9);   // second collision (35 bytes each): a = 9 ...
    put_u32(bad, 12 + 35 + 11, 4);  // ... b = 4: the pair of the first collision again
    CHECK(rejects(t, bad));
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
    if (bad != UINT32_MAX) std::printf("  %s: resumed run from %u diverged at frame %u\n", s.name, save_at, bad);
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
            o.flags = kMoveFlagSkipValidation;
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
    put_u32(bad, 0, 10);
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
    test_rejections();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all movement save tests passed\n");
    return 0;
}
