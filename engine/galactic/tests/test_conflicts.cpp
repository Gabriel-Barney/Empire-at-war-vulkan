// alamo_galactic_conflict_tests: S10 battle transitions, galactic side (docs/re/BATTLE_TRANSITIONS.md).
//   1. contrast table parsed from PGAICommands-style Lua text;
//   2. auto-resolve hand-computed cases (exchange of fire, winner, budgets, survival-roll RNG order,
//      degenerate space case, Super rule, transports, starbase downgrade);
//   3. the space force split (Â§2.3) hand-computed;
//   4. conflict flow on a synthetic galaxy: AI vs AI auto-resolve on fleet arrival, human Fight with a
//      tactical host (finish_battle write-back + retreat), human Pending choice (galaxy paused until
//      choose()), land invasion, determinism (two runs give the same conflict + galactic digests).
#include <algorithm>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/galactic/auto_resolve.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/special_systems.hpp"
#include "alamo/objects/game_object_types.hpp"

using namespace alamo;
using namespace alamo::galactic;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

constexpr uint64_t kCatFighter = 0x1, kCatCapital = 0x2, kCatInfantry = 0x4, kCatStructure = 0x8, kCatSuper = 0x10;

ContrastTable make_contrast() {
    ContrastTable t;
    ContrastRow f;
    f.enemy = "Fighter";
    f.enemy_mask = kCatFighter;
    f.friendly = {{"Fighter", kCatFighter, 1.0f}, {"Capital", kCatCapital, 0.5f}};
    ContrastRow c;
    c.enemy = "Capital";
    c.enemy_mask = kCatCapital;
    c.friendly = {{"Fighter", kCatFighter, 1.0f}, {"Capital", kCatCapital, 1.0f}};
    ContrastRow i;
    i.enemy = "Infantry";
    i.enemy_mask = kCatInfantry;
    i.friendly = {{"Infantry", kCatInfantry, 1.0f}, {"Structure", kCatStructure, 1.0f}};
    ContrastRow s;
    s.enemy = "Structure";
    s.enemy_mask = kCatStructure;
    s.friendly = {{"Infantry", kCatInfantry, 1.0f}};
    t.rows = {f, c, i, s};
    return t;
}

ArCombatant unit(int32_t owner, float p, uint64_t cats, const char* name = "Ship") {
    ArCombatant c;
    c.owner = owner;
    c.own_power = p;
    ArPiece piece;
    piece.name = name;
    piece.categories = cats;
    piece.power = p;
    c.pieces.push_back(piece);
    return c;
}

// ---- 1. contrast parse ----------------------------------------------------------------------------------------
void test_contrast_parse() {
    const char* src =
        "function Foo()\n end\n"
        "function Set_Contrast_Values()\n"
        "\t_e_cnt = 1;\n"
        "\tEnemyContrastTypes[_e_cnt] = \"Fighter\"\n"
        "\tFriendlyContrastTypeNames = {\"AntiFighter\", \"Bomber\", \"Fighter\"}\n"
        "\tFriendlyContrastWeights =\t{4.0, 0.25, 1.0}\n"
        "\tFriendlyContrastTypes[_e_cnt] = WeightedTypeList.Create()\n"
        "\tFriendlyContrastTypes[_e_cnt].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)\n"
        "\t_e_cnt = _e_cnt+1;\n"
        "\tEnemyContrastTypes[_e_cnt] = \"Transport\" -- comment\n"
        "\tFriendlyContrastTypeNames = {\"Fighter\", \"Some_Unit\", }\n"
        "\tFriendlyContrastWeights =\t{1.0, 2.5}\n"
        "\tFriendlyContrastTypes[_e_cnt].Parse(FriendlyContrastTypeNames, FriendlyContrastWeights)\n"
        "end\n"
        "EnemyContrastTypes[9] = \"Ignored\"\n";
    auto mask = [](std::string_view n) -> uint64_t {
        if (n == "Fighter") return 1;
        if (n == "Bomber") return 2;
        if (n == "AntiFighter") return 4;
        if (n == "Transport") return 8;
        return 0;
    };
    ContrastTable t;
    std::vector<std::string> diag;
    CHECK(t.parse_lua(src, mask, &diag));
    CHECK(t.rows.size() == 2);
    if (t.rows.size() == 2) {
        CHECK(t.rows[0].enemy == "Fighter" && t.rows[0].enemy_mask == 1);
        CHECK(t.rows[0].friendly.size() == 3 && t.rows[0].friendly[0].weight == 4.0f && t.rows[0].friendly[1].weight == 0.25f);
        CHECK(t.rows[1].enemy == "Transport" && t.rows[1].friendly.size() == 2);
        CHECK(t.contrast(0, "X", 4) == 4.0f);       // AntiFighter category, first entry
        CHECK(t.contrast(0, "X", 2 | 1) == 0.25f);  // Bomber listed before Fighter
        CHECK(t.contrast(1, "some_unit", 0) == 2.5f);  // type-name entry, case-insensitive
        CHECK(t.contrast(1, "Other", 2) == 0.0f);
    }
}

// ---- 2. auto-resolve ------------------------------------------------------------------------------------------
void test_auto_resolve_basic() {
    const ContrastTable ct = make_contrast();
    AutoResolveConstants k;
    AutoResolveInput in;
    in.space = true;
    in.attacker = 1;
    in.combatants = {unit(0, 1000.0f, kCatCapital), unit(1, 1000.0f, kCatCapital), unit(1, 1000.0f, kCatCapital)};
    in.players = {{0, true, true}, {1, true, true}};
    SimRng rng(4242u), copy(4242u);
    const AutoResolveResult r = auto_resolve(in, ct, k, rng);
    CHECK(r.valid && !r.degenerate);
    CHECK(r.side_player[0] == 0 && r.side_player[1] == 1);
    // Vectors [land, space, Fighter, Capital, Infantry, Structure].
    CHECK(r.vec[0][1] == 1000.0 && r.vec[0][3] == 1000.0 && r.vec[1][1] == 2000.0 && r.vec[1][3] == 2000.0);
    // Side 0 fires 1000 at R1 Capital 2000: c = 1, a = 1000, total 2000 - 1000.
    CHECK(r.rem[1][3] == 1000.0 && r.rem[1][1] == 1000.0);
    // Side 1: first shot a = 1000 (R0 Capital -> 0, total -> 0), second shot finds no target: total -= 1000.
    CHECK(r.rem[0][3] == 0.0 && r.rem[0][1] == -1000.0);
    CHECK(r.strength[0] == 0.0 && r.strength[1] == 2000.0);
    CHECK(r.winner == 1 && r.loser == 0);
    // Budgets: winner 1000 + 1000 x 0.25 = 1250; loser -1000 + 2000 x 0.2 = -600.
    CHECK(r.budget[1] == 1000.0 + 1000.0 * (1.0 - static_cast<double>(0.75f)));
    CHECK(r.budget[0] == -1000.0 + 2000.0 * (1.0 - static_cast<double>(0.80f)));
    // Winner first: one draw int(0, 1) picks the survivor (1250 - 333 > 0 -> pays 1000, then 250 - 333 < 0).
    const int32_t first = copy.range_int(0, 1);
    CHECK(r.survives[0] == 0);
    CHECK(r.survives[static_cast<size_t>(1 + first)] == 1 && r.survives[static_cast<size_t>(2 - first)] == 0);
    CHECK(r.draws == 1 && rng.state() == copy.state());  // the loser's single pick needs no draw
    CHECK(r.retreating == -1 && r.losses[0] == 1 && r.losses[1] == 1);
    // Determinism: same input + seed -> same result digest.
    SimRng rng2(4242u);
    const AutoResolveResult r2 = auto_resolve(in, ct, k, rng2);
    StateHasher h1, h2;
    r.hash(h1);
    r2.hash(h2);
    CHECK(h1.digest() == h2.digest());
}

void test_auto_resolve_rules() {
    const ContrastTable ct = make_contrast();
    AutoResolveConstants k;
    {  // degenerate space case: no power at all -> more combatants wins, the loser retreats, nothing dies
        AutoResolveInput in;
        in.space = true;
        in.attacker = 1;
        in.combatants = {unit(0, 0.0f, kCatCapital), unit(1, 0.0f, kCatCapital), unit(1, 0.0f, kCatCapital)};
        SimRng rng(1u);
        const AutoResolveResult r = auto_resolve(in, ct, k, rng);
        CHECK(r.degenerate && r.winner == 1 && r.loser == 0 && r.retreating == 0 && r.draws == 0);
        CHECK(r.survives[0] && r.survives[1] && r.survives[2]);
        in.combatants.pop_back();  // tie: the side that is not the attacker wins
        const AutoResolveResult t = auto_resolve(in, ct, k, rng);
        CHECK(t.winner == 0 && t.loser == 1);
    }
    {  // Super without an answer: the other side retreats and loses even when stronger
        AutoResolveInput in;
        in.space = true;
        in.attacker = 0;
        ArCombatant sup = unit(0, 100.0f, kCatSuper | kCatCapital);
        sup.super = true;
        in.combatants = {sup, unit(1, 5000.0f, kCatCapital)};
        SimRng rng(7u);
        const AutoResolveResult r = auto_resolve(in, ct, k, rng);
        CHECK(r.winner == 0 && r.loser == 1 && r.retreating == 1);
        in.combatants[1].super_killer = true;  // answered: strength decides
        const AutoResolveResult s = auto_resolve(in, ct, k, rng);
        CHECK(s.winner == 1 && s.retreating == -1);
    }
    {  // exact tie -> the attacker; both AI and one side not playable -> it loses outright
        AutoResolveInput in;
        in.space = false;
        in.attacker = 1;
        in.combatants = {unit(0, 100.0f, kCatInfantry, "Inf"), unit(1, 100.0f, kCatInfantry, "Inf")};
        SimRng rng(9u);
        const AutoResolveResult r = auto_resolve(in, ct, k, rng);
        CHECK(r.strength[0] == r.strength[1] && r.winner == 1);
        in.combatants = {unit(0, 100.0f, kCatInfantry, "Inf"), unit(1, 10.0f, kCatInfantry, "Inf")};
        const AutoResolveResult s = auto_resolve(in, ct, k, rng);
        CHECK(s.winner == 0);
        // Both sides keep positive strength (no contrast between them), both AI, side 0 not playable: it loses.
        in.combatants = {unit(0, 200.0f, 0, "Odd"), unit(1, 150.0f, kCatStructure, "Wall")};
        in.players = {{0, true, false}, {1, true, true}};
        const AutoResolveResult u = auto_resolve(in, ct, k, rng);
        CHECK(u.strength[0] == 50.0 && u.strength[1] == 150.0);
        CHECK(u.winner == 1 && u.budget[0] == 0.0 && !u.survives[0]);
    }
    {  // heroes are picked first; the winner keeps its weakest unit when nothing else survives
        AutoResolveInput in;
        in.space = true;
        in.attacker = 1;
        ArCombatant hero = unit(1, 300.0f, kCatCapital);
        hero.hero = true;
        in.combatants = {unit(0, 400.0f, kCatCapital), unit(1, 500.0f, kCatCapital), hero};
        SimRng rng(11u), copy(11u);
        const AutoResolveResult r = auto_resolve(in, ct, k, rng);
        // Side 0 fires 400 at R1 Capital 800: a = 400, R1 = 400 / total 400. Side 1: 500 at 400 -> a = 400,
        // R0 Capital 0, total 400 - 400 - 0.2 x 500 = -100; hero 300: no target -> total -400.
        CHECK(r.rem[1][1] == 400.0 && r.rem[0][1] == -400.0);
        CHECK(r.winner == 1);
        // Winner budget 400 + 400 x 0.25 = 500: hero first (500 - 100 > 0 -> 200 left), then the other
        // (no draw: one left; 200 - 166.7 > 0 -> survives).
        CHECK(r.survives[2] == 1 && r.survives[1] == 1 && r.draws == 0 && rng.state() == copy.state());
        // Zero budget for the winner: it still keeps its weakest unit.
        AutoResolveConstants all;
        all.winner_attrition = 1.0f;
        in.combatants = {unit(0, 1000.0f, kCatCapital), unit(1, 900.0f, kCatFighter), unit(1, 800.0f, kCatFighter)};
        SimRng rng2(13u);
        const AutoResolveResult w = auto_resolve(in, ct, all, rng2);
        if (w.winner == 1) {
            CHECK(w.survives[2] == 1);  // weakest (800)
        }
    }
    {  // transports (space, loser cargo) and starbase downgrade
        AutoResolveInput in;
        in.space = true;
        in.attacker = 1;
        ArCombatant sb = unit(0, 600.0f, kCatStructure, "SB2");
        sb.starbase = {{12u, 600.0f}, {11u, 300.0f}};
        in.combatants = {unit(0, 100.0f, kCatCapital), unit(1, 3000.0f, kCatCapital), sb};
        in.cargo = {{0, false}, {0, true}, {0, false}, {0, false}};
        SimRng rng(21u);
        const AutoResolveResult r = auto_resolve(in, ct, k, rng);
        CHECK(r.winner == 1 && r.loser == 0);
        // k = 4 carried units: m = round(0.667 x 4) = 3 non-hero survivors; the hero always survives.
        CHECK(r.cargo_survives[0] == 1 && r.cargo_survives[1] == 1 && r.cargo_survives[2] == 1 && r.cargo_survives[3] == 1);
        CHECK(r.retreating == 0);
        // Retreat flagged: loser budget uses 0.65. The starbase falls to the highest affordable level.
        const double b = r.budget[0];
        CHECK(b == r.rem[0][1] + (r.vec[0][1] - r.rem[0][1]) * (1.0 - static_cast<double>(0.65f)));
        const uint32_t sbt = r.starbase_type[2];
        if (600.0 * static_cast<double>(k.attrition_allowance) <= b) CHECK(sbt == 12u || !r.survives[2]);
        else if (300.0 * static_cast<double>(k.attrition_allowance) <= b) CHECK(sbt == 11u || !r.survives[2]);
        in.cargo.resize(1);  // k = 1 -> m = 0
        const AutoResolveResult one = auto_resolve(in, ct, k, rng);
        CHECK(one.cargo_survives[0] == 0);
    }
}

// ---- 3. force split -----------------------------------------------------------------------------------------------
enum TypeId : uint32_t { tPlanetA, tPlanetB, tPlanetC, tSB1, tSB2, tFrigate, tFighters, tInfantry, tBarracks, tCorvette, tDeathStar, tTypeCount };
enum Row : PlanetIndex { rA = 0, rB = 1, rC = 2 };
enum PlayerId : int32_t { pEmpire = 0, pUnder = 1, pNeutral = 2 };

GalacticRules make_rules() {
    GalacticRules r;
    FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    FactionRules und;
    und.name = "Underworld";
    und.can_control_planets = und.is_playable = true;
    FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, und, neu};
    r.types.resize(tTypeCount);
    const char* pn[] = {"PlanetA", "PlanetB", "PlanetC"};
    for (uint32_t i = 0; i < 3; ++i) {
        r.types[i].name = pn[i];
        r.types[i].kind = UnitKind::Planet;
        r.types[i].credit_value = 10;
    }
    auto unit = [&r](uint32_t id, const char* name, UnitKind kind, float power) -> TypeInfo& {
        TypeInfo& t = r.types[id];
        t.name = name;
        t.kind = kind;
        t.ai_combat_power = power;
        t.build_cost = 100;
        t.build_time_seconds = 10;
        return t;
    };
    unit(tSB1, "SB_1", UnitKind::StarBase, 300.0f).base_level = 1;
    unit(tSB2, "SB_2", UnitKind::StarBase, 600.0f).base_level = 2;
    TypeInfo& fr = unit(tFrigate, "Frigate", UnitKind::SpaceUnit, 1000.0f);
    fr.hyperspace = true;
    fr.hyperspace_speed = 1.5f;
    unit(tFighters, "Fighters", UnitKind::Squadron, 0.0f);
    TypeInfo& inf = unit(tInfantry, "Infantry", UnitKind::GroundUnit, 100.0f);
    inf.hyperspace = true;
    inf.hyperspace_speed = 1.5f;
    unit(tBarracks, "Barracks", UnitKind::GroundStructure, 200.0f);
    TypeInfo& co = unit(tCorvette, "Corvette", UnitKind::SpaceUnit, 400.0f);
    co.hyperspace = true;
    co.hyperspace_speed = 2.0f;
    TypeInfo& ds = unit(tDeathStar, "Death_Star", UnitKind::SpaceUnit, 5000.0f);  // the galactic super weapon
    ds.hyperspace = true;
    ds.hyperspace_speed = 1.0f;
    ds.planet_destruction = true;
    return r;
}

ConflictRules make_conflict_rules() {
    ConflictRules c;
    c.contrast = make_contrast();
    c.super_category = kCatSuper;
    c.types.resize(tTypeCount);
    c.types[tSB1].categories = c.types[tSB2].categories = kCatStructure;
    c.types[tSB2].lower_starbase = tSB1;
    c.types[tFrigate].categories = kCatCapital;
    c.types[tFrigate].population = 2;
    c.types[tCorvette].categories = kCatCapital;
    c.types[tCorvette].population = 1;
    c.types[tFighters].members = {{tCorvette, 2}};  // a "squadron" container (members' power counts)
    c.types[tFighters].categories = kCatFighter;
    c.types[tInfantry].categories = kCatInfantry;
    c.types[tBarracks].categories = kCatStructure;
    c.types[tDeathStar].categories = kCatSuper;
    c.types[tDeathStar].population = 1;
    for (uint32_t p : {tPlanetA, tPlanetB, tPlanetC}) c.types[p].terrain = 0;  // Temperate
    ConflictFactionInfo f;
    f.space_unit_cap = 10.0f;
    c.factions = {f, f, f};
    return c;
}

GalacticSetup make_setup(std::vector<StartingForce> forces, bool empire_human, bool route_bc = true) {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "S10";
    const char* names[] = {"A", "B", "C"};
    const Vec3 pos[] = {{0, 0, 0}, {60, 0, 0}, {120, 0, 0}};
    for (uint32_t i = 0; i < 3; ++i) {
        m.planets.add(names[i]);
        m.planets.position[i] = pos[i];
        m.planets.flags[i] = static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    auto route = [&m](const char* n, PlanetIndex a, PlanetIndex b) {
        const uint32_t r = m.routes.add(n);
        m.routes.point_a[r] = a;
        m.routes.point_b[r] = b;
        m.routes.hs_speed_factor[r] = 5.0f;
    };
    route("A_B", rA, rB);
    if (route_bc) route("B_C", rB, rC);
    else m.max_fleet_move_distance = 0.0f;  // no free (route-less) hop for the path finder either
    s.planet_type = {tPlanetA, tPlanetB, tPlanetC};
    PlayerSetup e;
    e.faction = 0;
    e.human = empire_human;
    e.ai = !empire_human;
    e.credits = 5000.0f;
    PlayerSetup u;
    u.faction = 1;
    u.ai = true;
    u.credits = 5000.0f;
    PlayerSetup n;
    n.faction = 2;
    s.players = {e, u, n};
    s.forces = std::move(forces);
    return s;
}

struct World {
    GalacticRules rules = make_rules();
    ConflictRules crules = make_conflict_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{0x5150u};
    GalacticSim sim{rules, store, rng};
    std::unique_ptr<ConflictManager> cm;
    World(std::vector<StartingForce> forces, bool empire_human, bool route_bc = true) {
        std::vector<std::string> diag;
        CHECK(sim.start(make_setup(std::move(forces), empire_human, route_bc), &diag));
        sim.set_emit_events(true);
        cm = std::make_unique<ConflictManager>(sim, crules, rng);
        cm->attach();
    }
    uint32_t fleet(PlanetIndex p, int32_t player) const {
        const std::vector<uint32_t> f = sim.fleets_at(p, player);
        return f.empty() ? kNoFleet : f[0];
    }
    // Moves the Underworld fleet at C to B and runs until it arrived (conflict queued + serviced).
    int arrive_at_b() {
        const uint32_t f = fleet(rC, pUnder);
        CHECK(f != kNoFleet);
        CHECK(sim.move_fleet(pUnder, f, rB) == commands::RejectReason::None);
        int n = 0;
        while (n < 2000 && sim.fleets()[f].alive && sim.fleets()[f].moving) {
            sim.update();
            ++n;
        }
        return n;
    }
    uint64_t digest() const {
        StateHasher h;
        sim.hash_state(h);
        cm->hash(h);
        return h.digest();
    }
};

void test_force_split() {
    World w({{pEmpire, rA, tFrigate}}, false);
    ConflictRules& c = w.crules;
    c.factions[0].space_unit_cap = 10.0f;
    // Units: container x2 (Fighters), Frigate (pop 2) x3, Corvette (pop 1) x4, interleaved.
    std::vector<objects::ObjectHandle> units;
    auto mk = [&w](uint32_t t) {
        objects::SpawnParams sp;
        sp.type = t;
        sp.owner = pEmpire;
        return w.store.create(sp);
    };
    const uint32_t order[] = {tFrigate, tFighters, tCorvette, tFrigate, tCorvette, tCorvette, tFighters, tFrigate, tCorvette};
    for (uint32_t t : order) units.push_back(mk(t));
    BattleForces f;
    w.cm->split_space_forces(pEmpire, units, f);
    // Containers: 2 slots (cap 8). Pass A: last Frigate (cap 6), last Corvette (cap 5). Pass B (n = 5):
    // Frigate floor(2/5 x 5 + 0.5) / 2 = 1, Corvette floor(3/5 x 5 + 0.5) / 1 = 3 -> used 5, cap 0.
    // Reserve: the first Frigate only.
    CHECK(f.start.size() == 8 && f.reserve.size() == 1);
    CHECK(!f.reserve.empty() && f.reserve[0] == units[0]);
    // Smaller cap: 3 -> containers 2 (cap 1), pass A: Frigate (2) does not fit, Corvette fits (cap 0).
    c.factions[0].space_unit_cap = 3.0f;
    w.cm->split_space_forces(pEmpire, units, f);
    CHECK(f.start.size() == 3 && f.reserve.size() == 6);
}

// ---- 4. conflict flow ---------------------------------------------------------------------------------------------
void test_ai_auto_resolve() {
    World w({{pEmpire, rB, tFrigate}, {pEmpire, rB, tSB1}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}}, false);
    w.arrive_at_b();
    const auto& log = w.cm->log();
    CHECK(log.size() == 1);
    if (log.size() == 1) {
        CHECK(log[0].how == 1 && log[0].attacker == pUnder && log[0].defender == pEmpire);
        CHECK(log[0].winner == pUnder || log[0].winner == pEmpire);
        std::printf("ai auto-resolve: %s\n", log[0].text(w.sim).c_str());
    }
    CHECK(w.cm->space_queue().empty() && w.cm->state() == ConflictState::Idle);
    CHECK(!w.sim.paused() && !w.sim.planets()[rB].in_battle);
    // Story events: BattleWon + BattleLost (+ BattleRetreat when someone retreated).
    int won = 0, lost = 0;
    for (const GalacticEvent& e : w.sim.events()) {
        won += e.kind == GalacticEventKind::BattleWon;
        lost += e.kind == GalacticEventKind::BattleLost;
    }
    CHECK(won == 1 && lost == 1);
    // The loser has no combat ship left in orbit of B (destroyed or retreating).
    const int32_t loser = log.empty() ? -1 : log[0].loser;
    for (uint32_t f : w.sim.fleets_at(rB, loser)) CHECK(w.sim.fleets()[f].moving || w.sim.fleets()[f].contents.empty());
    // Determinism.
    World w2({{pEmpire, rB, tFrigate}, {pEmpire, rB, tSB1}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}}, false);
    w2.arrive_at_b();
    CHECK(w.digest() == w2.digest());
}

// BT §4.4 "Loss records for carried units": a space auto-resolve records one loss per carried unit killed by the
// transport roll (the port's one-unit transports, ar_cargo_), plus one per combatant killed by the survival roll.
void test_cargo_losses() {
    World w({{pEmpire, rB, tFrigate}, {pEmpire, rB, tFrigate}, {pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate},
             {pUnder, rC, tInfantry}, {pUnder, rC, tInfantry}, {pUnder, rC, tInfantry}},
            false);
    std::vector<objects::ObjectHandle> ground;
    for (objects::ObjectHandle h : w.sim.planets()[rC].surface)
        if (w.store.alive(h) && w.store.owner(h) == pUnder) ground.push_back(h);
    if (!ground.empty()) CHECK(w.sim.launch_units(pUnder, rC, ground) == commands::RejectReason::None);
    std::vector<objects::ObjectHandle> all;
    for (int32_t p : {pEmpire, pUnder})
        for (PlanetIndex pl : {rB, rC})
            for (uint32_t f : w.sim.fleets_at(pl, p))
                for (objects::ObjectHandle h : w.sim.fleets()[f].contents) all.push_back(h);
    std::vector<int32_t> owner;
    std::vector<uint8_t> is_ground;
    for (objects::ObjectHandle h : all) {  // recorded before the battle: dead handles are not read
        owner.push_back(w.store.owner(h));
        is_ground.push_back(w.rules.types[w.store.type(h)].kind == UnitKind::GroundUnit ? 1 : 0);
    }
    uint32_t cargo_in_orbit = 0;
    for (uint8_t g : is_ground) cargo_in_orbit += g;
    CHECK(all.size() == 7 && cargo_in_orbit == 3);
    const std::vector<uint32_t> movers = w.sim.fleets_at(rC, pUnder);
    for (uint32_t f : movers) CHECK(w.sim.move_fleet(pUnder, f, rB) == commands::RejectReason::None);
    for (int n = 0; n < 2000 && w.cm->log().empty(); ++n) w.sim.update();
    CHECK(w.cm->log().size() == 1);
    if (w.cm->log().size() != 1) return;
    const ConflictLogEntry& e = w.cm->log()[0];
    CHECK(e.kind == ConflictKind::Space && e.how == 1 && e.attacker == pUnder && e.winner == pEmpire);
    uint32_t dead_att = 0, dead_def = 0, dead_cargo = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (w.store.alive(all[i])) continue;
        (owner[i] == pUnder ? dead_att : dead_def)++;
        dead_cargo += is_ground[i];
    }
    // Loser cargo k = 3: m = round(2/3 x 3) = 2 survive, 1 dies in the transport roll and is recorded.
    CHECK(dead_cargo == 1);
    CHECK(e.attacker_losses == dead_att && e.defender_losses == dead_def);
}

void test_human_fight() {
    World w({{pEmpire, rA, tFrigate}, {pEmpire, rB, tFrigate}, {pEmpire, rB, tCorvette}, {pUnder, rC, tFrigate}}, true);
    w.cm->config().human_choice = BattleChoice::Fight;
    BattleRequest got;
    int started = 0;
    w.cm->start_battle = [&](const BattleRequest& r) {
        got = r;
        ++started;
        return true;
    };
    w.arrive_at_b();
    CHECK(started == 1 && w.cm->battle_active());
    CHECK(got.info.planet == rB && got.info.attacker == pUnder && got.info.defender == pEmpire);
    CHECK(got.forces.size() == 2 && got.forces[0].player == pEmpire && got.forces[1].player == pUnder);
    CHECK(got.forces[0].start.size() == 2 && got.forces[1].start.size() == 1);
    if (got.forces.size() != 2 || got.forces[0].start.size() < 2 || got.forces[1].start.empty()) return;
    CHECK(got.map == "_Space_Temperate1.ted");  // no planet map field, no picker: fallback
    CHECK(w.sim.planets()[rB].in_battle && !w.sim.paused());
    // Battle: the attacker loses its frigate; the defender lost the corvette.
    BattleResult res;
    res.winner = pEmpire;
    res.destroyed = {got.forces[1].start[0], got.forces[0].start[1]};
    w.cm->finish_battle(res);
    CHECK(!w.cm->battle_active() && w.cm->space_queue().empty());
    CHECK(w.cm->log().size() == 1 && w.cm->log()[0].how == 2 && w.cm->log()[0].attacker_losses == 1 &&
          w.cm->log()[0].defender_losses == 1);
    CHECK(!w.store.alive(got.forces[1].start[0]) && !w.store.alive(got.forces[0].start[1]));
    CHECK(w.store.alive(got.forces[0].start[0]));
    CHECK(!w.sim.planets()[rB].in_battle);
}

void test_human_retreat_after_battle() {
    // The attacker loses with survivors left in reserve: its fleet retreats (OI galactic, TACTICAL_GAPS 4.5: nobody
    // controls an orbit or holds a base, so the nearest neutral-owned planet wins: A (60) - C is Underworld's).
    World w({{pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate}, {pUnder, rC, tCorvette}}, true);
    w.cm->config().human_choice = BattleChoice::Fight;
    BattleRequest got;
    w.cm->start_battle = [&](const BattleRequest& r) {
        got = r;
        return true;
    };
    w.arrive_at_b();
    CHECK(w.cm->battle_active());
    if (got.forces.size() != 2 || got.forces[1].start.empty()) return;
    BattleResult res;
    res.winner = pEmpire;
    res.destroyed = {got.forces[1].start[0]};
    w.cm->finish_battle(res);
    const std::vector<uint32_t> f = w.sim.fleets_at(rB, pUnder);
    bool moving_to_c = false;
    for (const FleetState& fl : w.sim.fleets())
        if (fl.alive && fl.owner == pUnder && fl.moving && fl.to == rA) moving_to_c = true;
    CHECK(moving_to_c);
    int retreat_events = 0;
    for (const GalacticEvent& e : w.sim.events()) retreat_events += e.kind == GalacticEventKind::BattleRetreat;
    CHECK(retreat_events == 0);  // R = -1: the loser's fleets retreat without a retreat event
}

void test_pending_choice() {
    World w({{pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}}, true);
    w.cm->decide = [](const ConflictInfo&) { return BattleChoice::Pending; };
    w.arrive_at_b();
    CHECK(w.cm->state() == ConflictState::Vote && w.sim.paused());
    const int32_t frame = w.sim.frame();
    for (int i = 0; i < 10; ++i) w.sim.update();
    CHECK(w.sim.frame() == frame && w.cm->state() == ConflictState::Vote);  // galaxy frozen while deciding
    w.cm->choose(BattleChoice::AutoResolve);
    w.sim.update();  // Auto-Resolve = state 6 + force (GALACTIC_DIALOGS §1.5): set up and resolved in this tick
    CHECK(w.cm->log().size() == 1 && w.cm->log()[0].how == 1);
    CHECK(!w.sim.paused() && w.cm->state() == ConflictState::Idle && w.sim.frame() == frame);
    w.sim.update();
    CHECK(w.sim.frame() == frame + 1);
}

// L1/S4 integrator ask (3): GalacticSim::change_owner on a planet (GE 4.2 "clears the conflict") also drops the planet's
// queued conflict at the next service tail (logged, how 0); the active head stays.
void test_change_owner_drops_queued() {
    World w({{pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}, {pEmpire, rA, tFrigate}, {pUnder, rA, tFrigate}},
            true);
    w.cm->decide = [](const ConflictInfo&) { return BattleChoice::Pending; };
    w.arrive_at_b();
    CHECK(w.cm->state() == ConflictState::Vote && w.cm->space_queue().size() == 1);
    CHECK(w.cm->initiate_space_conflict(rA, pUnder));
    CHECK(w.cm->space_queue().size() == 2 && w.sim.planets()[rA].in_battle);
    const size_t logged = w.cm->log().size();
    CHECK(w.sim.change_owner(w.sim.planets()[rA].object, pUnder));
    CHECK(!w.sim.planets()[rA].in_battle && w.cm->space_queue().size() == 2);
    w.sim.update();  // paused (Vote): the service tail still runs
    CHECK(w.cm->space_queue().size() == 1 && w.cm->space_queue()[0].planet == rB && w.cm->state() == ConflictState::Vote);
    CHECK(w.cm->log().size() == logged + 1 && w.cm->log().back().planet == rA && w.cm->log().back().how == 0);
    CHECK(!w.sim.planets()[rA].in_battle && w.sim.planets()[rB].in_battle);
}

// P4 sim: Set_Retreating (BT §4.6) during the pending choice and during a running auto-resolve.
void test_p4_set_retreating() {
    {
        World w({{pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}}, true);
        w.cm->decide = [](const ConflictInfo&) { return BattleChoice::Pending; };
        w.arrive_at_b();
        CHECK(w.cm->state() == ConflictState::Vote);
        CHECK(!w.cm->set_retreating(pEmpire));   // before Begin: refused (0x80670003)
        CHECK(!w.cm->set_retreating(pNeutral));  // not a side
        // SP Auto-Resolve is forced (GALACTIC_DIALOGS §1.5): resolved in the same tick, so no window for a
        // withdrawal (that is the MP "Always" dialog's Withdraw, §3.2, not modelled).
        w.cm->choose(BattleChoice::AutoResolve);
        w.sim.update();
        CHECK(w.cm->log().size() == 1 && w.cm->log()[0].how == 1 && w.cm->state() == ConflictState::Idle);
        CHECK(!w.cm->set_retreating(pUnder));  // nothing active
    }
    {
        World w({{pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}}, true);
        w.cm->decide = [](const ConflictInfo&) { return BattleChoice::Pending; };
        w.arrive_at_b();
        w.cm->choose(BattleChoice::Retreat);  // the pending dialog's Retreat (the human defender)
        w.sim.update();
        w.sim.update();
        CHECK(w.cm->log().size() == 1 && w.cm->log()[0].retreating == pEmpire && w.cm->log()[0].winner == pUnder);
    }
}

// P4 sim: a running tactical battle resolved by auto-resolve (BT §4.5): deaths so far removed, forced, from tactical.
void test_p4_resolve_battle() {
    World w({{pEmpire, rA, tFrigate}, {pEmpire, rB, tFrigate}, {pEmpire, rB, tCorvette}, {pUnder, rC, tFrigate}}, true);
    w.cm->config().human_choice = BattleChoice::Fight;
    BattleRequest got;
    w.cm->start_battle = [&](const BattleRequest& r) {
        got = r;
        return true;
    };
    BattleResult none;
    CHECK(w.cm->resolve_battle(none) == kNoPlayer);  // no battle
    w.arrive_at_b();
    CHECK(w.cm->battle_active());
    if (got.forces.size() != 2 || got.forces[0].start.size() < 2) return;
    BattleResult so_far;
    so_far.destroyed = {got.forces[0].start[1]};  // the defender's corvette died in the battle
    const int32_t winner = w.cm->resolve_battle(so_far);
    CHECK(winner == pEmpire || winner == pUnder);
    CHECK(!w.store.alive(got.forces[0].start[1]) && !w.cm->battle_active() && w.cm->space_queue().empty());
    CHECK(w.cm->log().size() == 1 && w.cm->log()[0].how == 1 && w.cm->log()[0].winner == winner);
    CHECK(!w.sim.planets()[rB].in_battle && !w.sim.paused());
    // Deterministic: the same flow again gives the same digest.
    World v({{pEmpire, rA, tFrigate}, {pEmpire, rB, tFrigate}, {pEmpire, rB, tCorvette}, {pUnder, rC, tFrigate}}, true);
    v.cm->config().human_choice = BattleChoice::Fight;
    BattleRequest got2;
    v.cm->start_battle = [&](const BattleRequest& r) {
        got2 = r;
        return true;
    };
    v.arrive_at_b();
    BattleResult so_far2;
    if (got2.forces.size() == 2 && got2.forces[0].start.size() >= 2) so_far2.destroyed = {got2.forces[0].start[1]};
    CHECK(v.cm->resolve_battle(so_far2) == winner && v.digest() == w.digest());
}

void test_land_invasion() {
    World w({{pEmpire, rB, tInfantry}, {pUnder, rB, tFrigate}, {pUnder, rB, tInfantry}, {pUnder, rB, tInfantry},
             {pUnder, rB, tInfantry}},
            false);
    // The Underworld infantry spawned in orbit (StartingForce ground units at a planet held by... check).
    const std::vector<uint32_t> uf = w.sim.fleets_at(rB, pUnder);
    std::vector<objects::ObjectHandle> invaders;
    for (uint32_t f : uf)
        for (objects::ObjectHandle h : w.sim.fleets()[f].contents)
            if (w.rules.types[w.store.type(h)].kind == UnitKind::GroundUnit) invaders.push_back(h);
    if (invaders.empty()) {
        // Starting ground forces land on the surface: move them up into orbit first.
        std::vector<objects::ObjectHandle> mine;
        for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
            if (w.store.owner(h) == pUnder) mine.push_back(h);
        CHECK(w.sim.launch_units(pUnder, rB, mine) == commands::RejectReason::None);
        invaders = mine;
    }
    CHECK(invaders.size() == 3);
    // There is also an orbit conflict at B (Underworld frigate vs nothing in orbit: no defender fleet).
    CHECK(w.sim.invade(pUnder, rB, invaders) == commands::RejectReason::None);
    CHECK(w.cm->land_queue().size() == 1);
    w.sim.update();
    CHECK(w.cm->land_queue().empty());
    CHECK(w.cm->log().size() == 1 && w.cm->log()[0].kind == ConflictKind::Land && w.cm->log()[0].how == 1);
    if (!w.cm->log().empty() && w.cm->log()[0].winner == pUnder) {
        // The attacker's survivors landed: no Empire unit left on the surface, Underworld units there.
        bool under = false, emp = false;
        for (objects::ObjectHandle h : w.sim.planets()[rB].surface) {
            if (!w.store.alive(h)) continue;
            under = under || w.store.owner(h) == pUnder;
            emp = emp || w.store.owner(h) == pEmpire;
        }
        CHECK(under && !emp);
    }
    std::printf("land invasion: %s\n", w.cm->log().empty() ? "-" : w.cm->log()[0].text(w.sim).c_str());
}

// a1h t03 (A1_READINGS §31.2): the Invade event executor 0x48ff50 drops the event while the galactic pause counter is
// raised, or while a story-linked battle is pending and the local player is attacker or defender of a queued conflict
// (0x2b0350). GalacticTaskForce.Invade 0x6ce600 calls Land_Invade (GalacticSim::invade) directly: no gate.
void test_invade_event_gates() {
    World w({{pEmpire, rA, tInfantry}, {pEmpire, rB, tInfantry}, {pUnder, rA, tFrigate}, {pUnder, rA, tInfantry},
             {pUnder, rB, tFrigate}, {pUnder, rB, tInfantry}, {pUnder, rB, tInfantry}},
            true);  // the Empire is the human (local) player
    CHECK(w.sim.local_player() == pEmpire);
    auto launch = [&w](PlanetIndex p) {
        std::vector<objects::ObjectHandle> mine;
        for (objects::ObjectHandle h : w.sim.planets()[p].surface)
            if (w.store.owner(h) == pUnder) mine.push_back(h);
        CHECK(w.sim.launch_units(pUnder, p, mine) == commands::RejectReason::None);
        return mine;
    };
    const std::vector<objects::ObjectHandle> at_a = launch(rA), at_b = launch(rB);
    CHECK(at_a.size() == 1 && at_b.size() == 2);
    auto event = [&w](PlanetIndex p, std::span<const objects::ObjectHandle> units) {
        commands::EventData ev(commands::EventType::Invade);
        ev.set("planet", static_cast<uint32_t>(w.store.id(w.sim.planets()[p].object)));
        std::vector<uint32_t> ids;
        for (objects::ObjectHandle h : units) ids.push_back(static_cast<uint32_t>(w.store.id(h)));
        ev.set_ids("units", std::move(ids));
        return ev;
    };
    const std::span<const objects::ObjectHandle> b0(at_b.data(), 1), b1(at_b.data() + 1, 1);
    // The pause counter is raised: dropped.
    w.sim.push_pause();
    CHECK(w.sim.execute(pUnder, event(rB, b0)) == commands::RejectReason::NotAllowed && w.cm->land_queue().empty());
    w.sim.pop_pause();
    // A linked battle is pending but the local player is in no queued conflict: the event runs.
    CHECK(w.sim.queue_linked_battle() && !w.cm->in_queued_conflict(pEmpire));
    CHECK(w.sim.execute(pUnder, event(rB, b0)) == commands::RejectReason::None && w.cm->land_queue().size() == 1);
    CHECK(w.cm->in_queued_conflict(pEmpire) && w.cm->in_queued_conflict(pUnder) && !w.cm->in_queued_conflict(pNeutral));
    // Now the local player (defender at B) is in a queued conflict: dropped.
    CHECK(w.sim.execute(pUnder, event(rB, b1)) == commands::RejectReason::NotAllowed && w.cm->land_queue().size() == 1);
    // The AI task-force path (Land_Invade directly) is not gated.
    CHECK(w.sim.invade(pUnder, rA, at_a) == commands::RejectReason::None && w.cm->land_queue().size() == 2);
    w.sim.clear_linked_battle();
    // Only the issuer's own units land: the Empire cannot invade with Underworld units.
    CHECK(w.sim.execute(pEmpire, event(rB, b1)) == commands::RejectReason::NotOwner);
    std::printf("invade event gates: pause and linked-battle gates on the event, none on the AI's direct call\n");
}

// a1i t03 (A1_READINGS §31.7, REMAINING_GAPS 6.4): the story block (linked battle pending + the LOCAL player in a queued
// conflict, 0x2b0350) gates the paused query 0x4b2360 and the FleetManagement 0x5af260, MoveThroughObjects 0x3c4460 and
// SpecialAbility 0x4b4d70 executors. It never looks at the issuer: an AI player's commands run while the local player
// is in no queued conflict, and are dropped once it is. MoveObjectToObject 0x3af410 has no gate.
void test_linked_battle_gates() {
    World w({{pEmpire, rA, tInfantry}, {pEmpire, rB, tInfantry}, {pUnder, rB, tFrigate}, {pUnder, rB, tInfantry},
             {pUnder, rC, tFrigate}, {pUnder, rC, tInfantry}, {pUnder, rC, tInfantry}},
            true);  // the Empire is the human (local) player; the Underworld is an AI player
    CHECK(w.sim.local_player() == pEmpire);
    const auto id = [&w](objects::ObjectHandle h) { return static_cast<uint32_t>(w.store.id(h)); };
    std::vector<objects::ObjectHandle> under_c;
    for (objects::ObjectHandle h : w.sim.planets()[rC].surface)
        if (w.store.owner(h) == pUnder) under_c.push_back(h);
    CHECK(under_c.size() == 2);
    const uint32_t fc = w.fleet(rC, pUnder);
    CHECK(fc != kNoFleet);
    auto run = [&w](const commands::EventData& ev) { return w.sim.execute(static_cast<uint8_t>(pUnder), ev); };
    auto launch = [&](objects::ObjectHandle u) {
        return run(fleet_management_event(kFleetLaunch, commands::kNoObject, commands::kNoObject, id(u)));
    };
    auto path_move = [&](uint32_t f, PlanetIndex to) {
        commands::EventData ev(commands::EventType::MoveThroughObjects);
        ev.set("object", id(w.sim.fleets()[f].object)).set_ids("waypoints", {id(w.sim.planets()[to].object)});
        return run(ev);
    };
    // No planet: refused for the target (InvalidTarget) when the gate lets it through, NotAllowed when it does not.
    const commands::EventData special = special_activate_event(commands::kNoObject, commands::kNoObject);

    // Linked battle pending, no queued conflict at all: not paused, the AI's commands run.
    CHECK(w.sim.queue_linked_battle() && !w.cm->in_queued_conflict(pEmpire));
    CHECK(!w.sim.story_block() && !w.sim.paused());
    CHECK(launch(under_c[0]) == commands::RejectReason::None);
    CHECK(run(special) == commands::RejectReason::InvalidTarget);

    // The AI invades B (its direct task-force path, not gated): the local Empire defends a queued land conflict.
    std::vector<objects::ObjectHandle> at_b;
    for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
        if (w.store.owner(h) == pUnder) at_b.push_back(h);
    CHECK(w.sim.launch_units(pUnder, rB, at_b) == commands::RejectReason::None);
    CHECK(w.sim.invade(pUnder, rB, at_b) == commands::RejectReason::None && w.cm->land_queue().size() == 1);
    CHECK(w.cm->in_queued_conflict(pEmpire));
    CHECK(w.sim.story_block() && w.sim.paused());
    // Every gated AI command is dropped now, whoever issued it.
    CHECK(launch(under_c[1]) == commands::RejectReason::NotAllowed);
    CHECK(std::find(w.sim.planets()[rC].surface.begin(), w.sim.planets()[rC].surface.end(), under_c[1]) !=
          w.sim.planets()[rC].surface.end());
    CHECK(path_move(fc, rA) == commands::RejectReason::NotAllowed && !w.sim.fleets()[fc].moving);
    CHECK(run(special) == commands::RejectReason::NotAllowed);
    // MoveObjectToObject has no gate in the original.
    commands::EventData move(commands::EventType::MoveObjectToObject);
    move.set("object", id(w.sim.fleets()[fc].object)).set("target", id(w.sim.planets()[rB].object));
    CHECK(run(move) == commands::RejectReason::None && w.sim.fleets()[fc].moving);

    // The flag cleared (the linked battle started): the queued conflict alone does not pause.
    w.sim.clear_linked_battle();
    CHECK(w.cm->in_queued_conflict(pEmpire) && !w.sim.story_block() && !w.sim.paused());
    CHECK(launch(under_c[1]) == commands::RejectReason::None);
    std::printf("linked battle gates: local player not in a queued conflict -> AI commands run; in one -> dropped\n");
}

// a1j t05 (A1_READINGS §31.7): GalacticSim::update freezes the mode frame on the paused query 0x4b2360, so the story
// block freezes it like the pause counter. 0x2be640 gates the hero spawn lists (0x3fc750) on the counter alone: they
// still run under the story block.
void test_story_block_freezes_frame() {
    World w({{pEmpire, rA, tInfantry}, {pEmpire, rB, tInfantry}, {pUnder, rB, tInfantry}, {pUnder, rC, tFrigate}},
            true);
    objects::ObjectHandle hero{};
    for (objects::ObjectHandle h : w.sim.planets()[rA].surface)
        if (w.store.owner(h) == pEmpire) hero = h;
    CHECK(w.store.exists(hero));
    // Linked battle pending, the local player in no queued conflict: not frozen.
    CHECK(w.sim.queue_linked_battle() && !w.sim.story_block());
    int32_t f = w.sim.frame();
    w.sim.update();
    CHECK(w.sim.frame() > f);

    // The local Empire defends a queued land conflict: the story block freezes the frame.
    std::vector<objects::ObjectHandle> at_b;
    for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
        if (w.store.owner(h) == pUnder) at_b.push_back(h);
    CHECK(w.sim.launch_units(pUnder, rB, at_b) == commands::RejectReason::None);
    CHECK(w.sim.invade(pUnder, rB, at_b) == commands::RejectReason::None);
    CHECK(w.sim.story_block() && w.sim.paused());
    // A respawn due now (0.01 s rounds to 0 frames): serviced under the story block although the frame stays.
    CHECK(w.sim.schedule_hero_respawn(hero, rA, 0.01f) && w.sim.respawns_pending(rA).size() == 1);
    f = w.sim.frame();
    w.sim.update();
    CHECK(w.sim.frame() == f);
    CHECK(w.sim.respawns_pending(rA).empty());
    // (The bare ConflictManager serviced the queued conflict in that tick's tail; skipping the state machines under the
    // block is the host gate's job, engine/apps/sim campaign_battles.cpp, tested in alamo_sim_battles_tests.)

    // The flag cleared, the pause counter raised: frozen, and the spawn lists wait.
    w.sim.clear_linked_battle();
    CHECK(!w.sim.story_block());
    w.sim.push_pause();
    f = w.sim.frame();
    CHECK(w.sim.schedule_hero_respawn(hero, rA, 0.01f) && w.sim.respawns_pending(rA).size() == 1);
    w.sim.update();
    CHECK(w.sim.frame() == f && w.sim.respawns_pending(rA).size() == 1);
    w.sim.pop_pause();
    std::printf("story block: frame frozen, hero spawn lists serviced; pause counter: frozen, spawn lists wait\n");
}

// a1j t05: without the story block (no linked battle record) a queued conflict of the local player leaves the frame
// running, and the frame stops as soon as the record is queued.
void test_story_block_not_frozen() {
    World w({{pEmpire, rB, tInfantry}, {pUnder, rB, tInfantry}, {pUnder, rC, tFrigate}}, true);
    std::vector<objects::ObjectHandle> at_b;
    for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
        if (w.store.owner(h) == pUnder) at_b.push_back(h);
    CHECK(w.sim.launch_units(pUnder, rB, at_b) == commands::RejectReason::None);
    CHECK(w.sim.invade(pUnder, rB, at_b) == commands::RejectReason::None);
    CHECK(w.cm->in_queued_conflict(pEmpire) && !w.sim.story_block() && !w.sim.paused());
    const int32_t f = w.sim.frame();
    CHECK(w.sim.queue_linked_battle() && w.sim.story_block());
    w.sim.update();
    CHECK(w.sim.frame() == f);
    w.sim.clear_linked_battle();
    CHECK(!w.sim.paused());
    std::printf("story block: no linked record -> not paused; record queued -> frame frozen\n");
}

// OI galactic: TACTICAL_GAPS 4.5 retreat finders, 4.2 land split, 4.8 corruption missions / super-weapon retreat,
// STRATEGIC_GAPS 1.1 survival-roll listener.
void test_oi_transitions() {
    {   // Finders: orbit controller first (Empire frigate orbiting C), then base owners, then neutral, then any.
        World w({{pEmpire, rA, tBarracks}, {pEmpire, rC, tFrigate}, {pUnder, rB, tInfantry}}, false);
        CHECK(w.cm->retreat_destination(pEmpire, rB) == rC);              // C: Empire controls its orbit (A is closer)
        CHECK(w.cm->retreat_destination_ground(pEmpire, rA) == rC);       // controller first; the first two ground calls do not exclude
        CHECK(w.cm->retreat_destination_ground(pEmpire, rA, true) == rA); // base owner first: A itself (distance 0)
        CHECK(w.cm->retreat_destination(pUnder, rB) == kInvalidPlanet || w.cm->retreat_destination(pUnder, rB) != rB);
        CHECK(w.cm->retreat_destination(pNeutral, rA) != rA);
    }
    {   // Land split: the defender's surface all starts; the attacker = lead (first non-hero) + reserve.
        World w({{pEmpire, rB, tInfantry}, {pEmpire, rB, tInfantry}, {pUnder, rB, tFrigate}}, false);
        w.crules.types[tInfantry].hero = false;
        std::vector<objects::ObjectHandle> rec;
        for (int i = 0; i < 3; ++i) rec.push_back(w.sim.spawn_unit(pUnder, rB, tInfantry));
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
            if (w.store.owner(h) == pUnder) up.push_back(h);
        if (!up.empty()) CHECK(w.sim.launch_units(pUnder, rB, up) == commands::RejectReason::None);
        ConflictInfo head;
        head.kind = ConflictKind::Land;
        head.planet = rB;
        head.attacker = pUnder;
        head.defender = pEmpire;
        head.units = {rec[1], rec[2]};  // the record names two of the three
        BattleForces d, a;
        w.cm->split_land_forces(head, d, a);
        CHECK(d.start.size() == 2 && d.reserve.empty());
        CHECK(a.start.size() == 1 && a.start[0] == rec[1] && a.reserve.size() == 2 && a.reserve[0] == rec[2] && a.reserve[1] == rec[0]);
        w.crules.preferred_pathfinder_types = {"INFANTRY"};
        w.cm->split_land_forces(head, d, a);
        CHECK(a.start.size() == 1 && a.start[0] == rec[1]);
        // Raid: every unit in the limited mask, 1 <= limited <= max -> all record units start.
        w.crules.raid_limited_mask = kCatInfantry;
        w.crules.raid_max_limited = 2;
        w.crules.raid_max_heroes = 1;
        CHECK(w.cm->raid_force(pUnder, head.units));
        w.cm->split_land_forces(head, d, a);
        CHECK(a.start.size() == 2 && a.reserve.empty());
        w.crules.raid_required_faction = "Empire";
        CHECK(!w.cm->raid_force(pUnder, head.units));
    }
    {   // Corruption mission: no write-back (the losing attacker's fleet stays), the deaths still apply.
        World w({{pEmpire, rB, tFrigate}, {pUnder, rC, tFrigate}, {pUnder, rC, tCorvette}}, true);
        w.cm->config().human_choice = BattleChoice::Fight;
        BattleRequest got;
        w.cm->start_battle = [&](const BattleRequest& r) {
            got = r;
            return true;
        };
        w.arrive_at_b();
        CHECK(w.cm->battle_active());
        if (got.forces.size() == 2 && !got.forces[1].start.empty()) {
            BattleResult res;
            res.winner = pEmpire;
            res.destroyed = {got.forces[1].start[0]};
            w.cm->finish_corruption_mission(res);
            CHECK(!w.cm->battle_active() && w.cm->space_queue().empty() && !w.store.alive(got.forces[1].start[0]));
            bool moving = false;
            for (const FleetState& fl : w.sim.fleets()) moving = moving || (fl.alive && fl.owner == pUnder && fl.moving);
            CHECK(!moving && w.cm->log().size() == 1);
            int won = 0;
            for (const GalacticEvent& e : w.sim.events()) won += e.kind == GalacticEventKind::BattleWon;
            CHECK(won == 0);
        }
    }
    {   // Survival-roll listener: one call per combatant of the auto-resolve.
        World w({{pEmpire, rB, tFrigate}, {pEmpire, rB, tSB1}, {pUnder, rC, tFrigate}, {pUnder, rC, tFrigate}}, false);
        int calls = 0, survived = 0;
        w.cm->on_survival_roll = [&](int32_t owner, uint32_t type, bool s) {
            CHECK((owner == pEmpire || owner == pUnder) && type < tTypeCount);
            ++calls;
            survived += s ? 1 : 0;
        };
        w.arrive_at_b();
        CHECK(calls == 4 && survived <= calls && w.cm->log().size() == 1);
    }
}

// BATTLE_TRANSITIONS §3.3a: the retreat (0x2ba6c0) is one direct hop [P, dest] that ignores trade routes. Probe L1 systems
// t06 (retail FoC, 2026-10-31): A = Byss (Empire) - B = Kuat (Rebel station + fleet); C = Dantooine (Rebel barracks, no
// route). The Death Star fleet destroys Kuat with the TSW and loses the space battle (escort dead, Death Star alive, 0xb,
// winner Rebel). The Rebels have no Is_Super_Weapon_Killer -> R := the winner (TACTICAL_GAPS 4.8); the Rebel fleet jumps
// to Dantooine (finder 0x2aa400) without a route, the Rebel starbase is cleared, the Death Star stays, Kuat's derived
// owner is the Empire. pUnder plays the Rebels. `fighters_only`: the Rebel fleet has base speed 0 and stays in orbit.
struct ProbeRun {
    bool battle = false, retreat_r = false, departed = false, direct = false, arrived = false, stays = false;
    bool starbase_cleared = false, death_star_stays = false, owner_empire = false, destroyed = false, no_rebels = false;
    bool no_surface = false, invade_refused = false;  // 0x375220: a destroyed planet has no surface
    uint64_t digest = 0;
};

ProbeRun run_retreat_probe(bool fighters_only) {
    ProbeRun out;
    World w({{pEmpire, rA, tBarracks}, {pEmpire, rA, tDeathStar}, {pEmpire, rA, tFrigate}, {pUnder, rB, tSB2},
             {pUnder, rB, fighters_only ? tFighters : tFrigate}, {pUnder, rC, tBarracks}},
            true, false);
    w.cm->config().human_choice = BattleChoice::Fight;
    BattleRequest got;
    w.cm->start_battle = [&](const BattleRequest& r) {
        got = r;
        return true;
    };
    const uint32_t ef = w.fleet(rA, pEmpire), rf = w.fleet(rB, pUnder);
    CHECK(ef != kNoFleet && rf != kNoFleet);
    if (ef == kNoFleet || rf == kNoFleet) return out;
    const PlanetIndex hop[] = {rA, rB};
    CHECK(w.sim.move_fleet_path(pEmpire, ef, hop) == commands::RejectReason::None);
    for (int n = 0; n < 5000 && !w.cm->battle_active(); ++n) w.sim.update();
    out.battle = w.cm->battle_active() && got.info.planet == rB && got.info.attacker == pEmpire && got.info.defender == pUnder;
    if (!out.battle) return out;
    // TSW state 9: the shared destroy core only (orbit, starbase and owner stay; TSW_PLANET_DESTROY §5).
    CHECK(w.sim.tsw_destroy_planet(rB, pEmpire));
    objects::ObjectHandle escort{}, death_star{};
    for (uint32_t f : w.sim.fleets_at(rB, pEmpire))
        for (objects::ObjectHandle h : w.sim.fleets()[f].contents) {
            if (static_cast<uint32_t>(w.store.type(h)) == tFrigate) escort = h;
            if (static_cast<uint32_t>(w.store.type(h)) == tDeathStar) death_star = h;
        }
    CHECK(w.store.alive(escort) && w.store.alive(death_star));
    BattleResult res;
    res.winner = pUnder;
    res.victory = kVictoryAllDestroyed;
    res.destroyed = {escort};
    w.cm->finish_battle(res);
    out.retreat_r = !w.cm->log().empty() && w.cm->log().back().retreating == pUnder && w.cm->log().back().winner == pUnder;
    const PlanetState& kuat = w.sim.planets()[rB];
    out.starbase_cleared = !w.store.alive(kuat.starbase) && kuat.starbase_level == 0;
    const FleetState& rebel = w.sim.fleets()[rf];
    out.departed = rebel.alive && rebel.moving && rebel.to == rC && rebel.from == rB;
    // No route: hop factor 1.0, so the hop speed is the base speed (a route hop would be 5x).
    out.direct = out.departed && rebel.hop_speed == rebel.base_speed && rebel.base_speed > 0.0f;
    out.stays = rebel.alive && !rebel.moving && rebel.planet == rB;
    for (int n = 0; n < 5000 && w.sim.fleets()[rf].alive && w.sim.fleets()[rf].moving; ++n) w.sim.update();
    for (int n = 0; n < 3; ++n) w.sim.update();  // derived owner service
    out.arrived = !w.sim.fleets_at(rC, pUnder).empty() && w.sim.fleets_at(rB, pUnder).empty();
    out.death_star_stays = w.store.alive(death_star) && w.sim.fleet_containing(death_star) != kNoFleet &&
                           w.sim.fleets()[w.sim.fleet_containing(death_star)].planet == rB;
    out.destroyed = kuat.destroyed;
    out.owner_empire = kuat.owner == pEmpire;
    out.no_surface = w.sim.surface_accessible(rA) && !w.sim.surface_accessible(rB);
    out.invade_refused = w.sim.invade(pEmpire, rB, {}) == commands::RejectReason::NotAllowed;
    bool rebels = !w.sim.fleets_at(rB, pUnder).empty() || (w.store.alive(kuat.starbase) && kuat.starbase_owner == pUnder);
    for (objects::ObjectHandle h : kuat.structures) rebels = rebels || (w.store.alive(h) && w.store.owner(h) == pUnder);
    for (objects::ObjectHandle h : kuat.surface) rebels = rebels || (w.store.alive(h) && w.store.owner(h) == pUnder);
    out.no_rebels = !rebels;
    out.digest = w.digest();
    return out;
}

void test_retreat_direct_hop() {
    {   // (a) the probe: retail Kuat destroyed=true owner=EMPIRE starbase none, no Rebel object, Death Star still there.
        const ProbeRun r = run_retreat_probe(false);
        CHECK(r.battle && r.retreat_r && r.starbase_cleared);
        CHECK(r.departed && r.direct);
        CHECK(r.arrived && r.death_star_stays && r.destroyed && r.owner_empire && r.no_rebels);
        CHECK(r.no_surface && r.invade_refused);
        CHECK(run_retreat_probe(false).digest == r.digest);  // determinism
    }
    {   // (b) base speed 0 (fighters, no hyperspace member): refused, the fleet stays in orbit (not destroyed); the
        // starbase is still cleared (the starbase step does not depend on any fleet leaving).
        const ProbeRun r = run_retreat_probe(true);
        CHECK(r.battle && r.retreat_r && r.starbase_cleared);
        CHECK(!r.departed && r.stays && r.death_star_stays && r.destroyed);
    }
    {   // (c) the entry point: moving / own planet refused; connected planets get the route factor.
        World w({{pEmpire, rA, tFrigate}, {pUnder, rC, tBarracks}}, false, false);
        const uint32_t f = w.fleet(rA, pEmpire);
        CHECK(f != kNoFleet);
        if (f == kNoFleet) return;
        using commands::RejectReason;
        CHECK(w.sim.move_fleet_direct(pEmpire, f, rA) == RejectReason::NotAllowed);  // destination = its planet
        CHECK(!w.sim.fleets()[f].moving && w.sim.fleets()[f].planet == rA);
        CHECK(w.sim.move_fleet_direct(pUnder, f, rB) == RejectReason::NotOwner);
        CHECK(w.sim.move_fleet_direct(pEmpire, f, rB) == RejectReason::None);  // A - B joined: HS_Speed_Factor 5
        const FleetState& fl = w.sim.fleets()[f];
        CHECK(fl.moving && fl.to == rB && fl.hop_speed == fl.base_speed * 5.0f && fl.path.size() == 2);
        CHECK(w.sim.move_fleet_direct(pEmpire, f, rC) == RejectReason::NotAllowed);  // already in transit
        CHECK(fl.moving && fl.to == rB);
        for (int n = 0; n < 5000 && w.sim.fleets()[f].moving; ++n) w.sim.update();
        CHECK(!w.sim.fleets()[f].moving && w.sim.fleets()[f].planet == rB);
        // B - C has no route here: move_fleet's path finder fails, the direct hop does not.
        CHECK(w.sim.move_fleet(pEmpire, f, rC) != RejectReason::None && !w.sim.fleets()[f].moving);
        CHECK(w.sim.move_fleet_direct(pEmpire, f, rC) == RejectReason::None);
        CHECK(w.sim.fleets()[f].moving && w.sim.fleets()[f].hop_speed == w.sim.fleets()[f].base_speed);
    }
}

// t50: Land_Skirmish_Unit_Cap_By_Player_Count keys (players, cap): shipped 0,10, 1,10, 8,5.
void test_land_skirmish_unit_cap() {
    ConflictFactionInfo f;
    CHECK(f.unit_cap_for_players(2.0f) < 0.0f);  // no curve
    f.land_skirmish_unit_cap = {{0.0f, 10.0f}, {1.0f, 10.0f}, {8.0f, 5.0f}};
    CHECK(f.unit_cap_for_players(0.0f) == 10.0f);
    CHECK(f.unit_cap_for_players(1.0f) == 10.0f);
    CHECK(f.unit_cap_for_players(8.0f) == 5.0f);
    // skai (SKIRMISH_SETUP §6): 0 outside the curve, the integer part inside (2 players: 9.29 -> 9, 4: 7.86 -> 7).
    CHECK(f.unit_cap_for_players(20.0f) == 0.0f);
    CHECK(f.unit_cap_for_players(-3.0f) == 0.0f);
    CHECK(f.unit_cap_for_players(2.0f) == 9.0f);
    CHECK(f.unit_cap_for_players(4.0f) == 7.0f);
    f.land_skirmish_unit_cap = {{8.0f, 5.0f}, {0.0f, 10.0f}, {1.0f, 10.0f}};  // sorted by x on use
    CHECK(f.unit_cap_for_players(2.0f) == 9.0f);
    CHECK(f.unit_cap_for_players(4.5f) < f.unit_cap_for_players(2.0f));  // never rises with more players
}

}  // namespace

// story t07 (BT §2.9.6): LINK_TACTICAL Param12 at the end of a linked battle. The human Empire attacks the AI Underworld at
// B (land: two infantry and a barracks on the surface; space: a frigate fleet). The loser would retreat (R = defender);
// with a resolution won by the human that retreat is skipped and the resolution runs instead. An AI winner: nothing.
void test_linked_resolution() {
    using LR = LinkedResolution;
    struct Case {
        bool space;
        LR res;
        int32_t winner;
        size_t under_units, under_structures;  // Underworld at B afterwards (surface units / structures; space: fleets in orbit)
    };
    for (const Case& c : {Case{false, LR::None, pEmpire, 0, 1}, Case{false, LR::RetreatPlayer, pEmpire, 2, 1},
                          Case{false, LR::RetreatAi, pEmpire, 0, 0}, Case{false, LR::DestroyAi, pEmpire, 0, 0},
                          Case{false, LR::DestroyAi, pUnder, 2, 1}, Case{true, LR::RetreatPlayer, pEmpire, 1, 0},
                          Case{true, LR::DestroyAi, pEmpire, 0, 0}}) {
        std::vector<StartingForce> f = {{pEmpire, rA, tInfantry}};
        if (c.space) f.insert(f.end(), {{pEmpire, rB, tFrigate}, {pUnder, rB, tFrigate}, {pUnder, rB, tCorvette}});
        else f.insert(f.end(), {{pUnder, rB, tInfantry}, {pUnder, rB, tInfantry}, {pUnder, rB, tBarracks}});
        World w(std::move(f), true);
        w.cm->start_battle = [](const BattleRequest&) { return true; };
        ConflictManager::LinkedBattle b;
        b.kind = c.space ? ConflictKind::Space : ConflictKind::Land;
        b.planet = rB;
        b.attacker = pEmpire;
        b.defender = pUnder;
        b.map = "M";
        CHECK(w.cm->start_linked(b) && w.cm->battle_active());
        BattleResult r;
        r.winner = c.winner;
        r.retreated = c.winner == pEmpire ? pUnder : kNoPlayer;
        w.cm->finish_battle(r, c.res);
        size_t units = 0, structures = 0;
        if (c.space) {
            for (uint32_t fi : w.sim.fleets_at(rB, pUnder))
                if (w.sim.fleets()[fi].alive && !w.sim.fleets()[fi].moving) ++units;
        } else {
            for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
                if (w.store.alive(h) && w.store.owner(h) == pUnder) ++units;
            for (objects::ObjectHandle h : w.sim.planets()[rB].structures)
                if (w.store.alive(h) && w.store.owner(h) == pUnder) ++structures;
        }
        std::printf("linked resolution %s %d, winner %d: Underworld at B %zu unit(s)/fleet(s), %zu structure(s)\n", c.space ? "space" : "land",
                    static_cast<int>(c.res), c.winner, units, structures);
        CHECK(units == c.under_units && structures == c.under_structures && !w.cm->battle_active());
    }
}

// story t07 (BT §2.3 rules 2 / 3, TACTICAL_GAPS §4.2 rule (a)): a sandbox linked battle's human side with Param6 (forced type)
// and Param14 (flagship split). Space: the human Empire's frigate + two corvettes at B against the AI Underworld.
void test_linked_sandbox_split() {
    struct Case {
        uint32_t forced;
        bool flagship;
        size_t start, reserve;
        uint32_t start_type;
    };
    for (const Case& c : {Case{tCorvette, true, 1, 2, tCorvette}, Case{kNoIndex, true, 1, 2, tFrigate}, Case{kNoIndex, false, 3, 0, kNoIndex},
                          Case{tDeathStar, false, 3, 0, kNoIndex}}) {
        World w({{pEmpire, rB, tFrigate}, {pEmpire, rB, tCorvette}, {pEmpire, rB, tCorvette}, {pUnder, rB, tFrigate}}, true);
        w.cm->start_battle = [](const BattleRequest&) { return true; };
        ConflictManager::LinkedBattle b;
        b.kind = ConflictKind::Space;
        b.planet = rB;
        b.attacker = pEmpire;
        b.defender = pUnder;
        b.map = "M";
        b.sandbox = true;
        b.forced_type = c.forced;
        b.flagship_split = c.flagship;
        CHECK(w.cm->start_linked(b));
        const BattleRequest& q = w.cm->active_request();
        CHECK(q.forces.size() == 2 && q.forces[0].start.empty() && q.forces[0].reserve.empty());  // the AI defender: nothing
        if (q.forces.size() != 2) continue;
        const BattleForces& a = q.forces[1];
        std::printf("linked sandbox space forced %u flagship %d: start %zu reserve %zu\n", c.forced, c.flagship ? 1 : 0, a.start.size(),
                    a.reserve.size());
        CHECK(a.start.size() == c.start && a.reserve.size() == c.reserve);
        if (c.start_type != kNoIndex && !a.start.empty()) CHECK(static_cast<uint32_t>(w.store.type(a.start[0])) == c.start_type);
    }
    // Land: the record units are the Empire's infantry that already landed on B (a link fired by STORY_LAND_ON / DEPLOY):
    // the forced type leads, the other record unit waits in the reserve; not sandbox: nobody.
    for (const bool sandbox : {true, false}) {
        World w({{pEmpire, rB, tInfantry}, {pEmpire, rB, tInfantry}, {pUnder, rB, tBarracks}}, true);
        w.cm->start_battle = [](const BattleRequest&) { return true; };
        ConflictManager::LinkedBattle b;
        b.kind = ConflictKind::Land;
        b.planet = rB;
        b.attacker = pEmpire;
        b.defender = pUnder;
        b.map = "M";
        b.sandbox = sandbox;
        b.forced_type = tInfantry;
        for (objects::ObjectHandle h : w.sim.planets()[rB].surface)
            if (w.store.owner(h) == pEmpire) b.units.push_back(h);
        CHECK(b.units.size() == 2);
        const objects::ObjectHandle first = b.units.empty() ? objects::ObjectHandle{} : b.units[0];
        CHECK(w.cm->start_linked(b));
        const BattleRequest& q = w.cm->active_request();
        CHECK(q.forces.size() == 2);
        if (q.forces.size() != 2) continue;
        std::printf("linked land %s: attacker start %zu reserve %zu\n", sandbox ? "sandbox" : "plain", q.forces[1].start.size(),
                    q.forces[1].reserve.size());
        if (sandbox) CHECK(q.forces[1].start.size() == 1 && q.forces[1].start[0] == first && q.forces[1].reserve.size() == 1);
        else CHECK(q.forces[1].start.empty() && q.forces[1].reserve.empty());
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("contrast\n");
    test_contrast_parse();
    std::printf("auto-resolve\n");
    test_auto_resolve_basic();
    test_auto_resolve_rules();
    std::printf("force split\n");
    test_force_split();
    std::printf("ai auto-resolve\n");
    test_ai_auto_resolve();
    std::printf("human fight\n");
    test_human_fight();
    test_human_retreat_after_battle();
    std::printf("pending choice\n");
    test_pending_choice();
    test_change_owner_drops_queued();
    std::printf("land invasion\n");
    test_land_invasion();
    test_invade_event_gates();
    test_linked_battle_gates();
    test_story_block_freezes_frame();
    test_story_block_not_frozen();
    test_p4_set_retreating();  // P4 sim
    test_cargo_losses();       // BT §4.4 loss records for carried units
    test_p4_resolve_battle();  // P4 sim
    std::printf("oi transitions\n");
    test_oi_transitions();  // OI galactic
    std::printf("retreat direct hop\n");
    test_retreat_direct_hop();  // BATTLE_TRANSITIONS §3.3a
    std::printf("land skirmish unit cap\n");
    test_land_skirmish_unit_cap();  // t50
    std::printf("linked battle params (story t07)\n");
    test_linked_resolution();
    test_linked_sandbox_split();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_galactic_conflict_tests: all passed\n");
    return 0;
}
