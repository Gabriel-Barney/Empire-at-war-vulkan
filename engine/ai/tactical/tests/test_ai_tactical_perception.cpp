// alamo_ai_tactical_perception_tests (no framework; non-zero exit on failure).
// A1 stage 1b (docs/re/AI_TACTICAL_PERCEPTION.md): leaf cache rules, the evaluator tree and every
// token family, normalised tokens, threat grid geometry / projection / 300-frame re-projection / fog
// memory (incl. the [M] cell-force double count), point / zone danger into zone_reachability, the
// Lua-facing functions, a scripted scenario CRC (golden shared by MSVC / clang / gcc; 1 / 4 / N
// threads) and, with ALAMO_FOC_DATA, every shipped tactical perceptual equation.
//
//   alamo_ai_tactical_perception_tests [--golden <file>] [--write-golden <file>]
#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "alamo/ai/perception_equations.hpp"
#include "alamo/ai/tactical/perception_cache.hpp"
#include "alamo/ai/tactical/perception_lua.hpp"
#include "alamo/ai/tactical/perception_world.hpp"
#include "alamo/ai/tactical/tactical_perception.hpp"
#include "alamo/ai/tactical/threat_grid.hpp"
#include "alamo/ai/tactical/zone_reachability.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

#if defined(ALAMO_AI_TACTICAL_HAVE_VFS)
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ai;
using namespace alamo::ai::tactical;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static bool near_eq(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps * std::max(1.0, std::fabs(b)); }

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

// Test category table (the default resolver is the objects module's GameObjectCategoryType).
static uint64_t test_category(std::string_view n) {
    if (iequal(n, "Infantry")) return 0x1;
    if (iequal(n, "Vehicle")) return 0x2;
    if (iequal(n, "Structure")) return 0x4;
    if (iequal(n, "LandHero")) return 0x8;
    if (iequal(n, "Capital")) return 0x10;
    if (iequal(n, "Fighter")) return 0x20;
    return 0;
}

static TacticalPerceptionConfig test_config(bool quirk = true) {
    TacticalPerceptionConfig c;
    c.category_mask = test_category;
    c.memory_counted_when_not_fogged = quirk;
    return c;
}

// ---------------------------------------------------------------------------------- world builders
enum : int32_t { kTank = 0, kTrooper = 1, kPad = 2, kBarracks = 3, kHero = 4, kStarDestroyer = 5, kXWing = 6, kTurret = 7 };

static void add_types(PerceptionWorld& w) {
    auto t = [&](const char* name, uint64_t cat, float power, bool hero = false) {
        PerceptionType x;
        x.name = name;
        x.category = cat;
        x.combat_power = power;
        x.is_hero = hero;
        w.types.push_back(x);
    };
    t("Empire_Tank", 0x2, 100.0f);
    t("Rebel_Trooper", 0x1, 10.0f);
    t("Skirmish_Build_Pad", 0x4, 0.0f);
    t("E_Ground_Barracks", 0x4, 50.0f);
    t("Darth_Vader", 0x8, 300.0f, true);
    t("Star_Destroyer", 0x10, 2000.0f);
    t("X_Wing", 0x20, 40.0f);
    t("Empire_Anti_Vehicle_Turret", 0x4, 80.0f);
    w.types[kTank].builds_into = -1;
}

static PerceptionObject make_obj(int32_t id, int32_t owner, int32_t type, float x, float y, float power, float range) {
    PerceptionObject o;
    o.id = id;
    o.owner = owner;
    o.type = type;
    o.has_unit_ai = true;
    o.position = {x, y, 0.0f};
    o.transform[3] = x;
    o.transform[7] = y;
    o.combat_power = power;
    o.max_attack_range = range;
    o.radius = 5.0f;
    return o;
}

static void move_obj(PerceptionObject& o, float x, float y) {
    o.position = {x, y, 0.0f};
    o.transform[3] = x;
    o.transform[7] = y;
}

// Players: 0 Empire (AI), 1 Rebel (human), 2 Underworld (AI, allied with 0), 3 neutral faction.
static void add_players(PerceptionWorld& w) {
    const char* f[] = {"Empire", "Rebel", "Underworld", "Neutral"};
    for (int i = 0; i < 4; ++i) {
        PerceptionPlayerInfo p;
        p.id = i + 1;
        p.faction = f[i];
        p.neutral_faction = i == 3;
        p.is_human = i == 1;
        p.ai_difficulty = i == 1 ? "" : "Hard";
        p.credits = 1000.0f * static_cast<float>(i + 1);
        p.pop_cap = 20;
        p.pop_used = 5 * i;
        w.players.push_back(p);
    }
    // relation [a*4+b]: 0 allied, 1 enemy, 2 neutral
    const uint8_t r[16] = {0, 1, 0, 2,  //
                           1, 0, 1, 2,  //
                           0, 1, 0, 2,  //
                           2, 2, 2, 0};
    w.relation.assign(r, r + 16);
}

// Land world: map -600..600 (land FOW 15 -> 80 FOW cells -> 20 x 20 threat cells of 60), shifted by `off` on both
// axes. Worlds with hint zones use kLandOff: zone passability cells sit at world (x * 20, y * 20) (A1_READINGS §12),
// so the zones need a map in the non-negative quadrant, as on real land maps.
constexpr float kLandOff = 600.0f;
static PerceptionWorld land_world(float off = 0.0f) {
    PerceptionWorld w;
    w.mode = TacticalMode::Land;
    w.map = {-600.0f + off, -600.0f + off, 600.0f + off, 600.0f + off};
    add_players(w);
    add_types(w);
    return w;
}

static std::vector<HintZone> land_zones() {
    // For land_world(kLandOff): zone i covers threat cells (cols 4i..4i+3, rows 8..11), i.e. world x 240i..240i+239,
    // y 481..720. Its passability cells (packed x | y << 16, world (20x, 20y)) are x 12i..12i+11, y 25..36, row by row,
    // so the threat-cell list comes out rows 8..11, columns ascending.
    std::vector<HintZone> zs;
    for (uint32_t i = 0; i < 5; ++i) {
        HintZone z;
        z.id = 100 + i;
        z.name = "Zone" + std::to_string(i);
        z.center[0] = kLandOff - 600.0f + 60.0f * (4.0f * static_cast<float>(i) + 2.0f);
        z.center[1] = kLandOff;
        for (uint32_t py = 36; py >= 25; --py)
            for (uint32_t px = 12 * i; px < 12 * i + 12; ++px) z.cells.push_back(px | (py << 16));
        if (i > 0) z.links.push_back({100 + i - 1, 1});
        if (i < 4) z.links.push_back({100 + i + 1, 1});  // non-zero flags (TACTICAL_GAPS §6.1)
        zs.push_back(z);
    }
    return zs;
}

// ---------------------------------------------------------------------------------- cache
static void test_cache_rules() {
    CHECK(!leaf_cache_hit(0, 30, 30));      // L = 0: never
    CHECK(!leaf_cache_hit(-1, 0, 0));       // frame 0 stamp: never
    CHECK(!leaf_cache_hit(5, 0, 1));
    CHECK(leaf_cache_hit(-1, 1, 1000000));  // forever
    CHECK(leaf_cache_hit(2, 30, 30));
    CHECK(leaf_cache_hit(2, 30, 90));       // NOW <= stamp + L*30 (inclusive)
    CHECK(!leaf_cache_hit(2, 30, 91));
    CHECK(leaf_cache_hit(10, 100, 400));
    CHECK(!leaf_cache_hit(10, 100, 401));
    LeafCache c;
    LeafCacheKey k;
    k.leaf = 7;
    k.self = 1;
    k.params = ";Parameter_Category=x";
    double v = 0;
    c.store(k, 2, 30, 5.0);
    CHECK(c.lookup(k, 2, 60, v) && v == 5.0);
    CHECK(!c.lookup(k, 2, 91, v));
    LeafCacheKey k2 = k;
    k2.target = 3;  // different binding set
    CHECK(!c.lookup(k2, 2, 60, v));
    k2 = k;
    k2.params = ";Parameter_Category=y";
    CHECK(!c.lookup(k2, 2, 60, v));
    c.store(k, 0, 40, 9.0);  // L = 0: not stored
    CHECK(c.lookup(k, 2, 60, v) && v == 5.0);
    c.store(k, -1, 0, 7.0);  // frame 0
    CHECK(!c.lookup(k, -1, 5, v));
    CHECK(c.size() == 1);
}

// ---------------------------------------------------------------------------------- grid
static void test_grid_geometry() {
    TacticalAiConstants c;
    TacticalPerceptionGrid g;
    g.init(TacticalMode::Land, {-600, -600, 600, 600}, 2, threat_grid_config(TacticalMode::Land, c));
    CHECK(g.fow_x() == 80 && g.nx() == 20 && g.ny() == 20);
    CHECK(g.cell_w() == 60.0f && g.cell_h() == 60.0f);
    CHECK(near_eq(g.half_diagonal(), std::sqrt(7200.0) / 2.0));
    CHECK(g.cell_clamped(-600.0f, 600.0f) == 0);          // row 0 at the top
    CHECK(g.cell_clamped(-599.0f, -599.0f) == 19 * 20);   // bottom-left
    CHECK(g.cell_clamped(599.0f, 599.0f) == 19);
    CHECK(g.cell_clamped(5000.0f, -5000.0f) == 399);      // clamp
    CHECK(g.cell_strict(5000.0f, 0.0f) == -1);
    const Vec3f cc = g.cell_center(0, 0);
    CHECK(cc.x == -570.0f && cc.y == 570.0f && cc.z == 0.0f);
    std::vector<uint32_t> cells;
    g.rect_cells({-600.0f, 481.0f, -481.0f, 600.0f}, cells);  // rows 0..1, cols 0..1
    CHECK((cells == std::vector<uint32_t>{0, 1, 20, 21}));
    // space: FOW 100 -> 100 cells -> 25 x 25 threat cells of 400
    TacticalPerceptionGrid s;
    s.init(TacticalMode::Space, {-5000, -5000, 5000, 5000}, 2, threat_grid_config(TacticalMode::Space, c));
    CHECK(s.fow_x() == 100 && s.nx() == 25 && s.cell_w() == 400.0f);
    // 512 FOW clamp: 20000 / 15 = 1333 -> 512 -> 128 threat cells
    TacticalPerceptionGrid big;
    big.init(TacticalMode::Land, {0, 0, 20000, 300}, 1, threat_grid_config(TacticalMode::Land, c));
    CHECK(big.fow_x() == 512 && big.nx() == 128 && big.fow_y() == 20 && big.ny() == 5);
    // ceil(nFow / k): 90 FOW cells -> 23 threat cells
    TacticalPerceptionGrid odd;
    odd.init(TacticalMode::Land, {0, 0, 1350, 1350}, 1, threat_grid_config(TacticalMode::Land, c));
    CHECK(odd.fow_x() == 90 && odd.nx() == 23);
}

static void test_grid_projection() {
    TacticalAiConstants c;
    PerceptionWorld w = land_world();
    // rebel trooper at (30, 570): gx = 630, gy = 30, r = 50 -> cols 9..11, rows 0..1
    w.objects.push_back(make_obj(1, 1, kTrooper, 30.0f, 570.0f, 100.0f, 50.0f));
    // empire tank with two weapon hard points (types 5 and 7) and a non-weapon one (type 3)
    PerceptionObject tank = make_obj(2, 0, kTank, -300.0f, -300.0f, 100.0f, 200.0f);
    PerceptionHardPoint hp;
    hp.type = 5;
    hp.weapon_range = 100.0f;
    hp.power = 30.0f;
    tank.hard_points.push_back(hp);
    hp.type = 3;
    tank.hard_points.push_back(hp);
    hp.type = 7;
    hp.power = 20.0f;
    hp.turret = true;
    hp.destroyable = true;
    hp.world_position = {-240.0f, -300.0f, 0.0f};
    tank.hard_points.push_back(hp);
    w.objects.push_back(tank);
    // an object without UNIT_AI is never in the grid
    PerceptionObject inert = make_obj(3, 1, kTrooper, 0.0f, 0.0f, 10.0f, 10.0f);
    inert.has_unit_ai = false;
    w.objects.push_back(inert);
    w.index();
    TacticalPerceptionGrid g;
    g.init(TacticalMode::Land, w.map, 4, threat_grid_config(TacticalMode::Land, c));
    // stamps as drawn at creation frame 10 (A1_READINGS §14: F + Get_Int(0, 300) - 300, here Get_Int = 300)
    CHECK(g.register_object(w, 1, 10, 10));
    CHECK(!g.register_object(w, 1, 10, 10));  // once
    CHECK(g.register_object(w, 2, 10, 10));
    CHECK(!g.register_object(w, 3, 10, 10));
    CHECK(g.entry_count() == 3);          // trooper whole + tank 2 hard points
    // trooper: v = 100 * 60 * 60 / (4 * 50^2) = 36 in 6 cells, hostile to players 0 and 2 (relation 1)
    const uint32_t c010 = 0 * 20 + 10;
    CHECK(g.threat(0, c010) == 36.0f && g.threat(2, c010) == 36.0f);
    CHECK(g.threat(1, c010) == 0.0f && g.threat(3, c010) == 0.0f);  // owner / neutral relation
    CHECK(g.snapshot(0, c010) == -36.0f);  // no TEAM / HIDE_WHEN_FOGGED: snapshot -= v (open item 3b)
    CHECK(g.threat(0, 1 * 20 + 9) == 36.0f && g.threat(0, 1 * 20 + 11) == 36.0f && g.threat(0, 2 * 20 + 10) == 0.0f);
    CHECK(g.live_entries(c010).size() == 1);
    // cell force for Self = 0, enemy: power / area * cellW * cellH = 36
    GridQuery q;
    q.self = 0;
    q.ally = false;
    // power / area x cellW x cellH x (1 - (1 - health) x A), float, left to right
    const float f36 = 100.0f / 10000.0f * 60.0f * 60.0f * (1.0f - (1.0f - 1.0f) * 0.0f);
    CHECK(near_eq(f36, 36.0, 1e-5));
    CHECK(g.cell_force(w, c010, q) == f36);
    q.ally = true;
    CHECK(g.cell_force(w, c010, q) == 0.0f);
    q.ally = false;
    q.attenuator = 1.0f;
    w.objects[0].health = 0.5f;
    CHECK(g.cell_force(w, c010, q) == 100.0f / 10000.0f * 60.0f * 60.0f * (1.0f - (1.0f - 0.5f) * 1.0f));  // health read now
    w.objects[0].health = 1.0f;
    q.attenuator = 0.0f;
    q.category = 0x2;  // vehicle only: trooper excluded
    CHECK(g.cell_force(w, c010, q) == 0.0f);
    q.category = 0x40;  // no registered type has it: early 0
    CHECK((g.category_union() & 0x40) == 0 && g.cell_force(w, c010, q) == 0.0f);
    q.category = ~0ull;
    q.faction = "REBEL";
    CHECK(g.cell_force(w, c010, q) == f36);
    q.faction = "Empire";
    CHECK(g.cell_force(w, c010, q) == 0.0f);
    q.faction = {};
    q.exclude = 1;
    CHECK(g.cell_force(w, c010, q) == 0.0f);
    q.exclude = -1;
    // power re-read now, area kept
    w.objects[0].combat_power = 200.0f;
    CHECK(g.cell_force(w, c010, q) == 200.0f / 10000.0f * 60.0f * 60.0f * (1.0f - (1.0f - 1.0f) * 0.0f));
    CHECK(g.threat(0, c010) == 36.0f);  // the layer keeps the projected value
    w.objects[0].combat_power = 100.0f;
    // tank hard points: type 5 at the tank (r 100), type 7 turret at (-240, -300) (r 100)
    const int32_t tc = g.cell_clamped(-300.0f, -300.0f);
    const int32_t hc = g.cell_clamped(-240.0f, -300.0f);
    CHECK(g.threat(1, static_cast<uint32_t>(tc)) > 0.0f);
    const float v5 = 30.0f * 3600.0f / 40000.0f, v7 = 20.0f * 3600.0f / 40000.0f;
    CHECK(g.threat(1, static_cast<uint32_t>(hc)) == v5 + v7);
    // radius clamp: tiny range -> half diagonal; huge -> cap 500
    PerceptionObject tiny = make_obj(4, 1, kTrooper, 300.0f, 300.0f, 10.0f, 1.0f);
    PerceptionObject huge = make_obj(5, 1, kTrooper, 300.0f, -300.0f, 10.0f, 5000.0f);
    w.objects.push_back(tiny);
    w.objects.push_back(huge);
    w.index();
    CHECK(g.register_object(w, 4, 10, 10) && g.register_object(w, 5, 10, 10));
    const float hd = g.half_diagonal();
    const auto ct = static_cast<uint32_t>(g.cell_clamped(300.0f, 300.0f));
    CHECK(g.threat(0, ct) == 10.0f * 3600.0f / (4.0f * hd * hd));
    CHECK(g.threat(0, 399) == 10.0f * 3600.0f / (4.0f * 500.0f * 500.0f));  // cap 500 covers the corner

    // ---- re-projection gate (A1_READINGS §14): the first check at the first service at F >= stamp (10), then
    // stamp += 300 per check without re-basing; re-projected only when the transform changed ----
    move_obj(w.objects[0], 30.0f, 330.0f);
    CHECK(g.service_object(w, 1, 100));   // first service: check, moved -> re-projected; next stamp 310
    CHECK(g.threat(0, c010) == 0.0f);
    CHECK(g.threat(0, static_cast<uint32_t>(g.cell_clamped(30.0f, 330.0f))) == 36.0f);
    CHECK(g.snapshot(0, c010) == 0.0f);   // removal subtracts again, clamped at 0
    CHECK(!g.service_object(w, 1, 200));  // gate closed
    CHECK(!g.service_object(w, 1, 310));  // check, not moved; next stamp 610
    move_obj(w.objects[0], 30.0f, 570.0f);
    CHECK(!g.service_object(w, 1, 500));  // gate closed
    CHECK(g.service_object(w, 1, 610));
    CHECK(g.threat(0, c010) == 36.0f);
    // destroyed destroyable hard point: r = -1 at the next re-projection
    w.objects[1].hard_points[2].destroyed = true;
    move_obj(w.objects[1], -300.0f, -301.0f);
    CHECK(g.service_object(w, 2, 310));
    CHECK(g.threat(1, static_cast<uint32_t>(hc)) == v5);
    CHECK(g.entry_count() == 5);

    // ---- fog memory and the [M] double count ----
    g.refog(0, c010, 900);
    CHECK(g.last_seen(0, c010) == 900);
    CHECK(g.memory_entries(0, c010).size() == 1);
    CHECK(g.snapshot(0, c010) == g.threat(0, c010));
    q = GridQuery();
    q.self = 0;
    q.ally = false;
    CHECK(g.cell_force(w, c010, q) == f36 + f36);  // live + memory (AI never fogged): counted twice [M]
    q.memory_counted_when_not_fogged = false;
    CHECK(g.cell_force(w, c010, q) == f36);
    // concentration / bitfield count memory only while fogged
    q.memory_counted_when_not_fogged = true;
    const uint32_t ctr = static_cast<uint32_t>(g.cell_clamped(30.0f, 570.0f));
    const uint32_t one[] = {ctr};
    CHECK(g.cells_concentration(w, one, q) == 10.0f);  // §16: the type's AI_Combat_Power (trooper 10), not the object's 100
    q.exclude = 1;
    CHECK(g.cells_concentration(w, one, q) == 10.0f);  // §16: no exclusion of the location's own unit
    q.exclude = -1;
    // destroyed: removed everywhere
    CHECK(g.on_object_destroyed(1));
    CHECK(g.threat(0, c010) == 0.0f && g.memory_entries(0, c010).empty() && g.live_entries(c010).empty());
    CHECK(g.cell_force(w, c010, q) == 0.0f);
    CHECK(!g.is_registered(1));

    // ---- human fog bytes: TEAM / HIDE_WHEN_FOGGED objects, FOW active ----
    w.objects[1].team_or_hide_when_fogged = true;
    w.fow_active = true;
    bool fog_on = true;
    w.object_fogged = [&](int32_t p, int32_t id) { return fog_on && p == 1 && id == 2; };
    TacticalPerceptionGrid h;
    h.init(TacticalMode::Land, w.map, 4, threat_grid_config(TacticalMode::Land, c));
    CHECK(h.register_object(w, 2, 0, 0));
    const auto tcell = static_cast<uint32_t>(h.cell_clamped(-300.0f, -301.0f));
    CHECK(h.snapshot(1, tcell) == 0.0f);  // TEAM/HIDE objects do not touch the snapshot
    h.refog(1, tcell, 50);  // human remembers the tank
    h.service_object(w, 2, 60);
    CHECK(h.fogged_for(2, 1) && !h.fogged_for(2, 0));  // AI player 0 never fogged (AIUsesFogOfWar False)
    GridQuery hq;
    hq.self = 1;
    hq.ally = false;
    // live entry skipped (fogged); memory entry: documented test also skips it while fogged
    CHECK(h.cell_force(w, tcell, hq) == 0.0f);
    hq.memory_counted_when_not_fogged = false;
    CHECK(h.cell_force(w, tcell, hq) > 0.0f);
    CHECK(!h.cell_visible(w, 1, tcell) && h.cell_visible(w, 0, tcell));
    // point danger: AI player 0 -> live layer scaled to 800^2
    const Vec3f tp{-300.0f, -301.0f, 0.0f};
    CHECK(h.point_danger(w, 1, tp.x, tp.y) == h.snapshot(1, tcell) * kLandPointDangerArea / 3600.0f);
    CHECK(h.point_danger(w, 0, 9999.0f, 0.0f) == 0.0f);
    // unfog: removed from the human's memory over the snapshot footprint
    CHECK(!h.memory_entries(1, tcell).empty());
    fog_on = false;
    h.service_object(w, 2, 61);
    CHECK(!h.fogged_for(2, 1) && h.memory_entries(1, tcell).empty());
    // owner change: re-projected for the new owner's enemies
    w.objects[1].owner = 1;
    CHECK(h.on_owner_changed(w, 2));
    CHECK(h.player_objects(0).empty() && h.player_objects(1).size() == 1);
    CHECK(h.threat(1, tcell) == 0.0f && h.threat(0, tcell) > 0.0f);
}

// ---------------------------------------------------------------------------------- tokens
struct Fixture {
    PerceptionWorld w = land_world(kLandOff);
    TacticalPerception p;
    EquationSet set;
    std::vector<HintZone> zones = land_zones();
    int32_t h_tank = -1, h_trooper = -1, h_pad = -1, h_hero = -1, h_zone2 = -1, h_outside = -1;

    Fixture() {
        constexpr float O = kLandOff;  // positions below are relative to the unshifted world
        // Empire (0): tank 10 at zone 2 (x ~ 0, y 0), build pad 12 with barracks built, hero 13
        // Rebel (1): trooper 11 near the tank; Underworld (2): tank 14; neutral (3): trooper 15
        w.objects.push_back(make_obj(10, 0, kTank, O + 0.0f, O + 0.0f, 100.0f, 100.0f));
        w.objects.push_back(make_obj(11, 1, kTrooper, O + 60.0f, O + 0.0f, 10.0f, 50.0f));
        PerceptionObject pad = make_obj(12, 0, kPad, O - 100.0f, O + 20.0f, 0.0f, 0.0f);
        pad.is_build_pad = true;
        pad.built_type = kBarracks;
        w.objects.push_back(pad);
        PerceptionObject hero = make_obj(13, 0, kHero, O - 500.0f, O + 500.0f, 300.0f, 120.0f);
        hero.contained_types = {kTrooper};
        w.objects.push_back(hero);
        w.objects.push_back(make_obj(14, 2, kTank, O + 120.0f, O + 0.0f, 100.0f, 100.0f));
        w.objects.push_back(make_obj(15, 3, kTrooper, O + 0.0f, O + 60.0f, 10.0f, 50.0f));
        PerceptionObject open_pad = make_obj(16, -1, kPad, O + 200.0f, O + 200.0f, 0.0f, 0.0f);
        open_pad.is_build_pad = true;
        open_pad.has_unit_ai = false;
        w.objects.push_back(open_pad);
        PerceptionObject barracks = make_obj(17, 0, kBarracks, O - 100.0f, O + 20.0f, 50.0f, 0.0f);
        barracks.built_structure = true;
        barracks.dummy_ground_structure = true;
        w.objects.push_back(barracks);
        w.objects[0].zone = 2;
        w.objects[1].zone = 2;
        w.objects[3].zone = -1;  // hero outside every zone
        w.players[0].reinforcements = {kTank, kTrooper};
        w.players[0].production_queue = {kHero};
        w.markers.push_back({StartMarker::Kind::PlayerPosition, 1, {O, O, 0.0f}});
        w.markers.push_back({StartMarker::Kind::PlayerPosition, 2, {O + 500.0f, O + 500.0f, 0.0f}});
        w.frame = 30;
        w.index();
        TacticalAiConstants c;
        p.init(TacticalMode::Land, c, w.map, 4, zones, test_config());
        for (const PerceptionObject& o : w.objects) p.grid().register_object(w, o.id, 1, 1);
        h_tank = p.register_unit(w, 10);
        h_trooper = p.register_unit(w, 11);
        h_pad = p.register_unit(w, 12);
        h_hero = p.register_unit(w, 13);
        h_zone2 = p.register_zone(2);
        h_outside = p.register_zone(0);
    }
    // evaluates "text" (a fresh equation); returns ok
    bool eval(const std::string& text, double& v, int32_t self = 0, int32_t target = -1, int32_t enemy = 1) {
        const std::string name = "E" + std::to_string(set.size());
        LoadReport rep;
        const uint32_t idx = set.add(name, text, &rep);
        if (idx == kUnresolved) {
            std::printf("  parse failed: %s\n", text.c_str());
            return false;
        }
        set.link();
        EvalContext ctx;
        ctx.self = self;
        ctx.target = target;
        ctx.enemy = enemy;
        ctx.human = 1;
        const EvalResult r = p.evaluate(set, idx, ctx, w);
        v = r.value;
        return r.ok;
    }
    double val(const std::string& text, int32_t self = 0, int32_t target = -1, int32_t enemy = 1) {
        double v = -12345.0;
        if (!eval(text, v, self, target, enemy)) {
            std::printf("  eval failed: %s\n", text.c_str());
            ++g_fail;
        }
        return v;
    }
};

static void test_tokens() {
    Fixture f;
    // ---- Game ----
    CHECK(f.val("Game.Age") == static_cast<double>(30.0f * (1.0f / 30.0f)));
    CHECK(f.val("Game.IsCampaignGame") == 0.0);
    // Game.ForceUnnormalized: every player's grid objects, health-scaled: 100+10+0+300+100+10+50 = 570
    CHECK(f.val("Game.ForceUnnormalized") == 570.0);
    CHECK(f.val("Game.ForceUnnormalized {Parameter_Category = GameObjectCategoryType[Vehicle]}") == 200.0);
    // ---- Player (omniscient grid lists) ----
    // Empire friendly = Empire (100+0+300+50) + Underworld ally (100) = 550; enemy = Rebel 10 + neutral-relation player 3 (10)
    CHECK(f.val("Variable_Self.FriendlyForceUnnormalized") == 550.0);
    CHECK(f.val("Variable_Self.EnemyForceUnnormalized") == 20.0);
    CHECK(f.val("Variable_Self.ForceUnnormalized") == 550.0);
    CHECK(f.val("Variable_Self.LandedForceUnnormalized") == 550.0);
    CHECK(f.val("Variable_Enemy.FriendlyForceUnnormalized") == 10.0);
    CHECK(f.val("Variable_Self.FriendlyForceUnnormalized {Parameter_Category = GameObjectCategoryType[Vehicle | LandHero]}") == 500.0);
    CHECK(near_eq(f.val("Variable_Self.FriendlyForce"), 550.0 / 570.0, 1e-12));
    CHECK(f.val("Variable_Self.ForceNBTD") == 1.0);  // same sum twice (vanilla quirk)
    CHECK(f.val("Variable_Self.ForceUnnormalized", 3) == 0.0);  // neutral faction: nothing
    CHECK(f.val("Variable_Self.IsFaction {Parameter_Faction = \"EMPIRE\"}") == 1.0);
    CHECK(f.val("Variable_Enemy.IsFaction {Parameter_Faction = \"Empire\"}") == 0.0);
    CHECK(f.val("Variable_Self.IsDifficulty {Parameter_Difficulty_Level_Type = DifficultyLevelType[Hard]}") == 1.0);
    CHECK(f.val("Variable_Enemy.IsDifficulty {Parameter_Difficulty_Level_Type = DifficultyLevelType[Hard]}") == 0.0);
    CHECK(f.val("Variable_Self.IsDefender") == 0.0);  // skirmish
    CHECK(f.val("Variable_Self.BaseLevel") == 1.0);
    CHECK(f.val("Variable_Self.CreditsUnnormalized") == 1000.0);
    CHECK(f.val("Variable_Enemy.CreditsUnnormalized") == 0.0);  // not Self
    CHECK(f.val("Variable_Self.UnitSpaceAvailable") == 20.0);
    CHECK(f.val("Variable_Self.TacticalBuiltStructureCount") == 1.0);
    CHECK(f.val("Variable_Self.TacticalBuiltStructureCount {Parameter_Type = \"E_Ground_Barracks\"}") == 1.0);
    CHECK(f.val("Variable_Self.TacticalBuiltStructureCount {Parameter_Type = \"Empire_Tank\"}") == 0.0);
    CHECK(f.val("Variable_Self.OpenBuildPadCount") == 1.0);
    double v = 0;
    CHECK(!f.eval("Variable_Enemy.OpenBuildPadCount", v));  // Self only
    CHECK(f.val("Variable_Self.ReinforcementsUnnormalized") == 110.0);
    CHECK(f.val("Variable_Self.HasUnit {Parameter_Type = \"Empire_Tank\"}") == 1.0);
    CHECK(f.val("Variable_Self.HasUnit {Parameter_Type = \"Empire_Tank\", Parameter_Count = 3}") == 0.0);
    CHECK(f.val("Variable_Self.HasUnit {Parameter_Type = \"Empire_Tank\", Parameter_Count = 2}") == 1.0);  // + reinforcement
    CHECK(f.val("Variable_Self.HasUnit {Parameter_Type = \"Darth_Vader\", Parameter_Count = 2}") == 1.0);  // + queue
    CHECK(f.val("Variable_Self.HasUnit {Parameter_Type = \"Darth_Vader\", Parameter_Count = 2, Parameter_Only_Consider_Complete = 1}") == 0.0);
    // ---- unit targets ----
    CHECK(f.val("Variable_Target.Health", 0, f.h_tank) == 1.0);
    CHECK(f.val("Variable_Target.ForceUnnormalized", 0, f.h_tank) == 100.0);
    CHECK(near_eq(f.val("Variable_Target.Force", 0, f.h_tank), 100.0 / 570.0, 1e-12));
    CHECK(f.val("Variable_Target.Force {Parameter_Category = GameObjectCategoryType[Infantry]}", 0, f.h_tank) == 0.0);
    CHECK(near_eq(f.val("Variable_Target.ForceNBTP", 0, f.h_tank), 100.0 / 550.0, 1e-12));
    CHECK(near_eq(f.val("Variable_Target.ForceNBTD", 1, f.h_trooper), 1.0, 1e-12));  // 10 / rebel friendly 10
    CHECK(f.val("Variable_Target.IsBuildPad", 0, f.h_pad) == 1.0);
    CHECK(f.val("Variable_Target.HasBuiltObject", 0, f.h_pad) == 1.0);
    CHECK(f.val("Variable_Target.HasBuiltObject {Parameter_Type = \"E_Ground_Barracks\"}", 0, f.h_pad) == 1.0);
    CHECK(f.val("Variable_Target.HasBuiltObject {Parameter_Type = \"Empire_Tank\"}", 0, f.h_pad) == 0.0);
    CHECK(f.val("Variable_Target.HasBuiltObject", 0, f.h_tank) == 0.0);
    CHECK(f.val("Variable_Target.ContainsHero", 0, f.h_hero) == 1.0);
    CHECK(f.val("Variable_Target.ContainsHero {Parameter_Type = \"Rebel_Trooper\"}", 0, f.h_hero) == 1.0);
    CHECK(f.val("Variable_Target.ContainsHero", 0, f.h_tank) == 0.0);
    CHECK(f.val("Variable_Target.Type.IsType {Parameter_Type = \"Empire_Tank\"}", 0, f.h_tank) == 1.0);
    CHECK(f.val("Variable_Target.Type.IsType {Parameter_Type = \"Rebel_Trooper\"}", 0, f.h_tank) == 0.0);
    CHECK(f.val("Variable_Target.Owner.IsFaction {Parameter_Faction = \"Rebel\"}", 0, f.h_trooper) == 1.0);
    CHECK(f.val("Variable_Target.NearbyOpenBuildPadCount {Parameter_Range = 400}", 0, f.h_tank) == 1.0);
    CHECK(!f.eval("Variable_Target.NearbyOpenBuildPadCount", v, 0, f.h_tank));  // needs Range
    // damage tracking
    f.w.objects[0].has_damage_tracking = true;
    f.w.objects[0].last_damage_frame = 20;
    CHECK(f.val("Variable_Target.HasTakenDamage {Parameter_Time = 1}", 0, f.h_tank) == 1.0);  // 30 <= 20 + 30
    f.w.frame = 51;
    CHECK(f.val("Variable_Target.HasTakenDamage {Parameter_Time = 1}", 0, f.h_tank) == 0.0);
    f.w.frame = 30;
    // distances: tank (r 5) to the nearest enemy of Self 0 = trooper 11 at 60 (3-D minus radii) = 50
    CHECK(f.val("Variable_Target.DistanceToNearestEnemy", 0, f.h_tank) == 50.0);
    // nearest friendly of the tank: Underworld tank at 120 -> 110 (the neutral-faction trooper is not friendly)
    CHECK(f.val("Variable_Target.DistanceToNearestFriendly {Parameter_Category = GameObjectCategoryType[Vehicle]}", 0, f.h_tank) == 110.0);
    CHECK(f.val("Variable_Target.DistanceToNearestEnemy {Parameter_Type = \"Darth_Vader\"}", 0, f.h_tank) == kNoDistance);
    // a path not registered on a unit evaluator fails the evaluation
    CHECK(!f.eval("Variable_Target.EnemyUnitConcentrationNBTD", v, 0, f.h_tank));
    CHECK(!f.eval("Variable_Target.EnemyForce", v, 0, f.h_tank));
    CHECK(!f.eval("Variable_Target.Health", v, 0, f.h_zone2));
    // ---- locations ----
    // zone 2 (cols 8..11, rows 8..11) holds the tank (x 0), trooper (x 60), Underworld tank (x 120),
    // neutral-faction trooper (not counted)
    const double enemy_zone = f.val("Variable_Target.EnemyForceUnnormalized", 0, f.h_zone2);
    CHECK(enemy_zone > 0.0);
    CHECK(f.val("Variable_Target.EnemyForceUnnormalized", 0, f.h_outside) == 0.0);
    CHECK(near_eq(f.val("Variable_Target.EnemyForce", 0, f.h_zone2), enemy_zone <= 570.0 ? enemy_zone / 570.0 : 1.0, 1e-9));
    const double tank_loc_enemy = f.val("Variable_Target.Location.EnemyForceUnnormalized", 0, f.h_tank);
    CHECK(tank_loc_enemy == enemy_zone);  // unit location = its current zone
    CHECK(f.val("Variable_Target.Location.EnemyForceUnnormalized", 0, f.h_hero) == 0.0);  // outside every zone
    CHECK(f.val("Variable_Target.Location.EnemyUnitConcentrationUnnormalized", 0, f.h_hero) == 0.0);
    CHECK(f.val("Variable_Target.Location.FriendlyForceUnnormalized", 0, f.h_tank) <
          f.val("Variable_Target.FriendlyForceUnnormalized", 0, f.h_zone2));  // the tank is excluded from its own location
    CHECK(f.val("Variable_Target.EnemyUnitConcentrationUnnormalized", 0, f.h_zone2) == 10.0);
    // tank 10 (100) + barracks 17 (50) + pad 12 (0) centred in zone 2; the Underworld tank (x 120) is in zone 3
    CHECK(f.val("Variable_Target.FriendlyUnitConcentrationUnnormalized", 0, f.h_zone2) == 150.0);
    CHECK(f.val("Variable_Target.Location.FriendlyUnitConcentrationUnnormalized", 0, f.h_tank) == 150.0);  // §16: tank counted
    CHECK(near_eq(f.val("Variable_Target.EnemyUnitConcentrationNBTD", 0, f.h_zone2), 10.0 / 20.0, 1e-12));
    // bitfield: types first, then categories
    CHECK(f.val("Variable_Target.EnemyUnitsBitfield {Parameter_Type = \"Rebel_Trooper\", Parameter_Type = \"Empire_Tank\", "
                "Parameter_Category = GameObjectCategoryType[Infantry]}",
                0, f.h_zone2) == 5.0);
    CHECK(f.val("Variable_Target.FriendlyUnitsBitfield {Parameter_Type = \"Rebel_Trooper\", Parameter_Type = \"Empire_Tank\"}", 0,
                f.h_zone2) == 2.0);
    CHECK(f.val("Variable_Target.DistanceToNearestEnemy", 0, f.h_zone2) == 60.0 - 5.0);  // zone centre (0,0), 2-D, minus found radius
    CHECK(f.val("Variable_Target.IsFriendlyStartLocation", 0, f.h_zone2) == 1.0);
    CHECK(near_eq(f.val("Variable_Target.IsEnemyStartLocation", 0, f.h_zone2), 0.0, 1e-12));
    CHECK(!f.eval("Variable_Target.IsFriendlyStartLocation", v, 2, f.h_zone2) || v < 1.0);
    // time last seen: no fog -> 0
    CHECK(f.val("Variable_Target.TimeLastSeenUnnormalized", 0, f.h_zone2) == 0.0);
    f.w.fow_active = true;
    f.w.point_fogged = [](int32_t, float x, float) { return x < kLandOff; };  // the left half of zone 2 is fogged
    f.w.frame = 600;
    // 8 of 16 cells fogged, never re-fogged (last seen 0): mean = 8 * 600 / 16 = 300 frames = 10 s
    // (TimeLastSeenUnnormalized has a 7 s cache; the frame-30 value was 0 -> recompute at 600)
    CHECK(f.val("Variable_Target.TimeLastSeenUnnormalized", 0, f.h_zone2) == 10.0);
    CHECK(f.val("Variable_Target.TimeLastSeen", 0, f.h_zone2) == 10.0 * 30.0 / 600.0);
    // A1_READINGS §26: a blocked cell (Steep / Water / off the grid) leaves the sum and the count. The unfogged right
    // half blocked: 8 fogged cells of 8 counted, 8 * 1200 / (8 * 30) = 40 s.
    f.w.point_blocked = [](float x, float) { return x >= kLandOff; };
    f.w.frame = 1200;
    CHECK(f.val("Variable_Target.TimeLastSeenUnnormalized", 0, f.h_zone2) == 40.0);
    f.w.point_blocked = [](float x, float) { return x < kLandOff; };  // the fogged half blocked: sum 0 -> 0
    f.w.frame = 1500;
    CHECK(f.val("Variable_Target.TimeLastSeenUnnormalized", 0, f.h_zone2) == 0.0);
    f.w.point_blocked = nullptr;
    f.w.frame = 600;  // the cache-lifetime checks below count from frame 600
    f.w.fow_active = false;
    f.w.point_fogged = nullptr;
    // point danger (Location.ForceUnnormalized) at the tank, zone total for the region
    CHECK(f.val("Variable_Target.Location.ForceUnnormalized", 0, f.h_tank) > 0.0);
    CHECK(f.val("Variable_Target.ForceUnnormalized", 0, f.h_zone2) > 0.0);
    // hints
    f.w.zone_hint = [](int32_t z, std::string_view n) { return (z == 2 && iequal(n, "High_Ground")) ? 1.0 : 0.0; };
    CHECK(f.val("Variable_Target.Hints.High_Ground", 0, f.h_zone2) == 1.0);
    CHECK(f.val("Variable_Target.Location.Hints.High_Ground", 0, f.h_tank) == 1.0);
    CHECK(f.val("Variable_Target.Hints.OffenseDelayGround", 0, f.h_zone2) == 0.0);

    // ---- owner frozen at registration ----
    f.w.objects[1].owner = 0;  // trooper captured by the Empire
    CHECK(f.val("Variable_Target.Owner.IsFaction {Parameter_Faction = \"Rebel\"}", 0, f.h_trooper) == 1.0);
    f.w.objects[1].owner = 1;

    // ---- cache lifetimes ----
    // Variable_Self.FriendlyForceUnnormalized (player, 2 s): frame 600 value cached until 660
    const double before = f.val("Variable_Self.FriendlyForceUnnormalized");
    f.w.objects[0].combat_power = 1100.0f;
    f.w.frame = 660;
    CHECK(f.val("Variable_Self.FriendlyForceUnnormalized") == before);  // still cached
    f.w.frame = 661;
    CHECK(f.val("Variable_Self.FriendlyForceUnnormalized") == before + 1000.0);
    // Game.ForceUnnormalized has its own 3 s lifetime; the normalised Self.FriendlyForce (3 s) caches its ratio
    // Unit Health has lifetime 0: always fresh
    f.w.objects[0].health = 0.25f;
    CHECK(f.val("Variable_Target.Health", 0, f.h_tank) == 0.25);
    // IsBuildPad cached forever per binding set
    f.w.objects[2].is_build_pad = false;
    CHECK(f.val("Variable_Target.IsBuildPad", 0, f.h_pad) == 1.0);
    CHECK(f.val("Variable_Target.IsBuildPad", 2, f.h_pad) == 0.0);  // other Self = other key
    // frame 0: stored values are never hits
    Fixture z;
    z.w.frame = 0;
    CHECK(z.val("Variable_Self.IsFaction {Parameter_Faction = \"Empire\"}") == 1.0);
    z.w.players[0].faction = "Rebel";
    CHECK(z.val("Variable_Self.IsFaction {Parameter_Faction = \"Empire\"}") == 0.0);
    z.w.frame = 1;
    CHECK(z.val("Variable_Self.IsFaction {Parameter_Faction = \"Empire\"}") == 0.0);
    z.w.players[0].faction = "Empire";
    CHECK(z.val("Variable_Self.IsFaction {Parameter_Faction = \"Empire\"}") == 0.0);  // forever since frame 1
    // normalised: numerator 0 -> 0 without the denominator
    CHECK(z.val("Variable_Target.Force {Parameter_Category = GameObjectCategoryType[Structure]}", 0, z.h_tank) == 0.0);

    // ---- coverage of implements() ----
    const TacticalTokenResolver r = f.p.resolver(f.w);
    TokenTable& tt = f.set.tokens();
    const TokenId loc = tt.intern("Location"), ef = tt.intern("EnemyForce"), hp = tt.intern("Health"), ty = tt.intern("Type"),
                  it = tt.intern("IsType"), gl = tt.intern("GalacticThing");
    const TokenId p1[] = {loc, ef};
    const TokenId p2[] = {hp};
    const TokenId p3[] = {ty, it};
    const TokenId p4[] = {gl};
    CHECK(r.implements(Root::Target, p1, tt) && r.implements(Root::Target, p2, tt) && r.implements(Root::Target, p3, tt));
    CHECK(!r.implements(Root::Target, p4, tt) && !r.implements(Root::Game, p2, tt));
}

// ---------------------------------------------------------------------------------- zone threat
// Script_<File>.Evaluate (AI_GALACTIC_2 §4.4): without a hook every Script_ reference fails the evaluation; with one
// the hook gets the context (Self, Target handle) and the reference's parameters in order, and its value / failure is
// the leaf's (no cache: every evaluation calls it again).
// A1 t03 (TACTICAL_GAPS 6.8): a zone repeating an earlier zone's name (any case) keeps its own handle and target, but
// evaluates through the evaluator of the first registered zone with that name; save / load keeps the mapping.
static void test_duplicate_zone_names() {
    Fixture f;
    f.zones[3].name = "ZONE2";  // repeats zone 2's name ("Zone2") in another case
    TacticalAiConstants c;
    f.p.init(TacticalMode::Land, c, f.w.map, 4, f.zones, test_config());
    for (const PerceptionObject& o : f.w.objects) f.p.grid().register_object(f.w, o.id, 1, 1);
    const int32_t h3 = f.p.register_zone(3);  // the later zone registered first: it owns the shared name id
    const int32_t h2 = f.p.register_zone(2);
    const int32_t h4 = f.p.register_zone(4);
    CHECK(h3 != h2 && f.p.target(h2).zone == 2 && f.p.target(h3).zone == 3);
    CHECK(f.p.first_zone_with_name(3) == 2 && f.p.first_zone_with_name(2) == 2 && f.p.first_zone_with_name(4) == 4);
    CHECK(f.p.first_zone_with_name(9) == -1);
    CHECK(f.p.evaluator_handle(h2) == h3 && f.p.evaluator_handle(h3) == h3 && f.p.evaluator_handle(h4) == h4);
    CHECK(f.p.evaluator_handle(-1) == -1 && f.p.evaluator_handle(999) == 999);
    // zone 2's tokens read zone 3's evaluator (its hints, its centre for distances)
    f.w.zone_hint = [](int32_t z, std::string_view n) { return (z == 3 && iequal(n, "High_Ground")) ? 1.0 : 0.0; };
    CHECK(f.val("Variable_Target.Hints.High_Ground", 0, h2) == 1.0);
    CHECK(f.val("Variable_Target.Hints.High_Ground", 0, h4) == 0.0);
    CHECK(f.val("Variable_Target.DistanceToNearestEnemy", 0, h2) == f.val("Variable_Target.DistanceToNearestEnemy", 0, h3));
    CHECK(f.val("Variable_Target.DistanceToNearestEnemy", 0, h2) != f.val("Variable_Target.DistanceToNearestEnemy", 0, h4));
    // save / load: the handles keep their zones and the shared evaluator
    ByteWriter sw;
    f.p.write(sw);
    TacticalPerception q;
    q.init(TacticalMode::Land, c, f.w.map, 4, f.zones, test_config());
    ByteReader sr(sw.bytes());
    CHECK(q.read(sr) && sr.done());
    CHECK(q.evaluator_handle(h2) == h3 && q.evaluator_handle(h3) == h3 && q.evaluator_handle(h4) == h4);
    // unique names: every zone is its own evaluator
    Fixture g;
    CHECK(g.p.evaluator_handle(g.h_zone2) == g.h_zone2 && g.p.evaluator_handle(g.h_outside) == g.h_outside);
}

// OPT2 t13: OpenBuildPadCount {Parameter_Type}: a pad counts when its own type is listed (case-insensitive), else when the
// host's pad_accepts hook accepts one of the listed types (known names only, in list order, stopping at the first yes); no
// hook means only the pad's own type counts.
static void test_open_pad_types() {
    Fixture f;
    PerceptionObject pad2 = make_obj(18, -1, kTurret, kLandOff + 300.0f, kLandOff + 200.0f, 0.0f, 0.0f);
    pad2.is_build_pad = true;
    pad2.has_unit_ai = false;
    f.w.objects.push_back(pad2);
    f.w.index();
    CHECK(f.val("Variable_Self.OpenBuildPadCount") == 2.0);
    CHECK(f.val("Variable_Self.OpenBuildPadCount {Parameter_Type = \"skirmish_build_pad\"}") == 1.0);
    CHECK(f.val("Variable_Self.OpenBuildPadCount {Parameter_Type = \"E_Ground_Barracks\"}") == 0.0);  // no hook
    struct Call {
        int32_t pad, type;
        std::string faction;
    };
    std::vector<Call> calls;
    f.w.pad_accepts = [&calls](int32_t pad_type, int32_t type, std::string_view faction) {
        calls.push_back({pad_type, type, std::string(faction)});
        return type == kBarracks && pad_type == kPad;
    };
    CHECK(f.val("Variable_Self.OpenBuildPadCount {Parameter_Type = \"No_Such_Type\", Parameter_Type = \"Darth_Vader\", "
                "Parameter_Type = \"E_Ground_Barracks\"}") == 1.0);
    const std::string faction = f.w.players[0].faction;
    CHECK(calls.size() == 4);  // pad 16: Darth_Vader, E_Ground_Barracks (yes); pad 18: both (no)
    if (calls.size() == 4) {
        CHECK(calls[0].pad == kPad && calls[0].type == kHero && calls[0].faction == faction);
        CHECK(calls[1].pad == kPad && calls[1].type == kBarracks);
        CHECK(calls[2].pad == kTurret && calls[2].type == kHero && calls[3].pad == kTurret && calls[3].type == kBarracks);
    }
    calls.clear();
    CHECK(f.val("Variable_Self.OpenBuildPadCount {Parameter_Type = \"Skirmish_Build_Pad\", Parameter_Type = \"Darth_Vader\"}") ==
          1.0);
    // pad 16 is listed by its own type; pad 18 asks for every listed type in order
    CHECK(calls.size() == 2 && calls[0].pad == kTurret && calls[0].type == kPad && calls[1].type == kHero);
}

static void test_script_hook() {
    Fixture f;
    double v = 0;
    const std::string eq =
        "2 * Script_TargetHasProperty.Evaluate{Parameter_Script_String = \"IsRushTarget\"} + 1";
    CHECK(!f.eval(eq, v, 0, f.h_tank));
    struct Call {
        int32_t self = -1, target = -1;
        std::string name;
        std::vector<std::string> params;
    };
    std::vector<Call> calls;
    f.p.set_script_evaluator([&calls](const EvalContext& c, std::string_view name, std::span<const Param> params, double& out) {
        Call k;
        k.self = c.self;
        k.target = c.target;
        k.name = std::string(name);
        for (const Param& p : params)
            k.params.push_back(p.name + "=" + (p.value.kind == ParamValue::Kind::Number ? std::to_string(p.value.number) : p.value.text));
        calls.push_back(k);
        if (name == "Fails") return false;
        out = name == "TargetHasProperty" ? 1.0 : 3000.0;
        return true;
    });
    CHECK(f.val(eq, 0, f.h_tank) == 3.0);
    CHECK(f.val(eq, 0, f.h_tank) == 3.0);  // not cached
    CHECK(calls.size() == 2 && calls[0].self == 0 && calls[0].target == f.h_tank && calls[0].name == "TargetHasProperty");
    CHECK(calls[0].params.size() == 1 && calls[0].params[0] == "Parameter_Script_String=IsRushTarget");
    calls.clear();
    CHECK(f.val("3000 / Script_GetDistanceToNearestWithProperty.Evaluate{Parameter_Script_String = \"INBASE\", "
                "Parameter_Script_String = \"FRIENDLY\"}", 1, f.h_pad) == 1.0);
    CHECK(calls.size() == 1 && calls[0].self == 1 && calls[0].target == f.h_pad && calls[0].params.size() == 2 &&
          calls[0].params[0] == "Parameter_Script_String=INBASE" && calls[0].params[1] == "Parameter_Script_String=FRIENDLY");
    CHECK(!f.eval("1 + 0 * Script_Fails.Evaluate", v, 0, f.h_tank));  // a failing evaluator fails the whole equation
    f.p.set_script_evaluator(nullptr);
    CHECK(!f.eval(eq, v, 0, f.h_tank));
    std::printf("script hook: OK\n");
}

static void test_zone_threat() {
    Fixture f;
    ZoneGraph g;
    g.build(f.zones);
    TacticalAiConstants c;
    // Self 0: zone 2 holds the rebel trooper (hostile power projected into it)
    const ZoneThreatFn threat = f.p.zone_threat_fn(f.w, 0);
    CHECK(threat(2) > 0.0 && threat(4) == 0.0);
    CHECK(threat(2) == static_cast<double>(f.p.grid().cells_danger(f.w, 0, f.p.zone_threat_cells(2))));
    // A1_READINGS §3: the path sum over the graph's weights (threat folded in by recompute_with_threat); every hop adds
    // at least 100 to the path threat, so No_Threat accepts only the source zone itself
    g.recompute_with_threat(threat);
    CHECK(zone_reachability_cost(g, 4, 4, Reachability::NoThreat, c) == 0.0);
    CHECK(zone_reachability_cost(g, 4, 3, Reachability::NoThreat, c) == kUnreachable);
    CHECK(zone_reachability_cost(g, 4, 3, Reachability::LowThreat, c) != kUnreachable);
    CHECK(zone_reachability_cost(g, 4, 1, Reachability::AnyThreat, c) != kUnreachable);  // through zone 2
}

// ---------------------------------------------------------------------------------- Lua functions
static float rsqrt_reference(float x) {
    const float x2 = 0.5f * x;
    float y = std::bit_cast<float>(0x5f375a86u - (std::bit_cast<uint32_t>(x) >> 1));
    y = y * (1.5f - (x2 * y) * y);
    y = y * (1.5f - (x2 * y) * y);
    return y;
}

static void test_lua_functions() {
    for (const float x : {1.0f, 2.0f, 0.5f, 3.0e-4f, 12345.678f, 1.0e7f})
        CHECK(std::bit_cast<uint32_t>(dmath::fast_rsqrt_retail(x)) == std::bit_cast<uint32_t>(rsqrt_reference(x)));
    const Rect2 map{-1000, -1000, 1000, 1000};
    const auto p = project_by_unit_range({0, 0, 0}, 100.0f, {30, 40, 0}, map);
    CHECK(p.has_value());
    const float inv = rsqrt_reference(2500.0f);
    CHECK(p->x == 30.0f + 30.0f * inv * 100.0f && p->y == 40.0f + 40.0f * inv * 100.0f && p->z == 0.0f);
    CHECK(!project_by_unit_range({5, 5, 5}, 100.0f, {5, 5, 5}, map).has_value());
    const auto clamped = project_by_unit_range({0, 0, 0}, 5000.0f, {900, 0, 0}, map);
    CHECK(clamped && clamped->x == 1000.0f);
    // range: max(leader's max attack range, live weapon hard points 5..10)
    PerceptionWorld w = land_world();
    // t48: an object outside the snapshot still projects, with range 0 (identical positions still give no result)
    const auto unlisted = project_by_unit_range(w, nullptr, Vec3f{0, 0, 0}, Vec3f{30, 40, 0});
    CHECK(unlisted && unlisted->x == 30.0f && unlisted->y == 40.0f);
    CHECK(!project_by_unit_range(w, nullptr, Vec3f{5, 5, 5}, Vec3f{5, 5, 5}).has_value());
    PerceptionObject leader = make_obj(1, 0, kTank, 0, 0, 1, 300.0f);
    PerceptionObject team = make_obj(2, 0, kTank, 0, 0, 1, 10.0f);
    team.team_leader = 1;
    PerceptionHardPoint hp;
    hp.type = 6;
    hp.weapon_range = 450.0f;
    team.hard_points.push_back(hp);
    hp.weapon_range = 900.0f;
    hp.destroyed = true;
    team.hard_points.push_back(hp);
    hp.type = 2;
    hp.destroyed = false;
    team.hard_points.push_back(hp);
    w.objects = {leader, team};
    w.index();
    CHECK(unit_projection_range(w, w.objects[1]) == 450.0f);
    // self threat
    const SelfThreatUnit units[] = {{10.0f, true, true}, {50.0f, false, true}, {30.0f, true, true}, {99.0f, true, false}};
    CHECK(self_threat_max(units) == 30.0f && self_threat_sum(units) == 40.0f);
    CHECK(self_threat_max({}) == 0.0f && self_threat_sum({}) == 0.0f);
    // Is_Good_Against
    galactic::ContrastTable ct;
    galactic::ContrastRow row;
    row.enemy = "Infantry";
    row.enemy_mask = 0x1;
    row.friendly.push_back({"Vehicle", 0x2, 1.5f});
    ct.rows.push_back(row);
    row.enemy = "Vehicle";
    row.enemy_mask = 0x2;
    row.friendly = {{"Vehicle", 0x2, 0.5f}};
    ct.rows.push_back(row);
    CHECK(is_good_against(ct, "Empire_Tank", 0x2, 0x1));
    CHECK(!is_good_against(ct, "Empire_Tank", 0x2, 0x2));
    CHECK(is_good_against(ct, "Empire_Tank", 0x2, 0x3));  // max over the bits
    CHECK(!is_good_against(ct, "Rebel_Trooper", 0x1, 0x1));
    // Get_Most_Defended_Position: allied force of `player`, ties -> one Get_Float01 draw each
    PerceptionWorld lw = land_world();
    lw.objects.push_back(make_obj(1, 0, kTank, -570.0f, 570.0f, 100.0f, 10.0f));   // cell 0 only
    lw.objects.push_back(make_obj(2, 2, kTank, -510.0f, 570.0f, 100.0f, 10.0f));   // ally, cell 1 only
    lw.objects.push_back(make_obj(3, 1, kTank, -450.0f, 570.0f, 500.0f, 10.0f));   // enemy, cell 2
    lw.index();
    TacticalPerceptionGrid g;
    g.init(TacticalMode::Land, lw.map, 4, threat_grid_config(TacticalMode::Land, TacticalAiConstants()));
    for (int32_t id = 1; id <= 3; ++id) CHECK(g.register_object(lw, id, 0, 0));
    const uint32_t cells[] = {0, 1, 2, 3};
    SimRng rng(77), ref(77);
    const DefendedPosition d = most_defended_position_cells(g, lw, cells, 0, rng);
    CHECK(d.found() && d.draws == 1);
    const float draw = ref.inline_unit_float();
    CHECK(d.cell == (draw > 0.5f ? 1 : 0));
    CHECK(rng.state() == ref.state());
    const Vec3f expect = g.cell_center(d.cell, 0);
    CHECK(d.position.x == expect.x && d.position.y == expect.y && d.position.z == 0.0f);
    // §17: the running best starts at -1, so zero-force cells still give a result: the first is taken, the 0 == 0 tie draws
    const uint32_t empty_cells[] = {3, 4};
    const float zdraw = ref.inline_unit_float();
    const DefendedPosition zero = most_defended_position_cells(g, lw, empty_cells, 0, rng);
    CHECK(zero.found() && zero.draws == 1 && zero.cell == (zdraw > 0.5f ? 4 : 3) && rng.state() == ref.state());
    const DefendedPosition none = most_defended_position_cells(g, lw, std::span<const uint32_t>(), 0, rng);
    CHECK(!none.found() && none.position.x == kNoDefendedPosition && none.draws == 0);
    TacticalPerceptionGrid empty_grid;  // nothing registered: category union 0 -> no position, no draws
    empty_grid.init(TacticalMode::Land, lw.map, 4, threat_grid_config(TacticalMode::Land, TacticalAiConstants()));
    const DefendedPosition unmasked = most_defended_position_cells(empty_grid, lw, cells, 0, rng);
    CHECK(!unmasked.found() && unmasked.draws == 0 && rng.state() == ref.state());
    const DefendedPosition enemy_view = most_defended_position_cells(g, lw, cells, 1, rng);
    // the enemy tank (radius = half diagonal) spreads over cells 1..3 equally: ties draw
    CHECK(enemy_view.found() && enemy_view.cell >= 1 && enemy_view.draws == 2);
}

// ---------------------------------------------------------------------------------- scenario CRC
static const char* kScenarioEquations[][2] = {
    {"ThreatAtTarget", "Variable_Target.EnemyForceUnnormalized {Parameter_Attenuator = 0.5}"},
    {"UnitThreat", "Variable_Target.Location.EnemyForce + Variable_Target.Force * 2"},
    {"Pressure", "Variable_Self.FriendlyForce - Variable_Self.EnemyForce + Game.Age / 100"},
    {"Hidden", "Variable_Target.TimeLastSeenUnnormalized + Variable_Target.DistanceToNearestEnemy / 1000"},
    {"Conc", "Variable_Target.EnemyUnitConcentrationNBTD + Variable_Target.FriendlyUnitConcentration"},
    {"Danger", "Variable_Target.ForceUnnormalized + Variable_Target.Location.ForceUnnormalized"},
    {"Bits", "Variable_Target.EnemyUnitsBitfield {Parameter_Type = \"Empire_Tank\", Parameter_Category = GameObjectCategoryType[Infantry]}"},
    {"Health", "Variable_Target.Health * Variable_Target.Owner.ForceNBTD + Variable_Target.HardPointHealth"},
};

static uint64_t g_scn_ok = 0, g_scn_total = 0;

// save_at != 0 (N1 save / load): at that frame the perception is written, read into a fresh one init()ed alike
// (truncated images refused first, the reader unchanged) and the run continues on the restored one.
static size_t g_scn_save_bytes = 0;
static uint64_t scenario_crc(const ParallelFor& pf, TacticalMode mode, uint32_t save_at = 0) {
    SimRng rng(mode == TacticalMode::Land ? 0xA1B0u : 0xA1B1u);
    PerceptionWorld w = land_world(kLandOff);
    w.mode = mode;
    if (mode == TacticalMode::Space) w.map = {-5000.0f, -5000.0f, 5000.0f, 5000.0f};
    const float span = w.map.max_x - w.map.min_x;
    std::vector<HintZone> zones;
    if (mode == TacticalMode::Land) zones = land_zones();
    auto owner = std::make_unique<TacticalPerception>();
    TacticalPerception* p = owner.get();
    TacticalAiConstants c;
    p->init(mode, c, w.map, 4, zones, test_config());
    int32_t next_id = 1;
    auto spawn = [&] {
        const int32_t owner = rng.range_int(0, 3);
        const int32_t type = mode == TacticalMode::Land ? rng.range_int(0, 4) : (rng.range_int(0, 1) ? kStarDestroyer : kXWing);
        PerceptionObject o = make_obj(next_id++, owner, type, w.map.min_x + rng.range_float(0.0f, span),
                                      w.map.min_y + rng.range_float(0.0f, span), w.types[static_cast<size_t>(type)].combat_power,
                                      rng.range_float(20.0f, mode == TacticalMode::Land ? 300.0f : 3000.0f));
        o.mobile = rng.range_int(0, 1) != 0;
        o.team_or_hide_when_fogged = rng.range_int(0, 2) == 0;
        o.zone = mode == TacticalMode::Land ? rng.range_int(-1, 4) : -1;
        if (type == kStarDestroyer) {
            for (int k = 0; k < 4; ++k) {
                PerceptionHardPoint hp;
                hp.type = 5 + k;
                hp.weapon_range = rng.range_float(500.0f, 2500.0f);
                hp.power = rng.range_float(50.0f, 500.0f);
                hp.destroyable = true;
                hp.turret = k % 2 == 0;
                hp.world_position = {o.position.x + 50.0f * static_cast<float>(k), o.position.y, 0.0f};
                o.hard_points.push_back(hp);
            }
        }
        w.objects.push_back(o);
        w.index();
        p->grid().register_object(w, o.id, w.frame, draw_unit_ai_stamps(rng, w.frame).reprojection_check);
        p->register_unit(w, o.id);
    };
    for (int i = 0; i < 40; ++i) spawn();
    for (uint32_t z = 0; z < zones.size(); ++z) p->register_zone(static_cast<int32_t>(z));
    w.fow_active = mode == TacticalMode::Land;
    w.object_fogged = [&](int32_t pl, int32_t id) { return ((static_cast<uint32_t>(id) * 2654435761u) >> 29) == static_cast<uint32_t>(pl); };
    w.point_fogged = [&](int32_t pl, float x, float y) { return (static_cast<int32_t>(x + y) / 97 + pl) % 3 == 0; };
    EquationSet set;
    for (const auto& e : kScenarioEquations) set.add(e[0], e[1]);
    set.link();
    StateHasher h;
    std::vector<uint64_t> out;
    for (uint32_t frame = 15; frame <= 1800; frame += 15) {
        w.frame = frame;
        // churn: move, damage, spawn, destroy, capture
        for (PerceptionObject& o : w.objects) {
            if (!o.mobile || !o.alive) continue;
            move_obj(o, std::clamp(o.position.x + rng.range_float(-40.0f, 40.0f), w.map.min_x, w.map.max_x),
                     std::clamp(o.position.y + rng.range_float(-40.0f, 40.0f), w.map.min_y, w.map.max_y));
            o.health = std::max(0.05f, o.health - rng.range_float(0.0f, 0.02f));
            for (PerceptionHardPoint& hp : o.hard_points)
                if (rng.range_int(0, 30) == 0) hp.destroyed = true;
        }
        if (rng.range_int(0, 2) == 0) spawn();
        if (rng.range_int(0, 3) == 0) {
            const int32_t dead = rng.range_int(1, next_id - 1);
            if (p->grid().on_object_destroyed(dead)) {
                p->on_object_destroyed(dead);
                for (PerceptionObject& o : w.objects)
                    if (o.id == dead) o.alive = false;
            }
        }
        if (rng.range_int(0, 9) == 0) {
            const int32_t cap = rng.range_int(1, next_id - 1);
            for (PerceptionObject& o : w.objects)
                if (o.id == cap && o.alive) o.owner = (o.owner + 1) % 4;
            p->grid().on_owner_changed(w, cap);
        }
        p->grid().service_all(w, frame);
        for (int k = 0; k < 3; ++k)
            p->grid().refog(rng.range_int(0, 3), static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(p->grid().cell_count()) - 1)), frame);
        // evaluate every (AI player, target, equation) in parallel, per-index outputs
        const int32_t selfs[] = {0, 2};
        const auto targets = static_cast<uint32_t>(p->target_count());
        const auto neq = static_cast<uint32_t>(set.size());
        const uint32_t n = 2 * targets * neq;
        out.assign(n, 0);
        pf(n, 16, [&](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                EvalContext ctx;
                ctx.self = selfs[i / (targets * neq)];
                ctx.target = static_cast<int32_t>((i / neq) % targets);
                ctx.enemy = 1;
                ctx.human = 1;
                const EvalResult r = p->evaluate(set, i % neq, ctx, w);
                out[i] = r.ok ? std::bit_cast<uint64_t>(r.value) : 0xFFFFFFFFFFFFFFFFull;
            }
        });
        for (const uint64_t x : out) {
            h.u64(x);
            g_scn_ok += x != 0xFFFFFFFFFFFFFFFFull ? 1u : 0u;
        }
        g_scn_total += out.size();
        // Lua functions (serial; draws)
        const DefendedPosition d = mode == TacticalMode::Land
                                       ? most_defended_position_cells(p->grid(), w, p->zone_threat_cells(static_cast<int32_t>(frame / 15 % 5)), 0, rng)
                                       : most_defended_position_space(p->grid(), w, {0.0f, 0.0f, 0.0f}, c.space_region_size, 0, rng);
        h.i32(d.cell);
        h.u32(d.draws);
        if (mode == TacticalMode::Land) {
            const ZoneThreatFn zt = p->zone_threat_fn(w, 0);
            for (uint32_t z = 0; z < zones.size(); ++z) h.f64(zt(z));
        }
        if (frame == save_at) {
            ByteWriter sw;
            p->write(sw);
            auto q = std::make_unique<TacticalPerception>();
            q->init(mode, c, w.map, 4, zones, test_config());
            StateHasher fresh;
            q->hash(fresh);
            bool refused = true;
            const size_t len = sw.bytes().size();
            for (size_t cut = 0; cut < len; cut += 1 + len / 211) {
                ByteReader tr(std::span<const uint8_t>(sw.bytes().data(), cut));
                refused = refused && !q->read(tr);
                StateHasher after;
                q->hash(after);
                refused = refused && after.digest() == fresh.digest();
            }
            CHECK(refused);
            ByteReader sr(sw.bytes());
            CHECK(q->read(sr) && sr.done());
            StateHasher hash_p, hash_q;
            p->hash(hash_p);
            q->hash(hash_q);
            CHECK(hash_p.digest() == hash_q.digest());
            ByteWriter again;
            q->write(again);
            CHECK(again.bytes() == sw.bytes());
            g_scn_save_bytes = len;
            owner = std::move(q);
            p = owner.get();
        }
    }
    p->hash(h);
    rng.hash(h);
    return h.digest();
}

static void test_scenario(const char* golden, const char* write_golden) {
    uint64_t r[2][3] = {};
    const TacticalMode modes[] = {TacticalMode::Land, TacticalMode::Space};
    for (int m = 0; m < 2; ++m) {
        g_scn_ok = g_scn_total = 0;
        r[m][0] = scenario_crc(serial_for, modes[m]);
        std::printf("scenario %s: %" PRIu64 " of %" PRIu64 " evaluations succeeded\n", m == 0 ? "land" : "space", g_scn_ok, g_scn_total);
        CHECK(g_scn_ok * 3 > g_scn_total);
        r[m][1] = scenario_crc(make_thread_pf(4), modes[m]);
        r[m][2] = scenario_crc(make_thread_pf(std::max(2u, std::thread::hardware_concurrency())), modes[m]);
        CHECK(r[m][0] == r[m][1] && r[m][0] == r[m][2]);
    }
    StateHasher h;
    h.u64(r[0][0]);
    h.u64(r[1][0]);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016" PRIx64, h.digest());
    std::printf("perception scenario crc %s (land %016" PRIx64 ", space %016" PRIx64 ")\n", buf, r[0][0], r[1][0]);
    if (write_golden) {
        std::ofstream(write_golden) << buf << "\n";
        std::printf("golden written: %s\n", write_golden);
    }
    if (golden) {
        std::ifstream in(golden);
        std::string want;
        in >> want;
        if (want.empty()) {
            std::printf("FAIL golden %s missing\n", golden);
            ++g_fail;
        } else if (want != buf) {
            std::printf("FAIL golden mismatch: want %s got %s\n", want.c_str(), buf);
            ++g_fail;
        }
    }
}

// N1 save / load: the scenario saved and restored mid-run (fog memory, snapshots, the leaf cache) ends with the CRC of
// the uninterrupted run, serial and threaded.
static void test_scenario_save() {
    const TacticalMode modes[] = {TacticalMode::Land, TacticalMode::Space};
    for (const TacticalMode mode : modes) {
        const uint64_t plain = scenario_crc(serial_for, mode);
        g_scn_save_bytes = 0;
        const uint64_t saved = scenario_crc(serial_for, mode, 900);
        const uint64_t threaded = scenario_crc(make_thread_pf(4), mode, 450);
        CHECK(g_scn_save_bytes > 0 && plain == saved && plain == threaded);
        std::printf("save: perception %s %zu bytes, restored run crc %016" PRIx64 " %s\n", mode == TacticalMode::Land ? "land" : "space",
                    g_scn_save_bytes, saved, plain == saved && plain == threaded ? "identical" : "DIFFERS");
    }
}

// ---------------------------------------------------------------------------------- real data
#if defined(ALAMO_AI_TACTICAL_HAVE_VFS)
static bool is_tactical_file(std::string_view path) {
    std::string up(path);
    for (char& ch : up) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    if (up.find("GALACTIC") != std::string::npos || up.find("BUDGET") != std::string::npos || up.find("STORY") != std::string::npos)
        return false;
    return up.find("LAND") != std::string::npos || up.find("SPACE") != std::string::npos || up.find("HERO") != std::string::npos;
}

static void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real data: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto fs = vfs::Vfs::create(cfg, &err);
    if (!fs) std::printf("real data: VFS: %s\n", err.c_str());
    CHECK(fs != nullptr);
    if (!fs) return;
    const std::vector<std::string> files = fs->list(kPerceptionEquationGlob);
    EquationSet set;
    const LoadReport rep = load_equation_files(*fs, files, set);
    CHECK(rep.files > 0 && rep.parse_errors == 0);
    // worlds: land (zones) and space (cells), objects of every test type, default category table.
    // An equation must evaluate for some target in some mode (land-only tokens fail in space and
    // vice versa, as in the original); Script_ references are not evaluated by S2.
    std::vector<uint8_t> ok_in(set.size(), 0), script_in(set.size(), 0), galactic_tokens(set.size(), 0);
    uint32_t unimpl = 0, paths = 0;
    for (const TacticalMode mode : {TacticalMode::Land, TacticalMode::Space}) {
        const float off = mode == TacticalMode::Land ? kLandOff : 0.0f;
        PerceptionWorld w = land_world(off);
        w.mode = mode;
        w.frame = 900;
        if (mode == TacticalMode::Space) w.map = {-5000, -5000, 5000, 5000};
        w.markers.push_back({StartMarker::Kind::PlayerPosition, 1, {off, off, 0}});
        w.markers.push_back({StartMarker::Kind::PlayerPosition, 2, {off + 300, off + 300, 0}});
        // Shipped FoC zones carry no hints, so Hints.<name> fails there (A1_READINGS §22; e.g. Is_Strong_Position's
        // Hints.High_Ground). A host hint of 0 stands in so the check still covers the rest of those equations.
        if (mode == TacticalMode::Land) w.zone_hint = [](int32_t, std::string_view) { return 0.0; };
        const std::vector<HintZone> zones = mode == TacticalMode::Land ? land_zones() : std::vector<HintZone>{};
        TacticalPerception p;
        p.init(mode, TacticalAiConstants(), w.map, 4, zones);  // default (objects) category table
        for (int32_t i = 0; i < 24; ++i) {
            const int32_t type = i % 8;
            PerceptionObject o = make_obj(100 + i, i % 4, type, off + static_cast<float>(i * 37 % 1000) - 500.0f,
                                          off + static_cast<float>(i * 53 % 1000) - 500.0f, 10.0f * static_cast<float>(i + 1), 150.0f);
            o.zone = mode == TacticalMode::Land ? i % 5 : -1;
            o.is_build_pad = type == kPad;
            w.objects.push_back(o);
        }
        w.index();
        std::vector<int32_t> handles;
        for (const PerceptionObject& o : w.objects) {
            p.grid().register_object(w, o.id, 1, 1);
            handles.push_back(p.register_unit(w, o.id));
        }
        if (mode == TacticalMode::Land)
            for (int32_t z = 0; z < 5; ++z) handles.push_back(p.register_zone(z));
        else
            for (int32_t cx = 0; cx < 5; ++cx)
                handles.push_back(p.register_cell(cx, 0, {-5000.0f + 2000.0f * static_cast<float>(cx), -5000.0f,
                                                          -3000.0f + 2000.0f * static_cast<float>(cx), -3000.0f}));
        uint32_t evals = 0, oks = 0;
        for (uint32_t e = 0; e < set.size(); ++e) {
            const Equation& eq = set.equation(e);
            if (!eq.parsed || !is_tactical_file(eq.file)) continue;
            for (const int32_t hnd : handles) {
                EvalContext ctx;
                ctx.self = 0;
                ctx.enemy = 1;
                ctx.human = 1;
                ctx.target = hnd;
                SimRng rng(5);
                ctx.rng = &rng;
                const EvalResult res = p.evaluate(set, e, ctx, w);
                ++evals;
                if (res.ok) {
                    ok_in[e] = 1;
                    ++oks;
                }
                if (res.error == EvalError::Script) script_in[e] = 1;
            }
        }
        std::printf("real data (%s): %u evaluations, %u ok\n", mode == TacticalMode::Land ? "land" : "space", evals, oks);
        if (mode == TacticalMode::Land) {
            const TacticalTokenResolver r = p.resolver(w);
            for (const CoverageEntry& ce : coverage(set, &r)) {
                if (ce.kind != CoverageEntry::Kind::Token) continue;
                ++paths;
                if (!ce.implemented) ++unimpl;
            }
            for (uint32_t e = 0; e < set.size(); ++e) {  // equations naming a token no tactical evaluator has
                const Equation& eq = set.equation(e);
                if (!eq.parsed) continue;
                for (const ExprNode& nd : eq.expr.nodes) {
                    if (nd.op != Op::Token) continue;
                    const std::span<const TokenId> pth(eq.expr.path.data() + nd.path_begin, nd.path_len);
                    if (!r.implements(nd.root, pth, set.tokens())) galactic_tokens[e] = 1;
                }
            }
        }
    }
    uint32_t tactical = 0, ok_any = 0, script_only = 0, failing = 0, galactic = 0;
    for (uint32_t e = 0; e < set.size(); ++e) {
        const Equation& eq = set.equation(e);
        if (!eq.parsed || !is_tactical_file(eq.file)) continue;
        ++tactical;
        if (ok_in[e]) ++ok_any;
        else if (galactic_tokens[e]) {
            ++galactic;
            std::printf("  galactic tokens only (not a tactical evaluator path): %s (%s)\n", eq.name.c_str(), eq.file.c_str());
        }
        else if (script_in[e]) ++script_only;
        else {
            ++failing;
            std::printf("  fails: %s (%s)\n", eq.name.c_str(), eq.file.c_str());
        }
    }
    std::printf("real data: %u equations (%u files, %u parse errors); %u in tactical files: %u evaluate, %u only fail through "
                "Script_, %u use galactic tokens, %u fail; token paths over all files %u, not implemented by the tactical "
                "resolver %u (galactic tokens)\n",
                rep.equations, rep.files, rep.parse_errors, tactical, ok_any, script_only, galactic, failing, paths, unimpl);
    CHECK(tactical > 0 && failing == 0);
}
#endif

// A1 g t02 / t03 (A1_READINGS §30.1, §30.3, §30.4).
static void test_a1g_readings() {
    {  // §30.1: both zone orders hold the zone's threat cells once each; +0x90 = copy-assign of +0xb0
        Fixture f;
        for (uint32_t z = 0; z < 5; ++z) {
            const std::span<const uint32_t> ev = f.p.zone_eval_cells(static_cast<int32_t>(z));
            const std::span<const uint32_t> zo = f.p.zone_threat_cells(static_cast<int32_t>(z));
            std::vector<uint32_t> want;
            for (uint32_t r = 8; r <= 11; ++r)
                for (uint32_t c = 4 * z; c < 4 * z + 4; ++c) want.push_back(r * 20 + c);
            std::vector<uint32_t> a(ev.begin(), ev.end()), b(zo.begin(), zo.end());
            StdextHashOrder src, dst;
            for (const uint32_t cell : a) src.insert((cell % 20) | ((cell / 20) << 16));
            dst.assign_from(src);
            std::vector<uint32_t> regrouped;
            for (const uint32_t k : dst.order()) regrouped.push_back((k >> 16) * 20 + (k & 0xFFFFu));
            CHECK(b == regrouped);
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            CHECK(a == want && b == want);
        }
        CHECK(f.p.zone_eval_cells(-1).empty() && f.p.zone_threat_cells(5).empty());
    }
    {  // §30.3: a TRANSPORT_LANDING object adds its source's representative type's power, not its own
        Fixture f;
        PerceptionObject land = make_obj(20, 0, kTank, kLandOff + 300.0f, kLandOff + 300.0f, 7.0f, 0.0f);
        land.has_unit_ai = false;
        land.transport_landing = true;
        land.parent_object = 500;
        f.w.objects.push_back(land);
        f.w.index();
        f.w.parent_rep_type = [](int32_t id) { return id == 500 ? int32_t{kStarDestroyer} : -1; };
        const auto friendly = [&f] {
            f.p.cache().clear();
            return f.val("Variable_Self.FriendlyForceUnnormalized");
        };
        f.w.skirmish_or_mp = true;  // (a) skirmish: the landing object itself, its type's power (Empire_Tank 100)
        CHECK(friendly() == 650.0);
        f.w.skirmish_or_mp = false;  // (b) campaign, parent found: the parent's type (Star_Destroyer 2000)
        CHECK(friendly() == 2550.0);
        f.w.objects.back().parent_object = 501;  // (c) campaign, parent not found: skipped
        CHECK(friendly() == 550.0);
        f.w.objects.back().parent_object = -1;  // no parent id: the object itself
        CHECK(friendly() == 650.0);
        f.w.objects.back().parent_object = 500;
        f.w.parent_rep_type = nullptr;  // no resolver: never found
        CHECK(friendly() == 550.0);
        f.p.cache().clear();
        CHECK(f.val("Variable_Self.FriendlyForceUnnormalized {Parameter_Category = GameObjectCategoryType[Infantry]}") == 0.0);
        // (d) a1h t08 (§32.2): a reinforcement's landing object carries its company type (transport +0x48, the host's
        // rep_type): in skirmish and without a parent id T is that carried type, not the dropship's own type.
        f.w.parent_rep_type = [](int32_t id) { return id == 500 ? int32_t{kStarDestroyer} : -1; };
        f.w.objects.back().rep_type = kStarDestroyer;
        f.w.objects.back().parent_object = -1;
        CHECK(friendly() == 2550.0);  // campaign, no parent id: the carried type (Star_Destroyer 2000)
        f.w.skirmish_or_mp = true;
        f.w.objects.back().parent_object = 500;
        CHECK(friendly() == 2550.0);  // skirmish: the landing object's own carried type, the parent is not read
        f.w.objects.back().rep_type = -1;  // a cinematic transport (none carried): its own type
        CHECK(friendly() == 650.0);
    }
    {  // §30.4: IsValidBombingTarget fails inside any active base shield (host callback, 3-D); IsInside*Shield stay 0
        Fixture f;
        const Vec3f shield{kLandOff, kLandOff, 0.0f};
        f.w.in_active_base_shield = [shield](const Vec3f& p) {
            const float dx = p.x - shield.x, dy = p.y - shield.y, dz = p.z - shield.z;
            return dx * dx + dy * dy + dz * dz < 150.0f * 150.0f;
        };
        CHECK(f.val("Variable_Target.IsValidBombingTarget", 0, f.h_tank) == 0.0);    // own tank inside the shield
        CHECK(f.val("Variable_Target.IsValidBombingTarget", 1, f.h_tank) == 0.0);    // for the enemy too (any owner)
        CHECK(f.val("Variable_Target.IsValidBombingTarget", 0, f.h_hero) == 1.0);    // the hero is outside it
        CHECK(f.val("Variable_Target.IsValidBombingTarget", 0, f.h_zone2) == 0.0);   // zone 2's centre is inside
        CHECK(f.val("Variable_Target.IsInsideEnemyShield", 0, f.h_tank) == 0.0);
        CHECK(f.val("Variable_Target.IsInsideFriendlyShield", 0, f.h_tank) == 0.0);
        Fixture g;  // 3-D prevention distance: Self's barracks (id 17) with Bombing_Run_Prevention_Radius 50
        g.w.types[kBarracks].bombing_run_prevention_radius = 50.0f;
        PerceptionObject& barracks = g.w.objects[7];
        CHECK(barracks.id == 17);
        barracks.position = {kLandOff, kLandOff, 60.0f};  // straight above the tank: 2-D 0, 3-D 60
        CHECK(g.val("Variable_Target.IsValidBombingTarget", 0, g.h_tank) == 1.0);
        g.p.cache().clear();
        barracks.position = {kLandOff, kLandOff, 40.0f};
        CHECK(g.val("Variable_Target.IsValidBombingTarget", 0, g.h_tank) == 0.0);
        g.p.cache().clear();
        CHECK(g.val("Variable_Target.IsValidBombingTarget", 1, g.h_tank) == 1.0);  // only Self's objects prevent
    }
    std::printf("a1g readings: OK\n");
}

int main(int argc, char** argv) {
    const char* golden = nullptr;
    const char* write_golden = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--golden") == 0) golden = argv[++i];
        else if (std::strcmp(argv[i], "--write-golden") == 0) write_golden = argv[++i];
    }
    test_cache_rules();
    test_grid_geometry();
    test_grid_projection();
    test_tokens();
    test_duplicate_zone_names();
    test_open_pad_types();
    test_script_hook();
    test_zone_threat();
    test_lua_functions();
    test_a1g_readings();
    test_scenario(golden, write_golden);
    test_scenario_save();
#if defined(ALAMO_AI_TACTICAL_HAVE_VFS)
    test_real_data();
#endif
    if (g_fail) {
        std::printf("alamo_ai_tactical_perception_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ai_tactical_perception_tests: OK\n");
    return 0;
}
