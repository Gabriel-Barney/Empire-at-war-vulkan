// alamo_galactic_special_tests: P4 sim, the FoC galactic special systems (special_systems.hpp,
// docs/re/P4_SIM_SYSTEMS.md = P4).
//   1. payloads (SpecialAbility 13 kinds 1 / 4 / 5 / 7) and quantization;
//   2. hand-computed cases on a synthetic galaxy: hero respawn scheduling (incl. the negative-default quirk), black
//      market offers / pricing / AI pick / human command / gate, sabotage (AI, human command, vigilance, halt, cost
//      escalation), hack + planet destruction + type lock, income bonus / gambling abilities and the by-tag breakdown
//      (vanilla mining and Thrawn's Revenge-style absolute / negative entries), corruption requirements / charge order /
//      AI pick / consumed prerequisites / AI apply, command dispatch;
//   3. a scripted run with every system whose per-frame CRC log must be identical for 0 / 1 / 4 / N workers;
//   4. FoC data (ALAMO_FOC_DATA = folder with loose Data\XML; skipped when unset): the tables and a palace / mining
//      income check on Sandbox_Heart_Of_The_Maw_Underworld.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alamo/commands/event_types.hpp"
#include "alamo/commands/pipeline.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "foc_data.hpp"
#include "thread_parallel_for.hpp"

using namespace alamo;
using namespace alamo::galactic;
using commands::RejectReason;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

// ---- synthetic galaxy -------------------------------------------------------------------------------------------------

enum TypeId : uint32_t {
    tPA, tPB, tPC, tPD, tPE,  // planets
    tSB1, tSB2, tFrigate, tDS2,
    tMine, tTRMine, tUpkeep, tArena, tSpecial, tSpecial0,
    tTyber, tUrai, tSaboteur, tHalter, tIG88, tMerc,
    tTypeCount
};
enum Row : PlanetIndex { rA = 0, rB = 1, rC = 2, rD = 3, rE = 4 };
enum PlayerId : int32_t { pEmp = 0, pReb = 1, pUnd = 2, pNeu = 3 };
enum AbilityId : uint32_t { aBMTyber, aBMUrai, aSab, aCor, aHalt, aHack, aMine, aTR, aUpkeep, aArena, aCount };

SpecialAbilityDef ability(SpecialAbilityClass cls, const char* name) {
    SpecialAbilityDef a;
    a.cls = cls;
    a.name = name;
    a.name_crc = special_name_crc(name);
    a.style = (cls == SpecialAbilityClass::IncomeBonus || cls == SpecialAbilityClass::IncomeGambling) ? ActivationStyle::GalacticAutomatic
                                                                                                     : ActivationStyle::GroundActivated;
    a.causes_despawn = cls == SpecialAbilityClass::BlackMarket || cls == SpecialAbilityClass::Sabotage || cls == SpecialAbilityClass::Corruption;
    return a;
}

GalacticRules make_rules() {
    GalacticRules r;
    auto faction = [](const char* n, bool neutral, bool benefits, int32_t adj) {
        FactionRules f;
        f.name = n;
        f.is_neutral = neutral;
        f.can_control_planets = f.is_playable = !neutral;
        f.benefits_from_corruption = benefits;
        f.displayed_tech_level_adjustment = adj;
        return f;
    };
    r.factions = {faction("Empire", false, false, 0), faction("Rebel", false, false, 1), faction("Underworld", false, true, 0),
                  faction("Neutral", true, false, 0)};
    r.types.resize(tTypeCount);
    const char* pn[] = {"PA", "PB", "PC", "PD", "PE"};
    const int32_t credit[] = {100, 20, 50, 30, 40};
    for (uint32_t i = 0; i < 5; ++i) {
        r.types[i].name = pn[i];
        r.types[i].kind = UnitKind::Planet;
        r.types[i].credit_value = credit[i];
        r.types[i].destroyed_credit_value = 2;
    }
    auto unit = [&r](uint32_t id, const char* name, UnitKind k) -> TypeInfo& {
        r.types[id].name = name;
        r.types[id].kind = k;
        return r.types[id];
    };
    unit(tSB1, "SB1", UnitKind::StarBase).base_level = 1;
    unit(tSB2, "SB2", UnitKind::StarBase).base_level = 2;
    unit(tFrigate, "Frigate", UnitKind::SpaceUnit).ai_combat_power = 100.0f;
    unit(tDS2, "Death_Star_II", UnitKind::SpaceUnit);
    unit(tMine, "Mine", UnitKind::GroundStructure);
    unit(tTRMine, "TR_Mine", UnitKind::GroundStructure);
    unit(tUpkeep, "TR_Upkeep", UnitKind::GroundStructure);
    unit(tArena, "Arena", UnitKind::GroundStructure);
    unit(tSpecial, "Special", UnitKind::GroundStructure);
    unit(tSpecial0, "Special0", UnitKind::GroundStructure);
    unit(tTyber, "Tyber", UnitKind::GroundUnit);
    unit(tUrai, "Urai", UnitKind::GroundUnit);
    unit(tSaboteur, "Saboteur", UnitKind::GroundUnit);
    unit(tHalter, "Halter", UnitKind::GroundUnit);
    unit(tIG88, "IG88", UnitKind::GroundUnit);
    unit(tMerc, "Merc", UnitKind::GroundUnit);

    SpecialRules& s = r.special;
    s.abilities.resize(aCount);
    s.abilities[aBMTyber] = ability(SpecialAbilityClass::BlackMarket, "Tyber_Zann_Black_Market");
    s.abilities[aBMTyber].bm_min_respawn.assign(5, 90.0f);
    s.abilities[aBMTyber].bm_max_respawn.assign(5, 90.0f);
    s.abilities[aBMUrai] = ability(SpecialAbilityClass::BlackMarket, "Urai_Fen_Black_Market");
    s.abilities[aBMUrai].bm_min_respawn.assign(5, 30.0f);
    s.abilities[aBMUrai].bm_max_respawn.assign(5, 30.0f);
    s.abilities[aBMUrai].price_modifier = 1.5f;
    s.abilities[aSab] = ability(SpecialAbilityClass::Sabotage, "Saboteur_Galactic_Sabotage");
    s.abilities[aSab].sabotage_can[kSabotageSpecialStructure] = true;
    s.abilities[aSab].min_respawn = s.abilities[aSab].max_respawn = 5.0f;
    s.abilities[aCor] = ability(SpecialAbilityClass::Corruption, "Saboteur_Corruption");
    s.abilities[aHalt] = ability(SpecialAbilityClass::Sabotage, "Halter_Sabotage");
    s.abilities[aHalt].sabotage_can[kSabotageHalt] = true;
    s.abilities[aHalt].halt_duration = 10.0f;
    s.abilities[aHack] = ability(SpecialAbilityClass::Hack, "IG88_Hack_Death_Star");
    s.abilities[aMine] = ability(SpecialAbilityClass::IncomeBonus, "Empire_Mining_Income_Bonus");
    s.abilities[aMine].income_percentage = 2.0f;
    s.abilities[aMine].mining_multiplier = 1.5f;
    s.abilities[aMine].source_text = "TEXT_ECONOMY_MINING_FACILITIES";
    s.abilities[aMine].source_tag = special_name_crc("TEXT_ECONOMY_MINING_FACILITIES");
    s.abilities[aTR] = ability(SpecialAbilityClass::IncomeBonus, "Mining_Income_Bonus");  // TR: 0 / +400
    s.abilities[aTR].income_absolute = 400.0f;
    s.abilities[aTR].source_tag = special_name_crc("TEXT_TR_MINING");
    s.abilities[aUpkeep] = ability(SpecialAbilityClass::IncomeBonus, "Maintenance_Cost");  // TR: 0 / -3000, no source text
    s.abilities[aUpkeep].income_absolute = -3000.0f;
    s.abilities[aArena] = ability(SpecialAbilityClass::IncomeGambling, "Grand_Arena_Gambling_Income");
    s.abilities[aArena].evaluation_interval = 30.0f;
    s.abilities[aArena].min_income_modifier = -0.2f;
    s.abilities[aArena].max_income_modifier = 0.6f;
    s.abilities[aArena].chance_to_win = 0.8f;

    s.types.resize(tTypeCount);
    s.types[tPA].mining_colony = true;
    s.types[tPA].black_market_modifier = 300.0f;
    s.types[tPC].planet_vigilance = true;
    {  // corruption choices of PB
        CorruptionChoiceDef c0;
        c0.slot = 1;
        c0.type = 1;  // Piracy
        c0.credit_cost = 500.0f;
        c0.required_unit_types = {tMerc};
        c0.required_unit_count = 2;
        CorruptionChoiceDef c1;
        c1.slot = 2;
        c1.type = 0;  // Intimidation
        c1.required_mission = 1;
        c1.required_mission_count = 1;
        CorruptionChoiceDef c2;
        c2.slot = 3;
        c2.type = 6;  // Black_Market
        c2.credit_cost = 200.0f;
        c2.required_planets = {tPD};
        s.types[tPB].corruption = {c0, c1, c2};
    }
    s.types[tSB2].prev_level_base = tSB1;
    auto give = [&s](uint32_t t, std::vector<uint32_t> abil) {
        s.types[t].abilities = std::move(abil);
        for (uint32_t a : s.types[t].abilities)
            if (s.abilities[a].cls == SpecialAbilityClass::IncomeBonus || s.abilities[a].cls == SpecialAbilityClass::IncomeGambling)
                s.types[t].has_income_ability = true;
    };
    give(tMine, {aMine});
    give(tTRMine, {aTR});
    give(tUpkeep, {aUpkeep});
    s.types[tUpkeep].text_crc = special_name_crc("TEXT_TR_UPKEEP");
    give(tArena, {aArena});
    s.types[tArena].text_crc = special_name_crc("TEXT_ARENA");
    s.types[tSpecial].sabotage_cost = 1000;
    s.types[tSpecial].tutorial_sabotage_cost = 50;
    give(tTyber, {aBMTyber});
    s.types[tTyber].named_hero = true;
    give(tUrai, {aBMUrai});
    s.types[tUrai].named_hero = true;
    give(tSaboteur, {aSab, aCor});
    s.types[tSaboteur].generic_hero = true;
    give(tHalter, {aHalt});
    s.types[tHalter].generic_hero = true;
    give(tIG88, {aHack});
    s.types[tIG88].named_hero = true;
    s.hack_required_type = tDS2;
    s.hack_cost = 3000.0f;  // test value (the cap would block the vanilla 50000 with one planet)
    s.activated_sabotage = {"Saboteur_Galactic_Sabotage"};

    BlackMarketItemDef i0;
    i0.name = "Buzz_Droid_Tech";
    i0.faction = 0;  // Empire
    i0.tech_level = 2;
    i0.base_cost = 1000;
    i0.uncommon_price_multiplier = 5.5f;
    i0.ability_names = {"Buzz_Droids_Viper"};
    i0.tutorial_base_cost = 100;
    BlackMarketItemDef i1;
    i1.name = "Proximity_Mine_Tech";
    i1.faction = 1;  // Rebel
    i1.tech_level = 3;
    i1.base_cost = 1000;
    i1.uncommon_price_multiplier = 5.5f;
    i1.ability_names = {"Grenadier_Proximity"};
    BlackMarketItemDef i2;
    i2.name = "Generic_Tech";
    i2.base_cost = 500;
    for (BlackMarketItemDef* d : {&i0, &i1, &i2}) {
        d->name_crc = special_name_crc(d->name);
        for (const std::string& a : d->ability_names) d->ability_crcs.push_back(special_name_crc(a));
    }
    s.items = {i0, i1, i2};
    return r;
}

GalacticSetup make_setup(bool und_human, bool emp_human = true) {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "P4Synthetic";
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
        m.routes.credit_gain_factor[r] = 0.0f;
    };
    route("A_B", rA, rB);
    route("B_C", rB, rC);
    route("B_D", rB, rD);
    route("C_E", rC, rE);
    s.planet_type = {tPA, tPB, tPC, tPD, tPE};
    auto player = [](uint32_t f, bool human, float credits) {
        PlayerSetup p;
        p.faction = f;
        p.human = human;
        p.ai = !human;
        p.credits = credits;
        return p;
    };
    s.players = {player(0, emp_human, 5000.0f), player(1, false, 5000.0f), player(2, und_human, 5000.0f), player(3, false, 0.0f)};
    s.players[3].ai = false;
    s.forces = {{pEmp, rA, tMine}, {pEmp, rB, tSpecial}, {pReb, rC, tSpecial}, {pUnd, rD, tArena}};
    return s;
}

struct World {
    GalacticRules rules = make_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
    SimRng rng{0x5EEDu};
    GalacticSim sim{rules, store, rng};
    explicit World(bool und_human = false, bool emp_human = true, float default_respawn = 360.0f) {
        rules.special.default_hero_respawn_time = default_respawn;
        std::vector<std::string> diag;
        CHECK(sim.start(make_setup(und_human, emp_human), &diag));
        sim.set_emit_events(true);
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) sim.update();
    }
    const PlanetState& planet(PlanetIndex p) const { return sim.planets()[p]; }
    uint32_t pid(PlanetIndex p) const { return static_cast<uint32_t>(store.id(planet(p).object)); }
    uint32_t oid(ObjectHandle h) const { return static_cast<uint32_t>(store.id(h)); }
    size_t count(GalacticEventKind k) const {
        size_t n = 0;
        for (const GalacticEvent& e : sim.events()) n += e.kind == k ? 1u : 0u;
        return n;
    }
    const GalacticEvent* last(GalacticEventKind k) const {
        const GalacticEvent* out = nullptr;
        for (const GalacticEvent& e : sim.events())
            if (e.kind == k) out = &e;
        return out;
    }
};

int32_t due_after(int32_t frame, float seconds) { return frame + static_cast<int32_t>(30.0f * seconds + 0.5f); }

// ---- 1. payloads ------------------------------------------------------------------------------------------------------

void test_payloads() {
    SpecialPayload p;
    CHECK(read_special_payload(special_activate_event(7, 9), p) && p.kind == kSpecialActivate && p.planet == 7 && p.unit == 9);
    CHECK(read_special_payload(black_market_purchase_event(1, 2, 0xABCDu, 1234.5f, 90.0f, true), p) && p.kind == kSpecialBlackMarket &&
          p.planet == 1 && p.unit == 2 && p.item_crc == 0xABCDu && p.only_one &&
          p.amount == commands::requantize(kQuantAmount, 1234.5f) && p.seconds == commands::requantize(kQuantDuration, 90.0f));
    CHECK(std::fabs(p.amount - 1234.5f) < 0.1f && std::fabs(p.seconds - 90.0f) < 0.01f);
    CHECK(read_special_payload(sabotage_structure_event(3, 4, 5, 2000.0f, 0.0f), p) && p.kind == kSpecialSabotage && p.structure == 5 &&
          p.amount == commands::requantize(kQuantAmount, 2000.0f) && p.seconds == 0.0f);
    CHECK(read_special_payload(hack_super_weapon_event(3, 4, 50000.0f), p) && p.kind == kSpecialHack &&
          p.amount == commands::requantize(kQuantAmount, 50000.0f));
    // Kind 6 (corruption) and other events are not read here (apps/sim CampaignBattles decodes kind 6).
    commands::EventData k6(commands::EventType::SpecialAbility);
    k6.set("kind", kSpecialCorruption);
    CHECK(!read_special_payload(k6, p));
    CHECK(!read_special_payload(commands::ev::stop(), p));
    // Activation-style names and the name CRC (bytes as written).
    CHECK(parse_activation_style(" Ground_Activated ") == ActivationStyle::GroundActivated &&
          parse_activation_style("galactic_automatic") == ActivationStyle::GalacticAutomatic &&
          parse_activation_style("COMBAT_AUTOMATIC") == ActivationStyle::CombatAutomatic &&
          parse_activation_style("x") == ActivationStyle::None);
    CHECK(special_name_crc("abc") == 0x352441C2u && special_name_crc("ABC") != special_name_crc("abc"));
}

// ---- 2a. respawns -----------------------------------------------------------------------------------------------------

void test_respawn() {
    World w;
    GalacticSim& s = w.sim;
    const ObjectHandle h = s.spawn_unit(pEmp, rC, tTyber);  // an Empire hero at the Rebel planet C
    CHECK(w.store.alive(h));
    const int32_t f0 = s.frame();
    CHECK(s.schedule_hero_respawn(h, rC, 10.0f));
    CHECK(s.respawns_pending(rC).size() == 1 && s.respawns_pending(rC)[0].due == f0 + 300 && s.respawns_pending(rC)[0].owner == pEmp);
    CHECK(s.schedule_hero_respawn(h, rC, 20.0f) && s.respawns_pending(rC).size() == 1);  // named hero: not queued twice
    CHECK(!s.special_state_default());
    // Due at frame f0 + 300: spawned at the owner's planet nearest to C: B (60 away) before A (120).
    const size_t b_surface = w.planet(rB).surface.size();
    w.run(300);
    CHECK(s.respawns_pending(rC).size() == 1);
    w.run(1);
    CHECK(s.respawns_pending(rC).empty() && w.planet(rB).surface.size() == b_surface + 1 &&
          w.store.type(w.planet(rB).surface.back()) == tTyber && w.count(GalacticEventKind::HeroRespawned) == 1);
    const GalacticEvent* ev = w.last(GalacticEventKind::HeroRespawned);
    CHECK(ev && ev->planet == rB && ev->player == pEmp && ev->type == tTyber);
    // Default time: seconds <= 0 -> Default_Hero_Respawn_Time (360 s = 10800 frames).
    const ObjectHandle g = s.spawn_unit(pEmp, rA, tMerc);
    CHECK(s.hero_depart(g, rA, 0.0f) && !w.store.alive(g) && s.respawns_pending(rA).size() == 1 &&
          s.respawns_pending(rA)[0].due == s.frame() + 10800);
    // A player without planets (the Rebel once C is destroyed): the entry is dropped when due.
    CHECK(s.planet_destroy(rC, pEmp) && s.planets_owned(pReb) == 0);
    const ObjectHandle n = s.spawn_unit(pReb, rE, tMerc);
    CHECK(s.hero_depart(n, rE, 1.0f) && s.respawns_pending(rE).size() == 1);
    w.run(31);
    CHECK(s.respawns_pending(rE).empty() && w.planet(rE).surface.empty() && s.planets_owned(pReb) == 0);
    // Thrawn's Revenge: Default_Hero_Respawn_Time = -360 -> a default-time respawn queues NOTHING but reports success
    // (the original's quirk, kept); an explicit positive time still queues.
    World tr(false, true, -360.0f);
    const ObjectHandle th = tr.sim.spawn_unit(pEmp, rA, tTyber);
    CHECK(tr.sim.schedule_hero_respawn(th, rA, 0.0f) && tr.sim.respawns_pending(rA).empty());
    CHECK(tr.sim.hero_depart(th, rA, 0.0f) && !tr.store.alive(th) && tr.sim.respawns_pending(rA).empty() && tr.sim.special_state_default());
    const ObjectHandle th2 = tr.sim.spawn_unit(pEmp, rA, tTyber);
    CHECK(tr.sim.schedule_hero_respawn(th2, rA, 5.0f) && tr.sim.respawns_pending(rA).size() == 1);
    // A2 galactic: HIJACK_UNIT step 3 (AI_GALACTIC_3 §4.3) - magic_spawn drops the pending entries of its type on every
    // planet (any owner), other types stay.
    const ObjectHandle th3 = tr.sim.spawn_unit(pEmp, rB, tMerc);
    CHECK(tr.sim.schedule_hero_respawn(th3, rB, 5.0f) && tr.sim.respawns_pending(rB).size() == 1);
    CHECK(tr.sim.magic_spawn(pEmp, rA, tTyber, false, false));
    CHECK(tr.sim.respawns_pending(rA).empty() && tr.sim.respawns_pending(rB).size() == 1 && tr.sim.respawns_pending(rB)[0].type == tMerc);
}

// ---- 2b. black market -------------------------------------------------------------------------------------------------

void test_black_market() {
    {
        World w;  // Underworld AI
        GalacticSim& s = w.sim;
        s.set_corruption(rA, 6, pUnd);  // Corruption_Black_Market at the Empire planet A
        const ObjectHandle ty = s.spawn_unit(pUnd, rA, tTyber);
        // Offers at A (owner Empire): item 0 (Empire) and 2 (no faction); item 1 (Rebel) filtered.
        // item 0: (1000 + 300) x 1.0 x 1.0 = 1300; the first Empire player has tech 1 < 2 -> x 5.5 = 7150.
        // item 2: (500 + 300) = 800.
        const std::vector<BlackMarketOffer> o = s.bm_offers(pUnd, rA, ty);
        CHECK(o.size() == 2 && o[0].item == 0 && o[0].price == 7150.0f && o[1].item == 2 && o[1].price == 800.0f && !o[0].purchased);
        // Tutorial: Tutorial_Base_Cost unscaled (100), tutorial tech 0 -> no uncommon factor.
        s.set_bm_tutorial(pUnd, true);
        CHECK(s.bm_offers(pUnd, rA, ty)[0].price == 100.0f);
        s.set_bm_tutorial(pUnd, false);
        // Urai (Price_Modifier 1.5) at the Rebel planet C: item 1 (1000 + 0) x 1.5 = 1500; the Rebel player's tech is
        // 1 + Displayed_Tech_Level_Adjustment 1 = 2 < 3 -> x 5.5 = 8250; item 2: 500 x 1.5 = 750.
        const ObjectHandle ur = s.spawn_unit(pUnd, rC, tUrai);
        CHECK(s.player_tech(pReb) == 2 && s.player_tech(pEmp) == 1);
        const std::vector<BlackMarketOffer> oc = s.bm_offers(pUnd, rC, ur);
        CHECK(oc.size() == 2 && oc[0].item == 1 && oc[0].price == 8250.0f && oc[1].item == 2 && oc[1].price == 750.0f);
        // AI apply: R = Get_Float(90, 90) (tech 1), pick over {item 2: 800} with Get_Float(0, 800), purchase, departure,
        // then the trigger's Causes_Despawn draw (Get_Float(0, 0)): 3 synced draws.
        SimRng expect = w.rng;
        const float r = expect.range_float(90.0f, 90.0f);
        (void)expect.range_float(0.0f, 800.0f);
        (void)expect.range_float(0.0f, 0.0f);
        const float c0 = s.credits(pUnd);
        CHECK(s.ground_activated_trigger(ty, rA));
        CHECK(w.rng == expect && r == 90.0f);
        CHECK(s.credits(pUnd) == c0 - 800.0f && s.bm_purchased(pUnd).size() == 1 && s.bm_purchased(pUnd)[0] == 2 && s.bm_has_item(pUnd, 2));
        CHECK(!w.store.alive(ty) && s.respawns_pending(rA).size() == 1 && s.respawns_pending(rA)[0].due == due_after(s.frame(), 90.0f));
        CHECK(w.count(GalacticEventKind::BlackMarketPurchase) == 1 && w.last(GalacticEventKind::BlackMarketPurchase)->other_player == 0 &&
              w.count(GalacticEventKind::SpecialApplied) == 1);
        CHECK(s.bm_offers(pUnd, rA)[1].purchased);
        // Tyber comes back at Underworld's only planet D after 90 s.
        w.run(2701);
        bool back = false;
        for (ObjectHandle u : w.planet(rD).surface) back = back || (w.store.alive(u) && w.store.type(u) == tTyber);
        CHECK(back && s.respawns_pending(rA).empty());
        // Not accepted without Corruption_Black_Market (C is not BM-corrupted): nothing applies, no draw.
        const SimRng before = w.rng;
        bool accepted = true;
        CHECK(!s.ground_activated_trigger(ur, rC, &accepted) && !accepted && w.rng == before);
    }
    {
        World w(true);  // Underworld human: the dialog; the purchase is command kind 4
        GalacticSim& s = w.sim;
        s.set_corruption(rA, 6, pUnd);
        const ObjectHandle ty = s.spawn_unit(pUnd, rA, tTyber);
        SimRng expect = w.rng;
        (void)expect.range_float(90.0f, 90.0f);  // R is drawn on every machine before the dialog opens
        bool accepted = false;
        CHECK(!s.ground_activated_trigger(ty, rA, &accepted) && accepted && w.rng == expect && w.store.alive(ty));
        const GalacticEvent* d = w.last(GalacticEventKind::SpecialDialog);
        CHECK(d && d->other_player == static_cast<int32_t>(kSpecialBlackMarket) && d->planet == rA &&
              d->value == static_cast<int32_t>(commands::quantize(kQuantDuration, 90.0f)) && d->object == static_cast<int32_t>(w.oid(ty)));
        const float c0 = s.credits(pUnd);
        commands::CommandPipeline pipe;
        pipe.set_handler(s.command_handler());
        pipe.begin_tick(0);
        pipe.submit(static_cast<uint8_t>(pUnd), commands::CommandSource::Player,
                    black_market_purchase_event(w.pid(rA), w.oid(ty), special_name_crc("Generic_Tech"), 800.0f, 90.0f));
        for (uint64_t t = 1; t <= 4; ++t) pipe.begin_tick(t);  // drained + executed at its scheduled tick
        const float q = commands::requantize(kQuantAmount, 800.0f), rq = commands::requantize(kQuantDuration, 90.0f);
        CHECK(s.credits(pUnd) == c0 - q && s.bm_has_item(pUnd, 2) && !w.store.alive(ty));
        CHECK(s.respawns_pending(rA).size() == 1 && s.respawns_pending(rA)[0].due == due_after(s.frame(), rq));
        CHECK(w.last(GalacticEventKind::BlackMarketPurchase) && w.last(GalacticEventKind::BlackMarketPurchase)->other_player == 1 &&
              w.last(GalacticEventKind::BlackMarketPurchase)->value == 2);
        // Too expensive: nothing happens (CannotAfford); a dead buyer: InvalidUnit; unknown item CRC: Malformed.
        const ObjectHandle ty2 = s.spawn_unit(pUnd, rA, tUrai);
        CHECK(s.bm_execute_purchase(pUnd, rA, ty2, 0, 1.0e5f, 0.0f) == RejectReason::CannotAfford && w.store.alive(ty2));
        CHECK(s.bm_execute_purchase(pUnd, rA, ty, 2, 1.0f, 0.0f) == RejectReason::InvalidUnit);
        CHECK(s.execute(static_cast<uint8_t>(pUnd), black_market_purchase_event(w.pid(rA), w.oid(ty2), 0x1234u, 1.0f, 0.0f)) ==
              RejectReason::Malformed);
        // Already purchased items are charged again (the list add is idempotent).
        const float c1 = s.credits(pUnd);
        CHECK(s.bm_execute_purchase(pUnd, rA, ty2, 2, 100.0f, 0.0f) == RejectReason::None &&
              s.credits(pUnd) == c1 - commands::requantize(kQuantAmount, 100.0f) &&
              s.bm_purchased(pUnd).size() == 1);
    }
    {
        // Must_Be_Bought_On_Black_Market gate (P4 1.5).
        World w;
        GalacticSim& s = w.sim;
        const uint32_t buzz = special_name_crc("Buzz_Droids_Viper");
        CHECK(!s.bm_allows_unit_ability(pUnd, buzz, true) && s.bm_allows_unit_ability(pUnd, buzz, false));
        CHECK(s.bm_allows_unit_ability(pUnd, special_name_crc("Not_Listed"), true));  // the TR case
        CHECK(s.bm_give_item(pUnd, "Buzz_Droid_Tech") && !s.bm_give_item(pUnd, "buzz_droid_tech"));  // exact name
        CHECK(s.bm_allows_unit_ability(pUnd, buzz, true) && s.bm_allows_special_ability(pUnd, buzz, true) &&
              !s.bm_allows_unit_ability(pEmp, buzz, true));
    }
}

// ---- 2c. sabotage -----------------------------------------------------------------------------------------------------

ObjectHandle first_structure(const World& w, PlanetIndex p, uint32_t type) {
    for (ObjectHandle h : w.planet(p).structures)
        if (w.store.alive(h) && w.store.type(h) == type) return h;
    return {};
}

void test_sabotage() {
    {
        World w;  // AI saboteur
        GalacticSim& s = w.sim;
        s.set_corruption(rB, 3, pUnd);
        const ObjectHandle sab = s.spawn_unit(pUnd, rB, tSaboteur);
        const ObjectHandle st = first_structure(w, rB, tSpecial);
        CHECK(s.sabotage_options(rB, sab) == std::vector<uint8_t>{kSabotageSpecialStructure});
        CHECK(s.sabotage_cost(pUnd, rB, st, sab) == 1000);
        s.set_sabotage_tutorial(pUnd, true);
        CHECK(s.sabotage_cost(pUnd, rB, st, sab) == 50);  // tutorial: unscaled
        s.set_sabotage_tutorial(pUnd, false);
        // n = 1 option: no draw; one candidate: Get_Int(0, 0) draws nothing; then vf12 = Get_Float(5, 5): 1 draw.
        SimRng expect = w.rng;
        const float r = expect.range_float(5.0f, 5.0f);
        const float c0 = s.credits(pUnd);
        CHECK(s.ground_activated_trigger(sab, rB));
        CHECK(w.rng == expect && !w.store.alive(st) && s.sabotage_count(rB) == 1 && s.credits(pUnd) == c0);  // AI not charged [H]
        CHECK(!w.store.alive(sab) && s.respawns_pending(rB).size() == 1 && s.respawns_pending(rB)[0].due == due_after(s.frame(), r));
        CHECK(w.count(GalacticEventKind::GalacticSabotage) == 0);  // the story event is the command path's
        // Cost escalation: 1000 x 2^n.
        const ObjectHandle st2 = s.spawn_unit(pEmp, rB, tSpecial);
        const ObjectHandle st0 = s.spawn_unit(pEmp, rB, tSpecial0);
        CHECK(s.sabotage_cost(pUnd, rB, st2) == 2000 && s.sabotage_cost(pUnd, rB, st0) == -1);
        CHECK(s.sabotage_structure_reward(rB, tSpecial) && !w.store.alive(st2) && s.sabotage_count(rB) == 2 &&
              w.count(GalacticEventKind::GalacticSabotage) == 1);
        const ObjectHandle st3 = s.spawn_unit(pEmp, rB, tSpecial);
        CHECK(s.sabotage_cost(pUnd, rB, st3) == 4000);
        // Only structures with cost >= 1 are dialog rows.
        const std::vector<SabotageTarget> rows = s.sabotage_targets(pUnd, rB);
        CHECK(rows.size() == 1 && rows[0].structure == st3 && rows[0].cost == 4000);
    }
    {
        World w(true);  // human: the dialog, then command kind 5
        GalacticSim& s = w.sim;
        s.set_corruption(rB, 3, pUnd);
        const ObjectHandle sab = s.spawn_unit(pUnd, rB, tSaboteur);
        const ObjectHandle st = first_structure(w, rB, tSpecial);
        const SimRng before = w.rng;
        bool accepted = false;
        CHECK(!s.ground_activated_trigger(sab, rB, &accepted) && accepted && w.store.alive(sab) && w.store.alive(st) && w.rng == before);
        CHECK(w.last(GalacticEventKind::SpecialDialog) && w.last(GalacticEventKind::SpecialDialog)->other_player == static_cast<int32_t>(kSpecialSabotage));
        const float c0 = s.credits(pUnd);
        CHECK(s.execute(static_cast<uint8_t>(pUnd), sabotage_structure_event(w.pid(rB), w.oid(sab), w.oid(st), 1000.0f, 0.0f)) ==
              RejectReason::None);
        CHECK(!w.store.alive(st) && !w.store.alive(sab) && s.credits(pUnd) == c0 - commands::requantize(kQuantAmount, 1000.0f) &&
              s.sabotage_count(rB) == 1);
        CHECK(s.respawns_pending(rB).size() == 1 && s.respawns_pending(rB)[0].due == s.frame() + 10800);  // R = 0 -> default
        CHECK(w.last(GalacticEventKind::GalacticSabotage) && w.last(GalacticEventKind::GalacticSabotage)->type == tSpecial);
        // Wrong structure / not affordable.
        const ObjectHandle sab2 = s.spawn_unit(pUnd, rB, tSaboteur);
        const ObjectHandle st2 = s.spawn_unit(pEmp, rB, tSpecial);
        CHECK(s.sabotage_execute_structure(pUnd, rB, sab2, st2, 1.0e5f, 0.0f) == RejectReason::CannotAfford && w.store.alive(st2));
        CHECK(s.sabotage_execute_structure(pUnd, rA, sab2, st2, 1.0f, 0.0f) == RejectReason::InvalidTarget);
    }
    {
        World w;  // Planet_Vigilance (C) + a generic hero: fails, the saboteur dies, respawn after Get_Float(5, 5)
        GalacticSim& s = w.sim;
        s.set_corruption(rC, 3, pUnd);
        const ObjectHandle sab = s.spawn_unit(pUnd, rC, tSaboteur);
        SimRng expect = w.rng;
        (void)expect.range_float(5.0f, 5.0f);  // vigilance vf12
        (void)expect.range_float(5.0f, 5.0f);  // the trigger's Causes_Despawn draw (hero already dead: nothing scheduled)
        CHECK(s.ground_activated_trigger(sab, rC));
        CHECK(w.rng == expect && !w.store.alive(sab) && first_structure(w, rC, tSpecial) != ObjectHandle{} && s.sabotage_count(rC) == 0);
        CHECK(s.respawns_pending(rC).size() == 1 && s.respawns_pending(rC)[0].due == due_after(s.frame(), 5.0f));
        CHECK(w.count(GalacticEventKind::SabotageVigilance) == 1 && w.last(GalacticEventKind::SabotageVigilance)->other_player == pReb);
    }
    {
        World w;  // credit halt (option 4): faction pairing Underworld -> Empire; 10 s
        GalacticSim& s = w.sim;
        const ObjectHandle h = s.spawn_unit(pUnd, rA, tHalter);
        CHECK(s.sabotage_options(rA, h) == std::vector<uint8_t>{kSabotageHalt});
        CHECK(s.planet_income(rA, false) > 0.0f);
        CHECK(s.ground_activated_trigger(h, rA));
        CHECK(w.planet(rA).halt_until == s.frame() + 300);
        CHECK(s.planet_income(rA, false) == 0.0f && !w.store.alive(h));
        const ObjectHandle h2 = s.spawn_unit(pUnd, rC, tHalter);  // Underworld -> Rebel also pairs
        CHECK(s.sabotage_options(rC, h2) == std::vector<uint8_t>{kSabotageHalt});
        const ObjectHandle h3 = s.spawn_unit(pEmp, rD, tHalter);  // Empire -> Underworld pairs too
        CHECK(s.sabotage_options(rD, h3) == std::vector<uint8_t>{kSabotageHalt});
        const ObjectHandle h4 = s.spawn_unit(pEmp, rE, tHalter);  // the neutral planet: no pairing
        CHECK(s.sabotage_options(rE, h4).empty());
    }
}

// ---- 2d. hack ---------------------------------------------------------------------------------------------------------

void test_hack() {
    {
        World w;  // AI IG-88: free hack
        GalacticSim& s = w.sim;
        const ObjectHandle ds = s.spawn_unit(pEmp, rB, tDS2);
        const ObjectHandle hero = s.spawn_unit(pEmp, rB, tTyber);  // an Empire hero on B: removed with a default respawn
        const ObjectHandle ig = s.spawn_unit(pUnd, rB, tIG88);
        CHECK(s.hack_find_super_weapon(rB) == ds && s.hack_find_super_weapon(rA) == ObjectHandle{});
        const float c0 = s.credits(pUnd);
        const SimRng before = w.rng;
        CHECK(s.ground_activated_trigger(ig, rB));
        CHECK(w.rng == before && s.credits(pUnd) == c0);  // Causes_Despawn No: no draw; AI: free
        CHECK(!w.store.alive(ds) && !w.store.alive(ig) && !w.store.alive(hero) && s.type_locked(pEmp, tDS2) && !s.type_locked(pUnd, tDS2));
        const PlanetState& b = w.planet(rB);
        CHECK(b.destroyed && b.owner == pNeu && b.structures.empty() && b.surface.empty() && b.corruption == kCorruptionNone);
        CHECK(s.fleets_at(rB, pEmp).empty());
        // IG-88: no respawn; the Empire hero: a default-time respawn.
        CHECK(s.respawns_pending(rB).size() == 1 && s.respawns_pending(rB)[0].type == tTyber && s.respawns_pending(rB)[0].owner == pEmp);
        CHECK(w.count(GalacticEventKind::PlanetDestroyed) == 1 && w.last(GalacticEventKind::PlanetDestroyed)->other_player == pEmp &&
              w.count(GalacticEventKind::SuperWeaponHacked) == 1);
        CHECK(s.planet_income(rB, true) == 2.0f);  // Planet_Destroyed_Credit_Value
        CHECK(!s.planet_destroy(rB, pUnd));        // already destroyed
    }
    {
        World w(true);  // human: dialog, then command kind 7 (Hack_Super_Weapon_Cost)
        GalacticSim& s = w.sim;
        const ObjectHandle ds = s.spawn_unit(pEmp, rB, tDS2);
        const ObjectHandle ig = s.spawn_unit(pUnd, rB, tIG88);
        bool accepted = false;
        CHECK(!s.ground_activated_trigger(ig, rB, &accepted) && accepted && w.store.alive(ds) && w.store.alive(ig));
        CHECK(w.last(GalacticEventKind::SpecialDialog)->other_player == static_cast<int32_t>(kSpecialHack));
        CHECK(s.hack_execute(pUnd, rB, ig, 1.0e5f) == RejectReason::CannotAfford && w.count(GalacticEventKind::SpecialError) == 1);
        const float c0 = s.credits(pUnd);
        CHECK(s.execute(static_cast<uint8_t>(pUnd), hack_super_weapon_event(w.pid(rB), w.oid(ig), s.hack_cost())) == RejectReason::None);
        CHECK(s.credits(pUnd) == c0 - commands::requantize(kQuantAmount, 3000.0f) && !w.store.alive(ds) && w.planet(rB).destroyed && s.type_locked(pEmp, tDS2));
        const ObjectHandle ig2 = s.spawn_unit(pUnd, rA, tIG88);
        CHECK(s.hack_execute(pUnd, rA, ig2, 0.0f) == RejectReason::InvalidTarget);  // no super weapon: error SFX
        // Lock_Tech: a locked type is not buildable.
        s.lock_type(pEmp, tFrigate, true);
        CHECK(s.type_locked(pEmp, tFrigate));
        s.lock_type(pEmp, tFrigate, false);
        CHECK(!s.type_locked(pEmp, tFrigate));
    }
}

// ---- 2e0. tech level and unlock sources (docs/re/REMAINING_GAPS.md 1.4, 1.5, 2) ---------------------------------------

// Types reused as unlock subjects: Build_Initially_Locked + Build_Can_Be_Unlocked_By_Slicer at the given Tech_Level.
void make_sliceable(GalacticRules& r, uint32_t type, int32_t tech) {
    r.types[type].build_initially_locked = true;
    r.types[type].sliceable = true;
    r.types[type].tech_level = tech;
}

void test_tech_unlocks() {
    using V = std::vector<uint32_t>;
    {  // Set_Tech_Level sweep: strict <, sliceable + initially locked only, affiliation, type order, max clamp.
        World w;
        GalacticSim& s = w.sim;
        std::vector<TypeInfo>& T = w.rules.types;
        make_sliceable(w.rules, tFrigate, 2);
        make_sliceable(w.rules, tDS2, 1);
        make_sliceable(w.rules, tMerc, 1);
        T[tMerc].sliceable = false;  // initially locked, not sliceable: never swept
        T[tTyber].sliceable = true;  // sliceable, not initially locked: never swept
        T[tTyber].tech_level = 1;
        make_sliceable(w.rules, tUrai, 1);
        T[tUrai].affiliation = {static_cast<uint32_t>(pReb)};  // Rebel only
        CHECK(s.type_affiliated(tUrai, pReb) && !s.type_affiliated(tUrai, pEmp) && s.type_affiliated(tDS2, pEmp));
        CHECK(s.players()[pEmp].tech_level == 1 && s.players()[pEmp].unlocked_types.empty() && s.max_tech_level(pEmp) == 5);
        s.set_tech_level(pEmp, 1);  // tech-1 types are not < 1
        CHECK(s.players()[pEmp].unlocked_types.empty() && w.count(GalacticEventKind::TechLevelChanged) == 0);
        s.set_tech_level(pEmp, 2);
        CHECK((s.players()[pEmp].unlocked_types == V{tDS2}) && s.players()[pEmp].tech_level == 2);
        CHECK(w.last(GalacticEventKind::TechLevelChanged)->value == 2 && w.last(GalacticEventKind::TechLevelChanged)->other_player == 1);
        CHECK(w.last(GalacticEventKind::TypeUnlocked)->type == tDS2 && w.last(GalacticEventKind::TypeUnlocked)->value == 1 &&
              w.last(GalacticEventKind::TypeUnlocked)->player == pEmp);
        s.set_tech_level(pEmp, 9);  // clamped to the max tech (5); tDS2 is not unlocked twice
        CHECK(s.players()[pEmp].tech_level == 5 && (s.players()[pEmp].unlocked_types == V{tDS2, tFrigate}));
        CHECK(w.count(GalacticEventKind::TypeUnlocked) == 2);
        // Rebel, 1 -> 3 in one call: every match in type order (not Tech_Level order), the Rebel-only type included.
        s.set_tech_level(pReb, 3);
        CHECK((s.players()[pReb].unlocked_types == V{tFrigate, tDS2, tUrai}));
        // No sweep; level < 1 -> 0.
        s.set_tech_level(pUnd, 4, false);
        CHECK(s.players()[pUnd].tech_level == 4 && s.players()[pUnd].unlocked_types.empty());
        s.set_tech_level(pUnd, -3, false);
        CHECK(s.players()[pUnd].tech_level == 0);
        s.set_max_tech_level(pUnd, 2);
        s.set_tech_level(pUnd, 4);
        CHECK(s.players()[pUnd].tech_level == 2 && (s.players()[pUnd].unlocked_types == V{tDS2}));
    }
    {  // raise_tech_level: +1 with the sweep below the max, false at the max.
        World w;
        GalacticSim& s = w.sim;
        make_sliceable(w.rules, tFrigate, 2);
        s.set_max_tech_level(pEmp, 3);
        s.set_tech_level(pEmp, 2);
        CHECK(s.players()[pEmp].unlocked_types.empty());
        CHECK(s.raise_tech_level(pEmp) && s.players()[pEmp].tech_level == 3 && s.type_unlocked(pEmp, tFrigate));
        const size_t n = w.count(GalacticEventKind::TechLevelChanged);
        CHECK(!s.raise_tech_level(pEmp) && s.players()[pEmp].tech_level == 3 && w.count(GalacticEventKind::TechLevelChanged) == n);
        CHECK(!s.raise_tech_level(9));
    }
    {  // Campaign start sweep (GalacticSetup::tech_sweep_at_start) and the PlayerSetup::max_tech_level clamp.
        GalacticRules rules = make_rules();
        make_sliceable(rules, tFrigate, 1);
        make_sliceable(rules, tDS2, 2);
        objects::GameObjectTypes types;
        objects::IdCounter ids;
        objects::ObjectStore store{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids};
        SimRng rng{0x5EEDu};
        GalacticSim s{rules, store, rng};
        GalacticSetup setup = make_setup(false);
        setup.tech_sweep_at_start = true;
        setup.players[pReb].tech_level = 4;
        setup.players[pReb].max_tech_level = 2;
        CHECK(s.start(setup, nullptr));
        CHECK(s.players()[pEmp].tech_level == 1 && s.players()[pEmp].unlocked_types.empty());
        CHECK(s.players()[pReb].tech_level == 2 && s.max_tech_level(pReb) == 2 && (s.players()[pReb].unlocked_types == V{tFrigate}));
        objects::IdCounter ids2;
        objects::ObjectStore store2{objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids2};
        GalacticSim plain{rules, store2, rng};  // hand-built setups: no start sweep
        GalacticSetup no_sweep = make_setup(false);
        no_sweep.players[pReb].tech_level = 3;
        CHECK(plain.start(no_sweep, nullptr) && plain.players()[pReb].tech_level == 3 && plain.players()[pReb].unlocked_types.empty());
    }
    {  // Lock_Tech removal vs Unlock_Tech; unlock_type sources; remove_unlock; slice candidates.
        World w;
        GalacticSim& s = w.sim;
        make_sliceable(w.rules, tFrigate, 1);
        make_sliceable(w.rules, tDS2, 1);
        make_sliceable(w.rules, tMerc, 2);
        CHECK(s.initial_build_locks() && s.build_locked(pEmp, tFrigate));
        CHECK((s.slice_candidates(pEmp) == V{tFrigate, tDS2}));  // Tech_Level <= tech (tMerc is tech 2)
        s.lock_type(pEmp, tFrigate, true);
        CHECK(s.type_locked(pEmp, tFrigate) && s.build_locked(pEmp, tFrigate) && (s.slice_candidates(pEmp) == V{tDS2}));
        s.lock_type(pEmp, tFrigate, false);  // leaves the locked list only
        CHECK(!s.type_locked(pEmp, tFrigate) && !s.type_unlocked(pEmp, tFrigate) && s.build_locked(pEmp, tFrigate));
        CHECK(w.count(GalacticEventKind::TypeUnlocked) == 0);
        s.lock_type(pEmp, tFrigate, true);
        s.unlock_tech(pEmp, tFrigate);  // Lua Unlock_Tech: out of the locked list and into the unlocked list
        CHECK(!s.type_locked(pEmp, tFrigate) && s.type_unlocked(pEmp, tFrigate) && !s.build_locked(pEmp, tFrigate));
        CHECK(w.last(GalacticEventKind::TypeUnlocked)->type == tFrigate && w.last(GalacticEventKind::TypeUnlocked)->value == 3);
        CHECK((s.slice_candidates(pEmp) == V{tDS2}));
        s.unlock_tech(pEmp, tMerc);  // no tech check
        CHECK(s.type_unlocked(pEmp, tMerc) && s.players()[pEmp].tech_level == 1);
        s.unlock_type(pEmp, tDS2, 2);  // slice
        CHECK(w.last(GalacticEventKind::TypeUnlocked)->value == 2 && s.slice_candidates(pEmp).empty());
        CHECK((s.players()[pEmp].unlocked_types == V{tFrigate, tMerc, tDS2}));
        s.remove_unlock(pEmp, tMerc);
        CHECK((s.players()[pEmp].unlocked_types == V{tFrigate, tDS2}) && s.build_locked(pEmp, tMerc));
        s.unlock_tech(pEmp, static_cast<uint32_t>(tTypeCount));  // out of range: ignored
        CHECK(s.players()[pEmp].unlocked_types.size() == 2);
    }
    {  // Story BUILDABLE_UNIT: unlock P1, lock P2; Good_Side_Name (REBEL) raises the tech when no candidate remains.
        World w;
        GalacticSim& s = w.sim;
        make_sliceable(w.rules, tFrigate, 1);
        make_sliceable(w.rules, tDS2, 2);
        CHECK(w.rules.constants.good_side_name.empty());  // modc t03: no built-in faction name
        w.rules.constants.good_side_name = "REBEL";       // as FoC's GameConstants.xml
        s.buildable_unit_reward(pReb, tFrigate, tMerc);
        CHECK(s.type_unlocked(pReb, tFrigate) && s.type_locked(pReb, tMerc));
        CHECK(s.players()[pReb].tech_level == 2 && (s.players()[pReb].unlocked_types == V{tFrigate}));  // tDS2 is not < 2
        CHECK((s.slice_candidates(pReb) == V{tDS2}));
        s.buildable_unit_reward(pReb, kNoIndex, kNoIndex);  // a candidate remains: no raise
        CHECK(s.players()[pReb].tech_level == 2);
        s.unlock_type(pReb, tDS2, 2);
        s.set_max_tech_level(pReb, 2);
        s.buildable_unit_reward(pReb, kNoIndex, kNoIndex);  // none left, at the max: no raise
        CHECK(s.players()[pReb].tech_level == 2);
        // Not the good side: never raised.
        s.buildable_unit_reward(pEmp, tFrigate, kNoIndex);
        CHECK(s.type_unlocked(pEmp, tFrigate) && s.slice_candidates(pEmp).empty() && s.players()[pEmp].tech_level == 1);
        s.buildable_unit_reward(pUnd, kNoIndex, kNoIndex);
        CHECK(s.players()[pUnd].tech_level == 1);
    }
    {  // Production-disabled list: hashed, read by the build bar / AI only, not by build_check.
        World w;
        GalacticSim& s = w.sim;
        auto digest = [&s] {
            StateHasher h;
            s.hash_state(h);
            return h.digest();
        };
        const auto fresh = digest();
        CHECK(s.special_state_default());
        const BuildCheck before = s.build_check(pEmp, rA, tFrigate);
        s.set_production_disabled(pEmp, tFrigate, true);
        s.set_production_disabled(pEmp, tFrigate, true);  // no duplicates
        CHECK(s.production_disabled(pEmp, tFrigate) && !s.production_disabled(pReb, tFrigate) &&
              s.players()[pEmp].production_disabled.size() == 1);
        const BuildCheck after = s.build_check(pEmp, rA, tFrigate);
        CHECK(after.ok == before.ok && after.step == before.step && after.reason == before.reason && after.detail == before.detail);
        CHECK(!s.special_state_default() && digest() != fresh);
        s.set_production_disabled(pEmp, tFrigate, false);
        CHECK(!s.production_disabled(pEmp, tFrigate) && s.special_state_default() && digest() == fresh);
    }
    {  // Corruption Success_Unlock_Unit_Type: the new type's joins the corrupting player's list (value 4, planet build
       // cost > 0), the replaced type's leaves the old corrupting player's list.
        World w;
        GalacticSim& s = w.sim;
        std::vector<CorruptionChoiceDef>& ch = w.rules.special.types[tPB].corruption;
        ch[0].success_unlock = tUrai;  // Piracy (1)
        ch[2].success_unlock = tMerc;  // Black_Market (6)
        w.rules.types[tMerc].build_cost = 100;
        CHECK(w.planet(rB).corruption == kCorruptionNone);
        s.set_corruption(rB, 6, pUnd);
        CHECK(s.type_unlocked(pUnd, tMerc) && !s.type_unlocked(pEmp, tMerc));
        CHECK(w.last(GalacticEventKind::TypeUnlocked)->value == 4 && w.last(GalacticEventKind::TypeUnlocked)->player == pUnd);
        s.set_corruption(rB, 6, pUnd);  // same type: nothing changes
        CHECK(w.count(GalacticEventKind::TypeUnlocked) == 1);
        s.set_corruption(rB, 1, pReb);  // replaced: tMerc leaves Underworld; tUrai has build cost 0 -> not unlocked
        CHECK(!s.type_unlocked(pUnd, tMerc) && !s.type_unlocked(pReb, tUrai) && w.count(GalacticEventKind::TypeUnlocked) == 1);
        s.set_corruption(rB, kCorruptionNone, kNoPlayer);
        w.rules.types[tUrai].build_cost = 100;
        s.set_corruption(rB, 1, pReb);
        CHECK(s.type_unlocked(pReb, tUrai) && w.count(GalacticEventKind::TypeUnlocked) == 2);
        s.set_corruption(rB, kCorruptionNone, kNoPlayer);
        CHECK(!s.type_unlocked(pReb, tUrai));
        // A type without a Success_Unlock (Intimidation 0) unlocks nothing.
        s.set_corruption(rB, 0, pUnd);
        CHECK(s.players()[pUnd].unlocked_types.empty());
    }
    {  // Capture variant 0x3fb040 (REMAINING_GAPS 1.5 item 5): a Benefits_From_Corruption faction taking a clean planet
       // corrupts it with type 3 and gains that type's unlock; a non-benefiting taker clears it and the unlock leaves.
        World w;
        GalacticSim& s = w.sim;
        CorruptionChoiceDef c3;
        c3.slot = 4;
        c3.type = kCorruptionRacketeering;
        c3.success_unlock = tMerc;
        w.rules.special.types[tPB].corruption.push_back(c3);
        w.rules.types[tMerc].build_cost = 100;
        CHECK(w.planet(rB).owner == pEmp && w.planet(rB).corruption == kCorruptionNone);
        CHECK(s.change_owner(w.planet(rB).object, pUnd));
        // GRC 4.3: a 10 s gain transition; type, unlock and notification at completion.
        CHECK(w.planet(rB).corruption == kCorruptionNone && s.effective_corruption_type(rB) == kCorruptionRacketeering &&
              !s.type_unlocked(pUnd, tMerc));
        w.run(301);
        CHECK(w.planet(rB).owner == pUnd);
        CHECK(w.planet(rB).corruption == kCorruptionRacketeering && w.planet(rB).corrupting_player == pUnd);
        CHECK(s.type_unlocked(pUnd, tMerc) && w.last(GalacticEventKind::TypeUnlocked)->value == 4);
        CHECK(s.change_owner(w.planet(rB).object, pEmp));
        CHECK(w.planet(rB).corruption == kCorruptionRacketeering && w.planet(rB).corruption_removing);
        w.run(301);
        CHECK(w.planet(rB).corruption == kCorruptionNone && !s.type_unlocked(pUnd, tMerc));
    }
}

// ---- 2e1. Rebel tech theft: Slicer_Ability (docs/re/REMAINING_GAPS.md 1.1-1.3) --------------------------------------

// Gives tMerc (a named hero here) a Slicer ability: price = (cost curve 1 + affinity) x Slice_Cost, chance = 1 + the
// difficulty curve (+ affinity), respawn lists 20 s at tech 1. tFrigate (Slice_Cost 100) and tDS2 (200) are candidates.
uint32_t add_slicer(World& w, float difficulty_mod) {
    SpecialRules& sp = w.rules.special;
    SpecialAbilityDef a = ability(SpecialAbilityClass::Slicer, "R2D2_Slicer");
    a.slice_min_respawn = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
    a.slice_max_respawn = a.slice_min_respawn;
    a.cost_by_tech_availability = PiecewiseCurve{{0.0f}, {1.0f}};
    a.chance_by_difficulty = PiecewiseCurve{{0.0f}, {difficulty_mod}};
    a.cost_affinity = 1.0f;
    sp.abilities.push_back(a);
    const uint32_t id = static_cast<uint32_t>(sp.abilities.size() - 1);
    sp.types[tMerc].abilities = {id};
    sp.types[tMerc].named_hero = true;
    sp.types[tFrigate].slice_cost = 100.0f;
    sp.types[tDS2].slice_cost = 200.0f;
    sp.types[tPB].slice_affinity = {tDS2};
    make_sliceable(w.rules, tFrigate, 1);
    make_sliceable(w.rules, tDS2, 1);
    return id;
}

void test_slicer() {
    using V = std::vector<uint32_t>;
    {  // AI slicer (Rebel at the Empire planet B), two entries: one Get_Float(0, 2) pick, guaranteed success.
        World w;
        GalacticSim& s = w.sim;
        const uint32_t ab = add_slicer(w, 0.0f);
        const ObjectHandle sl = s.spawn_unit(pReb, rB, tMerc);
        CHECK(s.slice_candidates(pReb) == (V{tFrigate, tDS2}));
        const std::vector<SliceEntry> es = s.slice_entries(pReb, rB, ab);
        CHECK(es.size() == 2 && es[0].type == tFrigate && es[0].price == 100.0f && es[0].chance == 1.0f);
        CHECK(es[1].type == tDS2 && es[1].price == 400.0f && es[1].chance == 1.0f);  // affinity: cost (1 + 1) x 200
        CHECK(s.slice_entries(pReb, rC, ab).size() == 2 && s.slice_entries(pReb, rB, 99).empty());
        const float c0 = s.credits(pReb);
        SimRng expect = w.rng;
        const float r = expect.inline_range_float(20.0f, 20.0f);  // R at tech 1
        const float x = expect.inline_range_float(0.0f, 2.0f);
        const size_t pick = x <= 1.0f ? 0u : 1u;
        (void)expect.inline_range_float(0.0f, 0.999f);  // s < 1: success
        CHECK(s.ground_activated_trigger(sl, rB));
        CHECK(w.rng == expect && !w.store.alive(sl));
        CHECK((s.players()[pReb].unlocked_types == V{es[pick].type}) && s.credits(pReb) == c0 - es[pick].price);
        CHECK(s.players()[pReb].tech_level == 1);  // two entries: no only-one raise
        const GalacticEvent* ev = w.last(GalacticEventKind::Slice);
        CHECK(ev && ev->value == 0 && ev->type == es[pick].type && ev->player == pReb && ev->other_player == pEmp);
        CHECK(w.last(GalacticEventKind::TypeUnlocked)->value == 2);
        CHECK(s.respawns_pending(rB).size() == 1 && s.respawns_pending(rB)[0].due == due_after(s.frame(), r));
        // Not at an own / allied planet, nor with no candidates left after both are taken.
        const ObjectHandle sl2 = s.spawn_unit(pReb, rC, tMerc);
        bool accepted = true;
        CHECK(!s.ground_activated_trigger(sl2, rC, &accepted) && !accepted);
        s.unlock_type(pReb, tFrigate, 2);
        s.unlock_type(pReb, tDS2, 2);
        const ObjectHandle sl3 = s.spawn_unit(pReb, rA, tMerc);
        CHECK(!s.ground_activated_trigger(sl3, rA, &accepted) && !accepted && w.store.alive(sl3));
    }
    {  // One candidate: only-one success raises the tech (with the sweep).
        World w;
        GalacticSim& s = w.sim;
        add_slicer(w, 0.0f);
        w.rules.types[tDS2].tech_level = 2;
        const ObjectHandle sl = s.spawn_unit(pReb, rB, tMerc);
        CHECK(s.ground_activated_trigger(sl, rB));
        CHECK(s.type_unlocked(pReb, tFrigate) && s.players()[pReb].tech_level == 2 && !s.type_unlocked(pReb, tDS2));
        CHECK(w.last(GalacticEventKind::TechLevelChanged)->value == 2);
    }
    {  // Failure (chance 1 - 1 = 0): the k draw; 1 captured (R x 2), 2 lost effort, 3 cheated (charged). No unlock.
        World w;
        GalacticSim& s = w.sim;
        add_slicer(w, -1.0f);
        w.rules.types[tDS2].tech_level = 2;  // one entry
        const ObjectHandle sl = s.spawn_unit(pReb, rB, tMerc);
        const float c0 = s.credits(pReb);
        SimRng expect = w.rng;
        const float r = expect.inline_range_float(20.0f, 20.0f);
        (void)expect.inline_range_float(0.0f, 1.0f);  // the AI pick
        (void)expect.inline_range_float(0.0f, 0.999f);
        const int32_t k = expect.range_int(1, 3);
        CHECK(!s.ground_activated_trigger(sl, rB));
        CHECK(w.rng == expect && !w.store.alive(sl) && s.players()[pReb].unlocked_types.empty() && s.players()[pReb].tech_level == 1);
        CHECK(w.last(GalacticEventKind::Slice)->value == k && w.last(GalacticEventKind::Slice)->type == tFrigate);
        CHECK(s.credits(pReb) == (k == 3 ? c0 - 100.0f : c0));
        CHECK(s.respawns_pending(rB).size() == 1 && s.respawns_pending(rB)[0].due == due_after(s.frame(), k == 1 ? 2.0f * r : r));
    }
    {  // Human slicer (Underworld): the dialog (SpecialDialog, other_player = kind 2), then command kind 2.
        World w(true);
        GalacticSim& s = w.sim;
        const uint32_t ab = add_slicer(w, 0.0f);
        (void)ab;
        const ObjectHandle sl = s.spawn_unit(pUnd, rB, tMerc);
        bool accepted = false;
        SimRng expect = w.rng;
        const float r = expect.inline_range_float(20.0f, 20.0f);
        CHECK(!s.ground_activated_trigger(sl, rB, &accepted) && accepted && w.store.alive(sl) && w.rng == expect);
        const GalacticEvent* d = w.last(GalacticEventKind::SpecialDialog);
        CHECK(d && d->other_player == static_cast<int32_t>(kSpecialSlice) &&
              d->value == static_cast<int32_t>(commands::quantize(kQuantDuration, r)));
        // Payload round trip.
        const commands::EventData ev = slice_event(w.pid(rB), w.oid(sl), tDS2, 400.0f, 0.75f, r, true);
        SpecialPayload pl;
        CHECK(read_special_payload(ev, pl) && pl.kind == kSpecialSlice && pl.planet == w.pid(rB) && pl.unit == w.oid(sl) &&
              pl.type == tDS2 && pl.only_one);
        CHECK(pl.amount == commands::requantize(kQuantAmount, 400.0f) && std::fabs(pl.chance - 0.75f) < 1e-3f &&
              std::fabs(pl.seconds - r) < 0.1f);
        // Rejections: unaffordable, unknown type, another player's unit.
        CHECK(s.execute(static_cast<uint8_t>(pUnd), slice_event(w.pid(rB), w.oid(sl), tDS2, 1.0e6f, 1.0f, r, false)) ==
              RejectReason::CannotAfford);
        CHECK(s.execute(static_cast<uint8_t>(pUnd), slice_event(w.pid(rB), w.oid(sl), 999u, 1.0f, 1.0f, r, false)) ==
              RejectReason::Malformed);
        CHECK(s.execute(static_cast<uint8_t>(pEmp), slice_event(w.pid(rB), w.oid(sl), tDS2, 1.0f, 1.0f, r, false)) ==
              RejectReason::InvalidUnit);
        CHECK(s.players()[pUnd].unlocked_types.empty() && w.store.alive(sl));
        // The pick: chance 1 -> success, charged, unlocked, only-one raise, the slicer departs.
        const float c0 = s.credits(pUnd);
        CHECK(s.execute(static_cast<uint8_t>(pUnd), slice_event(w.pid(rB), w.oid(sl), tDS2, 400.0f, 1.0f, r, true)) ==
              RejectReason::None);
        CHECK(s.type_unlocked(pUnd, tDS2) && s.credits(pUnd) == c0 - commands::requantize(kQuantAmount, 400.0f));
        CHECK(s.players()[pUnd].tech_level == 2 && !w.store.alive(sl) && w.last(GalacticEventKind::Slice)->value == 0);
    }
}

// ---- 2e. income abilities ---------------------------------------------------------------------------------------------

const IncomeEntry* ability_entry(const World& w, PlanetIndex p, uint32_t crc) {
    for (const IncomeEntry& e : w.planet(p).income)
        if (e.ability == crc) return &e;
    return nullptr;
}

void test_income() {
    World w;
    GalacticSim& s = w.sim;
    // Frame 0 service: the gambling arena at D draws u, then m (2 draws); nothing else draws on frame 0.
    SimRng expect = w.rng;
    const float u = expect.inline_unit_float();
    const float m = u < 0.8f ? expect.range_float(0.0f, 0.6f) : expect.range_float(-0.2f, 0.0f);
    w.run(1);
    CHECK(w.rng == expect);
    // Mining (vanilla Empire_Mining_Income_Bonus at the mining colony A, base 100): (100 x 2.0 + 0) x 1.5 = 300.
    const IncomeEntry* mine = ability_entry(w, rA, special_name_crc("Empire_Mining_Income_Bonus"));
    CHECK(mine && mine->value == 300.0f && mine->tag == economy_mining_tag() && mine->kind == kIncomeAbility && mine->expiry == econ::kNoExpiry);
    CHECK(s.mining_income(pEmp) == 300.0f);
    const IncomeEntry* arena = ability_entry(w, rD, special_name_crc("Grand_Arena_Gambling_Income"));
    CHECK(arena && arena->value == m * 30.0f && arena->next_eval == 900 && arena->tag == special_name_crc("TEXT_ARENA"));
    // Thrawn's Revenge-style entries at B (base 20, not a mining colony): Mining_Income_Bonus 0 / +400 -> 400;
    // Maintenance_Cost 0 / -3000 -> -3000 under the owner's Text_ID tag.
    const ObjectHandle trm = s.spawn_unit(pEmp, rB, tTRMine);
    const ObjectHandle upk = s.spawn_unit(pEmp, rB, tUpkeep);
    w.run(1);
    CHECK(ability_entry(w, rB, special_name_crc("Mining_Income_Bonus")) &&
          ability_entry(w, rB, special_name_crc("Mining_Income_Bonus"))->value == 400.0f);
    CHECK(s.economy_income_for_tag(pEmp, special_name_crc("TEXT_TR_MINING")) == 400.0f &&
          s.economy_income_for_tag(pEmp, special_name_crc("TEXT_TR_UPKEEP")) == -3000.0f && s.mining_income(pEmp) == 300.0f);
    // planet_income sums the entries: B = base 20 + 400 - 3000 (no force / trade / political terms here) = -2580.
    CHECK(s.planet_income(rB, false) == 20.0f + 400.0f - 3000.0f);
    // The breakdown groups by tag in first-seen order (A's entries, then B's); standard entries keep tag 0. It shows the
    // entries as last refreshed, and no fiscal cycle has run yet (the first is on frame 1350, q2f t02): refresh A here.
    CHECK(s.planet_income(rA, false) > 0.0f);
    const std::vector<EconomyRow> rows = s.economy_breakdown_by_tag(pEmp);
    bool saw_mine = false, saw_base = false;
    for (const EconomyRow& r : rows) {
        saw_mine = saw_mine || (r.tag == economy_mining_tag() && r.value == 300.0f);
        saw_base = saw_base || (r.tag == 0 && r.kind == kIncomeBase && r.value == 120.0f);  // 100 + 20
    }
    CHECK(saw_mine && saw_base);
    // The source leaves: the entry is removed (vf31). The value was fixed at apply time.
    s.destroy_object(trm);
    w.run(1);
    CHECK(!ability_entry(w, rB, special_name_crc("Mining_Income_Bonus")) && ability_entry(w, rB, special_name_crc("Maintenance_Cost")));
    s.destroy_object(upk);
    w.run(1);
    CHECK(!ability_entry(w, rB, special_name_crc("Maintenance_Cost")));
    // Gambling re-evaluates every 30 s (900 frames): 2 more draws at frame 900.
    const uint64_t d0 = w.rng.draws();
    while (s.frame() < 900) w.run(1);
    CHECK(w.rng.draws() == d0);
    w.run(1);
    CHECK(w.rng.draws() == d0 + 2 && ability_entry(w, rD, special_name_crc("Grand_Arena_Gambling_Income"))->next_eval == 1800);
}

// ---- 2f. corruption ---------------------------------------------------------------------------------------------------

void test_corruption() {
    World w;
    GalacticSim& s = w.sim;
    const std::span<const CorruptionChoiceDef> ch = s.corruption_choices(rB);
    CHECK(ch.size() == 3);
    const ObjectHandle sab = s.spawn_unit(pUnd, rB, tSaboteur);
    CHECK(s.corruption_accepts(rB, sab) && !s.corruption_accepts(rE, sab) && s.sabotage_options(rB, sab).empty());
    CorruptionRequirements r0 = s.corruption_requirements(pUnd, rB, ch[0]);
    CHECK(r0.credits && !r0.units && r0.units_have == 0 && r0.units_need == 2 && r0.mission && r0.planets && !r0.all());
    s.spawn_unit(pUnd, rD, tMerc);
    CHECK(s.corruption_requirements(pUnd, rB, ch[0]).units_have == 1);
    const CorruptionRequirements r1 = s.corruption_requirements(pUnd, rB, ch[1]);
    CHECK(!r1.mission && r1.mission_have == 0 && r1.mission_need == 1);
    const CorruptionRequirements r2 = s.corruption_requirements(pUnd, rB, ch[2]);
    CHECK(r2.all() && r2.planets_have == 1 && !s.corruption_requirements(pEmp, rB, ch[2]).planets);
    // AI pick with only choice 2 passing: weight 2.5, one Get_Float(0, 2.5) draw.
    {
        SimRngRestore guard(w.rng);
        SimRng expect = w.rng;
        (void)expect.range_float(0.0f, 2.5f);
        CHECK(s.corruption_ai_pick(pUnd, rB) == 2 && w.rng == expect);
    }
    // The executor's order: credits, charge, then the units (no refund when they fail).
    const float c0 = s.credits(pUnd);
    CHECK(s.corruption_charge(pUnd, rB, 1, 0.0f) == CorruptionCharge::UnitsUnmet && s.credits(pUnd) == c0 - 500.0f);
    s.add_credits(pUnd, 500.0f);
    CHECK(s.corruption_charge(pUnd, rB, 1, 1.0e6f) == CorruptionCharge::UnitsUnmet);  // the planet type's cost, not the fallback
    s.add_credits(pUnd, 500.0f);
    // AI apply through the trigger: pick (1 draw), charge 200, corruption, counter, default-time departure; then the
    // Causes_Despawn draw (Get_Float(0, 0)).
    SimRng expect = w.rng;
    (void)expect.range_float(0.0f, 2.5f);
    (void)expect.range_float(0.0f, 0.0f);
    const float c1 = s.credits(pUnd);
    CHECK(s.ground_activated_trigger(sab, rB));
    // GRC 4.3: the direct path begins a 5 s gain (wrapper: the particle pairs are rebuilt at once).
    CHECK(w.rng == expect && w.planet(rB).corruption == kCorruptionNone && s.effective_corruption_type(rB) == 6 &&
          s.corruption_particle_pairs(rB).present && s.credits(pUnd) == c1 - 200.0f);
    CHECK(w.planet(rB).corruption_end - w.planet(rB).corruption_start == 150u);
    CHECK(s.corruptions_completed(pUnd, 6) == 1 && !w.store.alive(sab) && s.respawns_pending(rB).size() == 1 &&
          s.respawns_pending(rB)[0].due == s.frame() + 10800);
    w.run(151);
    CHECK(w.planet(rB).corruption == 6 && w.planet(rB).corrupting_player == pUnd && !s.corruption_transition_active(rB));
    // Weighted pick over all three (2 mercs, one Piracy done): Piracy 1 + 1/2 = 1.5, Intimidation 2/1 = 2, Black_Market
    // 2.5; cumulative 1.5, 3.5, 6.0 -> the first >= x.
    World v;
    GalacticSim& t = v.sim;
    t.spawn_unit(pUnd, rD, tMerc);
    t.spawn_unit(pUnd, rD, tMerc);
    t.count_corruption(pUnd, 1);
    SimRng e2 = v.rng;
    const float x = e2.range_float(0.0f, 6.0f);
    const int32_t want = x <= 1.5f ? 0 : (x <= 3.5f ? 1 : 2);
    CHECK(t.corruption_ai_pick(pUnd, rB) == want && v.rng == e2);
    // Consumed prerequisites (Destroy_When_Used_As_Corruption_Prereq): one Get_Float(0, 4) per candidate, the 2 heaviest
    // die.
    v.rules.special.types[tMerc].destroy_when_prereq = true;
    const ObjectHandle m3 = t.spawn_unit(pUnd, rD, tMerc);
    (void)m3;
    SimRng e3 = v.rng;
    for (int i = 0; i < 3; ++i) (void)e3.range_float(0.0f, 4.0f);
    CHECK(t.corruption_charge(pUnd, rB, 1, 0.0f) == CorruptionCharge::Charged && v.rng == e3);
    int32_t alive = 0;
    for (ObjectHandle h : v.planet(rD).surface) alive += (v.store.alive(h) && v.store.type(h) == tMerc) ? 1 : 0;
    CHECK(alive == 1);
}

// ---- 2g. command dispatch ---------------------------------------------------------------------------------------------

void test_commands() {
    World w;
    GalacticSim& s = w.sim;
    s.set_corruption(rA, 6, pUnd);
    const ObjectHandle ty = s.spawn_unit(pUnd, rA, tTyber);
    // Kind 1 (activate) for a unit not owned by the issuer: InvalidUnit; not at the planet: InvalidTarget.
    CHECK(s.execute(static_cast<uint8_t>(pEmp), special_activate_event(w.pid(rA), w.oid(ty))) == RejectReason::InvalidUnit);
    CHECK(s.execute(static_cast<uint8_t>(pUnd), special_activate_event(w.pid(rB), w.oid(ty))) == RejectReason::InvalidTarget);
    // Galactic pause: every kind is skipped (battle pending).
    s.push_pause();
    CHECK(s.execute(static_cast<uint8_t>(pUnd), special_activate_event(w.pid(rA), w.oid(ty))) == RejectReason::NotAllowed);
    s.pop_pause();
    CHECK(s.execute(static_cast<uint8_t>(pUnd), special_activate_event(w.pid(rA), w.oid(ty))) == RejectReason::None && !w.store.alive(ty) &&
          s.bm_has_item(pUnd, 2));
    // A fleet: its first member with an accepting Ground_Activated ability (the hero in orbit).
    const ObjectHandle fr = s.spawn_unit(pUnd, rA, tFrigate);
    (void)fr;
    const uint32_t f = s.fleets_at(rA, pUnd).front();
    CHECK(s.execute(static_cast<uint8_t>(pUnd), special_activate_event(w.pid(rA), static_cast<uint32_t>(w.store.id(s.fleets()[f].object)))) ==
          RejectReason::NotAllowed);  // no member has one
    // Kind 6 is left to the host (apps/sim CampaignBattles).
    commands::EventData k6(commands::EventType::SpecialAbility);
    k6.set("kind", kSpecialCorruption);
    CHECK(s.execute(static_cast<uint8_t>(pUnd), k6) == RejectReason::Unhandled);
    CHECK(s.execute(static_cast<uint8_t>(pUnd), special_activate_event(0x7FFFFFF0u, w.oid(ty))) == RejectReason::InvalidTarget);
}

// ---- 3. determinism ---------------------------------------------------------------------------------------------------

std::vector<std::pair<unsigned, ParallelFor>> worker_configs() {
    std::vector<std::pair<unsigned, ParallelFor>> v;
    v.push_back({0u, ParallelFor(serial_for)});
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned n : {1u, 4u, hw}) v.push_back({n, testing::make_thread_parallel_for(n)});
    return v;
}

CrcLog run_scripted(const ParallelFor& pf, int32_t frames) {
    World w(false, true);
    GalacticSim& s = w.sim;
    s.set_parallel_for(pf);
    CrcRegistry reg;
    s.register_crc(reg, "galactic");
    reg.add("rng", [&w](StateHasher& h) { w.rng.hash(h); });
    commands::CommandPipeline pipe;
    pipe.set_handler(s.command_handler());
    CrcLog log;
    log.systems = reg.names();
    ObjectHandle emp_hero, sab, halt, ig;
    for (int32_t t = 0; t < frames; ++t) {
        pipe.begin_tick(static_cast<uint64_t>(t));
        if (t == 2) s.set_corruption(rA, 6, pUnd);
        if (t == 3) s.ground_activated_trigger(s.spawn_unit(pUnd, rA, tTyber), rA);  // AI purchase
        if (t == 5) {
            s.set_corruption(rB, 3, pUnd);
            sab = s.spawn_unit(pUnd, rB, tSaboteur);
        }
        if (t == 6) s.ground_activated_trigger(sab, rB);  // AI sabotage
        if (t == 7) s.spawn_unit(pEmp, rA, tTRMine);      // TR income entry
        if (t == 9) {
            halt = s.spawn_unit(pUnd, rA, tHalter);
            s.ground_activated_trigger(halt, rA);
        }
        if (t == 12) {  // the human Empire's black market at Underworld's D (BM-corrupted by a story stand-in)
            s.set_corruption(rD, 6, pUnd);
            emp_hero = s.spawn_unit(pEmp, rD, tTyber);
            s.ground_activated_trigger(emp_hero, rD);  // dialog
        }
        if (t == 13)
            pipe.submit(static_cast<uint8_t>(pEmp), commands::CommandSource::Player,
                        black_market_purchase_event(w.pid(rD), w.oid(emp_hero), special_name_crc("Generic_Tech"), 500.0f, 90.0f));
        if (t == 20) {
            s.spawn_unit(pEmp, rC, tDS2);
            ig = s.spawn_unit(pUnd, rC, tIG88);
        }
        if (t == 21) s.ground_activated_trigger(ig, rC);  // AI hack: C destroyed
        if (t == 30) {
            const ObjectHandle d = s.spawn_unit(pUnd, rE, tSaboteur);
            s.ground_activated_trigger(d, rE);  // neutral planet: nothing
        }
        s.update();
        log.records.push_back(reg.capture(static_cast<uint64_t>(t), pf));
    }
    std::printf("  scripted: %d frames, credits E %.2f R %.2f U %.2f, U items %zu, C destroyed %d, respawns A %zu B %zu\n", frames,
                static_cast<double>(s.credits(pEmp)), static_cast<double>(s.credits(pReb)), static_cast<double>(s.credits(pUnd)),
                s.bm_purchased(pUnd).size(), w.planet(rC).destroyed ? 1 : 0, s.respawns_pending(rA).size(), s.respawns_pending(rB).size());
    return log;
}

void test_determinism() {
    const int32_t frames = 4 * 1350;
    CrcLog base;
    bool first = true;
    for (auto& [n, pf] : worker_configs()) {
        const CrcLog l = run_scripted(pf, frames);
        if (first) {
            base = l;
            first = false;
            continue;
        }
        const CrcDivergence d = first_divergence(base, l);
        if (d.diverged) std::printf("FAIL workers %u: %s\n", n, d.message.c_str());
        CHECK(!d.diverged);
    }
    // Sessions that never touch the special systems keep the S3 hash (the P4 tail is skipped).
    World a, b;
    StateHasher ha, hb;
    a.sim.hash_state(ha);
    b.sim.hash_state(hb);
    CHECK(ha.digest() == hb.digest() && a.sim.special_state_default());
    b.sim.set_bm_tutorial(pUnd, true);
    StateHasher hc;
    b.sim.hash_state(hc);
    CHECK(hc.digest() != ha.digest());
}

// ---- 4. FoC data ------------------------------------------------------------------------------------------------------

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

const SpecialAbilityDef* find_ability(const GalacticRules& r, uint32_t type, SpecialAbilityClass cls) {
    for (uint32_t a : r.special.type(type).abilities)
        if (r.special.abilities[a].cls == cls) return &r.special.abilities[a];
    return nullptr;
}

void test_foc() {
    const std::string root = env_or("ALAMO_FOC_DATA");
    std::string how;
    std::unique_ptr<FileProvider> provider = testing::open_foc_data(root, how);
    if (!provider) {
        std::printf("ALAMO_FOC_DATA unset or without Data\\XML (loose, or a FoC install through the VFS): FoC special-system data checks skipped\n");
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
    const SpecialRules& s = rules.special;
    // BlackMarketItems.xml: 10 items, 5 Empire + 5 Rebel, tech 1..5, cost 1000..4800, multiplier 5.5.
    uint32_t emp = 0, reb = 0, mn = 1u << 30, mx = 0;
    for (const BlackMarketItemDef& i : s.items) {
        emp += i.faction == rules.find_faction("Empire") ? 1u : 0u;
        reb += i.faction == rules.find_faction("Rebel") ? 1u : 0u;
        mn = std::min(mn, static_cast<uint32_t>(i.base_cost));
        mx = std::max(mx, static_cast<uint32_t>(i.base_cost));
        CHECK(i.uncommon_price_multiplier == 5.5f && i.tech_level >= 1 && i.tech_level <= 5 && !i.ability_crcs.empty());
    }
    CHECK(s.items.size() == 10 && emp == 5 && reb == 5 && mn == 1000 && mx == 4800);
    CHECK(s.items[0].name == "Buzz_Droid_Tech" && s.items[0].ability_crcs[0] == special_name_crc("Buzz_Droids_Viper") &&
          s.find_item(special_name_crc("Buzz_Droid_Tech")) == 0);
    // Hero abilities (P4 1.2 vanilla users).
    const SpecialAbilityDef* tz = find_ability(rules, rules.find_type("Tyber_Zann"), SpecialAbilityClass::BlackMarket);
    const SpecialAbilityDef* uf = find_ability(rules, rules.find_type("Urai_Fen"), SpecialAbilityClass::BlackMarket);
    const SpecialAbilityDef* si = find_ability(rules, rules.find_type("Silri"), SpecialAbilityClass::BlackMarket);
    CHECK(tz && tz->bm_min_respawn.size() == 5 && tz->bm_min_respawn[0] == 90.0f && tz->price_modifier == 1.0f && tz->causes_despawn &&
          tz->style == ActivationStyle::GroundActivated && tz->tech_level_adjustment == -1);
    CHECK(uf && uf->price_modifier == 1.5f && uf->bm_max_respawn[4] == 30.0f);
    CHECK(si && si->price_modifier == 0.75f && si->bm_min_respawn[2] == 60.0f);
    CHECK(find_ability(rules, rules.find_type("Tyber_Zann_Team"), SpecialAbilityClass::BlackMarket) != nullptr);  // team: first member
    const uint32_t st = rules.find_type("Underworld_Saboteur_Team");
    const SpecialAbilityDef* sab = find_ability(rules, st, SpecialAbilityClass::Sabotage);
    CHECK(sab && sab->sabotage_can[3] && !sab->sabotage_can[0] && !sab->sabotage_can[4] && sab->cost_increase_factor == 2.0f &&
          find_ability(rules, st, SpecialAbilityClass::Corruption) && s.type(st).generic_hero);
    CHECK(find_ability(rules, rules.find_type("IG88_Team"), SpecialAbilityClass::Hack) && s.type(rules.find_type("IG88_Team")).named_hero);
    // GameConstants.
    CHECK(s.hack_required_type == rules.find_type("Death_Star_II") && s.hack_required_type != kNoIndex && s.hack_cost == 50000.0f &&
          s.default_hero_respawn_time == 360.0f && s.sabotage_particle == "Planet_Explosion_Galactic" &&
          s.hack_particle == "IG88_Death_Star_Hack_Explosion");
    CHECK(s.activated_sabotage.size() == 3 && s.activated_sabotage[2] == "Saboteur_Galactic_Sabotage" && s.activated_black_market.size() == 3);
    // Income abilities (P4 4.1 / 4.2 vanilla values).
    const SpecialAbilityDef* palace = find_ability(rules, rules.find_type("U_Ground_Palace"), SpecialAbilityClass::IncomeBonus);
    CHECK(palace && palace->income_percentage == 7.0f && palace->mining_multiplier == 2.5f && palace->source_tag == economy_mining_tag() &&
          palace->style == ActivationStyle::GalacticAutomatic);
    uint32_t mining = 0, gambling = 0, colonies = 0, bm_planets = 0, vig = 0, prereq = 0, sab_cost = 0;
    for (const SpecialAbilityDef& a : s.abilities) {
        if (a.name == "Empire_Mining_Income_Bonus")
            mining += (a.income_percentage == 2.0f && a.mining_multiplier == 1.5f && a.source_tag == economy_mining_tag()) ? 1u : 0u;
        if (a.name == "Grand_Arena_Gambling_Income")
            gambling += (a.evaluation_interval == 30.0f && a.min_income_modifier == -0.2f && a.max_income_modifier == 0.6f &&
                         a.chance_to_win == 0.8f)
                            ? 1u
                            : 0u;
    }
    bool tyber_union = false;
    for (uint32_t t = 0; t < s.types.size(); ++t) {
        colonies += s.types[t].mining_colony ? 1u : 0u;
        bm_planets += s.types[t].black_market_modifier != 0.0f ? 1u : 0u;
        vig += s.types[t].planet_vigilance ? 1u : 0u;
        prereq += s.types[t].destroy_when_prereq ? 1u : 0u;
        sab_cost += s.types[t].sabotage_cost > 0 ? 1u : 0u;
        for (const CorruptionChoiceDef& c : s.types[t].corruption)
            if (c.required_unit_types.size() == 3 && c.required_unit_types[0] == rules.find_type("Tyber_Zann")) tyber_union = true;
    }
    CHECK(mining >= 1 && gambling >= 1 && colonies == 2 && tyber_union);
    std::printf("foc data: %zu items, %zu galactic special abilities, %u mining colonies, %u planets with a BM modifier, %u vigilant, %u "
                "prereq-consumed types, %u sabotageable types\n",
                s.items.size(), s.abilities.size(), colonies, bm_planets, vig, prereq, sab_cost);
    // Heart of the Maw: the Underworld palaces' income entries (Underworld_Palace_Income_Bonus, mining tag) after the
    // first frame = sum over the palaces of (base x 7.0) x (2.5 at a mining colony).
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    SimRng rng(0xC0FFEEu);
    GalacticSim sim(rules, store, rng);
    const GalacticSetup setup = build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    CHECK(sim.start(setup, &diag));
    sim.update();
    const int32_t U = sim.player_of_faction("Underworld");
    float expect = 0.0f;
    uint32_t sources = 0;
    for (const PlanetState& p : sim.planets()) {
        if (!p.is_planet || p.owner != U) continue;
        for (ObjectHandle h : p.structures) {
            if (!store.alive(h)) continue;
            for (uint32_t ai : s.type(static_cast<uint32_t>(store.type(h))).abilities) {
                const SpecialAbilityDef& a = s.abilities[ai];
                if (a.cls != SpecialAbilityClass::IncomeBonus || a.source_tag != economy_mining_tag()) continue;
                float v = static_cast<float>(p.base_value) * a.income_percentage + a.income_absolute;
                if (s.type(p.type).mining_colony) v *= a.mining_multiplier;
                expect += v;
                ++sources;
            }
        }
    }
    std::printf("foc Heart of the Maw: %u Underworld mining-tag sources, mining income %.1f (expected %.1f)\n", sources,
                static_cast<double>(sim.mining_income(U)), static_cast<double>(expect));
    CHECK(sim.mining_income(U) == expect);
}

}  // namespace

// ---- REMAINING_GAPS 6.1 launch reveal, 6.4 linked-battle flag -------------------------------------------------------

void test_launch_and_linked() {
    {  // range 0: the launch planet only; a huge range: every planet; no duplicates.
        World w;
        GalacticSim& s = w.sim;
        const ObjectHandle g = s.spawn_unit(pEmp, rA, tMerc);
        const int32_t id = static_cast<int32_t>(w.oid(g));
        const ObjectHandle one[] = {g};
        CHECK(s.launch_units(pEmp, rA, one) == RejectReason::None);
        CHECK(w.planet(rA).influence == std::vector<int32_t>{id} && w.planet(rB).influence.empty());
        w.rules.types[tMerc].influence_range = 1.0e9f;
        s.launch_reveal(rA, g);
        s.launch_reveal(rA, g);
        for (PlanetIndex p = 0; p < s.planets().size(); ++p)
            if (w.planet(p).is_planet) CHECK(w.planet(p).influence == std::vector<int32_t>{id});
    }
    {  // linked battle: a second queue refused, the hash moves. With no conflict queues the local player is in no queued
       // conflict, so the flag alone does not pause (0x2b0350; blocked case: alamo_galactic_conflict_tests).
        World w;
        GalacticSim& s = w.sim;
        auto digest = [&s] {
            StateHasher h;
            s.hash_state(h);
            return h.digest();
        };
        const auto before = digest();
        CHECK(!s.paused() && s.queue_linked_battle() && !s.queue_linked_battle() && s.linked_battle_pending());
        CHECK(!s.paused() && !s.story_block());
        CHECK(digest() != before);
        s.clear_linked_battle();
        CHECK(!s.paused() && !s.linked_battle_pending());
    }
}

int main() {
    test_payloads();
    test_respawn();
    test_black_market();
    test_sabotage();
    test_hack();
    test_tech_unlocks();
    test_slicer();
    test_launch_and_linked();
    test_income();
    test_corruption();
    test_commands();
    test_determinism();
    test_foc();
    if (g_fail) {
        std::printf("%d of %d check(s) failed\n", g_fail, g_checks);
        return 1;
    }
    std::printf("alamo_galactic_special_tests: all %d checks passed\n", g_checks);
    return 0;
}
