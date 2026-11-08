// alamo_galactic_save_tests: N1 save / load of the galactic simulation (GalacticSim::write / read, "galactic.state";
// ConflictManager::write / read, "galactic.conflicts"; layout in src/galactic_save.cpp).
//   1. synthetic galaxy with a conflict manager: a scripted run (production, multi-hop fleet moves, a space conflict
//      waiting for the human's choice, a tactical battle in progress, a land invasion auto-resolved, corruption
//      transition, visibility entries, hero respawn, stealth, claims, victory hold, relations, slots, linked battle, income
//      entries);
//      saved at fixed ticks and at every conflict state change. Each save is loaded into a FRESH GalacticSim + manager
//      built from the same setup (start(), the store restored, then read) and continued to the end of the run: every
//      tick's galactic hash, store hash + parity, RNG, conflict hash, save bytes and drained S4 events must equal the
//      original's. Save -> load -> save is byte-identical.
//   2. bad data: every truncation (sampled for big saves), a trailing byte, a wrong version, a non-started sim, objects
//      the store does not have, random bit flips: read() false leaves the sim / manager unchanged.
//   3. the same on FoC data (ALAMO_FOC_DATA; skipped when unset): Sandbox_Heart_Of_The_Maw_Underworld with the FoC
//      conflict rules, AI production and moves, a human attack whose conflict waits for the choice.
// The ObjectStore's own save is n1-objects' (a stub until it lands): the restore copies the store as it was at the save
// tick (plus the shared IdCounter value), which is what ObjectStore::read must reproduce.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "foc_data.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"

using namespace alamo;
using namespace alamo::galactic;
using commands::RejectReason;

using ::alamo::test::g_fail;

namespace {

// ---- harness ------------------------------------------------------------------------------------------------------------

struct TickDigest {
    uint64_t galactic = 0, objects = 0, rng = 0, conflicts = 0, save = 0, events = 0;
    uint32_t parity = 0;
    bool operator==(const TickDigest&) const = default;
};

std::vector<uint8_t> sim_bytes(const GalacticSim& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}

std::vector<uint8_t> cm_bytes(const ConflictManager& cm) {
    ByteWriter w;
    cm.write(w);
    return w.bytes();
}

uint64_t sim_hash(const GalacticSim& s) {
    StateHasher h;
    s.hash_state(h);
    return h.digest();
}

uint64_t cm_hash(const ConflictManager& cm) {
    StateHasher h;
    cm.hash(h);
    return h.digest();
}

TickDigest digest_of(GalacticSim& s, const ConflictManager& cm, const objects::ObjectStore& store, const SimRng& rng) {
    TickDigest d;
    d.galactic = sim_hash(s);
    {
        StateHasher h;
        store.hash_state(h);
        d.objects = h.digest();
    }
    d.parity = store.parity_crc();
    {
        StateHasher h;
        rng.hash(h);
        d.rng = h.digest();
    }
    d.conflicts = cm_hash(cm);
    {
        ByteWriter w;
        s.write(w);  // before the drain: pending events are part of the save
        cm.write(w);
        StateHasher h;
        h.bytes(w.bytes().data(), w.size());
        d.save = h.digest();
    }
    std::vector<GalacticEvent> ev;
    s.drain_events(ev);
    StateHasher h;
    h.u32(static_cast<uint32_t>(ev.size()));
    for (const GalacticEvent& e : ev) {
        h.i32(e.frame);
        h.u8(static_cast<uint8_t>(e.kind));
        h.i32(e.player);
        h.i32(e.other_player);
        h.u32(e.planet);
        h.u32(e.other_planet);
        h.u32(e.fleet);
        h.u32(e.type);
        h.i32(e.object);
        h.i32(e.value);
    }
    d.events = h.digest();
    return d;
}

std::string diff_text(const TickDigest& a, const TickDigest& b) {
    std::string s;
    if (a.galactic != b.galactic) s += " galactic.state";
    if (a.objects != b.objects) s += " objects";
    if (a.parity != b.parity) s += " parity";
    if (a.rng != b.rng) s += " rng";
    if (a.conflicts != b.conflicts) s += " conflicts";
    if (a.save != b.save) s += " save-bytes";
    if (a.events != b.events) s += " events";
    return s;
}

struct SavePoint {
    int32_t t = 0;
    std::vector<uint8_t> sim, cm, rng;
    std::unique_ptr<objects::ObjectStore> store;  // the store as ObjectStore::read must restore it
    objects::ObjectId next_id = 1;
    ConflictState state = ConflictState::Idle;
    bool moving = false, queued = false, respawn = false, transition = false, linked = false, events = false;
};

using Driver = std::function<void(GalacticSim&, ConflictManager&, const objects::ObjectStore&, int32_t)>;

struct Scenario {
    std::string name;
    const GalacticRules* rules = nullptr;
    const ConflictRules* crules = nullptr;
    const objects::GameObjectTypes* types = nullptr;
    GalacticSetup setup;
    ConflictConfig cfg;
    uint32_t seed = 1;
    int32_t ticks = 0;
    std::vector<int32_t> save_ticks;
    std::function<void(GalacticSim&)> prepare;  // original run only, after start() (restored from the save otherwise)
    Driver drive;
    size_t full_prefix_limit = 0;  // truncation test: every prefix up to this save size, else ~600 samples
    bool expect_conflicts = true;  // synthetic: the scripted Vote / InBattle save points must be hit
};

// Host side of the conflict manager (re-attached after a load like any host callback): tactical battles are accepted
// and finished later by the driver.
void attach_host(ConflictManager& cm) {
    cm.start_battle = [](const BattleRequest&) { return true; };
}

// Conflict answers shared by the scenarios (stateless: a function of the state and the tick): space conflicts are fought
// (a tactical battle the driver finishes later), land conflicts auto-resolved (synced RNG draws).
void drive_conflicts(GalacticSim& s, ConflictManager& cm, const objects::ObjectStore& st, int32_t t) {
    if (cm.state() == ConflictState::Vote && t % 61 == 0)
        cm.choose(!cm.space_queue().empty() ? BattleChoice::Fight : BattleChoice::AutoResolve);
    if (cm.battle_active() && t % 97 == 0) {
        const ConflictInfo& h = cm.active_request().info;
        const bool attacker_human = h.attacker >= 0 && s.players()[static_cast<size_t>(h.attacker)].human;
        BattleResult r;
        r.winner = attacker_human ? h.attacker : h.defender;  // the human side wins
        r.victory = kVictoryAllDestroyed;
        const int32_t loser = attacker_human ? h.defender : h.attacker;
        for (uint32_t f : s.fleets_at(h.planet, loser))  // the loser loses its first ship, the rest retreats
            for (ObjectHandle m : s.fleets()[f].contents)
                if (st.alive(m) && r.destroyed.empty()) r.destroyed.push_back(m);
        cm.finish_battle(r);
    }
}

void check_unchanged_after_bad_reads(GalacticSim& c, ConflictManager& cm, const SavePoint& sp, size_t full_limit) {
    const std::vector<uint8_t> sim0 = sim_bytes(c), cm0 = cm_bytes(cm);
    const uint64_t sh0 = sim_hash(c), ch0 = cm_hash(cm);
    CHECK(sim0 == sp.sim && cm0 == sp.cm);
    const auto sim_same = [&] { return sim_bytes(c) == sim0 && sim_hash(c) == sh0; };
    const auto cm_same = [&] { return cm_bytes(cm) == cm0 && cm_hash(cm) == ch0; };
    int32_t bad = 0;
    // Truncations.
    for (const std::vector<uint8_t>* data : {&sp.sim, &sp.cm}) {
        const bool is_sim = data == &sp.sim;
        const size_t n = data->size();
        const size_t step = n <= full_limit ? 1 : std::max<size_t>(1, n / 600);
        std::vector<size_t> lens;
        for (size_t len = 0; len < n; len += step) lens.push_back(len);
        lens.push_back(n - 1);
        for (size_t len : lens) {
            ByteReader r(std::span<const uint8_t>(data->data(), len));
            const bool ok = is_sim ? c.read(r) : cm.read(r);
            if (ok) ++bad;
        }
        // A trailing byte and a wrong version.
        std::vector<uint8_t> extra = *data;
        extra.push_back(0);
        ByteReader re(extra);
        if (is_sim ? c.read(re) : cm.read(re)) ++bad;
        std::vector<uint8_t> ver = *data;
        ver[0] = static_cast<uint8_t>(ver[0] + 1);
        ByteReader rv(ver);
        if (is_sim ? c.read(rv) : cm.read(rv)) ++bad;
    }
    CHECK(bad == 0);
    CHECK(sim_same() && cm_same());
    // Random bit flips: a read that fails leaves everything as it was; one that succeeds (a flipped float...) is undone by
    // reading the original bytes again.
    uint32_t lcg = 0x9E3779B9u;
    auto next = [&lcg] {
        lcg = lcg * 1664525u + 1013904223u;
        return lcg >> 8;
    };
    int32_t rejected = 0, accepted = 0, broken = 0;
    for (int32_t trial = 0; trial < 1200; ++trial) {
        const bool is_sim = (trial & 1) == 0;
        std::vector<uint8_t> bytes = is_sim ? sp.sim : sp.cm;
        const size_t at = next() % bytes.size();
        bytes[at] = static_cast<uint8_t>(bytes[at] ^ (1u << (next() % 8)));
        ByteReader r(bytes);
        const bool ok = is_sim ? c.read(r) : cm.read(r);
        if (!ok) {
            ++rejected;
            if (!(is_sim ? sim_same() : cm_same())) ++broken;
            continue;
        }
        ++accepted;
        ByteReader back(is_sim ? sp.sim : sp.cm);
        const bool restored = is_sim ? c.read(back) : cm.read(back);
        if (!restored || !(is_sim ? sim_same() : cm_same())) ++broken;
    }
    CHECK(broken == 0 && rejected > 0);
    std::printf("    bad data at t=%d: %zu + %zu bytes, truncations / trailing / version rejected, %d flips rejected, %d accepted "
                "(re-read), %d broken\n",
                sp.t, sp.sim.size(), sp.cm.size(), rejected, accepted, broken);
}

void run_scenario(const Scenario& sc) {
    std::printf("  %s: %d ticks\n", sc.name.c_str(), sc.ticks);
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, *sc.types, &ids);
    SimRng rng(sc.seed);
    std::vector<TickDigest> rec(static_cast<size_t>(sc.ticks));
    std::vector<SavePoint> saves;
    size_t log_entries = 0;
    {
        GalacticSim s(*sc.rules, store, rng);
        std::vector<std::string> diag;
        CHECK(s.start(sc.setup, &diag));
        for (const std::string& d : diag) std::printf("    setup: %s\n", d.c_str());
        if (sc.prepare) sc.prepare(s);
        ConflictManager cm(s, *sc.crules, rng, sc.cfg);
        attach_host(cm);
        cm.attach();
        ConflictState prev = cm.state();
        for (int32_t t = 0; t < sc.ticks; ++t) {
            sc.drive(s, cm, store, t);
            s.update();
            const bool state_change = cm.state() != prev && (cm.state() == ConflictState::Vote || cm.state() == ConflictState::InBattle);
            prev = cm.state();
            if (state_change || std::find(sc.save_ticks.begin(), sc.save_ticks.end(), t) != sc.save_ticks.end()) {
                SavePoint sp;
                sp.t = t;
                sp.sim = sim_bytes(s);
                sp.cm = cm_bytes(cm);
                ByteWriter rw;
                rng.write(rw);
                sp.rng = rw.bytes();
                sp.store = std::make_unique<objects::ObjectStore>(store);
                sp.next_id = ids.next();
                sp.state = cm.state();
                for (const FleetState& f : s.fleets()) sp.moving = sp.moving || (f.alive && f.moving);
                for (const PlanetState& p : s.planets()) {
                    sp.queued = sp.queued || !p.queue[0].empty() || !p.queue[1].empty();
                    sp.respawn = sp.respawn || !p.respawns.empty();
                    sp.transition = sp.transition || p.corruption_protected || p.corruption_removing;
                }
                sp.linked = s.linked_battle_pending();
                sp.events = !s.events().empty();
                saves.push_back(std::move(sp));
            }
            rec[static_cast<size_t>(t)] = digest_of(s, cm, store, rng);
        }
        log_entries = cm.log().size();
        size_t how[3] = {0, 0, 0};
        for (const ConflictLogEntry& e : cm.log()) ++how[std::min<size_t>(e.how, 2)];
        std::printf("    original: frame %d, day %d, fleets %zu, objects %zu, conflicts logged %zu (dropped %zu, auto %zu, battle %zu)\n",
                    s.frame(), s.galactic_day(), s.fleets().size(), store.live_count(), log_entries, how[0], how[1], how[2]);
        if (sc.expect_conflicts) CHECK(how[1] > 0 && how[2] > 0);
    }
    // Coverage of the save points.
    bool vote = false, battle = false, moving = false, queued = false, respawn = false, transition = false, linked = false,
         events = false;
    for (const SavePoint& sp : saves) {
        vote = vote || sp.state == ConflictState::Vote;
        battle = battle || sp.state == ConflictState::InBattle;
        moving = moving || sp.moving;
        queued = queued || sp.queued;
        respawn = respawn || sp.respawn;
        transition = transition || sp.transition;
        linked = linked || sp.linked;
        events = events || sp.events;
    }
    std::printf("    %zu save points; covered: conflict vote %d, battle %d, fleets moving %d, queues %d, respawns %d, corruption "
                "transition %d, linked battle %d, pending events %d\n",
                saves.size(), vote, battle, moving, queued, respawn, transition, linked, events);
    CHECK(moving && queued && events);
    if (sc.expect_conflicts) CHECK(vote && battle && respawn && transition && linked);

    // Load every save into a fresh sim and continue to the end; alternate serial and threaded owner derivation.
    size_t bad_data_at = saves.size();
    for (size_t i = 0; i < saves.size(); ++i)
        if (saves[i].state == ConflictState::Vote) {
            bad_data_at = i;
            break;
        }
    if (bad_data_at == saves.size() && !saves.empty()) bad_data_at = saves.size() / 2;
    int32_t loaded = 0, compared = 0;
    for (size_t i = 0; i < saves.size(); ++i) {
        const SavePoint& sp = saves[i];
        const ParallelFor pf = (i & 1) ? ::alamo::test::make_thread_parallel_for(4) : ParallelFor(serial_for);
        GalacticSim c(*sc.rules, store, rng);
        c.set_parallel_for(pf);
        CHECK(c.start(sc.setup));  // the loader builds the session from the same data first
        if (i == 0) {
            GalacticSim unstarted(*sc.rules, store, rng);
            ByteReader r(sp.sim);
            CHECK(!unstarted.read(r));  // no galaxy: fingerprint mismatch
        }
        if (i + 1 == saves.size()) {
            // A session started on its own store, never restored: it does not hold the objects made later in the run, so
            // the late save names unknown ids -> false, nothing changed.
            objects::IdCounter ids2;
            objects::ObjectStore store2(objects::ModeKind::Galactic, objects::StoreKind::Primary, *sc.types, &ids2);
            SimRng rng2(sc.seed);
            GalacticSim fresh(*sc.rules, store2, rng2);
            CHECK(fresh.start(sc.setup));
            const std::vector<uint8_t> before = sim_bytes(fresh);
            const uint64_t hash_before = sim_hash(fresh);
            ByteReader r(sp.sim);
            CHECK(!fresh.read(r));
            CHECK(sim_bytes(fresh) == before && sim_hash(fresh) == hash_before);
        }
        store = *sp.store;  // ObjectStore::read stand-in
        ids.set_next(sp.next_id);
        ByteReader rr(sp.rng);
        CHECK(rng.read(rr) && rr.done());
        ByteReader sr(sp.sim);
        const bool ok_sim = c.read(sr);
        CHECK(ok_sim);
        ConflictManager cm(c, *sc.crules, rng, sc.cfg);
        attach_host(cm);
        cm.attach();
        ByteReader cr(sp.cm);
        const bool ok_cm = cm.read(cr);
        CHECK(ok_cm);
        if (!ok_sim || !ok_cm) continue;
        ++loaded;
        CHECK(sim_bytes(c) == sp.sim);  // save -> load -> save
        CHECK(cm_bytes(cm) == sp.cm);
        if (i == bad_data_at) check_unchanged_after_bad_reads(c, cm, sp, sc.full_prefix_limit);
        const TickDigest d0 = digest_of(c, cm, store, rng);
        if (!(d0 == rec[static_cast<size_t>(sp.t)]))
            std::printf("FAIL %s load at t=%d: hashes differ after read:%s\n", sc.name.c_str(), sp.t,
                        diff_text(d0, rec[static_cast<size_t>(sp.t)]).c_str());
        CHECK(d0 == rec[static_cast<size_t>(sp.t)]);
        for (int32_t t = sp.t + 1; t < sc.ticks; ++t) {
            sc.drive(c, cm, store, t);
            c.update();
            const TickDigest d = digest_of(c, cm, store, rng);
            ++compared;
            if (!(d == rec[static_cast<size_t>(t)])) {
                std::printf("FAIL %s loaded at t=%d diverged at t=%d:%s\n", sc.name.c_str(), sp.t, t,
                            diff_text(d, rec[static_cast<size_t>(t)]).c_str());
                ++g_fail;
                break;
            }
        }
        CHECK(cm.log().size() == log_entries);
    }
    CHECK(loaded == static_cast<int32_t>(saves.size()) && !saves.empty());
    std::printf("    %d loads continued in lockstep with the original: %d ticks compared\n", loaded, compared);
}

// ---- 1. synthetic galaxy ------------------------------------------------------------------------------------------------

enum TypeId : uint32_t {
    tPlanetA, tPlanetB, tPlanetC, tPlanetD, tPlanetE,
    tSB1, tSB2, tFrigate, tFighters, tInfantry, tBarracks, tTechUp, tCorvette, tTypeCount
};
enum Row : PlanetIndex { rCore = 0, rA = 1, rB = 2, rC = 3, rD = 4, rE = 5 };
enum PlayerId : int32_t { pEmpire = 0, pUnder = 1, pNeutral = 2 };

constexpr uint64_t kCatFighter = 0x1, kCatCapital = 0x2, kCatInfantry = 0x4, kCatStructure = 0x8, kCatSuper = 0x10;

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
    inf.hyperspace = true;
    inf.hyperspace_speed = 1.5f;
    unit(tBarracks, "Barracks", UnitKind::GroundStructure, 600, 20).ai_combat_power = 200.0f;
    TypeInfo& tu = unit(tTechUp, "TechUp", UnitKind::Other, 1000, 30);
    tu.required_star_base = 1;
    tu.advances_tech = 2;
    tu.tech_level = 1;
    TypeInfo& co = unit(tCorvette, "Corvette", UnitKind::SpaceUnit, 800, 10);
    co.required_star_base = 1;
    co.hyperspace = true;
    co.hyperspace_speed = 2.0f;
    co.ai_combat_power = 600.0f;
    co.piracy_value = 30.0f;
    return r;
}

ConflictRules make_conflict_rules() {
    ConflictRules c;
    ContrastRow f;
    f.enemy = "Fighter";
    f.enemy_mask = kCatFighter;
    f.friendly = {{"Fighter", kCatFighter, 1.0f}, {"Capital", kCatCapital, 0.5f}};
    ContrastRow cap;
    cap.enemy = "Capital";
    cap.enemy_mask = kCatCapital;
    cap.friendly = {{"Fighter", kCatFighter, 1.0f}, {"Capital", kCatCapital, 1.0f}};
    ContrastRow i;
    i.enemy = "Infantry";
    i.enemy_mask = kCatInfantry;
    i.friendly = {{"Infantry", kCatInfantry, 1.0f}, {"Structure", kCatStructure, 1.0f}};
    ContrastRow s;
    s.enemy = "Structure";
    s.enemy_mask = kCatStructure;
    s.friendly = {{"Infantry", kCatInfantry, 1.0f}};
    c.contrast.rows = {f, cap, i, s};
    c.super_category = kCatSuper;
    c.types.resize(tTypeCount);
    c.types[tSB1].categories = c.types[tSB2].categories = kCatStructure;
    c.types[tSB2].lower_starbase = tSB1;
    c.types[tFrigate].categories = kCatCapital;
    c.types[tFrigate].population = 2;
    c.types[tCorvette].categories = kCatCapital;
    c.types[tCorvette].population = 1;
    c.types[tFighters].categories = kCatFighter;
    c.types[tInfantry].categories = kCatInfantry;
    c.types[tBarracks].categories = kCatStructure;
    for (uint32_t p : {tPlanetA, tPlanetB, tPlanetC, tPlanetD, tPlanetE}) c.types[p].terrain = 0;
    ConflictFactionInfo fi;
    fi.space_unit_cap = 10.0f;
    c.factions = {fi, fi, fi};
    return c;
}

GalacticSetup make_setup() {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SaveSynthetic";
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
    e.human = true;
    e.credits = 8000.0f;
    e.home = rA;
    PlayerSetup u;
    u.faction = 1;
    u.ai = true;
    u.difficulty = 1;
    u.credits = 8000.0f;
    PlayerSetup n;
    n.faction = 2;
    s.players = {e, u, n};
    s.forces = {{pEmpire, rA, tSB1},     {pEmpire, rA, tBarracks}, {pEmpire, rA, tFrigate},  {pEmpire, rA, tFrigate},
                {pEmpire, rB, tBarracks}, {pUnder, rE, tSB1},       {pUnder, rE, tBarracks},  {pUnder, rE, tCorvette},
                {pUnder, rC, tCorvette},  {pUnder, rC, tCorvette},  {pUnder, rD, tInfantry}};
    return s;
}

uint32_t idle_fleet(const GalacticSim& s, PlanetIndex p, int32_t player) {
    for (uint32_t f : s.fleets_at(p, player))
        if (!s.fleets()[f].moving) return f;
    return kNoFleet;
}

void drive_synthetic(GalacticSim& s, ConflictManager& cm, const objects::ObjectStore& st, int32_t t) {
    const auto id = [&st](ObjectHandle h) { return static_cast<uint32_t>(st.id(h)); };
    switch (t) {
        case 1:
            s.enqueue(pEmpire, rA, tFrigate);
            s.enqueue(pEmpire, rA, tInfantry);
            break;
        case 2:
            s.enqueue(pUnder, rE, tCorvette);
            s.enqueue(pUnder, rE, tInfantry);
            break;
        case 3:
            s.set_black_market_influence(rC, true);
            s.set_black_market_influence(rB, true);
            break;
        case 5: {
            const uint32_t f = idle_fleet(s, rA, pEmpire);
            if (f != kNoFleet) s.move_fleet(pEmpire, f, rB);
            break;
        }
        case 8:
            s.enqueue(pEmpire, rA, tFighters);
            s.enqueue(pEmpire, rA, tFighters);
            break;
        case 9: s.cancel(pEmpire, rA, kSpaceQueue, 2); break;
        case 10: s.begin_corruption_gain(rB, kCorruptionPiracy, pUnder, 20.0f); break;
        case 12: {
            VisibilityKey k;
            k.object = 1234;
            k.sub = 7;
            s.add_visibility_modifier(rC, pEmpire, k, kVisFleetContents, 30.0f, 99u);
            break;
        }
        case 15: s.set_galactic_stealth(s.planets()[rE].starbase, true); break;
        case 16:  // cu8f t04b: Set_Prefer_Ground_Over_Space false on one unit (save v5)
            for (ObjectHandle u : s.planets()[rA].surface)
                if (st.alive(u)) {
                    s.set_prefer_ground_over_space(u, false);
                    break;
                }
            break;
        case 20: {
            const uint32_t f = idle_fleet(s, rC, pUnder);
            if (f != kNoFleet) s.move_fleet(pUnder, f, rB);
            break;
        }
        case 25: {
            const uint32_t f = idle_fleet(s, rE, pUnder);
            if (f != kNoFleet && !s.fleets()[f].contents.empty()) s.hero_depart(s.fleets()[f].contents[0], rE, 25.0f);
            break;
        }
        case 30:
            s.lock_type(pEmpire, tCorvette, true);
            s.set_production_disabled(pUnder, tFighters, true);
            s.set_bm_tutorial(pEmpire, true);
            s.count_corruption(pUnder, 1);
            break;
        case 35: s.set_relation(pEmpire, pNeutral, Relation::Ally); break;
        case 40:
            s.claim_victory(pUnder, 3, 100000);
            s.victory_hold_raise();  // story t11: the galactic monitor hold (save v2)
            s.victory_hold_raise();
            break;
        case 45:
            s.set_siphon(rA, pUnder, 0.25f, s.frame() + 2000);
            s.add_income_entry(rB, 15.0f, s.frame() + 1500);
            s.set_credit_halt(rE, s.frame() + 100);
            break;
        case 50: s.story_set_planet_visibility(rD, "Empire", "Fleet_Presence", "SAVE_TEST_KEY", 40.0f); break;
        case 55: s.set_tech_level(pUnder, 2); break;
        case 60:  // q6 (AI_GALACTIC_TOKENS §9.4): persisted tactical built objects on D (save v2), filtered by its land result
            if (std::vector<PersistedBuiltObject>* v = s.built_objects(rD)) {
                v->push_back({tInfantry, 100.0f, -50.0f, 0.1f, 2, pUnder});
                v->push_back({tFrigate, 300.5f, 40.25f, 12.1f, 0, pEmpire});
            }
            break;
        case 300: {
            const uint32_t f = idle_fleet(s, rE, pUnder);
            if (f != kNoFleet)
                s.execute(static_cast<uint8_t>(pUnder),
                          fleet_management_event(kFleetSlot, commands::kNoObject, id(s.fleets()[f].object), commands::kNoObject,
                                                 commands::kNoObject, 2));
            break;
        }
        case 700: {
            std::vector<ObjectHandle> units;
            for (ObjectHandle u : s.planets()[rA].surface)
                if (st.alive(u) && st.owner(u) == pEmpire) units.push_back(u);
            if (!units.empty()) s.launch_units(pEmpire, rA, units);
            const uint32_t f = idle_fleet(s, rA, pEmpire);
            if (f != kNoFleet) s.move_fleet(pEmpire, f, rD);  // A -> B -> D
            break;
        }
        case 1200: s.queue_linked_battle(); break;
        case 1260: s.clear_linked_battle(); break;
        case 2200:
            if (s.corrupted(rB)) s.begin_corruption_removal(rB, 10.0f);
            break;
        default: break;
    }
    // A land invasion of D by the Empire fleet once it is idle there (the human's choice comes from drive_conflicts).
    if (t > 700 && t % 50 == 0 && cm.land_queue().empty() && s.planets()[rD].owner != pEmpire) {
        const uint32_t f = idle_fleet(s, rD, pEmpire);
        if (f != kNoFleet) s.invade(pEmpire, rD, {});
    }
    drive_conflicts(s, cm, st, t);
}

void test_synthetic() {
    const GalacticRules rules = make_rules();
    const ConflictRules crules = make_conflict_rules();
    const objects::GameObjectTypes types;
    Scenario sc;
    sc.name = "synthetic";
    sc.rules = &rules;
    sc.crules = &crules;
    sc.types = &types;
    sc.setup = make_setup();
    sc.cfg.human_choice = BattleChoice::Pending;  // the human's choice waits (Vote) until the driver answers
    sc.seed = 0x5A5Eu;
    sc.ticks = 3000;
    sc.save_ticks = {0, 12, 40, 100, 150, 250, 400, 650, 760, 1000, 1230, 1500, 2000, 2300, 2900, 2998};
    sc.prepare = [](GalacticSim& s) {
        s.set_emit_events(true);
        s.reveal_all_planets();
    };
    sc.drive = drive_synthetic;
    sc.full_prefix_limit = 1u << 20;  // every prefix
    run_scenario(sc);
}

// A2k t04 (A2J_RE_BUILD_MARKET §4): start() draws each planet's two build-queue phases (synced Get_Int(1, 30), queue 0
// then 1, row order) at the PRODUCTION attach; nothing else in start() draws, so the RNG after start() is the reference
// stream after exactly those draws. The phases are saved: a session started from another seed reads the saved values.
void test_queue_phases() {
    std::printf("  queue phases\n");
    const GalacticRules rules = make_rules();
    const objects::GameObjectTypes types;
    const GalacticSetup setup = make_setup();
    for (const uint32_t seed : {0x5A5Eu, 1u, 0xDEADBEEFu}) {
        objects::IdCounter ids;
        objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
        SimRng rng(seed), ref(seed);
        GalacticSim s(rules, store, rng);
        CHECK(s.start(setup));
        bool spread = false;
        for (PlanetIndex p = 0; p < s.planets().size(); ++p) {
            for (uint32_t q = 0; q < 2; ++q) {
                const int32_t v = s.build_queue_phase(p, q);
                if (!s.planets()[p].is_planet) {
                    CHECK(v == 0);
                    continue;
                }
                CHECK(v == ref.range_int(1, 30) && v >= 1 && v <= 30);
                spread = spread || v != s.build_queue_phase(rA, 0);
            }
            CHECK(s.build_queue_phase(p, 2) == 0);
        }
        CHECK(s.build_queue_phase(rCore, 0) == 0 && s.build_queue_phase(static_cast<PlanetIndex>(s.planets().size()), 0) == 0);
        CHECK(spread);   // ten draws are not all equal
        CHECK(rng == ref);  // the draws are the only ones in start()

        // Saved and restored: a session started from another seed (other phases) reads the saved ones.
        const std::vector<uint8_t> bytes = sim_bytes(s);
        objects::IdCounter ids2;
        objects::ObjectStore store2(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids2);
        SimRng rng2(seed + 7u);
        GalacticSim c(rules, store2, rng2);
        CHECK(c.start(setup));
        bool differ = false;
        for (PlanetIndex p = 0; p < c.planets().size(); ++p)
            for (uint32_t q = 0; q < 2; ++q) differ = differ || c.build_queue_phase(p, q) != s.build_queue_phase(p, q);
        CHECK(differ);
        store2 = store;  // ObjectStore::read stand-in, as in run_scenario
        ids2.set_next(ids.next());
        ByteReader r(bytes);
        CHECK(c.read(r));
        for (PlanetIndex p = 0; p < c.planets().size(); ++p)
            for (uint32_t q = 0; q < 2; ++q) CHECK(c.build_queue_phase(p, q) == s.build_queue_phase(p, q));
        CHECK(sim_bytes(c) == bytes);

        // Only the current version is read (cu6 rule): the same bytes stamped with v3 are rejected.
        std::vector<uint8_t> older = bytes;
        older[0] = 3;
        objects::IdCounter ids3;
        objects::ObjectStore store3(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids3);
        SimRng rng3(seed + 7u);
        GalacticSim o(rules, store3, rng3);
        CHECK(o.start(setup));
        store3 = store;
        ids3.set_next(ids.next());
        ByteReader orr(older);
        CHECK(!o.read(orr));

        // cu8f t04b: v5 adds the Prefer_Ground_Over_Space false ids at the end; a v4 image (no list) still loads with every
        // unit preferring ground. The flag is saved and enters the hash only once a live unit has it false.
        CHECK(bytes.size() >= 4 && bytes[0] == 5 && bytes[bytes.size() - 4] == 0 && bytes[bytes.size() - 1] == 0);
        std::vector<uint8_t> v4(bytes.begin(), bytes.end() - 4);
        v4[0] = 4;
        ByteReader r4(v4);
        CHECK(o.read(r4) && sim_bytes(o) == bytes);
        ObjectHandle unit{};
        for (const PlanetState& ps : s.planets())
            for (ObjectHandle u : ps.surface)
                if (!unit && store.alive(u)) unit = u;
        CHECK(static_cast<bool>(unit) && s.prefer_ground_over_space(unit));
        const uint64_t h0 = sim_hash(s);
        s.set_prefer_ground_over_space(unit, false);
        CHECK(!s.prefer_ground_over_space(unit) && sim_hash(s) != h0);
        const std::vector<uint8_t> flagged = sim_bytes(s);
        ByteReader rf(flagged);
        CHECK(o.read(rf) && !o.prefer_ground_over_space(o.store().find(store.id(unit))) && sim_bytes(o) == flagged);
        s.set_prefer_ground_over_space(unit, true);
        CHECK(s.prefer_ground_over_space(unit) && sim_hash(s) == h0 && sim_bytes(s) == bytes);
    }
}

// ---- 3. FoC data ----------------------------------------------------------------------------------------------------------

using ::alamo::test::env_or_empty;

void test_foc() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    std::string how;
    std::unique_ptr<FileProvider> provider = testing::open_foc_data(root, how);
    if (!provider) {
        std::printf("ALAMO_FOC_DATA unset or without Data\\XML: FoC save / load run skipped\n");
        return;
    }
    std::printf("  FoC data: %s\n", how.c_str());
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    GalacticRules rules;
    ConflictRules crules;
    std::vector<std::string> diag;
    CHECK(db.load(*provider, {}, &diag));
    CHECK(raw.load(*provider, &diag));
    CHECK(types.build(raw, provider.get(), &diag));
    CHECK(load_galactic_rules(*provider, db, types, rules, &diag));
    CHECK(load_conflict_rules(*provider, db, types, rules, crules, &diag));
    Scenario sc;
    sc.name = "FoC Sandbox_Heart_Of_The_Maw_Underworld";
    sc.rules = &rules;
    sc.crules = &crules;
    sc.types = &types;
    sc.setup = build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    sc.cfg.human_choice = BattleChoice::Pending;
    sc.seed = 0xC0FFEEu;
    sc.ticks = 3000;
    sc.save_ticks = {0, 11, 61, 95, 155, 400, 900, 1500, 2100, 2800};
    sc.expect_conflicts = false;  // the human attack depends on the campaign's start positions (reported, not required)
    sc.full_prefix_limit = 4096;
    struct Names {
        PlanetIndex felucia, shola, kuat, corellia, kamino, naboo, mon_cal, maw, tatooine;
        uint32_t gunship, mercs, tartan, corvette, starviper;
    };
    const GalaxyMap& m = sc.setup.map;
    const Names n{m.planets.find("Felucia"), m.planets.find("Shola"),   m.planets.find("Kuat"),
                  m.planets.find("Corellia"), m.planets.find("Kamino"), m.planets.find("Naboo"),
                  m.planets.find("MonCalimari"), m.planets.find("The_Maw"), m.planets.find("Tatooine"),
                  rules.find_type("Crusader_Gunship"), rules.find_type("Underworld_Merc_Squad"),
                  rules.find_type("Tartan_Patrol_Cruiser"), rules.find_type("Corellian_Corvette"),
                  rules.find_type("StarViper_Squadron")};
    sc.prepare = [](GalacticSim& s) {
        s.set_emit_events(true);
        const PlanetIndex felucia = s.map().planets.find("Felucia");
        s.set_black_market_influence(felucia, true);
    };
    sc.drive = [n](GalacticSim& s, ConflictManager& cm, const objects::ObjectStore& st, int32_t t) {
        const int32_t U = s.player_of_faction("Underworld"), E = s.player_of_faction("Empire"), R = s.player_of_faction("Rebel");
        if (t == 10) {
            s.enqueue(U, n.felucia, n.gunship);
            s.enqueue(U, n.shola, n.mercs);
            s.enqueue(U, n.shola, n.mercs);
        }
        if (t == 12) s.enqueue(E, n.kuat, n.tartan);
        if (t == 14) s.enqueue(R, n.mon_cal, n.corvette);
        if (t == 20) {
            s.enqueue(U, n.shola, n.starviper);
            s.enqueue(U, n.shola, n.starviper);
        }
        if (t == 21) s.cancel(U, n.shola, kSpaceQueue, 1);
        if (t == 60) {
            // The human attacks: its first idle fleet (fleet order) with a revealed neighbour (edge order) that holds a
            // hostile combat fleet or a hostile starbase.
            bool done = false;
            for (uint32_t f = 0; f < s.fleets().size() && !done; ++f) {
                const FleetState& fl = s.fleets()[f];
                if (!fl.alive || fl.moving || fl.owner != U || fl.planet == kInvalidPlanet) continue;
                for (uint32_t e = s.graph().edge_begin(fl.planet); e < s.graph().edge_end(fl.planet) && !done; ++e) {
                    const PlanetIndex nb = s.graph().neighbour(e);
                    if (!s.planet_revealed_raw(nb)) continue;
                    const PlanetState& p = s.planets()[nb];
                    bool hostile = p.starbase_level > 0 && s.is_hostile(U, p.starbase_owner);
                    for (uint32_t g : p.orbit) hostile = hostile || (s.fleets()[g].alive && s.is_hostile(U, s.fleets()[g].owner));
                    if (hostile && s.move_fleet(U, f, nb) == RejectReason::None) done = true;
                }
            }
            std::printf("    human attack order %s\n", done ? "given" : "not possible (no hostile neighbour)");
        }
        if (t == 90) {
            const uint32_t f = idle_fleet(s, n.kuat, E);
            if (f != kNoFleet) s.move_fleet(E, f, n.corellia);
        }
        if (t == 150) {
            const uint32_t f = idle_fleet(s, n.kamino, R);
            if (f != kNoFleet) s.move_fleet(R, f, n.naboo);
        }
        if (t == 300) s.begin_corruption_gain(n.tatooine, kCorruptionPiracy, U, 15.0f);
        if (t == 400) {
            for (ObjectHandle u : s.planets()[n.shola].surface)
                if (st.alive(u) && st.owner(u) == U) {
                    s.hero_depart(u, n.shola, 20.0f);
                    break;
                }
        }
        if (t == 1500) {
            const uint32_t f = idle_fleet(s, n.naboo, R);
            if (f != kNoFleet) s.move_fleet(R, f, n.mon_cal);
        }
        if (t % 1350 == 700) {
            s.enqueue(E, n.kuat, n.tartan);
            s.enqueue(R, n.mon_cal, n.corvette);
        }
        drive_conflicts(s, cm, st, t);
    };
    run_scenario(sc);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_synthetic();
    test_queue_phases();
    test_foc();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("alamo_galactic_save_tests: all passed\n");
    return 0;
}
