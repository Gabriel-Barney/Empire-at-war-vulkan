// alamo_galactic_economy_tests: S3 galactic economy & fleets (docs/re/GALACTIC_ECONOMY.md).
//   1. formula unit tests with hand-computed values;
//   2. scenario tests on a synthetic galaxy (income, corruption, caps, production queues and refunds, fleet
//      movement, interception, merging, piracy, ownership and capture);
//   3. a scripted synthetic campaign (commands through engine/commands) whose per-frame CRC log must be
//      identical for 0/1/4/N workers and match tests/golden/galactic_synthetic_crc.txt;
//   4. the same on real FoC data (ALAMO_FOC_DATA = folder with loose Data\XML; skipped when unset):
//      Sandbox_Heart_Of_The_Maw_Underworld for N galactic days, golden galactic_foc_crc.txt.
// Args: --golden-dir <dir> (compare), --write-golden (rewrite the golden files instead), --quick (1./2. only),
// --only units|synthetic|foc (one part; CMake registers the three parts as separate ctest entries).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alamo/commands/pipeline.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/galactic/victory.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "foc_data.hpp"
#include "thread_parallel_for.hpp"
#ifdef ALAMO_GALACTIC_TESTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::galactic;
using commands::RejectReason;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// ---- 1. formulas ---------------------------------------------------------------------------------------------

void test_formulas() {
    using namespace econ;
    CHECK(fiscal_period_frames(45.0f) == 1350);
    CHECK(fiscal_period_frames(0.05f) == 2);  // 1.5 rounds half away from zero
    CHECK(timer_frames(8.0f) == 240);
    CHECK(timer_frames(13.5f) == 405);
    CHECK(timer_frames(6.4f) == 192);
    CHECK(timer_remaining_seconds(100, 240, 130) == 7.0f);
    CHECK(timer_remaining_seconds(100, 240, 400) == 0.0f);
    CHECK(timer_due(100, 240, 340) && !timer_due(100, 240, 339));

    CreditAccount a{100.0f, -1.0f};
    CHECK(add_credits(a, 50.0f, false, 1.2f) == 50.0f && a.credits == 150.0f);
    CHECK(add_credits(a, 100.0f, true, 1.2f) == 120.0f && a.credits == 270.0f);  // AI gain x Credit_Multiplier
    CHECK(add_credits(a, -100.0f, true, 1.2f) == -100.0f && a.credits == 170.0f); // costs are not multiplied
    CHECK(add_credits(a, -500.0f, false, 1.0f) == -170.0f && a.credits == 0.0f);  // no debt
    a = {900.0f, 1000.0f};
    CHECK(add_credits(a, 500.0f, false, 1.0f) == 100.0f && a.credits == 1000.0f);  // cap
    CHECK(credit_cap(0, 40000.0f) == 40000.0f && credit_cap(3, 40000.0f) == 120000.0f);

    bool keep = false;
    CHECK(income_base(10, 2.5f) == 25.0f);
    CHECK(income_force(10, ForceSide::Light, ForceSide::Light, 0.5f, 0.5f, 0.5f, &keep) == 2.5f && keep);
    CHECK(income_force(10, ForceSide::Light, ForceSide::Dark, 0.5f, 0.5f, 0.5f, &keep) == -2.5f && keep);
    CHECK(income_force(10, ForceSide::Neutral, ForceSide::Dark, 0.5f, 0.5f, 0.5f, &keep) == 0.0f && !keep);
    const float z = income_force(10, ForceSide::Dark, ForceSide::Neutral, 0.0f, 0.5f, 0.5f, &keep);
    CHECK(z == 0.0f && std::signbit(z) && keep);  // -0.0 entry kept (vanilla planets are Neutral, k = 0)
    const PiecewiseCurve curve = PiecewiseCurve::parse("0,0, 1,1, 5,1");
    CHECK(curve.x.size() == 3 && curve.eval(0.25f) == 0.25f && curve.eval(-1.0f) == 0.0f && curve.eval(9.0f) == 1.0f);
    CHECK(PiecewiseCurve::parse("0,1, 1,1, 5,1").eval(0.5f) == 1.0f);
    CHECK(income_political(10, 1.0f, curve) == 0.0f);
    CHECK(income_political(10, 0.5f, curve) == -5.0f);  // negative while the control ramps
    CHECK(income_political(10, 0.0f, curve) == 0.0f);   // q = 0 -> no term
    CHECK(income_trade(10, 1.0f) == 10.0f);
    float s = 0.0f;
    CHECK(income_finish(30.0f, true, 40.0f, false, 0.0f, false, 0.0f, &s) == 40.0f);
    CHECK(income_finish(30.0f, false, 0.0f, true, 20.0f, false, 0.0f, &s) == 20.0f);
    CHECK(income_finish(30.0f, false, 0.0f, false, 0.0f, true, 0.25f, &s) == 22.5f && s == 7.5f);
    CHECK(income_finish(30.0f, false, 0.0f, false, 0.0f, true, 1.5f, &s) == 30.0f && s == 0.0f);
    EconomyConstants c;
    CHECK(corruption_income_pct(c, 3) == 4.5f && corruption_income_pct(c, 8) == 0.0f &&
          corruption_income_pct(c, -1) == 0.0f);

    CHECK(build_time_t0(15, 1.0f) == 15.0f && build_time_t0(10, 0.95f) == 9.0f);
    CHECK(tech_time_factor(3, 1, c.base_vs_tech) == 0.75f && tech_time_factor(5, 0, c.base_vs_tech) == 0.45f &&
          tech_time_factor(1, 2, c.base_vs_tech) == 1.0f && tech_time_factor(2, 1, c.base_vs_tech) == 0.9f);
    BuildTimeInputs in;
    in.build_time_seconds = 15;
    in.tech_factor_applies = true;
    in.starbase_level = 3;
    in.tech_level = 1;
    in.base_vs_tech = &c.base_vs_tech;
    in.ai_multiplier = 0.5f;
    in.corruption_multiplier = 0.75f;
    in.factories = 2;
    CHECK(build_time(in) == 2.109375f);  // 15 * 0.75 * 0.5 * 0.75 / 2
    in.planet_bonus = 0.25f;
    CHECK(build_time(in) == 1.58203125f);
    in.starbase_level = 0;  // tech factor needs L in 1..5
    CHECK(build_time(in) == 2.109375f);
    in.cheat = true;
    CHECK(build_time(in) == 5.0f);
    CHECK(build_cost(1000, 1.0f, 0.9f, 1.0f) == 900);
    CHECK(build_cost(1, 0.5f, 1.0f, 1.0f) == 1);  // raw > 0 -> at least 1
    CHECK(build_cost(3, 0.5f, 1.0f, 1.0f) == 2 && build_cost(5, 0.5f, 1.0f, 1.0f) == 3);  // half away
    CHECK(build_cost(0, 1.0f, 1.0f, 1.0f) == 0);
    CHECK(payg_installment(8.0f, 7.5f, 10.0f, 1000.0f) == 50.0f);
    CHECK(refund_amount(false, 2.5f, 10.0f, 1000.0f) == 1000.0f);
    CHECK(refund_amount(true, 2.5f, 10.0f, 1000.0f) == 750.0f);
    CHECK(refund_amount(true, 12.0f, 10.0f, 1000.0f) == 0.0f);

    CHECK(hop_factor(false, true, true, false, 0.0f, 1.5f) == 1.0f);
    CHECK(hop_factor(true, true, true, false, 0.0f, 1.5f) == 1.5f);
    CHECK(hop_factor(true, true, false, false, 0.0f, 1.5f) == 1.0f);
    CHECK(hop_factor(false, false, false, true, 5.0f, 1.5f) == 5.0f);
    CHECK(hop_factor(true, true, true, true, 0.5f, 1.5f) == 1.5f);  // a slow route never slows
    CHECK(hop_factor(false, false, false, true, 0.5f, 1.5f) == 1.0f);
    const float v0 = base_speed(1.5f, 0.07f, 1.0f);
    CHECK(v0 == 1.5f * 0.07f);
    CHECK(hop_frames_1d(60.0f, hop_speed(v0, 5.0f)) == 113);  // GE 3.2 example: ~113 frames
    CHECK(hop_frames_1d(60.0f, v0) == 570);                   // no route: ~20 s
    Vec3f p{0, 0, 0}, f{};
    CHECK(!fleet_step(p, {10, 0, 0}, 1.0f, &f) && p.x == 1.0f && f.x == 1.0f);
    p = {7.5f, 0, 0};
    CHECK(fleet_step(p, {10, 0, 0}, 1.0f, &f) && p.x == 10.0f);  // 1.5 left < 2 * 1 -> snap

    CHECK(political_delta(1, 1.0f, 7.0f) == 1.0f / 210.0f);
    float ctl = 0.0f;
    int frames = 0;
    while (ctl < 1.0f && frames < 1000) {
        ctl = political_ramp(ctl, 1.0f, political_delta(1, 1.0f, 7.0f));
        ++frames;
    }
    CHECK(frames >= 210 && frames <= 211);
    CHECK(influence_alignment_factor(ForceSide::Light, ForceSide::Light, 0.5f, 0.5f, 0.5f) == 0.75f);
    CHECK(influence_alignment_factor(ForceSide::Light, ForceSide::Dark, 0.5f, 0.5f, 0.5f) == 1.25f);
    CHECK(influence_alignment_factor(ForceSide::Neutral, ForceSide::Dark, 0.5f, 0.5f, 0.5f) == 1.0f);
    CHECK(political_ramp(0.9f, 0.5f, 0.25f) == 0.65f && political_ramp(0.6f, 0.5f, 0.25f) == 0.5f);
    CHECK(parse_force_side(" light ") == ForceSide::Light && parse_force_side("DARK") == ForceSide::Dark &&
          parse_force_side("Neutral") == ForceSide::Neutral);

    // Black-market draw = the synced LCG step, (max - min) * (r15 / 32767) + min.
    SimRng r(12345u);
    const uint32_t st = 12345u * 0x41C64E6Du + 0xBDFu;
    const float u = static_cast<float>((st >> 10) & 0x7FFFu) / 32767.0f;
    CHECK(r.range_float(1.0f, 3.0f) == 2.0f * u + 1.0f && r.state() == st);
}

// ---- synthetic galaxy -------------------------------------------------------------------------------------------

enum TypeId : uint32_t {
    tPlanetA, tPlanetB, tPlanetC, tPlanetD, tPlanetE,
    tSB1, tSB2, tFrigate, tFighters, tInfantry, tBarracks, tTechUp, tCorvette, tTypeCount
};
enum Row : PlanetIndex { rCore = 0, rA = 1, rB = 2, rC = 3, rD = 4, rE = 5 };
enum PlayerId : int32_t { pEmpire = 0, pUnder = 1, pNeutral = 2 };

GalacticRules make_rules() {
    GalacticRules r;
    FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    emp.force_alignment = ForceSide::Dark;
    emp.corruption_time_mult = {1.0f, 0.95f};
    emp.corruption_price_mult = {1.0f, 0.9f};
    FactionRules und;
    und.name = "Underworld";
    und.can_control_planets = und.is_playable = und.benefits_from_corruption = true;
    und.force_alignment = ForceSide::Light;
    FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, und, neu};
    r.difficulties = {{"Normal", 1.0f, 0.8f}, {"Hard", 1.2f, 0.6f}};
    r.types.resize(tTypeCount);
    const char* pnames[] = {"PlanetA", "PlanetB", "PlanetC", "PlanetD", "PlanetE"};
    const int32_t credit[] = {10, 20, 5, 15, 8};
    for (uint32_t i = 0; i < 5; ++i) {
        TypeInfo& t = r.types[i];
        t.name = pnames[i];
        t.kind = UnitKind::Planet;
        t.credit_value = credit[i];
        t.destroyed_credit_value = 2;
        t.capture_bonus = 500;
    }
    auto unit = [&r](uint32_t id, const char* name, UnitKind k, int32_t cost, int32_t secs) -> TypeInfo& {
        TypeInfo& t = r.types[id];
        t.name = name;
        t.kind = k;
        t.build_cost = cost;
        t.build_time_seconds = secs;
        return t;
    };
    unit(tSB1, "SB1", UnitKind::StarBase, 1000, 15).base_level = 1;
    TypeInfo& sb2 = unit(tSB2, "SB2", UnitKind::StarBase, 1500, 20);
    sb2.base_level = 2;
    sb2.required_star_base = 1;
    TypeInfo& fr = unit(tFrigate, "Frigate", UnitKind::SpaceUnit, 1250, 15);
    fr.required_star_base = 1;
    fr.hyperspace = true;
    fr.hyperspace_speed = 1.5f;
    fr.piracy_value = 50.0f;
    fr.ai_combat_power = 1000.0f;
    fr.squadron_capacity = 1;
    TypeInfo& fi = unit(tFighters, "Fighters", UnitKind::Squadron, 200, 8);
    fi.required_star_base = 1;
    fi.ai_combat_power = 100.0f;
    TypeInfo& inf = unit(tInfantry, "Infantry", UnitKind::GroundUnit, 300, 10);
    inf.required_ground_base = 1;
    inf.ai_combat_power = 100.0f;
    unit(tBarracks, "Barracks", UnitKind::GroundStructure, 600, 20);
    TypeInfo& tu = unit(tTechUp, "TechUp", UnitKind::Other, 1000, 30);
    tu.required_star_base = 1;
    tu.advances_tech = 2;  // REMAINING_GAPS 2: the level the item sets (FoC tech upgrades name their target level)
    tu.tech_level = 1;  // OI galactic: a tech upgrade needs the player at exactly its Tech_Level (SG 1.3 step 7)
    TypeInfo& co = unit(tCorvette, "Corvette", UnitKind::SpaceUnit, 800, 10);
    co.required_star_base = 1;
    co.hyperspace = true;
    co.hyperspace_speed = 2.0f;
    co.ai_combat_power = 600.0f;
    co.piracy_value = 30.0f;
    return r;
}

GalacticSetup make_setup(std::vector<StartingForce> forces, bool empire_ai = false) {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "Synthetic";
    const char* names[] = {"Core", "A", "B", "C", "D", "E"};
    const Vec3 pos[] = {{500, 500, 0}, {0, 0, 0}, {60, 0, 0}, {120, 0, 0}, {60, 80, 0}, {300, 0, 0}};
    for (uint32_t i = 0; i < 6; ++i) {
        m.planets.add(names[i]);
        m.planets.position[i] = pos[i];
        m.planets.flags[i] = i == 0 ? 0u : static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    auto route = [&m](const char* n, PlanetIndex a, PlanetIndex b, float hs, float gain) {
        const uint32_t r = m.routes.add(n);
        m.routes.point_a[r] = a;
        m.routes.point_b[r] = b;
        m.routes.hs_speed_factor[r] = hs;
        m.routes.credit_gain_factor[r] = gain;
    };
    route("A_B", rA, rB, 5.0f, 0.5f);
    route("B_C", rB, rC, 6.0f, 0.5f);
    route("B_D", rB, rD, 10.0f, 0.0f);
    route("C_E", rC, rE, 7.0f, 0.0f);
    s.planet_type = {kNoIndex, tPlanetA, tPlanetB, tPlanetC, tPlanetD, tPlanetE};
    PlayerSetup e;
    e.faction = 0;
    e.human = !empire_ai;
    e.ai = empire_ai;
    e.difficulty = empire_ai ? 0u : kNoIndex;
    e.credits = 5000.0f;
    PlayerSetup u;
    u.faction = 1;
    u.ai = true;
    u.difficulty = 1;  // Hard: Credit_Multiplier 1.2, build time 0.6
    u.credits = 5000.0f;
    PlayerSetup n;
    n.faction = 2;
    s.players = {e, u, n};
    s.forces = std::move(forces);
    return s;
}

struct World {
    GalacticRules rules = make_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{0x1234u};
    GalacticSim sim{rules, store, rng};
    explicit World(std::vector<StartingForce> forces, bool empire_ai = false) {
        std::vector<std::string> diag;
        CHECK(sim.start(make_setup(std::move(forces), empire_ai), &diag));
        for (const std::string& d : diag) std::printf("  setup: %s\n", d.c_str());
        sim.reveal_all_planets();  // GRC: these scenarios test movement / economy, not the human hop gate's fog
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) sim.update();
    }
    uint32_t fleet(PlanetIndex p, int32_t player, size_t k = 0) const {
        const std::vector<uint32_t> f = sim.fleets_at(p, player);
        return k < f.size() ? f[k] : kNoFleet;
    }
    const PlanetState& planet(PlanetIndex p) const { return sim.planets()[p]; }
    const FleetState& fl(uint32_t f) const { return sim.fleets()[f]; }
};

// ---- 2a. income, corruption, caps, fiscal cycle ------------------------------------------------------------------

void test_income() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1}});
    GalacticSim& s = w.sim;
    CHECK(w.planet(rA).owner == pEmpire && w.planet(rB).owner == pEmpire && w.planet(rC).owner == pNeutral &&
          w.planet(rD).owner == pNeutral && w.planet(rE).owner == pUnder);
    CHECK(!w.planet(rCore).is_planet);
    CHECK(w.planet(rE).corruption == kCorruptionRacketeering && w.planet(rA).corruption == kCorruptionNone);
    CHECK(w.planet(rA).control == 1.0f);
    // A: base 10 + force -0 (Dark owner, Neutral planet, k 0) + trade 10 * 0.5 (A_B, B held) = 15.
    CHECK(s.planet_income(rA, false) == 15.0f);
    CHECK(w.planet(rA).income.size() == 3 && w.planet(rA).income[0].kind == kIncomeBase &&
          w.planet(rA).income[1].kind == kIncomeForce && w.planet(rA).income[2].kind == kIncomeTrade);
    CHECK(s.planet_income(rB, false) == 30.0f);  // 20 + 20 * 0.5 (B_C: C not held, B_D gain 0)
    CHECK(s.player_income(pEmpire, false) == 45.0f);
    CHECK(s.player_income(pUnder, false) == 8.0f);
    CHECK(s.player_income(pNeutral, false) == 0.0f);  // Is_Neutral faction: no income
    CHECK(w.sim.players()[pEmpire].account.cap == 80000.0f && w.sim.players()[pUnder].account.cap == 40000.0f);
    // q2f t02 [H]: the frame-0 cycle ran before the galaxy existed (start() leaves its bookkeeping: day 1, next 1350), so
    // frame 0 pays nothing; the first payment is on frame 1350. AI income x 1.2.
    CHECK(s.galactic_day() == 1 && s.last_cycle_frame() == 0 && s.next_cycle_frame() == 1350 && s.frame() == 0);
    w.run(1);
    CHECK(s.galactic_day() == 1 && s.next_cycle_frame() == 1350 && s.frame() == 1);
    CHECK(s.credits(pEmpire) == 5000.0f && s.credits(pUnder) == 5000.0f);
    w.run(1348);  // frames 1..1348
    CHECK(s.galactic_day() == 1);
    w.run(1);  // frame 1349
    CHECK(s.galactic_day() == 1 && s.credits(pEmpire) == 5000.0f);
    w.run(1);  // frame 1350: first paid cycle
    CHECK(s.galactic_day() == 2 && s.last_cycle_frame() == 1350 && s.next_cycle_frame() == 2700);
    CHECK(s.credits(pEmpire) == 5045.0f);
    CHECK(s.credits(pUnder) == 5000.0f + 8.0f * 1.2f);
    // Pause freezes the mode frame.
    s.push_pause();
    w.run(5);
    CHECK(s.frame() == 1351);
    s.pop_pause();
    // Corruption income: Underworld collects C's gross income x 4.5 (Racketeering).
    s.set_corruption(rC, kCorruptionRacketeering, pUnder);
    CHECK(s.player_income(pUnder, false) == 8.0f + 5.0f * 4.5f);
    CHECK(s.player_income(pEmpire, false) == 45.0f);  // the owner is not charged
    // Siphon: Underworld takes 25 % of B.
    s.set_siphon(rB, pUnder, 0.25f, 1 << 30);
    CHECK(s.player_income(pEmpire, false) == 15.0f + 22.5f);
    CHECK(s.player_income(pUnder, false) == 8.0f + 22.5f + 7.5f);
    s.set_siphon(rB, kNoPlayer, 0.0f, 0);
    // Piracy pool is taken once (consume) by the first benefiting player.
    s.set_corruption(rB, kCorruptionPiracy, pUnder);
    // Credit halt, destroyed planet, ability entry with expiry, clamps.
    s.set_credit_halt(rA, s.frame() + 10);
    CHECK(s.planet_income(rA, false) == 0.0f);
    w.run(11);
    CHECK(s.planet_income(rA, false) == 15.0f);
    s.set_destroyed(rA, true);
    CHECK(s.planet_income(rA, false) == 3.0f);  // Planet_Destroyed_Credit_Value 2 + trade 1
    s.set_destroyed(rA, false);
    s.add_income_entry(rA, 100.0f, s.frame() + 2);
    CHECK(s.planet_income(rA, false) == 115.0f);
    w.run(3);
    CHECK(s.planet_income(rA, false) == 15.0f);
    s.set_income_limits(rA, true, 40.0f, false, 0.0f);
    CHECK(s.planet_income(rA, false) == 40.0f);
    s.set_income_limits(rA, false, 0.0f, true, 12.0f);
    CHECK(s.planet_income(rA, false) == 12.0f);
    s.set_income_limits(rA, false, 0.0f, false, 0.0f);
    // Cap: clamped on every change.
    s.add_credits(pEmpire, 200000.0f);
    CHECK(s.credits(pEmpire) == 80000.0f);
    // Black-market reroll on the next cycle: one synced draw per qualifying planet, planet order.
    s.set_black_market_influence(rC, true);
    s.set_black_market_influence(rD, true);
    while (s.frame() < s.next_cycle_frame()) w.run(1);
    SimRng expect = w.rng;
    w.run(1);
    const float d1 = expect.range_float(1.0f, 3.0f), d2 = expect.range_float(1.0f, 3.0f);
    CHECK(w.planet(rC).black_market == d1 && w.planet(rD).black_market == d2 && w.rng == expect);
    CHECK(w.planet(rC).black_market >= 1.0f && w.planet(rC).black_market <= 3.0f);
}

// ---- 2b. production queues -------------------------------------------------------------------------------------

void test_production() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pUnder, rE, tSB1}, {pUnder, rE, tBarracks}});
    GalacticSim& s = w.sim;
    // Human, up-front payment: cost on enqueue, T = 15 * 0.9 (starbase 1 vs tech 0) = 13.5 s = 405 frames.
    CHECK(s.build_time_for(pEmpire, rA, tFrigate) == 13.5f);
    CHECK(s.enqueue(pEmpire, rA, tFrigate) == RejectReason::None);
    CHECK(s.credits(pEmpire) == 3750.0f);
    const BuildEntry head = w.planet(rA).queue[kSpaceQueue][0];
    CHECK(head.start == 0 && head.duration == 405 && head.cost == 1250.0f);
    CHECK(s.enqueue(pEmpire, rA, tSB2) == RejectReason::None);  // upgrade, queued behind
    CHECK(s.enqueue(pEmpire, rA, tFighters) == RejectReason::None);
    CHECK(s.enqueue(pEmpire, rA, tFighters) == RejectReason::None);
    CHECK(s.enqueue(pEmpire, rA, tFighters) == RejectReason::None);
    CHECK(s.enqueue(pEmpire, rA, tFighters) == RejectReason::QueueFull);  // Max_Build_Queue 5 (humans)
    CHECK(s.enqueue(pEmpire, rA, tSB1) == RejectReason::PrerequisitesUnmet);  // base already level 1
    CHECK(s.enqueue(pEmpire, rB, tFrigate) == RejectReason::NotOwner);
    CHECK(s.enqueue(pEmpire, rA, tPlanetA) == RejectReason::NotAllowed);  // cost < 1
    const float before = s.credits(pEmpire);
    CHECK(s.cancel(pEmpire, rA, kSpaceQueue, 4) == RejectReason::None);
    CHECK(s.credits(pEmpire) == before + 200.0f);  // full refund for an up-front payer
    CHECK(s.cancel(pUnder, rA, kSpaceQueue, 0) == RejectReason::NotOwner);
    // Infantry on the ground queue (needs ground level 1 = the barracks).
    CHECK(s.enqueue(pEmpire, rA, tInfantry) == RejectReason::None);
    w.run(405);  // frames 0..404
    CHECK(w.planet(rA).queue[kSpaceQueue].size() == 4 && w.sim.fleets_at(rA, pEmpire).empty());
    w.run(1);  // frame 405: done
    CHECK(w.planet(rA).queue[kSpaceQueue].size() == 3);
    const uint32_t f = w.fleet(rA, pEmpire);
    CHECK(f != kNoFleet && w.fl(f).contents.size() == 1 && w.store.type(w.fl(f).contents[0]) == tFrigate);
    CHECK(w.planet(rA).queue[kSpaceQueue][0].type == tSB2 && w.planet(rA).queue[kSpaceQueue][0].start == 405);
    // Infantry: 10 s * 0.9 (the starbase level is used for ground units too) = 9 s = 270 frames.
    CHECK(w.planet(rA).surface.size() == 1);
    // Starbase upgrade head while the starbase is not owned -> dropped without refund.
    const float c2 = s.credits(pEmpire);
    CHECK(s.destroy_object(w.planet(rA).starbase));
    CHECK(w.planet(rA).starbase_level == 0 && w.planet(rA).starbase_owner == kNoPlayer);
    w.run(1);
    CHECK(w.planet(rA).queue[kSpaceQueue].size() == 2 && w.planet(rA).queue[kSpaceQueue][0].type == tFighters);
    CHECK(s.credits(pEmpire) == c2);
    // Re-evaluation when a fleet leaves: fighters need starbase 1, the planet has 0 -> deleted, no refund.
    CHECK(s.move_fleet(pEmpire, f, rB) == RejectReason::None);
    CHECK(w.planet(rA).queue[kSpaceQueue].empty() && s.credits(pEmpire) == c2);
    // Up-front: cannot afford.
    s.add_credits(pEmpire, -1.0e9f);
    CHECK(s.enqueue(pEmpire, rA, tBarracks) == RejectReason::CannotAfford);

    // AI pays as it goes: nothing on enqueue, installments per frame; refund = paid share x 1.2 (quirk).
    const float u0 = s.credits(pUnder);
    CHECK(s.enqueue(pUnder, rE, tCorvette) == RejectReason::None);
    CHECK(s.credits(pUnder) == u0);
    const BuildEntry ce = w.planet(rE).queue[kSpaceQueue][0];
    CHECK(ce.build_time == 10.0f * 0.9f * 0.6f && ce.duration == econ::timer_frames(ce.build_time));
    w.run(60);
    const BuildEntry mid = w.planet(rE).queue[kSpaceQueue][0];
    const float paid = u0 - s.credits(pUnder);
    CHECK(near(paid, (ce.remaining - mid.remaining) / ce.build_time * ce.cost, 0.05f));
    CHECK(near(mid.remaining, ce.remaining - 59.0f / 30.0f, 1e-4f));  // the enqueue frame advances nothing
    const float before_cancel = s.credits(pUnder);
    CHECK(s.cancel(pUnder, rE, kSpaceQueue, 0) == RejectReason::None);
    const float refund = econ::refund_amount(true, mid.remaining, mid.build_time, mid.cost);
    CHECK(s.credits(pUnder) == before_cancel + refund * 1.2f);
    // PAYG stall: no credits -> the timer is re-armed every frame (paused).
    s.add_credits(pUnder, -1.0e9f);
    CHECK(s.credits(pUnder) == 0.0f);
    CHECK(s.enqueue(pUnder, rE, tCorvette) == RejectReason::None);
    w.run(400);
    CHECK(w.planet(rE).queue[kSpaceQueue].size() == 1 && w.sim.fleets_at(rE, pUnder).empty());
    const BuildEntry stalled = w.planet(rE).queue[kSpaceQueue][0];
    CHECK(stalled.start == s.frame() - 1 && stalled.remaining > 4.0f);
    // Tech upgrade sets the player's tech level to its Build_Advances_Tech_Level (REMAINING_GAPS 2).
    World t({{pEmpire, rA, tSB1}});
    CHECK(t.sim.enqueue(pEmpire, rA, tTechUp) == RejectReason::None);
    t.run(t.planet(rA).queue[kSpaceQueue][0].duration + 1);
    CHECK(t.sim.players()[pEmpire].tech_level == 2 && t.planet(rA).queue[kSpaceQueue].empty());
}

// ---- 2c. fleets ----------------------------------------------------------------------------------------------------

int32_t frames_for_hop(Vec3 from, Vec3 to, float v) {
    econ::Vec3f p{from.x, from.y, from.z};
    for (int32_t k = 1; k < 100000; ++k)
        if (econ::fleet_step(p, {to.x, to.y, to.z}, v, nullptr)) return k;
    return -1;
}

void test_fleets() {
    {
        World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tFighters}, {pEmpire, rA, tFighters}});
        GalacticSim& s = w.sim;
        const uint32_t f = w.fleet(rA, pEmpire);
        CHECK(f != kNoFleet && w.fl(f).contents.size() == 3);
        CHECK(s.move_fleet(pEmpire, f, rC) == RejectReason::None);
        // Carrier: holds 1 -> one fighter carried, the other stays behind in a new fleet at A.
        CHECK(w.fl(f).contents.size() == 2 && w.fl(f).carried[1] == 1);
        const uint32_t left = w.fleet(rA, pEmpire);
        CHECK(left != kNoFleet && left != f && w.fl(left).contents.size() == 1);
        CHECK(s.move_fleet(pEmpire, left, rB) == RejectReason::NotAllowed);  // fighters cannot jump
        CHECK((w.fl(f).path == std::vector<PlanetIndex>{rA, rB, rC}));
        const float v0 = econ::base_speed(1.5f, 0.07f, 1.0f);
        CHECK(w.fl(f).base_speed == v0 && w.fl(f).hop_speed == econ::hop_speed(v0, 5.0f));
        const int32_t k1 = frames_for_hop({0, 0, 0}, {60, 0, 0}, econ::hop_speed(v0, 5.0f));
        const int32_t k2 = frames_for_hop({60, 0, 0}, {120, 0, 0}, econ::hop_speed(v0, 6.0f));
        CHECK(k1 == 113);
        w.run(k1 - 1);
        CHECK(w.fl(f).from == rA && w.fl(f).moving);
        w.run(1);
        CHECK(w.fl(f).from == rB && w.fl(f).hop_speed == econ::hop_speed(v0, 6.0f));  // through B, no stop
        w.run(k2 - 1);
        CHECK(w.fl(f).moving);
        w.run(1);
        CHECK(!w.fl(f).moving && w.fl(f).planet == rC && w.fl(f).path.empty());
        CHECK(w.store.position(w.fl(f).object).x == 120.0f);
        CHECK(w.planet(rC).owner == pNeutral);  // orbit alone does not capture an accessible planet
    }
    {   // Merge on arrival into the friendly fleet already in orbit.
        World w({{pEmpire, rA, tFrigate}, {pEmpire, rB, tFrigate}});
        const uint32_t fa = w.fleet(rA, pEmpire), fb = w.fleet(rB, pEmpire);
        CHECK(w.sim.move_fleet(pEmpire, fa, rB) == RejectReason::None);
        w.run(200);
        CHECK(!w.fl(fa).alive && w.fl(fb).contents.size() == 2 && w.sim.fleets_at(rB, pEmpire).size() == 1);
        CHECK(!w.store.alive(w.fl(fa).object));
    }
    {   // Interception at the first contested hop; Underworld runs through.
        World w({{pEmpire, rA, tFrigate}, {pUnder, rB, tCorvette}, {pUnder, rE, tCorvette}, {pEmpire, rC, tFrigate}});
        const uint32_t fe = w.fleet(rA, pEmpire);
        CHECK(w.sim.move_fleet(pEmpire, fe, rE) == RejectReason::None);
        const uint32_t fu = w.fleet(rE, pUnder);
        CHECK(w.sim.move_fleet(pUnder, fu, rA) == RejectReason::None);
        for (int i = 0; i < 3000 && (w.fl(fe).moving || (w.fl(fu).alive && w.fl(fu).moving)); ++i) w.run(1);
        CHECK(w.fl(fe).planet == rB && !w.fl(fe).moving && w.fl(fe).path.empty());  // stopped at B
        CHECK(w.fl(fu).alive && w.fl(fu).planet == rA);  // ran through Empire-held C (frigate in orbit)
    }
    {   // Corruption hyperspace bonus on a free hop between two corrupted planets.
        World w({{pUnder, rD, tCorvette}, {pUnder, rD, tSB1}});
        w.sim.set_corruption(rC, static_cast<int8_t>(0), pUnder);  // Intimidation
        const uint32_t f = w.fleet(rD, pUnder);
        CHECK(w.planet(rD).corruption == kCorruptionRacketeering);
        CHECK(w.sim.move_fleet_path(pUnder, f, std::vector<PlanetIndex>{rD, rC}) == RejectReason::None);
        CHECK(w.fl(f).hop_speed == econ::hop_speed(econ::base_speed(2.0f, 0.07f, 1.0f), 1.5f));
    }
    {   // Piracy: an enemy fleet leaving a Piracy-corrupted planet fills the pool; Underworld takes it once.
        World w({{pEmpire, rB, tFrigate}, {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1}});
        w.sim.set_corruption(rB, kCorruptionPiracy, pUnder);
        CHECK(w.sim.move_fleet(pEmpire, w.fleet(rB, pEmpire), rA) == RejectReason::None);
        CHECK(w.planet(rB).piracy_pool == 50.0f);
        const float base = w.sim.player_income(pUnder, false);
        CHECK(base == 8.0f + 20.0f * 0.8f + 50.0f);  // E + B gross x Piracy 0.8 + pool
        CHECK(w.sim.player_income(pUnder, true) == base && w.planet(rB).piracy_pool == 0.0f);
    }
    {   // Inaccessible planet: turn back to the previous hop and stop there.
        World w({{pEmpire, rA, tFrigate}});
        w.sim.set_hidden(rC, true);
        const uint32_t f = w.fleet(rA, pEmpire);
        CHECK(w.sim.move_fleet_path(pEmpire, f, std::vector<PlanetIndex>{rA, rB, rC}) == RejectReason::None);
        for (int i = 0; i < 2000 && w.fl(f).moving; ++i) w.run(1);
        CHECK(w.fl(f).planet == rB && !w.fl(f).moving);
        CHECK(w.sim.move_fleet_path(pEmpire, f, std::vector<PlanetIndex>{rB, rE}) == RejectReason::InvalidTarget);
    }
}

// ---- 2d. ownership and capture -------------------------------------------------------------------------------------

void test_ownership() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tInfantry},
             {pUnder, rE, tSB1}, {pUnder, rD, tCorvette}, {pUnder, rD, tInfantry}});
    GalacticSim& s = w.sim;
    CHECK(w.planet(rD).owner == pUnder && w.planet(rC).owner == pNeutral);
    // Underworld loads the infantry, flies D -> C (free 100-unit hop) and lands.
    const ObjectHandle inf = w.planet(rD).surface[0];
    CHECK(s.launch_units(pUnder, rD, std::vector<ObjectHandle>{inf}) == RejectReason::None);
    const uint32_t f = w.fleet(rD, pUnder);
    CHECK(w.fl(f).contents.size() == 2);
    w.run(1);
    CHECK(w.planet(rD).owner == pUnder);  // leaning keeps D
    CHECK(s.move_fleet(pUnder, f, rC) == RejectReason::None);
    for (int i = 0; i < 3000 && w.fl(f).moving; ++i) w.run(1);
    CHECK(w.fl(f).planet == rC);
    CHECK(s.invade(pUnder, rC, {}) == RejectReason::None);
    CHECK(w.planet(rC).surface.size() == 1 && w.fl(f).contents.size() == 1);
    const float u0 = s.credits(pUnder);
    w.run(1);
    // Captured: control 0 then one ramp step, bonus 500 x 1.2 (AI), a 10 s corruption gain begun (GRC 4.3: type 3,
    // no pair rebuild, the type follows at completion), caps recomputed.
    CHECK(w.planet(rC).owner == pUnder && w.planet(rC).control == econ::political_delta(1, 1.0f, 7.0f));
    CHECK(s.credits(pUnder) == u0 + 600.0f);
    CHECK(w.planet(rC).corruption == kCorruptionNone && s.corruption_transition_active(rC) &&
          s.effective_corruption_type(rC) == kCorruptionRacketeering && !s.corruption_particle_pairs(rC).present);
    CHECK(w.planet(rC).corruption_end - w.planet(rC).corruption_start == 300u);
    CHECK(s.players()[pUnder].account.cap == 120000.0f);
    w.run(104);
    const float ctl = w.planet(rC).control;
    CHECK(ctl > 0.49f && ctl < 0.51f);
    CHECK(near(s.planet_income(rC, false), 5.0f * ctl));  // base + (c * b - b)
    w.run(110);
    CHECK(w.planet(rC).control == 1.0f && s.planet_income(rC, false) == 5.0f);
    CHECK(w.planet(rC).corruption == kCorruptionNone && s.corruption_transition_fraction(rC) > 0.7f);
    w.run(86);  // frame start + 300: completed in the planet service
    CHECK(w.planet(rC).corruption == kCorruptionRacketeering && w.planet(rC).corrupting_player == pUnder &&
          !s.corruption_transition_active(rC) && s.corruption_transition_fraction(rC) == 1.0f);
    // Units leave: the planet stays (leaning follows the capture) [L].
    CHECK(s.launch_units(pUnder, rC, std::vector<ObjectHandle>{w.planet(rC).surface[0]}) == RejectReason::None);
    w.run(1);
    CHECK(w.planet(rC).owner == pUnder);
    // Invasion is refused while hostile ground forces hold the surface (battles are tactical).
    CHECK(s.invade(pUnder, rA, {}) == RejectReason::NotAllowed || w.fleet(rA, pUnder) == kNoFleet);
    // Empire recaptures C: corruption is removed (old owner benefited, new does not), human bonus x 1.
    const uint32_t ef = w.fleet(rA, pEmpire);
    CHECK(s.launch_units(pEmpire, rA, std::vector<ObjectHandle>{w.planet(rA).surface[0]}) == RejectReason::None);
    CHECK(s.move_fleet(pEmpire, ef, rC) == RejectReason::None);
    for (int i = 0; i < 3000 && w.fl(ef).moving; ++i) w.run(1);
    CHECK(w.fl(ef).planet == rC);  // final hop: stops at C next to the Underworld fleet (conflict)
    const uint32_t uf = w.fleet(rC, pUnder);
    if (uf != kNoFleet) CHECK(s.destroy_object(w.fl(uf).object));  // battle outcome stand-in
    CHECK(s.invade(pEmpire, rC, {}) == RejectReason::None);
    const float e0 = s.credits(pEmpire);
    w.run(1);
    // GRC 4.3: a 10 s removal begins; the corruption stays until it completes.
    CHECK(w.planet(rC).owner == pEmpire && w.planet(rC).corruption == kCorruptionRacketeering &&
          w.planet(rC).corruption_removing && s.effective_corruption_type(rC) == kCorruptionNone);
    CHECK(s.credits(pEmpire) == e0 + 500.0f);
    CHECK(s.players()[pUnder].account.cap == 80000.0f);
    w.run(300);
    CHECK(w.planet(rC).corruption == kCorruptionNone && !s.corruption_transition_active(rC));
}

// ---- 2e. S4 story event stream ---------------------------------------------------------------------------------------

std::string event_digest(const std::vector<GalacticEvent>& evs) {
    std::string s;
    for (const GalacticEvent& e : evs) {
        char line[200];
        std::snprintf(line, sizeof line, "%d %s p%d/%d pl%u/%u f%u t%u o%d v%d\n", e.frame,
                      std::string(galactic_event_name(e.kind)).c_str(), e.player, e.other_player, e.planet, e.other_planet,
                      e.fleet, e.type, e.object, e.value);
        s += line;
    }
    return s;
}

// Runs the capture scenario of test_ownership plus production, tech and a destruction; returns the stream
// and the final galactic state digest.
std::pair<std::vector<GalacticEvent>, uint64_t> event_scenario(bool emit) {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rA, tFrigate}, {pUnder, rE, tSB1},
             {pUnder, rD, tCorvette}, {pUnder, rD, tInfantry}});
    GalacticSim& s = w.sim;
    s.set_emit_events(emit);
    CHECK(s.events().empty());
    std::vector<GalacticEvent> all;
    CHECK(s.launch_units(pUnder, rD, std::vector<ObjectHandle>{w.planet(rD).surface[0]}) == RejectReason::None);
    const uint32_t f = w.fleet(rD, pUnder);
    w.run(1);  // frame 0 (no fiscal cycle: the first is on frame 1350, q2f t02)
    CHECK(s.move_fleet(pUnder, f, rC) == RejectReason::None);
    for (int i = 0; i < 3000 && w.fl(f).moving; ++i) w.run(1);
    CHECK(s.invade(pUnder, rC, {}) == RejectReason::None);
    w.run(1);  // capture
    w.run(300);  // GRC 4.3: the capture's 10 s corruption gain completes (CorruptionChanged)
    CHECK(s.enqueue(pEmpire, rA, tTechUp) == RejectReason::None);
    CHECK(s.enqueue(pEmpire, rA, tInfantry) == RejectReason::None);
    CHECK(s.cancel(pEmpire, rA, kGroundQueue, 0) == RejectReason::None);
    w.run(w.planet(rA).queue[kSpaceQueue][0].duration + 1);
    s.set_corruption(rB, kCorruptionPiracy, pUnder);
    CHECK(s.destroy_object(w.fl(w.fleet(rA, pEmpire)).object));
    s.drain_events(all);
    CHECK(s.events().empty());
    StateHasher h;
    s.hash_state(h);
    return {all, h.digest()};
}

void test_event_stream() {
    const auto [evs, digest] = event_scenario(true);
    const auto [none, digest_off] = event_scenario(false);
    CHECK(none.empty());
    CHECK(digest == digest_off);  // the stream is not state: emission never changes the galactic CRC
    const auto again = event_scenario(true);
    CHECK(event_digest(again.first) == event_digest(evs));
    using K = GalacticEventKind;
    // GalacticDayPassed: one per paid fiscal cycle, on frames 1350, 2700, ... with the day after the increment (the
    // frame-0 cycle runs before the galaxy exists and emits nothing, q2f t02). The other events in order:
    std::vector<GalacticEvent> o;
    int32_t days = 0;
    for (const GalacticEvent& e : evs) {
        if (e.kind != K::GalacticDayPassed) {
            o.push_back(e);
            continue;
        }
        ++days;
        CHECK(e.frame == 1350 * days && e.value == days + 1);
    }
    std::vector<K> kinds;
    for (const GalacticEvent& e : o) kinds.push_back(e.kind);
    // D -> C runs through B (D_B, B_C routes): pass-through arrival + departure at B.
    const std::vector<K> expect = {K::FleetDeparted,      K::FleetArrived,       K::FleetDeparted,     K::FleetArrived,
                                   K::UnitsLanded,        K::PlanetOwnerChanged, K::CorruptionChanged, K::ProductionBegin,
                                   K::ProductionBegin,    K::ProductionCanceled, K::ProductionCompleted, K::TechLevelChanged,
                                   K::CorruptionChanged,  K::ObjectDestroyed,    K::ObjectDestroyed};
    CHECK(kinds == expect);
    if (kinds != expect) std::printf("%s", event_digest(evs).c_str());
    if (kinds.size() == expect.size()) {
        CHECK(o[0].planet == rD && o[0].other_planet == rB && o[0].player == pUnder);
        CHECK(o[1].planet == rB && o[1].value == kArrivePassThrough && o[1].frame > o[0].frame);
        CHECK(o[2].planet == rB && o[2].other_planet == rC && o[2].frame == o[1].frame);
        CHECK(o[3].planet == rC && o[3].value == kArriveFinal && o[3].fleet == o[0].fleet);
        CHECK(o[4].planet == rC && o[4].value == 1);
        CHECK(o[5].planet == rC && o[5].player == pUnder && o[5].other_player == pNeutral &&
              o[5].frame == o[4].frame);
        CHECK(o[6].value == kCorruptionRacketeering && o[6].player == pUnder);
        CHECK(o[7].type == tTechUp && o[7].value == kSpaceQueue && o[8].type == tInfantry);
        CHECK(o[9].type == tInfantry && o[9].other_player == 1);
        CHECK(o[10].type == tTechUp && o[10].object == 0 && o[11].value == 2 && o[11].other_player == 1);
        CHECK(o[12].planet == rB && o[12].value == kCorruptionPiracy);
        CHECK(o[13].type == tFrigate && o[13].planet == rA && o[14].type == kNoIndex);
    }
}

// ---- 2f. A2 galactic: AI hooks (income ticks, production queries, FleetManagement) ------------------------------------

void test_a2_income_hook() {
    // A2 gal t03 (A2GAL_READINGS 1.1): per AI player inside the payment loop, raw income, none for humans or I = 0.
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1}});
    GalacticSim& s = w.sim;
    std::vector<IncomeTick> ticks;
    std::vector<float> credits_at_tick;
    const uint32_t id = s.add_income_hook([&](const IncomeTick& t) {
        ticks.push_back(t);
        credits_at_tick.push_back(s.credits(t.player));
        CHECK(s.galactic_day() + 1 == t.day);  // inside the loop: the counter increments after it
    });
    CHECK(id != 0 && s.last_income(pUnder) == 0.0f && s.last_income(99) == 0.0f);
    const float under0 = s.credits(pUnder);
    w.run(1);  // frame 0: no fiscal cycle (the first is on frame 1350, q2f t02), so no tick
    CHECK(ticks.empty() && s.last_income(pUnder) == 0.0f);
    while (s.frame() < s.next_cycle_frame()) w.run(1);
    CHECK(s.credits(pUnder) == under0);
    w.run(1);  // frame 1350: first fiscal cycle
    // Empire is human (no tick), Neutral earns nothing (no tick): only Underworld.
    CHECK(ticks.size() == 1);
    if (ticks.size() == 1) {
        CHECK(ticks[0].player == pUnder && ticks[0].gain == 8.0f && ticks[0].period_seconds == 45.0f && ticks[0].day == 2 &&
              ticks[0].frame == 1350);  // raw income: Underworld's Credit_Multiplier 1.2 is not in it
        CHECK(credits_at_tick[0] == under0 + 8.0f * 1.2f);  // fired after this player's Add_Credits
    }
    CHECK(s.last_income(pEmpire) == 45.0f && s.last_income(pUnder) == 8.0f && s.last_income(pNeutral) == 0.0f);
    // The AI pays a corvette as it goes from the frame before the cycle: the cycle frame's installment is in the
    // credit change but not in the income tick.
    while (s.frame() + 1 < s.next_cycle_frame()) w.run(1);
    CHECK(s.enqueue(pUnder, rE, tCorvette) == RejectReason::None);
    w.run(1);
    const float before = s.credits(pUnder);
    w.run(1);
    CHECK(ticks.size() == 2 && ticks[1].gain == 8.0f && ticks[1].day == 3 && ticks[1].frame == 2700);
    CHECK(s.credits(pUnder) - before < 8.0f * 1.2f - 1.0f);
    s.remove_income_hook(id);
    const int32_t stop = s.next_cycle_frame();
    while (s.frame() <= stop) w.run(1);
    CHECK(ticks.size() == 2);

    // AI Empire: ticks in player order, the gain taken before the cap clamp.
    World a({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1}}, true);
    GalacticSim& t = a.sim;
    std::vector<IncomeTick> at;
    t.add_income_hook([&at](const IncomeTick& k) { at.push_back(k); });
    t.add_credits(pEmpire, 79990.0f - t.credits(pEmpire));
    a.run(1351);  // frames 0..1350: the first fiscal cycle (q2f t02)
    CHECK(at.size() == 2 && t.credits(pEmpire) == 80000.0f);
    if (at.size() == 2) CHECK(at[0].player == pEmpire && at[0].gain == 45.0f && at[1].player == pUnder);
    // An AI player with no income this cycle (Underworld owns nothing here): no Add_Credits and no tick.
    World z({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}}, true);
    std::vector<IncomeTick> zt;
    z.sim.add_income_hook([&zt](const IncomeTick& k) { zt.push_back(k); });
    CHECK(z.sim.player_income(pUnder, false) == 0.0f);
    z.run(1351);
    CHECK(zt.size() == 1 && !zt.empty() && zt[0].player == pEmpire && z.sim.last_income(pUnder) == 0.0f);
}

void test_a2_production_queries() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pUnder, rE, tSB1}});
    GalacticSim& s = w.sim;
    CHECK(s.queue_kind(tFrigate) == kSpaceQueue && s.queue_kind(tInfantry) == kGroundQueue && s.queue_kind(tBarracks) == kGroundQueue);
    CHECK(s.queue_kind(tSB1) == kSpaceQueue && s.queue_kind(tPlanetA) == -1 && s.queue_kind(9999) == -1);
    // Every buildable type at A for the Empire (type order); the base is level 1 already, so SB1 is not.
    const std::vector<uint32_t> at_a = s.buildable(rA, pEmpire);
    CHECK((at_a == std::vector<uint32_t>{tSB2, tFrigate, tFighters, tInfantry, tBarracks, tTechUp, tCorvette}));
    CHECK(s.buildable(rB, pEmpire).empty());  // not the owner
    CHECK(!s.buildable(rE, pUnder, tInfantry) && s.buildable(rE, pUnder, tBarracks));  // no ground base at E
    // can_enqueue == enqueue's answer, no side effects; queue_of shows the queue.
    s.add_credits(pEmpire, 10000.0f);
    for (uint32_t t = 0; t < tTypeCount; ++t) {
        const RejectReason pred = s.can_enqueue(pEmpire, rA, t);
        const float c0 = s.credits(pEmpire);
        const size_t n0 = s.queue_of(rA, kSpaceQueue).size() + s.queue_of(rA, kGroundQueue).size();
        const RejectReason got = s.enqueue(pEmpire, rA, t);
        CHECK(pred == got);
        const size_t n1 = s.queue_of(rA, kSpaceQueue).size() + s.queue_of(rA, kGroundQueue).size();
        CHECK((got == RejectReason::None) == (n1 == n0 + 1));
        if (got != RejectReason::None) CHECK(s.credits(pEmpire) == c0);
    }
    CHECK(s.queue_of(rA, kSpaceQueue).size() == 5 && s.queue_of(rA, kSpaceQueue)[0].type == tSB2);
    CHECK(s.can_enqueue(pEmpire, rA, tFighters) == RejectReason::QueueFull && s.buildable(rA, pEmpire, tFighters));
    CHECK(s.queue_of(rA, 2).empty() && s.queue_of(rCore, kSpaceQueue).empty() && s.queue_of(99, kSpaceQueue).empty());
    s.add_credits(pEmpire, -1.0e9f);
    CHECK(s.can_enqueue(pEmpire, rA, tBarracks) == RejectReason::CannotAfford && s.buildable(rA, pEmpire, tBarracks));
    CHECK(s.can_enqueue(7, rA, tBarracks) == RejectReason::InvalidPlayer && s.can_enqueue(pEmpire, rCore, tBarracks) == RejectReason::InvalidTarget);
}

void test_a2_spawn_convert() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pNeutral, rC, tCorvette}, {pNeutral, rC, tInfantry}});
    GalacticSim& s = w.sim;
    const float c0 = s.credits(pEmpire);
    const ObjectHandle fr = s.spawn_unit(pEmpire, rA, tFrigate);
    CHECK(w.store.alive(fr) && w.store.owner(fr) == pEmpire && w.store.type(fr) == tFrigate);
    const uint32_t f = w.fleet(rA, pEmpire);
    CHECK(f != kNoFleet && s.fleet_containing(fr) == f && s.credits(pEmpire) == c0);
    const ObjectHandle inf = s.spawn_unit(pEmpire, rA, tInfantry);
    CHECK(w.planet(rA).surface.size() == 1 && w.planet(rA).surface[0] == inf);
    const ObjectHandle sb = s.spawn_unit(pEmpire, rA, tSB2);
    CHECK(w.planet(rA).starbase == sb && w.planet(rA).starbase_level == 2);
    CHECK(!s.spawn_unit(pEmpire, rA, tTechUp) && !s.spawn_unit(pEmpire, rCore, tFrigate) && !s.spawn_unit(9, rA, tFrigate));
    CHECK(w.planet(rA).queue[kSpaceQueue].empty() && w.planet(rA).queue[kGroundQueue].empty());
    // Stealing: the player's own frigate elsewhere is killed silently, a new one appears at the destination.
    s.set_emit_events(true);
    const ObjectHandle fb = s.spawn_unit(pEmpire, rB, tFrigate);
    CHECK(s.steal_unit(pEmpire, tFrigate, rA, false));  // the one at A is skipped, B's is taken
    CHECK(!w.store.alive(fb) && w.store.alive(fr) && w.fleet(rB, pEmpire) == kNoFleet);
    CHECK(!s.steal_unit(pEmpire, tFrigate, rA, false) && s.steal_unit(pEmpire, tFrigate, rA, true));  // hero rule
    CHECK(!w.store.alive(fr));
    const ObjectHandle m1 = s.spawn_unit(pEmpire, rB, tCorvette);
    const ObjectHandle m2 = s.magic_spawn(pEmpire, rA, tCorvette, true, false);
    CHECK(!w.store.alive(m1) && w.store.alive(m2) && s.fleet_containing(m2) == w.fleet(rA, pEmpire));
    const ObjectHandle m3 = s.magic_spawn(pEmpire, rA, tCorvette, false, false);  // no stealing: both stay
    CHECK(w.store.alive(m2) && w.store.alive(m3));
    CHECK(s.events().empty());  // no story events from magic spawns or silent kills
    CHECK(!s.magic_spawn(pEmpire, rCore, tCorvette, false, false));
    // Blockade: a hostile combat fleet in orbit.
    CHECK(!s.blockaded(rA, pEmpire));
    s.spawn_unit(pUnder, rA, tCorvette);
    CHECK(s.blockaded(rA, pEmpire) && s.blockaded(rA, pUnder) && !s.blockaded(rB, pEmpire) && !s.blockaded(rCore, pEmpire));
}

// ---- A2 gal t04 / t05 / t06 (docs/re/A2GAL_READINGS.md §2-§4) -------------------------------------------------------

void test_a2gal_population() {
    // §2: the starbase counts through its planet (once, its Population_Value is not "used"); the non-human skip is
    // "planet not revealed"; objects in transit are never skipped.
    GalacticRules rules = make_rules();
    rules.constants.ai_uses_galactic_pop_cap = true;
    rules.types[tPlanetA].population_capacity = 4;
    rules.types[tPlanetE].population_capacity = 5;
    rules.types[tSB1].population_capacity = 10;
    rules.types[tSB1].population = 7;
    rules.types[tBarracks].population_capacity = 1;
    rules.types[tFrigate].population = 3;
    rules.factions[1].base_population_capacity = 2;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{1u};
    GalacticSim s{rules, store, rng};
    CHECK(s.start(make_setup({{pEmpire, rA, tSB1}, {pUnder, rE, tSB1}, {pUnder, rE, tBarracks}, {pUnder, rE, tFrigate}})));
    CHECK(s.planets()[rE].owner == pUnder && store.alive(s.planets()[rE].starbase));
    s.set_planet_auto_reveal(false);  // the reveal state decides (auto-reveal makes every planet revealed)
    s.set_planet_revealed(rE, false);
    CHECK(s.population_skips(pUnder, rE) && !s.population_skips(pEmpire, rE) && !s.population_skips(pUnder, kInvalidPlanet));
    CHECK(s.population_capacity(pUnder) == 2 && s.population_used(pUnder, false) == 0);  // everything at E skipped
    s.set_planet_revealed(rE, true);
    // 2 base + planet E 5 + starbase type 10 (once) + barracks 1; used = frigate 3 (the starbase's 7 is not "used").
    CHECK(s.population_capacity(pUnder) == 18 && s.population_used(pUnder, false) == 3);
    // Human Empire: never skips; planet A 4 + its starbase 10.
    s.set_planet_revealed(rA, false);
    CHECK(s.population_capacity(pEmpire) == 14 && s.population_used(pEmpire, false) == 0);
    // The frigate's fleet in transit has no planet: counted even while E is not revealed.
    s.set_planet_revealed(rE, false);
    const std::vector<uint32_t> fl = s.fleets_at(rE, pUnder);
    CHECK(fl.size() == 1);
    if (fl.size() == 1) {
        const PlanetIndex hop[] = {rC};
        const RejectReason mv = s.move_fleet_path(pUnder, fl[0], hop);
        if (mv != RejectReason::None) std::printf("  a2gal pop: move refused %d\n", static_cast<int>(mv));
        CHECK(mv == RejectReason::None);
        s.update();
        CHECK(s.fleets()[fl[0]].moving && s.fleets()[fl[0]].planet == kInvalidPlanet);
        CHECK(s.population_used(pUnder, false) == 3 && s.population_capacity(pUnder) == 2);
    }
    // refix t05: starbase_type() (0x3f8b40) is the planet's starbase object's type, kNoIndex without one; a starbase
    // removed behind the sim's back (a direct store destroy) adds nothing and is cleared at the next update (level 0,
    // owner none, StarbaseLevelChanged), like destroy_object.
    CHECK(s.starbase_type(rA) == tSB1 && s.starbase_type(rC) == kNoIndex && s.starbase_type(kInvalidPlanet) == kNoIndex);
    CHECK(s.population_capacity(pEmpire) == 14 && s.planets()[rA].starbase_level == 1);
    s.set_emit_events(true);
    CHECK(store.destroy(s.planets()[rA].starbase));
    CHECK(s.starbase_type(rA) == kNoIndex && s.population_capacity(pEmpire) == 4);
    CHECK(s.planets()[rA].starbase_level == 1 && s.planets()[rA].starbase_owner == pEmpire);  // not seen yet
    s.update();
    CHECK(s.planets()[rA].starbase_level == 0 && s.planets()[rA].starbase_owner == kNoPlayer && !s.planets()[rA].starbase);
    std::vector<GalacticEvent> evs;
    s.drain_events(evs);
    const auto cleared = std::count_if(evs.begin(), evs.end(), [](const GalacticEvent& e) {
        return e.kind == GalacticEventKind::StarbaseLevelChanged && e.planet == rA && e.player == pEmpire && e.value == 0;
    });
    CHECK(cleared == 1);
    // Planet E's starbase is untouched; a second update clears nothing more.
    CHECK(s.starbase_type(rE) == tSB1 && s.planets()[rE].starbase_level == 1);
    s.update();
    evs.clear();
    s.drain_events(evs);
    CHECK(std::none_of(evs.begin(), evs.end(), [](const GalacticEvent& e) { return e.kind == GalacticEventKind::StarbaseLevelChanged; }));
}

void test_a2gal_steal_order() {
    // §3: the owner's list newest first; a capture moves the object to the newest end; units in transit are stealable.
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}});
    GalacticSim& s = w.sim;
    const ObjectHandle f1 = s.spawn_unit(pEmpire, rB, tFrigate);
    const ObjectHandle f2 = s.spawn_unit(pEmpire, rC, tFrigate);
    CHECK(s.steal_unit(pEmpire, tFrigate, rA, false));
    CHECK(w.store.alive(f1) && !w.store.alive(f2));  // the newest goes first
    const ObjectHandle u = s.spawn_unit(pUnder, rD, tFrigate);
    const ObjectHandle f3 = s.spawn_unit(pEmpire, rB, tFrigate);
    CHECK(w.store.set_owner(u, pEmpire));  // captured: now the Empire's newest, although created before f3
    CHECK(s.steal_unit(pEmpire, tFrigate, rA, false) && !w.store.alive(u) && w.store.alive(f3) && w.store.alive(f1));
    CHECK(s.steal_unit(pEmpire, tFrigate, rA, false) && !w.store.alive(f3) && w.store.alive(f1));
    // In transit toward the destination: stealable; the newer one at the destination is not.
    const uint32_t moving = s.fleet_containing(f1);
    CHECK(moving != kNoFleet && s.move_fleet(pEmpire, moving, rA) == RejectReason::None);
    w.run(1);
    CHECK(w.fl(moving).moving);
    const ObjectHandle at_dest = s.spawn_unit(pEmpire, rA, tFrigate);
    CHECK(s.steal_unit(pEmpire, tFrigate, rA, false) && !w.store.alive(f1) && w.store.alive(at_dest));
    CHECK(!s.steal_unit(pEmpire, tFrigate, rA, false) && s.steal_unit(pEmpire, tFrigate, rA, true));  // hero rule
    // refix t04: the match is GalacticSim::representative_type (= the store type in the port model; a fleet object's
    // own type, never a member's type).
    const ObjectHandle rep = s.spawn_unit(pEmpire, rB, tFrigate);
    CHECK(s.representative_type(rep) == static_cast<uint32_t>(tFrigate));
    const uint32_t rf = s.fleet_containing(rep);
    CHECK(rf != kNoFleet && s.representative_type(w.fl(rf).object) == static_cast<uint32_t>(w.store.type(w.fl(rf).object)));
    CHECK(s.representative_type(ObjectHandle{}) == kNoIndex);
}

void test_a2gal_ai_blockade() {
    // §4: the first visible orbit fleet decides, relative to the planet owner; no combat filter, neutrals blockade.
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1}});
    GalacticSim& s = w.sim;
    CHECK(!s.ai_blockaded(rA) && !s.ai_blockaded(rCore) && !s.ai_blockaded(kInvalidPlanet));
    const ObjectHandle own = s.spawn_unit(pEmpire, rA, tFrigate);
    const ObjectHandle foe = s.spawn_unit(pUnder, rA, tCorvette);
    CHECK(w.planet(rA).orbit.size() == 2);
    CHECK(!s.ai_blockaded(rA) && s.blockaded(rA, pEmpire));  // the owner's own fleet is listed first
    CHECK(s.destroy_object(own));
    CHECK(s.ai_blockaded(rA));
    s.set_galactic_stealth(foe, true);  // a fleet whose contents are all hidden is skipped
    CHECK(!s.ai_blockaded(rA));
    s.set_galactic_stealth(foe, false);
    s.set_relation(pEmpire, pUnder, Relation::Ally);  // friendly to the planet owner
    CHECK(!s.ai_blockaded(rA));
    s.set_relation(pEmpire, pUnder, Relation::Enemy);
    s.set_relation(pUnder, pEmpire, Relation::Enemy);
    CHECK(s.ai_blockaded(rA));
    // Relative to the planet owner: Underworld's fleet at its own planet E does not blockade E.
    s.spawn_unit(pUnder, rE, tCorvette);
    CHECK(!s.ai_blockaded(rE) && s.blockaded(rE, pEmpire));
    // A neutral, unarmed fleet blockades (the HUD / starbase test ignores it).
    w.rules.types[tFighters].ai_combat_power = 0.0f;
    s.spawn_unit(pNeutral, rB, tFighters);
    CHECK(s.ai_blockaded(rB) && !s.blockaded(rB, pEmpire));
}

void test_a2_population_terrain() {
    GalacticRules rules = make_rules();
    rules.types[tPlanetA].population_capacity = 4;
    rules.types[tPlanetB].population_capacity = 3;
    rules.types[tSB1].population_capacity = 2;
    rules.types[tFrigate].population = 3;
    rules.types[tCorvette].population = 2;
    rules.types[tInfantry].population = 1;
    rules.factions[0].base_population_capacity = 1;
    rules.types[tPlanetB].terrain = 2;  // Desert
    rules.types[tPlanetB].restricted_categories = 0x4;
    rules.types[tPlanetB].restricted_types = {tInfantry};
    rules.types[tCorvette].categories = 0x4 | 0x1;
    rules.types[tFrigate].categories = 0x1;
    rules.types[tInfantry].movement_class = 0;
    rules.types[tFrigate].movement_class = 3;
    rules.terrain_effectiveness.value = {{}, {}, {0.5, 1.0, 1.0, 2.0}};
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{1u};
    GalacticSim s{rules, store, rng};
    CHECK(s.start(make_setup({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pEmpire, rA, tFrigate},
                              {pUnder, rE, tSB1}, {pUnder, rE, tBarracks}})));
    s.add_credits(pEmpire, 20000.0f);
    // Human Empire: 1 base + A 4 + B 3 + SB1 2 = 10; used = frigate 3.
    CHECK(s.population_capped(pEmpire) && !s.population_capped(pUnder));
    CHECK(s.population_capacity(pEmpire) == 10 && s.population_used(pEmpire) == 3 && s.population_room(pEmpire) == 7);
    CHECK(s.enqueue(pEmpire, rA, tFrigate) == RejectReason::None);  // queued pop counts: room 4
    CHECK(s.population_used(pEmpire, false) == 3 && s.population_room(pEmpire) == 4);
    CHECK(s.enqueue(pEmpire, rA, tFrigate) == RejectReason::None);   // room 1
    CHECK(s.can_enqueue(pEmpire, rA, tCorvette) == kRejectPopulationCap && s.buildable(rA, pEmpire, tCorvette));
    CHECK(s.enqueue(pEmpire, rA, tCorvette) == kRejectPopulationCap);
    CHECK(s.enqueue(pEmpire, rA, tInfantry) == RejectReason::None);  // pop 1 fits exactly
    CHECK(s.enqueue(pEmpire, rA, tBarracks) == RejectReason::None);  // pop 0 always fits
    // The AI has no galactic cap in shipped data (AI_Uses_Galactic_Pop_Cap false).
    for (int i = 0; i < 6; ++i) CHECK(s.enqueue(pUnder, rE, tCorvette) == RejectReason::None);
    // Terrain.
    CHECK(s.planet_terrain(rB) == 2 && s.planet_terrain(rA) == -1 && s.planet_terrain(rCore) == -1);
    CHECK(s.planet_restricts(rB, tCorvette) && s.planet_restricts(rB, tInfantry) && !s.planet_restricts(rB, tFrigate));
    CHECK(!s.planet_restricts(rA, tCorvette));
    CHECK(s.terrain_effectiveness(rB, tInfantry) == 0.5 && s.terrain_effectiveness(rB, tFrigate) == 2.0);
    CHECK(s.terrain_effectiveness(rA, tInfantry) == 1.0 && s.terrain_effectiveness(rB, tCorvette) == 1.0);  // no class
    CHECK(terrain_id("desert") == 2 && terrain_id("Space") == 7 && terrain_id("Moon") == -1);
}

void test_a2_victory() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1}, {pUnder, rE, tBarracks}});
    GalacticSim& s = w.sim;
    CHECK(parse_victory_condition("galactic_cycles_elapsed") == VictoryCondition::CyclesElapsed &&
          parse_victory_condition("x") == VictoryCondition::Unknown);
    VictoryRules r;
    r.human.conditions = {VictoryCondition::SuperWeaponDestruction, VictoryCondition::ControlNamedPlanets};
    r.human.planets = {rA, rB, rC};
    r.ai.conditions = {VictoryCondition::AllPlanetsControlled, VictoryCondition::CyclesElapsed};
    r.ai.cycle_limit = 3;
    CHECK(!check_victory(s, r).finished() && victory_outcome(s, r, pEmpire) == 0);
    // Empire takes C (named planets A, B, C).
    s.change_owner(w.planet(rC).object, pEmpire);
    VictoryResult v = check_victory(s, r);
    CHECK(v.winner == pEmpire && v.how == VictoryCondition::ControlNamedPlanets && victory_outcome(s, r, pEmpire) == 1 &&
          victory_outcome(s, r, pUnder) == -1);
    // The AI wins on the cycle limit (the human's named planets lost again).
    s.change_owner(w.planet(rC).object, pNeutral);
    while (s.galactic_day() <= 3) w.run(1);  // Cycles_Elapsed: cycles > limit (SG 1.4)
    v = check_victory(s, r);
    CHECK(v.winner == pUnder && v.how == VictoryCondition::CyclesElapsed);
    // Elimination: Underworld loses E and everything on it.
    r.ai.conditions.clear();
    const std::vector<ObjectHandle> kill(w.planet(rE).structures.begin(), w.planet(rE).structures.end());
    for (ObjectHandle h : kill) s.destroy_object(h);
    s.destroy_object(w.planet(rE).starbase);
    s.change_owner(w.planet(rE).object, pNeutral);  // the leaning would keep it otherwise
    w.run(1);
    CHECK(player_eliminated(s, pUnder) && !player_eliminated(s, pEmpire));
    v = check_victory(s, r);
    CHECK(!v.finished() && v.eliminated == std::vector<int32_t>{pUnder} && victory_outcome(s, r, pUnder) == -1);
    // All planets: every planet's owner the Empire or allied with it (SG 1.4: neutral-owned planets block).
    r.human.conditions = {VictoryCondition::AllPlanetsControlled};
    CHECK(check_victory(s, r).winner == kNoPlayer);
    for (PlanetIndex p : {rC, rD, rE}) s.change_owner(w.planet(p).object, pEmpire);
    CHECK(check_victory(s, r).winner == pEmpire);
}

void test_a2_prerequisites_and_market_hook() {
    GalacticRules rules = make_rules();
    rules.types[tFrigate].required_special_structures = {tBarracks};
    rules.types[tPlanetD].surface_accessible = false;
    rules.types[tSB2].base_level = 3;  // an upgrade two levels up
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{1u};
    GalacticSim s{rules, store, rng};
    CHECK(s.start(make_setup({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tSB1}, {pEmpire, rD, tSB1}})));
    s.add_credits(pEmpire, 20000.0f);
    // Required_Special_Structures: the frigate needs a barracks on the planet.
    CHECK(s.buildable(rA, pEmpire, tFrigate) && !s.buildable(rB, pEmpire, tFrigate) && s.buildable(rB, pEmpire, tCorvette));
    CHECK(s.can_enqueue(pEmpire, rB, tFrigate) == RejectReason::PrerequisitesUnmet);
    // Surface: no ground structures / units on an inaccessible planet.
    CHECK(!s.buildable(rD, pEmpire, tBarracks) && s.buildable(rB, pEmpire, tBarracks) && s.buildable(rD, pEmpire, tCorvette));
    CHECK(s.enqueue(pEmpire, rD, tBarracks) == RejectReason::PrerequisitesUnmet);
    // Starbases: only the next level.
    CHECK(!s.buildable(rA, pEmpire, tSB2) && s.enqueue(pEmpire, rA, tSB2) == RejectReason::PrerequisitesUnmet);
    // Production-service hooks: per planet, newest registered first (reverse row order), primary slot first.
    std::vector<std::pair<int, PlanetIndex>> calls;
    s.set_production_service_hook([&calls](PlanetIndex p) { calls.push_back({0, p}); });
    const uint32_t h1 = s.add_production_service_hook([&calls](PlanetIndex p) { calls.push_back({1, p}); });
    s.add_production_service_hook([&calls](PlanetIndex p) { calls.push_back({2, p}); });
    s.update();
    const std::vector<PlanetIndex> rows = {rE, rD, rC, rB, rA};
    CHECK(calls.size() == 15);
    for (size_t i = 0; i < calls.size() && i < 15; ++i)
        CHECK(calls[i].first == static_cast<int>(i % 3) && calls[i].second == rows[i / 3]);
    calls.clear();
    s.remove_production_service_hook(h1);
    s.set_production_service_hook(nullptr);
    s.update();
    CHECK(calls.size() == 5 && calls[0].first == 2 && calls[4].second == rA);
}

// Relation table (Make_Ally / Make_Enemy) behind every hostility test, and the build locks behind buildability.
void test_diplomacy_and_build_locks() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pUnder, rE, tSB1}, {pUnder, rE, tCorvette}});
    GalacticSim& s = w.sim;
    auto digest = [&s] {
        StateHasher h;
        s.hash_state(h);
        return h.digest();
    };
    const auto fresh = digest();
    // Defaults reproduce the former rule: enemies unless a side is Is_Neutral.
    CHECK(s.relation(pEmpire, pUnder) == Relation::Enemy && s.relation(pUnder, pEmpire) == Relation::Enemy);
    CHECK(s.relation(pEmpire, pNeutral) == Relation::Neutral && s.relation(pEmpire, pEmpire) == Relation::Self);
    CHECK(s.relation(-1, pEmpire) == Relation::Neutral && s.relation(pEmpire, 7) == Relation::Neutral);
    CHECK(s.is_hostile(pEmpire, pUnder) && !s.is_hostile(pEmpire, pNeutral) && !s.is_hostile(pEmpire, pEmpire));
    CHECK(s.allied(pEmpire, pEmpire) && !s.allied(pEmpire, pUnder) && !s.allied(pEmpire, kNoPlayer));
    CHECK(s.diplomacy_state_default() && s.blockaded(rE, pEmpire));
    CHECK(!s.player_view(pEmpire).held_by_ally.test(rE) && s.player_view(pEmpire).held_by_ally.test(rA));
    VictoryRules vr;
    vr.human.conditions = {VictoryCondition::AllPlanetsControlled};
    CHECK(check_victory(s, vr).winner == kNoPlayer);  // Underworld still holds E
    // Make_Ally: both directions; no blockade, E counts as held by an ally, no hostile planet left.
    s.set_relation(pEmpire, pUnder, Relation::Ally);
    CHECK(s.relation(pUnder, pEmpire) == Relation::Ally && !s.is_hostile(pUnder, pEmpire) && s.allied(pEmpire, pUnder));
    CHECK(s.relation(pEmpire, pNeutral) == Relation::Neutral);  // untouched pairs keep their defaults
    CHECK(!s.blockaded(rE, pEmpire) && s.player_view(pEmpire).held_by_ally.test(rE));
    CHECK(check_victory(s, vr).winner == kNoPlayer);  // C and D are neutral-owned (SG 1.4: owner self or ally)
    s.set_relation(pEmpire, pNeutral, Relation::Ally);
    CHECK(check_victory(s, vr).winner == pEmpire);
    s.set_relation(pEmpire, pNeutral, Relation::Neutral);
    CHECK(!s.diplomacy_state_default() && digest() != fresh);
    // Ignored: self, Self relation, invalid players.
    s.set_relation(pEmpire, pEmpire, Relation::Enemy);
    s.set_relation(pEmpire, pNeutral, Relation::Self);
    s.set_relation(pEmpire, 9, Relation::Enemy);
    CHECK(s.relation(pEmpire, pEmpire) == Relation::Self && s.relation(pEmpire, pNeutral) == Relation::Neutral);
    // Make_Enemy restores hostility (the table stays hashed); reset_relations drops the tail.
    s.set_relation(pUnder, pEmpire, Relation::Enemy);
    CHECK(s.is_hostile(pEmpire, pUnder) && s.blockaded(rE, pEmpire) && digest() != fresh);
    s.set_relation(pEmpire, pNeutral, Relation::Enemy);  // a neutral faction made an enemy explicitly
    CHECK(s.is_hostile(pNeutral, pEmpire));
    s.reset_relations();
    CHECK(s.diplomacy_state_default() && digest() == fresh && !s.is_hostile(pNeutral, pEmpire));

    // Build locks. Lock_Tech list: refused by buildable / enqueue until unlocked.
    CHECK(s.buildable(rA, pEmpire, tFrigate));
    s.lock_type(pEmpire, tFrigate, true);
    CHECK(s.build_locked(pEmpire, tFrigate) && !s.buildable(rA, pEmpire, tFrigate) && !s.build_locked(pUnder, tFrigate));
    CHECK(s.enqueue(pEmpire, rA, tFrigate) == RejectReason::PrerequisitesUnmet);
    s.lock_type(pEmpire, tFrigate, false);
    CHECK(!s.build_locked(pEmpire, tFrigate) && s.buildable(rA, pEmpire, tFrigate) && s.diplomacy_state_default());
    // Build_Initially_Locked: only with initial build locks on (the default); Unlock_Tech (unlock_tech) adds it to the
    // unlocked list.
    CHECK(s.initial_build_locks());
    w.rules.types[tCorvette].build_initially_locked = true;
    s.set_initial_build_locks(false);
    CHECK(s.buildable(rA, pEmpire, tCorvette));
    s.set_initial_build_locks(true);
    CHECK(s.build_locked(pEmpire, tCorvette) && !s.buildable(rA, pEmpire, tCorvette) && s.build_locked(pUnder, tCorvette));
    CHECK(s.diplomacy_state_default() && digest() == fresh);  // a switch, not state
    s.lock_type(pEmpire, tCorvette, false);  // only leaves the locked list (REMAINING_GAPS 1.5): still initially locked
    CHECK(!s.type_unlocked(pEmpire, tCorvette) && s.build_locked(pEmpire, tCorvette) && digest() == fresh);
    s.unlock_tech(pEmpire, tCorvette);
    CHECK(s.type_unlocked(pEmpire, tCorvette) && !s.build_locked(pEmpire, tCorvette) && s.buildable(rA, pEmpire, tCorvette));
    CHECK(s.build_locked(pUnder, tCorvette) && !s.diplomacy_state_default() && digest() != fresh);
    s.lock_type(pEmpire, tCorvette, true);  // the Lock_Tech list wins over the unlocked list
    CHECK(s.build_locked(pEmpire, tCorvette));
    s.lock_type(pEmpire, tCorvette, false);
    // The tech gate is a plain >= (SG 1.3 step 16, reason 1): unlocked types do not skip it.
    w.rules.types[tCorvette].tech_level = 3;
    w.rules.types[tFrigate].tech_level = 3;
    CHECK(!s.buildable(rA, pEmpire, tCorvette) && !s.buildable(rA, pEmpire, tFrigate));
    CHECK(s.build_check(pEmpire, rA, tCorvette).reason == kBuildTechLevel && s.build_check(pEmpire, rA, tCorvette).detail == 3u);
    s.unlock_type(pEmpire, tCorvette);  // no duplicates
    CHECK(s.players()[pEmpire].unlocked_types.size() == 1);
    CHECK(s.enqueue(pEmpire, rA, tCorvette) == RejectReason::PrerequisitesUnmet);
    s.set_tech_level(pEmpire, 3);
    CHECK(s.enqueue(pEmpire, rA, tCorvette) == RejectReason::None);
}

// ---- OI galactic: docs/re/STRATEGIC_GAPS.md (SG) ----------------------------------------------------------------------

enum OiType : uint32_t {
    tShuttle = tTypeCount, tCompany, tSmuggler, tOrbitCompany, tDeathStar, tKiller, tStructA, tStructB, tStructC, tGated,
    tLimited, tMember, tDetector, tOiCount
};

GalacticRules make_oi_rules(float m = 1.0f) {
    GalacticRules r = make_rules();
    r.constants.fleet_speed_multiplier = m;
    r.types.resize(tOiCount);
    r.special.types.resize(tOiCount);
    auto unit = [&r](uint32_t id, const char* name, UnitKind k, int32_t cost) -> TypeInfo& {
        TypeInfo& t = r.types[id];
        t.name = name;
        t.kind = k;
        t.build_cost = cost;
        t.build_time_seconds = 10;
        return t;
    };
    TypeInfo& sh = unit(tShuttle, "Shuttle", UnitKind::Other, 0);  // a Company_Transport_Unit (never queued)
    sh.hyperspace = true;
    sh.hyperspace_speed = 1.5f;
    for (uint32_t c : {static_cast<uint32_t>(tCompany), static_cast<uint32_t>(tSmuggler), static_cast<uint32_t>(tOrbitCompany)}) {
        TypeInfo& t = unit(c, c == tCompany ? "Company" : c == tSmuggler ? "Smuggler_Team" : "Orbit_Company", UnitKind::GroundUnit, 300);
        t.dummy = kDummyGroundCompany;
        t.required_ground_base = 1;
        t.company_transport = tShuttle;
        t.company_units = {tInfantry};
        t.ai_combat_power = 100.0f;
    }
    r.types[tOrbitCompany].always_spawn_in_orbit = true;
    r.special.types[tSmuggler].galactic_stealth = true;  // Galactic_Stealth_Ability, Initially_Enabled
    r.special.types[tSmuggler].generic_hero = true;
    TypeInfo& ds = unit(tDeathStar, "Death_Star", UnitKind::SpaceUnit, 0);
    ds.planet_destruction = true;
    ds.hyperspace = true;
    ds.hyperspace_speed = 0.5f;
    ds.ai_combat_power = 5000.0f;
    TypeInfo& k = unit(tKiller, "Rogue_Squadron", UnitKind::SpaceUnit, 0);
    k.super_weapon_killer = true;
    k.hyperspace = true;
    k.hyperspace_speed = 2.0f;
    unit(tStructA, "StructA", UnitKind::GroundStructure, 100);
    unit(tStructB, "StructB", UnitKind::GroundStructure, 100);
    unit(tStructC, "StructC", UnitKind::GroundStructure, 100);
    TypeInfo& g = unit(tGated, "Gated", UnitKind::SpaceUnit, 500);
    g.required_star_base = 1;
    g.required_special_groups = {{tStructA, tStructB}, {tStructC}};  // `StructA | StructB, StructC`
    TypeInfo& lim = unit(tLimited, "Limited", UnitKind::SpaceUnit, 400);
    lim.required_star_base = 1;
    lim.limit_lifetime = 1;
    lim.hyperspace = true;
    lim.hyperspace_speed = 1.0f;
    TypeInfo& mem = unit(tMember, "Member", UnitKind::SpaceUnit, 400);
    mem.required_star_base = 1;
    mem.squadron_member = true;
    unit(tDetector, "Detector", UnitKind::GroundUnit, 0);
    r.special.types[tDetector].named_hero = true;
    return r;
}

struct OiWorld {
    GalacticRules rules;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{0x5151u};
    GalacticSim sim;
    explicit OiWorld(std::vector<StartingForce> forces, float m = 1.0f) : rules(make_oi_rules(m)), sim(rules, store, rng) {
        CHECK(sim.start(make_setup(std::move(forces))));
        sim.reveal_all_planets();  // GRC: not a fog test
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) sim.update();
    }
    const PlanetState& planet(PlanetIndex p) const { return sim.planets()[p]; }
    const FleetState& fl(uint32_t f) const { return sim.fleets()[f]; }
    uint32_t id(ObjectHandle h) const { return static_cast<uint32_t>(store.id(h)); }
    uint32_t pid(PlanetIndex p) const { return id(planet(p).object); }
};

bool store_alive_type(const GalacticSim& s, uint32_t type) {
    for (ObjectHandle h : s.store().all_objects())
        if (s.store().alive(h) && static_cast<uint32_t>(s.store().type(h)) == type) return true;
    return false;
}

// SG 2.1: M = Object_Max_Speed_Multiplier_Galactic (FoC 5.0) at release game speed in the original (G1 2026-10-29: the retail
// reference trace moves unrouted fleets 0.525 units/tick and HS 5 route hops 2.625 units/tick = 1.5 x 0.07 x M x R with M = 5,
// tools/probe_mod/g1_hops/analyze_hops.py). The port applies the constant only when strategic.cpp's kApplyModeSpeedMultiplier is
// true (false since the 2026-10-28 playtest: M = 1.0); these checks hold for either setting, and the retail numbers are locked
// by test_retail_hops / test_foc_corellia_kuat_hop.
void test_oi_speed_multiplier() {
    std::printf("  oi: speed_multiplier\n");
    {   // Rules with the 5.0 constant.
        OiWorld w({{pEmpire, rA, tFrigate}}, 5.0f);
        CHECK(w.rules.constants.fleet_speed_multiplier == 5.0f);
        const float m = w.sim.fleet_speed_multiplier();  // 5.0 once the port applies the original's value, else 1.0
        CHECK(m == 5.0f || m == 1.0f);
        const uint32_t f = w.sim.fleets_at(rA, pEmpire)[0];
        CHECK(w.sim.move_fleet(pEmpire, f, rB) == RejectReason::None);
        const float v0 = econ::base_speed(1.5f, 0.07f, m);
        CHECK(w.fl(f).base_speed == v0 && w.fl(f).hop_speed == econ::hop_speed(v0, 5.0f));
        // 60 units at v0 x route 5: 21 frames with M = 5, 113 with M = 1 (the GE 3.2 example).
        const int32_t expect = frames_for_hop({0, 0, 0}, {60, 0, 0}, econ::hop_speed(v0, 5.0f));
        CHECK(expect == (m == 5.0f ? 21 : 113));
        CHECK(frames_for_hop({0, 0, 0}, {60, 0, 0}, econ::hop_speed(econ::base_speed(1.5f, 0.07f, 5.0f), 5.0f)) == 21);
        int32_t k = 0;
        while (w.fl(f).moving && k < 1000) {
            w.run(1);
            ++k;
        }
        CHECK(k == expect && w.fl(f).planet == rB);
        if (m != 5.0f)
            std::printf("  NOTE: M = %.1f applied (the original: 5.0); 60-unit route hop %d ticks (original speed: 21)\n",
                        static_cast<double>(m), k);
    }
    {   // Rules without the tag keep the mode default 1.0 either way.
        OiWorld w({{pEmpire, rA, tFrigate}}, 1.0f);
        CHECK(w.sim.fleet_speed_multiplier() == 1.0f);
        const uint32_t f = w.sim.fleets_at(rA, pEmpire)[0];
        CHECK(w.sim.move_fleet(pEmpire, f, rB) == RejectReason::None);
        int32_t k = 0;
        while (w.fl(f).moving && k < 1000) {
            w.run(1);
            ++k;
        }
        CHECK(k == 113 && w.fl(f).planet == rB);
    }
}

// Retail hops (G1 hop timing, 2026-10-29): fleets of the original game (FoC 1.121, Sandbox_Heart_Of_The_Maw_Empire, hands-off,
// reference trace tools/probe_mod/q2_traces/out/20261027-101057-galactic-vanilla/trace.q2t, read with
// tools/probe_mod/g1_hops/analyze_hops.py). `frames` = ticks from the departure sample (fleet on the origin planet centre) to
// the arrival sample (snap to the orbit slot); step = units per tick. All fleets here are Empire/Rebel with
// Hyperspace_Speed 1.5 and no corruption bonus (the trace's owner 0); v = 1.5 x 0.07 x M x R with M = 5 and R = the route's
// HS_Speed_Factor (1 without a route). This locks econ::base_speed / hop_speed / fleet_step to the original, tick by tick.
struct RetailHop {
    const char* what;
    Vec3 from, to;
    float route;    // HS_Speed_Factor of the route, 1 = none
    float step;     // observed units per tick (float32 as printed by the trace, 4 decimals)
    int32_t frames; // observed
};
const RetailHop kRetailHops[] = {
    {"Corellia -> Kuat (route 5)", {30, -110, 10}, {90, -70, 10}, 5.0f, 2.6249f, 26},
    {"Tatooine -> Corellia (route 6)", {-5, -190, 10}, {30, -110, 10}, 6.0f, 3.1499f, 26},
    {"Kuat -> Kashyyyk (route 5)", {90, -70, 10}, {140, -130, 10}, 5.0f, 2.6249f, 28},
    {"Kashyyyk -> NalHutta (route 6)", {140, -130, 10}, {140, -200, 10}, 6.0f, 3.1499f, 21},
    {"Naboo -> Tatooine (no route)", {-50, -230, 10}, {-5, -190, 10}, 1.0f, 0.5250f, 113},
    {"NalHutta -> Shola (no route)", {140, -200, 10}, {200, -260, 10}, 1.0f, 0.5250f, 160},
    {"Yavin -> MonCalimari (route 5)", {240, 100, 10}, {300, -75, 10}, 5.0f, 2.6250f, 69},
};

void test_retail_hops() {
    std::printf("  retail hop reference (G1)\n");
    const float v0 = econ::base_speed(1.5f, 0.07f, 5.0f);
    CHECK(std::fabs(v0 - 0.525f) < 1e-6f);
    for (const RetailHop& h : kRetailHops) {
        const float v = econ::hop_speed(v0, h.route);
        const int32_t k = frames_for_hop(h.from, h.to, v);
        if (std::fabs(v - h.step) > 5e-4f || k != h.frames)
            std::printf("FAIL retail hop %s: v %.5f (retail %.4f), %d frames (retail %d)\n", h.what, static_cast<double>(v),
                        static_cast<double>(h.step), k, h.frames);
        CHECK(std::fabs(v - h.step) <= 5e-4f);
        CHECK(k == h.frames);
    }
    // M = 1 would be 5x slower: Corellia -> Kuat in 136 frames (4.5 s) instead of the retail 26 (0.87 s at 30 ticks/s).
    CHECK(frames_for_hop({30, -110, 10}, {90, -70, 10}, econ::hop_speed(econ::base_speed(1.5f, 0.07f, 1.0f), 5.0f)) == 136);
}

// ---- GALACTIC_REVEAL_CORRUPTION (GRC) ------------------------------------------------------------------------------------

struct GrcWorld {
    GalacticRules rules = make_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{0x2468u};
    GalacticSim sim{rules, store, rng};
    explicit GrcWorld(bool auto_reveal) {
        rules.constants.particle_brightness_per_level = {0.3f, 1.0f};  // FoC
        GalacticSetup setup = make_setup({{pEmpire, rA, tSB1}, {pEmpire, rA, tFrigate}, {pUnder, rE, tSB1}});
        setup.players[pEmpire].home = rA;
        setup.planet_auto_reveal = auto_reveal;
        CHECK(sim.start(setup));
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) sim.update();
    }
    const PlanetState& planet(PlanetIndex p) const { return sim.planets()[p]; }
};

// GRC 1: home reveal, the pre-step (owned source, auto-reveal spread with strict <, orbit delay) and the human hop gate
// on the raw byte.
void test_grc_reveal() {
    std::printf("  grc: reveal\n");
    {  // Auto-reveal (campaign default): A owned -> A and its route / in-range neighbours.
        GrcWorld w(true);
        GalacticSim& s = w.sim;
        CHECK(s.local_player() == pEmpire && s.planet_auto_reveal());
        CHECK(s.planet_revealed_raw(rA) && !s.planet_revealed_raw(rB));  // start: the home planet only
        const uint32_t f = s.fleets_at(rA, pEmpire)[0];
        CHECK(s.move_fleet(pEmpire, f, rB) == RejectReason::InvalidTarget);  // before frame 0's pre-step
        w.run(1);
        CHECK(s.planet_revealed_raw(rB));   // route neighbour (and 60 < 100)
        CHECK(!s.planet_revealed_raw(rD));  // |AD| = 100: not < MaximumFleetMovementDistance 100; B is not a source
        CHECK(!s.planet_revealed_raw(rC) && !s.planet_revealed_raw(rE));
        CHECK(s.is_planet_revealed(rC) && s.player_view(pEmpire).is_planet_revealed(rC));  // auto-reveal short-circuit
        CHECK(!s.consume_reveal_sfx());  // frame 0: no SFX
        // Hop gate reads the raw byte: C (unrevealed) refused, B (revealed) accepted.
        CHECK(s.move_fleet(pEmpire, f, rC) == RejectReason::InvalidTarget);
        CHECK(s.move_fleet(pEmpire, f, rB) == RejectReason::None);
        int k = 0;
        while (s.fleets()[f].moving && k < 1000) {
            w.run(1);
            ++k;
        }
        CHECK(s.fleets()[f].planet == rB);
        // Fleet at B: after the delay B becomes a source and the spread reveals C and D (route neighbours).
        w.run(20);
        CHECK(s.planet_revealed_raw(rC) && s.planet_revealed_raw(rD) && s.consume_reveal_sfx() && !s.consume_reveal_sfx());
        CHECK(s.move_fleet(pEmpire, f, rC) == RejectReason::None);
    }
    {  // No auto-reveal: no spread; the orbit delay reveals on the (floor(30 x 0.5) + 2) = 17th qualifying frame.
        GrcWorld w(false);
        GalacticSim& s = w.sim;
        w.run(1);
        CHECK(s.planet_revealed_raw(rA) && !s.planet_revealed_raw(rB) && !s.is_planet_revealed(rB));
        const uint32_t f = s.fleets_at(rA, pEmpire)[0];
        s.set_planet_revealed(rB, true);  // story REVEAL_PLANET (P2 = 1)
        CHECK(s.move_fleet(pEmpire, f, rB) == RejectReason::None);
        s.set_planet_revealed(rB, false);  // REVEAL_PLANET P2 = 0 while in transit
        while (s.fleets()[f].moving) w.run(1);  // arrives inside this frame's service, after its pre-step
        CHECK(s.fleets()[f].planet == rB && w.planet(rB).reveal_delay == 0);
        w.run(16);
        CHECK(!s.planet_revealed_raw(rB) && w.planet(rB).reveal_delay == 16);
        w.run(1);
        CHECK(s.planet_revealed_raw(rB) && !s.planet_revealed_raw(rC));  // no spread without auto-reveal
        // Story rewards / ENABLE_GALACTIC_REVEAL.
        s.reveal_planets_of_type(tPlanetE, true);
        CHECK(s.planet_revealed_raw(rE) && !s.planet_revealed_raw(rC));
        s.set_planet_auto_reveal(true);
        CHECK(s.is_planet_revealed(rC) && !s.planet_revealed_raw(rC));
        s.reveal_all_planets();
        CHECK(s.planet_revealed_raw(rC) && s.planet_revealed_raw(rD));
        s.set_hidden(rC, true);
        CHECK(!s.is_planet_revealed(rC) && s.planet_revealed_raw(rC));  // hidden -> false (0x3fa420)
    }
}

// GRC 4: corruption transitions (begin / complete / cancel, fraction, pairs).
void test_grc_corruption_transition() {
    std::printf("  grc: corruption_transition\n");
    GrcWorld w(true);
    GalacticSim& s = w.sim;
    w.run(5);  // f0 > 1: now = 0 means "the mode frame"
    s.set_corruption(rB, kCorruptionRacketeering, pUnder);
    GalacticSim::CorruptionParticlePairs pp = s.corruption_particle_pairs(rB);
    CHECK(pp.present && pp.brightness_start == 1.0f && pp.brightness_end == 1.0f && pp.scale_start == 1.0f);
    CHECK(!s.begin_corruption_gain(rB, 0, pUnder, 5.0f));  // already corrupted
    const uint32_t f0 = static_cast<uint32_t>(s.frame());
    CHECK(s.begin_corruption_removal(rB, 2.0f));
    const PlanetState& p = w.planet(rB);
    CHECK(p.corruption_removing && p.corruption_start == f0 && p.corruption_end == f0 + 60u);
    CHECK(s.effective_corruption_type(rB) == kCorruptionNone && s.corruption_transition_active(rB));
    pp = s.corruption_particle_pairs(rB);
    CHECK(pp.present && pp.brightness_start == 1.0f && pp.brightness_end == 0.3f);  // f(current 3), f(effective -1)
    CHECK(s.corruption_transition_fraction(rB, f0 + 30u) == 0.5f && s.corruption_transition_fraction(rB, f0 + 90u) == 1.0f);
    CHECK(s.corruption_transition_fraction(rB, f0 - 1u) == 1.0f);  // now < start wraps (unsigned) -> 1.0
    CHECK(s.corruption_transition_fraction(rB) == 0.0f);           // now = the mode frame = start
    CHECK(!s.begin_corruption_removal(rB, 1.0f) && !s.begin_corruption_gain(rB, 0, pUnder, 1.0f));  // one at a time
    w.run(60);  // frames f0 .. f0 + 59
    CHECK(p.corruption == kCorruptionRacketeering && p.corruption_removing);
    w.run(1);  // frame f0 + 60 >= end: completed in the planet service
    CHECK(p.corruption == kCorruptionNone && p.corrupting_player == kNoPlayer && !s.corruption_transition_active(rB));
    CHECK(p.corruption_changed == f0 + 60u && p.corruption_start == 0u && p.corruption_end == 0u);
    CHECK(!s.corruption_particle_pairs(rB).present && s.corruption_transition_fraction(rB) == 1.0f);
    // Gain, 0 s: end = start (fraction 1 at once), completes in the same frame's service.
    CHECK(s.begin_corruption_gain(rB, kCorruptionPiracy, pUnder, 0.0f));
    pp = s.corruption_particle_pairs(rB);
    CHECK(pp.present && pp.brightness_start == 0.3f && pp.brightness_end == 1.0f && s.corruption_transition_fraction(rB) == 1.0f);
    w.run(1);
    CHECK(p.corruption == kCorruptionPiracy && p.corrupting_player == pUnder && !s.corruption_transition_active(rB));
    // Removal 0 s -> 0.1 s = (int)(30 x 0.1f) = 3 frames; Cancel keeps the type.
    CHECK(s.begin_corruption_removal(rB, 0.0f) && p.corruption_end - p.corruption_start == 3u);
    s.cancel_corruption_transition(rB);
    CHECK(p.corruption == kCorruptionPiracy && !s.corruption_transition_active(rB) && p.corruption_end == 0u);
    // Durations truncate: 0.05 s -> (int)1.5 = 1 frame.
    CHECK(s.begin_corruption_removal(rB, 0.05f) && p.corruption_end - p.corruption_start == 1u);
}

// SG 1.7: companies as transports.
void test_oi_company_transports() {
    std::printf("  oi: company_transports\n");
    std::vector<StartingForce> forces = {{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}};
    for (int i = 0; i < 12; ++i) forces.push_back({pEmpire, rA, tCompany});
    OiWorld w(forces);
    GalacticSim& s = w.sim;
    // Max_Ground_Forces_On_Planet 10: ten land, the other two get a new Galactic_Fleet each.
    CHECK(w.planet(rA).surface.size() == 10 && s.surface_forces(rA) == 10);
    std::vector<uint32_t> fl = s.fleets_at(rA, pEmpire);
    CHECK(fl.size() == 2 && w.fl(fl[0]).contents.size() == 1 && w.fl(fl[1]).contents.size() == 1);
    const ObjectHandle c = w.fl(fl[0]).contents[0];
    CHECK(s.is_company_transport(c) && s.travel_type(c) == tShuttle && static_cast<uint32_t>(w.store.type(c)) == tCompany);
    // The transport's own Hyperspace_Speed (1.5): the fleet jumps (a company without a transport could not).
    CHECK(s.move_fleet(pEmpire, fl[0], rB) == RejectReason::None && w.fl(fl[0]).base_speed == econ::base_speed(1.5f, 0.07f, 1.0f));
    // Land (op 5) is refused while the surface is full; with room it lands.
    const auto fm = [&s](FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit, uint32_t a = commands::kNoObject) {
        return s.execute(static_cast<uint8_t>(pEmpire), fleet_management_event(op, target, fleet, unit, a));
    };
    const uint32_t none = commands::kNoObject;
    const ObjectHandle c2 = w.fl(fl[1]).contents[0];
    CHECK(fm(kFleetLand, w.pid(rA), w.id(w.fl(fl[1]).object), w.id(c2)) == RejectReason::NotAllowed);
    CHECK(fm(kFleetJoin, none, w.pid(rA), w.id(c2)) == RejectReason::NotAllowed);  // op 1 onto a planet = land
    const ObjectHandle out = w.planet(rA).surface[9];
    CHECK(fm(kFleetLaunch, none, none, w.id(out)) == RejectReason::None && s.surface_forces(rA) == 9);
    CHECK(fm(kFleetLand, w.pid(rA), w.id(w.fl(fl[1]).object), w.id(c2)) == RejectReason::None && s.surface_forces(rA) == 10);
    // Op 1 into a fleet (given by a member), op 6 = a new fleet moving toward `a`.
    const uint32_t of = s.fleets_at(rA, pEmpire)[0];  // the orbit fleet `out` was launched into
    const ObjectHandle s0 = w.planet(rA).surface[0];
    CHECK(fm(kFleetJoin, none, w.id(out), w.id(s0)) == RejectReason::None && w.fl(of).contents.size() == 2);
    CHECK(fm(kFleetMoveNew, w.pid(rA), none, w.id(s0), w.pid(rB)) == RejectReason::None);
    const uint32_t mover = s.fleet_containing(s0);
    CHECK(mover != kNoFleet && mover != of && w.fl(mover).moving && w.fl(mover).to == rB);
    CHECK(fm(kFleetSlot, none, w.id(w.fl(of).object), none) == RejectReason::None);
    // Always_Spawn_In_Orbit: in orbit unless stealthed-or-quiet fails (a hostile visible fleet in orbit -> surface).
    const size_t before = s.fleets_at(rA, pEmpire).size();
    CHECK(s.spawn_unit(pEmpire, rB, tOrbitCompany));
    CHECK(w.planet(rB).surface.empty() && s.fleets_at(rB, pEmpire).size() >= 1);
    (void)before;
    OiWorld h({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pUnder, rA, tCorvette}});
    CHECK(h.sim.spawn_unit(pEmpire, rA, tOrbitCompany) && h.planet(rA).surface.size() == 1);
}

// SG 2.2: galactic stealth.
void test_oi_stealth() {
    std::printf("  oi: stealth\n");
    OiWorld w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}});
    GalacticSim& s = w.sim;
    StateHasher h0;
    s.hash_state(h0);
    // A stealthed company alone on neutral C does not take the planet (owner derivation skips hidden units).
    const ObjectHandle sm = s.spawn_unit(pUnder, rC, tSmuggler);
    CHECK(s.galactic_stealth(sm) && s.hidden(sm));
    w.run(1);
    CHECK(w.planet(rC).owner == pNeutral);
    const ObjectHandle co = s.spawn_unit(pUnder, rD, tCompany);
    CHECK(!s.hidden(co));
    w.run(1);
    CHECK(w.planet(rD).owner == pUnder);
    s.set_galactic_stealth(sm, false);
    w.run(1);
    CHECK(w.planet(rC).owner == pUnder);
    s.set_galactic_stealth(sm, true);
    // A fleet is hidden only when every member is.
    const ObjectHandle orbiting = s.spawn_unit(pUnder, rB, tOrbitCompany);
    const uint32_t f = s.fleet_containing(orbiting);
    CHECK(f != kNoFleet && !s.hidden(w.fl(f).object));
    s.set_galactic_stealth(orbiting, true);
    CHECK(s.hidden(w.fl(f).object));
    // Detection: a named detector finds a generic candidate without a draw; otherwise one synced draw each.
    const ObjectHandle det = s.spawn_unit(pEmpire, rC, tDetector);
    CHECK(det);
    SimRng before = w.rng;
    CHECK(s.detect_stealthed(det, 0.0f) == 1 && !s.galactic_stealth(sm) && w.rng == before);
    s.set_galactic_stealth(sm, true);
    w.rules.special.types[tSmuggler].generic_hero = false;
    before = w.rng;
    CHECK(s.detect_stealthed(det, 0.0f) == 0 && !(w.rng == before) && s.galactic_stealth(sm));
    SimRng expect = before;
    expect.range_float(0.0f, 1.0f);
    CHECK(w.rng == expect);  // exactly one draw
    CHECK(s.detect_stealthed(det, 1.5f) == 1);  // draw < chance
    StateHasher h1;
    s.hash_state(h1);
    CHECK(h0.digest() != h1.digest());
}

// REMAINING_GAPS 6.3: base info display queries (corruption, Planet_*_Stealth + allied owner, planet-visibility bit).
void test_oi_base_info() {
    std::printf("  oi: base_info\n");
    OiWorld w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}});
    GalacticSim& s = w.sim;
    StateHasher h0;
    s.hash_state(h0);
    const SimRng rng0 = w.rng;
    CHECK(!s.can_see_starbase_info(-1, rA) && !s.can_see_groundbase_info(pUnder, rCore) && !s.can_see_starbase_info(7, rA));
    // Step 3: the GRC 2 visibility mask (0x40 starbase, 0x100 ground base). The owner (local) has Planet_Owner 0x9FFF;
    // the AI Underworld only its presence default 0x11 here.
    CHECK(s.visibility_mask(rA, pEmpire) == kVisPlanetOwner && s.visibility_mask(rA, pUnder) == kVisGoodDefault);
    CHECK(s.can_see_starbase_info(pEmpire, rA) && !s.can_see_starbase_info(pUnder, rA) && !s.can_see_groundbase_info(pUnder, rA));
    VisibilityKey vk;
    vk.object = 4242;
    CHECK(s.add_visibility_modifier(rA, pUnder, vk, GalacticSim::kPlanetVisStarbase, -1.0f));
    CHECK(s.can_see_starbase_info(pUnder, rA) && !s.can_see_groundbase_info(pUnder, rA));
    // Step 2: the stealth flag with an owned base decides by alliance alone (the visibility bit is not read).
    CHECK(s.add_visibility_modifier(rA, pUnder, vk, kVisMaximum, -1.0f) && s.add_visibility_modifier(rB, pUnder, vk, kVisMaximum, -1.0f));
    w.rules.types[tPlanetA].starbase_stealth = true;
    CHECK(s.can_see_starbase_info(pEmpire, rA) && !s.can_see_starbase_info(pUnder, rA) && s.can_see_groundbase_info(pUnder, rA));
    w.rules.types[tPlanetA].groundbase_stealth = true;
    CHECK(s.can_see_groundbase_info(pEmpire, rA) && !s.can_see_groundbase_info(pUnder, rA));
    s.set_relation(pEmpire, pUnder, Relation::Ally);
    CHECK(s.can_see_starbase_info(pUnder, rA) && s.can_see_groundbase_info(pUnder, rA));
    s.set_relation(pEmpire, pUnder, Relation::Enemy);
    // The flag without a base owner falls through to step 3.
    w.rules.types[tPlanetB].starbase_stealth = w.rules.types[tPlanetB].groundbase_stealth = true;
    CHECK(s.can_see_starbase_info(pUnder, rB) && s.can_see_groundbase_info(pUnder, rB));
    // Step 1: a corrupted planet is visible to a Benefits_From_Corruption faction whatever the flags.
    s.set_corruption(rA, kCorruptionRacketeering, pUnder);
    CHECK(s.can_see_starbase_info(pUnder, rA) && s.can_see_groundbase_info(pUnder, rA));
    s.set_corruption(rA, kCorruptionNone, kNoPlayer);
    CHECK(!s.can_see_starbase_info(pUnder, rA));
    // Side-effect free: no RNG draw (only the relation / corruption / modifier calls above moved the hashed state).
    CHECK(w.rng == rng0);
    s.reset_relations();
    CHECK(s.remove_visibility_modifier(rA, pUnder, vk) && s.remove_visibility_modifier(rB, pUnder, vk));
    StateHasher h1;
    s.hash_state(h1);
    CHECK(h0.digest() == h1.digest());
}

// SG 1.3: buildability order and reason codes.
void test_oi_buildability() {
    std::printf("  oi: buildability\n");
    OiWorld w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rB, tBarracks}, {pUnder, rB, tCorvette}});
    GalacticSim& s = w.sim;
    s.add_credits(pEmpire, 50000.0f);
    // OR chains: `StructA | StructB, StructC` = (A or B) and C, on this planet; detail = the failing chain's first type.
    BuildCheck b = s.build_check(pEmpire, rA, tGated);
    CHECK(!b.ok && b.step == 14 && b.reason == kBuildRequiredStructure && b.detail == tStructA);
    CHECK(s.spawn_unit(pEmpire, rA, tStructB));
    b = s.build_check(pEmpire, rA, tGated);
    CHECK(!b.ok && b.reason == kBuildRequiredStructure && b.detail == tStructC);
    CHECK(s.spawn_unit(pEmpire, rA, tStructC));
    CHECK(s.build_check(pEmpire, rA, tGated).ok && s.buildable(rA, pEmpire, tGated));
    // Squadron members are not buildable (step 11); restricted planet (reason 8); Required_Planets (reason 5).
    CHECK(s.build_check(pEmpire, rA, tMember).step == 11);
    w.rules.types[tPlanetA].restricted_types = {tFrigate};
    CHECK(s.build_check(pEmpire, rA, tFrigate).reason == kBuildPlanetRestricted);
    w.rules.types[tPlanetA].restricted_types.clear();
    w.rules.types[tFrigate].required_planets = {tPlanetB};
    CHECK(s.build_check(pEmpire, rA, tFrigate).reason == kBuildRequiredPlanet);
    w.rules.types[tFrigate].required_planets.clear();
    // Initially locked: reason 7 with the initial locks on.
    w.rules.types[tFrigate].build_initially_locked = true;
    s.set_initial_build_locks(true);
    CHECK(s.build_check(pEmpire, rA, tFrigate).reason == kBuildInitiallyLocked);
    s.set_initial_build_locks(false);
    w.rules.types[tFrigate].build_initially_locked = false;
    // Starbase: the next level within Max_Space_Base; a hostile visible fleet in orbit blocks a new base (reason 10).
    CHECK(s.build_check(pEmpire, rA, tSB2).ok);
    w.rules.types[tPlanetA].max_space_base = 1;
    CHECK(s.build_check(pEmpire, rA, tSB2).step == 20 && !s.buildable(rA, pEmpire, tSB2));
    w.rules.types[tPlanetA].max_space_base = 5;
    CHECK(s.build_check(pEmpire, rB, tSB1).reason == kBuildHostileFleet);
    // Ground structure slots (Special_Structures_Land): present + queued.
    w.rules.types[tPlanetA].ground_structure_slots = 4;  // barracks, StructB, StructC present
    CHECK(s.build_check(pEmpire, rA, tStructA).ok && s.enqueue(pEmpire, rA, tStructA) == RejectReason::None);
    CHECK(s.build_check(pEmpire, rA, tStructA).step == 18);
    // Build_Max_Instances_Per_Planet: one StructB here already.
    w.rules.types[tStructB].max_instances_per_planet = 1;
    w.rules.types[tPlanetA].ground_structure_slots = -1;
    CHECK(s.build_check(pEmpire, rA, tStructB).reason == kBuildLimit);
    // Lifetime limit 1: buildable until one was completed; current limit 0 = never.
    CHECK(s.build_check(pEmpire, rA, tLimited).ok && s.enqueue(pEmpire, rA, tLimited) == RejectReason::None);
    for (int i = 0; i < 2000 && s.built_count(pEmpire, tLimited) == 0; ++i) w.run(1);
    CHECK(s.built_count(pEmpire, tLimited) == 1 && s.build_check(pEmpire, rA, tLimited).step == 22);
    w.rules.types[tFrigate].limit_current = 0;
    CHECK(s.build_check(pEmpire, rA, tFrigate).step == 23);
    w.rules.types[tFrigate].limit_current = 1;
    CHECK(s.build_check(pEmpire, rA, tFrigate).ok);
    CHECK(s.spawn_unit(pEmpire, rA, tFrigate) && s.live_count(pEmpire, tFrigate) == 1);
    CHECK(s.build_check(pEmpire, rA, tFrigate).reason == kBuildLimit);
    w.rules.types[tFrigate].limit_current = -1;
    // Data rules enforce Affiliation (an empty list = nobody); credits step 6 for humans when asked.
    w.rules.types[tFrigate].data_rules = true;
    CHECK(s.build_check(pEmpire, rA, tFrigate).step == 2);
    w.rules.types[tFrigate].affiliation = {0u};
    CHECK(s.build_check(pEmpire, rA, tFrigate).ok);
    s.add_credits(pEmpire, -1.0e9f);
    CHECK(s.build_check(pEmpire, rA, tFrigate, true).step == 6 && s.build_check(pEmpire, rA, tFrigate, false).ok);
    // Max tech: a tech upgrade needs max tech >= its advance.
    s.set_max_tech_level(pEmpire, 0);
    CHECK(s.build_check(pEmpire, rA, tTechUp).step == 7);
    s.set_max_tech_level(pEmpire, 5);
    CHECK(s.build_check(pEmpire, rA, tTechUp).step != 7);
}

// SG 1.4: polled tests and the event-driven super-weapon claims.
void test_oi_victory() {
    std::printf("  oi: victory\n");
    OiWorld w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rC, tDeathStar}, {pUnder, rE, tSB1},
               {pUnder, rE, tBarracks}, {pUnder, rB, tKiller}});
    GalacticSim& s = w.sim;
    VictorySide side;
    // Credits, percentage.
    side.credit_target = 6000;
    CHECK(!victory_condition_met(s, side, VictoryCondition::CreditsAccrued, pEmpire));
    s.add_credits(pEmpire, 1000.0f);
    CHECK(victory_condition_met(s, side, VictoryCondition::CreditsAccrued, pEmpire));
    side.galactic_control = 0.4f;  // A and C (the Death Star's leaning) of 5 planets
    CHECK(victory_condition_met(s, side, VictoryCondition::PercentageControl, pEmpire));
    side.galactic_control = 0.45f;
    CHECK(!victory_condition_met(s, side, VictoryCondition::PercentageControl, pEmpire));
    CHECK(!victory_condition_met(s, side, VictoryCondition::OpponentControlsNoPlanets, pEmpire));
    side.faction_names = {"EMPIRE"};  // only these factions count
    CHECK(victory_condition_met(s, side, VictoryCondition::OpponentControlsNoPlanets, pEmpire));
    CHECK(!victory_condition_met(s, side, VictoryCondition::CreditsAccrued, pNeutral));  // not playable
    // 3: the battle winner owns a super-weapon killer -> the weapon dies, the claim wins after the countdown (5).
    CHECK(store_alive_type(s, tDeathStar));
    CHECK(s.resolve_battle_super_weapon(rC, pUnder) && !store_alive_type(s, tDeathStar));
    CHECK(s.victory_claims().size() == 1 && s.victory_claims()[0].type == 3 && s.victory_claims()[0].countdown == 5);
    VictoryRules vr;
    vr.ai.conditions = {VictoryCondition::SuperWeaponDestruction};
    CHECK(check_victory(s, vr).winner == kNoPlayer);
    w.run(5);
    CHECK(check_victory(s, vr).winner == pUnder && check_victory(s, vr).how == VictoryCondition::SuperWeaponDestruction);
    // 6: no killer, the weapon owner's playable enemies hold no undestroyed planet.
    OiWorld w6({{pEmpire, rA, tSB1}, {pEmpire, rC, tDeathStar}, {pUnder, rE, tSB1}});
    CHECK(!w6.sim.resolve_battle_super_weapon(rC, pEmpire) && w6.sim.victory_claims().empty());
    w6.sim.set_destroyed(rE, true);
    CHECK(!w6.sim.resolve_battle_super_weapon(rC, pEmpire));
    CHECK(w6.sim.victory_claims().size() == 1 && w6.sim.victory_claims()[0].type == 6 && w6.sim.victory_claims()[0].player == pEmpire);
    // 7: planet destruction with the victim faction's leader on it; 8: no enemy leader on any planet.
    // TSW_PLANET_DESTROY §6: victim = the planet's owner; the leader must be a team MEMBER (Company_Units) of a surface
    // object - here Faction_Leader = the infantry unit inside the Company team.
    GalacticRules r7 = make_oi_rules();
    r7.factions[1].leader = tInfantry;
    r7.factions[1].leader_company = tCompany;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{9u};
    GalacticSim s7{r7, store, rng};
    CHECK(s7.start(make_setup({{pEmpire, rA, tSB1}, {pUnder, rE, tSB1}, {pUnder, rE, tBarracks}, {pUnder, rD, tCompany}})));
    VictorySide none;
    CHECK(s7.planets()[rD].owner == pUnder && s7.super_weapon_leader_hit(rD) && !s7.super_weapon_leader_hit(rE));
    CHECK(!victory_condition_met(s7, none, VictoryCondition::KillEnemyLeader, pEmpire));
    CHECK(s7.super_weapon_destroy_planet(rD, pEmpire) && s7.planets()[rD].destroyed);
    CHECK(s7.victory_claims().size() == 1 && s7.victory_claims()[0].type == 7 && s7.victory_claims()[0].countdown == 30);
    CHECK(!s7.super_weapon_destroy_planet(rD, pEmpire) && s7.victory_claims().size() == 1);  // already destroyed
    {  // the leader type itself on the surface (not as a team member) does not count; nor does a leader in orbit
        GalacticRules r = make_oi_rules();
        r.factions[1].leader = tCompany;
        objects::GameObjectTypes ty;
        objects::IdCounter id2;
        objects::ObjectStore st{objects::ModeKind::Galactic, objects::StoreKind::Primary, ty, &id2};
        SimRng rg{9u};
        GalacticSim sx{r, st, rg};
        CHECK(sx.start(make_setup({{pEmpire, rA, tSB1}, {pUnder, rE, tSB1}, {pUnder, rD, tCompany}})));
        CHECK(!sx.super_weapon_leader_hit(rD));
        CHECK(sx.super_weapon_destroy_planet(rD, pEmpire) && sx.victory_claims().empty());
    }
    {  // refix t06 [port]: the leader as a bare ground unit on the surface (its own one-unit transport) counts for the TSW
        GalacticRules r = make_oi_rules();
        r.factions[1].leader = tInfantry;
        objects::GameObjectTypes ty;
        objects::IdCounter id2;
        objects::ObjectStore st{objects::ModeKind::Galactic, objects::StoreKind::Primary, ty, &id2};
        SimRng rg{9u};
        GalacticSim sx{r, st, rg};
        CHECK(sx.start(make_setup({{pEmpire, rA, tSB1}, {pUnder, rE, tSB1}, {pUnder, rD, tBarracks}})));
        CHECK(sx.planets()[rD].owner == pUnder && !sx.super_weapon_leader_hit(rD));
        const ObjectHandle lead = sx.spawn_unit(pUnder, rD, tInfantry);
        CHECK(static_cast<bool>(lead) && std::find(sx.planets()[rD].surface.begin(), sx.planets()[rD].surface.end(), lead) !=
                                             sx.planets()[rD].surface.end());
        CHECK(sx.super_weapon_leader_hit(rD) && !sx.super_weapon_leader_hit(rE));
        bool hit = false;
        CHECK(sx.tsw_destroy_planet(rD, pEmpire, &hit) && hit && sx.planets()[rD].destroyed);
        CHECK(sx.victory_claims().size() == 1 && sx.victory_claims()[0].type == 7 && sx.victory_claims()[0].player == pEmpire &&
              sx.victory_claims()[0].countdown == 30);
    }
    CHECK(victory_condition_met(s7, none, VictoryCondition::KillEnemyLeader, pEmpire));
    // 5: home planet (Primary_Enemy's Home_Planet) owned and its leader company gone.
    r7.factions[0].primary_enemy = 1;
    r7.factions[1].home_planet_type = tPlanetE;
    CHECK(!victory_condition_met(s7, none, VictoryCondition::ConquerEnemyHomeworldAndLeader, pEmpire));
    s7.change_owner(s7.planets()[rE].object, pEmpire);
    CHECK(victory_condition_met(s7, none, VictoryCondition::ConquerEnemyHomeworldAndLeader, pEmpire));
}

// L1 systems t02 (TSW_PLANET_DESTROY §5): the tactical super weapon and the galactic Death Star use the shared core only -
// orbit fleets (the Death Star's own and the defender's), starbase and owner stay; the surface dies. The hack path still
// does it all.
void test_tsw_planet_destroy() {
    std::printf("  oi: TSW / Death Star planet destruction (core only)\n");
    auto fleets_ok = [](const GalacticSim& s, PlanetIndex p) {
        return s.fleets_at(p, pEmpire).size() == 1 && s.fleets_at(p, pUnder).size() == 1;
    };
    for (int pass = 0; pass < 3; ++pass) {  // 0 tactical TSW, 1 galactic Death Star, 2 hack
        OiWorld w({{pEmpire, rA, tSB1}, {pUnder, rC, tSB1}, {pUnder, rC, tCompany}, {pUnder, rC, tFrigate}, {pEmpire, rC, tDeathStar}});
        GalacticSim& s = w.sim;
        s.set_emit_events(true);
        const int32_t owner = s.planets()[rC].owner;
        const ObjectHandle sb = s.planets()[rC].starbase;
        CHECK(owner == pUnder && w.store.alive(sb) && !s.planets()[rC].surface.empty() && fleets_ok(s, rC));
        bool hit = true;
        if (pass == 0) CHECK(s.tsw_destroy_planet(rC, pEmpire, &hit) && !hit);
        if (pass == 1) CHECK(s.super_weapon_destroy_planet(rC, pEmpire));
        if (pass == 2) CHECK(s.planet_destroy(rC, pEmpire));
        CHECK(s.planets()[rC].destroyed && s.planets()[rC].surface.empty());
        std::vector<GalacticEvent> ev;
        s.drain_events(ev);
        int32_t value = -1;
        for (const GalacticEvent& e : ev)
            if (e.kind == GalacticEventKind::PlanetDestroyed && e.planet == rC) value = e.value;
        CHECK(value == (pass == 0 ? 1 : 0));
        if (pass < 2) {
            CHECK(fleets_ok(s, rC) && store_alive_type(s, tDeathStar));
            CHECK(w.store.alive(sb) && s.planets()[rC].starbase == sb && s.planets()[rC].owner == owner);
            CHECK(s.victory_claims().empty());
            CHECK(!s.tsw_destroy_planet(rC, pEmpire) && !s.super_weapon_destroy_planet(rC, pEmpire));  // once
        } else {
            CHECK(s.fleets_at(rC, pEmpire).empty() && s.fleets_at(rC, pUnder).empty() && !store_alive_type(s, tDeathStar));
            CHECK(!w.store.alive(sb) && s.planets()[rC].owner == s.neutral_player());
        }
    }
}

void test_a2_fleet_management() {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tCorvette},
             {pEmpire, rA, tCorvette}, {pEmpire, rA, tInfantry}, {pEmpire, rA, tInfantry}, {pUnder, rE, tCorvette}});
    GalacticSim& s = w.sim;
    const objects::ObjectStore& st = w.store;
    auto id = [&st](ObjectHandle h) { return static_cast<uint32_t>(st.id(h)); };
    const uint32_t pa = id(w.planet(rA).object);
    const uint32_t f0 = w.fleet(rA, pEmpire);
    CHECK(f0 != kNoFleet && w.fl(f0).contents.size() == 3 && w.planet(rA).surface.size() == 2);
    const ObjectHandle frigate = w.fl(f0).contents[0], c1 = w.fl(f0).contents[1], c2 = w.fl(f0).contents[2];
    const ObjectHandle inf1 = w.planet(rA).surface[0], inf2 = w.planet(rA).surface[1];
    const auto run = [&](FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit) {
        return s.execute(static_cast<uint8_t>(pEmpire), fleet_management_event(op, target, fleet, unit));
    };
    const uint32_t none = commands::kNoObject;
    // Form into a new fleet (fleet = kNoObject).
    CHECK(run(kFleetForm, pa, none, id(c1)) == RejectReason::None);
    CHECK(s.fleets_at(rA, pEmpire).size() == 2 && w.fl(f0).contents.size() == 2);
    const uint32_t f1 = s.fleets_at(rA, pEmpire)[1];
    CHECK(w.fl(f1).contents.size() == 1 && w.fl(f1).contents[0] == c1);
    CHECK(run(kFleetForm, pa, none, id(c1)) == RejectReason::None && s.fleets_at(rA, pEmpire).size() == 2);  // alone already
    // Form into an existing fleet.
    CHECK(run(kFleetForm, pa, id(w.fl(f1).object), id(c2)) == RejectReason::None);
    CHECK(w.fl(f1).contents.size() == 2 && w.fl(f0).contents.size() == 1 && w.fl(f0).contents[0] == frigate);
    CHECK(run(kFleetForm, pa, id(w.fl(f1).object), id(frigate)) == RejectReason::None && !w.fl(f0).alive);  // emptied: dies
    CHECK(run(kFleetForm, pa, none, 999999u) == RejectReason::InvalidUnit);
    CHECK(run(kFleetForm, id(w.planet(rB).object), none, id(c1)) == RejectReason::InvalidUnit);  // not orbiting B
    // Detach, merge (one unit, then the whole fleet).
    CHECK(run(kFleetDetach, pa, none, id(frigate)) == RejectReason::None && s.fleets_at(rA, pEmpire).size() == 2);
    const uint32_t f2 = s.fleets_at(rA, pEmpire)[1];
    CHECK(w.fl(f2).contents.size() == 1 && w.fl(f2).contents[0] == frigate);
    CHECK(run(kFleetMerge, id(w.fl(f2).object), id(w.fl(f1).object), id(c1)) == RejectReason::None);
    CHECK(w.fl(f2).contents.size() == 2 && w.fl(f1).contents.size() == 1);
    CHECK(run(kFleetMerge, id(w.fl(f2).object), id(w.fl(f1).object), none) == RejectReason::None && !w.fl(f1).alive);
    CHECK(w.fl(f2).contents.size() == 3 && s.fleets_at(rA, pEmpire).size() == 1);
    CHECK(run(kFleetMerge, id(w.fl(f2).object), id(w.fl(f2).object), none) == RejectReason::InvalidTarget);  // into itself
    // Launch both infantry (orbit fleet), land one, then all ground units.
    CHECK(run(kFleetLaunch, none, none, id(inf1)) == RejectReason::None);
    CHECK(run(kFleetLaunch, id(w.fl(f2).object), none, id(inf2)) == RejectReason::None);
    CHECK(w.planet(rA).surface.empty() && w.fl(f2).contents.size() == 5);
    CHECK(run(kFleetLand, pa, id(w.fl(f2).object), id(inf2)) == RejectReason::None && w.planet(rA).surface.size() == 1);
    CHECK(run(kFleetLand, pa, id(w.fl(f2).object), id(frigate)) == RejectReason::InvalidUnit);  // not a ground unit
    CHECK(run(kFleetLand, pa, id(w.fl(f2).object), none) == RejectReason::None && w.planet(rA).surface.size() == 2);
    CHECK(w.fl(f2).contents.size() == 3);
    CHECK(run(kFleetLand, id(w.planet(rE).object), id(w.fl(f2).object), none) == RejectReason::InvalidTarget);  // not owned
    // Other players' fleets, unknown ops.
    const uint32_t fu = w.fleet(rE, pUnder);
    CHECK(run(kFleetDetach, id(w.planet(rE).object), id(w.fl(fu).object), id(w.fl(fu).contents[0])) == RejectReason::InvalidUnit);
    CHECK(run(static_cast<FleetOp>(9), pa, none, id(frigate)) == RejectReason::NotAllowed);  // ops 0-8 exist (SG 1.7)
    // Ignored while the galactic pause counter is raised (also MoveThroughObjects).
    s.push_pause();
    CHECK(run(kFleetDetach, pa, none, id(frigate)) == RejectReason::NotAllowed);
    commands::EventData mto(commands::EventType::MoveThroughObjects);
    mto.set("object", id(w.fl(f2).object)).set_ids("waypoints", {id(w.planet(rB).object)});
    CHECK(s.execute(static_cast<uint8_t>(pEmpire), mto) == RejectReason::NotAllowed);
    s.pop_pause();
    CHECK(s.execute(static_cast<uint8_t>(pEmpire), mto) == RejectReason::None);
    // The same ops through the scheduled command pipeline (the HUD / replay path) and the direct path (AI).
    World v({{pEmpire, rA, tFrigate}, {pEmpire, rA, tCorvette}});
    commands::CommandPipeline pipe;
    pipe.set_handler(v.sim.command_handler());
    const uint32_t g = v.fleet(rA, pEmpire);
    const uint32_t vpa = static_cast<uint32_t>(v.store.id(v.planet(rA).object));
    const uint32_t cor = static_cast<uint32_t>(v.store.id(v.fl(g).contents[1]));
    pipe.begin_tick(0);
    pipe.submit(static_cast<uint8_t>(pEmpire), commands::CommandSource::Player, fleet_management_event(kFleetDetach, vpa, none, cor));
    v.sim.update();
    pipe.begin_tick(1);
    v.sim.update();
    CHECK(v.sim.fleets_at(rA, pEmpire).size() == 2);
    const uint32_t lone = static_cast<uint32_t>(v.store.id(v.fl(v.sim.fleets_at(rA, pEmpire)[1]).object));
    pipe.execute_direct(2, static_cast<uint8_t>(pEmpire), commands::CommandSource::AI,
                        fleet_management_event(kFleetMerge, static_cast<uint32_t>(v.store.id(v.fl(g).object)), lone, none));
    CHECK(v.sim.fleets_at(rA, pEmpire).size() == 1 && v.fl(g).contents.size() == 2);
}

// ---- Playtest 3 t07: fleet orbit slots (campaign.hpp FleetState::slot, src/fleet_slots.cpp) ---------------------------------

void test_fleet_slots() {
    std::printf("  fleet slots\n");
    const uint32_t none = commands::kNoObject;
    // The same scripted orders on two worlds (determinism: equal CRC state), the second with one different slot choice.
    const auto script = [&](World& w, uint32_t detach_slot, std::vector<uint64_t>* trace) {
        GalacticSim& s = w.sim;
        const auto id = [&w](ObjectHandle h) { return static_cast<uint32_t>(w.store.id(h)); };
        const auto fm = [&](FleetOp op, uint32_t target, uint32_t fleet, uint32_t unit, uint32_t a = commands::kNoObject, uint32_t slot = 0,
                            uint32_t b = commands::kNoObject) {
            commands::EventData e = fleet_management_event(op, target, fleet, unit, a, slot);
            e.set("b", b);
            return s.execute(static_cast<uint8_t>(pEmpire), e);
        };
        const uint32_t pa = id(w.planet(rA).object), pb = id(w.planet(rB).object);
        const uint32_t f0 = w.fleet(rA, pEmpire);
        const uint32_t fb = w.fleet(rB, pEmpire);
        CHECK(f0 != kNoFleet && fb != kNoFleet && w.fl(f0).contents.size() == 4);
        // Fleets entering an orbit take the owner's lowest free slot (separate slot spaces per owner and planet).
        CHECK(w.fl(f0).slot == 0 && w.fl(fb).slot == 0 && w.fl(w.fleet(rE, pUnder)).slot == 0);
        CHECK(s.slot_state_default() && s.fleet_in_slot(rA, pEmpire, 0) == f0 && s.fleet_in_slot(rA, pEmpire, 1) == kNoFleet);
        const std::vector<ObjectHandle> m = w.fl(f0).contents;
        // Detach with a chosen slot (`b`), then without one (lowest free).
        CHECK(fm(kFleetDetach, pa, none, id(m[1]), none, 0, detach_slot) == RejectReason::None);
        CHECK(!s.slot_state_default());
        const uint32_t fA = s.fleet_containing(m[1]);
        CHECK(fA != kNoFleet && fA != f0 && w.fl(fA).slot == static_cast<int8_t>(detach_slot));
        CHECK(fm(kFleetDetach, pa, none, id(m[2])) == RejectReason::None);
        const uint32_t fB = s.fleet_containing(m[2]);
        CHECK(fB != kNoFleet && w.fl(fB).slot == (detach_slot == 1 ? 2 : 1));
        // A taken slot in `b` keeps the automatic one: m[3] -> the only slot left.
        CHECK(fm(kFleetDetach, pa, none, id(m[3]), none, 0, 0u) == RejectReason::None);
        const uint32_t fC = s.fleet_containing(m[3]);
        CHECK(fC != kNoFleet && w.fl(f0).slot == 0 && w.fl(fC).slot == -1);  // all three slots taken: unslotted overflow
        CHECK(s.fleet_in_slot(rA, pEmpire, 0) == f0 && w.fl(f0).contents.size() == 1);
        // Op 8 in place: a free slot moves the fleet, a taken one merges into the fleet in it, bad slots are refused.
        if (trace) {
            StateHasher h;
            s.hash_state(h);
            trace->push_back(h.digest());  // after the three detaches: the slot choice is in the CRC state
        }
        CHECK(fm(kFleetSlot, pa, id(w.fl(fA).object), none, none, 3) == RejectReason::InvalidTarget);
        CHECK(fm(kFleetSlot, pa, id(w.fl(fA).object), none, none, static_cast<uint32_t>(w.fl(fB).slot)) == RejectReason::None);  // merge into fB
        CHECK(!w.fl(fA).alive && w.fl(fB).contents.size() == 2);
        CHECK(w.fl(fC).slot == static_cast<int8_t>(detach_slot));  // the freed slot went to the unslotted fleet (orbit order)
        CHECK(fm(kFleetSlot, pa, id(w.fl(fB).object), none, none, static_cast<uint32_t>(w.fl(fB).slot)) == RejectReason::None && w.fl(fB).alive);
        CHECK(fm(kFleetSlot, pa, id(w.fl(fB).object), none, none, 0) == RejectReason::None);  // slot 0 is f0's: merge
        CHECK(!w.fl(fB).alive && w.fl(f0).contents.size() == 3 && s.fleet_in_slot(rA, pEmpire, w.fl(fC).slot == 1 ? 2 : 1) == kNoFleet);
        CHECK(fm(kFleetSlot, pa, id(w.fl(fC).object), none, none, 1) == RejectReason::None);  // a free slot: it moves
        CHECK(w.fl(fC).slot == 1 && s.fleet_in_slot(rA, pEmpire, 1) == fC && s.fleet_in_slot(rA, pEmpire, 2) == kNoFleet);
        // Another owner's fleet cannot be re-slotted.
        const uint32_t fu = w.fleet(rE, pUnder);
        CHECK(fm(kFleetSlot, id(w.planet(rE).object), id(w.fl(fu).object), none, none, 1) == RejectReason::InvalidUnit);
        // Departure frees the slot; the requested slot at the destination (op 8 with a = planet) is taken on arrival.
        CHECK(fm(kFleetSlot, pa, id(w.fl(fC).object), none, pb, 1) == RejectReason::None);
        CHECK(w.fl(fC).want_planet == rB && w.fl(fC).want_slot == 1 && w.fl(fC).slot == 1);
        CHECK(s.move_fleet(pEmpire, fC, rB) == RejectReason::None);
        CHECK(w.fl(fC).slot == -1 && s.fleet_in_slot(rA, pEmpire, 1) == kNoFleet);
        w.run(200);
        CHECK(w.fl(fC).alive && !w.fl(fC).moving && w.fl(fC).planet == rB && w.fl(fC).slot == 1);  // free slot 1: no merge
        CHECK(w.fl(fC).want_planet == kInvalidPlanet && w.fl(fb).alive && w.fl(fb).slot == 0 && s.fleets_at(rB, pEmpire).size() == 2);
        // A request for an occupied slot merges into that fleet on arrival; without a request it merges into the first one.
        CHECK(fm(kFleetDetach, pa, none, id(m[2])) == RejectReason::None);
        const uint32_t fD = s.fleet_containing(m[2]);
        CHECK(fD != kNoFleet && fD != f0 && !w.fl(fD).moving && w.fl(fD).slot == 1);
        CHECK(fm(kFleetSlot, pa, id(w.fl(fD).object), none, pb, 1) == RejectReason::None);
        CHECK(s.move_fleet(pEmpire, fD, rB) == RejectReason::None);
        w.run(200);
        CHECK(!w.fl(fD).alive && w.fl(fC).contents.size() == 2 && s.fleets_at(rB, pEmpire).size() == 2);
        CHECK(s.move_fleet(pEmpire, f0, rB) == RejectReason::None);
        w.run(200);
        CHECK(!w.fl(f0).alive && w.fl(fb).contents.size() == 3 && s.fleets_at(rB, pEmpire).size() == 2);  // into the first (slot 0)
    };
    std::vector<uint64_t> ta, tb, tc;
    std::vector<StartingForce> forces = {{pEmpire, rA, tSB1}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tFrigate},
                                         {pEmpire, rA, tFrigate}, {pEmpire, rB, tCorvette}, {pUnder, rE, tCorvette}};
    World wa(forces), wb(forces), wc(forces);
    script(wa, 2, &ta);
    script(wb, 2, &tb);
    script(wc, 1, &tc);
    StateHasher ha, hb;
    wa.sim.hash_state(ha);
    wb.sim.hash_state(hb);
    CHECK(ha.digest() == hb.digest() && ta == tb && ta.size() == 1);  // same orders: same slots, same CRC
    CHECK(ta != tc);                                                  // a different slot choice is part of the CRC state
    // A world that never chose a slot keeps its CRC independent of the (automatic) slots: no "FSL1" tail.
    World wd({{pEmpire, rA, tFrigate}, {pEmpire, rB, tFrigate}});
    CHECK(wd.sim.slot_state_default());
    CHECK(wd.sim.move_fleet(pEmpire, wd.fleet(rA, pEmpire), rB) == RejectReason::None);
    wd.run(200);
    CHECK(wd.sim.slot_state_default() && wd.sim.fleets_at(rB, pEmpire).size() == 1 && wd.fl(wd.fleet(rB, pEmpire)).slot == 0);
    {   // Ops 6 / 7 and a unit dragged off its fleet (MoveObjectToObject {unit, planet}).
        World w({{pEmpire, rA, tFrigate}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tFrigate}, {pEmpire, rA, tSB1}});
        GalacticSim& s = w.sim;
        const auto id = [&w](ObjectHandle h) { return static_cast<uint32_t>(w.store.id(h)); };
        const uint32_t pa = id(w.planet(rA).object), pb = id(w.planet(rB).object);
        const uint32_t f0 = w.fleet(rA, pEmpire);
        const std::vector<ObjectHandle> m = w.fl(f0).contents;
        // Op 6 with b: the new fleet moves toward B and takes slot 2 there.
        commands::EventData e = fleet_management_event(kFleetMoveNew, pa, none, id(m[0]), pb);
        e.set("b", 2u);
        CHECK(s.execute(static_cast<uint8_t>(pEmpire), e) == RejectReason::None);
        const uint32_t mv = s.fleet_containing(m[0]);
        CHECK(mv != kNoFleet && w.fl(mv).moving && w.fl(mv).slot == -1 && w.fl(mv).want_planet == rB && w.fl(mv).want_slot == 2);
        w.run(300);
        CHECK(w.fl(mv).slot == 2 && w.fl(mv).planet == rB && !w.fl(mv).moving && w.fl(mv).want_planet == kInvalidPlanet);
        // A unit drag to a planet: the unit is split off into its own moving fleet; the rest stays in slot 0.
        commands::EventData mo(commands::EventType::MoveObjectToObject);
        mo.set("object", id(m[1])).set("target", pb);
        CHECK(s.execute(static_cast<uint8_t>(pEmpire), mo) == RejectReason::None);
        const uint32_t mv2 = s.fleet_containing(m[1]);
        CHECK(mv2 != kNoFleet && mv2 != f0 && w.fl(mv2).moving && w.fl(mv2).contents.size() == 1 && w.fl(f0).contents.size() == 1);
        CHECK(w.fl(f0).slot == 0 && s.fleet_in_slot(rA, pEmpire, 1) == kNoFleet);
        // The last member moves its own fleet; a drop on its own planet does nothing.
        mo.set("object", id(m[2])).set("target", pa);
        CHECK(s.execute(static_cast<uint8_t>(pEmpire), mo) == RejectReason::None);
        CHECK(s.fleet_containing(m[2]) == f0 && w.fl(f0).contents.size() == 1 && !w.fl(f0).moving);
        w.run(300);
        CHECK(!w.fl(mv2).alive && w.fl(mv).contents.size() == 2 && w.fl(mv).slot == 2);  // no request: merged into the first one
    }
}

// ---- 3./4. scripted campaigns, CRC logs -------------------------------------------------------------------------------

struct RunResult {
    CrcLog log;          // every captured frame
    CrcLog golden_view;  // every `stride`-th frame + the last
};

CrcRegistry make_registry(const GalacticSim& sim, const SimRng& rng) {
    CrcRegistry reg;
    sim.register_crc(reg, "galactic");
    reg.add("rng", [&rng](StateHasher& h) { rng.hash(h); });
    reg.add_parity("rng.parity", [&rng] { return parity_rng_crc(rng.state()); });
    return reg;
}

uint32_t oid(const objects::ObjectStore& st, ObjectHandle h) { return static_cast<uint32_t>(st.id(h)); }

// Synthetic campaign: orders through the scheduled pipeline (human Empire) and the direct path (AI Underworld).
RunResult run_synthetic(const ParallelFor& pf, int32_t days, int32_t stride) {
    World w({{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pEmpire, rA, tFrigate}, {pEmpire, rB, tBarracks},
             {pUnder, rE, tSB1}, {pUnder, rE, tBarracks}, {pUnder, rE, tCorvette}, {pUnder, rD, tInfantry}});
    GalacticSim& s = w.sim;
    s.set_parallel_for(pf);
    s.set_black_market_influence(rC, true);
    s.set_black_market_influence(rB, true);
    commands::CommandPipeline pipe;
    pipe.set_handler(s.command_handler());
    const CrcRegistry reg = make_registry(s, w.rng);
    RunResult out;
    out.log.systems = out.golden_view.systems = reg.names();
    out.log.parity_parts = out.golden_view.parity_parts = reg.parity_names();
    auto pid = [&](PlanetIndex p) { return oid(w.store, w.planet(p).object); };
    bool landed = false;
    const int32_t frames = days * econ::fiscal_period_frames(w.rules.constants.fiscal_cycle_seconds);
    for (int32_t t = 0; t < frames; ++t) {
        pipe.begin_tick(static_cast<uint64_t>(t));
        auto submit = [&](commands::EventData ev) {
            pipe.submit(static_cast<uint8_t>(pEmpire), commands::CommandSource::Player, std::move(ev));
        };
        auto direct = [&](int32_t player, const commands::EventData& ev) {
            pipe.execute_direct(static_cast<uint64_t>(t), static_cast<uint8_t>(player), commands::CommandSource::AI, ev);
        };
        auto prod = [&](TypeId ty, PlanetIndex p) { return commands::ev::production(static_cast<uint16_t>(ty), pid(p)); };
        if (t == 1) submit(prod(tFrigate, rA));
        if (t == 2) submit(prod(tInfantry, rA));
        if (t == 3) direct(pUnder, prod(tCorvette, rE));
        if (t == 5) {
            submit(prod(tFighters, rA));
            submit(prod(tFighters, rA));
        }
        if (t == 7) submit(commands::ev::production(0, pid(rA), true, kSpaceQueue, 2));  // cancel the 2nd fighters
        if (t == 50) {
            const uint32_t f = w.fleet(rA, pEmpire);
            if (f != kNoFleet) submit(commands::ev::move_object_to_object(oid(w.store, w.fl(f).object), pid(rC)));
        }
        if (t == 400) {
            const uint32_t f = w.fleet(rE, pUnder);
            if (f != kNoFleet) direct(pUnder, commands::ev::move_object_to_object(oid(w.store, w.fl(f).object), pid(rC)));
        }
        if (t == 700) s.set_corruption(rB, kCorruptionPiracy, pUnder);
        if (t == 900 && !w.planet(rA).surface.empty()) {
            s.launch_units(pEmpire, rA, std::vector<ObjectHandle>(w.planet(rA).surface.begin(), w.planet(rA).surface.end()));
            const uint32_t f = w.fleet(rA, pEmpire);
            if (f != kNoFleet) submit(commands::ev::move_object_to_object(oid(w.store, w.fl(f).object), pid(rD)));
        }
        if (!landed && t > 900) {
            const uint32_t f = w.fleet(rD, pEmpire);
            if (f != kNoFleet && !w.fl(f).moving) {
                const uint32_t uf = w.fleet(rD, pUnder);
                if (uf != kNoFleet) s.destroy_object(w.fl(uf).object);
                if (!w.planet(rD).surface.empty()) s.destroy_object(w.planet(rD).surface[0]);  // battle stand-in
                commands::EventData inv(commands::EventType::Invade);
                inv.set("planet", pid(rD));
                submit(inv);
                landed = true;
            }
        }
        if (t == 3000) {
            const uint32_t f = w.fleet(rC, pUnder);
            if (f != kNoFleet) direct(pUnder, commands::ev::move_object_to_object(oid(w.store, w.fl(f).object), pid(rE)));
        }
        if (t % 1350 == 600) direct(pUnder, prod(tCorvette, rE));
        s.update();
        CrcRecord rec = reg.capture(static_cast<uint64_t>(t), pf);
        if (t % stride == 0 || t + 1 == frames) out.golden_view.records.push_back(rec);
        out.log.records.push_back(std::move(rec));
    }
    std::printf("  synthetic: %d days, frame %d, credits E %.2f U %.2f, owners C=%d D=%d, fleets %zu, landed %d\n", days,
                s.frame(), static_cast<double>(s.credits(pEmpire)), static_cast<double>(s.credits(pUnder)),
                w.planet(rC).owner, w.planet(rD).owner, s.fleets().size(), landed ? 1 : 0);
    return out;
}

std::string g_golden_dir;
bool g_write_golden = false;
bool g_quick = false;

void check_golden(const CrcLog& log, const std::string& file) {
    if (g_golden_dir.empty()) {
        std::printf("  (no --golden-dir: %s not compared)\n", file.c_str());
        return;
    }
    const std::string path = g_golden_dir + "/" + file;
    if (g_write_golden) {
        std::string err;
        CHECK(log.save(path, &err));
        std::printf("  wrote %s (%zu records)\n", path.c_str(), log.records.size());
        return;
    }
    CrcLog golden;
    std::string err;
    if (!CrcLog::load(path, golden, &err)) {
        std::printf("FAIL golden %s: %s\n", path.c_str(), err.c_str());
        ++g_fail;
        return;
    }
    const CrcDivergence d = first_divergence(golden, log);
    if (d.diverged) std::printf("FAIL golden %s: %s\n", file.c_str(), d.message.c_str());
    CHECK(!d.diverged);
}

std::vector<std::pair<unsigned, ParallelFor>> worker_configs() {
    std::vector<std::pair<unsigned, ParallelFor>> v;
    v.push_back({0u, ParallelFor(serial_for)});
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned n : {1u, 4u, hw}) v.push_back({n, testing::make_thread_parallel_for(n)});
    return v;
}

void test_synthetic_campaign() {
    const int32_t days = 8;
    RunResult base;
    bool first = true;
    for (auto& [n, pf] : worker_configs()) {
        RunResult r = run_synthetic(pf, days, 90);
        if (first) {
            base = std::move(r);
            first = false;
            continue;
        }
        const CrcDivergence d = first_divergence(base.log, r.log);
        if (d.diverged) std::printf("FAIL workers %u: %s\n", n, d.message.c_str());
        CHECK(!d.diverged);
    }
    base.golden_view.comments.push_back("S3 synthetic campaign, 8 galactic days, every 90th frame + last");
    base.golden_view.comments.push_back("2026-10-28 OI galactic: lifetime build counts hashed (STRATEGIC_GAPS 1.3 step 22)");
    check_golden(base.golden_view, "galactic_synthetic_crc.txt");
}

// ---- 4. real FoC data -------------------------------------------------------------------------------------------------

std::string env_or(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

struct FocData {
    std::unique_ptr<FileProvider> provider;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    GalacticRules rules;
};

RunResult run_foc(const FocData& d, const ParallelFor& pf, int32_t days, int32_t stride) {
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, d.types, &ids);
    SimRng rng(0xC0FFEEu);
    GalacticSim s(d.rules, store, rng);
    s.set_parallel_for(pf);
    std::vector<std::string> diag;
    const GalacticSetup setup = build_campaign_setup(d.db, d.rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    CHECK(s.start(setup, &diag));
    const int32_t U = s.player_of_faction("Underworld"), E = s.player_of_faction("Empire"), R = s.player_of_faction("Rebel");
    CHECK(U >= 0 && E >= 0 && R >= 0 && s.players()[static_cast<size_t>(U)].human && s.players()[static_cast<size_t>(E)].ai);
    auto row = [&](const char* n) { return s.map().planets.find(n); };
    auto type = [&](const char* n) { return d.rules.find_type(n); };
    auto pid = [&](PlanetIndex p) { return static_cast<uint32_t>(store.id(s.planets()[p].object)); };
    auto fid = [&](uint32_t f) { return static_cast<uint32_t>(store.id(s.fleets()[f].object)); };
    auto fleet_at = [&](PlanetIndex p, int32_t pl) {
        const std::vector<uint32_t> f = s.fleets_at(p, pl);
        return f.empty() ? kNoFleet : f[0];
    };
    s.set_black_market_influence(row("Felucia"), true);
    s.set_corruption(row("Naboo"), kCorruptionPiracy, U);
    commands::CommandPipeline pipe;
    pipe.set_handler(s.command_handler());
    const CrcRegistry reg = make_registry(s, rng);
    RunResult out;
    out.log.systems = out.golden_view.systems = reg.names();
    out.log.parity_parts = out.golden_view.parity_parts = reg.parity_names();
    const PlanetIndex tatooine = row("Tatooine"), shola = row("Shola");
    bool raided = false, landed = false;
    const int32_t frames = days * econ::fiscal_period_frames(d.rules.constants.fiscal_cycle_seconds);
    for (int32_t t = 0; t < frames; ++t) {
        pipe.begin_tick(static_cast<uint64_t>(t));
        auto submit = [&](commands::EventData ev) { pipe.submit(static_cast<uint8_t>(U), commands::CommandSource::Player, std::move(ev)); };
        auto direct = [&](int32_t player, const commands::EventData& ev) {
            pipe.execute_direct(static_cast<uint64_t>(t), static_cast<uint8_t>(player), commands::CommandSource::AI, ev);
        };
        auto produce = [&](const char* ty, const char* planet) {
            return commands::ev::production(static_cast<uint16_t>(type(ty)), pid(row(planet)));
        };
        if (t == 10) {
            submit(produce("Crusader_Gunship", "Felucia"));
            submit(produce("Underworld_Merc_Squad", "Shola"));
            submit(produce("Underworld_Merc_Squad", "Shola"));
        }
        if (t == 12) direct(E, produce("Tartan_Patrol_Cruiser", "Kuat"));
        if (t == 14) direct(R, produce("Corellian_Corvette", "MonCalimari"));
        if (t == 20) {
            submit(produce("StarViper_Squadron", "Shola"));
            submit(produce("StarViper_Squadron", "Shola"));
        }
        if (t == 21) submit(commands::ev::production(0, pid(shola), true, kSpaceQueue, 1));
        if (t == 60) {
            const uint32_t f = fleet_at(row("The_Maw"), U);
            if (f != kNoFleet) submit(commands::ev::move_object_to_object(fid(f), pid(shola)));
        }
        if (t == 90) {
            const uint32_t f = fleet_at(row("Kuat"), E);
            if (f != kNoFleet) direct(E, commands::ev::move_object_to_object(fid(f), pid(row("Corellia"))));
        }
        if (t == 150) {
            const uint32_t f = fleet_at(row("Kamino"), R);
            if (f != kNoFleet) direct(R, commands::ev::move_object_to_object(fid(f), pid(row("Naboo"))));
        }
        if (t == 1500) {
            const uint32_t f = fleet_at(row("Naboo"), R);
            if (f != kNoFleet) direct(R, commands::ev::move_object_to_object(fid(f), pid(row("MonCalimari"))));
        }
        if (t == 2000) {
            const uint32_t f = fleet_at(row("MonCalimari"), R);
            if (f != kNoFleet) direct(R, commands::ev::move_object_to_object(fid(f), pid(shola)));
        }
        if (t == 2700 && !raided) {
            // Battle-outcome stand-in: the Empire garrison of Tatooine is wiped out, then Underworld loads its
            // mercenaries at Shola and sails for Tatooine.
            const PlanetState& p = s.planets()[tatooine];
            std::vector<ObjectHandle> kill(p.structures.begin(), p.structures.end());
            kill.insert(kill.end(), p.surface.begin(), p.surface.end());
            if (store.alive(p.starbase)) kill.push_back(p.starbase);
            for (uint32_t f : s.fleets_at(tatooine, E)) kill.push_back(s.fleets()[f].object);
            for (ObjectHandle h : kill) s.destroy_object(h);
            const PlanetState& sh = s.planets()[shola];
            std::vector<ObjectHandle> mercs;
            for (ObjectHandle h : sh.surface)
                if (store.owner(h) == U) mercs.push_back(h);
            s.launch_units(U, shola, mercs);
            const uint32_t f = fleet_at(shola, U);
            if (f != kNoFleet) submit(commands::ev::move_object_to_object(fid(f), pid(tatooine)));
            raided = true;
        }
        if (raided && !landed) {
            const uint32_t f = fleet_at(tatooine, U);
            if (f != kNoFleet && !s.fleets()[f].moving) {
                commands::EventData inv(commands::EventType::Invade);
                inv.set("planet", pid(tatooine));
                submit(inv);
                landed = true;
            }
        }
        if (t % 1350 == 700) {
            direct(E, produce("Tartan_Patrol_Cruiser", "Kuat"));
            direct(R, produce("Corellian_Corvette", "MonCalimari"));
        }
        s.update();
        CrcRecord rec = reg.capture(static_cast<uint64_t>(t), pf);
        if (t % stride == 0 || t + 1 == frames) out.golden_view.records.push_back(rec);
        out.log.records.push_back(std::move(rec));
    }
    std::printf("  FoC Heart of the Maw: %d days, players %zu, planets owned U %d E %d R %d, credits U %.1f E %.1f R %.1f, "
                "Tatooine owner %d (U %d, landed %d, surface %zu), fleets %zu, objects %zu\n",
                days, s.players().size(), s.planets_owned(U), s.planets_owned(E), s.planets_owned(R),
                static_cast<double>(s.credits(U)), static_cast<double>(s.credits(E)), static_cast<double>(s.credits(R)),
                s.planets()[tatooine].owner, U, landed ? 1 : 0, s.planets()[tatooine].surface.size(), s.fleets().size(), store.live_count());
    return out;
}

// OI galactic on FoC data: M, company transports, dummy behaviours, buildability fields (SG 1.2 / 1.3 / 1.7 / 2.1).
void test_oi_foc(const FocData& d) {
    const GalacticRules& r = d.rules;
    CHECK(r.constants.fleet_speed_multiplier == 5.0f);  // Object_Max_Speed_Multiplier_Galactic
    const uint32_t sab = r.find_type("Underworld_Saboteur_Team"), shuttle = r.find_type("Underworld_Hero_Shuttle");
    const uint32_t member = r.find_type("Underworld_Saboteur");
    CHECK(sab != kNoIndex && shuttle != kNoIndex && member != kNoIndex);
    if (sab == kNoIndex || shuttle == kNoIndex || member == kNoIndex) return;
    CHECK(r.types[sab].dummy == kDummyGroundCompany && r.types[sab].company_transport == shuttle);
    CHECK(r.types[shuttle].hyperspace && r.types[shuttle].hyperspace_speed == 1.5f);
    CHECK(r.types[member].company_member && !r.types[sab].company_member);
    const uint32_t sb1 = r.find_type("Underworld_Star_Base_1"), xw = r.find_type("X-Wing");
    CHECK(sb1 != kNoIndex && r.types[sb1].dummy == kDummyStarBase && is_star_base_type(r.types[sb1]));
    CHECK(xw != kNoIndex && r.types[xw].squadron_member);
    const uint32_t gal = r.find_type("Gallofree_Transport");
    CHECK(gal != kNoIndex && r.types[gal].hyperspace && r.types[gal].hyperspace_speed == 1.5f);
    int32_t limited = 0, never = 0, small_bases = 0, gated = 0;
    for (const TypeInfo& t : r.types) {
        limited += t.limit_current > 0 ? 1 : 0;
        never += t.limit_lifetime > 0 ? 1 : 0;  // the shipped 0 values are all commented out
        small_bases += (t.kind == UnitKind::Planet && t.max_space_base < 5) ? 1 : 0;
        gated += t.required_special_groups.empty() ? 0 : 1;
    }
    std::printf("  oi FoC: current-limited %d, lifetime-limited %d, planets with Max_Space_Base < 5 %d, gated %d\n", limited, never,
                small_bases, gated);
    CHECK(limited > 0 && never > 0 && small_bases > 0 && gated > 0);
    // REMAINING_GAPS 6.3 data: FoC sets Planet_Starbase_Stealth on 1 planet and Planet_Groundbase_Stealth on 7.
    int32_t sb_stealth = 0, gb_stealth = 0;
    for (const TypeInfo& t : r.types) {
        sb_stealth += (t.kind == UnitKind::Planet && t.starbase_stealth) ? 1 : 0;
        gb_stealth += (t.kind == UnitKind::Planet && t.groundbase_stealth) ? 1 : 0;
    }
    std::printf("  oi FoC: planets with starbase stealth %d, ground-base stealth %d\n", sb_stealth, gb_stealth);
    CHECK(sb_stealth == 1 && gb_stealth == 7);
    // The saboteur team is created as a transport on Underworld's Shola, launches and jumps at the shuttle's speed x M.
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, d.types, &ids);
    SimRng rng(7u);
    GalacticSim s(r, store, rng);
    CHECK(s.start(build_campaign_setup(d.db, r, "Sandbox_Heart_Of_The_Maw_Underworld")));
    const int32_t U = s.player_of_faction("Underworld");
    const PlanetIndex shola = s.map().planets.find("Shola"), tat = s.map().planets.find("Tatooine");
    CHECK(U >= 0 && shola != kInvalidPlanet && tat != kInvalidPlanet);
    if (U < 0 || shola == kInvalidPlanet) return;
    s.update();  // GRC: frame 0's pre-step reveals the hop targets first
    const ObjectHandle team = s.spawn_unit(U, shola, sab);
    CHECK(store.alive(team) && s.is_company_transport(team) && s.travel_type(team) == shuttle && s.galactic_stealth(team));
    const bool landed = std::find(s.planets()[shola].surface.begin(), s.planets()[shola].surface.end(), team) !=
                        s.planets()[shola].surface.end();
    // GRC: Tatooine (two hops away) is not revealed on frame 0, so the human's order there is refused by the hop gate;
    // a revealed route neighbour of Shola is accepted.
    CHECK(!s.planet_revealed_raw(tat) && s.is_planet_revealed(tat));  // auto-reveal campaign: Is_Planet_Revealed true
    const PlanetIndex nb = s.graph().neighbour(s.graph().edge_begin(shola));
    CHECK(s.planet_revealed_raw(nb));
    if (landed) {
        CHECK(s.execute(static_cast<uint8_t>(U), fleet_management_event(kFleetMoveNew, static_cast<uint32_t>(store.id(s.planets()[shola].object)),
                                                                         commands::kNoObject, static_cast<uint32_t>(store.id(team)),
                                                                         static_cast<uint32_t>(store.id(s.planets()[nb].object)))) ==
              RejectReason::None);
    } else {
        CHECK(s.move_fleet(U, s.fleet_containing(team), tat) == RejectReason::InvalidTarget);
        CHECK(s.move_fleet(U, s.fleet_containing(team), nb) == RejectReason::None);
    }
    const uint32_t f = s.fleet_containing(team);
    CHECK(f != kNoFleet && s.fleets()[f].moving &&
          s.fleets()[f].base_speed == econ::base_speed(1.5f, r.factions[s.players()[static_cast<size_t>(U)].faction].hyperspace_speed_factor, s.fleet_speed_multiplier()));
}

// Playtest 2026-10-28 ("fleets can't be moved"): the human player's move order on FoC Heart of the Maw, as the host
// sends it (MoveObjectToObject: fleet object id -> planet object id, through GalacticSim::execute). Every Underworld
// fleet at start is ordered to each neighbour of its planet (a fresh sim per order): the order is accepted, the
// fleet departs on the one-hop path and arrives at the tick fleet_step predicts unless stopped by a conflict.
void test_foc_player_fleet_moves(const FocData& d) {
    std::printf("  FoC player fleet moves\n");
    int32_t orders = 0, arrived = 0, intercepted = 0;
    for (uint32_t pick = 0;; ++pick) {
        objects::IdCounter ids;
        objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, d.types, &ids);
        SimRng rng(0xC0FFEEu);
        GalacticSim s(d.rules, store, rng);
        CHECK(s.start(build_campaign_setup(d.db, d.rules, "Sandbox_Heart_Of_The_Maw_Underworld")));
        const int32_t U = s.player_of_faction("Underworld");
        CHECK(U >= 0 && s.players()[static_cast<size_t>(U)].human);
        if (U < 0) return;
        // GRC: frame 0 runs first (as in play): its pre-step reveals the owned planets and their neighbours, which the
        // human single-player hop gate requires (raw revealed byte of each hop target).
        CHECK(s.local_player() == U);
        s.update();
        // The pick-th (fleet, neighbour) pair in fleet index / edge order.
        uint32_t f = kNoFleet;
        PlanetIndex to = kInvalidPlanet;
        uint32_t k = 0;
        for (uint32_t i = 0; i < s.fleets().size() && f == kNoFleet; ++i) {
            const FleetState& fl = s.fleets()[i];
            if (!fl.alive || fl.owner != U || fl.planet == kInvalidPlanet) continue;
            for (uint32_t e = s.graph().edge_begin(fl.planet); e < s.graph().edge_end(fl.planet); ++e)
                if (k++ == pick) {
                    f = i;
                    to = s.graph().neighbour(e);
                    break;
                }
        }
        if (f == kNoFleet) break;
        const PlanetIndex from = s.fleets()[f].planet;
        const RejectReason rr = s.execute(static_cast<uint8_t>(U),
                                          commands::ev::move_object_to_object(static_cast<commands::ObjectId>(store.id(s.fleets()[f].object)),
                                                                              static_cast<commands::ObjectId>(store.id(s.planets()[to].object))));
        if (rr != RejectReason::None)
            std::printf("FAIL move %s -> %s refused (%d)\n", s.map().planets.name[from].c_str(), s.map().planets.name[to].c_str(),
                        static_cast<int>(rr));
        CHECK(rr == RejectReason::None);
        if (rr != RejectReason::None) continue;
        ++orders;
        const FleetState& fl = s.fleets()[f];
        const ObjectHandle member = fl.contents.empty() ? ObjectHandle{} : fl.contents[0];
        CHECK(fl.moving && fl.from == from && fl.to == to && fl.path.size() == 2 && fl.planet == kInvalidPlanet);
        CHECK(fl.base_speed > 0.0f && fl.hop_speed >= fl.base_speed);
        const int32_t expect = frames_for_hop(s.planets()[from].position, s.planets()[to].position, fl.hop_speed);
        CHECK(expect > 1);
        int32_t t = 0;
        while (s.fleets()[f].alive && s.fleets()[f].moving && t < 100000) {
            s.update();
            ++t;
        }
        // On arrival the fleet may merge into a friendly fleet already in orbit (GE 3.3 step 4): follow a member.
        const uint32_t g = s.fleet_containing(member);
        CHECK(g != kNoFleet && !s.fleets()[g].moving && s.fleets()[g].planet == to);
        CHECK(t == expect);
        if (s.planets()[to].in_battle) ++intercepted;
        else ++arrived;
    }
    std::printf("  FoC player fleet moves: %d orders, %d arrived, %d into a conflict\n", orders, arrived, intercepted);
    CHECK(orders > 0);
}

// G1 hop timing (2026-10-29): the Empire's Corellia fleet of FoC Sandbox_Heart_Of_The_Maw_Empire jumps to Kuat (route
// Corellia_Kuat, HS_Speed_Factor 5). The time is derived from the data alone (planet positions, the route, the slowest
// Hyperspace_Speed of the fleet, the faction factor, M) and must equal what GalacticSim does and what the original did:
// 26 ticks = 0.87 s at 30 ticks/s (retail trace: departure sample at tick 3038, arrival sample at 3064).
void test_foc_corellia_kuat_hop(const FocData& d) {
    std::printf("  FoC Corellia -> Kuat hop (G1)\n");
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, d.types, &ids);
    SimRng rng(0xC0FFEEu);
    GalacticSim s(d.rules, store, rng);
    CHECK(s.start(build_campaign_setup(d.db, d.rules, "Sandbox_Heart_Of_The_Maw_Empire")));
    const int32_t E = s.player_of_faction("Empire");
    const PlanetIndex cor = s.map().planets.find("Corellia"), kuat = s.map().planets.find("Kuat");
    CHECK(E >= 0 && s.players()[static_cast<size_t>(E)].human && cor != kInvalidPlanet && kuat != kInvalidPlanet);
    if (E < 0 || cor == kInvalidPlanet || kuat == kInvalidPlanet) return;
    s.update();  // GRC: frame 0's pre-step reveals the hop targets the human order needs
    // Data: positions (Planets.xml Galactic_Position), the route, the fleet's slowest Hyperspace_Speed, the faction factor, M.
    CHECK(s.planets()[cor].position.x == 30.0f && s.planets()[cor].position.y == -110.0f && s.planets()[cor].position.z == 10.0f);
    CHECK(s.planets()[kuat].position.x == 90.0f && s.planets()[kuat].position.y == -70.0f && s.planets()[kuat].position.z == 10.0f);
    float route = 0.0f, distance = 0.0f;
    for (uint32_t e = s.graph().edge_begin(cor); e < s.graph().edge_end(cor); ++e)
        if (s.graph().neighbour(e) == kuat && s.graph().route(e) != kNoRoute) {
            route = s.graph().speed_factor(e);
            distance = s.graph().distance(e);
        }
    CHECK(route == 5.0f);  // TradeRoutes.xml Corellia_Kuat HS_Speed_Factor
    CHECK(std::fabs(distance - 72.111f) < 1e-3f);  // sqrt(60^2 + 40^2)
    const std::vector<uint32_t> at = s.fleets_at(cor, E);
    CHECK(!at.empty());
    if (at.empty()) return;
    const uint32_t f = at[0];
    CHECK(s.move_fleet(E, f, kuat) == RejectReason::None);
    const FleetState& fl = s.fleets()[f];
    CHECK(fl.moving && fl.path.size() == 2 && fl.to == kuat);
    const ObjectHandle member = fl.contents.empty() ? ObjectHandle{} : fl.contents[0];  // the fleet may merge on arrival
    float slowest = 1.0e9f;
    for (size_t i = 0; i < fl.contents.size(); ++i) {
        const TypeInfo& t = d.rules.types[s.travel_type(fl.contents[i])];
        if (t.hyperspace && !fl.carried[i]) slowest = std::min(slowest, t.hyperspace_speed);
    }
    CHECK(slowest == 1.5f);  // Tartan Patrol Cruiser, Victory Destroyer
    const float factor = d.rules.factions[s.players()[static_cast<size_t>(E)].faction].hyperspace_speed_factor;
    const float m_orig = d.rules.constants.fleet_speed_multiplier;  // 5.0, what the original applies
    const float m = s.fleet_speed_multiplier();                      // what the port applies (5.0, or 1.0 while the flip waits)
    CHECK(factor == 0.07f && m_orig == 5.0f && (m == m_orig || m == 1.0f));
    // The original: v = 2.625 units/tick, 26 ticks (analyze_hops.py on the Q2 trace) = the derivation from the data with M = 5.
    const float v_orig = econ::hop_speed(econ::base_speed(slowest, factor, m_orig), route);
    const int32_t orig_ticks = frames_for_hop(s.planets()[cor].position, s.planets()[kuat].position, v_orig);
    CHECK(std::fabs(v_orig - kRetailHops[0].step) <= 5e-4f && orig_ticks == 26 && orig_ticks == kRetailHops[0].frames);
    // The port's sim must equal the same derivation with its own M.
    const float v = econ::hop_speed(econ::base_speed(slowest, factor, m), route);
    const int32_t expect = frames_for_hop(s.planets()[cor].position, s.planets()[kuat].position, v);
    CHECK(fl.hop_speed == v);
    int32_t t = 0;
    while (s.fleets()[f].alive && s.fleets()[f].moving && t < 100000) {
        s.update();
        ++t;
    }
    CHECK(t == expect);
    const uint32_t g = s.fleet_containing(member);
    CHECK(g != kNoFleet && s.fleets()[g].planet == kuat);
    std::printf("  FoC Corellia -> Kuat: %.3f units, v %.4f/tick, %d ticks = %.3f s at 30 ticks/s (original: %d ticks, %.3f s)%s\n",
                static_cast<double>(distance), static_cast<double>(v), t, static_cast<double>(t) / 30.0, orig_ticks,
                static_cast<double>(orig_ticks) / 30.0, t == orig_ticks ? "" : "  [port differs: strategic.cpp kApplyModeSpeedMultiplier]");
}

// Thrawn's Revenge (workshop 1125571106) through the VFS (mod folder over the FoC install), when present: M = 11.34 and
// the `|` OR groups of Required_Special_Structures (SG 1.3 step 14, NR_Government.xml / Research_Dummies.xml).
void test_oi_thrawns_revenge() {
#ifdef ALAMO_GALACTIC_TESTS_HAVE_VFS
    std::string tr = env_or("ALAMO_TR_DATA");
    std::string foc = env_or("ALAMO_FOC_INSTALL");
    if (tr.empty()) {
        // Same Steam library as a Steam FoC install: <lib>\steamapps\common\Star Wars Empire at War\corruption.
        const std::string f = env_or("ALAMO_FOC_DATA");
        const size_t at = f.find("\\steamapps\\common\\");
        if (at != std::string::npos) tr = f.substr(0, at) + "\\steamapps\\workshop\\content\\32470\\1125571106";
        if (foc.empty() && at != std::string::npos) foc = f;
    }
    if (foc.empty() && !tr.empty()) {
        const size_t at = tr.find("\\steamapps\\workshop\\");
        if (at != std::string::npos) foc = tr.substr(0, at) + "\\steamapps\\common\\Star Wars Empire at War\\corruption";
    }
    DirectoryProvider probe(tr.empty() ? std::string(".") : tr);
    if (tr.empty() || foc.empty() || !probe.exists("Data\\XML\\GameConstants.xml")) {
        std::printf("Thrawn's Revenge data not found (ALAMO_TR_DATA / Steam library): TR checks skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = foc;
    cfg.mod_paths = {tr};
    std::string err;
    std::unique_ptr<vfs::Vfs> fs = vfs::Vfs::create(cfg, &err);
    if (!fs) {
        std::printf("Thrawn's Revenge: VFS not created (%s): TR checks skipped\n", err.c_str());
        return;
    }
    std::vector<std::string> diag;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    GalacticRules rules;
    CHECK(db.load(*fs, {}, &diag));
    CHECK(raw.load(*fs, &diag));
    CHECK(types.build(raw, fs.get(), &diag));
    CHECK(load_galactic_rules(*fs, db, types, rules, &diag));
    CHECK(rules.constants.fleet_speed_multiplier == 11.34f);
    int32_t or_chains = 0;
    for (const TypeInfo& t : rules.types)
        for (const std::vector<uint32_t>& g : t.required_special_groups) or_chains += g.size() > 1 ? 1 : 0;
    CHECK(or_chains > 0);
    std::printf("  Thrawn's Revenge: %zu types, M %.2f, %d OR chains\n", rules.types.size(),
                static_cast<double>(rules.constants.fleet_speed_multiplier), or_chains);
#endif
}

void test_foc_campaign() {
    const std::string root = env_or("ALAMO_FOC_DATA");
    FocData d;
    std::string how;
    d.provider = testing::open_foc_data(root, how);
    if (!d.provider) {
        std::printf("ALAMO_FOC_DATA unset or without Data\\XML (loose, or a FoC install through the VFS): FoC campaign run skipped\n");
        return;
    }
    std::printf("  FoC data: %s\n", how.c_str());
    std::vector<std::string> diag;
    CHECK(d.db.load(*d.provider, {}, &diag));
    CHECK(d.raw.load(*d.provider, &diag));
    CHECK(d.types.build(d.raw, d.provider.get(), &diag));
    CHECK(load_galactic_rules(*d.provider, d.db, d.types, d.rules, &diag));
    CHECK(d.rules.constants.fiscal_cycle_seconds == 45.0f && d.rules.constants.credit_cap_per_planet == 40000.0f);
    CHECK(d.rules.constants.corruption_income_pct[3] == 4.5f && d.rules.constants.corruption_hyperspace_bonus == 1.5f);
    CHECK(d.rules.constants.political_income_curve.eval(0.5f) == 0.5f);
    const uint32_t uw = d.rules.find_faction("Underworld");
    CHECK(uw != kNoIndex && d.rules.factions[uw].benefits_from_corruption && d.rules.factions[uw].hyperspace_speed_factor == 0.07f);
    const uint32_t emp = d.rules.find_faction("Empire");
    CHECK(emp != kNoIndex && d.rules.factions[emp].corruption_price_mult[1] == 0.9f);
    const uint32_t hard = d.rules.find_difficulty("Hard_Default");
    CHECK(hard != kNoIndex && d.rules.difficulties[hard].credit_multiplier == 1.2f);
    const uint32_t cg = d.rules.find_type("Crusader_Gunship");
    CHECK(cg != kNoIndex && d.rules.types[cg].kind == UnitKind::SpaceUnit && d.rules.types[cg].build_cost == 1400 &&
          d.rules.types[cg].hyperspace_speed == 2.0f && d.rules.types[cg].required_star_base == 2);
    const uint32_t sb = d.rules.find_type("Underworld_Star_Base_1");
    CHECK(sb != kNoIndex && d.rules.types[sb].kind == UnitKind::StarBase && d.rules.types[sb].base_level == 1);
    test_oi_foc(d);  // OI galactic
    test_foc_player_fleet_moves(d);
    test_foc_corellia_kuat_hop(d);  // G1
    const int32_t days = 10;
    RunResult base;
    bool first = true;
    for (auto& [n, pf] : worker_configs()) {
        RunResult r = run_foc(d, pf, days, 150);
        if (first) {
            base = std::move(r);
            first = false;
            continue;
        }
        const CrcDivergence dv = first_divergence(base.log, r.log);
        if (dv.diverged) std::printf("FAIL FoC workers %u: %s\n", n, dv.message.c_str());
        CHECK(!dv.diverged);
    }
    base.golden_view.comments.push_back("S3 FoC Sandbox_Heart_Of_The_Maw_Underworld, 10 galactic days, every 150th frame + last");
    base.golden_view.comments.push_back("2026-10-28 OI galactic: M = 5.0, companies as transports, galactic stealth, buildability order, build counts");
    check_golden(base.golden_view, "galactic_foc_crc.txt");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps the output so far
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--golden-dir" && i + 1 < argc) g_golden_dir = argv[++i];
        else if (a == "--write-golden") g_write_golden = true;
        else if (a == "--quick") g_quick = true;  // unit / scenario tests only (no campaign CRC runs)
        else if (a == "--only" && i + 1 < argc) only = argv[++i];  // units | synthetic | foc (ctest runs them apart)
    }
    const bool all = only.empty();
    if (all || only == "units") {
        test_formulas();
        test_income();
        test_production();
        test_fleets();
        test_ownership();
        test_event_stream();
        test_a2_income_hook();
        test_a2_production_queries();
        test_a2_fleet_management();
        test_fleet_slots();
        test_a2_spawn_convert();
        test_a2_population_terrain();
        test_a2gal_population();
        test_a2gal_steal_order();
        test_a2gal_ai_blockade();
        test_a2_victory();
        test_a2_prerequisites_and_market_hook();
        test_diplomacy_and_build_locks();
        test_oi_speed_multiplier();
        test_retail_hops();  // G1
        test_grc_reveal();
        test_grc_corruption_transition();
        test_oi_company_transports();
        test_oi_stealth();
        test_oi_base_info();
        test_oi_buildability();
        test_oi_victory();
        test_tsw_planet_destroy();
    }
    if (!g_quick && (all || only == "synthetic")) test_synthetic_campaign();
    if (!g_quick && (all || only == "foc")) {
        test_foc_campaign();
        test_oi_thrawns_revenge();  // OI galactic
    }
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("alamo_galactic_economy_tests: all passed\n");
    return 0;
}
