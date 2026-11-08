// alamo_galactic_visibility_tests: GRC §2 (docs/re/GALACTIC_REVEAL_CORRUPTION.md) planet visibility masks on GalacticSim.
//   1. the VisibilityLevelType converter and the faction defaults;
//   2. each source on a synthetic galaxy: baseline (local player only), presence (AI players only), always-on spies
//      (unit / structure / planet, influence range), Ground_Activated spies (probe: entry, expiry, range, removal), story
//      SET_PLANET_VISIBILITY_LEVEL (upsert, second key, "none", bad input, expiry);
//   3. the fold (OR over the sources and the list, list order kept by upserts), base info step 3, trade-route visibility;
//   4. CRC: the "VIS1" tail only while entries exist; identical runs hash identically;
//   5. FoC data (ALAMO_FOC_DATA; skipped when unset): the spy masks / durations / fix-ups of the shipped abilities.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/support.hpp"
#include "foc_data.hpp"

using namespace alamo;
using namespace alamo::galactic;

using ::alamo::test::g_fail;
using ::alamo::test::g_checks;

namespace {

enum TypeId : uint32_t { tPA, tPB, tPC, tPD, tPE, tFrigate, tInfantry, tProbe, tProbeWide, tSmuggler, tMara, tOLRS, tSB, tTypeCount };
enum Row : PlanetIndex { rA = 0, rB = 1, rC = 2, rD = 3, rE = 4 };
enum PlayerId : int32_t { pEmp = 0, pReb = 1, pUnd = 2, pNeu = 3 };
enum AbilityId : uint32_t { aProbe, aProbeWide, aSmuggler, aMara, aOLRS, aPlanetD, aCount };

SpecialAbilityDef spy(const char* name, ActivationStyle style, uint32_t mask, float duration) {
    SpecialAbilityDef a;
    a.cls = SpecialAbilityClass::SystemSpy;
    a.tag = "System_Spy_Ability";
    a.name = name;
    a.name_crc = special_name_crc(name);
    a.style = style;
    a.spy_mask = mask;
    a.spy_duration = duration;
    return a;
}

GalacticRules make_rules() {
    GalacticRules r;
    auto faction = [](const char* n, bool neutral, bool benefits) {
        FactionRules f;
        f.name = n;
        f.is_neutral = neutral;
        f.can_control_planets = f.is_playable = !neutral;
        f.benefits_from_corruption = benefits;
        return f;
    };
    r.factions = {faction("Empire", false, false), faction("Rebel", false, false), faction("Underworld", false, true),
                  faction("Neutral", true, false)};
    r.types.resize(tTypeCount);
    const char* pn[] = {"PA", "PB", "PC", "PD", "PE"};
    for (uint32_t i = 0; i < 5; ++i) {
        r.types[i].name = pn[i];
        r.types[i].kind = UnitKind::Planet;
        r.types[i].credit_value = 10;
    }
    auto unit = [&r](uint32_t id, const char* name, UnitKind k) -> TypeInfo& {
        r.types[id].name = name;
        r.types[id].kind = k;
        return r.types[id];
    };
    unit(tFrigate, "Frigate", UnitKind::SpaceUnit);
    unit(tInfantry, "Infantry", UnitKind::GroundUnit);
    unit(tProbe, "Probe", UnitKind::GroundUnit);
    unit(tProbeWide, "ProbeWide", UnitKind::GroundUnit).influence_range = 70.0f;
    unit(tSmuggler, "Smuggler", UnitKind::GroundUnit);
    unit(tMara, "Mara", UnitKind::GroundUnit).influence_range = 70.0f;
    unit(tOLRS, "OLRS", UnitKind::GroundStructure);
    unit(tSB, "SB", UnitKind::StarBase).base_level = 1;
    r.types[tPD].influence_range = 100.0f;

    SpecialRules& s = r.special;
    s.abilities.resize(aCount);
    s.abilities[aProbe] = spy("Probe_Spy", ActivationStyle::GroundActivated, 0x7F6, 60.0f);
    s.abilities[aProbe].causes_despawn = true;
    s.abilities[aProbeWide] = spy("Probe_Wide_Spy", ActivationStyle::GroundActivated, 0x206, 30.0f);
    s.abilities[aSmuggler] = spy("Smuggler_Limited_Spy", ActivationStyle::GalacticAutomatic, 0x30, -1.0f);
    s.abilities[aMara] = spy("Mara_Spy", ActivationStyle::GalacticAutomatic, 0x206, -1.0f);
    s.abilities[aOLRS] = spy("OLRS_Spy", ActivationStyle::GalacticAutomatic, 0x60, -1.0f);
    s.abilities[aPlanetD] = spy("PlanetD_Spy_Network", ActivationStyle::GalacticAutomatic, 0x2000, -1.0f);
    s.types.resize(tTypeCount);
    auto give = [&s](uint32_t t, uint32_t a) {
        s.types[t].abilities.push_back(a);
        if (s.abilities[a].style == ActivationStyle::GalacticAutomatic) s.types[t].auto_spy_mask |= s.abilities[a].spy_mask;
    };
    give(tProbe, aProbe);
    s.types[tProbe].generic_hero = true;
    give(tProbeWide, aProbeWide);
    give(tSmuggler, aSmuggler);
    give(tMara, aMara);
    give(tOLRS, aOLRS);
    give(tPD, aPlanetD);
    return r;
}

GalacticSetup make_setup(int32_t human) {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "GrcVisibility";
    const char* names[] = {"A", "B", "C", "D", "E"};
    const Vec3 pos[] = {{0, 0, 0}, {60, 0, 0}, {120, 0, 0}, {60, 80, 0}, {300, 0, 0}};
    for (uint32_t i = 0; i < 5; ++i) {
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
    route("B_C", rB, rC);
    route("B_D", rB, rD);
    route("C_E", rC, rE);
    s.planet_type = {tPA, tPB, tPC, tPD, tPE};
    for (uint32_t f = 0; f < 4; ++f) {
        PlayerSetup p;
        p.faction = f;
        p.human = static_cast<int32_t>(f) == human;
        p.ai = !p.human && f != 3;
        p.credits = 1000.0f;
        s.players.push_back(p);
    }
    s.forces = {{pEmp, rA, tInfantry}, {pReb, rC, tInfantry}, {pUnd, rD, tInfantry}};
    s.planet_auto_reveal = false;
    return s;
}

struct World {
    GalacticRules rules = make_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{0x7155u};
    GalacticSim sim{rules, store, rng};
    explicit World(int32_t human = pEmp) { CHECK(sim.start(make_setup(human))); }
    void run(int n) {
        for (int i = 0; i < n; ++i) sim.update();
    }
    uint64_t digest() const {
        StateHasher h;
        sim.hash_state(h);
        return h.digest();
    }
    int32_t pid(PlanetIndex p) const { return static_cast<int32_t>(store.id(sim.planets()[p].object)); }
};

// ---- 1. converter -----------------------------------------------------------------------------------------------------

void test_parse() {
    std::printf("  parse\n");
    uint32_t v = 7;
    CHECK(parse_visibility_level("Maximum", &v) && v == 0xFFFFFFFFu);
    CHECK(parse_visibility_level("planet_owner", &v) && v == 0x9FFFu);
    CHECK(parse_visibility_level(" Starbase_Level | Credit_Income ", &v) && v == 0xAu);
    CHECK(parse_visibility_level("Fleet_Presence,Num_Fleets", &v) && v == 0x30u);
    CHECK(parse_visibility_level("Enemy_Minor_Stealth_Heroes", &v) && v == 0x2000u);
    CHECK(parse_visibility_level("Super_Weapons", &v) && v == 0x40000u);
    CHECK(parse_visibility_level("Good_Default", &v) && v == 0x11u);
    CHECK(parse_visibility_level("Evil_Default", &v) && v == 0u);
    CHECK(parse_visibility_level("0x30", &v) && v == 0x30u && parse_visibility_level("17", &v) && v == 17u);
    v = 5;
    CHECK(!parse_visibility_level("", &v) && !parse_visibility_level("none", &v) && !parse_visibility_level("Bogus", &v) &&
          !parse_visibility_level("Maximum|Bogus", &v) && v == 5u);
    CHECK(faction_default_visibility("Rebel") == 0x11u && faction_default_visibility("Underworld") == 0x11u &&
          faction_default_visibility("Empire") == 0u && faction_default_visibility("Hutts") == 0u);
}

// ---- 2. sources -------------------------------------------------------------------------------------------------------

void test_baseline() {
    std::printf("  baseline\n");
    World w;  // Empire human = local
    GalacticSim& s = w.sim;
    CHECK(s.local_player() == pEmp);
    CHECK(s.planets()[rA].owner == pEmp && s.planets()[rC].owner == pReb && s.planets()[rD].owner == pUnd);
    // Owner: Planet_Owner; elsewhere the Empire default (0).
    CHECK(s.visibility_sources(rA, pEmp).baseline == kVisPlanetOwner && s.visibility_mask(rA, pEmp) == kVisPlanetOwner);
    CHECK(s.visibility_mask(rC, pEmp) == 0u && s.visibility_mask(rB, pEmp) == 0u);
    // Non-local players get no baseline; a player with neither baseline nor AI has an empty mask.
    CHECK(s.visibility_sources(rC, pReb).baseline == 0u && s.visibility_mask(rA, pNeu) == 0u);
    // Ownership drives it: the Empire takes C.
    CHECK(s.change_owner(s.planets()[rC].object, pEmp));
    CHECK(s.visibility_mask(rC, pEmp) == kVisPlanetOwner);
    // Invalid inputs.
    CHECK(s.visibility_mask(99, pEmp) == 0u && s.visibility_mask(rA, 9) == 0u && s.visibility_mask(rA, -1) == 0u);

    World r(pReb);  // Rebel local: the Good default where it does not own
    CHECK(r.sim.local_player() == pReb && r.sim.visibility_mask(rA, pReb) == kVisGoodDefault &&
          r.sim.visibility_mask(rC, pReb) == kVisPlanetOwner);
    CHECK(r.sim.visibility_sources(rA, pEmp).presence == kVisMaximum && r.sim.visibility_sources(rA, pEmp).baseline == 0u);  // AI owner
}

void test_presence() {
    std::printf("  presence\n");
    World w;
    GalacticSim& s = w.sim;
    // Rebel AI at the Empire's A: its faction default.
    CHECK(s.visibility_sources(rA, pReb).presence == kVisGoodDefault);
    CHECK(s.visibility_sources(rA, pUnd).presence == kVisGoodDefault);
    // An owned planet: Maximum (signalled level -1).
    CHECK(s.visibility_mask(rC, pReb) == kVisMaximum && s.visibility_mask(rD, pUnd) == kVisMaximum);
    // Orbit -> 0x71, ground -> | 0x191.
    const ObjectHandle fr = s.spawn_unit(pReb, rA, tFrigate);
    CHECK(static_cast<bool>(fr));
    CHECK(s.visibility_sources(rA, pReb).presence == kVisPresenceOrbit);
    const ObjectHandle inf = s.spawn_unit(pReb, rA, tInfantry);
    CHECK(static_cast<bool>(inf));
    CHECK(s.visibility_mask(rA, pReb) == (kVisPresenceOrbit | kVisPresenceGround));
    // Removing the forces reverts to the default.
    CHECK(s.destroy_object(fr) && s.destroy_object(inf));
    CHECK(s.visibility_mask(rA, pReb) == kVisGoodDefault);
    // The local human has no presence entry (no AI system).
    const ObjectHandle ef = s.spawn_unit(pEmp, rC, tFrigate);
    CHECK(static_cast<bool>(ef) && s.visibility_sources(rC, pEmp).presence == 0u && s.visibility_mask(rC, pEmp) == 0u);
}

void test_auto_spy() {
    std::printf("  auto spy\n");
    World w;
    GalacticSim& s = w.sim;
    // The Underworld planet D's spy network: D itself and every planet within D's range 100 (B at 80; A at 100; C at 100;
    // E at ~250 not).
    CHECK(s.visibility_sources(rD, pUnd).auto_spy == 0x2000u);
    CHECK(s.visibility_sources(rB, pUnd).auto_spy == 0x2000u && s.visibility_sources(rA, pUnd).auto_spy == 0x2000u &&
          s.visibility_sources(rC, pUnd).auto_spy == 0x2000u && s.visibility_sources(rE, pUnd).auto_spy == 0u);
    CHECK(s.visibility_sources(rB, pReb).auto_spy == 0u);
    // A smuggler (no range) at E spies on E only.
    const ObjectHandle sm = s.spawn_unit(pReb, rE, tSmuggler);
    CHECK(static_cast<bool>(sm));
    CHECK(s.visibility_sources(rE, pReb).auto_spy == 0x30u && s.visibility_sources(rC, pReb).auto_spy == 0u);
    // Mara (range 70) at B: A and C (60) and B; not D (80) nor E.
    const ObjectHandle ma = s.spawn_unit(pEmp, rB, tMara);
    CHECK(static_cast<bool>(ma));
    CHECK(s.visibility_sources(rB, pEmp).auto_spy == 0x206u && s.visibility_sources(rC, pEmp).auto_spy == 0x206u &&
          s.visibility_sources(rA, pEmp).auto_spy == 0x206u && s.visibility_sources(rD, pEmp).auto_spy == 0u &&
          s.visibility_sources(rE, pEmp).auto_spy == 0u);
    CHECK(s.visibility_mask(rC, pEmp) == 0x206u);  // Empire default 0 | spy
    // A structure spy at the owner's planet (structures count too).
    const ObjectHandle ol = s.spawn_unit(pEmp, rA, tOLRS);
    CHECK(static_cast<bool>(ol) && s.visibility_sources(rA, pEmp).auto_spy == (0x206u | 0x60u));
    // The spy leaves: the contribution goes with it (no stored entry, nothing hashed).
    CHECK(s.destroy_object(sm) && s.visibility_sources(rE, pReb).auto_spy == 0u);
    CHECK(s.visibility_modifiers(rE, pReb).empty());
    // The planet's network belongs to its owner: losing D moves it.
    CHECK(s.change_owner(s.planets()[rD].object, pReb));
    CHECK(s.visibility_sources(rB, pUnd).auto_spy == 0u && s.visibility_sources(rB, pReb).auto_spy == 0x2000u);
}

void test_ground_spy() {
    std::printf("  ground-activated spy\n");
    World w;
    GalacticSim& s = w.sim;
    const uint64_t d0 = w.digest();
    // Probe at the Rebel planet C (no range): one entry for the Empire at C, 60 s, then the probe departs.
    const ObjectHandle pr = s.spawn_unit(pEmp, rC, tProbe);
    CHECK(static_cast<bool>(pr));
    const int32_t probe_id = static_cast<int32_t>(w.store.id(pr));
    const uint64_t d1 = w.digest();
    CHECK(d1 != d0);  // the spawn itself
    const int32_t f0 = s.frame();
    bool accepted = false;
    CHECK(s.ground_activated_trigger(pr, rC, &accepted) && accepted);
    CHECK(!w.store.alive(pr));  // Causes_Despawn
    const auto mods = s.visibility_modifiers(rC, pEmp);
    CHECK(mods.size() == 1u);
    if (mods.size() == 1u) {
        CHECK(mods[0].value == 0x7F6u && mods[0].name_hash == special_name_crc("Probe_Spy"));
        CHECK(mods[0].key.object == probe_id && mods[0].key.sub == special_name_crc("Probe_Spy"));
        CHECK(mods[0].expiry == static_cast<uint32_t>(f0 + 1800));
    }
    CHECK(s.visibility_mask(rC, pEmp) == 0x7F6u && s.visibility_sources(rC, pEmp).stored == 0x7F6u);
    CHECK(s.visibility_modifiers(rB, pEmp).empty() && s.visibility_mask(rC, pReb) == kVisMaximum);
    CHECK(s.can_see_starbase_info(pEmp, rC) && s.can_see_groundbase_info(pEmp, rC));  // 0x40 / 0x100 in 0x7F6
    // Expiry: removed by the first planet service on a frame > expiry, i.e. after 1802 updates from f0.
    w.run(1801);
    CHECK(s.visibility_modifiers(rC, pEmp).size() == 1u);
    w.run(1);
    CHECK(s.visibility_modifiers(rC, pEmp).empty() && s.visibility_mask(rC, pEmp) == 0u);
    CHECK(!s.can_see_starbase_info(pEmp, rC));

    // A ranged Ground_Activated spy (range 70) at B: entries at A, B, C (row order); not D (80) / E. No despawn.
    const ObjectHandle pw = s.spawn_unit(pEmp, rB, tProbeWide);
    CHECK(s.ground_activated_trigger(pw, rB));
    CHECK(s.visibility_modifiers(rA, pEmp).size() == 1u && s.visibility_modifiers(rB, pEmp).size() == 1u &&
          s.visibility_modifiers(rC, pEmp).size() == 1u && s.visibility_modifiers(rD, pEmp).empty() &&
          s.visibility_modifiers(rE, pEmp).empty());
    CHECK(s.visibility_modifiers(rB, pEmp)[0].expiry == static_cast<uint32_t>(s.frame() + 900));
    // Re-activation upserts (same key: still one entry per planet, expiry renewed).
    w.run(5);
    CHECK(s.ground_activated_trigger(pw, rB));
    CHECK(s.visibility_modifiers(rB, pEmp).size() == 1u && s.visibility_modifiers(rB, pEmp)[0].expiry == static_cast<uint32_t>(s.frame() + 900));
    // Deactivation 0x70fb90: every entry keyed by the unit goes.
    CHECK(s.system_spy_remove(pw) == 3);
    CHECK(s.visibility_modifiers(rA, pEmp).empty() && s.visibility_modifiers(rC, pEmp).empty());
    CHECK(s.system_spy_remove(pw) == 0);
}

void test_story() {
    std::printf("  story SET_PLANET_VISIBILITY_LEVEL\n");
    World w;
    GalacticSim& s = w.sim;
    const uint64_t d0 = w.digest();
    // Empire Act I Fondor style: Maximum, then none.
    CHECK(s.story_set_planet_visibility(rC, "Empire", "Maximum", "Fondor_Reveal"));
    CHECK(s.visibility_mask(rC, pEmp) == kVisMaximum);
    auto mods = s.visibility_modifiers(rC, pEmp);
    CHECK(mods.size() == 1u && mods[0].key.object == w.pid(rC) && mods[0].key.sub == special_name_crc("Fondor_Reveal") &&
          mods[0].name_hash == special_name_crc("Fondor_Reveal") && mods[0].expiry == kVisNever);
    const uint64_t d1 = w.digest();
    CHECK(d1 != d0);  // the VIS1 tail
    // Same key: upsert in place.
    CHECK(s.story_set_planet_visibility(rC, "Empire", "Planet_Owner", "Fondor_Reveal"));
    CHECK(s.visibility_modifiers(rC, pEmp).size() == 1u && s.visibility_mask(rC, pEmp) == kVisPlanetOwner);
    // A second key: appended; the fold ORs both, list order kept by a later upsert of the first.
    CHECK(s.story_set_planet_visibility(rC, "EMPIRE", "Super_Weapons", "Second"));
    CHECK(s.story_set_planet_visibility(rC, "Empire", "Fleet_Presence", "Fondor_Reveal"));
    mods = s.visibility_modifiers(rC, pEmp);
    CHECK(mods.size() == 2u && mods[0].key.sub == special_name_crc("Fondor_Reveal") && mods[0].value == 0x10u &&
          mods[1].value == 0x40000u);
    CHECK(s.visibility_mask(rC, pEmp) == 0x40010u);
    // "none" removes (case-insensitive); twice -> nothing to remove.
    CHECK(s.story_set_planet_visibility(rC, "Empire", " NONE ", "Second"));
    CHECK(!s.story_set_planet_visibility(rC, "Empire", "none", "Second"));
    CHECK(s.story_set_planet_visibility(rC, "Empire", "none", "Fondor_Reveal"));
    CHECK(s.visibility_modifiers(rC, pEmp).empty() && w.digest() == d0);  // tail gone
    // Bad input changes nothing.
    CHECK(!s.story_set_planet_visibility(rC, "Hutts", "Maximum", "K") && !s.story_set_planet_visibility(rC, "Empire", "Bogus", "K") &&
          !s.story_set_planet_visibility(99, "Empire", "Maximum", "K"));
    CHECK(w.digest() == d0);
    // Another player's list is separate.
    CHECK(s.story_set_planet_visibility(rA, "Rebel", "Credit_Income", "K"));
    CHECK(s.visibility_sources(rA, pReb).stored == 0x8u && s.visibility_sources(rA, pEmp).stored == 0u);
    // P5 seconds: 2 s = 60 frames; 0 s expires on the next frame's service.
    const int32_t f0 = s.frame();
    CHECK(s.story_set_planet_visibility(rE, "Empire", "Num_Fleets", "Timed", 2.0f));
    CHECK(s.story_set_planet_visibility(rE, "Empire", "Fleet_Contents", "Instant", 0.0f));
    mods = s.visibility_modifiers(rE, pEmp);
    CHECK(mods.size() == 2u && mods[0].expiry == static_cast<uint32_t>(f0 + 60) && mods[1].expiry == static_cast<uint32_t>(f0));
    w.run(1);  // service at f0: f0 < f0 is false
    CHECK(s.visibility_modifiers(rE, pEmp).size() == 2u);
    w.run(1);  // service at f0 + 1
    CHECK(s.visibility_modifiers(rE, pEmp).size() == 1u && s.visibility_mask(rE, pEmp) == 0x20u);
    w.run(60);  // last service at f0 + 61 > f0 + 60
    CHECK(s.visibility_modifiers(rE, pEmp).empty());
}

void test_api() {
    std::printf("  add / remove / fold\n");
    World w;
    GalacticSim& s = w.sim;
    const uint64_t d0 = w.digest();
    VisibilityKey k1, k2;
    k1.object = 10;
    k2.object = 10;
    k2.sub = 1;
    CHECK(s.add_visibility_modifier(rB, pEmp, k1, 0x2, -1.0f));
    CHECK(s.add_visibility_modifier(rB, pEmp, k2, 0x4, -1.0f, 77));
    CHECK(s.visibility_mask(rB, pEmp) == 0x6u);
    CHECK(s.add_visibility_modifier(rB, pEmp, k1, 0x8, -1.0f));  // upsert keeps the first position
    auto mods = s.visibility_modifiers(rB, pEmp);
    CHECK(mods.size() == 2u && mods[0].key == k1 && mods[0].value == 0x8u && mods[1].name_hash == 77u);
    CHECK(s.visibility_mask(rB, pEmp) == 0xCu);
    // Fold with the derived sources: an AI Rebel at B gets its default | stored.
    CHECK(s.add_visibility_modifier(rB, pReb, k1, 0x40000, -1.0f));
    CHECK(s.visibility_mask(rB, pReb) == (kVisGoodDefault | 0x40000u));
    CHECK(!s.add_visibility_modifier(99, pEmp, k1, 1, -1.0f) && !s.add_visibility_modifier(rB, 7, k1, 1, -1.0f));
    CHECK(!s.remove_visibility_modifier(rB, pUnd, k1));
    CHECK(s.remove_visibility_modifier(rB, pEmp, k1) && s.remove_visibility_modifier(rB, pEmp, k2) &&
          s.remove_visibility_modifier(rB, pReb, k1));
    CHECK(w.digest() == d0);
    // Trade routes: both endpoints' raw revealed bytes (A is the Empire's: revealed by the first pre-step).
    w.run(1);
    CHECK(s.planet_revealed_raw(rA) && !s.planet_revealed_raw(rB));
    CHECK(!s.trade_route_visible(0) && !s.trade_route_visible(99));
    s.set_planet_revealed(rB, true);
    CHECK(s.trade_route_visible(0) && !s.trade_route_visible(1));
}

// REMAINING_GAPS 6.3 (corrected): the per-bit planet-info queries.
void test_planet_info() {
    std::printf("  planet info queries\n");
    using PI = GalacticSim::PlanetInfo;
    struct Row {
        PI q;
        uint32_t bits;  // any of these grants the query
        bool bypass;
        uint8_t gate;  // 1 starbase, 2 ground, 3 both
    };
    const Row rows[] = {
        {PI::StarbaseLevel, 0x2, true, 1},         {PI::GroundBaseLevel, 0x4, true, 2},
        {PI::FleetContents, 0x40, true, 1},        {PI::FleetPresence, 0x30, true, 1},
        {PI::NumFleets, 0x20, true, 1},            {PI::GroundCompanyContents, 0x100, true, 2},
        {PI::NumGroundCompanies, 0x180, true, 2},  {PI::MostPowerfulShip, 0x10000, false, 1},
        {PI::SpecialStructures, 0x200, true, 3},   {PI::PoliticalControl, 0xC00, true, 0},
        {PI::CreditIncome, 0x8008, false, 0},      {PI::CreditIncomeBreakdown, 0x8000, false, 0},
        {PI::MinorStealthHeroes, 0x2000, true, 0}, {PI::MajorStealthHeroes, 0x4000, true, 0},
        {PI::ForceSensitiveHeroes, 0x20000, false, 0},
    };
    World w;
    GalacticSim& s = w.sim;
    CHECK(GalacticSim::kPlanetVisStarbase == 0x2u && GalacticSim::kPlanetVisGroundbase == 0x4u);
    // Empire (local) at the Rebel planet C: mask 0, so nothing is visible; each single bit opens exactly its queries.
    CHECK(s.visibility_mask(rC, pEmp) == 0u);
    for (const Row& r : rows) CHECK(!s.can_see_planet_info(pEmp, rC, r.q));
    VisibilityKey k;
    k.object = 99;
    for (uint32_t b = 0; b < 19; ++b) {
        const uint32_t bit = 1u << b;
        CHECK(s.add_visibility_modifier(rC, pEmp, k, bit, -1.0f));
        for (const Row& r : rows) CHECK(s.can_see_planet_info(pEmp, rC, r.q) == ((r.bits & bit) != 0));
    }
    CHECK(s.can_see_starbase_info(pEmp, rC) == false);  // last bit 0x40000
    CHECK(s.add_visibility_modifier(rC, pEmp, k, kVisStarbaseLevel, -1.0f) && s.can_see_starbase_info(pEmp, rC) &&
          !s.can_see_groundbase_info(pEmp, rC));
    // Stealth gates: a Rebel starbase and ground structure at C, both flags on; the Empire has Maximum there.
    CHECK(static_cast<bool>(s.spawn_unit(pReb, rC, tSB)) && static_cast<bool>(s.spawn_unit(pReb, rC, tOLRS)));
    CHECK(s.planets()[rC].starbase_owner == pReb && s.planets()[rC].starbase_level == 1);
    CHECK(s.add_visibility_modifier(rC, pEmp, k, kVisMaximum, -1.0f));
    w.rules.types[tPC].starbase_stealth = true;
    for (const Row& r : rows) CHECK(s.can_see_planet_info(pEmp, rC, r.q) == ((r.gate & 1) == 0));
    w.rules.types[tPC].groundbase_stealth = true;
    for (const Row& r : rows) CHECK(s.can_see_planet_info(pEmp, rC, r.q) == (r.gate == 0));
    // Allied with the base owner: the gates pass.
    s.set_relation(pEmp, pReb, Relation::Ally);
    for (const Row& r : rows) CHECK(s.can_see_planet_info(pEmp, rC, r.q));
    // Base-level fallback: allied with the base owner shows the level even without the bit.
    CHECK(s.remove_visibility_modifier(rC, pEmp, k));
    CHECK(s.can_see_starbase_info(pEmp, rC) && s.can_see_groundbase_info(pEmp, rC) &&
          !s.can_see_planet_info(pEmp, rC, PI::FleetContents));
    s.reset_relations();
    w.rules.types[tPC].starbase_stealth = w.rules.types[tPC].groundbase_stealth = false;
    CHECK(!s.can_see_starbase_info(pEmp, rC) && !s.can_see_groundbase_info(pEmp, rC));
    // Faction +0x20ac [M] <= the base level (starbase 1, ground base 1 structure).
    w.rules.factions[0].info_base_level_threshold = 1;
    CHECK(s.can_see_starbase_info(pEmp, rC) && s.can_see_groundbase_info(pEmp, rC));
    w.rules.factions[0].info_base_level_threshold = 2;
    CHECK(!s.can_see_starbase_info(pEmp, rC) && !s.can_see_groundbase_info(pEmp, rC));
    w.rules.factions[0].info_base_level_threshold = INT32_MAX;
    // Corruption bypass: the Underworld (Benefits_From_Corruption) at a corrupted C sees the bypass queries only.
    s.set_corruption(rC, 1, pUnd);
    const uint32_t und = s.visibility_mask(rC, pUnd);
    for (const Row& r : rows) CHECK(s.can_see_planet_info(pUnd, rC, r.q) == (r.bypass || (und & r.bits) != 0));
    CHECK(!s.can_see_planet_info(pEmp, rC, PI::PoliticalControl));  // the Empire does not benefit
    // Invalid inputs.
    CHECK(!s.can_see_planet_info(-1, rC, PI::PoliticalControl) && !s.can_see_planet_info(pEmp, 99, PI::PoliticalControl) &&
          !s.can_see_planet_info(pEmp, rC, static_cast<PI>(200)));
}

// PlayerView::ownership_visible / local_orbit_visible = the LOCAL mask bits 0x1 / 0x10 (whatever the mover).
void test_player_view() {
    std::printf("  player view\n");
    World w;  // Empire local
    GalacticSim& s = w.sim;
    PlayerView v = s.player_view(pEmp);
    CHECK(v.ownership_visible.size() == 5u && v.local_orbit_visible.size() == 5u);
    CHECK(v.ownership_visible.test(rA) && v.local_orbit_visible.test(rA));    // owner: 0x9FFF
    CHECK(!v.ownership_visible.test(rC) && !v.local_orbit_visible.test(rC));  // Empire default 0
    // Passability (Q5 step 3, human): hidden ownership counts as unknown.
    PathRequest req;
    req.allow_unknown_or_neutral = false;
    CHECK(!v.is_passable(rC, req));
    CHECK(s.story_set_planet_visibility(rC, "Empire", "Fleet_Presence", "K"));
    v = s.player_view(pEmp);
    CHECK(!v.ownership_visible.test(rC) && v.local_orbit_visible.test(rC));
    CHECK(s.story_set_planet_visibility(rC, "Empire", "Planet_Ownership", "K"));
    v = s.player_view(pEmp);
    CHECK(v.ownership_visible.test(rC) && !v.local_orbit_visible.test(rC) && v.is_passable(rC, req));
    // Another mover still gets the LOCAL bits (quirk of the original).
    const PlayerView vr = s.player_view(pReb);
    CHECK(vr.ownership_visible.test(rC) && vr.ownership_visible.test(rA) && !vr.ownership_visible.test(rB));
    // Rebel local: the Good default 0x11 carries both bits.
    World r(pReb);
    const PlayerView rv = r.sim.player_view(pReb);
    CHECK(rv.ownership_visible.test(rA) && rv.local_orbit_visible.test(rA) && rv.ownership_visible.test(rE));
    // No local player: empty (= all visible).
    World n(-1);
    CHECK(n.sim.local_player() == kNoPlayer);
    const PlayerView nv = n.sim.player_view(pEmp);
    CHECK(nv.ownership_visible.empty() && nv.local_orbit_visible.empty() && nv.is_passable(rC, PathRequest{}));
}

// A2 t01: PlayerView::ai_perceived_non_neutral for a mover with an AI system (stand-in: the true owner, no fogged memory).
void test_player_view_ai_perceived() {
    std::printf("  player view ai perceived owner\n");
    World w;  // Empire human; Rebel and Underworld are AI players; A = Emp, C = Reb, D = Und, B and E neutral-owned
    GalacticSim& s = w.sim;
    CHECK(s.planets()[rB].owner == pNeu && s.planets()[rE].owner == pNeu);  // start() gives the rest to the neutral faction
    const PlayerView rv = s.player_view(pReb);  // AI player
    CHECK(rv.has_ai_system && !rv.is_human && rv.ai_perceived_non_neutral.size() == 5u);
    CHECK(rv.ai_perceived_non_neutral.test(rA));   // enemy planet (Empire): flag 1
    CHECK(rv.ai_perceived_non_neutral.test(rC));   // own planet
    CHECK(rv.ai_perceived_non_neutral.test(rD));   // another enemy
    CHECK(!rv.ai_perceived_non_neutral.test(rB));  // owned by the neutral faction: flag 0
    CHECK(!rv.ai_perceived_non_neutral.test(rE));
    CHECK(rv.ai_eval_failed.empty());              // the sim view never fails the evaluation
    // The AI branch of Is_Planet_Passable_For_Player now follows the perceived owner (no ally / corruption shortcut here).
    PathRequest enemy_only;
    enemy_only.allow_unknown_or_neutral = false;
    enemy_only.allow_enemy_owned = true;
    PathRequest neutral_only = enemy_only;
    neutral_only.allow_unknown_or_neutral = true;
    neutral_only.allow_enemy_owned = false;
    CHECK(rv.is_passable(rA, enemy_only) && !rv.is_passable(rA, neutral_only));
    CHECK(!rv.is_passable(rB, enemy_only) && rv.is_passable(rB, neutral_only));
    CHECK(!rv.is_passable(rE, enemy_only) && rv.is_passable(rE, neutral_only));
    // The bit is the inverse of unowned_or_neutral on every planet row.
    for (PlanetIndex i = 0; i < 5; ++i) CHECK(rv.ai_perceived_non_neutral.test(i) == !rv.unowned_or_neutral.test(i));
    // A human mover without an AI system gets no AI fields (human branch uses unowned_or_neutral).
    const PlayerView ev = s.player_view(pEmp);
    CHECK(!ev.has_ai_system && ev.ai_perceived_non_neutral.empty());
    // A human with an AI system has the field filled but the human branch still decides.
    World h(pReb);  // Rebel human
    const PlayerView hv = h.sim.player_view(pEmp);  // Empire is an AI player now
    CHECK(hv.has_ai_system && hv.ai_perceived_non_neutral.test(rC) && !hv.ai_perceived_non_neutral.test(rB));
    // Determinism: repeated calls are identical.
    const PlayerView rv2 = s.player_view(pReb);
    CHECK(rv2.ai_perceived_non_neutral.size() == rv.ai_perceived_non_neutral.size());
    for (PlanetIndex i = 0; i < 5; ++i) CHECK(rv2.ai_perceived_non_neutral.test(i) == rv.ai_perceived_non_neutral.test(i));
}

void test_determinism() {
    std::printf("  determinism\n");
    auto script = [](World& w) {
        GalacticSim& s = w.sim;
        s.story_set_planet_visibility(rC, "Empire", "Maximum", "A", 3.0f);
        const ObjectHandle pr = s.spawn_unit(pEmp, rB, tProbeWide);
        s.ground_activated_trigger(pr, rB);
        s.spawn_unit(pReb, rE, tSmuggler);
        std::vector<uint64_t> log;
        for (int i = 0; i < 1000; ++i) {
            w.run(1);
            if (i == 500) s.story_set_planet_visibility(rA, "Underworld", "Planet_Owner", "B", 1.0f);
            uint64_t m = 0;
            for (PlanetIndex p = 0; p < 5; ++p)
                for (int32_t pl = 0; pl < 4; ++pl) m = m * 1099511628211ull + s.visibility_mask(p, pl);
            log.push_back(w.digest() ^ m);
        }
        return log;
    };
    World a, b;
    const std::vector<uint64_t> la = script(a), lb = script(b);
    CHECK(la == lb);
    CHECK(a.sim.visibility_modifiers(rC, pEmp).empty() && a.sim.visibility_modifiers(rB, pEmp).empty());  // all expired
}

// ---- 5. FoC data ------------------------------------------------------------------------------------------------------

using ::alamo::test::env_or_empty;

const SpecialAbilityDef* find_spy(const GalacticRules& r, uint32_t type) {
    for (uint32_t a : r.special.type(type).abilities)
        if (r.special.abilities[a].cls == SpecialAbilityClass::SystemSpy) return &r.special.abilities[a];
    return nullptr;
}

void test_foc() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    std::string how;
    std::unique_ptr<FileProvider> provider = testing::open_foc_data(root, how);
    if (!provider) {
        std::printf("ALAMO_FOC_DATA unset or without Data\\XML (loose, or a FoC install through the VFS): FoC spy data checks skipped\n");
        return;
    }
    std::printf("  FoC data: %s\n", how.c_str());
    std::vector<std::string> diag;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    GalacticRules rules;
    CHECK(db.load(*provider, {}, &diag));
    CHECK(raw.load(*provider, &diag));
    CHECK(types.build(raw, provider.get(), &diag));
    CHECK(load_galactic_rules(*provider, db, types, rules, &diag));
    // Probe_Droid_System_Spy: Ground_Activated, 60 s, Base_Levels | Num_Fleets | Fleet_Contents | Num_Ground_Companies |
    // Ground_Company_Contents | Special_Structures | Political_Control.
    const uint32_t probe = rules.find_type("Generic_Probe_Droid");
    const SpecialAbilityDef* p = find_spy(rules, probe);
    CHECK(p && p->name == "Probe_Droid_System_Spy" && p->style == ActivationStyle::GroundActivated && p->spy_duration == 60.0f &&
          p->spy_mask == 0x7F6u && p->causes_despawn);
    CHECK(probe != kNoIndex && rules.special.type(probe).auto_spy_mask == 0u);
    // Smuggler_Limited_Spy: Galactic_Automatic, Num_Fleets, never expires; the type's always-on mask.
    const uint32_t sm = rules.find_type("Generic_Smuggler_E");
    const SpecialAbilityDef* q = find_spy(rules, sm);
    CHECK(q && q->name == "Smuggler_Limited_Spy" && q->style == ActivationStyle::GalacticAutomatic && q->spy_duration == -1.0f &&
          q->spy_mask == 0x30u);
    CHECK(sm != kNoIndex && rules.special.type(sm).auto_spy_mask == 0x30u);
    // Bothawui's network (a planet type) and Mara Jade (range 100).
    const uint32_t bo = rules.find_type("Bothawui");
    CHECK(bo != kNoIndex && (rules.special.type(bo).auto_spy_mask & 0x6u) == 0x6u && (rules.special.type(bo).auto_spy_mask & 1u) == 0u);
    std::printf("    Bothawui spy mask 0x%X\n", bo != kNoIndex ? rules.special.type(bo).auto_spy_mask : 0u);
    const uint32_t mj = rules.find_type("Mara_Jade");
    CHECK(mj != kNoIndex && rules.special.type(mj).auto_spy_mask == 0x206u && rules.types[mj].influence_range == 100.0f);
    // Every spy: no bit 0, a known style, prerequisites present.
    uint32_t n = 0;
    for (const SpecialAbilityDef& a : rules.special.abilities) {
        if (a.cls != SpecialAbilityClass::SystemSpy) continue;
        ++n;
        CHECK((a.spy_mask & 1u) == 0u);
        CHECK(a.style == ActivationStyle::GalacticAutomatic || a.style == ActivationStyle::GroundActivated);
        if (a.spy_mask & 0x10040u) CHECK((a.spy_mask & 0x30u) == 0x30u);
        if (a.spy_mask & 0x100u) CHECK(a.spy_mask & 0x80u);
        if (a.spy_mask & 0x800u) CHECK(a.spy_mask & 0x400u);
        if (a.spy_mask & 0x8000u) CHECK(a.spy_mask & 0x8u);
    }
    std::printf("    %u System_Spy_Ability definitions\n", n);
    CHECK(n >= 10);
}

}  // namespace

int main() {
    test_parse();
    test_baseline();
    test_presence();
    test_auto_spy();
    test_ground_spy();
    test_story();
    test_api();
    test_planet_info();
    test_player_view();
    test_player_view_ai_perceived();
    test_determinism();
    test_foc();
    if (g_fail) {
        std::printf("%d of %d check(s) failed\n", g_fail, g_checks);
        return 1;
    }
    std::printf("alamo_galactic_visibility_tests: all %d checks passed\n", g_checks);
    return 0;
}
