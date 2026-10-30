// alamo_ai_tactical tests (no framework; non-zero exit on failure).
// A1 stage 1 (docs/re/AI_TACTICAL.md): target lists (unit / land hint-zone / space grid), Applies,
// zone reachability, FindTarget selection (draw counts, P(best) = p), DAMAGE_TRACKING +
// FindDeadlyEnemy, free-store admission + service cadence, land credit transfer, a scripted
// scenario CRC (golden shared by MSVC / clang / gcc; zone all-pairs serial vs threaded) and, with
// ALAMO_FOC_DATA, every shipped map's hint zones and space grid.
//
//   alamo_ai_tactical_tests [--golden <file>] [--write-golden <file>]
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "alamo/ai/tactical/applies.hpp"
#include "alamo/ai/tactical/constants.hpp"
#include "alamo/ai/tactical/damage_tracking.hpp"
#include "alamo/ai/tactical/find_target.hpp"
#include "alamo/ai/tactical/free_store.hpp"
#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/ai/tactical/land_connectivity.hpp"
#include "alamo/ai/tactical/tactical_credits.hpp"
#include "alamo/ai/tactical/target_list.hpp"
#include "alamo/ai/tactical/zone_reachability.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

#if defined(ALAMO_AI_TACTICAL_HAVE_DATA) && defined(ALAMO_AI_TACTICAL_HAVE_TERRAIN)
#include "alamo/data/game_database.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/vfs/vfs.hpp"
#define AI_TACTICAL_REAL_DATA 1
#endif

using namespace alamo;
using namespace alamo::ai::tactical;
namespace econ = ::alamo::galactic::econ;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
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

static TargetObjectInfo obj(int32_t id, bool land, bool space_rep, bool space_own, bool dead = false) {
    TargetObjectInfo o;
    o.id = id;
    o.dead_or_removing = dead;
    o.rep_has_land_evaluator = land;
    o.rep_has_space_evaluator = space_rep;
    o.own_has_space_evaluator = space_own;
    return o;
}

static HintZone zone(uint32_t id, const char* name, float x, float y, std::vector<uint32_t> links) {
    HintZone z;
    z.id = id;
    z.name = name;
    z.center[0] = x;
    z.center[1] = y;
    for (uint32_t l : links) z.links.push_back({l, 1});  // non-zero link flags (TACTICAL_GAPS §6.1 edge rule)
    return z;
}

// ---------------------------------------------------------------- 3.1 / 3.2 target lists
static void test_target_list_land() {
    TacticalTargetList t(TacticalMode::Land);
    const TargetObjectInfo existing[] = {obj(5, true, false, false), obj(3, false, true, true), obj(9, true, false, false, true),
                                         obj(7, true, false, false), obj(5, true, false, false)};
    CHECK(t.add_existing_objects(existing) == 2);  // 5, 7 (3 has no land evaluator, 9 dead, 5 duplicate)
    const HintZone zones[] = {zone(1, "ZoneA", 10, 20, {}), zone(2, "ZoneB", 30, 40, {})};
    CHECK(t.add_land_regions(zones) == 2);
    CHECK(t.size() == 4);
    CHECK(t[0].object_id == 5 && t[0].name == "OBJECT_5" && t[0].is_unit());
    CHECK(t[1].object_id == 7);
    CHECK(t[2].kind == TargetKind::Region && t[2].name == "ZoneA" && t[2].zone == 0 && t[2].position.x == 10.0f);
    CHECK(t[3].name == "ZoneB" && t[3].has_reachability() && !t[0].has_reachability());
    CHECK(t.build_space_grid({-5000, -5000, 5000, 5000}, 2000.0f, true) == 0);  // land: no grid
    // runtime: appended after the regions; representative type rule
    CHECK(t.on_object_added(obj(12, true, false, false)));
    CHECK(!t.on_object_added(obj(12, true, false, false)));  // already registered
    CHECK(!t.on_object_added(obj(13, false, true, true)));
    CHECK(!t.on_object_added(obj(14, true, false, false, true)));
    CHECK(t.size() == 5 && t[4].object_id == 12);
    // destroyed: erased in place, order kept
    CHECK(t.on_object_destroyed(7));
    CHECK(!t.on_object_destroyed(7));
    CHECK(t.size() == 4 && t[0].object_id == 5 && t[1].name == "ZoneA" && t[2].name == "ZoneB" && t[3].object_id == 12);
    CHECK(!t.is_registered(7));
    CHECK(t.on_object_added(obj(7, true, false, false)));  // re-adding after death appends
    CHECK(t[4].object_id == 7);
    CHECK(t.find_region("zoneb") == 2 && t.find_region("ZoneC") == -1 && t.find_object(12) == 3);
    // a map without hint zones has no Tactical_Location targets
    TacticalTargetList none(TacticalMode::Land);
    CHECK(none.add_land_regions({}) == 0 && none.size() == 0);
    const TacticalTarget& za = t[1];
    const Rect2 rr = region_rect(za, 2000.0f);
    CHECK(rr.min_x == -990.0f && rr.max_y == 1020.0f);
}

static void test_target_list_space() {
    TacticalTargetList t(TacticalMode::Space);
    const TargetObjectInfo existing[] = {obj(1, true, true, false), obj(2, false, false, true), obj(4, false, true, true)};
    CHECK(t.add_existing_objects(existing) == 2);  // space init tests the OWN type: 2 and 4
    CHECK(t[0].object_id == 2 && t[1].object_id == 4);
    CHECK(t.on_object_added(obj(6, false, true, false)));   // runtime: representative type
    CHECK(!t.on_object_added(obj(8, false, false, true)));
    CHECK(t.add_land_regions(std::vector<HintZone>{zone(1, "Z", 0, 0, {})}) == 0);  // space: no hint zones
    // no threat grid -> no cells
    CHECK(t.build_space_grid({-5000, -5000, 5000, 5000}, 2000.0f, false) == 0);
    TacticalTargetList g(TacticalMode::Space);
    CHECK(g.build_space_grid({-5000, -5000, 5000, 5000}, 2000.0f, true) == 25);
    CHECK(g[0].name == "CELL_0_0" && g[1].name == "CELL_0_1" && g[5].name == "CELL_1_0" && g[24].name == "CELL_4_4");
    CHECK(g[0].position == (Vec3f{-4000.0f, -4000.0f, 0.0f}));
    CHECK(!g[0].has_reachability() && g[0].kind == TargetKind::Region);
    // ceil + clipping: 10001 wide -> 6 columns; the last column's rect is clipped, its centre is not
    TacticalTargetList c(TacticalMode::Space);
    CHECK(c.build_space_grid({0, 0, 10001, 3000}, 2000.0f, true) == 12);
    const TacticalTarget& last = c[11];
    CHECK(last.name == "CELL_5_1" && last.cell_x == 5 && last.cell_y == 1);
    CHECK(last.eval_rect == (Rect2{10000.0f, 2000.0f, 10001.0f, 3000.0f}));
    CHECK(last.position == (Vec3f{11000.0f, 3000.0f, 0.0f}));
    CHECK(c.build_space_grid({0, 0, 0, 0}, 2000.0f, true) == 0);
    CHECK(c.build_space_grid({0, 0, 100, 100}, 0.0f, true) == 0);
    CHECK(unit_target_name(-3) == "OBJECT_-3" && cell_target_name(10, 2) == "CELL_10_2");
}

// ---------------------------------------------------------------- 3.3 Applies
static void test_applies() {
    UnitAppliesInfo u;
    u.object_id = 1;
    // class order: locomotor > build pad > cash point > reinforcement point > structure
    u.has_locomotor = true;
    u.has_build_objects = true;
    u.has_cash_point = true;
    u.reinforcement_enabling_radius = 100.0f;
    CHECK(classify_unit(u).friendly == kAppFriendlyUnit && classify_unit(u).enemy == kAppEnemyUnit);
    u.has_locomotor = false;
    CHECK(classify_unit(u).friendly == kAppFriendlyBuildPad);
    u.has_build_objects = false;
    CHECK(classify_unit(u).enemy == kAppEnemyCashPoint);
    u.has_cash_point = false;
    CHECK(classify_unit(u).friendly == kAppFriendlyReinforcementPoint);
    u.reinforcement_enabling_radius = 0.0f;
    CHECK(classify_unit(u).enemy == kAppEnemyStructure);
    // friendly needs an owner AND alliance; unowned counts as enemy
    u.has_locomotor = true;
    u.has_owner = false;
    u.owner_allied = true;
    CHECK(!is_friendly(u) && unit_applies(kAppEnemyUnit, u, false) && !unit_applies(kAppFriendlyUnit, u, false));
    u.has_owner = true;
    CHECK(is_friendly(u) && unit_applies(kAppFriendlyUnit, u, false) && !unit_applies(kAppEnemyUnit, u, false));
    u.owner_allied = false;
    CHECK(unit_applies(kAppEnemyUnit | kAppEnemyStructure, u, false));
    CHECK(!unit_applies(kAppGlobal | kAppTacticalLocation | kAppEnemyStructure, u, false));
    // fog
    CHECK(!unit_applies(kAppEnemyUnit, u, true));
    u.last_state_visible_under_fow = true;
    CHECK(unit_applies(kAppEnemyUnit, u, true));
    u.last_state_visible_under_fow = false;
    u.initial_state_visible_under_fow = true;
    CHECK(unit_applies(kAppEnemyUnit, u, true));
    int fog_calls = 0;
    const FogQuery fog = [&](int32_t id) {
        ++fog_calls;
        return id == 1;
    };
    CHECK(unit_applies(kAppEnemyUnit, u, fog) && fog_calls == 0);  // visible-under-FOW type: no query
    u.initial_state_visible_under_fow = false;
    CHECK(!unit_applies(kAppEnemyUnit, u, fog) && fog_calls == 1);
    u.object_id = 2;
    CHECK(unit_applies(kAppEnemyUnit, u, fog) && fog_calls == 2);
    // regions
    CHECK(region_applies(kAppTacticalLocation) && region_applies(kAppTacticalLocation | kAppEnemyUnit));
    CHECK(!region_applies(kAppGlobal) && !region_applies(kAppEnemyUnit | kAppFriendlyStructure));
    // parsers
    uint32_t bits = 0;
    CHECK(parse_goal_application("Enemy_Unit | enemy_structure", bits) && bits == (kAppEnemyUnit | kAppEnemyStructure));
    CHECK(parse_goal_application(" Tactical_Location ", bits) && bits == kAppTacticalLocation);
    CHECK(!parse_goal_application("Enemy_Unit | Bogus", bits));
    CHECK(!parse_goal_application("", bits));
    Reachability r = Reachability::Any;
    CHECK(parse_reachability("any_threat", r) && r == Reachability::AnyThreat);
    CHECK(parse_reachability("Enemy_Undefended", r) && r == Reachability::EnemyUndefended);
    CHECK(parse_reachability("Friendly_only", r) && r == Reachability::FriendlyOnly);
    CHECK(!parse_reachability("Sometimes", r));
}

// ---------------------------------------------------------------- 3.4 reachability
static void test_reachability() {
    const std::vector<HintZone> zones = {zone(10, "A", 0, 0, {20, 99}), zone(20, "B", 100, 0, {10, 30, 30}),
                                         zone(30, "C", 200, 0, {20}), zone(40, "D", 0, 500, {})};
    ZoneGraph g;
    g.build(zones);
    CHECK(g.size() == 4 && g.unresolved_links() == 1 && g.edge_count() == 4);
    CHECK(g.index_of_id(30) == 2 && g.index_of_name("c") == 2 && g.index_of_id(99) == -1);
    // TACTICAL_GAPS §6.1: hop = 3-D distance / 10, node cost 0 -> 0x10000 + hop.
    CHECK(g.weight(0, 1) == 0x10000u + 10u && g.weight(0, 2) == 2u * (0x10000u + 10u) &&
          g.weight(0, 3) == ::alamo::galactic::kUnlinked);
    std::vector<uint32_t> p;
    CHECK(g.path(0, 2, p) && p == (std::vector<uint32_t>{0, 1, 2}));
    CHECK(!g.path(0, 3, p));
    CHECK(g.nearest_zone(140.0f, 10.0f) == 1 && g.nearest_zone(150.0f, 0.0f) == 1);  // tie -> lower index
    const TacticalAiConstants c;
    const ZoneThreatFn threat = [](uint32_t z) { return z == 1 ? 500.0 : (z == 2 ? 2000.0 : 0.0); };
    const double w02 = 2.0 * (0x10000 + 10), w01 = 0x10000 + 10;
    CHECK(zone_reachability_cost(g, 0, 2, Reachability::AnyThreat, c, threat) == w02);
    CHECK(zone_reachability_cost(g, 0, 2, Reachability::HighThreat, c, threat) == w02);
    CHECK(zone_reachability_cost(g, 0, 2, Reachability::MediumThreat, c, threat) == w02);
    CHECK(zone_reachability_cost(g, 0, 2, Reachability::LowThreat, c, threat) == kUnreachable);  // C 2000 > 1000
    CHECK(zone_reachability_cost(g, 0, 1, Reachability::LowThreat, c, threat) == w01);
    CHECK(zone_reachability_cost(g, 0, 1, Reachability::NoThreat, c, threat) == kUnreachable);
    CHECK(zone_reachability_cost(g, 0, 0, Reachability::NoThreat, c, threat) == 0.0);
    CHECK(zone_reachability_cost(g, 1, 1, Reachability::NoThreat, c, threat) == kUnreachable);  // own zone threat
    CHECK(zone_reachability_cost(g, 0, 3, Reachability::AnyThreat, c, threat) == kUnreachable);
    CHECK(zone_reachability_cost(g, 0, 1, Reachability::Any, c, threat) == kUnreachable);       // levels < 6
    CHECK(zone_reachability_cost(g, 0, 1, Reachability::FriendlyOnly, c) == kUnreachable);
    CHECK(zone_reachability_cost(g, -1, 1, Reachability::AnyThreat, c) == kUnreachable);
    CHECK(zone_reachability_cost(g, 0, 7, Reachability::AnyThreat, c) == kUnreachable);
    CHECK(reachability_tolerance(Reachability::MediumThreat, c) == 3000.0);
    // Node_Weight: node cost = (uint32)(int64)max(0, threat) / 100; edge into j = n(j) x 65536 + hop.
    CHECK(land_zone_node_cost(-5.0) == 0u && land_zone_node_cost(99.9) == 0u && land_zone_node_cost(2050.7) == 20u);
    g.recompute_with_threat(threat);  // zone 1: 500 -> 5, zone 2: 2000 -> 20
    CHECK(g.node_costs()[1] == 5u && g.node_costs()[2] == 20u && g.node_costs()[0] == 0u);
    CHECK(g.weight(0, 1) == 5u * 65536u + 10u && g.weight(1, 2) == 20u * 65536u + 10u && g.weight(1, 0) == 0x10000u + 10u);
    {
        HintZone hz = zone(1, "H", 0, 0, {});
        HintZone hz2 = zone(2, "H2", 30, 40, {});
        hz2.center[2] = 120.0f;  // 3-D distance 130 -> hop 13
        CHECK(land_zone_hop(hz, hz2) == 13u && land_zone_edge_weight(hz, hz2, 0) == 0x10000u + 13u);
    }
    // Edge direction and flags: i -> j needs zone j's link to i with non-zero flags (0x5a9af0) [M direction].
    {
        std::vector<HintZone> d = {zone(1, "P", 0, 0, {2}), zone(2, "Q", 100, 0, {}), zone(3, "R", 200, 0, {2})};
        d[2].links[0].kind = 0;  // R -> Q link with zero flags: no edge Q -> R
        ZoneGraph dg;
        dg.build(d);
        CHECK(dg.weight(1, 0) == 0x10000u + 10u);                   // P links Q -> edge Q -> P
        CHECK(dg.weight(0, 1) == ::alamo::galactic::kUnlinked);     // Q has no link to P
        CHECK(dg.weight(1, 2) == ::alamo::galactic::kUnlinked && dg.edge_count() == 1);
    }
    // custom edge weights (drop B -> C)
    g.recompute([](uint32_t from, uint32_t to, uint32_t) { return (from == 1 && to == 2) ? ::alamo::galactic::kUnlinked : 7u; });
    CHECK(g.weight(0, 1) == 7u && !g.path(0, 2, p) && g.weight(2, 0) == 14u);
    // serial vs threaded all-pairs on a larger random zone graph
    SimRng r(1234);
    std::vector<HintZone> big;
    for (uint32_t i = 0; i < 300; ++i) {
        std::vector<uint32_t> links;
        const int32_t nl = r.range_int(0, 4);
        for (int32_t k = 0; k < nl; ++k) links.push_back(static_cast<uint32_t>(r.range_int(0, 299)) + 1000u);
        big.push_back(zone(1000u + i, "Z", r.range_float(-8000, 8000), r.range_float(-8000, 8000), links));
    }
    ZoneGraph a, b;
    a.build(big);
    b.build(big, {}, make_thread_pf(8));
    CHECK(a.matrices() == b.matrices());
}

// ---------------------------------------------------------------- 6.1 / 6.2 selection
static std::vector<ScoredTarget> scored(std::initializer_list<double> s) {
    std::vector<ScoredTarget> v;
    uint32_t i = 0;
    for (double x : s) v.push_back({i++, x});
    return v;
}

static void test_select_target() {
    CHECK(valid_pick_probability(1.0f) && valid_pick_probability(0.5f) && !valid_pick_probability(0.0f) &&
          !valid_pick_probability(1.5f));
    CHECK(pick_weight(1.0f) == 0.0f && pick_weight(0.5f) == 1.0f);
    SimRng rng(77);
    // no candidates / non-positive scores -> nil, no draw
    TargetPick p = select_target({}, 1.0f, rng);
    CHECK(p.target == -1 && !p.drew && rng.draws() == 0);
    p = select_target(scored({0.0, -2.0}), 1.0f, rng);
    CHECK(p.target == -1 && !p.drew && rng.draws() == 0);
    // one positive candidate -> best, no draw
    p = select_target(scored({0.0, 4.0, -1.0}), 1.0f, rng);
    CHECK(p.target == 1 && !p.drew && rng.draws() == 0);
    // w == 0 (p = 1) -> best, no draw; ties keep the earlier
    p = select_target(scored({2.0, 2.0, 1.0}), 0.0f, rng);
    CHECK(p.target == 0 && !p.drew && rng.draws() == 0);
    // [3, 5, 1], p = 0.5: list t0 (3), t2 (cum 4), best t1 (+4 -> 8): one draw Get_Float(0, 8)
    for (int k = 0; k < 50; ++k) {
        SimRng copy = rng;
        const float rr = copy.range_float(0.0f, 8.0f);
        const int32_t expect = rr <= 3.0f ? 0 : (rr <= 4.0f ? 2 : 1);
        const uint64_t before = rng.draws();
        p = select_target(scored({3.0, 5.0, 1.0}), pick_weight(0.5f), rng);
        CHECK(p.drew && rng.draws() == before + 1 && p.target == expect && rng == copy);
    }
    // P(best) = p
    for (float prob : {0.25f, 0.5f, 0.9f}) {
        SimRng s(99);
        int best = 0;
        const int n = 20000;
        for (int k = 0; k < n; ++k) best += select_target(scored({1.0, 6.0, 2.0, 3.0}), pick_weight(prob), s).target == 1;
        const double f = static_cast<double>(best) / n;
        CHECK(std::fabs(f - prob) < 0.02);
        CHECK(s.draws() == static_cast<uint64_t>(n));
    }
    // return kinds
    TacticalTarget unit, region;
    region.kind = TargetKind::Region;
    CHECK(find_target_return_kind(&unit) == FindTargetReturn::GameObject);
    CHECK(find_target_return_kind(&region) == FindTargetReturn::TargetLocation);
    CHECK(reachable_target_return_kind(&unit) == FindTargetReturn::TargetLocation);
    CHECK(find_target_return_kind(nullptr) == FindTargetReturn::Nil && reachable_target_return_kind(nullptr) == FindTargetReturn::Nil);
}

static void test_collect_candidates() {
    TacticalTargetList t(TacticalMode::Land);
    const TargetObjectInfo objs[] = {obj(1, true, false, false), obj(2, true, false, false), obj(3, true, false, false)};
    t.add_existing_objects(objs);
    std::vector<HintZone> zs = {zone(1, "Near", 100, 0, {}), zone(2, "Far", 5000, 0, {})};
    t.add_land_regions(zs);
    // units at x = 0, 300, 900
    CandidateFilter f;
    f.applies = [](const TacticalTarget& x) { return x.object_id != 2; };
    int evals = 0;
    f.score = [&](const TacticalTarget& x) -> std::optional<double> {
        ++evals;
        if (x.object_id == 3) return std::nullopt;  // failed evaluation
        return x.is_unit() ? 1.0 : 2.0;
    };
    std::vector<ScoredTarget> out;
    collect_candidates(t.targets(), f, out);
    CHECK(out.size() == 3 && out[0].target == 0 && out[1].target == 3 && out[2].target == 4 && evals == 4);
    // range (squared 3-D) with live unit positions
    f.position = [](const TacticalTarget& x) {
        if (!x.is_unit()) return x.position;
        return Vec3f{x.object_id == 1 ? 0.0f : (x.object_id == 2 ? 300.0f : 900.0f), 0.0f, 0.0f};
    };
    f.max_range = 500.0f;
    f.origin = {0.0f, 0.0f, 400.0f};
    collect_candidates(t.targets(), f, out);
    // unit 1 at d = 400 and "Near" at d = sqrt(100^2 + 400^2) = 412 are in range, "Far" is not
    CHECK(out.size() == 2 && out[0].target == 0 && out[1].target == 3);
    f.max_range = 0.0f;
    // reachability: only regions are filtered
    int reach_calls = 0;
    f.reachability = [&](const TacticalTarget& x) {
        ++reach_calls;
        return x.name == "Far" ? kUnreachable : 10.0;
    };
    collect_candidates(t.targets(), f, out);
    CHECK(out.size() == 2 && out[0].target == 0 && out[1].target == 3 && reach_calls == 2);
}

// ---------------------------------------------------------------- 6.3 damage tracking
static void test_damage_tracking() {
    DamageTrackingStore s;
    s.enable(10, 100.0f, 50.0f);
    s.enable(11, 40.0f, 0.0f);
    s.record(10, 7, 20.0f);
    s.record(10, 3, 20.0f);
    s.record(12, 7, 99.0f);  // untracked victim: ignored
    CHECK(s.tracks(10) && !s.tracks(12) && s.entries(12) == nullptr);
    const int32_t v10[] = {10};
    CHECK(find_deadly_enemy(s, v10) == 3);  // tie -> lowest id
    s.record(10, 7, 0.5f);
    CHECK(find_deadly_enemy(s, v10) == 7);
    CHECK(find_deadly_enemy(s, v10, [](int32_t a) { return a != 7; }) == kNoAttacker);  // attacker gone -> nil
    s.service(10, 0.05f);  // -7.5 each
    CHECK(s.entries(10)->at(7) == 13.0f && s.entries(10)->at(3) == 12.5f);
    s.service(10, 0.05f);
    s.service(10, 0.05f);  // 3: -2.5 -> removed; 7: -2.0 -> removed
    CHECK(s.entries(10)->empty() && find_deadly_enemy(s, v10) == kNoAttacker);
    // teams: members recursively, summed by attacker
    s.record(10, 7, 10.0f);
    s.record(10, 3, 15.0f);
    s.record(11, 7, 30.0f);
    const TeamMembersFn members = [](int32_t o, std::vector<int32_t>& m) {
        if (o == 100) m = {200, 11};
        if (o == 200) m = {10};
    };
    std::vector<int32_t> vic;
    collect_tracked(s, 100, members, vic);
    CHECK(vic == (std::vector<int32_t>{10, 11}));
    CHECK(find_deadly_enemy(s, vic) == 7);
    vic.clear();
    collect_tracked(s, 999, members, vic);
    CHECK(vic.empty());
    s.service_all(0.05f);  // 10: -7.5, 11: -2.0
    CHECK(s.entries(10)->at(7) == 2.5f && s.entries(11)->at(7) == 28.0f);
    s.disable(10);
    CHECK(!s.tracks(10) && s.tracked_count() == 1);
#if defined(ALAMO_AI_TACTICAL_HAVE_COMBAT)
    namespace cb = ::alamo::tactical::combat;
    cb::DamageRequest rq;
    rq.victim = 11;
    rq.source = 5;
    cb::DamageResult rs;
    rs.applied = true;
    rs.after_multipliers = 4.0f;
    record_combat_damage(s, rq, rs);
    CHECK(s.entries(11)->at(5) == 4.0f);
    rs.applied = false;
    record_combat_damage(s, rq, rs);
    rq.source = cb::kNoObject;
    rs.applied = true;
    record_combat_damage(s, rq, rs);
    CHECK(s.entries(11)->size() == 2 && s.entries(11)->at(5) == 4.0f);
#endif
}

// ---------------------------------------------------------------- 5 free store
static void test_free_store() {
    FreeStoreObjectInfo o;
    o.owned_by_player = true;
    o.has_locomotor = true;
    o.is_team = true;
    CHECK(land_free_store_admits(o) && space_free_store_admits(o));
    auto refuse = [&](auto mutate) {
        FreeStoreObjectInfo x = o;
        mutate(x);
        return !land_free_store_admits(x);
    };
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.owned_by_player = false; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.is_transport = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.type_create_team = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.type_is_dummy = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.type_stay_in_transport = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.has_indigenous_or_explosion = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.has_locomotor = false; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.is_team = false; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.type_category_bit_5f = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.team_has_bit_20 = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.type_bit_5b = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.subobject_f8_nonempty = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.in_mode_manager_set = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.dead_or_removing = true; }));
    CHECK(refuse([](FreeStoreObjectInfo& x) { x.in_world = false; }));
    {
        FreeStoreObjectInfo x = o;
        x.is_team = false;
        x.is_starship_combatant = true;  // starship combatant with a locomotor
        CHECK(land_free_store_admits(x));
        x.is_starship_combatant = false;
        x.has_locomotor = false;
        x.has_lobbing_superweapon = true;  // lobbing superweapon without locomotor
        CHECK(land_free_store_admits(x));
        x = o;
        x.type_stay_in_transport = true;  // land-only test
        CHECK(!land_free_store_admits(x) && space_free_store_admits(x));
    }
    TacticalFreeStore fs;
    CHECK(fs.admit(30, 2) && fs.admit(10, 2) && fs.admit(20, 5) && !fs.admit(10, 2));
    CHECK(fs.size() == 3 && fs.type_count(2) == 2 && fs.type_count(5) == 1 && fs.type_count(9) == 0);
    std::vector<int32_t> u;
    fs.units(u, true);
    CHECK(u == (std::vector<int32_t>{10, 20, 30}));  // ascending id [port]
    CHECK(fs.set_order_lock(20, true) && fs.order_locked(20) && !fs.set_order_lock(99, true));
    fs.units(u, true);
    CHECK(u == (std::vector<int32_t>{10, 30}));
    CHECK(fs.remove(10) && !fs.remove(10) && fs.type_count(2) == 1);

    // cadence: BusyTacticalFreeStore, Execution services every 3 frames from frame 0
    FreeStoreSchedule sched = busy_tactical_schedule();
    std::vector<uint32_t> svc, unit_svc;
    FreeStoreServiceStep step;
    for (uint32_t f = 0; f <= 1300; f += 3) {
        free_store_service(sched, fs, f, step);
        if (step.run_service) svc.push_back(f);
        if (step.run_unit_service) {
            unit_svc.push_back(f);
            CHECK(step.units == (std::vector<int32_t>{30}));
        }
    }
    // Literal float formula: now = (float)F * (1/30) -> 600 * 0.033333335f = 20.0000019 > 20, so the
    // first FreeStoreService is at F = 600 (not 603), then every 600 frames.
    // ... while 60 * 0.033333335f rounds to exactly 2.0f, so the first On_Unit_Service pass is at F = 63.
    CHECK(free_store_mode_time(600) > 20.0f && free_store_mode_time(60) == 2.0f);
    CHECK(svc == (std::vector<uint32_t>{600, 1200}));
    CHECK(unit_svc.size() > 3 && unit_svc[0] == 63);
    std::printf("free store: FreeStoreService at F =");
    for (uint32_t f : svc) std::printf(" %u", f);
    std::printf("; On_Unit_Service at F =");
    for (size_t i = 0; i < unit_svc.size() && i < 8; ++i) std::printf(" %u", unit_svc[i]);
    std::printf(" ...\n");
    // absent rates -> nothing runs
    FreeStoreSchedule none;
    free_store_service(none, fs, 100000, step);
    CHECK(!step.run_service && !step.run_unit_service && step.units.empty());
}

// ---------------------------------------------------------------- 1.3 credits
static void test_credits() {
    std::vector<TacticalBudgetCategory> b = {{0.5f, 1000.0f, 1000.0f}, {0.0f, 500.0f, 500.0f}, {2.0f, 200.0f, 300.0f},
                                             {0.5f, -10.0f, 0.0f}};
    econ::CreditAccount player{5000.0f, -1.0f};
    TacticalCreditTransfer st;
    // skirmish (no galactic parent) and human: no transfer
    CHECK(begin_land_battle_credits(b, player, false, false, st) == 0.0f && !st.active && player.credits == 5000.0f);
    CHECK(begin_land_battle_credits(b, player, true, true, st) == 0.0f && !st.active);
    CHECK(end_land_battle_credits(player, st, 1.2f) == 0.0f && player.credits == 5000.0f);
    CHECK(begin_land_battle_credits(b, player, true, false, st) == 700.0f);
    CHECK(st.active && st.saved == 4300.0f && player.credits == 700.0f);
    CHECK(b[0].available == 500.0f && b[0].pool == 500.0f && b[1].available == 500.0f && b[2].available == 0.0f &&
          b[2].pool == 100.0f && b[3].available == -10.0f);
    player.credits = 300.0f;  // spent 400 in battle
    econ::CreditAccount expect{4300.0f, -1.0f};
    const float applied_expect = econ::add_credits(expect, 300.0f, true, 1.2f);
    CHECK(end_land_battle_credits(player, st, 1.2f) == applied_expect);
    CHECK(player.credits == expect.credits && !st.active);
    // released credits are not returned; a negative return goes through Add_Credits unmultiplied
    econ::CreditAccount p2{100.0f, -1.0f};
    std::vector<TacticalBudgetCategory> b2 = {{1.0f, 50.0f, 50.0f}};
    CHECK(begin_land_battle_credits(b2, p2, true, false, st) == 50.0f && st.saved == 50.0f);
    st.released = 80.0f;
    p2.credits = 20.0f;
    econ::CreditAccount e2{50.0f, -1.0f};
    econ::add_credits(e2, -60.0f, true, 1.2f);
    end_land_battle_credits(p2, st, 1.2f);
    CHECK(p2.credits == e2.credits);
}

// ---------------------------------------------------------------- scenario CRC (determinism / golden)
static uint64_t scenario_crc(const ParallelFor& pf) {
    SimRng rng(0xA1u);
    TacticalAiConstants c;
    TacticalTargetList land(TacticalMode::Land), space(TacticalMode::Space);
    std::vector<HintZone> zones;
    for (uint32_t i = 0; i < 40; ++i) {
        std::vector<uint32_t> links;
        for (int k = 0; k < 3; ++k) links.push_back(static_cast<uint32_t>(rng.range_int(0, 39)));
        zones.push_back(zone(i, ("Zone" + std::to_string(i)).c_str(), rng.range_float(0, 6000), rng.range_float(0, 6000), links));
    }
    std::vector<TargetObjectInfo> objs;
    for (int32_t id = 0; id < 60; ++id) objs.push_back(obj(id, rng.range_int(0, 3) != 0, rng.range_int(0, 1) != 0, rng.range_int(0, 1) != 0));
    land.add_existing_objects(objs);
    land.add_land_regions(zones);
    space.add_existing_objects(objs);
    space.build_space_grid({-7000, -7000, 7000, 7000}, c.space_region_size, true);
    ZoneGraph g;
    g.build(zones, {}, pf);
    DamageTrackingStore dmg;
    TacticalFreeStore fs;
    FreeStoreSchedule sched = busy_tactical_schedule();
    std::vector<int32_t> picks;
    int32_t next_id = 60;
    FreeStoreServiceStep step;
    for (uint32_t frame = 0; frame < 1800; frame += 3) {
        // churn
        if (rng.range_int(0, 3) == 0) land.on_object_added(obj(next_id, true, true, true));
        if (rng.range_int(0, 3) == 0) space.on_object_added(obj(next_id, true, true, true));
        if (rng.range_int(0, 1) == 0) {
            dmg.enable(next_id, rng.range_float(50, 500), rng.range_float(0, 200));
            fs.admit(next_id, static_cast<uint32_t>(rng.range_int(0, 5)));
        }
        ++next_id;
        if (rng.range_int(0, 4) == 0) {
            const int32_t dead = rng.range_int(0, next_id - 1);
            land.on_object_destroyed(dead);
            space.on_object_destroyed(dead);
            dmg.disable(dead);
            fs.remove(dead);
        }
        for (int k = 0; k < 4; ++k) dmg.record(rng.range_int(0, next_id - 1), rng.range_int(0, 50), rng.range_float(1, 60));
        dmg.service_all(c.space_threat_decay_step);
        if (rng.range_int(0, 9) == 0) fs.set_order_lock(rng.range_int(0, next_id - 1), rng.range_int(0, 1) != 0);
        free_store_service(sched, fs, frame, step);
        // FindTarget-like pick over the land list with a pseudo score, and a Reachable_Target-like
        // pick over regions with the zone reachability filter
        CandidateFilter f;
        const uint32_t salt = frame;
        f.score = [salt](const TacticalTarget& t) -> std::optional<double> {
            const uint32_t h = (static_cast<uint32_t>(t.object_id) * 2654435761u) ^ (static_cast<uint32_t>(t.zone) * 40503u) ^ salt;
            return static_cast<double>(h % 1000u) - 200.0;
        };
        std::vector<ScoredTarget> cand;
        collect_candidates(land.targets(), f, cand);
        picks.push_back(select_target(cand, pick_weight(0.5f), rng).target);
        f.applies = [](const TacticalTarget& t) { return !t.is_unit(); };
        const int32_t src = static_cast<int32_t>(frame % 40u);
        f.reachability = [&](const TacticalTarget& t) {
            return zone_reachability_cost(g, src, t.zone, Reachability::MediumThreat, c,
                                          [salt](uint32_t z) { return static_cast<double>((z * 977u + salt) % 4000u); });
        };
        collect_candidates(land.targets(), f, cand);
        picks.push_back(select_target(cand, pick_weight(0.75f), rng).target);
        const int32_t victims[] = {rng.range_int(0, next_id - 1), rng.range_int(0, next_id - 1)};
        picks.push_back(find_deadly_enemy(dmg, victims));
    }
    StateHasher h;
    rng.hash(h);
    land.hash(h);
    space.hash(h);
    g.hash(h);
    dmg.hash(h);
    fs.hash(h);
    sched.hash(h);
    h.u32(static_cast<uint32_t>(picks.size()));
    for (int32_t p : picks) h.i32(p);
    return h.digest();
}

static void test_scenario(const char* golden, const char* write_golden) {
    const uint64_t a = scenario_crc(serial_for);
    const uint64_t b = scenario_crc(make_thread_pf(4));
    const uint64_t c = scenario_crc(make_thread_pf(std::max(2u, std::thread::hardware_concurrency())));
    CHECK(a == b && a == c);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016" PRIx64, a);
    std::printf("scenario crc %s\n", buf);
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

#if defined(ALAMO_AI_TACTICAL_HAVE_MOVEMENT)
static void test_land_connectivity() {
    namespace mv = ::alamo::tactical::movement;
    mv::PassabilityGrid grid;
    grid.init(8, 4);
    // wall at column 4
    for (int32_t y = 0; y < 4; ++y) grid.set_base(4, y, mv::kPassObstacle);
    mv::RegionMap rm;
    const uint32_t masks[] = {mv::kPassClear};
    rm.build(grid, masks);
    CHECK(land_points_connected(rm, mv::kPassClear, 0.0f, 0.0f, 60.0f, 40.0f));
    CHECK(!land_points_connected(rm, mv::kPassClear, 0.0f, 0.0f, 120.0f, 0.0f));
    CHECK(!land_points_connected(rm, 0x12345u, 0.0f, 0.0f, 60.0f, 40.0f));
}
#endif

#ifdef AI_TACTICAL_REAL_DATA
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
    CHECK(fs != nullptr);
    if (!fs) return;
    TacticalAiConstants c;
    {
        data::GameDatabase db;
        if (db.load(*fs)) c.load(db.constants());
        std::printf("real data: AI_SpaceEvaluatorRegionSize %.1f, land %.1f, tolerances %.0f/%.0f/%.0f, decay %.3f\n",
                    static_cast<double>(c.space_region_size), static_cast<double>(c.land_region_size),
                    static_cast<double>(c.low_threat_tolerance), static_cast<double>(c.medium_threat_tolerance),
                    static_cast<double>(c.high_threat_tolerance), static_cast<double>(c.space_threat_decay_step));
        CHECK(c.space_region_size > 0.0f && c.low_threat_tolerance <= c.medium_threat_tolerance &&
              c.medium_threat_tolerance <= c.high_threat_tolerance);
    }
    const auto maps = fs->list("Data\\Art\\Maps\\*.ted");
    uint32_t land_maps = 0, land_zoned = 0, zones_total = 0, links_total = 0, unresolved = 0, empty_names = 0, dup_names = 0;
    uint32_t space_maps = 0, cells_total = 0, parse_fail = 0, zero_flag_links = 0;
    uint64_t pairs = 0, reachable = 0;
    ParallelFor pf = make_thread_pf(4);
    for (const std::string& path : maps) {
        std::vector<uint8_t> bytes;
        terrain::MapData map;
        terrain::ParseOptions opt;
        opt.objects = false;
        if (!fs->read(path, bytes) || !terrain::parse_ted(bytes, map, nullptr, opt)) {
            ++parse_fail;
            continue;
        }
        Rect2 bounds;
        const bool have_bounds = map_bounds(map, bounds);
        if (!map.is_space()) {
            ++land_maps;
            const std::vector<HintZone> zones = hint_zones_from_map(map);
            TacticalTargetList t(TacticalMode::Land);
            CHECK(t.add_land_regions(zones) == zones.size());
            if (zones.empty()) {
                CHECK(t.size() == 0);  // no Tactical_Location targets
                continue;
            }
            ++land_zoned;
            zones_total += static_cast<uint32_t>(zones.size());
            std::set<std::string> names;
            for (size_t i = 0; i < zones.size(); ++i) {
                if (zones[i].name.empty()) ++empty_names;
                if (!names.insert(zones[i].name).second) ++dup_names;
                links_total += static_cast<uint32_t>(zones[i].links.size());
                for (const HintZone::Link& l : zones[i].links) zero_flag_links += l.kind == 0 ? 1u : 0u;
                CHECK(t[i].name == zones[i].name && t[i].zone == static_cast<int32_t>(i));
                if (have_bounds) {  // zone centres lie on the map (with a cell of slack)
                    CHECK(zones[i].center[0] >= bounds.min_x - 2000.0f && zones[i].center[0] <= bounds.max_x + 2000.0f);
                }
            }
            ZoneGraph g, gp;
            g.build(zones);
            gp.build(zones, {}, pf);
            CHECK(g.matrices() == gp.matrices());
            unresolved += g.unresolved_links();
            for (uint32_t i = 0; i < g.size(); ++i)
                for (uint32_t j = 0; j < g.size(); ++j) {
                    ++pairs;
                    if (zone_reachability_cost(g, static_cast<int32_t>(i), static_cast<int32_t>(j), Reachability::AnyThreat, c) !=
                        kUnreachable)
                        ++reachable;
                }
        } else {
            ++space_maps;
            CHECK(have_bounds);
            if (!have_bounds) continue;
            TacticalTargetList t(TacticalMode::Space);
            const uint32_t n = t.build_space_grid(bounds, c.space_region_size, true);
            const auto nx = static_cast<uint32_t>(std::ceil((bounds.max_x - bounds.min_x) / c.space_region_size));
            const auto ny = static_cast<uint32_t>(std::ceil((bounds.max_y - bounds.min_y) / c.space_region_size));
            CHECK(n == nx * ny && n > 0);
            cells_total += n;
            std::set<std::string> names;
            for (const TacticalTarget& x : t.targets()) {
                CHECK(names.insert(x.name).second);
                CHECK(x.eval_rect.max_x <= bounds.max_x && x.eval_rect.max_y <= bounds.max_y &&
                      x.eval_rect.min_x < x.eval_rect.max_x && x.eval_rect.min_y < x.eval_rect.max_y);
            }
        }
    }
    std::printf("real data: %zu maps (%u parse failures): %u land (%u with hint zones, %u zones, %u links, %u unresolved, "
                "%u empty / %u duplicate names; %" PRIu64 "/%" PRIu64 " zone pairs reachable), %u space (%u grid cells)\n",
                maps.size(), parse_fail, land_maps, land_zoned, zones_total, links_total, unresolved, empty_names, dup_names,
                reachable, pairs, space_maps, cells_total);
    std::printf("real data: %u links with zero flags (no edge, TACTICAL_GAPS §6.1)\n", zero_flag_links);
    CHECK(land_maps > 0 && space_maps > 0 && land_zoned > 0);
}
#endif

// ---------------------------------------------------------------- N1 save / load of the components
// The land path finder (node costs saved, matrices rebuilt and checked by digest), target lists, the free store and
// its schedule: round trip, write -> read -> write, refused bad data leaves the object unchanged.
template <class T>
static uint64_t digest_of(const T& x) {
    StateHasher h;
    x.hash(h);
    return h.digest();
}

static void test_component_saves() {
    std::vector<HintZone> zs;
    for (uint32_t i = 0; i < 12; ++i) {
        std::vector<uint32_t> links;
        if (i > 0) links.push_back(i - 1);
        if (i + 1 < 12) links.push_back(i + 1);
        if (i % 3 == 0 && i + 3 < 12) links.push_back(i + 3);
        zs.push_back(zone(i, "Z", 700.0f * static_cast<float>(i), 300.0f * static_cast<float>(i % 4), links));
    }
    ZoneGraph built;
    built.build(zs);
    ZoneGraph g = built;
    g.recompute_with_threat([](uint32_t z) { return 250.0 * static_cast<double>(z); });
    CHECK(!(g.matrices() == built.matrices()));
    ByteWriter w;
    g.write(w);
    ZoneGraph h = built;
    {
        ByteReader r(w.bytes());
        CHECK(h.read(r) && r.done());
    }
    CHECK(h.matrices() == g.matrices() && digest_of(h) == digest_of(g));
    std::vector<uint32_t> pa, pb;
    CHECK(h.path(0, 11, pa) && g.path(0, 11, pb) && pa == pb);
    ByteWriter again;
    h.write(again);
    CHECK(again.bytes() == w.bytes());
    {
        ZoneGraph other;  // other zones: refused
        other.build(std::vector<HintZone>(zs.begin(), zs.begin() + 6));
        const uint64_t before = digest_of(other);
        ByteReader r(w.bytes());
        CHECK(!other.read(r) && digest_of(other) == before);
    }
    {
        std::vector<uint8_t> bad = w.bytes();
        bad[8 + 4 * 5 + 1] ^= 1;  // node cost of zone 5 (+256; zone 0 is the path start): the rebuilt matrices miss the saved digest
        ZoneGraph x = built;
        ByteReader r(bad);
        CHECK(!x.read(r) && x.matrices() == built.matrices());
    }

    TacticalTargetList land(TacticalMode::Land);
    std::vector<TargetObjectInfo> objs;
    for (int32_t id = 0; id < 20; ++id) objs.push_back(obj(id, id % 3 != 0, id % 2 == 0, id % 2 == 0));
    land.add_existing_objects(objs);
    land.add_land_regions(zs);
    land.on_object_added(obj(40, true, false, false));
    land.on_object_destroyed(4);
    ByteWriter lw;
    land.write(lw);
    TacticalTargetList land2(TacticalMode::Land);
    {
        ByteReader r(lw.bytes());
        CHECK(land2.read(r) && r.done());
    }
    CHECK(digest_of(land2) == digest_of(land) && land2.is_registered(40) && !land2.is_registered(4) && land2.find_region("z") >= 0);
    TacticalTargetList space(TacticalMode::Space);
    {
        ByteReader r(lw.bytes());  // another mode: refused
        CHECK(!space.read(r) && space.size() == 0);
    }
    bool trunc = true;
    for (size_t cut = 0; cut < lw.bytes().size(); ++cut) {
        ByteReader r(std::span<const uint8_t>(lw.bytes().data(), cut));
        trunc = trunc && !land2.read(r) && digest_of(land2) == digest_of(land);
    }
    CHECK(trunc);

    TacticalFreeStore fs;
    for (int32_t id : {9, 2, 14, 5}) fs.admit(id, static_cast<uint32_t>(id % 3));
    fs.set_order_lock(14, true);
    fs.remove(2);
    ByteWriter fw;
    fs.write(fw);
    TacticalFreeStore fs2;
    fs2.admit(77, 1);
    {
        ByteReader r(fw.bytes());
        CHECK(fs2.read(r) && r.done());
    }
    CHECK(digest_of(fs2) == digest_of(fs) && fs2.type_count(2) == fs.type_count(2) && fs2.order_locked(14) && !fs2.contains(77));
    {
        std::vector<uint8_t> bad = fw.bytes();
        bad[8] = 20;  // first id 5 -> 20 (> 9): not ascending
        const uint64_t before = digest_of(fs2);
        ByteReader r(bad);
        CHECK(!fs2.read(r) && digest_of(fs2) == before);
    }
    FreeStoreSchedule sched = busy_tactical_schedule();
    sched.last_service = 21.5;
    ByteWriter sw;
    sched.write(sw);
    FreeStoreSchedule sched2;
    {
        ByteReader r(sw.bytes());
        CHECK(sched2.read(r) && r.done());
    }
    CHECK(digest_of(sched2) == digest_of(sched) && sched2.unit_service_rate == sched.unit_service_rate);
    std::printf("save: zone graph %zu bytes, land target list %zu bytes, free store %zu bytes: round trips identical\n", w.bytes().size(),
                lw.bytes().size(), fw.bytes().size());
}

int main(int argc, char** argv) {
    const char* golden = nullptr;
    const char* write_golden = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--golden") == 0) golden = argv[++i];
        else if (std::strcmp(argv[i], "--write-golden") == 0) write_golden = argv[++i];
    }
    test_target_list_land();
    test_target_list_space();
    test_applies();
    test_reachability();
    test_select_target();
    test_collect_candidates();
    test_damage_tracking();
    test_free_store();
    test_credits();
    test_component_saves();  // N1 stage 2
#if defined(ALAMO_AI_TACTICAL_HAVE_MOVEMENT)
    test_land_connectivity();
#endif
    test_scenario(golden, write_golden);
#ifdef AI_TACTICAL_REAL_DATA
    test_real_data();
#endif
    if (g_fail) {
        std::printf("alamo_ai_tactical_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ai_tactical_tests: OK\n");
    return 0;
}
