// alamo_ai_tactical_readings_tests (no framework; non-zero exit on failure).
// A1 t01 (docs/re/A1_READINGS.md): the settled threat-grid and free-store readings.
//   13  nFow rounds away from zero (ceil), clamped 0..512
//   14  the UnitAI data-pack stamps (two synced draws) and the first re-projection check at the first service,
//       then +300 per service without re-basing (catch up)
//   15  destroyed objects leave every player's memory lists [port]
//   6   free-store admission: land (0x690a10) and the separate space list (0x6904b0)
//   9   FreeStoreService first eligible at F = 600 (then 1200), On_Unit_Service at F = 61
#include <cstdint>
#include <cstdio>
#include <vector>

#include "alamo/ai/tactical/free_store.hpp"
#include "alamo/ai/tactical/perception_world.hpp"
#include "alamo/ai/tactical/threat_grid.hpp"
#include "alamo/core/sim_rng.hpp"

using namespace alamo;
using namespace alamo::ai::tactical;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

// ---------------------------------------------------------------- 13 nFow
static ThreatGridConfig land_cfg() {
    ThreatGridConfig c;
    c.fog_cells_per_threat_cell = 4;
    c.desired_fow_cell_size = 15.0f;
    c.threat_range_cap = 500.0f;
    return c;
}

static void test_fow_rounding() {
    TacticalPerceptionGrid g;
    // 1200 / 15 = 80 exactly: no extra cell
    g.init(TacticalMode::Land, {0.0f, 0.0f, 1200.0f, 1200.0f}, 1, land_cfg());
    CHECK(g.fow_x() == 80 && g.nx() == 20 && g.fow_y() == 80);
    // 1205 / 15 = 80.33: ceil -> 81 (round-to-nearest gave 80) -> 21 threat cells
    g.init(TacticalMode::Land, {0.0f, 0.0f, 1205.0f, 1200.0f}, 1, land_cfg());
    CHECK(g.fow_x() == 81 && g.nx() == 21 && g.fow_y() == 80 && g.ny() == 20);
    CHECK(g.cell_w() == 1205.0f / 21.0f);
    // 1 / 15 = 0.067: ceil -> 1 (round-to-nearest gave 0, an empty grid)
    g.init(TacticalMode::Land, {0.0f, 0.0f, 1.0f, 1.0f}, 1, land_cfg());
    CHECK(g.valid() && g.fow_x() == 1 && g.nx() == 1 && g.ny() == 1);
    // 7680 / 15 = 512 exactly; 7681 / 15 = 512.07 -> 513 -> clamped to 512
    g.init(TacticalMode::Land, {0.0f, 0.0f, 7680.0f, 7681.0f}, 1, land_cfg());
    CHECK(g.fow_x() == 512 && g.fow_y() == 512 && g.nx() == 128 && g.ny() == 128);
    // empty extent: no grid
    g.init(TacticalMode::Land, {0.0f, 0.0f, 0.0f, 100.0f}, 1, land_cfg());
    CHECK(!g.valid() && g.fow_x() == 0);
}

// ---------------------------------------------------------------- world for 14 / 15
// Players: 0 AI, 1 human, enemies. Map 0..1200 (20 x 20 threat cells of 60).
static PerceptionWorld make_world() {
    PerceptionWorld w;
    w.mode = TacticalMode::Land;
    w.map = {0.0f, 0.0f, 1200.0f, 1200.0f};
    for (int i = 0; i < 2; ++i) {
        PerceptionPlayerInfo p;
        p.id = i + 1;
        p.faction = i == 0 ? "Empire" : "Rebel";
        p.is_human = i == 1;
        w.players.push_back(p);
    }
    w.relation = {0, 1, 1, 0};
    PerceptionType t;
    t.name = "Tank";
    t.category = 0x2;
    t.combat_power = 100.0f;
    w.types.push_back(t);
    return w;
}

static PerceptionObject make_unit(int32_t id, int32_t owner, float x, float y) {
    PerceptionObject o;
    o.id = id;
    o.owner = owner;
    o.type = 0;
    o.has_unit_ai = true;
    o.position = {x, y, 0.0f};
    o.transform[3] = x;
    o.transform[7] = y;
    o.combat_power = 100.0f;
    o.max_attack_range = 50.0f;
    return o;
}

static void nudge(PerceptionObject& o) {  // a transform change of one unit in x
    o.position.x += 1.0f;
    o.transform[3] = o.position.x;
}

// ---------------------------------------------------------------- 14 data-pack stamps, first check
static void test_unit_ai_stamps() {
    // two range_int draws in order: +0x20 from Get_Int(0, 300), then +0x34 from Get_Int(0, 150)
    SimRng a(12345u), b(12345u);
    for (uint32_t f : {0u, 1u, 299u, 1000u, 54321u}) {
        const UnitAiStamps s = draw_unit_ai_stamps(a, f);
        const int32_t r1 = b.range_int(0, 300), r2 = b.range_int(0, 150);
        const auto fi = static_cast<int32_t>(f);
        CHECK(s.reprojection_check == fi + r1 - 300 && s.second == fi + r2 - 150);
        CHECK(a == b);  // same state and draw count: exactly the two draws
        CHECK(s.reprojection_check <= fi && s.reprojection_check >= fi - 300);
        CHECK(s.second <= fi && s.second >= fi - 150);
    }
    // the stamps cover the whole [F - spread, F] range over many objects
    SimRng r(7u);
    bool lo = false, hi = false;
    for (int i = 0; i < 20000; ++i) {
        const UnitAiStamps s = draw_unit_ai_stamps(r, 500);
        lo = lo || s.reprojection_check == 200;
        hi = hi || s.reprojection_check == 500;
    }
    CHECK(lo && hi);

    PerceptionWorld w = make_world();
    w.objects.push_back(make_unit(1, 0, 300.0f, 300.0f));
    w.objects.push_back(make_unit(2, 0, 600.0f, 600.0f));
    w.objects.push_back(make_unit(3, 0, 900.0f, 900.0f));
    w.objects.push_back(make_unit(4, 0, 900.0f, 300.0f));
    w.index();
    TacticalPerceptionGrid g;
    g.init(TacticalMode::Land, w.map, 2, land_cfg());

    // stamp F - 250 at F = 1000: the first service (F = 1000) runs the check, then 1050, 1350, ...
    CHECK(g.register_object(w, 1, 1000, 750));
    nudge(w.objects[0]);
    CHECK(g.service_object(w, 1, 1000));   // first service: check, moved -> re-projected; stamp 1050
    nudge(w.objects[0]);
    CHECK(!g.service_object(w, 1, 1049));  // gate closed
    CHECK(g.service_object(w, 1, 1050));   // stamp 1050 -> 1350
    nudge(w.objects[0]);
    CHECK(!g.service_object(w, 1, 1349));
    CHECK(g.service_object(w, 1, 1350));

    // an unmoved object: the check runs but nothing is re-projected; the stamp still advances
    CHECK(g.register_object(w, 2, 1000, 1000));
    CHECK(!g.service_object(w, 2, 1000));  // check at the first service, same transform; stamp 1300
    nudge(w.objects[1]);
    CHECK(!g.service_object(w, 2, 1299));
    CHECK(g.service_object(w, 2, 1300));

    // catch-up: a stamp far behind advances one step (300) per service, not re-based on F
    CHECK(g.register_object(w, 3, 1000, 400));
    nudge(w.objects[2]);
    CHECK(g.service_object(w, 3, 1000));   // 400 -> 700
    nudge(w.objects[2]);
    CHECK(g.service_object(w, 3, 1001));   // 700 <= 1001 -> 1000
    nudge(w.objects[2]);
    CHECK(g.service_object(w, 3, 1002));   // 1000 <= 1002 -> 1300
    nudge(w.objects[2]);
    CHECK(!g.service_object(w, 3, 1003));  // 1300 > 1003
    CHECK(g.service_object(w, 3, 1300));

    // a stamp before frame 0 (object created at F < 300)
    TacticalPerceptionGrid z;
    z.init(TacticalMode::Land, w.map, 2, land_cfg());
    CHECK(z.register_object(w, 4, 0, -300));
    nudge(w.objects[3]);
    CHECK(z.service_object(w, 4, 0));   // -300 -> 0
    nudge(w.objects[3]);
    CHECK(z.service_object(w, 4, 1));   // 0 -> 300
    nudge(w.objects[3]);
    CHECK(!z.service_object(w, 4, 2));
    CHECK(z.service_object(w, 4, 300));

    // [port] legacy 3-argument form: stamp = registration + 300
    TacticalPerceptionGrid l;
    l.init(TacticalMode::Land, w.map, 2, land_cfg());
    CHECK(l.register_object(w, 1, 10));
    nudge(w.objects[0]);
    CHECK(!l.service_object(w, 1, 10) && !l.service_object(w, 1, 309));
    CHECK(l.service_object(w, 1, 310));
}

// ---------------------------------------------------------------- 15 destroyed objects [port]
static void test_destroy_memory() {
    PerceptionWorld w = make_world();
    PerceptionObject o = make_unit(1, 1, 300.0f, 300.0f);  // human-owned, hostile to player 0
    o.team_or_hide_when_fogged = true;
    w.objects.push_back(o);
    PerceptionObject e = make_unit(2, 0, 600.0f, 600.0f);  // AI-owned, hostile to player 1
    e.team_or_hide_when_fogged = true;
    w.objects.push_back(e);
    w.index();
    w.fow_active = true;
    // object 2 is fogged for the human player 1; AI player 0 never sees fog (AIUsesFogOfWarLand False)
    w.object_fogged = [](int32_t p, int32_t id) { return p == 1 && id == 2; };
    TacticalPerceptionGrid g;
    g.init(TacticalMode::Land, w.map, 2, land_cfg());
    CHECK(g.register_object(w, 1, 0, 0) && g.register_object(w, 2, 0, 0));
    const auto c1 = static_cast<uint32_t>(g.cell_clamped(300.0f, 300.0f));
    const auto c2 = static_cast<uint32_t>(g.cell_clamped(600.0f, 600.0f));
    // both players remember both cells
    for (int32_t p = 0; p < 2; ++p) {
        g.refog(p, c1, 5);
        g.refog(p, c2, 5);
    }
    g.service_all(w, 1);
    CHECK(g.fogged_for(2, 1) && !g.fogged_for(2, 0) && !g.fogged_for(1, 0) && !g.fogged_for(1, 1));
    CHECK(g.memory_entries(0, c2).size() == 1 && g.memory_entries(1, c2).size() == 1);
    CHECK(g.memory_entries(0, c1).size() == 1 && g.memory_entries(1, c1).size() == 1);
    // object 2 (fogged for player 1 only): the original keeps player 0's stale memory entry; the port removes both
    CHECK(g.on_object_destroyed(2));
    CHECK(g.memory_entries(0, c2).empty() && g.memory_entries(1, c2).empty() && g.live_entries(c2).empty());
    // object 1 (fogged for nobody): the original keeps every memory entry; the port removes them all
    CHECK(g.on_object_destroyed(1));
    CHECK(g.memory_entries(0, c1).empty() && g.memory_entries(1, c1).empty() && g.entry_count() == 0);
    // the memory lists hold no stale slot: a new object reusing the slot is not remembered
    w.objects.push_back(make_unit(3, 1, 900.0f, 900.0f));
    w.index();
    CHECK(g.register_object(w, 3, 2, 2));
    CHECK(g.memory_entries(0, c1).empty() && g.memory_entries(0, c2).empty());
}

// ---------------------------------------------------------------- 6 free-store admission
static void test_admission() {
    // land: a TEAM with a locomotor owned by the AI
    FreeStoreObjectInfo land;
    land.owned_by_player = true;
    land.has_locomotor = true;
    land.is_team = true;
    CHECK(land_free_store_admits(land));
    auto land_refuses = [&](auto mutate) {
        FreeStoreObjectInfo x = land;
        mutate(x);
        return !land_free_store_admits(x);
    };
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.type_create_team = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.is_transport = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.type_category_bit_5f = true; }));  // DUMMY_GROUND_STRUCTURE
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.type_is_dummy = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.has_locomotor = false; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.has_indigenous_or_explosion = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.team_has_bit_20 = true; }));  // INDIGENOUS member type
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.type_bit_5b = true; }));      // DUMMY_GROUND_BASE
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.owned_by_player = false; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.type_stay_in_transport = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.subobject_f8_nonempty = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.in_mode_manager_set = true; }));  // bombing-run manager
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.dead_or_removing = true; }));
    CHECK(land_refuses([](FreeStoreObjectInfo& x) { x.in_world = false; }));
    {
        FreeStoreObjectInfo x = land;
        x.is_team = false;
        x.team_has_bit_20 = true;  // the member test applies to TEAMs only
        x.is_starship_combatant = true;
        CHECK(land_free_store_admits(x));
        // a dummy with LOBBING_SUPERWEAPON qualifies (Is_Dummy belongs to the second branch), also without a
        // locomotor and with DUMMY_GROUND_STRUCTURE
        x = land;
        x.is_team = false;
        x.has_locomotor = false;
        x.type_is_dummy = true;
        x.type_category_bit_5f = true;
        x.has_lobbing_superweapon = true;
        CHECK(land_free_store_admits(x));
        // the space-only facts do not affect land
        x = land;
        x.type_squadron_member = x.type_company_member = x.type_dummy_orbital_structure = true;
        x.type_dummy_star_base = x.has_tactical_super_weapon = true;
        CHECK(land_free_store_admits(x));
        // SPECIAL_WEAPON is not a land qualifier
        x = land;
        x.is_team = false;
        x.has_special_weapon = true;
        CHECK(!land_free_store_admits(x));
    }

    // space: a STARSHIP_COMBATANT with a locomotor owned by the AI
    FreeStoreObjectInfo space;
    space.owned_by_player = true;
    space.has_locomotor = true;
    space.is_starship_combatant = true;
    CHECK(space_free_store_admits(space));
    auto space_refuses = [&](auto mutate) {
        FreeStoreObjectInfo x = space;
        mutate(x);
        return !space_free_store_admits(x);
    };
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.type_squadron_member = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.type_company_member = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.type_is_dummy = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.has_locomotor = false; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.is_starship_combatant = false; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.type_dummy_orbital_structure = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.type_dummy_star_base = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.has_tactical_super_weapon = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.owned_by_player = false; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.subobject_f8_nonempty = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.dead_or_removing = true; }));
    CHECK(space_refuses([](FreeStoreObjectInfo& x) { x.in_world = false; }));
    {
        // none of the land-only tests apply in space
        FreeStoreObjectInfo x = space;
        x.is_transport = true;
        x.type_create_team = true;
        x.type_stay_in_transport = true;
        x.has_indigenous_or_explosion = true;
        x.type_bit_5b = true;
        x.type_category_bit_5f = true;
        x.in_mode_manager_set = true;
        CHECK(space_free_store_admits(x));
        x.is_starship_combatant = false;
        x.is_team = true;
        x.team_has_bit_20 = true;
        CHECK(space_free_store_admits(x));  // TEAM with a locomotor qualifies too
        // SPECIAL_WEAPON qualifies alone: dummy, no locomotor, not a combatant
        x = space;
        x.is_starship_combatant = false;
        x.has_locomotor = false;
        x.type_is_dummy = true;
        x.has_special_weapon = true;
        CHECK(space_free_store_admits(x));
        x.has_tactical_super_weapon = true;  // ... but the later tests still refuse
        CHECK(!space_free_store_admits(x));
        // LOBBING_SUPERWEAPON is not a space qualifier
        x = space;
        x.is_starship_combatant = false;
        x.has_lobbing_superweapon = true;
        CHECK(!space_free_store_admits(x));
    }
}

// ---------------------------------------------------------------- 9 service timing
static void test_service_timing() {
    TacticalFreeStore fs;
    CHECK(fs.admit(5, 1));
    FreeStoreServiceStep step;
    // every frame: the first eligible frames
    {
        FreeStoreSchedule s = busy_tactical_schedule();
        std::vector<uint32_t> svc, unit;
        for (uint32_t f = 0; f <= 1260; ++f) {
            free_store_service(s, fs, f, step);
            if (step.run_service) svc.push_back(f);
            if (step.run_unit_service && unit.size() < 2) unit.push_back(f);
        }
        CHECK(svc == (std::vector<uint32_t>{600, 1200}));  // now(600) = 20.0000019 > 20; not 599
        CHECK(unit.size() == 2 && unit[0] == 61);           // now(60) is exactly 2.0: not passed
        CHECK(unit[1] == 121);                               // now(121) - now(61) = 2.0000002 > 2 (float clock)
        std::printf("free store (every frame): FreeStoreService at F = %u, %u; On_Unit_Service from F = %u, %u\n", svc[0],
                    svc[1], unit[0], unit[1]);
    }
    CHECK(free_store_mode_time(600) == 20.0000019f && free_store_mode_time(1200) == 40.0000038f);
    CHECK(free_store_mode_time(60) == 2.0f && free_store_mode_time(599) < 20.0f);
    CHECK(free_store_mode_time(1) == 0.033333335f);
    // Execution services every 3 frames from F = 0: the next service at or after those frames
    {
        FreeStoreSchedule s = busy_tactical_schedule();
        std::vector<uint32_t> svc;
        uint32_t unit0 = 0;
        for (uint32_t f = 0; f <= 1806; f += 3) {
            free_store_service(s, fs, f, step);
            if (step.run_service) svc.push_back(f);
            if (step.run_unit_service && unit0 == 0) unit0 = f;
        }
        // now(1800) - now(1200) is exactly 20.0, which does not pass the strict test: the third service is at 1803
        CHECK(svc == (std::vector<uint32_t>{600, 1200, 1803}) && unit0 == 63);
    }
    // another cadence phase (Execution on F = 1 mod 3): 601 and 61
    {
        FreeStoreSchedule s = busy_tactical_schedule();
        uint32_t svc0 = 0, unit0 = 0;
        for (uint32_t f = 1; f <= 700; f += 3) {
            free_store_service(s, fs, f, step);
            if (step.run_service && svc0 == 0) svc0 = f;
            if (step.run_unit_service && unit0 == 0) unit0 = f;
        }
        CHECK(svc0 == 601 && unit0 == 61);
    }
}

int main() {
    test_fow_rounding();
    test_unit_ai_stamps();
    test_destroy_memory();
    test_admission();
    test_service_timing();
    if (g_fail) {
        std::printf("alamo_ai_tactical_readings_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ai_tactical_readings_tests: OK\n");
    return 0;
}
