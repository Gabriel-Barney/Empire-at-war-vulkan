// alamo_ai tests (no framework; non-zero exit on failure).
//   * equation language: numbers, precedence, comparisons, parameters, enums, '#', Function_/Script_,
//     recursion, errors;
//   * hand-computed Node_Weight / danger / passability cases (GALACTIC_ECONOMY.md section 6.2,
//     PHASE2_ANSWERS Q5-Q7) on a 4-planet snapshot;
//   * determinism: perception + all-pairs over a synthetic galaxy for 1 / 4 / N workers (and a
//     20000-planet snapshot that takes the parallel path), plus a golden CRC
//     (tests/golden/ai_golden.txt; regenerate with --write-golden);
//   * real data (ALAMO_FOC_DATA / ALAMO_EAW_DATA): every shipped perception equation file parsed,
//     coverage report to logs/ai_perception_coverage_<game>.txt, every campaign seeded and run.
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "alamo/ai/ai_path_finder.hpp"
#include "alamo/ai/galactic_perception.hpp"
#include "alamo/ai/galactic_seed.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"  // A2 tactical: '#' reseed CRC
#include "alamo/core/log.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/all_pairs.hpp"
#include "alamo/galactic/planet_graph.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#ifdef ALAMO_AI_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif
#ifdef ALAMO_AI_HAVE_JOBS
#include "alamo/jobs/job_system.hpp"
#endif

using namespace alamo;
using namespace alamo::ai;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

// ---- equation language --------------------------------------------------------------------------

// Resolver over a fixed name -> value table ("Variable_Target.A.B" keys); Self must be bound.
class MapResolver final : public TokenResolver {
public:
    std::map<std::string, double> values;
    bool resolve(const EvalContext& ctx, const TokenRef& t, double& out) const override {
        if (ctx.self < 0) return false;
        std::string key = root_name(t.root);
        for (TokenId id : t.path) {
            key += '.';
            key += t.tokens->name(id);
        }
        for (const Param& p : t.params) key += "{" + p.name + "}";
        const auto it = values.find(key);
        if (it == values.end()) return false;
        out = it->second;
        return true;
    }
    bool implements(Root, std::span<const TokenId>, const TokenTable&) const override { return true; }
};

static double eval_text(const char* text, const MapResolver& r, bool* ok = nullptr, SimRng* rng = nullptr) {
    EquationSet set;
    const uint32_t i = set.add("T", text);
    set.link();
    EvalContext ctx;
    ctx.self = 0;
    ctx.rng = rng;
    const EvalResult res = evaluate(set, i, ctx, r);
    if (ok) *ok = res.ok;
    return res.value;
}

static void test_language() {
    MapResolver r;
    r.values["Variable_Target.EnemyForce.SpaceTotal"] = 0.25;
    r.values["Variable_Self.IsFaction{Parameter_Faction}"] = 1.0;
    r.values["Game.IsCampaignGame"] = 1.0;
    bool ok = false;
    CHECK(eval_text("1.5", r, &ok) == 1.5 && ok);
    CHECK(eval_text(" 0.000000001 ", r) == 0.000000001);
    CHECK(eval_text("1 + 2 * 3", r) == 7.0);
    CHECK(eval_text("(1 + 2) * 3", r) == 9.0);
    CHECK(eval_text("10 - 4 - 3", r) == 3.0);  // left associative
    CHECK(eval_text("8 / 4 / 2", r) == 1.0);
    CHECK(eval_text("-2 * -3", r) == 6.0);
    CHECK(eval_text("2 > 1", r) == 1.0 && eval_text("1 > 2", r) == 0.0);
    CHECK(eval_text("2 >= 2", r) == 1.0 && eval_text("2 == 2.0", r) == 1.0 && eval_text("2 != 2", r) == 0.0);
    CHECK(eval_text("1 + 1 > 1", r) == 1.0);  // comparison binds loosest
    CHECK(precedence_sensitive("1 + 1 > 1") && !precedence_sensitive("(1 + 1) > 1") && !precedence_sensitive("2 * 3"));
    // S2 grammar (STRATEGIC_GAPS §3): '#' binds like '*' (left to right), comparisons chain left to right,
    // unary minus tightest, a lookup needs a dot, '+' only as a literal sign.
    CHECK(eval_text("2 * 3 # 6", r, &ok) == 6.0 && ok);  // (2*3) # 6 = 6 # 6
    CHECK(eval_text("6 # 6 * 2", r, &ok) == 12.0 && ok);  // (6 # 6) * 2
    CHECK(eval_text("12 / 2 # 6", r, &ok) == 6.0 && ok);
    CHECK(precedence_sensitive("2 * 3 # 6") && precedence_sensitive("1 # 2 * 3") && !precedence_sensitive("(2 * 3) # 6"));
    CHECK(eval_text("3 > 2 > 1", r) == 0.0 && eval_text("1 < 2 < 3", r) == 1.0);  // (3 > 2) > 1 = 1 > 1
    CHECK(eval_text("1 > 0 * 5", r) == 1.0);                                       // 1 > (0*5)
    CHECK(eval_text("- 3 * 2", r) == -6.0 && eval_text("+3", r) == 3.0 && eval_text("2 * +.5", r) == 1.0);
    eval_text("+ 3", r, &ok);
    CHECK(!ok);
    eval_text("+Game.IsCampaignGame", r, &ok);
    CHECK(!ok);
    eval_text("Game", r, &ok);
    CHECK(!ok);
    eval_text("1 + Variable_Self", r, &ok);
    CHECK(!ok);
    CHECK(eval_text("15.0 * Variable_Target.EnemyForce.SpaceTotal / 30.0", r) == 0.125);
    CHECK(eval_text("variable_target.enemyforce.spacetotal", r) == 0.25);  // case-insensitive
    CHECK(eval_text("Variable_Self.IsFaction {Parameter_Faction = \"Empire\"}", r) == 1.0);
    CHECK(eval_text("Game.IsCampaignGame", r) == 1.0);
    CHECK(eval_text("clamp(5 - 1, 0.0, 1.0)", r) == 1.0 && eval_text("Clamp(-3, 0, 1)", r) == 0.0 &&
          eval_text("2 * clamp(0.25, 0, 1)", r) == 0.5);
    // Unknown token, Script_, unresolved Function_ fail the whole evaluation.
    eval_text("1 + Variable_Target.Nope", r, &ok);
    CHECK(!ok);
    eval_text("0 * Script_AnyCurrentThreats.Evaluate", r, &ok);
    CHECK(!ok);
    eval_text("Function_Missing.Evaluate", r, &ok);
    CHECK(!ok);
    // --- A2 tactical: '#' uses its own LCG reseeded per evaluation (AI_GALACTIC_3 §7.3): 0 synced draws, the same
    // value for the same (seed, player, equation, target), lo/hi swapped. Seed here: 0 + (0 x 131 + 0x123456FB) +
    // CRC-32("T") + 0. ---
    {
        SimRng rng(7);
        const uint32_t s0 = 0x123456FBu + crc32("T", 1);
        const uint32_t s1 = s0 * 1103515245u + 3039u;
        const float expect = static_cast<float>((s1 >> 10) & 0x7FFFu) / 32767.0f * 20.0f + 20.0f;
        const double v = eval_text("(20 # 40)", r, &ok, &rng);
        CHECK(ok && v == static_cast<double>(expect) && rng.draws() == 0);
        CHECK(eval_text("(40 # 20)", r, &ok) == v && ok);  // no rng needed; bounds swapped; reseeded -> same value
        CHECK(eval_text("(0#1>2)", r, &ok, &rng) == 0.0 && ok && rng.draws() == 0);
    }
    // --- end A2 tactical ---
    // Parameters: strings, numbers, enum bitfields, repeated names.
    {
        TokenTable t;
        Expression x;
        ParseError err;
        CHECK(parse_expression("Variable_Target.HasStructure {Parameter_Type = \"A\", Parameter_Type = \"B\", "
                               "Parameter_Only_Consider_Complete = 1, Parameter_Category = "
                               "GameObjectCategoryType[Fighter | Bomber], Parameter_Level = -2.5}",
                               t, x, &err));
        CHECK(x.params.size() == 5 && x.params[0].value.text == "A" && x.params[1].value.text == "B");
        CHECK(x.params[2].value.kind == ParamValue::Kind::Number && x.params[2].value.number == 1.0);
        CHECK(x.params[3].value.kind == ParamValue::Kind::Enum && x.params[3].value.text == "GameObjectCategoryType" &&
              x.params[3].value.enum_values.size() == 2 && x.params[3].value.enum_values[1] == "Bomber");
        CHECK(x.params[4].value.number == -2.5);
        CHECK(!parse_expression("1 +", t, x, &err) && !err.message.empty());
        CHECK(!parse_expression("(1", t, x, &err));
        CHECK(!parse_expression("Foo.Bar", t, x, &err));
        CHECK(!parse_expression("Function_X.Run", t, x, &err));
        CHECK(!parse_expression("1 2", t, x, &err));
        CHECK(parse_expression("Variable_Target.EnemyForce.SpaceTotal.TimeLastSeenUnnormalized", t, x, &err) &&
              x.path.size() == 3);
    }
    // Registry ids are fixed and identical in every table.
    {
        TokenTable a, b;
        CHECK(TokenTable::registry_size() == 222);
        CHECK(a.find("EnemyForce") == registry_token("enemyforce") && b.find("EnemyForce") == a.find("EnemyForce"));
        CHECK(registry_token("NotAToken") == kNoToken);
        const TokenId u = a.intern("My_New_Token");
        CHECK(u >= TokenTable::registry_size() && !TokenTable::is_registered(u) && a.intern("MY_NEW_TOKEN") == u);
    }
    // Function_ references, duplicates (first wins), recursion.
    {
        const char* xml =
            "<?xml version=\"1.0\"?>\n<Equations>\n<!-- c -->\n<A> Function_B.Evaluate * 2 </A>\n<B>3.0</B>\n"
            "<B>99</B>\n<Loop> Function_Loop.Evaluate </Loop>\n<Bad> 1 + </Bad>\n</Equations>\n";
        EquationSet set;
        LoadReport rep;
        set.add_file(xml, "test.xml", rep);
        set.link(&rep);
        CHECK(rep.files == 1 && rep.equations == 4 && rep.duplicates == 1 && rep.parse_errors == 1);
        CHECK(rep.unresolved_functions == 0);
        EvalContext ctx;
        ctx.self = 0;
        EvalResult res = evaluate(set, set.find("a"), ctx, r);
        CHECK(res.ok && res.value == 6.0);
        res = evaluate(set, set.find("Loop"), ctx, r);
        CHECK(!res.ok && res.error == EvalError::Recursion);
        res = evaluate(set, set.find("Bad"), ctx, r);
        CHECK(!res.ok);
        CHECK(set.equation(set.find("B")).line == 5);
    }
}

// ---- hand-computed galactic cases ----------------------------------------------------------------

// Players: 0 Empire (AI), 1 Rebel (AI), 2 Pirates (AI, hostile to all), 3 Neutral (no AI).
// Empire and Pirates are NOT allied; nobody is allied.
static GalacticSnapshot hand_snapshot() {
    GalacticSnapshot s;
    s.planet_count = 4;
    s.players.resize(4);
    for (int p = 0; p < 3; ++p) s.players[static_cast<size_t>(p)].has_ai_system = true;
    s.players[3].neutral = true;
    s.players[2].benefits_from_corruption = true;
    s.relation.assign(16, 1);
    for (size_t p = 0; p < 4; ++p) s.relation[p * 4 + p] = 0;
    // planet 0: Empire, starbase L3 (5000), orbit: Rebel 300 + Rebel 200 (space), Rebel 150 (ground),
    //           Empire 999 (own).
    // planet 1: Rebel, starbase L2 (2000.5), orbit: Empire 100, Rebel 400; structures 120 + 80.
    // planet 2: Neutral, no starbase, orbit: unowned 50 (space).
    // planet 3: Empire but political control 0 (not "held"), starbase L1 (0.1f), no units.
    s.owner = {0, 1, 3, 0};
    s.political_control = {1.0f, 1.0f, 1.0f, 0.0f};
    s.starbase_level = {3, 2, 0, 1};
    s.starbase_power = {5000.0f, 2000.5f, 0.0f, 0.1f};
    s.corruption = {-1, -1, 2, -1};
    s.units = {
        {1, 300.0f, UnitLayer::Space}, {1, 200.0f, UnitLayer::Space}, {1, 150.0f, UnitLayer::Ground},
        {0, 999.0f, UnitLayer::Space},  // planet 0
        {0, 100.0f, UnitLayer::Space}, {1, 400.0f, UnitLayer::Space},  // planet 1
        {kNoPlayer, 50.0f, UnitLayer::Space},                          // planet 2
    };
    s.unit_begin = {0, 4, 6, 7, 7};
    s.structure_begin = {0, 0, 2, 2, 2};
    s.structure_power = {120.0f, 80.0f};
    // Rebel (1) never sees planet 0; everyone else sees everything.
    s.sees_orbit.assign(4, galactic::Bitset());
    s.sees_orbit[1] = galactic::Bitset(4, true);
    s.sees_orbit[1].set(0, false);
    return s;
}

static void test_hand_cases() {
    GalacticSnapshot s = hand_snapshot();
    CHECK(s.validate().empty());
    GalacticPerception gp;
    gp.reset(s);
    gp.update(s, 30);
    CHECK(gp.tracks(0) && gp.tracks(1) && gp.tracks(2) && !gp.tracks(3));

    // Empire (0):
    const PlanetPerception& e = gp.results(0);
    // p0 held by Empire: perception only = hostile space 300 + 200.
    CHECK(e.space_total[0] == 500.0 && e.ground_total[0] == 150.0 && e.node_value[0] == 500.0);
    // p1 Rebel: hostile space 400 (own 100 excluded) + starbase power 2000.5f.
    CHECK(e.space_total[1] == 400.0 && e.node_value[1] == 400.0 + static_cast<double>(2000.5f));
    // ground variant on p1: ground perception 0 + structures 120 + 80.
    CHECK(e.ground_value[1] == 200.0);
    // p2 neutral: unowned unit counts as hostile; no starbase.
    CHECK(e.space_total[2] == 50.0 && e.node_value[2] == 50.0);
    // p3 own but control 0: not "held" -> the starbase (0.1f, exactly as a double) is added.
    CHECK(e.node_value[3] == static_cast<double>(0.1f));
    // Q7 node costs: trunc(value) / 100.
    CHECK(node_cost(e.node_value[0]) == 5u && node_cost(e.node_value[1]) == 24u && node_cost(e.node_value[2]) == 0u);
    CHECK(node_cost(-50.0) == 42949672u && node_cost(4294967295.0) == 42949672u);
    CHECK(e.target_player_ok[2] && e.target_player[2] == 3.0);

    // Rebel (1) has never seen planet 0.
    const PlanetPerception& rb = gp.results(1);
    CHECK(rb.space_total[0] == 0.0 && rb.space_ok[0] && !rb.target_player_ok[0]);
    CHECK(rb.node_value[0] == 5000.0);  // not held: starbase power only
    CHECK(rb.space_total[1] == 100.0 && rb.node_value[1] == 100.0);  // own planet: Empire 100 only
    // Neutral (3) has no AI system: Node_Weight 0xFFFFFFFF.
    CHECK(gp.results(3).node_value[0] == 4294967295.0);

    // PlayerView for the Empire.
    galactic::PlayerView v;
    gp.fill_view(s, 0, v);
    CHECK(v.has_ai_system && !v.is_human && !v.benefits_from_corruption);
    CHECK(v.held_by_ally.test(0) && !v.held_by_ally.test(1) && !v.held_by_ally.test(2) && !v.held_by_ally.test(3));
    CHECK(!v.unowned_or_neutral.test(0) && v.unowned_or_neutral.test(2) && v.corrupted.test(2) && !v.corrupted.test(0));
    // Danger: starbase planets -> dangerous unless held; p2 (no starbase) -> HasSpaceForce (50 > 0).
    CHECK(!v.dangerous.test(0) && v.dangerous.test(1) && v.dangerous.test(2) && v.dangerous.test(3));
    // Passability inputs: perceived owners. p2 -> neutral player -> flag 0 path.
    CHECK(!v.ai_eval_failed.test(1) && v.ai_perceived_non_neutral.test(1) && !v.ai_perceived_non_neutral.test(2));
    // Rebel view of planet 0: never seen -> evaluation failed -> passable.
    galactic::PlayerView vr;
    gp.fill_view(s, 1, vr);
    CHECK(vr.ai_eval_failed.test(0) && vr.dangerous.test(0));
    galactic::PathRequest req = galactic::PathRequest::for_ai_mode(1, 0, 0);
    req.allow_unknown_or_neutral = false;
    req.allow_enemy_owned = false;
    CHECK(vr.is_passable(0, req));  // perception failure => passable (Q5)
    CHECK(!v.is_passable(1, req));  // Empire perceives a non-neutral enemy owner

    // Memory: the Rebel AI keeps its last sighting after the planet goes dark.
    s.owner[1] = 0;
    s.sees_orbit[1].set(1, false);
    gp.update(s, 60);
    CHECK(gp.results(1).target_player[1] == 1.0 && gp.memory(1).last_seen[1] == 30u);
    CHECK(gp.results(0).target_player[1] == 0.0 && gp.memory(0).last_seen[1] == 60u);

    // All-pairs inputs: node values + the Is_Planet_Revealed mask for a non-human AI.
    const galactic::AllPairsInput in = gp.all_pairs_input(0);
    CHECK(in.node_value.size() == 4 && in.revealed == &gp.revealed_mask() && in.revealed->size() == 4);
    s.revealed = galactic::Bitset(4, true);
    s.revealed.set(2, false);
    gp.update(s, 90);
    CHECK(!gp.revealed_mask().test(2) && gp.revealed_mask().test(1));
    s.multiplayer = true;
    gp.update(s, 120);
    CHECK(gp.revealed_mask().test(2));

    // The consumer paths are ordinary equations of the language.
    CHECK(gp.consumer_paths().size() == 4 && gp.consumer_paths().equation(0).parsed);
}

// GRC 2 visibility levels drive the fogged leaves per mask (AI_GALACTIC_TOKENS §0): owner 0x1, space units 0x20,
// ground units 0x80; a visibility row replaces sees_orbit for that player; fog off ignores both.
static void test_visibility_levels() {
    GalacticSnapshot s = hand_snapshot();
    s.visibility.assign(s.players.size(), {});
    s.visibility[0] = {0x1u, 0x20u, 0x80u, 0u};  // Empire: p0 owner only, p1 space only, p2 ground only, p3 nothing
    s.visibility[1] = {0xFFFFFFFFu, 0x71u, 0x191u, 0x11u};  // Rebel: owned (-1), orbit presence, ground presence, Good_Default
    CHECK(s.validate().empty());
    CHECK(s.sees(0, 0, kVisMaskOwner) && !s.sees(0, 0, kVisMaskSpaceUnits) && !s.sees(0, 3, kVisMaskOwner));
    CHECK(s.sees(1, 1, kVisMaskSpaceUnits) && !s.sees(1, 1, kVisMaskGroundUnits) && s.sees(1, 2, kVisMaskGroundUnits));
    CHECK(s.sees(1, 3, kVisMaskOwner) && !s.sees(1, 3, kVisMaskSpaceUnits));
    CHECK(s.sees(2, 0, kVisMaskSpaceUnits));  // no row for player 2: sees_orbit (empty = sees all)
    GalacticPerception gp;
    gp.reset(s);
    gp.update(s, 30);
    const PerceptionMemory& e = gp.memory(0);
    CHECK(e.seen[0] && e.owner[0] == 0 && e.space_total[0] == 0.0);   // owner visible, orbit not
    CHECK(!e.seen[1] && e.space_total[1] == 400.0);                    // space visible, owner not
    CHECK(!e.seen[3] && e.last_seen[3] == 0u && e.last_seen[1] == 30u);
    CHECK(!gp.results(0).target_player_ok[1] && gp.results(0).target_player_ok[0]);
    const PerceptionMemory& r = gp.memory(1);
    // Rebel's row overrides its sees_orbit (which hides p0): level -1 sees all, the hostile Empire 999 in orbit counts.
    CHECK(r.seen[0] && r.space_total[0] == 999.0 && r.ground_total[0] == 0.0 && gp.results(1).space_total[0] == 999.0);
    // Fog off: every leaf live whatever the rows say.
    s.fog_disabled = true;
    CHECK(s.sees(0, 3, kVisMaskOwner) && s.sees(0, 0, kVisMaskSpaceUnits));
}

// ---- determinism + golden ------------------------------------------------------------------------

static GalaxyMap synthetic_map(uint32_t n, uint32_t seed) {
    GalaxyMap m;
    m.campaign_name = "ai_synthetic";
    m.max_fleet_move_distance = 100.0f;
    SimRng r(seed);
    const float extent = 60.0f * static_cast<float>(n) / 10.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const auto p = m.planets.add("P" + std::to_string(i));
        m.planets.flags[p] = kPlanetHasPlanetBehavior;
        m.planets.position[p] = {static_cast<float>(r.range_int(0, 30000)) / 30000.0f * extent,
                                 static_cast<float>(r.range_int(0, 30000)) / 30000.0f * extent, 0.0f};
    }
    for (uint32_t i = 0; i + 1 < n; ++i) {
        const auto j = static_cast<uint32_t>(r.range_int(0, static_cast<int32_t>(n) - 1));
        if (j == i) continue;
        const auto rt = m.routes.add("R" + std::to_string(i));
        m.routes.point_a[rt] = i;
        m.routes.point_b[rt] = j;
        m.routes.hs_speed_factor[rt] = r.range_int(0, 1) ? 5.0f : 2.5f;
    }
    return m;
}

struct RunDigest {
    uint64_t perception = 0;
    uint64_t matrices = 0;
    uint64_t nonzero_costs = 0;
};

static RunDigest run_synthetic(uint32_t planets, uint32_t ais, uint32_t frames, const ParallelFor& pf) {
    const GalaxyMap map = synthetic_map(planets, 11);
    const galactic::PlanetGraph graph = galactic::PlanetGraph::build(map, {}, pf);
    GalacticSnapshot s = make_synthetic_snapshot(planets, ais, 5);
    GalacticPerception gp;
    gp.reset(s);
    CrcRegistry reg;
    register_crc(reg, gp);
    StateHasher hm;
    RunDigest d;
    std::vector<galactic::AllPairsCache> caches(ais);
    StateHasher hp;
    for (uint32_t f = 0; f < frames; ++f) {
        perturb_snapshot(s, 3, f);
        gp.update(s, f, pf);
        hp.u64(reg.capture(f).combined);
        if (f % 30 == 0) {  // all-pairs recompute (any fixed cadence: the inputs are what is tested)
            for (uint32_t a = 0; a < ais; ++a) {
                galactic::AllPairsInput in = gp.all_pairs_input(static_cast<PlayerId>(a));
                caches[a].update(graph, in, pf);
                hm.span(caches[a].matrices().weights());
                hm.span(caches[a].matrices().preds());
                for (double v : gp.node_values(static_cast<PlayerId>(a)))
                    if (node_cost(v) != 0) ++d.nonzero_costs;
            }
        }
    }
    d.perception = hp.digest();
    d.matrices = hm.digest();
    return d;
}

static ParallelFor thread_pf(uint32_t threads) {
    return [threads](uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t g = grain ? grain : 1;
        const uint32_t chunks = (count + g - 1) / g;
        std::vector<std::thread> pool;
        for (uint32_t t = 0; t < threads; ++t) {
            pool.emplace_back([&, t] {
                for (uint32_t c = t; c < chunks; c += threads) {
                    const uint32_t b = c * g;
                    const uint32_t e = b + g < count ? b + g : count;
                    fn(b, e);
                }
            });
        }
        for (auto& th : pool) th.join();
    };
}

static std::string golden_text(const RunDigest& d) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "perception %016" PRIx64 "\nmatrices %016" PRIx64 "\nnonzero_costs %" PRIu64 "\n",
                  d.perception, d.matrices, d.nonzero_costs);
    return buf;
}

static void test_determinism(bool write_golden) {
    const RunDigest serial = run_synthetic(160, 4, 181, serial_for);
    CHECK(serial.nonzero_costs > 0);  // computed, not placeholder
    const uint32_t hw = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
    for (uint32_t workers : {1u, 4u, hw}) {
        const RunDigest d = run_synthetic(160, 4, 181, thread_pf(workers));
        CHECK(d.perception == serial.perception && d.matrices == serial.matrices);
    }
#ifdef ALAMO_AI_HAVE_JOBS
    for (int32_t workers : {0, 1, 4, -1}) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        const RunDigest d = run_synthetic(160, 4, 181, jobs::make_parallel_for(js));
        CHECK(d.perception == serial.perception && d.matrices == serial.matrices);
    }
#endif
    // Above kPerceptionParallelMin the per-planet fan-out runs; results must not change.
    {
        GalacticSnapshot big = make_synthetic_snapshot(20000, 3, 9);
        GalacticPerception a, b;
        a.reset(big);
        b.reset(big);
        a.update(big, 1, serial_for);
        b.update(big, 1, thread_pf(hw < 4 ? 4 : hw));
        StateHasher ha, hb;
        a.hash(ha);
        b.hash(hb);
        CHECK(ha.digest() == hb.digest());
    }
    // Golden (MSVC / clang / gcc must agree).
    const std::string text = golden_text(serial);
    const std::string path = std::string(ALAMO_AI_GOLDEN_DIR) + "/ai_golden.txt";
    if (write_golden) {
        std::error_code ec;
        fs::create_directories(path_from_utf8(ALAMO_AI_GOLDEN_DIR), ec);
        std::ofstream(path_from_utf8(path), std::ios::binary) << text;
        std::printf("golden written: %s\n%s", path.c_str(), text.c_str());
        return;
    }
    std::vector<uint8_t> bytes;
    if (!read_host_file(path, bytes)) {
        std::printf("FAIL golden missing: %s (run with --write-golden)\n", path.c_str());
        ++g_fail;
        return;
    }
    const std::string want(bytes.begin(), bytes.end());
    if (want != text) {
        std::printf("FAIL golden mismatch:\nwant:\n%sgot:\n%s", want.c_str(), text.c_str());
        ++g_fail;
    }
}

// ---- AI execution path finder (ai_path_finder.hpp, GALACTIC.md §4) -------------------------------

// A(0,0) B(50,0) C(100,0) D(50,30); free hop <= 60: links A-B, B-C, A-D, D-C, B-D (A-C = 100 is not linked).
static GalaxyMap diamond_map() {
    GalaxyMap m;
    m.campaign_name = "ai_paths";
    m.max_fleet_move_distance = 60.0f;
    const float xy[4][2] = {{0.0f, 0.0f}, {50.0f, 0.0f}, {100.0f, 0.0f}, {50.0f, 30.0f}};
    for (uint32_t i = 0; i < 4; ++i) {
        const auto p = m.planets.add(std::string(1, static_cast<char>('A' + i)));
        m.planets.flags[p] = kPlanetHasPlanetBehavior;
        m.planets.position[p] = {xy[i][0], xy[i][1], 0.0f};
    }
    return m;
}

static void test_path_finder_cadence() {
    const galactic::PlanetGraph g = galactic::PlanetGraph::build(diamond_map());
    const std::vector<double> nv(4, 0.0);
    const galactic::AllPairsInput in = ai_path_input(nv, nullptr, true, false);
    uint32_t inputs = 0;
    auto make = [&] {
        ++inputs;
        return in;
    };
    AiPathFinder f;
    CHECK(!f.created() && f.path(0, 2).empty() && f.weight(0, 1) == galactic::kUnlinked);
    CHECK(!f.service(100, g, make));  // not created: the service does nothing
    // Creation at frame 10, slot 1 of K = 3: recompute now, next = 10 + int(1 * 30.0f + 0.5f) = 40.
    f.create(g, in, 10, 1, 3);
    CHECK(f.created() && f.recomputes() == 1 && f.next_recompute() == 40);
    for (int32_t fr = 10; fr < 40; ++fr) CHECK(!f.service(fr, g, make));
    CHECK(inputs == 0);  // inputs are built only when the cadence fires
    CHECK(f.service(40, g, make) && f.next_recompute() == 130 && f.recomputes() == 2 && inputs == 1);
    CHECK(!f.service(129, g, make));
    CHECK(f.service(133, g, make) && f.next_recompute() == 223);  // late service: next = that frame + 90
    // Same inputs: the cadence fires, the matrices are not rebuilt (identical result).
    CHECK(f.recomputes() == 3 && f.matrix_recomputes() == 1);
    // Stagger rules: K < 2 -> frame + 90; slot 2 of K = 4 -> + int(2 * 22.5f + 0.5f) = 45; slot 0 -> due at once.
    AiPathFinder a, b, c;
    a.create(g, in, 7, 3, 1);
    b.create(g, in, 7, 2, 4);
    c.create(g, in, 7, 0, 4);
    CHECK(a.next_recompute() == 97 && b.next_recompute() == 52 && c.next_recompute() == 7);
    CHECK(c.service(7, g, make) && c.next_recompute() == 97);
    // Lazy creation (0x524c10): recompute now, next stays 0, so the first service recomputes again.
    AiPathFinder lz;
    lz.create_lazy(g, in);
    CHECK(lz.created() && lz.next_recompute() == 0 && lz.recomputes() == 1);
    CHECK(lz.service(5, g, make) && lz.next_recompute() == 95 && lz.recomputes() == 2);
    // Hash covers the cadence state.
    StateHasher h1, h2;
    lz.hash(h1);
    f.hash(h2);
    CHECK(h1.digest() != h2.digest());
    lz.reset();
    CHECK(!lz.created() && lz.path(0, 1).empty());
}

static void test_path_finder_costs_and_fog() {
    const galactic::PlanetGraph g = galactic::PlanetGraph::build(diamond_map());
    const PlanetIndex A = 0, B = 1, C = 2, D = 3;
    const auto hop = [](float d) { return static_cast<uint32_t>(galactic::hop_from_distance(d)); };
    const float ad = std::sqrt(50.0f * 50.0f + 30.0f * 30.0f);
    {   // No node costs: A -> B -> C (hop 500 + 500) beats A -> D -> C (583 + 583).
        const std::vector<double> nv(4, 0.0);
        AiPathFinder f;
        f.create(g, ai_path_input(nv, nullptr, true, false), 0, 0, 1);
        CHECK(f.path(A, C) == (std::vector<PlanetIndex>{A, B, C}));
        CHECK(f.weight(A, B) == hop(50.0f) && f.weight(A, C) == 2u * hop(50.0f));
        std::vector<PlanetIndex> p;
        std::vector<uint32_t> w;
        CHECK(f.get_path(A, C, p, &w) && w == (std::vector<uint32_t>{hop(50.0f), hop(50.0f), 0u}));  // per hop (Q10)
        CHECK(f.path(B, B) == (std::vector<PlanetIndex>{B, B}));  // src == dst -> [src, src]
        CHECK(f.path(A, 7).empty());                              // unknown planet
    }
    // Node_Weight on B (perception 1234.9 -> trunc / 100 = 12): every edge INTO B costs 12 * 65536 + hop (Q7/Q8), so
    // A -> D -> C wins; the all-pairs value is the sum of the per-hop values.
    std::vector<double> nv(4, 0.0);
    nv[B] = 1234.9;
    CHECK(node_cost(nv[B]) == 12u);
    AiPathFinder f;
    f.create(g, ai_path_input(nv, nullptr, true, false), 0, 0, 1);
    CHECK(f.weight(A, B) == galactic::pack_edge_weight(12u, galactic::hop_from_distance(50.0f)) && f.weight(B, A) == hop(50.0f));
    CHECK(f.path(A, C) == (std::vector<PlanetIndex>{A, D, C}));
    CHECK(f.weight(A, D) == hop(ad) && f.weight(A, C) == f.weight(A, D) + f.weight(D, C));
    // Fog gate (Q8 step 2): D not revealed -> every edge touching D is unlinked for a NON-HUMAN AI player, so the
    // costly B is the only way; a human player with an AI system and a player without one get no gate.
    galactic::Bitset revealed(4, true);
    revealed.set(D, false);
    AiPathFinder ai, human, no_ai;
    ai.create(g, ai_path_input(nv, &revealed, true, false), 0, 0, 1);
    human.create(g, ai_path_input(nv, &revealed, true, true), 0, 0, 1);
    no_ai.create(g, ai_path_input(nv, &revealed, false, false), 0, 0, 1);
    CHECK(ai.path(A, C) == (std::vector<PlanetIndex>{A, B, C}) && ai.path(A, D).empty() && ai.path(D, D).size() == 2);
    CHECK(human.path(A, C) == (std::vector<PlanetIndex>{A, D, C}) && no_ai.path(A, C) == human.path(A, C));
    // The fog mask from perception: Is_Planet_Revealed honours Planet_Auto_Reveal (but not over the hidden byte), and
    // all_pairs_input hands it only to non-human AI players.
    GalacticSnapshot s = make_synthetic_snapshot(4, 2, 1);
    s.players[1].is_human = true;
    s.revealed = galactic::Bitset(4, true);
    s.revealed.set(D, false);
    s.hidden = galactic::Bitset(4);
    GalacticPerception gp;
    gp.reset(s);
    gp.update(s, 0);
    CHECK(gp.tracks(0) && gp.tracks(1));
    CHECK(gp.all_pairs_input(0).revealed == &gp.revealed_mask() && gp.all_pairs_input(1).revealed == nullptr);
    CHECK(!gp.revealed_mask().test(D));
    s.planet_auto_reveal = true;
    gp.update(s, 1);
    CHECK(gp.revealed_mask().test(D));
    s.hidden.set(D, true);
    gp.update(s, 2);
    CHECK(!gp.revealed_mask().test(D));
    // Layer_Z_Adjust (Q2): the SOURCE type's value raises the destination's z; D -> B becomes 50 instead of 30 here.
    const std::vector<float> z = {0.0f, 0.0f, 0.0f, 40.0f};
    AiPathFinder zf;
    zf.create(g, ai_path_input(std::vector<double>(4, 0.0), nullptr, true, false, z), 0, 0, 1);
    CHECK(zf.weight(D, B) == hop(50.0f) && zf.weight(B, D) == hop(30.0f));
}

// The finder on the Q11 cadence over a perturbed synthetic galaxy: K AI players, staggered, 90-frame services every
// 3 frames (the Execution interval). Digest over every finder's hash after each frame + all paths at the end.
// OPT2 t02 / OPT3 t04: `latency` > 0 runs the background mode on `bg` (publish at the start of each frame); `save_at` >= 0
// saves every finder at the start of that frame and continues with fresh finders read from the save
// (`saved_in_flight`: whether a recompute was in flight then).
static uint64_t run_path_finders(uint32_t planets, uint32_t ais, uint32_t frames, const ParallelFor& pf, uint32_t latency = 0,
                                 const BackgroundStart& bg = {}, int32_t save_at = -1, bool* saved_in_flight = nullptr) {
    const galactic::PlanetGraph graph = galactic::PlanetGraph::build(synthetic_map(planets, 23), {}, pf);
    GalacticSnapshot s = make_synthetic_snapshot(planets, ais, 7);
    for (uint32_t p = 0; p < planets; ++p) s.revealed.set(p, p % 13 != 5);  // some fog for the gate
    GalacticPerception gp;
    gp.reset(s);
    std::vector<AiPathFinder> f(ais);
    for (AiPathFinder& x : f) x.set_background(latency, bg);
    StateHasher h;
    for (uint32_t fr = 0; fr < frames; ++fr) {
        perturb_snapshot(s, 3, fr);
        gp.update(s, fr, pf);
        const auto frame = static_cast<int32_t>(fr);
        if (frame == save_at) {
            ByteWriter w;
            for (const AiPathFinder& x : f) {
                x.write(w);
                if (saved_in_flight) *saved_in_flight = *saved_in_flight || x.pending();
            }
            std::vector<AiPathFinder> loaded(ais);
            ByteReader r(w.bytes());
            for (AiPathFinder& x : loaded) {
                x.set_background(latency, bg);
                CHECK(x.read(r, graph, pf));
            }
            CHECK(r.ok() && r.done());
            f = std::move(loaded);
        }
        for (AiPathFinder& x : f) x.publish(frame);
        for (uint32_t a = 0; a < ais; ++a) {
            const auto p = static_cast<PlayerId>(a);
            if (fr == 1) f[a].create(graph, gp.all_pairs_input(p), frame, static_cast<int32_t>(a), static_cast<int32_t>(ais), pf);
            else if (fr % 3 == 1) f[a].service(frame, graph, [&] { return gp.all_pairs_input(p); }, pf);
            f[a].hash(h);
        }
    }
    uint32_t reachable = 0;
    for (uint32_t a = 0; a < ais; ++a)
        for (PlanetIndex i = 0; i < planets; i += 7)
            for (PlanetIndex j = 0; j < planets; j += 5) {
                const std::vector<PlanetIndex> p = f[a].path(i, j);
                h.span(p);
                reachable += p.empty() ? 0u : 1u;
            }
    CHECK(reachable > 0);
    for (const AiPathFinder& x : f) CHECK(x.recomputes() >= frames / 90);
    return h.digest();
}

// OPT2 t02: the background mode. A recompute kicked on frame N is applied on N + latency exactly (never earlier, also
// when it finished at once), results are identical for every worker count, and a save with recomputes in flight loads
// into a run identical to the uninterrupted one (OPT3 t04).
static void test_path_finder_background() {
    const galactic::PlanetGraph g = galactic::PlanetGraph::build(diamond_map());
    const std::vector<double> flat(4, 0.0);
    std::vector<double> costly(4, 0.0);
    costly[1] = 5000.0;  // B expensive: A -> C goes over D
    AiPathFinder f;
    f.set_background(15);
    f.create(g, ai_path_input(flat, nullptr, true, false), 10, 1, 3);  // creation stays synchronous; next = 40
    CHECK(f.path(0, 2) == (std::vector<PlanetIndex>{0, 1, 2}) && !f.pending());
    const galactic::AllPairsInput in = ai_path_input(costly, nullptr, true, false);
    CHECK(f.service(40, g, [&] { return in; }) && f.pending() && f.ready_frame() == 55 && f.recomputes() == 2);
    for (int32_t fr = 40; fr < 55; ++fr) {
        CHECK(!f.publish(fr));
        CHECK(f.path(0, 2) == (std::vector<PlanetIndex>{0, 1, 2}));  // the previous matrices until N + k
    }
    CHECK(f.publish(55) && !f.pending() && f.path(0, 2) == (std::vector<PlanetIndex>{0, 3, 2}) && f.matrix_recomputes() == 2);
    CHECK(!f.publish(56));
    // Hash covers the in-flight state.
    AiPathFinder a, b;
    for (AiPathFinder* x : {&a, &b}) {
        x->set_background(15);
        x->create(g, ai_path_input(flat, nullptr, true, false), 0, 0, 1);
    }
    CHECK(b.service(90, g, [&] { return ai_path_input(flat, nullptr, true, false); }));
    StateHasher ha, hb;
    a.hash(ha);
    b.hash(hb);
    CHECK(ha.digest() != hb.digest());
    // Same results as the synchronous run apart from the timing; identical for every host and worker count; saves.
    const uint64_t sync = run_path_finders(160, 3, 400, serial_for);
    const uint64_t serial = run_path_finders(160, 3, 400, serial_for, 15);
    CHECK(serial != sync);
    CHECK(run_path_finders(160, 3, 400, thread_pf(4), 15) == serial);
    // Kicks (creation at 1, slots 0..2 of 3, services every 3rd frame): 4, 94, 184 / 31, 121, 211 / 61, 151, 241.
    for (const int32_t save_at : {136, 160, 200}) {
        bool in_flight = false;
        CHECK(run_path_finders(160, 3, 400, serial_for, 15, {}, save_at, &in_flight) == serial);
        CHECK(in_flight == (save_at != 200));  // 136: kicked at 121, due that frame; 160: kicked at 151
    }
#ifdef ALAMO_AI_HAVE_JOBS
    for (int32_t workers : {0, 1, 4, 16, -1}) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        const ParallelFor pf = jobs::make_parallel_for(js);
        const BackgroundStart bg = jobs::make_background_start(js);
        CHECK(run_path_finders(160, 3, 400, pf, 15, bg) == serial);
        CHECK(run_path_finders(160, 3, 400, pf, 15, bg, 160) == serial);
        CHECK(run_path_finders(300, 3, 200, pf, 15, bg) == run_path_finders(300, 3, 200, serial_for, 15));
    }
#endif
}

static void test_path_finder_determinism() {
    const uint32_t hw = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
    // 160 planets: the fast path (when eligible); 300: the row-parallel reference (>= kParallelReferenceMinPlanets).
    for (const uint32_t planets : {40u, 160u, 300u}) {
        const uint32_t frames = planets >= 300 ? 200u : 400u;
        const uint64_t serial = run_path_finders(planets, 3, frames, serial_for);
        for (uint32_t workers : {1u, 4u, hw}) CHECK(run_path_finders(planets, 3, frames, thread_pf(workers)) == serial);
#ifdef ALAMO_AI_HAVE_JOBS
        for (int32_t workers : {0, 1, 4, -1}) {
            jobs::JobSystem js(jobs::JobSystemConfig{workers});
            CHECK(run_path_finders(planets, 3, frames, jobs::make_parallel_for(js)) == serial);
        }
#endif
    }
}

// ---- real data -----------------------------------------------------------------------------------

static std::string env_or(const char* name, const char* fallback) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out = fallback;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        if (*v) out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
#endif
}

// q6: galactic seed roles (make_force_type_lookup) follow galactic::galactic_unit_kind. Synthetic types: a mod tag takes
// its DUMMY_* behaviour's role; the seed's vanilla tags keep their rule (a vanilla-unclassified tag ignores the behaviour).
namespace {
class MemFiles final : public FileProvider {
public:
    void add(const std::string& path, const std::string& text) { files_[normalize_path(path)] = std::vector<uint8_t>(text.begin(), text.end()); }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files_.find(normalize_path(path));
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem:" + std::string(path) : ""; }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};
}  // namespace

static void test_seed_roles() {
    MemFiles f;
    f.add("Data\\XML\\GameObjectFiles.xml", "<L><File>SeedTypes.xml</File></L>");
    f.add("Data\\XML\\SeedTypes.xml",
          "<Types>"
          "<SkirmishSpaceUnit Name=\"Mod_Ship\"><Behavior>DUMMY_STARSHIP</Behavior><CategoryMask>Infantry</CategoryMask></SkirmishSpaceUnit>"
          "<ModStation Name=\"Mod_Base\"><Behavior>DUMMY_STAR_BASE</Behavior><Base_Level>3</Base_Level></ModStation>"
          "<ModBuilding Name=\"Mod_Barracks\"><Behavior>DUMMY_GROUND_STRUCTURE</Behavior></ModBuilding>"
          "<ModCompany Name=\"Mod_Company\"><Behavior>DUMMY_GROUND_COMPANY</Behavior><CategoryMask>Capital</CategoryMask></ModCompany>"
          "<ModWing Name=\"Mod_Wing\"><Behavior>DUMMY_SPACE_FIGHTER_SQUADRON</Behavior></ModWing>"
          "<ModThing Name=\"Mod_Plain\"><CategoryMask>Frigate</CategoryMask></ModThing>"
          "<SpaceUnit Name=\"Van_Frigate\"><CategoryMask>Frigate</CategoryMask></SpaceUnit>"
          "<GroundBase Name=\"Van_Ground_Base\"><Behavior>DUMMY_GROUND_BASE</Behavior></GroundBase>"
          "<Props_Story Name=\"Van_Story_Prop\"><Behavior>DUMMY_STARSHIP</Behavior></Props_Story>"
          "</Types>");
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    CHECK(raw.load(f) && types.build(raw, &f) && types.size() == 9);
    const ForceTypeLookup lookup = make_force_type_lookup(types);
    const ForceType ship = lookup("Mod_Ship"), base = lookup("Mod_Base"), barracks = lookup("Mod_Barracks"),
                    company = lookup("Mod_Company"), wing = lookup("Mod_Wing"), plain = lookup("Mod_Plain"),
                    frigate = lookup("Van_Frigate"), gbase = lookup("Van_Ground_Base"), story = lookup("Van_Story_Prop");
    CHECK(ship.valid && ship.layer == UnitLayer::Space && !ship.starbase && !ship.structure);  // the old rule: ground
    CHECK(base.valid && base.starbase && base.layer == UnitLayer::Space && base.base_level == 3);
    CHECK(barracks.valid && barracks.structure && barracks.layer == UnitLayer::Ground);
    CHECK(company.valid && company.layer == UnitLayer::Ground && !company.structure);  // the old rule: space (Capital)
    CHECK(wing.valid && wing.layer == UnitLayer::Space);
    CHECK(plain.valid && plain.layer == UnitLayer::Space && !plain.structure);  // no behaviour: CategoryMask
    CHECK(frigate.valid && frigate.layer == UnitLayer::Space);
    CHECK(gbase.valid && !gbase.structure && gbase.layer == UnitLayer::Ground);  // the seed's GroundBase rule [port]
    CHECK(story.valid && story.layer == UnitLayer::Ground);  // vanilla-unclassified tag: the behaviour is ignored
    CHECK(!lookup("Missing").valid);
    const ForceType old_ship = force_type_of(types, types.resolve_name("Mod_Ship"), true);
    CHECK(old_ship.layer == UnitLayer::Ground);
    std::printf("   seed roles (synthetic): mod tags by DUMMY_* behaviour, vanilla tags unchanged\n");
}

static void real_data(const std::string& root, const char* label, bool foc) {
    std::error_code ec;
    if (!fs::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("%s data not found at %s: real-data tests skipped\n", label, root.c_str());
        return;
    }
#ifndef ALAMO_AI_HAVE_VFS
    (void)foc;
    std::printf("%s: this build has no vfs module: real-data tests skipped\n", label);
#else
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    if (!foc) cfg.eaw_root = "";
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("%s: VFS failed (%s): skipped\n", label, err.c_str());
        return;
    }
    // 1. Every shipped perception equation file.
    const std::vector<std::string> files = v->list(kPerceptionEquationGlob);
    EquationSet set;
    const LoadReport rep = load_equation_files(*v, files, set);
    std::printf("== %s perception equations: %u files, %u equations, %u parse errors, %u duplicates, "
                "%u precedence-sensitive, %u unresolved Function_ refs\n",
                label, rep.files, rep.equations, rep.parse_errors, rep.duplicates, rep.precedence_sensitive,
                rep.unresolved_functions);
    CHECK(rep.files >= 10 && rep.files_failed == 0);
    CHECK(rep.parse_errors == 0);
    CHECK(rep.equations >= 200);
    // Coverage against the galactic resolver.
    GalacticSnapshot dummy = make_synthetic_snapshot(1, 1, 1);
    std::vector<PerceptionMemory> mem(dummy.players.size());
    const GalacticTokenResolver resolver(dummy, mem);
    const std::string report = coverage_report(set, rep, &resolver, label);
    const std::string out = std::string(ALAMO_AI_LOG_DIR) + "/ai_perception_coverage_" + (foc ? "foc" : "eaw") + ".txt";
    fs::create_directories(path_from_utf8(ALAMO_AI_LOG_DIR), ec);
    std::ofstream(path_from_utf8(out), std::ios::binary) << report;
    const std::vector<CoverageEntry> cov = coverage(set, &resolver);
    uint32_t paths = 0, registered = 0, implemented = 0;
    for (const CoverageEntry& c : cov) {
        if (c.kind != CoverageEntry::Kind::Token) continue;
        ++paths;
        if (c.registered) ++registered;
        if (c.implemented) ++implemented;
        if (c.path == "Variable_Target.EnemyForce.HasSpaceForce" ||
            c.path == "Variable_Target.EnemyForce.SpaceTotalUnnormalized" ||
            c.path == "Variable_Target.EnemyForce.GroundTotalUnnormalized")
            CHECK(c.registered && c.implemented);
    }
    std::printf("   token paths %u: registered %u, implemented %u -> %s\n", paths, registered, implemented, out.c_str());
    // Every equation evaluates without crashing (most fail on not-yet-implemented tokens).
    {
        for (PerceptionMemory& m : mem) m.reset(dummy.planet_count);
        uint32_t ok = 0;
        SimRng rng(1);
        for (uint32_t i = 0; i < set.size(); ++i) {
            EvalContext ctx;
            ctx.self = 0;
            ctx.target = 0;
            ctx.rng = &rng;
            if (evaluate(set, i, ctx, resolver).ok) ++ok;
        }
        std::printf("   equations evaluable with the galactic resolver alone: %u / %zu\n", ok, set.size());
    }
    // S2 grammar (STRATEGIC_GAPS §3): the precedence-dependent equations, and every equation evaluates when each token
    // resolves (constant 1.5); only Script_ references (no Lua here) may fail.
    {
        struct AllTokens final : TokenResolver {
            bool resolve(const EvalContext&, const TokenRef&, double& out) const override {
                out = 1.5;
                return true;
            }
            bool implements(Root, std::span<const TokenId>, const TokenTable&) const override { return true; }
        } all;
        uint32_t sensitive = 0, ok = 0, script = 0, other = 0;
        std::string names;
        for (uint32_t i = 0; i < set.size(); ++i) {
            const Equation& e = set.equation(i);
            if (precedence_sensitive(e.text)) {
                ++sensitive;
                names += (names.empty() ? "" : ", ") + e.name;
            }
            EvalContext ctx;
            ctx.self = 0;
            const EvalResult r = evaluate(set, i, ctx, all);
            if (r.ok) ++ok;
            else if (r.error == EvalError::Script) ++script;
            else {
                ++other;
                std::printf("   FAIL %s: %s\n", e.name.c_str(), eval_error_name(r.error));
            }
        }
        std::printf("   precedence-sensitive (%u): %s\n", sensitive, names.c_str());
        std::printf("   all tokens resolved: %u / %zu evaluate, %u fail only on Script_, %u other failures\n", ok, set.size(),
                    script, other);
        CHECK(other == 0);
        if (foc) CHECK(sensitive == rep.precedence_sensitive);
    }

    // 2. Every campaign: seeded snapshot -> perception -> all-pairs.
    data::GameDatabase db;
    std::vector<std::string> diag;
    db.load(*v, {}, &diag);
    objects::ObjectTypeDatabase raw;
    raw.load(*v, &diag);
    objects::GameObjectTypes types;
    types.build(raw, v.get(), &diag);
    // q6: the seed's roles come from galactic_unit_kind; every vanilla type keeps the role of the seed's old tag rule.
    {
        uint32_t differ = 0;
        for (objects::TypeIndex t = 0; t < types.size(); ++t) {
            const ForceType a = force_type_of(types, t), b = force_type_of(types, t, true);
            if (a.layer == b.layer && a.starbase == b.starbase && a.structure == b.structure && a.base_level == b.base_level)
                continue;
            if (++differ <= 20)
                std::printf("   seed role differs: %s <%.*s> layer %d/%d starbase %d/%d structure %d/%d\n",
                            std::string(types.raw().types().name[t]).c_str(), static_cast<int>(types.raw().element_name(t).size()),
                            types.raw().element_name(t).data(), static_cast<int>(a.layer), static_cast<int>(b.layer), a.starbase,
                            b.starbase, a.structure, b.structure);
        }
        std::printf("   %s seed roles: %zu types, %u differ from the old tag rule\n", label, types.size(), differ);
        CHECK(differ == 0);
    }
    const ForceTypeLookup lookup = make_force_type_lookup(types);
    uint32_t campaigns = 0, with_values = 0, unknown_forces = 0;
    for (const data::CampaignDef& c : db.campaigns()) {
        const GalaxyMap map = data::build_galaxy_map(db, c.name);
        if (map.planets.size() == 0) continue;
        CampaignSeed seed = seed_campaign_snapshot(c, map, lookup);
        CHECK(seed.snapshot.validate().empty());
        for (const std::string& d : seed.diagnostics)
            if (d.find("unknown type") != std::string::npos) ++unknown_forces;
        GalacticPerception gp;
        gp.reset(seed.snapshot);
        gp.update(seed.snapshot, 0);
        const galactic::PlanetGraph graph = galactic::PlanetGraph::build(map);
        bool any = false;
        for (size_t p = 0; p < seed.snapshot.players.size(); ++p) {
            if (!gp.tracks(static_cast<PlayerId>(p))) continue;
            for (double val : gp.node_values(static_cast<PlayerId>(p)))
                if (node_cost(val) != 0) any = true;
            galactic::AllPairsMatrices m;
            galactic::compute_all_pairs(graph, gp.all_pairs_input(static_cast<PlayerId>(p)), m);
            CHECK(m.size() == graph.size());
        }
        ++campaigns;
        if (any) ++with_values;
    }
    std::printf("   campaigns seeded %u, with non-zero node costs %u, unknown Starting_Forces types %u\n", campaigns,
                with_values, unknown_forces);
    CHECK(campaigns > 0 && with_values > 0);
#endif
}

// q6: Thrawn's Revenge (workshop 1125571106, next to a Steam FoC install or ALAMO_TR_DATA) through the VFS: its own
// element tags (SkirmishSpaceUnit, DummyStructure, ...) take their DUMMY_* roles in the galactic seed.
static void tr_seed_roles(const std::string& foc_data) {
#ifdef ALAMO_AI_HAVE_VFS
    std::string tr = env_or("ALAMO_TR_DATA", "");
    const size_t at = foc_data.find("\\steamapps\\common\\");
    if (tr.empty() && at != std::string::npos) tr = foc_data.substr(0, at) + "\\steamapps\\workshop\\content\\32470\\1125571106";
    std::error_code ec;
    if (tr.empty() || foc_data.empty() || !fs::is_directory(path_from_utf8(tr) / "Data" / "XML", ec)) {
        std::printf("Thrawn's Revenge data not found (ALAMO_TR_DATA / Steam library): TR seed roles skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = foc_data;
    cfg.mod_paths = {tr};
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("Thrawn's Revenge: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    std::vector<std::string> diag;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    CHECK(raw.load(*v, &diag) && types.build(raw, v.get(), &diag));
    uint32_t differ = 0, starbases = 0, structures = 0, space = 0;
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        const ForceType a = force_type_of(types, t), b = force_type_of(types, t, true);
        if (a.layer == b.layer && a.starbase == b.starbase && a.structure == b.structure) continue;
        ++differ;
        starbases += a.starbase ? 1u : 0u;
        structures += a.structure ? 1u : 0u;
        space += !a.starbase && a.layer == UnitLayer::Space ? 1u : 0u;
    }
    std::printf("   Thrawn's Revenge seed roles: %zu types, %u take a DUMMY_* role the old tag rule missed (%u starbases, %u "
                "structures, %u space units)\n",
                types.size(), differ, starbases, structures, space);
    CHECK(differ > 0 && structures > 0 && space > 0);
#else
    (void)foc_data;
#endif
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Error);
    bool write_golden = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--write-golden") write_golden = true;
        if (std::string(argv[i]) == "--tr-seed-roles") {  // q6: the Thrawn's Revenge check alone (slow: ~19k types)
            tr_seed_roles(env_or("ALAMO_FOC_DATA", ""));
            std::printf(g_fail ? "alamo_ai_tests: %d failure(s)\n" : "alamo_ai_tests: OK\n", g_fail);
            return g_fail ? 1 : 0;
        }
    }
    test_language();
    test_hand_cases();
    test_visibility_levels();
    test_determinism(write_golden);
    test_path_finder_cadence();
    test_path_finder_costs_and_fog();
    test_path_finder_determinism();
    test_path_finder_background();
    test_seed_roles();
    real_data(env_or("ALAMO_FOC_DATA", "/home/claude/data/foc"), "FoC", true);
    real_data(env_or("ALAMO_EAW_DATA", "/home/claude/data/eaw"), "EaW", false);
    std::printf(g_fail ? "alamo_ai_tests: %d failure(s)\n" : "alamo_ai_tests: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
