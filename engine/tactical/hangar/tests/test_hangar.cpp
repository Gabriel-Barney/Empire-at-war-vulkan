// Hangar tests (docs/re/STATION_HANGARS_SOUND.md §1-§3): SPAWN_SQUADRON service (first-due draw, one launch per
// service and delay, the three synced draws, bay bone placement, escort vs move order), loss / timer rules, reserves,
// can-spawn gates (cinematic, corruption, hyperspace, DEFEND, planet restriction, +0x3CB, bays), DEPLOY_SQUAD state 4,
// tech slots, community property and pending deliveries, carrier death; the station upgrade driven through the
// abilities module (Starbase_Upgrade_Ability service -> StationUpgrade + RemoveSilently -> complete_station_upgrade);
// engine loop requests; a repeat determinism check; and FoC data (ALAMO_FOC_DATA: an Empire station upgraded
// mid-battle, a Star Destroyer launching and replacing squadrons).
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/hangar/contents.hpp"
#include "alamo/tactical/hangar/engine_loops.hpp"
#include "alamo/tactical/hangar/hangar_system.hpp"
#ifdef ALAMO_HANGAR_TESTS_HAVE_DATA
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::combat;
using namespace alamo::tactical::hangar;
namespace ta = alamo::tactical::abilities;

namespace {

int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

bool near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }
bool near3(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

constexpr int32_t kTypeA = 10, kTypeB = 11;

// Combat-only sandbox: a carrier with two bays (indices 0 and 2) and a laser, a bay-less structure, fighters.
struct World {
    CombatData cd;
    std::unique_ptr<CombatSystem> sys;
    std::unique_ptr<HangarSystem> hg;
    SimRng rng{99u};   // combat (unit add)
    SimRng hrng{7u};   // the hangar's synced draws (separate so tests can mirror them)
    uint32_t fighter = 0, carrier = 0, structure = 0;
    std::vector<LaunchRequest> log;
    std::vector<uint32_t> log_frame;
    std::vector<ObjectId> log_id;
    std::vector<HangarEvent> ev;
    ObjectId next_id = 100;

    void make(std::vector<HangarDesc> descs, std::vector<PlayerInfo> players = {PlayerInfo{}, PlayerInfo{}}) {
        HardpointDesc bay;
        bay.kind = HardpointKind::FighterBay;
        bay.health = 100.0f;
        bay.destroyable = bay.targetable = true;
        bay.attach_offset = Vec3{100.0f, 0.0f, 0.0f};
        bay.fighter_bay_flyout_distance = 400.0f;
        HardpointDesc bay2 = bay;
        bay2.attach_offset = Vec3{-100.0f, 50.0f, 10.0f};
        bay2.fighter_bay_flyout_distance = 200.0f;
        HardpointDesc laser;
        laser.kind = HardpointKind::WeaponLaser;
        laser.health = 100.0f;
        laser.destroyable = laser.targetable = true;
        cd.hardpoints = {bay, laser, bay2};
        UnitDesc f;
        f.name = "Fighter";
        f.tactical_health = 10.0f;
        UnitDesc c;
        c.name = "Carrier";
        c.tactical_health = 5000.0f;
        c.radius = 100.0f;
        c.hardpoints = {0, 1, 2};
        UnitDesc s = c;
        s.name = "Structure";
        s.hardpoints = {1};
        s.has_locomotor = false;
        cd.units = {f, c, s};
        fighter = 0;
        carrier = 1;
        structure = 2;
        CombatConfig cfg;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        sys = std::make_unique<CombatSystem>(cd, cfg, std::move(players));
        sys->bind_rng(&rng);
        hg = std::make_unique<HangarSystem>(*sys, std::move(descs));
    }
    void add(ObjectId id, uint32_t desc, int32_t owner, Vec3 p, Vec3 f = {1, 0, 0}) {
        UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = p;
        s.facing = f;
        CHECK(sys->add_unit(s, 1, rng));
    }
    // Host: create one fighter per request and bind it; keep the requests and events.
    void step(uint32_t frame, bool create = true) {
        hg->update(frame, hrng);
        for (const LaunchRequest& q : hg->launches()) {
            log.push_back(q);
            log_frame.push_back(frame);
            ObjectId id = kNoObject;
            if (create) {
                id = next_id++;
                add(id, fighter, q.owner, q.origin, q.facing);
                CHECK(hg->bind_launch(q.serial, id));
            }
            log_id.push_back(id);
        }
        for (const HangarEvent& e : hg->events()) ev.push_back(e);
        hg->clear_requests();
    }
    void run(uint32_t from, uint32_t to) {
        for (uint32_t f = from; f < to; ++f) step(f);
    }
    // The squadron container is removed (signal 0x28).
    void lose(ObjectId id, uint32_t frame) {
        sys->kill_unit(id, kNoObject, kKindNormal, frame);
        hg->squadron_removed(id, frame);
    }
    size_t events(HangarEventKind k) const {
        size_t n = 0;
        for (const HangarEvent& e : ev) n += e.kind == k ? 1u : 0u;
        return n;
    }
};

// Starting A x2, B x1; reserve A +1 (B none); 5 s.
HangarDesc two_entry_desc() {
    HangarDesc d;
    d.name = "Carrier";
    d.starting[0] = {SpawnEntry{kTypeA, 2}, SpawnEntry{kTypeB, 1}};
    d.reserve[0] = {SpawnEntry{kTypeA, 1}};
    d.delay_seconds = 5.0f;
    return d;
}

HangarDesc one_entry_desc(int32_t count, int32_t reserve, float delay) {
    HangarDesc d;
    d.starting[0] = {SpawnEntry{kTypeA, count}};
    d.reserve[0] = {SpawnEntry{kTypeA, reserve}};
    d.delay_seconds = delay;
    return d;
}

void test_parse() {
    std::string t;
    int32_t n = 0;
    CHECK(parse_type_count(" TIE_Fighter_Squadron, 2 ", t, n) && t == "TIE_Fighter_Squadron" && n == 2);
    CHECK(parse_type_count("Y-Wing_Squadron,-1", t, n) && t == "Y-Wing_Squadron" && n == -1);
    CHECK(!parse_type_count("Nothing", t, n) && !parse_type_count("A, x", t, n));
    const std::vector<std::string_view> st = {"TIE_Interceptor_Squadron, 2", "TIE_Bomber_Squadron", "1", "Unknown_Squadron, 3"};
    const SpawnList l = parse_spawn_list(st, [](std::string_view s) -> int32_t {
        if (s == "TIE_Interceptor_Squadron") return 1;
        if (s == "TIE_Bomber_Squadron") return 2;
        return -1;
    });
    CHECK(l.size() == 3 && l[0].type == 1 && l[0].count == 2 && l[1].type == 2 && l[1].count == 1 && l[2].type == -1);
    // Tech slot: the highest N <= tech with entries; tech > 5 none.
    std::array<SpawnList, kTechSlots> lists;
    lists[0] = {SpawnEntry{1, 1}};
    lists[2] = {SpawnEntry{2, 1}};
    CHECK(tech_slot(lists, 0) == 0 && tech_slot(lists, 1) == 0 && tech_slot(lists, 2) == 2 && tech_slot(lists, 5) == 2);
    CHECK(tech_slot(lists, 6) == -1 && tech_slot(lists, -1) == -1);
    lists[0].clear();
    CHECK(tech_slot(lists, 1) == -1);
    // Starbase_Upgrade_Ability: service interval 2 (STATION_HANGARS_SOUND §1.1).
    CHECK(ta::special_class_from_tag("starbase_upgrade_ability") == ta::SpecialClass::StarbaseUpgrade);
    CHECK(ta::special_service_interval(ta::SpecialClass::StarbaseUpgrade, 30) == 2);
}

// §2.3 / §2.5: first-due draw, one launch per service, the three draws, bay bone placement, delay, cap.
void test_launch_sequence() {
    World w;
    w.make({two_entry_desc()});
    w.add(1, w.carrier, 0, Vec3{0, 0, 0}, Vec3{0, 2, 0});  // facing +y (normalised in XY)
    SimRng m = w.hrng;
    CHECK(w.hg->add_carrier(1, 0, 10, &w.hrng));
    const uint32_t due = 10u + static_cast<uint32_t>(m.range_int(0, 29));
    CHECK(w.hrng == m && w.hg->next_service_frame(1) == due);
    CHECK(!w.hg->add_carrier(1, 0, 10, nullptr) && !w.hg->add_carrier(2, 0, 10, nullptr) && !w.hg->add_carrier(1, 5, 10, nullptr));
    w.run(10, due);
    CHECK(w.log.empty() && w.hg->entry_count(1) == 0);
    // First service: the list is built, then can-spawn draw, launch draw, bay draw.
    const int32_t r1 = m.range_int(0, 1);
    const int32_t r2 = m.range_int(0, 1);
    const int32_t r3 = m.range_int(0, 1);
    (void)r1;
    w.step(due);
    CHECK(w.hrng == m);
    CHECK(w.hg->entry_count(1) == 2 && w.hg->max_alive(1, 0) == 2 && w.hg->remaining(1, 0) - (r2 == 0 ? -1 : 0) == 3);
    CHECK(w.hg->remaining(1, 1) - (r2 == 1 ? -1 : 0) == 1);  // no reserve entry: start only
    CHECK(w.log.size() == 1);
    if (w.log.size() != 1) return;
    const LaunchRequest& q = w.log[0];
    CHECK(q.entry == r2 && q.type == (r2 == 0 ? kTypeA : kTypeB) && q.owner == 0 && q.escort && !q.from_pending);
    if (r3 == 0) {  // bay 0 local (100, 0, 0) rotated to +y -> (0, 100, 0); flyout 400 along the bone X axis
        CHECK(q.bay == 0 && near3(q.origin, Vec3{0, 100, 0}) && near3(q.flyout, Vec3{0, 500, 0}));
    } else {        // bay 2 local (-100, 50, 10) -> (-50, -100, 10); flyout 200
        CHECK(q.bay == 2 && near3(q.origin, Vec3{-50, -100, 10}) && near3(q.flyout, Vec3{-50, 100, 10}));
    }
    CHECK(near3(q.direction, Vec3{0, 1, 0}) && near3(q.facing, Vec3{0, 1, 0}));
    CHECK(w.hg->next_spawn_frame(1) == due + 150);  // int(now + 30 x 5)
    // One per delay, on the service frames: due + 150, due + 300; then the cap holds.
    w.run(due + 1, due + 1000);
    CHECK(w.log.size() == 3);
    if (w.log.size() == 3) {
        CHECK(w.log_frame[1] == due + 150 && w.log_frame[2] == due + 300);
        int a = 0, b = 0;
        for (const LaunchRequest& x : w.log) (x.type == kTypeA ? a : b) += 1;
        CHECK(a == 2 && b == 1);
    }
    CHECK(w.hg->live(1, 0) == 2 && w.hg->live(1, 1) == 1 && w.hg->remaining(1, 0) == 1 && w.hg->remaining(1, 1) == 0);
    CHECK(w.events(HangarEventKind::Launched) == 3 && w.hg->carrier_of(100) == 1 && w.hg->carrier_of(1) == kNoObject);
}

// §2.7: loss -> live - 1; the timer restarts only when every entry up to the matched one was full; reserves.
void test_losses() {
    World w;
    w.make({two_entry_desc()});
    w.add(1, w.carrier, 0, Vec3{0, 0, 0});
    CHECK(w.hg->add_carrier(1, 0, 0, nullptr));  // due 0
    w.run(0, 400);
    CHECK(w.log.size() == 3);
    if (w.log.size() != 3) return;
    ObjectId b = kNoObject;
    std::vector<ObjectId> as;
    for (size_t i = 0; i < w.log.size(); ++i) {
        if (w.log[i].type == kTypeB) b = w.log_id[i];
        else as.push_back(w.log_id[i]);
    }
    CHECK(b != kNoObject && as.size() == 2);
    // Lose B (entries 0..1 full): restart, next = int(30 x 5) + now. B has no reserve left: never relaunched.
    w.lose(b, 410);
    CHECK(w.hg->next_spawn_frame(1) == 560 && w.hg->live(1, 1) == 0);
    w.run(400, 900);
    CHECK(w.log.size() == 3 && w.events(HangarEventKind::SquadronLost) == 1);
    // Lose an A (entry 0 full): restart at 910 + 150; lose the second A 10 frames later (entry 0 no longer full): kept.
    w.lose(as[0], 910);
    CHECK(w.hg->next_spawn_frame(1) == 1060);
    w.lose(as[1], 920);
    CHECK(w.hg->next_spawn_frame(1) == 1060 && w.hg->live(1, 0) == 0);
    // One reserve launch is left for A: at the first service >= 1060 (service frames are multiples of 30: 1080).
    w.run(900, 2000);
    CHECK(w.log.size() == 4 && w.log_frame.back() == 1080 && w.log.back().type == kTypeA);
    CHECK(w.hg->live(1, 0) == 1 && w.hg->remaining(1, 0) == 0);
    // Unknown objects are ignored.
    const size_t lost = w.events(HangarEventKind::SquadronLost);
    w.hg->squadron_removed(9999, 2000);
    CHECK(w.hg->events().empty() && lost == 3);
    // A request the host did not bind is rolled back (the spawn failed).
    World u;
    u.make({one_entry_desc(1, 2, 1.0f)});
    u.add(1, u.carrier, 0, Vec3{0, 0, 0});
    CHECK(u.hg->add_carrier(1, 0, 0, nullptr));
    u.step(0, false);
    CHECK(u.log.size() == 1 && u.hg->live(1, 0) == 0 && u.hg->remaining(1, 0) == 3);
    u.run(1, 31);
    CHECK(u.log.size() == 2 && u.hg->live(1, 0) == 1 && u.hg->remaining(1, 0) == 2);
}

// §2.4: cinematic / corruption / hyperspace / DEFEND / planet restriction / +0x3CB gates; destroyed vs disabled bays.
void test_gates_and_bays() {
    World w;
    w.make({one_entry_desc(10, -1, 1.0f), one_entry_desc(1, -1, 1.0f)});
    bool cine = false, corrupt = false, hyper = false, defend = false, restricted = false;
    HangarHost h;
    h.cinematic_camera = [&] { return cine; };
    h.corruption_blocked = [&](int32_t) { return corrupt; };
    h.in_hyperspace = [&](ObjectId) { return hyper; };
    h.defend_active = [&](ObjectId) { return defend; };
    h.planet_restricted = [&](int32_t t) { return restricted && t == kTypeA; };
    w.hg->set_host(h);
    w.add(1, w.carrier, 0, Vec3{0, 0, 0});
    CHECK(w.hg->add_carrier(1, 0, 0, nullptr));
    auto blocked = [&](bool& flag, uint32_t frame) {
        flag = true;
        const size_t n = w.log.size();
        const SimRng before = w.hrng;
        w.step(frame);
        CHECK(w.log.size() == n && w.hrng == before);
        flag = false;
    };
    blocked(cine, 0);
    blocked(corrupt, 30);
    blocked(hyper, 60);
    blocked(defend, 90);
    blocked(restricted, 120);
    w.hg->set_spawn_enabled(1, false);
    w.step(150);
    CHECK(w.log.empty() && !w.hg->spawn_enabled(1));
    w.hg->set_spawn_enabled(1, true);
    SimRng m = w.hrng;
    (void)m.range_int(0, 1);  // the bay draw (one entry: no entry draws)
    w.step(180);
    CHECK(w.log.size() == 1 && w.hrng == m);
    // Destroy bay 0: erased at the next can-spawn; the one bay left needs no draw. Disabled bay 2 still launches.
    DamageRequest r;
    r.victim = 1;
    r.amount = 1000.0f;
    r.hardpoint = 0;
    w.sys->take_damage(r, 200);
    w.sys->set_hardpoint_disabled(1, 2, true);
    m = w.hrng;
    w.step(210);
    CHECK(w.log.size() == 2 && w.log.back().bay == 2 && w.hrng == m && w.hg->bay_count(1) == 1);
    // Destroy bay 2 as well: BaysGone, never again.
    r.hardpoint = 2;
    w.sys->take_damage(r, 220);
    w.run(211, 600);
    CHECK(w.log.size() == 2 && w.events(HangarEventKind::BaysGone) == 1 && w.hg->bay_count(1) == 0);
    // A spawner without FIGHTER_BAY hardpoints never spawns (space); a station's order is the move to P1.
    w.add(1000, w.structure, 0, Vec3{1000, 0, 0});
    CHECK(w.hg->add_carrier(1000, 1, 600, nullptr));
    w.run(600, 700);
    CHECK(w.log.size() == 2 && w.events(HangarEventKind::BaysGone) == 2);
}

// §2.5 DEPLOY_SQUAD: a human-owned spawner goes to state 4 (manual); an AI-owned one launches. The handler resets it.
void test_deploy_squad() {
    World w;
    PlayerInfo ai;
    ai.ai = true;
    HangarDesc d = one_entry_desc(5, -1, 1.0f);
    d.deploy_squad = true;
    w.make({d}, {PlayerInfo{}, ai});
    w.add(1, w.carrier, 0, Vec3{0, 0, 0});
    w.add(2, w.carrier, 1, Vec3{5000, 0, 0});
    CHECK(w.hg->add_carrier(1, 0, 0, nullptr) && w.hg->add_carrier(2, 0, 0, nullptr));
    w.step(0);
    CHECK(w.log.size() == 1 && w.log[0].carrier == 2);
    CHECK(w.hg->state(1) == 4 && w.hg->deploy_ready(1) && w.events(HangarEventKind::Manual) == 1);
    w.step(30);
    CHECK(w.log.size() == 2 && w.log[1].carrier == 2);
    // Activation at frame 40 with Recharge_Seconds 3: state 0, next = 40 + int((3 - 1) x 30) = 100.
    w.hg->deploy_squad(1, 40, 3.0f);
    CHECK(w.hg->state(1) == 0 && !w.hg->deploy_ready(1) && w.hg->next_spawn_frame(1) == 100);
    w.run(31, 121);
    size_t from1 = 0;
    for (const LaunchRequest& q : w.log) from1 += q.carrier == 1 ? 1u : 0u;
    CHECK(from1 == 1 && w.hg->state(1) == 0);  // launched at the 120 service
    w.run(121, 200);
    CHECK(w.hg->state(1) == 4);  // the next due launch goes manual again
}

// §2.1 tech slots; §2.4 item 8 / §2.5: community property rotation and the skirmish pending-delivery path.
void test_tech_community_pending() {
    {
        World w;
        HangarDesc d;
        d.starting[0] = {SpawnEntry{kTypeA, 1}};
        d.starting[2] = {SpawnEntry{kTypeB, 1}};
        d.reserve[0] = {SpawnEntry{kTypeB, 5}};
        w.make({d});
        int32_t tech = 3;
        HangarHost h;
        h.tech_level = [&](int32_t) { return tech; };
        w.hg->set_host(h);
        w.add(1, w.carrier, 0, Vec3{0, 0, 0});
        w.add(2, w.carrier, 0, Vec3{0, 0, 0});
        CHECK(w.hg->add_carrier(1, 0, 0, nullptr) && w.hg->add_carrier(2, 0, 1, nullptr));
        w.step(0);
        CHECK(w.hg->entry_type(1, 0) == kTypeB && w.hg->max_alive(1, 0) == 1 && w.hg->remaining(1, 0) == 5);  // 1 + 5, one spent
        tech = 1;
        w.step(1);
        CHECK(w.hg->entry_type(2, 0) == kTypeA && w.hg->remaining(2, 0) == 0);  // reserve slot 0 has no A: start only
        tech = 0;
        w.run(2, 300);
        CHECK(w.hg->entry_type(1, 0) == kTypeB);  // a tech change after the build does not change the list
    }
    {
        World w;
        HangarDesc st;
        st.starting[0] = {SpawnEntry{kTypeA, 3}};
        st.reserve[0] = {SpawnEntry{kTypeA, -1}};
        st.delay_seconds = 1.0f;
        st.community_property = true;
        st.station = true;
        HangarDesc empty;
        empty.station = true;
        empty.delay_seconds = 1.0f;
        w.make({st, empty}, {PlayerInfo{}, PlayerInfo{}, PlayerInfo{}});
        std::vector<int32_t> pending2 = {kTypeB};
        HangarHost h;
        h.allied = [](int32_t a, int32_t b) { return a == b || (a == 0 && b == 2) || (a == 2 && b == 0); };
        h.pending_delivery = [&](int32_t p, bool pop) -> int32_t {
            if (p != 2 || pending2.empty()) return -1;
            const int32_t t = pending2.front();
            if (pop) pending2.erase(pending2.begin());
            return t;
        };
        w.hg->set_host(h);
        w.add(3, w.carrier, 0, Vec3{0, 0, 0});
        w.add(4, w.carrier, 0, Vec3{0, 0, 0});
        CHECK(w.hg->add_carrier(3, 0, 0, nullptr) && w.hg->add_carrier(4, 1, 0, nullptr));
        w.run(0, 200);
        std::vector<int32_t> owners3;
        size_t pend = 0;
        for (const LaunchRequest& q : w.log) {
            if (q.carrier == 3) owners3.push_back(q.owner);
            if (q.carrier == 4) {
                CHECK(q.from_pending && q.entry == -1 && q.owner == 2 && q.type == kTypeB);
                ++pend;
            }
        }
        CHECK(owners3.size() == 3 && owners3[0] == 0 && owners3[1] == 2 && owners3[2] == 0);
        // +0x60 went to 2 for that launch; the empty list is rebuilt on every later service, which resets it to the owner.
        CHECK(pend == 1 && pending2.empty() && w.hg->spawn_player(4) == 0);
    }
}

// §2.9: carrier death - tracking stops, spawned units stay; silent removal.
void test_carrier_death() {
    World w;
    w.make({one_entry_desc(2, -1, 1.0f)});
    w.add(1, w.carrier, 0, Vec3{0, 0, 0});
    w.add(2, w.carrier, 0, Vec3{0, 0, 0});
    CHECK(w.hg->add_carrier(1, 0, 0, nullptr) && w.hg->add_carrier(2, 0, 0, nullptr));
    w.run(0, 40);
    CHECK(w.log.size() == 4);
    const ObjectId sq = w.log_id[0];
    const ObjectId owner_c = w.log[0].carrier;
    w.sys->kill_unit(owner_c, kNoObject, kKindNormal, 40);
    w.step(41);
    CHECK(w.events(HangarEventKind::CarrierLost) == 1 && !w.hg->has_carrier(owner_c) && w.sys->alive(sq));
    CHECK(w.hg->carrier_of(sq) == kNoObject);
    w.lose(sq, 50);
    CHECK(w.events(HangarEventKind::SquadronLost) == 0);
    w.hg->remove_carrier(owner_c == 1 ? 2 : 1);
    CHECK(!w.hg->has_carrier(1) && !w.hg->has_carrier(2) && w.hg->events().empty());
}

// §3: engine loop requests.
void test_engine_loops() {
    EngineLoops e(10.0f);  // threshold 11
    auto expect = [&](std::vector<EngineLoopRequest> want) {
        const auto got = e.requests();
        bool ok = got.size() == want.size();
        for (size_t i = 0; ok && i < got.size(); ++i)
            ok = got[i].id == want[i].id && got[i].loop == want[i].loop && got[i].start == want[i].start;
        CHECK(ok);
        e.clear_requests();
    };
    using L = EngineLoop;
    e.mode_enter(5, 1);
    expect({{5, L::Idle, true, 1}});
    e.locomotor_service(5, 10.99f, false, 2);
    expect({});
    e.locomotor_service(5, 11.0f, false, 3);
    expect({{5, L::Moving, true, 3}, {5, L::Idle, false, 3}});  // the moving call first
    e.locomotor_service(5, 50.0f, false, 4);
    expect({});
    e.locomotor_service(5, 0.0f, true, 5);  // hyperspace: untouched
    expect({});
    e.locomotor_service(5, 0.0f, false, 6);
    expect({{5, L::Moving, false, 6}, {5, L::Idle, true, 6}});
    e.engines_offline(5, 7);
    expect({{5, L::Idle, false, 7}});
    e.locomotor_service(5, 0.0f, false, 8);  // disabled engines are heard as idle
    expect({{5, L::Idle, true, 8}});
    e.engines_online(5, 9);
    expect({});
    e.timed_disable(5, 10);
    expect({{5, L::Idle, false, 10}});
    e.engines_online(5, 11);
    expect({{5, L::Idle, true, 11}});
    e.suppress(5, 12);
    expect({{5, L::Idle, false, 12}});
    e.locomotor_service(5, 20.0f, false, 13);
    expect({});
    e.unsuppress(5);
    e.locomotor_service(5, 20.0f, false, 14);
    expect({{5, L::Moving, true, 14}});
    CHECK(e.moving_on(5) && !e.idle_on(5));
    e.remove(5);
    CHECK(!e.moving_on(5) && e.requests().empty());
}

// The same script twice gives the same hashed state and draws; a different script differs.
uint64_t scripted_digest(bool variant) {
    World w;
    w.make({two_entry_desc(), one_entry_desc(3, 4, 2.0f)});
    w.add(1, w.carrier, 0, Vec3{0, 0, 0});
    w.add(2, w.carrier, 1, Vec3{3000, 0, 0});
    w.hg->add_carrier(2, 1, 7, &w.hrng);
    w.hg->add_carrier(1, 0, 1, &w.hrng);
    StateHasher all;
    for (uint32_t f = 1; f < 1500; ++f) {
        if (f % 97 == 0 && !w.log_id.empty()) {
            const ObjectId id = w.log_id[f % w.log_id.size()];
            if (w.sys->alive(id)) w.lose(id, f);
        }
        if (variant && f == 500) w.sys->kill_unit(1, kNoObject, kKindNormal, f);
        w.step(f);
        StateHasher h;
        w.hg->hash(h);
        all.u64(h.digest());
        all.u32(w.hrng.state());
    }
    return all.digest();
}

void test_determinism() {
    const uint64_t a = scripted_digest(false), b = scripted_digest(false), c = scripted_digest(true);
    std::printf("hangar scripted digest %016llx\n", static_cast<unsigned long long>(a));
    CHECK(a == b && a != c);
    World w;
    w.make({two_entry_desc()});
    CrcRegistry reg;
    w.hg->register_crc(reg, "hangar");
    CHECK(reg.size() == 1);
}

// §1: Starbase_Upgrade_Ability end to end. The upgrade object's ability is scheduled at Get_Int(now, now + 2); on that
// service the host gets one StationUpgrade request and one RemoveSilently event; it spawns Next_Level_Base, completes
// the upgrade (hardpoints by index, +0x3CB, the old station leaves the hangar uncounted) and removes the old station.
// The new station launches a full complement of its own.
void test_station_upgrade_abilities() {
    CombatData cd;
    ta::AbilityData ad;
    HardpointDesc bay;
    bay.kind = HardpointKind::FighterBay;
    bay.health = 100.0f;
    bay.destroyable = bay.targetable = true;
    bay.fighter_bay_flyout_distance = 300.0f;
    HardpointDesc laser = bay;
    laser.kind = HardpointKind::WeaponLaser;
    HardpointDesc sw = laser;
    sw.kind = HardpointKind::WeaponSpecial;
    sw.allows_special_weapon_use = true;
    cd.hardpoints = {bay, laser, sw};
    auto push = [&](UnitDesc u, std::vector<uint32_t> specials) {
        ta::AbilityObjectDesc a;
        a.name = u.name;
        a.specials = std::move(specials);
        cd.units.push_back(std::move(u));
        ad.objects.push_back(std::move(a));
        return static_cast<uint32_t>(cd.units.size() - 1);
    };
    UnitDesc st1;
    st1.name = "Station_1";
    st1.tactical_health = 4000.0f;
    st1.repairs_hardpoints = true;  // DUMMY_STAR_BASE
    st1.has_locomotor = false;
    st1.hardpoints = {0, 1, 2};
    UnitDesc st2 = st1;
    st2.name = "Station_2";
    st2.hardpoints = {0, 1, 2, 1};
    UnitDesc fighter;
    fighter.name = "Fighter";
    fighter.tactical_health = 10.0f;
    UnitDesc dummy;
    dummy.name = "Upgrade";
    ta::SpecialAbilityDesc up;
    up.name = "ES_Level_Two_Starbase_Upgrade_Effect";
    up.class_tag = "Starbase_Upgrade_Ability";
    up.cls = ta::special_class_from_tag(up.class_tag);
    up.style = ta::ActivationStyle::SkirmishAutomatic;
    ad.specials.push_back(up);
    const uint32_t d1 = push(st1, {}), d2 = push(st2, {}), df = push(fighter, {}), du = push(dummy, {0});
    CombatConfig cfg;
    cfg.object_max_health_multiplier = 1.0f;
    cfg.hardpoint_health_multiplier = 1.0f;
    CombatSystem sys(cd, cfg, {PlayerInfo{}, PlayerInfo{}});
    ta::AbilityConfig acfg;
    acfg.skirmish = true;
    ta::AbilitySystem ab(sys, ad, acfg);
    SimRng rng{5u};
    sys.bind_rng(&rng);
    ab.bind_rng(&rng);
    HangarDesc h1;
    h1.starting[0] = {SpawnEntry{static_cast<int32_t>(df), 2}};
    h1.reserve[0] = {SpawnEntry{static_cast<int32_t>(df), -1}};
    h1.delay_seconds = 10.0f;
    h1.station = true;
    HangarDesc h2 = h1;
    h2.starting[0][0].count = 3;
    HangarSystem hg(sys, {h1, h2});
    hg.set_abilities(&ab);
    ObjectId next = 10;
    std::vector<std::pair<ObjectId, ObjectId>> launched;  // (carrier, squadron)
    auto spawn_unit = [&](uint32_t desc, int32_t owner, Vec3 p) {
        UnitSpawn s;
        s.id = next++;
        s.desc = desc;
        s.owner = owner;
        s.position = p;
        CHECK(ab.add_unit(s, desc, 1, rng));
        return s.id;
    };
    auto host = [&](uint32_t frame) {
        hg.update(frame, rng);
        for (const LaunchRequest& q : hg.launches()) {
            CHECK(!q.escort);  // a station: formation move to P1
            const ObjectId id = spawn_unit(static_cast<uint32_t>(q.type), q.owner, q.origin);
            hg.bind_launch(q.serial, id);
            launched.emplace_back(q.carrier, id);
        }
        hg.clear_requests();
    };
    const ObjectId old_st = spawn_unit(d1, 0, Vec3{100, 200, 0});
    CHECK(hg.add_carrier(old_st, 0, 1, &rng));
    for (uint32_t f = 1; f < 700; ++f) host(f);
    CHECK(launched.size() == 2 && hg.live(old_st, 0) == 2);
    // battle damage: the bay and the special-weapon hardpoint are shot down; Set_Garrison_Spawn(false)
    DamageRequest r;
    r.victim = old_st;
    r.amount = 1000.0f;
    r.hardpoint = 0;
    sys.take_damage(r, 700);
    r.hardpoint = 2;
    sys.take_damage(r, 700);
    CHECK(!sys.special_weapon_hp_enabled(0));
    hg.set_spawn_enabled(old_st, false);
    // The upgrade object is built (a content of the station): the ability is scheduled at Get_Int(now, now + 2).
    ta::AbilityHost ah;
    ObjectId container_of_upgrade = old_st;
    ah.upgrade_container = [&](ObjectId) { return container_of_upgrade; };
    ab.set_host(ah);
    ab.clear_events();
    const ObjectId upg = next++;
    SimRng m = rng;
    CHECK(ab.add_container(upg, du, 0, {}, 710, &rng));
    const uint32_t due = static_cast<uint32_t>(m.range_int(710, 712));
    CHECK(rng == m && ab.spawn_requests().empty());
    ObjectId new_st = kNoObject;
    uint32_t requests = 0, removals = 0, swap_frame = 0;
    for (uint32_t f = 710; f < 720; ++f) {
        ab.before_units(f, rng);
        const std::vector<ta::SpawnRequest> reqs(ab.spawn_requests().begin(), ab.spawn_requests().end());
        const std::vector<ta::AbilityEvent> evs(ab.events().begin(), ab.events().end());
        for (const ta::SpawnRequest& q : reqs) {
            if (q.purpose != ta::SpawnPurpose::StationUpgrade) continue;
            ++requests;
            swap_frame = f;
            CHECK(q.source == old_st && q.target == upg && q.owner == 0 && near3(q.position, Vec3{100, 200, 0}));
            // Host: S' (fresh object, its own first-due draw), steps 4 / 9 / 10, then S removed silently.
            new_st = spawn_unit(d2, q.owner, q.position);
            CHECK(hg.add_carrier(new_st, 1, f, &rng));
            CHECK(complete_station_upgrade(sys, &hg, old_st, new_st, f));
            ab.remove(old_st);
        }
        for (const ta::AbilityEvent& e : evs)
            if (e.kind == ta::AbilityEventKind::RemoveSilently) {
                CHECK(e.id == upg && requests == 1);  // after the request
                ++removals;
                ab.remove(upg);
            }
        ab.clear_events();
    }
    CHECK(requests == 1 && removals == 1 && swap_frame == due);
    CHECK(!sys.has_unit(old_st) && !hg.has_carrier(old_st) && hg.has_carrier(new_st));
    // hardpoints: [0] bay destroyed -> disabled at 0.1; [2] special weapon destroyed -> disabled, flag stays 0
    CHECK(sys.hardpoint_disabled(new_st, 0) && near(sys.hardpoint_health(new_st, 0), 0.1f) && !sys.hardpoint_destroyed(new_st, 0));
    CHECK(sys.hardpoint_disabled(new_st, 2) && !sys.special_weapon_hp_enabled(0) && sys.special_weapon_hp_enabled(1));
    CHECK(!sys.hardpoint_disabled(new_st, 1) && !sys.hardpoint_disabled(new_st, 3));
    // +0x3CB copied (false); hull full (not carried); the old squadrons fly on, uncounted.
    CHECK(!hg.spawn_enabled(new_st) && near(sys.health(new_st), sys.max_health(new_st)));
    for (const auto& [c, s] : launched) CHECK(sys.alive(s) && hg.carrier_of(s) == kNoObject);
    hg.set_spawn_enabled(new_st, true);
    // A full new complement (3) from the disabled bay, one per 10 s.
    const size_t before = launched.size();
    for (uint32_t f = 720; f < 1800; ++f) host(f);
    CHECK(launched.size() - before == 3 && hg.live(new_st, 0) == 3 && hg.bay_count(new_st) == 1);
    // An upgrade object without a station: no request, still removed (one-shot).
    container_of_upgrade = kNoObject;
    const ObjectId upg2 = next++;
    CHECK(ab.add_container(upg2, du, 1, {}, 1800, &rng));
    requests = removals = 0;
    for (uint32_t f = 1800; f < 1810; ++f) {
        ab.before_units(f, rng);
        for (const ta::SpawnRequest& q : ab.spawn_requests()) requests += q.purpose == ta::SpawnPurpose::StationUpgrade ? 1u : 0u;
        for (const ta::AbilityEvent& e : ab.events()) removals += e.kind == ta::AbilityEventKind::RemoveSilently ? 1u : 0u;
        ab.clear_events();
    }
    CHECK(requests == 0 && removals == 1);
    // Not in skirmish (campaign): Skirmish_Automatic is never scheduled.
    ta::AbilityConfig camp;
    camp.skirmish = false;
    ta::AbilitySystem ab2(sys, ad, camp);
    CHECK(ab2.add_container(next++, du, 0, {}, 1900, &rng));
    for (uint32_t f = 1900; f < 1910; ++f) ab2.before_units(f, rng);
    CHECK(ab2.spawn_requests().empty() && ab2.events().empty());
    sys.set_hooks(&ab);
}

// Contents lists and tactical production on their own.
void test_contents_production() {
    ContentsSystem cs;
    CHECK(cs.add(1, 10) && cs.add(1, 11) && cs.add(2, 20) && !cs.add(3, 10) && !cs.add(4, 4) && !cs.add(kNoObject, 5));
    CHECK(cs.container_of(11) == 1 && cs.container_of(99) == kNoObject && cs.contents(1).size() == 2);
    CHECK(cs.move_all(1, 5) == 2 && cs.contents(1).empty() && cs.contents(5).size() == 2);
    if (cs.contents(5).size() == 2) CHECK(cs.contents(5)[0] == 10 && cs.contents(5)[1] == 11);  // list order kept
    CHECK(cs.remove(10) && !cs.remove(10) && cs.container_of(11) == 5);
    const std::vector<ObjectId> left = cs.remove_container(5);
    CHECK(left.size() == 1 && left[0] == 11 && cs.container_of(11) == kNoObject);
    StateHasher h1, h2;
    cs.hash(h1);
    ContentsSystem cs2;
    cs2.add(2, 20);
    cs2.hash(h2);
    CHECK(h1.digest() == h2.digest());

    TacticalProduction tp(30);
    const uint32_t a = tp.enqueue(0, 1, 7, 1.0f, 0);  // 30 frames
    const uint32_t b = tp.enqueue(0, 1, 8, 0.5f, 0);  // 15 frames, after a (same builder)
    const uint32_t c = tp.enqueue(1, 1, 9, 0.2f, 0);  // 6 frames, player 1's own list
    const uint32_t d = tp.enqueue(0, 3, 9, 0.1f, 0);  // 3 frames, another builder of player 0: runs in parallel
    CHECK(a && b && c && d && tp.count_type(0, 7) == 1 && tp.count_type(0, 9) == 1 && tp.queue(0).size() == 3);
    std::vector<std::pair<uint32_t, uint32_t>> done;  // (order, frame)
    for (uint32_t f = 1; f <= 60; ++f) {
        if (f == 10) CHECK(tp.retarget(0, 1, 2) == 2 && tp.retarget(1, 1, 2) == 0);  // progress kept
        tp.update(f);
        for (const BuildOrder& o : tp.completed()) done.emplace_back(o.id, f);
        tp.clear_completed();
    }
    CHECK(done.size() == 4);
    if (done.size() == 4) {
        CHECK(done[0].first == d && done[0].second == 3);
        CHECK(done[1].first == c && done[1].second == 6);
        CHECK(done[2].first == a && done[2].second == 30);
        CHECK(done[3].first == b && done[3].second == 45);
    }
    const uint32_t e = tp.enqueue(0, 2, 7, 1.0f, 60);
    CHECK(tp.cancel(e) && !tp.cancel(e));
    tp.enqueue(0, 2, 7, 1.0f, 60);
    tp.enqueue(1, 2, 8, 1.0f, 60);
    const std::vector<BuildOrder> gone = tp.builder_removed(2);
    CHECK(gone.size() == 2 && gone[0].id < gone[1].id && tp.queue(0).empty() && tp.queue(1).empty());
}

// Real-play path of the skirmish upgrade (§1.1): the station builds the upgrade object (tactical production time), the
// object becomes a content of the station, its ability fires 0-2 frames later, the host swaps the station, moves the
// contents and retargets the queue. Also the team lookup (AbilitySystem::container_of).
void test_upgrade_real_play() {
    CombatData cd;
    ta::AbilityData ad;
    HardpointDesc bay;
    bay.kind = HardpointKind::FighterBay;
    bay.health = 100.0f;
    bay.destroyable = bay.targetable = true;
    cd.hardpoints = {bay};
    auto push = [&](UnitDesc u, std::vector<uint32_t> specials) {
        ta::AbilityObjectDesc a;
        a.name = u.name;
        a.specials = std::move(specials);
        cd.units.push_back(std::move(u));
        ad.objects.push_back(std::move(a));
        return static_cast<uint32_t>(cd.units.size() - 1);
    };
    UnitDesc st;
    st.name = "Station_1";
    st.tactical_health = 4000.0f;
    st.repairs_hardpoints = true;
    st.has_locomotor = false;
    st.hardpoints = {0};
    UnitDesc st2 = st;
    st2.name = "Station_2";
    UnitDesc fighter;
    fighter.name = "Fighter";
    fighter.tactical_health = 10.0f;
    UnitDesc dummy;
    dummy.name = "Upgrade";
    UnitDesc plain;
    plain.name = "Improved_Weapons";
    ta::SpecialAbilityDesc up;
    up.name = "Upgrade_Effect";
    up.class_tag = "Starbase_Upgrade_Ability";
    up.cls = ta::special_class_from_tag(up.class_tag);
    up.style = ta::ActivationStyle::SkirmishAutomatic;
    ad.specials.push_back(up);
    const uint32_t d1 = push(st, {}), d2 = push(st2, {}), df = push(fighter, {}), du = push(dummy, {0}), dp = push(plain, {});
    constexpr int32_t kOtherType = 77;  // a unit order queued behind the upgrade at the same station
    CombatConfig cfg;
    cfg.object_max_health_multiplier = 1.0f;
    cfg.hardpoint_health_multiplier = 1.0f;
    CombatSystem sys(cd, cfg, {PlayerInfo{}, PlayerInfo{}});
    ta::AbilityConfig acfg;
    acfg.skirmish = true;
    ta::AbilitySystem ab(sys, ad, acfg);
    SimRng rng{11u};
    sys.bind_rng(&rng);
    ab.bind_rng(&rng);
    ContentsSystem cs;
    TacticalProduction tp(30);
    ta::AbilityHost ah;
    ah.upgrade_container = [&](ObjectId u) { return cs.container_of(u); };
    ab.set_host(ah);
    ObjectId next = 10;
    auto spawn_unit = [&](uint32_t desc, int32_t owner, ObjectId container = kNoObject) {
        UnitSpawn s;
        s.id = next++;
        s.desc = desc;
        s.owner = owner;
        CHECK(ab.add_unit(s, desc, 1, rng, container));
        return s.id;
    };
    // Team lookup: two fighters and their squadron container.
    const ObjectId f1 = spawn_unit(df, 0), f2 = spawn_unit(df, 0);
    const ObjectId sq = next++;
    const std::vector<ObjectId> mem = {f1, f2};
    CHECK(ab.add_container(sq, dp, 0, mem, 1, &rng));
    CHECK(ab.container_of(f1) == sq && ab.container_of(f2) == sq && ab.container_of(sq) == kNoObject);
    CHECK(ab.members_of(sq).size() == 2 && ab.members_of(f1).empty() && ab.container_of(9999) == kNoObject);
    // Station with an earlier upgrade dummy in its contents.
    const ObjectId station = spawn_unit(d1, 0);
    const ObjectId earlier = next++;
    CHECK(ab.add_container(earlier, dp, 0, {}, 1, &rng) && cs.add(station, earlier));
    // Buy the upgrade (2 s) and then another unit (1 s) at the station.
    CHECK(tp.enqueue(0, station, static_cast<int32_t>(du), 2.0f, 0) != 0);
    CHECK(tp.enqueue(0, station, kOtherType, 1.0f, 0) != 0);
    ObjectId upg = kNoObject, s2 = kNoObject;
    uint32_t built = 0, swapped = 0, removed = 0, other_done = 0;
    ObjectId other_builder = kNoObject;
    for (uint32_t f = 1; f <= 150; ++f) {
        tp.update(f);
        for (const BuildOrder& o : tp.completed()) {
            if (o.type == static_cast<int32_t>(du)) {
                upg = next++;
                CHECK(cs.add(o.builder, upg) && ab.add_container(upg, du, o.player, {}, f, &rng));
                built = f;
            } else {
                other_done = f;
                other_builder = o.builder;
            }
        }
        tp.clear_completed();
        ab.before_units(f, rng);
        const std::vector<ta::SpawnRequest> reqs(ab.spawn_requests().begin(), ab.spawn_requests().end());
        const std::vector<ta::AbilityEvent> evs(ab.events().begin(), ab.events().end());
        for (const ta::SpawnRequest& q : reqs) {
            if (q.purpose != ta::SpawnPurpose::StationUpgrade) continue;
            CHECK(q.source == station && q.target == upg);
            s2 = spawn_unit(d2, q.owner);
            CHECK(complete_station_upgrade(sys, nullptr, station, s2, f));
            CHECK(cs.move_all(station, s2) == 2);  // the earlier dummy, then the running upgrade object
            CHECK(tp.retarget(0, station, s2) == 1);
            CHECK(cs.remove_container(station).empty());
            ab.remove(station);
            swapped = f;
        }
        for (const ta::AbilityEvent& e : evs)
            if (e.kind == ta::AbilityEventKind::RemoveSilently && e.id == upg) {
                CHECK(cs.container_of(upg) == s2);
                cs.remove(upg);
                ab.remove(upg);
                ++removed;
            }
        ab.clear_events();
    }
    CHECK(built == 60 && swapped >= 60 && swapped <= 62 && removed == 1);
    CHECK(s2 != kNoObject && cs.contents(s2).size() == 1 && cs.container_of(earlier) == s2);
    CHECK(other_done == 90 && other_builder == s2);  // queued behind the upgrade, finished at the new station
    (void)d2;
}

// ================================================================================================
// FoC data (ALAMO_FOC_DATA)
// ================================================================================================
#ifdef ALAMO_HANGAR_TESTS_HAVE_DATA
struct HpData {
    std::string name;
    HardpointKind kind = HardpointKind::DummyArt;
    float health = 0.0f, flyout = 0.0f;
    bool destroyable = false;
};

HardpointKind kind_of(std::string_view t) {
    struct K {
        const char* n;
        HardpointKind k;
    };
    static const K ks[] = {{"HARD_POINT_ENGINE", HardpointKind::Engine},         {"HARD_POINT_SHIELD_GENERATOR", HardpointKind::ShieldGenerator},
                           {"HARD_POINT_GRAVITY_WELL", HardpointKind::GravityWell}, {"HARD_POINT_FIGHTER_BAY", HardpointKind::FighterBay},
                           {"HARD_POINT_TRACTOR_BEAM", HardpointKind::TractorBeam}, {"HARD_POINT_WEAPON_LASER", HardpointKind::WeaponLaser},
                           {"HARD_POINT_WEAPON_MISSILE", HardpointKind::WeaponMissile}, {"HARD_POINT_WEAPON_TORPEDO", HardpointKind::WeaponTorpedo},
                           {"HARD_POINT_WEAPON_ION_CANNON", HardpointKind::WeaponIonCannon},
                           {"HARD_POINT_WEAPON_MASS_DRIVER", HardpointKind::WeaponMassDriver},
                           {"HARD_POINT_WEAPON_SPECIAL", HardpointKind::WeaponSpecial},
                           {"HARD_POINT_ENABLE_SPECIAL_ABILITY", HardpointKind::EnableSpecialAbility}};
    for (const K& k : ks)
        if (t == k.n) return k.k;
    return HardpointKind::DummyArt;
}

float to_f(std::string_view s) { return s.empty() ? 0.0f : std::strtof(std::string(s).c_str(), nullptr); }
bool to_b(std::string_view s) { return !s.empty() && (s[0] == 'y' || s[0] == 'Y' || s[0] == 't' || s[0] == 'T' || s[0] == '1'); }

void test_foc_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("foc data: skipped (ALAMO_FOC_DATA unset)\n");
        return;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string err;
    const std::unique_ptr<vfs::Vfs> v = vfs::Vfs::create(vcfg, &err);
    if (!v) {
        std::printf("FAIL foc data: VFS: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    objects::ObjectTypeDatabase db;
    std::vector<std::string> diag;
    CHECK(db.load(*v, &diag));
    objects::GameObjectTypes types;
    CHECK(types.build(db, v.get(), &diag));
    // Hardpoint records (last definition of a name wins).
    std::vector<HpData> hps;
    for (const std::string& p : v->list("Data\\XML\\*.XML")) {
        std::vector<uint8_t> bytes;
        if (!v->read(p, bytes)) continue;
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, p);
        if (!doc.ok()) continue;
        for (data::XmlNode e = doc.root().first_child(); e; e = e.next_sibling()) {
            if (e.name() != "HardPoint") continue;
            HpData h;
            h.name = std::string(e.attribute("Name"));
            h.kind = kind_of(e.child_text("Type"));
            h.health = to_f(e.child_text("Health"));
            h.destroyable = to_b(e.child_text("Is_Destroyable"));
            h.flyout = to_f(e.child_text("Fighter_Bay_Flyout_Distance"));
            bool found = false;
            for (HpData& o : hps)
                if (o.name == h.name) {
                    o = h;
                    found = true;
                }
            if (!found) hps.push_back(h);
        }
    }
    CombatData cd;
    ta::AbilityData ad;
    std::vector<objects::TypeIndex> unit_type;  // UnitDesc index -> TypeIndex
    auto unit_of = [&](objects::TypeIndex t) -> int32_t {
        for (size_t i = 0; i < unit_type.size(); ++i)
            if (unit_type[i] == t) return static_cast<int32_t>(i);
        UnitDesc u;
        u.name = std::string(db.types().name[t]);
        u.tactical_health = types.get_float(t, "Tactical_Health");
        if (!(u.tactical_health > 0.0f)) u.tactical_health = 100.0f;
        u.repairs_hardpoints = types.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("DUMMY_STAR_BASE"));
        u.has_locomotor = !u.repairs_hardpoints;  // [test] stations do not move; ships do
        for (const uint32_t hi : types.hardpoints(t)) {
            const std::string_view hn = types.hardpoint_names()[hi];
            HardpointDesc hd;
            hd.name = std::string(hn);
            for (const HpData& h : hps)
                if (h.name == hn) {
                    hd.kind = h.kind;
                    hd.health = h.health > 0.0f ? h.health : 100.0f;
                    hd.destroyable = h.destroyable;
                    hd.targetable = true;
                    hd.fighter_bay_flyout_distance = h.flyout;
                }
            cd.hardpoints.push_back(hd);
            u.hardpoints.push_back(static_cast<uint32_t>(cd.hardpoints.size() - 1));
        }
        cd.units.push_back(u);
        ta::AbilityObjectDesc a;
        a.name = u.name;
        ad.objects.push_back(a);
        unit_type.push_back(t);
        return static_cast<int32_t>(cd.units.size() - 1);
    };
    auto hangar_of = [&](objects::TypeIndex t) {
        HangarDesc d;
        const auto resolve = [&](std::string_view n) -> int32_t {
            const objects::TypeIndex x = types.resolve_name(n);
            return x == objects::kNone ? -1 : unit_of(x);
        };
        for (uint32_t n = 0; n < kTechSlots; ++n) {
            const std::string sn = "Starting_Spawned_Units_Tech_" + std::to_string(n);
            const std::string rn = "Reserve_Spawned_Units_Tech_" + std::to_string(n);
            d.starting[n] = parse_spawn_list(types.get_list(t, sn), resolve);
            d.reserve[n] = parse_spawn_list(types.get_list(t, rn), resolve);
        }
        d.delay_seconds = types.get_float(t, "Spawned_Squadron_Delay_Seconds");
        d.name = std::string(db.types().name[t]);
        d.station = types.has_behavior(t, objects::BehaviorSet::Union, objects::behavior_type_id("DUMMY_STAR_BASE"));
        return d;
    };
    const objects::TypeIndex es1 = types.resolve_name("Empire_Star_Base_1");
    const objects::TypeIndex isd = types.resolve_name("Star_Destroyer");
    CHECK(es1 != objects::kNone && isd != objects::kNone);
    if (es1 == objects::kNone || isd == objects::kNone) return;
    const objects::TypeIndex es2 = types.get_type_ref(es1, "Next_Level_Base");
    CHECK(es2 != objects::kNone && db.types().name[es2] == "Empire_Star_Base_2");
    if (es2 == objects::kNone) return;
    const int32_t u_es1 = unit_of(es1), u_es2 = unit_of(es2), u_isd = unit_of(isd);
    std::vector<HangarDesc> hd = {hangar_of(es1), hangar_of(es2), hangar_of(isd)};
    for (const HangarDesc& d : hd) {
        std::printf("foc data: %s delay %.1f tech0 starting", d.name.c_str(), static_cast<double>(d.delay_seconds));
        for (const SpawnEntry& e : d.starting[0]) std::printf(" (%d x%d)", e.type, e.count);
        std::printf(" reserve");
        for (const SpawnEntry& e : d.reserve[0]) std::printf(" (%d x%d)", e.type, e.count);
        std::printf("\n");
    }
    // Vanilla: Empire_Star_Base_1 two squadron types, unlimited reserve, 10 s; Star_Destroyer 2 interceptors + 1 bomber,
    // 5 s (inherited from Generic_Star_Destroyer), an interceptor reserve of 8.
    CHECK(hd[0].starting[0].size() == 2 && hd[0].delay_seconds == 10.0f);
    CHECK(hd[2].starting[0].size() == 2 && hd[2].starting[0][0].count == 2 && hd[2].starting[0][1].count == 1);
    CHECK(!hd[2].reserve[0].empty() && hd[2].reserve[0][0].type == hd[2].starting[0][0].type && hd[2].reserve[0][0].count == 8);
    CHECK(hd[2].delay_seconds == 5.0f);
    for (const HangarDesc& d : hd)
        for (const SpawnEntry& e : d.starting[0]) CHECK(e.type >= 0);
    uint32_t isd_bays = 0;
    for (const uint32_t h : cd.units[static_cast<size_t>(u_isd)].hardpoints)
        isd_bays += cd.hardpoints[h].kind == HardpointKind::FighterBay ? 1u : 0u;
    CHECK(isd_bays == 1);
    uint32_t es1_bays = 0, es2_bays = 0;
    for (const uint32_t h : cd.units[static_cast<size_t>(u_es1)].hardpoints)
        es1_bays += cd.hardpoints[h].kind == HardpointKind::FighterBay ? 1u : 0u;
    for (const uint32_t h : cd.units[static_cast<size_t>(u_es2)].hardpoints)
        es2_bays += cd.hardpoints[h].kind == HardpointKind::FighterBay ? 1u : 0u;
    std::printf("foc data: fighter bays: Empire_Star_Base_1 %u, Empire_Star_Base_2 %u, Star_Destroyer %u\n", es1_bays, es2_bays, isd_bays);
    CHECK(es1_bays >= 1 && es2_bays >= 1);
    // Upgrade object with the shipped Starbase_Upgrade_Ability.
    {
        bool have = false;
        for (const std::string& p : v->list("Data\\XML\\UPGRADEOBJECTS.XML")) {
            std::vector<uint8_t> bytes;
            if (!v->read(p, bytes)) continue;
            const data::XmlDocument doc = data::XmlDocument::parse(bytes, p);
            for (data::XmlNode e = doc.root().first_child(); e && !have; e = e.next_sibling()) {
                if (e.attribute("Name") != "ES_Level_Two_Starbase_Upgrade") continue;
                ta::AbilityResolvers r;
                const std::vector<uint32_t> sp = ta::parse_special_abilities(e, ad, r);
                ta::AbilityObjectDesc a;
                a.name = "ES_Level_Two_Starbase_Upgrade";
                a.specials = sp;
                cd.units.push_back(UnitDesc{});
                ad.objects.push_back(a);
                have = sp.size() == 1 && ad.specials[sp[0]].cls == ta::SpecialClass::StarbaseUpgrade &&
                       ad.specials[sp[0]].style == ta::ActivationStyle::SkirmishAutomatic;
            }
        }
        CHECK(have);
        if (!have) return;
    }
    const auto u_up = static_cast<uint32_t>(cd.units.size() - 1);
    CombatConfig cfg;
    CombatSystem sys(cd, cfg, {PlayerInfo{}, PlayerInfo{}});
    ta::AbilityConfig acfg;
    acfg.skirmish = true;
    ta::AbilitySystem ab(sys, ad, acfg);
    SimRng rng{2026u};
    sys.bind_rng(&rng);
    ab.bind_rng(&rng);
    HangarSystem hg(sys, hd);
    hg.set_abilities(&ab);
    ObjectId next = 1;
    struct Launch {
        ObjectId carrier, id;
        int32_t type;
        uint32_t frame;
    };
    std::vector<Launch> launches;
    auto spawn = [&](uint32_t desc, int32_t owner, Vec3 p, Vec3 f) {
        UnitSpawn s;
        s.id = next++;
        s.desc = desc;
        s.owner = owner;
        s.position = p;
        s.facing = f;
        CHECK(ab.add_unit(s, desc, 1, rng));
        return s.id;
    };
    auto host = [&](uint32_t frame) {
        hg.update(frame, rng);
        for (const LaunchRequest& q : hg.launches()) {
            const ObjectId id = spawn(static_cast<uint32_t>(q.type), q.owner, q.origin, q.facing);
            hg.bind_launch(q.serial, id);
            launches.push_back(Launch{q.carrier, id, q.type, frame});
        }
        hg.clear_requests();
    };
    auto count = [&](ObjectId c) {
        size_t n = 0;
        for (const Launch& l : launches) n += l.carrier == c ? 1u : 0u;
        return n;
    };
    // Empire station and a Star Destroyer (player 0): one launch per service, the first 0-29 frames after creation.
    const ObjectId station = spawn(static_cast<uint32_t>(u_es1), 0, Vec3{0, 0, 0}, Vec3{1, 0, 0});
    const ObjectId sd = spawn(static_cast<uint32_t>(u_isd), 0, Vec3{2000, 0, 0}, Vec3{0, 1, 0});
    CHECK(hg.add_carrier(station, 0, 1, &rng) && hg.add_carrier(sd, 2, 1, &rng));
    const uint32_t sd_due = hg.next_service_frame(sd);
    for (uint32_t f = 1; f < 31; ++f) host(f);
    CHECK(count(station) == 1 && count(sd) == 1);
    // Star Destroyer: 3 launches 150 frames apart (2 interceptors + 1 bomber), each escorting the carrier.
    for (uint32_t f = 31; f < 600; ++f) host(f);
    CHECK(count(sd) == 3);
    std::vector<Launch> sdl;
    for (const Launch& l : launches)
        if (l.carrier == sd) sdl.push_back(l);
    if (sdl.size() == 3) CHECK(sdl[0].frame == sd_due && sdl[1].frame == sd_due + 150 && sdl[2].frame == sd_due + 300);
    const int32_t interceptor = hd[2].starting[0][0].type;
    CHECK(hg.live(sd, 0) == 2 && hg.live(sd, 1) == 1 && hg.remaining(sd, 0) == 2 + 8 - 2);
    // Lose an interceptor squadron: replaced from the reserve after 5 s, at a service frame.
    ObjectId lost = kNoObject;
    for (const Launch& l : sdl)
        if (l.type == interceptor && lost == kNoObject) lost = l.id;
    sys.kill_unit(lost, kNoObject, kKindNormal, 600);
    hg.squadron_removed(lost, 600);
    CHECK(hg.next_spawn_frame(sd) == 750);
    for (uint32_t f = 600; f < 900; ++f) host(f);
    CHECK(count(sd) == 4 && launches.back().type == interceptor && hg.remaining(sd, 0) == 7 && hg.live(sd, 0) == 2);
    for (const Launch& l : launches)
        if (l.carrier == sd && l.frame >= 600) CHECK(l.frame >= 750 && (l.frame - sd_due) % 30 == 0);
    // The station has launched one per 10 s so far.
    const size_t st_before = count(station);
    std::printf("foc data: Empire_Star_Base_1 launched %zu by frame 900; Star_Destroyer %zu\n", st_before, count(sd));
    CHECK(st_before >= 3 && st_before <= 4);
    // The station is upgraded mid-battle: its first hardpoint is shot down first.
    DamageRequest r;
    r.victim = station;
    r.amount = 1.0e6f;
    r.hardpoint = 0;
    sys.take_damage(r, 900);
    ta::AbilityHost ah;
    ah.upgrade_container = [&](ObjectId) { return station; };
    ab.set_host(ah);
    ab.clear_events();
    const ObjectId upg = next++;
    CHECK(ab.add_container(upg, u_up, 0, {}, 901, &rng));
    ObjectId st2 = kNoObject;
    for (uint32_t f = 901; f < 905; ++f) {
        ab.before_units(f, rng);
        const std::vector<ta::SpawnRequest> reqs(ab.spawn_requests().begin(), ab.spawn_requests().end());
        const std::vector<ta::AbilityEvent> evs(ab.events().begin(), ab.events().end());
        for (const ta::SpawnRequest& q : reqs) {
            if (q.purpose != ta::SpawnPurpose::StationUpgrade) continue;
            CHECK(q.source == station && f <= 903);
            st2 = spawn(static_cast<uint32_t>(u_es2), q.owner, q.position, q.facing);
            CHECK(hg.add_carrier(st2, 1, f, &rng));
            CHECK(complete_station_upgrade(sys, &hg, station, st2, f));
            ab.remove(station);
        }
        for (const ta::AbilityEvent& e : evs)
            if (e.kind == ta::AbilityEventKind::RemoveSilently && e.id == upg) ab.remove(upg);
        ab.clear_events();
        host(f);
    }
    CHECK(st2 != kNoObject && !ab.has_object(upg));
    if (st2 == kNoObject) return;
    CHECK(sys.hardpoint_disabled(st2, 0) && !sys.hardpoint_disabled(st2, 1));
    CHECK(hg.has_carrier(st2) && !hg.has_carrier(station));
    // A full new complement (its starting counts), one per delay; the old station's squadrons are not counted.
    int32_t cap = 0;
    for (const SpawnEntry& e : hd[1].starting[0]) cap += e.count;
    for (uint32_t f = 905; f < 905 + static_cast<uint32_t>(cap + 2) * 300u; ++f) host(f);
    std::printf("foc data: Empire_Star_Base_2 complement %d, launched after the upgrade %zu\n", cap, count(st2));
    CHECK(static_cast<int32_t>(count(st2)) == cap);
    // Carrier death: its squadrons stay.
    sys.kill_unit(sd, kNoObject, kKindNormal, 5000);
    host(5000);
    CHECK(!hg.has_carrier(sd));
    for (const Launch& l : launches)
        if (l.carrier == sd && l.id != lost) CHECK(sys.alive(l.id));
}
#endif

// ---- N1 stage 2: save / load mid-battle (HangarSystem, ContentsSystem, TacticalProduction, EngineLoops write / read) ----

template <class S>
std::vector<uint8_t> bytes_of(const S& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}

// The scripted_digest battle (two carriers, squadrons lost every 97 frames), with World::step split at the host step so
// a save can hold pending launch requests.
void hangar_scene(World& w) {
    w.make({two_entry_desc(), one_entry_desc(3, 4, 2.0f)});
    w.add(1, w.carrier, 0, Vec3{0, 0, 0});
    w.add(2, w.carrier, 1, Vec3{3000, 0, 0});
    w.hg->add_carrier(2, 1, 7, &w.hrng);
    w.hg->add_carrier(1, 0, 1, &w.hrng);
}

void hangar_update(World& w, uint32_t f) {
    if (f % 97 == 0 && !w.log_id.empty()) {
        const ObjectId id = w.log_id[f % w.log_id.size()];
        if (w.sys->alive(id)) w.lose(id, f);
    }
    w.hg->update(f, w.hrng);
}

void hangar_host(World& w, uint32_t f) {
    for (const LaunchRequest& q : w.hg->launches()) {
        w.log.push_back(q);
        w.log_frame.push_back(f);
        const ObjectId id = w.next_id++;
        w.add(id, w.fighter, q.owner, q.origin, q.facing);
        CHECK(w.hg->bind_launch(q.serial, id));
        w.log_id.push_back(id);
    }
    for (const HangarEvent& e : w.hg->events()) w.ev.push_back(e);
    w.hg->clear_requests();
}

uint64_t hangar_digest(const World& w) {
    StateHasher h;
    w.hg->hash(h);
    w.sys->hash_units(h);
    w.rng.hash(h);
    w.hrng.hash(h);
    return h.digest();
}

void test_hangar_save() {
    World a;
    hangar_scene(a);
    uint32_t f = 1;
    for (; f < 1500; ++f) {
        hangar_update(a, f);
        if (f >= 300 && !a.hg->launches().empty()) break;  // a carrier launching: requests not bound yet
        hangar_host(a, f);
    }
    CHECK(f < 1500 && !a.hg->launches().empty() && !a.log_id.empty());
    const uint32_t save_frame = f;
    const size_t launched_before = a.log.size(), pending_at_save = a.hg->launches().size();
    const std::vector<uint8_t> hg_bytes = bytes_of(*a.hg), combat_bytes = bytes_of(*a.sys);
    const std::vector<uint8_t> rng_bytes = bytes_of(a.rng), hrng_bytes = bytes_of(a.hrng);

    World b;
    b.make({two_entry_desc(), one_entry_desc(3, 4, 2.0f)});  // definitions only; carriers come from the save
    ByteReader r1(combat_bytes), r2(hg_bytes), r3(rng_bytes), r4(hrng_bytes);
    CHECK(b.sys->read(r1) && b.hg->read(r2) && b.rng.read(r3) && b.hrng.read(r4));
    b.log = a.log;  // the host's own state
    b.log_frame = a.log_frame;
    b.log_id = a.log_id;
    b.ev = a.ev;
    b.next_id = a.next_id;
    CHECK(bytes_of(*b.hg) == hg_bytes && bytes_of(*b.sys) == combat_bytes);  // write -> read -> write
    CHECK(hangar_digest(a) == hangar_digest(b));
    CHECK(b.hg->launches().size() == a.hg->launches().size());
    hangar_host(a, save_frame);
    hangar_host(b, save_frame);
    uint32_t diverged = 0;
    for (uint32_t g = save_frame + 1; g < 1500; ++g) {
        hangar_update(a, g);
        hangar_update(b, g);
        bool same = a.hg->launches().size() == b.hg->launches().size();
        for (size_t i = 0; same && i < a.hg->launches().size(); ++i) {
            const LaunchRequest& x = a.hg->launches()[i];
            const LaunchRequest& y = b.hg->launches()[i];
            same = x.serial == y.serial && x.carrier == y.carrier && x.type == y.type && x.bay == y.bay &&
                   x.origin.x == y.origin.x && x.origin.y == y.origin.y && x.flyout.x == y.flyout.x;
        }
        hangar_host(a, g);
        hangar_host(b, g);
        if (!same || hangar_digest(a) != hangar_digest(b)) ++diverged;
    }
    CHECK(diverged == 0 && a.log.size() == b.log.size() && a.ev.size() == b.ev.size() && a.log.size() > launched_before);
    CHECK(bytes_of(*a.hg) == bytes_of(*b.hg) && bytes_of(*a.sys) == bytes_of(*b.sys));

    // Rejected data leaves the system unchanged: truncation, trailing bytes, version, descriptor count, carrier desc.
    World c;
    c.make({two_entry_desc(), one_entry_desc(3, 4, 2.0f)});
    const std::vector<uint8_t> before = bytes_of(*c.hg);
    const auto rejected = [&c](const std::vector<uint8_t>& v) {
        ByteReader r(v);
        return !c.hg->read(r);
    };
    uint32_t accepted = 0;
    for (size_t len = 0; len < hg_bytes.size(); ++len)
        if (!rejected(std::vector<uint8_t>(hg_bytes.begin(), hg_bytes.begin() + static_cast<std::ptrdiff_t>(len)))) ++accepted;
    CHECK(accepted == 0);
    std::vector<uint8_t> bad = hg_bytes;
    bad.push_back(0);
    CHECK(rejected(bad));
    bad = hg_bytes;
    bad[0] = 2;
    CHECK(rejected(bad));
    bad = hg_bytes;
    bad[4] = 3;  // descriptor count 3, the battle has 2
    CHECK(rejected(bad));
    bad = hg_bytes;
    bad[20] = 7;  // the first carrier's descriptor (version, count, serial, carrier count, id)
    CHECK(rejected(bad));
    CHECK(bytes_of(*c.hg) == before);
    World d;
    d.make({two_entry_desc()});
    ByteReader rd(hg_bytes);
    CHECK(!d.hg->read(rd));
    ByteReader rc(hg_bytes);
    CHECK(c.hg->read(rc) && bytes_of(*c.hg) == hg_bytes);
    std::printf("hangar save: %zu bytes at frame %u (%zu launch requests pending), continued to 1500 identically "
                "(%zu launches after the save)\n",
                hg_bytes.size(), save_frame, pending_at_save, a.log.size() - launched_before);
}

void test_contents_production_loops_save() {
    // ContentsSystem: lists (ascending container) and list order survive; the loaded copy continues identically.
    ContentsSystem cs;
    CHECK(cs.add(1, 10) && cs.add(1, 11) && cs.add(2, 20) && cs.add(7, 70));
    const std::vector<uint8_t> cb = bytes_of(cs);
    ContentsSystem cs2;
    ByteReader r(cb);
    CHECK(cs2.read(r) && bytes_of(cs2) == cb && cs2.container_of(11) == 1 && cs2.contents(1).size() == 2);
    StateHasher h1, h2;
    cs.hash(h1);
    cs2.hash(h2);
    CHECK(h1.digest() == h2.digest());
    CHECK(cs.move_all(1, 2) == 2 && cs2.move_all(1, 2) == 2 && bytes_of(cs) == bytes_of(cs2));
    ContentsSystem cs3;
    cs3.add(5, 50);
    const std::vector<uint8_t> cs3_before = bytes_of(cs3);
    uint32_t accepted = 0;
    for (size_t len = 0; len < cb.size(); ++len) {
        ByteReader t(std::span<const uint8_t>(cb.data(), len));
        if (cs3.read(t)) ++accepted;
    }
    std::vector<uint8_t> bad = cb;
    bad[8] = 2;  // the first container 2: lists 2, 2, 7 are not ascending
    ByteReader rb(bad);
    CHECK(accepted == 0 && !cs3.read(rb) && bytes_of(cs3) == cs3_before);

    // TacticalProduction: queues with progress and completed orders not yet cleared.
    TacticalProduction tp(30), tp2(30);
    tp.enqueue(0, 1, 7, 1.0f, 0);
    tp.enqueue(0, 1, 8, 0.5f, 0);
    tp.enqueue(1, 1, 9, 0.2f, 0);
    tp.enqueue(0, 3, 9, 0.1f, 0);
    for (uint32_t f = 1; f <= 10; ++f) tp.update(f);
    CHECK(tp.completed().size() == 2);
    const std::vector<uint8_t> pb = bytes_of(tp);
    ByteReader rp(pb);
    CHECK(tp2.read(rp) && bytes_of(tp2) == pb && tp2.completed().size() == 2 && tp2.count_type(0, 7) == 1);
    uint32_t diverged = 0;
    for (uint32_t f = 11; f <= 60; ++f) {
        if (f == 20) {
            tp.enqueue(1, 4, 5, 0.3f, f);
            tp2.enqueue(1, 4, 5, 0.3f, f);
        }
        tp.clear_completed();
        tp2.clear_completed();
        tp.update(f);
        tp2.update(f);
        bool same = tp.completed().size() == tp2.completed().size();
        for (size_t i = 0; same && i < tp.completed().size(); ++i) same = tp.completed()[i].id == tp2.completed()[i].id;
        StateHasher x, y;
        tp.hash(x);
        tp2.hash(y);
        if (!same || x.digest() != y.digest()) ++diverged;
    }
    CHECK(diverged == 0 && tp.queue(0).empty() && bytes_of(tp) == bytes_of(tp2));
    TacticalProduction tp3(30);
    tp3.enqueue(2, 9, 1, 1.0f, 0);
    const std::vector<uint8_t> tp3_before = bytes_of(tp3);
    accepted = 0;
    for (size_t len = 0; len < pb.size(); ++len) {
        ByteReader t(std::span<const uint8_t>(pb.data(), len));
        if (tp3.read(t)) ++accepted;
    }
    bad = pb;
    bad[0] = 0;
    ByteReader rpb(bad);
    CHECK(accepted == 0 && !tp3.read(rpb) && bytes_of(tp3) == tp3_before);

    // EngineLoops: flags and the pending requests (a loaded battle restarts no loop twice).
    EngineLoops e(10.0f), e2(10.0f);
    e.mode_enter(5, 1);
    e.locomotor_service(5, 20.0f, false, 2);
    e.mode_enter(3, 2);
    e.suppress(3, 3);
    CHECK(!e.requests().empty());
    const std::vector<uint8_t> eb = bytes_of(e);
    ByteReader re(eb);
    CHECK(e2.read(re) && bytes_of(e2) == eb && e2.moving_on(5) && !e2.idle_on(3) && e2.requests().size() == e.requests().size());
    for (EngineLoops* l : {&e, &e2}) {
        l->clear_requests();
        l->locomotor_service(5, 0.0f, false, 4);
        l->unsuppress(3);
        l->locomotor_service(3, 20.0f, false, 5);
    }
    CHECK(bytes_of(e) == bytes_of(e2) && e.moving_on(3) && e2.moving_on(3) && e2.idle_on(5));
    EngineLoops e3(10.0f);
    e3.mode_enter(9, 0);
    const std::vector<uint8_t> e3_before = bytes_of(e3);
    accepted = 0;
    for (size_t len = 0; len < eb.size(); ++len) {
        ByteReader t(std::span<const uint8_t>(eb.data(), len));
        if (e3.read(t)) ++accepted;
    }
    // version, 2 objects (7 bytes each), request count, then the first request's id and loop byte
    bad = eb;
    bad[4 + 4 + 14 + 4 + 4] = 9;
    ByteReader reb(bad);
    CHECK(accepted == 0 && !e3.read(reb) && bytes_of(e3) == e3_before);
}

}  // namespace

int main() {
    test_parse();
    test_launch_sequence();
    test_losses();
    test_gates_and_bays();
    test_deploy_squad();
    test_tech_community_pending();
    test_carrier_death();
    test_engine_loops();
    test_determinism();
    test_station_upgrade_abilities();
    test_contents_production();
    test_hangar_save();  // N1 stage 2
    test_contents_production_loops_save();
    test_upgrade_real_play();
#ifdef ALAMO_HANGAR_TESTS_HAVE_DATA
    test_foc_data();
#else
    std::printf("foc data: skipped (no vfs / objects in this build)\n");
#endif
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all hangar tests passed\n");
    return 0;
}
